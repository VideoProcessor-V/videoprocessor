#pragma once

#include <libplacebo/renderer.h>

#include <libplacebo/shaders/lut.h>

#include <cstdint>
#include <string>
#include <vector>

// Stage 10 (10a, 10b): BTB/WTW preserved through libplacebo for SDR
// sources (`preserve_extended_range`, docs/VP-btb-wtw-answers.md; 10b:
// docs/VP-stage10b-side-texture.md).
//
// libplacebo clips values outside 0..1 when it linearises (every SDR
// downscale, every transfer or primaries conversion) and when it
// sigmoidises (every SDR upscale). Two native hooks avoid both without
// touching libplacebo. libplacebo is told the source is already in the
// output's colour space, so its own (clamping) conversion is a no-op (it
// always converts from the frame's declared colour space, whatever a hook
// reports), and the render target is not touched (overlays and subtitles
// convert as before).
//
// - Carry, the last hook at PL_HOOK_RGB (after NLS): first the source gamma
//   becomes the output gamma, sign-preserving (|v|^(source/output)), and
//   the primaries are converted (BT.709 -> the output's, which contain
//   BT.709, with sign-preserving transfers) - before scaling, where
//   libplacebo converts without the switch; libplacebo then linearises
//   with the output gamma and gets exactly the linear light it uses today.
//   Then each pixel is split: the picture
//   clipped to 0..1 goes on to libplacebo (exactly what it gets with the
//   switch off, so every scaler and user shader - SSIM, SSimSuperRes, the
//   Stage 8 blend, prescalers - sees today's picture), and the per-channel
//   remainder (value - clipped) goes into a texture VP keeps.
// - Restore, the first hook at PL_HOOK_SCALED (before the LED measurement,
//   the LED compensation and the PRE_OUTPUT texture hooks): undoes
//   libplacebo's linear-light state with its own delinearize, adds the
//   remainder back - sampled at the output pixel's position in the carry's
//   rectangle, averaged over the pixel's footprint (positive weights only, so
//   nothing rings and pixels away from out-of-range areas get exactly 0).
//
// 10a carried one offset per pixel in alpha and shifted the pixel by it,
// and converted the primaries after scaling: on saturated colours a
// non-linear shader (SSIM, SSimSuperRes) saw other values than without the
// switch (up to 46 codes, measured). 10b leaves alpha alone and converts
// where libplacebo does.
//
// Off is today's exact path: no hooks, the source colour space untouched.
//
// Audit 2026-10-16 (owner: BTB and WTW preserved in ALL cases). The old
// eligibility (pure-power transfers, BT.709 sources, no LUT, no HDR) is
// widened by four switches in Settings, each default on; with all four off
// Decide() returns exactly the old decisions.
//
// 1. piecewiseTransfers: BT.1886 and sRGB on either side. Every transfer is
//    handled in "relative linear light" E(v) in 0..1 for v in 0..1 (the
//    black-scaled linear light libplacebo's own linearize/delinearize pair
//    uses: (L - black) / (white - black)), extended to all reals:
//    - below 0, the odd extension E(-v) = -E(v) (sign-preserving, as the
//      pure-power carry always was; C1 at 0 for BT.1886 and sRGB, whose
//      slope at 0 is finite and non-zero);
//    - above 1, the curve's own analytic form continued (power: v^g; sRGB:
//      ((v+0.055)/1.055)^2.4; BT.1886: ((v+b)^2.4 - b^2.4) / ((1+b)^2.4 -
//      b^2.4)). Each is strictly increasing and C-infinity across 1 (the same
//      formula on both sides) and has a closed-form inverse, so WTW stays
//      monotonic and invertible; a C1 linear extension would also be
//      monotonic but would bend the curve at 1 and differ from what an
//      unclamped libplacebo would compute. b is derived from libplacebo's
//      own linearize at 0 and 1 (pl_color_linearize) after
//      pl_color_space_infer_map, so the in-range part matches libplacebo's
//      conversion exactly whenever source and output black levels match
//      (libplacebo's SDR default; the pure-power carry had the same limit).
// 2. wideSourcePrimaries: SDR sources in primaries other than BT.709.
//    - Source gamut inside the output's (pl_primaries_superset, e.g. BT.601
//      EBU SDR on a BT.2020 output): the carry converts with the matrix, as
//      for BT.709 (libplacebo maps nothing for a contained gamut either).
//    - Source gamut not inside the output's (BT.2020 SDR on BT.709 / P3; also
//      Display P3 on BT.2020, whose red lies ~0.0017 outside BT.2020's
//      red-green edge, so libplacebo gamut-maps it):
//      libplacebo keeps its own gamut mapping of the clipped picture (it is
//      told output transfer + SOURCE primaries; switch-off picture for every
//      in-range pixel). The remainder is carried in relative LINEAR light
//      (E(v) - E(clip(v)), source primaries) and restored after libplacebo's
//      conversion by the late restore at PRE_OUTPUT: out = Eout^-1(Eout(c) +
//      M * remainder), M the relative-colorimetric matrix, applied linearly
//      (negative and >1 values allowed). With a calibration LUT as well the
//      pre-LUT value is not observable at PRE_OUTPUT, so the carry converts
//      the primaries itself (matrix + clip = relative-colorimetric clipping
//      instead of libplacebo's gamut mapping for out-of-gamut colours) and
//      the LUT carry below applies; logged as "gamut clip".
// 3. calibrationLut: libplacebo clamps a 3D LUT's input to 0..1
//    (shaders/lut.c). The SCALED restore still hands libplacebo the whole
//    value v (the LUT clamps it itself, exactly as with the switch off), and
//    the late restore, the first hook at PRE_OUTPUT, adds the part the LUT
//    cut as a linear extrapolation of the LUT at the clamped point c:
//    out = LUT(c) + sum_k d_k * dLUT/dk(c), d = v - c, the slope along each
//    clamped axis k estimated from the LUT's edge cell (one grid step
//    inwards, trilinear in the other two axes). v is read back from the
//    SCALED restore's own texture at the same output pixel. Only for a 3D
//    PL_LUT_NORMALIZED target LUT without shaper matrices (VP's case).
// 4. hdrPassthrough: HDR sources. libplacebo decodes limited range without
//    clamping, but its linearize clamps at 0 (BTB lost on every linear-light
//    downscale), and tone mapping clamps to the source's mastering range
//    (WTW above 940 in PQ is above 10,000 nits). The carry, in the signal
//    domain (no transfer, no matrix), is active only when libplacebo maps
//    nothing: source and output colour spaces equal (pl_color_space_equal,
//    which also means identity tone mapping). With any tone or gamut mapping
//    a signal-domain remainder has no meaning after the curve (negative PQ
//    is negative light; after a BT.2390 curve the output black and white
//    are the curve's, not the source's), so the switch stays off there and
//    the in-range picture goes through libplacebo's tone mapping as before.
//    SDR sources on an HDR output stay off (not covered).
namespace ExtendedRange
{
	enum class Reason
	{
		Active,
		Off,              // the switch is off
		HdrSource,        // HDR source tone-mapped to SDR (or hdrPassthrough off)
		HdrOutput,        // SDR source on an HDR output
		CalibrationLut,   // a LUT and calibrationLut off (or no LUT data)
		SourceTransfer,   // not a pure power (BT.1886, sRGB with the switch off; unknown)
		OutputTransfer,
		SourcePrimaries,  // e.g. an SDR BT.2020 source on a P3 output (switch off)
		OutputPrimaries,  // the output does not contain BT.709 (switch off)
		HookFailed,       // libplacebo disabled one of the hooks
		HdrMapped,        // audit 2026-10-16: HDR source and output differ (tone/gamut mapping)
		LutUnsupported,   // audit 2026-10-16: 1D LUT, shaper matrices or not PL_LUT_NORMALIZED
		TemporalMix,      // hunt 2 (08 #1): libplacebo frame mixing with a late restore (cannot follow a mix)
	};

	const char* ToString(Reason reason);

	// The gamma of a pure-power transfer, 0 for any other.
	double PowerGamma(enum pl_color_transfer transfer);

	// Audit 2026-10-16: the eligibility switches (see the top of this file).
	// All default on; all off = the old eligibility.
	struct Settings
	{
		bool piecewiseTransfers = true;   // 1: BT.1886 and sRGB
		bool wideSourcePrimaries = true;  // 2: SDR primaries other than BT.709
		bool calibrationLut = true;       // 3: carry around a 3D calibration LUT
		bool hdrPassthrough = true;       // 4: HDR when libplacebo maps nothing
		// Audit 2026-10-16 (GPU run): not a switch but the frame's state - true
		// when libplacebo's tone mapping is dynamic (peak detection on,
		// pl_render_params.peak_detect_params): equal colour spaces are then
		// still tone-mapped (measured: PQ 502 -> 371), the restore at SCALED
		// would be clamped by it, so HDR passthrough is off (HdrMapped).
		bool hdrDynamicToneMapping = false;
	};

	// Audit 2026-10-16: a transfer in relative linear light (see 1. above).
	enum class TransferKind : int
	{
		Power = 0,     // param = gamma
		Bt1886 = 1,    // param = b (black offset; 0 = pure 2.4)
		Srgb = 2,      // param unused
		Identity = 3,  // signal domain (HDR passthrough)
	};

	struct Transfer
	{
		TransferKind kind = TransferKind::Identity;
		float param = 0.0f;
	};

	// False for a transfer the carry cannot express (unknown, HDR, log, or
	// BT.1886/sRGB with piecewise off). colour: as libplacebo will see it.
	bool DescribeTransfer(const pl_color_space& colour, bool piecewise, Transfer& out);
	// Sign-preserving, analytic above 1 (odd extension below 0).
	double ToLinear(const Transfer& t, double v);
	double FromLinear(const Transfer& t, double l);
	const char* TransferName(const Transfer& t);

	// Audit 2026-10-16: CPU mirror of the late restore's LUT extrapolation
	// (the GLSL in ExtendedRange.cpp computes the same). data: RGB triplets,
	// R innermost, B outermost (pl_custom_lut's layout); each size >= 2.
	struct LutView
	{
		int size[3] = { 0, 0, 0 };
		const float* data = nullptr;
	};
	bool LutCarryable(const pl_custom_lut* lut);
	// Trilinear, input clamped to 0..1 (as libplacebo clamps).
	void LutSample(const LutView& lut, const float p[3], float out[3]);
	// The slope along axis at the in-range point c from the edge cell:
	// upper: (L(c) - L(c - step)) / step, else (L(c + step) - L(c)) / step.
	void LutEdgeSlope(const LutView& lut, const float c[3], int axis, bool upper, float out[3]);
	// L(clamp(v)) + sum over clamped axes of (v_k - c_k) * slope_k.
	void LutExtrapolate(const LutView& lut, const float v[3], float out[3]);

	struct Decision
	{
		Reason reason = Reason::Off;
		bool Active() const { return reason == Reason::Active; }
		float sourceGamma = 0.0f;   // pure-power gammas (0 for other transfers; see source/output)
		float outputGamma = 0.0f;
		// Source to output primaries (relative colorimetric), column-major
		// as GLSL's mat3 reads it.
		float matrix[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		bool convertGamma = false;      // the carry re-encodes source -> output transfer
		bool convertPrimaries = false;  // the carry applies the matrix
		// Audit 2026-10-16.
		Transfer source{};              // the carry's source transfer
		Transfer output{};              // the output transfer (carry, late restore)
		// 2 (wide source gamut, no LUT): libplacebo converts the primaries
		// (told sourcePrimaries), the remainder is linear light, restored
		// late with the matrix; no SCALED restore.
		bool libplaceboMapsPrimaries = false;
		enum pl_color_primaries sourcePrimaries = PL_COLOR_PRIM_UNKNOWN;
		bool gamutClip = false;         // 2 with a LUT: the carry clips out-of-gamut colours
		bool hdrPassthrough = false;    // 4
		const pl_custom_lut* lut = nullptr;   // 3: this frame's target LUT (late restore)
		bool LateRestore() const { return libplaceboMapsPrimaries || lut != nullptr; }
		bool ScaledRestore() const { return !libplaceboMapsPrimaries; }
	};

	// source: the colour space libplacebo would decode the frame in (after
	// VP's SDR transfer override); output: the render target's.
	// The old form: default Settings, no LUT data (a LUT keeps it off).
	Decision Decide(bool enabled, const pl_color_space& source, const pl_color_space& output,
		bool calibrationLut);
	// Audit 2026-10-16: lut = the target's LUT when attached (target.lut),
	// lutType = target.lut_type.
	Decision Decide(bool enabled, const Settings& settings, const pl_color_space& source,
		const pl_color_space& output, const pl_custom_lut* lut, enum pl_lut_type lutType);
	// Hunt 2 (08 #1): the decision for a frame drawn through the temporal
	// renderer (deinterlacing / libplacebo smooth motion). The retained frames
	// are relabelled with the prepared colour (TemporalRenderer::Render's
	// historyColor), so every frame is converted exactly once by the carry
	// and the SCALED restore, both of which run inside each frame's own
	// libplacebo pass (deinterlaced or mixed). The late restore (calibration
	// LUT, libplacebo gamut mapping) runs once after the mix and reads the
	// SCALED texture / remainder of whichever frame was rendered last, so it
	// cannot follow a mix: frameMixing with LateRestore() returns TemporalMix
	// (off: libplacebo's own clamping conversion, no double conversion).
	// Deinterlacing alone renders one frame per draw and keeps every mode.
	// Note: libplacebo's mixer linearises each frame with a clamp at 0, so a
	// mixed refresh loses BTB even when this stays active (WTW is kept);
	// VP's own motion (de-judder) is the BTB-safe path (docs/VP-btb-wtw-answers.md).
	Decision ForTemporal(const Decision& decision, bool frameMixing);
	// One log line's worth: transfers, primaries, modes.
	std::string Describe(const Decision& decision);

	// Efficiency audit 2026-10-24 #7: how the carry ran. None = one pass with
	// two outputs; the others name why the two original passes are used
	// (identical output, one extra source-size pass).
	enum class CarryPassReason { None, NoCompute, NotStorable, OnePassFailed };
	const char* CarryPassReasonText(CarryPassReason reason);
	// The one-pass body (pure text, CPU-tested): the old body up to the
	// clamp, then the remainder store and the clipped colour.
	std::string CarryOnePassBody();
	// The original carry body (both two-pass forms; for the CPU text test).
	const char* CarryBody();

	class Hooks
	{
	public:
		Hooks();
		Hooks(const Hooks&) = delete;
		Hooks& operator=(const Hooks&) = delete;

		void SetFrame(const Decision& decision, const pl_color_space& output);
		const pl_hook* Carry() const { return &carry; }
		// Audit 2026-10-16: the late restore, the first hook at PRE_OUTPUT
		// (LUT extrapolation, or the linear remainder after libplacebo's
		// gamut mapping). Renders its own float texture (an inline form is
		// rejected by libplacebo at PRE_OUTPUT, measured).
		const pl_hook* LateRestore() const { return &late; }
		// textureForm: the restore renders its own texture (after another
		// hook's texture output at PRE/POST_KERNEL, or hooks on the planes
		// before RGB, or a one-plane RGB source; see RunRestoreTexture).
		const pl_hook* Restore(bool textureForm = false) const
		{
			return textureForm ? &restoreTexture : &restore;
		}
		bool Owns(uint64_t signature) const
		{
			return signature == carry.signature || signature == restore.signature ||
				signature == restoreTexture.signature || signature == late.signature;
		}
		unsigned long long Applied() const { return applied; }
		// Efficiency audit 2026-10-24 #7 (diagnostic readout).
		CarryPassReason CarryPasses() const { return carryOnePassReason; }
		// GPU byte-identity test (#7): the two original passes on demand (A/B of the same frame).
		void ForceTwoPassCarryForTest(bool twoPass)
		{
			carryOnePassReason = twoPass ? CarryPassReason::OnePassFailed : CarryPassReason::None;
		}
		std::string CarryPassesDescribe() const;
		// The textures: before the GPU goes (and, efficiency audit 2026-10-24 #8b, after the
		// feature has been idle: FeatureTextureIdle; each is rewritten before it is read).
		void Release();
		bool HoldsTextures() const { return clipped || remainder || restoreTex || lateTex || lutTex; }

	private:
		static pl_hook_res RunCarry(void* priv, const pl_hook_params* params);
		static bool EnsureFloat(pl_gpu gpu, pl_gpu& owner, pl_tex& tex, int w, int h, bool storable = false);
		bool RunCarryOnePass(const pl_hook_params* params, const float& convertGamma, const float& convertPrimaries,
			const float& sourceKind, const float& outputKind, const float& linearRemainder);
		static pl_hook_res RunRestore(void* priv, const pl_hook_params* params);
		static pl_hook_res RunRestoreTexture(void* priv, const pl_hook_params* params);
		bool AppendRestore(pl_shader sh, pl_color_space& color, const pl_rect2df& rect) const;
		static pl_hook_res RunLate(void* priv, const pl_hook_params* params);
		bool AppendLate(pl_shader sh, const pl_rect2df& rect) const;
		int LateReady(pl_gpu gpu);
		bool EnsureLut(pl_gpu gpu);

		pl_hook carry{};
		pl_hook restore{};
		pl_hook restoreTexture{};
		pl_hook late{};   // audit 2026-10-16
		Decision decision{};
		pl_color_space output{};
		float gammaRatio = 1.0f;
		unsigned long long applied = 0;
		CarryPassReason carryOnePassReason = CarryPassReason::None;
		unsigned long long carryOnePassFrames = 0, carryTwoPassFrames = 0;
		pl_gpu gpu = nullptr;
		pl_tex clipped = nullptr;     // the carry's output (the picture libplacebo goes on with)
		pl_tex remainder = nullptr;   // value - clipped, per channel
		pl_rect2df carryRect{};       // the picture's rectangle in both
		pl_tex restoreTex = nullptr;
		// Audit 2026-10-16.
		pl_rect2df scaledRect{};      // restoreTex's picture rectangle (SCALED)
		bool scaledValid = false;     // restoreTex holds this frame's SCALED picture
		pl_tex lateTex = nullptr;     // the late restore's output
		pl_tex lutTex = nullptr;      // the target LUT as an rgba32f 3D texture
		uint64_t lutSignature = 0;
		const float* lutData = nullptr;
		int lutSize[3] = { 0, 0, 0 };
	};

	// For an active decision: the source colour space becomes the output's
	// (libplacebo converts nothing); the carry goes last in the list (the
	// last hook at RGB, after NLS) and the restore first (the first at
	// SCALED). Nothing happens for an inactive decision.
	// Audit 2026-10-16: with libplaceboMapsPrimaries the source keeps its
	// primaries (output transfer) and there is no SCALED restore; with
	// LateRestore() the late restore goes first in the list (the first hook
	// at PRE_OUTPUT, before the LED hooks and any other PRE_OUTPUT hook).
	// singlePlaneRgb: the source frame is one RGB plane (the restore's form).
	void Prepare(const Decision& decision, Hooks& hooks, pl_color_space& sourceColor,
		const pl_color_space& output, std::vector<const pl_hook*>& list, bool singlePlaneRgb = false);
}
