#pragma once

#include <deque>
#include <string>
#include <vector>

// Stage 12 step B (docs/VP-stage12-build-plan.md; owner 2026-10-15): the
// effective resolution of the source - the height an upstream upscaler started
// from - as a reading only. Nothing acts on it yet (step C).
//
// An upscale resamples the picture from a lower rate r (source lines per
// output line). Its spectrum is then periodic in r: the content at frequency f
// and at r - f are mirror copies with one fixed phase relation, the same for
// every frequency and every line. Real detail at the two frequencies is
// independent. Each tile's mirror coherence is measured per candidate r on
// both axes (a dense sweep, not a list of classes: owner's standing rule,
// general, not fixed); the height is r times the frame's own height. The
// coherence finds r to a fraction of a percent with any upscaler kernel.
//
// Biased to native (owner: "the detection of the base resolution doesn't
// trigger inaccurately"): a frame reads lower only when both axes were
// resampled (real detail on either axis keeps native) and block-coding edges
// are excluded; any detail above the declared rate returns to native at once,
// while a lower reading needs a sustained run across several shots; flat or
// dark tiles carry no evidence; anything uncertain stays native.
namespace SourceResolution
{
	// One tile's vertical (down the columns) and horizontal power spectra,
	// bins 0..n/2, normalised so white noise of variance v reads v per bin.
	struct Spectrum
	{
		int n = 0;
		std::vector<double> vertical, horizontal;
		double mean = 0.0;   // the tile's mean level (for cut detection)
		// Mirror coherence per candidate source rate m / n (index m): near 0
		// for real detail, high where the picture was resampled from rate m / n.
		std::vector<double> mirrorVertical, mirrorHorizontal;
		// Coding (spec 1b / 2d): the code histogram, blocking (boundary step
		// against the others at the 8 / 16 px periods; 1 = none), the mean
		// step, and the 8x8 blocks that are flat or hold one band edge.
		std::vector<unsigned> histogram;
		double blockiness = 1.0, gradient = 0.0;
		int blocks = 0, bandEdges = 0, flatBlocks = 0;
	};

	// `n` must be a power of two (64..1024); `luma` is row-major, `stride` in
	// elements, values in 0..1 (code / max code).
	// `codeMaximum` is the source's code range (1023 for 10-bit) for the coding
	// measures; the tile should sit on the frame's 16-pixel grid.
	bool Analyse(const float* luma, int stride, int n, Spectrum& out, int codeMaximum = 1023);
	void MeasureCoding(const float* luma, int stride, int n, int codeMaximum, Spectrum& out);

	struct Thresholds
	{
		double minimumRatio = 0.25;       // lowest candidate (a quarter of the height)
		double maximumRatio = 0.95;       // highest candidate (above reads native)
		double coherence = 0.4;           // mirror coherence that marks a resample (real detail ~0.1-0.2)
		double gridMargin = 0.2;          // a block-grid candidate must clear the other grid positions by this
		double gridQuiet = 0.3;           // ... and they must all be below this (no coding signature),
		double gridPeakMargin = 0.1;      // ... or it stands this far above both grid neighbours (coding cannot)
		double harmonicShare = 0.85;      // a sub-multiple this strong is the fundamental
		double axisAgreement = 0.05;      // an axis takes the other's rate when its curve holds it within this of its own peak
		// Per-axis readings (owner 2026-10-16: a source upscaled in one direction only):
		// stricter than the both-axes rule - one axis must be clearly resampled.
		double singleAxisCoherence = 0.5;
		double informativeDecades = 1.5;  // lower band above the quantisation floor
		int codeMaximum = 1023;           // the source's code range (floor)
	};

	// Enough texture on both axes to judge.
	bool Informative(const Spectrum& spectrum, const Thresholds& thresholds = {});

	struct MirrorPeak
	{
		double ratio = 1.0;       // the source rate (effective / native size); 1 = none found
		double coherence = 0.0;   // at the peak
		double runnerUp = 0.0;    // the best candidate away from the peak
		double grid = 0.0;        // coherence at 7/8: the 8x8 coding signature
	};

	// The strongest resample rate in one axis's averaged coherence curve.
	MirrorPeak FindPeak(const std::vector<double>& curve, int n, const Thresholds& thresholds = {});

	struct FrameReading
	{
		int tiles = 0, informativeTiles = 0;
		double ratio = 1.0;         // effective / native height; 1 = native
		// Per axis: the height (vertical, down the columns) and the width (horizontal)
		// read alone, at the stricter single-axis coherence; 1 = native on that axis.
		double ratioV = 1.0, ratioH = 1.0;
		double mean = 0.0;          // mean level over the tiles
		double signature = 0.0;     // the tile means, weighted by position: an unchanged frame repeats it
		MirrorPeak vertical, horizontal;
		std::vector<double> verticalCurve, horizontalCurve;   // the averaged coherence per candidate (diagnostic)
		std::vector<double> verticalPower, horizontalPower;   // the informative tiles' averaged power spectra
		// Spec 1b: 1 = clean, 0 = badly blocked or banded; spec 2d: the bits
		// the picture really uses (8 for 8-bit content in a 10-bit signal,
		// also after a curve such as SDR re-encoded to PQ); 0 = unknown.
		double bitrateQuality = 1.0, effectiveBits = 0.0;
		double blockiness = 1.0, bandShare = 0.0;
		bool informative() const { return informativeTiles > 0; }
	};

	// One frame from its tiles: the informative tiles' coherence curves
	// averaged per axis; a lower ratio only when both axes were resampled.
	FrameReading ReadFrame(const std::vector<Spectrum>& tiles, const Thresholds& thresholds = {});
	// The coding readings alone (part of ReadFrame).
	void ReadCoding(const std::vector<Spectrum>& tiles, FrameReading& frame);

	// Variable-bitrate streams (owner 2026-10-16): a starved frame can lose an
	// upscale's mirror coherence and read "none found" (1) without holding any
	// real detail. Real detail above a source rate r shows as picture power
	// above r's Nyquist; an upscale leaves only its kernel's leakage and coding
	// noise there, and a starved native frame has lost it too. Returns, in
	// decades, the power just above r's Nyquist against the power just below
	// it, on the axis holding more (detail on either axis keeps native).
	// The level depends on the content (sharp 4K ~-0.5, soft 4K ~-1.2) and on
	// the upscaler (-0.8 .. -1.3), so the tracker compares a frame with the
	// frames of the same stream that read r, not with a fixed level. Unknown
	// (no spectra, or r too close to 1 to measure above it) returns +infinity.
	double DetailAbove(const FrameReading& frame, double ratio);

	// One axis's reading as a frame of its own (the per-axis trackers): the ratio is
	// that axis's, and the detail-above test reads that axis's spectrum only.
	FrameReading AxisFrame(const FrameReading& frame, bool vertical);

	// The declared reading over time.
	class Tracker
	{
	public:
		struct Settings
		{
			double tolerance = 0.03;        // hysteresis in ratio
			double nativeAbove = 0.9;       // a ratio this high reads native
			// Owner 2026-10-16: the standard is 3 shots and 20 s of evidence (the earlier minute was
			// longer than needed: real 4K reads 0.1-0.2, upscales 0.9-0.99); the Picture slider
			// will offer faster or more cautious settings.
			int minimumShots = 3;           // distinct shots agreeing before a lower reading
			double minimumSeconds = 20.0;   // and this long with evidence
			double windowSeconds = 120.0;   // the rolling evidence window
			double cutLevel = 20.0 / 255.0; // tile mean jump that marks a cut (Stage 8's rule)
			double maximumGapSeconds = 2.0; // a longer gap between frames is a discontinuity
			// A frame reading none found (1) counts as native detail only when it holds
			// more detail above the rate in question than the window's frames that read
			// that rate (their median DetailAbove) by this many decades; else it is no
			// evidence either way - a starved frame of a variable-bitrate stream. Coding
			// moves an upscale's level by under 0.05; sharp 4K stands 0.10 (a bilinear
			// 3/4) to 0.7 (1080) above.
			double detailMarginDecades = 0.07;
		};

		Tracker() = default;
		explicit Tracker(const Settings& s) : settings(s) {}

		// One analysed source frame at `seconds` (the source's clock). A
		// `repeated` frame (or one identical to the last) adds no evidence and
		// no time, so a paused or frozen picture never builds a reading.
		void Update(const FrameReading& frame, double seconds, bool repeated = false);
		// Seek / skip, source or format change, signal loss, resync, drops:
		// back to native and start the evidence again.
		void Discontinuity(const char* reason);
		void Reset();
		unsigned long long Discontinuities() const { return discontinuities; }
		const std::string& LastDiscontinuity() const { return lastReason; }

		double Ratio() const { return declared; }               // 1 = native
		double Candidate() const;                                // the window's evidence so far
		// The window's median DetailAbove for frames reading `ratio`; +infinity when none.
		double ReferenceDetail(double ratio) const;
		int Shots() const;                                       // shots in the window
		double EvidenceSeconds() const;
		unsigned long long Cuts() const { return cuts; }
		// Frames reading none found without detail above the rate in question (no evidence).
		unsigned long long Starved() const { return starved; }
		// Height in lines for a native height, rounded to an even number; the
		// native height itself when native.
		int Height(int nativeHeight) const;

	private:
		struct Sample { double seconds; double ratio; unsigned long long shot; double dt; double detail; };
		Settings settings;
		std::deque<Sample> window;
		double declared = 1.0;
		double lastMean = -1.0, lastSignature = 0.0, lastSeconds = -1.0, evidence = 0.0;
		bool haveLast = false;
		unsigned long long shot = 0, cuts = 0, discontinuities = 0, starved = 0;
		std::string lastReason;
	};

	// "native" / "~1080 (0.50)" for the stats line.
	std::string Describe(double ratio, int nativeHeight);
}
