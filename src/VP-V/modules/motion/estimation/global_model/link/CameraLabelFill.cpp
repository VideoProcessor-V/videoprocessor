// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraLabelFill.h"
#include "CameraFitFieldPod.h"

namespace CameraLink
{
	CameraLabelFill::CameraLabelFill(VpSkeleton::SlotTable& table, ICameraLink& link, ILabelDevice& device,
		VpIndicator::Indicator* indicator)
		: m_lease(VpSkeleton::LaneLease::Borrow(&table, nullptr, indicator)), m_link(link), m_device(device), m_gap(*this)
	{
	}

	CameraLabelFill::CameraLabelFill(VpSkeleton::LaneLeaseSource source, ICameraLink& link, ILabelDevice& device)
		: m_source(std::move(source)), m_link(link), m_device(device), m_gap(*this)
	{
		if (m_source)
			m_lease = m_source();
	}

	bool CameraLabelFill::Rebind()
	{
		if (!m_source)
			return false;
		VpSkeleton::LaneLease cur = m_source();
		if (!VpSkeleton::LaneLeaseNewer(cur, m_lease))
			return false;
		m_lease = std::move(cur);   // the old state dies with its last holder
		m_cursor = 0;               // the old graph's sequences
		m_counters.rebound.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool CameraLabelFill::FitFieldMatchesP7(const VpSkeleton::LaneLease& lease, uint64_t seq)
	{
		if (!lease.table)
			return false;
		VpSkeleton::SlotSet* p7 = lease.table->Find(VpSkeleton::kP7CameraFit);
		VpSkeleton::SlotSet* pf = lease.table->Find(VpSkeleton::kCameraFitField);
		if (!p7 || !pf)
			return false;   // the product is not in this graph: nothing to read
		const VpSkeleton::ReadInfo a = p7->Read(seq, true);
		const VpSkeleton::ReadInfo b = pf->Read(seq, true);
		if (a.kind != VpSkeleton::ReadInfo::Ring || b.kind != VpSkeleton::ReadInfo::Ring || !a.header || !b.header)
			return false;
		const VpIndicator::Stamp sa = a.header->honoured;
		const VpIndicator::Stamp sb = b.header->honoured;
		return VpIndicator::SameFor(sa, sb, ~VpIndicator::CounterMask(0));
	}

	bool CameraLabelFill::ReadFitField(const VpSkeleton::LaneLease& lease, uint64_t seq, FieldCopy& out) const
	{
		if (!m_fieldSource || !FitFieldMatchesP7(lease, seq))
			return false;
		const VpSkeleton::LaneLease fl = m_fieldSource();
		if (!fl.table || !fl.bytes || fl.generation != lease.generation)
			return false;
		VpSkeleton::SlotSet* pf = fl.table->Find(VpSkeleton::kCameraFitField);
		if (!pf)
			return false;
		const VpSkeleton::ReadInfo a = pf->Read(seq, true);
		if (a.kind != VpSkeleton::ReadInfo::Ring || a.index < 0)
			return false;
		const uint8_t* bytes = fl.bytes->Ring(static_cast<size_t>(a.index));
		if (!bytes || !DecodeFitFieldPod(bytes, fl.bytes->Bytes(), out))
			return false;
		// The ring may have been recycled while it was copied: the slot must still be the same published ring.
		const VpSkeleton::ReadInfo b = pf->Read(seq, true);
		return b.kind == VpSkeleton::ReadInfo::Ring && b.index == a.index && FitFieldMatchesP7(lease, seq);
	}

	// The next frame the lane can produce: P7 and its fit field product published (the record is in the pool), P8 slot not yet used, not below the floor.
	bool CameraLabelFill::NextSeq(uint64_t* out)
	{
		Rebind();
		if (!m_lease.table)
			return false;
		VpSkeleton::SlotSet* p8 = m_lease.table->Find(VpSkeleton::kP8CameraLabels);
		VpSkeleton::SlotSet* p7 = m_lease.table->Find(VpSkeleton::kP7CameraFit);
		VpSkeleton::SlotSet* pf = m_lease.table->Find(VpSkeleton::kCameraFitField);
		if (!p8 || !p7 || !pf)
			return false;
		const uint64_t start = m_cursor > m_floor ? m_cursor : m_floor;
		for (uint64_t s = start; s < start + p8->Ring(); ++s)
		{
			if (p7->StateOf(s) != VpSkeleton::SlotState::Published && p7->StateOf(s) != VpSkeleton::SlotState::Sealed)
				continue;
			if (pf->StateOf(s) != VpSkeleton::SlotState::Published && pf->StateOf(s) != VpSkeleton::SlotState::Sealed)
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
		const VpSkeleton::LaneLease lease = m_lease;   // held for the whole production: a rebuild during it cannot free the table
		VpSkeleton::SlotSet* p8 = lease.table->Find(VpSkeleton::kP8CameraLabels);
		m_cursor = seq + 1;
		VpSkeleton::LaneTicket ticket;
		if (p8->Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &ticket) != VpSkeleton::ClaimResult::Claimed)
		{
			m_counters.laneBlocked.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		VpAncestry::Header& hdr = p8->LaneHeader(ticket);
		VpSkeleton::LaneInputs pins;
		if (!p8->Writable(ticket) || !lease.table->LaneAncestors(VpSkeleton::kP8CameraLabels, seq, hdr, pins))
		{
			p8->Abandon(ticket);
			m_counters.laneBlocked.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		hdr.honoured = lease.indicator ? lease.indicator->StampOf(seq) : VpIndicator::Stamp();
		std::shared_ptr<const CameraModel::CameraMotion> rec;
		FieldCopy field;
		bool haveField = false;
		if (FitFieldMatchesP7(lease, seq))
		{
			m_link.FitEntryOf(seq, rec);                       // the record only; the field comes from the product slot
			haveField = rec && ReadFitField(lease, seq, field);
		}
		LabelTarget target;
		target.live = false;
		target.index = ticket.slot;
		if (!rec || !haveField || !m_device.Produce(*rec, &field, target))
		{
			pins.Unpin();
			p8->Abandon(ticket);
			m_counters.laneBlocked.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		// A rebuild during the production (the device call can take a while): this slot belongs to a graph that no longer is the lane's.
		Rebind();
		if (m_lease.generation != lease.generation)
		{
			pins.Unpin();
			p8->Abandon(ticket);
			m_counters.staleLane.fetch_add(1, std::memory_order_relaxed);
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
		Rebind();
		VpSkeleton::SlotSet* p7 = m_lease.table ? m_lease.table->Find(VpSkeleton::kP7CameraFit) : nullptr;
		const bool p7Sealed = p7 && p7->Read(seq).kind == VpSkeleton::ReadInfo::Ring;
		std::shared_ptr<const CameraModel::CameraMotion> rec;
		FieldCopy field;
		bool haveField = false;
		// The fit field product must be sealed too and carry P7's stamp (it is resolved before P8, like P7): else P8 stays absent (declined, counted).
		VpSkeleton::SlotSet* pf = m_lease.table ? m_lease.table->Find(VpSkeleton::kCameraFitField) : nullptr;
		if (p7Sealed && pf && pf->Read(seq).kind == VpSkeleton::ReadInfo::Ring && FitFieldMatchesP7(m_lease, seq))
		{
			m_link.FitEntryOf(seq, rec);
			haveField = rec && ReadFitField(m_lease, seq, field);   // content from the product slot, stamp-gated; else no labels
		}
		if (!rec || !haveField)
		{
			m_counters.declined.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		LabelTarget target;
		target.live = true;
		target.index = liveIndex;
		if (!m_device.Produce(*rec, &field, target))
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
		Rebind();
		VpSkeleton::SlotSet* p8 = m_lease.table ? m_lease.table->Find(VpSkeleton::kP8CameraLabels) : nullptr;
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
