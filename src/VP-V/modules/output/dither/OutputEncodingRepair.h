#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <vector>

// Works around a libplacebo 7.360.1 renderer bug. Fixed in the VP Renderer
// Beta's libplacebo (master, API 377): there Needed() is always false.
//
// pass_output_target() takes the image's shader (`sh = img_sh(...)`) before
// the PRE_OUTPUT hooks run. A hook with a texture input (every mpv user
// shader, and VP's LED measurement hook) makes the renderer finish that shader
// into a texture first (img_tex). Afterwards the renderer still applies the
// output encoding - pl_shader_encode_color(sh, target repr): the range
// (full -> limited), YCbCr encoding, alpha - and a native target LUT to the
// old `sh`, which is now finished: "Attempted to modify an immutable
// shader!", the step is skipped, and the picture is drawn unencoded. With VP's
// limited-range output, black comes out at 0 instead of 16 and white at 255
// instead of 235.
//
// This hook runs first at the OUTPUT stage, only in frames whose hook chain
// has a texture-input PRE_OUTPUT hook, and applies exactly that encoding to
// the image. VP never uses the steps it does not repeat: an alpha/background
// blend (only for 4-component images; VP's are 3), a native target LUT (VP's
// is normalized, applied earlier in colour management) or rotation.
namespace OutputEncodingRepair
{
	// True when the chain contains a hook that triggers the bug: one that runs
	// at PRE_OUTPUT with a texture input and that libplacebo has not disabled
	// (a disabled hook is skipped before the shader is finished, so it does
	// not trigger the bug - repairing then would encode twice). False when
	// the target LUT is a CONVERSION LUT: libplacebo does not encode then.
	bool Needed(const std::vector<const pl_hook*>& hooks,
		const pl_render_errors* errors = nullptr, enum pl_lut_type targetLut = PL_LUT_UNKNOWN);

	class Hook
	{
	public:
		Hook();
		Hook(const Hook&) = delete;
		Hook& operator=(const Hook&) = delete;

		// The target's representation for this frame (before normalisation,
		// as passed to pl_render_image).
		void SetTarget(const pl_color_repr& targetRepr) { target = targetRepr; }
		const pl_hook* Get() const { return &hook; }
		unsigned long long Applied() const { return applied; }

	private:
		static pl_hook_res Run(void* priv, const pl_hook_params* params);
		pl_hook_res Repair(const pl_hook_params* params);

		pl_hook hook{};
		pl_color_repr target{};
		unsigned long long applied = 0;
	};
}
