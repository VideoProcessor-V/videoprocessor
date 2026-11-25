// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <VP-V/modules/scaling/route_4k/Route4k.h>

#include <VP-V/core/ChromaSiting.h>
#include <VP-V/core/LumaWeights.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>

#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Route4k
{
	namespace
	{
		std::string NormaliseToken(const std::string& s)
		{
			size_t a = 0;
			while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
				++a;
			size_t b = s.size();
			while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
				--b;
			std::string out;
			out.reserve(b - a);
			for (size_t i = a; i < b; ++i)
				out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
			return out;
		}

		void AppendNote(std::string& dst, const std::string& entry)
		{
			if (entry.empty() || dst.find(entry) != std::string::npos)
				return;
			if (!dst.empty())
				dst += "; ";
			dst += entry;
		}

		// Compute body for the hybrid luma blend. Uses hb_weight from HybridBlend::WeightGlsl()
		// directly (single source of truth) and writes unclamped float output so BTB (< 0.0)
		// and WTW (> 1.0) pass through untouched. On single-plane RGB/RGBA inputs (`rgb_luma_only != 0`),
		// applies the exact HybridBlend::BlendRgb luma-only law with LumaCoeff::ForSystem weights.
		const char* const HYBRID_BLEND_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 sz = imageSize(dst_img);
if (p.x < sz.x && p.y < sz.y) {
    vec4 s = texelFetch(sharp_tex, p, 0);
    vec4 f = texelFetch(safe_tex, p, 0);
    float w = hb_weight(w_base, 0.0, 1.0, 1.0);
    vec4 o;
    if (rgb_luma_only != 0) {
        float ys = dot(s.rgb, kw);
        float yf = dot(f.rgb, kw);
        float d = (1.0 - w) * (yf - ys);
        o = vec4(s.rgb + vec3(d), s.a);
    } else {
        o = vec4(s.rgb + (1.0 - w) * (f.rgb - s.rgb), s.a);
    }
    imageStore(dst_img, p, o);
}
)GLSL";

		pl_fmt SelectBlendFormat(pl_gpu gpu, int components, bool preferFloat32, std::string& note)
		{
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			auto findFloat = [&](int bits) -> pl_fmt {
				pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, components, bits, bits, caps);
				if (!f && components == 3)
					f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, bits, bits, caps);
				return f;
			};
			if (preferFloat32)
			{
				if (pl_fmt f32 = findFloat(32))
					return f32;
				if (pl_fmt f16 = findFloat(16))
				{
					AppendNote(note, "route_4k blend: half float (32-bit float not storable)");
					return f16;
				}
				return nullptr;
			}
			if (pl_fmt f16 = findFloat(16))
			{
				AppendNote(note, "route_4k blend: half float (f16 requested)");
				return f16;
			}
			return nullptr;
		}
	}

	Mode ParseMode(const std::string& value, bool* ok)
	{
		const std::string v = NormaliseToken(value);
		if (v == "off")
		{
			if (ok) *ok = true;
			return Mode::Off;
		}
		if (v == "rebased_only")
		{
			if (ok) *ok = true;
			return Mode::RebasedOnly;
		}
		if (v == "always_4k")
		{
			if (ok) *ok = true;
			return Mode::Always4k;
		}
		if (ok) *ok = false;
		return Mode::Off;
	}

	const char* ModeName(Mode mode)
	{
		switch (mode)
		{
		case Mode::RebasedOnly: return "rebased_only";
		case Mode::Always4k: return "always_4k";
		case Mode::Off: break;
		}
		return "off";
	}

	bool ParseKernel(const std::string& token, ScalerKernels::Spec& out)
	{
		const std::string v = NormaliseToken(token);
		const auto& table = ScalerKernels::Table();
		auto pickByName = [&](const char* tableName) -> bool {
			const int idx = ScalerKernels::Find(tableName);
			if (idx < 0 || static_cast<size_t>(idx) >= table.size())
				return false;
			out = table[static_cast<size_t>(idx)].spec;
			return true;
		};
		if (v == "lanczos3") return pickByName("lanczos3");
		if (v == "lanczos2") return pickByName("lanczos2");
		if (v == "lanczos4") return pickByName("lanczos4");
		if (v == "ewa_lanczossharp") return pickByName("ewa_lanczossharp");
		if (v == "hermite") return pickByName("hermite");
		if (v == "bilinear") return pickByName("bilinear");
		if (v == "bicubic") return pickByName("bicubic C0.5 (Catmull-Rom)");
		if (v == "catmull_rom")
		{
			if (!pickByName("bicubic C0.5 (Catmull-Rom)"))
				return false;
			out.kind = ScalerKernels::Kind::CatmullRom;
			return true;
		}
		return false;
	}

	bool SameKernel(const ScalerKernels::Spec& a, const ScalerKernels::Spec& b)
	{
		if (a.kind != b.kind)
			return false;
		if (a.kind == ScalerKernels::Kind::Lanczos)
			return a.radius == b.radius;
		if (a.kind == ScalerKernels::Kind::Bicubic)
			return std::fabs(a.B - b.B) < 1e-9 && std::fabs(a.C - b.C) < 1e-9;
		return true;
	}

	DerivedGeometry DerivedGeometryRatios(int inputW, int inputH, int targetW, int targetH)
	{
		DerivedGeometry d;
		if (inputW <= 0 || inputH <= 0 || targetW <= 0 || targetH <= 0)
			return d;
		const int dstW = (std::max)(inputW, targetW);
		const int dstH = (std::max)(inputH, targetH);
		d.ratioX = static_cast<double>(dstW) / static_cast<double>(inputW);
		d.ratioY = static_cast<double>(dstH) / static_cast<double>(inputH);
		d.sourceRatioX = static_cast<double>(inputW) / static_cast<double>(dstW);
		d.sourceRatioY = static_cast<double>(inputH) / static_cast<double>(dstH);

		bool parked = false;
		const char* match = Scaling::GeometrySet::ConventionalPair(inputW, dstW, inputH, dstH, dstW, dstH, &parked);
		if (match != nullptr)
		{
			d.conventional = true;
			d.parked = parked;
			d.name = match;
			// Confirm the canonical RatioForms entry for the matched geometry on this target raster.
			std::size_t count = 0;
			const Scaling::GeometrySet::Geometry* g = Scaling::GeometrySet::Geometries(count);
			for (std::size_t i = 0; i < count; ++i)
			{
				if (g[i].name == match)
				{
					double rx[2] = {}, ry[2] = {};
					const int forms = Scaling::GeometrySet::RatioForms(g[i], dstW, dstH, rx, ry);
					for (int f = 0; f < forms; ++f)
					{
						if (Scaling::GeometrySet::Near(d.sourceRatioX, rx[f]) &&
							Scaling::GeometrySet::Near(d.sourceRatioY, ry[f]))
						{
							d.sourceRatioX = rx[f];
							d.sourceRatioY = ry[f];
							break;
						}
					}
					break;
				}
			}
		}
		return d;
	}

	PlanResult Plan(int inputW, int inputH, int deliveredW, int deliveredH, int outputW, int outputH,
		bool wasRebased, bool slotRuns, bool originalPathway, const Config& cfg)
	{
		PlanResult p;
		if (originalPathway)
		{
			p.reason = "original pathway";
			return p;
		}
		if (!slotRuns)
		{
			p.reason = "slot off";
			return p;
		}
		if (cfg.mode == Mode::Off)
		{
			p.reason = "upscale_route off";
			return p;
		}
		const int targetW = (std::max)(deliveredW > 0 ? deliveredW : 0, outputW > 0 ? outputW : 0);
		const int targetH = (std::max)(deliveredH > 0 ? deliveredH : 0, outputH > 0 ? outputH : 0);
		if (inputW <= 0 || inputH <= 0 || targetW <= 0 || targetH <= 0)
		{
			p.reason = "invalid geometry";
			return p;
		}
		const int dstW = (std::max)(inputW, targetW);
		const int dstH = (std::max)(inputH, targetH);
		p.geometry.targetW = targetW;
		p.geometry.targetH = targetH;
		p.geometry.dstW = dstW;
		p.geometry.dstH = dstH;
		p.geometry.factorX = static_cast<double>(dstW) / static_cast<double>(inputW);
		p.geometry.factorY = static_cast<double>(dstH) / static_cast<double>(inputH);
		p.geometry.derived = DerivedGeometryRatios(inputW, inputH, targetW, targetH);

		if (cfg.mode == Mode::RebasedOnly && !wasRebased)
		{
			p.reason = "not rebased (rebased_only)";
			return p;
		}
		const bool upX = targetW > inputW;
		const bool upY = targetH > inputH;
		if (!upX && !upY)
		{
			p.reason = "input already at or above target geometry";
			return p;
		}
		p.shouldRun = true;
		p.oneAxis = (upX != upY);
		p.reason = ModeName(cfg.mode);
		if (p.oneAxis)
			p.reason += " (one-axis)";
		if (p.geometry.derived.conventional && p.geometry.derived.name)
		{
			p.reason += " [";
			p.reason += p.geometry.derived.name;
			p.reason += "]";
		}
		return p;
	}

	bool WillDownstreamUpscale(const PlanResult& plan, int inputW, int inputH, int outputW, int outputH)
	{
		const float postW = static_cast<float>(plan.shouldRun ? plan.geometry.dstW : inputW);
		const float postH = static_cast<float>(plan.shouldRun ? plan.geometry.dstH : inputH);
		return AdaptiveDownscale::UpscaleAxesOf(postW, postH,
			static_cast<float>(outputW), static_cast<float>(outputH)).any;
	}

	Module::~Module()
	{
		// GPU textures belong to a device that may already be destroyed; owner calls Release(gpu).
	}

	void Module::Release(pl_gpu gpu)
	{
		sharpPass.Release(gpu);
		safePass.Release(gpu);
		for (int plane = 0; plane < PL_MAX_PLANES; ++plane)
			chromaPasses[plane].Release(gpu);
		for (int s = 0; s < kSlots; ++s)
		{
			if (blendTex[s] && gpu)
				pl_tex_destroy(gpu, &blendTex[s]);
			blendTex[s] = nullptr;
		}
	}

	FallbackCatalog::Decision Module::Fallback() const
	{
		if (!error.empty())
			return FallbackCatalog::Engaged("4K route", "4K route: off (" + error + ")");
		return FallbackCatalog::Released("4K route");
	}

	bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const PlanResult& plan,
		const Config& cfg, pl_frame& out, int slot)
	{
		error.clear();
		if (!plan.shouldRun)
		{
			formatNote.clear();
			status = "4K route: off (" + plan.reason + ")";
			out = source;
			return true;
		}
		auto fail = [this](const std::string& why) {
			error = why;
			status = "4K route: off (" + why + ")";
			++failures;
			return false;
		};
		if (!gpu || !dispatch)
			return fail("no GPU or dispatch");
		if (source.num_planes <= 0 || source.num_planes > PL_MAX_PLANES)
			return fail("invalid plane count");
		if (slot < 0 || slot >= kSlots)
			return fail("slot out of range (0..2)");

		const pl_tex lumaIn = source.planes[0].texture;
		if (!lumaIn || !lumaIn->params.sampleable || !lumaIn->params.format ||
			lumaIn->params.w <= 0 || lumaIn->params.h <= 0)
			return fail("plane 0 is not sampleable");

		const int srcW = lumaIn->params.w;
		const int srcH = lumaIn->params.h;
		const int dstW = (std::max)(srcW, plan.geometry.dstW);
		const int dstH = (std::max)(srcH, plan.geometry.dstH);
		if (dstW <= 0 || dstH <= 0)
			return fail("invalid target size");

		const double fx = static_cast<double>(dstW) / static_cast<double>(srcW);
		const double fy = static_cast<double>(dstH) / static_cast<double>(srcH);
		const ScalerPass::Precision prec = cfg.preferFloat32 ? ScalerPass::Precision::F32 : ScalerPass::Precision::F16;

		auto axisFor = [](const ScalerKernels::Spec& spec, int inW, int inH, int outW, int outH) -> ScalerPass::Axis {
			if (ScalerKernels::IsPolar(spec.kind))
				return ScalerPass::Axis::Both;
			const bool sx = outW != inW;
			const bool sy = outH != inH;
			if (sx && !sy) return ScalerPass::Axis::X;
			if (!sx && sy) return ScalerPass::Axis::Y;
			return ScalerPass::Axis::Both;
		};

		std::string note;
		pl_frame result = source;

		// Plane 0 (luma / RGB): sharp branch, alias-safe branch, or HybridBlend of both.
		const float wBase = HybridBlend::SanitizeUnit(cfg.wBase);
		const bool safeOnly = plan.oneAxis || wBase <= 0.0f;
		const bool sameBranch = SameKernel(cfg.sharpFilter, cfg.safeFilter) &&
			std::fabs(cfg.sharpAntiring - cfg.safeAntiring) < 1e-6f;
		const bool sharpOnly = !safeOnly && (wBase >= 1.0f || sameBranch);
		const ScalerPass::Source lumaSrc = {
			lumaIn, 0.0f, 0.0f, static_cast<float>(srcW), static_cast<float>(srcH)
		};

		pl_tex lumaOut = nullptr;
		const char* branchLabel = "blend";
		if (safeOnly)
		{
			ScalerPass::Settings st;
			st.filter = cfg.safeFilter;
			st.axis = axisFor(st.filter, srcW, srcH, dstW, dstH);
			st.plane = ScalerPass::Plane::Luma;
			st.precision = prec;
			st.order = ScalerPass::Order::AcrossFirst;
			st.antiring = cfg.safeAntiring;
			if (!safePass.Apply(gpu, dispatch, lumaSrc, dstW, dstH, st, lumaOut, slot))
				return fail("safe luma pass failed: " + safePass.LastError());
			AppendNote(note, safePass.Note());
			branchLabel = "safe";
		}
		else if (sharpOnly)
		{
			ScalerPass::Settings st;
			st.filter = cfg.sharpFilter;
			st.axis = axisFor(st.filter, srcW, srcH, dstW, dstH);
			st.plane = ScalerPass::Plane::Luma;
			st.precision = prec;
			st.order = ScalerPass::Order::AcrossFirst;
			st.antiring = cfg.sharpAntiring;
			if (!sharpPass.Apply(gpu, dispatch, lumaSrc, dstW, dstH, st, lumaOut, slot))
				return fail("sharp luma pass failed: " + sharpPass.LastError());
			AppendNote(note, sharpPass.Note());
			branchLabel = "sharp";
		}
		else
		{
			ScalerPass::Settings sharpSt;
			sharpSt.filter = cfg.sharpFilter;
			sharpSt.axis = axisFor(sharpSt.filter, srcW, srcH, dstW, dstH);
			sharpSt.plane = ScalerPass::Plane::Luma;
			sharpSt.precision = prec;
			sharpSt.order = ScalerPass::Order::AcrossFirst;
			sharpSt.antiring = cfg.sharpAntiring;
			pl_tex sharpTex = nullptr;
			if (!sharpPass.Apply(gpu, dispatch, lumaSrc, dstW, dstH, sharpSt, sharpTex, slot))
				return fail("sharp luma pass failed: " + sharpPass.LastError());
			AppendNote(note, sharpPass.Note());

			ScalerPass::Settings safeSt;
			safeSt.filter = cfg.safeFilter;
			safeSt.axis = axisFor(safeSt.filter, srcW, srcH, dstW, dstH);
			safeSt.plane = ScalerPass::Plane::Luma;
			safeSt.precision = prec;
			safeSt.order = ScalerPass::Order::AcrossFirst;
			safeSt.antiring = cfg.safeAntiring;
			pl_tex safeTex = nullptr;
			if (!safePass.Apply(gpu, dispatch, lumaSrc, dstW, dstH, safeSt, safeTex, slot))
				return fail("safe luma pass failed: " + safePass.LastError());
			AppendNote(note, safePass.Note());

			pl_fmt blendFmt = SelectBlendFormat(gpu, lumaIn->params.format->num_components, cfg.preferFloat32, note);
			if (!blendFmt)
				return fail("no storable float format for hybrid blend");

			pl_tex_params tp{};
			tp.w = dstW;
			tp.h = dstH;
			tp.format = blendFmt;
			tp.sampleable = true;
			tp.storable = true;
			tp.renderable = (blendFmt->caps & PL_FMT_CAP_RENDERABLE) != 0;
			tp.blit_src = (blendFmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
			tp.blit_dst = (blendFmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
			if (!pl_tex_recreate(gpu, &blendTex[slot], &tp))
				return fail("hybrid blend texture allocation failed");

			pl_shader_desc desc[3] = {};
			desc[0].desc.name = "sharp_tex";
			desc[0].desc.type = PL_DESC_SAMPLED_TEX;
			desc[0].binding.object = sharpTex;
			desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

			desc[1].desc.name = "safe_tex";
			desc[1].desc.type = PL_DESC_SAMPLED_TEX;
			desc[1].binding.object = safeTex;
			desc[1].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			desc[1].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

			desc[2].desc.name = "dst_img";
			desc[2].desc.type = PL_DESC_STORAGE_IMG;
			desc[2].desc.access = PL_DESC_ACCESS_WRITEONLY;
			desc[2].binding.object = blendTex[slot];

			const int rgbLumaOnly = (lumaIn->params.format->num_components >= 3 &&
				(source.num_planes == 1 || source.repr.sys == PL_COLOR_SYSTEM_RGB)) ? 1 : 0;
			double kwD[3] = {};
			LumaCoeff::ForSystem(source.repr.sys, source.color.primaries, kwD);
			const float kw[3] = {
				static_cast<float>(kwD[0]),
				static_cast<float>(kwD[1]),
				static_cast<float>(kwD[2])
			};

			const pl_shader_var vars[3] = {
				{ pl_var_float("w_base"), &wBase, true },
				{ pl_var_vec3("kw"), kw, true },
				{ pl_var_int("rgb_luma_only"), &rgbLumaOnly, true }
			};

			pl_custom_shader custom{};
			custom.description = "VideoProcessor 4K route: hybrid luma blend";
			custom.header = HybridBlend::WeightGlsl();
			custom.body = HYBRID_BLEND_BODY;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = desc;
			custom.num_descriptors = 3;
			custom.variables = vars;
			custom.num_variables = 3;
			custom.compute = true;
			custom.compute_group_size[0] = 16;
			custom.compute_group_size[1] = 16;

			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &custom))
			{
				if (sh) pl_dispatch_abort(dispatch, &sh);
				return fail("hybrid blend shader compilation failed");
			}
			pl_dispatch_compute_params cp{};
			cp.shader = &sh;
			cp.dispatch_size[0] = (dstW + 15) / 16;
			cp.dispatch_size[1] = (dstH + 15) / 16;
			cp.dispatch_size[2] = 1;
			if (!pl_dispatch_compute(dispatch, &cp))
			{
				if (sh) pl_dispatch_abort(dispatch, &sh);
				return fail("hybrid blend compute dispatch failed");
			}
			lumaOut = blendTex[slot];
		}
		result.planes[0].texture = lumaOut;

		// Chroma planes (plane > 0): preserve subsampling ratios and scale via ScalerPass.
		for (int plane = 1; plane < source.num_planes; ++plane)
		{
			const pl_tex cIn = source.planes[plane].texture;
			if (!cIn || !cIn->params.sampleable || !cIn->params.format ||
				cIn->params.w <= 0 || cIn->params.h <= 0)
				return fail("plane " + std::to_string(plane) + " is not sampleable");

			const int cw = (std::max)(1, static_cast<int>(std::lround(static_cast<double>(cIn->params.w) * fx)));
			const int ch = (std::max)(1, static_cast<int>(std::lround(static_cast<double>(cIn->params.h) * fy)));
			const ScalerPass::Source cSrc = {
				cIn, 0.0f, 0.0f, static_cast<float>(cIn->params.w), static_cast<float>(cIn->params.h)
			};
			ScalerPass::Settings cs;
			cs.filter = plan.oneAxis ? cfg.safeFilter : cfg.sharpFilter;
			cs.axis = axisFor(cs.filter, cIn->params.w, cIn->params.h, cw, ch);
			cs.plane = ScalerPass::Plane::Chroma;
			cs.precision = prec;
			cs.order = ScalerPass::Order::AcrossFirst;
			cs.antiring = plan.oneAxis ? cfg.safeAntiring : cfg.sharpAntiring;

			pl_tex cOut = nullptr;
			if (!chromaPasses[plane].Apply(gpu, dispatch, cSrc, cw, ch, cs, cOut, slot))
				return fail("chroma plane " + std::to_string(plane) + " failed: " + chromaPasses[plane].LastError());
			AppendNote(note, chromaPasses[plane].Note());
			result.planes[plane].texture = cOut;
		}

		// Update chroma siting shifts and crop for the resampled raster.
		for (int plane = 0; plane < result.num_planes; ++plane)
		{
			result.planes[plane].shift_x = ChromaSiting::ShiftAfterResample(source.planes[plane].shift_x, fx);
			result.planes[plane].shift_y = ChromaSiting::ShiftAfterResample(source.planes[plane].shift_y, fy);
		}
		result.crop.x0 = static_cast<float>(source.crop.x0 * fx);
		result.crop.y0 = static_cast<float>(source.crop.y0 * fy);
		result.crop.x1 = static_cast<float>(source.crop.x1 * fx);
		result.crop.y1 = static_cast<float>(source.crop.y1 * fy);

		out = result;
		formatNote = note;
		char buf[192];
		std::snprintf(buf, sizeof buf, "4K route: %s (%s), %dx%d -> %dx%d",
			ModeName(cfg.mode), branchLabel, srcW, srcH, dstW, dstH);
		status = buf;
		return true;
	}
}
