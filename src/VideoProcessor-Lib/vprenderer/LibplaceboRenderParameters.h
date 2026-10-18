#pragma once

// Maps the resolved VP Renderer settings to the exact libplacebo parameter
// structures passed to pl_render_image.  Keeping this boundary separate makes
// the mapping independently testable without creating a D3D11 renderer.

#include <libplacebo/renderer.h>

#include <string>
#include "../ToneMappingTuning.h"

namespace LibplaceboRenderParameters
{
    // A saved zero is explicit black, not libplacebo's unspecified sentinel.
    inline float ResolveTargetBlack(float blackNits)
    {
        return blackNits == 0.0f ? PL_COLOR_HDR_BLACK : blackNits;
    }

	// "Auto" deliberately means retain the selected quality preset's value.
	enum class Toggle
	{
		Auto,
		On,
		Off
	};

	// Peak detection has two deliberately distinct native parameter sets.
	// Do not collapse Standard and HighQuality into Toggle::On: the latter has
	// a much higher percentile and a substantially higher cost.
	enum class PeakDetection
	{
		Auto,
		Standard,
		HighQuality,
		Off
	};

	struct Settings
	{
		std::string quality = "high";
		std::string toneMapping = "auto";
		std::string gamutMapping = "auto";
		PeakDetection peakDetection = PeakDetection::Auto;
		ToneMappingTuning::Overrides toneTuning{};
		bool dynamicTarget = true;
        bool customToneMapping = false;
		bool hasContrastRecovery = false;
		float contrastRecovery = 0.0f;
		// Saturation offset in percent (-100..100). 0 leaves libplacebo's
		// colour adjustment unset; +5 matches madVR's "saturation +5".
		int saturation = 0;
		std::string upscaler = "auto";
		std::string chromaUpscaler = "auto";   // plane_upscaler; auto = Catmull-Rom, upscaler = the upscaler
		std::string downscaler = "auto";
		// Canonical setting for new configuration. "Auto" preserves the
		// quality preset, "off" disables it, and the other values select an
		// explicit parameter set. `deband` remains for legacy configuration.
		std::string debandStrength = "auto";
		Toggle deband = Toggle::Auto;
		// deband_grain (owner 2026-10-17: film grain is never averaged away; grain audit):
		// false = libplacebo's deband adds no grain (grain 0; VP's dither does that job);
		// true = the selected parameters' grain, as before.
		bool debandGrain = false;
		Toggle sigmoid = Toggle::Auto;
		// auto uses the quality preset; the remaining values name the
		// libplacebo PL_DITHER_* methods, with off disabling dithering.
		std::string dithering = "auto";
		// VP is an interactive renderer: output dimensions, crop geometry and
		// presentation properties can change while the renderer remains live.
		// Keep those values out of compiled program keys so a window resize does
		// not synchronously manufacture a new shader variant.
		bool dynamicConstants = false;
	};

	struct Projection
	{
		// These names identify the data export read from libplacebo. They are
		// intentionally retained for tests and diagnostics; the actual
		// structures below are the values sent to libplacebo.
		std::string qualityPresetExport;
		std::string toneMappingExport;
		std::string gamutMappingExport;
		std::string upscalerExport;
		std::string downscalerExport;

		pl_render_params renderParams{};
		pl_color_map_params colorMapParams{};
		pl_peak_detect_params peakDetectParams{};
		pl_sigmoid_params sigmoidParams{};
		pl_deband_params debandParams{};
		pl_dither_params ditherParams{};
		pl_color_adjustment colorAdjustment{};
	};

	// The target calibration LUT is attached to pl_frame, not render parameters;
	// `hasDisplayLut` remains for source compatibility and does not alter DTM,
	// peak detection, dither, or error diffusion.
	// Returns false only when the expected libplacebo runtime data cannot be
	// obtained. The caller receives a human-readable error in `error`.
    const char* ResolvedPeakDetection(const pl_render_params& parameters);
    std::string ResolvedDithering(const pl_render_params& parameters);

	bool Build(const Settings& settings, bool hasDisplayLut,
		Projection& projection, std::string& error);

	// SDR is display-referred: brightness controls belong only to HDR tone mapping.
	// Equal fixed source/target references preserve SDR code values and keep the
	// renderer's HDR-only scaling/peak decisions independent of display settings.
	void ApplySourceLuminance(bool inputIsSdr, pl_color_space& source);
	void ApplyTargetLuminance(bool inputIsSdr, float targetNits, float blackNits,
		pl_color_space& target);

	// Hunt 3 F2: ambient adaptation for SDR input. The equal source/target references above
	// keep SDR codes as they are, so sdr_black_nits never reaches an SDR picture (intended:
	// the signal is display-referred and the display's own calibration owns its black;
	// CONFIGURATION.html). Ambient adaptation's rise of the effective black above the owner's
	// black (the room's veil x strength) is applied for SDR input as the BT.1886 black lift
	// it stands for: on the owner's display (black B, white W, exponent g of the source
	// transfer) the picture is shown as on a display whose black is B_eff:
	//   L(V') = EOTF_B(V') = EOTF_Beff(V)  =>  V' = s V + o,
	//   s = (lw - le) / (lw - lb), o = (le - lb) / (lw - lb), lb = B^(1/g), le = B_eff^(1/g), lw = W^(1/g)
	// - affine in the signal (s + o = 1: white fixed; s > 0: code order kept, BTB stays below
	// black and WTW above white, both carried as values), applied by libplacebo's colour
	// decode (pl_color_adjustment contrast s, brightness o) before anything else.
	// False (nothing changes, bit-identical) when B_eff <= B (ambient off, strength 0, a dark
	// room) or the source transfer is not a power law (BT.1886, gamma 1.8-2.8).
	bool SdrAmbientLift(double ownerBlackNits, double effectiveBlackNits, double whiteNits,
		enum pl_color_transfer sourceTransfer, float& contrast, float& brightness);
	// The lift on top of an existing adjustment (saturation etc.): RGB' = s (c RGB + b) + o.
	void ApplySdrAmbientLift(float contrast, float brightness, pl_color_adjustment& adjustment);

	// Contain the pre-upstream-4dbc490b D3D11 max_luma-based HDR selection.
	// This copy is transport metadata only; HDR tone mapping keeps its target.
	pl_color_space MakeSwapchainColorHint(const pl_color_space& output);

	// Applies the user-declared native display precision to an already resolved
	// target representation. The storage depth remains unchanged; lower display
	// precision is represented MSB-aligned so libplacebo dithers and writes the
	// correct full-range values into a higher-depth RGB swapchain.
	void ApplyDisplayBitDepth(const std::string& displayBitDepth,
		pl_color_repr& targetRepresentation);
}
