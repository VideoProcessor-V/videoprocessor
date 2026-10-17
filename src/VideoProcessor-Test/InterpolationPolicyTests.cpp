#include "pch.h"
#include "CppUnitTest.h"

#include <vprenderer/InterpolationPolicy.h>

#include <cmath>
#include <string>

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
				Assert::IsTrue(InterpolationPolicy::IsFilmLike(SourceClass::Measured, justFilm, display, false));
				Assert::IsFalse(InterpolationPolicy::IsFilmLike(SourceClass::Measured, justVideo, display, false));
				Assert::IsTrue(InterpolationPolicy::IsFilmLike(SourceClass::Measured, justVideo, display, true));
			}
			// 48p on 60 Hz: legacy film (HFR), measured video -> interpolated in auto.
			Input hfr = VideoInput(48.0, 60.0);
			hfr.dejudderStrength = 0.6;
			Assert::IsTrue(Decide(hfr).reason == Reason::FilmToDejudder);
			hfr.sourceClass = SourceClass::Measured;
			Assert::IsTrue(Decide(hfr).reason == Reason::Interpolate);
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
