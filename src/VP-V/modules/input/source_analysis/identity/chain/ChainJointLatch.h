// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainJointLatch (SID Stage S1, Owner-approved OD-1 & OD-2, doc 100 s2.4, R1/M1/M2/H3):
// Two-witness joint latch combining Classical (Witness A) and Chain (Witness B), plus R1 fundamental-harmonic
// order selection.
//
// Policy contract (Owner-approved 2026-11-23):
//   - Default OFF (JointLatchSettings::enabled = false): inert, visibly reports JointLatchReason::Disabled.
//   - Entry Authority (OD-1, default EntryPolicy::ChainOrJoint):
//       (a) Chain alone (full fallback when A is blind, e.g. HDMI loop): 2+ shots, conventional non-parked pair,
//           both axes >= Bar(t, c1).
//       (b) Joint (fast 1-shot path): fresh per-frame informative A reading at a conventional non-parked pair
//           AND B current-shot dwell >= kMinShotDwellSec (3 s) AND B current-shot credit >= j(t) * c1 on both axes,
//           where j(t) grows linearly from jMin = 0.5 (shotCapFraction) to jMax = 1.0 over [graceSec, graceSec+growSec].
//       (c) Classical alone NEVER enters: visibly counted and reported as "would-be (classical only)" (Rule 4).
//   - Hard-Bound Holding Policy (OD-2):
//       (a) Default G1 (HoldPolicy::G1_ChainOnlyHardBound): ONLY fresh Chain proof resets the 120 s hard bound.
//           Fresh agreeing A may pause the shorter 60 s starvation clock ONLY while B is not informative-and-non-crediting.
//           A never moves the 120 s hard bound. Deferred starvation release carries a strict 60 s ceiling checked
//           before dwell, guaranteeing release <= hardBoundSec + deferCeilingSec = 180 s (Audit 100b H3).
//       (b) Switchable G2 (HoldPolicy::G2_JointOncePerBound): same as G1, except upon reaching the 120 s bound it may
//           renew ONCE per bound if both A (fresh, non-void, agreeing) and B (fresh post-anchor current-shot credit
//           >= j(t) * c1 after 3 s dwell) jointly re-prove the latched pair. A second bound without Chain-alone proof
//           cannot renew and releases <= 180 s.
//   - M1 Debounced detail_above: single-frame detail_above is ignored (counted); sustained detail_above >= 3 s
//     (1 shot dwell) releases cleanly as ReleasedDetailAbove.
//   - M2 Classical Void: during MenuHold / mixed / subtitle / fade, A is void (neither enters, confirms, pauses
//     starvation clock, nor triggers detail_above). MenuHold also triggers immediate R7 release.
//   - R1 Fundamental-Harmonic Order Selection: SelectFundamentalHarmonic picks the smallest conventional member
//     of an integer-ratio family (e.g. 1/3 over 2/3) confirmed by A roll-off (no detail_above) + B credit.
//
// Pure C++14, header-only, deterministic, no clock, no threads, no Windows headers.
#include "ChainCommon.h"
#include "ChainEvidence.h"
#include "ChainLatch.h"

#include <VP-V/modules/scaling/geometry/SourceGeometrySet.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace SourceIdentity
{
	namespace Chain
	{
		enum class JointEntryPolicy : int
		{
			ChainOrJoint = 0, // Owner-approved OD-1 Option 1 (default): Chain-alone (2 shots) OR Joint (1 shot); classical-alone NEVER enters
			JointOnly = 1     // OD-1 Option 2 (eval option): Joint only; neither classical-alone nor chain-alone enters
		};

		enum class JointHoldPolicy : int
		{
			G1_ChainOnlyHardBound = 0, // Owner-approved OD-2 Option 1 (default): only Chain resets 120 s bound; guaranteed release <= 180 s
			G2_JointOncePerBound = 1   // OD-2 Option 2 (eval option): fresh joint re-proof may renew 120 s bound at most once per bound
		};

		enum class JointLatchEvent : int
		{
			None = 0,
			EnteredChainAlone,
			EnteredJoint,
			Moved,
			ReleasedStarvation,
			ReleasedHardBound,
			ReleasedCeiling,
			ReleasedNative,
			ReleasedDetailAbove,
			ReleasedMenu,
			ReleasedAbr,
			ReleasedReset
		};

		enum class JointLatchEntryPath : int
		{
			None = 0,
			ChainAlone = 1,
			Joint = 2
		};

		enum class JointLatchReason : int
		{
			None = 0,
			Disabled,
			InvalidInput,               // HIGH-3: non-finite or non-positive c1 / malformed floating input
			MenuHoldActive,             // LOW-5: menu / overlay active while unlatched
			WouldBeClassicalOnly,       // OD-1: classical proposed alone without qualifying chain corroboration
			NonConventionalPair,        // R2: non-conventional or parked anamorphic geometry (e.g. 1440x1080, 1600x900)
			HeldBackByBar,              // Evidence below entry/move bar
			BlockedByStepMark,          // ChainStepMark pending: ENTER/MOVE frozen
			ClassicalVoided,            // M2: classical reading voided during menu/mixed/subtitle/fade
			EnteredChainAlone,
			EnteredJoint,
			Moved,
			ChainReconfirmed,
			ClassicalPausedStarvation,  // A paused 60 s clock while B was uninformative/consistent (never moves 120 s bound)
			ClassicalAssistDeniedByB,   // H3: B informative and non-crediting; A forbidden from pausing 60 s clock
			G2JointRenewal,             // OD-2 G2: once-per-bound fresh joint renewal at 120 s
			G2RenewalExhausted,         // OD-2 G2: already renewed once this bound; must release
			StarvationDeferred,
			StarvationDeferCancelled,
			DetailAboveTransient,       // M1: brief detail_above ignored pending 1-shot dwell
			DetailAboveSustained,       // M1: sustained detail_above >= 3 s released latch
			ReleasedNative,
			ReleasedMenu,
			ReleasedAbr,
			ReleasedCeiling,
			ReleasedHardBound,
			ReleasedStarvation,
			ReleasedReset,
			BackwardTimeReanchored
		};

		inline const char* JointLatchReasonText(JointLatchReason r)
		{
			switch (r)
			{
			case JointLatchReason::None: return "none";
			case JointLatchReason::Disabled: return "joint latch disabled";
			case JointLatchReason::InvalidInput: return "rejected (invalid input)";
			case JointLatchReason::MenuHoldActive: return "holding (menu / overlay)";
			case JointLatchReason::WouldBeClassicalOnly: return "would-be (classical only)";
			case JointLatchReason::NonConventionalPair: return "held back (non-conventional or parked geometry)";
			case JointLatchReason::HeldBackByBar: return "held back (below bar)";
			case JointLatchReason::BlockedByStepMark: return "held back (ABR step mark pending)";
			case JointLatchReason::ClassicalVoided: return "classical reading voided (menu/mixed/subtitle/fade)";
			case JointLatchReason::EnteredChainAlone: return "latched (chain alone)";
			case JointLatchReason::EnteredJoint: return "latched (joint A+B)";
			case JointLatchReason::Moved: return "moved to another conventional size";
			case JointLatchReason::ChainReconfirmed: return "re-confirmed by fresh chain evidence";
			case JointLatchReason::ClassicalPausedStarvation: return "starvation clock paused by agreeing classical reading";
			case JointLatchReason::ClassicalAssistDeniedByB: return "classical hold-assist denied (chain informative, non-crediting)";
			case JointLatchReason::G2JointRenewal: return "G2 hard bound renewed once by fresh joint re-proof";
			case JointLatchReason::G2RenewalExhausted: return "G2 once-per-bound renewal already used";
			case JointLatchReason::StarvationDeferred: return "starvation bound reached (release deferred to shot boundary)";
			case JointLatchReason::StarvationDeferCancelled: return "deferred starvation release cancelled by chain re-confirmation";
			case JointLatchReason::DetailAboveTransient: return "transient detail_above ignored (debouncing)";
			case JointLatchReason::DetailAboveSustained: return "released to native (sustained detail_above)";
			case JointLatchReason::ReleasedNative: return "released to native (native evidence)";
			case JointLatchReason::ReleasedMenu: return "released to native (menu / overlay)";
			case JointLatchReason::ReleasedAbr: return "released to native (ABR step)";
			case JointLatchReason::ReleasedCeiling: return "released to native (no cut within ceiling; <= 180 s hard release)";
			case JointLatchReason::ReleasedHardBound: return "released to native (hard bound)";
			case JointLatchReason::ReleasedStarvation: return "released to native (starvation clock)";
			case JointLatchReason::ReleasedReset: return "released to native (reset)";
			case JointLatchReason::BackwardTimeReanchored: return "backward source time re-anchored";
			default: return "unknown";
			}
		}

		inline bool JointLatchEventIsRelease(JointLatchEvent e)
		{
			return e >= JointLatchEvent::ReleasedStarvation;
		}

		struct JointLatchSettings
		{
			bool enabled = false;  // OFF by default (Stage S1 safe contract)
			JointEntryPolicy entryPolicy = JointEntryPolicy::ChainOrJoint;      // Owner-approved OD-1 Option 1
			JointHoldPolicy holdPolicy = JointHoldPolicy::G1_ChainOnlyHardBound; // Owner-approved OD-2 Option 1

			// Contract constants matching ChainLatch.h (E0/R5/N1/N2) and ChainEvidence.h (E2):
			double confirmFraction = 0.25;      // of c1: fresh chain credit on both axes that re-confirms & resets 120 s bound
			double releaseBoundSec = 60.0;      // shorter starvation clock bound (ChainLatch.h:63)
			double absentClockRate = 0.25;      // R5b slowed clock through uninformative/consistent evals (ChainLatch.h:64)
			double hardBoundSec = 120.0;        // R5b hard bound since newest confirming proof (ChainLatch.h:65)
			double graceSec = 30.0;             // bar & j(t) growth starts after this stable time (ChainLatch.h:66)
			double growSec = 120.0;             // bar & j(t) reach maximum over this much more time (ChainLatch.h:67)
			double growMax = 1.0;               // chain-alone bar at most doubles (ChainLatch.h:68)
			double movedTolerance = 0.004;      // relative ratio delta counting as a move (ChainLatch.h:70)
			bool deferStarvation = true;        // N1: defer starvation release to next shot boundary
			double deferDwellSec = kMinShotDwellSec; // 3.0 s: new shot dwell to re-confirm (ChainEvidence.h:72)
			int deferDwellExtraEvaluations = 1; // N2: 1 extra eval after dwell end (ChainLatch.h:74)
			double deferCeilingSec = 60.0;      // N1: hard ceiling => hardBoundSec + deferCeilingSec = 180 s guaranteed max

			// Derived joint-entry parameters (Plan s4.1):
			// jMin = EvidenceSettings::shotCapFraction (0.5): 1-shot cap per axis when corroborated by independent witness A.
			// jMax = 2 * shotCapFraction (1.0): full c1 on a long stable stream (prevents slow drift false entries).
			double jointFractionMin = 0.5;
			double jointFractionMax = 1.0;
			// Derived from ChainEvidence::kMinShotDwellSec (3.0 s): 1 shot dwell for joint entry and M1 detail_above debounce.
			double minShotDwellSec = kMinShotDwellSec;
			double detailAboveDebounceSec = kMinShotDwellSec;
			// Derived from ChainEvidence two-shot requirement (shotCapFraction = 0.5 => >= 2 distinct shots to reach c1).
			int chainAloneMinShots = 2;
			// Derived from Scaling::GeometrySet::kConventionTolerance (0.006): pair agreement tolerance.
			double pairMatchTolerance = Scaling::GeometrySet::kConventionTolerance;
		};

		struct JointLatchInput
		{
			int64_t timeUs = 0;
			double c1 = 14.873;                  // Derived default from ComputeThresholds(EvidenceSettings())
			int rasterW = 0, rasterH = 0;        // Delivered raster; 0 = unknown => nothing conventional

			// Context & gating flags:
			bool menuHold = false;               // R7: immediate release, blocks entry, voids A
			bool mixedContent = false;           // M2: voids A
			bool subtitleOverlay = false;        // M2: voids A
			bool inFade = false;                 // M2: voids A
			bool stepMarkPending = false;        // ChainStepMark::BlocksEntryOrMove(): blocks ENTER/MOVE
			bool abrStepImmediateRelease = false;// ChainStepMark::ConfirmVoidAndRelease: immediate release
			bool shotBoundary = false;           // N1: new evidence shot boundary (cut/black/fade)
			bool chainNativeBoth = false;        // Both chain axes read confident native

			// Witness B (Chain) entry & holding state:
			bool chainInformative = false;       // Chain earned evidence on at least one axis this eval
			bool haveChainPair = false;
			int chainNx = 0, chainNxDenom = 0, chainNy = 0, chainNyDenom = 0;
			double chainLlrX = 0.0, chainLlrY = 0.0;
			int chainShots = 0;                  // Distinct informative shots contributing to chainPair

			// Witness B current-shot state (used for Joint entry and G2 fresh joint re-proof):
			double currentShotDwellSec = 0.0;
			double currentShotCreditX = 0.0, currentShotCreditY = 0.0;
			int64_t currentShotNewestCreditUs = -1; // Source time of newest positive credit in current shot

			// Witness B incremental credits for the currently latched size (M1, matching ChainLatch.h:135-137):
			double gainX = 0.0, gainY = 0.0;
			double rawX = 0.0, rawY = 0.0;
			int64_t gainTimeX = -1, gainTimeY = -1;

			// Witness A (Classical) FRESH per-frame informative reading (never sticky tracker state):
			bool classicalValid = false;         // Fresh informative per-frame A reading present
			int classicalNx = 0, classicalNxDenom = 0, classicalNy = 0, classicalNyDenom = 0;
			bool classicalDetailAbove = false;   // M1: above-band spectral detail seen on this frame
		};

		struct JointLatchOutput
		{
			bool latched = false;
			int nx = 0, Nx = 0, ny = 0, Ny = 0;
			JointLatchEvent event = JointLatchEvent::None;
			JointLatchEntryPath entryPath = JointLatchEntryPath::None;
			JointLatchReason reason = JointLatchReason::None;
			bool confirmed = false;
			bool g2Renewed = false;
			bool deferredNow = false;
			bool deferCancelled = false;
			bool deferred = false;
			bool classicalOnlyBlocked = false;   // true whenever A proposed alone and was visibly refused
			bool classicalVoided = false;        // true whenever A input was present but voided (M2)
			bool heldBack = false;
			bool nonConventional = false;
			uint32_t reasonBits = 0;
		};

		struct JointLatchCounters
		{
			uint64_t entriesChainAlone = 0;
			uint64_t entriesJoint = 0;
			uint64_t moves = 0;
			uint64_t confirmsChain = 0;
			uint64_t g2JointRenewals = 0;
			uint64_t classicalHoldPauses = 0;
			uint64_t classicalHoldDeniedByB = 0;
			uint64_t wouldBeClassicalOnly = 0;   // OD-1 visible counter for refused classical-alone proposals
			uint64_t classicalVoided = 0;        // M2 visible counter
			uint64_t blockedByStepMark = 0;
			uint64_t heldBack = 0;
			uint64_t nonConventional = 0;
			uint64_t detailAboveTransientIgnored = 0;
			uint64_t releases = 0;
			uint64_t releasesStarvation = 0;
			uint64_t releasesHardBound = 0;
			uint64_t releasesCeiling = 0;
			uint64_t releasesAtCut = 0;
			uint64_t releasesNative = 0;
			uint64_t releasesDetailAbove = 0;
			uint64_t releasesMenu = 0;
			uint64_t releasesAbr = 0;
			uint64_t releasesReset = 0;
			uint64_t deferred = 0;
			uint64_t deferCancelled = 0;
			uint64_t invalidInputs = 0;
		};

		// ============================================================================================================
		// R1 Fundamental-Harmonic Order Selection (doc 100 P3, s2.2-2.3, FM-3)
		// ============================================================================================================
		// An n-bandlimited upscaled signal lies in the span of every integer harmonic k*n (k >= 2), so e.g. a
		// 720p->2160p (1/3) upscale creates dips at both 1/3 and 2/3. Among conventional candidates meeting the
		// current-shot credit floor, if a smaller candidate `a` is an integer sub-harmonic of `b` (b/a ≈ k >= 2 on
		// both axes within kConventionTolerance) AND `a` is confirmed by classical roll-off with NO detail_above,
		// then `a` is the true fundamental and `b` is an alias; conversely, if `a` has detail_above (real energy
		// exists between `a` and `b`), `a` is rejected as a false sub-harmonic.
		struct HarmonicCandidate
		{
			int nx = 0, Nx = 0, ny = 0, Ny = 0;
			double creditX = 0.0, creditY = 0.0;
			bool classicalRollOffConfirmed = false; // Witness A confirms roll-off at this candidate's Nyquist
			bool detailAbove = false;               // Witness A sees genuine energy above this candidate's Nyquist
		};

		enum class HarmonicSelectionReason : int
		{
			NoValidCandidate = 0,
			SingleConventionalCandidate,
			SelectedHighestCredit,
			SelectedFundamentalConfirmed,     // Chose smaller integer sub-harmonic confirmed by A roll-off + B credit
			RejectedSubHarmonicDetailAbove    // Smaller sub-harmonic had detail_above; kept larger candidate
		};

		struct HarmonicSelectionResult
		{
			bool found = false;
			int selectedIndex = -1;
			int nx = 0, Nx = 0, ny = 0, Ny = 0;
			HarmonicSelectionReason reason = HarmonicSelectionReason::NoValidCandidate;
			int rejectedHarmonicCount = 0;
		};

		inline bool IsIntegerHarmonicOf(double subRatio, double superRatio, double tol = Scaling::GeometrySet::kConventionTolerance)
		{
			if (!std::isfinite(subRatio) || !std::isfinite(superRatio) || !std::isfinite(tol))
				return false;
			if (!(subRatio > 0.0) || !(superRatio > subRatio))
				return false;
			const double mult = superRatio / subRatio;
			const double nearest = std::floor(mult + 0.5);
			if (nearest < 2.0)
				return false;
			return std::fabs(mult - nearest) <= tol * nearest + 1e-9;
		}

		inline HarmonicSelectionResult SelectFundamentalHarmonic(
			const std::vector<HarmonicCandidate>& candidates,
			int rasterW,
			int rasterH,
			double minCredit,
			double ratioTol = Scaling::GeometrySet::kConventionTolerance)
		{
			HarmonicSelectionResult res;
			if (!std::isfinite(minCredit) || !std::isfinite(ratioTol) || ratioTol < 0.0)
				return res;

			// HIGH-3 + MEDIUM-3: require finite credit on BOTH axes before comparing, and separate clean candidates
			// (!detailAbove) from candidates invalidated by genuine high-frequency detail above their Nyquist.
			std::vector<int> cleanValid;
			std::vector<int> detailAboveValid;
			for (size_t i = 0; i < candidates.size(); ++i)
			{
				const HarmonicCandidate& c = candidates[i];
				if (c.nx <= 0 || c.Nx <= 0 || c.ny <= 0 || c.Ny <= 0)
					continue;
				if (!std::isfinite(c.creditX) || !std::isfinite(c.creditY))
					continue;
				if (!Scaling::GeometrySet::AgreeingPair(c.nx, c.Nx, c.ny, c.Ny, rasterW, rasterH))
					continue;
				if ((std::min)(c.creditX, c.creditY) < minCredit)
					continue;
				if (c.detailAbove)
					detailAboveValid.push_back(static_cast<int>(i));
				else
					cleanValid.push_back(static_cast<int>(i));
			}

			if (cleanValid.empty())
			{
				if (!detailAboveValid.empty())
					res.reason = HarmonicSelectionReason::RejectedSubHarmonicDetailAbove;
				return res;
			}

			auto candScore = [&](int idx) {
				const HarmonicCandidate& c = candidates[static_cast<size_t>(idx)];
				return (std::min)(c.creditX, c.creditY);
			};
			auto candAreaRatio = [&](int idx) {
				const HarmonicCandidate& c = candidates[static_cast<size_t>(idx)];
				return (static_cast<double>(c.nx) / c.Nx) * (static_cast<double>(c.ny) / c.Ny);
			};

			// Pick the baseline leader ONLY from clean (!detailAbove) candidates by highest min(creditX, creditY).
			int bestIdx = cleanValid[0];
			for (size_t k = 1; k < cleanValid.size(); ++k)
			{
				const int idx = cleanValid[k];
				const double sNew = candScore(idx);
				const double sBest = candScore(bestIdx);
				if (sNew > sBest + 1e-12 || (std::fabs(sNew - sBest) <= 1e-12 && candAreaRatio(idx) < candAreaRatio(bestIdx)))
					bestIdx = idx;
			}

			const HarmonicCandidate& leader = candidates[static_cast<size_t>(bestIdx)];
			const double leaderRx = static_cast<double>(leader.nx) / static_cast<double>(leader.Nx);
			const double leaderRy = static_cast<double>(leader.ny) / static_cast<double>(leader.Ny);

			// Check if any detail_above candidate was a rejected sub-harmonic of the clean leader.
			bool sawDetailAboveSubHarmonic = false;
			for (size_t k = 0; k < detailAboveValid.size(); ++k)
			{
				const HarmonicCandidate& sub = candidates[static_cast<size_t>(detailAboveValid[k])];
				const double subRx = static_cast<double>(sub.nx) / static_cast<double>(sub.Nx);
				const double subRy = static_cast<double>(sub.ny) / static_cast<double>(sub.Ny);
				if (IsIntegerHarmonicOf(subRx, leaderRx, ratioTol) && IsIntegerHarmonicOf(subRy, leaderRy, ratioTol))
					sawDetailAboveSubHarmonic = true;
			}

			if (cleanValid.size() == 1)
			{
				res.found = true;
				res.selectedIndex = bestIdx;
				res.nx = leader.nx; res.Nx = leader.Nx; res.ny = leader.ny; res.Ny = leader.Ny;
				res.reason = sawDetailAboveSubHarmonic
					? HarmonicSelectionReason::RejectedSubHarmonicDetailAbove
					: HarmonicSelectionReason::SingleConventionalCandidate;
				return res;
			}

			// Check whether any smaller clean conventional candidate is an integer sub-harmonic of bestIdx,
			// confirmed by A roll-off (!detailAbove is already guaranteed by cleanValid).
			int fundamentalIdx = -1;
			int aliasesExplained = 0;

			for (size_t k = 0; k < cleanValid.size(); ++k)
			{
				const int idx = cleanValid[k];
				if (idx == bestIdx)
					continue;
				const HarmonicCandidate& sub = candidates[static_cast<size_t>(idx)];
				const double subRx = static_cast<double>(sub.nx) / static_cast<double>(sub.Nx);
				const double subRy = static_cast<double>(sub.ny) / static_cast<double>(sub.Ny);
				if (IsIntegerHarmonicOf(subRx, leaderRx, ratioTol) && IsIntegerHarmonicOf(subRy, leaderRy, ratioTol))
				{
					if (sub.classicalRollOffConfirmed)
					{
						if (fundamentalIdx < 0 || candAreaRatio(idx) < candAreaRatio(fundamentalIdx))
							fundamentalIdx = idx;
						++aliasesExplained;
					}
				}
			}

			// Also check if bestIdx itself is already the smallest confirmed fundamental and has higher integer harmonics in cleanValid.
			if (fundamentalIdx < 0 && leader.classicalRollOffConfirmed)
			{
				for (size_t k = 0; k < cleanValid.size(); ++k)
				{
					const int idx = cleanValid[k];
					if (idx == bestIdx)
						continue;
					const HarmonicCandidate& sup = candidates[static_cast<size_t>(idx)];
					const double supRx = static_cast<double>(sup.nx) / static_cast<double>(sup.Nx);
					const double supRy = static_cast<double>(sup.ny) / static_cast<double>(sup.Ny);
					if (IsIntegerHarmonicOf(leaderRx, supRx, ratioTol) && IsIntegerHarmonicOf(leaderRy, supRy, ratioTol))
						++aliasesExplained;
				}
				if (aliasesExplained > 0)
					fundamentalIdx = bestIdx;
			}

			const int chosen = fundamentalIdx >= 0 ? fundamentalIdx : bestIdx;
			const HarmonicCandidate& out = candidates[static_cast<size_t>(chosen)];
			res.found = true;
			res.selectedIndex = chosen;
			res.nx = out.nx; res.Nx = out.Nx; res.ny = out.ny; res.Ny = out.Ny;
			res.rejectedHarmonicCount = aliasesExplained;
			if (fundamentalIdx >= 0)
				res.reason = HarmonicSelectionReason::SelectedFundamentalConfirmed;
			else if (sawDetailAboveSubHarmonic)
				res.reason = HarmonicSelectionReason::RejectedSubHarmonicDetailAbove;
			else
				res.reason = HarmonicSelectionReason::SelectedHighestCredit;
			return res;
		}

		// ============================================================================================================
		// ChainJointLatch state machine
		// ============================================================================================================
		class ChainJointLatch
		{
		public:
			ChainJointLatch() {}
			explicit ChainJointLatch(const JointLatchSettings& s) : s_(s) {}

			const JointLatchSettings& Settings() const { return s_; }
			const JointLatchCounters& Counters() const { return c_; }
			bool Latched() const { return latched_; }
			int LatchedNx() const { return lnx_; }
			int LatchedNy() const { return lny_; }
			int LatchedNxDenom() const { return lNx_; }
			int LatchedNyDenom() const { return lNy_; }
			JointLatchEntryPath EntryPath() const { return entryPath_; }
			double ReleaseClockSec() const { return clock_; }
			bool Deferred() const { return deferred_; }
			bool Dwelling() const { return deferred_ && dwelling_; }
			int64_t LastConfirmUs() const { return lastConfUs_; }
			bool G2RenewedThisBound() const { return g2RenewedThisBound_; }

			// Chain-alone entry / move bar: c1 * (1 + clamp((stable - graceSec) / growSec, 0, growMax)).
			double Bar(int64_t timeUs, double c1) const
			{
				const double stable = started_ ? static_cast<double>(timeUs - stableSinceUs_) / 1e6 : 0.0;
				const double g = (std::isfinite(s_.growSec) && s_.growSec > 0.0) ? (stable - s_.graceSec) / s_.growSec : 0.0;
				const double gSafe = std::isfinite(g) ? ClampD(g, 0.0, s_.growMax) : 0.0;
				return c1 * (1.0 + gSafe);
			}

			// Joint entry fraction j(t): grows linearly from jointFractionMin (0.5) to jointFractionMax (1.0)
			// over [graceSec, graceSec + growSec].
			double JointFraction(int64_t timeUs) const
			{
				const double stable = started_ ? static_cast<double>(timeUs - stableSinceUs_) / 1e6 : 0.0;
				const double raw = (std::isfinite(s_.growSec) && s_.growSec > 0.0) ? (stable - s_.graceSec) / s_.growSec : 0.0;
				const double frac = std::isfinite(raw) ? ClampD(raw, 0.0, 1.0) : 0.0;
				return s_.jointFractionMin + (s_.jointFractionMax - s_.jointFractionMin) * frac;
			}

			JointLatchOutput Reset()
			{
				JointLatchOutput o;
				if (!s_.enabled)
				{
					o.reason = JointLatchReason::Disabled;
					return o;
				}
				if (latched_)
				{
					o.event = Release(JointLatchEvent::ReleasedReset);
					o.reason = JointLatchReason::ReleasedReset;
					o.reasonBits = kReasonLatchRelease;
				}
				started_ = false;
				havePrev_ = false;
				detailAboveActive_ = false;
				detailAboveSinceUs_ = -1;
				Fill(o);
				return o;
			}

			JointLatchOutput Step(const JointLatchInput& in)
			{
				JointLatchOutput o;
				if (!s_.enabled)
				{
					o.reason = JointLatchReason::Disabled;
					return o;
				}
				// HIGH-3: validate c1 is finite and strictly positive before any threshold / bar / debt math.
				if (!std::isfinite(in.c1) || !(in.c1 > 0.0))
				{
					++c_.invalidInputs;
					o.reason = JointLatchReason::InvalidInput;
					Fill(o);
					return o;
				}

				const int64_t t = in.timeUs;
				if (!started_)
				{
					started_ = true;
					stableSinceUs_ = t;
				}
				if (havePrev_ && t < prevUs_)
				{
					// L5 backward source-time step without Reset: re-anchor clocks visibly.
					stableSinceUs_ = t;
					if (latched_)
						lastConfUs_ = t;
					newestX_ = newestY_ = -1;
					detailAboveActive_ = false;
					detailAboveSinceUs_ = -1;
					if (deferred_)
					{
						deferUs_ = t;
						dwellStartUs_ = t;
						dwellExtra_ = 0;
					}
					o.reason = JointLatchReason::BackwardTimeReanchored;
				}
				const double dt = (havePrev_ && t > prevUs_) ? static_cast<double>(t - prevUs_) / 1e6 : 0.0;
				prevUs_ = t;
				havePrev_ = true;

				// M2: Classical A is strictly VOID during menu, mixed content, subtitles, or fades.
				const bool classicalVoid = in.menuHold || in.mixedContent || in.subtitleOverlay || in.inFade;
				if (classicalVoid && (in.classicalValid || in.classicalDetailAbove))
				{
					++c_.classicalVoided;
					o.classicalVoided = true;
				}

				// R7: Menu/overlay HOLD immediately releases any active latch and blocks all entries.
				if (in.menuHold)
				{
					if (latched_)
					{
						o.event = Release(JointLatchEvent::ReleasedMenu);
						o.reason = JointLatchReason::ReleasedMenu;
						o.reasonBits |= kReasonLatchRelease;
					}
					else
					{
						// LOW-5: only report ClassicalVoided when a classical reading was actually voided.
						o.reason = o.classicalVoided ? JointLatchReason::ClassicalVoided : JointLatchReason::MenuHoldActive;
					}
					stableSinceUs_ = t;
					detailAboveActive_ = false;
					detailAboveSinceUs_ = -1;
					o.reasonBits |= kReasonMenuHold;
					Fill(o);
					return o;
				}

				// Immediate release triggers while latched: ABR step-up (from ChainStepMark) or chain native.
				if (latched_ && in.abrStepImmediateRelease)
				{
					o.event = Release(JointLatchEvent::ReleasedAbr);
					o.reason = JointLatchReason::ReleasedAbr;
					o.reasonBits |= kReasonAbrStep | kReasonLatchRelease;
					Fill(o);
					return o;
				}
				if (latched_ && in.chainNativeBoth)
				{
					o.event = Release(JointLatchEvent::ReleasedNative);
					o.reason = JointLatchReason::ReleasedNative;
					o.reasonBits |= kReasonLatchRelease;
					Fill(o);
					return o;
				}

				// M1: Debounced classical detail_above while latched (ignored when classical is void).
				if (latched_)
				{
					if (!classicalVoid && in.classicalDetailAbove)
					{
						if (!detailAboveActive_)
						{
							detailAboveActive_ = true;
							detailAboveSinceUs_ = t;
							++c_.detailAboveTransientIgnored;
							o.reason = JointLatchReason::DetailAboveTransient;
						}
						else
						{
							const double sustainedSec = static_cast<double>(t - detailAboveSinceUs_) / 1e6;
							if (sustainedSec >= s_.detailAboveDebounceSec)
							{
								o.event = Release(JointLatchEvent::ReleasedDetailAbove);
								o.reason = JointLatchReason::DetailAboveSustained;
								o.reasonBits |= kReasonLatchRelease;
								Fill(o);
								return o;
							}
							++c_.detailAboveTransientIgnored;
							o.reason = JointLatchReason::DetailAboveTransient;
						}
					}
					else
					{
						detailAboveActive_ = false;
						detailAboveSinceUs_ = -1;
					}
				}
				else
				{
					detailAboveActive_ = false;
					detailAboveSinceUs_ = -1;
				}

				// Evaluate candidate legality (R2: non-parked published conventional geometry ONLY).
				const bool chainPairValid = in.haveChainPair && in.chainNx > 0 && in.chainNxDenom > 0 && in.chainNy > 0 && in.chainNyDenom > 0;
				const bool chainConv = chainPairValid &&
					Scaling::GeometrySet::AgreeingPair(in.chainNx, in.chainNxDenom, in.chainNy, in.chainNyDenom, in.rasterW, in.rasterH);

				const bool classicalActive = in.classicalValid && !classicalVoid &&
					in.classicalNx > 0 && in.classicalNxDenom > 0 && in.classicalNy > 0 && in.classicalNyDenom > 0;
				const bool classicalConv = classicalActive &&
					Scaling::GeometrySet::AgreeingPair(in.classicalNx, in.classicalNxDenom, in.classicalNy, in.classicalNyDenom, in.rasterW, in.rasterH);

				const double bar = Bar(t, in.c1);
				const double jNeed = JointFraction(t) * in.c1;

				// HIGH-3: guard every std::min / threshold comparison against NaN (IEEE std::min(10.0, NaN) == 10.0).
				const bool chainLlrFinite = std::isfinite(in.chainLlrX) && std::isfinite(in.chainLlrY) && std::isfinite(bar);
				const bool shotCreditFinite = std::isfinite(in.currentShotDwellSec) &&
					std::isfinite(in.currentShotCreditX) && std::isfinite(in.currentShotCreditY) && std::isfinite(jNeed);

				const bool chainAloneEligible = (s_.entryPolicy == JointEntryPolicy::ChainOrJoint) &&
					chainConv &&
					chainLlrFinite &&
					in.chainShots >= s_.chainAloneMinShots &&
					(std::min)(in.chainLlrX, in.chainLlrY) >= bar;

				const bool jointEligible = classicalConv &&
					!in.classicalDetailAbove &&
					shotCreditFinite &&
					in.currentShotDwellSec >= s_.minShotDwellSec &&
					(std::min)(in.currentShotCreditX, in.currentShotCreditY) >= jNeed;

				if (!latched_)
				{
					if (in.stepMarkPending)
					{
						if (chainPairValid || classicalActive)
						{
							++c_.blockedByStepMark;
							o.heldBack = true;
							o.reason = JointLatchReason::BlockedByStepMark;
							o.reasonBits |= kReasonLatchBar | kReasonAbrStep;
						}
						Fill(o);
						return o;
					}

					// Reject parked anamorphic or non-conventional pairs explicitly.
					if ((chainPairValid && !chainConv) || (classicalActive && !classicalConv))
					{
						// If one path is valid and eligible, let it proceed; otherwise record non-conventional refusal.
						if (!chainAloneEligible && !jointEligible)
						{
							++c_.nonConventional;
							o.nonConventional = true;
							o.heldBack = true;
							o.reason = JointLatchReason::NonConventionalPair;
							o.reasonBits |= kReasonNonConventional | kReasonLatchBar;
							Fill(o);
							return o;
						}
					}

					if (chainAloneEligible)
					{
						LatchPair(in.chainNx, in.chainNxDenom, in.chainNy, in.chainNyDenom, JointLatchEntryPath::ChainAlone);
						o.event = JointLatchEvent::EnteredChainAlone;
						o.entryPath = JointLatchEntryPath::ChainAlone;
						o.reason = JointLatchReason::EnteredChainAlone;
						++c_.entriesChainAlone;
						stableSinceUs_ = t;
						Anchor(t);
					}
					else if (jointEligible)
					{
						LatchPair(in.classicalNx, in.classicalNxDenom, in.classicalNy, in.classicalNyDenom, JointLatchEntryPath::Joint);
						o.event = JointLatchEvent::EnteredJoint;
						o.entryPath = JointLatchEntryPath::Joint;
						o.reason = JointLatchReason::EnteredJoint;
						++c_.entriesJoint;
						stableSinceUs_ = t;
						Anchor(t);
					}
					else if (classicalConv)
					{
						// OD-1: Classical alone NEVER enters — log and count visibly as "would-be (classical only)".
						++c_.wouldBeClassicalOnly;
						o.classicalOnlyBlocked = true;
						o.heldBack = true;
						o.reason = JointLatchReason::WouldBeClassicalOnly;
						o.reasonBits |= kReasonLatchBar;
					}
					else if (chainConv)
					{
						++c_.heldBack;
						o.heldBack = true;
						o.reason = JointLatchReason::HeldBackByBar;
						o.reasonBits |= kReasonLatchBar;
					}

					Fill(o);
					return o;
				}

				// ====================================================================================================
				// Currently LATCHED: update chain credits, starvation clock, G1/G2 hard bound, and moves.
				// ====================================================================================================
				const double need = s_.confirmFraction * in.c1;
				// HIGH-3: never let NaN gainX/gainY poison or silently reset accX_/accY_ to -need via std::max.
				if (std::isfinite(in.gainX))
				{
					accX_ = (std::max)(-need, accX_ + in.gainX);
					if (in.gainX > 0.0 && in.gainTimeX > newestX_)
						newestX_ = in.gainTimeX;
				}
				else
				{
					++c_.invalidInputs;
				}
				if (std::isfinite(in.gainY))
				{
					accY_ = (std::max)(-need, accY_ + in.gainY);
					if (in.gainY > 0.0 && in.gainTimeY > newestY_)
						newestY_ = in.gainTimeY;
				}
				else
				{
					++c_.invalidInputs;
				}

				const bool chainConsistent = (std::isfinite(in.rawX) && in.rawX > 0.0) ||
					(std::isfinite(in.rawY) && in.rawY > 0.0);
				// LOW-5: a frame reporting detail_above contradicts the latched upscale and must NOT pause the 60 s clock.
				const bool classicalAgreesLatched = classicalConv && !in.classicalDetailAbove && PairsMatch(
					in.classicalNx, in.classicalNxDenom, in.classicalNy, in.classicalNyDenom,
					lnx_, lNx_, lny_, lNy_);

				if (accX_ >= need && accY_ >= need)
				{
					// Fresh Chain proof: resets BOTH the 60 s starvation clock AND the 120 s hard bound (and G2 renewal flag).
					int64_t ev = (std::min)(newestX_, newestY_);
					if (ev < 0 || ev > t)
						ev = t;
					lastConfUs_ = (std::max)(lastConfUs_, ev);
					accX_ = accY_ = 0.0;
					newestX_ = newestY_ = -1;
					clock_ = 0.0;
					g2RenewedThisBound_ = false;
					++c_.confirmsChain;
					o.confirmed = true;
					o.reason = JointLatchReason::ChainReconfirmed;
					if (deferred_)
					{
						ClearDeferral();
						++c_.deferCancelled;
						o.deferCancelled = true;
						o.reason = JointLatchReason::StarvationDeferCancelled;
					}
				}
				else
				{
					// Starvation clock advancement (60 s shorter bound):
					// H3 rule: NO classical hold-assist while B is informative and non-crediting!
					const bool bInformativeNonCrediting = in.chainInformative && !chainConsistent;
					if (classicalAgreesLatched && !bInformativeNonCrediting)
					{
						// Classical pauses the shorter 60 s starvation clock (never moves 120 s hard bound!).
						++c_.classicalHoldPauses;
						if (o.reason == JointLatchReason::None)
							o.reason = JointLatchReason::ClassicalPausedStarvation;
					}
					else
					{
						if (classicalAgreesLatched && bInformativeNonCrediting)
						{
							++c_.classicalHoldDeniedByB;
							if (o.reason == JointLatchReason::None)
								o.reason = JointLatchReason::ClassicalAssistDeniedByB;
						}
						clock_ += dt * (bInformativeNonCrediting ? 1.0 : s_.absentClockRate);
					}
				}

				// Check move to another conventional size (blocked while ABR step mark is pending).
				const bool canMoveChain = !in.stepMarkPending && chainAloneEligible &&
					DiffersFromLatched(in.chainNx, in.chainNxDenom, in.chainNy, in.chainNyDenom);
				const bool canMoveJoint = !in.stepMarkPending && jointEligible &&
					DiffersFromLatched(in.classicalNx, in.classicalNxDenom, in.classicalNy, in.classicalNyDenom);

				if (canMoveChain || canMoveJoint)
				{
					if (canMoveChain)
						LatchPair(in.chainNx, in.chainNxDenom, in.chainNy, in.chainNyDenom, JointLatchEntryPath::ChainAlone);
					else
						LatchPair(in.classicalNx, in.classicalNxDenom, in.classicalNy, in.classicalNyDenom, JointLatchEntryPath::Joint);
					o.event = JointLatchEvent::Moved;
					o.reason = JointLatchReason::Moved;
					++c_.moves;
					stableSinceUs_ = t;
					Anchor(t);
					ClearDeferral();
					Fill(o);
					return o;
				}
				if (in.stepMarkPending && ((chainAloneEligible && DiffersFromLatched(in.chainNx, in.chainNxDenom, in.chainNy, in.chainNyDenom)) ||
					(jointEligible && DiffersFromLatched(in.classicalNx, in.classicalNxDenom, in.classicalNy, in.classicalNyDenom))))
				{
					++c_.blockedByStepMark;
					o.heldBack = true;
					o.reason = JointLatchReason::BlockedByStepMark;
					o.reasonBits |= kReasonLatchBar | kReasonAbrStep;
				}

				// Hard bound & starvation checks:
				if (deferred_)
				{
					StepDeferred(in, t, o);
				}
				else
				{
					const double sinceConfSec = static_cast<double>(t - lastConfUs_) / 1e6;
					if (sinceConfSec >= s_.hardBoundSec)
					{
						// OD-2 G2 + HIGH-3 + MEDIUM-4: May renew ONCE per bound ONLY on fresh joint re-proof at the
						// full entry standard, with finite credits and timestamp strictly in (lastConfUs_, t] AND
						// earned at/after the bound OR within the current minShotDwellSec (3 s) window (never 119 s stale).
						const int64_t boundUs = lastConfUs_ + static_cast<int64_t>(s_.hardBoundSec * 1e6);
						const bool creditFreshForBound = in.currentShotNewestCreditUs > lastConfUs_ &&
							in.currentShotNewestCreditUs <= t &&
							(in.currentShotNewestCreditUs >= boundUs ||
								static_cast<double>(t - in.currentShotNewestCreditUs) / 1e6 <= s_.minShotDwellSec);
						const bool freshJointReproof = (s_.holdPolicy == JointHoldPolicy::G2_JointOncePerBound) &&
							!g2RenewedThisBound_ &&
							classicalAgreesLatched &&
							shotCreditFinite &&
							in.currentShotDwellSec >= s_.minShotDwellSec &&
							(std::min)(in.currentShotCreditX, in.currentShotCreditY) >= jNeed &&
							creditFreshForBound;

						if (freshJointReproof)
						{
							lastConfUs_ = t;
							clock_ = 0.0;
							accX_ = accY_ = 0.0;
							newestX_ = newestY_ = -1;
							g2RenewedThisBound_ = true;
							++c_.g2JointRenewals;
							o.g2Renewed = true;
							o.confirmed = true;
							o.reason = JointLatchReason::G2JointRenewal;
						}
						else
						{
							if (s_.holdPolicy == JointHoldPolicy::G2_JointOncePerBound && g2RenewedThisBound_)
								o.reason = JointLatchReason::G2RenewalExhausted;
							Starved(JointLatchEvent::ReleasedHardBound, in, t, o);
						}
					}
					else if (clock_ >= s_.releaseBoundSec)
					{
						Starved(JointLatchEvent::ReleasedStarvation, in, t, o);
					}
				}

				Fill(o);
				return o;
			}

		private:
			bool PairsMatch(int nx1, int Nx1, int ny1, int Ny1, int nx2, int Nx2, int ny2, int Ny2) const
			{
				if (Nx1 <= 0 || Ny1 <= 0 || Nx2 <= 0 || Ny2 <= 0)
					return false;
				const double rx1 = static_cast<double>(nx1) / static_cast<double>(Nx1);
				const double ry1 = static_cast<double>(ny1) / static_cast<double>(Ny1);
				const double rx2 = static_cast<double>(nx2) / static_cast<double>(Nx2);
				const double ry2 = static_cast<double>(ny2) / static_cast<double>(Ny2);
				return std::fabs(rx1 - rx2) <= s_.pairMatchTolerance * (std::max)(rx2, 1e-6) &&
					std::fabs(ry1 - ry2) <= s_.pairMatchTolerance * (std::max)(ry2, 1e-6);
			}

			bool DiffersFromLatched(int nx, int Nx, int ny, int Ny) const
			{
				if (!latched_ || Nx <= 0 || Ny <= 0 || lNx_ <= 0 || lNy_ <= 0)
					return false;
				const double lrx = static_cast<double>(lnx_) / static_cast<double>(lNx_);
				const double lry = static_cast<double>(lny_) / static_cast<double>(lNy_);
				const double rx = static_cast<double>(nx) / static_cast<double>(Nx);
				const double ry = static_cast<double>(ny) / static_cast<double>(Ny);
				return std::fabs(rx - lrx) > s_.movedTolerance * lrx ||
					std::fabs(ry - lry) > s_.movedTolerance * lry;
			}

			void Starved(JointLatchEvent cause, const JointLatchInput& in, int64_t t, JointLatchOutput& o)
			{
				if (cause == JointLatchEvent::ReleasedHardBound)
					++c_.releasesHardBound;
				else
					++c_.releasesStarvation;

				if (!s_.deferStarvation)
				{
					o.event = Release(cause, false);
					o.reason = (cause == JointLatchEvent::ReleasedHardBound)
						? JointLatchReason::ReleasedHardBound
						: JointLatchReason::ReleasedStarvation;
					o.reasonBits |= kReasonLatchRelease;
					return;
				}
				deferred_ = true;
				deferCause_ = cause;
				deferUs_ = t;
				dwelling_ = false;
				dwellExtra_ = 0;
				++c_.deferred;
				o.deferredNow = true;
				if (o.reason != JointLatchReason::G2RenewalExhausted)
					o.reason = JointLatchReason::StarvationDeferred;
				StepDeferred(in, t, o);
			}

			void StepDeferred(const JointLatchInput& in, int64_t t, JointLatchOutput& o)
			{
				// H3 strict invariant (HIGH-5): enforce BOTH deferral-relative ceiling (t - deferUs >= deferCeilingSec)
				// AND total anchor-relative ceiling (t - lastConfUs >= hardBoundSec + deferCeilingSec = 180 s), so
				// neither 5 s latched cadence overshoot nor eval gaps can ever push release past 180 s!
				const bool pastDeferCeiling = static_cast<double>(t - deferUs_) / 1e6 >= s_.deferCeilingSec;
				const bool pastHardTotalCeiling = static_cast<double>(t - lastConfUs_) / 1e6 >= (s_.hardBoundSec + s_.deferCeilingSec);
				const bool pastCeiling = pastDeferCeiling || pastHardTotalCeiling;
				if (pastCeiling)
				{
					++c_.releasesCeiling;
					o.event = Release(JointLatchEvent::ReleasedCeiling, false);
					o.reason = JointLatchReason::ReleasedCeiling;
					o.reasonBits |= kReasonLatchRelease;
					return;
				}
				if (in.shotBoundary)
				{
					dwelling_ = true;
					dwellStartUs_ = t;
					dwellExtra_ = 0;
				}
				if (dwelling_)
				{
					if (static_cast<double>(t - dwellStartUs_) / 1e6 >= s_.deferDwellSec)
					{
						if (dwellExtra_ >= s_.deferDwellExtraEvaluations)
						{
							++c_.releasesAtCut;
							o.event = Release(deferCause_, false);
							o.reason = (deferCause_ == JointLatchEvent::ReleasedHardBound)
								? JointLatchReason::ReleasedHardBound
								: JointLatchReason::ReleasedStarvation;
							o.reasonBits |= kReasonLatchRelease;
						}
						else
						{
							++dwellExtra_;
						}
					}
				}
			}

			void LatchPair(int nx, int Nx, int ny, int Ny, JointLatchEntryPath path)
			{
				latched_ = true;
				lnx_ = nx; lNx_ = Nx;
				lny_ = ny; lNy_ = Ny;
				entryPath_ = path;
			}

			void Anchor(int64_t t)
			{
				lastConfUs_ = t;
				clock_ = 0.0;
				accX_ = accY_ = 0.0;
				newestX_ = newestY_ = -1;
				g2RenewedThisBound_ = false;
				detailAboveActive_ = false;
				detailAboveSinceUs_ = -1;
			}

			void ClearDeferral()
			{
				deferred_ = false;
				dwelling_ = false;
				dwellExtra_ = 0;
			}

			JointLatchEvent Release(JointLatchEvent cause, bool countCause = true)
			{
				latched_ = false;
				entryPath_ = JointLatchEntryPath::None;
				clock_ = 0.0;
				accX_ = accY_ = 0.0;
				newestX_ = newestY_ = -1;
				g2RenewedThisBound_ = false;
				detailAboveActive_ = false;
				detailAboveSinceUs_ = -1;
				ClearDeferral();
				++c_.releases;
				if (countCause)
				{
					switch (cause)
					{
					case JointLatchEvent::ReleasedNative: ++c_.releasesNative; break;
					case JointLatchEvent::ReleasedDetailAbove: ++c_.releasesDetailAbove; break;
					case JointLatchEvent::ReleasedMenu: ++c_.releasesMenu; break;
					case JointLatchEvent::ReleasedAbr: ++c_.releasesAbr; break;
					case JointLatchEvent::ReleasedReset: ++c_.releasesReset; break;
					default: break;
					}
				}
				stableSinceUs_ = prevUs_;
				return cause;
			}

			void Fill(JointLatchOutput& o) const
			{
				o.latched = latched_;
				o.nx = lnx_; o.Nx = lNx_;
				o.ny = lny_; o.Ny = lNy_;
				o.entryPath = entryPath_;
				o.deferred = deferred_;
			}

			JointLatchSettings s_;
			JointLatchCounters c_;
			bool latched_ = false;
			bool started_ = false;
			bool havePrev_ = false;
			JointLatchEntryPath entryPath_ = JointLatchEntryPath::None;
			int lnx_ = 0, lNx_ = 1, lny_ = 0, lNy_ = 1;
			int64_t prevUs_ = 0, stableSinceUs_ = 0, lastConfUs_ = 0;
			double clock_ = 0.0;
			double accX_ = 0.0, accY_ = 0.0;
			int64_t newestX_ = -1, newestY_ = -1;
			bool g2RenewedThisBound_ = false;
			bool deferred_ = false, dwelling_ = false;
			JointLatchEvent deferCause_ = JointLatchEvent::None;
			int64_t deferUs_ = 0, dwellStartUs_ = 0;
			int dwellExtra_ = 0;
			bool detailAboveActive_ = false;
			int64_t detailAboveSinceUs_ = -1;
		};
	}
}
