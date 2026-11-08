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

#include <VP-V/modules/motion/consumers/tests/DejudderDecisionRunnerHost.h>
#include <VP-V/modules/motion/consumers/tests/DejudderDecisionSequences.h>

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
}
