#pragma once

#include "../QueuedFrameStatistics.h"
#include "LedMeasurement.h"

#include <cstdint>
#include <memory>
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
	bool LumaIsFullRange(const AnalysisLumaSource& luma);

	// Measures a raw frame the way the GPU pass measures a rendered one: 3x3
	// block means of the mapped luma, in code units, on a block grid of at
	// most 96 x 54 within the active picture. transfer: RAW_TRANSFER_SIZE
	// entries, already raised to the gamma boost.
	bool MeasureRawFrame(const AnalysisLumaSource& luma, const QueuedFrameStatistics::PictureRect& rect,
		const std::vector<float>& transfer, RawMeasurement& out);

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

	private:
		LedDriveModule& module;
		StatsBuilder builder;
		uint64_t builderGeneration = 0;
		uint64_t lastSequence = 0;
		std::shared_ptr<const RawTransfer> transfer;
		std::vector<float> boosted;
		double boostedExponent = 0.0;   // the boost the table was built with
	};
}
