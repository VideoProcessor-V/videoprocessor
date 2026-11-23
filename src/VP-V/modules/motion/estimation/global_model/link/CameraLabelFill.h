// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K2-2): the P8 fill rule (TEAM-RULES "How a shared item gets filled"). One implementation, three entrances:
//   * LANE   an IGapWork (LaneCompute) produces P8(N) in an idle gap once P7(N) and its fit field product (p7f_camera_fit_field,
//            published with P7 under P7's stamp) are PUBLISHED: claim -> pins -> write ->
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
		std::atomic<uint64_t> epochResets{ 0 };    // GEM-127: the numbering restarted without a lane rebuild: floor and cursor reset
		std::atomic<uint64_t> rebound{ 0 };        // times the fill took a new lane lease (a rebuild was noticed)
		std::atomic<uint64_t> fieldMissing{ 0 };        // P7 published but the fit field product is not (node absent or no published slot for the frame): no labels
		std::atomic<uint64_t> fieldStampMismatch{ 0 };  // both published but the field's stamp differs from P7's: no labels
		std::atomic<uint64_t> fieldUnreadable{ 0 };     // fit field product has no storage, bad content, or was recycled during the read: no labels
	};

	// "camera labels: lane 12, late 0, blocked 0, live 3, declined 1, served 15, absent 2, stale lane 0 (refused after a graph rebuild), lane rebinds 0, fit field missing 0 (no labels), fit field stamp mismatch 0 (no labels), fit field unreadable 0 (no labels)"
	inline std::string LabelFillStatusLine(const LabelFillCounters& c)
	{
		char b[720];
		std::snprintf(b, sizeof(b), "camera labels: %llu lane, %llu late, %llu blocked, %llu live, %llu declined, %llu served, %llu absent, %llu stale lane (refused after a graph rebuild), %llu lane rebinds, %llu fit field missing (no labels), %llu fit field stamp mismatch (no labels), %llu fit field unreadable (no labels), %llu numbering restarts (gap floor reset)",
			(unsigned long long)c.lane.load(), (unsigned long long)c.laneLate.load(), (unsigned long long)c.laneBlocked.load(),
			(unsigned long long)c.live.load(), (unsigned long long)c.declined.load(), (unsigned long long)c.served.load(),
			(unsigned long long)c.absent.load(), (unsigned long long)c.staleLane.load(), (unsigned long long)c.rebound.load(),
			(unsigned long long)c.fieldMissing.load(), (unsigned long long)c.fieldStampMismatch.load(), (unsigned long long)c.fieldUnreadable.load(), (unsigned long long)c.epochResets.load());
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
		// The fit field product's bytes (AnalysisLane/CameraLane Lease(kCameraFitField)): the product slot IS the field's storage, P8's only
		// read of it. Not set (or a generation that differs from P8's): no field, so no labels.
		void SetFieldSource(VpSkeleton::LaneLeaseSource source) { m_fieldSource = std::move(source); }
		uint64_t LaneGeneration() const { return m_lease.generation; }

		// Render thread, at the seal: the SlotTable::Resolve fill callback target for P8.
		bool FillLive(uint64_t seq, int liveIndex, VpAncestry::Header& hdr);
		// Consumers (render thread, after the seal).
		bool LabelsFor(uint64_t seq, CameraConsumer c, CameraLabelsView& out);
		// The frame about to be rendered: gap work below it is pointless.
		// `epoch` (optional, 0 = not supplied): the vectors-valid epoch (IVectorEngine::FieldEra). A change means the source numbering
		// restarted: the floor and cursor of the old numbering are void even when the lane was not rebuilt (GEM-127), counted.
		void OnFrameStart(uint64_t seq, uint64_t epoch = 0)
		{
			if (epoch != 0 && epoch != m_epoch)
			{
				if (m_epoch != 0)
				{
					m_floor = 0;
					m_cursor = 0;
					m_fieldMissNext = 0;
					m_counters.epochResets.fetch_add(1, std::memory_order_relaxed);
				}
				m_epoch = epoch;
			}
			if (seq > m_floor) m_floor = seq;
		}

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
		// CP-A1-D4-CAMLABEL-FIELDCOPY: the fit field is read ONLY through the declared product p7f_camera_fit_field. True when its slot for
		// `seq` is published (or sealed) and its stamp is EXACTLY P7(seq)'s. Missing product or a mismatch: false (no labels, counted by the
		// caller as blocked / declined); never a fall-back to the pool's copy.
		enum class FieldGate { Ok, Missing, StampMismatch };
		static FieldGate FitFieldGate(const VpSkeleton::LaneLease& lease, uint64_t seq);
		static bool FitFieldMatchesP7(const VpSkeleton::LaneLease& lease, uint64_t seq) { return FitFieldGate(lease, seq) == FieldGate::Ok; }
		// Reads the field's CONTENT from the product slot (stamp gate first, then the slot's ring bytes, then a re-read of the slot so a
		// recycled ring is never used). False: no field (no labels). No other source exists.
		bool ReadFitField(const VpSkeleton::LaneLease& lease, uint64_t seq, FieldCopy& out);

		VpSkeleton::LaneLease m_lease;            // render thread only
		VpSkeleton::LaneLeaseSource m_source;
		VpSkeleton::LaneLeaseSource m_fieldSource;
		FieldCopy m_field;                        // render thread only: the decode target, reused (no allocation at steady state)
		uint64_t m_fieldMissNext = 0;             // NextSeq counts each frame's missing field once
		ICameraLink& m_link;
		ILabelDevice& m_device;
		Gap m_gap;
		uint64_t m_floor = 0;
		uint64_t m_epoch = 0;                     // the last vectors-valid epoch seen by OnFrameStart
		uint64_t m_cursor = 0;
		LabelFillCounters m_counters;
	};
}
