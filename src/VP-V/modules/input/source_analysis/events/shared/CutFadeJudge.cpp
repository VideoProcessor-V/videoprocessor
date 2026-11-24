// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Out-of-line non-template CPU bodies of VpUnit::CutFadeJudge (see CutFadeJudge.h).
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>

namespace VpUnit
{
namespace CutFadeJudge
{

void ComputeInteriorCurStats(const double cur[kGridH][kGridW], InteriorCurStats* out)
{
    constexpr int R = kLocalRadius;
    constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));
    double sy = 0.0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x)
            sy += cur[y][x];
    const double my = sy / kN;
    double syy = 0.0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            const double dy = cur[y][x] - my;
            out->curCentered[y][x] = dy;
            syy += dy * dy;
        }
    out->my = my;
    out->syy = syy;
}

void EvalIntShiftCorr(const double ref[kGridH][kGridW], const InteriorCurStats& cs,
                      int sy, int sx, double* outA, double* outC, double* outSxx, double* outCorr)
{
    constexpr int R = kLocalRadius;
    constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));
    double sxSum = 0.0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x)
            sxSum += ref[y + sy][x + sx];
    const double mx = sxSum / kN;
    double sxx = 0.0, sxy = 0.0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            const double dx = ref[y + sy][x + sx] - mx;
            sxx += dx * dx;
            sxy += dx * cs.curCentered[y][x];
        }
    const double a = sxx > 1e-6 ? sxy / sxx : 0.0;
    const double c = cs.my - a * mx;
    const double den = std::sqrt((sxx > 0.0 ? sxx : 0.0) * (cs.syy > 0.0 ? cs.syy : 0.0));
    *outA = a;
    *outC = c;
    *outSxx = sxx;
    *outCorr = den > 1e-12 ? sxy / den : 0.0;
}

double EvalIntShiftEFade(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW],
                         int sy, int sx, double a, double c)
{
    constexpr int R = kLocalRadius;
    constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));
    double ef = 0.0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x)
            ef += std::fabs(cur[y][x] - (a * ref[y + sy][x + sx] + c));
    return ef / kN;
}

HoistedBilinear MakeHoistedBilinear(double shiftY, double shiftX)
{
    HoistedBilinear hb;
    const int iy0 = int(std::floor(shiftY));
    const int ix0 = int(std::floor(shiftX));
    const double fy = shiftY - double(iy0);
    const double fx = shiftX - double(ix0);
    hb.oy0 = iy0;
    hb.oy1 = fy > 0.0 ? iy0 + 1 : iy0;
    hb.ox0 = ix0;
    hb.ox1 = fx > 0.0 ? ix0 + 1 : ix0;
    hb.exactInt = (fy == 0.0 && fx == 0.0);
    hb.w00 = (1.0 - fy) * (1.0 - fx);
    hb.w01 = (1.0 - fy) * fx;
    hb.w10 = fy * (1.0 - fx);
    hb.w11 = fy * fx;
    return hb;
}

double SampleHoisted(const double g[kGridH][kGridW], int y, int x, const HoistedBilinear& hb)
{
    if (hb.exactInt)
        return g[y + hb.oy0][x + hb.ox0];
    return hb.w00 * g[y + hb.oy0][x + hb.ox0] + hb.w01 * g[y + hb.oy0][x + hb.ox1]
         + hb.w10 * g[y + hb.oy1][x + hb.ox0] + hb.w11 * g[y + hb.oy1][x + hb.ox1];
}

double SampleHoistedClamped(const double g[kGridH][kGridW], int y, int x, const HoistedBilinear& hb)
{
    const int y0 = (std::min)(kGridH - 1, (std::max)(0, y + hb.oy0));
    const int x0 = (std::min)(kGridW - 1, (std::max)(0, x + hb.ox0));
    if (hb.exactInt)
        return g[y0][x0];
    const int y1 = (std::min)(kGridH - 1, (std::max)(0, y + hb.oy1));
    const int x1 = (std::min)(kGridW - 1, (std::max)(0, x + hb.ox1));
    return hb.w00 * g[y0][x0] + hb.w01 * g[y0][x1]
         + hb.w10 * g[y1][x0] + hb.w11 * g[y1][x1];
}

void EvalInteriorAffine(const double aligned[kGridH][kGridW], const double cur[kGridH][kGridW],
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

void SampleInteriorPairShifted(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW],
                               int iy, int ix, double fy, double fx,
                               double refS[kGridH][kGridW], double curS[kGridH][kGridW])
{
    constexpr int R = kLocalRadius;
    const double hfy = 0.5 * fy;
    const double hfx = 0.5 * fx;
    const HoistedBilinear hbRef = MakeHoistedBilinear(double(iy) + hfy, double(ix) + hfx);
    const HoistedBilinear hbCur = MakeHoistedBilinear(-hfy, -hfx);
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            refS[y][x] = SampleHoisted(ref, y, x, hbRef);
            curS[y][x] = SampleHoisted(cur, y, x, hbCur);
        }
}

void AlignGrid(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW], int searchRadius,
               double refAligned[kGridH][kGridW], double curAligned[kGridH][kGridW],
               double* outVx, double* outVy)
{
    constexpr int R = kLocalRadius;
    constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));
    const int rad = (std::min)(searchRadius, R);

    // Step 1a: precompute cur centering once, evaluate (0, 0) baseline, then search integer shifts [-rad .. +rad]^2
    // by Pearson correlation with direct integer indexing and lazy L1 eFade evaluation.
    InteriorCurStats curStats;
    ComputeInteriorCurStats(cur, &curStats);
    double a0 = 0.0, c0 = 0.0, sxx0 = 0.0, corr0 = 0.0;
    EvalIntShiftCorr(ref, curStats, 0, 0, &a0, &c0, &sxx0, &corr0);
    const double eFade0 = EvalIntShiftEFade(ref, cur, 0, 0, a0, c0);

    int bestSy = 0, bestSx = 0;
    double bestA = a0, bestC = c0, bestSxx = sxx0;
    double bestCorr = corr0;
    double bestEFade = eFade0;

    if (sxx0 > 1e-6) {
        for (int sy = -rad; sy <= rad; ++sy) {
            for (int sx = -rad; sx <= rad; ++sx) {
                if (sy == 0 && sx == 0)
                    continue;
                double a = 0.0, c = 0.0, sxx = 0.0, corr = 0.0;
                EvalIntShiftCorr(ref, curStats, sy, sx, &a, &c, &sxx, &corr);
                if (sxx <= 1e-6 || !(corr > bestCorr))
                    continue;
                const double eFade = EvalIntShiftEFade(ref, cur, sy, sx, a, c);
                // Only depart from stationary (0, 0) if the shift genuinely improves the affine fit
                // (prevents noise from choosing an arbitrary shift along a degenerate 1D linear ramp;
                // 1 - corr check is scale-invariant for sparse bright regions on black backgrounds).
                if (eFade < eFade0 - kFadeCellStepEps || eFade < kFadeResidualReduction * eFade0 ||
                    (1.0 - corr) < kFadeResidualReduction * (1.0 - corr0)) {
                    bestCorr = corr;
                    bestSy = sy;
                    bestSx = sx;
                    bestA = a;
                    bestC = c;
                    bestSxx = sxx;
                    bestEFade = eFade;
                }
            }
        }
    }

    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            refAligned[y][x] = ref[y + bestSy][x + bestSx];
            curAligned[y][x] = cur[y][x];
        }

    double fx = 0.0;
    double fy = 0.0;
    double curA = bestA;
    double curC = bestC;
    double curSxx = bestSxx;
    double curEFade = bestEFade;
    double curCorr = bestCorr;
    double candRef[kGridH][kGridW], candCur[kGridH][kGridW];

    // Step 1b: sub-cell Gauss-Newton / Lucas-Kanade refinement clamped to [-0.5, +0.5]^2 around (bestSx, bestSy).
    // Uses exact 1-cell centered bilinear differences (+-0.5) with hoisted stencil weights so sharp edges do not suffer 2x overshoot.
    constexpr int kAlignMaxIters = 4;
    for (int iter = 0; iter < kAlignMaxIters; ++iter) {
        const double a = curA;
        const double c = curC;
        if (curSxx <= 1e-6 || !(a > 1e-6))
            break;

        const double hfy = 0.5 * fy;
        const double hfx = 0.5 * fx;
        const HoistedBilinear hbRefXp = MakeHoistedBilinear(double(bestSy) + hfy, double(bestSx) + hfx + 0.5);
        const HoistedBilinear hbRefXm = MakeHoistedBilinear(double(bestSy) + hfy, double(bestSx) + hfx - 0.5);
        const HoistedBilinear hbRefYp = MakeHoistedBilinear(double(bestSy) + hfy + 0.5, double(bestSx) + hfx);
        const HoistedBilinear hbRefYm = MakeHoistedBilinear(double(bestSy) + hfy - 0.5, double(bestSx) + hfx);
        const HoistedBilinear hbCurXp = MakeHoistedBilinear(-hfy, -hfx + 0.5);
        const HoistedBilinear hbCurXm = MakeHoistedBilinear(-hfy, -hfx - 0.5);
        const HoistedBilinear hbCurYp = MakeHoistedBilinear(-hfy + 0.5, -hfx);
        const HoistedBilinear hbCurYm = MakeHoistedBilinear(-hfy - 0.5, -hfx);

        double jxArr[kGridH][kGridW], jyArr[kGridH][kGridW];
        double sumJx = 0.0, sumJy = 0.0;
        for (int y = R; y < kGridH - R; ++y) {
            for (int x = R; x < kGridW - R; ++x) {
                const double rgx = SampleHoistedClamped(ref, y, x, hbRefXp) - SampleHoistedClamped(ref, y, x, hbRefXm);
                const double rgy = SampleHoistedClamped(ref, y, x, hbRefYp) - SampleHoistedClamped(ref, y, x, hbRefYm);
                const double cgx = SampleHoisted(cur, y, x, hbCurXp) - SampleHoisted(cur, y, x, hbCurXm);
                const double cgy = SampleHoisted(cur, y, x, hbCurYp) - SampleHoisted(cur, y, x, hbCurYm);
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
        curA = na;
        curC = nc;
        curSxx = nsxx;
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

GridFadeFit FitGridFadeAligned(const double refAligned[kGridH][kGridW], const double curAligned[kGridH][kGridW],
                               double vx, double vy)
{
    constexpr int R = kLocalRadius;
    constexpr double kN = double((kGridH - 2 * R) * (kGridW - 2 * R));
    GridFadeFit fit;
    fit.vx = vx;
    fit.vy = vy;

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

GridFadeFit FitMotionTolerantFade(const double refAligned[kGridH][kGridW], const double curAligned[kGridH][kGridW],
                                  double vx, double vy, bool allowNearBlackCollapse, bool activeFade)
{
    constexpr int R = kLocalRadius;
    constexpr int kIntH = kGridH - 2 * R;
    constexpr int kIntW = kGridW - 2 * R;
    constexpr int kIntN = kIntH * kIntW;
    constexpr double kN = double(kIntN);
    static_assert(kIntH == 14 && kIntW == 28, "FitMotionTolerantFade tiles assume 28x14 interior");

    GridFadeFit fit;
    fit.vx = vx;
    fit.vy = vy;

    // Part A: Order-statistic (quantile) OLS affine fit over sorted 28x14 = 392 interior cells
    // (invariant under area-preserving non-rigid local motion; zero attenuation bias).
    double rSorted[kIntN], cSorted[kIntN];
    int idx = 0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            rSorted[idx] = refAligned[y][x];
            cSorted[idx] = curAligned[y][x];
            ++idx;
        }
    std::sort(rSorted, rSorted + kIntN);
    std::sort(cSorted, cSorted + kIntN);

    double sx = 0.0, sy = 0.0;
    for (int k = 0; k < kIntN; ++k) {
        sx += rSorted[k];
        sy += cSorted[k];
    }
    const double mx = sx / kN;
    const double my = sy / kN;
    double sxx = 0.0, sxy = 0.0;
    for (int k = 0; k < kIntN; ++k) {
        const double dx = rSorted[k] - mx;
        sxx += dx * dx;
        sxy += dx * (cSorted[k] - my);
    }
    if (sxx <= 1e-6)
        return fit;

    const double A = sxy / sxx;
    const double c = my - A * mx;
    fit.gain = A;
    // FitMotionTolerantFade runs only when rigid 392-cell OLS failed V_fade < kFadeResidualReduction * V_id,
    // meaning non-rigid motion accounts for >= kFadeResidualReduction (50%) of variance. Matching rigid OLS's
    // implied-black b = c / (1 - A) SNR on onset therefore requires 1 / kFadeResidualReduction (2x) cumulative gain;
    // once a fade is already confirmed (activeFade), kFadeRunMinGain preserves continuity across pan leapfrogs.
    const double minMotionGain = activeFade ? kFadeRunMinGain : (kFadeRunMinGain / kFadeResidualReduction);
    if (!(A > 0.0 && std::fabs(1.0 - A) > minMotionGain))
        return fit;

    const double b = c / (1.0 - A);
    fit.black = b;
    if (!(b > double(kFadeMinBlack) && b < double(kFadeMaxBlack)) || !(mx > b && my > b))
        return fit;

    const double minCell = (std::min)(rSorted[0], cSorted[0]);
    const double litFloor = (std::max)(b, minCell);
    const int dir = A > 1.0 ? 1 : -1;
    fit.dir = dir;

    double sumIdSq = 0.0, sumFadeSq = 0.0;
    int litCells = 0, sameSign = 0, oppSign = 0;
    for (int k = 0; k < kIntN; ++k) {
        if ((std::max)(rSorted[k], cSorted[k]) > litFloor + double(kFadeMinSpread)) {
            ++litCells;
            const double delta = cSorted[k] - rSorted[k];
            const double res = cSorted[k] - (A * rSorted[k] + c);
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
    if (litCells < kFadeMinLitCells)
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

    // Part B: Exact 4x2 = 8 square 7x7 macro-tile spatial uniformity & affine scaling confirmation
    // (cancels internal motion dipoles while rejecting local occlusions, non-uniform lighting drifts, partial shadows, and zooms).
    constexpr int kTileCols = 4;
    constexpr int kTileRows = 2;
    constexpr int kTileSize = 7;
    constexpr int kNumTiles = kTileCols * kTileRows;
    constexpr double kTileArea = double(kTileSize * kTileSize);
    int tileLit = 0, tileSame = 0, tileOpp = 0, tileConsistent = 0;
    double vIdTile = 0.0, vFadeTile = 0.0;
    for (int tr = 0; tr < kTileRows; ++tr) {
        for (int tc = 0; tc < kTileCols; ++tc) {
            double sumR = 0.0, sumC = 0.0;
            const int y0 = R + tr * kTileSize;
            const int x0 = R + tc * kTileSize;
            for (int dy = 0; dy < kTileSize; ++dy)
                for (int dx = 0; dx < kTileSize; ++dx) {
                    sumR += refAligned[y0 + dy][x0 + dx];
                    sumC += curAligned[y0 + dy][x0 + dx];
                }
            const double refTile = sumR / kTileArea;
            const double curTile = sumC / kTileArea;
            if ((std::max)(refTile, curTile) > litFloor + double(kFadeMinSpread)) {
                ++tileLit;
                const double dId = curTile - refTile;
                const double dFade = curTile - (A * refTile + c);
                vIdTile += dId * dId;
                vFadeTile += dFade * dFade;
                if (double(dir) * dId > double(kFadeMinSpread))
                    ++tileSame;
                else if (-double(dir) * dId > double(kFadeMinSpread))
                    ++tileOpp;
                if (dir < 0) {
                    if (refTile > b + double(kFadeMinSpread)) {
                        const double aTile = (curTile - b) / (refTile - b);
                        if (double(dir) * (aTile - 1.0) >= kFadeResidualReduction * std::fabs(1.0 - A) &&
                            std::fabs(dFade) < std::fabs(dId))
                            ++tileConsistent;
                    }
                } else {
                    if (refTile > b + double(kFadeMinSpread)) {
                        const double aTile = (curTile - b) / (refTile - b);
                        if (double(dir) * (aTile - 1.0) >= kFadeResidualReduction * std::fabs(1.0 - A) &&
                            std::fabs(dFade) < std::fabs(dId))
                            ++tileConsistent;
                    } else if (curTile > b + double(kFadeMinSpread)) {
                        if (std::fabs(dFade) < std::fabs(dId))
                            ++tileConsistent;
                    }
                }
            }
        }
    }
    if (double(tileLit) < kFadeSpatialCoverage * double(kNumTiles))
        return fit;
    if (tileSame <= 0)
        return fit;
    if (double(tileSame) < kFadeSpatialConsensus * double(tileSame + tileOpp))
        return fit;
    if (double(tileSame) < kFadeSpatialConsensus * double(tileLit))
        return fit;
    if (!(vFadeTile < kFadeResidualReduction * vIdTile))
        return fit;
    if (double(tileConsistent) < kFadeSpatialConsensus * double(tileLit))
        return fit;

    // Part C: Coarse 14x7 (2x2-pooled) scene-structure continuity >= kSameStructure (0.4).
    // 2x2 sums are scale-invariant for Pearson correlation and avoid unrelated cuts with similar histograms.
    // Skip only when an already-active fade-out has collapsed >75% into black (gain < 1/kFadeMaxDiv = 0.25),
    // where spatial correlation naturally degrades into the black floor despite Part A+B passing.
    if (!(allowNearBlackCollapse && fit.dir < 0 && fit.gain < 1.0 / double(kFadeMaxDiv))) {
        constexpr int kPoolH = kIntH / 2;
        constexpr int kPoolW = kIntW / 2;
        constexpr int kPoolN = kPoolH * kPoolW;
        double rp[kPoolN], cp[kPoolN];
        double sumRp = 0.0, sumCp = 0.0;
        int pIdx = 0;
        for (int py = 0; py < kPoolH; ++py) {
            const int y = R + 2 * py;
            for (int px = 0; px < kPoolW; ++px) {
                const int x = R + 2 * px;
                const double rv = refAligned[y][x] + refAligned[y][x + 1] + refAligned[y + 1][x] + refAligned[y + 1][x + 1];
                const double cv = curAligned[y][x] + curAligned[y][x + 1] + curAligned[y + 1][x] + curAligned[y + 1][x + 1];
                rp[pIdx] = rv;
                cp[pIdx] = cv;
                sumRp += rv;
                sumCp += cv;
                ++pIdx;
            }
        }
        const double mxP = sumRp / double(kPoolN);
        const double myP = sumCp / double(kPoolN);
        double sxxP = 0.0, syyP = 0.0, sxyP = 0.0;
        for (int k = 0; k < kPoolN; ++k) {
            const double dr = rp[k] - mxP;
            const double dc = cp[k] - myP;
            sxxP += dr * dr;
            syyP += dc * dc;
            sxyP += dr * dc;
        }
        const double denP = std::sqrt((sxxP > 0.0 ? sxxP : 0.0) * (syyP > 0.0 ? syyP : 0.0));
        const double corr2x2 = denP > 1e-12 ? sxyP / denP : 0.0;
        if (!(corr2x2 >= kSameStructure))
            return fit;
    }

    fit.valid = true;
    return fit;
}

bool HighlightScalingConsistent(uint32_t refP99P4, uint32_t curP99P4, double gain, double black, int dir,
                                double clippedMinDrop)
{
    const double rp = double(refP99P4) * kGridScale;
    const double cp = double(curP99P4) * kGridScale;
    if (!(rp > black + double(kFadeMinSpread) && cp > black + double(kFadeMinSpread)))
        return true;
    // Clipped-white linear fade-out censorship: when a verified fade-out has dropped the unclipped
    // scene by >= clippedMinDrop (default 1/kFadeMaxDiv = 25% for runs; kFadeResidualReduction/kFadeMaxDiv = 12.5%
    // for single-pair ClassifyEvents) while highlights sit at ITU-R BT.709-6 studio peak white (>= 235 - 1 code)
    // and remain non-rising within 1-code quantization jitter (cp <= rp + kFadeMinSpread), p99 may be censored
    // at studio peak white until the highlight unclips.
    if (dir < 0 && rp >= kStudioPeakWhiteCodes - double(kFadeMinSpread) &&
        cp <= rp + double(kFadeMinSpread) && (1.0 - gain) >= clippedMinDrop)
        return true;
    const double aP99 = (cp - black) / (rp - black);
    // For brightening (dir > 0), enforce exact multiplicative reciprocal [0.5, 2.0] consistency around (gain - 1)
    // to reject both pinned highlights (ambient flicker) and localized bright surges / flashes (aP99 >> gain).
    // For darkening (dir < 0), enforce lower bound on highlight drop so fast-unclipping fade-outs are preserved.
    if (dir > 0)
        return kFadeResidualReduction * (gain - 1.0) <= (aP99 - 1.0) &&
            (aP99 - 1.0) <= (1.0 / kFadeResidualReduction) * (gain - 1.0);
    return double(dir) * (aP99 - 1.0) >= kFadeResidualReduction * std::fabs(1.0 - gain);
}

// Derived stationary-background shift guard (Fix #5): when >= kFadeSpatialCoverage (50%) of lit interior cells
// are stationary at (0, 0) (|cur - ref| <= kFadeCellStepEps), reject a nonzero AlignGrid shift so a moving dark
// occluder crossing a static gradient cannot pull global alignment off (0, 0) into a synthetic uniform darkening.
static bool BackgroundIsStationaryAtZeroShift(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW])
{
    constexpr int R = kLocalRadius;
    double minCell = double(kFadeNoBlack);
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            const double m = (std::min)(ref[y][x], cur[y][x]);
            if (m < minCell)
                minCell = m;
        }
    int unshiftedLit = 0, stationaryLit = 0;
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            if ((std::max)(ref[y][x], cur[y][x]) > minCell + double(kFadeMinSpread)) {
                ++unshiftedLit;
                if (std::fabs(cur[y][x] - ref[y][x]) <= kFadeCellStepEps)
                    ++stationaryLit;
            }
        }
    return unshiftedLit > 0 && double(stationaryLit) >= kFadeSpatialCoverage * double(unshiftedLit);
}

static void AlignGridGuarded(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW], int searchRadius,
                             double refAligned[kGridH][kGridW], double curAligned[kGridH][kGridW],
                             double* outVx, double* outVy)
{
    if (BackgroundIsStationaryAtZeroShift(ref, cur)) {
        constexpr int R = kLocalRadius;
        for (int y = R; y < kGridH - R; ++y)
            for (int x = R; x < kGridW - R; ++x) {
                refAligned[y][x] = ref[y][x];
                curAligned[y][x] = cur[y][x];
            }
        if (outVx)
            *outVx = 0.0;
        if (outVy)
            *outVy = 0.0;
        return;
    }
    AlignGrid(ref, cur, searchRadius, refAligned, curAligned, outVx, outVy);
}

// Multiplicative footprint preservation (Derived Fix A, scoped to sparse scenes < kFadeMinLitCells):
// full-picture scenes (>= kFadeMinLitCells) already pass FitGridFadeAligned's whole-picture coverage & R^2
// and may experience Rec.709 shadow-toe compression. On sparse scenes (< 25% of interior lit), any cell whose
// predicted faded value stays strictly above litFloor + 2*kFadeMinSpread cannot collapse straight to black
// (<= litFloor + kFadeMinSpread), and lit cells must explain > 75% L2 variance with >= 75% two-sided uniformity.
static bool FootprintPreserved(const double refAligned[kGridH][kGridW], const double curAligned[kGridH][kGridW],
                               const GridFadeFit& fit)
{
    if (!fit.valid || !(fit.gain > 0.0) || fit.dir == 0)
        return false;
    if (fit.litCells >= kFadeMinLitCells)
        return true;   // full-picture scenes: FitGridFadeAligned already enforces whole-picture coverage & R^2
    constexpr int R = kLocalRadius;
    const double c = fit.black * (1.0 - fit.gain);
    double minCell = double(kFadeNoBlack);
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            const double m = (std::min)(refAligned[y][x], curAligned[y][x]);
            if (m < minCell)
                minCell = m;
        }
    const double litFloor = (std::max)(fit.black, minCell);
    const double litBar = litFloor + double(kFadeMinSpread);
    const double predBar = litFloor + 2.0 * double(kFadeMinSpread);
    int collapsed = 0;
    if (fit.dir < 0) {
        for (int y = R; y < kGridH - R; ++y)
            for (int x = R; x < kGridW - R; ++x) {
                const double rv = refAligned[y][x];
                if (rv > litBar && (fit.gain * rv + c) > predBar && curAligned[y][x] <= litBar)
                    ++collapsed;
            }
    } else {
        for (int y = R; y < kGridH - R; ++y)
            for (int x = R; x < kGridW - R; ++x) {
                const double cv = curAligned[y][x];
                if (cv > litBar && ((cv - c) / fit.gain) > predBar && refAligned[y][x] <= litBar)
                    ++collapsed;
            }
    }
    if (collapsed != 0)
        return false;
    if (!(fit.eFade * fit.eFade < (1.0 - kFadeSpatialConsensus) * fit.eId * fit.eId))
        return false;
    int checkedCells = 0, uniformCells = 0;
    const double refBar = fit.black + double(kFadeMinSpread);
    for (int y = R; y < kGridH - R; ++y)
        for (int x = R; x < kGridW - R; ++x) {
            const double rv = refAligned[y][x];
            const double cv = curAligned[y][x];
            if (rv > refBar && (std::max)(rv, cv) > litBar) {
                ++checkedCells;
                const double pred = fit.gain * rv + c;
                if (std::fabs(cv - pred) <= kFadeResidualReduction * std::fabs(pred - rv))
                    ++uniformCells;
            }
        }
    return checkedCells > 0 && double(uniformCells) >= kFadeSpatialConsensus * double(checkedCells);
}

GridFadeFit FitGridFadeGrid(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW], int searchRadius)
{
    double refAligned[kGridH][kGridW], curAligned[kGridH][kGridW];
    double vx = 0.0, vy = 0.0;
    AlignGrid(ref, cur, searchRadius, refAligned, curAligned, &vx, &vy);
    return FitGridFadeAligned(refAligned, curAligned, vx, vy);
}

EventSignals ClassifyEvents(const SourceStats::Record& prev, const SourceStats::Record& cur, bool fullBetween,
                            bool continuityBetween, AlignedPairCache* outCache)
{
    VPV_COMPUTE_ONCE("U7", ::ComputeOnce::Variant(fullBetween ? 1 : 0, continuityBetween ? 1 : 0));
    if (outCache) {
        outCache->valid = false;
        outCache->adjFitValid = false;
    }
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
                auto AlignedMeanStep = [](const double a[kGridH][kGridW], const double b[kGridH][kGridW]) -> double {
                    constexpr int kIntN = (kGridH - 2 * kLocalRadius) * (kGridW - 2 * kLocalRadius);
                    double s = 0.0;
                    for (int y = kLocalRadius; y < kGridH - kLocalRadius; ++y)
                        for (int x = kLocalRadius; x < kGridW - kLocalRadius; ++x)
                            s += b[y][x] - a[y][x];
                    return s / double(kIntN);
                };
                if (outCache) {
                    AlignGridGuarded(gp.g, gc.g, kShiftRadius, outCache->prevAligned, outCache->curAligned,
                                     &outCache->vx, &outCache->vy);
                    outCache->adjFit = FitGridFadeAligned(outCache->prevAligned, outCache->curAligned,
                                                          outCache->vx, outCache->vy);
                    outCache->valid = true;
                    outCache->adjFitValid = true;
                    const double stepMean = AlignedMeanStep(outCache->prevAligned, outCache->curAligned);
                    e.fade = outCache->adjFit.valid &&
                        stepMean * double(outCache->adjFit.dir) > 0.0 &&
                        FootprintPreserved(outCache->prevAligned, outCache->curAligned, outCache->adjFit) &&
                        std::fabs(1.0 - outCache->adjFit.gain) < 1.0 / double(kFadeMaxDiv) &&
                        HighlightScalingConsistent(prev.p99, cur.p99, outCache->adjFit.gain, outCache->adjFit.black,
                                                   outCache->adjFit.dir, kFadeResidualReduction / double(kFadeMaxDiv));
                } else {
                    double prevA[kGridH][kGridW], curA[kGridH][kGridW];
                    double vx = 0.0, vy = 0.0;
                    AlignGridGuarded(gp.g, gc.g, kShiftRadius, prevA, curA, &vx, &vy);
                    const GridFadeFit fit = FitGridFadeAligned(prevA, curA, vx, vy);
                    const double stepMean = AlignedMeanStep(prevA, curA);
                    e.fade = fit.valid && stepMean * double(fit.dir) > 0.0 &&
                        FootprintPreserved(prevA, curA, fit) &&
                        std::fabs(1.0 - fit.gain) < 1.0 / double(kFadeMaxDiv) &&
                        HighlightScalingConsistent(prev.p99, cur.p99, fit.gain, fit.black, fit.dir,
                                                   kFadeResidualReduction / double(kFadeMaxDiv));
                }
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

bool FoldFadeRun(const SourceStats::Record& prev, const SourceStats::Record& cur, const EventSignals& e, FadeRun& run,
                 double* frameKeep, const AlignedPairCache* preAligned)
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

        // (1) Active fade-out collapsing into flat/near-flat black, or continuing across near-black histogram quantization
        // (time-reversal symmetry with black-origin fade-in below; never triggers on a hard cut to a different dark shot).
        if (run.direction < 0 && activeFade && dMean < 0 && cur.mean < kBlackMeanMaxP4 &&
            aMean < kMaxFadeStepP4 && aMean * kFadeMaxDiv < contRef) {
            bool curFlatBlack = spreadCur <= kBlackSpreadP4;
            if (!curFlatBlack && hasGrid) {
                Grid gc;
                GridOf(cur, &gc);
                curFlatBlack = Flatness(gc) < kFlatF;
            }
            if (curFlatBlack && CollapseToBlackOk()) {
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
            if (hasGrid && aSpread < kMaxFadeStepP4) {
                GridFadeFit fadeOutFit;
                bool fromBase = false;
                if (run.baseGridValid) {
                    fadeOutFit = FitGridFade(run.baseCells, cur.cells, kLocalRadius);
                    if (fadeOutFit.valid && fadeOutFit.dir < 0 &&
                        HighlightScalingConsistent(run.baseP99, cur.p99, fadeOutFit.gain, fadeOutFit.black, fadeOutFit.dir))
                        fromBase = true;
                    else
                        fadeOutFit = GridFadeFit();
                }
                if (!fadeOutFit.valid) {
                    double prevABuf[kGridH][kGridW], curABuf[kGridH][kGridW];
                    double svx = 0.0, svy = 0.0;
                    const double (*prevA)[kGridW] = prevABuf;
                    const double (*curA)[kGridW] = curABuf;
                    if (preAligned != nullptr && preAligned->valid) {
                        prevA = preAligned->prevAligned;
                        curA = preAligned->curAligned;
                        svx = preAligned->vx;
                        svy = preAligned->vy;
                    } else {
                        double prevG[kGridH][kGridW], curG[kGridH][kGridW];
                        LoadGridCodes(prev.cells, prevG);
                        LoadGridCodes(cur.cells, curG);
                        AlignGrid(prevG, curG, kShiftRadius, prevABuf, curABuf, &svx, &svy);
                    }
                    const GridFadeFit adjOut = (preAligned != nullptr && preAligned->adjFitValid)
                        ? preAligned->adjFit
                        : FitGridFadeAligned(prevA, curA, svx, svy);
                    if (adjOut.valid && adjOut.dir < 0 &&
                        HighlightScalingConsistent(prev.p99, cur.p99, adjOut.gain, adjOut.black, adjOut.dir))
                        fadeOutFit = adjOut;
                }
                if (fadeOutFit.valid) {
                    const double k = (fromBase ? run.anchorKeep : run.keep) * (std::min)(fadeOutFit.gain, 1.0 / fadeOutFit.gain);
                    if (k < run.keep) {
                        ++run.pairs;
                        run.refSpread = (std::max)(run.refSpread, spread);
                        FadeRunEndAt(run, cur);
                        if (frameKeep)
                            *frameKeep = run.keep > 0.0 ? k / run.keep : 0.0;
                        run.keep = k;
                        ++run.fadePairs;
                        return true;
                    }
                }
            }
        }

        // (2) Black-origin fade-in continuation across near-black histogram bin quantization.
        bool blackOriginContinue = false;
        GridFadeFit blackFit;
        if (run.blackOrigin && run.direction > 0 && run.pairs >= 1u && dMean > 0 &&
            aMean < kMaxFadeStepP4 && aSpread < kMaxFadeStepP4) {
            if (hasGrid) {
                double prevABuf[kGridH][kGridW], curABuf[kGridH][kGridW];
                double svx = 0.0, svy = 0.0;
                const double (*prevA)[kGridW] = prevABuf;
                const double (*curA)[kGridW] = curABuf;
                if (preAligned != nullptr && preAligned->valid) {
                    prevA = preAligned->prevAligned;
                    curA = preAligned->curAligned;
                    svx = preAligned->vx;
                    svy = preAligned->vy;
                } else {
                    double prevG[kGridH][kGridW], curG[kGridH][kGridW];
                    LoadGridCodes(prev.cells, prevG);
                    LoadGridCodes(cur.cells, curG);
                    AlignGrid(prevG, curG, kShiftRadius, prevABuf, curABuf, &svx, &svy);
                }
                const bool stepUp = StepBrightening(prevA, curA);
                if (!run.baseGridValid && spreadPrev > kBlackSpreadP4) {
                    CopyCellsTo16(prev.cells, run.baseCells);
                    run.baseMean = prev.mean;
                    run.baseP99 = prev.p99;
                    run.baseGridValid = true;
                    run.midGridValid = false;
                    run.cumVx = 0.0;
                    run.cumVy = 0.0;
                    run.anchorKeep = run.keep;
                }
                if (run.baseGridValid)
                    blackFit = FitGridFade(run.baseCells, cur.cells, kLocalRadius);
                if (!blackFit.valid && spreadPrev > kBlackSpreadP4) {
                    blackFit = (preAligned != nullptr && preAligned->adjFitValid)
                        ? preAligned->adjFit
                        : FitGridFadeAligned(prevA, curA, svx, svy);
                }

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
        double prevAlignedBuf[kGridH][kGridW], curAlignedBuf[kGridH][kGridW];
        double stepVx = 0.0, stepVy = 0.0;
        const double (*prevAligned)[kGridW] = prevAlignedBuf;
        const double (*curAligned)[kGridW] = curAlignedBuf;
        if (preAligned != nullptr && preAligned->valid) {
            prevAligned = preAligned->prevAligned;
            curAligned = preAligned->curAligned;
            stepVx = preAligned->vx;
            stepVy = preAligned->vy;
        } else {
            double prevG[kGridH][kGridW], curG[kGridH][kGridW];
            LoadGridCodes(prev.cells, prevG);
            LoadGridCodes(cur.cells, curG);
            AlignGridGuarded(prevG, curG, kShiftRadius, prevAlignedBuf, curAlignedBuf, &stepVx, &stepVy);
        }
        double stepSum = 0.0;
        for (int y = R; y < kGridH - R; ++y)
            for (int x = R; x < kGridW - R; ++x)
                stepSum += curAligned[y][x] - prevAligned[y][x];
        const double stepMean = stepSum / kN;
        const int32_t stepDir = stepMean > 0.0 ? 1 : (stepMean < 0.0 ? -1 : (dMean > 0 ? 1 : (dMean < 0 ? -1 : 0)));

        // Whole-frame order-statistic step gain (prev -> cur): immune to single-tail clipped-p99 unclipping while
        // catching true abrupt whole-picture dips (>= 1/kFadeMaxDiv = 25%) and verifying step slope in StepStillMoving.
        constexpr int kIntN = (kGridH - 2 * kLocalRadius) * (kGridW - 2 * kLocalRadius);
        double pSorted[kIntN], cSorted[kIntN];
        int sIdx = 0;
        for (int y = R; y < kGridH - R; ++y)
            for (int x = R; x < kGridW - R; ++x) {
                pSorted[sIdx] = prevAligned[y][x];
                cSorted[sIdx] = curAligned[y][x];
                ++sIdx;
            }
        std::sort(pSorted, pSorted + kIntN);
        std::sort(cSorted, cSorted + kIntN);
        // Exclude a pinned ITU-R BT.709-6 studio-peak-white ceiling plateau (>= 235 - 1 code, prev & cur max
        // within kFadeCellStepEps) from stepAQ: when midtones darken while hard-clipped peak white stays pinned
        // at ceiling, the ceiling lever arm can otherwise force Cov(p, c) > Var(p) -> stepAQ > 1.0 despite every
        // unclipped pixel darkening. Unclipped flat-top scenes (< 234 codes) are never stripped.
        const bool topPinned = pSorted[kIntN - 1] >= kStudioPeakWhiteCodes - double(kFadeMinSpread) &&
            cSorted[kIntN - 1] >= kStudioPeakWhiteCodes - double(kFadeMinSpread) &&
            std::fabs(pSorted[kIntN - 1] - cSorted[kIntN - 1]) <= kFadeCellStepEps;
        int kEnd = kIntN;
        if (topPinned) {
            const double pCeil = pSorted[kIntN - 1] - double(kFadeMinSpread);
            const double cCeil = cSorted[kIntN - 1] - double(kFadeMinSpread);
            while (kEnd > 2 && pSorted[kEnd - 1] >= pCeil && cSorted[kEnd - 1] >= cCeil)
                --kEnd;
        }
        double sxQ = 0.0, syQ = 0.0;
        for (int k = 0; k < kEnd; ++k) {
            sxQ += pSorted[k];
            syQ += cSorted[k];
        }
        const double invEnd = 1.0 / double(kEnd);
        const double mxQ = sxQ * invEnd;
        const double myQ = syQ * invEnd;
        double stepSxxQ = 0.0, stepSxyQ = 0.0;
        for (int k = 0; k < kEnd; ++k) {
            const double dx = pSorted[k] - mxQ;
            stepSxxQ += dx * dx;
            stepSxyQ += dx * (cSorted[k] - myQ);
        }
        const double stepAQ = stepSxxQ > 1e-6 ? stepSxyQ / stepSxxQ : 1.0;
        const double stepGainDelta = std::fabs(1.0 - stepAQ);
        const bool seedGainJump = stepSxxQ > 1e-6 && stepGainDelta >= 1.0 / double(kFadeMaxDiv);
        // On an active fade-out (run.direction < 0), scale by run.keep (prev's level relative to run start) so
        // linear fade-out tails approaching black (e.g. 2/N -> 1/N -> 0) stay proportional to 1/N instead of
        // jumping relative to prev's tiny remaining contrast.
        const double contGainStep = run.direction < 0 ? (run.keep * stepGainDelta) : stepGainDelta;
        const bool contGainJump = stepSxxQ > 1e-6 && contGainStep >= 1.0 / double(kFadeMaxDiv);
        // Require BOTH raw spread jump AND whole-frame quantile contrast jump to flag an abrupt contrast dip:
        // 1% specular unclipping jumps only aSpread (not stepAQ), while fast clipped-white fade-outs jump only
        // stepAQ (while aSpread stays near zero), whereas true abrupt dips jump both.
        const bool contSpreadJump = (aSpread * kFadeMaxDiv >= contRef) && contGainJump;
        const bool seedSpreadJump = (aSpread * kFadeMaxDiv >= int64_t(spread)) && seedGainJump;

        const bool fromBlack = spreadPrev <= kBlackSpreadP4 && prev.mean < kBlackMeanMaxP4 &&
            dMean > 0 && dP99 > 0 && aMean < kMaxFadeStepP4 && aSpread < kMaxFadeStepP4;
        // Bounded near-black fade-in rise (e.g. frames k = 1..3 of a linear ramp after a failed black-origin reset,
        // where spreadPrev > 1 code yet aSpread / spread = 1/(k+1) >= 1/kFadeMaxDiv): exempt from relative jump guard.
        const bool nearBlackRise = prev.mean < kBlackMeanMaxP4 && dMean > 0 && dP99 > 0 && dSpread > 0 &&
            aMean < kMaxFadeStepP4 && aSpread < kMaxFadeStepP4;
        // Time-reversal dual for an active near-black fade-out collapsing into dark/black: exempt from contSpreadJump.
        const bool nearBlackFall = run.direction < 0 && run.fadePairs >= 1u &&
            prev.mean < kBlackMeanMaxP4 && cur.mean < kBlackMeanMaxP4 &&
            dMean < 0 && aMean < kMaxFadeStepP4 && aMean * kFadeMaxDiv < contRef;
        const bool jump = (aMean * kFadeMaxDiv >= contRef || (contSpreadJump && !nearBlackFall)) &&
            !(run.blackOrigin && dMean > 0 && aMean < kMaxFadeStepP4 && aSpread < kMaxFadeStepP4) && !nearBlackRise;
        const bool continues = run.direction != 0 && !jump && (stepDir == run.direction || stepDir == 0);

        if (!continues) {
            run = FadeRun();
            if (stepDir == 0 || (!(fromBlack || nearBlackRise) &&
                                 (aMean * kFadeMaxDiv >= int64_t(spread) || seedSpreadJump))) {
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
                    run.baseP99 = run.midP99; // baseMean is unused on the grid path (gridless fallback only)
                    run.anchorKeep = run.midAnchorKeep;
                    run.cumVx = run.midCumVx;
                    run.cumVy = run.midCumVy;
                    run.midGridValid = false;
                } else {
                    CopyCellsTo16(prev.cells, run.baseCells);
                    run.baseMean = prev.mean;
                    run.baseP99 = prev.p99;
                    run.anchorKeep = run.keep;
                    run.cumVx = stepVx;
                    run.cumVy = stepVy;
                }
            }
            if (!run.midGridValid && (std::max)(std::fabs(run.cumVx), std::fabs(run.cumVy)) >= double(kShiftRadius)) {
                CopyCellsTo16(cur.cells, run.midCells);
                run.midP99 = cur.p99;
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
        // (D4: symmetric-noise Bayes likelihood-ratio strict majority on above-LSB cells + all-lit majority;
        //  D8: hoisted order-statistic OLS step slope must scale in run.direction).
        const bool stepSlopeOk = !(stepSxxQ > 1e-6) || (double(run.direction) * (stepAQ - 1.0) > 0.0);
        auto StepStillMoving = [&](double black) -> bool {
            const bool stationary = (stepVx == 0.0 && stepVy == 0.0);
            if (stationary) {
                if (dMean * int64_t(run.direction) <= 0)
                    return false;
            } else {
                if (stepMean * double(run.direction) <= 0.0)
                    return false;
            }
            if (!stepSlopeOk)
                return false;

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
            // Guard sub-half-LSB plateaus against noise outliers without letting sub-LSB bilinear rounding noise
            // outvote genuine above-LSB cells when the aligned picture mean moved by > kFadeCellStepEps (0.5 code).
            if ((std::fabs(stepMean) <= kFadeCellStepEps || sSameEps + sOppEps == 0) && !(sSameZero > sOppZero))
                return false;

            // Fix #4: Exact 4x2 (7x7) macro-tile step consensus scoped to near-stationary holds
            // (|stepMean| <= kFadeCellStepEps AND |stepAQ - 1| <= kFadeScaleEps): cancels internal motion dipoles
            // during constant-gain holds while never blocking genuine multi-percent scaling steps.
            if (std::fabs(stepMean) <= kFadeCellStepEps && std::fabs(stepAQ - 1.0) <= double(kFadeScaleEps)) {
                constexpr int kTileCols = 4;
                constexpr int kTileRows = 2;
                constexpr int kTileSize = 7;
                constexpr int kNumTiles = kTileCols * kTileRows;
                constexpr double kTileArea = double(kTileSize * kTileSize);
                int tileSameZero = 0, tileOppZero = 0;
                for (int tr = 0; tr < kTileRows; ++tr) {
                    for (int tc = 0; tc < kTileCols; ++tc) {
                        double sumP = 0.0, sumC = 0.0;
                        const int y0 = kLocalRadius + tr * kTileSize;
                        const int x0 = kLocalRadius + tc * kTileSize;
                        for (int dy = 0; dy < kTileSize; ++dy)
                            for (int dx = 0; dx < kTileSize; ++dx) {
                                sumP += prevAligned[y0 + dy][x0 + dx];
                                sumC += curAligned[y0 + dy][x0 + dx];
                            }
                        const double prevTile = sumP / kTileArea;
                        const double curTile = sumC / kTileArea;
                        if ((std::max)(prevTile, curTile) > floor + double(kFadeMinSpread)) {
                            const double delta = curTile - prevTile;
                            if (double(run.direction) * delta > 0.0)
                                ++tileSameZero;
                            else if (-double(run.direction) * delta > 0.0)
                                ++tileOppZero;
                        }
                    }
                }
                if (tileSameZero + tileOppZero >= int(kFadeSpatialCoverage * double(kNumTiles)) &&
                    double(tileSameZero) < kFadeSpatialConsensus * double(tileSameZero + tileOppZero))
                    return false;
            }

            if (sSameEps + sOppEps > 0)
                return sSameEps > sOppEps;
            return double(sSameZero) >= kFadeSpatialConsensus * double(sSameZero + sOppZero);
        };

        // 1. Cumulative spatial affine fit: baseCells -> cur.cells (searchRadius = kLocalRadius = 2).
        // Align once (guarded against shifting off a stationary background), try rigid FitGridFadeAligned
        // with multiplicative footprint preservation. If the rigid fit fails or stalls from non-rigid OLS
        // attenuation bias (without an explicit FootprintPreserved rejection), fall back to FitMotionTolerantFade.
        bool cumFootprintRejected = false;
        if (run.baseGridValid) {
            double baseG[kGridH][kGridW], curG[kGridH][kGridW];
            double baseAligned[kGridH][kGridW], curBaseAligned[kGridH][kGridW];
            LoadGridCodes(run.baseCells, baseG);
            LoadGridCodes(cur.cells, curG);
            double cumFitVx = 0.0, cumFitVy = 0.0;
            AlignGridGuarded(baseG, curG, kLocalRadius, baseAligned, curBaseAligned, &cumFitVx, &cumFitVy);
            const GridFadeFit rigidFit = FitGridFadeAligned(baseAligned, curBaseAligned, cumFitVx, cumFitVy);
            const bool rigidValidDir = rigidFit.valid && rigidFit.dir == run.direction;
            const bool rigidFootprintOk = rigidValidDir && FootprintPreserved(baseAligned, curBaseAligned, rigidFit);
            cumFootprintRejected = rigidValidDir && !rigidFootprintOk;

            auto TryAcceptCumFit = [&](const GridFadeFit& candidate) -> bool {
                if (!(candidate.valid && candidate.dir == run.direction))
                    return false;
                if (!HighlightScalingConsistent(run.baseP99, cur.p99, candidate.gain, candidate.black, candidate.dir))
                    return false;
                const double k = run.anchorKeep * (std::min)(candidate.gain, 1.0 / candidate.gain);
                if (!(k < run.keep))
                    return false;
                if (!StepStillMoving(candidate.black))
                    return false;
                if (frameKeep)
                    *frameKeep = run.keep > 0.0 ? k / run.keep : 0.0;
                run.keep = k;
                ++run.fadePairs;
                if (midCapturedThisFrame)
                    run.midAnchorKeep = run.keep;
                return true;
            };

            if (rigidFootprintOk && TryAcceptCumFit(rigidFit))
                return true;
            if (!cumFootprintRejected) {
                const GridFadeFit mtFit = FitMotionTolerantFade(baseAligned, curBaseAligned, cumFitVx, cumFitVy,
                                                                nearBlackFall, run.fadePairs >= 1u);
                if (TryAcceptCumFit(mtFit))
                    return true;
            }
        }

        // 2. Rolling adjacent re-anchor for fast pan + fade (prev.cells -> cur.cells, searchRadius = kShiftRadius = 1).
        // Do not allow an adjacent pair to bypass an explicit cumulative footprint-collapse veto.
        if (!cumFootprintRejected) {
            const GridFadeFit rigidAdj = (preAligned != nullptr && preAligned->adjFitValid)
                ? preAligned->adjFit
                : FitGridFadeAligned(prevAligned, curAligned, stepVx, stepVy);
            const bool adjValidDir = rigidAdj.valid && rigidAdj.dir == run.direction;
            const bool adjFootprintOk = adjValidDir && FootprintPreserved(prevAligned, curAligned, rigidAdj);
            const bool adjFootprintRejected = adjValidDir && !adjFootprintOk;
            GridFadeFit adjFit = rigidAdj;
            bool adjOk = adjFootprintOk;
            if (!adjOk && !adjFootprintRejected) {
                adjFit = FitMotionTolerantFade(prevAligned, curAligned, stepVx, stepVy,
                                               nearBlackFall, run.fadePairs >= 1u);
                adjOk = adjFit.valid && adjFit.dir == run.direction;
            }
            if (adjOk && HighlightScalingConsistent(prev.p99, cur.p99, adjFit.gain, adjFit.black, adjFit.dir) &&
                StepStillMoving(adjFit.black)) {
                const double k = run.keep * (std::min)(adjFit.gain, 1.0 / adjFit.gain);
                if (k < run.keep) {
                    CopyCellsTo16(prev.cells, run.baseCells);
                    run.baseMean = prev.mean;
                    run.baseP99 = prev.p99;
                    run.baseGridValid = true;
                    run.midGridValid = false;
                    run.cumVx = stepVx;
                    run.cumVy = stepVy;
                    run.anchorKeep = run.keep;
                    if (frameKeep)
                        *frameKeep = run.keep > 0.0 ? k / run.keep : 0.0;
                    run.keep = k;
                    ++run.fadePairs;
                    return true;
                }
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

} // namespace CutFadeJudge
} // namespace VpUnit
