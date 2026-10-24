#include "CpuNetExecutor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace VpNets
{
	float RoundToHalf(float v)
	{
		if (!std::isfinite(v)) return v;
		const float a = std::fabs(v);
		if (a >= 65520.0f) return v > 0 ? INFINITY : -INFINITY;   // above the largest half (65504) rounds to inf
		if (a < 6.103515625e-05f)                                   // subnormal halves: a fixed step of 2^-24
		{
			const float step = 5.9604644775390625e-08f;
			return std::nearbyint(v / step) * step;
		}
		int e = 0;
		std::frexp(a, &e);                                          // a = m * 2^e, m in [0.5, 1)
		const float step = std::ldexp(1.0f, e - 11);                // 10 mantissa bits after the leading one
		return std::nearbyint(v / step) * step;
	}

	CpuNetExecutor::CpuNetExecutor() {}
	CpuNetExecutor::~CpuNetExecutor() {}

	NetCapabilities CpuNetExecutor::Capabilities(const NetDeviceContext&) const
	{
		NetCapabilities c;
		c.fragment = c.compute = true;
		c.renderableRgba16f = c.renderableRgba32f = true;
		c.storableRgba16f = c.storableRgba32f = true;
		c.linearRgba16f = c.linearRgba32f = true;
		return c;
	}

	bool CpuNetExecutor::Prepare(const NetDeviceContext&, const NetPackage& package, const NetPrepareParams& p, std::string* error)
	{
		Release();
		pkg = package;
		params = p;
		for (const NetPass& pass : pkg.passes)
		{
			std::unique_ptr<Glsl::Program> prog(new Glsl::Program());
			std::string err;
			if (!prog->Parse(pass.body, &err))
			{
				if (error) *error = pkg.name + ": pass '" + pass.desc + "': " + err;
				return false;
			}
			if (!prog->HasFunction("hook"))
			{
				if (error) *error = pkg.name + ": pass '" + pass.desc + "' has no hook()";
				return false;
			}
			programs.push_back(std::move(prog));
		}
		prepared = true;
		return true;
	}

	void CpuNetExecutor::Release()
	{
		programs.clear();
		saved.clear();
		stageWrites.clear();
		stages.clear();
		inputs.clear();
		prepared = false;
	}

	const CpuTexture* CpuNetExecutor::Texture(const std::string& name) const
	{
		const auto it = saved.find(name);
		return it == saved.end() ? nullptr : &it->second;
	}

	const CpuTexture* CpuNetExecutor::Lookup(const std::string& name, const std::string& hook) const
	{
		if (name == "HOOKED")
		{
			const auto st = stages.find(hook);
			return st == stages.end() ? nullptr : st->second;
		}
		const auto s = saved.find(name);
		if (s != saved.end()) return &s->second;
		const auto st = stages.find(name);
		if (st != stages.end()) return st->second;
		const auto in = inputs.find(name);
		if (in != inputs.end()) return in->second;
		return nullptr;
	}

	bool CpuNetExecutor::Run(const NetDeviceContext&, const NetRunInputs& in, NetTexture& out, std::string* error)
	{
		if (!prepared)
		{
			if (error) *error = "not prepared";
			return false;
		}
		const auto t0 = std::chrono::steady_clock::now();
		const ResolvedInputs res = ResolveInputs(pkg, in);
		if (!res.ok || res.bypass)
		{
			if (error) *error = res.bypass ? "bypass: " + res.reason : res.reason;
			return false;
		}
		saved.clear();
		stageWrites.clear();
		stages.clear();
		inputs.clear();
		for (const auto& kv : in.textures)
			if (kv.second.Valid())
				inputs[kv.first] = static_cast<const CpuTexture*>(kv.second.handle);
		const bool postKernel = pkg.contract.site == RunSite::PostKernelHook;
		const CpuTexture* hooked = inputs[postKernel ? "SCALER_OUT" : "HOOKED"];
		// The hooked stage(s): LUMA nets hook the plane; D hooks SCALER_OUT and SCALER_SRC.
		for (const NetPass& p : pkg.passes)
		{
			if (p.hook == "SCALER_SRC") stages[p.hook] = inputs["SCALER_SRC"];
			else stages[p.hook] = hooked;
		}
		if (res.neighbourFallback)
		{
			// The net's trained single-frame fallback: neighbours = the frame itself, validity 0.
			zeroMask = CpuTexture(hooked->width, hooked->height, 2);
			for (size_t i = 0; i < zeroMask.rgba.size(); i += 4) zeroMask.rgba[i + 3] = 1.0f;
			inputs["VPNN_PREV"] = hooked;
			inputs["VPNN_NEXT"] = hooked;
			inputs["VPNN_MASK"] = &zeroMask;
		}
		for (size_t i = 0; i < programs.size(); ++i)
			if (!RunPass(i, in, error))
				return false;
		out = output.Ref(params.output == TexFormat::Unknown ? TexFormat::R32f : params.output);
		const auto t1 = std::chrono::steady_clock::now();
		RecordTiming(std::chrono::duration<double, std::milli>(t1 - t0).count());
		return true;
	}

	bool CpuNetExecutor::RunPass(size_t index, const NetRunInputs& in, std::string* error)
	{
		const NetPass& pass = pkg.passes[index];
		const Glsl::Program& prog = *programs[index];
		auto fail = [&](const std::string& why)
		{
			if (error) *error = pkg.name + ": pass " + std::to_string(index) + " '" + pass.desc + "': " + why;
			return false;
		};
		const CpuTexture* hooked = Lookup("HOOKED", pass.hook);
		if (!hooked) return fail("hooked stage " + pass.hook + " not bound");

		// Output size: the RPN expressions, or the hooked size.
		int w = hooked->width, h = hooked->height;
		if (!pass.width.empty())
		{
			auto lookup = [&](const std::string& n, char wh, int& v)
			{
				const CpuTexture* t = Lookup(n, pass.hook);
				if (!t) return false;
				v = wh == 'w' ? t->width : t->height;
				return true;
			};
			std::string err;
			if (!EvalSizeExpr(pass.width, lookup, w, &err) || !EvalSizeExpr(pass.height, lookup, h, &err))
				return fail(err);
		}
		if (w <= 0 || h <= 0) return fail("empty output size");

		// Bind the externs.
		const std::vector<Glsl::Extern>& ext = prog.Externs();
		Glsl::Context ctx;
		ctx.externs.resize(ext.size());
		std::vector<size_t> perPixelPos, fragCoord, invocation, localId, groupId;
		std::map<std::string, int> textureIndex;
		auto bindTexture = [&](const std::string& name, int& idx) -> bool
		{
			const auto it = textureIndex.find(name);
			if (it != textureIndex.end()) { idx = it->second; return true; }
			const CpuTexture* t = Lookup(name, pass.hook);
			if (!t) return false;
			Glsl::TexView tv;
			tv.width = t->width;
			tv.height = t->height;
			tv.rgba = t->rgba.data();
			ctx.textures.push_back(tv);
			idx = static_cast<int>(ctx.textures.size()) - 1;
			textureIndex[name] = idx;
			return true;
		};
		for (size_t e = 0; e < ext.size(); ++e)
		{
			const Glsl::Extern& x = ext[e];
			Glsl::Value& v = ctx.externs[e];
			switch (x.kind)
			{
			case Glsl::ExternKind::Raw:
			{
				int idx = -1;
				if (!bindTexture(x.binding, idx)) return fail("texture " + x.binding + " not bound");
				// mpv semantics: only //!BIND textures are visible (HOOKED is always bindable).
				v.base = Glsl::Base::Sampler;
				v.n = 1;
				v.i[0] = idx;
				break;
			}
			case Glsl::ExternKind::Size: case Glsl::ExternKind::Pt:
			{
				const CpuTexture* t = Lookup(x.binding, pass.hook);
				if (!t) return fail("texture " + x.binding + " not bound");
				const float sw = static_cast<float>(t->width), sh = static_cast<float>(t->height);
				v = Glsl::Value::Vec(0, 0, 0, 0);
				v.n = 2;
				v.f[0] = x.kind == Glsl::ExternKind::Size ? sw : 1.0f / sw;
				v.f[1] = x.kind == Glsl::ExternKind::Size ? sh : 1.0f / sh;
				break;
			}
			case Glsl::ExternKind::Mul:
				v = Glsl::Value::Float(1.0f);
				break;
			case Glsl::ExternKind::Pos:
				if (!Lookup(x.binding, pass.hook)) return fail("texture " + x.binding + " not bound");
				perPixelPos.push_back(e);
				break;
			case Glsl::ExternKind::Image:
				if (!pass.IsCompute()) return fail("out_image in a fragment pass");
				v.base = Glsl::Base::Image;
				v.n = 1;
				break;
			case Glsl::ExternKind::Plain:
				if (x.name == "gl_FragCoord") fragCoord.push_back(e);
				else if (x.name == "gl_GlobalInvocationID") invocation.push_back(e);
				else if (x.name == "gl_LocalInvocationID") localId.push_back(e);
				else if (x.name == "gl_WorkGroupID") groupId.push_back(e);
				else
				{
					const NetParam* prm = pkg.FindParam(x.name);
					if (!prm) return fail("unknown name '" + x.name + "'");
					const auto pv = in.params.find(x.name);
					const double val = pv != in.params.end() ? pv->second : prm->defaultValue;
					v = prm->isInt ? Glsl::Value::Int(static_cast<int>(std::lround(val))) : Glsl::Value::Float(static_cast<float>(val));
				}
				break;
			}
		}
		// Every bound texture must be declared with //!BIND (mpv/libplacebo only binds those).
		for (const auto& kv : textureIndex)
			if (kv.first != "HOOKED" && std::find(pass.binds.begin(), pass.binds.end(), kv.first) == pass.binds.end())
				return fail("reads " + kv.first + " without //!BIND");

		CpuTexture result(w, h, pass.components);
		if (pass.IsCompute())
		{
			ctx.image.width = w;
			ctx.image.height = h;
			ctx.image.rgba = result.rgba.data();
			ctx.image.components = pass.components;
			std::fill(result.rgba.begin(), result.rgba.end(), 0.0f);
		}
		static const float kDefault[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const float fx = x + 0.5f, fy = y + 0.5f;
				for (size_t e : perPixelPos) { Glsl::Value& v = ctx.externs[e]; v = Glsl::Value::Vec(fx / w, fy / h, 0, 0); v.n = 2; }
				for (size_t e : fragCoord) ctx.externs[e] = Glsl::Value::Vec(fx, fy, 0.0f, 1.0f);
				auto ivec3 = [](int a, int b, int c) { Glsl::Value v; v.base = Glsl::Base::Int; v.n = 3; v.i[0] = a; v.i[1] = b; v.i[2] = c; return v; };
				if (!invocation.empty() || !localId.empty() || !groupId.empty())
				{
					const int bx = pass.IsCompute() ? pass.computeX : 1, by = pass.IsCompute() ? pass.computeY : 1;
					for (size_t e : invocation) ctx.externs[e] = ivec3(x, y, 0);
					for (size_t e : localId) ctx.externs[e] = ivec3(x % bx, y % by, 0);
					for (size_t e : groupId) ctx.externs[e] = ivec3(x / bx, y / by, 0);
				}
				Glsl::Value r;
				if (!prog.Call("hook", ctx, r))
					return fail(ctx.error + " (pixel " + std::to_string(x) + "," + std::to_string(y) + ")");
				if (!pass.IsCompute())
				{
					if (r.base != Glsl::Base::Float || r.n != 4) return fail("hook() did not return a vec4");
					float* px = &result.rgba[(static_cast<size_t>(y) * w + x) * 4];
					for (int c = 0; c < 4; ++c) px[c] = c < pass.components ? r.f[c] : kDefault[c];
				}
			}
		// Intermediate format emulation (the net output keeps 32-bit float unless asked). A pass
		// without SAVE replaces its hooked stage (mpv semantics); the last one is the net output.
		const bool isLast = index + 1 == pkg.passes.size();
		if (!isLast && params.intermediate == TexFormat::Rgba16f)
			for (float& v : result.rgba) v = RoundToHalf(v);
		if (isLast && params.output == TexFormat::R16Unorm)
			for (size_t i = 0; i < result.rgba.size(); i += 4)
			{
				const float c = std::min(std::max(result.rgba[i], 0.0f), 1.0f);
				result.rgba[i] = std::nearbyint(c * 65535.0f) / 65535.0f;
			}
		if (pass.save.empty())
		{
			CpuTexture& slot = stageWrites[pass.hook];
			slot = std::move(result);
			stages[pass.hook] = &slot;
			if (isLast) output = slot;
		}
		else
		{
			saved[pass.save] = std::move(result);
		}
		return true;
	}
}
