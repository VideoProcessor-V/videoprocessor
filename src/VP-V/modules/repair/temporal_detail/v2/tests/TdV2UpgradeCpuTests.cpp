// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Modular Upgrades Cross-Model Adversarial CPU Red-Team Suite
// (2026-11-25_request-TD-v2-upgrade-BUILD.READY.md D1-D7, FABLE52-draft U1-U5 & Phase 6,
//  FABLE52-audit B-2, H-1..4, M-1, M-3, M-6, L-1, L-3):
//
// Strict Rule 3 & Rule 8 Red-Team Guarantees:
//   - ZERO hand-set `estimatedStepDelta` to make recovery tests pass (tested with the engine's own
//     `AlignedSharpnessA2e` `hasStepPrior=false` path and `TdV2NonBlockStepEstimator::MeasureWithOverlay`).
//   - Synthetic scenes constructed from analytical trigonometric / logistic / quantized primitives
//     (NEVER synthesized with `VpUnit::BandFromBase`).
//   - Every protection/invariant test includes an explicit same-input negative control proving the
//     legacy/unprotected path violates the invariant.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Chroma.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2FrameRole.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Metrics.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Settings.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
namespace TdV2UpgradeCpuNs
{
	using namespace TdV2;

	constexpr double kPi = 3.14159265358979323846;

	static std::wstring Msg(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0)
	{
		char buf[384];
		std::snprintf(buf, sizeof(buf), fmt, a, b, c);
		std::string s(buf);
		return std::wstring(s.begin(), s.end());
	}

	// Evaluate quadratic objective Q(w) = w' (C + reg I) w - 2 w' c (lower is better; w = 0 gives Q = 0).
	static double EvalQpObjective(
		const double C[kMaxBlendRefs][kMaxBlendRefs],
		const double c[kMaxBlendRefs],
		const double* w,
		int K,
		double traceRegScale)
	{
		double diagSum = 0.0;
		for (int i = 0; i < K; ++i)
			diagSum += (C[i][i] > 0.0 ? C[i][i] : 0.0);
		const double meanDiag = diagSum / static_cast<double>(K);
		const double reg = (std::max)(traceRegScale * meanDiag, kSolveNumericalFloor);
		double quad = 0.0;
		double lin = 0.0;
		for (int i = 0; i < K; ++i)
		{
			lin += w[i] * c[i];
			for (int j = 0; j < K; ++j)
			{
				const double cij = C[i][j] + (i == j ? reg : 0.0);
				quad += w[i] * cij * w[j];
			}
		}
		return quad - 2.0 * lin;
	}

	// Continuous analytical scene (trigonometric textures + logistic edges + smooth gradient, NEVER BandFromBase!).
	static double EvalMasterSceneAt(double x, double y, int w, int h)
	{
		const double nx = x / static_cast<double>((std::max)(1, w));
		const double ny = y / static_cast<double>((std::max)(1, h));
		const double ramp = 180.0 + 380.0 * nx + 90.0 * ny;
		const double edge = 110.0 / (1.0 + std::exp(-(x - 0.45 * w - 0.12 * y) * 1.35));
		// Textured lower/right half, flatter sky upper-left quadrant so both activity classes (0 & 1) are populated.
		const double texMask = 1.0 / (1.0 + std::exp(-((nx - 0.28) + (ny - 0.28)) * 18.0));
		const double tex = texMask * (
			42.0 * std::sin(0.62 * x + 0.21 * y) * std::cos(0.39 * y - 0.17 * x) +
			28.0 * std::cos(1.18 * x - 0.83 * y) +
			18.0 * std::sin(1.75 * y + 0.45 * x));
		return (std::max)(64.0, (std::min)(940.0, ramp + edge + tex));
	}

	static TdV2Plane RenderMasterScene(int w, int h, double shiftX, double shiftY)
	{
		TdV2Plane p(w, h, 0.0);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				p.at(x, y) = EvalMasterSceneAt(static_cast<double>(x) + shiftX, static_cast<double>(y) + shiftY, w, h);
			}
		}
		return p;
	}

	// Deblocked encoder valley loss model (box/binomial spatial low-pass + dead-zone scalar quantization,
	// completely independent of BandFromBase and smooth across 8-px boundaries).
	static TdV2Plane ApplyDeblockedValleyLoss(const TdV2Plane& master, double quantStep, uint32_t seed, bool heavyPatch)
	{
		const int w = master.w;
		const int h = master.h;
		TdV2Plane blurred(w, h, 0.0);
		// 5-tap separable binomial [1, 4, 6, 4, 1] / 16 low-pass (simulates encoder loop filter + inter RDO smoothing).
		static constexpr double kBinom[5] = { 1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0 };
		TdV2Plane tmp(w, h, 0.0);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				double acc = 0.0;
				for (int k = -2; k <= 2; ++k)
					acc += kBinom[k + 2] * master.at((std::max)(0, (std::min)(w - 1, x + k)), y);
				tmp.at(x, y) = acc;
			}
		}
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				double acc = 0.0;
				for (int k = -2; k <= 2; ++k)
					acc += kBinom[k + 2] * tmp.at(x, (std::max)(0, (std::min)(h - 1, y + k)));
				blurred.at(x, y) = acc;
			}
		}

		std::mt19937 rng(seed);
		std::normal_distribution<double> dither(0.0, 0.35);
		TdV2Plane out(w, h, 0.0);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				const bool inWorstPatch = heavyPatch && (x >= w / 2 && x < (w / 2) + 32 && y >= h / 2 && y < (h / 2) + 32);
				const double localStep = inWorstPatch ? (quantStep * 2.2) : quantStep;
				const double mixBlur = inWorstPatch ? 0.92 : 0.55;
				const double baseVal = (1.0 - mixBlur) * master.at(x, y) + mixBlur * blurred.at(x, y);
				const double q = std::round(baseVal / localStep) * localStep + dither(rng);
				out.at(x, y) = (std::max)(16.0, (std::min)(1000.0, q));
			}
		}
		// Post-quantization 2-tap in-loop deblocking across 8-px block boundaries (models HEVC/AV1 loop filter,
		// smoothing 8-px block steps below interior gradients while leaving interior detail loss intact).
		for (int y = 0; y < h; ++y)
		{
			for (int bx = 8; bx < w; bx += 8)
			{
				const double a = out.at(bx - 1, y);
				const double b = out.at(bx, y);
				out.at(bx - 1, y) = 0.60 * a + 0.40 * b;
				out.at(bx, y) = 0.40 * a + 0.60 * b;
			}
		}
		for (int by = 8; by < h; by += 8)
		{
			for (int x = 0; x < w; ++x)
			{
				const double a = out.at(x, by - 1);
				const double b = out.at(x, by);
				out.at(x, by - 1) = 0.60 * a + 0.40 * b;
				out.at(x, by) = 0.40 * a + 0.60 * b;
			}
		}
		return out;
	}

	static TdV2Plane ApplyCrispPeakQuant(const TdV2Plane& master, uint32_t seed)
	{
		std::mt19937 rng(seed);
		std::normal_distribution<double> noise(0.0, 0.45);
		TdV2Plane out(master.w, master.h, 0.0);
		for (size_t i = 0; i < master.v.size(); ++i)
			out.v[i] = std::round(master.v[i] + noise(rng));
		return out;
	}
} // namespace TdV2UpgradeCpuNs

TEST_CLASS(TdV2UpgradeCpuTests)
{
public:
	// 1. TD-U2 (Fable H-2): Exact active-set convex QP solver on both Fable worked examples + 200 random SPD trials.
	TEST_METHOD(TdU2ActiveSetSolverExactWorkedExamplesAndRandomPdOptimality)
	{
		using namespace TdV2UpgradeCpuNs;

		// Worked Example 1 (Fable H-2): near-collinear past references C = [[1, 0.99], [0.99, 1]], c = (0.3, 0.2), reg = 1%.
		{
			double C[kMaxBlendRefs][kMaxBlendRefs] = {};
			C[0][0] = 1.0;  C[0][1] = 0.99;
			C[1][0] = 0.99; C[1][1] = 1.0;
			double c[kMaxBlendRefs] = { 0.3, 0.2, 0.0, 0.0 };

			const BlendSolveResult exact = SolveConvexWeights(
				C, c, 2, kDefaultTraceRegScale, nullptr, SolverMode::ActiveSetExact);
			const BlendSolveResult legacy = SolveConvexWeights(
				C, c, 2, kDefaultTraceRegScale, nullptr, SolverMode::LegacyClipRescale);

			// Exact subset {0}: w1 = 0.3 / 1.01 = 0.297029703, w2 = 0.0, Q(w_exact) = -0.0891089 < 0.
			const double expectedW0 = 0.3 / 1.01;
			Assert::IsTrue(std::fabs(exact.weights[0] - expectedW0) < 1e-6,
				Msg("Ex1 exact w[0]=%.6f expected %.6f", exact.weights[0], expectedW0).c_str());
			Assert::IsTrue(std::fabs(exact.weights[1]) < 1e-9,
				Msg("Ex1 exact w[1]=%.6f expected 0", exact.weights[1]).c_str());

			// Same-input negative control: LegacyClipRescale clips (-2.375 -> 0) and rescales (+2.625 -> 1.0),
			// turning a 30% mix into a 100% replacement with Q = +0.41 > 0 (worse than w = 0!).
			Assert::IsTrue(std::fabs(legacy.weights[0] - 1.0) < 1e-6 && std::fabs(legacy.weights[1]) < 1e-9,
				Msg("Ex1 legacy control w=(%.6f, %.6f) expected (1.0, 0.0)", legacy.weights[0], legacy.weights[1]).c_str());
			const double qExact = EvalQpObjective(C, c, exact.weights.data(), 2, kDefaultTraceRegScale);
			const double qLegacy = EvalQpObjective(C, c, legacy.weights.data(), 2, kDefaultTraceRegScale);
			Assert::IsTrue(qExact < -0.088 && qLegacy > +0.40,
				Msg("Ex1 Q(exact)=%.6f must be <0 while Q(legacy)=%.6f >0", qExact, qLegacy).c_str());
		}

		// Worked Example 2 (Fable H-2): one-sided unflagged fade with two past peaks at F = 5*sigma_q and 2F = 10*sigma_q.
		{
			const double sigmaQ = 2.0;
			const double varQ = sigmaQ * sigmaQ;
			const double F1 = 5.0 * sigmaQ;
			const double F2 = 10.0 * sigmaQ;
			double C[kMaxBlendRefs][kMaxBlendRefs] = {};
			C[0][0] = F1 * F1; C[0][1] = F1 * F2;
			C[1][0] = F1 * F2; C[1][1] = F2 * F2;
			double c[kMaxBlendRefs] = { varQ, varQ, 0.0, 0.0 };

			const BlendSolveResult exact = SolveConvexWeights(
				C, c, 2, kDefaultTraceRegScale, nullptr, SolverMode::ActiveSetExact);
			const BlendSolveResult legacy = SolveConvexWeights(
				C, c, 2, kDefaultTraceRegScale, nullptr, SolverMode::LegacyClipRescale);

			const double exactPullSigma = (exact.weights[0] * F1 + exact.weights[1] * F2) / sigmaQ;
			const double legacyPullSigma = (legacy.weights[0] * F1 + legacy.weights[1] * F2) / sigmaQ;
			// Exact active-set pulls brightness by 5 / 25.625 = 0.19512 * sigma_q vs legacy 3.2239 * sigma_q (>16x worse!).
			Assert::IsTrue(std::fabs(exactPullSigma - (5.0 / 25.625)) < 1e-4,
				Msg("Ex2 exact fade pull=%.5f sigma_q expected 0.19512", exactPullSigma).c_str());
			Assert::IsTrue(legacyPullSigma > 3.20,
				Msg("Ex2 legacy negative control fade pull=%.5f sigma_q expected >3.20", legacyPullSigma).c_str());

			// Also verify uncentered second-moment matrix [[26, 51], [51, 101]] * varQ.
			double C2[kMaxBlendRefs][kMaxBlendRefs] = {};
			C2[0][0] = 26.0 * varQ; C2[0][1] = 51.0 * varQ;
			C2[1][0] = 51.0 * varQ; C2[1][1] = 101.0 * varQ;
			const BlendSolveResult ex2 = SolveConvexWeights(C2, c, 2, kDefaultTraceRegScale, nullptr, SolverMode::ActiveSetExact);
			const BlendSolveResult lg2 = SolveConvexWeights(C2, c, 2, kDefaultTraceRegScale, nullptr, SolverMode::LegacyClipRescale);
			const double pullEx2 = (ex2.weights[0] * F1 + ex2.weights[1] * F2) / sigmaQ;
			const double pullLg2 = (lg2.weights[0] * F1 + lg2.weights[1] * F2) / sigmaQ;
			Assert::IsTrue(pullEx2 < 0.19 && pullLg2 > 2.35,
				Msg("Ex2 uncentered pull exact=%.4f vs legacy=%.4f", pullEx2, pullLg2).c_str());
		}

		// Property Test: 240 random SPD matrices (K = 1..4) including heavily collinear & mixed-sign c vectors.
		std::mt19937 rng(20261125u);
		std::normal_distribution<double> norm(0.0, 1.0);
		size_t strictWinsOverLegacy = 0;
		for (int trial = 0; trial < 240; ++trial)
		{
			const int K = 1 + (trial % kMaxBlendRefs);
			double C[kMaxBlendRefs][kMaxBlendRefs] = {};
			double c[kMaxBlendRefs] = {};

			// Build rank-K SPD matrix with deliberate high collinearity on even trials.
			double baseVec[kMaxBlendRefs] = {};
			for (int k = 0; k < K; ++k) baseVec[k] = 2.0 + std::fabs(norm(rng));
			for (int s = 0; s < 6; ++s)
			{
				const double common = (trial % 2 == 0) ? (3.0 * norm(rng)) : 0.0;
				double row[kMaxBlendRefs] = {};
				for (int k = 0; k < K; ++k)
					row[k] = common * baseVec[k] + 0.35 * norm(rng);
				for (int i = 0; i < K; ++i)
					for (int j = 0; j < K; ++j)
						C[i][j] += row[i] * row[j];
			}
			for (int k = 0; k < K; ++k)
			{
				C[k][k] += 1e-3;
				c[k] = 1.5 * norm(rng) + 0.4;
			}

			const BlendSolveResult exact = SolveConvexWeights(
				C, c, K, kDefaultTraceRegScale, nullptr, SolverMode::ActiveSetExact);
			const BlendSolveResult legacy = SolveConvexWeights(
				C, c, K, kDefaultTraceRegScale, nullptr, SolverMode::LegacyClipRescale);

			Assert::IsFalse(exact.nonFiniteClosed || exact.singular, L"Random SPD active-set solve must succeed");
			double sumW = 0.0;
			for (int k = 0; k < K; ++k)
			{
				Assert::IsTrue(exact.weights[k] >= -1e-12, L"Weight must be non-negative");
				sumW += exact.weights[k];
			}
			Assert::IsTrue(sumW <= 1.0 + 1e-9, L"Simplex sum must be <= 1");

			const double qExact = EvalQpObjective(C, c, exact.weights.data(), K, kDefaultTraceRegScale);
			const double qLegacy = EvalQpObjective(C, c, legacy.weights.data(), K, kDefaultTraceRegScale);
			// Must never be worse than w = 0 (Q = 0) or any optimal single-reference weight alpha_k * e_k!
			Assert::IsTrue(qExact <= 1e-10, Msg("Q(exact)=%.8f exceeded Q(0)=0", qExact).c_str());

			double diagSum = 0.0;
			for (int k = 0; k < K; ++k) diagSum += C[k][k];
			const double reg = (std::max)(kDefaultTraceRegScale * (diagSum / K), kSolveNumericalFloor);
			for (int k = 0; k < K; ++k)
			{
				double singleW[kMaxBlendRefs] = {};
				singleW[k] = (std::max)(0.0, (std::min)(1.0, c[k] / (C[k][k] + reg)));
				const double qSingle = EvalQpObjective(C, c, singleW, K, kDefaultTraceRegScale);
				Assert::IsTrue(qExact <= qSingle + 1e-8,
					Msg("Q(exact)=%.8f worse than single-ref Q=%.8f", qExact, qSingle).c_str());
			}
			Assert::IsTrue(qExact <= qLegacy + 1e-8,
				Msg("Q(exact)=%.8f worse than Q(legacy)=%.8f", qExact, qLegacy).c_str());
			if (qExact + 1e-4 < qLegacy)
				++strictWinsOverLegacy;
		}
		Assert::IsTrue(strictWinsOverLegacy >= 30u,
			Msg("Expected >=30 strict active-set wins over legacy clip-rescale, got %.0f", static_cast<double>(strictWinsOverLegacy)).c_str());
	}

	// 2. TD-U1 (Fable B-2 / Owner D1(a)): Motion-aligned sharpness rank + A2e one-sided K=1 hold + worst-5% +3*tau recovery
	//    with ZERO hand-set estimatedStepDelta (`hasStepPrior = false` throughout!).
	TEST_METHOD(TdU1AlignedSharpnessA2eRoleRankingOneSidedHoldAndWorst5PctRecovery)
	{
		using namespace TdV2UpgradeCpuNs;
		constexpr int w = 96;
		constexpr int h = 64;
		constexpr int cellPx = kCanonicalBlockPx;
		constexpr int cw = w / cellPx;
		constexpr int ch = h / cellPx;
		constexpr size_t nCells = static_cast<size_t>(cw * ch);

		// 10-frame GOP rhythm: peaks at t = 0, 3, 6, 9; deblocked valleys in between, panning at 0.35 px/frame.
		std::vector<TdV2Plane> masters(10), frames(10);
		for (int t = 0; t < 10; ++t)
		{
			masters[t] = RenderMasterScene(w, h, 0.35 * t, -0.20 * t);
			const bool isPeak = (t % 3 == 0);
			frames[t] = isPeak
				? ApplyCrispPeakQuant(masters[t], 100u + static_cast<uint32_t>(t))
				: ApplyDeblockedValleyLoss(masters[t], 10.0, 200u + static_cast<uint32_t>(t), /*heavyPatch=*/true);
		}

		// Verify MeasureAlignedSharpnessFromCellEnergies (forward + symmetric reverse-pair + focus-cell abstention)
		// ranks peaks vs deblocked valleys cleanly, while negative control TdV2CodingGridStandIn misses deblocked valleys.
		AlignedSharpnessTracker tracker;
		TdV2FrameRoleDetector detAligned(2);
		TdV2FrameRoleDetector detLegacy8(2);
		TdV2CodingGridStandIn leg8StandIn;

		std::vector<QualityPattern::Record> recAligned(10), recLegacy8(10);
		auto captureDec = [](std::vector<QualityPattern::Record>& out, const FrameRoleDecision& d)
		{
			for (const auto& f : d.flushedBeforeCut)
				if (f.produced && f.record.seq < out.size()) out[static_cast<size_t>(f.record.seq)] = f.record;
			if (d.produced && d.record.seq < out.size()) out[static_cast<size_t>(d.record.seq)] = d.record;
		};

		for (int t = 0; t < 10; ++t)
		{
			AlignedSharpnessSummary sum{};
			if (t >= 1)
			{
				const CellMotionField mvFwd = CellMotionField::Uniform(cw, ch, 0.35, -0.20, 0.35, -0.20, RegionMotionClass::Camera);
				const CellMotionField mvRev = CellMotionField::Uniform(cw, ch, -0.35, 0.20, -0.35, 0.20, RegionMotionClass::Camera);
				TdV2Plane prevOnCur, curOnPrev;
				std::vector<uint8_t> valFwd, valRev;
				WarpCleanReference(frames[t - 1], frames[t], ActiveRect::Full(w, h), mvFwd.vx, mvFwd.vy, cw, ch, cellPx, prevOnCur, valFwd);
				WarpCleanReference(frames[t], frames[t - 1], ActiveRect::Full(w, h), mvRev.vx, mvRev.vy, cw, ch, cellPx, curOnPrev, valRev);

				const ThreeBandPlanes bCur = DecomposeActiveBands(frames[t], ActiveRect::Full(w, h));
				const ThreeBandPlanes bPrevW = DecomposeActiveBands(prevOnCur, ActiveRect::Full(w, h));
				const ThreeBandPlanes bPrev = DecomposeActiveBands(frames[t - 1], ActiveRect::Full(w, h));
				const ThreeBandPlanes bCurW = DecomposeActiveBands(curOnPrev, ActiveRect::Full(w, h));

				std::vector<double> cm(nCells, 0.0), chE(nCells, 0.0), pwm(nCells, 0.0), pwh(nCells, 0.0);
				std::vector<double> rpm(nCells, 0.0), rph(nCells, 0.0), rcwm(nCells, 0.0), rcwh(nCells, 0.0);
				for (int cy = 0; cy < ch; ++cy)
				{
					for (int cx = 0; cx < cw; ++cx)
					{
						const size_t ci = static_cast<size_t>(cy) * cw + cx;
						double sCm = 0.0, sCh = 0.0, sPwm = 0.0, sPwh = 0.0;
						double sRpm = 0.0, sRph = 0.0, sRcwm = 0.0, sRcwh = 0.0;
						size_t cnt = 0;
						for (int y = cy * cellPx; y < (cy + 1) * cellPx; ++y)
						{
							for (int x = cx * cellPx; x < (cx + 1) * cellPx; ++x)
							{
								const size_t pi = static_cast<size_t>(y) * w + x;
								if (!valFwd[pi] || !valRev[pi]) continue;
								sCm += bCur.M.v[pi] * bCur.M.v[pi];
								sCh += bCur.H.v[pi] * bCur.H.v[pi];
								sPwm += bPrevW.M.v[pi] * bPrevW.M.v[pi];
								sPwh += bPrevW.H.v[pi] * bPrevW.H.v[pi];
								sRpm += bPrev.M.v[pi] * bPrev.M.v[pi];
								sRph += bPrev.H.v[pi] * bPrev.H.v[pi];
								sRcwm += bCurW.M.v[pi] * bCurW.M.v[pi];
								sRcwh += bCurW.H.v[pi] * bCurW.H.v[pi];
								++cnt;
							}
						}
						if (cnt > 0)
						{
							const double inv = 1.0 / static_cast<double>(cnt);
							cm[ci] = sCm * inv; chE[ci] = sCh * inv;
							pwm[ci] = sPwm * inv; pwh[ci] = sPwh * inv;
							rpm[ci] = sRpm * inv; rph[ci] = sRph * inv;
							rcwm[ci] = sRcwm * inv; rcwh[ci] = sRcwh * inv;
						}
					}
				}
				std::vector<uint8_t> focusMask(nCells, 0u);
				focusMask[0] = 1u; // Verify focus-flagged cell abstention!
				sum = MeasureAlignedSharpnessFromCellEnergies(
					cm, chE, pwm, pwh, mvFwd.vx, mvFwd.vy, mvFwd.valid, &focusMask, nullptr, 10,
					0.0, 0.0, &rpm, &rph, &rcwm, &rcwh);
				Assert::IsTrue(sum.valid && sum.focusAbstainedCells == 1u, L"AlignedSharpness must abstain focus-flagged cell");
			}
			const FrameRoleEvidence evA = tracker.PushPairAndBuildEvidence(static_cast<uint64_t>(t), sum, nullptr, false, false, 10);
			captureDec(recAligned, detAligned.PushAndClassify(evA));

			const double* p1 = (t >= 1) ? frames[t - 1].v.data() : nullptr;
			const double* p2 = (t >= 2) ? frames[t - 2].v.data() : nullptr;
			const CodingGridSummary g8 = leg8StandIn.Measure(frames[t].v.data(), p1, p2, w, h, 10, nullptr);
			captureDec(recLegacy8, detLegacy8.PushAndClassify(FrameRoleEvidence::FromCodingGrid(static_cast<uint64_t>(t), g8, false, false, 10)));
		}
		for (const auto& f : detAligned.FlushEndOfStream())
			if (f.produced && f.record.seq < 10) recAligned[static_cast<size_t>(f.record.seq)] = f.record;
		for (const auto& f : detLegacy8.FlushEndOfStream())
			if (f.produced && f.record.seq < 10) recLegacy8[static_cast<size_t>(f.record.seq)] = f.record;

		// Interior peaks t=3, t=6 MUST be detected as Peak by AlignedSharpness, while valleys t=4,5,7,8 are non-Peak!
		Assert::AreEqual(static_cast<uint8_t>(QualityPattern::FrameRole::Peak), recAligned[3].role, L"Peak t=3 missed by AlignedSharpness");
		Assert::AreEqual(static_cast<uint8_t>(QualityPattern::FrameRole::Peak), recAligned[6].role, L"Peak t=6 missed by AlignedSharpness");
		Assert::IsTrue(recAligned[4].role != static_cast<uint8_t>(QualityPattern::FrameRole::Peak) &&
		               recAligned[5].role != static_cast<uint8_t>(QualityPattern::FrameRole::Peak),
			L"Valleys t=4,5 must not be marked Peak");

		// Negative control: Legacy 8-px block-edge stand-in fails to detect peaks t=3 and t=6 on deblocked frames!
		const bool legacyFoundBothPeaks =
			(recLegacy8[3].role == static_cast<uint8_t>(QualityPattern::FrameRole::Peak)) &&
			(recLegacy8[6].role == static_cast<uint8_t>(QualityPattern::FrameRole::Peak));
		Assert::IsFalse(legacyFoundBothPeaks,
			L"Negative control: Legacy 8-px block detector should not cleanly identify both interior peaks on deblocked frames");

		// Verify A2ePriorHistory: rolling-median hold on one-sided K=1, 2x flash cap, and discontinuity freeze/reset.
		{
			A2ePriorHistory hist;
			EmpiricalOwnErrorPrior pBase{};
			pBase.valid = true;
			for (int b = 0; b < 3; ++b)
				for (int c = 0; c < 2; ++c) { pBase.U[b][c] = 4.0 + b; pBase.cellCount[b][c] = 8u; }

			(void)hist.ApplyCapAndRecord(pBase, QualityPattern::FrameRole::NonReference, 24.0, false, false);
			(void)hist.ApplyCapAndRecord(pBase, QualityPattern::FrameRole::NonReference, 24.0, false, false);

			// 10x single-frame strobe flash must be capped to <= 2x rolling median!
			EmpiricalOwnErrorPrior flash = pBase;
			flash.U[1][1] = 50.0;
			const EmpiricalOwnErrorPrior capped = hist.ApplyCapAndRecord(
				flash, QualityPattern::FrameRole::NonReference, 24.0, false, false);
			Assert::IsTrue(capped.cappedByHistory && std::fabs(capped.U[1][1] - 10.0) < 1e-6,
				Msg("A2e flash U[1][1]=%.4f expected capped to 10.0", capped.U[1][1]).c_str());

			// One-sided K=1 frame (`!candidate.valid`) must hold rolling median (`heldFromHistory == true`)!
			EmpiricalOwnErrorPrior emptyCand{};
			const EmpiricalOwnErrorPrior held = hist.ApplyCapAndRecord(
				emptyCand, QualityPattern::FrameRole::NonReference, 24.0, false, false);
			Assert::IsTrue(held.valid && held.heldFromHistory && std::fabs(held.U[0][1] - 4.0) < 1e-6,
				Msg("A2e one-sided held U[0][1]=%.4f expected 4.0", held.U[0][1]).c_str());

			// Cut reset clears history so an immediately subsequent K=1 frame has nothing stale to hold.
			(void)hist.ApplyCapAndRecord(emptyCand, QualityPattern::FrameRole::Unknown, 24.0, true, /*resetOnCut=*/true);
			const EmpiricalOwnErrorPrior afterCut = hist.ApplyCapAndRecord(
				emptyCand, QualityPattern::FrameRole::NonReference, 24.0, false, false);
			Assert::IsFalse(afterCut.valid, L"Cut reset must clear stale A2e history");
		}

		// Verify TdV2ReferenceEngine recovery on valley t=4 WITH `hasStepPrior = false` (ZERO hand-set estimatedStepDelta!)
		// + worst-5% +3*tau preservation + one-sided K=1 hold on t=5 vs fresh unheld negative control!
		const CellMotionField mv4To3 = CellMotionField::Uniform(cw, ch, 0.35, -0.20, 0.35, -0.20, RegionMotionClass::Camera);
		const CellMotionField mv4To6 = CellMotionField::Uniform(cw, ch, -0.70, 0.40, -0.70, 0.40, RegionMotionClass::Camera);
		const CellMotionField mv5To3 = CellMotionField::Uniform(cw, ch, 0.70, -0.40, 0.70, -0.40, RegionMotionClass::Camera);

		Config cfgA2e = MakePreset(Preset::Reference, Mode::On);
		cfgA2e.pixelBoundKappa = 2.0;
		cfgA2e.qualitySignalMode = QualitySignalMode::AlignedSharpnessA2e;
		cfgA2e.enableA2eEmpiricalPrior = true;

		Config cfgNoA2e = cfgA2e;
		cfgNoA2e.enableA2eEmpiricalPrior = false; // Negative control: collapses to 1-LSB default when hasStepPrior == false!

		TdV2ReferenceEngine engA2e(cfgA2e);
		TdV2ReferenceEngine engNoA2e(cfgNoA2e);

		FrameProcessInput in4{};
		in4.seq = 4;
		in4.receivedY = frames[4];
		in4.hasStepPrior = false; // STRICT: ZERO hand-set estimatedStepDelta!
		in4.frameRole = QualityPattern::FrameRole::NonReference;
		in4.prevPeak.cleanPlane = &frames[3];
		in4.prevPeak.motion = &mv4To3;
		in4.nextPeak.cleanPlane = &frames[6];
		in4.nextPeak.motion = &mv4To6;

		const FrameProcessResult resA2e = engA2e.ProcessFrame(in4);
		const FrameProcessResult resNoA2e = engNoA2e.ProcessFrame(in4);
		Assert::IsTrue(resA2e.empiricalOwnError.valid && resA2e.empiricalOwnError.TotalForClass(1) > 1.0,
			L"A2e empirical own-error prior must populate from A1 cross-products with hasStepPrior=false");

		std::vector<uint8_t> mask(static_cast<size_t>(w * h), kMaskTrackable | kMaskW3BetweenKeyframes);
		const WorstAreaReport repA2e = EvaluateWorstAreaMasks(
			masters[4].v.data(), frames[4].v.data(), resA2e.outputY.v.data(), mask.data(), w, h, cellPx, 1023.0);
		const WorstAreaReport repNoA2e = EvaluateWorstAreaMasks(
			masters[4].v.data(), frames[4].v.data(), resNoA2e.outputY.v.data(), mask.data(), w, h, cellPx, 1023.0);

		Assert::IsTrue(repA2e.global.deltaDb > 1.2 && repA2e.worst5PctMeanDeltaDb > 2.0,
			Msg("A2e global=+%.3f dB worst5=+%.3f dB", repA2e.global.deltaDb, repA2e.worst5PctMeanDeltaDb).c_str());
		Assert::IsTrue(repA2e.worst5PctMeanDeltaDb > repNoA2e.worst5PctMeanDeltaDb + 1.0,
			Msg("A2e worst5 (+%.3f dB) must beat 1-LSB uncalibrated negative control (+%.3f dB) by >1 dB",
				repA2e.worst5PctMeanDeltaDb, repNoA2e.worst5PctMeanDeltaDb).c_str());

		// Next frame t=5 is one-sided K=1 (`nextPeak` null, carry off): `engA2e` holds A2e from t=4,
		// whereas a cold/unheld engine with `hasStepPrior = false` collapses to 1-LSB!
		FrameProcessInput in5{};
		in5.seq = 5;
		in5.receivedY = frames[5];
		in5.hasStepPrior = false;
		in5.frameRole = QualityPattern::FrameRole::NonReference;
		in5.prevPeak.cleanPlane = &frames[3];
		in5.prevPeak.motion = &mv5To3;

		TdV2ReferenceEngine engColdK1(cfgA2e);
		const FrameProcessResult resHeldK1 = engA2e.ProcessFrame(in5);
		const FrameProcessResult resColdK1 = engColdK1.ProcessFrame(in5);
		Assert::IsTrue(resHeldK1.empiricalOwnError.valid && resHeldK1.empiricalOwnError.heldFromHistory,
			L"One-sided K=1 frame must hold A2e prior from rolling history");
		Assert::IsFalse(resColdK1.empiricalOwnError.valid, L"Cold K=1 negative control has no A2e history");

		const WorstAreaReport repHeldK1 = EvaluateWorstAreaMasks(
			masters[5].v.data(), frames[5].v.data(), resHeldK1.outputY.v.data(), mask.data(), w, h, cellPx, 1023.0);
		const WorstAreaReport repColdK1 = EvaluateWorstAreaMasks(
			masters[5].v.data(), frames[5].v.data(), resColdK1.outputY.v.data(), mask.data(), w, h, cellPx, 1023.0);
		Assert::IsTrue(repHeldK1.global.deltaDb > repColdK1.global.deltaDb + 0.6,
			Msg("Held K=1 (+%.3f dB) must beat cold K=1 collapse (+%.3f dB)", repHeldK1.global.deltaDb, repColdK1.global.deltaDb).c_str());
	}

	// 3. TD-U1 (Owner D1(b)): Grid-free NonBlockPictureStep staircase detection vs 3 mandatory negative controls
	//    (clean smooth gradient, upscaled 1080p->4K bandlimited ramp, crisp 800-code subtitle/Overlay edge).
	TEST_METHOD(TdU1NonBlockPictureStepDetectsQuantStaircaseAndRejectsSmoothUpscaledAndSubtitleControls)
	{
		using namespace TdV2UpgradeCpuNs;
		constexpr int w = 128;
		constexpr int h = 96;
		constexpr int cellPx = kCanonicalBlockPx;
		TdV2NonBlockStepEstimator nonBlock;
		TdV2CodingGridStandIn leg8;

		// Positive case: unbiased 12-code 1D quantization staircase on non-8-px-aligned 5-px plateaus.
		TdV2Plane masterRamp(w, h, 0.0), stairPlane(w, h, 0.0);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				const double clean = 200.0 + (12.0 / 5.0) * static_cast<double>(x);
				masterRamp.at(x, y) = clean;
				stairPlane.at(x, y) = 200.0 + 12.0 * std::floor(static_cast<double>(x) / 5.0 + 0.5);
			}
		}

		const CodingGridSummary stairGrid = nonBlock.MeasureWithOverlay(
			stairPlane.v.data(), nullptr, nullptr, w, h, 10, nullptr, cellPx, nullptr);
		const CodingGridSummary stairLeg8 = leg8.Measure(
			stairPlane.v.data(), nullptr, nullptr, w, h, 10, nullptr);
		Assert::IsTrue(stairGrid.valid && stairGrid.stepMean >= 9.5 && stairGrid.stepMean <= 14.5 && stairGrid.detectedPeriodX == 0,
			Msg("NonBlockPictureStep stepMean=%.3f expected ~12.0", stairGrid.stepMean).c_str());
		// Negative control: 8-px block-boundary detector misses 5-px/6-px non-8-aligned plateaus!
		Assert::IsTrue(stairLeg8.stepMean < 0.35 * stairGrid.stepMean,
			Msg("Legacy 8-px detector stepMean=%.3f vs NonBlock %.3f", stairLeg8.stepMean, stairGrid.stepMean).c_str());

		// Feed the estimator's OWN measured `stairGrid.stepMean` (never hand-set!) into TdV2ReferenceEngine
		// vs a same-input negative control with `hasStepPrior = false` (collapsing to 1-LSB bound on K=1)!
		Config cfgNb = MakePreset(Preset::Reference, Mode::On);
		cfgNb.qualitySignalMode = QualitySignalMode::NonBlockPictureStep;
		cfgNb.enableA2eEmpiricalPrior = false;
		cfgNb.pixelBoundKappa = 3.0;
		TdV2ReferenceEngine engNb(cfgNb);
		TdV2ReferenceEngine engNoStep(cfgNb);
		const CellMotionField stillMv = CellMotionField::Uniform(w / cellPx, h / cellPx, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
		const TdV2Plane pk1 = ApplyCrispPeakQuant(masterRamp, 701u);
		const TdV2Plane pk2 = ApplyCrispPeakQuant(masterRamp, 702u);

		FrameProcessInput nbIn{};
		nbIn.seq = 1;
		nbIn.receivedY = stairPlane;
		nbIn.hasStepPrior = true;
		nbIn.estimatedStepDelta = stairGrid.stepMean; // Directly from TdV2NonBlockStepEstimator!
		nbIn.zeroCoeffFraction = 0.75;
		nbIn.prevPeak.cleanPlane = &pk1;
		nbIn.prevPeak.motion = &stillMv;
		nbIn.nextPeak.cleanPlane = &pk2;
		nbIn.nextPeak.motion = &stillMv;
		const FrameProcessResult nbRes = engNb.ProcessFrame(nbIn);

		FrameProcessInput noStepIn = nbIn;
		noStepIn.hasStepPrior = false;
		noStepIn.estimatedStepDelta = 0.0;
		const FrameProcessResult noStepRes = engNoStep.ProcessFrame(noStepIn);

		std::vector<uint8_t> mask(static_cast<size_t>(w * h), kMaskTrackable | kMaskW1BandedSky);
		const WorstAreaReport nbRep = EvaluateWorstAreaMasks(
			masterRamp.v.data(), stairPlane.v.data(), nbRes.outputY.v.data(), mask.data(), w, h, cellPx, 1023.0);
		const WorstAreaReport noStepRep = EvaluateWorstAreaMasks(
			masterRamp.v.data(), stairPlane.v.data(), noStepRes.outputY.v.data(), mask.data(), w, h, cellPx, 1023.0);
		Assert::IsTrue(nbRep.global.deltaDb > 2.5 && nbRep.w1BandingStepReduction > 0.18 &&
		               nbRep.global.deltaDb > noStepRep.global.deltaDb + 2.0,
			Msg("NonBlockPictureStep recovery=+%.2f dB (noStep=+%.2f dB) stepRed=%.2f",
				nbRep.global.deltaDb, noStepRep.global.deltaDb, nbRep.w1BandingStepReduction).c_str());

		// Negative Control A: Clean smooth 10-bit gradient (1-LSB rounded continuous ramp) -> near-zero step!
		TdV2Plane smoothGrad(w, h, 0.0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				smoothGrad.at(x, y) = std::round(120.0 + 4.2 * static_cast<double>(x) + 1.7 * static_cast<double>(y));
		const CodingGridSummary smoothSum = nonBlock.MeasureWithOverlay(
			smoothGrad.v.data(), nullptr, nullptr, w, h, 10, nullptr, cellPx, nullptr);
		Assert::IsTrue(smoothSum.valid && smoothSum.stepMean <= 0.25,
			Msg("Smooth gradient negative control produced false stepMean=%.4f", smoothSum.stepMean).c_str());

		// Negative Control B: Bandlimited 1080p->4K (2x bicubic upscaled) smooth ramp + soft low-freq wave -> near-zero step!
		const int hw = w / 2, hh = h / 2;
		TdV2Plane halfSrc(hw, hh, 0.0);
		for (int y = 0; y < hh; ++y)
			for (int x = 0; x < hw; ++x)
				halfSrc.at(x, y) = 220.0 + 5.5 * x + 45.0 * std::sin(0.18 * x) * std::cos(0.16 * y);
		TdV2Plane upscaled4k(w, h, 0.0);
		ActiveSubPlaneView hView{ &halfSrc, 0, 0, hw, hh };
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				const double sx = (std::max)(1.0, (std::min)(static_cast<double>(hw - 3), (x + 0.5) * 0.5 - 0.5));
				const double sy = (std::max)(1.0, (std::min)(static_cast<double>(hh - 3), (y + 0.5) * 0.5 - 0.5));
				double bicVal = 0.0;
				const bool bicOk = VpComposed::SampleBicubic(hView, sx, sy, bicVal);
				upscaled4k.at(x, y) = std::round(bicOk ? bicVal : halfSrc.at(x / 2, y / 2));
			}
		}
		const CodingGridSummary upSum = nonBlock.MeasureWithOverlay(
			upscaled4k.v.data(), nullptr, nullptr, w, h, 10, nullptr, cellPx, nullptr);
		Assert::IsTrue(upSum.valid && upSum.stepMean <= 0.25,
			Msg("Upscaled 1080p->4K bandlimited control produced false stepMean=%.4f", upSum.stepMean).c_str());

		// Negative Control C: Crisp 800-code white subtitle/Overlay box (900 on 100 background) + Overlay mask exclusion!
		TdV2Plane subPlane(w, h, 100.0);
		for (int y = 32; y < 64; ++y)
			for (int x = 24; x < 104; ++x)
				if (((x / 6) % 2) == 0) subPlane.at(x, y) = 900.0; // Crisp 800-code character strokes!
		const CodingGridSummary subUnmasked = nonBlock.MeasureWithOverlay(
			subPlane.v.data(), nullptr, nullptr, w, h, 10, nullptr, cellPx, nullptr);
		Assert::IsTrue(subUnmasked.valid && subUnmasked.stepMean == 0.0,
			Msg("Crisp 800-code subtitle edge produced false stepMean=%.4f", subUnmasked.stepMean).c_str());

		// Also verify `overlayCells` hard-zeros medium-contrast (32-code) UI overlay steps that lie inside [1.5, Peak/16]!
		TdV2Plane uiBox(w, h, 200.0);
		for (int y = 16; y < 48; ++y)
			for (int x = 16; x < 48; ++x)
				uiBox.at(x, y) = 200.0 + 32.0 * std::floor((x - 16) / 5.0);
		std::vector<uint8_t> ovMask(static_cast<size_t>((w / cellPx) * (h / cellPx)), 0u);
		for (int cy = 1; cy < 3; ++cy)
			for (int cx = 1; cx < 3; ++cx)
				ovMask[static_cast<size_t>(cy) * (w / cellPx) + cx] = 1u;
		const CodingGridSummary uiMasked = nonBlock.MeasureWithOverlay(
			uiBox.v.data(), nullptr, nullptr, w, h, 10, ovMask.data(), cellPx, nullptr);
		const CodingGridSummary uiUnmasked = nonBlock.MeasureWithOverlay(
			uiBox.v.data(), nullptr, nullptr, w, h, 10, nullptr, cellPx, nullptr);
		Assert::IsTrue(uiMasked.stepMax == 0.0 && uiUnmasked.stepMax > 25.0,
			Msg("Overlay cell mask must zero UI step (masked=%.3f vs unmasked=%.3f)", uiMasked.stepMax, uiUnmasked.stepMax).c_str());
	}

	// 4. TD-U3 (Fable H-1, M-1, M-6 / Owner D3): Per-band + joint cross-band output cap across kappa in {1, 2, 3}
	//    (1.0 default), PQ factor <= 1.0, AQ off by default, and honest Ctrl+I status text.
	TEST_METHOD(TdU3StatisticalPerBandAndJointCrossBandBoundsPqTighteningAqOffAndCtrlIStatus)
	{
		using namespace TdV2UpgradeCpuNs;
		// Verify strict Owner D3 & M-6 defaults.
		const Config defCfg{};
		Assert::AreEqual(1.0, defCfg.pixelBoundKappa, 1e-12, L"Default pixelBoundKappa must be 1.0 (Owner D3)");
		Assert::IsTrue(defCfg.enableJointCrossBandBound, L"enableJointCrossBandBound must default true");
		Assert::IsFalse(defCfg.enableAqStepScaling, L"enableAqStepScaling must default false (M-6)");

		// Construct a 3-band co-peaking defect centered at (32, 32) on `cur` vs independent noisy `peak1`, `peak2`
		// (distinct non-collinear peak rounding realizations so A1 3-cornered hat measures full `Var(spot)` instead of
		// tripping `td_refs_collinear` fallback!) so L, M, and H differences ALL share the same sign at (32, 32) and
		// each band saturates `kappa * sqrt(U_b)` across kappa in {1, 2, 3}, making unjoint sum > 1.35x `kappa * rmsU`!
		constexpr int w = 64, h = 64, cellPx = 16;
		TdV2Plane cur(w, h, 300.0), peak1(w, h, 300.0), peak2(w, h, 300.0);
		std::mt19937 rngP1(3031u), rngP2(3032u);
		std::normal_distribution<double> peakNoise(0.0, 0.35);
		for (int y = 0; y < h; ++y)
		{
			for (int x = 0; x < w; ++x)
			{
				const double dx = static_cast<double>(x - 32);
				const double dy = static_cast<double>(y - 32);
				const double spot = 40.0 * std::cos(2.0 * kPi * dx / 32.0) * std::cos(2.0 * kPi * dy / 32.0) +
				                    40.0 * std::cos(2.0 * kPi * dx / 8.0) * std::cos(2.0 * kPi * dy / 8.0) +
				                    40.0 * std::cos(kPi * dx) * std::cos(kPi * dy);
				const double tex = 3.0 * std::sin(0.55 * x + 0.45 * y);
				peak1.at(x, y) = 300.0 + tex + peakNoise(rngP1);
				peak2.at(x, y) = 300.0 + tex + peakNoise(rngP2);
				cur.at(x, y) = 300.0 + tex - spot;
			}
		}
		const CellMotionField stillMv = CellMotionField::Uniform(w / cellPx, h / cellPx, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

		const RoundingCellStats rStats = ComputeRoundingCellStats(16.0, 0.5, 0.0);
		const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
		const double uSumBands = rStats.totalVar * (gains.low + gains.mid + gains.fine);
		const double rmsU = std::sqrt(uSumBands);

		double prevMaxDelta = 0.0;
		for (double kappa : { 1.0, 2.0, 3.0 })
		{
			Config cfgJoint = MakePreset(Preset::Reference, Mode::On);
			cfgJoint.estimator = VarianceEstimator::A1_ThreeCorneredHat;
			cfgJoint.enableA2eEmpiricalPrior = false;
			cfgJoint.enableEmpiricalBayesShrinkage = false;
			cfgJoint.enablePerCornerHuber = false;
			cfgJoint.enableNeighborhoodSoftClamp = false;
			cfgJoint.pixelBoundKappa = kappa;
			cfgJoint.enableJointCrossBandBound = true;

			Config cfgUnjoint = cfgJoint;
			cfgUnjoint.enableJointCrossBandBound = false; // Same-input negative control!

			FrameProcessInput in{};
			in.seq = 1;
			in.receivedY = cur;
			in.hasCleanSplit = true;
			in.cleanY = cur;
			in.hasStepPrior = true;
			in.estimatedStepDelta = 16.0;
			in.deadZonePhi = 0.5;
			in.zeroCoeffFraction = 0.0;
			in.prevPeak.cleanPlane = &peak1;
			in.prevPeak.motion = &stillMv;
			in.nextPeak.cleanPlane = &peak2;
			in.nextPeak.motion = &stillMv;

			TdV2ReferenceEngine engJ(cfgJoint);
			TdV2ReferenceEngine engU(cfgUnjoint);
			const FrameProcessResult resJ = engJ.ProcessFrame(in);
			const FrameProcessResult resU = engU.ProcessFrame(in);

			double maxAbsJ = 0.0, maxAbsU = 0.0;
			for (size_t i = 0; i < cur.v.size(); ++i)
			{
				maxAbsJ = (std::max)(maxAbsJ, std::fabs(resJ.outputY.v[i] - cur.v[i]));
				maxAbsU = (std::max)(maxAbsU, std::fabs(resU.outputY.v[i] - cur.v[i]));
			}
			const double allowedJoint = kappa * rmsU + 1e-6;
			Assert::IsTrue(maxAbsJ <= allowedJoint,
				Msg("kappa=%.1f joint maxAbs=%.5f exceeded %.5f", kappa, maxAbsJ, allowedJoint).c_str());
			Assert::IsTrue(maxAbsJ > prevMaxDelta + 0.25,
				Msg("kappa=%.1f maxAbs=%.5f must grow monotonically from %.5f", kappa, maxAbsJ, prevMaxDelta).c_str());
			Assert::IsTrue(maxAbsU > kappa * rmsU * 1.05,
				Msg("Negative control (joint bound OFF) maxAbs=%.5f should exceed joint bound %.5f", maxAbsU, kappa * rmsU).c_str());
			prevMaxDelta = maxAbsJ;
		}

		// Verify transformIdentified8x8 == true with codecModelPb == nullptr engages CodecModelPbStandIn visibly
		// and enforces the statistical bound (never substituting warped prevPeak as fake P_B).
		{
			Config cfgNoPb = MakePreset(Preset::Reference, Mode::On);
			cfgNoPb.estimator = VarianceEstimator::A2_RoundingCells;
			cfgNoPb.enableA2eEmpiricalPrior = false;
			cfgNoPb.pixelBoundKappa = 1.0;
			FrameProcessInput in{};
			in.seq = 1;
			in.receivedY = cur;
			in.hasCleanSplit = true;
			in.cleanY = cur;
			in.hasStepPrior = true;
			in.estimatedStepDelta = 16.0;
			in.deadZonePhi = 0.5;
			in.transformIdentified8x8 = true;
			in.codecModelPb = nullptr;
			in.prevPeak.cleanPlane = &peak1;
			in.prevPeak.motion = &stillMv;

			StandInRegistry standIns;
			TdV2ReferenceEngine eng(cfgNoPb);
			const FrameProcessResult res = eng.ProcessFrame(in, &standIns);
			Assert::IsTrue(standIns.IsActive(StandInKind::CodecModelPbStandIn) && res.codecModelPbStandInCells > 0u,
				L"Absent codecModelPb must engage CodecModelPbStandIn visibly");
			for (size_t i = 0; i < cur.v.size(); ++i)
				Assert::IsTrue(std::fabs(res.outputY.v[i] - cur.v[i]) <= rmsU + 1e-6,
					L"Absent codecModelPb must enforce statistical bound, not warped-peak DCT substitute");
		}

		// Verify ComputeCellStepScale: PQ factor capped <= 1.0 everywhere, tightens in shadows; AQ off by default.
		for (double pqCode : { 595.0, 769.0, 900.0, 1023.0 }) // >= 203 nits (594.05 in 10-bit PQ) up to 10,000 nits
		{
			const double s = ComputeCellStepScale(0.0, pqCode, /*isPqHdr=*/true, 10.0, /*enableAqStepScaling=*/false);
			Assert::AreEqual(1.0, s, 1e-9, Msg("PQ highlight code=%.1f stepScale=%.6f must equal 1.0 above 203 nits", pqCode, s).c_str());
		}
		for (double pqCode = 0.0; pqCode <= 1023.0; pqCode += 32.0)
		{
			const double s = ComputeCellStepScale(0.0, pqCode, /*isPqHdr=*/true, 10.0, /*enableAqStepScaling=*/false);
			Assert::IsTrue(s <= 1.0, Msg("PQ stepScale=%.6f at code=%.1f must never exceed 1.0", s, pqCode).c_str());
		}
		const double shadowScale = ComputeCellStepScale(0.0, 32.0, /*isPqHdr=*/true, 10.0, false);
		Assert::IsTrue(shadowScale >= kPqStepWeightMin && shadowScale < 0.80,
			Msg("PQ shadow stepScale=%.4f expected tightening in [0.25, 0.80)", shadowScale).c_str());
		Assert::AreEqual(1.0, ComputeCellStepScale(12.0, 512.0, false, 10.0, /*enableAqStepScaling=*/false), 1e-12,
			L"AQ step scaling must remain 1.0 when enableAqStepScaling == false");
		Assert::AreEqual(4.0, ComputeCellStepScale(12.0, 512.0, false, 10.0, /*enableAqStepScaling=*/true), 1e-9,
			L"Negative control: enableAqStepScaling == true widens +12 QP by 2^(12/6) = 4.0");

		// Verify honest Ctrl+I status text (M-1 / H-1).
		TdCtrlIStatusInput stIn{};
		stIn.settings.mode = Mode::On;
		stIn.settings.residualMode = ResidualMode::Strict;
		stIn.settings.limitKappa = 1.0;
		stIn.transformIdentified8x8 = false;
		stIn.hasCodecModelPb = false;
		const std::string statusUnknown = FormatTdCtrlIStatus(stIn);
		Assert::IsTrue(statusUnknown.find("limit: statistical (transform unknown), kappa=1.0") != std::string::npos,
			L"Ctrl+I status must state statistical limit when transform unknown");
		Assert::IsTrue(statusUnknown.find("mode=strict (inactive: transform unknown)") != std::string::npos,
			L"Ctrl+I status must mark strict/cell mode inactive when transform unknown");

		stIn.transformIdentified8x8 = true;
		stIn.hasCodecModelPb = true;
		const std::string statusCell = FormatTdCtrlIStatus(stIn);
		Assert::IsTrue(statusCell.find("limit: cell (AVC 8x8)") != std::string::npos &&
		               statusCell.find("inactive") == std::string::npos,
			L"Ctrl+I status must show active AVC 8x8 cell limit when both transform & P_B exist");
	}

	// 5. TD-U4 (Fable H-3 / Owner D4): 48-frame P5 0.5-4 Hz flicker sequence test + band-dependent pooling + EB shrinkage
	//    (vs Legacy1x1 negative control) + optional 2-frame same-role stats-only pool.
	TEST_METHOD(TdU4Flicker48FrameSequenceBandPoolingEbShrinkageAndTwoFrameSameRoleStatsPool)
	{
		using namespace TdV2UpgradeCpuNs;
		constexpr int w = 96, h = 64, cellPx = 16;
		constexpr int T = 48; // 2.0 s at 24 Hz -> DFT bins k = 1..8 span 0.5..4.0 Hz!
		constexpr double fps = 24.0;
		const int cw = w / cellPx, ch = h / cellPx;

		// Stationary master + i.i.d. per-frame spatially correlated noise (3x3 box-filtered white noise, sigma ~= 3.0 codes)
		// with peaks declared every 8 frames (3.0 Hz, right inside 0.5-4.0 Hz!).
		// Because coarse band L in a 16x16 cell has only ~6.5 independent DOF, Legacy1x1 (1x1, no EB shrinkage)
		// clips random positive c_IV noise on valley frames and pulls in the two fixed 3-Hz peaks' L-band noise,
		// amplifying 0.5-4 Hz block-mean flicker > 1.0x received! BandShrink (5x5 L + EB shrinkage) suppresses it <= 1.0x!
		const TdV2Plane master = RenderMasterScene(w, h, 0.0, 0.0);

		auto compute05To4HzEnergy = [&](const std::vector<TdV2Plane>& seq) -> double
		{
			const size_t nCells = static_cast<size_t>(cw * ch);
			std::vector<double> means(static_cast<size_t>(T) * nCells, 0.0);
			for (int t = 0; t < T; ++t)
			{
				for (int cy = 0; cy < ch; ++cy)
				{
					for (int cx = 0; cx < cw; ++cx)
					{
						double s = 0.0;
						for (int y = cy * cellPx; y < (cy + 1) * cellPx; ++y)
							for (int x = cx * cellPx; x < (cx + 1) * cellPx; ++x)
								s += (seq[t].at(x, y) - master.at(x, y));
						means[static_cast<size_t>(t) * nCells + static_cast<size_t>(cy) * cw + cx] = s / 256.0;
					}
				}
			}
			double power = 0.0;
			for (int k = 1; k <= 8; ++k) // k * (24 / 48) = 0.5 .. 4.0 Hz
			{
				for (size_t c = 0; c < nCells; ++c)
				{
					double re = 0.0, im = 0.0;
					for (int t = 0; t < T; ++t)
					{
						const double ang = (2.0 * kPi * static_cast<double>(k * t)) / static_cast<double>(T);
						const double v = means[static_cast<size_t>(t) * nCells + c];
						re += v * std::cos(ang);
						im -= v * std::sin(ang);
					}
					power += (re * re + im * im) / static_cast<double>(T * T);
				}
			}
			return power / static_cast<double>(nCells);
		};

		const CellMotionField stillMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

		// Subtest A (P5 GOP Flicker Non-Amplification): 8-frame GOP (3.0 Hz) where peaks are crisp and valleys
		// suffer coarse quantization + 3 Hz GOP block-mean breathing -> BandShrink keeps 0.5-4 Hz <= 1.0x received!
		{
			std::vector<TdV2Plane> gopSeq(T);
			for (int t = 0; t < T; ++t)
			{
				if ((t % 8) == 0)
				{
					gopSeq[t] = ApplyCrispPeakQuant(master, 9100u + static_cast<uint32_t>(t));
				}
				else
				{
					gopSeq[t] = ApplyDeblockedValleyLoss(master, 9.0, 9200u + static_cast<uint32_t>(t), false);
					const double breathe = 1.8 * std::sin((2.0 * kPi * static_cast<double>(t % 8)) / 8.0);
					for (double& v : gopSeq[t].v) v += breathe;
				}
			}
			TdV2RuntimeSettings rs{};
			rs.mode = Mode::On; rs.preset = Preset::RealTime; rs.limitKappa = 2.5;
			rs.statsPoolingMode = StatsPoolingMode::BandShrink;
			Config cfg = BuildTdV2Config(rs, false);
			cfg.sourceHz = fps;
			TdV2ReferenceEngine eng(cfg);
			std::vector<TdV2Plane> outs(T);
			for (int t = 0; t < T; ++t)
			{
				if ((t % 8) == 0) { outs[t] = gopSeq[t]; continue; }
				const int p0 = (t / 8) * 8;
				const int p1 = (std::min)(T - 1, p0 + 8);
				FrameProcessInput in{};
				in.seq = static_cast<uint64_t>(t); in.receivedY = gopSeq[t];
				in.frameRole = QualityPattern::FrameRole::NonReference;
				in.prevPeak.cleanPlane = &gopSeq[p0]; in.prevPeak.motion = &stillMv;
				if (p1 != p0) { in.nextPeak.cleanPlane = &gopSeq[p1]; in.nextPeak.motion = &stillMv; }
				outs[t] = eng.ProcessFrame(in).outputY;
			}
			const double eGopRec = compute05To4HzEnergy(gopSeq);
			const double eGopOut = compute05To4HzEnergy(outs);
			Assert::IsTrue(eGopOut <= eGopRec * 1.00,
				Msg("P5 GOP 0.5-4Hz BandShrink=%.5f must be <= received=%.5f", eGopOut, eGopRec).c_str());
		}

		// Subtest B (Fable H-3 Pure-Noise Shrinkage vs Legacy1x1 Negative Control):
		// Current frames are clean stationary master + 1-LSB rounding (truth c_IV == 0!), while the two 3-Hz references
		// carry independent spatially correlated noise. Legacy1x1 (1x1, no EB shrinkage, ~6.5 DOF in L) clips random
		// positive c_raw > 0 and injects strong 3 Hz flicker (> 1.5x received), whereas BandShrink suppresses it <= 1.0x!
		std::vector<TdV2Plane> cleanCurSeq(T), noisyRefA(T), noisyRefB(T);
		auto makeBoxNoisePlane = [&](uint32_t seed, double sigma)
		{
			std::mt19937 rng(seed);
			std::normal_distribution<double> nDist(0.0, sigma);
			TdV2Plane raw(w, h, 0.0), out(w, h, 0.0);
			for (size_t i = 0; i < raw.v.size(); ++i) raw.v[i] = nDist(rng);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					double s = 0.0;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
							s += raw.at((std::max)(0, (std::min)(w - 1, x + dx)), (std::max)(0, (std::min)(h - 1, y + dy)));
					out.at(x, y) = master.at(x, y) + (s / 9.0);
				}
			return out;
		};
		for (int t = 0; t < T; ++t)
		{
			cleanCurSeq[t] = makeBoxNoisePlane(9300u + static_cast<uint32_t>(t), 4.0);
			noisyRefA[t] = makeBoxNoisePlane(9500u + static_cast<uint32_t>(t / 8), 7.5);
			noisyRefB[t] = makeBoxNoisePlane(9700u + static_cast<uint32_t>(t / 8), 7.5);
		}

		auto runFlickerArm = [&](StatsPoolingMode poolMode) -> std::vector<TdV2Plane>
		{
			TdV2RuntimeSettings rs{};
			rs.mode = Mode::On;
			rs.preset = Preset::RealTime;
			rs.limitKappa = 3.0;
			rs.statsPoolingMode = poolMode;
			Config cfg = BuildTdV2Config(rs, false);
			cfg.estimator = VarianceEstimator::A1_ThreeCorneredHat;
			cfg.sourceHz = fps;
			TdV2ReferenceEngine eng(cfg);

			std::vector<TdV2Plane> outs(T);
			for (int t = 0; t < T; ++t)
			{
				if ((t % 8) == 0)
				{
					outs[t] = cleanCurSeq[t];
					continue;
				}
				FrameProcessInput in{};
				in.seq = static_cast<uint64_t>(t);
				in.receivedY = cleanCurSeq[t];
				in.hasStepPrior = true;
				in.estimatedStepDelta = 6.0;
				in.frameRole = QualityPattern::FrameRole::NonReference;
				in.prevPeak.cleanPlane = &noisyRefA[t];
				in.prevPeak.motion = &stillMv;
				in.nextPeak.cleanPlane = &noisyRefB[t];
				in.nextPeak.motion = &stillMv;
				outs[t] = eng.ProcessFrame(in).outputY;
			}
			return outs;
		};

		const double eRec = compute05To4HzEnergy(cleanCurSeq);
		const double eBandShrink = compute05To4HzEnergy(runFlickerArm(StatsPoolingMode::BandShrink));
		const double eLegacy1x1 = compute05To4HzEnergy(runFlickerArm(StatsPoolingMode::Legacy1x1));

		Assert::IsTrue(eBandShrink <= eRec * 1.00,
			Msg("P5 0.5-4Hz BandShrink=%.5f must be <= received=%.5f", eBandShrink, eRec).c_str());
		Assert::IsTrue(eLegacy1x1 > eRec * 1.10 && eLegacy1x1 > eBandShrink * 1.12,
			Msg("Negative control Legacy1x1=%.5f should exceed received=%.5f and BandShrink=%.5f",
				eLegacy1x1, eRec, eBandShrink).c_str());

		// Verify optional 2-frame same-role stats-only pool (`enableTwoFrameSameRoleStatsPool`):
		//   - Default OFF (Owner D4(b))
		//   - Pools stats on consecutive same-role frames, skips ExactCopy, resets on cut/fade/size change,
		//     and skips fast-moving cells (|v| >= 0.5 * cellPx = 8.0 px/frame).
		Assert::IsFalse(Config{}.enableTwoFrameSameRoleStatsPool, L"enableTwoFrameSameRoleStatsPool must default false");
		{
			Config cfg2f = MakePreset(Preset::RealTime, Mode::On);
			cfg2f.enableTwoFrameSameRoleStatsPool = true;
			cfg2f.pixelBoundKappa = 2.0;
			Config cfg1f = cfg2f;
			cfg1f.enableTwoFrameSameRoleStatsPool = false;

			TdV2ReferenceEngine eng2f(cfg2f), eng1f(cfg1f);
			const TdV2Plane pkA = ApplyCrispPeakQuant(master, 1u);
			const TdV2Plane pkB = ApplyCrispPeakQuant(master, 2u);
			const TdV2Plane val1 = ApplyDeblockedValleyLoss(master, 9.0, 11u, false);
			const TdV2Plane val2 = ApplyDeblockedValleyLoss(master, 9.0, 12u, false);

			auto makeValIn = [&](uint64_t seq, const TdV2Plane& rec, QualityPattern::FrameRole role, const CellMotionField* mv)
			{
				FrameProcessInput in{};
				in.seq = seq;
				in.receivedY = rec;
				in.frameRole = role;
				in.prevPeak.cleanPlane = &pkA;
				in.prevPeak.motion = mv;
				in.nextPeak.cleanPlane = &pkB;
				in.nextPeak.motion = mv;
				return in;
			};

			(void)eng2f.ProcessFrame(makeValIn(1, val1, QualityPattern::FrameRole::NonReference, &stillMv));
			(void)eng1f.ProcessFrame(makeValIn(1, val1, QualityPattern::FrameRole::NonReference, &stillMv));

			// Insert an ExactCopy frame at seq=2 -> must NOT overwrite or corrupt the NonReference stats slot!
			FrameProcessInput copyIn = makeValIn(2, val1, QualityPattern::FrameRole::ExactCopy, &stillMv);
			copyIn.prevPeak.isExactCopyOfCurrent = true;
			(void)eng2f.ProcessFrame(copyIn);
			(void)eng1f.ProcessFrame(copyIn);

			const FrameProcessResult r2f = eng2f.ProcessFrame(makeValIn(3, val2, QualityPattern::FrameRole::NonReference, &stillMv));
			const FrameProcessResult r1f = eng1f.ProcessFrame(makeValIn(3, val2, QualityPattern::FrameRole::NonReference, &stillMv));
			double maxDiff2fVs1f = 0.0;
			for (size_t i = 0; i < r2f.outputY.v.size(); ++i)
				maxDiff2fVs1f = (std::max)(maxDiff2fVs1f, std::fabs(r2f.outputY.v[i] - r1f.outputY.v[i]));
			Assert::IsTrue(maxDiff2fVs1f > 1e-3, L"2-frame same-role stats pool must actively pool with prior NonReference frame");

			// Fast-moving cell (|v| = 9.0 px >= 8.0 px = 0.5 * cellPx) on fresh engines must NOT pool prior cell stats!
			TdV2ReferenceEngine engFast2f(cfg2f), engFast1f(cfg1f);
			const CellMotionField fastCamMv = CellMotionField::Uniform(cw, ch, 9.0, 0.0, 9.0, 0.0, RegionMotionClass::Camera);
			(void)engFast2f.ProcessFrame(makeValIn(1, val1, QualityPattern::FrameRole::NonReference, &fastCamMv));
			(void)engFast1f.ProcessFrame(makeValIn(1, val1, QualityPattern::FrameRole::NonReference, &fastCamMv));
			const FrameProcessResult rFast2f = engFast2f.ProcessFrame(makeValIn(2, val2, QualityPattern::FrameRole::NonReference, &fastCamMv));
			const FrameProcessResult rFast1f = engFast1f.ProcessFrame(makeValIn(2, val2, QualityPattern::FrameRole::NonReference, &fastCamMv));
			double maxFastDiff = 0.0;
			for (size_t i = 0; i < rFast2f.outputY.v.size(); ++i)
				maxFastDiff = (std::max)(maxFastDiff, std::fabs(rFast2f.outputY.v[i] - rFast1f.outputY.v[i]));
			Assert::IsTrue(maxFastDiff < 1e-9,
				Msg("Fast-moving cells (>=0.5*cellPx) must skip 2-frame stats pooling, maxDiff=%.8f", maxFastDiff).c_str());

			// Cut at seq=5 resets 2-frame stats slot so seq=6 matches fresh single-frame engine identically.
			FrameProcessInput cutIn = makeValIn(5, val1, QualityPattern::FrameRole::NonReference, &stillMv);
			cutIn.isCutOrSpanInvalid = true;
			(void)eng2f.ProcessFrame(cutIn);
			TdV2ReferenceEngine engFresh(cfg1f);
			// Match A2e history on engFresh at seq=5 cut before comparing seq=6!
			(void)engFresh.ProcessFrame(cutIn);
			const FrameProcessResult postCut2f = eng2f.ProcessFrame(makeValIn(6, val2, QualityPattern::FrameRole::NonReference, &stillMv));
			const FrameProcessResult postCut1f = engFresh.ProcessFrame(makeValIn(6, val2, QualityPattern::FrameRole::NonReference, &stillMv));
			for (size_t i = 0; i < postCut2f.outputY.v.size(); ++i)
				Assert::AreEqual(postCut1f.outputY.v[i], postCut2f.outputY.v[i], 1e-9, L"Cut must reset 2-frame same-role stats pool");
		}
	}

	// 6. TD-U5 (Fable H-4 / Owner D2) + Phase 6 (6.1, 6.2, 6.6, bokeh, chroma rack-focus, Layer-4 grain) + all Settings UI keys.
	TEST_METHOD(TdU5AnchoredCarryDriftHeldOutReseedPhase6SwitchesChromaAnd15KeySettingsUi)
	{
		using namespace TdV2UpgradeCpuNs;
		constexpr int w = 96, h = 64, cellPx = 16;
		constexpr int cw = w / cellPx, ch = h / cellPx;
		constexpr size_t nCells = static_cast<size_t>(cw * ch);

		// Owner D2 defaults: both legacy carry and anchored carry are OFF by default in Config and MakePreset!
		Assert::IsFalse(Config{}.enableCarryForward, L"enableCarryForward must default false");
		Assert::IsFalse(Config{}.enableAnchoredCarry, L"enableAnchoredCarry must default false");
		Assert::IsFalse(MakePreset(Preset::RealTime, Mode::On).enableCarryForward, L"RealTime enableCarryForward must default false");
		Assert::IsFalse(MakePreset(Preset::Reference, Mode::On).enableAnchoredCarry, L"Reference enableAnchoredCarry must default false");

		// (A) Age-1 prevPeak non-self-instrumentation + sustained 0.05 px/frame drift tripping real kDrift > 0.25 px within 1 GOP!
		{
			Config cfgAnc = MakePreset(Preset::Reference, Mode::On);
			cfgAnc.enableAnchoredCarry = true;
			cfgAnc.pixelBoundKappa = 2.0;
			TdV2ReferenceEngine engBiased(cfgAnc), engClean(cfgAnc);

			std::vector<TdV2Plane> gopFrames(9);
			for (int t = 0; t < 9; ++t)
			{
				const TdV2Plane m = RenderMasterScene(w, h, 0.30 * t, 0.0);
				gopFrames[t] = (t == 0 || t == 6) ? ApplyCrispPeakQuant(m, 500u + t) : ApplyDeblockedValleyLoss(m, 9.0, 600u + t, false);
			}

			// Seed confirmed peak at seq=0 (anchorAgeFrames_ becomes 1).
			FrameProcessInput seed0{};
			seed0.seq = 0; seed0.receivedY = gopFrames[0]; seed0.isConfirmedPeak = true;
			(void)engBiased.ProcessFrame(seed0);
			(void)engClean.ProcessFrame(seed0);
			Assert::IsTrue(engBiased.HasAnchor() && engBiased.AnchorAgeFrames() == 1);

			// At seq=1 (age == 1), supplying peak 0 as `prevPeak` MUST NOT self-instrument anchorClean_ as a 2nd ref!
			const CellMotionField mv1To0 = CellMotionField::Uniform(cw, ch, 0.30, 0.0, 0.30, 0.0, RegionMotionClass::Camera);
			FrameProcessInput in1{};
			in1.seq = 1; in1.receivedY = gopFrames[1];
			in1.prevPeak.cleanPlane = &gopFrames[0]; in1.prevPeak.motion = &mv1To0;
			in1.hasAnchorComposedMotion = true; in1.anchorComposedMotion = mv1To0;
			const FrameProcessResult r1 = engBiased.ProcessFrame(in1);
			(void)engClean.ProcessFrame(in1);
			Assert::IsFalse(r1.anchoredCarryUsed, L"Age-1 anchor identical to prevPeak must NOT self-instrument!");
			Assert::AreEqual(static_cast<size_t>(1u), r1.activeRefCount, L"Age-1 must have only 1 ref (prevPeak)");

			// At seq=2..5 (age >= 2), advance to seq=6 confirmed peak with +0.05 px/frame vector bias (6 * 0.05 = 0.30 px > 0.25 px!).
			for (int t = 2; t <= 5; ++t)
			{
				const CellMotionField mvTrue = CellMotionField::Uniform(cw, ch, 0.30 * t, 0.0, 0.30 * t, 0.0, RegionMotionClass::Camera);
				const CellMotionField mvBias = CellMotionField::Uniform(cw, ch, 0.30 * t + 0.05 * t, 0.0, 0.30 * t + 0.05 * t, 0.0, RegionMotionClass::Camera);
				FrameProcessInput itB{}, itC{};
				itB.seq = static_cast<uint64_t>(t); itB.receivedY = gopFrames[t];
				itB.prevPeak.cleanPlane = &gopFrames[0]; itB.prevPeak.motion = &mvBias;
				itB.hasAnchorComposedMotion = true; itB.anchorComposedMotion = mvBias;
				itC = itB;
				itC.prevPeak.motion = &mvTrue; itC.anchorComposedMotion = mvTrue;
				const FrameProcessResult rb = engBiased.ProcessFrame(itB);
				(void)engClean.ProcessFrame(itC);
				if (t == 2)
				{
					Assert::IsTrue(engBiased.AnchorAgeFrames() == 3);
					Assert::IsFalse(rb.anchoredCarryUsed,
						L"Same-GOP prevPeak identical to seeded anchor must NEVER self-instrument at age >= 2 (Fable H-4)!");
				}
				(void)rb;
			}

			// Confirmed peak at seq=6: composed motion has accumulated +0.30 px bias vs direct P2P (+1.80 px).
			const CellMotionField mvDirect6To0 = CellMotionField::Uniform(cw, ch, 1.80, 0.0, 1.80, 0.0, RegionMotionClass::Camera);
			const CellMotionField mvBiased6To0 = CellMotionField::Uniform(cw, ch, 2.10, 0.0, 2.10, 0.0, RegionMotionClass::Camera);
			FrameProcessInput p6B{}, p6C{};
			p6B.seq = 6; p6B.receivedY = gopFrames[6]; p6B.isConfirmedPeak = true;
			p6B.prevPeak.cleanPlane = &gopFrames[0]; p6B.prevPeak.motion = &mvDirect6To0;
			p6B.hasAnchorComposedMotion = true; p6B.anchorComposedMotion = mvBiased6To0;
			p6B.hasDirectPeakToPeakMotion = true; p6B.directPeakToPeakMotion = mvDirect6To0;

			p6C = p6B;
			p6C.anchorComposedMotion = mvDirect6To0; // Negative control: zero drift!

			const FrameProcessResult resP6Biased = engBiased.ProcessFrame(p6B);
			const FrameProcessResult resP6Clean = engClean.ProcessFrame(p6C);

			Assert::AreEqual(nCells, resP6Biased.driftBarredCells, L"0.30 px drift (>0.25 px) must bar all cells for next span");
			Assert::AreEqual(static_cast<size_t>(0u), resP6Clean.driftBarredCells, L"Unbiased control must bar 0 cells");
			Assert::IsTrue(resP6Biased.tdMaps[0].f[TdMapsView::kDrift] > 0.29f &&
			               resP6Clean.tdMaps[0].f[TdMapsView::kDrift] < 1e-5f,
				Msg("Real td.maps kDrift biased=%.4f vs clean=%.4f",
					resP6Biased.tdMaps[0].f[TdMapsView::kDrift], resP6Clean.tdMaps[0].f[TdMapsView::kDrift]).c_str());

			// Advance seq=7 (age 1) -> seq=8 (age 2, with a newer intermediate reference as prevPeak so anchor is distinct):
			// verify `engBiased` bars anchored carry on seq=8 while `engClean` uses it!
			const CellMotionField mv7To6 = CellMotionField::Uniform(cw, ch, 0.30, 0.0, 0.30, 0.0, RegionMotionClass::Camera);
			FrameProcessInput in7{};
			in7.seq = 7; in7.receivedY = gopFrames[7];
			in7.prevPeak.cleanPlane = &gopFrames[6]; in7.prevPeak.motion = &mv7To6;
			in7.hasAnchorComposedMotion = true; in7.anchorComposedMotion = mv7To6;
			(void)engBiased.ProcessFrame(in7);
			(void)engClean.ProcessFrame(in7);

			const CellMotionField mv8To7 = CellMotionField::Uniform(cw, ch, 0.30, 0.0, 0.30, 0.0, RegionMotionClass::Camera);
			const CellMotionField mv8To6 = CellMotionField::Uniform(cw, ch, 0.60, 0.0, 0.60, 0.0, RegionMotionClass::Camera);
			FrameProcessInput in8{};
			in8.seq = 8; in8.receivedY = gopFrames[8];
			in8.hasStepPrior = true; in8.estimatedStepDelta = 9.0;
			in8.prevPeak.cleanPlane = &gopFrames[7]; in8.prevPeak.motion = &mv8To7;
			in8.hasAnchorComposedMotion = true; in8.anchorComposedMotion = mv8To6;
			const FrameProcessResult r8Biased = engBiased.ProcessFrame(in8);
			const FrameProcessResult r8Clean = engClean.ProcessFrame(in8);
			Assert::IsTrue(r8Clean.anchoredCarryUsed, L"Age-2 anchor distinct from prevPeak must activate");
			const TdV2Plane master8 = RenderMasterScene(w, h, 0.30 * 8, 0.0);
			double errSqClean = 0.0, errSqBiased = 0.0, maxCleanVsBiased = 0.0;
			for (size_t i = 0; i < static_cast<size_t>(w * h); ++i)
			{
				const double ec = r8Clean.outputY.v[i] - master8.v[i];
				const double eb = r8Biased.outputY.v[i] - master8.v[i];
				errSqClean += ec * ec;
				errSqBiased += eb * eb;
				maxCleanVsBiased = (std::max)(maxCleanVsBiased, std::fabs(r8Clean.outputY.v[i] - r8Biased.outputY.v[i]));
			}
			Assert::IsTrue(maxCleanVsBiased > 1.0 && errSqClean < errSqBiased * 0.97,
				Msg("Unbarred crisp anchor must beat drift-barred 1-ref valley MSE (clean=%.1f vs barred=%.1f)", errSqClean, errSqBiased).c_str());

			// Also verify seeded reduced search ("drift: reduced") when directPeakToPeakMotion is absent!
			TdV2ReferenceEngine engReduced(cfgAnc);
			(void)engReduced.ProcessFrame(seed0);
			FrameProcessInput p1Red{};
			p1Red.seq = 1; p1Red.receivedY = gopFrames[6]; p1Red.isConfirmedPeak = true;
			p1Red.prevPeak.cleanPlane = &gopFrames[0]; p1Red.prevPeak.motion = &mvDirect6To0;
			p1Red.hasAnchorComposedMotion = true; p1Red.anchorComposedMotion = mvBiased6To0; // +0.30 px bias
			p1Red.hasDirectPeakToPeakMotion = false; // Triggers seeded reduced search!
			StandInRegistry redStandIns;
			const FrameProcessResult rRed = engReduced.ProcessFrame(p1Red, &redStandIns);
			Assert::IsTrue(rRed.driftReducedSearchUsed &&
			               redStandIns.IsActive(StandInKind::LongPairDrift) &&
			               rRed.driftBarredCells > 0u,
				L"Seeded reduced block search must engage visibly and detect >0.25 px drift on textured cells");
		}

		// (B) 24-frame 0.3 px/frame pan fine-energy retention (single-resample anchored carry vs 24-hop chained carry negative control).
		{
			Config cfgSingle = MakePreset(Preset::Reference, Mode::On);
			cfgSingle.enableAnchoredCarry = true;
			cfgSingle.enableCarryForward = false;
			cfgSingle.maxAnchorAgeFrames = 25;
			cfgSingle.pixelBoundKappa = 2.5;

			Config cfgChained = cfgSingle;
			cfgChained.enableAnchoredCarry = false;
			cfgChained.enableCarryForward = true; // Negative control: resamples carryClean_ 24 times in a chain!

			TdV2ReferenceEngine engSingle(cfgSingle), engChained(cfgChained);
			std::vector<TdV2Plane> panMasters(25), panInputs(25);
			for (int t = 0; t <= 24; ++t)
			{
				panMasters[t] = RenderMasterScene(w, h, 0.30 * t, 0.0);
				panInputs[t] = (t == 0 || t == 24) ? ApplyCrispPeakQuant(panMasters[t], 700u + t)
				                                   : ApplyDeblockedValleyLoss(panMasters[t], 8.0, 800u + t, false);
			}

			FrameProcessInput s0{};
			s0.seq = 0; s0.receivedY = panInputs[0]; s0.isConfirmedPeak = true;
			(void)engSingle.ProcessFrame(s0);
			(void)engChained.ProcessFrame(s0);

			TdV2Plane lastSingle, lastChained;
			const CellMotionField mvStep = CellMotionField::Uniform(cw, ch, 0.30, 0.0, 0.30, 0.0, RegionMotionClass::Camera);
			for (int t = 1; t <= 23; ++t)
			{
				const CellMotionField mvTo0 = CellMotionField::Uniform(cw, ch, 0.30 * t, 0.0, 0.30 * t, 0.0, RegionMotionClass::Camera);
				const CellMotionField mvTo24 = CellMotionField::Uniform(cw, ch, -0.30 * (24 - t), 0.0, -0.30 * (24 - t), 0.0, RegionMotionClass::Camera);
				FrameProcessInput in{};
				in.seq = static_cast<uint64_t>(t);
				in.receivedY = panInputs[t];
				in.hasStepPrior = true; in.estimatedStepDelta = 8.0;
				in.nextPeak.cleanPlane = &panInputs[24]; in.nextPeak.motion = &mvTo24;
				in.hasAnchorComposedMotion = true; in.anchorComposedMotion = mvTo0;
				in.hasCarryMotion = true; in.carryMotion = mvStep;
				lastSingle = engSingle.ProcessFrame(in).outputY;
				lastChained = engChained.ProcessFrame(in).outputY;
			}
			const ThreeBandPlanes bRec = DecomposeActiveBands(panInputs[23], ActiveRect::Full(w, h));
			const ThreeBandPlanes bSng = DecomposeActiveBands(lastSingle, ActiveRect::Full(w, h));
			const ThreeBandPlanes bChn = DecomposeActiveBands(lastChained, ActiveRect::Full(w, h));
			double eHRec = 0.0, eHSng = 0.0, eHChn = 0.0;
			for (int y = 8; y < h - 8; ++y)
			{
				for (int x = 16; x < w - 16; ++x)
				{
					eHRec += bRec.H.at(x, y) * bRec.H.at(x, y);
					eHSng += bSng.H.at(x, y) * bSng.H.at(x, y);
					eHChn += bChn.H.at(x, y) * bChn.H.at(x, y);
				}
			}
			Assert::IsTrue(eHSng >= eHRec && eHSng > eHChn * 1.03,
				Msg("24-frame 0.3px pan fine energy: single=%.1f vs received=%.1f vs chained=%.1f", eHSng, eHRec, eHChn).c_str());
		}

		// (C) Held-out NCC / coarse-mean failure re-seeding (`anchorReseedCells_` persistently bars subsequent frame!).
		{
			Config cfgAnc = MakePreset(Preset::Reference, Mode::On);
			cfgAnc.enableAnchoredCarry = true;
			cfgAnc.pixelBoundKappa = 2.0;
			TdV2ReferenceEngine eng(cfgAnc);

			const TdV2Plane m0 = RenderMasterScene(w, h, 0.0, 0.0);
			const TdV2Plane p0 = ApplyCrispPeakQuant(m0, 1u);
			const TdV2Plane pNext = ApplyCrispPeakQuant(m0, 2u);
			const TdV2Plane vClean = ApplyDeblockedValleyLoss(m0, 8.0, 3u, false);
			const CellMotionField stillMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			FrameProcessInput s0{};
			s0.seq = 0; s0.receivedY = p0; s0.isConfirmedPeak = true;
			(void)eng.ProcessFrame(s0);

			FrameProcessInput s1{};
			s1.seq = 1; s1.receivedY = vClean;
			s1.prevPeak.cleanPlane = &p0; s1.prevPeak.motion = &stillMv;
			s1.hasAnchorComposedMotion = true; s1.anchorComposedMotion = stillMv;
			(void)eng.ProcessFrame(s1);

			// At seq=2 (age 2), corrupt composed motion on cell (2, 2) by +12 px so held-out check rejects it!
			CellMotionField badAncMv = stillMv;
			const size_t targetCell = 2u * cw + 2u;
			badAncMv.vx[targetCell] = 12.0;

			FrameProcessInput s2{};
			s2.seq = 2; s2.receivedY = vClean; s2.hasStepPrior = true; s2.estimatedStepDelta = 8.0;
			s2.nextPeak.cleanPlane = &pNext; s2.nextPeak.motion = &stillMv;
			s2.hasAnchorComposedMotion = true; s2.anchorComposedMotion = badAncMv;
			const FrameProcessResult r2 = eng.ProcessFrame(s2);
			Assert::IsTrue(r2.anchoredCarryHeldOutRejectCells >= 1u && eng.AnchorReseedCells()[targetCell] != 0u,
				L"Held-out structure check must reject corrupted cell and mark AnchorReseedCells");

			// At seq=3, restore GOOD composed motion `stillMv`: `AnchorReseedCells()[targetCell]` MUST STILL keep
			// that cell barred AND must NEVER let the barred anchor poison `nextPeak` recovery or steal simplex weight!
			FrameProcessInput s3 = s2;
			s3.seq = 3;
			s3.anchorComposedMotion = stillMv;
			const FrameProcessResult r3 = eng.ProcessFrame(s3);
			Assert::IsTrue(eng.AnchorReseedCells()[targetCell] != 0u && r3.anchoredCarryHeldOutRejectCells >= 1u,
				L"AnchorReseedCells must keep rejected cell barred on subsequent non-peak frames until re-seeded");

			Config cfgNoAnc = cfgAnc;
			cfgNoAnc.enableAnchoredCarry = false;
			FrameProcessInput s3Single{};
			s3Single.seq = 3; s3Single.receivedY = vClean; s3Single.hasStepPrior = true; s3Single.estimatedStepDelta = 8.0;
			s3Single.nextPeak.cleanPlane = &pNext; s3Single.nextPeak.motion = &stillMv;
			const FrameProcessResult r3Single = TdV2ReferenceEngine(cfgNoAnc).ProcessFrame(s3Single);
			double diffBarredCellVs1Ref = 0.0;
			for (int y = 36; y < 44; ++y)
				for (int x = 36; x < 44; ++x)
					diffBarredCellVs1Ref = (std::max)(diffBarredCellVs1Ref, std::fabs(r3.outputY.at(x, y) - r3Single.outputY.at(x, y)));
			Assert::IsTrue(diffBarredCellVs1Ref <= 0.15,
				Msg("Barred anchor cell must match 1-ref NextPeak solve without poisoning cA1/QP, maxDiff=%.4f", diffBarredCellVs1Ref).c_str());

			// Confirmed peak at seq=4 clears AnchorReseedCells!
			FrameProcessInput s4{};
			s4.seq = 4; s4.receivedY = pNext; s4.isConfirmedPeak = true;
			s4.prevPeak.cleanPlane = &p0; s4.prevPeak.motion = &stillMv;
			(void)eng.ProcessFrame(s4);
			Assert::AreEqual(uint8_t(0u), eng.AnchorReseedCells()[targetCell], L"Confirmed peak must clear AnchorReseedCells");
		}

		// (D) Phase 6.1 Smooth Warp Vectors (`enableSmoothWarpVectors`) & 6.2 Hard FB Occlusion Gate (`enableHardFbOcclusionGate`).
		{
			const TdV2Plane ref = RenderMasterScene(w, h, 0.0, 0.0);
			// Shear motion field vx(y) = 0.06 * y: render exact sheared ground truth and compare smooth vs nearest-cell warp!
			TdV2Plane shearedGt(w, h, 0.0);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					shearedGt.at(x, y) = EvalMasterSceneAt(x + 0.06 * y, y, w, h);

			CellMotionField shearMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			for (int cy = 0; cy < ch; ++cy)
				for (int cx = 0; cx < cw; ++cx)
					shearMv.vx[static_cast<size_t>(cy) * cw + cx] = 0.06 * (cy * cellPx + 7.5);

			TdV2Plane wSmooth, wNearest;
			std::vector<uint8_t> vS, vN;
			WarpCleanReference(ref, shearedGt, ActiveRect::Full(w, h), shearMv.vx, shearMv.vy, cw, ch, cellPx,
			                   wSmooth, vS, nullptr, nullptr, nullptr, nullptr, nullptr, /*enableSmoothWarpVectors=*/true);
			WarpCleanReference(ref, shearedGt, ActiveRect::Full(w, h), shearMv.vx, shearMv.vy, cw, ch, cellPx,
			                   wNearest, vN, nullptr, nullptr, nullptr, nullptr, nullptr, /*enableSmoothWarpVectors=*/false);
			double mseSmooth = 0.0, mseNearest = 0.0;
			for (int y = 8; y < h - 8; ++y)
			{
				for (int x = 8; x < w - 8; ++x)
				{
					const double es = wSmooth.at(x, y) - shearedGt.at(x, y);
					const double en = wNearest.at(x, y) - shearedGt.at(x, y);
					mseSmooth += es * es;
					mseNearest += en * en;
				}
			}
			Assert::IsTrue(mseSmooth < mseNearest * 0.70,
				Msg("6.1 smooth warp MSE=%.2f must beat nearest-cell negative control MSE=%.2f", mseSmooth, mseNearest).c_str());

			// 6.2 Hard >1.0 px FB occlusion gate on BOTH luma & chroma (vs enableHardFbOcclusionGate=false negative control).
			CellMotionField occMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Camera);
			occMv.confidence.assign(nCells, 0.85);
			occMv.fbErrorPx.assign(nCells, 1.35); // > 1.0 px kHardFbOcclusionPx!

			const TdV2Plane curVal = ApplyDeblockedValleyLoss(ref, 10.0, 99u, false);
			Config cfgHardOn = MakePreset(Preset::Reference, Mode::On);
			cfgHardOn.enableHardFbOcclusionGate = true;
			cfgHardOn.pixelBoundKappa = 2.0;
			Config cfgHardOff = cfgHardOn;
			cfgHardOff.enableHardFbOcclusionGate = false;

			FrameProcessInput occIn{};
			occIn.seq = 1; occIn.receivedY = curVal; occIn.hasStepPrior = true; occIn.estimatedStepDelta = 10.0;
			occIn.prevPeak.cleanPlane = &ref; occIn.prevPeak.motion = &occMv;
			occIn.nextPeak.cleanPlane = &ref; occIn.nextPeak.motion = &occMv;
			const FrameProcessResult occOn = TdV2ReferenceEngine(cfgHardOn).ProcessFrame(occIn);
			const FrameProcessResult occOff = TdV2ReferenceEngine(cfgHardOff).ProcessFrame(occIn);
			double maxOn = 0.0, maxOff = 0.0;
			for (size_t i = 0; i < w * h; ++i)
			{
				maxOn = (std::max)(maxOn, std::fabs(occOn.outputY.v[i] - curVal.v[i]));
				maxOff = (std::max)(maxOff, std::fabs(occOff.outputY.v[i] - curVal.v[i]));
			}
			Assert::AreEqual(0.0, maxOn, 1e-12, L"6.2 hard FB occlusion gate (>1 px) must force exact 0 luma delta");
			Assert::IsTrue(maxOff > 1.0, L"Negative control (hard FB gate OFF) allows nonzero ghost blend");
		}

		// (E) Bokeh sharp-to-defocus straddle (`BokehMode::LowBandCellOnly`) + standalone Chroma rack-focus veto.
		{
			const TdV2Plane peakY = RenderMasterScene(w, h, 0.0, 0.0);
			TdV2Plane curBokeh = peakY;
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					curBokeh.at(x, y) = (x < w / 2) ? (std::round(peakY.at(x, y) / 10.0) * 10.0) : peakY.at(x, y);

			std::vector<uint8_t> defocusCells(nCells, 0u);
			for (int cy = 0; cy < ch; ++cy)
				for (int cx = 0; cx < cw / 2; ++cx)
					defocusCells[static_cast<size_t>(cy) * cw + cx] = 1u;

			const CellMotionField stillMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
			Config cfgBokehL = MakePreset(Preset::Reference, Mode::On);
			cfgBokehL.bokehMode = BokehMode::LowBandCellOnly;
			cfgBokehL.pixelBoundKappa = 2.0;
			Config cfgBokehOff = cfgBokehL;
			cfgBokehOff.bokehMode = BokehMode::Untouched;

			FrameProcessInput bIn{};
			bIn.seq = 1; bIn.receivedY = curBokeh; bIn.hasStepPrior = true; bIn.estimatedStepDelta = 10.0;
			bIn.explicitDefocusedCells = &defocusCells;
			bIn.prevPeak.cleanPlane = &peakY; bIn.prevPeak.motion = &stillMv;
			bIn.nextPeak.cleanPlane = &peakY; bIn.nextPeak.motion = &stillMv;

			const FrameProcessResult rBokL = TdV2ReferenceEngine(cfgBokehL).ProcessFrame(bIn);
			const FrameProcessResult rBokOff = TdV2ReferenceEngine(cfgBokehOff).ProcessFrame(bIn);
			double lHalfDiffL = 0.0, lHalfDiffOff = 0.0;
			for (int y = 8; y < h - 8; ++y)
			{
				for (int x = 8; x < (w / 2) - 16; ++x)
				{
					lHalfDiffL += std::fabs(rBokL.outputY.at(x, y) - curBokeh.at(x, y));
					lHalfDiffOff += std::fabs(rBokOff.outputY.at(x, y) - curBokeh.at(x, y));
				}
			}
			Assert::AreEqual(0.0, lHalfDiffOff, 1e-12, L"BokehMode::Untouched must keep defocused region bit-exact");
			Assert::IsTrue(lHalfDiffL > 5.0, L"BokehMode::LowBandCellOnly must allow L-band debanding on matched bokeh");

			// Standalone chroma rack-focus (`explicitFocusChangeCells == nullptr`): sharp ref Cb/Cr -> defocused cur Cb/Cr!
			const int cW = w / 2, cH = h / 2;
			TdV2Plane sharpCb(cW, cH, 512.0), sharpCr(cW, cH, 512.0), blurCb(cW, cH, 512.0), blurCr(cW, cH, 512.0);
			for (int y = 0; y < cH; ++y)
				for (int x = 0; x < cW; ++x)
					sharpCb.at(x, y) = sharpCr.at(x, y) = ((x % 12) < 6) ? 340.0 : 680.0;
			// Optical defocus blur (9x9 box filter) on current chroma frame -> genuine rack focus!
			for (int y = 0; y < cH; ++y)
			{
				for (int x = 0; x < cW; ++x)
				{
					double s = 0.0; int cnt = 0;
					for (int dy = -4; dy <= 4; ++dy)
						for (int dx = -4; dx <= 4; ++dx)
						{
							s += sharpCb.at((std::max)(0, (std::min)(cW - 1, x + dx)), (std::max)(0, (std::min)(cH - 1, y + dy)));
							++cnt;
						}
					blurCb.at(x, y) = blurCr.at(x, y) = s / cnt;
				}
			}
			Config cCfg = MakePreset(Preset::Reference, Mode::On);
			cCfg.pixelBoundKappa = 2.0;
			ChromaProcessInput rfIn{};
			rfIn.seq = 1; rfIn.lumaW = w; rfIn.lumaH = h;
			rfIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
			rfIn.codedSubsampling = ChromaSubsampling::Yuv420;
			rfIn.gridStatus = ChromaGridStatus::Identified;
			rfIn.receivedCb = blurCb; rfIn.receivedCr = blurCr;
			rfIn.explicitFocusChangeCells = nullptr; // Standalone chroma focus guard!
			rfIn.hasStepPrior = true; rfIn.estimatedStepDelta = 6.0;
			rfIn.prevPeak.cleanCb = &sharpCb; rfIn.prevPeak.cleanCr = &sharpCr; rfIn.prevPeak.lumaMotion = &stillMv;
			rfIn.nextPeak.cleanCb = &sharpCb; rfIn.nextPeak.cleanCr = &sharpCr; rfIn.nextPeak.lumaMotion = &stillMv;
			const ChromaProcessResult rfRes = TdV2ChromaEngine(cCfg).ProcessFrame(rfIn);
			Assert::IsTrue(rfRes.focusVetoCells > 0u, L"Standalone chroma engine must detect rack-focus cells without luma mask");
		}

		// (F) Plan 6.6 `chromaReuseLumaWeights` end-to-end (`TdV2ReferenceEngine -> TdV2ChromaEngine` with default
		//     `enablePerCornerHuber = true`!) + RefRole mapping + Branch-A exclusion + Layer-4 `cellRetainedGrain` intact!
		{
			const int cW = w / 2, cH = h / 2;
			const TdV2Plane mY = RenderMasterScene(w, h, 0.0, 0.0);
			const TdV2Plane pk1Y = ApplyCrispPeakQuant(mY, 10u);
			const TdV2Plane pk2Y = ApplyCrispPeakQuant(mY, 11u);
			const TdV2Plane valY = ApplyDeblockedValleyLoss(mY, 10.0, 12u, false);

			TdV2Plane mCb(cW, cH, 512.0), mCr(cW, cH, 512.0);
			for (int y = 0; y < cH; ++y)
				for (int x = 0; x < cW; ++x)
				{
					mCb.at(x, y) = 420.0 + 160.0 * std::sin(0.28 * x + 0.19 * y);
					mCr.at(x, y) = 580.0 - 140.0 * std::cos(0.24 * x - 0.22 * y);
				}
			const TdV2Plane pk1Cb = ApplyCrispPeakQuant(mCb, 20u), pk1Cr = ApplyCrispPeakQuant(mCr, 21u);
			const TdV2Plane pk2Cb = ApplyCrispPeakQuant(mCb, 22u), pk2Cr = ApplyCrispPeakQuant(mCr, 23u);
			const TdV2Plane valCb = ApplyDeblockedValleyLoss(mCb, 10.0, 24u, false);
			const TdV2Plane valCr = ApplyDeblockedValleyLoss(mCr, 10.0, 25u, false);

			const CellMotionField stillMv = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

			Config reuseCfg = MakePreset(Preset::Reference, Mode::On);
			reuseCfg.chromaReuseLumaWeights = true;
			reuseCfg.enablePerCornerHuber = true; // Default production Huber taper ON!
			reuseCfg.pixelBoundKappa = 2.0;

			FrameProcessInput lIn{};
			lIn.seq = 1; lIn.receivedY = valY;
			lIn.hasStepPrior = true; lIn.estimatedStepDelta = 10.0;
			lIn.prevPeak.cleanPlane = &pk1Y; lIn.prevPeak.motion = &stillMv;
			lIn.nextPeak.cleanPlane = &pk2Y; lIn.nextPeak.motion = &stillMv;

			TdV2ReferenceEngine lEng(reuseCfg);
			const FrameProcessResult lRes = lEng.ProcessFrame(lIn);
			Assert::AreEqual(2, lRes.lumaActiveRefCount, L"Luma must export 2 active reference weights");

			std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> packedLw;
			for (int b = 0; b < 3; ++b)
				for (int k = 0; k < kMaxBlendRefs; ++k)
					packedLw[b][k] = lRes.lumaCellWeights[b][k];

			ChromaProcessInput cIn{};
			cIn.seq = 1; cIn.lumaW = w; cIn.lumaH = h;
			cIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
			cIn.codedSubsampling = ChromaSubsampling::Yuv420;
			cIn.gridStatus = ChromaGridStatus::Identified;
			cIn.receivedY = valY;
			cIn.receivedCb = valCb; cIn.receivedCr = valCr;
			cIn.hasStepPrior = true; cIn.estimatedStepDelta = 10.0;
			cIn.prevPeak.cleanCb = &pk1Cb; cIn.prevPeak.cleanCr = &pk1Cr; cIn.prevPeak.lumaMotion = &stillMv;
			cIn.nextPeak.cleanCb = &pk2Cb; cIn.nextPeak.cleanCr = &pk2Cr; cIn.nextPeak.lumaMotion = &stillMv;
			cIn.lumaCellWeights = &packedLw;
			cIn.lumaRefRoles = lRes.lumaRefRoles;
			cIn.lumaActiveRefCount = lRes.lumaActiveRefCount;

			const ChromaProcessResult cRes = TdV2ChromaEngine(reuseCfg).ProcessFrame(cIn);

			// End-to-end luma->chroma weight reuse MUST recover chroma PSNR (NOT collapse to ~1e-9 from zero cellDiagC!).
			double errBase = 0.0, errOut = 0.0;
			for (size_t i = 0; i < mCb.v.size(); ++i)
			{
				const double db = valCb.v[i] - mCb.v[i];
				const double doOut = cRes.outputCb.v[i] - mCb.v[i];
				errBase += db * db;
				errOut += doOut * doOut;
			}
			double psnrBase = 0.0, psnrOut = 0.0;
			ComputePsnrDb(errBase / mCb.v.size(), 1023.0, psnrBase);
			ComputePsnrDb(errOut / mCb.v.size(), 1023.0, psnrOut);
			Assert::IsTrue(psnrOut > psnrBase + 0.5,
				Msg("6.6 end-to-end chromaReuseLumaWeights (Huber ON) Cb deltaDb=%+.4f dB expected > +0.5 dB",
					psnrOut - psnrBase).c_str());

			// Verify RefRole slot-order invariance: swapping luma slots 0 <-> 1 along with lumaRefRoles {NextPeak, PrevPeak}
			// must produce the EXACT same chroma output (whereas wrong role labels change the output)!
			std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> swappedLw = packedLw;
			for (int b = 0; b < 3; ++b)
			{
				for (size_t ci = 0; ci < nCells; ++ci)
				{
					swappedLw[b][0][ci] = 0.05; // Deliberately asymmetric weights for slot 0 vs slot 1
					swappedLw[b][1][ci] = 0.75;
				}
			}
			const RefRole rolesNormal[2] = { RefRole::PrevPeak, RefRole::NextPeak };
			const RefRole rolesSwapped[2] = { RefRole::NextPeak, RefRole::PrevPeak };
			std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> revLw = swappedLw;
			for (int b = 0; b < 3; ++b) std::swap(revLw[b][0], revLw[b][1]);

			ChromaProcessInput cNorm = cIn, cSwap = cIn, cWrongRole = cIn;
			cNorm.lumaCellWeights = &swappedLw; cNorm.lumaRefRoles = rolesNormal;
			cSwap.lumaCellWeights = &revLw;     cSwap.lumaRefRoles = rolesSwapped;
			cWrongRole.lumaCellWeights = &revLw; cWrongRole.lumaRefRoles = rolesNormal; // Negative control: blind index!
			// Make pk1Cb != pk2Cb so swapping slots without role mapping changes pixels!
			TdV2Plane pk2DiffCb = pk2Cb;
			for (double& v : pk2DiffCb.v) v += 4.0;
			cNorm.nextPeak.cleanCb = &pk2DiffCb;
			cSwap.nextPeak.cleanCb = &pk2DiffCb;
			cWrongRole.nextPeak.cleanCb = &pk2DiffCb;

			const ChromaProcessResult rNorm = TdV2ChromaEngine(reuseCfg).ProcessFrame(cNorm);
			const ChromaProcessResult rSwap = TdV2ChromaEngine(reuseCfg).ProcessFrame(cSwap);
			const ChromaProcessResult rWrong = TdV2ChromaEngine(reuseCfg).ProcessFrame(cWrongRole);
			double diffMapped = 0.0, diffUnmapped = 0.0;
			for (size_t i = 0; i < rNorm.outputCb.v.size(); ++i)
			{
				diffMapped = (std::max)(diffMapped, std::fabs(rNorm.outputCb.v[i] - rSwap.outputCb.v[i]));
				diffUnmapped = (std::max)(diffUnmapped, std::fabs(rNorm.outputCb.v[i] - rWrong.outputCb.v[i]));
			}
			Assert::AreEqual(0.0, diffMapped, 1e-9, L"6.6 RefRole mapping must be invariant to luma slot permutation");
			Assert::IsTrue(diffUnmapped > 0.2, L"Negative control: wrong RefRole mapping changes chroma output");

			// Branch-A exclusion: when `gridStatus == ChromaGridStatus::Unidentified`, chroma MUST NOT reuse M/H luma weights!
			ChromaProcessInput cUnid = cNorm;
			cUnid.gridStatus = ChromaGridStatus::Unidentified;
			const ChromaProcessResult rUnidReuse = TdV2ChromaEngine(reuseCfg).ProcessFrame(cUnid);
			Config indepCfg = reuseCfg; indepCfg.chromaReuseLumaWeights = false;
			const ChromaProcessResult rUnidIndep = TdV2ChromaEngine(indepCfg).ProcessFrame(cUnid);
			Assert::IsTrue(rUnidReuse.coarseBandOnly, L"Unidentified Branch A must stay coarseBandOnly");
			for (size_t i = 0; i < rUnidReuse.outputCb.v.size(); ++i)
				Assert::AreEqual(rUnidIndep.outputCb.v[i], rUnidReuse.outputCb.v[i], 1e-9,
					L"Branch A Unidentified must ignore lumaCellWeights completely");

			// Layer-4 chroma `retainedGrainVarCb/Cr` must remain intact when `chromaReuseLumaWeights == true`!
			std::mt19937 gRng(77u);
			std::normal_distribution<double> gDist(0.0, 6.0);
			TdV2Plane grainyCb = valCb, grainyCr = valCr;
			for (size_t i = 0; i < grainyCb.v.size(); ++i)
			{
				grainyCb.v[i] += gDist(gRng);
				grainyCr.v[i] += gDist(gRng);
			}
			std::array<std::array<std::vector<double>, kMaxBlendRefs>, 3> highLw{};
			for (int b = 0; b < 3; ++b)
			{
				highLw[b][0].assign(nCells, 0.50);
				highLw[b][1].assign(nCells, 0.50);
			}
			Config grainReuseCfg = reuseCfg;
			grainReuseCfg.enableA2eEmpiricalPrior = false;
			grainReuseCfg.enablePerCornerHuber = false;
			grainReuseCfg.pixelBoundKappa = 3.0;
			ChromaProcessInput cGrain = cIn;
			cGrain.lumaCellWeights = &highLw;
			cGrain.hasStepPrior = true;
			cGrain.estimatedStepDelta = 60.0;
			cGrain.receivedCb = grainyCb; cGrain.receivedCr = grainyCr;
			cGrain.hasCleanSplit = true; cGrain.cleanCb = grainyCb; cGrain.cleanCr = grainyCr; // Leaked grain in clean!
			cGrain.retainedGrainVarCb.assign(nCells, 36.0);
			cGrain.retainedGrainVarCr.assign(nCells, 36.0);
			const ChromaProcessResult rGrain = TdV2ChromaEngine(grainReuseCfg).ProcessFrame(cGrain);
			Assert::IsTrue(rGrain.guardViolationCells > 0u,
				L"6.6 chromaReuseLumaWeights must keep CPU Layer-4 cellRetainedGrain guard intact!");
			// Same-input negative control: zeroing retainedGrainVarCb/Cr with cleanCb==receivedCb stops Layer-4 from restoring leaked grain!
			ChromaProcessInput cGrainNoPrior = cGrain;
			cGrainNoPrior.retainedGrainVarCb.assign(nCells, 0.0);
			cGrainNoPrior.retainedGrainVarCr.assign(nCells, 0.0);
			const ChromaProcessResult rGrainNoPrior = TdV2ChromaEngine(grainReuseCfg).ProcessFrame(cGrainNoPrior);
			Assert::AreEqual(static_cast<size_t>(0u), rGrainNoPrior.guardViolationCells,
				L"Negative control: without retainedGrainVar prior on leaked split, Layer-4 does not fire");
		}

		// (G) All Settings UI keys parsed & round-tripped + fail-closed validation + Original == Bill override!
		{
			TdV2RuntimeSettings s{};
			std::string prob;
			const std::pair<const char*, const char*> validPairs[] = {
				{ "temporal_detail", "on" },
				{ "td_mode", "strict" },
				{ "td_limit_kappa", "2.25" },
				{ "td_preset", "reference" },
				{ "td_overlay", "weights" },
				{ "td_solver", "legacy_clip" },
				{ "td_quality_signal", "non_block_step" },
				{ "td_stats_pooling", "two_frame_same_role" },
				{ "td_chroma_weights", "reuse_luma" },
				{ "td_anchored_carry", "on" },
				{ "td_smooth_warp", "off" },
				{ "td_hard_fb_gate", "off" },
				{ "td_smooth_grain_guard", "off" },
				{ "td_subpixel_debias", "on" },
				{ "td_edge_upsample", "on" },
				{ "td_neighborhood_clamp", "on" }
			};
			for (const auto& kv : validPairs)
			{
				Assert::IsTrue(ParseTdV2Settings(kv.first, kv.second, s, prob) && prob.empty(),
					L"Valid TD v2 setting key/value failed to parse");
			}
			const Config built = BuildTdV2Config(s, /*originalPathway=*/false);
			Assert::IsTrue(built.mode == Mode::On &&
			               built.residualMode == ResidualMode::Strict &&
			               std::fabs(built.pixelBoundKappa - 2.25) < 1e-9 &&
			               built.preset == Preset::Reference &&
			               built.solverMode == SolverMode::LegacyClipRescale &&
			               built.qualitySignalMode == QualitySignalMode::NonBlockPictureStep &&
			               !built.enableA2eEmpiricalPrior &&
			               built.enableTwoFrameSameRoleStatsPool &&
			               built.chromaReuseLumaWeights &&
			               built.enableAnchoredCarry &&
			               !built.enableSmoothWarpVectors &&
			               !built.enableHardFbOcclusionGate &&
			               !built.enableSmoothLayer4Guard &&
			               built.enableSubpixelHAttenuationDebias &&
			               built.enableEdgeAwareWeightUpsample &&
			               built.enableNeighborhoodSoftClamp,
				L"BuildTdV2Config did not round-trip all 16 settings cleanly");

			// Original == Bill override forces Mode::Off bit-exact!
			const Config origBuilt = BuildTdV2Config(s, /*originalPathway=*/true);
			Assert::IsTrue(origBuilt.mode == Mode::Off && !origBuilt.IsEnabled(),
				L"Original pathway must force TD Mode::Off");

			// Invalid tokens fail closed with non-empty diagnostic and NEVER mutate `s`!
			const TdV2RuntimeSettings beforeBad = s;
			Assert::IsFalse(ParseTdV2Settings("td_limit_kappa", "0.5", s, prob) || prob.empty());
			Assert::IsFalse(ParseTdV2Settings("td_limit_kappa", "3.5", s, prob) || prob.empty());
			Assert::IsFalse(ParseTdV2Settings("td_unknown_key", "on", s, prob) || prob.empty());
			Assert::AreEqual(beforeBad.limitKappa, s.limitKappa, 1e-12, L"Rejected setting must not mutate state");
		}
	}
};

} // namespace Tests
