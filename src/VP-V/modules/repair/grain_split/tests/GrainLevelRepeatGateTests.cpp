// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// P10g C8 repeated-frame gate (floor design s15): a neighbour that repeats the current frame leaves B's set, independent-grain
// neighbours are never dropped, a frozen picture falls back to the L1+L2 gains, the setting parses with a visible problem text,
// and the fallback entry engages exactly while a repeat was dropped. CPU only, deterministic, synthetic frames. New tests only.

#include <VP-V/modules/repair/grain_split/GrainLevel.h>
#include <VP-V/core/FallbackCatalog.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainLevelRepeatGateNs
{
	using GrainSplit::Plane;

	inline void Log(const std::string& s) { Logger::WriteMessage(s.c_str()); }
	inline void Require(bool ok, const std::string& what)
	{
		if (!ok) Log("FAILED: " + what);
		const std::wstring w(what.begin(), what.end());
		Assert::IsTrue(ok, w.c_str());
	}

	constexpr int kW = 192, kH = 128, kB = 16;

	inline double Tex(double x, double y)
	{
		return 400.0 + 25.0 * std::sin(0.30 * x + 0.20 * y) + 15.0 * std::sin(0.17 * x - 0.23 * y) + 8.0 * std::sin(0.55 * x + 0.41 * y);
	}
	inline Plane Frame(double sigma, uint32_t seed)
	{
		Plane p(kW, kH);
		std::mt19937 rng(seed);
		std::normal_distribution<double> nd(0.0, 1.0);
		for (int y = 0; y < kH; ++y)
			for (int x = 0; x < kW; ++x) p.at(x, y) = Tex(x, y) + sigma * nd(rng);
		return p;
	}
	inline GrainSplit::Split Split(const Plane& y) { return GrainSplit::Compute(y, 1.0, kB); }

	struct Setup
	{
		GrainSplit::Split cur;
		GrainLevel::Result t1;
		std::vector<Plane> bands;   // bands[i] owned here; neighbours point into it
		GrainLevel::BlockField f;
	};

	// seeds: grain seed per neighbour; a seed equal to curSeed makes that neighbour an exact repeat of the current frame.
	inline GrainLevel::NeighbourResult Run(Setup& s, double sigma, uint32_t curSeed, const std::vector<uint32_t>& seeds, const GrainLevel::Params& p)
	{
		s.cur = Split(Frame(sigma, curSeed));
		s.f.Init(kW, kH, kB, 0.0, 0.0);
		s.bands.clear();
		for (uint32_t sd : seeds) s.bands.push_back(GrainLevel::BandD(Split(Frame(sigma, sd))));
		std::vector<GrainLevel::Neighbour> nb(seeds.size());
		for (size_t k = 0; k < seeds.size(); ++k) { nb[k].d = &s.bands[k]; nb[k].field = s.f; nb[k].epoch = 0; nb[k].offset = -static_cast<int>(k + 1); }
		GrainLevel::Estimator est; GrainLevel::Context c; c.sourceHz = 24.0; c.field = &s.f;
		est.Step(Split(Frame(sigma, 1)), c);
		s.t1 = est.Step(s.cur, c);
		GrainLevel::Window win; win.back = 8; win.ahead = 4;
		return GrainLevel::NeighbourWiener(s.cur, s.t1, nb, 0, win, p);
	}

	TEST_CLASS(GrainLevelRepeatGateTests)
	{
	public:
		TEST_METHOD(RepeatLeavesTheSetAndIndependentGrainStays)
		{
			Require(GrainLevel::Params().repeatGate, "default on");
			Setup s;
			const GrainLevel::NeighbourResult on = Run(s, 2.0, 50, { 51, 50, 52, 53 }, GrainLevel::Params());
			GrainLevel::Params off; off.repeatGate = false;
			const GrainLevel::NeighbourResult no = Run(s, 2.0, 50, { 51, 50, 52, 53 }, off);
			Log("gate on: dropped " + std::to_string(on.droppedRepeat) + " used " + std::to_string(on.usedNeighbours) + " | off: dropped " + std::to_string(no.droppedRepeat) + " used " + std::to_string(no.usedNeighbours));
			Require(on.droppedRepeat == 1 && on.usedNeighbours == 3, "exactly the one repeat left the set");
			Require(no.droppedRepeat == 0 && no.usedNeighbours == 4, "gate off keeps all four");
		}

		TEST_METHOD(IndependentGrainNeverFires)
		{
			const double sigmas[] = { 1.0, 2.0, 4.0 };
			for (double sg : sigmas)
			{
				Setup s;
				const GrainLevel::NeighbourResult r = Run(s, sg, 60, { 61, 62, 63, 64, 65, 66, 67, 68 }, GrainLevel::Params());
				Log("sigma " + std::to_string(sg) + ": dropped " + std::to_string(r.droppedRepeat) + " used " + std::to_string(r.usedNeighbours));
				Require(r.droppedRepeat == 0 && r.usedNeighbours == 8, "no false fire on independent grain at sigma " + std::to_string(sg));
			}
		}

		TEST_METHOD(FrozenPictureFallsBackToTheLevelGains)
		{
			Setup s;
			const GrainLevel::NeighbourResult r = Run(s, 2.0, 70, { 70, 70, 70, 70 }, GrainLevel::Params());
			Require(r.droppedRepeat == 4 && r.usedNeighbours == 0, "every neighbour is a repeat");
			Require(r.fallbackPixels == r.pixels, "no pixel takes the neighbour path");
			bool gains = true;
			for (int y = 0; y < kH && gains; ++y)
				for (int x = 0; x < kW; ++x)
					if (r.cleanD.at(x, y) != s.t1.s1.at(x, y) + s.t1.s2.at(x, y)) { gains = false; break; }
			Require(gains, "the clean band is the L1+L2 gains exactly (never the repeated grain)");
		}

		TEST_METHOD(SettingParsesWithAVisibleProblem)
		{
			bool v = false; std::string problem;
			Require(GrainLevel::ParseRepeatGate("on", v, problem) && v && problem.empty(), "on");
			Require(GrainLevel::ParseRepeatGate(" OFF ", v, problem) && !v, "off, case and spaces tolerated");
			Require(!GrainLevel::ParseRepeatGate("maybe", v, problem) && v && problem.find("maybe") != std::string::npos, "invalid keeps on and names the text");
		}

		TEST_METHOD(FallbackEntryEngagesOnlyWhileARepeatWasDropped)
		{
			using FallbackCatalog::GrainRepeatGate;
			Require(!GrainRepeatGate(true, 0, 8, 2).engaged, "nothing dropped: released");
			Require(!GrainRepeatGate(false, 3, 0, 2).engaged, "setting off: released");
			const FallbackCatalog::Decision d = GrainRepeatGate(true, 4, 0, 2);
			Require(d.engaged && d.id == "grain_repeat_gate" && d.reason.find("L1+L2") != std::string::npos, "frozen: engaged, says the L1+L2 gains run");
			const FallbackCatalog::Decision e = GrainRepeatGate(true, 1, 7, 2);
			Require(e.engaged && e.reason.find("still average") != std::string::npos, "one dropped, seven left: engaged, the rest average");
		}
	};
}
