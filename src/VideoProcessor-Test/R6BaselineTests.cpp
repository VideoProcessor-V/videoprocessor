#include "pch.h"
#include "CppUnitTest.h"

// r6 baseline fixes (2026-10-24): the pure parts - "Fallbacks engaged" (fix 7), render-ahead
// inside the fixed delay budget (fix 4), the motion look-ahead as a time (fix 3), the
// [vprenderer.Default]-not-applied warning (fix 6) and the known-failure allow-list (fix 8).

#include <ConfigFile.h>
#include <VP-V/core/FallbackStatus.h>
#include <MotionAhead.h>
#include <RendererProfileConfig.h>
#include <vprenderer/RenderAheadPolicy.h>

#include <fstream>
#include <set>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace R6BaselineTest
{
	TEST_CLASS(FallbacksEngagedTests)
	{
	public:
		TEST_METHOD(NoneWhenClean)
		{
			FallbackStatus::Registry r;
			Assert::AreEqual(std::string("Fallbacks engaged: none"), r.Heading());
			Assert::IsTrue(FallbackStatus::Ids(r.Heading()).empty());
		}
		TEST_METHOD(PopulatedWithEachFallbackAndItsReason)
		{
			FallbackStatus::Registry r;
			Assert::IsTrue(r.Engage("capture GPU unpack", "CPU formatters for the session: cannot create texture"));
			Assert::IsFalse(r.Engage("capture GPU unpack", "CPU formatters for the session: cannot create texture"),
				L"the same reason again: no change");
			Assert::IsTrue(r.Engage("render-ahead", "off: its 33 ms lead does not fit the 209 ms delay budget; a; b"));
			Assert::AreEqual(std::string("Fallbacks engaged: capture GPU unpack (CPU formatters for the session: "
				"cannot create texture); render-ahead (off: its 33 ms lead does not fit the 209 ms delay budget; a; b)"), r.Heading());
			const auto ids = FallbackStatus::Ids(r.Heading());
			Assert::AreEqual(size_t{ 2 }, ids.size(), L"nested parentheses and '; ' inside a reason do not split");
			Assert::AreEqual(std::string("capture GPU unpack"), ids[0]);
			Assert::AreEqual(std::string("render-ahead"), ids[1]);
			Assert::IsTrue(r.Release("render-ahead"));
			Assert::IsFalse(r.Release("render-ahead"));
			Assert::IsTrue(r.Engaged("capture GPU unpack"));
			Assert::IsFalse(r.Engaged("render-ahead"));
		}
		TEST_METHOD(EachFallbackIsAnnouncedOncePerSession)
		{
			FallbackStatus::Notices n;
			auto first = n.Take("Fallbacks engaged: capture GPU unpack (x)");
			Assert::AreEqual(size_t{ 1 }, first.size());
			Assert::IsTrue(n.Take("Fallbacks engaged: capture GPU unpack (x)").empty());
			Assert::IsTrue(n.Take("Fallbacks engaged: none").empty());
			const auto second = n.Take("Fallbacks engaged: capture GPU unpack (x); Dolby Vision GPU unpack (y)");
			Assert::AreEqual(size_t{ 1 }, second.size());
			Assert::AreEqual(std::string("Dolby Vision GPU unpack"), second[0]);
			Assert::IsTrue(n.Take("Fallbacks engaged: capture GPU unpack (z)").empty(), L"released and engaged again: no second notice");
		}
	};

	TEST_CLASS(RenderAheadDelayBudgetTests)
	{
	public:
		// 23.976p on a 60 Hz display, the default queue (5 frames = 208.5 ms).
		TEST_METHOD(NoAddedLatencyAt24p)
		{
			const double ps = 1001.0 / 24000.0, pd = 1.0 / 60.0;
			// The credit off: the 33 ms lead would add delay - render-ahead stays off.
			const auto off = RenderAhead::FitDelayBudget(ps, pd, 5, false);
			Assert::IsFalse(off.fits);
			Assert::AreEqual(33.33, off.leadMs, 0.05);
			// The credit on: one frame given back covers the lead; the total stays inside the budget.
			const auto on = RenderAhead::FitDelayBudget(ps, pd, 5, true);
			Assert::IsTrue(on.fits);
			Assert::AreEqual(size_t{ 1 }, on.creditFrames);
			Assert::IsTrue(on.totalMs <= on.budgetMs + 0.5, L"never more delay than render_ahead off");
			Assert::AreEqual(5 * ps * 1000.0, on.budgetMs, 1e-6);
			// A queue too short to give a frame back (3 frames: A, B and one spare must stay).
			Assert::IsFalse(RenderAhead::FitDelayBudget(ps, pd, 3, true).fits);
			Assert::IsTrue(RenderAhead::FitDelayBudget(ps, pd, 4, true).fits);
		}
		TEST_METHOD(OtherRatesFollowTheSameRule)
		{
			// 50/60p on 60 Hz: no extras, no lead - always fits, nothing given back.
			const auto p60 = RenderAhead::FitDelayBudget(1.0 / 60.0, 1.0 / 60.0, 12, false);
			Assert::IsTrue(p60.fits);
			Assert::AreEqual(size_t{ 0 }, p60.creditFrames);
			// 30p on 60 Hz: a 16.7 ms lead, one frame back.
			const auto p30 = RenderAhead::FitDelayBudget(1.0 / 30.0, 1.0 / 60.0, 6, true);
			Assert::IsTrue(p30.fits);
			Assert::AreEqual(size_t{ 1 }, p30.creditFrames);
			// 24p on 120 Hz: 4 extras, 33 ms lead; 6 fps on 60 Hz: 9 extras, 150 ms lead (one frame back).
			Assert::IsTrue(RenderAhead::FitDelayBudget(1.0 / 24.0, 1.0 / 120.0, 5, true).fits);
			const auto slow = RenderAhead::FitDelayBudget(1.0 / 6.0, 1.0 / 60.0, 4, true);
			Assert::IsTrue(slow.fits);
			Assert::AreEqual(size_t{ 1 }, slow.creditFrames);
			// Unknown rates: nothing to plan, nothing vetoed.
			Assert::IsTrue(RenderAhead::FitDelayBudget(0.0, 1.0 / 60.0, 5, false).fits);
		}
		TEST_METHOD(TheLatchKeepsTheBudgetCredit)
		{
			RenderAhead::CreditLatch latch;
			Assert::AreEqual(size_t{ 1 }, latch.CommitFrames(1, 2.5));
			Assert::AreEqual(size_t{ 1 }, latch.Break(0.0));
			Assert::AreEqual(size_t{ 1 }, latch.Break(1.0));
			Assert::AreEqual(size_t{ 0 }, latch.Break(2.5), L"returned after the release time");
		}
	};

	TEST_CLASS(MotionLookAheadTimeTests)
	{
	public:
		TEST_METHOD(TwoAheadAt24pAndGracefulOnAShortQueue)
		{
			const double film = 24000.0 / 1001.0;
			const int depth = MotionAhead::DepthAt(4, film);
			Assert::AreEqual(2, depth, L"66.7 ms at 23.976p = 2 frames");
			Assert::IsTrue(MotionAhead::Decide(depth, false, true, 4).reason == MotionAhead::Reason::Active);
			const auto shallow = MotionAhead::Decide(depth, false, true, 1);
			Assert::AreEqual(1, shallow.depth, L"one queued ahead: one used, no waiting");
			Assert::IsTrue(MotionAhead::Decide(depth, false, true, 0).reason == MotionAhead::Reason::NoFramesAhead);
			// Low rates (6 fps) still look one frame ahead; high rates keep the time.
			Assert::AreEqual(1, MotionAhead::DepthAt(4, 6.0));
			Assert::AreEqual(8, MotionAhead::DepthAt(4, 120.0));
		}
	};

	TEST_CLASS(DefaultSectionWarningTests)
	{
	public:
		static std::string WarningFor(const char* text, const std::set<std::string>& applied = {})
		{
			char path[MAX_PATH] = {};
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			GetTempFileNameA(dir, "r6d", 0, path);
			std::ofstream(path) << text;
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			DeleteFileA(path);
			return RendererProfileConfig::DefaultSectionNotAppliedWarning(config, applied);
		}
		TEST_METHOD(WarnsWhenAMinimalFileDropsIt)
		{
			const std::string w = WarningFor("[vprenderer.Default]\ndejudder_strength: 0.25\n");
			Assert::IsFalse(w.empty());
			Assert::IsTrue(w.find("[vprenderer.Default] exists but is not applied") != std::string::npos);
		}
		TEST_METHOD(SilentWhenAppliedOrAbsentOrUnified)
		{
			Assert::IsTrue(WarningFor("[vprenderer.Default]\ndejudder_strength: 0.25\n", { "vprenderer.default" }).empty());
			Assert::IsTrue(WarningFor("[general]\npersist_profile_selection: false\n[vprenderer.Default]\nquality: high\n").empty());
			Assert::IsTrue(WarningFor("[vprenderer]\nquality: high\n").empty());
		}
	};
}
