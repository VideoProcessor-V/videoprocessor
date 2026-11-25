// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Y1b: the skeleton's P2 feed. The motion engine tells it (IVectorEngine::SetP2PublishSink, on the NVOF producer's
// worker or R6 async extras thread) that a P2 pair was accepted by its slot; the feed claims, stamps and publishes
// the lane slot of the pair's later frame right away, AHEAD of the frame's seal, instead of waiting for the render
// thread's FeedEngine to find the field (Begin / AfterRender).
//
// Claim protocol (the lane's own, core/LaneRunner.h PrepareJob; SK-W lease rules):
//   lease = LeaseFor(host, kP2VectorPair)          holds the lane state for the call; empty lease = no external demand (counted)
//   LaneAncestors(P2, to, scratch, pins)           input pins BEFORE the claim: nothing is claimed while an input (P1) is missing
//   Claim(to, Provenance::AheadOfa)                TooEarly / ReleasePending / !Writable: unpin and PARK (bounded ring, drained at Begin / AfterRender)
//   StartWriting(ticket)                           two-phase Claimed -> Writing; preempted = Abandon & count stale
//   hdr.honoured = lease.indicator->StampOf(to)    K1: read AFTER the pins, never the engine-time stamp of the field
//   Publish, Unpin
//
// Threading & zero-render-thread-wait invariant:
//   - Offer() runs on the BACK / producer worker thread: tries claimMutex_ (parks if busy), then parks into the bounded
//     fixed-size ring under ringMutex_ (oldest out on overflow, counted; zero heap allocation).
//   - Drain() and TryLockClaim() run on the render thread and use ONLY std::try_to_lock: the render thread NEVER blocks
//     waiting for the BACK thread; every busy deferral is counted visibly in renderTryBusy_.
//   - Pending() and Read() are lock-free atomic reads safe from any thread.
//   - Global usability of the pair is a FACT (P2Published::globalUsable), never a publish gate.
//
// Pure C++14 header.
#pragma once

#include <VP-V/core/LaneLease.h>
#include <VP-V/core/ProductSlots.h>
#include <VP-V/modules/motion/estimation/LaneAheadPolicy.h>
#include <VP-V/modules/motion/estimation/VectorProducts.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace Vpv
{
	namespace Skeleton
	{
		class P2LaneFeed
		{
		public:
			// Rule 1 derivation: bounded by the P2 slot ring capacity (LaneAhead::kP2RingPairs == MotionAhead::kP2RingPairs == 16),
			// since at most one ring of P2 pairs can be live in the slot at once.
			static constexpr size_t kPendingCap = static_cast<size_t>(VpMotion::LaneAhead::kP2RingPairs);
			using LeaseFn = std::function<VpSkeleton::LaneLease()>;
			using GenerationFn = std::function<uint64_t()>;   // P2Slot::Generation (empty: no guard)

			enum class Outcome : uint8_t { Published, Parked, Stale, NoLease, GenerationMoved };

			struct Counters
			{
				uint64_t offered = 0;            // pairs the engine's slot announced
				uint64_t published = 0;          // lane slots published (provenance AheadOfa)
				uint64_t parked = 0;             // attempts that had to wait (inputs missing, ring not ready, release pending, or claim busy)
				uint64_t retried = 0;            // parked pairs tried again
				uint64_t overwritten = 0;        // parked pairs pushed out of the bounded ring (the oldest goes)
				uint64_t stale = 0;              // the frame already left the ring / was published (a claim failure)
				uint64_t noLease = 0;            // no lane / no P2 product in the graph at the time
				uint64_t generationDropped = 0;  // the slot's generation moved on (a discontinuity)
				uint64_t pending = 0;            // parked right now
				uint64_t renderTryBusy = 0;      // render-thread Drain / TryLockClaim saw the worker holding a lock (zero render wait)
			};

			P2LaneFeed(LeaseFn lease, GenerationFn generation) : lease_(std::move(lease)), generation_(std::move(generation)) {}
			P2LaneFeed(const P2LaneFeed&) = delete;
			P2LaneFeed& operator=(const P2LaneFeed&) = delete;

			// The sink (producer worker / R6 async extras thread). Never throws; zero heap allocations.
			Outcome Offer(const VpMotion::P2Published& p)
			{
				offered_.fetch_add(1, std::memory_order_relaxed);
				Outcome o = Outcome::Parked;
				{
					std::unique_lock<std::mutex> claim(claimMutex_, std::try_to_lock);
					if (claim.owns_lock())
					{
						o = Attempt(p);
					}
					else
					{
						parked_.fetch_add(1, std::memory_order_relaxed);
						o = Outcome::Parked;
					}
				}
				if (o == Outcome::Parked)
					Park(p);
				return o;
			}

			// Render thread (Begin / AfterRender): tries parked pairs again, oldest first, with ZERO render-thread wait.
			// If either ringMutex_ or claimMutex_ is currently held by the BACK thread, increments renderTryBusy_ and
			// returns the current pending count immediately without blocking or losing any parked item.
			size_t Drain()
			{
				std::unique_lock<std::mutex> ringLock(ringMutex_, std::try_to_lock);
				if (!ringLock.owns_lock())
				{
					renderTryBusy_.fetch_add(1, std::memory_order_relaxed);
					return pendingApprox_.load(std::memory_order_acquire);
				}
				if (count_ == 0)
					return 0;
				std::unique_lock<std::mutex> claimLock(claimMutex_, std::try_to_lock);
				if (!claimLock.owns_lock())
				{
					renderTryBusy_.fetch_add(1, std::memory_order_relaxed);
					return count_;
				}
				std::array<VpMotion::P2Published, kPendingCap> take;
				const size_t n = count_;
				for (size_t i = 0; i < n; ++i)
					take[i] = ring_[(head_ + i) % kPendingCap];
				head_ = 0;
				count_ = 0;
				for (size_t i = 0; i < n; ++i)
				{
					retried_.fetch_add(1, std::memory_order_relaxed);
					if (Attempt(take[i]) == Outcome::Parked)
						ring_[count_++] = take[i];
				}
				pendingApprox_.store(count_, std::memory_order_release);
				return count_;
			}

			// Render thread (PublishEngineSlots): try-locks claimMutex_ so the render thread NEVER stalls waiting on
			// the BACK thread. If busy, increments renderTryBusy_ visibly and returns an unlocked unique_lock.
			std::unique_lock<std::mutex> TryLockClaim()
			{
				std::unique_lock<std::mutex> lock(claimMutex_, std::try_to_lock);
				if (!lock.owns_lock())
					renderTryBusy_.fetch_add(1, std::memory_order_relaxed);
				return lock;
			}

			size_t Pending() const { return pendingApprox_.load(std::memory_order_acquire); }

			Counters Read() const
			{
				Counters c;
				c.offered = offered_.load(std::memory_order_relaxed);
				c.published = published_.load(std::memory_order_relaxed);
				c.parked = parked_.load(std::memory_order_relaxed);
				c.retried = retried_.load(std::memory_order_relaxed);
				c.overwritten = overwritten_.load(std::memory_order_relaxed);
				c.stale = stale_.load(std::memory_order_relaxed);
				c.noLease = noLease_.load(std::memory_order_relaxed);
				c.generationDropped = generationDropped_.load(std::memory_order_relaxed);
				c.pending = Pending();
				c.renderTryBusy = renderTryBusy_.load(std::memory_order_relaxed);
				return c;
			}

			void Reset()
			{
				{
					std::lock_guard<std::mutex> lock(ringMutex_);
					head_ = 0;
					count_ = 0;
					pendingApprox_.store(0, std::memory_order_release);
				}
				offered_.store(0, std::memory_order_relaxed);
				published_.store(0, std::memory_order_relaxed);
				parked_.store(0, std::memory_order_relaxed);
				retried_.store(0, std::memory_order_relaxed);
				overwritten_.store(0, std::memory_order_relaxed);
				stale_.store(0, std::memory_order_relaxed);
				noLease_.store(0, std::memory_order_relaxed);
				generationDropped_.store(0, std::memory_order_relaxed);
				renderTryBusy_.store(0, std::memory_order_relaxed);
			}

		private:
			Outcome Attempt(const VpMotion::P2Published& p)
			{
				if (generation_ && generation_() != p.generation)
				{
					generationDropped_.fetch_add(1, std::memory_order_relaxed);
					return Outcome::GenerationMoved;
				}
				const VpSkeleton::LaneLease lease = lease_ ? lease_() : VpSkeleton::LaneLease();   // holds the lane state for this call
				VpSkeleton::SlotSet* set = lease.table ? lease.table->Find(VpSkeleton::kP2VectorPair) : nullptr;
				if (!set)
				{
					noLease_.fetch_add(1, std::memory_order_relaxed);
					return Outcome::NoLease;
				}
				VpAncestry::Header scratch;
				VpSkeleton::LaneInputs pins;
				if (!lease.table->LaneAncestors(VpSkeleton::kP2VectorPair, p.to, scratch, pins))
				{
					parked_.fetch_add(1, std::memory_order_relaxed);   // an input (P1) is not published yet: nothing was claimed
					return Outcome::Parked;
				}
				VpSkeleton::LaneTicket ticket;
				const VpSkeleton::ClaimResult cr = set->Claim(p.to, VpAncestry::Provenance::AheadOfa, ~uint64_t(0), &ticket);
				if (cr == VpSkeleton::ClaimResult::TooEarly || cr == VpSkeleton::ClaimResult::ReleasePending)
				{
					pins.Unpin();
					parked_.fetch_add(1, std::memory_order_relaxed);
					return Outcome::Parked;
				}
				if (cr != VpSkeleton::ClaimResult::Claimed)
				{
					pins.Unpin();
					stale_.fetch_add(1, std::memory_order_relaxed);   // Stale / out of range: the frame left the ring (or already has its content)
					return Outcome::Stale;
				}
				if (!set->Writable(ticket))
				{
					const bool unclaimed = set->Unclaim(ticket);
					pins.Unpin();
					if (unclaimed)
					{
						parked_.fetch_add(1, std::memory_order_relaxed);
						return Outcome::Parked;
					}
					stale_.fetch_add(1, std::memory_order_relaxed);
					return Outcome::Stale;
				}
				if (!set->StartWriting(ticket))
				{
					pins.Unpin();
					set->Abandon(ticket);
					stale_.fetch_add(1, std::memory_order_relaxed);
					return Outcome::Stale;
				}
				VpAncestry::Header& hdr = set->LaneHeader(ticket);
				hdr.ancestors = scratch.ancestors;
				hdr.ancestorCount = scratch.ancestorCount;
				hdr.chainReduced = scratch.chainReduced;
				hdr.reads = scratch.reads;
				// K1: the stamp of the pair's later frame is read AFTER the pins (a late event then either precedes this read, and is honoured, or
				// the seal sees the mismatch and the reader goes live). Never the stamp the engine made the field under.
				hdr.honoured = lease.indicator ? lease.indicator->StampOf(p.to) : VpIndicator::Stamp();
				const VpSkeleton::PublishResult pr = set->Publish(ticket);
				pins.Unpin();
				if (pr == VpSkeleton::PublishResult::Published)
				{
					published_.fetch_add(1, std::memory_order_relaxed);
					return Outcome::Published;
				}
				stale_.fetch_add(1, std::memory_order_relaxed);
				return Outcome::Stale;
			}

			void Park(const VpMotion::P2Published& p)
			{
				std::lock_guard<std::mutex> lock(ringMutex_);
				if (count_ == kPendingCap)
				{
					head_ = (head_ + 1) % kPendingCap;   // the oldest goes
					--count_;
					overwritten_.fetch_add(1, std::memory_order_relaxed);
				}
				ring_[(head_ + count_) % kPendingCap] = p;
				++count_;
				pendingApprox_.store(count_, std::memory_order_release);
			}

			LeaseFn lease_;
			GenerationFn generation_;
			std::mutex claimMutex_;                 // one claimant at a time (the slot protocol has one producer per product)
			mutable std::mutex ringMutex_;
			std::array<VpMotion::P2Published, kPendingCap> ring_;
			size_t head_ = 0, count_ = 0;
			std::atomic<size_t> pendingApprox_{ 0 };
			std::atomic<uint64_t> offered_{ 0 }, published_{ 0 }, parked_{ 0 }, retried_{ 0 }, overwritten_{ 0 }, stale_{ 0 }, noLease_{ 0 }, generationDropped_{ 0 }, renderTryBusy_{ 0 };
		};
	}
}
