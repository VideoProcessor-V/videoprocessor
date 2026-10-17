#pragma once

#include <cmath>
#include <cstddef>
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

	// fixes: [general] capture_display_fixes (CaptureDisplayFixes.h); false = the
	// earlier rules (interlaced accepted; refresh first, then area).
	inline bool IsMaximumModeCandidate(const DisplayModeCandidate& mode, bool fixes = true)
	{
		return mode.bitsPerPixel >= 32 && mode.width != 0 && mode.height != 0 &&
			mode.refreshHz != 0 && (!fixes || !mode.interlaced);
	}

	inline bool DisplayModeRanksAbove(const DisplayModeCandidate& candidate,
		const DisplayModeCandidate& best, bool fixes = true)
	{
		const uint64_t area = static_cast<uint64_t>(candidate.width) * candidate.height;
		const uint64_t bestArea = static_cast<uint64_t>(best.width) * best.height;
		if (!fixes)
			return candidate.refreshHz > best.refreshHz ||
				(candidate.refreshHz == best.refreshHz && area > bestArea);
		return area > bestArea ||
			(area == bestArea && candidate.refreshHz > best.refreshHz);
	}

	// Bug hunt 2 (2026-10-17, M3): ranking by area alone picked DSR/VSR
	// super-resolution modes (5120x2880, 7680x4320 on a 4K projector) and
	// DCI 4096-wide compatibility modes above the panel's native mode: the
	// GPU then renders a supersampled desktop and scales it down. The native
	// size comes from the display (the target's preferred mode, EDID), never
	// from a fixed resolution. With it known, modes larger than native in
	// either dimension are not candidates, the exact native size ranks first,
	// then area, then refresh. nativeWidth/Height 0 = unknown (the area-first
	// ranking). Part of capture_display_fixes (off = the earlier ranking).
	inline bool FitsNativeMode(const DisplayModeCandidate& mode,
		uint32_t nativeWidth, uint32_t nativeHeight, bool fixes = true)
	{
		if (!fixes || nativeWidth == 0 || nativeHeight == 0)
			return true;
		return mode.width <= nativeWidth && mode.height <= nativeHeight;
	}

	inline bool DisplayModeRanksAboveNative(const DisplayModeCandidate& candidate,
		const DisplayModeCandidate& best, uint32_t nativeWidth,
		uint32_t nativeHeight, bool fixes = true)
	{
		if (fixes && nativeWidth != 0 && nativeHeight != 0)
		{
			const bool candidateNative =
				candidate.width == nativeWidth && candidate.height == nativeHeight;
			const bool bestNative =
				best.width == nativeWidth && best.height == nativeHeight;
			if (candidateNative != bestNative)
				return candidateNative;
		}
		return DisplayModeRanksAbove(candidate, best, fixes);
	}

	// The full choice over an enumerated list (also used by the tests).
	// Returns the index of the chosen mode, or -1 when none qualifies. When a
	// native size is known but no mode fits it, falls back to the unrestricted
	// ranking rather than failing.
	template <typename Container>
	int ChooseMaximumMode(const Container& modes, uint32_t nativeWidth,
		uint32_t nativeHeight, bool fixes = true)
	{
		for (int pass = 0; pass < 2; ++pass)
		{
			const bool restrict = pass == 0;
			int bestIndex = -1;
			for (size_t i = 0; i < modes.size(); ++i)
			{
				const DisplayModeCandidate& mode = modes[i];
				if (!IsMaximumModeCandidate(mode, fixes))
					continue;
				if (restrict && !FitsNativeMode(mode, nativeWidth, nativeHeight, fixes))
					continue;
				if (bestIndex < 0 || DisplayModeRanksAboveNative(mode,
					modes[static_cast<size_t>(bestIndex)],
					restrict ? nativeWidth : 0u, restrict ? nativeHeight : 0u, fixes))
					bestIndex = static_cast<int>(i);
			}
			if (bestIndex >= 0 || !fixes || nativeWidth == 0 || nativeHeight == 0)
				return bestIndex;
		}
		return -1;
	}

	// Captures and persists the current topology, then atomically replaces it
	// with the one uniquely connected target matching friendlyName. The target
	// may currently be inactive (bug hunt 2, M2: an inactive target is
	// activated with a paths-only topology and Windows picks its modes; the
	// maximum-mode step then chooses the native mode). Existing topology is
	// restored on any failure after persistence.
	bool BeginTargetOnly(const std::wstring& friendlyName,
		const std::string& statePath, std::string& error);
}
