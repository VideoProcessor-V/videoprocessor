#pragma once

#include "DebugLog.h"
#include "GeneralRate.h"

// Fixed-constant batch B7 (2026-10-18, sweep #25): a log line whenever an unmeasured-rate fallback
// starts being used. The fallbacks stay (a value is needed until the rate is measured); the log
// says so, once per use (FallbackNotice), with the site and the value assumed. Kept apart from
// GeneralRate.h so the pure policy headers do not pull in the logger.
namespace GeneralRate
{
	// `measured`: whether the real rate was available this time. Returns `measured`.
	inline bool NoteFallback(FallbackNotice& notice, bool measured, const char* site, double fallbackHz)
	{
		if (notice.Use(measured))
			DebugLog::Log("Rate fallback: %s has no measured rate; assuming %.3f Hz until it is measured (use %llu)",
				site, fallbackHz, static_cast<unsigned long long>(notice.Uses()));
		return measured;
	}
}
