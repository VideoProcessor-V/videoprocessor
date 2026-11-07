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
//   veto grid    cut = (grid difference >= G and histogram distance >= H) or histogram distance >= H_alone;
//                entering or leaving black is a cut                                    (GridHistCut)
//   fade fit     the matcher's M4 law on two frames' mean / spread: gain = clamp(sa / sb, 0.5, 2), offset = ma - gain * mb,
//                active only inside the per-frame code window; keeps its switch in Params (FadeFit, the CPU twin of the
//                matcher's GPU FADE_BODY; the GLSL itself stays in motion/consumers/DejudderWarp.cpp)
// The indicator mapping: Strength::Full = the Indicator's FULL (cut), Strength::Minor = "cut possible".
// Bill's SceneDetector (LibplaceboVideoRenderer) is NOT replaced: it stays the Original pathway.
//
// Header-only, C++14, no Windows or GPU headers.
#pragma once

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
constexpr uint32_t kUnitVersion = 1;

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

// ---- the veto detector's rule (HdrVetoCutDetector) ---------------------------------------------------------------------

struct GridHistThresholds {
    float gridDifference = 24.0f;        // mean absolute difference of the 16 x 9 cell means (10-bit codes)
    float histogramDistance = 0.15f;
    float histogramDistanceAlone = 0.35f;
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

struct FadeParams {
    bool enabled = true;           // the matcher's fade_match switch
    float minStepCodes = 0.75f;    // |mean step| window per frame, scaled by the caller to the source rate (never a literal rate)
    float maxStepCodes = 24.0f;
    float minBlack = -12.0f;       // plausible implied black level (codes)
    float maxBlack = 48.0f;
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
    const float g = in.spreadB / (std::max)(in.spreadA, 1e-3f);
    const float black = std::fabs(1.0f - g) > 0.01f ? (in.meanB - g * in.meanA) / (1.0f - g) : 1e9f;
    const bool scaled = std::fabs(1.0f - g) > 0.01f && black > p.minBlack && black < p.maxBlack;
    const float ad = std::fabs(d), ap = std::fabs(in.previousStep);
    const bool sameSign = (d > 0.0f) == (in.previousStep > 0.0f) && (d < 0.0f) == (in.previousStep < 0.0f);
    o.fading = scaled && ad > p.minStepCodes && ad < p.maxStepCodes && ap > p.minStepCodes && sameSign &&
        in.spreadA > 1.0f && in.spreadB > 1.0f;
    o.gain = (std::min)((std::max)(in.spreadA / (std::max)(in.spreadB, 1e-3f), 0.5f), 2.0f);
    o.offset = in.meanA - o.gain * in.meanB;
    o.step = d;
    return o;
}

} // namespace CutFadeJudge
} // namespace VpUnit
