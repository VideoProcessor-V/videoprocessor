#pragma once

#include <map>
#include <string>

// Label schema v1 ("vpnn-labels-v1") as the exported shaders' PARAM values.
//
// Why: net A is conditioned on the TRUE encoding identity (owner, vp-neural-bounded): the
// encoded/delivered size per axis, the player kernel class, the codec class, the AV1 film-grain
// flag, the bits-per-pixel tier and 8/10-bit. VP's identification supplies these per frame/scene;
// improving the identification needs no retrain, changing the schema does (the schema version is
// in the fingerprint). The index tables below MUST match the kit (trainkit/models.py CODECS,
// trainkit/common.py PLAYER_KERNELS); NetRunnerTests checks them against the exported PARAM ranges.
// Header-only, C++14.
namespace VpNets
{
	constexpr const char* kLabelSchemaV1 = "vpnn-labels-v1";

	struct LabelsV1
	{
		double ratioY = 1.0;              // encoded / delivered height (1 = native)
		double ratioX = 1.0;              // encoded / delivered width
		std::string kernel = "none";      // player upscale kernel: bicubic, lanczos, spline, bilinear, none
		std::string codec = "unknown";    // AVC, HEVC, AV1, VP9, unknown
		bool filmGrainSynthesis = false;  // AV1 FGS
		double bppTier = 0.0;             // log2(bpp / 0.05) / 4 (trainkit/models.py bpp_tier)
		bool tenBit = false;
	};

	// Kit order: PLAYER_KERNELS = [bicubic, lanczos, spline, bilinear], then "none" (unknown).
	inline int KernelIndexV1(const std::string& k)
	{
		static const char* kKernels[] = { "bicubic", "lanczos", "spline", "bilinear" };
		for (int i = 0; i < 4; ++i) if (k == kKernels[i]) return i;
		return 4;
	}

	// Kit order: CODECS = [AVC, HEVC, AV1, VP9, unknown].
	inline int CodecIndexV1(const std::string& c)
	{
		static const char* kCodecs[] = { "AVC", "HEVC", "AV1", "VP9" };
		for (int i = 0; i < 4; ++i) if (c == kCodecs[i]) return i;
		return 4;
	}

	// The PARAM values an A export reads (vpnn_ry, vpnn_rx, vpnn_kernel, vpnn_codec, vpnn_fgs,
	// vpnn_bpp_tier, vpnn_bits10).
	inline std::map<std::string, double> ParamsFromLabelsV1(const LabelsV1& l)
	{
		return {
			{ "vpnn_ry", l.ratioY },
			{ "vpnn_rx", l.ratioX },
			{ "vpnn_kernel", static_cast<double>(KernelIndexV1(l.kernel)) },
			{ "vpnn_codec", static_cast<double>(CodecIndexV1(l.codec)) },
			{ "vpnn_fgs", l.filmGrainSynthesis ? 1.0 : 0.0 },
			{ "vpnn_bpp_tier", l.bppTier },
			{ "vpnn_bits10", l.tenBit ? 1.0 : 0.0 },
		};
	}
}
