// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// W2-C5b commit 4: the render-time request, issued at the queue dequeue (the renderer's ONE VPV-HOOK line where the pair is
// accepted: A = the frame dequeued, B = `next`). The look-ahead walk runs before the dequeue selects B, so it cannot see
// the selected pair; the dequeue can. Called under the renderer's queue mutex: cheap (the frames' analysis sources are the
// walk's cached ones; the request is queued, never computed, here). The estimate (VectorEngineEstimate.inl) claims it with
// VectorEngine::OnDemandFill within the frame budget (OnDemand::RenderBudget: source period minus measured render time).
//
// Issued only when: optical_flow_on_demand wants it (OnDemand::Wanted: auto = not on the low-latency path, the default;
// see OnDemandEstimate.h), the producer runs, the pair is not already computed ahead (P2) or covered by the walk (an
// adjacent pair whose B was submitted), and an estimate read a pair recently (no consumer = no GPU work).
// Templated on the host types (testable with a fake host; no renderer header here). C++14.

#include <VP-V/modules/motion/estimation/OnDemandEstimate.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace MotionAhead
{
	template <class Impl, class Frame, class StatePtr, class Converted, class Cache, class Profile, class Queued>
	void IssueOnDemandAtDequeue(Impl* impl, const Frame& frame, const StatePtr& state, const Converted& converted, const Cache& cache,
		Profile queuedProfile, uint64_t sourceFormatGeneration, uint64_t from, const Queued& next, double pairRateHz,
		double dequeueWaitMs)
	{
		if (!impl)
			return;
		auto& engine = impl->vectorEngine;
		const ::VpMotion::OnDemand::Setting setting = ::VpMotion::OnDemand::SettingOf(impl->activeSettings.opticalFlowOnDemand);
		engine.SetOnDemandSetting(setting);   // ProducerPlan: `on` keeps a producer on the low-latency path
		if (!::VpMotion::OnDemand::Wanted(setting, impl->lowLatencyQueueActive.load(std::memory_order_acquire)))
			return;
		engine.NoteOnDemandCycle(pairRateHz, dequeueWaitMs);
		const auto producer = std::atomic_load(&impl->motionProducer);
		if (!producer || !producer->Running())
			return;
		const uint64_t to = next.sourceSequence;
		if (to <= from)
			return;
		if (to == from + 1 && producer->LastSubmitted() >= to)
			return;   // the walk submitted B: its job computes (or computed) this pair
		if (engine.P2().Find(from, to))
			return;   // already computed ahead
		if (!engine.OnDemandConsumerActive())
			return;   // counted (idleSkips)
		if (!state || !next.state)
			return;
		const auto a = Impl::PrepareFrameAnalysis(frame, *state, converted, cache, queuedProfile, sourceFormatGeneration, false,
			(*state).dolbyVisionTunnel);
		const auto b = Impl::PrepareFrameAnalysis(next.frame, *next.state, next.converted, next.analysis, queuedProfile,
			next.activePictureIdentity.sourceFormatGeneration, false, (*next.state).dolbyVisionTunnel);
		if (!a.source.IsValid() || !b.source.IsValid())
			return;
		::VpMotion::OnDemand::PairRequest request;
		request.from = from;
		request.to = to;
		request.generation = impl->motionValidity.StreamGeneration();
		request.lumaFrom = a.source;
		request.lumaTo = b.source;
		request.chainOk = true;
		request.budgetMs = engine.OnDemandBudgetMs(pairRateHz);   // <= 0: RequestPair answers NoBudget (counted, no work)
		engine.IssueOnDemand(std::move(request));
	}
}
