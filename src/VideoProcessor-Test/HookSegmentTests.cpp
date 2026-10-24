#include "pch.h"
#include "CppUnitTest.h"

#include <vprenderer/HookSegmentPool.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// DRAFT (2026-10-24): the pure parts of HookSegment (pool reuse, run grouping, sampling).
namespace VideoProcessorTest
{
	TEST_CLASS(HookSegmentTests)
	{
	public:
		TEST_METHOD(PoolReusesAcrossRunsAndNeverTwiceInOneRun)
		{
			int live = 0, made = 0;
			HookSegment::TexturePool<int*, int> pool(
				[&](int, int, int) -> int* { ++live; return new int(++made); },
				[&](int* t) { --live; delete t; });
			pool.BeginRun();
			int* a = pool.Get(1920, 1080, 1);
			int* b = pool.Get(1920, 1080, 1);
			Assert::IsTrue(a != b);
			pool.BeginRun();
			Assert::IsTrue(pool.Get(1920, 1080, 1) == a);
			Assert::IsTrue(pool.Get(1920, 1080, 1) == b);
			Assert::AreEqual<uint64_t>(2, pool.Created());
			Assert::AreEqual<uint64_t>(2, pool.Reused());
			pool.BeginRun();
			pool.Get(3840, 2160, 1);   // a size change
			pool.Trim();
			Assert::AreEqual<size_t>(1, pool.Size());
			Assert::AreEqual(1, live);
			pool.Clear();
			Assert::AreEqual(0, live);
		}

		TEST_METHOD(PoolCountsFailedAllocations)
		{
			HookSegment::TexturePool<int*, int> pool([](int, int, int) -> int* { return nullptr; }, [](int*) {});
			pool.BeginRun();
			Assert::IsNull(pool.Get(8, 8, 0));
			Assert::AreEqual<uint64_t>(1, pool.Failed());
			Assert::AreEqual<size_t>(0, pool.Size());
		}

		TEST_METHOD(RunsGroupAdjacentMembersAtOneStage)
		{
			const uint32_t kPre = 1u << 14, kOut = 1u << 15, kMain = 1u << 2;
			using Runs = std::vector<int>;
			// three members at PRE: one run
			Assert::IsTrue(HookSegment::GroupRuns({ kPre, kPre, kPre }, { true, true, true }, { false, false, false }) == Runs({ 0, 0, 0 }));
			// a non-member at PRE splits; the single remainder is not merged
			Assert::IsTrue(HookSegment::GroupRuns({ kPre, kPre, kPre }, { true, false, true }, { false, false, false }) == Runs({ -1, -1, -1 }));
			// a hook of another stage in between does not split
			Assert::IsTrue(HookSegment::GroupRuns({ kPre, kMain, kPre }, { true, false, true }, { false, false, false }) == Runs({ 0, -1, 0 }));
			// two stages: two runs
			Assert::IsTrue(HookSegment::GroupRuns({ kPre, kPre, kOut, kOut }, { true, true, true, true }, { false, false, false, false }) == Runs({ 0, 0, 1, 1 }));
			// a multi-stage member is never merged and splits its stages' runs
			Assert::IsTrue(HookSegment::GroupRuns({ kPre, kPre | kOut, kPre }, { true, true, true }, { false, false, false }) == Runs({ -1, -1, -1 }));
			// a disabled hook is left alone and does not split
			Assert::IsTrue(HookSegment::GroupRuns({ kPre, kPre, kPre }, { true, true, true }, { false, true, false }) == Runs({ 0, -1, 0 }));
		}

		TEST_METHOD(SamplerTimesOneRunInN)
		{
			HookSegment::Sampler s(3);
			std::string pattern;
			for (int i = 0; i < 7; ++i)
				pattern += s.BeginRun() ? 'T' : '.';
			Assert::AreEqual(std::string("T..T..T"), pattern);
		}
	};
}
