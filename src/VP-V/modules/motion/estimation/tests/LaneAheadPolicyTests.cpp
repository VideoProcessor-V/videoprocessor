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
