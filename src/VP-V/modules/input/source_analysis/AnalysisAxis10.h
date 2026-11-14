// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// S-2a W1: the ONE analysis-luma decode law (rule 20: one copy; refs/119 rows 1, 2, 3, 9). Pure integer/float,
// no Windows, libplacebo or Bill headers, so every consumer (CPU tap, histograms, GLSL twins) includes the same
// constants. AnalysisLumaDomain.h (the DIV-AN-01 owner) re-exports it as the unit's public face.
//
// THE AXIS (coordinator ruling 2026-11-14 08:40, Q1 a: "12-bit as fractions on the 10-bit axis"): every analysis
// threshold keeps its 10-bit meaning (black 64, white 940 ...). A sample of ANY depth is carried as `Q6`: the 10-bit
// axis value times 64 (six fraction bits), which is exactly the 16-bit lane scale the half planes, P4 records and the
// matcher luma already use ("codes are on the 16-bit scale"). A 10-bit source is an integer on the axis
// (Q6 = code << 6, bit/decision identical to before); a 12-bit source gains 2 fraction bits (four codes per axis
// code); nothing is re-tuned.
//
// TWO decode laws, because the two legacy 12 -> 10 reductions differ and a 10-bit consumer must not move:
//   Shift  (YCbCr planes, the DV tunnel tap): code << (16 - bits). 12-bit I is `code12 << 4` = the lane value the unpack
//          returns (Intensity() >> 0), so the legacy `>> 6` ten-bit tap is Floor10FromQ6 of it, exactly.
//   Scale  (12-bit RGB, R12B / R12L): code * 1023 / codeMaximum with Q6 resolution; the legacy ten-bit code
//          (CDeckLinkRGBToP010 / AnalysisLumaSource Round12To10) is Round12To10 below, which this header owns a twin of.
// Both are exact for codeMaximum 1023 (Q6 = code * 64).

#include <cstdint>

namespace Vpv { namespace AnalysisDomain
{
	constexpr int kAxisBits = 10;                        // the analysis axis every threshold is written on
	constexpr uint32_t kAxisMax = (1u << kAxisBits) - 1u; // 1023
	constexpr int kAxisFractionBits = 6;                 // Q6: the 16-bit lane scale (16 - kAxisBits)
	constexpr uint32_t kAxisQ6Max = kAxisMax << kAxisFractionBits; // 65472: white of the Q6 axis
	constexpr uint32_t kAxisBins = 1u << kAxisBits;      // 1024: one histogram bin per whole axis code

	enum class AxisLaw : uint8_t { Shift, Scale };

	// A decoded native sample: its code and the depth it was declared at (codeMaximum = 2^bits - 1).
	struct Decoded
	{
		uint32_t code = 0;
		uint32_t codeMaximum = kAxisMax;
	};

	constexpr uint32_t CodeMaximum(int bits) { return bits >= 1 && bits <= 16 ? (1u << bits) - 1u : kAxisMax; }

	// Depth of a codeMaximum of the form 2^n - 1 (anything else reports the nearest power below).
	constexpr int BitsOf(uint32_t codeMaximum)
	{
		int bits = 0;
		while (bits < 16 && (codeMaximum >> bits) != 0) ++bits;
		return bits;
	}

	// The sample on the axis as Q6 (axis value * 64). Shift: bits <= 16 required (clamped).
	inline uint32_t AxisQ6(uint32_t code, uint32_t codeMaximum, AxisLaw law = AxisLaw::Shift)
	{
		if (codeMaximum == 0) return 0;
		if (code > codeMaximum) code = codeMaximum;
		if (law == AxisLaw::Scale)
			return (code * kAxisQ6Max + codeMaximum / 2u) / codeMaximum;
		const int bits = BitsOf(codeMaximum);
		return code << (16 - bits);
	}

	// The sample on the axis as a float: an integer for every 10-bit code, a multiple of 1/4 (Shift) for 12-bit.
	inline float ToAxis10(uint32_t code, uint32_t codeMaximum, AxisLaw law = AxisLaw::Shift)
	{
		return static_cast<float>(AxisQ6(code, codeMaximum, law)) * (1.0f / 64.0f);
	}

	inline float Q6ToAxis10(uint32_t q6) { return static_cast<float>(q6) * (1.0f / 64.0f); }

	// The whole axis code of a Q6 sample (the legacy ten-bit decision value): the `>> 6` every private decode used.
	inline uint32_t Floor10FromQ6(uint32_t q6) { return q6 >> kAxisFractionBits; }

	// Histogram bin of a Q6 sample (one bin per axis code, clamped) and the Q6 centre of a bin (P4 percentiles).
	inline uint32_t HistBinFromQ6(uint32_t q6)
	{
		const uint32_t bin = q6 >> kAxisFractionBits;
		return bin < kAxisBins ? bin : kAxisBins - 1u;
	}
	inline uint32_t BinCenterQ6(uint32_t bin) { return (bin << kAxisFractionBits) | (1u << (kAxisFractionBits - 1)); }

	// The legacy integer 12 -> 10 reduction of 12-bit RGB (Bill's Round12To10, AnalysisLumaSource.cpp; Original keeps
	// its own private copy untouched): nearest ten-bit code. Pinned against AxisQ6(Scale) by the unit tests.
	inline uint16_t Round12To10(uint16_t value)
	{
		return static_cast<uint16_t>((static_cast<uint32_t>(value) * 1023U + 2047U) / 4095U);
	}

	// 12-bit lane word (the 12 bits in the high of 16, as DolbyVisionTunnelUnpackReference::Intensity returns I) -> the
	// plain 12-bit code, and back.
	inline uint32_t Code12FromLane16(uint32_t lane16) { return lane16 >> 4; }
	inline uint32_t Lane16FromCode12(uint32_t code12) { return code12 << 4; }

	// Luma of 12-bit full-range RGB with Q16 weights, as the 12-bit code (rounded, clamped): the matcher's wide luma.
	inline uint32_t Luma12FromRgb12(int64_t weightR, int64_t weightG, int64_t weightB, int64_t r, int64_t g, int64_t b)
	{
		const int64_t sum = (weightR * r + weightG * g + weightB * b + 32768) >> 16;
		return static_cast<uint32_t>(sum < 0 ? 0 : (sum > 4095 ? 4095 : sum));
	}

	// GLSL twin of AxisQ6 for 12-bit Scale-law codes (the GPU unpack's matcher luma); generated from the constants above
	// so the text cannot drift from the CPU law (the parity test pins both). `au_axis_q6_scale(code, code_max)` is
	// integer-exact in 32 bits for code_max <= 4095 (code * 65472 < 2^28).
	inline const char* AxisGlsl()
	{
		return R"GLSL(
// VP-V analysis axis (AnalysisAxis10.h): 10-bit axis * 64 (Q6); Scale law = round(code * 1023 * 64 / code_max).
const int AU_AXIS_Q6_MAX = 65472;
int au_axis_q6_scale(int code, int code_max) { return (code * AU_AXIS_Q6_MAX + code_max / 2) / code_max; }
int au_axis_q6_shift(int code, int bits) { return code << (16 - bits); }
)GLSL";
	}
} }
