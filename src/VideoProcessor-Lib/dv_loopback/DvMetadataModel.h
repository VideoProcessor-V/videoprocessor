#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// DV tunnel loopback (2026-10-17): metadata as a flat list of named values, so
// a sent DM and a decoded DM can be compared field by field (loop checks and
// the perturbation tests: change one field, exactly that field must change).
//
// ExpectedDecode() states what VP's decoder must report for a transmitted
// dm_metadata(), from the ETSI GS CCM 001 fields and VP's documented rules
// (docs/VP_DOLBY_VISION_TUNNEL.md): 12-bit opponent colour only (colour space
// 0 or 2); YCCtoRGB / 8192, offsets / 2^28, RGBtoLMS / 16384; L1 must be 6
// bytes with codes <= 4095 (else the whole DM is rejected); L2 trims fill the
// slots first, then L8, at most kMaxTrims; L5 8 bytes (16-bit containers) or 7
// bytes (RPU-packed 13-bit), offsets <= 8191; L6 values > 10000 nits drop the
// block, MaxCLL 0 or MaxFALL > MaxCLL drop the content light; L11 4 bytes. The
// first block of L5 / L6 / L11 wins. Flatten() reads the decoder's struct.
#include "DvTunnelPacker.h"
#include <video_frame_formatter/IVideoFrameFormatter.h>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace DvLoopback
{
	using FlatMetadata = std::map<std::string, double>;

	inline uint16_t Be16(const std::vector<uint8_t>& p, size_t at)
	{
		return static_cast<uint16_t>((p[at] << 8) | p[at + 1]);
	}

	inline std::string Key(const char* base, int a = -1, int b = -1)
	{
		char text[64];
		if (b >= 0) snprintf(text, sizeof(text), "%s[%d][%d]", base, a, b);
		else if (a >= 0) snprintf(text, sizeof(text), "%s[%d]", base, a);
		else snprintf(text, sizeof(text), "%s", base);
		return text;
	}

	inline void AddTrim(FlatMetadata& m, int slot, int level, int index, int targetPq, int slope, int offset,
		int power, int chroma, int saturation, int ms, int hasMid = 0, int mid = 0, int clip = 0,
		int hasSat = 0, int hasHue = 0)
	{
		const std::string k = "trim" + std::to_string(slot) + ".";
		m[k + "level"] = level; m[k + "targetIndex"] = index; m[k + "targetMaxPq"] = targetPq;
		m[k + "slope"] = slope; m[k + "offset"] = offset; m[k + "power"] = power;
		m[k + "chromaWeight"] = chroma; m[k + "saturationGain"] = saturation; m[k + "msWeight"] = ms;
		m[k + "hasMidContrastAndClip"] = hasMid; m[k + "midContrast"] = mid; m[k + "clipTrim"] = clip;
		m[k + "hasSaturationVector"] = hasSat; m[k + "hasHueVector"] = hasHue;
	}

	// What VP must decode from this DM; accepted = false when VP must reject it
	// (the previous metadata then stays in force).
	inline FlatMetadata ExpectedDecode(const DmFields& dm, bool& accepted, size_t maxTrims =
		DolbyVisionTunnelFrameMetadata::kMaxTrims)
	{
		FlatMetadata m;
		accepted = dm.signalBitDepth == 12 && (dm.signalColorSpace == 0 || dm.signalColorSpace == 2);
		if (!accepted)
			return m;
		m["valid"] = 1;
		m["fullRange"] = dm.signalFullRange != 0 ? 1 : 0;
		m["sourceMinPq"] = dm.sourceMinPq;
		m["sourceMaxPq"] = dm.sourceMaxPq;
		for (int r = 0; r < 3; ++r)
		{
			for (int c = 0; c < 3; ++c)
			{
				m[Key("nonlinear", r, c)] = static_cast<float>(dm.yccToRgb[r * 3 + c]) / 8192.0f;
				m[Key("linear", r, c)] = static_cast<float>(dm.rgbToLms[r * 3 + c]) / 16384.0f;
			}
			// VP converts the 32-bit offset to float (24-bit mantissa) first.
			m[Key("nonlinearOffset", r)] = static_cast<float>(dm.yccToRgbOffset[r]) / 268435456.0f;
		}
		m["L1.present"] = 0; m["L1.min"] = 0; m["L1.max"] = 0; m["L1.avg"] = 0;
		m["L5.present"] = 0; m["L5.left"] = 0; m["L5.right"] = 0; m["L5.top"] = 0; m["L5.bottom"] = 0;
		m["L6.present"] = 0; m["L6.maxMastering"] = 0; m["L6.minMastering"] = 0;
		m["L6.contentLight"] = 0; m["L6.maxCll"] = 0; m["L6.maxFall"] = 0;
		m["L11.present"] = 0; m["L11.contentType"] = 0; m["L11.whitepoint"] = 0; m["L11.referenceMode"] = 0;
		bool l5 = false, l6 = false, l11 = false;
		struct T { int level, index, target, s, o, p, cw, sg, ms, hasMid, mid, clip, hasSat, hasHue; };
		std::vector<T> l2, l8;
		bool overflow = false;
		for (const DmExtBlock& b : dm.ext)
		{
			const std::vector<uint8_t>& p = b.payload;
			const size_t n = p.size();
			if (b.level == 1)
			{
				if (n != 6) { accepted = false; return FlatMetadata(); }
				const uint16_t mn = Be16(p, 0), mx = Be16(p, 2), av = Be16(p, 4);
				if (mn > 4095 || mx > 4095 || av > 4095) { accepted = false; return FlatMetadata(); }
				m["L1.present"] = 1; m["L1.min"] = mn; m["L1.max"] = mx; m["L1.avg"] = av;
			}
			else if (b.level == 2 && n >= 14)
			{
				T t{ 2, 0, Be16(p, 0), Be16(p, 2), Be16(p, 4), Be16(p, 6), Be16(p, 8), Be16(p, 10),
					static_cast<int16_t>(Be16(p, 12)), 0, 0, 0, 0, 0 };
				if (l2.size() >= maxTrims) overflow = true; else l2.push_back(t);
			}
			else if (b.level == 8 && n >= 13)
			{
				T t{ 8, p[0], 0, Be16(p, 1), Be16(p, 3), Be16(p, 5), Be16(p, 7), Be16(p, 9),
					static_cast<int16_t>(Be16(p, 11)), 0, 0, 0, 0, 0 };
				size_t at = 13;
				if (n >= at + 4) { t.hasMid = 1; t.mid = Be16(p, at); t.clip = Be16(p, at + 2); at += 4; }
				if (n >= at + 6) { t.hasSat = 1; at += 6; }
				if (n >= at + 6) { t.hasHue = 1; }
				if (l8.size() >= maxTrims) overflow = true; else l8.push_back(t);
			}
			else if (b.level == 5 && !l5)
			{
				uint16_t v[4] = {};
				bool ok = true;
				if (n >= 8) for (int k = 0; k < 4; ++k) v[k] = Be16(p, k * 2);
				else if (n == 7)
				{
					uint64_t bits = 0;
					for (int k = 0; k < 7; ++k) bits = (bits << 8) | p[k];
					bits >>= 4;
					for (int k = 0; k < 4; ++k) v[k] = static_cast<uint16_t>((bits >> (13 * (3 - k))) & 0x1FFF);
				}
				else ok = false;
				for (uint16_t x : v) if (x > 0x1FFF) ok = false;
				if (ok)
				{
					l5 = true;
					m["L5.present"] = 1; m["L5.left"] = v[0]; m["L5.right"] = v[1]; m["L5.top"] = v[2]; m["L5.bottom"] = v[3];
				}
			}
			else if (b.level == 6 && !l6 && n >= 8)
			{
				const uint16_t mx = Be16(p, 0), mn = Be16(p, 2), cll = Be16(p, 4), fall = Be16(p, 6);
				if (mx <= 10000 && cll <= 10000 && fall <= 10000)
				{
					l6 = true;
					const bool light = cll > 0 && fall <= cll;
					m["L6.present"] = 1; m["L6.maxMastering"] = mx; m["L6.minMastering"] = mn;
					m["L6.contentLight"] = light ? 1 : 0; m["L6.maxCll"] = light ? cll : 0; m["L6.maxFall"] = light ? fall : 0;
				}
			}
			else if (b.level == 11 && !l11 && n >= 4)
			{
				l11 = true;
				m["L11.present"] = 1; m["L11.contentType"] = p[0] & 0x0F; m["L11.whitepoint"] = p[1] & 0x0F;
				m["L11.referenceMode"] = (p[1] & 0x10) ? 1 : 0;
			}
		}
		size_t slot = 0;
		for (const std::vector<T>* list : { &l2, &l8 })
			for (const T& t : *list)
			{
				if (slot >= maxTrims) { overflow = true; break; }
				AddTrim(m, static_cast<int>(slot++), t.level, t.index, t.target, t.s, t.o, t.p, t.cw, t.sg, t.ms,
					t.hasMid, t.mid, t.clip, t.hasSat, t.hasHue);
			}
		m["trimCount"] = static_cast<double>(slot);
		m["trimOverflow"] = overflow ? 1 : 0;
		return m;
	}

	inline FlatMetadata Flatten(const DolbyVisionTunnelFrameMetadata& md)
	{
		FlatMetadata m;
		if (!md.valid)
			return m;
		m["valid"] = 1;
		m["fullRange"] = md.fullRange ? 1 : 0;
		m["sourceMinPq"] = md.sourceMinPq;
		m["sourceMaxPq"] = md.sourceMaxPq;
		for (int r = 0; r < 3; ++r)
		{
			for (int c = 0; c < 3; ++c)
			{
				m[Key("nonlinear", r, c)] = md.nonlinear[r][c];
				m[Key("linear", r, c)] = md.linear[r][c];
			}
			m[Key("nonlinearOffset", r)] = md.nonlinearOffset[r];
		}
		m["L1.present"] = md.hasSceneMetadata ? 1 : 0;
		m["L1.min"] = md.hasSceneMetadata ? md.sceneMinPq : 0;
		m["L1.max"] = md.hasSceneMetadata ? md.sceneMaxPq : 0;
		m["L1.avg"] = md.hasSceneMetadata ? md.sceneAvgPq : 0;
		m["L5.present"] = md.hasActiveArea ? 1 : 0;
		m["L5.left"] = md.activeAreaLeft; m["L5.right"] = md.activeAreaRight;
		m["L5.top"] = md.activeAreaTop; m["L5.bottom"] = md.activeAreaBottom;
		m["L6.present"] = md.hasL6 ? 1 : 0;
		m["L6.maxMastering"] = md.l6MaxDisplayMasteringLuminance;
		m["L6.minMastering"] = md.l6MinDisplayMasteringLuminance;
		m["L6.contentLight"] = md.hasContentLight ? 1 : 0;
		m["L6.maxCll"] = md.maxCll; m["L6.maxFall"] = md.maxFall;
		m["L11.present"] = md.hasL11 ? 1 : 0;
		m["L11.contentType"] = md.hasL11 ? md.l11ContentType : 0;
		m["L11.whitepoint"] = md.hasL11 ? md.l11Whitepoint : 0;
		m["L11.referenceMode"] = md.hasL11 && md.l11ReferenceMode ? 1 : 0;
		for (int s = 0; s < md.trimCount; ++s)
		{
			const auto& t = md.trims[s];
			AddTrim(m, s, t.level, t.targetDisplayIndex, t.targetMaxPq, t.slope, t.offset, t.power,
				t.chromaWeight, t.saturationGain, t.msWeight, t.hasMidContrastAndClip ? 1 : 0,
				t.hasMidContrastAndClip ? t.midContrast : 0, t.hasMidContrastAndClip ? t.clipTrim : 0,
				t.hasSaturationVector ? 1 : 0, t.hasHueVector ? 1 : 0);
		}
		m["trimCount"] = md.trimCount;
		m["trimOverflow"] = md.trimOverflow ? 1 : 0;
		return m;
	}

	// "name: want=x got=y" for every differing or one-sided key.
	inline std::vector<std::string> DiffMetadata(const FlatMetadata& want, const FlatMetadata& got)
	{
		std::vector<std::string> out;
		char line[160];
		for (const auto& kv : want)
		{
			const auto it = got.find(kv.first);
			if (it == got.end())
			{
				snprintf(line, sizeof(line), "%s: want=%.9g got=(absent)", kv.first.c_str(), kv.second);
				out.push_back(line);
			}
			else if (it->second != kv.second)
			{
				snprintf(line, sizeof(line), "%s: want=%.9g got=%.9g", kv.first.c_str(), kv.second, it->second);
				out.push_back(line);
			}
		}
		for (const auto& kv : got)
			if (!want.count(kv.first))
			{
				snprintf(line, sizeof(line), "%s: want=(absent) got=%.9g", kv.first.c_str(), kv.second);
				out.push_back(line);
			}
		return out;
	}
}
