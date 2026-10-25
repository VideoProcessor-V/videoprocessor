#include "pch.h"

#include "AnalysisLumaSource.h"
#include "vprenderer/DolbyVisionTunnelUnpackReference.h"

#include <algorithm>
#include <array>
#include <limits>
#if defined(__AVX__)
#include <immintrin.h>
#endif

namespace
{
	bool CheckedMultiply(size_t left, size_t right, size_t& result)
	{
		if (left != 0 && right > std::numeric_limits<size_t>::max() / left)
			return false;
		result = left * right;
		return true;
	}

	uint16_t P010Code(const uint8_t* address)
	{
		return static_cast<uint16_t>((static_cast<uint16_t>(address[0]) |
			(static_cast<uint16_t>(address[1]) << 8)) >> 6);
	}

	// DV output stores exact 12-bit codes in the high bits of 16-bit lanes.
	// The established detector contract uses the upper ten bits (no rounding).
	uint16_t DecodedDolbyVisionCode(const uint8_t* address)
	{
		return P010Code(address);
	}

	uint32_t ReadBigEndian32(const uint8_t* address)
	{
		return (static_cast<uint32_t>(address[0]) << 24) |
			(static_cast<uint32_t>(address[1]) << 16) |
			(static_cast<uint32_t>(address[2]) << 8) |
			static_cast<uint32_t>(address[3]);
	}

	uint32_t ReadLittleEndian32(const uint8_t* address)
	{
		return static_cast<uint32_t>(address[0]) |
			(static_cast<uint32_t>(address[1]) << 8) |
			(static_cast<uint32_t>(address[2]) << 16) |
			(static_cast<uint32_t>(address[3]) << 24);
	}

	uint16_t Expand8To10(uint8_t value)
	{
		return static_cast<uint16_t>((static_cast<uint32_t>(value) * 1023 + 127) / 255);
	}

	bool ReadNativeYuv422(const AnalysisLumaSource& source, int x, int y,
		AnalysisLumaSample& result)
	{
		const uint8_t* row = source.data +
			static_cast<size_t>(y) * source.rowBytes;
		if (source.encoding == VideoFrameEncoding::UYVY ||
			source.encoding == VideoFrameEncoding::HDYC)
		{
			const uint8_t* pair = row + static_cast<size_t>(x / 2) * 4;
			result.chromaU = static_cast<uint16_t>(pair[0]) << 2;
			result.luma = static_cast<uint16_t>(pair[(x & 1) ? 3 : 1]) << 2;
			result.chromaV = static_cast<uint16_t>(pair[2]) << 2;
			return true;
		}
		if (source.encoding != VideoFrameEncoding::V210)
			return false;

		const uint8_t* group = row + static_cast<size_t>(x / 6) * 16;
		const uint32_t words[4] = {
			ReadLittleEndian32(group),
			ReadLittleEndian32(group + 4),
			ReadLittleEndian32(group + 8),
			ReadLittleEndian32(group + 12)
		};
		auto component = [&words](int word, int shift)
		{
			return static_cast<uint16_t>((words[word] >> shift) & 0x3ff);
		};
		switch (x % 6)
		{
		case 0:
			result = { component(0, 10), component(0, 0), component(0, 20) };
			break;
		case 1:
			result = { component(1, 0), component(0, 0), component(0, 20) };
			break;
		case 2:
			result = { component(1, 20), component(1, 10), component(2, 0) };
			break;
		case 3:
			result = { component(2, 10), component(1, 10), component(2, 0) };
			break;
		case 4:
			result = { component(3, 0), component(2, 20), component(3, 10) };
			break;
		default:
			result = { component(3, 20), component(2, 20), component(3, 10) };
			break;
		}
		return true;
	}

	uint16_t Clamp10(int value)
	{
		return static_cast<uint16_t>(std::max(0, std::min(1023, value)));
	}

	struct LimitedRgbCoefficients
	{
		int32_t yR, yG, yB;
		int32_t cbR, cbG, cbB;
		int32_t crR, crG, crB;
	};

	constexpr LimitedRgbCoefficients BT709_R10 = {
		222927, 749942, 75707, -122880, -413378, 536258,
		536258, -487086, -49172
	};
	constexpr LimitedRgbCoefficients BT709_R210 = {
		217951, 733202, 74017, -120138, -404150, 524288,
		524288, -476214, -48074
	};
	constexpr LimitedRgbCoefficients BT2020_R10 = {
		275461, 710935, 62181, -149755, -386503, 536258,
		536258, -493128, -43130
	};
	constexpr LimitedRgbCoefficients BT2020_R210 = {
		269312, 695065, 60793, -146413, -377875, 524288,
		524288, -482120, -42168
	};
	// audit 2026-10-16: BT.601 (Kr 0.299, Kb 0.114) with the same scaling
	// as the BT.709 tables above (R10: luma 1, chroma 896/876; r210: luma
	// 876/896, chroma 1). Rows sum to the BT.709 rows' totals.
	constexpr LimitedRgbCoefficients BT601_R10 = {
		313524, 615514, 119538, -180972, -355286, 536258,
		536258, -449049, -87209
	};
	constexpr LimitedRgbCoefficients BT601_R210 = {
		306526, 601775, 116869, -176934, -347354, 524288,
		524288, -439026, -85262
	};

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

	bool IsBt601(ColorSpace colorspace)
	{
		return colorspace == ColorSpace::REC_601_525 ||
			colorspace == ColorSpace::REC_601_576 ||
			colorspace == ColorSpace::REC_601_625;
	}

	// Symmetric round-half-away-from-zero division; denominator > 0.
	int64_t RoundDivide(int64_t numerator, int64_t denominator)
	{
		return numerator >= 0 ? (numerator + denominator / 2) / denominator :
			-((-numerator + denominator / 2) / denominator);
	}

	// audit 2026-10-16: full-range 10-bit code (scaled by `scale`) to the
	// limited code domain every analysis consumer assumes. Monotonic and
	// unclamped before the final 10-bit container clamp, so codes outside
	// the nominal range keep their order (nothing is folded onto black/white).
	uint16_t FullLumaToLimited(int64_t scaledLuma, int64_t scale)
	{
		return Clamp10(64 + static_cast<int>(
			RoundDivide(scaledLuma * 876, 1023LL * scale)));
	}

	uint16_t FullChromaToLimited(int64_t scaledOffset, int64_t scale)
	{
		return Clamp10(512 + static_cast<int>(
			RoundDivide(scaledOffset * 896, 1023LL * scale)));
	}

	int RoundQ20(int64_t value)
	{
		constexpr int64_t half = 1LL << 19;
		return value >= 0 ? static_cast<int>((value + half) >> 20) :
			-static_cast<int>(((-value) + half) >> 20);
	}

	uint16_t Round12To10(uint16_t value)
	{
		return static_cast<uint16_t>(
			(static_cast<uint32_t>(value) * 1023U + 2047U) / 4095U);
	}

	bool ReadR12BPixel(const uint8_t* source, int x,
		uint16_t& red, uint16_t& green, uint16_t& blue)
	{
		const uint8_t* block = source + static_cast<size_t>(x / 8) * 36;
		auto byte = [block](int word, int index) noexcept
		{
			return block[word * 4 + index];
		};
		auto low = [&byte](int word, int index) noexcept
		{
			return static_cast<uint16_t>(byte(word, index) & 0x0f);
		};
		auto high = [&byte](int word, int index) noexcept
		{
			return static_cast<uint16_t>(byte(word, index) >> 4);
		};
		switch (x % 8)
		{
		case 0:
			red = byte(0, 3) | (low(0, 2) << 8);
			green = high(0, 2) | (static_cast<uint16_t>(byte(0, 1)) << 4);
			blue = byte(0, 0) | (low(1, 3) << 8);
			break;
		case 1:
			red = high(1, 3) | (static_cast<uint16_t>(byte(1, 2)) << 4);
			green = byte(1, 1) | (low(1, 0) << 8);
			blue = high(1, 0) | (static_cast<uint16_t>(byte(2, 3)) << 4);
			break;
		case 2:
			red = byte(2, 2) | (low(2, 1) << 8);
			green = high(2, 1) | (static_cast<uint16_t>(byte(2, 0)) << 4);
			blue = byte(3, 3) | (low(3, 2) << 8);
			break;
		case 3:
			red = high(3, 2) | (static_cast<uint16_t>(byte(3, 1)) << 4);
			green = byte(3, 0) | (low(4, 3) << 8);
			blue = high(4, 3) | (static_cast<uint16_t>(byte(4, 2)) << 4);
			break;
		case 4:
			red = byte(4, 1) | (low(4, 0) << 8);
			green = high(4, 0) | (static_cast<uint16_t>(byte(5, 3)) << 4);
			blue = byte(5, 2) | (low(5, 1) << 8);
			break;
		case 5:
			red = high(5, 1) | (static_cast<uint16_t>(byte(5, 0)) << 4);
			green = byte(6, 3) | (low(6, 2) << 8);
			blue = high(6, 2) | (static_cast<uint16_t>(byte(6, 1)) << 4);
			break;
		case 6:
			red = byte(6, 0) | (low(7, 3) << 8);
			green = high(7, 3) | (static_cast<uint16_t>(byte(7, 2)) << 4);
			blue = byte(7, 1) | (low(7, 0) << 8);
			break;
		default:
			red = high(7, 0) | (static_cast<uint16_t>(byte(8, 3)) << 4);
			green = byte(8, 2) | (low(8, 1) << 8);
			blue = high(8, 1) | (static_cast<uint16_t>(byte(8, 0)) << 4);
			break;
		}
		red = Round12To10(red);
		green = Round12To10(green);
		blue = Round12To10(blue);
		return true;
	}

	bool ReadRgb10(const AnalysisLumaSource& source, int x, int y,
		uint16_t& red, uint16_t& green, uint16_t& blue)
	{
		const uint8_t* row = source.data + static_cast<size_t>(y) * source.rowBytes;
		const uint8_t* pixel = row + static_cast<size_t>(x) * 4;
		switch (source.encoding)
		{
		case VideoFrameEncoding::BGRA_8BIT:
			blue = Expand8To10(pixel[0]);
			green = Expand8To10(pixel[1]);
			red = Expand8To10(pixel[2]);
			return true;
		case VideoFrameEncoding::ARGB_8BIT:
			red = Expand8To10(pixel[1]);
			green = Expand8To10(pixel[2]);
			blue = Expand8To10(pixel[3]);
			return true;
		case VideoFrameEncoding::R210:
		{
			const uint32_t word = ReadBigEndian32(pixel);
			red = static_cast<uint16_t>((word >> 20) & 0x3ff);
			green = static_cast<uint16_t>((word >> 10) & 0x3ff);
			blue = static_cast<uint16_t>(word & 0x3ff);
			return true;
		}
		case VideoFrameEncoding::R10b:
		{
			const uint32_t word = ReadBigEndian32(pixel);
			red = static_cast<uint16_t>((word >> 22) & 0x3ff);
			green = static_cast<uint16_t>((word >> 12) & 0x3ff);
			blue = static_cast<uint16_t>((word >> 2) & 0x3ff);
			return true;
		}
		case VideoFrameEncoding::R10l:
		{
			const uint32_t word = ReadLittleEndian32(pixel);
			red = static_cast<uint16_t>((word >> 22) & 0x3ff);
			green = static_cast<uint16_t>((word >> 12) & 0x3ff);
			blue = static_cast<uint16_t>((word >> 2) & 0x3ff);
			return true;
		}
		case VideoFrameEncoding::R12L:
		{
			const uint8_t* pair = row + static_cast<size_t>(x / 2) * 9;
			if ((x & 1) == 0)
			{
				red = static_cast<uint16_t>(pair[0] | ((pair[1] & 0x0f) << 8));
				green = static_cast<uint16_t>((pair[1] >> 4) | (pair[2] << 4));
				blue = static_cast<uint16_t>(pair[3] | ((pair[4] & 0x0f) << 8));
			}
			else
			{
				red = static_cast<uint16_t>((pair[4] >> 4) | (pair[5] << 4));
				green = static_cast<uint16_t>(pair[6] | ((pair[7] & 0x0f) << 8));
				blue = static_cast<uint16_t>((pair[7] >> 4) | (pair[8] << 4));
			}
			red = Round12To10(red);
			green = Round12To10(green);
			blue = Round12To10(blue);
			return true;
		}
		case VideoFrameEncoding::R12B:
			return ReadR12BPixel(row, x, red, green, blue);
		default:
			return false;
		}
	}
}

bool AnalysisLumaSource::IsValid() const
{
	if (!data || width < 1 || height < 1 || rowBytes == 0)
		return false;
	size_t requiredBytes = 0;
	if (!CheckedMultiply(rowBytes, static_cast<size_t>(height), requiredBytes) ||
		requiredBytes > dataBytes)
		return false;
	if (format == AnalysisLumaFormat::PackedDolbyVision422)
		return DolbyVisionTunnelUnpackReference::SupportsContainer(encoding) && (width & 1) == 0 &&
			(rgbBitAlignment == 1 || rgbBitAlignment == 2) &&
			rowBytes >= static_cast<size_t>(width) * 4;
	if (format == AnalysisLumaFormat::SdrTunnelRgb)
		return encoding == VideoFrameEncoding::R210 &&
			(rgbBitAlignment == 1 || rgbBitAlignment == 2) &&
			rowBytes >= static_cast<size_t>(width) * 4;
	if (format == AnalysisLumaFormat::NativeRgb)
	{
		if (encoding == VideoFrameEncoding::R12B ||
			encoding == VideoFrameEncoding::R12L)
			return (width % 8) == 0 &&
				rowBytes >= static_cast<size_t>(width) * 36 / 8;
		return rowBytes >= static_cast<size_t>(width) * 4;
	}
	if (format == AnalysisLumaFormat::NativeYuv422)
	{
		if ((width & 1) != 0)
			return false;
		if (encoding == VideoFrameEncoding::UYVY ||
			encoding == VideoFrameEncoding::HDYC)
			return rowBytes >= static_cast<size_t>(width) * 2;
		return encoding == VideoFrameEncoding::V210 &&
			rowBytes >= ((static_cast<size_t>(width) + 5) / 6) * 16;
	}
	if ((width & 1) != 0 ||
		(format == AnalysisLumaFormat::P010 && (height & 1) != 0) ||
		rowBytes < static_cast<size_t>(width) * 2 ||
		chromaRowBytes < static_cast<size_t>(width) * 2)
		return false;
	size_t chromaBytes = 0;
	const size_t chromaHeight = format == AnalysisLumaFormat::P210 ||
		format == AnalysisLumaFormat::DecodedDolbyVision422 ?
		static_cast<size_t>(height) : static_cast<size_t>(height / 2);
	return CheckedMultiply(chromaRowBytes, chromaHeight, chromaBytes) &&
		requiredBytes <= std::numeric_limits<size_t>::max() - chromaBytes &&
		requiredBytes + chromaBytes <= dataBytes;
}

bool AnalysisLumaSource::Sample(int x, int y, AnalysisLumaSample& result) const
{
	if (!IsValid() || x < 0 || y < 0 || x >= width || y >= height)
		return false;
	return SampleUnchecked(x, y, result);
}

bool AnalysisLumaSource::SampleLuma(int x, int y, uint16_t& luma) const
{
	if (x >= 0 && x < width)
		if (const uint16_t* row = SharedLumaRow(y))
		{
			luma = row[x];
			return true;
		}
	AnalysisLumaSample sample;
	if (!Sample(x, y, sample))
		return false;
	luma = sample.luma;
	return true;
}

void AnalysisLumaPlane::Bind(const AnalysisLumaSource& source)
{
	width = source.width;
	height = source.height;
	boundData = source.data;
	boundRowBytes = source.rowBytes;
	boundChromaRowBytes = source.chromaRowBytes;
	boundFormat = static_cast<int>(source.format);
	boundEncoding = static_cast<int>(source.encoding);
	boundColorspace = static_cast<int>(source.colorspace);
	boundRange = (source.fullRangeYuv ? 1 : 0);
	boundAlignment = source.rgbBitAlignment;
}

bool AnalysisLumaPlane::Matches(const AnalysisLumaSource& source) const
{
	return width == source.width && height == source.height && boundData == source.data &&
		boundRowBytes == source.rowBytes && boundChromaRowBytes == source.chromaRowBytes &&
		boundFormat == static_cast<int>(source.format) && boundEncoding == static_cast<int>(source.encoding) &&
		boundColorspace == static_cast<int>(source.colorspace) && boundRange == (source.fullRangeYuv ? 1 : 0) &&
		boundAlignment == source.rgbBitAlignment;
}

const uint16_t* AnalysisLumaSource::SharedLumaRow(int y) const
{
	const AnalysisLumaPlane* plane = sharedLuma.get();
	if (!plane || y < 0 || y >= height || !plane->Ready() || !plane->Matches(*this) ||
		plane->codes.size() != static_cast<size_t>(width) * static_cast<size_t>(height))
		return nullptr;
	return plane->codes.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
}

bool AnalysisLumaSource::SampleLumaRow(int x0, int y, int count, uint16_t* out) const
{
	if (!out || count < 0 || !IsValid() || x0 < 0 || y < 0 || y >= height || count > width - x0)
		return false;
	// Full-range YCbCr codes to the limited domain, as Sample() maps them.
	static const auto fullToLimited = []
	{
		std::array<uint16_t, 1024> table{};
		for (int code = 0; code < 1024; ++code)
			table[code] = FullLumaToLimited(code, 1);
		return table;
	}();
	if (format == AnalysisLumaFormat::P010 || format == AnalysisLumaFormat::P210 ||
		format == AnalysisLumaFormat::DecodedDolbyVision422)
	{
		// The upper ten bits of each little-endian 16-bit lane (P010Code and
		// DecodedDolbyVisionCode alike).
		const uint8_t* row = data + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x0) * 2;
		if (fullRangeYuv)
			for (int i = 0; i < count; ++i)
				out[i] = fullToLimited[P010Code(row + static_cast<size_t>(i) * 2)];
		else
			for (int i = 0; i < count; ++i)
				out[i] = P010Code(row + static_cast<size_t>(i) * 2);
		return true;
	}
	if (format == AnalysisLumaFormat::NativeYuv422 &&
		(encoding == VideoFrameEncoding::UYVY || encoding == VideoFrameEncoding::HDYC))
	{
		const uint8_t* row = data + static_cast<size_t>(y) * rowBytes;
		for (int i = 0; i < count; ++i)
		{
			const int x = x0 + i;
			const uint16_t code = static_cast<uint16_t>(row[static_cast<size_t>(x / 2) * 4 + ((x & 1) ? 3 : 1)]) << 2;
			out[i] = fullRangeYuv ? fullToLimited[code] : code;
		}
		return true;
	}
	if (format == AnalysisLumaFormat::NativeYuv422 && encoding == VideoFrameEncoding::V210)
	{
		// Each 16-byte group carries six lumas (ReadNativeYuv422's layout).
		static constexpr int lumaWord[6] = { 0, 1, 1, 2, 3, 3 };
		static constexpr int lumaShift[6] = { 10, 0, 20, 10, 0, 20 };
		const uint8_t* row = data + static_cast<size_t>(y) * rowBytes;
		for (int i = 0; i < count; ++i)
		{
			const int x = x0 + i, k = x % 6;
			const uint8_t* group = row + static_cast<size_t>(x / 6) * 16;
			const uint16_t code = static_cast<uint16_t>(
				(ReadLittleEndian32(group + lumaWord[k] * 4) >> lumaShift[k]) & 0x3ff);
			out[i] = fullRangeYuv ? fullToLimited[code] : code;
		}
		return true;
	}
	if (format == AnalysisLumaFormat::NativeRgb || format == AnalysisLumaFormat::SdrTunnelRgb)
	{
		// Sample()'s luma arithmetic without the chroma.
		const bool bt2020 = colorspace == ColorSpace::BT_2020;
		const bool bt601 = IsBt601(colorspace);
		const bool tunnel = format == AnalysisLumaFormat::SdrTunnelRgb;
		uint16_t red = 0, green = 0, blue = 0;
		if (!tunnel && (encoding == VideoFrameEncoding::R210 || encoding == VideoFrameEncoding::R10b ||
			encoding == VideoFrameEncoding::R10l))
		{
			const bool r210 = encoding == VideoFrameEncoding::R210;
			const LimitedRgbCoefficients& c = bt2020 ? (r210 ? BT2020_R210 : BT2020_R10) :
				(bt601 ? (r210 ? BT601_R210 : BT601_R10) : (r210 ? BT709_R210 : BT709_R10));
			// ReadRgb10's word layouts, decoded inline: r210 big-endian 2:10:10:10,
			// R10b big-endian and R10l little-endian 10:10:10:2.
			const uint8_t* pixel = data + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x0) * 4;
			const bool bigEndian = encoding != VideoFrameEncoding::R10l;
			const int shift = r210 ? 0 : 2;
			// |sum| <= (222927 + 749942 + 75707) x 1023 < 2^31: exact in 32 bits.
			const int32_t yR = c.yR, yG = c.yG, yB = c.yB;
			int i = 0;
#if defined(__AVX__)
			{
				// Four pixels at a time (SSE4.1, in the AVX baseline), the same
				// integer arithmetic: byte order, shift, mask, Q20 products,
				// round half away from zero, clamp.
				const __m128i order = bigEndian ?
					_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12) :
					_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
				const __m128i wordShift = _mm_cvtsi32_si128(shift);
				const __m128i mask = _mm_set1_epi32(0x3ff), black = _mm_set1_epi32(64);
				const __m128i half = _mm_set1_epi32(1 << 19), top = _mm_set1_epi32(1023);
				const __m128i cr = _mm_set1_epi32(yR), cg = _mm_set1_epi32(yG), cb = _mm_set1_epi32(yB);
				for (; i + 4 <= count; i += 4, pixel += 16)
				{
					const __m128i words = _mm_srl_epi32(_mm_shuffle_epi8(
						_mm_loadu_si128(reinterpret_cast<const __m128i*>(pixel)), order), wordShift);
					const __m128i r = _mm_sub_epi32(_mm_and_si128(_mm_srli_epi32(words, 20), mask), black);
					const __m128i g = _mm_sub_epi32(_mm_and_si128(_mm_srli_epi32(words, 10), mask), black);
					const __m128i b = _mm_sub_epi32(_mm_and_si128(words, mask), black);
					const __m128i sum = _mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(cr, r),
						_mm_mullo_epi32(cg, g)), _mm_mullo_epi32(cb, b));
					const __m128i magnitude = _mm_srli_epi32(_mm_add_epi32(_mm_abs_epi32(sum), half), 20);
					const __m128i luma = _mm_min_epi32(_mm_max_epi32(
						_mm_add_epi32(black, _mm_sign_epi32(magnitude, sum)), _mm_setzero_si128()), top);
					_mm_storel_epi64(reinterpret_cast<__m128i*>(out + i), _mm_packus_epi32(luma, luma));
				}
			}
#endif
			for (; i < count; ++i, pixel += 4)
			{
				const uint32_t word = (bigEndian ? ReadBigEndian32(pixel) : ReadLittleEndian32(pixel)) >> shift;
				const int32_t sum = yR * (static_cast<int32_t>((word >> 20) & 0x3ff) - 64) +
					yG * (static_cast<int32_t>((word >> 10) & 0x3ff) - 64) +
					yB * (static_cast<int32_t>(word & 0x3ff) - 64);
				out[i] = Clamp10(64 + RoundQ20(sum));
			}
			return true;
		}
		const FullRgbCoefficients& f = bt2020 ? BT2020_FULL : (bt601 ? BT601_FULL : BT709_FULL);
		const uint8_t* row = data + static_cast<size_t>(y) * rowBytes;
		const bool high = rgbBitAlignment == 1;
		auto channel = [high](uint32_t code)
		{
			return Expand8To10(static_cast<uint8_t>(high ? (code >> 2) : (code & 0xff)));
		};
		if (!tunnel && (encoding == VideoFrameEncoding::BGRA_8BIT || encoding == VideoFrameEncoding::ARGB_8BIT))
		{
			// 8-bit RGB: byte order as ReadRgb10.
			const bool bgra = encoding == VideoFrameEncoding::BGRA_8BIT;
			const uint8_t* pixel = row + static_cast<size_t>(x0) * 4;
			for (int i = 0; i < count; ++i, pixel += 4)
			{
				red = Expand8To10(pixel[bgra ? 2 : 1]);
				green = Expand8To10(pixel[bgra ? 1 : 2]);
				blue = Expand8To10(pixel[bgra ? 0 : 3]);
				out[i] = FullLumaToLimited(f.yR * static_cast<int64_t>(red) + f.yG * static_cast<int64_t>(green) +
					f.yB * static_cast<int64_t>(blue), 65536);
			}
			return true;
		}
		for (int i = 0; i < count; ++i)
		{
			if (tunnel)
			{
				const uint32_t word = ReadBigEndian32(row + static_cast<size_t>(x0 + i) * 4);
				red = channel((word >> 20) & 0x3ff);
				green = channel((word >> 10) & 0x3ff);
				blue = channel(word & 0x3ff);
			}
			else if (!ReadRgb10(*this, x0 + i, y, red, green, blue))
				return false;
			out[i] = FullLumaToLimited(f.yR * static_cast<int64_t>(red) + f.yG * static_cast<int64_t>(green) +
				f.yB * static_cast<int64_t>(blue), 65536);
		}
		return true;
	}
	AnalysisLumaSample sample;
	for (int i = 0; i < count; ++i)
	{
		if (!SampleUnchecked(x0 + i, y, sample))
			return false;
		out[i] = sample.luma;
	}
	return true;
}

const uint16_t* AnalysisLumaSource::LumaPlaneRow(int y) const
{
	if ((format != AnalysisLumaFormat::P010 && format != AnalysisLumaFormat::P210 &&
		format != AnalysisLumaFormat::DecodedDolbyVision422) || y < 0 || y >= height || !IsValid())
		return nullptr;
	// Little-endian lanes read in place (x86/x64).
	const uint8_t* row = data + static_cast<size_t>(y) * rowBytes;
	if ((reinterpret_cast<uintptr_t>(row) & 1) != 0)
		return nullptr;
	return reinterpret_cast<const uint16_t*>(row);
}

uint16_t AnalysisLumaSource::PlaneLuma(uint16_t code) const
{
	code &= 0x3ff;
	return fullRangeYuv ? FullLumaToLimited(code, 1) : code;
}

bool AnalysisLumaSource::SampleUnchecked(int x, int y, AnalysisLumaSample& result) const
{
	if (format == AnalysisLumaFormat::PackedDolbyVision422)
	{
		// The codes DecodedDolbyVision422 reads from the CPU's planes: the
		// pixel's 12-bit intensity and its pair's (C0, C1), upper ten bits.
		const uint8_t* row = data + static_cast<size_t>(y) * rowBytes;
		const uint32_t shift = DolbyVisionTunnelUnpackReference::ContainerShift(encoding, rgbBitAlignment == 1);
		const bool bigEndian = DolbyVisionTunnelUnpackReference::ContainerBigEndian(encoding);
		auto word = [row, bigEndian](int at)
		{
			const uint8_t* pixel = row + static_cast<size_t>(at) * 4;
			return bigEndian ? ReadBigEndian32(pixel) : ReadLittleEndian32(pixel);
		};
		const int pair = x & ~1;
		result.luma = static_cast<uint16_t>(DolbyVisionTunnelUnpackReference::Intensity(word(x), shift) >> 6);
		result.chromaU = static_cast<uint16_t>(DolbyVisionTunnelUnpackReference::Chroma(word(pair), shift) >> 6);
		result.chromaV = static_cast<uint16_t>(DolbyVisionTunnelUnpackReference::Chroma(word(pair + 1), shift) >> 6);
		if (fullRangeYuv)
		{
			// As the decoded planes: full-range DV codes to the limited domain.
			result.luma = FullLumaToLimited(result.luma, 1);
			result.chromaU = FullChromaToLimited(static_cast<int>(result.chromaU) - 512, 1);
			result.chromaV = FullChromaToLimited(static_cast<int>(result.chromaV) - 512, 1);
		}
		return true;
	}
	if (format != AnalysisLumaFormat::NativeRgb &&
		format != AnalysisLumaFormat::SdrTunnelRgb)
	{
		// audit 2026-10-16: full-range YCbCr (e.g. decoded DV with the DM
		// full-range flag) is mapped from its upper-ten-bit codes to the
		// limited domain; limited sources are returned unchanged.
		auto toLimited = [this](AnalysisLumaSample& sample)
		{
			if (!fullRangeYuv)
				return;
			sample.luma = FullLumaToLimited(sample.luma, 1);
			sample.chromaU = FullChromaToLimited(
				static_cast<int>(sample.chromaU) - 512, 1);
			sample.chromaV = FullChromaToLimited(
				static_cast<int>(sample.chromaV) - 512, 1);
		};
		if (format == AnalysisLumaFormat::NativeYuv422)
		{
			if (!ReadNativeYuv422(*this, x, y, result))
				return false;
			toLimited(result);
			return true;
		}
		const uint8_t* luma = data + static_cast<size_t>(y) * rowBytes +
			static_cast<size_t>(x) * 2;
		const size_t lumaBytes = rowBytes * static_cast<size_t>(height);
		const size_t chromaLine = format == AnalysisLumaFormat::P210 ||
			format == AnalysisLumaFormat::DecodedDolbyVision422 ?
			static_cast<size_t>(y) : static_cast<size_t>(y / 2);
		const uint8_t* chroma = data + lumaBytes + chromaLine * chromaRowBytes +
			static_cast<size_t>(x / 2) * 4;
		const auto readCode = format == AnalysisLumaFormat::DecodedDolbyVision422 ?
			DecodedDolbyVisionCode : P010Code;
		result.luma = readCode(luma);
		result.chromaU = readCode(chroma);
		result.chromaV = readCode(chroma + 2);
		toLimited(result);
		return true;
	}
	uint16_t red = 0;
	uint16_t green = 0;
	uint16_t blue = 0;
	if (format == AnalysisLumaFormat::SdrTunnelRgb)
	{
		const uint8_t* pixel = data + static_cast<size_t>(y) * rowBytes +
			static_cast<size_t>(x) * 4;
		const uint32_t word = ReadBigEndian32(pixel);
		auto channel = [this](uint16_t code)
		{
			const uint8_t byte = static_cast<uint8_t>(rgbBitAlignment == 1 ?
				(code >> 2) : (code & 0xff));
			return Expand8To10(byte);
		};
		red = channel(static_cast<uint16_t>((word >> 20) & 0x3ff));
		green = channel(static_cast<uint16_t>((word >> 10) & 0x3ff));
		blue = channel(static_cast<uint16_t>(word & 0x3ff));
	}
	else if (!ReadRgb10(*this, x, y, red, green, blue))
		return false;
	const bool bt2020 = colorspace == ColorSpace::BT_2020;
	const bool bt601 = IsBt601(colorspace); // audit 2026-10-16
	const bool limitedRgb = format != AnalysisLumaFormat::SdrTunnelRgb &&
		(encoding == VideoFrameEncoding::R210 ||
		encoding == VideoFrameEncoding::R10b ||
		encoding == VideoFrameEncoding::R10l);
	if (limitedRgb)
	{
		const bool r210 = encoding == VideoFrameEncoding::R210;
		const LimitedRgbCoefficients& coefficients = bt2020 ?
			(r210 ? BT2020_R210 : BT2020_R10) :
			(bt601 ? (r210 ? BT601_R210 : BT601_R10) :
			(r210 ? BT709_R210 : BT709_R10));
		const int r = static_cast<int>(red) - 64;
		const int g = static_cast<int>(green) - 64;
		const int b = static_cast<int>(blue) - 64;
		result.luma = Clamp10(64 + RoundQ20(
			static_cast<int64_t>(coefficients.yR) * r +
			static_cast<int64_t>(coefficients.yG) * g +
			static_cast<int64_t>(coefficients.yB) * b));
		result.chromaU = Clamp10(512 + RoundQ20(
			static_cast<int64_t>(coefficients.cbR) * r +
			static_cast<int64_t>(coefficients.cbG) * g +
			static_cast<int64_t>(coefficients.cbB) * b));
		result.chromaV = Clamp10(512 + RoundQ20(
			static_cast<int64_t>(coefficients.crR) * r +
			static_cast<int64_t>(coefficients.crG) * g +
			static_cast<int64_t>(coefficients.crB) * b));
		return true;
	}
	// audit 2026-10-16: full-range RGB (8-bit, R12B/R12L, SDR tunnel). The
	// Q16 full-range Y'CbCr sums are mapped straight to the limited code
	// domain with a single rounding (previously emitted as 0..1023 / +-511,
	// which every darkness threshold misread as studio codes).
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

const char* AnalysisLumaFormatName(const AnalysisLumaSource& source)
{
	if (source.format == AnalysisLumaFormat::DecodedDolbyVision422)
		return "decoded-dv-12bit-422";
	if (source.format == AnalysisLumaFormat::SdrTunnelRgb)
		return "sdr-tunnel-rgb8";
	if (source.format == AnalysisLumaFormat::PackedDolbyVision422)
		return "packed-dv-12bit-422";
	if (source.format == AnalysisLumaFormat::NativeRgb)
		return "native-rgb-sparse";
	if (source.format == AnalysisLumaFormat::NativeYuv422)
		return "native-yuv422-sparse";
	return source.format == AnalysisLumaFormat::P210 ? "p210-plane" : "p010-plane";
}
