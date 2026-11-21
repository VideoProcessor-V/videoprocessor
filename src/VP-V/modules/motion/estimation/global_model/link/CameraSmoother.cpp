// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraSmoother.h"

#include <algorithm>

namespace CameraLink
{
	namespace
	{
		// Keep enough causal records to rebuild the +-kMaxCameraLead smoother window and answer recent queries.
		constexpr size_t kRetainedRecords = static_cast<size_t>(VpSkeleton::kMaxCameraLead) * 4u + 16u;
	}

	size_t EncodeSmoothedPod(const CameraModel::CameraMotion& m, uint8_t effectiveLead, uint8_t* out, size_t cap)
	{
		if (!out || cap == 0)
			return 0;
		detail::Writer w(out, cap < kSmoothedPodMaxBytes ? cap : kSmoothedPodMaxBytes);
		w.Put(kSmoothedPodMagic);
		w.Put(m.seq);
		w.Put(m.pairSeconds);
		w.Put(uint8_t(m.valid ? 1 : 0));
		w.Put(uint8_t(m.smoothed ? 1 : 0));
		w.Put(effectiveLead);
		w.Put(uint8_t(m.reason));
		w.Put(m.verdictFlags);
		const CameraModel::Params& active = ActiveParams(m);
		w.Put(uint8_t(active.type));
		for (int i = 0; i < 9; ++i)
			w.Put(active.h[i]);
		w.Put(m.confidence);
		w.Put(int32_t(m.picture.left));
		w.Put(int32_t(m.picture.top));
		w.Put(int32_t(m.picture.right));
		w.Put(int32_t(m.picture.bottom));
		return w.ok ? w.n : 0;
	}

	bool DecodeSmoothedPod(const uint8_t* in, size_t bytes, CameraModel::CameraMotion& m, uint8_t* effectiveLeadOut)
	{
		if (!in || bytes == 0)
			return false;
		detail::Reader r(in, bytes);
		if (r.Get<uint32_t>() != kSmoothedPodMagic)
			return false;
		CameraModel::CameraMotion x;
		x.seq = r.Get<uint64_t>();
		x.pairSeconds = r.Get<double>();
		x.valid = r.Get<uint8_t>() != 0;
		x.smoothed = r.Get<uint8_t>() != 0;
		const uint8_t lead = r.Get<uint8_t>();
		const uint8_t reason = r.Get<uint8_t>();
		const uint8_t verdictFlags = r.Get<uint8_t>();
		const uint8_t type = r.Get<uint8_t>();
		if (reason > kRecordPodLastReason || type > kRecordPodLastModelType)
			return false;
		x.reason = static_cast<CameraModel::Reason>(reason);
		x.verdictFlags = verdictFlags;
		CameraModel::Params active;
		active.type = static_cast<CameraModel::ModelType>(type);
		for (int i = 0; i < 9; ++i)
			active.h[i] = r.Get<double>();
		x.confidence = r.Get<double>();
		x.picture.left = r.Get<int32_t>();
		x.picture.top = r.Get<int32_t>();
		x.picture.right = r.Get<int32_t>();
		x.picture.bottom = r.Get<int32_t>();
		if (!r.ok)
			return false;
		x.params = active;
		if (x.smoothed)
			x.smoothedParams = active;
		m = x;
		if (effectiveLeadOut)
			*effectiveLeadOut = lead;
		return true;
	}

	CameraSmoother::CameraSmoother(int laneLead)
		: m_effectiveLead(EffectiveCameraLead(laneLead))
		, m_smoother(m_effectiveLead)
	{
	}

	void CameraSmoother::SetLaneLead(int laneLead)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const int nextLead = EffectiveCameraLead(laneLead);
		if (nextLead == m_effectiveLead)
			return;
		m_effectiveLead = nextLead;
		RebuildSmootherLocked();
	}

	int CameraSmoother::EffectiveLead() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_effectiveLead;
	}

	void CameraSmoother::Reset()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_smoother.Reset();
		m_records.clear();
		m_counters.resets.fetch_add(1, std::memory_order_relaxed);
	}

	void CameraSmoother::RebuildSmootherLocked()
	{
		m_smoother = CameraModel::FixedLagSmoother(m_effectiveLead);
		for (const std::shared_ptr<const CameraModel::CameraMotion>& r : m_records)
			if (r)
				m_smoother.Push(*r);
	}

	void CameraSmoother::Push(const std::shared_ptr<const CameraModel::CameraMotion>& record)
	{
		if (!record)
			return;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_records.empty() && record->seq <= m_records.back()->seq)
		{
			// Non-monotonic sequence (seek / restart / rebuild): reset cleanly before pushing.
			m_smoother.Reset();
			m_records.clear();
			m_counters.resets.fetch_add(1, std::memory_order_relaxed);
		}
		m_records.push_back(record);
		while (m_records.size() > kRetainedRecords)
			m_records.pop_front();
		m_smoother.Push(*record);
		m_counters.pushed.fetch_add(1, std::memory_order_relaxed);
	}

	bool CameraSmoother::PushFromP7Slot(const VpSkeleton::LaneLease& p7Lease, uint64_t seq)
	{
		if (!p7Lease.table || !p7Lease.bytes)
			return false;
		VpSkeleton::SlotSet* set = p7Lease.table->Find(VpSkeleton::kP7CameraFit);
		if (!set)
			return false;
		const VpSkeleton::SlotState st = set->StateOf(seq);
		if (st != VpSkeleton::SlotState::Published && st != VpSkeleton::SlotState::Sealed)
			return false;
		const uint16_t slot = set->SlotOf(seq);
		const uint8_t* raw = p7Lease.bytes->Ring(static_cast<size_t>(slot));
		if (!raw)
			return false;
		auto rec = std::make_shared<CameraModel::CameraMotion>();
		if (!DecodeRecordPod(raw, p7Lease.bytes->Bytes(), *rec))
			return false;
		Push(rec);
		return true;
	}

	bool CameraSmoother::Query(uint64_t seq, SmoothQueryResult& out) const
	{
		out = SmoothQueryResult();
		std::lock_guard<std::mutex> lock(m_mutex);
		out.effectiveLead = m_effectiveLead;
		std::shared_ptr<const CameraModel::CameraMotion> causal;
		for (const std::shared_ptr<const CameraModel::CameraMotion>& r : m_records)
		{
			if (r && r->seq == seq)
			{
				causal = r;
				break;
			}
		}
		if (!causal)
		{
			m_counters.missing.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (m_effectiveLead <= 0)
		{
			// Queue / lane depth 0: causal P7 is served with visible reduced-mode status.
			out.record = causal;
			out.smoothed = false;
			out.causalShortQueue = true;
			m_counters.causalShortQueue.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		CameraModel::Params sp;
		if (m_smoother.Smoothed(seq, sp))
		{
			auto rec = std::make_shared<CameraModel::CameraMotion>(*causal);
			rec->smoothed = true;
			rec->smoothedParams = sp;
			out.record = std::move(rec);
			out.smoothed = true;
			m_counters.smoothed.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		// Future neighbours N+1..N+k have not arrived yet (or causal record invalid): serve causal P7 visibly, never block.
		out.record = causal;
		out.smoothed = false;
		out.causalWindowShort = true;
		m_counters.causalWindowShort.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool CameraSmoother::PublishSlot(const VpSkeleton::LaneLease& p9Lease, uint64_t seq, const VpIndicator::Stamp& fallbackStamp) const
	{
		auto refuse = [&]() {
			m_counters.slotRefused.fetch_add(1, std::memory_order_relaxed);
			return false;
		};
		if (!p9Lease.table)
			return refuse();
		VpSkeleton::SlotSet* set = p9Lease.table->Find(VpSkeleton::kP9CameraSmoothed);
		if (!set)
			return refuse();
		SmoothQueryResult q;
		if (!Query(seq, q) || !q.record)
			return refuse();
		VpSkeleton::LaneTicket ticket;
		if (set->Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &ticket) != VpSkeleton::ClaimResult::Claimed)
			return refuse();
		if (!set->Writable(ticket))
		{
			set->Abandon(ticket);
			return refuse();
		}
		VpAncestry::Header& hdr = set->LaneHeader(ticket);
		VpSkeleton::LaneInputs pins;
		if (!p9Lease.table->LaneAncestors(VpSkeleton::kP9CameraSmoothed, seq, hdr, pins))
		{
			pins.Unpin();
			set->Abandon(ticket);
			return refuse();
		}
		hdr.honoured = p9Lease.indicator ? p9Lease.indicator->StampOf(seq) : fallbackStamp;
		if (p9Lease.bytes)
		{
			uint8_t* dst = p9Lease.bytes->Ring(static_cast<size_t>(ticket.slot));
			if (dst && EncodeSmoothedPod(*q.record, static_cast<uint8_t>(q.effectiveLead), dst, p9Lease.bytes->Bytes()) == 0)
			{
				pins.Unpin();
				set->Abandon(ticket);
				return refuse();
			}
		}
		const VpSkeleton::PublishResult pr = set->Publish(ticket);
		pins.Unpin();
		if (pr != VpSkeleton::PublishResult::Published)
			return refuse();
		m_counters.slotPublished.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
}
