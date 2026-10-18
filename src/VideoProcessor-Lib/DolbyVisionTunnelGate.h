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
// The gate cannot look at the wire bit depth: an 8-bit tunnel padded to 10
// bits on the link arrives "high8" and is a valid tunnel. So a 10-bit RGB
// 4:4:4 signal (LLDV converted to RGB by an HDFury) and 8-bit RGB LLDV enter
// the tunnel formatter too. DV input fixes 2026-10-18 ([general]
// dv_tunnel_handoff, default on): the GUI's hand-off probe
// (DolbyVisionTunnelHandoff.h) watches the captured pictures while the gate
// is open; a signal proven not to be a tunnel (no CRC-valid header and no
// tunnel-like content for about a second, plus 10-bit content or BT.2020
// colorimetry) is handed off: the gate closes for it (the normal r210 path
// shows it) and the tunnel no longer owns it, so newlldv can infer PQ. A
// CRC-valid header seen later takes the signal back.
//
// The tunnel is only ever 8-bit RGB 4:4:4 on the link, so a YCbCr capture
// (V210 / UYVY / HDYC: Apple TV LLDV is usually YCbCr 4:2:2 12-bit) can never
// be a tunnel. Under the same switch, a profile whose DV mode is DV Tunneling
// no longer owns a YCbCr signal either (it used to own every signal, so
// newlldv never inferred PQ for LLDV / HDR10 without InfoFrame whenever such
// a profile matched); the gate itself never opened for YCbCr.
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
	// DV input fixes 2026-10-18: the hand-off probe proved this signal is not
	// a tunnel (only set while dv_tunnel_handoff is on).
	bool handedOff = false;
	// [general] dv_tunnel_handoff: YCbCr is never claimed (see above).
	bool declineYCbCr = true;
};

// A capture the HDMI tunnel can never arrive in (YCbCr 4:2:2).
inline bool DolbyVisionTunnelImpossibleEncoding(VideoFrameEncoding encoding)
{
	return encoding == VideoFrameEncoding::V210 || encoding == VideoFrameEncoding::UYVY ||
		encoding == VideoFrameEncoding::HDYC;
}

// The tunnel formatter would take this capture if it were not handed off:
// the condition under which the hand-off probe watches the pictures.
inline bool DolbyVisionTunnelGateWouldOpen(const DolbyVisionTunnelGateInput& in)
{
	return in.tunnelSwitch && in.libplaceboRenderer && in.captureStateValid &&
		in.encoding == VideoFrameEncoding::R210 && in.eotf == EOTF::SDR;
}

// The capture goes through CR210DolbyVisionTunnelFormatter (VideoState::
// dolbyVisionTunnel).
inline bool DolbyVisionTunnelGateOpen(const DolbyVisionTunnelGateInput& in)
{
	return !in.handedOff && DolbyVisionTunnelGateWouldOpen(in);
}

// The tunnel owns the signal for the newlldv inference (HdrInferenceSignal::
// dolbyVisionTunnel): the same predicate, or a profile that forces the tunnel;
// never a signal the hand-off probe proved is not a tunnel.
inline bool DolbyVisionTunnelOwnsSignal(const DolbyVisionTunnelGateInput& in)
{
	if (in.declineYCbCr && DolbyVisionTunnelImpossibleEncoding(in.encoding))
		return false;
	return !in.handedOff && (in.profileForcesTunnel || (in.tunnelSwitch && in.libplaceboRenderer &&
		in.encoding == VideoFrameEncoding::R210 && in.eotf == EOTF::SDR));
}
