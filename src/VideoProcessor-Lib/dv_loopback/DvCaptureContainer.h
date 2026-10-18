#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// DV tunnel loopback (2026-10-17): how a capture card stores an HDMI RGB
// 4:4:4 8-bit picture in memory, and back. Used by the simulated loop (the
// capture card in software) and by the analyzer (reading recorded corpora).
//
// DeckLink 10-bit RGB containers (Blackmagic DeckLink SDK, pixel formats):
//   r210  bmdFormat10BitRGB     32-bit big-endian [2 pad | R10 | G10 | B10]
//   R10b  bmdFormat10BitRGBX    32-bit big-endian [R10 | G10 | B10 | 2 pad]
//   R10l  bmdFormat10BitRGBXLE  as R10b, little-endian
// An 8-bit channel lands in the 10-bit field either as byte << 2 ("high8",
// what a card does when it scales 8 to 10 bits) or as the byte itself
// ("low8"); VP's formatter accepts both and proves the placement by CRC.
//   ARGB  bmdFormat8BitARGB     bytes A R G B
//   BGRA  bmdFormat8BitBGRA     bytes B G R A
#include <VideoFrameEncoding.h>
#include <cstdint>
#include <vector>

namespace DvLoopback
{
	struct ContainerOptions
	{
		VideoFrameEncoding encoding = VideoFrameEncoding::R210;
		bool high8 = false;          // 10-bit containers: byte << 2 (true) or byte (false)
		uint32_t rowBytes = 0;       // 0 = width * 4 (no padding)
		unsigned r10Padding = 0;     // R10b/R10l bottom two bits (a reader must ignore them)
		bool rangeConvert = false;   // fault: the card rescales full 0-255 to 64-940 (breaks the tunnel)
		uint8_t rowPaddingByte = 0xA5;
	};

	inline bool IsTenBitRgb(VideoFrameEncoding e)
	{
		return e == VideoFrameEncoding::R210 || e == VideoFrameEncoding::R10b || e == VideoFrameEncoding::R10l;
	}

	inline uint32_t ContainerRowBytes(const ContainerOptions& o, uint32_t width)
	{
		return o.rowBytes ? o.rowBytes : width * 4;
	}

	// rgb: R, G, B per pixel (width * height * 3).
	inline void WrapRgb8(const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height,
		const ContainerOptions& o, std::vector<uint8_t>& out)
	{
		const uint32_t stride = ContainerRowBytes(o, width);
		out.assign(static_cast<size_t>(stride) * height, o.rowPaddingByte);
		for (uint32_t y = 0; y < height; ++y)
		{
			uint8_t* row = out.data() + static_cast<size_t>(y) * stride;
			for (uint32_t x = 0; x < width; ++x)
			{
				const uint8_t* px = rgb.data() + (static_cast<size_t>(y) * width + x) * 3;
				uint8_t* at = row + static_cast<size_t>(x) * 4;
				if (o.encoding == VideoFrameEncoding::ARGB_8BIT)
				{
					at[0] = 0xFF; at[1] = px[0]; at[2] = px[1]; at[3] = px[2];
					continue;
				}
				if (o.encoding == VideoFrameEncoding::BGRA_8BIT)
				{
					at[0] = px[2]; at[1] = px[1]; at[2] = px[0]; at[3] = 0xFF;
					continue;
				}
				uint32_t field[3];
				for (int c = 0; c < 3; ++c)
					field[c] = o.rangeConvert ? 64U + (px[c] * 876U + 127U) / 255U :
						(o.high8 ? static_cast<uint32_t>(px[c]) << 2 : px[c]);
				const uint32_t r210 = (field[0] << 20) | (field[1] << 10) | field[2];
				uint32_t word = r210;
				if (o.encoding != VideoFrameEncoding::R210)
					word = (r210 << 2) | (o.r10Padding & 3U);
				if (o.encoding == VideoFrameEncoding::R10l)
				{
					at[0] = static_cast<uint8_t>(word); at[1] = static_cast<uint8_t>(word >> 8);
					at[2] = static_cast<uint8_t>(word >> 16); at[3] = static_cast<uint8_t>(word >> 24);
				}
				else
				{
					at[0] = static_cast<uint8_t>(word >> 24); at[1] = static_cast<uint8_t>(word >> 16);
					at[2] = static_cast<uint8_t>(word >> 8); at[3] = static_cast<uint8_t>(word);
				}
			}
		}
	}

	// The RGB8 bytes back from a container, given the placement (10-bit
	// containers). Returns false for an unsupported encoding.
	inline bool UnwrapRgb8(const uint8_t* data, uint32_t width, uint32_t height, uint32_t stride,
		VideoFrameEncoding encoding, bool high8, std::vector<uint8_t>& rgb)
	{
		rgb.resize(static_cast<size_t>(width) * height * 3);
		for (uint32_t y = 0; y < height; ++y)
		{
			const uint8_t* row = data + static_cast<size_t>(y) * stride;
			for (uint32_t x = 0; x < width; ++x)
			{
				const uint8_t* at = row + static_cast<size_t>(x) * 4;
				uint8_t* px = rgb.data() + (static_cast<size_t>(y) * width + x) * 3;
				if (encoding == VideoFrameEncoding::ARGB_8BIT)
				{
					px[0] = at[1]; px[1] = at[2]; px[2] = at[3];
					continue;
				}
				if (encoding == VideoFrameEncoding::BGRA_8BIT)
				{
					px[0] = at[2]; px[1] = at[1]; px[2] = at[0];
					continue;
				}
				if (!IsTenBitRgb(encoding))
					return false;
				uint32_t word = encoding == VideoFrameEncoding::R10l ?
					(static_cast<uint32_t>(at[3]) << 24) | (static_cast<uint32_t>(at[2]) << 16) |
					(static_cast<uint32_t>(at[1]) << 8) | at[0] :
					(static_cast<uint32_t>(at[0]) << 24) | (static_cast<uint32_t>(at[1]) << 16) |
					(static_cast<uint32_t>(at[2]) << 8) | at[3];
				if (encoding != VideoFrameEncoding::R210)
					word >>= 2;
				const unsigned shift = high8 ? 2U : 0U;
				px[0] = static_cast<uint8_t>(((word >> 20) & 0x3FFU) >> shift);
				px[1] = static_cast<uint8_t>(((word >> 10) & 0x3FFU) >> shift);
				px[2] = static_cast<uint8_t>((word & 0x3FFU) >> shift);
			}
		}
		return true;
	}

	// An 8-bit ARGB/BGRA recording (e.g. VideoProcessorStreamRecorder, which
	// captures 8-bit RGB input as bmdFormat8BitARGB) re-contained losslessly as
	// r210 low8, the form VP's tunnel formatter reads. Every RGB8 byte is kept.
	inline void EightBitRgbToR210(const uint8_t* data, uint32_t width, uint32_t height, uint32_t stride,
		VideoFrameEncoding encoding, std::vector<uint8_t>& out)
	{
		std::vector<uint8_t> rgb;
		UnwrapRgb8(data, width, height, stride, encoding, false, rgb);
		ContainerOptions o;
		o.encoding = VideoFrameEncoding::R210;
		WrapRgb8(rgb, width, height, o, out);
	}
}
