// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// repair.grain_split GrainLevel (CPU law v5, brief-S-3c T2): L1 NLF_t, L2 coherence gate, guards, resets, rate-aware forgetting,
// the visible single-frame fallback, the B 8+4 window and neighbour Wiener bookkeeping. CPU only, deterministic (fixed seeds).
// Every assertion is analytic or a structural identity; measured numbers are only logged (study rows belong to T5).

#include <VP-V/modules/repair/grain_split/GrainLevel.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainLevelLawTestsNs
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

	// Smooth structure translated by shiftX px (band-limited: bicubic warps it almost exactly).
	inline double Struct(double x, double y, double shiftX)
	{
		const double xs = x - shiftX;
		return 400.0 + 25.0 * std::sin(0.30 * xs + 0.20 * y) + 15.0 * std::sin(0.17 * xs - 0.23 * y);
	}
	inline Plane Frame(double shiftX, double sigma, uint32_t seed)
	{
		Plane p(kW, kH);
		std::mt19937 rng(seed);
		std::normal_distribution<double> nd(0.0, 1.0);
		for (int y = 0; y < kH; ++y)
			for (int x = 0; x < kW; ++x) p.at(x, y) = Struct(x, y, shiftX) + (sigma > 0.0 ? sigma * nd(rng) : 0.0);
		return p;
	}
	inline GrainSplit::Split Split(const Plane& y) { return GrainSplit::Compute(y, 1.0, kB); }

	inline bool Same(const Plane& a, const Plane& b)
	{
		if (a.w != b.w || a.h != b.h) return false;
		for (size_t i = 0; i < a.v.size(); ++i) if (a.v[i] != b.v[i]) return false;
		return true;
	}

	TEST_CLASS(GrainLevelLawTests)
	{
	public:
		// Forgetting is in SECONDS: after halfLife x rate frames exactly half remains, at every measured rate (rule 6).
		TEST_METHOD(HalfLifeIsSecondsAtEveryRate)
		{
			const double rates[] = { 23.976, 25.0, 29.97, 50.0, 59.94, 120.0 };
			for (double hz : rates)
			{
				const double d = GrainLevel::DecayPerFrame(0.5, hz);
				const double half = std::pow(d, GrainLevel::HalfLifeFrames(0.5, hz));
				Log("rate " + std::to_string(hz) + " decay " + std::to_string(d) + " frames " + std::to_string(GrainLevel::HalfLifeFrames(0.5, hz)));
				Require(std::fabs(half - 0.5) < 1e-9, "half remains after the half-life at " + std::to_string(hz) + " Hz");
			}
			Require(GrainLevel::DecayPerFrame(0.5, 0.0) == 0.0, "unknown rate forgets everything (no assumed rate)");
		}

		// Cuts, fades, an absent engine, a missing previous frame and an untrusted pair are the unit-4 law EXACTLY, visibly.
		TEST_METHOD(FallbackIsUnit4BitIdentical)
		{
			const Plane y0 = Frame(0.0, 2.0, 11), y1 = Frame(1.0, 2.0, 12);
			const GrainSplit::Split a = Split(y0), b = Split(y1);
			GrainLevel::BlockField f; f.Init(kW, kH, kB, -1.0, 0.0);

			struct Row { GrainLevel::Reset reset; bool fading, engine, pair; GrainLevel::Reason want; const char* name; };
			const Row rows[] = {
				{ GrainLevel::Reset::Cut, false, true, true, GrainLevel::Reason::Cut, "cut" },
				{ GrainLevel::Reset::FadeStart, false, true, true, GrainLevel::Reason::FadeStart, "fade start" },
				{ GrainLevel::Reset::MotionEpoch, false, true, true, GrainLevel::Reason::MotionEpoch, "motion epoch" },
				{ GrainLevel::Reset::SettingsChange, false, true, true, GrainLevel::Reason::SettingsChange, "settings change" },
				{ GrainLevel::Reset::None, true, true, true, GrainLevel::Reason::Fading, "fading" },
				{ GrainLevel::Reset::None, false, false, true, GrainLevel::Reason::EngineOff, "engine off" },
				{ GrainLevel::Reset::None, false, true, false, GrainLevel::Reason::UntrustedPair, "untrusted pair" },
			};
			for (const Row& r : rows)
			{
				GrainLevel::Estimator est;
				est.Step(a, GrainLevel::Context());   // frame 0: no previous
				GrainLevel::Context c;
				c.sourceHz = 24.0; c.field = &f; c.reset = r.reset; c.fading = r.fading; c.engineOn = r.engine; c.pairTrusted = r.pair;
				const GrainLevel::Result o = est.Step(b, c);
				Require(o.mode == GrainLevel::Mode::SingleFrame, std::string(r.name) + ": single-frame mode");
				Require(o.reason == r.want, std::string(r.name) + ": visible reason " + GrainLevel::Token(o.reason));
				Require(Same(o.f, b.f) && Same(o.clean, b.clean) && Same(o.grain, b.grain) && Same(o.s1, b.s1) && Same(o.s2, b.s2), std::string(r.name) + ": planes bit identical to unit 4");
				Require(o.Nb == b.N, std::string(r.name) + ": N identical to unit 4");
			}
			GrainLevel::Estimator e0;
			const GrainLevel::Result first = e0.Step(a, GrainLevel::Context());
			Require(first.reason == GrainLevel::Reason::NoPrevious && Same(first.clean, a.clean), "first frame: no previous, unit-4 planes");
		}

		// Any reset empties the forgotten histogram.
		TEST_METHOD(ResetEmptiesTheAccumulator)
		{
			GrainLevel::BlockField f; f.Init(kW, kH, kB, -1.0, 0.0);
			GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;
			const GrainLevel::Reset causes[] = { GrainLevel::Reset::Cut, GrainLevel::Reset::FadeStart, GrainLevel::Reset::MotionEpoch, GrainLevel::Reset::SettingsChange };
			for (GrainLevel::Reset cz : causes)
			{
				GrainLevel::Estimator e2;
				for (int t = 0; t <= 3; ++t) e2.Step(Split(Frame(1.0 * t, 2.0, 31 + t)), c);
				Require(e2.EvidenceWeight() > 0.0, "evidence before reset");
				GrainLevel::Context rc = c; rc.reset = cz;
				e2.Step(Split(Frame(4.0, 2.0, 40)), rc);
				Require(e2.EvidenceWeight() == 0.0, "reset empties the histogram");
			}
		}

		// The skip-copy guard: a repeated frame (I_r = 0, v = 0) is excluded, g = 0 and counted; without the guard the same pair
		// would read as perfectly coherent (rho huge) and open the gate (why the guard is essential).
		TEST_METHOD(SkipCopyGuardExcludesRepeats)
		{
			const Plane y0 = Frame(0.0, 2.0, 51);
			const GrainSplit::Split a = Split(y0);
			GrainLevel::BlockField f; f.Init(kW, kH, kB, 0.0, 0.0);
			GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;

			GrainLevel::Estimator on;
			on.Step(a, GrainLevel::Context());
			const GrainLevel::Result g = on.Step(a, c);
			Require(g.mode == GrainLevel::Mode::Temporal, "temporal");
			Require(g.regionsTrusted > 0, "interior regions trusted");
			Require(g.skipRegions == g.regionsTrusted, "every trusted region of a repeat is a skip-copy region");
			Require(g.gatedRegions == 0, "no region is gated open");
			double gmax = 0.0; for (double v : g.gate.v) gmax = (std::max)(gmax, v);
			Require(gmax == 0.0, "gate is 0 everywhere");

			GrainLevel::Params p; p.skipGuard = false;
			GrainLevel::Estimator off(p);
			off.Step(a, GrainLevel::Context());
			const GrainLevel::Result o = off.Step(a, c);
			Require(o.gatedRegions > 0, "guard off: the repeat opens the gate (rho = C / floor)");
		}

		// Invariants of the temporal frame: N_b <= unit-4 N, f' >= f (mirror off), clean + grain = Y, g in [0, 1].
		TEST_METHOD(TemporalInvariants)
		{
			GrainLevel::BlockField f; f.Init(kW, kH, kB, -1.0, 0.0);
			GrainLevel::Estimator est;
			GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;
			est.Step(Split(Frame(0.0, 2.0, 61)), c);
			GrainLevel::Result o;
			GrainSplit::Split u;
			for (int t = 1; t <= 6; ++t) { u = Split(Frame(1.0 * t, 2.0, 61 + t)); o = est.Step(u, c); }
			Require(o.mode == GrainLevel::Mode::Temporal, "temporal");
			Require(o.version == GrainLevel::kLawVersion && GrainLevel::kLawVersion == 5, "law version 5");
			for (size_t i = 0; i < u.N.size(); ++i) Require(o.Nb[i] <= u.N[i] + 1e-12, "N_b <= N of unit 4 (c_t 1.0, min law)");
			double worst = 1.0, gmin = 1.0, gmax = 0.0, ysum = 0.0;
			for (size_t i = 0; i < u.f.v.size(); ++i)
			{
				worst = (std::min)(worst, o.f.v[i] - u.f.v[i]);
				gmin = (std::min)(gmin, o.gate.v[i]); gmax = (std::max)(gmax, o.gate.v[i]);
				ysum = (std::max)(ysum, std::fabs((o.clean.v[i] + o.grain.v[i]) - (u.clean.v[i] + u.grain.v[i])));
			}
			Log("min(f' - f) " + std::to_string(worst) + " gate [" + std::to_string(gmin) + ", " + std::to_string(gmax) + "] age " + std::to_string(o.ageFrames)
				+ " trusted " + std::to_string(o.trustedShare) + " skip " + std::to_string(o.skipRegions) + " gated " + std::to_string(o.gatedRegions));
			Require(worst >= -1e-12, "f' >= f everywhere with the mirror gate off");
			Require(gmin >= 0.0 && gmax <= 1.0, "gate within [0, 1]");
			Require(ysum < 1e-9, "clean + grain reproduces Y");
			Require(o.retained.size() == u.N.size(), "retained share reported per block");
		}

		// Mirror gate: behind its own switch, DEFAULT OFF; on, it can only lower f'.
		TEST_METHOD(MirrorGateOnlyLowersAndDefaultsOff)
		{
			Require(!GrainLevel::Params().mirrorGate, "mirror gate default OFF");
			GrainLevel::BlockField f; f.Init(kW, kH, kB, -1.0, 0.0);
			GrainLevel::Params pm; pm.mirrorGate = true;
			GrainLevel::Estimator off, on(pm);
			GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;
			off.Step(Split(Frame(0.0, 2.0, 71)), c); on.Step(Split(Frame(0.0, 2.0, 71)), c);
			GrainLevel::Result a, b;
			for (int t = 1; t <= 4; ++t) { const GrainSplit::Split u = Split(Frame(1.0 * t, 2.0, 71 + t)); a = off.Step(u, c); b = on.Step(u, c); }
			double maxUp = 0.0;
			for (size_t i = 0; i < a.f.v.size(); ++i) maxUp = (std::max)(maxUp, b.f.v[i] - a.f.v[i]);
			Log("mirror regions " + std::to_string(b.mirrorRegions) + " max f rise " + std::to_string(maxUp));
			Require(maxUp <= 1e-12, "mirror gate never raises the structure share");
			Require(a.mirrorRegions == 0, "switch off: no region counted");
		}

		// The cross form is unbiased by the sub-pixel phase: structure-only frames shifted by 0.3 and 1.7 px read ~0 non-repeating energy.
		TEST_METHOD(CrossFormPhaseUnbiased)
		{
			const double shifts[] = { 0.3, 1.7 };
			for (double s : shifts)
			{
				const Plane y0 = Frame(0.0, 0.0, 1), y1 = Frame(s, 0.0, 1);
				const GrainSplit::Split a = Split(y0), b = Split(y1);
				GrainLevel::BlockField f; f.Init(kW, kH, kB, -s, 0.0);
				const GrainLevel::Warped w = GrainLevel::WarpBicubic(GrainLevel::BandD(a), f, kW, kH);
				GrainLevel::Params p;
				const GrainLevel::Regions r = GrainLevel::CrossRegions(GrainLevel::BandD(b), w, b.base, f, b.N, nullptr, nullptr, p, kB);
				double e = 0.0, i = 0.0; int n = 0;
				for (size_t k = 0; k < r.I.size(); ++k) if (r.trusted[k]) { e += r.E[k]; i += r.I[k]; ++n; }
				Log("shift " + std::to_string(s) + " regions " + std::to_string(n) + " E " + std::to_string(e) + " I " + std::to_string(i));
				Require(n > 0, "trusted regions exist");
				Require(e > 0.0 && std::fabs(i) < 0.1 * e, "I_r is small against E_r at shift " + std::to_string(s));
			}
		}

		// The B window and the neighbour Wiener bookkeeping.
		TEST_METHOD(WindowAndNeighbourBookkeeping)
		{
			const GrainLevel::Params p;
			GrainLevel::Window w = GrainLevel::WindowFor(p, false, false, 10);
			Require(w.back == 8 && w.ahead == 4, "standard window 8 back + 4 ahead");
			w = GrainLevel::WindowFor(p, false, false, 2);
			Require(w.back == 8 && w.ahead == 2, "ahead never exceeds the real look-ahead");
			w = GrainLevel::WindowFor(p, true, false, 10);
			Require(w.back == 6 && w.ahead == 0, "low latency: 6 back, 0 ahead");
			w = GrainLevel::WindowFor(p, true, true, 10);
			Require(w.back == 10 && w.ahead == 0, "low latency with per-pixel composed vectors: 10 back");

			const Plane y0 = Frame(0.0, 2.0, 81), y1 = Frame(1.0, 2.0, 82);
			const GrainSplit::Split a = Split(y0), b = Split(y1);
			GrainLevel::BlockField f; f.Init(kW, kH, kB, -1.0, 0.0);
			GrainLevel::Estimator est; GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;
			est.Step(a, c);
			const GrainLevel::Result t1 = est.Step(b, c);
			const Plane da = GrainLevel::BandD(a);

			std::vector<GrainLevel::Neighbour> none;
			const GrainLevel::NeighbourResult r0 = GrainLevel::NeighbourWiener(b, t1, none, 0, GrainLevel::WindowFor(p, false, false, 4), p);
			Require(r0.fallbackPixels == r0.pixels, "no neighbours: every pixel takes the L1+L2 gains");
			const Plane db = GrainLevel::BandD(b);
			double err = 0.0, err2 = 0.0;
			for (int y = 0; y < kH; ++y) for (int x = 0; x < kW; ++x)
			{
				err = (std::max)(err, std::fabs(r0.cleanD.at(x, y) + r0.grainD.at(x, y) - db.at(x, y)));
				err2 = (std::max)(err2, std::fabs(r0.cleanD.at(x, y) - (t1.s1.at(x, y) + t1.s2.at(x, y))));
			}
			Require(err < 1e-9, "clean + grain = D");
			Require(err2 < 1e-12, "fallback pixels equal the L1+L2 clean band");

			std::vector<GrainLevel::Neighbour> nb(2);
			nb[0].d = &da; nb[0].field = f; nb[0].epoch = 1; nb[0].offset = -1;   // other epoch: dropped
			nb[1].d = &da; nb[1].field = f; nb[1].epoch = 0; nb[1].offset = -1;
			const GrainLevel::NeighbourResult r1 = GrainLevel::NeighbourWiener(b, t1, nb, 0, GrainLevel::WindowFor(p, false, false, 4), p);
			Require(r1.droppedCrossEpoch == 1 && r1.usedNeighbours == 1, "the other-epoch neighbour is dropped and counted");
			Require(r1.fallbackPixels == r1.pixels, "one neighbour is below minNeighbours: L1+L2 gains everywhere");
		}

		// Cost ledger: every step states its 4K cost; the budget share stays unset until the budget exists.
		TEST_METHOD(CostLedgerStatesFourKCosts)
		{
			int n = 0;
			const GrainLevel::CostRow* rows = GrainLevel::CostRows(n);
			Require(n >= 5, "rows present");
			for (int i = 0; i < n; ++i)
			{
				Require(rows[i].ms4kHi >= rows[i].ms4kLo && rows[i].source && rows[i].step, "row complete: " + std::string(rows[i].step));
				Require(GrainLevel::BudgetShare(rows[i], 0.0) < 0.0, "budget share unset without a budget");
			}
			Log("share of a 2 ms budget (illustrative only): B window " + std::to_string(GrainLevel::BudgetShare(rows[4], 2.0)));
		}
	};
}
