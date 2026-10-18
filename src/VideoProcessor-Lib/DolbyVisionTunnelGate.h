#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// When VP routes a capture through the Dolby Vision tunnel formatter (moved
// out of VideoProcessorDlg unchanged, 2026-10-17, so the DV loopback harness
// and the HDR-mode tests apply the GUI's own rule): the tunnel switch (or a
// profile that forces it) is on, the renderer is libplacebo, the capture state
// is valid, the container is r210 and the capture reports SDR (DeckLink
// reports the tunnel's RGB wrapper as SDR / Rec.709).
//
// Known limitation (DV loopback finding 2026-10-17, not changed here): the
// gate does not look at the wire bit depth, so a 10-bit RGB 4:4:4 signal
// captured as r210 (e.g. LLDV converted to RGB by an HDFury) enters the tunnel
// formatter when the switch is on: the alignment probe never resolves on
// 10-bit content and the picture stays black, and the newlldv inference is
// suppressed (signal.dolbyVisionTunnel). A bit-depth condition is not safe as
// a fix: an 8-bit tunnel padded to 10 bits on the link arrives "high8" and is
// a valid tunnel. See docs/VP-dv-tunnel-autotest.md.
#include <EOTF.h>
#include <VideoFrameEncoding.h>

struct DolbyVisionTunnelGateInput
{
	bool tunnelSwitch = false;        // [general] dolby_vision_tunnel / profile override
	bool profileForcesTunnel = false; // profile override == 1
	bool libplaceboRenderer = false;
	bool captureStateValid = false;
	VideoFrameEncoding encoding = VideoFrameEncoding::UNKNOWN;
	EOTF eotf = EOTF::UNKNOWN;
};

// The capture goes through CR210DolbyVisionTunnelFormatter (VideoState::
// dolbyVisionTunnel).
inline bool DolbyVisionTunnelGateOpen(const DolbyVisionTunnelGateInput& in)
{
	return in.tunnelSwitch && in.libplaceboRenderer && in.captureStateValid &&
		in.encoding == VideoFrameEncoding::R210 && in.eotf == EOTF::SDR;
}

// The tunnel owns the signal for the newlldv inference (HdrInferenceSignal::
// dolbyVisionTunnel): the same predicate, or a profile that forces the tunnel.
inline bool DolbyVisionTunnelOwnsSignal(const DolbyVisionTunnelGateInput& in)
{
	return in.profileForcesTunnel || (in.tunnelSwitch && in.libplaceboRenderer &&
		in.encoding == VideoFrameEncoding::R210 && in.eotf == EOTF::SDR);
}
