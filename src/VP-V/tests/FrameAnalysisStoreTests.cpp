// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// WP-23 frame analysis store (src/VP-V/core/FrameAnalysisStore.h): the ring keyed by source
// sequence, the discontinuity epoch shared with vector validity, per-group validity, late and
// missing records (visible fallback text), captured repeats pointing at the original, ring depth
// from the delay budget, and the reader/writer threads. CPU only.

#include <VP-V/core/FrameAnalysisStore.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace FrameAnalysisStoreTestsNs
{
	using namespace FrameAnalysis;

	Identity Id(uint64_t seq)
	{
		Identity id;
		id.seq = seq;
		id.width = 3840;
		id.height = 2160;
		id.picture = { 0, 276, 3840, 1884 };
		return id;
	}

	LumaStats Stats(uint16_t code)
	{
		LumaStats stats;
		stats.histogram[code] = 100;
		stats.samples = 100;
		stats.mean = code;
		stats.minimum = stats.p1 = stats.p50 = stats.p99 = stats.maximum = code;
		return stats;
	}

	void Measure(Store& store, uint64_t seq, uint16_t code)
	{
		Assert::IsTrue(store.PublishMeasured(seq, kMeasuredGroups, Stats(code), RangeOccupancy{}, GrainEstimate{}));
	}

	TEST_CLASS(FrameAnalysisStoreTests)
	{
	public:
		TEST_METHOD(RingDepthComesFromTheBudget)
		{
			Assert::AreEqual(size_t(10), RingDepth(4, 4));
			Assert::AreEqual(size_t(7), RingDepth(2, 3));
			Assert::AreEqual(size_t(3), RingDepth(0, 0));   // a record always outlives its display
			Assert::AreEqual(size_t(3), RingDepth(-5, -1));
			Store store(RingDepth(3, 2));
			Assert::AreEqual(size_t(7), store.Depth());
		}

		TEST_METHOD(RecordIsLateUntilPublishedThenServed)
		{
			Store store(RingDepth(4, 4));
			Assert::AreNotEqual(uint64_t(0), store.Begin(Id(10), kMeasuredGroups));
			Record record;
			Assert::IsTrue(store.Read(10, Use::SingleFrame, kLuma, record) == ReadStatus::Late);
			Assert::IsTrue(store.Read(10, Use::SingleFrame, kIdentity, record) == ReadStatus::Ok);
			Assert::AreEqual(int(276), record.id.picture.top);
			Measure(store, 10, 500);
			Assert::IsTrue(store.Read(10, Use::Temporal, kLuma | kRange | kGrain, record) == ReadStatus::Ok);
			Assert::AreEqual(uint32_t(100), record.luma.histogram[500]);
			Assert::IsTrue(record.Complete());
		}

		TEST_METHOD(MissingAndNotMeasuredAreDistinct)
		{
			Store store(RingDepth(4, 4));
			Record record;
			Assert::IsTrue(store.Read(99, Use::SingleFrame, kLuma, record) == ReadStatus::Missing);
			store.Begin(Id(5), kMeasuredGroups);
			// Labels were never asked of the producer: "not measured", not "late".
			Assert::IsTrue(store.Read(5, Use::SingleFrame, kLabels, record) == ReadStatus::GroupInvalid);
		}

		TEST_METHOD(EveryDiscontinuityCauseStopsTemporalUse)
		{
			for (int c = 0; c < static_cast<int>(Discontinuity::Count); ++c)
			{
				Store store(RingDepth(4, 4));
				store.Begin(Id(1), kMeasuredGroups);
				Measure(store, 1, 400);
				const uint64_t before = store.Epoch();
				store.Invalidate(static_cast<Discontinuity>(c));
				Assert::AreEqual(before + 1, store.Epoch());
				Record record;
				Assert::IsTrue(store.Read(1, Use::Temporal, kLuma, record) == ReadStatus::StaleEpoch,
					L"a stale record must never serve temporal use");
				// Single-frame use: served, its own group bits are valid.
				Assert::IsTrue(store.Read(1, Use::SingleFrame, kLuma, record) == ReadStatus::Ok);
				Discontinuity last;
				Assert::IsTrue(store.LastCause(last));
				Assert::IsTrue(last == static_cast<Discontinuity>(c));
				Assert::AreEqual(uint64_t(1), store.Snapshot().invalidations[static_cast<size_t>(c)]);
			}
		}

		TEST_METHOD(MotionIsClearedAtADiscontinuityAndNeverSpansOne)
		{
			Store store(RingDepth(4, 4));
			store.Begin(Id(1), kMotion);
			MotionSummary motion;
			motion.vectorsValid = true;
			motion.pairFrom = 0;
			motion.source = VectorSource::EngineAhead;
			Assert::IsTrue(store.PublishMotion(1, motion));
			Record record;
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kMotion, record) == ReadStatus::Ok);
			Assert::IsTrue(record.motion.vectorsValid);
			store.Invalidate(Discontinuity::DroppedFrame);
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kMotion, record) == ReadStatus::GroupInvalid);
			// A pair landing after the discontinuity for a record opened before it is refused.
			Assert::IsFalse(store.PublishMotion(1, motion));
			// A record opened after it takes motion again.
			store.Begin(Id(2), kMotion);
			Assert::IsTrue(store.PublishMotion(2, motion));
		}

		TEST_METHOD(WindowNeedsOneCurrentEpoch)
		{
			Store store(RingDepth(4, 4));
			for (uint64_t s = 1; s <= 4; ++s)
			{
				store.Begin(Id(s), kMeasuredGroups);
				Measure(store, s, static_cast<uint16_t>(100 + s));
			}
			std::vector<Record> window;
			Assert::IsTrue(store.ReadWindow({ 1, 2, 3, 4 }, kLuma, window) == ReadStatus::Ok);
			Assert::AreEqual(size_t(4), window.size());
			Assert::AreEqual(uint32_t(100), window[2].luma.histogram[103]);
			store.Invalidate(Discontinuity::QueueFlush);
			store.Begin(Id(5), kMeasuredGroups);
			Measure(store, 5, 200);
			Assert::IsTrue(store.ReadWindow({ 4, 5 }, kLuma, window) == ReadStatus::StaleEpoch);
			Assert::IsTrue(window.empty());
			Assert::IsTrue(store.ReadWindow({ 5 }, kLuma, window) == ReadStatus::Ok);
			// A late frame inside the window fails the whole window.
			store.Begin(Id(6), kMeasuredGroups);
			Assert::IsTrue(store.ReadWindow({ 5, 6 }, kLuma, window) == ReadStatus::Late);
		}

		TEST_METHOD(RepeatPointsAtTheOriginalWithoutRemeasure)
		{
			Store store(RingDepth(4, 4));
			store.Begin(Id(1), kMeasuredGroups);
			Measure(store, 1, 612);
			store.Begin(Id(2), kMeasuredGroups);
			Assert::IsTrue(store.MarkRepeat(2, 1));
			Record record;
			Assert::IsTrue(store.Read(2, Use::Temporal, kLuma | kCadence, record) == ReadStatus::Ok);
			Assert::IsTrue(record.cadence.repeatOfPrevious);
			Assert::AreEqual(uint64_t(1), record.cadence.repeatOf);
			Assert::AreEqual(uint32_t(100), record.luma.histogram[612]);
			// A repeat of a repeat points at the original picture.
			store.Begin(Id(3), kMeasuredGroups);
			Assert::IsTrue(store.MarkRepeat(3, 2));
			Assert::IsTrue(store.Read(3, Use::SingleFrame, kCadence, record) == ReadStatus::Ok);
			Assert::AreEqual(uint64_t(1), record.cadence.repeatOf);
			Assert::AreEqual(uint64_t(2), store.Snapshot().repeats);
			// Not across a discontinuity, and not from an unmeasured original.
			store.Invalidate(Discontinuity::SeekOrReset);
			store.Begin(Id(4), kMeasuredGroups);
			Assert::IsFalse(store.MarkRepeat(4, 3));
			store.Begin(Id(5), kMeasuredGroups);
			Assert::IsFalse(store.MarkRepeat(5, 4));
		}

		TEST_METHOD(SlotsAreReusedAndReleased)
		{
			Store store(3);
			for (uint64_t s = 1; s <= 3; ++s) store.Begin(Id(s), kMeasuredGroups);
			store.Begin(Id(4), kMeasuredGroups);   // takes slot 1's place
			Record record;
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kIdentity, record) == ReadStatus::Missing);
			Assert::IsTrue(store.Read(4, Use::SingleFrame, kIdentity, record) == ReadStatus::Ok);
			Assert::AreEqual(uint64_t(1), store.Snapshot().evicted);
			store.Release(4);
			Assert::IsTrue(store.Read(4, Use::SingleFrame, kIdentity, record) == ReadStatus::Missing);
			Assert::IsFalse(store.PublishMeasured(4, kMeasuredGroups, Stats(1), RangeOccupancy{}, GrainEstimate{}));
		}

		TEST_METHOD(DisabledStoreServesNothing)
		{
			Store store(RingDepth(4, 4));
			store.SetEnabled(false);
			Assert::AreEqual(uint64_t(0), store.Begin(Id(1), kMeasuredGroups));
			Record record;
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kIdentity, record) == ReadStatus::Disabled);
			Assert::AreEqual(std::string("Frame analysis: off"), store.Summary());
		}

		TEST_METHOD(FallbackTextIsVisibleAndNamesTheConsumer)
		{
			const FallbackStatus::Entry entry = FallbackEntry("LED look-ahead", ReadStatus::Late);
			Assert::AreEqual(std::string("Fallbacks engaged: frame analysis (LED look-ahead: record late)"),
				FallbackStatus::Format({ entry }));
			Assert::AreEqual(std::string("record from before a discontinuity"),
				std::string(StatusText(ReadStatus::StaleEpoch)));
		}

		TEST_METHOD(SummaryCountsServedLateMissing)
		{
			Store store(RingDepth(2, 2));
			Record record;
			store.Begin(Id(1), kMeasuredGroups);
			store.Read(1, Use::SingleFrame, kLuma, record);   // late
			store.Read(7, Use::SingleFrame, kLuma, record);   // missing
			Measure(store, 1, 300);
			store.Read(1, Use::SingleFrame, kLuma, record);   // served
			const Counters c = store.Snapshot();
			Assert::AreEqual(uint64_t(1), c.late);
			Assert::AreEqual(uint64_t(1), c.missing);
			Assert::AreEqual(uint64_t(1), c.served);
			Assert::IsTrue(store.Summary().find("served 1, late 1, missing 1") != std::string::npos);
		}

		TEST_METHOD(ReadersAndWriterOnSeparateThreads)
		{
			// The arrival thread begins, the worker publishes, two readers read: every served record
			// is consistent (histogram matches its own sequence) and nothing deadlocks.
			Store store(RingDepth(4, 4));
			std::atomic<bool> stop{ false };
			std::atomic<uint64_t> inconsistent{ 0 }, served{ 0 };
			std::atomic<uint64_t> head{ 0 };
			auto reader = [&]()
			{
				Record record;
				while (!stop.load())
				{
					const uint64_t s = head.load();
					if (store.Read(s, Use::SingleFrame, kLuma, record) == ReadStatus::Ok)
					{
						++served;
						if (record.luma.histogram[s % 1000] != 100 || record.id.seq != s) ++inconsistent;
					}
				}
			};
			std::thread r1(reader), r2(reader);
			std::thread writer([&]()
			{
				for (uint64_t s = 1; s <= 20000; ++s)
				{
					store.Begin(Id(s), kMeasuredGroups);
					store.PublishMeasured(s, kMeasuredGroups, Stats(static_cast<uint16_t>(s % 1000)), RangeOccupancy{},
						GrainEstimate{});
					head.store(s);
					if (s % 997 == 0) store.Invalidate(Discontinuity::DroppedFrame);
				}
			});
			writer.join();
			stop.store(true);
			r1.join();
			r2.join();
			Assert::AreEqual(uint64_t(0), inconsistent.load());
			Assert::IsTrue(served.load() > 0);
		}
	};
}
