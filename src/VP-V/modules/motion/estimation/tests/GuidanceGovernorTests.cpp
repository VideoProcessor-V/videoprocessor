// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K4-4: the guidance governor: trip on > 10 % escalation or an unconfirmed cut skip, cool-down in measured seconds
// (24 / 50 / 60 / VFR), automatic recovery, the switches it turns off. CPU only.
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceGovernor.h>

#include <cmath>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(GuidanceGovernorTests)
	{
	public:
		static CameraLink::GuidedPairResult Pair(uint32_t planned, uint32_t escalated, double seconds)
		{
			CameraLink::GuidedPairResult r;
			r.guided = true;
			r.plannedNarrow = planned;
			r.escalated = escalated;
			r.pairSeconds = seconds;
			return r;
		}

		TEST_METHOD(TripsAboveTenPercentAndNotAtOrBelow)
		{
			CameraLink::GuidanceGovernor g;
			Assert::IsFalse(g.OnPair(Pair(1000, 100, 1.0 / 24)), L"exactly 10 % does not trip");
			Assert::IsFalse(g.Tripped());
			Assert::IsTrue(g.OnPair(Pair(1000, 101, 1.0 / 24)));
			Assert::IsTrue(g.Tripped());
			Assert::AreEqual(uint32_t(1), g.Trips());
			Assert::IsFalse(g.StatusLine().empty());
			Assert::AreEqual(std::string("camera guidance: escalations high, full search"), g.StatusLine());
			// A handful of blocks is noise, not a trip.
			CameraLink::GuidanceGovernor h;
			Assert::IsFalse(h.OnPair(Pair(8, 8, 1.0 / 24)));
		}

		TEST_METHOD(CooldownIsMeasuredSecondsAtEveryRate)
		{
			const double rates[] = { 24.0, 50.0, 60.0, 23.976 };
			for (double hz : rates)
			{
				CameraLink::GuidanceGovernor g;
				const double dt = 1.0 / hz;
				Assert::IsTrue(g.OnPair(Pair(1000, 500, dt)));
				int frames = 0;
				while (g.Tripped() && frames < 1000)
				{
					g.OnPair(Pair(0, 0, dt));
					++frames;
				}
				// 1 s of measured time: ceil(hz) frames (floating tolerance of one).
				Assert::IsTrue(std::abs(frames - static_cast<int>(std::ceil(hz))) <= 1, L"cool-down frames follow the measured rate");
				Assert::AreEqual(uint32_t(1), g.Recoveries(), L"automatic recovery");
				Assert::IsFalse(g.Tripped());
				Assert::IsTrue(g.StatusLine().empty());
			}
		}

		TEST_METHOD(VariableFrameRateCooldownFollowsTheSumOfDurations)
		{
			CameraLink::GuidanceGovernor g;
			Assert::IsTrue(g.OnPair(Pair(1000, 500, 0.02)));
			const double dts[] = { 0.1, 0.4, 0.05, 0.3, 0.2, 0.01 };   // sums to 1.06 s
			int n = 0;
			for (double d : dts)
			{
				Assert::IsTrue(g.Tripped(), L"still cooling");
				g.OnPair(Pair(0, 0, d));
				++n;
				if (!g.Tripped())
					break;
			}
			Assert::AreEqual(5, n, L"released only once 1 s of measured time has passed (1.05 s)");
			Assert::IsFalse(g.Tripped());
		}

		TEST_METHOD(UnconfirmedCutSkipTripsConfirmedDoesNot)
		{
			CameraLink::GuidanceGovernor g;
			CameraLink::GuidedPairResult r;
			r.guided = true;
			r.matcherSkipped = true;
			r.skipUnconfirmed = false;
			r.pairSeconds = 1.0 / 24;
			Assert::IsFalse(g.OnPair(r));
			r.skipUnconfirmed = true;
			Assert::IsTrue(g.OnPair(r));
		}

		TEST_METHOD(TrippedTurnsNarrowAndE1OffButKeepsGlobalCandidateAndCutSkip)
		{
			CameraModel::MeasurementSwitches sw;
			sw.modelGlobal = sw.guidedCandidate = sw.narrowWindow = sw.e1Skip = sw.cutSkip = sw.staticGate = true;
			CameraLink::GuidanceGovernor g;
			CameraModel::MeasurementSwitches a = g.Apply(sw);
			Assert::IsTrue(a.narrowWindow && a.e1Skip);
			g.OnPair(Pair(1000, 900, 1.0 / 24));
			a = g.Apply(sw);
			Assert::IsFalse(a.narrowWindow || a.e1Skip);
			Assert::IsTrue(a.modelGlobal && a.guidedCandidate && a.cutSkip && a.staticGate);
			Assert::IsTrue(a.Any());
			// Only narrow on: tripped -> nothing left.
			CameraModel::MeasurementSwitches only;
			only.narrowWindow = true;
			Assert::IsFalse(g.Apply(only).Any());
		}

		TEST_METHOD(ARetripAfterRecoveryIsCountedAgain)
		{
			CameraLink::GuidanceGovernor g;
			g.OnPair(Pair(1000, 500, 0.5));
			g.OnPair(Pair(0, 0, 0.6));
			g.OnPair(Pair(0, 0, 0.6));
			Assert::IsFalse(g.Tripped());
			Assert::IsTrue(g.OnPair(Pair(1000, 500, 0.5)));
			Assert::AreEqual(uint32_t(2), g.Trips());
		}
		TEST_METHOD(ElapseAdvancesTheCooldownWithoutJudgingAPair)
		{
			CameraLink::GuidanceGovernor g;
			Assert::IsTrue(g.OnPair(Pair(1000, 500, 0.02)));
			g.Elapse(0.5);
			Assert::IsTrue(g.Tripped());
			Assert::AreEqual(0.5, g.CooldownRemaining(), 1e-12);
			g.Elapse(0.0);
			Assert::AreEqual(0.5, g.CooldownRemaining(), 1e-12);
			g.Elapse(0.6);
			Assert::IsFalse(g.Tripped());
			Assert::AreEqual(uint32_t(1), g.Recoveries());
			g.Elapse(1.0);
			Assert::AreEqual(uint32_t(1), g.Recoveries(), L"not tripped: nothing to advance");
		}
	};
}
