/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The shadow-deploy evidence log (SID-C2b, doc 78 section 9b, identity/chain/ChainEvidenceLog.h): segment / state / aggregate rows, the
// RAM ring of the last 30 evaluations' lines dumped on the triggers (disagreement, flap, thin latch, HOLD, UNSTABLE, power unseen,
// coverage bucket, hotkey) with their rate limits (<= 1 per 5 min, <= 20 per day), the frames level, the 2 GB FIFO cap (tested small),
// the off states, the one-time notice, a failing disk reported once. In-memory file system, injected clock, hand-made evaluations.
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace
	{
		using namespace ChainFx;

		const std::string kFolder = "mem\\ev";

		int Count(const std::string& s, const std::string& what)
		{
			int n = 0;
			size_t p = 0;
			while ((p = s.find(what, p)) != std::string::npos)
			{
				++n;
				p += what.size();
			}
			return n;
		}

		std::shared_ptr<const C::LineSnapshot> Snap(uint64_t seq, uint16_t fill)
		{
			std::shared_ptr<C::LineSnapshot> s = std::make_shared<C::LineSnapshot>();
			s->seq = seq;
			s->timeUs = static_cast<int64_t>(seq) * 1000;
			s->active.w = 100;
			s->active.h = 80;
			s->inner.x = 5;
			s->inner.y = 5;
			s->inner.w = 40;
			s->inner.h = 30;
			s->rowCount = 2;
			s->colCount = 2;
			s->rowY = { 10, 40 };
			s->colX = { 5, 25 };
			s->rows.assign(2u * 40u, fill);
			s->cols.assign(2u * 30u, fill);
			return s;
		}

		C::LogEval MakeEval(uint64_t seq, double sec, const std::shared_ptr<const C::LineSnapshot>& lines)
		{
			C::LogEval e;
			e.seq = seq;
			e.timeUs = static_cast<int64_t>(sec * 1e6);
			e.active.w = 3840;
			e.active.h = 2160;
			e.c1 = 15.0;
			e.c2 = 18.0;
			e.stepCodes = 4;
			e.intervalSec = 1.0;
			for (int a = 0; a < 2; ++a)
			{
				C::LogAxis& L = e.axis[a];
				L.evaluated = true;
				L.informative = true;
				L.state = G::LatchState::Provisional;
				L.N = a == 0 ? 3840 : 2160;
				L.lines = 32;
				L.gatedLines = 20;
				L.maskedLines = 1;
			}
			e.lines = lines;
			return e;
		}

		C::LogEval MakeEval(uint64_t seq, double sec) { return MakeEval(seq, sec, Snap(seq, 100)); }

		void SetLatched(C::LogEval& e, int axis, int n, double llr)
		{
			C::LogAxis& L = e.axis[axis];
			L.state = G::LatchState::Latched;
			L.appliedState = G::LatchState::Latched;
			L.appliedN = n;
			L.measuredN = n;
			L.appliedKernel = 5;   // lanczos3
			L.measuredKernel = 5;
			L.appliedLlr = llr;
			L.measuredLlr = llr;
			L.appliedChanged = true;
			L.stateChanged = true;
			L.latchReason = static_cast<int>(C::LatchReason::Latched);
		}

		void SetUnstable(C::LogEval& e)
		{
			e.axis[0].state = G::LatchState::Unstable;
			e.axis[0].stateChanged = true;
		}

		struct Env
		{
			std::shared_ptr<MemFs> fs;
			ManualClock clk;
			std::unique_ptr<C::EvidenceLog> log;
			explicit Env(const C::EvidenceLogSettings& s = C::EvidenceLogSettings(), C::EvidenceLevel level = C::EvidenceLevel::Lines)
				: fs(std::make_shared<MemFs>())
			{
				log.reset(new C::EvidenceLog(s, fs, clk.Fn(), FixedStamp()));
				log->Configure(kFolder, level, true, "shadow");
			}
			std::string Jsonl() { return fs->Get(kFolder, fs->FirstWithSuffix(kFolder, ".jsonl")); }
		};
	}

	TEST_CLASS(ChainEvidenceLogTests)
	{
	public:
		TEST_METHOD(LevelNamesRoundTrip)
		{
			const C::EvidenceLevel all[4] = { C::EvidenceLevel::Off, C::EvidenceLevel::Log, C::EvidenceLevel::Lines, C::EvidenceLevel::Frames };
			for (int i = 0; i < 4; ++i)
			{
				C::EvidenceLevel out = C::EvidenceLevel::Frames;
				Assert::IsTrue(C::ParseEvidenceLevel(C::EvidenceLevelText(all[i]), out));
				Assert::IsTrue(out == all[i]);
			}
			C::EvidenceLevel l = C::EvidenceLevel::Off;
			Assert::IsTrue(C::ParseEvidenceLevel(" LOG+Lines ", l));
			Assert::IsTrue(l == C::EvidenceLevel::Lines);
			Assert::IsFalse(C::ParseEvidenceLevel("everything", l));
			Assert::AreEqual(std::string("log+lines+frames"), std::string(C::EvidenceLevelText(C::EvidenceLevel::Frames)));
		}

		TEST_METHOD(OffAndInactiveStatesWriteNothing)
		{
			Env env;
			env.log->Configure(kFolder, C::EvidenceLevel::Lines, false, "shadow");   // the pathway / consensus says off
			env.log->OnEvaluation(MakeEval(1, 1.0), nullptr);
			env.log->Configure(kFolder, C::EvidenceLevel::Off, true, "shadow");      // the key says off
			env.log->OnEvaluation(MakeEval(2, 2.0), nullptr);
			env.log->Configure(std::string(), C::EvidenceLevel::Lines, true, "shadow");   // no folder
			env.log->OnEvaluation(MakeEval(3, 3.0), nullptr);
			env.log->Close(3000000, "test");
			Assert::IsFalse(env.log->Active());
			Assert::AreEqual<size_t>(0, env.fs->FileCount());
			Assert::AreEqual(std::string("evidence: log+lines requested, no folder (set the history folder)"), env.log->StatusText(),
				L"a level asked for while the estimator runs, with nowhere to write: said, not silently off");
			std::string notice, all;
			while (env.log->TakeNotice(notice))
				all += notice + "\n";
			Assert::IsTrue(all.find("no history folder is set") != std::string::npos, L"and said once as a notice");
			env.log->Configure(kFolder, C::EvidenceLevel::Off, false, "shadow");
			Assert::AreEqual(std::string("evidence: off"), env.log->StatusText(), L"nothing requested while nothing runs: plainly off");
		}

		TEST_METHOD(AVisibleNoticeComesOnceWhenItStarts)
		{
			Env env;
			std::string text;
			Assert::IsTrue(env.log->TakeNotice(text));
			Assert::AreEqual(std::string("Source identity: evidence logging on (shadow)"), text);
			Assert::IsFalse(env.log->TakeNotice(text), L"one-time");
			env.log->Configure(kFolder, C::EvidenceLevel::Lines, true, "shadow");
			Assert::IsFalse(env.log->TakeNotice(text), L"the same configuration says nothing again");
			env.log->Configure(kFolder, C::EvidenceLevel::Lines, false, "shadow");
			env.log->Configure(kFolder, C::EvidenceLevel::Lines, true, "on");
			Assert::IsTrue(env.log->TakeNotice(text), L"off and on again: announced again");
			Assert::AreEqual(std::string("Source identity: evidence logging on (on)"), text);
			Assert::IsTrue(env.log->StatusText().find("evidence: log+lines") != std::string::npos);
		}

		TEST_METHOD(SegmentStateAndAggregateRows)
		{
			Env env(C::EvidenceLogSettings(), C::EvidenceLevel::Log);
			for (int i = 0; i <= 25; ++i)
			{
				C::LogEval e = MakeEval(static_cast<uint64_t>(i), static_cast<double>(i));
				if (i == 3)
				{
					e.axis[0].state = G::LatchState::Candidate;
					e.axis[0].stateChanged = true;
				}
				if (i == 6)
					SetLatched(e, 0, 1080, 20.0);
				env.log->OnEvaluation(e, nullptr);
			}
			env.log->Close(25000000, "test end");
			const std::string rows = env.Jsonl();
			Assert::AreEqual(2, Count(rows, "\"t\":\"state\""), L"one row per state change, full detail");
			Assert::AreEqual(2, Count(rows, "\"t\":\"segment\""), L"closed at the applied change, and at the end");
			Assert::AreEqual(2, Count(rows, "\"t\":\"agg\""), L"one aggregate row per 10 s of source time");
			Assert::IsTrue(rows.find("\"end\":\"applied change\"") != std::string::npos);
			Assert::IsTrue(rows.find("\"end\":\"test end\"") != std::string::npos);
			Assert::IsTrue(rows.find("\"t\":\"trigger\"") != std::string::npos, L"the coverage bucket was empty: a trigger row");
			Assert::IsTrue(rows.find("skipped: level log") != std::string::npos);
			Assert::AreEqual<size_t>(0, env.fs->CountWithSuffix(kFolder, ".sidl"), L"level log writes no dump");
		}

		TEST_METHOD(ATriggerDumpsTheRingOfTheLastThirtyEvaluations)
		{
			Env env;
			for (int i = 1; i <= 35; ++i)
				env.log->OnEvaluation(MakeEval(static_cast<uint64_t>(i), static_cast<double>(i)), nullptr);
			C::LogEval thin = MakeEval(36, 36.0);
			SetLatched(thin, 0, 1080, 15.5);   // c1 = 15: LLR <= c1 + 1 nat is a thin latch
			env.log->OnEvaluation(thin, nullptr);
			const std::string name = env.fs->FirstWithSuffix(kFolder, ".sidl");
			Assert::IsFalse(name.empty());
			Assert::IsTrue(name.find("thin") != std::string::npos);
			std::string header;
			std::vector<C::LineSnapshot> snaps;
			Assert::IsTrue(C::ParseDump(env.fs->Get(kFolder, name), header, snaps));
			Assert::AreEqual<size_t>(30, snaps.size());
			Assert::AreEqual<uint64_t>(7, snaps.front().seq);
			Assert::AreEqual<uint64_t>(36, snaps.back().seq);
			Assert::IsTrue(header.find("\"trigger\":\"thin") != std::string::npos);
			Assert::AreEqual<uint64_t>(1, env.log->GetStatus().dumps);
		}

		TEST_METHOD(DumpsAreRateLimitedPerGapAndPerDayButTheHotkeyIsNot)
		{
			Env env;
			auto fire = [&env](uint64_t seq, double clockMs) {
				env.clk.Set(clockMs);
				C::LogEval e = MakeEval(seq, static_cast<double>(seq));
				SetUnstable(e);
				env.log->OnEvaluation(e, nullptr);
			};
			fire(1, 0.0);
			Assert::AreEqual<uint64_t>(1, env.log->GetStatus().dumps);
			fire(2, 60000.0);   // one minute later: inside the 5 minute gap
			Assert::AreEqual<uint64_t>(1, env.log->GetStatus().dumps);
			Assert::AreEqual<uint64_t>(1, env.log->GetStatus().suppressed);
			fire(3, 301000.0);
			Assert::AreEqual<uint64_t>(2, env.log->GetStatus().dumps, L"the gap has passed");
			// The hotkey: exempt from the gap, debounced at 5 s.
			env.clk.Set(302000.0);
			env.log->RequestDump("hotkey");
			env.log->Poll();
			Assert::AreEqual<uint64_t>(3, env.log->GetStatus().dumps, L"a deliberate press is never dropped by the gap");
			env.clk.Set(303000.0);
			env.log->RequestDump("hotkey");
			env.log->Poll();
			Assert::AreEqual<uint64_t>(3, env.log->GetStatus().dumps, L"a second press within 5 s is a debounce");
			env.clk.Set(310000.0);
			env.log->RequestDump("hotkey");
			env.log->Poll();
			Assert::AreEqual<uint64_t>(4, env.log->GetStatus().dumps);

			C::EvidenceLogSettings s;
			s.triggerGapMs = 1000.0;
			s.maxPerDay = 2;
			Env day(s);
			auto fireDay = [&day](uint64_t seq, double clockMs) {
				day.clk.Set(clockMs);
				C::LogEval e = MakeEval(seq, static_cast<double>(seq));
				SetUnstable(e);
				day.log->OnEvaluation(e, nullptr);
			};
			fireDay(1, 0.0);
			fireDay(2, 2000.0);
			fireDay(3, 4000.0);
			Assert::AreEqual<uint64_t>(2, day.log->GetStatus().dumps, L"at most 2 per 24 hours here");
			Assert::AreEqual<uint64_t>(1, day.log->GetStatus().suppressed);
			fireDay(4, 24.0 * 3600.0 * 1000.0 + 10000.0);
			Assert::AreEqual<uint64_t>(3, day.log->GetStatus().dumps, L"a day later the budget is back");
		}

		TEST_METHOD(TheCapEvictsTheOldestFilesFirstAndKeepsTheOpenLog)
		{
			C::EvidenceLogSettings s;
			s.capBytes = 6000;
			s.ringEvaluations = 3;
			s.triggerGapMs = 0.0;
			s.maxPerDay = 100;
			Env env(s);
			for (int i = 1; i <= 8; ++i)
			{
				env.clk.Set(6000.0 * i);   // the hotkey is debounced at 5 s: plain evaluations, so the open log stays tiny
				env.log->RequestDump("hotkey");
				env.log->OnEvaluation(MakeEval(static_cast<uint64_t>(i), static_cast<double>(i)), nullptr);
			}
			Assert::AreEqual<uint64_t>(8, env.log->GetStatus().dumps);
			Assert::IsTrue(env.fs->TotalBytes(kFolder) <= 6000, L"the folder stays inside the cap");
			Assert::IsTrue(env.log->GetStatus().evicted > 0);
			Assert::IsTrue(env.fs->files.count(C::JoinPath(kFolder, "20260101-000000-dump-01-hotkey.sidl")) == 0, L"FIFO: the first dump is gone");
			Assert::IsTrue(env.fs->files.count(C::JoinPath(kFolder, "20260101-000000-dump-08-hotkey.sidl")) == 1, L"the newest is kept");
			Assert::IsFalse(env.fs->FirstWithSuffix(kFolder, ".jsonl").empty(), L"the open log file is never evicted");
		}

		TEST_METHOD(FramesComeOnlyAtTheFramesLevelAndAtMostFour)
		{
			for (int pass = 0; pass < 2; ++pass)
			{
				Env env(C::EvidenceLogSettings(), pass == 0 ? C::EvidenceLevel::Lines : C::EvidenceLevel::Frames);
				std::shared_ptr<std::vector<uint16_t>> plane = std::make_shared<std::vector<uint16_t>>(8u * 4u, static_cast<uint16_t>(7));
				for (int i = 1; i <= 5; ++i)
				{
					C::FrameRef f;
					f.keepAlive = plane;
					f.luma = plane->data();
					f.width = 8;
					f.height = 4;
					f.strideSamples = 8;
					f.msbShift = 6;
					f.seq = static_cast<uint64_t>(i);
					C::LogEval e = MakeEval(static_cast<uint64_t>(i), static_cast<double>(i));
					if (i == 5)
						SetUnstable(e);
					env.log->OnEvaluation(e, &f);
				}
				if (pass == 0)
					Assert::AreEqual<size_t>(0, env.fs->CountWithSuffix(kFolder, ".u16"), L"lines level: no frames");
				else
				{
					Assert::AreEqual<size_t>(4, env.fs->CountWithSuffix(kFolder, ".u16"), L"at most 4 evaluation frames");
					const std::string name = env.fs->FirstWithSuffix(kFolder, ".u16");
					const std::string data = env.fs->Get(kFolder, name);
					Assert::AreEqual<size_t>(28u + 8u * 4u * 2u, data.size());
					Assert::AreEqual(std::string("SIDF"), data.substr(0, 4));
				}
			}
		}

		TEST_METHOD(AHotkeyPressWhileTheLogIsInactiveIsRefusedWithANoticeAndNotKept)
		{
			Env env;
			env.log->Configure(kFolder, C::EvidenceLevel::Lines, false, "shadow");   // the estimator is off
			env.log->RequestDump("hotkey");
			env.log->Poll();
			std::string notice, all;
			while (env.log->TakeNotice(notice))
				all += notice + "\n";
			Assert::IsTrue(all.find("evidence dump ignored") != std::string::npos, L"a visible refusal");
			env.log->Configure(kFolder, C::EvidenceLevel::Lines, true, "shadow");
			env.log->OnEvaluation(MakeEval(1, 1.0), nullptr);
			env.log->Poll();
			Assert::AreEqual<uint64_t>(0, env.log->GetStatus().dumps, L"the old press does not fire at an unrelated later moment");
		}

		TEST_METHOD(ADebouncedOrLevelLogHotkeyPressSaysSo)
		{
			Env env(C::EvidenceLogSettings(), C::EvidenceLevel::Log);
			env.log->OnEvaluation(MakeEval(1, 1.0), nullptr);
			std::string notice, all;
			while (env.log->TakeNotice(notice))
				all += notice + "\n";
			env.clk.Set(1000.0);
			env.log->RequestDump("hotkey");
			env.log->Poll();
			while (env.log->TakeNotice(notice))
				all += notice + "\n";
			Assert::IsTrue(all.find("level log keeps no lines") != std::string::npos, L"the press at level log is said, not only a log row");
			Env lines;
			lines.log->OnEvaluation(MakeEval(1, 1.0), nullptr);
			lines.clk.Set(1000.0);
			lines.log->RequestDump("hotkey");
			lines.log->Poll();
			lines.clk.Set(2000.0);
			lines.log->RequestDump("hotkey");   // inside the 5 s debounce
			lines.log->Poll();
			all.clear();
			while (lines.log->TakeNotice(notice))
				all += notice + "\n";
			Assert::IsTrue(all.find("a dump was made a moment ago") != std::string::npos, L"a debounced press is said");
		}

		TEST_METHOD(TheHotkeyMayComeFromAnotherThread)
		{
			Env env;
			std::thread t([&env]() { env.log->RequestDump("hotkey"); });
			t.join();
			env.log->Poll();
			Assert::AreEqual<uint64_t>(0, env.log->GetStatus().dumps, L"nothing evaluated yet: nothing to dump");
			env.log->OnEvaluation(MakeEval(1, 1.0), nullptr);   // the pending request is taken here
			Assert::AreEqual<uint64_t>(1, env.log->GetStatus().dumps);
			Assert::IsTrue(env.fs->FirstWithSuffix(kFolder, ".sidl").find("hotkey") != std::string::npos);
		}

		TEST_METHOD(AFailingDiskIsReportedOncePerEpisodeAndRecovers)
		{
			Env env;
			std::string text;
			Assert::IsTrue(env.log->TakeNotice(text));   // the start notice
			auto changed = [&env](uint64_t seq, double sec) {
				C::LogEval e = MakeEval(seq, sec);
				e.axis[0].state = G::LatchState::Candidate;
				e.axis[0].stateChanged = true;   // a state row: something to write
				env.log->OnEvaluation(e, nullptr);
			};
			env.fs->failWrites = true;
			changed(1, 1.0);
			changed(2, 2.0);
			Assert::IsTrue(env.log->GetStatus().failedWrites >= 2);
			Assert::IsTrue(env.log->TakeNotice(text));
			Assert::IsTrue(text.find("write failed") != std::string::npos);
			Assert::IsFalse(env.log->TakeNotice(text), L"once per failure episode");
			Assert::IsTrue(env.log->StatusText().find("write failed") != std::string::npos);
			env.fs->failWrites = false;
			changed(3, 3.0);
			Assert::IsTrue(env.log->GetStatus().error.empty(), L"a good write ends the episode");
			Assert::IsTrue(env.log->StatusText().find("evidence: log+lines") != std::string::npos);
			Assert::IsFalse(env.fs->FirstWithSuffix(kFolder, ".jsonl").empty(), L"the rows kept for the retry were written");
		}

		TEST_METHOD(TheDumpFormatRoundTripsAndRefusesCorruptInput)
		{
			std::vector<std::shared_ptr<const C::LineSnapshot>> v;
			for (uint64_t i = 1; i <= 3; ++i)
			{
				std::shared_ptr<C::LineSnapshot> s = std::make_shared<C::LineSnapshot>(*Snap(i, static_cast<uint16_t>(10 * i)));
				s->flags = C::kEvCut;
				s->cuts = 2;
				s->eventTimeUs = 1234;
				s->streamKey = 9;
				v.push_back(s);
			}
			const std::string bytes = C::SerializeDump("{\"a\":1}", v);
			std::string header;
			std::vector<C::LineSnapshot> out;
			Assert::IsTrue(C::ParseDump(bytes, header, out));
			Assert::AreEqual(std::string("{\"a\":1}"), header);
			Assert::AreEqual<size_t>(3, out.size());
			for (size_t i = 0; i < 3; ++i)
				Assert::IsTrue(C::SnapshotHash(*v[i]) == C::SnapshotHash(out[i]), L"every field survives");
			std::string bad = bytes;
			bad[0] = 'X';
			Assert::IsFalse(C::ParseDump(bad, header, out), L"bad magic");
			bad = bytes;
			bad[4] = 9;
			Assert::IsFalse(C::ParseDump(bad, header, out), L"another version");
			Assert::IsFalse(C::ParseDump(bytes.substr(0, bytes.size() - 5), header, out), L"truncated");
			Assert::IsFalse(C::ParseDump(std::string("SIDL"), header, out));
		}

		TEST_METHOD(ACoverageBucketBelowItsTargetTriggersUntilFilled)
		{
			C::EvidenceLogSettings s;
			s.coverageTarget = 2;
			s.triggerGapMs = 0.0;
			s.maxPerDay = 100;
			Env env(s, C::EvidenceLevel::Log);
			for (int i = 1; i <= 4; ++i)
			{
				env.clk.Set(1000.0 * i);
				C::LogEval e = MakeEval(static_cast<uint64_t>(i), 10.0 * i);
				SetLatched(e, 1, 1080, 20.0);   // 1080 of 2160 = 1/2, lanczos, 8-bit: the same bucket every time; LLR 20 is not thin
				env.log->OnEvaluation(e, nullptr);
			}
			env.log->Close(40000000, "end");
			Assert::AreEqual<uint64_t>(2, env.log->GetStatus().triggers, L"the bucket needed 2 latches: the first two trigger, the rest do not");
			const std::string cov = env.fs->Get(kFolder, "coverage.txt");
			Assert::IsTrue(cov.find("1/2|lanczos|8bit|unclassified|na 4") != std::string::npos);
		}

		TEST_METHOD(AFlapPowerAndHoldTriggersFire)
		{
			{
				C::EvidenceLogSettings s;
				s.triggerGapMs = 0.0;
				s.maxPerDay = 100;
				Env env(s, C::EvidenceLevel::Log);
				C::LogEval a = MakeEval(1, 0.0);
				SetLatched(a, 0, 1080, 20.0);
				env.log->OnEvaluation(a, nullptr);
				C::LogEval b = MakeEval(2, 30.0);   // a revert 30 s after the latch
				b.axis[0].appliedChanged = true;
				b.axis[0].appliedState = G::LatchState::Latched;
				b.axis[0].appliedN = 2160;
				b.axis[0].N = 2160;
				b.axis[0].latchReason = static_cast<int>(C::LatchReason::Reverted);
				env.log->OnEvaluation(b, nullptr);
				Assert::IsTrue(env.log->GetStatus().lastTrigger.find("flap") != std::string::npos, L"a revert within 60 s of a latch");
			}
			{
				Env env(C::EvidenceLogSettings(), C::EvidenceLevel::Log);
				for (int i = 0; i <= 90; ++i)
				{
					C::LogEval e = MakeEval(static_cast<uint64_t>(i), static_cast<double>(i));
					e.axis[0].state = G::LatchState::Hold;
					env.log->OnEvaluation(e, nullptr);
				}
				Assert::AreEqual<uint64_t>(1, env.log->GetStatus().triggers, L"HOLD for more than 60 s: once per episode");
				Assert::IsTrue(env.log->GetStatus().lastTrigger.find("hold") != std::string::npos);
			}
			{
				Env env(C::EvidenceLogSettings(), C::EvidenceLevel::Log);
				for (int i = 0; i <= 84; ++i)   // 7 minutes at one evaluation per 5 s
				{
					C::LogEval e = MakeEval(static_cast<uint64_t>(i), 5.0 * i);
					for (int a = 0; a < 2; ++a)
					{
						e.axis[a].probeRan = true;
						e.axis[a].probeSeen = false;
					}
					env.log->OnEvaluation(e, nullptr);
				}
				Assert::AreEqual<uint64_t>(1, env.log->GetStatus().triggers, L"power unseen > 80% for 5 minutes: once per episode");
				Assert::IsTrue(env.log->GetStatus().lastTrigger.find("power") != std::string::npos);
			}
		}

		TEST_METHOD(ADisagreementWithTheClassicalDecisionTriggers)
		{
			C::EvidenceLogSettings s;
			s.triggerGapMs = 0.0;
			s.maxPerDay = 100;
			s.coverageTarget = 0;   // keep the coverage trigger out of the way
			Env env(s, C::EvidenceLevel::Log);
			C::LogEval e = MakeEval(1, 1.0);
			SetLatched(e, 0, 1080, 20.0);
			e.classicalValid = true;
			e.classicalRatio[0] = 1.0;   // the classical decision says native on H, the chain latched 1080 of 2160... (axis 0 N is 3840 here)
			e.axis[0].N = 2160;
			env.log->OnEvaluation(e, nullptr);
			Assert::IsTrue(env.log->GetStatus().lastTrigger.find("disagree") != std::string::npos);
			env.log->Close(1000000, "end");
			Assert::IsTrue(env.Jsonl().find("\"disagree\":true") != std::string::npos, L"the segment row flags it");
		}
	};
}
