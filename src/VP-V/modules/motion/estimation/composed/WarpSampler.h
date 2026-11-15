// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cmath>

namespace VpComposed {

// The ONE shared image-plane warp sampler (owner S-T1 Q1 11-14: the warp is BICUBIC, Catmull-Rom; R14-95). Every reader that resamples an image plane
// at a landing from Plane::Warp (P10g grain level, TemporalDetail) calls this. `P` needs int w, h and double at(x, y).
// The Catmull-Rom (Keys a = -0.5) weight law is CubicWeights (rule 20): repair/temporal_detail/TemporalDetailModel.h (Detail::MakeTaps; its
// edge-clamped SampleTaps is its own footprint rule) takes its weights here, global_model/PhotometricRefine.cpp samples through SampleBicubic.
// One CPU copy remains, an open item to move: scaling/chroma_upscale/ChromaCfl.h CatmullRomWeights (ECV-BICUBIC-CHROMA, lead18 c5-fix-ecv3).
// GPU shaders keep their own HLSL form of the law.
// False when any of the 16 taps lies outside the frame or the position is not finite (the footprint rule: no clamped taps). CPU reference of the GPU neighbour-warp product.
inline void CubicWeights(double t, double* w)
{
    const double t2 = t * t, t3 = t2 * t;
    w[0] = -0.5 * t3 + t2 - 0.5 * t;
    w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    w[3] = 0.5 * t3 - 0.5 * t2;
}
template <class P>
inline bool SampleBicubic(const P& img, double sx, double sy, double& out)
{
    // Range test BEFORE the cast: a NaN / inf / out-of-int-range coordinate would make the cast undefined. For finite input this is exactly
    // the tap rule floor(sx) - 1 >= 0 and floor(sx) + 2 < w (sx >= 1 and sx < w - 2), and the same in y.
    if (!(sx >= 1.0 && sy >= 1.0 && sx < img.w - 2.0 && sy < img.h - 2.0))
        return false;
    const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
    double wx[4], wy[4];
    CubicWeights(sx - x0, wx);
    CubicWeights(sy - y0, wy);
    double acc = 0.0;
    for (int j = 0; j < 4; ++j) {
        double row = 0.0;
        for (int i = 0; i < 4; ++i)
            row += wx[i] * img.at(x0 - 1 + i, y0 - 1 + j);
        acc += wy[j] * row;
    }
    out = acc;
    return true;
}

}   // namespace VpComposed
