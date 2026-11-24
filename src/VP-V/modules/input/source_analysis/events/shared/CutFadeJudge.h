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
// Pure C++14, no Windows or GPU headers. Heavy non-template CPU bodies live in CutFadeJudge.cpp (VideoProcessor-Lib).
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
constexpr uint32_t kUnitVersion = 5;

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
// Whole-picture lit-area floor derived from kFadeMaxDiv (4): 392 / 4 = 98 interior cells (25%), below letterboxed scenes (~56-75%) while rejecting localized graphics on black.
constexpr int kFadeMinLitCells = ((kGridH - 2 * kLocalRadius) * (kGridW - 2 * kLocalRadius)) / int(kFadeMaxDiv);
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

struct InteriorCurStats {
    double curCentered[kGridH][kGridW];
    double my = 0.0;
    double syy = 0.0;
};

// Out-of-line non-template alignment and affine fade-fit helpers in CutFadeJudge.cpp (VideoProcessor-Lib).
// Precompute cur's zero-mean interior once for all (2 * rad + 1)^2 integer shifts.
void ComputeInteriorCurStats(const double cur[kGridH][kGridW], InteriorCurStats* out);

// Evaluates (a, c, sxx, corr) for integer shift (sy, sx) with zero copies and zero BilinearGrid calls.
// Precondition: |sy| <= kLocalRadius and |sx| <= kLocalRadius (guarantees y + sy and x + sx stay in-bounds).
void EvalIntShiftCorr(const double ref[kGridH][kGridW], const InteriorCurStats& cs,
                      int sy, int sx, double* outA, double* outC, double* outSxx, double* outCorr);

// Evaluates L1 affine residual for integer shift (sy, sx), called lazily only for (0, 0) and improving shifts.
double EvalIntShiftEFade(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW],
                         int sy, int sx, double a, double c);

struct HoistedBilinear {
    int oy0 = 0, oy1 = 0, ox0 = 0, ox1 = 0;
    double w00 = 1.0, w01 = 0.0, w10 = 0.0, w11 = 0.0;
    bool exactInt = true;
};

// Precomputes constant integer offsets and 4 scalar weights for uniform shift (shiftY, shiftX).
// When |shiftY| <= kLocalRadius and |shiftX| <= kLocalRadius, y + oy1 in [0, kGridH - 1] and x + ox1 in [0, kGridW - 1]
// for all interior cells (when shift == +R, fy == 0 so oy1 == oy0), so SampleHoisted reads strictly in-bounds.
HoistedBilinear MakeHoistedBilinear(double shiftY, double shiftX);
double SampleHoisted(const double g[kGridH][kGridW], int y, int x, const HoistedBilinear& hb);

// Clamped hoisted bilinear sampler for shifts that can step outside [0, kGridH - 1] x [0, kGridW - 1]
// (e.g. Gauss-Newton +-0.5 finite differences at +-kLocalRadius): zero std::floor per cell and equivalent to BilinearGrid.
double SampleHoistedClamped(const double g[kGridH][kGridW], int y, int x, const HoistedBilinear& hb);

void EvalInteriorAffine(const double aligned[kGridH][kGridW], const double cur[kGridH][kGridW],
                        double* outA, double* outC, double* outSxx, double* outCorr, double* outEFade);

// Precondition: |iy + 0.5 * fy| <= kLocalRadius, |ix + 0.5 * fx| <= kLocalRadius, |0.5 * fy| <= kLocalRadius, |0.5 * fx| <= kLocalRadius.
void SampleInteriorPairShifted(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW],
                               int iy, int ix, double fy, double fx,
                               double refS[kGridH][kGridW], double curS[kGridH][kGridW]);

// Decoupled global translation alignment (integer Pearson correlation + sub-cell Lucas-Kanade).
// Uses exact integer shift (iy, ix) plus symmetric fractional half-shifts (+0.5*f on ref, -0.5*f on cur)
// so both grids experience identical bilinear frequency response with zero smoothing at integer shifts.
void AlignGrid(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW], int searchRadius,
               double refAligned[kGridH][kGridW], double curAligned[kGridH][kGridW],
               double* outVx, double* outVy);

GridFadeFit FitGridFadeAligned(const double refAligned[kGridH][kGridW], const double curAligned[kGridH][kGridW],
                               double vx, double vy);

// Motion-tolerant order-statistic OLS + exact 4x2 square macro-tile uniformity + 14x7 2x2-pooled structure continuity
// fallback over the already-aligned 28x14 interior (zero extra AlignGrid calls, zero new constants, zero heap allocations).
GridFadeFit FitMotionTolerantFade(const double refAligned[kGridH][kGridW], const double curAligned[kGridH][kGridW],
                                  double vx, double vy);

// Verifies raw-pixel highlight p99 scales consistently with the cell-OLS fade gain about implied black
// (rejects midtone flicker with pinned highlights; near-black p99 <= black + kFadeMinSpread passes to black-boundary rules).
bool HighlightScalingConsistent(uint32_t refP99P4, uint32_t curP99P4, double gain, double black, int dir,
                                double residualRatio = 1.0, bool interiorClipped = false);

GridFadeFit FitGridFadeGrid(const double ref[kGridH][kGridW], const double cur[kGridH][kGridW], int searchRadius);

// Reusable per-pair kShiftRadius alignment and adjacent fit shared between ClassifyEvents and FoldFadeRun.
struct AlignedPairCache {
    bool valid = false;
    double prevAligned[kGridH][kGridW] = {};
    double curAligned[kGridH][kGridW] = {};
    double vx = 0.0;
    double vy = 0.0;
    GridFadeFit adjFit;
    bool adjFitValid = false;
};

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

// Out-of-line in CutFadeJudge.cpp (VideoProcessor-Lib).
EventSignals ClassifyEvents(const SourceStats::Record& prev, const SourceStats::Record& cur, bool fullBetween,
                            bool continuityBetween, AlignedPairCache* outCache = nullptr);

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
                if (darkened == 0 && !(ahead->scored && ahead->score.G == 0.0)) {
                    const GridFadeFit aheadFit = FitGridFadeGrid(k->grid.g, ahead->grid.g, kShiftRadius);
                    // Duration-invariant emergence ratio from black (linear 2.0, BT.709 1.366, half-frame phase 1.258):
                    // require gain >= 1 + 1/kFadeMaxDiv (1.25) so hard cuts into dim shots with gentle +5..15% lighting rise stay cuts.
                    if (aheadFit.valid && aheadFit.dir > 0 && aheadFit.gain >= 1.0 + 1.0 / double(kFadeMaxDiv)) {
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

// ---- the multi-frame fade law (unit version 5) ---------------------------------------------------------------------------

struct FadeRun {
    int32_t direction = 0;      // +1 the picture brightens, -1 it darkens, 0 no run
    uint32_t pairs = 0;         // pairs in the run
    uint32_t fadePairs = 0;     // confirmed fade pairs inside the run
    uint32_t baseMean = 0;      // the run's anchor record: mean and p99 (P4 codes)
    uint32_t baseP99 = 0;
    uint32_t midP99 = 0;        // p99 of the midCells anchor snapshot (kept in sync across pan leapfrogs)
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

// Out-of-line in CutFadeJudge.cpp (VideoProcessor-Lib).
bool FoldFadeRun(const SourceStats::Record& prev, const SourceStats::Record& cur, const EventSignals& e, FadeRun& run,
                 double* frameKeep = nullptr, const AlignedPairCache* preAligned = nullptr);

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
