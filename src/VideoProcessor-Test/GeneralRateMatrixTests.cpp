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

#include <algorithm>
#include <cmath>
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
}
