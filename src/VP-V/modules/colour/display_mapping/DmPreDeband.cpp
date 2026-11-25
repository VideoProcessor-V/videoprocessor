// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// colour.dm_pre (CF-DV P5): post-decode I-only Dolby Vision debander policy and
// shader parameter derivation. Never performs a private decode: requires the shared
// D5 12-bit intensity tap and P14 coding quality, engaging a visible fallback when
// either is absent.
#include <VP-V/modules/colour/display_mapping/DmPreDeband.h>
#include <VP-V/modules/repair/deband/VpDebander.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace DmPreDeband
{
	namespace
	{
		// Derived from the 12-bit Dolby Vision intensity depth (HDMI 1.4a §6.5.4 / DvTunnelGlsl.h / DmCurve::kCodeBits).
		constexpr int kDvCodeBits = 12;
		constexpr uint16_t kCode12Mask = static_cast<uint16_t>((1u << kDvCodeBits) - 1u);   // 4095 (0x0FFF)
		constexpr double kCode12Max = static_cast<double>(kCode12Mask);                      // 4095.0
		// 12-bit code sits in the high bits of the 16-bit container: scale = 2^(16 - 12) = 16 (DvTunnelGlsl.h).
		constexpr unsigned kTapHighAlignScale = 1u << (16u - static_cast<unsigned>(kDvCodeBits));

		// Sample ring radius law shared with VpDebander (VpDebander.h: 16 px at 1080 lines, scaled by height).
		constexpr double kBaseRadiusPx = 16.0;
		constexpr double kReferenceHeightLines = 1080.0;
		// Clamped to [1/4, 4] of the 1080p base radius (270..4320 lines -> [4.0, 64.0] px).
		constexpr double kMinRadiusPx = kBaseRadiusPx / 4.0;
		constexpr double kMaxRadiusPx = kBaseRadiusPx * 4.0;

		std::string NormalizeToken(const std::string& raw)
		{
			size_t begin = 0;
			size_t end = raw.size();
			while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin])))
				++begin;
			while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])))
				--end;
			std::string out;
			out.reserve(end - begin);
			for (size_t i = begin; i < end; ++i)
				out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw[i]))));
			return out;
		}
	}

	Mode ParseMode(const std::string& mode)
	{
		return NormalizeToken(mode) == "auto" ? Mode::Auto : Mode::Off;
	}

	uint16_t TapCode12FromIntensity16(uint16_t intensity16)
	{
		return static_cast<uint16_t>(static_cast<unsigned>(intensity16) / kTapHighAlignScale);
	}

	uint16_t Tap16FromCode12(uint16_t code12)
	{
		return static_cast<uint16_t>((static_cast<unsigned>(code12) & static_cast<unsigned>(kCode12Mask)) * kTapHighAlignScale);
	}

	Decision Decide(const Inputs& in)
	{
		Decision d;
		d.radius = kBaseRadiusPx;
		d.grainGuardOn = in.grainGuard;

		if (in.originalPathway)
		{
			d.reason = "Original pathway";
			d.status = "DV deband: off (Original pathway)";
			return d;
		}
		if (!in.slotRuns)
		{
			d.reason = "slot off";
			d.status = "DV deband: off (slot off)";
			return d;
		}
		if (!in.dolbyVisionPicture)
		{
			d.reason = "not a DV picture";
			d.status = "DV deband: off (not a DV picture)";
			return d;
		}
		if (ParseMode(in.mode) == Mode::Off)
		{
			d.reason = "setting off";
			d.status = "DV deband: off (setting off)";
			return d;
		}
		if (!in.tapAvailable)
		{
			d.fallbackEngaged = true;
			d.reason = "D5 12-bit tap absent (no private decode)";
			d.status = "DV deband: off (D5 12-bit tap absent; no private decode)";
			return d;
		}
		if (!in.qualityAvailable || !std::isfinite(in.effectiveBits) || !std::isfinite(in.bitrateQuality))
		{
			d.fallbackEngaged = true;
			d.reason = "P14 coding quality absent";
			d.status = "DV deband: off (P14 coding quality absent)";
			return d;
		}

		const double thresholdCodes = VpDebander::Strength(in.effectiveBits, kDvCodeBits, in.bitrateQuality, true, in.snapLevels);
		if (!(thresholdCodes > 0.0) || !std::isfinite(thresholdCodes))
		{
			d.reason = "clean 12-bit picture (threshold 0)";
			d.status = "DV deband: off (clean 12-bit picture)";
			return d;
		}

		d.active = true;
		d.fallbackEngaged = false;
		d.thresholdCodes = thresholdCodes;
		d.thresholdNorm = thresholdCodes / kCode12Max;
		d.grainGuardOn = in.grainGuard;
		d.reason = "active";

		char buf[160];
		std::snprintf(buf, sizeof(buf), "DV deband: active (I-only, threshold %.1f/4095, grain guard %s)",
			d.thresholdCodes, d.grainGuardOn ? "on" : "off");
		d.status = buf;
		return d;
	}

	std::vector<std::pair<const char*, double>> ShaderParameters(const Decision& d, int frameHeight, bool ictcpInput)
	{
		const double height = static_cast<double>((std::max)(1, frameHeight));
		const double scaledRadius = kBaseRadiusPx * height / kReferenceHeightLines;
		const double radius = (std::max)(kMinRadiusPx, (std::min)(kMaxRadiusPx, scaledRadius));
		return {
			{ "dm_deband_active", d.active ? 1.0 : 0.0 },
			{ "dm_deband_thr", d.active ? d.thresholdNorm : 0.0 },
			{ "dm_deband_radius", radius },
			{ "dm_deband_grain_guard", d.grainGuardOn ? 1.0 : 0.0 },
			{ "dm_deband_ictcp_input", ictcpInput ? 1.0 : 0.0 },
		};
	}
}
