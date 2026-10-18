#pragma once

// VP Renderer DirectShow filter: media-type acceptance and parsing.
//
// Pure helpers (Windows SDK types only, no DirectShow base classes, no MFC) so
// the CPU tests in VideoProcessor-Test compile them directly.
//
// Colorimetry is decoded to VP's EOTF/ColorSpace by filesource/
// FileMediaTypeAdapter.h (FilterTimingGlue.cpp DecideColor); rates come from
// filesource/RationalRate.h.

#include <Windows.h>
#include <strmif.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <VideoFrameEncoding.h>
#include <filesource/RationalRate.h>


namespace VPRendererFilter
{
	// GUIDs defined locally: P010/P016/v210 are not in every SDK's strmiids.lib.
	extern const GUID kMediaTypeVideo;
	extern const GUID kFormatVideoInfo;
	extern const GUID kFormatVideoInfo2;
	extern const GUID kSubtypeNV12;
	extern const GUID kSubtypeP010;
	extern const GUID kSubtypeP016;
	extern const GUID kSubtypeV210;
	extern const GUID kSubtypeRGB32;

	struct SubtypeInfo
	{
		GUID subtype;
		VideoFrameEncoding encoding;
		const char* name;
	};

	// The subtypes the input pin accepts, in the order GetMediaType proposes
	// them: P010, P016, NV12, v210, RGB32. YUY2 and RGB48 are deliberately
	// absent: ABI 37 has no VideoFrameEncoding for them (add them here once an
	// encoding exists; UYVY exists but is not part of the filter contract).
	const std::vector<SubtypeInfo>& AcceptedSubtypes();
	bool LookupSubtype(const GUID& subtype, SubtypeInfo& out);

	// DXVA_ExtendedFormat fields carried in VIDEOINFOHEADER2::dwControlFlags
	// when AMCONTROL_COLORINFO_PRESENT is set. Values are the DXVA2/MF enums
	// (MFVideoTransferFunction etc., including the 2084/HLG extensions).
	struct Colorimetry
	{
		bool present = false;
		uint32_t chromaSubsampling = 0;
		uint32_t nominalRange = 0;
		uint32_t transferMatrix = 0;
		uint32_t lighting = 0;
		uint32_t primaries = 0;
		uint32_t transferFunction = 0;
	};
	Colorimetry DecodeColorimetry(DWORD controlFlags);

	struct VideoFormat
	{
		GUID subtype = GUID_NULL;
		VideoFrameEncoding encoding = VideoFrameEncoding::UNKNOWN;

		// Visible picture (rcSource when set, else the coded size).
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t originX = 0;
		uint32_t originY = 0;

		// Coded buffer: biWidth (pixels per row, including decoder padding) and
		// |biHeight|; sourceStrideBytes is the plane-0 row pitch in a sample.
		uint32_t codedWidth = 0;
		uint32_t codedHeight = 0;
		uint32_t sourceStrideBytes = 0;
		// Bottom-up DIB (RGB with positive biHeight). YUV is always top-down.
		bool bottomUp = false;

		// Picture aspect ratio (VIDEOINFOHEADER2), else the visible size.
		uint32_t aspectX = 0;
		uint32_t aspectY = 0;

		int64_t avgTimePerFrame = 0;   // 100 ns units; 0 = unknown
		uint32_t bitRate = 0;
		uint32_t bitErrorRate = 0;
		bool interlaced = false;
		bool topFieldFirst = true;
		bool hasVideoInfo2 = false;
		Colorimetry colorimetry;
	};

	// Parses MEDIATYPE_Video + (FORMAT_VideoInfo | FORMAT_VideoInfo2) with an
	// accepted subtype. False with a reason for anything else.
	bool ParseVideoMediaType(const AM_MEDIA_TYPE& mediaType, VideoFormat& out,
		std::string& reason);

	// Plane-0 row pitch of a DirectShow sample for a coded width.
	uint32_t SourceStrideBytes(VideoFrameEncoding encoding, uint32_t codedWidth);
	// Bytes of a sample holding codedHeight rows at strideBytes (both planes).
	size_t SourceFrameBytes(VideoFrameEncoding encoding, uint32_t strideBytes,
		uint32_t codedHeight);

	// The tightly packed layout of VideoFrameEncoding.h; equal to
	// VideoState::BytesPerRow() / BytesPerFrame() for the same width/height.
	uint32_t TightRowBytes(VideoFrameEncoding encoding, uint32_t width);
	size_t TightFrameBytes(VideoFrameEncoding encoding, uint32_t width,
		uint32_t height);

	// Bytes per luma/RGB sample position (NV12 1, P010/P016 2, RGB32 4);
	// 0 for packed formats without a per-pixel byte size (v210).
	uint32_t BytesPerPixelPosition(VideoFrameEncoding encoding);

	// The exact source rate (filesource/RationalRate.h: AvgTimePerFrame snapped
	// to a rational and checked against the sample timestamps).
	using RationalRate = filesource::RationalRate;
}
