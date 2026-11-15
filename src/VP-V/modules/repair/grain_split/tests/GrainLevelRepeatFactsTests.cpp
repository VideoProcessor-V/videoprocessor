// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// P10g C8 (floor design s15) with the per-neighbour RepeatFact tri-state: the block test is PRIMARY (owner OD:744) and runs for every
// neighbour not named Repeat; a Repeat fact only adds a drop, a Distinct fact never vetoes the test. Frozen-12 and dup2 rows, still and
// aerial-crop rows with 0 false fires on both paths, threshold clamp, gate off, status text. CPU only, deterministic, synthetic frames.

#include <VP-V/modules/repair/grain_split/GrainLevel.h>
#include <VP-V/core/FallbackCatalog.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainLevelRepeatFactsNs
{
	using GrainSplit::Plane;
	using GrainLevel::RepeatFact;

	inline void Log(const std::string& s) { Logger::WriteMessage(s.c_str()); }
	inline void Require(bool ok, const std::string& what)
	{
		if (!ok) Log("FAILED: " + what);
		const std::wstring w(what.begin(), what.end());
		Assert::IsTrue(ok, w.c_str());
	}

	constexpr int kW = 192, kH = 128, kB = 16;

	// textured = strong high-frequency content: unit 4's N overestimates the grain there (the aerial-crop case of the floor design).
	inline double Tex(double x, double y, bool textured)
	{
		if (textured) return 400.0 + 60.0 * std::sin(1.1 * x + 0.7 * y) + 45.0 * std::sin(0.9 * x - 1.3 * y) + 30.0 * std::sin(2.1 * x + 0.3 * y);
		return 400.0 + 25.0 * std::sin(0.30 * x + 0.20 * y) + 15.0 * std::sin(0.17 * x - 0.23 * y) + 8.0 * std::sin(0.55 * x + 0.41 * y);
	}
	inline Plane Frame(double sigma, uint32_t seed, bool textured)
	{
		Plane p(kW, kH);
		std::mt19937 rng(seed);
		std::normal_distribution<double> nd(0.0, 1.0);
		for (int y = 0; y < kH; ++y)
			for (int x = 0; x < kW; ++x) p.at(x, y) = Tex(x, y, textured) + sigma * nd(rng);
		return p;
	}
	inline GrainSplit::Split Split(const Plane& y) { return GrainSplit::Compute(y, 1.0, kB); }

	struct Rig
	{
		GrainSplit::Split cur;
		GrainLevel::Result t1;
		std::vector<Plane> bands;
		GrainLevel::BlockField f;
	};

	// seeds[k] == curSeed makes neighbour k an exact repeat of the current frame; facts[k] is the producer fact handed to the gate.
	inline GrainLevel::NeighbourResult Run(Rig& s, double sigma, bool textured, uint32_t curSeed, const std::vector<uint32_t>& seeds,
		const std::vector<RepeatFact>& facts, const GrainLevel::Params& p)
	{
		s.cur = Split(Frame(sigma, curSeed, textured));
		s.f.Init(kW, kH, kB, 0.0, 0.0);
		s.bands.clear();
		for (uint32_t sd : seeds) s.bands.push_back(GrainLevel::BandD(Split(Frame(sigma, sd, textured))));
		std::vector<GrainLevel::Neighbour> nb(seeds.size());
		for (size_t k = 0; k < seeds.size(); ++k)
		{
			nb[k].d = &s.bands[k]; nb[k].field = s.f; nb[k].epoch = 0; nb[k].offset = k < 8 ? -static_cast<int>(k + 1) : static_cast<int>(k - 7);   // 8 back + 4 ahead
			nb[k].fact = facts.empty() ? RepeatFact::Unknown : facts[k];
		}
		GrainLevel::Estimator est; GrainLevel::Context c; c.sourceHz = 24.0; c.field = &s.f;
		est.Step(Split(Frame(sigma, 1, textured)), c);
		s.t1 = est.Step(s.cur, c);
		GrainLevel::Window win; win.back = 8; win.ahead = 4;
		return GrainLevel::NeighbourWiener(s.cur, s.t1, nb, 0, win, p);
	}

	inline std::vector<uint32_t> Seeds(uint32_t first, int n) { std::vector<uint32_t> v; for (int i = 0; i < n; ++i) v.push_back(first + static_cast<uint32_t>(i)); return v; }

	TEST_CLASS(GrainLevelRepeatFactsTests)
	{
	public:
		TEST_METHOD(BlockTestIsPrimaryFactsSupport)
		{
			Rig s;
			const std::vector<uint32_t> seeds = { 91, 90, 92, 93 };
			const GrainLevel::NeighbourResult a = Run(s, 2.0, false, 90, seeds, { RepeatFact::Distinct, RepeatFact::Repeat, RepeatFact::Distinct, RepeatFact::Distinct }, GrainLevel::Params());
			Require(a.droppedByFacts == 1 && a.droppedByBlockTest == 0 && a.blockTestRuns == 3 && a.usedNeighbours == 3, "a Repeat fact drops its neighbour without a test; the block test still runs for the three Distinct ones and keeps them");
			const GrainLevel::NeighbourResult b = Run(s, 2.0, false, 90, seeds, {}, GrainLevel::Params());
			Require(b.droppedByFacts == 0 && b.droppedByBlockTest == 1 && b.blockTestRuns == 4 && b.usedNeighbours == 3, "facts Unknown: the block test ran for all four and found the repeat");
			const GrainLevel::NeighbourResult c = Run(s, 2.0, false, 90, seeds, { RepeatFact::Distinct, RepeatFact::Distinct, RepeatFact::Distinct, RepeatFact::Distinct }, GrainLevel::Params());
			Require(c.droppedRepeat == 1 && c.droppedByBlockTest == 1 && c.blockTestRuns == 4 && c.usedNeighbours == 3, "a Distinct fact never vetoes the block test: the exact repeat the bit called Distinct is dropped");
			Require(c.droppedByFacts == 0 && c.repeatJudged == 4 && c.repeatJudged == b.repeatJudged && c.blockTestRuns == b.blockTestRuns && c.droppedByBlockTest == b.droppedByBlockTest,
				"Distinct facts give the same block-test verdict as Unknown facts (the bit does not change the test)");
			// positive: one Distinct neighbour that really is distinct still gets the block test, which judges and keeps it
			const GrainLevel::NeighbourResult d = Run(s, 2.0, false, 90, { 91 }, { RepeatFact::Distinct }, GrainLevel::Params());
			Require(d.offered == 1 && d.blockTestRuns == 1 && d.repeatJudged == 1 && d.droppedRepeat == 0 && d.usedNeighbours == 1, "a single Distinct neighbour still runs the block test and is kept");
		}

		TEST_METHOD(Frozen12AndDup2OnBothPaths)
		{
			Rig s;
			const std::vector<uint32_t> frozen(12, 100);
			const std::vector<RepeatFact> allRepeat(12, RepeatFact::Repeat);
			for (int path = 0; path < 2; ++path)
			{
				const GrainLevel::NeighbourResult r = Run(s, 2.0, false, 100, frozen, path == 0 ? allRepeat : std::vector<RepeatFact>(), GrainLevel::Params());
				Log(std::string("frozen-12 ") + (path == 0 ? "facts=Repeat" : "facts=Unknown (block test)") + ": dropped " + std::to_string(r.droppedRepeat) + " used " + std::to_string(r.usedNeighbours)
					+ " fallback px " + std::to_string(r.fallbackPixels) + "/" + std::to_string(r.pixels));
				Require(r.droppedRepeat == 12 && r.usedNeighbours == 0 && r.fallbackPixels == r.pixels, "frozen-12: every neighbour leaves, the L1+L2 gains run");
				Require((path == 0) == (r.droppedByFacts == 12) && (path == 1) == (r.droppedByBlockTest == 12), "dropped by the named path");
			}
			// dup2: t-1 and t+1 repeat t among 12 neighbours.
			std::vector<uint32_t> seeds = Seeds(201, 12); seeds[0] = 200; seeds[1] = 200;
			std::vector<RepeatFact> facts(12, RepeatFact::Distinct); facts[0] = RepeatFact::Repeat; facts[1] = RepeatFact::Repeat;
			for (int path = 0; path < 2; ++path)
			{
				const GrainLevel::NeighbourResult r = Run(s, 2.0, false, 200, seeds, path == 0 ? facts : std::vector<RepeatFact>(), GrainLevel::Params());
				Log(std::string("dup2 ") + (path == 0 ? "facts" : "block test") + ": dropped " + std::to_string(r.droppedRepeat) + " used " + std::to_string(r.usedNeighbours) + " meanK " + std::to_string(r.meanKeff));
				Require(r.droppedRepeat == 2 && r.usedNeighbours == 10, "dup2: exactly the two repeats leave");
			}
		}

		TEST_METHOD(StillAndAerialCropFireNothing)
		{
			Rig s;
			for (int textured = 0; textured < 2; ++textured)
				for (int path = 0; path < 2; ++path)
				{
					const GrainLevel::NeighbourResult r = Run(s, 2.0, textured == 1, 300, Seeds(301, 12), path == 0 ? std::vector<RepeatFact>(12, RepeatFact::Distinct) : std::vector<RepeatFact>(), GrainLevel::Params());
					Log(std::string(textured ? "aerial crop " : "still ") + (path == 0 ? "facts=Distinct" : "facts=Unknown") + ": dropped " + std::to_string(r.droppedRepeat) + " judged " + std::to_string(r.repeatJudged));
					Require(r.droppedRepeat == 0, std::string("0 false fires: ") + (textured ? "aerial crop " : "still ") + (path == 0 ? "facts" : "block test"));
				}
		}

		TEST_METHOD(ThresholdIsClamped)
		{
			Rig s;
			GrainLevel::Params wide; wide.repeatFrac = 0.05;      // the value that false-fired: clamped to 0.020, so it cannot fire on independent grain
			Require(Run(s, 2.0, true, 400, Seeds(401, 12), {}, wide).droppedRepeat == 0, "0.05 clamps to 0.020: no false fire on the aerial crop");
			GrainLevel::Params tiny; tiny.repeatFrac = 1e-9;      // clamped up to 0.001: an exact repeat (residual 0) still leaves
			const GrainLevel::NeighbourResult r = Run(s, 2.0, false, 410, { 410, 411, 412 }, {}, tiny);
			Require(r.droppedByBlockTest == 1 && r.usedNeighbours == 2, "tiny threshold clamps up; an exact repeat is still found");
			double t = 0.0; std::string problem;
			Require(!GrainLevel::ParseRepeatThreshold("0.05", t, problem) && t == 0.005 && !problem.empty(), "0.05 rejected at the parser");
		}

		TEST_METHOD(OffDropsNothingAndEngagesTheRow)
		{
			Rig s;
			GrainLevel::Params off; off.repeatGate = false;
			const GrainLevel::NeighbourResult r = Run(s, 2.0, false, 500, { 500, 500, 501 }, { RepeatFact::Repeat, RepeatFact::Unknown, RepeatFact::Unknown }, off);
			Require(r.droppedRepeat == 0 && r.blockTestRuns == 0 && r.usedNeighbours == 3, "off: no drops, no block test, even for a Repeat fact");
			const FallbackCatalog::Decision d = FallbackCatalog::GrainSepRepeatGateOff(true, r.offered, r.repeatJudged);
			Require(d.engaged && d.id == "grain_sep.repeat_gate_off", "off: the row is Engaged");
			Require(FallbackCatalog::GrainSepRepeatStatus(true, r.offered, r.blockTestRuns, r.droppedByBlockTest).empty(), "off: no status text (the row covers it)");
		}

		TEST_METHOD(StatusTextIsTheDefaultStateWithoutANotice)
		{
			const std::string t = FallbackCatalog::GrainSepRepeatStatus(false, 12, 12, 3);
			Require(t.find("repeat facts unavailable: block test") != std::string::npos && t.find("3") != std::string::npos, "block test ran: the text says so with the count");
			Require(FallbackCatalog::GrainSepRepeatStatus(false, 12, 0, 0).empty(), "producer facts decided everything: no text");
			Require(FallbackCatalog::GrainSepRepeatStatus(false, 0, 0, 0).empty(), "nothing offered: no text");
			// not a registry entry: nothing in the registry means the GUI announces nothing
			FallbackStatus::Registry reg;
			Require(!reg.Engaged("grain_sep.repeat_facts"), "no registry entry exists for the default state");
		}
	};
}
