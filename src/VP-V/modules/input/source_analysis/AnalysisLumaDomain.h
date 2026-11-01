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
#include <cstdint>

#include <ColorSpace.h>
#include <VideoFrameEncoding.h>

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
