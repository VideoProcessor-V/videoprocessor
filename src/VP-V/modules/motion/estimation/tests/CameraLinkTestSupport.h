// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Test helper (header-only): camera link rigs shared by the K1/K2/K4 CPU tests. A SlotTable over a real graph with the
// P7 inputs published, a FieldCopy of a synthetic field, and the frame retire helper.
#pragma once

#include "SyntheticCameraFields.h"

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/core/ProductSlots.h>
#include <VP-V/modules/motion/estimation/global_model/CameraProducts.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitWorker.h>

#include <memory>
#include <vector>

namespace VideoProcessorTest
{
namespace CameraLinkTest
{
	inline std::shared_ptr<CameraLink::FieldCopy> CopyOf(const CameraSynth::Field& f, uint64_t seq, uint32_t era = 1)
	{
		auto c = std::make_shared<CameraLink::FieldCopy>();
		c->seq = seq;
		c->pairFrom = seq - 1;
		c->pairTo = seq;
		c->gridW = f.gridW;
		c->gridH = f.gridH;
		c->blockSize = f.blockSize;
		c->fwdX = f.fx;
		c->fwdY = f.fy;
		c->fwdTrust = f.trust;
		if (f.haveBackward)
		{
			c->bwdX = f.bx;
			c->bwdY = f.by;
			c->bwdTrust = f.btrust;
		}
		c->stamp.era = era;
		return c;
	}

	inline VpSkeleton::GraphSettings Settings(uint8_t laneLead = 2)
	{
		VpSkeleton::GraphSettings gs;
		gs.dims.width = 1920;
		gs.dims.height = 1080;
		gs.laneLead = laneLead;
		gs.queueDepth = 6;
		gs.lookAheadWalk = 2;
		gs.ofaPresent = true;
		return gs;
	}

	inline std::vector<VpSkeleton::ConsumerDecl> Consumers(std::initializer_list<CameraLink::CameraConsumer> active)
	{
		CameraLink::CameraDemandInput in;
		for (CameraLink::CameraConsumer c : active)
			in.active[static_cast<int>(c)] = true;
		std::vector<VpSkeleton::ConsumerDecl> out;
		CameraLink::DeclareCameraConsumers(in, out);
		return out;
	}

	// A graph with P7 (Diagnostics consumer) and its table, byte ring and indicator.
	struct Rig
	{
		VpSkeleton::Graph graph;
		std::unique_ptr<VpSkeleton::SlotTable> table;
		std::unique_ptr<VpSkeleton::CpuBuffers> bytes;
		std::unique_ptr<VpIndicator::Indicator> indicator;

		explicit Rig(uint8_t laneLead = 2)
		{
			graph = VpSkeleton::BuildGraph(VpSkeleton::SkeletonProducts(), Consumers({ CameraLink::CameraConsumer::Diagnostics }), Settings(laneLead));
			table.reset(new VpSkeleton::SlotTable(graph));
			const VpSkeleton::Node* n = table->NodeOf(VpSkeleton::kP7CameraFit);
			bytes.reset(new VpSkeleton::CpuBuffers(*n));
			indicator.reset(new VpIndicator::Indicator(1, 256));
			// Frame 0 is the previous frame of the lag-1 inputs (P2 reads P1 at lag 1) and must be sealed before frame 1.
			PublishInputs(0);
			table->Resolve(0, indicator->Seal(0), [](const VpSkeleton::Node&, uint64_t, int, VpAncestry::Header&, uint32_t) { return true; });
		}

		size_t Ring() { return table->Find(VpSkeleton::kP7CameraFit)->Ring(); }

		// Publish every product P7 and P8 depend on (all nodes except P7..P9, in topological order) as the lane would,
		// with recorded ancestors and the honoured stamp, so a later Resolve() accepts them.
		void PublishInputs(uint64_t seq)
		{
			for (size_t i = 0; i < table->Count(); ++i)
			{
				const VpSkeleton::Node& n = table->NodeAt(i);
				if (n.id == VpSkeleton::kP7CameraFit || n.id == VpSkeleton::kP8CameraLabels || n.id == VpSkeleton::kP9CameraSmoothed)
					continue;
				VpSkeleton::SlotSet& s = table->At(i);
				VpSkeleton::LaneTicket t;
				if (s.Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &t) != VpSkeleton::ClaimResult::Claimed)
					continue;
				VpAncestry::Header& hdr = s.LaneHeader(t);
				VpSkeleton::LaneInputs pins;
				table->LaneAncestors(n.id, seq, hdr, pins);
				hdr.honoured = indicator->StampOf(seq);
				s.Publish(t);
				pins.Unpin();
			}
		}

		// Seal everything (accepting) and release frame seq, so a later frame can reuse the slots.
		void Retire(uint64_t seq)
		{
			for (size_t i = 0; i < table->Count(); ++i)
			{
				table->At(i).Seal(seq, [](const VpAncestry::Header&) { return VpSkeleton::SealReason::Accepted; });
				table->At(i).Release(seq);
			}
		}

		// The render sealed frame seq without a published P7 (the fit is late): the slot is Rejected.
		void SealP7Empty(uint64_t seq)
		{
			table->Find(VpSkeleton::kP7CameraFit)->Seal(seq, [](const VpAncestry::Header&) { return VpSkeleton::SealReason::Accepted; });
		}

		CameraLink::CameraFitEnv Env(CameraLink::CameraRecordPool* pool, CameraLink::FitCounters* counters, size_t queue = 4)
		{
			CameraLink::CameraFitEnv e;
			e.table = table.get();
			e.pool = pool;
			e.counters = counters;
			e.queueCapacity = queue;
			VpSkeleton::CpuBuffers* b = bytes.get();
			e.slotBytes = [b](int slot) { return b->Ring(static_cast<size_t>(slot)); };
			VpIndicator::Indicator* ind = indicator.get();
			e.stampOf = [ind](uint64_t seq) { return ind->StampOf(seq); };
			return e;
		}
	};

	inline CameraLink::CameraFitJob PanJob(uint64_t seq, double tx, double pairSeconds = 1.0 / 24)
	{
		CameraSynth::Field f(CameraSynth::Grid4K());
		f.FromModel(CameraSynth::PanModel(tx, 0));
		f.AddNoise(0.05, 100 + seq);
		CameraLink::CameraFitJob j;
		j.seq = seq;
		j.field = CopyOf(f, seq);
		j.facts.pairSeconds = pairSeconds;
		j.stamp = j.field->stamp;
		return j;
	}
}
}
