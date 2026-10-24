#pragma once
/*
 * Copyright(C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// Dolby Vision DM levels L9 and L10 (feat/dv-l8, 2026-10-24): what an L8 trim's
// target_display_index refers to. Header-only and pure (C++14): the tunnel formatter
// calls it for each extension block; the tests call it directly.
//
// Field order and widths follow libdovi's public parser (quietvoid/dovi_tool,
// dolby_vision/src/rpu/extension/level9.rs, level10.rs):
//   L9:  source_primary_index (u8) [+ 8 explicit source primaries (u16) when length 17]
//   L10: target_display_index (u8), target_max_pq (u12), target_min_pq (u12),
//        target_primary_index (u8) [+ 8 explicit target primaries (u16) when length 21]
// The tunnel's DM carries L1/L2 in big-endian 16-bit containers (the formatter's convention),
// so L10 is read from that form first (6 or 22 bytes: u8, u16, u16, u8 [+ 16 bytes]) and,
// failing that, from the RPU's packed form (5 or 21 bytes: 8 + 12 + 12 + 8 bits [+ 16 bytes]).
// Like the formatter's other levels, a short or out-of-range block is skipped and never
// rejects the DM; the container layout is an assumption until a live packet is checked
// (the formatter logs the raw L2/L8 bytes once per title; the same applies here).
#include <video_frame_formatter/IVideoFrameFormatter.h>

#include <cstddef>
#include <cstdint>

namespace DvSignalLevels
{
	inline uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

	// Decodes one L10 block into `out` (false: skipped).
	inline bool DecodeL10(const uint8_t* payload, size_t length, DolbyVisionTunnelFrameMetadata::TargetDisplay& out)
	{
		DolbyVisionTunnelFrameMetadata::TargetDisplay t;
		if (length == 6 || length == 22)
		{
			t.index = payload[0];
			t.maxPq = Be16(payload + 1);
			t.minPq = Be16(payload + 3);
			t.primaryIndex = payload[5];
		}
		else if (length == 5 || length == 21)
		{
			// 40 bits, MSB first: index 8, max 12, min 12, primary 8.
			uint64_t bits = 0;
			for (size_t i = 0; i < 5; ++i)
				bits = (bits << 8) | payload[i];
			t.index = static_cast<uint8_t>((bits >> 32) & 0xFFU);
			t.maxPq = static_cast<uint16_t>((bits >> 20) & 0xFFFU);
			t.minPq = static_cast<uint16_t>((bits >> 8) & 0xFFFU);
			t.primaryIndex = static_cast<uint8_t>(bits & 0xFFU);
		}
		else
			return false;
		if (t.maxPq > 4095 || t.minPq > 4095 || t.maxPq == 0 || t.minPq >= t.maxPq)
			return false;
		out = t;
		return true;
	}

	// Adds an L10 block to the DM (the first block per target index wins; at most
	// kMaxTargetDisplays). False when skipped.
	inline bool ParseL10(const uint8_t* payload, size_t length, DolbyVisionTunnelFrameMetadata& md)
	{
		DolbyVisionTunnelFrameMetadata::TargetDisplay t;
		if (!DecodeL10(payload, length, t))
			return false;
		for (uint8_t i = 0; i < md.targetDisplayCount; ++i)
			if (md.targetDisplays[i].index == t.index)
				return false;
		if (md.targetDisplayCount >= DolbyVisionTunnelFrameMetadata::kMaxTargetDisplays)
			return false;
		md.targetDisplays[md.targetDisplayCount++] = t;
		return true;
	}

	// L9: the source primary index (the first block wins). Explicit primaries are not kept.
	inline bool ParseL9(const uint8_t* payload, size_t length, DolbyVisionTunnelFrameMetadata& md)
	{
		if (md.hasL9 || (length != 1 && length != 17))
			return false;
		md.hasL9 = true;
		md.l9SourcePrimaryIndex = payload[0];
		return true;
	}

	// The L10 target with this index, or null.
	inline const DolbyVisionTunnelFrameMetadata::TargetDisplay* FindTarget(const DolbyVisionTunnelFrameMetadata& md, uint8_t index)
	{
		for (uint8_t i = 0; i < md.targetDisplayCount && i < DolbyVisionTunnelFrameMetadata::kMaxTargetDisplays; ++i)
			if (md.targetDisplays[i].index == index)
				return &md.targetDisplays[i];
		return nullptr;
	}
}
