#pragma once

#include <VideoFrameEncoding.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

// A C++ transcription of GpuDolbyVisionTunnelUnpack's compute shader, step by
// step (owner 2026-10-16: the GPU tunnel unpack must be bit-exact with the CPU
// formatter, CR210DolbyVisionTunnelFormatter). The tests run it against the
// CPU unpack on synthetic tunnel frames; keep it in lockstep with DV_BODY.
//
// Containers: r210 ([00 R10 G10 B10], big-endian), R10b / R10l ([R10 G10 B10
// 00], big / little endian). An R10 word is the r210 word << 2 (its two
// padding bits ignored), so it takes the r210 maths with the shift + 2.
//
// The shader, one invocation per pixel pair of a row:
//   1. fetch the pair's two rgba8 texels (one 32-bit word each) and recover
//      the bytes as round(unorm * 255)
//   2. rebuild each word: big endian p = b0 << 24 | b1 << 16 | b2 << 8 | b3,
//      little endian (R10l) p = b3 << 24 | b2 << 16 | b1 << 8 | b0
//   3. s = (2 for RGB8 in the high eight bits of the 10-bit channels, else 0)
//      + (2 for R10b / R10l):
//        I = ((p >> (2 + s)) & 0xFF00) | ((p << (4 - s)) & 0xF0)
//        C = ((p >> (12 + s)) & 0xFF00) | ((p >> s) & 0xF0)
//      (HDMI 1.4a 6.5.4: G8 = I[11:4], R8 = C[11:4], B8 = C[3:0] I[3:0]; the
//      12-bit codes land in the high bits of 16-bit samples, as the CPU's)
//   4. store I0, I1 to the r16 intensity plane, (C0, C1) as one rg16 texel of
//      the chroma plane, each as float(v) / 65535 into unorm16
namespace DolbyVisionTunnelUnpackReference
{
	inline bool SupportsContainer(VideoFrameEncoding encoding)
	{
		return encoding == VideoFrameEncoding::R210 || encoding == VideoFrameEncoding::R10b ||
			encoding == VideoFrameEncoding::R10l;
	}

	inline bool ContainerBigEndian(VideoFrameEncoding encoding)
	{
		return encoding != VideoFrameEncoding::R10l;
	}

	// Step 3's s for a container and RGB8 placement.
	inline uint32_t ContainerShift(VideoFrameEncoding encoding, bool highAligned)
	{
		return (highAligned ? 2u : 0u) + (encoding == VideoFrameEncoding::R210 ? 0u : 2u);
	}

	// Step 1: unorm8 sampled as float, back to the byte (round half even, as
	// the GPU's round() may; never a tie here).
	inline uint32_t FetchByte(uint8_t byte)
	{
		const float sampled = static_cast<float>(byte) / 255.0f;
		return static_cast<uint32_t>(std::nearbyint(sampled * 255.0f));
	}

	// Step 4: float(v) / 65535 written to a unorm16 target (D3D: clamp, then
	// round to nearest even).
	inline uint16_t StoreUnorm16(uint32_t value)
	{
		float f = static_cast<float>(value) / 65535.0f;
		f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
		return static_cast<uint16_t>(std::nearbyint(f * 65535.0f));
	}

	// Step 2.
	inline uint32_t Word(const uint8_t* bytes, bool bigEndian)
	{
		const uint32_t b0 = FetchByte(bytes[0]), b1 = FetchByte(bytes[1]),
			b2 = FetchByte(bytes[2]), b3 = FetchByte(bytes[3]);
		return bigEndian ? (b0 << 24) | (b1 << 16) | (b2 << 8) | b3 :
			(b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
	}

	// Step 3.
	inline uint32_t Intensity(uint32_t p, uint32_t s)
	{
		return ((p >> (2u + s)) & 0xFF00u) | ((p << (4u - s)) & 0xF0u);
	}

	inline uint32_t Chroma(uint32_t p, uint32_t s)
	{
		return ((p >> (12u + s)) & 0xFF00u) | ((p >> s) & 0xF0u);
	}

	// One invocation: pair `pair` of row `row` (the raw frame rowBytes per row).
	// intensity: the row's two samples; chroma: the pair's (C0, C1) texel.
	inline void Invocation(const uint8_t* raw, size_t rowBytes, int pair, int row,
		VideoFrameEncoding encoding, bool highAligned, uint16_t intensity[2], uint16_t chroma[2])
	{
		const uint8_t* texels = raw + static_cast<size_t>(row) * rowBytes + static_cast<size_t>(pair) * 8;
		const uint32_t s = ContainerShift(encoding, highAligned);
		const bool bigEndian = ContainerBigEndian(encoding);
		const uint32_t p0 = Word(texels, bigEndian);
		const uint32_t p1 = Word(texels + 4, bigEndian);
		intensity[0] = StoreUnorm16(Intensity(p0, s));
		intensity[1] = StoreUnorm16(Intensity(p1, s));
		chroma[0] = StoreUnorm16(Chroma(p0, s));
		chroma[1] = StoreUnorm16(Chroma(p1, s));
	}

	// The whole dispatch, written in the CPU formatter's layout (P216: the
	// intensity plane width x height, then the chroma plane with each pair's
	// (C0, C1) side by side, width samples per row). width must be even.
	inline void UnpackFrame(const uint8_t* raw, size_t rowBytes, int width, int height,
		VideoFrameEncoding encoding, bool highAligned, uint16_t* output)
	{
		const size_t planePixels = static_cast<size_t>(width) * static_cast<size_t>(height);
		for (int row = 0; row < height; ++row)
			for (int pair = 0; pair < width / 2; ++pair)
			{
				const size_t at = static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(pair) * 2;
				Invocation(raw, rowBytes, pair, row, encoding, highAligned, output + at, output + planePixels + at);
			}
	}

	// Tests: an r210 frame rewritten in another container (each word << 2,
	// stored big or little endian; the padding bits from `padding`).
	inline void RepackR210(const uint8_t* r210, uint8_t* out, size_t bytes, VideoFrameEncoding encoding,
		uint32_t padding = 0)
	{
		for (size_t i = 0; i + 4 <= bytes; i += 4)
		{
			const uint32_t w = (static_cast<uint32_t>(r210[i]) << 24) | (static_cast<uint32_t>(r210[i + 1]) << 16) |
				(static_cast<uint32_t>(r210[i + 2]) << 8) | r210[i + 3];
			const uint32_t v = encoding == VideoFrameEncoding::R210 ? w : ((w << 2) | ((padding + static_cast<uint32_t>(i)) & 3u));
			for (unsigned k = 0; k < 4; ++k)
				out[i + k] = static_cast<uint8_t>(ContainerBigEndian(encoding) ? v >> (24 - 8 * k) : v >> (8 * k));
		}
	}
}
