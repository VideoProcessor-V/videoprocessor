#include "pch.h"
#include "CppUnitTest.h"

#include <DisplayTopologySession.h>
#include <GuiLifecyclePolicy.h>
#include <IntentDescriptor.h>
#include <RendererResetPolicy.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Bug hunt 2 (2026-10-17), GUI lifecycle: the shutdown deadline (every
// blocking kind, forced exit restores and keeps the crash state), the held
// `content` rule value, the native-over-DSR display mode choice and the
// renderer failure retry cap.
namespace VideoProcessorTest
{
	namespace
	{
		ContentIntentStatus Status(bool enabled, ContentClass content)
		{
			ContentIntentStatus status;
			status.enabled = enabled;
			status.content = content;
			status.source = IntentSource::Inferred;
			return status;
		}

		DisplayTopologySession::DisplayModeCandidate Mode(
			uint32_t width, uint32_t height, uint32_t refreshHz)
		{
			DisplayTopologySession::DisplayModeCandidate mode;
			mode.width = width;
			mode.height = height;
			mode.refreshHz = refreshHz;
			mode.bitsPerPixel = 32;
			return mode;
		}
	}

	TEST_CLASS(BugHunt2GuiTests)
	{
	public:
		TEST_METHOD(ShutdownDeadlineArmsOnceAtTheFirstCloseRequest)
		{
			ShutdownDeadline::Tracker tracker;
			Assert::IsTrue(tracker.Evaluate(500) == ShutdownDeadline::Decision::NotArmed);
			Assert::IsTrue(tracker.Arm(1000));
			// A second close request keeps the first deadline.
			Assert::IsFalse(tracker.Arm(9000));
			Assert::AreEqual<uint64_t>(1000 + ShutdownDeadline::ForcedExitDeadlineMs,
				tracker.Deadline());
			Assert::IsTrue(tracker.Evaluate(1000 + ShutdownDeadline::ForcedExitDeadlineMs - 1) ==
				ShutdownDeadline::Decision::Waiting);
			Assert::IsTrue(tracker.Evaluate(1000 + ShutdownDeadline::ForcedExitDeadlineMs) ==
				ShutdownDeadline::Decision::Expire);
			// Expire is reported once.
			Assert::IsTrue(tracker.Evaluate(1000 + ShutdownDeadline::ForcedExitDeadlineMs + 5) ==
				ShutdownDeadline::Decision::Expired);
		}

		TEST_METHOD(ShutdownDeadlineFailClosedRetirementOnlyBringsItForward)
		{
			ShutdownDeadline::Tracker tracker;
			tracker.Arm(0);
			// Fail-closed retirement known at 2 s: 2 s + 10 s < 20 s.
			tracker.BringForward(2000, ShutdownBlockedRetirementDeadlineMs);
			Assert::AreEqual<uint64_t>(2000 + ShutdownBlockedRetirementDeadlineMs,
				tracker.Deadline());
			// Polling it again later never pushes the deadline back.
			tracker.BringForward(8000, ShutdownBlockedRetirementDeadlineMs);
			Assert::AreEqual<uint64_t>(2000 + ShutdownBlockedRetirementDeadlineMs,
				tracker.Deadline());
			// Known late (at 15 s): the first-request deadline stays.
			ShutdownDeadline::Tracker late;
			late.Arm(0);
			late.BringForward(15000, ShutdownBlockedRetirementDeadlineMs);
			Assert::AreEqual<uint64_t>(ShutdownDeadline::ForcedExitDeadlineMs, late.Deadline());
			// Not armed (no close yet): BringForward arms it.
			ShutdownDeadline::Tracker unarmed;
			unarmed.BringForward(100, ShutdownBlockedRetirementDeadlineMs);
			Assert::IsTrue(unarmed.Armed());
		}

		TEST_METHOD(EveryBlockingKindIsCoveredAndTheForcedExitRestoresEverything)
		{
			using Blockers = ShutdownDeadline::Blockers;
			std::vector<Blockers> kinds(7);
			kinds[0].retirementPending = true;          // worker hung in Retire()
			kinds[1].failedRetirement = true;
			kinds[2].retirementShutdownBlocked = true;
			kinds[3].constructionActive = true;
			kinds[4].captureStopping = true;
			kinds[5].rendererPresent = true;
			kinds[6].capturePresent = true;
			for (const Blockers& kind : kinds)
			{
				Assert::IsTrue(ShutdownDeadline::AnyBlocking(kind));
				Assert::AreNotEqual(std::string("none"),
					std::string(ShutdownDeadline::Describe(kind)));
				const ShutdownDeadline::ForcedExitPlan plan =
					ShutdownDeadline::PlanForcedExit(kind);
				Assert::IsTrue(plan.restoreLed);        // CNTR / dimming restored
				Assert::IsTrue(plan.restoreTopology);   // target-only undone
				Assert::IsTrue(plan.keepCrashState);    // not a clean exit
				Assert::IsTrue(plan.terminateProcess);  // no windowless process
			}
			Assert::IsFalse(ShutdownDeadline::AnyBlocking(Blockers{}));
			Assert::AreEqual(std::string("retirement-pending"),
				std::string(ShutdownDeadline::Describe(kinds[0])));
		}

		TEST_METHOD(ForcedOrUnquiescedExitKeepsTheCrashState)
		{
			Assert::IsTrue(ShutdownDeadline::CrashStateClean(false, false, false, true));
			Assert::IsFalse(ShutdownDeadline::CrashStateClean(false, false, true, true));
			Assert::IsFalse(ShutdownDeadline::CrashStateClean(false, false, false, false));
			Assert::IsFalse(ShutdownDeadline::CrashStateClean(true, false, false, true));
			Assert::IsFalse(ShutdownDeadline::CrashStateClean(false, true, false, true));
			Assert::AreNotEqual<uint32_t>(0, ShutdownDeadline::ForcedExitCode);
			// The UI thread's forced exit (LED restores, topology) fits in the
			// watchdog's grace.
			Assert::IsTrue(ShutdownDeadline::WatchdogGraceMs > ShutdownLedRestoreTimeoutMs);
			Assert::IsTrue(ShutdownBlockedRetirementDeadlineMs <
				ShutdownDeadline::ForcedExitDeadlineMs);
		}

		TEST_METHOD(ContentRuleValueIsHeldAcrossRendererStopsAndRestarts)
		{
			ContentRuleHold hold;
			Assert::AreEqual(std::string(), hold.Update(true, true, Status(true, ContentClass::Unknown)));
			Assert::AreEqual(std::string("film"), hold.Update(true, true, Status(true, ContentClass::Film)));
			// Renderer stopping / starting: not rendering keeps the value.
			Assert::AreEqual(std::string("film"), hold.Update(false, false, ContentIntentStatus()));
			// The new renderer reclassifies from Unknown: still held.
			Assert::AreEqual(std::string("film"), hold.Update(true, true, Status(true, ContentClass::Unknown)));
			// A real classification change.
			Assert::AreEqual(std::string("sports"), hold.Update(true, true, Status(true, ContentClass::Sports)));
			Assert::AreEqual(std::string("sports"), hold.Value());
		}

		TEST_METHOD(ContentRuleValueClearsWhenSwitchedOffOrNoClassifier)
		{
			ContentRuleHold hold;
			hold.Update(true, true, Status(true, ContentClass::Film));
			// intent_content off (the renderer reports disabled).
			Assert::AreEqual(std::string(), hold.Update(true, true, Status(false, ContentClass::Film)));
			hold.Update(true, true, Status(true, ContentClass::Video));
			// A rendering renderer without a classifier (status unavailable).
			Assert::AreEqual(std::string(), hold.Update(true, false, ContentIntentStatus()));
		}

		TEST_METHOD(MaximumModePrefersTheNativeModeOverDsrAndDci)
		{
			const std::vector<DisplayTopologySession::DisplayModeCandidate> modes = {
				Mode(1920, 1080, 120),
				Mode(3840, 2160, 60),
				Mode(3840, 2160, 50),
				Mode(4096, 2160, 60),   // DCI compatibility mode
				Mode(5120, 2880, 60),   // DSR
				Mode(7680, 4320, 30),   // DSR
			};
			// Native 3840x2160 (from the display): the native mode at its highest refresh.
			Assert::AreEqual(1, DisplayTopologySession::ChooseMaximumMode(modes, 3840, 2160, true));
			// A DCI-native projector: 4096x2160.
			Assert::AreEqual(3, DisplayTopologySession::ChooseMaximumMode(modes, 4096, 2160, true));
			// Native unknown: area first, as before.
			Assert::AreEqual(5, DisplayTopologySession::ChooseMaximumMode(modes, 0, 0, true));
			// capture_display_fixes off: the earlier refresh-first ranking.
			Assert::AreEqual(0, DisplayTopologySession::ChooseMaximumMode(modes, 3840, 2160, false));
		}

		TEST_METHOD(MaximumModeNativeFallbacks)
		{
			// Native not offered: the largest mode within native.
			const std::vector<DisplayTopologySession::DisplayModeCandidate> noNative = {
				Mode(1280, 720, 60), Mode(2560, 1440, 60), Mode(5120, 2880, 60) };
			Assert::AreEqual(1, DisplayTopologySession::ChooseMaximumMode(noNative, 3840, 2160, true));
			// Nothing fits the reported native size: unrestricted ranking, not a failure.
			const std::vector<DisplayTopologySession::DisplayModeCandidate> allLarger = {
				Mode(5120, 2880, 60), Mode(7680, 4320, 30) };
			Assert::AreEqual(1, DisplayTopologySession::ChooseMaximumMode(allLarger, 3840, 2160, true));
			// Nothing at all.
			const std::vector<DisplayTopologySession::DisplayModeCandidate> none;
			Assert::AreEqual(-1, DisplayTopologySession::ChooseMaximumMode(none, 3840, 2160, true));
		}

		TEST_METHOD(RendererFailureRetryCountClearsOnlyAfterSustainedRendering)
		{
			const uint64_t stable = RendererFailureRetryStableRenderingMs();
			// Derived from the schedule: twice the longest back-off.
			Assert::AreEqual<uint64_t>(2ull * RendererFailureRetryDelayMs(RendererFailureRetryMaxAttempts),
				stable);
			Assert::IsTrue(stable > RendererFailureRetryDelayMs(RendererFailureRetryMaxAttempts));
			// The first RENDERING alone does not clear it.
			Assert::IsFalse(RendererFailureRetryShouldClear(true, 1000, 1000));
			Assert::IsFalse(RendererFailureRetryShouldClear(true, 1000, 1000 + stable - 1));
			Assert::IsTrue(RendererFailureRetryShouldClear(true, 1000, 1000 + stable));
			Assert::IsFalse(RendererFailureRetryShouldClear(false, 1000, 1000 + stable));
			Assert::IsFalse(RendererFailureRetryShouldClear(true, 0, 1000 + stable));
			// A renderer that fails seconds after starting every time therefore
			// runs into the cap.
			uint32_t attempts = 0;
			while (RendererFailureRetryAllowed(attempts))
			{
				Assert::IsFalse(RendererFailureRetryShouldClear(true, 5000, 5000 + 3000));
				++attempts;
			}
			Assert::AreEqual(RendererFailureRetryMaxAttempts, attempts);
		}

		TEST_METHOD(RendererFailureRetryIsScheduledOncePerFailure)
		{
			Assert::IsTrue(RendererFailureRetryShouldSchedule(0));
			Assert::IsFalse(RendererFailureRetryShouldSchedule(123456));
		}
	};
}
