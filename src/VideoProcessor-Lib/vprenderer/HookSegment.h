#pragma once

#include "HookSegmentPool.h"

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

// DRAFT (2026-10-24, not integrated): runs a contiguous run of hooks as ONE libplacebo hook.
//
// Every hook handed to pl_render_image at PRE_OUTPUT / OUTPUT is a pass boundary: libplacebo
// finishes its picture into an FBO, calls the hook, and continues from the hook's result.
// Ten hooks in a row are ten boundaries with libplacebo-owned intermediates. A HookSegment
// replaces a run of adjacent hooks at one stage with a single hook whose callback calls the
// original hooks in order, handing each one's output texture directly to the next (from its
// own texture pool). libplacebo then sees one entry and one exit for the whole run.
//
// What it does NOT do: change any hook's maths. Each inner hook gets the same pl_hook_params it
// would get from libplacebo (gpu, dispatch, stage, rect, colour, representation, source and
// destination rectangles) with the previous hook's output as its input texture. The output is
// therefore identical up to the intermediate format: the pool uses the format libplacebo's
// FBOs use (rgba32f with fbo_float32, else rgba16f), so it is byte-identical in practice, but
// pass boundaries changed, so callers gate it with a tolerance test.
//
// Rules:
// - Set every hook parameter BEFORE Fuse(): the merged hook does not export the inner
//   hooks' parameters, and code that finds hooks by pointer must use the unmerged list.
// - A hook that runs at more than one stage, or that libplacebo has disabled, is never merged.
// - An inner hook that fails is skipped for the rest of the session (as libplacebo disables a
//   failed hook's signature) and reported through Failures(); the run continues.
// - Optional per-hook GPU timing: 1 in N runs, polled without waiting.
//
// Self-contained (libplacebo only); no dependency on VP's own modules.
namespace HookSegment
{
	struct Failure
	{
		uint64_t signature = 0;
		std::string reason;
	};

	class Runner
	{
	public:
		explicit Runner(pl_gpu gpu);
		~Runner();
		Runner(const Runner&) = delete;
		Runner& operator=(const Runner&) = delete;

		// Intermediate format: rgba32f (true, matches fbo_float32) or rgba16f.
		void SetFloat32(bool on) { float32 = on; }
		// Timing: 0 = off; N = time one run in N.
		void SetSampleEvery(uint32_t n) { sampleEvery = n; }

		// Copies `in` to `out`, replacing each run of two or more `member` hooks at one stage
		// with one merged hook (placed where the run's first hook was). `errors` (optional) lists
		// the hooks libplacebo disabled; they are left as they are. The merged hooks stay valid
		// until the next Fuse() call or Release().
		void Fuse(const std::vector<const pl_hook*>& in, std::vector<const pl_hook*>& out,
			const std::function<bool(const pl_hook*)>& member, const pl_render_errors* errors = nullptr);

		// Inner hooks that failed (skipped from then on). TakeFailures() clears the list.
		std::vector<Failure> TakeFailures();
		// The last polled GPU time of an inner hook in nanoseconds (0 = none yet).
		uint64_t LastNs(const pl_hook* inner) const;
		// Frees textures and timers (before the GPU goes away).
		void Release();

	private:
		struct Fused;
		static pl_hook_res Run(void* priv, const pl_hook_params* params);
		static void Reset(void* priv);
		static pl_tex GetTex(void* priv, int width, int height);
		pl_hook_res RunFused(Fused& f, const pl_hook_params* params);
		pl_fmt Format() const;

		pl_gpu gpu = nullptr;
		bool float32 = true;
		uint32_t sampleEvery = 0;
		std::vector<std::unique_ptr<Fused>> fused;
		size_t used = 0;
		std::set<uint64_t> disabled;
		std::vector<Failure> failures;
		std::vector<std::pair<const pl_hook*, uint64_t>> lastNs;
	};
}
