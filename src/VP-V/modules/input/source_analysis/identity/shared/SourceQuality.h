// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared unit U12 "source quality / ABR" (doc 62 section 1.2): the one law that turns the classical bitrate reading and the
// identity consensus record into "how good is this source" (tier, quality, confidence, ABR drop seen). It is the pure core
// under SourceQualityView (the P14 read API) and the vote / pipeline code; the P4 field `quality` carries its output (no new
// product id). Consumers: deband k_bitrate, every repair's "get out of the way" test, TD admission, dither need.
//
// Law (moved unchanged from SourceQualityView.h, SourceIdVote.h and SourceIdPipeline.h):
//   tier        = the number of tier edges (0.25, 0.5, 0.75) that the quality reaches; -1 when there is no reading
//   consensus   quality = min(classical quality, the record's event-anchored level), tier = the record's latched tier
//   classical   quality = the snapshot's smoothed bitrate quality, tier from it (consensus off or the record unusable)
//   dither need = 1 - quality, clamped to 0..1
//   contrast recovery gates OFF on a clean source: the best tier and quality >= cleanQuality
// The caller validates the record (version, age, latched) and decides the visible fallback; this header holds no catalog and
// no clock. Header-only, C++14, no Windows or GPU headers. No GLSL (the plan has none for U12).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace VpUnit
{
namespace SourceQuality
{
// Bump on ANY change of a tier edge, the combination rule or the meaning of an output.
constexpr uint32_t kUnitVersion = 1;

// The single source of the defaults (VoteSettings and SourceQualityView take them from here).
inline std::vector<double> DefaultTierEdges() { return { 0.25, 0.5, 0.75 }; }   // tier = number of edges <= quality
constexpr double kClassicalConfidence = 0.6;   // a bitrate reading is indirect evidence of the tier
constexpr double kCleanQuality = 0.9;          // contrast recovery gates off above this on the best tier

struct Params {
    std::vector<double> tierEdges = DefaultTierEdges();
    double classicalConfidence = kClassicalConfidence;
    double cleanQuality = kCleanQuality;
};

struct In {
    bool reading = false;            // the classical detector has a reading
    double bitrateQuality = 1.0;     // spec 1b, smoothed (what every consumer read before)
    bool consensus = false;          // a usable sealed record (fresh, latched, same version) stands behind the next fields
    double recordLevel = 1.0;        // record.level (1 until an ABR event)
    int recordTier = -1;             // record.tier
    double recordConfidence = 0.0;   // record.confidence[Quality]
    bool abrDropSeen = false;        // an ABR Drop is the newest event of the fresh record
};

struct Out {
    double quality = 1.0;            // 0..1, 1 = clean
    int tier = -1;                   // 0 = lowest ... edges.size() = best; -1 = unknown
    double confidence = 0.0;         // support of the tier (0 when unknown)
    bool consensus = false;
    bool abrDropSeen = false;
};

inline int TierOf(double quality, const std::vector<double>& edges)
{
    int tier = 0;
    for (size_t i = 0; i < edges.size(); ++i)
        if (quality >= edges[i])
            ++tier;
    return tier;
}

// The classical reading capped by the record's level (a quiet stream reads exactly as before: level is 1).
inline double Combine(double classicalQuality, double level) { return (std::min)(classicalQuality, level); }

// The CPU reference of the unit.
inline void Compute(const In& in, const Params& p, Out& out)
{
    out = Out();
    out.quality = in.bitrateQuality;
    out.tier = in.reading ? TierOf(in.bitrateQuality, p.tierEdges) : -1;
    out.confidence = in.reading ? p.classicalConfidence : 0.0;
    if (!in.consensus)
        return;
    out.consensus = true;
    out.quality = Combine(in.bitrateQuality, in.recordLevel);
    out.tier = in.recordTier;
    out.confidence = in.recordConfidence;
    out.abrDropSeen = in.abrDropSeen;
}

// Dither need in 0..1: the lower the quality, the more a step is visible. Smooth, monotone.
inline double DitherNeed(double quality) { return (std::min)(1.0, (std::max)(0.0, 1.0 - quality)); }

// Contrast recovery gates OFF on a clean high-bitrate source (get out of the way): the best tier and a high quality.
inline bool ContrastRecoveryActive(int tier, double quality, const Params& p)
{
    const int top = static_cast<int>(p.tierEdges.size());
    return !(tier >= top && quality >= p.cleanQuality);
}

// ---- the one access to a monitor snapshot's classical quality (any struct with bitrateQuality / effectiveBits) -----------

template <class Snapshot>
inline double Of(const Snapshot& s) { return s.bitrateQuality; }

template <class Snapshot>
inline void Store(Snapshot& s, double quality, double effectiveBits)
{
    s.bitrateQuality = quality;
    s.effectiveBits = effectiveBits;
}

// Copies the classical quality of `from` (a snapshot) into `to` (a vote reading or a view input).
template <class To, class Snapshot>
inline void CopyClassical(To& to, const Snapshot& from) { to.bitrateQuality = from.bitrateQuality; }

} // namespace SourceQuality
} // namespace VpUnit
