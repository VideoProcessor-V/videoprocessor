// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// repair/grain_split: the KEY and the LAW of the band memo (product P30 kBandMemo, "band_memo"; C8 slice 8, coreplan-C8-8-U2, shared unit U2).
// One low band of a plane = one separable Gaussian, computed ONCE per (frame, BandKey) by the owner fill (BandMemoFill.h) and read by every
// consumer that used to keep a private copy of it (interpolator, debander, DetailPulse, TextureRestore, temporal detail, frame quality).
// Pure and header-only (no GPU, no Windows headers), C++14: the key codec, the taps (the CPU form of the law) and the GLSL text of the
// GPU form are all here, so a test sees the same figures the fill uses. The law itself is VpUnit::BandFromBase (shared/BandFromBase.h):
//   Law::Taps  BandFromBase law 1 (Radius/Weight; factor, minOne, eps are key fields), any sigma;
//   Law::Low   Bill's low-band law (GrainPreserve::LowRadius / LowWeight, called unchanged: Original stays Bill's), the grain band of the
//              interpolator, the debander and DetailPulse's wide band.
// The VariantKey codec (the skeleton's key has sigma x 1000 only, so the sigma's remaining micro-pixels, the law and the edge mode travel in
// the owner-defined param fields; every field of BandKey survives ToVariant -> FromVariant exactly):
//   scale     1 = HALF resolution (the 2x2 box plane, P1), 0 = FULL resolution (the plane as it is)
//   stage     VariantKey::stage unchanged (0 = the source planes; StageAfter(slot) once the slot planes are served: REBASE-ON-CA)
//   plane     VariantKey::plane unchanged (0 = luma)
//   width/height  VariantKey size axis: the plane's dims in samples (needed by the CPU form; both or none)
//   sigmaMilli + param0   sigma px = (sigmaMilli * 1000 + param0) / 1e6 (param0 = 0..999 micro-pixels; 1e-6 px resolution)
//   param1    bit 0 edge (0 Replicate, 1 Reflect), bit 1 law (0 Taps, 1 Low), bit 2 minOne cleared (0 = at least one tap),
//             bits 3..4 factor (0 = 3.0, 1 = 2.0, 2 = 4.0, 3 = 2.5), bit 5 eps (0 = none, 1 = 1e-9)
// A key outside this vocabulary does not decode (FromVariant false) and the fill declines it visibly.
#include <VP-V/core/VariantKey.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace VpUnit
{
namespace BandMemo
{
	// Bump on ANY change of the codec or of the emitted pass text.
	constexpr uint32_t kUnitVersion = 1;

	enum class Stage : uint8_t { Full = 0, Half = 1 };
	enum class Law : uint8_t { Taps = 0, Low = 1 };

	struct BandKey
	{
		Stage resolution = Stage::Full;   // Half = the P1 half plane, Full = the plane as it is
		uint8_t pipelineStage = 0;        // VariantKey::stage (0 = source planes)
		uint8_t plane = 0;                // VariantKey::plane (0 = luma)
		uint32_t width = 0, height = 0;   // VariantKey size axis: the plane's own dims in samples (the CPU form needs them; 0 = the product's own)
		double sigmaPx = 0.0;             // in the pixels of `resolution` for Law::Taps; the FULL-resolution sigma for Law::Low (as GrainPreserve takes it)
		Law law = Law::Taps;
		BandFromBase::EdgeMode edge = BandFromBase::EdgeMode::Replicate;
		double factor = 3.0;              // Law::Taps: radius = ceil(factor * sigma - eps)
		bool minOne = true;
		double eps = 0.0;
	};

	constexpr uint32_t kMicroPerPx = 1000000u;
	constexpr double kFactors[4] = { 3.0, 2.0, 4.0, 2.5 };

	inline int FactorIndex(double f)
	{
		for (int i = 0; i < 4; ++i)
			if (std::fabs(kFactors[i] - f) < 1e-12)
				return i;
		return -1;
	}
	inline bool EpsIsEncodable(double e) { return e == 0.0 || std::fabs(e - 1e-9) < 1e-18; }

	// True when `k` is inside the codec's vocabulary (sigma > 0 and below ~4e6 px, a listed factor, eps 0 or 1e-9; Law::Low carries no
	// factor / minOne / eps of its own: they must be the defaults so one law has one spelling).
	inline bool Encodable(const BandKey& k)
	{
		if (!(k.sigmaPx > 0.0) || k.sigmaPx > 4.0e6)
			return false;
		if (k.law == Law::Low)
			return k.factor == 3.0 && k.minOne && k.eps == 0.0;
		return FactorIndex(k.factor) >= 0 && EpsIsEncodable(k.eps);
	}

	// The skeleton key of `k` (a key that is not Encodable yields the default VariantKey: callers test Encodable first).
	inline VpSkeleton::VariantKey ToVariant(const BandKey& k)
	{
		VpSkeleton::VariantKey v;
		if (!Encodable(k))
			return v;
		const uint64_t micro = static_cast<uint64_t>(std::llround(k.sigmaPx * double(kMicroPerPx)));
		v.scale = k.resolution == Stage::Half ? uint8_t(1) : uint8_t(0);
		v.stage = k.pipelineStage;
		v.plane = k.plane;
		if (k.width != 0 && k.height != 0)
		{
			v.width = k.width;
			v.height = k.height;
		}
		v.sigmaMilli = static_cast<uint32_t>(micro / 1000u);
		v.param0 = static_cast<uint16_t>(micro % 1000u);
		uint16_t p1 = 0;
		if (k.edge == BandFromBase::EdgeMode::Reflect) p1 |= 1u;
		if (k.law == Law::Low) p1 |= 2u;
		if (!k.minOne) p1 |= 4u;
		p1 = static_cast<uint16_t>(p1 | (FactorIndex(k.factor) << 3));
		if (k.eps != 0.0) p1 |= 32u;
		v.param1 = p1;
		return v;
	}

	// The key of a canonical VariantKey (its defaults are the zero fields: sigma 0 is not a band). False = not a band key.
	inline bool FromVariant(const VpSkeleton::VariantKey& v, BandKey* out)
	{
		if (v.sigmaMilli == 0u && v.param0 == 0u)
			return false;
		if (v.scale > 1u || v.precision != 0 || v.codeUnit != 0 || v.HasCrop() || v.param1 >= 64u)
			return false;
		BandKey k;
		k.resolution = v.scale == 1u ? Stage::Half : Stage::Full;
		k.pipelineStage = v.stage;
		k.plane = v.plane;
		k.width = v.width;
		k.height = v.height;
		k.sigmaPx = (double(v.sigmaMilli) * 1000.0 + double(v.param0)) / double(kMicroPerPx);
		k.edge = (v.param1 & 1u) ? BandFromBase::EdgeMode::Reflect : BandFromBase::EdgeMode::Replicate;
		k.law = (v.param1 & 2u) ? Law::Low : Law::Taps;
		k.minOne = (v.param1 & 4u) == 0u;
		k.factor = kFactors[(v.param1 >> 3) & 3u];
		k.eps = (v.param1 & 32u) ? 1e-9 : 0.0;
		if (!Encodable(k))
			return false;
		if (out != nullptr)
			*out = k;
		return true;
	}

	// The taps -r..r of the key's law, as double (the GPU table and the CPU blur both come from this one list).
	inline std::vector<double> TapsOf(const BandKey& k)
	{
		if (k.law == Law::Low)
		{
			const int r = GrainPreserve::LowRadius(k.sigmaPx);
			std::vector<double> t(static_cast<size_t>(2 * r + 1));
			for (int i = -r; i <= r; ++i)
				t[static_cast<size_t>(i + r)] = GrainPreserve::LowWeight(i, k.sigmaPx);
			return t;
		}
		return BandFromBase::Taps<double>(k.sigmaPx, k.factor, k.minOne, k.eps);
	}

	// The CPU form: the band of a w x h plane (rows across, then columns down, taps summed in index order -r..r in the element type T).
	// T = float matches a float32 consumer (and the GPU texture); T = double the oracle (BandFromHalf for Law::Low, Replicate edge).
	template <class T> inline std::vector<T> CpuBand(const std::vector<T>& plane, int w, int h, const BandKey& k)
	{
		const std::vector<double> d = TapsOf(k);
		std::vector<T> taps(d.size());
		for (size_t i = 0; i < d.size(); ++i)
			taps[i] = static_cast<T>(d[i]);
		std::vector<T> out;
		BandFromBase::BlurTaps<T>(plane, out, w, h, taps, k.edge);
		return out;
	}

	// The GLSL of the GPU form: the shared emitter's text (table + vpu_band_from_base_w / _across / _down) for this key's taps.
	inline std::string GlslOf(const BandKey& k) { return BandFromBase::GlslFromTaps(TapsOf(k)); }

	// Pass bodies of the fill: `bm_src` (sampled) -> `bm_out` (storage r32f), one separable axis each. Edge: the shared emitter's clamp
	// (Replicate); a Reflect key is a CPU-only form (the identity-chain tile blur) and the fill declines it visibly.
	inline const char* AcrossBodyGlsl()
	{
		return "ivec2 bm_q = ivec2(gl_GlobalInvocationID.xy);\n"
		       "ivec2 bm_sz = imageSize(bm_out);\n"
		       "if (bm_q.x < bm_sz.x && bm_q.y < bm_sz.y)\n"
		       "    imageStore(bm_out, bm_q, vec4(vpu_band_from_base_across(bm_src, bm_q).x, 0.0, 0.0, 1.0));\n";
	}
	inline const char* DownBodyGlsl()
	{
		return "ivec2 bm_q = ivec2(gl_GlobalInvocationID.xy);\n"
		       "ivec2 bm_sz = imageSize(bm_out);\n"
		       "if (bm_q.x < bm_sz.x && bm_q.y < bm_sz.y)\n"
		       "    imageStore(bm_out, bm_q, vec4(vpu_band_from_base_down(bm_src, bm_q).x, 0.0, 0.0, 1.0));\n";
	}
}
}
