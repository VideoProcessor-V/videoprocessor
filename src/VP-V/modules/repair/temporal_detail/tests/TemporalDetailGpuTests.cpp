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
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/control/skeleton/gpu/OwnerGpuDevice.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/GrainSplitVariantOwner.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailGpu.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailModel.h>
#include <VP-V/core/Ramp.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
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
	// Compact vs Full shared grain product: worst output difference allowed, in codes (WARP-measured value + margin).
	constexpr double kCompactVsFullMaxCodes = 0.05;

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
		return 1.0 - RampMath::Smoothstep<double>(13.0, 16.0, std::fabs(m - 32.0), RampMath::SmoothRule::Unguarded);
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

	// WAIVER-B2-TD-OWNSPLIT: the module computes no grain split of its own; with no fitting shared product it Acquires the
	// owner's variant from the skeleton memo. The rig is the test's skeleton side: the REAL P13 source-plane provider
	// (input/capture SourcePlanes: its owner fill, the frame's luma offered for this rig's host), the skeleton's owner GPU device
	// (the sanctioned GPU context of the grain split fill: device, dispatch, textures) and repair.grain_split's variant owner,
	// registered on a memo with a GPU dispatcher and a budget. Per frame: Begin (the source plane offered) before the modules
	// run, End (the frame sealed, the offers dropped, the released textures destroyed) after them. One rig per stream: a sealed
	// frame is never served again (FrameReleased).
	// WAIVER-C5-GSVO-RIG: the stand-in and the owner's captured device are gone (C5 MUST-2, option 1), so the rig wires the
	// production chain instead (P13 owner + OwnerGpuDevice); no assertion changes.
	struct MemoRig
	{
		Vpv::Skeleton::OwnerGpuDevice device;
		GrainSplit::Gpu::VariantOwner owner;
		VpSkeleton::SkeletonMemo memo;
		// WAIVER-C5-GF-T2 (GF-P1 MUST-5): Register takes the largest observed luma (w x h; the stream's size) and the owner sets
		// reserveBytes from it, so the memo's planned need is non-zero before the first fill. `planned` sets the ledger grant to
		// that need (memo.GpuNeedBytes()) instead of the roomy 4 GiB, as the module's grant does.
		explicit MemoRig(GpuDevice& dev, int w = kW, int h = kH, bool planned = false) : device(dev.Gpu())
		{
			memo.SetHost(this);
			memo.SetGpuDispatcherPresent(true);
			memo.SetGpuBudget(true, uint64_t(1) << 32);
			memo.SetGpuDevice(&device);
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Gpu, Vpv::Capture::SourcePlanes::kUnit,
				Vpv::Capture::SourcePlanes::MakeOwnerFill()) == VpSkeleton::SkeletonMemo::Registration::Registered, L"the P13 source-plane owner registers its fill");
			Assert::IsTrue(owner.Register(memo, w, h) == VpSkeleton::SkeletonMemo::Registration::Registered, L"the grain split owner registers its fill");
			if (planned)
				memo.SetGpuBudget(true, memo.GpuNeedBytes());
		}
		~MemoRig()
		{
			memo.ReleaseGpuSpots();   // the spots hand their textures back to the device first
			device.Release();         // then the device destroys them (the GPU is still alive here)
			Vpv::Capture::SourcePlanes::Forget(this);
		}
		MemoRig(const MemoRig&) = delete;
		MemoRig& operator=(const MemoRig&) = delete;
		void Begin(uint64_t seq, pl_tex tex)
		{
			Vpv::Capture::SourcePlaneSet set;
			set.seq = seq;
			set.planes = 1;
			set.tex[0] = tex;
			set.components[0] = 1;
			set.repr = LumaFrame(tex).repr;
			set.origin = "temporal detail GPU test rig";
			Assert::IsTrue(Vpv::Capture::SourcePlanes::Offer(this, set), L"the frame's luma is offered to P13");
		}
		void End(uint64_t seq)
		{
			memo.OnSeal(seq);
			Vpv::Capture::SourcePlanes::BeginFrame(this);
			device.FlushReleases();
		}
	};

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
		MemoRig rig(dev);
		GrainSplit::Gpu::Splitter cropSplit;
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
			p.memo = &rig.memo;
			// An active-picture crop is a variant the owner does not serve (VariantUndeclared): the producer side supplies the
			// cropped plane's split as the shared product, as a lane does (the same law, scale and code unit the module reads).
			const ActiveRect rect = p.active.Resolve(w, h);
			const bool whole = rect.left == 0 && rect.top == 0 && rect.right == w && rect.bottom == h;
			RegionEvidence::GrainProduct cropProduct;
			pl_tex cropTex = nullptr;
			if (!whole && !p.grain.Valid())
			{
				Plane crop(rect.Width(), rect.Height());
				for (int y = 0; y < crop.h; ++y)
					for (int x = 0; x < crop.w; ++x) crop.at(x, y) = frames[t].at(rect.left + x, rect.top + y);
				cropTex = UploadLuma(gpu, crop);
				const double scale = Scale(p.planeLines > 0.0 ? p.planeLines : h);
				GrainSplit::Gpu::Options o;
				o.seq = t;
				Assert::IsTrue(cropSplit.Compute(gpu, dev.dispatch, cropTex, 0, VpvGpu::CodeUnit(cropTex, LumaFrame(cropTex).repr.bits, p.colorDepth), scale,
					GrainSplit::Gpu::BlockFor(scale), GrainSplit::DefaultParams(), o), Wide(cropSplit.LastError()).c_str());
				cropProduct.gpu[0] = &cropSplit.Out();
				p.grain = RegionEvidence::GrainView(&cropProduct);
				p.grainSeq = t;
			}
			rig.Begin(t, tex);
			pl_frame out{};
			const bool ok = module.Apply(gpu, dev.dispatch, LumaFrame(tex), p, out);
			rig.End(t);
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
			if (cropTex) pl_tex_destroy(gpu, &cropTex);
		}
		r.changedShare = changed / static_cast<double>(frames.size());
		module.Release(gpu);
		cropSplit.Release(gpu);
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

	// --- The shared grain product: absent, misfit and Compact (SA2 B3) ---
	enum class TdShared { Absent, EmptyProduct, Full, Compact, WrongBlock, WrongUnit, WrongComponent, StaleSeq, StaleVersion, Shed };

	struct TdGrainRun
	{
		std::vector<Plane> out;
		std::vector<GrainSplit::GrainStatus> status;
		std::vector<bool> engaged;
		std::vector<std::string> text, id;
	};

	// One module over frames of a panning grainy scene; frame t reads the product kind pick(t) (built on that frame's luma).
	TdGrainRun RunTdGrain(GpuDevice& dev, const std::vector<Plane>& frames, const std::function<TdShared(size_t)>& pick)
	{
		pl_gpu gpu = dev.Gpu();
		pl_tex bwdTex = UploadPair(gpu, MotionPair::Uniform(kW, kH, 16.0, -0.3, 0.0));
		Gpu::Module module;
		GrainSplit::Gpu::Splitter full, compact, wrongBlock, wrongUnit, wrongComp, stale;
		GrainSplit::Gpu::Frame tampered;
		const double cu = 64.0 / 65535.0;
		TdGrainRun r;
		MemoRig rig(dev);
		for (size_t t = 0; t < frames.size(); ++t)
		{
			pl_tex tex = UploadLuma(gpu, frames[t]);
			const TdShared kind = pick(t);
			RegionEvidence::GrainProduct product;
			switch (kind)
			{
			case TdShared::Full:
				Assert::IsTrue(full.Compute(gpu, dev.dispatch, tex, 0, cu, 1.0, 16), Wide(full.LastError()).c_str());
				product.gpu[0] = &full.Out();
				break;
			case TdShared::Compact:
			{
				GrainSplit::Gpu::Options o;
				o.storage = GrainSplit::Storage::Compact;
				Assert::IsTrue(compact.Compute(gpu, dev.dispatch, tex, 0, cu, 1.0, 16, GrainSplit::Params(), o), Wide(compact.LastError()).c_str());
				product.gpu[0] = &compact.Out();
				break;
			}
			case TdShared::Shed:   // a good Full product that the governor dropped: the module must not read it
				Assert::IsTrue(full.Compute(gpu, dev.dispatch, tex, 0, cu, 1.0, 16), Wide(full.LastError()).c_str());
				product.gpu[0] = &full.Out();
				break;
			case TdShared::StaleSeq:   // computed for another source frame than the one being presented
			{
				GrainSplit::Gpu::Options o;
				o.seq = t + 100;
				Assert::IsTrue(stale.Compute(gpu, dev.dispatch, tex, 0, cu, 1.0, 16, GrainSplit::Params(), o), Wide(stale.LastError()).c_str());
				product.gpu[0] = &stale.Out();
				break;
			}
			case TdShared::StaleVersion:   // made by another version of the grain split unit
			{
				GrainSplit::Gpu::Options o;
				o.seq = t;
				Assert::IsTrue(stale.Compute(gpu, dev.dispatch, tex, 0, cu, 1.0, 16, GrainSplit::Params(), o), Wide(stale.LastError()).c_str());
				tampered = stale.Out();
				tampered.version = GrainSplit::kUnitVersion + 1;
				product.gpu[0] = &tampered;
				break;
			}
			case TdShared::WrongBlock:
				Assert::IsTrue(wrongBlock.Compute(gpu, dev.dispatch, tex, 0, cu, 1.0, 32), Wide(wrongBlock.LastError()).c_str());
				product.gpu[0] = &wrongBlock.Out();
				break;
			case TdShared::WrongUnit:
				Assert::IsTrue(wrongUnit.Compute(gpu, dev.dispatch, tex, 0, cu * 2.0, 1.0, 16), Wide(wrongUnit.LastError()).c_str());
				product.gpu[0] = &wrongUnit.Out();
				break;
			case TdShared::WrongComponent:
				Assert::IsTrue(wrongComp.Compute(gpu, dev.dispatch, tex, 1, cu, 1.0, 16), Wide(wrongComp.LastError()).c_str());
				product.gpu[0] = &wrongComp.Out();
				break;
			default:
				break;   // Absent: no view at all; EmptyProduct: a view of a product with neither form (invalid)
			}
			Gpu::Params gp;
			gp.settings = Steady();
			gp.seq = t;
			gp.planeLines = 1080.0;
			gp.backward.tex = bwdTex;
			if (kind != TdShared::Absent) gp.grain = RegionEvidence::GrainView(&product);
			if (kind == TdShared::StaleSeq || kind == TdShared::StaleVersion) gp.grainSeq = t;
			if (kind == TdShared::Shed) gp.grainShed = true;
			gp.memo = &rig.memo;
			pl_frame o{};
			rig.Begin(t, tex);
			const bool ok = module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, o);
			rig.End(t);
			Assert::IsTrue(ok, Wide(module.LastError()).c_str());
			r.out.push_back(DownloadLuma(gpu, o.planes[0].texture));
			r.status.push_back(module.LastGrainStatus());
			const FallbackCatalog::Decision d = module.GrainFallbackDecision();
			r.engaged.push_back(d.engaged);
			r.id.push_back(d.id);
			r.text.push_back(module.Status());
			pl_tex_destroy(gpu, &tex);
		}
		module.Release(gpu);
		for (GrainSplit::Gpu::Splitter* sp : { &full, &compact, &wrongBlock, &wrongUnit, &wrongComp, &stale }) sp->Release(gpu);
		pl_tex_destroy(gpu, &bwdTex);
		return r;
	}

	std::vector<Plane> TdGrainFrames() { return Stream(8, "IBBBPBBB", 0.3, 0.0, 29u, 1.2); }

	double WorstDiff(const std::vector<Plane>& a, const std::vector<Plane>& b)
	{
		double worst = 0.0;
		for (size_t t = 0; t < a.size(); ++t)
			for (size_t i = 0; i < a[t].v.size(); ++i) worst = (std::max)(worst, std::fabs(a[t].v[i] - b[t].v[i]));
		return worst;
	}

	// SA6: the same grainy stream through one module, with or without the scratch arena. Frame t reads a shared Full grain product
	// (built on that frame's luma) when sharedAt(t), else the owner's memo variant. The arena counters are read before the arena
	// is released.
	struct TdArenaRun
	{
		std::vector<Plane> out;
		std::vector<std::string> status;
		unsigned long long failures = 0;
		uint64_t acquired = 0, exhausted = 0;
	};

	TdArenaRun RunTdArena(GpuDevice& dev, const std::vector<Plane>& frames, bool useArena, const std::function<bool(size_t)>& sharedAt, double contentHz = 0.0)
	{
		pl_gpu gpu = dev.Gpu();
		pl_tex bwdTex = UploadPair(gpu, MotionPair::Uniform(kW, kH, 16.0, -0.3, 0.0));
		VpSkeleton::PlTexBackend backend(gpu);
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
		if (useArena)
		{
			const auto requests = Gpu::Module::ArenaRequests(gpu, kW, kH, 1080.0);
			Assert::IsFalse(requests.empty(), L"the float formats map to arena formats");
			Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(requests, VpSkeleton::Tier::T2)), L"arena reserve");
		}
		Gpu::Module module;
		GrainSplit::Gpu::Splitter producer;
		TdArenaRun r;
		MemoRig rig(dev);
		for (size_t t = 0; t < frames.size(); ++t)
		{
			pl_tex tex = UploadLuma(gpu, frames[t]);
			RegionEvidence::GrainProduct product;
			const bool shared = sharedAt(t);
			if (shared)
			{
				Assert::IsTrue(producer.Compute(gpu, dev.dispatch, tex, 0, 64.0 / 65535.0, 1.0, 16), Wide(producer.LastError()).c_str());
				product.gpu[0] = &producer.Out();
			}
			Gpu::Params gp;
			gp.settings = Steady();
			gp.seq = t;
			gp.planeLines = 1080.0;
			gp.contentHz = contentHz;
			gp.backward.tex = bwdTex;
			if (shared) gp.grain = RegionEvidence::GrainView(&product);
			gp.memo = &rig.memo;
			if (useArena)
			{
				gp.arena = &arena;
				arena.BeginFrame();
			}
			pl_frame o{};
			rig.Begin(t, tex);
			const bool ok = module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, o);
			rig.End(t);
			Assert::IsTrue(ok, Wide(module.LastError()).c_str());
			if (useArena)
			{
				Assert::IsFalse(module.ScratchShort(), L"no shortage");
				Assert::IsFalse(arena.FrameDecision().engaged, L"scratch_arena released");
			}
			r.out.push_back(DownloadLuma(gpu, o.planes[0].texture));
			r.status.push_back(module.Status());
			pl_tex_destroy(gpu, &tex);
		}
		r.failures = module.Failures();
		r.acquired = arena.Acquired();
		r.exhausted = arena.Exhausted();
		module.Release(gpu);
		producer.Release(gpu);
		arena.Release();
		pl_tex_destroy(gpu, &bwdTex);
		return r;
	}
	// --- TD-NONFINITE: a shared Full grain product whose block texture carries a planted grain level N (NaN or an infinity) ---
	// One module over frames of a panning grainy scene reading the shared Full product of each frame; when `tamper` is set the
	// product's per-block texture (N, e3, 0, 0) is replaced by a copy with block (bx, by)'s N set to `value`. Returns the outputs.
	// hardAtBlock (gate cb-u1 r2 MUST 3): when set, every frame appends ev_a.x (`hard`) of block (bx, by) for each slot k, read
	// from the module's evidence texture (Gpu::Module::LastEvidenceTexture(0), texel (k x bw + bx, by)); slots = K is returned.
	// channel (gate cb-u1 r3 MUST 2): the block texture component planted, 0 = N (.x), 1 = e3 (.y, which feeds F = e2 / (e2 +
	// e3 + floor)). evA / evB: when set, every frame appends all four components of ev_a / ev_b at block (bx, by), slot-major.
	// radius (cb-u1 U1 red, WAIVER-U1-BLEED): the plant covers every block within `radius` of (bx, by) (clamped to the grid);
	// 0 = the one block, as before. guard: Gpu::Params::guard (default on, as before). The transfer blends the four nearest
	// blocks' terms bilinearly (kTransferBody: g = (p + 0.5) / B - 0.5), so every pixel of block b also takes 34-50 % of its
	// change from blocks b +- 1; radius 1 closes the whole support of block b's pixels. The guard's add-back pools tile sums
	// over 3 x 3 blocks and interpolates them bilinearly (a wider support again), so the exact claim runs with it off.
	std::vector<Plane> RunTdPlantedN(GpuDevice& dev, const std::vector<Plane>& frames, bool tamper, float value, int bx, int by,
		std::vector<float>* hardAtBlock = nullptr, int* slots = nullptr, int channel = 0, std::vector<float>* evAAtBlock = nullptr,
		std::vector<float>* evBAtBlock = nullptr, int radius = 0, bool guard = true)
	{
		pl_gpu gpu = dev.Gpu();
		pl_tex bwdTex = UploadPair(gpu, MotionPair::Uniform(kW, kH, 16.0, -0.3, 0.0));
		Gpu::Module module;
		GrainSplit::Gpu::Splitter full;
		const double cu = 64.0 / 65535.0;
		std::vector<Plane> out;
		for (size_t t = 0; t < frames.size(); ++t)
		{
			pl_tex tex = UploadLuma(gpu, frames[t]);
			Assert::IsTrue(full.Compute(gpu, dev.dispatch, tex, 0, cu, 1.0, 16), Wide(full.LastError()).c_str());
			GrainSplit::Gpu::Frame product = full.Out();
			pl_tex planted = nullptr;
			if (tamper)
			{
				std::vector<float> blocks = DownloadFloat(gpu, dev.dispatch, product.blocks);
				Assert::IsTrue(blocks.size() == static_cast<size_t>(product.bw) * static_cast<size_t>(product.bh) * 4, L"the block texture was read back");
				for (int j = (std::max)(0, by - radius); j <= (std::min)(product.bh - 1, by + radius); ++j)
					for (int i = (std::max)(0, bx - radius); i <= (std::min)(product.bw - 1, bx + radius); ++i)
						blocks[(static_cast<size_t>(j) * product.bw + i) * 4 + static_cast<size_t>(channel)] = value;
				pl_tex_params tp{};
				tp.w = product.bw; tp.h = product.bh; tp.format = pl_find_named_fmt(gpu, "rgba32f"); tp.sampleable = true; tp.initial_data = blocks.data();
				planted = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(planted);
				product.blocks = planted;
			}
			RegionEvidence::GrainProduct shared;
			shared.gpu[0] = &product;
			Gpu::Params gp;
			gp.settings = Steady();
			gp.seq = t;
			gp.planeLines = 1080.0;
			gp.backward.tex = bwdTex;
			gp.guard = guard;
			gp.grain = RegionEvidence::GrainView(&shared);
			pl_frame o{};
			Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, o), Wide(module.LastError()).c_str());
			Assert::IsTrue(module.LastGrainStatus() == GrainSplit::GrainStatus::Ring, L"the (planted) shared product was read");
			out.push_back(DownloadLuma(gpu, o.planes[0].texture));
			if (hardAtBlock)
			{
				pl_tex evA = module.LastEvidenceTexture(0);
				Assert::IsNotNull(evA, L"the evidence texture exists after an Apply");
				const int bw = (kW + 15) / 16, texW = evA->params.w, k = texW / bw;
				Assert::IsTrue(k >= 1 && k * bw == texW && evA->params.h == (kH + 15) / 16, L"the evidence texture is (K x bw) x bh");
				const std::vector<float> ev = DownloadFloat(gpu, dev.dispatch, evA);
				Assert::IsTrue(ev.size() == static_cast<size_t>(texW) * static_cast<size_t>(evA->params.h) * 4, L"the evidence texture was read back");
				for (int slot = 0; slot < k; ++slot)
					hardAtBlock->push_back(ev[(static_cast<size_t>(by) * texW + static_cast<size_t>(slot) * bw + bx) * 4]);
				if (slots) *slots = k;
			}
			for (int which = 0; which < 2; ++which)
			{
				std::vector<float>* dst = which == 0 ? evAAtBlock : evBAtBlock;
				if (!dst) continue;
				pl_tex ev = module.LastEvidenceTexture(which);
				Assert::IsNotNull(ev, L"the evidence texture exists after an Apply");
				const int bw = (kW + 15) / 16, texW = ev->params.w, k = texW / bw;
				Assert::IsTrue(k >= 1 && k * bw == texW && ev->params.h == (kH + 15) / 16, L"the evidence texture is (K x bw) x bh");
				const std::vector<float> v = DownloadFloat(gpu, dev.dispatch, ev);
				Assert::IsTrue(v.size() == static_cast<size_t>(texW) * static_cast<size_t>(ev->params.h) * 4, L"the evidence texture was read back");
				for (int slot = 0; slot < k; ++slot)
					for (int c = 0; c < 4; ++c)
						dst->push_back(v[(static_cast<size_t>(by) * texW + static_cast<size_t>(slot) * bw + bx) * 4 + static_cast<size_t>(c)]);
			}
			if (planted) pl_tex_destroy(gpu, &planted);
			pl_tex_destroy(gpu, &tex);
		}
		module.Release(gpu);
		full.Release(gpu);
		pl_tex_destroy(gpu, &bwdTex);
		return out;
	}

	// The largest |a - b| inside a block's core (`inset` px inset, default 2) over all frames; whole = the whole picture instead.
	double WorstInBlock(const std::vector<Plane>& a, const std::vector<Plane>& b, int bx, int by, bool whole = false, int inset = 2)
	{
		double worst = 0.0;
		for (size_t t = 0; t < a.size(); ++t)
			for (int y = 0; y < kH; ++y)
				for (int x = 0; x < kW; ++x)
				{
					if (!whole && (x < bx * 16 + inset || x >= bx * 16 + 16 - inset || y < by * 16 + inset || y >= by * 16 + 16 - inset)) continue;
					worst = (std::max)(worst, std::fabs(a[t].at(x, y) - b[t].at(x, y)));
				}
		return worst;
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
			MemoRig rig(dev);
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
				gp.memo = &rig.memo;
				pl_frame out{};
				rig.Begin(gp.seq, tex);
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, out), Wide(module.LastError()).c_str());
				if (t == 2)
				{
					first = out;
					const Plane a = DownloadLuma(gpu, out.planes[0].texture);
					Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, again));
					const Plane b = DownloadLuma(gpu, again.planes[0].texture);
					Assert::IsTrue(first.planes[0].texture == again.planes[0].texture && a.v == b.v, L"a repeat returns the last output");
				}
				rig.End(gp.seq);
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
				MemoRig rig(dev);
				for (size_t t = 0; t < frames.size(); ++t)
				{
					pl_tex tex = UploadLuma(gpu, frames[t]);
					Gpu::Params gp;
					gp.settings = Steady();
					gp.seq = t;
					gp.backward.tex = bwdTex;
					gp.memo = &rig.memo;
					pl_frame out{};
					rig.Begin(t, tex);
					const bool ok = module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, out);
					rig.End(t);
					Assert::IsTrue(ok, Wide(module.LastError()).c_str());
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
			// Owner rule (one producer, many readers): with the shared grain.split product of the frame the module reads it,
			// and the output is bit-identical to the output on the owner's memo variant (no shared product); a product that
			// does not fit (another block size) falls back visibly.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = Stream(10, "IBBBPBBB", 0.3, 0.0, 29u, 1.2);
			pl_tex bwdTex = UploadPair(gpu, MotionPair::Uniform(kW, kH, 16.0, -0.3, 0.0));
			Gpu::Module own, reader;
			GrainSplit::Gpu::Splitter producer, wrong;
			MemoRig rig(dev);
			for (size_t t = 0; t < frames.size(); ++t)
			{
				pl_tex tex = UploadLuma(gpu, frames[t]);
				Gpu::Params gp;
				gp.settings = Steady();
				gp.seq = t;
				gp.planeLines = 1080.0;
				gp.backward.tex = bwdTex;
				gp.memo = &rig.memo;
				pl_frame a{}, b{};
				rig.Begin(t, tex);
				const bool okOwn = own.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, a);
				rig.End(t);
				Assert::IsTrue(okOwn, Wide(own.LastError()).c_str());
				const Plane pa = DownloadLuma(gpu, a.planes[0].texture);
				Assert::IsTrue(own.Status().find("memo variant") != std::string::npos, L"no product: the owner's memo variant, visible");
				Assert::IsTrue(producer.Compute(gpu, dev.dispatch, tex, 0, 64.0 / 65535.0, 1.0, 16), Wide(producer.LastError()).c_str());
				RegionEvidence::GrainProduct shared;
				shared.gpu[0] = &producer.Out();
				gp.grain = RegionEvidence::GrainView(&shared);
				Assert::IsTrue(gp.grain.Valid() && !gp.grain.CpuValid(), L"the GPU form alone makes a readable product");
				Assert::IsTrue(reader.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, b), Wide(reader.LastError()).c_str());
				const Plane pb = DownloadLuma(gpu, b.planes[0].texture);
				Assert::IsTrue(reader.Status().find("memo variant") == std::string::npos, L"the shared product is read");
				Assert::IsTrue(pa.v == pb.v, L"bit-identical to the owner's memo variant");
				pl_tex_destroy(gpu, &tex);
			}
			{
				pl_tex tex = UploadLuma(gpu, frames[0]);
				Assert::IsTrue(wrong.Compute(gpu, dev.dispatch, tex, 0, 64.0 / 65535.0, 1.0, 32));
				Gpu::Module m;
				MemoRig misfitRig(dev);
				Gpu::Params gp;
				gp.settings = Steady();
				gp.memo = &misfitRig.memo;
				RegionEvidence::GrainProduct misfit;
				misfit.gpu[0] = &wrong.Out();
				gp.grain = RegionEvidence::GrainView(&misfit);
				pl_frame o{};
				misfitRig.Begin(0, tex);
				const bool ok = m.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, o);
				misfitRig.End(0);
				Assert::IsTrue(ok, Wide(m.LastError()).c_str());
				Assert::IsTrue(m.Status().find("does not fit") != std::string::npos, L"a product that does not fit falls back visibly");
				m.Release(gpu);
				pl_tex_destroy(gpu, &tex);
			}
			own.Release(gpu); reader.Release(gpu); producer.Release(gpu); wrong.Release(gpu);
			pl_tex_destroy(gpu, &bwdTex);
		}

		TEST_METHOD(TemporalDetailGpuGrainProductAbsentFallsBackVisibly)
		{
			// No shared product (no view, or a view of a product with neither form): the module reads the owner's memo variant
			// (Full), says so (status, LastGrainStatus Missing, the "grain split (temporal detail)" fallback engaged).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = TdGrainFrames();
			for (const TdShared kind : { TdShared::Absent, TdShared::EmptyProduct })
			{
				const TdGrainRun r = RunTdGrain(dev, frames, [&](size_t) { return kind; });
				for (size_t t = 0; t < frames.size(); ++t)
				{
					Assert::IsTrue(r.status[t] == GrainSplit::GrainStatus::Missing, L"absent product: Missing");
					Assert::IsTrue(r.engaged[t] && r.id[t] == "grain split (temporal detail)", L"the fallback is engaged and named");
					Assert::IsTrue(r.text[t].find("grain split: memo variant, filled at read") != std::string::npos, L"status names the memo variant");
				}
			}
		}

		TEST_METHOD(TemporalDetailGpuGrainProductMisfitFallsBackAsWrongSize)
		{
			// A product of another block size, another code unit or another component does not fit: WrongSize, engaged, status
			// "does not fit"; the output equals the memo-variant output (the owner's variant is what is read).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = TdGrainFrames();
			const TdGrainRun own = RunTdGrain(dev, frames, [](size_t) { return TdShared::Absent; });
			for (const TdShared kind : { TdShared::WrongBlock, TdShared::WrongUnit, TdShared::WrongComponent })
			{
				const TdGrainRun r = RunTdGrain(dev, frames, [&](size_t) { return kind; });
				for (size_t t = 0; t < frames.size(); ++t)
				{
					Assert::IsTrue(r.status[t] == GrainSplit::GrainStatus::WrongSize, L"misfit product: WrongSize");
					Assert::IsTrue(r.engaged[t], L"the fallback is engaged");
					Assert::IsTrue(r.text[t].find("does not fit") != std::string::npos, L"status says the shared product does not fit");
				}
				Assert::IsTrue(WorstDiff(own.out, r.out) == 0.0, L"a misfit product is not read: the output is the memo-variant output");
			}
		}

		TEST_METHOD(TemporalDetailGpuFittingFullProductIsReadThroughTheAccessor)
		{
			// A fitting Full product: Ring, the fallback released, no "memo variant", output bit-identical to the memo variant's.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = TdGrainFrames();
			const TdGrainRun own = RunTdGrain(dev, frames, [](size_t) { return TdShared::Absent; });
			const TdGrainRun shared = RunTdGrain(dev, frames, [](size_t) { return TdShared::Full; });
			for (size_t t = 0; t < frames.size(); ++t)
			{
				Assert::IsTrue(shared.status[t] == GrainSplit::GrainStatus::Ring, L"fitting product: Ring");
				Assert::IsTrue(!shared.engaged[t], L"the fallback is released");
				Assert::IsTrue(shared.text[t].find("memo variant") == std::string::npos, L"the shared product is read");
			}
			Assert::IsTrue(WorstDiff(own.out, shared.out) == 0.0, L"bit-identical to the memo variant");
		}

		TEST_METHOD(TemporalDetailGpuCompactProductIsReadWithinOneCode)
		{
			// A Compact product (rg16f + rg16 unorm) fits and is read through the same accessor: Ring, released, output within
			// one code of the Full-shared output.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = TdGrainFrames();
			const TdGrainRun full = RunTdGrain(dev, frames, [](size_t) { return TdShared::Full; });
			const TdGrainRun compact = RunTdGrain(dev, frames, [](size_t) { return TdShared::Compact; });
			for (size_t t = 0; t < frames.size(); ++t)
			{
				Assert::IsTrue(compact.status[t] == GrainSplit::GrainStatus::Ring && !compact.engaged[t], L"Compact product: Ring, released");
				Assert::IsTrue(compact.text[t].find("memo variant") == std::string::npos, L"the Compact product is read");
			}
			const double worst = WorstDiff(full.out, compact.out);
			LogF("temporal detail: Compact vs Full shared product, worst output difference %.5f codes", worst);
			// Measured on WARP (finisher, 2026-11-01): 0.0156 codes (one fp16 step of s1/s2 at these magnitudes). Bound = measured
			// plus margin (about 3x), so a real precision regression fails instead of hiding under a one-code bound.
			Assert::IsTrue(worst <= kCompactVsFullMaxCodes, L"Compact output within the measured bound of Full");
		}

		TEST_METHOD(TemporalDetailGpuGrainFallbackRecoversAutomatically)
		{
			// absent -> fits -> absent -> misfit -> fits, on one module: engaged, released, engaged, engaged, released. The
			// status is recomputed every frame, and (Full products being bit-identical to the memo variant) the output never differs.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = TdGrainFrames();
			const TdShared seq[] = { TdShared::Absent, TdShared::Full, TdShared::Full, TdShared::Absent, TdShared::WrongBlock, TdShared::Full, TdShared::Compact, TdShared::Absent };
			const bool engaged[] = { true, false, false, true, true, false, false, true };
			const TdGrainRun own = RunTdGrain(dev, frames, [](size_t) { return TdShared::Absent; });
			const TdGrainRun r = RunTdGrain(dev, frames, [&](size_t t) { return seq[t]; });
			for (size_t t = 0; t < frames.size(); ++t)
			{
				Assert::IsTrue(r.engaged[t] == engaged[t], L"engaged exactly while the shared product does not fit");
				if (seq[t] == TdShared::Absent) Assert::IsTrue(r.status[t] == GrainSplit::GrainStatus::Missing, L"Missing");
				else if (seq[t] == TdShared::WrongBlock) Assert::IsTrue(r.status[t] == GrainSplit::GrainStatus::WrongSize, L"WrongSize");
				else Assert::IsTrue(r.status[t] == GrainSplit::GrainStatus::Ring, L"Ring");
			}
			for (size_t t = 0; t < 6; ++t)   // frames before the Compact one are exact (Full or memo variant); Compact is within one code
				Assert::IsTrue(r.out[t].v == own.out[t].v, L"no visible change across the switches");
			Assert::IsTrue(WorstDiff(own.out, r.out) <= kCompactVsFullMaxCodes, L"a Compact frame in the sequence stays within the measured bound");
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
				MemoRig rig(dev);
				for (size_t t = 0; t < in.size(); ++t)
				{
					pl_tex tex = UploadLuma(gpu, in[t]);
					Gpu::Params gp;
					gp.settings = Steady();
					gp.seq = t;
					gp.contentHz = kHz;
					gp.backward.tex = bwdTex;
					gp.memo = &rig.memo;
					pl_frame out{};
					rig.Begin(t, tex);
					const bool ok = module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, out);
					rig.End(t);
					Assert::IsTrue(ok, Wide(module.LastError()).c_str());
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
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu,
				"TemporalDetailGpuTiming", ExtendedRangeGpuProbe::OnHardwareGpu(), "needs VP_TEST_GPU=hardware"))
				return;
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
				MemoRig rig(dev);
				std::vector<double> msAdmit, msOther;
				for (int k = 0; k < 48; ++k)
				{
					Gpu::Params gp;
					gp.settings = Steady();
					gp.seq = static_cast<uint64_t>(k);
					gp.contentHz = kHz;
					gp.backward.tex = bwdTex;
					gp.memo = &rig.memo;
					pl_frame out{};
					rig.Begin(gp.seq, texs[static_cast<size_t>(k % 8)]);
					pl_gpu_finish(gpu);
					// The timed span includes the owner's variant fill (no shared product here): the module's whole cost.
					const auto t0 = std::chrono::steady_clock::now();
					const bool ok = module.Apply(gpu, dev.dispatch, LumaFrame(texs[static_cast<size_t>(k % 8)]), gp, out);
					pl_gpu_finish(gpu);
					const auto t1 = std::chrono::steady_clock::now();
					rig.End(gp.seq);
					Assert::IsTrue(ok, Wide(module.LastError()).c_str());
					Gpu::Counters c;
					module.ReadCounters(gpu, c);
					if (k >= 8) (c.admitted ? msAdmit : msOther).push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				}
				auto median = [](std::vector<double> v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
				const double medAdmit = median(msAdmit), medOther = median(msOther);
				Assert::IsTrue(!msAdmit.empty() || !msOther.empty(), L"at least one timed frame bucket must be populated");
				if (!msAdmit.empty()) Assert::IsTrue(std::isfinite(medAdmit) && medAdmit > 0.0, L"admission median ms must be finite and positive");
				if (!msOther.empty()) Assert::IsTrue(std::isfinite(medOther) && medOther > 0.0, L"non-admission median ms must be finite and positive");
				char text[220];
				std::snprintf(text, sizeof text, "temporal detail %dx%d: median %.3f ms on admission frames (%zu), %.3f ms otherwise (%zu); textures %.0f MB", w, h,
					medAdmit, msAdmit.size(), medOther, msOther.size(), Gpu::Module::TextureBytes(w, h, h) / 1048576.0);
				Logger::WriteMessage(text);
				module.Release(gpu);
				pl_tex_destroy(gpu, &bwdTex);
				for (pl_tex& t : texs) pl_tex_destroy(gpu, &t);
			}
		}

		TEST_METHOD(TemporalDetailGpuStaleProductFallsBackVisibly)
		{
			// A shared product computed for another source frame, or by another version of the grain split unit, is STALE:
			// the module reads the owner's memo variant (output equal to the memo-variant output), the entry is engaged and named, and the
			// next frame with a fitting product recovers by itself.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = TdGrainFrames();
			const TdGrainRun own = RunTdGrain(dev, frames, [](size_t) { return TdShared::Absent; });
			for (const TdShared kind : { TdShared::StaleSeq, TdShared::StaleVersion })
			{
				const TdGrainRun r = RunTdGrain(dev, frames, [&](size_t) { return kind; });
				for (size_t t = 0; t < frames.size(); ++t)
				{
					Assert::IsTrue(r.status[t] == GrainSplit::GrainStatus::Stale, L"stale product: Stale");
					Assert::IsTrue(r.engaged[t] && r.id[t] == "grain split (temporal detail)", L"engaged and named");
				}
				Assert::IsTrue(WorstDiff(own.out, r.out) == 0.0, L"a stale product is not read: the output is the memo-variant output");
			}
			const TdGrainRun rec = RunTdGrain(dev, frames, [](size_t t) { return t < 2 ? TdShared::StaleSeq : t < 5 ? TdShared::Full : TdShared::StaleVersion; });
			for (size_t t = 0; t < frames.size(); ++t)
			{
				const bool stale = t < 2 || t >= 5;
				Assert::IsTrue(rec.engaged[t] == stale, L"engaged exactly while stale: automatic recovery");
				Assert::IsTrue(rec.status[t] == (stale ? GrainSplit::GrainStatus::Stale : GrainSplit::GrainStatus::Ring), L"status follows the product");
			}
		}

		TEST_METHOD(TemporalDetailGpuShedProductFallsBackVisibly)
		{
			// The governor dropped the product (Params::grainShed): even a fitting product is not read, the status is Shed, the
			// entry is engaged, and the output is the memo-variant output; recovery when it is back.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = TdGrainFrames();
			const TdGrainRun own = RunTdGrain(dev, frames, [](size_t) { return TdShared::Absent; });
			const TdGrainRun shed = RunTdGrain(dev, frames, [](size_t) { return TdShared::Shed; });
			for (size_t t = 0; t < frames.size(); ++t)
			{
				Assert::IsTrue(shed.status[t] == GrainSplit::GrainStatus::Shed, L"Shed");
				Assert::IsTrue(shed.engaged[t] && shed.id[t] == "grain split (temporal detail)", L"engaged and named");
			}
			Assert::IsTrue(WorstDiff(own.out, shed.out) == 0.0, L"the shed product is not read");
			const TdGrainRun rec = RunTdGrain(dev, frames, [](size_t t) { return t < 3 ? TdShared::Shed : TdShared::Full; });
			for (size_t t = 0; t < frames.size(); ++t)
				Assert::IsTrue(rec.engaged[t] == (t < 3), L"released the frame the product is back");
		}

		TEST_METHOD(TemporalDetailGpuArenaIsByteIdentical)
		{
			// SA6: the accumulators and the delta from the scratch arena change nothing: the same stream through a module without
			// an arena and with one gives exactly equal planes (the memo variant every frame, the shared product every frame, and
			// the product from frame 3 on).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = TdGrainFrames();
			struct Case { const char* name; std::function<bool(size_t)> shared; };
			const Case cases[] = {
				{ "memo variant", [](size_t) { return false; } },
				{ "shared product", [](size_t) { return true; } },
				{ "product from frame 3", [](size_t t) { return t >= 3; } } };
			for (const Case& c : cases)
			{
				const TdArenaRun priv = RunTdArena(dev, frames, false, c.shared);
				const TdArenaRun ar = RunTdArena(dev, frames, true, c.shared);
				Assert::IsTrue(priv.out.size() == frames.size() && ar.out.size() == frames.size(), L"every frame processed");
				Assert::IsTrue(WorstDiff(priv.out, ar.out) == 0.0, (std::wstring(L"arena output equals private output: ") + Wide(c.name)).c_str());
				Assert::IsTrue(priv.failures == 0 && ar.failures == 0, L"no failures");
				Assert::AreEqual(uint64_t(0), ar.exhausted, L"the arena never ran short");
				Assert::IsTrue(ar.acquired >= 3 * frames.size(), L"the arena was really used (two accumulators and the delta per frame)");
				LogF("%.0f frames identical, arena acquired %.0f", static_cast<double>(frames.size()), static_cast<double>(ar.acquired));
				Logger::WriteMessage(c.name);
			}
		}

		TEST_METHOD(TemporalDetailGpuArenaShortageIsVisibleAndRecovers)
		{
			// SA6: no scratch texture (an empty plan, or the tier shed) = the frame fails visibly (Apply false, ScratchShort, the
			// failure counted, scratch_arena engaged); the next frame under a proper plan restarts by itself and equals a private
			// module's output on the same frame.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = TdGrainFrames();
			pl_tex bwdTex = UploadPair(gpu, MotionPair::Uniform(kW, kH, 16.0, -0.3, 0.0));
			const auto requests = Gpu::Module::ArenaRequests(gpu, kW, kH, 1080.0);
			Assert::IsFalse(requests.empty(), L"the float formats map to arena formats");
			for (int tierCase = 0; tierCase < 2; ++tierCase)
			{
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				// 0: an undersized plan (1 of 3 needed textures at T2): exhausted; 1: a plan whose tier (T0) is below the module's (T1): shed.
				if (tierCase == 0)
				{
					auto shortReq = requests;
					shortReq[0].count = 1;
					arena.Reserve(VpSkeleton::PlanArena(shortReq, VpSkeleton::Tier::T2));
				}
				else
					arena.Reserve(VpSkeleton::PlanArena(requests, VpSkeleton::Tier::T0));
				Gpu::Module module;
				MemoRig rig(dev);
				pl_tex tex0 = UploadLuma(gpu, frames[0]), tex1 = UploadLuma(gpu, frames[1]);
				Gpu::Params gp;
				gp.settings = Steady();
				gp.seq = 0;
				gp.planeLines = 1080.0;
				gp.backward.tex = bwdTex;
				gp.arena = &arena;
				gp.memo = &rig.memo;
				pl_frame out{};
				arena.BeginFrame();
				const unsigned long long before = module.Failures();
				rig.Begin(0, tex0);
				const bool shortOk = module.Apply(gpu, dev.dispatch, LumaFrame(tex0), gp, out);
				rig.End(0);
				Assert::IsFalse(shortOk, L"no scratch texture: Apply fails");
				Assert::IsTrue(module.ScratchShort(), L"ScratchShort");
				Assert::IsTrue(module.LastError().find("scratch memory short") != std::string::npos, Wide(module.LastError()).c_str());
				Assert::IsTrue(module.Failures() == before + 1, L"one failure counted");
				const FallbackCatalog::Decision d = arena.FrameDecision();
				Assert::IsTrue(d.engaged && d.id == "scratch_arena", L"scratch_arena engaged");
				Assert::IsTrue(tierCase == 0 ? arena.Exhausted() > 0 : arena.Shed() > 0,
					tierCase == 0 ? L"undersized plan counted as Exhausted" : L"T0 plan counted as Shed");
				// Recovery: a proper plan, a new frame.
				arena.BeginFrame();
				Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(requests, VpSkeleton::Tier::T2)), L"arena reserve");
				Assert::IsFalse(arena.FrameDecision().engaged, L"released after BeginFrame");
				gp.seq = 1;
				pl_frame out2{};
				rig.Begin(1, tex1);
				Assert::IsTrue(module.Apply(gpu, dev.dispatch, LumaFrame(tex1), gp, out2), Wide(module.LastError()).c_str());
				Assert::IsFalse(module.ScratchShort(), L"recovered");
				Assert::IsFalse(arena.FrameDecision().engaged, L"no shortage after recovery");
				const Plane got = DownloadLuma(gpu, out2.planes[0].texture);
				Gpu::Module fresh;
				Gpu::Params pf = gp;
				pf.arena = nullptr;
				pl_frame outF{};
				const bool okFresh = fresh.Apply(gpu, dev.dispatch, LumaFrame(tex1), pf, outF);
				rig.End(1);
				Assert::IsTrue(okFresh, Wide(fresh.LastError()).c_str());
				Assert::IsTrue(got.v == DownloadLuma(gpu, outF.planes[0].texture).v, L"the recovered frame equals a private-texture module's");
				Logger::WriteMessage(tierCase == 0 ? "undersized plan: visible, recovered" : "tier shed: visible, recovered");
				module.Release(gpu);
				fresh.Release(gpu);
				arena.Release();
				pl_tex_destroy(gpu, &tex0);
				pl_tex_destroy(gpu, &tex1);
			}
			pl_tex_destroy(gpu, &bwdTex);
		}

		TEST_METHOD(TemporalDetailGpuAcquiresTheVariantOncePerFrame)
		{
			// WAIVER-B2-TD-OWNSPLIT (a), replacing the own-split hold/release assertions: with no shared product the split is
			// the owner's memo variant, Acquired ONCE per frame (filled at read by the first reader) and Reused by the second
			// reader of the same frame and variant; the two readers' outputs are bit-identical; frames with a fitting shared
			// product Acquire nothing. Both the arena and the private mode.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = Stream(12, "IBBBPBBB", 0.3, 0.0, 29u, 1.2);
			pl_tex bwdTex = UploadPair(gpu, MotionPair::Uniform(kW, kH, 16.0, -0.3, 0.0));
			for (int useArena = 0; useArena < 2; ++useArena)
			{
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				if (useArena) Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(Gpu::Module::ArenaRequests(gpu, kW, kH, 1080.0), VpSkeleton::Tier::T2)), L"arena reserve");
				MemoRig rig(dev);
				Gpu::Module first, second;
				GrainSplit::Gpu::Splitter producer;
				uint64_t variantFrames = 0;
				for (size_t t = 0; t < frames.size(); ++t)
				{
					const bool shared = t >= 3 && t < 10;
					pl_tex tex = UploadLuma(gpu, frames[t]);
					RegionEvidence::GrainProduct product;
					if (shared)
					{
						Assert::IsTrue(producer.Compute(gpu, dev.dispatch, tex, 0, 64.0 / 65535.0, 1.0, 16), Wide(producer.LastError()).c_str());
						product.gpu[0] = &producer.Out();
					}
					Gpu::Params gp;
					gp.settings = Steady();
					gp.seq = t;
					gp.planeLines = 1080.0;
					gp.contentHz = 4.0;
					gp.backward.tex = bwdTex;
					gp.memo = &rig.memo;
					if (shared) gp.grain = RegionEvidence::GrainView(&product);
					if (useArena) { gp.arena = &arena; arena.BeginFrame(); }
					pl_frame a{}, b{};
					rig.Begin(t, tex);
					const bool okA = first.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, a);
					const VpSkeleton::AcquireStatus statusA = first.LastAcquireStatus();
					const VpSkeleton::AcquireCause causeA = first.LastAcquireCause();
					const Plane pa = okA ? DownloadLuma(gpu, a.planes[0].texture) : Plane(kW, kH);
					const bool okB = second.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, b);
					const VpSkeleton::AcquireStatus statusB = second.LastAcquireStatus();
					rig.End(t);
					Assert::IsTrue(okA, Wide(first.LastError()).c_str());
					Assert::IsTrue(okB, Wide(second.LastError()).c_str());
					Assert::IsTrue(causeA == VpSkeleton::AcquireCause::None, L"no refusal");
					if (!shared)
					{
						++variantFrames;
						Assert::IsTrue(statusA == VpSkeleton::AcquireStatus::FilledAtRead, (L"the first reader fills the variant at frame " + std::to_wstring(t)).c_str());
						Assert::IsTrue(statusB == VpSkeleton::AcquireStatus::Reused, (L"the second reader reuses it at frame " + std::to_wstring(t)).c_str());
						Assert::IsTrue(first.Status().find("grain split: memo variant, filled at read") != std::string::npos, L"the status names the variant");
						Assert::IsTrue(second.Status().find("grain split: memo variant, reused") != std::string::npos, L"the status names the reuse");
					}
					else
						Assert::IsTrue(first.Status().find("memo variant") == std::string::npos && second.Status().find("memo variant") == std::string::npos,
							L"a fitting shared product: no variant");
					Assert::IsTrue(pa.v == DownloadLuma(gpu, b.planes[0].texture).v, L"both readers produce the same planes");
					pl_tex_destroy(gpu, &tex);
				}
				const VpSkeleton::AcquireCounters c = rig.memo.CountersOf(VpSkeleton::kP10GrainSplit);
				LogF("variant frames %.0f: filled at read %.0f, reused %.0f, owner computes %.0f", static_cast<double>(variantFrames),
					static_cast<double>(c.filledAtRead), static_cast<double>(c.reusedMemo), static_cast<double>(rig.owner.Computes()));
				Assert::AreEqual(uint64_t(5), variantFrames, L"frames 0-2 and 10-11 have no shared product");
				Assert::AreEqual(variantFrames, c.filledAtRead, L"Acquired once per frame");
				Assert::AreEqual(variantFrames, c.reusedMemo, L"Reused by the second reader");
				Assert::AreEqual(variantFrames, rig.owner.Computes(), L"the owner computed each frame's variant once");
				first.Release(gpu);
				second.Release(gpu);
				producer.Release(gpu);
				arena.Release();
			}
			pl_tex_destroy(gpu, &bwdTex);
		}

		TEST_METHOD(TemporalDetailGpuRefusesVisiblyWithoutAVariant)
		{
			// WAIVER-B2-TD-OWNSPLIT (c): no shared product and (1) no skeleton memo: Apply fails, "no skeleton memo", nothing
			// computed; (2) an active-picture crop: the owner serves no crop variant, so the module refuses VariantUndeclared
			// (Apply fails, the cause named) and the memo is never asked; (3) the same crop with the cropped plane's split as
			// the shared product runs.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = Stream(2, "IB", 0.3, 0.0, 29u, 1.2);
			pl_tex bwdTex = UploadPair(gpu, MotionPair::Uniform(kW, kH, 16.0, -0.3, 0.0));
			pl_tex tex = UploadLuma(gpu, frames[0]);
			{
				Gpu::Module m;
				Gpu::Params gp;
				gp.settings = Steady();
				gp.backward.tex = bwdTex;
				pl_frame o{};
				const unsigned long long before = m.Failures();
				Assert::IsFalse(m.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, o), L"no memo: Apply fails");
				Assert::IsTrue(m.LastError().find("no skeleton memo") != std::string::npos, Wide(m.LastError()).c_str());
				Assert::IsTrue(m.Failures() == before + 1, L"the failure is counted");
				m.Release(gpu);
			}
			{
				MemoRig rig(dev);
				Gpu::Module m;
				Gpu::Params gp;
				gp.settings = Steady();
				gp.backward.tex = bwdTex;
				gp.memo = &rig.memo;
				gp.active = ActiveRect{ 0, 8, kW, kH - 8 };
				pl_frame o{};
				rig.Begin(0, tex);
				const bool ok = m.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, o);
				rig.End(0);
				Assert::IsFalse(ok, L"crop, no shared product: Apply fails");
				Assert::IsTrue(m.LastAcquireCause() == VpSkeleton::AcquireCause::VariantUndeclared, L"the refusal is VariantUndeclared");
				Assert::IsTrue(m.LastError().find("variant not served by the owner") != std::string::npos, Wide(m.LastError()).c_str());
				const VpSkeleton::AcquireCounters c = rig.memo.CountersOf(VpSkeleton::kP10GrainSplit);
				Assert::AreEqual(uint64_t(0), c.filledAtRead + c.reusedMemo + c.ownerFillCalls, L"the memo is never asked for a crop");
				Assert::AreEqual(uint64_t(0), rig.owner.Computes(), L"nothing computed");
				m.Release(gpu);
			}
			{
				Gpu::Params p;
				p.settings = Steady();
				const MatchResult r = RunMatch(dev, frames, 0.3, 0.0, p, Model::Tuning(), [&](int, Gpu::Params& gp, Model::FrameIn& in) {
					gp.active = ActiveRect{ 0, 8, kW, kH - 8 }; in.active = gp.active;
				});
				Assert::IsTrue(r.worst <= 0.125, L"a crop with its shared product runs and follows the model");
			}
			pl_tex_destroy(gpu, &tex);
			pl_tex_destroy(gpu, &bwdTex);
		}

		TEST_METHOD(TemporalDetailGpuTextureBytesCountsTheArenaTemporaries)
		{
			// SA6: with the temporaries in the arena the module's own bytes drop by 24 per pixel (two accumulators 2 x 8, delta 8;
			// the module holds no grain split of its own any more); the default form is the private-texture form, unchanged.
			const uint64_t priv = Gpu::Module::TextureBytes(3840, 2160, 2160), inArena = Gpu::Module::TextureBytes(3840, 2160, 2160, kSlots, true);
			Assert::IsTrue(priv == Gpu::Module::TextureBytes(3840, 2160, 2160, kSlots, false), L"the default is the private form");
			Assert::IsTrue(priv - inArena == uint64_t(24) * 3840 * 2160, L"24 bytes per pixel move to the arena");
		}

		TEST_METHOD(TemporalDetailGpuOutputIdentityPins)
		{
			// WAIVER-B2-TD-OWNSPLIT (b): the module's output planes on the existing fixtures, hashed (FNV-1a over the 1/64-code
			// samples). The pins were recorded on the binary BEFORE the own split was deleted (its own GrainSplit::Params equal
			// GrainSplit::DefaultParams()); after it the split comes from the owner's variant (Acquire), and the pixels must not move.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			struct Case { const char* name; int clean; bool boost; double lines; bool crop; uint64_t pin; };
			const Case cases[] = {
				{ "cleaner", 1, false, 1080.0, false, 0x741024ba588a1431ull },
				{ "single-frame template", 0, false, 1080.0, false, 0xdbabc20e752ba037ull },
				{ "boost", 1, true, 1080.0, false, 0xa3fe074ff293bcd5ull },
				{ "4320 lines (scale 2)", 1, false, 4320.0, false, 0x478b1e7426b6c11full },
				{ "active-picture crop", 1, false, 1080.0, true, 0x587d99624b647447ull },
			};
			const ActiveRect cropRect{ 0, 8, kW, kH - 8 };
			const std::vector<Plane> frames = Stream(16, "IBBBPBBB", 0.37, 0.21, 11u, 1.2);
			bool allPinned = true, allMatch = true;
			for (const Case& c : cases)
			{
				Gpu::Params p;
				p.settings = Steady();
				p.settings.boost = c.boost;
				p.templateClean = c.clean != 0;
				Model::Tuning tune;
				tune.templateClean = c.clean != 0;
				const MatchResult r = RunMatch(dev, frames, 0.37, 0.21, p, tune, [&](int, Gpu::Params& gp, Model::FrameIn& in) {
					gp.planeLines = c.lines; in.planeLines = c.lines;
					if (c.crop) { gp.active = cropRect; in.active = cropRect; }
				});
				uint64_t hsh = 0xcbf29ce484222325ull;
				for (const Plane& o : r.gpu)
					for (double v : o.v)
					{
						const uint32_t q = static_cast<uint32_t>(std::lround(v * 64.0));
						for (int b = 0; b < 4; ++b) { hsh ^= (q >> (8 * b)) & 0xffu; hsh *= 0x100000001b3ull; }
					}
				char line[200];
				std::snprintf(line, sizeof line, "identity pin %s: 0x%016llx (pinned 0x%016llx)", c.name, static_cast<unsigned long long>(hsh),
					static_cast<unsigned long long>(c.pin));
				Logger::WriteMessage(line);
				allPinned = allPinned && c.pin != 0;
				allMatch = allMatch && hsh == c.pin;
			}
			Assert::IsTrue(allPinned, L"every identity pin is recorded");
			Assert::IsTrue(allMatch, L"the output planes equal the pinned (pre-change) planes bit for bit");
		}

		TEST_METHOD(GrainVariantNeedIsNonZeroBeforeTheFirstFill)
		{
			// WAIVER-C5-GF-T2 (GF-P1 MUST-5, GV AMEND): the grain split owner registers with the largest observed luma, so the memo's
			// planned need is non-zero BEFORE any fill (it was 0: the ledger grant was never made and every Acquire failed
			// NoLedgerGrant). With the ledger grant = that need, the first frame's variant is served: filled at read, no refusal.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = Stream(2, "IB", 0.3, 0.0, 29u, 1.2);
			pl_tex bwdTex = UploadPair(gpu, MotionPair::Uniform(kW, kH, 16.0, -0.3, 0.0));
			pl_tex tex = UploadLuma(gpu, frames[0]);
			{
				MemoRig rig(dev, kW, kH, true);
				Assert::IsTrue(rig.memo.GpuNeedBytes() > 0, L"the planned need is non-zero before the first fill");
				Gpu::Module m;
				Gpu::Params gp;
				gp.settings = Steady();
				gp.seq = 0;
				gp.planeLines = 1080.0;
				gp.contentHz = 4.0;
				gp.backward.tex = bwdTex;
				gp.memo = &rig.memo;
				pl_frame out{};
				rig.Begin(0, tex);
				const bool ok = m.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, out);
				const VpSkeleton::AcquireCause cause = m.LastAcquireCause();
				const VpSkeleton::AcquireStatus status = m.LastAcquireStatus();
				rig.End(0);
				Assert::IsTrue(ok, Wide(m.LastError()).c_str());
				Assert::IsTrue(cause != VpSkeleton::AcquireCause::NoLedgerGrant, L"the first frame is not refused for want of a ledger grant");
				Assert::IsTrue(cause == VpSkeleton::AcquireCause::None, L"no refusal");
				Assert::IsTrue(status == VpSkeleton::AcquireStatus::FilledAtRead, L"the first frame's variant is filled at read");
				const VpSkeleton::AcquireCounters c = rig.memo.CountersOf(VpSkeleton::kP10GrainSplit);
				Assert::AreEqual(uint64_t(0), c.unavailable[size_t(VpSkeleton::AcquireCause::NoLedgerGrant)], L"0 NoLedgerGrant");
				m.Release(gpu);
			}
			pl_tex_destroy(gpu, &tex);
			pl_tex_destroy(gpu, &bwdTex);
		}

		TEST_METHOD(TemporalDetailGpuSceneRestartInputFollowsTheModel)
		{
			// WAIVER-C5-SOFTRESET-TD (cut_soft_reset in temporal detail; review D:\codex\vp-handover\batchB\review-softreset-r1.md L1):
			// the production consumers' scene answer (SceneRestartInput, as SharedSceneSource copies it from SceneRestartOf) on the GPU
			// follows the model: a soft reset (keep 0.5) and a fade (keep 0.8) scale the stored slots through the one fade_keep (no
			// reset), a cut answer resets; outputs within the resets test's match tolerance. Both statuses name the soft reset and carry
			// the consumers' note after the status.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = Stream(22, "IBBBPBBB", 0.5, 0.0, 17u, 1.2);
			auto answer = [](int t) {
				SceneRestartInput s;
				s.given = true;
				if (t == 9) { s.softPartial = true; s.historyKeep = 0.5; s.strength = 1.0; }
				if (t == 13) { s.fadePartial = true; s.historyKeep = 0.8; }
				if (t == 17) { s.restart = true; s.historyKeep = 0.0; }
				return s;
			};
			Gpu::Params p;
			p.settings = Steady();
			Model::Tuning tune;
			const MatchResult r = RunMatch(dev, frames, 0.5, 0.0, p, tune, [&](int t, Gpu::Params& gp, Model::FrameIn& in) {
				gp.sceneRestart = answer(t);
				in.sceneRestart = answer(t);
			});
			LogF("scene answers: worst %.4f codes, worst frame mean %.5f", r.worst, r.worstMean);
			Assert::IsTrue(r.worst <= 0.125 && r.worstMean <= 0.005 && r.maxOverShare <= 0.0, L"the GPU follows the model through the scene answers");

			// The statuses, model and GPU: the soft reset named; the consumers' note right after the status.
			pl_gpu gpu = dev.Gpu();
			Gpu::Module module;
			Model model(Steady());
			const MotionPair bwd = MotionPair::Uniform(kW, kH, 16.0, 0.0, 0.0);
			FieldMotionSource motion;
			for (int t = 0; t < 4; ++t) motion.SetBackward(static_cast<uint64_t>(t), bwd);
			pl_tex bwdTex = UploadPair(gpu, bwd);
			// WAIVER-C5-GF-T2 (MUST-13): the module's only grain split source is the skeleton memo (no shared product here), so this
			// half runs on the memo rig too: Begin offers the frame's luma, End seals it; the sequence runs 0..3 as RunMatch uses it.
			MemoRig rig(dev);
			for (int t = 0; t < 4; ++t)
			{
				SceneRestartInput s;
				s.given = true;
				if (t == 2) { s.softPartial = true; s.historyKeep = 0.5; s.strength = 1.0; }
				if (t == 3) s.statusNote = ", soft reset: off";
				pl_tex tex = UploadLuma(gpu, frames[static_cast<size_t>(t)]);
				Gpu::Params gp;
				gp.settings = Steady();
				gp.seq = static_cast<uint64_t>(t);
				gp.backward.tex = bwdTex;
				gp.sceneRestart = s;
				gp.memo = &rig.memo;
				pl_frame out{};
				rig.Begin(gp.seq, tex);
				const bool applied = module.Apply(gpu, dev.dispatch, LumaFrame(tex), gp, out);
				rig.End(gp.seq);
				Assert::IsTrue(applied, Wide(module.LastError()).c_str());
				Model::FrameIn in;
				in.luma = frames[static_cast<size_t>(t)];
				in.seq = static_cast<uint64_t>(t);
				in.motion = &motion;
				in.sceneRestart = s;
				model.Process(in);
				const char* expect = t == 2 ? "soft reset: partial" : t == 3 ? "ok, soft reset: off" : nullptr;
				if (expect)
				{
					Assert::IsTrue(module.Status().find(expect) == 0, Wide(module.Status()).c_str());
					Assert::IsTrue(model.Diag().status.find(expect) == 0, Wide(model.Diag().status).c_str());
				}
				pl_tex_destroy(gpu, &tex);
			}
			module.Release(gpu);
			pl_tex_destroy(gpu, &bwdTex);
		}

		// --- TD-NONFINITE on the GPU (gate cb-u1 r1 MUST 2 / MUST 4): the shaders close the gates the way the CPU model does ---

		TEST_METHOD(TemporalDetailGpuNonFiniteGrainLevelClosesTheBlock)
		{
			// A block whose grain level N (the shared product's per-block texture) is NaN or +-inf: N feeds the significance
			// edge, the base-match allowance and the stronger gate, so a non-finite N used to give NaN (or open) gates and a
			// non-zero `hard`. The block must instead transfer nothing (output = input there), and no pixel may go wrong
			// anywhere (a NaN weight turns into a black pixel through the unorm conversion).
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = Stream(12, "IBBBPBBB", 0.3, 0.0, 29u, 1.2);
			const std::vector<Plane> control = RunTdPlantedN(dev, frames, false, 0.0f, 0, 0);
			const int bw = (kW + 15) / 16, bh = (kH + 15) / 16;
			int bestX = 1, bestY = 1;
			double best = -1.0;
			for (int by = 1; by < bh - 1; ++by)
				for (int bx = 1; bx < bw - 1; ++bx)
				{
					const double c = WorstInBlock(control, frames, bx, by);
					if (c > best) { best = c; bestX = bx; bestY = by; }
				}
			LogF("control: the block (%.0f, %.0f) changes by up to %.3f codes", bestX, bestY, best);
			Assert::IsTrue(best > 0.25, L"control: the transfer changes the picture inside the block that is planted below (the test is not vacuous)");
			// Gate cb-u1 r2 MUST 3: the closure itself. With a finite N at least one slot's `hard` (ev_a.x) at the block is > 0 on
			// some frame; with a planted non-finite N every slot's `hard` there is exactly 0 on every frame. (On WARP the output
			// alone does not tell: a NaN N already zeroed `stronger`, so no soft weight built, while `hard` stayed open.)
			std::vector<float> controlHard;
			int slots = 0;
			RunTdPlantedN(dev, frames, false, 0.0f, bestX, bestY, &controlHard, &slots);
			float controlHardMax = 0.0f;
			for (float h : controlHard) controlHardMax = (std::max)(controlHardMax, h);
			LogF("control: %.0f slots, largest hard at the block %.4f", static_cast<double>(slots), static_cast<double>(controlHardMax));
			Assert::IsTrue(controlHardMax > 0.0f, L"control: with a finite N some slot's hard (ev_a.x) at the block is > 0");
			const double controlWorstAll = WorstInBlock(control, frames, 0, 0, true);
			// WAIVER-U1-BLEED control: the exact claim below runs with the guard off; nothing planted, the block still changes there
			// (so "output = input" over the closed support is not vacuous).
			const std::vector<Plane> controlNoGuard = RunTdPlantedN(dev, frames, false, 0.0f, bestX, bestY, nullptr, nullptr, 0, nullptr, nullptr, 0, false);
			const double controlNoGuardBlock = WorstInBlock(controlNoGuard, frames, bestX, bestY, false, 0);
			LogF("control, guard off: the block changes by up to %.4f codes", controlNoGuardBlock);
			Assert::IsTrue(controlNoGuardBlock > 0.25, L"control, guard off: the transfer changes the block (the support claim is not vacuous)");
			const float values[3] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };
			const char* const names[3] = { "NaN", "+inf", "-inf" };
			for (int k = 0; k < 3; ++k)
			{
				const std::vector<Plane> planted = RunTdPlantedN(dev, frames, true, values[k], bestX, bestY);
				const double inBlock = WorstInBlock(planted, frames, bestX, bestY);
				const double all = WorstInBlock(planted, frames, 0, 0, true);
				LogF((std::string("N = ") + names[k] + ": block change %.4f codes, worst anywhere %.4f (control %.4f)").c_str(), inBlock, all, controlWorstAll);
				std::vector<float> hard;
				RunTdPlantedN(dev, frames, true, values[k], bestX, bestY, &hard, nullptr);
				Assert::IsTrue(hard.size() == controlHard.size(), L"the planted run read the same evidence texels");
				bool closed = true;
				for (float h : hard) closed = closed && h == 0.0f;
				Assert::IsTrue(closed, Wide(std::string("N = ") + names[k] + ": hard (ev_a.x) at the planted block is 0 in every slot on every frame").c_str());
				// WAIVER-U1-BLEED: a pixel of the planted block also takes its neighbours' (open) terms through the bilinear block field,
				// so the one-block plant leaves the block's core changed (the block change logged above). The exact claim plants the
				// block's whole support (3 x 3 blocks), guard off: no term reaches the block, every pixel is the input bit-exact.
				const std::vector<Plane> support = RunTdPlantedN(dev, frames, true, values[k], bestX, bestY, nullptr, nullptr, 0, nullptr, nullptr, 1, false);
				const double supportBlock = WorstInBlock(support, frames, bestX, bestY, false, 0);
				LogF((std::string("N = ") + names[k] + " over the 3 x 3 support, guard off: block change %.4f codes").c_str(), supportBlock);
				Assert::IsTrue(supportBlock == 0.0, Wide(std::string("N = ") + names[k] + ": over the block's whole support the block transfers nothing (output = input, bit-exact, all 16 x 16 px)").c_str());
				// WAIVER-U1-RAIL: "no pixel goes wrong" is the NaN / inf weight failure (the unorm conversion makes NaN black and an
				// infinity a rail), tested exactly: no output pixel sits on a rail (0 or 65535 / 64 codes) that its input is not on.
				// The former bound "worst anywhere <= control + 1 code" is not a property of the law: closing one block lowers the
				// open neighbours' amp and wpix at the pixels they share, so the per-pixel grain budget beta = min(1, budget / amp)
				// and the pixel cap renormalise them (kTransferBody, as the model's pass 3) and a neighbour may change more than in
				// the control (8.70 -> 9.98 codes, WARP and the 3050 Ti alike). The worst excess and its block are logged.
				int railHits = 0, exX = -1, exY = -1;
				double excess = 0.0;
				const double railHi = 65535.0 / 64.0;
				for (size_t t = 0; t < planted.size(); ++t)
					for (int y = 0; y < kH; ++y)
						for (int x = 0; x < kW; ++x)
						{
							const double in = frames[t].at(x, y), o = planted[t].at(x, y);
							if ((o == 0.0 && in != 0.0) || (o == railHi && in != railHi)) ++railHits;
							const double d = std::fabs(o - in) - controlWorstAll;
							if (d > excess) { excess = d; exX = x / 16; exY = y / 16; }
						}
				LogF((std::string("N = ") + names[k] + ": %.0f pixels on a new rail; worst excess over the control %.4f codes at block (%.0f, %.0f)").c_str(),
					static_cast<double>(railHits), excess, static_cast<double>(exX), static_cast<double>(exY));
				Assert::IsTrue(railHits == 0, Wide(std::string("N = ") + names[k] + ": no pixel anywhere goes wrong (no output on a rail its input is not on)").c_str());
			}
		}

		TEST_METHOD(TemporalDetailGpuNonFiniteFocusInputClosesTheBlock)
		{
			// Gate cb-u1 r3 MUST 2 (the r2 MUST 2 shader closure): a NaN e3 in the shared block texture makes the block's F
			// (meas.z = e2 / (e2 + e3 + floor)) NaN, and the F the slot stores from it. td_gs maps the NaN focus ratio to a finite 0,
			// so before f41a2c1c5 only `focus` (and with it `hard`) closed while stronger (ev_a.z), the high band (ev_b.x) and
			// unc x base x corr (ev_b.z) stayed open; the model closes every gate on a non-finite F. Every slot on every frame must
			// now read 0 there.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			const std::vector<Plane> frames = Stream(12, "IBBBPBBB", 0.3, 0.0, 29u, 1.2);
			const std::vector<Plane> control = RunTdPlantedN(dev, frames, false, 0.0f, 0, 0);
			const int bw = (kW + 15) / 16, bh = (kH + 15) / 16;
			int bestX = 1, bestY = 1;
			double best = -1.0;
			for (int by = 1; by < bh - 1; ++by)
				for (int bx = 1; bx < bw - 1; ++bx)
				{
					const double c = WorstInBlock(control, frames, bx, by);
					if (c > best) { best = c; bestX = bx; bestY = by; }
				}
			LogF("control: the block (%.0f, %.0f) changes by up to %.3f codes", bestX, bestY, best);
			Assert::IsTrue(best > 0.25, L"control: the transfer runs in the block planted below");
			std::vector<float> ca, cb;
			RunTdPlantedN(dev, frames, false, 0.0f, bestX, bestY, nullptr, nullptr, 1, &ca, &cb);
			Assert::IsTrue(!ca.empty() && ca.size() == cb.size() && ca.size() % 4 == 0, L"control: the evidence texels were read");
			float strongerMax = 0.0f, ebzMax = 0.0f;
			for (size_t i = 0; i < ca.size(); i += 4)
			{
				strongerMax = (std::max)(strongerMax, ca[i + 2]);
				ebzMax = (std::max)(ebzMax, cb[i + 2]);
			}
			LogF("control: largest stronger %.4f, unc x base x corr %.4f", static_cast<double>(strongerMax), static_cast<double>(ebzMax));
			Assert::IsTrue(strongerMax > 0.0f, L"control: with a finite e3 some slot's stronger (ev_a.z) at the block is > 0");
			Assert::IsTrue(ebzMax > 0.0f, L"control: with a finite e3 some slot's unc x base x corr (ev_b.z) at the block is > 0");
			std::vector<float> pa, pb;
			const std::vector<Plane> planted = RunTdPlantedN(dev, frames, true, std::numeric_limits<float>::quiet_NaN(), bestX, bestY,
				nullptr, nullptr, 1, &pa, &pb);
			Assert::IsTrue(pa.size() == ca.size() && pb.size() == cb.size(), L"the planted run read the same evidence texels");
			bool stronger = true, hb = true, ebz = true;
			for (size_t i = 0; i < pa.size(); i += 4)
			{
				stronger = stronger && pa[i + 2] == 0.0f;
				hb = hb && pb[i] == 0.0f;
				ebz = ebz && pb[i + 2] == 0.0f;
			}
			Assert::IsTrue(stronger, L"e3 = NaN: stronger (ev_a.z) is 0 in every slot on every frame");
			Assert::IsTrue(hb, L"e3 = NaN: the high-band gate (ev_b.x) is 0 in every slot on every frame");
			Assert::IsTrue(ebz, L"e3 = NaN: unc x base x corr (ev_b.z) is 0 in every slot on every frame");
			double worst = 0.0;
			bool finite = true;
			for (const Plane& p : planted)
				for (double v : p.v) finite = finite && std::isfinite(v);
			worst = WorstInBlock(planted, frames, bestX, bestY);
			LogF("e3 = NaN: block change %.4f codes", worst);
			Assert::IsTrue(finite, L"e3 = NaN: the output is finite");
			// WAIVER-U1-BLEED: as in the GrainLevel test, the one-block plant leaves the core changed by the neighbours' open terms
			// (the bilinear block field). The exact claim plants e3 over the block's whole support (3 x 3 blocks), guard off; the
			// control is the same guard-off run with nothing planted.
			const std::vector<Plane> controlNoGuard = RunTdPlantedN(dev, frames, false, 0.0f, bestX, bestY, nullptr, nullptr, 1, nullptr, nullptr, 0, false);
			const double controlNoGuardBlock = WorstInBlock(controlNoGuard, frames, bestX, bestY, false, 0);
			const std::vector<Plane> support = RunTdPlantedN(dev, frames, true, std::numeric_limits<float>::quiet_NaN(), bestX, bestY,
				nullptr, nullptr, 1, nullptr, nullptr, 1, false);
			const double supportBlock = WorstInBlock(support, frames, bestX, bestY, false, 0);
			LogF("guard off: control block change %.4f codes, e3 = NaN over the 3 x 3 support %.4f", controlNoGuardBlock, supportBlock);
			Assert::IsTrue(controlNoGuardBlock > 0.25, L"control, guard off: the transfer changes the block (the support claim is not vacuous)");
			Assert::IsTrue(supportBlock == 0.0, L"e3 = NaN over the block's whole support: the block transfers nothing (output = input, bit-exact, all 16 x 16 px)");
		}

		TEST_METHOD(TemporalDetailGpuNonFiniteBlockGateTexelIsNeutral)
		{
			// The per-block gate factors are sanitised on the GPU (td_bgate: non-finite -> 1) as on the CPU (SanitizeBlockGate),
			// so a NaN or infinite texel in the bgates texture behaves as the neutral factor: the model gives the neutral
			// result exactly, the GPU follows it, and the GPU result equals the neutral GPU result.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<Plane> frames = Stream(12, "IBBBPBBB", 0.37, 0.0, 19u, 1.2);
			const int bw = (kW + 15) / 16, bh = (kH + 15) / 16;
			Gpu::Params p;
			p.settings = Steady();
			const MatchResult neutral = RunMatch(dev, frames, 0.37, 0.0, p, Model::Tuning());
			Assert::IsTrue(neutral.changedShare > 0.001, L"control: the transfer runs on this scene");
			const float values[3] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };
			for (int k = 0; k < 3; ++k)
			{
				std::vector<float> gates(static_cast<size_t>(bw) * bh * 4, 1.0f);
				for (int c = 0; c < 4; ++c)
				{
					gates[(static_cast<size_t>(2) * bw + 2) * 4 + static_cast<size_t>(c)] = values[k];   // block (2, 2): all four factors
					gates[(static_cast<size_t>(3) * bw + 4) * 4 + static_cast<size_t>(c)] = values[(k + 1) % 3];   // block (4, 3): another kind
				}
				pl_tex_params tp{};
				tp.w = bw; tp.h = bh; tp.format = pl_find_named_fmt(gpu, "rgba32f"); tp.sampleable = true; tp.initial_data = gates.data();
				pl_tex gtex = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(gtex);
				const MatchResult r = RunMatch(dev, frames, 0.37, 0.0, p, Model::Tuning(), [&](int, Gpu::Params& gp, Model::FrameIn& in) {
					gp.blockGates = gtex;
					in.blockGates = gates;
				});
				double modelDiff = 0.0;
				bool finite = true;
				for (size_t t = 0; t < frames.size(); ++t)
					for (size_t i = 0; i < r.model[t].v.size(); ++i)
					{
						modelDiff = (std::max)(modelDiff, std::fabs(r.model[t].v[i] - neutral.model[t].v[i]));
						finite = finite && std::isfinite(r.model[t].v[i]) && std::isfinite(r.gpu[t].v[i]);
					}
				const double gpuDiff = WorstDiff(r.gpu, neutral.gpu);
				LogF("non-finite gate texels: model vs neutral %.6f codes, GPU vs neutral GPU %.4f, GPU vs model worst %.4f", modelDiff, gpuDiff, r.worst);
				Assert::IsTrue(finite, L"the model and the GPU outputs are finite");
				Assert::IsTrue(modelDiff == 0.0, L"the model treats a non-finite factor as neutral (identical result)");
				Assert::IsTrue(r.worst <= 0.125 && r.worstMean <= 0.005 && r.maxOverShare <= 0.0, L"the GPU follows the model with non-finite gate texels");
				Assert::IsTrue(gpuDiff <= 0.125, L"the GPU treats a non-finite factor as neutral");
				pl_tex_destroy(gpu, &gtex);
			}
		}
	};
}
