// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cmath>

namespace VpComposed {

// The ONE shared image-plane warp sampler (owner S-T1 Q1 11-14: the warp is BICUBIC, Catmull-Rom; R14-95). Every reader that resamples an image plane
// at a landing from Plane::Warp (P10g grain level, TemporalDetail) calls this; nobody keeps a private bicubic. `P` needs int w, h and double at(x, y).
// False when any of the 16 taps lies outside the frame (the footprint rule: no clamped taps). CPU reference of the GPU neighbour-warp product.
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
    const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
    if (x0 - 1 < 0 || y0 - 1 < 0 || x0 + 2 >= img.w || y0 + 2 >= img.h)
        return false;
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
