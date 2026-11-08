// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-5): the interim driver of the P7 producer until SK-W makes the lane runner live (K1-9 replaces it by a
// LaneRunner registration of the same CameraFitProducer). ONE std::thread, started only when the graph contains P7, with a
// bounded job queue (mutex + condition variable). It drives the producer through Claim -> write -> stamp -> Publish
// exactly as the lane protocol (SlotSet) requires; the render thread never waits on it:
//  * Submit() never blocks beyond the queue mutex; a full queue drops the NEWEST job, counts it and requests a fitter reset
//    (a hole in the pair chain would otherwise feed the temporal seed a non-consecutive pair): visible, never silent.
//  * Every submitted pair is FITTED, even when it turns out to be late; a late result is not published (the SlotSet
//    discards it, the pool entry is erased) so the fit history is the same with and without lateness.
// Deterministic runs call ProcessJob() directly on the calling thread (no thread started at all).
#pragma once

#include "CameraFitProducer.h"

#include <VP-V/core/LaneLease.h>
#include <VP-V/core/ProductSlots.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace CameraLink
{
	// PublishedOnTime: AHEAD FIT only, the record went into the on-time target (before its frame's seal) instead of the lag bridge.
	enum class PublishOutcome : uint8_t { Published, LateClaim, LateNotWritable, LateInputs, LateSeal, NoProduct, StaleLane, PublishedOnTime, StaleP2 };
	inline bool IsPublished(PublishOutcome o) { return o == PublishOutcome::Published || o == PublishOutcome::PublishedOnTime; }

	inline const char* Text(PublishOutcome o)
	{
		switch (o)
		{
		case PublishOutcome::Published: return "published";
		case PublishOutcome::LateClaim: return "late (slot already sealed or taken)";
		case PublishOutcome::LateNotWritable: return "late (slot still pinned)";
		case PublishOutcome::LateInputs: return "late (inputs not published)";
		case PublishOutcome::LateSeal: return "late (sealed before the publish)";
		case PublishOutcome::StaleLane: return "refused (the lane graph was rebuilt after the job was submitted)";
		case PublishOutcome::PublishedOnTime: return "published on time";
		case PublishOutcome::StaleP2: return "refused (the P2 pair's vectors-valid epoch moved: a resync / gap after FlowOf)";
		default: return "no P7 product in the graph";
		}
	}

	struct CameraFitEnv
	{
		// The lane objects (SlotTable, the P7 bytes ring, the Indicator) are held through a LaneLease, never raw pointers: the lane
		// swaps them on every rebuild. `leaseSource` re-acquires the current lease (AnalysisLane::Lease bound to P7; thread-safe);
		// `lease` is the first one. Standalone / tests without a lane: lease = LaneLease::Borrow(...) and no source (never stale).
		VpSkeleton::LaneLease lease;
		VpSkeleton::LaneLeaseSource leaseSource;
		CameraRecordPool* pool = nullptr;
		FitCounters* counters = nullptr;
		// Optional overrides; unset, the lease's bytes ring (>= kRecordPodMaxBytes per slot) and Indicator::StampOf are used.
		std::function<uint8_t*(int slot)> slotBytes;                   // the P7 ring slot bytes
		std::function<VpIndicator::Stamp(uint64_t seq)> stampOf;       // Indicator::StampOf, read AFTER the pins
		size_t queueCapacity = 4;
		// Called on the fitting thread after every job (published or late): status and the trace. Must be short.
		std::function<void(const CameraFitJob&, const CameraFitResult&, PublishOutcome)> onResult;
		// AHEAD FIT (camera_ahead_fit; false = the worker runs exactly as before). A job marked `ahead` is fitted at once (in submit order,
		// so the fit history is the same whenever it is published) and its result is PARKED until it can be published:
		//   * on time: into the on-time target (onTimeSource: the lane that seals P7(N) at N), while N's seal has not run (N > the render
		//     position given to Tick); "inputs not published" there is retried at the next Tick (the target's P3 / P4 / P2 of N appear
		//     just before N's seal), any other refusal goes to the bridge;
		//   * bridged: into the lag bridge (the worker's own lease), once N has started rendering (its markers exist; N <= render position).
		// Every park ends in exactly one onResult. A full park ends its oldest entry as late. Seed jobs (not ahead) publish as before.
		bool aheadMode = false;
		VpSkeleton::LaneLeaseSource onTimeSource;   // empty = no on-time target: every ahead result is bridged (shown)
		// G-A3: the engine's CURRENT vectors-valid epoch (IVectorEngine::FieldEra, any thread); an Ahead field whose p2Epoch differs is
		// refused at the claim (PublishOutcome::StaleP2, FitCounters::p2RefusedClaim). Empty = no check.
		std::function<uint64_t()> p2Epoch;
		// AHEAD FIT (audit F3): the pair's whole-frame motion (the engine's FlowGlobal), worked out on the camera worker before the fit. Any
		// thread, pure CPU. Empty = no engine-global prior for ahead jobs.
		std::function<void(AheadCopy&)> aheadGlobal;
		size_t parkCapacity = 64;
	};

	class CameraFitWorker
	{
	public:
		CameraFitWorker(CameraFitProducer& producer, const CameraFitEnv& env);
		~CameraFitWorker();
		CameraFitWorker(const CameraFitWorker&) = delete;
		CameraFitWorker& operator=(const CameraFitWorker&) = delete;

		// Creates the single thread. Call only when the graph contains P7. False when already running.
		bool Start();
		void Stop();
		bool Running() const { return m_running; }
		static int ThreadsEverStarted();   // test hook: the CameraOff test asserts it stays at its starting value

		// Render thread. True: queued (a thread, or ProcessQueued, runs it). False: the queue is full (counted, fitter reset requested).
		// Rebinds first (a changed lane generation is noticed here), then stamps the job with the lease's generation.
		bool Submit(const CameraFitJob& job);

		// ---- lane rebind (any thread; the camera link calls it every frame start) ----
		// Takes the lane's CURRENT lease from env.leaseSource; true when it differs (counted in FitCounters::rebound). The old lease
		// is released only when no publish in flight still holds it. No source (borrowed lease): never changes.
		bool Rebind();
		VpSkeleton::LaneLease Lease() const;   // a copy (shares the hold): safe to use after any rebuild
		uint64_t LaneGeneration() const;       // the held lease's generation (0 = borrowed or none)
		size_t Pending() const;

		// Fit + publish on the calling thread (the worker thread calls this too).
		PublishOutcome ProcessJob(const CameraFitJob& job);
		PublishOutcome PublishFit(const CameraFitJob& job, const CameraFitResult& fit);
		// AHEAD FIT (render thread, every Begin and AfterRender of frame renderSeq): the render position the parked results are judged
		// by; wakes the fitting thread to retry them. Without a thread (deterministic / tests) call RetryParked on the calling thread.
		void Tick(uint64_t renderSeq);
		size_t RetryParked();          // fitting thread (or the only thread): returns the parked results that ended
		size_t Parked() const;
		uint64_t RenderPosition() const { return m_renderSeq.load(std::memory_order_acquire); }
		// Test hook: blocks until the queue is empty and the worker is idle (never used on the render thread).
		void WaitIdle();
		// Deterministic mode (no thread started): fit + publish every queued job on the calling thread, in order.
		size_t ProcessQueued();

	private:
		void Loop();
		// The publish into one lease (Claim -> pins -> stamp -> bytes -> pool -> Publish). countLate: count a refusal as late (the bridge).
		PublishOutcome PublishInto(const VpSkeleton::LaneLease& lease, const CameraFitJob& job, const CameraFitResult& fit, bool countLate);
		struct ParkedResult
		{
			CameraFitJob job;
			CameraFitResult fit;
			bool onTimeDone = false;   // the on-time target was tried and refused for good (or does not exist)
		};
		// One attempt for a parked result: true = it ended (outcome set), false = keep it parked.
		bool TryParked(ParkedResult& p, uint64_t renderSeq, PublishOutcome& outcome);
		void Finish(const CameraFitJob& job, const CameraFitResult& fit, PublishOutcome o);

		CameraFitProducer& m_producer;
		CameraFitEnv m_env;
		mutable std::mutex m_leaseMutex;      // guards m_lease (the render thread rebinds, the fit thread publishes)
		VpSkeleton::LaneLease m_lease;
		mutable std::mutex m_mutex;
		std::condition_variable m_cv, m_idleCv;
		std::deque<CameraFitJob> m_queue;
		bool m_stop = false;
		bool m_running = false;
		bool m_busy = false;
		bool m_tick = false;                  // AHEAD FIT: a Tick arrived (under m_mutex)
		std::atomic<uint64_t> m_renderSeq{ 0 };
		std::deque<ParkedResult> m_parked;    // the fitting thread only (or the one calling thread without a thread)
		std::atomic<size_t> m_parkedCount{ 0 };
		std::thread m_thread;
	};
}
