// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Bug hunt 35 #1 (2026-10-31): the vector producer (MotionAhead::Producer) used to stop for the
// whole session after three failed jobs in a row (a transient 4K texture allocation failure on a
// 4 GB card, a reset of its private D3D11 device). Every vector consumer then fell back to the
// render-time matcher until the settings changed. Now the producer restarts itself while it is
// still wanted, with a back-off: the first restart after a short wait, later ones after a long
// one, and the short wait again once a restart has run healthy for the healthy interval.
//
// Pure policy (no clock, no D3D): the producer passes its steady-clock milliseconds. CPU-tested
// in src/VP-V/tests/FallbackCatalogTests.cpp (with the status entry that shows the state).

#include <cstdint>

namespace MotionAhead
{
	class ProducerRecovery
	{
	public:
		// Time constants of a device or driver recovery, not rates.
		static constexpr uint64_t kFirstDelayMs = 5000;    // the first restart after a stop
		static constexpr uint64_t kLaterDelayMs = 60000;   // further restarts while it keeps failing
		static constexpr uint64_t kHealthyMs = 60000;      // a restart that ran this long resets the back-off

		// The producer stopped (three failures in a row) at nowMs: the wait before the restart.
		uint64_t OnStopped(uint64_t nowMs)
		{
			// A restart that ran healthy for the healthy interval forgives the earlier stops.
			if (m_restarted && nowMs - m_restartedMs >= kHealthyMs)
				m_attempt = 0;
			uint64_t delay = kLaterDelayMs;   // (copies: no ODR use of the constants under C++14)
			if (m_attempt == 0) delay = kFirstDelayMs;
			++m_attempt;
			++m_stops;
			m_recovering = true;
			m_retryAtMs = nowMs + delay;
			m_restarted = false;
			return delay;
		}
		// The restart happened (a new device and engine on the next frame).
		void OnRestarted(uint64_t nowMs)
		{
			m_recovering = false;
			m_restarted = true;
			m_restartedMs = nowMs;
			++m_restarts;
		}
		bool Recovering() const { return m_recovering; }
		uint64_t RetryAtMs() const { return m_retryAtMs; }
		uint64_t RemainingMs(uint64_t nowMs) const { return m_recovering && nowMs < m_retryAtMs ? m_retryAtMs - nowMs : 0; }
		unsigned Stops() const { return m_stops; }
		unsigned Restarts() const { return m_restarts; }

	private:
		unsigned m_attempt = 0, m_stops = 0, m_restarts = 0;
		bool m_recovering = false, m_restarted = false;
		uint64_t m_retryAtMs = 0, m_restartedMs = 0;
	};
}
