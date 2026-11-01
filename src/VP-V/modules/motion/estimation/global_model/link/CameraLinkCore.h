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

#include "CameraFitWorker.h"
#include "CameraLink.h"
#include "FieldReadbackRing.h"
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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace CameraLink
{
	// The per-frame facts the H1 FrameStart phase carries (defined locally; the adapter fills them).
	struct FramePhaseFacts
	{
		uint64_t seq = 0;                 // the frame starting to render
		double displayPeriodSeconds = 0;  // the measured OUTPUT period (ResolveRate(DisplayRefresh)); 0 = unknown
		bool p7SealedPublished = false;   // the seal accepted a lane-published P7 for `seq`
	};

	struct CameraLinkSettings
	{
		bool p7Wanted = false;            // the product graph contains P7
		bool deterministic = false;       // N1 mode: no deadline; the host drains with ProcessPending()
		bool laneDepthZero = false;       // the graph has no lane depth (no look-ahead): visible
		size_t queueCapacity = 4;         // the bounded job queue
		size_t readbackEntries = 0;       // 0 = lane lead + 2, at least 3 (set by the host from the graph)
		size_t poolRing = 8;              // parallel to the P7 SlotSet ring
		CameraModel::Config fit;
	};

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
		// A (re)built graph: the P7 SlotSet table and its byte ring. nullptr table = the graph has no P7 -> DetachGraph.
		void AttachGraph(VpSkeleton::SlotTable* table, VpSkeleton::CpuBuffers* p7Bytes, VpIndicator::Indicator* indicator);
		void DetachGraph();
		void SetEngine(VpMotion::IVectorEngine* engine) { m_engine = engine; }
		void SetFactsProvider(PairFactsProvider p) { m_provider = std::move(p); }
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

		// Deterministic mode / tests: fit + publish every queued job on the calling thread (no thread involved).
		size_t ProcessPending();
		// Test hook for deterministic drivers that have no GPU: submit a ready field copy exactly as the ring path does.
		bool SubmitField(std::shared_ptr<const FieldCopy> field, const FramePhaseFacts& f);

		// ---- introspection ----
		bool Active() const { return m_active; }
		uint64_t LastRequestedField() const { return m_lastRequestedField; }
		void DebugSetLastRequestedField(uint64_t v) { m_lastRequestedField = v; }   // tests only
		bool HasThread() const { return m_worker && m_worker->Running(); }
		bool Allocated() const { return m_producer != nullptr; }
		const FitCounters& Counters() const { return m_counters; }
		uint64_t HistoryHash() const { return m_producer ? m_producer->HistoryHash() : 0; }
		CameraFitProducer* Producer() { return m_producer.get(); }
		CameraFitWorker* Worker() { return m_worker.get(); }
		CameraRecordPool* Pool() { return m_pool.get(); }

		// ---- ICameraLink ----
		std::shared_ptr<const CameraModel::CameraMotion> SealedRecord(uint64_t seq) const override;
		std::shared_ptr<const CameraModel::CameraMotion> CausalRecord(uint64_t seq) const override;
		std::shared_ptr<const FieldCopy> FieldOf(uint64_t seq) const override;
		bool LabelsFor(uint64_t seq, CameraConsumer c, CameraLabelsView& out) override;
		std::shared_ptr<const CameraModel::GuidanceFrame> GuideFor(uint64_t seq) override;

		// K2 / K4 plug in here (set by their owners; null = unavailable).
		std::function<bool(uint64_t, CameraConsumer, CameraLabelsView&)> labelsProvider;
		std::function<std::shared_ptr<const CameraModel::GuidanceFrame>(uint64_t)> guideProvider;

	private:
		void Teardown();
		void SubmitReady(const std::vector<std::shared_ptr<FieldCopy>>& ready, const FramePhaseFacts& f);
		void OnResult(const CameraFitJob& job, const CameraFitResult& r, PublishOutcome o);

		FrameAnalysis::Store& m_store;
		CameraSealToStore m_sealToStore;
		CameraLinkSettings m_settings;
		bool m_active = false;
		VpSkeleton::SlotTable* m_table = nullptr;
		VpSkeleton::CpuBuffers* m_bytes = nullptr;
		VpIndicator::Indicator* m_indicator = nullptr;
		VpMotion::IVectorEngine* m_engine = nullptr;
		PairFactsProvider m_provider;
		FitCounters m_counters;
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
		enum { kEvLate, kEvReadback, kEvTiming, kEvSlip, kEvQueue, kEvCount };
		Hold m_hold[kEvCount];
		std::atomic<uint32_t> m_events{ 0 };   // bits set by any thread, consumed by OnFrameStart
		uint64_t m_seenQueueFull = 0;
		std::vector<uint64_t> m_sealedMark;   // seq accepted at the seal, indexed seq mod size

		// status state touched by the worker's onResult
		mutable std::mutex m_statusMutex;
		AmbiguousTracker m_ambiguous;
		StatusFacts m_facts;
		uint64_t m_records = 0, m_ambiguousRecords = 0;
		std::mutex m_traceMutex;
		std::ofstream m_trace;
		uint64_t m_seenReadbackDropped = 0;
	};
}
