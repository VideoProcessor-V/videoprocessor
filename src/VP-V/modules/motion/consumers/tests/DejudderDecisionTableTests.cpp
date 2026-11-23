// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5b-0 (doc 59 section 2.3): the de-judder / interpolation decision as recorded rows and sequences.
// Each row is a synthetic input (strength, rates, cadence, validity, RGB, interlaced, low latency, smooth
// motion, gap repair, vector demand, idle engine, file frames shown without motion) written as neutral
// Facts and mapped to the reason, the strength, the interpolation decision, the hold / status text and
// the log lines, recorded from the verbatim copy of today's UpdateDejudderEstimate decision part
// (DejudderDecisionLegacyCopy.h). The sequences (DejudderDecisionSequences.h) pin the throttles and the
// release across frames. P5b-3 adds the same checks for the policy runner (same Facts, its own adapter).
// Pure CPU.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/motion/consumers/DejudderAhead.h>
#include <VP-V/modules/motion/consumers/DejudderHistory.h>
#include <VP-V/modules/motion/consumers/tests/DejudderDecisionRunnerHost.h>
#include <VP-V/modules/motion/consumers/tests/DejudderDecisionSequences.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>

#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace DejudderDecisionTableTest
{
	using DejudderDecisionLegacy::Facts;
	using DejudderDecisionLegacy::Observed;
	using DejudderDecisionLegacy::Outcome;
	typedef DejudderPolicy::Reason R;
	typedef InterpolationPolicy::Reason I;
	typedef Outcome O;

	struct Expected
	{
		R reason;
		bool extrasAllowed;
		float effectiveStrength;
		int evenCycle;
		bool interpolating;
		I interpolationReason;
		double interpolationRatio;
		bool filmLike;
		bool perVsync;
		float gapPhaseScale;
		double cadenceRatio;
		bool analysisEstimate;
		O outcome;
		bool warmPendingAfter;
		bool budgetPolled;
		bool warmed;
		bool released;
		int statusPublishes;
		bool statusCleared;
		const char* firstStatusHead;
		const char* statusHead;
		const char* logs;
		unsigned long long diagModeTick;
		unsigned long long diagModeSkipped;
		const char* decisions;
	};

	struct Row
	{
		const char* name;
		void (*setup)(Facts&);
		Expected expected;
	};

	// The recorded rows' shorthands (as in the recorder).
#define DJ24 s.activeSettings.dejudderStrength=0.5; s.sourceHz=23.976; s.measuredDisplayHz=60.0; s.haveNext=true; s.nextMatches=true;
#define IP5060 s.activeSettings.interpolation=InterpolationPolicy::Mode::Auto; s.sourceHz=50.0; s.measuredDisplayHz=60.0; s.haveNext=true; s.nextMatches=true;
#define ROW(name, body, ...) { name, [](Facts& s) { (void)s; body }, __VA_ARGS__ },
	static const Row kRows[] = {
#include "DejudderDecisionRows.inl"
	};
#undef ROW
#undef IP5060
#undef DJ24

	struct RecordedSequence
	{
		const char* name;
		const char* trace;
	};
#define DECISION_SEQUENCE(name, ...) { name, __VA_ARGS__ },
	static const RecordedSequence kSequences[] = {
#include "DejudderDecisionSequenceRows.inl"
	};
#undef DECISION_SEQUENCE

	static std::wstring Wide(const std::string& v) { return std::wstring(v.begin(), v.end()); }

	static std::vector<std::string> Lines(const char* text)
	{
		std::vector<std::string> lines;
		std::istringstream in(text);
		std::string line;
		while (std::getline(in, line))
			lines.push_back(line);
		return lines;
	}

	// One row's observation against its recorded expectation (shared with P5b-3's runner check).
	static void CheckRow(const Row& row, const Observed& s, const wchar_t* who)
	{
		const Expected& e = row.expected;
		const std::wstring n = std::wstring(who) + L" " + Wide(row.name);
		std::string logs;
		for (const std::string& l : s.logs)
			logs += l + "\n";
		Assert::IsTrue(s.reason == e.reason, (n + L": reason").c_str());
		Assert::AreEqual(e.extrasAllowed, s.extrasAllowed, (n + L": extras allowed").c_str());
		Assert::IsTrue(s.effectiveStrength == e.effectiveStrength, (n + L": effective strength").c_str());
		Assert::AreEqual(e.evenCycle, s.evenCycle, (n + L": even cycle").c_str());
		Assert::AreEqual(e.interpolating, s.interpolating, (n + L": interpolating").c_str());
		Assert::IsTrue(s.interpolation.reason == e.interpolationReason, (n + L": interpolation reason").c_str());
		Assert::IsTrue(s.interpolation.ratio == e.interpolationRatio, (n + L": interpolation ratio").c_str());
		Assert::AreEqual(e.filmLike, s.interpolation.filmLike, (n + L": film-like").c_str());
		Assert::AreEqual(e.perVsync, s.interpolation.perVsync, (n + L": per-vsync").c_str());
		Assert::IsTrue(s.gapPhaseScale == e.gapPhaseScale, (n + L": gap phase scale").c_str());
		Assert::IsTrue(s.cadenceRatio == e.cadenceRatio, (n + L": cadence ratio").c_str());
		Assert::AreEqual(e.analysisEstimate, s.analysisEstimate, (n + L": analysis estimate").c_str());
		Assert::IsTrue(s.outcome == e.outcome, (n + L": outcome").c_str());
		Assert::AreEqual(e.warmPendingAfter, s.warmPendingAfter, (n + L": warm pending").c_str());
		Assert::AreEqual(e.budgetPolled, s.budgetPolled, (n + L": budget polled").c_str());
		Assert::AreEqual(e.warmed, s.warmed, (n + L": warmed").c_str());
		Assert::AreEqual(e.released, s.released, (n + L": released").c_str());
		Assert::AreEqual(e.statusPublishes, s.statusPublishes, (n + L": status publishes").c_str());
		Assert::AreEqual(e.statusCleared, s.statusCleared, (n + L": status cleared").c_str());
		Assert::AreEqual(std::string(e.firstStatusHead), s.firstStatusHead, (n + L": first publish text").c_str());
		Assert::AreEqual(std::string(e.statusHead), s.statusHead, (n + L": hold / status text").c_str());
		Assert::AreEqual(std::string(e.logs), logs, (n + L": log lines").c_str());
		std::string decisions;
		for (const std::string& d : s.decisions)
			decisions += d + "\n";
		Assert::IsTrue(s.diagModeTick == e.diagModeTick, (n + L": decision record tick").c_str());
		Assert::IsTrue(s.diagModeSkipped == e.diagModeSkipped, (n + L": decision rows skipped").c_str());
		Assert::AreEqual(std::string(e.decisions), decisions, (n + L": decision rows").c_str());
	}

	static void CheckSequence(const RecordedSequence& rec, const std::vector<std::string>& actual, const wchar_t* who)
	{
		const std::vector<std::string> expected = Lines(rec.trace);
		const std::wstring n = std::wstring(who) + L" " + Wide(rec.name);
		Assert::AreEqual(expected.size(), actual.size(), (n + L": frame count").c_str());
		for (size_t k = 0; k < expected.size(); ++k)
			Assert::AreEqual(expected[k], actual[k], (n + L": frame " + std::to_wstring(k)).c_str());
	}

	TEST_CLASS(DejudderDecisionTableTests)
	{
	public:
		TEST_METHOD(RecordedTableCoversTheDecisionSpace)
		{
			const size_t rows = sizeof(kRows) / sizeof(kRows[0]);
			Assert::IsTrue(rows >= 80, L"at least 80 recorded rows");
			// Every reason the decision can give for the de-judder appears at least once.
			bool seen[12] = {};
			for (const Row& row : kRows)
				seen[static_cast<int>(row.expected.reason)] = true;
			for (int r = 0; r < 12; ++r)
				Assert::IsTrue(seen[r], (L"a row with de-judder reason " + std::to_wstring(r)).c_str());
			const std::vector<DejudderDecisionLegacy::Sequence> sequences = DejudderDecisionLegacy::Sequences();
			Assert::AreEqual(sequences.size(), sizeof(kSequences) / sizeof(kSequences[0]), L"one recorded trace per sequence");
			for (size_t i = 0; i < sequences.size(); ++i)
				Assert::AreEqual(sequences[i].name, std::string(kSequences[i].name), L"sequence order");
		}

		TEST_METHOD(LegacyCopyReproducesEveryRecordedRow)
		{
			for (const Row& row : kRows)
			{
				Facts f;
				row.setup(f);
				CheckRow(row, DejudderDecisionLegacy::RunRowLegacy(f), L"copy");
			}
		}

		TEST_METHOD(LegacyCopyReproducesEveryRecordedSequence)
		{
			const std::vector<DejudderDecisionLegacy::Sequence> sequences = DejudderDecisionLegacy::Sequences();
			for (size_t i = 0; i < sequences.size() && i < sizeof(kSequences) / sizeof(kSequences[0]); ++i)
				CheckSequence(kSequences[i], DejudderDecisionLegacy::RunSequenceLegacy(sequences[i].base, sequences[i].steps), L"copy");
		}

		// M2 P5b-3: the MOVED decision (DejudderPolicyRunner.h, the product template) over the same Facts.
		TEST_METHOD(NewRunnerReproducesEveryRecordedRow)
		{
			for (const Row& row : kRows)
			{
				Facts f;
				row.setup(f);
				CheckRow(row, DejudderDecisionRunner::RunRow(f), L"runner");
			}
		}

		TEST_METHOD(NewRunnerReproducesEveryRecordedSequence)
		{
			const std::vector<DejudderDecisionLegacy::Sequence> sequences = DejudderDecisionLegacy::Sequences();
			for (size_t i = 0; i < sequences.size() && i < sizeof(kSequences) / sizeof(kSequences[0]); ++i)
				CheckSequence(kSequences[i], DejudderDecisionRunner::RunSequence(sequences[i].base, sequences[i].steps), L"runner");
		}
	};

	// M2 P5b-7 (doc 50 Q4): the motion-history reader action at the de-judder's apply point.
	TEST_CLASS(DejudderHistoryActionTests)
	{
	public:
		// R2 off / shadow, and on with no reader registered: exactly today's epoch decision, pending untouched.
		TEST_METHOD(OffShadowOrUnregisteredIsTheEpochPath)
		{
			for (const bool resyncOn : { false, true })
				for (const bool moved : { false, true })
					for (const bool release : { false, true })
					{
						VpMotionResync::Readers readers;   // never registered (Rearms() == 0)
						MotionValidity::Pending p;
						p.changed = moved;
						p.release = release;
						Assert::AreEqual(moved, VpMotion::TakeDejudderHistoryAction(resyncOn, moved, p, readers));
						Assert::AreEqual(release, p.release, L"pending untouched");
						Assert::IsFalse(VpMotion::HistoryReaderDrives(resyncOn, readers));
					}
		}

		// Registered but resync off: still today's path (the switch decides, not the registration).
		TEST_METHOD(RegisteredButOffIsTheEpochPath)
		{
			VpMotionResync::Readers readers;
			readers.Rearm();
			MotionValidity::Pending p;
			Assert::IsFalse(VpMotion::TakeDejudderHistoryAction(false, false, p, readers));
			Assert::IsFalse(p.release);
			Assert::IsTrue(VpMotion::HistoryReaderDrives(true, readers));
			Assert::IsFalse(VpMotion::HistoryReaderDrives(false, readers));
		}

		// On and registered with nothing pending: as the epoch path; with non-empty HistoryPending:
		// action fires once (setting release when needed) and single-consumption clears it for the next call.
		TEST_METHOD(OnRegisteredNothingPendingIsTheEpochPath)
		{
			VpMotionResync::Readers readers;
			readers.Rearm();
			for (const bool moved : { false, true })
			{
				MotionValidity::Pending p;
				p.changed = moved;
				Assert::AreEqual(moved, VpMotion::TakeDejudderHistoryAction(true, moved, p, readers));
				Assert::IsFalse(p.release);
			}
			// Non-empty HistoryPending via a real Indicator FULL event: fires once and is consumed.
			VpIndicator::Indicator ind(1, 16);
			MotionValidity::State mv;
			ind.Advance(VpIndicator::kFull, VpResync::Cause::InPlaceSourceChange, 1);
			VpIndicator::Indicator::Change ch{};
			ch.seq = 1;
			ch.changed = VpIndicator::Bit(VpIndicator::kFull);
			ch.relevant = VpIndicator::Bit(VpIndicator::kFull);
			readers.OnHistory(ind, mv, ch);
			Assert::IsTrue(readers.PeekHistory().changed, L"pending history action armed");
			MotionValidity::Pending p1;
			Assert::IsTrue(VpMotion::TakeDejudderHistoryAction(true, false, p1, readers),
				L"non-empty HistoryPending must trigger action even when epochMoved is false");
			Assert::IsTrue(p1.release, L"FULL source change must set release");
			MotionValidity::Pending p2;
			Assert::IsFalse(VpMotion::TakeDejudderHistoryAction(true, false, p2, readers),
				L"single-consumption: second call with no new event must not re-fire");
			Assert::IsFalse(p2.release);
		}

		// R21 Fix 1 (adversarial + in-test negative controls): live per-frame C (`dejudderAheadPayload`) is
		// preserved across non-release ApplyMotionInvalidate() so estimate-ahead does not spuriously fail with
		// Reason::NoAfter ("no frame after next (short queue)"), while stale prior-epoch ahead history is cleared,
		// resource-releasing invalidations MUST drop C, and invalid/stale C sequence boundaries MUST drop C.
		TEST_METHOD(LiveAheadPayloadPreservedAcrossNonReleaseInvalidateAndDroppedOnRelease)
		{
			constexpr uint64_t kA = 10;
			constexpr uint64_t kB = 11;
			constexpr uint64_t kC = 12;

			auto makeConditions = [](uint64_t source, uint64_t next, uint64_t after) {
				Dejudder::AheadSchedule::Conditions c;
				c.enabled = true;
				c.renderedWarp = true;
				c.active = true;
				c.source = source;
				c.next = next;
				c.after = after;
				c.afterGap = false;
				c.estimated = source;
				c.layoutMatches = true;
				return c;
			};

			auto makeSteps = [](int& estimateCalls, uint64_t& estA, uint64_t& estB) {
				Dejudder::AheadSchedule::Steps s;
				s.snapshot = []() { return true; };
				s.uploadAfter = []() { return true; };
				s.estimate = [&estimateCalls, &estA, &estB](uint64_t a, uint64_t b) {
					++estimateCalls;
					estA = a;
					estB = b;
					return true;
				};
				return s;
			};

			// ---- Negative Control #1 (naive wipe without preservation): prove the R21 NoAfter hazard is real ----
			{
				MotionValidity::State mv;
				uint64_t seenEpoch = mv.Epoch();
				mv.Invalidate(MotionValidity::Cause::Gap);
				const MotionValidity::Pending p = mv.Consume(seenEpoch);
				Assert::IsTrue(p.changed && !p.release && !MotionValidity::ReleasesResources(p.cause),
					L"Gap before A=10 is a non-release invalidation");

				Dejudder::AheadSchedule naiveAhead;
				int seedCalls = 0;
				uint64_t seedA = 0, seedB = 0;
				naiveAhead.NoteEpoch(MotionValidity::State::kInitialEpoch);
				Assert::IsTrue(naiveAhead.TryAhead(makeConditions(8, 9, 10), makeSteps(seedCalls, seedA, seedB)),
					L"prior-epoch ahead entry seeded");
				Assert::IsTrue(naiveAhead.Pending(), L"prior-epoch ahead entry pending before invalidate");

				// Unprotected ApplyMotionInvalidate(): resets dejudderAhead AND wipes live C (after = 0).
				naiveAhead.Reset();
				uint64_t wipedAfter = 0;
				naiveAhead.NoteEpoch(p.epoch);

				int calls = 0;
				uint64_t estA = 0, estB = 0;
				Assert::IsFalse(naiveAhead.TryAhead(makeConditions(kA, kB, wipedAfter), makeSteps(calls, estA, estB)),
					L"negative control: naive wipe without C preservation fails TryAhead");
				Assert::IsTrue(naiveAhead.LastReason() == Dejudder::AheadSchedule::Reason::NoAfter,
					L"negative control: naive wipe produces spurious Reason::NoAfter");
				Assert::AreEqual<unsigned long long>(1, naiveAhead.Count(Dejudder::AheadSchedule::Reason::NoAfter),
					L"negative control: NoAfter counted");
				Assert::AreEqual(std::string("no frame after next (short queue)"),
					std::string(Dejudder::AheadSchedule::Name(naiveAhead.LastReason())),
					L"negative control: exact R21 spurious short-queue reason");
				Assert::AreEqual(0, calls, L"negative control: estimator did not run");
			}

			// ---- Protected Path (all non-release invalidations: Gap, DriftDrop, RebaseChange, etc.) ----
			const MotionValidity::Cause nonReleaseCauses[] = {
				MotionValidity::Cause::Gap,
				MotionValidity::Cause::DriftDrop,
				MotionValidity::Cause::RebaseChange,
				MotionValidity::Cause::QueueReset,
				MotionValidity::Cause::Seek,
				MotionValidity::Cause::CadenceRepeat,
				MotionValidity::Cause::SmoothMotionMix,
				MotionValidity::Cause::MotionSourceChange,
				MotionValidity::Cause::SettingsChange,
			};
			for (const MotionValidity::Cause cause : nonReleaseCauses)
			{
				const std::wstring label = L"non-release cause " + Wide(MotionValidity::Name(cause));
				Assert::IsFalse(MotionValidity::ReleasesResources(cause), (label + L": must not release resources").c_str());

				MotionValidity::State mv;
				uint64_t seenEpoch = mv.Epoch();
				const uint64_t priorEpoch = seenEpoch;
				mv.Invalidate(cause);
				const MotionValidity::Pending p = mv.Consume(seenEpoch);
				Assert::IsTrue(p.changed && !p.release && p.epoch > priorEpoch, (label + L": epoch moved, non-release").c_str());

				Dejudder::AheadSchedule ahead;
				int seedCalls = 0;
				uint64_t seedA = 0, seedB = 0;
				ahead.NoteEpoch(priorEpoch);
				Assert::IsTrue(ahead.TryAhead(makeConditions(8, 9, 10), makeSteps(seedCalls, seedA, seedB)),
					(label + L": seed stale prior-epoch ahead entry").c_str());
				Assert::IsTrue(ahead.Pending(), (label + L": stale ahead pending before invalidate").c_str());

				// ApplyMotionInvalidate() forgets prior-epoch history first.
				ahead.Reset();
				Assert::IsFalse(ahead.Pending(), (label + L": stale prior-epoch ahead entry genuinely cleared").c_str());
				Assert::IsFalse(ahead.Consume(9, 10, priorEpoch), (label + L": stale prior-epoch pair cannot hit").c_str());

				// R21 decision: preserve live per-frame C (seq=12) strictly after B (seq=11).
				const bool preserve = VpMotion::ShouldPreserveLiveAheadAfterInvalidate(
					p.release, /*hadLiveAhead=*/true, kC, /*hasNext=*/true, kB);
				Assert::IsTrue(preserve, (label + L": live C strictly after B must be preserved").c_str());

				const uint64_t afterSeq = preserve ? kC : 0;
				ahead.NoteEpoch(p.epoch);
				int calls = 0;
				uint64_t estA = 0, estB = 0;
				Assert::IsTrue(ahead.TryAhead(makeConditions(kA, kB, afterSeq), makeSteps(calls, estA, estB)),
					(label + L": TryAhead succeeds with preserved C").c_str());
				Assert::IsTrue(ahead.LastReason() == Dejudder::AheadSchedule::Reason::Ahead,
					(label + L": reason is Ahead").c_str());
				Assert::AreEqual<unsigned long long>(0, ahead.Count(Dejudder::AheadSchedule::Reason::NoAfter),
					(label + L": zero spurious NoAfter").c_str());
				Assert::AreEqual(1, calls, (label + L": estimated once").c_str());
				Assert::IsTrue(estA == kB && estB == kC, (label + L": estimated live (B, C) pair").c_str());
				Assert::IsTrue(ahead.Consume(kB, kC, p.epoch), (label + L": live (B, C) hits in new epoch").c_str());
			}

			// Reader-driven rebase-only (no epoch move, forgetRebaseHolders = true, release = false) also preserves C.
			{
				auto ind = std::make_shared<VpIndicator::Indicator>(1, 64);
				MotionValidity::State mv;
				VpMotionResync::Readers readers;
				VpMotionResync::RegisterOn(ind, mv, readers);
				ind->SealAndObserve(9);   // latch baseline
				ind->Advance(VpIndicator::kRebaseH, VpResync::Cause::None, kA);
				ind->SealAndObserve(kA);
				MotionValidity::Pending p;
				Assert::IsTrue(VpMotion::TakeDejudderHistoryAction(/*resyncOn=*/true, /*epochMoved=*/false, p, readers),
					L"reader rebase-only triggers forget action");
				Assert::IsFalse(p.release, L"reader rebase-only is non-release");
				Assert::IsTrue(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(p.release, true, kC, true, kB),
					L"reader rebase-only preserves live C");
			}

			// ---- Negative Control #2 (Release invalidations MUST NOT preserve C) ----
			const MotionValidity::Cause releaseCauses[] = {
				MotionValidity::Cause::SourceFormatChange,
				MotionValidity::Cause::Resync,
			};
			for (const MotionValidity::Cause cause : releaseCauses)
			{
				const std::wstring label = L"release cause " + Wide(MotionValidity::Name(cause));
				Assert::IsTrue(MotionValidity::ReleasesResources(cause), (label + L": must release resources").c_str());

				MotionValidity::State mv;
				uint64_t seenEpoch = mv.Epoch();
				mv.Invalidate(cause);
				const MotionValidity::Pending p = mv.Consume(seenEpoch);
				Assert::IsTrue(p.changed && p.release, (label + L": pending.release is true").c_str());

				const bool preserve = VpMotion::ShouldPreserveLiveAheadAfterInvalidate(
					p.release, /*hadLiveAhead=*/true, kC, /*hasNext=*/true, kB);
				Assert::IsFalse(preserve, (label + L": release invalidation MUST NOT preserve C").c_str());

				Dejudder::AheadSchedule ahead;
				ahead.NoteEpoch(p.epoch);
				int calls = 0;
				uint64_t estA = 0, estB = 0;
				Assert::IsFalse(ahead.TryAhead(makeConditions(kA, kB, preserve ? kC : 0), makeSteps(calls, estA, estB)),
					(label + L": TryAhead refuses after release dropped C").c_str());
				Assert::IsTrue(ahead.LastReason() == Dejudder::AheadSchedule::Reason::NoAfter,
					(label + L": Reason::NoAfter after release dropped C").c_str());
				Assert::AreEqual(0, calls, (label + L": estimator did not run on released payload").c_str());
			}

			// Coalesced release + non-release (SourceFormatChange then Gap before Consume): release bit is sticky!
			{
				MotionValidity::State mv;
				uint64_t seenEpoch = mv.Epoch();
				mv.Invalidate(MotionValidity::Cause::SourceFormatChange);
				mv.Invalidate(MotionValidity::Cause::Gap);
				const MotionValidity::Pending p = mv.Consume(seenEpoch);
				Assert::IsTrue(p.changed && p.release && p.cause == MotionValidity::Cause::Gap,
					L"coalesced FormatChange + Gap preserves release=true even though LastCause is Gap");
				Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(p.release, true, kC, true, kB),
					L"coalesced release MUST NOT preserve C");
			}

			// Reader-driven FULL release (SourceFormatChanged / RendererReset) via TakeDejudderHistoryAction: MUST NOT preserve C.
			for (const VpResync::Cause fullCause : { VpResync::Cause::SourceFormatChanged, VpResync::Cause::RendererReset })
			{
				auto ind = std::make_shared<VpIndicator::Indicator>(2, 64);
				MotionValidity::State mv;
				VpMotionResync::Readers readers;
				VpMotionResync::RegisterOn(ind, mv, readers);
				ind->SealAndObserve(9);
				ind->Advance(VpIndicator::kFull, fullCause, kA);
				ind->SealAndObserve(kA);
				MotionValidity::Pending p;
				Assert::IsTrue(VpMotion::TakeDejudderHistoryAction(/*resyncOn=*/true, /*epochMoved=*/false, p, readers),
					L"reader FULL release triggers action");
				Assert::IsTrue(p.release, L"reader FULL release raises p.release");
				Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(p.release, true, kC, true, kB),
					L"reader-driven release MUST NOT preserve C");
			}

			// Direct released = true (e.g. ReleaseDejudder() cleared dejudder / dejudderLayout): MUST NOT preserve C.
			Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(/*released=*/true, true, kC, true, kB),
				L"explicit released=true MUST NOT preserve C");

			// ---- Negative Control #3 (Invalid / stale C boundaries MUST NOT preserve C even when released=false) ----
			Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, /*hadLiveAhead=*/false, kC, true, kB),
				L"null liveAhead payload must not preserve");
			Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, true, /*liveAheadSeq=*/0, true, kB),
				L"liveAheadSeq == 0 must not preserve");
			Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, true, kC, /*hasNext=*/false, kB),
				L"missing next payload (hasNext == false) must not preserve");
			Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, true, kC, true, /*nextSeq=*/0),
				L"nextSeq == 0 must not preserve");
			Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, true, 0, true, 0),
				L"both sequences 0 must not preserve");
			Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, true, /*liveAheadSeq=*/kB, true, kB),
				L"stale C == B must not preserve");
			Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, true, /*liveAheadSeq=*/kA, true, kB),
				L"older epoch C == A < B must not preserve");
			Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, true, /*liveAheadSeq=*/1, true, kB),
				L"older epoch C < B must not preserve");
			constexpr uint64_t kMaxSeq = (std::numeric_limits<uint64_t>::max)();
			Assert::IsFalse(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, true, kMaxSeq, true, kMaxSeq),
				L"max uint64 C == B must not preserve");
			Assert::IsTrue(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, true, kMaxSeq, true, kMaxSeq - 1),
				L"max uint64 C > B preserves without overflow");
			Assert::IsTrue(VpMotion::ShouldPreserveLiveAheadAfterInvalidate(false, true, 2, true, 1),
				L"minimal valid positive boundary (B=1, C=2) preserves");
		}
	};
}
