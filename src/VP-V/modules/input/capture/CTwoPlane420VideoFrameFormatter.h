// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <video_frame_formatter/IVideoFrameFormatter.h>
#include <VideoFrameEncoding.h>


// ABI 37 (VP Renderer DirectShow filter): file frames in the two-plane 4:2:0
// encodings (NV12, P010, P016; layout in VideoFrameEncoding.h) into the
// renderer's 16-bit two-plane layout (the same planes as the capture P010
// formatters: a luma plane of width 16-bit samples per row, then ceil(height/2)
// rows of interleaved CbCr, both with a row stride of width * 2 bytes).
//
// P010 / P016 are already in that layout and are copied; NV12 is widened to
// 16-bit words with its eight bits in the high byte (contract 8 / shift 8, as
// the UYVY formatters). No sample value changes. The source range is not
// carried by VideoState, so the contract is limited range (video levels).
// Odd widths are refused: the renderer's chroma plane needs an even width.
class CTwoPlane420VideoFrameFormatter :
	public IVideoFrameFormatter
{
public:
	// The renderer-side layout for a frame of this size (pure; tested).
	static size_t OutRowBytes(uint32_t width) { return static_cast<size_t>(width) * 2; }
	static uint32_t ChromaRows(uint32_t height) { return (height + 1) / 2; }
	static VideoFrameFormatterOutputContract ContractFor(VideoFrameEncoding encoding);

	void OnVideoState(VideoStateComPtr& videoState) override;
	bool FormatVideoFrame(const VideoFrame& inFrame, BYTE* outBuffer) override;
	LONG GetOutFrameSize() const override;
	VideoFrameFormatterOutputContract GetOutputContract() const override
	{
		return ContractFor(m_encoding);
	}

private:
	VideoFrameEncoding m_encoding = VideoFrameEncoding::UNKNOWN;
	uint32_t m_width = 0;
	uint32_t m_rows = 0;          // luma rows + chroma rows
	size_t m_inRowBytes = 0;
	size_t m_outRowBytes = 0;
	size_t m_outFrameBytes = 0;
};
