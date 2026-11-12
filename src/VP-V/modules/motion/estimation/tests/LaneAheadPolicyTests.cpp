// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// B3-Z Z2 scaffold: the matcher top-up decision (LaneAheadPolicy.h) and the E12 epoch rule of the estimate-ahead
// schedule (consumers/DejudderAhead.h, three-argument Consume). Pure CPU.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/LaneAheadPolicy.h>
#include <VP-V/modules/motion/consumers/DejudderAhead.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpMotion::LaneAhead;

namespace LaneAheadPolicyTest
{
	namespace
	{
		Input On()
		{
			Input in;
			in.key = SpareKey::On;
			in.matcherActive = true;
			in.queuedAfterNext = 6;
			return in;
		}
	}

	TEST_CLASS(LaneAheadPolicyTests)
	{
	public:
		TEST_METHOD(KeyAutoIsOff)
		{
			SpareKey k = SpareKey::On;
			Assert::IsTrue(ParseKey("auto", k) && k == SpareKey::Auto);
			Assert::IsTrue(ParseKey("off", k) && k == SpareKey::Off);
			Assert::IsTrue(ParseKey("on", k) && k == SpareKey::On);
			Assert::IsFalse(ParseKey("onn", k));
			Assert::IsFalse(ParseKey("", k));
			Assert::IsFalse(ParseKey(nullptr, k));
			Assert::IsTrue(k == SpareKey::On, L"a bad value leaves the key");
			Input in = On();
			in.key = SpareKey::Auto;
			Assert::IsTrue(Decide(in).reason == Reason::Off, L"auto = off until the Z4 ledger");
			in.key = SpareKey::Off;
			Assert::IsTrue(Decide(in).reason == Reason::Off);
		}

		TEST_METHOD(LatencyModes)
		{
			Input in = On();
			in.lowLatency = true;
			const Decision d = Decide(in);
			Assert::IsTrue(d.reason == Reason::LowLatency);
			Assert::AreEqual(0, d.pairsWanted, L"no lane work in low latency");
			in.competitive = true;
			Assert::IsTrue(Decide(in).reason == Reason::Competitive);
			in = On();
			in.matcherActive = false;
			Assert::IsTrue(Decide(in).reason == Reason::EngineOff);
		}

		// Doc 101 C6 / TD 99 s3.4: the maximum depth is derived (design lead = 2 + anchor slots), capped by the P2 ring (16 - 3 = 13).
		TEST_METHOD(DepthMaxIsDerivedAndCappedByTheP2Ring)
		{
			Assert::AreEqual(13, kStructuralMaxLead);
			Assert::AreEqual(kP2RingPairs - kPairsKeptBehind, kStructuralMaxLead);
			Assert::AreEqual(4, DesignLead(2));
			Assert::AreEqual(4, DepthMax(), L"the default design point: 2 + 2 anchor slots");
			Assert::AreEqual(6, DepthMax(4));
			Assert::AreEqual(13, DepthMax(40), L"the P2 ring caps the design lead");
			Assert::AreEqual(13, DepthMax(2, true), L"extended tier: up to the ring cap");
			Assert::AreEqual(kDepthMin, DepthMax(-1));
			Assert::AreEqual(6, RingCapacity(9, DepthMax(4)));
			Input in = On();
			in.requestedDepth = 9;
			in.queuedAfterNext = 20;
			in.anchorSlots = 4;
			Assert::AreEqual(6, Decide(in).depth);
			in.extendedTier = true;
			Assert::AreEqual(9, Decide(in).depth);
			in.requestedDepth = 30;
			Assert::AreEqual(13, Decide(in).depth);
			in.queuedAfterNext = 3;
			Assert::AreEqual(3, Decide(in).depth, L"still never more than queued");
		}

		// Doc 101 row 9: IAfterEstimate is called once per estimate with both luma planes valid, on the spare path AND the
		// render-time path; a missing plane, a key mismatch, no hook or a repeated estimate is not dispatched (counted).
		TEST_METHOD(AfterEstimateOncePerEstimateBothPaths)
		{
			struct Hook : IAfterEstimate {
				int calls = 0, spare = 0, render = 0, planesValid = 0;
				void AfterEstimate(const AfterEstimateView& v, Entry& e) override
				{
					++calls;
					if (v.atRender) ++render; else ++spare;
					if (v.fromLuma && v.toLuma && v.key == e.key) ++planesValid;
				}
			} hook;
			const int lumaA = 0, lumaB = 0;
			AfterEstimateDispatch d;
			Entry e;
			e.key = { 10, 11, 3 };
			AfterEstimateView v;
			v.key = e.key;
			v.fromLuma = &lumaA;
			v.toLuma = &lumaB;
			Assert::IsTrue(d.Notify(&hook, v, e, 1) == AfterEstimateResult::Called, L"spare path");
			Assert::IsTrue(d.Notify(&hook, v, e, 1) == AfterEstimateResult::Repeat, L"the same estimate twice: once");
			v.atRender = true;
			e.key = { 11, 12, 3 };
			v.key = e.key;
			Assert::IsTrue(d.Notify(&hook, v, e, 2) == AfterEstimateResult::Called, L"render-time path");
			v.toLuma = nullptr;
			Assert::IsTrue(d.Notify(&hook, v, e, 3) == AfterEstimateResult::PlanesMissing);
			v.toLuma = &lumaB;
			Assert::IsTrue(d.Notify(&hook, v, e, 3) == AfterEstimateResult::Repeat, L"a refused estimate is not dispatched later");
			e.key = { 12, 13, 3 };
			Assert::IsTrue(d.Notify(&hook, v, e, 4) == AfterEstimateResult::BadKey, L"view and entry name different pairs");
			v.key = e.key;
			Assert::IsTrue(d.Notify(nullptr, v, e, 5) == AfterEstimateResult::NoHook);
			Assert::IsTrue(d.Notify(&hook, v, e, 0) == AfterEstimateResult::Repeat, L"serial 0 is no estimate");
			Assert::AreEqual(2, hook.calls);
			Assert::AreEqual(2, hook.planesValid, L"every call had both planes and its own entry");
			Assert::AreEqual(1, hook.spare);
			Assert::AreEqual(1, hook.render);
			Assert::AreEqual(1ull, d.CalledInSpare());
			Assert::AreEqual(1ull, d.CalledAtRender());
			Assert::AreEqual(2ull, d.Count(AfterEstimateResult::Called));
			Assert::AreEqual(1ull, d.Count(AfterEstimateResult::PlanesMissing));
			Assert::AreEqual(1ull, d.Count(AfterEstimateResult::BadKey));
			Assert::AreEqual(1ull, d.Count(AfterEstimateResult::NoHook));
			Assert::AreEqual(3ull, d.Count(AfterEstimateResult::Repeat));
		}

		TEST_METHOD(DepthTargetFourMinTwoBoundedByQueueAndLead)
		{
			Assert::AreEqual(4, kDepthTarget);
			Assert::AreEqual(2, kDepthMin);
			Assert::AreEqual(2, RingCapacity(1));
			Assert::AreEqual(4, RingCapacity(9));
			Input in = On();
			Assert::AreEqual(4, Decide(in).depth);
			Assert::AreEqual(4, Decide(in).pairsWanted);
			in.queuedAfterNext = 1;
			Assert::AreEqual(1, Decide(in).depth, L"never more than queued: no added queueing");
			in.queuedAfterNext = 0;
			Assert::IsTrue(Decide(in).reason == Reason::ShortQueue);
			in = On();
			in.laneLead = 3;
			Assert::AreEqual(2, Decide(in).depth, L"lane on: lead - 1");
			in.held = 2;
			Assert::IsTrue(Decide(in).reason == Reason::RingFull);
			in.held = 1;
			Assert::AreEqual(1, Decide(in).pairsWanted);
		}

		TEST_METHOD(P2NotReadyOnlyWhileTheProducerRuns)
		{
			Input in = On();
			in.nextPairP2Ready = false;
			Assert::IsTrue(Decide(in).reason == Reason::Ahead, L"producer not running: matcher alone");
			in.producerRunning = true;
			Assert::IsTrue(Decide(in).reason == Reason::P2NotReady);
			ReasonCounts counts;
			counts.Note(Decide(in).reason);
			counts.Note(Reason::NoSpare);
			Assert::AreEqual(1ull, counts.Count(Reason::P2NotReady));
			Assert::IsTrue(counts.Last() == Reason::NoSpare);
			for (int r = 0; r < static_cast<int>(Reason::Count); ++r)
				Assert::AreNotEqual(std::string("?"), std::string(ReasonName(static_cast<Reason>(r))));
		}

		TEST_METHOD(EntryKeyIsPairAndEpochWithOneExtraGrid)
		{
			EntryKey a{ 10, 11, 3 }, b{ 10, 11, 4 };
			Assert::IsTrue(a.Valid());
			Assert::IsTrue(a.SamePair(b));
			Assert::IsTrue(a != b, L"same pair, other epoch: another entry");
			Assert::IsFalse((EntryKey{ 11, 11, 3 }).Valid());
			Assert::IsFalse((EntryKey{ 10, 11, 0 }).Valid());
			Assert::AreEqual(1, kExtraGridTextures);
			Entry e;
			Assert::AreEqual(16u, e.extra[0].cellPx);
			Assert::IsTrue(e.extra[0].texture == nullptr);
		}

		// E12: an entry made ahead keeps its epoch; a consume in a later epoch is a miss.
		TEST_METHOD(EstimateAheadEntryKeepsItsEpoch)
		{
			using Schedule = Dejudder::AheadSchedule;
			Schedule::Conditions c;
			c.source = 10; c.next = 11; c.after = 12; c.estimated = 10;
			Schedule::Steps steps;
			steps.snapshot = []() { return true; };
			steps.uploadAfter = []() { return true; };
			steps.estimate = [](uint64_t, uint64_t) { return true; };
			{
				Schedule s;
				s.NoteEpoch(5);
				Assert::IsTrue(s.TryAhead(c, steps));
				Assert::IsTrue(s.Consume(11, 12, 5), L"same epoch: hit");
				Assert::AreEqual(5ull, static_cast<unsigned long long>(s.ConsumedEpoch()));
				Assert::AreEqual(1ull, s.Hits());
				Assert::AreEqual(0ull, s.EpochMisses());
			}
			{
				Schedule s;
				s.NoteEpoch(5);
				Assert::IsTrue(s.TryAhead(c, steps));
				Assert::IsFalse(s.Consume(11, 12, 6), L"the epoch moved: estimated now");
				Assert::AreEqual(1ull, s.Misses());
				Assert::AreEqual(1ull, s.EpochMisses());
				Assert::AreEqual(0ull, static_cast<unsigned long long>(s.ConsumedEpoch()));
				Assert::IsFalse(s.Pending());
			}
			{
				Schedule s;   // no epoch noted: never a hit of the checked form
				Assert::IsTrue(s.TryAhead(c, steps));
				Assert::IsFalse(s.Consume(11, 12, 5));
			}
			{
				Schedule s;
				s.NoteEpoch(5);
				Assert::IsTrue(s.TryAhead(c, steps));
				Assert::IsFalse(s.Consume(11, 13, 5), L"another next frame: an ordinary miss");
				Assert::AreEqual(1ull, s.Misses());
				Assert::AreEqual(0ull, s.EpochMisses());
			}
			{
				Schedule s;   // the two-argument form is unchanged
				Assert::IsTrue(s.TryAhead(c, steps));
				Assert::IsTrue(s.Consume(11, 12));
			}
			{
				Schedule s;
				s.NoteEpoch(5);
				Assert::IsTrue(s.TryAhead(c, steps));
				s.Reset();
				Assert::IsFalse(s.Consume(11, 12, 5), L"Reset drops the entry and its epoch");
			}
		}
	};
}
