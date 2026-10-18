/*
 * Copyright(C) 2021 Dennis Fleurbaaij <mail@dennisfleurbaaij.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once


#include <atomic>

#include <EOTF.h>
#include <ColorSpace.h>
#include <DisplayMode.h>
#include <VideoFrameEncoding.h>
#include <HDRData.h>

#include <cstdint>

// The Dolby Vision VSIF as a capture device reports it (VideoState).
enum class DolbyVisionVsif : uint8_t
{
	Unknown,     // the device does not report it (DeckLink)
	Absent,      // reported: no Dolby VSIF
	LowLatency,  // player-led LLDV (source-mapped PQ, no tunnel)
	TvLed,       // TV-led Dolby Vision (the RGB tunnel)
};


/**
 * This describes the video stream
 */
class VideoState:
	public IUnknown
{
public:
	VideoState() {}
	VideoState(const VideoState&);

	// True if valid, else do not interpret other fields
	bool valid = false;

	DisplayModeSharedPtr displayMode = nullptr;
	VideoFrameEncoding videoFrameEncoding = VideoFrameEncoding::UNKNOWN;
	EOTF eotf = EOTF::UNKNOWN;
	ColorSpace colorspace = ColorSpace::UNKNOWN;
	bool invertedVertical = false;
	// TV-led Dolby Vision HDMI RGB tunnel in DeckLink R210.
	bool dolbyVisionTunnel = false;

	// [general] newlldv: PQ / BT.2020 was inferred for a BT.2020, 10/12-bit
	// source without an HDR InfoFrame (LLDV or HDR10 with the flag dropped).
	// Rule variable hdr_inferred.
	bool hdrInferred = false;

	// Will be non-null if valid
	HDRDataSharedPtr hdrData = nullptr;

	// DV input fixes 2026-10-18: what the source's Dolby Vision vendor-
	// specific InfoFrame (VSIF) says, when the capture device reports it.
	// DeckLink does not expose the VSIF (Unknown); a device that does sets
	// LowLatency for player-led LLDV (low_latency = 1) or TvLed for the
	// tunnel. It is the explicit LLDV indication and wins over inference
	// (DolbyVisionTunnelHandoff.h, HdrInference).
	DolbyVisionVsif dolbyVisionVsif = DolbyVisionVsif::Unknown;

	// IUnknown
	HRESULT	QueryInterface(REFIID iid, LPVOID* ppv) override;
	ULONG AddRef() override;
	ULONG Release() override;

	// Return the the amount of bytes needed to store a row/line of pixels in this format
	uint32_t BytesPerRow() const;

	// Return the the amount of bytes needed to store a full frame of pixels in this format
	uint32_t BytesPerFrame() const;

private:

	std::atomic<ULONG> m_refCount{ 0 };   // Bug hunt 2026-10-17: was uninitialised (callers assign it to a CComPtr)
};


typedef CComPtr<VideoState> VideoStateComPtr;
