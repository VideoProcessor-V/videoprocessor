// Fixed-constant batch 2026-10-18 (general_rate_fixes, owner rule [[vp-general-not-fixed]]):
// the rate / resolution matrix. Nothing may be hard-coded to a refresh rate, a frame rate, a
// cadence or a resolution: the film / video class is derived from the measured rate
// (DejudderPolicy::IsFilmLike(hz, true)), the gap span is a time turned into whole periods at
// the measured rate (InterpolationTimeline::MaxSpanFrames), the display / capture rate bounds
// are single shared ranges, and the pixel-based helpers depend on the picture HEIGHT only
// (never the width: DCI 4096, flat 3996, scope and anamorphic pictures behave as their height).
// Every standard rate (23.976 .. 240) must behave exactly as before; the odd rates show the
// general rule. CPU only, deterministic: no files, no GPU, no sleeps. Rate policies do not
// depend on resolution (nothing to test there beyond that).
// Skipped: the Scaled(settings, height) helper of DejudderWarp is private and needs libplacebo types.
#include "pch.h"
#include "CppUnitTest.h"

#include <DisplayRefreshRatePolicy.h>
#include <vprenderer/DejudderPolicy.h>
#include <vprenderer/GrainPreserve.h>
#include <vprenderer/InterpolationPolicy.h>
#include <vprenderer/InterpolationTimeline.h>
#include <vprenderer/MotionAheadProducer.h>
#include <GeneralRate.h>
#include <QueueLatency.h>
#include <MotionAhead.h>
#include <RefreshKey.h>
#include <LiveEpochConvergenceController.h>
#include <microsoft_directshow/DirectShowRatePolicy.h>
#include <PPMCorrectionLoader.h>
#include <ConfigFile.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GeneralRateMatrix
{
	// 1000/1001 rates as exact quotients, so the tests do not depend on a rounded literal.
	constexpr double R23976 = 24000.0 / 1001.0;
	constexpr double R2997 = 30000.0 / 1001.0;
	constexpr double R47952 = 48000.0 / 1001.0;
	constexpr double R5994 = 60000.0 / 1001.0;
	constexpr double R71928 = 72000.0 / 1001.0;
	constexpr double R11988 = 120000.0 / 1001.0;

	inline std::vector<double> StandardRates()
	{
		return { R23976, 24.0, 25.0, R2997, 30.0, R47952, 48.0, 50.0, R5994, 60.0, 100.0, R11988, 120.0, 144.0, 240.0 };
	}
	inline std::vector<double> DisplayRates()
	{
		return { 24.0, 25.0, 30.0, 48.0, 50.0, R5994, 60.0, 72.0, 100.0, R11988, 120.0, 144.0, 165.0, 240.0, 360.0 };
	}
	inline bool IsFilmStandard(double hz) { return hz < 49.0; }

	inline std::wstring W(const wchar_t* what, double a)
	{
		return std::wstring(what) + L" " + std::to_wstring(a);
	}
	inline std::wstring W(const wchar_t* what, double a, double b)
	{
		return std::wstring(what) + L" source " + std::to_wstring(a) + L" display " + std::to_wstring(b);
	}
	inline std::wstring WI(const wchar_t* what, double a, long long n)
	{
		return std::wstring(what) + L" rate " + std::to_wstring(a) + L" n " + std::to_wstring(n);
	}

	struct Picture { const wchar_t* name; int width; int height; };
	inline std::vector<Picture> Pictures()
	{
		return {
			{ L"1280x720", 1280, 720 }, { L"1920x1080", 1920, 1080 }, { L"2560x1440", 2560, 1440 },
			{ L"3840x2160", 3840, 2160 }, { L"DCI 4096x2160", 4096, 2160 }, { L"scope DCI 4096x1716", 4096, 1716 },
			{ L"flat 3996x2160", 3996, 2160 }, { L"HDV 1440x1080 (PAR 4:3)", 1440, 1080 },
			{ L"SD 720x576", 720, 576 }, { L"SD 720x480", 720, 480 } };
	}

	// The timeline: a steady stream (sequence 1..n, pts = 100 + i / rate).
	struct Stream
	{
		InterpolationTimeline::Timeline timeline;
		double rate = 0.0, display = 0.0;
		uint64_t sequence = 0;
		void Feed(int frames)
		{
			for (int i = 0; i < frames; ++i)
			{
				const double pts = 100.0 + static_cast<double>(sequence) / rate;
				++sequence;
				timeline.Plan(1, sequence, pts, 0.0, rate, display, false);
			}
		}
	};

	inline DejudderPolicy::Input DejudderCase(double source, double display, bool general, double strength = 0.5,
		bool includes1001 = true)
	{
		DejudderPolicy::Input in;
		in.strength = strength;
		in.strengthEven = 0.5;
		in.evenIncludes1001 = includes1001;
		in.sourceHz = source;
		in.displayHz = display;
		in.haveNext = true;
		in.nextMatches = true;
		in.generalRates = general;
		return in;
	}

	TEST_CLASS(ClassifierMatrix)
	{
	public:
		TEST_METHOD(StandardRatesUnchangedAndSplitAt49)
		{
			for (double hz : StandardRates())
			{
				Assert::AreEqual(DejudderPolicy::IsFilmLike(hz), DejudderPolicy::IsFilmLike(hz, true), W(L"legacy vs derived class at", hz).c_str());
				Assert::AreEqual(IsFilmStandard(hz), DejudderPolicy::IsFilmLike(hz, true), W(L"derived class (film below 49) at", hz).c_str());
				Assert::AreEqual(IsFilmStandard(hz), DejudderPolicy::IsFilmLike(hz, false), W(L"legacy class (film below 49) at", hz).c_str());
			}
		}
		TEST_METHOD(OddRatesFollowTheDerivedRule)
		{
			for (double hz : { 35.0, 40.0, 44.955, 48.8 })
			{
				Assert::IsTrue(DejudderPolicy::IsFilmLike(hz, true), W(L"odd rate is film with generalRates", hz).c_str());
				Assert::IsFalse(DejudderPolicy::IsFilmLike(hz), W(L"odd rate is video in legacy", hz).c_str());
				Assert::IsFalse(DejudderPolicy::IsFilmLike(hz, false), W(L"odd rate is video with generalRates off", hz).c_str());
			}
			for (double hz : { 49.2, R71928 })
			{
				Assert::IsFalse(DejudderPolicy::IsFilmLike(hz, true), W(L"video with generalRates", hz).c_str());
				Assert::IsFalse(DejudderPolicy::IsFilmLike(hz), W(L"video in legacy", hz).c_str());
			}
			for (double hz : { 15.0, 20.0 })
			{
				Assert::IsTrue(DejudderPolicy::IsFilmLike(hz, true), W(L"film with generalRates", hz).c_str());
				Assert::IsTrue(DejudderPolicy::IsFilmLike(hz), W(L"film in legacy", hz).c_str());
			}
		}
		TEST_METHOD(BoundaryAndInvalidRates)
		{
			Assert::IsTrue(DejudderPolicy::IsFilmLike(48.99, true), L"48.99 Hz is film");
			Assert::IsFalse(DejudderPolicy::IsFilmLike(49.0, true), L"49.0 Hz is video");
			Assert::AreEqual(49.0, DejudderPolicy::VideoRateFloorHz(), 1e-9, L"floor = 50 x (1 - 0.02)");
			Assert::IsFalse(DejudderPolicy::IsFilmLike(0.0, true), L"0 Hz");
			Assert::IsFalse(DejudderPolicy::IsFilmLike(-24.0, true), L"-24 Hz");
			Assert::IsFalse(DejudderPolicy::IsFilmLike(std::numeric_limits<double>::quiet_NaN(), true), L"NaN");
			Assert::IsFalse(DejudderPolicy::IsFilmLike(0.0, false), L"0 Hz legacy");
			Assert::IsFalse(DejudderPolicy::IsFilmLike(std::numeric_limits<double>::quiet_NaN(), false), L"NaN legacy");
		}
	};

	TEST_CLASS(DejudderMatrix)
	{
	public:
		TEST_METHOD(MainPathIdenticalWithSwitchOnAndOff)
		{
			struct Pair { double source, display; };
			std::vector<Pair> pairs;
			for (double s : { R23976, 24.0 })
				for (double d : { 24.0, 48.0, R5994, 60.0, R11988, 120.0, 144.0, 240.0 })
					pairs.push_back({ s, d });
			pairs.push_back({ 60.0, 60.0 });
			pairs.push_back({ 60.0, 120.0 });
			for (const Pair& p : pairs)
			{
				for (bool includes1001 : { true, false })
				{
					const auto on = DejudderCase(p.source, p.display, true, 0.5, includes1001);
					const auto off = DejudderCase(p.source, p.display, false, 0.5, includes1001);
					Assert::AreEqual(DejudderPolicy::EffectiveStrength(off), DejudderPolicy::EffectiveStrength(on), 1e-12, W(L"EffectiveStrength", p.source, p.display).c_str());
					Assert::IsTrue(DejudderPolicy::Decide(off) == DejudderPolicy::Decide(on), W(L"Decide", p.source, p.display).c_str());
					Assert::AreEqual(DejudderPolicy::ExtrasAllowed(off), DejudderPolicy::ExtrasAllowed(on), W(L"ExtrasAllowed", p.source, p.display).c_str());
				}
			}
		}
		TEST_METHOD(FullSweepStandardSourcesIdentical)
		{
			for (double s : StandardRates())
				for (double d : DisplayRates())
					for (double strength : { 0.0, 0.5 })
						for (bool includes1001 : { true, false })
						{
							const auto on = DejudderCase(s, d, true, strength, includes1001);
							const auto off = DejudderCase(s, d, false, strength, includes1001);
							Assert::AreEqual(DejudderPolicy::EffectiveStrength(off), DejudderPolicy::EffectiveStrength(on), 1e-12, W(L"sweep EffectiveStrength", s, d).c_str());
							Assert::IsTrue(DejudderPolicy::Decide(off) == DejudderPolicy::Decide(on), W(L"sweep Decide", s, d).c_str());
							Assert::AreEqual(DejudderPolicy::ExtrasAllowed(off), DejudderPolicy::ExtrasAllowed(on), W(L"sweep ExtrasAllowed", s, d).c_str());
							if (s >= 50.0)
								Assert::AreEqual(0.0, DejudderPolicy::EffectiveStrength(on), 1e-12, W(L"video never de-juddered", s, d).c_str());
						}
		}
		TEST_METHOD(OddFilmRatesOnUnevenCadence)
		{
			struct Case { double source, display; };
			const Case cases[] = { { 40.0, 60.0 }, { 35.0, 120.0 }, { 44.955, 60.0 }, { 35.0, 60.0 }, { 40.0, 100.0 } };
			for (const Case& c : cases)
			{
				Assert::IsTrue(DejudderPolicy::IsUnevenCadence(DejudderPolicy::CadenceRatio(c.source, c.display)), W(L"uneven cadence", c.source, c.display).c_str());
				const auto on = DejudderCase(c.source, c.display, true);
				const auto off = DejudderCase(c.source, c.display, false);
				Assert::AreEqual(0.5, DejudderPolicy::EffectiveStrength(on), 1e-12, W(L"generalRates gives the strength", c.source, c.display).c_str());
				Assert::AreEqual(0.0, DejudderPolicy::EffectiveStrength(off), 1e-12, W(L"legacy gives 0", c.source, c.display).c_str());
				Assert::IsTrue(DejudderPolicy::ExtrasAllowed(on), W(L"extras allowed", c.source, c.display).c_str());
				Assert::IsFalse(DejudderPolicy::ExtrasAllowed(off), W(L"extras not allowed (legacy)", c.source, c.display).c_str());
				Assert::IsTrue(DejudderPolicy::Decide(on) == DejudderPolicy::Reason::Active, W(L"Decide active", c.source, c.display).c_str());
				Assert::IsTrue(DejudderPolicy::Decide(off) == DejudderPolicy::Reason::IneligibleCadence, W(L"Decide ineligible (legacy)", c.source, c.display).c_str());
			}
		}
		TEST_METHOD(CadenceHelpersAreRatioOnly)
		{
			Assert::AreEqual(2, DejudderPolicy::EvenCycle(DejudderPolicy::CadenceRatio(24.0, 48.0)), L"24 on 48");
			Assert::AreEqual(5, DejudderPolicy::EvenCycle(DejudderPolicy::CadenceRatio(24.0, 120.0)), L"24 on 120");
			Assert::AreEqual(0, DejudderPolicy::EvenCycle(DejudderPolicy::CadenceRatio(24.0, 60.0)), L"24 on 60 is uneven");
			Assert::IsTrue(DejudderPolicy::IsUnevenCadence(2.5), L"2.5");
			Assert::IsFalse(DejudderPolicy::IsUnevenCadence(2.0), L"2.0");
			Assert::AreEqual(2, DejudderPolicy::MaxExtras(2.5), L"24 on 60");
			Assert::AreEqual(4, DejudderPolicy::MaxExtras(5.0), L"24 on 120");
			Assert::IsTrue(DejudderPolicy::IsRate1001(R23976), L"23.976");
			Assert::IsTrue(DejudderPolicy::IsRate1001(R2997), L"29.97");
			Assert::IsTrue(DejudderPolicy::IsRate1001(R47952), L"47.952");
			Assert::IsFalse(DejudderPolicy::IsRate1001(24.0), L"24");
			Assert::IsFalse(DejudderPolicy::IsRate1001(30.0), L"30");
		}
	};

	TEST_CLASS(InterpolationDecisionMatrix)
	{
	public:
		TEST_METHOD(FullSweepIdenticalWithSwitchOnAndOff)
		{
			using namespace InterpolationPolicy;
			for (Mode mode : { Mode::Auto, Mode::Always })
				for (SourceClass cls : { SourceClass::Legacy, SourceClass::Measured })
					for (double s : StandardRates())
						for (double d : DisplayRates())
							for (double strength : { 0.0, 0.5 })
							{
								Input in;
								in.mode = mode;
								in.sourceClass = cls;
								in.sourceHz = s;
								in.displayHz = d;
								in.dejudderStrength = strength;
								in.dejudderStrengthEven = strength;
								in.dejudderEvenIncludes1001 = true;
								in.haveNext = true;
								in.perVsyncAvailable = true;
								in.classFixes = true;
								in.generalRates = true;
								const Decision on = Decide(in);
								in.generalRates = false;
								const Decision off = Decide(in);
								Assert::IsTrue(on.reason == off.reason, W(L"same reason on/off", s, d).c_str());
								Assert::AreEqual(off.filmLike, on.filmLike, W(L"same filmLike on/off", s, d).c_str());
								Assert::AreEqual(IsFilmStandard(s), on.filmLike, W(L"filmLike below 49 only", s, d).c_str());
							}
		}
		TEST_METHOD(VideoRatesNeverFilmAndFilmRatesAlwaysFilm)
		{
			using namespace InterpolationPolicy;
			for (double s : StandardRates())
				for (double d : DisplayRates())
				{
					Input in;
					in.mode = Mode::Auto;
					in.sourceHz = s;
					in.displayHz = d;
					in.haveNext = true;
					in.perVsyncAvailable = true;
					const Decision decision = Decide(in);
					if (s >= 50.0)
						Assert::IsFalse(decision.filmLike, W(L"video never filmLike", s, d).c_str());
					else
						Assert::IsTrue(decision.filmLike, W(L"23.976..48 always filmLike", s, d).c_str());
				}
		}
		TEST_METHOD(OddRatesClassifiedByTheDerivedRule)
		{
			using namespace InterpolationPolicy;
			for (double s : { 35.0, 40.0, 44.955, 48.8 })
			{
				Assert::IsTrue(IsFilmLike(SourceClass::Measured, s, 120.0, false, true, true), W(L"film (general)", s).c_str());
				Assert::IsFalse(IsFilmLike(SourceClass::Measured, s, 120.0, false, true, false), W(L"video (legacy)", s).c_str());
			}
			for (double s : { 49.2, R71928 })
				Assert::IsFalse(IsFilmLike(SourceClass::Measured, s, 120.0, false, true, true), W(L"video (general)", s).c_str());
		}
	};

	TEST_CLASS(GapSpanMatrix)
	{
	public:
		TEST_METHOD(MaxSpanFramesTable)
		{
			struct Row { double rate; int frames; };
			const Row table[] = { { R23976, 4 }, { 24.0, 4 }, { 25.0, 4 }, { R2997, 5 }, { 30.0, 5 }, { R47952, 8 }, { 48.0, 8 },
				{ 50.0, 8 }, { R5994, 10 }, { 60.0, 10 }, { R71928, 12 }, { 100.0, 16 }, { R11988, 20 }, { 120.0, 20 },
				{ 144.0, 24 }, { 240.0, 40 } };
			for (const Row& r : table)
				Assert::AreEqual(r.frames, InterpolationTimeline::MaxSpanFrames(r.rate), W(L"span frames at", r.rate).c_str());
		}
		TEST_METHOD(LegacyAndUnknownRatesGiveFour)
		{
			for (double rate : StandardRates())
				Assert::AreEqual(4, InterpolationTimeline::MaxSpanFrames(rate, false), W(L"inTime off at", rate).c_str());
			const double bad[] = { 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() };
			for (double rate : bad)
			{
				Assert::AreEqual(4, InterpolationTimeline::MaxSpanFrames(rate), W(L"unknown rate", rate).c_str());
				Assert::AreEqual(4, InterpolationTimeline::MaxSpanFrames(rate, false), W(L"unknown rate (off)", rate).c_str());
			}
			Assert::AreEqual(4, InterpolationTimeline::kMaxSpanFrames, L"legacy span");
		}
		TEST_METHOD(SpanIsTheTimeInWholePeriods)
		{
			std::vector<double> rates = StandardRates();
			for (double extra : { R71928, 15.0, 20.0, 35.0, 40.0, 44.955, 48.8, 49.2 })
				rates.push_back(extra);
			for (double rate : rates)
			{
				const int frames = InterpolationTimeline::MaxSpanFrames(rate);
				Assert::IsTrue(frames >= 1, W(L"at least one period at", rate).c_str());
				Assert::IsTrue(frames / rate <= InterpolationTimeline::kMaxSpanSeconds + 1e-9, W(L"span within the time at", rate).c_str());
				Assert::IsTrue((frames + 1) / rate > InterpolationTimeline::kMaxSpanSeconds, W(L"one more period would exceed the time at", rate).c_str());
			}
		}
		TEST_METHOD(GapFillPairBoundary)
		{
			using InterpolationTimeline::GapFillPair;
			for (double rate : StandardRates())
			{
				const int m = InterpolationTimeline::MaxSpanFrames(rate);
				for (int gap = 1; gap < m; ++gap)
					Assert::IsTrue(GapFillPair(true, true, static_cast<uint32_t>(gap), true, false, m), WI(L"gap fills", rate, gap).c_str());
				Assert::IsFalse(GapFillPair(true, true, static_cast<uint32_t>(m), true, false, m), WI(L"gap at the span does not fill", rate, m).c_str());
				Assert::IsFalse(GapFillPair(true, true, 0, true, false, m), W(L"flush never fills", rate).c_str());
				Assert::IsFalse(GapFillPair(true, true, 1, true, true, m), W(L"low latency never fills", rate).c_str());
				Assert::IsFalse(GapFillPair(true, true, 1, false, false, m), W(L"time not within the span", rate).c_str());
			}
			Assert::IsTrue(GapFillPair(true, true, 3, true, false), L"default argument: gap 3 fills");
			Assert::IsFalse(GapFillPair(true, true, 4, true, false), L"default argument: gap 4 does not");
		}
	};

	TEST_CLASS(TimelineSpanMatrix)
	{
	public:
		static void CheckGap(double rate, double display, bool inTime)
		{
			Stream s;
			s.rate = rate;
			s.display = display;
			s.timeline.SetSpanInTime(inTime);
			Assert::AreEqual(inTime, s.timeline.SpanInTime(), L"SpanInTime round trip");
			s.Feed(12);
			const unsigned long long restarts = s.timeline.Restarts();
			// In time: the longest pair that also fits kMaxOutputs refreshes (MaxPairSpanFrames).
			const int m = inTime ? InterpolationTimeline::MaxPairSpanFrames(rate, display) :
				InterpolationTimeline::MaxSpanFrames(rate);
			const int k = m - 1;   // missed frames
			const double pts = 100.0 + static_cast<double>(s.sequence) / rate;
			s.timeline.Plan(1, s.sequence + 1, pts, pts + (k + 1) / rate, rate, display, false);
			const int expected = (inTime || k + 1 <= 4) ? k + 1 : 1;
			const std::wstring label = std::wstring(inTime ? L"span in time" : L"legacy span") + L" rate " + std::to_wstring(rate) +
				L" display " + std::to_wstring(display);
			Assert::AreEqual(expected, s.timeline.LastSpanFrames(), label.c_str());
			Assert::AreEqual(restarts, s.timeline.Restarts(), (label + L": no restart").c_str());
			if (inTime)
				Assert::IsTrue(s.timeline.Initialized(), (label + L": the pair fits the output budget (no single-present fallback)").c_str());
		}
		TEST_METHOD(PairSpanFitsTheOutputBudget)
		{
			// The owner's paths keep the full time span; high rates are capped by kMaxOutputs refreshes.
			Assert::AreEqual(4, InterpolationTimeline::MaxPairSpanFrames(24000.0 / 1001.0, 60.0), L"23.976p on 60 Hz");
			Assert::AreEqual(4, InterpolationTimeline::MaxPairSpanFrames(24.0, 24.0), L"24p on 24 Hz");
			Assert::AreEqual(10, InterpolationTimeline::MaxPairSpanFrames(60.0, 60.0), L"60p on 60 Hz");
			Assert::AreEqual(10, InterpolationTimeline::MaxPairSpanFrames(60000.0 / 1001.0, 60000.0 / 1001.0), L"59.94p on 59.94 Hz");
			Assert::AreEqual(8, InterpolationTimeline::MaxPairSpanFrames(60.0, 120.0), L"60p on 120 Hz");
			Assert::AreEqual(16, InterpolationTimeline::MaxPairSpanFrames(120.0, 120.0), L"120p on 120 Hz");
			Assert::AreEqual(1, InterpolationTimeline::MaxPairSpanFrames(24.0, 240.0), L"24p on 240 Hz: no gap pair");
			Assert::AreEqual(20, InterpolationTimeline::MaxPairSpanFrames(120.0, 0.0), L"display unknown: the time span");
			Assert::AreEqual(4, InterpolationTimeline::MaxPairSpanFrames(120.0, 120.0, false), L"legacy");
			for (double rate : StandardRates())
				for (double display : DisplayRates())
				{
					const int span = InterpolationTimeline::MaxPairSpanFrames(rate, display);
					Assert::IsTrue(span >= 1 && span <= InterpolationTimeline::MaxSpanFrames(rate), W(L"pair span within the time span", rate, display).c_str());
					Assert::IsTrue(span == 1 || span * display / rate <= static_cast<double>(InterpolationTimeline::kMaxOutputs) + 1e-6,
						W(L"pair span fits the outputs", rate, display).c_str());
				}
		}
		TEST_METHOD(GapPairSpanAtEverySourceRate)
		{
			for (double rate : StandardRates())
			{
				CheckGap(rate, rate, true);
				CheckGap(rate, rate * 2.0, true);
				if (rate < 49.0)
					CheckGap(rate, 60.0, true);
			}
			CheckGap(R71928, R71928, true);
			CheckGap(R71928, R71928 * 2.0, true);
		}
		TEST_METHOD(LegacySpanIsFourPeriods)
		{
			for (double rate : StandardRates())
			{
				CheckGap(rate, rate, false);
				CheckGap(rate, rate * 2.0, false);
				if (rate < 49.0)
					CheckGap(rate, 60.0, false);
			}
		}
		TEST_METHOD(JumpBeyondTheSpanRestarts)
		{
			for (double rate : StandardRates())
			{
				Stream s;
				s.rate = rate;
				s.display = rate;
				s.Feed(12);
				const unsigned long long restarts = s.timeline.Restarts();
				const int jump = InterpolationTimeline::MaxSpanFrames(rate) + 2;
				const double pts = 100.0 + static_cast<double>(s.sequence - 1 + jump) / rate;
				s.timeline.Plan(1, s.sequence + static_cast<uint64_t>(jump), pts, 0.0, rate, rate, false);
				Assert::AreEqual(restarts + 1, s.timeline.Restarts(), W(L"jump of span + 2 periods restarts at", rate).c_str());
			}
		}
		TEST_METHOD(SteadyStreamNeverRestarts)
		{
			for (double rate : StandardRates())
				for (double display : DisplayRates())
				{
					Stream s;
					s.rate = rate;
					s.display = display;
					s.Feed(30);
					Assert::AreEqual(0ull, s.timeline.Restarts(), W(L"steady stream does not restart", rate, display).c_str());
				}
		}
	};

	TEST_CLASS(BoundsMatrix)
	{
	public:
		TEST_METHOD(DisplayBoundsAcceptRange)
		{
			for (double hz : { R23976, 24.0, 25.0, R2997, 30.0, R47952, 48.0, 50.0, R5994, 60.0, 72.0, 100.0, R11988, 120.0, 144.0, 165.0, 240.0, 360.0, 480.0, 500.0 })
				Assert::IsTrue(DisplayRefreshBounds::Accepts(hz), W(L"display accepts", hz).c_str());
			for (double hz : { 9.9, 500.1, 0.0, -60.0, std::numeric_limits<double>::quiet_NaN() })
				Assert::IsFalse(DisplayRefreshBounds::Accepts(hz), W(L"display rejects", hz).c_str());
		}
		TEST_METHOD(DisplayPeriodBounds)
		{
			const long long qpc = 10000000;
			for (double hz : { 24.0, 60.0, 144.0, 240.0, 360.0, 480.0 })
				Assert::IsTrue(DisplayRefreshBounds::AcceptsPeriod(static_cast<long long>(std::llround(qpc / hz)), qpc), W(L"period accepted at", hz).c_str());
			for (double hz : { 520.0, 9.0 })
				Assert::IsFalse(DisplayRefreshBounds::AcceptsPeriod(static_cast<long long>(std::llround(qpc / hz)), qpc), W(L"period rejected at", hz).c_str());
			Assert::IsFalse(DisplayRefreshBounds::AcceptsPeriod(0, qpc), L"zero period");
			Assert::IsFalse(DisplayRefreshBounds::AcceptsPeriod(-166667, qpc), L"negative period");
			Assert::IsFalse(DisplayRefreshBounds::AcceptsPeriod(166667, 0), L"zero frequency");
			Assert::IsFalse(DisplayRefreshBounds::AcceptsPeriod(166667, -qpc), L"negative frequency");
		}
		TEST_METHOD(CaptureBounds)
		{
			for (double hz : StandardRates())
				Assert::IsTrue(CaptureRateBounds::Accepts(hz), W(L"capture accepts", hz).c_str());
			Assert::IsFalse(CaptureRateBounds::Accepts(250.0), L"capture rejects 250");
			Assert::IsFalse(CaptureRateBounds::Accepts(9.9), L"capture rejects 9.9");
			Assert::IsFalse(CaptureRateBounds::Accepts(std::numeric_limits<double>::quiet_NaN()), L"capture rejects NaN");
		}
	};

	TEST_CLASS(ResolutionMatrix)
	{
	public:
		TEST_METHOD(SearchRangeScalesWithHeightOnly)
		{
			for (int range = 0; range <= 8; ++range)
				for (const Picture& p : Pictures())
				{
					const int expected = (std::max)(0, (std::min)(static_cast<int>(std::lround(range * p.height / 1080.0)), 16));
					const std::wstring label = std::wstring(L"search range ") + std::to_wstring(range) + L" at " + p.name;
					Assert::AreEqual(expected, DejudderPolicy::ScaledSearchRange(range, 1080.0f, p.height), label.c_str());
					// no reference height: unscaled (clamped), whatever the picture
					Assert::AreEqual(range, DejudderPolicy::ScaledSearchRange(range, 0.0f, p.height), (label + L" (no reference)").c_str());
				}
		}
		TEST_METHOD(SearchRangeMonotonicInHeight)
		{
			std::vector<int> heights;
			for (const Picture& p : Pictures())
				heights.push_back(p.height);
			std::sort(heights.begin(), heights.end());
			for (int range = 0; range <= 16; ++range)
			{
				int previous = 0;
				for (int h : heights)
				{
					const int value = DejudderPolicy::ScaledSearchRange(range, 1080.0f, h);
					Assert::IsTrue(value >= previous, (std::wstring(L"monotonic at range ") + std::to_wstring(range) + L" height " + std::to_wstring(h)).c_str());
					Assert::IsTrue(value >= 0 && value <= 16, (std::wstring(L"clamped at height ") + std::to_wstring(h)).c_str());
					previous = value;
				}
			}
		}
		TEST_METHOD(ReferenceValuesAt2160Unchanged)
		{
			Assert::AreEqual(4, DejudderPolicy::ScaledSearchRange(2, 1080.0f, 2160), L"range 2 at 2160 doubles");
			Assert::AreEqual(2, DejudderPolicy::ScaledSearchRange(2, 1080.0f, 1080), L"range 2 at 1080 unchanged");
			Assert::AreEqual(1, DejudderPolicy::ScaledSearchRange(2, 1080.0f, 720), L"range 2 at 720 (1.33 rounds to 1)");
			Assert::AreEqual(16, DejudderPolicy::ScaledSearchRange(12, 1080.0f, 2160), L"clamped to 16");
		}
		TEST_METHOD(WidthNeverMatters)
		{
			// Pictures of one height (3840x2160, DCI 4096x2160, flat 3996x2160; the helpers take no width):
			// one answer from every height-based helper.
			for (const Picture& a : Pictures())
				for (const Picture& b : Pictures())
					if (a.height == b.height)
					{
						Assert::AreEqual(GrainPreserve::GrainScale(a.height), GrainPreserve::GrainScale(b.height), 1e-12, a.name);
						for (int range = 0; range <= 8; ++range)
							Assert::AreEqual(DejudderPolicy::ScaledSearchRange(range, 1080.0f, a.height), DejudderPolicy::ScaledSearchRange(range, 1080.0f, b.height), a.name);
					}
		}
		TEST_METHOD(GrainScaleFollowsPlaneHeight)
		{
			for (const Picture& p : Pictures())
				Assert::AreEqual(1.0, GrainPreserve::GrainScale(p.height), 1e-12, (std::wstring(L"grain scale at ") + p.name).c_str());
			Assert::AreEqual(2.0, GrainPreserve::GrainScale(4320.0), 1e-12, L"8K");
			Assert::AreEqual(1.0, GrainPreserve::GrainScale(2160.0), 1e-12, L"2160 reference");
			Assert::AreEqual(GrainPreserve::kLowSigma, GrainPreserve::LowSigmaFor(2160.0), 1e-12, L"sigma at the reference");
			Assert::AreEqual(GrainPreserve::kLowSigma * 2.0, GrainPreserve::LowSigmaFor(4320.0), 1e-12, L"sigma at 8K");
		}
		TEST_METHOD(CameraWindowByHalvings)
		{
			Assert::AreEqual(6.0f, DejudderPolicy::CameraWindowPixels(0), L"no halving: 6 px floor");
			Assert::AreEqual(8.0f, DejudderPolicy::CameraWindowPixels(1), L"1 halving");
			Assert::AreEqual(16.0f, DejudderPolicy::CameraWindowPixels(2), L"2 halvings");
			Assert::AreEqual(32.0f, DejudderPolicy::CameraWindowPixels(3), L"3 halvings");
			Assert::AreEqual(6.0f, DejudderPolicy::CameraWindowPixels(-1), L"negative clamps to 0 halvings");
			float previous = 0.0f;
			for (int h = 0; h <= 8; ++h)
			{
				const float value = DejudderPolicy::CameraWindowPixels(h);
				Assert::IsTrue(value >= previous, (std::wstring(L"monotonic at halvings ") + std::to_wstring(h)).c_str());
				previous = value;
			}
		}
	};

	// ---------------------------------------------------------------------------------------
	// General-rate helpers (batches B3-B7): the matrix of rates and heights for the shared rules.
	// ---------------------------------------------------------------------------------------
	inline std::vector<double> MatrixRates()
	{
		return { R23976, 24.0, 25.0, R2997, 30.0, R47952, 48.0, 50.0, R5994, 60.0, R71928, 72.0, 90.0, 100.0, R11988, 120.0,
			143.856, 144.0, 165.0, 200.0, 239.76, 240.0 };
	}
	inline std::vector<double> OddRates()
	{
		return { 15.0, 20.0, 35.5, 61.0, 62.5, 75.0, 59.999, 60.002, 1.0e6 / 16666.0 };
	}
	inline std::vector<double> AllMatrixRates()
	{
		std::vector<double> rates = MatrixRates();
		for (double r : OddRates()) rates.push_back(r);
		return rates;
	}
	// The rates that count as "at or below 60 Hz" for every frame-count rule (60 x (1 + 1e-3) = 60.06).
	inline bool AtOrBelow60(double hz) { return hz <= 60.0 * (1.0 + 1e-3); }
	inline std::vector<int> MatrixHeights()
	{
		std::vector<int> heights = { 480, 576, 720, 1080, 1440, 1600, 2160, 2880, 4320 };
		for (const Picture& p : Pictures()) heights.push_back(p.height);
		std::sort(heights.begin(), heights.end());
		heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
		return heights;
	}
	inline std::wstring WH(const wchar_t* what, int height)
	{
		return std::wstring(what) + L" height " + std::to_wstring(height);
	}
	inline std::wstring AsWide(const char* text) { return std::wstring(text, text + std::strlen(text)); }

	TEST_CLASS(FramesWithFloorMatrix)
	{
	public:
		TEST_METHOD(LegacyCountsUnchangedAtAndBelow60)
		{
			const long long counts[] = { 1, 4, 5, 6, 12, 16, 120, 600, 2400 };
			const double nan = std::numeric_limits<double>::quiet_NaN();
			for (long long legacy : counts)
			{
				for (double r : AllMatrixRates())
				{
					if (AtOrBelow60(r))
						Assert::AreEqual(legacy, GeneralRate::FramesWithFloor(legacy, r, true), WI(L"unchanged at or below 60 Hz", r, legacy).c_str());
					Assert::AreEqual(legacy, GeneralRate::FramesWithFloor(legacy, r, false), WI(L"general off", r, legacy).c_str());
				}
				for (double r : { 0.0, nan, -24.0, -120.0 })
					Assert::AreEqual(legacy, GeneralRate::FramesWithFloor(legacy, r, true), WI(L"unknown rate", r, legacy).c_str());
			}
			Assert::AreEqual(0ll, GeneralRate::FramesWithFloor(0ll, 120.0, true), L"legacy 0 stays 0");
			Assert::AreEqual(-3ll, GeneralRate::FramesWithFloor(-3ll, 120.0, true), L"legacy negative stays");
			for (double r : { 59.94, 60.0, 60.002 })
				Assert::AreEqual(12ll, GeneralRate::FramesWithFloor(12ll, r, true), W(L"60 Hz family keeps 12 at", r).c_str());
		}
		TEST_METHOD(AboveSixtyKeepsTheSixtyHertzTime)
		{
			const long long counts[] = { 1, 4, 5, 6, 12, 16, 120, 600, 2400 };
			for (long long legacy : counts)
				for (double r : AllMatrixRates())
				{
					if (AtOrBelow60(r)) continue;
					const long long n = GeneralRate::FramesWithFloor(legacy, r, true);
					Assert::IsTrue(n >= legacy, WI(L"not below legacy at", r, legacy).c_str());
					const double seconds = static_cast<double>(n) / r, target = static_cast<double>(legacy) / 60.0;
					// Never more than one period over the 60 Hz time, and short of it by no more than the 1e-3
					// measurement tolerance (2400 at 240 Hz is 9591 frames, not 9600).
					Assert::IsTrue(seconds <= target + 1.0 / r + 1e-9, WI(L"time not over target by more than a period at", r, legacy).c_str());
					Assert::IsTrue(seconds >= target * (1.0 - 1e-3) - 1e-9, WI(L"time not short of target by more than the tolerance at", r, legacy).c_str());
				}
		}
		TEST_METHOD(ExactValues)
		{
			Assert::AreEqual(8ll, GeneralRate::FramesWithFloor(4ll, 120.0), L"4 @ 120");
			Assert::AreEqual(16ll, GeneralRate::FramesWithFloor(4ll, 240.0), L"4 @ 240");
			Assert::AreEqual(32ll, GeneralRate::FramesWithFloor(16ll, 120.0), L"16 @ 120");
			Assert::AreEqual(24ll, GeneralRate::FramesWithFloor(12ll, 120.0), L"12 @ 120");
			Assert::AreEqual(8ll, GeneralRate::FramesWithFloor(4ll, R11988), L"4 @ 119.88");
			Assert::AreEqual(8ll, GeneralRate::FramesWithFloor(4ll, 1.0e6 / 8333.0), L"4 @ 8333 us period (120.005 Hz) is 8, not 9");
			Assert::AreEqual(8, GeneralRate::FramesWithFloor(4, 120.0), L"int overload");
			Assert::AreEqual(size_t{ 8 }, GeneralRate::FramesWithFloor(size_t{ 4 }, 120.0), L"size_t overload");
			Assert::AreEqual(uint32_t{ 8 }, GeneralRate::FramesWithFloor(uint32_t{ 4 }, 120.0), L"uint32_t overload");
			Assert::AreEqual(4, GeneralRate::FramesWithFloor(4, 120.0, false), L"int overload off");
		}
		TEST_METHOD(MonotonicInRate)
		{
			for (long long legacy : { 1ll, 4ll, 5ll, 6ll, 12ll, 16ll, 120ll, 600ll, 2400ll })
			{
				long long previous = 0;
				for (double r = 1.0; r <= 500.0; r += 0.25)
				{
					const long long n = GeneralRate::FramesWithFloor(legacy, r, true);
					Assert::IsTrue(n >= previous, WI(L"monotonic at", r, legacy).c_str());
					previous = n;
				}
			}
		}
		TEST_METHOD(SecondsOfIsTheSixtyHertzTime)
		{
			Assert::AreEqual(0.4, GeneralRate::SecondsOf(24.0), 1e-12, L"24 frames at 60 Hz");
			Assert::AreEqual(2.0, GeneralRate::SecondsOf(120.0), 1e-12, L"120 frames at 60 Hz");
		}
	};

	TEST_CLASS(PerFrameWeightMatrix)
	{
	public:
		TEST_METHOD(UnchangedAtAndBelow60)
		{
			for (double alpha : { 0.05, 0.1, 0.5, 0.9 })
				for (double r : AllMatrixRates())
				{
					if (AtOrBelow60(r))
						Assert::AreEqual(alpha, GeneralRate::PerFrameWeight(alpha, r, true), 1e-15, W(L"weight unchanged at", r).c_str());
					Assert::AreEqual(alpha, GeneralRate::PerFrameWeight(alpha, r, false), 1e-15, W(L"weight unchanged when off at", r).c_str());
				}
			Assert::AreEqual(0.5, GeneralRate::PerFrameWeight(0.5, 0.0, true), 1e-15, L"unknown rate");
			Assert::AreEqual(0.0, GeneralRate::PerFrameWeight(0.0, 120.0, true), 1e-15, L"alpha 0");
			Assert::AreEqual(1.0, GeneralRate::PerFrameWeight(1.0, 120.0, true), 1e-15, L"alpha 1");
		}
		TEST_METHOD(At120IsOneMinusSqrtHalf)
		{
			Assert::AreEqual(1.0 - std::sqrt(0.5), GeneralRate::PerFrameWeight(0.5, 120.0, true), 1e-12, L"0.5 @ 120");
		}
		TEST_METHOD(TimeConstantPreservedAbove60)
		{
			for (double alpha : { 0.1, 0.5, 0.9 })
				for (double r : AllMatrixRates())
				{
					if (AtOrBelow60(r)) continue;
					const double w = GeneralRate::PerFrameWeight(alpha, r, true);
					Assert::IsTrue(w > 0.0 && w < alpha, W(L"weight smaller above 60 Hz at", r).c_str());
					Assert::AreEqual(1.0 - alpha, std::pow(1.0 - w, r / 60.0), 1e-9, W(L"decay per 1/60 s preserved at", r).c_str());
				}
		}
	};

	TEST_CLASS(HelpersMatrix)
	{
	public:
		TEST_METHOD(LogIntervalFollowsTheClock)
		{
			using Clock = std::chrono::steady_clock;
			const Clock::time_point t0 = Clock::time_point{} + std::chrono::seconds(1000);
			GeneralRate::LogInterval log(2.0);
			Assert::IsTrue(log.Due(t0), L"first call is due");
			Assert::IsFalse(log.Due(t0 + std::chrono::seconds(1)), L"1 s: not due");
			Assert::IsTrue(log.Due(t0 + std::chrono::seconds(2)), L"2 s: due");
			Assert::IsFalse(log.Due(t0 + std::chrono::seconds(3)), L"3 s: not due");
			Assert::IsTrue(log.DueOr(true, t0 + std::chrono::seconds(3)), L"forced");
			Assert::IsFalse(log.Due(t0 + std::chrono::seconds(4)), L"interval restarts at the forced event");
			Assert::IsFalse(log.DueOr(false, t0 + std::chrono::seconds(4)), L"unforced follows Due");
			Assert::IsTrue(log.Due(t0 + std::chrono::seconds(5)), L"due again");
			GeneralRate::LogInterval frames = GeneralRate::LogInterval::ForFrames(120.0);
			Assert::IsTrue(frames.Due(t0), L"ForFrames first");
			Assert::IsFalse(frames.Due(t0 + std::chrono::milliseconds(1999)), L"120 frames = 2 s: not yet");
			Assert::IsTrue(frames.Due(t0 + std::chrono::milliseconds(2000)), L"120 frames = 2 s");
			GeneralRate::LogInterval none(0.0);
			Assert::IsTrue(none.Due(t0), L"zero interval first");
			Assert::IsTrue(none.Due(t0), L"zero interval is always due");
		}
		TEST_METHOD(FallbackNoticeLogsOncePerFallback)
		{
			GeneralRate::FallbackNotice notice;
			Assert::IsFalse(notice.Use(true), L"measured: no notice");
			Assert::IsTrue(notice.Use(false), L"first unmeasured use");
			Assert::IsFalse(notice.Use(false), L"still unmeasured");
			Assert::IsFalse(notice.Use(true), L"measured again");
			Assert::IsTrue(notice.Use(false), L"unmeasured after a measured call");
			Assert::AreEqual(uint64_t{ 2 }, notice.Uses(), L"two notices");
		}
		TEST_METHOD(ProcessFlagRoundTrip)
		{
			GeneralRate::SetProcessEnabled(false);
			Assert::IsFalse(GeneralRate::ProcessEnabled(), L"off");
			GeneralRate::SetProcessEnabled(true);
			Assert::IsTrue(GeneralRate::ProcessEnabled(), L"on");
		}
		TEST_METHOD(GeneralScaleByHeight)
		{
			for (int h : MatrixHeights())
			{
				const double on = GeneralScale::AboveReference(h, true), off = GeneralScale::AboveReference(h, false);
				Assert::AreEqual(1.0, off, 1e-15, WH(L"off is 1 at", h).c_str());
				Assert::AreEqual(h > 2160 ? h / 2160.0 : 1.0, on, 1e-12, WH(L"scale at", h).c_str());
				if (h <= 2160)
					Assert::AreEqual(40, GeneralScale::AboveReferencePixels(40, h, true), WH(L"pixels unchanged at", h).c_str());
				Assert::AreEqual(40, GeneralScale::AboveReferencePixels(40, h, false), WH(L"pixels off at", h).c_str());
			}
			Assert::AreEqual(80, GeneralScale::AboveReferencePixels(40, 4320.0, true), L"40 px at 4320");
			Assert::AreEqual(1.0, GeneralScale::AboveReference(std::numeric_limits<double>::quiet_NaN(), true), 1e-15, L"NaN lines");
			Assert::AreEqual(1.0, GeneralScale::AboveReference(0.0, true), 1e-15, L"0 lines");
		}
	};

	TEST_CLASS(QueueLatencyMatrix)
	{
	public:
		TEST_METHOD(IdenticalAtAndBelow60)
		{
			for (double r : AllMatrixRates())
			{
				if (!AtOrBelow60(r)) continue;
				Assert::AreEqual(size_t{ 16 }, QueueLatency::MaxFramesForLatency(r, true), W(L"cap 16 at", r).c_str());
				Assert::AreEqual(size_t{ 16 }, QueueLatency::MaxFramesForLatency(r, false), W(L"cap 16 (off) at", r).c_str());
				for (int ms : { 0, 1, 50, 125, 167, 200, 250, 500, 1000 })
					Assert::AreEqual(QueueLatency::FramesForLatency(ms, r, false), QueueLatency::FramesForLatency(ms, r, true), WI(L"frames for latency on/off at", r, ms).c_str());
			}
		}
		TEST_METHOD(CapScalesAbove60)
		{
			for (double r : AllMatrixRates())
			{
				if (AtOrBelow60(r)) continue;
				Assert::AreEqual(GeneralRate::FramesWithFloor(size_t{ 16 }, r), QueueLatency::MaxFramesForLatency(r, true), W(L"cap above 60 at", r).c_str());
				Assert::AreEqual(size_t{ 16 }, QueueLatency::MaxFramesForLatency(r, false), W(L"cap off at", r).c_str());
				for (int ms : { 1, 50, 125, 250, 1000 })
				{
					const size_t on = QueueLatency::FramesForLatency(ms, r, true), off = QueueLatency::FramesForLatency(ms, r, false);
					Assert::IsTrue(on >= off && on <= QueueLatency::MaxFramesForLatency(r, true), WI(L"latency frames within cap at", r, ms).c_str());
				}
			}
			Assert::AreEqual(size_t{ 32 }, QueueLatency::FramesForLatency(1000, 120.0, true), L"1000 ms @ 120");
			Assert::AreEqual(size_t{ 64 }, QueueLatency::FramesForLatency(1000, 240.0, true), L"1000 ms @ 240");
			Assert::AreEqual(size_t{ 16 }, QueueLatency::FramesForLatency(1000, 120.0, false), L"1000 ms @ 120 off");
		}
		TEST_METHOD(ResolveTargetFramesIdenticalOnAndOff)
		{
			struct Case { bool ownLat; int lat; bool ownFr; size_t fr; int baseLat; size_t baseFr; };
			const Case cases[] = {
				{ true, 125, false, 0, 0, 8 },     // own latency
				{ false, 0, true, 6, 0, 8 },       // own frames
				{ false, 0, false, 0, 200, 8 },    // inherited latency
				{ false, 0, false, 0, 0, 8 },      // base frames
				{ true, 0, true, 3, 200, 8 },      // own latency 0 switches the inherited one off
				{ true, 1000, false, 0, 0, 8 } };  // clamped to the cap
			for (double r : { R23976, 24.0, R5994, 60.0 })
			{
				int index = 0;
				for (const Case& c : cases)
				{
					const auto on = QueueLatency::ResolveTargetFrames(c.ownLat, c.lat, c.ownFr, c.fr, c.baseLat, c.baseFr, r, true);
					const auto off = QueueLatency::ResolveTargetFrames(c.ownLat, c.lat, c.ownFr, c.fr, c.baseLat, c.baseFr, r, false);
					const std::wstring label = WI(L"ResolveTargetFrames case", r, index++);
					Assert::AreEqual(off.frames, on.frames, label.c_str());
					Assert::AreEqual(off.fromLatency, on.fromLatency, label.c_str());
					Assert::AreEqual(off.latencyMs, on.latencyMs, label.c_str());
					Assert::AreEqual(off.effectiveMs, on.effectiveMs, 1e-12, label.c_str());
				}
			}
		}
	};

	TEST_CLASS(MotionAheadMatrix)
	{
	public:
		TEST_METHOD(DepthIsTimeBasedAtEveryRate)
		{
			// r6 fix 3: the look-ahead is a time (4 frames at 60 Hz = 66.7 ms) at every rate.
			for (double r : AllMatrixRates())
			{
				const int depth = MotionAhead::DepthAt(4, r, true);
				Assert::IsTrue(depth >= 1, W(L"at least one frame at", r).c_str());
				Assert::IsTrue(depth * 1000.0 / r >= MotionAhead::AheadMs(4) * (1.0 - 2e-3), W(L"covers the time at", r).c_str());
				Assert::IsTrue(depth == 1 || (depth - 1) * 1000.0 / r < MotionAhead::AheadMs(4), W(L"no extra frame at", r).c_str());
				Assert::AreEqual(0, MotionAhead::DepthAt(0, r, true), W(L"depth 0 at", r).c_str());
				Assert::AreEqual(4, MotionAhead::DepthAt(4, r, false), W(L"legacy (general off) at", r).c_str());
				Assert::AreEqual(0, MotionAhead::DepthAt(-2, r, true), W(L"negative configured at", r).c_str());
			}
			Assert::AreEqual(2, MotionAhead::DepthAt(4, 24000.0 / 1001.0, true), L"23.976p: 2 ahead");
			Assert::AreEqual(2, MotionAhead::DepthAt(4, 24.0, true), L"24p: 2 ahead");
			Assert::AreEqual(2, MotionAhead::DepthAt(4, 25.0, true), L"25p: 2 ahead");
			Assert::AreEqual(2, MotionAhead::DepthAt(4, 30.0, true), L"30p: 2 ahead");
			Assert::AreEqual(2, MotionAhead::DepthAt(4, 30000.0 / 1001.0, true), L"29.97p: 2 ahead");
			Assert::AreEqual(4, MotionAhead::DepthAt(4, 50.0, true), L"50p: 4 ahead");
			Assert::AreEqual(4, MotionAhead::DepthAt(4, 60000.0 / 1001.0, true), L"59.94p: 4 ahead");
			Assert::AreEqual(4, MotionAhead::DepthAt(4, 60.0, true), L"60p: 4 ahead");
			Assert::AreEqual(1, MotionAhead::DepthAt(4, 6.0, true), L"6 fps: 1 ahead");
			Assert::AreEqual(4, MotionAhead::DepthAt(4, 0.0, true), L"unmeasured: the configured frames (the queue bounds it)");
			Assert::AreEqual(4, MotionAhead::DepthAt(9, 60.0, true), L"configured clamped to 4");
		}
		TEST_METHOD(DepthKeepsTheTimeAbove60)
		{
			Assert::AreEqual(8, MotionAhead::DepthAt(4, 120.0, true), L"120");
			Assert::AreEqual(16, MotionAhead::DepthAt(4, 240.0, true), L"240");
			Assert::AreEqual(16, MotionAhead::DepthAt(4, 360.0, true), L"360 capped");
			Assert::AreEqual(8, MotionAhead::DepthAt(4, R11988, true), L"119.88");
			for (double r : AllMatrixRates())
			{
				const int depth = MotionAhead::DepthAt(4, r, true);
				Assert::IsTrue(depth >= 1 && depth <= MotionAhead::kMaxGeneralDepth, W(L"depth within bounds at", r).c_str());
			}
		}
		TEST_METHOD(QueueAndPendingJobs)
		{
			Assert::AreEqual(size_t{ 3 }, MotionAhead::QueueFramesFor(MotionAhead::DepthAt(4, 24.0)), L"queue at 24 (r6: time-based)");
			Assert::AreEqual(size_t{ 5 }, MotionAhead::QueueFramesFor(MotionAhead::DepthAt(4, 60.0)), L"queue at 60");
			Assert::AreEqual(size_t{ 6 }, MotionAhead::PendingJobsFor(4), L"pending at 4");
			Assert::AreEqual(size_t{ 18 }, MotionAhead::PendingJobsFor(16), L"pending at 16");
			Assert::AreEqual(size_t{ 6 }, MotionAhead::PendingJobsFor(0), L"pending floor");
		}
		TEST_METHOD(DecideUsesTheRateDepth)
		{
			const auto active = MotionAhead::Decide(MotionAhead::DepthAt(4, 120.0), false, true, 20);
			Assert::AreEqual(8, active.depth, L"depth @ 120");
			Assert::IsTrue(active.reason == MotionAhead::Reason::Active, L"active @ 120");
			const auto shallow = MotionAhead::Decide(8, false, true, 3);
			Assert::AreEqual(3, shallow.depth, L"shallow depth");
			Assert::IsTrue(shallow.reason == MotionAhead::Reason::ShallowQueue, L"shallow queue");
			Assert::AreEqual(MotionAhead::kMaxGeneralDepth, MotionAhead::Decide(40, false, true, 100).configured, L"configured clamped to the general cap");
		}
	};

	TEST_CLASS(DirectShowRateMatrix)
	{
	public:
		TEST_METHOD(DefaultQueueIsFiveAtEveryRate)
		{
			for (double r : AllMatrixRates())
			{
				Assert::AreEqual(size_t{ 5 }, DirectShowRatePolicy::StartupBufferingFrames(4, r, true), W(L"startup general at", r).c_str());
				Assert::AreEqual(size_t{ 5 }, DirectShowRatePolicy::StartupBufferingFrames(4, r, false), W(L"startup legacy at", r).c_str());
			}
		}
		TEST_METHOD(NominalZeroMatchesLegacyBelow30)
		{
			for (double r : AllMatrixRates())
				if (r >= 20.0 && r < 30.0)
					Assert::AreEqual(DirectShowRatePolicy::StartupBufferingFrames(0, r, false), DirectShowRatePolicy::StartupBufferingFrames(0, r, true), W(L"nominal 0 at", r).c_str());
			// Documented difference: at 15 fps the 200 ms lead is 4 frames (legacy always 5).
			Assert::AreEqual(size_t{ 4 }, DirectShowRatePolicy::StartupBufferingFrames(0, 15.0, true), L"nominal 0 at 15 general");
			Assert::AreEqual(size_t{ 5 }, DirectShowRatePolicy::StartupBufferingFrames(0, 15.0, false), L"nominal 0 at 15 legacy");
			Assert::AreEqual(DirectShowRatePolicy::StartupBufferingFrames(3, 0.0, false), DirectShowRatePolicy::StartupBufferingFrames(3, 0.0, true), L"unmeasured fps falls back to legacy");
		}
		TEST_METHOD(DeliberateDifferences)
		{
			Assert::AreEqual(size_t{ 5 }, DirectShowRatePolicy::StartupBufferingFrames(0, 60.0, true), L"nominal 0 at 60 general");
			Assert::AreEqual(size_t{ 3 }, DirectShowRatePolicy::StartupBufferingFrames(0, 60.0, false), L"nominal 0 at 60 legacy");
			for (size_t nominal : { size_t{ 5 }, size_t{ 6 }, size_t{ 8 } })
			{
				Assert::AreEqual(nominal + 1, DirectShowRatePolicy::StartupBufferingFrames(nominal, 24.0, true), L"nominal >= 5 at 24 general = nominal + 1");
				Assert::AreEqual(nominal, DirectShowRatePolicy::StartupBufferingFrames(nominal, 24.0, false), L"nominal >= 5 at 24 legacy = nominal");
			}
		}
		TEST_METHOD(DeliveryHold)
		{
			struct Scale { uint64_t scale, ticks; };
			const Scale rates[] = { { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 60, 1 }, { 120, 1 }, { 240, 1 } };
			for (const Scale& s : rates)
			{
				const std::wstring label = W(L"hold at timescale/ticks", static_cast<double>(s.scale), static_cast<double>(s.ticks));
				Assert::AreEqual(int64_t{ 50000 }, DirectShowRatePolicy::DeliveryHoldTimeoutUs(true, s.scale, s.ticks), label.c_str());
				Assert::AreEqual(int64_t{ 50000 }, DirectShowRatePolicy::DeliveryHoldTimeoutUs(false, s.scale, s.ticks), (label + L" legacy").c_str());
			}
			Assert::AreEqual(int64_t{ 76667 }, DirectShowRatePolicy::DeliveryHoldTimeoutUs(true, 15, 1), L"15 fps");
			Assert::AreEqual(int64_t{ 57500 }, DirectShowRatePolicy::DeliveryHoldTimeoutUs(true, 20, 1), L"20 fps");
			Assert::AreEqual(int64_t{ 1000000 }, DirectShowRatePolicy::DeliveryHoldTimeoutUs(true, 1, 1), L"1 fps capped");
			Assert::AreEqual(int64_t{ 50000 }, DirectShowRatePolicy::DeliveryHoldTimeoutUs(true, 0, 1), L"timescale 0");
			Assert::AreEqual(int64_t{ 50000 }, DirectShowRatePolicy::DeliveryHoldTimeoutUs(true, 24, 0), L"ticks 0");
		}
	};

	TEST_CLASS(LiveEpochConvergenceMatrix)
	{
	public:
		static uint64_t PeriodUs(double r) { return static_cast<uint64_t>(std::llround(1.0e6 / r)); }
		TEST_METHOD(UnchangedAtAndBelow60)
		{
			std::vector<uint64_t> periods;
			for (double r : AllMatrixRates())
				if (AtOrBelow60(r))
					periods.push_back(PeriodUs(r));
			periods.push_back(16666);
			periods.push_back(16667);
			for (uint64_t us : periods)
			{
				const std::wstring label = WI(L"period us", static_cast<double>(us), static_cast<long long>(us));
				Assert::AreEqual(uint32_t{ 12 }, LiveEpochConvergenceController::PacedWarmupDeliveries(us, true), (label + L" warm-up").c_str());
				Assert::AreEqual(uint32_t{ 6 }, LiveEpochConvergenceController::RequiredPacedDeliveries(us, true), (label + L" paced").c_str());
			}
			for (double r : AllMatrixRates())
			{
				const uint64_t us = PeriodUs(r);
				Assert::AreEqual(uint32_t{ 12 }, LiveEpochConvergenceController::PacedWarmupDeliveries(us, false), W(L"warm-up off at", r).c_str());
				Assert::AreEqual(uint32_t{ 6 }, LiveEpochConvergenceController::RequiredPacedDeliveries(us, false), W(L"paced off at", r).c_str());
			}
		}
		TEST_METHOD(HighRatesKeepTheTime)
		{
			Assert::AreEqual(uint32_t{ 24 }, LiveEpochConvergenceController::PacedWarmupDeliveries(8333, true), L"warm-up @ 120");
			Assert::AreEqual(uint32_t{ 12 }, LiveEpochConvergenceController::RequiredPacedDeliveries(8333, true), L"paced @ 120");
			Assert::AreEqual(uint32_t{ 48 }, LiveEpochConvergenceController::PacedWarmupDeliveries(4167, true), L"warm-up @ 240");
			Assert::AreEqual(uint32_t{ 24 }, LiveEpochConvergenceController::RequiredPacedDeliveries(4167, true), L"paced @ 240");
			for (double r : AllMatrixRates())
			{
				if (AtOrBelow60(r)) continue;
				Assert::IsTrue(LiveEpochConvergenceController::PacedWarmupDeliveries(PeriodUs(r), true) >= 12, W(L"warm-up at least legacy at", r).c_str());
				Assert::IsTrue(LiveEpochConvergenceController::RequiredPacedDeliveries(PeriodUs(r), true) >= 6, W(L"paced at least legacy at", r).c_str());
			}
		}
		TEST_METHOD(UnknownDurationIsLegacy)
		{
			Assert::AreEqual(uint32_t{ 12 }, LiveEpochConvergenceController::PacedWarmupDeliveries(0, true), L"duration 0 warm-up");
			Assert::AreEqual(uint32_t{ 6 }, LiveEpochConvergenceController::RequiredPacedDeliveries(0, true), L"duration 0 paced");
		}
	};

	TEST_CLASS(CameraWindowMatrix)
	{
	public:
		// The legacy halvings of an input height: the matcher halves while height^2 >= 2 x 1080^2, then its
		// inner matcher (half the height) may halve again.
		static int LegacyHalvings(int height)
		{
			int n = 0;
			for (long long h = height; h * h >= 2LL * 1080 * 1080; h /= 2)
				++n;
			return n;
		}
		TEST_METHOD(EqualToTheSteppedWindowAtTheReferenceHeights)
		{
			for (int h : { 720, 1080, 1440, 2160, 4320 })
			{
				const int halvings = LegacyHalvings(h);
				Assert::AreEqual(DejudderPolicy::CameraWindowPixels(halvings), DejudderPolicy::CameraWindowPixels(halvings, h, true), WH(L"window equals the stepped one at", h).c_str());
			}
			Assert::AreEqual(6.0f, DejudderPolicy::CameraWindowPixels(0, 1080, true), L"1080");
			Assert::AreEqual(8.0f, DejudderPolicy::CameraWindowPixels(1, 2160, true), L"2160");
			Assert::AreEqual(16.0f, DejudderPolicy::CameraWindowPixels(2, 4320, true), L"4320");
		}
		TEST_METHOD(WiderThanHalfTheGlobalStep)
		{
			for (int h : MatrixHeights())
			{
				const float step = 4.0f * static_cast<float>(1 << LegacyHalvings(h));
				Assert::IsTrue(DejudderPolicy::CameraWindowPixels(LegacyHalvings(h), h, true) >= 0.5f * step, WH(L"window at least half the step at", h).c_str());
			}
		}
		TEST_METHOD(ContinuousInHeight)
		{
			float previous = DejudderPolicy::CameraWindowPixels(LegacyHalvings(700), 700, true);
			for (int h = 701; h <= 4400; ++h)
			{
				const float value = DejudderPolicy::CameraWindowPixels(LegacyHalvings(h), h, true);
				Assert::IsTrue(std::fabs(value - previous) <= 0.5f, WH(L"no jump at", h).c_str());
				Assert::IsTrue(value >= previous, WH(L"non-decreasing at", h).c_str());
				previous = value;
			}
		}
		TEST_METHOD(OffIsLegacyForEveryHeight)
		{
			for (int h : MatrixHeights())
				for (int halvings = 0; halvings <= 4; ++halvings)
					Assert::AreEqual(DejudderPolicy::CameraWindowPixels(halvings), DejudderPolicy::CameraWindowPixels(halvings, h, false), WH(L"off equals legacy at", h).c_str());
			Assert::AreEqual(DejudderPolicy::CameraWindowPixels(2), DejudderPolicy::CameraWindowPixels(2, 0, true), L"no height: legacy");
		}
	};

	TEST_CLASS(FlowVoteMatrix)
	{
	public:
		TEST_METHOD(VoteRangeByGridHeight)
		{
			for (int height : MatrixHeights())
			{
				const int gridH = height / 8;
				const int on = MotionAhead::FlowVoteRange(gridH, true);
				if (8 * gridH <= 2160)
					Assert::AreEqual(40, on, WH(L"vote range at most 2160 lines, source", height).c_str());
				Assert::AreEqual(40, MotionAhead::FlowVoteRange(gridH, false), WH(L"vote range off, source", height).c_str());
				Assert::IsTrue(on >= 40, WH(L"vote range never below 40 at", height).c_str());
			}
			Assert::AreEqual(40, MotionAhead::FlowVoteRange(1080 / 8, true), L"1080");
			Assert::AreEqual(40, MotionAhead::FlowVoteRange(2160 / 8, true), L"2160");
			Assert::AreEqual(80, MotionAhead::FlowVoteRange(4320 / 8, true), L"4320");
		}
		TEST_METHOD(UniformFieldGlobalIdenticalOnAndOff)
		{
			const int gridW = 240, gridH = 135;
			const size_t cells = static_cast<size_t>(gridW) * gridH;
			std::vector<int16_t> vectors(cells * 2);
			for (size_t i = 0; i < cells; ++i)
			{
				vectors[i * 2] = 64;
				vectors[i * 2 + 1] = -32;
			}
			const std::vector<uint8_t> costs(cells, 10);
			float onGlobal[4], onSecond[4], offGlobal[4], offSecond[4];
			MotionAhead::FlowGlobal(vectors, costs, gridW, gridH, onGlobal, onSecond, true);
			MotionAhead::FlowGlobal(vectors, costs, gridW, gridH, offGlobal, offSecond, false);
			for (int i = 0; i < 4; ++i)
			{
				Assert::AreEqual(offGlobal[i], onGlobal[i], (std::wstring(L"global component ") + std::to_wstring(i)).c_str());
				Assert::AreEqual(offSecond[i], onSecond[i], (std::wstring(L"second component ") + std::to_wstring(i)).c_str());
			}
		}
	};

	TEST_CLASS(RefreshKeyMatrix)
	{
	public:
		static std::vector<double> LegacyKeys()
		{
			return { 23, 24, 25, 29, 30, 47, 48, 50, 59, 60, 71, 72, 89, 90, 100, 119, 120, 143, 144, 239, 240 };
		}
		static std::string MigratedOrSame(double key)
		{
			const std::string text = std::to_string(static_cast<int>(key));
			std::string migrated;
			return RefreshKey::MigratedKey(text, migrated) ? migrated : text;
		}
		TEST_METHOD(LegacyKeyIsTheIntegerBand)
		{
			Assert::AreEqual(23, RefreshKey::LegacyKey(R23976), L"23.976");
			Assert::AreEqual(59, RefreshKey::LegacyKey(R5994), L"59.94");
			Assert::AreEqual(60, RefreshKey::LegacyKey(60.0), L"60");
			Assert::AreEqual(60, RefreshKey::LegacyKey(59.99995), L"59.99995 reads 60");
			Assert::AreEqual(0, RefreshKey::LegacyKey(0.0), L"0");
			Assert::AreEqual(0, RefreshKey::LegacyKey(-5.0), L"negative");
			Assert::AreEqual(0, RefreshKey::LegacyKey(std::numeric_limits<double>::quiet_NaN()), L"NaN");
		}
		TEST_METHOD(ParseAcceptsRatesAndRejectsJunk)
		{
			double hz = 0.0;
			Assert::IsTrue(RefreshKey::Parse("60", hz) && hz == 60.0, L"60");
			Assert::IsTrue(RefreshKey::Parse("59.94", hz) && std::fabs(hz - 59.94) < 1e-12, L"59.94");
			Assert::IsTrue(RefreshKey::Parse("60000/1001", hz) && std::fabs(hz - 59.94006) < 1e-5, L"60000/1001");
			Assert::IsTrue(RefreshKey::Parse(" 24 ", hz) && hz == 24.0, L"padded 24");
			for (const char* bad : { "", "abc", "0", "-5", "1/0", "/", "60/" })
			{
				Assert::IsFalse(RefreshKey::Parse(bad, hz), (L"rejects '" + AsWide(bad) + L"'").c_str());
				Assert::AreEqual(0.0, hz, L"hz cleared on failure");
			}
		}
		TEST_METHOD(MigrationTable)
		{
			struct Row { const char* from; const char* to; };
			const Row table[] = { { "23", "23.976" }, { "29", "29.97" }, { "47", "47.952" }, { "59", "59.94" }, { "71", "71.928" },
				{ "89", "89.91" }, { "119", "119.88" }, { "143", "143.856" }, { "239", "239.76" } };
			for (const Row& row : table)
			{
				std::string migrated;
				Assert::IsTrue(RefreshKey::MigratedKey(row.from, migrated), (L"migrates " + AsWide(row.from)).c_str());
				Assert::AreEqual(std::string(row.to), migrated, (L"migrated spelling of " + AsWide(row.from)).c_str());
			}
			for (const char* same : { "24", "25", "30", "48", "49", "50", "60", "100", "120", "59.94", "60000/1001", "abc", "0", "1000", "" })
			{
				std::string migrated;
				Assert::IsFalse(RefreshKey::MigratedKey(same, migrated), (L"unchanged " + AsWide(same)).c_str());
			}
			Assert::AreEqual(std::string("59.94"), RefreshKey::RateText(R5994), L"RateText 59.94");
			Assert::AreEqual(std::string("24"), RefreshKey::RateText(24.0), L"RateText 24");
			Assert::IsTrue(RefreshKey::IsNtscFamilyWhole(60) && RefreshKey::IsNtscFamilyWhole(24) && !RefreshKey::IsNtscFamilyWhole(50) && !RefreshKey::IsNtscFamilyWhole(0), L"IsNtscFamilyWhole");
		}
		TEST_METHOD(FindSelectsTheSameEntryAfterMigration)
		{
			const std::vector<double> legacy = LegacyKeys();
			std::vector<double> migrated;
			for (double key : legacy)
			{
				double parsed = 0.0;
				Assert::IsTrue(RefreshKey::Parse(MigratedOrSame(key), parsed), L"migrated key parses");
				migrated.push_back(parsed);
			}
			std::vector<double> rates = MatrixRates();
			for (double r : { 15.0, 20.0, 35.5, 61.0, 62.5, 75.0 })
				rates.push_back(r);
			for (double r : rates)
			{
				const int a = RefreshKey::Find(legacy, r, false);
				const int b = RefreshKey::Find(migrated, r, true);
				Assert::AreEqual(a, b, W(L"same entry selected at", r).c_str());
			}
			Assert::AreEqual(R5994, migrated[static_cast<size_t>(RefreshKey::Find(migrated, R5994, true))], 1e-3, L"59.94 selects the 59.94 key (written 59.94, the rate 59.94006)");
			Assert::AreEqual(60.0, migrated[static_cast<size_t>(RefreshKey::Find(migrated, 60.0, true))], 1e-12, L"60 selects 60");
			Assert::AreEqual(-1, RefreshKey::Find(migrated, 0.0, true), L"unknown rate");
			Assert::AreEqual(-1, RefreshKey::Find(std::vector<double>{}, 60.0, true), L"no keys");
		}
		TEST_METHOD(GeneralPrefersTheExactKey)
		{
			// Intended fix: 59.9997 Hz is 60, legacy truncation reads the 59 band.
			const std::vector<double> bandKeys = { 59.0, 60.0 };
			Assert::AreEqual(0, RefreshKey::Find(bandKeys, 59.9997, false), L"legacy picks 59 at 59.9997");
			Assert::AreEqual(1, RefreshKey::Find(bandKeys, 59.9997, true), L"general picks 60 at 59.9997");
			// An exact key wins over a band.
			const std::vector<double> mixed = { 59.0, 59.94, 60.0 };
			Assert::AreEqual(1, RefreshKey::Find(mixed, R5994, true), L"exact 59.94 beats the 59 band");
			// An unmigrated integer band key still serves the 1000/1001 rate when nothing is exact.
			Assert::AreEqual(0, RefreshKey::Find(std::vector<double>{ 59.0 }, R5994, true), L"band key serves 59.94");
		}
		TEST_METHOD(ApplyMigratesKeyedSectionsOnly)
		{
			std::map<std::string, std::map<std::string, std::string>> sections;
			sections["ppm_correction"] = { { "23", "0" }, { "59", "5" }, { "60", "5" }, { "50", "3" } };
			sections["display_refresh_rate_override"] = { { "23", "1" }, { "119", "2" }, { "24", "3" } };
			sections["general"] = { { "59", "x" } };
			const size_t changed = RefreshKey::Apply(sections);
			Assert::AreEqual(size_t{ 4 }, changed, L"keys rewritten");
			const std::map<std::string, std::string> ppm = { { "23.976", "0" }, { "59.94", "5" }, { "60", "5" }, { "50", "3" } };
			Assert::IsTrue(ppm == sections["ppm_correction"], L"ppm_correction migrated");
			const std::map<std::string, std::string> overrides = { { "23.976", "1" }, { "119.88", "2" }, { "24", "3" } };
			Assert::IsTrue(overrides == sections["display_refresh_rate_override"], L"display_refresh_rate_override migrated");
			const std::map<std::string, std::string> general = { { "59", "x" } };
			Assert::IsTrue(general == sections["general"], L"other sections untouched");
			Assert::IsTrue(RefreshKey::IsKeyedSection("ppm_correction") && !RefreshKey::IsKeyedSection("general"), L"IsKeyedSection");
		}
		TEST_METHOD(ApplyKeepsTheExplicitEntry)
		{
			std::map<std::string, std::map<std::string, std::string>> sections;
			sections["ppm_correction"] = { { "59", "5" }, { "59.94", "9" } };
			Assert::AreEqual(size_t{ 1 }, RefreshKey::Apply(sections), L"one key dropped");
			const std::map<std::string, std::string> expected = { { "59.94", "9" } };
			Assert::IsTrue(expected == sections["ppm_correction"], L"59 dropped, 59.94 value kept");
			const auto plan = RefreshKey::PlanSection({ "59", "59.94", "60" });
			Assert::AreEqual(size_t{ 1 }, plan.size(), L"plan has one rename");
			Assert::IsTrue(plan[0].from == "59" && plan[0].to == "59.94" && plan[0].drop, L"plan drops 59");
		}
	};

	namespace
	{
		struct TempConfigFile
		{
			std::string path;
			TempConfigFile()
			{
				char directory[MAX_PATH]{}, file[MAX_PATH]{};
				Assert::IsTrue(GetTempPathA(MAX_PATH, directory) != 0);
				Assert::IsTrue(GetTempFileNameA(directory, "vpg", 0, file) != 0);
				path = file;
			}
			~TempConfigFile() { DeleteFileA(path.c_str()); }
			void Write(const char* text) { std::ofstream(path, std::ios::trunc) << text; }
		};
		// The process flag is global: every test that changes it ends with it back on.
		struct ProcessFlagRestore
		{
			~ProcessFlagRestore() { GeneralRate::SetProcessEnabled(true); }
		};
	}

	TEST_CLASS(PPMCorrectionEndToEnd)
	{
	public:
		TEST_METHOD(SavedBandKeysServeTheExactRates)
		{
			TempConfigFile file;
			file.Write("[ppm_correction]\n23=0\n24=1\n59=5\n60=7\n50=3\n");
			ConfigFile config;
			Assert::IsTrue(config.Load(file.path), L"config loads");
			PPMCorrectionLoader loader;
			Assert::IsTrue(loader.LoadCorrections(config), L"corrections load");
			ProcessFlagRestore restore;
			for (bool general : { true, false })
			{
				GeneralRate::SetProcessEnabled(general);
				const std::wstring suffix = general ? L" (general)" : L" (legacy)";
				Assert::AreEqual(0, loader.GetPPMCorrection(R23976), (L"23.976" + suffix).c_str());
				Assert::AreEqual(1, loader.GetPPMCorrection(24.0), (L"24" + suffix).c_str());
				Assert::AreEqual(5, loader.GetPPMCorrection(R5994), (L"59.94" + suffix).c_str());
				Assert::AreEqual(7, loader.GetPPMCorrection(60.0), (L"60" + suffix).c_str());
				Assert::AreEqual(3, loader.GetPPMCorrection(50.0), (L"50" + suffix).c_str());
				Assert::AreEqual(0, loader.GetPPMCorrection(R2997), (L"29.97 has no key" + suffix).c_str());
				Assert::IsFalse(loader.IsAutomatic(60.0), (L"60 not automatic" + suffix).c_str());
			}
		}
		TEST_METHOD(DirectShowPpmFormAppliesToEveryRate)
		{
			TempConfigFile file;
			file.Write("[directshow.ppm]\nppm: 4\n");
			ConfigFile config;
			Assert::IsTrue(config.Load(file.path), L"config loads");
			PPMCorrectionLoader loader;
			Assert::IsTrue(loader.LoadCorrections(config), L"corrections load");
			ProcessFlagRestore restore;
			for (bool general : { true, false })
			{
				GeneralRate::SetProcessEnabled(general);
				for (double r : { R23976, R5994, 120.0, 143.856 })
					Assert::AreEqual(4, loader.GetPPMCorrection(r), W(general ? L"ppm 4 (general) at" : L"ppm 4 (legacy) at", r).c_str());
			}
		}
	};
}
