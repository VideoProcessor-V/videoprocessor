#pragma once

// Synthetic capture frames in every AnalysisLumaSource format, for the WP-23 tests (producer
// reductions vs a reference, shared-luma equivalence of every consumer). A letterboxed picture:
// black bars (top/bottom), a textured picture with deterministic grain, a BTB patch (code 4), a WTW
// patch (code 1019), a bright highlight and white "subtitle" blocks in the bottom bar. Values are
// written as 10-bit intent codes per format (8-bit formats take the upper eight bits).

#include <AnalysisLumaSource.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace SyntheticAnalysisFrames
{
	enum class Kind
	{
		P010,
		P010Full,
		P210,
		DecodedDv422Full,
		V210,
		V210Full,
		Uyvy,
		R210,
		R10b,
		R10l,
		Bgra,
		Argb,
		R12L,
		R12B,
		SdrTunnel,
		PackedDvR210,
		Count
	};

	inline const wchar_t* KindName(Kind kind)
	{
		static const wchar_t* names[] = { L"P010", L"P010Full", L"P210", L"DecodedDv422Full", L"V210", L"V210Full",
			L"Uyvy", L"R210", L"R10b", L"R10l", L"Bgra", L"Argb", L"R12L", L"R12B", L"SdrTunnel", L"PackedDvR210" };
		return names[static_cast<int>(kind)];
	}

	constexpr int kWidth = 384;    // multiple of 2, 6 and 8 (v210 groups, R12 blocks)
	constexpr int kHeight = 216;
	constexpr int kBar = 27;       // letterbox bar height

	struct Frame
	{
		std::shared_ptr<std::vector<uint8_t>> bytes;
		AnalysisLumaSource source;
	};

	// Deterministic per-pixel noise in [-amp, amp].
	inline int Noise(int x, int y, int frame, int amp)
	{
		uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^
			static_cast<uint32_t>(frame + 1) * 83492791u;
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		return amp ? static_cast<int>(h % static_cast<uint32_t>(2 * amp + 1)) - amp : 0;
	}

	// The scene as 10-bit codes (r, g, b), frame-dependent (motion of the patches, a cut at 6).
	inline void Pixel(int x, int y, int frame, int& r, int& g, int& b)
	{
		auto clamp = [](int v) { return v < 0 ? 0 : (v > 1023 ? 1023 : v); };
		if (y < kBar || y >= kHeight - kBar)
		{
			r = g = b = 64;   // black bars
			// "Subtitle" blocks in the bottom bar.
			if (y >= kHeight - kBar + 6 && y < kHeight - kBar + 14 && x >= 120 && x < 264 && ((x / 6) % 3) != 2)
				r = g = b = 900;
			return;
		}
		const int shift = frame < 6 ? frame * 3 : 40 + frame;   // a cut at frame 6
		int base = frame < 6 ? 180 + ((x + shift) * 600) / kWidth : 760 - ((y + shift) * 500) / kHeight;
		base += Noise(x, y, frame, 12);   // grain
		r = clamp(base + 20);
		g = clamp(base);
		b = clamp(base - 30);
		if (x >= 20 && x < 52 && y >= 40 && y < 64) r = g = b = 4;          // BTB probe
		if (x >= 300 && x < 332 && y >= 40 && y < 64) r = g = b = 1019;     // WTW probe
		if (x >= 180 + shift % 40 && x < 200 + shift % 40 && y >= 100 && y < 120) r = g = b = 980;   // highlight
	}

	inline void Put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
	inline void PutBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
	inline void PutLe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }

	// BT.709 limited-range luma/chroma of 10-bit RGB intent codes (only to make plausible YCbCr).
	inline void Ycc(int r, int g, int b, int& yy, int& u, int& v)
	{
		yy = (2126 * r + 7152 * g + 722 * b) / 10000;
		u = 512 + (b - yy) / 2;
		v = 512 + (r - yy) / 2;
		auto clamp = [](int c) { return c < 0 ? 0 : (c > 1023 ? 1023 : c); };
		yy = clamp(yy); u = clamp(u); v = clamp(v);
	}

	inline Frame Make(Kind kind, int frame = 0)
	{
		Frame out;
		out.bytes = std::make_shared<std::vector<uint8_t>>();
		std::vector<uint8_t>& data = *out.bytes;
		AnalysisLumaSource& s = out.source;
		s.width = kWidth;
		s.height = kHeight;
		s.colorspace = ColorSpace::REC_709;
		s.generation = 1;
		const int w = kWidth, h = kHeight;
		int r, g, b, yy, u, v;
		switch (kind)
		{
		case Kind::P010:
		case Kind::P010Full:
		case Kind::P210:
		case Kind::DecodedDv422Full:
		{
			const bool p010 = kind == Kind::P010 || kind == Kind::P010Full;
			s.format = p010 ? AnalysisLumaFormat::P010 : (kind == Kind::P210 ? AnalysisLumaFormat::P210 :
				AnalysisLumaFormat::DecodedDolbyVision422);
			s.fullRangeYuv = kind == Kind::P010Full || kind == Kind::DecodedDv422Full;
			s.rowBytes = s.chromaRowBytes = static_cast<size_t>(w) * 2;
			const int chromaRows = p010 ? h / 2 : h;
			data.assign(s.rowBytes * h + s.chromaRowBytes * chromaRows, 0);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					Pixel(x, y, frame, r, g, b);
					Ycc(r, g, b, yy, u, v);
					// Low six bits nonzero on the DV planes (12-bit codes): readers take the upper ten.
					const uint16_t low = kind == Kind::DecodedDv422Full ? static_cast<uint16_t>((x * 7 + y) & 0x3f) : 0;
					Put16(&data[static_cast<size_t>(y) * s.rowBytes + static_cast<size_t>(x) * 2], static_cast<uint16_t>((yy << 6) | low));
					if ((x & 1) == 0 && (p010 ? (y & 1) == 0 : true))
					{
						uint8_t* c = &data[s.rowBytes * h + static_cast<size_t>(p010 ? y / 2 : y) * s.chromaRowBytes + static_cast<size_t>(x / 2) * 4];
						Put16(c, static_cast<uint16_t>(u << 6));
						Put16(c + 2, static_cast<uint16_t>(v << 6));
					}
				}
			break;
		}
		case Kind::V210:
		case Kind::V210Full:
		case Kind::Uyvy:
		{
			s.format = AnalysisLumaFormat::NativeYuv422;
			s.fullRangeYuv = kind == Kind::V210Full;
			const bool v210 = kind != Kind::Uyvy;
			s.encoding = v210 ? VideoFrameEncoding::V210 : VideoFrameEncoding::UYVY;
			s.rowBytes = v210 ? static_cast<size_t>((w + 5) / 6) * 16 : static_cast<size_t>(w) * 2;
			data.assign(s.rowBytes * h, 0);
			for (int y = 0; y < h; ++y)
			{
				uint8_t* row = &data[static_cast<size_t>(y) * s.rowBytes];
				for (int x = 0; x < w; x += 2)
				{
					int y0, y1, u0, v0, ud, vd;
					Pixel(x, y, frame, r, g, b); Ycc(r, g, b, y0, u0, v0);
					Pixel(x + 1, y, frame, r, g, b); Ycc(r, g, b, y1, ud, vd);
					if (!v210)
					{
						uint8_t* p = row + static_cast<size_t>(x / 2) * 4;
						p[0] = static_cast<uint8_t>(u0 >> 2); p[1] = static_cast<uint8_t>(y0 >> 2);
						p[2] = static_cast<uint8_t>(v0 >> 2); p[3] = static_cast<uint8_t>(y1 >> 2);
					}
					else
					{
						// v210 group of six pixels: Cb0 Y0 Cr0 | Y1 Cb2 Y2 | Cr2 Y3 Cb4 | Y4 Cr4 Y5.
						uint8_t* group = row + static_cast<size_t>(x / 6) * 16;
						uint32_t words[4];
						std::memcpy(words, group, 16);
						const int k = x % 6;
						auto set = [&](int word, int shift, int value)
						{
							words[word] = (words[word] & ~(0x3ffu << shift)) | (static_cast<uint32_t>(value & 0x3ff) << shift);
						};
						if (k == 0) { set(0, 0, u0); set(0, 10, y0); set(0, 20, v0); set(1, 0, y1); }
						if (k == 2) { set(1, 10, u0); set(1, 20, y0); set(2, 0, v0); set(2, 10, y1); }
						if (k == 4) { set(2, 20, u0); set(3, 0, y0); set(3, 10, v0); set(3, 20, y1); }
						std::memcpy(group, words, 16);
					}
				}
			}
			break;
		}
		case Kind::R210:
		case Kind::R10b:
		case Kind::R10l:
		case Kind::Bgra:
		case Kind::Argb:
		case Kind::SdrTunnel:
		case Kind::PackedDvR210:
		{
			const bool tunnel = kind == Kind::SdrTunnel;
			const bool packed = kind == Kind::PackedDvR210;
			s.format = tunnel ? AnalysisLumaFormat::SdrTunnelRgb : packed ? AnalysisLumaFormat::PackedDolbyVision422 :
				AnalysisLumaFormat::NativeRgb;
			s.encoding = kind == Kind::R10b ? VideoFrameEncoding::R10b : kind == Kind::R10l ? VideoFrameEncoding::R10l :
				kind == Kind::Bgra ? VideoFrameEncoding::BGRA_8BIT : kind == Kind::Argb ? VideoFrameEncoding::ARGB_8BIT :
				VideoFrameEncoding::R210;
			if (tunnel || packed) s.rgbBitAlignment = 1;
			s.rowBytes = static_cast<size_t>(w) * 4;
			data.assign(s.rowBytes * h, 0);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					Pixel(x, y, frame, r, g, b);
					uint8_t* p = &data[static_cast<size_t>(y) * s.rowBytes + static_cast<size_t>(x) * 4];
					switch (kind)
					{
					case Kind::R210: PutBe32(p, (uint32_t(r) << 20) | (uint32_t(g) << 10) | uint32_t(b)); break;
					case Kind::R10b: PutBe32(p, (uint32_t(r) << 22) | (uint32_t(g) << 12) | (uint32_t(b) << 2)); break;
					case Kind::R10l: PutLe32(p, (uint32_t(r) << 22) | (uint32_t(g) << 12) | (uint32_t(b) << 2)); break;
					case Kind::Bgra: p[0] = uint8_t(b >> 2); p[1] = uint8_t(g >> 2); p[2] = uint8_t(r >> 2); p[3] = 255; break;
					case Kind::Argb: p[0] = 255; p[1] = uint8_t(r >> 2); p[2] = uint8_t(g >> 2); p[3] = uint8_t(b >> 2); break;
					default:
						// SDR tunnel: 8-bit RGB in the high eight bits of each r210 channel. Packed DV: the
						// same container carries tunnel words (the sampler unpacks whatever is there).
						PutBe32(p, (uint32_t(r & 0x3fc) << 20) | (uint32_t(g & 0x3fc) << 10) | uint32_t(b & 0x3fc) |
							(packed ? static_cast<uint32_t>(Noise(x, y, frame, 1) + 1) : 0u));
						break;
					}
				}
			break;
		}
		case Kind::R12L:
		case Kind::R12B:
		{
			s.format = AnalysisLumaFormat::NativeRgb;
			s.encoding = kind == Kind::R12L ? VideoFrameEncoding::R12L : VideoFrameEncoding::R12B;
			s.rowBytes = static_cast<size_t>(w) * 36 / 8;
			data.assign(s.rowBytes * h, 0);
			for (int y = 0; y < h; ++y)
			{
				uint8_t* row = &data[static_cast<size_t>(y) * s.rowBytes];
				for (int x = 0; x < w; ++x)
				{
					Pixel(x, y, frame, r, g, b);
					const unsigned R = static_cast<unsigned>(r) << 2 | (x & 3), G = static_cast<unsigned>(g) << 2,
						B = static_cast<unsigned>(b) << 2 | (y & 3);
					if (kind == Kind::R12L)
					{
						uint8_t* pair = row + static_cast<size_t>(x / 2) * 9;
						if ((x & 1) == 0)
						{
							pair[0] = uint8_t(R); pair[1] = uint8_t((pair[1] & 0xf0) | ((R >> 8) & 0x0f));
							pair[1] = uint8_t((pair[1] & 0x0f) | ((G & 0x0f) << 4)); pair[2] = uint8_t(G >> 4);
							pair[3] = uint8_t(B); pair[4] = uint8_t((pair[4] & 0xf0) | ((B >> 8) & 0x0f));
						}
						else
						{
							pair[4] = uint8_t((pair[4] & 0x0f) | ((R & 0x0f) << 4)); pair[5] = uint8_t(R >> 4);
							pair[6] = uint8_t(G); pair[7] = uint8_t((pair[7] & 0xf0) | ((G >> 8) & 0x0f));
							pair[7] = uint8_t((pair[7] & 0x0f) | ((B & 0x0f) << 4)); pair[8] = uint8_t(B >> 4);
						}
					}
					else
					{
						// R12B: arbitrary but deterministic bytes; the sampler and the row decoders read
						// the same layout, which is all the equivalence tests need.
						uint8_t* block = row + static_cast<size_t>(x / 8) * 36;
						const int k = x % 8;
						block[(k * 9 + 0) % 36] ^= uint8_t(R);
						block[(k * 9 + 4) % 36] ^= uint8_t(G >> 2);
						block[(k * 9 + 7) % 36] ^= uint8_t(B >> 3);
					}
				}
			}
			break;
		}
		default:
			break;
		}
		s.data = data.data();
		s.dataBytes = data.size();
		s.backingOwner = out.bytes;
		return out;
	}
}
