#pragma once

// S0 MOTION SEGMENT (VP-ONLY, 2026-10-24 draft; central-migration-needs.md section 6, segment S0).
//
// Today the renderer host estimates (UploadDejudderPlanes -> RunDejudderEstimate -> producer Take /
// dejudder->Estimate), then hands the vectors to each consumer itself: the interpolator's Motion
// (assembled twice: VP phase picture 7369-7382 and the extras 17810-17820), the sigma-delta's VPVEC
// on extras (17895-17935) and on B's first present (24860-24890), the occlusion mask / grain map
// (7554, 24604-24660) and the B3 entrance snapshot (6089-6192). Each hand-off re-checks validity
// with its own expression (motionValidity.Current(...) + sequence compares + geometry).
//
// The segment owns, in one place:
//   - the ONE vectors-valid state (MotionValidity::State; vp-vector-validity);
//   - the published fields per pair (forward A->B for de-judder/interpolation; backward n-1->n for
//     low latency, current AND last kept: vp-low-latency-vectors) with the estimate's source
//     (engine ahead / engine at render / matcher) and its epoch + geometry generation;
//   - delivery: consumers FETCH (pair, or "pair starting at", or "pair ending at") and get either a
//     valid field or a counted fallback with a reason. No host hand-offs.
//   - visibility (vp-no-silent-fallbacks): a sustained engine->matcher downgrade becomes a
//     "Fallbacks engaged" entry; routine per-pair fallbacks (repeats, cuts) are counters only.
//
// Header-only and GPU-free: Field is the renderer's GPU bundle (pl_tex field/trust/sub/mask) in the
// adapter, a plain struct in the CPU tests. Engine/matcher EXCLUSIVITY with libplacebo smooth
// motion (vp-motion-engine-exclusive) is enforced by Engage(): while smooth motion runs the segment
// is disengaged and every fetch falls back.

#include <MotionValidity.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace VpMotion
{
	enum class Consumer : int
	{
		DejudderWarp = 0,    // de-judder extras' warp
		Interpolator,        // VP phase pictures + interpolated extras (Stage A)
		DitherExtras,        // sigma-delta VPVEC/VPPIX on extras
		DitherNext,          // sigma-delta VPVEC on B's first present (made from A's field)
		OcclusionMask,       // 6c mask for the sigma-delta / interpolator
		GrainMap,            // adaptive grain map
		EntranceSnapshot,    // B3: (A, B) snapshot after B's field was estimated ahead
		TextureRestore,      // texture restore / detail pulse vectors
		LowLatency,          // 50/60p backward-vector users (dither, scaling, artifact detection)
	};
	constexpr size_t kConsumerCount = 9;

	inline const char* Name(Consumer c)
	{
		static const char* const names[kConsumerCount] = { "dejudder_warp", "interpolator", "dither_extras",
			"dither_next", "occlusion_mask", "grain_map", "entrance_snapshot", "texture_restore", "low_latency" };
		const int i = static_cast<int>(c);
		return i >= 0 && static_cast<size_t>(i) < kConsumerCount ? names[i] : "unknown";
	}

	enum class Source : int { None = 0, EngineAhead, EngineAtRender, Matcher };
	enum class Direction : int { Forward = 0, Backward };

	inline const char* Name(Source s)
	{
		switch (s)
		{
		case Source::EngineAhead: return "optical flow (ahead)";
		case Source::EngineAtRender: return "optical flow (at render)";
		case Source::Matcher: return "matcher";
		default: return "none";
		}
	}

	enum class Miss : int
	{
		None = 0,
		Disengaged,       // smooth motion / off / low latency without backward vectors
		NotEstimated,     // no field published for that pair
		Stale,            // epoch moved since the estimate (resync, gap, drift drop, ...)
		NotAdjacent,      // the pair is not two adjacent kept frames
		Geometry,         // the consumer's geometry changed since the estimate
	};

	inline const char* Describe(Miss m)
	{
		switch (m)
		{
		case Miss::Disengaged: return "motion disengaged";
		case Miss::NotEstimated: return "no estimate for the pair";
		case Miss::Stale: return "vectors invalidated";
		case Miss::NotAdjacent: return "not adjacent frames";
		case Miss::Geometry: return "geometry changed";
		default: return "ok";
		}
	}

	template <class Field>
	struct Delivery
	{
		const Field* field = nullptr;   // non-null only when valid; valid until the next Publish
		Miss miss = Miss::NotEstimated;
		Source source = Source::None;
		uint64_t from = 0, to = 0;
		explicit operator bool() const { return field != nullptr; }
	};

	struct ConsumerStats
	{
		uint64_t uses = 0, fallbacks = 0;
		Miss lastMiss = Miss::None;
	};

	template <class Field, size_t Slots = 4>
	class MotionSegment
	{
	public:
		// Engine downgrade visibility: this many consecutive matcher estimates while the engine is
		// wanted make a "Fallbacks engaged" entry (cleared by the next engine estimate).
		static constexpr unsigned kVisibleMatcherRun = 8;

		// external: share the host's state while the host still owns it (shadow step, P-b1);
		// nullptr: the segment owns it (the target).
		explicit MotionSegment(MotionValidity::State* external = nullptr) : m_validity(external ? *external : m_own) {}
		MotionSegment(const MotionSegment&) = delete;
		MotionSegment& operator=(const MotionSegment&) = delete;

		MotionValidity::State& Validity() { return m_validity; }
		const MotionValidity::State& Validity() const { return m_validity; }

		// Engine/matcher vs libplacebo smooth motion: one or the other (vp-motion-engine-exclusive).
		// Disengaging invalidates (SmoothMotionMix) so nothing made before is used after.
		void Engage(bool engaged)
		{
			if (engaged == m_engaged)
				return;
			m_engaged = engaged;
			m_validity.Invalidate(MotionValidity::Cause::SmoothMotionMix);
		}
		bool Engaged() const { return m_engaged; }
		void SetEngineWanted(bool wanted) { m_engineWanted = wanted; if (!wanted) m_matcherRun = 0; }

		// Called by the estimation part of the segment once a field exists. epochAtInputs: the
		// validity epoch when the inputs were taken (estimates that straddle an invalidation are
		// stored but never delivered).
		// fillPair: a gap-fill pair (hunt 2 L1: a pair spanning missed capture frames on purpose),
		// delivered although `to` is not `from + 1`.
		void Publish(Direction direction, uint64_t from, uint64_t to, const Field& field, Source source,
			uint64_t epochAtInputs, uint64_t geometryGeneration, bool fillPair = false)
		{
			Slot& s = m_slots[m_next];
			m_next = (m_next + 1) % Slots;
			s.used = true;
			s.direction = direction;
			s.from = from;
			s.to = to;
			s.field = field;
			s.source = source;
			s.epoch = epochAtInputs;
			s.geometry = geometryGeneration;
			s.serial = ++m_serial;
			s.fillPair = fillPair;
			if (direction == Direction::Forward)
			{
				if (source == Source::Matcher && m_engineWanted)
					++m_matcherRun;
				else if (source != Source::Matcher)
					m_matcherRun = 0;
			}
		}

		// The live outputs are being replaced or released (the host's dejudderLiveFieldFrom = 0 sites:
		// an estimate in progress, a release, a reset): nothing published so far in `direction` is
		// delivered any more. Unlike Invalidate this does not move the epoch (other state stays valid).
		void Retract(Direction direction)
		{
			for (Slot& s : m_slots)
				if (s.direction == direction)
					s.used = false;
		}

		// The field for exactly (from, to).
		Delivery<Field> Fetch(Consumer consumer, Direction direction, uint64_t from, uint64_t to, uint64_t geometryGeneration)
		{
			Delivery<Field> d;
			d.from = from;
			d.to = to;
			d.miss = Evaluate(direction, from, to, geometryGeneration, d);
			return Count(consumer, d);
		}

		// The pair that starts at `from` (extras of A, next-frame VPVEC made from A's field).
		Delivery<Field> FetchFrom(Consumer consumer, uint64_t from, uint64_t geometryGeneration)
		{
			return Fetch(consumer, Direction::Forward, from, from + 1, geometryGeneration);
		}

		// The pair that ends at `to` (B's occlusion mask).
		Delivery<Field> FetchAt(Consumer consumer, uint64_t to, uint64_t geometryGeneration)
		{
			return Fetch(consumer, Direction::Forward, to > 0 ? to - 1 : 0, to, geometryGeneration);
		}

		// Low latency (vp-low-latency-vectors): the newest valid backward pair ending at `to`
		// (current refresh), or the one before it (a frame-old field for statistics/grain maps).
		Delivery<Field> FetchBackward(Consumer consumer, uint64_t to, bool allowFrameOld, uint64_t geometryGeneration)
		{
			Delivery<Field> d = Fetch(consumer, Direction::Backward, to > 0 ? to - 1 : 0, to, geometryGeneration);
			if (!d && allowFrameOld && to > 1)
			{
				--m_stats[Index(consumer)].fallbacks;   // counted once, below
				d = Fetch(consumer, Direction::Backward, to - 2, to - 1, geometryGeneration);
			}
			return d;
		}

		const ConsumerStats& Stats(Consumer consumer) const { return m_stats[Index(consumer)]; }

		// "" when clean; else one "Fallbacks engaged" entry (ADR-0001 format).
		std::string FallbackEntry() const
		{
			if (m_engineWanted && m_engaged && m_matcherRun >= kVisibleMatcherRun)
				return "motion: matcher (optical flow unavailable)";
			return std::string();
		}

	private:
		struct Slot
		{
			bool used = false;
			Direction direction = Direction::Forward;
			uint64_t from = 0, to = 0;
			Field field{};
			Source source = Source::None;
			uint64_t epoch = 0, geometry = 0;
			uint64_t serial = 0;   // publish order: the newest estimate of a pair wins
			bool fillPair = false;
		};

		static size_t Index(Consumer c)
		{
			const int i = static_cast<int>(c);
			return i >= 0 && static_cast<size_t>(i) < kConsumerCount ? static_cast<size_t>(i) : 0;
		}

		Miss Evaluate(Direction direction, uint64_t from, uint64_t to, uint64_t geometryGeneration, Delivery<Field>& d) const
		{
			if (!m_engaged)
				return Miss::Disengaged;
			if (from == 0 || to <= from)
				return Miss::NotAdjacent;   // a repeat or a step back is never a pair
			const Slot* found = nullptr;
			for (const Slot& s : m_slots)
				if (s.used && s.direction == direction && s.from == from && s.to == to &&
					(!found || s.serial > found->serial))
					found = &s;
			if (!found)
				return MotionValidity::Adjacent(from, to) ? Miss::NotEstimated : Miss::NotAdjacent;
			if (!MotionValidity::Adjacent(from, to) && !found->fillPair)
				return Miss::NotAdjacent;
			if (!m_validity.Current(found->epoch))
				return Miss::Stale;
			if (found->geometry != geometryGeneration)
				return Miss::Geometry;
			d.field = &found->field;
			d.source = found->source;
			return Miss::None;
		}

		Delivery<Field> Count(Consumer consumer, Delivery<Field> d)
		{
			ConsumerStats& s = m_stats[Index(consumer)];
			if (d)
				++s.uses;
			else
			{
				++s.fallbacks;
				s.lastMiss = d.miss;
			}
			return d;
		}

		MotionValidity::State m_own;
		MotionValidity::State& m_validity;
		std::array<Slot, Slots> m_slots{};
		size_t m_next = 0;
		uint64_t m_serial = 0;
		std::array<ConsumerStats, kConsumerCount> m_stats{};
		bool m_engaged = true;
		bool m_engineWanted = true;
		unsigned m_matcherRun = 0;
	};
}
