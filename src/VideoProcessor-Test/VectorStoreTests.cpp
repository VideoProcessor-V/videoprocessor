#include "pch.h"
#include "CppUnitTest.h"

#include <VectorStore.h>

#include <atomic>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// docs/VP-nvof-ahead.md (owner 2026-10-16): vectors computed ahead of display,
// stored per source-frame pair; a miss means the matcher runs as today.
namespace VectorStoreTest
{
	TEST_CLASS(VectorStoreTests)
	{
	public:
		TEST_METHOD(AHitReturnsTheStoredPairAMissFallsBack)
		{
			VectorStore<int> store;
			store.SetGeneration(1);
			Assert::IsTrue(store.Put(10, 11, 1, 42));
			VectorStore<int>::Entry e;
			Assert::IsTrue(store.Get(10, 11, e));
			Assert::AreEqual(42, e.payload);
			Assert::IsFalse(store.Get(11, 12, e), L"not computed yet: a miss (the matcher runs)");
			const auto s = store.GetStats();
			Assert::AreEqual(1ull, s.hits);
			Assert::AreEqual(1ull, s.misses);
		}

		TEST_METHOD(ADiscontinuityClearsAndAStalePairIsRefused)
		{
			VectorStore<int> store;
			store.SetGeneration(1);
			store.Put(10, 11, 1, 1);
			store.Put(11, 12, 1, 2);
			store.SetGeneration(2);   // seek / resync / format change / drop
			VectorStore<int>::Entry e;
			Assert::IsFalse(store.Get(10, 11, e), L"pairs from before the discontinuity are gone");
			Assert::AreEqual(size_t{ 0 }, store.Size());
			// A pair finished after the discontinuity but computed before it.
			Assert::IsFalse(store.Put(12, 13, 1, 3), L"computed in the old generation: refused");
			Assert::IsTrue(store.Put(20, 21, 2, 4));
			Assert::AreEqual(1ull, store.GetStats().stale);
			Assert::AreEqual(1ull, store.GetStats().cleared);
		}

		TEST_METHOD(PruneAndCapacityKeepItSmall)
		{
			VectorStore<int> store(4);
			store.SetGeneration(7);
			for (uint64_t f = 1; f <= 6; ++f) store.Put(f, f + 1, 7, static_cast<int>(f));
			Assert::AreEqual(size_t{ 4 }, store.Size(), L"bounded");
			Assert::AreEqual(2ull, store.GetStats().evicted, L"oldest first out");
			Assert::IsFalse(store.Has(1, 2));
			Assert::IsTrue(store.Has(6, 7));
			store.Prune(5);   // rendering frame 5: pairs ending before it are not needed
			Assert::IsFalse(store.Has(3, 4));
			Assert::IsTrue(store.Has(4, 5), L"(prev, cur) kept for the causal consumers");
			Assert::IsTrue(store.Has(5, 6));
		}

		TEST_METHOD(PlanAsksForTheMissingPairsAheadNearestFirst)
		{
			VectorStore<int> store;
			store.SetGeneration(1);
			store.Put(100, 101, 1, 0);
			// Queued after the current frame 100: 101..106 (a cadence repeat is not queued twice).
			const std::vector<uint64_t> queued = { 101, 102, 103, 104, 105, 106 };
			auto plan = store.Plan(100, queued, 4, false);
			Assert::AreEqual(size_t{ 3 }, plan.size(), L"4 ahead, (100,101) already stored");
			Assert::IsTrue(plan[0] == std::make_pair(101ull, 102ull) && plan[2] == std::make_pair(103ull, 104ull));
			// With the two-frame span (detail accumulation v2).
			plan = store.Plan(100, queued, 4, true);
			Assert::AreEqual(size_t{ 6 }, plan.size());
			Assert::IsTrue(plan[1] == std::make_pair(100ull, 102ull));
			// A shallow queue: only what is there (no waiting, no latency added).
			plan = store.Plan(100, { 101 }, 4, true);
			Assert::AreEqual(size_t{ 0 }, plan.size());
		}

		TEST_METHOD(ProducerAndConsumerThreads)
		{
			// One producer storing ahead while the consumer renders and prunes.
			VectorStore<int> store(16);
			store.SetGeneration(1);
			std::atomic<uint64_t> produced{ 0 };
			std::thread producer([&] {
				for (uint64_t f = 1; f <= 2000; ++f) { store.Put(f, f + 1, 1, static_cast<int>(f)); produced = f; }
			});
			unsigned long long hits = 0;
			for (uint64_t f = 1; f <= 2000; ++f)
			{
				VectorStore<int>::Entry e;
				if (store.Get(f, f + 1, e)) { ++hits; Assert::AreEqual(static_cast<int>(f), e.payload); }
				store.Prune(f);
			}
			producer.join();
			Assert::IsTrue(store.Size() <= 16);
			const auto s = store.GetStats();
			Assert::AreEqual(2000ull, s.hits + s.misses, L"every request answered: a hit or a miss");
		}
	};
}
