#include "pch.h"
#include "CppUnitTest.h"

#include <vprenderer/HookSegment.h>
#include <vprenderer/HookSegmentPool.h>

#include <deque>

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
			// a hook libplacebo disabled is left in place and does not split (libplacebo skips it)
			Assert::IsTrue(HookSegment::GroupRuns({ kPre, kPre, kPre }, { true, true, true }, { false, true, false }) == Runs({ 0, -1, 0 }));
			// a non-member (e.g. a hook that failed inside a merged hook) splits: nothing is reordered
			Assert::IsTrue(HookSegment::GroupRuns({ kPre, kPre, kPre, kPre, kPre }, { true, true, false, true, true },
				{ false, false, false, false, false }) == Runs({ 0, 0, -1, 1, 1 }));
		}

		// Runner::Fuse without a GPU (no texture is created before a merged hook runs).
		struct Fakes
		{
			std::deque<pl_hook> hooks;
			static pl_hook_res Noop(void*, const pl_hook_params*) { return pl_hook_res{}; }
			const pl_hook* Make(uint32_t stage, uint64_t signature)
			{
				pl_hook h{};
				h.stages = static_cast<enum pl_hook_stage>(stage);
				h.input = PL_HOOK_SIG_TEX;
				h.hook = &Noop;
				h.signature = signature;
				hooks.push_back(h);
				return &hooks.back();
			}
		};

		TEST_METHOD(FuseNeverMovesAFailedHook)
		{
			const uint32_t kPre = 1u << 14, kOut = 1u << 15;
			Fakes f;
			const pl_hook* a = f.Make(kPre, 1); const pl_hook* b = f.Make(kPre, 2); const pl_hook* x = f.Make(kPre, 3);
			const pl_hook* c = f.Make(kPre, 4); const pl_hook* d = f.Make(kPre, 5);
			const pl_hook* r = f.Make(kOut, 6); const pl_hook* m = f.Make(kOut, 7); const pl_hook* s = f.Make(kOut, 8);
			std::vector<const pl_hook*> in = { a, b, x, c, d, r, m, s }, out;
			HookSegment::Runner runner(nullptr);
			auto all = [](const pl_hook*) { return true; };
			runner.Fuse(in, out, all);
			Assert::AreEqual<size_t>(2, out.size(), L"one PRE_OUTPUT and one OUTPUT hook");
			Assert::AreEqual<size_t>(2, runner.MergedCount());
			// x fails inside the merged hook: from then on it runs at its own place.
			runner.MarkFailed(x, "test");
			runner.Fuse(in, out, all);
			Assert::AreEqual<size_t>(4, out.size());
			Assert::IsTrue(out[1] == x, L"the failed hook keeps its place between b and c");
			// The mask failing in S3 [repair, mask, dither]: the dither stays last.
			runner.MarkFailed(m, "test");
			runner.Fuse(in, out, all);
			Assert::IsTrue(out.back() == s, L"the dither stays last");
			Assert::IsTrue(out[out.size() - 2] == m);
			Assert::IsTrue(out[out.size() - 3] == r);
			Assert::AreEqual<size_t>(2, runner.TakeFailures().size());
			// A settings change forgets the failures and salts the merged signatures.
			runner.ClearFailed();
			runner.Fuse(in, out, all);
			Assert::AreEqual<size_t>(2, out.size());
		}

		TEST_METHOD(FuseReportsAMergedHookLibplaceboDisabled)
		{
			const uint32_t kPre = 1u << 14;
			Fakes f;
			std::vector<const pl_hook*> in = { f.Make(kPre, 11), f.Make(kPre, 12) }, out;
			HookSegment::Runner runner(nullptr);
			runner.Fuse(in, out, [](const pl_hook*) { return true; });
			Assert::AreEqual<size_t>(1, out.size());
			const uint64_t merged = out[0]->signature;
			pl_render_errors errors{};
			errors.disabled_hooks = &merged;
			errors.num_disabled_hooks = 1;
			const auto disabled = runner.DisabledRuns(&errors);
			Assert::AreEqual<size_t>(1, disabled.size());
			Assert::IsTrue(disabled[0] == in);
			// After ClearFailed the next merge has a new signature (not pre-disabled).
			runner.ClearFailed();
			runner.Fuse(in, out, [](const pl_hook*) { return true; });
			Assert::IsTrue(out[0]->signature != merged);
			Assert::IsTrue(runner.DisabledRuns(&errors).empty());
			// An inner hook libplacebo disabled is left in place and is not merged.
			const uint64_t inner = in[1]->signature;
			errors.disabled_hooks = &inner;
			runner.Fuse(in, out, [](const pl_hook*) { return true; }, &errors);
			Assert::IsTrue(out == in);
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
