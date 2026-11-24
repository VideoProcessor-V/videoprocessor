// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

// U7 unit version 4 (576-cell spatial affine fade law; version 3 is 102b U7-MULTIFRAME-FADE-LAW; version 2 is U7-ACCURACY's cut law): the multi-frame fade law (CutFadeJudge::FadeRun / FoldFadeRun). Real P4 records
// (SourceStats::ComputeRecord) of a synthetic half plane: a horizontal ramp with a deterministic per-frame noise, scaled about black
// (4096, 8-bit 16) by the fade gain. The pair law (ClassifyEvents) is checked alongside. CPU only.

#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CutFadeJudgeFadeRunTestsNs
{
	using Record = VpUnit::SourceStats::Record;
	using Events = VpUnit::CutFadeJudge::EventSignals;
	using VpUnit::CutFadeJudge::FadeRun;

	constexpr uint32_t kW = 160, kH = 90;
	constexpr double kBlack = 4096.0;

	inline int Noise(uint32_t x, uint32_t y, uint32_t k)
	{
		uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (k * 83492791u);
		h ^= h >> 13;
		h *= 0x5bd1e995u;
		h ^= h >> 15;
		return static_cast<int>(h % 513u) - 256;
	}

	// gain about black; shift > 0: the ramp is twice the plane wide and moves left by `shift` px (a pan, no fade).
	inline Record Picture(double gain, uint32_t k, uint32_t shift = 0, bool pan = false)
	{
		std::vector<uint16_t> half(size_t(kW) * kH);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kW; ++x)
			{
				const int64_t base = 8192 + (int64_t(x + shift) * 48000) / (pan ? int64_t(2 * kW) : int64_t(kW - 1));
				const double v = kBlack + gain * (double(base) - kBlack) + double(Noise(x, y, k));
				half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
			}
		std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
		Record r;
		Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()), L"the record");
		Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r), L"the record reads back");
		return r;
	}

	inline Record Flat(uint32_t k)
	{
		std::vector<uint16_t> half(size_t(kW) * kH);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kW; ++x)
				half[size_t(y) * kW + x] = static_cast<uint16_t>(6000 + Noise(x, y, k));
		std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
		Record r;
		Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()));
		Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r));
		return r;
	}

	inline Record Black(uint32_t k, bool noisy = false)
	{
		std::vector<uint16_t> half(size_t(kW) * kH);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kW; ++x)
			{
				const double v = kBlack + (noisy ? double(Noise(x, y, k)) / 4.0 : 0.0);
				half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
			}
		std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
		Record r;
		Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()));
		Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r));
		return r;
	}

	// `pre` static frames, a fade over `n` frames from gain 1 to `g1`, then `post` static frames at g1.
	inline std::vector<Record> FadeSequence(int n, int pre, int post, double g1)
	{
		std::vector<Record> r;
		uint32_t k = 0;
		for (int i = 0; i < pre; ++i)
			r.push_back(Picture(1.0, k++));
		for (int i = 1; i <= n; ++i)
			r.push_back(Picture(1.0 - (1.0 - g1) * i / n, k++));
		for (int i = 0; i < post; ++i)
			r.push_back(Picture(g1, k++));
		return r;
	}

	struct Pair
	{
		bool pairFade = false, runFade = false, cut = false, flash = false;
		bool Fade() const { return pairFade || runFade; }
	};

	// Every pair in order through one run, as the skeleton's seal does.
	inline std::vector<Pair> Play(const std::vector<Record>& recs)
	{
		std::vector<Pair> out(recs.size());
		FadeRun run;
		for (size_t k = 1; k < recs.size(); ++k)
		{
			const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[k - 1], recs[k], false, false);
			out[k].pairFade = e.fade;
			out[k].cut = e.cut;
			out[k].flash = e.flash;
			out[k].runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[k - 1], recs[k], e, run);
		}
		return out;
	}

	inline int Count(const std::vector<Pair>& p, size_t from, size_t to, bool pairLawOnly = false)
	{
		int n = 0;
		for (size_t k = from; k < to && k < p.size(); ++k)
			n += (pairLawOnly ? p[k].pairFade : p[k].Fade()) ? 1 : 0;
		return n;
	}

	// Self-contained frozen W1 v4 (pre-D10, commit e7d39d29e) unoptimized alignment and affine fade-fit reference.
	// Used strictly by D10 decision-identity and before/after microbenchmark tests (never in production).
	namespace W1RefV4
	{
		using VpUnit::CutFadeJudge::GridFadeFit;
		using VpUnit::CutFadeJudge::kFadeCellStepEps;
		using VpUnit::CutFadeJudge::kFadeMaxBlack;
		using VpUnit::CutFadeJudge::kFadeMinBlack;
		using VpUnit::CutFadeJudge::kFadeMinSpread;
		using VpUnit::CutFadeJudge::kFadeNoBlack;
		using VpUnit::CutFadeJudge::kFadeResidualReduction;
		using VpUnit::CutFadeJudge::kFadeRunMinGain;
		using VpUnit::CutFadeJudge::kFadeSpatialConsensus;
		using VpUnit::CutFadeJudge::kFadeSpatialCoverage;
		using VpUnit::CutFadeJudge::kGridH;
		using VpUnit::CutFadeJudge::kGridW;
		using VpUnit::CutFadeJudge::kLocalRadius;

		inline double BilinearGrid(const double g[kGridH][kGridW], double y, double x)
		{
			const double cy = (std::min)(double(kGridH - 1), (std::max)(0.0, y));
			const double cx = (std::min)(double(kGridW - 1), (std::max)(0.0, x));
			const int y0 = int(std::floor(cy));
			const int x0 = int(std::floor(cx));
			const int y1 = (std::min)(y0 + 1, kGridH - 1);
			const int x1 = (std::min)(x0 + 1, kGridW - 1);
			const double fy = cy - double(y0);
			const double fx = cx - double(x0);
			const double top = g[y0][x0] * (1.0 - fx) + g[y0][x1] * fx;
			const double bot = g[y1][x0] * (1.0 - fx) + g[y1][x1] * fx;
			return top * (1.0 - fy) + bot * fy;
		}

		inline void EvalInteriorAffine(const double aligned[kGridH][kGridW], const double cur[kGridH][kGridW],
		                               double* outA, double* outC, double* outSxx, double* outCorr, double* outEFade)
		{
			constexpr int R = kLocalRadius;
			constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));
			double sx = 0.0, sy = 0.0;
			for (int y = R; y < kGridH - R; ++y)
				for (int x = R; x < kGridW - R; ++x)
				{
					sx += aligned[y][x];
					sy += cur[y][x];
				}
			const double mx = sx / kN;
			const double my = sy / kN;
			double sxx = 0.0, syy = 0.0, sxy = 0.0;
			for (int y = R; y < kGridH - R; ++y)
				for (int x = R; x < kGridW - R; ++x)
				{
					const double dx = aligned[y][x] - mx;
					const double dy = cur[y][x] - my;
					sxx += dx * dx;
					syy += dy * dy;
					sxy += dx * dy;
				}
			const double a = sxx > 1e-6 ? sxy / sxx : 0.0;
			const double c = my - a * mx;
			const double den = std::sqrt((sxx > 0.0 ? sxx : 0.0) * (syy > 0.0 ? syy : 0.0));
			const double corr = den > 1e-12 ? sxy / den : 0.0;
			double ef = 0.0;
			for (int y = R; y < kGridH - R; ++y)
				for (int x = R; x < kGridW - R; ++x)
					ef += std::fabs(cur[y][x] - (a * aligned[y][x] + c));
			*outA = a;
			*outC = c;
			*outSxx = sxx;
			*outCorr = corr;
			*outEFade = ef / kN;
		}

		inline void SampleInteriorPairShifted(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW],
		                                      int iy, int ix, double fy, double fx,
		                                      double refS[kGridH][kGridW], double curS[kGridH][kGridW])
		{
			constexpr int R = kLocalRadius;
			const double hfy = 0.5 * fy;
			const double hfx = 0.5 * fx;
			for (int y = R; y < kGridH - R; ++y)
				for (int x = R; x < kGridW - R; ++x)
				{
					refS[y][x] = BilinearGrid(ref, double(y + iy) + hfy, double(x + ix) + hfx);
					curS[y][x] = BilinearGrid(cur, double(y) - hfy, double(x) - hfx);
				}
		}

		inline void ReferenceAlignGrid(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW], int searchRadius,
		                               double refAligned[kGridH][kGridW], double curAligned[kGridH][kGridW],
		                               double* outVx, double* outVy)
		{
			constexpr int R = kLocalRadius;
			constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));
			const int rad = (std::min)(searchRadius, R);

			double candRef[kGridH][kGridW], candCur[kGridH][kGridW];
			SampleInteriorPairShifted(ref, cur, 0, 0, 0.0, 0.0, refAligned, curAligned);
			double a0 = 0.0, c0 = 0.0, sxx0 = 0.0, corr0 = 0.0, eFade0 = 0.0;
			EvalInteriorAffine(refAligned, curAligned, &a0, &c0, &sxx0, &corr0, &eFade0);

			int bestSy = 0, bestSx = 0;
			double bestCorr = corr0;
			double bestEFade = eFade0;

			if (sxx0 > 1e-6)
			{
				for (int sy = -rad; sy <= rad; ++sy)
				{
					for (int sx = -rad; sx <= rad; ++sx)
					{
						if (sy == 0 && sx == 0)
							continue;
						SampleInteriorPairShifted(ref, cur, sy, sx, 0.0, 0.0, candRef, candCur);
						double a = 0.0, c = 0.0, sxx = 0.0, corr = 0.0, eFade = 0.0;
						EvalInteriorAffine(candRef, candCur, &a, &c, &sxx, &corr, &eFade);
						if (sxx <= 1e-6)
							continue;
						if (corr > bestCorr &&
							(eFade < eFade0 - kFadeCellStepEps || eFade < kFadeResidualReduction * eFade0 ||
							 (1.0 - corr) < kFadeResidualReduction * (1.0 - corr0)))
						{
							bestCorr = corr;
							bestSy = sy;
							bestSx = sx;
							bestEFade = eFade;
						}
					}
				}
			}

			double fx = 0.0;
			double fy = 0.0;
			SampleInteriorPairShifted(ref, cur, bestSy, bestSx, fy, fx, refAligned, curAligned);
			double curEFade = bestEFade;
			double curCorr = bestCorr;

			constexpr int kAlignMaxIters = 4;
			for (int iter = 0; iter < kAlignMaxIters; ++iter)
			{
				double a = 0.0, c = 0.0, sxx = 0.0, corr = 0.0, ef = 0.0;
				EvalInteriorAffine(refAligned, curAligned, &a, &c, &sxx, &corr, &ef);
				if (sxx <= 1e-6 || !(a > 1e-6))
					break;

				const double hfy = 0.5 * fy;
				const double hfx = 0.5 * fx;
				double jxArr[kGridH][kGridW], jyArr[kGridH][kGridW];
				double sumJx = 0.0, sumJy = 0.0;
				for (int y = R; y < kGridH - R; ++y)
				{
					for (int x = R; x < kGridW - R; ++x)
					{
						const double rgx = BilinearGrid(ref, double(y + bestSy) + hfy, double(x + bestSx) + hfx + 0.5) -
							BilinearGrid(ref, double(y + bestSy) + hfy, double(x + bestSx) + hfx - 0.5);
						const double rgy = BilinearGrid(ref, double(y + bestSy) + hfy + 0.5, double(x + bestSx) + hfx) -
							BilinearGrid(ref, double(y + bestSy) + hfy - 0.5, double(x + bestSx) + hfx);
						const double cgx = BilinearGrid(cur, double(y) - hfy, double(x) - hfx + 0.5) -
							BilinearGrid(cur, double(y) - hfy, double(x) - hfx - 0.5);
						const double cgy = BilinearGrid(cur, double(y) - hfy + 0.5, double(x) - hfx) -
							BilinearGrid(cur, double(y) - hfy - 0.5, double(x) - hfx);
						const double jx = 0.5 * (a * rgx + cgx);
						const double jy = 0.5 * (a * rgy + cgy);
						jxArr[y][x] = jx;
						jyArr[y][x] = jy;
						sumJx += jx;
						sumJy += jy;
					}
				}
				const double meanJx = sumJx / kN;
				const double meanJy = sumJy / kN;

				double h11 = 0.0, h12 = 0.0, h22 = 0.0, b1 = 0.0, b2 = 0.0;
				for (int y = R; y < kGridH - R; ++y)
				{
					for (int x = R; x < kGridW - R; ++x)
					{
						const double jx = jxArr[y][x] - meanJx;
						const double jy = jyArr[y][x] - meanJy;
						const double res = curAligned[y][x] - (a * refAligned[y][x] + c);
						h11 += jx * jx;
						h12 += jx * jy;
						h22 += jy * jy;
						b1 += jx * res;
						b2 += jy * res;
					}
				}

				double du = 0.0, dv = 0.0;
				const double trace = h11 + h22;
				const double det = h11 * h22 - h12 * h12;
				if (det > 1e-6 * trace * trace && trace > 1e-6)
				{
					du = (h22 * b1 - h12 * b2) / det;
					dv = (h11 * b2 - h12 * b1) / det;
				}
				else if (h11 > 1e-6 && h11 >= h22)
				{
					du = b1 / h11;
				}
				else if (h22 > 1e-6)
				{
					dv = b2 / h22;
				}
				else
				{
					break;
				}

				const double minFx = (std::max)(-double(R) - double(bestSx), -0.5);
				const double maxFx = (std::min)(double(R) - double(bestSx), 0.5);
				const double minFy = (std::max)(-double(R) - double(bestSy), -0.5);
				const double maxFy = (std::min)(double(R) - double(bestSy), 0.5);
				const double nfx = (std::min)(maxFx, (std::max)(minFx, fx + du));
				const double nfy = (std::min)(maxFy, (std::max)(minFy, fy + dv));
				if (std::fabs(nfx - fx) < 1e-4 && std::fabs(nfy - fy) < 1e-4)
					break;

				SampleInteriorPairShifted(ref, cur, bestSy, bestSx, nfy, nfx, candRef, candCur);
				double na = 0.0, nc = 0.0, nsxx = 0.0, ncorr = 0.0, newEFade = 0.0;
				EvalInteriorAffine(candRef, candCur, &na, &nc, &nsxx, &ncorr, &newEFade);

				const bool improved = (curEFade > kFadeCellStepEps)
					? (newEFade < curEFade)
					: (newEFade < kFadeResidualReduction * curEFade ||
					   (newEFade < curEFade && (1.0 - ncorr) < kFadeResidualReduction * (1.0 - curCorr)));
				if (!improved)
					break;

				fx = nfx;
				fy = nfy;
				curEFade = newEFade;
				curCorr = ncorr;
				for (int y = R; y < kGridH - R; ++y)
					for (int x = R; x < kGridW - R; ++x)
					{
						refAligned[y][x] = candRef[y][x];
						curAligned[y][x] = candCur[y][x];
					}
			}

			if (outVx)
				*outVx = double(bestSx) + fx;
			if (outVy)
				*outVy = double(bestSy) + fy;
		}

		inline GridFadeFit ReferenceFitGridFadeGrid(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW], int searchRadius)
		{
			constexpr int R = kLocalRadius;
			constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));
			GridFadeFit fit;

			double refAligned[kGridH][kGridW], curAligned[kGridH][kGridW];
			ReferenceAlignGrid(ref, cur, searchRadius, refAligned, curAligned, &fit.vx, &fit.vy);

			double sx = 0.0, sy = 0.0;
			for (int y = R; y < kGridH - R; ++y)
				for (int x = R; x < kGridW - R; ++x)
				{
					sx += refAligned[y][x];
					sy += curAligned[y][x];
				}
			const double mx = sx / kN;
			const double my = sy / kN;
			double sxx = 0.0, sxy = 0.0;
			for (int y = R; y < kGridH - R; ++y)
				for (int x = R; x < kGridW - R; ++x)
				{
					const double dx = refAligned[y][x] - mx;
					sxx += dx * dx;
					sxy += dx * (curAligned[y][x] - my);
				}
			if (sxx <= 1e-6)
				return fit;

			const double A = sxy / sxx;
			const double c = my - A * mx;
			fit.gain = A;
			if (!(A > 0.0) || std::fabs(1.0 - A) <= kFadeRunMinGain)
				return fit;

			const double b = c / (1.0 - A);
			fit.black = b;
			if (!(b > double(kFadeMinBlack) && b < double(kFadeMaxBlack)) || !(mx > b && my > b))
				return fit;

			double minCell = double(kFadeNoBlack);
			for (int y = R; y < kGridH - R; ++y)
				for (int x = R; x < kGridW - R; ++x)
				{
					const double m = (std::min)(refAligned[y][x], curAligned[y][x]);
					if (m < minCell)
						minCell = m;
				}
			const double litFloor = (std::max)(b, minCell);
			const int dir = A > 1.0 ? 1 : -1;
			fit.dir = dir;
			double sumIdSq = 0.0, sumFadeSq = 0.0;
			int litCells = 0, sameSign = 0, oppSign = 0;
			for (int y = R; y < kGridH - R; ++y)
				for (int x = R; x < kGridW - R; ++x)
				{
					if ((std::max)(refAligned[y][x], curAligned[y][x]) > litFloor + double(kFadeMinSpread))
					{
						++litCells;
						const double delta = curAligned[y][x] - refAligned[y][x];
						const double res = curAligned[y][x] - (A * refAligned[y][x] + c);
						sumIdSq += delta * delta;
						sumFadeSq += res * res;
						if (double(dir) * delta > kFadeCellStepEps)
							++sameSign;
						else if (-double(dir) * delta > kFadeCellStepEps)
							++oppSign;
					}
				}
			fit.litCells = litCells;
			fit.sameSign = sameSign;
			fit.oppSign = oppSign;
			if (litCells <= 0)
				return fit;

			const double vId = sumIdSq / double(litCells);
			const double vFade = sumFadeSq / double(litCells);
			fit.eId = std::sqrt(vId);
			fit.eFade = std::sqrt(vFade);
			if (!(fit.eId > kFadeCellStepEps && vFade < kFadeResidualReduction * vId))
				return fit;
			if (double(sameSign) < kFadeSpatialConsensus * double(sameSign + oppSign))
				return fit;
			if (double(sameSign) < kFadeSpatialCoverage * double(litCells))
				return fit;

			fit.valid = true;
			return fit;
		}
	}

	inline Record MakeRecordFromHalf(const std::vector<uint16_t>& half)
	{
		std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
		Record r;
		Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()));
		Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r));
		return r;
	}

	// Non-repeating 2D textured synthetic picture scaled about kBlack and shifted by (shiftCellsX, shiftCellsY) grid cells
	// (1 cell = 5.0 px on 160x90 -> 32x18 grid).
	inline Record TexturedPanFadePicture(double gain, double shiftCellsX, double shiftCellsY, uint32_t k)
	{
		const double pxPerCellX = double(kW) / double(VpUnit::CutFadeJudge::kGridW);
		const double pxPerCellY = double(kH) / double(VpUnit::CutFadeJudge::kGridH);
		const double shiftPxX = shiftCellsX * pxPerCellX;
		const double shiftPxY = shiftCellsY * pxPerCellY;
		std::vector<uint16_t> half(size_t(kW) * kH);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kW; ++x)
			{
				const double sx = double(x) + shiftPxX;
				const double sy = double(y) + shiftPxY;
				const double pat = 0.50
					+ 0.22 * std::sin(sx * 0.22 + sy * 0.07)
					+ 0.16 * std::cos(sx * 0.13 - sy * 0.19)
					+ 0.09 * std::sin(sx * 0.053 + sy * 0.089);
				const double v = kBlack + gain * pat * 48000.0 + double(Noise(x, y, k));
				half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
			}
		return MakeRecordFromHalf(half);
	}

	inline Record ConstantFlatPicture(double levelCodes)
	{
		const uint16_t code = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(levelCodes * 256.0 + 0.5))));
		std::vector<uint16_t> half(size_t(kW) * kH, code);
		return MakeRecordFromHalf(half);
	}

	inline Record SmoothHorizontalGradientPicture(double rangeCodes)
	{
		std::vector<uint16_t> half(size_t(kW) * kH);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kW; ++x)
			{
				const double v = kBlack + rangeCodes * 256.0 * double(x) / double(kW - 1);
				half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
			}
		return MakeRecordFromHalf(half);
	}

	inline std::vector<Record> BuildLocalDarkeningBoxSequence(int n)
	{
		std::vector<Record> recs;
		recs.reserve(size_t(n));
		for (int i = 0; i < n; ++i)
		{
			const double boxGain = i < 4 ? 1.0 : (i < n - 4 ? 1.0 - double(i - 3) / double(n - 7) : 0.0);
			std::vector<uint16_t> half(size_t(kW) * kH);
			for (uint32_t y = 0; y < kH; ++y)
				for (uint32_t x = 0; x < kW; ++x)
				{
					const double base = 0.25 + 0.65 * double(x) / double(kW) + 0.10 * ((y * 7u + x * 3u) % 11u) / 10.0;
					const bool inBox = (x >= kW * 7u / 10u);
					const double g = inBox ? boxGain : 1.0;
					const double v = kBlack + g * base * 48000.0 + double(Noise(x, y, uint32_t(i)));
					half[size_t(y) * kW + x] = static_cast<uint16_t>(v < 0.0 ? 0.0 : (v > 65535.0 ? 65535.0 : v));
				}
			recs.push_back(MakeRecordFromHalf(half));
		}
		return recs;
	}

	inline std::vector<Record> BuildVerticalTiltSequence(int n)
	{
		std::vector<Record> recs;
		recs.reserve(size_t(n));
		for (int i = 0; i < n; ++i)
		{
			std::vector<uint16_t> half(size_t(kW) * kH);
			for (uint32_t y = 0; y < kH; ++y)
				for (uint32_t x = 0; x < kW; ++x)
				{
					const double sy = double(y) + double(i) * 1.5;
					const double grad = 1.0 / (1.0 + std::exp((sy - double(kH) * 0.65) / 14.0));
					const double tex = 0.08 * std::sin(double(x) * 0.25) * std::cos(sy * 0.20);
					const double v = kBlack + (grad + tex + 0.10) * 44000.0 + double(Noise(x, y, uint32_t(i)));
					half[size_t(y) * kW + x] = static_cast<uint16_t>(v < 0.0 ? 0.0 : (v > 65535.0 ? 65535.0 : v));
				}
			recs.push_back(MakeRecordFromHalf(half));
		}
		return recs;
	}

	struct BenchStats
	{
		double meanUs = 0.0;
		double p50Us = 0.0;
		double p99Us = 0.0;
		double maxUs = 0.0;
	};

	inline BenchStats ComputeBenchStats(std::vector<double> samplesUs)
	{
		BenchStats s;
		if (samplesUs.empty())
			return s;
		std::sort(samplesUs.begin(), samplesUs.end());
		double sum = 0.0;
		for (double v : samplesUs)
			sum += v;
		s.meanUs = sum / double(samplesUs.size());
		s.p50Us = samplesUs[samplesUs.size() / 2];
		const size_t p99Idx = (std::min)(samplesUs.size() - 1, (samplesUs.size() * 99) / 100);
		s.p99Us = samplesUs[p99Idx];
		s.maxUs = samplesUs.back();
		return s;
	}

	TEST_CLASS(CutFadeJudgeFadeRunTests)
	{
	public:
		TEST_METHOD(UnitVersionIsFive)
		{
			Assert::AreEqual(5u, VpUnit::CutFadeJudge::kUnitVersion, L"U7 v5 derived D4/D5/D6/D8 fade and veto rules (5, after v4 576-cell spatial affine fade law, v3 multi-frame fade, and v2 T2 sequence law)");
		}

		// A 300-frame fade (0.25 % of gain per frame): the pair law finds none of it; the run finds it once the gain moved 2 %.
		TEST_METHOD(ASlowFadeIsFoundByTheRunWhereThePairLawMissesIt)
		{
			const int pre = 20, n = 300, post = 20;
			const std::vector<Pair> p = Play(FadeSequence(n, pre, post, 0.25));
			Assert::AreEqual(0, Count(p, 1, size_t(pre)), L"no fade on the static frames before");
			Assert::IsTrue(Count(p, size_t(pre), size_t(pre + n), true) <= n / 10, L"the pair law alone misses the slow fade (unit version 1)");
			Assert::IsTrue(Count(p, size_t(pre), size_t(pre + n)) >= n - 15, L"the run finds the fade after a 2 % gain");
			Assert::IsTrue(Count(p, size_t(pre + n), p.size()) <= 2, L"at most a frame or two of lag after the fade ends");
			for (const Pair& q : p)
			{
				Assert::IsFalse(q.cut, L"a fade is never a cut");
				Assert::IsFalse(q.flash);
			}
		}

		// The same gain path ten times faster: the same law, no rate or length in it.
		TEST_METHOD(AFastFadeIsFoundByTheSameLaw)
		{
			const int pre = 20, n = 30, post = 20;
			const std::vector<Pair> p = Play(FadeSequence(n, pre, post, 0.25));
			Assert::AreEqual(0, Count(p, 1, size_t(pre)));
			Assert::IsTrue(Count(p, size_t(pre), size_t(pre + n)) >= n - 2, L"every fade pair after the first two");
			Assert::IsTrue(Count(p, size_t(pre + n), p.size()) <= 2);
		}

		TEST_METHOD(AStaticPictureAndAPanAreNoFade)
		{
			std::vector<Record> still, pan;
			for (uint32_t k = 0; k < 120; ++k)
				still.push_back(Picture(1.0, k));
			for (uint32_t k = 0; k < 100; ++k)
				pan.push_back(Picture(1.0, k, 2 * k, true));
			Assert::AreEqual(0, Count(Play(still), 1, still.size()), L"noise is no fade");
			Assert::AreEqual(0, Count(Play(pan), 1, pan.size()), L"a pan shifts the picture; it does not scale it about black");
		}

		// Every fade frame shown twice (a repeat cadence): a repeated record is never a fade and the run holds through it.
		TEST_METHOD(RepeatedRecordsHoldTheRun)
		{
			std::vector<Record> recs;
			uint32_t k = 0;
			for (int i = 0; i < 20; ++i)
				recs.push_back(Picture(1.0, k++));
			for (int i = 1; i <= 60; ++i)
			{
				const Record r = Picture(1.0 - 0.5 * i / 60, k++);
				recs.push_back(r);
				recs.push_back(r);
			}
			const std::vector<Pair> p = Play(recs);
			int repeatFades = 0, moving = 0, movingFades = 0;
			for (size_t j = 21; j < p.size(); j += 2)
				repeatFades += p[j].Fade() ? 1 : 0;
			for (size_t j = 22; j < p.size(); j += 2)
			{
				++moving;
				movingFades += p[j].Fade() ? 1 : 0;
			}
			Assert::AreEqual(0, repeatFades, L"a repeated record changed nothing");
			Assert::IsTrue(movingFades >= moving - 4, L"the fade goes on across the repeats");
		}

		// A hard cut inside a fade stays a cut (the pair law, unchanged) and ends the run.
		TEST_METHOD(ACutInsideAFadeStaysACutAndEndsTheRun)
		{
			std::vector<Record> recs = FadeSequence(30, 20, 0, 0.25);
			recs.resize(35);
			recs.push_back(Flat(35));
			recs.push_back(Flat(36));
			FadeRun run;
			bool fadeBeforeCut = false;
			for (size_t k = 1; k < recs.size(); ++k)
			{
				const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[k - 1], recs[k], false, false);
				const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[k - 1], recs[k], e, run);
				if (k == 34)
					fadeBeforeCut = runFade || e.fade;
				if (k == 35)
				{
					Assert::IsTrue(e.cut, L"the jump to another picture is a cut");
					Assert::IsFalse(e.fade);
					Assert::IsFalse(runFade, L"a cut is never a fade");
					Assert::AreEqual(0, run.direction, L"the cut ends the run");
					Assert::AreEqual(0u, run.pairs);
				}
			}
			Assert::IsTrue(fadeBeforeCut, L"the fade before the cut was found");
		}

		// The run is continued only by the pair that starts at its end record (a skipped frame or a restarted numbering starts over).
		TEST_METHOD(ARunContinuesOnlyFromItsOwnEndRecord)
		{
			const std::vector<Record> recs = FadeSequence(30, 20, 20, 0.25);
			FadeRun run;
			bool last = false;
			for (size_t k = 1; k <= 30; ++k)
				last = VpUnit::CutFadeJudge::FoldFadeRun(recs[k - 1], recs[k], VpUnit::CutFadeJudge::ClassifyEvents(recs[k - 1], recs[k], false, false), run);
			Assert::IsTrue(last, L"the run is a fade by frame 30");
			Assert::IsTrue(VpUnit::CutFadeJudge::FadeRunEndsAt(run, recs[30]));
			Assert::IsFalse(VpUnit::CutFadeJudge::FadeRunEndsAt(run, recs[31]));
			const bool skipped = VpUnit::CutFadeJudge::FoldFadeRun(recs[31], recs[32], VpUnit::CutFadeJudge::ClassifyEvents(recs[31], recs[32], false, false), run);
			Assert::IsFalse(skipped, L"one pair of a new run is no fade yet");
			Assert::AreEqual(1u, run.pairs, L"a new run started at the pair");
		}

		// A hard cut from a static dim shot directly into black stays a cut (never reclassified as a fade).
		TEST_METHOD(AHardCutFromDimShotIntoBlackStaysACut)
		{
			const double gains[] = { 0.15, 0.015 };
			const bool noisyFlags[] = { false, true };
			for (double gain : gains)
			{
				for (bool noisy : noisyFlags)
				{
					std::vector<Record> recs;
					uint32_t k = 0;
					for (int i = 0; i < 10; ++i)
						recs.push_back(Picture(gain, k++));
					for (int i = 0; i < 10; ++i)
						recs.push_back(Black(k++, noisy));

					FadeRun run;
					for (size_t i = 1; i < recs.size(); ++i)
					{
						const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[i - 1], recs[i], false, false);
						const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[i - 1], recs[i], e, run);
						if (i == 10)
						{
							Assert::IsTrue(e.cut, L"hard cut from dim shot into black must be a cut");
							Assert::IsTrue(e.cutPossible, L"hard cut into black sets cutPossible");
							Assert::IsFalse(e.fade, L"hard cut into black must not be a pair fade");
							Assert::IsFalse(runFade, L"hard cut into black must not be a run fade");
							Assert::AreEqual(0, run.direction, L"cut into black without active fade leaves no run");
							Assert::AreEqual(0u, run.pairs, L"cut into black without active fade resets run pairs");
						}
						else
						{
							Assert::IsFalse(e.fade || runFade, L"static dim or static black frames must not fade");
						}
					}
				}
			}
		}

		// A hard cut from black directly into a static dim shot stays a cut (never reclassified as a fade).
		TEST_METHOD(AHardCutFromBlackIntoDimShotStaysACut)
		{
			const double gains[] = { 0.15, 0.015 };
			for (double gain : gains)
			{
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < 10; ++i)
					recs.push_back(Black(k++));
				for (int i = 0; i < 10; ++i)
					recs.push_back(Picture(gain, k++));

				FadeRun run;
				for (size_t i = 1; i < recs.size(); ++i)
				{
					const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[i - 1], recs[i], false, false);
					const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[i - 1], recs[i], e, run);
					if (i == 10)
					{
						Assert::IsTrue(e.cut, L"hard cut from black into dim shot must be a cut");
						Assert::IsTrue(e.cutPossible, L"hard cut from black sets cutPossible");
						Assert::IsFalse(e.fade, L"hard cut from black must not be a pair fade");
						Assert::IsFalse(runFade, L"hard cut from black must not be a run fade");
						if (gain >= 0.1)
						{
							Assert::AreEqual(0, run.direction, L"large jump from black does not seed a fade run");
							Assert::AreEqual(0u, run.pairs, L"large jump from black resets run pairs");
						}
					}
					else
					{
						Assert::IsFalse(e.fade || runFade, L"static black or static dim frames must not fade");
					}
				}
			}
		}

		// A genuine multi-frame fade-out to black and fade-in back from black is still detected across the black boundaries.
		TEST_METHOD(AGenuineMultiFrameFadeToBlackAndBackFromBlackIsStillAFade)
		{
			const int pre = 10, nOut = 48, holdBlack = 12, nIn = 48, post = 10;
			std::vector<Record> recs;
			uint32_t k = 0;
			for (int i = 0; i < pre; ++i)
				recs.push_back(Picture(1.0, k++));
			for (int i = 1; i <= nOut; ++i)
				recs.push_back(i == nOut ? Black(k++) : Picture(1.0 - double(i) / double(nOut), k++));
			for (int i = 0; i < holdBlack; ++i)
				recs.push_back(Black(k++));
			for (int i = 1; i <= nIn; ++i)
				recs.push_back(Picture(double(i) / double(nIn), k++));
			for (int i = 0; i < post; ++i)
				recs.push_back(Picture(1.0, k++));

			const size_t outBegin = size_t(pre);
			const size_t outEnd = outBegin + size_t(nOut);
			const size_t blackEnd = outEnd + size_t(holdBlack);
			const size_t inEnd = blackEnd + size_t(nIn);

			FadeRun run;
			int staticPreFades = 0, fadeOutFades = 0, staticBlackFades = 0, fadeInFades = 0, staticPostFades = 0;
			bool hitBlackIsFade = false;
			double fadeOutKeepProd = 1.0;

			for (size_t i = 1; i < recs.size(); ++i)
			{
				const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[i - 1], recs[i], false, false);
				double runKeep = 1.0;
				const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[i - 1], recs[i], e, run, &runKeep);
				const bool isFade = e.fade || runFade;
				if (i < outBegin)
					staticPreFades += isFade ? 1 : 0;
				else if (i < outEnd)
				{
					fadeOutFades += isFade ? 1 : 0;
					if (i == outEnd - 1)
						hitBlackIsFade = isFade;
					fadeOutKeepProd *= VpUnit::CutFadeJudge::FrameFadeKeep(recs[i - 1], recs[i], e.fade, runFade, runKeep);
				}
				else if (i < blackEnd)
					staticBlackFades += isFade ? 1 : 0;
				else if (i < inEnd)
					fadeInFades += isFade ? 1 : 0;
				else
					staticPostFades += isFade ? 1 : 0;
			}

			Assert::AreEqual(0, staticPreFades, L"no fade on static pre-fade frames");
			Assert::AreEqual(0, staticBlackFades, L"no fade on static black hold frames");
			Assert::AreEqual(0, staticPostFades, L"no fade on static post-fade frames");
			Assert::IsTrue(fadeOutFades >= 46, L"genuine 48-frame fade-out to black must fire on >= 46/48 frames");
			Assert::IsTrue(hitBlackIsFade, L"final fade-out frame hitting black must stay a fade");
			Assert::IsTrue(fadeInFades >= 46, L"genuine 48-frame fade-in from black must fire on >= 46/48 frames");
			Assert::IsTrue(fadeOutKeepProd < 0.5, L"fade-out to black must decay history keep well below 0.5");
		}

		// Local foreground darkening (30% of the picture darkens smoothly while 70% stays constant) must NOT be called a global fade.
		TEST_METHOD(ALocalDarkeningBoxIsNotAFade)
		{
			std::vector<Record> recs;
			for (int i = 0; i < 30; ++i)
			{
				const double boxGain = i < 5 ? 1.0 : (i < 25 ? 1.0 - double(i - 4) / 20.0 : 0.0);
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double base = 0.25 + 0.65 * double(x) / double(kW) + 0.10 * ((y * 7u + x * 3u) % 11u) / 10.0;
						// Place the darkening box on the bright side (right 30%) so both mean and p99 drop together,
						// ensuring the test defeats 2-scalar (mean, p99) laws and validates the 576-cell spatial law.
						const bool inBox = (x >= kW * 7u / 10u);
						const double g = inBox ? boxGain : 1.0;
						const double v = kBlack + g * base * 48000.0 + Noise(x, y, uint32_t(i));
						half[size_t(y) * kW + x] = static_cast<uint16_t>(v < 0.0 ? 0.0 : (v > 65535.0 ? 65535.0 : v));
					}
				std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
				Record r;
				Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()));
				Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r));
				recs.push_back(r);
			}
			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, recs.size()), L"30% local darkening box must emit 0 fades");
		}

		// Vertical camera tilt across a bright-to-dark scene gradient must NOT be called a fade.
		TEST_METHOD(ACameraTiltAcrossAGradientIsNotAFade)
		{
			std::vector<Record> recs;
			for (int i = 0; i < 30; ++i)
			{
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double sy = double(y) + double(i) * 1.5;
						const double grad = 1.0 / (1.0 + std::exp((sy - double(kH) * 0.65) / 14.0));
						const double tex = 0.08 * std::sin(double(x) * 0.25) * std::cos(sy * 0.20);
						const double v = kBlack + (grad + tex + 0.10) * 44000.0 + Noise(x, y, uint32_t(i));
						half[size_t(y) * kW + x] = static_cast<uint16_t>(v < 0.0 ? 0.0 : (v > 65535.0 ? 65535.0 : v));
					}
				std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
				Record r;
				Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()));
				Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r));
				recs.push_back(r);
			}
			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, recs.size()), L"vertical camera tilt across a gradient must emit 0 fades");
		}

		// A simultaneous horizontal camera pan and linear fade to black is detected cleanly with zero pre/post false fades.
		TEST_METHOD(APanWithSimultaneousFadeIsDetected)
		{
			const int pre = 8, nFade = 48, post = 8;
			std::vector<Record> recs;
			for (int i = 0; i < pre + nFade + post; ++i)
			{
				const double gain = i < pre ? 1.0 : (i < pre + nFade ? 1.0 - double(i - pre + 1) / double(nFade) : 0.0);
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double sx = double(x) + double(i) * 3.0;
						const double pat = 0.45 + 0.30 * std::sin(sx * 0.09) + 0.20 * std::cos(double(y) * 0.13 + sx * 0.05);
						const double v = kBlack + gain * pat * 48000.0 + Noise(x, y, uint32_t(i));
						half[size_t(y) * kW + x] = static_cast<uint16_t>(v < 0.0 ? 0.0 : (v > 65535.0 ? 65535.0 : v));
					}
				std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
				Record r;
				Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()));
				Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r));
				recs.push_back(r);
			}
			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, size_t(pre)), L"no fade during pre-fade pure pan");
			Assert::IsTrue(Count(res, size_t(pre), size_t(pre + nFade)) >= 44, L"pan + 48-frame fade-out must fire on >= 44/48 frames");
			Assert::AreEqual(0, Count(res, size_t(pre + nFade), recs.size()), L"no fade during post-fade black hold");
		}

		// Item 2 (2417->2418->2419 shape): black hold -> 1-frame +3 code flat lift -> hard cut into bright or dim/moderate
		// textured shot (including aMean < 24 codes) must stay a cut, never co-label as fade, and reset the run.
		TEST_METHOD(AHardCutAfterOneFrameLiftFromBlackStaysACutAndResetsRun)
		{
			auto FlatLift = [](uint32_t k, double noiseScale) -> Record {
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double v = kBlack + 3.0 * 256.0 + double(Noise(x, y, k)) * noiseScale;
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
				Record r;
				Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()));
				Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r));
				return r;
			};

			// Test bright (0.8), dim (0.15: aMean ~13.5 < 24 codes), and moderate-dim (0.10: aMean ~8 < 24 & aSpread ~18 < 24 codes)
			// across both spread <= 1 code (noiseScale 0.5) and spread ~1.5 codes with Flatness < kFlatF (noiseScale 1.0).
			const double gains[] = { 0.8, 0.15, 0.10 };
			const double liftNoiseScales[] = { 0.5, 1.0 };
			for (double gain : gains)
			{
				for (double liftNoise : liftNoiseScales)
				{
					std::vector<Record> recs;
					uint32_t k = 0;
					for (int i = 0; i < 8; ++i)
						recs.push_back(Black(k++));
					recs.push_back(FlatLift(k++, liftNoise));   // index 8: 1-frame +3 code flat lift from black
					for (int i = 0; i < 8; ++i)
						recs.push_back(Picture(gain, k++));     // index 9: hard cut into textured shot

					FadeRun run;
					for (size_t i = 1; i < recs.size(); ++i)
					{
						const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[i - 1], recs[i], false, false);
						const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[i - 1], recs[i], e, run);
						if (i == 9)
						{
							Assert::IsTrue(e.cut, L"hard cut after 1-frame flat lift from black must be a cut");
							Assert::IsFalse(e.fade, L"hard cut after 1-frame flat lift must not be a pair fade");
							Assert::IsFalse(runFade, L"hard cut after 1-frame flat lift must not be co-labelled as a run fade");
							Assert::AreEqual(0, run.direction, L"hard cut after 1-frame flat lift must reset run direction");
							Assert::AreEqual(0u, run.pairs, L"hard cut after 1-frame flat lift must reset run pairs");
						}
						else
						{
							Assert::IsFalse(e.fade || runFade, L"static black, single flat lift, or static post-cut shot must not fade");
						}
					}
				}
			}
		}

		// Item 2 (2323->2324->2325 shape): genuine fade-out to black -> black hold -> hard cut into bright or dim shot
		// must emit 0 fades during the black hold and stay a clean cut with reset run at the cut frame.
		TEST_METHOD(AFadeOutThenBlackHoldThenHardCutIntoBrightShotStaysACut)
		{
			const int pre = 8, nOut = 48, holdBlack = 12, post = 8;
			const double postGains[] = { 0.8, 0.15 };
			const bool noisyBlackFlags[] = { false, true };
			for (double postGain : postGains)
			{
				for (bool noisyBlack : noisyBlackFlags)
				{
					std::vector<Record> recs;
					uint32_t k = 0;
					for (int i = 0; i < pre; ++i)
						recs.push_back(Picture(1.0, k++));
					for (int i = 1; i <= nOut; ++i)
						recs.push_back(i == nOut ? Black(k++, noisyBlack) : Picture(1.0 - double(i) / double(nOut), k++));
					for (int i = 0; i < holdBlack; ++i)
						recs.push_back(Black(k++, noisyBlack));
					for (int i = 0; i < post; ++i)
						recs.push_back(Picture(postGain, k++));

					const size_t outBegin = size_t(pre);
					const size_t outEnd = outBegin + size_t(nOut);
					const size_t blackEnd = outEnd + size_t(holdBlack);

					FadeRun run;
					int fadeOutFades = 0, blackHoldFades = 0, postCutFades = 0;
					for (size_t i = 1; i < recs.size(); ++i)
					{
						const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[i - 1], recs[i], false, false);
						const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[i - 1], recs[i], e, run);
						const bool isFade = e.fade || runFade;
						if (i >= outBegin && i < outEnd)
							fadeOutFades += isFade ? 1 : 0;
						else if (i >= outEnd && i < blackEnd)
							blackHoldFades += isFade ? 1 : 0;
						else if (i == blackEnd)
						{
							Assert::IsTrue(e.cut, L"hard cut out of black hold after fade-out must be a cut");
							Assert::IsFalse(e.fade, L"hard cut out of black hold must not be a pair fade");
							Assert::IsFalse(runFade, L"hard cut out of black hold must not be a run fade");
							Assert::AreEqual(0, run.direction, L"hard cut out of black hold must reset run direction");
							Assert::AreEqual(0u, run.pairs, L"hard cut out of black hold must reset run pairs");
						}
						else if (i > blackEnd)
							postCutFades += isFade ? 1 : 0;
					}
					Assert::IsTrue(fadeOutFades >= 45, L"genuine fade-out before black hold must be detected");
					Assert::AreEqual(0, blackHoldFades, L"black hold after fade-out must emit 0 fades");
					Assert::AreEqual(0, postCutFades, L"static post-cut shot must emit 0 fades");
				}
			}
		}

		// Item 1 (CutTracker fade-in-from-black veto): a genuine fast fade-in from flat black emits 0 T2 cuts,
		// while a hard cut from flat black into a static shot still emits a T2 cut at the cut frame.
		TEST_METHOD(CutTrackerVetoesFadeInFromBlackWhileKeepingHardCutFromBlack)
		{
			// 1. Fast 10-frame fade-in from flat black (frame 1 jumps past L >= kSpikeL and H > kHardH).
			{
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < 8; ++i)
					recs.push_back(Black(k++));
				for (int i = 1; i <= 10; ++i)
					recs.push_back(Picture(double(i) / 10.0, k++));
				for (int i = 0; i < 6; ++i)
					recs.push_back(Picture(1.0, k++));

				VpUnit::CutFadeJudge::CutTracker tracker;
				int cuts = 0;
				for (size_t i = 0; i < recs.size(); ++i)
				{
					const VpUnit::CutFadeJudge::CutVerdict v = tracker.Push(static_cast<uint64_t>(i) + 1, recs[i]);
					if (v.known && v.cut)
						++cuts;
				}
				Assert::AreEqual(0, cuts, L"genuine fast fade-in from flat black must emit 0 T2 cuts");
			}

			// 2. Hard cut from flat black into a static shot (both full brightness and dim) stays a T2 cut.
			for (double gain : { 1.0, 0.25 })
			{
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < 8; ++i)
					recs.push_back(Black(k++));
				for (int i = 0; i < 8; ++i)
					recs.push_back(Picture(gain, k++));

				VpUnit::CutFadeJudge::CutTracker tracker;
				std::vector<int> cutFrames;
				for (size_t i = 0; i < recs.size(); ++i)
				{
					const VpUnit::CutFadeJudge::CutVerdict v = tracker.Push(static_cast<uint64_t>(i) + 1, recs[i]);
					if (v.known && v.cut)
						cutFrames.push_back(static_cast<int>(v.seq) - 1);
				}
				Assert::AreEqual(size_t(1), cutFrames.size(), L"hard cut from flat black into a static shot must emit exactly 1 T2 cut");
				Assert::AreEqual(8, cutFrames[0], L"T2 cut must fire at the exact cut frame");
			}
		}

		// D10 decision-identity pin + pan alignment negative control + Opt D fast-skip boundary checks.
		// Pins optimized AlignGrid / FitGridFadeGrid / FitGridFadeAligned against the frozen pre-D10 W1 v4 reference,
		// pins AlignedPairCache (Opt C) bit-for-bit against uncached ClassifyEvents + FoldFadeRun + FrameFadeKeep,
		// and proves the pan alignment hazard via an in-test naive unaligned baseline (Rule 3).
		TEST_METHOD(D10OptimizedAlignAndFitAreDecisionIdenticalToReference)
		{
			using namespace VpUnit::CutFadeJudge;

			struct SeqCase
			{
				const char* name;
				std::vector<Record> recs;
			};

			auto BuildPanFade = [](double dxCells, double dyCells, int nFrames) -> std::vector<Record> {
				std::vector<Record> recs;
				recs.reserve(size_t(nFrames));
				for (int i = 0; i < nFrames; ++i)
				{
					const double gain = 1.0 - 0.06 * double(i);
					recs.push_back(TexturedPanFadePicture(gain, dxCells * double(i), dyCells * double(i), uint32_t(1000 + i)));
				}
				return recs;
			};

			std::vector<SeqCase> matrix;
			{
				std::vector<Record> still;
				for (uint32_t k = 0; k < 20; ++k)
					still.push_back(Picture(1.0, k));
				matrix.push_back({ "static", std::move(still) });
			}
			matrix.push_back({ "slow_fade_300", FadeSequence(300, 6, 6, 0.25) });
			matrix.push_back({ "fast_fade_30", FadeSequence(30, 6, 6, 0.25) });
			matrix.push_back({ "pan_fade_int_pos1", BuildPanFade(+1.0, 0.0, 10) });
			matrix.push_back({ "pan_fade_int_neg1", BuildPanFade(-1.0, 0.0, 10) });
			matrix.push_back({ "pan_fade_int_pos2", BuildPanFade(+2.0, 0.0, 10) });
			matrix.push_back({ "pan_fade_int_neg2", BuildPanFade(-2.0, 0.0, 10) });
			matrix.push_back({ "pan_fade_sub_0p3", BuildPanFade(+0.3, 0.0, 10) });
			matrix.push_back({ "pan_fade_sub_0p7", BuildPanFade(+0.7, 0.0, 10) });
			matrix.push_back({ "pan_fade_sub_1p5", BuildPanFade(+1.5, 0.0, 10) });
			matrix.push_back({ "pan_fade_sub_neg1p5", BuildPanFade(-1.5, 0.0, 10) });
			matrix.push_back({ "pan_fade_sub_1p7", BuildPanFade(+1.7, 0.0, 10) });
			matrix.push_back({ "pan_fade_sub_neg1p7", BuildPanFade(-1.7, 0.0, 10) });
			matrix.push_back({ "pan_fade_diag_1p5_neg0p7", BuildPanFade(+1.5, -0.7, 10) });
			matrix.push_back({ "pan_fade_y_pos2", BuildPanFade(0.0, +2.0, 10) });
			matrix.push_back({ "pan_fade_y_neg2", BuildPanFade(0.0, -2.0, 10) });
			matrix.push_back({ "vertical_tilt", BuildVerticalTiltSequence(20) });
			matrix.push_back({ "local_darkening_box", BuildLocalDarkeningBoxSequence(25) });
			{
				std::vector<Record> bo;
				uint32_t k = 0;
				for (int i = 0; i < 6; ++i)
					bo.push_back(Picture(1.0, k++));
				for (int i = 1; i <= 24; ++i)
					bo.push_back(i == 24 ? Black(k++) : Picture(1.0 - double(i) / 24.0, k++));
				for (int i = 0; i < 6; ++i)
					bo.push_back(Black(k++));
				for (int i = 1; i <= 24; ++i)
					bo.push_back(Picture(double(i) / 24.0, k++));
				for (int i = 0; i < 6; ++i)
					bo.push_back(Picture(1.0, k++));
				matrix.push_back({ "black_origin_fade_out_in", std::move(bo) });
			}
			{
				std::vector<Record> hc;
				uint32_t k = 0;
				for (int i = 0; i < 6; ++i)
					hc.push_back(Black(k++));
				for (int i = 0; i < 6; ++i)
					hc.push_back(Picture(0.8, k++));
				for (int i = 0; i < 6; ++i)
					hc.push_back(Black(k++));
				for (int i = 0; i < 6; ++i)
					hc.push_back(Picture(0.15, k++));
				for (int i = 0; i < 6; ++i)
					hc.push_back(Black(k++));
				matrix.push_back({ "hard_cuts_black", std::move(hc) });
			}

			int totalPairsChecked = 0;
			int totalValidFits = 0;
			int totalCachedFramesChecked = 0;

			// Part 1: Pairwise AlignGrid + FitGridFadeGrid + FitGridFadeAligned vs frozen W1 v4 reference,
			// AND full ClassifyEvents + FoldFadeRun + FrameFadeKeep WITH vs WITHOUT AlignedPairCache.
			for (const SeqCase& sc : matrix)
			{
				FadeRun runCached, runUncached;
				for (size_t i = 1; i < sc.recs.size(); ++i)
				{
					double prevG[kGridH][kGridW], curG[kGridH][kGridW];
					LoadGridCodes(sc.recs[i - 1].cells, prevG);
					LoadGridCodes(sc.recs[i].cells, curG);

					for (int rad : { kShiftRadius, kLocalRadius })
					{
						double refA_ref[kGridH][kGridW], curA_ref[kGridH][kGridW];
						double vxRef = 0.0, vyRef = 0.0;
						W1RefV4::ReferenceAlignGrid(prevG, curG, rad, refA_ref, curA_ref, &vxRef, &vyRef);
						const GridFadeFit fitRef = W1RefV4::ReferenceFitGridFadeGrid(prevG, curG, rad);

						double refA_opt[kGridH][kGridW], curA_opt[kGridH][kGridW];
						double vxOpt = 0.0, vyOpt = 0.0;
						AlignGrid(prevG, curG, rad, refA_opt, curA_opt, &vxOpt, &vyOpt);
						const GridFadeFit fitOpt = FitGridFadeGrid(prevG, curG, rad);
						const GridFadeFit fitAligned = FitGridFadeAligned(refA_opt, curA_opt, vxOpt, vyOpt);

						Assert::IsTrue(std::fabs(vxOpt - vxRef) <= 1e-12, L"AlignGrid vx must match frozen W1 v4 reference within 1e-12");
						Assert::IsTrue(std::fabs(vyOpt - vyRef) <= 1e-12, L"AlignGrid vy must match frozen W1 v4 reference within 1e-12");
						Assert::AreEqual(fitRef.valid, fitOpt.valid, L"FitGridFadeGrid valid must match frozen W1 v4 reference");
						Assert::AreEqual(fitRef.dir, fitOpt.dir, L"FitGridFadeGrid dir must match frozen W1 v4 reference");
						Assert::AreEqual(fitOpt.valid, fitAligned.valid, L"FitGridFadeAligned valid must match FitGridFadeGrid");
						Assert::AreEqual(fitOpt.dir, fitAligned.dir, L"FitGridFadeAligned dir must match FitGridFadeGrid");
						if (fitRef.valid)
						{
							++totalValidFits;
							Assert::IsTrue(std::fabs(fitOpt.gain - fitRef.gain) <= 1e-12, L"FitGridFadeGrid gain must match W1 v4 reference within 1e-12");
							Assert::IsTrue(std::fabs(fitOpt.black - fitRef.black) <= 1e-9, L"FitGridFadeGrid black must match W1 v4 reference within 1e-9");
							Assert::IsTrue(std::fabs(fitAligned.gain - fitOpt.gain) <= 1e-12, L"FitGridFadeAligned gain must match FitGridFadeGrid within 1e-12");
							Assert::IsTrue(std::fabs(fitAligned.black - fitOpt.black) <= 1e-9, L"FitGridFadeAligned black must match FitGridFadeGrid within 1e-9");
						}
						++totalPairsChecked;
					}

					AlignedPairCache cache;
					const Events eCached = ClassifyEvents(sc.recs[i - 1], sc.recs[i], false, false, &cache);
					const Events eUncached = ClassifyEvents(sc.recs[i - 1], sc.recs[i], false, false, nullptr);
					Assert::AreEqual(eUncached.cut, eCached.cut, L"AlignedPairCache must not alter e.cut");
					Assert::AreEqual(eUncached.cutPossible, eCached.cutPossible, L"AlignedPairCache must not alter e.cutPossible");
					Assert::AreEqual(eUncached.fade, eCached.fade, L"AlignedPairCache must not alter e.fade");
					Assert::AreEqual(eUncached.flash, eCached.flash, L"AlignedPairCache must not alter e.flash");
					Assert::AreEqual(eUncached.histDistance, eCached.histDistance, L"AlignedPairCache must not alter e.histDistance");

					double keepCached = 1.0, keepUncached = 1.0;
					const bool runFadeCached = FoldFadeRun(sc.recs[i - 1], sc.recs[i], eCached, runCached, &keepCached, &cache);
					const bool runFadeUncached = FoldFadeRun(sc.recs[i - 1], sc.recs[i], eUncached, runUncached, &keepUncached, nullptr);
					Assert::AreEqual(runFadeUncached, runFadeCached, L"AlignedPairCache must produce identical runFade");
					Assert::IsTrue(std::fabs(keepCached - keepUncached) <= 1e-12, L"AlignedPairCache must produce identical run frameKeep");

					const double fkCached = FrameFadeKeep(sc.recs[i - 1], sc.recs[i], eCached.fade, runFadeCached, keepCached);
					const double fkUncached = FrameFadeKeep(sc.recs[i - 1], sc.recs[i], eUncached.fade, runFadeUncached, keepUncached);
					Assert::IsTrue(std::fabs(fkCached - fkUncached) <= 1e-12, L"AlignedPairCache must produce identical FrameFadeKeep");
					++totalCachedFramesChecked;
				}
			}
			Assert::IsTrue(totalPairsChecked > 800, L"decision-identity matrix must exercise > 800 pair/radius combinations");
			Assert::IsTrue(totalValidFits > 200, L"decision-identity matrix must exercise > 200 valid fade fits");
			Assert::IsTrue(totalCachedFramesChecked > 400, L"AlignedPairCache parity must exercise > 400 stream frames");

			// Part 2 (Mandatory In-Test Negative Control, Rule 3): on pan+fade sequences (shift = +-1.5, +-1.7, +-2.0 cells/frame),
			// run the naive unaligned baseline FitGridFadeAligned(prevG, curG, 0.0, 0.0) on the EXACT SAME inputs and assert
			// it FAILS (valid == false or gain error > 0.05) while optimized AlignGrid + FitGridFadeGrid recovers true gain & shift.
			const double panShifts[] = { +1.5, -1.5, +1.7, -1.7, +2.0, -2.0 };
			int negControlPairs = 0;
			for (double shiftCells : panShifts)
			{
				const std::vector<Record> recs = BuildPanFade(shiftCells, 0.0, 10);
				for (size_t i = 1; i < recs.size(); ++i)
				{
					const double trueGain = (1.0 - 0.06 * double(i)) / (1.0 - 0.06 * double(i - 1));
					double prevG[kGridH][kGridW], curG[kGridH][kGridW];
					LoadGridCodes(recs[i - 1].cells, prevG);
					LoadGridCodes(recs[i].cells, curG);

					const GridFadeFit naiveFit = FitGridFadeAligned(prevG, curG, 0.0, 0.0);
					const bool naiveFailed = !naiveFit.valid || std::fabs(naiveFit.gain - trueGain) > 0.05;
					Assert::IsTrue(naiveFailed, L"Negative control: naive unaligned FitGridFadeAligned(0, 0) must fail on 1.5..2.0 cell/frame pan+fade");

					const GridFadeFit optFit = FitGridFadeGrid(prevG, curG, kLocalRadius);
					Assert::IsTrue(optFit.valid, L"Aligned optimized FitGridFadeGrid must detect pan+fade accurately");
					Assert::AreEqual(-1, optFit.dir, L"Aligned optimized FitGridFadeGrid must report darkening direction");
					Assert::IsTrue(std::fabs(optFit.gain - trueGain) < 0.02, L"Aligned optimized FitGridFadeGrid must recover true gain within 0.02");
					Assert::IsTrue(std::fabs(optFit.vx - shiftCells) < 0.15, L"Aligned optimized AlignGrid must recover true pan shift within 0.15 cells");
					++negControlPairs;
				}
			}
			Assert::AreEqual(54, negControlPairs, L"all 6 pan speeds x 9 pairs must be verified by negative control");

			// Part 3: CutTracker::Judge Opt D fast-skip boundary checks.
			// 3a. Identical-frame repeat (ahead->score.G == 0.0) after hard cut from black -> confirmed cut still fires.
			{
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < 8; ++i)
					recs.push_back(Black(k++));
				const Record repeated = TexturedPanFadePicture(0.20, 0.0, 0.0, 777u);
				recs.push_back(repeated);   // frame 8: cut from black (kMean - prevMean < 24, darkened == 0)
				recs.push_back(repeated);   // frame 9: identical repeat (G == 0.0)
				for (int i = 0; i < 4; ++i)
					recs.push_back(TexturedPanFadePicture(0.20, 0.0, 0.0, 778u + uint32_t(i)));

				Grid g8, g9;
				GridOf(recs[8], &g8);
				GridOf(recs[9], &g9);
				Assert::AreEqual(0.0, ScorePair(g8, g9).G, 1e-12, L"precondition: identical repeat must yield G == 0.0");
				Assert::IsFalse(W1RefV4::ReferenceFitGridFadeGrid(g8.g, g9.g, kShiftRadius).valid,
					L"precondition: unskipped W1 v4 FitGridFadeGrid returns invalid on identical repeat");

				CutTracker tracker;
				std::vector<int> cutFrames;
				for (size_t i = 0; i < recs.size(); ++i)
				{
					const CutVerdict v = tracker.Push(static_cast<uint64_t>(i) + 1, recs[i]);
					if (v.known && v.cut)
						cutFrames.push_back(static_cast<int>(v.seq) - 1);
				}
				Assert::AreEqual(size_t(1), cutFrames.size(), L"cut from black followed by identical repeat must still emit 1 confirmed cut");
				Assert::AreEqual(8, cutFrames[0], L"confirmed cut must fire at exact cut frame 8");
			}

			// 3b. Truly constant flat ahead frame (ahead->flat with syy == 0) after hard cut from black -> confirmed cut still fires.
			{
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < 8; ++i)
					recs.push_back(Black(k++));
				recs.push_back(TexturedPanFadePicture(0.20, 0.0, 0.0, 888u));   // frame 8: cut from black (kMean - prevMean ~ 18.75 < 24)
				for (int i = 0; i < 6; ++i)
					recs.push_back(ConstantFlatPicture(40.0));                  // frame 9+: constant flat gray (Flatness == 0 < kFlatF, not a flash)

				Grid g8, g9;
				GridOf(recs[8], &g8);
				GridOf(recs[9], &g9);
				Assert::IsTrue(Flatness(g9) < kFlatF, L"precondition: constant flat ahead frame must satisfy ahead->flat");
				Assert::IsFalse(W1RefV4::ReferenceFitGridFadeGrid(g8.g, g9.g, kShiftRadius).valid,
					L"precondition: unskipped W1 v4 FitGridFadeGrid returns invalid on constant flat ahead");

				CutTracker tracker;
				bool cutAtFrame8 = false;
				for (size_t i = 0; i < recs.size(); ++i)
				{
					const CutVerdict v = tracker.Push(static_cast<uint64_t>(i) + 1, recs[i]);
					if (v.known && v.cut && v.seq == 9u)
						cutAtFrame8 = true;
				}
				Assert::IsTrue(cutAtFrame8, L"cut from black followed by constant flat ahead frame must still fire confirmed cut at frame 8");
			}

			// 3c. Adversarial boundary test for Opt D's `ahead->flat` fast-skip:
			// A smooth noiseless low-contrast horizontal gradient fade-in from black (16 -> 16..34 -> 16..40 -> 16..46 ...)
			// has Flatness(ahead) < kFlatF (0.02) YET large spatial variance (sxx, syy >> 1e-6) and an EXACT affine brightening fit
			// (A = 4/3, b = 16, valid == true, dir == +1). Unskipped W1 v4 CutTracker::Judge vetoes the cut (0 cuts);
			// decision-identical D10 must ALSO emit 0 cuts and never skip a genuine gradient fade-in via Flatness < kFlatF.
			{
				std::vector<Record> recs;
				for (int i = 0; i < 8; ++i)
					recs.push_back(ConstantFlatPicture(16.0));
				recs.push_back(SmoothHorizontalGradientPicture(18.0));   // frame 8: range 18 codes, mean 25, G=9 >= 6, H > 0.5 (hard)
				recs.push_back(SmoothHorizontalGradientPicture(24.0));   // frame 9: range 24 codes, A = 4/3 about black 16, Flatness ~ 0.018 < 0.02
				recs.push_back(SmoothHorizontalGradientPicture(30.0));
				recs.push_back(SmoothHorizontalGradientPicture(36.0));
				recs.push_back(SmoothHorizontalGradientPicture(42.0));
				recs.push_back(SmoothHorizontalGradientPicture(48.0));

				Grid g8, g9;
				GridOf(recs[8], &g8);
				GridOf(recs[9], &g9);
				const double flatAhead = Flatness(g9);
				const GridFadeFit refAheadFit = W1RefV4::ReferenceFitGridFadeGrid(g8.g, g9.g, kShiftRadius);
				Assert::IsTrue(flatAhead < kFlatF,
					L"precondition: smooth low-contrast gradient ahead frame has Flatness(ahead) < kFlatF (triggers ahead->flat)");
				Assert::IsTrue(refAheadFit.valid && refAheadFit.dir > 0,
					L"precondition: unskipped W1 v4 FitGridFadeGrid confirms genuine brightening fade-in (A = 4/3, b = 16)");

				CutTracker tracker;
				int cuts = 0;
				for (size_t i = 0; i < recs.size(); ++i)
				{
					const CutVerdict v = tracker.Push(static_cast<uint64_t>(i) + 1, recs[i]);
					if (v.known && v.cut)
						++cuts;
				}
				Assert::AreEqual(0, cuts,
					L"Decision identity with W1 v4: genuine smooth gradient fade-in from black must be vetoed (0 cuts) even when Flatness(ahead) < kFlatF");
			}
		}

		// D10 before/after microbenchmark harness (200 iterations): logs exact mean/p50/p99/max us and speedup ratios
		// across AlignGrid+FitGridFadeGrid, steady-state U7, SceneRunBefore-equivalent 7-pair rebuild, and CutTracker::Judge veto.
		// Strictly ZERO wall-clock pass/fail timing gates (Rule 3); asserts exact functional invariants to prevent DCE.
		TEST_METHOD(D10MicrobenchmarkLogsBeforeAfterAndVetoCost)
		{
			using namespace VpUnit::CutFadeJudge;
			using Clock = std::chrono::steady_clock;

			constexpr int kIters = 200;
			auto LogRow = [](const char* label, const BenchStats& st, double speedup = 0.0) {
				char buf[256] = {};
				if (speedup > 0.0)
				{
					std::snprintf(buf, sizeof(buf),
						"[D10-BENCH] %-42s mean=%7.2f us  p50=%7.2f us  p99=%7.2f us  max=%7.2f us  speedup=%5.2fx",
						label, st.meanUs, st.p50Us, st.p99Us, st.maxUs, speedup);
				}
				else
				{
					std::snprintf(buf, sizeof(buf),
						"[D10-BENCH] %-42s mean=%7.2f us  p50=%7.2f us  p99=%7.2f us  max=%7.2f us",
						label, st.meanUs, st.p50Us, st.p99Us, st.maxUs);
				}
				Logger::WriteMessage(buf);
			};

			// Precompute all synthetic inputs outside timed loops.
			const Record panFadeA = TexturedPanFadePicture(1.0, 0.0, 0.0, 3001u);
			const Record panFadeB = TexturedPanFadePicture(0.92, 1.3, 0.0, 3002u);
			double panFadeGA[kGridH][kGridW], panFadeGB[kGridH][kGridW];
			LoadGridCodes(panFadeA.cells, panFadeGA);
			LoadGridCodes(panFadeB.cells, panFadeGB);

			const std::vector<Record> nonFadeMono = BuildLocalDarkeningBoxSequence(20);
			double nonFadeGA[kGridH][kGridW], nonFadeGB[kGridH][kGridW];
			LoadGridCodes(nonFadeMono[6].cells, nonFadeGA);
			LoadGridCodes(nonFadeMono[13].cells, nonFadeGB);

			std::vector<Record> activePanFadeSeq;
			for (int i = 0; i < 16; ++i)
				activePanFadeSeq.push_back(TexturedPanFadePicture(1.0 - 0.045 * double(i), 0.5 * double(i), 0.0, uint32_t(4000 + i)));

			std::vector<Record> staticSeq;
			for (uint32_t i = 0; i < 16; ++i)
				staticSeq.push_back(Picture(1.0, 5000u + i));

			// 1. Frozen reference W1 v4 vs optimized AlignGrid + FitGridFadeGrid (active fade+pan & non-fade monotone worst-case).
			auto BenchFitPair = [&](const char* baseName, const double gA[kGridH][kGridW], const double gB[kGridH][kGridW],
			                        int rad, bool expectValid) {
				std::vector<double> refSamples, optSamples;
				refSamples.reserve(kIters);
				optSamples.reserve(kIters);
				int refValidCount = 0, optValidCount = 0;
				double gainAccRef = 0.0, gainAccOpt = 0.0;

				for (int it = 0; it < kIters; ++it)
				{
					const auto t0 = Clock::now();
					const GridFadeFit fRef = W1RefV4::ReferenceFitGridFadeGrid(gA, gB, rad);
					const auto t1 = Clock::now();
					refSamples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
					refValidCount += fRef.valid ? 1 : 0;
					gainAccRef += fRef.gain;

					const auto t2 = Clock::now();
					const GridFadeFit fOpt = FitGridFadeGrid(gA, gB, rad);
					const auto t3 = Clock::now();
					optSamples.push_back(std::chrono::duration<double, std::micro>(t3 - t2).count());
					optValidCount += fOpt.valid ? 1 : 0;
					gainAccOpt += fOpt.gain;
				}
				Assert::AreEqual(expectValid ? kIters : 0, refValidCount, L"reference fit validity must match expectation across all iterations");
				Assert::AreEqual(refValidCount, optValidCount, L"optimized fit validity must match reference across all iterations");
				Assert::IsTrue(std::fabs(gainAccOpt - gainAccRef) <= 1e-9 * double(kIters), L"benchmarked gains must match reference");

				const BenchStats refSt = ComputeBenchStats(refSamples);
				const BenchStats optSt = ComputeBenchStats(optSamples);
				const double speedup = optSt.meanUs > 0.0 ? refSt.meanUs / optSt.meanUs : 0.0;
				char refLabel[64] = {}, optLabel[64] = {};
				std::snprintf(refLabel, sizeof(refLabel), "%s [W1-ref-v4]", baseName);
				std::snprintf(optLabel, sizeof(optLabel), "%s [D10-opt]", baseName);
				LogRow(refLabel, refSt);
				LogRow(optLabel, optSt, speedup);
			};

			BenchFitPair("FitGridFade(fade+pan, r=2)", panFadeGA, panFadeGB, kLocalRadius, true);
			BenchFitPair("FitGridFade(nonfade-mono, r=2)", nonFadeGA, nonFadeGB, kLocalRadius, false);

			// 2. Steady-state U7 (ClassifyEvents + FoldFadeRun + FrameFadeKeep + CutTracker::Push with AlignedPairCache).
			auto BenchSteadyState = [&](const char* label, const std::vector<Record>& seq, bool expectAnyFade, bool expectZeroFades) {
				std::vector<double> samples;
				samples.reserve(kIters);
				int totalFades = 0;
				int totalCuts = 0;
				double keepChecksum = 0.0;
				for (int it = 0; it < kIters; ++it)
				{
					FadeRun run;
					CutTracker tracker;
					tracker.Push(1u, seq[0]);
					const auto t0 = Clock::now();
					for (size_t i = 1; i < seq.size(); ++i)
					{
						AlignedPairCache cache;
						const Events e = ClassifyEvents(seq[i - 1], seq[i], false, false, &cache);
						double runKeep = 1.0;
						const bool runFade = FoldFadeRun(seq[i - 1], seq[i], e, run, &runKeep, &cache);
						keepChecksum += FrameFadeKeep(seq[i - 1], seq[i], e.fade, runFade, runKeep);
						const CutVerdict cv = tracker.Push(static_cast<uint64_t>(i) + 1, seq[i]);
						if (e.fade || runFade)
							++totalFades;
						if (e.cut || (cv.known && cv.cut))
							++totalCuts;
					}
					const auto t1 = Clock::now();
					const double perFrameUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / double(seq.size() - 1);
					samples.push_back(perFrameUs);
				}
				Assert::IsTrue(keepChecksum > 0.0, L"anti-DCE keep checksum must remain positive");
				Assert::AreEqual(0, totalCuts, L"steady-state non-cut benchmark streams must emit 0 cuts");
				if (expectAnyFade)
					Assert::IsTrue(totalFades > kIters * 8, L"active pan+fade stream must reliably detect fades across all iterations");
				if (expectZeroFades)
					Assert::AreEqual(0, totalFades, L"non-fade stream must emit 0 fades across all iterations");
				LogRow(label, ComputeBenchStats(samples));
			};

			BenchSteadyState("SteadyU7/frame (active fade+pan)", activePanFadeSeq, true, false);
			BenchSteadyState("SteadyU7/frame (non-fade monotone)", nonFadeMono, false, true);
			BenchSteadyState("SteadyU7/frame (static)", staticSeq, false, true);

			// 3. SceneRunBefore-equivalent 7-pair rebuild (7x replay + 1x current) on non-fade monotone worst-case:
			// compare W1 v4 unoptimized per-pair alignment/fit cost vs optimized ClassifyEvents+FoldFadeRun with AlignedPairCache.
			{
				// Slice 9 consecutive records (indices 5..13 of local darkening box -> 7 replay pairs + 1 current pair).
				constexpr size_t kStart = 5;
				constexpr size_t kPairs = 8; // 7 replay + 1 current
				double grids[kPairs + 1][kGridH][kGridW];
				for (size_t j = 0; j <= kPairs; ++j)
					LoadGridCodes(nonFadeMono[kStart + j].cells, grids[j]);

				std::vector<double> refSamples, optSamples;
				refSamples.reserve(kIters);
				optSamples.reserve(kIters);
				int refValidTotal = 0, optFadeTotal = 0;

				for (int it = 0; it < kIters; ++it)
				{
					// W1 v4 unoptimized 7-pair replay + 1 current equivalent work:
					// per pair: ClassifyEvents FitGridFadeGrid(r=1) + FoldFadeRun step AlignGrid(r=1)
					//           + for pairs >= 2: base->cur FitGridFadeGrid(r=2) + adjacent fallback FitGridFadeGrid(r=1).
					const auto t0 = Clock::now();
					for (size_t p = 0; p < kPairs; ++p)
					{
						const GridFadeFit fCls = W1RefV4::ReferenceFitGridFadeGrid(grids[p], grids[p + 1], kShiftRadius);
						double ra[kGridH][kGridW], ca[kGridH][kGridW];
						double vx = 0.0, vy = 0.0;
						W1RefV4::ReferenceAlignGrid(grids[p], grids[p + 1], kShiftRadius, ra, ca, &vx, &vy);
						refValidTotal += fCls.valid ? 1 : 0;
						if (p >= 1)
						{
							const GridFadeFit fBase = W1RefV4::ReferenceFitGridFadeGrid(grids[0], grids[p + 1], kLocalRadius);
							refValidTotal += fBase.valid ? 1 : 0;
							if (!fBase.valid)
							{
								const GridFadeFit fAdj = W1RefV4::ReferenceFitGridFadeGrid(grids[p], grids[p + 1], kShiftRadius);
								refValidTotal += fAdj.valid ? 1 : 0;
							}
						}
					}
					const auto t1 = Clock::now();
					refSamples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());

					// Optimized SceneRunBefore 7-pair replay + 1 current with AlignedPairCache.
					const auto t2 = Clock::now();
					FadeRun run;
					AlignedPairCache cache;
					for (size_t p = 0; p < kPairs; ++p)
					{
						const Record& prevR = nonFadeMono[kStart + p];
						const Record& curR = nonFadeMono[kStart + p + 1];
						const Events e = ClassifyEvents(prevR, curR, false, false, &cache);
						const bool rf = FoldFadeRun(prevR, curR, e, run, nullptr, &cache);
						if (e.fade || rf)
							++optFadeTotal;
					}
					const auto t3 = Clock::now();
					optSamples.push_back(std::chrono::duration<double, std::micro>(t3 - t2).count());
				}
				Assert::AreEqual(0, refValidTotal, L"non-fade monotone 7-pair reference rebuild must emit 0 valid fades");
				Assert::AreEqual(0, optFadeTotal, L"non-fade monotone 7-pair optimized rebuild must emit 0 fades");

				const BenchStats refSt = ComputeBenchStats(refSamples);
				const BenchStats optSt = ComputeBenchStats(optSamples);
				const double speedup = optSt.meanUs > 0.0 ? refSt.meanUs / optSt.meanUs : 0.0;
				LogRow("SceneRunBefore 7+1 rebuild [W1-ref-v4]", refSt);
				LogRow("SceneRunBefore 7+1 rebuild [D10-opt]", optSt, speedup);
			}

			// 4. CutTracker::Judge veto candidate call cost (active candidate vs Opt D fast-skip candidate).
			{
				const Record blk = Black(6000u);
				const Record picA = TexturedPanFadePicture(0.10, 0.0, 0.0, 6001u);
				const Record picB = TexturedPanFadePicture(0.20, 0.0, 0.0, 6002u);

				std::vector<double> activeSamples, skipSamples;
				activeSamples.reserve(kIters);
				skipSamples.reserve(kIters);
				int activeCuts = 0, skipCuts = 0;

				for (int it = 0; it < kIters; ++it)
				{
					// Active veto candidate: Black -> picA -> picB (FitGridFadeGrid executes and vetoes cut).
					CutTracker tAct;
					for (uint64_t s = 1; s <= 5; ++s)
						tAct.Push(s, blk);
					tAct.Push(6u, picA);
					const auto t0 = Clock::now();
					const CutVerdict vAct = tAct.Push(7u, picB);
					const auto t1 = Clock::now();
					activeSamples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
					if (vAct.known && vAct.cut)
						++activeCuts;

					// Fast-skip candidate: Black -> picA -> identical picA (G == 0.0 triggers Opt D fast-skip, cut confirmed).
					CutTracker tSkip;
					for (uint64_t s = 1; s <= 5; ++s)
						tSkip.Push(s, blk);
					tSkip.Push(6u, picA);
					const auto t2 = Clock::now();
					const CutVerdict vSkip = tSkip.Push(7u, picA);
					const auto t3 = Clock::now();
					skipSamples.push_back(std::chrono::duration<double, std::micro>(t3 - t2).count());
					if (vSkip.known && vSkip.cut)
						++skipCuts;
				}
				Assert::AreEqual(0, activeCuts, L"active fade-in-from-black veto candidate must emit 0 cuts across all iterations");
				Assert::AreEqual(kIters, skipCuts, L"identical-repeat fast-skip candidate must confirm cut on all iterations");
				LogRow("CutTracker::Judge veto (active cand)", ComputeBenchStats(activeSamples));
				LogRow("CutTracker::Judge veto (fast-skip G=0)", ComputeBenchStats(skipSamples));
			}
		}

		// ---- U7 v5 D5: Motion-tolerant fade recall & adversarial non-fade suite ---------------------------------------------

		// D5 #1: Textured 160x90 scene + 2 contrasting foreground discs translating in opposite directions (+-2 px/frame),
		// defeating global translation alignment so rigid FitGridFadeGrid fails mid-fade while FitMotionTolerantFade succeeds.
		TEST_METHOD(SlowFadeUnderNonRigidObjectMotionIsDetected)
		{
			using namespace VpUnit::CutFadeJudge;

			auto TriangleBounce = [](double v, double lo, double hi) -> double {
				const double span = hi - lo;
				const double period = 2.0 * span;
				double m = std::fmod(v - lo, period);
				if (m < 0.0)
					m += period;
				return m <= span ? (lo + m) : (hi - (m - span));
			};

			auto NonRigidFrame = [&](int frameIdx, double gain, double panPx) -> Record {
				const double cx1 = TriangleBounce(36.0 + 2.0 * double(frameIdx), 26.0, 134.0);
				const double cx2 = TriangleBounce(124.0 - 2.0 * double(frameIdx), 26.0, 134.0);
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double sx = double(x) + panPx;
						double pat = 0.45 + 0.26 * std::cos(double(y) * 0.065) + 0.14 * std::sin(sx * 0.09);
						const double d1 = (double(x) - cx1) * (double(x) - cx1) + (double(y) - 30.0) * (double(y) - 30.0);
						const double d2 = (double(x) - cx2) * (double(x) - cx2) + (double(y) - 60.0) * (double(y) - 60.0);
						if (d1 <= 100.0)
							pat = 0.92;
						if (d2 <= 100.0)
							pat = 0.06;
						const double v = kBlack + gain * pat * 48000.0 + double(Noise(x, y, uint32_t(frameIdx))) * 0.25;
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				return MakeRecordFromHalf(half);
			};

			const int pre = 10, nOut = 60, hold = 10, nIn = 60, post = 10;
			std::vector<Record> recs;
			int idx = 0;
			for (int i = 0; i < pre; ++i)
				recs.push_back(NonRigidFrame(idx++, 1.0, 0.0));
			for (int i = 1; i <= nOut; ++i)
				recs.push_back(NonRigidFrame(idx++, 1.0 - 0.85 * double(i) / double(nOut), 0.0));
			for (int i = 0; i < hold; ++i)
				recs.push_back(NonRigidFrame(idx++, 0.15, 0.0));
			for (int i = 1; i <= nIn; ++i)
				recs.push_back(NonRigidFrame(idx++, 0.15 + 0.85 * double(i) / double(nIn), 0.0));
			for (int i = 0; i < post; ++i)
				recs.push_back(NonRigidFrame(idx++, 1.0, 0.0));

			// Mandatory In-Test Negative Control (Rule 3): prove rigid FitGridFadeGrid alone FAILS mid-fade on the exact same
			// pair (frame pre -> pre+8) due to opposite-moving foreground discs, while FitMotionTolerantFade SUCCEEDS!
			{
				double gBase[kGridH][kGridW], gMid[kGridH][kGridW];
				double ra[kGridH][kGridW], ca[kGridH][kGridW];
				double vx = 0.0, vy = 0.0;
				LoadGridCodes(recs[size_t(pre - 1)].cells, gBase);
				LoadGridCodes(recs[size_t(pre + 7)].cells, gMid);
				AlignGrid(gBase, gMid, kLocalRadius, ra, ca, &vx, &vy);
				const GridFadeFit rigidFit = FitGridFadeAligned(ra, ca, vx, vy);
				const GridFadeFit mtFit = FitMotionTolerantFade(ra, ca, vx, vy);
				Assert::IsFalse(rigidFit.valid, L"Negative control: rigid FitGridFadeGrid must fail mid-fade under opposite-moving discs");
				Assert::IsTrue(mtFit.valid && mtFit.dir < 0, L"FitMotionTolerantFade must succeed on the exact same aligned grids");
			}

			const size_t outBegin = size_t(pre);
			const size_t outEnd = outBegin + size_t(nOut);
			const size_t holdEnd = outEnd + size_t(hold);
			const size_t inEnd = holdEnd + size_t(nIn);

			FadeRun run;
			int preFades = 0, outFades = 0, holdFades = 0, inFades = 0, postFades = 0, cuts = 0;
			double fadeOutKeepProd = 1.0;
			for (size_t i = 1; i < recs.size(); ++i)
			{
				AlignedPairCache cache;
				const Events e = ClassifyEvents(recs[i - 1], recs[i], false, false, &cache);
				double rk = 1.0;
				const bool rf = FoldFadeRun(recs[i - 1], recs[i], e, run, &rk, &cache);
				const bool isFade = e.fade || rf;
				if (e.cut)
					++cuts;
				if (i < outBegin)
					preFades += isFade ? 1 : 0;
				else if (i < outEnd)
				{
					outFades += isFade ? 1 : 0;
					fadeOutKeepProd *= FrameFadeKeep(recs[i - 1], recs[i], e.fade, rf, rk);
				}
				else if (i < holdEnd)
					holdFades += isFade ? 1 : 0;
				else if (i < inEnd)
					inFades += isFade ? 1 : 0;
				else
					postFades += isFade ? 1 : 0;
			}

			Assert::AreEqual(0, cuts, L"slow fade under non-rigid object motion must emit 0 cuts");
			Assert::AreEqual(0, preFades, L"0 false fades during pre-fade non-rigid motion");
			Assert::AreEqual(0, holdFades, L"0 false fades during mid-hold non-rigid motion");
			Assert::AreEqual(0, postFades, L"0 false fades during post-fade non-rigid motion");
			Assert::IsTrue(outFades >= 50, L"60-frame fade-out under non-rigid motion must hit >= 50/60 frames");
			Assert::IsTrue(inFades >= 50, L"60-frame fade-in under non-rigid motion must hit >= 50/60 frames");
			Assert::IsTrue(fadeOutKeepProd < 0.5, L"fade-out under non-rigid motion must decay history keep < 0.5");
		}

		// D5 #2: Simultaneous horizontal camera pan (2 px/frame) + opposite-moving foreground discs + 60-frame fade-out.
		TEST_METHOD(SlowFadeUnderCameraPanAndLocalMotionIsDetected)
		{
			using namespace VpUnit::CutFadeJudge;

			auto TriangleBounce = [](double v, double lo, double hi) -> double {
				const double span = hi - lo;
				const double period = 2.0 * span;
				double m = std::fmod(v - lo, period);
				if (m < 0.0)
					m += period;
				return m <= span ? (lo + m) : (hi - (m - span));
			};

			auto PanMotionFrame = [&](int frameIdx, double gain) -> Record {
				const double panPx = 2.0 * double(frameIdx);
				const double cx1 = TriangleBounce(36.0 + 2.0 * double(frameIdx), 26.0, 134.0);
				const double cx2 = TriangleBounce(124.0 - 2.0 * double(frameIdx), 26.0, 134.0);
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double sx = double(x) + panPx;
						double pat = 0.48
							+ 0.28 * std::sin(sx * 0.18) * std::cos(double(y) * 0.14)
							+ 0.12 * std::cos(sx * 0.09 + double(y) * 0.08);
						const double d1 = (double(x) - cx1) * (double(x) - cx1) + (double(y) - 30.0) * (double(y) - 30.0);
						const double d2 = (double(x) - cx2) * (double(x) - cx2) + (double(y) - 60.0) * (double(y) - 60.0);
						if (d1 <= 100.0)
							pat = 0.94;
						if (d2 <= 100.0)
							pat = 0.04;
						const double v = kBlack + gain * pat * 48000.0 + double(Noise(x, y, uint32_t(frameIdx))) * 0.25;
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				return MakeRecordFromHalf(half);
			};

			const int pre = 10, nOut = 60, post = 10;
			std::vector<Record> recs;
			int idx = 0;
			for (int i = 0; i < pre; ++i)
				recs.push_back(PanMotionFrame(idx++, 1.0));
			for (int i = 1; i <= nOut; ++i)
				recs.push_back(PanMotionFrame(idx++, 1.0 - 0.85 * double(i) / double(nOut)));
			for (int i = 0; i < post; ++i)
				recs.push_back(PanMotionFrame(idx++, 0.15));

			// Mandatory In-Test Negative Control (Rule 3): rigid FitGridFadeGrid fails mid-fade on the exact aligned pan+motion pair
			// while FitMotionTolerantFade succeeds cleanly.
			{
				double gBase[kGridH][kGridW], gMid[kGridH][kGridW];
				double ra[kGridH][kGridW], ca[kGridH][kGridW];
				double vx = 0.0, vy = 0.0;
				LoadGridCodes(recs[size_t(pre + 5)].cells, gBase);
				LoadGridCodes(recs[size_t(pre + 10)].cells, gMid);
				AlignGrid(gBase, gMid, kLocalRadius, ra, ca, &vx, &vy);
				const GridFadeFit rigidFit = FitGridFadeAligned(ra, ca, vx, vy);
				const GridFadeFit mtFit = FitMotionTolerantFade(ra, ca, vx, vy);
				Assert::IsFalse(rigidFit.valid, L"Negative control: rigid FitGridFadeGrid must fail mid-fade under pan + local motion");
				Assert::IsTrue(mtFit.valid && mtFit.dir < 0, L"FitMotionTolerantFade must succeed on aligned pan + local motion pair");
			}

			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, size_t(pre)), L"0 false fades during pre-fade pure pan + local motion");
			Assert::IsTrue(Count(res, size_t(pre), size_t(pre + nOut)) >= 48, L"60-frame fade-out under pan + local motion must hit >= 48/60");
			Assert::AreEqual(0, Count(res, size_t(pre + nOut), recs.size()), L"0 false fades during post-fade pure pan + local motion");
		}

		// D5 #3: 60-frame slow fade-out through near-black histogram quantization (e.cut == true while spread > 1 code & non-flat)
		// -> 12-frame black hold -> 60-frame slow fade-in, with static dim-to-near-black hard cut negative control.
		TEST_METHOD(SlowFadeIntoAndOutOfBlackAcrossNearBlackQuantization)
		{
			using namespace VpUnit::CutFadeJudge;

			auto HighTexPicture = [](double gain) -> Record {
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double pat = 0.50 + 0.45 * std::sin(double(x) * 0.42) * std::cos(double(y) * 0.42);
						const double v = kBlack + gain * pat * 48000.0;
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				return MakeRecordFromHalf(half);
			};

			const int pre = 8, nOut = 60, holdBlack = 12, nIn = 60, post = 8;
			std::vector<Record> recs;
			for (int i = 0; i < pre; ++i)
				recs.push_back(HighTexPicture(1.0));
			for (int i = 1; i <= nOut; ++i)
				recs.push_back(i == nOut ? ConstantFlatPicture(16.0) : HighTexPicture(1.0 - double(i) / double(nOut)));
			for (int i = 0; i < holdBlack; ++i)
				recs.push_back(ConstantFlatPicture(16.0));
			for (int i = 1; i <= nIn; ++i)
				recs.push_back(HighTexPicture(double(i) / double(nIn)));
			for (int i = 0; i < post; ++i)
				recs.push_back(HighTexPicture(1.0));

			const size_t outBegin = size_t(pre);
			const size_t outEnd = outBegin + size_t(nOut);
			const size_t blackEnd = outEnd + size_t(holdBlack);
			const size_t inEnd = blackEnd + size_t(nIn);

			FadeRun run;
			int outFades = 0, blackHoldFades = 0, inFades = 0, prePostFades = 0;
			int nearBlackNonFlatCutCaught = 0;
			for (size_t i = 1; i < recs.size(); ++i)
			{
				const Events e = ClassifyEvents(recs[i - 1], recs[i], false, false);
				const bool rf = FoldFadeRun(recs[i - 1], recs[i], e, run);
				const bool isFade = e.fade || rf;
				if (i < outBegin || i >= inEnd)
					prePostFades += isFade ? 1 : 0;
				else if (i < outEnd)
				{
					outFades += isFade ? 1 : 0;
					const uint32_t spCur = recs[i].p99 > recs[i].p01 ? recs[i].p99 - recs[i].p01 : 0u;
					Grid gc;
					GridOf(recs[i], &gc);
					if (e.cut && spCur > kBlackSpreadP4 && Flatness(gc) >= kFlatF && rf)
						++nearBlackNonFlatCutCaught;
				}
				else if (i < blackEnd)
					blackHoldFades += isFade ? 1 : 0;
				else
					inFades += isFade ? 1 : 0;
			}

			Assert::AreEqual(0, prePostFades, L"0 false fades on static pre/post frames");
			Assert::AreEqual(0, blackHoldFades, L"0 false fades during black hold");
			Assert::IsTrue(nearBlackNonFlatCutCaught >= 1,
				L"D5 near-black fade-out e.cut symmetry must catch at least one e.cut && spread > 1 code && non-flat fade-out frame");
			Assert::IsTrue(outFades >= 56, L"60-frame fade-out through near-black quantization must hit >= 56/60");
			Assert::IsTrue(inFades >= 56, L"60-frame fade-in from black must hit >= 56/60");

			// Mandatory In-Test Negative Control (Rule 3): a static dim shot (activeFade == false) hard-cutting to a darker
			// near-black textured frame (exact same e.cut && spread > kBlackSpreadP4 shape!) MUST NOT produce runFade == true!
			{
				std::vector<Record> hardDim;
				for (int i = 0; i < 6; ++i)
					hardDim.push_back(HighTexPicture(2.0 / 60.0));
				for (int i = 0; i < 6; ++i)
					hardDim.push_back(HighTexPicture(1.0 / 60.0));
				FadeRun negRun;
				bool sawNearBlackCut = false;
				for (size_t i = 1; i < hardDim.size(); ++i)
				{
					const Events e = ClassifyEvents(hardDim[i - 1], hardDim[i], false, false);
					const bool rf = FoldFadeRun(hardDim[i - 1], hardDim[i], e, negRun);
					if (i == 6)
					{
						const uint32_t spCur = hardDim[i].p99 > hardDim[i].p01 ? hardDim[i].p99 - hardDim[i].p01 : 0u;
						Assert::IsTrue(e.cut && spCur > kBlackSpreadP4,
							L"Negative control premise: static dim-to-near-black step hits e.cut with spread > kBlackSpreadP4");
						Assert::IsFalse(rf, L"Negative control: without activeFade, near-black e.cut must NOT emit runFade");
						sawNearBlackCut = true;
					}
				}
				Assert::IsTrue(sawNearBlackCut);
			}
		}

		// D5 #4: 200-frame near-unity-slope fade (0.35%/frame, single-pair |1 - A| <= 0.012 < 0.02):
		// negative control proves single-pair ClassifyEvents fires 0/200 while FoldFadeRun hits >= 188/200 once cumulative gain > 2%.
		TEST_METHOD(NearUnitySlopeFadeCrossingTwoPercentCumulativeGain)
		{
			const int pre = 10, n = 200, post = 10;
			std::vector<Record> recs;
			uint32_t k = 0;
			for (int i = 0; i < pre; ++i)
				recs.push_back(Picture(1.0, k++));
			for (int i = 1; i <= n; ++i)
				recs.push_back(Picture(1.0 - 0.0035 * double(i), k++));
			for (int i = 0; i < post; ++i)
				recs.push_back(Picture(1.0 - 0.0035 * double(n), k++));

			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, size_t(pre)), L"0 fades on pre-fade static frames");
			Assert::AreEqual(0, Count(res, size_t(pre + n), recs.size()), L"0 fades on post-fade static frames");
			// Mandatory In-Test Negative Control (Rule 3): single-pair law alone misses 100% of near-unity-slope steps!
			Assert::AreEqual(0, Count(res, size_t(pre), size_t(pre + n), true),
				L"Negative control: single-pair ClassifyEvents (e.fade) must fire on 0/200 frames when single-pair |1-A| < 0.02");
			Assert::IsTrue(Count(res, size_t(pre), size_t(pre + n)) >= 188,
				L"multi-frame FoldFadeRun must detect >= 188/200 frames once cumulative gain crosses 2%");
		}

		// D5 #5: Short 5-frame fade-out (1.0 -> 0.50) and 5-frame fade-in (0.50 -> 1.0).
		TEST_METHOD(ShortFiveFrameFadeOutAndFadeInAreDetected)
		{
			const int pre = 8, n = 5, post = 8;
			// Fade-out 1.0 -> 0.50
			{
				const auto res = Play(FadeSequence(n, pre, post, 0.50));
				Assert::AreEqual(0, Count(res, 1, size_t(pre)), L"0 fades before 5-frame fade-out");
				Assert::IsTrue(Count(res, size_t(pre), size_t(pre + n)) >= 4, L"5-frame fade-out must hit >= 4/5 frames");
				Assert::AreEqual(0, Count(res, size_t(pre + n), res.size()), L"0 fades after 5-frame fade-out");
			}
			// Fade-in 0.50 -> 1.0
			{
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < pre; ++i)
					recs.push_back(Picture(0.50, k++));
				for (int i = 1; i <= n; ++i)
					recs.push_back(Picture(0.50 + 0.50 * double(i) / double(n), k++));
				for (int i = 0; i < post; ++i)
					recs.push_back(Picture(1.0, k++));
				const auto res = Play(recs);
				Assert::AreEqual(0, Count(res, 1, size_t(pre)), L"0 fades before 5-frame fade-in");
				Assert::IsTrue(Count(res, size_t(pre), size_t(pre + n)) >= 4, L"5-frame fade-in must hit >= 4/5 frames");
				Assert::AreEqual(0, Count(res, size_t(pre + n), res.size()), L"0 fades after 5-frame fade-in");
			}
		}

		// D5 #6: A large dark foreground object (30% of picture) translating into a bright textured room drops mean monotonically
		// over >= 10 frames (negative control) yet must emit 0 fades.
		TEST_METHOD(ALargeForegroundObjectCrossingStaticSceneIsNotAFade)
		{
			std::vector<Record> recs;
			for (int i = 0; i < 26; ++i)
			{
				// Dark occluder slides in from right edge across frames 4..20, reaching 30% width (48 px).
				const double frac = i < 4 ? 0.0 : (i <= 20 ? double(i - 3) / 17.0 : 1.0);
				const uint32_t occX0 = kW - static_cast<uint32_t>(std::floor(frac * double(kW) * 0.30 + 0.5));
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double room = 0.35 + 0.45 * double(x) / double(kW) + 0.15 * std::sin(double(y) * 0.15);
						const bool occluded = (x >= occX0);
						const double v = occluded ? (kBlack + double(Noise(x, y, uint32_t(i))) * 0.25)
							: (kBlack + room * 48000.0 + double(Noise(x, y, uint32_t(i))));
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				recs.push_back(MakeRecordFromHalf(half));
			}

			// Mandatory In-Test Negative Control (Rule 3): verify frame mean drops monotonically over >= 10 frames
			// AND unguarded AlignGrid pulls alignment off (0, 0) (|vx| + |vy| > 0.5) on a crossing pair,
			// proving the stationary-background shift guard + spatial coverage reject a genuine hazard!
			int monoDropSteps = 0;
			for (size_t i = 5; i <= 20; ++i)
				if (recs[i].mean < recs[i - 1].mean)
					++monoDropSteps;
			Assert::IsTrue(monoDropSteps >= 12, L"Negative control: occluder must cause >= 12 monotonic mean-drop frames");
			{
				using namespace VpUnit::CutFadeJudge;
				double gA[kGridH][kGridW], gB[kGridH][kGridW], ra[kGridH][kGridW], ca[kGridH][kGridW];
				double vx = 0.0, vy = 0.0;
				LoadGridCodes(recs[4].cells, gA);
				LoadGridCodes(recs[12].cells, gB);
				AlignGrid(gA, gB, kLocalRadius, ra, ca, &vx, &vy);
				Assert::IsTrue(std::fabs(vx) + std::fabs(vy) > 0.5,
					L"Negative control: unguarded AlignGrid must shift off (0, 0) on crossing occluder pair");
			}

			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, recs.size()), L"30% foreground object crossing static room must emit 0 fades");
		}

		// D5 #7: Animated bright bars/shards expanding smoothly over a black background raise mean monotonically (negative control)
		// yet must emit 0 fades.
		TEST_METHOD(AnimatedBrightShardsSweepingIntoBlackAreNotAFade)
		{
			std::vector<Record> recs;
			for (int i = 0; i < 26; ++i)
			{
				const double frac = i < 4 ? 0.0 : (i <= 20 ? double(i - 3) / 17.0 : 1.0);
				const double sweepX = frac * double(kW) * 0.45;
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const bool inBar = (double(x) < sweepX) && ((y % 18u) < 9u);
						const double v = inBar ? (50000.0 + double(Noise(x, y, uint32_t(i))) * 0.5)
							: (kBlack + double(Noise(x, y, uint32_t(i))) * 0.125);
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				recs.push_back(MakeRecordFromHalf(half));
			}

			// Mandatory In-Test Negative Control (Rule 3): verify frame mean rises monotonically over >= 10 frames
			// AND unguarded AlignGrid shifts off (0, 0) while p99 stays pinned despite large mean growth!
			int monoRiseSteps = 0;
			for (size_t i = 5; i <= 20; ++i)
				if (recs[i].mean > recs[i - 1].mean)
					++monoRiseSteps;
			Assert::IsTrue(monoRiseSteps >= 12, L"Negative control: expanding bright shards must cause >= 12 monotonic mean-rise frames");
			{
				using namespace VpUnit::CutFadeJudge;
				double gA[kGridH][kGridW], gB[kGridH][kGridW], ra[kGridH][kGridW], ca[kGridH][kGridW];
				double vx = 0.0, vy = 0.0;
				LoadGridCodes(recs[5].cells, gA);
				LoadGridCodes(recs[10].cells, gB);
				AlignGrid(gA, gB, kLocalRadius, ra, ca, &vx, &vy);
				Assert::IsTrue(std::fabs(vx) + std::fabs(vy) > 0.5,
					L"Negative control: unguarded AlignGrid must shift off (0, 0) on expanding bright shards");
				Assert::IsTrue(double(recs[18].mean) > 1.3 * double(recs[5].mean) &&
					std::fabs(double(recs[18].p99) - double(recs[5].p99)) < 2.0 * 256.0,
					L"Negative control: sweeping shards raise mean >30% while p99 stays pinned");
			}

			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, recs.size()), L"animated bright shards sweeping into black must emit 0 fades");
		}

		// D5 #8: Centered graphic on black shrinking and darkening smoothly drops BOTH mean and p99 monotonically (negative control)
		// yet must emit 0 fades (localized center graphic is not a whole-picture fade).
		TEST_METHOD(ZoomingDarkeningCenterGraphicOnBlackIsNotAFade)
		{
			std::vector<Record> recs;
			for (int i = 0; i < 24; ++i)
			{
				const double t = i < 4 ? 0.0 : (i <= 18 ? double(i - 3) / 15.0 : 1.0);
				const double halfW = 24.0 * (1.0 - 0.55 * t);
				const double halfH = 16.0 * (1.0 - 0.55 * t);
				const double gain = 1.0 - 0.65 * t;
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double dx = std::fabs(double(x) - 0.5 * double(kW));
						const double dy = std::fabs(double(y) - 0.5 * double(kH));
						double v = kBlack;
						if (dx <= halfW && dy <= halfH)
						{
							const double art = 0.40 + 0.55 * (1.0 - 0.5 * (dx / (halfW + 1e-6) + dy / (halfH + 1e-6)));
							v = kBlack + gain * art * 48000.0 + double(Noise(x, y, uint32_t(i))) * 0.25;
						}
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				recs.push_back(MakeRecordFromHalf(half));
			}

			// Mandatory In-Test Negative Control (Rule 3): verify BOTH mean and p99 drop monotonically over >= 10 frames
			// AND raw FitGridFadeGrid (WITHOUT FootprintPreserved!) returns valid == true on mid-shrink pair recs[3] -> recs[8]!
			int monoMeanAndP99Drops = 0;
			for (size_t i = 5; i <= 18; ++i)
				if (recs[i].mean < recs[i - 1].mean && recs[i].p99 < recs[i - 1].p99)
					++monoMeanAndP99Drops;
			Assert::IsTrue(monoMeanAndP99Drops >= 10,
				L"Negative control: zooming/darkening center graphic must monotonically drop both mean and p99");
			{
				using namespace VpUnit::CutFadeJudge;
				const GridFadeFit rawFit = FitGridFade(recs[3].cells, recs[8].cells, kLocalRadius);
				Assert::IsTrue(rawFit.valid && rawFit.dir < 0,
					L"Negative control: raw FitGridFadeGrid without FootprintPreserved falsely accepts mid-shrink center graphic");
			}

			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, recs.size()), L"zooming darkening center graphic on black must emit 0 fades");
		}

		// ---- U7 v5 D4: Standalone symmetric noise-model proof (100% independent of real clips / D8) -------------------------

		// D4 #9: Verifies StepStillMoving's Neyman-Pearson strict above-LSB majority (sSameEps > sOppEps) against exact
		// Gaussian tail probability theory across drift mu in {-0.15, 0.0, +0.10, +0.15, +0.25} codes/frame (sigma = 0.5,
		// 100 trials each), proves a 0.75 threshold fails >50% of genuine slow fades while >0.5 passes >=95%, verifies
		// reversal rejection (100% in FoldFadeRun), and verifies exact 50/50 tie rejection (sSameEps == sOppEps == 2).
		TEST_METHOD(StepStillMovingAboveLsbMajorityFollowsSymmetricNoiseModel)
		{
			using namespace VpUnit::CutFadeJudge;

			constexpr int R = kLocalRadius;
			constexpr int kIntN = (kGridH - 2 * R) * (kGridW - 2 * R);
			constexpr double kSigma = 0.5;
			constexpr double kEps = kFadeCellStepEps;   // 0.5 code
			constexpr int kTrials = 100;
			const double kInvSqrt2 = 1.0 / std::sqrt(2.0);

			auto TailProb = [&](double z) -> double {
				return 0.5 * std::erfc(z * kInvSqrt2);
			};

			// Build a deterministic synthetic Record directly from a 32x18 cell grid (exact P4-code scale).
			auto MakeGridRecord = [](const double cells[kGridH][kGridW]) -> Record {
				Record r;
				r.gridFound = 1;
				r.pixelCount = kW * kH;
				double sum = 0.0;
				std::vector<uint32_t> vals;
				vals.reserve(size_t(kGridW * kGridH));
				for (int y = 0; y < kGridH; ++y)
					for (int x = 0; x < kGridW; ++x)
					{
						const uint32_t p4 = static_cast<uint32_t>((std::min)(65535.0, (std::max)(0.0, std::floor(cells[y][x] * 256.0 + 0.5))));
						r.cells[y * kGridW + x] = p4;
						sum += double(p4);
						vals.push_back(p4);
						const uint32_t bin = (std::min)(VpUnit::SourceStats::kHistBins - 1u, p4 / kP4HistBinWidth);
						r.hist[bin] += 25u;
					}
				std::sort(vals.begin(), vals.end());
				r.mean = static_cast<uint32_t>(std::floor(sum / double(kGridW * kGridH) + 0.5));
				r.p01 = vals[vals.size() / 100];
				r.p50 = vals[vals.size() / 2];
				r.p99 = vals[(vals.size() * 99) / 100];
				return r;
			};

			std::mt19937 rng(20261123u);
			std::normal_distribution<double> noiseDist(0.0, kSigma);

			// 1. Empirical Gaussian tail geometry across mu in {-0.15, 0.0, +0.10, +0.15, +0.25}.
			const double mus[] = { -0.15, 0.0, +0.10, +0.15, +0.25 };
			for (double mu : mus)
			{
				const double pPlus = TailProb((kEps - mu) / kSigma);
				const double pMinus = TailProb((kEps + mu) / kSigma);
				const double qTheory = pPlus / (pPlus + pMinus);

				double qSum = 0.0;
				int pass75 = 0, passStrictMaj = 0;
				for (int t = 0; t < kTrials; ++t)
				{
					int sSame = 0, sOpp = 0;
					for (int i = 0; i < kIntN; ++i)
					{
						const double delta = mu + noiseDist(rng);
						if (delta > kEps)
							++sSame;
						else if (delta < -kEps)
							++sOpp;
					}
					const int m = sSame + sOpp;
					Assert::IsTrue(m > 0, L"non-empty tail sample");
					const double q = double(sSame) / double(m);
					qSum += q;
					if (double(sSame) >= kFadeSpatialConsensus * double(m))
						++pass75;
					if (sSame > sOpp)
						++passStrictMaj;
				}
				const double qEmp = qSum / double(kTrials);
				Assert::IsTrue(std::fabs(qEmp - qTheory) <= 0.05,
					L"empirical above-LSB tail fraction must match normal-tail analytical prediction within +-0.05");

				if (mu == +0.10 || mu == +0.15)
				{
					Assert::IsTrue(qEmp > 0.55 && qEmp < 0.74,
						L"for mu in {+0.10, +0.15} with sigma=0.5, empirical tail fraction lies strictly in (0.55, 0.74)");
					// Mandatory In-Test Negative Control (Rule 3): naive 0.75 threshold rejects > 50% of genuine slow fades,
					// whereas Neyman-Pearson strict majority (sSame > sOpp) passes >= 95%!
					Assert::IsTrue(pass75 < 50,
						L"Negative control: naive 0.75 threshold must reject > 50% of genuine slow-fade trials at mu=0.10..0.15");
					Assert::IsTrue(passStrictMaj >= 95,
						L"Neyman-Pearson strict majority (sSame > sOpp) must pass >= 95% of genuine slow-fade trials");
				}
				else if (mu == -0.15)
				{
					Assert::IsTrue(passStrictMaj <= 5,
						L"reversal mu=-0.15 must be rejected by strict majority (sSame > sOpp) on >= 95% of trials");
				}
			}

			// 2. Real FoldFadeRun reversal rejection (mu = -0.15 on an active brightening run -> 100% rejected across 100 trials)
			//    AND exact 50/50 above-LSB tie (sSameEps == sOppEps == 2) rejection!
			double baseGrid[kGridH][kGridW], prevGrid[kGridH][kGridW];
			for (int y = 0; y < kGridH; ++y)
				for (int x = 0; x < kGridW; ++x)
				{
					const double pat = 20.0 + 100.0 * double(x) / double(kGridW - 1) + 20.0 * std::sin(double(y) * 0.5);
					baseGrid[y][x] = 16.0 + 0.80 * pat;
					prevGrid[y][x] = 16.0 + 0.90 * pat;   // +12.5% cumulative brightening from base
				}
			double midGrid[kGridH][kGridW];
			for (int y = 0; y < kGridH; ++y)
				for (int x = 0; x < kGridW; ++x)
					midGrid[y][x] = 0.5 * (baseGrid[y][x] + prevGrid[y][x]);

			const Record rBase = MakeGridRecord(baseGrid);
			const Record rMid = MakeGridRecord(midGrid);
			const Record rPrev = MakeGridRecord(prevGrid);

			auto PrimeActiveBrighteningRun = [&]() -> FadeRun {
				FadeRun r;
				const Events e1 = ClassifyEvents(rBase, rMid, false, false);
				FoldFadeRun(rBase, rMid, e1, r);
				const Events e2 = ClassifyEvents(rMid, rPrev, false, false);
				const bool active = FoldFadeRun(rMid, rPrev, e2, r);
				Assert::IsTrue(active && r.direction > 0, L"precondition: primed active brightening fade run");
				return r;
			};

			// 2a. Reversal trials (mu = -0.15 codes + N(0, 0.5) noise): FoldFadeRun must reject 100/100!
			int revAccepted = 0;
			for (int t = 0; t < kTrials; ++t)
			{
				double revGrid[kGridH][kGridW];
				for (int y = 0; y < kGridH; ++y)
					for (int x = 0; x < kGridW; ++x)
						revGrid[y][x] = prevGrid[y][x] - 0.15 + noiseDist(rng);
				const Record rRev = MakeGridRecord(revGrid);
				FadeRun run = PrimeActiveBrighteningRun();
				const Events e = ClassifyEvents(rPrev, rRev, false, false);
				if (FoldFadeRun(rPrev, rRev, e, run))
					++revAccepted;
			}
			Assert::AreEqual(0, revAccepted, L"FoldFadeRun must reject 100% of mu=-0.15 reversal trials");

			// 2b. Exact 50/50 above-LSB tie (sSameEps == 2, sOppEps == 2) on a slightly expanding plateau:
			//     Isolates strict (sSameEps > sOppEps) vs old (>= 0.5 * (sSameEps + sOppEps)):
			//     dMean > 0, stepA_Q > 1.0, sSameZero >> sOppZero, cumulative fit still valid with k < run.keep,
			//     yet sSameEps == sOppEps == 2 -> FoldFadeRun MUST return false!
			{
				double tieGrid[kGridH][kGridW];
				for (int y = 0; y < kGridH; ++y)
					for (int x = 0; x < kGridW; ++x)
						tieGrid[y][x] = 16.0 + 1.003 * (prevGrid[y][x] - 16.0);   // tiny +0.3% expansion (< 0.45 code everywhere)
				// Inject exact 2 cells > +0.5 code and 2 cells < -0.5 code inside interior [R..H-R)x[R..W-R):
				tieGrid[R + 2][R + 4] = prevGrid[R + 2][R + 4] + 0.8;
				tieGrid[R + 3][R + 5] = prevGrid[R + 3][R + 5] + 0.8;
				tieGrid[R + 4][R + 6] = prevGrid[R + 4][R + 6] - 0.8;
				tieGrid[R + 5][R + 7] = prevGrid[R + 5][R + 7] - 0.8;

				const Record rTie = MakeGridRecord(tieGrid);
				// Verify exact tie precondition directly on the encoded P4 grid:
				int tieSame = 0, tieOpp = 0;
				for (int y = R; y < kGridH - R; ++y)
					for (int x = R; x < kGridW - R; ++x)
					{
						const double delta = (double(rTie.cells[y * kGridW + x]) - double(rPrev.cells[y * kGridW + x])) * kGridScale;
						if (delta > kEps)
							++tieSame;
						else if (delta < -kEps)
							++tieOpp;
					}
				Assert::AreEqual(2, tieSame, L"precondition: exact 2 above-LSB brightening cells");
				Assert::AreEqual(2, tieOpp, L"precondition: exact 2 above-LSB darkening cells");
				// Negative control: old >= 0.5 threshold would accept this 2/4 tie!
				Assert::IsTrue(double(tieSame) >= kFadeSpatialCoverage * double(tieSame + tieOpp),
					L"Negative control: old >= 0.5 threshold would have accepted exact 2-vs-2 tie");

				FadeRun run = PrimeActiveBrighteningRun();
				const Events eTie = ClassifyEvents(rPrev, rTie, false, false);
				Assert::IsFalse(FoldFadeRun(rPrev, rTie, eTie, run),
					L"D4 strict Bayes majority (sSameEps > sOppEps) must reject exact 50/50 above-LSB tie");
			}
		}

		// ---- U7 v5 D8: False-fade counter-example suite ---------------------------------------------------------------------

		// D8 #10: Dark interior ramp + pinned bright practical lamp (~2.5% pixels at p99 = 235 codes); midtones oscillate +-4%
		// over 3-5 frames. Negative control proves rigid FitGridFadeGrid alone returns valid == true while HighlightScalingConsistent
		// rejects it, emitting 0 fades across the entire sequence.
		TEST_METHOD(AmbientFillFlickerWithPinnedHighlightsIsNotAFade)
		{
			using namespace VpUnit::CutFadeJudge;

			auto InteriorFlickerFrame = [](double midGain, uint32_t k) -> Record {
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						// Practical lamp pinned at 235 codes in border strip (10 x 36 = 360 px = 2.5% of 14400 px -> pins p99 = 235)
						// while leaving the entire 28x14 interior grid purely midtone walls.
						if (x < 10u && y < 36u)
						{
							half[size_t(y) * kW + x] = static_cast<uint16_t>(235u * 256u);
						}
						else
						{
							const double base = 8192.0 + 18000.0 * double(x) / double(kW - 1) + 4000.0 * std::sin(double(y) * 0.14);
							const double v = kBlack + midGain * (base - kBlack) + double(Noise(x, y, k)) * 0.25;
							half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
						}
					}
				return MakeRecordFromHalf(half);
			};

			// 4 static frames, then 4 frames rising +4%/frame, then 4 frames falling -4%/frame, then 4 frames rising +4%/frame.
			const double gains[] = {
				1.00, 1.00, 1.00, 1.00,
				1.04, 1.08, 1.12, 1.16,
				1.12, 1.08, 1.04, 1.00,
				1.04, 1.08, 1.12, 1.16,
				1.16, 1.16, 1.16, 1.16
			};
			std::vector<Record> recs;
			uint32_t k = 0;
			for (double g : gains)
				recs.push_back(InteriorFlickerFrame(g, k++));

			// Mandatory In-Test Negative Control (Rule 3): prove rigid FitGridFadeGrid alone (WITHOUT HighlightScalingConsistent)
			// returns valid == true on the flicker ramp (index 3 -> index 7, +16% midtone rise), while HighlightScalingConsistent
			// rejects it because p99 is pinned at 235 codes!
			{
				const GridFadeFit rawFit = FitGridFade(recs[3].cells, recs[7].cells, kLocalRadius);
				Assert::IsTrue(rawFit.valid && rawFit.dir > 0,
					L"Negative control: rigid FitGridFadeGrid alone must return valid on midtone flicker ramp");
				Assert::IsFalse(HighlightScalingConsistent(recs[3].p99, recs[7].p99, rawFit.gain, rawFit.black, rawFit.dir),
					L"HighlightScalingConsistent must reject midtone flicker with pinned p99 highlight");
			}

			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, recs.size()),
				L"ambient fill flicker with pinned highlights must emit 0 fades");
		}

		// D8 #11: Textured shot drops spread by 40% in 1 frame (aMean < spread/4 while aSpread >= spread/4) -> 0 fades and run reset.
		TEST_METHOD(AbruptFortyPercentSpreadDipIsNotAFade)
		{
			using namespace VpUnit::CutFadeJudge;

			auto MakeSpreadFrame = [](double spreadScale, double meanOffsetCodes, uint32_t k) -> Record {
				constexpr double kCenter = 32192.0;
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double base = 8192.0 + double(x) * 48000.0 / double(kW - 1);
						const double v = kCenter + meanOffsetCodes * 256.0 + spreadScale * (base - kCenter)
							+ double(Noise(x, y, k)) * 0.25;
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				return MakeRecordFromHalf(half);
			};

			// Case A: abrupt 40% spread dip from static shot (0 fades, run does not seed).
			// Case B: abrupt 40% spread dip mid-fade-out (0 fades on dip frame, active run resets to direction == 0, pairs == 0).
			std::vector<Record> recs;
			uint32_t k = 0;
			for (int i = 0; i < 6; ++i)
				recs.push_back(Picture(1.0, k++));
			// Active fade-out for 6 frames (1.0 -> 0.85) so a real FadeRun is active before the abrupt 40% spread collapse.
			for (int i = 1; i <= 6; ++i)
				recs.push_back(Picture(1.0 - 0.025 * double(i), k++));
			// Index 12: abrupt 40% spread dip (scale 0.85 -> 0.50 -> ~41% spread drop) keeping mean shift small (< spread / 4).
			const double prevMeanCodes = double(recs.back().mean) / 256.0;
			recs.push_back(MakeSpreadFrame(0.50, prevMeanCodes - (32192.0 / 256.0) - 2.0, k++));
			for (int i = 0; i < 6; ++i)
				recs.push_back(MakeSpreadFrame(0.50, prevMeanCodes - (32192.0 / 256.0) - 2.0, k++));

			// Mandatory In-Test Negative Control (Rule 3): prove index 11 -> 12 satisfies aMean < spread/4 (would pass
			// old mean-only jump check!) while tripping aSpread >= spread/4!
			const Record& prevR = recs[11];
			const Record& dipR = recs[12];
			const uint32_t spPrev = prevR.p99 > prevR.p01 ? prevR.p99 - prevR.p01 : 0u;
			const uint32_t spDip = dipR.p99 > dipR.p01 ? dipR.p99 - dipR.p01 : 0u;
			const int64_t sp = int64_t((std::max)(spPrev, spDip));
			const int64_t dMeanDip = int64_t(dipR.mean) - int64_t(prevR.mean);
			const int64_t aMean = dMeanDip < 0 ? -dMeanDip : dMeanDip;
			const int64_t dSpreadDip = int64_t(spDip) - int64_t(spPrev);
			const int64_t aSpread = dSpreadDip < 0 ? -dSpreadDip : dSpreadDip;
			Assert::IsTrue(aMean * kFadeMaxDiv < sp,
				L"Negative control: abrupt spread dip keeps aMean < spread/4 (would bypass mean-only jump check)");
			Assert::IsTrue(aSpread * kFadeMaxDiv >= sp,
				L"Negative control: abrupt 40% spread dip trips aSpread >= spread/4");

			FadeRun run;
			bool activeBeforeDip = false;
			for (size_t i = 1; i < recs.size(); ++i)
			{
				const Events e = ClassifyEvents(recs[i - 1], recs[i], false, false);
				const bool rf = FoldFadeRun(recs[i - 1], recs[i], e, run);
				if (i == 11)
					activeBeforeDip = e.fade || rf;
				if (i == 12)
				{
					Assert::IsFalse(e.fade, L"abrupt 40% spread dip must not be a pair fade");
					Assert::IsFalse(rf, L"abrupt 40% spread dip must not be a run fade");
					Assert::AreEqual(0, run.direction, L"abrupt 40% spread dip must reset run direction");
					Assert::AreEqual(0u, run.pairs, L"abrupt 40% spread dip must reset run pairs");
				}
				if (i > 12)
					Assert::IsFalse(e.fade || rf, L"post-dip static frames must emit 0 fades");
			}
			Assert::IsTrue(activeBeforeDip, L"precondition: fade-out run was active immediately before the abrupt spread dip");
		}

		// D8 #12: 48-frame fade-out & fade-in followed by 20 noisy static AND panning plateau frames -> 0 post-fade tail frames!
		// Negative control proves cumulative FitGridFade(base -> firstPlateau) is STILL valid == true (would emit tail fades
		// without strict k < run.keep + stepA_Q termination).
		TEST_METHOD(PostFadePlateauEmitsZeroTailFades)
		{
			using namespace VpUnit::CutFadeJudge;

			const int pre = 8, n = 48, post = 20;

			// 1. 48-frame fade-out -> 20 noisy static plateau frames.
			{
				const std::vector<Record> recs = FadeSequence(n, pre, post, 0.25);
				// Negative control: cumulative FitGridFade from fade start (pre - 1) to first plateau frame (pre + n) is STILL valid!
				const GridFadeFit cumFit = FitGridFade(recs[size_t(pre - 1)].cells, recs[size_t(pre + n)].cells, kLocalRadius);
				Assert::IsTrue(cumFit.valid && cumFit.dir < 0,
					L"Negative control: cumulative FitGridFade(base -> firstPlateau) is still valid, proving strict termination is required");

				const auto res = Play(recs);
				Assert::AreEqual(0, Count(res, 1, size_t(pre)), L"0 pre-fade fades");
				Assert::IsTrue(Count(res, size_t(pre), size_t(pre + n)) >= 45, L"48-frame fade-out detected");
				Assert::AreEqual(0, Count(res, size_t(pre + n), recs.size()),
					L"D8 strict termination: static plateau after fade-out must emit 0 tail fades");
			}

			// 2. 48-frame fade-in -> 20 noisy static plateau frames.
			{
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < pre; ++i)
					recs.push_back(Picture(0.25, k++));
				for (int i = 1; i <= n; ++i)
					recs.push_back(Picture(0.25 + 0.75 * double(i) / double(n), k++));
				for (int i = 0; i < post; ++i)
					recs.push_back(Picture(1.0, k++));

				const GridFadeFit cumFit = FitGridFade(recs[size_t(pre - 1)].cells, recs[size_t(pre + n)].cells, kLocalRadius);
				Assert::IsTrue(cumFit.valid && cumFit.dir > 0,
					L"Negative control: cumulative FitGridFade(base -> firstPlateau) is still valid after fade-in");

				const auto res = Play(recs);
				Assert::AreEqual(0, Count(res, 1, size_t(pre)), L"0 pre-fade-in fades");
				Assert::IsTrue(Count(res, size_t(pre), size_t(pre + n)) >= 45, L"48-frame fade-in detected");
				Assert::AreEqual(0, Count(res, size_t(pre + n), recs.size()),
					L"D8 strict termination: static plateau after fade-in must emit 0 tail fades");
			}

			// 3. 48-frame pan+fade-out -> 20 noisy panning plateau frames.
			{
				std::vector<Record> recs;
				for (int i = 0; i < pre + n + post; ++i)
				{
					const double gain = i < pre ? 1.0 : (i < pre + n ? 1.0 - 0.75 * double(i - pre + 1) / double(n) : 0.25);
					recs.push_back(TexturedPanFadePicture(gain, 0.4 * double(i), 0.0, uint32_t(7000 + i)));
				}
				const auto res = Play(recs);
				Assert::AreEqual(0, Count(res, 1, size_t(pre)), L"0 pre-fade pan fades");
				Assert::IsTrue(Count(res, size_t(pre), size_t(pre + n)) >= 44, L"48-frame pan+fade-out detected");
				Assert::AreEqual(0, Count(res, size_t(pre + n), recs.size()),
					L"D8 strict termination: panning plateau after pan+fade-out must emit 0 tail fades");
			}
		}

		// D8 #13: Active fade-out followed by a frame where mean still drops -0.2 code while spread rebounds +3 codes
		// -> terminates immediately (0 fades on rebound frame). Negative control proves dMean < 0 AND cumulative FitGridFade
		// (base -> rebound) is still valid == true.
		TEST_METHOD(FadeOutFollowedBySpreadReversalTerminatesImmediately)
		{
			using namespace VpUnit::CutFadeJudge;

			std::vector<Record> recs;
			uint32_t k = 0;
			for (int i = 0; i < 6; ++i)
				recs.push_back(Picture(1.0, k++));
			for (int i = 1; i <= 20; ++i)
				recs.push_back(Picture(1.0 - 0.50 * double(i) / 20.0, k++));   // ends at gain = 0.50 (index 25)

			// Build rebound frame: mean ticks down -0.25 code while spread rebounds +4 codes (+5% contrast expansion about mean).
			const double endMeanP4 = double(recs.back().mean);
			std::vector<uint16_t> reboundHalf(size_t(kW) * kH);
			for (uint32_t y = 0; y < kH; ++y)
				for (uint32_t x = 0; x < kW; ++x)
				{
					const double base = 8192.0 + double(x) * 48000.0 / double(kW - 1);
					const double prevV = kBlack + 0.50 * (base - kBlack);
					const double v = (endMeanP4 - 0.25 * 256.0) + 1.05 * (prevV - endMeanP4);
					reboundHalf[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
				}
			recs.push_back(MakeRecordFromHalf(reboundHalf));   // index 26: rebound frame

			// Mandatory In-Test Negative Control (Rule 3): dMean < 0, spread rebounded >= +3 codes, AND cumulative FitGridFade
			// from base (index 5) to rebound (index 26) is STILL valid == true!
			const Record& endR = recs[25];
			const Record& rebR = recs[26];
			const uint32_t spEnd = endR.p99 - endR.p01;
			const uint32_t spReb = rebR.p99 - rebR.p01;
			Assert::IsTrue(rebR.mean < endR.mean, L"Negative control: rebound frame still has dMean < 0");
			Assert::IsTrue(double(spReb) - double(spEnd) >= 3.0 * 256.0, L"Negative control: rebound frame expands spread by >= 3 codes");
			const GridFadeFit cumFit = FitGridFade(recs[5].cells, rebR.cells, kLocalRadius);
			Assert::IsTrue(cumFit.valid && cumFit.dir < 0,
				L"Negative control: cumulative FitGridFade(base -> rebound) is still valid, proving stepA_Q / k < run.keep termination");

			FadeRun run;
			bool activeBeforeRebound = false;
			for (size_t i = 1; i < recs.size(); ++i)
			{
				const Events e = ClassifyEvents(recs[i - 1], recs[i], false, false);
				const bool rf = FoldFadeRun(recs[i - 1], recs[i], e, run);
				if (i == 25)
					activeBeforeRebound = e.fade || rf;
				if (i == 26)
				{
					Assert::IsFalse(e.fade, L"rebound frame must not be a pair fade");
					Assert::IsFalse(rf, L"fade-out followed by spread reversal must terminate immediately (runFade == false)");
				}
			}
			Assert::IsTrue(activeBeforeRebound, L"precondition: fade-out run was active immediately before spread reversal");
		}

		// ---- U7 v5 D6: CutTracker fade-in-from-black emergence veto hardening & provisional-vs-confirmed tests --------------

		// D6 #14: Hard cut from black into bright (0 -> 0.80 -> 0.88 -> 0.96) AND dim (0 -> 0.20 -> 0.22 -> 0.24, +10%/frame
		// lighting rise) shot must BOTH emit exact 1 confirmed T2 cut at frame 8.
		// Negative control proves dim case has aheadFit.valid && aheadFit.dir > 0 with gain < 1.25 (W1 v4 would falsely veto!).
		TEST_METHOD(CutTrackerKeepsHardCutFromBlackIntoShotWithLightingRise)
		{
			using namespace VpUnit::CutFadeJudge;

			struct Case { double g0, g1, g2; bool checkDimNegControl; };
			const Case cases[] = {
				{ 0.80, 0.88, 0.96, false },
				{ 0.20, 0.22, 0.24, true }
			};
			for (const Case& c : cases)
			{
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < 8; ++i)
					recs.push_back(Black(k++));
				recs.push_back(Picture(c.g0, k++));   // frame 8: hard cut from black
				recs.push_back(Picture(c.g1, k++));   // frame 9: +10% lighting rise
				recs.push_back(Picture(c.g2, k++));   // frame 10: +9% lighting rise
				for (int i = 0; i < 4; ++i)
					recs.push_back(Picture(c.g2, k++));

				if (c.checkDimNegControl)
				{
					// Mandatory In-Test Negative Control (Rule 3): prove W1 v4 (without the 1 + 1/kFadeMaxDiv = 1.25 gain bar)
					// WOULD have falsely vetoed this hard cut because aheadFit.valid && aheadFit.dir > 0 with gain ~ 1.10 < 1.25!
					Grid g8, g9;
					GridOf(recs[8], &g8);
					GridOf(recs[9], &g9);
					const GridFadeFit aheadFit = FitGridFadeGrid(g8.g, g9.g, kShiftRadius);
					Assert::IsTrue(aheadFit.valid && aheadFit.dir > 0,
						L"Negative control: dim lighting-rise pair satisfies W1 v4 aheadFit.valid && dir > 0");
					Assert::IsTrue(aheadFit.gain < 1.0 + 1.0 / double(kFadeMaxDiv),
						L"Negative control: +10%/frame lighting rise has gain < 1.25 (below duration-invariant black emergence bar)");
				}

				CutTracker tracker;
				std::vector<int> cutFrames;
				for (size_t i = 0; i < recs.size(); ++i)
				{
					const CutVerdict v = tracker.Push(static_cast<uint64_t>(i) + 1, recs[i]);
					if (v.known && v.cut)
						cutFrames.push_back(static_cast<int>(v.seq) - 1);
				}
				Assert::AreEqual(size_t(1), cutFrames.size(),
					L"hard cut from black into shot with lighting rise must emit exact 1 confirmed T2 cut");
				Assert::AreEqual(8, cutFrames[0], L"confirmed T2 cut must fire at exact cut frame 8");
			}
		}

		// D6 #15: Hard cut from black into a panning shot (both bright gain=0.80 and dim gain=0.20) emits exact 1 T2 cut at frame 8.
		TEST_METHOD(CutTrackerKeepsHardCutFromBlackIntoPan)
		{
			using namespace VpUnit::CutFadeJudge;

			for (double gain : { 0.80, 0.20 })
			{
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < 8; ++i)
					recs.push_back(Black(k++));
				for (int i = 0; i < 8; ++i)
					recs.push_back(Picture(gain, k++, uint32_t(2 * i), true));

				CutTracker tracker;
				std::vector<int> cutFrames;
				for (size_t i = 0; i < recs.size(); ++i)
				{
					const CutVerdict v = tracker.Push(static_cast<uint64_t>(i) + 1, recs[i]);
					if (v.known && v.cut)
						cutFrames.push_back(static_cast<int>(v.seq) - 1);
				}
				Assert::AreEqual(size_t(1), cutFrames.size(),
					L"hard cut from black into panning shot must emit exact 1 confirmed T2 cut");
				Assert::AreEqual(8, cutFrames[0], L"confirmed T2 cut must fire at exact cut frame 8");
			}
		}

		// D6 #16: 10-frame fade-in from black WITH simultaneous pan: Provisional() at frame 8 (ahead == nullptr) reports cut == true,
		// confirmed verdict for frame 8 after pushing frame 9 vetoes it (v.cut == false), and 0 confirmed T2 cuts fire overall.
		TEST_METHOD(CutTrackerVetoesFadeInFromBlackUnderPanAndProvisionalDiffers)
		{
			using namespace VpUnit::CutFadeJudge;

			std::vector<Record> recs;
			uint32_t k = 0;
			for (int i = 0; i < 8; ++i)
				recs.push_back(Black(k++));
			for (int i = 1; i <= 10; ++i)
				recs.push_back(TexturedPanFadePicture(double(i) / 10.0, 0.4 * double(i), 0.0, k++));
			for (int i = 1; i <= 6; ++i)
				recs.push_back(TexturedPanFadePicture(1.0, 0.4 * double(10 + i), 0.0, k++));

			CutTracker tracker;
			int confirmedCuts = 0;
			bool checkedProvisional8 = false;
			bool checkedConfirmed8 = false;
			for (size_t i = 0; i < recs.size(); ++i)
			{
				const CutVerdict v = tracker.Push(static_cast<uint64_t>(i) + 1, recs[i]);
				if (v.known && v.cut)
					++confirmedCuts;
				if (i == 8)
				{
					const CutVerdict prov = tracker.Provisional();
					Assert::IsTrue(prov.known && !prov.confirmed, L"Provisional() at frame 8 has ahead == nullptr");
					Assert::IsTrue(prov.cut, L"Provisional() at frame 8 flags cut == true before frame 9 confirms fade-in");
					checkedProvisional8 = true;
				}
				if (i == 9)
				{
					Assert::IsTrue(v.known && v.confirmed && v.seq == 9u, L"pushing frame 9 confirms frame 8 (seq 9)");
					Assert::IsFalse(v.cut, L"confirmed verdict for frame 8 must veto cut (v.cut == false) under pan fade-in");
					checkedConfirmed8 = true;
				}
			}
			Assert::IsTrue(checkedProvisional8 && checkedConfirmed8);
			Assert::AreEqual(0, confirmedCuts, L"genuine fade-in from black under pan must emit 0 confirmed T2 cuts");
		}

		// ---- U7 v5 D7: failedBlackOrigin re-seed bound (at most 1 seed frame lost) ------------------------------------------

		// D7 #17: Tests both N=48 and N=96 fade-ins in Baseline vs AfterFailedBlackOrigin (7x Black -> 1x FlatLift(+2 codes)
		// -> N fade-in -> 6x Picture(1.0)). Proves failedBlackOrigin actually fires on fade-in frame 1 and loses <= 1 seed frame.
		TEST_METHOD(GenuineFadeInRightAfterFailedBlackOriginLosesAtMostOneSeedFrame)
		{
			using namespace VpUnit::CutFadeJudge;

			for (int n : { 48, 96 })
			{
				// Build Baseline: 8x Black -> N fade-in frames -> 6x Picture(1.0).
				std::vector<Record> baseRecs;
				uint32_t kBase = 0;
				for (int i = 0; i < 8; ++i)
					baseRecs.push_back(Black(kBase++));
				for (int i = 1; i <= n; ++i)
					baseRecs.push_back(Picture(double(i) / double(n), kBase++));
				for (int i = 0; i < 6; ++i)
					baseRecs.push_back(Picture(1.0, kBase++));

				// Build AfterFailedBlackOrigin: 7x Black -> 1x near-black FlatLift(+2 codes, 0 < spread <= kBlackSpreadP4 so it trips e.cut, not e.flash)
				// -> identical N fade-in frames -> 6x Picture(1.0).
				auto MakeNearBlackFlatLift = [](uint32_t k) -> Record {
					std::vector<uint16_t> half(size_t(kW) * kH);
					for (uint32_t y = 0; y < kH; ++y)
						for (uint32_t x = 0; x < kW; ++x)
						{
							const double v = kBlack + 2.0 * 256.0 + double(Noise(x, y, k)) * 0.5;
							half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
						}
					return MakeRecordFromHalf(half);
				};
				std::vector<Record> afterRecs;
				for (int i = 0; i < 7; ++i)
					afterRecs.push_back(baseRecs[size_t(i)]);
				afterRecs.push_back(MakeNearBlackFlatLift(7u));   // index 7: +2 code flat lift from black (spread ~0.98 code <= 1 code -> e.cut)
				for (size_t i = 8; i < baseRecs.size(); ++i)
					afterRecs.push_back(baseRecs[i]);

				// Play Baseline.
				int baseTotalFades = 0, firstBaseFadeIdx = -1, basePrePostFades = 0;
				{
					FadeRun run;
					for (size_t i = 1; i < baseRecs.size(); ++i)
					{
						const Events e = ClassifyEvents(baseRecs[i - 1], baseRecs[i], false, false);
						const bool rf = FoldFadeRun(baseRecs[i - 1], baseRecs[i], e, run);
						const bool isFade = e.fade || rf;
						if (i < 8u || i >= size_t(8 + n))
							basePrePostFades += isFade ? 1 : 0;
						else if (isFade)
						{
							++baseTotalFades;
							if (firstBaseFadeIdx < 0)
								firstBaseFadeIdx = int(i);
						}
					}
				}
				Assert::AreEqual(0, basePrePostFades, L"baseline must emit 0 false fades on static pre/post frames");
				Assert::IsTrue(firstBaseFadeIdx >= 8 && baseTotalFades >= n - 4, L"baseline detects genuine fade-in");

				// Play AfterFailedBlackOrigin and verify failedBlackOrigin fires on fade-in frame 1 (index 8).
				int postTotalFades = 0, firstPostFadeIdx = -1, postPrePostFades = 0;
				{
					FadeRun run;
					for (size_t i = 1; i < afterRecs.size(); ++i)
					{
						const Events e = ClassifyEvents(afterRecs[i - 1], afterRecs[i], false, false);
						const bool rf = FoldFadeRun(afterRecs[i - 1], afterRecs[i], e, run);
						const bool isFade = e.fade || rf;
						if (i == 7u)
						{
							Assert::IsTrue(e.cut, L"precondition: +2 code flat lift from black trips e.cut");
							Assert::IsFalse(rf, L"single flat lift pair never fires runFade");
							Assert::IsTrue(run.blackOrigin && run.direction == 1 && run.pairs == 1u,
								L"precondition: flat lift seeds blackOrigin run (direction=1, pairs=1)");
						}
						else if (i == 8u)
						{
							Assert::IsTrue(e.cut, L"precondition: fade-in frame 1 after flat lift trips e.cut");
							Assert::IsFalse(rf, L"failedBlackOrigin blocks pair 1 from firing runFade");
							Assert::AreEqual(0, run.direction, L"failedBlackOrigin must reset run.direction to 0 (proving guard fired)");
							Assert::AreEqual(0u, run.pairs, L"failedBlackOrigin must reset run.pairs to 0 (proving guard fired)");
						}

						if (i < 8u || i >= size_t(8 + n))
							postPrePostFades += isFade ? 1 : 0;
						else if (isFade)
						{
							++postTotalFades;
							if (firstPostFadeIdx < 0)
								firstPostFadeIdx = int(i);
						}
					}
				}
				Assert::AreEqual(0, postPrePostFades, L"AfterFailedBlackOrigin must emit 0 false fades on static pre/post frames");
				Assert::IsTrue(firstPostFadeIdx >= 8, L"AfterFailedBlackOrigin must re-seed and detect the genuine fade-in");
				Assert::IsTrue(postTotalFades >= baseTotalFades - 1,
					L"D7 bound: genuine fade-in right after failedBlackOrigin loses at most 1 seed frame in total fade hits");
				Assert::IsTrue(firstPostFadeIdx <= firstBaseFadeIdx + 1,
					L"D7 bound: first detected fade frame after failedBlackOrigin is delayed by at most 1 frame");
			}
		}

		// Derived Fix A (FootprintPreserved vs blanket 25% lit-area floor): a sparse centered static textured title card
		// covering only 60/392 = 15.3% of the interior (< kFadeMinLitCells = 98) on pure black must still be detected cleanly
		// across fade-out to black and fade-in from black, while ZoomingDarkeningCenterGraphicOnBlackIsNotAFade stays 0 fades.
		TEST_METHOD(SparseStaticTitleCardOnBlackFadeOutAndInIsDetected)
		{
			using namespace VpUnit::CutFadeJudge;

			auto TitleCardFrame = [](double gain, uint32_t k) -> Record {
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						// Centered static card: 50x30 px = 10x6 = 60 interior grid cells (15.3% of 392, strictly < kFadeMinLitCells = 98).
						const bool inCard = (x >= 55u && x < 105u && y >= 30u && y < 60u);
						double v = kBlack;
						if (inCard && gain > 0.0)
						{
							const double pat = 0.60 + 0.20 * std::sin(double(x) * 0.25) + 0.15 * std::cos(double(y) * 0.30);
							v = kBlack + gain * pat * 48000.0 + double(Noise(x, y, k)) * 0.125;
						}
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				return MakeRecordFromHalf(half);
			};

			const int pre = 8, nOut = 48, holdBlack = 12, nIn = 48, post = 8;
			std::vector<Record> recs;
			uint32_t k = 0;
			for (int i = 0; i < pre; ++i)
				recs.push_back(TitleCardFrame(1.0, k++));
			for (int i = 1; i <= nOut; ++i)
				recs.push_back(i == nOut ? TitleCardFrame(0.0, k++) : TitleCardFrame(1.0 - double(i) / double(nOut), k++));
			for (int i = 0; i < holdBlack; ++i)
				recs.push_back(TitleCardFrame(0.0, k++));
			for (int i = 1; i <= nIn; ++i)
				recs.push_back(TitleCardFrame(double(i) / double(nIn), k++));
			for (int i = 0; i < post; ++i)
				recs.push_back(TitleCardFrame(1.0, k++));

			// Mandatory In-Test Negative Control (Rule 3): verify on a mid-fade pair that rigid FitGridFadeGrid succeeds
			// with litCells strictly below kFadeMinLitCells (98) — proving a blanket 25% lit-area floor would wrongly reject
			// this genuine sparse title-card fade, whereas FootprintPreserved accepts it!
			{
				double gBase[kGridH][kGridW], gMid[kGridH][kGridW];
				LoadGridCodes(recs[size_t(pre - 1)].cells, gBase);
				LoadGridCodes(recs[size_t(pre + 12)].cells, gMid);
				const GridFadeFit midFit = FitGridFadeGrid(gBase, gMid, kLocalRadius);
				Assert::IsTrue(midFit.valid && midFit.dir < 0,
					L"Negative control: rigid FitGridFadeGrid must detect sparse static title-card fade");
				Assert::IsTrue(midFit.litCells > 0 && midFit.litCells < kFadeMinLitCells,
					L"Negative control: title card litCells must be strictly below kFadeMinLitCells (98) so a blanket 25% floor would reject it");
			}

			const size_t outBegin = size_t(pre);
			const size_t outEnd = outBegin + size_t(nOut);
			const size_t blackEnd = outEnd + size_t(holdBlack);
			const size_t inEnd = blackEnd + size_t(nIn);

			FadeRun run;
			int preFades = 0, outFades = 0, blackFades = 0, inFades = 0, postFades = 0;
			double fadeOutKeepProd = 1.0;
			for (size_t i = 1; i < recs.size(); ++i)
			{
				const Events e = ClassifyEvents(recs[i - 1], recs[i], false, false);
				double rk = 1.0;
				const bool rf = FoldFadeRun(recs[i - 1], recs[i], e, run, &rk);
				const bool isFade = e.fade || rf;
				if (i < outBegin)
					preFades += isFade ? 1 : 0;
				else if (i < outEnd)
				{
					outFades += isFade ? 1 : 0;
					fadeOutKeepProd *= FrameFadeKeep(recs[i - 1], recs[i], e.fade, rf, rk);
				}
				else if (i < blackEnd)
					blackFades += isFade ? 1 : 0;
				else if (i < inEnd)
					inFades += isFade ? 1 : 0;
				else
					postFades += isFade ? 1 : 0;
			}

			Assert::AreEqual(0, preFades, L"0 false fades on static pre-fade title card");
			Assert::AreEqual(0, blackFades, L"0 false fades on black hold");
			Assert::AreEqual(0, postFades, L"0 false fades on static post-fade title card");
			Assert::IsTrue(outFades >= 44, L"sparse static title-card 48-frame fade-out must hit >= 44/48 frames");
			Assert::IsTrue(inFades >= 44, L"sparse static title-card 48-frame fade-in must hit >= 44/48 frames");
			Assert::IsTrue(fadeOutKeepProd < 0.5, L"sparse static title-card fade-out must decay history keep < 0.5");
		}

		// Derived Fix B (FitMotionTolerantFade Part B per-tile affine scaling uniformity + macro-tile L2 variance reduction):
		// A full-frame textured scene under a non-uniform shadow sweep (left 4 macro-tiles darken ~25%, right 4 darken ~2.5%)
		// with gentle local drift passes Part A quantile OLS, passes Part C coarse structure, AND darkens all 8/8 macro-tiles
		// > kFadeCellStepEps (so naive sign-only Part B would falsely fire!), yet FitMotionTolerantFade returns valid == false
		// and Play(recs) emits 0 fades across all 30 frames.
		TEST_METHOD(NonUniformLightingDriftAndShadowSweepAreNotAFade)
		{
			using namespace VpUnit::CutFadeJudge;

			std::vector<Record> recs;
			recs.reserve(30);
			for (int i = 0; i < 30; ++i)
			{
				const double t = i < 4 ? 0.0 : (i > 25 ? 1.0 : double(i - 3) / 22.0);
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						const double sx = double(x) + (y < kH / 2u ? 6.0 * t : -6.0 * t);
						double u = 0.0;
						double gain = 1.0;
						if (x < kW / 2u)
						{
							u = 0.53 + 0.22 * std::sin(sx * 0.24) * std::cos(double(y) * 0.18) + 0.14 * std::cos(double(y) * 0.09);
							gain = 1.0 - 0.25 * t;
						}
						else
						{
							u = 0.47 + 0.17 * std::sin(sx * 0.24) * std::cos(double(y) * 0.18) + 0.12 * std::cos(double(y) * 0.09);
							gain = 1.0 - 0.025 * t;
						}
						const double v = kBlack + gain * u * 48000.0 + double(Noise(x, y, uint32_t(i))) * 0.125;
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				recs.push_back(MakeRecordFromHalf(half));
			}

			// Mandatory In-Test Negative Control (Rule 3) on mid-sweep pair recs[6] -> recs[22]:
			// Prove (1) all 8/8 4x2 macro-tiles darkened > kFadeCellStepEps (naive sign-only Part B would accept!),
			// (2) sorted order-statistic OLS (Part A alone) satisfies |1 - A_q| > kFadeRunMinGain, b_q in (-12, 48),
			// and vFade_q < kFadeResidualReduction * vId_q, YET (3) production FitMotionTolerantFade returns valid == false!
			{
				constexpr int R = kLocalRadius;
				constexpr int kIntN = (kGridH - 2 * R) * (kGridW - 2 * R);
				double gA[kGridH][kGridW], gB[kGridH][kGridW];
				double ra[kGridH][kGridW], ca[kGridH][kGridW];
				double vx = 0.0, vy = 0.0;
				LoadGridCodes(recs[6].cells, gA);
				LoadGridCodes(recs[22].cells, gB);
				AlignGrid(gA, gB, kLocalRadius, ra, ca, &vx, &vy);

				// Check Part A (sorted order-statistic OLS) directly on (ra, ca).
				std::vector<double> rSorted, cSorted;
				rSorted.reserve(size_t(kIntN));
				cSorted.reserve(size_t(kIntN));
				for (int y = R; y < kGridH - R; ++y)
					for (int x = R; x < kGridW - R; ++x)
					{
						rSorted.push_back(ra[y][x]);
						cSorted.push_back(ca[y][x]);
					}
				std::sort(rSorted.begin(), rSorted.end());
				std::sort(cSorted.begin(), cSorted.end());
				double sxQ = 0.0, syQ = 0.0;
				for (int k = 0; k < kIntN; ++k)
				{
					sxQ += rSorted[size_t(k)];
					syQ += cSorted[size_t(k)];
				}
				const double mxQ = sxQ / double(kIntN);
				const double myQ = syQ / double(kIntN);
				double sxxQ = 0.0, sxyQ = 0.0;
				for (int k = 0; k < kIntN; ++k)
				{
					const double dx = rSorted[size_t(k)] - mxQ;
					sxxQ += dx * dx;
					sxyQ += dx * (cSorted[size_t(k)] - myQ);
				}
				const double aQ = sxyQ / sxxQ;
				const double cQ = myQ - aQ * mxQ;
				const double bQ = cQ / (1.0 - aQ);
				double vIdQ = 0.0, vFadeQ = 0.0;
				for (int k = 0; k < kIntN; ++k)
				{
					const double dId = cSorted[size_t(k)] - rSorted[size_t(k)];
					const double dFade = cSorted[size_t(k)] - (aQ * rSorted[size_t(k)] + cQ);
					vIdQ += dId * dId;
					vFadeQ += dFade * dFade;
				}
				Assert::IsTrue(aQ > 0.0 && std::fabs(1.0 - aQ) > kFadeRunMinGain,
					L"Negative control: Part A quantile OLS satisfies |1 - A_q| > kFadeRunMinGain");
				Assert::IsTrue(bQ > double(kFadeMinBlack) && bQ < double(kFadeMaxBlack),
					L"Negative control: Part A quantile implied black lies strictly in (kFadeMinBlack, kFadeMaxBlack)");
				Assert::IsTrue(vFadeQ < kFadeResidualReduction * vIdQ,
					L"Negative control: Part A quantile OLS satisfies vFade_q < 0.5 * vId_q");

				// Check 4x2 macro-tile signs directly on (ra, ca).
				int tileSameEps = 0, tileOppEps = 0;
				for (int tr = 0; tr < 2; ++tr)
					for (int tc = 0; tc < 4; ++tc)
					{
						double sr = 0.0, sc = 0.0;
						const int y0 = R + tr * 7;
						const int x0 = R + tc * 7;
						for (int dy = 0; dy < 7; ++dy)
							for (int dx = 0; dx < 7; ++dx)
							{
								sr += ra[y0 + dy][x0 + dx];
								sc += ca[y0 + dy][x0 + dx];
							}
						const double dTile = (sc - sr) / 49.0;
						if (-dTile > kFadeCellStepEps)
							++tileSameEps;
						else if (dTile > kFadeCellStepEps)
							++tileOppEps;
					}
				Assert::IsTrue(tileSameEps >= 6 && tileOppEps == 0,
					L"Negative control: >= 6/8 macro-tiles darkened > kFadeCellStepEps with 0 opposing (sign-only Part B would falsely pass!)");

				const GridFadeFit mtFit = FitMotionTolerantFade(ra, ca, vx, vy);
				Assert::IsFalse(mtFit.valid,
					L"Derived Fix B: FitMotionTolerantFade must reject non-uniform shadow sweep via per-tile affine uniformity / macro-tile L2 reduction");
			}

			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, recs.size()),
				L"non-uniform lighting drift and shadow sweep must emit 0 fades");
		}

		// Rec.709 clipped-white fade-out censorship + whole-frame stepAQ jump guard vs clipped-white ambient flicker:
		// 1. Linear-exposure 40-frame fade-out with ~2.5% specular highlights clamped to Rec.709 peak white (235 codes)
		//    is detected BOTH while p99 remains clipped at 235 (once 1 - gain >= 0.25) AND across steep p99 unclipping
		//    where aSpread * 4 >= contRef while run.keep * |1 - stepAQ| < 0.25 (tested at both 3.0x and 12.0x highlight radiance).
		// 2. Mandatory Negative Controls (Rule 3): proves aP99 == 1.0 on clipped mid-fade pair (un-exempted check would reject)
		//    AND proves +-4% clipped-white ambient flicker (1 - gain = 0.04 < 0.25) is STILL rejected by HighlightScalingConsistent!
		TEST_METHOD(ClippedWhiteHighlightFadeOutIsDetectedWhileAmbientFlickerIsRejected)
		{
			using namespace VpUnit::CutFadeJudge;

			auto ClippedSpecularFrame = [](double gain, double specRadiance, uint32_t k) -> Record {
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						double lin = 0.0;
						if (x < 10u && y < 36u)
						{
							// 10 x 36 = 360 px = 2.5% of 14400 px -> controls p99 at Rec.709 peak white (235 codes) until unclipped.
							lin = specRadiance * gain;
						}
						else
						{
							const double pat = 0.50
								+ 0.22 * std::sin(double(x) * 0.18) * std::cos(double(y) * 0.16)
								+ 0.16 * std::cos(double(x) * 0.07 + double(y) * 0.09);
							lin = pat * gain;
						}
						const double clamped = (std::min)(1.0, (std::max)(0.0, lin));
						const double code = 16.0 + 219.0 * clamped + (gain > 0.0 && clamped < 1.0 ? double(Noise(x, y, k)) / (256.0 * 16.0) : 0.0);
						const double p4 = code * 256.0;
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(p4 + 0.5))));
					}
				return MakeRecordFromHalf(half);
			};

			// Test both 3.0x specular radiance (standard Rec.709 clipped highlight) and 12.0x HDR specular radiance
			// (exercises steep p99 unclipping where aSpread * kFadeMaxDiv >= contRef while run.keep * |1 - stepAQ| < 0.25).
			const double specRadiances[] = { 3.0, 12.0 };
			bool verifiedSteepUnclipContinuation = false;
			for (double specRad : specRadiances)
			{
				const int pre = 8, nOut = 40, postBlack = 10;
				std::vector<Record> recs;
				uint32_t k = 0;
				for (int i = 0; i < pre; ++i)
					recs.push_back(ClippedSpecularFrame(1.0, specRad, k++));
				for (int i = 1; i <= nOut; ++i)
					recs.push_back(ClippedSpecularFrame(1.0 - double(i) / double(nOut), specRad, k++));
				for (int i = 0; i < postBlack; ++i)
					recs.push_back(ClippedSpecularFrame(0.0, specRad, k++));

				// Negative Control 1 (on clipped mid-fade pair: pre-1 -> pre+11, gain = 0.70, 1 - gain = 0.30 >= 0.25):
				// Verify p99 is STILL clipped (curP99 == refP99 -> aP99 == 1.0, so un-exempted highlight check WOULD reject it!)
				// while HighlightScalingConsistent accepts it due to clipped-white fade-out censorship.
				{
					const Record& rBase = recs[size_t(pre - 1)];
					const Record& rMid = recs[size_t(pre + 11)];
					Assert::AreEqual(rBase.p99, rMid.p99, L"Negative control: mid-fade p99 must still be clipped at 235 codes");
					const GridFadeFit fit = FitGridFade(rBase.cells, rMid.cells, kLocalRadius);
					Assert::IsTrue(fit.valid && fit.dir < 0 && (1.0 - fit.gain) >= 1.0 / double(kFadeMaxDiv),
						L"precondition: clipped mid-fade pair has valid affine fit with 1 - gain >= 0.25");
					const double aP99 = (double(rMid.p99) * kGridScale - fit.black) / (double(rBase.p99) * kGridScale - fit.black);
					Assert::AreEqual(1.0, aP99, 1e-12, L"Negative control: clipped p99 has aP99 == 1.0 (un-exempted check would reject)");
					Assert::IsTrue(HighlightScalingConsistent(rBase.p99, rMid.p99, fit.gain, fit.black, fit.dir),
						L"HighlightScalingConsistent must accept clipped-white fade-out once 1 - gain >= 0.25");
				}

				const uint32_t clippedP99 = recs[size_t(pre - 1)].p99;
				FadeRun run;
				int preFades = 0, outFades = 0, blackFades = 0;
				int hitsWhileClipped = 0;
				for (size_t i = 1; i < recs.size(); ++i)
				{
					const uint32_t spPrev = recs[i - 1].p99 > recs[i - 1].p01 ? recs[i - 1].p99 - recs[i - 1].p01 : 0u;
					const uint32_t spCur = recs[i].p99 > recs[i].p01 ? recs[i].p99 - recs[i].p01 : 0u;
					const int64_t contRefBefore = int64_t((std::max)(run.refSpread, (std::max)(spPrev, spCur)));
					const int64_t dSp = int64_t(spCur) - int64_t(spPrev);
					const int64_t aSp = dSp < 0 ? -dSp : dSp;
					const bool steepP99Unclip = (aSp * kFadeMaxDiv >= contRefBefore) && (recs[i].p99 < clippedP99);

					const Events e = ClassifyEvents(recs[i - 1], recs[i], false, false);
					const bool rf = FoldFadeRun(recs[i - 1], recs[i], e, run);
					const bool isFade = e.fade || rf;
					if (i < size_t(pre))
						preFades += isFade ? 1 : 0;
					else if (i < size_t(pre + nOut))
					{
						outFades += isFade ? 1 : 0;
						if (isFade && recs[i].p99 == clippedP99)
							++hitsWhileClipped;
						if (steepP99Unclip && !e.cut && rf)
							verifiedSteepUnclipContinuation = true;
					}
					else
						blackFades += isFade ? 1 : 0;
				}
				Assert::AreEqual(0, preFades, L"0 false fades during static pre-fade clipped scene");
				Assert::AreEqual(0, blackFades, L"0 false fades during black hold");
				Assert::IsTrue(outFades >= 26, L"clipped-white 40-frame linear fade-out must hit >= 26/40 frames");
				Assert::IsTrue(hitsWhileClipped >= 10, L"must detect fade-out hits while p99 is still clipped at 235 codes");
			}
			Assert::IsTrue(verifiedSteepUnclipContinuation,
				L"hoisted whole-frame stepAQ jump guard must continue fade-out cleanly across steep p99 unclipping (aSpread * 4 >= contRef, !e.cut)");

			// Negative Control 2: +-4% clipped-white ambient flicker (1 - gain = 0.04 < 0.25, curP99 == refP99 == 235 codes)
			// must STILL be rejected by HighlightScalingConsistent!
			{
				const Record rA = ClippedSpecularFrame(1.00, 3.0, 9001u);
				const Record rB = ClippedSpecularFrame(0.96, 3.0, 9002u);
				Assert::AreEqual(rA.p99, rB.p99, L"Negative control: +-4% flicker keeps p99 clipped at 235 codes");
				const GridFadeFit flFit = FitGridFade(rA.cells, rB.cells, kLocalRadius);
				Assert::IsTrue(flFit.valid && flFit.dir < 0 && (1.0 - flFit.gain) < 1.0 / double(kFadeMaxDiv),
					L"Negative control: 4% drop has valid affine fit with 1 - gain < 0.25");
				Assert::IsFalse(HighlightScalingConsistent(rA.p99, rB.p99, flFit.gain, flFit.black, flFit.dir),
					L"HighlightScalingConsistent must STILL reject +-4% clipped-white ambient flicker (1 - gain < 0.25)");
			}
		}

		// Part A: Full-LSB macro-tile displacement bar (kFadeMinSpread = 1.0 code across >= 75% of lit tiles) rejects
		// sub-code motion/exposure wobble (0.55..0.90 codes) where old 0.5-code bar would fire.
		// Part B: Symmetric brightening HighlightScalingConsistent rejects localized bright surges (aP99 = 1.35 vs gain = 1.12)
		// where a one-sided lower bound would accept.
		TEST_METHOD(SubCodeMotionExposureWobbleAndBrighteningSurgeAreNotAFade)
		{
			using namespace VpUnit::CutFadeJudge;

			// ---- Part A: Sub-code motion/exposure wobble ----
			{
				auto WobbleFrame = [](int frameIdx, double gain) -> Record {
					// Discs oscillate +-5 px strictly inside tiles (0,0) and (1,3): defeats rigid cell-OLS without crossing tile edges.
					const double cx1 = 27.0 + 5.0 * std::sin(double(frameIdx) * 0.7);
					const double cx2 = 132.0 - 5.0 * std::sin(double(frameIdx) * 0.7);
					std::vector<uint16_t> half(size_t(kW) * kH);
					for (uint32_t y = 0; y < kH; ++y)
						for (uint32_t x = 0; x < kW; ++x)
						{
							double pat = 0.20 + 0.08 * std::sin(double(x) * 0.18) * std::cos(double(y) * 0.14) + 0.04 * std::cos(double(y) * 0.09);
							const double d1 = (double(x) - cx1) * (double(x) - cx1) + (double(y) - 27.0) * (double(y) - 27.0);
							const double d2 = (double(x) - cx2) * (double(x) - cx2) + (double(y) - 62.0) * (double(y) - 62.0);
							if (d1 <= 81.0)
								pat = 0.38;
							if (d2 <= 81.0)
								pat = 0.04;
							const double v = kBlack + gain * pat * 145.0 * 256.0;
							half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
						}
					return MakeRecordFromHalf(half);
				};

				const double gains[] = {
					1.000, 1.000, 1.000, 1.000,
					0.993, 0.986, 0.979, 0.972,
					0.979, 0.986, 0.993, 1.000,
					0.993, 0.986, 0.979, 0.972,
					0.979, 0.986, 0.993, 1.000,
					1.000, 1.000, 1.000, 1.000
				};
				std::vector<Record> recs;
				int idx = 0;
				for (double g : gains)
					recs.push_back(WobbleFrame(idx++, g));

				// Mandatory In-Test Negative Control (Rule 3) on peak wobble pair recs[3] -> recs[7]:
				// Assert >= 6/8 macro-tiles move > 0.5 code (old bar would pass!) while < 6/8 move > 1.0 code
				// AND production FitMotionTolerantFade returns valid == false!
				{
					constexpr int R = kLocalRadius;
					double gA[kGridH][kGridW], gB[kGridH][kGridW];
					double ra[kGridH][kGridW], ca[kGridH][kGridW];
					double vx = 0.0, vy = 0.0;
					LoadGridCodes(recs[3].cells, gA);
					LoadGridCodes(recs[7].cells, gB);
					AlignGrid(gA, gB, kLocalRadius, ra, ca, &vx, &vy);

					int tilesOverHalf = 0, tilesOverOne = 0;
					for (int tr = 0; tr < 2; ++tr)
						for (int tc = 0; tc < 4; ++tc)
						{
							double sr = 0.0, sc = 0.0;
							const int y0 = R + tr * 7;
							const int x0 = R + tc * 7;
							for (int dy = 0; dy < 7; ++dy)
								for (int dx = 0; dx < 7; ++dx)
								{
									sr += ra[y0 + dy][x0 + dx];
									sc += ca[y0 + dy][x0 + dx];
								}
							const double drop = (sr - sc) / 49.0;
							if (drop > kFadeCellStepEps)
								++tilesOverHalf;
							if (drop > double(kFadeMinSpread))
								++tilesOverOne;
						}
					Assert::IsTrue(tilesOverHalf >= 6,
						L"Negative control: peak sub-code wobble must move >= 6/8 macro-tiles > 0.5 code (old bar would fire)");
					Assert::IsTrue(tilesOverOne < 6,
						L"Negative control: peak sub-code wobble must move < 6/8 macro-tiles > 1.0 code");
					Assert::IsFalse(FitGridFadeAligned(ra, ca, vx, vy).valid,
						L"precondition: opposite-oscillating discs defeat rigid FitGridFadeAligned");
					Assert::IsFalse(FitMotionTolerantFade(ra, ca, vx, vy).valid,
						L"full-LSB macro-tile bar (1.0 code across >= 75% of lit tiles) must reject sub-code wobble");
				}

				const auto res = Play(recs);
				Assert::AreEqual(0, Count(res, 1, recs.size()),
					L"sub-code motion/exposure wobble must emit 0 fades across the entire sequence");
			}

			// ---- Part B: Brightening highlight surge (aP99 = 1.35 vs gain = 1.12, dir = +1) ----
			{
				const double black = 16.0;
				const double gain = 1.12;
				const uint32_t refP99 = static_cast<uint32_t>(160.0 * 256.0);
				// (210.4 - 16) / (160 - 16) = 194.4 / 144.0 = exact 1.35
				const uint32_t curP99 = static_cast<uint32_t>(std::floor(210.4 * 256.0 + 0.5));
				const double aP99 = (double(curP99) * kGridScale - black) / (double(refP99) * kGridScale - black);

				// Mandatory In-Test Negative Control (Rule 3): one-sided lower bound WOULD accept this +35% surge!
				Assert::IsTrue((aP99 - 1.0) >= kFadeResidualReduction * std::fabs(1.0 - gain),
					L"Negative control: one-sided brightening check would falsely accept aP99=1.35 vs gain=1.12");
				Assert::IsFalse(HighlightScalingConsistent(refP99, curP99, gain, black, +1),
					L"symmetric brightening HighlightScalingConsistent must reject highlight surge aP99=1.35 vs gain=1.12");

				// Synthetic frame sequence: interior rises +12% (+3%/frame over 4 frames) while a 2.5% practical lamp surges +35%.
				auto SurgeFrame = [](double sceneGain, double lampGain, uint32_t k) -> Record {
					std::vector<uint16_t> half(size_t(kW) * kH);
					for (uint32_t y = 0; y < kH; ++y)
						for (uint32_t x = 0; x < kW; ++x)
						{
							double v = kBlack;
							if (x < 10u && y < 36u)
							{
								v = kBlack + lampGain * (160.0 - 16.0) * 256.0;
							}
							else
							{
								const double base = 8192.0 + 20000.0 * double(x) / double(kW - 1) + 4000.0 * std::sin(double(y) * 0.14);
								v = kBlack + sceneGain * (base - kBlack) + double(Noise(x, y, k)) * 0.125;
							}
							half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
						}
					return MakeRecordFromHalf(half);
				};

				std::vector<Record> surgeRecs;
				uint32_t k = 0;
				for (int i = 0; i < 4; ++i)
					surgeRecs.push_back(SurgeFrame(1.00, 1.00, k++));
				for (int i = 1; i <= 4; ++i)
					surgeRecs.push_back(SurgeFrame(1.00 + 0.03 * double(i), 1.00 + 0.0875 * double(i), k++));
				for (int i = 0; i < 4; ++i)
					surgeRecs.push_back(SurgeFrame(1.12, 1.35, k++));

				// Negative control: rigid FitGridFade alone returns valid == true on surgeRecs[3] -> surgeRecs[7],
				// while HighlightScalingConsistent rejects it!
				const GridFadeFit surgeFit = FitGridFade(surgeRecs[3].cells, surgeRecs[7].cells, kLocalRadius);
				Assert::IsTrue(surgeFit.valid && surgeFit.dir > 0,
					L"Negative control: rigid FitGridFade alone returns valid on brightening scene + lamp surge");
				Assert::IsFalse(HighlightScalingConsistent(surgeRecs[3].p99, surgeRecs[7].p99, surgeFit.gain, surgeFit.black, surgeFit.dir),
					L"HighlightScalingConsistent must reject synthetic brightening highlight surge");

				const auto surgeRes = Play(surgeRecs);
				Assert::AreEqual(0, Count(surgeRes, 1, surgeRecs.size()),
					L"brightening highlight surge (+35% p99 vs +12% scene) must emit 0 fades");
			}
		}

		// ---- Blind-Audit Pass 2 Break-It & Negative-Control Tests (HIGH-3 & MEDIUM-3) ---------------------------------------

		// HIGH-3 #1: FitMotionTolerantFade enforces the derived 2x SNR gain floor (kFadeRunMinGain / kFadeResidualReduction = 0.04):
		// a 3% non-rigid exposure ripple under opposite-moving discs (|1 - A_q| in (0.02, 0.04)) is rejected while a 6.5% fade-out
		// under identical non-rigid motion is accepted.
		TEST_METHOD(MotionTolerantFadeEnforcesTwoXSnrGainFloor)
		{
			using namespace VpUnit::CutFadeJudge;

			auto MakeNonRigidDiscs = [](int frameIdx, double gain) -> Record {
				const double cx1 = 36.0 + 2.0 * double(frameIdx);
				const double cx2 = 124.0 - 2.0 * double(frameIdx);
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						double pat = 0.45 + 0.26 * std::cos(double(y) * 0.065) + 0.14 * std::sin(double(x) * 0.09);
						const double d1 = (double(x) - cx1) * (double(x) - cx1) + (double(y) - 30.0) * (double(y) - 30.0);
						const double d2 = (double(x) - cx2) * (double(x) - cx2) + (double(y) - 60.0) * (double(y) - 60.0);
						if (d1 <= 100.0)
							pat = 0.92;
						if (d2 <= 100.0)
							pat = 0.06;
						const double v = kBlack + gain * pat * 48000.0;
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
					}
				return MakeRecordFromHalf(half);
			};

			const Record rBase = MakeNonRigidDiscs(0, 1.00);
			const Record rRipple3Pct = MakeNonRigidDiscs(6, 0.97);
			const Record rFade6p5Pct = MakeNonRigidDiscs(6, 0.935);

			double gBase[kGridH][kGridW], gRip[kGridH][kGridW], gFade[kGridH][kGridW];
			LoadGridCodes(rBase.cells, gBase);
			LoadGridCodes(rRipple3Pct.cells, gRip);
			LoadGridCodes(rFade6p5Pct.cells, gFade);

			// 1. 3% non-rigid exposure ripple: rigid fails, order-statistic |1 - A_q| is strictly in (0.02, 0.04)
			//    (negative control: old 0.02 floor would accept!), yet FitMotionTolerantFade rejects via 2x SNR floor!
			{
				double ra[kGridH][kGridW], ca[kGridH][kGridW];
				double vx = 0.0, vy = 0.0;
				AlignGrid(gBase, gRip, kLocalRadius, ra, ca, &vx, &vy);
				Assert::IsFalse(FitGridFadeAligned(ra, ca, vx, vy).valid,
					L"precondition: rigid FitGridFadeAligned fails under opposite-moving discs");
				const GridFadeFit mtRip = FitMotionTolerantFade(ra, ca, vx, vy);
				Assert::IsTrue(std::fabs(1.0 - mtRip.gain) > kFadeRunMinGain &&
					std::fabs(1.0 - mtRip.gain) < kFadeRunMinGain / kFadeResidualReduction,
					L"Negative control: 3% non-rigid ripple has |1 - A_q| in (0.02, 0.04) so old 0.02 floor would accept");
				Assert::IsFalse(mtRip.valid,
					L"FitMotionTolerantFade must reject 3% non-rigid ripple below 2x SNR gain floor (0.04)");
			}

			// 2. 6.5% fade-out under identical non-rigid motion: rigid still fails, FitMotionTolerantFade succeeds cleanly!
			{
				double ra[kGridH][kGridW], ca[kGridH][kGridW];
				double vx = 0.0, vy = 0.0;
				AlignGrid(gBase, gFade, kLocalRadius, ra, ca, &vx, &vy);
				Assert::IsFalse(FitGridFadeAligned(ra, ca, vx, vy).valid,
					L"precondition: rigid FitGridFadeAligned still fails on 6.5% fade under opposite-moving discs");
				const GridFadeFit mtFade = FitMotionTolerantFade(ra, ca, vx, vy);
				Assert::IsTrue(mtFade.valid && mtFade.dir < 0,
					L"FitMotionTolerantFade must accept 6.5% fade-out (> 0.04) under identical non-rigid motion");
			}
		}

		// HIGH-3 #2: Studio-white topPinned exclusion strips an interior 235-code clipped ceiling (where full-392 stepAQ > 1.0!)
		// without ever stripping unclipped flat-top scenes (< 234 codes).
		TEST_METHOD(TopPinnedStudioWhiteExcludesInteriorCeilingWithoutStrippingUnclippedFlatTop)
		{
			using namespace VpUnit::CutFadeJudge;

			constexpr int R = kLocalRadius;
			constexpr int kIntN = (kGridH - 2 * R) * (kGridW - 2 * R);
			auto Full392StepSlope = [&](const Record& prevR, const Record& curR) -> double {
				double gp[kGridH][kGridW], gc[kGridH][kGridW];
				LoadGridCodes(prevR.cells, gp);
				LoadGridCodes(curR.cells, gc);
				std::vector<double> ps, cs;
				ps.reserve(size_t(kIntN));
				cs.reserve(size_t(kIntN));
				for (int y = R; y < kGridH - R; ++y)
					for (int x = R; x < kGridW - R; ++x)
					{
						ps.push_back(gp[y][x]);
						cs.push_back(gc[y][x]);
					}
				std::sort(ps.begin(), ps.end());
				std::sort(cs.begin(), cs.end());
				double sx = 0.0, sy = 0.0;
				for (int i = 0; i < kIntN; ++i)
				{
					sx += ps[size_t(i)];
					sy += cs[size_t(i)];
				}
				const double mx = sx / double(kIntN), my = sy / double(kIntN);
				double sxx = 0.0, sxy = 0.0;
				for (int i = 0; i < kIntN; ++i)
				{
					const double dx = ps[size_t(i)] - mx;
					sxx += dx * dx;
					sxy += dx * (cs[size_t(i)] - my);
				}
				return sxx > 1e-6 ? sxy / sxx : 1.0;
			};

			// (a) Interior 235-code clipped patch (2x2 interior cells at x in [25..35), y in [20..30), strictly inside interior!)
			//     during fade-out: by g = 0.18 -> 0.15, full-392 sorted OLS gives stepAQ > 1.0 (negative control!),
			//     yet FoldFadeRun's studio-white topPinned exclusion succeeds cleanly (runFade == true)!
			{
				auto InteriorClippedFrame = [](double gain) -> Record {
					std::vector<uint16_t> half(size_t(kW) * kH);
					for (uint32_t y = 0; y < kH; ++y)
						for (uint32_t x = 0; x < kW; ++x)
						{
							double code = 16.0;
							if (x >= 25u && x < 35u && y >= 20u && y < 30u)
							{
								code = 16.0 + (std::min)(219.0, 10.0 * gain * 219.0);
							}
							else
							{
								const double pat = 0.28 + 0.60 * double(x) / double(kW - 1) + 0.08 * std::cos(double(y) * 0.14);
								code = (std::min)(235.0, 16.0 + gain * pat * 215.0);
							}
							half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(code * 256.0 + 0.5))));
						}
					return MakeRecordFromHalf(half);
				};

				std::vector<Record> recs;
				for (int i = 0; i < 4; ++i)
					recs.push_back(InteriorClippedFrame(1.0));
				// Smooth fade-out from 1.0 down to 0.12 in steps of 0.03 (includes 0.18 -> 0.15 and 0.15 -> 0.12).
				for (int s = 1; s <= 29; ++s)
					recs.push_back(InteriorClippedFrame(1.0 - 0.03 * double(s)));

				// Negative control: verify full-392 sorted OLS gives stepAQ > 1.0 on the late step (0.16 -> 0.13)
				// due to the interior 235-code ceiling lever arm!
				const size_t idxLate = 31u;   // recs[31] = 0.16 -> recs[32] = 0.13
				const double rawSlope = Full392StepSlope(recs[idxLate], recs[idxLate + 1u]);
				Assert::IsTrue(rawSlope > 1.0,
					L"Negative control: interior 235-code clipped patch must force full-392 stepAQ > 1.0 on late fade-out step");

				FadeRun run;
				bool caughtStepWithRawSlopeOverOne = false;
				for (size_t i = 1; i < recs.size(); ++i)
				{
					const Events e = ClassifyEvents(recs[i - 1], recs[i], false, false);
					const bool rf = FoldFadeRun(recs[i - 1], recs[i], e, run);
					if (i == idxLate + 1u)
						caughtStepWithRawSlopeOverOne = rf;
				}
				Assert::IsTrue(caughtStepWithRawSlopeOverOne,
					L"studio-white topPinned exclusion must allow FoldFadeRun to continue cleanly even when full-392 stepAQ > 1.0");
			}

			// (b) Unclipped flat-top scene (top plateau at 180 codes < 234) fading gently (~ -0.25 codes/frame):
			//     assert max interior cell < 234 (topPinned does NOT trigger) and FoldFadeRun continues cleanly!
			{
				auto UnclippedFlatTopFrame = [](double gain) -> Record {
					std::vector<uint16_t> half(size_t(kW) * kH);
					for (uint32_t y = 0; y < kH; ++y)
						for (uint32_t x = 0; x < kW; ++x)
						{
							const double raw = (x >= 100u) ? 180.0 : (24.0 + 156.0 * double(x) / 100.0);
							const double code = 16.0 + gain * (raw - 16.0);
							half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(code * 256.0 + 0.5))));
						}
					return MakeRecordFromHalf(half);
				};

				std::vector<Record> recs;
				for (int i = 0; i < 4; ++i)
					recs.push_back(UnclippedFlatTopFrame(1.0));
				for (int i = 1; i <= 40; ++i)
					recs.push_back(UnclippedFlatTopFrame(1.0 - 0.0025 * double(i)));   // ~ -0.25.. -0.41 codes/frame

				double gMax = 0.0;
				double g0[kGridH][kGridW];
				LoadGridCodes(recs[4].cells, g0);
				for (int y = R; y < kGridH - R; ++y)
					for (int x = R; x < kGridW - R; ++x)
						if (g0[y][x] > gMax)
							gMax = g0[y][x];
				Assert::IsTrue(gMax < kStudioPeakWhiteCodes - double(kFadeMinSpread),
					L"precondition: unclipped flat-top ceiling (180 codes) sits strictly below 234 studio-white guard");

				const auto res = Play(recs);
				Assert::AreEqual(0, Count(res, 1, 4), L"0 fades on pre-fade static unclipped flat-top");
				Assert::IsTrue(Count(res, 4, recs.size()) >= 30,
					L"gentle fade of unclipped 180-code flat-top must be detected cleanly without stripping its ceiling");
			}
		}

		// HIGH-3 #3: HighlightScalingConsistent restricts clipped-white bypass strictly to studio peak white (>= 234 codes)
		// and uses the derived half-window (0.125) for single-pair ClassifyEvents.
		TEST_METHOD(HighlightScalingConsistentRestrictsClippedBypassToStudioPeakWhiteAndSinglePairHalfWindow)
		{
			using namespace VpUnit::CutFadeJudge;

			// (a) Unclipped 150-code pinned highlight under 30% midtone drop (gain = 0.70, 1 - gain = 0.30 >= 0.25) -> FALSE,
			//     whereas exact same gain = 0.70 with studio peak white (235 codes >= 234) -> TRUE!
			const uint32_t p99Unclipped = static_cast<uint32_t>(150.0 * 256.0);
			const uint32_t p99StudioWhite = static_cast<uint32_t>(235.0 * 256.0);
			Assert::IsFalse(HighlightScalingConsistent(p99Unclipped, p99Unclipped, 0.70, 16.0, -1),
				L"unclipped 150-code pinned highlight must NOT receive clipped-white bypass even at 30% drop");
			Assert::IsTrue(HighlightScalingConsistent(p99StudioWhite, p99StudioWhite, 0.70, 16.0, -1),
				L"Negative control: exact same 30% drop with 235-code studio peak white MUST receive clipped-white bypass");

			// (b) Single-pair ClassifyEvents (e.fade): -16% clipped-white (235) fade-out (0.16 in [0.125, 0.25)) -> e.fade == true,
			//     while -4% clipped-white flicker (0.04 < 0.125) -> e.fade == false!
			auto ClippedPairFrame = [](double gain, uint32_t k) -> Record {
				std::vector<uint16_t> half(size_t(kW) * kH);
				for (uint32_t y = 0; y < kH; ++y)
					for (uint32_t x = 0; x < kW; ++x)
					{
						double code = 16.0;
						if (x < 10u && y < 36u)
							code = 235.0;
						else
						{
							const double pat = 0.50 + 0.22 * std::sin(double(x) * 0.18) * std::cos(double(y) * 0.16)
								+ 0.16 * std::cos(double(x) * 0.07 + double(y) * 0.09);
							code = 16.0 + gain * pat * 190.0 + double(Noise(x, y, k)) / (256.0 * 16.0);
						}
						half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(code * 256.0 + 0.5))));
					}
				return MakeRecordFromHalf(half);
			};

			const Record r100 = ClippedPairFrame(1.00, 9501u);
			const Record r084 = ClippedPairFrame(0.84, 9502u);   // -16% single-pair clipped-white fade-out
			const Record r096 = ClippedPairFrame(0.96, 9503u);   // -4% single-pair clipped-white flicker

			const Events eFade16 = ClassifyEvents(r100, r084, false, false);
			Assert::IsTrue(eFade16.fade,
				L"single-pair ClassifyEvents must accept -16% clipped-white fade-out via derived 0.125 half-window");
			const Events eFlicker4 = ClassifyEvents(r100, r096, false, false);
			Assert::IsFalse(eFlicker4.fade,
				L"Negative control: single-pair ClassifyEvents must STILL reject -4% clipped-white flicker (< 0.125)");
		}

		// HIGH-3 #4:
		// (a) StepStillMoving sub-LSB scoping: when |stepMean| > 0.5 code and sSameEps > sOppEps, sub-LSB opposing jitter
		//     making sSameZero < sOppZero does NOT block an active fade-out, whereas a near-zero plateau (|stepMean| < 0.05)
		//     with sSameZero < sOppZero IS rejected.
		// (b) Near-black collapse Part C exemption: active near-black fade-out collapse (mean < 48, gain < 0.25, corr2x2 < 0.4)
		//     succeeds with allowNearBlackCollapse = true and in FoldFadeRun, while allowNearBlackCollapse = false AND cold-start
		//     (activeFade == false) both reject!
		TEST_METHOD(StepStillMovingSubLsbScopingAndNearBlackCollapseExemption)
		{
			using namespace VpUnit::CutFadeJudge;

			constexpr int R = kLocalRadius;
			auto MakeGridRecord = [](const double cells[kGridH][kGridW]) -> Record {
				Record r;
				r.gridFound = 1;
				r.pixelCount = kW * kH;
				double sum = 0.0;
				std::vector<uint32_t> vals;
				vals.reserve(size_t(kGridW * kGridH));
				for (int y = 0; y < kGridH; ++y)
					for (int x = 0; x < kGridW; ++x)
					{
						const uint32_t p4 = static_cast<uint32_t>((std::min)(65535.0, (std::max)(0.0, std::floor(cells[y][x] * 256.0 + 0.5))));
						r.cells[y * kGridW + x] = p4;
						sum += double(p4);
						vals.push_back(p4);
						const uint32_t bin = (std::min)(VpUnit::SourceStats::kHistBins - 1u, p4 / kP4HistBinWidth);
						r.hist[bin] += 25u;
					}
				std::sort(vals.begin(), vals.end());
				r.mean = static_cast<uint32_t>(std::floor(sum / double(kGridW * kGridH) + 0.5));
				r.p01 = vals[vals.size() / 100];
				r.p50 = vals[vals.size() / 2];
				r.p99 = vals[(vals.size() * 99) / 100];
				return r;
			};

			// ---- Part (a): StepStillMoving sub-LSB scoping ----
			{
				double baseG[kGridH][kGridW], midG[kGridH][kGridW], prevG[kGridH][kGridW];
				for (int y = 0; y < kGridH; ++y)
					for (int x = 0; x < kGridW; ++x)
					{
						const double base = (x < 19)
							? (24.0 + 8.0 * double(x) / 18.0 + 2.0 * std::sin(double(y) * 0.6))
							: (100.0 + 70.0 * double(x - 19) / 12.0 + 8.0 * std::cos(double(y) * 0.5));
						baseG[y][x] = base;
						midG[y][x] = 16.0 + 0.90 * (base - 16.0);
						prevG[y][x] = 16.0 + 0.80 * (base - 16.0);
					}
				const Record rBase = MakeGridRecord(baseG);
				const Record rMid = MakeGridRecord(midG);
				const Record rPrev = MakeGridRecord(prevG);

				auto PrimeDarkRun = [&]() -> FadeRun {
					FadeRun run;
					const Events e1 = ClassifyEvents(rBase, rMid, false, false);
					FoldFadeRun(rBase, rMid, e1, run);
					const Events e2 = ClassifyEvents(rMid, rPrev, false, false);
					Assert::IsTrue(FoldFadeRun(rMid, rPrev, e2, run) && run.direction < 0,
						L"precondition: primed active darkening fade-out run");
					return run;
				};

				// Active step: bright 154 cells drop -3.0 codes while dim 238 cells jitter +0.04 code (sub-LSB):
				// |stepMean| > 0.5, sSameEps = 154 > sOppEps = 0, yet sSameZero = 154 < sOppZero = 238!
				double activeG[kGridH][kGridW];
				for (int y = 0; y < kGridH; ++y)
					for (int x = 0; x < kGridW; ++x)
						activeG[y][x] = (x < 19) ? (prevG[y][x] + 0.04) : (prevG[y][x] - 3.0);
				const Record rActive = MakeGridRecord(activeG);
				{
					FadeRun run = PrimeDarkRun();
					const Events eAct = ClassifyEvents(rPrev, rActive, false, false);
					Assert::IsTrue(FoldFadeRun(rPrev, rActive, eAct, run),
						L"active fade step with |stepMean| > 0.5 and sSameEps > sOppEps must continue despite sub-LSB jitter");
				}

				// Negative control: near-zero plateau (|stepMean| < 0.05) with 10 bright cells dropping -0.65 code
				// and 180 dim cells jittering +0.02 code -> sSameEps = 10 > 0 yet sSameZero = 10 < sOppZero = 180 -> REJECTED!
				double plateauG[kGridH][kGridW];
				for (int y = 0; y < kGridH; ++y)
					for (int x = 0; x < kGridW; ++x)
						plateauG[y][x] = prevG[y][x];
				int dropped = 0;
				for (int y = R; y < kGridH - R && dropped < 10; ++y)
				{
					plateauG[y][kGridW - R - 1] -= 0.65;
					++dropped;
				}
				int jittered = 0;
				for (int y = R; y < kGridH - R; ++y)
					for (int x = R; x < 19 && jittered < 180; ++x)
					{
						plateauG[y][x] += 0.02;
						++jittered;
					}
				const Record rPlateau = MakeGridRecord(plateauG);
				{
					FadeRun run = PrimeDarkRun();
					const Events ePlat = ClassifyEvents(rPrev, rPlateau, false, false);
					Assert::IsFalse(FoldFadeRun(rPrev, rPlateau, ePlat, run),
						L"Negative control: near-zero plateau (|stepMean| < 0.05) with sSameZero < sOppZero must be rejected");
				}
			}

			// ---- Part (b): Near-black collapse Part C exemption ----
			{
				double baseG[kGridH][kGridW], darkG[kGridH][kGridW];
				for (int y = 0; y < kGridH; ++y)
					for (int x = 0; x < kGridW; ++x)
					{
						const double pat = 0.45 + 0.25 * std::sin(double(x) * 0.5) * std::cos(double(y) * 0.6)
							+ 0.15 * std::cos(double(x) * 0.25);
						baseG[y][x] = 16.0 + pat * 160.0;
					}
				// Collapsed dark frame: exact 0.09 quantile scaling about 16, spatially inverted so corr2x2 < 0.4!
				for (int y = 0; y < kGridH; ++y)
					for (int x = 0; x < kGridW; ++x)
						darkG[y][x] = 16.0 + 0.09 * (baseG[kGridH - 1 - y][kGridW - 1 - x] - 16.0);

				// Direct API + Negative Control: allowNearBlackCollapse=false rejects (corr2x2 < 0.4), true accepts!
				Assert::IsFalse(FitMotionTolerantFade(baseG, darkG, 0.0, 0.0, false).valid,
					L"Negative control: FitMotionTolerantFade(allowNearBlackCollapse=false) must reject corr2x2 < 0.4");
				const GridFadeFit collapseFit = FitMotionTolerantFade(baseG, darkG, 0.0, 0.0, true);
				Assert::IsTrue(collapseFit.valid && collapseFit.dir < 0 && collapseFit.gain < 1.0 / double(kFadeMaxDiv),
					L"FitMotionTolerantFade(allowNearBlackCollapse=true) must accept active near-black collapse (gain < 0.25)");

				// Prime a smooth 12-step fade-out (1.0 -> 0.14, ~7%/step << 25% jump guard) so run is genuinely active
				// with prev.mean ~ 26 < 48 before feeding the spatially-scrambled collapsed dark frame (gain = 0.09).
				std::vector<Record> rampRecs;
				for (int s = 0; s <= 12; ++s)
				{
					const double g = 1.0 - 0.86 * double(s) / 12.0;
					double stepG[kGridH][kGridW];
					for (int y = 0; y < kGridH; ++y)
						for (int x = 0; x < kGridW; ++x)
							stepG[y][x] = 16.0 + g * (baseG[y][x] - 16.0);
					rampRecs.push_back(MakeGridRecord(stepG));
				}
				const Record& rPrev = rampRecs.back();
				const Record rDark = MakeGridRecord(darkG);

				// Active fade-out run -> nearBlackFall == true -> FoldFadeRun continues!
				{
					FadeRun run;
					bool activeBefore = false;
					for (size_t i = 1; i < rampRecs.size(); ++i)
					{
						const Events e = ClassifyEvents(rampRecs[i - 1], rampRecs[i], false, false);
						activeBefore = FoldFadeRun(rampRecs[i - 1], rampRecs[i], e, run);
					}
					Assert::IsTrue(activeBefore && run.direction < 0 && run.fadePairs >= 1u,
						L"precondition: fade-out run is active before near-black collapse");
					const Events eCol = ClassifyEvents(rPrev, rDark, false, false);
					Assert::IsTrue(FoldFadeRun(rPrev, rDark, eCol, run),
						L"active near-black fade-out collapse must continue in FoldFadeRun");
				}

				// Negative control: cold-start (activeFade == false) transition into noisy dark frame -> runFade == false!
				{
					FadeRun coldRun;
					const Events eCold = ClassifyEvents(rPrev, rDark, false, false);
					Assert::IsFalse(FoldFadeRun(rPrev, rDark, eCold, coldRun),
						L"Negative control: cold-start (activeFade == false) transition into noisy dark frame must NOT fire runFade");
				}
			}
		}

		// MEDIUM-3: 48-frame slow fade-in (gain 0.20 -> 1.00) WITH simultaneous horizontal pan (2 px/frame across > 4 cells,
		// triggering multiple midP99 -> baseP99 leapfrogs). Negative control proves stale frame-0 baseP99 fails HighlightScalingConsistent!
		TEST_METHOD(LongPanWithSimultaneousFadeInSyncsMidP99AcrossLeapfrogs)
		{
			using namespace VpUnit::CutFadeJudge;

			const int pre = 8, nIn = 48, post = 8;
			std::vector<Record> recs;
			recs.reserve(size_t(pre + nIn + post));
			for (int i = 0; i < pre + nIn + post; ++i)
			{
				const double gain = i < pre ? 0.20
					: (i < pre + nIn ? 0.20 + 0.80 * double(i - pre + 1) / double(nIn) : 1.00);
				// 2.0 px/frame = 0.4 cells/frame -> 48 frames pans 19.2 cells (> 4 cells, multiple midP99 -> baseP99 leapfrogs!).
				recs.push_back(TexturedPanFadePicture(gain, 0.4 * double(i), 0.0, uint32_t(8500 + i)));
			}

			// Mandatory In-Test Negative Control (Rule 3): verify that using stale initial frame-0 baseP99 against a late
			// fade-in frame (pre + 40) with its post-leapfrog gain (4 frames span) FAILS HighlightScalingConsistent!
			{
				const Record& rInitial = recs[size_t(pre - 1)];
				const Record& rLeapAnchor = recs[size_t(pre + 36)];
				const Record& rLate = recs[size_t(pre + 40)];
				const GridFadeFit leapFit = FitGridFade(rLeapAnchor.cells, rLate.cells, kLocalRadius);
				Assert::IsTrue(leapFit.valid && leapFit.dir > 0,
					L"precondition: post-leapfrog pan+fade-in pair has valid brightening fit");
				Assert::IsTrue(HighlightScalingConsistent(rLeapAnchor.p99, rLate.p99, leapFit.gain, leapFit.black, leapFit.dir),
					L"synced leapfrog anchor p99 passes HighlightScalingConsistent");
				Assert::IsFalse(HighlightScalingConsistent(rInitial.p99, rLate.p99, leapFit.gain, leapFit.black, leapFit.dir),
					L"Negative control: stale initial baseP99 fails HighlightScalingConsistent against post-leapfrog gain");
			}

			const auto res = Play(recs);
			Assert::AreEqual(0, Count(res, 1, size_t(pre)), L"0 false fades during pre-fade pure pan");
			Assert::IsTrue(Count(res, size_t(pre), size_t(pre + nIn)) >= 40,
				L"48-frame pan + slow fade-in must hit >= 40/48 frames across multiple midP99 leapfrogs");
			Assert::AreEqual(0, Count(res, size_t(pre + nIn), recs.size()), L"0 false fades during post-fade pure pan");
		}
	};
}
