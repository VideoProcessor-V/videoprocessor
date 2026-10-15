#pragma once

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <VideoFrameEncoding.h>

#include <string>

// Roadmap item 2 (owner 2026-10-15): native ingest of DeckLink's packed RGB
// formats that VP otherwise converts on the CPU to 10-bit 4:2:0 P010
// (CDeckLinkRGBToP010VideoFrameFormatter):
//   R12B, R12L  12-bit RGB 4:4:4, full range (SMPTE 268M C4: two pixels in
//               nine bytes; R12B byte-swaps every 32-bit word)
//   R210        10-bit RGB 4:4:4 in a big-endian word, levels 64-960
//   R10b, R10l  10-bit RGB 4:4:4, [R10 G10 B10 00] big / little endian,
//               levels 64-940 (no D3D11 texture format matches their bit
//               order: libplacebo's native upload cannot take them)
//   ARGB, BGRA  8-bit RGB 4:4:4, full range ([A R G B] / [B G R A] bytes):
//               the same picture as libplacebo's native upload, through
//               this path so the de-judder gets its frames and luma
// The raw bytes are uploaded as they are and one compute pass writes:
//   rgb   rgba16 unorm, the picture at full precision for the main render:
//         R12 as the 12-bit value << 4 (full range); R210 mapped from its
//         64-960 span to standard 16-bit limited levels (4096 + (v - 64) x
//         438/7, rounded; below-black and above-white codes kept); R10b/R10l
//         as the 10-bit value << 6 (limited)
//   luma  r16 unorm, the de-judder matcher's luma: exactly the Y plane the CPU
//         formatter writes (its integer coefficients and rounding; 10-bit
//         code << 6; R12 full range, R210 limited), so the matcher sees the
//         same values as on the converted path.
class GpuRgbUnpack
{
public:
	GpuRgbUnpack(pl_log log, pl_gpu gpu);
	~GpuRgbUnpack();
	GpuRgbUnpack(const GpuRgbUnpack&) = delete;
	GpuRgbUnpack& operator=(const GpuRgbUnpack&) = delete;

	static bool Supports(VideoFrameEncoding encoding);
	// The rgb texture's encoding for libplacebo: full range 12-bit in the high
	// bits (R12), or 16-bit limited (R210).
	static bool Is8Bit(VideoFrameEncoding encoding)
	{
		return encoding == VideoFrameEncoding::ARGB_8BIT || encoding == VideoFrameEncoding::BGRA_8BIT;
	}
	static bool FullRange(VideoFrameEncoding encoding)
	{
		return encoding == VideoFrameEncoding::R12B || encoding == VideoFrameEncoding::R12L || Is8Bit(encoding);
	}
	// The rgb texture's libplacebo bit encoding: color depth and shift.
	static int ColorDepth(VideoFrameEncoding encoding)
	{
		return Is8Bit(encoding) ? 8 : FullRange(encoding) ? 12 : encoding == VideoFrameEncoding::R210 ? 16 : 10;
	}
	static int BitShift(VideoFrameEncoding encoding)
	{
		return Is8Bit(encoding) ? 8 : FullRange(encoding) ? 4 : encoding == VideoFrameEncoding::R210 ? 0 : 6;
	}

	// data: the raw frame, rowBytes per row, width x height pixels (even
	// width; R12 widths a multiple of 8). bt2020 picks the luma matrix.
	// rgb and luma are created or recreated here.
	// lumaOnly: the matcher's luma alone (rgb untouched; the de-judder's next
	// and ahead frames, which need no picture).
	bool Unpack(VideoFrameEncoding encoding, bool bt2020, const void* data, size_t rowBytes, int width, int height,
		pl_tex& rgb, pl_tex& luma, bool lumaOnly = false);
	const std::string& Error() const { return error; }
	void SetHostReadableForTest(bool on) { hostReadable = on; }
	void SetTimerForTest(pl_timer t) { timer = t; }

private:
	bool hostReadable = false;
	pl_timer timer = nullptr;
	pl_gpu gpu = nullptr;
	pl_dispatch dispatch = nullptr;
	pl_tex raw = nullptr;
	std::string error;
};
