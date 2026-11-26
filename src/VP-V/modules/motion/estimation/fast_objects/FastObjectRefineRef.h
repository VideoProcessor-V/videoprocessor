// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Stage B.5 CPU reference: ROI-only coarse-to-fine masked ZNCC strictly over the ball's binary
// support mask (quarter -> half +/-2 -> full +/-2 -> parabolic sub-pel fit), background subtraction
// for disjoint fast movers, differential 1D motion-blur PSF synthesis for accelerating streaks,
// and transparent kCentroidVector fallback for tiny/textureless discs.
// Pure C++14/17, deterministic, caller-owned workspace (zero steady-state heap allocations).
#pragma once

#include <cstdint>
#include <vector>

#include "FastObjectTypes.h"

namespace FastObjects
{
	struct RefineInput
	{
		// Full-resolution (or finest available) float luma planes (width * height).
		const float* lumaA = nullptr;          // Frame t
		const float* lumaB = nullptr;          // Frame t+1
		int width = 0;
		int height = 0;
		float planeScale = 1.0f;               // Source px per plane sample (1.0 = full-res, 2.0 = half-res)

		// Camera model H_{t -> t+1} in picture-centred source px (for background subtraction on disjoint movers).
		CameraModel::Params camAToB;
		bool camUsable = false;
		CameraModel::Rect picture;

		// Track prediction & support mask in source px.
		float cx = 0.0f;
		float cy = 0.0f;
		float predVx = 0.0f;                   // Predicted frame t -> t+1 displacement (source px)
		float predVy = 0.0f;
		float radiusPx = 0.0f;
		float streakLenPx = 0.0f;
		float streakAngleRad = 0.0f;
		float searchRadiusPx = 6.0f;           // ceil(3*sqrt(lambda_max(S))) + ceil(r_est)
		float deltaBlurLenPx = 0.0f;           // Differential blur (||v_t|| - ||v_{t-1}||) * tau_s (source px)
		float blurAxisX = 1.0f;                // Unit tangent vector along motion blur
		float blurAxisY = 0.0f;
		CameraModel::Rect maskBox;
		uint64_t maskBits[4] = { 0ull, 0ull, 0ull, 0ull };
		bool hasMaskBits = false;
	};

	struct RefineOutput
	{
		float vx = 0.0f;                       // Refined displacement (source px)
		float vy = 0.0f;
		float bestZncc = 0.0f;                 // Peak masked ZNCC in [-1, 1]
		float bestCost = 1.0f;                 // 1 - bestZncc in [0, 2]
		float meanCost = 1.0f;                 // Mean cost over full-res +/-2 search surface
		float matchMargin = 0.0f;              // clamp(1 - bestCost / meanCost, 0, 1)
		bool usedCentroidFallback = false;     // True -> kept trajectory prediction (tiny/textureless/flat)
		bool usedBackgroundSub = false;        // True -> background-subtracted foreground templates used
		bool usedDifferentialBlur = false;     // True -> 1D line PSF applied for differential streak blur
	};

	// Caller-owned scratch workspace for ROI-only patch extraction (no full-frame buffers).
	struct RefineWorkspace
	{
		std::vector<float> patchA;
		std::vector<float> patchABlurred;
		std::vector<float> patchB;
		std::vector<float> patchBBlurred;
		std::vector<uint8_t> maskA;            // Binary support mask in ROI coords (0 or 1)
		std::vector<float> halfA;
		std::vector<float> halfB;
		std::vector<uint8_t> halfMask;
		std::vector<float> quarterA;
		std::vector<float> quarterB;
		std::vector<uint8_t> quarterMask;

		void Reserve(int maxPatchSide = 96);
	};

	// Runs Stage B.5 ROI-only masked pyramid ZNCC refinement for a single track.
	// Always produces a finite output vector; sets out.usedCentroidFallback = true on tiny/textureless discs.
	void RefineFastObjectVector(
		const RefineInput& in,
		RefineWorkspace& ws,
		RefineOutput& out);
}
