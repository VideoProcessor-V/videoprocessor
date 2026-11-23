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
#include <limits>
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

			// In-test negative control (Rule 3): a naive OR-voting / classical-alone latch enters on the exact same input!
			bool naiveClassicalEntered = false;

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
				if (in.classicalValid && Scaling::GeometrySet::AgreeingPair(in.classicalNx, in.classicalNxDenom, in.classicalNy, in.classicalNyDenom, in.rasterW, in.rasterH))
					naiveClassicalEntered = true;
				const auto o = latch.Step(in);
				Assert::IsFalse(o.latched);
				Assert::IsTrue(o.classicalOnlyBlocked);
				Assert::IsTrue(o.reason == JointLatchReason::WouldBeClassicalOnly);
				Assert::AreEqual(std::string("would-be (classical only)"), std::string(JointLatchReasonText(o.reason)));
			}

			Assert::IsTrue(naiveClassicalEntered, L"negative control: naive classical-alone baseline falsely enters on the exact same stream");
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
				// In-test negative control (Rule 3): naive OD-2 Option 3 ("either alone resets hard bound") on the exact same stream.
				int64_t naiveLastConfSec = 0;
				bool naiveEverReleased = false;
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

					if (in.classicalValid && in.classicalNx == 1920 && in.classicalNy == 1080)
						naiveLastConfSec = s; // naive Option 3: A resets hard bound every tick -> lock-without-release!
					if (s - naiveLastConfSec >= 180)
						naiveEverReleased = true;

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
				Assert::IsFalse(naiveEverReleased, L"negative control: naive classical-resets-bound stays locked past 180 s (Audit 100b H3 hazard)");
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

			// Brief transient detail_above glitch at t = 5.0 s..5.5 s (< 3.0 s dwell): ignored (debounced), latch stays held.
			// In-test negative control (Rule 3): a real ChainJointLatch configured with zero debounce
			// (detailAboveDebounceSec = 0.0), latched identically and stepped on the EXACT SAME transient spike
			// sequence, prematurely releases while the debounced (3.0 s) latch holds!
			{
				JointLatchSettings noDebounceCfg = Enabled();
				noDebounceCfg.detailAboveDebounceSec = 0.0;
				ChainJointLatch naiveNoDebounce(noDebounceCfg);
				Assert::IsTrue(naiveNoDebounce.Step(ChainOnly1080(0, kC1 + 1.0, 2)).latched);

				JointLatchInput in1 = BaseInput(5);
				in1.classicalDetailAbove = true;
				JointLatchInput in2 = BaseInput(5);
				in2.timeUs = 5500000; // 0.5 s transient spike (<< 3.0 s shot-dwell debounce)
				in2.classicalDetailAbove = true;

				const auto oNaive1 = naiveNoDebounce.Step(in1);
				const auto oNaive2 = naiveNoDebounce.Step(in2);
				Assert::IsTrue(oNaive1.latched, L"first sample anchors detailAboveSinceUs_");
				Assert::IsFalse(oNaive2.latched, L"negative control: real zero-debounce ChainJointLatch prematurely releases on a 0.5 s transient glitch");
				Assert::IsTrue(oNaive2.event == JointLatchEvent::ReleasedDetailAbove);
				Assert::AreEqual(uint64_t(1), naiveNoDebounce.Counters().releasesDetailAbove);

				const auto o1 = latch.Step(in1);
				Assert::IsTrue(o1.latched);
				Assert::IsTrue(o1.reason == JointLatchReason::DetailAboveTransient);
				const auto o2 = latch.Step(in2);
				Assert::IsTrue(o2.latched, L"debounced ChainJointLatch holds across 0.5 s transient glitch");
				Assert::IsTrue(o2.reason == JointLatchReason::DetailAboveTransient);
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

				// In-test negative control (Rule 3): naive max-credit selector picks the 2x alias 2560x1440!
				int naiveBest = 0;
				for (size_t i = 1; i < cands.size(); ++i)
					if ((std::min)(cands[i].creditX, cands[i].creditY) > (std::min)(cands[naiveBest].creditX, cands[naiveBest].creditY))
						naiveBest = static_cast<int>(i);
				Assert::AreEqual(2560, cands[static_cast<size_t>(naiveBest)].nx, L"negative control: naive max-credit picks the 2/3 alias");

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

		TEST_METHOD(NanInputsAndNonPositiveC1RejectedVisiblyWithoutPoisoningState)
		{
			using namespace JointLatchFx;
			const double nan = std::numeric_limits<double>::quiet_NaN();
			ChainJointLatch latch(Enabled());

			// HIGH-3: c1 <= 0 or NaN rejected visibly.
			for (double badC1 : { 0.0, -5.0, nan })
			{
				JointLatchInput in = Joint1080(1, 5.0, 20.0);
				in.c1 = badC1;
				const auto o = latch.Step(in);
				Assert::IsFalse(o.latched);
				Assert::IsTrue(o.reason == JointLatchReason::InvalidInput);
			}
			Assert::AreEqual(uint64_t(3), latch.Counters().invalidInputs);

			// HIGH-3: NaN in chainLlrY or currentShotCreditY must NEVER bypass std::min bar check!
			{
				JointLatchInput in = ChainOnly1080(5, 100.0, 3);
				in.chainLlrY = nan;
				Assert::IsFalse(latch.Step(in).latched);
			}
			{
				JointLatchInput in = Joint1080(6, 5.0, 100.0);
				in.currentShotCreditY = nan;
				Assert::IsFalse(latch.Step(in).latched);
			}

			// HIGH-3: NaN in HarmonicCandidate creditY excluded from SelectFundamentalHarmonic.
			{
				std::vector<HarmonicCandidate> cands;
				HarmonicCandidate bad;
				bad.nx = 1920; bad.Nx = kRasterW; bad.ny = 1080; bad.Ny = kRasterH;
				bad.creditX = 100.0; bad.creditY = nan;
				cands.push_back(bad);
				Assert::IsFalse(SelectFundamentalHarmonic(cands, kRasterW, kRasterH, 5.0).found);
			}

			// HIGH-3: NaN gainX while latched must NOT poison or reset accX_ to -need.
			Assert::IsTrue(latch.Step(ChainOnly1080(10, kC1 + 1.0, 2)).latched);
			{
				JointLatchInput halfGain = BaseInput(11);
				halfGain.gainX = 0.2 * kC1; halfGain.gainY = 0.2 * kC1;
				halfGain.gainTimeX = 11 * 1000000; halfGain.gainTimeY = 11 * 1000000;
				latch.Step(halfGain);

				JointLatchInput nanGain = BaseInput(12);
				nanGain.gainX = nan; nanGain.gainY = 0.0;
				latch.Step(nanGain);
				Assert::IsTrue(latch.Counters().invalidInputs >= 4);

				// Another 0.1 * c1 brings total to 0.3 * c1 >= 0.25 * c1 => confirms cleanly (accX_ was preserved!).
				JointLatchInput finish = BaseInput(13);
				finish.gainX = 0.1 * kC1; finish.gainY = 0.1 * kC1;
				finish.gainTimeX = 13 * 1000000; finish.gainTimeY = 13 * 1000000;
				const auto oConf = latch.Step(finish);
				Assert::IsTrue(oConf.confirmed);
			}
		}

		TEST_METHOD(H3HardBoundEnforces180sUnder5sLatchedCadenceAndEvalGap)
		{
			using namespace JointLatchFx;

			// HIGH-5a: 5 s latched cadence offset so bound triggers at t = 123 s -> MUST still release at t = 180 s (not 183 s!).
			{
				ChainJointLatch latch(Enabled());
				Assert::IsTrue(latch.Step(ChainOnly1080(0, kC1 + 1.0, 2)).latched);
				int64_t releasedAtSec = -1;
				for (int s = 3; s <= 185; s += 5)
				{
					// Include t = 180 explicitly in the cadence schedule (e.g. next eval at 180 s).
					const int tSec = (s == 183) ? 180 : s;
					JointLatchInput in = BaseInput(tSec);
					in.classicalValid = true;
					in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
					in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
					const auto o = latch.Step(in);
					if (!o.latched)
					{
						releasedAtSec = tSec;
						Assert::IsTrue(o.event == JointLatchEvent::ReleasedCeiling);
						break;
					}
				}
				Assert::AreEqual(int64_t(180), releasedAtSec);
			}

			// HIGH-5b: Eval gap from 118 s -> 185 s releases IMMEDIATELY on the 185 s eval (not deferred to 245 s!).
			{
				ChainJointLatch latch(Enabled());
				Assert::IsTrue(latch.Step(ChainOnly1080(0, kC1 + 1.0, 2)).latched);
				Assert::IsTrue(latch.Step(BaseInput(118)).latched);
				const auto oGap = latch.Step(BaseInput(185));
				Assert::IsFalse(oGap.latched);
				Assert::IsTrue(oGap.event == JointLatchEvent::ReleasedCeiling);
			}
		}

		TEST_METHOD(R1DetailAboveSubHarmonicWithHigherCreditNeverBeatsCleanSuperHarmonic)
		{
			using namespace JointLatchFx;
			// MEDIUM-3: 540p has HIGHER credit (20.0) than 1080p (12.0), but 540p has detailAbove == true!
			// 540p MUST NOT beat clean 1080p.
			std::vector<HarmonicCandidate> cands;
			HarmonicCandidate c540;
			c540.nx = 960; c540.Nx = kRasterW; c540.ny = 540; c540.Ny = kRasterH;
			c540.creditX = 20.0; c540.creditY = 20.0;
			c540.classicalRollOffConfirmed = true;
			c540.detailAbove = true;

			HarmonicCandidate c1080;
			c1080.nx = 1920; c1080.Nx = kRasterW; c1080.ny = 1080; c1080.Ny = kRasterH;
			c1080.creditX = 12.0; c1080.creditY = 12.0;
			c1080.classicalRollOffConfirmed = true;
			c1080.detailAbove = false;

			cands.push_back(c540);
			cands.push_back(c1080);

			// In-test negative control (Rule 3): naive max-credit selector picks the dirty 540p false sub-harmonic!
			int naiveBest = 0;
			for (size_t i = 1; i < cands.size(); ++i)
				if ((std::min)(cands[i].creditX, cands[i].creditY) > (std::min)(cands[naiveBest].creditX, cands[naiveBest].creditY))
					naiveBest = static_cast<int>(i);
			Assert::AreEqual(960, cands[static_cast<size_t>(naiveBest)].nx, L"negative control: naive max-credit picks the detailAbove false sub-harmonic");

			const auto res = SelectFundamentalHarmonic(cands, kRasterW, kRasterH, 7.5);
			Assert::IsTrue(res.found);
			Assert::AreEqual(1920, res.nx);
			Assert::AreEqual(1080, res.ny);
			Assert::IsTrue(res.reason == HarmonicSelectionReason::RejectedSubHarmonicDetailAbove);

			// When ALL candidates have detailAbove == true, none is selected.
			cands[1].detailAbove = true;
			const auto allDirty = SelectFundamentalHarmonic(cands, kRasterW, kRasterH, 7.5);
			Assert::IsFalse(allDirty.found);
			Assert::IsTrue(allDirty.reason == HarmonicSelectionReason::RejectedSubHarmonicDetailAbove);
		}

		TEST_METHOD(G2RejectsStaleAndFutureCreditAtHardBound)
		{
			using namespace JointLatchFx;
			// MEDIUM-4: stale credit from t = 1 s (119 s old) or future credit (t = 130 s) must NOT renew G2 at t = 120 s!
			for (int64_t badCreditSec : { int64_t(1), int64_t(60), int64_t(130) })
			{
				ChainJointLatch g2(Enabled(JointHoldPolicy::G2_JointOncePerBound));
				g2.Step(ChainOnly1080(0, kC1 + 1.0, 2));
				for (int s = 1; s < 120; ++s)
				{
					JointLatchInput in = BaseInput(s);
					in.classicalValid = true;
					in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
					in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
					g2.Step(in);
				}
				const auto o = g2.Step(Joint1080(120, 4.0, kC1, badCreditSec));
				Assert::IsFalse(o.g2Renewed);
				Assert::IsTrue(o.deferredNow);
				Assert::AreEqual(uint64_t(0), g2.Counters().g2JointRenewals);
			}
		}

		TEST_METHOD(PendingStepMarkBlocksMoveWhileLatchedAndDetailAboveNeverPausesClock)
		{
			using namespace JointLatchFx;
			ChainJointLatch latch(Enabled());

			// LOW-5: unlatched menuHold with no classical reading reports MenuHoldActive (not ClassicalVoided).
			{
				JointLatchInput m = BaseInput(0);
				m.menuHold = true;
				const auto om = latch.Step(m);
				Assert::IsTrue(om.reason == JointLatchReason::MenuHoldActive);
				Assert::IsFalse(om.classicalVoided);
			}

			// Enter at 1920x1080.
			Assert::IsTrue(latch.Step(ChainOnly1080(1, kC1 + 1.0, 2)).latched);

			// LOW-5: frame with BOTH classicalValid agreeing AND classicalDetailAbove must NOT pause starvation clock!
			{
				JointLatchInput contradiction = BaseInput(2);
				contradiction.classicalValid = true;
				contradiction.classicalNx = 1920; contradiction.classicalNxDenom = kRasterW;
				contradiction.classicalNy = 1080; contradiction.classicalNyDenom = kRasterH;
				contradiction.classicalDetailAbove = true;
				latch.Step(contradiction);
				Assert::AreEqual(uint64_t(0), latch.Counters().classicalHoldPauses);
				Assert::IsTrue(latch.ReleaseClockSec() > 0.0);
			}

			// LOW-6: while latched at 1920x1080, stepMarkPending == true blocks MOVE to 1280x720!
			{
				JointLatchInput mv = BaseInput(5);
				mv.stepMarkPending = true;
				mv.haveChainPair = true;
				mv.chainNx = 1280; mv.chainNxDenom = kRasterW;
				mv.chainNy = 720; mv.chainNyDenom = kRasterH;
				mv.chainLlrX = 2.0 * kC1; mv.chainLlrY = 2.0 * kC1;
				mv.chainShots = 3;
				const auto oBlk = latch.Step(mv);
				Assert::IsTrue(oBlk.latched);
				Assert::AreEqual(1920, oBlk.nx);
				Assert::IsTrue(oBlk.heldBack);
				Assert::IsTrue(oBlk.reason == JointLatchReason::BlockedByStepMark);
				Assert::AreEqual(uint64_t(0), latch.Counters().moves);
				Assert::AreEqual(uint64_t(1), latch.Counters().blockedByStepMark);

				// Once pending mark clears, MOVE succeeds!
				mv.timeUs = 6 * 1000000;
				mv.stepMarkPending = false;
				const auto oMv = latch.Step(mv);
				Assert::IsTrue(oMv.latched);
				Assert::IsTrue(oMv.event == JointLatchEvent::Moved);
				Assert::AreEqual(1280, oMv.nx);
				Assert::AreEqual(720, oMv.ny);
				Assert::AreEqual(uint64_t(1), latch.Counters().moves);
			}
		}

		TEST_METHOD(JointOnlyPolicyG2ChainResetBackwardTimeAndInfGuards)
		{
			using namespace JointLatchFx;
			const double inf = std::numeric_limits<double>::infinity();

			// 1. JointOnly policy blocks Chain-alone while still admitting Joint (with ChainOrJoint negative control).
			{
				const JointLatchInput chainIn = ChainOnly1080(5, 2.0 * kC1, 3);
				ChainJointLatch chainOrJoint(Enabled(JointHoldPolicy::G1_ChainOnlyHardBound, JointEntryPolicy::ChainOrJoint));
				Assert::IsTrue(chainOrJoint.Step(chainIn).latched, L"negative control: ChainOrJoint admits 3-shot Chain-alone");

				ChainJointLatch jointOnly(Enabled(JointHoldPolicy::G1_ChainOnlyHardBound, JointEntryPolicy::JointOnly));
				Assert::IsFalse(jointOnly.Step(chainIn).latched, L"JointOnly blocks Chain-alone even at 2*c1 over 3 shots");
				Assert::AreEqual(uint64_t(0), jointOnly.Counters().entriesChainAlone);

				const auto oJoint = jointOnly.Step(Joint1080(6, 4.0, 0.6 * kC1, 6));
				Assert::IsTrue(oJoint.latched && oJoint.event == JointLatchEvent::EnteredJoint);
				Assert::AreEqual(uint64_t(1), jointOnly.Counters().entriesJoint);
			}

			// 2. G2 renewal flag resets cleanly after a genuine Chain-alone re-confirmation, permitting 1 renewal on the NEXT bound.
			{
				ChainJointLatch g2(Enabled(JointHoldPolicy::G2_JointOncePerBound));
				Assert::IsTrue(g2.Step(ChainOnly1080(0, kC1 + 1.0, 2)).latched);
				for (int s = 1; s < 120; ++s)
				{
					JointLatchInput in = BaseInput(s);
					in.classicalValid = true;
					in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
					in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
					g2.Step(in);
				}
				Assert::IsTrue(g2.Step(Joint1080(120, 4.0, kC1, 120)).g2Renewed);
				Assert::IsTrue(g2.G2RenewedThisBound());

				// Genuine fresh Chain-alone confirmation at t = 130 s resets lastConfUs_ and g2RenewedThisBound_!
				JointLatchInput chainConfirm = BaseInput(130);
				chainConfirm.chainInformative = true;
				chainConfirm.gainX = 0.3 * kC1; chainConfirm.gainY = 0.3 * kC1;
				chainConfirm.rawX = 0.3 * kC1; chainConfirm.rawY = 0.3 * kC1;
				chainConfirm.gainTimeX = 130 * 1000000; chainConfirm.gainTimeY = 130 * 1000000;
				const auto oConf = g2.Step(chainConfirm);
				Assert::IsTrue(oConf.confirmed && oConf.reason == JointLatchReason::ChainReconfirmed);
				Assert::IsFalse(g2.G2RenewedThisBound());

				// Advance to the NEXT 120 s bound (t = 250 s): one fresh joint renewal is permitted again.
				for (int s = 131; s < 250; ++s)
				{
					JointLatchInput in = BaseInput(s);
					in.classicalValid = true;
					in.classicalNx = 1920; in.classicalNxDenom = kRasterW;
					in.classicalNy = 1080; in.classicalNyDenom = kRasterH;
					g2.Step(in);
				}
				const auto oRenew2 = g2.Step(Joint1080(250, 4.0, kC1, 250));
				Assert::IsTrue(oRenew2.g2Renewed);
				Assert::AreEqual(uint64_t(2), g2.Counters().g2JointRenewals);
			}

			// 3. Backward source time jump while latched re-anchors cleanly without negative clock or spurious release.
			{
				ChainJointLatch latch(Enabled());
				Assert::IsTrue(latch.Step(ChainOnly1080(100, kC1 + 1.0, 2)).latched);
				const auto oBack = latch.Step(BaseInput(50));
				Assert::IsTrue(oBack.latched);
				Assert::IsTrue(oBack.reason == JointLatchReason::BackwardTimeReanchored);
				Assert::IsTrue(latch.ReleaseClockSec() >= 0.0);
			}

			// 4. +Inf / -Inf inputs rejected visibly without latching or poisoning.
			{
				ChainJointLatch latch(Enabled());
				JointLatchInput badInfC1 = Joint1080(5, 4.0, 10.0);
				badInfC1.c1 = inf;
				Assert::IsTrue(latch.Step(badInfC1).reason == JointLatchReason::InvalidInput);

				JointLatchInput badInfLlr = ChainOnly1080(6, inf, 2);
				Assert::IsFalse(latch.Step(badInfLlr).latched);

				JointLatchInput badInfShot = Joint1080(7, 4.0, inf);
				Assert::IsFalse(latch.Step(badInfShot).latched);
				Assert::IsTrue(latch.Counters().invalidInputs >= 1);
			}
		}
	};
}
