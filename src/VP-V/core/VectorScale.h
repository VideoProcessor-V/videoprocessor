#pragma once

#include <cmath>

// Per-axis vector scaling (design freeze 2026-10-24; ADR-0006; slot motion.vector_delivery).
//
// Motion is estimated on the ORIGINAL frame at SOURCE size. Every later stage may scale the two
// axes differently: a single-axis re-base (e.g. 1920x1080 delivered from a 1440-wide encode)
// shrinks x only, the 4K route doubles only the axes that need it, the output scaler maps to the
// output active area. A vector therefore travels with one factor PER AXIS, never one scalar:
// dx * sx, dy * sy. The same mechanism serves the 8K plan (motion at 1/4 per axis).
//
// Factors compose by multiplication along the chain (source -> encoded -> processing -> output).
// Geometry warps that are not a pure scale (A-lens, NLS) map positions separately; when that map
// is unknown, the vectors are invalid and every consumer falls back (MotionValidity).
struct VectorScale
{
	double sx = 1.0;
	double sy = 1.0;

	static VectorScale FromSizes(double fromWidth, double fromHeight, double toWidth, double toHeight)
	{
		VectorScale s;
		s.sx = fromWidth > 0.0 ? toWidth / fromWidth : 1.0;
		s.sy = fromHeight > 0.0 ? toHeight / fromHeight : 1.0;
		return s;
	}

	// This scale followed by `next` (e.g. re-base, then the 4K route).
	VectorScale Then(const VectorScale& next) const { return { sx * next.sx, sy * next.sy }; }

	VectorScale Inverse() const { return { sx != 0.0 ? 1.0 / sx : 0.0, sy != 0.0 ? 1.0 / sy : 0.0 }; }

	// True when the axes scale differently (single-axis re-base, anamorphic routes).
	bool Anisotropic(double tolerance = 1e-9) const { return std::fabs(sx - sy) > tolerance; }

	bool Identity(double tolerance = 1e-9) const
	{
		return std::fabs(sx - 1.0) <= tolerance && std::fabs(sy - 1.0) <= tolerance;
	}

	// A displacement (dx, dy) in the source space to the target space.
	template <class Vec>
	Vec Apply(const Vec& v) const
	{
		Vec out = v;
		out.x = static_cast<decltype(v.x)>(v.x * sx);
		out.y = static_cast<decltype(v.y)>(v.y * sy);
		return out;
	}

	void Apply(float& dx, float& dy) const
	{
		dx = static_cast<float>(dx * sx);
		dy = static_cast<float>(dy * sy);
	}
};
