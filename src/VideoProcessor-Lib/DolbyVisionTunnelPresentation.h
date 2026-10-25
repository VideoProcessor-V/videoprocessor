#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// What the renderer presents for one picture captured with the Dolby Vision
// tunnel enabled (moved out of LibplaceboVideoRenderer.cpp unchanged,
// 2026-10-17, so the DV loopback harness applies the renderer's own rule):
//   SdrPassthrough  r210 only, no valid DM, SDR allowed (before DV lock or
//                   after EOS / the SDR exit) and the RGB8 placement known;
//   Black           no valid DM, or the current picture is untrusted;
//   Picture         decoded 12-bit IPT with its DM.
// R10b / R10l never take the SDR passthrough (libplacebo's native upload has
// no plane format for them); they stay black until DV is trusted.
#include <VideoFrameEncoding.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>

enum class DolbyVisionTunnelPresentation { Picture, SdrPassthrough, Black };

inline bool DolbyVisionTunnelSdrPassthrough(VideoFrameEncoding encoding,
	const DolbyVisionTunnelFrameMetadata& dolby)
{
	return encoding == VideoFrameEncoding::R210 && !dolby.valid && dolby.allowSdrFallback &&
		(dolby.rgbBitAlignment == 1 || dolby.rgbBitAlignment == 2);
}

inline DolbyVisionTunnelPresentation DecideDolbyVisionTunnelPresentation(
	VideoFrameEncoding encoding, const DolbyVisionTunnelFrameMetadata& dolby)
{
	if (DolbyVisionTunnelSdrPassthrough(encoding, dolby))
		return DolbyVisionTunnelPresentation::SdrPassthrough;
	if (!dolby.valid || dolby.requiresBlackFrame)
		return DolbyVisionTunnelPresentation::Black;
	return DolbyVisionTunnelPresentation::Picture;
}
