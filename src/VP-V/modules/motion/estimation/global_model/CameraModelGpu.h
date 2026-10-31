// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19, phase 2 prep): the GPU label pass. One thread per block computes the model
// displacement, the residual and the label that GlobalModelFit.cpp Labels() computes on the CPU, from the
// record's parameters and thresholds; a second pass resolves the zero-motion clusters (static overlays).
//
// Texture layouts (rgba32f, gridW x gridH, one texel per block):
//   cm_field (in)  : (fwdX, fwdY, trust, unused)           source px, trust 0..1
//   cm_aux   (in)  : (excluded 0/1, pixelResidual or -1, texture or -1, unused)
//   cm_out   (out) : (modelX, modelY, |residual|, code)    code = label + 4*tinyMover + 8*zeroCandidate,
//                                                          an exact small integer float
// Pass two (cm_in -> cm_out): a block with the zeroCandidate bit and >= overlayMin 4-neighbours with the bit
// becomes Overlay (code = 3 | 8, tinyMover 0). Decisions read only the candidate bits (order independent).
//
// LabelBlockMirror / ZeroClusterMirror perform the shader's arithmetic in the same order in float; the WARP
// test compares the shader to them, and the CPU tests compare them to the double precision Labels().
// The CPU accessor (CameraAccessor.h) and the shader accessor compare the same float |residual|, namely
// sqrtf(rx*rx + ry*ry) (the CPU accessor recomputes it from resX/resY; the texture stores it).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/dispatch.h>

#include "CameraMotion.h"
#include "GlobalModelFit.h"

namespace CameraModel
{
	// The "constant buffer" of the label pass.
	struct LabelConstants
	{
		float h[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };   // model, row-major (float copy of the record's Params)
		float dh[3] = { 0, 0, 0 };                    // h0 - 1, h4 - 1, h8 - 1 rounded once from double (accuracy)
		float cx = 0, cy = 0;                         // picture centre, frame px
		float blockSize = 0;
		float absThr = 0;                             // BlockData::absThresholdPx
		float rel = 0;                                // relThreshold
		int confident = 0;                            // record confidence >= 0.5
		float pixMin = 0;                             // Config::pixelResidualMinShare
		float flat = 0;                               // Config::flatTexture
		int haveResidual = 0, haveTexture = 0;
		int gridW = 0, gridH = 0;
		int perspective = 0;                          // the model has a non-affine bottom row
	};

	// Thresholds exactly as the fitter computes them: absThr = absThresholdSigmas * prec,
	// prec = precisionPx > 0 ? precisionPx : 0.25 * blockSize / 16.
	LabelConstants MakeLabelConstants(const CameraMotion& rec, const Config& cfg, const PairInput& in);

	// Pack the pass inputs (rgba32f, gridW*gridH*4 floats each) from a pair input and the record's excluded mask.
	void PackLabelInputs(const PairInput& in, const BlockData& blocks, std::vector<float>& field, std::vector<float>& aux);

	// GLSL compute bodies (libplacebo custom shader, 8x8 groups; one thread per block).
	const char* LabelPassGlsl();      // descriptors: cm_field, cm_aux (sampled), cm_out (storage); uniforms in the .cpp
	const char* ZeroClusterGlsl();    // descriptors: cm_in (sampled), cm_out (storage); uniforms cm_grid, cm_overlay_min

	// CPU float mirrors.
	void LabelBlockMirror(const LabelConstants& k, int bx, int by, float fx, float fy, float trust, float excluded,
		float pres, float tex, float out[4]);
	void ZeroClusterMirror(int gridW, int gridH, int overlayMin, const float* in4, float* out4);
	// Whole grid: per-block mirror then the zero-cluster mirror. field/aux as PackLabelInputs; out4 = gridW*gridH*4.
	void LabelGridMirror(const LabelConstants& k, const float* field, const float* aux, int overlayMin, std::vector<float>& out4);

	inline int CodeLabel(float code) { return static_cast<int>(code + 0.5f) & 3; }
	inline int CodeTiny(float code) { return (static_cast<int>(code + 0.5f) >> 2) & 1; }
	inline int CodeZero(float code) { return (static_cast<int>(code + 0.5f) >> 3) & 1; }

	// libplacebo dispatch of both passes. Self-contained (own pl_dispatch). Not thread-safe.
	class LabelPass
	{
	public:
		LabelPass(pl_log log, pl_gpu gpu, bool hostReadable = false);
		~LabelPass();
		LabelPass(const LabelPass&) = delete;
		LabelPass& operator=(const LabelPass&) = delete;

		// field / aux: rgba32f sampleable textures of the grid size (see the layouts above).
		bool Run(const LabelConstants& k, pl_tex field, pl_tex aux, int overlayMin);
		pl_tex Output() const { return m_tex[1]; }
		const std::string& Error() const { return m_error; }

	private:
		bool Ensure(int w, int h);
		bool Dispatch(const char* description, const char* body, const pl_shader_desc* desc, int numDesc,
			const pl_shader_var* vars, int numVars, int w, int h);

		pl_log m_log;
		pl_gpu m_gpu;
		pl_dispatch m_dispatch = nullptr;
		pl_tex m_tex[2] = { nullptr, nullptr };
		bool m_readable;
		std::string m_error;
	};
}
