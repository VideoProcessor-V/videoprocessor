#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// DV tunnel loopback (2026-10-17): the receiving side. Captured frames go
// through VP's own tunnel formatter (CR210DolbyVisionTunnelFormatter, the
// decode the renderer runs) and the renderer's presentation decision
// (DolbyVisionTunnelPresentation.h); each frame is identified from its
// stamp in the raw RGB8 bytes, aligned to what was sent, and compared bit for
// bit: wire bytes, decoded planes, decoded metadata (DvMetadataModel.h), and
// optionally an independent reference check supplied by the caller.
//
// The report gives latency (frames and ms), dropped / repeated / out-of-order
// frames, black or wrong-kind frames, metadata mismatches and pixel errors.
#include "DvLoopbackPattern.h"
#include "DvMetadataModel.h"
#include <VideoFrame.h>
#include <VideoState.h>
#include <video_frame_formatter/CR210DolbyVisionTunnelFormatter.h>
#include <VP-V/modules/input/signal/DolbyVisionTunnelHandoff.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace DvLoopback
{
	// Anything that receives captured frames: the analyzer, a recorder.
	class IDvCaptureSink
	{
	public:
		virtual ~IDvCaptureSink() = default;
		virtual void OnState(const VideoStateComPtr& state) = 0;
		// arrivalQpc: when the frame reached VP (QPC ticks; 0 = unknown).
		virtual void OnFrame(const VideoFrame& frame, int64_t arrivalQpc) = 0;
	};

	// Handoff: the GUI's hand-off probe gave the signal to the normal path
	// (DolbyVisionTunnelHandoff.h, DV input fixes 2026-10-18).
	enum class Shown { Picture, Sdr, Black, Rejected, TunnelOff, Handoff };
	const char* ToString(Shown shown);

	struct FrameRecord
	{
		uint64_t ordinal = 0, counter = 0;
		int64_t timestamp = 0, arrivalQpc = 0;
		Shown shown = Shown::Black;
		bool identified = false;          // stamp read from the raw RGB8 bytes
		uint32_t sentIndex = 0;
		bool decodedStampAgrees = true;   // Picture: the decoded I plane's stamp
		uint64_t pixelMismatches = 0;
		uint64_t transportErrors = 0;     // wire bytes that differ from the sent RGB8
		size_t metadataMismatches = 0;
		bool referenceMismatch = false;
		bool wrongKind = false;           // e.g. a DV picture shown for an SDR frame
		double latencyMs = -1.0;
	};

	struct LoopReport
	{
		uint64_t captured = 0, identified = 0, unidentified = 0;
		uint64_t pictures = 0, sdr = 0, black = 0, rejected = 0, tunnelOff = 0;
		uint64_t handedOff = 0, handoffs = 0, takeBacks = 0;   // DV input fixes 2026-10-18
		uint64_t dropped = 0, repeated = 0, outOfOrder = 0;
		uint64_t pixelMismatchFrames = 0, pixelMismatchSamples = 0;
		uint64_t transportErrorFrames = 0, transportErrorBytes = 0;
		uint64_t metadataMismatchFrames = 0, referenceMismatchFrames = 0, wrongKindFrames = 0;
		uint64_t blackWhereDvExpected = 0, blackWhereSdrExpected = 0;
		int64_t frameOffset = 0;            // capture ordinal - sent index (median)
		double latencyMsMedian = -1.0, latencyMsMin = -1.0, latencyMsMax = -1.0;
		double latencyFrames = -1.0;        // median latency x the signal's rate
		std::vector<uint32_t> droppedIndices;
		std::vector<std::string> issues;    // the first few, human readable
		std::vector<FrameRecord> frames;
		std::vector<std::string> log;       // the formatter's log lines (when collected)

		bool BitExact() const
		{
			return pixelMismatchSamples == 0 && transportErrorBytes == 0 && metadataMismatchFrames == 0 &&
				referenceMismatchFrames == 0 && wrongKindFrames == 0;
		}
		bool Logged(const std::string& text) const;
		std::string Summary() const;
	};

	struct AnalyzerOptions
	{
		uint8_t sequenceId = 0x5A;
		// The frame that was sent with this index (false: unknown index).
		std::function<bool(uint32_t index, LoopFrame& frame)> expected;
		// The QPC tick each index was sent at (0 = unknown) and the QPC rate.
		std::function<int64_t(uint32_t index)> sentQpc;
		int64_t qpcFrequency = 0;
		// Independent reference: given the sent index and the captured RGB8,
		// an error text or empty. Catches packer errors the decoder shares.
		std::function<std::string(uint32_t index, const std::vector<uint8_t>& capturedRgb8)> reference;
		// A decoder fault model for tests (applied to the decoded planes, as a
		// bug shared with the packer would be); empty = none.
		std::function<void(std::vector<uint16_t>& planes, uint32_t width, uint32_t height)> decodedFault;
		bool collectLog = true;
		size_t maxIssues = 40;
		bool keepFrames = true;
		bool bugHuntFixes = true;            // dv_tunnel_fixes
		bool scalarUnpack = false;           // force the scalar CPU unpack
		// [general] dv_tunnel_handoff: run the GUI's hand-off probe on every
		// picture while the tunnel gate is open (DV input fixes 2026-10-18).
		bool handoff = true;
	};

	class DvLoopbackAnalyzer : public IDvCaptureSink
	{
	public:
		explicit DvLoopbackAnalyzer(const AnalyzerOptions& options);
		~DvLoopbackAnalyzer() override;
		void OnState(const VideoStateComPtr& state) override;
		void OnFrame(const VideoFrame& frame, int64_t arrivalQpc) override;
		// Latency, drops and repeats over everything received so far.
		LoopReport Finish();
		// The last decoded frame (tests): planes, metadata, presentation.
		const std::vector<uint16_t>& LastPlanes() const { return m_planes; }
		const DolbyVisionTunnelFrameMetadata& LastMetadata() const { return m_lastMetadata; }
		Shown LastShown() const { return m_lastShown; }
		bool HandedOff() const { return m_handoff.HandedOff(); }
		const DolbyVisionTunnelHandoffProbe& Handoff() const { return m_handoff; }

	private:
		void Issue(const std::string& text);
		AnalyzerOptions m_options;
		VideoStateComPtr m_state;
		std::unique_ptr<CR210DolbyVisionTunnelFormatter> m_formatter;
		std::vector<uint8_t> m_output;
		std::vector<uint16_t> m_planes;
		std::vector<uint8_t> m_rgb;
		DolbyVisionTunnelFrameMetadata m_lastMetadata;
		Shown m_lastShown = Shown::Black;
		LoopReport m_report;
		uint64_t m_ordinal = 0;
		bool m_logHooked = false;
		DolbyVisionTunnelHandoffProbe m_handoff;
	};
}
