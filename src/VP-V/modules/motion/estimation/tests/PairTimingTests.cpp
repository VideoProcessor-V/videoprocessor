// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K1-1: the pair duration. CPU only.
#include <VP-V/modules/motion/estimation/global_model/link/PairTiming.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(PairTimingTests)
	{
	public:
		TEST_METHOD(FilmLockUsesTheFilmRateNotTheMeasuredDisplayCadence)
		{
			CameraLink::TimingFacts f;
			f.cadenceLocked = true;
			f.filmHz = 24000.0 / 1001.0;
			f.measuredSourceHz = 30.0;   // 3:2 pulled-down stream delivers 30 frames per second
			f.nominalHz = 30.0;
			CameraLink::PairTiming t;
			t.Latch(f);
			Assert::IsTrue(t.Basis() == CameraLink::TimingBasis::FilmLock);
			Assert::AreEqual(1.0 / f.filmHz, t.Seconds(1), 1e-15);
			Assert::AreEqual(2.0 / f.filmHz, t.Seconds(2), 1e-15, L"two unique steps");
		}

		TEST_METHOD(TwoTwoCadenceUsesTheMeasuredSourceRate)
		{
			CameraLink::TimingFacts f;
			f.measuredSourceHz = 25.0;   // 2:2 on a 50 Hz stream: 25 unique frames per second
			f.nominalHz = 50.0;
			CameraLink::PairTiming t;
			t.Latch(f);
			Assert::IsTrue(t.Basis() == CameraLink::TimingBasis::Measured);
			Assert::AreEqual(0.04, t.Seconds(1), 1e-15);
		}

		TEST_METHOD(VariableFrameRateTimestampsWin)
		{
			CameraLink::TimingFacts f;
			f.measuredSourceHz = 24.0;
			CameraLink::PairTiming t;
			t.Latch(f);
			Assert::AreEqual(0.0417, t.Seconds(1, true, 10.0, 10.0417), 1e-12);
			// Unusable timestamps fall back to the latched period.
			Assert::AreEqual(1.0 / 24.0, t.Seconds(1, true, 10.0, 10.0), 1e-15);
			Assert::AreEqual(1.0 / 24.0, t.Seconds(1, true, 10.0, 9.0), 1e-15);
		}

		TEST_METHOD(RelatchChangesThePeriodAndKeepsNoOtherState)
		{
			CameraLink::PairTiming t;
			CameraLink::TimingFacts a;
			a.measuredSourceHz = 24.0;
			t.Latch(a);
			Assert::AreEqual(1.0 / 24.0, t.Period(), 1e-15);
			CameraLink::TimingFacts b;
			b.measuredSourceHz = 60.0;
			t.Latch(b);
			Assert::AreEqual(1.0 / 60.0, t.Period(), 1e-15);
			Assert::AreEqual(2u, t.Latches());
			CameraLink::TimingFacts c;
			c.cadenceLocked = true;
			c.filmHz = 25.0;
			c.measuredSourceHz = 60.0;
			t.Latch(c);
			Assert::IsTrue(t.Basis() == CameraLink::TimingBasis::FilmLock);
			Assert::AreEqual(0.04, t.Period(), 1e-15);
		}

		TEST_METHOD(NothingKnownIsZeroAndNominalIsAVisibleBasis)
		{
			CameraLink::PairTiming t;
			Assert::AreEqual(0.0, t.Seconds(1), L"never latched");
			t.Latch(CameraLink::TimingFacts());
			Assert::IsTrue(t.Basis() == CameraLink::TimingBasis::Unknown);
			Assert::AreEqual(0.0, t.Seconds(1), L"unknown -> 0 (NoInput)");
			CameraLink::TimingFacts n;
			n.nominalHz = 30.0;
			t.Latch(n);
			Assert::IsTrue(t.Basis() == CameraLink::TimingBasis::Nominal);
			Assert::AreEqual(1.0 / 30.0, t.Seconds(1), 1e-15);
			Assert::AreEqual(0.0, t.Seconds(0), L"no steps");
			// A locked cadence without a film rate does not lock.
			CameraLink::TimingFacts l;
			l.cadenceLocked = true;
			l.measuredSourceHz = 20.0;
			t.Latch(l);
			Assert::IsTrue(t.Basis() == CameraLink::TimingBasis::Measured);
		}
	};
}
