#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

// One vectors-valid state (docs/VP-interpolator-50-60-plan.md, build order step 1;
// owner 2026-10-14, [[vp-vector-validity]]): every consumer of motion vectors
// (the de-judder's extras, the sigma-delta's VPVEC/VPPIX, the occlusion mask, the
// grain map, VP deband's hold, the producer's stored pairs, later the
// interpolator and the Stage 12 / delta 15 refinements) falls back to its
// no-vector path on a resync, a queue reset or seek, a frame gap, a drift
// correction drop, a smooth-motion mix, a re-base change, a source format
// change, a motion source change or a motion settings change.
//
// Pure and header-only: no GPU, no capture types, no logging. The renderer owns
// one State and wires the causes and the consumers to it.
//
// The model
// - An epoch counter, starting at kInitialEpoch. Invalidate(cause) moves it; a
//   vector pair, or any temporal state built from one, belongs to the epoch it
//   was made in and is usable only while that epoch is current.
// - A pair (from, to) is valid when both frames belong to the current epoch and
//   are adjacent in source sequence. Cadence repeats carry the sequence of the
//   frame they repeat, so excluding them is automatic: a repeat is never a pair
//   (from == to) and never breaks adjacency. A caller that knows a break the
//   sequence cannot show (missed capture frames keep the sequence contiguous:
//   the queue's gapBefore; a pulldown lock change) passes contiguous = false.
// - NoteKept(sequence), on the render thread for every presented source frame,
//   tracks the kept run: a repeat is counted and excluded, the next sequence
//   continues the run, anything else (a hole: a drop; a step back) invalidates
//   with the given cause and starts a new run. This is the safety net for drops
//   that no other path reports.
// - The motion generation (the producer's VectorStore generation, today's
//   Impl::motionGeneration) is the epoch: Generation() == Epoch().
//
// Threads: Invalidate may be called from any thread (the capture callback, the
// UI thread's Reset / ResetLiveQueue, the render thread); Epoch / PairValid /
// Current / Count are lock-free and safe from any thread (the render thread, the
// producer's worker). NoteKept / KeptPairValid / LastKept take a small mutex
// (the kept run is two sequences and an epoch that must change together); they
// are meant for the render thread. Consume is meant for one consumer thread (the
// render thread's apply point).
namespace MotionValidity
{
	enum class Cause : int
	{
		Resync = 0,          // HDMI resync / DeckLink format change / renderer reset
		QueueReset,          // live queue reset (frames discarded)
		Gap,                 // missed capture frames, overflow / backlog drops, a sequence hole
		CadenceRepeat,       // a repeat that must break the pair (hard break; plain repeats go through NoteKept)
		DriftDrop,           // a frame dropped by drift (cadence) correction
		SmoothMotionMix,     // libplacebo's smooth motion engine / mixing switched on or off
		RebaseChange,        // the re-base factor (source plane size) changed
		SourceFormatChange,  // the frames' layout / format / range changed (SameDejudderVideoState false)
		Seek,                // a seek or skip in the source
		MotionSourceChange,  // optical-flow engine <-> matcher, the producer started / stopped
		SettingsChange,      // a motion setting that changes what the vectors mean
	};
	constexpr size_t kCauseCount = 11;

	inline size_t Index(Cause cause)
	{
		const int i = static_cast<int>(cause);
		return i >= 0 && static_cast<size_t>(i) < kCauseCount ? static_cast<size_t>(i) : kCauseCount;
	}

	// A short token, for counters and key=value logs.
	inline const char* Name(Cause cause)
	{
		switch (cause)
		{
		case Cause::Resync: return "resync";
		case Cause::QueueReset: return "queue_reset";
		case Cause::Gap: return "gap";
		case Cause::CadenceRepeat: return "cadence_repeat";
		case Cause::DriftDrop: return "drift_drop";
		case Cause::SmoothMotionMix: return "smooth_motion_mix";
		case Cause::RebaseChange: return "rebase_change";
		case Cause::SourceFormatChange: return "source_format_change";
		case Cause::Seek: return "seek";
		case Cause::MotionSourceChange: return "motion_source_change";
		case Cause::SettingsChange: return "settings_change";
		}
		return "unknown";
	}

	// A phrase for the log line ("motion invalidated (%s; ...)").
	inline const char* Describe(Cause cause)
	{
		switch (cause)
		{
		case Cause::Resync: return "resync or source change";
		case Cause::QueueReset: return "live queue reset";
		case Cause::Gap: return "frame gap";
		case Cause::CadenceRepeat: return "cadence repeat";
		case Cause::DriftDrop: return "drift correction drop";
		case Cause::SmoothMotionMix: return "smooth motion mixing changed";
		case Cause::RebaseChange: return "re-base factor changed";
		case Cause::SourceFormatChange: return "source format changed";
		case Cause::Seek: return "seek or skip";
		case Cause::MotionSourceChange: return "motion source changed";
		case Cause::SettingsChange: return "motion settings changed";
		}
		return "unknown cause";
	}

	// Causes after which the renderer releases the de-judder's GPU resources
	// (today's RequestMotionInvalidate(true)); the others only forget history.
	inline bool ReleasesResources(Cause cause)
	{
		return cause == Cause::Resync || cause == Cause::SourceFormatChange;
	}

	// Two source frames are a pair only when `to` directly follows `from`.
	inline bool Adjacent(uint64_t from, uint64_t to)
	{
		return from != 0 && to == from + 1;
	}

	enum class Continuity
	{
		First,       // the first kept frame of a run (start, or after an invalidation): no pair ends here
		Contiguous,  // follows the last kept frame: (last, this) is a pair
		Repeat,      // the same source frame again (cadence repeat, redraw): excluded, the run continues
		Break,       // a hole or a step back: invalidated, a new run starts here
	};

	inline const char* Describe(Continuity c)
	{
		switch (c)
		{
		case Continuity::First: return "first";
		case Continuity::Contiguous: return "contiguous";
		case Continuity::Repeat: return "repeat";
		case Continuity::Break: return "break";
		}
		return "unknown";
	}

	// What changed since the consumer's last look (the render thread's apply point).
	struct Pending
	{
		bool changed = false;          // the epoch moved: forget the vector-derived history
		bool release = false;          // a cause that releases resources was among them
		Cause cause = Cause::Gap;      // the most recent cause (for the log)
		uint64_t epoch = 0;            // the epoch now
		uint64_t invalidations = 0;    // how many since the last look
	};

	class State
	{
	public:
		static constexpr uint64_t kInitialEpoch = 1;

		State() = default;
		State(const State&) = delete;
		State& operator=(const State&) = delete;

		// Any thread. Moves the epoch; returns the new one. Everything made before
		// (pairs, masks, grain, hold history, stored vectors) is stale from here.
		uint64_t Invalidate(Cause cause) noexcept
		{
			const size_t i = Index(cause);
			if (i >= kCauseCount)
				return epoch.load(std::memory_order_acquire);
			// The cause and the release request are published before the epoch, so a
			// reader that sees the new epoch (acquire) sees them too.
			lastCause.store(static_cast<int>(cause), std::memory_order_relaxed);
			if (ReleasesResources(cause))
				releasePending.store(true, std::memory_order_relaxed);
			counts[i].fetch_add(1, std::memory_order_relaxed);
			return epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
		}

		// Any thread.
		uint64_t Epoch() const noexcept { return epoch.load(std::memory_order_acquire); }
		// The motion generation (VectorStore / MotionAhead::Producer) is the epoch.
		uint64_t Generation() const noexcept { return Epoch(); }
		// The coarse generation that moves only when the stream itself restarts
		// (resync, queue reset, seek, source format change): exactly today's
		// Impl::motionGeneration when Reset() invalidates with Resync and
		// ResetLiveQueue() with QueueReset. For the producer's store and the
		// look-ahead engagement while step 1 must not change behaviour (a gap moves
		// the epoch, and keying the store on the epoch would discard the ahead pairs
		// computed for the frames after the gap). Monotonic; any thread.
		uint64_t StreamGeneration() const noexcept
		{
			return kInitialEpoch + Count(Cause::Resync) + Count(Cause::QueueReset) + Count(Cause::Seek) +
				Count(Cause::SourceFormatChange);
		}
		bool Current(uint64_t madeInEpoch) const noexcept { return madeInEpoch != 0 && madeInEpoch == Epoch(); }

		uint64_t Count(Cause cause) const noexcept
		{
			const size_t i = Index(cause);
			return i < kCauseCount ? counts[i].load(std::memory_order_relaxed) : 0;
		}
		uint64_t Total() const noexcept
		{
			uint64_t total = 0;
			for (const auto& c : counts)
				total += c.load(std::memory_order_relaxed);
			return total;
		}
		Cause LastCause() const noexcept { return static_cast<Cause>(lastCause.load(std::memory_order_acquire)); }
		// Repeats seen by NoteKept (excluded from pairs; not invalidations).
		uint64_t Repeats() const noexcept { return repeats.load(std::memory_order_relaxed); }

		// Any thread. Valid only when both frames were taken in the current epoch and
		// `to` directly follows `from` in source sequence (repeats share their
		// frame's sequence, so they are excluded by construction).
		bool PairValid(uint64_t from, uint64_t to, uint64_t epochAtFrom, uint64_t epochAtTo) const noexcept
		{
			return PairValid(from, to, epochAtFrom, epochAtTo, true);
		}
		// The same, with the caller's own contiguity knowledge: false for a break the
		// sequence does not show (the next frame's gapBefore, a pulldown lock change,
		// a film update).
		bool PairValid(uint64_t from, uint64_t to, uint64_t epochAtFrom, uint64_t epochAtTo, bool contiguous) const noexcept
		{
			if (!contiguous || !Adjacent(from, to) || epochAtFrom != epochAtTo)
				return false;
			return Current(epochAtFrom);
		}

		// Render thread: every presented source frame, in presentation order (cadence
		// repeats included: they are recognised and excluded). A hole or a step back
		// invalidates with `holeCause` and starts a new run. Sequence 0 means "no
		// source sequence" and is ignored (First, nothing changes).
		Continuity NoteKept(uint64_t sequence, Cause holeCause = Cause::Gap)
		{
			if (sequence == 0)
				return Continuity::First;
			std::lock_guard<std::mutex> lock(keptMutex);
			const uint64_t now = Epoch();
			if (lastKept == 0 || keptEpoch != now)
			{
				// After an invalidation (or at the start) the run starts over; the same
				// frame shown again right after still counts as its repeat.
				if (lastKept == sequence && keptEpoch != now)
				{
					previousKept = 0;
					keptEpoch = now;
					repeats.fetch_add(1, std::memory_order_relaxed);
					return Continuity::Repeat;
				}
				StartRunLocked(sequence, now);
				return Continuity::First;
			}
			if (sequence == lastKept)
			{
				repeats.fetch_add(1, std::memory_order_relaxed);
				return Continuity::Repeat;
			}
			if (sequence == lastKept + 1)
			{
				previousKept = lastKept;
				lastKept = sequence;
				return Continuity::Contiguous;
			}
			StartRunLocked(sequence, Invalidate(holeCause));
			return Continuity::Break;
		}

		// Render thread: (from, to) are the last two kept frames of the current run in
		// the current epoch (a causal consumer: the pair that ends at the frame now shown).
		bool KeptPairValid(uint64_t from, uint64_t to) const
		{
			std::lock_guard<std::mutex> lock(keptMutex);
			return previousKept != 0 && from == previousKept && to == lastKept && Current(keptEpoch);
		}
		uint64_t LastKept() const
		{
			std::lock_guard<std::mutex> lock(keptMutex);
			return lastKept;
		}
		// The epoch the last kept frame was taken in (its "epochAtFrom" for the pair
		// starting there; 0 = none).
		uint64_t EpochOfLastKept() const
		{
			std::lock_guard<std::mutex> lock(keptMutex);
			return lastKept != 0 ? keptEpoch : 0;
		}

		// One consumer thread (the render thread's apply point, today's
		// ApplyMotionInvalidate): what changed since `seenEpoch`, which it updates.
		Pending Consume(uint64_t& seenEpoch) noexcept
		{
			Pending p;
			p.epoch = Epoch();
			if (p.epoch == seenEpoch)
				return p;
			p.changed = true;
			p.invalidations = p.epoch > seenEpoch ? p.epoch - seenEpoch : 1;
			p.release = releasePending.exchange(false, std::memory_order_acq_rel);
			p.cause = LastCause();
			seenEpoch = p.epoch;
			return p;
		}

	private:
		void StartRunLocked(uint64_t sequence, uint64_t inEpoch)
		{
			previousKept = 0;
			lastKept = sequence;
			keptEpoch = inEpoch;
		}

		std::atomic<uint64_t> epoch{ kInitialEpoch };
		std::atomic<int> lastCause{ static_cast<int>(Cause::Gap) };
		std::atomic<bool> releasePending{ false };
		std::array<std::atomic<uint64_t>, kCauseCount> counts{};
		std::atomic<uint64_t> repeats{ 0 };

		mutable std::mutex keptMutex;
		uint64_t lastKept = 0, previousKept = 0, keptEpoch = 0;
	};

	// The consumer gate: a consumer that keeps vector-derived state (a mask for
	// frame B, a field for (A, B), VP deband's hold weights, the next-frame VPVEC)
	// binds it to the pair and epoch it was made in and checks before each use.
	// Owned by one thread (the consumer's); the State it checks is shared.
	class ConsumerGate
	{
	public:
		// At production: the pair and the epoch the inputs were taken in (by default now).
		void Bind(uint64_t from, uint64_t to, uint64_t madeInEpoch)
		{
			boundFrom = from;
			boundTo = to;
			boundEpoch = madeInEpoch;
		}
		void Bind(const State& state, uint64_t from, uint64_t to) { Bind(from, to, state.Epoch()); }
		void Clear() { boundFrom = boundTo = boundEpoch = 0; }

		bool Bound() const { return boundEpoch != 0; }
		uint64_t From() const { return boundFrom; }
		uint64_t To() const { return boundTo; }
		uint64_t EpochBound() const { return boundEpoch; }

		// Usable for exactly this pair in the current epoch.
		bool Usable(const State& state, uint64_t from, uint64_t to) const
		{
			return Bound() && from == boundFrom && to == boundTo && state.Current(boundEpoch);
		}
		// Usable while presenting `sequence` as the pair's first frame (the extras,
		// the next-frame vectors made from A's field) or as its last (B's mask).
		bool UsableFrom(const State& state, uint64_t sequence) const
		{
			return Bound() && sequence != 0 && sequence == boundFrom && state.Current(boundEpoch);
		}
		bool UsableAt(const State& state, uint64_t sequence) const
		{
			return Bound() && sequence != 0 && sequence == boundTo && state.Current(boundEpoch);
		}

		// Usable(...), counted: the uses and the fallbacks, for the consumer's log line.
		bool Check(const State& state, uint64_t from, uint64_t to)
		{
			const bool ok = Usable(state, from, to);
			++(ok ? uses : fallbacks);
			return ok;
		}
		bool CheckAt(const State& state, uint64_t sequence)
		{
			const bool ok = UsableAt(state, sequence);
			++(ok ? uses : fallbacks);
			return ok;
		}
		uint64_t Uses() const { return uses; }
		uint64_t Fallbacks() const { return fallbacks; }

	private:
		uint64_t boundFrom = 0, boundTo = 0, boundEpoch = 0;
		uint64_t uses = 0, fallbacks = 0;
	};
}
