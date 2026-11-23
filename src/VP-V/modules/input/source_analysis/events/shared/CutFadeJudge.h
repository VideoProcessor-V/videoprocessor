// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared unit U7 "cut / fade / flash judge" (doc 62 section 1.2): the one decision of "the picture changed" derived from two
// P4 records, and the veto detector's grid + histogram cut rule. Shared by the lane (ClassifyEvents), SkeletonModule, and
// HdrVetoCutDetector::Observe so every consumer calls ONE implementation. Cut/fade/flash are never stored: every consumer derives them.
//
// First-principles 576-cell spatial affine fade law (owner ruling 2026-11-22 16:53 EST; zero fitted constants):
//   - Decoupled global translation alignment (integer + symmetric sub-cell Gauss-Newton / Lucas-Kanade) over the 32x18 P4 cell grid
//   - Global OLS affine fit cur ~= A * ref_aligned + c with implied black b = c / (1 - A) in (kFadeMinBlack, kFadeMaxBlack)
//   - Motion-compensated L2 variance reduction over litCells: V_fade < 0.5 * V_id (R^2_delta > 0.50, with sqrt(V_id) > 0.5 code)
//   - Motion-compensated spatial consensus (>= 75% of changing lit cells) and whole-picture coverage (>= 50% of lit cells)
//   - Gridless fallback preserves the M4 (mean, spread) pair/run laws for tiny planes (< 32x18) and synthetic gridless records.
//
// Header-only, C++14, no Windows or GPU headers.
#pragma once

#include <VP-V/core/ComputeOnce.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace VpUnit
{
namespace CutFadeJudge
{
// Bump on ANY change of a threshold, a rule above or the meaning of an output.
constexpr uint32_t kUnitVersion = 4;

constexpr double kCutHistFrac = 0.50;          // normalised L1 distance / 2 above this: a different picture
constexpr double kCutPossibleHistFrac = 0.25;  // above this (or a continuity slip): a cut is possible
constexpr int64_t kFadeMinDiv = 64;            // fade: |mean step| >= spread / 64 ...
constexpr int64_t kFadeMaxDiv = 4;             // ... and < spread / 4 (more is a jump, not a ramp)
constexpr int64_t kFlashMeanDiv = 4;           // flash: |mean step| >= spread / 4 with the spread kept within spread / 4

// The fade law's numbers, one set for the CPU twin and the kernel text (FadeKernelBody carries the same literals; the pinned
// kernel hash and FadeKernelCarriesTheLawsNumbers in SharedUnitsW1Tests tie the two).
constexpr float kFadeMinStepCodes = 0.75f;   // |mean step| window per frame at the reference rate (the caller scales it to the source rate)
constexpr float kFadeMaxStepCodes = 24.0f;
constexpr float kFadeMinBlack = -12.0f;      // plausible implied black level (codes)
constexpr float kFadeMaxBlack = 48.0f;
constexpr float kFadeScaleEps = 0.01f;       // |1 - g| at or below this: the spread did not scale (a pan), no implied black
constexpr float kFadeGainMin = 0.5f;         // gain = clamp(sa / sb, min, max)
constexpr float kFadeGainMax = 2.0f;
constexpr float kFadeMinSpread = 1.0f;       // both frames' spread (codes) must exceed this
constexpr float kFadeDivGuard = 1e-3f;       // floor of a spread used as a divisor
constexpr float kFadeNoBlack = 1e9f;         // the implied black when the spread did not scale

constexpr double kFadeRunMinGain = 0.02;      // |1 - gain| of the run (first record -> this one) above this: the picture was scaled
constexpr double kP4CodesPerEightBit = 256.0; // P4 records hold 16-bit half-plane codes: one 8-bit-like code = 256 P4 codes
constexpr uint32_t kFadeRunMinPairs = 2;      // a run needs two pairs (as FadeFit needs the previous pair's step): one pair is not a ramp

// Derived P4-scale constants (16-bit half-plane codes).
constexpr uint32_t kP4HistBinWidth = 65536u / SourceStats::kHistBins;                // 64: 1 10-bit P4 histogram bin
constexpr uint32_t kBlackSpreadP4 = uint32_t(kFadeMinSpread * kP4CodesPerEightBit);  // 256: 1 8-bit code
constexpr uint32_t kBlackMeanMaxP4 = uint32_t(kFadeMaxBlack * kP4CodesPerEightBit);  // 12288: 48 codes
constexpr int64_t kMaxFadeStepP4 = int64_t(kFadeMaxStepCodes * kP4CodesPerEightBit); // 6144: 24 codes
constexpr int64_t kMinFadeStepP4 = int64_t(kFadeMinStepCodes * kP4CodesPerEightBit); // 192: 0.75 codes
constexpr double kBlackMinP4 = double(kFadeMinBlack) * kP4CodesPerEightBit;          // -3072: -12 codes
constexpr double kBlackMaxP4 = double(kFadeMaxBlack) * kP4CodesPerEightBit;          // 12288: 48 codes

// Elementary textbook ratios for the 576-cell spatial affine fade law.
constexpr double kFadeResidualReduction = 0.5;                // 50% reduction bar: lit-cell L2 variance explained > 50% (V_fade < 0.5 * V_id) & AlignGrid shift-improvement check
constexpr double kFadeSpatialConsensus = 0.75;                // >= 3/4 of changing lit cells move in the fade direction
constexpr double kFadeSpatialCoverage = 0.5;                  // >= 1/2 of lit cells participate in the fade direction & above-LSB StepStillMoving majority
constexpr double kFadeCellStepEps = 0.5 * double(kFadeMinSpread); // 0.5 8-bit code = half-LSB quantization floor

// ---- the record-sequence cut law "T2" (U7-accuracy, v2) constants & grid helpers -----------------------------------------
constexpr int kGridW = int(SourceStats::kGridCols);
constexpr int kGridH = int(SourceStats::kGridRows);
constexpr int kLocalRadius = 2;                // L: the +-cell neighbourhood
constexpr int kShiftRadius = 1;                // S: the +-cell shifts
constexpr double kGridScale = 1.0 / 256.0;     // 16-bit cell codes to the grid scale of the thresholds
constexpr double kLogOffset = 4.0;             // log(g + 4)
constexpr double kSpikeL = 6.0;
constexpr double kSpikeRatio = 1.8;
constexpr double kSpikeEps = 1.0;
constexpr double kLowTierL = 4.0;
constexpr double kLowTierS = 0.2;
constexpr double kHardH = 0.5;
constexpr double kHardG = 6.0;
constexpr double kSameStructure = 0.4;
constexpr double kFlatF = 0.02;
constexpr double kReturnFraction = 3.0;        // flash: the k-1 -> k+1 return under a third of the step
constexpr int kBackFrames = 3;
constexpr int kRefFrames = 4;

struct Grid {
    double g[kGridH][kGridW];
    double hn[SourceStats::kHistBins];
    bool valid = false;
};

inline void GridOf(const SourceStats::Record& r, Grid* out)
{
    out->valid = r.gridFound != 0;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            out->g[y][x] = double(r.cells[y * kGridW + x]) * kGridScale;
    double total = 0.0;
    for (uint32_t i = 0; i < SourceStats::kHistBins; ++i)
        total += double(r.hist[i]);
    const double inv = total > 0.0 ? 1.0 / total : 0.0;
    for (uint32_t i = 0; i < SourceStats::kHistBins; ++i)
        out->hn[i] = double(r.hist[i]) * inv;
}

inline double MeanCellDifference(const Grid& a, const Grid& b)
{
    double s = 0.0;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            s += std::fabs(a.g[y][x] - b.g[y][x]);
    return s / double(kGridH * kGridW);
}

inline double HistHalfL1(const Grid& a, const Grid& b)
{
    double s = 0.0;
    for (uint32_t i = 0; i < SourceStats::kHistBins; ++i)
        s += std::fabs(a.hn[i] - b.hn[i]);
    return 0.5 * s;
}

// L and the fitted gain A of prev -> cur.
inline void LocalResidual(const Grid& prev, const Grid& cur, double* residual, double* gain)
{
    const double n = double(kGridH * kGridW);
    double sx = 0.0, sy = 0.0;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x) {
            sx += prev.g[y][x];
            sy += cur.g[y][x];
        }
    const double mx = sx / n, my = sy / n;
    double sxx = 0.0, sxy = 0.0;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x) {
            const double dx = prev.g[y][x] - mx;
            sxx += dx * dx;
            sxy += dx * (cur.g[y][x] - my);
        }
    const double a = sxx > 1e-12 ? sxy / sxx : 0.0;
    const double c = my - a * mx;
    double s = 0.0;
    for (int y = kLocalRadius; y < kGridH - kLocalRadius; ++y)
        for (int x = kLocalRadius; x < kGridW - kLocalRadius; ++x) {
            double best = 1e9;
            for (int dy = -kLocalRadius; dy <= kLocalRadius; ++dy)
                for (int dx = -kLocalRadius; dx <= kLocalRadius; ++dx) {
                    const double d = std::fabs(cur.g[y][x] - (a * prev.g[y + dy][x + dx] + c));
                    if (d < best)
                        best = d;
                }
            s += best;
        }
    *residual = s / double((kGridH - 2 * kLocalRadius) * (kGridW - 2 * kLocalRadius));
    *gain = sxx > 1e-12 ? a : 1.0;
}

struct Gradients {
    double h[kGridH][kGridW - 1];
    double v[kGridH - 1][kGridW];
};

inline void GradientsOf(const Grid& a, Gradients* out)
{
    double lg[kGridH][kGridW];
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            lg[y][x] = std::log(a.g[y][x] + kLogOffset);
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x + 1 < kGridW; ++x)
            out->h[y][x] = lg[y][x + 1] - lg[y][x];
    for (int y = 0; y + 1 < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            out->v[y][x] = lg[y + 1][x] - lg[y][x];
}

inline double Flatness(const Grid& a)
{
    Gradients d;
    GradientsOf(a, &d);
    double s = 0.0;
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x + 1 < kGridW; ++x)
            s += d.h[y][x] * d.h[y][x];
    for (int y = 0; y + 1 < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            s += d.v[y][x] * d.v[y][x];
    return std::sqrt(s / double(kGridH * (kGridW - 1) + (kGridH - 1) * kGridW));
}

// ---- 576-cell spatial affine fade helpers -------------------------------------------------------------------------------

struct GridFadeFit {
    bool valid = false;
    int dir = 0;
    double gain = 1.0;
    double black = double(kFadeNoBlack);
    double eId = 0.0;
    double eFade = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    int litCells = 0;
    int sameSign = 0;
    int oppSign = 0;
};

template <typename CellT>
inline void LoadGridCodes(const CellT* cells, double out[kGridH][kGridW])
{
    for (int y = 0; y < kGridH; ++y)
        for (int x = 0; x < kGridW; ++x)
            out[y][x] = double(cells[y * kGridW + x]) * kGridScale;
}

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
        for (int x = R; x < kGridW - R; ++x) {
            sx += aligned[y][x];
            sy += cur[y][x];
        }
    const double mx = sx / kN;
    const double my = sy / kN;
    double sxx = 0.0, syy = 0.0, sxy = 0.0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
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
        for (int x = R; x < kGridW - R; ++x) {
            refS[y][x] = BilinearGrid(ref, double(y + iy) + hfy, double(x + ix) + hfx);
            curS[y][x] = BilinearGrid(cur, double(y) - hfy, double(x) - hfx);
        }
}

// Decoupled global translation alignment (integer Pearson correlation + sub-cell Lucas-Kanade).
// Uses exact integer shift (iy, ix) plus symmetric fractional half-shifts (+0.5*f on ref, -0.5*f on cur)
// so both grids experience identical bilinear frequency response with zero smoothing at integer shifts.
inline void AlignGrid(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW], int searchRadius,
                      double refAligned[kGridH][kGridW], double curAligned[kGridH][kGridW],
                      double* outVx, double* outVy)
{
    constexpr int R = kLocalRadius;
    constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));
    const int rad = (std::min)(searchRadius, R);

    // Step 1a: evaluate (0, 0) baseline, then search integer shifts [-rad .. +rad]^2 by Pearson correlation.
    double candRef[kGridH][kGridW], candCur[kGridH][kGridW];
    SampleInteriorPairShifted(ref, cur, 0, 0, 0.0, 0.0, refAligned, curAligned);
    double a0 = 0.0, c0 = 0.0, sxx0 = 0.0, corr0 = 0.0, eFade0 = 0.0;
    EvalInteriorAffine(refAligned, curAligned, &a0, &c0, &sxx0, &corr0, &eFade0);

    int bestSy = 0, bestSx = 0;
    double bestCorr = corr0;
    double bestEFade = eFade0;

    if (sxx0 > 1e-6) {
        for (int sy = -rad; sy <= rad; ++sy) {
            for (int sx = -rad; sx <= rad; ++sx) {
                if (sy == 0 && sx == 0)
                    continue;
                SampleInteriorPairShifted(ref, cur, sy, sx, 0.0, 0.0, candRef, candCur);
                double a = 0.0, c = 0.0, sxx = 0.0, corr = 0.0, eFade = 0.0;
                EvalInteriorAffine(candRef, candCur, &a, &c, &sxx, &corr, &eFade);
                if (sxx <= 1e-6)
                    continue;
                // Only depart from stationary (0, 0) if the shift genuinely improves the affine fit
                // (prevents noise from choosing an arbitrary shift along a degenerate 1D linear ramp;
                // 1 - corr check is scale-invariant for sparse bright regions on black backgrounds).
                if (corr > bestCorr &&
                    (eFade < eFade0 - kFadeCellStepEps || eFade < kFadeResidualReduction * eFade0 ||
                     (1.0 - corr) < kFadeResidualReduction * (1.0 - corr0))) {
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

    // Step 1b: sub-cell Gauss-Newton / Lucas-Kanade refinement clamped to [-0.5, +0.5]^2 around (bestSx, bestSy).
    // Uses exact 1-cell centered bilinear differences (+-0.5) so sharp edges do not suffer 2x central-difference overshoot.
    constexpr int kAlignMaxIters = 4;
    for (int iter = 0; iter < kAlignMaxIters; ++iter) {
        double a = 0.0, c = 0.0, sxx = 0.0, corr = 0.0, ef = 0.0;
        EvalInteriorAffine(refAligned, curAligned, &a, &c, &sxx, &corr, &ef);
        if (sxx <= 1e-6 || !(a > 1e-6))
            break;

        const double hfy = 0.5 * fy;
        const double hfx = 0.5 * fx;
        double jxArr[kGridH][kGridW], jyArr[kGridH][kGridW];
        double sumJx = 0.0, sumJy = 0.0;
        for (int y = R; y < kGridH - R; ++y) {
            for (int x = R; x < kGridW - R; ++x) {
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
        for (int y = R; y < kGridH - R; ++y) {
            for (int x = R; x < kGridW - R; ++x) {
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
        if (det > 1e-6 * trace * trace && trace > 1e-6) {
            du = (h22 * b1 - h12 * b2) / det;
            dv = (h11 * b2 - h12 * b1) / det;
        } else if (h11 > 1e-6 && h11 >= h22) {
            du = b1 / h11;
        } else if (h22 > 1e-6) {
            dv = b2 / h22;
        } else {
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
            for (int x = R; x < kGridW - R; ++x) {
                refAligned[y][x] = candRef[y][x];
                curAligned[y][x] = candCur[y][x];
            }
    }

    if (outVx)
        *outVx = double(bestSx) + fx;
    if (outVy)
        *outVy = double(bestSy) + fy;
}

inline GridFadeFit FitGridFadeGrid(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW], int searchRadius)
{
    constexpr int R = kLocalRadius;
    constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));
    GridFadeFit fit;

    double refAligned[kGridH][kGridW], curAligned[kGridH][kGridW];
    AlignGrid(ref, cur, searchRadius, refAligned, curAligned, &fit.vx, &fit.vy);

    // Step 2: Global OLS affine fit curAligned ~= A * refAligned + c over interior cells.
    double sx = 0.0, sy = 0.0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            sx += refAligned[y][x];
            sy += curAligned[y][x];
        }
    const double mx = sx / kN;
    const double my = sy / kN;
    double sxx = 0.0, sxy = 0.0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
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

    // Step 3 & 4: Evaluate exact OLS L2 variance explained (> 50%: V_fade < 0.5 * V_id),
    // spatial consensus (>= 75%), and whole-picture coverage (>= 50%) over litCells
    // (invariant to letterbox bars and black backgrounds).
    // Floor lit-cell classification by the frame's own minimum interior code so tiny OLS black-level noise
    // below a constant black background (e.g. b=14 vs black=16) never pulls pure-black cells into litCells.
    double minCell = double(kFadeNoBlack);
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
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
        for (int x = R; x < kGridW - R; ++x) {
            if ((std::max)(refAligned[y][x], curAligned[y][x]) > litFloor + double(kFadeMinSpread)) {
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

template <typename RefT, typename CurT>
inline GridFadeFit FitGridFade(const RefT* refCells, const CurT* curCells, int searchRadius)
{
    double ref[kGridH][kGridW], cur[kGridH][kGridW];
    LoadGridCodes(refCells, ref);
    LoadGridCodes(curCells, cur);
    return FitGridFadeGrid(ref, cur, searchRadius);
}

// Count interior cells brightening / darkening beyond half-LSB between two grids (for black-boundary monotonicity checks).
inline void CountCellStepSigns(const uint32_t* prevCells, const uint32_t* curCells, int* outBrightened, int* outDarkened)
{
    constexpr int R = kLocalRadius;
    int brightened = 0, darkened = 0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            const double delta = (double(curCells[y * kGridW + x]) - double(prevCells[y * kGridW + x])) * kGridScale;
            if (delta > kFadeCellStepEps)
                ++brightened;
            else if (delta < -kFadeCellStepEps)
                ++darkened;
        }
    *outBrightened = brightened;
    *outDarkened = darkened;
}

struct EventSignals {
    bool cut = false;
    bool cutPossible = false;
    bool fade = false;
    bool flash = false;
    uint32_t histDistance = 0;   // normalised L1 distance, 0..65535 (65535 = disjoint histograms)
};

inline uint32_t HistDistance(const SourceStats::Record& a, const SourceStats::Record& b)
{
    const double na = a.pixelCount > 0 ? double(a.pixelCount) : 1.0;
    const double nb = b.pixelCount > 0 ? double(b.pixelCount) : 1.0;
    const double frac = SourceStats::HalfL1Distance<double>(a.hist, b.hist, SourceStats::kHistBins, na, nb);   // 0..1
    const double v = frac * 65535.0 + 0.5;
    return v >= 65535.0 ? 65535u : uint32_t(v);
}

inline EventSignals ClassifyEvents(const SourceStats::Record& prev, const SourceStats::Record& cur, bool fullBetween,
                                   bool continuityBetween)
{
    VPV_COMPUTE_ONCE("U7", ::ComputeOnce::Variant(fullBetween ? 1 : 0, continuityBetween ? 1 : 0));
    EventSignals e;
    e.histDistance = HistDistance(prev, cur);
    const double frac = double(e.histDistance) / 65535.0;
    if (fullBetween) {
        e.cut = true;
        e.cutPossible = true;
        return e;
    }
    if (continuityBetween)
        e.cutPossible = true;
    if (frac > kCutPossibleHistFrac)
        e.cutPossible = true;

    const uint32_t spreadPrev = prev.p99 > prev.p01 ? prev.p99 - prev.p01 : 0u;
    const uint32_t spreadCur = cur.p99 > cur.p01 ? cur.p99 - cur.p01 : 0u;
    const int64_t spread = int64_t(std::max<uint32_t>(1u, std::max(spreadPrev, spreadCur)));
    const int64_t dMean = int64_t(cur.mean) - int64_t(prev.mean);
    const int64_t aMean = dMean < 0 ? -dMean : dMean;
    const int64_t dSpread = int64_t(spreadCur) - int64_t(spreadPrev);
    const int64_t aSpread = dSpread < 0 ? -dSpread : dSpread;
    const bool sameSign = (dMean > 0 && dSpread > 0) || (dMean < 0 && dSpread < 0);

    if (frac > kCutHistFrac) {
        // A shifted histogram of unchanged spread is a flash (or a cut between look-alike
        // scenes: reported as a flash with cutPossible); any other jump is a cut.
        if (aMean * kFlashMeanDiv >= spread && aSpread * 4 <= spread) {
            e.flash = true;
            e.cutPossible = true;
        } else {
            e.cut = true;
            e.cutPossible = true;
        }
        return e;
    }

    if (prev.gridFound && cur.gridFound) {
        if (spreadPrev > kBlackSpreadP4 && spreadCur > kBlackSpreadP4 &&
            aMean * kFadeMaxDiv < spread && aSpread * kFadeMaxDiv < spread) {
            Grid gp, gc;
            GridOf(prev, &gp);
            GridOf(cur, &gc);
            if (Flatness(gp) >= kFlatF && Flatness(gc) >= kFlatF) {
                const GridFadeFit fit = FitGridFade(prev.cells, cur.cells, kShiftRadius);
                e.fade = fit.valid && std::fabs(1.0 - fit.gain) < 1.0 / double(kFadeMaxDiv);
            }
        }
        return e;
    }

    // Gridless fallback (tiny planes < 32x18 or synthetic gridless records): M4 (mean, spread) pair law.
    if (sameSign && spreadPrev > kBlackSpreadP4 && spreadCur > kBlackSpreadP4 &&
        aMean * kFadeMaxDiv < spread && aSpread * kFadeMaxDiv < spread && aSpread * kFadeMinDiv >= spread) {
        const double g = double(spreadCur) / double(spreadPrev);
        if (std::fabs(1.0 - g) > kFadeRunMinGain) {
            const double black = (double(cur.mean) - g * double(prev.mean)) / (1.0 - g);
            if (black > kBlackMinP4 && black < kBlackMaxP4)
                e.fade = true;
        }
    }
    return e;
}

enum class Strength { None, Minor, Full };

inline Strength StrengthOf(const EventSignals& e) { return e.cut ? Strength::Full : e.cutPossible ? Strength::Minor : Strength::None; }

// ---- the record-sequence cut law "T2" (U7-accuracy, v2) ------------------------------------------------------------------
inline double ShiftedCorrelation(const Gradients& p, const Gradients& c, int sy, int sx)
{
    struct Sums {
        double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0, n = 0.0;
        void Add(double va, double vb)
        {
            sa += va;
            sb += vb;
            saa += va * va;
            sbb += vb * vb;
            sab += va * vb;
            n += 1.0;
        }
    };
    Sums hs, vs;
    const int py0 = sy > 0 ? sy : 0, cy0 = sy < 0 ? -sy : 0;
    const int px0 = sx > 0 ? sx : 0, cx0 = sx < 0 ? -sx : 0;
    const int ay = sy < 0 ? -sy : sy, ax = sx < 0 ? -sx : sx;
    for (int y = 0; y < kGridH - ay; ++y)
        for (int x = 0; x < kGridW - 1 - ax; ++x)
            hs.Add(p.h[py0 + y][px0 + x], c.h[cy0 + y][cx0 + x]);
    for (int y = 0; y < kGridH - 1 - ay; ++y)
        for (int x = 0; x < kGridW - ax; ++x)
            vs.Add(p.v[py0 + y][px0 + x], c.v[cy0 + y][cx0 + x]);
    double va = 0.0, vb = 0.0, cov = 0.0;
    const Sums* parts[2] = { &hs, &vs };
    for (const Sums* s : parts) {
        if (s->n <= 0.0)
            continue;
        va += s->saa - s->sa * s->sa / s->n;
        vb += s->sbb - s->sb * s->sb / s->n;
        cov += s->sab - s->sa * s->sb / s->n;
    }
    const double d = std::sqrt((va > 0.0 ? va : 0.0) * (vb > 0.0 ? vb : 0.0));
    return d > 1e-12 ? cov / d : 0.0;
}

inline double StructureCorrelation(const Grid& prev, const Grid& cur)
{
    Gradients p, c;
    GradientsOf(prev, &p);
    GradientsOf(cur, &c);
    double best = -1.0;
    for (int sy = -kShiftRadius; sy <= kShiftRadius; ++sy)
        for (int sx = -kShiftRadius; sx <= kShiftRadius; ++sx) {
            const double r = ShiftedCorrelation(p, c, sy, sx);
            if (r > best)
                best = r;
        }
    return best;
}

struct PairScore {
    double L = 0.0, A = 1.0, G = 0.0, H = 0.0, S = 0.0;
};

inline PairScore ScorePair(const Grid& prev, const Grid& cur)
{
    PairScore s;
    LocalResidual(prev, cur, &s.L, &s.A);
    s.G = MeanCellDifference(prev, cur);
    s.H = HistHalfL1(prev, cur);
    s.S = StructureCorrelation(prev, cur);
    return s;
}

struct CutVerdict {
    uint64_t seq = 0;
    bool known = false;
    bool confirmed = false;
    bool cut = false;
    bool flash = false;
    bool candidate = false;
    bool partStart = false;
    bool gridLaw = false;
    float strength = 0.0f;
    float bar = 0.0f;
    PairScore score;
};

class CutTracker {
public:
    void Reset()
    {
        count_ = 0;
        hasPending_ = false;
    }

    CutVerdict Push(uint64_t seq, const SourceStats::Record& rec)
    {
        Entry next;
        next.seq = seq;
        GridOf(rec, &next.grid);
        next.fallbackRec = rec.gridFound == 0;
        if (next.fallbackRec)
            next.rec = rec;
        next.F = next.grid.valid ? Flatness(next.grid) : 1.0;
        next.flat = next.grid.valid && next.F < kFlatF;
        CutVerdict out;
        if (hasPending_) {
            const bool contiguous = seq == pending_.seq + 1u;
            if (contiguous)
                ScoreAgainst(pending_, &next);
            out = Judge(&pending_, contiguous ? &next : nullptr);
            out.confirmed = true;
            pending_.verdict.confirmed = true;
            Commit(pending_);
            if (!contiguous)
                count_ = 0;
        }
        next.partStart = count_ == 0;
        pending_ = next;
        hasPending_ = true;
        if (pending_.partStart)
            pending_.verdict = StartVerdict(pending_.seq);
        return out;
    }

    CutVerdict Provisional()
    {
        if (!hasPending_)
            return CutVerdict();
        Entry copy = pending_;
        return Judge(&copy, nullptr);
    }

private:
    struct Entry {
        uint64_t seq = 0;
        Grid grid;
        SourceStats::Record rec;
        bool fallbackRec = false;
        bool partStart = false;
        bool flat = false;
        double F = 1.0;
        PairScore score;
        bool scored = false;
        CutVerdict verdict;
    };
    static constexpr int kHistory = kRefFrames + 1;

    static CutVerdict StartVerdict(uint64_t seq)
    {
        CutVerdict v;
        v.seq = seq;
        v.known = true;
        v.partStart = true;
        return v;
    }

    const Entry* Back(int i) const
    {
        return i >= 1 && i <= count_ ? &history_[(head_ + kHistory - i) % kHistory] : nullptr;
    }

    void ScoreAgainst(const Entry& prev, Entry* cur) const
    {
        if (prev.grid.valid && cur->grid.valid) {
            cur->score = ScorePair(prev.grid, cur->grid);
            cur->scored = true;
        }
    }

    void Commit(const Entry& e)
    {
        head_ = (head_ + 1) % kHistory;
        history_[(head_ + kHistory - 1) % kHistory] = e;
        if (count_ < kHistory)
            ++count_;
    }

    static bool Lit(const Entry& j, const Entry* before)
    {
        return j.scored && j.score.S >= kSameStructure && !j.flat && before != nullptr && !before->flat;
    }

    CutVerdict Judge(Entry* k, const Entry* ahead) const
    {
        if (k->partStart) {
            k->verdict = StartVerdict(k->seq);
            k->verdict.confirmed = true;
            return k->verdict;
        }
        CutVerdict v;
        v.seq = k->seq;
        v.known = true;
        v.confirmed = ahead != nullptr;
        const Entry* prev = Back(1);
        if (!k->scored || prev == nullptr) {
            if (prev != nullptr && k->fallbackRec == prev->fallbackRec && k->fallbackRec) {
                const EventSignals e = ClassifyEvents(prev->rec, k->rec, false, false);
                v.cut = e.cut;
                v.flash = e.flash;
                v.candidate = e.cutPossible;
                v.strength = e.cut ? 1.0f : 0.0f;
            }
            v.confirmed = true;
            k->verdict = v;
            return v;
        }
        v.gridLaw = true;
        v.score = k->score;
        const double L = k->score.L;
        double ctx = 0.0;
        bool anyCtx = false;
        for (int i = 1; i <= kBackFrames; ++i) {
            const Entry* j = Back(i);
            if (j == nullptr || j->partStart)
                break;
            if (!j->scored || j->verdict.cut || j->verdict.flash || Lit(*j, Back(i + 1)))
                continue;
            ctx = anyCtx && ctx >= j->score.L ? ctx : j->score.L;
            anyCtx = true;
        }
        PairScore aheadScore;
        bool aheadScored = false;
        if (ahead != nullptr && ahead->scored) {
            aheadScore = ahead->score;
            aheadScored = true;
            const bool aheadLit = aheadScore.S >= kSameStructure && !ahead->flat && !k->flat;
            if (!aheadLit) {
                ctx = anyCtx && ctx >= aheadScore.L ? ctx : aheadScore.L;
                anyCtx = true;
            }
        }
        const double bar = kSpikeRatio * ctx + kSpikeEps;
        v.bar = float(bar);
        bool spike = L >= kSpikeL && L >= bar;
        if (!spike && L >= kLowTierL && L >= bar && !k->flat && !prev->flat && k->score.S < kLowTierS)
            spike = true;
        const bool hard = k->score.H > kHardH && k->score.G >= kHardG;
        v.strength = L >= kSpikeL ? float(L / (bar + 1e-9) < 1.0 ? L / (bar + 1e-9) : 1.0) : 0.0f;
        if (!(spike || hard)) {
            k->verdict = v;
            return v;
        }
        // Emergence of a genuine brightening fade-in from flat dark/black: prev is flat dark (< kFadeMaxBlack),
        // prev -> k brightens monotonically within a single-frame fade step (< kFadeMaxStepCodes) with zero interior
        // cells darkening > kFadeCellStepEps, and k -> ahead is a verified 576-cell affine brightening fade.
        if (prev->flat && ahead != nullptr && ahead->grid.valid) {
            double prevMean = 0.0, kMean = 0.0;
            for (int y = 0; y < kGridH; ++y)
                for (int x = 0; x < kGridW; ++x) {
                    prevMean += prev->grid.g[y][x];
                    kMean += k->grid.g[y][x];
                }
            prevMean /= double(kGridH * kGridW);
            kMean /= double(kGridH * kGridW);
            if (prevMean < double(kFadeMaxBlack) && kMean > prevMean && (kMean - prevMean) < double(kFadeMaxStepCodes)) {
                int darkened = 0;
                for (int y = kLocalRadius; y < kGridH - kLocalRadius; ++y)
                    for (int x = kLocalRadius; x < kGridW - kLocalRadius; ++x)
                        if (k->grid.g[y][x] - prev->grid.g[y][x] < -kFadeCellStepEps)
                            ++darkened;
                if (darkened == 0) {
                    const GridFadeFit aheadFit = FitGridFadeGrid(k->grid.g, ahead->grid.g, kShiftRadius);
                    if (aheadFit.valid && aheadFit.dir > 0) {
                        k->verdict = v;
                        return v;
                    }
                }
            }
        }
        v.candidate = true;
        int refIndex = 1;
        while (refIndex < kRefFrames) {
            const Entry* r = Back(refIndex);
            if (r == nullptr || !r->flat || r->partStart)
                break;
            ++refIndex;
        }
        const Entry* ref = Back(refIndex);
        if (ref == nullptr)
            ref = prev;
        const bool prevFlash = prev->verdict.flash;
        if (k->flat) {
            if (ahead != nullptr && ahead->grid.valid && !ahead->flat && !ref->flat && ref->grid.valid
                && StructureCorrelation(ref->grid, ahead->grid) >= kSameStructure) {
                v.flash = true;
            } else if (!prevFlash) {
                v.cut = true;
            }
        } else if (!ref->flat && ref->grid.valid
                   && (ref == prev ? k->score.S : StructureCorrelation(ref->grid, k->grid)) >= kSameStructure) {
            v.flash = true;
        } else if (aheadScored && ahead->grid.valid
                   && MeanCellDifference(prev->grid, ahead->grid) * kReturnFraction < k->score.G
                   && HistHalfL1(prev->grid, ahead->grid) * kReturnFraction < k->score.H) {
            v.flash = true;
        } else if (!(prevFlash && !prev->flat)) {
            v.cut = true;
        }
        if (v.cut)
            v.strength = 1.0f;
        k->verdict = v;
        return v;
    }

    Entry history_[kHistory];
    int head_ = 0;
    int count_ = 0;
    Entry pending_;
    bool hasPending_ = false;
};

// ---- the soft reset on a high-confidence non-cut candidate (cut_soft_reset; C5 batch B) --------------------------------------
constexpr float kSoftResetBar = 1.0f;
constexpr float kSoftResetDepth = 0.5f;

inline float SoftResetStrength(bool confirmed, bool candidate, bool cut, bool flash, bool fade, float strength)
{
    if (!confirmed || !candidate || cut || flash || fade || !(strength >= kSoftResetBar))
        return 0.0f;
    return strength < 1.0f ? strength : 1.0f;
}

inline float SoftResetKeep(float softStrength)
{
    const float s = softStrength > 0.0f ? (softStrength < 1.0f ? softStrength : 1.0f) : 0.0f;
    return 1.0f - kSoftResetDepth * s;
}

// ---- the veto detector's rule (HdrVetoCutDetector) ---------------------------------------------------------------------
constexpr float kGridDifference = 24.0f;
constexpr float kHistogramDistance = 0.15f;
constexpr float kHistogramDistanceAlone = 0.35f;

struct GridHistThresholds {
    float gridDifference = kGridDifference;
    float histogramDistance = kHistogramDistance;
    float histogramDistanceAlone = kHistogramDistanceAlone;
};

template <class T>
inline T MeanAbsDifference(const T* a, const T* b, size_t n)
{
    T sum = T(0);
    for (size_t i = 0; i < n; ++i)
        sum += std::fabs(a[i] - b[i]);
    return sum / static_cast<T>(n);
}

inline bool GridHistCut(float grid, float histogram, const GridHistThresholds& t = GridHistThresholds())
{
    return (grid >= t.gridDifference && histogram >= t.histogramDistance) || histogram >= t.histogramDistanceAlone;
}

// ---- the fade fit (M4) -------------------------------------------------------------------------------------------------
struct FadeParams {
    bool enabled = true;
    float minStepCodes = kFadeMinStepCodes;
    float maxStepCodes = kFadeMaxStepCodes;
    float minBlack = kFadeMinBlack;
    float maxBlack = kFadeMaxBlack;
};

struct FadeIn {
    float meanA = 0, meanB = 0, spreadA = 0, spreadB = 0;
    float previousStep = 0;
};

struct FadeOut {
    bool fading = false;
    float gain = 1.0f;
    float offset = 0.0f;
    float step = 0.0f;
};

inline FadeOut FadeFit(const FadeIn& in, const FadeParams& p = FadeParams())
{
    FadeOut o;
    if (!p.enabled)
        return o;
    const float d = in.meanB - in.meanA;
    const float g = in.spreadB / (std::max)(in.spreadA, kFadeDivGuard);
    const float black = std::fabs(1.0f - g) > kFadeScaleEps ? (in.meanB - g * in.meanA) / (1.0f - g) : kFadeNoBlack;
    const bool scaled = std::fabs(1.0f - g) > kFadeScaleEps && black > p.minBlack && black < p.maxBlack;
    const float ad = std::fabs(d), ap = std::fabs(in.previousStep);
    const bool sameSign = (d > 0.0f) == (in.previousStep > 0.0f) && (d < 0.0f) == (in.previousStep < 0.0f);
    o.fading = scaled && ad > p.minStepCodes && ad < p.maxStepCodes && ap > p.minStepCodes && sameSign &&
        in.spreadA > kFadeMinSpread && in.spreadB > kFadeMinSpread;
    o.gain = (std::min)((std::max)(in.spreadA / (std::max)(in.spreadB, kFadeDivGuard), kFadeGainMin), kFadeGainMax);
    o.offset = in.meanA - o.gain * in.meanB;
    o.step = d;
    return o;
}

// ---- the multi-frame fade law (unit version 4) ---------------------------------------------------------------------------

struct FadeRun {
    int32_t direction = 0;      // +1 the picture brightens, -1 it darkens, 0 no run
    uint32_t pairs = 0;         // pairs in the run
    uint32_t fadePairs = 0;     // confirmed fade pairs inside the run
    uint32_t baseMean = 0;      // the run's anchor record: mean and p99 (P4 codes)
    uint32_t baseP99 = 0;
    uint32_t blackBaseMean = 0; // original flat-black origin mean / p99 when blackOrigin is true
    uint32_t blackBaseP99 = 0;
    uint32_t refSpread = 0;     // the largest p99 - p01 inside the run
    uint32_t endPixels = 0;     // the record the run ends at
    uint32_t endMean = 0, endP01 = 0, endP50 = 0, endP99 = 0;
    bool baseGridValid = false;
    bool midGridValid = false;
    bool blackOrigin = false;
    uint16_t baseCells[kGridW * kGridH] = {};
    uint16_t midCells[kGridW * kGridH] = {};
    uint16_t blackBaseCells[kGridW * kGridH] = {};
    double cumVx = 0.0, cumVy = 0.0;
    double midCumVx = 0.0, midCumVy = 0.0;
    double anchorKeep = 1.0;
    double midAnchorKeep = 1.0;
    double keep = 1.0;
};

inline bool FadeRunEndsAt(const FadeRun& run, const SourceStats::Record& r)
{
    return run.endPixels == r.pixelCount && run.endMean == r.mean && run.endP01 == r.p01 && run.endP50 == r.p50 && run.endP99 == r.p99;
}

inline void FadeRunEndAt(FadeRun& run, const SourceStats::Record& r)
{
    run.endPixels = r.pixelCount;
    run.endMean = r.mean;
    run.endP01 = r.p01;
    run.endP50 = r.p50;
    run.endP99 = r.p99;
}

inline void CopyCellsTo16(const uint32_t* src, uint16_t* dst)
{
    for (int i = 0; i < kGridW * kGridH; ++i)
        dst[i] = uint16_t(src[i] > 65535u ? 65535u : src[i]);
}

inline double PairFadeKeep(const SourceStats::Record& prev, const SourceStats::Record& cur);

inline bool FoldFadeRun(const SourceStats::Record& prev, const SourceStats::Record& cur, const EventSignals& e, FadeRun& run,
                        double* frameKeep = nullptr)
{
    if (frameKeep)
        *frameKeep = 1.0;
    if (!FadeRunEndsAt(run, prev))
        run = FadeRun();

    const int64_t dMean = int64_t(cur.mean) - int64_t(prev.mean);
    const int64_t dP99 = int64_t(cur.p99) - int64_t(prev.p99);
    const uint32_t spreadPrev = prev.p99 > prev.p01 ? prev.p99 - prev.p01 : 0u;
    const uint32_t spreadCur = cur.p99 > cur.p01 ? cur.p99 - cur.p01 : 0u;
    const uint32_t spread = (std::max)(1u, (std::max)(spreadPrev, spreadCur));
    const int64_t dSpread = int64_t(spreadCur) - int64_t(spreadPrev);
    const int64_t aSpread = dSpread < 0 ? -dSpread : dSpread;
    const int64_t aMean = dMean < 0 ? -dMean : dMean;
    const int32_t dirP99 = dP99 > 0 ? 1 : dP99 < 0 ? -1 : 0;
    const bool hasGrid = prev.gridFound != 0 && cur.gridFound != 0;
    const bool activeFade = run.keep < 1.0 || run.fadePairs >= kFadeRunMinPairs;
    const int64_t contRef = int64_t((std::max)(run.refSpread, spread));

    // Hard-cut / flash protection: flash always resets.
    if (e.flash) {
        run = FadeRun();
        FadeRunEndAt(run, cur);
        return false;
    }

    // Helper: check spatial brightening consensus from flat-black origin cells -> cur cells.
    auto BlackOriginBrightening = [&]() -> bool {
        if (!hasGrid)
            return dirP99 > 0 && dSpread > 0;
        constexpr int R = kLocalRadius;
        const double b0 = double(run.blackBaseMean) * kGridScale;
        int litCells = 0, sameSign = 0, oppSign = 0;
        for (int y = R; y < kGridH - R; ++y)
            for (int x = R; x < kGridW - R; ++x) {
                const double r0 = double(run.blackBaseCells[y * kGridW + x]) * kGridScale;
                const double c0 = double(cur.cells[y * kGridW + x]) * kGridScale;
                if ((std::max)(r0, c0) > b0 + double(kFadeMinSpread)) {
                    ++litCells;
                    const double delta = c0 - r0;
                    if (delta > kFadeCellStepEps)
                        ++sameSign;
                    else if (delta < -kFadeCellStepEps)
                        ++oppSign;
                }
            }
        return litCells > 0 &&
            double(sameSign) >= kFadeSpatialConsensus * double(sameSign + oppSign) &&
            double(sameSign) >= kFadeSpatialCoverage * double(litCells);
    };

    // Helper: check that prev -> cur is genuinely brightening across lit cells (rejects static 50/50 noise on dim shots
    // after a black->dim hard cut while still accepting sub-0.5-code fade-in steps from black).
    auto StepBrightening = [&](const double prevA[kGridH][kGridW], const double curA[kGridH][kGridW]) -> bool {
        constexpr int R = kLocalRadius;
        double minC = double(kFadeNoBlack);
        for (int y = R; y < kGridH - R; ++y)
            for (int x = R; x < kGridW - R; ++x) {
                const double m = (std::min)(prevA[y][x], curA[y][x]);
                if (m < minC)
                    minC = m;
            }
        const double floor = (std::max)(double(run.blackBaseMean) * kGridScale, minC);
        int brightEps = 0, darkEps = 0, brightZero = 0, darkZero = 0;
        for (int y = R; y < kGridH - R; ++y)
            for (int x = R; x < kGridW - R; ++x) {
                if ((std::max)(prevA[y][x], curA[y][x]) > floor + double(kFadeMinSpread)) {
                    const double delta = curA[y][x] - prevA[y][x];
                    if (delta > kFadeCellStepEps)
                        ++brightEps;
                    else if (delta < -kFadeCellStepEps)
                        ++darkEps;
                    if (delta > 0.0)
                        ++brightZero;
                    else if (delta < 0.0)
                        ++darkZero;
                }
            }
        if (brightEps + darkEps > 0)
            return darkEps == 0 || double(brightEps) >= kFadeSpatialConsensus * double(brightEps + darkEps);
        return brightZero > 0 && double(brightZero) >= kFadeSpatialConsensus * double(brightZero + darkZero);
    };

    // Helper: check that lit cells of prev are darkening toward flat/near-flat black cur with zero brightening.
    // Requires litPrev > 0 so two consecutive flat-black frames (black hold) never trigger a collapse fade.
    auto CollapseToBlackOk = [&]() -> bool {
        if (!hasGrid)
            return spreadPrev > kBlackSpreadP4;
        constexpr int R = kLocalRadius;
        const double bCur = double(cur.mean) * kGridScale;
        int litPrev = 0, darkZero = 0, brightEps = 0;
        for (int y = R; y < kGridH - R; ++y)
            for (int x = R; x < kGridW - R; ++x) {
                const double p0 = double(prev.cells[y * kGridW + x]) * kGridScale;
                const double c0 = double(cur.cells[y * kGridW + x]) * kGridScale;
                const double delta = c0 - p0;
                if (delta > kFadeCellStepEps)
                    ++brightEps;
                if (p0 > bCur + double(kFadeMinSpread)) {
                    ++litPrev;
                    if (delta < 0.0)
                        ++darkZero;
                }
            }
        return brightEps == 0 && litPrev > 0 && darkZero > 0;
    };

    if (e.cut) {
        int brightened = 0, darkened = 0;
        if (hasGrid)
            CountCellStepSigns(prev.cells, cur.cells, &brightened, &darkened);

        // (1) Active fade-out collapsing into flat/near-flat black (never triggers on a hard cut to a textured dark shot).
        bool curFlatBlack = spreadCur <= kBlackSpreadP4;
        if (!curFlatBlack && hasGrid) {
            Grid gc;
            GridOf(cur, &gc);
            curFlatBlack = Flatness(gc) < kFlatF;
        }
        const bool activeFadeOutToBlack = run.direction < 0 && activeFade && dMean < 0 && curFlatBlack &&
            cur.mean < kBlackMeanMaxP4 && aMean < kMaxFadeStepP4 && aMean * kFadeMaxDiv < contRef &&
            CollapseToBlackOk();
        if (activeFadeOutToBlack) {
            ++run.pairs;
            run.refSpread = (std::max)(run.refSpread, spread);
            FadeRunEndAt(run, cur);
            const double fk = PairFadeKeep(prev, cur);
            if (frameKeep)
                *frameKeep = fk;
            run.keep *= fk;
            ++run.fadePairs;
            return true;
        }

        // (2) Black-origin fade-in continuation across near-black histogram bin quantization.
        bool blackOriginContinue = false;
        GridFadeFit blackFit;
        if (run.blackOrigin && run.direction > 0 && run.pairs >= 1u && dMean > 0 &&
            aMean < kMaxFadeStepP4 && aSpread < kMaxFadeStepP4) {
            if (hasGrid) {
                double prevG[kGridH][kGridW], curG[kGridH][kGridW];
                double prevA[kGridH][kGridW], curA[kGridH][kGridW];
                LoadGridCodes(prev.cells, prevG);
                LoadGridCodes(cur.cells, curG);
                double svx = 0.0, svy = 0.0;
                AlignGrid(prevG, curG, kShiftRadius, prevA, curA, &svx, &svy);
                const bool stepUp = StepBrightening(prevA, curA);
                if (!run.baseGridValid && spreadPrev > kBlackSpreadP4) {
                    CopyCellsTo16(prev.cells, run.baseCells);
                    run.baseGridValid = true;
                    run.midGridValid = false;
                    run.cumVx = 0.0;
                    run.cumVy = 0.0;
                    run.anchorKeep = run.keep;
                }
                if (run.baseGridValid)
                    blackFit = FitGridFade(run.baseCells, cur.cells, kLocalRadius);
                if (!blackFit.valid && spreadPrev > kBlackSpreadP4)
                    blackFit = FitGridFade(prev.cells, cur.cells, kShiftRadius);

                Grid gp, gc;
                GridOf(prev, &gp);
                GridOf(cur, &gc);
                const bool prevNearBlack = spreadPrev <= kBlackSpreadP4 || Flatness(gp) < kFlatF;
                const bool curNearBlack = spreadCur <= kBlackSpreadP4 && Flatness(gc) < kFlatF;
                if (blackFit.valid && blackFit.dir > 0 && stepUp) {
                    blackOriginContinue = true;
                } else if (prevNearBlack && curNearBlack && stepUp && cur.mean > prev.mean &&
                           prev.mean >= run.blackBaseMean && cur.p99 > run.blackBaseP99 &&
                           BlackOriginBrightening()) {
                    blackOriginContinue = true;
                }
            } else {
                blackOriginContinue = (dirP99 > 0 && dSpread > 0);
            }
        }
        if (blackOriginContinue) {
            ++run.pairs;
            run.refSpread = (std::max)(run.refSpread, spread);
            FadeRunEndAt(run, cur);
            double fk = PairFadeKeep(prev, cur);
            if (hasGrid && blackFit.valid) {
                const double k = run.anchorKeep * (std::min)(blackFit.gain, 1.0 / blackFit.gain);
                if (k < run.keep) {
                    fk = run.keep > 0.0 ? k / run.keep : 0.0;
                    run.keep = k;
                } else {
                    fk = 1.0;
                }
            } else {
                run.keep *= fk;
            }
            if (frameKeep)
                *frameKeep = fk;
            ++run.fadePairs;
            return true;
        }

        // Any other cut resets the run; optionally seed black-origin fade-in pair 1 (ALWAYS returns false on pair 1).
        const bool failedBlackOrigin = run.blackOrigin && run.direction > 0;
        run = FadeRun();
        if (!failedBlackOrigin && spreadPrev <= kBlackSpreadP4 && prev.mean < kBlackMeanMaxP4 && dMean > 0 && dP99 > 0 &&
            aMean < kMaxFadeStepP4 && aSpread < kMaxFadeStepP4 && (!hasGrid || darkened == 0)) {
            run.direction = 1;
            run.pairs = 1u;
            run.blackOrigin = true;
            run.baseMean = prev.mean;
            run.baseP99 = prev.p99;
            run.blackBaseMean = prev.mean;
            run.blackBaseP99 = prev.p99;
            run.refSpread = spread;
            if (hasGrid) {
                CopyCellsTo16(prev.cells, run.blackBaseCells);
                run.baseGridValid = false;
            }
        }
        FadeRunEndAt(run, cur);
        return false;
    }

    if (dMean == 0 && dP99 == 0 && e.histDistance == 0) {
        FadeRunEndAt(run, cur);   // repeated record: hold run unchanged
        return false;
    }

    // ---- Grid path: 576-cell spatial affine fade law ----------------------------------------------------------------------
    if (hasGrid) {
        constexpr int R = kLocalRadius;
        constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));

        // Motion-compensated adjacent step (prev -> cur) for run tracking and post-fade plateau detection.
        double prevG[kGridH][kGridW], curG[kGridH][kGridW];
        double prevAligned[kGridH][kGridW], curAligned[kGridH][kGridW];
        LoadGridCodes(prev.cells, prevG);
        LoadGridCodes(cur.cells, curG);
        double stepVx = 0.0, stepVy = 0.0;
        AlignGrid(prevG, curG, kShiftRadius, prevAligned, curAligned, &stepVx, &stepVy);
        double stepSum = 0.0;
        for (int y = R; y < kGridH - R; ++y)
            for (int x = R; x < kGridW - R; ++x)
                stepSum += curAligned[y][x] - prevAligned[y][x];
        const double stepMean = stepSum / kN;
        const int32_t stepDir = stepMean > 0.0 ? 1 : (stepMean < 0.0 ? -1 : (dMean > 0 ? 1 : (dMean < 0 ? -1 : 0)));

        const bool fromBlack = spreadPrev <= kBlackSpreadP4 && prev.mean < kBlackMeanMaxP4 &&
            dMean > 0 && dP99 > 0 && aMean < kMaxFadeStepP4 && aSpread < kMaxFadeStepP4;
        const bool jump = aMean * kFadeMaxDiv >= contRef && !(run.blackOrigin && dMean > 0 && aMean < kMaxFadeStepP4);
        const bool continues = run.direction != 0 && !jump && (stepDir == run.direction || stepDir == 0);

        if (!continues) {
            run = FadeRun();
            if (stepDir == 0 || (!fromBlack && aMean * kFadeMaxDiv >= int64_t(spread))) {
                FadeRunEndAt(run, cur);
                return false;
            }
            run.direction = stepDir;
            run.blackOrigin = fromBlack;
            run.baseMean = prev.mean;
            run.baseP99 = prev.p99;
            run.refSpread = spread;
            if (fromBlack) {
                run.blackBaseMean = prev.mean;
                run.blackBaseP99 = prev.p99;
                CopyCellsTo16(prev.cells, run.blackBaseCells);
                run.baseGridValid = false;
            } else {
                CopyCellsTo16(prev.cells, run.baseCells);
                run.baseGridValid = true;
            }
            run.cumVx = 0.0;
            run.cumVy = 0.0;
            run.anchorKeep = 1.0;
        }

        // If blackOrigin has emerged to non-flat prev, snapshot the first lit spatial anchor for OLS regression.
        if (run.blackOrigin && !run.baseGridValid && spreadPrev > kBlackSpreadP4) {
            CopyCellsTo16(prev.cells, run.baseCells);
            run.baseMean = prev.mean;
            run.baseP99 = prev.p99;
            run.baseGridValid = true;
            run.midGridValid = false;
            run.cumVx = 0.0;
            run.cumVy = 0.0;
            run.anchorKeep = run.keep;
        }

        // Integrate per-frame camera displacement so baseCells stays inside the +-kLocalRadius alignment window during pans.
        bool midCapturedThisFrame = false;
        if (run.baseGridValid) {
            run.cumVx += stepVx;
            run.cumVy += stepVy;
            if (run.midGridValid) {
                run.midCumVx += stepVx;
                run.midCumVy += stepVy;
            }
            if ((std::max)(std::fabs(run.cumVx), std::fabs(run.cumVy)) > double(kLocalRadius)) {
                if (run.midGridValid) {
                    for (int i = 0; i < kGridW * kGridH; ++i)
                        run.baseCells[i] = run.midCells[i];
                    run.anchorKeep = run.midAnchorKeep;
                    run.cumVx = run.midCumVx;
                    run.cumVy = run.midCumVy;
                    run.midGridValid = false;
                } else {
                    CopyCellsTo16(prev.cells, run.baseCells);
                    run.anchorKeep = run.keep;
                    run.cumVx = stepVx;
                    run.cumVy = stepVy;
                }
            }
            if (!run.midGridValid && (std::max)(std::fabs(run.cumVx), std::fabs(run.cumVy)) >= double(kShiftRadius)) {
                CopyCellsTo16(cur.cells, run.midCells);
                run.midGridValid = true;
                run.midAnchorKeep = run.keep;
                run.midCumVx = 0.0;
                run.midCumVy = 0.0;
                midCapturedThisFrame = true;
            }
        }

        ++run.pairs;
        run.refSpread = (std::max)(run.refSpread, spread);
        FadeRunEndAt(run, cur);

        if (run.pairs < kFadeRunMinPairs || stepDir != run.direction) {
            if (e.fade && stepDir == run.direction)
                ++run.fadePairs;
            return false;
        }

        // Helper: verify that the current motion-aligned step (prev -> cur) is still moving in run.direction
        // (supports sub-0.5-code slow fades and scrolling text while stopping immediately on static plateaus).
        auto StepStillMoving = [&](double black) -> bool {
            const bool stationary = (stepVx == 0.0 && stepVy == 0.0);
            if (stationary) {
                if (dMean * int64_t(run.direction) <= 0)
                    return false;
            } else {
                if (stepMean * double(run.direction) <= 0.0)
                    return false;
            }
            double minC = double(kFadeNoBlack);
            for (int y = R; y < kGridH - R; ++y)
                for (int x = R; x < kGridW - R; ++x) {
                    const double m = (std::min)(prevAligned[y][x], curAligned[y][x]);
                    if (m < minC)
                        minC = m;
                }
            const double floor = (std::max)(black, minC);
            int sSameEps = 0, sOppEps = 0;
            int sSameZero = 0, sOppZero = 0;
            for (int y = R; y < kGridH - R; ++y)
                for (int x = R; x < kGridW - R; ++x) {
                    if ((std::max)(prevAligned[y][x], curAligned[y][x]) > floor + double(kFadeMinSpread)) {
                        const double delta = curAligned[y][x] - prevAligned[y][x];
                        if (double(run.direction) * delta > kFadeCellStepEps)
                            ++sSameEps;
                        else if (-double(run.direction) * delta > kFadeCellStepEps)
                            ++sOppEps;
                        if (double(run.direction) * delta > 0.0)
                            ++sSameZero;
                        else if (-double(run.direction) * delta > 0.0)
                            ++sOppZero;
                    }
                }
            // Cumulative FitGridFade(base -> cur) already enforces strict kFadeSpatialConsensus (0.75) over all lit cells.
            // On a single-frame step of a slow fade most cells step < kFadeCellStepEps (0.5 code), so require simple
            // majority (kFadeSpatialCoverage = 0.5) on the sparse above-LSB subset to tolerate local subject motion.
            if (sSameEps + sOppEps > 0)
                return double(sSameEps) >= kFadeSpatialCoverage * double(sSameEps + sOppEps);
            return sSameZero > 0 && double(sSameZero) >= kFadeSpatialConsensus * double(sSameZero + sOppZero);
        };

        // 1. Cumulative spatial affine fit: baseCells -> cur.cells (searchRadius = kLocalRadius = 2).
        if (run.baseGridValid) {
            GridFadeFit fit = FitGridFade(run.baseCells, cur.cells, kLocalRadius);
            if (fit.valid && fit.dir == run.direction && StepStillMoving(fit.black)) {
                const double k = run.anchorKeep * (std::min)(fit.gain, 1.0 / fit.gain);
                if (k < run.keep) {
                    if (frameKeep)
                        *frameKeep = run.keep > 0.0 ? k / run.keep : 0.0;
                    run.keep = k;
                }
                ++run.fadePairs;
                if (midCapturedThisFrame)
                    run.midAnchorKeep = run.keep;
                return true;
            }
        }

        // 2. Rolling adjacent re-anchor for fast pan + fade (prev.cells -> cur.cells, searchRadius = kShiftRadius = 1).
        {
            const GridFadeFit adjFit = FitGridFade(prev.cells, cur.cells, kShiftRadius);
            if (adjFit.valid && adjFit.dir == run.direction && StepStillMoving(adjFit.black)) {
                CopyCellsTo16(prev.cells, run.baseCells);
                run.baseGridValid = true;
                run.midGridValid = false;
                run.cumVx = stepVx;
                run.cumVy = stepVy;
                run.anchorKeep = run.keep;
                const double k = run.anchorKeep * (std::min)(adjFit.gain, 1.0 / adjFit.gain);
                if (k < run.keep) {
                    if (frameKeep)
                        *frameKeep = run.keep > 0.0 ? k / run.keep : 0.0;
                    run.keep = k;
                }
                ++run.fadePairs;
                return true;
            }
        }

        // 3. Black-origin near-black fade-in continuation while prev is near-flat and cur is still flat black (<= 1 code & < kFlatF).
        if (run.blackOrigin && run.direction > 0 && dMean > 0 && aMean < kMaxFadeStepP4 && aSpread < kMaxFadeStepP4) {
            Grid gp, gc;
            GridOf(prev, &gp);
            GridOf(cur, &gc);
            if ((spreadPrev <= kBlackSpreadP4 || Flatness(gp) < kFlatF) &&
                (spreadCur <= kBlackSpreadP4 && Flatness(gc) < kFlatF) &&
                cur.mean > prev.mean && prev.mean >= run.blackBaseMean && cur.p99 > run.blackBaseP99 &&
                StepBrightening(prevAligned, curAligned) && BlackOriginBrightening()) {
                const double fk = PairFadeKeep(prev, cur);
                if (frameKeep)
                    *frameKeep = fk;
                run.keep *= fk;
                ++run.fadePairs;
                return true;
            }
        }

        // 4. Active fade-out collapsing into flat/near-flat black without triggering e.cut (e.g. sparse title card on black).
        if (run.direction < 0 && activeFade && dMean < 0 && cur.mean < kBlackMeanMaxP4 &&
            aMean < kMaxFadeStepP4 && aMean * kFadeMaxDiv < contRef) {
            Grid gc;
            GridOf(cur, &gc);
            if ((spreadCur <= kBlackSpreadP4 || Flatness(gc) < kFlatF) && CollapseToBlackOk()) {
                const double fk = PairFadeKeep(prev, cur);
                if (frameKeep)
                    *frameKeep = fk;
                run.keep *= fk;
                ++run.fadePairs;
                return true;
            }
        }

        if (e.fade)
            ++run.fadePairs;
        return false;
    }

    // ---- Gridless fallback (planes < 32x18 or synthetic gridless records): v3 M4 linear fade-run law ----------------------
    const bool continues = run.direction != 0 && (dirP99 == run.direction || dirP99 == 0) &&
        aMean * kFadeMaxDiv < contRef;
    if (!continues) {
        run = FadeRun();
        if (dirP99 == 0 || aMean * kFadeMaxDiv >= int64_t(spread)) {
            FadeRunEndAt(run, cur);
            return false;
        }
        run.direction = dirP99;
        run.baseMean = prev.mean;
        run.baseP99 = prev.p99;
        run.refSpread = spread;
    }
    ++run.pairs;
    run.refSpread = (std::max)(run.refSpread, spread);
    FadeRunEndAt(run, cur);
    if (run.pairs < kFadeRunMinPairs || dirP99 != run.direction || dMean * run.direction <= 0)
        return false;
    const double m0 = double(run.baseMean), p0 = double(run.baseP99), m1 = double(cur.mean), p1 = double(cur.p99);
    const double den = (p0 - p1) - (m0 - m1);
    if (den == 0.0)
        return false;
    const double black = (m1 * p0 - m0 * p1) / den;
    if (!(black > kBlackMinP4 && black < kBlackMaxP4) || p0 <= black || m0 <= black)
        return false;
    const double gain = (p1 - black) / (p0 - black);
    const bool fade = std::fabs(1.0 - gain) > kFadeRunMinGain && (gain - 1.0) * double(run.direction) > 0.0 &&
        (m1 - m0) * double(run.direction) > 0.0;
    if (fade) {
        const double k = gain <= 0.0 ? 0.0 : (std::min)(gain, 1.0 / gain);
        if (k < run.keep) {
            if (frameKeep)
                *frameKeep = run.keep > 0.0 ? k / run.keep : 0.0;
            run.keep = k;
        }
        ++run.fadePairs;
    }
    return fade;
}

// ---- the partial reset on fades (unit version 4) -------------------------------------------------------------------------
inline double PairFadeKeep(const SourceStats::Record& prev, const SourceStats::Record& cur)
{
    const uint32_t sp = prev.p99 > prev.p01 ? prev.p99 - prev.p01 : 0u;
    const uint32_t sc = cur.p99 > cur.p01 ? cur.p99 - cur.p01 : 0u;
    const uint32_t lo = (std::min)(sp, sc), hi = (std::max)(sp, sc);
    return hi == 0u ? 1.0 : double(lo) / double(hi);
}

inline double FrameFadeKeep(const SourceStats::Record& prev, const SourceStats::Record& cur, bool pairFade, bool runFade, double runKeep)
{
    double k = 1.0;
    if (runFade)
        k = (std::min)(k, runKeep);
    if (pairFade)
        k = (std::min)(k, PairFadeKeep(prev, cur));
    return k < 0.0 ? 0.0 : k;
}

// ---- the matcher's trust cut (M3) and AdaptiveDownscale's mean-step cut -------------------------------------------------------
constexpr float kTrustCutShareAlone = 0.05f;
constexpr float kMeanStepCutCodes = 20.0f;

inline bool TrustCut(float share, float minShare, float histDistance, float cutHistogram, bool regional)
{
    return regional ? (share < minShare && (histDistance > cutHistogram || share < kTrustCutShareAlone)) : share < minShare;
}

inline bool MeanStepCut(float previousInitialised, float previousMean, float mean)
{
    return previousInitialised < 0.5f || std::fabs(mean - previousMean) > kMeanStepCutCodes;
}

// ---- GLSL -----------------------------------------------------------------------------------------------------------------
inline const char* FadeKernelHeader()
{
    return R"GLSL(
shared vec4 fm_[256];
shared float fn_[256];
)GLSL";
}

inline const char* FadeKernelBody()
{
    return R"GLSL(
uint i = gl_LocalInvocationIndex;
ivec2 ls = textureSize(l2_a, 0);
int lc = ls.x * ls.y;
vec4 acc = vec4(0.0);
float an = 0.0;
#ifdef PICTURE
vec4 pa_r = texelFetch(picture, ivec2(0), 0);
#endif
for (int k = int(i); k < lc; k += 256) {
    ivec2 p = ivec2(k % ls.x, k / ls.x);
#ifdef PICTURE
    // Black bars: the picture's mean and spread only.
    vec2 c_ = (vec2(p) + vec2(0.5)) * 32.0;
    if (c_.x < pa_r.x || c_.y < pa_r.y || c_.x >= pa_r.z || c_.y >= pa_r.w)
        continue;
#endif
    float a = texelFetch(l2_a, p, 0).r, b = texelFetch(l2_b, p, 0).r;
    acc += vec4(a, b, a * a, b * b);
    an += 1.0;
}
fm_[i] = acc;
fn_[i] = an;
barrier();
for (uint st = 128u; st > 0u; st >>= 1) {
    if (i < st) {
        fm_[i] += fm_[i + st];
        fn_[i] += fn_[i + st];
    }
    barrier();
}
if (i == 0u) {
    float n = max(fn_[0], 1.0);
    float ma = fm_[0].x / n, mb = fm_[0].y / n;
    float sa = sqrt(max(fm_[0].z / n - ma * ma, 0.0)), sb = sqrt(max(fm_[0].w / n - mb * mb, 0.0));
    vec4 prev = imageLoad(state_io, ivec2(0));
    float d = mb - ma;
    // A fade scales the picture about its black level: B ~ black + g x (A -
    // black), with g = B's spread over A's. A pan moves the mean with the
    // content entering and leaving while the spread stays (g ~ 1), which puts
    // the implied black level far outside the plausible range.
    float g = sb / max(sa, 1e-3);
    float black = abs(1.0 - g) > 0.01 ? (mb - g * ma) / (1.0 - g) : 1e9;
    bool scaled = abs(1.0 - g) > 0.01 && black > -12.0 && black < 48.0;
    bool fade_on = scaled && abs(d) > fade_lo && abs(d) < fade_hi && abs(prev.w) > fade_lo && sign(d) == sign(prev.w) && sa > 1.0 && sb > 1.0;
    float gain = clamp(sa / max(sb, 1e-3), 0.5, 2.0);
    imageStore(state_io, ivec2(0), vec4(gain, ma - gain * mb, fade_on ? 1.0 : 0.0, d));
}
)GLSL";
}

inline const char* TrustCutRegionalLine()
{
    return R"GLSL(    bool cut = share < min_share && (hist_d > cut_hist || share < 0.05);
)GLSL";
}

inline const char* TrustCutPlainLine()
{
    return R"GLSL(    bool cut = share < min_share;
)GLSL";
}

inline const char* MeanStepCutLine()
{
    return R"GLSL(    bool cut = prev.a < 0.5 || abs(luma_mean - prev.b) > 20.0;
)GLSL";
}

} // namespace CutFadeJudge
} // namespace VpUnit
