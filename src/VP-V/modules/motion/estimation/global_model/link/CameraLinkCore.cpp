// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraLinkCore.h"

#include "CameraDump.h"
#include "CameraUsable.h"

#include <VP-V/core/PassTimerInbox.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace CameraLink
{
	namespace
	{
		constexpr double kHoldSeconds = 0.5;   // same measured interval as the ambiguity engage time
		constexpr uint64_t kMaxFieldsPerPump = 16;
		constexpr uint64_t kSeedPendingFrames = 16;   // AHEAD FIT: a seed fallback copy not back after this many frames is given up
		constexpr size_t kAheadQueue = 16;            // AHEAD FIT: the fit queue (the P2 slot holds 16 pairs: the deepest lead)
		constexpr size_t kAheadPool = 32;             // AHEAD FIT: records live from the ahead submit to the lag bridge's seal
		// The stamp counters P7's content depends on (core/Products.h: content = full + continuity, settings = motion).
		const VpIndicator::CounterMask kP7StampMask =
			VpIndicator::Bit(VpIndicator::kFull) | VpIndicator::Bit(VpIndicator::kContinuity) | VpIndicator::Bit(VpIndicator::kSettingsMotion);

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
		for (size_t i = 0; i < kAheadMarks; ++i)
			m_onTimeMark[i].store(0, std::memory_order_relaxed);
		m_retired.assign(kAheadMarks, 0);
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
		m_requestedAt.clear();
		{
			std::lock_guard<std::mutex> lock(m_chainMutex);
			m_chain.clear();
		}
		m_lastRequestedField = 0;
		m_aheadNext = 0;
		m_aheadRefusedSeq = 0;
		m_seedPending.clear();
		for (size_t i = 0; i < kAheadMarks; ++i)
			m_onTimeMark[i].store(0, std::memory_order_relaxed);
		m_retired.assign(kAheadMarks, 0);
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
		AttachLease(VpSkeleton::LaneLease::Borrow(table, p7Bytes, indicator), VpSkeleton::LaneLeaseSource());
	}

	void CameraLinkCore::AttachLane(VpSkeleton::LaneLeaseSource source)
	{
		const VpSkeleton::LaneLease first = source ? source() : VpSkeleton::LaneLease();
		AttachLease(first, std::move(source));
	}

	void CameraLinkCore::OnLaneRebound()
	{
		// A rebuilt graph can size the P7 ring differently: the seal marks follow it (the pool keeps its own capacity).
		const VpSkeleton::LaneLease l = m_worker ? m_worker->Lease() : VpSkeleton::LaneLease();
		VpSkeleton::SlotSet* set = l.table ? l.table->Find(VpSkeleton::kP7CameraFit) : nullptr;
		if (set && set->Ring() != m_sealedMark.size())
			m_sealedMark.assign(set->Ring(), ~uint64_t(0));
	}

	void CameraLinkCore::AttachLease(const VpSkeleton::LaneLease& first, VpSkeleton::LaneLeaseSource source)
	{
		Teardown();
		// The measured costs belong to the graph that made them: a rebuild starts a fresh window (the caller derives d from
		// the old window BEFORE it attaches the new graph).
		m_lagMeter.Clear();
		VpSkeleton::SlotTable* table = first.table;
		if (!m_active || !table || !table->Find(VpSkeleton::kP7CameraFit))
			return;
		const size_t ring = table->Find(VpSkeleton::kP7CameraFit)->Ring();
		const bool ahead = AheadActive();
		m_pool.reset(new CameraRecordPool((std::max)((std::max)(static_cast<size_t>(ring), m_settings.poolRing), ahead ? kAheadPool : size_t(0))));
		m_sealedMark.assign(ring, ~uint64_t(0));
		m_producer.reset(new CameraFitProducer(m_settings.fit, &m_counters));
		CameraFitEnv env;
		env.lease = first;
		env.leaseSource = std::move(source);
		env.pool = m_pool.get();
		env.counters = &m_counters;
		env.queueCapacity = ahead ? (std::max)(m_settings.queueCapacity, kAheadQueue) : m_settings.queueCapacity;
		env.aheadMode = ahead;
		env.onTimeSource = ahead ? m_onTimeSource : VpSkeleton::LaneLeaseSource();
		env.p2Epoch = ahead ? m_p2Epoch : std::function<uint64_t()>();
		env.aheadGlobal = ahead ? m_aheadGlobal : std::function<void(AheadCopy&)>();
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
		const auto now = std::chrono::steady_clock::now();
		for (const std::shared_ptr<FieldCopy>& fc : ready)
		{
			// The readback latency (request -> ready) is one of the two measured terms of the guide lag d.
			const auto at = m_requestedAt.find(fc->seq);
			if (at != m_requestedAt.end())
			{
				const double latency = std::chrono::duration<double>(now - at->second).count();
				m_lagMeter.AddReadbackSeconds(latency);
				if (latency > 0)
					PassTimers::Inbox::Motion().PostCameraReadback(latency * 1000.0);   // camera.readback: a wait, never counted as work
				m_requestedAt.erase(at);
			}
			if (AheadActive())
			{
				// AHEAD FIT: a seed fallback copy is back; the ahead submissions that waited for it (fit order) continue.
				const auto pending = std::find(m_seedPending.begin(), m_seedPending.end(), fc->seq);
				if (pending != m_seedPending.end())
				{
					m_seedPending.erase(pending);
					if (SubmitField(fc, f))
						m_counters.aheadSeedFallback.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
			}
			SubmitField(fc, f);
		}
	}

	bool CameraLinkCore::SubmitAheadField(std::shared_ptr<const FieldCopy> field, const AheadCopy* global)
	{
		if (!m_worker || !field || field->origin != FieldOrigin::Ahead)
			return false;
		CameraFitJob job;
		job.seq = field->seq;
		job.field = field;
		job.stamp = field->stamp;
		job.ahead = true;
		job.outGridW = m_aheadGridW;   // the record's outputs on the consumer grid; the fit runs on the native cells
		job.outGridH = m_aheadGridH;
		PairDescription d;
		if (m_aheadFacts && !m_aheadFacts(*field, d))
			return false;
		job.facts = d.facts;
		job.uniqueFramesBetween = d.uniqueFramesBetween;
		job.haveTimestamps = d.haveTimestamps;
		job.tsFrom = d.tsFrom;
		job.tsTo = d.tsTo;
		job.facts.deadlineMicros = 0;   // as every job: no deadline in the Fitter (the history stays deterministic)
		if (global && global->globalValid && !job.facts.haveEngineGlobal)
		{
			job.facts.haveEngineGlobal = true;   // the ahead pair's own whole-frame motion (the render-time EngineGlobal does not exist yet)
			job.facts.engineGx = global->globalX;
			job.facts.engineGy = global->globalY;
		}
		if (!m_worker->Submit(job))
			return false;
		m_counters.aheadSubmitted.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool CameraLinkCore::SubmitAheadCopy(std::shared_ptr<const AheadCopy> copy)
	{
		if (!m_worker || !copy)
			return false;
		CameraFitJob job;
		job.seq = copy->seq;
		job.stamp = copy->stamp;
		job.ahead = true;
		job.aheadCopy = copy;
		job.aheadConsistency = m_aheadConsistency;
		job.outGridW = m_aheadGridW;
		job.outGridH = m_aheadGridH;
		PairDescription d;
		if (m_aheadFacts)
		{
			FieldCopy header;   // the pair's frames only: the facts read the frame-analysis store, not the vectors
			header.seq = copy->seq;
			header.pairFrom = copy->pairFrom;
			header.pairTo = copy->pairTo;
			header.origin = FieldOrigin::Ahead;
			if (!m_aheadFacts(header, d))
				return false;
		}
		job.facts = d.facts;
		job.uniqueFramesBetween = d.uniqueFramesBetween;
		job.haveTimestamps = d.haveTimestamps;
		job.tsFrom = d.tsFrom;
		job.tsTo = d.tsTo;
		job.facts.deadlineMicros = 0;
		if (!m_worker->Submit(job))
			return false;
		m_counters.aheadSubmitted.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool CameraLinkCore::Retired(uint64_t seq) const
	{
		return !m_retired.empty() && m_retired[seq % m_retired.size()] == seq + 1;
	}

	void CameraLinkCore::MarkRetired(uint64_t seq)
	{
		if (!m_retired.empty())
			m_retired[seq % m_retired.size()] = seq + 1;
	}

	void CameraLinkCore::PollAhead()
	{
		if (!AheadActive() || !m_worker || !m_pool)
			return;
		// A seed fallback is in flight: its frame must be fitted before any later ahead pair (the fit history is in frame order).
		if (!m_seedPending.empty())
			return;
		if (m_aheadNext == 0)
		{
			// Start after everything the render-time path already asked for: no frame is fitted twice.
			const uint64_t latest = m_engine ? m_engine->LatestVerifiedFieldSeq() : 0;
			m_aheadNext = (std::max)((std::max)(m_renderSeq, latest), m_lastRequestedField) + 1;
		}
		const VpSkeleton::LaneLease lease = m_worker->Lease();
		for (uint64_t k = 0; k < kMaxFieldsPerPump; ++k)
		{
			if (m_worker->Pending() >= kAheadQueue)
				break;   // back-pressure: ask again at the next phase (never a drop)
			const uint64_t seq = m_aheadNext;
			AheadCopy copy;
			const AheadRefusal r = m_aheadSource(seq, copy);
			if (r == AheadRefusal::NoPair)
				break;   // not computed yet: normal until the producer gets there; a frame the render reaches without one takes the seed
			if (r != AheadRefusal::None)
			{
				if (m_aheadRefusedSeq != seq)
				{
					m_aheadRefusedSeq = seq;   // counted once per frame; the frame takes the seed when the render reaches it
					(r == AheadRefusal::Incomplete ? m_counters.aheadIncomplete : r == AheadRefusal::EpochMoved ? m_counters.aheadEpochRace :
						m_counters.aheadLayout).fetch_add(1, std::memory_order_relaxed);
				}
				break;
			}
			// Audit F3: only the copy (and the epoch read) happen on the render thread; the conversion, the trust and the global run on
			// the camera worker.
			copy.stamp = lease.indicator ? lease.indicator->StampOf(seq) : VpIndicator::Stamp();
			if (!SubmitAheadCopy(std::make_shared<const AheadCopy>(std::move(copy))))
				break;   // facts refused or the queue is full (counted by the worker): the render-time path takes the frame
			{
				std::lock_guard<std::mutex> lock(m_chainMutex);
				if (m_chain.empty() || seq > m_chain.back())
				{
					if (m_chain.size() >= 64)
						m_chain.erase(m_chain.begin());
					m_chain.push_back(seq);
				}
			}
			++m_aheadNext;
		}
	}

	void CameraLinkCore::OnTimeSeal(uint64_t seq)
	{
		if (!AheadActive() || !m_onTimeSource || !m_pool)
			return;
		// Only the camera publishes P7 into the on-time target (it is declared published outside the lane, so the seal never live-fills
		// it): a Sealed P7(seq) there is this link's on-time record. The mark (set by the worker after its publish) catches a publish the
		// seal REJECTED; a mark still in flight when the seal accepted is covered by the Sealed state itself.
		const VpSkeleton::LaneLease l = m_onTimeSource();
		VpSkeleton::SlotSet* set = l.table ? l.table->Find(VpSkeleton::kP7CameraFit) : nullptr;
		bool sealed = set && set->StateOf(seq) == VpSkeleton::SlotState::Sealed;
		if (!sealed && m_onTimeMark[seq % kAheadMarks].load(std::memory_order_acquire) != seq + 1)
			return;   // nothing was published on time for this frame: the lag bridge seals it
		PoolEntry e;
		const bool have = m_pool->Get(seq, e, true);
		// Re-check at the seal: the stamp honoured at the publish against the frame's stamp now (a resync after the publish rejects it).
		VpIndicator::Indicator* ind = l.indicator ? l.indicator : (m_worker ? m_worker->Lease().indicator : nullptr);
		if (sealed && have && e.haveHonoured && ind && !VpIndicator::SameFor(e.honoured, ind->StampOf(seq), kP7StampMask))
			sealed = false;
		if (sealed && have && P2Stale(e))
		{
			sealed = false;   // G-A3 at the seal: the P2 epoch moved since FlowOf served the pair
			m_counters.p2RefusedSeal.fetch_add(1, std::memory_order_relaxed);
		}
		if (sealed && have)
		{
			const bool accepted = m_sealToStore.OnSealed(seq, e.record);
			if (accepted && !m_sealedMark.empty())
				m_sealedMark[seq % m_sealedMark.size()] = seq;
			(accepted ? m_counters.sealedOk : m_counters.sealedLate).fetch_add(1, std::memory_order_relaxed);
			m_counters.onTimeSealed.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			m_counters.onTimeRejected.fetch_add(1, std::memory_order_relaxed);
			m_pool->Erase(seq);   // never served
		}
		MarkRetired(seq);   // the lag bridge's later seal of this frame is not a second verdict
	}

	bool CameraLinkCore::PreviousRecordSeq(uint64_t seq, unsigned back, uint64_t& out) const
	{
		if (back == 0)
			return false;
		std::lock_guard<std::mutex> lock(m_chainMutex);
		const size_t k = static_cast<size_t>(std::lower_bound(m_chain.begin(), m_chain.end(), seq) - m_chain.begin());
		if (k < back)
			return false;
		out = m_chain[k - back];
		return true;
	}

	uint64_t CameraLinkCore::LatestRecordSeq() const
	{
		std::lock_guard<std::mutex> lock(m_chainMutex);
		return m_chain.empty() ? 0 : m_chain.back();
	}

	void CameraLinkCore::SetFitDoneObserver(std::function<void(uint64_t fitSeq)> f)
	{
		// Called from inside the observer it would wait for itself: the lock rule (CameraLinkCore.h) forbids it.
		assert(m_observerThread.load() != std::this_thread::get_id());
		std::lock_guard<std::mutex> lock(m_observerMutex);
		m_fitDone = std::move(f);
	}

	bool CameraLinkCore::SubmitField(std::shared_ptr<const FieldCopy> field, const FramePhaseFacts& f)
	{
		(void)f;
		if (!m_worker || !field)
			return false;
		// F2 provenance: the GUIDED live matcher field is never fitted (the fit would re-measure its own guide). Seed, Ahead (the
		// producer's unguided NVOF pair, AheadFlow.h) and Synthetic are accepted.
		if (field->origin == FieldOrigin::Live)
		{
			m_counters.guidedRefused.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
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
		if (m_worker && m_worker->Rebind())   // the lane was rebuilt: take the new lease (and size the seal marks)
			OnLaneRebound();
		const bool ahead = AheadActive();
		m_renderSeq = f.renderSeq != 0 ? f.renderSeq : f.seq;
		// (0) AHEAD FIT: the on-time seal of the frame starting now (its P7 was published into the on-time target before the seal).
		if (ahead)
		{
			const bool usable = m_onTimeSource && m_onTimeSource().table != nullptr;
			{
				std::lock_guard<std::mutex> lock(m_statusMutex);
				m_onTimeUsable = usable;
			}
			OnTimeSeal(m_renderSeq);
		}
		// (1) seal observer (the lag bridge's seal of f.seq; a frame the on-time seal already decided is skipped)
		if (!(ahead && Retired(f.seq)))
		{
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
		}
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
				m_requestedAt.clear();
				m_aheadNext = 0;          // AHEAD FIT: restarts after the new numbering's newest field
				m_seedPending.clear();
				std::lock_guard<std::mutex> lock(m_chainMutex);
				m_chain.clear();   // the numbering restarted: the old records are not "previous" any more
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
				// AHEAD FIT: a frame the ahead fit already took needs no seed readback.
				if (ahead && m_aheadNext != 0 && s < m_aheadNext)
					continue;
				// F2: the fit reads the optical-flow SEED only (never FieldOf, the guided live field).
				VpMotion::SeedField vf;
				if (!m_engine->SeedFieldOf(s, vf))
				{
					if (s == latest)
						m_counters.seedAbsent.fetch_add(1, std::memory_order_relaxed);   // the engine has the pair but no seed
					if (ahead && m_aheadNext != 0 && s >= m_aheadNext)
						m_aheadNext = s + 1;   // nothing to fit for s (a cadence repeat): the ahead fit goes on after it
					continue;   // no seed for this seq (a cadence repeat, or no seed: the camera_fit fallback): not a drop
				}
				bool seedFallback = false;
				if (ahead)
				{
					SetAheadConsistency(vf.consistencyPx);   // the matcher's own limit, as the ahead trust judges it
					if (m_aheadNext != 0 && s >= m_aheadNext)
					{
						// The render reached s without an ahead pair (low latency, a producer drop, a pair not computed in time): the seed
						// takes s (the bridge path), visibly; later ahead pairs wait until its fit is submitted (fit order).
						m_counters.aheadNoPair.fetch_add(1, std::memory_order_relaxed);
						m_aheadNext = s + 1;
						m_events.fetch_or(1u << kEvAhead, std::memory_order_relaxed);
						seedFallback = true;
					}
				}
				if (seedFallback && m_ring->Request(vf))
					m_seedPending.push_back(s);   // the copy is on the way: the ahead submissions wait for its fit
				else if (seedFallback)
				{
					m_counters.readbackDropped.fetch_add(1, std::memory_order_relaxed);
					m_events.fetch_or(1u << kEvReadback, std::memory_order_relaxed);
					if (m_producer)
						m_producer->NoteReset();   // a hole in the pair chain
					continue;
				}
				if (!seedFallback && !m_ring->Request(vf))
				{
					m_counters.readbackDropped.fetch_add(1, std::memory_order_relaxed);
						m_events.fetch_or(1u << kEvReadback, std::memory_order_relaxed);
					if (m_producer)
						m_producer->NoteReset();   // a hole in the pair chain
				}
				else
				{
					{
						// The record chain: this field WILL be a record (its copy is on the way), whatever its fit does later.
						std::lock_guard<std::mutex> lock(m_chainMutex);
						if (m_chain.empty() || vf.seq > m_chain.back())
						{
							if (m_chain.size() >= 64)
								m_chain.erase(m_chain.begin());
							m_chain.push_back(vf.seq);
						}
					}
					if (m_requestedAt.size() >= 64)
						m_requestedAt.erase(m_requestedAt.begin());   // bounded: a copy that never came back is not timed
					m_requestedAt[vf.seq] = now;
				}
			}
			if (latest > m_lastRequestedField)
				m_lastRequestedField = latest;
		}
		if (ahead)
		{
			// AHEAD FIT: a seed fallback copy that never came back (a dropped readback) does not hold the ahead fit for ever.
			for (auto it = m_seedPending.begin(); it != m_seedPending.end();)
				it = *it + kSeedPendingFrames < m_renderSeq ? m_seedPending.erase(it) : it + 1;
			PollAhead();
			if (m_worker)
				m_worker->Tick(m_renderSeq);   // the parked results see the new render position (the bridge's markers of it exist now)
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
		if (!m_active)
			return;
		if (m_ring)
		{
			std::vector<std::shared_ptr<FieldCopy>> ready;
			m_ring->Poll(ready);
			FramePhaseFacts none;
			SubmitReady(ready, none);
		}
		if (AheadActive())
		{
			// AHEAD FIT: after the render the on-time target's inputs of the next frame exist (the skeleton feeds them at AfterRender):
			// the parked results try again, and pairs the producer finished meanwhile are taken.
			PollAhead();
			if (m_worker)
				m_worker->Tick(m_renderSeq);
		}
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
				m_lastFit.have = true;
				m_lastFit.type = rec->params.type;
				m_lastFit.confidence = rec->confidence;
				m_lastFit.fitMs = double(rec->costMicros) / 1000.0;
				m_lastFit.reason = rec->reason;
				m_lastFit.verdictFlags = rec->verdictFlags;
				m_lastFit.published = IsPublished(o);
			}
			if (r.timingUnknown)
				m_events.fetch_or(1u << kEvTiming, std::memory_order_relaxed);
			if (r.status == AssembleStatus::NotFittedSlip || r.status == AssembleStatus::NotFittedAncient)
				m_events.fetch_or(1u << kEvSlip, std::memory_order_relaxed);
		}
		if (job.ahead)
		{
			// AHEAD FIT: where the result went. On time: the render thread's on-time seal reads the mark at the frame's Begin.
			if (o == PublishOutcome::PublishedOnTime)
				m_onTimeMark[job.seq % kAheadMarks].store(job.seq + 1, std::memory_order_release);
			else
				m_events.fetch_or(1u << kEvAhead, std::memory_order_relaxed);   // bridged or late: the frame missed the on-time seal
		}
		{
			std::shared_ptr<CameraDump> dump;
			{
				std::lock_guard<std::mutex> lock(m_dumpMutex);
				dump = m_dump;
			}
			if (dump)
				dump->Offer(job.field, r.record);   // after Fit: a bounded push that drops visibly, never a wait
		}
		if (rec && rec->costMicros > 0)
		{
			m_lagMeter.AddFitSeconds(static_cast<double>(rec->costMicros) * 1e-6);
			PassTimers::Inbox::Motion().PostCameraFit(static_cast<double>(rec->costMicros) * 1e-3);   // camera.fit: CPU work (the worker)
		}
		{
			std::lock_guard<std::mutex> lock(m_traceMutex);
			if (m_trace.is_open() && rec)
			{
				char b[512];
				std::snprintf(b, sizeof(b), "%llu,%llu,%llu,%.9g,%s,%.6g,%d,%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g,%lld,%d,%d\n",
					(unsigned long long)job.seq, (unsigned long long)rec->pairFrom, (unsigned long long)rec->pairTo, rec->pairSeconds,
					CameraModel::ReasonTextOf(rec->reason, rec->verdictFlags), rec->confidence, int(rec->params.type),
					rec->params.h[0], rec->params.h[1], rec->params.h[2], rec->params.h[3], rec->params.h[4], rec->params.h[5],
					rec->params.h[6], rec->params.h[7], rec->params.h[8],
					(long long)rec->costMicros, IsPublished(o) ? 0 : 1, IsPublished(o) ? 1 : 0);
				m_trace << b;
			}
		}
		// K4: the guide lane job. After the publish, outside the status / trace locks. The observer mutex is held for the call, so
		// SetFitDoneObserver(nullptr) (the adapter's Detach) returns only when no call is in flight: the observer's captures stay valid.
		std::lock_guard<std::mutex> lock(m_observerMutex);
		if (m_fitDone)
		{
			m_observerThread.store(std::this_thread::get_id());
			m_fitDone(job.seq);
			m_observerThread.store(std::thread::id());
		}
	}

	void CameraLinkCore::SetDump(std::shared_ptr<CameraDump> dump)
	{
		std::lock_guard<std::mutex> lock(m_dumpMutex);
		m_dump = std::move(dump);
	}

	LastFitSummary CameraLinkCore::LastFit() const
	{
		std::lock_guard<std::mutex> lock(m_statusMutex);
		return m_lastFit;
	}

	std::vector<FallbackCatalog::Decision> CameraLinkCore::CollectDecisions()
	{
		StatusFacts f;
		bool usable = false;
		{
			std::lock_guard<std::mutex> lock(m_statusMutex);
			f = m_facts;
			usable = m_onTimeUsable;
		}
		std::vector<FallbackCatalog::Decision> d = BuildDecisions(f);
		if (m_settings.aheadFit && !m_settings.deterministic && f.graphHasP7)
		{
			// AHEAD FIT (only when it is on): no usable on-time target = every frame bridged, a visible downgrade. A late frame with a target
			// is informational only (AheadLateLine in the status lines), never engaged here.
			const std::string why = "camera: ahead fit has no on-time seal target (" + std::string(kAheadNoTargetText) + ")";
			d.push_back(!usable ? FallbackCatalog::Engaged(kIdAheadNoTarget, why) : FallbackCatalog::Released(kIdAheadNoTarget));
		}
		return d;
	}

	void CameraLinkCore::CollectStatus(std::vector<std::string>& lines)
	{
		if (!m_active)
			return;
		lines.push_back(StatusLine(m_counters));
		if (m_settings.aheadFit)
		{
			lines.push_back(m_settings.deterministic ? std::string(kAheadDeterministicText)
				: AheadStatusLine(m_counters, OnTimeUsable() ? m_onTimeName : std::string(kAheadNoTargetText)));
			if (!m_settings.deterministic)
				lines.push_back(AheadLateLine(m_counters));   // G-A2: informational
		}
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
		if (!m_pool->Get(seq, e))
			return nullptr;
		if (P2Stale(e))
		{
			m_counters.p2RefusedUse.fetch_add(1, std::memory_order_relaxed);   // G-A3 at the consumer read
			return nullptr;
		}
		if (e.haveHonoured && m_worker)
		{
			// AHEAD FIT re-check at consumption: the stamp honoured at the publish must still be the frame's stamp in the Indicator of the
			// lane it was published into (a resync after the publish rejects the record here too; counted, shown in the ahead status line).
			const VpSkeleton::LaneLease l = e.onTime && m_onTimeSource ? m_onTimeSource() : m_worker->Lease();
			if (l.indicator && !VpIndicator::SameFor(e.honoured, l.indicator->StampOf(seq), kP7StampMask))
			{
				m_counters.staleAtUse.fetch_add(1, std::memory_order_relaxed);
				return nullptr;
			}
		}
		return e.record;
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
