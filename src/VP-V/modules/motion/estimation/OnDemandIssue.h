// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// W2-C5b commit 4, reworked for rule 32 (HOOK FREEZE: no new hook lines in Bill's files): the render-time request is
// staged from the EXISTING look-ahead call, MotionAhead::CollectQueueAhead (MotionAheadWalk.h; the render loop's preview
// pass, the caller holding the queue mutex), and posted from the EXISTING ResolveMotionEngine call (PhaseTimeline.inl; the
// render loop after the queue block, before RenderLocked's estimate, renderMutex held, queue mutex not held). The walk
// cannot see the dequeue's choice, so it PREDICTS it with the dequeue's own selection: A = the queue head, B = the next
// entry (the one after a pulldown film update while interpolating); a pair the dequeue then refuses is simply not claimed
// by the estimate (counted `unclaimed`). Stall fix (re-check MUST 3) kept: under the queue mutex the walk only STAGES the
// pair (copies); PostStagedOnDemand issues it with no queue lock held. It never decodes: it only PEEKS each frame's
// analysis cache (the queued-capture luma, under the cache's own mutex) and skips the pair, counted as `uncached` (Ctrl+I),
// when either frame has none yet. In standard mode the walk has usually cached both; at depth 0 (low latency) nothing
// walks ahead, so the uncached share is measured (commit 6), never hidden. The request is queued, never computed, here.
// The estimate (VectorEngineEstimate.inl) claims it with VectorEngine::OnDemandFill within the frame budget
// (OnDemand::RenderBudget: source period minus measured render time). The render sample is now the walk-to-walk interval
// with no wait subtracted (the host's dequeue wait is not visible without a hook line): the budget errs low (conservative).
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

	// The stage, under the renderer's queue mutex: STAGES a pair and returns (stall fix, W2-C5b re-check MUST 3). Called by
	// StageOnDemandAtWalk with the predicted pair (the tests call it directly with a chosen pair).
	// Only atomics and copies here: the producer pointer (engine off = zero work), the setting, then the two caches' shared
	// pointers in a peek closure. No lock but the engine's small staging mutex is taken; the cache peeks, the P2 lookup, the
	// producer's mutex and its wake all run in PostStagedOnDemand, after the queue block.
	template <class Impl, class Cache, class Queued>
	void IssueOnDemandAtDequeue(Impl* impl, const Cache& cache, uint64_t sourceFormatGeneration, uint64_t from, const Queued& next,
		double pairRateHz, double dequeueWaitMs)
	{
		if (!impl)
			return;
		const auto producer = std::atomic_load(&impl->vectorEngine.motionProducer);
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

	// After the renderer's queue block (from Impl::ResolveMotionEngine, PhaseTimeline.inl), on the render thread with the queue
	// mutex not held (renderMutex is; no on-demand lock is ever held while taking renderMutex):
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
		const auto producer = std::atomic_load(&impl->vectorEngine.motionProducer);
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
			engine.NoteOnDemandUncached(from, to);   // never decoded here either: counted, and the fill names the cause
			return;
		}
		request.from = from;
		request.to = to;
		request.generation = impl->motionValidity.StreamGeneration();
		request.chainOk = true;
		request.budgetMs = engine.OnDemandBudgetMs(staged.pairRateHz);   // <= 0: RequestPair answers NoBudget (counted, no work)
		engine.IssueOnDemand(std::move(request));
	}

	// Rule 32: the walk's stage (from CollectQueueAhead, the caller holding the queue mutex). Predicts the dequeue's pair with
	// its own selection and filters (A: not a cadence repeat, has a state, not a file frame shown without motion; B: the next
	// entry, or the one after a pulldown film update while interpolating; not a repeat, not an update, same generation, a
	// different source, has a state, not shown without motion). Once per new A. Rate: A's film rate (pulldown locked) else the
	// content rate the last estimate measured (0 until then: no budget, counted). Only atomics and copies.
	template <class Impl, class Queue>
	void StageOnDemandAtWalk(Impl* impl, const Queue& queue)
	{
		if (!impl || queue.size() < 2)
			return;
		const auto producer = std::atomic_load(&impl->vectorEngine.motionProducer);
		if (!producer || !producer->Running())
			return;   // engine off: zero work
		const auto& a = queue[0];
		if (a.cadenceRepeat || !a.state || a.fileMotionOff)
			return;
		size_t bIndex = 1;
		if (impl->interpolating && queue[1].filmUpdate && queue.size() >= 3 && !queue[1].gapBefore && !queue[2].filmUpdate)
			bIndex = 2;
		const auto& b = queue[bIndex];
		if (b.cadenceRepeat || b.filmUpdate || !b.state || b.fileMotionOff || b.generation != a.generation ||
			b.sourceSequence == a.sourceSequence)
			return;
		if (!impl->vectorEngine.NoteOnDemandWalkFrame(a.sourceSequence))
			return;   // the same A as the last stage (the dequeue held it)
		const double rateHz = a.filmHz > 0.0 ? a.filmHz : impl->dejudderSourceHz;
		IssueOnDemandAtDequeue(impl, a.analysis, a.activePictureIdentity.sourceFormatGeneration, a.sourceSequence, b, rateHz, 0.0);
	}

	namespace detail
	{
		template <class...> struct MakeVoid { using type = void; };
		// True for a host that runs the on-demand engine (the renderer's Impl); the look-ahead walk's test fakes do not.
		template <class Impl, class = void> struct HostsOnDemand : std::false_type {};
		template <class Impl> struct HostsOnDemand<Impl, typename MakeVoid<
			decltype(std::declval<Impl&>().vectorEngine.NoteOnDemandWalkFrame(uint64_t(0))),
			decltype(std::declval<Impl&>().dejudderSourceHz), decltype(std::declval<Impl&>().interpolating)>::type> : std::true_type {};
		template <class Impl, class Queue> void StageAtWalk(Impl* impl, const Queue& queue, std::true_type) { StageOnDemandAtWalk(impl, queue); }
		template <class Impl, class Queue> void StageAtWalk(Impl*, const Queue&, std::false_type) {}
		template <class Impl> void PostAt(Impl* impl, std::true_type) { PostStagedOnDemand(impl); }
		template <class Impl> void PostAt(Impl*, std::false_type) {}
	}

	// The existing call sites' entries (CollectQueueAhead; Impl::ResolveMotionEngine): a no-op for a host without the engine.
	template <class Impl, class Queue>
	void StageOnDemandIfHosted(Impl* impl, const Queue& queue) { detail::StageAtWalk(impl, queue, detail::HostsOnDemand<Impl>()); }
	template <class Impl>
	void PostOnDemandIfHosted(Impl* impl) { detail::PostAt(impl, detail::HostsOnDemand<Impl>()); }
}
