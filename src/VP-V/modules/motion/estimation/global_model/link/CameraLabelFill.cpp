// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraLabelFill.h"

namespace CameraLink
{
	CameraLabelFill::CameraLabelFill(VpSkeleton::SlotTable& table, ICameraLink& link, ILabelDevice& device,
		VpIndicator::Indicator* indicator)
		: m_table(table), m_link(link), m_device(device), m_indicator(indicator), m_gap(*this)
	{
	}

	// The next frame the lane can produce: P7 published (its record is in the pool), P8 slot not yet used, not below the floor.
	bool CameraLabelFill::NextSeq(uint64_t* out) const
	{
		VpSkeleton::SlotSet* p8 = m_table.Find(VpSkeleton::kP8CameraLabels);
		VpSkeleton::SlotSet* p7 = m_table.Find(VpSkeleton::kP7CameraFit);
		if (!p8 || !p7)
			return false;
		const uint64_t start = m_cursor > m_floor ? m_cursor : m_floor;
		for (uint64_t s = start; s < start + p8->Ring(); ++s)
		{
			if (p7->StateOf(s) != VpSkeleton::SlotState::Published && p7->StateOf(s) != VpSkeleton::SlotState::Sealed)
				continue;
			if (p8->StateOf(s) != VpSkeleton::SlotState::Empty && p8->StateOf(s) != VpSkeleton::SlotState::Released)
				continue;
			if (!m_link.CausalRecord(s))
				continue;
			if (out)
				*out = s;
			return true;
		}
		return false;
	}

	void CameraLabelFill::RunLane()
	{
		uint64_t seq = 0;
		if (!NextSeq(&seq))
			return;
		VpSkeleton::SlotSet* p8 = m_table.Find(VpSkeleton::kP8CameraLabels);
		m_cursor = seq + 1;
		VpSkeleton::LaneTicket ticket;
		if (p8->Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &ticket) != VpSkeleton::ClaimResult::Claimed)
		{
			m_counters.laneBlocked.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		VpAncestry::Header& hdr = p8->LaneHeader(ticket);
		VpSkeleton::LaneInputs pins;
		if (!p8->Writable(ticket) || !m_table.LaneAncestors(VpSkeleton::kP8CameraLabels, seq, hdr, pins))
		{
			p8->Abandon(ticket);
			m_counters.laneBlocked.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		hdr.honoured = m_indicator ? m_indicator->StampOf(seq) : VpIndicator::Stamp();
		const std::shared_ptr<const CameraModel::CameraMotion> rec = m_link.CausalRecord(seq);
		const std::shared_ptr<const FieldCopy> field = m_link.FieldOf(seq);
		LabelTarget target;
		target.live = false;
		target.index = ticket.slot;
		if (!rec || !m_device.Produce(*rec, field.get(), target))
		{
			pins.Unpin();
			p8->Abandon(ticket);
			m_counters.laneBlocked.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const VpSkeleton::PublishResult pr = p8->Publish(ticket);
		pins.Unpin();
		(pr == VpSkeleton::PublishResult::Published ? m_counters.lane : m_counters.laneLate).fetch_add(1, std::memory_order_relaxed);
	}

	bool CameraLabelFill::FillLive(uint64_t seq, int liveIndex, VpAncestry::Header& hdr)
	{
		// The live form needs the SEALED P7: a late or rejected fit leaves P8 absent (enabled = 0), never a live fit.
		// P7 is resolved before P8 within the same seal (topological order), so its slot already says Sealed (a lane result the
		// seal accepted): Read().kind == Ring. The record itself comes from the pool.
		VpSkeleton::SlotSet* p7 = m_table.Find(VpSkeleton::kP7CameraFit);
		const bool p7Sealed = p7 && p7->Read(seq).kind == VpSkeleton::ReadInfo::Ring;
		const std::shared_ptr<const CameraModel::CameraMotion> rec = p7Sealed ? m_link.CausalRecord(seq) : nullptr;
		if (!rec)
		{
			m_counters.declined.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		LabelTarget target;
		target.live = true;
		target.index = liveIndex;
		if (!m_device.Produce(*rec, m_link.FieldOf(seq).get(), target))
		{
			m_counters.declined.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		(void)hdr;
		m_counters.live.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool CameraLabelFill::LabelsFor(uint64_t seq, CameraConsumer c, CameraLabelsView& out)
	{
		(void)c;
		out = CameraLabelsView();
		VpSkeleton::SlotSet* p8 = m_table.Find(VpSkeleton::kP8CameraLabels);
		if (!p8)
		{
			m_counters.absent.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		const VpSkeleton::ReadInfo r = p8->Read(seq);
		LabelTarget t;
		if (r.kind == VpSkeleton::ReadInfo::Ring)
			t.live = false;
		else if (r.kind == VpSkeleton::ReadInfo::Live)
			t.live = true;
		else
		{
			m_counters.absent.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		t.index = r.index;
		const pl_tex tex = m_device.TextureOf(t);
		const std::shared_ptr<const CameraModel::CameraMotion> rec = m_link.CausalRecord(seq);
		if (!tex || !rec || !rec->blocks)
		{
			m_counters.absent.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		out.available = true;
		out.tex = tex;
		out.gridW = rec->blocks->gridW;
		out.gridH = rec->blocks->gridH;
		m_counters.served.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
}
