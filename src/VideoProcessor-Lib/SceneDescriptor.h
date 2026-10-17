#pragma once

// DV next step (f), planner answers 2026-10-17 section 5: a format-neutral
// per-frame scene descriptor. Every metadata source (DV tunnel L1/L2, later
// DV2 and HDR10+, static HDR10, LLDV static values, VP's own measurement) is
// turned into this one description by its own parser; the consumers (the
// tone-curve mapping policy, later the LED/CNTR look-ahead, Stage 12 and
// stats) read only the descriptor. DV2 = a parser + descriptor extension, not
// a rewrite.
//
// Pure: no renderer, GPU or libplacebo state. Luminance values carry the
// domain the source transmitted (PQ code or nits) so that a consumer which
// must reproduce today's libplacebo inputs bit for bit (MappingPolicy.h) can
// convert from the transmitted value, while consumers that only need a
// number read nits/pq directly (VP's own ST 2084 conversion).
//
// Wording (owner/planner 2026-10-17): this is VP's interpretation of DV
// metadata; nothing here claims Dolby Vision compliance.

#include <HDRData.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>

#include <cmath>
#include <cstdint>

namespace SceneSt2084
{
    // SMPTE ST 2084 constants.
    constexpr double kM1 = 2610.0 / 16384.0;
    constexpr double kM2 = 2523.0 / 4096.0 * 128.0;
    constexpr double kC1 = 3424.0 / 4096.0;
    constexpr double kC2 = 2413.0 / 4096.0 * 32.0;
    constexpr double kC3 = 2392.0 / 4096.0 * 32.0;

    inline float PqToNits(float pq)
    {
        if (!(pq > 0.0f))
            return 0.0f;
        const double p = std::pow(static_cast<double>(pq < 1.0f ? pq : 1.0f), 1.0 / kM2);
        const double num = p - kC1 > 0.0 ? p - kC1 : 0.0;
        const double den = kC2 - kC3 * p;
        if (!(den > 0.0))
            return 10000.0f;
        return static_cast<float>(std::pow(num / den, 1.0 / kM1) * 10000.0);
    }

    inline float NitsToPq(float nits)
    {
        if (!(nits > 0.0f))
            return 0.0f;
        const double y = static_cast<double>(nits < 10000.0f ? nits : 10000.0f) / 10000.0;
        const double p = std::pow(y, kM1);
        return static_cast<float>(std::pow((kC1 + kC2 * p) / (1.0 + kC3 * p), kM2));
    }
}

// Which source the descriptor's values came from (its source of truth).
enum class SceneMetadataSource
{
    None,         // no HDR metadata (SDR, or libplacebo infers from transfer/primaries)
    DvTunnelL1,   // HDMI DV tunnel DM packet: L1 scene stats, DM source range, L2/L8 trims
    Dv2,          // reserved: a future DV2 parser
    Hdr10Plus,    // reserved: HDR10+ VSIF (needs a capture card with infoframe access)
    Hdr10Static,  // HDR10 static InfoFrame (mastering display, MaxCLL/MaxFALL)
    LldvStatic,   // player-led DV: plain PQ, VP substitutes the static [lldv.*] values
    Measured,     // VP's own queued-frame statistics (placeholder, not produced yet)
};

inline const char* ToString(SceneMetadataSource source)
{
    switch (source)
    {
    case SceneMetadataSource::DvTunnelL1: return "dv_tunnel_l1";
    case SceneMetadataSource::Dv2: return "dv2";
    case SceneMetadataSource::Hdr10Plus: return "hdr10plus";
    case SceneMetadataSource::Hdr10Static: return "hdr10_static";
    case SceneMetadataSource::LldvStatic: return "lldv_static";
    case SceneMetadataSource::Measured: return "measured";
    default: return "none";
    }
}

// One luminance quantity with the domain the source transmitted.
struct SceneLuminance
{
    bool present = false;
    bool fromPq = false;      // true: pq (and pqCode12) is what the source sent; nits is converted
    uint16_t pqCode12 = 0;    // the 12-bit PQ code when fromPq
    float pq = 0.0f;          // ST 2084, 0..1
    float nits = 0.0f;        // cd/m2

    // Same normalisation as DolbyVisionLuminancePolicy (code / 4095).
    static SceneLuminance FromPqCode12(uint16_t code)
    {
        SceneLuminance value;
        value.present = true;
        value.fromPq = true;
        value.pqCode12 = code;
        value.pq = static_cast<float>(code) / 4095.0f;
        value.nits = SceneSt2084::PqToNits(value.pq);
        return value;
    }

    static SceneLuminance FromNits(float nits)
    {
        SceneLuminance value;
        value.present = true;
        value.nits = nits;
        value.pq = SceneSt2084::NitsToPq(nits);
        return value;
    }
};

// Optional per-display trim (DV L2 / L8 today). Codes are kept as transmitted.
struct SceneTrim
{
    uint8_t level = 0;                // 2 or 8
    uint8_t targetDisplayIndex = 0;   // L8: refers to an L10 / predefined target (unparsed)
    SceneLuminance target;            // L2: target display peak (PQ code); absent for L8
    uint16_t slope = 0;
    uint16_t offset = 0;
    uint16_t power = 0;
    uint16_t chromaWeight = 0;
    uint16_t saturationGain = 0;
    int16_t msWeight = 0;
};

// Content intent (intent interpreter item 1, 2026-10-17): which metadata
// supplied a descriptor field that more than one source can fill.
enum class SceneFieldSource
{
    None,     // not given
    Static,   // HDR10 static InfoFrame (or LLDV substitution)
    DvL5,     // DV tunnel L5 (active area)
    DvL6,     // DV tunnel L6 (MaxCLL / MaxFALL)
    DvL11,    // DV tunnel L11 (content type, reference mode, hints)
};

inline const char* ToString(SceneFieldSource source)
{
    switch (source)
    {
    case SceneFieldSource::Static: return "static";
    case SceneFieldSource::DvL5: return "dv_l5";
    case SceneFieldSource::DvL6: return "dv_l6";
    case SceneFieldSource::DvL11: return "dv_l11";
    default: return "none";
    }
}

// DV L5: the active picture as offsets from each raster edge (source pixels).
struct SceneActiveArea
{
    bool present = false;
    SceneFieldSource source = SceneFieldSource::None;
    uint16_t left = 0, right = 0, top = 0, bottom = 0;

    // The offsets leave a non-empty picture inside a width x height raster.
    bool FitsIn(int width, int height) const
    {
        return present && width > 0 && height > 0 &&
            static_cast<int>(left) + static_cast<int>(right) < width &&
            static_cast<int>(top) + static_cast<int>(bottom) < height;
    }
};

// DV L11: the content type and the grader's intended picture-mode hints, as
// transmitted (2-bit hints, 0 = no preference).
struct SceneContentIntent
{
    bool present = false;
    SceneFieldSource source = SceneFieldSource::None;
    uint8_t contentType = 0;   // 0 default, 1 movies, 2 game, 3 sport, 4 user-generated
    uint8_t whitepoint = 0;
    bool referenceMode = false;
    uint8_t sharpness = 0, noiseReduction = 0, mpegNoiseReduction = 0;
    uint8_t frameRateConversion = 0, brightness = 0, color = 0;
};

struct SceneMasteringDisplay
{
    bool hasPrimaries = false;
    float redX = 0, redY = 0, greenX = 0, greenY = 0, blueX = 0, blueY = 0;
    float whiteX = 0, whiteY = 0;
    SceneLuminance peak;
    SceneLuminance min;
    // DV: peak/min are the DM source range from the packet, not the static
    // InfoFrame fallback.
    bool rangeFromDynamicMetadata = false;
};

struct SceneDescriptor
{
    SceneMetadataSource source = SceneMetadataSource::None;
    // The source delivered usable metadata for this frame (DV: a valid DM
    // packet). A DV descriptor with valid == false keeps only the static fallback.
    bool valid = false;

    // Per-frame / per-scene statistics (DV L1, HDR10+ / DV2 later, measured).
    SceneLuminance sceneMax;
    SceneLuminance sceneAvg;
    SceneLuminance sceneMin;

    SceneMasteringDisplay mastering;
    SceneLuminance maxCll;
    SceneLuminance maxFall;
    // Content intent 2026-10-17: where maxCll / maxFall came from (static
    // InfoFrame or DV L6), and the DV L5 / L11 fields.
    SceneFieldSource maxCllSource = SceneFieldSource::None;
    SceneFieldSource maxFallSource = SceneFieldSource::None;
    SceneActiveArea activeArea;
    SceneContentIntent contentIntent;

    static constexpr uint8_t kMaxTrims = DolbyVisionTunnelFrameMetadata::kMaxTrims;
    uint8_t trimCount = 0;
    bool trimOverflow = false;
    SceneTrim trims[kMaxTrims] = {};

    bool HasSceneStats() const { return sceneMax.present && sceneAvg.present; }
};

// Placeholder for VP's queued-frame statistics (PRE_OUTPUT / look-ahead
// measurement). Not produced yet; a future LED/stats consumer fills it.
struct MeasuredSceneStats
{
    bool valid = false;
    float maxNits = 0.0f;
    float avgNits = 0.0f;
    bool hasMin = false;
    float minNits = 0.0f;
    uint32_t frameCount = 0;
};

// The four values VP substitutes on LLDV (RendererProfileConfig::LldvMetadata
// after command-line / [lldv.*] / default resolution), kept here as plain
// values so this header stays independent of the configuration code.
struct LldvStaticValues
{
    double maxCll = 0.0;
    double maxFall = 0.0;
    double masteringMinLuminance = 0.0;
    double masteringMaxLuminance = 0.0;
};

// Parser interface: one function per source. Reserved sources (Dv2,
// Hdr10Plus) get their parser when the transport delivers them.
namespace SceneDescriptorParsers
{
    // Static HDR10 metadata, with exactly the acceptance rules the renderer
    // applies when it builds libplacebo's source colour space today: a fully
    // valid InfoFrame keeps every value; an InfoFrame with zero primaries
    // keeps only the positive luminance values (min only with a max).
    inline SceneDescriptor FromStaticHdr(const HDRData* hdr)
    {
        SceneDescriptor d;
        if (!hdr)
            return d;
        d.source = SceneMetadataSource::Hdr10Static;
        if (hdr->IsValid())
        {
            d.valid = true;
            SceneMasteringDisplay& m = d.mastering;
            m.hasPrimaries = true;
            m.redX = static_cast<float>(hdr->displayPrimaryRedX);
            m.redY = static_cast<float>(hdr->displayPrimaryRedY);
            m.greenX = static_cast<float>(hdr->displayPrimaryGreenX);
            m.greenY = static_cast<float>(hdr->displayPrimaryGreenY);
            m.blueX = static_cast<float>(hdr->displayPrimaryBlueX);
            m.blueY = static_cast<float>(hdr->displayPrimaryBlueY);
            m.whiteX = static_cast<float>(hdr->whitePointX);
            m.whiteY = static_cast<float>(hdr->whitePointY);
            m.min = SceneLuminance::FromNits(static_cast<float>(hdr->masteringDisplayMinLuminance));
            m.peak = SceneLuminance::FromNits(static_cast<float>(hdr->masteringDisplayMaxLuminance));
            d.maxCll = SceneLuminance::FromNits(static_cast<float>(hdr->maxCll));
            d.maxFall = SceneLuminance::FromNits(static_cast<float>(hdr->maxFall));
            d.maxCllSource = d.maxFallSource = SceneFieldSource::Static;
            return d;
        }
        if (hdr->maxCll > 0)
        {
            d.maxCll = SceneLuminance::FromNits(static_cast<float>(hdr->maxCll));
            d.maxCllSource = SceneFieldSource::Static;
        }
        if (hdr->maxFall > 0)
        {
            d.maxFall = SceneLuminance::FromNits(static_cast<float>(hdr->maxFall));
            d.maxFallSource = SceneFieldSource::Static;
        }
        if (hdr->masteringDisplayMaxLuminance > 0)
        {
            d.mastering.peak = SceneLuminance::FromNits(
                static_cast<float>(hdr->masteringDisplayMaxLuminance));
            if (hdr->masteringDisplayMinLuminance > 0)
                d.mastering.min = SceneLuminance::FromNits(
                    static_cast<float>(hdr->masteringDisplayMinLuminance));
        }
        d.valid = d.maxCll.present || d.maxFall.present || d.mastering.peak.present;
        return d;
    }

    // LLDV: the player sends plain PQ without metadata; VP substitutes the
    // static values (the GUI writes them over the capture HDRData, keeping its
    // primaries). existing = the capture's HDRData before substitution, or null.
    inline SceneDescriptor FromLldvStatic(const LldvStaticValues& values, const HDRData* existing)
    {
        HDRData substituted;
        if (existing)
            substituted = *existing;
        substituted.maxCll = values.maxCll;
        substituted.maxFall = values.maxFall;
        substituted.masteringDisplayMinLuminance = values.masteringMinLuminance;
        substituted.masteringDisplayMaxLuminance = values.masteringMaxLuminance;
        SceneDescriptor d = FromStaticHdr(&substituted);
        d.source = SceneMetadataSource::LldvStatic;
        return d;
    }

    // HDMI DV tunnel DM packet. staticFallback = the static descriptor for the
    // same frame (FromStaticHdr); it supplies primaries and, when the packet's
    // source range is unusable, the mastering range. Validity rules are those
    // of DolbyVisionLuminancePolicy::Apply / SceneMinNits; MaxCLL/MaxFALL come
    // only from an L6 block (content intent 2026-10-17; tagged DvL6, and not
    // given to libplacebo: MappingPolicy skips them); the DM source range is used when max > min and
    // max <= 4095; L1 max/avg when present and <= 4095; the L1 minimum
    // additionally needs min <= 4095.
    inline SceneDescriptor FromDolbyVisionTunnel(const DolbyVisionTunnelFrameMetadata& dm,
        const SceneDescriptor& staticFallback)
    {
        SceneDescriptor d;
        d.source = SceneMetadataSource::DvTunnelL1;
        d.valid = dm.valid;
        d.mastering = staticFallback.mastering;
        d.mastering.rangeFromDynamicMetadata = false;
        if (!dm.valid)
            return d;
        if (dm.sourceMaxPq > dm.sourceMinPq && dm.sourceMaxPq <= 4095)
        {
            d.mastering.min = SceneLuminance::FromPqCode12(dm.sourceMinPq);
            d.mastering.peak = SceneLuminance::FromPqCode12(dm.sourceMaxPq);
            d.mastering.rangeFromDynamicMetadata = true;
        }
        if (dm.hasSceneMetadata && dm.sceneMaxPq <= 4095 && dm.sceneAvgPq <= 4095)
        {
            d.sceneMax = SceneLuminance::FromPqCode12(dm.sceneMaxPq);
            d.sceneAvg = SceneLuminance::FromPqCode12(dm.sceneAvgPq);
            if (dm.sceneMinPq <= 4095)
                d.sceneMin = SceneLuminance::FromPqCode12(dm.sceneMinPq);
        }
        const int limit = SceneDescriptor::kMaxTrims;
        const int count = dm.trimCount < limit ? static_cast<int>(dm.trimCount) : limit;
        for (int i = 0; i < count; ++i)
        {
            const DolbyVisionTunnelFrameMetadata::Trim& in = dm.trims[i];
            SceneTrim& out = d.trims[d.trimCount++];
            out.level = in.level;
            out.targetDisplayIndex = in.targetDisplayIndex;
            if (in.level == 2 && in.targetMaxPq <= 4095)
                out.target = SceneLuminance::FromPqCode12(in.targetMaxPq);
            out.slope = in.slope;
            out.offset = in.offset;
            out.power = in.power;
            out.chromaWeight = in.chromaWeight;
            out.saturationGain = in.saturationGain;
            out.msWeight = in.msWeight;
        }
        d.trimOverflow = dm.trimOverflow || dm.trimCount > SceneDescriptor::kMaxTrims;
        // Content intent 2026-10-17: L6 / L5 / L11, each tagged with its level.
        if (dm.hasContentLight && dm.maxCll > 0 && dm.maxCll <= 10000 && dm.maxFall <= dm.maxCll)
        {
            d.maxCll = SceneLuminance::FromNits(static_cast<float>(dm.maxCll));
            d.maxCllSource = SceneFieldSource::DvL6;
            if (dm.maxFall > 0)
            {
                d.maxFall = SceneLuminance::FromNits(static_cast<float>(dm.maxFall));
                d.maxFallSource = SceneFieldSource::DvL6;
            }
        }
        if (dm.hasActiveArea)
        {
            d.activeArea.present = true;
            d.activeArea.source = SceneFieldSource::DvL5;
            d.activeArea.left = dm.activeAreaLeft;
            d.activeArea.right = dm.activeAreaRight;
            d.activeArea.top = dm.activeAreaTop;
            d.activeArea.bottom = dm.activeAreaBottom;
        }
        if (dm.hasL11)
        {
            SceneContentIntent& c = d.contentIntent;
            c.present = true;
            c.source = SceneFieldSource::DvL11;
            c.contentType = dm.l11ContentType;
            c.whitepoint = dm.l11Whitepoint;
            c.referenceMode = dm.l11ReferenceMode;
            c.sharpness = dm.l11Sharpness;
            c.noiseReduction = dm.l11NoiseReduction;
            c.mpegNoiseReduction = dm.l11MpegNoiseReduction;
            c.frameRateConversion = dm.l11FrameRateConversion;
            c.brightness = dm.l11Brightness;
            c.color = dm.l11Color;
        }
        return d;
    }

    // VP's measurement (placeholder): scene statistics in nits, keeping the
    // static mastering data of the same frame.
    inline SceneDescriptor FromMeasured(const MeasuredSceneStats& stats,
        const SceneDescriptor& staticFallback)
    {
        SceneDescriptor d;
        d.source = SceneMetadataSource::Measured;
        d.mastering = staticFallback.mastering;
        d.maxCll = staticFallback.maxCll;
        d.maxFall = staticFallback.maxFall;
        d.maxCllSource = staticFallback.maxCllSource;
        d.maxFallSource = staticFallback.maxFallSource;
        d.valid = stats.valid;
        if (!stats.valid)
            return d;
        d.sceneMax = SceneLuminance::FromNits(stats.maxNits);
        d.sceneAvg = SceneLuminance::FromNits(stats.avgNits);
        if (stats.hasMin)
            d.sceneMin = SceneLuminance::FromNits(stats.minNits);
        return d;
    }
}
