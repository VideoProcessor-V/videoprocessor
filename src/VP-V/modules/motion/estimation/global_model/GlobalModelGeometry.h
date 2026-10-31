// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: minimal solvers, weighted least squares, inversion, composition,
// similarity decomposition, time scaling. Pure C++14, deterministic (fixed-order double sums).
// Coordinates and conventions: see CameraMotion.h.
#pragma once

#include "CameraMotion.h"

namespace CameraModel
{
	// A displacement sample: position p (picture-centred source px) and measured displacement
	// (dx, dy) = q - p. Weight w >= 0.
	struct Sample
	{
		double x = 0, y = 0, dx = 0, dy = 0, w = 1;
	};

	Params IdentityParams(ModelType type = ModelType::None);

	// Minimal sample count: Translation 1, Similarity 2, Affine 3, Homography 4 (0 for None).
	int MinimalCount(ModelType type);

	// Exact fit through exactly MinimalCount(type) samples. False when degenerate (coincident or
	// collinear points for Affine/Homography, coincident points for Similarity).
	bool SolveMinimal(ModelType type, const Sample* samples, int count, Params& out);

	// Weighted least squares over n samples (n >= MinimalCount). Translation/Similarity/Affine
	// solve the normal equations; Homography uses DLT with Hartley normalisation (points
	// normalised by their weighted centroid and mean distance). `scale` is the picture half
	// diagonal in px (used to normalise the conditioning tests; no pixel constants).
	// Fixed accumulation order over the samples as given. False when degenerate.
	bool FitWeighted(ModelType type, const Sample* samples, int count, double scale, Params& out);

	// Inverse (3x3). `ok` (optional) false when singular (then identity of the same type returned).
	Params Invert(const Params& p, bool* ok = nullptr);

	// a o b : apply b first, then a. Result type = the higher of the two types.
	Params Compose(const Params& a, const Params& b);

	// Decompose into similarity rates per SECOND for a pair lasting pairSeconds (> 0):
	// rates.logScale = ln(s)/pairSeconds, angle/pairSeconds, tx/ty = the translation of the
	// picture centre (the mapped centre's displacement) / pairSeconds. For Affine/Homography the
	// best similarity approximation is taken from the four picture-corner displacements
	// (least squares similarity fit). False if pairSeconds <= 0 or the model is degenerate.
	bool ToSimilarity(const Params& p, const Rect& picture, double pairSeconds, SimilarityRates& out);

	// Similarity params from rates over `seconds` (inverse of ToSimilarity for similarity models).
	Params FromSimilarity(const SimilarityRates& rates, double seconds);

	// Model at `fraction` of the pair (0 -> identity, 1 -> p). Similarity and Translation:
	// log-scale, angle and translation interpolated linearly. Affine/Homography: the four
	// picture-corner displacements are scaled by `fraction` and the model of the same type is
	// refitted EXACTLY through the four displaced corners.
	Params Interpolate(const Params& p, double fraction, const Rect& picture);

	// Same rule as Interpolate (fraction may exceed 1 or be negative): used to rescale the
	// previous pair's model to a different pair duration (ratio = newSeconds / oldSeconds).
	Params TimeScale(const Params& p, double ratio, const Rect& picture);

	// Degenerate: not finite, near-singular, or maps the picture's corners to a folded /
	// collapsed quadrilateral or to a size outside [1/8, 8] of the picture. Relative to the
	// picture size only, no pixel constants.
	bool IsDegenerate(const Params& p, const Rect& picture);

	// Largest displacement difference between two models over the four picture corners and the
	// centre, divided by the picture diagonal.
	double MaxCornerDifference(const Params& a, const Params& b, const Rect& picture);

	// Deterministic generator (splitmix64). No std distributions anywhere in this module.
	struct SplitMix64
	{
		uint64_t state;
		explicit SplitMix64(uint64_t seed) : state(seed) {}
		uint64_t Next()
		{
			uint64_t z = (state += 0x9E3779B97F4A7C15ull);
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return z ^ (z >> 31);
		}
		// Uniform integer in [0, n), n > 0 (modulo bias negligible for n << 2^64, deterministic).
		uint32_t Below(uint32_t n) { return static_cast<uint32_t>(Next() % n); }
		double Unit() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); }
	};
}
