#pragma once

#include "../QueuedFrameStatistics.h"
#include "LedMeasurement.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Lookahead statistics for mode: compensate (docs/VP-led-drive-answers-2.md
// B6). Queued frames are raw captures, not yet rendered, so they cannot be
// measured at PRE_OUTPUT. Instead each luma sample goes through a 1D transfer
// that predicts the rendered value (input transfer, tone map at the target
// nits, output_gamma; built by LedRawTransfer from the renderer's own
// colour-mapping settings), and the blocks are measured exactly like the GPU
// pass. The module corrects the predictions against the exact measurement of
// each frame once it is rendered.
namespace LedDrive
{
	class LedDriveModule;

	// Entries over the normalised input signal (0 = black, 1 = white after
	// the range expansion); values are the normalised PRE_OUTPUT signal in the
	// target transfer, before the gamma boost.
	constexpr int RAW_TRANSFER_SIZE = 1024;
	using RawTransfer = std::vector<float>;

	// The identity transfer (output = input), used until the renderer has
	// built a real one.
	RawTransfer IdentityRawTransfer();

	// Whether the luma reader returns full-range codes for this source (8-bit
	// and 12-bit RGB) rather than limited-range ones (YUV, 10-bit RGB). Its
	// RGB luma uses BT.709 (or BT.2020) weights where the exact pass uses
	// apl_weights; the correction ratio absorbs the difference.
	// audit 2026-10-16: false whenever kAnalysisLumaSampleLimitedDomain (the
	// sampler emits limited codes for every format).
	bool LumaIsFullRange(const AnalysisLumaSource& luma);
	// audit 2026-10-16: the source's own codes are full range and Sample()
	// mapped them to the limited domain (8-bit/12-bit RGB, DV SDR-tunnel RGB,
	// full-range YCbCr planes). MeasureRawFrame recovers the source code
	// before applying source_range, so the auto range reads these as full.
	bool SampleRemapsFullRange(const AnalysisLumaSource& luma);

	// Measures a raw frame the way the GPU pass measures a rendered one: 3x3
	// block means of the mapped luma, in code units, on a block grid of at
	// most 96 x 54 within the active picture (APL, histogram, cells; the
	// peak below). transfer: RAW_TRANSFER_SIZE
	// entries, already raised to the gamma boost. range: the captured
	// signal's range as the picture uses it (source_range: 1 full, 0
	// limited); -1 = from the format (LumaIsFullRange).
	// Hunt 2 (owner 2026-10-17): the look-ahead's measurement made the same
	// as the exact one. peakFullCoverage (led_peak_full_coverage): the peak
	// is AreaAveragedMaximum over the active picture, boxes the script's
	// share of the screen; screenWidth/Height: the output screen in this
	// frame's raster pixels (0 = the frame fills it). masks (led_picture_only,
	// raster pixels): left out of the peak and of every block. The peak
	// counts every pixel (FullCoveragePeak: a row reader, a code table and a
	// separable box sum, about 2-3 ms for a 4K P010 frame on one core).
	struct RawMeasureOptions
	{
		bool peakFullCoverage = true;
		double screenWidth = 0.0, screenHeight = 0.0;
		PixelRect masks[MAX_MASK_RECTS];
		int maskCount = 0;
	};

	// Every 10-bit code Sample() can return.
	constexpr int LUMA_CODES = 1024;
	using LumaValueTable = std::array<float, LUMA_CODES>;
	// The raw measurement's per-pixel mapping as a table: a Sample() luma
	// code to its boosted signal (normally 0..1) through `transfer` (RAW_TRANSFER_SIZE
	// entries) under `range` (as MeasureRawFrame).
	LumaValueTable PeakValueTable(const AnalysisLumaSource& luma, const std::vector<float>& transfer, int range);
	// Hunt 2 (look-ahead cost, owner 2026-10-17): AreaAveragedMaximum of the
	// table's values over [left,right) x [top,bottom) at every pixel, read a
	// row at a time (AnalysisLumaSource::SampleLumaRow) into an
	// AreaPeakAccumulator. Same boxes, masks and result (within float
	// rounding); -1 when no box was measured.
	double FullCoveragePeak(const AnalysisLumaSource& luma, int left, int top, int right, int bottom,
		const LumaValueTable& table, double screenW, double screenH, const PixelRect* masks, int maskCount);

	bool MeasureRawFrame(const AnalysisLumaSource& luma, const QueuedFrameStatistics::PictureRect& rect,
		const std::vector<float>& transfer, RawMeasurement& out, int range = -1,
		const RawMeasureOptions* options = nullptr);

	// Subscribes the module to the renderer's queued-frame walk. Each frame is
	// measured once, the first time the walk sees it, in source order, so the
	// raw cut and fade detection see every frame exactly once.
	class LookaheadObserver : public QueuedFrameStatistics::IObserver
	{
	public:
		explicit LookaheadObserver(LedDriveModule& module);
		void BeginWalk(size_t futureFrames) override;
		void ObserveFrame(uint64_t sourceSequence, size_t framesAhead,
			const AnalysisLumaSource& luma, const QueuedFrameStatistics::PictureRect& activePicture) override;
		// Efficiency round 2026-10-18: ObserveFrame's own early returns (predictions off, a frame
		// already measured), checked without measuring.
		bool WantsFrame(uint64_t sourceSequence) const override;
		// Hunt 2 (owner 2026-10-17), from the render thread every frame: the
		// output screen in source raster pixels (the script's box size; 0 =
		// the frame fills the screen) and the current frame's detected
		// subtitle rectangles in source raster pixels (led_picture_only; none
		// = unmasked). Queued frames have not been through the subtitle
		// detection yet, so the current frame's box stands for them: a cue
		// usually stays in place, and the exact measurement of each frame
		// corrects the prediction once it is rendered.
		void SetPictureGeometry(double screenWidth, double screenHeight, const PixelRect* masks, int count);

	private:
		LedDriveModule& module;
		StatsBuilder builder;
		uint64_t builderGeneration = 0;
		uint64_t lastSequence = 0;
		// Hunt 3 (LED1): the builder's clock. sourceSequence x the current
		// source period jumped backwards whenever the rate changed (pulldown
		// lock lost: film frames counted at the capture period) or a stream
		// restarted its sequence, so the persistence windows never expired.
		// Instead each newly measured frame advances it by the frames since
		// the last one at the current period: monotonic by construction.
		double streamTimeMs = 0.0;
		bool haveStreamTime = false;
		// Hunt 3 (LED2): LedDriveModule::ResetLookahead()'s count when this
		// observer last started a stream (its state is dropped on a change).
		uint64_t streamEpoch = 0;
		std::shared_ptr<const RawTransfer> transfer;
		std::vector<float> boosted;
		double boostedExponent = 0.0;   // the boost the table was built with
		std::mutex geometryMutex;
		RawMeasureOptions geometry;     // under geometryMutex
	};
}
