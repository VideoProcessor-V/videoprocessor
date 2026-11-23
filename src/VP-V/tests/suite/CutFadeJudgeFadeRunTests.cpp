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
		TEST_METHOD(UnitVersionIsFour)
		{
			Assert::AreEqual(4u, VpUnit::CutFadeJudge::kUnitVersion, L"the 576-cell spatial affine fade law is a registered law change (4, after v3 multi-frame fade and v2 T2 sequence law)");
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
	};
}
