// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// colour.appearance: the decision of the CAM16 appearance pass (CF-1 CAM-3). CPU only: it chooses the reference
// white (Lw_ref), resolves the room (RoomLight), packs the uniform block and turns it into the shader's parameter
// list. The host (CoreSegments.inl) only calls Decide() once per frame and hands the result to the S2 segment; the
// shader is shaders\vp_cam16_appearance.glsl.
//
// Reference white (refs/125 s2): the DV trim target's peak when a trim was applied (the picture is graded for it),
// else the configured SDR target nits, else 100 nits (SDR). An LLDV picture is treated as HDR10 (no DV reference).
// Reference mode (L11): when the picture says it is a reference-mode picture and appearance_honour_l11 is on, the room
// is ignored and the pass stays out (said in the status, never silent).
//
// General, not fixed: every number comes from the inputs; nothing here names a display, a rate or a resolution.

#include "Cam16Uniforms.h"
#include "RoomLight.h"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace AppearanceStage
{
	using ParameterList = std::vector<std::pair<const char*, double>>;

	struct Inputs
	{
		bool on = false;                       // appearance (the VP-V switch)
		double strength = 1.0;                 // appearance_strength
		RoomLight::Params room;                // appearance_room + the ambient anchors / reflect / cap
		RoomLight::Record record;              // the room_light product (latest sealed)
		bool recordAvailable = false;          // something was published
		bool atCut = false;
		double now = 0.0;                      // monotonic seconds
		double screenWhiteNits = 48.0;         // appearance_screen_white_nits (measured or the placeholder)
		bool screenWhiteMeasured = false;      // appearance_screen_white_measured
		bool trimApplied = false;              // the DV trim ran this frame
		double trimTargetNits = 0.0;           // the trim's display peak
		double sdrTargetNits = 0.0;            // the configured SDR target nits (0 = unknown)
		bool signalPq = false;                 // the output transfer is PQ (else a power-law gamma)
		double outputGamma = 2.4;
		bool l11ReferenceMode = false;         // the picture carries the L11 reference-mode flag
		bool honourL11 = true;                 // appearance_honour_l11
		double rgbToXyz[9] = { 0.4124, 0.3576, 0.1805, 0.2126, 0.7152, 0.0722, 0.0193, 0.1192, 0.9505 };   // output primaries, white Y = 1
	};

	struct Decision
	{
		bool active = false;                   // the hook runs this frame
		bool l11Ignored = false;               // CAM-4: the L11 reference-mode flag held the room back this frame
		double referenceWhiteNits = 100.0;
		Cam16::Cam16Uniforms uniforms = {};
		RoomLight::Resolved room;
		ParameterList parameters;              // the shader's parameters, in a fixed order
		std::string status;                    // one plain-language line for Ctrl+I / Config
	};

	inline double ReferenceWhite(const Inputs& in)
	{
		if (in.trimApplied && in.trimTargetNits > 0.0)
			return in.trimTargetNits;
		if (in.sdrTargetNits > 0.0)
			return in.sdrTargetNits;
		return 100.0;
	}

	// Output primaries (xy) -> RGB to XYZ with the white at Y = 1 (row-major), for the shader's matrix.
	inline bool RgbToXyz(const double red[2], const double green[2], const double blue[2], const double white[2], double out[9])
	{
		if (!(red[1] > 0.0) || !(green[1] > 0.0) || !(blue[1] > 0.0) || !(white[1] > 0.0))
			return false;
		const double xr = red[0] / red[1], zr = (1.0 - red[0] - red[1]) / red[1];
		const double xg = green[0] / green[1], zg = (1.0 - green[0] - green[1]) / green[1];
		const double xb = blue[0] / blue[1], zb = (1.0 - blue[0] - blue[1]) / blue[1];
		const double xw = white[0] / white[1], zw = (1.0 - white[0] - white[1]) / white[1];
		const double m[3][3] = { { xr, xg, xb }, { 1, 1, 1 }, { zr, zg, zb } };
		const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		if (!(std::fabs(det) > 1e-12))
			return false;
		const double inv[3][3] = {
			{ (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det },
			{ (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det },
			{ (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det } };
		const double s[3] = { inv[0][0] * xw + inv[0][1] * 1.0 + inv[0][2] * zw, inv[1][0] * xw + inv[1][1] * 1.0 + inv[1][2] * zw,
			inv[2][0] * xw + inv[2][1] * 1.0 + inv[2][2] * zw };
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				out[r * 3 + c] = m[r][c] * s[c];
		return true;
	}

	inline void AddSide(ParameterList& p, const char* const names[10], const Cam16::SideUniforms& s)
	{
		p.push_back({ names[0], s.dc[0] });
		p.push_back({ names[1], s.dc[1] });
		p.push_back({ names[2], s.dc[2] });
		p.push_back({ names[3], s.fl });
		p.push_back({ names[4], s.nbb });
		p.push_back({ names[5], s.aw });
		p.push_back({ names[6], s.cz });
		p.push_back({ names[7], s.nc });
		p.push_back({ names[8], s.tFactor });
		p.push_back({ names[9], s.flQuarter });
	}

	// The shader's parameters from a packed block (names match shaders\vp_cam16_appearance.glsl).
	inline ParameterList ShaderParameters(const Cam16::Cam16Uniforms& u, const Inputs& in)
	{
		static const char* const ref[10] = { "apr_dc_r", "apr_dc_g", "apr_dc_b", "apr_fl", "apr_nbb", "apr_aw", "apr_cz", "apr_nc", "apr_t", "apr_flq" };
		static const char* const scr[10] = { "aps_dc_r", "aps_dc_g", "aps_dc_b", "aps_fl", "aps_nbb", "aps_aw", "aps_cz", "aps_nc", "aps_t", "aps_flq" };
		static const char* const mat[9] = { "apm_00", "apm_01", "apm_02", "apm_10", "apm_11", "apm_12", "apm_20", "apm_21", "apm_22" };
		ParameterList p;
		AddSide(p, ref, u.reference);
		AddSide(p, scr, u.screen);
		p.push_back({ "apx_strength", u.strength });
		p.push_back({ "apx_veil", u.veil });
		p.push_back({ "apx_screen_nits", u.screenWhiteNits });
		p.push_back({ "apx_ref_nits", u.referenceWhiteNits });
		p.push_back({ "apx_pq", in.signalPq ? 1.0 : 0.0 });
		p.push_back({ "apx_gamma", in.outputGamma });
		p.push_back({ "apx_knee", 0.0 });
		for (int i = 0; i < 9; ++i)
			p.push_back({ mat[i], in.rgbToXyz[i] });
		return p;
	}

	// One decision per frame. Off: nothing (the status line "appearance: off" is the host's). Never silent otherwise.
	inline Decision Decide(const Inputs& in, RoomLight::Smoother& smoother)
	{
		Decision d;
		if (!in.on)
		{
			smoother.Reset();
			d.status = "appearance: off";
			return d;
		}
		d.referenceWhiteNits = ReferenceWhite(in);
		if (in.l11ReferenceMode && in.honourL11)
		{
			// The picture asks for reference viewing conditions: the room is not applied (and its slow adaptation restarts).
			smoother.Reset();
			d.l11Ignored = true;
			d.status = "appearance on: L11 reference mode: room light ignored";
			return d;
		}
		d.room = RoomLight::Resolve(in.room, in.record, smoother, in.atCut, in.now);
		Cam16::UniformInputs ui;
		ui.referenceWhiteNits = d.referenceWhiteNits;
		ui.screenWhiteNits = in.screenWhiteNits > 0.0 ? in.screenWhiteNits : d.referenceWhiteNits;
		ui.referenceSurround = Cam16::kSurroundDim;
		ui.screenSurround = d.room.surround;
		ui.veilNits = d.room.veilNits;
		ui.strength = in.strength;
		d.uniforms = Cam16::PackUniforms(ui);
		d.parameters = ShaderParameters(d.uniforms, in);
		d.active = d.uniforms.strength > 0.0f;
		d.status = std::string("appearance on: ") + d.room.status + (in.screenWhiteMeasured ? "" : " (screen white not measured)");
		return d;
	}
}
