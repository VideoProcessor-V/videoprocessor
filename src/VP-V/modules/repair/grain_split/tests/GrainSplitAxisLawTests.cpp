// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Slice 21 (W21-3), P10 law equivalence. Two documented laws describe the grain split on a 12-bit source:
//   AXIS law   the lane tap is the Q6 axis for every depth (a 12-bit sample is code12 << 4, a 10-bit sample code10 << 6):
//              plane value = Q6 / 65535, codeUnit 64 / 65535, sourceBits 10 (codes are 10-bit-axis codes, 1/4 code steps).
//   NATIVE law the plane holds the 12-bit code: plane value = (code12 << 4) / 65535, codeUnit 16 / 65535, sourceBits 12
//              (every code-domain limit scaled by DepthScale(12) = 4, energies by 16).
// The two read the same texels. They are the same law if every product of the native run is the axis run scaled by the
// power of two its codes are scaled by: base, d1, d2, d3, s1, s2 x 4; f unchanged; N, e1, e2, e3 x 16.
//   CpuAxisEqualsNative      GrainSplit::Compute on both planes, every plane and block value.
//   GpuAxisEqualsNative      the GPU splitter (WARP unless VP_TEST_GPU=hardware), Full and Compact, same relation.
// A failure here means the axis law and the native law differ: the lead decides which is named (plan W21-3). No tolerance
// is widened to fit a result (rule 28); every worst value is written to the test log.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainSplitAxisLawTestsNs
{
	using GrainSplit::Plane;
	using GrainSplit::Storage;

	constexpr double kPi = 3.14159265358979323846;
	constexpr double kAxisUnit = 64.0 / 65535.0;     // axis law: Q6 steps (1/64 axis code)
	constexpr double kNativeUnit = 16.0 / 65535.0;   // native law: 12-bit codes in a 16-bit plane (code12 << 4)
	constexpr double kScaleCode = 4.0;               // DepthScale(12): native codes = 4 x axis codes
	constexpr double kScaleEnergy = 16.0;

	void LogF(const char* fmt, double a = 0, double b = 0, double c = 0)
	{
		char line[320];
		std::snprintf(line, sizeof line, fmt, a, b, c);
		Logger::WriteMessage(line);
	}
	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// The 12-bit codes: flat gradient, fine texture, structure and grain (sigma 8 native codes), integers in 0..4095.
	std::vector<int> MakeCodes12(int w, int h, uint32_t seed)
	{
		std::vector<int> c(static_cast<size_t>(w) * h);
		std::mt19937 g(seed);
		std::normal_distribution<double> noise(0.0, 8.0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				double v;
				if (x < w / 3)
					v = 1000.0 + 1.6 * x + 1.2 * y;
				else if (x < 2 * w / 3)
					v = 1400.0 + 56.0 * std::sin(2.0 * kPi * x / 5.0) * std::cos(2.0 * kPi * y / 6.5) + 36.0 * std::sin(2.0 * kPi * (x + y) / 9.0);
				else
				{
					const double rx0 = 0.72 * w, rx1 = 0.92 * w, ry0 = 0.2 * h, ry1 = 0.8 * h;
					const double inX = 0.5 * (std::tanh((x - rx0) / 0.7) - std::tanh((x - rx1) / 0.7));
					const double inY = 0.5 * (std::tanh((y - ry0) / 0.7) - std::tanh((y - ry1) / 0.7));
					v = 1200.0 + 1000.0 * inX * inY + 320.0 * std::tanh((x * 0.6 + y * 0.8 - 0.70 * w) / 0.9);
				}
				v += noise(g);
				c[static_cast<size_t>(y) * w + x] = static_cast<int>(std::lround((std::min)(4095.0, (std::max)(0.0, v))));
			}
		return c;
	}

	Plane PlaneOf(const std::vector<int>& codes, int w, int h, double codeScale)
	{
		Plane p(w, h);
		for (size_t i = 0; i < codes.size(); ++i) p.v[i] = codes[i] * codeScale;
		return p;
	}

	// native == factor x axis, relative to max(1, |native|); worst keeps the largest miss.
	bool Near(double native, double axis, double factor, double tol, double& worst)
	{
		const double err = std::fabs(native - factor * axis) / (std::max)(1.0, std::fabs(native));
		worst = (std::max)(worst, err);
		return err <= tol;
	}

	struct GpuDevice
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_dispatch dispatch = nullptr;
		std::string name;
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
			const pl_d3d11_params params = ExtendedRangeGpuProbe::TestDeviceParams(&name);
			Logger::WriteMessage(("grain split axis-law GPU tests on: " + name).c_str());
			d3d11 = pl_d3d11_create(log, &params);
			if (!d3d11) return false;
			dispatch = pl_dispatch_create(log, d3d11->gpu);
			return dispatch != nullptr;
		}
		pl_gpu Gpu() const { return d3d11->gpu; }
	};

	// The 16-bit plane both laws read: texel = code12 << 4 (the Q6 axis value of the 12-bit sample).
	pl_tex UploadQ6(pl_gpu gpu, const std::vector<int>& codes, int w, int h)
	{
		std::vector<uint16_t> data(codes.size());
		for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint16_t>(codes[i] << 4);
		pl_tex_params p{};
		p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	std::vector<float> DownloadRGBA(pl_gpu gpu, pl_dispatch dispatch, pl_tex tex)
	{
		if (!tex) return {};
		pl_tex_params rp{};
		rp.w = tex->params.w; rp.h = tex->params.h; rp.format = VpvGpu::FloatFormat(gpu, 4); rp.host_readable = true; rp.storable = true;
		pl_tex readable = pl_tex_create(gpu, &rp);
		if (!readable) return {};
		const pl_shader_desc d[2] = { VpvGpu::Sampled("src", tex), VpvGpu::Storage("dst", readable) };
		VpvGpu::Pass(dispatch, "test copy", std::string(), R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < textureSize(src, 0).x && p.y < textureSize(src, 0).y) imageStore(dst, p, texelFetch(src, p, 0));
)GLSL", d, 2, nullptr, 0, (rp.w + 15) / 16, (rp.h + 15) / 16, 0);
		std::vector<float> v(static_cast<size_t>(rp.w) * rp.h * 4);
		pl_tex_transfer_params tr{}; tr.tex = readable; tr.ptr = v.data();
		const bool ok = pl_tex_download(gpu, &tr);
		pl_tex_destroy(gpu, &readable);
		if (!ok) v.clear();
		return v;
	}

	struct Product
	{
		bool ok = false;
		std::string error;
		bool compact = false;
		int w = 0, h = 0, bw = 0, bh = 0;
		std::vector<float> a, b, blocks;
	};

	Product Run(GpuDevice& dev, pl_tex tex, int B, double scale, Storage storage, double codeUnit, int sourceBits)
	{
		Product r;
		pl_gpu gpu = dev.Gpu();
		GrainSplit::Gpu::Splitter splitter;
		GrainSplit::Gpu::Options o;
		o.storage = storage;
		GrainSplit::Params params;
		params.sourceBits = sourceBits;
		r.ok = splitter.Compute(gpu, dev.dispatch, tex, 0, codeUnit, scale, B, params, o);
		r.error = splitter.LastError();
		if (r.ok)
		{
			const GrainSplit::Gpu::Frame& f = splitter.Out();
			r.compact = GrainSplit::Gpu::Compact(&f);
			r.w = f.w; r.h = f.h; r.bw = f.bw; r.bh = f.bh;
			r.a = DownloadRGBA(gpu, dev.dispatch, f.split);
			if (r.compact) r.b = DownloadRGBA(gpu, dev.dispatch, f.splitB);
			r.blocks = DownloadRGBA(gpu, dev.dispatch, f.blocks);
		}
		splitter.Release(gpu);
		return r;
	}

	// (s1, s2, f, base) in the run's own codes of texel i, whichever the layout.
	struct Texel { double s1, s2, f, base; };
	Texel Decode(const Product& p, size_t i, double codeUnit)
	{
		if (!p.compact) return { p.a[i * 4], p.a[i * 4 + 1], p.a[i * 4 + 2], p.a[i * 4 + 3] };
		return { p.a[i * 4], p.a[i * 4 + 1], p.b[i * 4], p.b[i * 4 + 1] / codeUnit };
	}
}

TEST_CLASS(AxisTapEqualsNativeTwelveLaw)
{
public:
	// The CPU law: the axis plane (code12 / 4, sourceBits 10) against the native plane (code12, sourceBits 12).
	TEST_METHOD(CpuAxisEqualsNative)
	{
		using namespace GrainSplitAxisLawTestsNs;
		const int sizes[2][2] = { { 257, 143 }, { 320, 180 } };
		const int edges[3] = { 8, 16, 32 };
		for (int si = 0; si < 2; ++si)
			for (int bi = 0; bi < 3; ++bi)
			{
				const int w = sizes[si][0], h = sizes[si][1], B = edges[bi];
				const double scale = B / 16.0;
				const std::vector<int> codes = MakeCodes12(w, h, 7u + static_cast<uint32_t>(si * 3 + bi));
				GrainSplit::Params axis;      // sourceBits 10
				GrainSplit::Params native;
				native.sourceBits = 12;
				const GrainSplit::Split a = GrainSplit::Compute(PlaneOf(codes, w, h, 1.0 / kScaleCode), scale, B, axis);
				const GrainSplit::Split n = GrainSplit::Compute(PlaneOf(codes, w, h, 1.0), scale, B, native);
				const double tol = 1e-9;
				double wb = 0, w1 = 0, w2 = 0, w3 = 0, wf = 0, wS1 = 0, wS2 = 0, wN = 0, wE = 0;
				bool ok = true;
				for (size_t i = 0; i < a.base.v.size(); ++i)
				{
					ok &= Near(n.base.v[i], a.base.v[i], kScaleCode, tol, wb);
					ok &= Near(n.d1.v[i], a.d1.v[i], kScaleCode, tol, w1);
					ok &= Near(n.d2.v[i], a.d2.v[i], kScaleCode, tol, w2);
					ok &= Near(n.d3.v[i], a.d3.v[i], kScaleCode, tol, w3);
					ok &= Near(n.s1.v[i], a.s1.v[i], kScaleCode, tol, wS1);
					ok &= Near(n.s2.v[i], a.s2.v[i], kScaleCode, tol, wS2);
					ok &= Near(n.f.v[i], a.f.v[i], 1.0, tol, wf);
				}
				Assert::AreEqual(a.bw, n.bw, L"block grid width");
				Assert::AreEqual(a.bh, n.bh, L"block grid height");
				for (size_t i = 0; i < a.N.size(); ++i)
				{
					ok &= Near(n.N[i], a.N[i], kScaleEnergy, tol, wN);
					ok &= Near(n.e1[i], a.e1[i], kScaleEnergy, tol, wE);
					ok &= Near(n.e2[i], a.e2[i], kScaleEnergy, tol, wE);
					ok &= Near(n.e3[i], a.e3[i], kScaleEnergy, tol, wE);
				}
				LogF("cpu axis==native: base %.3g d1 %.3g d2 %.3g", wb, w1, w2);
				LogF("cpu axis==native: d3 %.3g s1 %.3g s2 %.3g", w3, wS1, wS2);
				LogF("cpu axis==native: f %.3g N %.3g e %.3g", wf, wN, wE);
				Assert::IsTrue(ok, L"the axis law and the native law differ on the CPU: lead decision needed (plan W21-3)");
			}
	}

	// The GPU kernel: the same texels, codeUnit 64 / 65535 + sourceBits 10 against codeUnit 16 / 65535 + sourceBits 12.
	BEGIN_TEST_METHOD_ATTRIBUTE(GpuAxisEqualsNative)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(GpuAxisEqualsNative)
	{
		using namespace GrainSplitAxisLawTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const int sizes[2][2] = { { 257, 143 }, { 320, 180 } };
		const int edges[3] = { 8, 16, 32 };
		for (int si = 0; si < 2; ++si)
			for (int bi = 0; bi < 3; ++bi)
			{
				const int w = sizes[si][0], h = sizes[si][1], B = edges[bi];
				const double scale = B / 16.0;
				const std::vector<int> codes = MakeCodes12(w, h, 41u + static_cast<uint32_t>(si * 3 + bi));
				pl_tex tex = UploadQ6(gpu, codes, w, h);
				Assert::IsNotNull(tex, L"upload");
				for (int st = 0; st < 2; ++st)
				{
					const Storage storage = st == 0 ? Storage::Full : Storage::Compact;
					const Product a = Run(dev, tex, B, scale, storage, kAxisUnit, 10);
					const Product n = Run(dev, tex, B, scale, storage, kNativeUnit, 12);
					Assert::IsTrue(a.ok, Wide(a.error).c_str());
					Assert::IsTrue(n.ok, Wide(n.error).c_str());
					Assert::IsTrue(!a.a.empty() && !n.a.empty() && !a.blocks.empty() && !n.blocks.empty(), L"downloaded");
					Assert::AreEqual(a.bw, n.bw, L"block grid width");
					Assert::AreEqual(a.bh, n.bh, L"block grid height");
					// The relation is a power-of-two scaling: a kernel that treats both laws alike agrees to float eps.
					const double tol = 1e-5;
					double wS = 0, wf = 0, wBase = 0, wBlk = 0;
					bool ok = true;
					for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
					{
						const Texel ta = Decode(a, i, kAxisUnit), tn = Decode(n, i, kNativeUnit);
						ok &= Near(tn.s1, ta.s1, kScaleCode, tol, wS);
						ok &= Near(tn.s2, ta.s2, kScaleCode, tol, wS);
						ok &= Near(tn.f, ta.f, 1.0, tol, wf);
						ok &= Near(tn.base, ta.base, kScaleCode, tol, wBase);
					}
					const int blockChannels = a.compact ? 4 : 2;   // Compact: (N, e3, e1, e2); Full: (N, e3)
					for (size_t i = 0; i < static_cast<size_t>(a.bw) * a.bh; ++i)
						for (int c = 0; c < blockChannels; ++c)
							ok &= Near(n.blocks[i * 4 + c], a.blocks[i * 4 + c], kScaleEnergy, tol, wBlk);
					LogF("gpu axis==native (storage %.0f): s %.3g f %.3g", static_cast<double>(st), wS, wf);
					LogF("gpu axis==native: base %.3g blocks %.3g", wBase, wBlk);
					Assert::IsTrue(ok, L"the axis law and the native law differ on the GPU: lead decision needed (plan W21-3)");
				}
				pl_tex_destroy(gpu, &tex);
			}
	}
};
