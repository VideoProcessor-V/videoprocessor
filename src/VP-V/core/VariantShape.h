// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// L-CORE deliverable 3 (lead8/iface-L-CORE.md s4): the VariantKey axes made LEDGER- and LAYOUT-aware. A memo spot's texture
// shape and bytes follow from its variant and the source dims, never from a hard-coded size (00-UNIVERSAL rule 6):
//   VariantDims        crop (the stage's rectangle) -> explicit size -> scale (one halving per level, rounded up);
//   TexFormatOf        the precision axis -> a TexFormat of the ledger (GpuAllocator.h), per channel count;
//   VariantTexShape    both, as the ledger's TexShape; VariantReserveBytes its bytes (OwnerFill::reserveBytes: reserved against
//                      the VRAM ledger grant BEFORE the fill, the B1 rule).
// Header-only, C++14, no GPU API.

#include "GpuAllocator.h"
#include "SkeletonAcquire.h"

#include <cstdint>

namespace VpSkeleton {

// The pixel dims of variant `k` (canonical or not) of a product whose own size is `full` (the source dims of the stage).
inline Dims VariantDims(const VariantKey& k, Dims full)
{
    Dims d = full;
    if (k.HasCrop()) {
        const uint64_t x1 = uint64_t(k.cropX) + k.cropW;
        const uint64_t y1 = uint64_t(k.cropY) + k.cropH;
        d.width = k.cropX >= full.width ? 0u : uint32_t((x1 > full.width ? full.width : x1) - k.cropX);    // clipped to the frame
        d.height = k.cropY >= full.height ? 0u : uint32_t((y1 > full.height ? full.height : y1) - k.cropY);
    }
    if (k.width != 0 && k.height != 0) {
        d.width = k.width;
        d.height = k.height;
    }
    for (uint8_t i = 0; i < k.scale && d.width > 0 && d.height > 0; ++i) {
        d.width = (d.width + 1u) / 2u;
        d.height = (d.height + 1u) / 2u;
    }
    return d;
}

// The ledger format of `precision` for `channels` (1, 2 or 4; 3 is stored as 4). `fallback` is the owner's default format
// (precision 0). False = no such format in the ledger (e.g. a 4-channel unorm16): the owner declines that variant visibly.
inline bool TexFormatOf(uint8_t precision, unsigned channels, TexFormat fallback, TexFormat* out)
{
    const unsigned c = channels == 3 ? 4u : channels;
    TexFormat f = fallback;
    switch (precision) {
    case kPrecisionDefault: break;
    case kPrecisionUnorm16:
        if (c == 1) f = TexFormat::R16;
        else if (c == 2) f = TexFormat::RG16;
        else return false;
        break;
    case kPrecisionFloat32:
        f = c == 1 ? TexFormat::R32F : c == 2 ? TexFormat::RG32F : TexFormat::RGBA32F;
        break;
    case kPrecisionFloat16:
        f = c == 1 ? TexFormat::R16F : c == 2 ? TexFormat::RG16F : TexFormat::RGBA16F;
        break;
    default: return false;
    }
    if (out != nullptr)
        *out = f;
    return true;
}

inline bool VariantTexShape(const VariantKey& k, Dims full, unsigned channels, TexFormat fallback, TexShape* out)
{
    TexFormat f = fallback;
    if (!TexFormatOf(k.precision, channels, fallback, &f))
        return false;
    const Dims d = VariantDims(k, full);
    if (d.width == 0 || d.height == 0)
        return false;
    if (out != nullptr) {
        out->w = d.width;
        out->h = d.height;
        out->fmt = f;
    }
    return true;
}

// Bytes of the variant's texture (0 = no valid shape: the owner declines).
inline uint64_t VariantReserveBytes(const VariantKey& k, Dims full, unsigned channels, TexFormat fallback)
{
    TexShape s;
    return VariantTexShape(k, full, channels, fallback, &s) ? ShapeBytes(s) : 0u;
}

}  // namespace VpSkeleton
