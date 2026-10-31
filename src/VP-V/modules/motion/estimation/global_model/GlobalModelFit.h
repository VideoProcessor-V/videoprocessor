// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the per-pair fitter (implementation plan section 1, steps 1-11).
// Pure C++14, deterministic, no threads, no unordered containers, no std distributions.
#pragma once

#include <cstdint>
#include <memory>

#include "CameraMotion.h"
#include "GlobalModelGeometry.h"
#include "GlobalModelObjects.h"
#include "GlobalModelTemporal.h"

namespace CameraModel
{
	// Microsecond clock injected by the caller (null = no deadline enforcement).
	using Clock = int64_t(*)();

	struct PairInput
	{
		int gridW = 0, gridH = 0;
		double blockSize = 0;                       // source px per block
		Rect picture;                               // active picture; blocks whose centre is outside are bars
		const float* fwdX = nullptr;                // forward vectors (source px), gridW*gridH
		const float* fwdY = nullptr;
		const float* fwdTrust = nullptr;            // 0..1, 0 = untrusted
		const float* bwdX = nullptr;                // optional backward field
		const float* bwdY = nullptr;
		const float* bwdTrust = nullptr;
		const uint8_t* textGuard = nullptr;         // optional, 1 = excluded (text / logo tile)
		const float* texture = nullptr;             // optional, 0 flat .. 1 textured
		const float* pixelResidual = nullptr;       // optional, per block share of pixels beyond noise after the camera warp
		bool haveEngineGlobal = false;
		double engineGx = 0, engineGy = 0;          // the engine's global translation over the pair (source px)
		bool cut = false, fade = false, flash = false;
		double fadeGain = 1, fadeOffset = 0;        // informational, copied to the record
		uint64_t seq = 0, pairFrom = 0, pairTo = 0;
		double pairSeconds = 0;                     // <= 0 -> NoInput
		uint64_t resyncGeneration = 0, rebaseGeneration = 0;
		double precisionPx = 0;                     // <= 0 -> 0.25 * blockSize / 16
		int64_t deadlineMicros = 0;                 // absolute, in Clock microseconds; 0 = none
		const PairInput* next = nullptr;            // queued next pair (or null)
	};

	struct Config
	{
		ModelType maxModel = ModelType::Homography;
		int targetCells = 1296;                     // ~48x27 at 16:9, aspect-scaled to the picture
		int stage1Subset = 256;
		int stage2Best = 4;
		int maxHypotheses = 300;
		int preemptiveBudget = 0;                   // 0 = off; otherwise a hard cap (depth-0 bound)
		int irlsIterations = 3;
		double relThreshold = 0.05;                 // share of the predicted displacement
		double absThresholdSigmas = 3;
		double complexityPenaltyPerParam = 0.02;
		double quadrantMinShare = 0.05;
		// Confidence = logistic(b0 + b1*inlierShare + b2*precision + b3*coverage + b4*temporal +
		// b5*conditioning + b6*engineAgreement + b7*inverse + b8*ambiguity). Conservative: needs
		// most signals high to exceed 0.5 (all signals 1 -> about 0.95; typical pan with neutral
		// 0.5 optional signals -> about 0.8; weak coverage / ambiguity -> below 0.5).
		double b[9] = { -8.5, 4.0, 1.0, 1.5, 0.5, 0.5, 0.5, 1.0, 2.5 };
		bool objects = false;
		int maxObjects = 3;
		double objectMinShare = 0.01;
		int overlayZeroClusterMin = 2;              // zero-motion 4-neighbours that make an overlay
		double pixelResidualMinShare = 0.01;
		double flatTexture = 0.15;
		int smootherLag = 0;                        // 0 = depth 0, causal
		Clock clock = nullptr;
	};

	class Fitter
	{
	public:
		explicit Fitter(const Config& config = Config());
		// One pair -> one record. Never throws. See the plan for the cut / flash / fade rules.
		CameraMotion Fit(const PairInput& in);
		void Reset();
		const CameraMotion* Previous() const { return m_havePrevious ? &m_previous : nullptr; }
		const Config& GetConfig() const { return m_config; }

	private:
		struct Impl;
		Config m_config;
		CameraMotion m_previous;
		bool m_havePrevious = false;
		TemporalFilter m_temporal;
		ObjectTracker m_objects;
		uint64_t m_resyncGeneration = 0, m_rebaseGeneration = 0;
		bool m_haveGenerations = false;
		bool m_seedPending = false;                 // previous call published a post-cut seed
		friend struct Impl;
	};
}
