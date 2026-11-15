// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S-3a S3 + S4 pure parts (lead9/brief-S-3a-scheduler.md s4 S3, S4): the Configure-time admission table (core/AdmissionTable.h) and the ONE
// overload arbiter (core/OverloadOrder.h). Pure CPU. Rates and sizes appear only as test INPUTS (a sweep), never as branches in the code.
// Not here yet: S3-T6 (refs/123 tier row: needs the 123 tier rows in code), S4-T6 (gap demotion: wiring) and the live GPU overload run.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/AdmissionTable.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackPlainText.h>
#include <VP-V/core/OverloadOrder.h>
#include <VP-V/core/PostPresentSpare.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpSkeleton;
using namespace VpSkeleton::Admission;

namespace AdmissionOverloadTest
{
	namespace
	{
		void Put(AdmissionFacts& f, ProductId id, double ms, bool measured = true)
		{
			f.present[id] = true;
			f.costMs[id] = ms;
			f.measured[id] = measured;
		}

		// A ledger-shaped fact set: core P1 / P3 / P4 / P11 / P16, fill P5 / P9 / P10, costs scaled by `scale` (the pixel ratio of a sweep).
		AdmissionFacts Facts(double hz, double renderMs, double scale)
		{
			AdmissionFacts f;
			f.periodMs = 1000.0 / hz;
			f.renderMs = renderMs;
			Put(f, kP1HalfLuma, 0.10 * scale);
			Put(f, kP3AnalysisField, 1.00 * scale);
			Put(f, kP4SourceStats, 0.05 * scale);
			Put(f, kP11RegionBlockStats, 0.30 * scale);
			Put(f, kCodingEvidence, 0.20 * scale);
			Put(f, kP5MonitorTiles, 0.40 * scale);
			Put(f, kP9CameraSmoothed, 0.05 * scale);
			Put(f, kP10GrainSplit, 1.50 * scale);
			f.tdMinMs = 1.0 * scale;
			f.tdWantMs = 3.0 * scale;
			return f;
		}

		bool CoreAllAdmitted(const AdmissionFacts& f, const Result& r)
		{
			const ProductId core[] = { kP1HalfLuma, kP3AnalysisField, kP4SourceStats, kP11RegionBlockStats, kCodingEvidence };
			for (ProductId id : core)
				if (f.present[id] && !r.admitted[id]) return false;
			return true;
		}

		int FillOn(const AdmissionFacts& f, const Result& r)
		{
			int n = 0;
			for (size_t i = 0; i < kProductCount; ++i)
				if (f.present[i] && r.cls[i] == JobClass::Fill && r.admitted[i]) ++n;
			return n;
		}

		SlotBudget Load(double periodMs, double renderMs, double usedMs)
		{
			SlotBudget b = {};
			b.periodMs = periodMs;
			b.renderMs = renderMs;
			b.usedMs = usedMs;
			b.budgetMs = periodMs - renderMs - periodMs * 0.1;
			b.measured = true;
			return b;
		}
	}

	TEST_CLASS(AdmissionTableTests)
	{
	public:
		// S3-T1: scaling the ledger up drops fill first, then shrinks the TD slice; core is never dropped.
		TEST_METHOD(AdmissionFollowsMeasuredCosts)
		{
			int lastFill = 1000;
			double lastSlice = 1e9;
			bool sliceShrankAfterFill = false;
			for (double scale = 0.5; scale <= 8.0; scale *= 1.25)
			{
				const AdmissionFacts f = Facts(30.0, 12.0, scale);
				const Result r = Admit(f);
				Assert::IsTrue(CoreAllAdmitted(f, r), L"core never dropped");
				const int fill = FillOn(f, r);
				Assert::IsTrue(fill <= lastFill, L"fill only drops as costs rise");
				if (r.sliceMs < lastSlice - 1e-9 && fill == 0) sliceShrankAfterFill = true;
				Assert::IsTrue(r.sliceMs <= f.tdWantMs + 1e-9);
				Assert::IsTrue(r.sliceMs + r.fillMs <= (std::max)(0.0, r.budgetMs - r.coreMs) + 1e-9, L"slice + fill fit the remainder");
				lastFill = fill;
				lastSlice = r.sliceMs;
			}
			Assert::IsTrue(sliceShrankAfterFill);
		}

		// S3-T2: a sweep of rates and sizes; the budget follows the one law, no NaN, no constant branch.
		TEST_METHOD(NoLiteralRatesSweep)
		{
			const double rates[] = { 23.976, 24.0, 25.0, 29.97, 30.0, 47.952, 50.0, 59.94, 60.0, 100.0, 119.88, 120.0 };
			const double pixels[] = { 1280.0 * 720.0, 1920.0 * 1080.0, 2560.0 * 1440.0, 3840.0 * 2160.0, 7680.0 * 4320.0 };
			for (double hz : rates)
				for (double px : pixels)
				{
					const double scale = px / (1920.0 * 1080.0);
					const AdmissionFacts f = Facts(hz, 3.0 * scale, scale);
					const Result r = Admit(f);
					const double period = 1000.0 / hz;
					Assert::AreEqual((std::max)(0.0, period - f.renderMs - period * 0.1), r.budgetMs, 1e-9);
					Assert::IsTrue(std::isfinite(r.sliceMs) && r.sliceMs >= 0.0);
					Assert::IsTrue(CoreAllAdmitted(f, r));
					Assert::IsFalse(r.line.empty());
				}
		}

		// S3-T3: the fixed core set is admitted even when it alone exceeds the budget (visible; the arbiter acts).
		TEST_METHOD(CoreFixedSetAlwaysAdmitted)
		{
			AdmissionFacts f = Facts(60.0, 14.0, 4.0);
			const Result r = Admit(f);
			Assert::IsTrue(r.coreOver);
			Assert::IsTrue(CoreAllAdmitted(f, r));
			Assert::AreEqual(0, FillOn(f, r));
			Assert::AreEqual(0.0, r.sliceMs);
			Assert::IsTrue(r.line.find("OVER BUDGET") != std::string::npos);
		}

		// S3-T4: an unmeasured optional product is not admitted and says so; unmeasured core is still admitted and named.
		TEST_METHOD(UnmeasuredCostAdmitsNothingOptional)
		{
			AdmissionFacts f = Facts(24.0, 8.0, 1.0);
			f.measured[kP5MonitorTiles] = false;
			f.measured[kP3AnalysisField] = false;
			const Result r = Admit(f);
			Assert::IsFalse(r.admitted[kP5MonitorTiles]);
			Assert::IsTrue(r.notMeasured[kP5MonitorTiles]);
			Assert::IsTrue(r.admitted[kP3AnalysisField]);
			Assert::AreEqual(1u, r.unmeasuredCore);
			Assert::IsTrue(r.line.find("not measured") != std::string::npos);
			Assert::IsTrue(r.line.find("some core not measured") != std::string::npos);
		}

		// S3-T5 (pure part): the lines per mode. Competitive = no slot work; games = no fill and no TD slice; estimates are labelled.
		TEST_METHOD(StatusLinesPerMode)
		{
			AdmissionFacts f = Facts(60.0, 6.0, 1.0);
			f.mode = LatencyMode::Competitive;
			Result r = Admit(f);
			Assert::AreEqual(std::string("admission (competitive): no slot work (latency 0)"), r.line);
			for (size_t i = 0; i < kProductCount; ++i) Assert::IsFalse(r.admitted[i]);

			f.mode = LatencyMode::Games;
			r = Admit(f);
			Assert::AreEqual(0, FillOn(f, r));
			Assert::AreEqual(0.0, r.sliceMs);
			Assert::IsTrue(r.line.find("(games: none)") != std::string::npos);
			Assert::IsTrue(CoreAllAdmitted(f, r));

			f.mode = LatencyMode::AvrSafe;
			f.estimated = true;
			r = Admit(f);
			Assert::IsTrue(r.line.find("AVR-safe, estimated") != std::string::npos);
			f.mode = LatencyMode::Standard;
			const Result s = Admit(f);
			Assert::AreEqual(s.sliceMs, r.sliceMs, L"standard and AVR-safe admit the same way");
		}

		// Q3: the TD slice comes from the measured remainder; below TD's minimum it is the reduced tier, visibly.
		TEST_METHOD(TdSliceFromMeasuredRemainder)
		{
			AdmissionFacts f = Facts(30.0, 26.0, 1.0);   // remainder ~2.36 ms: below want 3, above min 1
			Result r = Admit(f);
			const double left = r.budgetMs - r.coreMs;
			Assert::AreEqual((std::min)(f.tdWantMs, left), r.sliceMs, 1e-9);
			Assert::IsFalse(r.tdReduced);
			f.renderMs = 27.5;
			r = Admit(f);
			Assert::IsTrue(r.tdReduced);
			Assert::IsTrue(r.line.find("reduced tier") != std::string::npos);
		}

		// Owner answer via lead 11 (2026-11-14): the ahead work is sized to the VRAM grant in order - fewer products early, then fewer frames
		// ahead, the queue shortened only last and never below the delay budget's minimum (no 200 ms floor: coordinator 2026-11-14 ruling via
		// lead 11, design s9 :168-176); games/competitive queues never touched; nothing evicted, every step on the line.
		// PREC2-S3A MUST 1: the plan is THE ladder's (QueueWindow.h PlanAheadLadder): bytes per product = bytesPerFrame x (1 + back + depth),
		// S-A sheds Fill first, S-B keeps the core's declared depth (aheadMinFrames 1), S-C is DelayBudget's next lower step for the same budget.
		TEST_METHOD(AheadPlanFollowsTheGrantInOrder)
		{
			AdmissionFacts f = Facts(60.0, 6.0, 1.0);
			f.aheadBytes[kP3AnalysisField] = 100;
			f.aheadBytes[kP5MonitorTiles] = 50;
			f.aheadFeed.activeConsumers = { AheadDemand{ kAheadCeiling, "TD" } };   // OD 11-14: aheadCompute from the declared need (= the ceiling itself)
			f.aheadFeed.queueAheadFrames = 15;
			// The delay budget the ladder's S-C steps through (rates as test inputs): 250 ms, 60 / 60 Hz -> fixed (2 + 1) refreshes, 4 queued.
			f.delayBudget.budgetMs = 250.0;
			f.delayBudget.sourceFps = 60.0;
			f.delayBudget.refreshHz = 60.0;
			f.delayBudgetFed = true;
			const int designQueue = DelayBudget::Compute(f.delayBudget).queueFrames;
			Assert::AreEqual(DelayBudget::kDesignDepth, designQueue);
			f.queueFrames = designQueue;
			f.vramGrantBytes = 2000;
			Result r = Admit(f);
			Assert::IsTrue(r.admitted[kP5MonitorTiles]);
			Assert::IsTrue(r.aheadAdmitted[kP3AnalysisField] && r.aheadAdmitted[kP5MonitorTiles]);
			Assert::AreEqual(int(kAheadCeiling), r.aheadFrames);
			Assert::AreEqual(uint64_t(1 + kAheadCeiling) * (100 + 50), r.aheadNeedBytes);
			Assert::AreEqual(designQueue, r.queueFrames);

			f.vramGrantBytes = 1000;   // 1. the optional product moves to render time (S-A: 7 x 100 + 1 x 50 = 750)
			r = Admit(f);
			Assert::AreEqual(1u, r.aheadProductsDropped);
			Assert::IsFalse(r.aheadAdmitted[kP5MonitorTiles]);
			Assert::IsTrue(r.aheadAdmitted[kP3AnalysisField]);
			Assert::AreEqual(int(kAheadCeiling), r.aheadFrames);
			Assert::IsTrue(r.aheadLine.find("fewer products early") != std::string::npos);

			f.vramGrantBytes = 500;    // 2. fewer frames ahead (S-B: (1 + 3) x 100 + 50 = 450)
			r = Admit(f);
			Assert::AreEqual(3, r.aheadFrames);
			Assert::AreEqual(designQueue, r.queueFrames, L"the queue is untouched while fewer frames ahead suffice");
			Assert::AreEqual(0, r.queueFramesDropped);
			Assert::IsTrue(r.aheadLine.find("fewer frames ahead") != std::string::npos);

			// 3. LAST: the queue. With 2 frames queued ahead the ahead cap follows the shorter queue (ahead - frames given up): S-B leaves P3 at
			// its declared 1 (250), the first S-C step keeps cap 1 (250), the second (queue 2) takes the cap to 0 (100 + 50 = 150).
			f.aheadFeed.queueAheadFrames = 2;
			f.vramGrantBytes = 260;
			r = Admit(f);
			Assert::AreEqual(1, r.aheadFrames);
			Assert::AreEqual(designQueue, r.queueFrames, L"S-B suffices: no S-C step");
			f.vramGrantBytes = 225;
			r = Admit(f);
			Assert::IsFalse(r.aheadOverGrant);
			Assert::AreEqual(DelayBudget::kMinimumDepth, r.queueFrames);
			Assert::AreEqual(designQueue - DelayBudget::kMinimumDepth, r.queueFramesDropped);
			Assert::AreEqual(0, r.aheadFrames, L"the shorter queue leaves no frame ahead");
			Assert::IsTrue(r.aheadLine.find("queue shortened") != std::string::npos);

			f.vramGrantBytes = 100;    // over at the delay budget's floor (DelayBudget::kMinimumDepth): nothing early, visible
			r = Admit(f);
			Assert::IsTrue(r.aheadOverGrant);
			Assert::AreEqual(DelayBudget::kMinimumDepth, r.queueFrames, L"never below the delay budget's minimum queue");
			Assert::IsTrue(r.aheadLine.find("limited by VRAM at the queue floor") != std::string::npos);
			Assert::IsFalse(r.aheadAdmitted[kP3AnalysisField]);
			Assert::IsTrue(r.admitted[kP3AnalysisField], L"core still admitted: it fills at its own render (never evicted)");
			Assert::IsTrue(r.aheadLine.find("ahead work at render") != std::string::npos);

			f.delayBudgetFed = false;   // no delay budget: no S-C step, and the line says why
			f.vramGrantBytes = 225;
			r = Admit(f);
			Assert::IsTrue(r.aheadOverGrant);
			Assert::AreEqual(designQueue, r.queueFrames, L"no budget fed: the queue is not touched");
			Assert::AreEqual(0, r.queueFramesDropped);
			Assert::IsTrue(r.aheadLine.find("delay budget not fed") != std::string::npos);
			f.delayBudgetFed = true;
			f.aheadFeed.queueAheadFrames = 15;

			f.mode = LatencyMode::AvrSafe;   // the 200 ms AVR-safe tier is a CEILING: the plan only shortens; its S-C floor is kMinimumDepth
			f.delayBudget.budgetMs = 200.0;
			const int avrQueue = DelayBudget::Compute(f.delayBudget).queueFrames;   // the tier's queue (DelayBudget owns it)
			f.queueFrames = avrQueue;
			f.vramGrantBytes = 2000;
			r = Admit(f);
			Assert::AreEqual(avrQueue, r.queueFrames, L"AVR-safe: the plan never lengthens the queue past the 200 ms tier");
			Assert::IsTrue(r.queueFrames * f.periodMs <= 200.0 + 1e-9);
			f.aheadFeed.queueAheadFrames = 2;
			f.vramGrantBytes = 100;
			r = Admit(f);
			Assert::AreEqual(DelayBudget::kMinimumDepth, r.queueFrames, L"AVR-safe: S-C floor = the ladder floor");
			Assert::IsTrue(r.queueFrames <= avrQueue);
			f.aheadFeed.queueAheadFrames = 15;

			f.mode = LatencyMode::Games;
			f.queueFrames = 1;
			r = Admit(f);
			Assert::AreEqual(0, r.aheadFrames, L"games: no look-ahead work");
			Assert::AreEqual(1, r.queueFrames, L"games: S-C is a no-op, never a raise");
			Assert::AreEqual(0, r.queueFramesDropped);

			f.mode = LatencyMode::Competitive;
			f.queueFrames = 0;
			r = Admit(f);
			Assert::AreEqual(0, r.queueFrames, L"competitive: no queueing added");

			f.mode = LatencyMode::Standard;   // the ahead ceiling binds visibly (G7; OD 11-14 kAheadCeiling replaces kMaxLead 13)
			f.delayBudget.budgetMs = 250.0;
			f.queueFrames = designQueue;
			f.vramGrantBytes = 2000;
			f.aheadFeed.activeConsumers[0].need = uint16_t(kAheadCeiling + 3);
			r = Admit(f);
			Assert::AreEqual(int(kAheadCeiling), r.aheadFrames);
			Assert::IsTrue(r.aheadBound == AheadBound::Ceiling);
			Assert::IsTrue(r.aheadComputeLine.find("ceiling " + std::to_string(kAheadCeiling)) != std::string::npos);
		}

		// OD 11-14 (lead10/design-queue-keyed-products.md s10): aheadCompute = min(max over ACTIVE consumers of the declared need,
		// kAheadCeiling, queue frames ahead); 0 with no active consumer; a ceiling, not a target (the budget may plan lower); the binding
		// consumer and the bound (ceiling | budget | queue) are on the Ctrl+Alt+D line, and on Ctrl+I / Config status when ceiling or budget bind.
		TEST_METHOD(AheadComputeIsTheLargestActiveNeedUnderTheCeiling)
		{
			AdmissionFacts f = Facts(60.0, 6.0, 1.0);
			f.aheadBytes[kP3AnalysisField] = 100;
			f.queueFrames = 16;
			f.aheadFeed.queueAheadFrames = 15;
			f.vramGrantBytes = 100000;

			Result r = Admit(f);   // no consumer at all: nothing ahead
			Assert::AreEqual(0, r.aheadFrames);
			Assert::IsTrue(r.aheadBound == AheadBound::NoConsumer);
			Assert::AreEqual(std::string("ahead compute 0 (no active consumer)"), r.aheadComputeLine);

			// the window's feed lists the ACTIVE consumers only (PlanAheadFeedOf); a listed consumer requesting nothing (need 0) is ignored
			f.aheadFeed.activeConsumers = { AheadDemand{ 4, "motion" }, AheadDemand{ 5, "TD" }, AheadDemand{ 0, "idle" } };
			r = Admit(f);
			Assert::AreEqual(5, r.aheadFrames, L"the max of the ACTIVE declared needs");
			Assert::AreEqual(5, r.aheadComputeLimit);
			Assert::AreEqual(std::string("ahead compute 5 (by TD)"), r.aheadComputeLine);
			Assert::IsFalse(r.aheadShortNotice, L"consumer-bound: Ctrl+Alt+D only");
			Assert::IsFalse(r.aheadCeilingInfo);

			f.aheadFeed.activeConsumers = { AheadDemand{ 0, "idle" } };   // motion and TD inactive: no longer in the feed
			r = Admit(f);
			Assert::AreEqual(0, r.aheadFrames, L"no ACTIVE consumer gives 0");

			f.aheadFeed.activeConsumers = { AheadDemand{ uint16_t(kAheadCeiling + 3), "TD" } };
			r = Admit(f);
			Assert::AreEqual(int(kAheadCeiling), r.aheadFrames, L"a request above the ceiling gives the ceiling");
			Assert::AreEqual("ahead compute " + std::to_string(kAheadCeiling) + " (by TD; ceiling " + std::to_string(kAheadCeiling) + ")", r.aheadComputeLine);
			Assert::IsTrue(r.aheadCeilingInfo, L"held at the owner's cap: Ctrl+I info line");
			Assert::IsFalse(r.aheadShortNotice, L"the cap is the designed state, not a downgrade (lead 12 R3)");

			f.aheadFeed.activeConsumers[0].need = 4;
			f.aheadFeed.queueAheadFrames = 3;
			r = Admit(f);
			Assert::AreEqual(3, r.aheadFrames, L"never more than the frames queued ahead");
			Assert::AreEqual(std::string("ahead compute 3 (by TD; queue)"), r.aheadComputeLine);
			Assert::IsTrue(r.aheadShortNotice, L"less than asked within the cap: visible notice");

			f.aheadFeed.queueAheadFrames = 15;
			f.vramGrantBytes = 100 * (1 + 2);   // the budget plans lower than the need (S-B, THE ladder's bytes: render + 2 ahead): 2 frames
			r = Admit(f);
			Assert::AreEqual(4, r.aheadComputeLimit);
			Assert::AreEqual(2, r.aheadFrames, L"the ceiling / the need is an upper bound, not a target");
			Assert::AreEqual(std::string("ahead compute 2 (by TD; budget)"), r.aheadComputeLine);
			Assert::IsTrue(r.aheadShortNotice);

			f.mode = LatencyMode::Games;
			f.queueFrames = 1;
			f.aheadFeed.queueAheadFrames = 0;
			r = Admit(f);
			Assert::AreEqual(0, r.aheadFrames);
			Assert::AreEqual(std::string("ahead compute 0 (by TD; games: backward-only)"), r.aheadComputeLine);
		}

		// Gate GWC C1 (lead17 gate-S3A-schk, ba21dfe76): a PARTIAL S-C stop at the admission level. One frame queued ahead, so S-B cannot
		// shrink P3 below its declared 1 frame; the first S-C step (DelayBudget 4 -> 3 queued) takes the ahead cap to 0 and fits any grant in
		// [150, 249] bytes (P3 100 + P5 50 at the render frame only), so the ladder stops there: queue 3, one frame given up, 0 frames ahead.
		TEST_METHOD(PartialShorterQueueStopsAtTheFirstStepThatFits)
		{
			AdmissionFacts f = Facts(60.0, 6.0, 1.0);
			f.aheadBytes[kP3AnalysisField] = 100;
			f.aheadBytes[kP5MonitorTiles] = 50;
			f.aheadFeed.activeConsumers = { AheadDemand{ kAheadCeiling, "TD" } };
			f.aheadFeed.queueAheadFrames = 1;
			f.delayBudget.budgetMs = 250.0;
			f.delayBudget.sourceFps = 60.0;
			f.delayBudget.refreshHz = 60.0;
			f.delayBudgetFed = true;
			const int designQueue = DelayBudget::Compute(f.delayBudget).queueFrames;
			Assert::AreEqual(DelayBudget::kDesignDepth, designQueue);
			f.queueFrames = designQueue;
			for (uint64_t grant : { uint64_t(150), uint64_t(200), uint64_t(249) })
			{
				f.vramGrantBytes = grant;
				const Result r = Admit(f);
				Assert::IsFalse(r.aheadOverGrant, L"the first S-C step fits: not at the floor");
				Assert::AreEqual(3, r.queueFrames, L"one S-C step: 4 -> 3 queued, then the ladder stops");
				Assert::AreEqual(1, r.queueFramesDropped);
				Assert::AreEqual(0, r.aheadFrames, L"the shorter queue leaves no frame ahead");
				Assert::IsTrue(r.queueFrames > DelayBudget::kMinimumDepth, L"stopped above the floor (partial)");
				Assert::IsTrue(r.aheadLine.find("queue shortened") != std::string::npos);
			}
		}

		// C4 s3a SHOULD 1 (lead17 opus-audit-s3a-inc): with no S-C step the queue result is the frames the queue holds now
		// (facts.queueFrames), never replaced by DelayBudget's own value; only a real S-C step (below the delay budget's start) changes it.
		TEST_METHOD(NoShorterQueueStepKeepsTheQueueHeldNow)
		{
			AdmissionFacts f = Facts(60.0, 6.0, 1.0);
			f.aheadBytes[kP3AnalysisField] = 100;
			f.aheadFeed.activeConsumers = { AheadDemand{ 4, "TD" } };
			f.aheadFeed.queueAheadFrames = 15;
			f.delayBudget.budgetMs = 250.0;
			f.delayBudget.sourceFps = 60.0;
			f.delayBudget.refreshHz = 60.0;
			f.delayBudgetFed = true;
			const int designQueue = DelayBudget::Compute(f.delayBudget).queueFrames;
			f.queueFrames = designQueue - 1;   // the queue holds fewer than the delay budget's plan right now
			f.vramGrantBytes = 100000;         // ample: the ladder takes no step
			Result r = Admit(f);
			Assert::AreEqual(designQueue - 1, r.queueFrames, L"no S-C step: the queue held now, not DelayBudget's value");
			Assert::AreEqual(0, r.queueFramesDropped);
			Assert::IsTrue(r.aheadLine.find("queue shortened") == std::string::npos);

			f.aheadFeed.queueAheadFrames = 1;   // a real S-C step (the GWC C1 shape) still sets the stepped queue
			f.aheadBytes[kP5MonitorTiles] = 50;
			f.aheadFeed.activeConsumers = { AheadDemand{ kAheadCeiling, "TD" } };
			f.vramGrantBytes = 150;
			f.queueFrames = designQueue;
			r = Admit(f);
			Assert::AreEqual(designQueue - 1, r.queueFrames, L"the first S-C step: 4 -> 3");
			Assert::AreEqual(1, r.queueFramesDropped);
		}

		// C4 s3a MUST 1 (102b LCORE-QW-AHEAD-FEED-CALLER): the admission reads THE window's feed (QueueWindow.h PlanAheadFeed, built by
		// PlanAheadFeedOf). A window aheadComputeLimit that agrees with the law is no drift; one that differs (window 3, need 6, queue 15:
		// the law gives 6) is DRIFT, flagged and named with both values on the Ctrl+Alt+D line (rule 5), and the plan follows the law.
		TEST_METHOD(WindowFeedDriftIsShownWithBothValues)
		{
			AdmissionFacts f = Facts(60.0, 6.0, 1.0);
			f.aheadBytes[kP3AnalysisField] = 100;
			f.queueFrames = 16;
			f.vramGrantBytes = 100000;
			QueueWindow w;
			w.ahead = 15;
			w.aheadCompute = kAheadCeiling;   // what the window derived for the same consumer: agrees with the law
			f.aheadFeed = PlanAheadFeedOf(w, { AheadDemand{ kAheadCeiling, "TD" } });
			Result r = Admit(f);
			Assert::IsFalse(r.aheadFeedDrift, L"window and plan agree");
			Assert::AreEqual(int(kAheadCeiling), r.aheadComputeLimit);
			Assert::AreEqual(std::string("ahead compute ") + std::to_string(kAheadCeiling) + " (by TD)", r.aheadComputeLine);

			f.aheadFeed.aheadComputeLimit = 3;   // the window says 3 for need 6, queue 15
			r = Admit(f);
			Assert::IsTrue(r.aheadFeedDrift);
			Assert::AreEqual(3, r.windowAheadComputeLimit);
			Assert::AreEqual(int(kAheadCeiling), r.aheadComputeLimit, L"the plan follows the law, never a silent pick of the window value");
			Assert::AreEqual(int(kAheadCeiling), r.aheadFrames);
			Assert::IsTrue(r.aheadComputeLine.find("window ahead compute 3 != plan " + std::to_string(kAheadCeiling)) != std::string::npos,
				L"both values on the Ctrl+Alt+D line");

			f.aheadFeed.aheadComputeLimit = 0;   // not fed: no drift check
			r = Admit(f);
			Assert::IsFalse(r.aheadFeedDrift);
		}

		TEST_METHOD(CostMarksFollowControlConfigRule4)
		{
			Assert::AreEqual(std::string(""), std::string(CostMark(1.0, false)));
			Assert::AreEqual(std::string("+"), std::string(CostMark(0.4, true)));
			Assert::AreEqual(std::string("++"), std::string(CostMark(1.5, true)));
			Assert::AreEqual(std::string("+++"), std::string(CostMark(2.5, true)));
		}

		// C4 s3a (owner OD:391, 2026-11-11 07:45: the regulator switches off optional extras only, "never core, never TD"; 115 #47 / #59):
		// the TD products are the TD slice's OWN jobs - admitted with the slice (even unmeasured, even when the slice is short), costed inside
		// it (fill and slice unchanged by their presence), never "not measured" fill and never in the FillOff list (OptionalFillIds).
		TEST_METHOD(TdProductsAreTheSlicesJobsNeverOptionalFill)
		{
			const ProductId td[] = { kTdBlocks, kTdMemory, kTdAnchorPass, kTdAnchorRefs, kTdDelta, kTdMaps, kTdTrailSummary };
			const AdmissionFacts base = Facts(60.0, 6.0, 1.0);
			const Result r0 = Admit(base);
			AdmissionFacts f = base;
			for (ProductId id : td) Put(f, id, 0.5, false);   // unmeasured: an optional fill would be left out as "not measured"
			Result r = Admit(f);
			for (ProductId id : td)
			{
				Assert::IsTrue(r.cls[id] == JobClass::Slice, L"a TD product is Slice class");
				Assert::IsTrue(r.admitted[id], L"admitted with the TD slice");
				Assert::IsFalse(r.notMeasured[id], L"never 'not measured' optional fill");
			}
			Assert::AreEqual(r0.sliceMs, r.sliceMs, 1e-12, L"costed inside the slice, never on top of it");
			Assert::AreEqual(r0.fillMs, r.fillMs, 1e-12, L"never admitted from the fill leftovers");
			Assert::AreEqual(r0.coreMs, r.coreMs, 1e-12);
			Assert::AreEqual(FillOn(base, r0), FillOn(f, r));
			Assert::IsTrue(r.line.find("not measured") == std::string::npos);
			Assert::IsTrue(r.line.find("TD jobs 7 in the TD slice") != std::string::npos);

			// the FillOff list is the admitted optional fills only (P5, P9, P10 of the fixture, in id order): no TD, no core
			const std::vector<ProductId> fills = OptionalFillIds(r);
			Assert::IsTrue(fills == std::vector<ProductId>{ kP5MonitorTiles, kP9CameraSmoothed, kP10GrainSplit });
			for (ProductId id : td)
				Assert::IsTrue(std::find(fills.begin(), fills.end(), id) == fills.end(), L"a TD product is never in FillOff's list");

			// core over the budget: the slice is 0 (TD's reduced tier, shown) and the TD products stay admitted; FillOff's list is empty
			AdmissionFacts over = Facts(60.0, 14.0, 4.0);
			for (ProductId id : td) Put(over, id, 0.5, true);
			r = Admit(over);
			Assert::IsTrue(r.coreOver);
			Assert::AreEqual(0.0, r.sliceMs);
			Assert::IsTrue(r.tdReduced);
			for (ProductId id : td) Assert::IsTrue(r.admitted[id], L"a short slice is TD's own ladder, never a TD product switched off");
			Assert::IsTrue(OptionalFillIds(r).empty());

			// games: optional fill off; the TD products stay TD's (its backward-only tier), never moved to fill
			f.mode = LatencyMode::Games;
			r = Admit(f);
			Assert::AreEqual(0, FillOn(f, r));
			for (ProductId id : td) Assert::IsTrue(r.admitted[id] && r.cls[id] == JobClass::Slice);
			Assert::IsTrue(OptionalFillIds(r).empty());
		}

		// Owner correction 2026-11-15 (C4 s3a), case 1 of 3: TD ON (TD an active consumer, its products demanded) = the TD products are
		// filled and NEVER shed: under FillOff pressure (the arbiter engaged with the admission's optional-fill list, and a cost sweep that
		// turns every optional fill off) each one stays admitted as a Slice job, never in the FillOff list or its line.
		TEST_METHOD(TdOnTdProductsAreFilledAndNeverShedUnderFillOff)
		{
			const ProductId td[] = { kTdBlocks, kTdMemory, kTdAnchorPass, kTdAnchorRefs, kTdDelta, kTdMaps, kTdTrailSummary };
			const Registry reg = SkeletonProducts();
			bool sawAllFillOff = false;
			for (double scale = 1.0; scale <= 8.0; scale *= 1.25)
			{
				AdmissionFacts f = Facts(60.0, 6.0, scale);
				for (ProductId id : td) Put(f, id, 0.5 * scale, true);
				Assert::IsTrue(TdOn(f), L"TD declares its tiers: TD is on");
				const Result r = Admit(f);
				for (ProductId id : td)
				{
					Assert::IsTrue(r.admitted[id], L"TD on: every demanded TD product is filled");
					Assert::IsTrue(r.cls[id] == JobClass::Slice, L"as TD's own slice job");
					Assert::IsTrue(SpareClassOf(r.cls[id]) == SpareClass::Slice, L"run in the slice, never as fill");
				}
				const std::vector<ProductId> fills = OptionalFillIds(r);
				for (ProductId id : td)
					Assert::IsTrue(std::find(fills.begin(), fills.end(), id) == fills.end(), L"never in the FillOff list");
				if (FillOn(f, r) == 0) sawAllFillOff = true;

				// FillOff engaged with exactly the list the wiring hands the arbiter
				std::vector<const char*> names;
				for (ProductId id : fills) names.push_back(reg.Get(id).token);
				OverloadArbiter a;
				a.SetOptionalFills(names.empty() ? nullptr : names.data(), names.size());
				const double period = f.periodMs;
				const SlotBudget over = Load(period, 0.7 * period, 0.4 * period);
				for (double t = 0.0; a.State().step == OverloadStep::None && t <= 5000.0; t += period) a.Step(t, over, false, 2);
				Assert::IsTrue(a.State().step == OverloadStep::FillOff);
				Assert::IsFalse(FillAllowed(a.State()), L"optional fill is off");
				const std::string line = std::string(a.State().line) + " " + a.FillLine();
				for (ProductId id : td)
					Assert::IsTrue(line.find(reg.Get(id).token) == std::string::npos, L"FillOff never names a TD product");
			}
			Assert::IsTrue(sawAllFillOff, L"the sweep reaches the state where every optional fill is off; the TD products stay filled");
		}

		// Owner correction 2026-11-15 (C4 s3a), case 2 of 3: TD OFF and no other active consumer reads a TD product = the active consumers'
		// graph has no TD node (DemandFromGraph), so no TD product is present, filled, costed or listed; nothing is "not measured" either.
		TEST_METHOD(TdOffWithNoOtherConsumerFillsNoTdProduct)
		{
			const ProductId td[] = { kTdBlocks, kTdMemory, kTdAnchorPass, kTdAnchorRefs, kTdDelta, kTdMaps, kTdTrailSummary };
			AdmissionFacts f = Facts(60.0, 6.0, 1.0);
			f.tdMinMs = 0.0;   // TD is not an active consumer
			f.tdWantMs = 0.0;
			Assert::IsFalse(TdOn(f));
			for (ProductId id : td) { f.costMs[id] = 0.1; f.measured[id] = true; }   // measured, so only demand can keep them out
			Graph g;   // the active consumers' graph: the fixture's products, no TD product
			g.valid = true;
			for (size_t i = 0; i < kProductCount; ++i)
				if (f.present[i]) { Node n; n.id = ProductId(i); g.nodes.push_back(n); }
			const AdmissionFacts before = f;
			DemandFromGraph(g, f);
			for (size_t i = 0; i < kProductCount; ++i) Assert::AreEqual(before.present[i], f.present[i], L"present = the graph's nodes");
			const Result r = Admit(f);
			for (ProductId id : td)
			{
				Assert::IsFalse(f.present[id]);
				Assert::IsFalse(r.admitted[id], L"TD off, no consumer: no fill");
				Assert::IsFalse(r.notMeasured[id]);
			}
			Assert::AreEqual(0.0, r.sliceMs, L"no TD slice");
			const std::vector<ProductId> fills = OptionalFillIds(r);
			for (ProductId id : td) Assert::IsTrue(std::find(fills.begin(), fills.end(), id) == fills.end());
			Assert::IsTrue(r.line.find("TD jobs") == std::string::npos);
			Assert::IsTrue(r.line.find("TD off") == std::string::npos);

			// an invalid graph demands nothing at all
			Graph bad = g;
			bad.valid = false;
			AdmissionFacts none = f;
			DemandFromGraph(bad, none);
			for (size_t i = 0; i < kProductCount; ++i) Assert::IsFalse(none.present[i]);
		}

		// Owner correction 2026-11-15 (C4 s3a), case 3 of 3: TD OFF but another active consumer depends on TD products (e.g. the dither
		// need read path on td.maps, the chroma read path on td.delta): the graph carries those nodes, and exactly those are filled for that
		// consumer and never shed (owner decision 2026-11-15 06:58): Derived class with the chain that reads them, costed in core, never
		// optional fill; the TD products nobody reads are not filled; the line names it.
		TEST_METHOD(TdOffWithAnotherActiveConsumerFillsWhatItDemands)
		{
			const ProductId td[] = { kTdBlocks, kTdMemory, kTdAnchorPass, kTdAnchorRefs, kTdDelta, kTdMaps, kTdTrailSummary };
			AdmissionFacts f = Facts(60.0, 6.0, 1.0);
			f.tdMinMs = 0.0;
			f.tdWantMs = 0.0;
			for (ProductId id : td) { f.costMs[id] = 0.1; f.measured[id] = true; }
			const Result r0 = Admit(f);
			Graph g;
			g.valid = true;
			for (size_t i = 0; i < kProductCount; ++i)
				if (f.present[i]) { Node n; n.id = ProductId(i); g.nodes.push_back(n); }
			for (ProductId id : { kTdMaps, kTdDelta }) { Node n; n.id = id; g.nodes.push_back(n); }   // another consumer's reads
			DemandFromGraph(g, f);
			Assert::IsFalse(TdOn(f));
			const Result r = Admit(f);
			for (ProductId id : td)
			{
				const bool demanded = id == kTdMaps || id == kTdDelta;
				Assert::AreEqual(demanded, f.present[id]);
				Assert::AreEqual(demanded, r.admitted[id], demanded ? L"filled for the consumer that depends on it" : L"nobody reads it: no fill");
				Assert::AreEqual(demanded, r.filledForConsumer[id]);
				if (demanded) Assert::IsTrue(r.cls[id] == JobClass::Derived, L"no TD slice: filled for its consumer with the chain, never as fill");
			}
			Assert::AreEqual(0.0, r.sliceMs);
			Assert::AreEqual(r0.coreMs + 0.2, r.coreMs, 1e-9, L"costed with the core chain that reads them");
			Assert::AreEqual(0.2, r.filledForConsumerMs, 1e-9);
			Assert::AreEqual(r0.fillMs, r.fillMs, 1e-9, L"never from the optional fill");
			Assert::AreEqual(FillOn(f, r0), FillOn(f, r));
			const std::vector<ProductId> offFills = OptionalFillIds(r);
			for (ProductId id : td) Assert::IsTrue(std::find(offFills.begin(), offFills.end(), id) == offFills.end(), L"never in FillOff's list");
			Assert::IsTrue(r.line.find("TD off: TD products 2 filled for another consumer (0.20 ms in core, never shed)") != std::string::npos);
			Assert::IsTrue(r.line.find("TD jobs") == std::string::npos);

			// the same demand with TD ON: the slice's own jobs again (case 1), never optional fill
			f.tdMinMs = 1.0;
			f.tdWantMs = 3.0;
			const Result on = Admit(f);
			for (ProductId id : { kTdMaps, kTdDelta })
				Assert::IsTrue(on.admitted[id] && on.cls[id] == JobClass::Slice);
			const std::vector<ProductId> fills = OptionalFillIds(on);
			for (ProductId id : td) Assert::IsTrue(std::find(fills.begin(), fills.end(), id) == fills.end());
		}

		// Owner decision 2026-11-15 06:58 (C4 s3a), (a): TD OFF, TD products another ACTIVE consumer depends on are filled for it and NEVER
		// shed: under a cost sweep that turns every optional fill off, and with the arbiter engaged at FillOff on the admission's own list,
		// each demanded TD product stays admitted (Derived, run with the core chain), never in the list or the FillOff line; an unmeasured
		// one is still filled and shown as "some core not measured"; the TD products nobody reads are not filled.
		TEST_METHOD(TdOffDemandedTdProductsAreNeverShedUnderFillOff)
		{
			const ProductId td[] = { kTdBlocks, kTdMemory, kTdAnchorPass, kTdAnchorRefs, kTdDelta, kTdMaps, kTdTrailSummary };
			const ProductId demandedIds[] = { kTdMaps, kTdDelta };
			const Registry reg = SkeletonProducts();
			bool sawAllFillOff = false;
			for (double scale = 1.0; scale <= 8.0; scale *= 1.25)
			{
				AdmissionFacts f = Facts(60.0, 6.0, scale);
				f.tdMinMs = 0.0;   // TD is not an active consumer
				f.tdWantMs = 0.0;
				for (ProductId id : demandedIds) Put(f, id, 0.5 * scale, true);   // another active consumer's reads (graph nodes)
				Assert::IsFalse(TdOn(f));
				const Result r = Admit(f);
				for (ProductId id : td)
				{
					const bool demanded = id == kTdMaps || id == kTdDelta;
					Assert::AreEqual(demanded, r.admitted[id], demanded ? L"TD off, demanded: filled for its consumer" : L"nobody reads it: no fill");
					if (!demanded) continue;
					Assert::IsTrue(r.filledForConsumer[id]);
					Assert::IsTrue(r.cls[id] == JobClass::Derived, L"never optional fill");
					Assert::IsTrue(SpareClassOf(r.cls[id]) == SpareClass::Core, L"run with the chain in the slot, never as fill");
					Assert::IsFalse(r.notMeasured[id]);
				}
				const std::vector<ProductId> fills = OptionalFillIds(r);
				for (ProductId id : td)
					Assert::IsTrue(std::find(fills.begin(), fills.end(), id) == fills.end(), L"never in the FillOff list");
				if (FillOn(f, r) == 0) sawAllFillOff = true;

				std::vector<const char*> names;
				for (ProductId id : fills) names.push_back(reg.Get(id).token);
				OverloadArbiter a;
				a.SetOptionalFills(names.empty() ? nullptr : names.data(), names.size());
				const double period = f.periodMs;
				const SlotBudget over = Load(period, 0.7 * period, 0.4 * period);
				for (double t = 0.0; a.State().step == OverloadStep::None && t <= 5000.0; t += period) a.Step(t, over, false, 2);
				Assert::IsTrue(a.State().step == OverloadStep::FillOff);
				Assert::IsFalse(FillAllowed(a.State()), L"optional fill is off");
				const std::string line = std::string(a.State().line) + " " + a.FillLine();
				for (ProductId id : td)
					Assert::IsTrue(line.find(reg.Get(id).token) == std::string::npos, L"FillOff never names a TD product");
			}
			Assert::IsTrue(sawAllFillOff, L"the sweep reaches the state where every optional fill is off; the demanded TD products stay filled");

			// unmeasured: an optional fill would be left out as "not measured"; a demanded TD product is filled anyway, and shown
			AdmissionFacts u = Facts(60.0, 6.0, 1.0);
			u.tdMinMs = 0.0;
			u.tdWantMs = 0.0;
			Put(u, kTdMaps, 0.5, false);
			const Result ru = Admit(u);
			Assert::IsTrue(ru.admitted[kTdMaps] && ru.cls[kTdMaps] == JobClass::Derived);
			Assert::IsFalse(ru.notMeasured[kTdMaps]);
			Assert::AreEqual(1u, ru.unmeasuredCore);
			Assert::IsTrue(ru.line.find("some core not measured") != std::string::npos);
			Assert::IsTrue(ru.line.find("TD off: TD products 1 filled for another consumer") != std::string::npos);
		}

		// Owner answer 2026-11-15 via lead 18 (S3A-GAMES), (b) + (c) in games: with TD off, a TD product another active consumer demands is
		// OPTIONAL (Fill class), not Derived, so games (no optional fill) does not make it; the consumer's downgrade is shown on the Fallbacks
		// entry; with no consumer, none is present or filled. Competitive (no slot work at all) admits nothing and shows the same downgrade.
		TEST_METHOD(TdOffGamesDemandedTdProductIsOptionalNotFilled)
		{
			const ProductId td[] = { kTdBlocks, kTdMemory, kTdAnchorPass, kTdAnchorRefs, kTdDelta, kTdMaps, kTdTrailSummary };
			AdmissionFacts f = Facts(60.0, 6.0, 1.0);
			f.tdMinMs = 0.0;
			f.tdWantMs = 0.0;
			f.mode = LatencyMode::Games;
			for (ProductId id : td) { f.costMs[id] = 0.1; f.measured[id] = true; }   // measured, so only demand and mode decide
			f.present[kTdMaps] = true;                                                // another active consumer's read
			const Result r = Admit(f);
			Assert::AreEqual(0, FillOn(f, r), L"games: no optional fill");
			for (ProductId id : td)
			{
				Assert::IsFalse(r.admitted[id], id == kTdMaps ? L"games, TD off, demanded: optional fill, games runs none" : L"no consumer: no fill");
				Assert::IsFalse(r.filledForConsumer[id], L"games: never Derived for the consumer");
			}
			Assert::IsTrue(r.cls[kTdMaps] == JobClass::Fill, L"games: a demanded TD product is optional (Fill), not Derived");
			Assert::AreEqual(1u, r.tdOptionalInGames);
			Assert::IsTrue(OptionalFillIds(r).empty(), L"nothing for FillOff to shed");
			Assert::AreEqual(0.0, r.sliceMs);
			Assert::AreEqual(0.0, r.filledForConsumerMs, 1e-9);
			Assert::IsTrue(r.line.find("fill 0 on / 4 off") != std::string::npos, L"P5, P9, P10 and td.maps: all optional, all off in games");
			Assert::IsTrue(r.line.find("TD off (games): TD products 1 demanded by another consumer are optional fill, not made (games runs no optional fill)") != std::string::npos);
			Assert::IsTrue(r.line.find("filled for another consumer") == std::string::npos);
			Assert::AreEqual(std::string("games (queue 1): TD products 1 demanded by another consumer not made (optional fill; games runs none); the consumer runs without them"),
				r.consumerDowngrade);
			const FallbackCatalog::Decision d = FallbackCatalog::SkeletonAdmission(r.consumerDowngrade);
			Assert::IsTrue(d.engaged, L"the consumer's downgrade is shown, never silent");
			Assert::AreEqual(std::string("skeleton_admission"), d.id);
			Assert::AreEqual(r.consumerDowngrade, d.reason);

			// (c) games, TD off, no consumer reads a TD product: none is present, none is filled, nothing to show
			AdmissionFacts none = f;
			none.present[kTdMaps] = false;
			const Result rn = Admit(none);
			for (ProductId id : td) Assert::IsFalse(rn.admitted[id] || rn.filledForConsumer[id]);
			Assert::AreEqual(r.coreMs, rn.coreMs, 1e-9, L"td.maps was never in core in games");
			Assert::IsTrue(rn.line.find("TD off") == std::string::npos);
			Assert::IsTrue(rn.consumerDowngrade.empty());
			Assert::IsFalse(FallbackCatalog::SkeletonAdmission(rn.consumerDowngrade).engaged);

			// competitive: no slot work at all; the consumer's downgrade is shown
			AdmissionFacts comp = f;
			comp.mode = LatencyMode::Competitive;
			const Result rc = Admit(comp);
			for (size_t i = 0; i < kProductCount; ++i) Assert::IsFalse(rc.admitted[i]);
			Assert::AreEqual(std::string("competitive (latency 0): TD products 1 demanded by another consumer not made (no slot work); the consumer runs without them"),
				rc.consumerDowngrade);
			Assert::IsTrue(FallbackCatalog::SkeletonAdmission(rc.consumerDowngrade).engaged);
		}

		// Owner answer 2026-11-15 via lead 18 (S3A-COMP): competitive makes NO TD products, and the consumer's downgrade is SHOWN through the
		// existing Fallbacks path (FallbackCatalog -> FallbackStatus registry -> "Fallbacks engaged" on Ctrl+I / Config status, plus the one-time
		// notice sentence). TD on: TD itself is the consumer; TD off: another consumer; no TD product demanded: nothing to show.
		TEST_METHOD(CompetitiveTdProductsNotMadeAndShown)
		{
			const ProductId td[] = { kTdBlocks, kTdMemory, kTdAnchorPass, kTdAnchorRefs, kTdDelta, kTdMaps, kTdTrailSummary };
			AdmissionFacts on = Facts(60.0, 6.0, 1.0);
			on.mode = LatencyMode::Competitive;
			for (ProductId id : td) Put(on, id, 0.5, true);
			Assert::IsTrue(TdOn(on));
			const Result r = Admit(on);
			for (size_t i = 0; i < kProductCount; ++i) Assert::IsFalse(r.admitted[i], L"competitive: no slot work");
			Assert::AreEqual(7u, r.tdNotMadeCompetitive);
			const std::string note = "competitive (latency 0): TD products 7 demanded by TD not made (no slot work); the consumer runs without them";
			Assert::AreEqual(note, r.consumerDowngrade);
			Assert::AreEqual(std::string("admission (competitive): no slot work (latency 0); ") + note, r.line);
			FallbackStatus::Registry reg;
			Assert::IsTrue(FallbackCatalog::Sync(reg, FallbackCatalog::SkeletonAdmission(r.consumerDowngrade)));
			Assert::AreEqual(std::string("Fallbacks engaged: skeleton_admission (") + note + ")", reg.Heading());
			Assert::IsFalse(std::string(FallbackPlainText::Sentence("skeleton_admission")).empty(), L"the one-time notice has its sentence");

			// TD off, another consumer demands td.delta only
			AdmissionFacts off = Facts(60.0, 6.0, 1.0);
			off.mode = LatencyMode::Competitive;
			off.tdMinMs = 0.0;
			off.tdWantMs = 0.0;
			Put(off, kTdDelta, 0.5, true);
			const Result ro = Admit(off);
			Assert::IsFalse(ro.admitted[kTdDelta]);
			Assert::AreEqual(std::string("competitive (latency 0): TD products 1 demanded by another consumer not made (no slot work); the consumer runs without them"),
				ro.consumerDowngrade);

			// no TD product demanded: no downgrade, the entry is released, the line is the plain competitive line
			AdmissionFacts plain = Facts(60.0, 6.0, 1.0);
			plain.mode = LatencyMode::Competitive;
			const Result rp = Admit(plain);
			Assert::IsTrue(rp.consumerDowngrade.empty());
			Assert::AreEqual(std::string("admission (competitive): no slot work (latency 0)"), rp.line);
			Assert::IsTrue(FallbackCatalog::Sync(reg, FallbackCatalog::SkeletonAdmission(rp.consumerDowngrade)));
			Assert::AreEqual(std::string("Fallbacks engaged: none"), reg.Heading());
		}

		// Owner answer 2026-11-15 via lead 18 (S3A-STALE), wiring: the graph and the consumer declarations reach the admission through
		// DemandFromGraph(graph, consumers, facts); a TD product read ONLY by an optional fill (td.maps read by P5, td.blocks read by td.maps;
		// the active consumer reads P5) follows that fill: admitted with it, listed for FillOff, and the arbiter's FillOff stops and names it at
		// the SAME step as its reader. A direct consumer read keeps the old rule (Derived, never listed). A reader left out at Configure
		// leaves its followers out too; an unmeasured follower keeps its reader out as "not measured".
		TEST_METHOD(TdOffTdProductStopsWithItsOnlyOptionalReaderAtFillOff)
		{
			const Registry reg = SkeletonProducts();
			const ProductId followers[] = { kTdMaps, kTdBlocks };
			const ProductId stopTogether[] = { kP5MonitorTiles, kTdMaps, kTdBlocks };
			AdmissionFacts f = Facts(60.0, 6.0, 1.0);
			f.tdMinMs = 0.0;   // TD off
			f.tdWantMs = 0.0;
			Put(f, kTdMaps, 0.2, true);
			Put(f, kTdBlocks, 0.1, true);
			Graph g;   // the active consumers' graph: P5 reads td.maps, td.maps reads td.blocks
			g.valid = true;
			for (size_t i = 0; i < kProductCount; ++i)
			{
				if (!f.present[i]) continue;
				Node n;
				n.id = ProductId(i);
				n.declared = ProductId(i);
				if (n.id == kP5MonitorTiles) { n.inputCount = 1; n.inputs[0].product = kTdMaps; }
				if (n.id == kTdMaps) { n.inputCount = 1; n.inputs[0].product = kTdBlocks; }
				g.nodes.push_back(n);
			}
			ConsumerDecl monitor;
			monitor.id = 1;
			monitor.name = "monitor";
			AddConsume(monitor, kP5MonitorTiles);
			const std::vector<ConsumerDecl> consumers{ monitor };
			DemandFromGraph(g, consumers, f);
			Assert::IsTrue(f.readersFed);
			Assert::IsTrue(f.readBy[kTdMaps] == (uint64_t(1) << kP5MonitorTiles), L"td.maps: read by P5 only");
			Assert::IsTrue(f.readBy[kTdBlocks] == (uint64_t(1) << kTdMaps), L"td.blocks: read by td.maps only");
			Assert::IsTrue(f.consumerReads[kP5MonitorTiles]);
			Assert::IsFalse(f.consumerReads[kTdMaps] || f.consumerReads[kTdBlocks]);

			// budget 16.667 - 6 - 1.667 = 9.0; core 1.65; left 7.35; P5 with its followers 0.4 + 0.2 + 0.1 = 0.7 fits; P9 0.05, P10 1.5:
			// fill 0.7 + 0.05 + 1.5 = 2.25 ms, five fills on (P5, P9, P10, td.maps, td.blocks)
			const Result r = Admit(f);
			for (ProductId id : followers)
			{
				Assert::IsTrue(r.followsReaders[id]);
				Assert::IsTrue(r.cls[id] == JobClass::Fill, L"read only by an optional fill: optional with it");
				Assert::IsTrue(r.admitted[id], L"on with its reader");
				Assert::IsFalse(r.filledForConsumer[id]);
			}
			Assert::IsTrue(r.admitted[kP5MonitorTiles]);
			Assert::AreEqual(1.65, r.coreMs, 1e-9, L"the followers are not in core");
			Assert::AreEqual(2.25, r.fillMs, 1e-9);
			Assert::AreEqual(5, FillOn(f, r));
			Assert::AreEqual(2u, r.tdFollowing);
			Assert::AreEqual(2u, r.tdFollowingOn);
			Assert::IsTrue(r.line.find("TD off: TD products 2 read only by optional fills, 2 on with them (they stop when their readers stop)") != std::string::npos);
			const std::vector<ProductId> fills = OptionalFillIds(r);
			for (ProductId id : stopTogether)
				Assert::IsTrue(std::find(fills.begin(), fills.end(), id) != fills.end(), L"in the list FillOff switches off");

			// the arbiter, fed the admission's own list: before FillOff nothing is named; at the step FillOff engages, the reader and both
			// followers are named together (FillAllowed is false for all of them at once)
			std::vector<const char*> names;
			for (ProductId id : fills) names.push_back(reg.Get(id).token);
			OverloadArbiter a;
			a.SetOptionalFills(names.data(), names.size());
			const double period = f.periodMs;
			const SlotBudget over = Load(period, 0.7 * period, 0.4 * period);
			for (double t = 0.0; a.State().step == OverloadStep::None && t <= 5000.0; t += period)
			{
				Assert::IsTrue(a.FillLine().empty());
				a.Step(t, over, false, 2);
			}
			Assert::IsTrue(a.State().step == OverloadStep::FillOff);
			Assert::IsFalse(FillAllowed(a.State()));
			const std::string line = std::string(a.State().line) + " " + a.FillLine();
			for (ProductId id : stopTogether)
				Assert::IsTrue(line.find(reg.Get(id).token) != std::string::npos, L"FillOff stops the reader and its TD products at the same step");

			// a direct consumer read of td.maps: the old rule (Derived, filled for the consumer, never listed); td.blocks then has a
			// non-optional reader (td.maps) and is Derived too
			ConsumerDecl dither;
			dither.id = 2;
			dither.name = "dither";
			AddConsume(dither, kTdMaps);
			const std::vector<ConsumerDecl> both{ monitor, dither };
			AdmissionFacts d = f;
			DemandFromGraph(g, both, d);
			Assert::IsTrue(d.consumerReads[kTdMaps]);
			const Result rd = Admit(d);
			const std::vector<ProductId> dFills = OptionalFillIds(rd);
			for (ProductId id : followers)
			{
				Assert::IsFalse(rd.followsReaders[id]);
				Assert::IsTrue(rd.cls[id] == JobClass::Derived && rd.admitted[id] && rd.filledForConsumer[id]);
				Assert::IsTrue(std::find(dFills.begin(), dFills.end(), id) == dFills.end(), L"a feature reads it: never in FillOff's list");
			}

			// the reader does not fit at Configure (8.0 + 0.3 > 7.35): it and its followers are left out together
			AdmissionFacts big = f;
			big.costMs[kP5MonitorTiles] = 8.0;
			const Result rb = Admit(big);
			Assert::IsFalse(rb.admitted[kP5MonitorTiles] || rb.admitted[kTdMaps] || rb.admitted[kTdBlocks]);
			Assert::AreEqual(0u, rb.tdFollowingOn);
			Assert::IsTrue(rb.line.find("TD off: TD products 2 read only by optional fills, 0 on with them") != std::string::npos);

			// an unmeasured follower keeps its reader out; both are shown as not measured
			AdmissionFacts u = f;
			u.measured[kTdMaps] = false;
			const Result ru = Admit(u);
			Assert::IsFalse(ru.admitted[kP5MonitorTiles] || ru.admitted[kTdMaps] || ru.admitted[kTdBlocks]);
			Assert::IsTrue(ru.notMeasured[kP5MonitorTiles] && ru.notMeasured[kTdMaps]);
			Assert::IsTrue(ru.line.find("not measured 1") != std::string::npos);

			// readers not fed: never a follower, the 6aa02f66d rule holds (Derived, filled for the consumer)
			AdmissionFacts nf = f;
			nf.readersFed = false;
			const Result rnf = Admit(nf);
			for (ProductId id : followers)
				Assert::IsTrue(!rnf.followsReaders[id] && rnf.cls[id] == JobClass::Derived && rnf.filledForConsumer[id]);
		}
	};

	TEST_CLASS(OverloadOrderTests)
	{
	public:
		// S4-T1: under sustained overload the steps come in the fixed order (TD before extras) across a rate x size x profile matrix.
		TEST_METHOD(OrderHoldsAcrossMatrix)
		{
			const double rates[] = { 24.0, 25.0, 30.0, 50.0, 60.0 };
			const double sizes[] = { 1.0, 4.0 };
			const uint8_t tiers[] = { 0, 2 };
			for (double hz : rates)
				for (double sz : sizes)
					for (uint8_t tier : tiers)
					{
						OverloadArbiter a;
						const double period = 1000.0 / hz;
						std::vector<OverloadStep> seen;
						int tdSteps = 0;
						uint8_t lastTier = tier;
						for (double t = 0.0; t <= 20000.0; t += period)
						{
							const OverloadState s = a.Step(t, Load(period, 0.7 * period * sz / sz, 0.4 * period), true, tier);
							if (seen.empty() || seen.back() != s.step) seen.push_back(s.step);
							if (s.tdTier < lastTier) ++tdSteps;
							lastTier = s.tdTier;
						}
						for (size_t i = 1; i < seen.size(); ++i)
							Assert::IsTrue(uint8_t(seen[i]) > uint8_t(seen[i - 1]), L"never out of order under sustained overload");
						Assert::IsTrue(seen.back() == OverloadStep::FeatureOff);
						const bool hasTd = std::find(seen.begin(), seen.end(), OverloadStep::TdStepDown) != seen.end();
						Assert::AreEqual(tier > 0, hasTd, L"the TD stage applies only when TD runs");
						Assert::AreEqual(int(tier), tdSteps, L"TD down one tier per stage to its lowest");
					}
		}

		// S4-T2 (CPU part): the slot never ends after the next vsync when costs are measured: used <= budget on every frame.
		TEST_METHOD(SlotNeverOverrunsTheBudgetWhenMeasured)
		{
			const double rates[] = { 24.0, 30.0, 50.0, 60.0 };
			for (double hz : rates)
			{
				const double period = 1000.0 / hz;
				RenderBudget rb;
				SpareHost host;
				FunctionSpareWork core(kP3AnalysisField, SpareClass::Core, 1), fill(kP5MonitorTiles, SpareClass::Fill, 2);
				const double coreMs = 0.2 * period, fillMs = 0.3 * period;
				core.hasWork = [] { return true; };
				fill.hasWork = [] { return true; };
				core.run = [&](uint32_t) { return coreMs; };
				fill.run = [&](uint32_t) { return fillMs; };
				host.Slot().Register(&core);
				host.Slot().Register(&fill);
				int presents = 0;
				for (uint64_t seq = 1; seq <= 200; ++seq)
				{
					const double render = (seq % 7 == 0 ? 0.6 : 0.45) * period;
					rb.Note(render);
					host.NotePresented(seq, false);
					host.SetBudget(rb.BudgetMs(hz));
					const SpareReport r = host.Fire(SpareTrigger::PostPresentNoExtras);
					++presents;
					if (seq > 3)
						Assert::IsTrue(r.usedMs <= r.budgetMs + 1e-9, L"slot work ends before the next vsync's margin");
				}
				Assert::AreEqual(200, presents, L"one present per frame: the slot never skips or repeats one");
			}
		}

		// S4-T3: one controller per signal: the extras gate is a function of the arbiter's state only.
		TEST_METHOD(OneControllerPerSignal)
		{
			OverloadState s = {};
			const OverloadStep steps[] = { OverloadStep::None, OverloadStep::FillOff, OverloadStep::TdStepDown, OverloadStep::ExtrasStepDown,
				OverloadStep::CoreDeferred, OverloadStep::FeatureOff };
			for (OverloadStep st : steps)
			{
				s.step = st;
				Assert::AreEqual(uint8_t(st) < uint8_t(OverloadStep::ExtrasStepDown), ExtrasAllowed(s));
				Assert::AreEqual(st == OverloadStep::None, FillAllowed(s));
			}
		}

		// S4-T4: down after 2 s over, up after 5 s under u - 0.10; the band in between holds and restarts both timers.
		TEST_METHOD(HysteresisTimes)
		{
			OverloadArbiter a;
			const double p = 40.0;
			const SlotBudget over = Load(p, 30.0, 10.0), under = Load(p, 20.0, 5.0), band = Load(p, 30.0, 4.0);
			Assert::IsTrue(a.Step(0.0, over, false, 0).step == OverloadStep::None);
			Assert::IsTrue(a.Step(1999.0, over, false, 0).step == OverloadStep::None);
			Assert::IsTrue(a.Step(2000.0, over, false, 0).step == OverloadStep::FillOff);
			Assert::IsTrue(a.Step(2100.0, band, false, 0).step == OverloadStep::FillOff, L"u in [0.9, 1]: hold");
			Assert::IsTrue(a.Step(3000.0, under, false, 0).step == OverloadStep::FillOff);
			Assert::IsTrue(a.Step(7999.0, under, false, 0).step == OverloadStep::FillOff);
			Assert::IsTrue(a.Step(8000.0, under, false, 0).step == OverloadStep::None);
			Assert::AreEqual(1ull, a.StepsDown());
			Assert::AreEqual(1ull, a.StepsUp());
			// unmeasured load never claims an overload
			SlotBudget none = over;
			none.measured = false;
			OverloadArbiter b;
			Assert::IsTrue(b.Step(0.0, none, true, 2).step == OverloadStep::None);
			Assert::IsTrue(b.Step(10000.0, none, true, 2).step == OverloadStep::None);
		}

		// S4-T5 (pure part): every state has its own plain line.
		TEST_METHOD(EveryStepVisible)
		{
			std::set<std::string> lines;
			for (int i = 0; i <= int(OverloadStep::FeatureOff); ++i)
			{
				const std::string l = OverloadStepLine(OverloadStep(i));
				Assert::IsTrue(l.find("overload: ") == 0);
				Assert::IsTrue(l.find('?') == std::string::npos);
				lines.insert(l);
			}
			Assert::AreEqual(size_t(6), lines.size());
			OverloadArbiter a;
			Assert::IsNotNull(a.State().line);
		}

		// Lead 13 ruling (2026-11-14): FillOff is the optional fills' step (the grain split included while it is optional fill) and it is
		// VISIBLE by name: the Ctrl+I line names each fill turned off, and the one-time notice fires once per engagement.
		TEST_METHOD(FillOffNamesEachFillAndNoticesOnce)
		{
			const double period = 1000.0 / 60.0;
			OverloadArbiter a;
			const char* fills[] = { "grain split (shared)", "monitor tiles" };
			a.SetOptionalFills(fills, 2);
			std::string notice;
			Assert::IsFalse(a.TakeFillNotice(&notice), L"no notice without overload");
			Assert::IsTrue(a.FillLine().empty());

			const SlotBudget over = Load(period, 0.7 * period, 0.4 * period);
			double t = 0.0;
			for (; a.State().step == OverloadStep::None && t <= 5000.0; t += period) a.Step(t, over, false, 0);
			Assert::IsTrue(a.State().step == OverloadStep::FillOff);
			const std::string line = a.State().line;
			Assert::AreEqual(std::string("overload: optional fill off: grain split (shared), monitor tiles"), line);
			Assert::IsTrue(a.TakeFillNotice(&notice), L"the one-time notice fires on FillOff");
			Assert::AreEqual(std::string("optional fill off: grain split (shared), monitor tiles"), notice);
			Assert::IsFalse(a.TakeFillNotice(&notice), L"once per engagement");

			for (; a.State().step == OverloadStep::FillOff && t <= 20000.0; t += period) a.Step(t, over, false, 0);
			Assert::IsTrue(uint8_t(a.State().step) > uint8_t(OverloadStep::FillOff));
			Assert::IsTrue(a.FillLine().find("grain split (shared)") != std::string::npos, L"deeper stages keep the fills-off list");

			const SlotBudget none = Load(period, 0.2 * period, 0.0);
			for (; a.State().step != OverloadStep::None && t <= 200000.0; t += period) a.Step(t, none, false, 0);
			Assert::IsTrue(a.State().step == OverloadStep::None);
			Assert::IsTrue(a.FillLine().empty());
			for (; a.State().step == OverloadStep::None && t <= 210000.0; t += period) a.Step(t, over, false, 0);
			Assert::IsTrue(a.TakeFillNotice(&notice), L"re-armed for a new engagement");
		}

		// Recovery returns in reverse order, TD tiers one per stage; the extras stage is left when extras stop.
		TEST_METHOD(RecoveryIsReverseOrder)
		{
			OverloadArbiter a;
			const double p = 40.0;
			const SlotBudget over = Load(p, 30.0, 10.0), under = Load(p, 10.0, 2.0);
			double t = 0.0;
			for (; t <= 12000.0; t += 100.0) a.Step(t, over, true, 2);
			Assert::IsTrue(a.State().step == OverloadStep::FeatureOff);
			std::vector<OverloadStep> back;
			for (double u = t; u <= t + 60000.0; u += 100.0)
			{
				const OverloadState s = a.Step(u, under, true, 2);
				if (back.empty() || back.back() != s.step) back.push_back(s.step);
			}
			for (size_t i = 1; i < back.size(); ++i) Assert::IsTrue(uint8_t(back[i]) < uint8_t(back[i - 1]));
			Assert::IsTrue(back.back() == OverloadStep::None);
			Assert::AreEqual(uint8_t(2), a.State().tdTier, L"TD back at its full tier");
		}
	};
}
