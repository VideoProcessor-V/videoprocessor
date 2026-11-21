// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// CC series A (A1): the de-judder extras' state, moved verbatim (types, initialisers, comments) out of the renderer's
// Impl struct into ONE member, `Vpv::Dejudder::ExtrasState dejudderExtras;` (no reference aliases: every reader and
// writer names `dejudderExtras.<member>`, the member names are the old ones). The values and their meaning are the old
// members'; nothing else changes. The render-ahead counters the Presenter already holds (RenderAheadPolicy.h, P6 rest
// S3: frames, lead, shownError, ...) are not here: their readers name the Presenter's fields. The present-id bookkeeping
// (dejudderExtraIds / dejudderMainRefresh / dejudderIdsSwapchain) keeps its own storage here, separate from the
// Presenter's extraIds / mainRefresh (held for the owner with PresentContext::native).
//
// Later steps (series A): the extras' body (PresentDejudderExtras) becomes Vpv::Dejudder::PresentExtras over this state
// and a host view, compiled in DejudderExtrasPresent.cpp.
//
// Threading: the render thread only, as before, apart from renderAheadFlushReanchor (atomic: set at the render loop's
// backlog flush, taken by the next frame's extras).

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/consumers/RenderedWarp.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>

#include <atlbase.h>
#include <dxgi.h>
#include <libplacebo/gpu.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>

namespace Vpv
{
	namespace Dejudder
	{
		// 6b-2 extra presents: the counters.
		struct DejudderExtraStats
		{
			unsigned long long sourceFrames = 0;    // source frames that got at least one extra
			unsigned long long extras = 0;
			unsigned long long holds = 0;           // source frames held (no extras) while active
			unsigned long long swapchainFull = 0;   // an extra's Present blocked > half a refresh
			unsigned long long late = 0;            // an extra finished after its vsync
			unsigned long long nextFrameLate = 0;   // the frame after extras showed a refresh late
			unsigned long long renderFailures = 0;
			double lastMs = 0.0;                    // warp + render + present of the last extra
			// Delta 8 R1: the sigma-delta's GPU time per extra (running averages),
			// with the de-judder's vectors and with its own search.
			double ditherMsExternal = 0.0;
			double ditherMsOwn = 0.0;
			bool ditherLastExternal = false;
			int lastExtrasPerFrame = 0;
			double cadenceRatio = 0.0;
			std::string lastHold;
		};

		struct ExtrasState
		{
			// ---- 6b-2 extra presents: phase state, the vsync output and the counters ----
			DejudderExtraStats dejudderExtraStats;
			CComPtr<IDXGIOutput> dejudderOutput;
			IDXGISwapChain* dejudderOutputSwapchain = nullptr;   // identity only, not owned
			IDXGISwapChain* dejudderOutputFailedSwapchain = nullptr;   // GetContainingOutput failed (identity only)
			// Present ids of recent extras and the refresh each main present (A)
			// showed on, so presentation telemetry sees what it would without extras.
			std::deque<uint32_t> dejudderExtraIds;
			std::deque<std::pair<uint32_t, uint32_t>> dejudderMainRefresh;
			IDXGISwapChain* dejudderIdsSwapchain = nullptr;   // identity only, not owned

			// ---- render-ahead (docs/VP-render-ahead-design.md): the extras' own counters, not the Presenter's ----
			unsigned long long renderAheadTimerOrdinal = 0, renderAheadTimerSerial = 0;
			unsigned long long renderAheadLogTick = 0;
			bool renderAheadSwapchainLogged = false;
			// Render-ahead: a backlog flush dropped queued frames to shed latency; the chain
			// re-anchors on the next frame (else its fixed L holds the shed latency back in
			// and the queue overflows again: a flush every few frames on the laptop panel).
			std::atomic<bool> renderAheadFlushReanchor{ false };

			// ---- 6c: the occlusion mask. Estimate(A, B) writes B's mask into the slot
			// not holding the mask for the frame now showing; dejudderMaskSequence
			// says which source frame each slot is for (0 = none/used). ----
			uint64_t dejudderMaskSequence[2] = { 0, 0 };
			uint64_t dejudderMaskEpoch[2] = { 0, 0 };   // the motion epoch each slot's mask was made in
			// VP's 50/60p interpolation (Stage A): the extras' machinery with the interpolator in place of the warp.
			unsigned long long interpolatedPresents = 0, interpolationFailures = 0;

			// ---- Stage 6d: A's PRE_OUTPUT picture for the extras, and its warp ----
			DejudderRendered::Cache dejudderCache;
			std::unique_ptr<DejudderRendered::Warp> dejudderRenderedWarp;
			DejudderRendered::Geometry dejudderGeometry;
			bool dejudderGeometryValid = false;   // this frame's geometry (both warp paths; delta 8 R1)
			// Hunt 2 10-layout M1: the source frame whose draw the NLS hook recovery redrew; its
			// extras hold (their geometry, cache and NLS map were the failed draw's).
			uint64_t dejudderNlsRetrySequence = 0;
			// Owner 2026-10-13: this frame's picture has a moved subtitle (cut/paste);
			// the rendered warp's extras run, holding its rectangles still.
			bool dejudderSubtitleComposed = false;
			// Delta 8 R1: the sigma-delta's vectors on the extras (VPVEC).
			std::unique_ptr<DejudderRendered::VectorField> dejudderVectorField;
			unsigned long long ditherVectorsExternal = 0;
			int ditherVectorsLogged = -1;

			// ---- motion.consumers (group D audit D3): failed warp / interpolation passes, visible while recent ----
			FallbackCatalog::MotionConsumerFailures motionConsumerFailures;
			// The de-judder held this frame because the display rate is not known (presentation entry).
			bool dejudderHeldNoDisplayRate = false;

			// ---- Efficiency (owner 2026-10-14): the sigma-delta's vectors for B's own
			// first present, made from A's field as A's extras end (the motion from
			// A's last picture to B), so B's refresh skips the sigma-delta's own
			// search (~6 ms on the laptop). For this sequence and geometry only. ----
			std::unique_ptr<DejudderRendered::VectorField> dejudderNextVectorField;
			pl_tex dejudderNextVectors = nullptr;
			uint64_t dejudderNextVectorsSequence = 0;
			uint64_t dejudderNextVectorsFrom = 0;   // R3: A's sequence (resync_event on: A and B must share their stamps)
			MotionValidity::ConsumerGate ditherNextGate;   // the next-frame VPVEC's pair and epoch (step 1 E)
			DejudderRendered::Geometry dejudderNextVectorsGeometry;
			int dejudderNextVectorsW = 0, dejudderNextVectorsH = 0;
			uint64_t ditherVectorsNextUsed = 0;
			bool ditherVectorsNextLogged = false;
		};
	}
}
