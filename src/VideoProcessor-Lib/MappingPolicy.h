#pragma once

// DV next step (f), planner answers 2026-10-17 sections 3 and 5: the mapping
// policy - which tone-curve implementation runs and which scene-descriptor
// fields it consumes - behind one "metadata -> curve parameters" interface.
// libplacebo's curve informed by L1 stays the default; a VP-owned curve is
// reserved as an option (a profile key later, not a rewrite).
//
// Pure (no renderer or GPU state). It includes libplacebo's colour-space
// header only for pl_hdr_metadata and pl_hdr_rescale, so the libplacebo
// mapping reproduces today's inputs bit for bit: the static values exactly as
// the renderer's source colour space carries them, then
// DolbyVisionLuminancePolicy::Apply for the DV tunnel.

#include <DisplayProfile.h>
#include <SceneDescriptor.h>
#include <libplacebo/colorspace.h>

#include <cstdint>
#include <cstring>

enum class CurveImplementation
{
    Libplacebo,  // default: libplacebo's tone mapping, informed by the descriptor
    VpOwned,     // reserved: a VP-owned curve for the measured display (not built;
                 // falls back to the libplacebo parameters)
};

inline const char* ToString(CurveImplementation curve)
{
    return curve == CurveImplementation::VpOwned ? "vp_owned(reserved)" : "libplacebo";
}

struct MappingPolicy
{
    CurveImplementation curve = CurveImplementation::Libplacebo;
    // Descriptor fields the curve consumes. Defaults = what libplacebo gets today.
    bool useMasteringPrimaries = true;  // pl_hdr_metadata.prim
    bool useMasteringRange = true;      // min_luma / max_luma (DM source range on DV)
    bool useContentLight = true;        // max_cll / max_fall (static sources only)
    bool useDynamicSceneStats = true;   // max_pq_y / avg_pq_y from transmitted dynamic metadata (DV L1)
    // Not consumed by libplacebo today (no input for them): reserved for a
    // VP-owned curve. Turning these on has no effect on the libplacebo mapping.
    bool useSceneMin = false;           // DV L1 minimum (shadows)
    bool useTrims = false;              // L2/L8 (the separate dv_trims stage owns them today)
    bool useMeasuredStats = false;      // VP's queued-frame statistics
};

namespace MappingPolicies
{
    inline MappingPolicy Default()
    {
        return MappingPolicy{};
    }

    // Nits of a descriptor value, converted from the transmitted domain with
    // libplacebo's own conversion (bit-identical to DolbyVisionLuminancePolicy).
    inline float LibplaceboNits(const SceneLuminance& value)
    {
        return value.fromPq ?
            pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, value.pq) : value.nits;
    }

    inline bool IsTransmittedDynamicSource(SceneMetadataSource source)
    {
        // Dv2 / Hdr10Plus join here when their parsers exist.
        return source == SceneMetadataSource::DvTunnelL1;
    }

    // (descriptor, display, policy) -> the HDR metadata libplacebo is given for
    // the source frame. The display profile does not enter libplacebo's source
    // metadata today (it is the target); it is part of the interface for a
    // VP-owned curve and a display that maps for itself.
    inline pl_hdr_metadata ToLibplaceboHdrMetadata(const SceneDescriptor& d,
        const DisplayProfile& display, const MappingPolicy& policy)
    {
        (void)display;
        pl_hdr_metadata hdr{};
        const SceneMasteringDisplay& m = d.mastering;
        if (policy.useMasteringPrimaries && m.hasPrimaries)
        {
            hdr.prim.red.x = m.redX; hdr.prim.red.y = m.redY;
            hdr.prim.green.x = m.greenX; hdr.prim.green.y = m.greenY;
            hdr.prim.blue.x = m.blueX; hdr.prim.blue.y = m.blueY;
            hdr.prim.white.x = m.whiteX; hdr.prim.white.y = m.whiteY;
        }
        if (policy.useMasteringRange)
        {
            if (m.min.present) hdr.min_luma = LibplaceboNits(m.min);
            if (m.peak.present) hdr.max_luma = LibplaceboNits(m.peak);
        }
        // Static sources only: a DV tunnel's L6 MaxCLL/MaxFALL (content intent
        // 2026-10-17, tagged DvL6) is carried for status and is not given to
        // libplacebo, so it stays out of today's mapping.
        if (policy.useContentLight)
        {
            if (d.maxCll.present && d.maxCllSource != SceneFieldSource::DvL6)
                hdr.max_cll = LibplaceboNits(d.maxCll);
            if (d.maxFall.present && d.maxFallSource != SceneFieldSource::DvL6)
                hdr.max_fall = LibplaceboNits(d.maxFall);
        }
        if (policy.useDynamicSceneStats && IsTransmittedDynamicSource(d.source) &&
            d.valid && d.HasSceneStats() && d.sceneMax.fromPq && d.sceneAvg.fromPq)
        {
            hdr.max_pq_y = d.sceneMax.pq;
            hdr.avg_pq_y = d.sceneAvg.pq;
        }
        // VpOwned (reserved) and the reserved fields: no libplacebo inputs exist
        // for them, so the parameters above are what libplacebo receives.
        return hdr;
    }

    inline bool SameFloatBits(float a, float b)
    {
        uint32_t x = 0, y = 0;
        std::memcpy(&x, &a, sizeof(x));
        std::memcpy(&y, &b, sizeof(y));
        return x == y;
    }

    // Bit-for-bit comparison of every pl_hdr_metadata field (padding ignored).
    inline bool SameHdrMetadataBits(const pl_hdr_metadata& a, const pl_hdr_metadata& b)
    {
        const pl_cie_xy* pa[4] = { &a.prim.red, &a.prim.green, &a.prim.blue, &a.prim.white };
        const pl_cie_xy* pb[4] = { &b.prim.red, &b.prim.green, &b.prim.blue, &b.prim.white };
        for (int i = 0; i < 4; ++i)
            if (!SameFloatBits(pa[i]->x, pb[i]->x) || !SameFloatBits(pa[i]->y, pb[i]->y))
                return false;
        if (!SameFloatBits(a.min_luma, b.min_luma) || !SameFloatBits(a.max_luma, b.max_luma) ||
            !SameFloatBits(a.max_cll, b.max_cll) || !SameFloatBits(a.max_fall, b.max_fall) ||
            !SameFloatBits(a.scene_avg, b.scene_avg) ||
            !SameFloatBits(a.max_pq_y, b.max_pq_y) || !SameFloatBits(a.avg_pq_y, b.avg_pq_y))
            return false;
        for (int i = 0; i < 3; ++i)
            if (!SameFloatBits(a.scene_max[i], b.scene_max[i]))
                return false;
        if (!SameFloatBits(a.ootf.target_luma, b.ootf.target_luma) ||
            !SameFloatBits(a.ootf.knee_x, b.ootf.knee_x) ||
            !SameFloatBits(a.ootf.knee_y, b.ootf.knee_y) ||
            a.ootf.num_anchors != b.ootf.num_anchors)
            return false;
        for (int i = 0; i < 15; ++i)
            if (!SameFloatBits(a.ootf.anchors[i], b.ootf.anchors[i]))
                return false;
        return true;
    }
}
