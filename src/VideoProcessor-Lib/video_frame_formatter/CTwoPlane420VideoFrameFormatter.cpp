#include <pch.h>
#include <VideoFrame.h>
#include <VideoState.h>

#include "CTwoPlane420VideoFrameFormatter.h"

#include <cstring>
#include <stdexcept>


VideoFrameFormatterOutputContract CTwoPlane420VideoFrameFormatter::ContractFor(
	VideoFrameEncoding encoding)
{
	switch (encoding)
	{
	case VideoFrameEncoding::NV12:
		return { VideoFrameSampleRange::LIMITED, 8, 8 };
	case VideoFrameEncoding::P010:
		return { VideoFrameSampleRange::LIMITED, 10, 6 };
	case VideoFrameEncoding::P016:
		return { VideoFrameSampleRange::LIMITED, 16, 0 };
	default:
		return {};
	}
}


void CTwoPlane420VideoFrameFormatter::OnVideoState(VideoStateComPtr& videoState)
{
	if (!videoState || !videoState->displayMode)
		throw std::runtime_error("Two-plane 4:2:0 formatter: video state or display mode missing");
	if (!VideoFrameEncodingIsTwoPlane420(videoState->videoFrameEncoding))
		throw std::runtime_error("Two-plane 4:2:0 formatter: encoding is not NV12, P010 or P016");
	const uint32_t width = videoState->displayMode->FrameWidth();
	const uint32_t height = videoState->displayMode->FrameHeight();
	if (width == 0 || height == 0)
		throw std::runtime_error("Two-plane 4:2:0 formatter: empty frame size");
	if (width % 2 != 0)
		throw std::runtime_error("Two-plane 4:2:0 formatter: odd frame width is not supported");

	m_encoding = videoState->videoFrameEncoding;
	m_width = width;
	m_rows = height + ChromaRows(height);
	m_inRowBytes = videoState->BytesPerRow();
	m_outRowBytes = OutRowBytes(width);
	m_outFrameBytes = m_outRowBytes * m_rows;
}


bool CTwoPlane420VideoFrameFormatter::FormatVideoFrame(
	const VideoFrame& inFrame, BYTE* outBuffer)
{
	if (m_outFrameBytes == 0)
		throw std::runtime_error("Two-plane 4:2:0 formatter: call OnVideoState() first");
	const BYTE* const in = static_cast<const BYTE*>(inFrame.GetData());
	if (!in || !outBuffer)
		return false;

	if (m_encoding == VideoFrameEncoding::NV12)
	{
		// Luma and interleaved CbCr rows alike: m_width 8-bit samples per row.
		for (uint32_t row = 0; row < m_rows; ++row)
		{
			const BYTE* source = in + static_cast<size_t>(row) * m_inRowBytes;
			uint16_t* target = reinterpret_cast<uint16_t*>(
				outBuffer + static_cast<size_t>(row) * m_outRowBytes);
			for (uint32_t x = 0; x < m_width; ++x)
				target[x] = static_cast<uint16_t>(source[x] << 8);
		}
		return true;
	}

	// P010 / P016: the input already is the 16-bit two-plane layout.
	if (m_inRowBytes == m_outRowBytes)
	{
		memcpy(outBuffer, in, m_outFrameBytes);
		return true;
	}
	for (uint32_t row = 0; row < m_rows; ++row)
		memcpy(outBuffer + static_cast<size_t>(row) * m_outRowBytes,
			in + static_cast<size_t>(row) * m_inRowBytes, m_outRowBytes);
	return true;
}


LONG CTwoPlane420VideoFrameFormatter::GetOutFrameSize() const
{
	return static_cast<LONG>(m_outFrameBytes);
}
