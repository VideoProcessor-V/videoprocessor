// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-6): the core of the camera extension, as plain methods mirroring the H1 phases (the IRendererExtension
// adapter is a later ~40-line wiring step, see link/README.md "Wiring TODO"):
//   OnFrameStart(facts)   after FrameSeal: seal observer (P7 -> FrameAnalysis store), poll the readback ring, submit jobs,
//                         request the copy of each new verified field
//   OnPostSubmitGap()     a second poll only
//   OnGpuCreated / OnGpuReleased   the readback ring's lifetime
//   OnSettings(...)       what the graph asks of the camera (P7 / P8 / guidance); nothing wanted -> nothing exists
//   CollectStatus(lines)  visible state (never log-only)
//   Link()                the ICameraLink consumers use ("vpv.camera.v1")
// With no consumer of P7 there is no product, ring, pool, producer, thread or shader text (byte-identical, CameraOffTests).
// Threading: every method except the worker's onResult and the ICameraLink readers runs on the render thread.
#pragma once

#include "AheadFlow.h"
#include "CameraFitWorker.h"
#include "CameraLink.h"
#include "FieldReadbackRing.h"
#include "GuidanceTimers.h"
#include "PairAssembly.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/Indicator.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/global_model/CameraReaders.h>
#include <VP-V/modules/motion/estimation/global_model/CameraStoreAdapter.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace CameraLink
{
	class CameraDump;

	// The per-frame facts the H1 FrameStart phase carries (defined locally; the adapter fills them).
	struct FramePhaseFacts
	{
		uint64_t seq = 0;                 // the frame starting to render
		double displayPeriodSeconds = 0;  // the measured OUTPUT period (ResolveRate(DisplayRefresh)); 0 = unknown
		bool p7SealedPublished = false;   // the seal accepted a lane-published P7 for `seq`
		// AHEAD FIT: the frame STARTING to render (its on-time seal just ran); 0 = the same as `seq` (no lag bridge in between).
		uint64_t renderSeq = 0;
	};

	// AHEAD FIT: the decision id (Fallbacks / Config status / Ctrl+Alt+D) engaged while there is NO usable on-time target (no skeleton lane
	// holds P7): then every frame is bridged. A single frame that is late with a target present is NOT a defect (G-A2, "on time or not at
	// all"): it is counted and shown as an informational line (AheadLateLine), never as an engaged entry.
	constexpr const char* kIdAheadNoTarget = "camera.ahead.no_target";
	// What the on-time target is called in the status line when there is none or it holds no P7.
	constexpr const char* kAheadNoTargetText =
		"none (no skeleton lane holds P7: analysis_lane off, no source yet, or the demand not applied yet): every frame goes to the lag bridge";
	// G-A2: the informational line of the frames that missed the on-time seal and were sealed by the lag bridge instead.
	inline std::string AheadLateLine(const FitCounters& c)
	{
		const unsigned long long late = c.aheadBridged.load() + c.aheadSeedFallback.load();
		const unsigned long long all = late + c.onTimeSealed.load();
		return "camera fit: " + std::to_string(late) + " of " + std::to_string(all) + " frames late (bridged): the lag bridge sealed them "
			"(informational: on time or not at all)";
	}
	// Deterministic runs take ONE path: the lag bridge from the seed (the ahead fit's on-time / bridged split depends on wall-clock).
	constexpr const char* kAheadDeterministicText =
		"camera ahead fit: off in deterministic mode (the deterministic path is the seed fit through the lag bridge)";

	struct CameraLinkSettings
	{
		bool p7Wanted = false;            // the product graph contains P7
		bool deterministic = false;       // N1 mode: no deadline; the host drains with ProcessPending()
		bool laneDepthZero = false;       // the graph has no lane depth (no look-ahead): visible
		size_t queueCapacity = 4;         // the bounded job queue
		size_t readbackEntries = 0;       // 0 = lane lead + 2, at least 3 (set by the host from the graph)
		size_t poolRing = 8;              // parallel to the P7 SlotSet ring
		CameraModel::Config fit;
		// AHEAD FIT (camera_ahead_fit): the fit takes its input from the ahead NVOF pair (SetAheadSource) when one exists, and publishes
		// on time into the on-time target (AttachOnTime) when it finished before the frame's seal, else into the lag bridge. Ignored in
		// deterministic mode (one path: the bridge; shown). Off: nothing below exists or runs.
		bool aheadFit = false;
	};

	// AHEAD FIT: the render-thread source of the ahead pair of `seq` (IVectorEngine::FlowOf + CopyAhead, copied at once).
	using AheadSource = std::function<AheadRefusal(uint64_t seq, AheadCopy& out)>;

	// What the host provides per field copy that is ready: the pair facts (P4 rect and events, engine global, slip).
	// Returns false when the pair cannot be described (counted as a readback drop).
	struct PairDescription
	{
		PairFacts facts;
		int uniqueFramesBetween = 1;
		bool haveTimestamps = false;
		double tsFrom = 0, tsTo = 0;
	};
	using PairFactsProvider = std::function<bool(const FieldCopy& field, PairDescription& out)>;

	class CameraLinkCore : public ICameraLink
	{
	public:
		explicit CameraLinkCore(FrameAnalysis::Store& store);
		~CameraLinkCore() override;

		// ---- configuration (render thread) ----
		void OnSettings(const CameraLinkSettings& s);
		// LIVE: the lane is held through leases, never raw pointers (the lane swaps its state on every rebuild). `source` re-acquires the
		// current lease (bind AnalysisLane::Lease(kP7CameraFit) in a lambda); the link takes the first one now and REBINDS when the
		// generation changes (CameraFitWorker::Rebind, noticed at Submit / publish / OnFrameStart). A job submitted against an older
		// generation is refused and counted (FitCounters::staleLane, the status line). An empty first lease or one without P7 -> no attach.
		void AttachLane(VpSkeleton::LaneLeaseSource source);
		// STANDALONE / TESTS: borrowed raw objects that outlive the link by construction; never stale. nullptr table = no P7.
		void AttachGraph(VpSkeleton::SlotTable* table, VpSkeleton::CpuBuffers* p7Bytes, VpIndicator::Indicator* indicator);
		void DetachGraph();
		void SetEngine(VpMotion::IVectorEngine* engine) { m_engine = engine; }
		void SetFactsProvider(PairFactsProvider p) { m_provider = std::move(p); }
		// ---- AHEAD FIT (render thread, before AttachLane / AttachGraph) ----
		// The ahead pair source and its facts provider (an ahead pair is not yet estimated by the matcher: the engine's render-time
		// facts (PairSpansSlip, EngineGlobal) do not exist for it; the host describes it from the pair and the frame-analysis store).
		// `global` (optional, any thread) works out the pair's whole-frame motion; it runs on the camera WORKER with the conversion (audit F3:
		// the render thread only copies).
		void SetAheadSource(AheadSource source, PairFactsProvider facts, std::function<void(AheadCopy&)> global = std::function<void(AheadCopy&)>())
		{
			m_aheadSource = std::move(source);
			m_aheadFacts = std::move(facts);
			m_aheadGlobal = std::move(global);
		}
		// The on-time target: the lane that seals P7(N) at N (the skeleton lane's AnalysisLane::Lease(kP7CameraFit)). Empty = none.
		// `name` is what the status line calls it.
		void AttachOnTime(VpSkeleton::LaneLeaseSource source, const std::string& name) { m_onTimeSource = std::move(source); m_onTimeName = name; }
		// The 16-px source grid the ahead pair is converted onto (the live field's grid that sized the camera graph).
		void SetAheadGrid(int sourceGridW, int sourceGridH) { m_aheadGridW = sourceGridW; m_aheadGridH = sourceGridH; }
		// G-A3: the engine's current vectors-valid epoch (IVectorEngine::FieldEra, any thread): an Ahead record whose P2 epoch differs is
		// refused at the claim, the on-time seal and every consumer read. Empty = no check. Set before AttachLane.
		void SetP2EpochSource(std::function<uint64_t()> source) { m_p2Epoch = std::move(source); }
		// The matcher's forward/backward consistency limit in engine px (SeedField::consistencyPx when one is seen; default 2, the
		// dejudder_consistency_px default, as the TD motion source).
		void SetAheadConsistency(float enginePx) { if (enginePx > 0.0f) m_aheadConsistency = enginePx; }
		bool AheadActive() const { return m_active && m_settings.aheadFit && !m_settings.deterministic && static_cast<bool>(m_aheadSource); }
		// The on-time target's lease held P7 at the last frame start (render thread; false without a target).
		bool OnTimeUsable() const { std::lock_guard<std::mutex> lock(m_statusMutex); return m_onTimeUsable; }
		uint64_t AheadNext() const { return m_aheadNext; }
		void SetTimingFacts(const TimingFacts& f);
		// Registers the reader (reset -> NoteReset, timing -> NoteTiming). Setup, before the first Observe.
		int RegisterReader(VpIndicator::Indicator& indicator);

		// ---- phases ----
		void OnFrameStart(const FramePhaseFacts& f);
		void OnPostSubmitGap();
		void OnGpuCreated(pl_gpu gpu, pl_log log = nullptr);
		void OnGpuReleased();
		void CollectStatus(std::vector<std::string>& lines);
		std::vector<FallbackCatalog::Decision> CollectDecisions();
		ICameraLink* Link() { return this; }

		// K3: the VP_CAMERA_DUMP writer. Fed from the camera worker after each Fit (a shared_ptr copy and a bounded-queue push, so the
		// fit deadline is untouched). null = no dump. Any thread; the core keeps the writer alive until it is replaced.
		void SetDump(std::shared_ptr<CameraDump> dump);
		// K3: the newest fit for the status line (have = false until a record was fitted). Any thread.
		LastFitSummary LastFit() const;

		// Deterministic mode / tests: fit + publish every queued job on the calling thread (no thread involved).
		size_t ProcessPending();
		// Test hook for deterministic drivers that have no GPU: submit a ready field copy exactly as the ring path does.
		bool SubmitField(std::shared_ptr<const FieldCopy> field, const FramePhaseFacts& f);
		// AHEAD FIT: submit an ahead field (origin Ahead) as the ahead poll does (tests drive it directly). False: refused / queue full.
		bool SubmitAheadField(std::shared_ptr<const FieldCopy> field, const AheadCopy* global = nullptr);
		// AHEAD FIT (audit F3): submit the COPY; the worker converts it. The facts are taken here from the pair's frames (render thread).
		bool SubmitAheadCopy(std::shared_ptr<const AheadCopy> copy);

		// ---- introspection ----
		bool Active() const { return m_active; }
		uint64_t LastRequestedField() const { return m_lastRequestedField; }
		void DebugSetLastRequestedField(uint64_t v) { m_lastRequestedField = v; }   // tests only
		bool HasThread() const { return m_worker && m_worker->Running(); }
		bool Allocated() const { return m_producer != nullptr; }
		const FitCounters& Counters() const { return m_counters; }
		FitCounters& MutableCounters() { return m_counters; }
		uint64_t HistoryHash() const { return m_producer ? m_producer->HistoryHash() : 0; }
		CameraFitProducer* Producer() { return m_producer.get(); }
		CameraFitWorker* Worker() { return m_worker.get(); }
		CameraRecordPool* Pool() { return m_pool.get(); }

		// ---- ICameraLink ----
		std::shared_ptr<const CameraModel::CameraMotion> SealedRecord(uint64_t seq) const override;
		std::shared_ptr<const CameraModel::CameraMotion> CausalRecord(uint64_t seq) const override;
		std::shared_ptr<const CameraModel::CameraMotion> OnTimeRecord(uint64_t seq, OnTimeCheck check, OnTimeMiss& why) const override;
		SealVerdict OnTimeSealVerdict(uint64_t seq) const override;
		std::shared_ptr<const FieldCopy> FieldOf(uint64_t seq) const override;
		bool LabelsFor(uint64_t seq, CameraConsumer c, CameraLabelsView& out) override;
		std::shared_ptr<const CameraModel::GuidanceFrame> GuideFor(uint64_t seq) override;

		// K4: the guide lane job's trigger. Called on the fitting thread after EVERY fit (published or late), with the record's seq,
		// once its pool entry is committed (publish happened before). Set at setup; a copy is taken per call under a mutex.
		// LOCK RULE: the observer runs under m_observerMutex on the camera worker, and SetFitDoneObserver (the adapter's Detach / Reconcile, render
		// thread, renderMutex held) takes the same mutex, so it waits for a call in flight. Therefore nothing the observer reaches (the guide
		// lane job: the host's p4 reader, the context provider) may ever take renderMutex or wait for the render thread.
		// Debug builds assert that SetFitDoneObserver is not called from the observer's own thread (a self-deadlock).
		void SetFitDoneObserver(std::function<void(uint64_t fitSeq)> f);
		// Costs (camera-live step 2): the fit time (CPU work, on the camera worker) and the readback latency (a WAIT, request -> ready) are
		// posted to the motion pass-timer inbox (PassTimers::Inbox::Motion().PostCameraFit / PostCameraReadback: rows camera.fit and
		// camera.readback). The host drains it into the pass-timer readout (LibplaceboVideoRenderer, SK-W); the skeleton shows the totals.

		// K2 / K4 plug in here (set by their owners; null = unavailable).
		std::function<bool(uint64_t, CameraConsumer, CameraLabelsView&)> labelsProvider;
		std::function<std::shared_ptr<const CameraModel::GuidanceFrame>(uint64_t)> guideProvider;

	private:
		void Teardown();
		void AttachLease(const VpSkeleton::LaneLease& first, VpSkeleton::LaneLeaseSource source);
		void OnLaneRebound();
		void SubmitReady(const std::vector<std::shared_ptr<FieldCopy>>& ready, const FramePhaseFacts& f);
		// AHEAD FIT (render thread).
		void PollAhead();
		void OnTimeSeal(uint64_t renderSeq);
		bool Retired(uint64_t seq) const;
		void MarkRetired(uint64_t seq);
		void OnResult(const CameraFitJob& job, const CameraFitResult& r, PublishOutcome o);

		FrameAnalysis::Store& m_store;
		CameraSealToStore m_sealToStore;
		CameraLinkSettings m_settings;
		bool m_active = false;
		VpMotion::IVectorEngine* m_engine = nullptr;
		PairFactsProvider m_provider;
		mutable FitCounters m_counters;   // mutable: the consumption re-check (SealedRecord, const) counts its refusals
		std::unique_ptr<CameraRecordPool> m_pool;
		std::unique_ptr<CameraFitProducer> m_producer;
		std::unique_ptr<CameraFitWorker> m_worker;
		std::unique_ptr<FieldReadbackRing> m_ring;
		pl_gpu m_gpu = nullptr;
		pl_log m_log = nullptr;
		uint64_t m_lastRequestedField = 0;
		uint64_t m_lastEra = 0;               // IVectorEngine::FieldEra of the last request
		std::chrono::steady_clock::time_point m_lastFrameAt;
		bool m_haveFrameAt = false;
		// Fallback holds: an event engages the id for kHoldSeconds of MEASURED time, so the status never flickers per call.
		struct Hold { double remaining = 0; bool engaged = false; };
		enum { kEvLate, kEvReadback, kEvTiming, kEvSlip, kEvQueue, kEvAhead, kEvCount };   // kEvAhead: a frame was bridged (AHEAD FIT)
		Hold m_hold[kEvCount];
		std::atomic<uint32_t> m_events{ 0 };   // bits set by any thread, consumed by OnFrameStart
		uint64_t m_seenQueueFull = 0;
		std::vector<uint64_t> m_sealedMark;   // seq accepted at the seal, indexed seq mod size
		std::map<uint64_t, std::chrono::steady_clock::time_point> m_requestedAt;   // render thread: field copy requests in flight
		std::mutex m_observerMutex;
		std::atomic<std::thread::id> m_observerThread{ std::thread::id() };   // the thread inside the observer right now (debug check)
		std::function<void(uint64_t)> m_fitDone;

		// status state touched by the worker's onResult
		mutable std::mutex m_statusMutex;
		AmbiguousTracker m_ambiguous;
		StatusFacts m_facts;
		LastFitSummary m_lastFit;
		uint64_t m_records = 0, m_ambiguousRecords = 0;
		std::mutex m_traceMutex;
		std::ofstream m_trace;
		mutable std::mutex m_dumpMutex;
		std::shared_ptr<CameraDump> m_dump;
		uint64_t m_seenReadbackDropped = 0;

		// AHEAD FIT state (render thread unless noted).
		AheadSource m_aheadSource;
		PairFactsProvider m_aheadFacts;
		VpSkeleton::LaneLeaseSource m_onTimeSource;
		std::string m_onTimeName;
		std::function<uint64_t()> m_p2Epoch;
		std::function<void(AheadCopy&)> m_aheadGlobal;
		bool P2Stale(const PoolEntry& e) const
		{
			return e.field && e.field->haveP2 && m_p2Epoch && m_p2Epoch() != e.field->p2Epoch;
		}
		int m_aheadGridW = 0, m_aheadGridH = 0;
		float m_aheadConsistency = 2.0f;
		uint64_t m_aheadNext = 0;            // the next frame the ahead pair is asked for (0 = not started)
		uint64_t m_aheadRefusedSeq = 0;      // the last frame whose pair was refused (counted once)
		uint64_t m_renderSeq = 0;            // the frame rendering now (the last OnFrameStart)
		bool m_onTimeUsable = false;         // under m_statusMutex: the on-time target's lease held P7 at the last frame start
		std::vector<uint64_t> m_seedPending; // frames the seed fallback is fetching while the ahead fit is on (ahead submissions wait)
		enum { kAheadMarks = 256 };
		std::unique_ptr<std::atomic<uint64_t>[]> m_onTimeMark{ new std::atomic<uint64_t>[kAheadMarks] };   // seq + 1 at seq % size: published on time (the worker writes)
		std::unique_ptr<std::atomic<uint64_t>[]> m_aheadAsked{ new std::atomic<uint64_t>[kAheadMarks] };   // seq + 1: the ahead copy of seq was submitted (render thread writes)
		std::unique_ptr<std::atomic<uint64_t>[]> m_sealVerdict{ new std::atomic<uint64_t>[kAheadMarks] };  // (seq + 1) * 2 + rejected: the on-time seal ran (render thread)
		void ClearAheadMarks();
		std::vector<uint64_t> m_retired;     // seq + 1 at seq % size: sealed (or rejected) by the on-time seal: the bridge seal skips it
	};
}
