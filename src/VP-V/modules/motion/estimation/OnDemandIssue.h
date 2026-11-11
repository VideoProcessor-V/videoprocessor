// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// W2-C5b commit 4: the render-time request, taken at the queue dequeue (the renderer's VPV-HOOK line where the pair is
// accepted: A = the frame dequeued, B = `next`). The look-ahead walk runs before the dequeue selects B, so it cannot see
// the selected pair; the dequeue can. Stall fix (re-check MUST 3): that hook runs under the renderer's queue mutex and only
// STAGES the pair (copies); PostStagedOnDemand, a second VPV-HOOK line after the queue block, issues it with no renderer lock
// held. It never decodes: it only PEEKS each frame's analysis cache (the queued-capture luma, under the cache's own mutex)
// and skips the pair, counted as `uncached` (Ctrl+I), when either frame has none yet. In standard
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

	// The dequeue hook, under the renderer's queue mutex: STAGES the accepted pair and returns (stall fix, W2-C5b re-check MUST 3).
	// Only atomics and copies here: the producer pointer (engine off = zero work), the setting, then the two caches' shared
	// pointers in a peek closure. No lock but the engine's small staging mutex is taken; the cache peeks, the P2 lookup, the
	// producer's mutex and its wake all run in PostStagedOnDemand, after the queue block.
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
		using Engine = typename std::decay<decltype(engine)>::type;
		typename Engine::OnDemandStaged staged;
		staged.from = from;
		staged.to = next.sourceSequence;
		staged.pairRateHz = pairRateHz;
		staged.dequeueWaitMs = dequeueWaitMs;
		const Cache cacheA = cache;
		const auto cacheB = next.analysis;
		const uint64_t generationB = next.activePictureIdentity.sourceFormatGeneration;
		staged.peek = [cacheA, sourceFormatGeneration, cacheB, generationB](AnalysisLumaSource& lumaFrom, AnalysisLumaSource& lumaTo) {
			using Prepared = typename std::decay<decltype(cacheA->queued)>::type;
			Prepared a, b;
			if (!PeekQueuedAnalysis(cacheA, sourceFormatGeneration, a) || !PeekQueuedAnalysis(cacheB, generationB, b))
				return false;
			lumaFrom = a.source;
			lumaTo = b.source;
			return true;
		};
		engine.StageOnDemand(std::move(staged));
	}

	// After the renderer's queue block (its second VPV-HOOK W2-C5b line), on the render thread with no renderer lock held:
	// the staged pair, if any, is checked and issued. The request is queued, never computed, here.
	template <class Impl>
	void PostStagedOnDemand(Impl* impl)
	{
		if (!impl)
			return;
		auto& engine = impl->vectorEngine;
		using Engine = typename std::decay<decltype(engine)>::type;
		typename Engine::OnDemandStaged staged;
		if (!engine.TakeStagedOnDemand(staged))
			return;   // nothing staged this cycle (off, engine off, no accepted pair): zero work
		const auto producer = std::atomic_load(&impl->motionProducer);
		if (!producer || !producer->Running())
			return;
		engine.NoteOnDemandCycle(staged.pairRateHz, staged.dequeueWaitMs);
		const uint64_t from = staged.from, to = staged.to;
		if (to <= from)
			return;
		if (to == from + 1 && producer->LastSubmitted() >= to)
			return;   // the walk submitted B: its job computes (or computed) this pair
		if (engine.P2().Find(from, to))
			return;   // already computed ahead
		if (!engine.OnDemandConsumerActive())
			return;   // counted (idleSkips)
		::VpMotion::OnDemand::PairRequest request;
		if (!staged.peek || !staged.peek(request.lumaFrom, request.lumaTo))
		{
			engine.NoteOnDemandUncached();   // never decoded here either: counted, visible
			return;
		}
		request.from = from;
		request.to = to;
		request.generation = impl->motionValidity.StreamGeneration();
		request.chainOk = true;
		request.budgetMs = engine.OnDemandBudgetMs(staged.pairRateHz);   // <= 0: RequestPair answers NoBudget (counted, no work)
		engine.IssueOnDemand(std::move(request));
	}
}
