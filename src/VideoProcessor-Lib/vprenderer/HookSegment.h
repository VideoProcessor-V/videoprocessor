#pragma once

#include "HookSegmentPool.h"

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// DRAFT (2026-10-24, not integrated): runs a contiguous run of hooks as ONE libplacebo hook.
//
// A HookSegment replaces a run of adjacent hooks at one stage with a single hook whose callback
// calls the original hooks in order, handing each one's output texture directly to the next
// (from its own texture pool). libplacebo then sees one entry and one exit for the whole run.
//
// What it does and does not save (audit 2026-10-25, M2): when every hook takes and returns a
// texture, libplacebo itself chains them without an extra pass (a texture result is the next
// hook's input as it is), so merging such hooks removes NO GPU pass and adds a second set of
// intermediates. The merge is the structure for later work: a run whose stages can share one
// shader (shader-returning stages chained into one dispatch) is where passes disappear. Do not
// claim a saving without measuring it.
//
// What it does NOT do: change any hook's maths. Each inner hook gets the pl_hook_params it would
// get from libplacebo (gpu, dispatch, stage, rect, colour, representation, source and destination
// rectangles) with the previous hook's output as its input texture. Intermediates use the format
// of the texture libplacebo handed in (its FBO format), so hand-offs round exactly as before.
//
// Rules:
// - Set every hook parameter BEFORE Fuse(): the merged hook does not export the inner hooks'
//   parameters, and code that finds hooks by pointer must use the unmerged list.
// - A hook that runs at more than one stage is never merged. A hook libplacebo disabled is left in
//   place (libplacebo skips it) and does not split a run.
// - An inner hook that fails inside a merged hook is reported through TakeFailures() and leaves
//   the merge from the next Fuse() on: it is passed to libplacebo again AT ITS OWN PLACE (it splits
//   the run), so the order never changes and the renderer's own failure handling sees it.
//   ClearFailed() forgets those hooks (on a settings change).
// - If libplacebo disables a merged hook (any hook error), DisabledRuns() names its inner hooks so
//   the caller can fall back visibly. Merged signatures are salted per ClearFailed() generation.
// - Optional per-hook GPU timing (shader-returning stages only), 1 in N runs, polled without
//   waiting. Texture-returning hooks dispatch inside themselves; time them through the renderer's
//   pass information (pl_render_params::info_callback).
//
// Self-contained (libplacebo only); no dependency on VP's own modules.
namespace HookSegment
{
	struct Failure
	{
		const pl_hook* hook = nullptr;   // the inner hook (valid while the caller's hooks live)
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

		// Timing: 0 = off; N = time one run in N.
		void SetSampleEvery(uint32_t n) { sampleEvery = n; }

		// Copies `in` to `out`, replacing each run of two or more `member` hooks at one stage
		// with one merged hook (placed where the run's first hook was). `errors` (optional) lists
		// the hooks libplacebo disabled; they are left as they are. The merged hooks stay valid
		// until the next Fuse() call or Release().
		void Fuse(const std::vector<const pl_hook*>& in, std::vector<const pl_hook*>& out,
			const std::function<bool(const pl_hook*)>& member, const pl_render_errors* errors = nullptr);

		// Inner hooks that failed since the last call. TakeFailures() clears the list.
		std::vector<Failure> TakeFailures();
		// Marks an inner hook failed (the runner does this itself; public for tests and for callers
		// that learn of a failure elsewhere).
		void MarkFailed(const pl_hook* hook, const std::string& reason);
		bool Failed(const pl_hook* hook) const;
		// Forgets the failed hooks and starts a new signature generation (on a settings change).
		void ClearFailed();
		// The inner hooks of each merged hook (of the last Fuse) that libplacebo has disabled.
		std::vector<std::vector<const pl_hook*>> DisabledRuns(const pl_render_errors* errors) const;
		// The timer results that arrived since the last call (inner hook, ns).
		std::vector<std::pair<const pl_hook*, uint64_t>> TakeTimings();
		// The last polled GPU time of an inner hook in nanoseconds (0 = none yet).
		uint64_t LastNs(const pl_hook* inner) const;
		// Merged hooks handed out by the last Fuse (tests and diagnostics).
		size_t MergedCount() const { return used; }
		// Frees textures and timers (before the GPU goes away).
		void Release();

	private:
		struct Fused;
		static pl_hook_res Run(void* priv, const pl_hook_params* params);
		static void Reset(void* priv);
		static pl_tex GetTex(void* priv, int width, int height);
		pl_hook_res RunFused(Fused& f, const pl_hook_params* params);
		pl_fmt FormatFor(pl_tex input) const;

		pl_gpu gpu = nullptr;
		uint32_t sampleEvery = 0;
		uint64_t generation = 0;
		std::vector<std::unique_ptr<Fused>> fused;
		size_t used = 0;
		std::set<uint64_t> failedSignatures;
		std::vector<Failure> failures;
		std::vector<std::pair<const pl_hook*, uint64_t>> lastNs;
		std::vector<std::pair<const pl_hook*, uint64_t>> fresh;
		// Reused per Fuse (no per-frame allocation once sized; audit L3).
		std::vector<uint32_t> stageBits;
		std::vector<bool> memberFlags, transparentFlags;
		std::vector<int> runs;
		std::vector<Fused*> byRun;
	};
}
