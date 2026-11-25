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

	ActiveRect NormalizedActiveRect(double cropX0, double cropY0, double cropX1, double cropY1, int frameWidth, int frameHeight)
	{
		ActiveRect full;
		if (frameWidth <= 0 || frameHeight <= 0 ||
			!std::isfinite(cropX0) || !std::isfinite(cropY0) || !std::isfinite(cropX1) || !std::isfinite(cropY1))
			return full;
		const double invW = 1.0 / static_cast<double>(frameWidth);
		const double invH = 1.0 / static_cast<double>(frameHeight);
		const double x0 = (std::max)(0.0, (std::min)(1.0, (std::min)(cropX0, cropX1) * invW));
		const double x1 = (std::max)(0.0, (std::min)(1.0, (std::max)(cropX0, cropX1) * invW));
		const double y0 = (std::max)(0.0, (std::min)(1.0, (std::min)(cropY0, cropY1) * invH));
		const double y1 = (std::max)(0.0, (std::min)(1.0, (std::max)(cropY0, cropY1) * invH));
		if (!(x1 > x0) || !(y1 > y0))
			return full;
		return ActiveRect{ x0, y0, x1, y1 };
	}

	ActiveRect IntersectNormalizedActiveRect(double cropX0, double cropY0, double cropX1, double cropY1,
		bool hasTrusted, int trustLeft, int trustTop, int trustRight, int trustBottom, int trustW, int trustH,
		int frameWidth, int frameHeight)
	{
		if (frameWidth <= 0 || frameHeight <= 0)
			return ActiveRect{};
		double ax0 = 0.0, ay0 = 0.0;
		double ax1 = static_cast<double>(frameWidth), ay1 = static_cast<double>(frameHeight);
		if (std::isfinite(cropX0) && std::isfinite(cropY0) && std::isfinite(cropX1) && std::isfinite(cropY1) &&
			cropX1 > cropX0 && cropY1 > cropY0)
		{
			ax0 = (std::max)(ax0, cropX0);
			ay0 = (std::max)(ay0, cropY0);
			ax1 = (std::min)(ax1, cropX1);
			ay1 = (std::min)(ay1, cropY1);
		}
		if (hasTrusted && trustW == frameWidth && trustH == frameHeight &&
			trustRight > trustLeft && trustBottom > trustTop)
		{
			ax0 = (std::max)(ax0, static_cast<double>(trustLeft));
			ay0 = (std::max)(ay0, static_cast<double>(trustTop));
			ax1 = (std::min)(ax1, static_cast<double>(trustRight));
			ay1 = (std::min)(ay1, static_cast<double>(trustBottom));
		}
		if (!(ax1 > ax0) || !(ay1 > ay0))
			return ActiveRect{};
		return NormalizedActiveRect(ax0, ay0, ax1, ay1, frameWidth, frameHeight);
	}

	std::vector<std::pair<const char*, double>> ShaderParameters(const Decision& d, int frameHeight, bool ictcpInput,
		const ActiveRect& activeRect)
	{
		const double height = static_cast<double>((std::max)(1, frameHeight));
		const double scaledRadius = kBaseRadiusPx * height / kReferenceHeightLines;
		const double radius = (std::max)(kMinRadiusPx, (std::min)(kMaxRadiusPx, scaledRadius)); // VPV-FALLBACK not-a-fallback: physical shader radius clamp [kMinRadiusPx, kMaxRadiusPx]
		auto unitClamp = [](double v, double fallback)
		{
			return std::isfinite(v) ? (std::max)(0.0, (std::min)(1.0, v)) : fallback;
		};
		double ax0 = unitClamp(activeRect.x0, 0.0);
		double ay0 = unitClamp(activeRect.y0, 0.0);
		double ax1 = unitClamp(activeRect.x1, 1.0);
		double ay1 = unitClamp(activeRect.y1, 1.0);
		if (!(ax1 > ax0)) { ax0 = 0.0; ax1 = 1.0; }
		if (!(ay1 > ay0)) { ay0 = 0.0; ay1 = 1.0; }
		return {
			{ "dm_deband_active", d.active ? 1.0 : 0.0 },
			{ "dm_deband_thr", d.active ? d.thresholdNorm : 0.0 },
			{ "dm_deband_radius", radius },
			{ "dm_deband_grain_guard", d.grainGuardOn ? 1.0 : 0.0 },
			{ "dm_deband_ictcp_input", ictcpInput ? 1.0 : 0.0 },
			{ "dm_active_x0", ax0 },
			{ "dm_active_y0", ay0 },
			{ "dm_active_x1", ax1 },
			{ "dm_active_y1", ay1 },
		};
	}
}
