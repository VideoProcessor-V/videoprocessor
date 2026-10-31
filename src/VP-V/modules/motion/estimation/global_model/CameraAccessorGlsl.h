// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19): the shader mirror of CameraAccessor.h FieldVector. Header-only, no Windows or
// libplacebo headers. The GLSL function is meant for a libplacebo custom shader HEADER; the host declares
// the sampler and the uniforms listed below (pl_shader_desc / pl_shader_var) and fills them from
// AccessorConstants (the weight comes from the CPU BlendState, once per source frame).
//
// camera_model : sampler2D (rgba32f), the LabelPass output layout: (modelX, modelY, |residual|, code),
//                code = label + 4*tinyMover + 8*zeroCandidate. The CPU FieldVector computes
//                sqrt(resX^2 + resY^2) in float from the stored residual; the texture stores that same float
//                value, so the CPU accessor and the shader compare the same number against the same limit.
// Uniforms: int camera_enabled, float camera_weight, int camera_prefer, float camera_abs_thr,
//           float camera_rel_thr, ivec2 camera_grid.
//
// Returns the Choice: 0 Measured, 1 Model, 2 Blend, 3 Hold. Branch order = FieldVector (README steps 0-3).
// A record whose per-block arrays live only on the GPU (CameraMotion::labelsOnGpu) is usable here: the
// CPU FieldVector treats its empty arrays as "no model" and returns the caller's value.
#pragma once

#include <cstddef>

#include "CameraAccessor.h"

namespace CameraModel
{
	inline const char* CameraFieldVectorGlsl()
	{
		return R"GLSL(
int camera_field_vector(ivec2 blk, bool measured_trusted, vec2 m, out vec2 o)
{
    o = m;
    // Step 0: today's behaviour.
    if (camera_enabled == 0 || !(camera_weight > 0.0) ||
        blk.x < 0 || blk.y < 0 || blk.x >= camera_grid.x || blk.y >= camera_grid.y)
        return measured_trusted ? 0 : 3;
    vec4 cm = texelFetch(camera_model, blk, 0);
    bool camera = (int(cm.w + 0.5) & 3) == 1;
    float wf = camera_weight;
    if (measured_trusted) {
        // Step 1: the measured vector stays unless the model agrees within noise and is preferred.
        if (camera_prefer != 0 && camera) {
            float res = cm.z;
            float mag = sqrt(cm.x * cm.x + cm.y * cm.y);
            float rel_limit = camera_rel_thr * mag;
            float limit = rel_limit > camera_abs_thr ? rel_limit : camera_abs_thr;
            if (res <= limit) {
                if (wf >= 1.0) {
                    o = cm.xy;
                    return 1;
                }
                o = vec2((1.0 - wf) * m.x + wf * cm.x, (1.0 - wf) * m.y + wf * cm.y);
                return 2;
            }
        }
        return 0;
    }
    // Step 2: an untrusted Camera block takes the model vector.
    if (camera) {
        if (wf >= 1.0) {
            o = cm.xy;
            return 1;
        }
        o = vec2((1.0 - wf) * m.x + wf * cm.x, (1.0 - wf) * m.y + wf * cm.y);
        return 2;
    }
    // Step 3: today's fallback.
    return 3;
}
)GLSL";
	}

	// The uniform values for one consumer and frame.
	struct AccessorConstants
	{
		int enabled = 0;
		float weight = 0;
		int prefer = 0;
		float absThr = 0, relThr = 0;
		int gridW = 0, gridH = 0;
	};

	// enabled = the CPU's ModelUsable (policy enabled, valid record with block data) and a non-empty grid; the
	// weight is the BlendState's. weight 0 or an unusable record make the shader return the caller's value.
	inline AccessorConstants MakeAccessorConstants(const Policy& policy, const BlendState& state, const CameraMotion* record)
	{
		AccessorConstants k;
		if (!ModelUsable(policy, record))
			return k;
		const BlockData& b = *record->blocks;
		if (b.gridW <= 0 || b.gridH <= 0)
			return k;
		k.enabled = 1;
		k.weight = static_cast<float>(state.Weight());
		k.prefer = policy.preferModelWhenAgrees ? 1 : 0;
		k.absThr = b.absThresholdPx;
		k.relThr = b.relThreshold;
		k.gridW = b.gridW;
		k.gridH = b.gridH;
		return k;
	}
}
