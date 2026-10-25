#pragma once

#include <libplacebo/colorspace.h>

#include <algorithm>
#include <cmath>

// Delta 5 (D5-1): the code domain the temporal sigma-delta quantises to,
// from the frame's target representation. The shader works in 8- or 10-bit
// codes only; a float or 16-bit target (R16G16B16A16_FLOAT, scRGB) is not
// a domain it can serve: quantising it to 10-bit codes would clip values
// above 1.0 and throw away the float precision (audit delta5-beta.md), so
// the renderer leaves the sigma-delta out for such a frame.
namespace SigmaDeltaDomain
{
	struct Domain
	{
		bool full = false;
		int sample = 8;      // swapchain sample depth
		int color = 8;       // colour depth quantised to
		bool supported = true;
	};

	inline Domain FromTarget(const pl_color_repr& repr)
	{
		Domain d;
		d.full = pl_color_levels_guess(&repr) == PL_COLOR_LEVELS_FULL;
		d.sample = repr.bits.sample_depth > 0 ? repr.bits.sample_depth : 8;
		d.color = repr.bits.color_depth > 0 ? repr.bits.color_depth : d.sample;
		d.supported = d.sample >= 8 && d.sample <= 10 && d.color >= 8 && d.color <= d.sample;
		return d;
	}

	// Hunt 2 L2: the output codes that survive libplacebo-beta's final clamp
	// (fork patch 0002, after every user hook), in the domain the shader
	// quantises to (`color` bits). Limited range reserves only the timing
	// reference codes, 2^(n-8) codes at each end (8-bit 0 and 255, 10-bit 0-3
	// and 1020-1023), so BTB/WTW stay; full range uses every code. Derived
	// from the bit depth, as the patch does, and mirrored by the shader.
	struct CodeLimits
	{
		double min = 0.0;
		double max = 255.0;
	};

	inline CodeLimits Limits(int colorBits, bool full)
	{
		const int bits = (std::min)((std::max)(colorBits, 8), 16);
		const double unit = std::ldexp(1.0, bits - 8);
		CodeLimits l;
		l.min = full ? 0.0 : unit;
		l.max = full ? std::ldexp(1.0, bits) - 1.0 : 255.0 * unit - 1.0;
		return l;
	}

	// Hunt 2 L1: the carried residual's bound. The dither d is not fed back,
	// so |x + r - k| reaches 0.5 + amplitude in normal operation; only the
	// code-limit clamp needs the bound (anti-windup).
	inline double ResidualBound(double noiseAmplitude)
	{
		return 0.5 + std::max(0.0, noiseAmplitude);
	}

	// CPU model of one quantiser step of `Temporal sigma-delta.glsl` (one
	// channel, same pixel, gate passed): x and r in output codes, d the
	// decorrelation dither. Returns the emitted code; r is updated in place.
	inline double QuantiseStep(double x, double& r, double d, const CodeLimits& limits, double noiseAmplitude)
	{
		const double k = (std::min)((std::max)(std::floor(x + r + d + 0.5), limits.min), limits.max);
		const double bound = ResidualBound(noiseAmplitude);
		r = (std::min)((std::max)(x + r - k, -bound), bound);
		return k;
	}

	// The shader's interleaved gradient noise (Jimenez 2014) with its
	// golden-ratio temporal shift, for the CPU model (float, as on the GPU).
	inline float Ign(float px, float py, int frame)
	{
		const float shift = 5.588238f * static_cast<float>(frame & 63);
		px += shift;
		py += shift;
		const float inner = 0.06711056f * px + 0.00583715f * py;
		const float v = 52.9829189f * (inner - std::floor(inner));
		return v - std::floor(v);
	}
}
