// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// input.source_analysis/regions: the GLSL twin of MeasureDomain.h (CB slice 18 S2a: a pure move of X5's MEASURE_HEADER
// ad_lin / ad_luma, unchanged text). Luma in 8-bit codes of the encoded picture (a linear PREKERNEL is re-encoded with 1/2.2
// so the code thresholds mean the same in both light domains). That 1/2.2 is a fixed reference domain for the edge / grain
// thresholds (they were tuned in it), not a claim about the source. AD_KW = the source's luma weights
// (VpMeasure::LumaWeightsGlsl); AD_HDR = a PQ / HLG picture decoded to relative linear first (MeasureDomain; CPU mirror
// VpMeasure::MeasureLumaCode). Needs `src` (the sampled picture) and the uniforms linear_in, ad_trc, ad_a, ad_b, ad_g.
// Header-only, C++14.

namespace VpMeasure
{
	inline const char* MeasureLumaGlsl()
	{
		return R"GLSL(#ifdef AD_HDR
float ad_lin(float c)
{
    c = max(c, 0.0);
    if (ad_trc < 1.5) {
        float e = pow(c, 1.0 / 78.84375);
        float n = max(e - 0.8359375, 0.0) / max(18.8515625 - 18.6875 * e, 1e-6);
        return pow(n, 1.0 / 0.1593017578125) * (10000.0 / 203.0);
    }
    float x = (1.0 - ad_b) * c + ad_b;
    float s = (x <= 0.5 ? 4.0 * x * x : exp((x - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
    return ad_a * pow(max(s, 0.0), ad_g);
}
#endif
float ad_luma(ivec2 p)
{
    ivec2 sz = textureSize(src, 0);
    vec3 c = texelFetch(src, clamp(p, ivec2(0), sz - ivec2(1)), 0).rgb;
#ifdef AD_HDR
    c = vec3(ad_lin(c.r), ad_lin(c.g), ad_lin(c.b));
#endif
    float y = dot(c, AD_KW);
    if (linear_in > 0.5)
        y = sign(y) * pow(abs(y), 1.0 / 2.2);
    return y * 255.0;
}
)GLSL";
	}
}
