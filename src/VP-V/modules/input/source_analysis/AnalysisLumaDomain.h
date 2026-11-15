// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// VP-V DIV-AN-01 (either/or; merge 2026-10-31): the limited (studio) analysis code domain.
// Domain logic moved out of Bill's AnalysisLumaSource (M2 footprint, behaviour-free: the
// arithmetic, evaluation order and integer widths are exactly what lived in
// AnalysisLumaSource.cpp / .h). Included at the end of AnalysisLumaSource.h.
//
// With AnalysisLumaSource::limitedDomain set (the VP-V pathway: renderer key
// source_coding_fixes=on), every format's Sample() emits luma black 64 / white 940,
// chroma 512 +/- 448: full-range RGB (8-bit and 12-bit RGB, the DV SDR-tunnel RGB) is
// mapped Y = 64 + 876/1023 * Y', C = 512 + 896/1023 * C' (and BT.601-tagged RGB uses
// BT.601 coefficients); full-range YCbCr planes are mapped when fullRangeYuv is set.
// Without it (the default, the Original pathway) Sample() is Bill's v1.4.02 sampler
// exactly: delivered codes, with AnalysisLumaRange describing their levels.
// Consumers ask the source which domain its codes are in (CodesFullRange / CodesRange /
// MapsToLimitedDomain) and never apply a second full-range interpretation.
// kAnalysisLumaSampleLimitedDomain is the VP-V DEFAULT (source_coding_fixes defaults on),
// kept for consumers that only run in the VP-V pathway; per-source queries are authoritative.
//
// Source-level domain queries (one authority per source; Bill's model when the VP-V
// fields are clear):
// - FullRangeLevels: the source's encoded levels are full range (8/12-bit RGB, the
//   SDR tunnel unless tagged Limited, YCbCr flagged fullRangeYuv or tagged Full).
// - MapsToLimitedDomain: Sample() mapped those full-range levels to studio codes.
// - CodesFullRange: Sample() codes are full-range levels (black 0, white 1023).
// - CodesRange: AnalysisLumaRange of the codes Sample() returns: `range`, except
//   Limited where the VP-V map produced them (Bill's range-aware consumers use it so
//   a mapped source is not interpreted as full range a second time).
// fullRangeYuv (audit 2026-10-16; Bill never sets it): YCbCr plane formats (P010, P210,
// decoded DV 4:2:2, native 4:2:2) carry full-range codes; Sample() maps them to the
// limited domain. PrepareAnalysisFrame sets it only together with limitedDomain.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include <ColorSpace.h>
#include <VideoFrameEncoding.h>

#include "AnalysisAxis10.h" // S-2a: the axis laws (Q6 fractions, Shift/Scale decode); this header is the unit's public face

struct AnalysisLumaSample;
struct AnalysisLumaSource;

namespace Vpv { namespace AnalysisDomain
{
	// Same clamp as Bill's Clamp10 in AnalysisLumaSource.cpp.
	inline uint16_t ClampCode10(int value)
	{
		return static_cast<uint16_t>(std::max(0, std::min(1023, value)));
	}

	// Full-range RGB -> full-range Y'CbCr, Q16, applied to 10-bit codes.
	struct FullRgbCoefficients
	{
		int32_t yR, yG, yB;
		int32_t cbR, cbG, cbB;
		int32_t crR, crG, crB;
	};
	constexpr FullRgbCoefficients BT709_FULL = {
		13933, 46871, 4732, -7508, -25259, 32768, 32768, -29763, -3005
	};
	constexpr FullRgbCoefficients BT2020_FULL = {
		17218, 44444, 3886, -9147, -23621, 32768, 32768, -30134, -2634
	};
	constexpr FullRgbCoefficients BT601_FULL = { // audit 2026-10-16
		19595, 38470, 7471, -11058, -21710, 32768, 32768, -27439, -5329
	};

	inline bool IsBt601(ColorSpace colorspace)
	{
		return colorspace == ColorSpace::REC_601_525 ||
			colorspace == ColorSpace::REC_601_576 ||
			colorspace == ColorSpace::REC_601_625;
	}

	// Symmetric round-half-away-from-zero division; denominator > 0.
	inline int64_t RoundDivide(int64_t numerator, int64_t denominator)
	{
		return numerator >= 0 ? (numerator + denominator / 2) / denominator :
			-((-numerator + denominator / 2) / denominator);
	}

	// audit 2026-10-16: full-range 10-bit code (scaled by `scale`) to the
	// limited code domain every analysis consumer assumes. Monotonic and
	// unclamped before the final 10-bit container clamp, so codes outside
	// the nominal range keep their order (nothing is folded onto black/white).
	inline uint16_t FullLumaToLimited(int64_t scaledLuma, int64_t scale)
	{
		return ClampCode10(64 + static_cast<int>(
			RoundDivide(scaledLuma * 876, 1023LL * scale)));
	}

	inline uint16_t FullChromaToLimited(int64_t scaledOffset, int64_t scale)
	{
		return ClampCode10(512 + static_cast<int>(
			RoundDivide(scaledOffset * 896, 1023LL * scale)));
	}

	// SampleLumaRow's full-range luma: the limited-domain map, or Bill's Q16 rounding of
	// the delivered codes (Original pathway).
	inline uint16_t FullRgbLuma(const FullRgbCoefficients& f, bool mapLimited,
		int64_t r, int64_t g, int64_t b)
	{
		const int64_t sum = f.yR * r + f.yG * g + f.yB * b;
		return mapLimited ? FullLumaToLimited(sum, 65536) :
			ClampCode10(static_cast<int>((sum + 32768) >> 16));
	}
} }

// Defined after the struct (AnalysisLumaSource.h includes this header at its end).
#include "../../../../VideoProcessor-Lib/AnalysisLumaSource.h"

namespace Vpv { namespace AnalysisDomain
{
	// Sample()'s full-range RGB tail under limitedDomain (8-bit, R12B/R12L, SDR tunnel):
	// the Q16 full-range Y'CbCr sums are mapped straight to the limited code domain with a
	// single rounding (previously emitted as 0..1023 / +-511, which every darkness
	// threshold misread as studio codes).
	inline bool MapFullRgb(bool bt2020, bool bt601, uint16_t red, uint16_t green, uint16_t blue,
		AnalysisLumaSample& result)
	{
		const FullRgbCoefficients& full = bt2020 ? BT2020_FULL :
			(bt601 ? BT601_FULL : BT709_FULL);
		const int64_t r = red;
		const int64_t g = green;
		const int64_t b = blue;
		const int64_t ySum = full.yR * r + full.yG * g + full.yB * b;
		const int64_t cbSum = full.cbR * r + full.cbG * g + full.cbB * b;
		const int64_t crSum = full.crR * r + full.crG * g + full.crB * b;
		result.luma = FullLumaToLimited(ySum, 65536);
		result.chromaU = FullChromaToLimited(cbSum, 65536);
		result.chromaV = FullChromaToLimited(crSum, 65536);
		return true;
	}
} }

inline bool AnalysisLumaSource::FullRangeLevels() const
{
	// The levels Sample()'s RGB paths treat as full range (its limitedRgb rule), or a
	// YCbCr source flagged / tagged full range.
	if (format == AnalysisLumaFormat::NativeRgb)
		return !(encoding == VideoFrameEncoding::R210 || encoding == VideoFrameEncoding::R10b ||
			encoding == VideoFrameEncoding::R10l);
	if (format == AnalysisLumaFormat::SdrTunnelRgb)
		return range != AnalysisLumaRange::Limited;
	return fullRangeYuv || range == AnalysisLumaRange::Full;
}

inline bool AnalysisLumaSource::MapsToLimitedDomain() const
{
	// VP-V DIV-AN-01: RGB under limitedDomain; YCbCr planes flagged fullRangeYuv.
	if (format == AnalysisLumaFormat::NativeRgb || format == AnalysisLumaFormat::SdrTunnelRgb)
		return limitedDomain && FullRangeLevels();
	return fullRangeYuv;
}

inline bool AnalysisLumaSource::CodesFullRange() const
{
	return FullRangeLevels() && !MapsToLimitedDomain();
}

inline AnalysisLumaRange AnalysisLumaSource::CodesRange() const
{
	return MapsToLimitedDomain() ? AnalysisLumaRange::Limited : range;
}

// ---- 15c C6 (owner answer 3, 2026-11-15: "the central one should be able to be changed to accommodate for this") ----
// The decode owner's analysis output for every format, RGB and full-range 4:2:x included: the frame's shared luma plane
// (AnalysisLumaPlane, Sample(x, y).luma of every pixel, 10-bit codes) and the one sampler-code -> studio-code map. Moved
// here from VpFrameAnalysis.cpp, where the producer kept its own copy (checker site D6): same formats, same per-row
// decoder / per-pixel sampler order, same luma law, same range map, same rounding (FullLumaToLimited above), same 10-bit
// depth. VpFrameAnalysis re-exports DecodeSharedLuma and StudioLumaMap and reads them; it decodes nothing itself.
// Bit identity against the producer's former private decode: tests/DecodeOwnerIdentityTests.cpp.
namespace Vpv { namespace AnalysisDomain
{
	// True when the source's luma has to be decoded into the shared plane: its Sample().luma is not already a readable
	// 10-bit plane. The 16-bit plane formats (P010, P210, decoded DV 4:2:2) at limited range are read in place; at full
	// range (fullRangeYuv) Sample() maps their codes, so they are decoded like every other format.
	inline bool NeedsLumaPlane(const AnalysisLumaSource& source)
	{
		const bool planeFormat = source.format == AnalysisLumaFormat::P010 || source.format == AnalysisLumaFormat::P210 ||
			source.format == AnalysisLumaFormat::DecodedDolbyVision422;
		return source.IsValid() && !(planeFormat && !source.fullRangeYuv);
	}

	// Fills and publishes the source's shared plane once (the first caller that claims it): each row through the
	// row decoder (SampleLumaRow), else pixel by pixel through Sample(). False when there is nothing to do (no plane,
	// already claimed or published, an invalid source) or a pixel cannot be read (the plane is then never published and
	// every reader keeps decoding for itself).
	inline bool DecodeSharedLuma(const AnalysisLumaSource& source)
	{
		AnalysisLumaPlane* plane = source.sharedLuma.get();
		if (!plane || !source.IsValid() || plane->Ready() || !plane->TryClaim())
			return false;
		const int width = source.width, height = source.height;
		plane->Bind(source);
		// resize, not assign: a recycled plane of the same size keeps its storage and is not
		// zero-filled (every element is written below).
		plane->codes.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
		for (int y = 0; y < height; ++y)
		{
			uint16_t* row = plane->codes.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
			if (source.SampleLumaRow(0, y, width, row))
				continue;
			for (int x = 0; x < width; ++x)
			{
				AnalysisLumaSample sample;
				if (!source.Sample(x, y, sample))
				{
					plane->codes.clear();
					plane->codes.shrink_to_fit();
					return false;
				}
				row[x] = sample.luma;
			}
		}
		plane->Publish();
		return true;
	}

	// Sample().luma code (0..1023) -> 10-bit studio code: the identity when the codes are already studio codes, else
	// (CodesFullRange: Bill's delivered full-range codes, Original pathway) the full -> limited luma law above with one
	// rounding. The single range conversion for every VP analysis consumer (FrameAnalysis::Store records).
	inline std::array<uint16_t, 1024> StudioLumaMap(const AnalysisLumaSource& source)
	{
		std::array<uint16_t, 1024> map{};
		const bool full = source.CodesFullRange();
		for (int code = 0; code < 1024; ++code)
			map[static_cast<size_t>(code)] = full ? FullLumaToLimited(code, 1) : static_cast<uint16_t>(code);
		return map;
	}
} }
