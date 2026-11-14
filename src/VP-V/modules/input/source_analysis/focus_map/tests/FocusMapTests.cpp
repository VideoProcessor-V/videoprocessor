// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/focus_map (job #1 L-FOCUS): the CPU reference (defocus ramp, rack-focus clip, change along the motion, the
// camera stand-in, the visible fallbacks, the packed form) and the Acquire single-build proof of the owner fill. CPU only.
#include "pch.h"
#include <VP-V/modules/input/source_analysis/focus_map/FocusMapFill.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		using namespace FocusMap;
		using RegionEvidence::kPrecursorChannels;
		using RegionEvidence::kCoverage;
		using RegionEvidence::kE1;
		using RegionEvidence::kE2;
		using RegionEvidence::kE3;
		using RegionEvidence::kStatN;

		constexpr int kW = 128, kH = 96;   // block 8: a 16 x 12 grid; the P2 cell is 8 px too

		void Check(bool ok, const std::string& what)
		{
			if (ok) return;
			Logger::WriteMessage((what + "\n").c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		// A P11 block: sharp (fine band holds the energy) or defocused (the energy sits in the coarse band), N = 2.
		void Sharp(float* b) { b[kCoverage] = 1; b[kStatN] = 2; b[kE1] = 42; b[kE2] = 10; b[kE3] = 3; }
		void Blur(float* b) { b[kCoverage] = 1; b[kStatN] = 2; b[kE1] = 3; b[kE2] = 10; b[kE3] = 40; }

		std::vector<float> Scene(const Geometry& g, const std::function<bool(int, int)>& sharp)
		{
			std::vector<float> s(g.Blocks() * kPrecursorChannels, 0.0f);
			for (int by = 0; by < g.gh; ++by)
				for (int bx = 0; bx < g.gw; ++bx)
				{
					float* b = &s[(static_cast<size_t>(by) * g.gw + bx) * kPrecursorChannels];
					if (sharp(bx, by)) Sharp(b); else Blur(b);
				}
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

		TEST_CLASS(FocusMapTests)
		{
		public:
			// Synthetic defocus ramp: fine-band energy rising across a row: the defocus level falls monotonically from 1 to 0.
			TEST_METHOD(DefocusRampIsMonotone)
			{
				const Params p;
				float prev = 2.0f;
				float first = -1, last = -1;
				for (int i = 0; i <= 60; ++i)
				{
					float b[kPrecursorChannels] = {};
					b[kCoverage] = 1; b[kStatN] = 2; b[kE1] = 2.0f + i; b[kE2] = 10; b[kE3] = 10;
					float d, c;
					uint32_t f;
					DefocusBlock<float>(p, b, d, c, f);
					Check(f & kValid, "a covered block is valid");
					Check(d <= prev + 1e-6f, "defocus must not rise as the fine band gains energy (step " + std::to_string(i) + ")");
					prev = d;
					if (i == 0) first = d;
					last = d;
				}
				Check(first > 0.99f && last < 0.01f, "the ramp spans defocused (" + std::to_string(first) + ") to sharp (" + std::to_string(last) + ")");
				float off[kPrecursorChannels] = {};   // coverage 0: no evidence, never valid, never flagged
				float d, c;
				uint32_t f;
				DefocusBlock<float>(p, off, d, c, f);
				Check(!(f & kValid) && c == 0.0f, "an uncovered block carries no evidence");
				float flat[kPrecursorChannels] = {};   // flat sky: tiny energy -> confidence 0, so it can never flag a change
				flat[kCoverage] = 1; flat[kStatN] = 0.1f; flat[kE1] = 0.2f; flat[kE2] = 0.1f; flat[kE3] = 0.1f;
				DefocusBlock<float>(p, flat, d, c, f);
				Check(c < 0.05f, "a flat block has no focus confidence");
			}

			// Rack focus: the left half goes from sharp to defocused (and back). Exactly those blocks are flagged; the right half is not.
			TEST_METHOD(RackFocusFlagsTheChangedRegion)
			{
				const Geometry g = Geometry::Make(kW, kH);
				Check(g.gw == 16 && g.gh == 12 && g.block == 8, "test geometry");
				const std::vector<float> a = Scene(g, [](int, int) { return true; });
				const std::vector<float> b = Scene(g, [](int bx, int) { return bx >= 8; });
				std::vector<Block> out;
				ComputeGrid(Params(), g, b.data(), a.data(), nullptr, nullptr, false, out);
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
				ComputeGrid(Params(), g, a.data(), b.data(), nullptr, nullptr, false, out);   // rack back: the region gets sharper
				Check(Count(out, kChanged) == 8 * 12 && Count(out, kSharper) == 8 * 12, "the reverse rack flags sharper");
				ComputeGrid(Params(), g, a.data(), nullptr, nullptr, nullptr, false, out);    // no previous frame: visible, no flag
				Check(Count(out, kNoPrevious) == static_cast<int>(g.Blocks()) && Count(out, kChanged) == 0, "no previous frame: no change flag");
				ComputeGrid(Params(), g, b.data(), a.data(), nullptr, nullptr, true, out);    // P10 unavailable: kNoSplit on every block
				Check(Count(out, kNoSplit) == static_cast<int>(g.Blocks()), "no split is visible on every block");
			}

			// The same sharp object panned by two blocks: with the P2 vectors (or the P7 camera) nothing changed; without any motion the
			// object's old and new positions look like focus changes (that is the defect the motion prevents).
			TEST_METHOD(ChangeFollowsTheMotion)
			{
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> a = Scene(g, [](int bx, int) { return bx >= 4 && bx < 6; });
				const std::vector<float> b = Scene(g, [](int bx, int) { return bx >= 6 && bx < 8; });
				std::vector<Block> out;
				const std::vector<uint8_t> p2 = Field(g, -16.0, 0.0);   // the content was 16 px to the left in the previous frame
				ComputeGrid(Params(), g, b.data(), a.data(), p2.data(), nullptr, false, out);
				Check(Count(out, kChanged) == 0, "with the P2 vectors the pan is not a focus change: " + std::to_string(Count(out, kChanged)));
				Check(Count(out, kNoMotion) == 0 && Count(out, kCameraMotion) == 0, "the field was used");
				Check(Count(out, kNoPrevious) == 2 * 12, "two edge columns look outside the previous frame: visible, not flagged");
				const float p7[4] = { 16.0f, 0.0f, 1.0f, 1.0f };   // the camera moved the content 16 px right
				ComputeGrid(Params(), g, b.data(), a.data(), nullptr, p7, false, out);
				Check(Count(out, kChanged) == 0, "the P7 stand-in gives the same answer");
				Check(Count(out, kCameraMotion) == static_cast<int>(g.Blocks()), "the camera stand-in is visible on every block");
				ComputeGrid(Params(), g, b.data(), a.data(), nullptr, nullptr, false, out);
				Check(Count(out, kChanged) == 4 * 12, "without motion the object's old and new places flag: " + std::to_string(Count(out, kChanged)));
				const float p7bad[4] = { 16.0f, 0.0f, 0.0f, 0.0f };   // an invalid camera fit is not used
				ComputeGrid(Params(), g, b.data(), a.data(), nullptr, p7bad, false, out);
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

			// The owner fill reads every input through Acquire: each input is built ONCE however many readers there are (frame and frame - 1
			// share one P11 build), a second read reuses the published map, and an unavailable P2 / P7 degrades visibly.
			TEST_METHOD(AcquireBuildsEveryInputOnce)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				SkeletonMemo m;
				std::atomic<int> p11Calls{ 0 }, p10Calls{ 0 }, p2Calls{ 0 }, p7Calls{ 0 };
				auto bytesOf = [](const std::vector<float>& s) {
					std::vector<uint8_t> v(s.size() * sizeof(float));
					std::memcpy(v.data(), s.data(), v.size());
					return v;
				};
				// Frame 5: the left half defocused. Every other frame: sharp.
				OwnerFill f11;
				f11.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) {
					++p11Calls;
					o.bytes = bytesOf(Scene(g, [&](int bx, int) { return r.seq != 5 || bx >= 8; }));
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
				Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, "repair/grain_split", f10) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP2VectorPair, AcquireForm::Cpu, "motion/estimation", f2) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP7CameraFit, AcquireForm::Cpu, "motion/estimation/global_model", f7) == SkeletonMemo::Registration::Registered);
				FillConfig cfg;
				cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered,
					L"the focus owner is the catalog's owner of kFocusMap");

				const AcquireResult a = m.Acquire(kFocusMap, 5, AcquireForm::Cpu);
				Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && a.cpuBytes == g.Blocks() * kBytesPerBlock, L"the map is built and published");
				const AcquireResult b = m.Acquire(kFocusMap, 5, AcquireForm::Cpu);
				Assert::IsTrue(b.status == AcquireStatus::Reused && a.cpu == b.cpu, L"the second reader reuses the one map");
				Check(p11Calls == 2 && p10Calls == 1 && p2Calls == 1 && p7Calls == 1,
					"every input built once: p11 " + std::to_string(p11Calls.load()) + " (frame + previous), p10 " + std::to_string(p10Calls.load()) + ", p2 "
					+ std::to_string(p2Calls.load()) + ", p7 " + std::to_string(p7Calls.load()));
				Assert::IsTrue(m.CountersOf(kFocusMap).ownerFillCalls == 1 && m.CountersOf(kFocusMap).filledAtRead == 1 && m.CountersOf(kFocusMap).reusedMemo == 1);
				// What the map says: the left half changed (static camera stand-in: kCameraMotion visible on every block).
				int changed = 0, camera = 0, noMotion = 0;
				for (size_t i = 0; i < g.Blocks(); ++i)
				{
					const Block x = Unpack(a.cpu + i * kBytesPerBlock);
					changed += (x.flags & kChanged) ? 1 : 0;
					camera += (x.flags & kCameraMotion) ? 1 : 0;
					noMotion += (x.flags & kNoMotion) ? 1 : 0;
				}
				Check(changed == 8 * 12 && camera == static_cast<int>(g.Blocks()) && noMotion == 0, "left half flagged through the P7 stand-in");
				// The next frame reuses frame 5's P11 (its previous frame): one more P11 build, none for 5.
				const AcquireResult c = m.Acquire(kFocusMap, 6, AcquireForm::Cpu);
				Assert::IsTrue(c.Ok());
				Check(p11Calls == 3 && p10Calls == 2 && p2Calls == 2 && p7Calls == 2, "the next frame builds only its own inputs: p11 " + std::to_string(p11Calls.load()));
				int sharper = 0;
				for (size_t i = 0; i < g.Blocks(); ++i) sharper += (Unpack(c.cpu + i * kBytesPerBlock).flags & kSharper) ? 1 : 0;
				Check(sharper == 8 * 12, "the focus returning is flagged sharper");
				Check(cfg.stats->fills == 2, "the fill ran once per frame");
			}

			// Measured, never asserted (no target): the CPU form at 4K (a 120 x 68 block grid), isolated (ComputeGrid) and live (the owner
			// fill through Acquire with the P11 / P10 / P2 inputs published by trivial owners, frame after frame).
			TEST_METHOD(BenchCpuFormAt4k)
			{
				using namespace VpSkeleton;
				using Clock = std::chrono::steady_clock;
				const Geometry g = Geometry::Make(3840, 2160);
				const std::vector<float> scene = Scene(g, [](int bx, int by) { return ((bx / 6) + (by / 6)) % 2 == 0; });
				const std::vector<float> prev = Scene(g, [](int bx, int by) { return ((bx / 6) + (by / 6)) % 3 != 0; });
				const std::vector<uint8_t> field = Field(g, 3.0, -2.0);
				std::vector<Block> out;
				const Params p;
				const int reps = 200;
				ComputeGrid(p, g, scene.data(), prev.data(), field.data(), nullptr, false, out);   // warm
				const auto t0 = Clock::now();
				for (int i = 0; i < reps; ++i) ComputeGrid(p, g, scene.data(), prev.data(), field.data(), nullptr, false, out);
				const double isolatedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / reps;

				SkeletonMemo m;
				auto bytesOf = [](const std::vector<float>& v) {
					std::vector<uint8_t> b(v.size() * sizeof(float));
					std::memcpy(b.data(), v.data(), b.size());
					return b;
				};
				const std::vector<uint8_t> b11a = bytesOf(scene), b11b = bytesOf(prev);
				OwnerFill f11;
				f11.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = (r.seq & 1) ? b11a : b11b; return true; };
				OwnerFill f10;
				f10.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes.assign(64, 1); return true; };
				OwnerFill f2;
				f2.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = field; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, "repair/grain_split", f10) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kP2VectorPair, AcquireForm::Cpu, "motion/estimation", f2) == SkeletonMemo::Registration::Registered);
				FillConfig cfg;
				cfg.geometry = [](uint64_t, int& w, int& h) { w = 3840; h = 2160; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);
				double liveMs = 0.0;
				int ok = 0;
				for (int i = 0; i < reps; ++i)
				{
					const auto a0 = Clock::now();
					ok += m.Acquire(kFocusMap, 10 + static_cast<uint64_t>(i), AcquireForm::Cpu).Ok() ? 1 : 0;
					liveMs += std::chrono::duration<double, std::milli>(Clock::now() - a0).count();
				}
				Check(ok == reps, "every live frame was built");
				char msg[256];
				std::snprintf(msg, sizeof(msg), "BENCH focus_map CPU form 3840x2160 (%dx%d blocks): isolated ComputeGrid %.3f ms/frame; live Acquire fill %.3f ms/frame (incl. fake-input copies)
",
					g.gw, g.gh, isolatedMs, liveMs / reps);
				Logger::WriteMessage(msg);
			}
		};
	}
}
