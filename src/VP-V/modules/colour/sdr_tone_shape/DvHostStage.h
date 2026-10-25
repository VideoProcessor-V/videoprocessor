#pragma once
/*
 * Copyright(C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// WP-14 (integration batch 1, M2 steps 6 and 14): the renderer host's Dolby Vision glue, moved out
// of LibplaceboVideoRenderer.cpp (Bill's host) into one VP-V adapter. The host keeps one call per
// hook point (docs/design/m2-dv-callsites.md); the logic, its per-renderer state and its log lines
// live here, unchanged (a behaviour-free move: commit 1 of the two-commit rule).
//
// Hook points, all on the render thread (RenderLocked):
//   OnTunnelState   - the DV metadata hold / corrupt-packet fallback entries, per picture
//                     (DvMetadataHold.h), and the tunnel's fallbacks released outside a tunnel;
//   IngressSuffix   - the "; DV trims ...; DV tone mapping ...; DV metadata ...; deband ..." tail of
//                     the Ctrl+I ingress line for a DV tunnel picture;
//   SkipVpDeband    - VP's debander skipped on the raw tunnel planes (DvTunnelPolicy.h), visibly;
//   OnPreOutput     - the Dolby-style curve hook (dv_tone_mapping; DvDolbyStyle.h), the L8 / L2
//                     trims at PRE_OUTPUT (dv_trims; DvTrimSelection.h, shaders/DV trims.glsl), the
//                     reference conditions of the last DV picture (for the CAM16 stage, Stage 13),
//                     and the DV comparison capture (DvCompareCapture.h);
//   SkipPlaneDeband - libplacebo's plane deband dropped on the raw tunnel planes, logged once;
//   ReleaseHooks    - the trims hook released with the device;
//   ToggleCompareCapture - the dv_compare_capture shortcut (plugin API).
// Fallbacks go through the host's registry (the two callbacks), so every entry stays under
// "Fallbacks engaged" exactly as before (ADR-0001, vp-no-silent-fallbacks).
//
// libplacebo's headers come first: DvDolbyStyle.h only defines its libplacebo part (the curve,
// CannotRun, ShaderParameters, ToneSwitch) when they are already included.
#pragma warning(push)
#pragma warning(disable: 4244) // conversion warning in an upstream inline helper (as in the renderer)
#include <libplacebo/colorspace.h>
#include <libplacebo/renderer.h>
#include <libplacebo/tone_mapping.h>
#include <libplacebo/shaders/colorspace.h>
#pragma warning(pop)

#include <DebugLog.h>
#include <VideoFrame.h>
#include <VideoState.h>
#include <microsoft_directshow/MadVRShaderLoader.h>
#include <vprenderer/CustomHookChain.h>
#include <VP-V/modules/colour/sdr_tone_shape/DolbyVisionTrims.h>
#include <VP-V/modules/colour/sdr_tone_shape/DvDolbyStyle.h>
#include <VP-V/modules/colour/sdr_tone_shape/DvTrimSelection.h>
#include <VP-V/modules/input/signal/DvCompareCapture.h>
#include <VP-V/modules/input/signal/DvMetadataHold.h>
#include <VP-V/modules/input/signal/DvTunnelPolicy.h>

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class DvHostStage
{
public:
	using Engage = std::function<void(const char* id, const std::string& reason)>;
	using Release = std::function<void(const char* id)>;

	// One render frame's inputs at the PRE_OUTPUT hook point.
	struct Frame
	{
		const std::string& toneMappingMode;   // dv_tone_mapping: libplacebo | dolby_style
		bool trims;                           // dv_trims
		const std::string& libplaceboCurve;   // tone_mapping (the curve libplacebo runs otherwise)
		const std::string& compareFolder;     // dv_compare_capture_folder ("" = VP_DV_COMPARE_PATH / off)
		bool colourStageRuns;                 // the colour.sdr_tone_shape slot runs this frame
		const pl_frame& image;                // the source picture as rendered
		const pl_frame& target;               // the output target (its LUT and transfer)
		const DolbyVisionTunnelFrameMetadata* dm;   // the picture's DM, or null (no converted payload)
		double outputGamma;                   // the output transfer's gamma (host: LedDisplayGamma)
		const VideoState& state;              // for the comparison capture
		const VideoFrame* rawFrame;           // the raw capture frame, or null
		uint64_t counter;                     // the frame counter
	};

	DvHostStage(CustomHookChain::Loader loader, Engage engage, Release release)
		: m_trimsHook(std::move(loader)), m_engage(std::move(engage)), m_release(std::move(release))
	{
	}

	// Per picture, once the tunnel state is known: test/dv-synthetic 2026-10-24 (owner rule: no
	// silent fallbacks): the tunnel's automatic degradations are listed under "Fallbacks engaged"
	// while they last; none outside a tunnel. DV open item (2026-10-24): a rejected DM silently kept
	// the previous one; now visible. feat/dv-l8: held briefly, then static metadata
	// (DvMetadataHold.h), counted on the DV line.
	void OnTunnelState(bool tunnelActive, const DolbyVisionTunnelFrameMetadata* dm)
	{
		std::string dvMetadataReason;
		if (tunnelActive && dm && DvMetadataHold::FallbackReason(*dm, dvMetadataReason))
			m_engage("DV metadata", dvMetadataReason);
		else
			m_release("DV metadata");
		if (!tunnelActive)
		{
			m_release("Dolby Vision metadata");
			m_release("Dolby Vision source range");
			m_release("Dolby Vision scene metadata");
			m_release("Dolby Vision trims");
		}
	}

	// The tail of the ingress status line for a DV tunnel picture.
	std::string IngressSuffix(const DolbyVisionTunnelFrameMetadata& dm) const
	{
		return "; " + m_trimsStatus + "; " + m_toneStatus + DvMetadataHold::StatusSuffix(dm) +
			DvTunnelPolicy::DebandStatus(m_debandSkipped, m_vpDebandSkipped);
	}

	// feat/dv-l8 (owner 2026-10-24, DvTunnelPolicy.h): VP's debander works on the source planes, which
	// for the DV tunnel are the 12-bit opponent planes before libplacebo's decode - skipped there,
	// visibly. Sets debandState when skipped; returns whether it is. vpDebandOn: deband_vp is on AND
	// the repair.deband slot runs (host: pipelineSlots.Runs(RepairDeband)) - with the slot off there
	// is nothing to skip and nothing is listed.
	bool SkipVpDeband(bool dolbyVisionPicture, bool vpDebandOn, std::string& debandState)
	{
		m_vpDebandSkipped = DvTunnelPolicy::SkipVpDeband(dolbyVisionPicture, vpDebandOn);
		if (m_vpDebandSkipped)
		{
			debandState = "VP deband: off on Dolby Vision tunnel pictures (12-bit opponent planes before the decode)";
			m_engage("VP deband", "off on the DV tunnel planes (no deband after decode yet)");
		}
		else
			m_release("VP deband");
		return m_vpDebandSkipped;
	}

	// feat/dv-l8 WIP (DvTunnelPolicy.h): no libplacebo plane deband on the raw DV tunnel planes
	// (opponent codes before the decode; its fp16 plane pass moved saturated near-zero channels by
	// up to 3.6 codes). Returns true when the host must drop this frame's deband parameters.
	// tunnelFixes (dv_tunnel_fixes; integ/batch1 dc149284): off - and so on the Original pathway -
	// Bill's deband is kept.
	bool SkipPlaneDeband(bool tunnelFixes, bool dolbyVisionPicture, bool debandRequested)
	{
		m_debandSkipped = tunnelFixes && DvTunnelPolicy::SkipPlaneDeband(dolbyVisionPicture) && debandRequested;
		if (m_debandSkipped)
		{
			if (!m_debandSkipLogged)
				DebugLog::Log("libplacebo deband: off on Dolby Vision tunnel pictures (12-bit opponent planes before the decode; DvTunnelPolicy.h)");
			m_debandSkipLogged = true;
		}
		return m_debandSkipped;
	}

	// The trims hook goes with the device.
	void ReleaseHooks()
	{
		m_trimsHook.Clear();
		m_trimsConfigured = false;
	}

	// feat/dv-l8 WIP (plugin API 43): the dv_compare_capture shortcut (DvCompareCapture.h).
	// TODO(WP-11, audit M-2): once feat/wp11-status is on integ/batch1, the renderer answers
	// InvokeModuleCommand("vp", "dv_compare_capture") with this, and the dedicated
	// IRenderer::ToggleDvCompareCapture virtual (Bill's interface) is removed.
	std::string ToggleCompareCapture() { return m_compare.Toggle(); }

	// The reference viewing conditions of the last DV picture (for the later CAM16 stage).
	const DvDolbyStyle::ReferenceConditions& ReferenceConditions() const { return m_referenceConditions; }
	const std::string& TrimsStatus() const { return m_trimsStatus; }
	const std::string& ToneStatus() const { return m_toneStatus; }

	// PRE_OUTPUT, after the SDR tone shape and before the LED stages. Appends only the DV trims hook(s)
	// to frameHooks; the host tags them to colour.sdr_tone_shape (TagSlotHooks) so they move with
	// the slot when it is reordered.
	void OnPreOutput(const Frame& f, pl_color_map_params& colorMapParams,
		const CustomHookChain::TemplateValues& hookTemplates, std::vector<const pl_hook*>& frameHooks)
	{
		// dv_trims (owner 2026-10-17; default on since the DV synthetic validation 2026-10-24): the Dolby Vision L2 trim for the
		// display's peak, after libplacebo's tone mapping (PRE_OUTPUT, before
		// the LED stages). Tunnel pictures with DM only; not after a
		// calibration LUT; PQ or gamma outputs. Non-certified (DolbyVisionTrims.h).
		// feat/dv-l8 task 5 (DvDolbyStyle.h): dv_tone_mapping dolby_style - the Dolby-style curve for DV pictures and
		// the trims in ICtCp; when it cannot run: bt2390 + the luminance trims, under "Fallbacks engaged".
		const pl_frame& renderImage = f.image;
		const pl_frame& target = f.target;
		// F-12 (owner rule, 2026-10-25): the slot switched off (pipeline_slots_off / Original pathway)
		// is OFF, not a fallback: the configured libplacebo curve runs and nothing is listed, exactly
		// as without the slot. (Before: "DV tone mapping: the colour tone-shape slot is off: bt2390".)
		const bool dvToneDolby = f.colourStageRuns &&
			DvDolbyStyle::ParseMode(f.toneMappingMode) == DvDolbyStyle::Mode::DolbyStyle;
		const bool dvTonePicture = renderImage.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION && f.dm && f.dm->valid;
		const std::string dvToneFallback = DvDolbyStyle::CannotRun(dvToneDolby && dvTonePicture, target.lut != nullptr,
			target.color.transfer, f.colourStageRuns, f.trims && m_trimsShaderMissing);
		m_toneDolbyRunning = dvToneDolby && dvTonePicture && dvToneFallback.empty();
		m_toneSwitch.Apply(colorMapParams, m_toneDolbyRunning ? &DvDolbyStyle::Function() : !dvToneFallback.empty() ? &pl_tone_map_bt2390 : nullptr);
		if (!dvToneFallback.empty()) m_engage("DV tone mapping", dvToneFallback + ": " + DvDolbyStyle::FallbackName(f.trims)); else m_release("DV tone mapping");
		// Audit 7: libplacebo's input_avg / input_max for the curve come from L1 via hdr.avg_pq_y / max_pq_y.
		const bool dvToneL1Missing = m_toneDolbyRunning && !(renderImage.color.hdr.max_pq_y > 0.0f && renderImage.color.hdr.avg_pq_y > 0.0f);
		if (m_toneDolbyRunning && static_cast<int>(dvToneL1Missing) != m_toneL1Logged)
		{
			m_toneL1Logged = dvToneL1Missing ? 1 : 0;
			DebugLog::Log("DV tone mapping (Dolby-style): libplacebo %s the scene values (hdr.max_pq_y=%.4f avg_pq_y=%.4f)%s", dvToneL1Missing ? "does NOT receive" : "receives",
				renderImage.color.hdr.max_pq_y, renderImage.color.hdr.avg_pq_y, dvToneL1Missing ? "; L1 missing -> mid-range fallback" : "");
		}
		m_toneStatus = DvDolbyStyle::StatusText(dvToneDolby ? DvDolbyStyle::Mode::DolbyStyle : DvDolbyStyle::Mode::Libplacebo,
			m_toneDolbyRunning, f.libplaceboCurve, dvToneFallback, f.trims, dvToneL1Missing);
		// The trim choice the reference conditions report: this frame's when the trims were evaluated.
		DvTrimSelection::Selection conditionsSelection;
		if (f.trims || !m_trimsLogged.empty())
		{
			std::string state;
			const bool dvPicture = state.empty() && renderImage.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION &&
				f.dm && f.dm->valid;
			pl_color_space display = target.color;
			pl_color_space_infer(&display);
			const bool pq = target.color.transfer == PL_COLOR_TRC_PQ;
			// Review C5: only transfers that are a pure power law (or BT.1886 / sRGB,
			// which LedDisplayGamma maps to 2.4 / 2.2); linear, HLG and others: off.
			const enum pl_color_transfer trc = target.color.transfer;
			const bool gammaSignal = trc == PL_COLOR_TRC_GAMMA18 || trc == PL_COLOR_TRC_GAMMA20 ||
				trc == PL_COLOR_TRC_GAMMA22 || trc == PL_COLOR_TRC_GAMMA24 || trc == PL_COLOR_TRC_GAMMA26 ||
				trc == PL_COLOR_TRC_GAMMA28 || trc == PL_COLOR_TRC_BT_1886 || trc == PL_COLOR_TRC_SRGB;
			std::string params;
			DvTrimSelection::Selection selection;   // feat/dv-l8: L8, else L2, else none
			if (!f.trims || !f.colourStageRuns)
				state = "off";
			else if (!dvPicture)
				state = "idle (no Dolby Vision picture with DM)";
			else if (target.lut)
				state = "off with a calibration LUT (it would act after the LUT)";
			else if (!pq && !gammaSignal)
				state = "off for this output transfer (PQ, gamma, BT.1886 or sRGB outputs only)";
			else
			{
				selection = DvTrimSelection::Select(*f.dm, display.hdr.max_luma);
				m_trimsSelection = selection;
				m_trimsEvaluated = true;
				m_trimsTargetNits = display.hdr.max_luma;
				if (!selection.apply)
					state = std::string("neutral: ") + selection.reason;
				else
				{
					if (!m_trimsConfigured)
					{
						ConfiguredShaderRule rule;
						rule.name = "dv_trims";
						rule.filename = "DV trims.glsl";
						m_trimsHook.Configure({ rule });
						m_trimsConfigured = true;
					}
					m_trimsHook.Ensure(hookTemplates);
					const size_t start = frameHooks.size();
					m_trimsHook.Collect(false, frameHooks);
					// Review C4: the luminance coefficients of the output's own primaries.
					const pl_raw_primaries* prim = pl_raw_primaries_get(display.primaries);
					double kr = 0.2126, kg = 0.7152, kb = 0.0722;
					if (prim)
					{
						const double red[2] = { prim->red.x, prim->red.y }, green[2] = { prim->green.x, prim->green.y };
						const double blue[2] = { prim->blue.x, prim->blue.y }, white[2] = { prim->white.x, prim->white.y };
						DolbyVisionTrims::LuminanceWeights(red, green, blue, white, kr, kg, kb);
					}
					const auto ictcp = DvDolbyStyle::ShaderParameters(m_toneDolbyRunning, prim);   // dv_space + output RGB -> LMS
					for (size_t i = start; i < frameHooks.size(); ++i)
					{
						for (const auto& parameter : DvTrimSelection::ShaderParameters(selection.params))
							CustomHookChain::SetHookParameter(frameHooks[i], parameter.first, parameter.second);
						for (const auto& parameter : ictcp)
							CustomHookChain::SetHookParameter(frameHooks[i], parameter.first, parameter.second);
						CustomHookChain::SetHookParameter(frameHooks[i], "dv_signal_pq", pq ? 1.0 : 0.0);
						CustomHookChain::SetHookParameter(frameHooks[i], "dv_gamma", f.outputGamma);
						CustomHookChain::SetHookParameter(frameHooks[i], "dv_peak_nits", display.hdr.max_luma);
						CustomHookChain::SetHookParameter(frameHooks[i], "dv_kr", kr);
						CustomHookChain::SetHookParameter(frameHooks[i], "dv_kb", kb);
					}
					state = frameHooks.size() == start ? "NOT applied (shaders\\DV trims.glsl did not load)" :
						std::string("active: ") + selection.reason;
					if (frameHooks.size() > start)
						params = DvTrimSelection::Describe(selection);
				}
			}
			// test/dv-synthetic 2026-10-24 (no silent fallbacks): dv_trims on, a DV picture with DM,
			// but the trim cannot act (a calibration LUT, an unsupported output transfer, the shader
			// missing): listed. Neutral (no L2 for this picture) is the trims' own answer, not a fallback.
			m_trimsStatus = DvTrimSelection::StatusText(f.trims, selection);
			conditionsSelection = selection;
			m_trimsShaderMissing = state.compare(0, 11, "NOT applied") == 0;
			// Audit 5: L8 present but every L8 target unknown and no L2: nothing applies - listed too.
			const bool dvTrimsUnknownTargets = dvPicture && selection.level == DvTrimSelection::Level::None && selection.l8Unresolved > 0;
			if (f.trims && dvPicture &&
				(target.lut || (!pq && !gammaSignal) || m_trimsShaderMissing || dvTrimsUnknownTargets))
				m_engage("Dolby Vision trims", dvTrimsUnknownTargets && !target.lut && (pq || gammaSignal) && !m_trimsShaderMissing ?
					"L8 trims present but every L8 target is unknown (no L10, not a known preset) and no L2: none applied" : state);
			else
				m_release("Dolby Vision trims");
			// Review S5: the reason on change; the parameters (they follow the
			// scene) at most every 10 s, and only when they changed.
			if (state != m_trimsLogged)
			{
				m_trimsLogged = f.trims ? state : std::string();
				DebugLog::Log("Dolby Vision trims (non-certified): %s (display peak %.1f nits)", state.c_str(),
					static_cast<double>(display.hdr.max_luma));
			}
			const ULONGLONG trimsNow = GetTickCount64();
			if (!params.empty() && params != m_trimsParamsLogged &&
				(m_trimsParamsTick == 0 || trimsNow - m_trimsParamsTick >= 10000))
			{
				m_trimsParamsLogged = params;
				m_trimsParamsTick = trimsNow;
				DebugLog::Log("Dolby Vision trims (non-certified): %s", params.c_str());
			}
		}
		// Audit M-3 (2026-10-25): the reference conditions (curve, L6 / L0 mastering, L9, L11, the
		// trims when they ran) for EVERY DV picture with DM - the CAM16 stage needs them with or
		// without dv_trims; they were refreshed only inside the trims block before.
		if (dvTonePicture)
			m_referenceConditions = DvDolbyStyle::Conditions(*f.dm, conditionsSelection, m_toneDolbyRunning, f.libplaceboCurve);
		m_compare.Configure(f.compareFolder);   // feat/dv-l8 WIP: the Config folder
		if (m_compare.Enabled())   // feat/dv-l8: DV comparison capture (diagnostic; VP_DV_COMPARE_PATH)
			m_compare.OnRender(f.state, f.rawFrame, f.counter,
				f.state.dolbyVisionTunnel ? f.dm : nullptr, f.trims,
				std::exchange(m_trimsEvaluated, false) ? &m_trimsSelection : nullptr, m_trimsTargetNits);
	}

private:
	CustomHookChain::Chain m_trimsHook;      // DV L2 trims (dv_trims)
	Engage m_engage;
	Release m_release;
	bool m_trimsConfigured = false;
	std::string m_trimsLogged;          // the state / reason last logged (on change)
	std::string m_trimsParamsLogged;    // the parameters last logged (at most every 10 s)
	ULONGLONG m_trimsParamsTick = 0;
	// feat/dv-l8: the trim level in use (the DV ingress / Ctrl+I line), the last choice for the
	// DV comparison capture (off unless VP_DV_COMPARE_PATH; DvCompareCapture.h).
	std::string m_trimsStatus = "DV trims: off";
	DvTrimSelection::Selection m_trimsSelection;
	bool m_trimsEvaluated = false;
	double m_trimsTargetNits = 0.0;
	DvCompareCapture::Recorder m_compare{ DvCompareCapture::Options::FromEnvironment() };
	// feat/dv-l8 task 5: the Dolby-style DV tone mapping (dv_tone_mapping), its status, and the
	// reference viewing conditions of the last DV picture (for the later CAM16 stage).
	DvDolbyStyle::ToneSwitch m_toneSwitch;
	bool m_toneDolbyRunning = false;
	std::string m_toneStatus = "DV tone mapping: libplacebo";
	DvDolbyStyle::ReferenceConditions m_referenceConditions;
	bool m_trimsShaderMissing = false;  // the last trims attempt found shaders/DV trims.glsl missing
	bool m_debandSkipped = false;       // DvTunnelPolicy: libplacebo deband dropped for this DV picture
	bool m_vpDebandSkipped = false;     // DvTunnelPolicy: VP's deband skipped for this DV picture
	bool m_debandSkipLogged = false;
	int m_toneL1Logged = -1;            // audit 7: L1 reaching libplacebo, logged on change
};
