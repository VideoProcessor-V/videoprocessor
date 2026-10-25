#include "pch.h"
#include "CppUnitTest.h"

#include <vprenderer/CaptureTimestampGap.h>
#include <vprenderer/DisplayRateHold.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>
#include <vprenderer/InterpolationTimeline.h>
#include <vprenderer/NlsHookRecovery.h>
#include <vprenderer/PulldownDetector.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Bug hunt 2026-10-17: CPU tests for the timing-continuity fixes (capture
// timestamp breaks, the film clock's re-base, the timeline's display-rate
// hold), the Dolby Vision pairing rule and the NLS recovery's hook list.
// Rates are swept or derived, never a fixed list (owner [[vp-general-not-fixed]]).
namespace BugHunt20261017Test
{
	static std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

	TEST_CLASS(CaptureTimestampGapTests)
	{
	public:
		// Every rate from 10 to 240 Hz on a 10 MHz timing clock: one period is normal,
		// 2..8 whole periods are missed frames, everything else is a break.
		TEST_METHOD(TimestampBreaksAreGapsAtEveryRate)
		{
			const long long ticksPerSecond = 10000000;
			for (double hz = 10.0; hz <= 240.0; hz += 3.7)
			{
				const long long expected = static_cast<long long>(std::llround(ticksPerSecond / hz));
				const std::string at = " at " + std::to_string(hz) + " Hz";
				using CaptureTimestampGap::Kind;
				Assert::IsTrue(CaptureTimestampGap::Classify(expected, expected).kind == Kind::Normal, Wide("one period" + at).c_str());
				Assert::IsTrue(CaptureTimestampGap::Classify(expected + expected / 5, expected).kind == Kind::Normal,
					Wide("jitter" + at).c_str());
				for (long long k = 2; k <= 8; ++k)
				{
					const CaptureTimestampGap::Step step = CaptureTimestampGap::Classify(k * expected + expected / 10, expected);
					Assert::IsTrue(step.kind == Kind::Missed, Wide("whole periods" + at).c_str());
					Assert::AreEqual(k - 1, step.missed);
				}
				// A long jump (9+ periods), an odd jump, a zero or backward step: breaks.
				for (long long k : { 9LL, 10LL, 25LL, 1000LL })
					Assert::IsTrue(CaptureTimestampGap::Classify(k * expected, expected).kind == Kind::Break,
						Wide("long jump" + at).c_str());
				Assert::IsTrue(CaptureTimestampGap::Classify(2 * expected + expected / 2, expected).kind == Kind::Break,
					Wide("odd jump" + at).c_str());
				Assert::IsTrue(CaptureTimestampGap::Classify(0, expected).kind == Kind::Break, Wide("zero step" + at).c_str());
				Assert::IsTrue(CaptureTimestampGap::Classify(-expected, expected).kind == Kind::Break,
					Wide("backward step" + at).c_str());
				Assert::IsTrue(CaptureTimestampGap::Classify(expected / 3, expected).kind == Kind::Break,
					Wide("short step" + at).c_str());
			}
			// No expectation yet: never judged.
			Assert::IsTrue(CaptureTimestampGap::Classify(123456, 0).kind == CaptureTimestampGap::Kind::Normal);
		}

		// The dequeue's defence: an ordinary pair is at most 1.5 periods apart; unknown
		// times or rate are not judged.
		TEST_METHOD(DequeuePairTimesAreAdjacent)
		{
			for (double hz = 12.0; hz <= 240.0; hz *= 1.37)
			{
				const double p = 1.0 / hz, a = 1000.0;
				Assert::IsTrue(CaptureTimestampGap::AdjacentPairTimes(a, a + p, hz));
				Assert::IsTrue(CaptureTimestampGap::AdjacentPairTimes(a, a + 1.45 * p, hz));
				Assert::IsFalse(CaptureTimestampGap::AdjacentPairTimes(a, a + 1.6 * p, hz));
				Assert::IsFalse(CaptureTimestampGap::AdjacentPairTimes(a, a + 10.0 * p, hz));
				Assert::IsFalse(CaptureTimestampGap::AdjacentPairTimes(a, a - p, hz));
				Assert::IsFalse(CaptureTimestampGap::AdjacentPairTimes(a, a, hz));
				Assert::IsTrue(CaptureTimestampGap::AdjacentPairTimes(0.0, a, hz));
				Assert::IsTrue(CaptureTimestampGap::AdjacentPairTimes(a, 0.0, hz));
				Assert::IsTrue(CaptureTimestampGap::AdjacentPairTimes(a, a + 10.0 * p, 0.0));
			}
		}
	};

	TEST_CLASS(FilmClockRebaseTests)
	{
	public:
		// Mid-lock, the film rate moves (the measured signal rate replacing the nominal
		// one) by a small relative amount at a large film index: re-based, the clock
		// stays continuous (one film period per frame); without, it jumps.
		TEST_METHOD(FilmClockRebasesOnRateChange)
		{
			for (double nominal = 23.0; nominal <= 61.0; nominal += 6.25)
			{
				for (double relative : { 1e-4, -1e-4, 1e-3, -2.5e-5 })
				{
					const double measured = nominal * (1.0 + relative);
					const uint64_t start = 200000;   // hours into a stream
					Pulldown::FilmClock rebased, plain;
					rebased.SetRebaseOnRateChange(true);
					double previous = 0.0, previousPlain = 0.0, maxStep = 0.0, maxStepPlain = 0.0;
					const double t0 = 5000.0;
					for (int i = 0; i < 400; ++i)
					{
						const uint64_t n = start + static_cast<uint64_t>(i);
						const double hz = i < 200 ? nominal : measured;
						// Arrivals follow the true (measured) rate with the stream's phase.
						const double arrival = t0 + static_cast<double>(i) / measured;
						const double t = rebased.Time(n, arrival, hz);
						const double tp = plain.Time(n, arrival, hz);
						if (i > 0)
						{
							maxStep = (std::max)(maxStep, std::abs((t - previous) - 1.0 / hz));
							maxStepPlain = (std::max)(maxStepPlain, std::abs((tp - previousPlain) - 1.0 / hz));
						}
						previous = t;
						previousPlain = tp;
					}
					const std::string at = "film " + std::to_string(nominal) + " Hz, change " + std::to_string(relative);
					// Continuous within 1 ms per frame (the anchor's own adjustment).
					Assert::IsTrue(maxStep < 0.001, Wide(at + ": re-based clock stepped " + std::to_string(maxStep)).c_str());
					Assert::AreEqual(1ULL, rebased.Rebases(), Wide(at).c_str());
					// The old form's ideal jumps by n * (1/f1 - 1/f0): forward (a slower rate) the
					// clock jumps by far more than a millisecond; backward it snaps to the arrivals.
					if (relative < 0.0)
						Assert::IsTrue(maxStepPlain > 0.05, Wide(at + ": the old clock did not jump?").c_str());
				}
			}
		}

		// A constant rate: re-basing on or off gives the same times (no behaviour change
		// in a steady stream), and a change below the tolerance does not re-base.
		TEST_METHOD(FilmClockUnchangedAtAConstantRate)
		{
			for (double hz = 12.0; hz <= 60.0; hz += 4.4)
			{
				Pulldown::FilmClock rebased, plain;
				rebased.SetRebaseOnRateChange(true);
				for (int i = 0; i < 300; ++i)
				{
					const double jitter = 0.002 * std::sin(i * 1.7);
					const double arrival = 100.0 + i / hz + jitter;
					const double drift = i % 2 ? 1.0 + 1e-7 : 1.0;   // below the tolerance
					const double a = rebased.Time(1000 + i, arrival, hz * drift);
					const double b = plain.Time(1000 + i, arrival, hz * drift);
					Assert::IsTrue(std::abs(a - b) < 1e-6, Wide("constant rate " + std::to_string(hz)).c_str());
				}
				Assert::AreEqual(0ULL, rebased.Rebases());
			}
		}

		// Reset starts the base over (a lock change resets the clock).
		TEST_METHOD(FilmClockResetForgetsTheBase)
		{
			Pulldown::FilmClock clock;
			clock.SetRebaseOnRateChange(true);
			clock.Time(500, 10.0, 24.0);
			clock.Time(501, 10.0 + 1.0 / 24.0, 24.024);
			Assert::AreEqual(1ULL, clock.Rebases());
			clock.Reset();
			const double t = clock.Time(48, 50.0, 24.0);
			Assert::IsTrue(t >= 50.0 && t < 50.0 + 1e-9);
		}
	};

	TEST_CLASS(DisplayRateHoldTests)
	{
	public:
		// A cadence reset zeroes the measured rate: the hold keeps the last measurement,
		// and the timeline does not restart (the 0.1% threshold) - at any fractional rate.
		TEST_METHOD(DisplayRateHeldWhileTheMeasurementResets)
		{
			// Measured 0.2% below the nominal rate (beyond the timeline's 0.1% threshold;
			// 119.88 vs 120 sits on it), from 60 Hz up (the threshold's 0.1 Hz floor).
			for (double monitor = 60.0; monitor <= 240.0; monitor *= 1.25)
			{
				const double measured = monitor * 0.998;
				for (int fix = 0; fix < 2; ++fix)
				{
					DisplayRateHold hold;
					InterpolationTimeline::Timeline timeline;
					const double sourceHz = 24000.0 / 1001.0;
					for (int i = 0; i < 400; ++i)
					{
						// Measured for 100 frames, reset (0) for 100, measured again.
						const double m = i >= 100 && i < 200 ? 0.0 : measured;
						const double display = hold.Update(m, monitor, fix != 0);
						timeline.Plan(1, 1 + i, 10.0 + i / sourceHz, 10.0 + (i + 1) / sourceHz, sourceHz, display, false);
					}
					const std::string at = "monitor " + std::to_string(monitor) + (fix ? " with the hold" : " without");
					if (fix)
						Assert::AreEqual(0ULL, timeline.Restarts(), Wide(at).c_str());
					else
						Assert::AreEqual(2ULL, timeline.Restarts(), Wide(at + " (the old behaviour)").c_str());
				}
			}
		}

		// Nothing measured since start: the monitor's rate. A mode change (the monitor's
		// rate moving) drops the held rate; so does a held rate far from the monitor's.
		TEST_METHOD(DisplayRateFallsBackOnlyWithoutAMeasurementForThisMode)
		{
			DisplayRateHold hold;
			Assert::AreEqual(60.0, hold.Update(0.0, 60.0, true));
			Assert::AreEqual(59.94, hold.Update(59.94, 60.0, true));
			Assert::AreEqual(59.94, hold.Update(0.0, 60.0, true));
			// Mode change to 50 Hz: the 59.94 measurement is not this mode's.
			Assert::AreEqual(50.0, hold.Update(0.0, 50.0, true));
			Assert::AreEqual(49.99, hold.Update(49.99, 50.0, true));
			Assert::AreEqual(49.99, hold.Update(0.0, 50.0, true));
			hold.Reset();
			Assert::AreEqual(50.0, hold.Update(0.0, 50.0, true));
			// A held rate implausible for the monitor's rate is not used.
			DisplayRateHold distant;
			distant.Update(72.0, 0.0, true);
			Assert::AreEqual(60.0, distant.Update(0.0, 60.0, true));
			// Off: the monitor's rate whenever the measurement is 0 (the old behaviour).
			DisplayRateHold off;
			off.Update(119.88, 120.0, false);
			Assert::AreEqual(120.0, off.Update(0.0, 120.0, false));
		}
	};

	TEST_CLASS(DolbyVisionPairingTests)
	{
	public:
		TEST_METHOD(DolbyVisionNextFrameMustBeTrustedAndTheSameKind)
		{
			using DejudderPolicy::ClassifyDolbyVisionFrame;
			using DejudderPolicy::DolbyVisionFrameKind;
			using DejudderPolicy::DolbyVisionPairTrusted;
			// Formatter states: confirmed tunnel (allowSdrFallback false) or not.
			// The last argument: r210 (an SDR fallback renders); R10b / R10l show it black.
			const DolbyVisionFrameKind dv = ClassifyDolbyVisionFrame(true, false, false, true, true);
			const DolbyVisionFrameKind dvUnconfirmed = ClassifyDolbyVisionFrame(true, false, true, true, true);
			const DolbyVisionFrameKind black = ClassifyDolbyVisionFrame(false, true, false, true, true);
			const DolbyVisionFrameKind lostPacket = ClassifyDolbyVisionFrame(false, false, false, true, true);
			const DolbyVisionFrameKind sdr = ClassifyDolbyVisionFrame(false, false, true, true, true);
			const DolbyVisionFrameKind sdrUnknownPlacement = ClassifyDolbyVisionFrame(false, false, true, false, true);
			// Integration 2026-10-17: off r210 an SDR fallback is untrusted (rendered black);
			// a DV picture is a picture on every encoding.
			Assert::IsTrue(ClassifyDolbyVisionFrame(false, false, true, true, false) == DolbyVisionFrameKind::Untrusted);
			Assert::IsTrue(ClassifyDolbyVisionFrame(true, false, false, true, false) == DolbyVisionFrameKind::Picture);
			Assert::IsTrue(dv == DolbyVisionFrameKind::Picture);
			Assert::IsTrue(dvUnconfirmed == DolbyVisionFrameKind::Picture);
			Assert::IsTrue(black == DolbyVisionFrameKind::Untrusted);
			Assert::IsTrue(lostPacket == DolbyVisionFrameKind::Untrusted);
			Assert::IsTrue(sdr == DolbyVisionFrameKind::SdrFallback);
			Assert::IsTrue(sdrUnknownPlacement == DolbyVisionFrameKind::Untrusted);
			// A valid packet marked for black is untrusted.
			Assert::IsTrue(ClassifyDolbyVisionFrame(true, true, false, true, true) == DolbyVisionFrameKind::Untrusted);

			Assert::IsTrue(DolbyVisionPairTrusted(true, dv, dv));
			Assert::IsTrue(DolbyVisionPairTrusted(true, dv, dvUnconfirmed));
			Assert::IsTrue(DolbyVisionPairTrusted(true, sdr, sdr));
			Assert::IsFalse(DolbyVisionPairTrusted(true, dv, black));
			Assert::IsFalse(DolbyVisionPairTrusted(true, dv, lostPacket));
			Assert::IsFalse(DolbyVisionPairTrusted(true, dv, sdr));
			Assert::IsFalse(DolbyVisionPairTrusted(true, sdr, dv));
			Assert::IsFalse(DolbyVisionPairTrusted(true, black, black));
			// Off the tunnel the rule does not apply.
			Assert::IsTrue(DolbyVisionPairTrusted(false, dv, black));
		}
	};

	TEST_CLASS(NlsHookRecoveryListTests)
	{
	public:
		// The retry's withdrawal edits the caller's list and points the parameters at it
		// (never at storage local to the recovery: the de-judder extras read them later).
		TEST_METHOD(NlsWithdrawRepointsParametersAtTheCallerList)
		{
			pl_hook a{}, nls{}, b{};
			std::vector<const pl_hook*> hooks = { &a, &nls, &b };
			pl_render_params params{};
			params.hooks = hooks.data();
			params.num_hooks = static_cast<int>(hooks.size());
			Assert::IsTrue(NlsHookRecovery::WithdrawHook(hooks, params, &nls));
			Assert::AreEqual(size_t{ 2 }, hooks.size());
			Assert::IsTrue(params.hooks == hooks.data());
			Assert::AreEqual(2, params.num_hooks);
			Assert::IsTrue(params.hooks[0] == &a && params.hooks[1] == &b);
			Assert::IsFalse(NlsHookRecovery::WithdrawHook(hooks, params, &nls));
			std::vector<const pl_hook*> only = { &nls };
			Assert::IsTrue(NlsHookRecovery::WithdrawHook(only, params, &nls));
			Assert::IsTrue(params.hooks == nullptr);
			Assert::AreEqual(0, params.num_hooks);
		}
	};
}
