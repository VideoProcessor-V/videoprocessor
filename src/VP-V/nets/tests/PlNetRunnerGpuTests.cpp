#include "CppUnitTest.h"

#include <VP-V/nets/CpuNetExecutor.h>
#include <VP-V/nets/NetFingerprint.h>
#include <VP-V/nets/NetSlot.h>
#include <VP-V/nets/NetFusion.h>
#include <VP-V/nets/PlNetRunner.h>

#include "NetTestSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders/custom.h>

#include <string>
#include <vector>

// PlNetRunner on a headless libplacebo device (D3D11), checked against the CPU reference
// executor on the same inputs, within the README tolerances.
//
// Device: WARP (software) by default, so the tests never compete with a running GPU job; with
// VP_TEST_GPU=hardware the adapter whose name contains VP_TEST_ADAPTER (default "NVIDIA"). Run the
// hardware variant on mains only and never alongside another GPU test or a running VideoProcessor.
// Short: test-size pictures only.
namespace NetRunnerTestsNs
{
	inline std::string Env(const char* name)
	{
		char* v = nullptr;
		size_t n = 0;
		std::string s;
		if (_dupenv_s(&v, &n, name) == 0 && v) { s = v; free(v); }
		return s;
	}

	// A headless libplacebo device for the net tests.
	struct NetGpu
	{
		pl_log log = nullptr;
		pl_d3d11 d3d = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		std::string device = "WARP (software)";

		NetGpu()
		{
			pl_log_params lp{};
			lp.log_level = Env("VP_TEST_PL_LOG") == "1" ? PL_LOG_ERR : PL_LOG_NONE;
			lp.log_cb = [](void*, enum pl_log_level, const char* msg) { Logger::WriteMessage(msg); Logger::WriteMessage("\n"); };
			log = pl_log_create(PL_API_VER, &lp);
			pl_d3d11_params dp{};
			if (Env("VP_TEST_GPU") == "hardware")
			{
				std::string want = Env("VP_TEST_ADAPTER");
				if (want.empty()) want = "NVIDIA";
				device = "default hardware adapter";
				IDXGIFactory1* factory = nullptr;
				if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
				{
					IDXGIAdapter1* adapter = nullptr;
					for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
					{
						DXGI_ADAPTER_DESC1 desc{};
						adapter->GetDesc1(&desc);
						adapter->Release();
						char name[128] = {};
						WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
						if (std::string(name).find(want) != std::string::npos && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
						{
							dp.adapter_luid = desc.AdapterLuid;
							device = name;
							break;
						}
					}
					factory->Release();
				}
			}
			else
			{
				dp.force_software = true;
				dp.allow_software = true;
			}
			d3d = pl_d3d11_create(log, &dp);
			Assert::IsNotNull(d3d, L"could not create the libplacebo test device");
			gpu = d3d->gpu;
			dispatch = pl_dispatch_create(log, gpu);
			Log("net GPU tests on: " + device);
		}
		~NetGpu()
		{
			pl_dispatch_destroy(&dispatch);
			pl_d3d11_destroy(&d3d);
			pl_log_destroy(&log);
		}
		NetDeviceContext Device() const
		{
			NetDeviceContext d;
			d.gpu = const_cast<pl_gpu_t*>(gpu);
			d.dispatch = dispatch;
			return d;
		}

		static const char* FloatFormat(int comps) { return comps <= 1 ? "r32f" : comps == 2 ? "rg32f" : "rgba32f"; }

		// A CPU texture as a float pl_tex (its first `components` channels).
		pl_tex Upload(const CpuTexture& t) const
		{
			const int comps = t.components >= 3 ? 4 : t.components;
			std::vector<float> data(static_cast<size_t>(t.width) * t.height * comps);
			for (size_t i = 0; i < static_cast<size_t>(t.width) * t.height; ++i)
				for (int c = 0; c < comps; ++c) data[i * comps + c] = t.rgba[i * 4 + c];
			pl_tex_params p{};
			p.w = t.width;
			p.h = t.height;
			p.format = pl_find_named_fmt(gpu, FloatFormat(comps));
			p.sampleable = true;
			p.initial_data = data.data();
			return pl_tex_create(gpu, &p);
		}

		// Any float pl_tex back as a CpuTexture (rgba32f readback through a blit).
		CpuTexture Download(pl_tex tex) const
		{
			const int w = tex->params.w, h = tex->params.h, comps = tex->params.format->num_components;
			pl_tex_params rp{};
			rp.w = w;
			rp.h = h;
			rp.format = tex->params.format;
			rp.host_readable = true;
			rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			Assert::IsNotNull(readable, L"readback texture");
			pl_tex_blit_params bp{};
			bp.src = tex;
			bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			// The HOST layout decides the decoding: D3D11 "r16f"/"rgba16f" are emulated (R16_FLOAT on the
			// GPU, fp32 on the host), so component_depth (16) must not be used here (fix/net-fp16-gate).
			const pl_fmt f = tex->params.format;
			const int hostBits = f->host_bits[0] ? f->host_bits[0] : static_cast<int>(f->texel_size * 8 / comps);
			Assert::IsTrue(hostBits == 16 || hostBits == 32, W(std::string("unexpected host layout of ") + f->name).c_str());
			Assert::AreEqual(static_cast<size_t>(comps) * hostBits / 8, f->texel_size, W(std::string("texel size of ") + f->name).c_str());
			std::vector<uint8_t> raw(static_cast<size_t>(w) * h * f->texel_size);
			pl_tex_transfer_params d{};
			d.tex = readable;
			d.ptr = raw.data();
			Assert::IsTrue(pl_tex_download(gpu, &d));
			pl_tex_destroy(gpu, &readable);
			return DecodeHostTexels(raw.data(), w, h, comps, hostBits);
		}

		// Values into a texture of the named format through its host layout (fp32 or IEEE half; a
		// half host layout needs values exactly representable in half).
		pl_tex UploadNamed(const char* name, int w, int h, const std::vector<float>& values) const
		{
			const pl_fmt f = pl_find_named_fmt(gpu, name);
			if (!f) return nullptr;
			const int comps = f->num_components;
			const int hostBits = f->host_bits[0] ? f->host_bits[0] : static_cast<int>(f->texel_size * 8 / comps);
			std::vector<uint8_t> raw(static_cast<size_t>(w) * h * f->texel_size);
			for (size_t i = 0; i < values.size() && i < static_cast<size_t>(w) * h * comps; ++i)
			{
				if (hostBits == 32) { std::memcpy(&raw[i * 4], &values[i], 4); continue; }
				const float v = values[i], a = std::fabs(v);
				uint16_t hb = v < 0 ? 0x8000 : 0;
				if (a >= 6.103515625e-05f)
				{
					int e = 0;
					const float m = std::frexp(a, &e);                       // a = m * 2^e, m in [0.5, 1)
					hb |= static_cast<uint16_t>(((e + 14) << 10) + (static_cast<int>(std::lround(m * 2048.0f)) - 1024));
				}
				else hb |= static_cast<uint16_t>(std::lround(a / 5.9604644775390625e-08f));
				std::memcpy(&raw[i * 2], &hb, 2);
			}
			pl_tex_params p{};
			p.w = w;
			p.h = h;
			p.format = f;
			p.sampleable = true;
			p.blit_src = (f->caps & PL_FMT_CAP_BLITTABLE) != 0;
			p.renderable = (f->caps & PL_FMT_CAP_RENDERABLE) != 0;
			p.initial_data = raw.data();
			return pl_tex_create(gpu, &p);
		}
	};

	inline double MaxDiffCodes(const CpuTexture& a, const CpuTexture& b, int channel = 0)
	{
		double m = 0.0;
		for (int y = 0; y < a.height; ++y)
			for (int x = 0; x < a.width; ++x)
				m = (std::max)(m, static_cast<double>(std::fabs(a.At(x, y, channel) - b.At(x, y, channel))));
		return m * 1023.0;
	}

	// README "Expected GPU-vs-CPU tolerance".
	inline double Tolerance(const NetPackage& p, TexFormat intermediate)
	{
		if (p.contract.usesLinearSampling) return 0.05;
		return intermediate == TexFormat::Rgba16f ? 0.01 : 0.005;
	}

	// A tile-gated net's gate branch on the GPU vs the CPU reference at the same intermediate format:
	// the gate-branch inputs (VPNNAGP_*, VPNNAG2_*), the logits (VPNNGATE) and the decision the gated
	// layers use (VPNNGATED, a 3x3 max of the logit signs). The net OUTPUT cannot be trusted to catch a
	// gate defect: GATED's dilation and near-zero smoke output weights hide a flipped tile.
	// Restores the hard assert that 328b6d29 relaxed to logging (fix/net-fp16-gate): the "implausible"
	// fp16 logits were the readback decoding emulated-fp16 (fp32 host) bytes as halves.
	inline void CheckGateBranch(const NetGpu& g, const std::string& name, TexFormat fmt, const CpuNetExecutor& cpu, const PlNetRunner& gpu)
	{
		const bool half = fmt == TexFormat::Rgba16f;
		for (const char* n : { "VPNNAGP_0", "VPNNAGP_1", "VPNNAG2_0", "VPNNAG2_1", "VPNNGATE" })
		{
			const CpuTexture* ct = cpu.Texture(n);
			void* gt = gpu.Intermediate(n);
			if (!ct) continue;
			Assert::IsNotNull(gt, W(name + ": GPU intermediate " + n + " missing").c_str());
			const CpuTexture got = g.Download(static_cast<pl_tex>(gt));
			Assert::IsTrue(got.width == ct->width && got.height == ct->height, W(name + ": " + n + " size differs").c_str());
			double maxAbs = 0.0, maxDiff = 0.0;
			for (int y = 0; y < ct->height; ++y)
				for (int x = 0; x < ct->width; ++x)
					for (int ch = 0; ch < ct->components && ch < 4; ++ch)
					{
						maxAbs = (std::max)(maxAbs, static_cast<double>(std::fabs(ct->At(x, y, ch))));
						maxDiff = (std::max)(maxDiff, static_cast<double>(std::fabs(ct->At(x, y, ch) - got.At(x, y, ch))));
					}
			// A few half ulps of the texture's scale at fp16 (rounding at each stored layer, tie
			// rounding); summation order only at fp32.
			const double tol = (half ? 8e-3 : 2e-4) * (std::max)(1.0, maxAbs);
			char line[220];
			snprintf(line, sizeof line, "%-27s %s: %-9s max |gpu - cpu| %.6f (max |cpu| %.4f, tolerance %.6f)", name.c_str(), FormatName(fmt), n, maxDiff, maxAbs, tol);
			Log(line);
			Assert::IsTrue(maxDiff <= tol, W(name + " " + FormatName(fmt) + ": gate-branch texture " + n + " differs from the CPU reference").c_str());
		}
		// Decisions. A logit within 0.05 of zero may round either way at fp16 (GPU vs the CPU's
		// round-to-nearest-even emulation): counted and logged, never at fp32.
		const CpuTexture gate = g.Download(static_cast<pl_tex>(gpu.Intermediate("VPNNGATE")));
		const CpuTexture& cg = *cpu.Texture("VPNNGATE");
		int nearFlips = 0;
		for (int y = 0; y < cg.height; ++y)
			for (int x = 0; x < cg.width; ++x)
			{
				const bool same = (cg.At(x, y) > 0.0f) == (gate.At(x, y) > 0.0f);
				if (!same && half && std::fabs(cg.At(x, y)) < 0.05f)
				{
					++nearFlips;
					Log(name + ": near-threshold fp16 gate tile (" + std::to_string(x) + "," + std::to_string(y) + ") cpu " + std::to_string(cg.At(x, y)) +
						", gpu " + std::to_string(gate.At(x, y)));
					continue;
				}
				Assert::IsTrue(same, W(name + " " + FormatName(fmt) + ": a gate tile flipped (cpu " + std::to_string(cg.At(x, y)) + ", gpu " +
					std::to_string(gate.At(x, y)) + ")").c_str());
			}
		const CpuTexture* cd = cpu.Texture("VPNNGATED");
		void* gdt = gpu.Intermediate("VPNNGATED");
		if (!cd) return;
		Assert::IsNotNull(gdt, W(name + ": GPU VPNNGATED missing").c_str());
		const CpuTexture gd = g.Download(static_cast<pl_tex>(gdt));
		for (int y = 0; y < cd->height; ++y)
			for (int x = 0; x < cd->width; ++x)
			{
				// GATED must be the 3x3 dilation of the GPU's own logit signs, and the CPU's where no
				// near-threshold tile flipped.
				float m = -1.0f;
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int sx = (std::min)((std::max)(x + dx, 0), gate.width - 1), sy = (std::min)((std::max)(y + dy, 0), gate.height - 1);
						if (gate.At(sx, sy) > 0.0f) m = 1.0f;
					}
				Assert::AreEqual(m, gd.At(x, y), W(name + ": VPNNGATED is not the dilation of the GPU gate").c_str());
				if (!nearFlips)
					Assert::AreEqual(cd->At(x, y), gd.At(x, y), W(name + " " + FormatName(fmt) + ": VPNNGATED differs from the CPU reference").c_str());
			}
		if (nearFlips) Log(name + ": " + std::to_string(nearFlips) + " near-threshold gate tile(s) rounded differently at fp16");
	}

	TEST_CLASS(PlNetRunnerGpuTests)
	{
	public:
		TEST_METHOD(GpuRunsTheSyntheticNetLikeTheCpu)
		{
			NetGpu g;
			const NetPackage pkg = SyntheticPackage();
			CpuTexture in = SyntheticPlane(37, 21);
			pl_tex tin = g.Upload(in);
			NetRunInputs inputs;
			inputs.textures["HOOKED"] = in.Ref();
			inputs.params["vpnn_gain"] = 0.125;
			CpuNetExecutor cpu;
			std::string err;
			NetPrepareParams pp;
			pp.intermediate = TexFormat::Rgba32f;
			Assert::IsTrue(cpu.Prepare(NetDeviceContext(), pkg, pp, &err), W(err).c_str());
			NetTexture cout_;
			Assert::IsTrue(cpu.Run(NetDeviceContext(), inputs, cout_, &err), W(err).c_str());

			PlNetRunner gpu;
			Assert::IsTrue(gpu.Prepare(g.Device(), pkg, pp, &err), W(err).c_str());
			NetRunInputs gin = inputs;
			gin.textures["HOOKED"].handle = const_cast<pl_tex_t*>(tin);
			NetTexture out;
			Assert::IsTrue(gpu.Run(g.Device(), gin, out, &err), W(err).c_str());
			Assert::AreEqual(37, out.width);
			const CpuTexture got = g.Download(static_cast<pl_tex>(out.handle));
			const double d = MaxDiffCodes(got, cpu.Output());
			Log("synthetic: max |gpu - cpu| = " + std::to_string(d) + " codes");
			Assert::IsTrue(d <= 0.005, L"synthetic net: GPU vs CPU");
			// The half-size compute pass and its RPN size.
			const pl_tex half = static_cast<pl_tex>(gpu.Intermediate("TH"));
			Assert::IsNotNull(half);
			Assert::AreEqual(19, half->params.w, L"roundf(37 / 2 + 0.25)");
			// Pooling: a second frame reuses the textures.
			Assert::IsTrue(gpu.Run(g.Device(), gin, out, &err), W(err).c_str());
			Assert::IsTrue(static_cast<pl_tex>(gpu.Intermediate("TH")) == half, L"pass textures are pooled across frames");
			gpu.Release();
			pl_tex_destroy(g.gpu, &tin);
		}

		// The fp16 readback in isolation: known values into r16f (emulated on D3D11: fp32 host layout)
		// and r16hf (half host layout), back through the same blit + download the net checks use.
		TEST_METHOD(R16fRoundTripReadback)
		{
			NetGpu g;
			// Exactly representable in half: the logits of the 2026-10-24 report, signs, a subnormal,
			// the smallest normal and the largest half.
			const std::vector<float> exact = { 22.859375f, 22.84375f, 23.375f, -0.625f, 2.0f, -512.0f, -0.0078125f, 1.98046875f,
				6.103515625e-05f, 3.0f * 5.9604644775390625e-08f, 65504.0f, -1.0f };
			const int w = 4, h = 3;
			int formats = 0;
			for (const char* fmtName : { "r16f", "r16hf" })
			{
				const pl_fmt f = pl_find_named_fmt(g.gpu, fmtName);
				if (!f) { Log(std::string(fmtName) + ": not offered by this backend"); continue; }
				char info[160];
				snprintf(info, sizeof info, "%s: component_depth %d, host_bits %d, texel_size %zu, emulated %d", fmtName, f->component_depth[0], f->host_bits[0],
					f->texel_size, f->emulated ? 1 : 0);
				Log(info);
				pl_tex t = g.UploadNamed(fmtName, w, h, exact);
				Assert::IsNotNull(t, W(std::string(fmtName) + ": upload").c_str());
				const CpuTexture got = g.Download(t);
				for (int i = 0; i < w * h; ++i)
					Assert::AreEqual(exact[i], got.At(i % w, i / w), W(std::string(fmtName) + ": value " + std::to_string(i) + " did not round-trip").c_str());
				pl_tex_destroy(g.gpu, &t);
				// Non-representable values through an fp32 host layout: the GPU's half store, within one
				// half ulp of round-to-nearest-even (a store may truncate).
				if (f->host_bits[0] == 32)
				{
					const std::vector<float> rough = { 0.1f, -0.3f, 22.86f, 1.0f / 3.0f, 1000.7f, -7.77e-5f, 0.0f, 3.14159f, -2.71828f, 0.5f, 12345.6f, -0.05f };
					pl_tex r = g.UploadNamed(fmtName, w, h, rough);
					Assert::IsNotNull(r);
					const CpuTexture gr = g.Download(r);
					for (int i = 0; i < w * h; ++i)
					{
						const float want = RoundToHalf(rough[i]), v = gr.At(i % w, i / w);
						int e = 0;
						std::frexp((std::max)(std::fabs(want), 6.103515625e-05f), &e);
						const float ulp = std::ldexp(1.0f, e - 11);
						Assert::IsTrue(std::fabs(v - want) <= ulp, W(std::string(fmtName) + ": " + std::to_string(rough[i]) + " read back as " + std::to_string(v)).c_str());
						Assert::AreEqual(RoundToHalf(v), v, W(std::string(fmtName) + ": the stored value is not a half").c_str());
					}
					pl_tex_destroy(g.gpu, &r);
				}
				++formats;
			}
			Assert::IsTrue(formats > 0, L"no fp16 format to test");
		}

		TEST_METHOD(GpuMatchesCpuOnTheSmokeExports)
		{
			NetGpu g;
			int ran = 0;
			for (const char* set : kSets)
				for (const char* caseName : kCases)
				{
					const std::string name = std::string(set) + "/" + caseName;
					RefCase c;
					std::string skip, err;
					if (!LoadCase(name, c, &skip)) { Log("SKIPPED " + name + ": " + skip); continue; }
					std::vector<pl_tex> uploaded;
					NetRunInputs gin = c.inputs;
					for (size_t k = 0; k < c.desc["bindings"].items.size(); ++k)
					{
						uploaded.push_back(g.Upload(c.textures[k]));
						gin.textures[c.desc["bindings"].items[k]["name"].text].handle = const_cast<pl_tex_t*>(uploaded.back());
					}
					for (TexFormat fmt : { TexFormat::Rgba32f, TexFormat::Rgba16f })
					{
						if (fmt == TexFormat::Rgba16f && !c.pkg.fingerprint.fp16Allowed && c.pkg.fingerprint.intermediate == TexFormat::Rgba32f)
							continue;   // the fingerprint keeps rgba32f
						CpuNetExecutor cpu;
						Assert::IsTrue(RunCase(c, fmt, cpu, &err), W(err).c_str());
						PlNetRunner gpu;
						NetPrepareParams pp;
						pp.intermediate = fmt;
						Assert::IsTrue(gpu.Prepare(g.Device(), c.pkg, pp, &err), W(name + ": " + err).c_str());
						NetTexture out;
						Assert::IsTrue(gpu.Run(g.Device(), gin, out, &err), W(name + ": " + err).c_str());
						const CpuTexture got = g.Download(static_cast<pl_tex>(out.handle));
						const double d = MaxDiffCodes(got, cpu.Output());
						const double tol = Tolerance(c.pkg, fmt);
						char line[200];
						snprintf(line, sizeof line, "%-27s %s: max |gpu - cpu| %.5f codes (tolerance %.3f); vs pytorch %.5f", name.c_str(),
							FormatName(fmt), d, tol, MaxDiffCodes(got, c.expected, c.outW, c.outH));
						Log(line);
						Assert::IsTrue(d <= tol, W(name + ": GPU vs CPU beyond the tolerance").c_str());
						// The gate branch, value by value, and the decisions (hard asserts at fp32 AND fp16).
						if (c.pkg.contract.tileGated && cpu.Texture("VPNNGATE"))
							CheckGateBranch(g, name, fmt, cpu, gpu);
						gpu.Release();
					}
					for (pl_tex& t : uploaded) pl_tex_destroy(g.gpu, &t);
					++ran;
				}
			Log("ran " + std::to_string(ran) + " cases on the GPU");
		}

		TEST_METHOD(PostKernelHookEntryRunsD)
		{
			RefCase c;
			std::string skip, err;
			if (!LoadCase("round1/D_hybrid_refine", c, &skip) && !LoadCase("smoke/D_hybrid_refine", c, &skip)) { Log("SKIPPED: " + skip); return; }
			NetGpu g;
			pl_tex out = nullptr, src = nullptr;
			for (size_t k = 0; k < c.desc["bindings"].items.size(); ++k)
			{
				const std::string n = c.desc["bindings"].items[k]["name"].text;
				(n == "SCALER_OUT" ? out : src) = g.Upload(c.textures[k]);
			}
			// What AdaptiveDownscale's POST_KERNEL hook receives (PL_HOOK_SIG_TEX): the picture over the
			// destination rect at the texture origin.
			pl_hook_params hp{};
			hp.gpu = g.gpu;
			hp.dispatch = g.dispatch;
			hp.tex = out;
			hp.dst_rect = { 0, 0, out->params.w, out->params.h };
			NetTexture srcTex;
			srcTex.handle = const_cast<pl_tex_t*>(src);
			srcTex.width = src->params.w;
			srcTex.height = src->params.h;
			srcTex.components = 1;
			PlNetRunner gpu;
			NetPrepareParams pp;
			pp.intermediate = TexFormat::Rgba32f;
			Assert::IsTrue(gpu.Prepare(PlNetRunner::DeviceFromHook(&hp), c.pkg, pp, &err), W(err).c_str());
			NetTexture result;
			Assert::IsTrue(gpu.RunInPostKernelHook(&hp, srcTex, result, &err), W(err).c_str());
			CpuNetExecutor cpu;
			Assert::IsTrue(RunCase(c, TexFormat::Rgba32f, cpu, &err), W(err).c_str());
			const double d = MaxDiffCodes(g.Download(static_cast<pl_tex>(result.handle)), cpu.Output());
			Log("D via the POST_KERNEL entry: max |gpu - cpu| " + std::to_string(d) + " codes");
			Assert::IsTrue(d <= Tolerance(c.pkg, TexFormat::Rgba32f));
			// A fractional source crop cannot be mapped exactly: refused, visibly.
			NetTexture cropped = srcTex;
			cropped.rect[0] = 0.5f; cropped.rect[1] = 0.0f; cropped.rect[2] = static_cast<float>(src->params.w); cropped.rect[3] = static_cast<float>(src->params.h);
			Assert::IsFalse(gpu.RunInPostKernelHook(&hp, cropped, result, &err));
			Assert::IsTrue(err.find("fractional") != std::string::npos, W(err).c_str());
			gpu.Release();
			pl_tex_destroy(g.gpu, &out);
			pl_tex_destroy(g.gpu, &src);
		}

		TEST_METHOD(GpuTimingIsRecordedAgainstTheSlot)
		{
			NetGpu g;
			pl_timer probe = pl_timer_create(g.gpu);
			if (!probe) { Log("SKIPPED: this device has no timer queries"); return; }
			pl_timer_destroy(g.gpu, &probe);
			const NetPackage pkg = SyntheticPackage();
			CpuTexture in = SyntheticPlane(64, 32);
			pl_tex tin = g.Upload(in);
			NetRunInputs inputs;
			inputs.textures["HOOKED"] = in.Ref();
			inputs.textures["HOOKED"].handle = const_cast<pl_tex_t*>(tin);
			PlNetRunner gpu;
			SlotTiming::Accumulator acc;
			gpu.SetTiming(&acc, SlotOf(pkg.role));
			std::string err;
			NetPrepareParams pp;
			pp.intermediate = TexFormat::Rgba32f;
			Assert::IsTrue(gpu.Prepare(g.Device(), pkg, pp, &err), W(err).c_str());
			NetTexture out;
			for (int i = 0; i < 6 && !acc.Samples(PipelineSlots::Id::RepairEncoding); ++i)
			{
				Assert::IsTrue(gpu.Run(g.Device(), inputs, out, &err), W(err).c_str());
				pl_gpu_finish(g.gpu);
			}
			Assert::IsTrue(acc.Samples(PipelineSlots::Id::RepairEncoding) > 0, L"pl_timer results reach SlotTiming");
			Log("synthetic net GPU time: " + std::to_string(acc.MeanMs(PipelineSlots::Id::RepairEncoding)) + " ms (" + g.device + ")");
			gpu.Release();
			pl_tex_destroy(g.gpu, &tin);
		}

		TEST_METHOD(UnsupportedFormatBypassesVisibly)
		{
			NetGpu g;
			const NetPackage pkg = SyntheticPackage();   // trained rgba32f, fp16 not proven
			PlNetRunner gpu;
			gpu.DisableFormat(TexFormat::Rgba32f);
			FallbackStatus::Registry registry;
			NetBypassSet bypass(registry);
			NetSlot slot(gpu, bypass);
			NetRuntimeContext ctx = ContextMatching(pkg);
			ctx.policy.upstreamKey = Requirement::IfDeclared;
			Assert::IsFalse(slot.Load(g.Device(), pkg, ctx));
			Log(registry.Heading());
			Assert::IsTrue(registry.Heading().find("nets: bypassed (T_tiny: no usable intermediate format (trained rgba32f") != std::string::npos,
				W(registry.Heading()).c_str());
			// With the format back, the same slot loads and the entry clears.
			PlNetRunner ok;
			NetSlot slot2(ok, bypass);
			Assert::IsTrue(slot2.Load(g.Device(), pkg, ctx));
			Assert::AreEqual(std::string("Fallbacks engaged: none"), registry.Heading());
		}

		// ------------------------------------------------------------ fused compute (efficiency batch)
		TEST_METHOD(FusedLayersMatchTheCpuReference)
		{
			NetGpu g;
			int ran = 0;
			for (const char* set : kSets)
				for (const char* caseName : { "D_hybrid_refine", "A_motion_small_open", "A_single_small_open", "C_rebase_refine_h" })
				{
					const std::string name = std::string(set) + "/" + caseName;
					RefCase c;
					std::string skip, err;
					if (!LoadCase(name, c, &skip)) { Log("SKIPPED " + name + ": " + skip); continue; }
					const std::vector<ConvLayer> layers = ExtractConvLayers(c.pkg);
					Assert::IsFalse(layers.empty(), W(name + ": no fusable conv layers found").c_str());
					std::vector<pl_tex> uploaded;
					NetRunInputs gin = c.inputs;
					for (size_t k = 0; k < c.desc["bindings"].items.size(); ++k)
					{
						uploaded.push_back(g.Upload(c.textures[k]));
						gin.textures[c.desc["bindings"].items[k]["name"].text].handle = const_cast<pl_tex_t*>(uploaded.back());
					}
					CpuNetExecutor cpu;
					Assert::IsTrue(RunCase(c, TexFormat::Rgba32f, cpu, &err), W(err).c_str());
					struct Variant { const char* label; FusedKind kind; bool fp16; };
					const Variant variants[] = { { "direct", FusedKind::Direct, false }, { "winograd", FusedKind::Winograd, false },
						{ "direct+fp16", FusedKind::Direct, true }, { "winograd+fp16", FusedKind::Winograd, true } };
					CpuNetExecutor cpu16;
					const bool fp16Ok = c.pkg.fingerprint.fp16Allowed;
					if (fp16Ok) Assert::IsTrue(RunCase(c, TexFormat::Rgba16f, cpu16, &err), W(err).c_str());
					for (const Variant& v : variants)
					{
						PlNetRunner gpu;
						gpu.SetFusion(true, v.kind, v.fp16);
						NetPrepareParams pp;
						pp.intermediate = TexFormat::Rgba32f;
						if (v.fp16 && !fp16Ok)
						{
							Assert::IsFalse(gpu.Prepare(g.Device(), c.pkg, pp, &err), W(name + ": fp16 storage must be refused").c_str());
							continue;
						}
						Assert::IsTrue(gpu.Prepare(g.Device(), c.pkg, pp, &err), W(name + ": " + err).c_str());
						Assert::AreEqual(layers.size(), gpu.FusedLayers(), W(name + ": every extracted layer fuses").c_str());
						NetTexture out;
						Assert::IsTrue(gpu.Run(g.Device(), gin, out, &err), W(name + " " + v.label + ": " + err).c_str());
						const double d = MaxDiffCodes(g.Download(static_cast<pl_tex>(out.handle)), v.fp16 ? cpu16.Output() : cpu.Output());
						const double tol = Tolerance(c.pkg, v.fp16 ? TexFormat::Rgba16f : TexFormat::Rgba32f);
						char line[200];
						snprintf(line, sizeof line, "%-27s fused %-14s (%zu layers): max |gpu - cpu| %.5f codes", name.c_str(), v.label, layers.size(), d);
						Log(line);
						Assert::IsTrue(d <= tol, W(name + " " + v.label + ": fused output beyond the tolerance").c_str());
						if (c.pkg.contract.tileGated && cpu.Texture("VPNNGATE"))
						{
							const CpuTexture gate = g.Download(static_cast<pl_tex>(gpu.Intermediate("VPNNGATE")));
							const CpuTexture& cg = *cpu.Texture("VPNNGATE");
							for (int y = 0; y < cg.height; ++y)
								for (int x = 0; x < cg.width; ++x)
									Assert::AreEqual(cg.At(x, y) > 0.0f, gate.At(x, y) > 0.0f, W(name + ": a gate tile flipped").c_str());
						}
						gpu.Release();
					}
					for (pl_tex& t : uploaded) pl_tex_destroy(g.gpu, &t);
					++ran;
				}
			if (!ran) Log("SKIPPED: no export here");
		}

		// Timing: exported passes vs fused (direct / Winograd, fp32 / fp16) for D at the projector
		// size (2160 source -> 2560x1600). Hardware GPU only (VP_TEST_GPU=hardware), short.
		TEST_METHOD(FusedTimingVsExportedPasses)
		{
			if (Env("VP_TEST_GPU") != "hardware") { Log("SKIPPED: timing needs VP_TEST_GPU=hardware"); return; }
			RefCase c;
			std::string skip, err;
			if (!LoadCase("round1/D_hybrid_refine", c, &skip) && !LoadCase("smoke/D_hybrid_refine", c, &skip)) { Log("SKIPPED: " + skip); return; }
			NetGpu g;
			const int ow = 2560, oh = 1600, sw = 3840, sh = 2160;
			CpuTexture out(ow, oh, 1), src(sw, sh, 1);
			uint32_t seed = 12345;
			auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return 0.2f + 0.6f * static_cast<float>(seed >> 8) / 16777216.0f; };
			for (size_t i = 0; i < out.rgba.size(); i += 4) out.rgba[i] = rnd();
			for (size_t i = 0; i < src.rgba.size(); i += 4) src.rgba[i] = rnd();
			pl_tex tout = g.Upload(out), tsrc = g.Upload(src);
			NetRunInputs in;
			in.textures["SCALER_OUT"] = out.Ref();
			in.textures["SCALER_OUT"].handle = const_cast<pl_tex_t*>(tout);
			in.textures["SCALER_SRC"] = src.Ref();
			in.textures["SCALER_SRC"].handle = const_cast<pl_tex_t*>(tsrc);
			struct Variant { const char* label; bool fused; FusedKind kind; bool fp16; };
			const Variant variants[] = { { "exported passes", false, FusedKind::Direct, false }, { "fused direct", true, FusedKind::Direct, false },
				{ "fused winograd", true, FusedKind::Winograd, false }, { "fused direct fp16", true, FusedKind::Direct, true },
				{ "fused winograd fp16", true, FusedKind::Winograd, true } };
			for (const Variant& v : variants)
			{
				PlNetRunner gpu;
				if (v.fused) gpu.SetFusion(true, v.kind, v.fp16);
				SlotTiming::Accumulator acc;
				gpu.SetTiming(&acc, PipelineSlots::Id::ScalingOutput);
				NetPrepareParams pp;
				pp.intermediate = TexFormat::Rgba32f;
				Assert::IsTrue(gpu.Prepare(g.Device(), c.pkg, pp, &err), W(err).c_str());
				NetTexture o;
				for (int f = 0; f < 24; ++f)
				{
					Assert::IsTrue(gpu.Run(g.Device(), in, o, &err), W(err).c_str());
					pl_gpu_finish(g.gpu);
					if (f == 3) acc = SlotTiming::Accumulator();   // drop warm-up (shader compiles)
				}
				char line[160];
				snprintf(line, sizeof line, "D %dx%d (src %dx%d) %-20s mean %.3f ms over %llu frames (%s)", ow, oh, sw, sh, v.label,
					acc.MeanMs(PipelineSlots::Id::ScalingOutput), static_cast<unsigned long long>(acc.Samples(PipelineSlots::Id::ScalingOutput)), g.device.c_str());
				Log(line);
				gpu.Release();
			}
			pl_tex_destroy(g.gpu, &tout);
			pl_tex_destroy(g.gpu, &tsrc);
		}
	};
}
