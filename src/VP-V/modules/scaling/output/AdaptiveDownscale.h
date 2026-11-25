#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/shaders.h>

#include <VP-V/core/LumaWeights.h>
#include <VP-V/modules/scaling/output/NlsMaskMapping.h>
#include <VP-V/modules/drive/led_measure/ReadbackRing.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleCut.h>
#include <VP-V/modules/scaling/shared/ScalerPass.h>
#include <VP-V/modules/input/source_analysis/regions/MeasureDomain.h>
#include <VP-V/modules/scaling/output/EdgeMapStageOrdinal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

// Stage 8-2 (docs/VP-stage8-build-plan.md): adaptive downscale — a per-pixel
// blend between a faithful downscale and igv's SSIM downscaler, luma only:
//     w = w_base x (1 - edge) x grain_scale
//     out = faithful + w (Y_ssim - Y_faithful)   (R, G, B; alpha alike)
//
// - Measure hook, PL_HOOK_PRE_KERNEL (texture input, handed back unchanged):
//   the faithful branch (libplacebo's polar ewa_lanczossharp with anti-ringing,
//   from the PREKERNEL picture to the destination size), the edge map at
//   source resolution (Sobel on luma in 8-bit codes, soft ramp edge_lo..
//   edge_hi), and the per-frame grain term: Laplacian energy where the
//   gradient is below edge_lo, shadow-weighted, reduced on the GPU into a
//   persistent 1x1 state with the ramp grain_lo..grain_hi -> grain_floor..1,
//   one-pole smoothing (grain_smooth_ms) and a snap on a cut (the delivered
//   confirmed cut of the skeleton's scene product, U7 v2/v3, read from the
//   frame-analysis store; never on a fade; see AdaptiveDownscaleCut.h;
//   FIX-X5-CUT-P4, C5). No read-back on the render path.
// - Blend hook, PL_HOOK_POST_KERNEL, after the user hooks (the SSIM shader's
//   passes): the blend above. Alpha is passed through (Stage 10b keeps its
//   out-of-range remainder in its own texture, not in alpha).
//
// Both hooks do nothing unless the frame downscales vertically, which is the
// SSIM shader's own condition (NATIVE_CROPPED.h > POSTKERNEL.h): the source
// crop's own height against the destination's, given per frame by the
// renderer, since a LUMA prescaler can make the PRE_KERNEL picture taller
// than the source (audit stage8-rs232 item 3). The SSIM shader (unchanged,
// on Mitchell) must be in the chain: the renderer checks.
// The grain state advances once per source frame (de-judder extras on the
// source path re-run the hooks without advancing it).
//
// Stage 8-3, the vector grain map: with the de-judder's matcher running for
// the frame (film rates, a next frame queued; DejudderPolicy grainMap), the
// grain term is per 16x16 source block instead of one number per frame. The
// matcher's high-passed temporal error per block (Dejudder::MotionWarp::
// GrainError) goes through the same ramp as the scalar, scaled to the same
// code units, and is smoothed per block over grain_map_smooth_ms, following
// the content along the block's vector; a cut resets it. In the blend it is
// bilinear between block centres; untrusted blocks, and frames without a
// field, take the scalar.
namespace LedDrive { class NativeTexReadback; }

namespace AdaptiveDownscale
{
	struct Settings
	{
		float wBase = 1.0f;
		float edgeLo = 8.0f, edgeHi = 40.0f;       // 8-bit codes (gradient)
		float grainLo = 2.0f, grainHi = 8.0f;      // 8-bit codes (Laplacian energy)
		float grainFloor = 0.4f;
		float grainSmoothMs = 1000.0f;
		float shadowBias = 0.5f;
		bool chromaSsim = false;                   // true: blend R, G, B fully
		float antiring = 0.5f;                     // the faithful branch's anti-ringing
		bool grainMap = true;                      // 8-3: per-block map from the vectors where available
		float grainMapSmoothMs = 500.0f;
		// Owner 2026-10-13 efficiency: the faithful branch as two separable
		// Lanczos passes instead of the polar ewa_lanczossharp.
		bool faithfulOrtho = false;
		// adaptive_faithful auto (owner 2026-10-16): polar where both axes shrink alike, the
		// separable (1D) filter when they differ by more than 10 % - a circular polar footprint
		// would soften the axis that should stay sharp (one-axis or anamorphic scaling).
		bool faithfulAuto = true;   // the default since 2026-10-16 (owner: Auto or 1D)
		// Stage 12 §2c / delta 15 S8-1 (owner 2026-10-15): the CNN's (upscale)
		// and SSIM's (downscale) added detail held along the de-judder's
		// trusted vectors (a one-pole over ~3 frames), so it does not shimmer;
		// the faithful part untouched; untrusted blocks, no vectors or NLS:
		// this frame's residual (today's output).
		bool cnnResidualStabilise = true;
		bool ssimResidualStabilise = true;
		// residual_stabilise_grain_guard (owner 2026-10-17: grain is never averaged away;
		// grain audit): the hold limited to the coding-artefact band - blocks the grain term
		// marks as grain pass this frame's residual through, the held change is clamped to
		// the expected coding-noise RMS (residualCodingNoiseCodes, when there is a bitrate
		// reading) and the previous residual is fetched by texel, not bilinear. off = the
		// 2026-10-15 one-pole, byte for byte.
		bool residualGrainGuard = true;
		// Set per frame by the renderer from the bitrate-quality reading
		// (GrainPreserve::CodingNoiseCodes; 8-bit codes); 0 = no reading, no clamp.
		float residualCodingNoiseCodes = 0.0f;
		// Audit 2026-10-16: the faithful branch's textures (they become the
		// picture) in 32-bit float. Set by the renderer from its rgba32f FBOs'
		// effective state (fbo_float32); falls back to 16-bit float when the
		// GPU has no suitable rgba32f format. Precision only, no switch.
		bool float32Picture = true;
		// Motion-adapted blend (owner 2026-10-31; continuum strength [0, 1], 0 = standard unadapted blend):
		// the sharper branch's weight (SSIM on a downscale, the CNN on an upscale) kept where
		// the content is steady and confidently tracked and lowered towards the faithful branch
		// on motion-boundary shear, fast rigid pans, or low confidence. Per 16x16 block once per source
		// frame: target = floor + (1 - floor) x conf x (1 - max(smoothstep(speed_lo, speed_hi, shear_speed),
		// smoothstep(2*speed_hi, 4*speed_hi, speed))); bilinear temporal advection along the vector with
		// asymmetric fast-attack (tau/4) / smooth-release (tau) one-pole; bilinear between block centres
		// in the blend; mf = 1 - motionStrength x (1 - mapFactor).
		float motionStrength = 0.0f;     // 0 = off (standard blend), 1 = full motion-adapted blend
		float motionSpeedLo = 0.1f;      // picture heights per second: full sharper-branch weight below
		float motionSpeedHi = 0.4f;      // picture heights per second: the floor above
		float motionFloor = 0.0f;        // the factor on fast / untrusted motion (0 = faithful only)
		float motionSmoothMs = 250.0f;
	};

	inline bool operator==(const Settings& a, const Settings& b)
	{
		return a.wBase == b.wBase && a.edgeLo == b.edgeLo && a.edgeHi == b.edgeHi && a.grainLo == b.grainLo &&
			a.grainHi == b.grainHi && a.grainFloor == b.grainFloor && a.grainSmoothMs == b.grainSmoothMs &&
			a.shadowBias == b.shadowBias && a.chromaSsim == b.chromaSsim && a.antiring == b.antiring &&
			a.grainMap == b.grainMap && a.grainMapSmoothMs == b.grainMapSmoothMs && a.faithfulOrtho == b.faithfulOrtho && a.faithfulAuto == b.faithfulAuto &&
			a.cnnResidualStabilise == b.cnnResidualStabilise && a.ssimResidualStabilise == b.ssimResidualStabilise &&
			a.residualGrainGuard == b.residualGrainGuard &&
			a.float32Picture == b.float32Picture &&
			a.motionStrength == b.motionStrength && a.motionSpeedLo == b.motionSpeedLo && a.motionSpeedHi == b.motionSpeedHi &&
			a.motionFloor == b.motionFloor && a.motionSmoothMs == b.motionSmoothMs;
	}

	// 8-3: the de-judder's field for the source frame being rendered.
	struct GrainField
	{
		pl_tex error = nullptr;        // MotionWarp::GrainError()
		pl_tex field = nullptr;        // MotionWarp::Field()
		pl_tex frameTrust = nullptr;   // MotionWarp::FrameTrust()
		float minTrustedShare = 0.25f; int fieldBlockPx = 16; float scaleToSourcePx = 1.0f;
		// The source crop libplacebo scales (the block grid is in source
		// pixels of the whole frame) and, with NLS, the NLS map over it.
		pl_rect2df sourceCrop{};
		NlsMaskMapping::Mapping nls{};
	};

	// X5-GRAIN-P10: the frame's grain energy derived from the shared products instead of the private Laplacian
	// partial sums: P10's grain level N per block (p10Blocks, .x), P11's block mean luma (p11Stats0, .x) and flat
	// coverage (p11Stats4: .x picture share x .y off-edge share), all on one block grid of blockPx source pixels.
	// No source (any null texture): the private Laplacian estimate stays as the VISIBLE fallback (Stage status line).
	struct GrainSource
	{
		pl_tex p10Blocks = nullptr;    // GrainSplit::Gpu view: gs_blocks(b).x = N (codes^2 in the product's code unit)
		pl_tex p11Stats0 = nullptr;    // RegionEvidence S0: meanY in codes (the product's code unit)
		pl_tex p11Stats4 = nullptr;    // RegionEvidence S4: coverage, offShare
		int blockPx = 16;              // source pixels per block edge
		float codeToEightBit = 1.0f;   // 255 / the code unit's white: N and meanY to the 8-bit codes the thresholds use
		pl_rect2df sourceCrop{};       // the source crop libplacebo scales, in the blocks' source pixels (blocks centred in it count)
		// Gate cb-u1 r1 SHOULD 5: the three textures must share ONE block grid, or the per-block reads would misalign.
		// Null-safe (r2 NIT): any missing texture is no shared grid.
		bool SameGrid() const
		{
			return p10Blocks && p11Stats0 && p11Stats4 &&
				p10Blocks->params.w == p11Stats0->params.w && p10Blocks->params.h == p11Stats0->params.h &&
				p10Blocks->params.w == p11Stats4->params.w && p10Blocks->params.h == p11Stats4->params.h;
		}
		bool Valid() const { return p10Blocks && p11Stats0 && p11Stats4 && blockPx > 0 && codeToEightBit > 0.0f && SameGrid(); }
	};

	// Which grain estimate the last scalar pass used (no silent fallbacks; gate cb-u1 r1 MUST 5 / 6).
	enum class GrainSourceUse : uint8_t
	{
		NotSupplied,     // no valid GrainSource this frame: the private estimate
		MeasureDomain,   // supplied, but the measure domain is linear / HDR, so P10 N (source codes) is not in the ramp's units
		NoUsableBlock,   // bound, but the last read-back frame had no usable block (all vetoed or outside the crop)
		Shared,          // the last read-back frame's energy came from P10 / P11
		Pending,         // bound now, but no read-back yet of a frame that bound them (r2 NIT): the outcome is not known yet
	};

	// One Gaussian-noise sigma in the mean-|Laplacian| energy units the grain ramp is defined in: the discrete
	// Laplacian 4e - (b + d + f + h) of white noise has std sqrt(20) sigma, and E|x| = sqrt(2/pi) std for a Gaussian.
	constexpr float kEdgeEnergyPerSigma = 3.5682482f;   // sqrt(20) * sqrt(2 / pi) = 4.4721360 * 0.7978846

	// Stage 12 §2c / S8-1: the de-judder's motion for the source frame being
	// rendered (from the last estimate or Acquire(kP3AnalysisField); null = none: this frame's residual).
	struct Motion
	{
		pl_tex field = nullptr;          // P3 analysis field: vx, vy (source px), trust, error per 16x16 block
		pl_tex frameTrust = nullptr;     // frame trust texture: r = the trusted share
		std::shared_ptr<const void> pin; // M1#1: keeps Acquire(kP3AnalysisField) alive until the next frame / ClearMotion()
		float minTrustedShare = 0.25f; int fieldBlockPx = 16; float scaleToSourcePx = 1.0f;
		pl_rect2df sourceCrop{};         // the source crop libplacebo scales, in the field's source pixels
		NlsMaskMapping::Mapping nls{};   // NLS coordinate mapping when active (residual hold stays off under NLS)
	};

	// Stage 12 §2c / delta 15 S8-2 (owner 2026-10-15): the weight scale for a
	// low bitrate quality - 1 at 0.8 and above (today's output), falling to
	// `floor` at 0.2 and below, in steps of 0.05 (no churn as the reading
	// moves). SSIM and the CNN sharpen blocking and mosquito noise as if they
	// were texture.
	// The guard transfer, its linear mirror and the measure domain moved to the edge map's owner (CB slice 18 S2a, unchanged:
	// input/source_analysis/regions/MeasureDomain.h); the names stay usable here.
	using VpMeasure::GuardTransferParams;
	using VpMeasure::GuardTransfer;
	using VpMeasure::GuardLinear;
	using VpMeasure::MeasureDomain;
	using VpMeasure::MeasureDomainFor;
	using VpMeasure::LumaWeightsGlsl;
	using VpMeasure::MeasureLumaCode;
	// CPU mirrors of the shader's rs_code / rs_slope (tests); rs_lin's is GuardLinear (MeasureDomain.h).
	inline double GuardCode(const GuardTransferParams& t, double y)
	{
		y = y > 0.0 ? y : 0.0;
		if (t.mode < 0.5)
			return (std::max)(std::pow(y / t.a, 1.0 / t.g) - t.b, 0.0);
		if (t.mode < 1.5)
		{
			const double v = std::pow(y * (203.0 / 10000.0), 0.1593017578125);
			return std::pow((0.8359375 + 18.8515625 * v) / (1.0 + 18.6875 * v), 78.84375);
		}
		const double v = std::pow(y / t.a, 1.0 / t.g) * 12.0;
		const double x = v <= 1.0 ? 0.5 * std::sqrt(v) : 0.17883277 * std::log((std::max)(v - 0.28466892, 1e-6)) + 0.55991073;
		return (x - t.b) / (std::max)(1.0 - t.b, 1e-6);
	}
	inline double GuardSlope(const GuardTransferParams& t, double y)
	{
		const double c = (std::max)(GuardCode(t, y), 1e-3);
		const double lo = (std::max)(c - 1e-3, 0.0), hi = c + 1e-3;
		return (GuardLinear(t, hi) - GuardLinear(t, lo)) / (hi - lo);
	}

	inline double BitrateScale(double quality, double floor)
	{
		floor = floor < 0.0 ? 0.0 : floor > 1.0 ? 1.0 : floor;
		double scale = quality >= 0.8 ? 1.0 : quality <= 0.2 ? floor : floor + (1.0 - floor) * (quality - 0.2) / 0.6;
		scale = static_cast<double>(static_cast<long long>(scale * 20.0 + 0.5)) / 20.0;
		return scale < floor ? floor : scale > 1.0 ? 1.0 : scale;
	}
	// The faithful branch's anti-ringing raised with it: halfway to 1 at the floor.
	inline float BitrateAntiring(float antiring, double scale, double floor)
	{
		if (scale >= 1.0 || floor >= 1.0) return antiring;
		const double t = (1.0 - scale) / (1.0 - floor);
		return static_cast<float>(antiring + (1.0 - antiring) * 0.5 * (t > 1.0 ? 1.0 : t));
	}

	// Motion-adapted blend: CPU mirrors of the motion-map shader (defined in AdaptiveDownscale.cpp).
	// `speed` and `shearSpeed` in picture heights per second; `confidence` continuous in 0..1.
	float MotionSanitizeUnit(float x);
	float MotionSmoothstep(float lo, float hi, float x);
	float MotionTarget(bool frameTrusted, float speed, float shearSpeed, float confidence, const Settings& s);
	float MotionTarget(bool frameTrusted, bool blockTrusted, float speed, float confidence, const Settings& s);
	float MotionSpeed(float vx, float vy, float cropHeight, float frameSeconds);
	float MotionShearSpeed(const float* fieldRgba, int bw, int bh, int bx, int by, float cropHeight, float frameSeconds);
	float MotionAdvectBilinear(const float* map, int bw, int bh, float pcX, float pcY, int stride = 1);
	void MotionAttackReleaseAlphas(float frameSeconds, float smoothMs, float& alphaAttack, float& alphaRelease);
	float MotionStepAsymmetric(float previousAlongVector, float target, float alphaAttack, float alphaRelease, bool reset);
	float MotionStep(float previousAlongVector, float target, float alpha, bool reset);

	// Matcher error (mean absolute residual of half-resolution luma between
	// two frames) to the scalar's Laplacian energy units, for white noise of
	// one code per source pixel: the Laplacian's mean absolute value is
	// 0.798 x sqrt(20) = 3.57; the residual of two half-resolution frames
	// (2x2 means) is 0.798 x sqrt(2) / 2 = 0.564. So one ramp (grain_lo..hi)
	// serves both.
	constexpr float kTemporalToSpatial = 3.57f / 0.564f;

	struct Stats
	{
		bool valid = false;
		float grainScale = 1.0f;
		float meanW = 0.0f;     // w_base x mean(1 - edge) x grain_scale
		float meanLuma = 0.0f;  // the newest advanced frame's APL (8-bit codes) from the P4 record; 0 when P4 was unavailable
		// Audit x5dup S2: the field above is taken when the statistics are collected, so it can be a frame newer than grainScale and
		// meanW (those are read back a few frames late).
		bool grainFromShared = false;   // X5-GRAIN-P10: the frame's grain energy came from P10 / P11 (stats tag 2)
		bool grainBound = false;        // ... and that frame's scalar pass bound the shared products (r2 NIT: Pending until then)
	};

	// Stage 8b (docs/VP-stage8b-upscale-blend.md), CF-2 B1 (refs/123 v2 N1.2: the hybrid law, scaling/shared/HybridBlend.h):
	// the same module upscaling a shorter source (1080p -> 1600p), any output ratio. Luma only:
	//     w = clamp(up_w_base x (1 - e) x grain_scale) x motion;  out = sharp + (1 - w)(Y_alias_safe - Y_sharp)
	// sharp = the POST_KERNEL picture (libplacebo's main scaler; a LUMA prescaler before it = the CNN evaluation arm),
	// alias_safe = the faithful ScalerPass from the PRE_KERNEL source-resolution picture. e = the edge ramp: kEdgeMap's stage variant
	// (CB-18: the measure pass offers its base picture and acquires EdgeMapStageKey; EdgeMapStatus names any other source).
	// grain_scale on this form = kP10gGrainLevel (SetGrainLevel, the stage's one ask per frame); not published = grain_floor
	// (the alias-safe lean, visible: PublishedGrainUsed false, the stage's status line and catalog entry). No private grain term.
	// Motion: the map's factor; motion_floor when the vectors are absent or invalid (vectorsInvalid; N1.4 alias-safe lean).
	// A clean flat picture is the sharp branch exactly and chroma stays the sharp
	// branch's (CfL). The faithful branch (anti-ringing) is from the source-resolution picture: PRE_KERNEL itself,
	// or, when a luma doubler (FSRCNNX / ArtCNN) made it exactly 2x the source
	// crop, its 2x2 box average (the doubler's own consistency, which
	// SSimSuperRes relies on too). Other prescale ratios: not applied (the
	// hooks hand the picture back). The grain term is measured on that same
	// source-resolution picture; the 8-3 map is shared. With superresAlpha the
	// blend writes the grain term into alpha for SSimSuperRes's final pass
	// (its vp_grain_strength), which scales its correction by it and writes
	// alpha back to 1.
	struct Upscale
	{
		bool on = false;            // this frame is an upscale handled by the module
		float wBase = 1.0f;
		bool superresAlpha = false; // SSimSuperRes reads the grain term from alpha
		float nativeHeight = 0.0f;  // the source crop's height (the doubler test)
		// Stage 8b one-axis (N9-P12): the source crop's width (0 = not given: the height decides
		// alone, as before) and a frame upscaled on one axis only: faithful branch only (w = 0;
		// with superresAlpha the grain term handed to SSimSuperRes is 0, so it adds nothing).
		float nativeWidth = 0.0f;
		bool oneAxis = false;
		// CF-2 B1 (N1.4): the vectors of this frame are absent or invalid (no live estimate, a resync, a drop, NLS, motion off):
		// with adaptive_motion on, the motion term is motion_floor (the alias-safe lean), never the plain factor 1.
		bool vectorsInvalid = false;
	};

	// CF-2 switch-over: kP10gGrainLevel (id 35, "grain.level") for the upscale form's grain term, acquired by the stage once per
	// frame through the skeleton; the pin keeps it alive until the next frame. level null = not published (grain_floor).
	struct GrainLevelSource
	{
		pl_tex level = nullptr;                // RGBA32F per blockPx x blockPx source-luma block: x = N_b, y = gate, z = f'^2 N_b, w = lag-1
		std::shared_ptr<const void> pin;
		pl_rect2df sourceCrop{};               // the source crop in the level's source-luma pixels
		float codeToEightBit = 255.0f / 1023.0f;   // M2 (grain-consumes-cb-u1): source analysis codes -> 8-bit codes
		int blockPx = 16;                          // S1: block size of the kP10gGrainLevel grid (GrainSplit::DefaultParams().blockPx)
		NlsMaskMapping::Mapping nls{};             // NLS coordinate mapping when active
	};

	// M2 (grain-consumes-cb-u1): sqrt(N_b) in source analysis codes -> the grain ramp's 8-bit mean-|Laplacian| energy units.
	// Derived from white-noise physics (no 1.0f placeholder): kEdgeEnergyPerSigma = sqrt(20) * sqrt(2/pi) = 3.5682482f,
	// scaled from source analysis codes to 8-bit codes (codeToEightBit, default 255 / 1023 for the 10-bit analysis axis).
	inline float GrainLevelEnergyScale(float codeToEightBit = 255.0f / 1023.0f)
	{
		return kEdgeEnergyPerSigma * (codeToEightBit > 0.0f ? codeToEightBit : (255.0f / 1023.0f));
	}

	// The prescale ratio of PRE_KERNEL to the source crop the measure accepts: 1 (the source), 2 (a doubler's exact 2x), 0 (not
	// applied: another ratio, or nothing upscaled). Independent of the output ratio (any destination size). Pure (tests).
	inline int UpscaleBaseRatioOf(float nativeW, float nativeH, float preW, float preH, int dw, int dh)
	{
		const float rh = nativeH > 0.0f ? preH / nativeH : 0.0f;
		const float rw = nativeW > 0.0f ? preW / nativeW : rh;
		const int ratio = std::fabs(rh - 1.0f) < 0.01f && std::fabs(rw - 1.0f) < 0.01f ? 1 :
			(std::fabs(rh - 2.0f) < 0.01f && std::fabs(rw - 2.0f) < 0.01f ? 2 : 0);
		const bool upscales = nativeH + 0.5f < static_cast<float>(dh) ||
			(nativeW > 0.0f && nativeW + 0.5f < static_cast<float>(dw));
		return upscales ? ratio : 0;
	}

	// The motion-adapted blend on the last blended frame (status, tests).
	enum class MotionState { Off, NoVectors, Used };

	// CF-2 FIX1 (gate M1): why the upscale form leans on the motion floor on a blended frame (nullptr = the factor map is
	// used). The stage's own verdict comes first (vectorsInvalid); the rest are the module's: the map did not run, is not
	// ready, belongs to another source frame, or has no texture. Pure (the module and the tests); the stage feeds the cause
	// into its "output scaling" entry, so a floor taken here is never only a status line.
	inline const char* MotionFloorCause(bool vectorsInvalid, MotionState state, bool mapReady, bool sameSequence, bool haveTexture)
	{
		if (vectorsInvalid)
			return "vectors invalid";
		if (state != MotionState::Used)
			return state == MotionState::Off ? "motion map not built on this frame" : "no motion map for this source frame";
		if (!mapReady)
			return "motion map not ready";
		if (!sameSequence)
			return "motion map from another source frame";
		if (!haveTexture)
			return "motion map texture missing";
		return nullptr;
	}

	// Stage 8b's gate and its one-axis form, from the re-based source crop and the destination
	// (half a pixel tolerance). The upscale form: the height upscaled (today's gate, whatever the
	// width does), or the width upscaled with the height 1:1 (new: the height-only gate missed it).
	// One axis = one axis upscaled and the other 1:1. A mixed frame (one axis up, the other down)
	// keeps today's routing: height up = the two-axis upscale blend; width up with the height
	// shrinking = the downscale form. No width given (0): the height decides alone.
	struct UpscaleAxes { bool any = false; bool oneAxis = false; };
	inline UpscaleAxes UpscaleAxesOf(float sourceW, float sourceH, float targetW, float targetH)
	{
		const bool haveW = sourceW > 0.0f;
		const bool upW = haveW && sourceW + 0.5f < targetW;
		const bool upH = sourceH > 0.0f && sourceH + 0.5f < targetH;
		const bool sameW = haveW && std::fabs(sourceW - targetW) <= 0.5f;
		const bool sameH = sourceH > 0.0f && std::fabs(sourceH - targetH) <= 0.5f;
		UpscaleAxes a;
		a.any = upH || (upW && sameH);
		a.oneAxis = (upH && sameW) || (upW && sameH);
		return a;
	}

	// Whether the map was used on the last rendered frame (for the stats line).
	enum class MapState { Off, NoField, Used };

	class Module
	{
	public:
		Module();
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;
		~Module();

		// Per render: settings, the source frame it shows and its duration.
		// The grain state advances only on a new source sequence.
		void SetFrame(const Settings& settings, uint64_t sourceSequence, float frameSeconds);
		// FIX-X5-CUT-P4 / C5: where the cut event of a new source frame comes from (the stage sets SceneCutSource(), the skeleton's
		// scene product; tests set their own).
		void SetCutSource(CutSource source) { cutSource = std::move(source); cutResolved = false; cutContinuous = false; }
		bool HasCutSource() const { return static_cast<bool>(cutSource); }
		// False when the last asked cut event could not be read (cutCause says why; no cut is made then, visibly).
		bool CutAvailable() const { return cutAvailable; }
		const std::string& CutCause() const { return cutCause; }
		unsigned long long CutUnavailableFrames() const { return cutUnavailableFrames; }
		// C5: advances whose scene group said fade (no snap: partial-reset territory), and snaps on a cut delivered late (normally
		// one source frame after the cut; Scene::cutLate). Status and tests.
		unsigned long long CutFadeFrames() const { return cutFadeFrames; }
		unsigned long long CutLateSnaps() const { return cutLateSnaps; }
		// The APL of the last asked frame was not read (the P4 Acquire failed): why. Empty when read. Not a cut fault.
		const std::string& AplCause() const { return aplCause; }
		// GEM-W1B V3 MUST-6: the faithful picture was asked in 32-bit float and the GPU gave 16-bit (visible, never silent).
		bool HalfFloatPictureFallback() const { return halfFloatPicture; }
		// The SSIM shader's condition for this frame: the source crop is
		// taller than the destination rectangle.
		void SetSourceDownscales(bool value) { sourceDownscales = value; }
		// Stage 8b: per frame, after SetFrame (off: the downscale module).
		void SetUpscale(const Upscale& value) { upscale = value; }
		// The last measured frame's source-resolution base: 1 (PRE_KERNEL),
		// 2 (a doubler's output averaged), 0 not applied; tests and the log.
		int LastUpscaleRatio() const { return upscaleRatio; }
		// CF-2 switch-over: per frame (the stage): the published grain level, or none (grain_floor on the upscale form).
		void SetGrainLevel(const GrainLevelSource& value) { grainLevel = value; }
		void ClearGrainLevel() { grainLevel = GrainLevelSource(); }
		// The last blended frame read the published grain level / refused it on linear-HDR domain / used the motion floor (status, tests).
		bool PublishedGrainUsed() const { return publishedGrainUsed; }
		bool PublishedGrainDomainRefused() const { return publishedGrainDomainRefused; }
		bool MotionFloorUsed() const { return motionFloorUsed; }
		// CF-2 FIX1 (gate M1): the cause of that floor (empty when it was not used).
		const std::string& MotionFloorCauseText() const { return motionFloorCause; }
		// Why the alias-safe ScalerPass failed last (empty: never); for the fallback line.
		const std::string& ScalerError() const { return faithfulScaler.LastError(); }
		// 8-3: after SetFrame, the field for this source frame (none: the
		// scalar). Call every frame; the textures are the matcher's.
		void SetGrainField(const GrainField& field) { grainField = field; }
		void ClearGrainField() { grainField = GrainField(); }
		// X5-GRAIN-P10: after SetFrame, the shared grain products for this source frame (call every frame;
		// ClearGrainSource = none: the private Laplacian estimate runs, shown in the stage status).
		void SetGrainSource(const GrainSource& value) { grainSource = value; }
		void ClearGrainSource() { grainSource = GrainSource(); }
		// Which estimate the grain energy came from (status, tests; gate cb-u1 r1 MUST 5). The per-frame "no usable block"
		// fallback runs inside the shader, so Shared needs the read-back stats tag, not only the binding. Gate cb-u1 r2 NIT:
		// the read-back lags the binding by a few frames, so until a read-back of a frame that BOUND the products arrives
		// (stats.grainBound) the outcome is Pending, never a transient NoUsableBlock.
		GrainSourceUse LastGrainSourceUse() const
		{
			if (!grainFromShared)
				return grainDomainRefused ? GrainSourceUse::MeasureDomain : GrainSourceUse::NotSupplied;
			if (!stats.valid || !stats.grainBound)
				return GrainSourceUse::Pending;
			return stats.grainFromShared ? GrainSourceUse::Shared : GrainSourceUse::NoUsableBlock;
		}
		// True when the last read-back grain energy came from the shared products (false: the private estimate).
		bool LastGrainFromSharedProducts() const { return LastGrainSourceUse() == GrainSourceUse::Shared; }
		// Stage 12 §2c / S8-1: after SetFrame, the motion for this source frame
		// (call every frame; ClearMotion when there is none or NLS is active).
		void SetMotion(const Motion& value) { motion = value; }
		void ClearMotion() { motion = Motion(); }
		// Frames whose residual was held along the vectors (stats line, tests).
		unsigned long long StabilisedFrames() const { return stabilised; }
		// Motion-adapted blend: its state on the last blended frame, and the newest map
		// (r = factor per block, g = trusted); tests.
		MotionState LastMotionState() const { return motionState; }
		pl_tex MotionMap() const { return motionReady ? motionMap[motionIndex] : nullptr; }
		MapState LastMapState() const { return mapState; }
		// Stream robustness (owner 2026-10-14): a discontinuity - the map no
		// longer follows the content and starts over (consumers see NoField).
		// C5: the next frame's cut is read from its own scene group only (no scan of the frames since the last advance: vector-validity
		// rule, no history across a discontinuity).
		void ForgetMap();   // gate B-S2: the next frame asks the cut again
		// The newest map (r = grain scale per block, g = trusted); tests.
		pl_tex GrainMap() const { return mapReady ? map[mapIndex] : nullptr; }
		bool hostReadableMap = false;   // tests: the map can be downloaded
		const pl_hook* Measure() const { return &measure; }
		const pl_hook* Blend() const { return &blend; }
		// True when the last measured frame downscaled (the blend ran on it).
		bool Active() const { return active; }
		// The newest finished statistics (read back a few frames late).
		Stats LastStats() const { return stats; }
		// Bug hunt 34 item 1: true while the statistics are read back without waiting (the
		// D3D11 staging ring); false on another API (libplacebo's poll works there) or when the
		// ring could not be made on D3D11 (then libplacebo's readback, which waits there).
		bool NativeStatsReadback() const;
		uint64_t StatsDropped() const { return statsSchedule.Dropped(); }
		unsigned long long Failures() const { return failures; }
		// CB-18 S3: where the last measure's edge map came from, when it was not the skeleton memo (a visible status line):
		// "edge map unavailable (<cause>)" or "edge map computed in the module (no skeleton memo)"; empty when the memo served it.
		const std::string& EdgeMapStatus() const { return edgeMapStatus; }
		// Frees every texture and shader object while the device exists.
		void Release();

	private:
		static pl_hook_res RunMeasure(void* priv, const pl_hook_params* params);
		static pl_hook_res RunBlend(void* priv, const pl_hook_params* params);
		pl_hook_res Measuring(const pl_hook_params* params);
		pl_hook_res Blending(const pl_hook_params* params);
		void PollStats();
		bool ResolveCut();
		bool UpdateMap(pl_dispatch dispatch, bool advance);
		bool UpdateMotionMap(pl_dispatch dispatch, bool advance);

		pl_hook measure{};
		pl_hook blend{};
		Settings settings{};
		uint64_t sequence = 0;
		uint64_t advancedSequence = 0;
		CutSource cutSource;
		// C5: true after an advance with no discontinuity since: the next cut ask covers the frames since advancedSequence.
		bool cutContinuous = false;
		// Audit x5dup S3: the verdict asked for (sequence, previous), reused when the frame did not advance after it (a failed
		// final dispatch): not asked or counted twice.
		bool cutResolved = false;
		uint64_t cutResolvedSequence = 0;
		uint64_t cutResolvedPrevious = 0;
		bool cutResolvedValue = false;
		bool cutAvailable = true;
		bool halfFloatPicture = false;      // see HalfFloatPictureFallback()
		bool pictureWas32 = true;           // the last frame's float32Picture: a switch off recreates the picture textures once
		std::string cutCause;
		unsigned long long cutUnavailableFrames = 0;
		unsigned long long cutFadeFrames = 0;
		unsigned long long cutLateSnaps = 0;
		std::string aplCause;
		float lastApl = -1.0f;
		float frameSeconds = 1.0f / 24.0f;
		bool sourceDownscales = true;
		pl_gpu gpu = nullptr;
		// The faithful branch is a ScalerPass (B5a, refs/123 N1): `faithful` aliases that unit's slot-0 texture (owned and
		// released by faithfulScaler, never destroyed here); its LUT objects live in the unit too.
		pl_tex faithful = nullptr;
		ScalerPass::Module faithfulScaler;
		pl_tex lowres = nullptr;          // 8b: a doubler's output at source resolution
		Upscale upscale{};
		int upscaleRatio = 0;
		GrainLevelSource grainLevel{};
		bool publishedGrainUsed = false;
		bool publishedGrainDomainRefused = false;   // M2: kP10gGrainLevel refused on linear/HDR measure domain (alias-safe lean)
		bool motionFloorUsed = false;
		std::string motionFloorCause;
		pl_tex edge = nullptr;
		pl_tex partial = nullptr;
		pl_tex state = nullptr;
		pl_tex map[2] = { nullptr, nullptr };
		int mapIndex = 0;
		bool mapReady = false;          // map[mapIndex] belongs to `sequence`
		uint64_t mapSequence = 0; float mapBlockPx = 16.0f;
		pl_rect2df mapSourceCrop{};     // S4-H1: source crop cached with map[mapIndex] (survives ClearGrainField on repeats)
		NlsMaskMapping::Mapping mapNls{};
		MapState mapState = MapState::Off;
		GrainField grainField{};
		GrainSource grainSource{};
		bool grainFromShared = false;      // the last scalar pass bound the shared products (AD_P10)
		bool grainDomainRefused = false;   // ... a valid source was refused: the measure domain is not the gamma code domain
		Motion motion{};
		pl_tex residual[2] = { nullptr, nullptr };   // luma residual: this source frame's and the previous one's
		int residualIndex = 0, residualW = 0, residualH = 0;   // residual[residualIndex] = residualSequence's; size (audit 2026-10-16)
		bool residualPrevValid = false, residualCropValid = false;
		uint64_t residualSequence = 0;
		pl_rect2df lastResidualCrop = pl_rect2df();    // GEM-070: source crop cached with residual[residualIndex]
		unsigned long long stabilised = 0;
		pl_tex motionMap[2] = { nullptr, nullptr };   // motion-adapted blend: factor per block
		int motionIndex = 0;
		bool motionReady = false;                      // motionMap[motionIndex] belongs to motionSequence
		uint64_t motionSequence = 0; float motionBlockPx = 16.0f;
		pl_rect2df motionSourceCrop{};                 // S4-H1: source crop cached with motionMap[motionIndex] (survives ClearMotion on repeats)
		NlsMaskMapping::Mapping motionNls{};
		MotionState motionState = MotionState::Off;
		pl_buf readback = nullptr;
		bool readbackPending = false;
		// Bug hunt 34 item 1 (2026-10-31): on D3D11 pl_buf_poll never says "busy" and pl_buf_read
		// blocks, so the statistics go through a native staging ring instead (NativeReadback.h),
		// scheduled by ReadbackRing; read a few frames late (the value is smoothed anyway).
		static constexpr int kStatsSlots = 3;
		std::unique_ptr<LedDrive::NativeTexReadback> statsNative;
		bool statsNativeTried = false;
		LedDrive::ReadbackRing statsSchedule{ kStatsSlots, 8 };
		uint64_t statsFrames = 0;
		// Whether the scalar pass whose statistics a slot (native ring) / the buffer holds bound the shared grain products
		// (gate cb-u1 r2 NIT: LastGrainSourceUse Pending until such a frame is read back).
		bool statsSlotBound[kStatsSlots] = {};
		bool readbackBound = false;
		bool active = false;
		pl_rect2df srcRect{};
		pl_rect2d dstRect{};
		float edgeOrigin[2] = {};
		Stats stats{};
		MeasureDomain domain{};   // the last measure's (the blend uses its weights)
		unsigned long long failures = 0;
		// CB-18 S3: the measure reads the edge map (P1 kEdgeMap, stage variant) instead of its own Sobel taps.
		EdgeMapStageOrdinal edgeOrdinal{};   // the picture ordinal of this frame's measures (EDGEMAP-EXTRAS-KEY)
		pl_tex edgeLocal = nullptr;          // no skeleton memo (Original pathway, test hosts): the module dispatches the shared fill
		std::string edgeMapStatus;           // the status line of the edge map's source (empty: served by the memo)
	};
}
