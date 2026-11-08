// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5b-0 (doc 59 section 2.3): the de-judder / interpolation decision as recorded rows. Each row is a
// synthetic input (strength, rates, cadence, validity, RGB, interlaced, low latency, smooth motion,
// gap repair, vector demand, idle engine, file frames shown without motion) mapped to the reason, the
// strength, the interpolation decision and the hold / status text, recorded from the verbatim copy of
// today's UpdateDejudderEstimate decision part (DejudderDecisionLegacyCopy.h). P5b-3 moves the decision
// into the policy runner; these rows must stay identical. Pure CPU.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/motion/consumers/tests/DejudderDecisionLegacyCopy.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace DejudderDecisionTableTest
{
	using DejudderDecisionLegacy::Outcome;
	using DejudderDecisionLegacy::State;
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
		const char* statusHead;
		const char* logs;
	};

	struct Row
	{
		const char* name;
		void (*setup)(State&);
		Expected expected;
	};

	// The recorded rows' shorthands (as in the recorder).
#define DJ24 s.activeSettings.dejudderStrength=0.5; s.sourceHz=23.976; s.measuredDisplayHz=60.0; s.haveNext=true; s.nextMatches=true;
#define IP5060 s.activeSettings.interpolation=InterpolationPolicy::Mode::Auto; s.sourceHz=50.0; s.measuredDisplayHz=60.0; s.haveNext=true; s.nextMatches=true;
#define ROW(name, body, ...) { name, [](State& s) { (void)s; body }, __VA_ARGS__ },
	static const Row kRows[] = {
#include "DejudderDecisionRows.inl"
	};
#undef ROW
#undef IP5060
#undef DJ24

	// Two frames per row, the second 600 ms later (the status shows the settled reason).
	static State RunRow(const Row& row)
	{
		State s;
		row.setup(s);
		s.Run();
		s.nowTick += 600;
		s.Run();
		return s;
	}

	static std::wstring Wide(const std::string& v) { return std::wstring(v.begin(), v.end()); }

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
		}

		TEST_METHOD(LegacyCopyReproducesEveryRecordedRow)
		{
			for (const Row& row : kRows)
			{
				const State s = RunRow(row);
				const Expected& e = row.expected;
				const std::wstring n = Wide(row.name);
				std::string logs;
				for (const std::string& l : s.logs)
					logs += l + "\n";
				Assert::IsTrue(s.dejudderReason == e.reason, (n + L": reason").c_str());
				Assert::AreEqual(e.extrasAllowed, s.dejudderExtrasAllowed, (n + L": extras allowed").c_str());
				Assert::IsTrue(s.dejudderEffectiveStrength == e.effectiveStrength, (n + L": effective strength").c_str());
				Assert::AreEqual(e.evenCycle, s.dejudderEvenCycle, (n + L": even cycle").c_str());
				Assert::AreEqual(e.interpolating, s.interpolating, (n + L": interpolating").c_str());
				Assert::IsTrue(s.interpolationDecision.reason == e.interpolationReason, (n + L": interpolation reason").c_str());
				Assert::IsTrue(s.interpolationDecision.ratio == e.interpolationRatio, (n + L": interpolation ratio").c_str());
				Assert::AreEqual(e.filmLike, s.interpolationDecision.filmLike, (n + L": film-like").c_str());
				Assert::AreEqual(e.perVsync, s.interpolationDecision.perVsync, (n + L": per-vsync").c_str());
				Assert::IsTrue(s.vpGapPhaseScale == e.gapPhaseScale, (n + L": gap phase scale").c_str());
				Assert::IsTrue(s.cadenceRatio == e.cadenceRatio, (n + L": cadence ratio").c_str());
				Assert::AreEqual(e.analysisEstimate, s.analysisEstimate, (n + L": analysis estimate").c_str());
				Assert::IsTrue(s.outcome == e.outcome, (n + L": outcome").c_str());
				Assert::AreEqual(e.warmPendingAfter, s.motionEngineWarmPending, (n + L": warm pending").c_str());
				Assert::AreEqual(e.budgetPolled, s.budgetPolled, (n + L": budget polled").c_str());
				Assert::AreEqual(e.warmed, s.warmed, (n + L": warmed").c_str());
				Assert::AreEqual(e.released, s.released, (n + L": released").c_str());
				Assert::AreEqual(e.statusPublishes, s.statusPublishes, (n + L": status publishes").c_str());
				Assert::AreEqual(e.statusCleared, s.statusCleared, (n + L": status cleared").c_str());
				Assert::AreEqual(std::string(e.statusHead), s.statusHead, (n + L": hold / status text").c_str());
				Assert::AreEqual(std::string(e.logs), logs, (n + L": log lines").c_str());
			}
		}
	};
}
