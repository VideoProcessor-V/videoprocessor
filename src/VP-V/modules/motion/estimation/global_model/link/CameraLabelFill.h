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
#include <VP-V/core/ProductSlots.h>

#include <atomic>
#include <cstdint>
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
	};

	class CameraLabelFill
	{
	public:
		CameraLabelFill(VpSkeleton::SlotTable& table, ICameraLink& link, ILabelDevice& device, VpIndicator::Indicator* indicator);

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

		bool NextSeq(uint64_t* out) const;
		void RunLane();

		VpSkeleton::SlotTable& m_table;
		ICameraLink& m_link;
		ILabelDevice& m_device;
		VpIndicator::Indicator* m_indicator;
		Gap m_gap;
		uint64_t m_floor = 0;
		uint64_t m_cursor = 0;
		LabelFillCounters m_counters;
	};
}
