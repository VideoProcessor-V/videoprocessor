// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-4): the visible state of the guided matcher, through the SKELETON's mechanism (FallbackCatalog Decisions, the
// same path as camera.no_model.*: Ctrl+I, Config status, the one-time notice), never a camera-private channel.
//   camera.guidance.escalations_high   the governor tripped: narrow and E1 skip are off for the cool-down ("camera guidance:
//                                      escalations high, full search"); released when it recovers
//   camera.guidance.refused            the last pairs were refused (slip, no dominant motion, confidence, plan empty); held for
//                                      0.5 s of MEASURED pair time after the last refusal, so it never flickers. "camera(N) not
//                                      ready in time" is per-pair and expected at d = 0: counted per cause in the status line only
//   camera.guidance.unguided           guided pairs were rejected at their on-time seal afterwards (R > 0, expected 0: a defect)
//   camera.guidance.narrow_idle        the narrow window was planned but the engine's field seeds the matcher (NVOF seed mode):
//                                      "engine field used, narrow idle". The seeds are never dropped; the narrow pass just does
//                                      not run. Held like the refusal entry.
// "All switches off" is NOT a fallback (the feature is off): no entry, no line. The per-reason counters stay in
// GuidanceScheduler::StatusLines().
// Pure, header-only, C++14, deterministic. No clock: the hold runs on the pair durations the host measured.
#pragma once

#include "GuidanceGate.h"

#include <VP-V/core/FallbackCatalog.h>

#include <string>
#include <vector>

namespace CameraLink
{
	constexpr const char* kIdGuidanceEscalations = "camera.guidance.escalations_high";
	constexpr const char* kIdGuidanceRefused = "camera.guidance.refused";
	constexpr const char* kIdGuidanceNarrowIdle = "camera.guidance.narrow_idle";
	constexpr const char* kIdGuidanceUnguided = "camera.guidance.unguided";
	constexpr double kGuidanceRefusalHoldSeconds = 0.5;

	// d = 0: where camera(N) can never be on time the cause is structural and named once in the Fallbacks list ("camera_guidance:
	// unguided - <cause>"), not per pair. Null = no structural cause (guidance can be on time). Called only while guidance is wanted.
	inline const char* GuidanceStructuralCause(bool aheadWanted, bool aheadDeterministic, bool liveBuilt, bool demand)
	{
		if (!aheadWanted)
			return aheadDeterministic ? "deterministic run" : "camera_ahead_fit is off";
		return liveBuilt && !demand ? "no on-time target (analysis_lane off)" : nullptr;
	}

	// Engages for kGuidanceRefusalHoldSeconds of measured time after each refusal worth showing.
	class RefusalHold
	{
	public:
		// One pair judged. pairSeconds <= 0 (unknown) lets the hold run out at the first pair that is not refused.
		void Observe(Refusal r, double pairSeconds)
		{
			if (r != Refusal::None && r != Refusal::AllOff && r != Refusal::Governor && r != Refusal::NotOnTime)
			{
				m_remaining = kGuidanceRefusalHoldSeconds;
				m_last = r;
				return;
			}
			m_remaining -= pairSeconds > 0 ? pairSeconds : kGuidanceRefusalHoldSeconds;
			if (m_remaining < 0)
				m_remaining = 0;
		}
		bool Engaged() const { return m_remaining > 0; }
		Refusal Last() const { return m_last; }
		void Reset() { m_remaining = 0; }

	private:
		double m_remaining = 0;
		Refusal m_last = Refusal::None;
	};

	struct GuidanceStatusFacts
	{
		bool switchesOn = false;        // any measurement switch is on (off = the feature is off: no entry)
		bool governorTripped = false;
		bool refusedNow = false;        // RefusalHold::Engaged()
		Refusal lastRefusal = Refusal::None;
		bool narrowIdleNow = false;     // narrow planned, the engine's field used instead (held)
		uint64_t rejectedAtSeal = 0;    // R: guided pairs whose on-time seal rejected camera(N) afterwards
	};

	inline std::vector<FallbackCatalog::Decision> BuildGuidanceDecisions(const GuidanceStatusFacts& f)
	{
		using namespace FallbackCatalog;
		std::vector<Decision> d;
		d.push_back(f.switchesOn && f.governorTripped ?
			Engaged(kIdGuidanceEscalations, "camera guidance: escalations high, full search") : Released(kIdGuidanceEscalations));
		d.push_back(f.switchesOn && f.refusedNow ?
			Engaged(kIdGuidanceRefused, std::string("camera guidance refused: ") + Text(f.lastRefusal) + " (the matcher runs its full search)") :
			Released(kIdGuidanceRefused));
		d.push_back(f.switchesOn && f.narrowIdleNow ?
			Engaged(kIdGuidanceNarrowIdle, "camera guidance: engine field used, narrow idle (the engine's vectors seed the matcher; no seed is dropped)") :
			Released(kIdGuidanceNarrowIdle));
		d.push_back(f.switchesOn && f.rejectedAtSeal > 0 ?
			Engaged(kIdGuidanceUnguided, "camera_guidance: unguided - guided pairs rejected at the seal") : Released(kIdGuidanceUnguided));
		return d;
	}
}
