// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K1-1: the pair duration. CPU only.
#include <VP-V/modules/motion/estimation/global_model/link/PairTiming.h>
#include <VP-V/core/FrameAnalysisStore.h>

#include <utility>

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

		// C7b: a missed read of the previous frame's cadence (not written yet / evicted) holds the latched film timing; only an Ok read
		// decides; a refused read (stale epoch, invalid group, store off) and no read unlock. The measured rate is never touched.
		TEST_METHOD(AMissedCadenceReadHoldsTheLatchAndOnlyAnOkReadDecides)
		{
			const double film = 24000.0 / 1001.0;
			CameraLink::TimingFacts latched;
			latched.filmHz = film;
			latched.cadenceLocked = true;
			latched.measuredSourceHz = 30.0;
			auto apply = [&latched](CameraLink::CadenceRead read, double filmHz, bool haveLatched) {
				CameraLink::TimingFacts t;
				t.measuredSourceHz = 29.97;
				CameraLink::ApplyCadenceRead(t, read, filmHz, haveLatched, latched);
				Assert::AreEqual(29.97, t.measuredSourceHz, L"the measured rate is the host's");
				return t;
			};
			CameraLink::TimingFacts t = apply(CameraLink::CadenceRead::NotYet, 0.0, true);
			Assert::IsTrue(t.cadenceLocked, L"a missed read holds the lock");
			Assert::AreEqual(film, t.filmHz);
			t = apply(CameraLink::CadenceRead::NotYet, 0.0, false);
			Assert::IsFalse(t.cadenceLocked, L"nothing latched: nothing to hold");
			t = apply(CameraLink::CadenceRead::Ok, 0.0, true);
			Assert::IsFalse(t.cadenceLocked, L"an Ok read without a film rate unlocks");
			Assert::AreEqual(0.0, t.filmHz);
			t = apply(CameraLink::CadenceRead::Ok, 25.0, true);
			Assert::IsTrue(t.cadenceLocked, L"an Ok read decides the new film rate");
			Assert::AreEqual(25.0, t.filmHz);
			t = apply(CameraLink::CadenceRead::Refused, 0.0, true);
			Assert::IsFalse(t.cadenceLocked, L"a refused read (stale epoch / invalid group / store off) unlocks");
			t = apply(CameraLink::CadenceRead::None, 0.0, true);
			Assert::IsFalse(t.cadenceLocked, L"no read (the first frame) unlocks");
			latched.cadenceLocked = false;
			latched.filmHz = 0.0;
			t = apply(CameraLink::CadenceRead::NotYet, 0.0, true);
			Assert::IsFalse(t.cadenceLocked, L"an unlocked latch is held unlocked");
		}

		// Camfix R8 (SHOULD): through a real frame-analysis store, a seq-1 cadence record that is missing (never begun) or late (begun,
		// cadence not published yet) keeps the latched film lock; every other outcome unlocks. CadenceReadOf is the mapping
		// CameraExtension's UpdateTiming uses.
		TEST_METHOD(AMissingSeqMinusOneCadenceRecordKeepsTheFilmLock)
		{
			const double film = 24000.0 / 1001.0;
			FrameAnalysis::Store store;
			auto id = [](uint64_t seq) { FrameAnalysis::Identity i; i.seq = seq; return i; };
			auto readAt = [&store](uint64_t seq) {
				FrameAnalysis::Record rec;
				const FrameAnalysis::ReadStatus rs = store.Read(seq - 1, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kCadence, rec);
				return std::make_pair(CameraLink::CadenceReadOf(rs), rec.cadence.filmHz);
			};
			// Frame 5 publishes a film lock; frame 6 reads it (Ok) and latches.
			store.Begin(id(5), FrameAnalysis::kCadence);
			FrameAnalysis::Cadence c;
			c.filmHz = film;
			Assert::IsTrue(store.PublishCadence(5, c));
			std::pair<CameraLink::CadenceRead, double> r = readAt(6);
			Assert::IsTrue(r.first == CameraLink::CadenceRead::Ok, L"a published record reads Ok");
			CameraLink::TimingFacts latched;
			CameraLink::ApplyCadenceRead(latched, r.first, r.second, false, CameraLink::TimingFacts());
			Assert::IsTrue(latched.cadenceLocked, L"the Ok read latches the film lock");
			// Frame 8 reads seq 7, never begun: Missing -> NotYet -> the lock is held.
			r = readAt(8);
			Assert::IsTrue(r.first == CameraLink::CadenceRead::NotYet, L"a missing seq-1 record is NotYet");
			CameraLink::TimingFacts t;
			CameraLink::ApplyCadenceRead(t, r.first, r.second, true, latched);
			Assert::IsTrue(t.cadenceLocked, L"a missing seq-1 record keeps cadenceLocked");
			Assert::AreEqual(film, t.filmHz);
			// Frame 10 reads seq 9, begun but its cadence not published: Late -> NotYet -> held.
			store.Begin(id(9), FrameAnalysis::kCadence);
			r = readAt(10);
			Assert::IsTrue(r.first == CameraLink::CadenceRead::NotYet, L"a late seq-1 record is NotYet");
			CameraLink::TimingFacts t2;
			CameraLink::ApplyCadenceRead(t2, r.first, r.second, true, latched);
			Assert::IsTrue(t2.cadenceLocked, L"a late seq-1 record keeps cadenceLocked");
			// Every other outcome unlocks (a stale epoch after a discontinuity, an invalid group, the store off).
			Assert::IsTrue(CameraLink::CadenceReadOf(FrameAnalysis::ReadStatus::StaleEpoch) == CameraLink::CadenceRead::Refused);
			Assert::IsTrue(CameraLink::CadenceReadOf(FrameAnalysis::ReadStatus::GroupInvalid) == CameraLink::CadenceRead::Refused);
			Assert::IsTrue(CameraLink::CadenceReadOf(FrameAnalysis::ReadStatus::Disabled) == CameraLink::CadenceRead::Refused);
		}
	};
}
