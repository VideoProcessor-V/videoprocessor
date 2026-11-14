// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P7 slot <-> CameraMotion record codec (L-B4, lead 8 ruling 2026-11-14): the ONE definition of the byte form of the camera
// fit that fits the P7 slot (ProductDecl p7 fixedBytes = 1024). It carries exactly what a consumer of the smoothed camera
// (P9, FixedLagSmoother) reads: stamps, pair, validity, model params, similarity rates, confidence, reason, flags, fade gain and
// the picture rectangle. Per-block arrays and object lists are NOT in the slot (they stay in the camera store); a decoded record
// therefore has empty blocks/objects, exactly like FixedLagSmoother's own copies.
// The P7 publisher (CameraLane / CameraFitWorker, lane L-B6) must use this same codec. Pure, header-only, C++14, no Windows, no GPU.
// Layout: little-endian by construction (memcpy of IEEE doubles on a little-endian host; static_assert below), version byte first.
#pragma once

#include <cstdint>
#include <cstring>
#include <cstddef>
#include <utility>

#include "CameraMotion.h"

namespace CameraModel
{
	namespace P7Slot
	{
		constexpr uint8_t kVersion = 1;
		constexpr size_t kBytes = 1 + 1 + 1 + 1 + 4      // version, valid, type, reason, flags(2) + pad
			+ 8 * 3 + 8 + 8 * 2                          // seq, pairFrom, pairTo, pairSeconds, generations
			+ 8 * 9 + 8 * 4 + 8 + 8 * 2 + 4 * 4;         // h, rates, confidence, fadeGain/Offset, picture
		static_assert(kBytes <= 1024, "the P7 slot holds 1024 bytes");

		namespace detail
		{
			template <typename T> inline void Put(uint8_t*& p, T v) { std::memcpy(p, &v, sizeof(T)); p += sizeof(T); }
			template <typename T> inline T Get(const uint8_t*& p) { T v; std::memcpy(&v, p, sizeof(T)); p += sizeof(T); return v; }
		}

		// Writes kBytes into `out` (capacity >= kBytes). Returns kBytes, or 0 when capacity is short.
		inline size_t Encode(const CameraMotion& m, uint8_t* out, size_t capacity)
		{
			if (!out || capacity < kBytes) return 0;
			uint8_t* p = out;
			detail::Put<uint8_t>(p, kVersion);
			detail::Put<uint8_t>(p, m.valid ? 1 : 0);
			detail::Put<uint8_t>(p, static_cast<uint8_t>(m.params.type));
			detail::Put<uint8_t>(p, static_cast<uint8_t>(m.reason));
			const uint32_t flags = (m.seededFromNext ? 1u : 0u) | (m.bridged ? 2u : 0u) | (m.inFade ? 4u : 0u) | (m.smoothed ? 8u : 0u);
			detail::Put<uint32_t>(p, flags);
			detail::Put<uint64_t>(p, m.seq);
			detail::Put<uint64_t>(p, m.pairFrom);
			detail::Put<uint64_t>(p, m.pairTo);
			detail::Put<double>(p, m.pairSeconds);
			detail::Put<uint64_t>(p, m.resyncGeneration);
			detail::Put<uint64_t>(p, m.rebaseGeneration);
			for (int i = 0; i < 9; ++i) detail::Put<double>(p, m.params.h[i]);
			detail::Put<double>(p, m.rates.logScale);
			detail::Put<double>(p, m.rates.angle);
			detail::Put<double>(p, m.rates.tx);
			detail::Put<double>(p, m.rates.ty);
			detail::Put<double>(p, m.confidence);
			detail::Put<double>(p, m.fadeGain);
			detail::Put<double>(p, m.fadeOffset);
			detail::Put<int32_t>(p, m.picture.left);
			detail::Put<int32_t>(p, m.picture.top);
			detail::Put<int32_t>(p, m.picture.right);
			detail::Put<int32_t>(p, m.picture.bottom);
			return static_cast<size_t>(p - out);
		}

		// Decodes into `m` (blocks/objects empty). False on a short buffer or an unknown version / out-of-range enum.
		inline bool Decode(const uint8_t* in, size_t size, CameraMotion& m)
		{
			if (!in || size < kBytes) return false;
			const uint8_t* p = in;
			if (detail::Get<uint8_t>(p) != kVersion) return false;
			CameraMotion r;
			r.valid = detail::Get<uint8_t>(p) != 0;
			const uint8_t type = detail::Get<uint8_t>(p);
			const uint8_t reason = detail::Get<uint8_t>(p);
			if (type > static_cast<uint8_t>(ModelType::Homography) || reason > static_cast<uint8_t>(Reason::Disabled)) return false;
			r.params.type = static_cast<ModelType>(type);
			r.reason = static_cast<Reason>(reason);
			const uint32_t flags = detail::Get<uint32_t>(p);
			r.seededFromNext = (flags & 1u) != 0; r.bridged = (flags & 2u) != 0; r.inFade = (flags & 4u) != 0; r.smoothed = (flags & 8u) != 0;
			r.seq = detail::Get<uint64_t>(p);
			r.pairFrom = detail::Get<uint64_t>(p);
			r.pairTo = detail::Get<uint64_t>(p);
			r.pairSeconds = detail::Get<double>(p);
			r.resyncGeneration = detail::Get<uint64_t>(p);
			r.rebaseGeneration = detail::Get<uint64_t>(p);
			for (int i = 0; i < 9; ++i) r.params.h[i] = detail::Get<double>(p);
			r.rates.logScale = detail::Get<double>(p);
			r.rates.angle = detail::Get<double>(p);
			r.rates.tx = detail::Get<double>(p);
			r.rates.ty = detail::Get<double>(p);
			r.confidence = detail::Get<double>(p);
			r.fadeGain = detail::Get<double>(p);
			r.fadeOffset = detail::Get<double>(p);
			r.picture.left = detail::Get<int32_t>(p);
			r.picture.top = detail::Get<int32_t>(p);
			r.picture.right = detail::Get<int32_t>(p);
			r.picture.bottom = detail::Get<int32_t>(p);
			m = std::move(r);
			return true;
		}
	}
}
