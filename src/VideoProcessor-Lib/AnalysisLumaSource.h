#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <ColorSpace.h>
#include <VideoFrameEncoding.h>

// A renderer-independent, source-coordinate analysis contract. Legacy raw
// views can borrow pixels or retain an immutable per-frame backing owner.
// The sampler retains beta 10-bit rounding and chroma footprints.
enum class AnalysisLumaFormat
{
	P010,
	P210,
	NativeRgb,
	NativeYuv422,
	DecodedDolbyVision422,
	SdrTunnelRgb,
	// dv_tunnel_gpu_unpack: the decoded DV planes live on the GPU; the same
	// 12-bit I / alternating C codes are unpacked here from the raw r210 words
	// at each sampled pixel (rgbBitAlignment: 1 high, 2 low eight bits).
	PackedDolbyVision422,
};

// audit 2026-10-16: every format's Sample() emits the limited (studio) 10-bit
// code domain: luma black 64 / white 940, chroma 512 +/- 448. Full-range
// sources (8-bit and 12-bit RGB, the DV SDR-tunnel RGB, and full-range YCbCr
// planes flagged by fullRangeYuv) are mapped Y = 64 + 876/1023 * Y',
// C = 512 + 896/1023 * C'. Consumers must not apply a second full-range
// interpretation to these codes (see VP-V/modules/drive/led_measure/LedLookahead LumaIsFullRange).
constexpr bool kAnalysisLumaSampleLimitedDomain = true;

struct AnalysisLumaSample
{
	uint16_t luma = 0;
	uint16_t chromaU = 512;
	uint16_t chromaV = 512;
};

// Optional shared per-frame luma product (WP-23, offered upstream): the
// Sample(x, y).luma of every pixel of ONE frame, decoded once by whichever
// producer owns the frame and then read by every analyser. Readers never wait:
// until Publish() the plane is ignored and Sample() decodes as before, so every
// value, and every decision built on it, is identical with or without it.
struct AnalysisLumaPlane
{
	int width = 0;
	int height = 0;
	std::vector<uint16_t> codes; // row-major, `width` per row: Sample(x, y).luma

	// One producer fills `codes` after a successful TryClaim(), then calls
	// Publish(); nothing writes the plane after that.
	bool TryClaim()
	{
		bool expected = false;
		return claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
	}
	void Publish() { ready.store(true, std::memory_order_release); }
	bool Ready() const { return ready.load(std::memory_order_acquire); }

private:
	std::atomic<bool> claimed{ false };
	std::atomic<bool> ready{ false };
};

struct AnalysisLumaSource
{
	const uint8_t* data = nullptr;
	size_t dataBytes = 0;
	int width = 0;
	int height = 0;
	size_t rowBytes = 0;
	size_t chromaRowBytes = 0;
	AnalysisLumaFormat format = AnalysisLumaFormat::P010;
	VideoFrameEncoding encoding = VideoFrameEncoding::UNKNOWN;
	ColorSpace colorspace = ColorSpace::UNKNOWN;
	uint64_t generation = 0;
	uint8_t rgbBitAlignment = 0; // SDR / packed DV tunnel: 1 high, 2 low eight bits
	std::shared_ptr<const void> backingOwner;
	// audit 2026-10-16: YCbCr plane formats (P010, P210, decoded DV 4:2:2,
	// native 4:2:2) carry full-range codes; Sample() maps them to the limited
	// domain. RGB formats derive their range from the encoding instead.
	bool fullRangeYuv = false;
	// WP-23: the optional shared luma plane (see AnalysisLumaPlane); copies of
	// the source share it.
	std::shared_ptr<AnalysisLumaPlane> sharedLuma;

	bool IsValid() const;
	bool Sample(int x, int y, AnalysisLumaSample& result) const;
	// WP-23: Sample(x, y).luma, read from the published shared plane when
	// there is one (same value, same failures).
	bool SampleLuma(int x, int y, uint16_t& luma) const;
	// WP-23: row y of the published shared plane (Sample().luma per pixel), or
	// null (no plane, not published yet, another size, outside the picture).
	const uint16_t* SharedLumaRow(int y) const;
	// Hunt 2 (look-ahead full-coverage peak): Sample(x, y).luma for `count`
	// pixels of row y from x0, decoded a row at a time (plane formats read
	// the row directly; others decode each pixel without re-validating the
	// frame). False when the span is outside the picture or the format
	// cannot be read; `out` is then unspecified.
	bool SampleLumaRow(int x0, int y, int count, uint16_t* out) const;
	// Row y of the luma plane for the 16-bit plane formats (P010, P210,
	// decoded DV 4:2:2: each lane's upper ten bits are the code), or null
	// (other formats, an unaligned row, outside the picture). PlaneLuma maps
	// such a code to Sample()'s luma (the full-range remap when flagged).
	const uint16_t* LumaPlaneRow(int y) const;
	uint16_t PlaneLuma(uint16_t code) const;

private:
	bool SampleUnchecked(int x, int y, AnalysisLumaSample& result) const;
};

const char* AnalysisLumaFormatName(const AnalysisLumaSource& source);
