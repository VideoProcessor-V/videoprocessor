#pragma once

#include <string>
#include <vector>

// Kernel-matched re-base (docs/VP-stage12-build-plan.md; owner 2026-10-16: "1080p will
// be the most common variant that streaming upscales from so this one needs to work ...
// optimize for it").
//
// A player upscaling a decoded 1080p stream 2x to 4K applies a known kind of filter.
// When that filter is known, its least-squares inverse recovers the decoded 1080p
// picture almost exactly (measured on real x265 decodes upscaled by ffmpeg: +8 to +15 dB
// over a Lanczos-type downscale). The filter is identified by the reprojection residual:
// each candidate's inverse, upscaled again with that candidate, reproduces the 4K frame
// to the quantisation floor only for the right one.
//
// Safety: interpolating kernels only (a smoothing kernel such as the B-spline fits a
// natural-spline upscale perfectly but its inverse returns prefilter coefficients, 20 dB
// worse); a frame decides only when the best fit is at the floor and clearly ahead of
// the next. Separable, one axis at a time, exactly 2:1.
namespace KernelInverse
{
	struct Candidate
	{
		std::string name;
		// The inverse filter: weights over the 4K row around output sample i, at 4K
		// positions 2i + offset for offset = first .. first + weights.size() - 1.
		std::vector<float> weights;
		int first = 0;
		// The upscale's two phase filters (positions i - 0.25 and i + 0.25 of the
		// 1080 grid): weights over 1080 samples i + d, d = phaseFirst ..
		std::vector<float> phase[2];
		int phaseFirst = 0;
	};

	// The candidates (interpolating kernels): bicubic B=0 with C = 0.5 / 0.6 / 0.75,
	// Lanczos 2 / 3 / 4, bilinear. Computed once.
	const std::vector<Candidate>& Candidates();
	int Find(const std::string& name);   // index, or -1

	// One axis: the 1080 samples from a 4K line with candidate `c` (out = in.size()/2).
	void InverseLine(const Candidate& c, const float* in, int n4, float* out);
	// One axis: the 4K line from 1080 samples (out has 2 * n1 samples).
	void UpscaleLine(const Candidate& c, const float* in, int n1, float* out);

	struct TileFit
	{
		int best = -1;              // the candidate with the smallest residual
		double bestResidual = 0.0;  // RMS, in codes of `codeMaximum`
		double secondResidual = 0.0;
		std::vector<double> residuals;
	};
	// A square tile of the 4K frame (n x n, n even, values 0..1): each candidate's
	// inverse in both axes, upscaled again, against the tile (the interior only).
	TileFit FitTile(const float* tile, int n, int stride, int codeMaximum = 1023);

	// The frame decides for `best` when the best residual is at the quantisation floor
	// (`floorCodes`) and the next is at least `margin` times larger.
	bool Accept(const TileFit& fit, double floorCodes, double margin = 1.1);
	// The floor for content of `effectiveBits` in a signal of `signalBits` (RMS codes of
	// the signal): rounding to the content's step, with a 20% allowance.
	double FloorCodes(double effectiveBits, int signalBits = 10);
}
