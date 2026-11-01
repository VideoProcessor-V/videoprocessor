// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.temporal_detail / repair.grain_split GPU passes against their CPU reference models (TemporalDetailModel.h,
// GrainSplit.h) on a libplacebo device: WARP (software, runs in the CPU tier) unless VP_TEST_GPU=hardware. Synthetic
// scenes as the model tests (an analytic textured master, a coding simulation per frame, fresh grain per frame, exact
// vectors). The timing test is hardware only (category GpuHardware).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailGpu.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailModel.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace TemporalDetailGpuTestsNs
{
	using namespace TemporalDetail;

	constexpr int kW = 128, kH = 96;
	constexpr double kHz = 23.976;
	constexpr double kPi = 3.14159265358979323846;

	void LogF(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0, double e = 0)
	{
		char line[256];
		std::snprintf(line, sizeof line, fmt, a, b, c, d, e);
		Logger::WriteMessage(line);
	}
	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// --- The model tests' synthetic master and coding simulation (TemporalDetailModelTests.cpp) ---
	double Pulse(double t)
	{
		const double m = t - 64.0 * std::floor(t / 64.0);
		return 1.0 - Smoothstep(13.0, 16.0, std::fabs(m - 32.0));
	}
	double Master(double x, double y)
	{
		struct S { double fx, fy, ph, a; };
		static const S fine[] = { { 1 / 4.5, 0.05, 0.3, 10 }, { 0.04, 1 / 5.3, 1.1, 9 }, { 0.7 / 6.1, 0.7 / 6.1, 2.0, 8 } };
		static const S mid[] = { { 1 / 11.0, 0.02, 0.7, 14 }, { 0.03, 1 / 14.0, 2.5, 12 }, { 0.7 / 18.0, -0.7 / 18.0, 1.3, 10 } };
		const double env = 1.0 - Pulse(x) * Pulse(y);
		double t = 0.0;
		for (const S& s : fine) t += s.a * std::sin(2 * kPi * (s.fx * x + s.fy * y) + s.ph);
		for (const S& s : mid) t += s.a * std::sin(2 * kPi * (s.fx * x + s.fy * y) + s.ph);
		const double level = 110.0 + 25.0 * std::sin(2 * kPi * x / 97.0 + 0.5) * std::cos(2 * kPi * y / 131.0);
		const double edges = 14.0 * std::tanh((x * 0.8 + y * 0.6 - 40.0) / 1.0) + 10.0 * std::tanh((x - y * 0.7 - 140.0) / 1.3);
		return level + env * t + edges;
	}
	Plane Sample(double dx, double dy, int w = kW, int h = kH)
	{
		Plane p(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x) p.at(x, y) = Master(x - dx, y - dy);
		return p;
	}
	Plane Code(const Plane& truth, double q)
	{
		if (q >= 1.0) return truth;
		const Plane g1 = Detail::Gauss(truth, 1.0);
		Plane o(truth.w, truth.h);
		for (size_t i = 0; i < o.v.size(); ++i) o.v[i] = truth.v[i] - (1.0 - q) * (truth.v[i] - g1.v[i]);
		return o;
	}
	struct Noise
	{
		std::mt19937 g;
		explicit Noise(uint32_t seed) : g(seed) {}
		double Uniform() { return (static_cast<double>(g()) + 0.5) / 4294967296.0; }
		double Next() { return std::sqrt(-2.0 * std::log(Uniform())) * std::cos(2.0 * kPi * Uniform()); }
	};
	double QualityOf(char c)
	{
		switch (c) { case 'I': return 1.0; case 'P': return 0.8; case 'B': return 0.3; case 'b': return 0.55; default: return 1.0; }
	}
	// The plane as the GPU sees it: 10-bit codes at 1/64-code resolution (a 16-bit plane with bit_shift 6).
	Plane Quantised(const Plane& p)
	{
		Plane q(p.w, p.h);
		for (size_t i = 0; i < q.v.size(); ++i) q.v[i] = (std::min)(65535.0, (std::max)(0.0, std::round(p.v[i] * 64.0))) / 64.0;
		return q;
	}
	// n frames: truth panned by (px, py) per frame, coding pattern, grain sigma, seed.
	std::vector<Plane> Stream(int n, const char* pattern, double px, double py, uint32_t seed, double sigma, bool flatLeft = false, int w = kW, int h = kH)
	{
		std::vector<Plane> out;
		Noise noise(seed);
		const size_t len = std::strlen(pattern);
		for (int t = 0; t < n; ++t)
		{
			Plane truth = Sample(px * t, py * t, w, h);
			if (flatLeft)
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w / 2; ++x) truth.at(x, y) = 100.0 + 0.1 * y;
			Plane f = Code(truth, QualityOf(pattern[static_cast<size_t>(t) % len]));
			for (double& v : f.v) v += sigma * noise.Next();
			out.push_back(Quantised(f));
		}
		return out;
	}

	// --- Device and textures ---
	struct GpuDevice
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_dispatch dispatch = nullptr;
		~GpuDevice()
		{
			if (dispatch) pl_dispatch_destroy(&dispatch);
			if (d3d11) pl_d3d11_destroy(&d3d11);
			if (log) pl_log_destroy(&log);
		}
		bool Create()
		{
			pl_log_params lp{};
			lp.log_level = PL_LOG_ERR;
			lp.log_cb = [](void*, enum pl_log_level, const char* msg) { Logger::WriteMessage((std::string("libplacebo: ") + msg).c_str()); };
			log = pl_log_create(PL_API_VER, &lp);
			std::string name;
			const pl_d3d11_params params = ExtendedRangeGpuProbe::TestDeviceParams(&name);
			Logger::WriteMessage(("temporal detail GPU tests on: " + name).c_str());
			d3d11 = pl_d3d11_create(log, &params);
			if (!d3d11) return false;
			dispatch = pl_dispatch_create(log, d3d11->gpu);
			return dispatch != nullptr;
		}
		pl_gpu Gpu() const { return d3d11->gpu; }
	};

	pl_tex UploadLuma(pl_gpu gpu, const Plane& y)
	{
		std::vector<uint16_t> data(y.v.size());
		for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint16_t>((std::min)(65535L, (std::max)(0L, std::lround(y.v[i] * 64.0))));
		pl_tex_params p{};
		p.w = y.w; p.h = y.h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	pl_tex UploadPair(pl_gpu gpu, const MotionPair& m)
	{
		std::vector<float> data(static_cast<size_t>(m.gridW) * m.gridH * 4);
		for (size_t i = 0; i < static_cast<size_t>(m.gridW) * m.gridH; ++i)
		{
			data[i * 4] = m.vx[i]; data[i * 4 + 1] = m.vy[i]; data[i * 4 + 2] = m.trust[i]; data[i * 4 + 3] = 0.0f;
		}
		pl_tex_params p{};
		p.w = m.gridW; p.h = m.gridH; p.format = pl_find_named_fmt(gpu, "rgba32f"); p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	// A float texture's texels, copied by a compute pass into a host-readable storage texture (float formats need not be
	// blittable).
	std::vector<float> DownloadFloat(pl_gpu gpu, pl_dispatch dispatch, pl_tex tex)
	{
		pl_tex_params rp{};
		rp.w = tex->params.w; rp.h = tex->params.h; rp.format = tex->params.format; rp.host_readable = true; rp.storable = true;
		pl_tex readable = pl_tex_create(gpu, &rp);
		if (!readable) return {};
		const pl_shader_desc d[2] = { VpvGpu::Sampled("src", tex), VpvGpu::Storage("dst", readable) };
		VpvGpu::Pass(dispatch, "test copy", std::string(), R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < textureSize(src, 0).x && p.y < textureSize(src, 0).y) imageStore(dst, p, texelFetch(src, p, 0));
)GLSL", d, 2, nullptr, 0, (rp.w + 15) / 16, (rp.h + 15) / 16, 0);
		std::vector<float> v(static_cast<size_t>(rp.w) * rp.h * tex->params.format->num_components);
		pl_tex_transfer_params tr{}; tr.tex = readable; tr.ptr = v.data();
		const bool ok = pl_tex_download(gpu, &tr);
		pl_tex_destroy(gpu, &readable);
		if (!ok) v.clear();
		return v;
	}

	Plane DownloadLuma(pl_gpu gpu, pl_tex tex)
	{
		pl_tex_params rp{};
		rp.w = tex->params.w; rp.h = tex->params.h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
		pl_tex readable = pl_tex_create(gpu, &rp);
		pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
		pl_tex_blit(gpu, &bp);
		std::vector<uint16_t> raw(static_cast<size_t>(rp.w) * rp.h * tex->params.format->num_components);
		pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
		const bool ok = pl_tex_download(gpu, &d);
		pl_tex_destroy(gpu, &readable);
		Plane out(rp.w, rp.h);
		const int nc = tex->params.format->num_components;
		if (ok)
			for (size_t i = 0; i < out.v.size(); ++i) out.v[i] = raw[i * nc] / 64.0;
		return out;
	}

	pl_frame LumaFrame(pl_tex tex)
	{
		pl_frame f{};
		f.num_planes = 1;
		f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
		f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
		f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
		f.color = pl_color_space_bt709;
		return f;
	}

	Settings Steady()
	{
		Settings s;
		s.mode = Mode::Steady;
		return s;
	}

	struct Compare
	{
		double worst = 0.0, meanAbs = 0.0;
		int over = 0;          // pixels more than 0.25 code apart
		double changed = 0.0;  // share of pixels the model changed by more than 0.05 code
	};
	Compare CompareOut(const Plane& gpuOut, const Plane& modelOut, const Plane& in)
	{
		Compare c;
		int changed = 0;
		for (size_t i = 0; i < gpuOut.v.size(); ++i)
		{
			const double dq = std::fabs(gpuOut.v[i] - std::round(modelOut.v[i] * 64.0) / 64.0);
			c.worst = (std::max)(c.worst, dq);
			c.meanAbs += dq;
			c.over += dq > 0.25 ? 1 : 0;
			changed += std::fabs(modelOut.v[i] - in.v[i]) > 0.05 ? 1 : 0;
		}
		c.meanAbs /= static_cast<double>(gpuOut.v.size());
		c.changed = static_cast<double>(changed) / static_cast<double>(gpuOut.v.size());
		return c;
	}

	// One stream through the GPU module and the CPU model side by side; per-frame comparison logged and asserted.
	struct MatchResult
	{
		std::vector<Plane> gpu, model;
		double worst = 0.0, worstMean = 0.0, maxOverShare = 0.0, changedShare = 0.0;
	};
	MatchResult RunMatch(GpuDevice& dev, const std::vector<Plane>& frames, double panX, double panY, const Gpu::Params& base,
		const Model::Tuning& tune, const std::function<void(int, Gpu::Params&, Model::FrameIn&)>& hook = nullptr)
	{
		pl_gpu gpu = dev.Gpu();
		const int w = frames[0].w, h = frames[0].h;
		const MotionPair bwd = MotionPair::Uniform(w, h, 16.0, -panX, -panY), fwd = MotionPair::Uniform(w, h, 16.0, panX, panY);
		pl_tex bwdTex = UploadPair(gpu, bwd), fwdTex = UploadPair(gpu, fwd);
		FieldMotionSource motion;
		for (size_t t = 0; t < frames.size(); ++t)
		{
			motion.SetBackward(t, bwd);
			motion.SetForward(t, fwd);
		}
		Gpu::Module module;
		Model model(base.settings);
		model.SetTuning(tune);
		MatchResult r;
		double changed = 0.0;
		for (size_t t = 0; t < frames.size(); ++t)
		{
			Gpu::Params p = base;
			p.seq = t;
			p.contentHz = kHz;
			p.planeLines = 1080.0;
			p.backward.tex = bwdTex;
			p.forward.tex = fwdTex;
			Model::FrameIn in;
			in.luma = frames[t];
			in.seq = t;
			in.contentHz = kHz;
			in.planeLines = 1080.0;
			in.motion = &motion;
			if (hook) hook(static_cast<int>(t), p, in);
			pl_tex tex = UploadLuma(gpu, frames[t]);
			pl_frame out{};
			const bool ok = module.Apply(gpu, dev.dispatch, LumaFrame(tex), p, out);
			Assert::IsTrue(ok, Wide(module.LastError()).c_str());
			r.gpu.push_back(DownloadLuma(gpu, out.planes[0].texture));
			r.model.push_back(model.Process(in));
			const Compare c = CompareOut(r.gpu.back(), r.model.back(), frames[t]);
			char line[200];
			std::snprintf(line, sizeof line, "frame %zu: worst %.4f, mean %.5f codes, %d px > 0.25; model changed %.1f%% (%s / %s)", t, c.worst, c.meanAbs, c.over,
				100.0 * c.changed, module.Status().c_str(), model.Diag().status.c_str());
			Logger::WriteMessage(line);
			r.worst = (std::max)(r.worst, c.worst);
			r.worstMean = (std::max)(r.worstMean, c.meanAbs);
			r.maxOverShare = (std::max)(r.maxOverShare, static_cast<double>(c.over) / static_cast<double>(w * h));
			changed += c.changed;
			pl_tex_destroy(gpu, &tex);
		}
		r.changedShare = changed / static_cast<double>(frames.size());
		module.Release(gpu);
		pl_tex_destroy(gpu, &bwdTex);
		pl_tex_destroy(gpu, &fwdTex);
		return r;
	}

	Plane Band(const Plane& p, double sigma)
	{
		const Plane g = Detail::Gauss(p, sigma);
		Plane o(p.w, p.h);
		for (size_t i = 0; i < o.v.size(); ++i) o.v[i] = p.v[i] - g.v[i];
		return o;
	}

	// The owner's grain metrics on a grain isolate (two seeds, same truth and coding): RMS ratio and lag-1 increase.
	void GrainIsolate(const std::vector<Plane>& ia, const std::vector<Plane>& ib, const std::vector<Plane>& oa, const std::vector<Plane>& ob, int t0,
		double& rmsRatio, double& lag1Increase)
	{
		std::vector<Plane> gi, go;
		double eIn = 0, eOut = 0, cIn = 0, cOut = 0;
		for (size_t t = static_cast<size_t>(t0); t < ia.size(); ++t)
		{
			Plane di(kW, kH), dd(kW, kH);
			for (size_t i = 0; i < di.v.size(); ++i) { di.v[i] = ia[t].v[i] - ib[t].v[i]; dd.v[i] = oa[t].v[i] - ob[t].v[i]; }
			gi.push_back(Band(di, 1.0)); go.push_back(Band(dd, 1.0));
			for (int y = 8; y < kH - 8; ++y)
				for (int x = kW / 2; x < kW - 8; ++x) { eIn += gi.back().at(x, y) * gi.back().at(x, y); eOut += go.back().at(x, y) * go.back().at(x, y); }
		}
		for (size_t t = 1; t < gi.size(); ++t)
			for (int y = 8; y < kH - 8; ++y)
				for (int x = kW / 2; x < kW - 8; ++x) { cIn += gi[t].at(x, y) * gi[t - 1].at(x, y); cOut += go[t].at(x, y) * go[t - 1].at(x, y); }
		rmsRatio = std::sqrt(eOut / eIn);
		const double scaleN = static_cast<double>(gi.size()) / static_cast<double>(gi.size() - 1);
		lag1Increase = cOut / eOut * scaleN - cIn / eIn * scaleN;
	}
}

namespace VideoProcessorTest
{
	using namespace TemporalDetailGpuTestsNs;

	TEST_CLASS(TemporalDetailGpuTests)
	{
	public:
		TEST_METHOD(GrainSplitGpuMatchesModel)
		{
			// repair.grain_split: the GPU split (s1, s2, f, base; N, e3) follows GrainSplit::Compute, and clean + G = Y.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = Stream(1, "B", 0.0, 0.0, 31u, 1.5, true);
			const Plane& y = frames[0];
			pl_tex tex = UploadLuma(gpu, y);
			GrainSplit::Gpu::Splitter splitter;
			Assert::IsTrue(splitter.Compute(gpu, dev.dispatch, tex, 0, 64.0 / 65535.0, 1.0, 16), Wide(splitter.LastError()).c_str());
			const std::vector<float> s = DownloadFloat(gpu, dev.dispatch, splitter.Out().split), b = DownloadFloat(gpu, dev.dispatch, splitter.Out().blocks);
			const GrainSplit::Split m = GrainSplit::Compute(y, 1.0, 16);
			Assert::IsFalse(s.empty() || b.empty(), L"downloaded");
			double worstS = 0, worstF = 0, worstBase = 0, worstClean = 0, worstN = 0;
			for (size_t i = 0; i < y.v.size(); ++i)
			{
				worstS = (std::max)(worstS, (std::max)(std::fabs(s[i * 4] - m.s1.v[i]), std::fabs(s[i * 4 + 1] - m.s2.v[i])));
				worstF = (std::max)(worstF, std::fabs(s[i * 4 + 2] - m.f.v[i]));
				worstBase = (std::max)(worstBase, std::fabs(s[i * 4 + 3] - m.base.v[i]));
				worstClean = (std::max)(worstClean, std::fabs(s[i * 4 + 3] + s[i * 4] + s[i * 4 + 1] - m.clean.v[i]));
			}
			for (size_t i = 0; i < m.N.size(); ++i) worstN = (std::max)(worstN, std::fabs(b[i * 4] - m.N[i]) / (std::fabs(m.N[i]) + 1e-3));
			LogF("grain split GPU vs model: |s| %.2e, |f| %.2e, |base| %.2e, |clean| %.2e codes, N rel %.2e", worstS, worstF, worstBase, worstClean, worstN);
			Assert::IsTrue(worstS < 2e-3 && worstBase < 2e-3 && worstClean < 2e-3, L"bands within 0.002 codes of the model");
			Assert::IsTrue(worstF < 1e-3 && worstN < 1e-4, L"Wiener share and grain level follow the model");
			splitter.Release(gpu);
			pl_tex_destroy(gpu, &tex);
		}

		TEST_METHOD(TemporalDetailGpuOffIsByteIdentical)
		{
			// Off: the source frame itself (same texture, no pass, nothing allocated).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = Stream(1, "B", 0.0, 0.0, 5u, 1.2);
			pl_tex tex = UploadLuma(gpu, frames[0]);
			Gpu::Module module;
			Gpu::Params p;   // mode off
			pl_frame out{};
			const pl_frame in = LumaFrame(tex);
			Assert::IsTrue(module.Apply(gpu, dev.dispatch, in, p, out));
			Assert::IsTrue(out.planes[0].texture == tex && out.num_planes == in.num_planes, L"off returns the input frame");
			Assert::AreEqual(std::string("off"), module.Status());
			// auto, idle: also the input itself
			p.settings.mode = Mode::Auto;
			p.autoEngaged = false;
			pl_frame idle{};
			Assert::IsTrue(module.Apply(gpu, dev.dispatch, in, p, idle));
			Assert::IsTrue(idle.planes[0].texture == tex, L"auto idle returns the input frame");
			Assert::AreEqual(std::string("auto: idle"), module.Status());
			module.Release(gpu);
			pl_tex_destroy(gpu, &tex);
		}

		TEST_METHOD(TemporalDetailGpuMatchesModel)
		{
			// The GPU passes follow the CPU model on an I/B-coded sub-pixel pan with grain (multi-frame cleaner on, and the
			// single-frame template): every frame within 1/8 code, mean within 0.005 code, no pixel
			// more than a quarter code apart; the model really transfers (so the comparison covers the transfer path).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = Stream(28, "IBBBPBBB", 0.37, 0.21, 11u, 1.2);
			for (int variant = 0; variant < 3; ++variant)
			{
				const int clean = variant == 1 ? 0 : 1;   // 0: cleaner, default boost off; 1: single-frame template; 2: boost on
				Gpu::Params p;
				p.settings = Steady();
				p.settings.boost = variant == 2;
				p.templateClean = clean != 0;
				Model::Tuning tune;
				tune.templateClean = clean != 0;
				const MatchResult r = RunMatch(dev, frames, 0.37, 0.21, p, tune);
				LogF("variant %.0f: worst %.4f codes, worst frame mean %.5f, max share over 0.25 %.4f, model changed %.3f of the pixels",
					variant, r.worst, r.worstMean, r.maxOverShare, r.changedShare);
				Assert::IsTrue(r.changedShare > 0.05, L"the model transfers on this scene");
				Assert::IsTrue(r.worst <= 0.125 && r.worstMean <= 0.005 && r.maxOverShare <= 0.0, L"the GPU follows the model");
			}
		}

		TEST_METHOD(TemporalDetailGpuLargeScaleBlocksFollowTheModel)
		{
			// Grain scale above 2160 lines (block 32 / patch 16 at 4320 lines, block 21 / patch 10 at 2880: not a power of two)
			// on a plane that is not a multiple of the block, with an active picture: the GPU follows the model.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const int pw = 150, ph = 110;
			const std::vector<Plane> pic = Stream(14, "IBBBPBBB", 0.45, 0.2, 61u, 1.2, false, 140, 98);
			std::vector<Plane> frames;
			for (const Plane& f : pic)
			{
				Plane full(pw, ph, 30.0);
				for (int y = 0; y < f.h; ++y)
					for (int x = 0; x < f.w; ++x) full.at(5 + x, 6 + y) = f.at(x, y);
				frames.push_back(full);
			}
			const ActiveRect rect = { 5, 6, 145, 104 };
			for (const double lines : { 4320.0, 2880.0 })
			{
				Gpu::Params p;
				p.settings = Steady();
				const MatchResult r = RunMatch(dev, frames, 0.45, 0.2, p, Model::Tuning(), [&](int, Gpu::Params& gp, Model::FrameIn& in) {
					gp.planeLines = lines; in.planeLines = lines;
					gp.active = rect; in.active = rect;
				});
				LogF("%.0f lines: worst %.4f codes, worst frame mean %.5f, model changed %.3f", lines, r.worst, r.worstMean, r.changedShare);
				// float vs double near a smooth gate edge can flip a few pixels for a frame (measured: 13 px, 0.36 code, gone the
				// next frame), so this case allows half a code on at most 0.1% of the pixels.
				Assert::IsTrue(r.worst <= 0.5 && r.worstMean <= 0.01 && r.maxOverShare <= 0.001, L"the GPU follows the model at a large grain scale");
			}
		}

		TEST_METHOD(TemporalDetailGpuBlockGatesFollowTheModel)
		{
			// Regionally adaptive gates (owner 2026-10-31): per-block factors on the margin, base allowance, uncertainty limit
			// and strength (here: left half stricter and weaker, a band with a loose uncertainty limit) reach the GPU exactly as
			// the model, and they change the result (so the inputs are really used).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = Stream(16, "IBBBPBBB", 0.37, 0.0, 19u, 1.2);
			const int bw = (kW + 15) / 16, bh = (kH + 15) / 16;
			std::vector<float> gates(static_cast<size_t>(bw) * bh * 4, 1.0f);
			for (int by = 0; by < bh; ++by)
				for (int bx = 0; bx < bw; ++bx)
				{
					float* g = &gates[(static_cast<size_t>(by) * bw + bx) * 4];
					if (bx < bw / 2) { g[0] = 2.0f; g[1] = 0.7f; g[3] = 0.5f; }
					if (by == 2) g[2] = 3.0f;
				}
			pl_tex_params tp{};
			tp.w = bw; tp.h = bh; tp.format = pl_find_named_fmt(gpu, "rgba32f"); tp.sampleable = true; tp.initial_data = gates.data();
			pl_tex gtex = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(gtex);
			Gpu::Params p;
			p.settings = Steady();
			const MatchResult r = RunMatch(dev, frames, 0.37, 0.0, p, Model::Tuning(), [&](int, Gpu::Params& gp, Model::FrameIn& in) {
				gp.blockGates = gtex;
				in.blockGates = gates;
			});
			const MatchResult neutral = RunMatch(dev, frames, 0.37, 0.0, p, Model::Tuning());
			double diff = 0.0;
			for (size_t t = 0; t < frames.size(); ++t)
				for (size_t i = 0; i < r.model[t].v.size(); ++i) diff = (std::max)(diff, std::fabs(r.model[t].v[i] - neutral.model[t].v[i]));
			LogF("block gates: worst %.4f codes, worst frame mean %.5f; differs from the global gates by up to %.3f codes", r.worst, r.worstMean, diff);
			Assert::IsTrue(r.worst <= 0.125 && r.worstMean <= 0.005 && r.maxOverShare <= 0.0, L"the GPU follows the model with per-block gates");
			Assert::IsTrue(diff > 0.1, L"the per-block gates change the result");
			pl_tex_destroy(gpu, &gtex);
		}

		TEST_METHOD(TemporalDetailGpuSlotTierFollowsTheModel)
		{
			// The reference-store memory tier: with 2 slots the GPU follows the model with Tuning::slots = 2, says so in its
			// status, and the texture budget shrinks (SlotsForBudget picks fewer slots for a smaller budget).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = Stream(20, "IBBBPBBB", 0.37, 0.0, 13u, 1.2);
			Gpu::Params p;
			p.settings = Steady();
			p.slots = 2;
			Model::Tuning tune;
			tune.slots = 2;
			const MatchResult r = RunMatch(dev, frames, 0.37, 0.0, p, tune);
			LogF("2 slots: worst %.4f codes, worst frame mean %.5f, model changed %.3f", r.worst, r.worstMean, r.changedShare);
			Assert::IsTrue(r.worst <= 0.125 && r.worstMean <= 0.005 && r.maxOverShare <= 0.0, L"the GPU follows the model at 2 slots");
			const uint64_t full = Gpu::Module::TextureBytes(3840, 2160, 2160), two = Gpu::Module::TextureBytes(3840, 2160, 2160, 2);
			LogF("4K textures: %.0f MB at 5 slots, %.0f MB at 2", full / 1048576.0, two / 1048576.0);
			Assert::IsTrue(two < full, L"fewer slots, less memory");
			Assert::AreEqual(5, Gpu::Module::SlotsForBudget(full, 3840, 2160, 2160));
			Assert::AreEqual(2, Gpu::Module::SlotsForBudget(two, 3840, 2160, 2160));
			Assert::AreEqual(1, Gpu::Module::SlotsForBudget(1, 3840, 2160, 2160));
		}

		TEST_METHOD(TemporalDetailGpuResetsRepeatsAndMotionLossFollowTheModel)
		{
			// A cut, a stamp change, a lost motion pair and a repeat on the GPU follow the model's resets (statuses equal,
			// outputs within the match tolerance); a repeat returns the same texture without advancing.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = Stream(22, "IBBBPBBB", 0.5, 0.0, 17u, 1.2);
			Gpu::Params p;
			p.settings = Steady();
			Model::Tuning tune;
			const MatchResult r = RunMatch(dev, frames, 0.5, 0.0, p, tune, [](int t, Gpu::Params& gp, Model::FrameIn& in) {
				if (t == 9) { gp.cut = true; in.cut = true; }
				if (t == 14) { gp.stamps.rebaseH = 1; in.stamps.rebaseH = 1; }
				if (t >= 14) { gp.stamps.rebaseH = 1; in.stamps.rebaseH = 1; }
				if (t == 18) { gp.backward.tex = nullptr; in.motion = nullptr; }
			});
			LogF("resets: worst %.4f codes, worst frame mean %.5f", r.worst, r.worstMean);
			Assert::IsTrue(r.worst <= 0.125 && r.worstMean <= 0.005 && r.maxOverShare <= 0.0, L"the GPU follows the model through resets");

			pl_gpu gpu = dev.Gpu();
			Gpu::Module module;
			const MotionPair bwd = MotionPair::Uniform(kW, kH, 16.0, 0.0, 0.0);
			pl_tex bwdTex = UploadPair(gpu, bwd);
			pl_frame first{}, again{};
			for (int t = 0; t < 3; ++t)
			{
				pl_tex tex = UploadLuma(gpu, frames[static_cast<size_t>(t)]);
				Gpu::Params gp;
				gp.settings = Steady();
				gp.seq = static_cast<uint64_t>(t);
				gp.backward.tex = bwdTex;
				pl_frame out{};
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, out), Wide(module.LastError()).c_str());
				if (t == 2)
				{
					first = out;
					const Plane a = DownloadLuma(gpu, out.planes[0].texture);
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, again));
					const Plane b = DownloadLuma(gpu, again.planes[0].texture);
					Assert::IsTrue(first.planes[0].texture == again.planes[0].texture && a.v == b.v, L"a repeat returns the last output");
				}
				pl_tex_destroy(gpu, &tex);
			}
			module.Release(gpu);
			pl_tex_destroy(gpu, &bwdTex);
		}

		TEST_METHOD(TemporalDetailGpuDeterministic)
		{
			// Same input, same output, bit for bit (fixed reduction orders, no atomics), and counters equal.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = Stream(12, "IBBBPBBB", 0.3, 0.1, 23u, 1.2);
			const MotionPair bwd = MotionPair::Uniform(kW, kH, 16.0, -0.3, -0.1);
			pl_tex bwdTex = UploadPair(gpu, bwd);
			std::vector<Plane> outs[2];
			Gpu::Counters counters[2];
			for (int run = 0; run < 2; ++run)
			{
				Gpu::Module module;
				for (size_t t = 0; t < frames.size(); ++t)
				{
					pl_tex tex = UploadLuma(gpu, frames[t]);
					Gpu::Params gp;
					gp.settings = Steady();
					gp.seq = t;
					gp.backward.tex = bwdTex;
					pl_frame out{};
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, out), Wide(module.LastError()).c_str());
					outs[run].push_back(DownloadLuma(gpu, out.planes[0].texture));
					pl_tex_destroy(gpu, &tex);
				}
				Assert::IsTrue(module.ReadCounters(gpu, counters[run]), L"counters read");
				module.Release(gpu);
			}
			for (size_t t = 0; t < frames.size(); ++t) Assert::IsTrue(outs[0][t].v == outs[1][t].v, L"bit-identical runs");
			Assert::AreEqual(counters[0].liveSlots, counters[1].liveSlots);
			Assert::AreEqual(counters[0].guardTrips, counters[1].guardTrips);
			LogF("counters: live slots %.0f, refreshes %.0f, guard trips %.0f, add-back blocks %.0f", counters[0].liveSlots, counters[0].refreshes,
				counters[0].guardTrips, counters[0].addBackBlocks);
			pl_tex_destroy(gpu, &bwdTex);
		}

		TEST_METHOD(TemporalDetailGpuReadsTheSharedGrainSplit)
		{
			// Owner rule (one producer, many readers): with the shared grain.split product of the frame the module computes no
			// split of its own and the output is bit-identical to its own-split output; a product that does not fit (another
			// block size) falls back visibly.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = Stream(10, "IBBBPBBB", 0.3, 0.0, 29u, 1.2);
			pl_tex bwdTex = UploadPair(gpu, MotionPair::Uniform(kW, kH, 16.0, -0.3, 0.0));
			Gpu::Module own, reader;
			GrainSplit::Gpu::Splitter producer, wrong;
			for (size_t t = 0; t < frames.size(); ++t)
			{
				pl_tex tex = UploadLuma(gpu, frames[t]);
				Gpu::Params gp;
				gp.settings = Steady();
				gp.seq = t;
				gp.planeLines = 1080.0;
				gp.backward.tex = bwdTex;
				pl_frame a{}, b{};
				Assert::IsTrue(own.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, a), Wide(own.LastError()).c_str());
				const Plane pa = DownloadLuma(gpu, a.planes[0].texture);
				Assert::IsTrue(own.Status().find("own grain split") != std::string::npos, L"no product: the module's own split, visible");
				Assert::IsTrue(producer.Compute(gpu, dev.dispatch, tex, 0, 64.0 / 65535.0, 1.0, 16), Wide(producer.LastError()).c_str());
				RegionEvidence::GrainProduct shared;
				shared.gpu[0] = &producer.Out();
				gp.grain = RegionEvidence::GrainView(&shared);
				Assert::IsTrue(gp.grain.Valid() && !gp.grain.CpuValid(), L"the GPU form alone makes a readable product");
				Assert::IsTrue(reader.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, b), Wide(reader.LastError()).c_str());
				const Plane pb = DownloadLuma(gpu, b.planes[0].texture);
				Assert::IsTrue(reader.Status().find("own grain split") == std::string::npos, L"the shared product is read");
				Assert::IsTrue(pa.v == pb.v, L"bit-identical to the module's own split");
				pl_tex_destroy(gpu, &tex);
			}
			{
				pl_tex tex = UploadLuma(gpu, frames[0]);
				Assert::IsTrue(wrong.Compute(gpu, dev.dispatch, tex, 0, 64.0 / 65535.0, 1.0, 32));
				Gpu::Module m;
				Gpu::Params gp;
				gp.settings = Steady();
				RegionEvidence::GrainProduct misfit;
				misfit.gpu[0] = &wrong.Out();
				gp.grain = RegionEvidence::GrainView(&misfit);
				pl_frame o{};
				Assert::IsTrue(m.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, o), Wide(m.LastError()).c_str());
				Assert::IsTrue(m.Status().find("does not fit") != std::string::npos, L"a product that does not fit falls back visibly");
				m.Release(gpu);
				pl_tex_destroy(gpu, &tex);
			}
			own.Release(gpu); reader.Release(gpu); producer.Release(gpu); wrong.Release(gpu);
			pl_tex_destroy(gpu, &bwdTex);
		}

		TEST_METHOD(TemporalDetailGpuKeepsGrain)
		{
			// The owner's grain bounds on the GPU output (two-seed grain isolate of the static KeepsGrain scene, textured
			// half): grain RMS within 5%, lag-1 correlation increase <= 0.05; and pixels with nothing to add are copied
			// unchanged (the current grain added back exactly).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const int n = 40;
			const std::vector<Plane> a = Stream(n, "IBBBPBBB", 0.0, 0.0, 101u, 1.2, true), b = Stream(n, "IBBBPBBB", 0.0, 0.0, 202u, 1.2, true);
			const MotionPair bwd = MotionPair::Uniform(kW, kH, 16.0, 0.0, 0.0);
			pl_tex bwdTex = UploadPair(gpu, bwd);
			std::vector<Plane> oa, ob;
			int copied = 0, total = 0;
			for (int run = 0; run < 2; ++run)
			{
				const std::vector<Plane>& in = run ? b : a;
				Gpu::Module module;
				for (size_t t = 0; t < in.size(); ++t)
				{
					pl_tex tex = UploadLuma(gpu, in[t]);
					Gpu::Params gp;
					gp.settings = Steady();
					gp.seq = t;
					gp.contentHz = kHz;
					gp.backward.tex = bwdTex;
					pl_frame out{};
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, out), Wide(module.LastError()).c_str());
					(run ? ob : oa).push_back(DownloadLuma(gpu, out.planes[0].texture));
					if (run == 0)
						for (size_t i = 0; i < in[t].v.size(); ++i) { copied += oa.back().v[i] == in[t].v[i] ? 1 : 0; ++total; }
					pl_tex_destroy(gpu, &tex);
				}
				module.Release(gpu);
			}
			double rms = 0, lag = 0;
			GrainIsolate(a, b, oa, ob, 16, rms, lag);
			LogF("GPU grain isolate: RMS ratio %.4f, lag-1 increase %.4f; %.1f%% of the pixels copied unchanged", rms, lag, 100.0 * copied / total);
			Assert::IsTrue(std::fabs(rms - 1.0) <= 0.05, L"grain RMS within 5%");
			Assert::IsTrue(lag <= 0.05, L"no frozen grain (lag-1 increase <= 0.05)");
			Assert::IsTrue(copied > 0, L"pixels with nothing to add are copied exactly");
			pl_tex_destroy(gpu, &bwdTex);
		}

		TEST_METHOD(TemporalDetailGpuActivePictureKeepsBars)
		{
			// Owner 2026-10-31: letterbox and pillarbox pictures with a pan next to the bar. Bar pixels stay bit-exact on the
			// GPU, the picture follows the model (which treats the bar boundary as the plane edge), and the picture's output
			// does not depend on the bar level (no tap across the boundary).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			struct Case { const char* name; int pw, ph; ActiveRect rect; };
			const Case cases[] = { { "letterbox", kW, kH + 32, { 0, 16, kW, kH + 16 } }, { "pillarbox", kW + 32, kH, { 16, 0, kW + 16, kH } } };
			const std::vector<Plane> pic = Stream(20, "IBBBPBBB", 0.6, 0.0, 41u, 1.2);
			for (const Case& c : cases)
			{
				std::vector<Plane> picOut[2];
				for (int bar = 0; bar < 2; ++bar)
				{
					std::vector<Plane> frames;
					for (const Plane& f : pic)
					{
						Plane full(c.pw, c.ph, bar ? 900.0 : 16.0);
						for (int y = 0; y < kH; ++y)
							for (int x = 0; x < kW; ++x) full.at(c.rect.left + x, c.rect.top + y) = f.at(x, y);
						frames.push_back(full);
					}
					Gpu::Params p;
					p.settings = Steady();
					const MatchResult r = RunMatch(dev, frames, 0.6, 0.0, p, Model::Tuning(), [&](int, Gpu::Params& gp, Model::FrameIn& in) {
						gp.active = c.rect;
						in.active = c.rect;
					});
					LogF("active picture: worst %.4f codes, worst frame mean %.5f, model changed %.3f", r.worst, r.worstMean, r.changedShare);
					Assert::IsTrue(r.worst <= 0.125 && r.worstMean <= 0.005 && r.maxOverShare <= 0.0, L"the GPU follows the model with an active picture");
					Assert::IsTrue(r.changedShare > 0.02, L"the transfer runs inside the picture");
					for (size_t t = 0; t < frames.size(); ++t)
					{
						Plane po(kW, kH);
						for (int y = 0; y < c.ph; ++y)
							for (int x = 0; x < c.pw; ++x)
							{
								const bool inside = x >= c.rect.left && x < c.rect.right && y >= c.rect.top && y < c.rect.bottom;
								if (!inside) Assert::IsTrue(r.gpu[t].at(x, y) == frames[t].at(x, y), L"bar pixels bit-exact on the GPU");
								else po.at(x - c.rect.left, y - c.rect.top) = r.gpu[t].at(x, y);
							}
						picOut[bar].push_back(po);
					}
				}
				double dep = 0.0;
				for (size_t t = 0; t < pic.size(); ++t)
					for (size_t i = 0; i < picOut[0][t].v.size(); ++i) dep = (std::max)(dep, std::fabs(picOut[0][t].v[i] - picOut[1][t].v[i]));
				LogF("picture output vs bar level: %.4f codes", dep);
				Assert::IsTrue(dep == 0.0, L"the picture does not depend on the bars");
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(TemporalDetailGpuTiming)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TemporalDetailGpuTiming)
		{
			// Hardware only (VP_TEST_GPU=hardware): Apply at 3840x2160 and 1920x1080 on a panning, I/B-coded, grainy scene,
			// median wall time with pl_gpu_finish per frame, split into frames with and without an admission (the cleaner).
			if (!ExtendedRangeGpuProbe::OnHardwareGpu())
			{
				Logger::WriteMessage("TemporalDetailGpuTiming: VP_TEST_GPU=hardware only; not run");
				return;
			}
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const int sizes[2][2] = { { 3840, 2160 }, { 1920, 1080 } };
			for (const auto& sz : sizes)
			{
				const int w = sz[0], h = sz[1];
				std::vector<pl_tex> texs;
				Noise noise(77u);
				for (int t = 0; t < 8; ++t)
				{
					Plane f = Code(Sample(0.5 * t, 0.25 * t, w, h), QualityOf("IBBBPBBB"[t]));
					for (double& v : f.v) v += 1.2 * noise.Next();
					texs.push_back(UploadLuma(gpu, Quantised(f)));
				}
				pl_tex bwdTex = UploadPair(gpu, MotionPair::Uniform(w, h, 16.0, -0.5, -0.25));
				Gpu::Module module;
				std::vector<double> msAdmit, msOther;
				for (int k = 0; k < 48; ++k)
				{
					Gpu::Params gp;
					gp.settings = Steady();
					gp.seq = static_cast<uint64_t>(k);
					gp.contentHz = kHz;
					gp.backward.tex = bwdTex;
					pl_frame out{};
					pl_gpu_finish(gpu);
					const auto t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(texs[static_cast<size_t>(k % 8)]), gp, out), Wide(module.LastError()).c_str());
					pl_gpu_finish(gpu);
					const auto t1 = std::chrono::steady_clock::now();
					Gpu::Counters c;
					module.ReadCounters(gpu, c);
					if (k >= 8) (c.admitted ? msAdmit : msOther).push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				}
				auto median = [](std::vector<double> v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
				char text[220];
				std::snprintf(text, sizeof text, "temporal detail %dx%d: median %.3f ms on admission frames (%zu), %.3f ms otherwise (%zu); textures %.0f MB", w, h,
					median(msAdmit), msAdmit.size(), median(msOther), msOther.size(), Gpu::Module::TextureBytes(w, h, h) / 1048576.0);
				Logger::WriteMessage(text);
				module.Release(gpu);
				pl_tex_destroy(gpu, &bwdTex);
				for (pl_tex& t : texs) pl_tex_destroy(gpu, &t);
			}
		}
	};
}
