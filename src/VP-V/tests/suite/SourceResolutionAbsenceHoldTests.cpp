// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// SID-B0 (doc 78 section 5): PINS the behaviour on integ, it changes none. Once a lower reading is declared (the thing the monitor
// re-bases to), frames that carry no evidence - dark, flat, credits-like (no informative tile), no tiles at all, starved of detail
// by a dark / low-contrast level or by a blur - HOLD the declaration; only a genuine native reading (informative, not mixed, not
// starved, reading native) clears it, and a discontinuity still clears it at once. The tracker is fed hand-made FrameReadings
// (the same technique as TrackerResumeAfterNativeEvidence), so the cases are exact and need no footage.
namespace VideoProcessorTest
{
	namespace
	{
		namespace AbsenceHold
		{
			constexpr double kDt = 0.5;          // the monitor's analysis cadence
			constexpr double kDeclared = 0.5;    // the lower reading the cases declare

			// 129 bins (a 256-point tile): the bands DetailAbove reads for a 0.5 rate are bins 36..57 (below) and 74..95 (above).
			std::vector<double> Spectrum(double belowLog10, double aboveLog10)
			{
				std::vector<double> power(129, std::pow(10.0, belowLog10));
				for (int k = 74; k <= 95; ++k) power[static_cast<size_t>(k)] = std::pow(10.0, aboveLog10);
				return power;
			}

			SourceResolution::FrameReading Frame(int i, double ratio, int tiles, int informativeTiles, double mean, double contrast,
				double belowLog10, double aboveLog10)
			{
				SourceResolution::FrameReading f;
				f.tiles = tiles;
				f.informativeTiles = informativeTiles;
				f.ratio = ratio;
				f.mean = mean + 0.001 * (i % 7);
				f.signature = f.mean * 10.0 + i * 1e-6;   // never a repeat of the last frame
				f.contrast = contrast;
				if (informativeTiles > 0)
				{
					f.verticalPower = Spectrum(belowLog10, aboveLog10);
					f.horizontalPower = f.verticalPower;
				}
				return f;
			}

			// The film: an informative frame reading the lower rate, its detail above the rate at -1.2 decades.
			SourceResolution::FrameReading Film(int i) { return Frame(i, kDeclared, 8, 8, 0.40, 0.20, -3.0, -4.2); }

			struct Run
			{
				SourceResolution::Tracker tracker;
				int i = 0;
				double t = 0.0;
				void Feed(const SourceResolution::FrameReading& f)
				{
					tracker.Update(f, t);
					t += kDt;
					++i;
				}
				// 16 s of film: past the 10 s standard, so the reading is declared.
				void DeclareFilm()
				{
					for (int n = 0; n < 32; ++n) Feed(Film(i));
				}
				bool Declared() const { return tracker.Ratio() < 1.0 && std::fabs(tracker.Ratio() - kDeclared) < 0.02; }
			};

			void AssertHolds(Run& run, int frames, const std::function<SourceResolution::FrameReading(int)>& make, const wchar_t* what)
			{
				for (int n = 0; n < frames; ++n)
				{
					run.Feed(make(run.i));
					Assert::IsTrue(run.Declared(), what);
				}
			}
		}
	}

	TEST_CLASS(SourceResolutionAbsenceHoldTests)
	{
	public:
		TEST_METHOD(TheFilmDeclaresItsLowerReading)
		{
			using namespace AbsenceHold;
			Run run;
			run.DeclareFilm();
			Assert::IsTrue(run.Declared(), L"16 s of lower readings declare 0.5");
		}

		TEST_METHOD(FramesWithNoEvidenceHoldTheDeclaration)
		{
			using namespace AbsenceHold;
			Run run;
			run.DeclareFilm();
			Assert::IsTrue(run.Declared());
			// Dark / flat / credits-like: tiles exist but none is informative (the frame reads "native" only by default).
			AssertHolds(run, 40, [](int i) { return Frame(i, 1.0, 8, 0, 0.02, 0.0, 0.0, 0.0); }, L"non-informative frames hold");
			// No tiles at all (no reading: nothing was copied or nothing could be placed).
			AssertHolds(run, 40, [](int i) { return Frame(i, 1.0, 0, 0, 0.40, 0.0, 0.0, 0.0); }, L"frames with no tiles hold");
			Assert::AreEqual(0ull, run.tracker.Starved(), L"neither kind reaches the starved gates (they return before)");
		}

		TEST_METHOD(StarvedNativeReadingsHoldTheDeclaration)
		{
			using namespace AbsenceHold;
			Run run;
			run.DeclareFilm();
			// Gate (a): a dark / low-contrast frame whose detail above the rate is within 0.5 decades of the rounding floor
			// (quantisationFloor -7.1 -> above-band level -8.5 is below -6.6).
			AssertHolds(run, 40, [](int i) { return Frame(i, 1.0, 8, 8, 0.05, 0.01, -8.0, -8.5); }, L"dark / low-contrast starved frames hold");
			Assert::IsTrue(run.tracker.FloorStarved() >= 40ull, L"counted by the level gate");
			// Gate (b): a blurred frame - above-band detail 1.6 decades under the band below it, against the window's reference of
			// -1.2 decades (+0.07 margin) - at a normal contrast.
			const unsigned long long before = run.tracker.Starved();
			AssertHolds(run, 40, [](int i) { return Frame(i, 1.0, 8, 8, 0.40, 0.25, -3.0, -4.6); }, L"blurred starved frames hold");
			Assert::IsTrue(run.tracker.Starved() >= before + 40ull, L"counted by the reference gate");
			// The film goes on: still declared.
			run.Feed(Film(run.i));
			Assert::IsTrue(run.Declared());
		}

		TEST_METHOD(AGenuineNativeReadingStillRevertsAtOnce)
		{
			using namespace AbsenceHold;
			Run run;
			run.DeclareFilm();
			Assert::IsTrue(run.Declared());
			// Informative, not mixed, detail above the rate at -0.6 decades (well over the -1.2 reference + 0.07), good contrast.
			run.Feed(Frame(run.i, 1.0, 8, 8, 0.40, 0.25, -3.0, -3.6));
			Assert::IsTrue(run.tracker.Ratio() >= 1.0, L"one genuine native frame is native evidence (today's rule)");
		}

		TEST_METHOD(ADiscontinuityStillRevertsAtOnce)
		{
			using namespace AbsenceHold;
			{
				Run run;
				run.DeclareFilm();
				Assert::IsTrue(run.Declared());
				run.tracker.Discontinuity("test");
				Assert::IsTrue(run.tracker.Ratio() >= 1.0, L"Discontinuity() clears the declaration");
			}
			{
				Run run;
				run.DeclareFilm();
				Assert::IsTrue(run.Declared());
				run.t += 3.0;   // a gap over maximumGapSeconds (2 s)
				run.Feed(Film(run.i));
				Assert::IsTrue(run.tracker.Ratio() >= 1.0, L"a gap with no frames is a discontinuity");
			}
		}
	};
}
