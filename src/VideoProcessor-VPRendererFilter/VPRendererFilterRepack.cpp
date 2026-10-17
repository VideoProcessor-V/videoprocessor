#include "VPRendererFilterRepack.h"

#include "VPRendererFilterMediaTypes.h"

#include <cstdio>
#include <cstring>


namespace VPRendererFilter
{
	namespace
	{
		bool Fail(std::string* reason, const char* text)
		{
			if (reason)
				*reason = text;
			return false;
		}

		// Copies rows of rowBytes from a plane; srcRowOf maps a destination row
		// to the source row index. Bounds checked against the whole buffer.
		template<typename RowMap>
		bool CopyRows(const RepackSource& s, size_t planeOffset, size_t xOffsetBytes,
			uint32_t rows, uint32_t rowBytes, RowMap srcRowOf, uint8_t* dst,
			std::string* reason)
		{
			for (uint32_t row = 0; row < rows; ++row)
			{
				const size_t srcRow = srcRowOf(row);
				const size_t begin = planeOffset + srcRow * s.strideBytes + xOffsetBytes;
				if (begin + rowBytes > s.size)
					return Fail(reason, "sample is smaller than its media type needs");
				memcpy(dst + static_cast<size_t>(row) * rowBytes, s.data + begin, rowBytes);
			}
			return true;
		}
	}


	bool RepackFrame(VideoFrameEncoding encoding, uint32_t width, uint32_t height,
		const RepackSource& s, uint8_t* dst, size_t dstSize, std::string* reason)
	{
		if (!s.data || !dst || width == 0 || height == 0)
			return Fail(reason, "empty source, destination or picture");
		const uint32_t rowBytes = TightRowBytes(encoding, width);
		if (rowBytes == 0)
			return Fail(reason, "encoding is not repackable");
		if (dstSize != TightFrameBytes(encoding, width, height))
			return Fail(reason, "destination size does not match the tight layout");
		if (s.originY + height > s.codedHeight)
			return Fail(reason, "picture extends below the coded height");

		const uint32_t bpp = BytesPerPixelPosition(encoding);
		const size_t xOffset = static_cast<size_t>(s.originX) * bpp;
		if (encoding == VideoFrameEncoding::V210 && s.originX != 0)
			return Fail(reason, "v210 picture origin must be 0");
		if (xOffset + rowBytes > s.strideBytes)
			return Fail(reason, "stride is narrower than the picture");

		if (VideoFrameEncodingIsTwoPlane420(encoding))
		{
			if ((s.originX & 1) != 0 || (s.originY & 1) != 0)
				return Fail(reason, "4:2:0 origin is not on a chroma sample");
			const uint32_t originY = s.originY;
			if (!CopyRows(s, 0, xOffset, height, rowBytes,
				[originY](uint32_t r) { return static_cast<size_t>(originY) + r; },
				dst, reason))
				return false;

			const uint32_t chromaRows = (height + 1) / 2;
			const uint32_t chromaOriginY = s.originY / 2;
			if (chromaOriginY + chromaRows > (s.codedHeight + 1) / 2)
				return Fail(reason, "CbCr rows extend below the coded chroma height");
			const size_t chromaPlane = static_cast<size_t>(s.strideBytes) * s.codedHeight;
			return CopyRows(s, chromaPlane, xOffset, chromaRows, rowBytes,
				[chromaOriginY](uint32_t r) { return static_cast<size_t>(chromaOriginY) + r; },
				dst + static_cast<size_t>(rowBytes) * height, reason);
		}

		const uint32_t originY = s.originY;
		const uint32_t codedHeight = s.codedHeight;
		if (s.bottomUp)
			return CopyRows(s, 0, xOffset, height, rowBytes,
				[originY, codedHeight](uint32_t r)
				{ return static_cast<size_t>(codedHeight) - 1 - originY - r; },
				dst, reason);
		return CopyRows(s, 0, xOffset, height, rowBytes,
			[originY](uint32_t r) { return static_cast<size_t>(originY) + r; },
			dst, reason);
	}
}
