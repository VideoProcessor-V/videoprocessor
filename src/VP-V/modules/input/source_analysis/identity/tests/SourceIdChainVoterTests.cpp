/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The chain voter (SID train r2, doc 78 section 5 owner decision): the Chain VoterId feeds AxisH / AxisV / Kernel from the CONFIDENT
// reading of the latest sealed P14g record, as an ORDINARY weighted voter of the consensus: no authority, a conservative placeholder
// trust (N5), and a dwell that counts DISTINCT informative evaluations (N4: a record that is read again - held, re-submitted for every
// analysed frame - never satisfies it). The classical voters outvote it, the classical safety veto still moves an axis to native at
// once, a stale chain vote is ignored. The pipeline reads the sealed record at lag <= 1 evaluation, refusing stale / other-stream /
// future / before-the-discontinuity records. CPU only, no clock.
#include "pch.h"
#include "CppUnitTest.h"

#include "GeometryFixtures.h"

#include <VP-V/modules/input/source_analysis/identity/SourceIdConsensus.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdPipeline.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdVote.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SidChain
	{
		using namespace SourceIdentity;

		struct Port : IndicatorPort
		{
			std::vector<std::pair<int, std::string>> calls;
			void AdvanceRebase(int axis, uint64_t, const char* cause) override { calls.push_back(std::make_pair(axis, std::string(cause))); }
			int Count(int axis) const
			{
				int n = 0;
				for (size_t i = 0; i < calls.size(); ++i)
					if (calls[i].first == axis)
						++n;
				return n;
			}
		};

		inline int64_t S(double sec) { return static_cast<int64_t>(sec * 1e6); }

		// A confident reading as the pipeline builds it from a record. `evalSeq` is the EVALUATION behind it (what N4 counts).
		inline ChainReading Reading(bool confidentH, bool confidentV, double ratio, int64_t timeUs, uint64_t evalSeq, int kernelClassical = -1,
			double confidence = 1.0)
		{
			ChainReading cr;
			cr.valid = true;
			cr.axis[0].confident = confidentH;
			cr.axis[0].ratio = ratio;
			cr.axis[0].confidence = confidence;
			cr.axis[1].confident = confidentV;
			cr.axis[1].ratio = ratio;
			cr.axis[1].confidence = confidence;
			cr.kernelClassical = kernelClassical;
			cr.kernelConf = 0.9;
			cr.seq = evalSeq;
			cr.timeUs = timeUs;
			return cr;
		}

		inline Vote Classical(double ratio, double conf, int64_t t, bool veto = false)
		{
			Vote v;
			v.voter = VoterId::ClassicalResolution;
			v.h.ratioH = ratio;
			v.h.ratioV = ratio;
			SetField(v, Field::AxisH, conf);
			SetField(v, Field::AxisV, conf);
			v.evidenceTimeUs = t;
			v.safetyVeto = veto;
			return v;
		}
	}

	TEST_CLASS(SourceIdChainVoterTests)
	{
	public:
		TEST_METHOD(TheChainVoteCarriesOnlyConfidentFieldsAndAVerifiedKernel)
		{
			using namespace SourceIdentity;
			ChainReading cr = SidChain::Reading(true, false, 0.5, 1000, 7, 4, 0.8);
			Vote v = ChainVote(cr);
			Assert::IsTrue(v.voter == VoterId::Chain);
			Assert::IsTrue((v.fieldMask & FieldBit(Field::AxisH)) != 0 && (v.fieldMask & FieldBit(Field::AxisV)) == 0, L"V is UNSURE: not voted");
			Assert::IsTrue((v.fieldMask & FieldBit(Field::Kernel)) != 0 && v.h.kernel == 4);
			Assert::IsTrue(v.h.ratioH == 0.5 && v.evidenceTimeUs == 1000);
			Assert::IsTrue(v.confidence[static_cast<int>(Field::AxisH)] == 0.8, L"the confidence is the reading's (LLR / c1, capped at 1)");
			Assert::IsTrue(v.evidenceSeq == 7, L"the evaluation behind the record, not the frame");
			Assert::IsTrue((v.fieldMask & FieldBit(Field::Quality)) == 0 && (v.fieldMask & FieldBit(Field::Codec)) == 0);
			cr.kernelClassical = -1;
			Assert::IsTrue((ChainVote(cr).fieldMask & FieldBit(Field::Kernel)) == 0, L"n known, kernel unsure: no kernel vote");
			cr.axis[0].confidence = 0.0;
			Assert::IsTrue((ChainVote(cr).fieldMask & FieldBit(Field::AxisH)) == 0, L"a confident axis without confidence votes nothing");
			cr.valid = false;
			Assert::IsTrue(ChainVote(cr).fieldMask == 0, L"no record: the chain abstains on everything");
			Assert::IsTrue(ChainKernelToClassical(0) == 6 && ChainKernelToClassical(1) == 0 && ChainKernelToClassical(2) == 1 &&
				ChainKernelToClassical(4) == 3 && ChainKernelToClassical(5) == 4 && ChainKernelToClassical(6) == 5);
			Assert::IsTrue(ChainKernelToClassical(3) == -1 && ChainKernelToClassical(7) == -1 && ChainKernelToClassical(-1) == -1 &&
				ChainKernelToClassical(99) == -1, L"no classical equivalent: abstain");
		}

		TEST_METHOD(TheChainIsAnOrdinaryVoterWithAConservativePlaceholderTrust)
		{
			using namespace SourceIdentity;
			const Settings s;
			const int chain = static_cast<int>(VoterId::Chain);
			Assert::IsTrue(s.trust[chain] > 0.0 && s.trust[chain] < s.trust[static_cast<int>(VoterId::ClassicalResolution)],
				L"N5: below the classical resolution voter, so the chain alone can pass the minimum weight but never outvotes it");
			Assert::IsTrue(s.trust[chain] * 1.0 >= s.enterMinWeight, L"a full-confidence chain reading alone still reaches enterMinWeight");
			Assert::IsTrue(s.chainMinDistinctEvaluations >= 2, L"one evaluation can never satisfy the dwell");
		}

		TEST_METHOD(ADistinctEvaluationsRunAndTheDwellDecideWhenTheChainAloneChangesAField)
		{
			using namespace SourceIdentity;
			Consensus c;
			SidChain::Port port;
			Result r;
			int changedAt = -1;
			for (int i = 0; i <= 14; ++i)
			{
				const int64_t t = SidChain::S(0.5 * i);
				// One evaluation per second: two analysed frames read the same record.
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t, 100 + static_cast<uint64_t>(i / 2), 4)));
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t, &port);
				if (r.Changed(Field::AxisH) && changedAt < 0)
					changedAt = i;
				if (i < 2)
					Assert::IsTrue(r.field[static_cast<int>(Field::AxisH)].reason.find("chain: waiting") != std::string::npos, L"one evaluation: waiting, visibly");
			}
			// Frame 2 (t = 1.0 s) is the second distinct evaluation: the chain joins, the candidate starts, the 3 s dwell ends at t = 4.0 s.
			Assert::IsTrue(changedAt == 8, L"second distinct evaluation at 1 s + the 3 s dwell = frame 8");
			Assert::IsTrue(r.value.ratioH == 0.5 && r.value.ratioV == 0.5 && r.value.kernel == 4, L"axes and the verified kernel");
			Assert::IsTrue(port.Count(0) == 1 && port.Count(1) == 1 && port.calls[0].second == "ratio", L"one re-base request per axis through the port");
		}

		TEST_METHOD(AHeldRecordNeverSatisfiesTheDwell)
		{
			using namespace SourceIdentity;
			Consensus c;
			Result r;
			bool anyChange = false;
			for (int i = 0; i <= 80; ++i)   // 40 s of ONE record read again at every analysed frame (an estimator that stopped)
			{
				const int64_t t = SidChain::S(0.5 * i);
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t, 100, 4)));
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t);
				anyChange = anyChange || r.Changed(Field::AxisH) || r.Changed(Field::AxisV) || r.Changed(Field::Kernel);
			}
			Assert::IsFalse(anyChange, L"the same evaluation read for 40 s is still one evaluation");
			Assert::IsTrue(r.value.ratioH == 1.0 && r.value.ratioV == 1.0);
			Assert::IsTrue(r.field[static_cast<int>(Field::AxisH)].reason.find("chain: waiting") != std::string::npos);
		}

		TEST_METHOD(ADifferentChainValueStartsTheDistinctEvaluationCountOver)
		{
			using namespace SourceIdentity;
			Consensus c;
			Result r;
			// 0.5 and 0.667 alternate evaluation by evaluation (an unsure picture): no value ever has two distinct evaluations in a row.
			for (int i = 0; i <= 60; ++i)
			{
				const int64_t t = SidChain::S(0.5 * i);
				const double ratio = (i / 2) % 2 == 0 ? 0.5 : 0.667;
				c.Submit(ChainVote(SidChain::Reading(true, true, ratio, t, 100 + static_cast<uint64_t>(i / 2))));
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t);
				Assert::IsFalse(r.Changed(Field::AxisH), L"alternating readings never reach the run length");
			}
			// A value that stays for two distinct evaluations does join.
			bool changed = false;
			for (int i = 61; i <= 80; ++i)
			{
				const int64_t t = SidChain::S(0.5 * i);
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t, 200 + static_cast<uint64_t>((i - 61) / 2))));
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t);
				changed = changed || r.Changed(Field::AxisH);
			}
			Assert::IsTrue(changed && r.value.ratioH == 0.5, L"the same value in distinct evaluations changes the field after the dwell");
		}

		TEST_METHOD(AConfidentClassicalVoterOutvotesTheConservativeChain)
		{
			using namespace SourceIdentity;
			Consensus c;
			Result r;
			for (int i = 0; i <= 40; ++i)
			{
				const int64_t t = SidChain::S(0.5 * i);
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t, 100 + static_cast<uint64_t>(i / 2), 4)));
				c.Submit(SidChain::Classical(1.0, 0.9, t));   // the classical voters say native all along
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t);
				Assert::IsTrue(r.value.ratioH == 1.0 && r.value.ratioV == 1.0, L"the chain alone against a confident classical vote never moves the field");
			}
			Assert::IsTrue(c.GetStats().changes == 0);
		}

		TEST_METHOD(AWeakChainReadingAloneDoesNotReachTheMinimumWeight)
		{
			using namespace SourceIdentity;
			Consensus c;
			Result r;
			for (int i = 0; i <= 40; ++i)
			{
				const int64_t t = SidChain::S(0.5 * i);
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t, 100 + static_cast<uint64_t>(i / 2), -1, 0.4)));   // 0.4 x trust 0.5 = 0.2
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t);
			}
			Assert::IsTrue(r.value.ratioH == 1.0 && r.field[static_cast<int>(Field::AxisH)].reason == "held: weak evidence");
		}

		TEST_METHOD(ChainAndClassicalAgreeingChangeTheFieldAfterTheDwell)
		{
			using namespace SourceIdentity;
			Consensus c;
			SidChain::Port port;
			Result r;
			int changes = 0;
			int changedAt = -1;
			for (int i = 0; i <= 20; ++i)
			{
				const int64_t t = SidChain::S(0.5 * i);
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t, 100 + static_cast<uint64_t>(i / 2))));
				c.Submit(SidChain::Classical(0.5, 0.9, t));
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t, &port);
				if (r.Changed(Field::AxisH))
				{
					++changes;
					if (changedAt < 0)
						changedAt = i;
				}
			}
			Assert::IsTrue(changes == 1 && changedAt >= 6 && changedAt <= 8, L"one change, after the consensus dwell (3 s)");
			Assert::IsTrue(r.value.ratioH == 0.5 && r.value.ratioV == 0.5);
			Assert::IsTrue(port.Count(0) == 1 && port.Count(1) == 1);
		}

		TEST_METHOD(AStaleChainVoteIsIgnored)
		{
			using namespace SourceIdentity;
			Consensus c;
			c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, 0, 1)));
			Result r = c.Evaluate(1, SidChain::S(11.0));   // 11 s old: the chain's evidence age is 10 s
			Assert::IsFalse(r.Changed(Field::AxisH), L"a stale chain vote decides nothing");
			int64_t changedAt = -1;
			for (int i = 0; i <= 8; ++i)
			{
				const int64_t t = SidChain::S(11.0 + 0.5 * i);
				c.Submit(SidChain::Classical(0.5, 0.9, t));
				r = c.Evaluate(static_cast<uint64_t>(i + 2), t);
				if (r.Changed(Field::AxisH) && changedAt < 0)
					changedAt = t;
			}
			Assert::IsTrue(changedAt == SidChain::S(14.0), L"the classical voter decides, after its own dwell");
		}

		TEST_METHOD(TheClassicalSafetyVetoStillMovesAnAxisToNativeAtOnce)
		{
			using namespace SourceIdentity;
			Consensus c;
			Result r;
			double ratioAt[41] = {};
			for (int i = 0; i <= 40; ++i)
			{
				const int64_t t = SidChain::S(static_cast<double>(i));
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t, 100 + static_cast<uint64_t>(i))));
				const bool veto = i >= 12 && i <= 14;   // sustained native evidence from the classical trackers
				c.Submit(SidChain::Classical(0.5, 0.9, t, veto));
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t);
				ratioAt[i] = r.value.ratioH;
			}
			Assert::IsTrue(ratioAt[5] == 0.5, L"fixture: chain and classical agreed and the value was adopted");
			Assert::IsTrue(ratioAt[12] == 1.0, L"the veto sets the axis native at once");
			Assert::IsTrue(c.GetStats().vetoes >= 1);
		}

		TEST_METHOD(TheEvaluationIsIdenticalWhateverTheSubmitOrder)
		{
			using namespace SourceIdentity;
			auto run = [](bool chainFirst) {
				Consensus c;
				std::string trace;
				for (int i = 0; i <= 30; ++i)
				{
					const int64_t t = SidChain::S(0.5 * i);
					const Vote cv = ChainVote(SidChain::Reading(true, i > 10, 0.5, t, 100 + static_cast<uint64_t>(i / 2), 4));
					const Vote kv = SidChain::Classical(0.667, 0.9, t);
					if (chainFirst) { c.Submit(cv); c.Submit(kv); } else { c.Submit(kv); c.Submit(cv); }
					trace += c.Evaluate(static_cast<uint64_t>(i + 1), t).Serialize() + "\n";
				}
				return trace;
			};
			Assert::IsTrue(run(true) == run(false));
		}

		static SourceIdentity::FrameInput Frame(uint64_t seq, int64_t timeUs, uint64_t streamKey)
		{
			SourceIdentity::FrameInput in;
			in.seq = seq;
			in.timeUs = timeUs;
			in.streamKey = streamKey;
			in.deliveredRatio = 1.0;
			in.apl = 0.4;
			return in;
		}

		TEST_METHOD(ThePipelineFeedsTheChainVoterFromTheLatestSealedRecord)
		{
			using namespace GeoFx;
			{
				SourceIdentity::Pipeline none;
				none.SetConsensusOn(true);
				Assert::IsTrue(none.GeometrySource() == nullptr);
				G::HeldState empty;
				none.SetGeometrySource(&empty);
				const SourceIdentity::Record e = none.Push(Frame(2, 1000000, 1));
				Assert::IsTrue(e.ratioH == 1.0f && (e.flags & SourceIdentity::kFlagChainVote) == 0, L"nothing published yet: no chain vote");
			}
			SourceIdentity::Pipeline p;
			p.SetConsensusOn(true);
			G::HeldState geo;
			p.SetGeometrySource(&geo);
			SourceIdentity::Record r;
			int changedAt = -1;
			for (int i = 0; i <= 14; ++i)
			{
				const uint64_t seq = 10 + static_cast<uint64_t>(i);
				const int64_t t = static_cast<int64_t>(i) * 500000;
				if (i % 2 == 0)
					geo.Publish(GeoFx::MakeRecord(seq, t, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5)));   // one evaluation per second; the frame between reads it again
				r = p.Push(Frame(seq, t, 1));
				Assert::IsTrue((r.flags & SourceIdentity::kFlagChainVote) != 0, L"a usable confident record is a chain vote on every frame");
				if (r.ratioH == 0.5f && changedAt < 0)
					changedAt = i;
			}
			Assert::IsTrue(changedAt == 8, L"two distinct evaluations (1 s) and the 3 s consensus dwell: the chain value is adopted at frame 8, not at once");
			Assert::IsTrue(r.ratioH == 0.5f && r.ratioV == 0.5f);
			Assert::IsTrue(r.Latched(SourceIdentity::Field::AxisH) && r.kernel == 4, L"lanczos3 as the classical kernel index");
			Assert::IsTrue(r.rebaseGen[0] == 1 && r.rebaseGen[1] == 1);
		}

		TEST_METHOD(ThePipelineIgnoresStaleFutureOtherStreamUnsureAndNoRecords)
		{
			using namespace GeoFx;
			// A record older than its maxAge.
			{
				SourceIdentity::Pipeline p;
				p.SetConsensusOn(true);
				G::HeldState geo;
				p.SetGeometrySource(&geo);
				G::Record g = MakeRecord(1, 0, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5));
				g.maxAgeUs = 1000000;
				geo.Publish(g);
				const SourceIdentity::Record r = p.Push(Frame(2, 5000000, 1));
				Assert::IsTrue(r.ratioH == 1.0f && (r.flags & SourceIdentity::kFlagChainVote) == 0, L"stale");
			}
			// A record from after this frame (lag: only sealed records <= the frame's sequence).
			{
				SourceIdentity::Pipeline p;
				p.SetConsensusOn(true);
				G::HeldState geo;
				p.SetGeometrySource(&geo);
				geo.Publish(MakeRecord(100, 1000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5)));
				const SourceIdentity::Record r = p.Push(Frame(2, 1500000, 1));
				Assert::IsTrue((r.flags & SourceIdentity::kFlagChainVote) == 0, L"a record newer than the evaluated frame is not used");
			}
			// Another stream (title): the stream key must match.
			{
				SourceIdentity::Pipeline p;
				p.SetConsensusOn(true);
				G::HeldState geo;
				p.SetGeometrySource(&geo);
				G::Record g = MakeRecord(1, 1000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5));
				g.streamKey = 7;
				geo.Publish(g);
				const SourceIdentity::Record r = p.Push(Frame(2, 1500000, 1));
				Assert::IsTrue((r.flags & SourceIdentity::kFlagChainVote) == 0, L"another stream key");
			}
			// A record whose axes are UNSURE votes nothing; the source can be removed again.
			{
				SourceIdentity::Pipeline p;
				p.SetConsensusOn(true);
				G::HeldState geo;
				p.SetGeometrySource(&geo);
				G::Record g = MakeRecord(1, 1000000, G::AxisGeom(), G::AxisGeom());
				geo.Publish(g);
				const SourceIdentity::Record r = p.Push(Frame(2, 1500000, 1));
				Assert::IsTrue(r.ratioH == 1.0f && (r.flags & SourceIdentity::kFlagChainVote) == 0, L"UNSURE: not a vote");
				p.SetGeometrySource(nullptr);
				Assert::IsTrue(p.GeometrySource() == nullptr);
			}
			// Control: a fresh confident record of this stream IS a vote.
			{
				SourceIdentity::Pipeline p;
				p.SetConsensusOn(true);
				G::HeldState geo;
				p.SetGeometrySource(&geo);
				geo.Publish(MakeRecord(1, 1000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5)));
				const SourceIdentity::Record r = p.Push(Frame(2, 1500000, 1));
				Assert::IsTrue((r.flags & SourceIdentity::kFlagChainVote) != 0, L"the control: the checks above are not vacuous");
			}
		}

		TEST_METHOD(ChainReadingOfUsesTheAppliedConfidentReadingAndOnlyAVerifiedKernel)
		{
			using namespace GeoFx;
			G::Record g = MakeRecord(7, 123, AppliedAxis(1920, 3840, -1), AppliedAxis(1080, 2160, 4));
			g.measured[0] = MeasuredAxis(G::LatchState::Provisional, 1280, 3840);   // the measured side must not leak into the vote
			const SourceIdentity::ChainReading cr = SourceIdentity::ChainReadingOf(g);
			Assert::IsTrue(cr.valid && cr.seq == 7 && cr.timeUs == 123);
			Assert::IsTrue(cr.axis[0].confident && cr.axis[0].ratio == 0.5 && cr.axis[1].confident);
			Assert::IsTrue(cr.axis[0].confidence > 0.98 && cr.axis[0].confidence <= 1.0, L"the record's applied confidence");
			Assert::IsTrue(cr.kernelClassical == 3, L"H is kernel-unsure; V's lanczos2 (4) is the classical index 3");
			g.applied[0].state = G::LatchState::Unknown;
			Assert::IsFalse(SourceIdentity::ChainReadingOf(g).axis[0].confident);
			g.applied[0].state = G::LatchState::Latched;
			g.applied[0].conf = 0.0f;
			const SourceIdentity::Vote v = SourceIdentity::ChainVote(SourceIdentity::ChainReadingOf(g));
			Assert::IsTrue((v.fieldMask & SourceIdentity::FieldBit(SourceIdentity::Field::AxisH)) == 0u, L"a confident axis with no confidence in the record is not voted");
		}

		TEST_METHOD(ARecordSealedBeforeTheLastDiscontinuityIsNeverUsedAgain)
		{
			using namespace GeoFx;
			SourceIdentity::Pipeline p;
			p.SetConsensusOn(true);
			G::HeldState geo;
			p.SetGeometrySource(&geo);
			geo.Publish(MakeRecord(5, 4000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5)));
			SourceIdentity::Record r = p.Push(Frame(6, 4500000, 1));
			Assert::IsTrue((r.flags & SourceIdentity::kFlagChainVote) != 0, L"fixture: the confident record is a vote");
			p.Discontinuity(7, 1);   // the first sequence of the new era is 7
			Assert::IsTrue(p.GeometryFloor() == 7);
			r = p.Push(Frame(7, 5000000, 1));
			Assert::IsTrue(r.ratioH == 1.0f && (r.flags & SourceIdentity::kFlagChainVote) == 0,
				L"the ring still holds the old record (same stream key): it must not vote again");
			r = p.Push(Frame(8, 5500000, 1));
			Assert::IsTrue((r.flags & SourceIdentity::kFlagChainVote) == 0);
			geo.Publish(MakeRecord(9, 5400000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5)));   // the writer's post-reset record
			r = p.Push(Frame(10, 6000000, 1));
			Assert::IsTrue((r.flags & SourceIdentity::kFlagChainVote) != 0, L"a record of the new era votes");
		}

		TEST_METHOD(TheRecordsOwnMaximumAgeJudgesFreshnessNotTheConsensusEvidenceAge)
		{
			using namespace GeoFx;
			SourceIdentity::Pipeline p;
			p.SetConsensusOn(true);
			G::HeldState geo;
			p.SetGeometrySource(&geo);
			// A governor-stretched cadence: the writer's maximum age is 60 s, the record is 30 s old: still alive, still a vote.
			G::Record g = MakeRecord(1, 0, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5));
			g.maxAgeUs = 60000000;
			geo.Publish(g);
			const SourceIdentity::Record r = p.Push(Frame(2, 30000000, 1));
			Assert::IsTrue((r.flags & SourceIdentity::kFlagChainVote) != 0, L"the 10 s consensus evidence age is not the judge");
		}
	};
}
