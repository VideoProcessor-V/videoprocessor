// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// L-CORE render-ahead lifetime (lead 10, coordinator ruling on the owner's render-ahead question, 2026-11-14 08:3x EST).
// With render-ahead ON at a depth D > 1 (D = 2, 3, 5), frame n is rendered (sealed, its memo spots made) D frames BEFORE it is
// presented, so a present-time reader of frame n runs while frame n + D renders:
//   (a) frames are rendered ahead of present (the simulated timeline: render t, present t - D);
//   (b) every memo spot read for frame n while n + D renders is the SAME spot (same pointer, same bytes, the owner fill not run
//       again, no epoch change: the seal numbering only moves forward);
//   (c) after its frame is released a read is REFUSED (FrameReleased: counted, on the status lines and the Fallbacks entry), never
//       stale; a reader's pin keeps the bytes it was given; P13 plane views (render-frame scoped by contract) are refused, never the
//       next frame's texture;
//   (d) the keep window covers the render-ahead depth: the skeleton module derives the renderer memo's keep window from the host's
//       measured render-ahead depth (Lifecycle::QueueDepth), with or without the analysis lane (rule 6: general, not fixed).
// CPU only (no GPU object: textures are plain structs), single process, every test bounded.

#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SkeletonRenderAheadLifetimeTestsNs
{
	using namespace VpSkeleton;
	namespace RE = RendererExtension;

	const char* const kGrainUnit = "repair/grain_split";   // P10's owner (ProductRows)
	const uint32_t kDepths[] = { 2u, 3u, 5u };

	bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

	// An owner CPU fill whose bytes name the frame (so a stale read would show another frame's bytes).
	OwnerFill FrameFill(std::atomic<int>* calls)
	{
		OwnerFill f;
		f.fill = [calls](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
			calls->fetch_add(1);
			out.bytes.assign(64, 0);
			for (size_t i = 0; i < out.bytes.size(); ++i)
				out.bytes[i] = uint8_t(r.seq * 13u + i);
			return true;
		};
		return f;
	}

	bool BytesOfFrame(const AcquireResult& r, uint64_t seq)
	{
		if (r.cpu == nullptr || r.cpuBytes != 64u)
			return false;
		for (size_t i = 0; i < 64u; ++i)
			if (r.cpu[i] != uint8_t(seq * 13u + i))
				return false;
		return true;
	}

	RE::FrameContext Begin(const void* host, int64_t seq)
	{
		RE::FrameContext c;
		c.host = host;
		c.phase = RE::FramePhase::Begin;
		c.seq = seq;
		return c;
	}

	void QueueDepth(Vpv::Skeleton::SkeletonModule& sk, const void* host, double frames)
	{
		RE::LifecycleFacts f;
		f.host = host;
		f.kind = RE::Lifecycle::QueueDepth;
		f.value = frames;
		sk.OnLifecycle(f);
	}

	// The render-ahead timeline on ONE memo: at render t the frame t is sealed and its spot made; at the same time the present of
	// frame t - D reads its spot. `keep` = the memo's keep window. Returns false (with `why`) at the first violated expectation.
	bool RunTimeline(SkeletonMemo& m, uint32_t depth, uint32_t keep, std::atomic<int>& calls, std::wstring& why)
	{
		std::map<uint64_t, AcquireResult> rendered;   // what each frame's render read (the pins the render holds)
		const uint64_t first = 10, frames = 40;
		for (uint64_t t = first; t < first + frames; ++t) {
			m.SetKeepFrames(keep);
			m.OnSeal(t);   // (a) render of frame t begins: seal
			const AcquireResult r = m.Acquire(kP10GrainSplit, t, AcquireForm::Cpu);
			if (r.status != AcquireStatus::FilledAtRead || !BytesOfFrame(r, t)) {
				why = L"render: the frame's spot is made once by the owner";
				return false;
			}
			rendered[t] = r;
			if (t < first + depth)
				continue;
			const uint64_t presented = t - depth;   // (a) the present lags the render by D frames
			const int before = calls.load();
			const AcquireResult p = m.Acquire(kP10GrainSplit, presented, AcquireForm::Cpu);
			if (p.status != AcquireStatus::Reused || p.source != AcquireSource::Memo) {
				why = L"(b) the present of frame n while n + D renders must REUSE n's spot";
				return false;
			}
			if (p.cpu != rendered[presented].cpu || !BytesOfFrame(p, presented) || calls.load() != before) {
				why = L"(b) same pointer and bytes, the owner fill not run again";
				return false;
			}
			// (c) a frame older than the keep window is refused, visibly, never recomputed or stale.
			const uint64_t gone = t - 1u - keep;
			if (t >= first + keep + 1u && gone >= first) {
				const uint64_t refusedBefore = m.CountersOf(kP10GrainSplit).unavailable[size_t(AcquireCause::FrameReleased)];
				const AcquireResult g = m.Acquire(kP10GrainSplit, gone, AcquireForm::Cpu);
				if (g.Ok() || g.cause != AcquireCause::FrameReleased || g.cpu != nullptr) {
					why = L"(c) a released frame is refused (FrameReleased), never served";
					return false;
				}
				if (m.CountersOf(kP10GrainSplit).unavailable[size_t(AcquireCause::FrameReleased)] != refusedBefore + 1u
					|| !Has(m.LastCause(kP10GrainSplit), "frame released")) {
					why = L"(c) the refusal is counted and shown";
					return false;
				}
				if (!BytesOfFrame(rendered[gone], gone)) {
					why = L"(c) a reader's pin keeps the bytes it was given";
					return false;
				}
				rendered.erase(gone);
			}
		}
		if (calls.load() != int(frames)) {
			why = L"computed exactly once per frame";
			return false;
		}
		return true;
	}

	TEST_CLASS(SkeletonRenderAheadLifetimeTests)
	{
	public:
		// (a)(b)(c) on the memo with a keep window of D: every spot read by the present of n while n + D renders is the same spot;
		// the frame one older than the window is refused visibly. The seal numbering only moves forward (no epoch change).
		TEST_METHOD(MemoSpotsLiveFromRenderToPresentAtDepth235)
		{
			for (uint32_t d : kDepths) {
				SkeletonMemo m;
				std::atomic<int> calls{ 0 };
				m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls));
				std::wstring why;
				const bool ok = RunTimeline(m, d, d, calls, why);
				Assert::IsTrue(ok, (L"depth " + std::to_wstring(d) + L": " + why).c_str());
				Assert::IsTrue(m.KeepFrames() == d && m.CountersOf(kP10GrainSplit).released > 0, L"spots released only past the window");
			}
		}

		// The same timeline with the per-frame memo of today's default (keep 0): the present-time read of frame n while n + D renders
		// is refused. Records what a keep window SHORTER than the depth does (visible refusal, never stale), for every depth.
		TEST_METHOD(KeepWindowShorterThanTheDepthIsRefusedVisiblyNeverStale)
		{
			for (uint32_t d : kDepths) {
				SkeletonMemo m;
				std::atomic<int> calls{ 0 };
				m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls));
				AcquireResult held;
				for (uint64_t t = 1; t <= 1u + d; ++t) {
					m.OnSeal(t);
					const AcquireResult r = m.Acquire(kP10GrainSplit, t, AcquireForm::Cpu);
					if (t == 1)
						held = r;
				}
				const AcquireResult p = m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu);   // present of frame 1 while 1 + D renders
				Assert::IsTrue(!p.Ok() && p.cause == AcquireCause::FrameReleased && p.cpu == nullptr, L"refused, not stale");
				Assert::IsTrue(BytesOfFrame(held, 1), L"the render's pin still holds frame 1's own bytes");
				bool shown = false;
				for (const FallbackCatalog::Decision& dec : m.Decisions())
					shown = shown || (dec.engaged && Has(dec.reason, "frame released"));
				Assert::IsTrue(shown, L"on the Fallbacks list");
			}
		}

		// (c) for P13 views: render-frame scoped by contract (the uploader's textures). While n + D renders, frame n's view is refused
		// visibly and never answered with another frame's texture; a pin taken during n's render keeps n's facts.
		TEST_METHOD(SourcePlaneViewsAreRefusedAfterTheirRenderNeverAnotherFramesTexture)
		{
			using Vpv::Capture::SourcePlanes;
			for (uint32_t d : kDepths) {
				int host = 0;
				SkeletonMemo m;
				m.SetHost(&host);
				m.SetKeepFrames(d);   // even with a keep window of D, a VIEW lives one render frame (frameScoped)
				m.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill());
				std::vector<pl_tex_t> tex(8);
				AcquireResult firstView;
				for (uint64_t t = 1; t <= 1u + d; ++t) {
					SourcePlanes::BeginFrame(&host);
					m.OnSeal(t);
					tex[t].params.w = int(100 + t);
					tex[t].params.h = 50;
					pl_frame image{};
					image.num_planes = 1;
					image.planes[0].texture = &tex[t];
					image.planes[0].components = 1;
					image.repr.sys = PL_COLOR_SYSTEM_BT_709;
					Assert::IsTrue(SourcePlanes::OfferFrame(&host, t, 0, image), L"Entrance offer");
					const AcquireResult v = m.Acquire(kP13SourceRing, t, AcquireForm::Gpu);
					Assert::IsTrue(v.Ok() && v.gpu.resource == &tex[t], L"the frame's own plane during its render");
					if (t == 1)
						firstView = v;
				}
				const AcquireResult late = m.Acquire(kP13SourceRing, 1, AcquireForm::Gpu);   // present of frame 1 while 1 + D renders
				Assert::IsTrue(!late.Ok() && late.gpu.resource == nullptr, L"refused after its render frame");
				Assert::IsTrue(late.cause == AcquireCause::OwnerDeclined || late.cause == AcquireCause::FrameReleased, L"a visible cause");
				const Vpv::Capture::SourcePlaneSet* facts = SourcePlanes::FactsOf(firstView);
				Assert::IsTrue(facts != nullptr && facts->seq == 1 && facts->tex[0] == &tex[1], L"the render's pin keeps frame 1's facts");
				SourcePlanes::Forget(&host);
			}
		}

		// (d) the keep window covers the render-ahead depth: the module derives the renderer memo's window from the host's measured
		// render-ahead depth (Lifecycle::QueueDepth, frames the plan keeps ahead of the present; a partial frame holds a slot), with the
		// analysis lane OFF (the default configuration), and the present-time reads of the timeline are served.
		TEST_METHOD(ModuleKeepWindowFollowsTheMeasuredRenderAheadDepth)
		{
			for (uint32_t d : kDepths) {
				Vpv::Skeleton::SkeletonModule sk([](ConfigFile&) { return false; });   // analysis_lane off (defaults)
				int host = 0;
				std::atomic<int> calls{ 0 };
				Assert::IsTrue(sk.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls))
					== SkeletonMemo::Registration::Registered, L"owner");
				const Vpv::Skeleton::FrameMemo fm = sk.MemoFor(&host, 1);
				Assert::IsTrue(fm.Valid(), L"memo");
				QueueDepth(sk, &host, double(d) - 0.25);   // measured: a partial frame rounds up
				std::map<uint64_t, const uint8_t*> made;
				for (uint64_t t = 1; t <= 20; ++t) {
					RE::FrameContext b = Begin(&host, int64_t(t));
					sk.OnFrame(b);
					Assert::IsTrue(fm.memo->KeepFrames() >= d, (L"(d) keep window >= render-ahead depth " + std::to_wstring(d)).c_str());
					made[t] = fm.memo->Acquire(kP10GrainSplit, t, AcquireForm::Cpu).cpu;
					if (t > d) {
						const AcquireResult p = fm.memo->Acquire(kP10GrainSplit, t - d, AcquireForm::Cpu);
						Assert::IsTrue(p.status == AcquireStatus::Reused && p.cpu == made[t - d] && BytesOfFrame(p, t - d),
							L"(b) the present of n while n + D renders reuses n's spot");
					}
				}
				Assert::AreEqual(20, calls.load(), L"computed once per frame");
				// Render-ahead switched off (the host has no depth: negative): the window falls back to the per-frame memo.
				QueueDepth(sk, &host, -1.0);
				RE::FrameContext b21 = Begin(&host, 21);
				sk.OnFrame(b21);
				Assert::IsTrue(fm.memo->KeepFrames() == 0, L"render-ahead off: the per-frame memo");
				RE::LifecycleFacts rel;
				rel.host = &host;
				rel.kind = RE::Lifecycle::GpuReleased;
				sk.OnLifecycle(rel);
			}
		}
	};
}
