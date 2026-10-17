#pragma once

// VP Renderer DirectShow filter: copy a system-memory DirectShow sample into
// the tightly packed layout VideoFrameEncoding.h documents (VideoState
// BytesPerRow/BytesPerFrame). Pure (no DirectShow classes, no MFC) for the CPU
// tests.

#include <cstddef>
#include <cstdint>
#include <string>

#include <VideoFrameEncoding.h>


namespace VPRendererFilter
{
	struct RepackSource
	{
		const uint8_t* data = nullptr;
		size_t size = 0;               // valid bytes at data
		uint32_t strideBytes = 0;      // plane-0 row pitch (also the CbCr pitch)
		uint32_t codedHeight = 0;      // rows in plane 0 (CbCr starts after them)
		bool bottomUp = false;         // packed RGB stored bottom row first
		uint32_t originX = 0;          // visible picture origin in the coded frame
		uint32_t originY = 0;
	};

	// Copies the width x height picture at (originX, originY) into dst, top row
	// first, rows tightly packed (TightRowBytes) and, for NV12/P010/P016, the
	// ceil(height / 2) CbCr rows directly after the luma rows. Bottom-up RGB is
	// flipped. False (with a reason) when the source is too small, the stride
	// is narrower than the picture, or dstSize != TightFrameBytes.
	bool RepackFrame(VideoFrameEncoding encoding, uint32_t width, uint32_t height,
		const RepackSource& source, uint8_t* dst, size_t dstSize,
		std::string* reason = nullptr);
}
