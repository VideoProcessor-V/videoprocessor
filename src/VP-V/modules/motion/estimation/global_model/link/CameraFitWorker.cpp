// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraFitWorker.h"

#include "CameraUsable.h"

#include <atomic>

namespace CameraLink
{
	namespace
	{
		std::atomic<int> g_threadsStarted{ 0 };
	}

	CameraFitWorker::CameraFitWorker(CameraFitProducer& producer, const CameraFitEnv& env)
		: m_producer(producer), m_env(env)
	{
		if (m_env.queueCapacity == 0)
			m_env.queueCapacity = 1;
		m_lease = std::move(m_env.lease);   // the worker's own copy is m_lease: env must not pin an old state for ever
		if (m_env.leaseSource && !m_lease.Valid())
			m_lease = m_env.leaseSource();
	}

	VpSkeleton::LaneLease CameraFitWorker::Lease() const
	{
		std::lock_guard<std::mutex> lock(m_leaseMutex);
		return m_lease;
	}

	uint64_t CameraFitWorker::LaneGeneration() const
	{
		std::lock_guard<std::mutex> lock(m_leaseMutex);
		return m_lease.generation;
	}

	bool CameraFitWorker::Rebind()
	{
		if (!m_env.leaseSource)
			return false;
		// Read OUTSIDE the mutex: the render thread (Submit) and the fit thread (publish) both rebind, and each may be descheduled between
		// the read and the install. Only a NEWER lease is installed (LaneLeaseNewer), so the older read of the two can never put the
		// worker back on a graph the other thread already left.
		VpSkeleton::LaneLease cur = m_env.leaseSource();
		VpSkeleton::LaneLease old;   // released after the unlock: the last hold of an old state dies outside the mutex
		{
			std::lock_guard<std::mutex> lock(m_leaseMutex);
			if (!VpSkeleton::LaneLeaseNewer(cur, m_lease))
				return false;
			old = std::move(m_lease);
			m_lease = std::move(cur);
		}
		if (m_env.counters)
			m_env.counters->rebound.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	CameraFitWorker::~CameraFitWorker()
	{
		Stop();
	}

	int CameraFitWorker::ThreadsEverStarted()
	{
		return g_threadsStarted.load();
	}

	bool CameraFitWorker::Start()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_running)
			return false;
		m_stop = false;
		m_running = true;
		g_threadsStarted.fetch_add(1);
		m_thread = std::thread([this]() { Loop(); });
		return true;
	}

	void CameraFitWorker::Stop()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_running)
				return;
			m_stop = true;
		}
		m_cv.notify_all();
		if (m_thread.joinable())
			m_thread.join();
		std::lock_guard<std::mutex> lock(m_mutex);
		m_running = false;
		m_queue.clear();
	}

	bool CameraFitWorker::Submit(const CameraFitJob& submitted)
	{
		CameraFitJob job = submitted;
		Rebind();   // a rebuilt lane is noticed before the job is stamped with the generation it will be published into
		job.laneGen = LaneGeneration();
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_queue.size() >= m_env.queueCapacity)
			{
				if (m_env.counters)
					m_env.counters->queueFull.fetch_add(1, std::memory_order_relaxed);
				// The pair chain has a hole: the next fit must not seed from the previous pair.
				m_producer.NoteReset();
				return false;
			}
			job.gensStamped = true;   // the generations as of THIS submit (after a drop's NoteReset above)
			job.resetGen = m_producer.ResetGen();
			job.timingGen = m_producer.TimingGen();
			m_queue.push_back(job);
			if (m_env.counters)
				m_env.counters->submitted.fetch_add(1, std::memory_order_relaxed);
		}
		m_cv.notify_one();
		return true;
	}

	size_t CameraFitWorker::Pending() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_queue.size();
	}

	size_t CameraFitWorker::ProcessQueued()
	{
		size_t n = 0;
		for (;;)
		{
			CameraFitJob job;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_queue.empty())
					break;
				job = std::move(m_queue.front());
				m_queue.pop_front();
			}
			ProcessJob(job);
			++n;
		}
		return n;
	}

	void CameraFitWorker::WaitIdle()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_idleCv.wait(lock, [this]() { return (m_queue.empty() && !m_busy) || !m_running; });
	}

	void CameraFitWorker::Loop()
	{
		for (;;)
		{
			CameraFitJob job;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_busy = false;
				m_idleCv.notify_all();
				m_cv.wait(lock, [this]() { return m_stop || !m_queue.empty(); });
				if (m_stop)
					return;
				job = std::move(m_queue.front());
				m_queue.pop_front();
				m_busy = true;
			}
			ProcessJob(job);
		}
	}

	PublishOutcome CameraFitWorker::ProcessJob(const CameraFitJob& job)
	{
		// Always fit (deterministic history); only the publication can be late.
		const CameraFitResult fit = m_producer.Fit(job);
		const PublishOutcome o = PublishFit(job, fit);
		if (m_env.onResult)
			m_env.onResult(job, fit, o);
		return o;
	}

	PublishOutcome CameraFitWorker::PublishFit(const CameraFitJob& job, const CameraFitResult& fit)
	{
		FitCounters* c = m_env.counters;
		auto late = [&](PublishOutcome o) {
			if (c)
				c->late.fetch_add(1, std::memory_order_relaxed);
			return o;
		};
		// The lease is taken ONCE and held for the whole publish: a rebuild during it cannot free the table, the bytes or the Indicator.
		Rebind();
		const VpSkeleton::LaneLease lease = Lease();
		if (job.laneGen != lease.generation)
		{
			// The job was submitted against another graph (its seq lives in slots that no longer exist): refused, counted, visible
			// (status line "stale lane", and the seal sees no P7 for that frame: camera.no_model.late).
			if (c)
				c->staleLane.fetch_add(1, std::memory_order_relaxed);
			return PublishOutcome::StaleLane;
		}
		VpSkeleton::SlotSet* set = lease.table ? lease.table->Find(VpSkeleton::kP7CameraFit) : nullptr;
		if (!set || !m_env.pool || !fit.record)
			return PublishOutcome::NoProduct;

		VpSkeleton::LaneTicket ticket;
		if (set->Claim(job.seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &ticket) != VpSkeleton::ClaimResult::Claimed)
			return late(PublishOutcome::LateClaim);
		if (!set->Writable(ticket))
		{
			set->Abandon(ticket);
			return late(PublishOutcome::LateNotWritable);
		}
		VpAncestry::Header& hdr = set->LaneHeader(ticket);
		VpSkeleton::LaneInputs pins;
		if (!lease.table->LaneAncestors(VpSkeleton::kP7CameraFit, job.seq, hdr, pins))
		{
			set->Abandon(ticket);
			return late(PublishOutcome::LateInputs);
		}
		// The stamp is read AFTER the pins (as the LaneRunner does): a late event is then either honoured here or seen by the seal.
		hdr.honoured = m_env.stampOf ? m_env.stampOf(job.seq) : (lease.indicator ? lease.indicator->StampOf(job.seq) : job.stamp);
		hdr.costUs = fit.record->costMicros > 0 ? static_cast<uint32_t>(fit.record->costMicros) : 0u;
		uint8_t* bytes = m_env.slotBytes ? m_env.slotBytes(ticket.slot)
			: (lease.bytes ? lease.bytes->Ring(static_cast<size_t>(ticket.slot)) : nullptr);
		if (bytes)
			EncodeRecordPod(*fit.record, bytes, kRecordPodMaxBytes);
		// The pool entry is written BEFORE the publish (the seal can accept the instant it is Published) and erased if the
		// publish turns out late, so a late record is never served.
		m_env.pool->Put(job.seq, fit.record, job.field);
		const VpSkeleton::PublishResult pr = set->Publish(ticket);
		pins.Unpin();
		if (pr != VpSkeleton::PublishResult::Published)
		{
			m_env.pool->Erase(job.seq);
			return late(PublishOutcome::LateSeal);
		}
		m_env.pool->Commit(job.seq);
		if (c)
		{
			c->published.fetch_add(1, std::memory_order_relaxed);
			if (!Usable(*fit.record))
				c->unusable.fetch_add(1, std::memory_order_relaxed);
		}
		return PublishOutcome::Published;
	}
}
