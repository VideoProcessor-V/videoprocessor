// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// SID train r2, N2 (doc 78 section 5, owner decision 2026-11-07): the native-evidence CLEAR consults the consensus. The incident of the
// 6.3 h run was ONE informative native-looking frame that cleared the declared lower reading two seconds after the re-base engaged.
// With source_id_consensus On and the chain's CONFIDENT evidence contradicting native, the Monitor sets
// Tracker::SetChainContradictsNative(true): such a frame is left out (counted in ChainHeld()), it does not end the window or the
// declaration. Without the flag (Off, Shadow, chain UNSURE or agreeing with native) the tracker is byte-identical to before: one genuine
// native frame still reverts at once (the pinning test of SID-B0, SourceResolutionAbsenceHoldTests, is unchanged). The tracker is fed
// hand-made FrameReadings (the technique of SourceResolutionAbsenceHoldTests), so the cases are exact and need no footage.
namespace VideoProcessorTest
{
	namespace
	{
		namespace ChainClear
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
			// One genuine native-looking frame: informative, not mixed, detail above the rate at -0.6 decades, good contrast.
			SourceResolution::FrameReading Native(int i) { return Frame(i, 1.0, 8, 8, 0.40, 0.25, -3.0, -3.6); }

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
		}
	}

	TEST_CLASS(SourceResolutionChainClearTests)
	{
	public:
		TEST_METHOD(TheFlagIsOffByDefaultAndTheTrackerIsTheOriginalOne)
		{
			using namespace ChainClear;
			Run run;
			Assert::IsFalse(run.tracker.ChainContradictsNative());
			Assert::AreEqual(0ull, run.tracker.ChainHeld());
			run.DeclareFilm();
			Assert::IsTrue(run.Declared());
			run.Feed(Native(run.i));
			Assert::IsTrue(run.tracker.Ratio() >= 1.0, L"no flag: one genuine native frame is native evidence at once (today's rule)");
			Assert::AreEqual(0ull, run.tracker.ChainHeld());
		}

		TEST_METHOD(AGenuineNativeFrameThatConfidentChainEvidenceContradictsDoesNotClearTheDeclaration)
		{
			using namespace ChainClear;
			Run run;
			run.DeclareFilm();
			Assert::IsTrue(run.Declared());
			run.tracker.SetChainContradictsNative(true);
			run.Feed(Native(run.i));
			Assert::IsTrue(run.Declared(), L"the contradicted native-looking frame does not clear the declaration");
			Assert::AreEqual(1ull, run.tracker.ChainHeld(), L"and it is counted, visibly");
			for (int n = 0; n < 40; ++n)
				run.Feed(Native(run.i));
			Assert::IsTrue(run.Declared(), L"however many such frames come while the chain stays confident");
			Assert::AreEqual(41ull, run.tracker.ChainHeld());
		}

		TEST_METHOD(AGenuineNativeReadingStillRevertsWhenTheChainAgreesOrIsUnsure)
		{
			using namespace ChainClear;
			Run run;
			run.DeclareFilm();
			run.tracker.SetChainContradictsNative(true);
			run.Feed(Native(run.i));
			Assert::IsTrue(run.Declared());
			// The chain no longer contradicts native (it read native, or it is UNSURE): the flag is lifted and the next native frame is
			// native evidence at once, as before.
			run.tracker.SetChainContradictsNative(false);
			run.Feed(Native(run.i));
			Assert::IsTrue(run.tracker.Ratio() >= 1.0, L"a genuine native reading the chain does not contradict reverts at once");
		}

		TEST_METHOD(TheGuardOnlyLeavesNativeLookingFramesOutTheFilmGoesOnAsBefore)
		{
			using namespace ChainClear;
			Run held, plain;
			held.DeclareFilm();
			plain.DeclareFilm();
			held.tracker.SetChainContradictsNative(true);
			for (int n = 0; n < 20; ++n)
			{
				held.Feed(Film(held.i));
				plain.Feed(Film(plain.i));
			}
			Assert::AreEqual(0ull, held.tracker.ChainHeld(), L"lower-reading frames are untouched by the flag");
			Assert::AreEqual(plain.tracker.Ratio(), held.tracker.Ratio(), 0.0);
			Assert::AreEqual(plain.tracker.Candidate(), held.tracker.Candidate(), 0.0);
			Assert::IsTrue(held.tracker.Shots() == plain.tracker.Shots() && held.tracker.EvidenceSeconds() == plain.tracker.EvidenceSeconds());
		}

		TEST_METHOD(ADiscontinuityStillClearsWhateverTheChainSays)
		{
			using namespace ChainClear;
			Run run;
			run.DeclareFilm();
			run.tracker.SetChainContradictsNative(true);
			Assert::IsTrue(run.Declared());
			run.tracker.Discontinuity("test");
			Assert::IsTrue(run.tracker.Ratio() >= 1.0, L"a resync / format change is not a frame: the chain's evidence belonged to the old source");
			Run gap;
			gap.DeclareFilm();
			gap.tracker.SetChainContradictsNative(true);
			gap.t += 3.0;   // a gap over maximumGapSeconds (2 s)
			gap.Feed(Film(gap.i));
			Assert::IsTrue(gap.tracker.Ratio() >= 1.0, L"a gap with no frames is a discontinuity");
		}

		TEST_METHOD(NativeLookingFramesWhileTheWindowGathersAreNotEvidenceEitherWhenTheChainContradicts)
		{
			using namespace ChainClear;
			Run held, plain;
			held.tracker.SetChainContradictsNative(true);
			// 6 s of film (not yet declared), then 6 s of native-looking frames, then 6 s of film again: the chain-held run must not have
			// lost the first 6 s to a native clear, so it declares sooner than the original, which started again after the native frames.
			for (int n = 0; n < 12; ++n) { held.Feed(Film(held.i)); plain.Feed(Film(plain.i)); }
			for (int n = 0; n < 12; ++n) { held.Feed(Native(held.i)); plain.Feed(Native(plain.i)); }
			for (int n = 0; n < 12; ++n) { held.Feed(Film(held.i)); plain.Feed(Film(plain.i)); }
			Assert::IsTrue(held.Declared(), L"12 s + 6 s of lower readings with the native frames left out reach the standard");
			Assert::IsFalse(plain.Declared(), L"the original ended the window at the native frames and has only 6 s again");
			Assert::AreEqual(12ull, held.tracker.ChainHeld());
		}
	};
}
