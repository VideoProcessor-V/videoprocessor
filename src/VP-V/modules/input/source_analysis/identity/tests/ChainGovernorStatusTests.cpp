/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The chain estimator's CPU-share governor arithmetic (SID-C2b, identity/chain/ChainGovernor.h: interval = max(nominal, cost / share))
// and its status texts (identity/chain/ChainStatus.h: the Ctrl+I / Config line and the Ctrl+Alt+D detail). Pure functions, hand-made
// inputs, no threads, no clock.
#include "pch.h"
#include "CppUnitTest.h"

#include "GeometryFixtures.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainGovernor.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainStatus.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace
	{
		namespace GG = GeoFx::G;
	}

	TEST_CLASS(ChainGovernorTests)
	{
	public:
		typedef SourceIdentity::Chain::GovernorSettings Settings;
		typedef SourceIdentity::Chain::Governor Gov;

		TEST_METHOD(IntervalIsTheLargerOfTheNominalCadenceAndCostOverShare)
		{
			const Settings s;
			// Acquiring: 1 s nominal, 15 % share.
			Assert::AreEqual(1.0, Gov::IntervalSec(s, false, 0.0), 1e-12);
			Assert::AreEqual(1.0, Gov::IntervalSec(s, false, 100.0), 1e-12, L"100 ms / 15 % = 0.67 s: the nominal cadence wins");
			Assert::AreEqual(2.0, Gov::IntervalSec(s, false, 300.0), 1e-12, L"300 ms / 15 % = 2 s");
			Assert::AreEqual(250.0 / 1000.0 / 0.15, Gov::IntervalSec(s, false, 250.0), 1e-12, L"the 250 ms budget: about 1.67 s");
			// Latched with power: 5 s nominal, 2 % share.
			Assert::AreEqual(5.0, Gov::IntervalSec(s, true, 60.0), 1e-12, L"60 ms / 2 % = 3 s < 5 s");
			Assert::AreEqual(10.0, Gov::IntervalSec(s, true, 200.0), 1e-12, L"200 ms / 2 % = 10 s");
			// A negative cost is no cost.
			Assert::AreEqual(1.0, Gov::IntervalSec(s, false, -5.0), 1e-12);
		}

		TEST_METHOD(TheIntervalIsCappedSoAPathologicalCostCannotStopTheEstimator)
		{
			Settings s;
			s.maxIntervalSec = 30.0;
			Assert::AreEqual(30.0, Gov::IntervalSec(s, false, 1e9), 1e-12);
			s.maxIntervalSec = 2.0;   // a cap below the latched nominal never undercuts the nominal cadence
			Assert::AreEqual(5.0, Gov::IntervalSec(s, true, 0.0), 1e-12);
		}

		TEST_METHOD(AWarmCostReplacesAColdOneAndIsSmoothed)
		{
			Gov g;
			Assert::AreEqual(0.0, g.CostMs(), 1e-12);
			g.NoteCost(900.0, true);
			Assert::AreEqual(900.0, g.CostMs(), 1e-12);
			Assert::IsTrue(g.CostIsCold());
			g.NoteCost(100.0, false);
			Assert::AreEqual(100.0, g.CostMs(), 1e-12, L"the first warm cost replaces the cold one");
			Assert::IsFalse(g.CostIsCold());
			g.NoteCost(300.0, false);
			Assert::AreEqual(200.0, g.CostMs(), 1e-12, L"half old, half new");
			g.NoteCost(5000.0, true);
			Assert::AreEqual(200.0, g.CostMs(), 1e-12, L"a later cold evaluation does not stretch the cadence for good");
			Assert::AreEqual<uint64_t>(4, g.Evaluations());
		}

		TEST_METHOD(ThePlanSaysWhetherTheBudgetStretchesTheCadence)
		{
			Gov g;
			g.NoteCost(100.0, false);
			SourceIdentity::Chain::GovernorPlan p = g.Plan(false);
			Assert::IsFalse(p.stretched);
			Assert::IsFalse(p.overBudget);
			Assert::AreEqual(1.0, p.intervalSec, 1e-12);
			g.NoteCost(600.0, false);   // smoothed: 350 ms
			p = g.Plan(false);
			Assert::AreEqual(350.0, p.costMs, 1e-12);
			Assert::IsTrue(p.stretched);
			Assert::IsTrue(p.overBudget, L"above 250 ms per evaluation");
			Assert::AreEqual(350.0 / 1000.0 / 0.15, p.intervalSec, 1e-12);
			p = g.Plan(true);
			Assert::AreEqual(350.0 / 1000.0 / 0.02, p.intervalSec, 1e-12);
			Assert::IsTrue(p.latched);
		}

		TEST_METHOD(DueFollowsTheLastStartAndTheInterval)
		{
			Gov g;
			Assert::IsTrue(g.Due(0.0, false), L"nothing started yet");
			g.NoteStart(1000.0);
			Assert::IsFalse(g.Due(1999.0, false));
			Assert::IsTrue(g.Due(2000.0, false));
			Assert::AreEqual(500.0, g.MsUntilDue(1500.0, false), 1e-9);
			Assert::AreEqual(0.0, g.MsUntilDue(2500.0, false), 1e-9);
			Assert::IsFalse(g.Due(5999.0, true), L"latched: 5 s");
			Assert::IsTrue(g.Due(6000.0, true));
			g.Reset();
			Assert::IsTrue(g.Due(0.0, false));
		}
	};

	TEST_CLASS(ChainStatusTests)
	{
	public:
		typedef SourceIdentity::Chain::ChainStatusInput Input;

		static Input Running()
		{
			Input in;
			in.running = true;
			in.tapAttached = true;
			in.haveRecord = true;
			in.last.c1 = 15.0;
			in.last.c2 = 18.0;
			return in;
		}

		static std::string Line(const Input& in) { return SourceIdentity::Chain::ChainStatusLine(in); }

		TEST_METHOD(ADecidedUpscaleNamesTheRungTheKernelFamilyAndTheSharpening)
		{
			Input in = Running();
			in.record.applied[0] = GeoFx::AppliedAxis(1920, 3840, 5);   // lanczos3
			in.record.applied[1] = GeoFx::AppliedAxis(1080, 2160, 5);
			in.record.applied[1].sharpen.present = true;
			in.record.applied[0].conf = in.record.applied[1].conf = 0.99f;
			Assert::AreEqual(std::string("Source identity: 1080 -> 2160 lanczos-like + sharpening, latched 0.99"), Line(in));
			in.record.applied[1].sharpen.present = false;
			in.record.applied[0].kernelIndex = in.record.applied[1].kernelIndex = -1;
			Assert::AreEqual(std::string("Source identity: 1080 -> 2160 kernel unsure, latched 0.99"), Line(in));
			in.shadow = true;
			Assert::AreEqual(std::string("Source identity: 1080 -> 2160 kernel unsure, latched 0.99 [shadow: would-be, classical decides]"), Line(in));
		}

		TEST_METHOD(NativeAndDisagreeingAxesAreSaidPlainly)
		{
			Input in = Running();
			in.record.applied[0] = GeoFx::AppliedAxis(3840, 3840, -1);
			in.record.applied[1] = GeoFx::AppliedAxis(2160, 2160, -1);
			in.record.applied[0].conf = in.record.applied[1].conf = 0.97f;
			Assert::AreEqual(std::string("Source identity: native, latched 0.97"), Line(in));
			in.record.applied[0] = GeoFx::AppliedAxis(1920, 3840, 1);   // bicubic
			in.record.applied[1] = GeoFx::AppliedAxis(1440, 2160, 8);   // spline36
			in.record.applied[0].conf = in.record.applied[1].conf = 0.98f;
			Assert::AreEqual(std::string("Source identity: H 1920 -> 3840 bicubic-like; V 1440 -> 2160 spline-like, latched 0.98"), Line(in));
		}

		TEST_METHOD(OneDecidedAxisAndTheOtherUnsure)
		{
			Input in = Running();
			in.record.applied[0] = GeoFx::AppliedAxis(1080, 2160, 5);
			in.record.applied[0].conf = 0.99f;
			in.record.measured[1] = GeoFx::MeasuredAxis(GG::LatchState::Abstain, 0, 2160);
			in.record.measured[1].reasonBits = SourceIdentity::Chain::kReasonDark;
			Assert::AreEqual(std::string("Source identity: H 1080 -> 2160 lanczos-like, latched 0.99; V unsure (dark)"), Line(in));
		}

		TEST_METHOD(NoPowerHoldAndAcquiringHaveTheirOwnWords)
		{
			Input in = Running();
			in.record.measured[0] = GeoFx::MeasuredAxis(GG::LatchState::Abstain, 0, 3840);
			in.record.measured[1] = GeoFx::MeasuredAxis(GG::LatchState::Abstain, 0, 2160);
			in.record.measured[0].reasonBits = SourceIdentity::Chain::kReasonDark | SourceIdentity::Chain::kReasonFlat;
			Assert::AreEqual(std::string("Source identity: unsure (dark, flat)"), Line(in));
			in.record.measured[1].state = GG::LatchState::Hold;
			Assert::AreEqual(std::string("Source identity: holding (overlay)"), Line(in));
			in.record.measured[0] = GeoFx::MeasuredAxis(GG::LatchState::Provisional, 960, 3840);
			in.record.measured[0].llr = 7.2f;
			in.record.measured[0].reasonBits = 0;
			in.record.measured[1] = GeoFx::MeasuredAxis(GG::LatchState::Provisional, 540, 2160);
			in.record.measured[1].llr = 3.0f;
			Assert::AreEqual(std::string("Source identity: acquiring, 7.2 of 15.0 nats"), Line(in));
		}

		TEST_METHOD(AnEstimatorThatDoesNotRunSaysWhyTheClassicalPathDecides)
		{
			Input in;
			in.off = "Original pathway";
			Assert::AreEqual(std::string("Source identity: classical only (Original pathway)"), Line(in));
			in.off.clear();
			Assert::AreEqual(std::string("Source identity: classical only (chain estimator off)"), Line(in));
			in = Running();
			in.haveRecord = false;
			Assert::AreEqual(std::string("Source identity: starting (no evaluation yet)"), Line(in));
			in.tapAttached = false;
			Assert::AreEqual(std::string("Source identity: classical only (no frame tap)"), Line(in), L"running without a tap: nothing can arrive");
		}

		TEST_METHOD(ALostTapAStalledWorkerAndAnUnusableAreaAreOnTheLineNotOnlyInTheDetail)
		{
			Input in = Running();
			in.governor.intervalSec = 1.0;
			in.wallMsSinceLastEvaluation = 3500.0;
			Assert::IsTrue(Line(in).find("stalled") == std::string::npos, L"2.5 x the planned interval + 1 s is the limit");
			in.wallMsSinceLastEvaluation = 3600.0;
			Assert::AreEqual(std::string("Source identity: stalled (no frames for 4 s)"), Line(in));
			in.unusableReason = "active area too small for the lines";
			Assert::AreEqual(std::string("Source identity: stalled (no frames for 4 s, active area too small for the lines)"), Line(in));
			in.shadow = true;
			Assert::AreEqual(std::string("Source identity: stalled (no frames for 4 s, active area too small for the lines) [shadow: would-be, classical decides]"), Line(in));
			in = Running();
			in.haveRecord = false;
			in.unusableReason = "active area too small for the lines";
			Assert::AreEqual(std::string("Source identity: starting (no evaluation yet, active area too small for the lines)"), Line(in));
			in = Running();
			in.tapLost = true;
			in.tapAttached = false;
			Assert::AreEqual(std::string("Source identity: classical only (frame tap lost: no new frames reach the estimator)"), Line(in),
				L"a registration that vanished is not shown as a healthy latch");
			Input slow = Running();
			slow.governor.intervalSec = 10.0;   // a stretched cadence: the stall limit follows the planned interval
			slow.wallMsSinceLastEvaluation = 20000.0;
			Assert::IsTrue(Line(slow).find("stalled") == std::string::npos);
			slow.wallMsSinceLastEvaluation = 27000.0;
			Assert::IsTrue(Line(slow).find("stalled") != std::string::npos);
		}

		TEST_METHOD(TheDetailTextHasTheFieldsGovernorCountsAndEvidenceStatus)
		{
			Input in = Running();
			in.record.evalSeq = 1234;
			in.record.maxAgeUs = 2000000;
			in.haveEval = true;
			in.last.stepCodes = 4;
			in.last.active.w = 3840;
			in.last.active.h = 2160;
			SourceIdentity::Chain::LogAxis& a = in.last.axis[0];
			a.state = GG::LatchState::Candidate;
			a.N = 3840;
			a.measuredN = 1920;
			a.measuredKernel = 5;
			a.measuredLlr = 12.5;
			a.probeRan = true;
			a.probeSeen = false;
			a.lines = 32;
			a.gatedLines = 20;
			a.maskedLines = 2;
			a.reasonBits = SourceIdentity::Chain::kReasonNoPower;
			a.candCount = 1;
			a.top[0].n = 1920;
			a.top[0].stat = -0.25;
			a.top[0].increment = 2.5;
			a.top[0].kernel = 5;
			a.top[0].informative = true;
			in.governor.intervalSec = 4.0;
			in.governor.nominalSec = 1.0;
			in.governor.share = 0.15;
			in.governor.costMs = 600.0;
			in.governor.stretched = true;
			in.governor.overBudget = true;
			in.evaluations = 7;
			in.delivered = 9;
			in.dropped = 2;
			in.repeatsSkipped = 3;
			in.evidenceText = "evidence: log+lines, 1.5 MB in sid-evidence (cap 2048 MB), 1 dumps";
			const std::string t = SourceIdentity::Chain::ChainDetailText(in);
			Assert::IsTrue(t.find("eval seq 1234") != std::string::npos);
			Assert::IsTrue(t.find("H: candidate, measured 1920/3840 lanczos3 llr 12.5 (c1 15.0, c2 18.0)") != std::string::npos);
			Assert::IsTrue(t.find("probe unseen, lines 20/32 informative, 2 masked, reasons: no power") != std::string::npos);
			Assert::IsTrue(t.find("candidate 1920: stat -0.250, increment 2.50, kernel lanczos3") != std::string::npos);
			Assert::IsTrue(t.find("governor: interval 4.00 s (nominal 1.0 s, share 15%, acquiring), cost 600.0 ms") != std::string::npos);
			Assert::IsTrue(t.find("cadence STRETCHED by the CPU budget") != std::string::npos);
			Assert::IsTrue(t.find("OVER the per-evaluation budget") != std::string::npos);
			Assert::IsTrue(t.find("evaluations 7, frames taken 9, dropped (newest wins) 2, repeats skipped 3") != std::string::npos);
			Assert::IsTrue(t.find("evidence: log+lines") != std::string::npos);
			Assert::IsTrue(t.find("output step 8-bit, active 3840x2160") != std::string::npos);
			Input off;
			off.off = "chain estimator off";
			Assert::IsTrue(SourceIdentity::Chain::ChainDetailText(off).find("governor") == std::string::npos, L"nothing to say when it does not run");
		}
	};
}
