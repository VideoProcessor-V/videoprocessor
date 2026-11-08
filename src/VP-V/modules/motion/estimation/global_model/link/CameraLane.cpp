// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// STOPGAP - TO BE RETIRED for skeleton slots once SK-W provides live P2 / P3 / P4 content, consumer declarations and the BeforeRender seal
// (owner rule: products live in the skeleton). See CameraLane.h.
#include "CameraLane.h"

#include <VP-V/modules/motion/estimation/global_model/CameraProducts.h>

namespace CameraLink
{
	namespace
	{
		constexpr uint64_t kMaxJump = 64;       // frames the sequence may advance between two calls before the lane is rebuilt
		constexpr size_t kFieldMarks = 256;
	}

	bool CameraLane::Build(uint32_t width, uint32_t height, bool labels)
	{
		Clear();
		VpSkeleton::GraphSettings gs;
		gs.dims.width = width;
		gs.dims.height = height;
		gs.renderAhead = kSealLag;   // the rings hold history + 1 + kSealLag frames: nothing is reclaimed before its seal
		gs.laneLead = 2;
		gs.queueDepth = 16;
		gs.lookAheadWalk = 0;
		gs.ofaPresent = false;       // P2 resolves to the matcher pair P2m (the camera never starts an optical-flow engine)
		CameraDemandInput demand;
		demand.active[static_cast<int>(CameraConsumer::Measurement)] = true;   // P7 at lag 1: the fit
		demand.active[static_cast<int>(CameraConsumer::Diagnostics)] = labels; // P7 + P8: the labels
		std::vector<VpSkeleton::ConsumerDecl> consumers;
		DeclareCameraConsumers(demand, consumers);
		m_graph = VpSkeleton::BuildGraph(VpSkeleton::SkeletonProducts(), consumers, gs);
		if (!m_graph.valid)
		{
			m_error = m_graph.ErrorText();
			return false;
		}
		std::unique_ptr<VpSkeleton::SlotTable> table(new VpSkeleton::SlotTable(m_graph));
		const VpSkeleton::Node* p7 = table->NodeOf(VpSkeleton::kP7CameraFit);
		if (!p7)
		{
			m_error = "the camera graph has no P7 node";
			return false;
		}
		m_p7Bytes.reset(new VpSkeleton::CpuBuffers(*p7));
		m_table = std::move(table);
		m_width = width;
		m_height = height;
		m_labels = labels && m_table->NodeOf(VpSkeleton::kP8CameraLabels) != nullptr;
		m_fieldMark.assign(kFieldMarks, 0);
		return true;
	}

	void CameraLane::Clear()
	{
		m_fill = nullptr;
		m_table.reset();
		m_p7Bytes.reset();
		m_graph = VpSkeleton::Graph();
		m_error.clear();
		m_width = m_height = 0;
		m_labels = false;
		m_started = false;
		m_lastSeq = 0;
		m_nextToSeal = 0;
		m_markers = 0;
		m_markerFailures = 0;
		m_sealedFrames = 0;
		m_fieldMark.clear();
	}

	int CameraLane::LabelRing() const
	{
		const VpSkeleton::Node* n = m_table ? m_table->NodeOf(VpSkeleton::kP8CameraLabels) : nullptr;
		return n ? static_cast<int>(n->ring) : 0;
	}

	int CameraLane::LabelLive() const
	{
		const VpSkeleton::Node* n = m_table ? m_table->NodeOf(VpSkeleton::kP8CameraLabels) : nullptr;
		return n ? static_cast<int>(n->livePool) : 0;
	}

	void CameraLane::NoteField(uint64_t seq)
	{
		if (!m_fieldMark.empty())
			m_fieldMark[static_cast<size_t>(seq % m_fieldMark.size())] = seq + 1;
	}

	bool CameraLane::OnFrame(uint64_t seq, VpIndicator::Indicator& indicator, Sealed& out)
	{
		out = Sealed();
		if (!m_table || seq == 0)
			return true;
		if (m_started && (seq <= m_lastSeq || seq > m_lastSeq + kMaxJump))
			return false;
		if (!m_started)
		{
			m_started = true;
			// Warm-up: the frame before the first one gets its markers too (tolerating the failures its lag-1 inputs cause), so the first
			// real frame finds its lag-1 ancestors (P2m reads P1 at lag 1). It is sealed and released like any frame, or its ring slot
			// would never come back.
			const uint64_t warm = seq - 1;
			PublishMarkers(warm, indicator, true);
			m_nextToSeal = warm;
		}
		m_lastSeq = seq;
		PublishMarkers(seq, indicator, false);
		const uint64_t lag = kSealLag;
		while (m_nextToSeal + lag <= seq)
		{
			SealOne(m_nextToSeal, indicator, out);
			++m_nextToSeal;
		}
		return true;
	}

	// Every node but P7..P9, in topological order, as a lane producer would publish it: the ancestors recorded, the honoured stamp
	// read AFTER the pins. The slot content is empty (see the header): what it carries is the ancestry the seal checks.
	void CameraLane::PublishMarkers(uint64_t seq, VpIndicator::Indicator& indicator, bool warmUp)
	{
		for (size_t i = 0; i < m_table->Count(); ++i)
		{
			const VpSkeleton::Node& n = m_table->NodeAt(i);
			if (n.id == VpSkeleton::kP7CameraFit || n.id == VpSkeleton::kP8CameraLabels || n.id == VpSkeleton::kP9CameraSmoothed)
				continue;
			VpSkeleton::SlotSet& set = m_table->At(i);
			VpSkeleton::LaneTicket ticket;
			if (set.Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &ticket) != VpSkeleton::ClaimResult::Claimed)
				continue;
			VpAncestry::Header& hdr = set.LaneHeader(ticket);
			VpSkeleton::LaneInputs pins;
			if (!m_table->LaneAncestors(n.id, seq, hdr, pins))
			{
				// An input slot is missing: never publish a header with an empty ancestor list (the seal would read it as complete).
				pins.Unpin();
				set.Abandon(ticket);
				if (!warmUp)
					++m_markerFailures;
				continue;
			}
			hdr.honoured = indicator.StampOf(seq);
			set.Publish(ticket);
			pins.Unpin();
			++m_markers;
		}
	}

	void CameraLane::SealOne(uint64_t seq, VpIndicator::Indicator& indicator, Sealed& out)
	{
		const VpIndicator::Stamp stamp = indicator.StampOf(seq);
		CameraLabelFill* fill = m_fill;
		// A product the seal rejects is filled live only for P8 (the labels, from a SEALED P7); P7 has no live fill (D-K1: the fit
		// never runs on the render thread) and the markers are published every frame, so every other fill is declined.
		const std::vector<VpSkeleton::ProductFrameResult>& res = m_table->Resolve(seq, stamp,
			[fill](const VpSkeleton::Node& n, uint64_t s, int live, VpAncestry::Header& hdr, uint32_t) -> bool {
				return fill != nullptr && n.id == VpSkeleton::kP8CameraLabels && fill->FillLive(s, live, hdr);
			});
		out.any = true;
		out.seq = seq;
		out.p7Published = false;
		for (const VpSkeleton::ProductFrameResult& r : res)
			if (r.id == VpSkeleton::kP7CameraFit)
				out.p7Published = r.seal.outcome == VpSkeleton::SealOutcome::Sealed && r.seal.reason == VpSkeleton::SealReason::Accepted &&
					!r.liveFilled;
		out.hadField = !m_fieldMark.empty() && m_fieldMark[static_cast<size_t>(seq % m_fieldMark.size())] == seq + 1;
		m_table->ReleaseAfter(seq);
		++m_sealedFrames;
	}
}
