#pragma once

#include <libplacebo/renderer.h>

#include <cstdint>
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
// The switch turns itself off (logged) for HDR, a calibration LUT, a
// transfer that is not a pure power, an SDR source in primaries other than
// BT.709 (unless equal to the output's), or output primaries that do not
// contain BT.709.
namespace ExtendedRange
{
	enum class Reason
	{
		Active,
		Off,              // the switch is off
		HdrSource,
		HdrOutput,
		CalibrationLut,
		SourceTransfer,   // not a pure power (BT.1886, sRGB, unknown)
		OutputTransfer,
		SourcePrimaries,  // e.g. an SDR BT.2020 source on a P3 output
		OutputPrimaries,  // the output does not contain BT.709
		HookFailed,       // libplacebo disabled one of the hooks
	};

	const char* ToString(Reason reason);

	// The gamma of a pure-power transfer, 0 for any other.
	double PowerGamma(enum pl_color_transfer transfer);

	struct Decision
	{
		Reason reason = Reason::Off;
		bool Active() const { return reason == Reason::Active; }
		float sourceGamma = 0.0f;
		float outputGamma = 0.0f;
		// Source to output primaries (relative colorimetric), column-major
		// as GLSL's mat3 reads it.
		float matrix[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		bool convertGamma = false;      // the carry re-encodes source -> output gamma
		bool convertPrimaries = false;  // the restore applies the matrix
	};

	// source: the colour space libplacebo would decode the frame in (after
	// VP's SDR transfer override); output: the render target's.
	Decision Decide(bool enabled, const pl_color_space& source, const pl_color_space& output,
		bool calibrationLut);

	class Hooks
	{
	public:
		Hooks();
		Hooks(const Hooks&) = delete;
		Hooks& operator=(const Hooks&) = delete;

		void SetFrame(const Decision& decision, const pl_color_space& output);
		const pl_hook* Carry() const { return &carry; }
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
				signature == restoreTexture.signature;
		}
		unsigned long long Applied() const { return applied; }
		// The textures: before the GPU goes.
		void Release();

	private:
		static pl_hook_res RunCarry(void* priv, const pl_hook_params* params);
		static bool EnsureFloat(pl_gpu gpu, pl_gpu& owner, pl_tex& tex, int w, int h);
		static pl_hook_res RunRestore(void* priv, const pl_hook_params* params);
		static pl_hook_res RunRestoreTexture(void* priv, const pl_hook_params* params);
		bool AppendRestore(pl_shader sh, pl_color_space& color, const pl_rect2df& rect) const;

		pl_hook carry{};
		pl_hook restore{};
		pl_hook restoreTexture{};
		Decision decision{};
		pl_color_space output{};
		float gammaRatio = 1.0f;
		unsigned long long applied = 0;
		pl_gpu gpu = nullptr;
		pl_tex clipped = nullptr;     // the carry's output (the picture libplacebo goes on with)
		pl_tex remainder = nullptr;   // value - clipped, per channel
		pl_rect2df carryRect{};       // the picture's rectangle in both
		pl_tex restoreTex = nullptr;
	};

	// For an active decision: the source colour space becomes the output's
	// (libplacebo converts nothing); the carry goes last in the list (the
	// last hook at RGB, after NLS) and the restore first (the first at
	// SCALED). Nothing happens for an inactive decision.
	// singlePlaneRgb: the source frame is one RGB plane (the restore's form).
	void Prepare(const Decision& decision, Hooks& hooks, pl_color_space& sourceColor,
		const pl_color_space& output, std::vector<const pl_hook*>& list, bool singlePlaneRgb = false);
}
