// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stages TD-S6 & TD-S7 Adversarial CPU Tests
// (2026-11-23_TD-consolidated-plan.md s3.2 TD-S6 & TD-S7, s4.3 Q6, Q7, Q8, Q9):
// Independent Red-Team verification (100% deterministic CPU, ZERO silent skips, ZERO untaken branches,
// mandatory in-test negative controls on exact same inputs per Rule 3 & Rule 8):
//   1. SliceWorkRespectsDeclaredShareNeverShedAndNeverHijacksCoreFirstGap
//   2. QueuePriorityQ6AndLeadLadderE0ToE6DegradeVisibly
//   3. TierStepDownLadderAndOverloadOrderQ7bStepTdBeforeExtrasPreservingCells
//   4. LateCarryPolicyQ7aCarriesUpTo2FramesNeverL3ClampAndResetsOnCut
//   5. SettingsDefaultOffOriginalPathwayBitExactConfigRoundTripAndCtrlIStatus
//   6. (M-5 split onto gemini/td-dither-split: DownstreamQ9DitherNeedBitDepthFloorAndSigmaDeltaAboveBlackBtbClamp)
//   7. LateCarryPersistsPrevCleanValidAcrossHop2AndGuardsMissingRefGuideOn8x8Pocs
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <afx.h>
#include "CppUnitTest.h"

#ifndef MAIN_SCHEMA_EXTENSION_HEADER
#define MAIN_SCHEMA_EXTENSION_HEADER "VP-V/modules/control/config/VpvStartupSchema.h"
#endif
#ifndef PROFILE_SCHEMA_EXTENSION_HEADER
#define PROFILE_SCHEMA_EXTENSION_HEADER "VP-V/modules/control/config/VpvSchemaKeys.h"
#endif

#include <RendererProfileConfig.h>
#include <VP-V/core/AdmissionTable.h>
#include <VP-V/core/AheadCeiling.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackStatus.h>
#include <VP-V/core/LaneSpareWork.h>
#include <VP-V/core/OverloadOrder.h>
#include <VP-V/core/PostPresentSpare.h>
#include <VP-V/core/Products.h>
#include <VP-V/modules/control/config/VpvRendererFields.h>
#include <VP-V/modules/control/config/VpvRendererSettings.h>
#include <VP-V/modules/control/config/config_pages/VpvOrganisation.h>
#include <VP-V/modules/motion/estimation/LaneAheadPolicy.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductContracts.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductReads.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Scheduler.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Settings.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
namespace TdV2SchedulerConfigNs
{
	using namespace TdV2;

	inline std::wstring FormatMsg(const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0)
	{
		char buf[384];
		std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
		std::string s(buf);
		return std::wstring(s.begin(), s.end());
	}

	inline std::string RepoRootFromFile()
	{
		std::string path = __FILE__;
		std::replace(path.begin(), path.end(), '/', '\\');
		const size_t pos = path.rfind("\\src\\");
		Assert::IsTrue(pos != std::string::npos, L"Could not locate \\src\\ in __FILE__ path");
		path.resize(pos + 1);
		return path;
	}

	inline std::string ReadWholeTextFile(const std::string& fullPath)
	{
		std::ifstream f(fullPath, std::ios::in | std::ios::binary);
		Assert::IsTrue(f.is_open(), std::wstring(fullPath.begin(), fullPath.end()).c_str());
		std::ostringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}

	inline std::string ToLowerAscii(const std::string& s)
	{
		std::string out;
		out.reserve(s.size());
		for (char c : s)
			out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
		return out;
	}

	inline bool ContainsIcase(const std::string& haystack, const std::string& needle)
	{
		return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
	}

	inline std::string StripWhitespace(const std::string& s)
	{
		std::string out;
		out.reserve(s.size());
		for (char c : s)
		{
			if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
				out.push_back(c);
		}
		return out;
	}

	inline double MaxAbsPlaneRect(const TdV2Plane& p, const ActiveRect& r)
	{
		double m = 0.0;
		for (int y = r.y0; y < r.y1; ++y)
		{
			for (int x = r.x0; x < r.x1; ++x)
			{
				const double v = p.at(x, y);
				if (!std::isfinite(v))
					return std::numeric_limits<double>::infinity();
				const double a = std::fabs(v);
				if (a > m)
					m = a;
			}
		}
		return m;
	}

	inline double MaxAbsDiffPlanes(const TdV2Plane& a, const TdV2Plane& b)
	{
		Assert::AreEqual(a.w, b.w, L"Plane width mismatch");
		Assert::AreEqual(a.h, b.h, L"Plane height mismatch");
		double m = 0.0;
		for (size_t i = 0; i < a.v.size(); ++i)
		{
			if (!std::isfinite(a.v[i]) || !std::isfinite(b.v[i]))
				return std::numeric_limits<double>::infinity();
			const double d = std::fabs(a.v[i] - b.v[i]);
			if (d > m)
				m = d;
		}
		return m;
	}

	TEST_CLASS(TdV2SchedulerAndConfigTests)
	{
	public:
		// =========================================================================
		// 1. TD-S6 Slice Work: Declared sliceMs Cap, Row-Splittable Anchor Work,
		//    NEVER in OptionalFillIds / NEVER Shed, NEVER Hijacks CoreFirst Gap
		// =========================================================================
		TEST_METHOD(SliceWorkRespectsDeclaredShareNeverShedAndNeverHijacksCoreFirstGap)
		{
			// ---- Part A: Gap eligibility & StaticClass contract ----
			TdV2SliceWork tdWork(VpSkeleton::kTdAnchorPass, /*graphOrder=*/10);
			Assert::AreEqual(static_cast<int>(VpSkeleton::SpareClass::Slice),
				static_cast<int>(tdWork.Class()), L"TdV2SliceWork must be SpareClass::Slice");
			Assert::AreEqual(static_cast<int>(VpSkeleton::JobClass::Slice),
				static_cast<int>(tdWork.JobDeclaration().cls), L"TdV2SliceWork JobDecl must be JobClass::Slice");
			Assert::IsTrue(tdWork.JobDeclaration().splittable, L"TdV2SliceWork anchor pass must be splittable");
			Assert::IsFalse(tdWork.IsGapEligible(VpSkeleton::ScheduleMode::CoreFirst),
				L"TD Slice work must NEVER hijack CoreFirst gap");
			Assert::IsFalse(VpSkeleton::GapEligible(tdWork.JobDeclaration(), VpSkeleton::ScheduleMode::CoreFirst),
				L"Core GapEligible(tdDecl, CoreFirst) must be false");
			Assert::IsTrue(tdWork.IsGapEligible(VpSkeleton::ScheduleMode::SharedGap),
				L"SharedGap mode (R10) permits all registrations");

			// NEGATIVE CONTROL (CoreFirst Gap): a Fill-classed job IS gap-eligible in CoreFirst!
			VpSkeleton::JobDecl fillDecl{};
			fillDecl.product = VpSkeleton::kP5MonitorTiles;
			fillDecl.cls = VpSkeleton::JobClass::Fill;
			fillDecl.graphOrder = 20;
			Assert::IsTrue(VpSkeleton::GapEligible(fillDecl, VpSkeleton::ScheduleMode::CoreFirst),
				L"Negative control: Fill-classed job MUST be GapEligible in CoreFirst");

			// ---- Part B: AdmissionTable — TD products NEVER in OptionalFillIds, Fill jobs ARE ----
			VpSkeleton::Admission::AdmissionFacts facts;
			facts.periodMs = 1000.0 / 24.0;
			facts.renderMs = 20.0; // Budget = 41.67 - 20.0 - 4.17 = 17.50 ms
			facts.mode = VpSkeleton::Admission::LatencyMode::Standard;
			facts.tdMinMs = 2.0;
			facts.tdWantMs = 4.0;

			// Present core + TD slice + competing optional Fill products.
			const VpSkeleton::ProductId coreIds[] = {
				VpSkeleton::kP1HalfLuma, VpSkeleton::kP2VectorPair, VpSkeleton::kP3AnalysisField,
				VpSkeleton::kQualityPattern, VpSkeleton::kCodingEvidence
			};
			for (VpSkeleton::ProductId id : coreIds)
			{
				facts.present[id] = true;
				facts.measured[id] = true;
				facts.costMs[id] = 0.8;
			}
			const VpSkeleton::ProductId tdIds[] = {
				VpSkeleton::kTdBlocks, VpSkeleton::kTdMemory, VpSkeleton::kTdAnchorPass,
				VpSkeleton::kTdAnchorRefs, VpSkeleton::kTdDelta, VpSkeleton::kTdMaps, VpSkeleton::kTdTrailSummary
			};
			for (VpSkeleton::ProductId id : tdIds)
			{
				facts.present[id] = true;
				facts.measured[id] = true;
				facts.costMs[id] = 0.5;
			}
			const VpSkeleton::ProductId fillIds[] = {
				VpSkeleton::kP5MonitorTiles, VpSkeleton::kP10GrainSplit
			};
			for (VpSkeleton::ProductId id : fillIds)
			{
				facts.present[id] = true;
				facts.measured[id] = true;
				facts.costMs[id] = 1.2;
			}

			const VpSkeleton::Admission::Result adm = VpSkeleton::Admission::Admit(facts);
			Assert::IsTrue(adm.sliceMs > 0.0 && adm.sliceMs <= facts.tdWantMs,
				FormatMsg("Expected valid TD sliceMs in (0, %.2f], got %.2f", facts.tdWantMs, adm.sliceMs).c_str());
			const std::vector<VpSkeleton::ProductId> optFills = VpSkeleton::Admission::OptionalFillIds(adm);

			for (VpSkeleton::ProductId id : tdIds)
			{
				Assert::IsTrue(adm.admitted[id], L"Every demanded TD product must be admitted with TD ON");
				Assert::AreEqual(static_cast<int>(VpSkeleton::JobClass::Slice), static_cast<int>(adm.cls[id]),
					L"TD product must keep JobClass::Slice");
				const bool inOpt = std::find(optFills.begin(), optFills.end(), id) != optFills.end();
				Assert::IsFalse(inOpt, L"TD Slice product must NEVER appear in OptionalFillIds (never shed by FillOff)");
			}

			// NEGATIVE CONTROL (AdmissionTable OptionalFillIds): Fill jobs on the EXACT SAME table ARE in OptionalFillIds!
			for (VpSkeleton::ProductId id : fillIds)
			{
				Assert::IsTrue(adm.admitted[id], L"Negative control: Fill job must be admitted in remaining budget");
				const bool inOpt = std::find(optFills.begin(), optFills.end(), id) != optFills.end();
				Assert::IsTrue(inOpt, L"Negative control: Fill job MUST appear in OptionalFillIds (sheddable by FillOff)");
			}

			// ---- Part C: Real SpareHost + Row-Splittable Anchor Work + Competing Fill Job ----
			VpSkeleton::SpareHost host;
			host.Slot().Register(&tdWork);

			bool fillEnabledByArbiter = true;
			bool fillHasPending = true;
			uint32_t fillRuns = 0;
			VpSkeleton::FunctionSpareWork fillWork(VpSkeleton::kP5MonitorTiles, VpSkeleton::SpareClass::Fill, /*order=*/0);
			fillWork.hasWork = [&]() { return fillEnabledByArbiter && fillHasPending; };
			fillWork.run = [&](uint32_t) -> double
			{
				++fillRuns;
				fillHasPending = false;
				return 1.2;
			};
			host.Slot().Register(&fillWork);

			// Warm up cost model on frame 1 (unmeasured Slice runs whole once as first work, establishing 0.05 ms/row -> 5.0 ms).
			const double msPerRow = 0.05;
			host.SetBudget(/*budgetMs=*/10.0, /*sliceMs=*/5.0);
			host.NotePresented(/*sourceSequence=*/1, /*cadenceRepeat=*/false);
			tdWork.BeginFrameWork(100u, [&](uint32_t, uint32_t count) { return static_cast<double>(count) * msPerRow; });
			const VpSkeleton::SpareReport warmRep = host.Fire(VpSkeleton::SpareTrigger::PostPresentNoExtras);
			Assert::AreEqual(static_cast<int>(VpSkeleton::SpareOutcome::Ran), static_cast<int>(warmRep.outcome),
				L"Warmup slot must run");
			Assert::AreEqual(100u, static_cast<uint32_t>(tdWork.TotalRowsCompleted()), L"Warmup must complete 100 rows");
			Assert::IsTrue(tdWork.Cost().PredictMs() >= 4.9 && tdWork.Cost().PredictMs() <= 5.1,
				L"Warmup cost model must predict ~5.0 ms");
			// Warm up Fill cost model too (ran after TD on frame 1? Unmeasured Fill skips when r.ran > 0, so run Fill once alone).
			fillHasPending = true;
			host.NotePresented(/*sourceSequence=*/2, /*cadenceRepeat=*/false);
			const VpSkeleton::SpareReport fillWarm = host.Fire(VpSkeleton::SpareTrigger::PostPresentNoExtras);
			Assert::AreEqual(static_cast<int>(VpSkeleton::SpareOutcome::Ran), static_cast<int>(fillWarm.outcome),
				L"Fill warmup slot must run");
			Assert::AreEqual(1u, fillRuns, L"Fill warmup must measure cost (1.2 ms)");

			// Now test declared sliceMs = 2.0 ms across multi-frame row-splittable anchor work (100 rows = 5.0 ms total).
			// Competing Fill job wants 1.2 ms, total budget = 2.2 ms -> TD takes <= 2.0 ms, remaining ~0.5 ms < 1.2 ms -> Fill skipped!
			const double declaredSliceMs = 2.0;
			const double tightBudgetMs = 2.2;
			host.SetBudget(tightBudgetMs, declaredSliceMs);

			std::vector<uint8_t> rowCovered(100u, 0u);
			uint32_t expectedNextRow = 0;
			double maxSingleSliceMs = 0.0;
			tdWork.BeginFrameWork(100u, [&](uint32_t startRow, uint32_t count) -> double
			{
				Assert::AreEqual(expectedNextRow, startRow, L"Row-split anchor work must advance contiguously");
				for (uint32_t r = startRow; r < startRow + count; ++r)
				{
					Assert::IsTrue(r < 100u, L"Row index out of bounds");
					Assert::AreEqual(0, static_cast<int>(rowCovered[r]), L"Row executed twice");
					rowCovered[r] = 1u;
				}
				expectedNextRow += count;
				const double ms = static_cast<double>(count) * msPerRow;
				if (ms > maxSingleSliceMs)
					maxSingleSliceMs = ms;
				return ms;
			});

			uint32_t totalFillSkipped = 0;
			const uint32_t fillRunsBeforeTight = fillRuns;
			uint64_t seq = 10;
			int framesUsed = 0;
			while (tdWork.HasWork() && framesUsed < 10)
			{
				fillHasPending = true;
				host.NotePresented(seq++, false);
				const VpSkeleton::SpareReport rep = host.Fire(VpSkeleton::SpareTrigger::PostPresentNoExtras);
				Assert::AreEqual(static_cast<int>(VpSkeleton::SpareOutcome::Ran), static_cast<int>(rep.outcome),
					L"Slice work slot must run");
				Assert::IsTrue(rep.usedMs <= declaredSliceMs + 1e-9,
					FormatMsg("TD slice exceeded declared share: used=%.4f > sliceMs=%.4f", rep.usedMs, declaredSliceMs).c_str());
				Assert::AreEqual(0u, rep.sliceSkipped, L"TD Slice work must NOT be skipped while sliceMs > 0");
				totalFillSkipped += rep.fillSkipped;
				++framesUsed;
			}

			Assert::IsFalse(tdWork.HasWork(), L"Row-splittable TD anchor pass must finish within 10 frames");
			Assert::AreEqual(100u, expectedNextRow, L"All 100 anchor rows must complete");
			Assert::IsTrue(tdWork.SplitRuns() >= 2u, L"Anchor pass must genuinely split across multiple frames");
			Assert::IsTrue(maxSingleSliceMs <= declaredSliceMs + 1e-9,
				FormatMsg("Max single TD slice=%.4f > declared %.4f", maxSingleSliceMs, declaredSliceMs).c_str());
			for (uint32_t r = 0; r < 100u; ++r)
				Assert::AreEqual(1, static_cast<int>(rowCovered[r]), L"Every anchor row must be covered exactly once");

			// NEGATIVE CONTROL (Same SpareHost under tight budget + FillOff):
			// Competing Fill job WAS shed/skipped (fillSkipped > 0, 0 extra runs) while TD Slice ran cleanly inside sliceMs!
			Assert::AreEqual(fillRunsBeforeTight, fillRuns,
				L"Negative control: Fill job must be skipped under tight budget while TD Slice runs");
			Assert::IsTrue(totalFillSkipped >= 3u,
				L"Negative control: competing Fill job must accumulate fillSkipped >= 3 on the exact same SpareHost");

			// Also verify FillOff via OverloadArbiter sheds Fill while TD Slice remains active and finishes within sliceMs!
			TdV2TierController ctrl(TdTier::Full);
			const char* fillNames[] = { "monitor tiles", "grain split" };
			ctrl.SetOptionalFills(fillNames, 2);
			VpSkeleton::SlotBudget overBudget;
			overBudget.measured = true;
			overBudget.periodMs = 40.0; // usable = 36 ms
			overBudget.renderMs = 30.0;
			overBudget.usedMs = 10.0;   // u = 40/36 > 1.0
			ctrl.Step(0.0, overBudget, /*extrasActive=*/true);
			ctrl.Step(2000.0, overBudget, /*extrasActive=*/true);
			Assert::IsFalse(ctrl.FillAllowed(), L"Negative control: OverloadArbiter FillOff must shed Fill jobs");
			Assert::AreEqual(static_cast<int>(TdTier::Full), static_cast<int>(ctrl.CurrentTier()),
				L"TD Slice must remain at Full tier during FillOff");
			fillEnabledByArbiter = ctrl.FillAllowed();
			host.SetBudget(/*budgetMs=*/10.0, /*sliceMs=*/declaredSliceMs); // Plenty of budget, yet FillOff shuts off Fill!
			tdWork.BeginFrameWork(20u, [&](uint32_t, uint32_t count) { return static_cast<double>(count) * msPerRow; });
			int fillOffFrames = 0;
			while (tdWork.HasWork() && fillOffFrames < 10)
			{
				fillHasPending = true;
				host.NotePresented(seq++, false);
				const VpSkeleton::SpareReport fillOffRep = host.Fire(VpSkeleton::SpareTrigger::PostPresentNoExtras);
				Assert::AreEqual(static_cast<int>(VpSkeleton::SpareOutcome::Ran), static_cast<int>(fillOffRep.outcome));
				Assert::IsTrue(fillOffRep.usedMs <= declaredSliceMs + 1e-9,
					FormatMsg("TD slice during FillOff exceeded sliceMs: %.4f > %.4f", fillOffRep.usedMs, declaredSliceMs).c_str());
				++fillOffFrames;
			}
			Assert::IsFalse(tdWork.HasWork(), L"TD Slice must keep running and finish during FillOff");
			Assert::AreEqual(fillRunsBeforeTight, fillRuns, L"Negative control: Fill job must remain shed during FillOff");

			// Runner returning NaN / negative ms fails visibly without advancing rowsDone.
			tdWork.BeginFrameWork(10u, [](uint32_t, uint32_t) { return std::numeric_limits<double>::quiet_NaN(); });
			const uint64_t failsBefore = tdWork.Failures();
			Assert::IsTrue(tdWork.Run(4u) < 0.0, L"NaN runner duration must report < 0 failure");
			Assert::AreEqual(failsBefore + 1u, tdWork.Failures(), L"Failed runner must increment Failures()");
			Assert::AreEqual(10u, tdWork.RowsLeft(), L"Failed runner must not falsely mark rows completed");
		}

		// =========================================================================
		// 2. Owner Q6 Queue Priority (250 ms default, TD Floor >= Render-Ahead Credit)
		//    & Lead E Ladder (E = 0..6, 99) Visible Degradation
		// =========================================================================
		TEST_METHOD(QueuePriorityQ6AndLeadLadderE0ToE6DegradeVisibly)
		{
			// Default queue budget stays at 250 ms (Owner Q6a).
			QueuePriorityInput defIn;
			Assert::AreEqual(250.0, defIn.queueBudgetMs, 1e-12, L"Default queueBudgetMs must be 250 ms (Q6a)");
			Assert::AreEqual(250.0, kDefaultQueueBudgetMs, 1e-12, L"kDefaultQueueBudgetMs must be 250 ms");

			// Tight 4K24 queue (queuedAheadFrames = 2, matching 250 ms minus motion latency):
			// TD enabled -> TD floor (LaneAhead::kDepthMin = 2) strictly outranks render-ahead credit!
			QueuePriorityInput tightIn;
			tightIn.tdEnabled = true;
			tightIn.queueBudgetMs = kDefaultQueueBudgetMs;
			tightIn.sourceHz = kDefaultSourceHz;
			tightIn.queuedAheadFrames = 2;
			tightIn.renderAheadCreditRequested = 2;
			tightIn.latencyMode = VpSkeleton::Admission::LatencyMode::Standard;

			StandInRegistry qSi;
			const QueuePriorityDecision tdTight = EvaluateQueuePriority(tightIn, &qSi);
			Assert::AreEqual(250.0, tdTight.effectiveQueueBudgetMs, 1e-12, L"Effective queue budget must remain 250 ms");
			Assert::AreEqual(VpMotion::LaneAhead::kDepthMin, tdTight.tdLookAheadFloor,
				L"TD look-ahead floor must equal VpMotion::LaneAhead::kDepthMin (2)");
			Assert::IsTrue(tdTight.tdWinsOverCredit, L"TD must win over render-ahead credit when TD is enabled (Q6b)");
			Assert::AreEqual(0, tdTight.renderAheadCreditGranted,
				L"Render-ahead credit must receive 0 frames on tight queue when TD is enabled");
			Assert::IsTrue(tdTight.creditTrimmed, L"creditTrimmed must visibly report denied credit");
			Assert::AreEqual(2, tdTight.usableLeadE, L"TD must keep full E = 2 look-ahead on tight queue");
			Assert::IsFalse(qSi.IsActive(StandInKind::NonFiniteClosure), L"Valid queue input must not trip NonFiniteClosure");

			// NEGATIVE CONTROL (Q6b): On the EXACT SAME tight queue with tdEnabled == false,
			// render-ahead credit IS granted (proving credit genuinely competes for those 2 frames!).
			QueuePriorityInput tightTdOff = tightIn;
			tightTdOff.tdEnabled = false;
			const QueuePriorityDecision noTdTight = EvaluateQueuePriority(tightTdOff, &qSi);
			Assert::IsFalse(noTdTight.tdWinsOverCredit, L"Negative control: tdWinsOverCredit must be false when TD off");
			Assert::AreEqual(2, noTdTight.renderAheadCreditGranted,
				L"Negative control: render-ahead credit MUST be granted (2 frames) on exact same tight queue when TD off");
			Assert::IsFalse(noTdTight.creditTrimmed, L"Negative control: credit must not be trimmed when TD off");
			Assert::AreEqual(0, noTdTight.usableLeadE, L"TD usableLeadE must be 0 when TD off");

			// Partial spare above TD floor (queued = 4, credit requested = 3 -> credit gets 4 - 2 = 2, TD keeps 2).
			QueuePriorityInput partialIn = tightIn;
			partialIn.queuedAheadFrames = 4;
			partialIn.renderAheadCreditRequested = 3;
			const QueuePriorityDecision partialDec = EvaluateQueuePriority(partialIn);
			Assert::AreEqual(2, partialDec.renderAheadCreditGranted, L"Credit may only take frames strictly above TD floor");
			Assert::IsTrue(partialDec.creditTrimmed, L"Partial credit trim must be flagged");
			Assert::AreEqual(2, partialDec.usableLeadE, L"TD floor of 2 must remain protected");

			// Time-derived 250 ms queue at 23.976 Hz (queuedAheadFrames = -1, motionLatencyMs = 150 ms -> 2 frames).
			QueuePriorityInput derivedIn;
			derivedIn.tdEnabled = true;
			derivedIn.queueBudgetMs = 250.0;
			derivedIn.sourceHz = 24000.0 / 1001.0;
			derivedIn.motionLatencyMs = 150.0;
			derivedIn.queuedAheadFrames = -1;
			derivedIn.renderAheadCreditRequested = 1;
			const QueuePriorityDecision derivedDec = EvaluateQueuePriority(derivedIn);
			Assert::AreEqual(2, derivedDec.totalQueuedAhead, L"250 ms - 150 ms at 23.976 Hz must yield 2 queued frames");
			Assert::AreEqual(2, derivedDec.usableLeadE, L"Derived 4K24 queue must yield central E = 2");
			Assert::AreEqual(0, derivedDec.renderAheadCreditGranted, L"Derived tight queue must protect TD floor over credit");

			// Ahead ceiling clamp: huge queue clamps usableLeadE strictly to VpSkeleton::kAheadCeiling (6).
			QueuePriorityInput hugeIn = tightIn;
			hugeIn.queuedAheadFrames = 20;
			hugeIn.renderAheadCreditRequested = 2;
			const QueuePriorityDecision hugeDec = EvaluateQueuePriority(hugeIn);
			Assert::AreEqual(static_cast<int>(VpSkeleton::kAheadCeiling), hugeDec.usableLeadE,
				L"usableLeadE must clamp to VpSkeleton::kAheadCeiling (6)");
			Assert::AreEqual(2, hugeDec.renderAheadCreditGranted, L"Generous queue grants full credit without hurting ceiling");

			// LatencyMode::AvrSafe enforces the 200 ms ceiling (never a floor) on queueBudgetMs!
			{
				QueuePriorityInput avrIn;
				avrIn.tdEnabled = true;
				avrIn.queueBudgetMs = 250.0;
				avrIn.sourceHz = 24000.0 / 1001.0;
				avrIn.motionLatencyMs = 0.0;
				avrIn.queuedAheadFrames = -1;
				avrIn.latencyMode = VpSkeleton::Admission::LatencyMode::AvrSafe;
				const QueuePriorityDecision avrDec = EvaluateQueuePriority(avrIn);
				Assert::AreEqual(200.0, avrDec.effectiveQueueBudgetMs, 1e-12,
					L"AvrSafe mode must cap queueBudgetMs=250.0 to 200.0 ms ceiling");
				Assert::AreEqual(4, avrDec.totalQueuedAhead,
					L"AvrSafe 200 ms at 23.976 Hz must yield 4 queued frames");

				// Negative control: exact same 250 ms input in Standard mode is NOT capped at 200 ms (5 queued frames).
				QueuePriorityInput stdIn = avrIn;
				stdIn.latencyMode = VpSkeleton::Admission::LatencyMode::Standard;
				const QueuePriorityDecision stdDec = EvaluateQueuePriority(stdIn);
				Assert::AreEqual(250.0, stdDec.effectiveQueueBudgetMs, 1e-12,
					L"Negative control: Standard mode keeps full 250 ms queue budget");
				Assert::AreEqual(5, stdDec.totalQueuedAhead,
					L"Negative control: Standard 250 ms at 23.976 Hz yields 5 queued frames");

				// Ceiling, never a floor: 150 ms under AvrSafe stays 150 ms.
				QueuePriorityInput avrLower = avrIn;
				avrLower.queueBudgetMs = 150.0;
				const QueuePriorityDecision avrLowerDec = EvaluateQueuePriority(avrLower);
				Assert::AreEqual(150.0, avrLowerDec.effectiveQueueBudgetMs, 1e-12,
					L"AvrSafe 200 ms is a ceiling, never a floor");
			}

			// Huge finite queueBudgetMs (1e18 ms) must NOT trigger int overflow / UB when deriving queuedAheadFrames!
			{
				QueuePriorityInput giantIn;
				giantIn.tdEnabled = true;
				giantIn.queueBudgetMs = 1.0e18;
				giantIn.sourceHz = 60.0;
				giantIn.motionLatencyMs = 0.0;
				giantIn.queuedAheadFrames = -1;
				giantIn.renderAheadCreditRequested = 2;
				giantIn.latencyMode = VpSkeleton::Admission::LatencyMode::Standard;
				const QueuePriorityDecision giantDec = EvaluateQueuePriority(giantIn);
				Assert::AreEqual(static_cast<int>(VpSkeleton::kAheadCeiling), giantDec.usableLeadE,
					L"Huge finite queueBudgetMs (1e18) must clamp cleanly to kAheadCeiling without overflow");
				Assert::IsTrue(giantDec.totalQueuedAhead >= static_cast<int>(VpSkeleton::kAheadCeiling),
					L"Huge finite queueBudgetMs must saturate positively (never wrap negative)");
				Assert::AreEqual(2, giantDec.renderAheadCreditGranted,
					L"Huge finite queueBudgetMs must still grant requested render-ahead credit");
			}

			// Games & Competitive modes (IC-2 R9): E == 0, credit == 0, AND creditTrimmed == true when credit requested > 0!
			for (VpSkeleton::Admission::LatencyMode gm : {
				VpSkeleton::Admission::LatencyMode::Games,
				VpSkeleton::Admission::LatencyMode::Competitive })
			{
				QueuePriorityInput gIn = tightIn;
				gIn.queuedAheadFrames = 6;
				gIn.renderAheadCreditRequested = 2;
				gIn.latencyMode = gm;
				const QueuePriorityDecision gDec = EvaluateQueuePriority(gIn);
				Assert::AreEqual(0, gDec.usableLeadE, L"Games/Competitive must return E = 0");
				Assert::AreEqual(0, gDec.renderAheadCreditGranted, L"Games/Competitive must grant 0 render-ahead credit");
				Assert::IsTrue(gDec.creditTrimmed,
					L"Games/Competitive must visibly flag creditTrimmed == true when renderAheadCreditRequested > 0");

				gIn.renderAheadCreditRequested = 0;
				const QueuePriorityDecision gNoReq = EvaluateQueuePriority(gIn);
				Assert::IsFalse(gNoReq.creditTrimmed,
					L"Games/Competitive must report creditTrimmed == false when renderAheadCreditRequested == 0");
			}

			// Non-finite timing fails closed visibly.
			{
				QueuePriorityInput badIn = tightIn;
				badIn.queueBudgetMs = std::numeric_limits<double>::quiet_NaN();
				StandInRegistry badSi;
				const QueuePriorityDecision badDec = EvaluateQueuePriority(badIn, &badSi);
				Assert::AreEqual(0, badDec.usableLeadE, L"NaN queueBudgetMs must close to E = 0");
				Assert::IsTrue(badSi.IsActive(StandInKind::NonFiniteClosure),
					L"NaN queueBudgetMs must engage NonFiniteClosure");
			}

			// ---- Lead E Ladder across E = -2, 0, 1, 2, 3, 4, 5, 6, 99 ----
			struct ExpectedLead
			{
				int rawE;
				int clampedE;
				bool nextPeak;
				bool p15Lead2;
				bool anchorPreShow;
				bool anchorNarrow;
				bool reduced;
			};
			const ExpectedLead cases[] = {
				{ -2, 0, false, false, false, false, true  },
				{  0, 0, false, false, false, false, true  },
				{  1, 1, true,  false, false, false, true  },
				{  2, 2, true,  true,  false, false, true  },
				{  3, 3, true,  true,  true,  true,  true  },
				{  4, 4, true,  true,  true,  false, false },
				{  5, 5, true,  true,  true,  false, false },
				{  6, 6, true,  true,  true,  false, false },
				{ 99, static_cast<int>(VpSkeleton::kAheadCeiling), true, true, true, false, false }
			};

			StandInRegistry ladderSi;
			for (const ExpectedLead& c : cases)
			{
				const LeadLadderState st = EvaluateLeadLadder(c.rawE, &ladderSi);
				Assert::AreEqual(c.clampedE, st.usableLeadE, L"Lead ladder clamped E mismatch");
				Assert::AreEqual(c.nextPeak, st.nextPeak, L"Lead ladder nextPeak mismatch");
				Assert::AreEqual(c.p15Lead2, st.p15Lead2, L"Lead ladder p15Lead2 mismatch");
				Assert::AreEqual(c.anchorPreShow, st.anchorPreShow, L"Lead ladder anchorPreShow mismatch");
				Assert::AreEqual(c.anchorNarrow, st.anchorPreShowNarrow, L"Lead ladder anchorPreShowNarrow mismatch");
				Assert::AreEqual(c.reduced, st.reduced, L"Lead ladder reduced mismatch");
				Assert::AreEqual(c.reduced, ladderSi.IsActive(StandInKind::LeadReduced),
					L"LeadReduced stand-in must be engaged iff E < 4 and cleared when E >= 4");
				Assert::IsTrue(st.statusNote.find("E=" + std::to_string(c.clampedE) + "/") != std::string::npos,
					L"Lead ladder statusNote must display exact E=n/4");
			}
			// Verify explicit transition back from E = 6 (cleared) -> E = 2 (re-engaged) -> E = 4 (re-cleared).
			Assert::IsFalse(ladderSi.IsActive(StandInKind::LeadReduced));
			EvaluateLeadLadder(2, &ladderSi);
			Assert::IsTrue(ladderSi.IsActive(StandInKind::LeadReduced), L"LeadReduced must re-engage at E=2");
			EvaluateLeadLadder(4, &ladderSi);
			Assert::IsFalse(ladderSi.IsActive(StandInKind::LeadReduced), L"LeadReduced must clear at E=4");
		}

		// =========================================================================
		// 3. Tier Step-Down Ladder & Owner Q7b Overload Order
		//    (None -> FillOff -> TdStepDown 3->2->1->0 -> ExtrasStepDown ->
		//     CoreDeferred -> FeatureOff; Hysteresis Recovery; Cell Integrity; Q8b 4K)
		// =========================================================================
		TEST_METHOD(TierStepDownLadderAndOverloadOrderQ7bStepTdBeforeExtrasPreservingCells)
		{
			TdV2TierController ctrl(TdTier::Full);
			const char* fills[] = { "grain split", "monitor tiles" };
			ctrl.SetOptionalFills(fills, 2);

			// Budget: periodMs = 40.0 -> usable = 40 - 4 = 36.0 ms.
			VpSkeleton::SlotBudget overBudget;
			overBudget.measured = true;
			overBudget.periodMs = 40.0;
			overBudget.renderMs = 30.0;
			overBudget.usedMs = 10.0;   // u = 40 / 36 = 1.111 > 1.0

			VpSkeleton::SlotBudget holdBudget = overBudget;
			holdBudget.usedMs = 4.0;    // u = 34 / 36 = 0.944 in [0.90, 1.0] (hold band)

			VpSkeleton::SlotBudget underBudget = overBudget;
			underBudget.renderMs = 20.0;
			underBudget.usedMs = 5.0;   // u = 25 / 36 = 0.694 < 0.90

			StandInRegistry si;

			// Initial state at t = 0 ms: None, tier 3, fill & extras allowed.
			Assert::AreEqual(static_cast<int>(TdTier::Full),
				static_cast<int>(ctrl.Step(0.0, overBudget, /*extrasActive=*/true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::None),
				static_cast<int>(ctrl.OverloadState().step));
			Assert::IsTrue(ctrl.FillAllowed() && ctrl.ExtrasAllowed());
			Assert::IsFalse(si.IsActive(StandInKind::TierSteppedDown));
			Assert::IsFalse(ctrl.FallbackDecision().engaged);

			// Before 2 s (t = 1999 ms): hysteresis prevents premature step-down.
			ctrl.Step(1999.0, overBudget, true, &si);
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::None),
				static_cast<int>(ctrl.OverloadState().step), L"Must hold for full 2 s before stepping down");

			// Step 1 (t = 2000 ms): FillOff (tier still 3, Extras still allowed!).
			Assert::AreEqual(static_cast<int>(TdTier::Full), static_cast<int>(ctrl.Step(2000.0, overBudget, true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::FillOff),
				static_cast<int>(ctrl.OverloadState().step), L"Step 1 must be FillOff");
			Assert::IsFalse(ctrl.FillAllowed(), L"FillAllowed must be false in FillOff");
			Assert::IsTrue(ctrl.ExtrasAllowed(), L"ExtrasAllowed must stay true in FillOff");
			Assert::IsTrue(ctrl.FallbackDecision().engaged, L"FallbackDecision must engage in FillOff");

			// Step 2 (t = 4000 ms): TdStepDown 3 -> 2 (ReducedAnchor), Extras STILL allowed!
			Assert::AreEqual(static_cast<int>(TdTier::ReducedAnchor),
				static_cast<int>(ctrl.Step(4000.0, overBudget, true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::TdStepDown),
				static_cast<int>(ctrl.OverloadState().step));
			Assert::IsTrue(ctrl.ExtrasAllowed(), L"Q7b: ExtrasAllowed MUST stay true at TD tier 2");
			Assert::IsTrue(si.IsActive(StandInKind::TierSteppedDown), L"TierSteppedDown must engage at tier 2");
			Assert::IsFalse(si.IsActive(StandInKind::LowConfidenceL3), L"Must NEVER engage LowConfidenceL3 for budget");

			// Step 3 (t = 6000 ms): TdStepDown 2 -> 1 (LocalPeaksOnly), Extras STILL allowed!
			Assert::AreEqual(static_cast<int>(TdTier::LocalPeaksOnly),
				static_cast<int>(ctrl.Step(6000.0, overBudget, true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::TdStepDown),
				static_cast<int>(ctrl.OverloadState().step));
			Assert::IsTrue(ctrl.ExtrasAllowed(), L"Q7b: ExtrasAllowed MUST stay true at TD tier 1");

			// Step 4 (t = 8000 ms): TdStepDown 1 -> 0 (OffBudget), Extras STILL allowed on the step TD hits 0!
			Assert::AreEqual(static_cast<int>(TdTier::OffBudget),
				static_cast<int>(ctrl.Step(8000.0, overBudget, true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::TdStepDown),
				static_cast<int>(ctrl.OverloadState().step));
			Assert::IsTrue(ctrl.ExtrasAllowed(), L"Q7b: ExtrasAllowed MUST stay true until the step AFTER TD reaches 0");

			// Step 5 (t = 10000 ms): ExtrasStepDown (Extras drops ONLY NOW, after TD reached 0!).
			Assert::AreEqual(static_cast<int>(TdTier::OffBudget),
				static_cast<int>(ctrl.Step(10000.0, overBudget, true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::ExtrasStepDown),
				static_cast<int>(ctrl.OverloadState().step), L"Q7b: Step 5 must be ExtrasStepDown");
			Assert::IsFalse(ctrl.ExtrasAllowed(), L"ExtrasAllowed must drop to false in ExtrasStepDown");

			// Step 6 (t = 12000 ms): CoreDeferred.
			Assert::AreEqual(static_cast<int>(TdTier::OffBudget),
				static_cast<int>(ctrl.Step(12000.0, overBudget, true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::CoreDeferred),
				static_cast<int>(ctrl.OverloadState().step), L"Q7b: Step 6 must be CoreDeferred");
			ctrl.NoteCoreDeferred(3u);
			Assert::AreEqual(static_cast<unsigned long long>(3u), ctrl.DeferredTotal());
			Assert::AreEqual(3u, ctrl.OverloadState().deferredCore);

			// Step 7 (t = 14000 ms): FeatureOff (deepest).
			Assert::AreEqual(static_cast<int>(TdTier::OffBudget),
				static_cast<int>(ctrl.Step(14000.0, overBudget, true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::FeatureOff),
				static_cast<int>(ctrl.OverloadState().step), L"Q7b: Step 7 must be FeatureOff");
			Assert::IsFalse(ctrl.ExtrasAllowed());
			Assert::AreEqual(static_cast<unsigned long long>(7u), ctrl.StepsDown());

			// Hold band (u = 0.944 in [0.90, 1.0]) must NOT step up or down even after 10 s!
			ctrl.Step(14100.0, holdBudget, true, &si);
			ctrl.Step(24100.0, holdBudget, true, &si);
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::FeatureOff),
				static_cast<int>(ctrl.OverloadState().step), L"Hold band must not trigger recovery");

			// Hysteresis recovery under u < 0.90 (5 s per step up, exact reverse order!).
			double t = 25000.0;
			ctrl.Step(t, underBudget, true, &si); // starts m_underSince = 25000
			ctrl.Step(t + 4999.0, underBudget, true, &si);
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::FeatureOff),
				static_cast<int>(ctrl.OverloadState().step), L"Must hold full 5 s under u<0.90 before stepping up");

			t += 5000.0; // FeatureOff -> CoreDeferred
			ctrl.Step(t, underBudget, true, &si);
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::CoreDeferred), static_cast<int>(ctrl.OverloadState().step));

			t += 5000.0; // CoreDeferred -> ExtrasStepDown
			ctrl.Step(t, underBudget, true, &si);
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::ExtrasStepDown), static_cast<int>(ctrl.OverloadState().step));
			Assert::IsFalse(ctrl.ExtrasAllowed());

			t += 5000.0; // ExtrasStepDown -> TdStepDown (tier 0; ExtrasAllowed restored first!)
			Assert::AreEqual(static_cast<int>(TdTier::OffBudget), static_cast<int>(ctrl.Step(t, underBudget, true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::TdStepDown), static_cast<int>(ctrl.OverloadState().step));
			Assert::IsTrue(ctrl.ExtrasAllowed(), L"Extras must restore before TD climbs back up");

			t += 5000.0; // TdStepDown 0 -> 1 (LocalPeaksOnly)
			Assert::AreEqual(static_cast<int>(TdTier::LocalPeaksOnly), static_cast<int>(ctrl.Step(t, underBudget, true, &si)));

			t += 5000.0; // TdStepDown 1 -> 2 (ReducedAnchor)
			Assert::AreEqual(static_cast<int>(TdTier::ReducedAnchor), static_cast<int>(ctrl.Step(t, underBudget, true, &si)));

			t += 5000.0; // TdStepDown 2 -> 3 (Full, FillOff)
			Assert::AreEqual(static_cast<int>(TdTier::Full), static_cast<int>(ctrl.Step(t, underBudget, true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::FillOff), static_cast<int>(ctrl.OverloadState().step));
			Assert::IsFalse(si.IsActive(StandInKind::TierSteppedDown), L"TierSteppedDown must clear once back at Full");

			t += 5000.0; // FillOff -> None (full recovery!)
			Assert::AreEqual(static_cast<int>(TdTier::Full), static_cast<int>(ctrl.Step(t, underBudget, true, &si)));
			Assert::AreEqual(static_cast<int>(VpSkeleton::OverloadStep::None), static_cast<int>(ctrl.OverloadState().step));
			Assert::IsTrue(ctrl.FillAllowed() && ctrl.ExtrasAllowed());
			Assert::IsFalse(ctrl.FallbackDecision().engaged, L"FallbackDecision must release at full recovery");
			Assert::AreEqual(static_cast<unsigned long long>(7u), ctrl.StepsUp());

			// ---- Every active tier preserves STRICT/CELL integrity & NEVER uses L3 clamp for budget ----
			for (ResidualMode rm : { ResidualMode::Cell, ResidualMode::Strict })
			{
				for (TdTier tier : { TdTier::Full, TdTier::ReducedAnchor, TdTier::LocalPeaksOnly, TdTier::OffBudget })
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.residualMode = rm;
					StandInRegistry tierSi;
					const TierSpec spec = ApplyTierToConfig(cfg, tier, &tierSi);

					Assert::IsTrue(spec.preservesCellIntegrity, L"Every tier must preserve STRICT/CELL integrity");
					Assert::IsFalse(spec.usesL3ClampForBudget, L"No tier may use L3 clamp for GPU budget (99 H3)");
					Assert::AreEqual(static_cast<int>(rm), static_cast<int>(cfg.residualMode),
						L"ApplyTierToConfig must NEVER mutate ResidualMode");
					Assert::IsFalse(tierSi.IsActive(StandInKind::LowConfidenceL3),
						L"ApplyTierToConfig must NEVER engage LowConfidenceL3");
					Assert::AreEqual(tier != TdTier::Full, tierSi.IsActive(StandInKind::TierSteppedDown),
						L"TierSteppedDown must engage iff tier != Full");
					if (tier == TdTier::OffBudget)
						Assert::AreEqual(static_cast<int>(Mode::Off), static_cast<int>(cfg.mode));
					else
						Assert::AreEqual(static_cast<int>(Mode::On), static_cast<int>(cfg.mode));
				}
			}

			// ---- ApplyTierToConfig takes min(preset, tier): Preset::RealTime shortcuts SC-01/SC-02 are NEVER overridden by TdTier::Full! ----
			{
				Config rtCfg = MakePreset(Preset::RealTime, Mode::On);
				Assert::AreEqual(0, rtCfg.momentPoolRadiusCells, L"Preset::RealTime baseline must have momentPoolRadiusCells == 0 (SC-01)");
				Assert::IsFalse(rtCfg.enableLongAnchor, L"Preset::RealTime baseline must have enableLongAnchor == false (SC-02)");
				Assert::IsFalse(rtCfg.enableCarryForward, L"Preset::RealTime baseline must have enableCarryForward == false (Owner D2)");
				rtCfg.enableCarryForward = true; // Enable explicitly to verify Full preserves true and LocalPeaksOnly steps down to false

				StandInRegistry rtFullSi;
				const TierSpec rtFullSpec = ApplyTierToConfig(rtCfg, TdTier::Full, &rtFullSi);
				Assert::IsTrue(rtFullSpec.tdActive && rtFullSpec.preservesCellIntegrity && !rtFullSpec.usesL3ClampForBudget);
				Assert::AreEqual(0, rtCfg.momentPoolRadiusCells,
					L"ApplyTierToConfig(Full) on Preset::RealTime MUST preserve momentPoolRadiusCells == 0 (min(preset, tier))");
				Assert::IsFalse(rtCfg.enableLongAnchor,
					L"ApplyTierToConfig(Full) on Preset::RealTime MUST preserve enableLongAnchor == false (min(preset, tier))");
				Assert::IsTrue(rtCfg.enableCarryForward,
					L"ApplyTierToConfig(Full) on Preset::RealTime keeps enableCarryForward == true");
				Assert::AreEqual(static_cast<int>(Mode::On), static_cast<int>(rtCfg.mode));
				Assert::IsFalse(rtFullSi.IsActive(StandInKind::TierSteppedDown),
					L"ApplyTierToConfig(Full) must not engage TierSteppedDown");

				// Stepping RealTime down to LocalPeaksOnly still drops enableCarryForward cleanly.
				ApplyTierToConfig(rtCfg, TdTier::LocalPeaksOnly, &rtFullSi);
				Assert::AreEqual(0, rtCfg.momentPoolRadiusCells);
				Assert::IsFalse(rtCfg.enableLongAnchor);
				Assert::IsFalse(rtCfg.enableCarryForward,
					L"ApplyTierToConfig(LocalPeaksOnly) on Preset::RealTime must disable carryForward");
				Assert::IsTrue(rtFullSi.IsActive(StandInKind::TierSteppedDown));

				// Negative control: Preset::Reference under TdTier::Full DOES keep 3x3 pooling (1) and enableLongAnchor == true,
				// proving Full tier genuinely permits full Reference features while min(preset, tier) protects RealTime shortcuts!
				Config refCfg = MakePreset(Preset::Reference, Mode::On);
				ApplyTierToConfig(refCfg, TdTier::Full, nullptr);
				Assert::AreEqual(1, refCfg.momentPoolRadiusCells,
					L"Negative control: Preset::Reference + TdTier::Full keeps 3x3 moment pooling (1)");
				Assert::IsTrue(refCfg.enableLongAnchor,
					L"Negative control: Preset::Reference + TdTier::Full keeps enableLongAnchor == true");
				ApplyTierToConfig(refCfg, TdTier::ReducedAnchor, nullptr);
				Assert::AreEqual(1, refCfg.momentPoolRadiusCells);
				Assert::IsFalse(refCfg.enableLongAnchor,
					L"Negative control: Preset::Reference + ReducedAnchor drops enableLongAnchor to false");
			}

			// ---- Owner Q8b: Laptop native 4K (3840x2160, isLaptopBudget = true) -> ReducedAnchor visibly ----
			{
				StandInRegistry q8bSi;
				const TdTier lap4k = SelectInitialTier(3840, 2160, /*sliceMs=*/6.0, /*tdMinMs=*/2.0, /*tdWantMs=*/4.0,
					/*isLaptopBudget=*/true, &q8bSi);
				Assert::AreEqual(static_cast<int>(TdTier::ReducedAnchor), static_cast<int>(lap4k),
					L"Q8b: laptop native 4K must select ReducedAnchor tier");
				Assert::IsTrue(q8bSi.IsActive(StandInKind::TierSteppedDown),
					L"Q8b: laptop native 4K ReducedAnchor must engage TierSteppedDown visibly");

				// Desktop native 4K (isLaptopBudget = false) with ample slice stays Full!
				StandInRegistry deskSi;
				const TdTier desk4k = SelectInitialTier(3840, 2160, 6.0, 2.0, 4.0, /*isLaptopBudget=*/false, &deskSi);
				Assert::AreEqual(static_cast<int>(TdTier::Full), static_cast<int>(desk4k),
					L"Desktop 4K with ample slice must select Full tier");
				Assert::IsFalse(deskSi.IsActive(StandInKind::TierSteppedDown));

				// Laptop 1080p with ample slice stays Full!
				StandInRegistry lap1080Si;
				const TdTier lap1080 = SelectInitialTier(1920, 1080, 6.0, 2.0, 4.0, /*isLaptopBudget=*/true, &lap1080Si);
				Assert::AreEqual(static_cast<int>(TdTier::Full), static_cast<int>(lap1080),
					L"Laptop 1080p with ample slice must select Full tier");

				// Reconciled AdmissionTable.h contract (Rule 1: zero invented 0.5x threshold):
				// sliceMs <= 0 -> OffBudget, sliceMs < tdMinMs -> LocalPeaksOnly, sliceMs < tdWantMs -> ReducedAnchor, else Full.
				Assert::AreEqual(static_cast<int>(TdTier::OffBudget),
					static_cast<int>(SelectInitialTier(1920, 1080, 0.0, 2.0, 4.0, false, nullptr)),
					L"Zero sliceMs must select OffBudget");
				Assert::AreEqual(static_cast<int>(TdTier::LocalPeaksOnly),
					static_cast<int>(SelectInitialTier(1920, 1080, 0.8, 2.0, 4.0, false, nullptr)),
					L"Positive sliceMs < tdMinMs must select LocalPeaksOnly");
				Assert::AreEqual(static_cast<int>(TdTier::LocalPeaksOnly),
					static_cast<int>(SelectInitialTier(1920, 1080, 1.5, 2.0, 4.0, false, nullptr)),
					L"Positive sliceMs < tdMinMs must select LocalPeaksOnly");
				Assert::AreEqual(static_cast<int>(TdTier::ReducedAnchor),
					static_cast<int>(SelectInitialTier(1920, 1080, 3.0, 2.0, 4.0, false, nullptr)),
					L"tdMinMs <= sliceMs < tdWantMs must select ReducedAnchor");
				StandInRegistry nanSi;
				Assert::AreEqual(static_cast<int>(TdTier::OffBudget),
					static_cast<int>(SelectInitialTier(1920, 1080, std::numeric_limits<double>::quiet_NaN(), 2.0, 4.0, false, &nanSi)));
				Assert::IsTrue(nanSi.IsActive(StandInKind::NonFiniteClosure));

				// Reduced base tier with zero overload must report "base tier reduced" (NEVER confusing "overload: none").
				TdV2TierController reducedBaseCtrl(TdTier::ReducedAnchor);
				const FallbackCatalog::Decision redDec = reducedBaseCtrl.FallbackDecision();
				Assert::IsTrue(redDec.engaged, L"Reduced base tier must engage FallbackDecision visibly");
				Assert::IsTrue(redDec.reason.find("overload: none") == std::string::npos,
					L"Reduced base tier FallbackDecision must NOT say \"overload: none\"");
				Assert::IsTrue(redDec.reason.find("base tier reduced") != std::string::npos,
					L"Reduced base tier FallbackDecision must state \"base tier reduced\"");
			}

			// ---- REAL BEHAVIORAL NEGATIVE CONTROLS (Q7b & 99 H3 — zero tautological struct mutations!) ----
			// (a) Drive BOTH real TdV2TierController AND a naive reversed-order overload controller (same OverloadLoad
			//     signal + same kOverloadDownAfterMs = 2000 ms timer, stepping Extras down BEFORE TD) over the EXACT
			//     SAME (nowMs, overBudget) overload trace!
			{
				struct NaiveReversedPolicy
				{
					bool fillAllowed = true;
					bool extrasAllowed = true;
					int tdTier = 3;
					double overSinceMs = -1.0;

					void Step(double nowMs, const VpSkeleton::SlotBudget& b)
					{
						const double u = VpSkeleton::OverloadLoad(b);
						if (u > 1.0)
						{
							if (overSinceMs < 0.0)
								overSinceMs = nowMs;
							if (nowMs - overSinceMs >= VpSkeleton::kOverloadDownAfterMs)
							{
								overSinceMs = nowMs;
								if (fillAllowed) fillAllowed = false;
								else if (extrasAllowed) extrasAllowed = false; // WRONG: drops extras before TD!
								else if (tdTier > 0) --tdTier;
							}
						}
						else
						{
							overSinceMs = -1.0;
						}
					}
				};

				TdV2TierController realCtrl(TdTier::Full);
				NaiveReversedPolicy naiveCtrl;
				const double timestamps[] = { 0.0, 1000.0, 2000.0, 4000.0, 6000.0, 8000.0, 10000.0 };
				bool realProtectedExtrasAcrossAllTdSteps = true;
				bool naiveDroppedExtrasWhileTdActive = false;
				for (double ts : timestamps)
				{
					const TdTier realTier = realCtrl.Step(ts, overBudget, /*extrasActive=*/true);
					naiveCtrl.Step(ts, overBudget);
					if (realTier != TdTier::OffBudget && !realCtrl.ExtrasAllowed())
						realProtectedExtrasAcrossAllTdSteps = false;
					if (!naiveCtrl.extrasAllowed && naiveCtrl.tdTier > 0)
						naiveDroppedExtrasWhileTdActive = true;
				}
				Assert::IsTrue(realProtectedExtrasAcrossAllTdSteps,
					L"Q7b: Real TdV2TierController must keep ExtrasAllowed() == true while TD tier > 0");
				Assert::IsTrue(naiveDroppedExtrasWhileTdActive,
					L"Negative control (a): Naive reversed policy driven on exact same SlotBudget trace drops extras while TD > 0");
			}

			// (b) Real signal-level 8x8 DCT quantization-cell fixture proving ApplyTierToConfig(..., LocalPeaksOnly)
			//     preserves STRICT & CELL DCT integrity (0 violation), while a naive L3-clamp-only low-band budget
			//     substitution on the EXACT SAME input violates sent DCT coefficients / cell bounds by > 2.0 codes!
			{
				const int fw = 32;
				const int fh = 32;
				const double stepDelta = 24.0;
				const double phi = kDeadZonePhiInter;
				const double theta = (1.0 - phi) * stepDelta;
				const ActiveRect fullRect = ActiveRect::Full(fw, fh);

				// Clean flat peak references (P_B = 400.0) and current valley frame carrying a large sent DC
				// residual (+72.0 codes -> sent DCT DC = 8*72 = 576.0, level k = 24) + sub-threshold AC ripple (1.5 codes).
				const TdV2Plane refPeak(fw, fh, 400.0);
				TdV2Plane curValley(fw, fh, 400.0);
				for (int y = 0; y < fh; ++y)
				{
					for (int x = 0; x < fw; ++x)
					{
						curValley.at(x, y) = 472.0 + 1.5 * std::cos(3.14159265358979323846 * (2.0 * x + 1.0) * 5.0 / 16.0);
					}
				}
				const CellMotionField stillMf = CellMotionField::Uniform(2, 2, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				FrameProcessInput fin;
				fin.seq = 1;
				fin.receivedY = curValley;
				fin.hasCleanSplit = true;
				fin.cleanY = curValley;
				fin.activeRect = fullRect;
				fin.hasStepPrior = true;
				fin.estimatedStepDelta = stepDelta;
				fin.deadZonePhi = phi;
				fin.zeroCoeffFraction = 0.5;
				fin.transformIdentified8x8 = true;
				fin.prevPeak.cleanPlane = &refPeak;
				fin.prevPeak.motion = &stillMf;
				fin.nextPeak.cleanPlane = &refPeak;
				fin.nextPeak.motion = &stillMf;
				fin.codecModelPb = &refPeak;

				auto measureMaxDctViolationCodes = [&](const TdV2Plane& candidate, ResidualMode checkMode) -> double
				{
					double maxViolCodes = 0.0;
					for (int by = 0; by < fh; by += 8)
					{
						for (int bx = 0; bx < fw; bx += 8)
						{
							double sentRes[64] = {};
							double candRes[64] = {};
							for (int dy = 0; dy < 8; ++dy)
							{
								for (int dx = 0; dx < 8; ++dx)
								{
									sentRes[dy * 8 + dx] = curValley.at(bx + dx, by + dy) - refPeak.at(bx + dx, by + dy);
									candRes[dy * 8 + dx] = candidate.at(bx + dx, by + dy) - refPeak.at(bx + dx, by + dy);
								}
							}
							double sentDct[64] = {};
							double candDct[64] = {};
							ForwardDct8x8(sentRes, sentDct);
							ForwardDct8x8(candRes, candDct);
							for (int i = 0; i < 64; ++i)
							{
								const double sc = sentDct[i];
								const double cc = candDct[i];
								double violDct = 0.0;
								if (std::fabs(sc) >= theta)
								{
									if (checkMode == ResidualMode::Strict)
									{
										violDct = std::fabs(cc - sc);
									}
									else
									{
										const double mag = std::floor(std::fabs(sc) / stepDelta + phi);
										const double lvl = (sc >= 0.0) ? mag : -mag;
										const double lo = (lvl > 0.0) ? ((lvl - phi) * stepDelta) : ((lvl - 1.0 + phi) * stepDelta);
										const double hi = (lvl > 0.0) ? ((lvl + 1.0 - phi) * stepDelta) : ((lvl + phi) * stepDelta);
										if (cc < lo) violDct = lo - cc;
										else if (cc > hi) violDct = cc - hi;
									}
								}
								// Normalize 8x8 orthonormal DCT units by 8 to pixel-equivalent code amplitude.
								const double violCodes = violDct / 8.0;
								if (violCodes > maxViolCodes)
									maxViolCodes = violCodes;
							}
						}
					}
					return maxViolCodes;
				};

				// Run real TdV2ReferenceEngine under LocalPeaksOnly tier (ApplyTierToConfig) for BOTH Strict and Cell!
				Config cfgStrict = MakePreset(Preset::Reference, Mode::On);
				cfgStrict.residualMode = ResidualMode::Strict;
				StandInRegistry siStrict;
				const TierSpec spStrict = ApplyTierToConfig(cfgStrict, TdTier::LocalPeaksOnly, &siStrict);
				Assert::IsTrue(spStrict.preservesCellIntegrity && !spStrict.usesL3ClampForBudget);
				Assert::IsFalse(siStrict.IsActive(StandInKind::LowConfidenceL3));
				TdV2ReferenceEngine engStrict(cfgStrict);
				const FrameProcessResult resStrict = engStrict.ProcessFrame(fin, &siStrict);
				const double strictViol = measureMaxDctViolationCodes(resStrict.outputClean, ResidualMode::Strict);
				Assert::IsTrue(strictViol < 1e-5,
					FormatMsg("LocalPeaksOnly STRICT violated sent DCT coeff by %.6f codes", strictViol).c_str());

				Config cfgCell = MakePreset(Preset::Reference, Mode::On);
				cfgCell.residualMode = ResidualMode::Cell;
				StandInRegistry siCell;
				const TierSpec spCell = ApplyTierToConfig(cfgCell, TdTier::LocalPeaksOnly, &siCell);
				Assert::IsTrue(spCell.preservesCellIntegrity && !spCell.usesL3ClampForBudget);
				TdV2ReferenceEngine engCell(cfgCell);
				const FrameProcessResult resCell = engCell.ProcessFrame(fin, &siCell);
				const double cellViol = measureMaxDctViolationCodes(resCell.outputClean, ResidualMode::Cell);
				Assert::IsTrue(cellViol < 1e-5,
					FormatMsg("LocalPeaksOnly CELL violated DCT cell I_k by %.6f codes", cellViol).c_str());
				// Non-vacuity: engine actually modified outputClean within the legal quantization cell!
				Assert::IsTrue(MaxAbsDiffPlanes(resCell.outputClean, curValley) > 0.1,
					L"LocalPeaksOnly CELL run must genuinely modify pixels within I_k (non-vacuous)");

				// NEGATIVE CONTROL (b): Naive L3 coarse-band range-clamp carry (ignoring quantization cells I_k to
				// save GPU budget) on the EXACT SAME input violates STRICT & CELL bounds by >> 2.0 codes!
				TdV2Plane naiveL3Out(fw, fh, 0.0);
				for (int y = 0; y < fh; ++y)
				{
					for (int x = 0; x < fw; ++x)
					{
						const double lo = (std::min)(curValley.at(x, y), refPeak.at(x, y));
						const double hi = (std::max)(curValley.at(x, y), refPeak.at(x, y));
						const double carry = 0.5 * (curValley.at(x, y) + refPeak.at(x, y));
						naiveL3Out.at(x, y) = (std::max)(lo, (std::min)(hi, carry));
					}
				}
				const double naiveStrictViol = measureMaxDctViolationCodes(naiveL3Out, ResidualMode::Strict);
				const double naiveCellViol = measureMaxDctViolationCodes(naiveL3Out, ResidualMode::Cell);
				Assert::IsTrue(naiveStrictViol > 2.0,
					FormatMsg("Negative control (b): naive L3 clamp STRICT violation=%.2f <= 2.0 codes", naiveStrictViol).c_str());
				Assert::IsTrue(naiveCellViol > 2.0,
					FormatMsg("Negative control (b): naive L3 clamp CELL violation=%.2f <= 2.0 codes", naiveCellViol).c_str());
			}
		}

		// =========================================================================
		// 4. Owner Q7a Late-Frame Carry Policy (Up to 2 Frames, Decay 2/3 -> 1/3 -> 0,
		//    Warped & Cell-Bounded, Overlay/Bars 0.0, Never L3 Clamp, Resets on Cut)
		// =========================================================================
		TEST_METHOD(LateCarryPolicyQ7aCarriesUpTo2FramesNeverL3ClampAndResetsOnCut)
		{
			const int w = 64;
			const int h = 64;
			const int cellPx = 16;
			const int cellsW = 4;
			const int cellsH = 4;
			const ActiveRect active{ 0, 8, 64, 56 }; // Letterbox bars: y < 8 and y >= 56
			const double stepTotalVar = 16.0;        // sqrt(16) = 4.0 -> explicit kappa=3.0 cell bound = 3.0 * 4.0 = 12.0
			LateCarryContext kappa3Ctx{};
			kappa3Ctx.pixelBoundKappa = 3.0;
			const double expectedBound = kappa3Ctx.pixelBoundKappa * std::sqrt(stepTotalVar);

			TdV2Plane curClean(w, h, 420.0);
			TdV2Plane prevDelta(w, h, 0.0);

			// Plant structured delta patterns across:
			//  - Letterbox bars (y < 8, y >= 56): +25.0 (MUST be masked to exact 0.0!)
			//  - Overlay cell (0, 1) [x in 0..16, y in 16..32): +30.0 (MUST be masked to exact 0.0!)
			//  - DynamicTexture cell (1, 2) [x in 16..32, y in 32..48): +28.0 (MUST be masked to exact 0.0!)
			//  - Interior moderate active Camera region [x in 20..44, y in 12..32): +9.0 (unclipped: 9*2/3 = 6.0, 9*1/3 = 3.0)
			//  - Interior excessive spike in cell (2, 2) [x in 34..46, y in 34..46): +60.0 (MUST clamp to expectedBound = 12.0!)
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					if (y < active.y0 || y >= active.y1)
						prevDelta.at(x, y) = 25.0;
					else if (x < 16 && y >= 16 && y < 32)
						prevDelta.at(x, y) = 30.0;
					else if (x >= 16 && x < 34 && y >= 32 && y < 48)
						prevDelta.at(x, y) = 28.0;
					else if (x >= 34 && x < 46 && y >= 34 && y < 46)
						prevDelta.at(x, y) = 60.0;
					else if (x >= 20 && x < 44 && y >= 12 && y < 32)
						prevDelta.at(x, y) = 9.0;
				}
			}

			// Panning motion field: (vx = +2.0, vy = +1.0) on Camera cells; Overlay on (0, 1); DynamicTexture on (1, 2); invalid on (3, 2).
			CellMotionField mf = CellMotionField::Uniform(cellsW, cellsH, 2.0, 1.0, 2.0, 1.0, RegionMotionClass::Camera);
			mf.cls[1u * cellsW + 0u] = RegionMotionClass::Overlay;
			mf.cls[2u * cellsW + 1u] = RegionMotionClass::DynamicTexture;
			mf.valid[2u * cellsW + 3u] = 0u;

			TdV2LateCarryController ctrl;
			StandInRegistry si;
			ctrl.OnOnTimeFrame(prevDelta, &si);
			Assert::IsTrue(ctrl.HasStoredDelta(), L"OnOnTimeFrame must store delta");
			Assert::AreEqual(0, ctrl.LateStreak());
			Assert::IsFalse(si.IsActive(StandInKind::LateCarryActive));
			Assert::IsFalse(si.IsActive(StandInKind::LateCarryExpired));

			const ActiveRect topBar{ 0, 0, w, active.y0 };
			const ActiveRect botBar{ 0, active.y1, w, h };
			const ActiveRect overlayRect{ 0, 16, 16, 32 };
			const ActiveRect dynTexRect{ 16, 32, 32, 48 };
			const ActiveRect invalidCellRect{ 48, 32, 64, 48 };

			// ---- Late Frame 1: decay = 2/3, 1-hop warped (x+2, y+1), cell-bounded, bars/Overlay/DynamicTexture/invalid = 0.0 ----
			const uint64_t boundHitsBefore1 = si.HitCount(StandInKind::TransformUnknownBound);
			const LateCarryDecision d1 = ctrl.OnLateFrame(curClean, active, &mf, stepTotalVar, cellsW, cellsH, cellPx, &si, &kappa3Ctx);
			const uint64_t boundHitsFrame1 = si.HitCount(StandInKind::TransformUnknownBound) - boundHitsBefore1;
			Assert::IsTrue(boundHitsFrame1 <= 1u,
				FormatMsg("TransformUnknownBound hitCount per late frame must be <= 1 (not W*H), got %.0f",
					static_cast<double>(boundHitsFrame1)).c_str());
			Assert::IsTrue(d1.carried, L"Late frame 1 must carry");
			Assert::IsFalse(d1.expired, L"Late frame 1 must not expire");
			Assert::IsFalse(d1.usesL3Clamp, L"Q7a: Late frame 1 must NEVER use L3 clamp");
			Assert::AreEqual(1, d1.lateStreak);
			Assert::AreEqual(2.0 / 3.0, d1.decayScale, 1e-12, L"Late frame 1 decay must be 2/3");
			Assert::IsTrue(d1.fallback.engaged, L"Late frame 1 must engage FallbackCatalog");
			Assert::AreEqual(std::string("product.temporal_detail"), d1.fallback.id,
				L"Late frame 1 FallbackCatalog id must be product.temporal_detail");
			Assert::IsTrue(si.IsActive(StandInKind::LateCarryActive), L"LateCarryActive must engage on late frame 1");
			Assert::IsFalse(si.IsActive(StandInKind::LateCarryExpired), L"LateCarryExpired must be clear on late frame 1");
			Assert::IsFalse(si.IsActive(StandInKind::LowConfidenceL3), L"Q7a: LowConfidenceL3 must NEVER engage on late carry");

			// Exact 0.0 on bars, Overlay, DynamicTexture, and invalid motion cell.
			Assert::AreEqual(0.0, MaxAbsPlaneRect(d1.delta, topBar), L"Late frame 1 leaked into top letterbox bar");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(d1.delta, botBar), L"Late frame 1 leaked into bottom letterbox bar");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(d1.delta, overlayRect), L"Late frame 1 leaked into Overlay cell");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(d1.delta, dynTexRect), L"Late frame 1 leaked into DynamicTexture cell");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(d1.delta, invalidCellRect), L"Late frame 1 leaked into invalid motion cell");

			// Cell bound strictly respected everywhere, AND interior pixel genuinely warped & carried!
			const double maxD1 = MaxAbsPlaneRect(d1.delta, active);
			Assert::IsTrue(maxD1 <= expectedBound + 1e-9,
				FormatMsg("Late frame 1 exceeded cell bound: max=%.4f > %.4f", maxD1, expectedBound).c_str());
			// Moderate interior pixel (30, 20): samples prevDelta at (32, 21) == 9.0 -> 9.0 * 2/3 = 6.0!
			Assert::AreEqual(6.0, d1.delta.at(30, 20), 1e-6, L"Late frame 1 interior warped delta must equal 6.0");
			// Spike interior pixel (38, 38): samples prevDelta at (40, 39) == 60.0 -> 40.0 clamped to expectedBound (12.0)!
			Assert::AreEqual(expectedBound, d1.delta.at(38, 38), 1e-6, L"Late frame 1 spike must clamp to cell bound");
			// 1-hop pan check: (18, 19) samples prevDelta(20, 20) == 9.0 -> 6.0, while (16, 18) samples prevDelta(18, 19) == 0.0!
			Assert::AreEqual(6.0, d1.delta.at(18, 19), 1e-6, L"Late frame 1 1-hop pan at (18,19) must equal 6.0");
			Assert::AreEqual(0.0, d1.delta.at(16, 18), 1e-6, L"Late frame 1 at (16,18) must still be 0.0 before 2nd hop");

			// ---- Late Frame 2: decay = 1/3, 2-hop pan alignment (x+4, y+2), bounded, bars/Overlay/DynamicTexture 0.0 ----
			const uint64_t boundHitsBefore2 = si.HitCount(StandInKind::TransformUnknownBound);
			const LateCarryDecision d2 = ctrl.OnLateFrame(curClean, active, &mf, stepTotalVar, cellsW, cellsH, cellPx, &si, &kappa3Ctx);
			const uint64_t boundHitsFrame2 = si.HitCount(StandInKind::TransformUnknownBound) - boundHitsBefore2;
			Assert::IsTrue(boundHitsFrame2 <= 1u,
				FormatMsg("Late frame 2 TransformUnknownBound hitCount must be <= 1, got %.0f",
					static_cast<double>(boundHitsFrame2)).c_str());
			Assert::IsTrue(d2.carried, L"Late frame 2 must carry");
			Assert::IsFalse(d2.expired, L"Late frame 2 must not expire");
			Assert::IsFalse(d2.usesL3Clamp, L"Q7a: Late frame 2 must NEVER use L3 clamp");
			Assert::AreEqual(2, d2.lateStreak);
			Assert::AreEqual(1.0 / 3.0, d2.decayScale, 1e-12, L"Late frame 2 decay must be 1/3");
			Assert::IsTrue(d2.decayScale < d1.decayScale, L"Late frame 2 decay must be strictly smaller than late frame 1");
			Assert::AreEqual(3.0, d2.delta.at(30, 20), 1e-6, L"Late frame 2 interior delta must decay to 3.0");
			Assert::IsTrue(std::fabs(d2.delta.at(30, 20)) < std::fabs(d1.delta.at(30, 20)),
				L"Late frame 2 carried magnitude must be strictly smaller than late frame 1");
			// 2-hop pan alignment check: (16, 18) samples warped1(18, 19) == prevDelta(20, 20) == 9.0 -> 9.0 * 1/3 = 3.0!
			Assert::AreEqual(3.0, d2.delta.at(16, 18), 1e-6,
				L"Late frame 2 must advance carried delta along 2-hop pan (x+4, y+2): (16,18) must equal 3.0");
			Assert::IsTrue(MaxAbsPlaneRect(d2.delta, active) <= expectedBound + 1e-9, L"Late frame 2 must obey cell bound");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(d2.delta, topBar), L"Late frame 2 leaked into top bar");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(d2.delta, botBar), L"Late frame 2 leaked into bottom bar");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(d2.delta, overlayRect), L"Late frame 2 leaked into Overlay");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(d2.delta, dynTexRect), L"Late frame 2 leaked into DynamicTexture");
			Assert::IsTrue(si.IsActive(StandInKind::LateCarryActive));

			// ---- Late Frame 3: EXPIRED -> exact 0.0 passthrough, LateCarryExpired engaged ----
			const LateCarryDecision d3 = ctrl.OnLateFrame(curClean, active, &mf, stepTotalVar, cellsW, cellsH, cellPx, &si);
			Assert::IsFalse(d3.carried, L"Late frame 3 must NOT carry (2-frame cap)");
			Assert::IsTrue(d3.expired, L"Late frame 3 must be expired");
			Assert::IsFalse(d3.usesL3Clamp, L"Q7a: Late frame 3 must NEVER use L3 clamp");
			Assert::AreEqual(3, d3.lateStreak);
			Assert::AreEqual(0.0, d3.decayScale, 1e-12, L"Late frame 3 decay must be 0.0");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(d3.delta, ActiveRect::Full(w, h)),
				L"Late frame 3 must be exact 0.0 passthrough everywhere");
			Assert::IsFalse(si.IsActive(StandInKind::LateCarryActive), L"LateCarryActive must clear when expired");
			Assert::IsTrue(si.IsActive(StandInKind::LateCarryExpired), L"LateCarryExpired must engage on late frame 3");
			Assert::IsTrue(d3.fallback.engaged && d3.fallback.id == "product.temporal_detail");

			// ---- Non-resurrection check: if late frame 1 expires (null motion), late frame 2 stays expired until OnOnTimeFrame! ----
			{
				StandInRegistry nrSi;
				ctrl.OnOnTimeFrame(prevDelta, &nrSi);
				const LateCarryDecision dNull1 = ctrl.OnLateFrame(curClean, active, nullptr, stepTotalVar, cellsW, cellsH, cellPx, &nrSi);
				Assert::IsFalse(dNull1.carried, L"Null carryMotion on late frame 1 must expire");
				Assert::IsTrue(dNull1.expired, L"Null carryMotion on late frame 1 must set expired == true");
				Assert::IsTrue(nrSi.IsActive(StandInKind::MotionFieldStandIn), L"Null carryMotion must engage MotionFieldStandIn");

				const LateCarryDecision dNoResurrect = ctrl.OnLateFrame(curClean, active, &mf, stepTotalVar, cellsW, cellsH, cellPx, &nrSi);
				Assert::IsFalse(dNoResurrect.carried, L"Late frame 2 after expired frame 1 must NOT resurrect stale delta");
				Assert::IsTrue(dNoResurrect.expired, L"Late frame 2 after expired frame 1 must stay expired until OnOnTimeFrame");
				Assert::AreEqual(0.0, MaxAbsPlaneRect(dNoResurrect.delta, ActiveRect::Full(w, h)),
					L"Non-resurrection frame must be exact 0.0 passthrough everywhere");
			}

			// ---- Cut / Reset (OnCutOrReset): immediate next late frame returns exact 0.0 passthrough ----
			ctrl.OnOnTimeFrame(prevDelta, &si);
			Assert::IsTrue(ctrl.HasStoredDelta());
			ctrl.OnCutOrReset(&si);
			Assert::IsFalse(ctrl.HasStoredDelta(), L"OnCutOrReset must clear stored delta");
			Assert::AreEqual(0, ctrl.LateStreak());
			Assert::IsFalse(si.IsActive(StandInKind::LateCarryActive));
			Assert::IsFalse(si.IsActive(StandInKind::LateCarryExpired));

			const LateCarryDecision dCut = ctrl.OnLateFrame(curClean, active, &mf, stepTotalVar, cellsW, cellsH, cellPx, &si);
			Assert::IsFalse(dCut.carried, L"Late frame immediately after cut must NOT carry across cut");
			Assert::IsTrue(dCut.expired, L"Late frame immediately after cut must expire cleanly to passthrough");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(dCut.delta, ActiveRect::Full(w, h)),
				L"Cross-cut late frame must have exact 0.0 delta everywhere (zero ghosting)");

			// ---- Non-finite stepTotalVar fails closed visibly ----
			{
				ctrl.OnOnTimeFrame(prevDelta, &si);
				StandInRegistry nfSi;
				const LateCarryDecision dNf = ctrl.OnLateFrame(
					curClean, active, &mf, std::numeric_limits<double>::quiet_NaN(), cellsW, cellsH, cellPx, &nfSi);
				Assert::IsFalse(dNf.carried);
				Assert::IsTrue(dNf.expired);
				Assert::AreEqual(0.0, MaxAbsPlaneRect(dNf.delta, ActiveRect::Full(w, h)));
				Assert::IsTrue(nfSi.IsActive(StandInKind::NonFiniteClosure), L"NaN stepTotalVar must engage NonFiniteClosure");
			}

			// ---- NEGATIVE CONTROLS (Exact Same prevDelta & Motion!) ----
			// (a) Naive un-bounded, un-masked carry (shifts prevDelta by (vx, vy) * 2/3 with NO ActiveRect,
			//     NO Overlay mask, and NO cell bound) leaks into bars & Overlay and blows past expectedBound!
			{
				TdV2Plane naiveUnbounded(w, h, 0.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const int sx = (std::max)(0, (std::min)(w - 1, x + 2));
						const int sy = (std::max)(0, (std::min)(h - 1, y + 1));
						naiveUnbounded.at(x, y) = prevDelta.at(sx, sy) * (2.0 / 3.0);
					}
				}
				const double naiveBarLeak = MaxAbsPlaneRect(naiveUnbounded, topBar);
				const double naiveOverlayLeak = MaxAbsPlaneRect(naiveUnbounded, overlayRect);
				const double naiveDynTexLeak = MaxAbsPlaneRect(naiveUnbounded, dynTexRect);
				const double naiveMaxActive = MaxAbsPlaneRect(naiveUnbounded, active);
				Assert::IsTrue(naiveBarLeak > 10.0,
					FormatMsg("Negative control (a): naive carry bar leak=%.2f <= 10.0", naiveBarLeak).c_str());
				Assert::IsTrue(naiveOverlayLeak > 15.0,
					FormatMsg("Negative control (a): naive carry Overlay leak=%.2f <= 15.0", naiveOverlayLeak).c_str());
				Assert::IsTrue(naiveDynTexLeak > 15.0,
					FormatMsg("Negative control (a): naive carry DynamicTexture leak=%.2f <= 15.0", naiveDynTexLeak).c_str());
				Assert::IsTrue(naiveMaxActive > expectedBound + 20.0,
					FormatMsg("Negative control (a): naive carry max=%.2f <= bound+20 (%.2f)", naiveMaxActive, expectedBound + 20.0).c_str());
			}
			// (b) Naive un-capped carry (exponential 0.5^k without 2-frame cap or cut reset) still applies stale
			//     delta on late frame 3 and leaks across a scene cut!
			{
				const double naiveFrame3Delta = prevDelta.at(32, 21) * std::pow(0.5, 3.0); // 9.0 * 0.125 = 1.125 > 1.0
				const double naivePostCutDelta = prevDelta.at(40, 39) * 0.5;              // 60.0 * 0.5 = 30.0 > 10.0
				Assert::IsTrue(naiveFrame3Delta > 1.0,
					FormatMsg("Negative control (b): naive un-capped frame-3 stale delta=%.3f <= 1.0", naiveFrame3Delta).c_str());
				Assert::IsTrue(naivePostCutDelta > 10.0,
					FormatMsg("Negative control (b): naive un-reset post-cut ghost=%.2f <= 10.0", naivePostCutDelta).c_str());
			}

			// ---- Active Bounds & Residual Consistency (8x8 STRICT / CELL Awareness when Transform Identified) ----
			{
				const int tw = 32;
				const int th = 32;
				const int tCw = 2;
				const int tCh = 2;
				const ActiveRect tFull = ActiveRect::Full(tw, th);
				const ActiveRect leftHalf{ 0, 0, 16, th };
				const ActiveRect rightHalf{ 16, 0, tw, th };
				const double tStep = 24.0;
				const double tPhi = kDeadZonePhiInter;
				const double tTheta = (1.0 - tPhi) * tStep;

				// Clean Field B prediction P_B = 400.0; current late frame carries strong sent DC residual (+72.0 codes
				// -> sent DCT DC = 576.0, k = 24, cell I_24 = [568, 592] i.e. pixel DC in [71.0, 74.0]) + small AC ripple.
				const TdV2Plane pbPlane(tw, th, 400.0);
				TdV2Plane lateCur(tw, th, 400.0);
				for (int y = 0; y < th; ++y)
					for (int x = 0; x < tw; ++x)
						lateCur.at(x, y) = 472.0 + 1.5 * std::cos(3.14159265358979323846 * (2.0 * x + 1.0) * 5.0 / 16.0);

				// Stored on-time delta:
				//   - Left half (x < 16):  +18.0 (OPPOSES desiredDir = ref - cur ~ -72 -> MUST be residual-rejected!)
				//   - Right half (x >= 16): -30.0 (AGREES with desiredDir ~ -72 -> carried, then projected onto STRICT/CELL!)
				TdV2Plane onTimeDelta(tw, th, 0.0);
				for (int y = 0; y < th; ++y)
					for (int x = 0; x < tw; ++x)
						onTimeDelta.at(x, y) = (x < 16) ? +18.0 : -30.0;

				const CellMotionField stillT = CellMotionField::Uniform(tCw, tCh, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				auto measureLateDctViolCodes = [&](const TdV2Plane& deltaPlane, ResidualMode checkMode) -> double
				{
					double maxViol = 0.0;
					for (int by = 0; by < th; by += 8)
					{
						for (int bx = 0; bx < tw; bx += 8)
						{
							double sentRes[64] = {}, candRes[64] = {}, sentDct[64] = {}, candDct[64] = {};
							for (int dy = 0; dy < 8; ++dy)
							{
								for (int dx = 0; dx < 8; ++dx)
								{
									const double c = lateCur.at(bx + dx, by + dy);
									const double p = pbPlane.at(bx + dx, by + dy);
									sentRes[dy * 8 + dx] = c - p;
									candRes[dy * 8 + dx] = (c + deltaPlane.at(bx + dx, by + dy)) - p;
								}
							}
							ForwardDct8x8(sentRes, sentDct);
							ForwardDct8x8(candRes, candDct);
							for (int i = 0; i < 64; ++i)
							{
								const double sc = sentDct[i];
								const double cc = candDct[i];
								if (std::fabs(sc) < tTheta)
									continue;
								double violDct = 0.0;
								if (checkMode == ResidualMode::Strict)
								{
									violDct = std::fabs(cc - sc);
								}
								else
								{
									const double mag = std::floor(std::fabs(sc) / tStep + tPhi);
									const double lvl = (sc >= 0.0) ? mag : -mag;
									const double lo = (lvl > 0.0) ? ((lvl - tPhi) * tStep) : ((lvl - 1.0 + tPhi) * tStep);
									const double hi = (lvl > 0.0) ? ((lvl + 1.0 - tPhi) * tStep) : ((lvl + tPhi) * tStep);
									if (cc < lo) violDct = lo - cc;
									else if (cc > hi) violDct = cc - hi;
								}
								const double violCodes = violDct / 8.0;
								if (violCodes > maxViol) maxViol = violCodes;
							}
						}
					}
					return maxViol;
				};

				// 1. Transform-identified 8x8 STRICT with explicit codecModelPb & referenceClean
				{
					TdV2LateCarryController strictCtrl;
					StandInRegistry strictSi;
					strictCtrl.OnOnTimeFrame(onTimeDelta, &strictSi);
					LateCarryContext strictCtx;
					strictCtx.transformIdentified8x8 = true;
					strictCtx.residualMode = ResidualMode::Strict;
					strictCtx.stepDelta = tStep;
					strictCtx.deadZonePhi = tPhi;
					strictCtx.sourceBits = 10;
					strictCtx.cellProjIterations = 2;
					strictCtx.codecModelPb = &pbPlane;
					strictCtx.referenceClean = &pbPlane;

					const LateCarryDecision dStrict = strictCtrl.OnLateFrame(
						lateCur, tFull, &stillT, stepTotalVar, tCw, tCh, cellPx, &strictSi, &strictCtx);
					Assert::IsTrue(dStrict.carried && !dStrict.expired && !dStrict.usesL3Clamp,
						L"Late carry STRICT must succeed without L3 clamp");
					Assert::IsTrue(dStrict.usedTransformProjection,
						L"Late carry STRICT must run 8x8 DCT projection");
					Assert::AreEqual(static_cast<size_t>(16u * th), dStrict.residualRejectedPixels,
						L"Late carry STRICT must reject all opposing-residual pixels on left half");
					Assert::AreEqual(0.0, MaxAbsPlaneRect(dStrict.delta, leftHalf), 1e-9,
						L"Late carry STRICT opposing left-half delta must be zeroed");
					Assert::IsFalse(strictSi.IsActive(StandInKind::CodecModelPbStandIn),
						L"Explicit codecModelPb must NOT engage CodecModelPbStandIn");
					const double strictViol = measureLateDctViolCodes(dStrict.delta, ResidualMode::Strict);
					Assert::IsTrue(strictViol < 1e-5,
						FormatMsg("Late carry STRICT violated sent DCT coeff by %.6f codes", strictViol).c_str());
				}

				// 2. Transform-identified 8x8 CELL with explicit codecModelPb & referenceClean
				{
					TdV2LateCarryController cellCtrl;
					StandInRegistry cellSi;
					cellCtrl.OnOnTimeFrame(onTimeDelta, &cellSi);
					LateCarryContext cellCtx;
					cellCtx.transformIdentified8x8 = true;
					cellCtx.residualMode = ResidualMode::Cell;
					cellCtx.stepDelta = tStep;
					cellCtx.deadZonePhi = tPhi;
					cellCtx.sourceBits = 10;
					cellCtx.cellProjIterations = 2;
					cellCtx.codecModelPb = &pbPlane;
					cellCtx.referenceClean = &pbPlane;

					const LateCarryDecision dCell = cellCtrl.OnLateFrame(
						lateCur, tFull, &stillT, stepTotalVar, tCw, tCh, cellPx, &cellSi, &cellCtx);
					Assert::IsTrue(dCell.carried && !dCell.expired && !dCell.usesL3Clamp,
						L"Late carry CELL must succeed without L3 clamp");
					Assert::IsTrue(dCell.usedTransformProjection,
						L"Late carry CELL must run 8x8 DCT projection");
					Assert::AreEqual(static_cast<size_t>(16u * th), dCell.residualRejectedPixels,
						L"Late carry CELL must reject all opposing-residual pixels on left half");
					Assert::AreEqual(0.0, MaxAbsPlaneRect(dCell.delta, leftHalf), 1e-9,
						L"Late carry CELL opposing left-half delta must be zeroed");
					const double cellViol = measureLateDctViolCodes(dCell.delta, ResidualMode::Cell);
					Assert::IsTrue(cellViol < 1e-5,
						FormatMsg("Late carry CELL violated DCT cell I_k by %.6f codes", cellViol).c_str());
					// Non-vacuity: right half genuinely moves within cell I_24 (72 -> 71 => |delta| >= 0.5)!
					const double cellRightMag = MaxAbsPlaneRect(dCell.delta, rightHalf);
					Assert::IsTrue(cellRightMag >= 0.5,
						FormatMsg("Late carry CELL non-vacuity: right-half |delta|=%.4f < 0.5", cellRightMag).c_str());
				}

				// 3. Missing Field B P_B (codecModelPb == nullptr) engages CodecModelPbStandIn visibly (Rule 4)
				{
					TdV2LateCarryController pbSiCtrl;
					StandInRegistry pbSi;
					pbSiCtrl.OnOnTimeFrame(onTimeDelta, &pbSi);
					LateCarryContext noPbCtx;
					noPbCtx.transformIdentified8x8 = true;
					noPbCtx.residualMode = ResidualMode::Cell;
					noPbCtx.stepDelta = tStep;
					noPbCtx.deadZonePhi = tPhi;
					noPbCtx.sourceBits = 10;
					noPbCtx.codecModelPb = nullptr;
					noPbCtx.referenceClean = &pbPlane;
					const LateCarryDecision dNoPb = pbSiCtrl.OnLateFrame(
						lateCur, tFull, &stillT, stepTotalVar, tCw, tCh, cellPx, &pbSi, &noPbCtx);
					Assert::IsTrue(dNoPb.carried && !dNoPb.usedTransformProjection);
					Assert::IsTrue(pbSi.IsActive(StandInKind::CodecModelPbStandIn),
						L"Late carry with null codecModelPb MUST engage StandInKind::CodecModelPbStandIn");
				}

				// 4. Invalid LateCarryContext (stepDelta <= 0 / NaN) expires cleanly & engages A2StatsInvalid visibly
				{
					TdV2LateCarryController badCtxCtrl;
					StandInRegistry badCtxSi;
					badCtxCtrl.OnOnTimeFrame(onTimeDelta, &badCtxSi);
					LateCarryContext badCtx;
					badCtx.transformIdentified8x8 = true;
					badCtx.stepDelta = std::numeric_limits<double>::quiet_NaN();
					badCtx.deadZonePhi = tPhi;
					const LateCarryDecision dBad = badCtxCtrl.OnLateFrame(
						lateCur, tFull, &stillT, stepTotalVar, tCw, tCh, cellPx, &badCtxSi, &badCtx);
					Assert::IsFalse(dBad.carried);
					Assert::IsTrue(dBad.expired);
					Assert::AreEqual(0.0, MaxAbsPlaneRect(dBad.delta, tFull));
					Assert::IsTrue(badCtxSi.IsActive(StandInKind::A2StatsInvalid),
						L"Invalid STRICT/CELL late-carry context must engage A2StatsInvalid");
					Assert::IsTrue(badCtxSi.IsActive(StandInKind::NonFiniteClosure),
						L"NaN stepDelta in late-carry context must engage NonFiniteClosure");
				}

				// 5. MANDATORY IN-TEST NEGATIVE CONTROL (Rule 3): Pixel-domain-only carry (ctx == nullptr)
				//    on the EXACT SAME (lateCur, onTimeDelta, stillT) fails to reject the opposing left-half delta
				//    AND violates BOTH STRICT & CELL DCT bounds by > 5.0 codes!
				{
					TdV2LateCarryController naivePixCtrl;
					naivePixCtrl.OnOnTimeFrame(onTimeDelta, nullptr);
					const LateCarryDecision dNaive = naivePixCtrl.OnLateFrame(
						lateCur, tFull, &stillT, stepTotalVar, tCw, tCh, cellPx, nullptr, &kappa3Ctx);
					Assert::IsTrue(dNaive.carried && !dNaive.usedTransformProjection);
					Assert::AreEqual(static_cast<size_t>(0u), dNaive.residualRejectedPixels,
						L"Negative control: pixel-only late carry does not check residual direction");
					Assert::IsTrue(MaxAbsPlaneRect(dNaive.delta, leftHalf) > 5.0,
						L"Negative control: pixel-only late carry leaks opposing +12 delta into left half");
					const double naiveStrictViol = measureLateDctViolCodes(dNaive.delta, ResidualMode::Strict);
					const double naiveCellViol = measureLateDctViolCodes(dNaive.delta, ResidualMode::Cell);
					Assert::IsTrue(naiveStrictViol > 5.0,
						FormatMsg("Negative control: pixel-only late carry STRICT violation=%.2f <= 5.0 codes", naiveStrictViol).c_str());
					Assert::IsTrue(naiveCellViol > 5.0,
						FormatMsg("Negative control: pixel-only late carry CELL violation=%.2f <= 5.0 codes", naiveCellViol).c_str());
				}
			}
		}

		// =========================================================================
		// 5. TD-S7 Settings, Config Schema/UI/Defaults, Original Pathway Bit-Exact,
		//    Q8a Dependencies, Live TdProducts::Snapshot, Shadow Mode & Ctrl+I Status
		// =========================================================================
		TEST_METHOD(SettingsDefaultOffOriginalPathwayBitExactConfigRoundTripAndCtrlIStatus)
		{
			// ---- Part A: VpvRenderer::Fields defaults ----
			{
				VpvRenderer::Fields fields;
				Assert::AreEqual(std::string("off"), fields.temporalDetail, L"VpvRenderer::Fields temporalDetail must default to off");
				Assert::AreEqual(std::string("cell"), fields.tdMode, L"VpvRenderer::Fields tdMode must default to cell");
				Assert::AreEqual(std::string("off"), fields.tdOverlay, L"VpvRenderer::Fields tdOverlay must default to off");
			}

			// ---- Part B: ProfileSchemaHooks::Detail::Input validation (valid accepted, invalid rejected) ----
			{
				using V = RendererProfileConfig::ProfileSchemaHooks::Verdict;
				for (const char* ok : { "off", "shadow", "on" })
				{
					Assert::AreEqual(static_cast<int>(V::Valid),
						static_cast<int>(RendererProfileConfig::ProfileSchemaHooks::Detail::Input("temporal_detail", ok)),
						L"Valid temporal_detail choice must be accepted");
					Assert::AreEqual(static_cast<int>(V::Valid),
						static_cast<int>(RendererProfileConfig::ProfileSchemaHooks::Validate("input", "temporal_detail", ok)));
				}
				for (const char* bad : { "auto", "true", "1", "l3", "" })
				{
					Assert::AreEqual(static_cast<int>(V::Invalid),
						static_cast<int>(RendererProfileConfig::ProfileSchemaHooks::Detail::Input("temporal_detail", bad)),
						L"Invalid temporal_detail value must be rejected");
				}

				for (const char* ok : { "cell", "strict" })
				{
					Assert::AreEqual(static_cast<int>(V::Valid),
						static_cast<int>(RendererProfileConfig::ProfileSchemaHooks::Detail::Input("td_mode", ok)));
				}
				for (const char* bad : { "l3", "auto", "fast", "" })
				{
					Assert::AreEqual(static_cast<int>(V::Invalid),
						static_cast<int>(RendererProfileConfig::ProfileSchemaHooks::Detail::Input("td_mode", bad)));
				}

				for (const char* ok : { "off", "weights", "maps", "focus", "motion" })
				{
					Assert::AreEqual(static_cast<int>(V::Valid),
						static_cast<int>(RendererProfileConfig::ProfileSchemaHooks::Detail::Input("td_overlay", ok)));
				}
				for (const char* bad : { "heatmap", "all", "on", "" })
				{
					Assert::AreEqual(static_cast<int>(V::Invalid),
						static_cast<int>(RendererProfileConfig::ProfileSchemaHooks::Detail::Input("td_overlay", bad)));
				}

				// Negative control: unknown key returns NotMine (not silently Valid!).
				Assert::AreEqual(static_cast<int>(V::NotMine),
					static_cast<int>(RendererProfileConfig::ProfileSchemaHooks::Detail::Input("td_nonexistent_key", "on")),
					L"Negative control: unknown key must return Verdict::NotMine");
			}

			// ---- Part C: Vpv::Organisation() Repair > Main > Temporal detail card placement ----
			{
				const auto& cards = Vpv::Organisation();
				const Vpv::Card* tdCard = nullptr;
				for (const auto& c : cards)
				{
					if (std::string(c.unit) == "repair" && std::string(c.id) == "temporal_detail")
					{
						tdCard = &c;
						break;
					}
				}
				Assert::IsNotNull(tdCard, L"Vpv::Organisation() must contain Repair > temporal_detail card");
				Assert::AreEqual(std::string("Temporal detail"), std::string(tdCard->title));
				Assert::IsFalse(tdCard->advanced, L"Temporal detail card must be on Repair > Main (advanced == false)");
				Assert::IsFalse(tdCard->collapsed, L"Temporal detail card must not be collapsed by default");

				bool hasTd = false, hasKappa = false, hasMode = false, hasOverlay = false;
				for (const auto& item : tdCard->items)
				{
					if (item.kind == Vpv::ItemKind::Toggle && std::string(item.id) == "temporal_detail") hasTd = true;
					if (item.kind == Vpv::ItemKind::Toggle && std::string(item.id) == "td_limit_kappa") hasKappa = true;
					if (item.kind == Vpv::ItemKind::Toggle && std::string(item.id) == "td_mode") hasMode = true;
					if (item.kind == Vpv::ItemKind::Toggle && std::string(item.id) == "td_overlay") hasOverlay = true;
				}
				Assert::IsTrue(hasTd && hasKappa && hasMode && hasOverlay,
					L"Repair > Main > Temporal detail card must expose Toggle controls temporal_detail, td_limit_kappa, td_mode, td_overlay (L-5 / Owner D7)");
			}

			// ---- Part C2 (H1 + L-5 / D7): Honest Config UI Drop-In Status in VpvOrganisation.h & VpvSection.cpp (NO false claim of live renderer wiring!) ----
			{
				Assert::IsFalse(TdProducts::kEngineRequestedToday,
					L"H1: TdProducts::kEngineRequestedToday must remain false while TD v2 is library-only");

				const std::string secPath = RepoRootFromFile() +
					"src\\VP-V\\modules\\control\\config\\config_pages\\VpvSection.cpp";
				const std::string secText = ReadWholeTextFile(secPath);

				const size_t tdRowPos = secText.find("if (k == \"temporal_detail\")");
				const size_t tdKappaPos = secText.find("if (k == \"td_limit_kappa\")");
				const size_t tdModePos = secText.find("if (k == \"td_mode\")");
				const size_t tdOverlayPos = secText.find("if (k == \"td_overlay\")");
				const size_t tdSolverPos = secText.find("if (k == \"td_solver\")");
				Assert::IsTrue(tdRowPos != std::string::npos && tdKappaPos != std::string::npos &&
				               tdModePos != std::string::npos && tdOverlayPos != std::string::npos &&
				               tdSolverPos != std::string::npos,
					L"VpvSection.cpp must declare help entries for temporal_detail, td_limit_kappa, td_mode, td_overlay, and td_solver");
				Assert::IsTrue(tdRowPos < tdKappaPos && tdKappaPos < tdModePos && tdModePos < tdOverlayPos && tdOverlayPos < tdSolverPos);

				const std::string tdRowBlock = secText.substr(tdRowPos, tdKappaPos - tdRowPos);
				const std::string tdModeBlock = secText.substr(tdModePos, tdOverlayPos - tdModePos);
				const std::string tdOverlayBlock = secText.substr(tdOverlayPos, tdSolverPos - tdOverlayPos);

				auto isHonestDropInStatus = [](const std::string& text) -> bool
				{
					const bool hasLibReady = ContainsIcase(text, "library ready");
					const bool hasNotWired = ContainsIcase(text, "not wired to live renderer yet");
					const bool hasFalseLiveClaim = ContainsIcase(text, "on: enables temporal detail repair (automatically enables");
					return hasLibReady && hasNotWired && !hasFalseLiveClaim;
				};

				Assert::IsTrue(isHonestDropInStatus(tdRowBlock),
					L"H1: VpvSection.cpp temporal_detail row must honestly state \"library ready\" and \"not wired to live renderer yet\" without false live-wiring claim");
				Assert::IsTrue(ContainsIcase(tdModeBlock, "inactive: transform unknown") && !ContainsIcase(tdModeBlock, "automatically enables"),
					L"H1/D3: VpvSection.cpp td_mode help must state honest transform-unknown status without false live-wiring claim");
				Assert::IsTrue(ContainsIcase(tdOverlayBlock, "planned for Wave 4-6") && !ContainsIcase(tdOverlayBlock, "automatically enables"),
					L"H1: VpvSection.cpp td_overlay help must state honest planned Wave 4-6 hookup status");

				// Negative control (H1): real production FormatTdCtrlIStatus(wiredToLiveRenderer=true) drops the
				// unwired caveat and fails the unwired-drop-in predicate, whereas default wiredToLiveRenderer=false passes!
				TdCtrlIStatusInput unwiredCheck;
				unwiredCheck.settings.mode = Mode::On;
				Assert::IsTrue(isHonestDropInStatus(FormatTdCtrlIStatus(unwiredCheck)),
					L"H1: default FormatTdCtrlIStatus(wiredToLiveRenderer=false) must satisfy honest drop-in check");
				TdCtrlIStatusInput wiredCheck = unwiredCheck;
				wiredCheck.wiredToLiveRenderer = true;
				Assert::IsFalse(isHonestDropInStatus(FormatTdCtrlIStatus(wiredCheck)),
					L"Negative control (H1): live FormatTdCtrlIStatus(wiredToLiveRenderer=true) must NOT match unwired drop-in status");
			}

			// ---- Part D: VpvRenderingDefaults.h defaults & direct production IsVpvOnlyKey sorted-table invariant ----
			{
				const std::string hdrPath = RepoRootFromFile() +
					"src\\VP-V\\modules\\control\\config\\config_pages\\VpvRenderingDefaults.h";
				const std::string text = ReadWholeTextFile(hdrPath);

				// Check exact built-in defaults in VpvRenderingDefault.
				Assert::IsTrue(text.find("if (key == QStringLiteral(\"temporal_detail\")) { out = QStringLiteral(\"off\"); return true; }") != std::string::npos,
					L"VpvRenderingDefaults.h must default temporal_detail to \"off\"");
				Assert::IsTrue(text.find("if (key == QStringLiteral(\"td_mode\")) { out = QStringLiteral(\"cell\"); return true; }") != std::string::npos,
					L"VpvRenderingDefaults.h must default td_mode to \"cell\"");
				Assert::IsTrue(text.find("if (key == QStringLiteral(\"td_overlay\")) { out = QStringLiteral(\"off\"); return true; }") != std::string::npos,
					L"VpvRenderingDefaults.h must default td_overlay to \"off\"");

				// Inspect the production IsVpvOnlyKey function directly: verify it uses std::binary_search
				// and that its production keys[] table is strictly sorted (no inversions, no duplicates).
				const size_t fnPos = text.find("inline bool IsVpvOnlyKey(");
				Assert::IsTrue(fnPos != std::string::npos, L"IsVpvOnlyKey not found in VpvRenderingDefaults.h");
				const size_t arrStart = text.find("keys[] = {", fnPos);
				const size_t arrEnd = text.find("};", arrStart);
				Assert::IsTrue(arrStart != std::string::npos && arrEnd != std::string::npos && arrEnd > arrStart);
				const size_t fnEnd = text.find("return std::binary_search(", arrEnd);
				Assert::IsTrue(fnEnd != std::string::npos && fnEnd < arrEnd + 400u,
					L"Production IsVpvOnlyKey must use std::binary_search on keys[]");
				const std::string arrBody = text.substr(arrStart, arrEnd - arrStart);

				std::vector<std::string> vpvKeys;
				size_t p = 0;
				while ((p = arrBody.find('"', p)) != std::string::npos)
				{
					const size_t q = arrBody.find('"', p + 1);
					Assert::IsTrue(q != std::string::npos);
					vpvKeys.push_back(arrBody.substr(p + 1, q - p - 1));
					p = q + 1;
				}
				Assert::IsTrue(vpvKeys.size() > 100u, L"IsVpvOnlyKey array extraction unexpectedly short");
				for (size_t i = 1; i < vpvKeys.size(); ++i)
				{
					Assert::IsTrue(std::strcmp(vpvKeys[i - 1].c_str(), vpvKeys[i].c_str()) < 0,
						L"Production IsVpvOnlyKey keys[] MUST be strictly ascending with zero duplicates for std::binary_search");
				}
				auto hasKeyInSortedTable = [&](const char* key) -> bool
				{
					return std::binary_search(vpvKeys.begin(), vpvKeys.end(), std::string(key),
						[](const std::string& a, const std::string& b) { return std::strcmp(a.c_str(), b.c_str()) < 0; });
				};
				for (const char* tdKey : { "td_mode", "td_overlay", "temporal_detail" })
				{
					Assert::IsTrue(hasKeyInSortedTable(tdKey),
						L"Production IsVpvOnlyKey keys[] must contain temporal_detail, td_mode, td_overlay");
				}
				for (const char* excludedKey : { "td_nonexistent", "temporal_detail_invalid", "td_preset" })
				{
					Assert::IsFalse(hasKeyInSortedTable(excludedKey),
						L"Production IsVpvOnlyKey keys[] must exclude unknown or harness-only keys");
				}
			}

			// ---- Part D2: Real Runtime Round-Trip of VpvRendererSettings (PostLoad/ApplyOriginal, Fingerprint, CopyLive, AppendDelta, Describe) ----
			{
				const VpvRenderer::Fields defFields;
				VpvRenderer::Fields modFields;
				modFields.temporalDetail = "on";
				modFields.tdMode = "strict";
				modFields.tdOverlay = "maps";

				// 1. Fingerprint: every TD key changes both includeLive=true and includeLive=false fingerprints,
				//    and TD keys sit at the tail (appended at the end so existing field prefix stays untouched).
				const std::string fpDefLive = VpvRendererSettings::Fingerprint(defFields, true);
				const std::string fpDefNoLive = VpvRendererSettings::Fingerprint(defFields, false);
				const std::string fpModLive = VpvRendererSettings::Fingerprint(modFields, true);
				const std::string fpModNoLive = VpvRendererSettings::Fingerprint(modFields, false);
				Assert::IsTrue(fpModLive != fpDefLive, L"Fingerprint(includeLive=true) must reflect TD key changes");
				Assert::IsTrue(fpModNoLive != fpDefNoLive, L"Fingerprint(includeLive=false) must reflect TD key changes");

				VpvRenderer::Fields onlyMode = defFields; onlyMode.tdMode = "strict";
				VpvRenderer::Fields onlyOverlay = defFields; onlyOverlay.tdOverlay = "focus";
				VpvRenderer::Fields onlyTd = defFields; onlyTd.temporalDetail = "shadow";
				const std::string fpOnlyTd = VpvRendererSettings::Fingerprint(onlyTd, false);
				const std::string fpOnlyMode = VpvRendererSettings::Fingerprint(onlyMode, false);
				const std::string fpOnlyOverlay = VpvRendererSettings::Fingerprint(onlyOverlay, false);
				Assert::IsTrue(fpOnlyTd != fpDefNoLive && fpOnlyMode != fpDefNoLive && fpOnlyOverlay != fpDefNoLive,
					L"Each TD field independently changes Fingerprint");
				Assert::IsTrue(fpOnlyTd != fpOnlyMode && fpOnlyMode != fpOnlyOverlay && fpOnlyTd != fpOnlyOverlay,
					L"Distinct TD field edits produce distinct Fingerprints");

				const std::string tailToken = "|on|strict|maps|1|active_set|aligned_a2e|band_shrink|off|on|on|independent|on|off|off|off";
				const size_t tailPosNoLive = fpModNoLive.rfind(tailToken);
				Assert::IsTrue(tailPosNoLive != std::string::npos && tailPosNoLive + tailToken.size() == fpModNoLive.size(),
					L"TD keys must appear at the end of VpvRendererSettings::Fingerprint(false)");
				const size_t tailPosLive = fpModLive.rfind(tailToken);
				Assert::IsTrue(tailPosLive != std::string::npos,
					L"TD keys must appear in VpvRendererSettings::Fingerprint(true)");

				// 2. CopyLive: copies temporalDetail, tdMode, tdOverlay cleanly to transport struct.
				VpvRenderer::Fields transport;
				VpvRendererSettings::CopyLive(transport, modFields);
				Assert::AreEqual(std::string("on"), transport.temporalDetail, L"CopyLive must copy temporalDetail");
				Assert::AreEqual(std::string("strict"), transport.tdMode, L"CopyLive must copy tdMode");
				Assert::AreEqual(std::string("maps"), transport.tdOverlay, L"CopyLive must copy tdOverlay");

				// 3. AppendDelta: reports exact changed TD keys and stays empty when unchanged.
				std::vector<const char*> sameDelta;
				VpvRendererSettings::AppendDelta(modFields, transport, sameDelta);
				Assert::AreEqual(static_cast<size_t>(0u), sameDelta.size(), L"AppendDelta must be empty for identical fields");

				std::vector<const char*> diffDelta;
				VpvRendererSettings::AppendDelta(defFields, modFields, diffDelta);
				auto hasDeltaKey = [&](const char* k) -> bool
				{
					for (const char* f : diffDelta)
						if (f != nullptr && std::strcmp(f, k) == 0) return true;
					return false;
				};
				Assert::IsTrue(hasDeltaKey("temporal_detail"), L"AppendDelta must report temporal_detail");
				Assert::IsTrue(hasDeltaKey("td_mode"), L"AppendDelta must report td_mode");
				Assert::IsTrue(hasDeltaKey("td_overlay"), L"AppendDelta must report td_overlay");

				// 4. Describe: emits temporal_detail, td_mode, td_overlay at the end of settings dump.
				const Dejudder::Settings defaultMatcher{};
				const std::string desc = VpvRendererSettings::Describe(modFields, "", defaultMatcher, "");
				const size_t posTaperSad = desc.find("matcher.taperSad=");
				const size_t posTdLine = desc.find("temporal_detail=on\n");
				const size_t posModeLine = desc.find("td_mode=strict\n");
				const size_t posOverlayLine = desc.find("td_overlay=maps\n");
				Assert::IsTrue(posTaperSad != std::string::npos, L"Describe missing baseline matcher.taperSad=");
				Assert::IsTrue(posTdLine != std::string::npos && posTdLine > posTaperSad,
					L"Describe must emit temporal_detail=on at the end");
				Assert::IsTrue(posModeLine != std::string::npos && posModeLine > posTaperSad,
					L"Describe must emit td_mode=strict at the end");
				Assert::IsTrue(posOverlayLine != std::string::npos && posOverlayLine > posTaperSad,
					L"Describe must emit td_overlay=maps at the end");

				// 5. PostLoad (ApplyOriginal): Original pathway resets TD keys cleanly to Bill defaults ("off"/"cell"/"off"),
				//    while non-Original pathway preserves the user's active TD settings (negative control).
				VpvRenderer::Fields nonOrig = modFields;
				nonOrig.pipelineOriginalPathway = false;
				VpvRendererSettings::PostLoad(nonOrig, false);
				Assert::AreEqual(std::string("on"), nonOrig.temporalDetail, L"Negative control: non-Original PostLoad preserves temporal_detail");
				Assert::AreEqual(std::string("strict"), nonOrig.tdMode, L"Negative control: non-Original PostLoad preserves td_mode");
				Assert::AreEqual(std::string("maps"), nonOrig.tdOverlay, L"Negative control: non-Original PostLoad preserves td_overlay");

				VpvRenderer::Fields origReset = modFields;
				origReset.pipelineOriginalPathway = true;
				VpvRendererSettings::PostLoad(origReset, false);
				Assert::AreEqual(std::string("off"), origReset.temporalDetail, L"Original PostLoad/ApplyOriginal must reset temporalDetail to off");
				Assert::AreEqual(std::string("cell"), origReset.tdMode, L"Original PostLoad/ApplyOriginal must reset tdMode to cell");
				Assert::AreEqual(std::string("off"), origReset.tdOverlay, L"Original PostLoad/ApplyOriginal must reset tdOverlay to off");
			}

			// ---- Part E: ParseTdV2Settings strict validation (no silent mutation on invalid input) ----
			{
				TdV2RuntimeSettings s;
				Assert::AreEqual(static_cast<int>(Mode::Off), static_cast<int>(s.mode));
				Assert::AreEqual(static_cast<int>(ResidualMode::Cell), static_cast<int>(s.residualMode));
				Assert::AreEqual(static_cast<int>(Preset::RealTime), static_cast<int>(s.preset));
				Assert::AreEqual(static_cast<int>(OverlayMode::Off), static_cast<int>(s.overlay));

				std::string problem;
				Assert::IsTrue(ParseTdV2Settings(" TEMPORAL_DETAIL ", " On ", s, problem) && problem.empty());
				Assert::AreEqual(static_cast<int>(Mode::On), static_cast<int>(s.mode));
				Assert::IsTrue(ParseTdV2Settings("td_mode", "Strict", s, problem) && problem.empty());
				Assert::AreEqual(static_cast<int>(ResidualMode::Strict), static_cast<int>(s.residualMode));
				Assert::IsTrue(ParseTdV2Settings("td_preset", "Reference", s, problem) && problem.empty());
				Assert::AreEqual(static_cast<int>(Preset::Reference), static_cast<int>(s.preset));
				Assert::IsTrue(ParseTdV2Settings("td_overlay", "Focus", s, problem) && problem.empty());
				Assert::AreEqual(static_cast<int>(OverlayMode::Focus), static_cast<int>(s.overlay));

				// Invalid values MUST return false, populate `problem`, and leave `s` completely untouched!
				Assert::IsFalse(ParseTdV2Settings("temporal_detail", "auto", s, problem));
				Assert::IsFalse(problem.empty(), L"Invalid temporal_detail must explain rejection");
				Assert::AreEqual(static_cast<int>(Mode::On), static_cast<int>(s.mode), L"Must not silently mutate mode");

				Assert::IsFalse(ParseTdV2Settings("td_mode", "l3", s, problem));
				Assert::IsFalse(problem.empty());
				Assert::AreEqual(static_cast<int>(ResidualMode::Strict), static_cast<int>(s.residualMode));

				Assert::IsFalse(ParseTdV2Settings("td_preset", "ultra", s, problem));
				Assert::IsFalse(problem.empty());
				Assert::AreEqual(static_cast<int>(Preset::Reference), static_cast<int>(s.preset));

				Assert::IsFalse(ParseTdV2Settings("td_overlay", "rainbow", s, problem));
				Assert::IsFalse(problem.empty());
				Assert::AreEqual(static_cast<int>(OverlayMode::Focus), static_cast<int>(s.overlay));

				Assert::IsFalse(ParseTdV2Settings("unknown_td_key", "on", s, problem));
				Assert::IsFalse(problem.empty());
			}

			// ---- Part F: BuildTdV2Config Original override & Q8a ResolveTdDependencies ----
			{
				TdV2RuntimeSettings onSettings;
				onSettings.mode = Mode::On;
				onSettings.residualMode = ResidualMode::Strict;
				onSettings.preset = Preset::Reference;

				const Config cfgOn = BuildTdV2Config(onSettings, /*originalPathway=*/false);
				Assert::AreEqual(static_cast<int>(Mode::On), static_cast<int>(cfgOn.mode));
				Assert::AreEqual(static_cast<int>(ResidualMode::Strict), static_cast<int>(cfgOn.residualMode));
				Assert::IsTrue(cfgOn.IsEnabled() && cfgOn.ModifiesPixels());

				const Config cfgOrig = BuildTdV2Config(onSettings, /*originalPathway=*/true);
				Assert::AreEqual(static_cast<int>(Mode::Off), static_cast<int>(cfgOrig.mode),
					L"Original pathway MUST force Mode::Off even when temporal_detail = on");
				Assert::IsFalse(cfgOrig.IsEnabled() || cfgOrig.ModifiesPixels());

				// Q8a dependency-plan helper: ON and Shadow plan analysis lane + motion/camera group while honestly stating not wired yet.
				const TdDependencyActivation depOn = ResolveTdDependencies(onSettings, false);
				Assert::IsTrue(depOn.enableAnalysisLane && depOn.enableMotionCameraGroup,
					L"Q8a: temporal_detail=on must plan analysis lane + motion/camera group");
				Assert::IsTrue(ContainsIcase(depOn.visibleNote, "library ready") &&
					ContainsIcase(depOn.visibleNote, "not wired to live renderer yet"),
					L"H1: ResolveTdDependencies(on) must honestly state library ready / not wired to live renderer yet");

				TdV2RuntimeSettings shadowSettings = onSettings;
				shadowSettings.mode = Mode::Shadow;
				const TdDependencyActivation depSh = ResolveTdDependencies(shadowSettings, false);
				Assert::IsTrue(depSh.enableAnalysisLane && depSh.enableMotionCameraGroup,
					L"Q8a: temporal_detail=shadow must plan analysis lane + motion/camera group");
				Assert::IsTrue(ContainsIcase(depSh.visibleNote, "library ready") &&
					ContainsIcase(depSh.visibleNote, "not wired to live renderer yet"),
					L"H1: ResolveTdDependencies(shadow) must honestly state library ready / not wired to live renderer yet");

				const TdDependencyActivation depOrig = ResolveTdDependencies(onSettings, true);
				Assert::IsFalse(depOrig.enableAnalysisLane || depOrig.enableMotionCameraGroup,
					L"Original pathway must leave analysis lane & motion/camera group disabled");
				Assert::IsTrue(depOrig.visibleNote.find("Original") != std::string::npos);

				TdV2RuntimeSettings offSettings;
				const TdDependencyActivation depOff = ResolveTdDependencies(offSettings, false);
				Assert::IsFalse(depOff.enableAnalysisLane || depOff.enableMotionCameraGroup,
					L"temporal_detail=off must leave analysis lane & motion/camera group disabled");
				Assert::IsTrue(ContainsIcase(depOff.visibleNote, "library ready") &&
					ContainsIcase(depOff.visibleNote, "not wired to live renderer yet"),
					L"H1: ResolveTdDependencies(off) must honestly state library ready / not wired to live renderer yet");
			}

			// ---- Part G: Mode::Shadow vs Mode::On vs Original (Bit-Exact Pixel Verification + Negative Control!) ----
			{
				const int w = 64, h = 64;
				const ActiveRect active = ActiveRect::Full(w, h);
				// In-focus sharp-detail synthetic scene (F > 0.30 >> f_defocus = 0.1170) so Q4 BokehMode::Untouched
				// recognizes it as sharp in-focus content rather than optically defocused bokeh.
				TdV2Plane cleanMaster(w, h, 0.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						cleanMaster.at(x, y) = 480.0
							+ 40.0 * std::sin(0.8 * static_cast<double>(x)) * std::cos(0.7 * static_cast<double>(y))
							+ 25.0 * ((((x / 4) + (y / 4)) & 1) ? 1.0 : -1.0);
					}
				}

				TdV2Plane curDamaged = cleanMaster;
				TdV2Plane prevPeak = cleanMaster;
				TdV2Plane nextPeak = cleanMaster;
				std::mt19937 rngCur(5151u), rngPrev(5152u), rngNext(5153u);
				std::normal_distribution<double> ndCur(0.0, 9.0);
				std::normal_distribution<double> ndPeak(0.0, 1.0);
				for (size_t i = 0; i < curDamaged.v.size(); ++i)
				{
					curDamaged.v[i] += ndCur(rngCur);
					prevPeak.v[i] += ndPeak(rngPrev);
					nextPeak.v[i] += ndPeak(rngNext);
				}

				const CellMotionField stillMf = CellMotionField::Uniform(4, 4, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				FrameProcessInput fin;
				fin.seq = 1;
				fin.receivedY = curDamaged;
				fin.hasCleanSplit = true;
				fin.cleanY = curDamaged;
				fin.activeRect = active;
				fin.hasStepPrior = true;
				fin.estimatedStepDelta = 24.0;
				fin.zeroCoeffFraction = 0.4;
				fin.prevPeak.cleanPlane = &prevPeak;
				fin.prevPeak.motion = &stillMf;
				fin.nextPeak.cleanPlane = &nextPeak;
				fin.nextPeak.motion = &stillMf;

				TdV2RuntimeSettings rs;
				rs.mode = Mode::On;
				rs.preset = Preset::Reference;
				rs.residualMode = ResidualMode::Cell;

				// Negative control: Mode::On (non-Original) genuinely alters pixels (> 2.0 codes)!
				TdV2ReferenceEngine engOn(BuildTdV2Config(rs, false));
				const FrameProcessResult resOn = engOn.ProcessFrame(fin);
				const double onDiff = MaxAbsDiffPlanes(resOn.outputY, curDamaged);
				Assert::IsTrue(onDiff > 2.0,
					FormatMsg("Negative control: Mode::On must genuinely modify damaged frame (diff=%.4f <= 2.0)", onDiff).c_str());

				// Original pathway forces Mode::Off -> bit-exact receivedY, zero active tdMaps!
				TdV2ReferenceEngine engOrig(BuildTdV2Config(rs, true));
				const FrameProcessResult resOrig = engOrig.ProcessFrame(fin);
				Assert::AreEqual(0.0, MaxAbsDiffPlanes(resOrig.outputY, curDamaged),
					L"Original pathway outputY must be bit-exact receivedY");
				Assert::IsTrue(resOrig.outputClean.Empty(),
					L"Original pathway (Mode::Off) must skip clean reconstruction");
				for (const auto& blk : resOrig.tdMaps)
					Assert::AreEqual(0.0f, blk.f[TdMapsView::kValidity], L"Original pathway tdMaps must be inactive");

				// Mode::Shadow -> bit-exact receivedY while STILL computing real non-zero clean/delta & valid tdMaps!
				rs.mode = Mode::Shadow;
				TdV2ReferenceEngine engSh(BuildTdV2Config(rs, false));
				const FrameProcessResult resSh = engSh.ProcessFrame(fin);
				Assert::AreEqual(0.0, MaxAbsDiffPlanes(resSh.outputY, curDamaged),
					L"Mode::Shadow outputY MUST be bit-exact receivedY");
				Assert::IsTrue(MaxAbsDiffPlanes(resSh.outputClean, curDamaged) > 2.0,
					L"Mode::Shadow MUST still compute real non-zero clean delta");
				Assert::AreEqual(0.0, MaxAbsDiffPlanes(resSh.outputClean, resOn.outputClean), 1e-12,
					L"Mode::Shadow computed outputClean/delta must match Mode::On");
				Assert::AreEqual(resOn.tdMaps.size(), resSh.tdMaps.size());
				bool anyValidMap = false;
				for (const auto& blk : resSh.tdMaps)
					if (blk.f[TdMapsView::kValidity] > 0.5f) anyValidMap = true;
				Assert::IsTrue(anyValidMap, L"Mode::Shadow MUST populate valid tdMaps");
			}

			// ---- Part H: BuildLiveTdProductSnapshot (Placeholder when OFF/Original, Live when Active) ----
			{
				LiveTdProductInputs pin;
				pin.settings.mode = Mode::Off;
				pin.mapsValid = true;
				pin.deltaValid = true;
				pin.trailValid = true;
				pin.codingEvidenceValid = true;
				pin.timeUs = 100000;
				pin.maxAgeUs = 50000;

				const LiveTdProductSnapshot offSnap = BuildLiveTdProductSnapshot(pin);
				Assert::IsFalse(offSnap.engineRequested, L"OFF mode snapshot must have engineRequested == false");
				Assert::IsTrue(offSnap.View() == TdProducts::PlaceholderSnapshot(),
					L"OFF mode snapshot View() must return TdProducts::PlaceholderSnapshot()");

				pin.settings.mode = Mode::On;
				pin.originalPathway = true;
				const LiveTdProductSnapshot origSnap = BuildLiveTdProductSnapshot(pin);
				Assert::IsFalse(origSnap.engineRequested, L"Original pathway snapshot must have engineRequested == false");
				Assert::IsTrue(origSnap.View() == TdProducts::PlaceholderSnapshot(),
					L"Original pathway snapshot View() must return PlaceholderSnapshot()");

				pin.originalPathway = false;
				pin.blocksValid = false;
				pin.anchorPassValid = true;
				pin.qualityPatternCausalValid = true;
				const LiveTdProductSnapshot liveSnap = BuildLiveTdProductSnapshot(pin);
				Assert::IsTrue(liveSnap.engineRequested, L"Active TD snapshot must set engineRequested == true");
				Assert::IsTrue(liveSnap.View() == &liveSnap.snapshot, L"Active TD snapshot View() must point to live snapshot");
				Assert::AreEqual(TdProducts::kUnitVersion, liveSnap.unit);
				Assert::IsTrue(ContainsIcase(liveSnap.statusText, "library ready") &&
					ContainsIcase(liveSnap.statusText, "not wired to live renderer yet"),
					L"H1: BuildLiveTdProductSnapshot default status must honestly report library ready / not wired to live renderer yet");
				// kTdBlocks must ONLY publish when blocksValid == true (never inferred from mapsValid/deltaValid)!
				Assert::IsFalse(liveSnap.snapshot.Of(VpSkeleton::kTdBlocks).published,
					L"kTdBlocks must NOT publish when blocksValid == false");
				Assert::IsTrue(liveSnap.snapshot.Of(VpSkeleton::kTdAnchorPass).published,
					L"anchorPassValid == true must publish kTdAnchorPass");
				Assert::AreEqual(TdProducts::kUnitVersion, liveSnap.snapshot.Of(VpSkeleton::kTdAnchorPass).unitVersion);
				Assert::IsTrue(liveSnap.snapshot.Of(VpSkeleton::kQualityPatternCausal).published,
					L"qualityPatternCausalValid == true must publish kQualityPatternCausal");
				Assert::AreEqual(TdProducts::kQualityPatternUnit, liveSnap.snapshot.Of(VpSkeleton::kQualityPatternCausal).unitVersion);

				LiveTdProductInputs withBlocksPin = pin;
				withBlocksPin.blocksValid = true;
				const LiveTdProductSnapshot withBlocksSnap = BuildLiveTdProductSnapshot(withBlocksPin);
				Assert::IsTrue(withBlocksSnap.snapshot.Of(VpSkeleton::kTdBlocks).published,
					L"blocksValid == true must publish kTdBlocks");

				const TdProducts::ReadResult rr = TdProducts::Read(
					TdProducts::Consumer::Deband, liveSnap.View(), liveSnap.engineRequested, /*nowUs=*/120000);
				Assert::IsTrue(rr.usable, L"Live published kTdMaps + kCodingEvidence must be usable by Deband consumer");
				Assert::IsFalse(rr.fallback.engaged, L"Live published kTdMaps + kCodingEvidence must not engage fallback");

				// Negative control: omitting required kCodingEvidence visibly engages Deband TD fallback!
				LiveTdProductInputs partialPin = pin;
				partialPin.codingEvidenceValid = false;
				const LiveTdProductSnapshot partialSnap = BuildLiveTdProductSnapshot(partialPin);
				const TdProducts::ReadResult rrPartial = TdProducts::Read(
					TdProducts::Consumer::Deband, partialSnap.View(), partialSnap.engineRequested, /*nowUs=*/120000);
				Assert::IsFalse(rrPartial.usable, L"Negative control: missing kCodingEvidence must mark Deband read unusable");
				Assert::IsTrue(rrPartial.fallback.engaged, L"Negative control: missing kCodingEvidence must visibly engage fallback");

				// Late-carry status strings propagate cleanly into LiveTdProductSnapshot, and lateExpired outranks lateCarried!
				LiveTdProductInputs latePin = pin;
				latePin.lateCarried = true;
				latePin.lateExpired = false;
				const LiveTdProductSnapshot lateSnap = BuildLiveTdProductSnapshot(latePin);
				Assert::AreEqual(std::string(MetaOf(StandInKind::LateCarryActive).statusText), lateSnap.statusText);

				latePin.lateCarried = true;
				latePin.lateExpired = true;
				const LiveTdProductSnapshot bothSnap = BuildLiveTdProductSnapshot(latePin);
				Assert::AreEqual(std::string(MetaOf(StandInKind::LateCarryExpired).statusText), bothSnap.statusText,
					L"When both lateExpired and lateCarried are set, lateExpired must take priority");

				latePin.lateCarried = false;
				latePin.lateExpired = true;
				const LiveTdProductSnapshot expSnap = BuildLiveTdProductSnapshot(latePin);
				Assert::AreEqual(std::string(MetaOf(StandInKind::LateCarryExpired).statusText), expSnap.statusText);
			}

			// ---- Part I: FormatTdCtrlIStatus (Honest H1 Default Drop-In Status, Mode, ResidualMode, Tier, Lead E=n/4, Late Carry, Every Stand-In) ----
			{
				TdCtrlIStatusInput cin;
				Assert::IsFalse(cin.wiredToLiveRenderer,
					L"H1: TdCtrlIStatusInput::wiredToLiveRenderer must default to false (honest drop-in status)");
				cin.settings.mode = Mode::Off;
				const std::string offHonest = FormatTdCtrlIStatus(cin);
				Assert::IsTrue(ContainsIcase(offHonest, "off") &&
					ContainsIcase(offHonest, "library ready") &&
					ContainsIcase(offHonest, "not wired to live renderer yet"),
					L"H1: Default OFF Ctrl+I status must state library ready and not wired to live renderer yet");

				cin.settings.mode = Mode::On;
				cin.originalPathway = true;
				const std::string origLine = FormatTdCtrlIStatus(cin);
				Assert::IsTrue(ContainsIcase(origLine, "off (Original pathway)"),
					L"Original pathway Ctrl+I status must state off (Original pathway)");

				cin.originalPathway = false;
				cin.settings.residualMode = ResidualMode::Strict;
				cin.settings.overlay = OverlayMode::Maps;
				cin.tier = TdTier::ReducedAnchor;
				cin.usableLeadE = 2;
				cin.lateCarried = true;
				cin.lateStreak = 1;

				StandInRegistry si;
				si.Engage(StandInKind::P10gGrainSeparator);
				si.Engage(StandInKind::LeadReduced, "E=2/4");
				si.Engage(StandInKind::LateCarryActive);
				cin.standIns = &si;

				const std::string line = FormatTdCtrlIStatus(cin);
				Assert::IsTrue(ContainsIcase(line, "library ready") &&
					ContainsIcase(line, "not wired to live renderer yet"),
					L"H1: Default active Ctrl+I status MUST honestly state library ready and not wired to live renderer yet");
				Assert::IsTrue(line.find("Temporal detail: on") == std::string::npos,
					L"H1: Unwired Ctrl+I status must NOT falsely claim \"Temporal detail: on\" is running live");
				Assert::IsTrue(line.find("requested=on") != std::string::npos, L"Ctrl+I missing requested=on");
				Assert::IsTrue(line.find("mode=strict") != std::string::npos, L"Ctrl+I missing residual mode");
				Assert::IsTrue(line.find("overlay=maps") != std::string::npos, L"Ctrl+I missing overlay mode");
				Assert::IsTrue(line.find(TdTierName(TdTier::ReducedAnchor)) != std::string::npos, L"Ctrl+I missing tier");
				Assert::IsTrue(line.find("lead E=2/4") != std::string::npos, L"Ctrl+I missing lead E=2/4");
				Assert::IsTrue(line.find(MetaOf(StandInKind::LateCarryActive).statusText) != std::string::npos,
					L"Ctrl+I missing late-carry active status");
				Assert::IsTrue(line.find("(1/2)") != std::string::npos, L"Ctrl+I missing late-carry streak (1/2)");
				Assert::IsTrue(line.find(MetaOf(StandInKind::P10gGrainSeparator).statusText) != std::string::npos,
					L"Ctrl+I missing engaged P10g stand-in");
				Assert::IsTrue(line.find(MetaOf(StandInKind::LeadReduced).statusText) != std::string::npos,
					L"Ctrl+I missing engaged LeadReduced stand-in");

				// Future Wave 4-6 wired mode (wiredToLiveRenderer == true) switches cleanly to live wording.
				TdCtrlIStatusInput wiredCin = cin;
				wiredCin.wiredToLiveRenderer = true;
				const std::string wiredLine = FormatTdCtrlIStatus(wiredCin);
				Assert::IsTrue(wiredLine.find("Temporal detail: on") != std::string::npos,
					L"Negative control (H1): wiredToLiveRenderer=true must emit live \"Temporal detail: on\"");
				Assert::IsFalse(ContainsIcase(wiredLine, "not wired to live renderer yet"),
					L"Negative control (H1): wiredToLiveRenderer=true must drop not-wired caveat");

				// Late-expired (including when both lateCarried & lateExpired set) & on-time & ceiling clamp E=99 -> E=6/4.
				cin.lateCarried = true;
				cin.lateExpired = true;
				cin.usableLeadE = 99;
				const std::string expLine = FormatTdCtrlIStatus(cin);
				Assert::IsTrue(expLine.find("lead E=6/4") != std::string::npos, L"Ctrl+I must clamp lead E to ceiling 6");
				Assert::IsTrue(expLine.find(MetaOf(StandInKind::LateCarryExpired).statusText) != std::string::npos,
					L"Ctrl+I must prioritize late-carry expired status over lateCarried");

				cin.lateCarried = false;
				cin.lateExpired = false;
				cin.standIns = nullptr;
				const std::string onTimeLine = FormatTdCtrlIStatus(cin);
				Assert::IsTrue(onTimeLine.find("timing=on-time") != std::string::npos, L"Ctrl+I missing timing=on-time");
			}
		}

		// 6. (M-5 split onto gemini/td-dither-split per Owner Ruling #2 & Fable audit M-5)

		// =========================================================================
		// 7. Item #5 Red-First: LateCarry prevCleanValid_ Persistence Across Hop 2
		//    & Guarding Missing refGuide on 8x8 DCT STRICT/CELL POCS
		// =========================================================================
		TEST_METHOD(LateCarryPersistsPrevCleanValidAcrossHop2AndGuardsMissingRefGuideOn8x8Pocs)
		{
			// Derived uniform quantization prior: stepDelta = 12.0, deadZonePhi = 1/6 -> theta = 10.0,
			// uniform quantization variance = Delta^2 / 12 = 12.0 -> 3-sigma pixel bound = 3 * sqrt(12) ~= 10.3923 codes.
			const double stepDelta = 12.0;
			const double phi = kDeadZonePhiInter;
			const double theta = (1.0 - phi) * stepDelta; // 10.0
			const double stepTotalVar = (stepDelta * stepDelta) / 12.0; // 12.0
			const int cellPx = 16;

			// -------------------------------------------------------------------------
			// Sub-case A: Hop-2 prevCleanValid_ persistence + invalid-origin protection
			// -------------------------------------------------------------------------
			{
				const int w = 64;
				const int h = 64;
				const int cellsW = w / cellPx; // 4
				const int cellsH = h / cellPx; // 4
				const ActiveRect active = ActiveRect::Full(w, h);

				// Sharp high-contrast horizontal step (+80 codes at y=40) + linear horizontal ramp (+1.0 code/px).
				// Linear in x and piecewise constant in y => exact zero bicubic ringing under pure horizontal pan (vx=+1.5, vy=0).
				auto scene = [](double x, double y) -> double
				{
					return 420.0 + (y >= 40.0 ? 80.0 : 0.0) + 1.0 * x;
				};

				TdV2Plane onTimeClean(w, h, 0.0);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						onTimeClean.at(x, y) = scene(static_cast<double>(x), static_cast<double>(y));

				const TdV2Plane onTimeDelta(w, h, +9.0);

				TdV2LateCarryController ctrl;
				StandInRegistry si;
				ctrl.OnOnTimeFrame(onTimeDelta, &si, &onTimeClean);

				// Hop 1: cell (1, 1) ([16, 32)^2) has valid == 0 and bogus large vector while curClean1 has a -80 code spike there.
				// Outside [16, 32)^2: curClean1 = scene - 6.0 (agrees with +9.0 * 2/3 = +6.0).
				CellMotionField mf1 = CellMotionField::Uniform(cellsW, cellsH, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Camera);
				const size_t invalidCell = 1u * static_cast<size_t>(cellsW) + 1u; // cell (1, 1) -> [16, 32)^2
				mf1.valid[invalidCell] = 0u;
				mf1.vx[invalidCell] = 25.0;
				mf1.vy[invalidCell] = -19.0;

				TdV2Plane curClean1(w, h, 0.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const bool inInvalid = (x >= 16 && x < 32 && y >= 16 && y < 32);
						curClean1.at(x, y) = scene(static_cast<double>(x), static_cast<double>(y)) - (inInvalid ? 80.0 : 6.0);
					}
				}

				const LateCarryDecision dec1 = ctrl.OnLateFrame(
					curClean1, active, &mf1, stepTotalVar, cellsW, cellsH, cellPx, &si, nullptr);
				Assert::IsTrue(dec1.carried && !dec1.expired && !dec1.usesL3Clamp,
					L"Sub-case A Hop 1 must carry cleanly");
				Assert::AreEqual(1, dec1.lateStreak);
				Assert::AreEqual(static_cast<size_t>(0u), dec1.residualRejectedPixels,
					L"Sub-case A Hop 1 must have 0 residual-rejected pixels");
				Assert::AreEqual(0.0, MaxAbsPlaneRect(dec1.delta, ActiveRect{ 16, 16, 32, 32 }),
					L"Sub-case A Hop 1 invalid cell [16,32)^2 must have exact 0.0 delta");

				// Hop 2: valid == 1 everywhere with pan (+1.5, 0.0) crossing cell (1, 1) boundary into adjacent pixels
				// (x = 14, 15 in 8x8 block (8, 16) and x = 30, 31 in 8x8 block (24, 16)), under 8x8 DCT STRICT with no codecModelPb/referenceClean.
				const CellMotionField mf2 = CellMotionField::Uniform(cellsW, cellsH, 1.5, 0.0, 1.5, 0.0, RegionMotionClass::Camera);
				TdV2Plane curClean2(w, h, 0.0);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						curClean2.at(x, y) = scene(static_cast<double>(x) + 1.5, static_cast<double>(y)) - 6.0;

				LateCarryContext ctx2;
				ctx2.transformIdentified8x8 = true;
				ctx2.residualMode = ResidualMode::Strict;
				ctx2.stepDelta = stepDelta;
				ctx2.deadZonePhi = phi;
				ctx2.sourceBits = 10;
				ctx2.cellProjIterations = 2;
				ctx2.pixelBoundKappa = 3.0;
				ctx2.codecModelPb = nullptr;
				ctx2.referenceClean = nullptr;

				StandInRegistry si2;
				const LateCarryDecision dec2 = ctrl.OnLateFrame(
					curClean2, active, &mf2, stepTotalVar, cellsW, cellsH, cellPx, &si2, &ctx2);

				Assert::IsTrue(dec2.carried && !dec2.expired && !dec2.usesL3Clamp,
					L"Sub-case A Hop 2 must carry cleanly without L3 clamp");
				Assert::AreEqual(2, dec2.lateStreak);
				Assert::IsFalse(si2.IsActive(StandInKind::NonFiniteClosure),
					L"Sub-case A Hop 2 must not trip NonFiniteClosure");

				// 1. ZERO false residual rejections against Hop-1's poisoned -80 fallback in [16, 32)^2!
				Assert::AreEqual(static_cast<size_t>(0u), dec2.residualRejectedPixels,
					FormatMsg("Sub-case A Hop 2 falsely rejected %.0f pixels against Hop-1 invalid -80 fallback",
						static_cast<double>(dec2.residualRejectedPixels)).c_str());

				// 2. Pixels & 8x8 blocks touching Hop-1's invalid [16, 32)^2 origin fall back cleanly to ApplyTransformUnknownBound:
				//    - At (14, 20) (sx=15.5) and (30, 20) (sx=31.5): Catmull-Rom([9,9,0,0], 0.5) * 1/3 = 4.5 / 3 = +1.5 (NOT rejected/crushed!).
				//    - At (10, 20) inside the same 8x8 block (8, 16): 9.0 * 1/3 = +3.0 (NOT crushed to 0 by poisoned -80 STRICT DCT!).
				Assert::AreEqual(1.5, dec2.delta.at(14, 20), 1e-6,
					FormatMsg("Sub-case A Hop 2 delta(14,20)=%.6f != 1.5 (poisoned by Hop-1 -80 fallback?)",
						dec2.delta.at(14, 20)).c_str());
				Assert::AreEqual(1.5, dec2.delta.at(30, 20), 1e-6,
					FormatMsg("Sub-case A Hop 2 delta(30,20)=%.6f != 1.5 (poisoned by Hop-1 -80 fallback?)",
						dec2.delta.at(30, 20)).c_str());
				Assert::AreEqual(3.0, dec2.delta.at(10, 20), 1e-6,
					FormatMsg("Sub-case A Hop 2 delta(10,20)=%.6f != 3.0 (8x8 block (8,16) projected against poisoned prevClean_?)",
						dec2.delta.at(10, 20)).c_str());
				Assert::IsTrue(si2.IsActive(StandInKind::TransformUnknownBound),
					L"Sub-case A Hop 2 must engage TransformUnknownBound when codecModelPb is absent (H-1)");
				Assert::IsTrue(si2.IsActive(StandInKind::CodecModelPbStandIn),
					L"Sub-case A Hop 2 with null codecModelPb must visibly engage CodecModelPbStandIn (H-1)");
				Assert::IsFalse(dec2.usedTransformProjection,
					L"Sub-case A Hop 2 with null codecModelPb must NOT substitute warped prevClean_ as fake P_B (H-1)");

				// 3. Anti-cheat non-vacuity: when real encoder prediction codecModelPb IS supplied, valid interior 8x8 block (40, 40)
				//    runs 8x8 DCT STRICT projection (sentRes = -6.0 => |DC| = 48 >= theta=10 => locked to 0.0 delta)!
				{
					TdV2Plane explicitPb(w, h, 0.0);
					for (int y = 0; y < h; ++y)
						for (int x = 0; x < w; ++x)
							explicitPb.at(x, y) = scene(static_cast<double>(x) + 1.5, static_cast<double>(y));
					TdV2LateCarryController pbCtrl;
					pbCtrl.OnOnTimeFrame(onTimeDelta, nullptr, &onTimeClean);
					pbCtrl.OnLateFrame(curClean1, active, &mf1, stepTotalVar, cellsW, cellsH, cellPx, nullptr, nullptr);
					LateCarryContext ctxWithPb = ctx2;
					ctxWithPb.codecModelPb = &explicitPb;
					const LateCarryDecision decWithPb = pbCtrl.OnLateFrame(
						curClean2, active, &mf2, stepTotalVar, cellsW, cellsH, cellPx, nullptr, &ctxWithPb);
					Assert::IsTrue(decWithPb.usedTransformProjection,
						L"Explicit codecModelPb Hop 2 must run 8x8 DCT projection");
					Assert::AreEqual(0.0, MaxAbsPlaneRect(decWithPb.delta, ActiveRect{ 40, 40, 48, 48 }), 1e-6,
						L"Explicit codecModelPb Hop 2 block (40,40) must enforce STRICT 8x8 DCT projection");
				}

				// 4. MANDATORY IN-TEST NEGATIVE CONTROL (Rule 3): Run an unprotected Hop-2 baseline that drops
				//    Hop-1 prevCleanValidMask (treating Hop-1's fallback-poisoned warpedPrevClean as valid everywhere,
				//    exactly like pre-fix code) on the EXACT SAME (onTimeClean, curClean1, curClean2, mf1, mf2)!
				{
					TdV2Plane naiveHop1Clean, naiveHop2Clean;
					std::vector<uint8_t> naiveMask1, naiveMask2;
					WarpCleanReference(onTimeClean, curClean1, active, mf1.vx, mf1.vy, cellsW, cellsH, cellPx,
						naiveHop1Clean, naiveMask1, nullptr, &mf1.valid);
					// Unprotected Hop 2 ignores naiveMask1 == 0 in [16,32)^2 and warps naiveHop1Clean as valid!
					WarpCleanReference(naiveHop1Clean, curClean2, active, mf2.vx, mf2.vy, cellsW, cellsH, cellPx,
						naiveHop2Clean, naiveMask2, nullptr, &mf2.valid);

					// Feed naiveHop2Clean as explicit referenceClean + codecModelPb (valid everywhere) into a fresh controller at Hop 2:
					TdV2LateCarryController naiveCtrl;
					naiveCtrl.OnOnTimeFrame(onTimeDelta, nullptr, &onTimeClean);
					naiveCtrl.OnLateFrame(curClean1, active, &mf1, stepTotalVar, cellsW, cellsH, cellPx, nullptr, nullptr);
					LateCarryContext naiveCtx2 = ctx2;
					naiveCtx2.codecModelPb = &naiveHop2Clean;
					naiveCtx2.referenceClean = &naiveHop2Clean;
					const LateCarryDecision naiveDec2 = naiveCtrl.OnLateFrame(
						curClean2, active, &mf2, stepTotalVar, cellsW, cellsH, cellPx, nullptr, &naiveCtx2);

					Assert::IsTrue(naiveDec2.residualRejectedPixels >= 32u,
						FormatMsg("Sub-case A negative control: unprotected Hop-2 rejected %.0f < 32 pixels on poisoned -80 ref",
							static_cast<double>(naiveDec2.residualRejectedPixels)).c_str());
					Assert::IsTrue(std::fabs(naiveDec2.delta.at(10, 20) - 3.0) > 1.5,
						FormatMsg("Sub-case A negative control: unprotected Hop-2 delta(10,20)=%.4f was not distorted > 1.5 codes",
							naiveDec2.delta.at(10, 20)).c_str());
				}
			}

			// -------------------------------------------------------------------------
			// Sub-case B: refGuide == nullptr with transformIdentified8x8 == true and
			//             codecModelPb == nullptr MUST fall back to ApplyTransformUnknownBound
			//             (NEVER synthesize predClean = curClean with sentRes == 0!)
			// -------------------------------------------------------------------------
			{
				const int w = 32;
				const int h = 32;
				const int cellsW = w / cellPx; // 2
				const int cellsH = h / cellPx; // 2
				const ActiveRect active = ActiveRect::Full(w, h);

				const TdV2Plane curClean(w, h, 420.0);
				// Plant constant +3.0 DC carried delta -> Hop 1 decay (2/3) = +2.0 everywhere.
				// In 8x8 DCT: DC = 8 * 2.0 = 16.0 > theta (10.0):
				//   - Buggy predClean = curClean (sentRes == 0) clips DC to theta = 10.0 -> pixel delta = 10/8 = 1.25!
				//   - Correct ApplyTransformUnknownBound (3 * sqrt(12) = 10.39) preserves pixel delta = +2.00 EXACTLY!
				const TdV2Plane onTimeDelta(w, h, +3.0);
				const CellMotionField stillMf = CellMotionField::Uniform(
					cellsW, cellsH, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				TdV2LateCarryController ctrlB;
				StandInRegistry siB;
				ctrlB.OnOnTimeFrame(onTimeDelta, &siB, /*onTimeClean=*/nullptr); // NO stored prevClean_!

				LateCarryContext ctxB;
				ctxB.transformIdentified8x8 = true;
				ctxB.residualMode = ResidualMode::Strict;
				ctxB.stepDelta = stepDelta;
				ctxB.deadZonePhi = phi;
				ctxB.sourceBits = 10;
				ctxB.cellProjIterations = 2;
				ctxB.codecModelPb = nullptr;
				ctxB.referenceClean = nullptr;

				const LateCarryDecision decB = ctrlB.OnLateFrame(
					curClean, active, &stillMf, stepTotalVar, cellsW, cellsH, cellPx, &siB, &ctxB);

				Assert::IsTrue(decB.carried && !decB.expired && !decB.usesL3Clamp,
					L"Sub-case B must carry cleanly without L3 clamp");
				Assert::IsFalse(decB.usedTransformProjection,
					L"Sub-case B with null codecModelPb and null refGuide must NOT claim 8x8 DCT projection ran");
				Assert::IsTrue(siB.IsActive(StandInKind::TransformUnknownBound),
					L"Sub-case B must visibly engage StandInKind::TransformUnknownBound");
				Assert::IsTrue(siB.IsActive(StandInKind::CodecModelPbStandIn),
					L"Sub-case B with transformIdentified8x8=true and null codecModelPb must visibly engage CodecModelPbStandIn (H-1)");

				// Exact +2.0 carried delta everywhere (NOT clipped to theta / 8 = 1.25!).
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						Assert::AreEqual(2.0, decB.delta.at(x, y), 1e-6,
							FormatMsg("Sub-case B delta(%.0f,%.0f)=%.6f != 2.0 (buggy predClean=curClean clipped to 1.25?)",
								static_cast<double>(x), static_cast<double>(y), decB.delta.at(x, y)).c_str());
					}
				}

				// MANDATORY IN-TEST NEGATIVE CONTROL (Rule 3) on the EXACT SAME (curClean, onTimeDelta, stillMf, stepDelta=12, phi=1/6):
				// 1. Direct naive predClean = curClean (sentRes == 0, blendRes == +2.0) 8x8 STRICT DCT clips DC from 16.0 to
				//    theta = 10.0 -> pixel delta = theta / 8 = 1.25 (0.75 code distortion proving the hazard is real!).
				double sentZero[64] = {};
				double blendTwo[64] = {};
				double projOut[64] = {};
				for (int i = 0; i < 64; ++i)
					blendTwo[i] = 2.0;
				ProjectResidual8x8Dct(sentZero, blendTwo, stepDelta, phi, ResidualMode::Strict, projOut);
				const double expectedBuggyClip = theta / 8.0; // 1.25
				Assert::AreEqual(expectedBuggyClip, projOut[0], 1e-6,
					FormatMsg("Sub-case B negative control: naive sentRes=0 DCT output=%.6f != %.6f",
						projOut[0], expectedBuggyClip).c_str());
				Assert::IsTrue(std::fabs(decB.delta.at(0, 0) - projOut[0]) > 0.70,
					L"Sub-case B negative control: protected fallback must differ from buggy sentRes=0 clip by > 0.70 codes");

				// 2. Positive control: supplying an explicit Field B codecModelPb (= 420.0) on the EXACT SAME inputs
				//    genuinely executes 8x8 DCT STRICT projection (delta == 1.25, usedTransformProjection == true)!
				TdV2LateCarryController ctrlPb;
				StandInRegistry siPb;
				ctrlPb.OnOnTimeFrame(onTimeDelta, &siPb, /*onTimeClean=*/nullptr);
				LateCarryContext ctxPb = ctxB;
				ctxPb.codecModelPb = &curClean;
				const LateCarryDecision decPb = ctrlPb.OnLateFrame(
					curClean, active, &stillMf, stepTotalVar, cellsW, cellsH, cellPx, &siPb, &ctxPb);
				Assert::IsTrue(decPb.carried && decPb.usedTransformProjection,
					L"Sub-case B positive control: explicit codecModelPb must execute 8x8 DCT projection");
				Assert::AreEqual(expectedBuggyClip, decPb.delta.at(0, 0), 1e-6,
					L"Sub-case B positive control: explicit codecModelPb with sentRes=0 must clip DC to theta/8 = 1.25");
				Assert::IsFalse(siPb.IsActive(StandInKind::CodecModelPbStandIn),
					L"Sub-case B positive control: explicit codecModelPb must not engage CodecModelPbStandIn");
			}
		}

		// =========================================================================
		// 9. Opus Repeat Audit LOW-1: Late-Carry Frame 1 Explicit referenceClean
		//    Must Advance prevClean_ So Frame 2 (Without Explicit Ref) Warps 1 Hop
		// =========================================================================
		TEST_METHOD(OpusLow1LateCarryFrame1ExplicitRefAdvancesPrevCleanForFrame2)
		{
			// On-time frame 0 has a sharp vertical step edge at x = 16 (600 left, 100 right) with +12.0 clean carry delta.
			// Late frame 1 shifts scene +8 px (step at x = 24, vx = -8.0) and supplies explicit ctx.referenceClean at x = 24.
			// Late frame 2 shifts scene another +8 px (step at x = 32, vx = -8.0) with NO explicit referenceClean (nullptr).
			// In interior strip x in [26..30), y in [16..48), 1-hop warp lands at sx in [18..22) (bicubic taps [17..23]):
			//   - Properly advanced prevClean_ (step at x = 24) yields refVal = 600 vs curVal = 594 -> desiredDir = +6 > 0 ->
			//     exact +4.0 (= +12 * 1/3) carry applied with zero false rejections on the strip!
			//   - Pre-fix stale 2-hop-old prevClean_ (step stuck at x = 16) yields refVal = 100 vs curVal = 594 ->
			//     desiredDir = -494 < 0 -> Q7a falsely rejects 100% of valid carry on [26..30) (delta == 0.0)!
			const int w = 64;
			const int h = 64;
			const int cellPx = 16;
			const int cellsW = w / cellPx; // 4
			const int cellsH = h / cellPx; // 4
			const ActiveRect active = ActiveRect::Full(w, h);
			const ActiveRect stepStrip{ 26, 16, 30, 48 }; // 4 x 32 = 128 pixels strictly inside shifted step band

			const double stepDelta = 12.0;
			const double phi = kDeadZonePhiInter;
			const RoundingCellStats a2 = ComputeRoundingCellStats(stepDelta, phi, 0.0);
			Assert::IsTrue(a2.valid && a2.totalVar > 0.0);
			const double stepTotalVar = a2.totalVar;

			auto makeStepPlane = [&](int stepX, double bias) -> TdV2Plane
			{
				TdV2Plane p(w, h, 0.0);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						p.at(x, y) = (x < stepX ? 600.0 : 100.0) + bias;
				return p;
			};

			const TdV2Plane onTimeClean = makeStepPlane(16, 0.0);
			const TdV2Plane onTimeDelta(w, h, +12.0);
			const CellMotionField mfShift = CellMotionField::Uniform(
				cellsW, cellsH, -8.0, 0.0, -8.0, 0.0, RegionMotionClass::Camera);

			const TdV2Plane refClean1 = makeStepPlane(24, 0.0);
			const TdV2Plane curClean1 = makeStepPlane(24, -6.0);
			const TdV2Plane curClean2 = makeStepPlane(32, -6.0);

			LateCarryContext ctx1;
			ctx1.transformIdentified8x8 = false;
			ctx1.referenceClean = &refClean1;

			TdV2LateCarryController ctrl;
			StandInRegistry si;
			ctrl.OnOnTimeFrame(onTimeDelta, &si, &onTimeClean);

			const LateCarryDecision res1 = ctrl.OnLateFrame(
				curClean1, active, &mfShift, stepTotalVar, cellsW, cellsH, cellPx, &si, &ctx1);
			Assert::IsTrue(res1.carried && !res1.expired, L"Late frame 1 with explicit referenceClean must carry");
			Assert::AreEqual(static_cast<size_t>(0u), res1.residualRejectedPixels,
				L"Late frame 1 aligned explicit referenceClean must have 0 residual rejections");
			Assert::IsTrue(res1.delta.at(28, 32) > 2.0,
				L"Late frame 1 must apply positive carried delta");

			// Late frame 2 WITHOUT explicit referenceClean (ctx == nullptr -> warps stored prevClean_ by 1 hop).
			const LateCarryDecision res2 = ctrl.OnLateFrame(
				curClean2, active, &mfShift, stepTotalVar, cellsW, cellsH, cellPx, &si, nullptr);
			Assert::IsTrue(res2.carried && !res2.expired, L"Late frame 2 without explicit referenceClean must carry");
			for (int y = stepStrip.y0; y < stepStrip.y1; ++y)
			{
				for (int x = stepStrip.x0; x < stepStrip.x1; ++x)
				{
					Assert::AreEqual(4.0, res2.delta.at(x, y), 1e-6,
						FormatMsg("Opus LOW-1: Late frame 2 delta(%.0f,%.0f)=%.6f != 4.0 (stale 2-hop prevClean_ falsely triggered Q7a rejection!)",
							static_cast<double>(x), static_cast<double>(y), res2.delta.at(x, y)).c_str());
				}
			}

			// Mandatory Same-Input Negative Control (Rule 3):
			// Exact same frame 0 + frame 1 + motion mfShift + ctx == nullptr, EXCEPT frame 2 curClean genuinely has an
			// unaligned scene cut/jump (900.0 everywhere -> refVal - curVal < 0 opposing +4.0 carry). Q7a MUST reject!
			const TdV2Plane curClean2Cut(w, h, 900.0);
			TdV2LateCarryController negCtrl;
			StandInRegistry negSi;
			negCtrl.OnOnTimeFrame(onTimeDelta, &negSi, &onTimeClean);
			const LateCarryDecision negRes1 = negCtrl.OnLateFrame(
				curClean1, active, &mfShift, stepTotalVar, cellsW, cellsH, cellPx, &negSi, &ctx1);
			Assert::IsTrue(negRes1.carried && !negRes1.expired);
			const LateCarryDecision negRes2 = negCtrl.OnLateFrame(
				curClean2Cut, active, &mfShift, stepTotalVar, cellsW, cellsH, cellPx, &negSi, nullptr);
			Assert::IsTrue(negRes2.carried && !negRes2.expired,
				L"Negative control late frame 2 must execute Q7a residual check (not expire)");
			Assert::AreEqual(0.0, MaxAbsPlaneRect(negRes2.delta, stepStrip), 1e-9,
				L"Negative control: unaligned scene cut on frame 2 MUST be 100% rejected by Q7a on stepStrip");
			Assert::IsTrue(negRes2.residualRejectedPixels >= static_cast<size_t>(stepStrip.Width() * stepStrip.Height()),
				L"Negative control: unaligned scene cut on frame 2 MUST report residualRejectedPixels >= strip size");
		}
	};
} // namespace TdV2SchedulerConfigNs
} // namespace Tests
