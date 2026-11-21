// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P10 D5 phase A (refs/118 s3): STORAGE of the P13 source ring, luma-only first slice of EU-P13. input.capture owns it
// (ProductCatalog.h P13, "VIEWS of the unpacked planes of the queued frames (owned by queue/upload): 0 extra bytes").
// THE DEFINITION of the ring is core/RingContract.h (SourceRingContract / RingWindowFrames): a seq-keyed window of
// BACK + 1 + LEAD frames, both derived. This class is its storage and adds nothing to the contract.
//
// What it is: a small list of the frames still QUEUED ahead of the render, each with the hand-off object of its early-upload
// slot (EarlyUploadHandoff) and the raw frame's facts (encoding, row pitch, width, height, DV tunnel). It owns no texture and
// makes no copy: a reader (the P10 tap builder, phase C) finds a frame by (seq, epoch), peeks the hand-off
// (EarlyUploadHandoff::PeekAttached: never waits, never abandons) and reads the slot on the render device.
//
// Sizing: DERIVED at every Update, never a literal. The skeleton publishes the host's window into this ring's own
// QueueWindowBoard (Board().Publish, beside the memo board); Update reads it (QueueWindowBoard::Read) and holds
// RingWindowFrames(window.back, window.ahead) entries. A frame beyond that is refused and counted (overCapacity, shown on
// the skeleton_tap_source status); a pass before any window was published takes nothing (noWindow, counted).
//
// Key: (seq, epoch), epoch = the memo / queue-fact epoch: the larger of the published window's epoch and the epoch the queue fact
// itself delivered (OnQueueFact, any thread, at the fact: Flush / Discontinuity / FormatChange / Gap / Reset / LiveQueueReset), so a fact
// between two Begins is seen by the very next Feed. Readers key on memo->Epoch(). An epoch change moves every entry to the graveyard
// (released at the next Begin). An entry is also replaced when its seq is listed again with a different hand-off (a re-used seq).
//
// Decode: the walk passes the queued frame's extension slot OPAQUELY (core/SourceRingFeed.h AheadFrame); THIS file decodes the
// capture payload (motion never includes capture).
//
// Fed through the core interface (core/SourceRingFeed.h, carrier (a)): the existing S2 call site MotionAhead::CollectQueueAhead
// finds this ring's feed through SourceRingFeeds and calls Feed (via Update) on the render-loop thread WITH m_queueMutex held.
// Lock order: queue mutex -> ring mutex; a reader never takes the queue mutex. The board is a seqlock (no lock at all).
//
// Lifetime law (refs/118 risk 2): nothing is destroyed under the queue lock. An entry that leaves the list is MOVED to the
// graveyard (a move destroys nothing); ReleaseGraveyard runs at the next S2 Begin (render thread, no queue lock) and is the
// only place an entry's reference is dropped, so the last reference to a hand-off, and with it the slot's return to the
// pool, never happens under m_queueMutex.
//
// Off by default: a ring does nothing until a reader declares itself with SetWanted(true) (the skeleton, when the lane is
// active and lane_tap_source = auto). Wanted() is one acquire atomic load: the walk builds no list when nobody wants one.
// Original pathway: the Original overlay turns the setting to cpu, so the ring is never fed. C++14, header-only.

#include <VP-V/core/QueueWindow.h>
#include <VP-V/core/RingContract.h>
#include <VP-V/core/SourceRingFeed.h>
#include <VP-V/modules/input/capture/CapturePayload.h>
#include <VP-V/modules/input/capture/EarlyUploadHandoff.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Vpv
{
	namespace Capture
	{
		class SourceRing : public VpSkeleton::ISourceRingFeed
		{
		public:
			// One ring entry: the decoded facts of a queued frame plus the epoch it was taken in (the key is (seq, epoch)).
			// It holds the early-upload hand-off only: never the payload extension (no raw frame buffer is kept alive).
			struct Entry
			{
				uint64_t seq = 0;
				uint64_t epoch = 0;
				std::shared_ptr<void> earlyHandoff;     // EarlyUploadHandoff; null = no early upload
				int32_t rawEncoding = 0;                // VideoFrameEncoding of the raw frame (0 = UNKNOWN: not raw)
				uint32_t rawRowBytes = 0;               // the raw frame's row pitch in bytes
				int32_t height = 0;                     // the rows the early slot holds
				int32_t width = 0;                      // the frame width in pixels (0 = not delivered)
				bool dvTunnel = false;                  // the raw frame is a Dolby Vision tunnel picture
				bool dvHighAligned = false;             // ... with RGB8 in the high eight bits of the channels
			};

			// Decodes one walk frame (the capture payload lives in the opaque extension slot).
			static void Decode(const VpSkeleton::AheadFrame& f, Entry& e)
			{
				e = Entry();
				e.seq = f.seq;
				e.width = f.width;
				e.height = f.height;
				const PayloadExtension* p = ExtensionOf(f.extension);
				if (!p)
					return;
				e.earlyHandoff = std::static_pointer_cast<void>(p->earlyHandoff);
				e.rawEncoding = static_cast<int32_t>(p->rawEncoding);
				e.rawRowBytes = static_cast<uint32_t>(p->rawRowBytes);
				e.height = p->earlyHeight;
				e.dvTunnel = p->dvTunnelRaw;
				e.dvHighAligned = p->dvTunnelHighAligned;
			}

			struct Stats
			{
				uint64_t updates = 0;        // Update calls that carried a list
				uint64_t added = 0;          // entries taken
				uint64_t displaced = 0;      // entries moved to the graveyard (left the queue list)
				uint64_t overCapacity = 0;   // entries refused because the derived window was full
				uint64_t noWindow = 0;       // passes that took nothing: no window was published yet
				uint64_t epochFlushes = 0;   // epoch changes that moved the whole ring to the graveyard
				uint64_t released = 0;       // entries dropped by ReleaseGraveyard
				size_t entries = 0;
				size_t graveyard = 0;
				size_t capacity = 0;         // the derived capacity of the last Update (RingWindowFrames; 0 = none yet)
				uint64_t epoch = 0;          // the epoch the entries carry
			};

			SourceRing() = default;
			SourceRing(const SourceRing&) = delete;
			SourceRing& operator=(const SourceRing&) = delete;
			~SourceRing() { SetWanted(false); }

			// ---- the window (derived sizing) ---------------------------------------------------------------------
			// The skeleton publishes the host's QueueWindow here at every Begin (one writer); Update reads it.
			VpSkeleton::QueueWindowBoard& Board() { return board_; }
			const VpSkeleton::QueueWindowBoard& Board() const { return board_; }

			// ---- the queue fact (any thread) -----------------------------------------------------------------------
			// SkeletonModule::OnQueue calls this with the memo's epoch AT the fact (after BumpEpoch); the next Update treats it as an
			// epoch change, whatever the last published window said. The board (single writer: Begin) is NOT touched.
			void OnQueueFact(uint64_t memoEpoch)
			{
				uint64_t cur = factEpoch_.load(std::memory_order_relaxed);
				while (memoEpoch > cur &&
					!factEpoch_.compare_exchange_weak(cur, memoEpoch, std::memory_order_release, std::memory_order_relaxed))
				{
				}
			}

			// ---- demand ------------------------------------------------------------------------------------------
			// Any thread. A reader declares itself (the skeleton, at its settings apply); idempotent per ring.
			void SetWanted(bool wanted)
			{
				const bool was = wanted_.exchange(wanted, std::memory_order_acq_rel);
				if (was == wanted)
					return;
				Hosts().fetch_add(wanted ? 1 : -1, std::memory_order_relaxed);
				if (!wanted)
					Clear();
			}
			bool Wanted() const override { return wanted_.load(std::memory_order_acquire); }
			// True when some ring (any renderer) is wanted (tests / diagnostics; the walk asks its own host's feed).
			static bool AnyWanted() { return Hosts().load(std::memory_order_relaxed) > 0; }

			// ---- producer: render-loop thread, queue lock held ---------------------------------------------------
			void Feed(const VpSkeleton::AheadFrame* frames, size_t count) override { Update(frames, count); }

			// `slots[0..count)`: the queue's frames from the front up to the lane lead. The ring afterwards holds exactly
			// those frames that are present (an entry keeps its place while its frame is listed; a new frame is added up to
			// the derived capacity; a frame no longer listed is moved to the graveyard). An epoch change first moves every
			// entry to the graveyard. Allocation-free once the vectors have grown to the capacity; a pass with no reader
			// is ignored. A frame is keyed by (seq, epoch).
			void Update(const VpSkeleton::AheadFrame* slots, size_t count)
			{
				if (!Wanted())
					return;
				VpSkeleton::QueueWindow window;
				const bool haveWindow = board_.Read(&window);
				std::lock_guard<std::mutex> lock(m_);
				// Re-checked under m_: SetWanted(false) clears after the exchange under m_, so a walk that passed the unlocked check
				// before teardown adds nothing after Clear (no hand-off can then die with the feed under m_queueMutex).
				if (!wanted_.load(std::memory_order_acquire))
					return;
				++stats_.updates;
				if (!haveWindow)
				{
					++stats_.noWindow;
					SweepLocked(nullptr, 0, epoch_);   // nothing can be sized: hold nothing
					return;
				}
				// THE P13 definition (core/RingContract.h): one formula, no second (rules 1 / 24). overCapacity is the refusal guard.
				capacity_ = VpSkeleton::RingWindowFrames(window.back, window.ahead);
				const uint64_t epoch = (std::max)(window.epoch, factEpoch_.load(std::memory_order_acquire));
				if (epoch != epoch_)
				{
					for (Entry& e : entries_)
						graveyard_.push_back(std::move(e));   // moves: nothing is destroyed under the caller's lock
					stats_.displaced += entries_.size();
					entries_.clear();
					++stats_.epochFlushes;
					epoch_ = epoch;
				}
				SweepLocked(slots, count, epoch_);
				for (size_t i = 0; i < count; ++i)
				{
					if (HasLocked(slots[i], epoch_))
						continue;
					if (entries_.size() >= capacity_)
					{
						++stats_.overCapacity;
						continue;
					}
					Entry e;
					Decode(slots[i], e);
					e.epoch = epoch_;
					entries_.push_back(std::move(e));
					++stats_.added;
				}
			}

			// ---- the S2 Begin: render thread, no queue lock -------------------------------------------------------
			// Drops the graveyard's references (the only place they are dropped). Returns how many entries it released.
			size_t ReleaseGraveyard()
			{
				std::vector<Entry> dead;   // destroyed after the lock is released
				{
					std::lock_guard<std::mutex> lock(m_);
					dead.swap(graveyard_);
					stats_.released += dead.size();
				}
				return dead.size();
			}

			// ---- reader: render thread ----------------------------------------------------------------------------
			// A copy of the entry for (seq, epoch), or false. The copy holds the hand-off, so the slot cannot be
			// returned to the pool while the reader uses it (the copy's release is the reader's, on the render thread).
			bool Find(uint64_t seq, uint64_t epoch, Entry& out) const
			{
				std::lock_guard<std::mutex> lock(m_);
				for (const Entry& e : entries_)
					if (e.seq == seq && e.epoch == epoch)
					{
						out = e;
						return true;
					}
				return false;
			}

			Stats GetStats() const
			{
				std::lock_guard<std::mutex> lock(m_);
				Stats s = stats_;
				s.entries = entries_.size();
				s.graveyard = graveyard_.size();
				s.capacity = capacity_;
				s.epoch = epoch_;
				return s;
			}

		private:
			static std::atomic<int>& Hosts()
			{
				static std::atomic<int> hosts(0);
				return hosts;
			}
			// Moves every entry whose (seq, epoch) is not listed to the graveyard (m_ held).
			void SweepLocked(const VpSkeleton::AheadFrame* slots, size_t count, uint64_t epoch)
			{
				for (size_t i = 0; i < entries_.size();)
				{
					if (entries_[i].epoch == epoch && ListedLocked(slots, count, entries_[i]))
					{
						++i;
						continue;
					}
					graveyard_.push_back(std::move(entries_[i]));   // a move: nothing is destroyed under the caller's lock
					if (i + 1 < entries_.size())
						entries_[i] = std::move(entries_.back());
					entries_.pop_back();
					++stats_.displaced;
				}
			}
			// The hand-off a walk frame carries (null: none).
			static const void* HandoffOf(const VpSkeleton::AheadFrame& f)
			{
				const PayloadExtension* p = ExtensionOf(f.extension);
				return p ? static_cast<const void*>(p->earlyHandoff.get()) : nullptr;
			}
			// An entry is still listed when its seq is listed WITH THE SAME hand-off (a re-used seq is another frame).
			static bool ListedLocked(const VpSkeleton::AheadFrame* slots, size_t count, const Entry& e)
			{
				for (size_t i = 0; i < count; ++i)
					if (slots[i].seq == e.seq && HandoffOf(slots[i]) == e.earlyHandoff.get())
						return true;
				return false;
			}
			bool HasLocked(const VpSkeleton::AheadFrame& f, uint64_t epoch) const
			{
				for (const Entry& e : entries_)
					if (e.seq == f.seq && e.epoch == epoch && e.earlyHandoff.get() == HandoffOf(f))
						return true;
				return false;
			}
			// SetWanted(false): the references go to the graveyard (released at the next Begin, or by the destructor's owner).
			void Clear()
			{
				std::lock_guard<std::mutex> lock(m_);
				for (Entry& e : entries_)
					graveyard_.push_back(std::move(e));
				entries_.clear();
			}

			std::atomic<bool> wanted_{ false };
			VpSkeleton::QueueWindowBoard board_;   // the host's window, published by the skeleton at Begin (a seqlock)
			mutable std::mutex m_;
			std::vector<Entry> entries_;     // under m_
			std::vector<Entry> graveyard_;   // under m_
			size_t capacity_ = 0;            // under m_: the derived capacity of the last Update
			uint64_t epoch_ = 0;             // under m_: the epoch the entries carry
			std::atomic<uint64_t> factEpoch_{ 0 };   // the epoch the last queue fact delivered (OnQueueFact; any thread)
			Stats stats_;                    // under m_
		};
	}
}
