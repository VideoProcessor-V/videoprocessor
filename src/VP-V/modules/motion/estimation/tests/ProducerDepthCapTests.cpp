// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Slice 16 D2 (refs/105 s3.4 / s5): ph3 deadline cap, skip-keep-luma, non-continuous next pair,
// lead clamped to kMaxLead (never kRingMaxLead), unknown period = no skip, history-behind-render
// guard, and sustained-behind visibility.
#include "pch.h"
#include <VP-V/modules/motion/estimation/MotionAhead.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(ProducerDepthCapTests)
	{
	public:
		TEST_METHOD(DeadlineBudgetUsesAheadCapNeverRingMaxLeadAndUnknownPeriodHasNoDeadline)
		{
			const auto t0 = std::chrono::steady_clock::now();
			// Unknown period (0 or negative or NaN) -> zero deadline (never skips).
			Assert::AreEqual(0.0, MotionAhead::DeadlineBudgetMs(4, 0.0), 1e-9);
			Assert::AreEqual(0.0, MotionAhead::DeadlineBudgetMs(4, -16.67), 1e-9);
			const auto dUnknown = MotionAhead::DeadlineFor(t0, 4, 0.0);
			Assert::AreEqual<long long>(0, dUnknown.time_since_epoch().count());
			Assert::IsFalse(MotionAhead::PastDeadline(t0 + std::chrono::hours(1), dUnknown), L"unknown period never expires");

			// Lead above kMaxLead (e.g. kRingMaxLead = 13) is clamped to kMaxLead (6).
			Assert::IsTrue(MotionAhead::kRingMaxLead > MotionAhead::kMaxLead);
			const double capBudget = MotionAhead::DeadlineBudgetMs(MotionAhead::kMaxLead, 16.67);
			const double ringBudget = MotionAhead::DeadlineBudgetMs(MotionAhead::kRingMaxLead, 16.67);
			Assert::AreEqual(capBudget, ringBudget, 1e-9, L"lead clamped to kMaxLead, never kRingMaxLead");
			Assert::AreEqual(static_cast<double>(MotionAhead::kMaxLead) * 16.67, ringBudget, 1e-9);
			Assert::AreEqual(16.67, MotionAhead::DeadlineBudgetMs(0, 16.67), 1e-9, L"minimum lead of 1");
		}

		TEST_METHOD(NoSkipWhenSlackIsPositiveOrPeriodUnknown)
		{
			MotionAhead::Producer producer;
			auto seam = std::make_shared<MotionAhead::Producer::TestEngine>();
			std::atomic<int> calls{ 0 };
			seam->job = [&](uint64_t, uint64_t) {
				++calls;
				return true;
			};
			Assert::IsTrue(producer.StartForTest(seam));
			producer.SetDepth(4);
			producer.SetLead(4);
			producer.SetSourcePeriodMs(1000.0);   // 4 s deadline: plenty of slack

			AnalysisLumaSource empty;
			for (uint64_t s = 1; s <= 5; ++s)
				producer.Submit(s, 1, empty);
			const auto waitUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (producer.GetStats().pairs < 4 && std::chrono::steady_clock::now() < waitUntil)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			const auto st = producer.GetStats();
			Assert::AreEqual(4ull, st.pairs);
			Assert::AreEqual(0ull, st.late, L"positive slack skips nothing");
			Assert::IsFalse(st.behind);
			Assert::AreEqual(5, calls.load());

			// Unknown period (0): even behind a slow job, nothing is skipped.
			producer.SetSourcePeriodMs(0.0);
			producer.Submit(6, 1, empty);
			producer.Submit(7, 1, empty);
			const auto waitUntil2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (producer.GetStats().pairs < 6 && std::chrono::steady_clock::now() < waitUntil2)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			Assert::AreEqual(6ull, producer.GetStats().pairs);
			Assert::AreEqual(0ull, producer.GetStats().late, L"unknown period skips nothing");
			producer.Stop();
		}

		TEST_METHOD(LateCoreJobSkipsRunKeepsLumaMarksNextPairNonContinuousAndSignalsBehind)
		{
			MotionAhead::Producer producer;
			std::mutex m;
			std::condition_variable cv;
			bool inJob2 = false;
			bool releaseJob2 = false;
			std::vector<uint64_t> ranSeqs;
			std::map<uint64_t, bool> continuousBySeq;

			auto seam = std::make_shared<MotionAhead::Producer::TestEngine>();
			seam->onPairContinuous = [&](uint64_t seq, bool cont) {
				std::lock_guard<std::mutex> lock(m);
				continuousBySeq[seq] = cont;
			};
			seam->job = [&](uint64_t seq, uint64_t) {
				{
					std::lock_guard<std::mutex> lock(m);
					ranSeqs.push_back(seq);
				}
				if (seq == 2)
				{
					std::unique_lock<std::mutex> lock(m);
					inJob2 = true;
					cv.notify_all();
					cv.wait_for(lock, std::chrono::seconds(5), [&] { return releaseJob2; });
				}
				return true;
			};
			Assert::IsTrue(producer.StartForTest(seam));
			producer.SetDepth(1);
			producer.SetLead(1);
			producer.SetSourcePeriodMs(15.0);   // 1 * 15 ms deadline

			AnalysisLumaSource empty;
			producer.Submit(1, 1, empty);
			producer.Submit(2, 1, empty);
			{
				std::unique_lock<std::mutex> lock(m);
				Assert::IsTrue(cv.wait_for(lock, std::chrono::seconds(5), [&] { return inJob2; }));
			}
			// Submit frame 3 while job 2 blocks; sleep 45 ms so job 2's service time (> 45 ms > 15 ms period)
			// and job 3's deadline (15 ms) both expire.
			producer.Submit(3, 1, empty);
			std::this_thread::sleep_for(std::chrono::milliseconds(45));
			{
				std::lock_guard<std::mutex> lock(m);
				releaseJob2 = true;
			}
			cv.notify_all();

			// Wait until frame 3 has been processed (late skipped, frames == 3, late == 1).
			const auto w1 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (producer.GetStats().frames < 3 && std::chrono::steady_clock::now() < w1)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));

			const auto afterLate = producer.GetStats();
			Assert::AreEqual(3ull, afterLate.frames);
			Assert::AreEqual(1ull, afterLate.pairs, L"pair (1,2) published; late (2,3) skipped");
			Assert::AreEqual(1ull, afterLate.late, L"frame 3 counted late");
			Assert::IsTrue(afterLate.behind, L"service EMA > measured period with late > 0 signals behind");
			Assert::IsTrue(producer.Take(2, 3) == nullptr, L"skipped pair (2,3) was not published");

			// Now submit frame 4 with plenty of slack (fresh deadline): because frame 3 KEPT its luma/chain state,
			// pair (3,4) MUST form and publish, AND must run non-continuous (hints reset by the late skip).
			producer.SetSourcePeriodMs(500.0);
			producer.Submit(4, 1, empty);
			producer.Submit(5, 1, empty);
			const auto w2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (producer.GetStats().pairs < 3 && std::chrono::steady_clock::now() < w2)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));

			Assert::IsTrue(producer.Take(3, 4) != nullptr, L"(3,4) survives because late frame 3 kept its luma");
			Assert::IsTrue(producer.Take(4, 5) != nullptr, L"(4,5) follows normally");
			{
				std::lock_guard<std::mutex> lock(m);
				// Frame 3 was skipped so job() was never called for seq 3.
				Assert::IsTrue(std::find(ranSeqs.begin(), ranSeqs.end(), 3ull) == ranSeqs.end(), L"late frame 3 skipped Run");
				Assert::IsFalse(continuousBySeq[4], L"pair (3,4) after late skip runs non-continuous");
				Assert::IsTrue(continuousBySeq[5], L"subsequent pair (4,5) resumes continuous hints");
			}
			producer.Stop();
		}

		TEST_METHOD(HistoryBehindRenderGuardPreventsLateSkipping)
		{
			MotionAhead::Producer producer;
			std::mutex m;
			std::condition_variable cv;
			bool inJob2 = false;
			bool releaseJob2 = false;
			auto seam = std::make_shared<MotionAhead::Producer::TestEngine>();
			seam->job = [&](uint64_t seq, uint64_t) {
				if (seq == 2)
				{
					std::unique_lock<std::mutex> lock(m);
					inJob2 = true;
					cv.notify_all();
					cv.wait_for(lock, std::chrono::seconds(5), [&] { return releaseJob2; });
				}
				return true;
			};
			Assert::IsTrue(producer.StartForTest(seam));
			producer.SetDepth(1);
			producer.SetLead(1);
			producer.SetSourcePeriodMs(10.0);
			producer.SetHistoryBehindRender(true);   // Owner item 1 guard

			AnalysisLumaSource empty;
			producer.Submit(1, 1, empty);
			producer.Submit(2, 1, empty);
			{
				std::unique_lock<std::mutex> lock(m);
				Assert::IsTrue(cv.wait_for(lock, std::chrono::seconds(5), [&] { return inJob2; }));
			}
			producer.Submit(3, 1, empty);
			std::this_thread::sleep_for(std::chrono::milliseconds(35));
			{
				std::lock_guard<std::mutex> lock(m);
				releaseJob2 = true;
			}
			cv.notify_all();
			const auto w = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (producer.GetStats().pairs < 2 && std::chrono::steady_clock::now() < w)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			Assert::AreEqual(2ull, producer.GetStats().pairs, L"pair (2,3) computed because historyBehindRender guard is active");
			Assert::AreEqual(0ull, producer.GetStats().late);
			Assert::IsTrue(producer.Take(2, 3) != nullptr);
			producer.Stop();
		}
	};
}
