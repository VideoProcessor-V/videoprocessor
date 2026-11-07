/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The chain voter (SID-C2a): the Chain VoterId feeds AxisH / AxisV / Kernel from the latest sealed P14g record. With the consensus on
// the chain is the AUTHORITY for its LATCHED fields (adopted without the consensus's own dwell, rebase requests as any axis change);
// the classical voters decide only while the chain does not vote on a field; the classical safety veto stays contrary evidence
// (T_min, then the chain value comes back once the veto lapses and T_min has passed); a stale chain vote is ignored. The pipeline
// reads the sealed record at lag <= 1 evaluation, refusing stale / other-stream / future records. CPU only, no clock.
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

		inline ChainReading Reading(bool latchedH, bool latchedV, double ratio, int64_t timeUs, int kernelClassical = -1)
		{
			ChainReading cr;
			cr.valid = true;
			cr.axis[0].latched = latchedH;
			cr.axis[0].ratio = ratio;
			cr.axis[1].latched = latchedV;
			cr.axis[1].ratio = ratio;
			cr.kernelClassical = kernelClassical;
			cr.kernelConf = 0.9;
			cr.seq = static_cast<uint64_t>(timeUs / 1000 + 1);
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

		inline int64_t S(double sec) { return static_cast<int64_t>(sec * 1e6); }
	}

	TEST_CLASS(SourceIdChainVoterTests)
	{
	public:
		TEST_METHOD(TheChainVoteCarriesOnlyLatchedFieldsAndAVerifiedKernel)
		{
			using namespace SourceIdentity;
			ChainReading cr = SidChain::Reading(true, false, 0.5, 1000, 4);
			Vote v = ChainVote(cr);
			Assert::IsTrue(v.voter == VoterId::Chain);
			Assert::IsTrue((v.fieldMask & FieldBit(Field::AxisH)) != 0 && (v.fieldMask & FieldBit(Field::AxisV)) == 0, L"V is not latched: not voted");
			Assert::IsTrue((v.fieldMask & FieldBit(Field::Kernel)) != 0 && v.h.kernel == 4);
			Assert::IsTrue(v.h.ratioH == 0.5 && v.evidenceTimeUs == 1000);
			Assert::IsTrue((v.fieldMask & FieldBit(Field::Quality)) == 0 && (v.fieldMask & FieldBit(Field::Codec)) == 0);
			cr.kernelClassical = -1;
			Assert::IsTrue((ChainVote(cr).fieldMask & FieldBit(Field::Kernel)) == 0, L"n known, kernel unsure: no kernel vote");
			cr.valid = false;
			Assert::IsTrue(ChainVote(cr).fieldMask == 0, L"no record: the chain abstains on everything");
			Assert::IsTrue(ChainKernelToClassical(0) == 6 && ChainKernelToClassical(1) == 0 && ChainKernelToClassical(2) == 1 &&
				ChainKernelToClassical(4) == 3 && ChainKernelToClassical(5) == 4 && ChainKernelToClassical(6) == 5);
			Assert::IsTrue(ChainKernelToClassical(3) == -1 && ChainKernelToClassical(7) == -1 && ChainKernelToClassical(-1) == -1 &&
				ChainKernelToClassical(99) == -1, L"no classical equivalent: abstain");
		}

		TEST_METHOD(ALatchedChainFieldIsAdoptedAtOnceAndOutranksTheClassicalVoter)
		{
			using namespace SourceIdentity;
			Consensus c;
			SidChain::Port port;
			Result r;
			for (int i = 0; i <= 40; ++i)
			{
				const int64_t t = SidChain::S(0.5 * i);
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t, 4)));
				c.Submit(SidChain::Classical(1.0, 0.9, t));   // the classical voter disagrees all along
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t, &port);
				if (i == 0)
				{
					Assert::IsTrue(r.Changed(Field::AxisH) && r.Changed(Field::AxisV), L"adopted at the first evaluation, no dwell");
					Assert::IsTrue(r.value.ratioH == 0.5 && r.value.ratioV == 0.5);
					Assert::IsTrue(r.field[static_cast<int>(Field::AxisH)].reason == "chain latched");
					Assert::IsTrue(r.Changed(Field::Kernel) && r.value.kernel == 4, L"the verified kernel too");
				}
				else
					Assert::IsTrue(r.value.ratioH == 0.5 && r.value.ratioV == 0.5, L"the classical dissent never moves a chain-latched field");
			}
			Assert::IsTrue(port.Count(0) == 1 && port.Count(1) == 1, L"one rebase request per axis, through the port");
			Assert::IsTrue(port.calls[0].second == "ratio");
			Assert::IsTrue(c.GetStats().chainAdoptions >= 3);
			Assert::IsTrue(c.GetStats().changes == c.GetStats().chainAdoptions);
		}

		TEST_METHOD(TheClassicalVoterDecidesWhileTheChainDoesNotVoteOnTheField)
		{
			using namespace SourceIdentity;
			Consensus c;
			Result r;
			int vChangedAt = -1;
			for (int i = 0; i <= 8; ++i)
			{
				const int64_t t = SidChain::S(0.5 * i);
				c.Submit(ChainVote(SidChain::Reading(true, false, 0.5, t)));   // only H is latched
				c.Submit(SidChain::Classical(0.5, 0.9, t));
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t);
				if (i == 0)
					Assert::IsTrue(r.Changed(Field::AxisH) && r.field[static_cast<int>(Field::AxisH)].reason == "chain latched");
				if (r.Changed(Field::AxisV) && vChangedAt < 0)
					vChangedAt = i;
			}
			Assert::IsTrue(vChangedAt == 6, L"V: the classical dwell (3 s = 6 half-second ticks)");
			Assert::IsTrue(r.value.ratioV == 0.5);
		}

		TEST_METHOD(AStaleChainVoteIsIgnored)
		{
			using namespace SourceIdentity;
			Consensus c;
			c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, 0)));
			Result r = c.Evaluate(1, SidChain::S(11.0));   // 11 s old: the chain's evidence age is 10 s
			Assert::IsFalse(r.Changed(Field::AxisH), L"a stale chain vote decides nothing");
			Assert::IsTrue(c.GetStats().chainAdoptions == 0);
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
			Assert::IsTrue(r.field[static_cast<int>(Field::AxisH)].reason != "chain latched");
		}

		TEST_METHOD(ChainAuthorityOffMakesTheChainAnOrdinaryWeightedVoter)
		{
			using namespace SourceIdentity;
			Settings s;
			s.chainAuthority = false;
			Consensus c(s);
			Result r;
			int changedAt = -1;
			for (int i = 0; i <= 8; ++i)
			{
				const int64_t t = SidChain::S(0.5 * i);
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t)));
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t);
				if (r.Changed(Field::AxisH) && changedAt < 0)
					changedAt = i;
			}
			Assert::IsTrue(changedAt == 6, L"it goes through the dwell like any voter");
			Assert::IsTrue(c.GetStats().chainAdoptions == 0);
		}

		TEST_METHOD(TheClassicalVetoIsContraryEvidenceThatObeysTMinAndTheChainValueReturnsAfterIt)
		{
			using namespace SourceIdentity;
			Consensus c;
			Result r;
			double ratioAt[41] = {};
			bool heldAt5 = false;
			for (int i = 0; i <= 40; ++i)
			{
				const int64_t t = SidChain::S(static_cast<double>(i));
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t)));
				const bool veto = i >= 5 && i <= 10;   // sustained native evidence from the classical trackers for 6 s
				c.Submit(SidChain::Classical(1.0, 0.9, t, veto));
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t);
				ratioAt[i] = r.value.ratioH;
				if (i == 5)
					heldAt5 = r.field[static_cast<int>(Field::AxisH)].held;
			}
			Assert::IsTrue(ratioAt[0] == 0.5);
			Assert::IsTrue(ratioAt[5] == 0.5 && heldAt5, L"inside T_min of the chain adoption the veto is held, visibly");
			Assert::IsTrue(ratioAt[9] == 0.5);
			Assert::IsTrue(ratioAt[10] == 1.0, L"T_min (10 s) passed: the veto moves the axis to native");
			Assert::IsTrue(ratioAt[11] == 1.0 && ratioAt[19] == 1.0, L"the chain value is not re-adopted inside T_min of the veto");
			Assert::IsTrue(ratioAt[20] == 0.5, L"the veto lapsed: the chain value comes back after T_min");
			Assert::IsTrue(c.GetStats().vetoes >= 1);
		}

		TEST_METHOD(ANewChainDecisionBypassesTMinAndTheDwell)
		{
			using namespace SourceIdentity;
			Consensus c;
			Result r;
			for (int i = 0; i <= 4; ++i)
			{
				const int64_t t = SidChain::S(static_cast<double>(i));
				c.Submit(ChainVote(SidChain::Reading(true, true, 0.5, t)));
				r = c.Evaluate(static_cast<uint64_t>(i + 1), t);
			}
			Assert::IsTrue(r.value.ratioV == 0.5);
			// 5 s after the adoption the chain latches another size: adopted on the next evaluation.
			c.Submit(ChainVote(SidChain::Reading(true, true, 0.667, SidChain::S(5.0))));
			r = c.Evaluate(6, SidChain::S(5.0));
			Assert::IsTrue(r.Changed(Field::AxisH) && r.value.ratioH == 0.667, L"the chain latch has its own dwell and T_min: not repeated here");
			Assert::IsTrue(r.rebase.size() == 2);
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
					const Vote cv = ChainVote(SidChain::Reading(true, i > 10, 0.5, t, 4));
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
			SourceIdentity::Pipeline p;
			p.SetConsensusOn(true);
			Assert::IsTrue(p.GeometrySource() == nullptr);
			G::HeldState geo;
			p.SetGeometrySource(&geo);
			SourceIdentity::Record r = p.Push(Frame(2, 1000000, 1));
			Assert::IsTrue(r.ratioH == 1.0f && (r.flags & SourceIdentity::kFlagChainVote) == 0, L"nothing published yet: no chain vote");
			geo.Publish(MakeRecord(1, 500000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5)));
			r = p.Push(Frame(3, 1500000, 1));
			Assert::IsTrue(r.ratioH == 0.5f && r.ratioV == 0.5f, L"the latched geometry is the consensus value at once");
			Assert::IsTrue((r.flags & SourceIdentity::kFlagChainVote) != 0);
			Assert::IsTrue(r.Latched(SourceIdentity::Field::AxisH) && r.kernel == 4, L"lanczos3 as the classical kernel index");
			Assert::IsTrue(r.reason[static_cast<int>(SourceIdentity::Field::AxisH)] == static_cast<uint8_t>(SourceIdentity::ReasonCode::Chain));
			Assert::IsTrue(r.rebaseGen[0] == 1 && r.rebaseGen[1] == 1);
		}

		TEST_METHOD(ThePipelineIgnoresStaleFutureOtherStreamAndNoRecords)
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
				Assert::IsTrue(r.ratioH == 1.0f, L"a record newer than the evaluated frame is not used");
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
				Assert::IsTrue(r.ratioH == 1.0f, L"another stream key");
			}
			// An unlatched geometry votes nothing; the source can be removed again.
			{
				SourceIdentity::Pipeline p;
				p.SetConsensusOn(true);
				G::HeldState geo;
				p.SetGeometrySource(&geo);
				G::Record g = MakeRecord(1, 1000000, G::AxisGeom(), G::AxisGeom());
				geo.Publish(g);
				const SourceIdentity::Record r = p.Push(Frame(2, 1500000, 1));
				Assert::IsTrue(r.ratioH == 1.0f && (r.flags & SourceIdentity::kFlagChainVote) == 0);
				p.SetGeometrySource(nullptr);
				Assert::IsTrue(p.GeometrySource() == nullptr);
			}
		}

		TEST_METHOD(ChainReadingOfUsesTheAppliedGeometryAndOnlyAVerifiedKernel)
		{
			using namespace GeoFx;
			G::Record g = MakeRecord(7, 123, AppliedAxis(1920, 3840, -1), AppliedAxis(1080, 2160, 4));
			g.measured[0] = MeasuredAxis(G::LatchState::Candidate, 1280, 3840);   // the measured side must not leak into the vote
			const SourceIdentity::ChainReading cr = SourceIdentity::ChainReadingOf(g);
			Assert::IsTrue(cr.valid && cr.seq == 7 && cr.timeUs == 123);
			Assert::IsTrue(cr.axis[0].latched && cr.axis[0].ratio == 0.5 && cr.axis[1].latched);
			Assert::IsTrue(cr.kernelClassical == 3, L"H is unsure; V's lanczos2 (4) is the classical index 3");
			g.applied[0].state = G::LatchState::Unknown;
			Assert::IsFalse(SourceIdentity::ChainReadingOf(g).axis[0].latched);
		}
		TEST_METHOD(ARecordSealedBeforeTheLastDiscontinuityIsNeverAdoptedAgain)
		{
			using namespace GeoFx;
			SourceIdentity::Pipeline p;
			p.SetConsensusOn(true);
			G::HeldState geo;
			p.SetGeometrySource(&geo);
			geo.Publish(MakeRecord(5, 4000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5)));
			SourceIdentity::Record r = p.Push(Frame(6, 4500000, 1));
			Assert::IsTrue(r.ratioH == 0.5f, L"fixture: the latched record is adopted");
			p.Discontinuity(7, 1);   // the first sequence of the new era is 7
			Assert::IsTrue(p.GeometryFloor() == 7);
			r = p.Push(Frame(7, 5000000, 1));
			Assert::IsTrue(r.ratioH == 1.0f && (r.flags & SourceIdentity::kFlagChainVote) == 0,
				L"the ring still holds the old LATCHED record (same stream key): it must not vote again");
			r = p.Push(Frame(8, 5500000, 1));
			Assert::IsTrue(r.ratioH == 1.0f);
			geo.Publish(MakeRecord(9, 5400000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5)));   // the writer's post-reset record
			r = p.Push(Frame(10, 6000000, 1));
			Assert::IsTrue(r.ratioH == 0.5f && (r.flags & SourceIdentity::kFlagChainVote) != 0, L"a record of the new era is adopted");
		}

		TEST_METHOD(TheRecordsOwnMaximumAgeJudgesFreshnessNotTheConsensusEvidenceAge)
		{
			using namespace GeoFx;
			SourceIdentity::Pipeline p;
			p.SetConsensusOn(true);
			G::HeldState geo;
			p.SetGeometrySource(&geo);
			// A governor-stretched cadence: the writer's maximum age is 60 s, the record is 30 s old: still alive, still the authority.
			G::Record g = MakeRecord(1, 0, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5));
			g.maxAgeUs = 60000000;
			geo.Publish(g);
			const SourceIdentity::Record r = p.Push(Frame(2, 30000000, 1));
			Assert::IsTrue(r.ratioH == 0.5f && (r.flags & SourceIdentity::kFlagChainVote) != 0, L"the 10 s consensus evidence age is not the judge");
		}

	};
}
