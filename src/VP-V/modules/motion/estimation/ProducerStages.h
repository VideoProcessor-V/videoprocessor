// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Producer throughput plan (refs/105 s3.2, s4 row 2, slice 16 ph2a): pure CPU pipeline bookkeeping
// between the ENGINE thread (D3D11 context + ONE NVOF session owner) and the BACK thread (parallel
// forward/backward FlowGlobal vote + FlowExtras + ordered P2 slot Publish / on-demand Answer).
//
// Components (pure C++14, no D3D11 headers):
//   - StagingRing<N> (default N = 3): tracks N staging readback sets (Free | Submitted | Ready) in
//     FIFO submit order, with an ENGINE-thread affinity debug assert (105 s7 risk 3) and stale
//     generation discard so in-flight sets of an older generation never publish.
//   - BackQueue<Item>: thread-safe FIFO between ENGINE and BACK with generation/epoch discard on
//     Stop or generation change (105 s3.2 last bullet).
//   - OrderedPublishGate: guarantees P2 Publish never regresses sequence order within a generation.
#pragma once

#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace MotionAhead
{
	namespace Pipeline
	{
		enum class SlotState : uint8_t
		{
			Free = 0,
			Submitted = 1,
			Ready = 2
		};

		struct JobKey
		{
			uint64_t generation = 0;
			uint64_t fromSeq = 0;
			uint64_t toSeq = 0;
			bool isRequest = false;
			bool breakBefore = false;
		};

		// ENGINE-thread-only ring of N staging sets (default 3: one running on OFA, one copying/ready,
		// one being mapped). Tracks FIFO submit order and generation-based discard.
		template <size_t N = 3>
		class StagingRing
		{
		public:
			static constexpr size_t kCapacity = N;
			static constexpr int kNoSlot = -1;

			struct Slot
			{
				SlotState state = SlotState::Free;
				JobKey key{};
				uint64_t epoch = 0;
				uint64_t order = 0;
				bool discarded = false;
				std::chrono::steady_clock::time_point submittedAt{};
			};

			// Bind (or rebind on restart) the single ENGINE thread that owns this ring and the D3D11 context.
			void BindEngineThread()
			{
				engineThread_ = std::this_thread::get_id();
				threadBound_ = true;
			}

			void UnbindEngineThread()
			{
				threadBound_ = false;
				engineThread_ = std::thread::id{};
			}

			void AssertEngineThread() const
			{
				if (threadBound_)
					assert(std::this_thread::get_id() == engineThread_ && "StagingRing accessed off the ENGINE thread");
			}

			int AcquireFree() const
			{
				AssertEngineThread();
				for (size_t i = 0; i < N; ++i)
					if (slots_[i].state == SlotState::Free)
						return static_cast<int>(i);
				return kNoSlot;
			}

			bool MarkSubmitted(int index, const JobKey& key, uint64_t epoch, std::chrono::steady_clock::time_point now)
			{
				AssertEngineThread();
				if (index < 0 || static_cast<size_t>(index) >= N)
					return false;
				Slot& s = slots_[static_cast<size_t>(index)];
				if (s.state != SlotState::Free)
					return false;
				s.state = SlotState::Submitted;
				s.key = key;
				s.epoch = epoch;
				s.order = ++nextOrder_;
				s.discarded = false;
				s.submittedAt = now;
				return true;
			}

			int OldestSubmitted() const
			{
				AssertEngineThread();
				int best = kNoSlot;
				uint64_t bestOrder = UINT64_MAX;
				for (size_t i = 0; i < N; ++i)
				{
					if (slots_[i].state == SlotState::Submitted && slots_[i].order < bestOrder)
					{
						bestOrder = slots_[i].order;
						best = static_cast<int>(i);
					}
				}
				return best;
			}

			int OldestReady() const
			{
				AssertEngineThread();
				int best = kNoSlot;
				uint64_t bestOrder = UINT64_MAX;
				for (size_t i = 0; i < N; ++i)
				{
					if (slots_[i].state == SlotState::Ready && slots_[i].order < bestOrder)
					{
						bestOrder = slots_[i].order;
						best = static_cast<int>(i);
					}
				}
				return best;
			}

			bool MarkReady(int index)
			{
				AssertEngineThread();
				if (index < 0 || static_cast<size_t>(index) >= N)
					return false;
				Slot& s = slots_[static_cast<size_t>(index)];
				if (s.state != SlotState::Submitted)
					return false;
				s.state = SlotState::Ready;
				return true;
			}

			void Release(int index)
			{
				AssertEngineThread();
				if (index < 0 || static_cast<size_t>(index) >= N)
					return;
				slots_[static_cast<size_t>(index)] = Slot{};
			}

			// Mark any in-flight slot belonging to an older generation as discarded (its GPU query still
			// completes safely before the staging buffers are reused, and it is never forwarded to BACK).
			size_t MarkDiscardedOtherThan(uint64_t keepGeneration)
			{
				AssertEngineThread();
				size_t marked = 0;
				for (Slot& s : slots_)
				{
					if (s.state != SlotState::Free && !s.discarded && s.key.generation != keepGeneration)
					{
						s.discarded = true;
						++marked;
					}
				}
				return marked;
			}

			size_t MarkAllDiscarded()
			{
				AssertEngineThread();
				size_t marked = 0;
				for (Slot& s : slots_)
				{
					if (s.state != SlotState::Free && !s.discarded)
					{
						s.discarded = true;
						++marked;
					}
				}
				return marked;
			}

			void Clear()
			{
				AssertEngineThread();
				for (Slot& s : slots_)
					s = Slot{};
			}

			const Slot& At(int index) const
			{
				AssertEngineThread();
				return slots_[static_cast<size_t>(index)];
			}

			size_t FreeCount() const
			{
				AssertEngineThread();
				size_t n = 0;
				for (const Slot& s : slots_)
					if (s.state == SlotState::Free) ++n;
				return n;
			}

			size_t SubmittedCount() const
			{
				AssertEngineThread();
				size_t n = 0;
				for (const Slot& s : slots_)
					if (s.state == SlotState::Submitted) ++n;
				return n;
			}

			size_t InFlightCount() const
			{
				AssertEngineThread();
				return N - FreeCount();
			}

		private:
			std::array<Slot, N> slots_{};
			uint64_t nextOrder_ = 0;
			bool threadBound_ = false;
			std::thread::id engineThread_{};
		};

		// Strict publish-order gate for a generation: guarantees P2 pairs never publish out of sequence order.
		class OrderedPublishGate
		{
		public:
			bool Accept(uint64_t generation, uint64_t fromSeq, uint64_t toSeq);
			void Reset();
			void SetGeneration(uint64_t generation);
			uint64_t Generation() const;
			uint64_t LastPublishedTo() const;
			bool HasPublished() const;

		private:
			mutable std::mutex mutex_;
			bool haveGeneration_ = false;
			uint64_t generation_ = 0;
			bool havePublished_ = false;
			uint64_t lastTo_ = 0;
		};

		// Thread-safe FIFO queue from ENGINE to BACK.
		// Supports bounded depth, generation-scoped discard, and epoch-scoped discard on Stop/restart.
		template <class Item>
		class BackQueue
		{
		public:
			struct Entry
			{
				Item item;
				uint64_t generation = 0;
				uint64_t epoch = 0;
				bool ignoreGeneration = false;   // e.g. forced off-chain requests that do not belong to the slot's generation
			};

			explicit BackQueue(size_t maxPending = 8) : maxPending_(maxPending < 1 ? 1 : maxPending) {}

			// Push an item in FIFO order; blocks briefly if BACK has reached maxPending (or returns false if closed).
			bool Push(Item item, uint64_t generation, uint64_t epoch, bool ignoreGeneration = false)
			{
				std::unique_lock<std::mutex> lock(mutex_);
				spaceCv_.wait(lock, [this] { return closed_ || queue_.size() < maxPending_; });
				if (closed_ || epoch != currentEpoch_)
					return false;
				if (!ignoreGeneration && generationPinned_ && generation != activeGeneration_)
					return false;
				queue_.push_back(Entry{ std::move(item), generation, epoch, ignoreGeneration });
				dataCv_.notify_one();
				return true;
			}

			// Pop the next live item in FIFO order; skips any stale-generation/epoch entries.
			// Sets busy_ = true under mutex_ before returning true so WaitIdle() cannot observe an
			// empty-and-idle window between Pop() and consumer processing; callers must pair each
			// true return with MarkBusy(false) once processing completes.
			// Returns false only when closed and drained (or discarded).
			bool Pop(Entry& out)
			{
				std::unique_lock<std::mutex> lock(mutex_);
				for (;;)
				{
					dataCv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
					while (!queue_.empty())
					{
						Entry candidate = std::move(queue_.front());
						queue_.pop_front();
						spaceCv_.notify_one();
						if (candidate.epoch != currentEpoch_)
						{
							++discarded_;
							continue;
						}
						if (!candidate.ignoreGeneration && generationPinned_ && candidate.generation != activeGeneration_)
						{
							++discarded_;
							continue;
						}
						out = std::move(candidate);
						busy_ = true;
						return true;
					}
					if (!busy_)
						idleCv_.notify_all();
					if (closed_)
						return false;
				}
			}

			// Advance the active generation and immediately discard queued items of other generations.
			size_t SetActiveGeneration(uint64_t generation)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				generationPinned_ = true;
				activeGeneration_ = generation;
				size_t dropped = 0;
				for (auto it = queue_.begin(); it != queue_.end();)
				{
					if (!it->ignoreGeneration && it->generation != generation)
					{
						it = queue_.erase(it);
						++dropped;
						++discarded_;
					}
					else
					{
						++it;
					}
				}
				if (dropped)
				{
					spaceCv_.notify_all();
					if (queue_.empty() && !busy_ && externalBusy_ == 0)
						idleCv_.notify_all();
				}
				return dropped;
			}

			bool IsLive(uint64_t generation, uint64_t epoch, bool ignoreGeneration = false) const
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (closed_ || epoch != currentEpoch_)
					return false;
				if (!ignoreGeneration && generationPinned_ && generation != activeGeneration_)
					return false;
				return true;
			}

			// Discard all queued items and bump epoch (Stop / ReleaseEngine / restart).
			size_t DiscardAllAndBumpEpoch()
			{
				std::lock_guard<std::mutex> lock(mutex_);
				++currentEpoch_;
				generationPinned_ = false;
				const size_t dropped = queue_.size();
				discarded_ += dropped;
				queue_.clear();
				spaceCv_.notify_all();
				if (dropped && !busy_ && externalBusy_ == 0)
					idleCv_.notify_all();
				return dropped;
			}

			// Wait until the queue is empty and BACK (including any handed-off R6 extras) is idle.
			void WaitIdle()
			{
				std::unique_lock<std::mutex> lock(mutex_);
				idleCv_.wait(lock, [this] { return !busy_ && externalBusy_ == 0 && (queue_.empty() || closed_); });
			}

			void MarkBusy(bool busy)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				busy_ = busy;
				if (!busy_ && externalBusy_ == 0 && (queue_.empty() || closed_))
					idleCv_.notify_all();
			}

			void AcquireExternalBusy()
			{
				std::lock_guard<std::mutex> lock(mutex_);
				++externalBusy_;
			}

			void ReleaseExternalBusy()
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (externalBusy_ > 0)
					--externalBusy_;
				if (!busy_ && externalBusy_ == 0 && (queue_.empty() || closed_))
					idleCv_.notify_all();
			}

			// Stop pushing new items while allowing BACK to drain already-queued live items in FIFO order.
			void Finish()
			{
				{
					std::lock_guard<std::mutex> lock(mutex_);
					closed_ = true;
				}
				dataCv_.notify_all();
				spaceCv_.notify_all();
				idleCv_.notify_all();
			}

			void Close()
			{
				{
					std::lock_guard<std::mutex> lock(mutex_);
					closed_ = true;
					++currentEpoch_;
					discarded_ += queue_.size();
					queue_.clear();
				}
				dataCv_.notify_all();
				spaceCv_.notify_all();
				idleCv_.notify_all();
			}

			void Reopen()
			{
				std::lock_guard<std::mutex> lock(mutex_);
				closed_ = false;
				busy_ = false;
				externalBusy_ = 0;
				generationPinned_ = false;
				queue_.clear();
			}

			uint64_t Epoch() const
			{
				std::lock_guard<std::mutex> lock(mutex_);
				return currentEpoch_;
			}

			size_t Size() const
			{
				std::lock_guard<std::mutex> lock(mutex_);
				return queue_.size();
			}

			uint64_t DiscardedCount() const
			{
				std::lock_guard<std::mutex> lock(mutex_);
				return discarded_;
			}

		private:
			mutable std::mutex mutex_;
			std::condition_variable dataCv_;
			std::condition_variable spaceCv_;
			std::condition_variable idleCv_;
			std::deque<Entry> queue_;
			size_t maxPending_ = 8;
			uint64_t currentEpoch_ = 1;
			uint64_t activeGeneration_ = 0;
			bool generationPinned_ = false;
			bool closed_ = false;
			bool busy_ = false;
			int externalBusy_ = 0;
			uint64_t discarded_ = 0;
		};
	}
}
