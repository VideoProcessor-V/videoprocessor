// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GuideLaneJob.h"

#include <algorithm>

namespace CameraLink
{
	void GuideLaneJob::OnFitDone(uint64_t fitSeq)
	{
		if (!m_sched.LaneWanted())
			return;   // every switch off: nothing is wanted, nothing is stored
		const uint64_t pair = fitSeq + m_sched.SnapshotLag();
		std::lock_guard<std::mutex> lock(m_mutex);
		if (std::find(m_wanted.begin(), m_wanted.end(), pair) != m_wanted.end())
			return;
		if (m_wanted.size() >= m_cap)
		{
			m_wanted.erase(m_wanted.begin());   // the oldest: its P4 never came; the render computes it live
			++m_stats.dropped;
		}
		m_wanted.push_back(pair);
		++m_stats.wanted;
	}

	size_t GuideLaneJob::Pump()
	{
		std::unique_lock<std::mutex> pumping(m_pumpMutex, std::try_to_lock);
		if (!pumping.owns_lock())
			return 0;
		std::vector<uint64_t> todo;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			todo = m_wanted;
		}
		std::vector<uint64_t> keep;
		uint64_t published = 0, discarded = 0, retried = 0;
		for (size_t i = 0; i < todo.size(); ++i)
		{
			switch (m_sched.TryPublishLane(todo[i]))
			{
			case GuidanceScheduler::LaneOutcome::Published:
				++published;
				break;
			case GuidanceScheduler::LaneOutcome::NotReady:
				keep.push_back(todo[i]);
				++retried;
				break;
			case GuidanceScheduler::LaneOutcome::Discarded:
				++discarded;
				break;
			default:   // Off
				break;
			}
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		// Entries added while this call ran stay; entries judged here leave unless they were not ready.
		std::vector<uint64_t> next;
		for (size_t i = 0; i < m_wanted.size(); ++i)
			if (std::find(todo.begin(), todo.end(), m_wanted[i]) == todo.end())
				next.push_back(m_wanted[i]);
		std::vector<uint64_t> merged;
		merged.reserve(keep.size() + next.size());
		merged.insert(merged.end(), keep.begin(), keep.end());
		merged.insert(merged.end(), next.begin(), next.end());
		m_wanted.swap(merged);
		m_stats.published += published;
		m_stats.discarded += discarded;
		m_stats.retried += retried;
		return static_cast<size_t>(published);
	}

	void GuideLaneJob::Reset()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_wanted.clear();
	}

	size_t GuideLaneJob::Pending() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_wanted.size();
	}

	GuideLaneJob::Stats GuideLaneJob::GetStats() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_stats;
	}
}
