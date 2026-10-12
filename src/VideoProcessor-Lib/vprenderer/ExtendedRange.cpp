#include <pch.h>
#include "ExtendedRange.h"

#include <libplacebo/colorspace.h>
#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/shaders/custom.h>

#include <cmath>

namespace ExtendedRange
{
	namespace
	{
		// The carried offset goes into alpha as it is (o in -0.5..0.5, about
		// 440 10-bit codes beyond the legal range; anything further is
		// clipped by libplacebo as without the switch).
		// libplacebo's intermediate textures are 16-bit float, and it never
		// sigmoidises, linearises or clamps alpha. In-range pixels carry
		// exactly 0, which any scaler keeps exactly 0 (its weights need not
		// sum to exactly 1: a constant non-zero alpha came back up to about
		// a code off), so they are scaled exactly as without the switch.
		const char* const CARRY_BODY = R"GLSL(
if (er_convert_gamma > 0.5)
    color.rgb = sign(color.rgb) * pow(abs(color.rgb), vec3(er_gamma_ratio));
float er_lo = min(min(color.r, color.g), color.b);
float er_hi = max(max(color.r, color.g), color.b);
float er_o = er_lo < 0.0 ? max(er_lo, -0.5) : (er_hi > 1.0 ? min(er_hi - 1.0, 0.5) : 0.0);
color.rgb -= vec3(er_o);
color.a = er_o;
)GLSL";

		// Guard (audit stage10-6d): a user shader between RGB and SCALED that
		// writes alpha (e.g. vec4(rgb, 1.0)) leaves a value outside the
		// carried range; it is ignored rather than added to every channel.
		const char* const RESTORE_BODY = R"GLSL(
color.rgb += vec3(abs(color.a) <= 0.5009 ? color.a : 0.0);
color.a = 1.0;
if (er_convert_primaries > 0.5) {
    vec3 er_l = sign(color.rgb) * pow(abs(color.rgb), vec3(er_output_gamma));
    er_l = er_matrix * er_l;
    color.rgb = sign(er_l) * pow(abs(er_l), vec3(1.0 / er_output_gamma));
}
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
	}

	const char* ToString(Reason reason)
	{
		switch (reason)
		{
		case Reason::Active: return "active";
		case Reason::Off: return "off";
		case Reason::HdrSource: return "off for HDR sources";
		case Reason::HdrOutput: return "off for an HDR output";
		case Reason::CalibrationLut: return "off with a calibration LUT";
		case Reason::SourceTransfer: return "off: the source transfer is not a pure power";
		case Reason::OutputTransfer: return "off: the output transfer is not a pure power";
		case Reason::SourcePrimaries: return "off: SDR source primaries other than BT.709";
		case Reason::OutputPrimaries: return "off: the output primaries do not contain BT.709";
		case Reason::HookFailed: return "off: libplacebo disabled its hook";
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

	Decision Decide(bool enabled, const pl_color_space& source, const pl_color_space& output,
		bool calibrationLut)
	{
		Decision d;
		if (!enabled) return d;
		if (pl_color_transfer_is_hdr(source.transfer)) { d.reason = Reason::HdrSource; return d; }
		if (pl_color_transfer_is_hdr(output.transfer)) { d.reason = Reason::HdrOutput; return d; }
		if (calibrationLut) { d.reason = Reason::CalibrationLut; return d; }
		const double sourceGamma = PowerGamma(source.transfer);
		const double outputGamma = PowerGamma(output.transfer);
		if (sourceGamma <= 0.0) { d.reason = Reason::SourceTransfer; return d; }
		if (outputGamma <= 0.0) { d.reason = Reason::OutputTransfer; return d; }
		if (!ContainsBt709(output.primaries)) { d.reason = Reason::OutputPrimaries; return d; }
		if (source.primaries != PL_COLOR_PRIM_BT_709 && source.primaries != output.primaries)
		{
			d.reason = Reason::SourcePrimaries;
			return d;
		}
		d.reason = Reason::Active;
		d.sourceGamma = static_cast<float>(sourceGamma);
		d.outputGamma = static_cast<float>(outputGamma);
		d.convertGamma = sourceGamma != outputGamma;
		d.convertPrimaries = source.primaries != output.primaries;
		if (d.convertPrimaries)
		{
			const pl_matrix3x3 m = pl_get_color_mapping_matrix(pl_raw_primaries_get(source.primaries),
				pl_raw_primaries_get(output.primaries), PL_INTENT_RELATIVE_COLORIMETRIC);
			for (int row = 0; row < 3; ++row)
				for (int column = 0; column < 3; ++column)
					d.matrix[column * 3 + row] = m.m[row][column];
		}
		return d;
	}

	Hooks::Hooks()
	{
		carry.stages = PL_HOOK_RGB;
		carry.input = PL_HOOK_SIG_COLOR;
		carry.priv = this;
		carry.hook = &Hooks::RunCarry;
		carry.signature = 0x5650585243415259ull;   // "VPXRCARY"
		restore.stages = PL_HOOK_SCALED;
		restore.input = PL_HOOK_SIG_COLOR;
		restore.priv = this;
		restore.hook = &Hooks::RunRestore;
		restore.signature = 0x5650585252535452ull;   // "VPXRRSTR"
	}

	void Hooks::SetFrame(const Decision& decisionIn, const pl_color_space& outputIn)
	{
		decision = decisionIn;
		output = outputIn;
		gammaRatio = decision.outputGamma > 0.0f ? decision.sourceGamma / decision.outputGamma : 1.0f;
	}

	pl_hook_res Hooks::RunCarry(void* priv, const pl_hook_params* params)
	{
		Hooks* self = static_cast<Hooks*>(priv);
		const float convertGamma = self->decision.convertGamma ? 1.0f : 0.0f;
		const pl_shader_var variables[2] = {
			{ pl_var_float("er_convert_gamma"), &convertGamma, true },
			{ pl_var_float("er_gamma_ratio"), &self->gammaRatio, true } };
		pl_custom_shader custom{};
		custom.description = "VP extended range: carry";
		custom.input = PL_SHADER_SIG_COLOR;
		custom.output = PL_SHADER_SIG_COLOR;
		custom.variables = variables;
		custom.num_variables = 2;
		custom.body = CARRY_BODY;
		if (!pl_shader_custom(params->sh, &custom))
			return Failure();
		pl_hook_res res{};
		res.output = PL_HOOK_SIG_COLOR;
		res.sh = params->sh;
		res.repr = params->repr;
		res.repr.alpha = PL_ALPHA_INDEPENDENT;
		res.color = params->color;
		res.components = 4;
		res.rect = params->rect;
		return res;
	}

	pl_hook_res Hooks::RunRestore(void* priv, const pl_hook_params* params)
	{
		Hooks* self = static_cast<Hooks*>(priv);
		// After linear-light (or sigmoid) scaling libplacebo hands this stage
		// linear values: back to the output transfer it was told, with its
		// own exact inverse (including its SDR black handling).
		pl_color_space color = params->color;
		if (color.transfer == PL_COLOR_TRC_LINEAR)
		{
			color.transfer = self->output.transfer;
			pl_shader_delinearize(params->sh, &color);
		}
		const float convertPrimaries = self->decision.convertPrimaries ? 1.0f : 0.0f;
		const pl_shader_var variables[3] = {
			{ pl_var_float("er_convert_primaries"), &convertPrimaries, true },
			{ pl_var_float("er_output_gamma"), &self->decision.outputGamma, true },
			{ pl_var_mat3("er_matrix"), self->decision.matrix, true } };
		pl_custom_shader custom{};
		custom.description = "VP extended range: restore";
		custom.input = PL_SHADER_SIG_COLOR;
		custom.output = PL_SHADER_SIG_COLOR;
		custom.variables = variables;
		custom.num_variables = 3;
		custom.body = RESTORE_BODY;
		if (!pl_shader_custom(params->sh, &custom))
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

	void Prepare(const Decision& decision, Hooks& hooks, pl_color_space& sourceColor,
		const pl_color_space& output, std::vector<const pl_hook*>& list)
	{
		if (!decision.Active())
			return;
		hooks.SetFrame(decision, output);
		sourceColor = output;
		list.insert(list.begin(), { hooks.Carry(), hooks.Restore() });
	}
}
