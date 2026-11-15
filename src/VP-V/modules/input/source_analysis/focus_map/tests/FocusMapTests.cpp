// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/focus_map (job #1 L-FOCUS): the CPU reference (defocus ramp, rack-focus clip, change along the motion, the
// camera stand-in, the visible fallbacks, the packed form) and the Acquire single-build proof of the owner fill; the registered fix
// FOCUS-EDGEMAP-SWITCH (the interim law bit for bit without an edge map, the edge law on a Sobel field, the edge map driving the change
// through Acquire, one law per frame pair) and the lag-1 P11 retention (kept spot vs visibly released). CPU only.
#include "pch.h"
#include <VP-V/modules/input/source_analysis/focus_map/FocusMapFill.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdge.h>

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

		// An edge-map CPU form (kW x kH float32 |G| codes): `g(x, y)` per pixel.
		std::vector<uint8_t> EdgeField(const std::function<float(int, int)>& g)
		{
			std::vector<float> e(static_cast<size_t>(kW) * kH);
			for (int y = 0; y < kH; ++y)
				for (int x = 0; x < kW; ++x) e[static_cast<size_t>(y) * kW + x] = g(x, y);
			return FloatBytes(e);
		}

		bool Contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

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
				// WAIVER-JOB1FOCUS-C3 (audit SHOULD 4): P10 is a declared edge only, never Acquired (P11's N = -1 carries "no split"), so its
				// full CPU form is not forced: p10 0 (was 1). Every Acquired input is still built exactly once.
				Check(p11Calls == 2 && p10Calls == 0 && p2Calls == 1 && p7Calls == 1,
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
				Check(p11Calls == 3 && p10Calls == 0 && p2Calls == 2 && p7Calls == 2, "the next frame builds only its own inputs: p11 " + std::to_string(p11Calls.load()));   // WAIVER-JOB1FOCUS-C3: p10 0 (was 2), SHOULD 4
				int sharper = 0;
				for (size_t i = 0; i < g.Blocks(); ++i) sharper += (Unpack(c.cpu + i * kBytesPerBlock).flags & kSharper) ? 1 : 0;
				Check(sharper == 8 * 12, "the focus returning is flagged sharper");
				Check(cfg.stats->fills == 2, "the fill ran once per frame");
			}

			// FOCUS-EDGEMAP-SWITCH equivalence: with NO kEdgeMap CPU form (the C3 state: GPU-only owner fill), the fill's map is the v1
			// law bit for bit (defocus, delta, confidence bytes identical to ComputeGrid without an edge input) and every block adds only
			// kSharpnessInterim; the interim is counted and on the product.focus_map Fallbacks line with the edge map's cause.
			TEST_METHOD(WithoutAnEdgeMapTheFillIsTheInterimLawBitForBit)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> a = Scene(g, [](int, int) { return true; });
				const std::vector<float> b = Scene(g, [](int bx, int) { return bx >= 8; });
				SkeletonMemo m;
				OwnerFill f11;
				f11.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = FloatBytes(r.seq == 5 ? b : a); return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
				FillConfig cfg;
				cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);
				Assert::IsFalse(m.HasOwnerFill(kEdgeMap, AcquireForm::Cpu), L"no edge-map CPU form (as on C3)");

				const AcquireResult r = m.Acquire(kFocusMap, 5, AcquireForm::Cpu);
				Assert::IsTrue(r.Ok() && r.cpuBytes == g.Blocks() * kBytesPerBlock);
				std::vector<Block> ref;
				ComputeGrid(Params(), g, b.data(), a.data(), nullptr, nullptr, false, ref);   // the v1 law, no edge input
				for (size_t i = 0; i < g.Blocks(); ++i)
				{
					uint8_t want[kBytesPerBlock];
					Pack(ref[i], want);
					Check(std::memcmp(want, r.cpu + i * kBytesPerBlock, 6) == 0, "block " + std::to_string(i) + ": defocus / delta / confidence are the v1 bytes");
					Check(Unpack(r.cpu + i * kBytesPerBlock).flags == static_cast<uint16_t>(ref[i].flags | kSharpnessInterim),
						"block " + std::to_string(i) + ": the v1 flags plus kSharpnessInterim only");
				}
				Check(CountPacked(r, g.Blocks(), kChanged) == 8 * 12, "the v1 rack focus is still flagged");
				Check(cfg.stats->sharpnessInterim == 1 && cfg.stats->edgeLaw == 0 && cfg.stats->lastSharpnessInterim, "the interim is counted");
				const FallbackCatalog::Decision d = StatusDecision(cfg.stats.get(), true);
				Check(d.engaged && Contains(d.reason, "sharpness: P11 interim, edge map pending") && Contains(d.reason, Token(AcquireCause::NoOwnerFill)),
					"visible on the Fallbacks line with the cause: " + d.reason);
				Check(m.CountersOf(kEdgeMap).filledAtRead == 0, "nothing computed the edge map privately");
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

			// The rack focus seen through the EDGE MAP: P11 is the same sharp scene in every frame (the interim law can see no change), the
			// edge map (an owner fill under the catalog's owner unit, Acquired by the focus fill) loses its gradient in the left half at frame
			// 5: exactly that half is flagged, under the edge law, and each edge-map frame is built once. Without the edge map the same
			// inputs flag nothing (the intended difference of FOCUS-EDGEMAP-SWITCH).
			TEST_METHOD(EdgeMapDrivesTheFocusChangeThroughAcquire)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> sharp = Scene(g, [](int, int) { return true; });
				const std::vector<uint8_t> crisp = EdgeField([](int, int) { return 60.0f; });
				const std::vector<uint8_t> soft = EdgeField([](int x, int) { return x < kW / 2 ? 5.0f : 60.0f; });
				for (int withEdge = 1; withEdge >= 0; --withEdge)
				{
					SkeletonMemo m;
					std::atomic<int> p11Calls{ 0 }, edgeCalls{ 0 };
					OwnerFill f11;
					f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { ++p11Calls; o.bytes = FloatBytes(sharp); return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
					if (withEdge)
					{
						OwnerFill fe;
						fe.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) { ++edgeCalls; o.bytes = r.seq == 5 ? soft : crisp; return true; };
						Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", fe) == SkeletonMemo::Registration::Registered,
							L"the edge map's CPU form is registered by its catalog owner unit");
					}
					FillConfig cfg;
					cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);
					Assert::IsTrue(m.Acquire(kFocusMap, 4, AcquireForm::Cpu).Ok());
					const AcquireResult r = m.Acquire(kFocusMap, 5, AcquireForm::Cpu);
					Assert::IsTrue(r.Ok());
					const int changed = CountPacked(r, g.Blocks(), kChanged), interim = CountPacked(r, g.Blocks(), kSharpnessInterim);
					if (withEdge)
					{
						Check(changed == 8 * 12 && CountPacked(r, g.Blocks(), kSharper) == 0, "the edge map's soft half is flagged: " + std::to_string(changed));
						for (size_t i = 0; i < g.Blocks(); ++i)
							Check(((Unpack(r.cpu + i * kBytesPerBlock).flags & kChanged) != 0) == (static_cast<int>(i % g.gw) < g.gw / 2), "changed exactly where the edge map softened");
						Check(interim == 0 && CountPacked(r, g.Blocks(), kNoPrevious) == 0, "the edge law on both frames");
						Check(edgeCalls == 3 && m.CountersOf(kEdgeMap).ownerFillCalls == 3, "edge map frames 3, 4, 5 each built once: " + std::to_string(edgeCalls.load()));
						Check(p11Calls == 3, "P11 frames 3, 4, 5 each built once");
						Check(cfg.stats->edgeLaw == 2 && cfg.stats->sharpnessInterim == 0, "the edge law is counted");
						Check(!StatusDecision(cfg.stats.get(), true).engaged, "served: the Fallbacks line is released");
					}
					else
					{
						Check(changed == 0 && interim == static_cast<int>(g.Blocks()), "without the edge map the same P11 shows no change (interim, visible)");
					}
				}
			}

			// One law per frame pair: the edge map is served for frame 5 but NOT for frame 4 (its owner declines). The previous frame is then
			// no reference (kNoPrevious on every block, not kNoPreviousRetained: it was not released), so a v2 level is never compared with a
			// v1 level (here that comparison would flag the soft half).
			TEST_METHOD(EdgeLawNeverComparesAcrossTwoLaws)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> sharp = Scene(g, [](int, int) { return true; });
				const std::vector<uint8_t> soft = EdgeField([](int x, int) { return x < kW / 2 ? 5.0f : 60.0f; });
				SkeletonMemo m;
				OwnerFill f11;
				f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { o.bytes = FloatBytes(sharp); return true; };
				OwnerFill fe;
				fe.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& o) { if (r.seq != 5) return false; o.bytes = soft; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
				Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, "input/source_analysis/regions", fe) == SkeletonMemo::Registration::Registered);
				FillConfig cfg;
				cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
				Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);
				const AcquireResult r = m.Acquire(kFocusMap, 5, AcquireForm::Cpu);
				Assert::IsTrue(r.Ok());
				Check(CountPacked(r, g.Blocks(), kNoPrevious) == static_cast<int>(g.Blocks()) && CountPacked(r, g.Blocks(), kChanged) == 0,
					"no reference under the edge law: kNoPrevious, never a mixed delta");
				Check(CountPacked(r, g.Blocks(), kNoPreviousRetained) == 0 && CountPacked(r, g.Blocks(), kSharpnessInterim) == 0, "edge law, nothing released");
				// The soft half reads defocused (the edge law ran on frame 5).
				int defocused = 0;
				for (size_t i = 0; i < g.Blocks(); ++i) defocused += Unpack(r.cpu + i * kBytesPerBlock).defocus > 0.9f ? 1 : 0;
				Check(defocused == 8 * 12, "the soft half is defocused under the edge law: " + std::to_string(defocused));
			}

			// MUST 3 (audit job1-focus): the lag-1 P11 read needs RETENTION. With the P11 owner fill declaring keepFrames 1, frame 4's P11 spot
			// survives the seal of frame 5 and the focus fill of frame 5 is served from it (no second P11 computation). With keepFrames 0 the
			// spot is released: the lag-1 read is FrameReleased, every block says kNoPrevious + kNoPreviousRetained, the cause is counted
			// and on the Fallbacks line, and P11(4) is never recomputed.
			TEST_METHOD(LagOneP11IsServedFromTheKeptSpotOrVisiblyReleased)
			{
				using namespace VpSkeleton;
				const Geometry g = Geometry::Make(kW, kH);
				const std::vector<float> sharp = Scene(g, [](int, int) { return true; });
				for (int keep = 1; keep >= 0; --keep)
				{
					SkeletonMemo m;
					std::atomic<int> p11Calls{ 0 };
					OwnerFill f11;
					f11.keepFrames = static_cast<uint8_t>(keep);
					f11.fill = [&](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) { ++p11Calls; o.bytes = FloatBytes(sharp); return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, "input/source_analysis/regions", f11) == SkeletonMemo::Registration::Registered);
					FillConfig cfg;
					cfg.geometry = [](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
					Assert::IsTrue(m.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, kUnitPath, MakeOwnerFill(cfg)) == SkeletonMemo::Registration::Registered);
					Assert::IsTrue(m.Acquire(kP11RegionBlockStats, 4, AcquireForm::Cpu).Ok());
					Check(p11Calls == 1, "P11(4) built once");
					m.OnSeal(5);
					const AcquireResult kept = m.Acquire(kP11RegionBlockStats, 4, AcquireForm::Cpu);
					const AcquireResult r = m.Acquire(kFocusMap, 5, AcquireForm::Cpu);
					Assert::IsTrue(r.Ok());
					if (keep)
					{
						Check(kept.status == AcquireStatus::Reused, "keepFrames 1: frame 4's spot is served after the seal of 5");
						Check(p11Calls == 2, "P11 4 and 5 built once each (the lag-1 read reuses the kept spot)");
						Check(CountPacked(r, g.Blocks(), kNoPrevious) == 0 && CountPacked(r, g.Blocks(), kNoPreviousRetained) == 0, "the focus change has its reference");
						Check(cfg.stats->noPreviousRetained == 0 && !cfg.stats->lastNoPreviousRetained, "nothing released");
					}
					else
					{
						Check(!kept.Ok() && kept.cause == AcquireCause::FrameReleased, "keepFrames 0: frame 4 is released at the seal of 5");
						Check(p11Calls == 2, "P11(4) is never recomputed (frame 5 is the only new build)");
						Check(CountPacked(r, g.Blocks(), kNoPrevious) == static_cast<int>(g.Blocks())
							&& CountPacked(r, g.Blocks(), kNoPreviousRetained) == static_cast<int>(g.Blocks()), "every block says why it has no reference");
						Check(cfg.stats->noPreviousRetained == 1 && cfg.stats->lastNoPreviousRetained, "the released reference is counted");
						const FallbackCatalog::Decision d = StatusDecision(cfg.stats.get(), true);
						Check(d.engaged && d.id == "product.focus_map" && Contains(d.reason, "released"), "visible on the Fallbacks line: " + d.reason);
					}
				}
				// A refused registration is never silent (audit SHOULD 5).
				Check(StatusDecision(nullptr, false, "already registered").engaged, "an unregistered fill engages the line");
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
				std::snprintf(msg, sizeof(msg), "BENCH focus_map CPU form 3840x2160 (%dx%d blocks): isolated ComputeGrid %.3f ms/frame; live Acquire fill %.3f ms/frame (incl. fake-input copies)\n",
					g.gw, g.gh, isolatedMs, liveMs / reps);
				Logger::WriteMessage(msg);
			}
		};
	}
}
