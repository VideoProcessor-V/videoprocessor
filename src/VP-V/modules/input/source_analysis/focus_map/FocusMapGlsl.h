// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/focus_map: the GLSL mirror of FocusMap::DefocusBlock<float> / ChangeBlock<float> (one thread per P11 block).
// Reads the P11 terms S2 (E1, E2, E3, offLap) and S4 (coverage, offShare, visibility, N) of this frame and of the previous frame, plus
// one rgba32f texel per block holding the block's motion (dx, dy source px to the previous frame, source 0 none / 1 field / 2 camera).
// Writes one rgba32f texel per block: (defocus, delta, confidence, flags). Flag values are FocusMap::Flag.
// Bindings (sampled, nearest): fm_s2, fm_s4, fm_p2, fm_p4, fm_motion; storage: fm_out.
// Variables: fm_grid ivec2, fm_block int, fm_has_prev int, fm_no_split int, fm_a vec4 (ratioLo, ratioHi, eps, minCoverage),
// fm_b vec4 (energyLo, energyHi, changeThreshold, minConfidence).
#pragma once

namespace FocusMap
{
	inline const char* FocusMapPassHeaderGlsl()
	{
		return R"GLSL(
float fm_ramp(float x, float lo, float hi) {
    float t = (x - lo) / (hi - lo);
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}
// out: x defocus, y confidence, z flags (as float)
vec3 fm_defocus_of(vec4 s2, vec4 s4) {
    float defocus = 0.0, confidence = 0.0;
    int flags = 0;
    if (!(s4.x >= fm_a.w)) return vec3(0.0, 0.0, 0.0);
    float n = fm_no_split != 0 ? -1.0 : s4.w;
    bool haveSplit = n >= 0.0;
    if (!haveSplit) flags |= 2;
    float e1 = s2.x > 0.0 ? s2.x : 0.0;
    float e2 = s2.y > 0.0 ? s2.y : 0.0;
    float e3 = s2.z > 0.0 ? s2.z : 0.0;
    float fine = haveSplit ? e1 - n : e1;
    if (fine < 0.0) fine = 0.0;
    float total = fine + e2 + e3;
    float ratio = fine / (total + fm_a.z);
    defocus = 1.0 - fm_ramp(ratio, fm_a.x, fm_a.y);
    confidence = fm_ramp(total, fm_b.x, fm_b.y);
    flags |= 1;
    return vec3(defocus, confidence, float(flags));
}
)GLSL";
	}

	inline const char* FocusMapPassGlsl()
	{
		return R"GLSL(
ivec2 blk = ivec2(gl_GlobalInvocationID.xy);
if (blk.x < fm_grid.x && blk.y < fm_grid.y) {
    vec3 cur = fm_defocus_of(texelFetch(fm_s2, blk, 0), texelFetch(fm_s4, blk, 0));
    int flags = int(cur.z);
    float delta = 0.0;
    vec4 mo = texelFetch(fm_motion, blk, 0);
    int src = int(mo.z + 0.5);
    if (src == 0) flags |= 4; else if (src == 2) flags |= 8;
    if ((flags & 1) != 0) {
        float px = (float(blk.x) + 0.5) * float(fm_block) + mo.x;
        float py = (float(blk.y) + 0.5) * float(fm_block) + mo.y;
        ivec2 sb = ivec2(int(floor(px / float(fm_block))), int(floor(py / float(fm_block))));
        bool pok = fm_has_prev != 0 && sb.x >= 0 && sb.y >= 0 && sb.x < fm_grid.x && sb.y < fm_grid.y;
        vec3 prev = vec3(0.0);
        if (pok) {
            prev = fm_defocus_of(texelFetch(fm_p2, sb, 0), texelFetch(fm_p4, sb, 0));
            pok = (int(prev.z) & 1) != 0;
        }
        if (!pok) {
            flags |= 16;
        } else {
            delta = cur.x - prev.x;
            float c = cur.y < prev.y ? cur.y : prev.y;
            if (c < fm_b.w) {
                flags |= 128;
            } else {
                float a = delta < 0.0 ? -delta : delta;
                if (a >= fm_b.z) {
                    flags |= 32;
                    if (delta < 0.0) flags |= 64;
                }
            }
        }
    }
    imageStore(fm_out, blk, vec4(cur.x, delta, cur.y, float(flags)));
}
)GLSL";
	}
}
