// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared unit U7 "cut / fade / flash judge" (doc 62 section 1.2): the one decision of "the picture changed" derived from two
// P4 records, and the veto detector's grid + histogram cut rule. Moved unchanged from the lane (ClassifyEvents) and from
// HdrVetoCutDetector::Observe so both call ONE implementation. Cut/fade/flash are never stored: every consumer derives them.
//
// Law (all thresholds relative to the frames' own spread (p99 - p1) or to normalised histogram mass):
//   record pair  cut when the lane saw a FULL indicator; otherwise histogram distance > 0.50 is a cut (a shifted histogram
//                of kept spread is a flash), > 0.25 or a continuity slip is "cut possible", a small same-sign step of
//                mean and spread is a fade                                              (ClassifyEvents)
//   fade run     (v3, 102b U7-MULTIFRAME-FADE-LAW) a slow fade moves each pair by much less than spread / 64, so the
//                pair law alone misses most of it. A run is the consecutive record pairs whose p99 moves one way (a pair whose
//                p99 holds stays in it; a cut, a flash, a reversal or a mean jump >= spread / 4 ends it; a repeated record leaves
//                it as it is). A pair inside a run of two or more pairs is a fade when its mean and p99 both moved the run's way
//                and the run, from its first record to this one, is a gain about a plausible black: the black implied by mean
//                and p99 lies in FadeFit's window (kFadeMinBlack .. kFadeMaxBlack, 8-bit-like codes) and the gain differs from 1
//                by more than kFadeRunMinGain. No frame rate, fade length or window length enters: the evidence is the
//                accumulated gain, so a fade of any length at any rate is found once it has scaled the picture by 2 %. The
//                caller keeps one FadeRun per stream and folds every pair in order; the pair law's fade still counts on its own
//                                                                                       (FoldFadeRun)
//   fade keep    (v3, owner 2026-11-15: partial reset on fades) a fade is not a restart: each fade frame's history keep is the
//                share of contrast that frame kept (the run's min(gain, 1 / gain) step, the pair's spread ratio; the smaller of
//                the laws that fired), so over a fade the history shrinks monotonically to the contrast the pre-fade picture
//                still explains; 1 on a frame that is no fade               (FoldFadeRun frameKeep, PairFadeKeep, FrameFadeKeep)
//   veto grid    cut = (grid difference >= G and histogram distance >= H) or histogram distance >= H_alone;
//                entering or leaving black is a cut                                    (GridHistCut)
//   fade fit     the matcher's M4 law on two frames' mean / spread: gain = clamp(sa / sb, 0.5, 2), offset = ma - gain * mb,
//                active only inside the per-frame code window; keeps its switch in Params (FadeFit, the CPU twin of the
//                matcher's GPU fade test; FadeKernelHeader / FadeKernelBody are that kernel, moved byte-for-byte from
//                motion/estimation/matcher/DejudderWarp.cpp)
//   trust cut    the matcher's frame-trust cut: the share of trusted blocks below the minimum share, confirmed by the coarse
//                luma histogram distance (SourceStats::CoarseL1) above the cut threshold, or the share below 0.05 alone
//                (TrustCut; GLSL TrustCutRegionalLine / TrustCutPlainLine)
//   mean step    AdaptiveDownscale's scalar pass: first frame, or the frame's mean luma (8-bit codes) moved more than 20
//                codes from the last one (MeanStepCut; GLSL MeanStepCutLine)
//   sequence     (v2) the cut law T2 over the P4 cell grids of frames k-4..k+1: a local-motion-tolerant residual spike
//                against its temporal context, with structure kept (a lighting change) or a return at k+1 judged a
//                flash; one frame late, provisional without k+1 (ScorePair / CutTracker, below)
// The indicator mapping: Strength::Full = the Indicator's FULL (cut), Strength::Minor = "cut possible".
// Bill's SceneDetector (LibplaceboVideoRenderer) is NOT replaced: it stays the Original pathway.
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
// 2 (U7-accuracy, 2026-11-15): the record-sequence cut law T2 (CutTracker) over the P4 v2 cell grid; the pair law is kept.
// 3 (C5, 2026-11-15; 102b U7-MULTIFRAME-FADE-LAW + U7-FADE-NO-HARDRESTART): the multi-frame fade law (FadeRun / FoldFadeRun) and the
//   partial reset on fades (FadeRun::keep, PairFadeKeep, FrameFadeKeep; a fade is no restart). The cut law of 2 is unchanged.
constexpr uint32_t kUnitVersion = 3;

constexpr double kCutHistFrac = 0.50;          // normalised L1 distance / 2 above this: a different picture
constexpr double kCutPossibleHistFrac = 0.25;  // above this (or a continuity slip): a cut is possible
constexpr int64_t kFadeMinDiv = 64;            // fade: |mean step| >= spread / 64 ...
constexpr int64_t kFadeMaxDiv = 4;             // ... and < spread / 4 (more is a jump, not a ramp)
constexpr int64_t kFlashMeanDiv = 4;           // flash: |mean step| >= spread / 4 with the spread kept within spread / 4

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
    // Gradual ramp: mean and contrast move together by a small step.
    const bool sameSign = (dMean > 0 && dSpread > 0) || (dMean < 0 && dSpread < 0);
    if (aMean * kFadeMinDiv >= spread && aMean * kFadeMaxDiv < spread && sameSign)
        e.fade = true;
    return e;
}


enum class Strength { None, Minor, Full };

inline Strength StrengthOf(const EventSignals& e) { return e.cut ? Strength::Full : e.cutPossible ? Strength::Minor : Strength::None; }

// ---- the record-sequence cut law "T2" (U7-accuracy, v2) ------------------------------------------------------------------
// Inputs: the P4 v2 cell grid (32 x 18 cell means / 256) and histogram of each frame. Per adjacent pair (ScorePair):
//   L  local residual: the previous grid gain/offset-fitted to the current one (least squares), then per cell the smallest
//      |cur - fitted prev| over a +-2-cell neighbourhood (local motion tolerance), averaged over the interior cells
//   G  mean |cur - prev| over the cells;  H  half L1 distance of the normalised histograms
//   S  structure correlation: Pearson correlation of the log-grid gradients (log(g + 4), horizontal and vertical
//      differences, each centred on its own mean, then pooled), the best over +-1-cell shifts (1 = same structure; a lighting change keeps it, a new shot does not)
//   F  flatness of one frame: RMS of its log-grid gradients (F < 0.02: a flat / black frame without structure)
// The decision for frame k (CutTracker) needs frame k+1 (the verdict is one frame late; without k+1 it is provisional):
//   context  ctx = the largest L of frames k-3..k-1 (not part starts, cuts, flashes or lit frames) and of frame k+1
//            (unless lit); lit(j) = S[j] >= 0.4 with neither j nor j-1 flat
//   candidate  spike: L >= 6 and L >= 1.8 ctx + 1; or the low tier L >= 4, L >= 1.8 ctx + 1, k and k-1 not flat and S < 0.2;
//              or hard: H > 0.5 and G >= 6
//   verdict  ref = the last non-flat frame before k (up to 4 back). Flat k: a flash when k+1 returns to ref's structure,
//            else a cut unless k-1 was a flash. Otherwise a flash when ref and k share structure (lighting), or when k+1
//            returns to k-1 (grid and histogram within a third of the step); no cut right after a non-flat flash; else a cut.
// strength = 1 on a cut, else min(1, L / (1.8 ctx + 1)) when L >= 6 (the soft evidence; 0 otherwise).
// A frame without a grid (gridFound = 0, a plane under 32 x 18) falls back to ClassifyEvents (cut / flash, always confirmed).
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
    // A flat previous grid fits as the current grid's mean (the least-squares minimum-norm fit gives the same values).
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

// The Pearson correlation of prev's gradient field shifted by (sy, sx) against cur's over the overlap. Each component (horizontal,
// vertical) is centred on its OWN mean before the two are pooled: pooled under one mean, a picture ramping left to right and another
// ramping top to bottom correlate (their mean gradients differ per component), and a new shot was read as the same structure.
inline double ShiftedCorrelation(const Gradients& p, const Gradients& c, int sy, int sx)
{
    // Pairs: p at (y + sy', x + sx') with c at (y, x), where for a shift s >= 0 p starts at s and c at 0 (and the reverse).
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
    bool known = false;       // a verdict exists for `seq`
    bool confirmed = false;   // judged with frame seq+1 (or the part ended at seq); false = provisional
    bool cut = false;
    bool flash = false;
    bool candidate = false;   // cut possible: the frame passed the candidate rule (cut, flash, or vetoed)
    bool partStart = false;   // first frame after a discontinuity / reset: never a cut
    bool gridLaw = false;     // T2 judged it (false: the ClassifyEvents fallback, no grid)
    float strength = 0.0f;    // 0..1 (1 = cut)
    float bar = 0.0f;         // the spike bar the frame was judged against (1.8 ctx + 1; 0: not judged by the grid law), for margins
    PairScore score;
};

// The stateful judge over the record sequence. Push each frame's P4 record in order; the return value is the CONFIRMED
// verdict of the previous frame (known = false when there is none). Provisional() judges the last pushed frame without
// its successor. A non-contiguous seq ends the part: the pending frame is judged without a successor and the new frame
// starts a new part.
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
            out.confirmed = true;   // final: its successor was seen, or the part ended at it
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
        SourceStats::Record rec;   // only kept for the no-grid fallback
        bool fallbackRec = false;
        bool partStart = false;
        bool flat = false;
        double F = 1.0;
        PairScore score;           // this frame against the one before it (scored at its own Push)
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

    const Entry* Back(int i) const   // i = 1: the frame before the pending one, ... (nullptr past the part)
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

    // Judge `k` (the pending frame, its score against k-1 already set) with its successor `ahead` (nullptr: none).
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
            // No grid on this pair: the pair law (ClassifyEvents) on the two records.
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
        // Context: back frames (not starts, cuts, flashes or lit) and the ahead frame (unless lit).
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
        v.candidate = true;
        // ref = the last non-flat frame before k (stops at a part start).
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
            v.flash = true;   // structure kept: a lighting change
        } else if (aheadScored && ahead->grid.valid
                   && MeanCellDifference(prev->grid, ahead->grid) * kReturnFraction < k->score.G
                   && HistHalfL1(prev->grid, ahead->grid) * kReturnFraction < k->score.H) {
            v.flash = true;   // k+1 returns to k-1
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

// ---- the veto detector's rule (HdrVetoCutDetector) ---------------------------------------------------------------------

// The veto detector's thresholds, tuned on the HFR clips sampled at 10-20 Hz (the single source; HdrVetoCutDetector aliases them).
constexpr float kGridDifference = 24.0f;          // mean absolute difference of the 16 x 9 cell means (10-bit codes)
constexpr float kHistogramDistance = 0.15f;
constexpr float kHistogramDistanceAlone = 0.35f;

struct GridHistThresholds {
    float gridDifference = kGridDifference;
    float histogramDistance = kHistogramDistance;
    float histogramDistanceAlone = kHistogramDistanceAlone;
};

// Mean absolute difference of two equally long arrays (the grid difference), summed in index order.
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

struct FadeParams {
    bool enabled = true;                         // the matcher's fade_match switch
    float minStepCodes = kFadeMinStepCodes;      // |mean step| window per frame, scaled by the caller to the source rate (never a literal rate)
    float maxStepCodes = kFadeMaxStepCodes;
    float minBlack = kFadeMinBlack;              // plausible implied black level (codes)
    float maxBlack = kFadeMaxBlack;
};

struct FadeIn {
    float meanA = 0, meanB = 0, spreadA = 0, spreadB = 0;   // picture mean and standard deviation of A and B (8-bit-like codes)
    float previousStep = 0;                                  // the previous pair's mean step (sign and size must continue)
};

struct FadeOut {
    bool fading = false;
    float gain = 1.0f;      // multiply B by gain ...
    float offset = 0.0f;    // ... and add offset to match A
    float step = 0.0f;      // mean step B - A
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

// ---- the multi-frame fade law (unit version 3) ---------------------------------------------------------------------------

constexpr double kFadeRunMinGain = 0.02;      // |1 - gain| of the run (first record -> this one) above this: the picture was scaled
constexpr double kP4CodesPerEightBit = 256.0; // P4 records hold 16-bit half-plane codes: one 8-bit-like code = 256 P4 codes
constexpr uint32_t kFadeRunMinPairs = 2;      // a run needs two pairs (as FadeFit needs the previous pair's step): one pair is not a ramp

// The run that ends at one record (the caller keeps one per stream, starting empty). end* name that record, so a run is only
// continued by the pair that starts there (a restarted frame numbering or a skipped frame starts a new run).
struct FadeRun {
    int32_t direction = 0;      // +1 the picture brightens (p99 rises), -1 it darkens, 0 no run
    uint32_t pairs = 0;         // pairs in the run
    uint32_t baseMean = 0;      // the run's first record: mean and p99 (P4 codes)
    uint32_t baseP99 = 0;
    uint32_t refSpread = 0;     // the largest p99 - p01 inside the run (the mean-jump limit's reference)
    uint32_t endPixels = 0;     // the record the run ends at
    uint32_t endMean = 0, endP01 = 0, endP50 = 0, endP99 = 0;
    // The partial reset (v3): the share of the run's first picture's contrast the picture at the run's end still has, as the run's
    // fades measured it (min(gain, 1 / gain) about the fitted black, never rising inside the run). 1 = nothing reduced yet.
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

// Fold the pair (prev, cur), whose pair verdict is `e` (ClassifyEvents on the same two records), into `run` (the run ending at
// prev; any other run is dropped first). Returns the multi-frame fade verdict of the pair; the frame's fade is this OR e.fade.
// `frameKeep` (v3, may be null) gets the run's partial-reset factor of this frame: 1 unless the pair is a run fade, else
// keep_after / keep_before of the run (see FadeRun::keep), in (0, 1]; 0 when the run has scaled the picture to its black.
inline bool FoldFadeRun(const SourceStats::Record& prev, const SourceStats::Record& cur, const EventSignals& e, FadeRun& run,
                        double* frameKeep = nullptr)
{
    if (frameKeep)
        *frameKeep = 1.0;
    if (!FadeRunEndsAt(run, prev))
        run = FadeRun();
    if (e.cut || e.flash) {
        run = FadeRun();
        FadeRunEndAt(run, cur);
        return false;
    }
    const int64_t dMean = int64_t(cur.mean) - int64_t(prev.mean);
    const int64_t dP99 = int64_t(cur.p99) - int64_t(prev.p99);
    if (dMean == 0 && dP99 == 0 && e.histDistance == 0) {
        FadeRunEndAt(run, cur);   // a repeated record: nothing changed, the run stays as it is
        return false;
    }
    const uint32_t spreadPrev = prev.p99 > prev.p01 ? prev.p99 - prev.p01 : 0u;
    const uint32_t spreadCur = cur.p99 > cur.p01 ? cur.p99 - cur.p01 : 0u;
    const uint32_t spread = (std::max)(1u, (std::max)(spreadPrev, spreadCur));
    const int32_t dir = dP99 > 0 ? 1 : dP99 < 0 ? -1 : 0;
    const int64_t aMean = dMean < 0 ? -dMean : dMean;
    const bool continues = run.direction != 0 && (dir == run.direction || dir == 0) &&
        aMean * kFadeMaxDiv < int64_t((std::max)(run.refSpread, spread));
    if (!continues) {
        run = FadeRun();
        if (dir == 0 || aMean * kFadeMaxDiv >= int64_t(spread)) {   // nothing to start from, or a jump: no ramp starts here
            FadeRunEndAt(run, cur);
            return false;
        }
        run.direction = dir;
        run.baseMean = prev.mean;
        run.baseP99 = prev.p99;
        run.refSpread = spread;
    }
    ++run.pairs;
    run.refSpread = (std::max)(run.refSpread, spread);
    FadeRunEndAt(run, cur);
    if (run.pairs < kFadeRunMinPairs || dir != run.direction || dMean * run.direction <= 0)
        return false;
    // A gain about a black b: (cur - b) = g * (base - b) for both the mean and the p99.
    const double m0 = double(run.baseMean), p0 = double(run.baseP99), m1 = double(cur.mean), p1 = double(cur.p99);
    const double den = (p0 - p1) - (m0 - m1);
    if (den == 0.0)
        return false;
    const double black = (m1 * p0 - m0 * p1) / den;
    if (!(black > double(kFadeMinBlack) * kP4CodesPerEightBit && black < double(kFadeMaxBlack) * kP4CodesPerEightBit) || p0 <= black ||
        m0 <= black)
        return false;
    const double gain = (p1 - black) / (p0 - black);
    const bool fade = std::fabs(1.0 - gain) > kFadeRunMinGain && (gain - 1.0) * double(run.direction) > 0.0 && (m1 - m0) * double(run.direction) > 0.0;
    if (fade) {
        // The partial reset (v3): the contrast the run's first picture still explains, min(gain, 1 / gain) (a fade out: the gain;
        // a fade in: its inverse). Never rising inside a run, so the history's reduction over the run is monotone.
        const double k = gain <= 0.0 ? 0.0 : (std::min)(gain, 1.0 / gain);
        if (k < run.keep) {
            if (frameKeep)
                *frameKeep = run.keep > 0.0 ? k / run.keep : 0.0;
            run.keep = k;
        }
    }
    return fade;
}

// ---- the partial reset on fades (unit version 3; owner 2026-11-15 09:20, 102b U7-FADE-NO-HARDRESTART) ---------------------------
// A fade is NOT a restart: no consumer starts over mid-fade. Each fade frame reduces the history by the share of the picture's
// contrast that changed on that frame, so the pre-fade picture's detail is not carried into what the fade leads to, and the
// product of the factors over a fade is the contrast the pre-fade picture still explains (a fade to black: 0). A full restart
// stays for cuts (the sequence law's delivered cut) only.
//   keep(frame) = min(run factor (FoldFadeRun's frameKeep), pair factor (PairFadeKeep)) over the laws that called the frame a fade;
//                 1 when it is no fade. Amplitude domain; a consumer holding energies (squared codes) applies keep^2.
// No frame count, rate or window enters: the factor is the measured contrast change of the frame itself.

// The pair law's factor for a pair it calls a fade: the smaller spread over the larger (p99 - p01, the pair law's own measure).
inline double PairFadeKeep(const SourceStats::Record& prev, const SourceStats::Record& cur)
{
    const uint32_t sp = prev.p99 > prev.p01 ? prev.p99 - prev.p01 : 0u;
    const uint32_t sc = cur.p99 > cur.p01 ? cur.p99 - cur.p01 : 0u;
    const uint32_t lo = (std::min)(sp, sc), hi = (std::max)(sp, sc);
    return hi == 0u ? 1.0 : double(lo) / double(hi);
}

// The frame's history keep (see above): pairFade / runFade = the two laws' verdicts, runKeep = FoldFadeRun's frameKeep.
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

constexpr float kTrustCutShareAlone = 0.05f;   // a trusted-block share below this is a cut without the histogram's word
constexpr float kMeanStepCutCodes = 20.0f;     // AdaptiveDownscale: the frame's mean luma (8-bit codes) moved more than this = a cut

// The matcher's frame-trust cut. `regional` = the histogram confirmation is available (dejudder_regional_hold); without it a
// share below the minimum alone is a cut. histDistance = SourceStats::CoarseL1, cutHistogram = the dejudder_cut_histogram setting.
inline bool TrustCut(float share, float minShare, float histDistance, float cutHistogram, bool regional)
{
    return regional ? (share < minShare && (histDistance > cutHistogram || share < kTrustCutShareAlone)) : share < minShare;
}

// AdaptiveDownscale's cut: no state yet (previousInitialised < 0.5, the state's alpha channel) or the mean luma stepped.
inline bool MeanStepCut(float previousInitialised, float previousMean, float mean)
{
    return previousInitialised < 0.5f || std::fabs(mean - previousMean) > kMeanStepCutCodes;
}

// ---- GLSL -----------------------------------------------------------------------------------------------------------------

// The matcher's M4 fade test, byte-for-byte what the matcher used to carry (FADE_HEADER / FADE_BODY): one work group of 256 on
// the 1/32 pyramids (samplers l2_a, l2_b, optionally `picture` under PICTURE, state image state_io, uniforms fade_lo / fade_hi
// = the per-frame step window kFadeMinStepCodes / kFadeMaxStepCodes scaled by the caller to the source rate). state_io holds
// (gain, offset, active, this pair's mean step); the CPU twin is FadeFit. Both literals begin with a newline.
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

// The trust pass's cut lines (FRAME_TRUST_BODY), spliced by the matcher under its own `#ifdef REGIONAL` / `#else`; they read
// share, min_share, hist_d (CoarseHistBody) and the uniform cut_hist. No leading newline, one line each.
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

// AdaptiveDownscale's FINAL_BODY cut line, spliced after `vec4 prev = imageLoad(state_io, ivec2(0));` (prev.a = initialised,
// prev.b = the last mean luma) and before the scale smoothing. No leading newline.
inline const char* MeanStepCutLine()
{
    return R"GLSL(    bool cut = prev.a < 0.5 || abs(luma_mean - prev.b) > 20.0;
)GLSL";
}

} // namespace CutFadeJudge
} // namespace VpUnit
