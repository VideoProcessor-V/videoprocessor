// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// vp-stamp/1: a machine-readable frame stamp for end-to-end checks (capture -> VP -> output).
// SINGLE SOURCE OF TRUTH for the format; tools/framestamp/framestamp.py mirrors it bit-exactly
// (golden vectors in FrameStampTests.cpp and test_framestamp.py pin both). Pure C++14, header
// only, no Windows headers: the CPU writer/decoder are used by tests, the clip generator and the
// StreamRecorder; FrameStampShader.h draws the OUTPUT stamp on the GPU with the same geometry.
//
// Payload, 64 bits, MSB first: magic 8 (0xA5) | kind 2 (0 source, 1 output) | aux 6 (source: the
// cadence phase; output: low 6 bits of the source sequence) | index 32 | CRC-16/CCITT-FALSE
// (poly 0x1021, init 0xFFFF, no reflection, xorout 0) over the first 48 bits as 6 bytes, MSB first.
//
// Geometry, proportional to the frame height H: unit u = H / 270. The block is 18 x 7 units
// including a 1-unit dark quiet border. Row 0 inside the border is the reference row (16 cells
// alternating 1,0,1,0,...: alignment and the two reference levels); rows 1..4 hold the 64 data bits
// MSB first, 16 per row. Every cell is 1u x 1u. Cell (col, row) covers
//   x in [bx + (1+col)u, bx + (2+col)u), y in [by + (1+row)u, by + (2+row)u)   (row 0 = reference).
// The source stamp is anchored top-left at (2u, 2u); the output stamp top-right at
// (W - 2u - 18u, 2u). A pixel belongs to a cell when its centre (x + 0.5, y + 0.5) lies inside the
// cell rectangle (fractional geometry: a stamp scaled with the picture keeps its geometry in units
// of the new height). The whole 18x7 block is painted: the border at the bit-0 (black) level.
//
// Luma only: bit 1 = white, bit 0 = black at the frame's depth and range (limited 16/235 << (depth-8),
// full 0 / (1<<depth)-1). The writer takes a luma plane only; a producer with chroma planes must set
// the chroma samples under the block to neutral itself (make_stamped_clip.py does; BlockRect gives
// the pixel bounds). The decoder reads luma only.
//
// Decoder: mean of the inner 50% of each cell; threshold = mean of the reference row's 1-cell average
// and 0-cell average; requires contrast (mean1 - mean0) >= 25% of the code range (1 << depth), an
// alternating reference row (each reference cell on its side of the threshold), the magic and a CRC
// match. A search radius (units, default 1.0, step 0.25u, both axes) tolerates small shifts.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace VpFrameStamp
{
	enum class Corner { Source = 0, Output = 1 };

	constexpr int kMagic = 0xA5;
	constexpr int kKindSource = 0;
	constexpr int kKindOutput = 1;
	constexpr int kCols = 16;
	constexpr int kRows = 5;                    // row 0 = reference, rows 1..4 = data
	constexpr double kUnitsAcross = 270.0;      // u = H / 270
	constexpr double kBlockW = 18.0;
	constexpr double kBlockH = 7.0;
	constexpr double kAnchor = 2.0;
	constexpr double kInnerLo = 0.25;
	constexpr double kInnerHi = 0.75;
	constexpr double kMinContrast = 0.25;       // of (1 << depth)
	constexpr double kSearchStep = 0.25;        // units
	constexpr double kDefaultRadius = 1.0;      // units

	inline uint16_t Crc16(const uint8_t* data, size_t size)
	{
		uint16_t crc = 0xFFFF;
		for (size_t i = 0; i < size; ++i)
		{
			crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));
			for (int b = 0; b < 8; ++b)
				crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
		}
		return crc;
	}

	// CRC over the first 48 bits (the top 48 bits of a 64-bit word), MSB first.
	inline uint16_t WordCrc(uint64_t top48Shifted)   // the word with its low 16 bits ignored
	{
		uint8_t bytes[6];
		for (int i = 0; i < 6; ++i)
			bytes[i] = static_cast<uint8_t>(top48Shifted >> (56 - 8 * i));
		return Crc16(bytes, 6);
	}

	inline uint64_t Word(int kind, int aux, uint32_t index)
	{
		const uint64_t head = (static_cast<uint64_t>(kMagic) << 40) | (static_cast<uint64_t>(kind & 3) << 38) |
			(static_cast<uint64_t>(aux & 63) << 32) | static_cast<uint64_t>(index);
		const uint64_t shifted = head << 16;
		return shifted | WordCrc(shifted);
	}

	inline uint32_t WordHi(uint64_t word) { return static_cast<uint32_t>(word >> 32); }
	inline uint32_t WordLo(uint64_t word) { return static_cast<uint32_t>(word & 0xFFFFFFFFull); }

	inline void Levels(int bitDepth, bool fullRange, uint32_t& black, uint32_t& white)
	{
		if (fullRange)
		{
			black = 0;
			white = (1u << bitDepth) - 1u;
		}
		else
		{
			const int shift = bitDepth - 8;
			black = 16u << shift;
			white = 235u << shift;
		}
	}

	inline double UnitOf(int h) { return static_cast<double>(h) / kUnitsAcross; }

	inline void BlockOrigin(int w, int h, Corner corner, double& bx, double& by)
	{
		const double u = UnitOf(h);
		by = kAnchor * u;
		bx = corner == Corner::Output ? static_cast<double>(w) - kAnchor * u - kBlockW * u : kAnchor * u;
	}

	// First integer pixel whose centre (i + 0.5) is >= a (exact after the correction loops).
	inline int PixStart(double a)
	{
		int i = static_cast<int>(std::ceil(a - 0.5));
		while (i + 0.5 < a) ++i;
		while ((i - 1) + 0.5 >= a) --i;
		return i;
	}

	inline int BitAt(uint64_t word, int col, int row)
	{
		if (row == 0)
			return col % 2 == 0 ? 1 : 0;
		const int idx = (row - 1) * kCols + col;
		return static_cast<int>((word >> (63 - idx)) & 1u);
	}

	// Integer pixel bounds [x0,x1) x [y0,y1) of the painted block, clipped to the frame.
	inline void BlockRect(int w, int h, Corner corner, int& x0, int& x1, int& y0, int& y1)
	{
		const double u = UnitOf(h);
		double bx, by;
		BlockOrigin(w, h, corner, bx, by);
		x0 = std::max(PixStart(bx), 0);
		x1 = std::min(PixStart(bx + kBlockW * u), w);
		y0 = std::max(PixStart(by), 0);
		y1 = std::min(PixStart(by + kBlockH * u), h);
	}

	// Paints the stamp into a luma plane (stride in samples). T = uint8_t or uint16_t.
	template <class T>
	void Write(T* plane, int w, int h, int stride, int bitDepth, bool fullRange, Corner corner, uint64_t word)
	{
		uint32_t black, white;
		Levels(bitDepth, fullRange, black, white);
		const double u = UnitOf(h);
		double bx, by;
		BlockOrigin(w, h, corner, bx, by);
		int x0, x1, y0, y1;
		BlockRect(w, h, corner, x0, x1, y0, y1);
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x)
				plane[static_cast<size_t>(y) * stride + x] = static_cast<T>(black);
		for (int row = 0; row < kRows; ++row)
			for (int col = 0; col < kCols; ++col)
			{
				if (!BitAt(word, col, row))
					continue;
				const int cx0 = std::max(PixStart(bx + (1 + col) * u), 0);
				const int cx1 = std::min(PixStart(bx + (2 + col) * u), w);
				const int cy0 = std::max(PixStart(by + (1 + row) * u), 0);
				const int cy1 = std::min(PixStart(by + (2 + row) * u), h);
				for (int y = cy0; y < cy1; ++y)
					for (int x = cx0; x < cx1; ++x)
						plane[static_cast<size_t>(y) * stride + x] = static_cast<T>(white);
			}
	}

	struct Result
	{
		bool found = false;
		int kind = 0;
		int aux = 0;
		uint32_t index = 0;
		double contrast = 0.0;
		double offsetX = 0.0;   // search shift in units (0 when the nominal geometry matched)
		double offsetY = 0.0;
		uint64_t word = 0;
	};

	namespace Detail
	{
		// Mean of the samples whose centres lie in [a0,a1) x [b0,b1); false when outside the frame.
		template <class Sampler>
		bool CellMean(Sampler& sample, int w, int h, double a0, double a1, double b0, double b1, double& mean)
		{
			int ix0 = PixStart(a0), ix1 = PixStart(a1);
			int iy0 = PixStart(b0), iy1 = PixStart(b1);
			if (ix1 <= ix0)
			{
				ix0 = static_cast<int>(std::floor((a0 + a1) * 0.5));
				ix1 = ix0 + 1;
			}
			if (iy1 <= iy0)
			{
				iy0 = static_cast<int>(std::floor((b0 + b1) * 0.5));
				iy1 = iy0 + 1;
			}
			if (ix0 < 0 || iy0 < 0 || ix1 > w || iy1 > h)
				return false;
			uint64_t total = 0;
			for (int y = iy0; y < iy1; ++y)
				for (int x = ix0; x < ix1; ++x)
					total += static_cast<uint64_t>(sample(x, y));
			mean = static_cast<double>(total) / static_cast<double>(static_cast<int64_t>(ix1 - ix0) * (iy1 - iy0));
			return true;
		}

		template <class Sampler>
		bool TryDecode(Sampler& sample, int w, int h, int bitDepth, Corner corner, double ox, double oy, Result& out)
		{
			const double u = UnitOf(h);
			double bx, by;
			BlockOrigin(w, h, corner, bx, by);
			bx += ox;
			by += oy;
			double means[kRows * kCols];
			for (int row = 0; row < kRows; ++row)
				for (int col = 0; col < kCols; ++col)
				{
					const double cx = bx + (1 + col) * u;
					const double cy = by + (1 + row) * u;
					if (!CellMean(sample, w, h, cx + kInnerLo * u, cx + kInnerHi * u, cy + kInnerLo * u, cy + kInnerHi * u,
						means[row * kCols + col]))
						return false;
				}
			double sum1 = 0.0, sum0 = 0.0;
			for (int col = 0; col < kCols; ++col)
			{
				if (col % 2 == 0) sum1 += means[col];
				else sum0 += means[col];
			}
			const double mean1 = sum1 / 8.0;
			const double mean0 = sum0 / 8.0;
			const double contrast = mean1 - mean0;
			if (contrast < kMinContrast * static_cast<double>(1u << bitDepth))
				return false;
			const double threshold = (mean1 + mean0) * 0.5;
			for (int col = 0; col < kCols; ++col)
			{
				if (col % 2 == 0)
				{
					if (means[col] < threshold) return false;
				}
				else if (means[col] >= threshold)
					return false;
			}
			uint64_t value = 0;
			for (int i = 0; i < 64; ++i)
				value = (value << 1) | (means[kCols + i] >= threshold ? 1u : 0u);
			if (WordCrc(value) != static_cast<uint16_t>(value & 0xFFFF))
				return false;
			if ((value >> 56) != static_cast<uint64_t>(kMagic))
				return false;
			out.found = true;
			out.kind = static_cast<int>((value >> 54) & 3);
			out.aux = static_cast<int>((value >> 48) & 63);
			out.index = static_cast<uint32_t>((value >> 16) & 0xFFFFFFFFull);
			out.contrast = contrast;
			out.word = value;
			return true;
		}
	}

	// Decodes through a luma sampler: sample(x, y) -> the luma code value at the frame's bit depth.
	// Lets a packed capture buffer (v210, UYVY, ...) be decoded without unpacking the whole frame.
	template <class Sampler>
	Result DecodeWith(Sampler sample, int w, int h, int bitDepth, Corner corner, double radiusUnits = kDefaultRadius)
	{
		struct Cand { int ix, iy; };
		std::vector<Cand> cands;
		const int n = static_cast<int>(std::floor(radiusUnits / kSearchStep + 0.5));
		for (int ix = -n; ix <= n; ++ix)
			for (int iy = -n; iy <= n; ++iy)
				cands.push_back({ ix, iy });
		std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b)
		{
			const int da = a.ix * a.ix + a.iy * a.iy, db = b.ix * b.ix + b.iy * b.iy;
			if (da != db) return da < db;
			if (a.iy != b.iy) return a.iy < b.iy;
			return a.ix < b.ix;
		});
		const double u = UnitOf(h);
		Result result;
		for (const Cand& c : cands)
			if (Detail::TryDecode(sample, w, h, bitDepth, corner, c.ix * kSearchStep * u, c.iy * kSearchStep * u, result))
			{
				result.offsetX = c.ix * kSearchStep;
				result.offsetY = c.iy * kSearchStep;
				return result;
			}
		return Result();
	}

	template <class T>
	Result Decode(const T* plane, int w, int h, int stride, int bitDepth, Corner corner, double radiusUnits = kDefaultRadius)
	{
		return DecodeWith([plane, stride](int x, int y) { return static_cast<uint32_t>(plane[static_cast<size_t>(y) * stride + x]); },
			w, h, bitDepth, corner, radiusUnits);
	}

	// FNV-1a 64 (the golden-vector hash).
	inline uint64_t Fnv1a64(const uint8_t* data, size_t size)
	{
		uint64_t h = 0xCBF29CE484222325ull;
		for (size_t i = 0; i < size; ++i)
			h = (h ^ data[i]) * 0x100000001B3ull;
		return h;
	}
}
