// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Q2: the motion family on the central Indicator. MotionValidity::State bound to an Indicator
// (deferred request-time invalidation, epoch moved at the seal by the motion-history reader,
// PairValidAt, settings[motion]), the vector-producer reader, VectorStoreKey and the Frame
// Analysis Store's RecordValid. The unbound State is checked against today's behaviour.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FrameAnalysisValidity.h>
#include <VP-V/core/Indicator.h>
#include <VP-V/modules/input/signal/ResyncDrive.h>
#include <VP-V/modules/motion/estimation/MotionResyncReaders.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/motion/estimation/VectorStore.h>

#include <memory>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using VpIndicator::Bit;

namespace Tests
{
	namespace MotionResyncQ2
	{
		using MVCause = MotionValidity::Cause;
		using RCause = VpResync::Cause;

		inline uint64_t U(uint64_t v) { return v; }

		// A renderer in miniature: an Indicator, a bound State and the two readers. Frames are
		// sealed and observed in order, like the render loop.
		struct Rig
		{
			std::shared_ptr<VpIndicator::Indicator> ind;
			MotionValidity::State state;
			VpMotionResync::Readers readers;
			VpMotionResync::Ids ids;

			Rig() : ind(std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), 64))
			{
				state.Bind(ind);
				ids = VpMotionResync::RegisterOn(*ind, state, readers);
			}
			void Frames(uint64_t first, uint64_t last)
			{
				for (uint64_t s = first; s <= last; ++s)
					ind->SealAndObserve(s);
			}
		};
	}

	using namespace MotionResyncQ2;

	TEST_CLASS(MotionValidityResyncPairTests)
	{
	public:
		TEST_METHOD(RegistrationGivesTwoReaders)
		{
			Rig rig;
			Assert::IsTrue(rig.ids.motionHistory >= 0);
			Assert::IsTrue(rig.ids.vectorProducer >= 0);
			Assert::IsTrue(rig.ids.motionHistory != rig.ids.vectorProducer);
			Assert::AreEqual(std::string("motion-history"), std::string(rig.ind->ReaderName(rig.ids.motionHistory)));
			Assert::AreEqual(std::string("vector-producer"), std::string(rig.ind->ReaderName(rig.ids.vectorProducer)));
			Assert::IsTrue(rig.state.Driven());
		}

		TEST_METHOD(RegistrationAfterFirstObserveIsRefused)
		{
			auto ind = std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), 64);
			ind->SealAndObserve(1);
			MotionValidity::State state;
			VpMotionResync::Readers readers;
			const VpMotionResync::Ids ids = VpMotionResync::RegisterOn(*ind, state, readers);
			Assert::AreEqual(-1, ids.motionHistory);
			Assert::AreEqual(-1, ids.vectorProducer);
			Assert::AreEqual(uint32_t(2), ind->RejectedReaders());
		}

		TEST_METHOD(PairValidAtMatchesEpochAndIsStricterForPreResetCopies)
		{
			Rig rig;
			rig.Frames(1, 3);
			const uint64_t e = rig.state.Epoch();
			// No event: the Indicator form and the epoch form agree.
			Assert::IsTrue(rig.state.PairValidAt(2, 3));
			Assert::IsTrue(rig.state.PairValid(2, 3, e, e));
			// Not adjacent / no source sequence: refused by both.
			Assert::IsFalse(rig.state.PairValidAt(1, 3));
			Assert::IsFalse(rig.state.PairValidAt(0, 1));
			Assert::IsFalse(rig.state.PairValidAt(3, 3));

			// Frames 4 and 5 are queued (unsealed); a FULL arrives for frame 5 on. The request
			// does not move the epoch, so the epoch form still accepts the pair (4, 5) that
			// spans the reset; the Indicator form refuses it.
			rig.ind->Advance(VpIndicator::kFull, RCause::LiveQueueReset, 5);
			Assert::AreEqual(U(e), U(rig.state.Invalidate(MVCause::QueueReset)));
			Assert::AreEqual(U(e), U(rig.state.Epoch()));
			Assert::IsTrue(rig.state.PairValid(4, 5, e, e));
			Assert::IsFalse(rig.state.PairValidAt(4, 5));
			// Pairs wholly before or wholly after the FULL stay valid.
			Assert::IsTrue(rig.state.PairValidAt(3, 4));
			Assert::IsTrue(rig.state.PairValidAt(5, 6));
			Assert::IsTrue(rig.state.PairValidAt(6, 7));
		}

		TEST_METHOD(UnboundStateIsToday)
		{
			MotionValidity::State s;
			const uint64_t k = MotionValidity::State::kInitialEpoch;
			Assert::IsFalse(s.Driven());
			Assert::AreEqual(U(k + 1), U(s.Invalidate(MVCause::Resync)));
			Assert::AreEqual(U(k + 1), U(s.Epoch()));
			Assert::AreEqual(U(1), U(s.Count(MVCause::Resync)));
			Assert::AreEqual(U(k + 1), U(s.StreamGeneration()));
			Assert::AreEqual(U(k + 1), U(s.StreamGenerationAt(99)));
			Assert::AreEqual(U(0), U(s.DeferredToSeal()));
			Assert::AreEqual(U(0), U(s.SettingsAdvances()));
			// A gap moves the epoch but not the stream generation.
			Assert::AreEqual(U(k + 2), U(s.Invalidate(MVCause::Gap)));
			Assert::AreEqual(U(k + 1), U(s.StreamGeneration()));
			// A settings change moves the epoch at once.
			Assert::AreEqual(U(k + 3), U(s.Invalidate(MVCause::SettingsChange)));
			// Pairs: the epoch form; PairValidAt is only adjacency.
			Assert::IsTrue(s.PairValid(5, 6, k + 3, k + 3));
			Assert::IsFalse(s.PairValid(5, 6, k + 2, k + 3));
			Assert::IsFalse(s.PairValid(5, 7, k + 3, k + 3));
			Assert::IsTrue(s.PairValidAt(5, 6));
			Assert::IsFalse(s.PairValidAt(5, 7));
			// Consume: one look sees the three moves, the release request of the resync.
			uint64_t seen = k;
			const MotionValidity::Pending p = s.Consume(seen);
			Assert::IsTrue(p.changed);
			Assert::IsTrue(p.release);
			Assert::AreEqual(U(3), U(p.invalidations));
			Assert::AreEqual(U(k + 3), U(seen));
			Assert::IsFalse(s.Consume(seen).changed);
		}

		TEST_METHOD(UnboundNoteKeptHoleStillInvalidatesAsGap)
		{
			MotionValidity::State s;
			const uint64_t k = MotionValidity::State::kInitialEpoch;
			Assert::IsTrue(s.NoteKept(5) == MotionValidity::Continuity::First);
			Assert::IsTrue(s.NoteKept(6) == MotionValidity::Continuity::Contiguous);
			Assert::IsTrue(s.NoteKept(6) == MotionValidity::Continuity::Repeat);
			Assert::IsTrue(s.NoteKept(8) == MotionValidity::Continuity::Break);
			Assert::AreEqual(U(k + 1), U(s.Epoch()));
			Assert::AreEqual(U(1), U(s.Count(MVCause::Gap)));
			Assert::AreEqual(U(1), U(s.Repeats()));
		}

		TEST_METHOD(UnbindRestoresTheImmediateEpochMove)
		{
			Rig rig;
			rig.Frames(1, 2);
			const uint64_t e = rig.state.Epoch();
			rig.state.Invalidate(MVCause::Resync);
			Assert::AreEqual(U(e), U(rig.state.Epoch()));
			Assert::AreEqual(U(1), U(rig.state.DeferredToSeal()));
			rig.state.Unbind();
			Assert::IsFalse(rig.state.Driven());
			Assert::AreEqual(U(e + 1), U(rig.state.Invalidate(MVCause::Resync)));
			Assert::AreEqual(U(1), U(rig.state.DeferredToSeal()));
		}
	};

	TEST_CLASS(MotionValidityResyncReaderTests)
	{
	public:
		TEST_METHOD(DeferredResyncMovesTheEpochAtTheSealOfTheCarryingFrame)
		{
			Rig rig;
			rig.Frames(1, 3);
			const uint64_t e = rig.state.Epoch();
			uint64_t seen = e;

			// The resync arm writes FULL for frame 4 and asks the State to invalidate.
			rig.ind->Advance(VpIndicator::kFull, RCause::RendererReset, 4);
			Assert::AreEqual(U(e), U(rig.state.Invalidate(MVCause::Resync)));
			Assert::AreEqual(U(1), U(rig.state.DeferredToSeal()));
			Assert::AreEqual(U(e), U(rig.state.Epoch()));
			Assert::AreEqual(U(0), U(rig.state.Count(MVCause::Resync)));
			Assert::IsFalse(rig.state.Consume(seen).changed);
			const unsigned actsBefore = rig.readers.HistoryActs();

			// The carrying frame is sealed: the reader fires once and the epoch moves once.
			rig.Frames(4, 4);
			Assert::AreEqual(actsBefore + 1u, rig.readers.HistoryActs());
			Assert::AreEqual(U(e + 1), U(rig.state.Epoch()));
			Assert::AreEqual(U(1), U(rig.state.Count(MVCause::Resync)));
			const VpMotionResync::HistoryPending pending = rig.readers.TakeHistory();
			Assert::IsTrue(pending.changed);
			Assert::IsTrue(pending.release);
			Assert::AreEqual(1u, pending.moves);
			Assert::IsTrue(pending.cause == MVCause::Resync);
			Assert::IsTrue((pending.counters & Bit(VpIndicator::kFull)) != 0);
			// A FULL moves the rebase axes too: the re-based-plane holders are forgotten.
			Assert::IsTrue(pending.forgetRebaseHolders);
			// Taken: cleared.
			Assert::IsFalse(rig.readers.PeekHistory().changed);

			// The State's own consumer sees the release request.
			const MotionValidity::Pending p = rig.state.Consume(seen);
			Assert::IsTrue(p.changed);
			Assert::IsTrue(p.release);

			// Later frames do not fire it again.
			rig.Frames(5, 8);
			Assert::AreEqual(actsBefore + 1u, rig.readers.HistoryActs());
			Assert::AreEqual(U(e + 1), U(rig.state.Epoch()));
		}

		TEST_METHOD(FullCausesMapAndOnlySourceChangesRelease)
		{
			struct Row { RCause cause; MVCause expect; bool release; };
			const Row rows[] = {
				{ RCause::RendererReset, MVCause::Resync, true },
				{ RCause::LiveQueueReset, MVCause::QueueReset, false },
				{ RCause::FileDiscontinuity, MVCause::Seek, false },
				{ RCause::SourceFormatChanged, MVCause::SourceFormatChange, true },
				{ RCause::InPlaceSourceChange, MVCause::SourceFormatChange, true },
			};
			for (const Row& row : rows)
			{
				Rig rig;
				rig.Frames(1, 2);
				uint64_t seen = rig.state.Epoch();
				rig.ind->Advance(VpIndicator::kFull, row.cause, 3);
				rig.Frames(3, 3);
				const VpMotionResync::HistoryPending pending = rig.readers.TakeHistory();
				Assert::IsTrue(pending.changed);
				Assert::IsTrue(pending.cause == row.expect);
				Assert::AreEqual(row.release, pending.release);
				const MotionValidity::Pending p = rig.state.Consume(seen);
				Assert::IsTrue(p.changed);
				Assert::AreEqual(row.release, p.release);
				Assert::IsTrue(p.cause == row.expect);
			}
		}

		TEST_METHOD(SettingsMotionAdvanceMovesOnlyThatCounter)
		{
			Rig rig;
			rig.Frames(1, 3);
			const uint64_t e = rig.state.Epoch();
			const VpIndicator::Stamp before = rig.ind->StampOf(3);

			Assert::AreEqual(U(e), U(rig.state.Invalidate(MVCause::SettingsChange)));
			Assert::AreEqual(U(1), U(rig.state.SettingsAdvances()));
			Assert::AreEqual(U(e), U(rig.state.Epoch()));
			Assert::AreEqual(U(0), U(rig.state.DeferredToSeal()));

			// Written for the first unsealed frame (4): frame 3 is untouched, frame 4 differs in
			// settings[motion] and nothing else.
			const VpIndicator::Stamp s3 = rig.ind->StampOf(3);
			const VpIndicator::Stamp s4 = rig.ind->StampOf(4);
			Assert::IsTrue(VpIndicator::Changed(before, s3) == 0);
			Assert::IsTrue(VpIndicator::Changed(s3, s4) == Bit(VpIndicator::kSettingsMotion));
			Assert::IsFalse(rig.state.PairValidAt(3, 4));
			Assert::IsTrue(rig.state.PairValidAt(2, 3));

			rig.Frames(4, 4);
			Assert::AreEqual(1u, rig.readers.HistoryActs());
			Assert::AreEqual(U(e + 1), U(rig.state.Epoch()));
			Assert::AreEqual(U(1), U(rig.state.Count(MVCause::SettingsChange)));
			Assert::AreEqual(U(0), U(rig.state.Count(MVCause::Resync)));
			const VpMotionResync::HistoryPending pending = rig.readers.TakeHistory();
			Assert::IsTrue(pending.cause == MVCause::SettingsChange);
			Assert::IsFalse(pending.release);
			Assert::IsFalse(pending.forgetRebaseHolders);
			Assert::IsTrue(pending.counters == Bit(VpIndicator::kSettingsMotion));
			Assert::IsTrue(rig.state.PairValidAt(4, 5));
		}

		TEST_METHOD(SettingsLikeCausesAdvanceAndGapStaysImmediate)
		{
			Rig rig;
			rig.Frames(1, 2);
			const uint64_t e = rig.state.Epoch();
			rig.state.Invalidate(MVCause::SettingsChange);
			rig.state.Invalidate(MVCause::MotionSourceChange);
			rig.state.Invalidate(MVCause::SmoothMotionMix);
			Assert::AreEqual(U(3), U(rig.state.SettingsAdvances()));
			Assert::AreEqual(U(e), U(rig.state.Epoch()));
			// The hole safety net is immediate.
			Assert::AreEqual(U(e + 1), U(rig.state.Invalidate(MVCause::Gap)));
			Assert::AreEqual(U(e + 1), U(rig.state.Invalidate(MVCause::Gap) - 1));
			Assert::AreEqual(U(3), U(rig.state.SettingsAdvances()));
		}

		TEST_METHOD(FullAndSettingsOnTheSameFrameFireEachReaderOnce)
		{
			Rig rig;
			rig.Frames(1, 3);
			const uint64_t e = rig.state.Epoch();
			rig.ind->Advance(VpIndicator::kFull, RCause::LiveQueueReset, 4);
			rig.ind->Advance(VpIndicator::kSettingsMotion, RCause::SettingsApply, 4);
			rig.Frames(4, 4);
			Assert::AreEqual(1u, rig.readers.HistoryActs());
			Assert::AreEqual(1u, rig.readers.ProducerActs());
			const VpMotionResync::HistoryPending pending = rig.readers.TakeHistory();
			Assert::AreEqual(2u, pending.moves);
			Assert::AreEqual(U(e + 2), U(rig.state.Epoch()));
			Assert::AreEqual(U(1), U(rig.state.Count(MVCause::QueueReset)));
			Assert::AreEqual(U(1), U(rig.state.Count(MVCause::SettingsChange)));
		}

		TEST_METHOD(ContinuityMovesTheEpochAsGapOrDriftDrop)
		{
			Rig rig;
			rig.Frames(1, 2);
			rig.ind->Advance(VpIndicator::kContinuity, RCause::MissedFrames, 3);
			rig.Frames(3, 3);
			Assert::AreEqual(U(1), U(rig.state.Count(MVCause::Gap)));
			Assert::IsTrue(rig.readers.TakeHistory().cause == MVCause::Gap);
			rig.ind->Advance(VpIndicator::kContinuity, RCause::DriftDrop, 4);
			rig.Frames(4, 4);
			Assert::AreEqual(U(1), U(rig.state.Count(MVCause::DriftDrop)));
			Assert::IsTrue(rig.readers.TakeHistory().cause == MVCause::DriftDrop);
			Assert::AreEqual(2u, rig.readers.HistoryActs());
		}

		TEST_METHOD(RebaseAxisAloneForgetsOnlyRebasedHolders)
		{
			Rig rig;
			rig.Frames(1, 2);
			const uint64_t e = rig.state.Epoch();
			rig.ind->Advance(VpIndicator::kRebaseH, RCause::RebaseDecision, 3);
			rig.Frames(3, 3);
			const VpMotionResync::HistoryPending pending = rig.readers.TakeHistory();
			Assert::IsTrue(pending.forgetRebaseHolders);
			Assert::IsFalse(pending.changed);
			Assert::AreEqual(U(e), U(rig.state.Epoch()));
			// The producer does not read the rebase axes.
			Assert::AreEqual(0u, rig.readers.ProducerActs());
		}

		TEST_METHOD(VectorProducerDropsTheInFlightJobKeyedOnTheOldFullKey)
		{
			Rig rig;
			rig.Frames(1, 3);
			const uint64_t oldKey = rig.readers.EngageGeneration();
			Assert::AreEqual(U(oldKey), U(rig.state.StreamGenerationAt(3)));
			Assert::AreEqual(U(oldKey), U(VpResync::Drive::FullKey(*rig.ind, 3)));
			Assert::IsFalse(rig.readers.TakeProducerNote().drop);

			rig.ind->Advance(VpIndicator::kFull, RCause::LiveQueueReset, 4);
			rig.Frames(4, 4);
			Assert::AreEqual(1u, rig.readers.ProducerActs());
			const VpMotionResync::ProducerNote note = rig.readers.TakeProducerNote();
			Assert::IsTrue(note.drop);
			Assert::AreEqual(U(oldKey), U(note.droppedInFlightGeneration));
			Assert::IsTrue(note.engageGeneration != oldKey);
			Assert::AreEqual(U(VpResync::Drive::FullKey(*rig.ind, 4)), U(note.engageGeneration));
			Assert::AreEqual(U(note.engageGeneration), U(rig.state.StreamGenerationAt(4)));
			Assert::AreEqual(U(1), U(rig.readers.InFlightDrops()));
			Assert::AreEqual(U(1), U(rig.readers.RestartsAvoided()));
			// Taken once; the generation stays current.
			Assert::IsFalse(rig.readers.TakeProducerNote().drop);
			Assert::AreEqual(U(note.engageGeneration), U(rig.readers.EngageGeneration()));
			// The bound State's StreamGeneration() is still the unbound formula; the LiveQueueReset moved
			// its count once, at the seal of the carrying frame (not at request time).
			Assert::AreEqual(U(MotionValidity::State::kInitialEpoch + 1), U(rig.state.StreamGeneration()));
		}

		TEST_METHOD(VectorProducerSettingsDropsAndContinuityOnlyNotes)
		{
			Rig rig;
			rig.Frames(1, 3);
			rig.state.Invalidate(MVCause::SettingsChange);
			rig.Frames(4, 4);
			const VpMotionResync::ProducerNote s = rig.readers.TakeProducerNote();
			Assert::IsTrue(s.drop);
			Assert::IsFalse(s.skipSpanning);
			Assert::AreEqual(U(1), U(rig.readers.InFlightDrops()));

			// A continuity event: note only, no drop, no re-key, no restart accounting.
			const uint64_t key = rig.readers.EngageGeneration();
			rig.ind->Advance(VpIndicator::kContinuity, RCause::MissedFrames, 5);
			rig.Frames(5, 5);
			const VpMotionResync::ProducerNote m = rig.readers.TakeProducerNote();
			Assert::IsFalse(m.drop);
			Assert::IsTrue(m.skipSpanning);
			Assert::AreEqual(U(key), U(m.engageGeneration));
			Assert::AreEqual(U(1), U(rig.readers.InFlightDrops()));
			Assert::AreEqual(U(1), U(rig.readers.RestartsAvoided()));
			Assert::IsFalse(rig.state.PairValidAt(4, 5));
		}
	};

	TEST_CLASS(MotionValidityResyncKeyTests)
	{
	public:
		TEST_METHOD(VectorStoreKeyMovesOnFullAndContinuityOnly)
		{
			auto ind = std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), 64);
			const uint64_t k1 = VectorStoreKey(*ind, 1);
			Assert::AreEqual(U(k1), U(VectorStoreKey(*ind, 9)));
			Assert::IsTrue(k1 != 0);

			// Timing never moves it.
			ind->Advance(VpIndicator::kTiming, RCause::RateStep, 2);
			Assert::AreEqual(U(k1), U(VectorStoreKey(*ind, 9)));

			// A continuity event moves it for frames at and after it, not before.
			ind->Advance(VpIndicator::kContinuity, RCause::MissedFrames, 4);
			Assert::AreEqual(U(k1), U(VectorStoreKey(*ind, 3)));
			const uint64_t k4 = VectorStoreKey(*ind, 4);
			Assert::IsTrue(k4 != k1);
			Assert::AreEqual(U(k4), U(VectorStoreKey(*ind, 7)));
			// The producer's engage generation (full key) does not move on a continuity event.
			Assert::AreEqual(U(VpResync::Drive::FullKey(*ind, 3)), U(VpResync::Drive::FullKey(*ind, 4)));

			// A FULL moves it again, and the full key too.
			ind->Advance(VpIndicator::kFull, RCause::LiveQueueReset, 8);
			Assert::AreEqual(U(k4), U(VectorStoreKey(*ind, 7)));
			const uint64_t k8 = VectorStoreKey(*ind, 8);
			Assert::IsTrue(k8 != k4);
			Assert::IsTrue(k8 != k1);
			Assert::IsTrue(VpResync::Drive::FullKey(*ind, 8) != VpResync::Drive::FullKey(*ind, 7));
		}

		TEST_METHOD(VectorStoreKeyDiffersAcrossEras)
		{
			VpIndicator::Indicator a(VpIndicator::NextEra(), 64);
			VpIndicator::Indicator b(VpIndicator::NextEra(), 64);
			Assert::IsTrue(VectorStoreKey(a, 1) != VectorStoreKey(b, 1));
		}

		TEST_METHOD(VectorStoreKeyKeepsAStoreGenerationApart)
		{
			// The key is a plain generation for VectorStore: a new key clears it, the same key keeps it.
			VectorStore<int> store(4);
			VpIndicator::Indicator ind(VpIndicator::NextEra(), 64);
			store.SetGeneration(VectorStoreKey(ind, 1));
			Assert::IsTrue(store.Put(1, 2, VectorStoreKey(ind, 2), 7));
			ind.Advance(VpIndicator::kContinuity, RCause::MissedFrames, 5);
			Assert::IsFalse(store.Put(4, 5, VectorStoreKey(ind, 5), 8));   // stale generation refused
		}

		TEST_METHOD(RecordValidIsFalseAcrossAFull)
		{
			VpIndicator::Indicator ind(VpIndicator::NextEra(), 64);
			ind.Advance(VpIndicator::kFull, RCause::LiveQueueReset, 5);
			const VpIndicator::CounterMask m = Bit(VpIndicator::kFull) | Bit(VpIndicator::kContinuity);
			Assert::IsFalse(VpFrameAnalysis::RecordValid(ind, 4, 5, m));
			Assert::IsFalse(VpFrameAnalysis::RecordValid(ind, 3, 9, m));
			Assert::IsTrue(VpFrameAnalysis::RecordValid(ind, 5, 6, m));
			Assert::IsTrue(VpFrameAnalysis::RecordValid(ind, 2, 4, m));
			// A mask that does not name the moved counters ignores it (timing only here).
			Assert::IsTrue(VpFrameAnalysis::RecordValid(ind, 4, 5, Bit(VpIndicator::kTiming)));
			// A settings event is seen only through its own counter.
			ind.Advance(VpIndicator::kSettingsMotion, RCause::SettingsApply, 7);
			Assert::IsTrue(VpFrameAnalysis::RecordValid(ind, 6, 7, m));
			Assert::IsFalse(VpFrameAnalysis::RecordValid(ind, 6, 7, Bit(VpIndicator::kSettingsMotion)));
		}
	};
}
