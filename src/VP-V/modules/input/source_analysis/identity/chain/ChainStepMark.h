// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainStepMark (SID Stage S1, doc 100 s2.5, FM-6): fast ABR mid-stream size-step sentinel.
//
// When an adaptive-bitrate stream switches renditions mid-play (e.g. 1080p <-> 540p), pre-step evidence sits in the
// open shot and the ~16 s recent window. AbrStepDetector detects resolution steps at +0.5 s (18/18 on R6) but emits
// 60-91/h steady-state false marks on static streams, so an ABR step is treated as a QUESTION, never a blind trigger:
//
//   1. MARK FAST: On an AbrStep with |ratioStep| >= minRatioStep at source time t0, enter Pending state:
//        - Freeze ENTER and MOVE in the latch (BlocksEntryOrMove() == true).
//        - Request pulling the next chain evaluation forward (PullNextEval() == true).
//        - Route all post-t0 evaluations into a bounded tentative bucket (TentativeBucket) instead of mutating
//          the committed evidence directly.
//   2. CONFIRM WITHIN 2 EVALS: The pending mark is confirmed if either:
//        - A post-t0 chain evaluation contradicts the pre-step / latched size (ConfirmedByChainContradiction), OR
//        - `classicalConfirmFrames` (2) consecutive fresh informative classical frames read a different conventional
//          size (ConfirmedByClassicalPair).
//   3. ACT BACKWARDS TO t0:
//        - Confirmed + truth is LARGER than currently latched size on either axis (harmful direction: re-basing too
//          low destroys real detail) -> ConfirmVoidAndRelease (immediate release, void all evidence/A readings < t0,
//          replay only ConfirmedPostBucket >= t0).
//        - Confirmed + truth is SMALLER than latched (or same / not latched) -> ConfirmVoidAndChallenge (void all
//          pre-t0 credit/A readings; if latched, open a 1-dwell + 1-eval challenge window before N1 release).
//        - Unconfirmed after `confirmWindowEvals` (2) post-t0 evaluations -> CancelAndMergeBucket with explicit reason
//          UnconfirmedTimeout; caller merges MergedBucket() back in exact arrival order, producing bit-identical
//          state to never having raised the mark (0 false releases, 0 false entries).
//
// Pure C++14, header-only, deterministic, no clock, no threads, no Windows headers. Default OFF (enabled = false).
// Every ignored, coalesced, timed-out, confirmed, or reset path sets an explicit StepMarkReason and counter (Rule 4).
#include "ChainCommon.h"
#include "ChainEvidence.h"

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
		enum class StepMarkState : int
		{
			Idle = 0,
			Pending = 1,
			Challenging = 2
		};

		enum class StepMarkAction : int
		{
			None = 0,
			MarkPending,             // Pending mark opened at t0; block ENTER/MOVE and bucket post-t0 evals
			CancelAndMergeBucket,    // Mark unconfirmed after confirmWindowEvals; merge MergedBucket() back identically
			ConfirmVoidOnly,         // Confirmed while unlatched: void pre-t0 credit/A readings, keep post-t0 bucket
			ConfirmVoidAndRelease,   // Confirmed while latched toward LARGER truth: void pre-t0 and release IMMEDIATELY
			ConfirmVoidAndChallenge, // Confirmed while latched toward SMALLER truth: void pre-t0 and start challenge dwell
			ChallengeExpiredRelease  // Step-down challenge dwell + extra eval elapsed without re-confirm: N1 release
		};

		enum class StepMarkReason : int
		{
			None = 0,
			Disabled,
			InvalidInput,
			BelowRatioStep,
			AlreadyPending,
			MarkRaised,
			ConfirmedByChainContradiction,
			ConfirmedByClassicalPair,
			UnconfirmedTimeout,
			ChallengeOpened,
			ChallengeClearedByReconfirm,
			ChallengeExpired,
			ResetOnCut,
			ResetDiscontinuity,
			BackwardTimeReset
		};

		inline const char* StepMarkReasonText(StepMarkReason r)
		{
			switch (r)
			{
			case StepMarkReason::None: return "none";
			case StepMarkReason::Disabled: return "step mark disabled";
			case StepMarkReason::InvalidInput: return "rejected (invalid input)";
			case StepMarkReason::BelowRatioStep: return "ignored (ratio step below threshold)";
			case StepMarkReason::AlreadyPending: return "coalesced (mark already pending)";
			case StepMarkReason::MarkRaised: return "pending ABR step mark raised";
			case StepMarkReason::ConfirmedByChainContradiction: return "confirmed by chain contradiction";
			case StepMarkReason::ConfirmedByClassicalPair: return "confirmed by consecutive classical readings";
			case StepMarkReason::UnconfirmedTimeout: return "unconfirmed within 2-eval window (bucket merged)";
			case StepMarkReason::ChallengeOpened: return "step-down challenge opened";
			case StepMarkReason::ChallengeClearedByReconfirm: return "challenge cleared by re-confirmation";
			case StepMarkReason::ChallengeExpired: return "challenge dwell expired (release)";
			case StepMarkReason::ResetOnCut: return "reset on cut";
			case StepMarkReason::ResetDiscontinuity: return "reset on discontinuity";
			case StepMarkReason::BackwardTimeReset: return "reset on backward source time";
			default: return "unknown";
			}
		}

		struct StepMarkSettings
		{
			bool enabled = false;  // OFF by default (Stage S1 safe contract)
			// Derived from AbrStepDetector::Settings::minRatioStep (AbrStepDetector.h:92): smallest ratio step
			// counted as a concurrent resolution change.
			double minRatioStep = 0.10;
			// Derived from doc 100 s2.5: 1st post-t0 eval may straddle the ABR segment boundary; 2nd eval is
			// purely post-t0, so 2 evaluations is the minimal deterministic confirmation window.
			int confirmWindowEvals = 2;
			// Derived from minimal temporal persistence: 1 frame can be a transient/cut artifact; 2 consecutive
			// fresh informative classical frames at a different conventional pair confirm a size step.
			int classicalConfirmFrames = 2;
			// Derived from Scaling::GeometrySet::kConventionTolerance (SourceGeometrySet.h:41): relative ratio
			// tolerance distinguishing two conventional sizes.
			double ratioChangeTolerance = Scaling::GeometrySet::kConventionTolerance;
			// Derived from ChainEvidence::kMinShotDwellSec (ChainEvidence.h:72) and ChainLatch N2
			// (deferDwellExtraEvaluations = 1): 1 shot dwell + 1 extra eval for step-down challenge.
			double challengeDwellSec = kMinShotDwellSec;
			int challengeExtraEvals = 1;
			// Backing-ring bound (Rule 2): bounded bucket capacity = 4 * confirmWindowEvals (8 by default, clamped >= 4).
			// Prevents unbounded vector growth if caller feeds extra records before timeout.
			size_t maxBucketRecords = 8;
		};

		// Deterministic record stored in the tentative bucket while a mark is pending.
		// Merging these records back on unconfirmed timeout restores bit-exact state vs no-mark.
		struct TentativeEvalRecord
		{
			int64_t timeUs = 0;
			uint64_t evalSeq = 0;
			int nx = 0, Nx = 0, ny = 0, Ny = 0;
			double creditX = 0.0, creditY = 0.0;
			bool chainContradicts = false;   // true when chain eval contradicts latched/pre-step size or sees native/new size
			bool chainSeesNative = false;    // true when chain eval reads confident native (ratio = 1.0 > any upscaled latch)
			bool hasChainPair = false;
			int rasterW = 0, rasterH = 0;
		};

		// Fresh per-frame informative classical reading fed during a pending mark.
		struct StepMarkClassicalFrame
		{
			int64_t timeUs = 0;
			bool informative = false;
			bool voided = false;             // true during menu/mixed/subtitle/fade (M2: ignored, never confirms)
			bool nativeReading = false;      // classical reads native 1.0x1.0
			bool hasPair = false;
			int nx = 0, Nx = 0, ny = 0, Ny = 0;
			int rasterW = 0, rasterH = 0;
		};

		// Current latch snapshot supplied by the caller so StepMark can distinguish step-up vs step-down.
		struct StepMarkLatchContext
		{
			bool latched = false;
			int nx = 0, Nx = 0, ny = 0, Ny = 0;
		};

		struct StepMarkOutput
		{
			StepMarkState state = StepMarkState::Idle;
			StepMarkAction action = StepMarkAction::None;
			StepMarkReason reason = StepMarkReason::None;
			bool blocksEntryOrMove = false;
			bool pullNextEval = false;
			int64_t actionFromTimeUs = -1;   // t0 for backward voiding when action is ConfirmVoid*
			uint32_t reasonBits = 0;         // Chain::kReason*
			std::vector<TentativeEvalRecord> bucket;  // MergedBucket on CancelAndMergeBucket, or PostBucket on ConfirmVoid*
		};

		struct StepMarkCounters
		{
			uint64_t marksRaised = 0;
			uint64_t ignoredBelowRatioStep = 0;
			uint64_t coalescedWhilePending = 0;
			uint64_t rejectedInvalidInput = 0;
			uint64_t confirmedByChain = 0;
			uint64_t confirmedByClassical = 0;
			uint64_t immediateReleases = 0;
			uint64_t challengesOpened = 0;
			uint64_t challengesCleared = 0;
			uint64_t challengesResumedAfterUnconfirmed = 0;
			uint64_t challengeReleases = 0;
			uint64_t unconfirmedTimeouts = 0;
			uint64_t bucketOverflowDrops = 0;
			uint64_t resetsOnCut = 0;
			uint64_t resetsDiscontinuity = 0;
			uint64_t resetsBackwardTime = 0;
		};

		class ChainStepMark
		{
		public:
			ChainStepMark() {}
			explicit ChainStepMark(const StepMarkSettings& s) : s_(s) {}

			const StepMarkSettings& Settings() const { return s_; }
			const StepMarkCounters& Counters() const { return c_; }
			StepMarkState State() const { return state_; }
			StepMarkReason LastReason() const { return lastReason_; }
			bool IsPending() const { return state_ == StepMarkState::Pending; }
			bool IsChallenging() const { return state_ == StepMarkState::Challenging; }
			bool BlocksEntryOrMove() const { return s_.enabled && state_ == StepMarkState::Pending; }
			bool PullNextEval() const { return s_.enabled && state_ == StepMarkState::Pending; }
			int64_t MarkTimeUs() const { return markTimeUs_; }
			int PendingEvalCount() const { return pendingEvals_; }
			const std::vector<TentativeEvalRecord>& PendingBucket() const { return bucket_; }

			// Clean reset on scene cut or stream discontinuity (Rule 4: explicit reason & counter).
			StepMarkOutput Reset(StepMarkReason reason = StepMarkReason::ResetDiscontinuity)
			{
				StepMarkOutput o;
				if (!s_.enabled)
				{
					lastReason_ = StepMarkReason::Disabled;
					o.reason = lastReason_;
					return o;
				}
				if (reason == StepMarkReason::ResetOnCut)
					++c_.resetsOnCut;
				else if (reason == StepMarkReason::BackwardTimeReset)
					++c_.resetsBackwardTime;
				else
				{
					reason = StepMarkReason::ResetDiscontinuity;
					++c_.resetsDiscontinuity;
				}
				ClearInternal();
				lastReason_ = reason;
				o.state = state_;
				o.action = StepMarkAction::None;
				o.reason = reason;
				return o;
			}

			// Raise a fast pending mark from an AbrStep event at source time t0 with signed ratioStep.
			StepMarkOutput OnAbrStep(int64_t t0Us, double ratioStep)
			{
				StepMarkOutput o;
				if (!s_.enabled)
				{
					lastReason_ = StepMarkReason::Disabled;
					o.reason = lastReason_;
					return o;
				}
				if (!std::isfinite(ratioStep) || t0Us < 0)
				{
					++c_.rejectedInvalidInput;
					lastReason_ = StepMarkReason::InvalidInput;
					o.state = state_;
					o.reason = lastReason_;
					o.blocksEntryOrMove = BlocksEntryOrMove();
					o.pullNextEval = PullNextEval();
					return o;
				}
				if (std::fabs(ratioStep) < s_.minRatioStep)
				{
					++c_.ignoredBelowRatioStep;
					lastReason_ = StepMarkReason::BelowRatioStep;
					o.state = state_;
					o.reason = lastReason_;
					o.blocksEntryOrMove = BlocksEntryOrMove();
					o.pullNextEval = PullNextEval();
					return o;
				}
				if (state_ == StepMarkState::Pending)
				{
					++c_.coalescedWhilePending;
					lastReason_ = StepMarkReason::AlreadyPending;
					o.state = state_;
					o.reason = lastReason_;
					o.blocksEntryOrMove = true;
					o.pullNextEval = true;
					o.actionFromTimeUs = markTimeUs_;
					return o;
				}
				// HIGH-2: preserve an active step-down challenge across a spurious ABR mark so unconfirmed noise
				// cannot silently wipe or reset the challenge deadline; if the new mark cancels unconfirmed, the
				// original challenge (with its original challengeStartUs_) resumes seamlessly.
				const bool hadChallenge = (state_ == StepMarkState::Challenging);
				const int64_t prevChallengeStartUs = challengeStartUs_;
				const int prevChallengeExtraSeen = challengeExtraSeen_;
				ClearInternal();
				if (hadChallenge)
				{
					savedChallengeActive_ = true;
					savedChallengeStartUs_ = prevChallengeStartUs;
					savedChallengeExtraSeen_ = prevChallengeExtraSeen;
				}
				state_ = StepMarkState::Pending;
				markTimeUs_ = t0Us;
				markRatioStep_ = ratioStep;
				++c_.marksRaised;
				lastReason_ = StepMarkReason::MarkRaised;
				o.state = state_;
				o.action = StepMarkAction::MarkPending;
				o.reason = lastReason_;
				o.blocksEntryOrMove = true;
				o.pullNextEval = true;
				o.actionFromTimeUs = markTimeUs_;
				o.reasonBits = kReasonAbrStep;
				return o;
			}

			// Feed a fresh per-frame classical reading while a mark is pending.
			// Two consecutive fresh non-void informative readings at a different conventional size (or native)
			// confirm the mark without waiting for the second chain evaluation.
			StepMarkOutput OnClassicalFrame(const StepMarkClassicalFrame& f, const StepMarkLatchContext& ctx)
			{
				StepMarkOutput o;
				if (!s_.enabled)
				{
					lastReason_ = StepMarkReason::Disabled;
					o.reason = lastReason_;
					return o;
				}
				if (state_ != StepMarkState::Pending)
				{
					o.state = state_;
					o.reason = StepMarkReason::None;
					return o;
				}
				if (f.timeUs < markTimeUs_)
				{
					// Pre-t0 frame cannot confirm a post-t0 mark (prevents stale pre-step A reading leakage).
					o.state = state_;
					o.blocksEntryOrMove = true;
					o.pullNextEval = true;
					o.actionFromTimeUs = markTimeUs_;
					return o;
				}
				if (!f.informative || f.voided)
				{
					// MEDIUM-2: a voided or uninformative frame breaks any consecutive-frame confirmation streak.
					classicalStreak_ = 0;
					o.state = state_;
					o.blocksEntryOrMove = true;
					o.pullNextEval = true;
					o.actionFromTimeUs = markTimeUs_;
					return o;
				}

				double rx = 1.0, ry = 1.0;
				bool validDiff = false;
				if (f.nativeReading)
				{
					// Native differs from any upscaled latch (and is strictly larger).
					validDiff = ctx.latched;
				}
				else if (f.hasPair && f.Nx > 0 && f.Ny > 0 && f.nx > 0 && f.ny > 0)
				{
					const bool conv = Scaling::GeometrySet::AgreeingPair(f.nx, f.Nx, f.ny, f.Ny, f.rasterW, f.rasterH);
					if (conv)
					{
						rx = static_cast<double>(f.nx) / static_cast<double>(f.Nx);
						ry = static_cast<double>(f.ny) / static_cast<double>(f.Ny);
						// MEDIUM-2: when unlatched with no known pre-step reference pair, a steady conventional
						// reading does NOT confirm a step (prevents self-voiding an acquiring 1080p stream).
						// Only confirm if latched (and differing from latched) OR an explicit pre-step reference
						// pair was provided in ctx and this reading differs from it.
						validDiff = (ctx.latched || HasReferencePair(ctx)) && DiffersFromReference(rx, ry, ctx);
					}
				}

				if (!validDiff)
				{
					classicalStreak_ = 0;
					o.state = state_;
					o.blocksEntryOrMove = true;
					o.pullNextEval = true;
					o.actionFromTimeUs = markTimeUs_;
					return o;
				}

				if (classicalStreak_ > 0 &&
					std::fabs(rx - classicalStreakRx_) <= s_.ratioChangeTolerance * (std::max)(rx, 1e-6) &&
					std::fabs(ry - classicalStreakRy_) <= s_.ratioChangeTolerance * (std::max)(ry, 1e-6))
				{
					++classicalStreak_;
				}
				else
				{
					classicalStreak_ = 1;
					classicalStreakRx_ = rx;
					classicalStreakRy_ = ry;
				}

				const int need = (std::max)(1, s_.classicalConfirmFrames);
				if (classicalStreak_ >= need)
				{
					++c_.confirmedByClassical;
					return ConfirmMark(f.timeUs, rx, ry, StepMarkReason::ConfirmedByClassicalPair, ctx);
				}

				o.state = state_;
				o.blocksEntryOrMove = true;
				o.pullNextEval = true;
				o.actionFromTimeUs = markTimeUs_;
				return o;
			}

			// Feed a chain evaluation.
			// - While Pending: buckets post-t0 records, checks contradiction/confirmation, or times out & merges bucket.
			// - While Challenging (step-down): tracks 1 dwell + 1 extra eval before releasing (or clears on re-confirm).
			StepMarkOutput OnChainEvaluation(const TentativeEvalRecord& rec, const StepMarkLatchContext& ctx, bool latchReconfirmed = false)
			{
				StepMarkOutput o;
				if (!s_.enabled)
				{
					lastReason_ = StepMarkReason::Disabled;
					o.reason = lastReason_;
					return o;
				}

				if (state_ == StepMarkState::Pending)
				{
					if (rec.timeUs < markTimeUs_)
					{
						// Non-monotonic backward time jump relative to pending mark: reset cleanly and visibly.
						return Reset(StepMarkReason::BackwardTimeReset);
					}
					AppendBucket(rec);
					++pendingEvals_;

					const bool chainDiffPair = rec.hasChainPair && rec.Nx > 0 && rec.Ny > 0 && rec.nx > 0 && rec.ny > 0 &&
						(ctx.latched || HasReferencePair(ctx)) &&
						DiffersFromReference(static_cast<double>(rec.nx) / rec.Nx, static_cast<double>(rec.ny) / rec.Ny, ctx);

					// HIGH-1: rec.chainSeesNative only contradicts an active upscaled latch (never a steady native stream).
					if (rec.chainContradicts || (ctx.latched && rec.chainSeesNative) || (ctx.latched && chainDiffPair))
					{
						double newRx = 1.0, newRy = 1.0;
						if (!rec.chainSeesNative && rec.hasChainPair && rec.Nx > 0 && rec.Ny > 0 && rec.nx > 0 && rec.ny > 0)
						{
							newRx = static_cast<double>(rec.nx) / static_cast<double>(rec.Nx);
							newRy = static_cast<double>(rec.ny) / static_cast<double>(rec.Ny);
						}
						else if (!rec.chainSeesNative && markRatioStep_ < 0.0 && ctx.latched && ctx.Nx > 0 && ctx.Ny > 0)
						{
							// Contradiction without a resolved new pair while ABR ratioStep was negative: step-down direction.
							newRx = (std::max)(0.0, static_cast<double>(ctx.nx) / ctx.Nx + markRatioStep_);
							newRy = (std::max)(0.0, static_cast<double>(ctx.ny) / ctx.Ny + markRatioStep_);
						}
						++c_.confirmedByChain;
						return ConfirmMark(rec.timeUs, newRx, newRy, StepMarkReason::ConfirmedByChainContradiction, ctx);
					}

					const int windowEvals = (std::max)(1, s_.confirmWindowEvals);
					if (pendingEvals_ >= windowEvals)
					{
						// Unconfirmed: hand back the exact bucket in arrival order so caller merges it back identically.
						o.action = StepMarkAction::CancelAndMergeBucket;
						o.reason = StepMarkReason::UnconfirmedTimeout;
						o.actionFromTimeUs = markTimeUs_;
						o.bucket = bucket_;
						++c_.unconfirmedTimeouts;
						// HIGH-2: resume any active challenge that was interrupted by this spurious mark (unless
						// the latch was already released or reconfirmed in the meantime).
						const bool resumeChallenge = savedChallengeActive_ && ctx.latched && !latchReconfirmed;
						const int64_t resumeStartUs = savedChallengeStartUs_;
						const int resumeExtra = savedChallengeExtraSeen_;
						ClearInternal();
						if (resumeChallenge)
						{
							state_ = StepMarkState::Challenging;
							challengeStartUs_ = resumeStartUs;
							challengeExtraSeen_ = resumeExtra;
							++c_.challengesResumedAfterUnconfirmed;
						}
						lastReason_ = StepMarkReason::UnconfirmedTimeout;
						o.state = state_;
						return o;
					}

					o.state = state_;
					o.blocksEntryOrMove = true;
					o.pullNextEval = true;
					o.actionFromTimeUs = markTimeUs_;
					return o;
				}

				if (state_ == StepMarkState::Challenging)
				{
					if (!ctx.latched || latchReconfirmed)
					{
						++c_.challengesCleared;
						ClearInternal();
						lastReason_ = StepMarkReason::ChallengeClearedByReconfirm;
						o.state = state_;
						o.reason = lastReason_;
						return o;
					}
					if (rec.timeUs < challengeStartUs_)
						return Reset(StepMarkReason::BackwardTimeReset);

					const double elapsedSec = static_cast<double>(rec.timeUs - challengeStartUs_) / 1e6;
					if (elapsedSec >= s_.challengeDwellSec)
					{
						if (challengeExtraSeen_ >= s_.challengeExtraEvals)
						{
							++c_.challengeReleases;
							ClearInternal();
							lastReason_ = StepMarkReason::ChallengeExpired;
							o.state = state_;
							o.action = StepMarkAction::ChallengeExpiredRelease;
							o.reason = lastReason_;
							o.reasonBits = kReasonAbrStep | kReasonLatchRelease;
							return o;
						}
						++challengeExtraSeen_;
					}
					o.state = state_;
					o.reason = StepMarkReason::ChallengeOpened;
					return o;
				}

				o.state = state_;
				o.reason = StepMarkReason::None;
				return o;
			}

		private:
			bool HasReferencePair(const StepMarkLatchContext& ctx) const
			{
				return ctx.nx > 0 && ctx.Nx > 0 && ctx.ny > 0 && ctx.Ny > 0;
			}

			bool DiffersFromReference(double rx, double ry, const StepMarkLatchContext& ctx) const
			{
				if (!HasReferencePair(ctx))
					return false;
				const double lrx = static_cast<double>(ctx.nx) / static_cast<double>(ctx.Nx);
				const double lry = static_cast<double>(ctx.ny) / static_cast<double>(ctx.Ny);
				return std::fabs(rx - lrx) > s_.ratioChangeTolerance * (std::max)(lrx, 1e-6) ||
					std::fabs(ry - lry) > s_.ratioChangeTolerance * (std::max)(lry, 1e-6);
			}

			bool IsLargerThanLatch(double rx, double ry, const StepMarkLatchContext& ctx) const
			{
				if (!ctx.latched || ctx.Nx <= 0 || ctx.Ny <= 0)
					return false;
				const double lrx = static_cast<double>(ctx.nx) / static_cast<double>(ctx.Nx);
				const double lry = static_cast<double>(ctx.ny) / static_cast<double>(ctx.Ny);
				return rx > lrx * (1.0 + s_.ratioChangeTolerance) ||
					ry > lry * (1.0 + s_.ratioChangeTolerance);
			}

			void AppendBucket(const TentativeEvalRecord& rec)
			{
				const size_t cap = s_.maxBucketRecords < 4 ? 4 : s_.maxBucketRecords;
				if (bucket_.size() >= cap)
				{
					// Bounded ring: never silently exceed cap; record overflow visibly (Rule 2 + Rule 4).
					bucket_.erase(bucket_.begin());
					++c_.bucketOverflowDrops;
				}
				bucket_.push_back(rec);
			}

			StepMarkOutput ConfirmMark(int64_t nowUs, double newRx, double newRy, StepMarkReason reason, const StepMarkLatchContext& ctx)
			{
				StepMarkOutput o;
				o.reason = reason;
				o.actionFromTimeUs = markTimeUs_;
				o.bucket = bucket_;
				o.reasonBits = kReasonAbrStep;

				if (!ctx.latched)
				{
					ClearInternal();
					lastReason_ = reason;
					o.state = state_;
					o.action = StepMarkAction::ConfirmVoidOnly;
					return o;
				}

				if (IsLargerThanLatch(newRx, newRy, ctx))
				{
					// Harmful direction (truth is larger than latched, e.g. 540p -> 1080p or -> native):
					// void pre-t0 credit and release IMMEDIATELY so real detail is not destroyed.
					++c_.immediateReleases;
					ClearInternal();
					lastReason_ = reason;
					o.state = state_;
					o.action = StepMarkAction::ConfirmVoidAndRelease;
					o.reasonBits |= kReasonLatchRelease;
					return o;
				}

				// Step-down direction (truth is smaller than latched): void pre-t0 and open challenge
				// (1 dwell + 1 extra eval before N1 release).
				ClearInternal();
				state_ = StepMarkState::Challenging;
				challengeStartUs_ = nowUs;
				challengeExtraSeen_ = 0;
				++c_.challengesOpened;
				lastReason_ = reason;
				o.state = state_;
				o.action = StepMarkAction::ConfirmVoidAndChallenge;
				return o;
			}

			void ClearInternal()
			{
				state_ = StepMarkState::Idle;
				markTimeUs_ = -1;
				markRatioStep_ = 0.0;
				pendingEvals_ = 0;
				classicalStreak_ = 0;
				classicalStreakRx_ = 0.0;
				classicalStreakRy_ = 0.0;
				challengeStartUs_ = -1;
				challengeExtraSeen_ = 0;
				savedChallengeActive_ = false;
				savedChallengeStartUs_ = -1;
				savedChallengeExtraSeen_ = 0;
				bucket_.clear();
			}

			StepMarkSettings s_;
			StepMarkCounters c_;
			StepMarkState state_ = StepMarkState::Idle;
			StepMarkReason lastReason_ = StepMarkReason::None;
			int64_t markTimeUs_ = -1;
			double markRatioStep_ = 0.0;
			int pendingEvals_ = 0;
			int classicalStreak_ = 0;
			double classicalStreakRx_ = 0.0, classicalStreakRy_ = 0.0;
			int64_t challengeStartUs_ = -1;
			int challengeExtraSeen_ = 0;
			bool savedChallengeActive_ = false;
			int64_t savedChallengeStartUs_ = -1;
			int savedChallengeExtraSeen_ = 0;
			std::vector<TentativeEvalRecord> bucket_;
		};
	}
}
