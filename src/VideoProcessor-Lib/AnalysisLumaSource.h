#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

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
// interpretation to these codes (see led/LedLookahead LumaIsFullRange).
constexpr bool kAnalysisLumaSampleLimitedDomain = true;

struct AnalysisLumaSample
{
	uint16_t luma = 0;
	uint16_t chromaU = 512;
	uint16_t chromaV = 512;
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

	bool IsValid() const;
	bool Sample(int x, int y, AnalysisLumaSample& result) const;
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
