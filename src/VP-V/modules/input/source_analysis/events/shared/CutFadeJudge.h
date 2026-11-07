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
//                matcher's GPU fade test; FadeKernelHeader / FadeKernelBody are that kernel, moved byte-for-byte from
//                motion/consumers/DejudderWarp.cpp)
//   trust cut    the matcher's frame-trust cut: the share of trusted blocks below the minimum share, confirmed by the coarse
//                luma histogram distance (SourceStats::CoarseL1) above the cut threshold, or the share below 0.05 alone
//                (TrustCut; GLSL TrustCutRegionalLine / TrustCutPlainLine)
//   mean step    AdaptiveDownscale's scalar pass: first frame, or the frame's mean luma (8-bit codes) moved more than 20
//                codes from the last one (MeanStepCut; GLSL MeanStepCutLine)
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
