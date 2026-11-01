// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraLinkCore.h"

#include "CameraUsable.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace CameraLink
{
	namespace
	{
		constexpr double kHoldSeconds = 0.5;   // same measured interval as the ambiguity engage time
		constexpr uint64_t kMaxFieldsPerPump = 16;

		std::string TracePath()
		{
#ifdef _WIN32
			char* v = nullptr;
			size_t n = 0;
			std::string s;
			if (_dupenv_s(&v, &n, "VP_CAMERA_FIT_TRACE") == 0 && v)
			{
				s = v;
				free(v);
			}
			return s;
#else
			const char* v = std::getenv("VP_CAMERA_FIT_TRACE");
			return v ? std::string(v) : std::string();
#endif
		}
	}

	CameraLinkCore::CameraLinkCore(FrameAnalysis::Store& store) : m_store(store), m_sealToStore(store)
	{
	}

	CameraLinkCore::~CameraLinkCore()
	{
		Teardown();
	}

	void CameraLinkCore::Teardown()
	{
		// Stop the thread first: it uses the pool, the producer and the table.
		if (m_worker)
			m_worker->Stop();
		m_worker.reset();
		m_producer.reset();
		m_pool.reset();
		m_ring.reset();
		m_sealedMark.clear();
		m_table = nullptr;
		m_bytes = nullptr;
		m_indicator = nullptr;
		m_lastRequestedField = 0;
		std::lock_guard<std::mutex> lock(m_traceMutex);
		if (m_trace.is_open())
			m_trace.close();
	}

	void CameraLinkCore::OnSettings(const CameraLinkSettings& s)
	{
		m_settings = s;
		m_active = s.p7Wanted;
		if (!m_active)
			Teardown();
		std::lock_guard<std::mutex> lock(m_statusMutex);
		m_facts.graphHasP7 = s.p7Wanted;
		m_facts.laneDepthZero = s.p7Wanted && s.laneDepthZero;
	}

	void CameraLinkCore::DetachGraph()
	{
		Teardown();
	}

	void CameraLinkCore::AttachGraph(VpSkeleton::SlotTable* table, VpSkeleton::CpuBuffers* p7Bytes, VpIndicator::Indicator* indicator)
	{
		Teardown();
		if (!m_active || !table || !table->Find(VpSkeleton::kP7CameraFit))
			return;
		m_table = table;
		m_bytes = p7Bytes;
		m_indicator = indicator;
		const size_t ring = table->Find(VpSkeleton::kP7CameraFit)->Ring();
		m_pool.reset(new CameraRecordPool((std::max)(static_cast<size_t>(ring), m_settings.poolRing)));
		m_sealedMark.assign(ring, ~uint64_t(0));
		m_producer.reset(new CameraFitProducer(m_settings.fit, &m_counters));
		CameraFitEnv env;
		env.table = table;
		env.pool = m_pool.get();
		env.counters = &m_counters;
		env.queueCapacity = m_settings.queueCapacity;
		VpSkeleton::CpuBuffers* bytes = p7Bytes;
		env.slotBytes = [bytes](int slot) -> uint8_t* { return bytes ? bytes->Ring(static_cast<size_t>(slot)) : nullptr; };
		VpIndicator::Indicator* ind = indicator;
		if (ind)
			env.stampOf = [ind](uint64_t seq) { return ind->StampOf(seq); };
		env.onResult = [this](const CameraFitJob& j, const CameraFitResult& r, PublishOutcome o) { OnResult(j, r, o); };
		m_worker.reset(new CameraFitWorker(*m_producer, env));
		if (!m_settings.deterministic)
			m_worker->Start();   // the ONE thread, only because the graph contains P7
		const std::string path = TracePath();
		if (!path.empty())
		{
			std::lock_guard<std::mutex> lock(m_traceMutex);
			m_trace.open(path.c_str(), std::ios::out | std::ios::trunc);
			if (m_trace.is_open())
				m_trace << "seq,pairFrom,pairTo,pairSeconds,reason,confidence,type,params,costUs,late,published\n";
		}
		if (m_gpu && m_settings.readbackEntries >= 3)
			m_ring.reset(new FieldReadbackRing(m_gpu, static_cast<int>(m_settings.readbackEntries), m_log));
		else if (m_gpu)
			m_ring.reset(new FieldReadbackRing(m_gpu, 3, m_log));
	}

	void CameraLinkCore::SetTimingFacts(const TimingFacts& f)
	{
		if (m_producer)
			m_producer->SetTimingFacts(f);
	}

	int CameraLinkCore::RegisterReader(VpIndicator::Indicator& indicator)
	{
		CameraReaderHooks h;
		// F2: the hooks run inside Observe on the render thread; they only bump atomics. The worker resets the Fitter.
		h.reset = [this]() { if (m_producer) m_producer->NoteReset(); };
		h.relatchTiming = [this]() { if (m_producer) m_producer->NoteTiming(); };
		return RegisterCameraReader(indicator, "camera-fit", h);
	}

	void CameraLinkCore::OnGpuCreated(pl_gpu gpu, pl_log log)
	{
		m_gpu = gpu;
		m_log = log;
		if (m_active && m_pool && gpu && !m_ring)
			m_ring.reset(new FieldReadbackRing(gpu, (std::max<int>)(3, static_cast<int>(m_settings.readbackEntries)), log));
	}

	void CameraLinkCore::OnGpuReleased()
	{
		m_ring.reset();   // destructor tolerates copies in flight
		m_gpu = nullptr;
	}

	void CameraLinkCore::SubmitReady(const std::vector<std::shared_ptr<FieldCopy>>& ready, const FramePhaseFacts& f)
	{
		(void)f;
		for (const std::shared_ptr<FieldCopy>& fc : ready)
			SubmitField(fc, f);
	}

	bool CameraLinkCore::SubmitField(std::shared_ptr<const FieldCopy> field, const FramePhaseFacts& f)
	{
		(void)f;
		if (!m_worker || !field)
			return false;
		CameraFitJob job;
		job.seq = field->seq;
		job.field = field;
		job.stamp = field->stamp;
		PairDescription d;
		if (m_provider)
		{
			if (!m_provider(*field, d))
			{
				m_counters.readbackDropped.fetch_add(1, std::memory_order_relaxed);
				if (m_producer)
					m_producer->NoteReset();
				return false;
			}
		}
		job.facts = d.facts;
		job.uniqueFramesBetween = d.uniqueFramesBetween;
		job.haveTimestamps = d.haveTimestamps;
		job.tsFrom = d.tsFrom;
		job.tsTo = d.tsTo;
		// Decision (overrides doc 56 1.4 deadline row): the Fitter gets NO deadline. Every pair is fully fitted, so the history is
		// deterministic; lateness is judged only at the publish / seal.
		job.facts.deadlineMicros = 0;
		return m_worker->Submit(job);
	}

	void CameraLinkCore::OnFrameStart(const FramePhaseFacts& f)
	{
		if (!m_active || !m_pool)
			return;
		// (1) seal observer
		std::shared_ptr<const CameraModel::CameraMotion> rec;
		if (f.p7SealedPublished)
		{
			PoolEntry e;
			if (m_pool->Get(f.seq, e))
				rec = e.record;
		}
		const bool accepted = m_sealToStore.OnSealed(f.seq, rec);
		if (accepted && !m_sealedMark.empty())
			m_sealedMark[f.seq % m_sealedMark.size()] = f.seq;
		(accepted ? m_counters.sealedOk : m_counters.sealedLate).fetch_add(1, std::memory_order_relaxed);
		if (m_settings.laneDepthZero)
			m_counters.depth0.fetch_add(1, std::memory_order_relaxed);
		// P7 lateness = the seal had no published P7 for this frame (independent of the store acceptance).
		if (!f.p7SealedPublished)
			m_events.fetch_or(1u << kEvLate, std::memory_order_relaxed);
		// Measured time since the previous frame start (the output period when the host supplies it).
		const auto now = std::chrono::steady_clock::now();
		double dt = f.displayPeriodSeconds;
		if (!(dt > 0) && m_haveFrameAt)
			dt = std::chrono::duration<double>(now - m_lastFrameAt).count();
		m_lastFrameAt = now;
		m_haveFrameAt = true;
		// (2) poll the ring, (3) request the copy of each new verified field
		OnPostSubmitGap();
		if (m_engine)
		{
			// A new field era (a seek / resync restarted the numbering of the engine): the last-requested bookkeeping is void.
			const uint64_t era = m_engine->FieldEra();
			if (era != m_lastEra)
			{
				m_lastEra = era;
				m_lastRequestedField = 0;
			}
		}
		if (m_ring && m_engine)
		{
			const uint64_t latest = m_engine->LatestVerifiedFieldSeq();
			uint64_t from = m_lastRequestedField + 1;
			if (latest > m_lastRequestedField && latest - m_lastRequestedField > kMaxFieldsPerPump)
				from = latest - kMaxFieldsPerPump + 1;
			for (uint64_t s = from; s <= latest && latest != 0; ++s)
			{
				VpMotion::VerifiedField vf;
				if (!m_engine->FieldOf(s, vf))
					continue;   // no field for this seq (a cadence repeat): not a drop
				if (!m_ring->Request(vf))
				{
					m_counters.readbackDropped.fetch_add(1, std::memory_order_relaxed);
						m_events.fetch_or(1u << kEvReadback, std::memory_order_relaxed);
					if (m_producer)
						m_producer->NoteReset();   // a hole in the pair chain
				}
			}
			if (latest > m_lastRequestedField)
				m_lastRequestedField = latest;
		}
		if (m_settings.deterministic && m_worker)
			m_worker->ProcessQueued();
		// Queue-full drops since the last frame.
		const uint64_t qf = m_counters.queueFull.load(std::memory_order_relaxed);
		if (qf != m_seenQueueFull)
		{
			m_seenQueueFull = qf;
			m_events.fetch_or(1u << kEvQueue, std::memory_order_relaxed);
		}
		// Holds: an event engages for kHoldSeconds of measured time; no event lets the time run down.
		const uint32_t ev = m_events.exchange(0, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(m_statusMutex);
		for (int i = 0; i < kEvCount; ++i)
		{
			Hold& h = m_hold[i];
			if ((ev >> i) & 1u)
				h.remaining = kHoldSeconds;
			else
				h.remaining -= dt > 0 ? dt : 0;
			h.engaged = h.remaining > 0;
		}
		m_facts.lateNow = m_hold[kEvLate].engaged;
		m_facts.readbackFailedNow = m_hold[kEvReadback].engaged;
		m_facts.timingUnknownNow = m_hold[kEvTiming].engaged;
		m_facts.slipNow = m_hold[kEvSlip].engaged;
		m_facts.queueFullNow = m_hold[kEvQueue].engaged;
	}

	void CameraLinkCore::OnPostSubmitGap()
	{
		if (!m_active || !m_ring)
			return;
		std::vector<std::shared_ptr<FieldCopy>> ready;
		m_ring->Poll(ready);
		FramePhaseFacts none;
		SubmitReady(ready, none);
	}

	size_t CameraLinkCore::ProcessPending()
	{
		return m_worker ? m_worker->ProcessQueued() : 0;
	}

	void CameraLinkCore::OnResult(const CameraFitJob& job, const CameraFitResult& r, PublishOutcome o)
	{
		const CameraModel::CameraMotion* rec = r.record.get();
		{
			std::lock_guard<std::mutex> lock(m_statusMutex);
			++m_records;
			if (rec)
			{
				m_ambiguousRecords += rec->reason == CameraModel::Reason::Ambiguous ? 1u : 0u;
				m_facts.ambiguousNow = m_ambiguous.Observe(rec->reason, rec->pairSeconds);
				m_facts.ambiguousShare = m_records ? double(m_ambiguousRecords) / double(m_records) : 0.0;
			}
			if (r.timingUnknown)
				m_events.fetch_or(1u << kEvTiming, std::memory_order_relaxed);
			if (r.status == AssembleStatus::NotFittedSlip || r.status == AssembleStatus::NotFittedAncient)
				m_events.fetch_or(1u << kEvSlip, std::memory_order_relaxed);
		}
		std::lock_guard<std::mutex> lock(m_traceMutex);
		if (m_trace.is_open() && rec)
		{
			char b[512];
			std::snprintf(b, sizeof(b), "%llu,%llu,%llu,%.9g,%s,%.6g,%d,%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g,%lld,%d,%d\n",
				(unsigned long long)job.seq, (unsigned long long)rec->pairFrom, (unsigned long long)rec->pairTo, rec->pairSeconds,
				CameraModel::ReasonText(rec->reason), rec->confidence, int(rec->params.type),
				rec->params.h[0], rec->params.h[1], rec->params.h[2], rec->params.h[3], rec->params.h[4], rec->params.h[5],
				rec->params.h[6], rec->params.h[7], rec->params.h[8],
				(long long)rec->costMicros, o == PublishOutcome::Published ? 0 : 1, o == PublishOutcome::Published ? 1 : 0);
			m_trace << b;
		}
	}

	std::vector<FallbackCatalog::Decision> CameraLinkCore::CollectDecisions()
	{
		StatusFacts f;
		{
			std::lock_guard<std::mutex> lock(m_statusMutex);
			f = m_facts;
		}
		return BuildDecisions(f);
	}

	void CameraLinkCore::CollectStatus(std::vector<std::string>& lines)
	{
		if (!m_active)
			return;
		lines.push_back(StatusLine(m_counters));
		for (const FallbackCatalog::Decision& d : CollectDecisions())
			if (d.engaged)
				lines.push_back(d.reason);
		lines.push_back("camera: text guard: none (no producer yet)");
		if (m_producer)
		{
			const PairTiming& t = m_producer->Timing();
			lines.push_back(std::string("camera: pair timing from ") + Text(t.Basis()));
		}
	}

	std::shared_ptr<const CameraModel::CameraMotion> CameraLinkCore::SealedRecord(uint64_t seq) const
	{
		if (!m_pool || m_sealedMark.empty() || m_sealedMark[seq % m_sealedMark.size()] != seq)
			return nullptr;
		PoolEntry e;
		return m_pool->Get(seq, e) ? e.record : nullptr;
	}

	std::shared_ptr<const CameraModel::CameraMotion> CameraLinkCore::CausalRecord(uint64_t seq) const
	{
		PoolEntry e;
		return m_pool && m_pool->Get(seq, e) ? e.record : nullptr;
	}

	std::shared_ptr<const FieldCopy> CameraLinkCore::FieldOf(uint64_t seq) const
	{
		PoolEntry e;
		return m_pool && m_pool->Get(seq, e) ? e.field : nullptr;
	}

	bool CameraLinkCore::LabelsFor(uint64_t seq, CameraConsumer c, CameraLabelsView& out)
	{
		out = CameraLabelsView();
		return labelsProvider ? labelsProvider(seq, c, out) : false;
	}

	std::shared_ptr<const CameraModel::GuidanceFrame> CameraLinkCore::GuideFor(uint64_t seq)
	{
		return guideProvider ? guideProvider(seq) : nullptr;
	}
}
