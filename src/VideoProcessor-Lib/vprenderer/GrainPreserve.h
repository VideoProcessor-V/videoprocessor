#pragma once

#include <algorithm>
#include <cmath>

// Film-grain preservation (owner 2026-10-17: film grain is never averaged or
// interpolated away; the goal is a faithful image with detail as if it were
// max-bitrate RGB). docs/audit/grain-audit-2026-10-17.md.
//
// Shared GLSL for the stages that resample a source frame (the interpolator,
// the de-judder's source warp) and the small CPU helpers the renderer uses to
// feed the guards. Every user keeps its pre-2026-10-17 path behind a switch.
namespace GrainPreserve
{
	// Catmull-Rom (B = 0, C = 0.5) done in the shader from texelFetch: fp32
	// weights, edges clamped, positions in the texture's own pixels (texel
	// centres at +0.5). Exact at a texel centre (a whole-pixel move gives the
	// source codes back), full precision for 16-bit planes, and - unlike
	// bilinear, which keeps 0.5x of white grain at a half-pixel shift - keeps
	// 0.8x or more of it at every fractional position. Values are never clamped
	// here (the overshoot is the kernel's; the storage format decides).
	inline const char* CatmullGlsl()
	{
		return R"GLSL(
vec4 gp_catmull_w(float t)
{
    return vec4(t * (-0.5 + t * (1.0 - 0.5 * t)), 1.0 + t * t * (-2.5 + 1.5 * t),
                t * (0.5 + t * (2.0 - 1.5 * t)), t * t * (-0.5 + 0.5 * t));
}
#define DEFINE_CATMULL(NAME, TEX) \
vec4 NAME(vec2 p) \
{ \
    ivec2 hi = textureSize(TEX, 0) - ivec2(1); \
    vec2 g = p - vec2(0.5); \
    ivec2 i0 = ivec2(floor(g)); \
    vec2 f = g - vec2(i0); \
    vec4 wx = gp_catmull_w(f.x); \
    vec4 wy = gp_catmull_w(f.y); \
    vec4 acc = vec4(0.0); \
    for (int j = 0; j < 4; j++) { \
        vec4 row = vec4(0.0); \
        for (int i = 0; i < 4; i++) \
            row += wx[i] * texelFetch(TEX, clamp(i0 + ivec2(i - 1, j - 1), ivec2(0), hi), 0); \
        acc += wy[j] * row; \
    } \
    return acc; \
}
)GLSL";
	}

	// The low band: a Gaussian of sigma 3 px; the band above it carries the
	// grain. Sigma 3 rather than the audit's 1.5: soft grain (a 0.7 px blur) keeps
	// enough energy below a 1.5 px split for the phase blend of the low band to
	// cost it 12 % at t = 0.5; above 3 px under 4 % (grainwork/proto6.py).
	// It is held at half resolution (a sigma-3 Gaussian passes 1.5e-5 at the
	// half-resolution Nyquist frequency): a 2x2 box average, then a separable
	// Gaussian of sigma sqrt(3^2 - 0.5^2) / 2 half-resolution pixels (the box adds
	// 0.25 px^2 per axis), 11 taps; read back bilinearly at p / 2 (texel centres
	// line up: full-resolution 2h + 1 is half-resolution h + 0.5). About a sixth of
	// a full-resolution 19-tap pass.
	constexpr double kLowSigma = 3.0;
	constexpr int kLowRadius = 5;   // half-resolution taps each side
	inline double LowHalfSigma() { return std::sqrt(kLowSigma * kLowSigma - 0.25) / 2.0; }
	inline double LowWeight(int i)
	{
		const double s = LowHalfSigma();
		double sum = 0.0;
		for (int k = -kLowRadius; k <= kLowRadius; ++k)
			sum += std::exp(-0.5 * k * k / (s * s));
		return std::exp(-0.5 * i * i / (s * s)) / sum;
	}

	// Gain limits of the per-block energy match (the interpolator's high band):
	// never below the source (1), never more than twice its amplitude.
	constexpr double kGainMin = 1.0, kGainMax = 2.0;
	// Blocks with fewer synthesised pixels than this keep gain 1 (too few to measure).
	constexpr int kGainMinPixels = 16;

	// The expected coding-noise RMS, in 8-bit codes, for a bitrate-quality
	// reading (SourceResolutionDetector's 0..1; 1 = clean). The residual
	// stabilisation may hold the CNN / SSIM residual only within this much of
	// the frame's own (the coding-artefact band); anything larger is the
	// picture (grain, detail) and passes. Half a code on a clean source, up to
	// three codes at quality 0 (VpDebander's low-bitrate allowance).
	inline double CodingNoiseCodes(double bitrateQuality)
	{
		const double q = (std::max)(0.0, (std::min)(1.0, bitrateQuality));
		return 0.5 + 2.5 * (1.0 - q);
	}

	// VpDebander's spatial grain guard (no vectors needed): per 16x16 block, the
	// share of busy pixels. A pixel is busy when
	//  - its Laplacian |4c - n - s - e - w| (largest channel) exceeds a third of the
	//    deband threshold - of the content-step part only (at most 1.5 8-bit steps):
	//    the low-bitrate allowance (up to 3 steps) raises the blur, never the level
	//    grain must reach to count as grain - snapped to the half code above it, so
	//    integer-coded input never sits on the comparison (on the GPU a Laplacian of
	//    exactly that many codes counted on some pixels and not others); or
	//  - it is a strict local extremum along x, y or either diagonal (both neighbours
	//    on that line above, or both below, it by more than a twelfth of the same
	//    content step: half a 10-bit code at an 8-bit-in-10 threshold; 2026-10-17 tune
	//    for soft 0.5-code grain, which slipped under the Laplacian alone - guard 0.91x).
	//    A quantised smooth gradient is monotone along every line through a pixel, of
	//    any slope and step size, so this never fires on clean bands: their share is
	//    the Laplacian's alone, as before, and they are debanded identically.
	// A banded gradient is flat inside its bands (only the band edges count: 0.31 at
	// most for bands 8 px wide or wider, 0.40 for 4-code staircases at 0.8 code/px);
	// grain is busy on 0.86 (white, 0.5 code) and 0.81 (soft, 0.5 code; blocks 0.74 and
	// up) of the pixels at the 8-bit-in-10 threshold (CPU model, the GPU test's picture).
	// Blur is vetoed from kVetoLo (none) to kVetoHi (fully); bands 4 px wide are partly vetoed.
	constexpr double kLapShareOfThreshold = 1.0 / 3.0;
	constexpr double kExtremumShareOfThreshold = 1.0 / 12.0;
	constexpr double kVetoLo = 0.4, kVetoHi = 0.7;
	// The content-step part of the threshold: min(threshold, 1.5 8-bit steps), in codes.
	inline double GuardContentCodes(double thresholdCodes, int colorDepth)
	{
		const double eightBitStep = std::pow(2.0, (std::max)(0, colorDepth - 8));
		return (std::min)(thresholdCodes, 1.5 * eightBitStep);
	}
	// The Laplacian level (codes of the signal, colorDepth-scaled) above which a
	// pixel counts as busy: a third of the content step, snapped to the half code
	// above it (integer-coded input: busy from the next whole code up, as before).
	inline double GuardLaplacianCodes(double thresholdCodes, int colorDepth)
	{
		return std::floor(GuardContentCodes(thresholdCodes, colorDepth) * kLapShareOfThreshold + 1e-9) + 0.5;
	}
	// The step (codes) both neighbours on a line must exceed for a local extremum.
	inline double GuardExtremumCodes(double thresholdCodes, int colorDepth)
	{
		return GuardContentCodes(thresholdCodes, colorDepth) * kExtremumShareOfThreshold;
	}
	inline double Smoothstep(double e0, double e1, double x)
	{
		const double t = (std::max)(0.0, (std::min)(1.0, (x - e0) / (e1 - e0)));
		return t * t * (3.0 - 2.0 * t);
	}
}
