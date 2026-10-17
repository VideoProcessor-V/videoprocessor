#include "VPRendererFilterMediaTypes.h"

#include <amvideo.h>
#include <dvdmedia.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>


namespace VPRendererFilter
{
	namespace
	{
		// FOURCC subtype GUID: {fourcc-0000-0010-8000-00AA00389B71}.
		constexpr GUID FourccGuid(uint32_t fourcc)
		{
			return GUID{ fourcc, 0x0000, 0x0010,
				{ 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
		}

		constexpr uint32_t Fourcc(char a, char b, char c, char d)
		{
			return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
				(static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
				(static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
				(static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
		}

		uint64_t Gcd(uint64_t a, uint64_t b)
		{
			while (b != 0)
			{
				const uint64_t t = a % b;
				a = b;
				b = t;
			}
			return a;
		}

		std::string Format(const char* format, ...)
		{
			char buffer[256];
			va_list args;
			va_start(args, format);
			vsnprintf(buffer, sizeof(buffer), format, args);
			va_end(args);
			return buffer;
		}

		// True when the exact duration 10^7 * denominator / numerator is within
		// one 100 ns unit of duration100ns (sources round or truncate).
		bool Reproduces(int64_t duration100ns, uint64_t numerator, uint64_t denominator)
		{
			if (numerator == 0 || denominator == 0 ||
				numerator > 0xFFFFFFFFull || denominator > 0xFFFFFFFFull)
				return false;
			// |10^7 * d / n - T| < 1  <=>  |10^7 * d - T * n| < n
			const int64_t lhs = static_cast<int64_t>(10000000ull * denominator) -
				duration100ns * static_cast<int64_t>(numerator);
			return static_cast<uint64_t>(lhs < 0 ? -lhs : lhs) < numerator;
		}

		RationalRate Reduced(uint64_t numerator, uint64_t denominator)
		{
			const uint64_t g = Gcd(numerator, denominator);
			RationalRate r;
			r.numerator = static_cast<uint32_t>(numerator / g);
			r.denominator = static_cast<uint32_t>(denominator / g);
			return r;
		}
	}

	const GUID kMediaTypeVideo = FourccGuid(Fourcc('v', 'i', 'd', 's'));
	const GUID kFormatVideoInfo = { 0x05589f80, 0xc356, 0x11ce,
		{ 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a } };
	const GUID kFormatVideoInfo2 = { 0xf72a76a0, 0xeb0a, 0x11d0,
		{ 0xac, 0xe4, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };
	const GUID kSubtypeNV12 = FourccGuid(Fourcc('N', 'V', '1', '2'));
	const GUID kSubtypeP010 = FourccGuid(Fourcc('P', '0', '1', '0'));
	const GUID kSubtypeP016 = FourccGuid(Fourcc('P', '0', '1', '6'));
	const GUID kSubtypeV210 = FourccGuid(Fourcc('v', '2', '1', '0'));
	const GUID kSubtypeRGB32 = { 0xe436eb7e, 0x524f, 0x11ce,
		{ 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };


	const std::vector<SubtypeInfo>& AcceptedSubtypes()
	{
		static const std::vector<SubtypeInfo> subtypes = {
			{ kSubtypeP010, VideoFrameEncoding::P010, "P010" },
			{ kSubtypeP016, VideoFrameEncoding::P016, "P016" },
			{ kSubtypeNV12, VideoFrameEncoding::NV12, "NV12" },
			{ kSubtypeV210, VideoFrameEncoding::V210, "v210" },
			{ kSubtypeRGB32, VideoFrameEncoding::BGRA_8BIT, "RGB32" },
		};
		return subtypes;
	}


	bool LookupSubtype(const GUID& subtype, SubtypeInfo& out)
	{
		for (const SubtypeInfo& info : AcceptedSubtypes())
		{
			if (IsEqualGUID(info.subtype, subtype))
			{
				out = info;
				return true;
			}
		}
		return false;
	}


	Colorimetry DecodeColorimetry(DWORD controlFlags)
	{
		Colorimetry c;
		c.present = (controlFlags & AMCONTROL_COLORINFO_PRESENT) != 0;
		if (!c.present)
			return c;
		// DXVA_ExtendedFormat: SampleFormat:8 (the AMCONTROL flags),
		// VideoChromaSubsampling:4, NominalRange:3, VideoTransferMatrix:3,
		// VideoLighting:4, VideoPrimaries:5, VideoTransferFunction:5.
		c.chromaSubsampling = (controlFlags >> 8) & 0xF;
		c.nominalRange = (controlFlags >> 12) & 0x7;
		c.transferMatrix = (controlFlags >> 15) & 0x7;
		c.lighting = (controlFlags >> 18) & 0xF;
		c.primaries = (controlFlags >> 22) & 0x1F;
		c.transferFunction = (controlFlags >> 27) & 0x1F;
		return c;
	}


	uint32_t BytesPerPixelPosition(VideoFrameEncoding encoding)
	{
		switch (encoding)
		{
		case VideoFrameEncoding::NV12: return 1;
		case VideoFrameEncoding::P010:
		case VideoFrameEncoding::P016: return 2;
		case VideoFrameEncoding::BGRA_8BIT: return 4;
		default: return 0;
		}
	}


	uint32_t SourceStrideBytes(VideoFrameEncoding encoding, uint32_t codedWidth)
	{
		switch (encoding)
		{
		case VideoFrameEncoding::V210:
			return ((codedWidth + 47) / 48) * 128;
		case VideoFrameEncoding::NV12:
		case VideoFrameEncoding::P010:
		case VideoFrameEncoding::P016:
		case VideoFrameEncoding::BGRA_8BIT:
			return codedWidth * BytesPerPixelPosition(encoding);
		default:
			return 0;
		}
	}


	size_t SourceFrameBytes(VideoFrameEncoding encoding, uint32_t strideBytes,
		uint32_t codedHeight)
	{
		if (VideoFrameEncodingIsTwoPlane420(encoding))
			return static_cast<size_t>(strideBytes) *
				(codedHeight + (codedHeight + 1) / 2);
		return static_cast<size_t>(strideBytes) * codedHeight;
	}


	uint32_t TightRowBytes(VideoFrameEncoding encoding, uint32_t width)
	{
		switch (encoding)
		{
		case VideoFrameEncoding::V210:
			return ((width + 47) / 48) * 128;
		case VideoFrameEncoding::BGRA_8BIT:
			return width * 4;
		case VideoFrameEncoding::NV12:
			return (width + 1) & ~1u;
		case VideoFrameEncoding::P010:
		case VideoFrameEncoding::P016:
			return ((width + 1) & ~1u) * 2;
		default:
			return 0;
		}
	}


	size_t TightFrameBytes(VideoFrameEncoding encoding, uint32_t width,
		uint32_t height)
	{
		const size_t row = TightRowBytes(encoding, width);
		if (VideoFrameEncodingIsTwoPlane420(encoding))
			return row * (height + (height + 1) / 2);
		return row * height;
	}


	bool ParseVideoMediaType(const AM_MEDIA_TYPE& mt, VideoFormat& out,
		std::string& reason)
	{
		out = VideoFormat();
		if (!IsEqualGUID(mt.majortype, kMediaTypeVideo))
		{
			reason = "major type is not MEDIATYPE_Video";
			return false;
		}
		SubtypeInfo info;
		if (!LookupSubtype(mt.subtype, info))
		{
			reason = Format("subtype %08lX-... is not accepted (P010, P016, NV12, v210, RGB32)",
				mt.subtype.Data1);
			return false;
		}
		out.subtype = info.subtype;
		out.encoding = info.encoding;

		const BITMAPINFOHEADER* bmi = nullptr;
		RECT rcSource = {};
		if (IsEqualGUID(mt.formattype, kFormatVideoInfo2))
		{
			if (!mt.pbFormat || mt.cbFormat < sizeof(VIDEOINFOHEADER2))
			{
				reason = "VIDEOINFOHEADER2 format block is missing or short";
				return false;
			}
			const VIDEOINFOHEADER2* vih2 =
				reinterpret_cast<const VIDEOINFOHEADER2*>(mt.pbFormat);
			bmi = &vih2->bmiHeader;
			rcSource = vih2->rcSource;
			out.avgTimePerFrame = vih2->AvgTimePerFrame;
			out.bitRate = vih2->dwBitRate;
			out.bitErrorRate = vih2->dwBitErrorRate;
			out.aspectX = vih2->dwPictAspectRatioX;
			out.aspectY = vih2->dwPictAspectRatioY;
			out.interlaced = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) != 0;
			out.topFieldFirst = !out.interlaced ||
				(vih2->dwInterlaceFlags & AMINTERLACE_Field1First) != 0;
			out.colorimetry = DecodeColorimetry(vih2->dwControlFlags);
			out.hasVideoInfo2 = true;
		}
		else if (IsEqualGUID(mt.formattype, kFormatVideoInfo))
		{
			if (!mt.pbFormat || mt.cbFormat < sizeof(VIDEOINFOHEADER))
			{
				reason = "VIDEOINFOHEADER format block is missing or short";
				return false;
			}
			const VIDEOINFOHEADER* vih =
				reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
			bmi = &vih->bmiHeader;
			rcSource = vih->rcSource;
			out.avgTimePerFrame = vih->AvgTimePerFrame;
			out.bitRate = vih->dwBitRate;
			out.bitErrorRate = vih->dwBitErrorRate;
		}
		else
		{
			reason = "format type is neither FORMAT_VideoInfo2 nor FORMAT_VideoInfo";
			return false;
		}

		if (bmi->biWidth <= 0 || bmi->biHeight == 0)
		{
			reason = Format("invalid coded size %ldx%ld", bmi->biWidth, bmi->biHeight);
			return false;
		}
		if (out.encoding == VideoFrameEncoding::BGRA_8BIT && bmi->biBitCount != 32)
		{
			reason = Format("RGB32 with biBitCount %u", bmi->biBitCount);
			return false;
		}
		out.codedWidth = static_cast<uint32_t>(bmi->biWidth);
		out.codedHeight = static_cast<uint32_t>(std::labs(bmi->biHeight));
		// Only RGB has bottom-up DIBs; YUV is top-down whatever the sign.
		out.bottomUp = out.encoding == VideoFrameEncoding::BGRA_8BIT && bmi->biHeight > 0;
		out.sourceStrideBytes = SourceStrideBytes(out.encoding, out.codedWidth);

		if (rcSource.right > rcSource.left && rcSource.bottom > rcSource.top)
		{
			if (rcSource.left < 0 || rcSource.top < 0 ||
				static_cast<uint32_t>(rcSource.right) > out.codedWidth ||
				static_cast<uint32_t>(rcSource.bottom) > out.codedHeight)
			{
				reason = Format("rcSource (%ld,%ld)-(%ld,%ld) outside the coded %ux%u",
					rcSource.left, rcSource.top, rcSource.right, rcSource.bottom,
					out.codedWidth, out.codedHeight);
				return false;
			}
			out.originX = static_cast<uint32_t>(rcSource.left);
			out.originY = static_cast<uint32_t>(rcSource.top);
			out.width = static_cast<uint32_t>(rcSource.right - rcSource.left);
			out.height = static_cast<uint32_t>(rcSource.bottom - rcSource.top);
		}
		else
		{
			out.width = out.codedWidth;
			out.height = out.codedHeight;
		}
		if (VideoFrameEncodingIsTwoPlane420(out.encoding) &&
			((out.originX & 1) != 0 || (out.originY & 1) != 0))
		{
			reason = Format("4:2:0 picture origin (%u,%u) is not on a chroma sample",
				out.originX, out.originY);
			return false;
		}
		if (out.encoding == VideoFrameEncoding::V210 && out.originX != 0)
		{
			reason = Format("v210 picture origin x=%u is not supported", out.originX);
			return false;
		}

		if (out.aspectX == 0 || out.aspectY == 0)
		{
			const uint64_t g = Gcd(out.width, out.height);
			out.aspectX = static_cast<uint32_t>(out.width / g);
			out.aspectY = static_cast<uint32_t>(out.height / g);
		}
		return true;
	}


	RationalRate RationalFromFrameDuration(int64_t duration100ns)
	{
		RationalRate none;
		if (duration100ns <= 0)
			return none;
		const uint64_t T = static_cast<uint64_t>(duration100ns);

		// Integer rate.
		const uint64_t integer = (10000000ull + T / 2) / T;
		if (Reproduces(duration100ns, integer, 1))
			return Reduced(integer, 1);

		// 1000/1001 timebase family: (k * 1000) / 1001.
		const uint64_t k = (10000000ull * 1001ull / T + 500ull) / 1000ull;
		if (k > 0 && Reproduces(duration100ns, k * 1000ull, 1001))
			return Reduced(k * 1000ull, 1001);

		// Simplest convergent of 10^7 / T that reproduces T; the last convergent
		// is exact, so this always ends.
		uint64_t a = 10000000ull, b = T;
		uint64_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;
		while (b != 0)
		{
			const uint64_t term = a / b;
			const uint64_t p2 = term * p1 + p0;
			const uint64_t q2 = term * q1 + q0;
			if (p2 > 0xFFFFFFFFull || q2 > 0xFFFFFFFFull)
				break;
			if (Reproduces(duration100ns, p2, q2))
				return Reduced(p2, q2);
			p0 = p1; q0 = q1; p1 = p2; q1 = q2;
			const uint64_t r = a % b;
			a = b;
			b = r;
		}
		return Reduced(10000000ull, T);
	}
}
