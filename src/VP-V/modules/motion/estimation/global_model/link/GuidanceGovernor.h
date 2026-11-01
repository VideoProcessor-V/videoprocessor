// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-4): the guidance governor (pure). A guided matcher whose narrow searches keep failing verification costs
// MORE than the full search, and a skipped cut that was not a cut loses a whole pair. So:
//   trip     escalations > 10 % of the planned narrow blocks of a pair, OR a pair whose search was skipped (cut / flash) and
//            whose skip was not confirmed afterwards;
//   action   narrow and E1 skip OFF (the model global and the candidate stay) for `cooldownSeconds` of MEASURED pair time
//            (default 1 s: seconds x the measured rate, never a frame count), then recover automatically;
//   status   "camera guidance: escalations high, full search".
// Every transition is visible: trips and the remaining cool-down are counted for the status line. Deterministic.
#pragma once

#include <VP-V/modules/motion/estimation/global_model/GlobalModelMeasurement.h>

#include <cstdint>
#include <string>

namespace CameraLink
{
	struct GovernorConfig
	{
		double escalationTrip = 0.10;     // escalated / planned narrow blocks
		double cooldownSeconds = 1.0;
		uint32_t minPlannedBlocks = 16;   // below this a ratio is noise: no trip from escalation alone
	};

	// What one finished guided pair tells the governor.
	struct GuidedPairResult
	{
		bool guided = false;               // the pair ran with an active plan
		uint32_t plannedNarrow = 0;        // blocks planned narrow (both directions summed as planned*directions)
		uint32_t escalated = 0;
		bool matcherSkipped = false;       // the search was skipped (cut / flash)
		bool skipUnconfirmed = false;      // ... and the cut / flash was NOT confirmed (the fit saw no cut)
		double pairSeconds = 0;            // measured duration of the pair (cool-down clock); <= 0 counts as 0
	};

	class GuidanceGovernor
	{
	public:
		explicit GuidanceGovernor(const GovernorConfig& c = GovernorConfig()) : m_cfg(c) {}

		// Feed one finished pair. Returns true when this pair tripped.
		bool OnPair(const GuidedPairResult& r)
		{
			bool tripped = false;
			if (m_cooldown > 0)
			{
				m_cooldown -= r.pairSeconds > 0 ? r.pairSeconds : 0;
				if (m_cooldown <= 0)
				{
					m_cooldown = 0;
					++m_recoveries;
				}
				return false;   // the pair ran without narrow / E1: nothing to judge
			}
			if (r.guided && r.matcherSkipped && r.skipUnconfirmed)
				tripped = true;
			else if (r.guided && r.plannedNarrow >= m_cfg.minPlannedBlocks &&
				static_cast<double>(r.escalated) > m_cfg.escalationTrip * static_cast<double>(r.plannedNarrow))
				tripped = true;
			if (tripped)
			{
				m_cooldown = m_cfg.cooldownSeconds;
				++m_trips;
			}
			return tripped;
		}

		bool Tripped() const { return m_cooldown > 0; }
		double CooldownRemaining() const { return m_cooldown; }
		uint32_t Trips() const { return m_trips; }
		uint32_t Recoveries() const { return m_recoveries; }
		void Reset() { m_cooldown = 0; }   // a discontinuity does not reset the trip count (diagnostics)

		// The switches the matcher may use now: while tripped narrow and E1 skip are off; global / candidate / cut skip stay.
		CameraModel::MeasurementSwitches Apply(CameraModel::MeasurementSwitches sw) const
		{
			if (m_cooldown > 0)
			{
				sw.narrowWindow = false;
				sw.e1Skip = false;
			}
			return sw;
		}

		std::string StatusLine() const
		{
			return m_cooldown > 0 ? "camera guidance: escalations high, full search" : std::string();
		}

	private:
		GovernorConfig m_cfg;
		double m_cooldown = 0;
		uint32_t m_trips = 0, m_recoveries = 0;
	};
}
