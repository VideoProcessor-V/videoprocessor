#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace DisplayTopologySession
{
	// Restores a durable pre-session topology, if present. A successful no-op
	// returns true with restored=false. The recovery record is removed only
	// after SetDisplayConfig succeeds and the restored targets are active.
	bool RestorePending(const std::string& statePath, const char* reason,
		bool& restored, std::string& error);

	bool HasPendingRecovery(const std::string& statePath);

	// audit 2026-10-16: startup form of RestorePending. When the exact saved
	// topology cannot be replayed (for example a display is gone), retries with
	// SDC_ALLOW_CHANGES, then SDC_TOPOLOGY_EXTEND, and quarantines the record
	// under a timestamped key so later starts are not blocked. Returns true
	// when some topology was applied (or nothing was pending); the caller
	// continues starting either way.
	bool RecoverPendingAtStartup(const std::string& statePath,
		const char* reason, bool& restored, bool& quarantined,
		std::string& error);

	// audit 2026-10-16: canonical display value for a refresh rate. A rate
	// within 0.01 Hz of N*1000/1001 Hz (integer N >= 11, any family:
	// 23.976, 29.97, 47.952, 59.94, 119.88 ...) becomes that exact fraction
	// rounded to 3 decimals; every other rate is returned unchanged. For
	// N >= 11 the fraction is more than 0.01 Hz from N, so integer rates
	// never snap.
	inline double CanonicalRefreshRateHz(double rate)
	{
		if (!(rate > 0.0) || !std::isfinite(rate))
			return rate;
		const double n = std::floor(rate * 1.001 + 0.5);
		if (n < 11.0)
			return rate;
		const double fractional = n * 1000.0 / 1001.0;
		if (std::abs(rate - fractional) >= 0.01)
			return rate;
		return std::floor(fractional * 1000.0 + 0.5) / 1000.0;
	}

	// Bug hunt 2026-10-17: the maximum-mode choice for a target-only session.
	// It ranked refresh before area (so a low-resolution high-refresh mode
	// beat the native resolution) and accepted interlaced modes. Rank by
	// area, then refresh; skip interlaced and sub-32-bit modes. No resolution
	// or rate is preferred by value.
	struct DisplayModeCandidate
	{
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t refreshHz = 0;
		uint32_t bitsPerPixel = 0;
		bool interlaced = false;
	};

	inline bool IsMaximumModeCandidate(const DisplayModeCandidate& mode)
	{
		return mode.bitsPerPixel >= 32 && mode.width != 0 && mode.height != 0 &&
			mode.refreshHz != 0 && !mode.interlaced;
	}

	inline bool DisplayModeRanksAbove(const DisplayModeCandidate& candidate,
		const DisplayModeCandidate& best)
	{
		const uint64_t area = static_cast<uint64_t>(candidate.width) * candidate.height;
		const uint64_t bestArea = static_cast<uint64_t>(best.width) * best.height;
		return area > bestArea ||
			(area == bestArea && candidate.refreshHz > best.refreshHz);
	}

	// Captures and persists the current topology, then atomically replaces it
	// with the one uniquely connected target matching friendlyName. The target
	// may currently be inactive. Existing topology is restored on any failure
	// after persistence.
	bool BeginTargetOnly(const std::wstring& friendlyName,
		const std::string& statePath, std::string& error);
}
