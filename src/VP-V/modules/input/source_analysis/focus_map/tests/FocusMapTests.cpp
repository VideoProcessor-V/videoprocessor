// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/focus_map (S18 F1, WAIVER-FOCUS-EDGEMAP-ONLY): v2 edge-map-only CPU reference
// (DefocusBlockEdge ramp, rack-focus clip, change along motion, camera stand-in, visible fallbacks, packed form)
// and the lag >= 1 owner fill (ZERO lag-0 kEdgeMap CPU requests, ZERO v1 interim fallback, non-latched
// ReadbackPending propagation, lag-2 retention vs FrameReleased, kHasGpuMirror == false). CPU only.
#include "pch.h"
#include "CppUnitTest.h"
using namespace Microsoft::VisualStudio::CppUnitTestFramework;

#include <VP-V/modules/input/source_analysis/focus_map/FocusMapFill.h>
#include <VP-V/modules/input/source_analysis/focus_map/FocusMapGlsl.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdge.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapFill.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace VideoProcessorTest
{
	using namespace FocusMap;
	using RegionEvidence::kPrecursorChannels;
	using RegionEvidence::kCoverage;
	using RegionEvidence::kE1;
	using RegionEvidence::kE2;
	using RegionEvidence::kE3;
	using RegionEvidence::kStatN;

	namespace
	{

		constexpr int kW = 128, kH = 96;   // block 8: a 16 x 12 grid; the P2 cell is 8 px too

		void Check(bool ok, const std::string& what)
		{
			if (ok) return;
			Logger::WriteMessage((what + "\n").c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		// A valid confident P11 block (N = 2, total = 53 -> confidence = 1.0) for edge-law normaliser + confidence.
		void ConfidentP11Block(float* b) { b[kCoverage] = 1; b[kStatN] = 2; b[kE1] = 42; b[kE2] = 10; b[kE3] = 3; }

		std::vector<float> ConfidentP11Scene(const Geometry& g)
		{
			std::vector<float> s(g.Blocks() * kPrecursorChannels, 0.0f);
			for (size_t i = 0; i < g.Blocks(); ++i)
				ConfidentP11Block(&s[i * kPrecursorChannels]);
			return s;
		}

		// A P2 grid whose backward vector is (bx, by) source px for every cell (S10.5 half-size px = 1/16 source px).
		std::vector<uint8_t> Field(const Geometry& g, double px, double py)
		{
			std::vector<uint8_t> f(static_cast<size_t>(g.p2gw) * g.p2gh * kP2BytesPerCell, 0);
			const int16_t vx = static_cast<int16_t>(std::lround(px * 16.0)), vy = static_cast<int16_t>(std::lround(py * 16.0));
			for (size_t i = 0; i < static_cast<size_t>(g.p2gw) * g.p2gh; ++i)
			{
				std::memcpy(&f[i * kP2BytesPerCell + 4], &vx, 2);
				std::memcpy(&f[i * kP2BytesPerCell + 6], &vy, 2);
			}
			return f;
		}

		int Count(const std::vector<Block>& b, uint16_t flag)
		{
			int n = 0;
			for (const Block& x : b) n += (x.flags & flag) ? 1 : 0;
			return n;
		}

		// The flag count of a published focus map (the 8-byte CPU form).
		int CountPacked(const VpSkeleton::AcquireResult& r, size_t blocks, uint16_t flag)
		{
			int n = 0;
			for (size_t i = 0; i < blocks; ++i) n += (Unpack(r.cpu + i * kBytesPerBlock).flags & flag) ? 1 : 0;
			return n;
		}

		std::vector<uint8_t> FloatBytes(const std::vector<float>& s)
		{
			std::vector<uint8_t> v(s.size() * sizeof(float));
			std::memcpy(v.data(), s.data(), v.size());
			return v;
		}

		// An edge-map CPU form (w x h float32 |G| codes): `g(x, y)` per pixel.
		std::vector<uint8_t> EdgeFieldSized(int w, int h, const std::function<float(int, int)>& g)
		{
			std::vector<float> e(static_cast<size_t>(w) * static_cast<size_t>(h));
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					e[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = g(x, y);
			return FloatBytes(e);
		}

		std::vector<uint8_t> EdgeField(const std::function<float(int, int)>& g)
		{
			return EdgeFieldSized(kW, kH, g);
		}

		// Edge map where block (bx, by) has uniform |G| = 60.0 (sharp -> defocus 0) if sharp(bx, by), else 5.0 (defocused -> defocus 1).
		std::vector<uint8_t> EdgeScene(const Geometry& g, const std::function<bool(int, int)>& sharp)
		{
			return EdgeFieldSized(g.width, g.height, [&](int x, int y) {
				return sharp(x / g.block, y / g.block) ? 60.0f : 5.0f;
			});
		}

		struct EdgeSumsHolder
		{
			std::vector<float> s2, s4;
			EdgeInput View() const
			{
				EdgeInput e;
				e.sum2 = s2.data();
				e.sum4 = s4.data();
				return e;
			}
		};

		EdgeSumsHolder MakeEdgeSums(const Geometry& g, const std::vector<uint8_t>& edgeBytes)
		{
			EdgeSumsHolder h;
			EdgeSums(g, edgeBytes.data(), h.s2, h.s4);
			return h;
		}

		bool Contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
	}

	TEST_CLASS(FocusMapTests)
	{
		public:
			// WAIVER-FOCUS-EDGEMAP-ONLY: migrated from deleted v1 DefocusBlock to v2 DefocusBlockEdge.
			// Synthetic edge-gradient ramp: peak |G| rising monotonically drives defocus monotonically from 1 to 0;
			// uncovered / flat blocks carry zero confidence; unserved EdgeInput in ComputeGrid produces zero valid blocks (no v1 fallback).
			TEST_METHOD(DefocusRampIsMonotone)
			{
				Assert::IsFalse(FocusMap::kHasGpuMirror, L"v1 interim GLSL mirror deleted in S18 F1 (kHasGpuMirror == false)");
				Assert::AreEqual(uint32_t(2), FocusMap::kUnitVersion, L"FocusMap unit version stays pinned at v2");

				const Params p;
				float b[kPrecursorChannels] = {};
				ConfidentP11Block(b);
				float prev = 2.0f;
				float first = -1, last = -1;
				for (int i = 0; i <= 60; ++i)
				{
					const float gval = 5.0f + static_cast<float>(i);   // 5 .. 65 codes
					const float g2 = gval * gval;
					const float sum2 = 64.0f * g2;
					const float sum4 = 64.0f * g2 * g2;
					float d = 0.0f, c = 0.0f;
					uint32_t f = 0;
					DefocusBlockEdge<float>(p, b, sum2, sum4, d, c, f);
					Check((f & kValid) != 0, "a covered block is valid");
					Check(d <= prev + 1e-6f, "defocus must not rise as edge gradient grows (step " + std::to_string(i) + ")");
					prev = d;
					if (i == 0) first = d;
					last = d;
				}
				Check(first > 0.99f && last < 0.01f, "the ramp spans defocused (" + std::to_string(first) + ") to sharp (" + std::to_string(last) + ")");

				float off[kPrecursorChannels] = {};   // coverage 0: no evidence, never valid, never flagged
				float d = 0.0f, c = 0.0f;
				uint32_t f = 0;
				DefocusBlockEdge<float>(p, off, 64.0f * 3600.0f, 64.0f * 3600.0f * 3600.0f, d, c, f);
				Check(!(f & kValid) && c == 0.0f, "an uncovered block carries no evidence");

				float flat[kPrecursorChannels] = {};   // flat sky: tiny energy -> confidence 0, so it can never flag a change
				flat[kCoverage] = 1; flat[kStatN] = 0.1f; flat[kE1] = 0.2f; flat[kE2] = 0.1f; flat[kE3] = 0.1f;
				DefocusBlockEdge<float>(p, flat, 64.0f * 25.0f, 64.0f * 625.0f, d, c, f);
				Check(c < 0.05f, "a flat block has no focus confidence");

				// Negative control: ComputeGrid without a served EdgeInput MUST NOT run any v1 interim law.
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> p11 = ConfidentP11Scene(g);
				std::vector<Block> unservedOut;
				ComputeGrid(p, g, p11.data(), p11.data(), nullptr, nullptr, false, unservedOut, EdgeInput(), EdgeInput());
				Check(Count(unservedOut, kValid) == 0 && Count(unservedOut, kChanged) == 0,
					"negative control: ComputeGrid with unserved EdgeInput emits zero valid blocks (no v1 interim fallback)");
			}

			// WAIVER-FOCUS-EDGEMAP-ONLY: migrated to v2 edge law (ComputeGrid + EdgeSums).
			// Rack focus: the left half goes from crisp to soft (and back). Exactly those blocks are flagged; the right half is not.
			TEST_METHOD(RackFocusFlagsTheChangedRegion)
			{
				const Geometry g = Geometry::Make(kW, kH);
				Check(g.gw == 16 && g.gh == 12 && g.block == 8, "test geometry");
				const std::vector<float> p11 = ConfidentP11Scene(g);
				const EdgeSumsHolder eSharp = MakeEdgeSums(g, EdgeScene(g, [](int, int) { return true; }));
				const EdgeSumsHolder eLeftSoft = MakeEdgeSums(g, EdgeScene(g, [](int bx, int) { return bx >= 8; }));

				std::vector<Block> out;
				ComputeGrid(Params(), g, p11.data(), p11.data(), nullptr, nullptr, false, out, eLeftSoft.View(), eSharp.View());
				Check(Count(out, kChanged) == 8 * 12, "the defocused half is flagged: " + std::to_string(Count(out, kChanged)));
				Check(Count(out, kSharper) == 0, "nothing got sharper");
				Check(Count(out, kNoMotion) == static_cast<int>(g.Blocks()), "no P2 and no P7: every block says so (visible)");
				for (int by = 0; by < g.gh; ++by)
					for (int bx = 0; bx < g.gw; ++bx)
					{
						const Block& x = out[static_cast<size_t>(by) * g.gw + bx];
						Check(((x.flags & kChanged) != 0) == (bx < 8), "changed exactly where the focus changed");
						Check(bx < 8 ? x.delta > 0.9f : std::fabs(x.delta) < 1e-6f, "delta = the defocus step");
					}

				// Rack back: the region gets sharper.
				ComputeGrid(Params(), g, p11.data(), p11.data(), nullptr, nullptr, false, out, eSharp.View(), eLeftSoft.View());
				Check(Count(out, kChanged) == 8 * 12 && Count(out, kSharper) == 8 * 12, "the reverse rack flags sharper");

				// No previous edge map: visible kNoPrevious, no change flag.
				ComputeGrid(Params(), g, p11.data(), p11.data(), nullptr, nullptr, false, out, eSharp.View(), EdgeInput());
				Check(Count(out, kNoPrevious) == static_cast<int>(g.Blocks()) && Count(out, kChanged) == 0, "no previous edge map: no change flag");

				// P10 unavailable: kNoSplit on every block.
				ComputeGrid(Params(), g, p11.data(), p11.data(), nullptr, nullptr, true, out, eLeftSoft.View(), eSharp.View());
				Check(Count(out, kNoSplit) == static_cast<int>(g.Blocks()), "no split is visible on every block");
			}

			// WAIVER-FOCUS-EDGEMAP-ONLY: migrated to v2 edge law (ComputeGrid + EdgeSums).
			// The same sharp object panned by two blocks: with P2 vectors (or P7 camera) nothing changed; without motion
			// the object's old and new positions falsely look like focus changes (mandatory same-input negative control).
			TEST_METHOD(ChangeFollowsTheMotion)
			{
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> p11 = ConfidentP11Scene(g);
				const EdgeSumsHolder ePrev = MakeEdgeSums(g, EdgeScene(g, [](int bx, int) { return bx >= 4 && bx < 6; }));
				const EdgeSumsHolder eCur = MakeEdgeSums(g, EdgeScene(g, [](int bx, int) { return bx >= 6 && bx < 8; }));

				std::vector<Block> out;
				const std::vector<uint8_t> p2 = Field(g, -16.0, 0.0);   // the content was 16 px to the left in the previous frame
				ComputeGrid(Params(), g, p11.data(), p11.data(), p2.data(), nullptr, false, out, eCur.View(), ePrev.View());
				Check(Count(out, kChanged) == 0, "with the P2 vectors the pan is not a focus change: " + std::to_string(Count(out, kChanged)));
				Check(Count(out, kNoMotion) == 0 && Count(out, kCameraMotion) == 0, "the field was used");
				Check(Count(out, kNoPrevious) == 2 * 12, "two edge columns look outside the previous frame: visible, not flagged");

				const float p7[4] = { 16.0f, 0.0f, 1.0f, 1.0f };   // the camera moved the content 16 px right
				ComputeGrid(Params(), g, p11.data(), p11.data(), nullptr, p7, false, out, eCur.View(), ePrev.View());
				Check(Count(out, kChanged) == 0, "the P7 stand-in gives the same answer");
				Check(Count(out, kCameraMotion) == static_cast<int>(g.Blocks()), "the camera stand-in is visible on every block");

				// Mandatory same-input negative control: without motion, the exact same panned edge maps flag 4 * 12 blocks.
				ComputeGrid(Params(), g, p11.data(), p11.data(), nullptr, nullptr, false, out, eCur.View(), ePrev.View());
				Check(Count(out, kChanged) == 4 * 12, "negative control: without motion the object's old and new places flag: " + std::to_string(Count(out, kChanged)));

				const float p7bad[4] = { 16.0f, 0.0f, 0.0f, 0.0f };   // an invalid camera fit is not used
				ComputeGrid(Params(), g, p11.data(), p11.data(), nullptr, p7bad, false, out, eCur.View(), ePrev.View());
				Check(Count(out, kNoMotion) == static_cast<int>(g.Blocks()) && Count(out, kCameraMotion) == 0, "an invalid P7 is absent");
			}

			TEST_METHOD(PackedFormRoundTrips)
			{
				Block b;
				b.defocus = 0.37f; b.delta = -0.62f; b.confidence = 0.91f; b.flags = kValid | kChanged | kSharper;
				uint8_t raw[kBytesPerBlock];
				Pack(b, raw);
				const Block r = Unpack(raw);
				Check(std::fabs(r.defocus - b.defocus) < 2e-5f && std::fabs(r.delta - b.delta) < 4e-5f && std::fabs(r.confidence - b.confidence) < 2e-5f
					&& r.flags == b.flags, "the 8-byte form round-trips");
			}

			// WAIVER-FOCUS-EDGEMAP-ONLY: every input is acquired at lag >= 1 (NEVER lag 0 for kEdgeMap CPU),
			// each input is built ONCE however many readers there are, frame 0 declines visibly (no lag >= 1 edge map),
			// and an unavailable P2 falls back visibly to P7.
			TEST_METHOD(AcquireBuildsEveryInputOnce)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> p11Scene = ConfidentP11Scene(g);
				const std::vector<uint8_t> crispEdge = EdgeScene(g, [](int, int) { return true; });
				const std::vector<uint8_t> softLeftEdge = EdgeScene(g, [](int bx, int) { return bx >= 8; });

				SkeletonMemo m;
				std::atomic<int> p11Calls{ 0 }, p10Calls{ 0 }, p2Calls{ 0 }, p7Calls{ 0 }, edgeCalls{ 0 };
				std::vector<uint64_t> edgeSeqsAsked;
				OwnerFill fe;
				fe.keepFrames = 2;
				fe.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) {
					++edgeCalls;
					edgeSeqsAsked.push_back(r.seq);
					// Frame 4 (lag-1 of focus frame 5): left half defocused; all other frames crisp.
					o.bytes = (r.seq == 4u) ? softLeftEdge : crispEdge;
					return true;
				};
				OwnerFill f11;
				f11.keepFrames = 2;
				f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) {
					++p11Calls;
					o.bytes = FloatBytes(p11Scene);
					return true;
				};
				OwnerFill f10;
				f10.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { ++p10Calls; o.bytes.assign(64, 1); return true; };
				OwnerFill f2;
				f2.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput&) { ++p2Calls; return false; };   // no vector grid
				OwnerFill f7;
				f7.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) {
					++p7Calls;
					const float v[4] = { 0.0f, 0.0f, 1.0f, 1.0f };   // a static camera
					o.bytes.resize(sizeof(v));
					std::memcpy(o.bytes.data(), v, sizeof(v));
					return true;
				};
				Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", fe) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, "repair/grain_split", f10) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP2VectorPair, AcquireForm::Cpu, "motion/estimation", f2) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP7CameraFit, AcquireForm::Cpu, "motion/estimation/global_model", f7) == SkeletonMemo::Registration::Registered);
				FillConfig cfg;
				cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered,
					L"the focus owner is the catalog's owner of kFocusMap");

				// Frame 0 has no lag >= 1 prior edge map -> declines visibly without ever asking kEdgeMap at lag 0.
				const AcquireResult r0 = m.Acquire(kFocusMap, 0u, AcquireForm::Cpu);
				Assert::IsTrue(r0.status == AcquireStatus::Unavailable && r0.cause == AcquireCause::OwnerDeclined,
					L"frame 0 declines visibly because lag >= 1 edge map does not exist");
				Assert::AreEqual(0, edgeCalls.load(), L"frame 0 makes ZERO kEdgeMap requests");

				// Focus frame 5 reads lag-1 = 4 and lag-2 = 3 (NEVER lag-0 = 5).
				const AcquireResult a = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
				Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && a.cpuBytes == g.Blocks() * kBytesPerBlock, L"the map is built and published");
				for (uint64_t s : edgeSeqsAsked)
					Assert::IsTrue(s < 5u, L"kEdgeMap CPU MUST NEVER be requested at lag 0 (seq == 5)");
				const AcquireResult b = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
				Assert::IsTrue(b.status == AcquireStatus::Reused && a.cpu == b.cpu, L"the second reader reuses the one map");
				Check(edgeCalls == 2 && p11Calls == 2 && p10Calls == 0 && p2Calls == 1 && p7Calls == 1,
					"every lag>=1 input built once: edge " + std::to_string(edgeCalls.load()) + ", p11 " + std::to_string(p11Calls.load())
					+ ", p10 " + std::to_string(p10Calls.load()) + ", p2 " + std::to_string(p2Calls.load()) + ", p7 " + std::to_string(p7Calls.load()));
				Assert::IsTrue(m.CountersOf(kFocusMap).ownerFillCalls == 2u && m.CountersOf(kFocusMap).filledAtRead == 1u && m.CountersOf(kFocusMap).reusedMemo == 1u);

				// Left half changed (frame 4 soft vs frame 3 crisp) through the P7 stand-in.
				int changed = 0, camera = 0, noMotion = 0;
				for (size_t i = 0; i < g.Blocks(); ++i)
				{
					const Block x = Unpack(a.cpu + i * kBytesPerBlock);
					changed += (x.flags & kChanged) ? 1 : 0;
					camera += (x.flags & kCameraMotion) ? 1 : 0;
					noMotion += (x.flags & kNoMotion) ? 1 : 0;
				}
				Check(changed == 8 * 12 && camera == static_cast<int>(g.Blocks()) && noMotion == 0, "left half flagged through the P7 stand-in");

				// Focus frame 6 reads lag-1 = 5 (new) and lag-2 = 4 (reused from frame 5's lag-1!), NEVER lag-0 = 6.
				edgeSeqsAsked.clear();
				const AcquireResult c = m.Acquire(kFocusMap, 6u, AcquireForm::Cpu);
				Assert::IsTrue(c.Ok());
				for (uint64_t s : edgeSeqsAsked)
					Assert::IsTrue(s < 6u, L"kEdgeMap CPU MUST NEVER be requested at lag 0 (seq == 6)");
				Check(edgeCalls == 3 && p11Calls == 3 && p10Calls == 0 && p2Calls == 2 && p7Calls == 2,
					"the next frame builds only its own lag-1 inputs (reusing lag-2 frame 4): edge " + std::to_string(edgeCalls.load()));
				int sharper = 0;
				for (size_t i = 0; i < g.Blocks(); ++i) sharper += (Unpack(c.cpu + i * kBytesPerBlock).flags & kSharper) ? 1 : 0;
				Check(sharper == 8 * 12, "the focus returning is flagged sharper");
				Check(cfg.stats->fills == 2 && cfg.stats->edgeLaw == 2, "the edge-law fill ran once per served frame");
			}

			// WAIVER-FOCUS-EDGEMAP-ONLY (replaces WithoutAnEdgeMapTheFillIsTheInterimLawBitForBit):
			// S18 F1 deleted the v1 interim fallback. Without a valid kEdgeMap CPU form (missing owner OR wrong byte size),
			// Acquire(kFocusMap) declines visibly (OwnerDeclined), increments edgeUnavailable, and engages product.focus_map
			// on the Fallbacks list. Positive control on a fresh memo with valid kEdgeMap succeeds and releases the decision.
			TEST_METHOD(WithoutAnEdgeMapTheFillDeclinesVisiblyWithZeroInterimFallback)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> p11 = ConfidentP11Scene(g);

				// Case 1: no kEdgeMap CPU owner fill registered -> declines visibly, ZERO interim fallback.
				{
					SkeletonMemo m;
					OwnerFill f11;
					f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = FloatBytes(p11); return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
					FillConfig cfg;
					cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);
					Assert::IsFalse(m.HasOwnerFill(kEdgeMap, AcquireForm::Cpu), L"no kEdgeMap CPU owner registered");

					const AcquireResult r = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
					Assert::IsTrue(r.status == AcquireStatus::Unavailable && r.cause == AcquireCause::OwnerDeclined,
						L"without kEdgeMap CPU, Acquire(kFocusMap) declines with OwnerDeclined (NEVER interim fill)");
					Assert::AreEqual(uint64_t(0), cfg.stats->fills.load(), L"zero fills succeeded without kEdgeMap");
					Assert::AreEqual(uint64_t(0), cfg.stats->edgeLaw.load());
					Assert::AreEqual(uint64_t(1), cfg.stats->edgeUnavailable.load(), L"edgeUnavailable counted");
					Assert::IsTrue(cfg.stats->lastEdgeUnavailable.load());
					const FallbackCatalog::Decision d = StatusDecision(cfg.stats.get(), true);
					Check(d.engaged && d.id == "product.focus_map" && Contains(d.reason, "edge_map lag-1 CPU unavailable")
						&& Contains(d.reason, Token(AcquireCause::NoOwnerFill)),
						"visible on product.focus_map Fallbacks line with cause: " + d.reason);

					// Negative control #2: kEdgeMap CPU serves wrong byte size -> still declines visibly (NO interim fallback).
					OwnerFill badSizeEdge;
					badSizeEdge.fill = [](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) {
						o.bytes.assign(16u, 0);   // far smaller than kW * kH * sizeof(float)
						return true;
					};
					Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", badSizeEdge) == SkeletonMemo::Registration::Registered);
					const AcquireResult rWrongSize = m.Acquire(kFocusMap, 6u, AcquireForm::Cpu);
					Assert::IsTrue(rWrongSize.status == AcquireStatus::Unavailable && rWrongSize.cause == AcquireCause::OwnerDeclined,
						L"wrong-size kEdgeMap CPU declines visibly (zero interim fallback)");
					Assert::AreEqual(uint64_t(0), cfg.stats->fills.load(), L"still zero fills succeeded");
					Assert::AreEqual(uint64_t(2), cfg.stats->edgeUnavailable.load());
					const FallbackCatalog::Decision dWrong = StatusDecision(cfg.stats.get(), true);
					Check(dWrong.engaged && Contains(dWrong.reason, "served at wrong size"),
						"wrong-size decline worded visibly: " + dWrong.reason);
				}

				// Positive control: once valid kEdgeMap CPU is supplied, Acquire(kFocusMap, 5) succeeds with v2 edge law and releases decision.
				{
					SkeletonMemo m;
					OwnerFill f11;
					f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = FloatBytes(p11); return true; };
					OwnerFill fe;
					fe.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = EdgeField([](int, int) { return 60.0f; }); return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
					Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", fe) == SkeletonMemo::Registration::Registered);
					FillConfig cfg;
					cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);

					const AcquireResult rOk = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
					Assert::IsTrue(rOk.status == AcquireStatus::FilledAtRead && rOk.Ok(), L"positive control: valid kEdgeMap serves v2 edge-law focus map");
					Assert::AreEqual(uint64_t(1), cfg.stats->edgeLaw.load());
					Assert::AreEqual(uint64_t(0), cfg.stats->edgeUnavailable.load());
					Assert::IsFalse(StatusDecision(cfg.stats.get(), true).engaged, L"Fallbacks decision released once served");
				}
			}

			// S18 F1 + R1: when kEdgeMap CPU at lag-1 (or lag-2) returns ReadbackPending, Acquire(kFocusMap) propagates
			// non-latched ReadbackPending (NEVER OwnerDeclined, NEVER interim fill) and succeeds on retry once ready.
			// Same-input negative control: ordinary decline (out.pending == false) latches OwnerDeclined and does NOT retry.
			TEST_METHOD(PendingLagOneEdgeMapPropagatesNonLatchedReadbackPending)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> p11 = ConfidentP11Scene(g);
				const std::vector<uint8_t> crisp = EdgeScene(g, [](int, int) { return true; });
				const std::vector<uint8_t> softLeft = EdgeScene(g, [](int bx, int) { return bx >= 8; });

				SkeletonMemo m;
				std::atomic<bool> readyEdge4{ false };
				std::atomic<bool> readyEdge5{ false };
				std::atomic<int> edge4Calls{ 0 }, edge5Calls{ 0 }, edge8Calls{ 0 };
				std::atomic<bool> allowEdge8{ false };
				std::vector<uint64_t> edgeSeqs;

				OwnerFill fe;
				fe.keepFrames = 2;
				fe.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) -> bool {
					edgeSeqs.push_back(r.seq);
					if (r.seq == 4u)
					{
						edge4Calls.fetch_add(1);
						if (!readyEdge4.load())
						{
							o.pending = true;
							o.declineReason = "edge_map lag-1 in flight";
							return false;
						}
						o.bytes = softLeft;
						return true;
					}
					if (r.seq == 5u)
					{
						edge5Calls.fetch_add(1);
						if (!readyEdge5.load())
						{
							o.pending = true;
							o.declineReason = "edge_map lag-2 in flight";
							return false;
						}
						o.bytes = crisp;
						return true;
					}
					if (r.seq == 8u)
					{
						// Negative control: ordinary decline (out.pending == false) on first call.
						edge8Calls.fetch_add(1);
						if (!allowEdge8.load())
						{
							o.pending = false;
							o.declineReason = "edge_map hard decline";
							return false;
						}
						o.bytes = crisp;
						return true;
					}
					o.bytes = crisp;
					return true;
				};
				OwnerFill f11;
				f11.keepFrames = 2;
				f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = FloatBytes(p11); return true; };

				Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", fe) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
				FillConfig cfg;
				cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);

				// 1. Lag-1 kEdgeMap(4) pending -> Acquire(kFocusMap, 5) returns non-latched ReadbackPending.
				const AcquireResult rPend1 = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
				Assert::IsTrue(rPend1.status == AcquireStatus::Unavailable && rPend1.cause == AcquireCause::ReadbackPending,
					L"pending lag-1 kEdgeMap propagates ReadbackPending (NOT OwnerDeclined, NOT interim fill)");
				Assert::AreEqual(uint64_t(1), cfg.stats->edgePending.load(), L"edgePending counted");
				Assert::AreEqual(uint64_t(0), cfg.stats->fills.load(), L"no fill published while pending");
				Assert::AreEqual(1, edge4Calls.load());
				for (uint64_t s : edgeSeqs)
					Assert::IsTrue(s < 5u, L"ZERO lag-0 kEdgeMap requests during pending Acquire(5)");

				// Once kEdgeMap(4) is ready, retrying Acquire(kFocusMap, 5) succeeds cleanly!
				readyEdge4.store(true);
				const AcquireResult rReady1 = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
				Assert::IsTrue(rReady1.status == AcquireStatus::FilledAtRead && rReady1.Ok(),
					L"retry of Acquire(kFocusMap, 5) after lag-1 kEdgeMap ready succeeds with FilledAtRead");
				Assert::AreEqual(2, edge4Calls.load(), L"kEdgeMap(4) re-invoked on retry");
				Assert::AreEqual(static_cast<int>(g.Blocks()), CountPacked(rReady1, g.Blocks(), kValid), L"all blocks valid under v2 edge law");
				Assert::AreEqual(8 * 12, CountPacked(rReady1, g.Blocks(), kChanged), L"soft left half flagged changed");
				Assert::AreEqual(uint64_t(1), cfg.stats->edgeLaw.load());

				// 2. Lag-2 kEdgeMap(5) pending while lag-1 kEdgeMap(6) is ready -> Acquire(kFocusMap, 7) propagates ReadbackPending, then retries cleanly.
				const AcquireResult rPendLag2 = m.Acquire(kFocusMap, 7u, AcquireForm::Cpu);
				Assert::IsTrue(rPendLag2.status == AcquireStatus::Unavailable && rPendLag2.cause == AcquireCause::ReadbackPending,
					L"pending lag-2 kEdgeMap(5) propagates non-latched ReadbackPending");
				readyEdge5.store(true);
				const AcquireResult rReadyLag2 = m.Acquire(kFocusMap, 7u, AcquireForm::Cpu);
				Assert::IsTrue(rReadyLag2.status == AcquireStatus::FilledAtRead && rReadyLag2.Ok(),
					L"retry of Acquire(kFocusMap, 7) succeeds once lag-2 kEdgeMap(5) is ready");
				Assert::AreEqual(2, edge5Calls.load());

				// 3. Mandatory same-input negative control: ordinary decline on kEdgeMap(8) latches OwnerDeclined on kFocusMap(9).
				const uint64_t focusCallsBefore9 = m.CountersOf(kFocusMap).ownerFillCalls;
				const AcquireResult rDecl1 = m.Acquire(kFocusMap, 9u, AcquireForm::Cpu);
				Assert::IsTrue(rDecl1.status == AcquireStatus::Unavailable && rDecl1.cause == AcquireCause::OwnerDeclined,
					L"negative control: ordinary decline of kEdgeMap(8) produces OwnerDeclined on kFocusMap(9)");
				Assert::AreEqual(1, edge8Calls.load());

				allowEdge8.store(true);
				const AcquireResult rDecl2 = m.Acquire(kFocusMap, 9u, AcquireForm::Cpu);
				Assert::IsTrue(rDecl2.status == AcquireStatus::Unavailable && rDecl2.cause == AcquireCause::OwnerDeclined,
					L"negative control: latched OwnerDeclined on kFocusMap(9) stays OwnerDeclined on retry");
				Assert::AreEqual(focusCallsBefore9 + 1u, m.CountersOf(kFocusMap).ownerFillCalls,
					L"negative control: latched OwnerDeclined does NOT re-invoke focus owner fill");
				Assert::AreEqual(1, edge8Calls.load(),
					L"negative control: latched OwnerDeclined does NOT re-invoke kEdgeMap(8)");
			}

			// The edge law on a real Sobel field (RegionEvidence::BusyEdge::SobelMagnitude, the edge map's CPU reference): a vertical step of
			// 40 codes blurred into a ramp of W px reads MORE defocused as W grows, sharp for a step and defocused for a 24 px ramp, with
			// the P11 band energies (the normaliser and the confidence) held fixed: the edge map alone moves the level.
			TEST_METHOD(EdgeLawDefocusFollowsTheBlurOfTheEdgeMap)
			{
				const Params p;
				float s[kPrecursorChannels] = {};
				s[kCoverage] = 1; s[kStatN] = 0; s[kE1] = 40; s[kE2] = 10; s[kE3] = 3;   // total 53, the same for every W
				const int tw = 32, th = 8;
				const int widths[] = { 1, 3, 6, 12, 24 };
				float d[5] = {};
				for (int k = 0; k < 5; ++k)
				{
					const double w = widths[k];
					std::vector<float> luma(static_cast<size_t>(tw) * th);
					for (int y = 0; y < th; ++y)
						for (int x = 0; x < tw; ++x)
						{
							double t = (x + 0.5 - (16.0 - w / 2.0)) / w;
							t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
							luma[static_cast<size_t>(y) * tw + x] = static_cast<float>(400.0 + 40.0 * t);
						}
					const std::vector<float> gmag = RegionEvidence::BusyEdge::SobelMagnitude(luma.data(), tw, th);
					double s2 = 0, s4 = 0;
					for (float v : gmag) { s2 += double(v) * v; s4 += double(v) * v * v * v; }
					float c;
					uint32_t f;
					DefocusBlockEdge<float>(p, s, static_cast<float>(s2), static_cast<float>(s4), d[k], c, f);
					Check((f & kValid) != 0 && c > 0.99f, "valid, confident (the P11 energy is fixed)");
					if (k > 0) Check(d[k] >= d[k - 1] - 1e-6f, "defocus must not fall as the edge is blurred (W " + std::to_string(widths[k]) + ")");
				}
				Check(d[0] < 0.05f && d[4] > 0.9f, "a step reads sharp (" + std::to_string(d[0]) + "), a 24 px ramp defocused (" + std::to_string(d[4]) + ")");
				// The per-block reduction of the edge map: block (bx, by) holding |G| = bx + 1 everywhere sums to 64 (bx + 1)^2 / ^4.
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<uint8_t> field = EdgeField([&](int x, int) { return static_cast<float>(x / g.block + 1); });
				std::vector<float> e2, e4;
				EdgeSums(g, field.data(), e2, e4);
				for (int bx = 0; bx < g.gw; ++bx)
				{
					const double v = bx + 1.0, px = double(g.block) * g.block;
					Check(std::fabs(e2[static_cast<size_t>(bx)] - px * v * v) < 1e-3 * px * v * v && std::fabs(e4[static_cast<size_t>(bx)] - px * v * v * v * v) < 1e-3 * px * v * v * v * v,
						"EdgeSums block " + std::to_string(bx));
				}
			}

			// WAIVER-FOCUS-EDGEMAP-ONLY: lag >= 1 rack focus driven by the EDGE MAP (P11 constant across frames):
			// kEdgeMap loses gradient in the left half at frame 4 (lag-1 of focus frame 5); focus frame 5 flags that half,
			// with ZERO lag-0 edge-map calls. Negative control without kEdgeMap declines visibly (zero interim fallback).
			TEST_METHOD(EdgeMapDrivesTheFocusChangeThroughAcquire)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> sharpP11 = ConfidentP11Scene(g);
				const std::vector<uint8_t> crisp = EdgeField([](int, int) { return 60.0f; });
				const std::vector<uint8_t> soft = EdgeField([](int x, int) { return x < kW / 2 ? 5.0f : 60.0f; });
				for (int withEdge = 1; withEdge >= 0; --withEdge)
				{
					SkeletonMemo m;
					std::atomic<int> p11Calls{ 0 }, edgeCalls{ 0 };
					std::vector<uint64_t> edgeSeqs;
					OwnerFill f11;
					f11.keepFrames = 2;
					f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { ++p11Calls; o.bytes = FloatBytes(sharpP11); return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
					if (withEdge)
					{
						OwnerFill fe;
						fe.keepFrames = 2;
						fe.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) {
							++edgeCalls;
							edgeSeqs.push_back(r.seq);
							o.bytes = (r.seq == 4u) ? soft : crisp;
							return true;
						};
						Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", fe) == SkeletonMemo::Registration::Registered,
							L"the edge map's CPU form is registered by its catalog owner unit");
					}
					FillConfig cfg;
					cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);
					const AcquireResult r4 = m.Acquire(kFocusMap, 4u, AcquireForm::Cpu);
					const AcquireResult r5 = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
					if (withEdge)
					{
						Assert::IsTrue(r4.Ok() && r5.Ok());
						for (uint64_t s : edgeSeqs)
							Assert::IsTrue(s <= 4u, L"ZERO lag-0 kEdgeMap calls (max asked for focus frame 5 is lag-1 == 4)");
						const int changed = CountPacked(r5, g.Blocks(), kChanged);
						Check(changed == 8 * 12 && CountPacked(r5, g.Blocks(), kSharper) == 0, "the edge map's soft half is flagged: " + std::to_string(changed));
						for (size_t i = 0; i < g.Blocks(); ++i)
							Check(((Unpack(r5.cpu + i * kBytesPerBlock).flags & kChanged) != 0) == (static_cast<int>(i % g.gw) < g.gw / 2), "changed exactly where the edge map softened");
						Check(CountPacked(r5, g.Blocks(), kNoPrevious) == 0, "the edge law on both lag-1 and lag-2 frames");
						Check(edgeCalls == 3 && m.CountersOf(kEdgeMap).ownerFillCalls == 3u, "edge map frames 2, 3, 4 each built once: " + std::to_string(edgeCalls.load()));
						Check(p11Calls == 3, "P11 frames 2, 3, 4 each built once");
						Check(cfg.stats->edgeLaw == 2 && cfg.stats->edgeUnavailable == 0, "the edge law is counted");
						Check(!StatusDecision(cfg.stats.get(), true).engaged, "served: the Fallbacks line is released");
					}
					else
					{
						Check(!r4.Ok() && !r5.Ok() && r5.cause == AcquireCause::OwnerDeclined,
							"negative control: without kEdgeMap CPU, Acquire(kFocusMap) declines visibly (no v1 interim fallback)");
						Check(cfg.stats->edgeUnavailable == 2 && StatusDecision(cfg.stats.get(), true).engaged,
							"decline counted and visible on Fallbacks line");
					}
				}
			}

			// WAIVER-FOCUS-EDGEMAP-ONLY: lag-1 edge map (frame 4) served, lag-2 edge map (frame 3) declined ->
			// focus frame 5 runs v2 edge law on frame 4 with kNoPrevious on every block (never a mixed or fabricated delta).
			TEST_METHOD(EdgeLawNeverComparesAcrossTwoLaws)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> sharpP11 = ConfidentP11Scene(g);
				const std::vector<uint8_t> soft = EdgeField([](int x, int) { return x < kW / 2 ? 5.0f : 60.0f; });
				SkeletonMemo m;
				OwnerFill f11;
				f11.keepFrames = 2;
				f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = FloatBytes(sharpP11); return true; };
				OwnerFill fe;
				fe.keepFrames = 2;
				fe.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) {
					Assert::IsTrue(r.seq < 5u, L"ZERO lag-0 kEdgeMap request");
					if (r.seq != 4u) return false;   // serve lag-1 (4), decline lag-2 (3)
					o.bytes = soft;
					return true;
				};
				Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", fe) == SkeletonMemo::Registration::Registered);
				FillConfig cfg;
				cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);
				const AcquireResult r = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
				Assert::IsTrue(r.Ok());
				Check(CountPacked(r, g.Blocks(), kNoPrevious) == static_cast<int>(g.Blocks()) && CountPacked(r, g.Blocks(), kChanged) == 0,
					"no lag-2 reference under the edge law: kNoPrevious, never a fabricated delta");
				Check(CountPacked(r, g.Blocks(), kNoPreviousRetained) == 0, "declined lag-2 is absent, not released");
				int defocused = 0;
				for (size_t i = 0; i < g.Blocks(); ++i) defocused += Unpack(r.cpu + i * kBytesPerBlock).defocus > 0.9f ? 1 : 0;
				Check(defocused == 8 * 12, "the soft half is defocused under the edge law: " + std::to_string(defocused));
			}

			// WAIVER-FOCUS-EDGEMAP-ONLY (lag-2 retention for lag-1 focus_map):
			// At OnSeal(6), Acquire(kFocusMap, 6) reads lag-1 = frame 5 and lag-2 = frame 4.
			// With keepFrames = 2, frame 4 survives OnSeal(6) and focus_map(6) has its delta reference.
			// With keepFrames = 1 (insufficient for lag-2), frame 4 is FrameReleased at OnSeal(6): every block sets
			// kNoPrevious | kNoPreviousRetained, counted and visible on the product.focus_map Fallbacks line, and P11(4) is never recomputed.
			TEST_METHOD(LagOneP11IsServedFromTheKeptSpotOrVisiblyReleased)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> sharpP11 = ConfidentP11Scene(g);
				const std::vector<uint8_t> crisp = EdgeField([](int, int) { return 60.0f; });
				for (int keep = 2; keep >= 1; --keep)
				{
					SkeletonMemo m;
					std::atomic<int> p11Calls{ 0 };
					OwnerFill f11;
					f11.keepFrames = static_cast<uint8_t>(keep);
					f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { ++p11Calls; o.bytes = FloatBytes(sharpP11); return true; };
					OwnerFill fe;
					fe.keepFrames = 2;
					fe.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = crisp; return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
					Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", fe) == SkeletonMemo::Registration::Registered);
					FillConfig cfg;
					cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);

					Assert::IsTrue(m.Acquire(kP11RegionBlockStats, 4u, AcquireForm::Cpu).Ok());
					Assert::IsTrue(m.Acquire(kP11RegionBlockStats, 5u, AcquireForm::Cpu).Ok());
					Check(p11Calls == 2, "P11(4) and P11(5) built once each");
					m.OnSeal(6u);
					const AcquireResult kept4 = m.Acquire(kP11RegionBlockStats, 4u, AcquireForm::Cpu);
					const AcquireResult r = m.Acquire(kFocusMap, 6u, AcquireForm::Cpu);
					Assert::IsTrue(r.Ok());
					if (keep == 2)
					{
						Check(kept4.status == AcquireStatus::Reused, "keepFrames 2: lag-2 frame 4 survives OnSeal(6)");
						Check(p11Calls == 2, "P11(4) and P11(5) reused without recompute");
						Check(CountPacked(r, g.Blocks(), kNoPrevious) == 0 && CountPacked(r, g.Blocks(), kNoPreviousRetained) == 0, "the focus change has its lag-2 reference");
						Check(cfg.stats->noPreviousRetained == 0 && !cfg.stats->lastNoPreviousRetained, "nothing released");
					}
					else
					{
						Check(!kept4.Ok() && kept4.cause == AcquireCause::FrameReleased, "keepFrames 1: lag-2 frame 4 is FrameReleased at OnSeal(6)");
						Check(p11Calls == 2, "P11(4) is never recomputed after release");
						Check(CountPacked(r, g.Blocks(), kNoPrevious) == static_cast<int>(g.Blocks())
							&& CountPacked(r, g.Blocks(), kNoPreviousRetained) == static_cast<int>(g.Blocks()), "every block says why it has no lag-2 reference");
						Check(cfg.stats->noPreviousRetained == 1 && cfg.stats->lastNoPreviousRetained, "the released reference is counted");
						const FallbackCatalog::Decision d = StatusDecision(cfg.stats.get(), true);
						Check(d.engaged && d.id == "product.focus_map" && Contains(d.reason, "released"), "visible on the Fallbacks line: " + d.reason);
					}
				}
				// A refused registration is never silent (audit SHOULD 5).
				Check(StatusDecision(nullptr, false, "already registered").engaged, "an unregistered fill engages the line");
			}

			// WAIVER-FOCUS-EDGEMAP-ONLY: benchmark updated for v2 edge-map-only law (isolated ComputeGrid with EdgeInput
			// and live Acquire with kEdgeMap + P11 + P2 at lag >= 1). Measured, never time-gated.
			TEST_METHOD(BenchCpuFormAt4k)
			{
				using namespace VpSkeleton;
				using Clock = std::chrono::steady_clock;
				const Geometry g = Geometry::Make(3840, 2160);
				const std::vector<float> scene = ConfidentP11Scene(g);
				const std::vector<uint8_t> edgeA = EdgeScene(g, [](int bx, int by) { return ((bx / 6) + (by / 6)) % 2 == 0; });
				const std::vector<uint8_t> edgeB = EdgeScene(g, [](int bx, int by) { return ((bx / 6) + (by / 6)) % 3 != 0; });
				const EdgeSumsHolder sumA = MakeEdgeSums(g, edgeA);
				const EdgeSumsHolder sumB = MakeEdgeSums(g, edgeB);
				const std::vector<uint8_t> field = Field(g, 3.0, -2.0);
				std::vector<Block> out;
				const Params p;
				const int reps = 20;
				ComputeGrid(p, g, scene.data(), scene.data(), field.data(), nullptr, false, out, sumA.View(), sumB.View());   // warm
				const auto t0 = Clock::now();
				for (int i = 0; i < reps; ++i)
					ComputeGrid(p, g, scene.data(), scene.data(), field.data(), nullptr, false, out, sumA.View(), sumB.View());
				const double isolatedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / reps;

				SkeletonMemo m;
				const std::vector<uint8_t> b11 = FloatBytes(scene);
				OwnerFill fe;
				fe.keepFrames = 2;
				fe.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = (r.seq & 1u) ? edgeA : edgeB; return true; };
				OwnerFill f11;
				f11.keepFrames = 2;
				f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = b11; return true; };
				OwnerFill f2;
				f2.keepFrames = 2;
				f2.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = field; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", fe) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP2VectorPair, AcquireForm::Cpu, "motion/estimation", f2) == SkeletonMemo::Registration::Registered);
				FillConfig cfg;
				cfg.geometry = [](uint64_t, int& w, int& h) { w = 3840; h = 2160; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);
				double liveMs = 0.0;
				int ok = 0;
				for (int i = 0; i < reps; ++i)
				{
					const uint64_t seq = 10u + static_cast<uint64_t>(i);
					m.OnSeal(seq);
					const auto a0 = Clock::now();
					ok += m.Acquire(kFocusMap, seq, AcquireForm::Cpu).Ok() ? 1 : 0;
					liveMs += std::chrono::duration<double, std::milli>(Clock::now() - a0).count();
				}
				Check(ok == reps, "every live frame was built");
				char msg[256];
				std::snprintf(msg, sizeof(msg), "BENCH focus_map v2 edge-law CPU 3840x2160 (%dx%d blocks): isolated ComputeGrid %.3f ms/frame; live Acquire fill %.3f ms/frame\n",
					g.gw, g.gh, isolatedMs, liveMs / reps);
				Logger::WriteMessage(msg);
			}

			// Pass A HIGH-2 regression: when kEdgeMap(seq-2) and kEdgeMap(seq-1) are BOTH Ready in EdgeMapCpuReadback /
			// ReadbackRing (NOT yet cached in SkeletonMemo), Acquire(kFocusMap, seq, Cpu) retrieves BOTH seq-2 and seq-1
			// without stale-dropping seq-2. Same-input negative control proves pre-fix keepOlder=0 taking seq-1 first drops seq-2.
			TEST_METHOD(AcquireRetrievesLagTwoAndLagOneFromReadbackRingWithoutStaleDrop)
			{
				using namespace VpSkeleton;
				using namespace Vpv::Regions;

				struct VramResetScope
				{
					VramResetScope() { VramUsers::Instance().ResetForTesting(); }
					~VramResetScope() { VramUsers::Instance().ResetForTesting(); }
				} vramScope;

				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> p11 = ConfidentP11Scene(g);
				const std::vector<uint8_t> crispBytes = EdgeScene(g, [](int, int) { return true; });
				const std::vector<uint8_t> softLeftBytes = EdgeScene(g, [](int bx, int) { return bx >= 8; });

				class FakeEdgeReadbackDevice : public IReadbackDevice
				{
				public:
					std::vector<char> done;
					std::vector<std::vector<uint8_t>> payloads;
					std::vector<uint64_t> slotSeq;
					std::vector<uint64_t> mappedOrder;
					const std::vector<uint8_t>* crisp = nullptr;
					const std::vector<uint8_t>* softLeft = nullptr;
					int discards = 0;

					FakeEdgeReadbackDevice(size_t slots, const std::vector<uint8_t>* c, const std::vector<uint8_t>* s)
						: done(slots, 0), payloads(slots), slotSeq(slots, 0), crisp(c), softLeft(s) {}

					bool CopyToStaging(ProductId id, uint64_t seq, int slot) override
					{
						if (id != kEdgeMap || slot < 0 || size_t(slot) >= payloads.size())
							return false;
						done[size_t(slot)] = 0;
						slotSeq[size_t(slot)] = seq;
						payloads[size_t(slot)] = (seq == 4u) ? *softLeft : *crisp;
						return true;
					}
					bool QueryDone(int slot) override
					{
						return slot >= 0 && size_t(slot) < done.size() && done[size_t(slot)] != 0;
					}
					void Discard(int slot) override
					{
						++discards;
						if (slot >= 0 && size_t(slot) < done.size())
							done[size_t(slot)] = 0;
					}
					const void* MapNoWait(int slot, size_t* bytes) override
					{
						if (slot < 0 || size_t(slot) >= payloads.size())
							return nullptr;
						mappedOrder.push_back(slotSeq[size_t(slot)]);
						*bytes = payloads[size_t(slot)].size();
						return payloads[size_t(slot)].data();
					}
					void Unmap(int) override {}
				};

				// Mandatory same-input negative control: naive ReadbackRing with pre-fix keepOlder=0 taking seq-1 (4)
				// BEFORE seq-2 (3) stale-drops seq-2 so the lag-2 readback is permanently lost.
				{
					FakeEdgeReadbackDevice naiveDev(kEdgeMapReadbackSlots, &crispBytes, &softLeftBytes);
					VpLaneProducts::ReadbackRing naiveRing(&naiveDev, kEdgeMapReadbackSlots);
					Assert::IsTrue(naiveRing.Submit(kEdgeMap, 3u));
					Assert::IsTrue(naiveRing.Submit(kEdgeMap, 4u));
					naiveDev.done[0] = 1;
					naiveDev.done[1] = 1;
					naiveRing.Poll();
					Assert::IsTrue(naiveRing.HasReady(3u) && naiveRing.HasReady(4u));

					size_t got4 = 0, got3 = 0;
					Assert::IsTrue(naiveRing.Take(4u, [&](const void*, size_t n) { got4 = n; }, 0u),
						L"negative control: naive Take(4, keepOlder=0) succeeds");
					Assert::AreEqual(softLeftBytes.size(), got4);
					Assert::AreEqual(uint64_t(1), naiveRing.Counters().staleDropped,
						L"negative control: naive Take(4, keepOlder=0) stale-drops Ready frame 3");
					Assert::IsFalse(naiveRing.Take(3u, [&](const void*, size_t n) { got3 = n; }, 0u),
						L"negative control: frame 3 is lost after naive Take(4, keepOlder=0)");
					Assert::AreEqual(size_t(0), got3);
				}

				// Production path: EdgeMapCpuReadback + FocusMapFill through SkeletonMemo when BOTH seq 3 and seq 4
				// are Ready in the ring and NOT yet cached in SkeletonMemo.
				FakeEdgeReadbackDevice dev(kEdgeMapReadbackSlots, &crispBytes, &softLeftBytes);
				EdgeMapCpuReadback rb;
				rb.AttachExternalDevice(&dev, kEdgeMapReadbackSlots, 0u);
				rb.SetDemand(true);

				Assert::IsTrue(rb.ScheduleExternal(3u));
				dev.done[0] = 1;
				Assert::IsTrue(rb.ScheduleExternal(4u));
				dev.done[1] = 1;
				rb.Poll();
				Assert::IsTrue(rb.HasReady(3u) && rb.HasReady(4u),
					L"precondition: both lag-2 (3) and lag-1 (4) are Ready in ReadbackRing");

				SkeletonMemo m;
				m.SetRenderThread(std::this_thread::get_id());
				OwnerFill f11;
				f11.keepFrames = 2;
				f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) {
					o.bytes = FloatBytes(p11);
					return true;
				};
				Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, kEdgeMapUnit, MakeEdgeMapCpuOwnerFill(&rb))
					== SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11)
					== SkeletonMemo::Registration::Registered);
				FillConfig cfg;
				cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg))
					== SkeletonMemo::Registration::Registered);

				Assert::AreEqual(uint64_t(0), m.CountersOf(kEdgeMap).ownerFillCalls,
					L"precondition: neither kEdgeMap(3) nor kEdgeMap(4) is cached in SkeletonMemo yet");

				const AcquireResult r5 = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
				Assert::IsTrue(r5.Ok() && r5.status == AcquireStatus::FilledAtRead,
					L"Acquire(kFocusMap, 5) succeeds when both lag-2 (3) and lag-1 (4) start uncached in ReadbackRing");
				Assert::AreEqual(0, CountPacked(r5, g.Blocks(), kNoPrevious),
					L"lag-2 kEdgeMap(3) was NOT stale-dropped: zero kNoPrevious blocks");
				Assert::AreEqual(8 * 12, CountPacked(r5, g.Blocks(), kChanged),
					L"rack-focus delta between seq 3 (crisp) and seq 4 (soft left) flags exact 8x12 left half");
				Assert::AreEqual(uint64_t(2), rb.Counters().taken, L"both seq 3 and seq 4 taken from ReadbackRing");
				Assert::AreEqual(uint64_t(0), rb.Counters().staleDropped, L"zero stale drops");
				Assert::AreEqual(size_t(2), dev.mappedOrder.size(), L"exactly two ring mappings");
				Assert::AreEqual(uint64_t(3), dev.mappedOrder[0], L"FocusMapFill acquires lag-2 (3) before lag-1 (4)");
				Assert::AreEqual(uint64_t(4), dev.mappedOrder[1], L"FocusMapFill acquires lag-1 (4) second");

				rb.SetDemand(false);
			}

			// W23-Fix2 HIGH-2 regression:
			// (1) EdgeSums clamps strictly to Geometry active picture rect (letterbox bar border causes zero bleed on straddling blocks;
			//     same-input negative control without active rect falsely reads sharp on the straddling row).
			// (2) FieldMotion scales S10.5 vectors using p2Cell / p2CellY and anisotropic p2ScaleX / p2ScaleY (no hardcoded / 16.0).
			// (3) FocusMapFill validates lag-1 (req.seq - 1) using sampleSeq's geometry (survives resolution switch at req.seq) and
			//     checks lag-2 ReadbackPending BEFORE acquiring P11 or running EdgeSums.
			TEST_METHOD(ActiveRectBarBleedAnisotropicP2CellLag1GeometryAndPendingOrder)
			{
				using namespace VpSkeleton;

				// 1. Active-rect bar-bleed negative control on 128x96 (block = 8; row by=1 covers y=8..15).
				const Geometry gNoRect = Geometry::Make(kW, kH);
				Geometry gActive = Geometry::Make(kW, kH);
				gActive.SetActiveRect(0, 12, kW, kH - 12);   // rows 0..11 and 84..95 are letterbox bars

				const std::vector<uint8_t> barEdge = EdgeField([](int, int y) {
					return (y < 12 || y >= kH - 12) ? 200.0f : 5.0f;   // bright bar border + defocused picture
				});
				const std::vector<float> p11 = ConfidentP11Scene(gNoRect);
				const EdgeSumsHolder naiveSums = MakeEdgeSums(gNoRect, barEdge);
				const EdgeSumsHolder clampedSums = MakeEdgeSums(gActive, barEdge);

				std::vector<Block> naiveOut, clampedOut;
				ComputeGrid(Params(), gNoRect, p11.data(), nullptr, nullptr, nullptr, false, naiveOut, naiveSums.View());
				ComputeGrid(Params(), gActive, p11.data(), nullptr, nullptr, nullptr, false, clampedOut, clampedSums.View());

				// Block (bx=4, by=1) straddles y=8..11 (bar) and y=12..15 (defocused picture).
				const size_t straddleIdx = static_cast<size_t>(1) * gNoRect.gw + 4u;
				Assert::IsTrue(naiveOut[straddleIdx].defocus < 0.1f,
					L"negative control: without active rect, bright bar edge makes straddling block falsely read sharp");
				Assert::IsTrue(clampedOut[straddleIdx].defocus > 0.9f,
					L"active-rect clamp excludes letterbox bar pixels so straddling block accurately reads defocused");

				// 2. P2CellOf scaling ({4, 8, 16, 32}) + anisotropic A-lens (p2ScaleY != p2ScaleX).
				auto makeUniformP2 = [](const Geometry& g, int16_t rawX, int16_t rawY) {
					std::vector<uint8_t> buf(static_cast<size_t>(g.p2gw) * static_cast<size_t>(g.p2gh) * kP2BytesPerCell, 0u);
					for (size_t i = 0; i < static_cast<size_t>(g.p2gw) * static_cast<size_t>(g.p2gh); ++i)
					{
						std::memcpy(&buf[i * kP2BytesPerCell + 4], &rawX, 2);
						std::memcpy(&buf[i * kP2BytesPerCell + 6], &rawY, 2);
					}
					return buf;
				};
				for (int cell : { 4, 8, 16, 32 })
				{
					const Geometry gc = Geometry::Make(kW, kH, cell);
					const std::vector<uint8_t> p2 = makeUniformP2(gc, 128, -64);   // 4.0 and -2.0 engine px
					const BlockMotion m = FieldMotion(gc, p2.data(), 2, 2);
					const double expectedDx = 4.0 * (static_cast<double>(cell) / 4.0);
					const double expectedDy = -2.0 * (static_cast<double>(cell) / 4.0);
					Assert::AreEqual(expectedDx, static_cast<double>(m.dx), 1e-4, L"FieldMotion dx scales with p2Cell");
					Assert::AreEqual(expectedDy, static_cast<double>(m.dy), 1e-4, L"FieldMotion dy scales with p2Cell");
				}
				{
					// Anisotropic A-lens: horizontal 2.0 source px / engine px, vertical 2.5 source px / engine px.
					const Geometry gAniso = Geometry::Make(kW, kH, 8, 10, 2.0, 2.5);
					const std::vector<uint8_t> p2 = makeUniformP2(gAniso, 64, 64);   // 2.0 engine px on both axes
					const BlockMotion m = FieldMotion(gAniso, p2.data(), 2, 2);
					Assert::AreEqual(4.0f, m.dx, 1e-4f, L"anisotropic FieldMotion dx = 2.0 * 2.0 = 4.0 source px");
					Assert::AreEqual(5.0f, m.dy, 1e-4f, L"anisotropic FieldMotion dy = 2.0 * 2.5 = 5.0 source px");
					Assert::IsTrue(std::fabs(m.dy - m.dx) > 0.5f, L"anisotropic scaleY is NOT collapsed into isotropic / 16.0");
				}

				// 3. Lag-1 geometry validation (resolution switch at req.seq == 5) & lag-2 ReadbackPending BEFORE P11/EdgeSums.
				{
					SkeletonMemo m;
					std::atomic<int> p11Calls{ 0 };
					std::atomic<bool> lag2Pending{ true };

					OwnerFill fe;
					fe.keepFrames = 2;
					fe.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) {
						if (r.seq == 3u && lag2Pending.load())
						{
							o.pending = true;
							o.declineReason = "lag-2 pending";
							return false;
						}
						o.bytes = barEdge;
						return true;
					};
					OwnerFill f11;
					f11.keepFrames = 2;
					f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) {
						++p11Calls;
						o.bytes = FloatBytes(p11);
						return true;
					};
					Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", fe) == SkeletonMemo::Registration::Registered);
					Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);

					FillConfig cfg;
					// Frame 5 switched to 192x144 while lag-1 (frame 4) and lag-2 (frame 3) are still 128x96!
					cfg.geometry = [](uint64_t seq, int& w, int& h) {
						if (seq >= 5u) { w = 192; h = 144; return true; }
						w = kW; h = kH; return true;
					};
					cfg.activeRect = [](uint64_t, int& x0, int& y0, int& x1, int& y1) {
						x0 = 0; y0 = 12; x1 = kW; y1 = kH - 12; return true;
					};
					Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);

					// First attempt: lag-2 (seq 3) is pending -> MUST return ReadbackPending BEFORE touching P11!
					const AcquireResult rPending = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
					Assert::IsFalse(rPending.Ok(), L"pending lag-2 declines non-latched");
					Assert::IsTrue(rPending.cause == AcquireCause::ReadbackPending, L"propagates ReadbackPending");
					Assert::AreEqual(0, p11Calls.load(), L"ReadbackPending checked BEFORE acquiring P11 or running EdgeSums");

					// Second attempt: lag-2 now ready -> succeeds using lag-1 (seq 4) geometry 128x96 (16x12 blocks), NOT seq 5's 192x144!
					lag2Pending.store(false);
					const AcquireResult rOk = m.Acquire(kFocusMap, 5u, AcquireForm::Cpu);
					Assert::IsTrue(rOk.Ok(), L"Acquire(kFocusMap, 5) succeeds using sampleSeq=4 geometry after seq-5 resize");
					Assert::AreEqual(gActive.Blocks() * kBytesPerBlock, rOk.cpuBytes, L"output grid matches sampleSeq=4 (128x96) block count");
					Assert::IsTrue(Unpack(rOk.cpu + straddleIdx * kBytesPerBlock).defocus > 0.9f,
						L"FocusMapFill activeRect callback prevents bar bleed on straddling block");
				}
			}
	};
}
