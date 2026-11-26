// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Top-level stateful orchestrator combining Stage A, Stage B, Stage B.5, Dormant/Active
// ROI-only scheduler, OverloadArbiter budget shedding, cut/flash/resync reset, and
// zero-cost OFF / OriginalPathway invariance.
#include "FastObjectTracker.h"

#include <algorithm>
#include <cmath>

namespace FastObjects
{
	FastObjectTracker::FastObjectTracker() = default;

	void FastObjectTracker::Reserve(int residualW, int residualH, int guardGridW, int guardGridH, int maxRefinePatch)
	{
		residualWs_.Reserve(residualW, residualH, guardGridW, guardGridH);
		refineWs_.Reserve(maxRefinePatch);
	}

	void FastObjectTracker::SetBudgetTier(SchedulerMode tier)
	{
		budgetTier_ = tier;
		if (tier == SchedulerMode::Off || tier == SchedulerMode::ShedBudget)
			currentMode_ = tier;
	}

	void FastObjectTracker::ResetScene()
	{
		trajectory_.ResetScene();
		quietFrames_ = 0;
		dormantTick_ = 0;
		stageARingCount_ = 0;
		for (int i = 0; i < kWindowRingCapacity; ++i)
			stageARing_[i] = StageADiagRecord{};
		lastStageADiag_ = StageADiagRecord{};
	}

	void FastObjectTracker::ResetSession()
	{
		ResetScene();
		trajectory_.ResetSession();
		hasResyncGeneration_ = false;
		lastResyncGeneration_ = 0;
		totalClutterOverflows_ = 0;
	}

	void FastObjectTracker::RecordStageADiag(const StageADiagRecord& rec)
	{
		lastStageADiag_ = rec;
		for (int i = 0; i < stageARingCount_; ++i)
		{
			if (stageARing_[i].valid && stageARing_[i].frameIndex == rec.frameIndex)
			{
				stageARing_[i] = rec;
				return;
			}
		}
		if (stageARingCount_ < kWindowRingCapacity)
		{
			stageARing_[stageARingCount_++] = rec;
		}
		else
		{
			for (int i = 0; i < kWindowRingCapacity - 1; ++i)
				stageARing_[i] = stageARing_[i + 1];
			stageARing_[kWindowRingCapacity - 1] = rec;
		}
	}

	const FastObjectTracker::StageADiagRecord* FastObjectTracker::FindStageADiag(uint64_t frameIndex) const
	{
		for (int i = stageARingCount_ - 1; i >= 0; --i)
		{
			if (stageARing_[i].valid && stageARing_[i].frameIndex == frameIndex)
				return &stageARing_[i];
		}
		return lastStageADiag_.valid ? &lastStageADiag_ : nullptr;
	}

	bool FastObjectTracker::CheckFastOffGate(const Config& cfg, FastObjectTrackSet* outTrackSet) const
	{
		if (cfg.originalPathway)
		{
			if (outTrackSet)
				outTrackSet->Clear(SchedulerMode::Off, AbstainReason::OriginalPathway);
			return true;
		}
		if (!cfg.enabled)
		{
			if (outTrackSet)
				outTrackSet->Clear(SchedulerMode::Off, AbstainReason::Disabled);
			return true;
		}
		if (budgetTier_ == SchedulerMode::Off || budgetTier_ == SchedulerMode::ShedBudget)
		{
			if (outTrackSet)
				outTrackSet->Clear(SchedulerMode::ShedBudget, AbstainReason::BudgetShed);
			return true;
		}
		return false;
	}

	bool FastObjectTracker::PushFrame(const Config& cfg, const TrackerFrameInput& in, ResidualOutput* outStageA)
	{
		if (outStageA)
			outStageA->Clear();

		if (CheckFastOffGate(cfg, nullptr))
		{
			currentMode_ = (budgetTier_ == SchedulerMode::ShedBudget) ? SchedulerMode::ShedBudget : SchedulerMode::Off;
			return false;
		}

		// Transition immediately out of Off once enabled & within budget (so Frame-0 ClutterOverflow reports accurately).
		const SchedulerMode activeTier = (budgetTier_ == SchedulerMode::Dormant)
			? SchedulerMode::Dormant
			: ((budgetTier_ == SchedulerMode::ActiveT2) ? SchedulerMode::ActiveT2 : SchedulerMode::ActiveT1);
		if (currentMode_ == SchedulerMode::Off || currentMode_ == SchedulerMode::ShedBudget)
			currentMode_ = activeTier;

		// Instant reset on U10 resync generation change or P4 cut/flash discontinuity.
		if (hasResyncGeneration_ && in.resyncGeneration != lastResyncGeneration_)
		{
			ResetScene();
		}
		lastResyncGeneration_ = in.resyncGeneration;
		hasResyncGeneration_ = true;

		if (in.residual.sceneDiscontinuity)
		{
			ResetScene();
			currentMode_ = activeTier;
			StageADiagRecord rec;
			rec.frameIndex = in.frameIndex;
			rec.abstainReason = AbstainReason::SceneDiscontinuity;
			rec.mode = currentMode_;
			rec.valid = true;
			RecordStageADiag(rec);
			if (outStageA)
				outStageA->abstainReason = AbstainReason::SceneDiscontinuity;
			return false;
		}

		// Dormant / Active ROI-only scheduler (spec C.4.1):
		// When quiet for >= dormantIdleFrames (default 6) with no active/coasting tracks (or when budgetTier_ == Dormant),
		// run full-frame wake-up scan every 2nd frame and skip odd frames cleanly.
		const bool wantDormant = (budgetTier_ == SchedulerMode::Dormant) ||
			(cfg.enableDormantScheduler && quietFrames_ >= cfg.dormantIdleFrames && !trajectory_.HasActiveOrCoastingTracks());
		if (wantDormant)
		{
			currentMode_ = SchedulerMode::Dormant;
			const bool skipThisFrame = ((dormantTick_++ & 1u) != 0u);
			if (skipThisFrame)
			{
				++quietFrames_;
				StageADiagRecord rec;
				rec.frameIndex = in.frameIndex;
				rec.abstainReason = in.residual.causalTwoBack ? AbstainReason::CausalNoLookAhead : AbstainReason::None;
				rec.scanSkippedDormant = true;
				rec.mode = SchedulerMode::Dormant;
				rec.valid = true;
				RecordStageADiag(rec);

				FrameObservation emptyObs;
				emptyObs.frameIndex = in.frameIndex;
				emptyObs.timestampSec = in.timestampSec;
				emptyObs.dtFromPrevSec = in.dtFromPrevSec;
				emptyObs.camPrevToCur = in.residual.camPrevToCur;
				emptyObs.camPrevUsable = in.residual.camPrevUsable;
				emptyObs.camSigmaPx = in.residual.camSigmaPx;
				emptyObs.picture = in.residual.picture;
				emptyObs.blobCount = 0;
				emptyObs.causalMode = in.residual.causalTwoBack;
				emptyObs.valid = true;
				trajectory_.PushFrame(cfg, emptyObs);
				return true;
			}
		}

		// Stage A: camera-compensated 3-frame residual + streak blob extraction.
		ResidualOutput stageA;
		const bool stageAOk = DetectFastObjectBlobs(cfg, in.residual, residualWs_, stageA);
		if (outStageA)
			*outStageA = stageA;

		if (stageA.clutterOverflow)
		{
			++totalClutterOverflows_;
			currentMode_ = activeTier;
			StageADiagRecord rec;
			rec.frameIndex = in.frameIndex;
			rec.activeCells = stageA.activeCells;
			rec.blobCount = stageA.blobCount;
			rec.droppedBlobs = stageA.droppedBlobs;
			rec.abstainReason = stageA.abstainReason;
			rec.clutterOverflow = true;
			rec.noiseSigmaEst = stageA.noiseSigmaEst;
			rec.mode = currentMode_;
			rec.valid = true;
			RecordStageADiag(rec);
			return false;
		}
		if (!stageAOk)
		{
			StageADiagRecord rec;
			rec.frameIndex = in.frameIndex;
			rec.activeCells = stageA.activeCells;
			rec.abstainReason = stageA.abstainReason;
			rec.noiseSigmaEst = stageA.noiseSigmaEst;
			rec.mode = currentMode_;
			rec.valid = true;
			RecordStageADiag(rec);
			return false;
		}

		// Wake immediately from Dormant on the first active cell trigger!
		if (stageA.activeCells > 0 || trajectory_.HasActiveOrCoastingTracks())
		{
			quietFrames_ = 0;
			dormantTick_ = 0;
			currentMode_ = activeTier;
		}
		else
		{
			++quietFrames_;
			if (cfg.enableDormantScheduler && quietFrames_ >= cfg.dormantIdleFrames)
				currentMode_ = SchedulerMode::Dormant;
			else
				currentMode_ = activeTier;
		}

		StageADiagRecord rec;
		rec.frameIndex = in.frameIndex;
		rec.activeCells = stageA.activeCells;
		rec.blobCount = stageA.blobCount;
		rec.droppedBlobs = stageA.droppedBlobs;
		rec.abstainReason = stageA.abstainReason;
		rec.clutterOverflow = false;
		rec.scanSkippedDormant = false;
		rec.noiseSigmaEst = stageA.noiseSigmaEst;
		rec.mode = currentMode_;
		rec.valid = true;
		RecordStageADiag(rec);

		// Stage B ingestion: push blobs into 13-frame ring and advance Hungarian association + bounce stitching.
		FrameObservation obs;
		obs.frameIndex = in.frameIndex;
		obs.timestampSec = in.timestampSec;
		obs.dtFromPrevSec = in.dtFromPrevSec;
		obs.camPrevToCur = in.residual.camPrevToCur;
		obs.camPrevUsable = in.residual.camPrevUsable;
		obs.camSigmaPx = in.residual.camSigmaPx;
		obs.picture = in.residual.picture;
		if (obs.picture.Empty() && in.residual.width > 0 && in.residual.height > 0 && in.residual.sourceScale > 0.0f)
		{
			obs.picture.left = 0;
			obs.picture.top = 0;
			obs.picture.right = static_cast<int>(std::lround(static_cast<double>(in.residual.width) * static_cast<double>(in.residual.sourceScale)));
			obs.picture.bottom = static_cast<int>(std::lround(static_cast<double>(in.residual.height) * static_cast<double>(in.residual.sourceScale)));
		}
		obs.blobCount = stageA.blobCount;
		for (uint32_t i = 0; i < stageA.blobCount; ++i)
			obs.blobs[i] = stageA.blobs[i];
		obs.causalMode = stageA.causalMode;
		obs.chromaUsed = stageA.chromaUsed;
		obs.valid = true;

		trajectory_.PushFrame(cfg, obs);
		return true;
	}

	bool FastObjectTracker::ResolveAt(
		const Config& cfg,
		uint64_t targetFrameIndex,
		const TrackerRefinePlanes* refinePlanes,
		FastObjectTrackSet& outTrackSet)
	{
		if (CheckFastOffGate(cfg, &outTrackSet))
			return false;

		const StageADiagRecord* frameDiag = FindStageADiag(targetFrameIndex);
		const SchedulerMode resolvedMode = frameDiag ? frameDiag->mode : currentMode_;
		const uint32_t resolvedAbstain = frameDiag ? frameDiag->abstainReason : AbstainReason::None;
		const bool resolvedOverflow = frameDiag ? frameDiag->clutterOverflow : false;

		outTrackSet.Clear(resolvedMode, resolvedAbstain);
		if (frameDiag)
		{
			outTrackSet.diag.activeCells = frameDiag->activeCells;
			outTrackSet.diag.blobCount = frameDiag->blobCount;
			outTrackSet.diag.droppedBlobs = frameDiag->droppedBlobs;
			outTrackSet.diag.clutterOverflow = frameDiag->clutterOverflow;
			outTrackSet.diag.scanSkippedDormant = frameDiag->scanSkippedDormant;
			outTrackSet.diag.noiseSigmaEst = frameDiag->noiseSigmaEst;
		}
		outTrackSet.diag.clutterOverflowCount = totalClutterOverflows_;
		outTrackSet.diag.quietFrames = quietFrames_;

		if (resolvedOverflow ||
			(resolvedAbstain & (AbstainReason::SceneDiscontinuity | AbstainReason::NoUsableCamera)) != 0u)
		{
			return false;
		}

		if (!trajectory_.ResolveAt(cfg, targetFrameIndex, outTrackSet))
			return false;

		// Stage B.5: ROI-only masked pyramid ZNCC refinement on confirmed Valid tracks.
		const bool canRefine = cfg.enableMaskedRefine &&
			refinePlanes != nullptr &&
			refinePlanes->lumaA != nullptr &&
			refinePlanes->lumaB != nullptr &&
			refinePlanes->width > 0 &&
			refinePlanes->height > 0;

		// Recompute validity counters cleanly after Stage B.5 confidence calibration.
		outTrackSet.diag.validTracks = 0;
		outTrackSet.diag.uncertainTracks = 0;
		outTrackSet.diag.coastingTracks = 0;
		outTrackSet.diag.ballisticTracks = 0;
		outTrackSet.diag.linearGlideTracks = 0;
		outTrackSet.diag.impulseTracks = 0;
		outTrackSet.diag.centroidFallbackTracks = 0;

		for (uint32_t i = 0; i < outTrackSet.count; ++i)
		{
			FastObjectTrack& trk = outTrackSet.tracks[i];

			if (trk.state == ValidityState::Valid && canRefine)
			{
				// Isoluminant chroma-driven tracks have no luma signal for luma-only ZNCC, and Impulse bounce-bracketing
				// pairs undergo discontinuous streak-angle changes across the bounce vertex: transparently keep the
				// trajectory-filtered centroid / two-leg chord displacement (kCentroidVector).
				if ((trk.flags & (TrackFlags::kIsoLumaChroma | TrackFlags::kImpulsePair)) != 0u)
				{
					trk.flags |= TrackFlags::kCentroidVector;
					trk.confidence = std::min(std::max(trk.confidence * 0.75f, 0.0f), 1.0f);
					++outTrackSet.diag.centroidFallbackTracks;
				}
				else
				{
					RefineInput rin;
					rin.lumaA = refinePlanes->lumaA;
					rin.lumaB = refinePlanes->lumaB;
					rin.width = refinePlanes->width;
					rin.height = refinePlanes->height;
					rin.planeScale = refinePlanes->planeScale;
					rin.camAToB = refinePlanes->camAToB;
					rin.camUsable = refinePlanes->camUsable;
					rin.picture = refinePlanes->picture;
					rin.cx = trk.cx;
					rin.cy = trk.cy;
					rin.predVx = trk.vx;
					rin.predVy = trk.vy;
					rin.radiusPx = trk.radiusPx;
					rin.streakLenPx = trk.streakLenPx;
					rin.streakAngleRad = trk.streakAngleRad;
					// Search uncertainty radius: ceil(3 * sigma_pred) + ceil(r_est), with sigma_pred derived from NIS/radius.
					const float sigmaPred = std::max(1.0f, std::sqrt(std::max(trk.nis, 1.0f)) * std::max(0.5f, 0.25f * trk.radiusPx));
					rin.searchRadiusPx = std::ceil(3.0f * sigmaPred) + std::ceil(trk.radiusPx);
					// Differential motion blur DeltaL = (||v_t|| - ||v_{t-1}||) * tau_s ~= a_parallel * dt^2 * tau_s.
					const float vMag = std::sqrt(trk.velXPerSec * trk.velXPerSec + trk.velYPerSec * trk.velYPerSec);
					if (vMag > 1e-3f && trk.shutterEst > 0.0f)
					{
						rin.blurAxisX = trk.velXPerSec / vMag;
						rin.blurAxisY = trk.velYPerSec / vMag;
						const float aPar = trk.ax * rin.blurAxisX + trk.ay * rin.blurAxisY;
						rin.deltaBlurLenPx = aPar * trk.pairSeconds * trk.pairSeconds * trk.shutterEst;
					}
					rin.maskBox = trk.maskBox;
					for (int bi = 0; bi < 4; ++bi)
						rin.maskBits[bi] = trk.maskBits[bi];
					rin.hasMaskBits = trk.hasMaskBits;

					RefineOutput rout;
					RefineFastObjectVector(rin, refineWs_, rout);

					const float gNis = std::exp(-std::max(0.0f, trk.nis - 2.0f) * 0.25f);
					const float gAge = std::min(1.0f, static_cast<float>(trk.ageFrames) / 3.0f);

					if (rout.usedCentroidFallback)
					{
						// Transparent kCentroidVector fallback with calibrated lower confidence (spec B.5 & C.3).
						trk.flags |= TrackFlags::kCentroidVector;
						trk.confidence = std::min(std::max(trk.confidence * 0.75f, 0.0f), 1.0f);
						++outTrackSet.diag.centroidFallbackTracks;
					}
					else
					{
						// Spec C.3 confidence: clamp((1 - cost_best / cost_mean) * g_NIS * g_blur * g_age * h_agree, 0, 1).
						const float predDiffX = rout.vx - rin.predVx;
						const float predDiffY = rout.vy - rin.predVy;
						const float predDiff = std::sqrt(predDiffX * predDiffX + predDiffY * predDiffY);
						const float hAgree = (predDiff > 2.0f * std::max(sigmaPred, 1.0f)) ? 0.5f : 1.0f;
						const float refinedConf = std::min(std::max(rout.matchMargin * gNis * trk.blurAlignFactor * gAge * hAgree, 0.0f), 1.0f);

						// If ZNCC slid along a motion-blurred streak OR matched globally (bestZncc >= 0.25) with weak
						// 5x5 peak sharpness (refinedConf < minConfidence while trajectory fit is solid), keep the
						// trajectory-filtered centroid displacement transparently (kCentroidVector).
						if (((trk.flags & TrackFlags::kBlurStretched) != 0u && hAgree < 1.0f) ||
							(refinedConf < cfg.minConfidence && rout.bestZncc >= 0.25f && trk.confidence * 0.75f >= cfg.minConfidence))
						{
							trk.flags |= TrackFlags::kCentroidVector;
							trk.confidence = std::min(std::max(trk.confidence * 0.75f, 0.0f), 1.0f);
							++outTrackSet.diag.centroidFallbackTracks;
						}
						else
						{
							trk.vx = rout.vx;
							trk.vy = rout.vy;
							trk.disocclusionCapsule.x1 = trk.cx + trk.vx;
							trk.disocclusionCapsule.y1 = trk.cy + trk.vy;
							trk.confidence = refinedConf;
						}
					}
				}

				if (trk.confidence < cfg.minConfidence)
				{
					trk.state = ValidityState::Uncertain;
					trk.vx = 0.0f;
					trk.vy = 0.0f;
					trk.disocclusionCapsule = Capsule{};
				}
			}
			else if (trk.state == ValidityState::Valid && !canRefine)
			{
				// No full-res refinement planes supplied: vector comes directly from trajectory centroid.
				trk.flags |= TrackFlags::kCentroidVector;
				++outTrackSet.diag.centroidFallbackTracks;
			}

			// Tally final track states & trajectory types.
			if (trk.state == ValidityState::Valid)
			{
				++outTrackSet.diag.validTracks;
				if (trk.type == TrajectoryType::Ballistic) ++outTrackSet.diag.ballisticTracks;
				else if (trk.type == TrajectoryType::LinearGlide) ++outTrackSet.diag.linearGlideTracks;
				else if (trk.type == TrajectoryType::Impulse) ++outTrackSet.diag.impulseTracks;
			}
			else if (trk.state == ValidityState::Coasting)
			{
				++outTrackSet.diag.coastingTracks;
			}
			else if (trk.state == ValidityState::Uncertain)
			{
				++outTrackSet.diag.uncertainTracks;
			}
		}

		return true;
	}

	bool FastObjectTracker::ProcessFrame(
		const Config& cfg,
		const TrackerFrameInput& in,
		const TrackerRefinePlanes* refinePlanes,
		FastObjectTrackSet& outTrackSet)
	{
		if (CheckFastOffGate(cfg, &outTrackSet))
		{
			currentMode_ = (budgetTier_ == SchedulerMode::ShedBudget) ? SchedulerMode::ShedBudget : SchedulerMode::Off;
			return false;
		}
		if (!PushFrame(cfg, in, nullptr))
		{
			const StageADiagRecord* frameDiag = FindStageADiag(in.frameIndex);
			outTrackSet.Clear(frameDiag ? frameDiag->mode : currentMode_, frameDiag ? frameDiag->abstainReason : AbstainReason::None);
			if (frameDiag)
			{
				outTrackSet.diag.activeCells = frameDiag->activeCells;
				outTrackSet.diag.blobCount = frameDiag->blobCount;
				outTrackSet.diag.clutterOverflow = frameDiag->clutterOverflow;
				outTrackSet.diag.noiseSigmaEst = frameDiag->noiseSigmaEst;
			}
			outTrackSet.diag.clutterOverflowCount = totalClutterOverflows_;
			outTrackSet.diag.quietFrames = quietFrames_;
			return false;
		}
		return ResolveAt(cfg, in.frameIndex, refinePlanes, outTrackSet);
	}
}
