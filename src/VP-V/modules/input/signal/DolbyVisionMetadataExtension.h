// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// An optional extension of VpvDolbyVisionTunnelFormatter's display-management (DM) handling.
// WP-14 (2026-10-25): lets VP-V add DM extension levels the base parser does not handle
// and a policy for rejected DM, without changing Bill's CRGBDolbyVisionTunnelFormatter.
// No extension installed (the default) = the formatter's own behaviour, unchanged.
//
// Call order per analysed picture (capture/conversion thread, the formatter's thread):
//   ParseExtensionBlock - while a CRC-valid metadata body is parsed, once per extension block
//                         whose level the formatter does not handle itself;
//   KeepDmPastLossLimit - when the packet-loss limit is reached with DV locked: true keeps the
//                         retained DM instead of clearing it;
//   OnPicture           - after the formatter's own loss handling: true clears the retained DM now
//                         (the picture then shows black until fresh DM, as after the loss limit);
//   Decorate            - when the renderer reads the current picture's DM (may reduce or
//                         annotate the copy it gets; the retained DM is not changed).
// OnSignal - on every new video state (rate measured), before the first picture of the signal.
#include <video_frame_formatter/IVideoFrameFormatter.h>

#include <cstddef>
#include <cstdint>

class IDolbyVisionMetadataExtension
{
public:
	virtual ~IDolbyVisionMetadataExtension() = default;

	struct Picture
	{
		bool fixesOn = false;          // the formatter's fixes switch (dv_tunnel_fixes)
		bool tunnelConfirmed = false;  // DV locked
		bool acceptedMetadata = false; // this picture's own DM was accepted
		bool haveMetadata = false;     // a retained DM exists (m_metadata.valid)
		bool bodyUnsupported = false;  // a CRC-valid packet whose body the formatter cannot use
		uint64_t counter = 0;          // the source picture counter
	};

	virtual void OnSignal(double frameRate, double sdrFallbackSeconds) { (void)frameRate; (void)sdrFallbackSeconds; }
	virtual bool ParseExtensionBlock(uint8_t level, const uint8_t* payload, size_t length, DolbyVisionTunnelFrameMetadata& metadata)
	{
		(void)level; (void)payload; (void)length; (void)metadata;
		return false;
	}
	virtual bool KeepDmPastLossLimit(bool fixesOn, bool supportedHeader, bool haveMetadata) const
	{
		(void)fixesOn; (void)supportedHeader; (void)haveMetadata;
		return false;
	}
	virtual bool OnPicture(const Picture& picture) { (void)picture; return false; }
	virtual void Decorate(DolbyVisionTunnelFrameMetadata& metadata, bool currentPictureHasValidHeader) const
	{
		(void)metadata; (void)currentPictureHasValidHeader;
	}
};
