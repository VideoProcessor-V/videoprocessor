// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// input.source_analysis/regions: the MEASURE DOMAIN of the edge / grain thresholds (CB slice 18 S2a, coreplan-CB-18-edgemap.md
// S2a: a pure move out of scaling/output/AdaptiveDownscale.h/.cpp, unchanged). The kEdgeMap stage variant (S2b) computes its luma
// in this domain, so the law lives with the product's owner; X5 keeps its names through using-declarations
// (AdaptiveDownscale.h) and keeps GuardCode / GuardSlope (its residual guard). The GLSL twin (ad_lin / ad_luma) is
// MeasureDomainGlsl.h. Header-only, C++14.
#include <VP-V/core/LumaWeights.h>

#include <libplacebo/colorspace.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace VpMeasure
{
	// Post-merge audit 2026-10-17 (general, not fixed): the residual grain guard's
	// code -> linear-light conversion follows the source's own transfer, as
	// pl_shader_linearize decodes it, instead of an assumed 2.2. mode 0: power,
	// a (c + b)^g (gamma 1.8-2.8 and unknown scaled by the nominal peak; BT.1886 with
	// its black lift; sRGB and the log curves as gamma 2.2, their nearest power);
	// 1: PQ (1.0 = SDR white, 203 nits); 2: HLG - a = nominal peak, b = black lift,
	// g = system gamma, the OOTF applied on luma.
	struct GuardTransferParams
	{
		float mode = 0.0f;
		float a = 1.0f;
		float b = 0.0f;
		float g = 2.2f;
	};
	inline GuardTransferParams GuardTransfer(const pl_color_space& source)
	{
		GuardTransferParams t;
		float cspMin = 0.0f, cspMax = 1.0f;
		pl_nominal_luma_params luma{};
		luma.color = &source;
		luma.metadata = PL_HDR_METADATA_HDR10;
		luma.scaling = PL_HDR_NORM;
		luma.out_min = &cspMin;
		luma.out_max = &cspMax;
		pl_color_space_nominal_luma_ex(&luma);
		if (!(cspMax > 0.0f)) cspMax = 1.0f;
		switch (source.transfer)
		{
		case PL_COLOR_TRC_LINEAR:
			t.g = 1.0f;
			break;
		case PL_COLOR_TRC_BT_1886:
		{
			const float lb = std::pow((std::max)(cspMin, 0.0f), 1.0f / 2.4f);
			const float lw = std::pow(cspMax, 1.0f / 2.4f);
			t.a = std::pow((std::max)(lw - lb, 1e-6f), 2.4f);
			t.b = lb / (std::max)(lw - lb, 1e-6f);
			t.g = 2.4f;
			break;
		}
		case PL_COLOR_TRC_GAMMA18: t.g = 1.8f; t.a = cspMax - cspMin; break;
		case PL_COLOR_TRC_GAMMA20: t.g = 2.0f; t.a = cspMax - cspMin; break;
		case PL_COLOR_TRC_GAMMA24: t.g = 2.4f; t.a = cspMax - cspMin; break;
		case PL_COLOR_TRC_GAMMA26: t.g = 2.6f; t.a = cspMax - cspMin; break;
		case PL_COLOR_TRC_GAMMA28: t.g = 2.8f; t.a = cspMax - cspMin; break;
		case PL_COLOR_TRC_PQ:
			t.mode = 1.0f;
			break;
		case PL_COLOR_TRC_HLG:
		{
			const float hlgRef = 1000.0f / PL_COLOR_SDR_WHITE;
			t.mode = 2.0f;
			t.a = cspMax;
			t.g = 1.2f * std::pow(1.111f, std::log2(cspMax / hlgRef));
			t.b = std::sqrt(3.0f * std::pow((std::max)(cspMin, 0.0f) / cspMax, 1.0f / t.g));
			break;
		}
		default:   // unknown, gamma 2.2, sRGB, the log curves: gamma 2.2
			t.g = 2.2f;
			t.a = (std::max)(cspMax - cspMin, 1e-6f);
			break;
		}
		if (!(t.a > 0.0f)) t.a = 1.0f;
		return t;
	}
	// CPU mirrors of the shader's rs_lin / rs_code / rs_slope (tests).
	inline double GuardLinear(const GuardTransferParams& t, double c)
	{
		c = c > 0.0 ? c : 0.0;
		if (t.mode < 0.5)
			return t.a * std::pow(c + t.b, static_cast<double>(t.g));
		if (t.mode < 1.5)
		{
			const double e = std::pow(c, 1.0 / 78.84375);
			const double n = (std::max)(e - 0.8359375, 0.0) / (18.8515625 - 18.6875 * e);
			return std::pow(n, 1.0 / 0.1593017578125) * (10000.0 / 203.0);
		}
		const double x = (1.0 - t.b) * c + t.b;
		const double s = (x <= 0.5 ? 4.0 * x * x : std::exp((x - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
		return t.a * std::pow((std::max)(s, 0.0), static_cast<double>(t.g));
	}

	// Bug hunt 33 F3 / F4 (2026-10-31): what the passes' "luma" is.
	//  - Weights: the source's own Kr, Kg, Kb (DetailPulse::LumaWeights: its YCbCr system, else
	//    BT.2020 for BT.2020 primaries, else BT.709). The passes run before colour management,
	//    in source-primaries RGB; BT.709 weights on a BT.2020 picture turned chroma-only branch
	//    differences into a neutral shift. BT.709 sources keep the literal weights (same shader).
	//  - Measure domain: the edge / grain thresholds were tuned in 8-bit codes of a 2.2-encoded
	//    picture. A linear PREKERNEL is re-encoded with 1/2.2 (mode 1, as before); a PQ or HLG
	//    picture is decoded to relative linear light (1.0 = SDR white, the residual guard's
	//    GuardLinear) and then re-encoded the same way (mode 2), so HDR gets the SDR thresholds'
	//    strengths instead of acting on PQ codes; SDR gamma-encoded pictures are read as they are
	//    (mode 0, as before).
	struct MeasureDomain
	{
		float w[3] = { 0.2126f, 0.7152f, 0.0722f };
		bool bt709 = true;            // the weights are BT.709's (the literal shader text)
		float mode = 0.0f;            // 0 codes as they are, 1 linear -> 2.2, 2 HDR decode -> 2.2
		GuardTransferParams transfer; // mode 2: the hooked picture's transfer
	};
	// The weights come from core/LumaWeights.h (LumaCoeff::ForSystem, the table DetailPulse::LumaWeights forwards to).
	inline MeasureDomain MeasureDomainFor(const pl_color_space& hooked, enum pl_color_system system,
		enum pl_color_primaries primaries)
	{
		MeasureDomain d;
		double k[3];
		LumaCoeff::ForSystem(system, primaries, k);
		for (int i = 0; i < 3; ++i) d.w[i] = static_cast<float>(k[i]);
		d.bt709 = k[0] == 0.2126 && k[2] == 0.0722;
		if (hooked.transfer == PL_COLOR_TRC_LINEAR)
			d.mode = 1.0f;
		else if (hooked.transfer == PL_COLOR_TRC_PQ || hooked.transfer == PL_COLOR_TRC_HLG)
		{
			d.mode = 2.0f;
			d.transfer = GuardTransfer(hooked);
		}
		return d;
	}
	// "vec3(Kr, Kg, Kb)" for the shaders (BT.709 exactly as the old literal).
	inline std::string LumaWeightsGlsl(const MeasureDomain& d)
	{
		if (d.bt709)
			return "vec3(0.2126, 0.7152, 0.0722)";
		char text[96];
		snprintf(text, sizeof(text), "vec3(%.9g, %.9g, %.9g)", d.w[0], d.w[1], d.w[2]);
		return text;
	}
	// CPU mirror of the measure pass's ad_luma: 8-bit reference codes of one pixel.
	inline double MeasureLumaCode(const MeasureDomain& d, const double rgb[3])
	{
		double c[3] = { rgb[0], rgb[1], rgb[2] };
		if (d.mode > 1.5)
			for (double& v : c) v = GuardLinear(d.transfer, v);
		double y = d.w[0] * c[0] + d.w[1] * c[1] + d.w[2] * c[2];
		if (d.mode > 0.5)
			y = (y < 0.0 ? -1.0 : 1.0) * std::pow(std::fabs(y), 1.0 / 2.2);
		return y * 255.0;
	}
}
