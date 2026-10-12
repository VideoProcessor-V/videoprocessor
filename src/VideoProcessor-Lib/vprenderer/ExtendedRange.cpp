#include <pch.h>
#include "ExtendedRange.h"

#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/shaders/custom.h>

#include <cmath>

namespace ExtendedRange
{
	namespace
	{
		// The carry: the source gamma re-encoded to the output's, the primaries
		// converted here, before scaling, where libplacebo converts without the
		// switch (a non-linear shader such as SSIM then sees the same values;
		// converting after scaling differed by up to 40 codes on saturated
		// colours, measured); then either the picture clipped to 0..1 or the
		// remainder value - clipped (ER_REMAINDER), per channel.
		const char* const CARRY_BODY = R"GLSL(
if (er_convert_gamma > 0.5)
    color.rgb = sign(color.rgb) * pow(abs(color.rgb), vec3(er_gamma_ratio));
if (er_convert_primaries > 0.5) {
    vec3 er_l = sign(color.rgb) * pow(abs(color.rgb), vec3(er_output_gamma));
    er_l = er_matrix * er_l;
    color.rgb = sign(er_l) * pow(abs(er_l), vec3(1.0 / er_output_gamma));
}
vec3 er_clipped = clamp(color.rgb, 0.0, 1.0);
#ifdef ER_REMAINDER
color = vec4(color.rgb - er_clipped, 1.0);
#else
color.rgb = er_clipped;
#endif
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
	}

	void Hooks::SetFrame(const Decision& decisionIn, const pl_color_space& outputIn)
	{
		decision = decisionIn;
		output = outputIn;
		gammaRatio = decision.outputGamma > 0.0f ? decision.sourceGamma / decision.outputGamma : 1.0f;
	}

	bool Hooks::EnsureFloat(pl_gpu gpu, pl_gpu& owner, pl_tex& tex, int w, int h)
	{
		if (tex && owner == gpu && tex->params.w == w && tex->params.h == h)
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
		tex = pl_tex_create(gpu, &tp);
		if (!tex)
			return false;
		owner = gpu;
		return true;
	}

	// The split: two passes over the RGB-stage texture into VP's own float
	// textures (the clipped picture, handed on; the remainder, kept).
	pl_hook_res Hooks::RunCarry(void* priv, const pl_hook_params* params)
	{
		Hooks* self = static_cast<Hooks*>(priv);
		if (!params->tex)
			return Failure();
		const int w = params->tex->params.w, h = params->tex->params.h;
		if (self->gpu && self->gpu != params->gpu)
			self->Release();
		pl_gpu ownerA = self->gpu, ownerB = self->gpu;
		if (!EnsureFloat(params->gpu, ownerA, self->clipped, w, h) ||
			!EnsureFloat(params->gpu, ownerB, self->remainder, w, h))
			return Failure();
		self->gpu = params->gpu;
		const float convertGamma = self->decision.convertGamma ? 1.0f : 0.0f;
		const float convertPrimaries = self->decision.convertPrimaries ? 1.0f : 0.0f;
		for (int pass = 0; pass < 2; ++pass)
		{
			pl_shader sh = pl_dispatch_begin(params->dispatch);
			pl_sample_src src{};
			src.tex = params->tex;
			src.components = 4;
			const pl_shader_var variables[5] = {
				{ pl_var_float("er_convert_gamma"), &convertGamma, true },
				{ pl_var_float("er_gamma_ratio"), &self->gammaRatio, true },
				{ pl_var_float("er_convert_primaries"), &convertPrimaries, true },
				{ pl_var_float("er_output_gamma"), &self->decision.outputGamma, true },
				{ pl_var_mat3("er_matrix"), self->decision.matrix, true } };
			pl_custom_shader custom{};
			custom.description = pass == 0 ? "VP extended range: remainder" : "VP extended range: clipped picture";
			custom.header = pass == 0 ? "#define ER_REMAINDER 1\n" : nullptr;
			custom.input = PL_SHADER_SIG_COLOR;
			custom.output = PL_SHADER_SIG_COLOR;
			custom.variables = variables;
			custom.num_variables = 5;
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
			for (pl_tex* tex : { &clipped, &remainder, &restoreTex })
				pl_tex_destroy(gpu, tex);
		gpu = nullptr;
		clipped = remainder = restoreTex = nullptr;
	}

	// After another hook's texture output at PRE_KERNEL / POST_KERNEL (the
	// SSIM downscaler, the Stage 8 blend) libplacebo rejects the inline
	// restore at SCALED once ("resizing non-resizable stage") and disables it
	// for the renderer's life: measured. There the restore renders its own
	// full-float texture. (Not used otherwise: a texture input makes
	// libplacebo store the scaled picture in a half-float texture first,
	// costing up to a quarter code; with those hooks it is stored anyway.)
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

	void Prepare(const Decision& decision, Hooks& hooks, pl_color_space& sourceColor,
		const pl_color_space& output, std::vector<const pl_hook*>& list, bool singlePlaneRgb)
	{
		if (!decision.Active())
			return;
		hooks.SetFrame(decision, output);
		sourceColor = output;
		// The restore renders its own texture: after the carry's texture output
		// libplacebo rejects an inline hook at SCALED (measured; 10a found the
		// same after any texture-output hook at PRE/POST_KERNEL).
		(void)singlePlaneRgb;
		const bool textureRestore = true;
		list.insert(list.begin(), hooks.Restore(textureRestore));
		list.push_back(hooks.Carry());
	}
}
