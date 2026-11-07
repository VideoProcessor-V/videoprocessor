// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the competing STATIC-CAMERA hypothesis (R1-STATIC). A coherent foreground object (a boat over
// water) can be fitted as the camera. Before a non-identity fit is accepted, the identity (static) model is scored
// against the fitted model on the BACKGROUND of the picture: the border ring, where a foreground object rarely sits
// and a real camera motion moves every block. When the static model explains the background about as well as the
// fit does (within `margin`) and explains a large share of it, the fit is demoted (Ambiguous, never usable). The test only ever demotes: it can
// not make a pair usable. Pure C++14, deterministic, CPU, O(ring blocks).
#pragma once

#include <cstdint>

#include "CameraMotion.h"

namespace CameraModel
{
	struct PairInput;

	struct StaticCameraConfig
	{
		// Agreement threshold of a block vector with a model: max(absSigmas * precision, rel * |model|), source px
		// (the fit's own rule). The static model has |model| = 0, so its threshold is absSigmas * precision.
		double absSigmas = 3.0;
		double rel = 0.05;
		// Ring width per side = max(1 block, round(ringFraction * picture extent in blocks)).
		double ringFraction = 0.125;
		// Minimum trusted ring blocks for the test to run.
		int minRingBlocks = 48;
		// The static model must explain at least this share of the trusted ring blocks ...
		double minStaticShare = 0.4;
		// ... and at least (the fitted model's ring share - margin): static explains the background about as well as the fit
		// (margin 0 = strictly "at least as well"; the real clips need 0.2, see the README tuning table).
		double margin = 0.2;
		// ... and the fitted model must actually move the ring: its median displacement over the ring blocks must exceed the
		// static threshold (otherwise static and fit are the same model within noise and the fit is left alone).
		bool requireMotion = true;
	};

	struct StaticCameraResult
	{
		bool computed = false;       // enough trusted ring blocks
		bool detected = false;       // the verdict: the static model wins, demote the fit
		double staticShare = 0;      // trusted ring blocks the identity model explains
		double fitShare = 0;         // trusted ring blocks the fitted model explains
		int ringBlocks = 0;
	};

	// Evaluates `model` against the identity model on the ring of `in`. Pure function of its arguments.
	StaticCameraResult EvaluateStaticCamera(const StaticCameraConfig& cfg, const PairInput& in, const Params& model);
}
