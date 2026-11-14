// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// repair.grain_split GrainLevel: detail loss with FROZEN gains (study proof method). The law must never push structure into the
// grain layer more than unit 4 does; the numbers on real film belong to T5 and are only logged here.

#include <VP-V/modules/repair/grain_split/GrainLevel.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainLevelDetailLossTestsNs
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

	inline Plane Frame(double shiftX, double sigma, uint32_t seed)
	{
		Plane p(kW, kH);
		std::mt19937 rng(seed);
		std::normal_distribution<double> nd(0.0, 1.0);
		for (int y = 0; y < kH; ++y)
			for (int x = 0; x < kW; ++x)
			{
				const double xs = x - shiftX;
				p.at(x, y) = 400.0 + 25.0 * std::sin(0.9 * xs) * std::sin(0.7 * y) + 15.0 * std::sin(0.31 * xs + 0.53 * y) + (sigma > 0.0 ? sigma * nd(rng) : 0.0);
			}
		return p;
	}

	TEST_CLASS(GrainLevelDetailLossTests)
	{
	public:
		// Frozen-gain leak of the temporal law is never above the unit-4 leak (f' >= f, mirror gate off); the values are logged.
		TEST_METHOD(TemporalLeakNotAboveUnit4)
		{
			GrainLevel::BlockField f; f.Init(kW, kH, kB, -1.0, 0.0);
			GrainLevel::Estimator est;
			GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;
			est.Step(GrainSplit::Compute(Frame(0.0, 2.0, 91), 1.0, kB), c);
			GrainLevel::Result o; GrainSplit::Split u; Plane s;
			for (int t = 1; t <= 6; ++t)
			{
				u = GrainSplit::Compute(Frame(1.0 * t, 2.0, 91 + t), 1.0, kB);
				o = est.Step(u, c);
				s = GrainLevel::BandD(GrainSplit::Compute(Frame(1.0 * t, 0.0, 0), 1.0, kB));   // structure-only band
			}
			const double l4 = GrainLevel::FrozenLeak(u.f, s, 2 * kB), l5 = GrainLevel::FrozenLeak(o.f, s, 2 * kB);
			Log("detail loss texture: unit-4 " + std::to_string(l4) + " v5 " + std::to_string(l5));
			Require(l5 <= l4 + 1e-12, "temporal leak <= unit-4 leak");
		}

		// Mirror gate off moves no detail: explicit OFF equals the default Params.
		TEST_METHOD(MirrorOffMovesNothing)
		{
			GrainLevel::BlockField f; f.Init(kW, kH, kB, -1.0, 0.0);
			GrainLevel::Params pOff; pOff.mirrorGate = false;
			GrainLevel::Estimator a(pOff), b;
			GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;
			a.Step(GrainSplit::Compute(Frame(0.0, 2.0, 101), 1.0, kB), c); b.Step(GrainSplit::Compute(Frame(0.0, 2.0, 101), 1.0, kB), c);
			GrainLevel::Result ra, rb;
			for (int t = 1; t <= 4; ++t) { const GrainSplit::Split u = GrainSplit::Compute(Frame(1.0 * t, 2.0, 101 + t), 1.0, kB); ra = a.Step(u, c); rb = b.Step(u, c); }
			Require(ra.f.v == rb.f.v && ra.clean.v == rb.clean.v, "explicit OFF equals default");
			Require(ra.mirrorRegions == 0, "no mirror region");
		}
	};
}
