// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Stage C pure header-only consumer accessors for FastObjectTrackSet:
//  * SampleObjectWarp / SampleObjectVector: exact curved/impulse mid-frame position p(phaseT)
//    + swept disocclusion capsule check (FrameInterpolator / De-Judder, zero double-ghosting).
//  * IsProtectedForTemporalDetail: dilated (r_est + 2 px) ball + swept trail protection mask
//    (Temporal Detail G5 / TD-36 / TD-37).
//  * BuildTinyMoverRois: populates CameraModel::SearchHints::Roi boxes (CameraGuidanceGpu).
//  * MicroRoiDeblurHint: exposes streak axis and deconvolved length L_est for opt-in micro-ROI
//    directional sharpening.
// Pure C++14/17, header-only, zero Windows headers, zero allocations (except optional BuildTinyMoverRois output vector).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../global_model/GlobalModelSearchHints.h"
#include "FastObjectTypes.h"

namespace FastObjects
{
	// Evaluates the exact trajectory position p(phaseT) in source px for phaseT in [0, 1] across pair [t, t+1].
	// LinearGlide: exact linear chord.
	// Ballistic: exact parabolic arc p0 + v*phi - 0.5*a_pair*phi*(1 - phi) passing through p(0) and p(1).
	// Impulse: exact two-leg curve passing through p(0), vertex (impulseX, impulseY) at impulseTau, and p(1) with zero overshoot.
	inline void EvaluateTrackPosition(const FastObjectTrack& trk, float phaseT, float& outX, float& outY)
	{
		const float phi = std::min(std::max(phaseT, 0.0f), 1.0f);
		const float p0x = trk.cx;
		const float p0y = trk.cy;
		const float p1x = trk.cx + trk.vx;
		const float p1y = trk.cy + trk.vy;
		const float dt2 = trk.pairSeconds * trk.pairSeconds;
		const float axPair = trk.ax * dt2;
		const float ayPair = trk.ay * dt2;

		if (trk.type == TrajectoryType::Impulse && trk.hasImpulse &&
			trk.impulseTau > 1e-4f && trk.impulseTau < 1.0f - 1e-4f)
		{
			const float tau = trk.impulseTau;
			const float psx = trk.impulseX;
			const float psy = trk.impulseY;
			if (phi <= tau)
			{
				const float u = phi / tau;
				outX = p0x + u * (psx - p0x) - 0.5f * axPair * phi * (tau - phi);
				outY = p0y + u * (psy - p0y) - 0.5f * ayPair * phi * (tau - phi);
			}
			else
			{
				const float u = (phi - tau) / (1.0f - tau);
				outX = psx + u * (p1x - psx) - 0.5f * axPair * (phi - tau) * (1.0f - phi);
				outY = psy + u * (p1y - psy) - 0.5f * ayPair * (phi - tau) * (1.0f - phi);
			}
			return;
		}

		if (trk.type == TrajectoryType::Ballistic || (trk.type == TrajectoryType::LinearGlide && (axPair != 0.0f || ayPair != 0.0f)))
		{
			const float arch = 0.5f * phi * (1.0f - phi);
			outX = p0x + phi * trk.vx - axPair * arch;
			outY = p0y + phi * trk.vy - ayPair * arch;
			return;
		}

		outX = p0x + phi * trk.vx;
		outY = p0y + phi * trk.vy;
	}

	// Checks whether point (x, y) lies inside a track's swept disocclusion hull (handling both single-leg
	// capsules and two-leg Impulse hulls through the bounce vertex).
	inline bool IsInsideSweptHull(const FastObjectTrack& trk, float x, float y, float extraRadiusPx = 0.0f)
	{
		const float r = trk.disocclusionCapsule.radiusPx + extraRadiusPx;
		if (!(r > 0.0f))
			return false;
		const float r2 = r * r;
		if (trk.type == TrajectoryType::Impulse && trk.hasImpulse &&
			trk.impulseTau > 1e-4f && trk.impulseTau < 1.0f - 1e-4f)
		{
			const float d0 = DistanceSqToSegment(x, y, trk.cx, trk.cy, trk.impulseX, trk.impulseY);
			const float d1 = DistanceSqToSegment(x, y, trk.impulseX, trk.impulseY, trk.cx + trk.vx, trk.cy + trk.vy);
			return (d0 <= r2) || (d1 <= r2);
		}
		return CapsuleContains(trk.disocclusionCapsule, x, y, extraRadiusPx);
	}

	// Exact object-layer curved/impulse warp accessor for FrameInterpolator at output phase `phaseT` in [0, 1].
	// Returns true if (x, y) (source px) lies inside a Valid track's footprint at phaseT, providing exact
	// backward warp (outToPrev = p(0) - p(phaseT)) and forward warp (outToNext = p(1) - p(phaseT)).
	// When outside all Valid object footprints at phaseT, sets outDisoccluded = true if (x, y) lies in a
	// Valid track's swept disocclusion trail (so the caller fills uncovered background cleanly from the
	// unoccluded frame). Outside all Valid tracks & trails: returns false with outDisoccluded = false.
	inline bool SampleObjectWarp(
		const FastObjectTrackSet& trackSet,
		float x,
		float y,
		float phaseT,
		float& outToPrevX,
		float& outToPrevY,
		float& outToNextX,
		float& outToNextY,
		bool& outDisoccluded,
		uint32_t* outTrackId = nullptr)
	{
		outToPrevX = 0.0f;
		outToPrevY = 0.0f;
		outToNextX = 0.0f;
		outToNextY = 0.0f;
		outDisoccluded = false;
		if (outTrackId)
			*outTrackId = 0;

		const uint32_t n = std::min(trackSet.count, kMaxTracksHardCap);
		int bestIdx = -1;
		float bestConf = -1.0f;
		float bestPx = 0.0f, bestPy = 0.0f;
		bool anyDisocc = false;

		for (uint32_t i = 0; i < n; ++i)
		{
			const FastObjectTrack& trk = trackSet.tracks[i];
			if (trk.state != ValidityState::Valid)
				continue;

			float px = 0.0f, py = 0.0f;
			EvaluateTrackPosition(trk, phaseT, px, py);

			// Map query pixel back into frame-A mask coordinates by subtracting shift (p(phaseT) - p(0)).
			const float ax = x - (px - trk.cx);
			const float ay = y - (py - trk.cy);
			bool hitMask = false;
			if (trk.hasMaskBits && !trk.maskBox.Empty())
			{
				hitMask = MaskContainsPoint(trk.maskBox, trk.maskBits, true, ax, ay);
			}
			else
			{
				const float dx = ax - trk.cx;
				const float dy = ay - trk.cy;
				const float r = std::max(trk.radiusPx, 1.0f);
				hitMask = (dx * dx + dy * dy <= r * r);
			}

			if (hitMask)
			{
				if (trk.confidence > bestConf)
				{
					bestConf = trk.confidence;
					bestIdx = static_cast<int>(i);
					bestPx = px;
					bestPy = py;
				}
			}
			else if (IsInsideSweptHull(trk, x, y, 0.0f))
			{
				anyDisocc = true;
			}
		}

		if (bestIdx >= 0)
		{
			const FastObjectTrack& trk = trackSet.tracks[bestIdx];
			outToPrevX = trk.cx - bestPx;
			outToPrevY = trk.cy - bestPy;
			outToNextX = (trk.cx + trk.vx) - bestPx;
			outToNextY = (trk.cy + trk.vy) - bestPy;
			outDisoccluded = false;
			if (outTrackId)
				*outTrackId = trk.id;
			return true;
		}

		outDisoccluded = anyDisocc;
		return false;
	}

	// Convenience object-layer pair displacement accessor at output phase `phaseT` in [0, 1].
	// Returns true when (x, y) hits a Valid object at phaseT, outputting exact pair vector (vx, vy)
	// and optionally the exact mid-frame curved/impulse center (outPosX, outPosY).
	inline bool SampleObjectVector(
		const FastObjectTrackSet& trackSet,
		float x,
		float y,
		float phaseT,
		float& outVx,
		float& outVy,
		bool& outDisoccluded,
		uint32_t* outTrackId = nullptr)
	{
		float toPrevX = 0.0f, toPrevY = 0.0f, toNextX = 0.0f, toNextY = 0.0f;
		if (!SampleObjectWarp(trackSet, x, y, phaseT, toPrevX, toPrevY, toNextX, toNextY, outDisoccluded, outTrackId))
		{
			outVx = 0.0f;
			outVy = 0.0f;
			return false;
		}
		outVx = toNextX - toPrevX;
		outVy = toNextY - toPrevY;
		return true;
	}

	// Temporal Detail fast-object protection mask (spec C.2.3, Owner G5 / TD-36 / TD-37):
	// Returns true inside any Valid track's dilated (r_est + dilatePx, default 2.0 px) ball + swept
	// trail footprint so TD sets temporal accumulation weight -> 0 and never erases flying balls.
	inline bool IsProtectedForTemporalDetail(
		const FastObjectTrackSet& trackSet,
		float x,
		float y,
		float dilatePx = 2.0f)
	{
		const uint32_t n = std::min(trackSet.count, kMaxTracksHardCap);
		const float d = std::max(dilatePx, 0.0f);
		for (uint32_t i = 0; i < n; ++i)
		{
			const FastObjectTrack& trk = trackSet.tracks[i];
			if (trk.state != ValidityState::Valid)
				continue;
			if (IsInsideSweptHull(trk, x, y, d))
				return true;
			if (!trk.maskBox.Empty())
			{
				if (x >= static_cast<float>(trk.maskBox.left) - d &&
					x < static_cast<float>(trk.maskBox.right) + d &&
					y >= static_cast<float>(trk.maskBox.top) - d &&
					y < static_cast<float>(trk.maskBox.bottom) + d)
					return true;
			}
		}
		return false;
	}

	// Populates CameraModel::SearchHints::Roi boxes (block units, right/bottom exclusive) covering both
	// current and predicted landing positions of every Valid track so CameraGuidanceGpu / matcher
	// marks those blocks as BlockHint::FinerGrid (never narrowing them to the background camera vector).
	inline void BuildTinyMoverRois(
		const FastObjectTrackSet& trackSet,
		int gridW,
		int gridH,
		double blockSize,
		std::vector<CameraModel::SearchHints::Roi>& outRois,
		uint32_t maxRois = kMaxBlobs)
	{
		outRois.clear();
		if (gridW <= 0 || gridH <= 0 || !(blockSize > 0.0) || maxRois == 0)
			return;
		const uint32_t n = std::min(trackSet.count, kMaxTracksHardCap);
		const double invBlock = 1.0 / blockSize;

		auto emitBox = [&](double x0, double y0, double x1, double y1)
		{
			if (outRois.size() >= maxRois)
				return;
			CameraModel::SearchHints::Roi r;
			r.bx0 = std::min(std::max(static_cast<int>(std::floor(x0 * invBlock)), 0), gridW);
			r.by0 = std::min(std::max(static_cast<int>(std::floor(y0 * invBlock)), 0), gridH);
			r.bx1 = std::min(std::max(static_cast<int>(std::ceil(x1 * invBlock)), 0), gridW);
			r.by1 = std::min(std::max(static_cast<int>(std::ceil(y1 * invBlock)), 0), gridH);
			if (r.bx1 > r.bx0 && r.by1 > r.by0)
				outRois.push_back(r);
		};

		for (uint32_t i = 0; i < n; ++i)
		{
			const FastObjectTrack& trk = trackSet.tracks[i];
			if (trk.state != ValidityState::Valid)
				continue;
			const double pad = std::fmax(static_cast<double>(trk.radiusPx), 2.0);
			if (!trk.maskBox.Empty())
			{
				emitBox(
					static_cast<double>(trk.maskBox.left) - pad,
					static_cast<double>(trk.maskBox.top) - pad,
					static_cast<double>(trk.maskBox.right) + pad,
					static_cast<double>(trk.maskBox.bottom) + pad);
				emitBox(
					static_cast<double>(trk.maskBox.left) + static_cast<double>(trk.vx) - pad,
					static_cast<double>(trk.maskBox.top) + static_cast<double>(trk.vy) - pad,
					static_cast<double>(trk.maskBox.right) + static_cast<double>(trk.vx) + pad,
					static_cast<double>(trk.maskBox.bottom) + static_cast<double>(trk.vy) + pad);
			}
			else
			{
				emitBox(trk.cx - pad, trk.cy - pad, trk.cx + pad, trk.cy + pad);
				emitBox(trk.cx + trk.vx - pad, trk.cy + trk.vy - pad, trk.cx + trk.vx + pad, trk.cy + trk.vy + pad);
			}
		}
	}

	// Micro-ROI directional motion-blur cleanup hint (spec C.4.6, opt-in future sharpening hook):
	// Returns true iff (x, y) lies inside a Valid blur-stretched track, exposing unit streak axis
	// (cos(theta), sin(theta)) and deconvolved streak length L_est (source px).
	inline bool MicroRoiDeblurHint(
		const FastObjectTrackSet& trackSet,
		float x,
		float y,
		float& outAxisX,
		float& outAxisY,
		float& outStreakLenPx)
	{
		outAxisX = 0.0f;
		outAxisY = 0.0f;
		outStreakLenPx = 0.0f;
		const uint32_t n = std::min(trackSet.count, kMaxTracksHardCap);
		for (uint32_t i = 0; i < n; ++i)
		{
			const FastObjectTrack& trk = trackSet.tracks[i];
			if (trk.state != ValidityState::Valid)
				continue;
			if ((trk.flags & TrackFlags::kBlurStretched) == 0u || !(trk.streakLenPx > trk.radiusPx))
				continue;
			if (!MaskContainsPoint(trk.maskBox, trk.maskBits, trk.hasMaskBits, x, y))
				continue;
			outAxisX = std::cos(trk.streakAngleRad);
			outAxisY = std::sin(trk.streakAngleRad);
			outStreakLenPx = trk.streakLenPx;
			return true;
		}
		return false;
	}
}
