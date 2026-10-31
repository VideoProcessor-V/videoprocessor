// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19, phase 2): CPU reference of the matcher's search semantics, used by tests
// and by the cost measurement. NOT on the runtime path. It works on float luma planes in MATCHER pixels
// (source px * sourceToMatcher; default 0.5 = the half-size working picture). A block is 8x8 matcher px and
// its SAD window is the block plus a 2-px margin (12x12; the window origin is clamped into the plane, the
// other frame's window follows at the same offset plus the vector, samples clamped). Fractional vectors use
// bilinear sampling. SAD per sample (sum / window area) is in code values. Deterministic, no threads.
#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "GlobalModelMeasurement.h"

namespace CameraModel
{
	namespace Reference
	{
		struct Plane
		{
			int w = 0, h = 0;
			const float* px = nullptr;
		};

		struct MatchConfig
		{
			double sourceToMatcher = 0.5;   // matcher px per source px
			int fullRadius = 12;            // exhaustive whole-pixel radius around zero (matcher px)
			float tolerance = 3.0f;         // accept / verify threshold, codes per sample
			float lambda = 0.5f;            // second-sweep smoothness weight
			bool secondSweep = true;
			bool gate = false;              // E4: keep the previous field when the scene is static
			float motionLo = 0.25f;         // E4 threshold, source px
		};

		// The previous frame's result (matcher px).
		struct MatchState
		{
			std::vector<float> vx, vy, sad;
			bool valid = false;
		};

		// Path codes of MatchResult::path.
		enum : uint8_t { kPathFull = 0, kPathNarrow = 1, kPathEscalated = 2, kPathStatic = 3, kPathSkipped = 4, kPathE1Skip = 5, kPathFiner = 6 };

		struct MatchResult
		{
			int bw = 0, bh = 0;
			std::vector<float> vx, vy, sad;      // matcher px, codes per sample (1e4 = untrusted)
			std::vector<uint8_t> path;
			uint64_t evaluations = 0;
			uint32_t narrowAccepted = 0, escalated = 0, e1Skipped = 0, staticKept = 0, finer = 0;
			bool skipped = false;
			double ms = 0;
		};

		MatchState StateFrom(const MatchResult& r);

		// plan == null or !plan->active: today's search. globalX/Y: today's D1 result in source px (NaN = none).
		MatchResult Match(const MatchConfig& cfg, const Plane& from, const Plane& to, const MeasurementPlan* plan,
			const MatchState* previous,
			double globalX = std::numeric_limits<double>::quiet_NaN(),
			double globalY = std::numeric_limits<double>::quiet_NaN());
	}
}
