#pragma once

#include <libplacebo/renderer.h>

#include <cstdint>
#include <vector>

// Stage 10 (10a): BTB/WTW preserved through libplacebo for SDR sources
// (`preserve_extended_range`, docs/VP-btb-wtw-answers.md).
//
// libplacebo clips values outside 0..1 when it linearises (every SDR
// downscale, every transfer or primaries conversion) and when it
// sigmoidises (every SDR upscale). Two native SIG_COLOR hooks avoid both
// without touching libplacebo. libplacebo is told the source is already in
// the output's colour space, so its own (clamping) conversion is a no-op
// (it always converts from the frame's declared colour space, whatever a
// hook reports), and the render target is not touched (overlays and
// subtitles convert as before).
//
// - Carry, at PL_HOOK_RGB (ahead of NLS): first the source gamma becomes
//   the output gamma, sign-preserving (|v|^(source/output)); libplacebo
//   then linearises with the output gamma and gets exactly the linear light
//   it uses today, so linear-light and sigmoid scaling are unchanged. Then a
//   pixel outside 0..1 is shifted into range by its offset o (the lowest
//   channel if negative, else the highest channel minus 1) and o goes into
//   alpha as 1 + o, which libplacebo never linearises or clamps (in-range
//   pixels carry exactly 1, so premultiplied user-shader input is unchanged).
// - Restore, at PL_HOOK_SCALED (before the LED measurement, the LED
//   compensation and the PRE_OUTPUT texture hooks): undoes libplacebo's
//   linear-light state with its own delinearize, adds o back, drops alpha
//   and applies libplacebo's primaries matrix with sign-preserving
//   transfers (BT.709 -> the output's primaries only, which contain BT.709,
//   so nothing leaves the gamut). The matrix is linear, so applying it after
//   scaling instead of before is the same picture.
//
// Alpha ownership: the carry owns alpha from PL_HOOK_RGB to PL_HOOK_SCALED
// and drops it there. The occlusion mask owns alpha from the native OUTPUT
// hook to the sigma-delta. They never overlap.
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
		// afterTextureHooks: another hook's texture output precedes the restore
		// (the SSIM downscaler, the Stage 8 blend); see RunRestoreTexture.
		const pl_hook* Restore(bool afterTextureHooks = false) const
		{
			return afterTextureHooks ? &restoreTexture : &restore;
		}
		bool Owns(uint64_t signature) const
		{
			return signature == carry.signature || signature == restore.signature ||
				signature == restoreTexture.signature;
		}
		unsigned long long Applied() const { return applied; }
		// The restore's own texture: before the GPU goes.
		void Release();

	private:
		static pl_hook_res RunCarry(void* priv, const pl_hook_params* params);
		static pl_hook_res RunRestore(void* priv, const pl_hook_params* params);
		static pl_hook_res RunRestoreTexture(void* priv, const pl_hook_params* params);
		bool AppendRestore(pl_shader sh, pl_color_space& color) const;

		pl_hook carry{};
		pl_hook restore{};
		pl_hook restoreTexture{};
		Decision decision{};
		pl_color_space output{};
		float gammaRatio = 1.0f;
		unsigned long long applied = 0;
		pl_gpu restoreGpu = nullptr;
		pl_tex restoreTex = nullptr;
	};

	// For an active decision: the source colour space becomes the output's
	// (libplacebo converts nothing) and the two hooks go first in the list,
	// so each runs ahead of any other hook of its stage (the carry before
	// NLS). Nothing happens for an inactive decision.
	void Prepare(const Decision& decision, Hooks& hooks, pl_color_space& sourceColor,
		const pl_color_space& output, std::vector<const pl_hook*>& list);
}
