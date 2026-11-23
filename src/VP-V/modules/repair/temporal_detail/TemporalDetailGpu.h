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
// Grain split (OD 11-14 04:30, WAIVER-B2-TD-OWNSPLIT): the module NEVER computes a grain split of its own. It reads the
// lane's shared product when that fits (Params::grain), else it asks the skeleton memo for its variant (Params::memo:
// Acquire(kP10GrainSplit, seq, Gpu, VariantFor(w, h, scale, colorDepth)); the owner repair/grain_split computes it once per
// (frame, variant), GrainSplitVariantOwner.h). Refused visibly (Apply false, LastError says why, LastAcquireCause()):
// no memo; an active-picture crop with no fitting shared product (the crop axis is not on this base: VariantUndeclared);
// a Scale the variant axis cannot represent; any memo refusal (its cause token).
//
// Memory (SA6): the per-frame temporaries (the cleaner's two accumulators and the transferred delta) can come from the
// shared scratch arena (Params::arena; null = private textures, byte-identical pixels to before). An arena shortage is never
// silent: Apply fails for that frame (ScratchShort(), LastError "scratch memory short (...)") and the arena's own
// FrameDecision() is the catalog entry "scratch_arena". The previous-frame grain (prevg) stays a module texture: it
// needs the previous frame's SOURCE luma and the module's own delta, neither of which the shared product holds.
#pragma once

#include <VP-V/core/ScratchArena.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
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
		// temporaries are requested under.
		constexpr uint16_t kArenaScope = 0x0401;
		constexpr const char* kArenaOwner = "temporal_detail";

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
			// U7 v3 partial reset (owner 2026-11-15): the fade's history keep (FrameAnalysis::Scene::fadeKeep, 0..1). A fade is NO reset:
			// every stored slot's keep is multiplied by it (the tab texture's row 1). < 0 = not given: that fade resets ("reset: fade (no keep)").
			float fadeKeep = -1.0f;
			// cut_soft_reset (the model's FrameIn::sceneRestart; review softreset r1 L1): the production consumers' scene answer, as
			// SharedSceneSource copies it. given: it replaces cut / fade / fadeKeep; a fade or a soft reset scales every stored slot's
			// keep by historyKeep through the same fade_keep (no shader change), its restart resets with its cause.
			SceneRestartInput sceneRestart;
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
			double guardTemplateShare = GrainSplit::Guard::kTemplateShare;
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
			// fitting: the module asks the memo for its variant (status says so); it never computes a split itself.
			RegionEvidence::GrainView grain;
			// The source frame the shared product must be for (Frame::seq); kAnySeq = not checked. Another frame or another
			// unit version is STALE: visible fallback to the memo variant, recovery on the next fitting frame.
			uint64_t grainSeq = GrainSplit::Gpu::kAnySeq;
			// The governor dropped the shared product (GrainNode::Shed()): the module asks the memo, whose regulator decides,
			// and says so (Shed).
			bool grainShed = false;
			// The skeleton memo (render thread): the grain split variant when the shared product does not fit. Null: no
			// variant can be had and the module refuses the frame visibly ("no skeleton memo").
			VpSkeleton::SkeletonMemo* memo = nullptr;
			bool autoEngaged = false;      // temporal_detail = auto: the host's AutoGate decision (idle = the input, no pass)
			// SA6: the render thread's scratch arena (the owner thread; BeginFrame per frame is the host's). Non-null: the
			// cleaner accumulators and the transferred delta come from it (the module destroys its private copies). Null:
			// private textures, identical pixels.
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
			// scale, code unit, component); Stale = another frame; Shed = the governor dropped it. Every status but Ring means
			// the split came from the memo variant (or the frame was refused). Off / idle: Ring (nothing read, nothing to show).
			GrainSplit::GrainStatus LastGrainStatus() const { return m_grainStatus; }
			// The memo's answer for the last frame that asked it (None = the shared product fit, or nothing was asked). A crop
			// with no fitting shared product is VariantUndeclared without an Acquire (the crop axis is not on this base).
			VpSkeleton::AcquireCause LastAcquireCause() const { return m_acquireCause; }
			VpSkeleton::AcquireStatus LastAcquireStatus() const { return m_acquireStatus; }
			// The visible fallback ("grain split (temporal detail)"): engaged while the lane's shared product does not fit.
			FallbackCatalog::Decision GrainFallbackDecision() const { return GrainSplit::ReaderFallback("temporal detail", m_grainStatus); }
			void Forget();                 // a discontinuity: the store restarts at the next frame
			void Release(pl_gpu gpu);
			const std::string& LastError() const { return m_error; }
			unsigned long long Failures() const { return m_failures; }
			// True when the LAST Apply failed because the scratch arena had no texture (Params::arena set). The frame is
			// then rendered as it was; the store restarts at the next frame (a skipped frame breaks the sequence anyway).
			bool ScratchShort() const { return m_scratchShort; }
			// Diagnostics and tests only, read-only (gate cb-u1 r2 MUST 3): the per (slot, block) evidence textures the
			// step 6 validation wrote on the last Apply. which = 0: ev_a (hard, ageGate, stronger, e1w); which = 1: ev_b (hb,
			// focus, unc x base x corr, ageSec). Layout (K x bw) x bh rgba32f, texel td_blk(k, b) = (k x bw + bx, by). Null
			// before the first Apply, after Release, or for any other `which`. The module owns the texture; it stays valid
			// until the next Apply / Release, and the caller must not write it.
			pl_tex LastEvidenceTexture(int which) const;

			// The scratch-arena requests of one Apply for an ACTIVE PICTURE of w x h (the module's working size): scope
			// kArenaScope, owner kArenaOwner, tier T1 (an optional repair: shed before the T0 temporaries), count 3 of one
			// rg32f w x h texture with the template cleaner (two accumulators + delta), else 1. Empty when the formats do not
			// map to arena formats. (The grain split variant is the memo owner's, on the memo's VRAM ledger grant.)
			static std::vector<VpSkeleton::ArenaRequest> ArenaRequests(pl_gpu gpu, int w, int h, double planeLines,
				bool templateClean = true);

			// VRAM of the module's textures at w x h (bytes; with the shared grain split's), for the cost table.
			// tempsInArena: the accumulators and the delta are the arena's (counted in its plan, not here): 24 bytes per pixel
			// less. The default is the private-texture form, unchanged.
			static uint64_t TextureBytes(int w, int h, double planeLines, int slots = kSlots, bool tempsInArena = false);
			// The largest slot count whose textures fit in budgetBytes (at least 1).
			static int SlotsForBudget(uint64_t budgetBytes, int w, int h, double planeLines);

		private:
			struct Impl;
			Impl* m_impl = nullptr;
			std::string m_error, m_status = "off";
			GrainSplit::GrainStatus m_grainStatus = GrainSplit::GrainStatus::Ring;
			VpSkeleton::AcquireCause m_acquireCause = VpSkeleton::AcquireCause::None;
			VpSkeleton::AcquireStatus m_acquireStatus = VpSkeleton::AcquireStatus::Unavailable;
			unsigned long long m_failures = 0;
			bool m_scratchShort = false;
		};
	}
}
