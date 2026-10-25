#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The static HDR metadata VP supplies for an LLDV (or newlldv-inferred) signal
// (moved out of CVideoProcessorDlg::BuildPushVideoState unchanged, 2026-10-17,
// so the incoming-HDR-mode tests check the GUI's own resolution).
// Precedence per field: command line (when given) > the active [lldv.*]
// profile > VP's mode-specific default (RendererProfileConfig::
// DefaultLldvMetadata). MaxCLL / MaxFALL / mastering minimum accept 0; the
// mastering maximum must be positive.
#include <RendererProfileConfig.h>

struct LldvCommandLineMetadata
{
	double maxCll = -1.0, maxFall = -1.0;
	double masteringMinLuminance = -1.0, masteringMaxLuminance = -1.0;
};

inline double ResolveLldvValue(double commandLineValue, double profileValue, double fallback,
	bool strictlyPositive)
{
	const bool hasCommandLineValue = strictlyPositive ? commandLineValue > 0.0 : commandLineValue >= 0.0;
	if (hasCommandLineValue)
		return commandLineValue;
	const bool hasProfileValue = strictlyPositive ? profileValue > 0.0 : profileValue >= 0.0;
	return hasProfileValue ? profileValue : fallback;
}

// profile: the active snapshot's lldv (may be null).
inline RendererProfileConfig::LldvMetadata ResolveLldvStaticMetadata(
	const RendererProfileConfig::ResolvedLldv* profile, bool useNewLldvHeuristic,
	const LldvCommandLineMetadata& commandLine)
{
	const RendererProfileConfig::LldvMetadata defaults =
		RendererProfileConfig::DefaultLldvMetadata(useNewLldvHeuristic);
	const double profileMaxCll = profile && profile->hasMaxCll ? profile->maxCll : -1.0;
	const double profileMaxFall = profile && profile->hasMaxFall ? profile->maxFall : -1.0;
	const double profileMin = profile && profile->hasMasteringMinLuminance ? profile->masteringMinLuminance : -1.0;
	const double profileMax = profile && profile->hasMasteringMaxLuminance ? profile->masteringMaxLuminance : -1.0;
	RendererProfileConfig::LldvMetadata out;
	out.maxCll = ResolveLldvValue(commandLine.maxCll, profileMaxCll, defaults.maxCll, false);
	out.maxFall = ResolveLldvValue(commandLine.maxFall, profileMaxFall, defaults.maxFall, false);
	out.masteringMinLuminance = ResolveLldvValue(commandLine.masteringMinLuminance, profileMin,
		defaults.masteringMinLuminance, false);
	out.masteringMaxLuminance = ResolveLldvValue(commandLine.masteringMaxLuminance, profileMax,
		defaults.masteringMaxLuminance, true);
	return out;
}
