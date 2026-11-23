/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Proving unit tests for ChainJointLatch (SID Stage S1, Owner-approved OD-1 & OD-2, R1/M1/M2/H3).
// CPU only, synthetic only, deterministic.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainJointLatch.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace JointLatchFx
	{
		using namespace SourceIdentity::Chain;

		constexpr int kRasterW = 3840;
		constexpr int kRasterH = 2160;
		constexpr double kC1 = 15.0;

		inline JointLatchSettings Enabled(JointHoldPolicy hold = JointHoldPolicy::G1_ChainOnlyHardBound,
			JointEntryPolicy entry = JointEntryPolicy::ChainOrJoint)
		{
			JointLatchSettings s;
			s.enabled = true;
			s.entryPolicy = entry;
			s.holdPolicy = hold;
			return s;
		}

		inline JointLatchInput BaseInput(int64_t sec)
		{
			JointLatchInput in;
			in.timeUs = sec * 1000000;
			in.c1 = kC1;
			in.rasterW = kRasterW;
			in.rasterH = kRasterH;
			return in;
		}

		inline JointLatchInput Joint1080(int64_t sec, double dwellSec, double shotCredit, int64_t newestCreditSec = -1)
		{
			JointLatchInput in = BaseInput(sec);
			in.classicalValid = true;
			in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
			in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
			in.currentShotDwellSec = dwellSec;
			in.currentShotCreditX = shotCredit;
			in.currentShotCreditY = shotCredit;
			in.currentShotNewestCreditUs = (newestCreditSec >= 0 ? newestCreditSec : sec) * 1000000;
			in.chainInformative = true;
			return in;
		}

		inline JointLatchInput ChainOnly1080(int64_t sec, double llr, int shots)
		{
			JointLatchInput in = BaseInput(sec);
			in.haveChainPair = true;
			in.chainNx = 1920; in.chainNxDenom = kRasterW;
			in.chainNy = 1080; in.chainNyDenom = kRasterH;
			in.chainLlrX = llr;
			in.chainLlrY = llr;
			in.chainShots = shots;
			in.chainInformative = true;
			return in;
		}
	}

	TEST_CLASS(ChainJointLatchTests)
	{
	public:
		TEST_METHOD(DefaultSettingsAreOffWithApprovedOd1AndOd2Policies)
		{
			using namespace JointLatchFx;
			ChainJointLatch latch;
			Assert::IsFalse(latch.Settings().enabled);
			Assert::IsTrue(latch.Settings().entryPolicy == JointEntryPolicy::ChainOrJoint);
			Assert::IsTrue(latch.Settings().holdPolicy == JointHoldPolicy::G1_ChainOnlyHardBound);

			const auto o = latch.Step(Joint1080(5, 4.0, 10.0));
			Assert::IsFalse(o.latched);
			Assert::IsTrue(o.reason == JointLatchReason::Disabled);
			Assert::IsTrue(latch.Reset().reason == JointLatchReason::Disabled);
		}

		TEST_METHOD(JointEntryLatchesInOneShotPastDwellAndGrowsJOverStableTime)
		{
			using namespace JointLatchFx;
			ChainJointLatch latch(Enabled());

			// Start stream at t = 0: j(0) == 0.5, j(30) == 0.5, j(90) == 0.75, j(150) == 1.0, j(200) == 1.0.
			latch.Step(BaseInput(0));
			Assert::IsTrue(std::fabs(latch.JointFraction(0) - 0.5) < 1e-9);
			Assert::IsTrue(std::fabs(latch.JointFraction(30 * 1000000) - 0.5) < 1e-9);
			Assert::IsTrue(std::fabs(latch.JointFraction(90 * 1000000) - 0.75) < 1e-9);
			Assert::IsTrue(std::fabs(latch.JointFraction(150 * 1000000) - 1.0) < 1e-9);
			Assert::IsTrue(std::fabs(latch.JointFraction(200 * 1000000) - 1.0) < 1e-9);

			// Before 3.0 s dwell: cannot enter joint (counted as would-be classical only).
			const auto oShort = latch.Step(Joint1080(2, 2.5, 0.6 * kC1));
			Assert::IsFalse(oShort.latched);
			Assert::IsTrue(oShort.classicalOnlyBlocked);

			// Past 3.0 s dwell with B current-shot credit >= 0.5 * c1: enters in 1 shot!
			const auto oEnter = latch.Step(Joint1080(4, 3.0, 0.5 * kC1));
			Assert::IsTrue(oEnter.latched);
			Assert::IsTrue(oEnter.event == JointLatchEvent::EnteredJoint);
			Assert::IsTrue(oEnter.entryPath == JointLatchEntryPath::Joint);
			Assert::AreEqual(1920, oEnter.nx);
			Assert::AreEqual(1080, oEnter.ny);
			Assert::AreEqual(uint64_t(1), latch.Counters().entriesJoint);
			Assert::AreEqual(uint64_t(0), latch.Counters().entriesChainAlone);

			// Verify grown j(t) after long stable stream: at t = 150 s (stable 150 s), 0.75 * c1 is NOT enough; 1.0 * c1 IS.
			ChainJointLatch longLatch(Enabled());
			longLatch.Step(BaseInput(0));
			const auto oBelowGrown = longLatch.Step(Joint1080(150, 4.0, 0.75 * kC1));
			Assert::IsFalse(oBelowGrown.latched);
			Assert::IsTrue(oBelowGrown.classicalOnlyBlocked);

			const auto oAtGrown = longLatch.Step(Joint1080(151, 5.0, 1.0 * kC1));
			Assert::IsTrue(oAtGrown.latched);
			Assert::IsTrue(oAtGrown.event == JointLatchEvent::EnteredJoint);
		}

		TEST_METHOD(ChainAloneFullPathLatchesInTwoShotsWhenClassicalIsSilent)
		{
			using namespace JointLatchFx;
			ChainJointLatch latch(Enabled());

			// 1 shot is never enough for chain-alone (even with high LLR): held back visibly.
			const auto o1 = latch.Step(ChainOnly1080(5, kC1 + 5.0, 1));
			Assert::IsFalse(o1.latched);
			Assert::IsTrue(o1.heldBack);
			Assert::IsTrue(o1.reason == JointLatchReason::HeldBackByBar);

			// 2 shots at >= Bar(t, c1) enters cleanly even though classical A is completely blind/absent!
			const auto o2 = latch.Step(ChainOnly1080(10, kC1, 2));
			Assert::IsTrue(o2.latched);
			Assert::IsTrue(o2.event == JointLatchEvent::EnteredChainAlone);
			Assert::IsTrue(o2.entryPath == JointLatchEntryPath::ChainAlone);
			Assert::AreEqual(1920, o2.nx);
			Assert::AreEqual(1080, o2.ny);
			Assert::AreEqual(uint64_t(1), latch.Counters().entriesChainAlone);
			Assert::AreEqual(uint64_t(0), latch.Counters().entriesJoint);
		}

		TEST_METHOD(ClassicalAloneNeverEntersAndLogsWouldBeClassicalOnly)
		{
			using namespace JointLatchFx;
			ChainJointLatch latch(Enabled());

			// Feed 60 seconds of strong classical 1920x1080 readings with zero chain corroboration.
			for (int s = 0; s < 60; ++s)
			{
				JointLatchInput in = BaseInput(s);
				in.classicalValid = true;
				in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
				in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
				in.currentShotDwellSec = 10.0;
				in.currentShotCreditX = 0.0;
				in.currentShotCreditY = 0.0;
				const auto o = latch.Step(in);
				Assert::IsFalse(o.latched);
				Assert::IsTrue(o.classicalOnlyBlocked);
				Assert::IsTrue(o.reason == JointLatchReason::WouldBeClassicalOnly);
				Assert::AreEqual(std::string("would-be (classical only)"), std::string(JointLatchReasonText(o.reason)));
			}

			Assert::IsFalse(latch.Latched());
			Assert::AreEqual(uint64_t(60), latch.Counters().wouldBeClassicalOnly);
			Assert::AreEqual(uint64_t(0), latch.Counters().entriesJoint);
			Assert::AreEqual(uint64_t(0), latch.Counters().entriesChainAlone);
		}

		TEST_METHOD(ParkedAnamorphicAndNonConventionalPairsNeverEnter)
		{
			using namespace JointLatchFx;
			ChainJointLatch latch(Enabled());

			// (a) Parked anamorphic HDV 1440x1080 (listed with apply = false) via Joint & Chain-alone.
			{
				JointLatchInput in = BaseInput(5);
				in.classicalValid = true;
				in.classicalNx = 1440; in.classicalNxDenom = kRasterW;
				in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
				in.currentShotDwellSec = 5.0;
				in.currentShotCreditX = 2.0 * kC1;
				in.currentShotCreditY = 2.0 * kC1;
				in.haveChainPair = true;
				in.chainNx = 1440; in.chainNxDenom = kRasterW;
				in.chainNy = 1080; in.chainNyDenom = kRasterH;
				in.chainLlrX = 2.0 * kC1;
				in.chainLlrY = 2.0 * kC1;
				in.chainShots = 3;
				const auto o = latch.Step(in);
				Assert::IsFalse(o.latched);
				Assert::IsTrue(o.nonConventional);
				Assert::IsTrue(o.reason == JointLatchReason::NonConventionalPair);
				Assert::IsTrue((o.reasonBits & kReasonNonConventional) != 0u);
			}

			// (b) Non-conventional unlisted 1600x900.
			{
				JointLatchInput in = BaseInput(10);
				in.classicalValid = true;
				in.classicalNx = 1600; in.classicalNxDenom = kRasterW;
				in.classicalNy = 900; in.classicalNyDenom = kRasterH;
				in.currentShotDwellSec = 5.0;
				in.currentShotCreditX = 2.0 * kC1;
				in.currentShotCreditY = 2.0 * kC1;
				const auto o = latch.Step(in);
				Assert::IsFalse(o.latched);
				Assert::IsTrue(o.nonConventional);
			}

			Assert::AreEqual(uint64_t(2), latch.Counters().nonConventional);
			Assert::AreEqual(uint64_t(0), latch.Counters().entriesJoint);
			Assert::AreEqual(uint64_t(0), latch.Counters().entriesChainAlone);
		}

		TEST_METHOD(H3HardBoundReleasesWithin180sUnderBothG1AndG2DespitePersistentClassicalMisreading)
		{
			using namespace JointLatchFx;

			for (int policyIdx = 0; policyIdx < 2; ++policyIdx)
			{
				const JointHoldPolicy policy = policyIdx == 0
					? JointHoldPolicy::G1_ChainOnlyHardBound
					: JointHoldPolicy::G2_JointOncePerBound;

				// Case A: Cut-free native stream with injected wrong latch at t = 0 + persistent classical misreading,
				// even with an adversarial cut at t = 179 s trying to stretch dwell past 180 s!
				ChainJointLatch latch(Enabled(policy));
				const auto oEnter = latch.Step(ChainOnly1080(0, kC1 + 1.0, 2));
				Assert::IsTrue(oEnter.latched);
				const int64_t anchorUs = latch.LastConfirmUs();
				Assert::AreEqual(int64_t(0), anchorUs);

				int64_t releasedAtSec = -1;
				for (int s = 1; s <= 200; ++s)
				{
					JointLatchInput in = BaseInput(s);
					// Persistent classical A misreading 1920x1080 every single second!
					in.classicalValid = true;
					in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
					in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
					// Chain B is uninformative (dark/flat, worst-case for G1/G2 because 60 s clock is paused by A!).
					in.chainInformative = false;
					in.gainX = 0.0; in.gainY = 0.0;
					in.rawX = 0.0; in.rawY = 0.0;
					// Adversarial late cut at t = 179 s must NOT push release past 180 s.
					in.shotBoundary = (s == 179);

					const auto o = latch.Step(in);
					// Classical A must NEVER move LastConfirmUs()!
					Assert::AreEqual(anchorUs, latch.LastConfirmUs());
					if (!o.latched)
					{
						releasedAtSec = s;
						Assert::IsTrue(o.event == JointLatchEvent::ReleasedCeiling);
						break;
					}
				}
				Assert::IsTrue(releasedAtSec > 0);
				Assert::IsTrue(releasedAtSec <= 180);
				Assert::IsTrue(latch.Counters().classicalHoldPauses > 0);

				// Case B: H3 no-A-assist rule while B is informative and non-crediting -> 60 s clock still advances!
				ChainJointLatch latchB(Enabled(policy));
				latchB.Step(ChainOnly1080(0, kC1 + 1.0, 2));
				Assert::IsTrue(latchB.Latched());

				int64_t deferredAtSec = -1;
				for (int s = 1; s <= 70; ++s)
				{
					JointLatchInput in = BaseInput(s);
					in.classicalValid = true;
					in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
					in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
					// B is informative and non-crediting (rawX == rawY == 0).
					in.chainInformative = true;
					const auto o = latchB.Step(in);
					if (o.deferredNow)
					{
						deferredAtSec = s;
						break;
					}
				}
				Assert::IsTrue(deferredAtSec > 0 && deferredAtSec <= 60);
				Assert::IsTrue(latchB.Counters().classicalHoldDeniedByB >= 60);
			}
		}

		TEST_METHOD(M1DetailAboveDebouncesTransientAndReleasesAfterSustainedDwell)
		{
			using namespace JointLatchFx;
			ChainJointLatch latch(Enabled());

			latch.Step(ChainOnly1080(0, kC1 + 1.0, 2));
			Assert::IsTrue(latch.Latched());

			// Single-frame detail_above at t = 5 s: ignored (debounced), latch stays held.
			{
				JointLatchInput in = BaseInput(5);
				in.classicalDetailAbove = true;
				const auto o = latch.Step(in);
				Assert::IsTrue(o.latched);
				Assert::IsTrue(o.reason == JointLatchReason::DetailAboveTransient);
			}
			// Clears at t = 6 s.
			latch.Step(BaseInput(6));
			Assert::IsTrue(latch.Latched());

			// Sustained detail_above starting at t = 10 s: held at 10, 11, 12 s; releases at t = 13 s (3.0 s dwell).
			for (int s = 10; s <= 12; ++s)
			{
				JointLatchInput in = BaseInput(s);
				in.classicalDetailAbove = true;
				const auto o = latch.Step(in);
				Assert::IsTrue(o.latched);
			}
			{
				JointLatchInput in = BaseInput(13);
				in.classicalDetailAbove = true;
				const auto o = latch.Step(in);
				Assert::IsFalse(o.latched);
				Assert::IsTrue(o.event == JointLatchEvent::ReleasedDetailAbove);
				Assert::IsTrue(o.reason == JointLatchReason::DetailAboveSustained);
				Assert::AreEqual(uint64_t(1), latch.Counters().releasesDetailAbove);
				Assert::IsTrue(latch.Counters().detailAboveTransientIgnored >= 4);
			}
		}

		TEST_METHOD(M2ClassicalVoidedDuringMenuMixedSubtitleAndFade)
		{
			using namespace JointLatchFx;

			// (a) Cannot enter Joint while mixed / subtitle / fade is active (A is void).
			{
				ChainJointLatch latch(Enabled());
				for (int mode = 0; mode < 3; ++mode)
				{
					JointLatchInput in = Joint1080(5 + mode, 5.0, 0.8 * kC1);
					in.mixedContent = (mode == 0);
					in.subtitleOverlay = (mode == 1);
					in.inFade = (mode == 2);
					const auto o = latch.Step(in);
					Assert::IsFalse(o.latched);
					Assert::IsTrue(o.classicalVoided);
				}
				Assert::AreEqual(uint64_t(3), latch.Counters().classicalVoided);
			}

			// (b) While latched, A during subtitle/fade neither pauses starvation clock nor triggers detail_above.
			{
				ChainJointLatch latch(Enabled());
				latch.Step(ChainOnly1080(0, kC1 + 1.0, 2));
				for (int s = 1; s <= 5; ++s)
				{
					JointLatchInput in = BaseInput(s);
					in.subtitleOverlay = true;
					in.classicalValid = true;
					in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
					in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
					in.classicalDetailAbove = true;
					const auto o = latch.Step(in);
					Assert::IsTrue(o.latched); // voided detail_above did NOT release even past 3 s!
					Assert::IsTrue(o.classicalVoided);
				}
				Assert::AreEqual(uint64_t(0), latch.Counters().classicalHoldPauses);
				Assert::IsTrue(latch.ReleaseClockSec() > 0.0);

				// MenuHold triggers immediate R7 release!
				JointLatchInput menuIn = BaseInput(6);
				menuIn.menuHold = true;
				const auto oMenu = latch.Step(menuIn);
				Assert::IsFalse(oMenu.latched);
				Assert::IsTrue(oMenu.event == JointLatchEvent::ReleasedMenu);
				Assert::IsTrue((oMenu.reasonBits & kReasonMenuHold) != 0u);
				Assert::AreEqual(uint64_t(1), latch.Counters().releasesMenu);
			}
		}

		TEST_METHOD(R1FundamentalHarmonicOrderSelectionPicksOneThirdOverTwoThirds)
		{
			using namespace JointLatchFx;

			// Case 1: Synthetic 720p->2160p (1/3 = 1280x720) where BOTH 1/3 and its 2x harmonic 2/3 (2560x1440) dip.
			// Even if 2/3 has higher B credit, 1/3 is confirmed by A roll-off (no detail_above) + B credit >= minCredit.
			{
				std::vector<HarmonicCandidate> cands;
				HarmonicCandidate c720;
				c720.nx = 1280; c720.Nx = kRasterW; c720.ny = 720; c720.Ny = kRasterH;
				c720.creditX = 8.0; c720.creditY = 8.0;
				c720.classicalRollOffConfirmed = true;
				c720.detailAbove = false;

				HarmonicCandidate c1440;
				c1440.nx = 2560; c1440.Nx = kRasterW; c1440.ny = 1440; c1440.Ny = kRasterH;
				c1440.creditX = 11.0; c1440.creditY = 11.0;
				c1440.classicalRollOffConfirmed = false;
				c1440.detailAbove = false;

				cands.push_back(c1440);
				cands.push_back(c720);

				const auto res = SelectFundamentalHarmonic(cands, kRasterW, kRasterH, 7.5);
				Assert::IsTrue(res.found);
				Assert::AreEqual(1280, res.nx);
				Assert::AreEqual(720, res.ny);
				Assert::IsTrue(res.reason == HarmonicSelectionReason::SelectedFundamentalConfirmed);
				Assert::AreEqual(1, res.rejectedHarmonicCount);
			}

			// Case 2: Genuine 1080p->2160p (1/2) where sub-harmonic 540p (1/4) dips slightly BUT has real detail_above!
			// Must reject 540p and select 1920x1080.
			{
				std::vector<HarmonicCandidate> cands;
				HarmonicCandidate c540;
				c540.nx = 960; c540.Nx = kRasterW; c540.ny = 540; c540.Ny = kRasterH;
				c540.creditX = 8.0; c540.creditY = 8.0;
				c540.classicalRollOffConfirmed = true;
				c540.detailAbove = true; // real 1080p energy above 540p Nyquist!

				HarmonicCandidate c1080;
				c1080.nx = 1920; c1080.Nx = kRasterW; c1080.ny = 1080; c1080.Ny = kRasterH;
				c1080.creditX = 12.0; c1080.creditY = 12.0;
				c1080.classicalRollOffConfirmed = true;
				c1080.detailAbove = false;

				cands.push_back(c540);
				cands.push_back(c1080);

				const auto res = SelectFundamentalHarmonic(cands, kRasterW, kRasterH, 7.5);
				Assert::IsTrue(res.found);
				Assert::AreEqual(1920, res.nx);
				Assert::AreEqual(1080, res.ny);
				Assert::IsTrue(res.reason == HarmonicSelectionReason::RejectedSubHarmonicDetailAbove);
			}
		}

		TEST_METHOD(G2RenewsOnlyOnFreshJointReproofAfterBoundAtMostOncePerBound)
		{
			using namespace JointLatchFx;

			// Under G2: enter at t = 0.
			ChainJointLatch g2(Enabled(JointHoldPolicy::G2_JointOncePerBound));
			g2.Step(ChainOnly1080(0, kC1 + 1.0, 2));
			Assert::IsTrue(g2.Latched());

			// Hold quietly until t = 119 s (classical pauses 60 s clock, B silent).
			for (int s = 1; s < 120; ++s)
			{
				JointLatchInput in = BaseInput(s);
				in.classicalValid = true;
				in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
				in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
				g2.Step(in);
			}
			Assert::IsTrue(g2.Latched());
			Assert::IsFalse(g2.G2RenewedThisBound());

			// At t = 120 s: fresh joint re-proof (A agrees + B dwell >= 3 s + credit >= j(120)*c1 + credit time > 0) renews ONCE!
			{
				JointLatchInput reproof = Joint1080(120, 4.0, kC1, 120);
				const auto oRenew = g2.Step(reproof);
				Assert::IsTrue(oRenew.latched);
				Assert::IsTrue(oRenew.g2Renewed);
				Assert::IsFalse(oRenew.deferred);
				Assert::IsTrue(oRenew.reason == JointLatchReason::G2JointRenewal);
				Assert::AreEqual(int64_t(120 * 1000000), g2.LastConfirmUs());
				Assert::IsTrue(g2.G2RenewedThisBound());
				Assert::AreEqual(uint64_t(1), g2.Counters().g2JointRenewals);
			}

			// At next 120 s bound (t = 240 s) WITHOUT any Chain-alone confirmation: G2 CANNOT renew a second time!
			for (int s = 121; s < 240; ++s)
			{
				JointLatchInput in = BaseInput(s);
				in.classicalValid = true;
				in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
				in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
				g2.Step(in);
			}
			{
				JointLatchInput secondReproof = Joint1080(240, 4.0, kC1, 240);
				const auto oExh = g2.Step(secondReproof);
				Assert::IsFalse(oExh.g2Renewed);
				Assert::IsTrue(oExh.deferredNow);
				Assert::IsTrue(oExh.reason == JointLatchReason::G2RenewalExhausted);
				Assert::AreEqual(uint64_t(1), g2.Counters().g2JointRenewals);
			}

			// Under G1 (default): the exact same fresh joint re-proof at t = 120 s does NOT renew!
			ChainJointLatch g1(Enabled(JointHoldPolicy::G1_ChainOnlyHardBound));
			g1.Step(ChainOnly1080(0, kC1 + 1.0, 2));
			for (int s = 1; s < 120; ++s)
			{
				JointLatchInput in = BaseInput(s);
				in.classicalValid = true;
				in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
				in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
				g1.Step(in);
			}
			const auto oG1 = g1.Step(Joint1080(120, 4.0, kC1, 120));
			Assert::IsFalse(oG1.g2Renewed);
			Assert::IsTrue(oG1.deferredNow);
			Assert::AreEqual(uint64_t(0), g1.Counters().g2JointRenewals);
		}

		TEST_METHOD(PendingStepMarkBlocksEntryAndMoveAndAbrImmediateReleases)
		{
			using namespace JointLatchFx;
			ChainJointLatch latch(Enabled());

			// Pending ABR step mark blocks Joint & Chain-alone entry visibly.
			JointLatchInput blocked = Joint1080(5, 4.0, kC1);
			blocked.stepMarkPending = true;
			const auto oBlk = latch.Step(blocked);
			Assert::IsFalse(oBlk.latched);
			Assert::IsTrue(oBlk.heldBack);
			Assert::IsTrue(oBlk.reason == JointLatchReason::BlockedByStepMark);
			Assert::AreEqual(uint64_t(1), latch.Counters().blockedByStepMark);

			// Once mark clears, entry succeeds.
			blocked.stepMarkPending = false;
			const auto oEnt = latch.Step(blocked);
			Assert::IsTrue(oEnt.latched);

			// Immediate ABR step-up release releases cleanly.
			JointLatchInput abrRel = BaseInput(10);
			abrRel.abrStepImmediateRelease = true;
			const auto oRel = latch.Step(abrRel);
			Assert::IsFalse(oRel.latched);
			Assert::IsTrue(oRel.event == JointLatchEvent::ReleasedAbr);
			Assert::AreEqual(uint64_t(1), latch.Counters().releasesAbr);
		}
	};
}
