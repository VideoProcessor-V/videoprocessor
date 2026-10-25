#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/shaders.h>

#include <VP-V/modules/scaling/geometry/NlsMaskMapping.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
//   one-pole smoothing (grain_smooth_ms) and a snap on a cut (the frame's mean
//   luma jumping by more than 20 codes). No read-back on the render path.
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
	};

	inline bool operator==(const Settings& a, const Settings& b)
	{
		return a.wBase == b.wBase && a.edgeLo == b.edgeLo && a.edgeHi == b.edgeHi && a.grainLo == b.grainLo &&
			a.grainHi == b.grainHi && a.grainFloor == b.grainFloor && a.grainSmoothMs == b.grainSmoothMs &&
			a.shadowBias == b.shadowBias && a.chromaSsim == b.chromaSsim && a.antiring == b.antiring &&
			a.grainMap == b.grainMap && a.grainMapSmoothMs == b.grainMapSmoothMs && a.faithfulOrtho == b.faithfulOrtho && a.faithfulAuto == b.faithfulAuto &&
			a.cnnResidualStabilise == b.cnnResidualStabilise && a.ssimResidualStabilise == b.ssimResidualStabilise &&
			a.residualGrainGuard == b.residualGrainGuard &&
			a.float32Picture == b.float32Picture;
	}

	// 8-3: the de-judder's field for the source frame being rendered.
	struct GrainField
	{
		pl_tex error = nullptr;        // MotionWarp::GrainError()
		pl_tex field = nullptr;        // MotionWarp::Field()
		pl_tex frameTrust = nullptr;   // MotionWarp::FrameTrust()
		float minTrustedShare = 0.25f;
		// The source crop libplacebo scales (the block grid is in source
		// pixels of the whole frame) and, with NLS, the NLS map over it.
		pl_rect2df sourceCrop{};
		NlsMaskMapping::Mapping nls{};
	};

	// Stage 12 §2c / S8-1: the de-judder's motion for the source frame being
	// rendered (from the last estimate; null = none: this frame's residual).
	struct Motion
	{
		pl_tex field = nullptr;        // MotionWarp::Field(): vx, vy (source px), trust, error per 16x16 block
		pl_tex frameTrust = nullptr;   // MotionWarp::FrameTrust(): r = the trusted share
		float minTrustedShare = 0.25f;
		pl_rect2df sourceCrop{};       // the source crop libplacebo scales, in the field's source pixels
	};

	// Stage 12 §2c / delta 15 S8-2 (owner 2026-10-15): the weight scale for a
	// low bitrate quality - 1 at 0.8 and above (today's output), falling to
	// `floor` at 0.2 and below, in steps of 0.05 (no churn as the reading
	// moves). SSIM and the CNN sharpen blocking and mosquito noise as if they
	// were texture.
	// Post-merge audit 2026-10-17 (general, not fixed): the residual grain guard's
	// code -> linear-light conversion follows the source's own transfer, as
	// pl_shader_linearize decodes it, instead of an assumed 2.2. mode 0: power,
	// a (c + b)^g (gamma 1.8-2.8 and unknown scaled by the nominal peak; BT.1886 with
	// its black lift; sRGB and the log curves as gamma 2.2, their nearest power);
	// 1: PQ (1.0 = SDR white, 203 nits); 2: HLG - a = nominal peak, b = black lift,
	// g = system gamma, the OOTF applied on luma.
	struct GuardTransferParams
	{
		float mode = 0.0f;
		float a = 1.0f;
		float b = 0.0f;
		float g = 2.2f;
	};
	GuardTransferParams GuardTransfer(const pl_color_space& source);
	// CPU mirrors of the shader's rs_lin / rs_code / rs_slope (tests).
	inline double GuardLinear(const GuardTransferParams& t, double c)
	{
		c = c > 0.0 ? c : 0.0;
		if (t.mode < 0.5)
			return t.a * std::pow(c + t.b, static_cast<double>(t.g));
		if (t.mode < 1.5)
		{
			const double e = std::pow(c, 1.0 / 78.84375);
			const double n = (std::max)(e - 0.8359375, 0.0) / (18.8515625 - 18.6875 * e);
			return std::pow(n, 1.0 / 0.1593017578125) * (10000.0 / 203.0);
		}
		const double x = (1.0 - t.b) * c + t.b;
		const double s = (x <= 0.5 ? 4.0 * x * x : std::exp((x - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
		return t.a * std::pow((std::max)(s, 0.0), static_cast<double>(t.g));
	}
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
		float meanLuma = 0.0f;  // the frame's mean luma (8-bit codes; the cut detector)
	};

	// Stage 8b (docs/VP-stage8b-upscale-blend.md): the same module upscaling a
	// shorter source (1080p -> 1600p). Luma only, grain only (no edge term):
	//     w = up_w_base x grain_scale;  out = cnn + (1 - w)(Y_faithful - Y_cnn)
	// so a clean picture is the CNN branch exactly and chroma stays the CNN
	// branch's (CfL). The faithful branch is libplacebo's ewa_lanczossharp
	// (anti-ringing) from the source-resolution picture: PRE_KERNEL itself,
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
	};

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
		// The SSIM shader's condition for this frame: the source crop is
		// taller than the destination rectangle.
		void SetSourceDownscales(bool value) { sourceDownscales = value; }
		// Stage 8b: per frame, after SetFrame (off: the downscale module).
		void SetUpscale(const Upscale& value) { upscale = value; }
		// The last measured frame's source-resolution base: 1 (PRE_KERNEL),
		// 2 (a doubler's output averaged), 0 not applied; tests and the log.
		int LastUpscaleRatio() const { return upscaleRatio; }
		// 8-3: after SetFrame, the field for this source frame (none: the
		// scalar). Call every frame; the textures are the matcher's.
		void SetGrainField(const GrainField& field) { grainField = field; }
		void ClearGrainField() { grainField = GrainField(); }
		// Stage 12 §2c / S8-1: after SetFrame, the motion for this source frame
		// (call every frame; ClearMotion when there is none or NLS is active).
		void SetMotion(const Motion& value) { motion = value; }
		void ClearMotion() { motion = Motion(); }
		// Frames whose residual was held along the vectors (stats line, tests).
		unsigned long long StabilisedFrames() const { return stabilised; }
		MapState LastMapState() const { return mapState; }
		// Stream robustness (owner 2026-10-14): a discontinuity - the map no
		// longer follows the content and starts over (consumers see NoField).
		void ForgetMap() { mapReady = false; mapSequence = 0; mapState = MapState::NoField; residualSequence = 0; residualPrevValid = false; }
		// The newest map (r = grain scale per block, g = trusted); tests.
		pl_tex GrainMap() const { return mapReady ? map[mapIndex] : nullptr; }
		bool hostReadableMap = false;   // tests: the map can be downloaded
		const pl_hook* Measure() const { return &measure; }
		const pl_hook* Blend() const { return &blend; }
		// True when the last measured frame downscaled (the blend ran on it).
		bool Active() const { return active; }
		// The newest finished statistics (read back a few frames late).
		Stats LastStats() const { return stats; }
		unsigned long long Failures() const { return failures; }
		// Frees every texture and shader object while the device exists.
		void Release();

	private:
		static pl_hook_res RunMeasure(void* priv, const pl_hook_params* params);
		static pl_hook_res RunBlend(void* priv, const pl_hook_params* params);
		pl_hook_res Measuring(const pl_hook_params* params);
		pl_hook_res Blending(const pl_hook_params* params);
		void PollStats();
		bool UpdateMap(pl_dispatch dispatch, bool advance);

		pl_hook measure{};
		pl_hook blend{};
		Settings settings{};
		uint64_t sequence = 0;
		uint64_t advancedSequence = 0;
		float frameSeconds = 1.0f / 24.0f;
		bool sourceDownscales = true;
		pl_gpu gpu = nullptr;
		pl_tex faithful = nullptr;
		pl_tex faithfulMid = nullptr;   // the ortho branch's vertical pass
		pl_tex lowres = nullptr;          // 8b: a doubler's output at source resolution
		Upscale upscale{};
		int upscaleRatio = 0;
		pl_tex edge = nullptr;
		pl_tex partial = nullptr;
		pl_tex state = nullptr;
		pl_tex map[2] = { nullptr, nullptr };
		int mapIndex = 0;
		bool mapReady = false;          // map[mapIndex] belongs to `sequence`
		uint64_t mapSequence = 0;
		MapState mapState = MapState::Off;
		GrainField grainField{};
		Motion motion{};
		pl_tex residual[2] = { nullptr, nullptr };   // luma residual: this source frame's and the previous one's
		int residualIndex = 0;                        // residual[residualIndex] = residualSequence's
		bool residualPrevValid = false;
		uint64_t residualSequence = 0;
		int residualW = 0, residualH = 0;              // the residual textures' size (audit 2026-10-16)
		unsigned long long stabilised = 0;
		pl_buf readback = nullptr;
		bool readbackPending = false;
		pl_shader_obj polarLut = nullptr;
		pl_shader_obj orthoLut[2] = { nullptr, nullptr };
		bool active = false;
		pl_rect2df srcRect{};
		pl_rect2d dstRect{};
		float edgeOrigin[2] = {};
		Stats stats{};
		unsigned long long failures = 0;
	};
}
