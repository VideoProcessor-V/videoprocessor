// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: independent object models (1..3) on Object-labelled trusted blocks with
// persistent ids. Only used when Config::objects. Pure C++14, deterministic.
#pragma once

#include <cstdint>
#include <vector>

#include "CameraMotion.h"

namespace CameraModel
{
	struct ObjectConfig
	{
		int maxObjects = 3;
		double objectMinShare = 0.01;          // of the picture's blocks
		double complexityPenaltyPerParam = 0.02;
		double relThreshold = 0.05;
		double absThresholdSigmas = 3;
	};

	struct ObjectInput
	{
		int gridW = 0, gridH = 0;
		double blockSize = 0;                  // source px per block
		Rect picture;
		const float* fwdX = nullptr;           // measured forward vectors (source px)
		const float* fwdY = nullptr;
		const float* trust = nullptr;          // 0..1
		const uint8_t* label = nullptr;        // Label per block; only Object (2) blocks with trust > 0 are used
		double precisionPx = 0;                // expected vector precision (> 0)
		uint64_t seed = 0;                     // for the RNG (splitmix64)
	};

	// Finds up to maxObjects models; assigns objectId (size gridW*gridH, 0 = none, ids persistent
	// across calls by predicted-centre matching to the previous call's objects, ids from 1, never
	// reused while the object persists). Sequential RANSAC: translation first, affine if it
	// beats the complexity penalty; min support = objectMinShare of the picture blocks;
	// compactness check (support must be spatially compact: its bounding box area must not exceed
	// 4x its block count). Deterministic: fixed loop orders, splitmix64.
	class ObjectTracker
	{
	public:
		void Reset();
		std::vector<ObjectModel> Update(const ObjectInput& in, const ObjectConfig& cfg,
			std::vector<uint8_t>& objectId);

	private:
		std::vector<ObjectModel> m_previous;
		uint8_t m_nextId = 1;
	};
}
