/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Source-ID consensus (input.source_identity): agreement latches after the dwell, disagreement
// holds, flapping votes do not flap the state, stale votes are ignored, the safety veto is
// immediate, the Indicator port is the only stamp path (per axis), the net's trust profile,
// minHold, the ABR dwell, discontinuity, and determinism.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_identity/SourceIdConsensus.h>
#include <VP-V/modules/input/source_identity/SourceIdVote.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SourceIdCons
	{
		using SourceIdentity::Consensus;
		using SourceIdentity::Field;
		using SourceIdentity::Result;
		using SourceIdentity::Vote;
		using SourceIdentity::VoterId;

		struct MockPort : SourceIdentity::IndicatorPort
		{
			std::vector<std::pair<int, uint64_t>> calls;
			std::vector<std::string> causes;
			void AdvanceRebase(int axis, uint64_t fromSeq, const char* cause) override
			{
				calls.push_back(std::make_pair(axis, fromSeq));
				causes.push_back(cause);
			}
			int Count(int axis) const
			{
				int n = 0;
				for (size_t i = 0; i < calls.size(); ++i)
					if (calls[i].first == axis)
						++n;
				return n;
			}
		};

		inline Vote AxisVote(VoterId voter, double rH, double rV, double conf, int64_t t, bool veto = false)
		{
			Vote v;
			v.voter = voter;
			v.h.ratioH = rH;
			v.h.ratioV = rV;
			SourceIdentity::SetField(v, Field::AxisH, conf);
			SourceIdentity::SetField(v, Field::AxisV, conf);
			v.evidenceTimeUs = t;
			v.evidenceSeq = static_cast<uint64_t>(t / 1000);
			v.safetyVeto = veto;
			return v;
		}

		inline Vote KernelVote(int kernel, double conf, int64_t t)
		{
			Vote v;
			v.voter = VoterId::ClassicalKernel;
			v.h.kernel = kernel;
			SourceIdentity::SetField(v, Field::Kernel, conf);
			v.evidenceTimeUs = t;
			return v;
		}

		inline bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

		// A scripted run whose votes are submitted in forward or reverse order within each tick.
		inline std::string RunScript(bool reverse)
		{
			Consensus c;
			MockPort port;
			std::string trace;
			uint64_t seq = 0;
			for (int64_t t = 0; t <= 30000000; t += 500000)
			{
				std::vector<Vote> votes;
				votes.push_back(AxisVote(VoterId::ClassicalResolution, 0.5, 0.5, 0.9, t));
				votes.push_back(AxisVote(VoterId::IdNet, t < 8000000 ? 0.75 : 0.5, t < 8000000 ? 0.75 : 0.5, 0.9, t));
				if (t >= 5000000)
					votes.push_back(KernelVote(2, 0.8, t));
				SourceIdentity::ClassicalReading rd;
				rd.reading = true;
				rd.bitrateQuality = 0.6;
				rd.timeUs = t;
				votes.push_back(SourceIdentity::BitrateQualityVote(rd, SourceIdentity::VoteSettings()));
				if (reverse)
					std::reverse(votes.begin(), votes.end());
				for (size_t i = 0; i < votes.size(); ++i)
					c.Submit(votes[i]);
				trace += c.Evaluate(++seq, t, &port).Serialize();
				trace += '\n';
			}
			for (size_t i = 0; i < port.calls.size(); ++i)
				trace += std::to_string(port.calls[i].first) + ":" + std::to_string(port.calls[i].second) + ":" + port.causes[i] + ";";
			return trace;
		}
	}

	TEST_CLASS(SourceIdConsensusTests)
	{
	public:
		TEST_METHOD(AgreeingVotesLatchAfterDwellNotBefore)
		{
			using namespace SourceIdCons;
			Consensus c;
			MockPort port;
			uint64_t seq = 0;
			int64_t changeAt = -1;
			for (int64_t t = 0; t <= 6000000; t += 500000)
			{
				c.Submit(AxisVote(VoterId::ClassicalResolution, 0.5, 0.5, 0.9, t));
				const Result r = c.Evaluate(++seq, t, &port);
				if (r.Changed(Field::AxisH) && changeAt < 0)
					changeAt = t;
			}
			Assert::IsTrue(changeAt == 3000000, L"the change latches exactly when the dwell has elapsed");
			Assert::IsTrue(c.Current().ratioH == 0.5 && c.Current().ratioV == 0.5, L"both axes took the ratio");
			Assert::AreEqual(1, port.Count(0));
			Assert::AreEqual(1, port.Count(1));
		}

		TEST_METHOD(DisagreementHoldsTheCurrentValue)
		{
			using namespace SourceIdCons;
			Consensus c;
			uint64_t seq = 0;
			Result last;
			for (int64_t t = 0; t <= 10000000; t += 500000)
			{
				c.Submit(AxisVote(VoterId::ClassicalResolution, 0.5, 0.5, 0.9, t));
				c.Submit(AxisVote(VoterId::IdNet, 0.75, 0.75, 0.9, t));
				last = c.Evaluate(++seq, t);
				Assert::IsTrue(last.changedMask == 0, L"no change while the voters disagree");
			}
			Assert::IsTrue(c.Current().ratioH == 1.0, L"still native");
			Assert::IsTrue(Has(last.field[0].reason, "disagreement"), L"held for disagreement");
			Assert::IsTrue(last.field[0].held);
		}

		TEST_METHOD(FlappingVoteProducesAtMostOneChange)
		{
			using namespace SourceIdCons;
			Consensus c;
			uint64_t seq = 0;
			int changes = 0;
			for (int64_t t = 0; t <= 60000000; t += 500000)
			{
				const double ratio = ((t / 1000000) % 2 == 0) ? 0.5 : 0.75;
				c.Submit(AxisVote(VoterId::ClassicalResolution, ratio, ratio, 0.9, t));
				if (c.Evaluate(++seq, t).Changed(Field::AxisH))
					++changes;
			}
			Assert::IsTrue(changes <= 1, L"a flapping voter does not flap the state");
		}

		TEST_METHOD(StaleVotesAreIgnored)
		{
			using namespace SourceIdCons;
			Consensus c;
			c.Submit(AxisVote(VoterId::ClassicalResolution, 0.5, 0.5, 0.9, 0));
			c.Evaluate(1, 0);
			// Not refreshed: at 10 s the vote is older than the classical voter's maximum evidence age.
			const Result r = c.Evaluate(2, 10000000);
			Assert::IsTrue(r.changedMask == 0, L"a stale vote cannot change the state");
			Assert::IsTrue(r.field[0].reason == "held: stale");
			Assert::IsTrue(r.field[0].stale == 1);
			Assert::IsTrue(c.Current().ratioH == 1.0);
		}

		TEST_METHOD(SafetyVetoSetsNativeAtOnceBypassingDwellAndHold)
		{
			using namespace SourceIdCons;
			Consensus c;
			MockPort port;
			uint64_t seq = 0;
			for (int64_t t = 0; t <= 3000000; t += 500000)
			{
				c.Submit(AxisVote(VoterId::ClassicalResolution, 0.5, 0.5, 0.9, t));
				c.Evaluate(++seq, t, &port);
			}
			Assert::IsTrue(c.Current().ratioH == 0.5, L"re-based state established");
			// 0.5 s later, well inside the 10 s minimum hold: detail above the re-based size.
			c.Submit(AxisVote(VoterId::ClassicalResolution, 0.5, 0.5, 0.9, 3500000, true));
			const Result r = c.Evaluate(++seq, 3500000, &port);
			Assert::IsTrue(c.Current().ratioH == 1.0 && c.Current().ratioV == 1.0, L"native at once");
			Assert::IsTrue(r.Changed(Field::AxisH) && r.Changed(Field::AxisV));
			Assert::IsTrue(Has(r.field[0].reason, "safety veto"));
			Assert::AreEqual(2, port.Count(0));
			Assert::AreEqual(2, port.Count(1));
			Assert::IsTrue(c.GetStats().vetoes == 2);
		}

		TEST_METHOD(SafetyVetoFromAnotherVoterIsIgnored)
		{
			using namespace SourceIdCons;
			Consensus c;
			// The net must not be able to veto.
			c.Submit(AxisVote(VoterId::IdNet, 0.5, 0.5, 0.9, 0, true));
			const Result r = c.Evaluate(1, 0);
			Assert::IsTrue(!Has(r.field[0].reason, "safety veto"));
		}

		TEST_METHOD(AnHOnlyChangeRebasesAxisZeroOnceAndNeverAxisOne)
		{
			using namespace SourceIdCons;
			Consensus c;
			MockPort port;
			uint64_t seq = 0;
			for (int64_t t = 0; t <= 6000000; t += 500000)
			{
				c.Submit(AxisVote(VoterId::ClassicalResolution, 0.5, 1.0, 0.9, t));
				c.Evaluate(++seq, t, &port);
			}
			Assert::IsTrue(c.Current().ratioH == 0.5 && c.Current().ratioV == 1.0);
			Assert::AreEqual(1, port.Count(0));
			Assert::AreEqual(0, port.Count(1));
			Assert::IsTrue(port.causes.size() == 1 && port.causes[0] == "ratio");
		}

		TEST_METHOD(AKernelChangeRebasesItsAxis)
		{
			using namespace SourceIdCons;
			Consensus c;
			MockPort port;
			uint64_t seq = 0;
			for (int64_t t = 0; t <= 8000000; t += 500000)
			{
				c.Submit(AxisVote(VoterId::ClassicalResolution, 0.5, 1.0, 0.9, t));
				if (t >= 4000000)
					c.Submit(KernelVote(2, 0.9, t));
				c.Evaluate(++seq, t, &port);
			}
			Assert::IsTrue(c.Current().kernel == 2, L"kernel latched");
			Assert::AreEqual(2, port.Count(0));       // the ratio change, then the kernel change
			Assert::AreEqual(0, port.Count(1));       // the native axis has no kernel to undo
			Assert::IsTrue(port.causes.back() == "kernel");
		}

		TEST_METHOD(NetKernelBelowItsMinimumConfidenceAbstains)
		{
			using namespace SourceIdCons;
			SourceIdentity::NetReading n;
			n.valid = true;
			n.kernel = 3;
			n.kernelConf = 0.8;
			const SourceIdentity::VoteSettings s;
			Assert::IsTrue((SourceIdentity::IdNetVote(n, s).fieldMask & SourceIdentity::FieldBit(Field::Kernel)) == 0u);
			n.kernelConf = 0.95;
			const Vote v = SourceIdentity::IdNetVote(n, s);
			Assert::IsTrue((v.fieldMask & SourceIdentity::FieldBit(Field::Kernel)) != 0u);
			Assert::IsTrue(v.h.kernel == 3);
		}

		TEST_METHOD(NetQualityTierIgnoredAndCodecTrustCapped)
		{
			using namespace SourceIdCons;
			SourceIdentity::NetReading n;
			n.valid = true;
			n.qualityTier = 2;
			n.qualityConf = 0.95;
			n.codec = SourceIdentity::CodecClass::Av1;
			n.codecConf = 0.9;
			const Vote v = SourceIdentity::IdNetVote(n, SourceIdentity::VoteSettings());
			Assert::IsTrue((v.fieldMask & SourceIdentity::FieldBit(Field::Quality)) == 0u, L"bitrate-tier head abstains by default");
			Assert::IsTrue((v.fieldMask & SourceIdentity::FieldBit(Field::Codec)) != 0u);
			Assert::IsTrue(v.confidence[static_cast<int>(Field::Codec)] <= 0.25 + 1e-12, L"codec trust is capped");
		}

		TEST_METHOD(MinHoldAfterChangeBlocksASecondChange)
		{
			using namespace SourceIdCons;
			Consensus c;
			uint64_t seq = 0;
			int64_t secondAt = -1;
			std::string reasonAt12;
			for (int64_t t = 0; t <= 14000000; t += 500000)
			{
				const double ratio = t < 3500000 ? 0.5 : 0.75;
				c.Submit(AxisVote(VoterId::ClassicalResolution, ratio, ratio, 0.9, t));
				const Result r = c.Evaluate(++seq, t);
				if (t == 12500000)
					reasonAt12 = r.field[0].reason;
				if (t > 3000000 && r.Changed(Field::AxisH) && secondAt < 0)
					secondAt = t;
			}
			Assert::IsTrue(reasonAt12 == "held: min hold", L"dwell is over but the minimum hold still blocks");
			Assert::IsTrue(secondAt == 13000000, L"the second change waits for the minimum hold after the first (3 s + 10 s)");
			Assert::IsTrue(c.Current().ratioH == 0.75);
		}

		TEST_METHOD(AnAbrBoundaryShortensTheDwell)
		{
			using namespace SourceIdCons;
			SourceIdentity::AbrEvent e;
			e.timeUs = 0;
			Consensus withAbr;
			withAbr.NoteAbrBoundary(e);
			Consensus without;
			int64_t abrAt = -1, plainAt = -1;
			uint64_t seq = 0;
			for (int64_t t = 0; t <= 4000000; t += 500000)
			{
				const Vote v = AxisVote(VoterId::ClassicalResolution, 0.5, 0.5, 0.9, t);
				withAbr.Submit(v);
				without.Submit(v);
				++seq;
				if (withAbr.Evaluate(seq, t).Changed(Field::AxisH) && abrAt < 0) abrAt = t;
				if (without.Evaluate(seq, t).Changed(Field::AxisH) && plainAt < 0) plainAt = t;
			}
			Assert::IsTrue(abrAt == 500000, L"a rendition switch is sudden: the short dwell applies");
			Assert::IsTrue(plainAt == 3000000, L"without a boundary the normal dwell applies");
		}

		TEST_METHOD(DiscontinuityResetsWithoutEmittingStamps)
		{
			using namespace SourceIdCons;
			Consensus c;
			MockPort port;
			uint64_t seq = 0;
			for (int64_t t = 0; t <= 3000000; t += 500000)
			{
				c.Submit(AxisVote(VoterId::ClassicalResolution, 0.5, 0.5, 0.9, t));
				c.Evaluate(++seq, t, &port);
			}
			const size_t before = port.calls.size();
			Assert::IsTrue(before == 2);
			c.Discontinuity(99);
			Assert::IsTrue(c.Current().ratioH == 1.0 && c.Current().ratioV == 1.0, L"back to native");
			const Result r = c.Evaluate(100, 3500000, &port);
			Assert::IsTrue(r.field[0].reason == "no votes", L"the old votes were dropped");
			Assert::IsTrue(port.calls.size() == before, L"consensus emits no stamp for a discontinuity");
			Assert::IsTrue(c.LastDiscontinuitySeq() == 99);
			Assert::IsTrue(c.GetStats().discontinuities == 1);
		}

		TEST_METHOD(SameVotesTwiceGiveTheSameTrace)
		{
			using namespace SourceIdCons;
			const std::string a = RunScript(false);
			const std::string b = RunScript(false);
			Assert::IsTrue(a == b);
			Assert::IsTrue(Has(a, "rebase(0,"), L"the script exercises a real change");
		}

		TEST_METHOD(VoteSubmissionOrderWithinATickDoesNotMatter)
		{
			using namespace SourceIdCons;
			Assert::IsTrue(RunScript(false) == RunScript(true));
		}
	};
}
