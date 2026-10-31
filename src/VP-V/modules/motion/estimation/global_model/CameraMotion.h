// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19): the per-source-frame-pair record. Pure data + two inline helpers.
// Header-only, C++14, no Windows headers. See README.md in this folder.
//
// Conventions (all modules and tests share them):
//  * Coordinates: SOURCE pixels, origin at the centre of the active picture, x right, y down.
//  * Params map a point in frame N-1 to frame N for the actual source-frame pair (3x3 row-major,
//    homogeneous). Identity = no motion.
//  * A block's forward vector at block centre p (frame N-1 coordinates) is q - p, where q is the
//    matching position in frame N. The model's prediction there is Apply(params, p) - p.
//    A backward vector at p (frame N coordinates) is the matching position in N-1 minus p; the
//    model prediction is Apply(inverse(params), p) - p.
//  * Block (bx, by) of a gridW x gridH grid of blockSize-pixel blocks anchored at the top-left of
//    the frame has its centre at ((bx + 0.5) * blockSize, (by + 0.5) * blockSize) in frame pixels;
//    BlockCentre() converts that to picture-centred coordinates.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace CameraModel
{
	// Finiteness by exponent bits (exact, inline; std::isfinite is an out-of-line call on MSVC).
	inline bool IsFiniteBits(float v) { uint32_t u; std::memcpy(&u, &v, sizeof(u)); return (u & 0x7F800000u) != 0x7F800000u; }
	inline bool IsFiniteBits(double v) { uint64_t u; std::memcpy(&u, &v, sizeof(u)); return (u & 0x7FF0000000000000ull) != 0x7FF0000000000000ull; }

	enum class ModelType : uint8_t { None, Translation, Similarity, Affine, Homography };

	enum class Label : uint8_t { Unknown = 0, Camera = 1, Object = 2, Overlay = 3 };

	enum class Reason : uint8_t
	{
		Ok, NoInput, TooFewSamples, Cut, Flash, PostCutSeeded, FlashBridged, LowConfidence,
		Degenerate, Ambiguous, InverseMismatch, DeadlineMissed, GenerationReset, Disabled
	};

	inline const char* ReasonText(Reason reason)
	{
		switch (reason)
		{
		case Reason::Ok: return "ok";
		case Reason::NoInput: return "no input";
		case Reason::TooFewSamples: return "too few samples";
		case Reason::Cut: return "scene cut";
		case Reason::Flash: return "flash frame";
		case Reason::PostCutSeeded: return "seeded from the first pair after a cut";
		case Reason::FlashBridged: return "flash bridged";
		case Reason::LowConfidence: return "low confidence";
		case Reason::Degenerate: return "degenerate model";
		case Reason::Ambiguous: return "ambiguous (no dominant camera motion)";
		case Reason::InverseMismatch: return "forward/backward mismatch";
		case Reason::DeadlineMissed: return "deadline missed";
		case Reason::GenerationReset: return "reset by a resync or re-base";
		case Reason::Disabled: return "disabled";
		}
		return "unknown";
	}

	// Active picture rectangle in frame pixels (right/bottom exclusive).
	struct Rect
	{
		int left = 0, top = 0, right = 0, bottom = 0;
		int Width() const { return right - left; }
		int Height() const { return bottom - top; }
		bool Empty() const { return right <= left || bottom <= top; }
		double CentreX() const { return 0.5 * (left + right); }
		double CentreY() const { return 0.5 * (top + bottom); }
		bool operator==(const Rect& o) const { return left == o.left && top == o.top && right == o.right && bottom == o.bottom; }
	};

	struct Params
	{
		ModelType type = ModelType::None;
		double h[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };   // row-major 3x3, identity by default
	};

	// Similarity decomposition, time-normalised: per SECOND.
	struct SimilarityRates
	{
		double logScale = 0, angle = 0, tx = 0, ty = 0;
	};

	// Confidence inputs, each 0..1 (neutral 0.5 where unavailable).
	struct Signals
	{
		double inlierShare = 0, precision = 0, coverage = 0, temporal = 0.5, conditioning = 0,
			engineAgreement = 0.5, inverse = 0.5, ambiguity = 1, logit = 0;
		bool haveBackward = false, haveEngine = false;
	};

	struct ObjectModel
	{
		uint8_t id = 0;
		ModelType type = ModelType::None;
		double h[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		uint32_t blocks = 0;
		double cx = 0, cy = 0;   // support centre, picture-centred source px
	};

	struct BlockData
	{
		int gridW = 0, gridH = 0;
		double blockSize = 0;
		std::vector<uint8_t> label;       // Label per block
		std::vector<float> resX, resY;    // measured minus model vector (0 where untrusted/unmeasured)
		std::vector<float> modelX, modelY;// model prediction at the block centre over the pair, source px
		std::vector<uint8_t> objectId;    // 0 = none
		std::vector<uint8_t> tinyMover;   // 1 = pixelResidual flagged a sub-block / tiny fast mover
		std::vector<uint8_t> excluded;    // 1 = bar or text-guard block (a static-logo Overlay is NOT excluded)
		// The fit's own agreement threshold: a vector agrees with the model when
		// |v - model| <= max(absThresholdPx, relThreshold * |model|) (source px).
		float absThresholdPx = 0, relThreshold = 0;
	};

	struct CameraMotion
	{
		uint64_t seq = 0, pairFrom = 0, pairTo = 0;
		double pairSeconds = 0;
		uint64_t resyncGeneration = 0, rebaseGeneration = 0;
		bool valid = false;
		Params params;
		SimilarityRates rates;
		double confidence = 0;
		Reason reason = Reason::NoInput;
		Signals signals;
		bool seededFromNext = false, bridged = false, inFade = false, smoothed = false;
		Params smoothedParams;
		double fadeGain = 1, fadeOffset = 0;
		uint32_t samples = 0, hypotheses = 0;
		int64_t costMicros = 0;
		std::vector<ObjectModel> objects;
		std::shared_ptr<const BlockData> blocks;
		Rect picture;
	};

	// Map a point (picture-centred, source px) through Params.
	inline void Apply(const Params& p, double x, double y, double* ox, double* oy)
	{
		const double w = p.h[6] * x + p.h[7] * y + p.h[8];
		const double iw = (w != 0.0) ? 1.0 / w : 0.0;
		*ox = (p.h[0] * x + p.h[1] * y + p.h[2]) * iw;
		*oy = (p.h[3] * x + p.h[4] * y + p.h[5]) * iw;
	}

	// Displacement the model predicts at a point.
	inline void Displacement(const Params& p, double x, double y, double* dx, double* dy)
	{
		double ox = 0, oy = 0;
		Apply(p, x, y, &ox, &oy);
		*dx = ox - x;
		*dy = oy - y;
	}

	// Block centre in picture-centred source px.
	inline void BlockCentre(int bx, int by, double blockSize, const Rect& picture, double* x, double* y)
	{
		*x = (bx + 0.5) * blockSize - picture.CentreX();
		*y = (by + 0.5) * blockSize - picture.CentreY();
	}
}
