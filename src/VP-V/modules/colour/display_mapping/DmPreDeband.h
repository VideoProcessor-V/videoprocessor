// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// colour.dm_pre (CF-DV P5): post-decode I-only Dolby Vision debander decision,
// D5 12-bit tap law helpers (DvTunnelGlsl.h), and dynamic shader parameter builder
// for shaders/vp_dm_pre_deband.glsl. Declaration-only header (zero inline bodies);
// implementations live in DmPreDeband.cpp.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace DmPreDeband
{
	enum class Mode
	{
		Off,
		Auto
	};

	Mode ParseMode(const std::string& mode);
	uint16_t TapCode12FromIntensity16(uint16_t intensity16);
	uint16_t Tap16FromCode12(uint16_t code12);

	struct Inputs
	{
		bool dolbyVisionPicture = false;
		bool slotRuns = false;
		bool originalPathway = false;
		std::string mode = "off";
		bool tapAvailable = false;
		bool qualityAvailable = false;
		double effectiveBits = 12.0;
		double bitrateQuality = 1.0;
		bool snapLevels = true;
		bool grainGuard = true;
		bool hasGrainProduct = false;
		double grainStrength = 0.0;
	};

	struct Decision
	{
		bool active = false;
		bool fallbackEngaged = false;
		double thresholdCodes = 0.0;
		double thresholdNorm = 0.0;
		double radius = 16.0;
		bool grainGuardOn = true;
		std::string reason;
		std::string status;
	};

	struct ActiveRect { double x0 = 0.0, y0 = 0.0, x1 = 1.0, y1 = 1.0; };
	ActiveRect NormalizedActiveRect(double cropX0, double cropY0, double cropX1, double cropY1, int frameWidth, int frameHeight);
	ActiveRect IntersectNormalizedActiveRect(double cropX0, double cropY0, double cropX1, double cropY1, bool hasTrusted, int trustLeft, int trustTop, int trustRight, int trustBottom, int trustW, int trustH, int frameWidth, int frameHeight);
	Decision Decide(const Inputs& in);
	std::vector<std::pair<const char*, double>> ShaderParameters(const Decision& d, int frameHeight, bool ictcpInput = false, const ActiveRect& activeRect = ActiveRect{});
}
