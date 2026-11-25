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
			Require(w.back == 10 && w.ahead == 0, "low latency with the per-pixel vector product: 10 back");

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

		// The 8x8 DCT used by the B Wiener is orthonormal: round trip and Parseval.
		TEST_METHOD(DctRoundTripAndParseval)
		{
			std::mt19937 rng(5);
			std::normal_distribution<double> nd(0.0, 3.0);
			double in[64], co[64], back[64], e0 = 0.0, e1 = 0.0, err = 0.0;
			for (int i = 0; i < 64; ++i) { in[i] = nd(rng); e0 += in[i] * in[i]; }
			GrainLevel::Dct8x8(in, co);
			for (int i = 0; i < 64; ++i) e1 += co[i] * co[i];
			GrainLevel::Idct8x8(co, back);
			for (int i = 0; i < 64; ++i) err = (std::max)(err, std::fabs(back[i] - in[i]));
			Require(err < 1e-9, "inverse DCT restores the tile");
			Require(std::fabs(e1 - e0) < 1e-9 * e0, "orthonormal: energy preserved");
		}

		// B with K >= 2: planted independent grain on a static structure. The neighbour Wiener must run (not the fallback) and the clean
		// band must be closer to the true structure band than the raw band is, while keeping the structure energy.
		TEST_METHOD(NeighbourWienerDenoisesWithManyNeighbours)
		{
			const GrainSplit::Split cur = Split(Frame(0.0, 2.0, 200));
			const Plane truth = GrainLevel::BandD(Split(Frame(0.0, 0.0, 0)));
			GrainLevel::BlockField f; f.Init(kW, kH, kB, 0.0, 0.0);
			std::vector<Plane> bands;
			for (int k = 1; k <= 4; ++k) bands.push_back(GrainLevel::BandD(Split(Frame(0.0, 2.0, 200 + k))));
			std::vector<GrainLevel::Neighbour> nb(4);
			for (int k = 0; k < 4; ++k) { nb[k].d = &bands[static_cast<size_t>(k)]; nb[k].field = f; nb[k].epoch = 0; nb[k].offset = -(k + 1); }
			GrainLevel::Estimator est; GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;
			est.Step(Split(Frame(0.0, 2.0, 199)), c);
			const GrainLevel::Result t1 = est.Step(cur, c);
			const GrainLevel::Params p;
			const GrainLevel::NeighbourResult r = GrainLevel::NeighbourWiener(cur, t1, nb, 0, GrainLevel::WindowFor(p, false, false, 0), p);
			const Plane raw = GrainLevel::BandD(cur);
			double eRaw = 0.0, eClean = 0.0, eTruth = 0.0, eClean2 = 0.0; long n = 0;
			for (int y = 2 * kB; y < kH - 2 * kB; ++y)
				for (int x = 2 * kB; x < kW - 2 * kB; ++x)
				{
					const double a = raw.at(x, y) - truth.at(x, y), b = r.cleanD.at(x, y) - truth.at(x, y);
					eRaw += a * a; eClean += b * b; eTruth += truth.at(x, y) * truth.at(x, y); eClean2 += r.cleanD.at(x, y) * r.cleanD.at(x, y); ++n;
				}
			Log("used " + std::to_string(r.usedNeighbours) + " fallback " + std::to_string(r.fallbackPixels) + "/" + std::to_string(r.pixels)
				+ " meanK " + std::to_string(r.meanKeff) + " white PSD " + std::to_string(r.psdWhite ? 1 : 0)
				+ " err raw " + std::to_string(eRaw / n) + " err clean " + std::to_string(eClean / n) + " structure kept " + std::to_string(eClean2 / eTruth));
			Require(r.usedNeighbours == 4, "4 neighbours used");
			Require(r.fallbackPixels < r.pixels / 4, "most pixels take the neighbour path");
			Require(r.meanKeff >= 2.0, "at least 2 neighbours kept on average");
			Require(eClean < 0.5 * eRaw, "clean band is much closer to the true structure than the raw band");
			Require(eClean2 > 0.5 * eTruth, "structure energy kept");
		}

		// AR(1) grain (coefficient 0.3 along x) is allowed to LOWER NLF_t; the law reports it and N never rises (brief s5 row).
		TEST_METHOD(Ar1GrainIsReportedNotHidden)
		{
			GrainLevel::BlockField f; f.Init(kW, kH, kB, -1.0, 0.0);
			GrainLevel::Estimator est; GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;
			GrainLevel::Result o; GrainSplit::Split u;
			for (int t = 0; t <= 6; ++t)
			{
				Plane y = Frame(1.0 * t, 0.0, 0);
				std::mt19937 rng(300 + t);
				std::normal_distribution<double> nd(0.0, 2.0);
				for (int yy = 0; yy < kH; ++yy)
				{
					double prev = 0.0;
					for (int x = 0; x < kW; ++x) { prev = 0.3 * prev + nd(rng); y.at(x, yy) += prev; }
				}
				u = Split(y); o = est.Step(u, c);
			}
			bool lowered = false;
			for (size_t i = 0; i < u.N.size(); ++i) { Require(o.Nb[i] <= u.N[i] + 1e-12, "N never rises"); if (o.Nb[i] < u.N[i]) lowered = true; }
			Log("AR(1) 0.3: N lowered by NLF_t = " + std::to_string(lowered ? 1 : 0) + " flag " + std::to_string(o.nlfTLowersN ? 1 : 0));
			Require(lowered == o.nlfTLowersN, "the lowering is reported exactly");
		}

		// Cost ledger: every step states its 4K cost; the budget share stays unset until the budget exists.
		TEST_METHOD(CostLedgerStatesFourKCosts)
		{
			int n = 0;
			const GrainLevel::CostRow* rows = GrainLevel::CostRows(n);
			Require(n >= 7, "rows present (incl. both low-latency rows)");
			for (int i = 0; i < n; ++i)
			{
				Require(rows[i].ms4kHi >= rows[i].ms4kLo && rows[i].source && rows[i].step, "row complete: " + std::string(rows[i].step));
				Require(GrainLevel::BudgetShare(rows[i], 0.0) < 0.0, "budget share unset without a budget");
			}
			Require(rows[4].ms4kLo == 2.5 && rows[4].ms4kHi == 3.9, "B 8+4 row = design-grain-chain-cost row 8 band");
			Log("share of the owner 4.5 ms budget: B 8+4 upper " + std::to_string(GrainLevel::BudgetShare(rows[4], GrainLevel::kOwnerBudget4kMs)));
		}

		// GS 157 (L#4 trace test, test-only; does not code GS 215): synthetic I-B-B-B GOP grain pulse (20 frames, GOP = 4)
		// on a flat + textured 10-bit plane with independent per-frame grain seeds across CRF 24 and CRF 32 starvation
		// regimes, plus same-input uniform-grain baseline negative controls. Logs per-frame N_unit4, NLF_t, N_b, flat
		// grain-to-clean leak, and the exact I-frame NLF_t / N_unit4 under-cap ratio for offline GS 215 evaluation.
		TEST_METHOD(Gs157IvsBFrameGrainPulseTracesNUnit4AndNlfT)
		{
			constexpr int kFrames = 20;
			constexpr int kGop = 4;
			constexpr double kBaseLuma = 448.0;            // centre of 10-bit NLF bin 3 ([384, 512))
			constexpr double kSigma0 = 5.0;                // nominal 10-bit film grain std dev (variance 25.0 codes^2)
			constexpr double kSourceHz = 24.0;
			constexpr double kMaxCodeSq = 1024.0 * 1024.0; // physical 10-bit squared-code upper bound
			constexpr int kFlatX0 = kB, kFlatX1 = kW - kB;
			constexpr int kFlatY0 = kB, kFlatY1 = 3 * kB;  // y in [16, 48): 12+ px clear of the ramp at y in (60, 68)
			constexpr int kMaxBins = 64;

			// Top half (y <= 60): pure flat midtone; smooth cubic ramp on (60, 68); bottom half (y >= 68): translating texture.
			// Pure horizontal pan (shiftX = 1.0 * t, vx = -1.0, vy = 0.0) keeps the flat/textured split stationary across all 20 frames.
			auto texMask = [](int y) -> double
			{
				if (y <= 60) return 0.0;
				if (y >= 68) return 1.0;
				const double u = static_cast<double>(y - 60) / 8.0;
				return u * u * (3.0 - 2.0 * u);
			};

			auto makeFlatTexFrame = [&](double shiftX, double sigma, uint32_t seed) -> Plane
			{
				Plane p(kW, kH);
				std::mt19937 rng(seed);
				std::normal_distribution<double> nd(0.0, 1.0);
				for (int y = 0; y < kH; ++y)
				{
					const double m = texMask(y);
					for (int x = 0; x < kW; ++x)
					{
						const double xs = static_cast<double>(x) - shiftX;
						const double tex = 20.0 * std::sin(0.30 * xs + 0.20 * static_cast<double>(y))
							+ 12.0 * std::sin(0.17 * xs - 0.23 * static_cast<double>(y));
						p.at(x, y) = kBaseLuma + m * tex + (sigma > 0.0 ? sigma * nd(rng) : 0.0);
					}
				}
				return p;
			};

			struct TraceSummary
			{
				double meanI_Nunit4 = 0.0, meanB_Nunit4 = 0.0;
				double meanI_NlfT = 0.0, meanB_NlfT = 0.0;
				double meanI_Nb = 0.0, meanB_Nb = 0.0;
				double meanI_NlfOverNunit4 = 0.0;  // mean over temporal I-frames of per-frame (NLF_t / N_unit4)
				double ratioI_NlfOverNunit4 = 0.0; // meanI_NlfT / meanI_Nunit4
				double meanI_FlatLeakV5 = 0.0, meanB_FlatLeakV5 = 0.0;
				double meanI_FlatLeakU4 = 0.0, meanB_FlatLeakU4 = 0.0;
				int temporalIFrames = 0, temporalBFrames = 0;
			};

			auto runRegime = [&](const std::string& tag, double sigmaI, double sigmaB, uint32_t seedBase) -> TraceSummary
			{
				GrainLevel::BlockField field;
				field.Init(kW, kH, kB, -1.0, 0.0);
				GrainLevel::Estimator est;
				const GrainLevel::Params& p = est.Parameters();
				const int bins = p.split.levelBins;
				Require(bins > 0 && bins <= kMaxBins, tag + ": levelBins in supported stack range");
				const double cs = GrainSplit::DepthScale(p.split.sourceBits), es = cs * cs;
				GrainLevel::Context ctx;
				ctx.sourceHz = kSourceHz;
				ctx.field = &field;

				TraceSummary s;
				for (int t = 0; t < kFrames; ++t)
				{
					const bool isI = (t % kGop) == 0;
					const double sigma = isI ? sigmaI : sigmaB;
					const Plane y = makeFlatTexFrame(static_cast<double>(t), sigma, seedBase + static_cast<uint32_t>(t));
					const GrainSplit::Split u = Split(y);
					const GrainLevel::Result o = est.Step(u, ctx);

					if (t == 0)
					{
						Require(o.mode == GrainLevel::Mode::SingleFrame && o.reason == GrainLevel::Reason::NoPrevious,
							tag + ": frame 0 is single-frame (no previous)");
					}
					else
					{
						Require(o.mode == GrainLevel::Mode::Temporal && o.reason == GrainLevel::Reason::None,
							tag + ": temporal mode active for t=" + std::to_string(t));
						Require(o.regionsTrusted > 0 && o.skipRegions == 0,
							tag + ": trusted regions present and skipGuard inactive on 1 px pan at t=" + std::to_string(t));
					}

					// Per-pixel conservation & bounds check.
					double maxYErr = 0.0;
					for (size_t i = 0; i < u.f.v.size(); ++i)
					{
						Require(std::isfinite(o.f.v[i]) && o.f.v[i] >= -1e-12 && o.f.v[i] <= 1.0 + 1e-12,
							tag + ": f finite and in [0, 1] at t=" + std::to_string(t));
						Require(std::isfinite(o.gate.v[i]) && o.gate.v[i] >= -1e-12 && o.gate.v[i] <= 1.0 + 1e-12,
							tag + ": gate finite and in [0, 1] at t=" + std::to_string(t));
						Require(o.f.v[i] >= u.f.v[i] - 1e-12,
							tag + ": f' >= f (mirror gate off) at t=" + std::to_string(t));
						maxYErr = (std::max)(maxYErr, std::fabs((o.clean.v[i] + o.grain.v[i]) - y.v[i]));
					}
					Require(maxYErr < 1e-9, tag + ": clean + grain reproduces Y exactly at t=" + std::to_string(t));

					// Per-block N_unit4, NLF_t, and N_b invariants.
					bool validB[kMaxBins] = {};
					if (t > 0)
					{
						Require(static_cast<int>(o.levelT.size()) == bins && static_cast<int>(o.validT.size()) == bins,
							tag + ": levelT/validT sized to levelBins at t=" + std::to_string(t));
						for (int j = 0; j < bins; ++j)
						{
							validB[j] = o.validT[static_cast<size_t>(j)] != 0;
							if (validB[j])
							{
								const double lv = o.levelT[static_cast<size_t>(j)];
								Require(std::isfinite(lv) && lv >= 0.0 && lv < kMaxCodeSq,
									tag + ": levelT finite, non-negative, bounded at t=" + std::to_string(t));
							}
						}
					}

					double sumN4 = 0.0, sumNlfT = 0.0, sumNb = 0.0;
					const size_t nb = u.N.size();
					Require(o.Nb.size() == nb && o.retained.size() == nb && nb > 0, tag + ": block arrays sized");
					for (int by = 0; by < u.bh; ++by)
						for (int bx = 0; bx < u.bw; ++bx)
						{
							const size_t i = static_cast<size_t>(by) * static_cast<size_t>(u.bw) + static_cast<size_t>(bx);
							const double n4 = u.N[i];
							const double nbVal = o.Nb[i];
							Require(std::isfinite(n4) && n4 >= 0.0 && n4 < kMaxCodeSq,
								tag + ": N_unit4 finite, non-negative, bounded at t=" + std::to_string(t));
							Require(std::isfinite(nbVal) && nbVal >= 0.0 && nbVal < kMaxCodeSq,
								tag + ": N_b finite, non-negative, bounded at t=" + std::to_string(t));
							Require(std::isfinite(o.retained[i]) && o.retained[i] >= 0.0 && o.retained[i] < kMaxCodeSq,
								tag + ": retained finite, non-negative, bounded at t=" + std::to_string(t));
							Require(nbVal <= n4 + 1e-6,
								tag + ": N_b never over-estimates above N_unit4 at t=" + std::to_string(t));

							sumN4 += n4;
							sumNb += nbVal;
							if (t > 0)
							{
								const double luma = u.base.at((std::min)(bx * u.B + u.B / 2, u.w - 1), (std::min)(by * u.B + u.B / 2, u.h - 1));
								double cap = 0.0;
								const bool hasCap = GrainSplit::GrainLevelCap(o.levelT.data(), validB, p.split, luma / cs, cap);
								Require(hasCap, tag + ": NLF_t cap valid on every block at t=" + std::to_string(t));
								const double nlfBlock = cap * es;
								Require(std::isfinite(nlfBlock) && nlfBlock >= 0.0 && nlfBlock < kMaxCodeSq,
									tag + ": NLF_t finite, non-negative, bounded at t=" + std::to_string(t));
								const double expectedNb = (std::min)(n4, p.ct * nlfBlock);
								Require(std::fabs(nbVal - expectedNb) <= 1e-12,
									tag + ": N_b == min(N_unit4, c_t * NLF_t) at t=" + std::to_string(t));
								sumNlfT += nlfBlock;
							}
						}

					const double meanN4 = sumN4 / static_cast<double>(nb);
					const double meanNb = sumNb / static_cast<double>(nb);
					const double meanNlfT = t > 0 ? sumNlfT / static_cast<double>(nb) : 0.0;

					// Flat-interior grain-to-clean leak: on y in [16, 48), true structure is constant kBaseLuma (D_struct = 0),
					// so D = d1 + d2 is pure planted grain and s1 + s2 = f * D is the exact grain energy leaking into clean.
					double sumD2 = 0.0, sumCleanU4_2 = 0.0, sumCleanV5_2 = 0.0;
					for (int py = kFlatY0; py < kFlatY1; ++py)
						for (int px = kFlatX0; px < kFlatX1; ++px)
						{
							const double d = u.d1.at(px, py) + u.d2.at(px, py);
							const double cu4 = u.s1.at(px, py) + u.s2.at(px, py);
							const double cv5 = o.s1.at(px, py) + o.s2.at(px, py);
							sumD2 += d * d;
							sumCleanU4_2 += cu4 * cu4;
							sumCleanV5_2 += cv5 * cv5;
						}
					Require(sumD2 > 0.0, tag + ": planted grain energy positive on flat interior at t=" + std::to_string(t));
					const double flatLeakU4 = sumCleanU4_2 / sumD2;
					const double flatLeakV5 = sumCleanV5_2 / sumD2;
					Require(std::isfinite(flatLeakU4) && flatLeakU4 >= 0.0 && flatLeakU4 <= 1.0 + 1e-12,
						tag + ": u4 flat grain-to-clean leak in [0, 1] at t=" + std::to_string(t));
					Require(std::isfinite(flatLeakV5) && flatLeakV5 >= 0.0 && flatLeakV5 <= 1.0 + 1e-12,
						tag + ": v5 flat grain-to-clean leak in [0, 1] at t=" + std::to_string(t));

					char line[320];
					if (t == 0)
					{
						std::snprintf(line, sizeof(line),
							"GS157 [%s] t=%2d (%c) sigma=%.3f N_unit4=%7.3f NLF_t=    N/A N_b=%7.3f NLF_t/N_u4=  N/A flatLeakV5=%.4f flatLeakU4=%.4f lowered=%d",
							tag.c_str(), t, isI ? 'I' : 'B', sigma, meanN4, meanNb, flatLeakV5, flatLeakU4, o.nlfTLowersN ? 1 : 0);
					}
					else
					{
						const double ratio = meanN4 > 0.0 ? meanNlfT / meanN4 : 0.0;
						std::snprintf(line, sizeof(line),
							"GS157 [%s] t=%2d (%c) sigma=%.3f N_unit4=%7.3f NLF_t=%7.3f N_b=%7.3f NLF_t/N_u4=%.3f flatLeakV5=%.4f flatLeakU4=%.4f lowered=%d",
							tag.c_str(), t, isI ? 'I' : 'B', sigma, meanN4, meanNlfT, meanNb, ratio, flatLeakV5, flatLeakU4, o.nlfTLowersN ? 1 : 0);

						if (isI)
						{
							s.meanI_Nunit4 += meanN4;
							s.meanI_NlfT += meanNlfT;
							s.meanI_Nb += meanNb;
							s.meanI_NlfOverNunit4 += ratio;
							s.meanI_FlatLeakV5 += flatLeakV5;
							s.meanI_FlatLeakU4 += flatLeakU4;
							++s.temporalIFrames;
						}
						else
						{
							s.meanB_Nunit4 += meanN4;
							s.meanB_NlfT += meanNlfT;
							s.meanB_Nb += meanNb;
							s.meanB_FlatLeakV5 += flatLeakV5;
							s.meanB_FlatLeakU4 += flatLeakU4;
							++s.temporalBFrames;
						}
					}
					Log(line);
				}

				Require(s.temporalIFrames == 4 && s.temporalBFrames == 15, tag + ": expected 4 temporal I-frames and 15 temporal B-frames");
				s.meanI_Nunit4 /= static_cast<double>(s.temporalIFrames);
				s.meanI_NlfT /= static_cast<double>(s.temporalIFrames);
				s.meanI_Nb /= static_cast<double>(s.temporalIFrames);
				s.meanI_NlfOverNunit4 /= static_cast<double>(s.temporalIFrames);
				s.meanI_FlatLeakV5 /= static_cast<double>(s.temporalIFrames);
				s.meanI_FlatLeakU4 /= static_cast<double>(s.temporalIFrames);
				s.meanB_Nunit4 /= static_cast<double>(s.temporalBFrames);
				s.meanB_NlfT /= static_cast<double>(s.temporalBFrames);
				s.meanB_Nb /= static_cast<double>(s.temporalBFrames);
				s.meanB_FlatLeakV5 /= static_cast<double>(s.temporalBFrames);
				s.meanB_FlatLeakU4 /= static_cast<double>(s.temporalBFrames);
				s.ratioI_NlfOverNunit4 = s.meanI_Nunit4 > 0.0 ? s.meanI_NlfT / s.meanI_Nunit4 : 0.0;

				char sumLine[360];
				std::snprintf(sumLine, sizeof(sumLine),
					"GS157 SUMMARY [%s] meanI_N_u4=%.3f meanB_N_u4=%.3f (B/I=%.3f) | I_NLF_t=%.3f I_N_b=%.3f | GS215_I_undercap(mean_ratio=%.4f, meanNLF/meanNu4=%.4f) | flatLeakV5(I=%.4f, B=%.4f) flatLeakU4(I=%.4f, B=%.4f)",
					tag.c_str(), s.meanI_Nunit4, s.meanB_Nunit4, s.meanB_Nunit4 / s.meanI_Nunit4,
					s.meanI_NlfT, s.meanI_Nb, s.meanI_NlfOverNunit4, s.ratioI_NlfOverNunit4,
					s.meanI_FlatLeakV5, s.meanB_FlatLeakV5, s.meanI_FlatLeakU4, s.meanB_FlatLeakU4);
				Log(sumLine);
				return s;
			};

			// CRF 24: I-frames keep 48% of nominal grain variance, B-frames starved to 13% variance; plus same-input constant-grain control.
			const double crf24SigmaI = std::sqrt(0.48) * kSigma0;
			const double crf24SigmaB = std::sqrt(0.13) * kSigma0;
			const TraceSummary crf24Pulse = runRegime("CRF24-IBBB", crf24SigmaI, crf24SigmaB, 15700u);
			const TraceSummary crf24Ctrl = runRegime("CRF24-UNIFORM-CTRL", crf24SigmaI, crf24SigmaI, 15700u);

			// CRF 32: stronger B-frame starvation (I-frames 24% variance, B-frames 3% variance); plus same-input constant-grain control.
			const double crf32SigmaI = std::sqrt(0.24) * kSigma0;
			const double crf32SigmaB = std::sqrt(0.03) * kSigma0;
			const TraceSummary crf32Pulse = runRegime("CRF32-IBBB", crf32SigmaI, crf32SigmaB, 15732u);
			const TraceSummary crf32Ctrl = runRegime("CRF32-UNIFORM-CTRL", crf32SigmaI, crf32SigmaI, 15732u);

			// Exact same-input proof: I-frames are bit-identical between each pulse and its uniform-grain control.
			Require(std::fabs(crf24Pulse.meanI_Nunit4 - crf24Ctrl.meanI_Nunit4) < 1e-12,
				"CRF24 same-input control: I-frame N_unit4 matches bit-for-bit");
			Require(std::fabs(crf32Pulse.meanI_Nunit4 - crf32Ctrl.meanI_Nunit4) < 1e-12,
				"CRF32 same-input control: I-frame N_unit4 matches bit-for-bit");

			// CRF 24 physical & trace invariants + negative control contrast.
			Require(crf24Pulse.meanB_Nunit4 < 0.5 * crf24Pulse.meanI_Nunit4,
				"CRF24 pulse: N_unit4 on starved B-frames < 0.5 * N_unit4 on I-frames");
			Require(crf24Ctrl.meanB_Nunit4 >= 0.85 * crf24Ctrl.meanI_Nunit4 && crf24Ctrl.meanB_Nunit4 <= 1.15 * crf24Ctrl.meanI_Nunit4,
				"CRF24 negative control: uniform grain keeps N_unit4 steady across I and B slots (proves B-frame drop is real)");
			Require(crf24Pulse.meanI_Nb < crf24Pulse.meanI_Nunit4,
				"CRF24 pulse: B-frame-pulled NLF_t caps I-frame N_b below N_unit4");
			Require(crf24Pulse.meanI_NlfOverNunit4 < 0.5 * crf24Ctrl.meanI_NlfOverNunit4,
				"CRF24 pulse vs uniform control: B-frame starvation pulls I-frame NLF_t/N_unit4 below half of uniform-grain control");
			Require(crf24Pulse.meanI_FlatLeakV5 > crf24Pulse.meanI_FlatLeakU4,
				"CRF24 pulse: I-frame NLF_t under-cap increases flat grain-to-clean leak over unit-4");
			Require(crf24Pulse.meanI_FlatLeakV5 > crf24Ctrl.meanI_FlatLeakV5,
				"CRF24 pulse vs uniform control: I-frame flat grain-to-clean leak exceeds uniform-grain control");

			// CRF 32 physical & trace invariants + negative control contrast.
			Require(crf32Pulse.meanB_Nunit4 < 0.5 * crf32Pulse.meanI_Nunit4,
				"CRF32 pulse: N_unit4 on starved B-frames < 0.5 * N_unit4 on I-frames");
			Require(crf32Ctrl.meanB_Nunit4 >= 0.85 * crf32Ctrl.meanI_Nunit4 && crf32Ctrl.meanB_Nunit4 <= 1.15 * crf32Ctrl.meanI_Nunit4,
				"CRF32 negative control: uniform grain keeps N_unit4 steady across I and B slots (proves B-frame drop is real)");
			Require(crf32Pulse.meanI_Nb < crf32Pulse.meanI_Nunit4,
				"CRF32 pulse: B-frame-pulled NLF_t caps I-frame N_b below N_unit4");
			Require(crf32Pulse.meanI_NlfOverNunit4 < 0.5 * crf32Ctrl.meanI_NlfOverNunit4,
				"CRF32 pulse vs uniform control: B-frame starvation pulls I-frame NLF_t/N_unit4 below half of uniform-grain control");
			Require(crf32Pulse.meanI_FlatLeakV5 > crf32Pulse.meanI_FlatLeakU4,
				"CRF32 pulse: I-frame NLF_t under-cap increases flat grain-to-clean leak over unit-4");
			Require(crf32Pulse.meanI_FlatLeakV5 > crf32Ctrl.meanI_FlatLeakV5,
				"CRF32 pulse vs uniform control: I-frame flat grain-to-clean leak exceeds uniform-grain control");

			// Cross-regime monotonicity: CRF 32 exhibits strictly stronger B-frame starvation and I-frame under-cap than CRF 24.
			Require((crf32Pulse.meanB_Nunit4 / crf32Pulse.meanI_Nunit4) < (crf24Pulse.meanB_Nunit4 / crf24Pulse.meanI_Nunit4),
				"CRF32 shows strictly stronger B-frame N_unit4 starvation than CRF24");
			Require(crf32Pulse.meanI_NlfOverNunit4 < crf24Pulse.meanI_NlfOverNunit4,
				"CRF32 shows strictly stronger I-frame NLF_t/N_unit4 under-cap than CRF24");
		}
	};
}
