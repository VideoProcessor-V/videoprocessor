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
			Require(l5 < 0.25, "coherent translating texture keeps its structure (leak < 25 percent)");
		}

		// Mirror gate: ON it fires on incoherent (pure-grain) pairs and only lowers the share; OFF it is exactly the same as an ON gate
		// whose energy threshold can never be met (no detail moves). On coherent translating structure the ON gate costs little detail.
		TEST_METHOD(MirrorGateMovesOnlyIncoherentRegions)
		{
			GrainLevel::BlockField f; f.Init(kW, kH, kB, -1.0, 0.0);
			GrainLevel::Params pOff; pOff.mirrorGate = false;
			GrainLevel::Params pOn; pOn.mirrorGate = true;
			GrainLevel::Params pNever; pNever.mirrorGate = true; pNever.mirrorMinEnergy = 1e9;
			GrainLevel::Estimator eOff(pOff), eOn(pOn), eNever(pNever);
			GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;
			GrainLevel::Result rOff, rOn, rNever;
			// pure grain, no structure: the pair is incoherent (rho ~ 0)
			for (int t = 0; t <= 4; ++t)
			{
				Plane y(kW, kH);
				std::mt19937 rng(400 + t);
				std::normal_distribution<double> nd(0.0, 3.0);
				for (int yy = 0; yy < kH; ++yy) for (int x = 0; x < kW; ++x) y.at(x, yy) = 400.0 + nd(rng);
				const GrainSplit::Split u = GrainSplit::Compute(y, 1.0, kB);
				rOff = eOff.Step(u, c); rOn = eOn.Step(u, c); rNever = eNever.Step(u, c);
			}
			double moved = 0.0, up = 0.0, neverMoved = 0.0;
			for (size_t i = 0; i < rOff.f.v.size(); ++i)
			{
				moved += rOff.f.v[i] - rOn.f.v[i]; up = (std::max)(up, rOn.f.v[i] - rOff.f.v[i]);
				neverMoved += std::fabs(rOff.f.v[i] - rNever.f.v[i]);
			}
			Log("mirror regions on " + std::to_string(rOn.mirrorRegions) + " total share lowered " + std::to_string(moved));
			Require(rOn.mirrorRegions > 0, "mirror ON fires on the incoherent pure-grain pair");
			Require(moved > 0.0, "mirror ON lowers the share on the incoherent pair");
			Require(rOff.mirrorRegions == 0, "switch off: no region counted");
			Require(neverMoved == 0.0, "OFF equals an ON gate that cannot fire: no detail moved");
			Require(up <= 1e-12, "ON never raises the share");

			// coherent translating texture: the ON gate costs little detail
			GrainLevel::Estimator a(pOff), b(pOn);
			a.Step(GrainSplit::Compute(Frame(0.0, 2.0, 501), 1.0, kB), c); b.Step(GrainSplit::Compute(Frame(0.0, 2.0, 501), 1.0, kB), c);
			GrainLevel::Result ra, rb; Plane s;
			for (int t = 1; t <= 6; ++t)
			{
				const GrainSplit::Split u = GrainSplit::Compute(Frame(1.0 * t, 2.0, 501 + t), 1.0, kB);
				ra = a.Step(u, c); rb = b.Step(u, c);
				s = GrainLevel::BandD(GrainSplit::Compute(Frame(1.0 * t, 0.0, 0), 1.0, kB));
			}
			const double la = GrainLevel::FrozenLeak(ra.f, s, 2 * kB), lb = GrainLevel::FrozenLeak(rb.f, s, 2 * kB);
			Log("detail loss texture, mirror off " + std::to_string(la) + " on " + std::to_string(lb));
			Require(lb - la < 0.10, "mirror ON adds < 10 points of leak on coherent structure");
		}
	};
}
