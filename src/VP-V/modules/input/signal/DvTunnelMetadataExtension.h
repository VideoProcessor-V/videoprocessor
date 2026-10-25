#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// WP-14 / audit M-1 (2026-10-25): VP's additions to the DV tunnel formatter's DM handling, moved
// out of Bill's CR210DolbyVisionTunnelFormatter into VP-V behind the upstreamable
// IDolbyVisionMetadataExtension hook (video_frame_formatter/DolbyVisionMetadataExtension.h):
//   - L9 (source primaries) and L10 (custom target displays), so L8 trims can be resolved
//     (DvSignalLevels.h);
//   - the bounded hold of a rejected DM (DvMetadataHold.h): held for 0.25 s at the measured rate,
//     then static metadata for SHOWN pictures (L1 and trims dropped), capped at the SDR-exit
//     window for transport damage; an unsupported body is cleared once the hold expires; every
//     rejected picture after DV lock counted. Only with the formatter's fixes switch
//     (dv_tunnel_fixes) on, as before.
// Behaviour and log lines are the code that was inline in the formatter (feat/dv-l8), moved
// verbatim. Installed by VP where the formatter is created (renderer, DV loopback analyzer):
// docs/design/m2-dv-formatter-hook.patch.
#include <video_frame_formatter/DolbyVisionMetadataExtension.h>
#include <VP-V/modules/input/signal/DvMetadataHold.h>
#include <VP-V/modules/input/signal/DvSignalLevels.h>

#include <DebugLog.h>

class DvTunnelMetadataExtension final : public IDolbyVisionMetadataExtension
{
public:
	void OnSignal(double frameRate, double sdrFallbackSeconds) override
	{
		m_dmHold.SetRate(frameRate, sdrFallbackSeconds);   // feat/dv-l8: times at the measured rate
		m_dmHold.Reset();
	}

	bool ParseExtensionBlock(uint8_t level, const uint8_t* payload, size_t length, DolbyVisionTunnelFrameMetadata& metadata) override
	{
		if (level == 10)   // feat/dv-l8: the targets L8 refers to
		{
			DvSignalLevels::ParseL10(payload, length, metadata);
			return true;
		}
		if (level == 9)
		{
			DvSignalLevels::ParseL9(payload, length, metadata);
			return true;
		}
		return false;
	}

	// feat/dv-l8 (owner 2026-10-24): a picture with a CRC-valid header is still a tunnel picture VP
	// can decode with the retained matrices, so past the loss limit it is shown with static
	// metadata ("Fallbacks engaged: DV metadata" + the counter); only a picture without a valid
	// header (not decodable as the tunnel) ends the DM.
	bool KeepDmPastLossLimit(bool fixesOn, bool supportedHeader, bool haveMetadata) const override
	{
		return fixesOn && supportedHeader && haveMetadata;
	}

	bool OnPicture(const Picture& p) override
	{
		// feat/dv-l8 (no silent fallbacks): a rejected DM is held only briefly, then reduced to
		// its static part; every rejected picture after DV lock is counted. The retained DM itself
		// is kept whole (a later no-metadata packet or a black picture still refers to it); only a
		// SHOWN picture past the hold gets the static copy (Decorate).
		if (p.fixesOn && (p.tunnelConfirmed || p.acceptedMetadata) &&
			m_dmHold.OnPicture(p.acceptedMetadata, p.haveMetadata, p.counter) == DvMetadataHold::Phase::Static &&
			m_dmHold.EnteredStatic() && !p.bodyUnsupported)
			DebugLog::Log("Dolby Vision RGB tunnel: DM rejected for longer than the %u-picture hold; shown pictures use static metadata only (scene L1 and trims dropped) until fresh DM; %u rejected pictures so far",
				m_dmHold.HoldPictures(), m_dmHold.RejectedPictures());
		// feat/dv-l8 (audit 2026-10-24): past the hold, an unsupported / malformed body (not
		// transport damage) is undecodable, and transport damage keeps the static picture only up
		// to the cap.
		if (p.fixesOn && p.tunnelConfirmed && !p.acceptedMetadata && p.haveMetadata &&
			m_dmHold.CurrentPhase() == DvMetadataHold::Phase::Static &&
			(p.bodyUnsupported || m_dmHold.RunSpan() > m_dmHold.StaticCapPictures()))
		{
			DebugLog::Log("Dolby Vision RGB tunnel: %s; DM cleared, black until fresh DM (%u rejected pictures so far)",
				p.bodyUnsupported ? "metadata body unsupported/malformed beyond the hold (e.g. a title change VP cannot decode)" :
				"transport damage beyond the static-metadata cap", m_dmHold.RejectedPictures());
			return true;
		}
		return false;
	}

	void Decorate(DolbyVisionTunnelFrameMetadata& metadata, bool currentPictureHasValidHeader) const override
	{
		metadata.dmHoldPhase = static_cast<uint8_t>(metadata.reusedPreviousDm ? m_dmHold.CurrentPhase() : DvMetadataHold::Phase::Own);
		metadata.dmRejectedPictures = m_dmHold.RejectedPictures();
		if (metadata.dmHoldPhase == static_cast<uint8_t>(DvMetadataHold::Phase::Static) && currentPictureHasValidHeader)
			DvMetadataHold::StripToStatic(metadata);   // feat/dv-l8: past the hold, a shown picture gets the static part
	}

	const DvMetadataHold::Tracker& Hold() const { return m_dmHold; }

private:
	DvMetadataHold::Tracker m_dmHold;          // feat/dv-l8: held, then static, counted
};
