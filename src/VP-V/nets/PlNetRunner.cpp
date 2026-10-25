#include "PlNetRunner.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders.h>
#include <libplacebo/shaders/custom.h>

#include "RgbLumaDelta.h"

#include <algorithm>
#include <cmath>
#include <regex>
#include <cstdio>
#include <cstring>

// The libplacebo NetRunner (see PlNetRunner.h for the why and the shim). Compiled where libplacebo
// is linked: the renderer project (when wired) and VideoProcessor-Test.
namespace VpNets
{
	namespace
	{
		const char* FamilyName(TexFormat intermediate, int comps)
		{
			const bool half = intermediate == TexFormat::Rgba16f;
			if (comps <= 1) return half ? "r16f" : "r32f";
			if (comps == 2) return half ? "rg16f" : "rg32f";
			return half ? "rgba16f" : "rgba32f";
		}

		std::string FloatLiteral(float v)
		{
			char b[48];
			std::snprintf(b, sizeof b, "%.9g", v);
			std::string s = b;
			if (s.find_first_of(".eE") == std::string::npos) s += ".0";
			return s;
		}
	}

	struct PlNetRunner::Impl
	{
		pl_gpu gpu = nullptr;
		NetPackage pkg;
		NetPrepareParams params;
		std::map<std::string, pl_tex> pool;      // SAVE name / "@stage:<hook>" -> texture (kept across frames)
		pl_tex zeroMask = nullptr;
		std::vector<pl_timer> timers;            // one per pass
		std::map<std::string, pl_tex> current;   // this run's name -> texture (inputs, saves, stage writes)
		std::map<std::string, float> currentMul;
		std::string outputKey;
		bool prepared = false;
		// Fusion plan: layer per first pass index.
		std::vector<ConvLayer> layers;
		std::vector<FusedShader> fused;
		std::map<size_t, size_t> layerAt;
		bool fp16Storage = false;

		void Destroy()
		{
			if (!gpu) return;
			for (auto& kv : pool) pl_tex_destroy(gpu, &kv.second);
			pool.clear();
			pl_tex_destroy(gpu, &zeroMask);
			for (pl_timer& t : timers) pl_timer_destroy(gpu, &t);
			timers.clear();
			current.clear();
			currentMul.clear();
		}

		// mpv binding order: HOOKED = the hooked stage; a stage name = its current texture (a pass
		// without SAVE may have replaced it); else a saved texture or an input.
		template <typename Map>
		static typename Map::const_iterator Find(const Map& m, const std::string& name, const std::string& hook)
		{
			if (name == "HOOKED") return m.find("@stage:" + hook);
			const auto st = m.find("@stage:" + name);
			return st != m.end() ? st : m.find(name);
		}
		pl_tex Lookup(const std::string& name, const std::string& hook) const
		{
			const auto it = Find(current, name, hook);
			return it == current.end() ? nullptr : it->second;
		}
		float MulOf(const std::string& name, const std::string& hook) const
		{
			const auto it = Find(currentMul, name, hook);
			return it == currentMul.end() ? 1.0f : it->second;
		}

		// A pooled pass texture of the right size and format.
		pl_tex Texture(const std::string& key, int w, int h, pl_fmt fmt, bool compute)
		{
			// The usage is part of the key: a fragment pass needs a renderable texture, a compute pass
			// a storable one, and a fused layer falling back to its exported passes must not get the
			// other kind (audit 2026-10-24).
			pl_tex& t = pool[key + (compute ? "#storage" : "#render")];
			if (t && t->params.w == w && t->params.h == h && t->params.format == fmt)
				return t;
			pl_tex_destroy(gpu, &t);
			pl_tex_params p{};
			p.w = w;
			p.h = h;
			p.format = fmt;
			p.sampleable = true;
			p.renderable = !compute && (fmt->caps & PL_FMT_CAP_RENDERABLE);
			p.storable = compute && (fmt->caps & PL_FMT_CAP_STORABLE);
			p.blit_src = (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
			t = pl_tex_create(gpu, &p);
			return t;
		}

		pl_fmt PassFormat(const NetPass& pass, bool isLast) const
		{
			const unsigned need = static_cast<unsigned>(PL_FMT_CAP_SAMPLEABLE) | static_cast<unsigned>(pass.IsCompute() ? PL_FMT_CAP_STORABLE : PL_FMT_CAP_RENDERABLE);
			auto ok = [&](pl_fmt f) { return f && (static_cast<unsigned>(f->caps) & need) == need; };
			if (isLast)
			{
				// The net output keeps full precision: fp16 has ~0.5 code steps near white.
				if (params.output == TexFormat::R16Unorm)
				{
					pl_fmt f = pl_find_named_fmt(gpu, "r16");
					if (ok(f)) return f;
				}
				pl_fmt f = pl_find_named_fmt(gpu, FamilyName(TexFormat::Rgba32f, pass.components));
				if (ok(f)) return f;
				f = pl_find_named_fmt(gpu, "rgba32f");
				return ok(f) ? f : nullptr;
			}
			pl_fmt f = pl_find_named_fmt(gpu, FamilyName(params.intermediate, pass.components));
			if (ok(f)) return f;
			f = pl_find_named_fmt(gpu, FamilyName(params.intermediate, 4));
			return ok(f) ? f : nullptr;
		}
	};

	// One fused layer: 1 = ran, 0 = not fusable this frame (sizes), -1 = error.
	int PlNetRunner::RunFusedLayer(const ConvLayer& L, const FusedShader& fs, void* dispatchPtr, void* timerPtr, std::string* error)
	{
		Impl& m = *impl;
		const pl_dispatch dispatch = static_cast<pl_dispatch>(dispatchPtr);
		const pl_tex hooked = m.Lookup("HOOKED", L.hook);
		if (!hooked) return 0;
		int w = hooked->params.w, h = hooked->params.h;
		if (!L.width.empty())
		{
			auto lookup = [&](const std::string& n, char wh, int& v)
			{
				const pl_tex t = m.Lookup(n, L.hook);
				if (!t) return false;
				v = wh == 'w' ? t->params.w : t->params.h;
				return true;
			};
			if (!EvalSizeExpr(L.width, lookup, w, nullptr) || !EvalSizeExpr(L.height, lookup, h, nullptr)) return 0;
		}
		std::vector<pl_tex> ins;
		for (const std::string& n : L.inputs)
		{
			const pl_tex t = m.Lookup(n, L.hook);
			if (!t || t->params.w != w || t->params.h != h || m.MulOf(n, L.hook) != 1.0f) return 0;
			ins.push_back(t);
		}
		pl_tex gate = nullptr;
		if (!L.gate.empty() && !(gate = m.Lookup(L.gate, L.hook))) return 0;
		const pl_fmt fmt = pl_find_named_fmt(m.gpu, m.fp16Storage || m.params.intermediate == TexFormat::Rgba16f ? "rgba16f" : "rgba32f");
		std::vector<pl_tex> outs;
		for (const std::string& o : L.outputs)
		{
			const pl_tex t = m.Texture(o, w, h, fmt, true);
			if (!t)
			{
				if (error) *error = m.pkg.name + ": fused layer: texture creation failed";
				return -1;
			}
			outs.push_back(t);
		}
		std::vector<std::string> names;
		names.reserve(ins.size() + outs.size() + 1);
		std::vector<pl_shader_desc> descs;
		for (size_t g = 0; g < ins.size(); ++g)
		{
			names.push_back("NFI" + std::to_string(g) + "_raw");
			pl_shader_desc d{};
			d.desc.name = names.back().c_str();
			d.desc.type = PL_DESC_SAMPLED_TEX;
			d.binding.object = ins[g];
			d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			d.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			descs.push_back(d);
		}
		if (gate)
		{
			pl_shader_desc d{};
			d.desc.name = "NFG_raw";
			d.desc.type = PL_DESC_SAMPLED_TEX;
			d.binding.object = gate;
			d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			d.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			descs.push_back(d);
		}
		for (size_t o = 0; o < outs.size(); ++o)
		{
			names.push_back("NFO" + std::to_string(o));
			pl_shader_desc d{};
			d.desc.name = names.back().c_str();
			d.desc.type = PL_DESC_STORAGE_IMG;
			d.desc.access = PL_DESC_ACCESS_WRITEONLY;
			d.binding.object = outs[o];
			descs.push_back(d);
		}
		const int size[2] = { w, h };
		pl_shader_var sv{};
		sv.var = pl_var_ivec2("nf_size");
		sv.data = size;
		pl_custom_shader cs{};
		cs.header = fs.glsl.c_str();
		cs.description = "VP fused conv layer";
		cs.body = "hook();";
		cs.input = PL_SHADER_SIG_NONE;
		cs.output = PL_SHADER_SIG_NONE;
		cs.descriptors = descs.data();
		cs.num_descriptors = static_cast<int>(descs.size());
		cs.variables = &sv;
		cs.num_variables = 1;
		cs.compute = true;
		cs.compute_shmem = fs.sharedBytes;
		cs.compute_group_size[0] = fs.groupX;
		cs.compute_group_size[1] = fs.groupY;
		pl_shader sh = pl_dispatch_begin(dispatch);
		if (!sh || !pl_shader_custom(sh, &cs))
		{
			if (sh) pl_dispatch_abort(dispatch, &sh);
			if (error) *error = m.pkg.name + ": fused layer refused by pl_shader_custom";
			return -1;
		}
		pl_dispatch_compute_params dp{};
		dp.shader = &sh;
		dp.dispatch_size[0] = (w + fs.tileX - 1) / fs.tileX;
		dp.dispatch_size[1] = (h + fs.tileY - 1) / fs.tileY;
		dp.dispatch_size[2] = 1;
		dp.timer = static_cast<pl_timer>(timerPtr);
		if (!pl_dispatch_compute(dispatch, &dp))
		{
			if (error) *error = m.pkg.name + ": fused layer dispatch failed";
			return -1;
		}
		for (size_t o = 0; o < outs.size(); ++o)
		{
			m.current[L.outputs[o]] = outs[o];
			m.currentMul[L.outputs[o]] = 1.0f;
		}
		return 1;
	}

	PlNetRunner::PlNetRunner() : impl(new Impl()) {}
	PlNetRunner::~PlNetRunner() { Release(); }

	NetCapabilities PlNetRunner::Capabilities(const NetDeviceContext& device) const
	{
		NetCapabilities c;
		c.fragment = false;
		const pl_gpu gpu = static_cast<pl_gpu>(device.gpu);
		if (!gpu || !device.dispatch) return c;
		c.fragment = true;
		c.compute = gpu->glsl.compute;
		c.maxTextureSize = static_cast<int>(gpu->limits.max_tex_2d_dim);
		auto probe = [&](const char* name, TexFormat f, bool& renderable, bool& storable, bool& linear)
		{
			const bool off = std::find(disabled.begin(), disabled.end(), f) != disabled.end();
			const pl_fmt fmt = off ? nullptr : pl_find_named_fmt(gpu, name);
			renderable = fmt && (fmt->caps & PL_FMT_CAP_RENDERABLE) && (fmt->caps & PL_FMT_CAP_SAMPLEABLE);
			storable = fmt && (fmt->caps & PL_FMT_CAP_STORABLE);
			linear = fmt && (fmt->caps & PL_FMT_CAP_LINEAR);
		};
		// A family counts only if every width the passes use (r, rg, rgba) has the capability; a
		// compute pass falls back to rgba when r/rg is not storable, so storable is judged on rgba.
		auto family = [&](const char* r, const char* rg, const char* rgba, TexFormat f, bool& ren, bool& sto, bool& lin)
		{
			bool r1, s1, l1, r2, s2, l2, r4, s4, l4;
			probe(r, f, r1, s1, l1);
			probe(rg, f, r2, s2, l2);
			probe(rgba, f, r4, s4, l4);
			ren = r1 && r2 && r4;
			sto = s4;
			lin = l1 && l2 && l4;
		};
		family("r16f", "rg16f", "rgba16f", TexFormat::Rgba16f, c.renderableRgba16f, c.storableRgba16f, c.linearRgba16f);
		family("r32f", "rg32f", "rgba32f", TexFormat::Rgba32f, c.renderableRgba32f, c.storableRgba32f, c.linearRgba32f);
		return c;
	}

	bool PlNetRunner::Prepare(const NetDeviceContext& device, const NetPackage& pkg, const NetPrepareParams& params, std::string* error)
	{
		Release();
		Impl& m = *impl;
		m.gpu = static_cast<pl_gpu>(device.gpu);
		if (!m.gpu || !device.dispatch)
		{
			if (error) *error = "no libplacebo gpu/dispatch";
			return false;
		}
		const CapabilityResult caps = CheckCapabilities(pkg, Capabilities(device));
		if (!caps.ok)
		{
			if (error) *error = caps.reason;
			return false;
		}
		if (params.intermediate != TexFormat::Rgba16f && params.intermediate != TexFormat::Rgba32f)
		{
			if (error) *error = "intermediates must be rgba16f or rgba32f";
			return false;
		}
		// Fused fp16 storage lowers the intermediate format: only where the fingerprint allows it.
		if (fusion && fusedFp16 && EffectiveIntermediate(pkg, params.intermediate) != TexFormat::Rgba16f)
		{
			if (error) *error = "fused fp16 storage refused: the fingerprint does not allow fp16 intermediates";
			return false;
		}
		m.pkg = pkg;
		m.params = params;
		// HLSL tanh can return NaN for |x| above ~44; tanh(15) is already 1 in float, so clamping the
		// argument changes nothing else (audit 2026-10-24).
		{
			static const std::regex kTanh(R"(tanh\(([^()]*)\))");
			for (NetPass& p : m.pkg.passes)
				p.body = std::regex_replace(p.body, kTanh, "tanh(clamp($1, -15.0, 15.0))");
		}
		m.layers.clear();
		m.fused.clear();
		m.layerAt.clear();
		m.fp16Storage = fusedFp16;
		if (fusion && m.gpu->glsl.compute)
		{
			const pl_fmt store = pl_find_named_fmt(m.gpu, fusedFp16 ? "rgba16f" : (params.intermediate == TexFormat::Rgba16f ? "rgba16f" : "rgba32f"));
			const bool storable = store && (store->caps & PL_FMT_CAP_STORABLE) && (store->caps & PL_FMT_CAP_SAMPLEABLE);
			for (const ConvLayer& L : ExtractConvLayers(pkg))
			{
				FusedShader fs = FusedLayerGlsl(L, fusedKind, fusedFp16);
				// Stays exported when the tile does not fit shared memory or the layer has more outputs
				// than D3D11's 8 compute UAV slots.
				if (!storable || fs.sharedBytes > m.gpu->glsl.max_shmem_size || L.outputs.size() > 8) continue;
				m.layerAt[L.passes.front()] = m.layers.size();
				m.layers.push_back(L);
				m.fused.push_back(fs);
			}
		}
		for (size_t i = 0; i < pkg.passes.size(); ++i)
			m.timers.push_back(pl_timer_create(m.gpu));   // may be null (no timer support): safe
		m.prepared = true;
		return true;
	}

	void PlNetRunner::Release()
	{
		impl->Destroy();
		impl->prepared = false;
	}

	size_t PlNetRunner::FusedLayers() const { return impl->layers.size(); }

	TexFormat PlNetRunner::EffectiveIntermediate(const NetPackage& pkg, TexFormat chosen) const
	{
		if (!fusion || !fusedFp16) return chosen;
		const NetFingerprint& fp = pkg.fingerprint;
		const bool fp16Ok = fp.intermediate == TexFormat::Rgba16f || (fp.intermediate == TexFormat::Rgba32f && fp.fp16Allowed);
		return fp16Ok ? TexFormat::Rgba16f : TexFormat::Unknown;   // Unknown: never matches a fingerprint
	}

	void* PlNetRunner::Intermediate(const std::string& name) const
	{
		if (name.empty())
		{
			const auto it = impl->current.find(impl->outputKey);
			return it == impl->current.end() ? nullptr : const_cast<pl_tex_t*>(it->second);
		}
		const auto it = impl->current.find(name);
		return it == impl->current.end() ? nullptr : const_cast<pl_tex_t*>(it->second);
	}

	NetDeviceContext PlNetRunner::DeviceFromHook(const pl_hook_params* params)
	{
		NetDeviceContext d;
		if (params)
		{
			d.gpu = const_cast<pl_gpu_t*>(params->gpu);
			d.dispatch = params->dispatch;
		}
		return d;
	}

	bool PlNetRunner::RunInPostKernelHook(const pl_hook_params* params, const NetTexture& scalerSrc, NetTexture& output, std::string* error,
		const PostKernelOptions& opt)
	{
		if (!params || !params->tex || !scalerSrc.Valid())
		{
			if (error) *error = "POST_KERNEL: no hook texture or scaler source";
			return false;
		}
		const NetDeviceContext device = DeviceFromHook(params);
		// The picture is the destination rect's size at the texture origin (AdaptiveDownscale::Blending).
		const int w = static_cast<int>(std::abs(params->dst_rect.x1 - params->dst_rect.x0));
		const int h = static_cast<int>(std::abs(params->dst_rect.y1 - params->dst_rect.y0));
		if (w <= 0 || h <= 0 || params->tex->params.w < w || params->tex->params.h < h)
		{
			if (error) *error = "POST_KERNEL: the hook texture is smaller than the destination rect";
			return false;
		}
		NetTexture so;
		so.handle = const_cast<pl_tex_t*>(params->tex);
		so.width = params->tex->params.w;
		so.height = params->tex->params.h;
		so.components = params->tex->params.format->num_components;
		so.mul = opt.mulOut;
		so.rect[2] = static_cast<float>(w);
		so.rect[3] = static_cast<float>(h);
		NetTexture ss = scalerSrc;
		ss.mul = opt.mulSrc;
		if (so.components == 1 && ss.components == 1)
		{
			NetRunInputs in;
			in.textures["SCALER_OUT"] = so;
			in.textures["SCALER_SRC"] = ss;
			return Run(device, in, output, error);
		}
		// RGB: luma of both (the trained range), D on luma, the luma delta onto R, G and B.
		auto prepareHelper = [&](std::unique_ptr<PlNetRunner>& r, const char* text, const char* name) -> bool
		{
			if (r) return true;
			ParseResult pr = ParseShaderText(text, name);
			if (!pr.ok)
			{
				if (error) *error = pr.error;
				return false;
			}
			r.reset(new PlNetRunner());
			NetPrepareParams pp;
			pp.intermediate = TexFormat::Rgba32f;
			// The helpers carry no fingerprint: prepared directly (they are VP's own passes).
			pr.package.fingerprint.intermediate = TexFormat::Rgba32f;
			return r->Prepare(device, pr.package, pp, error);
		};
		if (!prepareHelper(lumaRunner, kLumaFromRgbPass, "vp_luma_from_rgb") || !prepareHelper(srcLumaRunner, kLumaFromRgbPass, "vp_luma_from_rgb_src") ||
			!prepareHelper(deltaRunner, kRgbLumaDeltaPass, "vp_rgb_luma_delta"))
			return false;
		const std::map<std::string, double> lumaParams = { { "vp_kr", opt.kr }, { "vp_kb", opt.kb }, { "vp_range_scale", opt.rangeScale },
			{ "vp_range_offset", opt.rangeOffset } };
		NetTexture yOut, ySrc, yNet;
		{
			NetRunInputs in;
			in.textures["HOOKED"] = so;
			in.params = lumaParams;
			if (!lumaRunner->Run(device, in, yOut, error)) return false;
		}
		if (ss.components == 1)
			ySrc = ss;
		else
		{
			NetRunInputs in;
			in.textures["HOOKED"] = ss;
			in.params = lumaParams;
			if (!srcLumaRunner->Run(device, in, ySrc, error)) return false;
		}
		{
			NetRunInputs in;
			in.textures["SCALER_OUT"] = yOut;
			in.textures["SCALER_SRC"] = ySrc;
			if (!Run(device, in, yNet, error)) return false;
		}
		NetRunInputs in;
		in.textures["HOOKED"] = so;
		in.textures["LUMA_BEFORE"] = yOut;
		in.textures["LUMA_AFTER"] = yNet;
		in.params["vp_delta_scale"] = opt.rangeScale != 0.0f ? 1.0 / opt.rangeScale : 1.0;
		return deltaRunner->Run(device, in, output, error);
	}

	bool PlNetRunner::Run(const NetDeviceContext& device, const NetRunInputs& in, NetTexture& output, std::string* error)
	{
		Impl& m = *impl;
		if (!m.prepared)
		{
			if (error) *error = "not prepared";
			return false;
		}
		const pl_dispatch dispatch = static_cast<pl_dispatch>(device.dispatch);
		if (static_cast<pl_gpu>(device.gpu) != m.gpu || !dispatch)
		{
			if (error) *error = "the call site's gpu is not the one the net was prepared on";
			return false;
		}
		const ResolvedInputs res = ResolveInputs(m.pkg, in);
		if (!res.ok || res.bypass)
		{
			if (error) *error = res.bypass ? "bypass: " + res.reason : res.reason;
			return false;
		}

		// The timers' latest results are from earlier runs (queries are asynchronous).
		{
			uint64_t ns = 0;
			for (pl_timer t : m.timers)
			{
				uint64_t v = 0, q;
				while ((q = pl_timer_query(m.gpu, t)) != 0) v = q;   // drain: keep the most recent
				ns += v;
			}
			if (ns) RecordTiming(static_cast<double>(ns) * 1e-6);
		}

		m.current.clear();
		m.currentMul.clear();
		const bool postKernel = m.pkg.contract.site == RunSite::PostKernelHook;
		for (const auto& kv : in.textures)
			if (kv.second.Valid())
			{
				pl_tex t = static_cast<pl_tex>(kv.second.handle);
				// A picture that occupies only part of its texture (a pooled hook texture, a crop): the
				// exported passes mirror at the TEXTURE edge, so the picture is copied to an exact-size
				// texture first. Fractional crops cannot be copied exactly: refused, visibly.
				if (kv.second.HasRect())
				{
					const float* r = kv.second.rect;
					const int x0 = static_cast<int>(std::lround(r[0])), y0 = static_cast<int>(std::lround(r[1]));
					const int x1 = static_cast<int>(std::lround(r[2])), y1 = static_cast<int>(std::lround(r[3]));
					if (std::fabs(r[0] - x0) > 1e-3f || std::fabs(r[1] - y0) > 1e-3f || std::fabs(r[2] - x1) > 1e-3f || std::fabs(r[3] - y1) > 1e-3f ||
						x0 < 0 || y0 < 0 || x1 > t->params.w || y1 > t->params.h || x1 <= x0 || y1 <= y0)
					{
						if (error) *error = kv.first + ": a fractional or out-of-range picture rect is not supported (exact copy impossible)";
						return false;
					}
					if (x0 != 0 || y0 != 0 || x1 != t->params.w || y1 != t->params.h)
					{
						if (!(t->params.format->caps & PL_FMT_CAP_BLITTABLE))
						{
							if (error) *error = kv.first + ": its format cannot be copied to crop the picture rect";
							return false;
						}
						pl_tex& c = m.pool["@crop:" + kv.first];
						if (!c || c->params.w != x1 - x0 || c->params.h != y1 - y0 || c->params.format != t->params.format)
						{
							pl_tex_destroy(m.gpu, &c);
							pl_tex_params cp{};
							cp.w = x1 - x0;
							cp.h = y1 - y0;
							cp.format = t->params.format;
							cp.sampleable = true;
							cp.blit_dst = true;
							c = pl_tex_create(m.gpu, &cp);
						}
						if (!c)
						{
							if (error) *error = kv.first + ": crop texture creation failed";
							return false;
						}
						pl_tex_blit_params bp{};
						bp.src = t;
						bp.dst = c;
						bp.src_rc = { x0, y0, 0, x1, y1, 1 };
						bp.dst_rc = { 0, 0, 0, x1 - x0, y1 - y0, 1 };
						pl_tex_blit(m.gpu, &bp);
						t = c;
					}
				}
				m.current[kv.first] = t;
				m.currentMul[kv.first] = kv.second.mul;
			}
		const std::string hookedName = postKernel ? "SCALER_OUT" : "HOOKED";
		for (const NetPass& p : m.pkg.passes)
		{
			const std::string src = p.hook == "SCALER_SRC" ? "SCALER_SRC" : hookedName;
			const auto it = in.textures.find(src);
			if (it == in.textures.end() || !m.current.count(src))
			{
				if (error) *error = "no " + src + " texture for the hooked stage " + p.hook;
				return false;
			}
			m.current["@stage:" + p.hook] = m.current[src];
			m.currentMul["@stage:" + p.hook] = it->second.mul;
		}
		if (res.neighbourFallback)
		{
			const pl_tex hooked = m.current[hookedName];
			pl_fmt f = pl_find_named_fmt(m.gpu, FamilyName(m.params.intermediate, 2));
			if (!f) f = pl_find_named_fmt(m.gpu, FamilyName(m.params.intermediate, 4));
			if (!f)
			{
				if (error) *error = "no texture format for A_motion's zero mask";
				return false;
			}
			if (!m.zeroMask || m.zeroMask->params.w != hooked->params.w || m.zeroMask->params.h != hooked->params.h)
			{
				pl_tex_destroy(m.gpu, &m.zeroMask);
				std::vector<uint8_t> zeros(static_cast<size_t>(hooked->params.w) * hooked->params.h * f->texel_size, 0);
				pl_tex_params p{};
				p.w = hooked->params.w;
				p.h = hooked->params.h;
				p.format = f;
				p.sampleable = true;
				p.initial_data = zeros.data();
				m.zeroMask = pl_tex_create(m.gpu, &p);
			}
			if (!m.zeroMask)
			{
				if (error) *error = "A_motion's zero mask could not be created";
				return false;
			}
			m.current["VPNN_PREV"] = hooked;
			m.current["VPNN_NEXT"] = hooked;
			m.current["VPNN_MASK"] = m.zeroMask;
			m.currentMul["VPNN_PREV"] = m.currentMul["VPNN_NEXT"] = m.currentMul[hookedName];
			m.currentMul["VPNN_MASK"] = 1.0f;
		}

		for (size_t i = 0; i < m.pkg.passes.size(); ++i)
		{
			const auto fusedHere = m.layerAt.find(i);
			if (fusedHere != m.layerAt.end())
			{
				int result = RunFusedLayer(m.layers[fusedHere->second], m.fused[fusedHere->second], dispatch, m.timers[i], error);
				if (result < 0) return false;
				if (result > 0)
				{
					i = m.layers[fusedHere->second].passes.back();
					continue;
				}
				// result 0: the layer's sizes do not allow fusion this frame; run the exported passes.
			}
			const NetPass& pass = m.pkg.passes[i];
			const bool isLast = i + 1 == m.pkg.passes.size();
			auto fail = [&](const std::string& why)
			{
				if (error) *error = m.pkg.name + ": pass " + std::to_string(i) + " '" + pass.desc + "': " + why;
				return false;
			};
			const pl_tex hooked = m.Lookup("HOOKED", pass.hook);
			if (!hooked) return fail("hooked stage " + pass.hook + " not bound");
			int w = hooked->params.w, h = hooked->params.h;
			if (!pass.width.empty())
			{
				auto lookup = [&](const std::string& n, char wh, int& v)
				{
					const pl_tex t = m.Lookup(n, pass.hook);
					if (!t) return false;
					v = wh == 'w' ? t->params.w : t->params.h;
					return true;
				};
				std::string err;
				if (!EvalSizeExpr(pass.width, lookup, w, &err) || !EvalSizeExpr(pass.height, lookup, h, &err)) return fail(err);
			}
			if (w <= 0 || h <= 0 || static_cast<uint32_t>(w) > m.gpu->limits.max_tex_2d_dim || static_cast<uint32_t>(h) > m.gpu->limits.max_tex_2d_dim) return fail("unsupported pass size");
			const pl_fmt fmt = m.PassFormat(pass, isLast);
			if (!fmt) return fail("no usable texture format for this pass");
			const std::string key = pass.save.empty() ? "@out:" + pass.hook + (isLast ? "" : "#" + std::to_string(i)) : pass.save;
			const pl_tex target = m.Texture(key, w, h, fmt, pass.IsCompute());
			if (!target) return fail("texture creation failed");

			// Descriptors (inputs), variables (out size, labels) and the mpv macros.
			std::vector<pl_shader_desc> descs;
			std::vector<std::string> names;          // keep the strings alive for the descriptors
			names.reserve(pass.binds.size() + 2);
			std::string header;
			header += pass.IsCompute() ? "#define VPNN_POS ((vec2(gl_GlobalInvocationID.xy) + vec2(0.5)) / vpnn_out_size)\n"
				: "#define VPNN_POS (gl_FragCoord.xy / vpnn_out_size)\n";
			for (const std::string& b : pass.binds)
			{
				const pl_tex t = m.Lookup(b, pass.hook);
				if (!t) return fail("texture " + b + " not bound");
				names.push_back(b + "_raw");
				pl_shader_desc d{};
				d.desc.name = names.back().c_str();
				d.desc.type = PL_DESC_SAMPLED_TEX;
				d.binding.object = t;
				d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
				const bool filterable = (t->params.format->caps & PL_FMT_CAP_LINEAR) != 0;
				if (!filterable && pass.body.find(b + "_tex(") != std::string::npos)
					return fail(b + " is sampled bilinearly (" + b + "_tex) but its format is not linear-filterable");
				d.binding.sample_mode = filterable ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;   // NEAREST only where unsampled
				descs.push_back(d);
				const float mul = m.MulOf(b, pass.hook);
				header += "#define " + b + "_pos VPNN_POS\n";
				header += "#define " + b + "_size vec2(textureSize(" + b + "_raw, 0))\n";
				header += "#define " + b + "_pt (vec2(1.0) / " + b + "_size)\n";
				header += "#define " + b + "_mul " + FloatLiteral(mul) + "\n";
				header += "#define " + b + "_tex(pos) (" + b + "_mul * textureLod(" + b + "_raw, pos, 0.0))\n";
				header += "#define " + b + "_texOff(off) " + b + "_tex(" + b + "_pos + " + b + "_pt * vec2(off))\n";
			}
			if (pass.IsCompute())
			{
				pl_shader_desc d{};
				d.desc.name = "out_image";
				d.desc.type = PL_DESC_STORAGE_IMG;
				d.desc.access = PL_DESC_ACCESS_WRITEONLY;
				d.binding.object = target;
				descs.push_back(d);
			}
			std::vector<pl_shader_var> vars;
			const float outSize[2] = { static_cast<float>(w), static_cast<float>(h) };
			pl_shader_var sv{};
			sv.var = pl_var_vec2("vpnn_out_size");
			sv.data = outSize;
			vars.push_back(sv);
			std::vector<float> fvals(m.pkg.params.size());
			std::vector<int> ivals(m.pkg.params.size());
			for (size_t k = 0; k < m.pkg.params.size(); ++k)
			{
				const NetParam& prm = m.pkg.params[k];
				if (pass.body.find(prm.name) == std::string::npos) continue;
				const auto pv = in.params.find(prm.name);
				const double val = pv != in.params.end() ? pv->second : prm.defaultValue;
				pl_shader_var v{};
				if (prm.isInt)
				{
					ivals[k] = static_cast<int>(std::lround(val));
					v.var = pl_var_int(prm.name.c_str());
					v.data = &ivals[k];
				}
				else
				{
					fvals[k] = static_cast<float>(val);
					v.var = pl_var_float(prm.name.c_str());
					v.data = &fvals[k];
				}
				vars.push_back(v);
			}
			const std::string fullHeader = header + pass.body;
			if (isLast && m.params.outputScale != 1.0f && pass.IsCompute())
				return fail("an output scale needs a fragment output pass");
			const std::string scaledBody = "color = hook() * " + FloatLiteral(m.params.outputScale) + ";";
			pl_custom_shader cs{};
			cs.header = fullHeader.c_str();
			cs.description = pass.desc.c_str();
			cs.input = PL_SHADER_SIG_NONE;
			cs.descriptors = descs.data();
			cs.num_descriptors = static_cast<int>(descs.size());
			cs.variables = vars.data();
			cs.num_variables = static_cast<int>(vars.size());
			if (pass.IsCompute())
			{
				cs.compute = true;
				cs.compute_group_size[0] = pass.computeX;
				cs.compute_group_size[1] = pass.computeY;
				cs.output = PL_SHADER_SIG_NONE;
				cs.body = "hook();";
			}
			else
			{
				cs.output = PL_SHADER_SIG_COLOR;
				cs.body = (isLast && m.params.outputScale != 1.0f) ? scaledBody.c_str() : "color = hook();";
			}
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!sh) return fail("pl_dispatch_begin failed");
			if (!pl_shader_custom(sh, &cs))
			{
				pl_dispatch_abort(dispatch, &sh);
				return fail("pl_shader_custom refused the pass (compute or shared memory unsupported?)");
			}
			bool ok;
			if (pass.IsCompute())
			{
				pl_dispatch_compute_params dp{};
				dp.shader = &sh;
				dp.dispatch_size[0] = (w + pass.computeX - 1) / pass.computeX;
				dp.dispatch_size[1] = (h + pass.computeY - 1) / pass.computeY;
				dp.dispatch_size[2] = 1;
				dp.width = w;
				dp.height = h;
				dp.timer = m.timers[i];
				ok = pl_dispatch_compute(dispatch, &dp);
			}
			else
			{
				pl_dispatch_params dp{};
				dp.shader = &sh;
				dp.target = target;
				dp.timer = m.timers[i];
				ok = pl_dispatch_finish(dispatch, &dp);
			}
			if (!ok) return fail("dispatch failed (shader compile or execution)");
			if (pass.save.empty())
			{
				m.current["@stage:" + pass.hook] = target;
				m.currentMul["@stage:" + pass.hook] = 1.0f;
				if (isLast)
				{
					m.outputKey = "@stage:" + pass.hook;
					m.current["@output"] = target;
				}
			}
			else
			{
				m.current[pass.save] = target;
				m.currentMul[pass.save] = 1.0f;
			}
		}
		const pl_tex out = m.current[m.outputKey];
		output = NetTexture();
		output.handle = const_cast<pl_tex_t*>(out);
		output.width = out->params.w;
		output.height = out->params.h;
		output.components = out->params.format->num_components;
		output.format = m.params.output == TexFormat::R16Unorm ? TexFormat::R16Unorm : (output.components == 1 ? TexFormat::R32f : TexFormat::Rgba32f);
		return true;
	}
}
