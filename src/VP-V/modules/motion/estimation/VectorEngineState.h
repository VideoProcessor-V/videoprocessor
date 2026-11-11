// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a: the vector engine's own state, moved verbatim out of the renderer's Impl (inventory items b, c, d of
// doc 52 section 3.1: the vector-demand bookkeeping, the optical-flow producer and look-ahead members, the one
// vectors-valid state and its invalidation counters). The renderer keeps ONE member of this type, `vectorEngine`,
// declared at the position of the earliest moved member, and a block of reference aliases with the old names so
// every reader in a zone another package still owns compiles unchanged (doc 52 1.2). Each alias is deleted by
// the package that moves its last reader (HOOKS.md in this folder lists them).
//
// Threading is unchanged and documented per member: any thread = atomics only; render thread = under renderMutex;
// render loop = the loop thread (no renderMutex). motionAheadEngaged is the one member whose two writers (the
// render loop and the render thread) hold different locks; it is an atomic here (doc 52 D-3, no value change).
// C++14: this header is included by VideoProcessor-Lib code.
#pragma once

#include <VP-V/modules/motion/estimation/MotionAhead.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/motion/estimation/MotionResyncReaders.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/consumers/DejudderPolicy.h>
#include <VP-V/modules/motion/estimation/MotionSegment.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/ProducerRecovery.h>
#include <VP-V/modules/motion/estimation/VectorProducts.h>

#include <libplacebo/gpu.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#ifndef _WINDOWS_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace VpMotion
{
	struct VectorEngineState
	{
		// ---- item b: vector engine on demand (VP-V/modules/motion/estimation/VectorDemand.h) ---------------------
		// The decision uses the previous frame's state (one frame of latency); with vector_demand off it is the
		// default-constructed Decision. Render thread.
		VectorDemand::Decision vectorDemand;
		std::atomic<int> queueAheadCapacity{ 0 };         // frames the queue holds after the current one (queue pass)
		std::atomic<bool> motionAheadBackward{ false };   // backward pairs: the producer is fed the current frame
		std::string vectorEngineLogged;
		int vectorDeliveredLogged = -1;
		int vectorDitherKeyLogged = -1;
		unsigned long long vectorPairs = 0, vectorBackwardPairs = 0;
		ULONGLONG vectorStatsTick = 0;
		uint64_t vectorBackwardSequence = 0;
		bool vectorGlobalMissSustained = false;
		unsigned vectorGlobalMisses = 0;
		unsigned vectorGlobalHits = 0;
		uint64_t vectorProducerFirstSubmit = 0;
		bool vectorProducerSkipSustained = false;

		// ---- W2-C1 (B9): the one motion-engine state (VectorDemand::MotionEngineStatus; IVectorEngine::State / Work) ----------
		// Derived in UpdateVectorEngine from this frame's decision and re-derived at the end of PlanProducer, so the published state
		// (FallbackStatus, Ctrl+I) is same-frame. ONE-FRAME LAG (documented, kept on purpose): the decision at frame N is made from
		// the previous frame's facts (the producer running / recovering / its start error as PlanProducer(N-1) left them); a
		// producer started at frame N cannot serve N. So a producer stop shows in State() at the latest at the next decision, and
		// already after PlanProducer(N) through the refresh. Render thread.
		VectorDemand::MotionEngineStatus motionEngineStatus;
		VectorDemand::MotionEngineWork motionEngineWork;
		VectorDemand::EngineFacts motionEngineFacts;      // the facts the status was last derived from (PlanProducer refreshes them)
		uint64_t motionEngineFrame = 0;                   // decisions made (the status's frame)
		unsigned long long motionEnginePairsBase = 0;     // producer pairs of producers already stopped (Work().producerPairs)
		bool motionOpticalFlow = true;                    // the optical_flow setting as PlanProducer last saw it
		bool motionLowLatency = false;                    // the low-latency path as PlanProducer last saw it
		bool motionVramRelease = false;                   // the idle keeper is pressured (set by the keeper, NoteVramRelease)
		bool motionCameraGated = false;                   // the camera fit wanted the engine but optical flow is off or did not start
		uint64_t motionStatusEpoch = 0;                   // the validity epoch last seen by the status (resync window)
		ULONGLONG motionStatusEpochAtMs = 0;              // ... and when it moved

		// ---- item c: producer, look-ahead and validity members -------------------------------------------------
		// Owner 2026-10-16: the look-ahead engages with N frames queued ahead and disengages when the queue holds
		// none or the sequence breaks (a skip, seek or reset: a new motion generation); it re-engages only once N
		// are queued again. Writers: the render loop (engage / disengage) and the render thread (producer plan,
		// ProducerPlan::Depth): two locks, hence the atomic. Generation: render loop only.
		std::atomic<bool> motionAheadEngaged{ false };
		uint64_t motionAheadEngagedGeneration = 0;
		// Integration 2026-10-17: the frame last submitted to the producer (its time and DV kind), for the break
		// rule (DejudderPolicy::MotionAheadBreakBefore). Render-loop thread only.
		bool motionAheadPrevKnown = false;
		uint64_t motionAheadPrevGeneration = 0;
		DejudderPolicy::MotionAheadFrame motionAheadPrev;
		// The producer computing the pairs ahead (render thread starts / stops it; the render loop submits frames;
		// the de-judder takes its pairs). It stores them under motionValidity.StreamGeneration().
		// Hardening (P5a-rest; audit correction): RenderLocked runs only inside RenderLoop, so the "render thread"
		// (PlanProducer) and the "render loop" are the SAME OS thread and no race existed between them. The one
		// cross-thread writer is the retirement path (GPU release, VectorEngine::ReleaseProducerFor), which is safe
		// because Retire joins m_renderThread before RetireOutput; ~Impl is single-threaded. The shared_ptr is kept as
		// cheap hardening: published with std::atomic_store (PublishProducer) and read by the loop's hand-off through
		// one std::atomic_load copy per use (LoopProducer), so it stays safe if a writer ever moves to another thread.
		std::shared_ptr<MotionAhead::Producer> motionProducer;
		void PublishProducer(std::shared_ptr<MotionAhead::Producer> producer) { std::atomic_store(&motionProducer, std::move(producer)); }
		std::shared_ptr<MotionAhead::Producer> LoopProducer() const { return std::atomic_load(&motionProducer); }
		// SK-MV (MV3): the P2 slot, the ONE store of the NVOF pairs. The producer PUBLISHES into it (PlanProducer injects it with
		// SetSlot); every consumer reads through P2() (a read-only view), never from the producer. It outlives a producer's
		// stop and restart; a started producer clears it.
		std::shared_ptr<VpMotion::P2Slot> p2Slot = std::make_shared<VpMotion::P2Slot>(MotionAhead::kP2RingPairs);
		VpMotion::P2View P2() const { return VpMotion::P2View(p2Slot); }
		// C2a-F 2 (every P2 reader of the engine): the stored pair (from, to) only when it was published under the current stream
		// generation (MotionValidity::State::PairGenerationCurrent). The slot keeps a pair computed before a resync / seek / queue
		// reset / format change until the producer's asynchronous SetGeneration re-keys it, and once a frame is kept in the new epoch
		// the epoch form of PairServes no longer refuses it (resync_event off; and on, after the seal). `stale` is set when a pair
		// was found and refused.
		MotionAhead::FlowPtr FindCurrent(uint64_t from, uint64_t to, bool* stale = nullptr) const
		{
			uint64_t pairGeneration = 0;
			MotionAhead::FlowPtr flow = P2().Find(from, to, pairGeneration);
			const bool refused = flow && !motionValidity.PairGenerationCurrent(pairGeneration);
			if (stale)
				*stale = refused;
			return refused ? MotionAhead::FlowPtr() : flow;
		}
		// SK-MV (MV2): estimates (optical flow wanted) that found no pair computed ahead and took the matcher path; sustained runs or a
		// sustained partial miss ratio make the visible "motion" fallback entry (NoPairTracker, VectorProducts.h). Render thread.
		VpMotion::NoPairTracker motionNoPair;
		// Why the look-ahead producer is not there, for the no-pair cause (render thread, set by PlanProducer): the motion_ahead_frames
		// setting, the low-latency path and the producer's real start error.
		int motionAheadSetting = -1;           // -1 = not planned yet
		bool motionLowLatencyNoVectors = false;
		// W2-C1 (H3Idle): the idle warm-up's last logged transient skip reason (logged once per change; cleared after a warm-up runs).
		const char* motionWarmSkipLogged = nullptr;
		std::string motionProducerStartError;
		// SK-MV: the standing cost status line ("motion engine standing for de-judder: N pairs/s, M ms/pair"; empty when not standing)
		// for Ctrl+I, refreshed every 5 s by PlanProducer. Render thread.
		std::string motionStandingLine;
		bool motionStandingHookSeen = false;   // the host has set ProducerInputs::dejudderCanEngage at least once
		std::chrono::steady_clock::time_point motionStandingAt{};
		unsigned long long motionStandingPairs = 0;
		double motionStandingMsSum = 0.0;
		// SK-MV: the cause and words of the last no-pair miss, kept while the miss is SUSTAINED so the fallback entry does not toggle with
		// every pair that does arrive (NoPairCause as unsigned char; 0 = none).
		unsigned char motionNoPairCause = 0;
		std::string motionNoPairDetail;
		// SK-MV hiccup gap seed: the last adjacent pair the shared P2 slot served (its whole-frame motion, the later frame, the epoch it was
		// taken in), kept so a fill pair across a gap can start from it (GapSeed.h). Render thread.
		bool motionLastSharedValid = false;
		float motionLastSharedGlobal[2][4] = {};
		uint64_t motionLastSharedTo = 0, motionLastSharedEpoch = 0;
		unsigned long long motionGapSeeded = 0, motionGapUnseeded = 0;   // fill pairs estimated with / without the seed (counters)
		unsigned long long motionGapRejected = 0;   // fill pairs whose handed-over seed the matcher dropped (SAD vs zero), counted apart from unseeded and logged
		// SK-MV: NvOpticalFlow::Sessions::Violations() as last logged (render thread).
		unsigned long long nvofViolationsSeen = 0;
		// One vectors-valid state (docs/VP-interpolator-50-60-plan.md step 1; owner [[vp-vector-validity]]):
		// every discontinuity moves its epoch from any thread; the render thread applies it
		// (ApplyMotionInvalidate) and every vector consumer checks the epoch its vectors were made in.
		MotionValidity::State motionValidity;
		uint64_t motionValiditySeen = MotionValidity::State::kInitialEpoch;   // render thread
		// W1-C2a A1: the motion readers the resync "motion" provider registers on each era's Indicator (VectorEngine::RegisterResyncFor).
		// Declared after motionValidity (its callbacks hold both addresses; the Indicator's owner, the host's Shadow, goes first).
		VpMotionResync::Readers motionResyncReaders;
		// S0 motion segment, SHADOW step (P-b1, 2026-10-24 draft): it shares motionValidity, records every live
		// estimate and withdraws it where the host clears dejudderLiveFieldFrom. Mismatches are counted and
		// logged; nothing is delivered from it yet, so pixels are unchanged.
		// SK-MV (MV1): the pair's P3 field set (the matcher's live outputs of the pair, published right after its Estimate; the
		// source of the delivery says whether P2 (EngineAhead) or P2m (Matcher) fed it). The textures are the matcher's: valid
		// until the next Estimate replaces them (the host Retracts the segment then) and never owned or freed here.
		struct MotionShadowField
		{
			uint64_t from = 0, to = 0;
			pl_tex field = nullptr, frameTrust = nullptr, subField = nullptr, camera = nullptr;
			pl_tex grainError = nullptr;   // SK-MV pass-through: the grain map of the same pair (null when the estimate did not ask for it)
		};
		VpMotion::MotionSegment<MotionShadowField> motionSegment{ &motionValidity };
		unsigned long long motionShadowChecks = 0, motionShadowMismatches = 0;
		std::atomic<int> motionAheadDepth{ 0 };   // the frames ahead to submit (0 = none)
		// r6 fix 3: the time-based depth at this source rate (MotionAhead::DepthAt): the look-ahead engages once
		// that many are queued ahead.
		std::atomic<int> motionAheadConfigured{ MotionAhead::kDefaultDepth };
		// W2-C1: the old single latch (motionProducerFailedLogged) split in two. startFailed = the last start failed: not retried every
		// frame, only when the back-off (motionProducerStartRetry, ProducerRecovery's waits) is due or the producer is wanted anew.
		// stopLogged = a running producer stopped and was logged once (log-once only; cleared when a producer runs again). A GPU
		// release (ReleaseProducerFor) clears both: it is a recovery event, so the producer may start again on the new device.
		bool motionProducerStartFailed = false;
		bool motionProducerStopLogged = false;
		MotionAhead::ProducerRecovery motionProducerStartRetry;
		unsigned long long motionAheadHits = 0, motionAheadMisses = 0;
		// The render loop's hand-off of frames to the producer (render loop thread writes).
		std::atomic<double> motionHandoffMaxMs{ 0.0 }, motionHandoffTotalMs{ 0.0 };
		std::atomic<unsigned long long> motionHandoffCount{ 0 };
		// The last logged optical-flow depth decision (render thread).
		std::string motionAheadLogged;
		std::chrono::steady_clock::time_point motionAheadLoggedAt{};

		// ---- item d: invalidation requests and their counters --------------------------------------------------
		uint64_t motionInvalidations = 0;
		ULONGLONG motionInvalidateLogTick = 0;
		bool motionRecoveryLogPending = false;   // log the first estimate after a logged invalidation
		int motionInvalidateLoggedCause = -1;    // a MotionValidity::Cause, -1 = none yet
		std::atomic<const char*> motionGapDetail{ nullptr };   // a frame gap's own cause (a static string), for the log
	};
}
