#pragma once

// VP Renderer (file sources): AM_MEDIA_TYPE (VIDEOINFOHEADER /
// VIDEOINFOHEADER2) -> VP stream description, plus the CPU repack from the
// decoder's stride to VP's tightly packed layout. Pure, header-only,
// graph-free; C++14. Windows SDK DirectShow headers only (no strmbase).
//
// Accepted subtypes: NV12, P010, P016 (two-plane 4:2:0), v210 (-> V210) and
// RGB32 (-> BGRA_8BIT). Everything else is rejected with a reason.
//
// Colorimetry comes from VIDEOINFOHEADER2::dwControlFlags when
// AMCONTROL_COLORINFO_PRESENT is set: the upper 24 bits carry a
// DXVA_ExtendedFormat (decoded with explicit shifts so dxva.h is not needed):
//   bits 12-14 NominalRange, 15-17 VideoTransferMatrix, 22-26 VideoPrimaries,
//   27-31 VideoTransferFunction.
// Values beyond DXVA2 follow the Media Foundation extensions that LAV Video
// writes: transfer 15 = SMPTE ST 2084 (PQ), 16 = HLG; matrix 4/5 = BT.2020;
// primaries 9 = BT.2020, 11 = DCI-P3.
//
// HDR side data (IMediaSideData) is not handled here.

#include <dshow.h>
#include <dvdmedia.h>

#include <EOTF.h>
#include <ColorSpace.h>
#include <DisplayMode.h>
#include <VideoFrameEncoding.h>
#include "RationalRate.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <algorithm>

#ifndef AMCONTROL_USED
#define AMCONTROL_USED 0x00000001
#endif
#ifndef AMCONTROL_COLORINFO_PRESENT
#define AMCONTROL_COLORINFO_PRESENT 0x00000080
#endif

namespace filesource
{
	// Raw DXVA/MF code points (see header comment).
	namespace dxva
	{
		enum NominalRange : uint32_t { RangeUnknown = 0, Range0_255 = 1, Range16_235 = 2, Range48_208 = 3 };
		enum TransferMatrix : uint32_t { MatrixUnknown = 0, MatrixBT709 = 1, MatrixBT601 = 2, MatrixSMPTE240M = 3,
			MatrixBT2020_10 = 4, MatrixBT2020_12 = 5 };
		enum Primaries : uint32_t { PrimariesUnknown = 0, PrimariesReserved = 1, PrimariesBT709 = 2,
			PrimariesBT470_2_SysM = 3, PrimariesBT470_2_SysBG = 4, PrimariesSMPTE170M = 5, PrimariesSMPTE240M = 6,
			PrimariesEBU3213 = 7, PrimariesSMPTE_C = 8, PrimariesBT2020 = 9, PrimariesXYZ = 10, PrimariesDCI_P3 = 11 };
		enum TransferFunction : uint32_t { TransferUnknown = 0, Transfer10 = 1, Transfer18 = 2, Transfer20 = 3,
			Transfer22 = 4, Transfer709 = 5, Transfer240M = 6, TransferSRGB = 7, Transfer28 = 8, TransferLog100 = 9,
			TransferLog316 = 10, Transfer709Sym = 11, Transfer2020Const = 12, Transfer2020 = 13, Transfer26 = 14,
			Transfer2084 = 15, TransferHLG = 16 };

		// Pack fields the way DXVA_ExtendedFormat lays them out (tests, encoders).
		inline uint32_t PackControlFlags(uint32_t nominalRange, uint32_t matrix, uint32_t primaries,
			uint32_t transfer)
		{
			return AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT |
				((nominalRange & 0x7u) << 12) | ((matrix & 0x7u) << 15) |
				((primaries & 0x1Fu) << 22) | ((transfer & 0x1Fu) << 27);
		}
	}

	enum class FileNominalRange { Unknown, Full, Limited, Range48_208 };

	struct FileColorimetry
	{
		bool present = false; // AMCONTROL_COLORINFO_PRESENT was set
		uint32_t nominalRangeCode = 0;
		uint32_t transferMatrixCode = 0;
		uint32_t primariesCode = 0;
		uint32_t transferFunctionCode = 0;

		FileNominalRange nominalRange = FileNominalRange::Unknown;
		EOTF eotf = EOTF::UNKNOWN;
		ColorSpace colorSpace = ColorSpace::UNKNOWN;
		// The colour space was not signalled and was derived from the picture
		// height with the rule the Magewell capture path uses.
		bool colorSpaceFromHeight = false;
	};

	// Colour space from picture height when nothing is signalled. Mirrors the
	// existing Magewell fallback (MagewellCaptureDevice.cpp: PQ/HLG -> BT.2020,
	// >= 720 lines -> BT.709, 576 -> Rec.601 625, else Rec.601 525); the 625
	// digital entry is used for 576 as the DeckLink Rec.601 mapping does.
	inline ColorSpace ColorSpaceFromHeight(uint32_t height, EOTF eotf)
	{
		if (eotf == EOTF::PQ || eotf == EOTF::HLG)
			return ColorSpace::BT_2020;
		if (height >= 720)
			return ColorSpace::REC_709;
		if (height == 576)
			return ColorSpace::REC_601_625;
		return ColorSpace::REC_601_525;
	}

	// Decode dwControlFlags. height is the picture height (Rec.601 split and the
	// unsignalled fallback only).
	inline FileColorimetry DecodeColorimetry(uint32_t controlFlags, uint32_t height)
	{
		FileColorimetry c;
		c.present = (controlFlags & AMCONTROL_COLORINFO_PRESENT) != 0;
		if (c.present)
		{
			c.nominalRangeCode = (controlFlags >> 12) & 0x7u;
			c.transferMatrixCode = (controlFlags >> 15) & 0x7u;
			c.primariesCode = (controlFlags >> 22) & 0x1Fu;
			c.transferFunctionCode = (controlFlags >> 27) & 0x1Fu;
		}

		switch (c.nominalRangeCode)
		{
		case dxva::Range0_255: c.nominalRange = FileNominalRange::Full; break;
		case dxva::Range16_235: c.nominalRange = FileNominalRange::Limited; break;
		case dxva::Range48_208: c.nominalRange = FileNominalRange::Range48_208; break;
		default: c.nominalRange = FileNominalRange::Unknown; break;
		}

		// Transfer -> EOTF. Gamma-type SDR curves -> SDR; linear, log and
		// unknown -> UNKNOWN (conservative).
		switch (c.transferFunctionCode)
		{
		case dxva::Transfer2084: c.eotf = EOTF::PQ; break;
		case dxva::TransferHLG: c.eotf = EOTF::HLG; break;
		case dxva::Transfer18:
		case dxva::Transfer20:
		case dxva::Transfer22:
		case dxva::Transfer709:
		case dxva::Transfer240M:
		case dxva::TransferSRGB:
		case dxva::Transfer28:
		case dxva::Transfer709Sym:
		case dxva::Transfer2020Const:
		case dxva::Transfer2020:
		case dxva::Transfer26:
			c.eotf = EOTF::SDR;
			break;
		default:
			c.eotf = EOTF::UNKNOWN;
			break;
		}

		// Primaries first (VP's ColorSpace describes the gamut), then the matrix.
		switch (c.primariesCode)
		{
		case dxva::PrimariesBT2020: c.colorSpace = ColorSpace::BT_2020; break;
		case dxva::PrimariesBT709: c.colorSpace = ColorSpace::REC_709; break;
		case dxva::PrimariesBT470_2_SysM:
		case dxva::PrimariesSMPTE170M:
		case dxva::PrimariesSMPTE240M:
		case dxva::PrimariesSMPTE_C:
			c.colorSpace = ColorSpace::REC_601_525;
			break;
		case dxva::PrimariesBT470_2_SysBG:
		case dxva::PrimariesEBU3213:
			c.colorSpace = ColorSpace::REC_601_625;
			break;
		case dxva::PrimariesDCI_P3: c.colorSpace = ColorSpace::P3_DCI; break;
		default:
			switch (c.transferMatrixCode)
			{
			case dxva::MatrixBT2020_10:
			case dxva::MatrixBT2020_12:
				c.colorSpace = ColorSpace::BT_2020;
				break;
			case dxva::MatrixBT709:
				c.colorSpace = ColorSpace::REC_709;
				break;
			case dxva::MatrixBT601:
			case dxva::MatrixSMPTE240M:
				c.colorSpace = (height == 576) ? ColorSpace::REC_601_625 : ColorSpace::REC_601_525;
				break;
			default:
				c.colorSpace = ColorSpaceFromHeight(height, c.eotf);
				c.colorSpaceFromHeight = true;
				break;
			}
			break;
		}
		return c;
	}

	struct FileVideoFormat
	{
		bool ok = false;
		const char* rejection = nullptr; // static string when !ok

		VideoFrameEncoding encoding = VideoFrameEncoding::UNKNOWN;
		bool isVideoInfoHeader2 = false;

		// Picture (rcSource, or the full buffer when rcSource is empty).
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t sourceOffsetX = 0;
		uint32_t sourceOffsetY = 0;

		// Buffer geometry from BITMAPINFOHEADER.
		uint32_t bufferWidth = 0;   // biWidth (pixels per stride)
		uint32_t bufferHeight = 0;  // |biHeight|
		uint32_t sourceStrideBytes = 0;
		bool bottomUp = false;      // RGB with biHeight > 0

		uint32_t pictureAspectX = 0;
		uint32_t pictureAspectY = 0;

		bool interlaced = false;
		bool oneFieldPerSample = false;
		DisplayMode::FieldOrder fieldOrder = DisplayMode::FieldOrder::Unknown;

		int64_t avgTimePerFrame = 0;
		RationalRate rate; // snapped AvgTimePerFrame (invalid when 0)

		FileColorimetry color;
	};

	namespace detail
	{
		inline bool IsFourCcGuid(const GUID& g, uint32_t fourcc)
		{
			static const unsigned char tail[8] = { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
			return g.Data1 == fourcc && g.Data2 == 0x0000 && g.Data3 == 0x0010 &&
				std::memcmp(g.Data4, tail, sizeof(tail)) == 0;
		}

		inline bool SameGuid(const GUID& a, const GUID& b)
		{
			return std::memcmp(&a, &b, sizeof(GUID)) == 0;
		}

		inline uint32_t FourCc(char a, char b, char c, char d)
		{
			return (uint32_t)(unsigned char)a | ((uint32_t)(unsigned char)b << 8) |
				((uint32_t)(unsigned char)c << 16) | ((uint32_t)(unsigned char)d << 24);
		}

		// Literal GUIDs so the header needs no strmiids.lib symbols.
		inline const GUID& MediaTypeVideo()
		{
			static const GUID g = { 0x73646976, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
			return g;
		}
		inline const GUID& SubtypeRgb32()
		{
			static const GUID g = { 0xe436eb7e, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
			return g;
		}
		inline const GUID& FormatVideoInfo()
		{
			static const GUID g = { 0x05589f80, 0xc356, 0x11ce, { 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a } };
			return g;
		}
		inline const GUID& FormatVideoInfo2()
		{
			static const GUID g = { 0xf72a76a0, 0xeb0a, 0x11d0, { 0xac, 0xe4, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };
			return g;
		}

		inline uint32_t GcdU32(uint32_t a, uint32_t b)
		{
			while (b != 0) { const uint32_t t = a % b; a = b; b = t; }
			return a;
		}

		inline FileVideoFormat Reject(const char* reason)
		{
			FileVideoFormat f;
			f.ok = false;
			f.rejection = reason;
			return f;
		}
	}

	// Bytes per sample of one plane element: NV12 1, P010/P016 2, BGRA 4.
	inline uint32_t FileBytesPerSample(VideoFrameEncoding e)
	{
		switch (e)
		{
		case VideoFrameEncoding::NV12: return 1;
		case VideoFrameEncoding::P010:
		case VideoFrameEncoding::P016: return 2;
		case VideoFrameEncoding::BGRA_8BIT: return 4;
		default: return 0;
		}
	}

	// Row bytes for a buffer of `pixels` width (decoder stride or VP packed row).
	// Same formulas as VideoState::BytesPerRow for these encodings; 0 = unsupported.
	inline uint32_t FileRowBytes(VideoFrameEncoding e, uint32_t pixels)
	{
		switch (e)
		{
		case VideoFrameEncoding::NV12: return (pixels + 1) & ~1u;
		case VideoFrameEncoding::P010:
		case VideoFrameEncoding::P016: return ((pixels + 1) & ~1u) * 2;
		case VideoFrameEncoding::BGRA_8BIT: return pixels * 4;
		case VideoFrameEncoding::V210: return ((pixels + 47) / 48) * 128;
		default: return 0;
		}
	}

	// VP's packed frame size (two-plane 4:2:0: luma + ceil(h/2) CbCr rows).
	inline size_t FilePackedFrameBytes(VideoFrameEncoding e, uint32_t width, uint32_t height)
	{
		const size_t row = FileRowBytes(e, width);
		if (VideoFrameEncodingIsTwoPlane420(e))
			return row * ((size_t)height + ((size_t)height + 1) / 2);
		return row * (size_t)height;
	}

	// Decoder stride in bytes for biWidth pixels (no even rounding: the stride
	// is what the decoder wrote).
	inline uint32_t FileSourceStrideBytes(VideoFrameEncoding e, uint32_t biWidth)
	{
		if (e == VideoFrameEncoding::V210)
			return FileRowBytes(e, biWidth);
		return biWidth * FileBytesPerSample(e);
	}

	inline FileVideoFormat ParseFileMediaType(const AM_MEDIA_TYPE& mt)
	{
		using namespace detail;
		if (!SameGuid(mt.majortype, MediaTypeVideo()))
			return Reject("major type is not video");

		FileVideoFormat f;
		if (IsFourCcGuid(mt.subtype, FourCc('N', 'V', '1', '2')))
			f.encoding = VideoFrameEncoding::NV12;
		else if (IsFourCcGuid(mt.subtype, FourCc('P', '0', '1', '0')))
			f.encoding = VideoFrameEncoding::P010;
		else if (IsFourCcGuid(mt.subtype, FourCc('P', '0', '1', '6')))
			f.encoding = VideoFrameEncoding::P016;
		else if (IsFourCcGuid(mt.subtype, FourCc('v', '2', '1', '0')))
			f.encoding = VideoFrameEncoding::V210;
		else if (SameGuid(mt.subtype, SubtypeRgb32()))
			f.encoding = VideoFrameEncoding::BGRA_8BIT;
		else
		{
			return Reject("unsupported subtype (accepted: NV12, P010, P016, v210, RGB32)");
		}

		if (mt.pbFormat == nullptr)
			return Reject("media type has no format block");

		const BITMAPINFOHEADER* bih = nullptr;
		RECT rcSource = { 0, 0, 0, 0 };
		if (SameGuid(mt.formattype, FormatVideoInfo2()))
		{
			if (mt.cbFormat < sizeof(VIDEOINFOHEADER2))
				return Reject("VIDEOINFOHEADER2 format block too small");
			const VIDEOINFOHEADER2* vih2 = reinterpret_cast<const VIDEOINFOHEADER2*>(mt.pbFormat);
			f.isVideoInfoHeader2 = true;
			bih = &vih2->bmiHeader;
			rcSource = vih2->rcSource;
			f.avgTimePerFrame = vih2->AvgTimePerFrame;
			f.pictureAspectX = vih2->dwPictAspectRatioX;
			f.pictureAspectY = vih2->dwPictAspectRatioY;
			f.interlaced = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) != 0;
			f.oneFieldPerSample = f.interlaced && (vih2->dwInterlaceFlags & AMINTERLACE_1FieldPerSample) != 0;
			if (f.interlaced)
				f.fieldOrder = (vih2->dwInterlaceFlags & AMINTERLACE_Field1First) != 0
					? DisplayMode::FieldOrder::TopFirst : DisplayMode::FieldOrder::BottomFirst;
			// Colour is decoded below once the picture height is known.
		}
		else if (SameGuid(mt.formattype, FormatVideoInfo()))
		{
			if (mt.cbFormat < sizeof(VIDEOINFOHEADER))
				return Reject("VIDEOINFOHEADER format block too small");
			const VIDEOINFOHEADER* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
			bih = &vih->bmiHeader;
			rcSource = vih->rcSource;
			f.avgTimePerFrame = vih->AvgTimePerFrame;
		}
		else
		{
			return Reject("unsupported format type (accepted: VIDEOINFOHEADER, VIDEOINFOHEADER2)");
		}

		if (bih->biWidth <= 0)
			return Reject("biWidth must be positive");
		if (bih->biHeight == 0 || bih->biHeight == LONG_MIN)
			return Reject("biHeight is zero or out of range");

		f.bufferWidth = (uint32_t)bih->biWidth;
		f.bufferHeight = (uint32_t)std::labs(bih->biHeight);
		// YUV is top-down whatever the sign; RGB with a positive height is bottom-up.
		f.bottomUp = f.encoding == VideoFrameEncoding::BGRA_8BIT && bih->biHeight > 0;
		f.sourceStrideBytes = FileSourceStrideBytes(f.encoding, f.bufferWidth);

		const bool emptySource = rcSource.right <= rcSource.left || rcSource.bottom <= rcSource.top;
		if (emptySource)
		{
			if (rcSource.left != 0 || rcSource.top != 0 || rcSource.right != 0 || rcSource.bottom != 0)
				return Reject("rcSource is inverted");
			f.width = f.bufferWidth;
			f.height = f.bufferHeight;
		}
		else
		{
			if (rcSource.left < 0 || rcSource.top < 0 ||
				(uint32_t)rcSource.right > f.bufferWidth || (uint32_t)rcSource.bottom > f.bufferHeight)
				return Reject("rcSource lies outside the buffer");
			f.sourceOffsetX = (uint32_t)rcSource.left;
			f.sourceOffsetY = (uint32_t)rcSource.top;
			f.width = (uint32_t)(rcSource.right - rcSource.left);
			f.height = (uint32_t)(rcSource.bottom - rcSource.top);
		}
		if (VideoFrameEncodingIsTwoPlane420(f.encoding) &&
			((f.sourceOffsetX & 1u) != 0 || (f.sourceOffsetY & 1u) != 0))
			return Reject("4:2:0 rcSource offset must be even");
		if (f.encoding == VideoFrameEncoding::V210 && (f.sourceOffsetX % 48u) != 0)
			return Reject("v210 rcSource left must be a multiple of 48");

		if (f.pictureAspectX == 0 || f.pictureAspectY == 0)
		{
			// VIDEOINFOHEADER (or unset): square pixels.
			const uint32_t g = detail::GcdU32(f.width, f.height);
			f.pictureAspectX = g ? f.width / g : 0;
			f.pictureAspectY = g ? f.height / g : 0;
		}

		f.rate = SnapAvgTimePerFrame(f.avgTimePerFrame);

		if (f.isVideoInfoHeader2)
		{
			const VIDEOINFOHEADER2* vih2 = reinterpret_cast<const VIDEOINFOHEADER2*>(mt.pbFormat);
			f.color = DecodeColorimetry((uint32_t)vih2->dwControlFlags, f.height);
		}
		else
		{
			f.color = DecodeColorimetry(0u, f.height);
		}

		f.ok = true;
		return f;
	}

	// DisplayMode from the picture size and a rational rate (the snapped
	// AvgTimePerFrame or the PTS-measured rate). Returns nullptr with a reason
	// when the rate is unknown or DisplayMode rejects the values.
	inline DisplayModeSharedPtr BuildFileDisplayMode(const FileVideoFormat& f, const RationalRate& rate,
		const char** rejection = nullptr)
	{
		const char* dummy = nullptr;
		const char** why = rejection ? rejection : &dummy;
		*why = nullptr;
		if (!f.ok)
		{
			*why = f.rejection ? f.rejection : "format not parsed";
			return nullptr;
		}
		if (!rate.IsValid())
		{
			*why = "frame rate unknown";
			return nullptr;
		}
		try
		{
			// Two fields per frame only when interlaced and not one field per sample.
			const bool twoFields = f.interlaced && !f.oneFieldPerSample;
			return std::make_shared<DisplayMode>(f.width, f.height, twoFields,
				rate.numerator, rate.denominator, f.fieldOrder);
		}
		catch (const std::exception&)
		{
			*why = "display mode rejected (resolution or refresh rate outside VP's range)";
			return nullptr;
		}
	}

	// Source bytes the repack reads for this format.
	inline size_t FileRequiredSourceBytes(const FileVideoFormat& f)
	{
		const size_t stride = f.sourceStrideBytes;
		if (VideoFrameEncodingIsTwoPlane420(f.encoding))
			return stride * (size_t)f.bufferHeight +
				stride * ((size_t)f.sourceOffsetY / 2 + ((size_t)f.height + 1) / 2);
		return stride * (size_t)f.bufferHeight;
	}

	namespace detail
	{
		// Copy one row: `available` source bytes, `rowBytes` destination bytes;
		// a short source row is completed by repeating its last element.
		inline void CopyRowPadded(uint8_t* dst, size_t rowBytes, const uint8_t* src, size_t available,
			size_t elementBytes)
		{
			const size_t n = available < rowBytes ? available : rowBytes;
			std::memcpy(dst, src, n);
			if (n < rowBytes)
			{
				if (elementBytes == 0 || n < elementBytes)
				{
					std::memset(dst + n, 0, rowBytes - n);
					return;
				}
				const uint8_t* last = src + (n - (n % elementBytes)) - elementBytes;
				size_t pos = n - (n % elementBytes);
				while (pos < rowBytes)
				{
					const size_t chunk = (rowBytes - pos) < elementBytes ? (rowBytes - pos) : elementBytes;
					std::memcpy(dst + pos, last, chunk);
					pos += chunk;
				}
			}
		}
	}

	// Repack a decoder buffer into VP's tightly packed layout (stride =
	// FileRowBytes(encoding, width) == VideoState::BytesPerRow()). Two-plane
	// 4:2:0: luma rows then ceil(height / 2) CbCr rows; bottom-up RGB is
	// flipped to top-down. Returns false on a short buffer or bad format.
	inline bool RepackToVpLayout(const FileVideoFormat& f, const uint8_t* src, size_t srcBytes,
		uint8_t* dst, size_t dstBytes)
	{
		if (!f.ok || src == nullptr || dst == nullptr)
			return false;
		const size_t dstRow = FileRowBytes(f.encoding, f.width);
		if (dstRow == 0)
			return false;
		if (dstBytes < FilePackedFrameBytes(f.encoding, f.width, f.height))
			return false;
		if (srcBytes < FileRequiredSourceBytes(f))
			return false;

		const size_t stride = f.sourceStrideBytes;
		if (VideoFrameEncodingIsTwoPlane420(f.encoding))
		{
			const size_t bps = FileBytesPerSample(f.encoding);
			const size_t xOffset = (size_t)f.sourceOffsetX * bps;
			const size_t available = stride > xOffset ? stride - xOffset : 0;
			// Luma.
			for (uint32_t y = 0; y < f.height; ++y)
			{
				const uint8_t* s = src + ((size_t)f.sourceOffsetY + y) * stride + xOffset;
				detail::CopyRowPadded(dst + (size_t)y * dstRow, dstRow, s, available, bps);
			}
			// Interleaved CbCr: x offset in bytes equals the luma one (pairs of
			// half-width samples); rows start at sourceOffsetY / 2.
			const uint8_t* chroma = src + stride * (size_t)f.bufferHeight;
			uint8_t* dstChroma = dst + dstRow * (size_t)f.height;
			const uint32_t chromaRows = (f.height + 1) / 2;
			for (uint32_t y = 0; y < chromaRows; ++y)
			{
				const uint8_t* s = chroma + ((size_t)f.sourceOffsetY / 2 + y) * stride + xOffset;
				detail::CopyRowPadded(dstChroma + (size_t)y * dstRow, dstRow, s, available, 2 * bps);
			}
			return true;
		}

		// Single plane (BGRA, v210).
		size_t xOffset = 0;
		size_t element = 0;
		if (f.encoding == VideoFrameEncoding::V210)
		{
			xOffset = (size_t)(f.sourceOffsetX / 48) * 128;
			element = 16; // 6 pixels per 16-byte group
		}
		else
		{
			xOffset = (size_t)f.sourceOffsetX * 4;
			element = 4;
		}
		const size_t available = stride > xOffset ? stride - xOffset : 0;
		for (uint32_t y = 0; y < f.height; ++y)
		{
			const size_t imageRow = (size_t)f.sourceOffsetY + y;
			const size_t bufferRow = f.bottomUp ? ((size_t)f.bufferHeight - 1 - imageRow) : imageRow;
			const uint8_t* s = src + bufferRow * stride + xOffset;
			detail::CopyRowPadded(dst + (size_t)y * dstRow, dstRow, s, available, element);
		}
		return true;
	}
}
