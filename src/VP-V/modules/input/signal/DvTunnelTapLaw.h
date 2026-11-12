// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P10 D5 phase D1: the Dolby Vision tunnel's analysis-tap law (the CPU reference), 12-bit.
//
// The tunnel is a packed raw payload (r210 / R10b / R10l, 4 B per pixel, RGB8 in the high or the low
// eight bits of each 10-bit channel, carrying the 12-bit intensity I and the alternating chroma C).
// Owner ruling 2026-11-12: the pipeline is 12-bit capable end to end, so the tap carries the FULL 12-bit
// intensity (kBits = 12), not today's CPU tap's `>> 6` ten-bit code. The decode is the existing
// DolbyVisionTunnelUnpackReference (Intensity returns I in the high 12 bits of 16; `>> 4` is the plain
// 12-bit I); nothing here re-implements it. The legacy ten-bit law stays provable: Legacy10 = code12 >> 2
// (exact when limited; the full-range map differs by at most one code, as it rounds at another precision).
// AnalysisLumaSource.cpp (the CPU tap) is NOT changed (rule 32: no hook sites); P10 reads the 12-bit tap through SourceTap12.
//
//   code12 = Intensity(word, ContainerShift(container, highAligned)) >> 4
//   tap12  = fullRange ? FullLumaToLimited12(code12) : code12
//            FullLumaToLimited12(c) = clamp(256 + round(c * 3504 / 4095), 0, 4095)   (4 x the 10-bit map's
//            64 + round(c * 876 / 1023): black 256, white 3760, rounded half away from zero)
//   tap16  = tap12 << 4                                                      (12 bits in the high of 16)
//
// The full-range map is table-driven for the GPU: BuildRangeTable() fills the 4096 entries from the same
// function (an 8 KB R16 texture / uniform), exact by construction.

#include <VP-V/modules/input/signal/DolbyVisionTunnelUnpackReference.h>
#include <VP-V/modules/input/source_analysis/AnalysisLumaDomain.h>

#include <VideoFrameEncoding.h>

#include <cstddef>
#include <cstdint>

namespace DvTunnelTapLaw
{
	constexpr int kBits = 12;                     // the tap's bit depth
	constexpr int kLevels = 1 << kBits;           // 4096
	constexpr int kLegacyBits = 10;               // today's CPU tap
	constexpr int kLegacyShift = kBits - kLegacyBits;

	// rgbBitAlignment as AnalysisLumaSource carries it: 1 high, 2 low eight bits.
	inline bool SupportsAlignment(int alignment) { return alignment == 1 || alignment == 2; }

	// The tap covers a (container, alignment, width) the CPU tap accepts (AnalysisLumaSource::IsValid).
	inline bool Supports(VideoFrameEncoding container, int alignment, int width)
	{
		return DolbyVisionTunnelUnpackReference::SupportsContainer(container) && SupportsAlignment(alignment) &&
			width > 0 && (width & 1) == 0;
	}

	// The 12-bit intensity as the shader's Intensity() returns it (I in the high bits of 16).
	inline uint32_t Intensity16(const uint8_t* pixel, VideoFrameEncoding container, int alignment)
	{
		namespace R = DolbyVisionTunnelUnpackReference;
		const uint32_t shift = R::ContainerShift(container, alignment == 1);
		const bool bigEndian = R::ContainerBigEndian(container);
		const uint32_t word = bigEndian ?
			(static_cast<uint32_t>(pixel[0]) << 24) | (static_cast<uint32_t>(pixel[1]) << 16) |
				(static_cast<uint32_t>(pixel[2]) << 8) | static_cast<uint32_t>(pixel[3]) :
			(static_cast<uint32_t>(pixel[3]) << 24) | (static_cast<uint32_t>(pixel[2]) << 16) |
				(static_cast<uint32_t>(pixel[1]) << 8) | static_cast<uint32_t>(pixel[0]);
		return R::Intensity(word, shift);
	}

	// The 12-bit code before the range map: I itself.
	inline uint16_t Code12(const uint8_t* pixel, VideoFrameEncoding container, int alignment)
	{
		return static_cast<uint16_t>(Intensity16(pixel, container, alignment) >> 4);
	}

	// Full range -> the limited code domain at 12-bit precision.
	inline uint16_t FullLumaToLimited12(int code12)
	{
		const int64_t limited = 256 + Vpv::AnalysisDomain::RoundDivide(static_cast<int64_t>(code12) * 3504, 4095);
		return static_cast<uint16_t>(limited < 0 ? 0 : (limited > 4095 ? 4095 : limited));
	}

	inline uint16_t ToLimited(uint16_t code12, bool fullRange)
	{
		return fullRange ? FullLumaToLimited12(code12) : code12;
	}

	// The tap code, twelve bits, in the analysis domain.
	inline uint16_t TapCode(const uint8_t* pixel, VideoFrameEncoding container, int alignment, bool fullRange)
	{
		return ToLimited(Code12(pixel, container, alignment), fullRange);
	}

	// The lane's 16-bit tap sample: the twelve-bit code in the high bits.
	inline uint16_t Tap16(const uint8_t* pixel, VideoFrameEncoding container, int alignment, bool fullRange)
	{
		return static_cast<uint16_t>(TapCode(pixel, container, alignment, fullRange) << 4);
	}

	// The legacy ten-bit tap (AnalysisLumaSource's Sample().luma today), derived from the 12-bit tap: the
	// limited code >> 2. Exact when limited; when full range the CPU maps the ten-bit code, so it may differ
	// by one code (see the tests).
	inline uint16_t Legacy10(uint16_t tap12) { return static_cast<uint16_t>(tap12 >> kLegacyShift); }

	// The GPU's range table: entry c = FullLumaToLimited12(c) for c in 0..4095.
	inline void BuildRangeTable(uint16_t (&table)[kLevels])
	{
		for (int code = 0; code < kLevels; ++code)
			table[code] = FullLumaToLimited12(code);
	}
	// The 12-bit tap of one pixel of a packed DV tunnel analysis source (the side read of the CPU tap: Bill's
	// AnalysisLumaSource::Sample() keeps its ten-bit luma byte-identical and is NOT edited; P10 reads the same frame
	// bytes here). SourceT = AnalysisLumaSource (duck-typed so this header does not include it): needs data, rowBytes,
	// width, height, encoding, rgbBitAlignment, fullRangeYuv. Returns false when the pixel is outside the frame or the
	// container / alignment is not a tunnel the tap covers. The caller checks format == PackedDolbyVision422.
	template <typename SourceT>
	inline bool SourceTap12(const SourceT& source, int x, int y, uint16_t& tap12)
	{
		if (!source.data || x < 0 || y < 0 || x >= source.width || y >= source.height ||
			!Supports(source.encoding, source.rgbBitAlignment, source.width))
			return false;
		const uint8_t* row = source.data + static_cast<size_t>(y) * source.rowBytes;
		tap12 = TapCode(row + static_cast<size_t>(x) * 4, source.encoding, source.rgbBitAlignment, source.fullRangeYuv);
		return true;
	}
}
