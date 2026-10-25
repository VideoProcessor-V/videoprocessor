#include <pch.h>
#include "ExtendedRange.h"

#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ExtendedRange
{
	namespace
	{
		// Audit 2026-10-16: the transfers in relative linear light, odd below
		// 0, analytic above 1 (ExtendedRange.h, 1.). kind: TransferKind as a
		// float (0 power, 1 BT.1886, 2 sRGB, 3 identity). Mirrors ToLinear /
		// FromLinear below.
		const char* const TRANSFER_GLSL = R"GLSL(
float er_lin1(float v, float kind, float p)
{
    float a = abs(v);
    float r = a;
    if (kind < 0.5) {
        r = pow(a, p);
    } else if (kind < 1.5) {
        float k = pow(p, 2.4);
        r = (pow(a + p, 2.4) - k) / (pow(1.0 + p, 2.4) - k);
    } else if (kind < 2.5) {
        r = a <= 0.04045 ? a * (1.0 / 12.92) : pow((a + 0.055) * (1.0 / 1.055), 2.4);
    }
    return v < 0.0 ? -r : r;
}
float er_enc1(float l, float kind, float p)
{
    float a = abs(l);
    float r = a;
    if (kind < 0.5) {
        r = pow(a, 1.0 / p);
    } else if (kind < 1.5) {
        float k = pow(p, 2.4);
        r = pow(a * (pow(1.0 + p, 2.4) - k) + k, 1.0 / 2.4) - p;
    } else if (kind < 2.5) {
        r = a <= 0.0031308 ? a * 12.92 : 1.055 * pow(a, 1.0 / 2.4) - 0.055;
    }
    return l < 0.0 ? -r : r;
}
vec3 er_to_linear(vec3 v, float kind, float p)
{
    return vec3(er_lin1(v.r, kind, p), er_lin1(v.g, kind, p), er_lin1(v.b, kind, p));
}
vec3 er_from_linear(vec3 l, float kind, float p)
{
    return vec3(er_enc1(l.r, kind, p), er_enc1(l.g, kind, p), er_enc1(l.b, kind, p));
}
)GLSL";

		// The carry: the source transfer re-encoded to the output's, the
		// primaries converted here, before scaling, where libplacebo converts
		// without the switch (a non-linear shader such as SSIM then sees the
		// same values; converting after scaling differed by up to 40 codes on
		// saturated colours, measured); then either the picture clipped to
		// 0..1 or the remainder value - clipped (ER_REMAINDER), per channel.
		// Audit 2026-10-16: two pure powers keep the original 10b arithmetic;
		// any other pair goes through relative linear light. With
		// er_linear_remainder the remainder is E(v) - E(clipped) in linear
		// light (wide source gamut: the late restore applies the matrix).
		const char* const CARRY_BODY = R"GLSL(
if (er_convert_gamma > 0.5 || er_convert_primaries > 0.5) {
    if (er_src_kind < 0.5 && er_out_kind < 0.5) {
        if (er_convert_gamma > 0.5)
            color.rgb = sign(color.rgb) * pow(abs(color.rgb), vec3(er_src_param / er_out_param));
        if (er_convert_primaries > 0.5) {
            vec3 er_l = sign(color.rgb) * pow(abs(color.rgb), vec3(er_out_param));
            er_l = er_matrix * er_l;
            color.rgb = sign(er_l) * pow(abs(er_l), vec3(1.0 / er_out_param));
        }
    } else {
        vec3 er_l = er_to_linear(color.rgb, er_src_kind, er_src_param);
        if (er_convert_primaries > 0.5)
            er_l = er_matrix * er_l;
        color.rgb = er_from_linear(er_l, er_out_kind, er_out_param);
    }
}
vec3 er_clipped = clamp(color.rgb, 0.0, 1.0);
#ifdef ER_REMAINDER
if (er_linear_remainder > 0.5)
    color = vec4(er_to_linear(color.rgb, er_out_kind, er_out_param) -
        er_to_linear(er_clipped, er_out_kind, er_out_param), 1.0);
else
    color = vec4(color.rgb - er_clipped, 1.0);
#else
color.rgb = er_clipped;
#endif
)GLSL";

		// Efficiency audit 2026-10-24 #7: the carry in ONE pass with two
		// outputs. The clipped picture leaves through the pass's own target
		// (exactly the old clipped pass: same sampling, same body); the
		// remainder is computed from the same sampled colour with the same
		// expressions as the old ER_REMAINDER pass and stored into the
		// remainder texture at this fragment's texel (a storage image in a
		// raster pass: D3D11 OMSetRenderTargetsAndUnorderedAccessViews).
		// Byte-identical by construction; proven by the GPU test in
		// docs/design/efficiency-byte-identical-tests.md (#7).
		const char* const CARRY_ONE_PASS_TAIL = R"GLSL(
{
    vec3 er_rem;
    if (er_linear_remainder > 0.5)
        er_rem = er_to_linear(color.rgb, er_out_kind, er_out_param) -
            er_to_linear(er_clipped, er_out_kind, er_out_param);
    else
        er_rem = color.rgb - er_clipped;
    imageStore(er_remainder_out, ivec2(gl_FragCoord.xy), vec4(er_rem, 1.0));
}
color.rgb = er_clipped;
)GLSL";

		// The restore: the remainder at the output pixel's position in the
		// carry's rectangle, averaged over the pixel's footprint (4x4 bilinear
		// taps, at least one source pixel wide), added back.
		const char* const RESTORE_HEADER = R"GLSL(
vec3 er_remainder_at(vec2 q)
{
    vec2 u = (q - er_out_rect.xy) / er_out_rect.zw;
    vec2 s = er_carry_rect.xy + u * er_carry_rect.zw;
    vec2 fp = max(er_carry_rect.zw / er_out_rect.zw, vec2(1.0));
    vec2 size = vec2(textureSize(er_remainder, 0));
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            sum += texture(er_remainder, (s + ((vec2(i, j) + 0.5) / 4.0 - 0.5) * fp) / size).rgb;
    return sum / 16.0;
}
)GLSL";

		const char* const RESTORE_BODY = R"GLSL(
color.rgb += er_remainder_at(gl_FragCoord.xy);
color.a = 1.0;
)GLSL";

		// Audit 2026-10-16: the late restore after a calibration LUT. v (the
		// SCALED restore's whole value, which libplacebo's LUT clamped) is
		// read back at the same output pixel; the cut part d = v - clip(v) is
		// added through the LUT's edge slope at clip(v) (LutExtrapolate).
		// er_lut_max = size - 1 per axis.
		const char* const LATE_LUT_HEADER = R"GLSL(
vec3 er_lut_at(vec3 i)
{
    return texelFetch(er_lut, ivec3(i + 0.5), 0).rgb;
}
vec3 er_lut_sample(vec3 p)
{
    vec3 g = clamp(p, 0.0, 1.0) * er_lut_max;
    vec3 i0 = min(floor(g), er_lut_max - 1.0);
    vec3 f = g - i0;
    vec3 c00 = mix(er_lut_at(i0), er_lut_at(i0 + vec3(1.0, 0.0, 0.0)), f.x);
    vec3 c10 = mix(er_lut_at(i0 + vec3(0.0, 1.0, 0.0)), er_lut_at(i0 + vec3(1.0, 1.0, 0.0)), f.x);
    vec3 c01 = mix(er_lut_at(i0 + vec3(0.0, 0.0, 1.0)), er_lut_at(i0 + vec3(1.0, 0.0, 1.0)), f.x);
    vec3 c11 = mix(er_lut_at(i0 + vec3(0.0, 1.0, 1.0)), er_lut_at(i0 + vec3(1.0, 1.0, 1.0)), f.x);
    return mix(mix(c00, c10, f.y), mix(c01, c11, f.y), f.z);
}
vec3 er_lut_slope(vec3 c, vec3 axis, float d)
{
    float n = dot(axis, er_lut_max);
    vec3 e = axis / n;
    vec3 s = d > 0.0 ? er_lut_sample(c) - er_lut_sample(c - e)
                     : er_lut_sample(c + e) - er_lut_sample(c);
    return s * n;
}
vec3 er_scaled_at(vec2 q)
{
    vec2 u = (q - er_out_rect.xy) / er_out_rect.zw;
    vec2 s = er_scaled_rect.xy + u * er_scaled_rect.zw;
    ivec2 size = textureSize(er_scaled, 0);
    return texelFetch(er_scaled, clamp(ivec2(floor(s)), ivec2(0), size - 1), 0).rgb;
}
)GLSL";

		const char* const LATE_LUT_BODY = R"GLSL(
vec3 er_v = er_scaled_at(gl_FragCoord.xy);
vec3 er_c = clamp(er_v, 0.0, 1.0);
vec3 er_d = er_v - er_c;
if (er_d.r != 0.0)
    color.rgb += er_d.r * er_lut_slope(er_c, vec3(1.0, 0.0, 0.0), er_d.r);
if (er_d.g != 0.0)
    color.rgb += er_d.g * er_lut_slope(er_c, vec3(0.0, 1.0, 0.0), er_d.g);
if (er_d.b != 0.0)
    color.rgb += er_d.b * er_lut_slope(er_c, vec3(0.0, 0.0, 1.0), er_d.b);
color.a = 1.0;
)GLSL";

		// Audit 2026-10-16: the late restore after libplacebo's gamut
		// mapping: the linear remainder (source primaries) through the matrix,
		// added in the output's relative linear light.
		const char* const LATE_GAMUT_BODY = R"GLSL(
vec3 er_r = er_remainder_at(gl_FragCoord.xy);
if (any(notEqual(er_r, vec3(0.0)))) {
    vec3 er_l = er_to_linear(color.rgb, er_out_kind, er_out_param) + er_matrix * er_r;
    color.rgb = er_from_linear(er_l, er_out_kind, er_out_param);
}
color.a = 1.0;
)GLSL";

		bool ContainsBt709(enum pl_color_primaries primaries)
		{
			return primaries == PL_COLOR_PRIM_BT_709 || primaries == PL_COLOR_PRIM_DISPLAY_P3 ||
				primaries == PL_COLOR_PRIM_BT_2020;
		}

		pl_hook_res Failure()
		{
			pl_hook_res res{};
			res.failed = true;
			return res;
		}

		bool ZeroMatrix(const pl_matrix3x3& m)
		{
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					if (m.m[r][c] != 0.0f)
						return false;
			return true;
		}

		void LutAt(const LutView& lut, int r, int g, int b, float out[3])
		{
			const size_t index = (static_cast<size_t>(b) * lut.size[1] + g) * lut.size[0] + r;
			for (int c = 0; c < 3; ++c)
				out[c] = lut.data[index * 3 + c];
		}

		float TransferKindValue(const Transfer& t) { return static_cast<float>(static_cast<int>(t.kind)); }
	}

	const char* ToString(Reason reason)
	{
		switch (reason)
		{
		case Reason::Active: return "active";
		case Reason::Off: return "off";
		case Reason::HdrSource: return "off for HDR sources (tone mapped to SDR)";
		case Reason::HdrOutput: return "off for an SDR source on an HDR output";
		case Reason::CalibrationLut: return "off with a calibration LUT";
		case Reason::SourceTransfer: return "off: the source transfer is not supported";
		case Reason::OutputTransfer: return "off: the output transfer is not supported";
		case Reason::SourcePrimaries: return "off: SDR source primaries other than BT.709";
		case Reason::OutputPrimaries: return "off: the output primaries do not contain BT.709";
		case Reason::HookFailed: return "off: libplacebo disabled its hook";
		case Reason::HdrMapped: return "off: HDR source tone/gamut mapped to the output";
		case Reason::LutUnsupported: return "off: the calibration LUT is not a plain 3D normalized LUT";
		case Reason::TemporalMix: return "off: libplacebo frame mixing (smooth motion) with a calibration LUT or gamut mapping";
		}
		return "off";
	}

	double PowerGamma(enum pl_color_transfer transfer)
	{
		switch (transfer)
		{
		case PL_COLOR_TRC_GAMMA18: return 1.8;
		case PL_COLOR_TRC_GAMMA20: return 2.0;
		case PL_COLOR_TRC_GAMMA22: return 2.2;
		case PL_COLOR_TRC_GAMMA24: return 2.4;
		case PL_COLOR_TRC_GAMMA26: return 2.6;
		case PL_COLOR_TRC_GAMMA28: return 2.8;
		default: return 0.0;
		}
	}

	// Audit 2026-10-16.
	bool DescribeTransfer(const pl_color_space& colour, bool piecewise, Transfer& out)
	{
		const double gamma = PowerGamma(colour.transfer);
		if (gamma > 0.0)
		{
			out.kind = TransferKind::Power;
			out.param = static_cast<float>(gamma);
			return true;
		}
		if (!piecewise)
			return false;
		if (colour.transfer == PL_COLOR_TRC_SRGB)
		{
			out.kind = TransferKind::Srgb;
			out.param = 0.0f;
			return true;
		}
		if (colour.transfer == PL_COLOR_TRC_BT_1886)
		{
			// libplacebo: L = a (v + b)^2.4 with L(0) = black, L(1) = white,
			// so (b / (1 + b))^2.4 = L(0) / L(1). Read from its own linearize.
			float lo[3] = { 0.0f, 0.0f, 0.0f }, hi[3] = { 1.0f, 1.0f, 1.0f };
			pl_color_linearize(&colour, lo);
			pl_color_linearize(&colour, hi);
			if (!(hi[0] > 0.0f) || !(lo[0] >= 0.0f) || !(lo[0] < hi[0]))
				return false;
			const double q = std::pow(static_cast<double>(lo[0]) / hi[0], 1.0 / 2.4);
			if (!(q < 1.0))
				return false;
			out.kind = TransferKind::Bt1886;
			out.param = static_cast<float>(q / (1.0 - q));
			return true;
		}
		return false;
	}

	double ToLinear(const Transfer& t, double v)
	{
		const double a = std::fabs(v);
		double r = a;
		switch (t.kind)
		{
		case TransferKind::Power:
			r = std::pow(a, static_cast<double>(t.param));
			break;
		case TransferKind::Bt1886:
		{
			const double b = t.param, k = std::pow(b, 2.4);
			r = (std::pow(a + b, 2.4) - k) / (std::pow(1.0 + b, 2.4) - k);
			break;
		}
		case TransferKind::Srgb:
			r = a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4);
			break;
		case TransferKind::Identity:
			break;
		}
		return v < 0.0 ? -r : r;
	}

	double FromLinear(const Transfer& t, double l)
	{
		const double a = std::fabs(l);
		double r = a;
		switch (t.kind)
		{
		case TransferKind::Power:
			r = std::pow(a, 1.0 / t.param);
			break;
		case TransferKind::Bt1886:
		{
			const double b = t.param, k = std::pow(b, 2.4);
			r = std::pow(a * (std::pow(1.0 + b, 2.4) - k) + k, 1.0 / 2.4) - b;
			break;
		}
		case TransferKind::Srgb:
			r = a <= 0.0031308 ? a * 12.92 : 1.055 * std::pow(a, 1.0 / 2.4) - 0.055;
			break;
		case TransferKind::Identity:
			break;
		}
		return l < 0.0 ? -r : r;
	}

	const char* TransferName(const Transfer& t)
	{
		switch (t.kind)
		{
		case TransferKind::Power: return "power";
		case TransferKind::Bt1886: return "BT.1886";
		case TransferKind::Srgb: return "sRGB";
		case TransferKind::Identity: return "signal";
		}
		return "?";
	}

	// Audit 2026-10-16: the LUT carry's math (CPU mirror of LATE_LUT_*).
	bool LutCarryable(const pl_custom_lut* lut)
	{
		if (!lut || !lut->data)
			return false;
		for (int i = 0; i < 3; ++i)
			if (lut->size[i] < 2 || lut->size[i] > 256)
				return false;
		return ZeroMatrix(lut->shaper_in) && ZeroMatrix(lut->shaper_out);
	}

	void LutSample(const LutView& lut, const float p[3], float out[3])
	{
		int i0[3];
		float f[3];
		for (int k = 0; k < 3; ++k)
		{
			const int last = lut.size[k] - 1;
			const float g = (std::min)((std::max)(p[k], 0.0f), 1.0f) * static_cast<float>(last);
			i0[k] = std::min(static_cast<int>(std::floor(g)), last - 1);
			f[k] = g - static_cast<float>(i0[k]);
		}
		float corner[8][3];
		for (int n = 0; n < 8; ++n)
			LutAt(lut, i0[0] + (n & 1), i0[1] + ((n >> 1) & 1), i0[2] + ((n >> 2) & 1), corner[n]);
		for (int c = 0; c < 3; ++c)
		{
			const float x00 = corner[0][c] + (corner[1][c] - corner[0][c]) * f[0];
			const float x10 = corner[2][c] + (corner[3][c] - corner[2][c]) * f[0];
			const float x01 = corner[4][c] + (corner[5][c] - corner[4][c]) * f[0];
			const float x11 = corner[6][c] + (corner[7][c] - corner[6][c]) * f[0];
			const float y0 = x00 + (x10 - x00) * f[1];
			const float y1 = x01 + (x11 - x01) * f[1];
			out[c] = y0 + (y1 - y0) * f[2];
		}
	}

	void LutEdgeSlope(const LutView& lut, const float c[3], int axis, bool upper, float out[3])
	{
		const float n = static_cast<float>(lut.size[axis] - 1);
		float q[3] = { c[0], c[1], c[2] };
		q[axis] += upper ? -1.0f / n : 1.0f / n;
		float a[3], b[3];
		LutSample(lut, c, a);
		LutSample(lut, q, b);
		for (int k = 0; k < 3; ++k)
			out[k] = (upper ? a[k] - b[k] : b[k] - a[k]) * n;
	}

	void LutExtrapolate(const LutView& lut, const float v[3], float out[3])
	{
		float c[3];
		for (int k = 0; k < 3; ++k)
			c[k] = (std::min)((std::max)(v[k], 0.0f), 1.0f);
		LutSample(lut, c, out);
		for (int axis = 0; axis < 3; ++axis)
		{
			const float d = v[axis] - c[axis];
			if (d == 0.0f)
				continue;
			float slope[3];
			LutEdgeSlope(lut, c, axis, d > 0.0f, slope);
			for (int k = 0; k < 3; ++k)
				out[k] += d * slope[k];
		}
	}

	Decision Decide(bool enabled, const pl_color_space& source, const pl_color_space& output,
		bool calibrationLut)
	{
		Decision d = Decide(enabled, Settings{}, source, output, nullptr, PL_LUT_UNKNOWN);
		// No LUT data in this form: the LUT carry cannot be built (old
		// behaviour; the HDR reasons come first, as they always did).
		if (calibrationLut && d.reason != Reason::Off && d.reason != Reason::HdrSource &&
			d.reason != Reason::HdrOutput && d.reason != Reason::HdrMapped)
			d = Decision{ Reason::CalibrationLut };
		return d;
	}

	Decision Decide(bool enabled, const Settings& settings, const pl_color_space& sourceIn,
		const pl_color_space& outputIn, const pl_custom_lut* lut, enum pl_lut_type lutType)
	{
		Decision d;
		if (!enabled) return d;
		const bool sourceHdr = pl_color_transfer_is_hdr(sourceIn.transfer);
		const bool outputHdr = pl_color_transfer_is_hdr(outputIn.transfer);
		// 4: HDR only when libplacebo maps nothing (identity tone mapping).
		if (sourceHdr)
		{
			if (!settings.hdrPassthrough) { d.reason = Reason::HdrSource; return d; }
			if (!outputHdr) { d.reason = Reason::HdrSource; return d; }
			if (!pl_color_space_equal(&sourceIn, &outputIn) || settings.hdrDynamicToneMapping)
			{
				d.reason = Reason::HdrMapped;
				return d;
			}
		}
		else if (outputHdr) { d.reason = Reason::HdrOutput; return d; }
		// 3: the calibration LUT.
		if (lut)
		{
			if (!settings.calibrationLut) { d.reason = Reason::CalibrationLut; return d; }
			if (lutType != PL_LUT_NORMALIZED || !LutCarryable(lut)) { d.reason = Reason::LutUnsupported; return d; }
		}
		if (sourceHdr)
		{
			d.reason = Reason::Active;
			d.hdrPassthrough = true;
			d.source = d.output = Transfer{ TransferKind::Identity, 0.0f };
			d.sourcePrimaries = sourceIn.primaries;
			d.lut = lut;
			return d;
		}
		// The colour spaces as libplacebo's renderer infers them (black levels
		// for BT.1886's b).
		pl_color_space source = sourceIn, output = outputIn;
		pl_color_space_infer_map(&source, &output);
		const bool piecewise = settings.piecewiseTransfers;
		if (!DescribeTransfer(source, piecewise, d.source)) { d.reason = Reason::SourceTransfer; return d; }
		if (!DescribeTransfer(output, piecewise, d.output)) { d.reason = Reason::OutputTransfer; return d; }
		// 2: the primaries.
		const bool samePrimaries = sourceIn.primaries == outputIn.primaries;
		bool contained = ContainsBt709(outputIn.primaries) &&
			(sourceIn.primaries == PL_COLOR_PRIM_BT_709 || samePrimaries);
		bool wide = false;
		if (!contained)
		{
			if (!settings.wideSourcePrimaries)
			{
				d.reason = ContainsBt709(outputIn.primaries) ? Reason::SourcePrimaries : Reason::OutputPrimaries;
				return d;
			}
			if (sourceIn.primaries == PL_COLOR_PRIM_UNKNOWN || outputIn.primaries == PL_COLOR_PRIM_UNKNOWN)
			{
				d.reason = sourceIn.primaries == PL_COLOR_PRIM_UNKNOWN ? Reason::SourcePrimaries : Reason::OutputPrimaries;
				return d;
			}
			contained = samePrimaries || pl_primaries_superset(pl_raw_primaries_get(outputIn.primaries),
				pl_raw_primaries_get(sourceIn.primaries));
			wide = !contained;
		}
		d.reason = Reason::Active;
		d.sourceGamma = d.source.kind == TransferKind::Power ? d.source.param : 0.0f;
		d.outputGamma = d.output.kind == TransferKind::Power ? d.output.param : 0.0f;
		d.convertGamma = d.source.kind != d.output.kind || d.source.param != d.output.param;
		d.sourcePrimaries = sourceIn.primaries;
		d.lut = lut;
		if (!samePrimaries)
		{
			const pl_matrix3x3 m = pl_get_color_mapping_matrix(pl_raw_primaries_get(sourceIn.primaries),
				pl_raw_primaries_get(outputIn.primaries), PL_INTENT_RELATIVE_COLORIMETRIC);
			for (int row = 0; row < 3; ++row)
				for (int column = 0; column < 3; ++column)
					d.matrix[column * 3 + row] = m.m[row][column];
			if (wide && !lut)
				d.libplaceboMapsPrimaries = true;   // the late restore applies the matrix
			else
			{
				d.convertPrimaries = true;
				d.gamutClip = wide;
			}
		}
		return d;
	}

	Decision ForTemporal(const Decision& decision, bool frameMixing)
	{
		if (decision.Active() && frameMixing && decision.LateRestore())
			return Decision{ Reason::TemporalMix };
		return decision;
	}

	std::string Describe(const Decision& d)
	{
		if (!d.Active())
			return ToString(d.reason);
		char buffer[320];
		std::snprintf(buffer, sizeof(buffer),
			"active, transfer %s %.3g -> %s %.3g%s, primaries %s%s%s%s",
			TransferName(d.source), d.source.param, TransferName(d.output), d.output.param,
			d.convertGamma ? "" : " (none)",
			pl_color_primaries_name(d.sourcePrimaries),
			d.convertPrimaries ? " (converted by VP)" : d.libplaceboMapsPrimaries ? " (libplacebo gamut mapping, linear remainder)" : " (none)",
			d.gamutClip ? ", gamut clip (calibration LUT)" : d.hdrPassthrough ? ", HDR passthrough" : "",
			d.lut ? ", carried around the calibration LUT" : "");
		return buffer;
	}

	Hooks::Hooks()
	{
		carry.stages = PL_HOOK_RGB;
		carry.input = PL_HOOK_SIG_TEX;
		carry.priv = this;
		carry.hook = &Hooks::RunCarry;
		carry.signature = 0x5650585253504C54ull;   // "VPXRSPLT"
		restore.stages = PL_HOOK_SCALED;
		restore.input = PL_HOOK_SIG_COLOR;
		restore.priv = this;
		restore.hook = &Hooks::RunRestore;
		restore.signature = 0x5650585252535452ull;   // "VPXRRSTR"
		restoreTexture = restore;
		restoreTexture.input = PL_HOOK_SIG_TEX;
		restoreTexture.hook = &Hooks::RunRestoreTexture;
		restoreTexture.signature = 0x5650585252535454ull;   // "VPXRRSTT"
		// Audit 2026-10-16: the late restore.
		late.stages = PL_HOOK_PRE_OUTPUT;
		late.priv = this;
		late.input = PL_HOOK_SIG_TEX;
		late.hook = &Hooks::RunLate;
		late.signature = 0x565058524C415445ull;   // "VPXRLATE"
	}

	void Hooks::SetFrame(const Decision& decisionIn, const pl_color_space& outputIn)
	{
		decision = decisionIn;
		output = outputIn;
		gammaRatio = decision.outputGamma > 0.0f ? decision.sourceGamma / decision.outputGamma : 1.0f;
		scaledValid = false;   // audit 2026-10-16: set again by this frame's SCALED restore
	}

	bool Hooks::EnsureFloat(pl_gpu gpu, pl_gpu& owner, pl_tex& tex, int w, int h, bool storable)
	{
		if (tex && owner == gpu && tex->params.w == w && tex->params.h == h && (!storable || tex->params.storable))
			return true;
		if (owner)
			pl_tex_destroy(owner, &tex);
		owner = nullptr;
		const pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR));
		if (!fmt)
			return false;
		pl_tex_params tp{};
		tp.w = w;
		tp.h = h;
		tp.format = fmt;
		tp.sampleable = true;
		tp.renderable = true;
		tp.storable = storable && (fmt->caps & PL_FMT_CAP_STORABLE) != 0;
		if (storable && !tp.storable)
			return false;
		tex = pl_tex_create(gpu, &tp);
		if (!tex)
			return false;
		owner = gpu;
		return true;
	}

	// The split into VP's own float textures (the clipped picture, handed on;
	// the remainder, kept). Efficiency audit 2026-10-24 #7: one pass with two
	// outputs when the GPU can store into the remainder; otherwise (or after
	// a one-pass failure, latched with its reason) the two original passes.
	pl_hook_res Hooks::RunCarry(void* priv, const pl_hook_params* params)
	{
		Hooks* self = static_cast<Hooks*>(priv);
		if (!params->tex)
			return Failure();
		const int w = params->tex->params.w, h = params->tex->params.h;
		if (self->gpu && self->gpu != params->gpu)
			self->Release();
		pl_gpu ownerA = self->gpu, ownerB = self->gpu;
		bool onePass = self->onePassAllowed && self->carryOnePassReason == CarryPassReason::None;
		if (onePass && !params->gpu->glsl.compute)
		{
			self->carryOnePassReason = CarryPassReason::NoCompute;
			onePass = false;
		}
		if (!EnsureFloat(params->gpu, ownerA, self->clipped, w, h))
			return Failure();
		if (onePass && !EnsureFloat(params->gpu, ownerB, self->remainder, w, h, true))
		{
			// Not storable here: the two-pass form, a plain renderable texture.
			self->carryOnePassReason = CarryPassReason::NotStorable;
			onePass = false;
		}
		if (!onePass && !EnsureFloat(params->gpu, ownerB, self->remainder, w, h))
			return Failure();
		self->gpu = params->gpu;
		const float convertGamma = self->decision.convertGamma ? 1.0f : 0.0f;
		const float convertPrimaries = self->decision.convertPrimaries ? 1.0f : 0.0f;
		const float sourceKind = TransferKindValue(self->decision.source);
		const float outputKind = TransferKindValue(self->decision.output);
		const float linearRemainder = self->decision.libplaceboMapsPrimaries ? 1.0f : 0.0f;
		if (onePass)
		{
			if (self->RunCarryOnePass(params, convertGamma, convertPrimaries, sourceKind, outputKind, linearRemainder))
			{
				++self->carryOnePassFrames;
				self->carryRect = params->rect;
				pl_hook_res res{};
				res.output = PL_HOOK_SIG_TEX;
				res.tex = self->clipped;
				res.repr = params->repr;
				res.color = params->color;
				res.components = params->components;
				res.rect = params->rect;
				return res;
			}
			// Latched: the two passes from now on (same output; visible in
			// CarryPassesDescribe for the diagnostic readout).
			self->carryOnePassReason = CarryPassReason::OnePassFailed;
		}
		++self->carryTwoPassFrames;
		const std::string remainderHeader = std::string("#define ER_REMAINDER 1\n") + TRANSFER_GLSL;
		for (int pass = 0; pass < 2; ++pass)
		{
			pl_shader sh = pl_dispatch_begin(params->dispatch);
			pl_sample_src src{};
			src.tex = params->tex;
			src.components = 4;
			const pl_shader_var variables[8] = {
				{ pl_var_float("er_convert_gamma"), &convertGamma, true },
				{ pl_var_float("er_convert_primaries"), &convertPrimaries, true },
				{ pl_var_float("er_src_kind"), &sourceKind, true },
				{ pl_var_float("er_src_param"), &self->decision.source.param, true },
				{ pl_var_float("er_out_kind"), &outputKind, true },
				{ pl_var_float("er_out_param"), &self->decision.output.param, true },
				{ pl_var_float("er_linear_remainder"), &linearRemainder, true },
				{ pl_var_mat3("er_matrix"), self->decision.matrix, true } };
			pl_custom_shader custom{};
			custom.description = pass == 0 ? "VP extended range: remainder" : "VP extended range: clipped picture";
			custom.header = pass == 0 ? remainderHeader.c_str() : TRANSFER_GLSL;
			custom.input = PL_SHADER_SIG_COLOR;
			custom.output = PL_SHADER_SIG_COLOR;
			custom.variables = variables;
			custom.num_variables = 8;
			custom.body = CARRY_BODY;
			if (!pl_shader_sample_direct(sh, &src) || !pl_shader_custom(sh, &custom))
			{
				pl_dispatch_abort(params->dispatch, &sh);
				return Failure();
			}
			pl_dispatch_params dp{};
			dp.shader = &sh;
			dp.target = pass == 0 ? self->remainder : self->clipped;
			if (!pl_dispatch_finish(params->dispatch, &dp))
				return Failure();
		}
		self->carryRect = params->rect;
		pl_hook_res res{};
		res.output = PL_HOOK_SIG_TEX;
		res.tex = self->clipped;
		res.repr = params->repr;
		res.color = params->color;
		res.components = params->components;
		res.rect = params->rect;
		return res;
	}

	bool Hooks::RunCarryOnePass(const pl_hook_params* params, const float& convertGamma,
		const float& convertPrimaries, const float& sourceKind, const float& outputKind,
		const float& linearRemainder)
	{
		pl_shader sh = pl_dispatch_begin(params->dispatch);
		pl_sample_src src{};
		src.tex = params->tex;
		src.components = 4;
		const pl_shader_var variables[8] = {
			{ pl_var_float("er_convert_gamma"), &convertGamma, true },
			{ pl_var_float("er_convert_primaries"), &convertPrimaries, true },
			{ pl_var_float("er_src_kind"), &sourceKind, true },
			{ pl_var_float("er_src_param"), &decision.source.param, true },
			{ pl_var_float("er_out_kind"), &outputKind, true },
			{ pl_var_float("er_out_param"), &decision.output.param, true },
			{ pl_var_float("er_linear_remainder"), &linearRemainder, true },
			{ pl_var_mat3("er_matrix"), decision.matrix, true } };
		pl_shader_desc out{};
		out.desc.name = "er_remainder_out";
		out.desc.type = PL_DESC_STORAGE_IMG;
		out.desc.access = PL_DESC_ACCESS_WRITEONLY;
		out.binding.object = remainder;
		const std::string body = CarryOnePassBody();
		pl_custom_shader custom{};
		custom.description = "VP extended range: clipped picture + remainder";
		custom.header = TRANSFER_GLSL;
		custom.input = PL_SHADER_SIG_COLOR;
		custom.output = PL_SHADER_SIG_COLOR;
		custom.variables = variables;
		custom.num_variables = 8;
		custom.descriptors = &out;
		custom.num_descriptors = 1;
		custom.body = body.c_str();
		if (!pl_shader_sample_direct(sh, &src) || !pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return false;
		}
		pl_dispatch_params dp{};
		dp.shader = &sh;
		dp.target = clipped;
		return pl_dispatch_finish(params->dispatch, &dp);
	}

	std::string CarryOnePassBody()
	{
		// The old body without ER_REMAINDER ends with "color.rgb = er_clipped;":
		// keep everything before the #ifdef (conversion + clamp), then the tail.
		std::string body = CARRY_BODY;
		body.erase(body.find("#ifdef ER_REMAINDER"));
		return body + CARRY_ONE_PASS_TAIL;
	}

	const char* CarryBody() { return CARRY_BODY; }

	const char* CarryPassReasonText(CarryPassReason reason)
	{
		switch (reason)
		{
		case CarryPassReason::None: return "one pass";
		case CarryPassReason::NoCompute: return "two passes (GPU has no storage images)";
		case CarryPassReason::NotStorable: return "two passes (rgba32f not storable)";
		case CarryPassReason::OnePassFailed: return "two passes (one-pass shader failed)";
		case CarryPassReason::Off: return "two passes (efficiency_fixes off)";
		}
		return "two passes";
	}

	std::string Hooks::CarryPassesDescribe() const
	{
		std::string s = "ER carry: ";
		s += CarryPassReasonText(CarryPasses());
		s += " (frames: one-pass " + std::to_string(carryOnePassFrames) + ", two-pass " +
			std::to_string(carryTwoPassFrames) + ")";
		return s;
	}

	// The restore on a shader: libplacebo's own delinearize when the stage
	// is linear, then the offset back and the primaries matrix.
	bool Hooks::AppendRestore(pl_shader sh, pl_color_space& color, const pl_rect2df& rect) const
	{
		if (!remainder)
			return false;
		// After linear-light (or sigmoid) scaling libplacebo hands this stage
		// linear values: back to the output transfer it was told, with its
		// own exact inverse (including its SDR black handling).
		if (color.transfer == PL_COLOR_TRC_LINEAR)
		{
			color.transfer = output.transfer;
			pl_shader_delinearize(sh, &color);
		}
		const float outRect[4] = { rect.x0, rect.y0, rect.x1 - rect.x0, rect.y1 - rect.y0 };
		const float carry[4] = { carryRect.x0, carryRect.y0, carryRect.x1 - carryRect.x0, carryRect.y1 - carryRect.y0 };
		const pl_shader_var variables[2] = {
			{ pl_var_vec4("er_out_rect"), outRect, true },
			{ pl_var_vec4("er_carry_rect"), carry, true } };
		pl_shader_desc remainderDesc{};
		remainderDesc.desc.name = "er_remainder";
		remainderDesc.desc.type = PL_DESC_SAMPLED_TEX;
		remainderDesc.binding.object = remainder;
		remainderDesc.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
		remainderDesc.binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
		pl_custom_shader custom{};
		custom.description = "VP extended range: restore";
		custom.header = RESTORE_HEADER;
		custom.input = PL_SHADER_SIG_COLOR;
		custom.output = PL_SHADER_SIG_COLOR;
		custom.descriptors = &remainderDesc;
		custom.num_descriptors = 1;
		custom.variables = variables;
		custom.num_variables = 2;
		custom.body = RESTORE_BODY;
		return pl_shader_custom(sh, &custom);
	}

	// Inline, in the scaler's own pass (no intermediate texture): exact.
	pl_hook_res Hooks::RunRestore(void* priv, const pl_hook_params* params)
	{
		Hooks* self = static_cast<Hooks*>(priv);
		pl_color_space color = params->color;
		if (!self->AppendRestore(params->sh, color, params->rect))
			return Failure();
		++self->applied;
		pl_hook_res res{};
		res.output = PL_HOOK_SIG_COLOR;
		res.sh = params->sh;
		res.repr = params->repr;
		res.repr.alpha = PL_ALPHA_NONE;
		res.color = color;
		res.components = 3;
		res.rect = params->rect;
		return res;
	}

	void Hooks::Release()
	{
		if (gpu)
			for (pl_tex* tex : { &clipped, &remainder, &restoreTex, &lateTex, &lutTex })
				pl_tex_destroy(gpu, tex);
		gpu = nullptr;
		clipped = remainder = restoreTex = lateTex = lutTex = nullptr;
		scaledValid = false;
		lutSignature = 0;
		lutData = nullptr;
		lutSize[0] = lutSize[1] = lutSize[2] = 0;
	}

	// After another hook's texture output at PRE_KERNEL / POST_KERNEL (the
	// SSIM downscaler, the Stage 8 blend) libplacebo rejects the inline
	// restore at SCALED once ("resizing non-resizable stage") and disables it
	// for the renderer's life: measured. There the restore renders its own
	// full-float texture. (Not used otherwise: a texture input makes
	// libplacebo store the scaled picture in an intermediate texture first;
	// with those hooks it is stored anyway. Precision of that texture (hunt 2,
	// 08 #9): rgba32f on the Beta build (output_fbo_float32 always set,
	// fbo_float32 default on: LibplaceboRenderParameters.cpp); half float,
	// up to about a quarter code, only on the Stable 7.360 build or with
	// fbo_float32 off by hand.)
	pl_hook_res Hooks::RunRestoreTexture(void* priv, const pl_hook_params* params)
	{
		Hooks* self = static_cast<Hooks*>(priv);
		if (!params->tex)
			return Failure();
		// Only its own texture is resized: the carry's may be this frame's
		// current image (Release() would free both).
		const int w = params->tex->params.w, h = params->tex->params.h;
		pl_gpu owner = self->gpu;
		if (!EnsureFloat(params->gpu, owner, self->restoreTex, w, h))
			return Failure();
		self->gpu = params->gpu;
		pl_shader sh = pl_dispatch_begin(params->dispatch);
		pl_sample_src src{};
		src.tex = params->tex;
		src.components = 4;
		pl_color_space color = params->color;
		if (!pl_shader_sample_direct(sh, &src) || !self->AppendRestore(sh, color, params->rect))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return Failure();
		}
		pl_dispatch_params dp{};
		dp.shader = &sh;
		dp.target = self->restoreTex;
		if (!pl_dispatch_finish(params->dispatch, &dp))
			return Failure();
		++self->applied;
		// Audit 2026-10-16: the late LUT restore reads v back from here.
		self->scaledRect = params->rect;
		self->scaledValid = true;
		pl_hook_res res{};
		res.output = PL_HOOK_SIG_TEX;
		res.tex = self->restoreTex;
		res.repr = params->repr;
		res.repr.alpha = PL_ALPHA_NONE;
		res.color = color;
		res.components = 3;
		res.rect = params->rect;
		return res;
	}

	// Audit 2026-10-16: the target LUT as VP's own rgba32f 3D texture
	// (texelFetch only), re-uploaded when the LUT changes.
	bool Hooks::EnsureLut(pl_gpu gpu)
	{
		const pl_custom_lut* lut = decision.lut;
		if (!LutCarryable(lut))
			return false;
		if (lutTex && lutSignature == lut->signature && lutData == lut->data &&
			lutSize[0] == lut->size[0] && lutSize[1] == lut->size[1] && lutSize[2] == lut->size[2])
			return true;
		if (lutTex)
			pl_tex_destroy(gpu, &lutTex);
		const int maxDim = std::max({ lut->size[0], lut->size[1], lut->size[2] });
		if (gpu->limits.max_tex_3d_dim < static_cast<uint32_t>(maxDim))
			return false;
		const pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
		if (!fmt)
			return false;
		const size_t count = static_cast<size_t>(lut->size[0]) * lut->size[1] * lut->size[2];
		std::vector<float> rgba(count * 4);
		for (size_t i = 0; i < count; ++i)
		{
			rgba[i * 4 + 0] = lut->data[i * 3 + 0];
			rgba[i * 4 + 1] = lut->data[i * 3 + 1];
			rgba[i * 4 + 2] = lut->data[i * 3 + 2];
			rgba[i * 4 + 3] = 1.0f;
		}
		pl_tex_params tp{};
		tp.w = lut->size[0];
		tp.h = lut->size[1];
		tp.d = lut->size[2];
		tp.format = fmt;
		tp.sampleable = true;
		tp.initial_data = rgba.data();
		lutTex = pl_tex_create(gpu, &tp);
		if (!lutTex)
			return false;
		lutSignature = lut->signature;
		lutData = lut->data;
		for (int i = 0; i < 3; ++i)
			lutSize[i] = lut->size[i];
		return true;
	}

	// Audit 2026-10-16: the late restore, the first hook at PRE_OUTPUT, into
	// VP's own float texture. LUT mode: v from the SCALED restore's texture,
	// the LUT's edge slope; gamut mode: the linear remainder through the
	// matrix after libplacebo's gamut mapping.
	// The late restore's shader, appended to VP's pass over the stage's texture.
	bool Hooks::AppendLate(pl_shader sh, const pl_rect2df& rect) const
	{
		const bool lutMode = decision.lut != nullptr;
		const float outRect[4] = { rect.x0, rect.y0, rect.x1 - rect.x0, rect.y1 - rect.y0 };
		const pl_rect2df& sr = scaledRect;
		const float scaled[4] = { sr.x0, sr.y0, sr.x1 - sr.x0, sr.y1 - sr.y0 };
		const float carry[4] = { carryRect.x0, carryRect.y0, carryRect.x1 - carryRect.x0, carryRect.y1 - carryRect.y0 };
		const float lutMax[3] = { static_cast<float>(lutSize[0] - 1), static_cast<float>(lutSize[1] - 1),
			static_cast<float>(lutSize[2] - 1) };
		const float outputKind = TransferKindValue(decision.output);
		std::string header;
		pl_shader_var variables[5];
		pl_shader_desc descriptors[2]{};
		int numVariables = 0, numDescriptors = 0;
		variables[numVariables++] = { pl_var_vec4("er_out_rect"), outRect, true };
		if (lutMode)
		{
			header = LATE_LUT_HEADER;
			variables[numVariables++] = { pl_var_vec4("er_scaled_rect"), scaled, true };
			variables[numVariables++] = { pl_var_vec3("er_lut_max"), lutMax, true };
			pl_shader_desc& s = descriptors[numDescriptors++];
			s.desc.name = "er_scaled";
			s.desc.type = PL_DESC_SAMPLED_TEX;
			s.binding.object = restoreTex;
			s.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			s.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			pl_shader_desc& table = descriptors[numDescriptors++];
			table.desc.name = "er_lut";
			table.desc.type = PL_DESC_SAMPLED_TEX;
			table.binding.object = lutTex;
			table.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			table.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		}
		else
		{
			header = std::string(TRANSFER_GLSL) + RESTORE_HEADER;
			variables[numVariables++] = { pl_var_vec4("er_carry_rect"), carry, true };
			variables[numVariables++] = { pl_var_float("er_out_kind"), &outputKind, true };
			variables[numVariables++] = { pl_var_float("er_out_param"), &decision.output.param, true };
			variables[numVariables++] = { pl_var_mat3("er_matrix"), decision.matrix, true };
			pl_shader_desc& rem = descriptors[numDescriptors++];
			rem.desc.name = "er_remainder";
			rem.desc.type = PL_DESC_SAMPLED_TEX;
			rem.binding.object = remainder;
			rem.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			rem.binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
		}
		pl_custom_shader custom{};
		custom.description = lutMode ? "VP extended range: late restore (LUT)" : "VP extended range: late restore (gamut)";
		custom.header = header.c_str();
		custom.input = PL_SHADER_SIG_COLOR;
		custom.output = PL_SHADER_SIG_COLOR;
		custom.descriptors = descriptors;
		custom.num_descriptors = numDescriptors;
		custom.variables = variables;
		custom.num_variables = numVariables;
		custom.body = lutMode ? LATE_LUT_BODY : LATE_GAMUT_BODY;
		return pl_shader_custom(sh, &custom);
	}

	// 0: nothing to add this frame, 1: ready, -1: failure.
	int Hooks::LateReady(pl_gpu gpu)
	{
		if (!this->gpu || this->gpu != gpu)
			return -1;
		const bool lutMode = decision.lut != nullptr;
		const bool gamutMode = decision.libplaceboMapsPrimaries;
		// Nothing to add (no SCALED picture this frame): leave the stage
		// untouched rather than fail (a failure disables the hook for good).
		if (lutMode == gamutMode || (lutMode && (!scaledValid || !restoreTex)))
			return 0;
		if (lutMode && !EnsureLut(gpu))
			return -1;
		if (gamutMode && !remainder)
			return -1;
		return 1;
	}

	// The texture form: VP's own pass into its float texture. Audit
	// 2026-10-16 (GPU run): an inline form in libplacebo's PRE_OUTPUT shader
	// is rejected ("User hook tried resizing non-resizable stage!", then
	// disabled), as at SCALED; so the picture goes through libplacebo's
	// intermediate texture first: rgba32f on the Beta build (hunt 2, 08 #9;
	// half float - about 1/4 code - only on Stable 7.360 or fbo_float32 off).
	pl_hook_res Hooks::RunLate(void* priv, const pl_hook_params* params)
	{
		Hooks* self = static_cast<Hooks*>(priv);
		if (!params->tex)
			return Failure();
		const int ready = self->LateReady(params->gpu);
		if (ready == 0)
			return pl_hook_res{};
		if (ready < 0)
			return Failure();
		const int w = params->tex->params.w, h = params->tex->params.h;
		pl_gpu owner = self->gpu;
		if (!EnsureFloat(params->gpu, owner, self->lateTex, w, h))
			return Failure();
		pl_shader sh = pl_dispatch_begin(params->dispatch);
		pl_sample_src src{};
		src.tex = params->tex;
		src.components = 4;
		if (!pl_shader_sample_direct(sh, &src) || !self->AppendLate(sh, params->rect))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return Failure();
		}
		pl_dispatch_params dp{};
		dp.shader = &sh;
		dp.target = self->lateTex;
		if (!pl_dispatch_finish(params->dispatch, &dp))
			return Failure();
		++self->applied;
		pl_hook_res res{};
		res.output = PL_HOOK_SIG_TEX;
		res.tex = self->lateTex;
		res.repr = params->repr;
		res.repr.alpha = PL_ALPHA_NONE;
		res.color = params->color;
		res.components = 3;
		res.rect = params->rect;
		return res;
	}

	void Prepare(const Decision& decision, Hooks& hooks, pl_color_space& sourceColor,
		const pl_color_space& output, std::vector<const pl_hook*>& list, bool singlePlaneRgb)
	{
		if (!decision.Active())
			return;
		hooks.SetFrame(decision, output);
		sourceColor = output;
		// Audit 2026-10-16: a wide source gamut keeps libplacebo's gamut
		// mapping (the carry re-encoded the transfer only).
		if (decision.libplaceboMapsPrimaries)
			sourceColor.primaries = decision.sourcePrimaries;
		// The restore renders its own texture: after the carry's texture output
		// libplacebo rejects an inline hook at SCALED (measured; 10a found the
		// same after any texture-output hook at PRE/POST_KERNEL).
		(void)singlePlaneRgb;
		const bool textureRestore = true;
		if (decision.ScaledRestore())
			list.insert(list.begin(), hooks.Restore(textureRestore));
		if (decision.LateRestore())
			list.insert(list.begin(), hooks.LateRestore());
		list.push_back(hooks.Carry());
	}
}
