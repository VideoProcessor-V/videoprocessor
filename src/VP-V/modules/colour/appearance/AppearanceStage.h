// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// colour.appearance: the decision of the CAM16 appearance pass (CF-1 CAM-3). CPU only: it chooses the reference
// white (Lw_ref), resolves the room (RoomLight), packs the uniform block and turns it into the shader's parameter
// list. The host (CoreSegments.inl) only calls Decide() once per frame and hands the result to the S2 segment; the
// shader is shaders\vp_cam16_appearance.glsl.
//
// Reference white (refs/125 s2): the DV trim target's peak when a trim was applied (the picture is graded for it),
// else, for SDR content, 100 nits; else (HDR10, DV without a trim, LLDV) the configured sdr_target_nits; else 100 nits.
// FIX1 SHOULD 1: keyed by the SOURCE content, not the output transfer. An LLDV picture is treated as HDR10 (no DV reference).
// Reference mode (L11): when the picture says it is a reference-mode picture and appearance_honour_l11 is on, the room
// is replaced by the reference room (dim, no veil) and the screen-white step still runs (said in the status, never silent).
//
// General, not fixed: every number comes from the inputs; nothing here names a display, a rate or a resolution.

#include "Cam16Uniforms.h"
#include "RoomLight.h"
#include <VP-V/modules/colour/display_mapping/DvMath.h>

#include <algorithm>
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
		bool sourceSdr = false;                // FIX1 SHOULD 1: the source picture is SDR (its reference white is 100 nits)
		double trimSaturationGain = 1.0;       // FIX1 SHOULD 2: the trim's largest saturation gain (doc 115 B13 cap)
		double toeKnee = 0.0;                  // FIX1 SHOULD 3: gamma_boost_knee (encoded fraction 0..0.5; 0 = no toe)
		bool signalPq = false;                 // the output transfer is PQ (else a power-law gamma)
		double outputGamma = 2.4;
		bool l11ReferenceMode = false;         // the picture carries the L11 reference-mode flag
		bool honourL11 = true;                 // appearance_honour_l11
		double rgbToXyz[9] = {};               // output primaries, white Y = 1 (row-major); BT.709 / D65 until set
		bool calibrationLutBefore = false;     // FIX1 MUST 2: libplacebo applies a calibration LUT before this pass (target.lut)
		bool transferSupported = true; int transferKind = 0; double transferParam = 0.0;   // S2-H2: ExtendedRange::OutputTransfer

		// CF1-FIN MUST 1: the default matrix comes from DvMath (the one home of the primaries maths), not a literal.
		Inputs() { DvMath::RgbToXyz(DvMath::kBt709Red, DvMath::kBt709Green, DvMath::kBt709Blue, DvMath::kD65, rgbToXyz); }
	};

	struct Decision
	{
		bool active = false;                   // the hook runs this frame
		bool l11Ignored = false;               // CAM-4: the L11 reference-mode flag held the room back this frame
		bool lutConflict = false;              // FIX1 MUST 2: off because a calibration LUT runs before it (listed, never silent)
		bool transferUnsupported = false;      // S2-H2: off for unsupported output transfer (listed, never silent)
		double referenceWhiteNits = 100.0;
		double huntGain = 1.0;                 // FIX1 SHOULD 2: the model's own saturation gain this frame
		double huntScale = 1.0;                // the factor on M that keeps trim x Hunt within the cap (1 without a trim)
		Cam16::Cam16Uniforms uniforms = {};
		RoomLight::Resolved room;
		ParameterList parameters;              // the shader's parameters, in a fixed order
		std::string status;                    // one plain-language line for Ctrl+I / Config
	};

	inline double ReferenceWhite(const Inputs& in)
	{
		if (in.trimApplied && in.trimTargetNits > 0.0)
			return in.trimTargetNits;
		if (in.sourceSdr)
			return 100.0;
		if (in.sdrTargetNits > 0.0)
			return in.sdrTargetNits;
		return 100.0;
	}

	// FIX1 SHOULD 3: the output white (Y = 100) is the matrix's white (row sums of RGB to XYZ, white Y = 1).
	inline void WhiteXyz(const double rgbToXyz[9], double out[3])
	{
		for (int r = 0; r < 3; ++r)
			out[r] = 100.0 * (rgbToXyz[r * 3 + 0] + rgbToXyz[r * 3 + 1] + rgbToXyz[r * 3 + 2]);
	}

	// FIX1 SHOULD 2 (doc 115 B13): the model's own saturation gain H, measured as the xy chroma gain of a representative
	// colour (linear RGB 0.4 / 0.2 / 0.2 of the output primaries, through the reference side then the screen side).
	inline double HuntChromaGain(const Cam16::UniformInputs& ui, const double rgbToXyz[9])
	{
		const double rgb[3] = { 0.4, 0.2, 0.2 };
		Cam16::Vec3 xyz = { { 0.0, 0.0, 0.0 } };
		for (int r = 0; r < 3; ++r)
			xyz[r] = 100.0 * (rgbToXyz[r * 3 + 0] * rgb[0] + rgbToXyz[r * 3 + 1] * rgb[1] + rgbToXyz[r * 3 + 2] * rgb[2]);
		const Cam16::Appearance a = Cam16::Forward(Cam16::Prepare(Cam16::ReferenceConditions(ui)), xyz);
		const Cam16::Vec3 out = Cam16::Inverse(Cam16::Prepare(Cam16::ScreenConditions(ui)), a.J, a.M, a.h);
		const double ws = ui.whiteXyz[0] + ui.whiteXyz[1] + ui.whiteXyz[2];
		const double si = xyz[0] + xyz[1] + xyz[2], so = out[0] + out[1] + out[2];
		if (!(ws > 0.0) || !(si > 0.0) || !(so > 0.0))
			return 1.0;
		const double wx = ui.whiteXyz[0] / ws, wy = ui.whiteXyz[1] / ws;
		const double di = std::hypot(xyz[0] / si - wx, xyz[1] / si - wy), dout = std::hypot(out[0] / so - wx, out[1] / so - wy);
		return di > 1e-9 && std::isfinite(dout) ? dout / di : 1.0;
	}

	// FIX1 SHOULD 2: the factor on the shader's M that keeps trim gain x Hunt gain within the larger of the two alone
	// (Cam16::CappedHuntGain). 1 without a trim.
	inline double HuntScale(bool trimApplied, double trimSaturationGain, double huntGain)
	{
		if (!trimApplied || !(trimSaturationGain > 0.0) || !(huntGain > 0.0))
			return 1.0;
		return Cam16::CappedHuntGain(trimSaturationGain, huntGain) / huntGain;
	}

	// Output primaries (xy) -> RGB to XYZ with the white at Y = 1 (row-major [9]): DvMath.h (CF1-FIN MUST 1).
	// false: a y <= 0 or singular primaries, the matrix untouched.
	using DvMath::RgbToXyz;

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
	inline ParameterList ShaderParameters(const Cam16::Cam16Uniforms& u, const Inputs& in, double huntScale = 1.0)
	{
		static const char* const ref[10] = { "apr_dc_r", "apr_dc_g", "apr_dc_b", "apr_fl", "apr_nbb", "apr_aw", "apr_cz", "apr_nc", "apr_t", "apr_flq" };
		static const char* const scr[10] = { "aps_dc_r", "aps_dc_g", "aps_dc_b", "aps_fl", "aps_nbb", "aps_aw", "aps_cz", "aps_nc", "aps_t", "aps_flq" };
		static const char* const mat[9] = { "apm_00", "apm_01", "apm_02", "apm_10", "apm_11", "apm_12", "apm_20", "apm_21", "apm_22" };
		static const char* const inv[9] = { "apn_00", "apn_01", "apn_02", "apn_10", "apn_11", "apn_12", "apn_20", "apn_21", "apn_22" };
		ParameterList p;
		AddSide(p, ref, u.reference);
		AddSide(p, scr, u.screen);
		p.push_back({ "apx_strength", u.strength });
		p.push_back({ "apx_veil", u.veil });
		p.push_back({ "apx_screen_nits", u.screenWhiteNits });
		p.push_back({ "apx_ref_nits", u.referenceWhiteNits });
		p.push_back({ "apx_pq", in.signalPq ? 1.0 : 0.0 });
		p.push_back({ "apx_gamma", in.outputGamma });
		p.push_back({ "apx_trc_kind", static_cast<double>(in.transferKind) });
		p.push_back({ "apx_trc_param", in.transferParam });
		p.push_back({ "apx_knee", std::min(0.5, std::max(0.0, in.toeKnee)) });   // FIX1 SHOULD 3: gamma_boost_knee, not a fixed 0
		for (int i = 0; i < 9; ++i)
			p.push_back({ mat[i], in.rgbToXyz[i] });
		// NIT 1: the inverse is published so the shader does not invert per pixel. Singular: the BT.709 inverse.
		double fromXyz[9];
		if (!DvMath::Invert(in.rgbToXyz, fromXyz))
			DvMath::Invert(Inputs().rgbToXyz, fromXyz);
		for (int i = 0; i < 9; ++i)
			p.push_back({ inv[i], fromXyz[i] });
		p.push_back({ "apx_hunt_scale", huntScale });   // FIX1 SHOULD 2
		return p;
	}

	// One decision per frame. Off: nothing (the status line "appearance: off" is the host's). Never silent otherwise.
	// CF1-G7b: ambient_adapt is replaced only while appearance really acts (the rule the saturation step uses per frame,
	// CF1-FIN SHOULD B). PostLoad forced ambient_adapt off for a configured appearance (replaced); a frame where the pass is
	// not active (strength 0 is never replaced; a calibration LUT before it, a lost shader) gives ambient_adapt back.
	inline bool AmbientAdaptGivenBack(bool replaced, bool appearanceActive) { return replaced && !appearanceActive; }
	inline bool AmbientAdaptOn(bool ambientAdapt, bool replaced, bool appearanceActive)
	{
		return ambientAdapt || AmbientAdaptGivenBack(replaced, appearanceActive);
	}
	// Gate G1 MUST-5: the host's LED intent evaluation runs on the SAVED ambient_adapt (or intent_black / intent_display, or
	// a non-zero LED limit already sent). True while ambient_adapt is given back but none of those runs it: the LED shadow
	// cap's ambient term is then not refreshed (FallbackCatalog::AmbientAdaptLedShadowCapSaved lists it).
	inline bool LedShadowCapUsesSavedAmbient(bool givenBack, bool savedAmbientAdapt, bool intentBlackOn, bool intentDisplay,
		double sentShadowMinimumShare, double sentAmbientBlackNits)
	{
		return givenBack && !savedAmbientAdapt && !intentBlackOn && !intentDisplay &&
			sentShadowMinimumShare == 0.0 && sentAmbientBlackNits == 0.0;
	}

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
		if (in.calibrationLutBefore)
		{
			// FIX1 MUST 2: the model would act on projector device codes after the LUT (wrong colour). calibration_lut_stage
			// auto moves the LUT to VP's last step; with libplacebo chosen explicitly the pass stays out, visibly.
			smoother.Reset();
			d.lutConflict = true;
			d.status = "appearance: off with a calibration LUT (it would act after the LUT)";
			return d;
		}
		if (!in.transferSupported)
		{
			smoother.Reset();
			d.transferUnsupported = true;
			d.status = "appearance: off for this output transfer (PQ, gamma, BT.1886 or sRGB outputs only)";
			return d;
		}
		if (in.l11ReferenceMode && in.honourL11)
		{
			// FIX1 MUST 4 (refs/125 s4, plan CAM-3): the picture asks for reference viewing conditions, so the ROOM becomes
			// the reference room (dim surround, no veil; its slow adaptation restarts). The pass stays on: the screen white
			// still differs from the reference white, and that Stevens/Hunt step is kept.
			smoother.Reset();
			d.l11Ignored = true;
			d.room.surround = Cam16::kSurroundDim;
			d.room.veilNits = 0.0;
			d.room.status = "room: L11 reference mode (dim, no veil; room light ignored)";
		}
		else
			d.room = RoomLight::Resolve(in.room, in.record, smoother, in.atCut, in.now);
		Cam16::UniformInputs ui;
		ui.referenceWhiteNits = d.referenceWhiteNits;
		ui.screenWhiteNits = in.screenWhiteNits > 0.0 ? in.screenWhiteNits : d.referenceWhiteNits;
		ui.referenceSurround = Cam16::kSurroundDim;
		ui.screenSurround = d.room.surround;
		ui.veilNits = d.room.veilNits;
		ui.strength = in.strength;
		WhiteXyz(in.rgbToXyz, ui.whiteXyz);   // FIX1 SHOULD 3: the output's own white
		d.uniforms = Cam16::PackUniforms(ui);
		d.huntGain = HuntChromaGain(ui, in.rgbToXyz);
		d.huntScale = HuntScale(in.trimApplied, in.trimSaturationGain, d.huntGain);
		d.parameters = ShaderParameters(d.uniforms, in, d.huntScale);
		d.active = d.uniforms.strength > 0.0f;
		d.status = std::string("appearance on: ") + d.room.status + (in.screenWhiteMeasured ? "" : " (screen white not measured)");
		return d;
	}
}
