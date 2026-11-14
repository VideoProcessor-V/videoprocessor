// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The VariantKey of a skeleton product and the precision axis values (L-CORE; split out of core/SkeletonAcquire.h unchanged so that
// core/QueueWindow.h can cost a variant: design-queue-keyed-products s9.0 A5 / audit S11a). Contract: lead8/iface-L-CORE.md s4.
// Header-only, C++14.

#include <cstdint>
#include <string>
#include <tuple>

namespace VpSkeleton {

// The PRECISION axis values (L-CORE, advisor A6 + coordinator 2026-11-14 05:11; 00-UNIVERSAL rule 7 "12-BIT END TO END"):
//   kPrecisionDefault  the owner's own form;
//   kPrecisionUnorm16  R16 unorm at FULL 16-bit scale (msb-aligned code: 10-bit << 6, 12-bit << 4); exact for sources <= 14
//                      bits including a 2x2 mean (2 extra fraction bits);
//   kPrecisionFloat32  R32F in the plane's normalised units (0..1 as sampled);
//   kPrecisionFloat16  R16F: an 11-bit mantissa, NOT 12-bit safe; never for pixel values (OwnerFill::pixelData refuses it).
enum : uint8_t { kPrecisionDefault = 0, kPrecisionUnorm16 = 1, kPrecisionFloat32 = 2, kPrecisionFloat16 = 3 };

inline const char* PrecisionName(uint8_t p)
{
    switch (p) {
    case kPrecisionDefault: return "default";
    case kPrecisionUnorm16: return "unorm16";
    case kPrecisionFloat32: return "float32";
    case kPrecisionFloat16: return "float16";
    }
    return "?";
}
// True when a pixel value stored at precision `p` keeps every code of a 12-bit pipeline (a 2x2 mean included).
inline bool PrecisionTwelveBitSafe(uint8_t p) { return p == kPrecisionUnorm16 || p == kPrecisionFloat32; }

// The variant axes (refs/126 s1.2; L-CORE adds STAGE, CROP, PARAM0/1 and the precision values). A zero field is "the
// product's default"; VariantKey{} is the default variant (the lane's form). Keys are CANONICAL before use (Canonical(): a
// field equal to the owner's declared default is 0, a size with one zero side is no size, a crop with a zero side or equal to
// the owner's full frame is no crop), so two spellings of one variant share one spot. Contract: lead8/iface-L-CORE.md s4.
struct VariantKey {
    uint8_t plane = 0;         // 0 = default plane (luma); 1.. = plane index + 1
    uint32_t width = 0;        // 0 = the product's own size
    uint32_t height = 0;
    uint8_t scale = 0;         // GrainScale / pyramid level, 0 = default
    uint8_t codeUnit = 0;      // code-unit form (bits), 0 = default
    uint32_t sigmaMilli = 0;   // band sigma x 1000, 0 = default
    uint8_t precision = 0;     // storage precision: kPrecision* above, 0 = default
    // STAGE (coordinator ruling iface-L-B3 5b, OD 11-14 04:44): the pipeline position of the planes the variant is computed on.
    // 0 = the SOURCE planes (as uploaded, before any slot) and every owner's default; StageAfter(slot) = the planes as they
    // LEAVE pipeline slot `slot` (core/PipelineSlots.h Id: a stable identity, so a reorder of the slots keeps the key).
    uint8_t stage = 0;
    // CROP (OD 11-14 04:30, 04:44): a rectangle of the stage's planes in source pixels; cropW == 0 || cropH == 0 = none.
    uint32_t cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    // PARAM0 / PARAM1 (lead 8 2026-11-14 05:2x for L-B5): generic owner-defined thresholds (e.g. P11 busy share lap_min / ext_min
    // in 1/16 tap code). The owner documents their meaning in its catalog row / interface.
    uint16_t param0 = 0, param1 = 0;

    enum : uint32_t {
        kAxisPlane = 1u << 0,
        kAxisSize = 1u << 1,
        kAxisScale = 1u << 2,
        kAxisCodeUnit = 1u << 3,
        kAxisSigma = 1u << 4,
        kAxisPrecision = 1u << 5,
        kAxisStage = 1u << 6,
        kAxisCrop = 1u << 7,
        kAxisParam0 = 1u << 8,
        kAxisParam1 = 1u << 9
    };

    static uint8_t StageAfter(uint8_t pipelineSlotId) { return uint8_t(pipelineSlotId + 1u); }
    // The pipeline slot whose output `stage` names (false = the source planes).
    static bool StageSlot(uint8_t stage, uint8_t* slotId)
    {
        if (stage == 0)
            return false;
        if (slotId != nullptr)
            *slotId = uint8_t(stage - 1u);
        return true;
    }
    bool HasCrop() const { return cropW != 0 && cropH != 0; }

    bool IsDefault() const { return Axes() == 0u; }
    uint32_t Axes() const
    {
        return (plane ? uint32_t(kAxisPlane) : 0u) | (width && height ? uint32_t(kAxisSize) : 0u) | (scale ? uint32_t(kAxisScale) : 0u)
               | (codeUnit ? uint32_t(kAxisCodeUnit) : 0u) | (sigmaMilli ? uint32_t(kAxisSigma) : 0u)
               | (precision ? uint32_t(kAxisPrecision) : 0u) | (stage ? uint32_t(kAxisStage) : 0u)
               | (HasCrop() ? uint32_t(kAxisCrop) : 0u) | (param0 ? uint32_t(kAxisParam0) : 0u) | (param1 ? uint32_t(kAxisParam1) : 0u);
    }
    // `defaults`: the owner's own form (its plane / size / scale ...); a field equal to it means "the default". The owner's
    // full frame for the crop axis is defaults.width x defaults.height when given (a crop covering it is no crop).
    VariantKey Canonical(const VariantKey& defaults = VariantKey()) const
    {
        VariantKey k = *this;
        if (k.width == 0 || k.height == 0 || (k.width == defaults.width && k.height == defaults.height))
            k.width = k.height = 0;
        if (k.plane == defaults.plane) k.plane = 0;
        if (k.scale == defaults.scale) k.scale = 0;
        if (k.codeUnit == defaults.codeUnit) k.codeUnit = 0;
        if (k.sigmaMilli == defaults.sigmaMilli) k.sigmaMilli = 0;
        if (k.precision == defaults.precision) k.precision = 0;
        if (k.stage == defaults.stage) k.stage = 0;
        if (k.param0 == defaults.param0) k.param0 = 0;
        if (k.param1 == defaults.param1) k.param1 = 0;
        const bool fullFrame = defaults.width != 0 && defaults.height != 0 && k.cropX == 0 && k.cropY == 0
                               && k.cropW == defaults.width && k.cropH == defaults.height;
        const bool sameAsDefault = k.cropX == defaults.cropX && k.cropY == defaults.cropY && k.cropW == defaults.cropW
                                   && k.cropH == defaults.cropH;
        if (!k.HasCrop() || fullFrame || sameAsDefault)
            k.cropX = k.cropY = k.cropW = k.cropH = 0;
        return k;
    }
    // FNV-1a 64 over every field (call it on a canonical key): the stable id of a variant for counters, logs and the
    // compute-once detector (iface-L-DET VPV_COMPUTE_ONCE variant). 0 for the default variant.
    uint64_t Hash() const
    {
        if (IsDefault())
            return 0;
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v, int bytes) {
            for (int i = 0; i < bytes; ++i) {
                h ^= (v >> (8 * i)) & 0xFFu;
                h *= 1099511628211ull;
            }
        };
        mix(plane, 1); mix(width, 4); mix(height, 4); mix(scale, 1); mix(codeUnit, 1); mix(sigmaMilli, 4); mix(precision, 1);
        mix(stage, 1); mix(cropX, 4); mix(cropY, 4); mix(cropW, 4); mix(cropH, 4); mix(param0, 2); mix(param1, 2);
        return h == 0 ? 1u : h;
    }
    std::string Text() const
    {
        if (IsDefault())
            return "default";
        std::string t;
        auto add = [&t](const char* k, uint64_t v) { t += (t.empty() ? "" : ",") + std::string(k) + "=" + std::to_string(v); };
        if (plane) add("plane", plane);
        if (width && height) { add("w", width); add("h", height); }
        if (scale) add("scale", scale);
        if (codeUnit) add("code", codeUnit);
        if (sigmaMilli) add("sigma_milli", sigmaMilli);
        if (precision) t += (t.empty() ? "" : ",") + std::string("precision=") + PrecisionName(precision);
        if (stage) add("stage", stage);
        if (HasCrop()) { add("crop_x", cropX); add("crop_y", cropY); add("crop_w", cropW); add("crop_h", cropH); }
        if (param0) add("p0", param0);
        if (param1) add("p1", param1);
        return t;
    }
};

inline bool operator==(const VariantKey& a, const VariantKey& b)
{
    return a.plane == b.plane && a.width == b.width && a.height == b.height && a.scale == b.scale && a.codeUnit == b.codeUnit
           && a.sigmaMilli == b.sigmaMilli && a.precision == b.precision && a.stage == b.stage && a.cropX == b.cropX
           && a.cropY == b.cropY && a.cropW == b.cropW && a.cropH == b.cropH && a.param0 == b.param0 && a.param1 == b.param1;
}
inline bool operator!=(const VariantKey& a, const VariantKey& b) { return !(a == b); }
inline bool operator<(const VariantKey& a, const VariantKey& b)
{
    return std::make_tuple(a.plane, a.width, a.height, a.scale, a.codeUnit, a.sigmaMilli, a.precision, a.stage, a.cropX, a.cropY,
                           a.cropW, a.cropH, a.param0, a.param1)
           < std::make_tuple(b.plane, b.width, b.height, b.scale, b.codeUnit, b.sigmaMilli, b.precision, b.stage, b.cropX, b.cropY,
                             b.cropW, b.cropH, b.param0, b.param1);
}

}  // namespace VpSkeleton
