// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19, phase 2): the measurement plan, driven through the CPU reference matcher on
// synthetic luma frames (deterministic hash texture, pans, zoom, pasted objects, tiny movers, fades, flashes,
// cuts). CPU only. Frame geometry: 320x192 matcher px = 40x24 blocks of 8 matcher px = 16 source px at the
// default sourceToMatcher 0.5, so the record's grid and the matcher's grid coincide.
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelFit.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelMeasurement.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelSearchHints.h>
#include <VP-V/modules/motion/estimation/global_model/ReferenceMatcher.h>
#include "SyntheticCameraFields.h"
#include "CppUnitTest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace MeasTest
	{
		using namespace CameraModel;
		using namespace CameraModel::Reference;

		const int kW = 320, kH = 192, kGW = 40, kGH = 24;
		const size_t kN = static_cast<size_t>(kGW) * kGH;
		const double kNaN = std::numeric_limits<double>::quiet_NaN();

		inline void Req(bool ok, const std::string& what)
		{
			if (ok) return;
			Logger::WriteMessage(what.c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		inline void Say(const std::string& s) { Logger::WriteMessage(s.c_str()); }

		// ---- deterministic texture ----
		inline uint64_t Mix(uint64_t x)
		{
			uint64_t z = x + 0x9E3779B97F4A7C15ull;
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return z ^ (z >> 31);
		}
		inline float Hash(int x, int y, uint64_t seed)
		{
			const uint64_t h = Mix(seed * 0x2545F4914F6CDD1Dull + (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) + static_cast<uint32_t>(y));
			return static_cast<float>(static_cast<double>(h >> 40) * (1.0 / 16777216.0) * 2.0 - 1.0);
		}
		inline float Tex(int x, int y, uint64_t seed, float amp)
		{
			return 128.0f + amp * (0.5f * Hash(x, y, seed) + 0.125f * (Hash(x - 1, y, seed) + Hash(x + 1, y, seed) + Hash(x, y - 1, seed) + Hash(x, y + 1, seed)));
		}

		// Content moves by (vx, vy): A(p) = T(p), B(q) = T(q - v), so the matcher's forward vector is v.
		struct Scene
		{
			int w = kW, h = kH;
			uint64_t seed = 1;
			float amp = 60;
			std::vector<float> a, b;
			explicit Scene(float amplitude = 60, uint64_t s = 1) : seed(s), amp(amplitude) {}
			void Pan(int vx, int vy)
			{
				a.assign(static_cast<size_t>(w) * h, 0.0f);
				b = a;
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						a[static_cast<size_t>(y) * w + x] = Tex(x, y, seed, amp);
						b[static_cast<size_t>(y) * w + x] = Tex(x - vx, y - vy, seed, amp);
					}
			}
			// Zoom about the centre, one integer vector per 8x8 block (the approximation of a smooth field).
			void Zoom(double s)
			{
				a.assign(static_cast<size_t>(w) * h, 0.0f);
				b = a;
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						const int bx = x / 8, by = y / 8;
						const int vx = static_cast<int>(std::floor((s - 1.0) * ((bx + 0.5) * 8 - 0.5 * w) + 0.5));
						const int vy = static_cast<int>(std::floor((s - 1.0) * ((by + 0.5) * 8 - 0.5 * h) + 0.5));
						a[static_cast<size_t>(y) * w + x] = Tex(x, y, seed, amp);
						b[static_cast<size_t>(y) * w + x] = Tex(x - vx, y - vy, seed, amp);
					}
			}
			void Object(int x0, int y0, int ow, int oh, int vx, int vy, uint64_t oseed, float oamp)
			{
				for (int y = y0; y < y0 + oh; ++y)
					for (int x = x0; x < x0 + ow; ++x)
						if (x >= 0 && y >= 0 && x < w && y < h) a[static_cast<size_t>(y) * w + x] = Tex(x, y, oseed, oamp);
				for (int y = y0 + vy; y < y0 + vy + oh; ++y)
					for (int x = x0 + vx; x < x0 + vx + ow; ++x)
						if (x >= 0 && y >= 0 && x < w && y < h) b[static_cast<size_t>(y) * w + x] = Tex(x - vx, y - vy, oseed, oamp);
			}
			void Fade(float gain, float offset) { for (float& v : b) v = gain * v + offset; }
			void Flash() { for (size_t i = 0; i < b.size(); ++i) b[i] = 240.0f + 4.0f * Hash(static_cast<int>(i % w), static_cast<int>(i / w), 77); }
			void Cut()
			{
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x) b[static_cast<size_t>(y) * w + x] = Tex(x, y, 9999, amp);
			}
			Plane A() const { Plane p; p.w = w; p.h = h; p.px = a.data(); return p; }
			Plane B() const { Plane p; p.w = w; p.h = h; p.px = b.data(); return p; }
		};

		// ---- records ----
		inline Params Pan(double sx, double sy)
		{
			Params p;
			p.type = ModelType::Translation;
			p.h[2] = sx;
			p.h[5] = sy;
			return p;
		}

		inline std::shared_ptr<CameraMotion> MakeRecord(const Params& p, double conf = 0.95, double pairSeconds = 1.0 / 24, Reason reason = Reason::Ok)
		{
			auto rec = std::make_shared<CameraMotion>();
			rec->valid = true;
			rec->confidence = conf;
			rec->reason = reason;
			rec->pairSeconds = pairSeconds;
			rec->params = p;
			rec->picture.left = 0; rec->picture.top = 0; rec->picture.right = kGW * 16; rec->picture.bottom = kGH * 16;
			auto b = std::make_shared<BlockData>();
			b->gridW = kGW; b->gridH = kGH; b->blockSize = 16;
			b->label.assign(kN, static_cast<uint8_t>(Label::Camera));
			b->resX.assign(kN, 0.0f); b->resY.assign(kN, 0.0f);
			b->modelX.assign(kN, 0.0f); b->modelY.assign(kN, 0.0f);
			b->objectId.assign(kN, 0); b->tinyMover.assign(kN, 0); b->excluded.assign(kN, 0);
			b->absThresholdPx = 0.75f; b->relThreshold = 0.05f;
			for (int y = 0; y < kGH; ++y)
				for (int x = 0; x < kGW; ++x)
				{
					double cx, cy, dx, dy;
					BlockCentre(x, y, 16, rec->picture, &cx, &cy);
					Displacement(p, cx, cy, &dx, &dy);
					b->modelX[static_cast<size_t>(y) * kGW + x] = static_cast<float>(dx);
					b->modelY[static_cast<size_t>(y) * kGW + x] = static_cast<float>(dy);
				}
			rec->blocks = b;
			return rec;
		}

		inline BlockData& Blocks(const std::shared_ptr<CameraMotion>& rec) { return const_cast<BlockData&>(*rec->blocks); }
		inline size_t Idx(int bx, int by) { return static_cast<size_t>(by) * kGW + bx; }

		inline MeasurementInput InputFor(const CameraMotion* prev)
		{
			MeasurementInput in;
			in.prev = prev;
			in.nextPairSeconds = prev ? prev->pairSeconds : 0.0;
			return in;
		}

		inline MeasurementSwitches Narrow()
		{
			MeasurementSwitches sw;
			sw.narrowWindow = true;
			return sw;
		}

		inline MatchResult Today(const Scene& s, const MatchConfig& cfg = MatchConfig(), const MatchState* prev = nullptr, double gx = kNaN, double gy = kNaN)
		{
			return Match(cfg, s.A(), s.B(), nullptr, prev, gx, gy);
		}

		inline bool Same(const MatchResult& a, const MatchResult& b)
		{
			return a.vx.size() == b.vx.size() && std::memcmp(a.vx.data(), b.vx.data(), a.vx.size() * sizeof(float)) == 0 &&
				std::memcmp(a.vy.data(), b.vy.data(), a.vy.size() * sizeof(float)) == 0 &&
				std::memcmp(a.sad.data(), b.sad.data(), a.sad.size() * sizeof(float)) == 0;
		}

		inline double Dist(const MatchResult& a, const MatchResult& b, size_t i) { return std::hypot(a.vx[i] - b.vx[i], a.vy[i] - b.vy[i]); }

		inline double MaxDiff(const MatchResult& a, const MatchResult& b)
		{
			double m = 0;
			for (size_t i = 0; i < a.vx.size(); ++i) m = std::max(m, Dist(a, b, i));
			return m;
		}

		inline int CountPath(const MatchResult& r, uint8_t path)
		{
			int c = 0;
			for (uint8_t p : r.path) if (p == path) ++c;
			return c;
		}

		inline bool Near(const MatchResult& r, int bx, int by, double vx, double vy, double tol = 0.5)
		{
			const size_t i = Idx(bx, by);
			return std::hypot(r.vx[i] - vx, r.vy[i] - vy) <= tol;
		}

		inline std::string Cost(const char* name, const MatchResult& full, const MatchResult& guided)
		{
			char buf[256];
			const double saving = full.evaluations > 0 ? 100.0 * (1.0 - static_cast<double>(guided.evaluations) / static_cast<double>(full.evaluations)) : 0.0;
			std::snprintf(buf, sizeof(buf), "CAMERA_MEAS_COST case=%s full=%llu guided=%llu saving=%.1f%% ms_full=%.2f ms_guided=%.2f", name,
				static_cast<unsigned long long>(full.evaluations), static_cast<unsigned long long>(guided.evaluations), saving, full.ms, guided.ms);
			return buf;
		}

		inline MeasurementSwitches AllSavings()
		{
			MeasurementSwitches sw;
			sw.modelGlobal = sw.guidedCandidate = sw.narrowWindow = sw.e1Skip = sw.staticGate = true;
			return sw;
		}
	}

	TEST_CLASS(CameraMeasurementTests)
	{
	public:
		// a) off = today's search, bit for bit
		TEST_METHOD(SwitchesOffGiveAnInactivePlanAndTheSameSearch)
		{
			using namespace MeasTest;
			Scene s;
			s.Pan(3, -2);
			auto rec = MakeRecord(Pan(6, -4));
			const MeasurementSwitches off;
			Req(!off.Any(), "no switch on");
			const MeasurementPlan plan = BuildMeasurementPlan(off, MeasurementConfig(), InputFor(rec.get()));
			Req(!plan.Active() && plan.hint.empty() && plan.rois.empty() && !plan.skipMatcher && !plan.useModelGlobal, "inactive plan is empty");
			const MatchResult base = Today(s);
			const MatchResult withInactive = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			Req(Same(base, withInactive), "inactive plan: bitwise today's search");
			Req(base.evaluations == withInactive.evaluations, "same evaluations");
			// An active plan with nothing to apply (cutSkip on a normal pair) is also today's search.
			MeasurementSwitches idle;
			idle.cutSkip = true;
			const MeasurementPlan p2 = BuildMeasurementPlan(idle, MeasurementConfig(), InputFor(rec.get()));
			Req(p2.Active() && p2.hint.empty() && !p2.skipMatcher, "active but nothing planned");
			Req(Same(base, Match(MatchConfig(), s.A(), s.B(), &p2, nullptr)), "idle plan: today's search");
		}

		// b) narrow window on a pan
		TEST_METHOD(NarrowWindowOnAPanMatchesTheFullSearchCheaply)
		{
			using namespace MeasTest;
			Scene s;
			s.Pan(3, -2);
			auto rec = MakeRecord(Pan(6, -4));
			const MeasurementPlan plan = BuildMeasurementPlan(Narrow(), MeasurementConfig(), InputFor(rec.get()));
			Req(plan.Active() && plan.counters.narrowBlocks == kN, "every block narrow");
			Req(plan.narrowRadiusPx >= 1.0, "radius floor");
			const MatchResult full = Today(s);
			const MatchResult guided = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			Req(guided.narrowAccepted >= static_cast<uint32_t>(0.9 * kN), "narrow accepted on >= 90% of blocks: " + std::to_string(guided.narrowAccepted));
			Req(MaxDiff(full, guided) <= 0.5, "vectors equal to the full search: " + std::to_string(MaxDiff(full, guided)));
			Req(guided.evaluations < full.evaluations, "fewer evaluations");
			Say(Cost("pan", full, guided));
		}

		// c) an object enters a camera-labelled region: verification fails, escalation finds it
		TEST_METHOD(ObjectInCameraRegionEscalatesAndIsFound)
		{
			using namespace MeasTest;
			Scene s;
			s.Pan(3, -2);
			s.Object(80, 64, 40, 40, -6, 4, 4242, 60);
			auto rec = MakeRecord(Pan(6, -4));
			MeasurementPlan plan = BuildMeasurementPlan(Narrow(), MeasurementConfig(), InputFor(rec.get()));
			const MatchResult r = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			for (int by = 9; by <= 11; ++by)
				for (int bx = 11; bx <= 13; ++bx)
				{
					Req(r.path[Idx(bx, by)] == kPathEscalated, "object block escalated");
					Req(Near(r, bx, by, -6, 4), "object block holds the object's vector");
				}
			std::vector<uint8_t> accepted(kN, 1);
			for (size_t i = 0; i < kN; ++i) accepted[i] = r.path[i] != kPathEscalated ? 1 : 0;
			const uint32_t n = ApplyVerification(plan, accepted.data());
			Req(n == r.escalated && n >= 9, "ApplyVerification counts the escalated blocks: " + std::to_string(n));
			Req(plan.counters.escalated == n, "counter");
			Req(plan.hint[Idx(12, 10)] == static_cast<uint8_t>(BlockHint::FullSearch) && plan.eligible[Idx(12, 10)] == 0, "escalated block is full search");
			Req(ApplyVerification(plan, accepted.data()) == 0, "idempotent");
		}

		// d) temporal seed
		TEST_METHOD(TemporalSeedNarrowsOnlyWhenItAgreesWithNvof)
		{
			using namespace MeasTest;
			auto rec = MakeRecord(Pan(6, -4));
			std::vector<float> fx(kN, 6.0f), fy(kN, -4.0f), nx(kN, 6.0f), ny(kN, -4.0f);
			MeasurementSwitches sw;
			sw.temporalSeed = true;
			MeasurementInput in = InputFor(rec.get());
			in.prevFwdX = fx.data(); in.prevFwdY = fy.data(); in.nvofX = nx.data(); in.nvofY = ny.data();
			MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), in);
			Req(plan.counters.narrowBlocks == kN && plan.counters.temporalSeeded == kN, "agreeing seed: all narrow and temporally seeded");
			// NVOF disagrees on the first 100 blocks.
			for (size_t i = 0; i < 100; ++i) { nx[i] += 4.0f; ny[i] += 4.0f; }
			plan = BuildMeasurementPlan(sw, MeasurementConfig(), in);
			Req(plan.counters.narrowBlocks == kN - 100 && plan.counters.fullBlocks == 100, "disagreeing blocks are full search");
			Req(plan.hint[0] == static_cast<uint8_t>(BlockHint::FullSearch) && plan.hint[100] == static_cast<uint8_t>(BlockHint::NarrowAroundPrediction), "which blocks");
			// The seed is the previous vector (agreeing with NVOF), not the model.
			std::fill(fx.begin(), fx.end(), 6.4f);
			std::fill(nx.begin(), nx.end(), 6.4f);
			std::fill(ny.begin(), ny.end(), -4.0f);
			MeasurementSwitches both = sw;
			both.narrowWindow = true;
			plan = BuildMeasurementPlan(both, MeasurementConfig(), in);
			Req(std::fabs(plan.seedX[5] - 6.4f) < 1e-5f, "seed from the previous forward vector");
			// Without the switch the seed is the model's even though prevFwd is supplied.
			plan = BuildMeasurementPlan(Narrow(), MeasurementConfig(), in);
			Req(plan.counters.temporalSeeded == 0 && std::fabs(plan.seedX[5] - 6.0f) < 1e-5f, "temporal switch off: model seed");
		}

		// e) E1 skip and its escalation
		TEST_METHOD(E1SkipKeepsTheFirstSweepWhenItAgreesWithTheModel)
		{
			using namespace MeasTest;
			Scene s;
			s.Pan(3, -2);
			MeasurementSwitches sw;
			sw.e1Skip = true;
			auto rec = MakeRecord(Pan(6, -4));
			const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
			Req(plan.e1Skip && plan.counters.fullBlocks == kN && plan.eligible[0] == 1, "E1 plan: full first sweep, eligible blocks");
			const MatchResult today = Today(s);
			const MatchResult r = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			Req(r.e1Skipped >= static_cast<uint32_t>(0.85 * kN), "E1 skipped most blocks: " + std::to_string(r.e1Skipped));
			Req(CountPath(r, kPathE1Skip) == static_cast<int>(r.e1Skipped), "path 5 count");
			Req(MaxDiff(today, r) <= 0.5, "within 0.5 px of the non-skip run");
			Req(r.evaluations < today.evaluations, "evaluations saved");
			// The model disagrees with the first sweep: no skip, every eligible block runs the sweep (escalation).
			auto wrong = MakeRecord(Pan(-10, 6));
			const MeasurementPlan bad = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(wrong.get()));
			const MatchResult r2 = Match(MatchConfig(), s.A(), s.B(), &bad, nullptr);
			Req(r2.e1Skipped == 0 && r2.escalated == kN, "wrong prediction: no skip, all escalated: " + std::to_string(r2.escalated));
			Req(Same(today, r2) && r2.evaluations == today.evaluations, "escalation gives today's result");
		}

		// f) E4 static from the model
		TEST_METHOD(StaticGateFromTheModel)
		{
			using namespace MeasTest;
			Scene s;
			s.Pan(0, 0);
			const MatchResult base = Today(s);
			const MatchState state = StateFrom(base);
			MatchConfig cfg;
			cfg.gate = true;
			MeasurementSwitches sw;
			sw.staticGate = true;
			auto still = MakeRecord(Pan(0, 0), 0.95);
			MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(still.get()));
			Req(plan.staticScene && plan.counters.staticFromModel, "static plan");
			MatchResult r = Match(cfg, s.A(), s.B(), &plan, &state);
			Req(r.staticKept == kN && r.evaluations == 0 && Same(r, base), "previous field kept");
			auto moving = MakeRecord(Pan(6, -4), 0.95);
			plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(moving.get()));
			Req(!plan.staticScene, "moving model is not static");
			Req(Match(cfg, s.A(), s.B(), &plan, &state).staticKept == 0, "no static keep");
			auto weak = MakeRecord(Pan(0, 0), 0.8);
			plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(weak.get()));
			Req(!plan.staticScene, "confidence below the static threshold");
			// A small model displacement at the corners within one sigma still counts as static; beyond it does not.
			auto drift = MakeRecord(Pan(0.2, 0), 0.95);
			Req(BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(drift.get())).staticScene, "within one precision sigma");
			auto drift2 = MakeRecord(Pan(0.6, 0), 0.95);
			Req(!BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(drift2.get())).staticScene, "beyond one precision sigma");
			// Today's D1-based E4 (no plan) keeps the same semantics.
			Req(Match(cfg, s.A(), s.B(), nullptr, &state, 0.0, 0.0).staticKept == kN, "E4 from the D1 global");
			Req(Match(cfg, s.A(), s.B(), nullptr, &state, 4.0, 0.0).staticKept == 0, "moving D1 global: searched");
		}

		// g) cut
		TEST_METHOD(CutSkipsTheMatcherAndSeedsFromTheQueuedPair)
		{
			using namespace MeasTest;
			Scene s;
			s.Pan(3, -2);
			s.Cut();
			auto rec = MakeRecord(Pan(6, -4));
			MeasurementInput in = InputFor(rec.get());
			in.cut = true;
			in.haveNextPair = true;
			MeasurementSwitches sw;
			sw.cutSkip = true;
			MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), in);
			Req(plan.active && plan.skipMatcher && plan.scene == SceneAction::SkipAcrossCut && plan.seedFromNext && plan.counters.matcherSkipped, "cut plan");
			Req(plan.hint.empty(), "a skipped pair has no per-block guidance");
			const MatchResult r = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			Req(r.skipped && r.evaluations == 0 && CountPath(r, kPathSkipped) == static_cast<int>(kN) && r.sad[0] >= 1e4f, "skipped pair");
			in.haveNextPair = false;
			Req(!BuildMeasurementPlan(sw, MeasurementConfig(), in).seedFromNext, "no queued pair: no seed");
			// Without the switch the matcher searches normally; the previous record is not used across the cut.
			MeasurementSwitches nw = Narrow();
			plan = BuildMeasurementPlan(nw, MeasurementConfig(), in);
			Req(!plan.skipMatcher && plan.hint.empty() && std::strlen(plan.fallbackReason) > 0, "cut without cutSkip: normal search, no guidance");
			const MatchResult normal = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			Req(!normal.skipped && normal.evaluations > 0 && Same(normal, Today(s)), "normal search");
			// A record that itself ends a cut gives no per-block guidance (first post-cut pair: full search).
			for (Reason why : { Reason::Cut, Reason::PostCutSeeded })
			{
				auto post = MakeRecord(Pan(6, -4), 0.95, 1.0 / 24, why);
				const MeasurementPlan p = BuildMeasurementPlan(nw, MeasurementConfig(), InputFor(post.get()));
				Req(p.hint.empty() && std::strlen(p.fallbackReason) > 0, std::string("record after a cut: ") + p.fallbackReason);
			}
		}

		// h) fade
		TEST_METHOD(FadeUsesTheZeroMeanCost)
		{
			using namespace MeasTest;
			Scene s(16.0f);
			s.Pan(3, -2);
			s.Object(80, 64, 40, 40, -6, 4, 4242, 16);
			s.Fade(0.7f, 12.0f);
			auto rec = MakeRecord(Pan(6, -4));
			MeasurementInput in = InputFor(rec.get());
			in.fade = true;
			in.fadeGain = 0.7;
			in.fadeOffset = 12;
			MeasurementSwitches sw = Narrow();
			sw.fadeNormalised = true;
			const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), in);
			Req(plan.fadeNormalised && plan.scene == SceneAction::FadeNormalised && plan.fadeGain == 0.7 && plan.fadeOffset == 12 && plan.counters.fadeNormalisedUsed, "fade plan");
			const MatchResult r = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			int accepted = 0, total = 0;
			for (int by = 3; by <= 20; ++by)
				for (int bx = 3; bx <= 36; ++bx)
				{
					if (bx >= 9 && bx <= 15 && by >= 7 && by <= 13) continue;   // the pasted object and its surround
					++total;
					if (r.path[Idx(bx, by)] == kPathNarrow && Near(r, bx, by, 3, -2)) ++accepted;
				}
			Req(accepted == total, "zero-mean narrow cost accepts the true vector: " + std::to_string(accepted) + "/" + std::to_string(total));
			// Plain SAD at the same vectors fails the tolerance (gain and offset), so the blocks escalate.
			const MeasurementPlan plain = BuildMeasurementPlan(Narrow(), MeasurementConfig(), in);
			Req(!plain.fadeNormalised, "switch off: plain cost");
			const MatchResult p = Match(MatchConfig(), s.A(), s.B(), &plain, nullptr);
			Req(p.path[Idx(20, 12)] == kPathEscalated && p.path[Idx(6, 6)] == kPathEscalated, "plain SAD fails tolerance and escalates");
			// A failing normalised block (the object) escalates to today's search.
			Req(r.path[Idx(12, 10)] == kPathEscalated, "object block escalates under the normalised cost");
			Req(r.escalated > 0, "escalation counted");
		}

		// i) flash
		TEST_METHOD(FlashIsBridgedAndABridgedRecordGivesNoTemporalSeed)
		{
			using namespace MeasTest;
			Scene s;
			s.Pan(3, -2);
			s.Flash();
			auto rec = MakeRecord(Pan(6, -4));
			MeasurementInput in = InputFor(rec.get());
			in.flash = true;
			MeasurementSwitches sw;
			sw.flashBridge = true;
			MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), in);
			Req(plan.skipMatcher && plan.scene == SceneAction::BridgeFlash && plan.counters.matcherSkipped, "flash pair skipped");
			Req(Match(MatchConfig(), s.A(), s.B(), &plan, nullptr).skipped, "matcher skips");
			in.flash = false;
			sw.flashBridge = false;
			// The bridged record is usable, but never seeds temporally.
			auto bridged = MakeRecord(Pan(6, -4), 0.95, 1.0 / 24, Reason::FlashBridged);
			bridged->bridged = true;
			std::vector<float> fx(kN, 6.4f), fy(kN, -4.0f), nx(kN, 6.4f), ny(kN, -4.0f);
			MeasurementInput bi = InputFor(bridged.get());
			bi.prevFwdX = fx.data(); bi.prevFwdY = fy.data(); bi.nvofX = nx.data(); bi.nvofY = ny.data();
			sw.narrowWindow = true;
			sw.temporalSeed = true;
			plan = BuildMeasurementPlan(sw, MeasurementConfig(), bi);
			Req(!plan.hint.empty() && plan.counters.narrowBlocks == kN, "guidance allowed from a bridged record");
			Req(plan.counters.temporalSeeded == 0 && std::fabs(plan.seedX[0] - 6.0f) < 1e-5f, "temporal seed forced off");
			Req(std::strlen(plan.fallbackReason) == 0, "no fallback");
			// The same inputs with an ordinary record do seed temporally.
			auto ordinary = MakeRecord(Pan(6, -4));
			bi.prev = ordinary.get();
			plan = BuildMeasurementPlan(sw, MeasurementConfig(), bi);
			Req(plan.counters.temporalSeeded == kN && std::fabs(plan.seedX[0] - 6.4f) < 1e-5f, "ordinary record: temporal seed");
		}

		// j) tiny movers
		TEST_METHOD(TinyMoversUseTheFinerGridAndAreNeverNarrowed)
		{
			using namespace MeasTest;
			Scene s(2.0f);
			s.Pan(0, 0);
			s.Object(80, 64, 3, 3, 2, 1, 555, 60);
			auto rec = MakeRecord(Pan(0, 0));
			Blocks(rec).label[Idx(10, 8)] = static_cast<uint8_t>(Label::Object);
			Blocks(rec).tinyMover[Idx(10, 8)] = 1;
			MeasurementSwitches sw = Narrow();
			sw.tinyMoverRoi = true;
			const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
			Req(plan.hint[Idx(10, 8)] == static_cast<uint8_t>(BlockHint::FinerGrid) && plan.rois.size() == 1 && plan.counters.finerBlocks == 1, "finer grid block");
			Req(plan.counters.narrowBlocks == kN - 1, "the rest is narrow");
			const MatchResult r = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			Req(r.path[Idx(10, 8)] == kPathFiner && r.finer == 1, "finer path");
			Req(Near(r, 10, 8, 2, 1), "the finer grid finds the 3x3 mover: " + std::to_string(r.vx[Idx(10, 8)]) + "," + std::to_string(r.vy[Idx(10, 8)]));
			Req(r.path[Idx(5, 5)] == kPathNarrow, "ordinary blocks narrow");
			// Without the switch the tiny-mover block is an ordinary (here: narrow) camera block, never FinerGrid.
			const MeasurementPlan none = BuildMeasurementPlan(Narrow(), MeasurementConfig(), InputFor(rec.get()));
			Req(none.counters.finerBlocks == 0 && none.rois.empty(), "switch off: no finer grid");
		}

		TEST_METHOD(TinyMoverRoiBudgetFallsBackInRasterOrder)
		{
			using namespace MeasTest;
			auto rec = MakeRecord(Pan(0, 0));
			const int xs[4] = { 2, 12, 22, 32 }, ys[4] = { 2, 8, 14, 20 };
			for (int j = 0; j < 4; ++j)
				for (int i = 0; i < 4; ++i)
					for (int dy = 0; dy < 2; ++dy)
						for (int dx = 0; dx < 2; ++dx)
						{
							Blocks(rec).label[Idx(xs[i] + dx, ys[j] + dy)] = static_cast<uint8_t>(Label::Object);
							Blocks(rec).tinyMover[Idx(xs[i] + dx, ys[j] + dy)] = 1;
						}
			MeasurementSwitches sw;
			sw.tinyMoverRoi = true;
			const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
			// 16 ROIs x 4 blocks = 64 > 5% of 960 = 48: the first 12 fit, the last row of ROIs does not.
			Req(plan.counters.roiOverBudget == 16, "over-budget blocks: " + std::to_string(plan.counters.roiOverBudget));
			Req(plan.rois.size() == 12, "kept ROIs");
			Req(plan.hint[Idx(2, 14)] == static_cast<uint8_t>(BlockHint::FinerGrid) && plan.hint[Idx(33, 15)] == static_cast<uint8_t>(BlockHint::FinerGrid), "earlier ROIs keep the finer grid");
			Req(plan.hint[Idx(2, 20)] == static_cast<uint8_t>(BlockHint::FullSearch) && plan.hint[Idx(33, 21)] == static_cast<uint8_t>(BlockHint::FullSearch), "later ROIs are full search");
			Req(plan.counters.finerBlocks == 48 && std::strlen(plan.fallbackReason) > 0, "counters and reason");
			// Within budget: nothing falls back.
			MeasurementConfig big;
			big.roiBudgetShare = 0.5;
			Req(BuildMeasurementPlan(sw, big, InputFor(rec.get())).counters.roiOverBudget == 0, "budget respected");
		}

		// k) object-model seeds
		TEST_METHOD(ObjectBlocksAreSeededFromTheObjectModel)
		{
			using namespace MeasTest;
			Scene s;
			s.Pan(3, -2);
			s.Object(80, 64, 40, 40, -4, 3, 4242, 60);
			auto rec = MakeRecord(Pan(6, -4));
			ObjectModel obj;
			obj.id = 1;
			obj.type = ModelType::Translation;
			obj.h[2] = -8; obj.h[5] = 6;
			rec->objects.push_back(obj);
			for (int by = 8; by <= 12; ++by)
				for (int bx = 10; bx <= 14; ++bx)
				{
					Blocks(rec).label[Idx(bx, by)] = static_cast<uint8_t>(Label::Object);
					Blocks(rec).objectId[Idx(bx, by)] = 1;
				}
			MeasurementSwitches sw = Narrow();
			sw.objects = true;
			const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
			Req(plan.counters.objectSeeded == 25, "object blocks narrow: " + std::to_string(plan.counters.objectSeeded));
			Req(std::fabs(plan.seedX[Idx(12, 10)] + 8.0f) < 1e-5f && std::fabs(plan.seedY[Idx(12, 10)] - 6.0f) < 1e-5f, "seed = the object prediction");
			Req(std::fabs(plan.predX[Idx(12, 10)] + 8.0f) < 1e-5f && plan.eligible[Idx(12, 10)] == 1, "prediction and eligibility");
			const MatchResult r = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			for (int by = 9; by <= 11; ++by)
				for (int bx = 11; bx <= 13; ++bx)
				{
					Req(r.path[Idx(bx, by)] == kPathNarrow, "object block accepted by the narrow window");
					Req(Near(r, bx, by, -4, 3), "object's vector");
				}
			// Without the object switch those blocks are plain full searches.
			const MeasurementPlan off = BuildMeasurementPlan(Narrow(), MeasurementConfig(), InputFor(rec.get()));
			Req(off.counters.objectSeeded == 0 && off.hint[Idx(12, 10)] == static_cast<uint8_t>(BlockHint::FullSearch) && off.eligible[Idx(12, 10)] == 0, "objects off: full search");
			const MatchResult r2 = Match(MatchConfig(), s.A(), s.B(), &off, nullptr);
			Req(r2.path[Idx(12, 10)] == kPathFull && Near(r2, 12, 10, -4, 3), "full search still finds it");
		}

		// l) the model as D1's global
		TEST_METHOD(ModelAsD1GlobalFindsAPanBeyondTheSearchRadius)
		{
			using namespace MeasTest;
			Scene s;
			s.Pan(18, -4);
			MeasurementSwitches sw;
			sw.modelGlobal = true;
			// Previous pair 2/24 s with a (24, -6) source-px pan; next pair 3/24 s: x1.5 = (36, -9) source = (18, -4.5) matcher.
			auto rec = MakeRecord(Pan(24, -6), 0.95, 2.0 / 24);
			MeasurementInput in = InputFor(rec.get());
			in.nextPairSeconds = 3.0 / 24;
			const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), in);
			Req(plan.useModelGlobal && plan.counters.modelGlobalUsed && plan.hint.empty(), "model global only");
			Req(std::fabs(plan.globalX - 36.0) < 1e-9 && std::fabs(plan.globalY + 9.0) < 1e-9, "forward global time-scaled");
			Req(std::fabs(plan.globalBwdX + 36.0) < 1e-9 && std::fabs(plan.globalBwdY - 9.0) < 1e-9, "backward global");
			in.nextPairSeconds = 2.0 / 24 * (2.0 / 3.0) * 1.5;   // 2/24: no scaling
			Req(std::fabs(BuildMeasurementPlan(sw, MeasurementConfig(), in).globalX - 24.0) < 1e-9, "same duration: unscaled");
			auto rec32 = MakeRecord(Pan(24, -6), 0.95, 3.0 / 24);
			MeasurementInput in2 = InputFor(rec32.get());
			in2.nextPairSeconds = 2.0 / 24;
			Req(std::fabs(BuildMeasurementPlan(sw, MeasurementConfig(), in2).globalX - 16.0) < 1e-9, "shorter pair scales down");

			const MatchResult withModel = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			const MatchResult today = Today(s);
			int okModel = 0, okToday = 0, total = 0;
			for (int by = 1; by <= 22; ++by)
				for (int bx = 1; bx <= 35; ++bx)
				{
					++total;
					if (Near(withModel, bx, by, 18, -4)) ++okModel;
					if (Near(today, bx, by, 18, -4)) ++okToday;
				}
			Req(okModel >= static_cast<int>(0.95 * total), "found via the global start: " + std::to_string(okModel) + "/" + std::to_string(total));
			Req(okToday < total / 2, "not found by the zero-centred search alone: " + std::to_string(okToday));
		}

		// m) layout + status line
		TEST_METHOD(GuideTexelsAndStatusLine)
		{
			using namespace MeasTest;
			auto rec = MakeRecord(Pan(6, -4));
			Blocks(rec).label[7] = static_cast<uint8_t>(Label::Overlay);
			MeasurementSwitches sw = Narrow();
			sw.guidedCandidate = true;
			MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
			std::vector<float> guide, code;
			PackGuideTexels(plan, guide, code);
			Req(guide.size() == kN * 4 && code.size() == kN, "sizes");
			Req(guide[0] == 6.0f && guide[1] == -4.0f && guide[2] == 6.0f && guide[3] == -4.0f, "seed and prediction channels");
			Req(code[0] == static_cast<float>(static_cast<int>(BlockHint::NarrowAroundPrediction) + 4), "code = hint + 4 * eligible");
			Req(code[7] == static_cast<float>(static_cast<int>(BlockHint::FullSearch)), "ineligible block: hint only");
			PackGuideTexels(MeasurementPlan(), guide, code);
			Req(guide.empty() && code.empty(), "inactive plan packs nothing");
			MeasurementSwitches g;
			g.modelGlobal = true;
			PackGuideTexels(BuildMeasurementPlan(g, MeasurementConfig(), InputFor(rec.get())), guide, code);
			Req(guide.empty() && code.empty(), "no per-block data packs nothing");

			MeasurementDiagnostics d;
			Req(!d.StatusLine().empty(), "status before any frame");
			d.Add(plan);
			MeasurementInput cutIn = InputFor(rec.get());
			cutIn.cut = true;
			MeasurementSwitches cs = Narrow();
			cs.cutSkip = true;
			d.Add(BuildMeasurementPlan(cs, MeasurementConfig(), cutIn));
			MeasurementSwitches lo = Narrow();
			auto weak = MakeRecord(Pan(6, -4), 0.3);
			d.Add(BuildMeasurementPlan(lo, MeasurementConfig(), InputFor(weak.get())));
			const std::string line = d.StatusLine();
			Say(line);
			Req(d.frames == 3 && d.matcherSkipped == 1 && d.narrow == kN - 1,"diagnostic totals");
			Req(line.find("narrow") != std::string::npos && line.find("skipped") != std::string::npos && line.find("low confidence") != std::string::npos, "status line text: " + line);
		}

		// n) determinism
		TEST_METHOD(PlansAndMatchesAreDeterministic)
		{
			using namespace MeasTest;
			Scene s;
			s.Pan(3, -2);
			s.Object(80, 64, 40, 40, -6, 4, 4242, 60);
			auto rec = MakeRecord(Pan(6, -4));
			const MeasurementSwitches sw = AllSavings();
			const MeasurementPlan p1 = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
			const MeasurementPlan p2 = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
			Req(p1.hint == p2.hint && p1.eligible == p2.eligible && p1.seedX == p2.seedX && p1.predY == p2.predY &&
				p1.globalX == p2.globalX && p1.narrowRadiusPx == p2.narrowRadiusPx, "identical plans");
			const MatchResult a = Match(MatchConfig(), s.A(), s.B(), &p1, nullptr);
			const MatchResult b = Match(MatchConfig(), s.A(), s.B(), &p2, nullptr);
			Req(Same(a, b) && a.path == b.path && a.evaluations == b.evaluations && a.escalated == b.escalated, "identical matches");
		}

		// the fitter's own record drives the plan
		TEST_METHOD(FitterRecordDrivesTheNarrowPlan)
		{
			using namespace MeasTest;
			CameraSynth::GridSpec g;
			g.w = kGW; g.h = kGH; g.blockSize = 16;
			CameraSynth::Field f(g);
			f.FromModel(CameraSynth::PanModel(6, -4));
			Fitter fitter;
			const CameraMotion rec = fitter.Fit(f.Input(1.0 / 24, 5));
			Req(rec.valid && rec.confidence >= 0.5, "fitter record: " + CameraSynth::Describe(rec));
			MeasurementConfig cfg;
			cfg.minConfidence = 0.5;
			MeasurementInput in = InputFor(&rec);
			const MeasurementPlan plan = BuildMeasurementPlan(Narrow(), cfg, in);
			Req(plan.counters.narrowBlocks >= static_cast<uint32_t>(0.95 * kN), "fitter record: narrow plan " + std::to_string(plan.counters.narrowBlocks));
			Scene s;
			s.Pan(3, -2);
			const MatchResult r = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
			Req(r.narrowAccepted >= static_cast<uint32_t>(0.9 * kN) && MaxDiff(r, Today(s)) <= 0.5, "fitter record: guided match");
		}

		// cost summary
		TEST_METHOD(CostSummary)
		{
			using namespace MeasTest;
			const MeasurementSwitches sw = AllSavings();
			// pan
			{
				Scene s;
				s.Pan(3, -2);
				auto rec = MakeRecord(Pan(6, -4));
				const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
				const MatchResult full = Today(s), guided = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
				Say(Cost("pan", full, guided));
				Req(guided.evaluations * 2 < full.evaluations, "pan saves more than half");
				double interior = 0;   // edge blocks may legitimately differ (the model candidate can improve them)
				for (int by = 1; by <= 22; ++by)
					for (int bx = 1; bx <= 37; ++bx) interior = std::max(interior, Dist(full, guided, Idx(bx, by)));
				Req(interior <= 0.5, "pan result unchanged in the interior: " + std::to_string(interior));
			}
			// zoom (per-block integer vectors approximate the smooth field)
			{
				Scene s;
				s.Zoom(1.02);
				Params z;
				z.type = ModelType::Similarity;
				z.h[0] = z.h[4] = 1.02;
				auto rec = MakeRecord(z);
				const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
				const MatchResult full = Today(s), guided = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
				Say(Cost("zoom", full, guided));
				Req(guided.evaluations <= full.evaluations, "zoom never costs more");
			}
			// many objects (blocks under an object are labelled Object: full search)
			{
				Scene s;
				s.Pan(3, -2);
				auto rec = MakeRecord(Pan(6, -4));
				SplitMix64 rng(31337);
				for (int k = 0; k < 10; ++k)
				{
					const int ox = 24 + static_cast<int>(rng.Below(240)), oy = 16 + static_cast<int>(rng.Below(130));
					const int vx = static_cast<int>(rng.Below(11)) - 5, vy = static_cast<int>(rng.Below(11)) - 5;
					s.Object(ox, oy, 24, 24, vx, vy, 1000 + k, 60);
					for (int by = (oy - 8) / 8; by <= (oy + 24 + 8) / 8; ++by)
						for (int bx = (ox - 8) / 8; bx <= (ox + 24 + 8) / 8; ++bx)
							if (bx >= 0 && by >= 0 && bx < kGW && by < kGH) Blocks(rec).label[Idx(bx, by)] = static_cast<uint8_t>(Label::Object);
				}
				const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
				const MatchResult full = Today(s), guided = Match(MatchConfig(), s.A(), s.B(), &plan, nullptr);
				Say(Cost("objects", full, guided));
				Req(guided.evaluations < full.evaluations, "objects: still cheaper");
			}
			// static (E4 from the model; the baseline is the plain search without the gate)
			{
				Scene s;
				s.Pan(0, 0);
				auto rec = MakeRecord(Pan(0, 0));
				const MeasurementPlan plan = BuildMeasurementPlan(sw, MeasurementConfig(), InputFor(rec.get()));
				const MatchResult full = Today(s);
				const MatchState state = StateFrom(full);
				MatchConfig cfg;
				cfg.gate = true;
				const MatchResult guided = Match(cfg, s.A(), s.B(), &plan, &state);
				Say(Cost("static", full, guided));
				Req(guided.evaluations == 0 && guided.staticKept == kN, "static: nothing searched");
			}
		}
	};
}
