#pragma once
/*
 * Copyright(C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// Dolby Vision tunnel: which generic plane stages may touch the raw tunnel planes (feat/dv-l8,
// 2026-10-24; WIP until the GPU check below has run). Header-only, pure.
//
// The DV tunnel's planes are 12-bit OPPONENT-colour codes (IPT / ICtCp-like I, Ct, Cp) that
// libplacebo decodes to LMS and RGB only after its plane stages. A stage built for YCbCr planes
// that runs before that decode works in the wrong domain, and every rounding it adds is
// amplified by the decode: near a saturated primary one Cp code moves a near-zero output channel
// by ~4-5 10-bit codes (gamma 2.2), while a grey hardly moves.
//
// Finding (the synthetic DV validation's "full pipeline differs by up to 3.6 codes", report
// D:\codex\dv-synth-out\report.json, job full_acc_1080p24-p5-r210-low8): the only patch over
// one code is "2020 green 20" (want R 5.18 / B 1.37, got 2 / 5, dE ITP ~0.2), the faithful path
// is exact on it, and the one setting that differs in the job logs is libplacebo's deband
// (deband=auto/on, grain 0). libplacebo's deband pass (renderer.c plane_deband ->
// pl_shader_deband) renders each source plane into an intermediate FBO (fbofmt[4]: rgba16f on
// D3D11). Ct ~0.31 and Cp ~0.69 sit where fp16 steps are 1-2 12-bit codes, and the deband's
// mix(avg, res, 1) is not bit-exact, so a truncating (or rounding) fp16 store moves the chroma by
// up to a step. Modelled with the corpus' P5 matrices: fp16 truncation of the three planes gives
// R 0-0.8 / B 3.8-4.5 against the measured 2 / 5; the same error pattern, and the measured
// shift solves to I +0.36, Ct -0.6, Cp -0.7 codes (all sub-step). Nothing to deband exists in
// a 12-bit tunnel picture, and debanding opponent planes is the wrong domain anyway.
//
// Policy: no libplacebo plane deband on DV tunnel pictures (the renderer drops deband_params for
// those frames and says so once in the log and on the DV status line).
//
// KNOWN GAP (owner 2026-10-24: streamed DV still carries compression banding from its 10-bit base
// layer): there is NO deband after the decode for the tunnel. VP's own debander (deband_vp,
// VpDebander.h, default off) is also a pass on the SOURCE planes before libplacebo's decode
// (every plane, the IPT chroma included), so it does not act on the decoded picture either; when
// switched on it debands the opponent planes, the same wrong domain. A post-decode deband for DV
// (e.g. on the intensity of the decoded, still-PQ picture) is future work, not built here.
// GPU check to run (queued): the full_acc job with this policy - the 2020 green patch within one
// code of the faithful result, and patch_max_err_codes <= 1.
namespace DvTunnelPolicy
{
	// True when libplacebo's plane deband must not run for this picture.
	inline bool SkipPlaneDeband(bool dolbyVisionTunnelPicture) { return dolbyVisionTunnelPicture; }

	inline const char* DebandStatus()
	{
		return "; libplacebo plane deband off on the tunnel (12-bit IPT; no deband after decode yet)";
	}
}
