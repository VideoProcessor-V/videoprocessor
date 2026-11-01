// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-5): the P7 producer. It owns the Fitter EXCLUSIVELY (F2: Fitter is not thread-safe, and K0's BindFitter
// would reset it on the render thread). The reader hooks only call NoteReset() / NoteTiming() (atomic bumps); the thread
// that runs Fit() consumes them before its next Fit. Every pair is fitted, even a late one, so the fitter history (and so
// every later record) is deterministic; only PUBLICATION depends on timing (CameraFitWorker).
// Implements VpSkeleton::ILaneProducer so K1-9 (SK-W) can register it with the LaneRunner unchanged.
#pragma once

#include "CameraRecordPool.h"
#include "CameraFitStatus.h"
#include "FieldCopy.h"
#include "PairAssembly.h"
#include "PairTiming.h"

#include <VP-V/core/LaneInterfaces.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelFit.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace CameraLink
{
	// One pair to fit. The FieldCopy is shared (kept alive by the pool entry of a published record).
	struct CameraFitJob
	{
		uint64_t seq = 0;
		std::shared_ptr<const FieldCopy> field;
		PairFacts facts;                 // facts.pairSeconds > 0 wins; otherwise derived from the timing below
		int uniqueFramesBetween = 1;
		bool haveTimestamps = false;
		double tsFrom = 0, tsTo = 0;
		VpIndicator::Stamp stamp;        // the field's stamp (generation mapping)
	};

	struct CameraFitResult
	{
		std::shared_ptr<const CameraModel::CameraMotion> record;
		AssembleStatus status = AssembleStatus::NoField;
		bool timingUnknown = false;
		bool pictureWasEmpty = false;
		bool noBackward = false;
	};

	class CameraFitProducer : public VpSkeleton::ILaneProducer
	{
	public:
		explicit CameraFitProducer(const CameraModel::Config& config = CameraModel::Config(), FitCounters* counters = nullptr);

		// ---- any thread (the reader hooks, the render thread) ----
		void NoteReset() { m_resetGen.fetch_add(1, std::memory_order_acq_rel); }
		void NoteTiming() { m_timingGen.fetch_add(1, std::memory_order_acq_rel); }
		void SetTimingFacts(const TimingFacts& f);
		uint64_t ResetGen() const { return m_resetGen.load(std::memory_order_acquire); }
		uint64_t TimingGen() const { return m_timingGen.load(std::memory_order_acquire); }

		// ---- the ONE fitting thread ----
		CameraFitResult Fit(const CameraFitJob& job);
		const PairTiming& Timing() const { return m_timing; }
		// A hash of the fit history (params + confidence + reason of every fitted record, in order): equal runs, equal hash.
		uint64_t HistoryHash() const { return m_hash; }
		uint32_t PredictFitUs() const { return m_costEwmaUs; }

		// ---- ILaneProducer (K1-9: LaneRunner registration) ----
		void SetPending(const CameraFitJob& job) { m_pending = job; m_havePending = true; }
		VpSkeleton::ProductId Product() const override { return VpSkeleton::kP7CameraFit; }
		bool Begin(const VpSkeleton::LaneJobContext& job) override;
		VpSkeleton::ChunkResult RunChunk(const VpSkeleton::LaneJobContext& job, uint32_t budgetUs) override;
		uint32_t PredictChunkUs() const override { return m_costEwmaUs; }

	private:
		void Consume();

		CameraModel::Fitter m_fitter;
		PairTiming m_timing;
		FitCounters* m_counters;
		std::atomic<uint64_t> m_resetGen{ 0 }, m_timingGen{ 0 };
		uint64_t m_seenReset = 0, m_seenTiming = 0;
		std::mutex m_factsMutex;
		TimingFacts m_facts;
		uint64_t m_hash = 1469598103934665603ull;
		uint32_t m_costEwmaUs = 0;
		CameraFitJob m_pending;
		bool m_havePending = false;
	};
}
