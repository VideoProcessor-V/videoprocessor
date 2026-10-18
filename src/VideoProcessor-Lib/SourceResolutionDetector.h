#pragma once

#include <algorithm>
#include <cmath>
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
		// Hunt 2 M2/M3 (source_coding_fixes, Thresholds::codingFixes): the robust readings.
		// flatHistogram: codes of the low-activity 8x8 blocks only (range up to
		// 2^(bits - 5) codes: ramps of an upscale and texture left out). blockinessRobust:
		// the coding grid found at any period from 8 px to an eighth of the tile (an
		// upscaled 8/16-px grid at 8 / r px), any phase, its boundary up to 3 px wide,
		// against the other phases, on both axes (the weaker); blockPeriod is that period (0 = none). bandEdgesRobust:
		// 8x8 blocks holding a band edge with two dominant levels, ramp or noise allowed.
		std::vector<unsigned> flatHistogram;
		double blockinessRobust = 1.0, blockPeriod = 0.0;
		int bandEdgesRobust = 0;
		// Overlay text (owner 2026-10-16, subtitles drawn by the player at the output
		// resolution over an upscaled picture): the share of the tile's sharp steps
		// (neighbours a third of the range apart) inside its densest band a quarter of the
		// tile high; 0 when the tile has too few sharp steps to judge.
		double textBand = 0.0;
		// The separate lines of sharp steps (text lines) along the axis of textBand; 0 when the
		// tile has too few sharp steps to judge (mixed-screen guard: subtitles hold one or two).
		int textLines = 0;
		// The share of neighbour pairs (both axes) a third of the range apart: full-contrast steps
		// one pixel wide, which an upscale (an edge spread over 1 / r pixels) rarely makes.
		double sharpSteps = 0.0;
		double sharpStepsV = 0.0, sharpStepsH = 0.0;   // per axis: steps down the columns / across the rows
		// Planner Q5 (2026-10-16): the strongest repeat of either axis's autocorrelation
		// (the window's own removed) at a lag of 3 .. n / 4 that stands after a dip: near
		// 1 for exactly periodic content (fences, grilles, test patterns), 0 when none.
		double periodicPeak = 0.0;
		// Latch review 2026-10-17 (source_resolution_latch_fixes): the tile's pixel spread (standard
		// deviation, 0..1) and the means of its four quadrants (row-major) - the shape the cut test
		// compares, and the flat-bar test.
		double spread = 0.0;
		double quadrants[4] = {};
		// Player-added bars: the full columns (from the left / right edge) and rows (top / bottom)
		// that are constant (within two codes) and dark (at most a tenth of the range) - a bar beside
		// or above the picture.
		int barLeft = 0, barRight = 0, barTop = 0, barBottom = 0;
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
		// Planner G1 (2026-10-16): periodic content (grilles, bricks, text) - an axis reads
		// lower only when its curve's floor is at most this (real upscales 0.09-0.17).
		double maximumFloor = 0.35;
		// A single axis reads lower only up to this rate: real one-axis cases are 0.5
		// (vertical) to 0.75 (anamorphic 1440 / 1920); periodic content's (P-1)/P (e.g. 8/9
		// from text columns) sits above.
		double singleAxisMaximumRatio = 0.8;
		double informativeDecades = 1.5;  // lower band above the quantisation floor
		// Overlay text: a tile whose sharp steps sit this much in one band (a line of text) is
		// no evidence - left out of the frame (safe: evidence only lost). <= 0 or > 1: off
		// (source_resolution_text_guard off).
		double textBand = 0.8;
		// Planner Q5: a tile repeating itself this exactly (periodicPeak) carries no reliable
		// coherence either way and is left out (the three G1 guards stay for near-periodic
		// content). <= 0: off.
		double periodicPeak = 0.9;
		// Mixed screens (owner 2026-10-16): a native-resolution UI with a lower-resolution video
		// inside it (a streaming app's menu with a preview, a 4K info panel or transport bar over
		// a 1080 film, picture-in-picture). The re-base is whole-frame, so such a frame must count
		// as native - re-basing it to the video's rate would soften the UI. When a frame reads
		// lower, every tile is checked for native detail at that rate: power above the rate's
		// Nyquist at least `mixedDetailDecades` above the resampled tiles' (their lower quartile) and at
		// least `mixedDetailFloorDecades` above the quantisation floor (noise in a flat area is
		// not detail). Such a tile is
		//   - a native picture tile when it is not resampled at the rate (coherence below
		//     `coherence` on the axes read) and not text-like: one is enough (mixedPictureTiles);
		//   - an overlay tile otherwise (text, or sharp detail over the resampled picture,
		//     including the tiles the text guard left out): the frame is mixed only when the
		//     overlay tiles spread over at least two rows AND two columns of the tile grid -
		//     a subtitle (a band of rows at the bottom or top, or a column of vertical text)
		//     never does, so subtitles stay ignored as before.
		// A mixed frame reads native on every axis (FrameReading::mixed) and the tracker counts it
		// as native detail (it blocks a lower reading and disengages one).
		// false: off - the readings are exactly as before (source_resolution_mixed_guard off).
		bool mixedGuard = true;
		double mixedDetailDecades = 0.3;       // above the resampled tiles' detail above the rate
		double mixedDetailFloorDecades = 2.5;  // above the quantisation floor (step^2 / 12)
		// A tile's textBand at or above this is text-like (overlay). Natural pictures read 0.25-0.43,
		// one subtitle line ~1, two lines ~0.55 (measured on the synthetic fixtures).
		double mixedTextBand = 0.45;
		int mixedPictureTiles = 1;             // native picture tiles that make the frame mixed
		// Native UI confined to one tile row (a bottom-half EPG or info panel, a half-screen menu
		// over the film; intent review 2026-10-16): an overlay tile that is NOT text-like
		// (textBand below mixedTextBand - several lines of text, icons, grid lines, a panel edge,
		// rather than one or two subtitle lines in a band) makes the frame mixed wherever it is.
		// Subtitles (one line ~1, two lines ~0.55) stay ignored; three or more subtitle lines in
		// one tile would read as UI (the safe direction: native). false: overlay tiles count only
		// through the two-rows-and-two-columns spread.
		bool mixedRowGuard = true;
		// ... and an overlay tile holding at least this many text lines (Spectrum::textLines) is UI
		// too, text-like or not. 0: off (default) - the count is fragile: steep edges of the
		// video under a two-line subtitle split or add runs (4 counted on one fixture shot), and
		// a false mixed on subtitles disables the re-base on real films.
		int mixedTextLines = 0;
		// ... and a tile with at least this share of one-pixel full-contrast steps
		// (Spectrum::sharpSteps) is native, never the resampled picture (left out of the
		// reference); one that is informative and not text-like makes the frame mixed (with
		// mixedRowGuard). Upscales measure 0.0000 (r 1/3) - 0.004 (r 3/4), native picture
		// 0.014 - 0.018, UI text 0.03 - 0.05. <= 0: off.
		double mixedSharpSteps = 0.01;
		// Also mixed when one grid row holds at least this many overlay tiles, text-like or not.
		// 0: off (default) - a subtitle line drawn across the full width of every tile of a row
		// (SourceResolutionDetectorTests: SubtitleTextOverAnUpscale..., 4 tiles) is
		// indistinguishable from a one-line UI band by tile statistics alone.
		int mixedRowTiles = 0;
		// The tile grid for the overlay spread: tiles are row-major in rows of this many (the
		// monitors sample 4 x 2). <= 0: one row of all the tiles (the spread rule never fires).
		int gridColumns = 4;
		// Explicit tile centres, normalised to the picture (0..1), one per tile (PlaceTiles); when
		// both sizes match the tile count they replace the gridColumns layout. The overlay spread
		// is then: overlay tiles at least mixedSpreadHeight apart vertically AND mixedSpreadWidth
		// apart horizontally (on the 4 x 2 grid: two rows and two columns, as before; on the dense
		// grid a subtitle band touching two neighbouring rows, 0.125 apart, is not a spread).
		std::vector<float> tileX, tileY;
		double mixedSpreadHeight = 0.3, mixedSpreadWidth = 0.2;
		// The frame mean, signature and so the tracker's cut / repeat tests come from the first
		// this many tiles only (the base 4 x 2 grid, which PlaceTiles always puts first), so
		// switching between the base and the dense grid is neither a cut nor a new shot.
		// <= 0 or more than the tiles: all of them (as before).
		int signatureTiles = 0;
		int codeMaximum = 1023;           // the source's code range (floor)
		// Hunt 2 M2/M3 (source_coding_fixes): the coding readings (effective bits, blocking,
		// banding) from the robust measures (Spectrum::flatHistogram, blockinessRobust,
		// bandEdgesRobust); false: the earlier fixed 8/16-px grid, exact two-level bands and
		// one-pixel code occupancy, exactly as before.
		bool codingFixes = true;
		// Latch review 2026-10-17: [vprenderer] source_resolution_latch_fixes (default on; false =
		// the readings exactly as before):
		//   - a bar tile - in the outer columns (rows), holding a constant dark strip of at least
		//     barMinimumShare of the tile along its edge facing the picture's edge (a player-added
		//     pillar- or letterbox bar) - is left out of the averaged curves and of the mixed-screen
		//     guard (its sharp bar edge lifted the curve floor and read as native UI, so a 4:3 picture
		//     with player bars never engaged);
		//   - flat bar tiles (near-constant, at or near black) are left out of the frame mean, the
		//     signature and the cut pattern;
		//   - FrameReading::pattern / black / quantisationFloor are filled (the tracker's shape-aware
		//     cut test, the real-black engage, the level-aware native test).
		bool latchFixes = true;
		// The signal's black (normalised): limited range 16 << (bits - 8), full range 0.
		double blackLevel = 64.0 / 1023.0;
		// Flat: the tile's spread at most this many codes (the rounding / dither floor) ...
		double flatSpreadCodes = 2.0;
		// ... and its mean within this share of the range above black (a bar, a black frame).
		double nearBlackShare = 0.05;
		double barMinimumShare = 1.0 / 32.0;   // of the tile (8 px of a 256 px tile)
	};

	// The monitor's tile placement (owner 2026-10-15; dense grid 2026-10-16): tiles of `tile` px
	// on the 16-px grid inside the picture (x0, y0, w, h). The base grid is up to 4 x 2, each tile
	// centred in its cell, row-major. `dense` (the monitor sets it while a lower reading is
	// declared or being gathered) appends two more rows with the same columns, centred at h / 2 and
	// 7h / 8 - the largest gap (between the base rows) and the bottom band where transport bars and
	// corner picture-in-picture sit - when the picture is at least four tiles high. The base tiles
	// always come first and sit where they do without `dense`.
	struct TilePlace
	{
		int x0 = 0, y0 = 0;              // the tile's origin (pixels)
		float centreX = 0, centreY = 0;  // its centre, normalised to the picture
	};
	std::vector<TilePlace> PlaceTiles(double x0, double y0, double w, double h, int tile, bool dense, int* baseTiles = nullptr);
	// The centres as Thresholds::tileX / tileY.
	void SetTileCentres(const std::vector<TilePlace>& places, Thresholds& thresholds);

	// Enough texture on both axes to judge.
	bool Informative(const Spectrum& spectrum, const Thresholds& thresholds = {});
	// Planner G1 (2026-10-16): how much of a line spectrum the tile has - the strongest
	// non-coding bin of the upper band against the band's median, the larger axis.
	// Exactly periodic content (grilles, text, test patterns) is far above natural or
	// upscaled pictures.
	double Peakiness(const Spectrum& spectrum);

	struct MirrorPeak
	{
		double ratio = 1.0;       // the source rate (effective / native size); 1 = none found
		double coherence = 0.0;   // at the peak
		double runnerUp = 0.0;    // the best candidate away from the peak
		double grid = 0.0;        // coherence at 7/8: the 8x8 coding signature
		// The curve's median over the candidates: low (0.1-0.2) beside a resample's peak;
		// near 1 for exactly periodic content, whose harmonics lock at almost every rate.
		double floor = 0.0;
		// Strong peaks that are neither harmonics of the chosen rate nor coding-grid
		// positions: periodic content (its harmonics lock at many unrelated rates).
		int foreignPeaks = 0;
		// How far the strongest non-harmonic candidate stands above the chosen peak (a
		// resample's own rate is never outranked by an unrelated one; periodic text is).
		double outranked = 0.0;
	};

	// The strongest resample rate in one axis's averaged coherence curve.
	MirrorPeak FindPeak(const std::vector<double>& curve, int n, const Thresholds& thresholds = {});

	struct FrameReading
	{
		int tiles = 0, informativeTiles = 0;
		int textTiles = 0;          // tiles left out as overlay text
		int periodicTiles = 0;      // tiles left out as exactly periodic (planner Q5)
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
		double peakiness = 0.0;   // the largest over the tiles (Peakiness)
		// Mixed screen (Thresholds::mixedGuard): the frame read lower but holds native-resolution
		// content (a UI around or over the video); ratio, ratioV and ratioH were set to native.
		bool mixed = false;
		double mixedRatio = 1.0;      // the lower reading the guard overruled (diagnostic)
		int mixedPictureTiles = 0;    // tiles of native picture (not resampled at the reading's rate)
		int mixedOverlayTiles = 0;    // tiles of native text / sharp overlay
		int mixedOverlayRows = 0, mixedOverlayColumns = 0;   // the grid rows / columns they occupy
		int mixedSharpTiles = 0;      // informative, not text-like tiles of one-pixel steps (mixedSharpSteps)
		int mixedUiTiles = 0;         // overlay tiles that are not text-like (mixedRowGuard)
		int mixedRowMost = 0;         // the most overlay tiles in one grid row (mixedRowTiles)
		// Latch review 2026-10-17 (Thresholds::latchFixes):
		int barTiles = 0;             // tiles left out as holding a player-added bar
		// The signature tiles' quadrant means (4 per tile, tile order) and which belong to flat bar
		// tiles (not compared): the shape the cut test correlates.
		std::vector<double> pattern;
		std::vector<char> patternFlat;
		double blackLevel = 64.0 / 1023.0;
		double noiseLevel = 2.0 / 1023.0;   // the rounding floor of a level (flatSpreadCodes)
		// A real black: every tile flat at or near black (not only uninformative - a dark scene is).
		bool black = false;
		// log10 of the rounding noise's power per bin (step^2 / 12) at the picture's effective bit
		// depth (the signal's when unknown): the floor a native reading's detail must stand above.
		double quantisationFloor = -7.1;
		// The informative tiles' mean pixel spread relative to the range above black (0 = unknown):
		// the level-aware native test applies to dark / low-contrast frames only.
		double contrast = 0.0;
		bool informative() const { return informativeTiles > 0; }
	};

	// One frame from its tiles: the informative tiles' coherence curves
	// averaged per axis; a lower ratio only when both axes were resampled.
	FrameReading ReadFrame(const std::vector<Spectrum>& tiles, const Thresholds& thresholds = {});
	// The coding readings alone (part of ReadFrame).
	void ReadCoding(const std::vector<Spectrum>& tiles, FrameReading& frame, bool codingFixes = true);

	// Hunt 2 M2: the effective bits of a code histogram: 16-code windows with at least 256
	// pixels whose neighbouring windows hold within 3x as many (a smooth envelope: a narrow
	// grain peak is no coarse step), each window's occupancy as a Hill number of order 4
	// ((sum p^4)^(-1/3) / 16: equally used codes count fully, rarely used ones - an upscale's
	// ramp values - little); the median window. 0 when no window can be judged.
	double EffectiveBitsOf(const std::vector<unsigned long long>& histogram);
	// Hunt 2 M3: the coding-grid blockiness of one axis's mean absolute step per position
	// (`steps[x]` between x and x + 1). Returns 1 when no grid stands out; `period` gets the
	// grid's period in pixels (0 = none).
	double GridBlockiness(const std::vector<double>& steps, double& period);
	// Hunt 2 M3: an 8x8 block (row-major codes) holds a band edge: two dominant levels a
	// minimum to maximum step apart, at least 85 % of the block within a code of them (or
	// on the ramp between), split along a line (few transitions after a 3x3 majority).
	bool BandEdgeBlock(const int* block, int minimumStep, int maximumStep);

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
	// ... and the above band's mean log10 power on the axis that gave the result (`level`).
	double DetailAbove(const FrameReading& frame, double ratio, double* level);
	// The frame's averaged mirror coherence at `ratio`, the weaker axis (0 when unknown).
	double CoherenceAtRate(const FrameReading& frame, double ratio);
	// The same measure on one tile's spectra (the mixed-screen guard). `axes`: 1 vertical,
	// 2 horizontal, 3 both (the larger). `level` (optional) receives the mean log10 power of the
	// band above r's Nyquist on the axis that gave the result.
	double DetailAbove(const Spectrum& tile, double ratio, int axes = 3, double* level = nullptr);

	// One axis's reading as a frame of its own (the per-axis trackers): the ratio is
	// that axis's, and the detail-above test reads that axis's spectrum only.
	FrameReading AxisFrame(const FrameReading& frame, bool vertical);

	// Latch review 2026-10-17: the shape-aware cut test. The new pattern y against the old x (the
	// quadrant means; positions flat in either are skipped) is fitted as y = a + b x - a uniform
	// brightness change (a fade, a flash, an exposure change) fits exactly. Returns the residual
	// (RMS) relative to the new pattern's spread (sqrt(1 - correlation^2)) in `decorrelation` and to
	// the signal's level above black in `share`; false when the shape cannot be judged (fewer than 8
	// positions, or either pattern's spread within `noise`: black, a flat frame).
	bool PatternChange(const std::vector<double>& x, const std::vector<char>& flatX, const std::vector<double>& y,
		const std::vector<char>& flatY, double blackLevel, double noise, double& decorrelation, double& share, double* jump = nullptr);
	// `jump` (optional): the change of the mean level relative to the level above black.

	// The analysis tile for a picture `height` px high (latch review 2026-10-17: a fraction of the
	// height, 256 px at 2160, so the share of the picture covered does not shrink with the
	// resolution). A power of two (the FFT), at least 128 (the coherence statistics need the
	// lines x bins) and at most `maximum` (the analysis cost grows as n^3 per tile: above 256 it
	// would no longer fit the 0.5 s tile interval on the CPU worker).
	int TileSize(double height, int maximum = 256);

	// The exact rendition behind a measured rate (latch precision, 2026-10-17). FindPeak reads
	// the rate on the tile's own sweep - candidates m / n, bins 1/n apart - with a parabolic
	// sub-bin offset; on an asymmetric coherence peak the offset is biased upwards by a
	// fraction of a bin (measured +0.03 to +0.45 bin per frame, up to +0.52 at the window's
	// maximum, which the tracker declares), so a 128-px tile (bins 8.4 lines apart at 1080)
	// read a 540-line source as ~542 (0.4 % off) where a 256-px tile, bins half as wide,
	// rounded to 540. The re-base needs the true size: the source had a whole number of lines,
	// an even number as every rendition the monitor re-bases to. Of the even sizes of
	// `frameSize` within kSnapBins of the reading (0.75 / tile of the rate either way: the
	// measured error with a margin), the one whose ratio to the frame is the simplest
	// fraction - the smallest denominator, the nearest on a tie. Derived from the measurement
	// and the frame only, no list of classes: 540 in 1080 is exactly 1/2, 576 in 1080 8/15,
	// 900 in 2160 5/12. A size off any simple ratio (1000 of 2160 = 25/54) stays exact while
	// the reading is within about a fifth of a bin; further out a simpler neighbour within
	// reach is taken (the sweep cannot tell them apart). `frameSize` is the axis's whole frame
	// size (the height for the declared and vertical ratios, the width for the horizontal
	// one: a letterboxed or pillarboxed picture scales as the frame does). A ratio of 1 or
	// more, or no frame size / tile, comes back unchanged.
	constexpr double kSnapBins = 0.75;
	double SnapRatio(double ratio, int frameSize, int tile);

	// The declared reading over time.
	class Tracker
	{
	public:
		struct Settings
		{
			double tolerance = 0.03;        // hysteresis in ratio
			double nativeAbove = 0.9;       // a ratio this high reads native
			// Owner 2026-10-16: the standard is 3 shots and 10 s of evidence (20 s was longer than
			// needed: real 4K reads 0.1-0.2, upscales 0.9-0.99; the shots guard against a soft
			// stretch of a real film). The Picture slider sets both
			// (source_resolution_evidence_shots / _seconds).
			int minimumShots = 3;           // distinct shots agreeing before a lower reading
			double minimumSeconds = 10.0;   // and this long with evidence
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
			// Planner F12 (2026-10-16): a rendition change - adaptive streaming dropping from
			// its 4K rendition to a lower one mid-shot. At least `renditionFrames` consecutive
			// frames read the same lower rate with no cut, and their detail above that rate
			// fell at least `renditionDetailDrop` decades below the same shot's native frames:
			// the older evidence is cleared and the lower reading needs only
			// `renditionSeconds` of evidence (time only - a mid-shot switch has no shots).
			int renditionFrames = 6;
			double renditionDetailDrop = 0.3;
			double renditionSeconds = 10.0;
			// Latch review 2026-10-17: [vprenderer] source_resolution_latch_fixes (default on; false =
			// the behaviour before):
			//   - after native evidence the lower reading needs the standard again (minimumShots,
			//     minimumSeconds) from the readings since, not the whole window: a native reading
			//     (a mixed screen included) clears the window instead of holding it for windowSeconds;
			//   - a native reading whose detail above the rate in question sits within
			//     nativeFloorDecades of the rounding floor (FrameReading::quantisationFloor) is no
			//     evidence (a dark or low-contrast frame of an upscale);
			//   - cuts are a change of the picture's shape (PatternChange), not a fixed mean jump: the
			//     tile pattern decorrelated by at least cutDecorrelation together with a level jump of at
			//     least cutJump of the level above black, or decorrelated by cutDecorrelationAlone at any
			//     level - a fade or flash (a uniform change, decorrelation <= 0.21 measured) is never a
			//     cut, motion within a shot (level jump <= 0.11 at the 0.5 s spacing, decorrelation up to
			//     0.78 in a fast pan) is not, real cuts measured 0.39 - 0.90 jump / 0.55 - 0.99;
			//   - the rendition streak is time (renditionStreakSeconds), not a frame count;
			//   - a lower reading is declared on minimumShots OR minimumSeconds, whichever comes
			//     first (owner 2026-10-17; off: both).
			bool latchFixes = true;
			double nativeFloorDecades = 0.5;
			// ... and only on a dark / low-contrast frame (FrameReading::contrast below this share of
			// the range above black; unknown: always): a defocused native shot at a normal level stays
			// native evidence (owner: never re-base real 4K).
			double nativeFloorContrast = 0.04;
			double cutDecorrelation = 0.4;
			double cutJump = 0.2;
			double cutDecorrelationAlone = 0.9;
			// ... and the decorrelation must stand cutMotionMargin above the shot's own motion level
			// (the frames' decorrelation without a cut, averaged over cutMotionSeconds): a fast pan
			// decorrelates by itself (up to 0.78 at the 0.5 s spacing), and with a fade over it the
			// level jumps too.
			double cutMotionMargin = 0.3;
			double cutMotionSeconds = 1.5;
			// After a cut the new shot's motion is not known yet: it is taken as at least this (a fast
			// shot) until its own frames bring the level down (cutMotionSeconds).
			double cutMotionAfterCut = 0.5;
			double renditionStreakSeconds = 2.5;   // 6 analysed frames at the 0.5 s interval
		};

		// The shape-aware cut decision with the motion level it keeps (Tracker, look-ahead).
		struct CutJudge
		{
			double motion = 0.0;
			bool Judge(double decorrelation, double jump, double seconds, const Settings& s)
			{
				const double needed = motion + s.cutMotionMargin;
				const bool cut = (decorrelation >= (std::max)(s.cutDecorrelation, needed) && jump >= s.cutJump) ||
					decorrelation >= (std::max)(s.cutDecorrelationAlone, needed);
				if (cut)
					motion = (std::max)(motion, s.cutMotionAfterCut);
				else
				{
					const double a = s.cutMotionSeconds > 0.0 ? 1.0 - std::exp(-(std::max)(0.0, seconds) / s.cutMotionSeconds) : 1.0;
					motion += a * (decorrelation - motion);
				}
				return cut;
			}
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
		const Settings& GetSettings() const { return settings; }
		// The Picture slider's evidence (takes effect at the next frame).
		void SetEvidence(int shots, double seconds) { settings.minimumShots = shots; settings.minimumSeconds = seconds; }
		void SetLatchFixes(bool on) { settings.latchFixes = on; }
		// Native readings left out as within the rounding floor (latchFixes).
		unsigned long long FloorStarved() const { return floorStarved; }
		double Candidate() const;                                // the window's evidence so far
		// The window's median DetailAbove for frames reading `ratio`; +infinity when none.
		double ReferenceDetail(double ratio) const;
		int Shots() const;                                       // shots in the window
		double EvidenceSeconds() const;
		unsigned long long Cuts() const { return cuts; }
		// Latch fixes: the time between the two frames the last cut was found between (how well its
		// moment is known: a lost tile copy widens it).
		double LastCutSpan() const { return lastCutSpan; }
		unsigned long long RenditionChanges() const { return renditionChanges; }
		// The declared lower reading came from a rendition change (F12: time only, no shots), so
		// its evidence is judged against renditionSeconds alone (audit 2026-10-17: the monitor's
		// confidence also required minimumShots, so F12 could never engage auto mid-shot).
		bool DeclaredByRendition() const { return declaredByRendition; }
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
		unsigned long long shot = 0, cuts = 0, discontinuities = 0, starved = 0, floorStarved = 0;
		// latchFixes: the last frame whose shape could be judged (the cut test compares across black).
		std::vector<double> lastPattern;
		std::vector<char> lastPatternFlat;
		double lastPatternSeconds = -1.0, lastCutSpan = 0.0;
		CutJudge cutJudge;
		// F12: the same shot's last native frame (its spectra), and the run of lower readings.
		FrameReading lastNative;
		bool haveLastNative = false;
		unsigned long long lastNativeShot = 0;
		int streak = 0;
		double streakRatio = 1.0;
		std::vector<double> streakDetail;
		double streakStart = 0.0;
		bool renditionMode = false, declaredByRendition = false;
		unsigned long long renditionChanges = 0;
		std::string lastReason;
	};

	// "native" / "~1080 (0.50)" for the stats line.
	std::string Describe(double ratio, int nativeHeight);
}
