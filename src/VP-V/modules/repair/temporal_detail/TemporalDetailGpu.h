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
#pragma once

#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>

#include <libplacebo/renderer.h>

#include <cstdint>
#include <string>

namespace TemporalDetail
{
	namespace Gpu
	{
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
			bool autoEngaged = false;      // temporal_detail = auto: the host's AutoGate decision (idle = the input, no pass)
		};

		struct Counters
		{
			int guardActive = 0, addBackBlocks = 0, refreshes = 0, liveSlots = 0;
			uint32_t guardTrips = 0;   // cumulative over the stream; exact (uint32 on the GPU, wraps at 2^32)
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
			void Forget();                 // a discontinuity: the store restarts at the next frame
			void Release(pl_gpu gpu);
			const std::string& LastError() const { return m_error; }
			unsigned long long Failures() const { return m_failures; }

			// VRAM of the module's textures at w x h (bytes; with the shared grain split's), for the cost table.
			static uint64_t TextureBytes(int w, int h, double planeLines, int slots = kSlots);
			// The largest slot count whose textures fit in budgetBytes (at least 1).
			static int SlotsForBudget(uint64_t budgetBytes, int w, int h, double planeLines);

		private:
			struct Impl;
			Impl* m_impl = nullptr;
			std::string m_error, m_status = "off";
			unsigned long long m_failures = 0;
		};
	}
}
