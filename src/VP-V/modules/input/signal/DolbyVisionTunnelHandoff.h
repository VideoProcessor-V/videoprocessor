#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// DV input fixes 2026-10-18 ([general] dv_tunnel_handoff, default on): the
// tunnel must not swallow signals that are not a tunnel.
//
// With the DV tunnel switch on, every r210 capture reported as SDR enters the
// tunnel formatter (DolbyVisionTunnelGate.h). Two player-led signals do too:
//   - LLDV / HDR10 as 10-bit RGB 4:4:4 (an HDFury converting LLDV to RGB):
//     the RGB8 placement never resolves, so the picture stayed black, and the
//     tunnel owned the signal, so newlldv never inferred PQ;
//   - LLDV as 8-bit RGB: shown through the tunnel's SDR passthrough as
//     BT.709 / BT.1886, a PQ picture shown as SDR.
// This probe watches the captured pictures on the capture thread while the
// gate is open (or the signal is handed off) and applies the formatter's own
// rules (CR210DolbyVisionTunnelFormatter's static probes):
//   tunnel evidence  a CRC-valid header (the only proof of a tunnel), or
//                    tunnel-like content (the SDR exit's cue: noisy low
//                    nibbles in B, neutral R) - a damaged or range-converted
//                    tunnel stays with the tunnel formatter (black, never
//                    packed pixels on screen);
//   hand-off         no tunnel evidence for kHandoffSeconds of source pictures
//                    (counted from the capture counter, so drops count) AND
//                    either 10-bit content (both ends of the 10-bit fields in
//                    use: never RGB8 stored untouched) at any colorimetry, or
//                    RGB content with BT.2020 colorimetry and no HDR InfoFrame
//                    (the LLDV signature; see the 8-bit decision below);
//                    a Dolby VSIF that says LLDV hands off at the first such
//                    picture; one that says TV-led never hands off;
//   take back        a CRC-valid header while handed off (e.g. the source
//                    switched to the tunnel without a capture-state change).
// BT.709 8-bit RGB is never handed off: SDR menus between DV titles stay on
// the tunnel's SDR passthrough, which knows the RGB8 placement.
//
// 8-bit LLDV decision. A capture card reports neither the Dolby VSIF
// (DeckLink) nor anything else that separates 8-bit LLDV from 8-bit SDR sent
// as BT.2020. The signal state that is available: RGB 8-bit, BT.2020 AVI
// colorimetry, no HDR (DRM) InfoFrame, and - proven here - no tunnel. A
// TV-led tunnel carries CRC-protected metadata in every picture and its AVI
// colorimetry is the RGB default, not BT.2020; so on an input where the owner
// enabled the DV tunnel, BT.2020 RGB without a tunnel is player-led Dolby
// Vision (LLDV) or HDR10 without its InfoFrame - PQ either way. VP therefore
// treats it as PQ: newlldv's 10-bit floor is waived for exactly this signal
// (HdrInferenceSignal::dvTunnelHandedOff) and for any signal whose VSIF says
// LLDV (lldvIndicated), the explicit indication winning. Real SDR sent as
// 8-bit BT.2020 remains protected by newlldv's picture-statistics SDR veto.
// With the tunnel switch off nothing changes (newlldv keeps its 10-bit floor).
#include <ColorSpace.h>
#include <VideoState.h>
// The GUI defines BLACK as a colour macro (WindowedVideoWindow.h).
#pragma push_macro("BLACK")
#undef BLACK
#include <video_frame_formatter/CR210DolbyVisionTunnelFormatter.h>
#pragma pop_macro("BLACK")

#include <algorithm>
#include <cmath>
#include <cstdint>

enum class DolbyVisionTunnelHandoffEvent { None, HandOff, TakeBack };

struct DolbyVisionTunnelHandoffSignal
{
	ColorSpace colorspace = ColorSpace::UNKNOWN;   // raw capture colorimetry
	bool hdrInfoFrame = false;
	DolbyVisionVsif vsif = DolbyVisionVsif::Unknown;
	double frameRate = 24000.0 / 1001.0;          // the display mode's measured rate
};

class DolbyVisionTunnelHandoffProbe
{
public:
	// No tunnel evidence for this long (in source pictures) before a hand-off.
	// A tunnel carries a CRC-valid header in every picture.
	static constexpr double kHandoffSeconds = 1.0;

	using Raster = CR210DolbyVisionTunnelFormatter::RasterView;
	using Content = CR210DolbyVisionTunnelFormatter::RgbContent;

	void Reset(const DolbyVisionTunnelHandoffSignal& signal)
	{
		m_signal = signal;
		const double rate = signal.frameRate > 0 ? signal.frameRate : 24000.0 / 1001.0;
		m_windowPictures = static_cast<uint64_t>((std::max)(1.0, std::ceil(rate * kHandoffSeconds - 1e-9)));
		m_handedOff = false;
		m_hasCandidate = false;
		m_candidateStart = 0;
		m_sawTenBit = false;
		m_sawRgb8 = false;
		m_lastContent = Content::UNKNOWN;
		m_reason = "no decision yet";
		m_analysed = 0;
	}

	// One captured picture of the signal Reset() described.
	DolbyVisionTunnelHandoffEvent OnFrame(const Raster& raster, uint64_t counter)
	{
		++m_analysed;
		if (m_handedOff)
		{
			// Only a CRC-valid header takes the signal back (content cues are
			// not proof, and the hand-off was decided on content already).
			if (CR210DolbyVisionTunnelFormatter::HasTunnelHeader(raster))
			{
				m_handedOff = false;
				m_hasCandidate = false;
				m_reason = "CRC-valid tunnel header after the hand-off";
				return DolbyVisionTunnelHandoffEvent::TakeBack;
			}
			return DolbyVisionTunnelHandoffEvent::None;
		}
		if (m_signal.vsif == DolbyVisionVsif::TvLed)
		{
			m_reason = "Dolby VSIF says TV-led (tunnel)";
			return DolbyVisionTunnelHandoffEvent::None;
		}
		if (CR210DolbyVisionTunnelFormatter::HasTunnelHeader(raster))
		{
			ClearCandidate("CRC-valid tunnel header");
			return DolbyVisionTunnelHandoffEvent::None;
		}
		if (CR210DolbyVisionTunnelFormatter::LooksLikeTunnelContent(raster))
		{
			ClearCandidate("tunnel-like content (damaged or converted tunnel stays with the tunnel path)");
			return DolbyVisionTunnelHandoffEvent::None;
		}
		const Content content = CR210DolbyVisionTunnelFormatter::ClassifyRgbContent(raster);
		m_lastContent = content;
		if (content == Content::UNKNOWN)
			return DolbyVisionTunnelHandoffEvent::None;   // dark / undecided: no evidence either way
		if (content == Content::TEN_BIT)
			m_sawTenBit = true;
		else
			m_sawRgb8 = true;
		if (!m_hasCandidate || counter < m_candidateStart)
		{
			m_hasCandidate = true;
			m_candidateStart = counter;
		}
		const bool lldvSignature = m_signal.colorspace == ColorSpace::BT_2020 && !m_signal.hdrInfoFrame;
		const bool vsifLldv = m_signal.vsif == DolbyVisionVsif::LowLatency;
		if (!(m_sawTenBit || lldvSignature || vsifLldv))
		{
			m_reason = "8-bit RGB without BT.2020: SDR stays on the tunnel's SDR passthrough";
			return DolbyVisionTunnelHandoffEvent::None;
		}
		const uint64_t span = counter - m_candidateStart + 1;
		if (!vsifLldv && span < m_windowPictures)
		{
			m_reason = "waiting: no tunnel evidence yet";
			return DolbyVisionTunnelHandoffEvent::None;
		}
		m_handedOff = true;
		m_reason = vsifLldv ? "Dolby VSIF says LLDV; no CRC-valid tunnel header" :
			m_sawTenBit ? "10-bit RGB content, no CRC-valid tunnel header (not a tunnel)" :
			"BT.2020 RGB without an HDR InfoFrame and no CRC-valid tunnel header (LLDV)";
		return DolbyVisionTunnelHandoffEvent::HandOff;
	}

	bool HandedOff() const { return m_handedOff; }
	const char* Reason() const { return m_reason; }
	uint64_t WindowPictures() const { return m_windowPictures; }
	bool SawTenBit() const { return m_sawTenBit; }
	Content LastContent() const { return m_lastContent; }
	uint64_t Analysed() const { return m_analysed; }

private:
	void ClearCandidate(const char* reason)
	{
		m_hasCandidate = false;
		m_sawTenBit = false;
		m_sawRgb8 = false;
		m_reason = reason;
	}

	DolbyVisionTunnelHandoffSignal m_signal;
	uint64_t m_windowPictures = 24;
	bool m_handedOff = false;
	bool m_hasCandidate = false;
	uint64_t m_candidateStart = 0;
	bool m_sawTenBit = false;
	bool m_sawRgb8 = false;
	Content m_lastContent = Content::UNKNOWN;
	const char* m_reason = "no decision yet";
	uint64_t m_analysed = 0;
};

inline const char* ToString(CR210DolbyVisionTunnelFormatter::RgbContent content)
{
	switch (content)
	{
	case CR210DolbyVisionTunnelFormatter::RgbContent::HIGH8: return "RGB8 high8";
	case CR210DolbyVisionTunnelFormatter::RgbContent::LOW8: return "RGB8 low8 (or dark 10-bit)";
	case CR210DolbyVisionTunnelFormatter::RgbContent::TEN_BIT: return "10-bit RGB";
	default: return "undecided";
	}
}
