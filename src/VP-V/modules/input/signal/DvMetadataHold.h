#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The bounded hold of a rejected Dolby Vision DM (feat/dv-l8, 2026-10-24; owner rule: no
// silent fallbacks). Header-only and pure (no clock, no Windows; C++14).
//
// Before: a picture whose DM was rejected (CRC-valid header, damaged / malformed body) kept
// the previous picture's DM, silently, for the whole loss limit (~1 s), dynamic values
// included - across a scene cut that is the previous shot's L1 and trims.
//
// Now, per run of rejected pictures (counted in SOURCE pictures since the first rejection,
// drops included, like the formatter's loss limit - a time, never a fixed frame count):
//   phase Held   - up to HoldPictures() (kHoldSeconds at the measured rate): the previous
//                  DM as it was (a lost packet or two is invisible);
//   phase Static - after the hold: a SHOWN picture gets static/base metadata only (the
//                  retained DM stays whole for black pictures and no-metadata packets). The decode matrices, the
//                  source range, L5/L6/L9/L10/L11 stay (they describe the title, not the
//                  shot); L1 scene values and the L2/L8 trims are dropped, so tone mapping
//                  falls back to the static source range and the trims to neutral;
//   the static phase applies to TRANSPORT damage only (CRC / length / continuation failures,
//   truncation: the body that arrived is the title's, damaged on the way) and is capped at
//   StaticCapPictures() (the formatter passes its SDR-exit window, 2 s); a CRC-valid packet
//   whose body is UNSUPPORTED / MALFORMED (e.g. a same-mode title change without EOS to a DM
//   VP cannot decode) is undecodable once the hold expires: the DM is cleared, black until
//   fresh DM (audit 2026-10-24: never the old title's matrices on the new title). Pictures
//   without a valid header clear it at the formatter's loss limit (~1 s), as before.
// Every rejected picture is counted (RejectedPictures, shown with the DV status); the
// renderer lists "DV metadata" under "Fallbacks engaged" while Held or Static lasts (the
// GUI's one-time notice comes with the first engagement).
#include <video_frame_formatter/IVideoFrameFormatter.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace DvMetadataHold
{
	enum class Phase : uint8_t { Own = 0, Held = 1, Static = 2 };

	// The hold: long enough to ride over a lost packet or a short burst, short enough that a
	// held L1 rarely crosses a cut. A time; the picture count follows the measured rate.
	constexpr double kHoldSeconds = 0.25;

	inline const char* PhaseName(Phase p)
	{
		switch (p)
		{
		case Phase::Held: return "held";
		case Phase::Static: return "static";
		default: return "own";
		}
	}

	// Drops the dynamic (per-shot) part of a DM, keeping what describes the title.
	inline void StripToStatic(DolbyVisionTunnelFrameMetadata& md)
	{
		md.hasSceneMetadata = false;
		md.sceneMinPq = md.sceneMaxPq = md.sceneAvgPq = 0;
		md.trimCount = 0;
		md.trimOverflow = false;
		for (auto& t : md.trims)
			t = DolbyVisionTunnelFrameMetadata::Trim();
	}

	class Tracker
	{
	public:
		// The caller's measured rate (the formatter's, after its own logged GeneralRate
		// fallback); staticSeconds caps the static phase (the formatter's SDR-exit window).
		// An invalid rate changes nothing (no fixed rate here).
		void SetRate(double hz, double staticSeconds)
		{
			if (!(std::isfinite(hz) && hz > 0.0))
				return;
			m_holdPictures = static_cast<uint32_t>(std::floor(hz * kHoldSeconds + 0.5));
			if (m_holdPictures < 1)
				m_holdPictures = 1;
			const double cap = std::ceil(hz * (staticSeconds > kHoldSeconds ? staticSeconds : kHoldSeconds) - 1e-9);
			m_staticCapPictures = cap > m_holdPictures ? static_cast<uint32_t>(cap) : m_holdPictures + 1;
		}
		uint32_t HoldPictures() const { return m_holdPictures; }
		uint32_t StaticCapPictures() const { return m_staticCapPictures; }
		// Source pictures since this run of rejections began (0 = not in a run).
		uint64_t RunSpan() const { return m_inRun ? m_lastSpan : 0; }

		void Reset()
		{
			m_inRun = false;
			m_runStart = 0;
			m_phase = Phase::Own;
			m_rejected = 0;
			m_runs = 0;
		}

		// One analysed picture: accepted = its own DM was accepted (or a healthy no-metadata
		// packet kept the active DM); haveMetadata = a DM is in force. counter = the source
		// picture counter (drops leave gaps; a counter going backwards restarts the run).
		// Returns the phase for this picture. On the first Static picture of a run the
		// caller strips its retained DM (StripToStatic).
		Phase OnPicture(bool accepted, bool haveMetadata, uint64_t counter)
		{
			m_enteredStatic = false;
			if (accepted)
			{
				m_inRun = false;
				m_phase = Phase::Own;
				return m_phase;
			}
			if (m_rejected != UINT32_MAX)
				++m_rejected;
			if (!m_inRun || counter < m_runStart)
			{
				if (!m_inRun)
					++m_runs;
				m_inRun = true;
				m_runStart = counter;
			}
			if (!haveMetadata)
			{
				m_phase = Phase::Own;   // nothing held: the formatter shows black
				return m_phase;
			}
			const uint64_t span = counter - m_runStart + 1;
			m_lastSpan = span;
			const Phase previous = m_phase;
			m_phase = span <= m_holdPictures ? Phase::Held : Phase::Static;
			m_enteredStatic = m_phase == Phase::Static && previous != Phase::Static;
			return m_phase;
		}

		Phase CurrentPhase() const { return m_phase; }
		bool EnteredStatic() const { return m_enteredStatic; }   // this picture began the static phase
		uint32_t RejectedPictures() const { return m_rejected; }
		uint32_t Runs() const { return m_runs; }

	private:
		uint32_t m_holdPictures = 1;          // until SetRate: the shortest hold
		uint32_t m_staticCapPictures = 2;
		uint64_t m_lastSpan = 0;
		bool m_inRun = false;
		uint64_t m_runStart = 0;
		Phase m_phase = Phase::Own;
		bool m_enteredStatic = false;
		uint32_t m_rejected = 0;
		uint32_t m_runs = 0;
	};

	// The "Fallbacks engaged" entry for a picture's DM: false when none (its own DM).
	inline bool FallbackReason(const DolbyVisionTunnelFrameMetadata& md, std::string& reason)
	{
		if (!md.valid || !md.reusedPreviousDm)
			return false;
		if (md.dmHoldPhase == static_cast<uint8_t>(Phase::Static))
			reason = "packets rejected beyond the hold: static metadata (no scene L1, no trims)";
		else
			reason = "packet rejected: the previous picture's DM held";
		return true;
	}

	// The DV status suffix: "; DV metadata: 3 rejected pictures (held)" or "" when clean.
	inline std::string StatusSuffix(const DolbyVisionTunnelFrameMetadata& md)
	{
		if (md.dmRejectedPictures == 0)
			return std::string();
		char text[96];
		std::snprintf(text, sizeof(text), "; DV metadata: %u rejected picture%s%s", md.dmRejectedPictures,
			md.dmRejectedPictures == 1 ? "" : "s",
			!md.reusedPreviousDm ? "" : md.dmHoldPhase == static_cast<uint8_t>(Phase::Static) ? " (static now)" : " (held now)");
		return text;
	}
}
