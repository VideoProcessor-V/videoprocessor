#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/motion/consumers/InterpolationPolicy.h>
#include <VP-V/modules/motion/consumers/InterpolationTimeline.h>
#include <VP-V/modules/repair/deband/VpDebander.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// docs/VP-interpolator-50-60-plan.md step 2: when VP's optical-flow interpolator
// runs. Ratio sweeps, not lists of rates (owner [[vp-general-not-fixed]]).
namespace InterpolationPolicyTest
{
	using InterpolationPolicy::Decide;
	using InterpolationPolicy::Input;
	using InterpolationPolicy::Mode;
	using InterpolationPolicy::Reason;
	using InterpolationPolicy::SourceClass;

	static Input VideoInput(double sourceHz, double displayHz, Mode mode = Mode::Auto)
	{
		Input in;
		in.mode = mode;
		in.sourceHz = sourceHz;
		in.displayHz = displayHz;
		in.haveNext = true;
		return in;
	}

	static std::wstring Describe(const char* what, double sourceHz, double displayHz, Reason reason)
	{
		const std::string s = std::string(what) + " source " + std::to_string(sourceHz) + " display " +
			std::to_string(displayHz) + ": " + InterpolationPolicy::Name(reason);
		return std::wstring(s.begin(), s.end());
	}

	TEST_CLASS(InterpolationPolicyTests)
	{
	public:
		TEST_METHOD(OffByDefaultAndForEveryRatio)
		{
			Input defaults;
			Assert::IsTrue(defaults.mode == Mode::Off, L"interpolation off by default");
			Assert::IsTrue(defaults.sourceClass == SourceClass::Legacy, L"source_class legacy by default");
			for (double display = 23.0; display <= 241.0; display += 7.3)
				for (double source = 10.0; source <= 241.0; source += 3.7)
				{
					Input in = VideoInput(source, display, Mode::Off);
					Assert::IsTrue(Decide(in).reason == Reason::Off);
				}
		}

		TEST_METHOD(ParsingAndNamesRoundTrip)
		{
			for (const char* v : { "off", "auto", "always" })
				Assert::AreEqual(std::string(v), std::string(InterpolationPolicy::Name(InterpolationPolicy::ParseMode(v))));
			Assert::IsTrue(InterpolationPolicy::ParseMode("bogus") == Mode::Off);
			Assert::IsTrue(InterpolationPolicy::ParseFallback("blend") == InterpolationPolicy::Fallback::Blend);
			Assert::IsTrue(InterpolationPolicy::ParseFallback("") == InterpolationPolicy::Fallback::Hold);
			Assert::IsTrue(InterpolationPolicy::ParseSourceClass("measured") == SourceClass::Measured);
			Assert::IsTrue(InterpolationPolicy::ParseSourceClass("x") == SourceClass::Legacy);
		}

		// Video-class sources (legacy class: above the film-like rates) on any
		// display: a non-integer ratio above 1 interpolates; whole ratios and
		// near-1 ratios pass through; below 1 needs Stage B.
		TEST_METHOD(AutoRatioSweepForVideoSources)
		{
			int interpolated = 0, passed = 0, below = 0;
			for (double display = 24.0; display <= 240.0; display += 0.5)
				for (double source = 49.0; source <= 240.0; source += 1.25)
				{
					if (DejudderPolicy::IsFilmLike(source))
						continue;
					const auto d = Decide(VideoInput(source, display));
					const double ratio = display / source;
					const double r = ratio < 1.0 ? 1.0 / ratio : ratio;
					const double n = std::round(r);
					const bool whole = std::abs(r - n) <= InterpolationPolicy::kRatioTolerance * n;
					Reason expected;
					if (whole)
						expected = Reason::IntegerRatio;
					else if (ratio < 1.0)
						expected = Reason::RatioBelowOne;
					else
						expected = Reason::Interpolate;
					Assert::IsTrue(d.reason == expected, Describe("auto", source, display, d.reason).c_str());
					Assert::AreEqual(ratio, d.ratio, 1e-12);
					interpolated += expected == Reason::Interpolate;
					passed += expected == Reason::IntegerRatio;
					below += expected == Reason::RatioBelowOne;
				}
			Assert::IsTrue(interpolated > 100 && passed > 10 && below > 100, L"the sweep covers every branch");
		}

		TEST_METHOD(TheRatiosThePlanNames)
		{
			// 1.2 (50p on 60 Hz): interpolate.
			Assert::IsTrue(Decide(VideoInput(50.0, 60.0)).reason == Reason::Interpolate);
			// 0.833 (60p on 50 Hz): per-vsync synthesis, not built; with it, interpolate.
			Assert::IsTrue(Decide(VideoInput(60.0, 50.0)).reason == Reason::RatioBelowOne);
			Input stageB = VideoInput(60.0, 50.0);
			stageB.perVsyncAvailable = true;
			const auto b = Decide(stageB);
			Assert::IsTrue(b.reason == Reason::Interpolate && b.perVsync);
			// 1.001 (59.94 on 60), and its inverse: left to drift correction.
			Assert::IsTrue(Decide(VideoInput(60000.0 / 1001.0, 60.0)).reason == Reason::IntegerRatio);
			Assert::IsTrue(Decide(VideoInput(60.0, 60000.0 / 1001.0)).reason == Reason::IntegerRatio);
			Assert::IsTrue(Decide(VideoInput(50.0, 50.0)).reason == Reason::IntegerRatio, L"50/60 sports untouched");
			Assert::IsTrue(Decide(VideoInput(60.0, 60.0)).reason == Reason::IntegerRatio);
			Assert::IsTrue(Decide(VideoInput(120.0, 60.0)).reason == Reason::IntegerRatio, L"a whole decimation");
			Assert::IsTrue(Decide(VideoInput(50.0, 100.0)).reason == Reason::IntegerRatio);
			// A real mismatch just outside the tolerance: interpolated (48 on 50 = 4%).
			Input hfr = VideoInput(49.0, 50.0);
			Assert::IsTrue(Decide(hfr).reason == Reason::Interpolate);
		}

		TEST_METHOD(AlwaysAddsWholeRatiosForVideoOnly)
		{
			Assert::IsTrue(Decide(VideoInput(50.0, 100.0, Mode::Always)).reason == Reason::Interpolate);
			Assert::IsTrue(Decide(VideoInput(50.0, 60.0, Mode::Always)).reason == Reason::Interpolate);
			// 1:1 and near-1 still pass through (nothing to fill; drift correction).
			Assert::IsTrue(Decide(VideoInput(60.0, 60.0, Mode::Always)).reason == Reason::IntegerRatio);
			Assert::IsTrue(Decide(VideoInput(59.94, 60.0, Mode::Always)).reason == Reason::IntegerRatio);
			// Film at a whole ratio keeps its even cadence (no soap opera).
			Input film = VideoInput(24.0, 48.0, Mode::Always);
			Assert::IsTrue(Decide(film).reason == Reason::FilmIntegerRatio);
		}

		TEST_METHOD(FilmStaysWithTheDejudder)
		{
			// Every film-like legacy rate the de-judder has an uneven strength for.
			for (double source = 12.0; source <= 30.5; source += 0.37)
			{
				Input in = VideoInput(source, 60.0);
				in.dejudderStrength = 0.6;
				const auto d = Decide(in);
				const double ratio = 60.0 / source;
				const bool uneven = DejudderPolicy::IsUnevenCadence(ratio);
				if (uneven)
					Assert::IsTrue(d.reason == Reason::FilmToDejudder, Describe("film", source, 60.0, d.reason).c_str());
				else
					Assert::IsTrue(d.reason == Reason::FilmIntegerRatio, Describe("film even", source, 60.0, d.reason).c_str());
			}
			// Without a de-judder strength, a film-like source at a non-integer ratio
			// may be interpolated in auto (25p inside a 50 Hz stream on 60 Hz).
			Input free = VideoInput(25.0, 60.0);
			free.pulldownLocked = true;
			Assert::IsTrue(Decide(free).reason == Reason::Interpolate);
			// ...never at a whole ratio.
			Assert::IsTrue(Decide(VideoInput(30.0, 60.0)).reason == Reason::FilmIntegerRatio);
		}

		TEST_METHOD(MeasuredClassUsesNoFixedRates)
		{
			// measured: film-like when the content owns at least two refreshes, or
			// came from a pulldown lock; a sweep over display rates shows the
			// boundary moves with the display (no fixed rate).
			for (double display = 48.0; display <= 240.0; display += 12.0)
			{
				const double justFilm = display / 2.0;
				const double justVideo = display / 1.9;
				// cadence_class_fixes off: the display-ratio rule (Hunt2CadenceClassPolicyTests: on).
				Assert::IsTrue(InterpolationPolicy::IsFilmLike(SourceClass::Measured, justFilm, display, false, false));
				Assert::IsFalse(InterpolationPolicy::IsFilmLike(SourceClass::Measured, justVideo, display, false, false));
				Assert::IsTrue(InterpolationPolicy::IsFilmLike(SourceClass::Measured, justVideo, display, true, false));
			}
			// 48p on 60 Hz: legacy film (HFR), measured video -> interpolated in auto.
			Input hfr = VideoInput(48.0, 60.0);
			hfr.dejudderStrength = 0.6;
			Assert::IsTrue(Decide(hfr).reason == Reason::FilmToDejudder);
			hfr.sourceClass = SourceClass::Measured;
			hfr.classFixes = false;
			Assert::IsTrue(Decide(hfr).reason == Reason::Interpolate);
			// With the fixes (default): 48p is the owner's HFR film on any display.
			hfr.classFixes = true;
			Assert::IsTrue(Decide(hfr).reason == Reason::FilmToDejudder);
		}

		TEST_METHOD(ExclusionsAndHolds)
		{
			auto with = [](void (*set)(Input&)) { Input in = VideoInput(50.0, 60.0); set(in); return Decide(in).reason; };
			Assert::IsTrue(with([](Input& i) { i.smoothMotion = true; }) == Reason::SmoothMotion, L"exclusive with smooth motion");
			Assert::IsTrue(with([](Input& i) { i.lowLatency = true; }) == Reason::LowLatency, L"low latency: never");
			Assert::IsTrue(with([](Input& i) { i.interlaced = true; }) == Reason::Interlaced);
			Assert::IsTrue(with([](Input& i) { i.nativeRgb = true; }) == Reason::NativeRgb, L"RGB holds");
			Assert::IsTrue(with([](Input& i) { i.subtitleComposed = true; }) == Reason::SubtitleComposed);
			Assert::IsTrue(with([](Input& i) { i.haveNext = false; }) == Reason::NoNextFrame, L"zero queue = A");
			Assert::IsTrue(with([](Input& i) { i.sourceHz = 0.0; }) == Reason::UnknownRates);
			Assert::IsTrue(with([](Input& i) { i.displayHz = 0.0; }) == Reason::UnknownRates);
			// Smooth motion outranks low latency (the engine choice is reported first).
			Assert::IsTrue(with([](Input& i) { i.smoothMotion = true; i.lowLatency = true; }) == Reason::SmoothMotion);
		}

		TEST_METHOD(PhaseTimelineAtAnyRatio)
		{
			Assert::AreEqual(0.0, InterpolationPolicy::Phase(1.0, 1.0, 0.02));
			Assert::AreEqual(1.0, InterpolationPolicy::Phase(1.02, 1.0, 0.02), 1e-12);
			Assert::AreEqual(0.5, InterpolationPolicy::Phase(1.01, 1.0, 0.02), 1e-9);
			Assert::AreEqual(0.0, InterpolationPolicy::Phase(0.5, 1.0, 0.02), L"clamped before A");
			Assert::AreEqual(1.0, InterpolationPolicy::Phase(9.0, 1.0, 0.02), L"clamped after B");
			Assert::AreEqual(0.0, InterpolationPolicy::Phase(1.0, 1.0, 0.0), L"no period");
			// Over many source frames at any ratio above 1, the filled refreshes per
			// frame average ratio - 1 and every phase lies in (0, 1).
			for (double ratio = 1.05; ratio <= 5.0; ratio += 0.0731)
			{
				double outputPhase = 0.0;   // the phase of the next refresh, in source frames since frame 0
				int filled = 0;
				const int frames = 2000;
				for (int f = 0; f < frames; ++f)
				{
					// Refreshes in [f, f + 1): the first is the frame's own present, the rest are filled.
					bool first = true;
					while (outputPhase < f + 1.0)
					{
						const double phase = outputPhase - f;
						if (!first)
						{
							Assert::IsTrue(phase > 0.0 && phase < 1.0);
							++filled;
						}
						first = false;
						outputPhase += 1.0 / ratio;
					}
				}
				Assert::AreEqual(InterpolationPolicy::FilledPerFrame(ratio), static_cast<double>(filled) / frames, 0.01);
			}
			// RefreshPhase: the k-th refresh after A's own.
			Assert::AreEqual(0.0 + 1.0 / 1.2, InterpolationPolicy::RefreshPhase(0.0, 1, 1.2), 1e-12);
			Assert::AreEqual(-1.0, InterpolationPolicy::RefreshPhase(0.5, 1, 1.2), L"past B: B's refresh");
			Assert::AreEqual(-1.0, InterpolationPolicy::RefreshPhase(0.0, 1, 0.8), L"below 1: no refresh of A's own to fill");
			Assert::AreEqual(0.0, InterpolationPolicy::FilledPerFrame(0.8));
		}
	};
}

namespace InterpolationTimelineTest
{
	using InterpolationTimeline::Output;
	using InterpolationTimeline::Timeline;

	// Feeds `frames` source frames at sourceHz (optional jitter) and returns every output in order.
	static std::vector<Output> Feed(Timeline& timeline, double sourceHz, double displayHz, int frames,
		double jitter = 0.0, int firstSequence = 1, double start = 100.0, std::vector<size_t>* perFrame = nullptr)
	{
		std::vector<Output> all;
		const double period = 1.0 / sourceHz;
		for (int f = 0; f < frames; ++f)
		{
			const double wobble = jitter * std::sin(f * 2.3999632);   // deterministic, +-jitter
			const double pts = start + f * period + wobble;
			const double next = start + (f + 1) * period + jitter * std::sin((f + 1) * 2.3999632);
			const auto& out = timeline.Plan(1, firstSequence + f, pts, next, sourceHz, displayHz, false);
			if (perFrame) perFrame->push_back(out.size());
			all.insert(all.end(), out.begin(), out.end());
		}
		return all;
	}

	TEST_CLASS(InterpolationTimelineTests)
	{
	public:
		// Any ratio: the outputs are one per display period (the display's cadence),
		// every phase in [0, 1), and over a long run the refreshes per source frame
		// average display / source - frames skipped below 1, several above.
		TEST_METHOD(RatioSweepKeepsTheDisplayCadence)
		{
			for (double display = 47.0; display <= 241.0; display += 13.7)
				for (double ratio = 0.41; ratio <= 4.9; ratio += 0.173)
				{
					const double source = display / ratio;
					Timeline timeline;
					std::vector<size_t> perFrame;
					const int frames = 600;
					const std::vector<Output> all = Feed(timeline, source, display, frames, 0.0, 1, 100.0, &perFrame);
					for (size_t i = 0; i < all.size(); ++i)
					{
						Assert::IsTrue(all[i].phase >= 0.0 && all[i].phase < 1.0, L"phase in [0, 1)");
						if (i > 0)
							Assert::AreEqual(1.0 / display, all[i].pts - all[i - 1].pts, 1e-9, L"one output per display period");
					}
					const double perSource = static_cast<double>(all.size()) / frames;
					Assert::AreEqual(ratio, perSource, 0.01, L"refreshes per source frame = the ratio");
					const bool skips = std::find(perFrame.begin(), perFrame.end(), size_t{ 0 }) != perFrame.end();
					Assert::AreEqual(ratio < 1.0, skips, L"frames skipped exactly when the source is faster");
					Assert::AreEqual(0ull, timeline.Restarts(), L"a steady stream never restarts");
				}
		}

		TEST_METHOD(SixtyOnFiftyAndFiftyOnSixty)
		{
			// 60p on 50 Hz (Stage B): one frame in six is skipped; each refresh sits
			// k/5 of the way through its pair (0, 0.2, 0.4, 0.6, 0.8 of a source period).
			Timeline timeline;
			std::vector<size_t> perFrame;
			const std::vector<Output> all = Feed(timeline, 60.0, 50.0, 60, 0.0, 1, 100.0, &perFrame);
			Assert::AreEqual(size_t{ 50 }, all.size());
			Assert::AreEqual(size_t{ 10 }, static_cast<size_t>(std::count(perFrame.begin(), perFrame.end(), size_t{ 0 })));
			for (size_t i = 0; i < 5; ++i)
				Assert::AreEqual(0.2 * static_cast<double>(i), all[i].phase, 1e-9);
			// 50p on 60 Hz: one frame in five gets two refreshes; phases 0, 5/6, 4/6 (=0.667), ...
			Timeline up;
			const std::vector<Output> six = Feed(up, 50.0, 60.0, 50);
			Assert::AreEqual(size_t{ 60 }, six.size());
			Assert::AreEqual(0.0, six[0].phase, 1e-9);
			Assert::AreEqual(5.0 / 6.0, six[1].phase, 1e-9, L"A's second refresh, near B");
			Assert::AreEqual(4.0 / 6.0, six[2].phase, 1e-9, L"the next frame's own refresh is not at phase 0");
		}

		TEST_METHOD(JitterDoesNotMovePhases)
		{
			// 2 ms of capture jitter at 50p on 59.94 Hz: every refresh's position in source
			// time (its frame index + phase) stays within a small share of a period of the
			// exact one (refresh time - start) / period, whatever frame it falls in.
			Timeline timeline;
			const double source = 50.0, display = 59.94, period = 1.0 / source, start = 100.0, jitter = 0.002;
			double worst = 0.0;
			for (int f = 0; f < 600; ++f)
			{
				const double pts = start + f * period + jitter * std::sin(f * 2.3999632);
				const double next = start + (f + 1) * period + jitter * std::sin((f + 1) * 2.3999632);
				for (const Output& o : timeline.Plan(1, 1 + f, pts, next, source, display, false))
					worst = (std::max)(worst, std::abs((f + o.phase) - (o.pts - start) / period));
			}
			Assert::IsTrue(worst < 0.05, (L"worst position error (source periods) " + std::to_wstring(worst)).c_str());
			Assert::AreEqual(0ull, timeline.Reanchors());
			Assert::AreEqual(0ull, timeline.Restarts());
		}

		TEST_METHOD(DiscontinuitiesRestartAtPhaseZero)
		{
			Timeline timeline;
			Feed(timeline, 50.0, 60.0, 20);
			// A new queue generation.
			auto out = timeline.Plan(2, 21, 100.4, 100.42, 50.0, 60.0, false);
			Assert::IsTrue(!out.empty() && out[0].phase == 0.0 && out[0].pts == 100.4);
			// A sequence that does not follow (a drop).
			out = timeline.Plan(2, 30, 100.6, 100.62, 50.0, 60.0, false);
			Assert::IsTrue(!out.empty() && out[0].phase == 0.0);
			// The source discontinuity flag.
			out = timeline.Plan(2, 31, 100.62, 100.64, 50.0, 60.0, true);
			Assert::IsTrue(!out.empty() && out[0].phase == 0.0);
			// A rate change.
			out = timeline.Plan(2, 32, 100.64, 100.66, 25.0, 60.0, false);
			Assert::IsTrue(!out.empty() && out[0].phase == 0.0);
			Assert::AreEqual(4ull, timeline.Restarts());
			// Rates unknown: A, once (today's present).
			out = timeline.Plan(2, 33, 100.7, 0.0, 0.0, 60.0, false);
			Assert::AreEqual(size_t{ 1 }, out.size());
			Assert::AreEqual(0.0, out[0].phase);
		}

		TEST_METHOD(ARepeatPresentsTheLastOutputAgain)
		{
			Timeline timeline;
			Feed(timeline, 50.0, 60.0, 10);
			const Output last = timeline.Outputs().back();
			const auto& again = timeline.Plan(1, 10, 100.0 + 9 * 0.02, 100.0 + 10 * 0.02, 50.0, 60.0, false);
			Assert::AreEqual(size_t{ 1 }, again.size());
			Assert::AreEqual(last.pts, again[0].pts);
			Assert::AreEqual(last.phase, again[0].phase);
			Assert::AreEqual(1ull, timeline.Repeats());
		}

		TEST_METHOD(GapsUpdatesSkipsAndTheTrim)
		{
			// A sequence hole within the span (a dropped queued frame) continues the cadence.
			Timeline hole;
			Feed(hole, 50.0, 60.0, 10);   // sequence 1..10
			const auto& afterHole = hole.Plan(1, 12, 100.0 + 11 * 0.02, 100.0 + 12 * 0.02, 50.0, 60.0, false);
			Assert::AreEqual(0ull, hole.Restarts(), L"a dropped frame does not restart the schedule");
			Assert::IsTrue(afterHole.empty() || afterHole[0].phase < 1.0);
			// A pulldown film update (the same film time, the next sequence): no refresh of its
			// own, no restart, and the next film frame continues.
			Timeline film;
			Feed(film, 24.0, 60.0, 10);
			const double last = 100.0 + 9.0 / 24.0;
			Assert::IsTrue(film.Plan(1, 11, last, 0.0, 24.0, 60.0, false).empty(), L"an update has no refresh of its own");
			Assert::AreEqual(1ull, film.Updates());
			Assert::IsFalse(film.Plan(1, 12, last + 1.0 / 24.0, 0.0, 24.0, 60.0, false).empty());
			Assert::AreEqual(0ull, film.Restarts());
			// A repeat of a skipped frame (60p on 50 Hz) shows that frame, not an older phase.
			Timeline fast;
			std::vector<size_t> perFrame;
			Feed(fast, 60.0, 50.0, 6, 0.0, 1, 100.0, &perFrame);
			Assert::AreEqual(size_t{ 0 }, perFrame[5], L"the sixth frame is skipped");
			const auto& repeat = fast.Plan(1, 6, 100.0 + 5.0 / 60.0, 0.0, 60.0, 50.0, false);
			Assert::AreEqual(size_t{ 1 }, repeat.size());
			Assert::AreEqual(0.0, repeat[0].phase);
			Assert::AreEqual(100.0 + 5.0 / 60.0, repeat[0].pts, 1e-9);
			// The trim: wider refresh spacing drains the queue (fewer refreshes per frame).
			Timeline steady, trimmed;
			trimmed.SetRateTrim(1.003);
			const size_t plain = Feed(steady, 50.0, 60.0, 3000).size();
			const size_t fewer = Feed(trimmed, 50.0, 60.0, 3000).size();
			Assert::IsTrue(fewer < plain && fewer >= plain - 12, (L"trimmed " + std::to_wstring(fewer) + L" of " + std::to_wstring(plain)).c_str());
			trimmed.SetRateTrim(5.0);
			Assert::AreEqual(1.01, trimmed.RateTrim(), L"clamped");
		}

		TEST_METHOD(AGapPairSpansTheMissingPeriods)
		{
			// Gap fill: B two periods after A (one missed frame) spans 2 periods: the
			// refreshes in between get phases over the whole span; the frame after the
			// gap continues the same display cadence.
			Timeline timeline;
			Feed(timeline, 50.0, 50.0, 5);   // 1:1, sequence 1..5, pts 100.00 .. 100.08
			const auto& gap = timeline.Plan(1, 6, 100.10, 100.14, 50.0, 50.0, false);   // frame 7 missed
			Assert::AreEqual(2, timeline.LastSpanFrames());
			Assert::AreEqual(size_t{ 2 }, gap.size());
			Assert::AreEqual(0.0, gap[0].phase, 1e-9);
			Assert::AreEqual(0.5, gap[1].phase, 1e-9, L"the missing frame's refresh, half way across the gap");
			const auto& after = timeline.Plan(1, 7, 100.14, 100.16, 50.0, 50.0, false);
			Assert::AreEqual(size_t{ 1 }, after.size());
			Assert::AreEqual(0.0, after[0].phase, 1e-9);
			Assert::AreEqual(0ull, timeline.Restarts());
			// Without B's time (no pair across the gap): one period, and the next frame
			// resumes at the first refresh at its time.
			Timeline plain;
			Feed(plain, 50.0, 50.0, 5);
			Assert::AreEqual(size_t{ 1 }, plain.Plan(1, 6, 100.10, 0.0, 50.0, 50.0, false).size());
			const auto& resumed = plain.Plan(1, 7, 100.14, 0.0, 50.0, 50.0, false);
			Assert::AreEqual(size_t{ 1 }, resumed.size());
			Assert::AreEqual(100.14, resumed[0].pts, 1e-9);
			// A jump beyond the longest span restarts.
			plain.Plan(1, 8, 100.40, 0.0, 50.0, 50.0, false);
			Assert::AreEqual(1ull, plain.Restarts());
		}
	};
}

namespace InterpolationDebandHoldTest
{
	TEST_CLASS(InterpolationDebandHoldTests)
	{
	public:
		// Stage B skips source frames no refresh shows; VP deband's hold carries over them
		// (the next shown frame still holds), but never over a broken run.
		TEST_METHOD(TheHoldCarriesOverSkippedFramesOnly)
		{
			VpDebander::Module module;
			module.CarryHold(5);
			Assert::AreEqual(0ull, static_cast<unsigned long long>(module.HoldSequence()), L"no run: nothing to carry");
			module.SetHoldSequenceForTest(10);
			module.CarryHold(11);
			Assert::AreEqual(11ull, static_cast<unsigned long long>(module.HoldSequence()), L"carried over the skipped frame");
			module.CarryHold(13);
			Assert::AreEqual(11ull, static_cast<unsigned long long>(module.HoldSequence()), L"a hole is not carried");
			module.ForgetHold();
			module.CarryHold(12);
			Assert::AreEqual(0ull, static_cast<unsigned long long>(module.HoldSequence()), L"an invalidation is not carried");
		}
	};
}
