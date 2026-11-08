// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-5): the PassTimers rows of the guided matcher, reported through the skeleton's pass-timer path
// (PassTimerHook::AddGpuGroups -> PassTimers::Accumulator, rows keyed by name), not a camera-private clock.
//   motion.estimate      every GPU pass of one MotionWarp::Estimate (sum)
//   motion.guided_pass   the camera guided passes: narrow search, E1 check, the 16 B counts pass
//   motion.block_search  the block searches and the second sweep (E1)
//   motion.d1            the global motion (D1: the search, the engine's global, or the camera model's, and the pick / vector steps)
// The host's call site takes MotionWarp::TakePassGpuMs() once after Estimate (completed timers only: never waited for), feeds the
// raw per-description groups as before and adds these rows from the SAME map (GUIDANCE-TIMERS call site in the cookbook).
// Classification is by the pass DESCRIPTION text (the key libplacebo and DejudderWarp use); GuidanceWiringTests pins the real
// descriptions, so a renamed pass fails a test instead of silently dropping out of a row.
// The governor itself trips on escalation COUNTS (GuidanceGovernor), not on milliseconds; a measured-cost feed from the skeleton
// into a consumer-side governor does not exist yet (MV7 item, see the K4 hand-back). Pure, header-only, C++14.
#pragma once

#include "MotionBenchWriter.h"

#include <map>
#include <string>

namespace CameraLink
{
	enum class MotionRow { EstimateOnly, GuidedPass, BlockSearch, D1 };

	// Which of the three specific rows a pass description feeds (EstimateOnly: only motion.estimate).
	inline MotionRow MotionRowOf(const std::string& description)
	{
		if (description.find("camera guided") != std::string::npos || description.find("camera guidance counts") != std::string::npos)
			return MotionRow::GuidedPass;
		if (description.find("global motion") != std::string::npos)
			return MotionRow::D1;
		if (description.find("block search") != std::string::npos || description.find("second sweep") != std::string::npos)
			return MotionRow::BlockSearch;
		return MotionRow::EstimateOnly;
	}

	// The rows of one Estimate from its completed pass timers (description -> ms). Rows with no time are left out.
	inline std::map<std::string, double> MotionTimerRows(const std::map<std::string, double>& passMs)
	{
		double total = 0, guided = 0, search = 0, d1 = 0;
		for (std::map<std::string, double>::const_iterator it = passMs.begin(); it != passMs.end(); ++it)
		{
			if (!(it->second > 0))
				continue;
			total += it->second;
			switch (MotionRowOf(it->first))
			{
			case MotionRow::GuidedPass: guided += it->second; break;
			case MotionRow::D1: d1 += it->second; break;
			case MotionRow::BlockSearch: search += it->second; break;
			default: break;
			}
		}
		std::map<std::string, double> rows;
		if (total > 0)
			rows[kTimerEstimate] = total;
		if (guided > 0)
			rows[kTimerGuidedPass] = guided;
		if (search > 0)
			rows[kTimerBlockSearch] = search;
		if (d1 > 0)
			rows[kTimerD1] = d1;
		return rows;
	}

	// The estimate row for the bench's gpu_ms column (-1 = not measured).
	inline double EstimateGpuMs(const std::map<std::string, double>& rows)
	{
		std::map<std::string, double>::const_iterator it = rows.find(kTimerEstimate);
		return it == rows.end() ? -1.0 : it->second;
	}
}
