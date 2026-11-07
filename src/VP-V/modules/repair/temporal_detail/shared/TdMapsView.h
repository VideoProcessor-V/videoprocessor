// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.temporal_detail / shared: U19 TdMapsView v1, the ONE read API of the td.maps product (doc 72, section 1.2 card 10 and
// section 2). td.maps holds, per 16 px block, 3 x RGBA16F = 12 named fields; the seven per-block map reservations of the first
// reset-1 list (trail confidence, residual, validity, real detail, grain level, stability, lighting, I-to-I drift) are fields of
// this one product, and a consumer reads only the fields it needs, through this accessor, with the unit-version check:
//
//   plane 0 (RGBA16F): F0 validity, F1 trail confidence band 0, F2 trail confidence band 1, F3 residual energy
//   plane 1 (RGBA16F): F4 real-detail confidence, F5 stability, F6 own-grain sigma, F7 coding-noise sigma
//   plane 2 (RGBA16F): F8 lighting gain, F9 lighting offset, F10 I-to-I drift (px, last GOP), F11 active-band mask + reason (packed)
//
// Storage layout of the texture (the GPU form): width = blocks across, height = 3 x blocks down; plane k sits at row offset
// k x blocks-down. The CPU twin below is the specification; the GLSL accessors are generated from the same tables. Reset 1 has no
// producer: every reader finds td.maps absent and takes its classical form (TdProductReads.h). Header-only, C++14.
#pragma once

#include <cstdint>
#include <string>

namespace TdMapsView
{
	constexpr uint32_t kUnitVersion = 1;
	constexpr int kFields = 12;
	constexpr int kPlanes = 3;
	constexpr int kBytesPerBlock = 24;   // 12 halves (3 x RGBA16F)

	enum Field : int
	{
		kValidity = 0, kTrailConfidence0, kTrailConfidence1, kResidual,
		kRealDetail, kStability, kOwnGrain, kCodingNoise,
		kLightGain, kLightOffset, kDrift, kBandMask
	};

	inline const char* Name(int f)
	{
		static const char* const kNames[kFields] = { "validity", "trail_conf0", "trail_conf1", "residual", "real_detail", "stability",
			"own_grain", "coding_noise", "light_gain", "light_offset", "drift", "band_mask" };
		return f >= 0 && f < kFields ? kNames[f] : "?";
	}
	inline int PlaneOf(int f) { return f / 4; }
	inline int ComponentOf(int f) { return f % 4; }

	// The readers of the product (doc 72 section 3 and section 1.2 card 10).
	enum class Reader : uint8_t { AdaptiveScaler, DitherNeed, SigmaDelta, ChromaStage, Deband, ContrastRecovery, DetailPulseTextureRestore };

	inline unsigned Bit(int f) { return 1u << unsigned(f); }

	// The fields each reader reads (a mask of Bit(Field)).
	inline unsigned FieldsOf(Reader r)
	{
		switch (r)
		{
		case Reader::AdaptiveScaler: return Bit(kRealDetail) | Bit(kStability) | Bit(kCodingNoise);
		case Reader::DitherNeed: return Bit(kOwnGrain) | Bit(kStability);
		case Reader::SigmaDelta: return Bit(kStability);
		case Reader::ChromaStage: return Bit(kValidity) | Bit(kTrailConfidence0);
		case Reader::Deband: return Bit(kValidity) | Bit(kStability);
		case Reader::ContrastRecovery: return Bit(kRealDetail) | Bit(kCodingNoise);
		case Reader::DetailPulseTextureRestore: return Bit(kValidity) | Bit(kRealDetail);
		}
		return 0;
	}

	// The CPU twin of one block and its packing into the three RGBA planes.
	struct Block
	{
		float f[kFields] = {};
	};
	inline void Pack(const Block& b, float out[kPlanes][4])
	{
		for (int i = 0; i < kFields; ++i) out[PlaneOf(i)][ComponentOf(i)] = b.f[i];
	}
	inline Block Unpack(const float in[kPlanes][4])
	{
		Block b;
		for (int i = 0; i < kFields; ++i) b.f[i] = in[PlaneOf(i)][ComponentOf(i)];
		return b;
	}
	inline float Fetch(const Block& b, Field f) { return b.f[f]; }

	// The reader's version check: a reader built against another unit version must take its classical form.
	inline bool VersionMatches(uint32_t producerVersion) { return producerVersion == kUnitVersion; }

	// GLSL accessors `float vpu_td_maps_<name>(ivec2 block)` over a sampled texture `sampler` (rgba16f, width = blocks across,
	// height = 3 x `blocksDown`). `blocksDown` is passed as the define VPU_TD_MAPS_GH by the caller.
	inline std::string Glsl(const char* sampler = "vpu_td_maps")
	{
		static const char* const kComp = "xyzw";
		std::string s;
		for (int i = 0; i < kFields; ++i)
		{
			s += std::string("float vpu_td_maps_") + Name(i) + "(ivec2 b)\n{\n    return texelFetch(" + sampler + ", ivec2(b.x, b.y + " +
				std::to_string(PlaneOf(i)) + " * VPU_TD_MAPS_GH), 0)." + kComp[ComponentOf(i)] + ";\n}\n";
		}
		return s;
	}
}
