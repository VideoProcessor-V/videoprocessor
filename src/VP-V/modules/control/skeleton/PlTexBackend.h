// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/GpuAllocator.h>

#include <libplacebo/gpu.h>

// Skeleton SA1: the libplacebo texture backend of the GPU allocator core (VP-V/core/GpuAllocator.h,
// ScratchArena.h). Header-only on purpose: no renderer or project entry is needed. A TexFormat maps to a
// pl_fmt through pl_find_fmt with FIXED caps (sampleable | storable, linear not required) and the exact
// component depth, so the same TexFormat is the same texture format on every card. Textures are created
// only when the arena / product textures reserve (never per frame) and destroyed through the same gpu.
namespace VpSkeleton {

struct PlTexBackend {
    using Handle = pl_tex;

    explicit PlTexBackend(pl_gpu g = nullptr) : gpu(g) {}

    // The format of `f` on this gpu, or null when the card has none that is sampleable and storable.
    static pl_fmt FindFormat(pl_gpu gpu, TexFormat f)
    {
        if (!gpu)
            return nullptr;
        const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
        switch (f) {
        case TexFormat::R8: return pl_find_fmt(gpu, PL_FMT_UNORM, 1, 8, 8, caps);
        case TexFormat::R16: return pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, caps);
        case TexFormat::RG16: return pl_find_fmt(gpu, PL_FMT_UNORM, 2, 16, 16, caps);
        case TexFormat::R16F: return pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 16, 16, caps);
        case TexFormat::RG16F: return pl_find_fmt(gpu, PL_FMT_FLOAT, 2, 16, 16, caps);
        case TexFormat::RGBA16F: return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16, caps);
        case TexFormat::R32F: return pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, caps);
        case TexFormat::RG32F: return pl_find_fmt(gpu, PL_FMT_FLOAT, 2, 32, 32, caps);
        case TexFormat::RGBA32F: return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, caps);
        }
        return nullptr;
    }

    // The TexFormat of an existing float/unorm pl_fmt (the exact component count and depth), false when
    // it is not one of the allocator's formats. Lets a module that chose its format with its own
    // lookup request the same format from the arena.
    static bool FormatOf(pl_fmt fmt, TexFormat* out)
    {
        if (!fmt || !out || fmt->num_components < 1 || fmt->num_components > 4)
            return false;
        const int n = fmt->num_components;
        const int d = fmt->component_depth[0];
        if (fmt->type == PL_FMT_FLOAT) {
            if (d == 16 && n == 1) *out = TexFormat::R16F;
            else if (d == 16 && n == 2) *out = TexFormat::RG16F;
            else if (d == 16 && n == 4) *out = TexFormat::RGBA16F;
            else if (d == 32 && n == 1) *out = TexFormat::R32F;
            else if (d == 32 && n == 2) *out = TexFormat::RG32F;
            else if (d == 32 && n == 4) *out = TexFormat::RGBA32F;
            else return false;
            return true;
        }
        if (fmt->type == PL_FMT_UNORM) {
            if (d == 8 && n == 1) *out = TexFormat::R8;
            else if (d == 16 && n == 1) *out = TexFormat::R16;
            else if (d == 16 && n == 2) *out = TexFormat::RG16;
            else return false;
            return true;
        }
        return false;
    }

    Handle Create(const TexShape& s)
    {
        const pl_fmt fmt = FindFormat(gpu, s.fmt);
        if (!fmt)
            return nullptr;
        pl_tex_params tp{};
        tp.w = int(s.w);
        tp.h = int(s.h);
        tp.format = fmt;
        tp.sampleable = true;
        tp.storable = true;
        return pl_tex_create(gpu, &tp);
    }

    void Destroy(Handle& h)
    {
        if (h && gpu)
            pl_tex_destroy(gpu, &h);
        h = nullptr;
    }

    pl_gpu gpu;
};

} // namespace VpSkeleton
