#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The newlldv inference input built from a capture state (moved out of
// CVideoProcessorDlg::BuildHdrInferenceSignal unchanged, 2026-10-17, so the
// incoming-HDR-mode tests drive HdrInferenceTracker with the GUI's own
// signal): colorimetry, EOTF, whether a static HDR (DRM) InfoFrame arrived,
// the wire bit depth (card report, else implied by the capture encoding),
// whether the DV tunnel owns the signal, and the signal identity key.
#include <BitDepth.h>
#include <VP-V/modules/input/signal/DolbyVisionTunnelGate.h>
#include <VP-V/modules/input/signal/HdrInference.h>
#include <VideoState.h>
#include <cmath>
#include <cstdint>

// tunnel: the switch / override / renderer fields; encoding, EOTF and
// validity are taken from the state here.
inline HdrInferenceSignal BuildHdrInferenceSignalFromState(const VideoState* state,
	bool newlldvEnabled, BitDepth cardInputBitDepth, DolbyVisionTunnelGateInput tunnel,
	bool explicitSettingsForbid)
{
	HdrInferenceSignal signal;
	signal.enabled = newlldvEnabled;
	signal.valid = state && state->valid;
	if (!signal.valid)
		return signal;
	signal.colorspace = state->colorspace;
	signal.eotf = state->eotf;
	signal.hdrInfoFrame = state->hdrData != nullptr;
	switch (cardInputBitDepth)
	{
	case BitDepth::BITDEPTH_8BIT: signal.bitDepth = 8; break;
	case BitDepth::BITDEPTH_10BIT: signal.bitDepth = 10; break;
	case BitDepth::BITDEPTH_12BIT: signal.bitDepth = 12; break;
	default:
		// The card did not report the wire depth: use the capture encoding.
		switch (state->videoFrameEncoding)
		{
		case VideoFrameEncoding::UYVY:
		case VideoFrameEncoding::HDYC:
		case VideoFrameEncoding::ARGB_8BIT:
		case VideoFrameEncoding::BGRA_8BIT:
			signal.bitDepth = 8; break;
		case VideoFrameEncoding::V210:
		case VideoFrameEncoding::R210:
		case VideoFrameEncoding::R10b:
		case VideoFrameEncoding::R10l:
			signal.bitDepth = 10; break;
		case VideoFrameEncoding::R12B:
		case VideoFrameEncoding::R12L:
			signal.bitDepth = 12; break;
		default:
			signal.bitDepth = 0; break;
		}
		break;
	}
	// The DV tunnel (same predicate as BuildPushVideoState) and a profile that
	// forces it own the signal; the inference never applies to them.
	tunnel.captureStateValid = state->valid;
	tunnel.encoding = state->videoFrameEncoding;
	tunnel.eotf = state->eotf;
	signal.dolbyVisionTunnel = DolbyVisionTunnelOwnsSignal(tunnel);
	// DV input fixes 2026-10-18: the explicit LLDV indications (8-bit may
	// then qualify): the Dolby VSIF, or the tunnel hand-off on a DV input.
	signal.lldvIndicated = state->dolbyVisionVsif == DolbyVisionVsif::LowLatency;
	signal.dvTunnelHandedOff = tunnel.handedOff;
	signal.explicitSettingsForbid = explicitSettingsForbid;
	const DisplayModeSharedPtr& mode = state->displayMode;
	uint64_t key = static_cast<uint64_t>(state->videoFrameEncoding) & 0xff;
	if (mode)
	{
		key = (key << 14) ^ (mode->FrameWidth() & 0x3fff);
		key = (key << 14) ^ (mode->FrameHeight() & 0x3fff);
		key = (key << 20) ^ (static_cast<uint64_t>(
			std::llround(mode->RefreshRateHz() * 1000.0)) & 0xfffff);
		key = (key << 1) ^ (mode->IsInterlaced() ? 1 : 0);
	}
	signal.signalKey = key;
	return signal;
}
