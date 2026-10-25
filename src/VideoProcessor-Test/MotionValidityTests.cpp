#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/MotionValidity.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// docs/VP-interpolator-50-60-plan.md step 1 (owner 2026-10-14, [[vp-vector-validity]]):
// one vectors-valid state, invalidated at every discontinuity; every consumer of
// the vectors falls back until a fresh contiguous pair in the current epoch.
namespace MotionValidityTest
{
	using MotionValidity::Cause;
	using MotionValidity::Continuity;

	// A local copy: passing the static constexpr member by reference would odr-use it (C++14).
	static const uint64_t kInitial = MotionValidity::State::kInitialEpoch;
	static const size_t kCauses = MotionValidity::kCauseCount;

	static const Cause kAllCauses[] = {
		Cause::Resync, Cause::QueueReset, Cause::Gap, Cause::CadenceRepeat, Cause::DriftDrop,
		Cause::SmoothMotionMix, Cause::RebaseChange, Cause::SourceFormatChange, Cause::Seek,
		Cause::MotionSourceChange, Cause::SettingsChange,
	};

	TEST_CLASS(MotionValidityTests)
	{
	public:
		TEST_METHOD(EveryCauseMovesTheEpochAndIsCountedOnItsOwn)
		{
			Assert::AreEqual(kCauses, sizeof(kAllCauses) / sizeof(kAllCauses[0]));
			MotionValidity::State state;
			Assert::AreEqual(kInitial, state.Epoch());
			uint64_t expectedEpoch = state.Epoch();
			for (size_t i = 0; i < MotionValidity::kCauseCount; ++i)
			{
				const Cause cause = kAllCauses[i];
				const uint64_t before = state.Epoch();
				// A pair taken just before the invalidation...
				Assert::IsTrue(state.PairValid(10, 11, before, before));
				const uint64_t after = state.Invalidate(cause);
				++expectedEpoch;
				Assert::AreEqual(expectedEpoch, after);
				Assert::AreEqual(after, state.Epoch());
				Assert::AreEqual(state.Epoch(), state.Generation(), L"the motion generation is the epoch");
				// ...is stale after it, whatever the cause.
				const std::string name = MotionValidity::Name(cause);
				const std::wstring message = L"stale after " + std::wstring(name.begin(), name.end());
				Assert::IsFalse(state.PairValid(10, 11, before, before), message.c_str());
				Assert::IsTrue(state.LastCause() == cause);
				for (size_t j = 0; j < MotionValidity::kCauseCount; ++j)
					Assert::AreEqual(j <= i ? 1ull : 0ull, static_cast<unsigned long long>(state.Count(kAllCauses[j])));
			}
			Assert::AreEqual(static_cast<unsigned long long>(MotionValidity::kCauseCount),
				static_cast<unsigned long long>(state.Total()));
		}

		TEST_METHOD(OnlyResyncAndFormatChangeRelease)
		{
			for (Cause cause : kAllCauses)
				Assert::AreEqual(cause == Cause::Resync || cause == Cause::SourceFormatChange,
					MotionValidity::ReleasesResources(cause));
		}

		TEST_METHOD(TheStreamGenerationMovesOnlyOnStreamRestarts)
		{
			MotionValidity::State state;
			uint64_t expected = kInitial;
			Assert::AreEqual(expected, state.StreamGeneration(), L"today's motionGeneration starts at 1");
			for (Cause cause : kAllCauses)
			{
				state.Invalidate(cause);
				if (cause == Cause::Resync || cause == Cause::QueueReset || cause == Cause::Seek ||
					cause == Cause::SourceFormatChange)
					++expected;
				Assert::AreEqual(expected, state.StreamGeneration());
			}
			Assert::AreEqual(kInitial + 4, state.StreamGeneration());
			Assert::AreEqual(kInitial + MotionValidity::kCauseCount, state.Epoch());
		}

		TEST_METHOD(ConsumeReportsTheChangeOnceWithReleaseAndCause)
		{
			MotionValidity::State state;
			uint64_t seen = state.Epoch();
			Assert::IsFalse(state.Consume(seen).changed, L"nothing happened");

			state.Invalidate(Cause::Gap);
			auto p = state.Consume(seen);
			Assert::IsTrue(p.changed);
			Assert::IsFalse(p.release, L"a gap forgets history, it does not release");
			Assert::IsTrue(p.cause == Cause::Gap);
			Assert::AreEqual(1ull, static_cast<unsigned long long>(p.invalidations));
			Assert::IsFalse(state.Consume(seen).changed, L"reported once");

			state.Invalidate(Cause::Gap);
			state.Invalidate(Cause::Resync);
			state.Invalidate(Cause::DriftDrop);
			p = state.Consume(seen);
			Assert::IsTrue(p.changed);
			Assert::IsTrue(p.release, L"a resync among them releases");
			Assert::IsTrue(p.cause == Cause::DriftDrop, L"the most recent cause, for the log");
			Assert::AreEqual(3ull, static_cast<unsigned long long>(p.invalidations));
			Assert::AreEqual(state.Epoch(), seen);

			state.Invalidate(Cause::Seek);
			p = state.Consume(seen);
			Assert::IsTrue(p.changed);
			Assert::IsFalse(p.release, L"the release was taken already");
		}

		TEST_METHOD(APairIsValidOnlyInsideOneCurrentEpochAndAdjacent)
		{
			MotionValidity::State state;
			const uint64_t e1 = state.Epoch();
			Assert::IsTrue(state.PairValid(5, 6, e1, e1));
			Assert::IsFalse(state.PairValid(5, 7, e1, e1), L"a hole: not adjacent");
			Assert::IsFalse(state.PairValid(6, 6, e1, e1), L"a repeat is not a pair");
			Assert::IsFalse(state.PairValid(6, 5, e1, e1), L"backwards");
			Assert::IsFalse(state.PairValid(0, 1, e1, e1), L"sequence 0 is none");
			Assert::IsFalse(state.PairValid(5, 6, e1, e1, false), L"the caller's break (gapBefore)");
			Assert::IsFalse(state.PairValid(5, 6, 0, 0), L"epoch 0 is none");

			const uint64_t e2 = state.Invalidate(Cause::Gap);
			Assert::IsFalse(state.PairValid(5, 6, e1, e2), L"across the invalidation");
			Assert::IsFalse(state.PairValid(5, 6, e1, e1), L"the old epoch");
			Assert::IsTrue(state.PairValid(7, 8, e2, e2), L"a fresh pair in the new epoch");
			Assert::IsFalse(state.PairValid(7, 8, e2 + 1, e2 + 1), L"a future epoch is not current");
		}

		TEST_METHOD(KeptRunExcludesRepeatsAndBreaksOnHoles)
		{
			MotionValidity::State state;
			Assert::IsTrue(state.NoteKept(100) == Continuity::First);
			Assert::IsTrue(state.NoteKept(101) == Continuity::Contiguous);
			Assert::IsTrue(state.KeptPairValid(100, 101));
			// 3:2-style cadence repeats: the same sequence again, excluded.
			Assert::IsTrue(state.NoteKept(101) == Continuity::Repeat);
			Assert::IsTrue(state.NoteKept(101) == Continuity::Repeat);
			Assert::IsTrue(state.KeptPairValid(100, 101), L"a repeat does not break the pair");
			Assert::IsTrue(state.NoteKept(102) == Continuity::Contiguous, L"adjacent after the repeats");
			Assert::IsTrue(state.KeptPairValid(101, 102));
			Assert::AreEqual(2ull, static_cast<unsigned long long>(state.Repeats()));
			Assert::AreEqual(0ull, static_cast<unsigned long long>(state.Total()), L"repeats are not invalidations");

			// A drift correction drop: 103 never shown.
			const uint64_t before = state.Epoch();
			Assert::IsTrue(state.NoteKept(104, Cause::DriftDrop) == Continuity::Break);
			Assert::AreEqual(before + 1, state.Epoch());
			Assert::AreEqual(1ull, static_cast<unsigned long long>(state.Count(Cause::DriftDrop)));
			Assert::IsFalse(state.KeptPairValid(102, 104));
			Assert::IsFalse(state.KeptPairValid(103, 104));
			Assert::IsFalse(state.PairValid(102, 103, before, before), L"pairs from before the drop are stale");
			Assert::IsTrue(state.NoteKept(105) == Continuity::Contiguous);
			Assert::IsTrue(state.KeptPairValid(104, 105), L"the new run pairs again");
			Assert::AreEqual(state.Epoch(), state.EpochOfLastKept());

			// A step back (a new queue generation reusing nothing) also breaks.
			Assert::IsTrue(state.NoteKept(50) == Continuity::Break);
			Assert::AreEqual(1ull, static_cast<unsigned long long>(state.Count(Cause::Gap)));
			Assert::IsTrue(state.NoteKept(0) == Continuity::First, L"sequence 0 is ignored");
			Assert::AreEqual(50ull, static_cast<unsigned long long>(state.LastKept()));
		}

		TEST_METHOD(AnInvalidationRestartsTheKeptRunWithoutAnotherCount)
		{
			MotionValidity::State state;
			state.NoteKept(10);
			state.NoteKept(11);
			state.Invalidate(Cause::Resync);
			Assert::IsFalse(state.KeptPairValid(10, 11), L"the run before the resync is stale");
			Assert::IsTrue(state.NoteKept(11) == Continuity::Repeat, L"the same frame shown again is its repeat");
			Assert::IsTrue(state.NoteKept(12) == Continuity::Contiguous);
			Assert::IsTrue(state.KeptPairValid(11, 12), L"a fresh pair in the new epoch");
			state.Invalidate(Cause::Seek);
			Assert::IsTrue(state.NoteKept(500) == Continuity::First, L"already invalidated: a new run, not a break");
			Assert::AreEqual(2ull, static_cast<unsigned long long>(state.Total()));
		}

		TEST_METHOD(TheGateFallsBackOnEveryCauseUntilRebound)
		{
			MotionValidity::State state;
			MotionValidity::ConsumerGate gate;
			Assert::IsFalse(gate.Bound());
			Assert::IsFalse(gate.Usable(state, 1, 2), L"nothing bound: fallback");
			for (Cause cause : kAllCauses)
			{
				gate.Bind(state, 20, 21);
				Assert::IsTrue(gate.Check(state, 20, 21));
				Assert::IsTrue(gate.UsableFrom(state, 20));
				Assert::IsTrue(gate.UsableAt(state, 21));
				Assert::IsFalse(gate.Usable(state, 21, 22), L"another pair");
				Assert::IsFalse(gate.UsableAt(state, 20));
				state.Invalidate(cause);
				Assert::IsFalse(gate.Check(state, 20, 21), L"stale after the cause");
				Assert::IsFalse(gate.CheckAt(state, 21));
			}
			Assert::AreEqual(static_cast<unsigned long long>(MotionValidity::kCauseCount),
				static_cast<unsigned long long>(gate.Uses()));
			Assert::AreEqual(static_cast<unsigned long long>(2 * MotionValidity::kCauseCount),
				static_cast<unsigned long long>(gate.Fallbacks()));
			// Bound to an epoch captured before the invalidation (inputs taken earlier).
			const uint64_t taken = state.Epoch();
			state.Invalidate(Cause::Gap);
			gate.Bind(30, 31, taken);
			Assert::IsFalse(gate.Usable(state, 30, 31), L"made from inputs taken before the gap");
			gate.Clear();
			Assert::IsFalse(gate.Bound());
		}

		TEST_METHOD(DescribeStringsAreDistinctAndNamed)
		{
			std::vector<std::string> names, phrases;
			for (Cause cause : kAllCauses)
			{
				const std::string name = MotionValidity::Name(cause);
				const std::string phrase = MotionValidity::Describe(cause);
				Assert::IsFalse(name.empty());
				Assert::IsFalse(phrase.empty());
				Assert::AreNotEqual(std::string("unknown"), name);
				Assert::AreNotEqual(std::string("unknown cause"), phrase);
				for (const auto& n : names) Assert::AreNotEqual(n, name);
				for (const auto& p : phrases) Assert::AreNotEqual(p, phrase);
				names.push_back(name);
				phrases.push_back(phrase);
			}
			Assert::AreEqual(std::string("resync or source change"), std::string(MotionValidity::Describe(Cause::Resync)));
			Assert::AreEqual(std::string("frame gap"), std::string(MotionValidity::Describe(Cause::Gap)));
			Assert::AreEqual(std::string("live queue reset"), std::string(MotionValidity::Describe(Cause::QueueReset)));
			Assert::AreEqual(std::string("drift_drop"), std::string(MotionValidity::Name(Cause::DriftDrop)));
			Assert::AreEqual(std::string("unknown"), std::string(MotionValidity::Name(static_cast<Cause>(99))));
			Assert::AreEqual(std::string("contiguous"), std::string(MotionValidity::Describe(Continuity::Contiguous)));
			MotionValidity::State state;
			const uint64_t e = state.Epoch();
			Assert::AreEqual(e, state.Invalidate(static_cast<Cause>(99)), L"an unknown cause is ignored");
			Assert::AreEqual(0ull, static_cast<unsigned long long>(state.Total()));
		}

		// Invalidations from "capture" and "UI" threads while a "render" thread keeps a
		// run and checks pairs and a "producer" thread checks stored pairs: no torn
		// state, every invalidation counted, and no pair taken before an invalidation
		// is ever valid after it.
		TEST_METHOD(ConcurrentInvalidateAndCheckStress)
		{
			MotionValidity::State state;
			std::atomic<bool> stop{ false };
			std::atomic<unsigned long long> staleAccepted{ 0 }, checks{ 0 };
			constexpr int kInvalidationsPerThread = 2000;

			auto invalidator = [&](Cause a, Cause b)
			{
				for (int i = 0; i < kInvalidationsPerThread; ++i)
				{
					state.Invalidate((i & 1) ? a : b);
					if ((i & 63) == 0) std::this_thread::yield();
				}
			};
			std::thread render([&]
			{
				uint64_t sequence = 1;
				uint64_t seen = state.Epoch();
				uint64_t older = state.Epoch();   // the epoch a pair was taken in, some iterations ago
				while (!stop.load(std::memory_order_acquire))
				{
					state.NoteKept(sequence);
					const uint64_t now = state.Epoch();
					// The epoch only moves forward: once it has been seen past `older`, a pair
					// taken in `older` must never be valid again.
					if (now != older && state.PairValid(sequence, sequence + 1, older, older)) staleAccepted.fetch_add(1);
					if ((sequence & 15) == 0) older = now;
					state.Consume(seen);
					++sequence;
					checks.fetch_add(1, std::memory_order_relaxed);
				}
			});
			std::thread producer([&]
			{
				MotionValidity::ConsumerGate gate;
				uint64_t from = 1000000;
				while (!stop.load(std::memory_order_acquire))
				{
					const uint64_t made = state.Epoch();
					gate.Bind(from, from + 1, made);
					// Once the epoch has moved on, the binding must never be usable again.
					if (state.Epoch() != made && gate.Usable(state, from, from + 1)) staleAccepted.fetch_add(1);
					++from;
					checks.fetch_add(1, std::memory_order_relaxed);
				}
			});
			std::thread capture(invalidator, Cause::Gap, Cause::DriftDrop);
			std::thread ui(invalidator, Cause::Resync, Cause::QueueReset);
			capture.join();
			ui.join();
			stop.store(true, std::memory_order_release);
			render.join();
			producer.join();

			Assert::AreEqual(0ull, staleAccepted.load());
			Assert::IsTrue(checks.load() > 0);
			const uint64_t explicitCount = static_cast<uint64_t>(4) * (kInvalidationsPerThread / 2);
			Assert::AreEqual(static_cast<unsigned long long>(kInvalidationsPerThread / 2),
				static_cast<unsigned long long>(state.Count(Cause::DriftDrop)));
			Assert::AreEqual(static_cast<unsigned long long>(kInvalidationsPerThread / 2),
				static_cast<unsigned long long>(state.Count(Cause::Resync)));
			Assert::AreEqual(static_cast<unsigned long long>(kInvalidationsPerThread / 2),
				static_cast<unsigned long long>(state.Count(Cause::QueueReset)));
			// Gap: the capture thread's plus any the render thread's NoteKept found (none:
			// its sequence is always contiguous), so exactly the explicit ones.
			Assert::AreEqual(static_cast<unsigned long long>(kInvalidationsPerThread / 2),
				static_cast<unsigned long long>(state.Count(Cause::Gap)));
			Assert::AreEqual(static_cast<unsigned long long>(explicitCount), static_cast<unsigned long long>(state.Total()));
			Assert::AreEqual(kInitial + explicitCount, state.Epoch(),
				L"every invalidation moved the epoch exactly once");
		}
	};
}
