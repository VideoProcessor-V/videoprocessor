// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// Slice A (lead10/design-queue-keyed-products.md s2.3-s2.5, s2.7, s4 MEMO-T1..T10): skeleton products attached to their frame in
// the queue (owner 2026-11-14 08:33 EST), kept until the frame LEFT the queue, admitted up to current + ahead, planned ahead
// depths, drops and discontinuities. Every window is DERIVED (DeriveWindow) from the terms a host would measure; no fixed depth.
// CPU only, every test bounded.

#include <VP-V/core/MemoGpuGrant.h>
#include <VP-V/core/QueueWindow.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace QueueKeyedMemoTestsNs
{
	using namespace VpSkeleton;

	const char* const kGrainUnit = "repair/grain_split";              // P10's owner
	const char* const kRegionUnit = "input/source_analysis/regions";  // P11

	bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

	QueueWindowTerms Terms(uint16_t R, uint16_t P, uint16_t H, uint16_t L, uint16_t W)
	{
		QueueWindowTerms t;
		t.renderAhead = R;
		t.presentLag = P;
		t.history = H;
		t.laneLead = L;
		t.lookAheadWalk = W;
		// One active ahead consumer asking for the whole queue ahead (amendment 3: capped by kAheadCeiling and the queue).
		t.aheadNeed = uint16_t(L + W);
		t.aheadNeedBy = "test consumer";
		t.wired = QueueWindowTerms::kAll;
		return t;
	}

	OwnerFill FrameFill(std::atomic<int>* calls, uint32_t axes = 0)
	{
		OwnerFill f;
		f.variantAxes = axes;
		f.fill = [calls](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
			calls->fetch_add(1);
			out.bytes.assign(32, 0);
			for (size_t i = 0; i < out.bytes.size(); ++i)
				out.bytes[i] = uint8_t(r.seq * 7u + r.variant.sigmaMilli + i);
			return true;
		};
		return f;
	}

	bool OfFrame(const AcquireResult& r, uint64_t seq, uint32_t sigma = 0)
	{
		if (r.cpu == nullptr || r.cpuBytes != 32u)
			return false;
		for (size_t i = 0; i < 32u; ++i)
			if (r.cpu[i] != uint8_t(seq * 7u + sigma + i))
				return false;
		return true;
	}

	// A render step on the memo the way the skeleton drives it (slice B): derive the window from the measured terms, set it, seal.
	void Render(SkeletonMemo& m, uint64_t t, const QueueWindowTerms& terms, uint64_t epoch = 0)
	{
		m.SetWindow(DeriveWindow(terms, t, epoch));
		m.OnSeal(t);
	}

	TEST_CLASS(QueueKeyedMemoTests)
	{
	public:
		// MEMO-T1: D = 2, 3, 5 and L = 0, 3, 6. Frame n's spot is the same pointer from its first ahead fill, through its render, to its
		// present at n + D; computed once per frame.
		TEST_METHOD(RenderAheadDepth235PlusLookAhead)
		{
			for (uint16_t D : { uint16_t(2), uint16_t(3), uint16_t(5) })
				for (uint16_t L : { uint16_t(0), uint16_t(3), uint16_t(6) }) {
					SkeletonMemo m;
					std::atomic<int> calls{ 0 };
					m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls));
					std::map<uint64_t, const uint8_t*> first;
					const uint64_t t0 = 20, frames = 40;
					for (uint64_t t = t0; t < t0 + frames; ++t) {
						Render(m, t, Terms(D, D, 0, L, 0));
						if (L > 0) {   // a reader earlier in the queue asks for frame t + L
							const AcquireResult a = m.Acquire(kP10GrainSplit, t + L, AcquireForm::Cpu);
							Assert::IsTrue(a.Ok() && OfFrame(a, t + L), L"ahead fill admitted (inside the window)");
							if (!first.count(t + L))
								first[t + L] = a.cpu;
						}
						const AcquireResult r = m.Acquire(kP10GrainSplit, t, AcquireForm::Cpu);   // the render of t
						Assert::IsTrue(r.Ok() && OfFrame(r, t), L"render read");
						if (!first.count(t))
							first[t] = r.cpu;
						Assert::IsTrue(r.cpu == first[t], L"the render reads the spot made ahead");
						if (t >= t0 + D) {
							const AcquireResult p = m.Acquire(kP10GrainSplit, t - D, AcquireForm::Cpu);   // the present of t - D
							Assert::IsTrue(p.status == AcquireStatus::Reused && p.cpu == first[t - D] && OfFrame(p, t - D),
								(L"present of n while n + D renders: same spot (D " + std::to_wstring(D) + L", L " + std::to_wstring(L) + L")").c_str());
						}
					}
					// t0 .. t0 + frames - 1 rendered, plus L frames ahead filled: once each.
					Assert::AreEqual(int(frames + L), calls.load(), L"computed once per frame");
					const AcquireCounters c = m.CountersOf(kP10GrainSplit);
					Assert::IsTrue(L == 0 || (c.aheadFilled > 0 && c.aheadReused > 0), L"ahead fills counted and reused at render");
					Assert::IsTrue(c.unavailable[size_t(AcquireCause::FrameReleased)] == 0, L"0 FrameReleased at present time");
				}
		}

		// MEMO-T2: an ahead demand while N renders, reused at N + 3's render with the same bytes; two variants, once each.
		TEST_METHOD(AheadDemandThenLaterReuse)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls, VariantKey::kAxisSigma));
			const QueueWindowTerms t = Terms(1, 1, 0, 4, 0);
			Render(m, 10, t);
			VariantKey v1, v2;
			v1.sigmaMilli = 3000;
			v2.sigmaMilli = 6000;
			const AcquireResult a1 = m.Acquire(kP10GrainSplit, 13, AcquireForm::Cpu, v1);
			const AcquireResult a2 = m.Acquire(kP10GrainSplit, 13, AcquireForm::Cpu, v2);
			Assert::IsTrue(a1.status == AcquireStatus::FilledAtRead && a2.status == AcquireStatus::FilledAtRead, L"filled ahead");
			for (uint64_t s = 11; s <= 13; ++s)
				Render(m, s, t);
			const AcquireResult r1 = m.Acquire(kP10GrainSplit, 13, AcquireForm::Cpu, v1);
			const AcquireResult r2 = m.Acquire(kP10GrainSplit, 13, AcquireForm::Cpu, v2);
			Assert::IsTrue(r1.status == AcquireStatus::Reused && r1.cpu == a1.cpu && OfFrame(r1, 13, 3000), L"variant 1 reused");
			Assert::IsTrue(r2.status == AcquireStatus::Reused && r2.cpu == a2.cpu && OfFrame(r2, 13, 6000), L"variant 2 reused");
			Assert::AreEqual(2, calls.load(), L"ownerFillCalls == 1 per (id, seq, variant)");
			Assert::IsTrue(m.CountersOf(kP10GrainSplit).aheadFilled == 2 && m.CountersOf(kP10GrainSplit).aheadReused == 2, L"counted");
		}

		// MEMO-T3: a B fill for N + 3 Acquires A(N + 3) and A(N + 2) (lag 1): both published, reused by a second reader, once each.
		TEST_METHOD(RecursiveAhead)
		{
			SkeletonMemo m;
			std::atomic<int> aCalls{ 0 }, bCalls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&aCalls));
			OwnerFill fb;
			fb.fill = [&bCalls](const OwnerFillRequest& r, SkeletonMemo& mm, OwnerFillOutput& out) {
				bCalls.fetch_add(1);
				const AcquireResult a = mm.Acquire(kP10GrainSplit, r.seq, AcquireForm::Cpu);
				const AcquireResult lag = mm.Acquire(kP10GrainSplit, r.seq - 1, AcquireForm::Cpu);
				if (!a.Ok() || !lag.Ok())
					return false;
				out.bytes.assign(a.cpu, a.cpu + a.cpuBytes);
				out.bytes[0] = uint8_t(out.bytes[0] ^ lag.cpu[0]);
				return true;
			};
			m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit, fb);
			Render(m, 10, Terms(0, 0, 1, 3, 0));
			Assert::IsTrue(m.Acquire(kP11RegionBlockStats, 13, AcquireForm::Cpu).status == AcquireStatus::FilledAtRead, L"B(13) ahead");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 13, AcquireForm::Cpu).status == AcquireStatus::Reused, L"A(13) published on the way");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 12, AcquireForm::Cpu).status == AcquireStatus::Reused, L"A(12) published on the way");
			Assert::IsTrue(aCalls.load() == 2 && bCalls.load() == 1, L"A once per frame, B once");
		}

		// MEMO-T4: no spot of f is released while f >= presented - H; a pin outlives the release; presented facts not fed -> the
		// render-seq edge, and the status line says so.
		TEST_METHOD(EvictionOnlyAfterLeavingTheQueue)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls));
			const uint16_t H = 1;
			QueueWindowTerms t = Terms(4, 4, H, 0, 0);
			Render(m, 10, t);
			const AcquireResult pin = m.Acquire(kP10GrainSplit, 10, AcquireForm::Cpu);
			for (uint64_t s = 11; s <= 15; ++s) {   // presented = s - 4: frame 10 is still queued while presented - H <= 10
				Render(m, s, t);
				Assert::IsTrue(m.Acquire(kP10GrainSplit, 10, AcquireForm::Cpu).status == AcquireStatus::Reused, L"still in the queue: kept");
			}
			Render(m, 16, t);   // presented 12, edge 12 - 1 = 11 > 10: frame 10 left the queue
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 10, AcquireForm::Cpu).cause == AcquireCause::FrameReleased, L"released after leaving");
			Assert::IsTrue(OfFrame(pin, 10), L"the pin outlives the release");
			Assert::AreEqual(7, calls.load(), L"never recomputed");
			// Present facts not fed: the render-seq edge (seal - back) and the line says so.
			QueueWindowTerms np = t;
			np.wired = uint8_t(QueueWindowTerms::kAll & ~QueueWindowTerms::kPresentLag);
			Render(m, 17, np);
			bool said = false;
			for (const std::string& l : m.StatusLines())
				said = said || Has(l, "present facts not fed");
			Assert::IsTrue(said, L"shown");
		}

		// MEMO-T5: beyond current + ahead is NotQueued: never a fill, on the Fallbacks list, released once served.
		TEST_METHOD(NotQueuedBeyondTheWindow)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls));
			Render(m, 10, Terms(0, 0, 0, 1, 2));   // ahead = L + W = 1 + 2 = 3
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 13, AcquireForm::Cpu).Ok(), L"current + ahead admitted");
			const AcquireResult far = m.Acquire(kP10GrainSplit, 14, AcquireForm::Cpu);
			Assert::IsTrue(far.cause == AcquireCause::NotQueued && calls.load() == 1, L"refused, the fill not called");
			bool engaged = false;
			for (const FallbackCatalog::Decision& d : m.Decisions())
				engaged = engaged || (d.engaged && Has(d.reason, "not in the queue"));
			Assert::IsTrue(engaged, L"visible");
			Render(m, 11, Terms(0, 0, 0, 1, 2));
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 14, AcquireForm::Cpu).Ok() && m.Decisions().empty(), L"in the window now: served, released");
			Assert::IsTrue(m.CountersOf(kP10GrainSplit).unavailable[size_t(AcquireCause::NotQueued)] == 1, L"counted");
		}

		// MEMO-T6: every discontinuity cause releases every spot of every frame, the epoch moves, the next Acquire refills once and
		// is counted as a recompute after invalidation (owner Q1 a).
		TEST_METHOD(FlushInvalidatesAhead)
		{
			const SkeletonMemo::InvalidateCause causes[] = { SkeletonMemo::InvalidateCause::Flush, SkeletonMemo::InvalidateCause::Reset,
				SkeletonMemo::InvalidateCause::LiveQueueReset, SkeletonMemo::InvalidateCause::Discontinuity,
				SkeletonMemo::InvalidateCause::Gap, SkeletonMemo::InvalidateCause::FormatChange };
			for (SkeletonMemo::InvalidateCause cause : causes) {
				SkeletonMemo m;
				std::atomic<int> calls{ 0 };
				m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls));
				Render(m, 10, Terms(2, 2, 0, 3, 0));
				for (uint64_t s = 8; s <= 13; ++s)
					m.Acquire(kP10GrainSplit, s, AcquireForm::Cpu);   // the presenter queue, the render frame and the ahead frames
				const size_t spots = m.MemoCount();
				Assert::IsTrue(spots == 6, L"six frames attached");
				const uint64_t epoch = m.Epoch();
				m.Invalidate(cause);
				Assert::IsTrue(m.Epoch() == epoch + 1 && m.Invalidations(cause) == 1 && m.Discontinuities() == 1, L"epoch + 1 at once");
				Render(m, 14, Terms(2, 2, 0, 3, 0));
				Assert::IsTrue(m.MemoCount() == 0, L"every spot of every frame released at the next seal");
				const AcquireResult again = m.Acquire(kP10GrainSplit, 13, AcquireForm::Cpu);
				Assert::IsTrue(again.status == AcquireStatus::FilledAtRead && OfFrame(again, 13), L"refilled");
				const AcquireCounters c = m.CountersOf(kP10GrainSplit);
				Assert::IsTrue(c.invalidatedRecompute == 1 && c.Computed() == 6, L"the recompute is counted apart, not as a duplicate");
				bool shown = false;
				for (const std::string& l : m.StatusLines())
					shown = shown || Has(l, "invalidated (");
				Assert::IsTrue(shown, L"on the status lines");
			}
		}

		// MEMO-T7: a drop of frames [s - k, s) releases only those frames.
		TEST_METHOD(DropDiscardsOnlyDropped)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls));
			Render(m, 10, Terms(0, 0, 0, 6, 0));
			for (uint64_t s = 10; s <= 16; ++s)
				m.Acquire(kP10GrainSplit, s, AcquireForm::Cpu);
			const uint64_t survivor = 15, k = 3;   // frames 12, 13, 14 dropped
			for (uint64_t f = survivor - k; f < survivor; ++f)
				m.Discard(f);
			for (uint64_t s = 10; s <= 16; ++s) {
				const bool dropped = s >= survivor - k && s < survivor;
				const AcquireResult r = m.Acquire(kP10GrainSplit, s, AcquireForm::Cpu);
				Assert::IsTrue(dropped ? r.status == AcquireStatus::FilledAtRead : r.status == AcquireStatus::Reused, L"only the dropped frames");
			}
			Assert::IsTrue(m.CountersOf(kP10GrainSplit).discarded == k, L"counted");
		}

		// MEMO-T8: an AHEAD fill measured above its declaration that does not fit is a plan miss: refused OverBudget, counted; every spot
		// already attached stays (never evict); the frame is made at its own render.
		TEST_METHOD(PlanMissRefusedVisiblyNothingEvicted)
		{
			SkeletonMemo m;
			m.SetGpuDispatcherPresent(true);
			m.SetGpuBudget(true, 3000);
			std::atomic<int> calls{ 0 };
			std::atomic<uint64_t> produce{ 1000 };
			OwnerFill g;
			g.reserveBytes = 1000;   // the declaration
			g.fill = [&calls, &produce](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
				calls.fetch_add(1);
				static int tex = 0;
				out.gpu.resource = &tex;
				out.gpu.bytes = produce.load();
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, g);
			Render(m, 10, Terms(0, 0, 0, 4, 0));
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 10, AcquireForm::Gpu).Ok() && m.Acquire(kP10GrainSplit, 11, AcquireForm::Gpu).Ok(), L"two spots");
			produce = 1500;   // the plan was wrong: this frame measures above its declaration
			const AcquireResult miss = m.Acquire(kP10GrainSplit, 12, AcquireForm::Gpu);
			Assert::IsTrue(miss.cause == AcquireCause::OverBudget && m.CountersOf(kP10GrainSplit).planMiss == 1, L"a plan miss, counted");
			Assert::IsTrue(m.MemoCount() == 2 && m.GpuBytes() == 2000u, L"nothing attached was evicted");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 11, AcquireForm::Gpu).status == AcquireStatus::Reused, L"kept frames stay");
			produce = 1000;
			Render(m, 11, Terms(0, 0, 0, 4, 0));
			Render(m, 12, Terms(0, 0, 0, 4, 0));   // frame 10 and 11 left: room again
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 12, AcquireForm::Gpu).status == AcquireStatus::FilledAtRead, L"made at its own render");
		}

		// MEMO-T9: a P13 VIEW stays render-frame scoped; an owned texture (per-result Window scope) is kept with its frame.
		TEST_METHOD(ViewsStayFrameScopedOwnedTexturesFollowTheWindow)
		{
			int host = 0;
			SkeletonMemo m;
			m.SetHost(&host);
			m.SetGpuDispatcherPresent(true);
			m.SetGpuBudget(true, 1u << 20);
			m.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, Vpv::Capture::SourcePlanes::kUnit, Vpv::Capture::SourcePlanes::MakeOwnerFill());
			OwnerFill owned;
			owned.frameScoped = true;   // the fill's default is a view; this result says otherwise
			owned.reserveBytes = 64;
			owned.fill = [](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
				static int tex = 0;
				out.gpu.resource = &tex;
				out.gpu.bytes = 64;
				out.scope = OwnerFillOutput::Scope::Window;
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, owned);
			pl_tex_t luma{};
			luma.params.w = 8;
			luma.params.h = 8;
			pl_frame image{};
			image.num_planes = 1;
			image.planes[0].texture = &luma;
			image.planes[0].components = 1;
			image.repr.sys = PL_COLOR_SYSTEM_BT_709;
			Render(m, 10, Terms(2, 2, 0, 2, 0));
			Vpv::Capture::SourcePlanes::OfferFrame(&host, 10, 0, image);
			Assert::IsTrue(m.Acquire(kP13SourceRing, 10, AcquireForm::Gpu).Ok() && m.Acquire(kP10GrainSplit, 12, AcquireForm::Gpu).Ok(), L"both");
			Vpv::Capture::SourcePlanes::BeginFrame(&host);
			Render(m, 11, Terms(2, 2, 0, 2, 0));
			Assert::IsTrue(m.Acquire(kP13SourceRing, 10, AcquireForm::Gpu).cause == AcquireCause::OwnerDeclined, L"the view went with its render frame");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 12, AcquireForm::Gpu).status == AcquireStatus::Reused, L"the owned ahead texture stays");
			Vpv::Capture::SourcePlanes::Forget(&host);
		}

		// MEMO-T10: beyond its product's planned ahead depth = NotPlannedAhead, visible; within it, one fill; the plan releases nothing.
		TEST_METHOD(NotPlannedAhead)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls));
			Render(m, 10, Terms(0, 0, 0, 6, 0));
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 15, AcquireForm::Cpu).Ok(), L"before a plan: the window");
			AheadPlan plan;
			plan.depth.push_back(AheadPlan::Entry{ uint16_t(kP10GrainSplit), 0u, uint16_t(2) });
			plan.step = AheadStep::FewerFramesAhead;
			plan.line = "memo ahead plan: fewer frames ahead";
			m.SetAheadPlan(plan);
			Assert::IsTrue(m.MemoCount() == 1, L"a plan releases nothing");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 15, AcquireForm::Cpu).status == AcquireStatus::Reused, L"an attached spot is still served");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 13, AcquireForm::Cpu).cause == AcquireCause::NotPlannedAhead, L"beyond the plan: refused");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 12, AcquireForm::Cpu).status == AcquireStatus::FilledAtRead, L"within the plan: one fill");
			Assert::IsTrue(Has(m.LastCause(kP10GrainSplit), "") && calls.load() == 2, L"fills: 15 once, 12 once");
			bool line = false;
			for (const std::string& l : m.StatusLines())
				line = line || Has(l, "memo ahead plan");
			Assert::IsTrue(line, L"the plan's line is shown");
		}

		// MEMO-T4b (audit M3; the WIP 2c96feb67 scenario): ONLY the render-ahead depth is fed (no present facts, no graph): the
		// no-present edge is current - (H + R), so t - d is Reused while t renders, and KeepFrames() (the reported back) >= d.
		TEST_METHOD(NoPresentFactsKeepsRenderAheadDepth)
		{
			for (uint16_t d : { uint16_t(2), uint16_t(3), uint16_t(5) }) {
				SkeletonMemo m;
				std::atomic<int> calls{ 0 };
				m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, FrameFill(&calls));
				QueueWindowTerms t;
				t.renderAhead = d;
				t.wired = QueueWindowTerms::kRenderAhead;   // QueueDepth only
				std::map<uint64_t, const uint8_t*> made;
				for (uint64_t s = 1; s <= 20; ++s) {
					Render(m, s, t);
					Assert::IsTrue(m.KeepFrames() >= d, L"KeepFrames() reports the window's back");
					made[s] = m.Acquire(kP10GrainSplit, s, AcquireForm::Cpu).cpu;
					if (s > d) {
						const AcquireResult p = m.Acquire(kP10GrainSplit, s - d, AcquireForm::Cpu);
						Assert::IsTrue(p.status == AcquireStatus::Reused && p.cpu == made[s - d], L"t - d reused while t renders");
					}
				}
				Assert::AreEqual(20, calls.load(), L"once per frame");
				bool said = false;
				for (const std::string& l : m.StatusLines())
					said = said || Has(l, "present facts not fed");
				Assert::IsTrue(said, L"shown");
			}
		}

		// MEMO-T6b (audit M5): a flush on ANOTHER thread (the queue thread's OnQueue) moves the epoch at once: an ahead Acquire before the
		// next Begin never returns pre-flush bytes. And Reset followed by its Flush counts ONE discontinuity.
		TEST_METHOD(FlushThenImmediateAheadAcquireNeverOldBytes)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 }, generation{ 0 };
			OwnerFill f;
			f.fill = [&calls, &generation](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				calls.fetch_add(1);
				out.bytes.assign(8, uint8_t(r.seq + 100u * uint32_t(generation.load())));
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, f);
			Render(m, 10, Terms(1, 1, 0, 4, 0));
			const AcquireResult before = m.Acquire(kP10GrainSplit, 12, AcquireForm::Cpu);
			Assert::IsTrue(before.Ok() && before.cpu[0] == 12, L"pre-flush spot");
			generation = 1;   // the source after the flush is different content
			std::thread queueThread([&m]() { m.BumpEpoch(SkeletonMemo::InvalidateCause::Flush); });
			queueThread.join();
			const AcquireResult after = m.Acquire(kP10GrainSplit, 12, AcquireForm::Cpu);   // before any Begin
			Assert::IsTrue(after.status == AcquireStatus::FilledAtRead && after.cpu[0] == uint8_t(112), L"never the old bytes");
			Assert::IsTrue(before.cpu[0] == 12, L"the old reader's pin still holds its own bytes");
			// Reset then its Flush (ResyncExtension: a Reset is followed by a Flush): one discontinuity in one Begin interval.
			Render(m, 11, Terms(1, 1, 0, 4, 0));
			const uint64_t d0 = m.Discontinuities();
			m.BumpEpoch(SkeletonMemo::InvalidateCause::Reset);
			m.BumpEpoch(SkeletonMemo::InvalidateCause::Flush);
			Assert::IsTrue(m.Discontinuities() == d0 + 1, L"ResetThenFlushCountsOneDiscontinuity");
			Assert::IsTrue(m.Invalidations(SkeletonMemo::InvalidateCause::Reset) == 1 && m.Invalidations(SkeletonMemo::InvalidateCause::Flush) == 2,
				L"each cause still recorded");
		}

		// MEMO-T11 (audit M6): the grant request has one level per ladder step; a grant between two levels yields THAT level, never 0.
		TEST_METHOD(GrantBetweenLevelsYieldsThatLevel)
		{
			SkeletonMemo m;
			m.SetGpuDispatcherPresent(true);
			const uint16_t ids[] = { uint16_t(kP10GrainSplit), uint16_t(kP11RegionBlockStats) };
			const char* units[] = { kGrainUnit, kRegionUnit };
			const JobClass cls[] = { JobClass::Fill, JobClass::Core };
			for (int i = 0; i < 2; ++i) {
				OwnerFill g;
				g.reserveBytes = 1u << 20;
				g.jobClass = cls[i];
				g.aheadDepth = 1;
				g.fill = [](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
					static int tex = 0;
					out.gpu.resource = &tex;
					out.gpu.bytes = 1u << 20;
					return true;
				};
				m.RegisterOwnerFill(ProductId(ids[i]), AcquireForm::Gpu, units[i], g);
			}
			m.SetWindow(DeriveWindow(Terms(1, 1, 0, 6, 0), 10, 0));
			const std::vector<uint64_t> levels = m.GpuNeedLevels();
			Assert::IsTrue(levels.size() >= 3 && levels.front() == m.GpuNeedBytes(), L"one level per step, top = the full window");
			for (size_t i = 1; i < levels.size(); ++i)
				Assert::IsTrue(levels[i] < levels[i - 1], L"non-increasing, distinct");
			CardProfile card;
			card.vramBudgetBytes = 1ull << 34;
			card.vramUsageBytes = 0;
			// A budget that holds level 2 but not level 1: the governor grants level 2.
			const uint64_t mid = levels[1] - 1;
			card.vramBudgetBytes = uint64_t(double(mid) / kHeadroomStart) + 1;
			const MemoGpuGrant g = PlanMemoGpuGrantLevels(card, true, 0, levels);
			Assert::IsTrue(g.granted && g.bytes == levels[2], L"the largest level that fits, not 0");
			Assert::IsTrue(g.need == levels.front(), L"need = the top level");
		}

		// MEMO-T12 (audit S8): the ladder's plan applied through SetAheadPlan constrains Acquire (NotPlannedAhead beyond its depth).
		TEST_METHOD(PlanConstrainsAcquire)
		{
			SkeletonMemo m;
			m.SetGpuDispatcherPresent(true);
			m.SetGpuBudget(true, 6u << 20);
			OwnerFill g;
			g.reserveBytes = 1u << 20;
			g.jobClass = JobClass::Core;
			g.aheadDepth = 1;
			g.fill = [](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
				static int tex = 0;
				out.gpu.resource = &tex;
				out.gpu.bytes = 1u << 20;
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, g);
			const QueueWindow w = DeriveWindow(Terms(1, 1, 0, 8, 0), 10, 0);
			m.SetWindow(w);
			m.OnSeal(10);
			const AheadPlan plan = PlanAheadLadder(m.MakeAheadNeed(w, QueueMode::Normal));
			uint16_t depth = 0;
			Assert::IsTrue(plan.Has(uint16_t(kP10GrainSplit), &depth) && depth < w.ahead && plan.step != AheadStep::None, L"a stepped-down plan");
			m.SetAheadPlan(plan);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 10 + depth, AcquireForm::Gpu).Ok(), L"within the planned depth: filled");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 10 + depth + 1, AcquireForm::Gpu).cause == AcquireCause::NotPlannedAhead, L"beyond it: refused");
		}

		// The admission link: AheadNeed lists the allocating GPU fills (reserve, or the measured size when larger) with their class and
		// declared depth; views and CPU fills cost no VRAM.
		TEST_METHOD(AheadNeedDescribesTheRegisteredWork)
		{
			SkeletonMemo m;
			m.SetGpuDispatcherPresent(true);
			m.SetGpuBudget(true, 1u << 30);
			OwnerFill g;
			g.reserveBytes = 4096;
			g.jobClass = JobClass::Derived;
			g.aheadDepth = 2;
			g.fill = [](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
				static int tex = 0;
				out.gpu.resource = &tex;
				out.gpu.bytes = 4096;
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, g);
			m.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, Vpv::Capture::SourcePlanes::kUnit, Vpv::Capture::SourcePlanes::MakeOwnerFill());
			const QueueWindow w = DeriveWindow(Terms(1, 1, 0, 3, 0), 10, 0);
			const AheadNeed n = m.MakeAheadNeed(w, QueueMode::Normal);
			Assert::IsTrue(n.costs.size() == 1 && n.costs[0].id == kP10GrainSplit && n.costs[0].bytesPerFrame == 4096u
				&& n.costs[0].cls == JobClass::Derived && n.costs[0].declaredDepth == 2, L"the allocating fill, not the view");
			Assert::IsTrue(n.grantBytes == (1u << 30) && n.mode == QueueMode::Normal && n.window.ahead == 3, L"grant, mode, window");
			Assert::IsTrue(m.GpuNeedBytes() == 4096u, L"no window yet: the render frame only");
			m.SetWindow(w);
			Assert::IsTrue(m.GpuNeedBytes() == 4096u * (1u + w.back + w.ahead), L"GpuNeedBytes x the whole window");
		}
	};
}
