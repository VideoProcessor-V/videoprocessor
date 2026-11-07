// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// TemporalDetail on the GPU (repair.temporal_detail): the CPU reference model TemporalDetailModel.h as libplacebo
// compute passes (TemporalDetailShaders.h), luma plane only, chroma passes through. No renderer call sites yet (they
// come after the Bill merge; the module README lists them).
//
// Implemented on the GPU: the shared grain split (repair.grain_split, GrainSplitGpu), block measures, composed
// motion and uncertainty (backward vectors, forward/backward disagreement when forward vectors are given), the
// per-(slot, block) validation, trackers / refresh / admission / eviction / crossfade / limiter / retire / boost,
// the multi-frame DCT-patch template cleaner (admission frames only), the template with its per-block projections,
// the grain budget and amplitude limit, the output-vs-input grain guard (tile sums, EMA, trip / release, add-back),
// recompose with the current grain added back exactly (Y_out = Y + delta; a pixel with nothing to add is copied).
// Not on the GPU yet (the model has them; visible in Status()): the queued-mode future slot / referee (no queue
// frames in this API: referee = 1) and the camera-model homography chain (not compiled until feat/camera-model
// merges; the model then uses the composed field, as here). Model-only measurement knobs Tuning::forceDrop /
// forceFreeze are not mirrored.
//
// Off: Apply returns the source frame itself (byte-identical) and no pass runs. Deterministic: fixed reduction
// orders, no atomics. Status counters are read on request (ReadCounters: a small blocking download, for
// diagnostics and tests; the render path never needs it).
//
// Memory (SA6): the per-frame temporaries (the cleaner's two accumulators and the transferred delta) can come from the
// shared scratch arena (Params::arena; null = private textures, byte-identical to before). The module's own grain
// split (the visible fallback when no shared product fits) takes its intermediates from the same arena and its private
// output is freed once the shared product has fitted for kSplitReleaseSeconds of content. An arena shortage is never
// silent: Apply fails for that frame (ScratchShort(), LastError "scratch memory short (...)") and the arena's own
// FrameDecision() is the catalog entry "scratch_arena". The previous-frame grain (prevg) stays a module texture: it
// needs the previous frame's SOURCE luma and the module's own delta, neither of which the shared product holds.
#pragma once

#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/gpu_memory/PlTexBackend.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/repair/grain_split/GrainNode.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>

#include <libplacebo/renderer.h>

#include <cstdint>
#include <string>
#include <vector>

namespace TemporalDetail
{
	namespace Gpu
	{
		// Scratch-arena scope of one Apply (one render stage = one scope; GpuAllocator.h) and the owner name its
		// temporaries are requested under. The module's own grain split opens its own scope (GrainSplit::Gpu::kArenaScope)
		// BEFORE this one, so the two never nest.
		constexpr uint16_t kArenaScope = 0x0401;
		constexpr const char* kArenaOwner = "temporal_detail";
		// The shared grain product must have fitted for this long (in content time, Frames(seconds, measured rate)) before
		// the module frees its own split's output textures; a product that drops out again costs one re-creation.
		constexpr double kSplitReleaseSeconds = 1.0;

		// A motion pair on its own grid (gridW x gridH texels, rgba32f or any float: x = vx, y = vy, z = trust 0..1) in
		// pixels of the re-based plane (TemporalDetailMotion.h: MotionPair). The host scales the vectors to the plane.
		struct MotionTex
		{
			pl_tex tex = nullptr;
			double blockPx = 16.0;
		};

		struct Params
		{
			Settings settings;
			uint64_t seq = 0;              // presented source frame sequence (consecutive, else the store resets)
			double contentHz = 0.0;        // measured content rate, <= 0: the reference rate
			double planeLines = 0.0;       // the picture's lines in this plane (sets Scale); 0 = the plane's height
			int colorDepth = 10;           // codes of the signal (the model's units)
			StampView stamps;
			bool cut = false, fade = false;
			ActiveRect active;             // the active picture in this plane (empty = whole plane); bars are copied unchanged
			MotionTex backward;            // pair (seq -> seq - 1) on frame seq's grid; absent = motion lost
			MotionTex forward;             // pair (seq - 1 -> seq), optional (uncertainty only)
			pl_tex quality = nullptr;      // optional bw x bh float texture (x: bitrate quality 0..1), absent = neutral
			// Pass a null backward/forward texture for an invalid pair (the model's PairUsable): null backward = motion lost.
			// What the host has but this GPU path does not use yet (the status then says so; never silent):
			bool lookAheadAvailable = false;   // queued future frames (the model's future slot / referee)
			bool cameraAvailable = false;      // a usable camera-model record (the model's homography chain)
			// Model-only measurement knobs (Model::Tuning; never user settings)
			double grainBudget = kGrainBudget;
			bool templateClean = true;
			double cleanKappa = kCleanKappa;
			double guardTemplateShare = kGuardTemplateShare;
			bool guard = true;
			// Reference-store tier: past slots kept (1 .. kSlots). Small cards keep fewer (SlotsForBudget); the status says
			// "store N of 5 slots (memory tier)" whenever it is below kSlots. The CPU model's Tuning::slots matches it.
			int slots = kSlots;
			GateTuning gates;              // the gate thresholds (the model's Tuning::gates)
			// Regionally adaptive gates: optional bw x bh float texture on the active picture's block grid, per block
			// (x margin, y base allowance, z uncertainty limit, w strength) factors on the tunables (FrameIn::blockGates).
			pl_tex blockGates = nullptr;
			// The shared grain.split product of this frame, read through its one API (RegionEvidence::GrainView; the luma
			// plane's GPU form, computed on the same active picture with this module's scale and block). Absent or not
			// fitting: the module computes its own (status says so).
			RegionEvidence::GrainView grain;
			// The source frame the shared product must be for (Frame::seq); kAnySeq = not checked. Another frame or another
			// unit version is STALE: visible fallback to the module's own split, recovery on the next fitting frame.
			uint64_t grainSeq = GrainSplit::Gpu::kAnySeq;
			// The governor dropped the shared product (GrainNode::Shed()): the module uses its own split and says so (Shed).
			bool grainShed = false;
			bool autoEngaged = false;      // temporal_detail = auto: the host's AutoGate decision (idle = the input, no pass)
			// SA6: the render thread's scratch arena (the owner thread; BeginFrame per frame is the host's). Non-null: the
			// cleaner accumulators, the transferred delta and the own split's intermediates come from it (the module
			// destroys its private copies). Null: private textures, exactly as before.
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena = nullptr;
		};

		struct Counters
		{
			int guardActive = 0, addBackBlocks = 0, guardTrips = 0, refreshes = 0, liveSlots = 0;
			bool admitted = false;
		};

		class Module
		{
		public:
			Module() = default;
			~Module();
			Module(const Module&) = delete;
			Module& operator=(const Module&) = delete;

			// The returned luma texture belongs to the module and stays valid until the next Apply (a host that keeps frames
			// copies it).
			// `source` with its luma plane processed into this module's texture (other planes pass through). Off:
			// `out` = `source`, no pass. A repeat (same sequence and size) returns the last output without advancing.
			// False (and `out` untouched) on any failure: the caller renders the frame as it was and shows the reason.
			bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
			// The counters of the last processed frame (blocking download of two texels).
			bool ReadCounters(pl_gpu gpu, Counters& out);
			// off, ok, reset: <reason>, motion lost; plus "; not on GPU: look-ahead, camera model" when relevant.
			const std::string& Status() const { return m_status; }
			// Where the grain split of the last processed frame came from (recomputed every frame, so the module returns to
			// the shared product the first frame it fits again): Ring = the shared product fit and was read; Missing = no
			// shared product (absent or invalid view); WrongSize = a product exists but does not fit this plane (size, block,
			// scale, code unit, component). Shed is the node's (never set here). Off / idle: Ring (nothing read, nothing to show).
			GrainSplit::GrainStatus LastGrainStatus() const { return m_grainStatus; }
			// The visible fallback ("grain split (temporal detail)"): engaged while the module runs its own grain split.
			FallbackCatalog::Decision GrainFallbackDecision() const { return GrainSplit::ReaderFallback("temporal detail", m_grainStatus); }
			void Forget();                 // a discontinuity: the store restarts at the next frame
			void Release(pl_gpu gpu);
			const std::string& LastError() const { return m_error; }
			unsigned long long Failures() const { return m_failures; }
			// True when the LAST Apply failed because the scratch arena had no texture (Params::arena set). The frame is
			// then rendered as it was; the store restarts at the next frame (a skipped frame breaks the sequence anyway).
			bool ScratchShort() const { return m_scratchShort; }
			// Whether the module currently holds its own grain split's output textures (diagnostics and tests): true after a
			// frame that computed its own split, false once the shared product has fitted for kSplitReleaseSeconds.
			bool OwnSplitHeld() const;

			// The scratch-arena requests of one Apply for an ACTIVE PICTURE of w x h (the module's working size): scope
			// kArenaScope, owner kArenaOwner, tier T1 (an optional repair: shed before the T0 temporaries), count 3 of one
			// rg32f w x h texture with the template cleaner (two accumulators + delta), else 1; plus, when ownSplit, the
			// grain split's own intermediates (GrainSplit::Gpu::Splitter::ArenaRequests, T0) so the visible fallback runs
			// inside the budget too. Empty when the formats do not map to arena formats.
			static std::vector<VpSkeleton::ArenaRequest> ArenaRequests(pl_gpu gpu, int w, int h, double planeLines,
				bool templateClean = true, bool ownSplit = true);

			// VRAM of the module's textures at w x h (bytes; with the shared grain split's), for the cost table.
			// tempsInArena: the accumulators, the delta and the own split's intermediates are the arena's (counted in its
			// plan, not here): 40 bytes per pixel less. The default is the private-texture form, unchanged.
			static uint64_t TextureBytes(int w, int h, double planeLines, int slots = kSlots, bool tempsInArena = false);
			// The largest slot count whose textures fit in budgetBytes (at least 1).
			static int SlotsForBudget(uint64_t budgetBytes, int w, int h, double planeLines);

		private:
			struct Impl;
			Impl* m_impl = nullptr;
			std::string m_error, m_status = "off";
			GrainSplit::GrainStatus m_grainStatus = GrainSplit::GrainStatus::Ring;
			unsigned long long m_failures = 0;
			bool m_scratchShort = false;
		};
	}
}
