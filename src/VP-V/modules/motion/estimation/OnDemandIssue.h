// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// W2-C5b commit 4: the render-time request, issued at the queue dequeue (the renderer's ONE VPV-HOOK line where the pair is
// accepted: A = the frame dequeued, B = `next`). The look-ahead walk runs before the dequeue selects B, so it cannot see
// the selected pair; the dequeue can. Called under the renderer's queue mutex, so it never decodes: it only PEEKS each frame's
// analysis cache (the queued-capture luma, under the cache's own mutex - the same lock order the renderer's
// PrepareFrameAnalysis uses) and skips the pair, counted as `uncached` (Ctrl+I), when either frame has none yet. In standard
// mode the walk has usually cached both; at depth 0 (low latency) nothing walks ahead, so the uncached share is measured
// (commit 6), never hidden. The request is queued, never computed, here. The estimate (VectorEngineEstimate.inl) claims it
// with VectorEngine::OnDemandFill within the frame budget (OnDemand::RenderBudget: source period minus measured render time).
//
// Issued only when: the producer runs (checked first: engine off = zero work), optical_flow_on_demand wants it
// (OnDemand::Wanted: `on` only - auto is off on every profile until the owner decides - and in low latency only with a queue
// ahead, so competitive stays motion-free; see OnDemandEstimate.h), the pair is not already computed ahead (P2) or covered by
// the walk (an adjacent pair whose B was submitted), an estimate read a pair recently (no consumer = no GPU work), and both
// frames' luma is cached.
// Templated on the host types (testable with a fake host; no renderer header here). C++14.

#include <VP-V/modules/motion/estimation/OnDemandEstimate.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace MotionAhead
{
	// Copies the queued-capture analysis a frame's cache already holds into `out` (its source stamped with `generation`) and
	// returns true; false (out untouched) when the cache is absent or holds none yet. Never decodes (PrepareFrameAnalysis would,
	// under the queue mutex).
	template <class CachePtr, class Prepared>
	bool PeekQueuedAnalysis(const CachePtr& cache, uint64_t generation, Prepared& out)
	{
		if (!cache)
			return false;
		std::lock_guard<std::mutex> guard(cache->mutex);
		if (!cache->queued.IsValid())
			return false;
		out = cache->queued;
		out.source.generation = generation;
		return true;
	}

	template <class Impl, class Cache, class Queued>
	void IssueOnDemandAtDequeue(Impl* impl, const Cache& cache, uint64_t sourceFormatGeneration, uint64_t from, const Queued& next,
		double pairRateHz, double dequeueWaitMs)
	{
		if (!impl)
			return;
		const auto producer = std::atomic_load(&impl->motionProducer);
		if (!producer || !producer->Running())
			return;   // engine off: zero work
		auto& engine = impl->vectorEngine;
		if (!::VpMotion::OnDemand::Wanted(engine.OnDemandSettingNow(), impl->lowLatencyQueueActive.load(std::memory_order_acquire),
			impl->queueAheadCapacity.load(std::memory_order_acquire)))
			return;
		engine.NoteOnDemandCycle(pairRateHz, dequeueWaitMs);
		const uint64_t to = next.sourceSequence;
		if (to <= from)
			return;
		if (to == from + 1 && producer->LastSubmitted() >= to)
			return;   // the walk submitted B: its job computes (or computed) this pair
		if (engine.P2().Find(from, to))
			return;   // already computed ahead
		if (!engine.OnDemandConsumerActive())
			return;   // counted (idleSkips)
		using Prepared = typename std::decay<decltype(cache->queued)>::type;
		Prepared a, b;
		if (!PeekQueuedAnalysis(cache, sourceFormatGeneration, a)
			|| !PeekQueuedAnalysis(next.analysis, next.activePictureIdentity.sourceFormatGeneration, b))
		{
			engine.NoteOnDemandUncached();   // not decoded here (the queue mutex is held): counted, visible
			return;
		}
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
