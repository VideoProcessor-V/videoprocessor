#pragma once

#include <libplacebo/renderer.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Hunt 3 F1 (the source-resolution crossfade, source_resolution_latch_fixes): the two paths of
// one frame (the re-base being left and the one being entered) mixed in the OUTPUT SIGNAL
// domain by VP's own PRE_OUTPUT hooks, not by libplacebo's frame mixer.
//
// libplacebo's pl_render_image_mix linearises every frame with a clamp at 0 before it mixes
// (BTB lost on every crossfade refresh), and the extended-range late restore (calibration LUT,
// wide source gamut) runs once after the mix with the last-rendered frame's values. Here:
//  1. the path being left is rendered by pl_render_image into a scratch target with only the
//     picture-stage hooks (everything before PRE_OUTPUT - the same hooks libplacebo's mixer
//     runs per frame - plus the extended-range late restore, the first PRE_OUTPUT hook) and a
//     capture hook after them that keeps its PRE_OUTPUT picture (output colour space, signal
//     domain, BTB/WTW carried, calibration LUT applied) in VP's rgba32f texture;
//  2. the path being entered is rendered as usual with the mix hook right after the late
//     restore (before the LED hooks, the de-judder's rendered-picture cache and every OUTPUT
//     hook: they all see the one mixed picture): out = from * (1 - w) + to * w per channel.
// Signal-domain mixing is exact at both ends (w = 0: the old path's values, w = 1: the new
// path's), monotonic, and an affine mix of two pictures that carry the same BTB/WTW values
// carries them exactly.
namespace CrossfadeMix
{
	// The mix (the shader computes the same in float): from * (1 - w) + to * w, w clamped to 0..1.
	double MixValue(double from, double to, double weight);

	// The hooks for the path being left: every hook whose stages lie before PRE_OUTPUT, plus
	// `lateRestore` (may be null) when the list holds it; order kept.
	std::vector<const pl_hook*> FromPathHooks(const pl_hook* const* hooks, int count, const pl_hook* lateRestore);

	// Where the mix hook goes: right after `lateRestore` when the list holds it, else first.
	size_t MixInsertIndex(const std::vector<const pl_hook*>& hooks, const pl_hook* lateRestore);

	class Mixer
	{
	public:
		Mixer();
		Mixer(const Mixer&) = delete;
		Mixer& operator=(const Mixer&) = delete;
		~Mixer();

		// Renders `from` (the path being left) to a scratch target shaped as `destination` and
		// keeps its PRE_OUTPUT picture. params: the frame's parameters (their hooks are filtered
		// with FromPathHooks; the capture hook is appended). False on any failure.
		bool RenderFrom(pl_renderer renderer, pl_gpu gpu, const pl_frame& from, const pl_frame& destination,
			const pl_render_params& params, const pl_hook* lateRestore);
		// The mix hook for the next render (the new path's weight w); valid after RenderFrom.
		const pl_hook* MixHook(double weight);
		// `hooks` with the mix hook inserted at MixInsertIndex.
		std::vector<const pl_hook*> WithMix(const pl_hook* const* hooks, int count, const pl_hook* lateRestore, double weight);
		bool Captured() const { return captured != nullptr && capturedValid; }
		unsigned long long Mixed() const { return mixed; }
		unsigned long long Failures() const { return failures; }
		void Release(pl_gpu gpu);

	private:
		static pl_hook_res RunCapture(void* priv, const pl_hook_params* params);
		static pl_hook_res RunMix(void* priv, const pl_hook_params* params);
		bool EnsureFloat(pl_gpu gpu, pl_tex& tex, int w, int h);

		pl_hook capture{};
		pl_hook mix{};
		pl_gpu owner = nullptr;
		pl_tex scratch = nullptr;     // the path being left's render target (its output is not used)
		pl_tex captured = nullptr;    // its PRE_OUTPUT picture (rgba32f)
		pl_tex mixedTex = nullptr;    // the mix hook's output (rgba32f)
		pl_rect2df capturedRect{};
		bool capturedValid = false;
		float weight = 1.0f;
		unsigned long long mixed = 0, failures = 0;
	};
}
