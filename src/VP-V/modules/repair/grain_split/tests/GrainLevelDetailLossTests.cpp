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

#include <algorithm>
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

		// GS 153+208 (L#2 test-first): synthetic 0.5-px sub-pixel pan (exact fractional shift vx = 0.5, vy = 0.0) vs exact
		// integer 1.0-px pan baseline on high-frequency spatial texture (~0.27-0.32 cyc/px, inside D1 = Y - G(1.0)Y), where
		// the ONE shared Catmull-Rom bicubic sampler (VpComposed::SampleBicubic) attenuates HF amplitude by ~21%.
		// Measures and logs L1 (I_r, NLF_t), L2 (rho, gate, FrozenLeak, retained) and Method B (NeighbourWiener) HF texture
		// leak on:
		//   (a) clean grain-free HF texture (sigma = 0), and
		//   (b) grainy flat + HF textured plane (sigma = 2.0, 10-bit).
		// Bias gate (GS 153+208): if >~5% L1/L2 sub-pixel bias is confirmed at Build Slot runtime, the conditional Blue Team
		// projection-form switch (I_r = E_cur - C^2 / E_prev) can be evaluated while keeping ONE shared bicubic sampler
		// (never inventing a private Lanczos sampler).
		TEST_METHOD(SubPixelHalfPxPanMeasuresDetailLossAndMethodBTextureLeak)
		{
			// High-frequency texture (~0.27-0.32 cyc/px along x) inside D1 = Y - G(1.0)Y.
			// At 0.5-px shift, Catmull-Rom MTF is H(w) = (9/8)*cos(w/2) - (1/8)*cos(3w/2):
			//   H(2.0) ~= 0.732 (26.8% atten), H(1.7) ~= 0.846 (15.4% atten) -> ~21.4% mean HF attenuation.
			auto hfTex = [](double xs, int y) -> double
			{
				return 12.0 * std::sin(2.0 * xs) * std::sin(0.7 * y) + 8.0 * std::sin(1.7 * xs + 0.53 * y);
			};

			// Smooth rigid-panning mask for Part (b): 0.0 (pure flat) for xs <= 64, smooth cubic ramp on (64, 72),
			// 1.0 (pure HF texture) for xs >= 72. Translates rigidly with xs = x - shiftX so there is no seam shear.
			auto splitMask = [](double xs) -> double
			{
				if (xs <= 64.0) return 0.0;
				if (xs >= 72.0) return 1.0;
				const double u = (xs - 64.0) / 8.0;
				return u * u * (3.0 - 2.0 * u);
			};

			auto makeFrame = [&](double shiftX, double sigma, uint32_t seed, bool flatPlusTextured) -> Plane
			{
				Plane p(kW, kH);
				std::mt19937 rng(seed);
				std::normal_distribution<double> nd(0.0, 1.0);
				for (int y = 0; y < kH; ++y)
					for (int x = 0; x < kW; ++x)
					{
						const double xs = x - shiftX;
						const double m = flatPlusTextured ? splitMask(xs) : 1.0;
						p.at(x, y) = 400.0 + m * hfTex(xs, y) + (sigma > 0.0 ? sigma * nd(rng) : 0.0);
					}
				return p;
			};

			struct ProbeMetrics
			{
				double warpEcur = 0.0, warpEprev = 0.0, warpC = 0.0;
				double warpEnergyRatio = 0.0;      // E_prev / E_cur (1.0 on integer pan; < 0.75 on 0.5-px pan)
				double rawCrossBias = 0.0;         // (E_cur - C) / E_cur
				double projResidual = 0.0;         // max(0, E_cur - max(0, C)^2 / E_prev) / E_cur
				double texE = 0.0, texC = 0.0, texI = 0.0, texRho = 0.0;
				double flatE = 0.0, flatI = 0.0, flatRho = 0.0;
				int texRegCount = 0, flatRegCount = 0;
				double levelTBin3 = 0.0;
				bool validTBin3 = false;
				double meanNb = 0.0, meanRetained = 0.0;
				double texMeanGate = 0.0;
				double leak4 = 0.0, leak5 = 0.0;
				int usedNeighbours = 0, droppedRepeat = 0, fallbackPixels = 0, totalPixels = 0;
				double meanKeff = 0.0;
				double texRawMse = 0.0, texCleanMse = 0.0;
				double texTruthE = 0.0, texCleanE = 0.0;
				double texGrainDLeakE = 0.0;       // mean(grainD^2) / mean(truthD^2)
				double texAmpLeak = 0.0;           // <grainD, truthD> / <truthD, truthD>
				double flatRawE = 0.0, flatCleanE = 0.0;
			};

			auto runPan = [&](double speedPx, double sigma, uint32_t seedBase, bool flatPlusTextured, bool disableRepeatGateForMethodB, const std::string& tag) -> ProbeMetrics
			{
				constexpr int kFrames = 7, kCur = 4;
				std::vector<GrainSplit::Split> splits(static_cast<size_t>(kFrames));
				std::vector<Plane> bands(static_cast<size_t>(kFrames));
				for (int t = 0; t < kFrames; ++t)
				{
					// shiftX = -speedPx * t -> xs = x + speedPx * t -> prev frame (t - 1) matches at x + speedPx (vx = +speedPx, vy = 0).
					const double shiftX = -speedPx * static_cast<double>(t);
					const uint32_t seed = sigma > 0.0 ? seedBase + static_cast<uint32_t>(t) : 0u;
					splits[static_cast<size_t>(t)] = GrainSplit::Compute(makeFrame(shiftX, sigma, seed, flatPlusTextured), 1.0, kB);
					bands[static_cast<size_t>(t)] = GrainLevel::BandD(splits[static_cast<size_t>(t)]);
				}

				// Pre-generated independent noiseless structure band at cur (never self-referenced).
				const Plane truthD = GrainLevel::BandD(GrainSplit::Compute(makeFrame(-speedPx * static_cast<double>(kCur), 0.0, 0u, flatPlusTextured), 1.0, kB));

				GrainLevel::BlockField fPrev;
				fPrev.Init(kW, kH, kB, speedPx, 0.0);
				GrainLevel::Estimator est;
				GrainLevel::Context ctx;
				ctx.sourceHz = 24.0;
				ctx.field = &fPrev;
				GrainLevel::Result t1;
				for (int t = 0; t <= kCur; ++t) t1 = est.Step(splits[static_cast<size_t>(t)], ctx);

				const GrainSplit::Split& uCur = splits[static_cast<size_t>(kCur)];
				const Plane& dCur = bands[static_cast<size_t>(kCur)];

				Require(t1.mode == GrainLevel::Mode::Temporal, tag + ": temporal mode active");
				Require(t1.regionsTrusted > 0 && t1.skipRegions == 0, tag + ": trusted regions exist and skipGuard does not fire on pan");

				ProbeMetrics m;
				for (size_t i = 0; i < uCur.N.size(); ++i)
				{
					Require(std::isfinite(t1.Nb[i]) && t1.Nb[i] >= 0.0 && t1.Nb[i] <= uCur.N[i] + 1e-12, tag + ": N_b finite, non-negative, <= unit-4 N");
					Require(std::isfinite(t1.retained[i]) && t1.retained[i] >= 0.0, tag + ": retained finite and non-negative");
					m.meanNb += t1.Nb[i];
					m.meanRetained += t1.retained[i];
				}
				m.meanNb /= static_cast<double>(uCur.N.size());
				m.meanRetained /= static_cast<double>(uCur.N.size());

				double maxYErr = 0.0;
				for (size_t i = 0; i < uCur.f.v.size(); ++i)
				{
					Require(std::isfinite(t1.f.v[i]) && t1.f.v[i] >= -1e-12 && t1.f.v[i] <= 1.0 + 1e-12, tag + ": f finite and in [0, 1]");
					Require(std::isfinite(t1.gate.v[i]) && t1.gate.v[i] >= -1e-12 && t1.gate.v[i] <= 1.0 + 1e-12, tag + ": gate finite and in [0, 1]");
					Require(t1.f.v[i] >= uCur.f.v[i] - 1e-12, tag + ": f' >= f (mirror gate off)");
					Require(std::isfinite(t1.clean.v[i]) && std::isfinite(t1.grain.v[i]), tag + ": clean and grain planes finite");
					maxYErr = (std::max)(maxYErr, std::fabs((t1.clean.v[i] + t1.grain.v[i]) - (uCur.clean.v[i] + uCur.grain.v[i])));
				}
				Require(maxYErr < 1e-9, tag + ": clean + grain reproduces Y exactly");

				if (t1.levelT.size() > 3 && t1.validT.size() > 3)
				{
					m.levelTBin3 = t1.levelT[3];
					m.validTBin3 = t1.validT[3] != 0;
				}
				m.leak4 = GrainLevel::FrozenLeak(uCur.f, truthD, 2 * kB);
				m.leak5 = GrainLevel::FrozenLeak(t1.f, truthD, 2 * kB);
				Require(std::isfinite(m.leak4) && std::isfinite(m.leak5), tag + ": FrozenLeak finite");
				Require(m.leak5 <= m.leak4 + 1e-12, tag + ": temporal leak <= unit-4 leak");

				// Direct CrossRegions & shared Catmull-Rom WarpBicubic measurement on (t = cur - 1 -> cur).
				const GrainLevel::Warped prevW = GrainLevel::WarpBicubic(bands[static_cast<size_t>(kCur - 1)], fPrev, kW, kH);
				const GrainLevel::Params pDef;
				const GrainLevel::Regions reg = GrainLevel::CrossRegions(dCur, prevW, uCur.base, fPrev, uCur.N, nullptr, nullptr, pDef, kB);

				const int iy0 = 2 * kB, iy1 = kH - 2 * kB;
				const int fx0 = kB, fx1 = 3 * kB;                           // x in [16, 48): 100% pure flat for t <= 4 (xs <= 52 < 64)
				const int tx0 = flatPlusTextured ? (5 * kB + 8) : (2 * kB); // x in [88, 160) for Part b (xs >= 90 > 72); [32, 160) for Part a
				const int tx1 = kW - 2 * kB;

				double sumEcur = 0.0, sumEprev = 0.0, sumC = 0.0, sumGate = 0.0;
				int nTexPx = 0;
				for (int y = iy0; y < iy1; ++y)
					for (int x = tx0; x < tx1; ++x)
					{
						const size_t idx = static_cast<size_t>(y) * kW + static_cast<size_t>(x);
						Require(prevW.valid[idx] != 0, tag + ": interior textured warp pixel valid");
						const double dc = dCur.at(x, y);
						const double dp = prevW.v.at(x, y);
						sumEcur += dc * dc;
						sumEprev += dp * dp;
						sumC += dc * dp;
						sumGate += t1.gate.at(x, y);
						++nTexPx;
					}
				m.warpEcur = sumEcur / static_cast<double>(nTexPx);
				m.warpEprev = sumEprev / static_cast<double>(nTexPx);
				m.warpC = sumC / static_cast<double>(nTexPx);
				m.warpEnergyRatio = m.warpEprev / (std::max)(m.warpEcur, 1e-12);
				m.rawCrossBias = (m.warpEcur - m.warpC) / (std::max)(m.warpEcur, 1e-12);
				const double cPos = (std::max)(0.0, m.warpC);
				m.projResidual = (std::max)(0.0, m.warpEcur - (cPos * cPos) / (std::max)(m.warpEprev, 1e-12)) / (std::max)(m.warpEcur, 1e-12);
				m.texMeanGate = sumGate / static_cast<double>(nTexPx);

				for (int ry = iy0 / reg.R; ry < iy1 / reg.R; ++ry)
					for (int rx = tx0 / reg.R; rx < tx1 / reg.R; ++rx)
					{
						const size_t i = static_cast<size_t>(ry) * reg.rw + static_cast<size_t>(rx);
						Require(reg.trusted[i] != 0 && reg.skip[i] == 0, tag + ": interior textured region trusted and unskipped");
						Require(std::isfinite(reg.E[i]) && std::isfinite(reg.C[i]) && std::isfinite(reg.I[i]) && std::isfinite(reg.rho[i]), tag + ": textured region stats finite");
						m.texE += reg.E[i]; m.texC += reg.C[i]; m.texI += reg.I[i]; m.texRho += reg.rho[i];
						++m.texRegCount;
					}
				if (m.texRegCount > 0)
				{
					m.texE /= m.texRegCount; m.texC /= m.texRegCount; m.texI /= m.texRegCount; m.texRho /= m.texRegCount;
				}

				if (flatPlusTextured)
				{
					for (int ry = iy0 / reg.R; ry < iy1 / reg.R; ++ry)
						for (int rx = fx0 / reg.R; rx < fx1 / reg.R; ++rx)
						{
							const size_t i = static_cast<size_t>(ry) * reg.rw + static_cast<size_t>(rx);
							Require(reg.trusted[i] != 0 && reg.skip[i] == 0, tag + ": interior flat region trusted and unskipped");
							Require(std::isfinite(reg.E[i]) && std::isfinite(reg.I[i]) && std::isfinite(reg.rho[i]), tag + ": flat region stats finite");
							m.flatE += reg.E[i]; m.flatI += reg.I[i]; m.flatRho += reg.rho[i];
							++m.flatRegCount;
						}
					if (m.flatRegCount > 0)
					{
						m.flatE /= m.flatRegCount; m.flatI /= m.flatRegCount; m.flatRho /= m.flatRegCount;
					}
				}

				// Method B (NeighbourWiener) with K = 4 contiguous neighbours at offsets {-2, -1, +1, +2}.
				const int offsets[4] = { -2, -1, 1, 2 };
				std::vector<GrainLevel::Neighbour> nb(4);
				for (int j = 0; j < 4; ++j)
				{
					nb[static_cast<size_t>(j)].d = &bands[static_cast<size_t>(kCur + offsets[j])];
					nb[static_cast<size_t>(j)].field.Init(kW, kH, kB, -static_cast<double>(offsets[j]) * speedPx, 0.0);
					nb[static_cast<size_t>(j)].epoch = 0;
					nb[static_cast<size_t>(j)].offset = offsets[j];
				}
				GrainLevel::Params pB;
				// On noise-free synthetic frames (Part a), an exact integer-shifted neighbour is bit-identical to dCur so C8's
				// repeat gate rightly classifies it as a repeat; disabling repeatGate isolates Method B's warp+Wiener HF bias.
				// Part (b) keeps the default repeatGate = true on real sigma = 2.0 grain.
				if (disableRepeatGateForMethodB) pB.repeatGate = false;
				const GrainLevel::Window win = GrainLevel::WindowFor(pB, false, false, 2);
				const GrainLevel::NeighbourResult nr = GrainLevel::NeighbourWiener(uCur, t1, nb, 0, win, pB);

				m.usedNeighbours = nr.usedNeighbours;
				m.droppedRepeat = nr.droppedRepeat;
				m.fallbackPixels = nr.fallbackPixels;
				m.totalPixels = nr.pixels;
				m.meanKeff = nr.meanKeff;

				double maxBErr = 0.0;
				for (int y = 0; y < kH; ++y)
					for (int x = 0; x < kW; ++x)
					{
						const double cl = nr.cleanD.at(x, y), gr = nr.grainD.at(x, y);
						Require(std::isfinite(cl) && std::isfinite(gr), tag + ": Method B cleanD/grainD finite");
						maxBErr = (std::max)(maxBErr, std::fabs((cl + gr) - dCur.at(x, y)));
					}
				Require(maxBErr < 1e-9, tag + ": Method B cleanD + grainD = D_t exactly");

				double sumTruth2 = 0.0, sumClean2 = 0.0, sumGrainD2 = 0.0, sumGrainDotTruth = 0.0, sumRawErr2 = 0.0, sumCleanErr2 = 0.0;
				for (int y = iy0; y < iy1; ++y)
					for (int x = tx0; x < tx1; ++x)
					{
						const double tr = truthD.at(x, y), raw = dCur.at(x, y), cl = nr.cleanD.at(x, y), gr = nr.grainD.at(x, y);
						sumTruth2 += tr * tr;
						sumClean2 += cl * cl;
						sumGrainD2 += gr * gr;
						sumGrainDotTruth += gr * tr;
						sumRawErr2 += (raw - tr) * (raw - tr);
						sumCleanErr2 += (cl - tr) * (cl - tr);
					}
				m.texTruthE = sumTruth2 / static_cast<double>(nTexPx);
				m.texCleanE = sumClean2 / static_cast<double>(nTexPx);
				m.texGrainDLeakE = sumGrainD2 / (std::max)(sumTruth2, 1e-12);
				m.texAmpLeak = sumGrainDotTruth / (std::max)(sumTruth2, 1e-12);
				m.texRawMse = sumRawErr2 / static_cast<double>(nTexPx);
				m.texCleanMse = sumCleanErr2 / static_cast<double>(nTexPx);

				if (flatPlusTextured)
				{
					double sRaw = 0.0, sClean = 0.0;
					int nFlatPx = 0;
					for (int y = iy0; y < iy1; ++y)
						for (int x = fx0; x < fx1; ++x)
						{
							const double raw = dCur.at(x, y), cl = nr.cleanD.at(x, y);
							sRaw += raw * raw; sClean += cl * cl;
							++nFlatPx;
						}
					m.flatRawE = sRaw / static_cast<double>(nFlatPx);
					m.flatCleanE = sClean / static_cast<double>(nFlatPx);
				}

				return m;
			};

			// Part (a): Clean grain-free high-frequency texture under 0.5-px sub-pixel pan vs exact integer 1.0-px pan.
			const ProbeMetrics aInt = runPan(1.0, 0.0, 0u, false, true, "clean-1.0px");
			const ProbeMetrics aHalf = runPan(0.5, 0.0, 0u, false, true, "clean-0.5px");

			Log("GS153+208 Part(a) clean HF texture | 1.0px: warpRatio=" + std::to_string(aInt.warpEnergyRatio)
				+ " rawCrossBias=" + std::to_string(aInt.rawCrossBias) + " projRes=" + std::to_string(aInt.projResidual)
				+ " E=" + std::to_string(aInt.texE) + " I=" + std::to_string(aInt.texI) + " I/E=" + std::to_string(aInt.texI / aInt.texE)
				+ " rho=" + std::to_string(aInt.texRho) + " retained=" + std::to_string(aInt.meanRetained)
				+ " leak5=" + std::to_string(aInt.leak5) + " B_leakE=" + std::to_string(aInt.texGrainDLeakE)
				+ " B_ampLeak=" + std::to_string(aInt.texAmpLeak) + " B_keff=" + std::to_string(aInt.meanKeff));
			Log("GS153+208 Part(a) clean HF texture | 0.5px: warpRatio=" + std::to_string(aHalf.warpEnergyRatio)
				+ " rawCrossBias=" + std::to_string(aHalf.rawCrossBias) + " projRes=" + std::to_string(aHalf.projResidual)
				+ " E=" + std::to_string(aHalf.texE) + " I=" + std::to_string(aHalf.texI) + " I/E=" + std::to_string(aHalf.texI / aHalf.texE)
				+ " rho=" + std::to_string(aHalf.texRho) + " retained=" + std::to_string(aHalf.meanRetained)
				+ " leak5=" + std::to_string(aHalf.leak5) + " B_leakE=" + std::to_string(aHalf.texGrainDLeakE)
				+ " B_ampLeak=" + std::to_string(aHalf.texAmpLeak) + " B_keff=" + std::to_string(aHalf.meanKeff)
				+ " B_fallback=" + std::to_string(aHalf.fallbackPixels) + "/" + std::to_string(aHalf.totalPixels));

			// Negative control ("Prove the Hazard Is Real"): exact same HF texture has ~1.0 warped energy & <1% cross bias
			// on integer 1.0-px pan, while 0.5-px Catmull-Rom bicubic warping attenuates HF energy (<0.75) and creates >10%
			// cross-form bias that projection form (E - C^2/E_prev) reduces to <2%.
			Require(aInt.warpEnergyRatio > 0.99 && aInt.warpEnergyRatio < 1.01, "Part(a): integer pan preserves HF warped energy (~1.0)");
			Require(aHalf.warpEnergyRatio > 0.40 && aHalf.warpEnergyRatio < 0.75, "Part(a): 0.5-px bicubic pan genuinely attenuates HF warped energy (< 0.75)");
			Require(std::fabs(aInt.rawCrossBias) < 0.01, "Part(a): integer pan has near-zero cross-form I_r/E_r bias");
			Require(aHalf.rawCrossBias > 0.10, "Part(a): 0.5-px pan proves >10% cross-form attenuation hazard on clean HF texture");
			Require(aHalf.projResidual < 0.02, "Part(a): projection-form residual < 2% on exact same 0.5-px warped pair");

			Require(aInt.texE > 40.0 && aHalf.texE > 40.0, "Part(a): clean HF texture carries strong band-D energy");
			Require(std::fabs(aInt.texI) / aInt.texE < 0.01, "Part(a): integer pan live CrossRegions I_r/E_r < 1%");
			Require(aInt.texRho > 10.0, "Part(a): integer pan clean HF texture reads high coherence rho > 10");
			Require(aInt.leak5 < 0.05 && aInt.meanRetained < 0.1, "Part(a): integer pan clean HF texture keeps structure and near-zero retained grain");
			Require(std::isfinite(aHalf.texI) && std::isfinite(aHalf.texRho) && aHalf.texRho > 0.0, "Part(a): 0.5-px L1/L2 outputs finite and positive");
			Require(aHalf.leak5 < 0.25 && aHalf.meanRetained >= 0.0, "Part(a): 0.5-px clean HF leak and retained share bounded");
			Require(aInt.usedNeighbours == 4 && aHalf.usedNeighbours == 4, "Part(a): Method B uses all 4 neighbours");
			Require(aInt.texGrainDLeakE < 0.01 && std::fabs(aInt.texAmpLeak) < 0.02, "Part(a): integer pan Method B has near-zero HF texture leak");
			Require(std::isfinite(aHalf.texGrainDLeakE) && aHalf.texGrainDLeakE >= 0.0 && aHalf.texGrainDLeakE < 0.5, "Part(a): 0.5-px Method B clean HF leak energy finite and bounded");
			Require(std::isfinite(aHalf.texAmpLeak) && aHalf.texAmpLeak > -0.1 && aHalf.texAmpLeak < 0.5, "Part(a): 0.5-px Method B clean HF amplitude leak finite and bounded");

			// Part (b): Grainy flat + HF textured plane (sigma = 2.0, 10-bit) under 0.5-px pan vs exact integer 1.0-px pan.
			const ProbeMetrics bInt = runPan(1.0, 2.0, 701u, true, false, "grainy-1.0px");
			const ProbeMetrics bHalf = runPan(0.5, 2.0, 701u, true, false, "grainy-0.5px");

			Log("GS153+208 Part(b) grainy flat+HF (sigma=2.0, 10b) | 1.0px: flatI=" + std::to_string(bInt.flatI)
				+ " flatRho=" + std::to_string(bInt.flatRho) + " texE=" + std::to_string(bInt.texE)
				+ " texI=" + std::to_string(bInt.texI) + " texI/E=" + std::to_string(bInt.texI / bInt.texE)
				+ " texRho=" + std::to_string(bInt.texRho) + " texGate=" + std::to_string(bInt.texMeanGate)
				+ " levelT3=" + std::to_string(bInt.levelTBin3) + " retained=" + std::to_string(bInt.meanRetained)
				+ " leak5=" + std::to_string(bInt.leak5) + " B_texMse=" + std::to_string(bInt.texCleanMse) + "/" + std::to_string(bInt.texRawMse)
				+ " B_ampLeak=" + std::to_string(bInt.texAmpLeak) + " B_flatE=" + std::to_string(bInt.flatCleanE) + "/" + std::to_string(bInt.flatRawE));
			Log("GS153+208 Part(b) grainy flat+HF (sigma=2.0, 10b) | 0.5px: flatI=" + std::to_string(bHalf.flatI)
				+ " flatRho=" + std::to_string(bHalf.flatRho) + " texE=" + std::to_string(bHalf.texE)
				+ " texI=" + std::to_string(bHalf.texI) + " texI/E=" + std::to_string(bHalf.texI / bHalf.texE)
				+ " texRho=" + std::to_string(bHalf.texRho) + " texGate=" + std::to_string(bHalf.texMeanGate)
				+ " levelT3=" + std::to_string(bHalf.levelTBin3) + " retained=" + std::to_string(bHalf.meanRetained)
				+ " leak5=" + std::to_string(bHalf.leak5) + " B_texMse=" + std::to_string(bHalf.texCleanMse) + "/" + std::to_string(bHalf.texRawMse)
				+ " B_ampLeak=" + std::to_string(bHalf.texAmpLeak) + " B_flatE=" + std::to_string(bHalf.flatCleanE) + "/" + std::to_string(bHalf.flatRawE));

			const double cleanL1Bias = (aHalf.texI / aHalf.texE) - (aInt.texI / aInt.texE);
			const double grainyL1Bias = (bHalf.texI / bHalf.texE) - (bInt.texI / bInt.texE);
			const double methodBAmpLeakDelta = bHalf.texAmpLeak - bInt.texAmpLeak;
			Log("GS153+208 BiasGate summary: cleanL1_I/E_delta=" + std::to_string(cleanL1Bias)
				+ " grainyL1_I/E_delta=" + std::to_string(grainyL1Bias)
				+ " methodB_ampLeak_delta=" + std::to_string(methodBAmpLeakDelta)
				+ " (gate > 0.05 -> evaluate conditional Blue Team projection-form switch; keep ONE shared bicubic sampler)");

			Require(bInt.flatRegCount > 0 && bHalf.flatRegCount > 0 && bInt.texRegCount > 0 && bHalf.texRegCount > 0, "Part(b): flat and textured interior regions present");
			Require(bInt.flatI > 1.0 && bInt.flatI < 8.0 && bHalf.flatI > 1.0 && bHalf.flatI < 8.0, "Part(b): flat grain I_r tracks sigma=2 band variance on both pans");
			Require(bInt.flatRho < 1.0 && bHalf.flatRho < 1.0, "Part(b): flat uncorrelated grain stays below gateA on both pans");
			Require(bInt.texI > 1.0 && bInt.texI < 8.0, "Part(b): integer pan textured I_r stays unbiased near sigma=2 grain variance");
			Require(bInt.texRho > 10.0 && bInt.texMeanGate > 0.95, "Part(b): integer pan textured plane reads high coherence and full L2 gate");
			Require(bInt.leak5 < 0.15, "Part(b): integer pan grainy textured plane keeps structure (leak < 15%)");
			Require(bInt.validTBin3 && bHalf.validTBin3 && std::isfinite(bHalf.levelTBin3) && bHalf.levelTBin3 > 0.0, "Part(b): luma bin 3 NLF_t valid and finite on both pans");
			Require(std::isfinite(bHalf.texI) && bHalf.texI >= 0.0 && std::isfinite(bHalf.texRho) && bHalf.texRho > 0.0, "Part(b): 0.5-px grainy textured I_r and rho finite and positive");
			Require(bHalf.leak5 < 0.25 && bHalf.meanRetained >= 0.0, "Part(b): 0.5-px grainy textured detail loss and retained share bounded");

			Require(bInt.usedNeighbours == 4 && bHalf.usedNeighbours == 4, "Part(b): Method B uses all 4 grainy neighbours");
			Require(bInt.droppedRepeat == 0 && bHalf.droppedRepeat == 0, "Part(b): real sigma=2 grain does not false-fire C8 repeat gate");
			Require(bInt.fallbackPixels < bInt.totalPixels / 4 && bHalf.fallbackPixels < bHalf.totalPixels / 4, "Part(b): most grainy pixels take the Method B neighbour path");
			Require(bInt.meanKeff >= 2.0 && bHalf.meanKeff >= 2.0, "Part(b): Method B keeps >= 2 neighbours on average on both pans");
			Require(bInt.flatCleanE < 0.5 * bInt.flatRawE && bHalf.flatCleanE < 0.5 * bHalf.flatRawE, "Part(b): Method B denoises flat grain on both pans");
			Require(bInt.texCleanMse < 0.7 * bInt.texRawMse, "Part(b): integer pan Method B denoises grainy HF texture against independent truth");
			Require(std::fabs(bInt.texAmpLeak) < 0.08, "Part(b): integer pan Method B keeps grainy HF texture amplitude leak < 8%");
			Require(std::isfinite(bHalf.texCleanMse) && bHalf.texCleanMse >= 0.0 && bHalf.texCleanMse < bHalf.texTruthE, "Part(b): 0.5-px grainy Method B MSE against truth finite and bounded");
			Require(std::isfinite(bHalf.texAmpLeak) && bHalf.texAmpLeak > -0.1 && bHalf.texAmpLeak < 0.5, "Part(b): 0.5-px grainy Method B HF amplitude leak finite and bounded");
		}
	};
}
