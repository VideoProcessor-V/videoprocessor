#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// WP-14 (integration batch 1, M2 step 14): the renderer host's glue around the Dolby Vision tunnel
// GPU unpack (dv_tunnel_gpu_unpack), moved out of LibplaceboVideoRenderer.cpp into VP-V. The
// session owns the unpack object (one per renderer device), its once-per-session log line and the
// rate-limited de-judder failure count; the host keeps the early-upload slot handling and the
// session-wide "failed" flag (read across threads by the frame preparation). Behaviour-free move.
//
// Host use (render thread), docs/design/m2-dv-callsites.md:
//   if (failed) return false;                                    // host's atomic
//   GpuDolbyVisionTunnelUnpack& unpack = session.Unpacker(log, gpu);
//   ... the early texture, unpack.Unpack(...), retry with the frame itself ...
//   return session.Finish(unpacked, width, height, highAligned, sessionFallback, failed, engage);
//   device teardown: session.Reset() (before the device goes).
#include <VP-V/modules/input/signal/GpuDolbyVisionTunnelUnpack.h>

#include <DebugLog.h>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class DvGpuUnpackSession
{
public:
	// The unpack object, created on first use for this device.
	GpuDolbyVisionTunnelUnpack& Unpacker(pl_log log, pl_gpu gpu)
	{
		if (!m_unpack)
			m_unpack = std::make_unique<GpuDolbyVisionTunnelUnpack>(log, gpu);
		return *m_unpack;
	}

	// After the unpack attempt(s): logs the first success once; a failed de-judder frame (no session
	// fallback) is counted and logged at most every 5 s; a failed render frame switches the session
	// to the CPU unpack (failed = true) and engages the fallback entry once. Returns unpacked.
	bool Finish(bool unpacked, int width, int height, bool highAligned, bool sessionFallback,
		std::atomic<bool>& failed, const std::function<void(const char*, const std::string&)>& engage)
	{
		if (unpacked)
		{
			if (!m_logged)
			{
				DebugLog::Log("Dolby Vision RGB tunnel: picture unpacked on the GPU to 12-bit 4:2:2 (%dx%d, RGB8 %s; the CPU reads only the DM packet)",
					width, height, highAligned ? "high8" : "low8");
				m_logged = true;
			}
			return true;
		}
		if (!sessionFallback)
		{
			// At most one line per 5 s, with the count.
			++m_dejudderFailures;
			const ULONGLONG now = GetTickCount64();
			if (m_dejudderFailureLogTick == 0 || now - m_dejudderFailureLogTick >= 5000)
			{
				DebugLog::Log("Dolby Vision RGB tunnel: a de-judder frame's GPU unpack failed (%s); that frame skipped (%llu so far)",
					m_unpack->Error().c_str(), static_cast<unsigned long long>(m_dejudderFailures));
				m_dejudderFailureLogTick = now;
			}
			return false;
		}
		if (!failed.exchange(true))
		{
			DebugLog::Log("Dolby Vision RGB tunnel: the GPU unpack failed (%s); the CPU unpack for the rest of the session",
				m_unpack->Error().c_str());
			engage("Dolby Vision GPU unpack", "CPU unpack for the session: " + m_unpack->Error());
		}
		return false;
	}

	// Device teardown: the unpack's textures and dispatch go before the device; a new device logs
	// its first GPU unpack again. (The host clears its failed flag and the fallback entry.)
	void Reset()
	{
		m_unpack.reset();
		m_logged = false;
	}

private:
	std::unique_ptr<GpuDolbyVisionTunnelUnpack> m_unpack;
	bool m_logged = false;
	uint64_t m_dejudderFailures = 0;      // de-judder frames whose GPU unpack failed
	ULONGLONG m_dejudderFailureLogTick = 0;
};
