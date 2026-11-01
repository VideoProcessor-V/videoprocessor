// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraFitProducer.h"

#include <chrono>
#include <cstring>

namespace CameraLink
{
	namespace
	{
		int64_t SteadyMicros()
		{
			return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void Mix(uint64_t& h, const void* p, size_t n)
		{
			const uint8_t* b = static_cast<const uint8_t*>(p);
			for (size_t i = 0; i < n; ++i)
			{
				h ^= b[i];
				h *= 1099511628211ull;
			}
		}

		CameraModel::Config WithClock(CameraModel::Config c)
		{
			if (!c.clock)
				c.clock = &SteadyMicros;
			return c;
		}
	}

	CameraFitProducer::CameraFitProducer(const CameraModel::Config& config, FitCounters* counters)
		: m_fitter(WithClock(config)), m_counters(counters)
	{
	}

	void CameraFitProducer::SetTimingFacts(const TimingFacts& f)
	{
		std::lock_guard<std::mutex> lock(m_factsMutex);
		m_facts = f;
	}

	void CameraFitProducer::Consume(const CameraFitJob& job)
	{
		const uint64_t r = job.gensStamped ? job.resetGen : m_resetGen.load(std::memory_order_acquire);
		if (r != m_seenReset)
		{
			m_seenReset = r;
			m_fitter.Reset();
			if (m_counters)
				m_counters->resets.fetch_add(1, std::memory_order_relaxed);
		}
		const uint64_t t = job.gensStamped ? job.timingGen : m_timingGen.load(std::memory_order_acquire);
		if (t != m_seenTiming)
		{
			m_seenTiming = t;
			TimingFacts f;
			{
				std::lock_guard<std::mutex> lock(m_factsMutex);
				f = m_facts;
			}
			m_timing.Latch(f);
			if (m_counters)
				m_counters->relatches.fetch_add(1, std::memory_order_relaxed);
		}
	}

	CameraFitResult CameraFitProducer::Fit(const CameraFitJob& job)
	{
		Consume(job);
		CameraFitResult res;
		PairFacts facts = job.facts;
		if (!(facts.pairSeconds > 0))
			facts.pairSeconds = m_timing.Seconds(job.uniqueFramesBetween, job.haveTimestamps, job.tsFrom, job.tsTo);
		res.timingUnknown = !(facts.pairSeconds > 0);

		CameraModel::PairInput in;
		AssembleInfo info;
		const AssembleStatus st = job.field ? AssemblePair(*job.field, facts, job.stamp, in, &info) : AssembleStatus::NoField;
		res.status = st;
		res.pictureWasEmpty = info.pictureWasEmpty;
		res.noBackward = info.noBackward;
		if (st != AssembleStatus::Ready)
		{
			// Not fitted: a GenerationReset record. The next Fit follows a discontinuity, so the history restarts.
			if (st == AssembleStatus::NotFittedSlip || st == AssembleStatus::NotFittedAncient)
				m_fitter.Reset();
			res.record = std::make_shared<const CameraModel::CameraMotion>(
				NotFittedRecord(job.seq, job.field ? job.field->pairFrom : 0, job.field ? job.field->pairTo : 0));
			if (m_counters && (st == AssembleStatus::NotFittedSlip || st == AssembleStatus::NotFittedAncient))
				m_counters->notFittedSlip.fetch_add(1, std::memory_order_relaxed);
			return res;
		}
		if (m_counters && res.timingUnknown)
			m_counters->timingUnknown.fetch_add(1, std::memory_order_relaxed);
		auto rec = std::make_shared<CameraModel::CameraMotion>(m_fitter.Fit(in));
		if (m_counters)
			m_counters->fitted.fetch_add(1, std::memory_order_relaxed);
		// EWMA of the measured cost (1/4 weight): the producer's chunk prediction.
		const uint32_t cost = rec->costMicros > 0 ? static_cast<uint32_t>(rec->costMicros) : 0u;
		m_costEwmaUs = m_costEwmaUs == 0 ? cost : (m_costEwmaUs * 3u + cost) / 4u;
		Mix(m_hash, &rec->seq, sizeof(rec->seq));
		Mix(m_hash, rec->params.h, sizeof(rec->params.h));
		Mix(m_hash, &rec->confidence, sizeof(rec->confidence));
		const uint8_t rr = static_cast<uint8_t>(rec->reason);
		Mix(m_hash, &rr, 1);
		res.record = rec;
		return res;
	}

	bool CameraFitProducer::Begin(const VpSkeleton::LaneJobContext& job)
	{
		(void)job;
		return m_havePending;
	}

	VpSkeleton::ChunkResult CameraFitProducer::RunChunk(const VpSkeleton::LaneJobContext& job, uint32_t budgetUs)
	{
		(void)budgetUs;
		if (!m_havePending)
			return VpSkeleton::ChunkResult::Failed;
		m_havePending = false;
		const CameraFitResult r = Fit(m_pending);
		if (!r.record || !job.out || job.outBytes < kRecordPodMaxBytes)
			return VpSkeleton::ChunkResult::Failed;
		if (EncodeRecordPod(*r.record, job.out, job.outBytes) == 0)
			return VpSkeleton::ChunkResult::Failed;
		if (job.header)
			job.header->costUs = r.record->costMicros > 0 ? static_cast<uint32_t>(r.record->costMicros) : 0u;
		return VpSkeleton::ChunkResult::Done;
	}
}
