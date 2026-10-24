#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// S0 motion segment + sampled timers (2026-10-24 draft, feat/motion-timing-extract). CPU only:
// the vectors-valid state as the segment owns it (vp-vector-validity), direct delivery per
// consumer with counted fallbacks, smooth-motion exclusivity, low-latency backward vectors
// (current + last), the visible engine->matcher downgrade, and timers that never drain.

#include <VP-V/modules/motion/MotionSegment.h>
#include <VP-V/modules/motion/SampledPassTimers.h>

#include <deque>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpMotion;

namespace
{
	struct FakeField { int id = 0; };
	using Segment = MotionSegment<FakeField>;

	// A fake GPU: a timer's result becomes readable `lag` polls after issue; Finish is never offered.
	struct FakeGpu
	{
		struct Pending { int timer; int pollsLeft; uint64_t ns; };
		std::deque<Pending> pending;
		int created = 0, destroyed = 0, queries = 0;
	};
	struct FakeOps
	{
		FakeGpu* gpu = nullptr;
		int Create() { return ++gpu->created; }
		uint64_t Query(int timer)
		{
			++gpu->queries;
			for (auto it = gpu->pending.begin(); it != gpu->pending.end(); ++it)
				if (it->timer == timer)
				{
					if (it->pollsLeft-- > 0) return 0;
					const uint64_t ns = it->ns;
					gpu->pending.erase(it);
					return ns;
				}
			return 0;
		}
		void Destroy(int& timer) { ++gpu->destroyed; timer = 0; }
	};
}

namespace VpvTests
{
	TEST_CLASS(MotionSegmentTests)
	{
	public:
		TEST_METHOD(ConsumersFetchValidPairsDirectly)
		{
			Segment s;
			const uint64_t e = s.Validity().Epoch();
			s.Publish(Direction::Forward, 10, 11, FakeField{ 7 }, Source::EngineAhead, e, 1);
			auto d = s.FetchFrom(Consumer::DejudderWarp, 10, 1);
			Assert::IsTrue(static_cast<bool>(d));
			Assert::AreEqual(7, d.field->id);
			Assert::IsTrue(d.source == Source::EngineAhead);
			Assert::IsTrue(static_cast<bool>(s.FetchAt(Consumer::OcclusionMask, 11, 1)));   // B's mask: the pair ending at B
			Assert::IsTrue(static_cast<bool>(s.Fetch(Consumer::Interpolator, Direction::Forward, 10, 11, 1)));
			Assert::AreEqual(uint64_t{ 1 }, s.Stats(Consumer::DejudderWarp).uses);
		}

		TEST_METHOD(EveryDiscontinuityMakesEveryConsumerFallBack)
		{
			const MotionValidity::Cause causes[] = { MotionValidity::Cause::Resync, MotionValidity::Cause::QueueReset,
				MotionValidity::Cause::Gap, MotionValidity::Cause::DriftDrop, MotionValidity::Cause::Seek,
				MotionValidity::Cause::RebaseChange, MotionValidity::Cause::SourceFormatChange,
				MotionValidity::Cause::MotionSourceChange, MotionValidity::Cause::SettingsChange };
			for (auto cause : causes)
			{
				Segment s;
				s.Publish(Direction::Forward, 5, 6, FakeField{ 1 }, Source::Matcher, s.Validity().Epoch(), 3);
				s.Validity().Invalidate(cause);
				for (size_t c = 0; c < kConsumerCount; ++c)
				{
					const auto d = s.FetchFrom(static_cast<Consumer>(c), 5, 3);
					Assert::IsFalse(static_cast<bool>(d));
					Assert::IsTrue(d.miss == Miss::Stale);
				}
			}
		}

		TEST_METHOD(EstimateStraddlingAnInvalidationIsNeverDelivered)
		{
			Segment s;
			const uint64_t inputsEpoch = s.Validity().Epoch();
			s.Validity().Invalidate(MotionValidity::Cause::Gap);   // during the estimate
			s.Publish(Direction::Forward, 1, 2, FakeField{ 1 }, Source::Matcher, inputsEpoch, 0);
			Assert::IsTrue(s.FetchFrom(Consumer::DitherExtras, 1, 0).miss == Miss::Stale);
		}

		TEST_METHOD(RepeatsGapsAndGeometryFallBack)
		{
			Segment s;
			s.Publish(Direction::Forward, 20, 21, FakeField{ 1 }, Source::Matcher, s.Validity().Epoch(), 4);
			Assert::IsTrue(s.Fetch(Consumer::DitherNext, Direction::Forward, 20, 20, 4).miss == Miss::NotAdjacent);   // a repeat
			Assert::IsTrue(s.Fetch(Consumer::DitherNext, Direction::Forward, 20, 22, 4).miss == Miss::NotAdjacent);   // a hole
			Assert::IsTrue(s.FetchFrom(Consumer::DitherNext, 20, 5).miss == Miss::Geometry);
			Assert::IsTrue(s.FetchFrom(Consumer::DitherNext, 21, 4).miss == Miss::NotEstimated);
			Assert::AreEqual(uint64_t{ 4 }, s.Stats(Consumer::DitherNext).fallbacks);
			Assert::IsTrue(s.Stats(Consumer::DitherNext).lastMiss == Miss::NotEstimated);
		}

		TEST_METHOD(SmoothMotionExcludesTheEngine)
		{
			Segment s;
			s.Publish(Direction::Forward, 1, 2, FakeField{ 1 }, Source::EngineAhead, s.Validity().Epoch(), 0);
			s.Engage(false);   // libplacebo smooth motion selected
			Assert::IsTrue(s.FetchFrom(Consumer::Interpolator, 1, 0).miss == Miss::Disengaged);
			s.Engage(true);    // back: nothing made before the switch is used
			Assert::IsTrue(s.FetchFrom(Consumer::Interpolator, 1, 0).miss == Miss::Stale);
		}

		TEST_METHOD(NewestEstimateOfAPairWins)
		{
			Segment s;
			const uint64_t e = s.Validity().Epoch();
			for (int i = 0; i < 6; ++i)   // wraps the 4-slot ring
				s.Publish(Direction::Forward, 30, 31, FakeField{ i }, i % 2 ? Source::Matcher : Source::EngineAtRender, e, 0);
			Assert::AreEqual(5, s.FetchFrom(Consumer::GrainMap, 30, 0).field->id);
		}

		TEST_METHOD(LowLatencyKeepsCurrentAndLastBackwardFields)
		{
			Segment s;
			const uint64_t e = s.Validity().Epoch();
			s.Publish(Direction::Backward, 40, 41, FakeField{ 41 }, Source::Matcher, e, 0);
			// Frame 42 just arrived, its (41, 42) field not ready: a frame-old field is allowed for statistics.
			auto d = s.FetchBackward(Consumer::LowLatency, 42, true, 0);
			Assert::IsTrue(static_cast<bool>(d));
			Assert::AreEqual(41, d.field->id);
			Assert::IsFalse(static_cast<bool>(s.FetchBackward(Consumer::LowLatency, 42, false, 0)));   // precision users: current only
			s.Publish(Direction::Backward, 41, 42, FakeField{ 42 }, Source::Matcher, e, 0);
			Assert::AreEqual(42, s.FetchBackward(Consumer::LowLatency, 42, false, 0).field->id);
			// Forward and backward never mix.
			Assert::IsTrue(s.Fetch(Consumer::DejudderWarp, Direction::Forward, 41, 42, 0).miss == Miss::NotEstimated);
			Assert::AreEqual(uint64_t{ 1 }, s.Stats(Consumer::LowLatency).fallbacks);   // the frame-old retry counted once
		}

		TEST_METHOD(SustainedMatcherFallbackIsVisible)
		{
			Segment s;
			const uint64_t e = s.Validity().Epoch();
			for (uint64_t i = 1; i < Segment::kVisibleMatcherRun; ++i)
				s.Publish(Direction::Forward, i, i + 1, FakeField{}, Source::Matcher, e, 0);
			Assert::IsTrue(s.FallbackEntry().empty());
			s.Publish(Direction::Forward, 100, 101, FakeField{}, Source::Matcher, e, 0);
			Assert::IsFalse(s.FallbackEntry().empty());
			s.Publish(Direction::Forward, 101, 102, FakeField{}, Source::EngineAhead, e, 0);
			Assert::IsTrue(s.FallbackEntry().empty());
			s.SetEngineWanted(false);   // optical_flow off: the matcher is the choice, not a fallback
			for (uint64_t i = 0; i < 20; ++i)
				s.Publish(Direction::Forward, 200 + i, 201 + i, FakeField{}, Source::Matcher, e, 0);
			Assert::IsTrue(s.FallbackEntry().empty());
		}

		TEST_METHOD(TimersSampleOneFrameInNAndOnlyPoll)
		{
			FakeGpu gpu;
			SampledPassTimers<int, FakeOps> timers(FakeOps{ &gpu }, 4);
			int timed = 0;
			for (int frame = 0; frame < 16; ++frame)
			{
				if (timers.BeginFrame())
				{
					++timed;
					const int t = timers.TimerFor("matcher search");
					Assert::IsTrue(t != 0);
					gpu.pending.push_back({ t, 2, 1500000 });   // 1.5 ms, readable after two polls
				}
				else
					Assert::AreEqual(0, timers.TimerFor("matcher search"));
				timers.Poll();   // never blocks: results arrive frames later
			}
			Assert::AreEqual(4, timed);
			Assert::AreEqual(1, gpu.created);   // one timer per pass description, reused
			Assert::IsTrue(timers.Samples("matcher search") >= 3);
			Assert::AreEqual(1.5, timers.MeanMs("matcher search"), 1e-9);
			timers.Release();
			Assert::AreEqual(1, gpu.destroyed);
		}
	};
}
