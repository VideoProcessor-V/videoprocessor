// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K2-2): the P8 fill rule (TEAM-RULES "How a shared item gets filled"). One implementation, three entrances:
//   * LANE   an IGapWork (LaneCompute) produces P8(N) in an idle gap once P7(N) is PUBLISHED: claim -> pins -> write ->
//            stamp -> publish, before the seal. A publish after the seal is Late: the SlotSet discards it (never merged).
//   * LIVE   if P8(N) was not published by the seal, the seal's live fill (SlotTable::Resolve calls FillLive) is the ONE
//            producer for the frame, and only when P7(N) was SEALED; every later consumer reuses the result.
//   * ABSENT P7(N) not sealed (late, rejected) -> FillLive declines -> P8 absent -> LabelsFor says unavailable and the consumer
//            runs with enabled = 0 (today's values), shown under the camera status ("no model (late)").
// The device (GPU upload / compute, or a fake in tests) is behind ILabelDevice. Render thread only.
#pragma once

#include "CameraLink.h"

#include <VP-V/core/LaneInterfaces.h>
#include <VP-V/core/LaneLease.h>
#include <VP-V/core/ProductSlots.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace CameraLink
{
	struct LabelTarget
	{
		bool live = false;   // false: a ring slot of the P8 SlotSet; true: a live-pool buffer
		int index = 0;
	};

	class ILabelDevice
	{
	public:
		virtual ~ILabelDevice() {}
		// Writes the label texels of `rec` into the target texture. `field` may be null (upload mode needs none).
		virtual bool Produce(const CameraModel::CameraMotion& rec, const FieldCopy* field, LabelTarget target) = 0;
		virtual pl_tex TextureOf(LabelTarget target) const = 0;
		virtual uint32_t PredictUs() const { return 0; }
	};

	struct LabelFillCounters
	{
		std::atomic<uint64_t> lane{ 0 };           // produced by the gap work and published
		std::atomic<uint64_t> laneLate{ 0 };       // produced but the seal came first (discarded by the SlotSet)
		std::atomic<uint64_t> laneBlocked{ 0 };    // claim / pins / device refused
		std::atomic<uint64_t> live{ 0 };           // produced by the seal's live fill
		std::atomic<uint64_t> declined{ 0 };       // live fill declined: P7 not sealed (absent)
		std::atomic<uint64_t> served{ 0 };         // LabelsFor answered with a texture
		std::atomic<uint64_t> absent{ 0 };         // LabelsFor answered "unavailable"
		std::atomic<uint64_t> staleLane{ 0 };      // lane productions REFUSED: the lane graph was rebuilt between the claim and the publish
		std::atomic<uint64_t> rebound{ 0 };        // times the fill took a new lane lease (a rebuild was noticed)
	};

	// "camera labels: lane 12, late 0, blocked 0, live 3, declined 1, served 15, absent 2, stale lane 0 (refused after a graph rebuild), lane rebinds 0"
	inline std::string LabelFillStatusLine(const LabelFillCounters& c)
	{
		char b[384];
		std::snprintf(b, sizeof(b), "camera labels: %llu lane, %llu late, %llu blocked, %llu live, %llu declined, %llu served, %llu absent, %llu stale lane (refused after a graph rebuild), %llu lane rebinds",
			(unsigned long long)c.lane.load(), (unsigned long long)c.laneLate.load(), (unsigned long long)c.laneBlocked.load(),
			(unsigned long long)c.live.load(), (unsigned long long)c.declined.load(), (unsigned long long)c.served.load(),
			(unsigned long long)c.absent.load(), (unsigned long long)c.staleLane.load(), (unsigned long long)c.rebound.load());
		return b;
	}

	class CameraLabelFill
	{
	public:
		// STANDALONE / TESTS: a borrowed table that outlives the fill by construction (never stale).
		CameraLabelFill(VpSkeleton::SlotTable& table, ICameraLink& link, ILabelDevice& device, VpIndicator::Indicator* indicator);
		// LIVE: the lane is held through a lease (core/LaneLease.h), never a raw table: the lane swaps its whole state at every rebuild.
		// `source` re-acquires the current lease (bind AnalysisLane::Lease(kP8CameraLabels) in a lambda). Every entrance rebinds first; a
		// production claimed in an older generation is abandoned, counted (LabelFillCounters::staleLane) and never published into the new
		// graph. The lease is held for the whole production, so a rebuild during it frees nothing under the fill.
		CameraLabelFill(VpSkeleton::LaneLeaseSource source, ICameraLink& link, ILabelDevice& device);
		// Takes the lane's current lease when it is newer; true when it changed (the lane cursor restarts: its sequences belong to the old graph).
		bool Rebind();
		uint64_t LaneGeneration() const { return m_lease.generation; }

		// Render thread, at the seal: the SlotTable::Resolve fill callback target for P8.
		bool FillLive(uint64_t seq, int liveIndex, VpAncestry::Header& hdr);
		// Consumers (render thread, after the seal).
		bool LabelsFor(uint64_t seq, CameraConsumer c, CameraLabelsView& out);
		// The frame about to be rendered: gap work below it is pointless.
		void OnFrameStart(uint64_t seq) { if (seq > m_floor) m_floor = seq; }

		VpSkeleton::IGapWork* GapWork() { return &m_gap; }
		const LabelFillCounters& Counters() const { return m_counters; }

	private:
		class Gap : public VpSkeleton::IGapWork
		{
		public:
			explicit Gap(CameraLabelFill& owner) : m_owner(owner) {}
			VpSkeleton::GapClass Class() const override { return VpSkeleton::GapClass::LaneCompute; }
			uint16_t Id() const override { return VpSkeleton::kP8CameraLabels; }
			bool HasWork() const override { return m_owner.NextSeq(nullptr); }
			uint32_t PredictUs() const override { return m_owner.m_device.PredictUs(); }
			void RunChunk(uint32_t budgetUs) override { (void)budgetUs; m_owner.RunLane(); }
		private:
			CameraLabelFill& m_owner;
		};

		bool NextSeq(uint64_t* out);
		void RunLane();

		VpSkeleton::LaneLease m_lease;            // render thread only
		VpSkeleton::LaneLeaseSource m_source;
		ICameraLink& m_link;
		ILabelDevice& m_device;
		Gap m_gap;
		uint64_t m_floor = 0;
		uint64_t m_cursor = 0;
		LabelFillCounters m_counters;
	};
}
