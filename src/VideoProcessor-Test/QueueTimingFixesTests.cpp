#include "pch.h"
#include "CppUnitTest.h"

// Hunt 2 group 5 (timing and motion), sub-agent A: queue_timing_fixes.
// 13-timing H1/H2 (the backlog flush and the VP timeline steering), M1/M2/L6 (the
// cadence correction's clocks, seconds and disabled phase), L1 (an overflow drop's
// gap), L2 (latency readouts), L3/L4 (frame counts where time is meant), and
// 15-intent C3 (rate bounds through one place, no copied literals).

#include <DisplayRefreshRatePolicy.h>
#include <TimingClock.h>
#include <vprenderer/AlphaCadenceCorrectionPolicy.h>
#include <vprenderer/AlphaQueuePolicy.h>
#include <vprenderer/OpenLoopDisplayRate.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <windows.h>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace QueueTimingSim
	{
		struct Result
		{
			int flushes = 0;
			int holds = 0;          // gate waits after the startup fill (a repeated refresh)
			size_t maxDepth = 0;
			double firstFlushSeconds = -1.0;
		};

		// A deterministic uniform value in [-1, 1].
		struct Lcg
		{
			uint64_t state = 0x9e3779b97f4a7c15ULL;
			double Next()
			{
				state = state * 6364136223846793005ULL + 1442695040888963407ULL;
				return static_cast<double>((state >> 11) & ((1ULL << 53) - 1)) /
					static_cast<double>(1ULL << 53) * 2.0 - 1.0;
			}
		};

		// The render loop with the VP timeline (interpolation): frames arrive from the
		// capture at the source rate on the capture's clock (ppm from the display's),
		// with arrival jitter; the timeline consumes one source frame per
		// period / trim of display time; the dequeue gate, the steering and the backlog
		// check after each render are the renderer's own (AlphaQueuePolicy).
		inline Result Run(double rateHz, double ppm, double jitterMs, bool fixes, double seconds,
			size_t target = 5)
		{
			Result result;
			const double period = 1.0 / rateHz;
			const double arrivalPeriod = period * (1.0 + ppm * 1e-6);
			Lcg random;
			uint64_t nextIndex = 0;
			auto arrivalTime = [&](uint64_t index) {
				return 0.010 + static_cast<double>(index) * arrivalPeriod;
			};
			std::vector<double> jitter;   // per frame, drawn in order
			auto jitterFor = [&](uint64_t index) {
				while (jitter.size() <= index)
					jitter.push_back(random.Next() * jitterMs * 1e-3);
				return jitter[static_cast<size_t>(index)];
			};
			std::deque<double> queue;   // enqueue times
			auto pushUntil = [&](double t) {
				while (true)
				{
					// Jitter never reorders frames: arrivals stay monotonic.
					const double at = arrivalTime(nextIndex) + jitterFor(nextIndex);
					if (at > t)
						break;
					queue.push_back(at);
					++nextIndex;
				}
			};
			auto nextArrival = [&]() { return arrivalTime(nextIndex) + jitterFor(nextIndex); };
			AlphaQueuePolicy::TimelineQueueSteering steering;
			bool startup = true;
			double now = 0.0;
			while (now < seconds)
			{
				pushUntil(now);
				if (!AlphaQueuePolicy::CanDequeue(queue.size(), target, startup))
				{
					if (!startup)
						++result.holds;
					now = (std::max)(now, nextArrival());
					continue;
				}
				startup = false;
				queue.pop_front();
				steering.Update(queue.size(), target, rateHz, fixes);
				const double cycle = period / steering.RateTrim();
				now += cycle;
				pushUntil(now);
				result.maxDepth = (std::max)(result.maxDepth, queue.size());
				const double oldestAgeMs = queue.empty() ? 0.0 : (now - queue.front()) * 1000.0;
				if (AlphaQueuePolicy::ShouldRecoverBacklog(queue.size(), target, oldestAgeMs,
					cycle * 1000.0, rateHz, fixes))
				{
					while (queue.size() > target)
						queue.pop_front();
					++result.flushes;
					if (result.firstFlushSeconds < 0.0)
						result.firstFlushSeconds = now;
					steering.Reset();   // vpQueueErrorReset
				}
			}
			return result;
		}
	}

	// The .cpp/.h files under a folder, recursively (Win32: this project is C++14).
	static void CollectSources(const std::string& folder, std::vector<std::string>& files)
	{
		WIN32_FIND_DATAA data{};
		const HANDLE find = FindFirstFileA((folder + "\\*").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return;
		do
		{
			const std::string name = data.cFileName;
			if (name == "." || name == "..")
				continue;
			const std::string path = folder + "\\" + name;
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				CollectSources(path, files);
			else if (name.size() > 4 && (name.substr(name.size() - 4) == ".cpp" || name.substr(name.size() - 2) == ".h"))
				files.push_back(path);
			else if (name.size() > 2 && name.substr(name.size() - 2) == ".h")
				files.push_back(path);
		} while (FindNextFileA(find, &data));
		FindClose(find);
	}

	TEST_CLASS(QueueTimingFixesTests)
	{
	public:
		// H1 + H2: the steering loop and the flush policy together. With the fixes no
		// periodic flush at any rate (film, PAL, NTSC, sports, high rate), with a
		// capture/display ppm difference either way and arrival jitter. Without them
		// the old cycle shows at 24-50 Hz: the steering builds the margin frame and the
		// flush throws it away about every 80 s.
		TEST_METHOD(TimelineSteeringNeverReachesTheFlushWithTheFixes)
		{
			const double rates[] = { 23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 59.94, 60.0, 120.0 };
			const double ppms[] = { 0.0, 100.0, -100.0 };
			for (double rate : rates)
			{
				for (double ppm : ppms)
				{
					const QueueTimingSim::Result fixed = QueueTimingSim::Run(rate, ppm, 2.0, true, 900.0);
					const std::wstring what = L"rate " + std::to_wstring(rate) + L" ppm " + std::to_wstring(ppm);
					Assert::AreEqual(0, fixed.flushes, (L"no backlog flush with the fixes: " + what).c_str());
					Logger::WriteMessage((L"fixed " + what + L": holds " + std::to_wstring(fixed.holds) +
						L" max depth " + std::to_wstring(fixed.maxDepth)).c_str());
					if (ppm == 0.0)
						Assert::AreEqual(0, fixed.holds, (L"no gate hold after the fill: " + what).c_str());
					Assert::IsTrue(fixed.maxDepth <= 5 + 2, (L"the queue stays within target+2: " + what).c_str());
				}
			}
		}

		TEST_METHOD(TheOldRulesFlushPeriodicallyAtFilmAndPalRates)
		{
			// The reported bug (documents the off switch, the old behaviour).
			for (double rate : { 24.0, 25.0, 30.0, 50.0 })
			{
				const QueueTimingSim::Result old = QueueTimingSim::Run(rate, 0.0, 2.0, false, 900.0);
				const std::wstring what = L"rate " + std::to_wstring(rate);
				Assert::IsTrue(old.flushes >= 3, (L"the old rule flushes repeatedly: " + what).c_str());
				Assert::IsTrue(old.firstFlushSeconds > 10.0 && old.firstFlushSeconds < 400.0,
					(L"after the steering rebuilt the margin: " + what).c_str());
			}
		}

		// H1 on its own: target+1 and target+2 are never stale (the target's own wait
		// is not counted); a real backlog still is; a render stall still is.
		TEST_METHOD(StaleAgeIsMeasuredBeyondTheTarget)
		{
			const size_t target = 5;
			for (double rate : { 24.0, 25.0, 30.0, 50.0, 60.0, 120.0 })
			{
				const double periodMs = 1000.0 / rate;
				for (size_t extra = 1; extra <= 2; ++extra)
				{
					// The front frame waited (depth-1) periods plus almost a period.
					const size_t depth = target + extra;
					const double ageMs = (static_cast<double>(depth - 1) + 0.99) * periodMs;
					Assert::IsFalse(AlphaQueuePolicy::ShouldRecoverBacklog(depth, target, ageMs, periodMs, rate, true),
						(L"target+" + std::to_wstring(extra) + L" at " + std::to_wstring(rate)).c_str());
				}
				const double backlogAgeMs = target * periodMs +
					AlphaQueuePolicy::StaleBacklogAgeThresholdMs(rate) + 1.0;
				Assert::IsTrue(AlphaQueuePolicy::ShouldRecoverBacklog(target + 8, target, backlogAgeMs, periodMs, rate, true),
					(L"a real backlog at " + std::to_wstring(rate)).c_str());
				Assert::IsTrue(AlphaQueuePolicy::ShouldRecoverBacklog(target + 1, target, 0.0,
					AlphaQueuePolicy::RenderStallThresholdMs(rate), rate, true),
					(L"a render stall at " + std::to_wstring(rate)).c_str());
			}
			// The old rule at 24 Hz: target+1 was already stale.
			Assert::IsTrue(AlphaQueuePolicy::ShouldRecoverBacklog(6, 5, 5.5 * 1000.0 / 24.0, 41.7, 24.0, false));
			Assert::IsFalse(AlphaQueuePolicy::ShouldRecoverBacklog(5, 5, 1000.0, 41.7, 24.0, true), L"at the target: never");
		}

		// H1 at 3:2 pulldown: a 60 Hz signal, 24 Hz film frames in the queue. With the
		// queue's own (film) rate target+1 and target+2 are not stale; judged with the
		// signal's period they would be (the renderer passes the front frame's film rate).
		TEST_METHOD(PulldownQueueIsJudgedAtTheFilmRate)
		{
			const size_t target = 5;
			const double filmPeriodMs = 1000.0 / 24.0;
			for (size_t extra = 1; extra <= 2; ++extra)
			{
				const size_t depth = target + extra;
				const double ageMs = (static_cast<double>(depth - 1) + 0.99) * filmPeriodMs;
				Assert::IsFalse(AlphaQueuePolicy::ShouldRecoverBacklog(depth, target, ageMs, 1000.0 / 60.0, 24.0, true),
					L"film rate: no flush");
				Assert::IsTrue(AlphaQueuePolicy::ShouldRecoverBacklog(depth, target, ageMs, 1000.0 / 60.0, 60.0, true),
					L"the signal's period would flush the film queue");
			}
			const QueueTimingSim::Result film = QueueTimingSim::Run(24.0, 50.0, 3.0, true, 900.0);
			Assert::AreEqual(0, film.flushes, L"the steering loop at the film rate: no flush");
		}

		// L3: the steering's smoothing is the same time at every source rate.
		TEST_METHOD(SteeringSmoothingIsATimeConstant)
		{
			for (double rate : { 24.0, 60.0, 120.0 })
			{
				const double w = AlphaQueuePolicy::TimelineQueueSteering::Weight(rate, true);
				const double framesInTau = -1.0 / std::log(1.0 - w);
				Assert::AreEqual(AlphaQueuePolicy::TimelineQueueSteering::kSmoothingSeconds, framesInTau / rate, 1e-9);
				Assert::AreEqual(0.05, AlphaQueuePolicy::TimelineQueueSteering::Weight(rate, false));
			}
			AlphaQueuePolicy::TimelineQueueSteering steering;
			for (int i = 0; i < 1000; ++i)
				steering.Update(3, 5, 25.0, true);   // two below the target
			Assert::AreEqual(-2.0, steering.Error(), 1e-6);
			Assert::IsTrue(steering.RateTrim() < 1.0, L"below the target: longer refreshes per frame, the queue fills");
			for (int i = 0; i < 1000; ++i)
				steering.Update(30, 5, 25.0, true);
			Assert::AreEqual(1.003, steering.RateTrim(), 1e-12, L"the trim is bounded at 0.3%");
		}

		// L1: an overflow drop merges the dropped frame's gap into its successor's.
		TEST_METHOD(OverflowDropKeepsTheDroppedFramesGap)
		{
			using AlphaQueuePolicy::GapState;
			using AlphaQueuePolicy::MergeDroppedGap;
			// The dropped frame carried a no-fill gap (a timestamp break): it survives.
			GapState merged = MergeDroppedGap(GapState{ true, 0 }, GapState{ false, 0 }, true);
			Assert::IsTrue(merged.gapBefore);
			Assert::AreEqual(0u, merged.gapFrames, L"no gap-fill pair across a break");
			// The old rule turned it into a fillable one-frame gap.
			Assert::AreEqual(1u, MergeDroppedGap(GapState{ true, 0 }, GapState{ false, 0 }, false).gapFrames);
			// Missed frames add up: 2 before the dropped frame + itself + 1 before the next.
			merged = MergeDroppedGap(GapState{ true, 2 }, GapState{ true, 1 }, true);
			Assert::AreEqual(4u, merged.gapFrames);
			Assert::AreEqual(2u, MergeDroppedGap(GapState{ true, 2 }, GapState{ true, 1 }, false).gapFrames);
			// A clean drop is a one-frame gap; a no-fill successor stays no-fill.
			Assert::AreEqual(1u, MergeDroppedGap(GapState{}, GapState{}, true).gapFrames);
			Assert::AreEqual(0u, MergeDroppedGap(GapState{}, GapState{ true, 0 }, true).gapFrames);
			Assert::AreEqual(0u, MergeDroppedGap(GapState{}, GapState{ true, 0 }, false).gapFrames);
		}

		static AlphaCadenceCorrectionInput CadenceInput(double captureRateHz, double displayRateHz, bool fixes)
		{
			AlphaCadenceCorrectionInput input;
			input.enabled = true;
			input.generation = 1;
			input.presentationEvidence = AlphaPresentationEvidence::Stable;
			input.captureRateHz = captureRateHz;
			input.displayRateHz = displayRateHz;
			input.desiredQueueDepth = 4;
			input.queueDepth = 4;          // a drop is held back (not above the target)
			input.presentationDebt = 1;
			input.timingFixes = fixes;
			return input;
		}

		// M1: the card's clock reads 30 ppm fast against QPC; the frames really arrive
		// at the display's rate on QPC. The QPC-measured capture rate sees no mismatch;
		// the old comparison of the two clocks integrates a phantom phase.
		TEST_METHOD(CadenceCorrectionComparesRatesOnOneClock)
		{
			for (double rate : { 24.0, 50.0, 60.0 })
			{
				const double cardRate = rate * (1.0 + 30e-6);
				for (bool fixes : { true, false })
				{
					AlphaCadenceCorrectionPolicy policy;
					AlphaCadenceCorrectionDecision decision;
					const uint64_t frames = static_cast<uint64_t>(rate * 3600.0);
					for (uint64_t k = 0; k < frames; ++k)
					{
						AlphaCadenceCorrectionInput input = CadenceInput(cardRate, rate, fixes);
						input.arrivalSeconds = 100.0 + static_cast<double>(k) / rate +
							(static_cast<int>((k * 7919) % 13) - 6) * 0.0003;   // +-1.8 ms arrival jitter
						input.sourceSequence = k;
						decision = policy.Evaluate(input);
					}
					const std::wstring what = L"rate " + std::to_wstring(rate) + (fixes ? L" fixes" : L" old");
					if (fixes)
					{
						Assert::IsTrue(std::abs(decision.diagnostic.filteredMismatchPpm) < 3.0,
							(L"no phantom mismatch: " + what + L" " + std::to_wstring(decision.diagnostic.filteredMismatchPpm)).c_str());
						Assert::IsTrue(std::abs(decision.phaseFrames) < 0.3,
							(L"no phantom phase: " + what + L" " + std::to_wstring(decision.phaseFrames)).c_str());
					}
					else
					{
						Assert::IsTrue(decision.phaseFrames > 1.5,
							(L"the old clocks build a phantom phase: " + what + L" " + std::to_wstring(decision.phaseFrames)).c_str());
					}
				}
			}
		}

		// M1: a real mismatch whose action the queue holds back cannot grow the
		// phase without limit.
		TEST_METHOD(CadencePhaseIsBounded)
		{
			AlphaCadenceCorrectionPolicy policy;
			AlphaCadenceCorrectionDecision decision;
			const double rate = 24.0, real = 24.0 * (1.0 + 150e-6);
			for (uint64_t k = 0; k < static_cast<uint64_t>(24.0 * 7200.0); ++k)
			{
				AlphaCadenceCorrectionInput input = CadenceInput(real, rate, true);
				input.arrivalSeconds = 50.0 + static_cast<double>(k) / real;
				decision = policy.Evaluate(input);
			}
			Assert::IsTrue(decision.phaseFrames >= 1.0, L"the real mismatch is seen");
			Assert::IsTrue(decision.phaseFrames <= 2.0 + 1e-9, L"and bounded");
		}

		// M1: without arrival times, a nominal fallback capture rate does not feed
		// the rate filter.
		TEST_METHOD(NominalFallbackRateDoesNotFeedTheFilter)
		{
			AlphaCadenceCorrectionPolicy policy;
			AlphaCadenceCorrectionInput input = CadenceInput(24.0, 24.0, true);
			input.captureRateMeasured = false;
			AlphaCadenceCorrectionDecision decision = policy.Evaluate(input);
			Assert::AreEqual(0u, decision.rateFilterSamples);
			Assert::IsTrue(decision.timingStatus == AlphaCadenceTimingStatus::Measuring);
			input.captureRateMeasured = true;
			decision = policy.Evaluate(input);
			Assert::AreEqual(1u, decision.rateFilterSamples);
			input.captureRateMeasured = false;
			input.captureRateHz = 24.024;   // 1000 ppm off: the nominal 24 for a 23.976 source, inverted
			decision = policy.Evaluate(input);
			Assert::AreEqual(1u, decision.rateFilterSamples, L"the fallback sample is skipped");
		}

		// M2: the warm-ups, the cooldown and the filter are seconds at every rate.
		TEST_METHOD(CadenceTimingsAreSeconds)
		{
			using P = AlphaCadenceCorrectionPolicy;
			Assert::AreEqual(240u, P::FramesFor(10.0, 24.0, P::LegacyPredictionSamples(), true));
			Assert::AreEqual(1200u, P::FramesFor(10.0, 120.0, P::LegacyPredictionSamples(), true));
			Assert::AreEqual(P::LegacyPredictionSamples(), P::FramesFor(10.0, 24.0, P::LegacyPredictionSamples(), false));
			Assert::AreEqual(P::LegacyCooldownFrames(), P::FramesFor(2.0, 0.0, P::LegacyCooldownFrames(), true),
				L"no valid rate: the legacy count");
			for (double rate : { 24.0, 60.0, 120.0 })
			{
				const double tauFrames = -1.0 / std::log(1.0 - P::FilterAlpha(rate, true));
				Assert::AreEqual(10.0, tauFrames / rate, 1e-9);
			}
			Assert::AreEqual(1.0 / 600.0, P::FilterAlpha(24.0, false));
			// The stable warm-up at 24 Hz: 2 s = 48 frames, not 120.
			AlphaCadenceCorrectionPolicy policy;
			AlphaCadenceCorrectionInput input = CadenceInput(24.0, 24.0, true);
			AlphaCadenceCorrectionDecision decision;
			for (int k = 0; k < 49; ++k)
			{
				input.arrivalSeconds = 0.0;   // the card's rate (measured)
				decision = policy.Evaluate(input);
			}
			Assert::IsTrue(decision.blockReason != AlphaCadenceBlockReason::StabilizingRates,
				L"stable after 2 s of frames at 24 Hz");
		}

		// L6: a disabled policy forgets its phase.
		TEST_METHOD(DisabledCadencePolicyForgetsItsPhase)
		{
			for (bool fixes : { true, false })
			{
				AlphaCadenceCorrectionPolicy policy;
				AlphaCadenceCorrectionDecision decision;
				for (int k = 0; k < 24 * 120; ++k)
					decision = policy.Evaluate(CadenceInput(24.0 * (1.0 + 150e-6), 24.0, fixes));
				Assert::IsTrue(decision.phaseFrames > 0.2, L"a phase built up");
				AlphaCadenceCorrectionInput off = CadenceInput(24.0, 24.0, fixes);
				off.enabled = false;
				decision = policy.Evaluate(off);
				if (fixes)
					Assert::AreEqual(0.0, decision.phaseFrames, L"forgotten while disabled");
				else
					Assert::IsTrue(decision.phaseFrames > 0.2, L"the old policy kept it");
			}
		}

		// L2: latency readouts keep their fraction; a clock without a rate gives 0.
		TEST_METHOD(TimingClockDiffKeepsTheFraction)
		{
			Assert::AreEqual(8.333, TimingClockDiffMs(0, 8333, 1000000), 1e-9);
			Assert::AreEqual(-8.333, TimingClockDiffMs(8333, 0, 1000000), 1e-9);
			Assert::AreEqual(0.0, TimingClockDiffMs(0, 1000, 0));
			Assert::AreEqual(0.0, TimingClockDiffMs(0, 1000, -5));
			// Large spans (days at 10 MHz) stay exact to the tick.
			const timingclocktime_t day = 10000000LL * 86400LL * 30LL;
			Assert::AreEqual(30.0 * 86400.0 * 1000.0 + 0.0001, TimingClockDiffMs(0, day + 1, 10000000), 1e-6);
		}

		// L3/L4: frame counts where time is meant.
		TEST_METHOD(FailureLimitAndPoolFollowTimeAndTarget)
		{
			Assert::AreEqual(120u, AlphaQueuePolicy::ConsecutiveFailureLimit(24.0, true));
			Assert::AreEqual(600u, AlphaQueuePolicy::ConsecutiveFailureLimit(120.0, true));
			Assert::AreEqual(300u, AlphaQueuePolicy::ConsecutiveFailureLimit(24.0, false));
			Assert::AreEqual(300u, AlphaQueuePolicy::ConsecutiveFailureLimit(0.0, true));
			Assert::AreEqual<size_t>(24, AlphaQueuePolicy::FramesForSeconds(1.0, 24.0, 24, true));
			Assert::AreEqual<size_t>(120, AlphaQueuePolicy::FramesForSeconds(1.0, 120.0, 24, true));
			Assert::AreEqual<size_t>(24, AlphaQueuePolicy::FramesForSeconds(1.0, 120.0, 24, false));
			Assert::AreEqual(11, AlphaQueuePolicy::EarlyUploadSlots(5), L"the default target: the old 8 + 3");
			Assert::AreEqual(22, AlphaQueuePolicy::EarlyUploadSlots(16), L"target_frames 16: its frames in flight");
			Assert::AreEqual(11, AlphaQueuePolicy::EarlyUploadSlots(1));
		}

		// 05-renderahead M5: the whole-hertz mode ranks below the vblank cadence.
		TEST_METHOD(WholeHertzModeRanksBelowTheVBlankCadence)
		{
			using S = OpenLoopDisplayRate::Source;
			OpenLoopDisplayRate::Candidates c;
			c.modeIntegerHz = 59.0;
			c.cadenceHz = 59.94;
			Assert::IsTrue(OpenLoopDisplayRate::Select(c, true).source == S::VBlankCadence);
			Assert::AreEqual(59.94, OpenLoopDisplayRate::Select(c, true).hz);
			Assert::IsTrue(OpenLoopDisplayRate::Select(c, true, false).source == S::ModeInteger, L"the old order");
			c.cadenceHz = 0.0;
			Assert::IsTrue(OpenLoopDisplayRate::Select(c, true).source == S::ModeInteger, L"only when nothing better exists");
			c.hostHz = 59.94;
			c.cadenceHz = 59.9;
			Assert::IsTrue(OpenLoopDisplayRate::Select(c, true).source == S::Host, L"the application's rate stays above");
		}

		// 15-intent C3: one display bound, one capture bound.
		TEST_METHOD(RateBoundsAreTheSharedOnes)
		{
			Assert::IsTrue(DisplayRefreshBounds::Accepts(300.0), L"a 300 Hz panel is a display rate");
			Assert::IsTrue(DisplayRefreshBounds::Accepts(500.0));
			Assert::IsFalse(DisplayRefreshBounds::Accepts(500.01));
			Assert::IsFalse(CaptureRateBounds::Accepts(300.0), L"not a capture rate");
			Assert::IsTrue(CaptureRateBounds::Accepts(240.0));
			Assert::IsFalse(CaptureRateBounds::Accepts(9.99));
			Assert::IsFalse(CaptureRateBounds::Accepts(std::nan("")));
			Assert::AreEqual(240.0, AlphaQueuePolicy::ValidFrameRateOrDefault(240.0));
			Assert::AreEqual(AlphaQueuePolicy::DEFAULT_FRAME_RATE_HZ, AlphaQueuePolicy::ValidFrameRateOrDefault(300.0));
		}

		// 15-intent C3: no copied rate-bound literal in the product sources. A
		// comparison of a rate/refresh/Hz value with 10, 240, 480 or 500, or a
		// min/max rate constant with one of those values, must use
		// DisplayRefreshBounds or CaptureRateBounds (DisplayRefreshRatePolicy.h).
		TEST_METHOD(NoCopiedRateBoundLiterals)
		{
			std::string here = __FILE__;
			std::replace(here.begin(), here.end(), '/', '\\');
			const std::string src = here.substr(0, here.rfind("\\VideoProcessor-Test\\"));
			const std::regex comparison(R"(([<>]=?|==)\s*(10|240|480|500)(\.0+)?\b(?!\s*[*/]))");
			const std::regex rateWord(R"(hz|refresh|fps|framerate|frame_rate|rate\b)");
			const std::regex constant(R"(constexpr\s+double\s+\w*(min|max)\w*(rate|refresh|hz)\w*\s*=\s*(10|240|480|500)(\.0+)?\s*;)");
			const std::regex returned(R"(refresh\w*hz\s*\(\)\s*(const\s*)?\{\s*return\s+(10|240|480|500)(\.0+)?\s*;)");
			std::vector<std::string> offenders;
			std::vector<std::string> files;
			for (const char* folder : { "VideoProcessor-Lib", "VideoProcessor-GUI", "VideoProcessor-Config", "VideoProcessor-VPRenderer" })
				CollectSources(src + "\\" + folder, files);
			Assert::IsTrue(files.size() > 50, L"the product sources were found");
			for (const std::string& path : files)
			{
				const std::string name = path.substr(path.rfind('\\') + 1);
				if (name == "DisplayRefreshRatePolicy.h")
					continue;   // the bounds themselves
				std::ifstream file(path, std::ios::binary);
				std::string line;
				int number = 0;
				while (std::getline(file, line))
				{
					++number;
					std::string lower = line;
					for (char& ch : lower)
						ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
					const size_t comment = lower.find("//");
					if (comment != std::string::npos)
						lower.resize(comment);
					const bool offends =
						(std::regex_search(lower, comparison) && std::regex_search(lower, rateWord)) ||
						std::regex_search(lower, constant) || std::regex_search(lower, returned);
					if (offends)
						offenders.push_back(name + ":" + std::to_string(number));
				}
			}
			std::string list;
			for (const std::string& offender : offenders)
				list += offender + " ";
			Assert::IsTrue(offenders.empty(), std::wstring(list.begin(), list.end()).c_str());
		}
	};
}
