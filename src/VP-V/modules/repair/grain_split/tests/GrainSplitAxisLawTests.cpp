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

	// Quiet-pedestal 12-bit plane: constant pedestal plus white Gaussian grain of std dev sigma12 (12-bit codes),
	// rounded to integer codes in 0..4095. With sigma12 = 2.0 (0.5 ten-bit-equivalent codes), exercises the
	// ten-bit vs twelve-bit depth-scale law cleanly without high-amplitude synthetic texture masking it.
	std::vector<int> MakeQuietPedestal12(int w, int h, int pedestal12, double sigma12, uint32_t seed)
	{
		std::vector<int> c(static_cast<size_t>(w) * h);
		std::mt19937 g(seed);
		std::normal_distribution<double> noise(0.0, sigma12);
		for (size_t i = 0; i < c.size(); ++i)
		{
			const double v = static_cast<double>(pedestal12) + noise(g);
			c[i] = static_cast<int>(std::lround((std::min)(4095.0, (std::max)(0.0, v))));
		}
		return c;
	}

	// Two-pedestal 12-bit plane: left half on a quiet mid-dark pedestal (luma12 >= 1024, bin 2 at 12-bit),
	// right half on a bright grainy pedestal (bin 6 at 12-bit). Under missing depth scaling (sourceBits = 10),
	// both pedestals exceed 1024 and collapse into bin 7, causing the quiet half's NLF cap to crush the bright
	// half's grain level N and leak grain through Wiener share f.
	std::vector<int> MakeTwoPedestal12(int w, int h, int quietPed12, double quietSigma12, int brightPed12, double brightSigma12, uint32_t seed)
	{
		std::vector<int> c(static_cast<size_t>(w) * h);
		std::mt19937 g(seed);
		std::normal_distribution<double> quietNoise(0.0, quietSigma12);
		std::normal_distribution<double> brightNoise(0.0, brightSigma12);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const bool left = x < w / 2;
				const double v = left ? (static_cast<double>(quietPed12) + quietNoise(g)) : (static_cast<double>(brightPed12) + brightNoise(g));
				c[static_cast<size_t>(y) * w + x] = static_cast<int>(std::lround((std::min)(4095.0, (std::max)(0.0, v))));
			}
		return c;
	}

	// Per-block quiet grain energy and block-centre luma by the v3 law (mirrors GrainSplit::Compute) so
	// GrainSplit::GrainLevelTable can be audited directly on the exact same 12-bit plane under sourceBits 12 vs 10.
	void ComputeBlockQuietAndLuma(const Plane& y, double scale, int B, const GrainSplit::Params& p,
		std::vector<double>& quiet, std::vector<double>& blockLuma)
	{
		const int bw = (y.w + B - 1) / B, bh = (y.h + B - 1) / B;
		const Plane g1 = GrainSplit::Gauss(y, p.sigmaFine * scale);
		const Plane yh = GrainSplit::V3Half(y);
		const Plane gbh = GrainSplit::V3HalfBlur(yh, p.sigmaBase * scale);
		Plane d(y.w, y.h);
		for (int yy = 0; yy < y.h; ++yy)
			for (int xx = 0; xx < y.w; ++xx)
			{
				const double gb = GrainSplit::V3LowAt(gbh, xx, yy);
				const double d1 = y.at(xx, yy) - g1.at(xx, yy);
				const double d2 = g1.at(xx, yy) - gb;
				d.at(xx, yy) = d1 + d2;
			}
		const size_t nb = static_cast<size_t>(bw) * bh;
		quiet.assign(nb, 0.0);
		blockLuma.assign(nb, 0.0);
		for (int by = 0; by < bh; ++by)
			for (int bx = 0; bx < bw; ++bx)
			{
				std::vector<double> e;
				for (int sj = 0; sj < p.subBlocks; ++sj)
					for (int si = 0; si < p.subBlocks; ++si)
					{
						const int x0 = bx * B + si * B / p.subBlocks, x1 = (std::min)(y.w, bx * B + (si + 1) * B / p.subBlocks);
						const int y0 = by * B + sj * B / p.subBlocks, y1 = (std::min)(y.h, by * B + (sj + 1) * B / p.subBlocks);
						if (x1 <= x0 || y1 <= y0) continue;
						double sum = 0.0;
						for (int yy = y0; yy < y1; ++yy)
							for (int xx = x0; xx < x1; ++xx) sum += d.at(xx, yy) * d.at(xx, yy);
						e.push_back(sum / static_cast<double>((x1 - x0) * (y1 - y0)));
					}
				std::sort(e.begin(), e.end());
				const size_t nq = (std::min)(e.size(), static_cast<size_t>((std::max)(1, p.quietSubBlocks)));
				double m = 0.0;
				for (size_t i = 0; i < nq; ++i) m += e[i];
				const size_t bi = static_cast<size_t>(by) * bw + bx;
				quiet[bi] = nq ? p.grainCal * m / static_cast<double>(nq) : 0.0;
				blockLuma[bi] = GrainSplit::V3LowAt(gbh, (std::min)(bx * B + B / 2, y.w - 1), (std::min)(by * B + B / 2, y.h - 1));
			}
	}

	struct PlTexGuard
	{
		pl_gpu gpu = nullptr;
		pl_tex tex = nullptr;
		PlTexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
		~PlTexGuard() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
		PlTexGuard(const PlTexGuard&) = delete;
		PlTexGuard& operator=(const PlTexGuard&) = delete;
	};
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

	// CT-M8 (Rule 3): quiet-pedestal 12-bit depth-scale law and same-input sourceBits = 10 negative control on the CPU.
	// Proves:
	//   1. On a quiet-pedestal 12-bit plane (sigma = 2.0 twelve-bit codes = 0.5 ten-bit codes), sourceBits = 12 matches
	//      the Q6 axis law and recovers the true ten-bit-equivalent grain level N (and NLF bin/level), while feeding the
	//      exact same 12-bit plane with naive sourceBits = 10 inflates ten-bit-equivalent N and NLF level by 16x
	//      (DepthScale(12)^2 = 16) and mis-bins mid-grey 12-bit luma into clipped-white bin 7.
	//   2. On a two-pedestal 12-bit plane (quiet bin-2 pedestal + bright bin-6 grain), missing depth scaling
	//      (sourceBits = 10) collapses both pedestals into bin 7 and directly corrupts Compute's N (>5x collapse) and f.
	TEST_METHOD(CpuQuietPedestalDepthScaleNegativeControl)
	{
		using namespace GrainSplitAxisLawTestsNs;
		const int w = 256, h = 144, B = 16;
		const double scale = 1.0;
		const double es10 = GrainSplit::DepthScale(10) * GrainSplit::DepthScale(10);
		const double es12 = GrainSplit::DepthScale(12) * GrainSplit::DepthScale(12);
		Assert::AreEqual(1.0, es10, 1e-12, L"DepthScale(10)^2 == 1");
		Assert::AreEqual(kScaleEnergy, es12, 1e-12, L"DepthScale(12)^2 == 16");

		GrainSplit::Params axisParams;     // sourceBits = 10
		GrainSplit::Params nativeParams;
		nativeParams.sourceBits = 12;
		Assert::IsTrue(nativeParams.levelBins == 8, L"default 8 NLF luma bins");

		// Part 1: pure quiet-pedestal 12-bit plane (pedestal 2304 = centre of 12-bit bin 4; sigma12 = 2.0 = 0.5 ten-bit codes).
		{
			const double sigma12 = 2.0;
			const double sigma10 = sigma12 / kScaleCode;
			const double sigma10Sq = sigma10 * sigma10;   // 0.25 ten-bit-equivalent codes^2
			const std::vector<int> codes = MakeQuietPedestal12(w, h, 2304, sigma12, 108u);
			const Plane planeAxis = PlaneOf(codes, w, h, 1.0 / kScaleCode);
			const Plane planeNative = PlaneOf(codes, w, h, 1.0);

			const GrainSplit::Split a = GrainSplit::Compute(planeAxis, scale, B, axisParams);
			const GrainSplit::Split n = GrainSplit::Compute(planeNative, scale, B, nativeParams);
			const GrainSplit::Split naive = GrainSplit::Compute(planeNative, scale, B, axisParams);

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
			Assert::AreEqual(a.bw, n.bw, L"quiet pedestal block grid width");
			Assert::AreEqual(a.bh, n.bh, L"quiet pedestal block grid height");
			Assert::AreEqual(a.N.size(), naive.N.size(), L"naive block count");

			double sumAxisN = 0.0, sumNativeTenN = 0.0, sumNaiveTenN = 0.0;
			double worstNaiveInflation = 0.0;
			bool allNaiveInflated16x = true;
			for (size_t i = 0; i < a.N.size(); ++i)
			{
				ok &= Near(n.N[i], a.N[i], kScaleEnergy, tol, wN);
				ok &= Near(n.e1[i], a.e1[i], kScaleEnergy, tol, wE);
				ok &= Near(n.e2[i], a.e2[i], kScaleEnergy, tol, wE);
				ok &= Near(n.e3[i], a.e3[i], kScaleEnergy, tol, wE);

				const double nativeTenN = n.N[i] / es12;
				const double naiveTenN = naive.N[i] / es10;
				sumAxisN += a.N[i];
				sumNativeTenN += nativeTenN;
				sumNaiveTenN += naiveTenN;
				allNaiveInflated16x &= Near(naiveTenN, nativeTenN, kScaleEnergy, 1e-6, worstNaiveInflation);
			}
			Assert::IsTrue(ok, L"quiet pedestal: CPU native 12-bit law must match Q6 axis law");

			const double nb = static_cast<double>(a.N.size());
			const double meanAxisN = sumAxisN / nb;
			const double meanNativeTenN = sumNativeTenN / nb;
			const double meanNaiveTenN = sumNaiveTenN / nb;
			double wMeanTen = 0.0;
			LogF("cpu quiet pedestal: axisN %.4f nativeTenN %.4f naiveTenN %.4f", meanAxisN, meanNativeTenN, meanNaiveTenN);
			Assert::IsTrue(meanAxisN >= 0.25 * sigma10Sq && meanAxisN <= 2.5 * sigma10Sq,
				L"quiet pedestal: axis 10-bit N must track planted white-noise variance sigma10^2 = 0.25");
			Assert::IsTrue(Near(meanNativeTenN, meanAxisN, 1.0, tol, wMeanTen),
				L"quiet pedestal: sourceBits = 12 must recover true ten-bit-equivalent N");
			Assert::IsTrue(allNaiveInflated16x,
				L"negative control: naive sourceBits = 10 on 12-bit plane must inflate ten-bit-equivalent N by 16x on every block");
			Assert::IsTrue(meanNaiveTenN > 2.5 * sigma10Sq,
				L"negative control: naive sourceBits = 10 ten-bit-equivalent N must violate the planted 10-bit grain bound");

			// Direct GrainLevelTable audit on the exact same 12-bit plane (level[] is contracted in ten-bit-equivalent codes^2).
			std::vector<double> quietAxis, lumaAxis, quiet12, luma12;
			ComputeBlockQuietAndLuma(planeAxis, scale, B, axisParams, quietAxis, lumaAxis);
			ComputeBlockQuietAndLuma(planeNative, scale, B, nativeParams, quiet12, luma12);
			double levelAxis[16] = {}, level12[16] = {}, level10[16] = {};
			bool validAxis[16] = {}, valid12[16] = {}, valid10[16] = {};
			GrainSplit::GrainLevelTable(quietAxis, lumaAxis, axisParams, levelAxis, validAxis);
			GrainSplit::GrainLevelTable(quiet12, luma12, nativeParams, level12, valid12);
			GrainSplit::GrainLevelTable(quiet12, luma12, axisParams, level10, valid10);

			constexpr int kMidBin = 4;
			const int topBin = nativeParams.levelBins - 1;
			double wLvlMatch = 0.0, wLvlNaive = 0.0;
			LogF("cpu quiet NLF: axis[4] %.4f native12[4] %.4f naive10[7] %.4f", levelAxis[kMidBin], level12[kMidBin], level10[topBin]);
			Assert::IsTrue(validAxis[kMidBin] && valid12[kMidBin] && !valid12[topBin],
				L"quiet pedestal: sourceBits = 12 must bin mid-grey 12-bit pedestal (2304) into bin 4, not bin 7");
			Assert::IsTrue(Near(level12[kMidBin], levelAxis[kMidBin], 1.0, tol, wLvlMatch),
				L"quiet pedestal: GrainLevelTable(sourceBits = 12) must recover exact ten-bit-equivalent NLF level");
			Assert::IsTrue(!valid10[kMidBin] && valid10[topBin],
				L"negative control: naive GrainLevelTable(sourceBits = 10) mis-bins 12-bit mid-grey (2304) into clipped-white bin 7");
			Assert::IsTrue(Near(level10[topBin], level12[kMidBin], kScaleEnergy, tol, wLvlNaive),
				L"negative control: naive GrainLevelTable(sourceBits = 10) inflates ten-bit-equivalent NLF level by 16x");
		}

		// Part 2: two-pedestal 12-bit plane (left: bin-2 quiet pedestal 1280, sigma12 = 2.0; right: bin-6 bright grain 3328, sigma12 = 12.0).
		// Proves missing depth scaling (sourceBits = 10) directly breaks GrainSplit::Compute's N and f on the same 12-bit plane.
		{
			const double quietSigma12 = 2.0, brightSigma12 = 12.0;
			const double brightSigma10Sq = (brightSigma12 / kScaleCode) * (brightSigma12 / kScaleCode);   // 9.0 ten-bit codes^2
			const std::vector<int> codes = MakeTwoPedestal12(w, h, 1280, quietSigma12, 3328, brightSigma12, 208u);
			const Plane planeAxis = PlaneOf(codes, w, h, 1.0 / kScaleCode);
			const Plane planeNative = PlaneOf(codes, w, h, 1.0);

			const GrainSplit::Split a = GrainSplit::Compute(planeAxis, scale, B, axisParams);
			const GrainSplit::Split n = GrainSplit::Compute(planeNative, scale, B, nativeParams);
			const GrainSplit::Split naive = GrainSplit::Compute(planeNative, scale, B, axisParams);

			const double tol = 1e-9;
			double wN = 0.0, wf = 0.0;
			bool ok = true;
			for (size_t i = 0; i < a.f.v.size(); ++i)
				ok &= Near(n.f.v[i], a.f.v[i], 1.0, tol, wf);
			for (size_t i = 0; i < a.N.size(); ++i)
				ok &= Near(n.N[i], a.N[i], kScaleEnergy, tol, wN);
			Assert::IsTrue(ok, L"two-pedestal: CPU native 12-bit law must match Q6 axis law");

			// Interior bright blocks (bx >= 11, >= 48 px from seam at x = 128, well clear of 5x5 block min and blur radius).
			double sumNativeBrightN = 0.0, sumNaiveBrightN = 0.0;
			int cntBrightBlocks = 0;
			for (int by = 0; by < a.bh; ++by)
				for (int bx = 11; bx < a.bw; ++bx)
				{
					const size_t bi = static_cast<size_t>(by) * a.bw + bx;
					sumNativeBrightN += n.N[bi];
					sumNaiveBrightN += naive.N[bi];
					++cntBrightBlocks;
				}
			Assert::IsTrue(cntBrightBlocks > 0, L"non-empty interior bright blocks");
			const double meanNativeBrightN = sumNativeBrightN / static_cast<double>(cntBrightBlocks);
			const double meanNaiveBrightN = sumNaiveBrightN / static_cast<double>(cntBrightBlocks);

			double sumNativeBrightF = 0.0, sumNaiveBrightF = 0.0;
			int cntBrightPx = 0;
			for (int y = 0; y < h; ++y)
				for (int x = 11 * B; x < w; ++x)
				{
					sumNativeBrightF += n.f.at(x, y);
					sumNaiveBrightF += naive.f.at(x, y);
					++cntBrightPx;
				}
			const double meanNativeBrightF = sumNativeBrightF / static_cast<double>(cntBrightPx);
			const double meanNaiveBrightF = sumNaiveBrightF / static_cast<double>(cntBrightPx);

			LogF("cpu two-pedestal bright: nativeN %.3f naiveN %.3f ratio %.4f",
				meanNativeBrightN, meanNaiveBrightN, meanNaiveBrightN / (std::max)(1e-9, meanNativeBrightN));
			LogF("cpu two-pedestal bright f: nativeF %.4f naiveF %.4f", meanNativeBrightF, meanNaiveBrightF);

			const double meanNativeBrightTenN = meanNativeBrightN / es12;
			Assert::IsTrue(meanNativeBrightTenN >= 0.25 * brightSigma10Sq && meanNativeBrightTenN <= 2.5 * brightSigma10Sq,
				L"two-pedestal: sourceBits = 12 must preserve true bright-zone grain level");
			Assert::IsTrue(meanNativeBrightF < 0.25,
				L"two-pedestal: sourceBits = 12 must suppress pure bright grain (low Wiener share f)");
			Assert::IsTrue(meanNaiveBrightN < 0.20 * meanNativeBrightN,
				L"negative control: naive sourceBits = 10 collapses bin 2 and bin 6 into bin 7, crushing bright N by >5x");
			Assert::IsTrue(meanNaiveBrightF > meanNativeBrightF + 0.40,
				L"negative control: naive sourceBits = 10 causes pure bright grain to leak through Wiener share f");
		}
	}

	// CT-M8 (Rule 3): GPU twin of CpuQuietPedestalDepthScaleNegativeControl on WARP/hardware (Full and Compact).
	// Exercises GS_CODE_SCALE = DepthScale(sourceBits) across live LEVEL / NMIN / APPLY compute passes.
	BEGIN_TEST_METHOD_ATTRIBUTE(GpuQuietPedestalDepthScaleNegativeControl)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(GpuQuietPedestalDepthScaleNegativeControl)
	{
		using namespace GrainSplitAxisLawTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();

		const int w = 256, h = 144, B = 16;
		const double scale = 1.0;
		const double tol = 1e-5;
		const double es10 = GrainSplit::DepthScale(10) * GrainSplit::DepthScale(10);
		const double es12 = GrainSplit::DepthScale(12) * GrainSplit::DepthScale(12);

		// Part 1: quiet-pedestal 12-bit plane (2304, sigma12 = 2.0 = 0.5 ten-bit codes).
		{
			const double sigma12 = 2.0;
			const double sigma10 = sigma12 / kScaleCode;
			const double sigma10Sq = sigma10 * sigma10;
			const std::vector<int> codes = MakeQuietPedestal12(w, h, 2304, sigma12, 108u);
			PlTexGuard texGuard(gpu, UploadQ6(gpu, codes, w, h));
			Assert::IsNotNull(texGuard.tex, L"upload quiet pedestal 12-bit texture");

			for (int st = 0; st < 2; ++st)
			{
				const Storage storage = st == 0 ? Storage::Full : Storage::Compact;
				const Product a = Run(dev, texGuard.tex, B, scale, storage, kAxisUnit, 10);
				const Product n = Run(dev, texGuard.tex, B, scale, storage, kNativeUnit, 12);
				const Product naive = Run(dev, texGuard.tex, B, scale, storage, kNativeUnit, 10);
				Assert::IsTrue(a.ok, Wide(a.error).c_str());
				Assert::IsTrue(n.ok, Wide(n.error).c_str());
				Assert::IsTrue(naive.ok, Wide(naive.error).c_str());
				Assert::IsTrue(!a.a.empty() && !n.a.empty() && !naive.a.empty() &&
					!a.blocks.empty() && !n.blocks.empty() && !naive.blocks.empty(), L"downloaded quiet pedestal products");
				if (a.compact)
					Assert::IsTrue(!a.b.empty() && !n.b.empty() && !naive.b.empty(), L"downloaded compact splitB");
				Assert::AreEqual(a.bw, n.bw, L"gpu quiet pedestal bw");
				Assert::AreEqual(a.bh, n.bh, L"gpu quiet pedestal bh");
				Assert::AreEqual(a.bw, naive.bw, L"gpu quiet pedestal naive bw");
				Assert::AreEqual(a.bh, naive.bh, L"gpu quiet pedestal naive bh");

				double wS = 0.0, wf = 0.0, wBase = 0.0, wBlk = 0.0;
				bool ok = true;
				for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
				{
					const Texel ta = Decode(a, i, kAxisUnit), tn = Decode(n, i, kNativeUnit);
					ok &= Near(tn.s1, ta.s1, kScaleCode, tol, wS);
					ok &= Near(tn.s2, ta.s2, kScaleCode, tol, wS);
					ok &= Near(tn.f, ta.f, 1.0, tol, wf);
					ok &= Near(tn.base, ta.base, kScaleCode, tol, wBase);
				}

				const size_t nb = static_cast<size_t>(a.bw) * a.bh;
				const int blockChannels = a.compact ? 4 : 2;
				double sumAxisN = 0.0, sumNativeTenN = 0.0, sumNaiveTenN = 0.0;
				double worstNaiveInflation = 0.0;
				bool allNaiveInflated16x = true;
				for (size_t i = 0; i < nb; ++i)
				{
					for (int c = 0; c < blockChannels; ++c)
						ok &= Near(n.blocks[i * 4 + c], a.blocks[i * 4 + c], kScaleEnergy, tol, wBlk);
					const double axisN = a.blocks[i * 4];
					const double nativeTenN = n.blocks[i * 4] / es12;
					const double naiveTenN = naive.blocks[i * 4] / es10;
					sumAxisN += axisN;
					sumNativeTenN += nativeTenN;
					sumNaiveTenN += naiveTenN;
					allNaiveInflated16x &= Near(naiveTenN, nativeTenN, kScaleEnergy, tol, worstNaiveInflation);
				}
				const double meanAxisN = sumAxisN / static_cast<double>(nb);
				const double meanNativeTenN = sumNativeTenN / static_cast<double>(nb);
				const double meanNaiveTenN = sumNaiveTenN / static_cast<double>(nb);
				LogF("gpu quiet pedestal (storage %.0f): axisN %.4f nativeTenN %.4f", static_cast<double>(st), meanAxisN, meanNativeTenN);
				LogF("gpu quiet pedestal: naiveTenN %.4f worstBlk %.3g worstInfl %.3g", meanNaiveTenN, wBlk, worstNaiveInflation);
				Assert::IsTrue(ok, L"gpu quiet pedestal: native 12-bit law must match Q6 axis law");
				Assert::IsTrue(meanAxisN >= 0.25 * sigma10Sq && meanAxisN <= 2.5 * sigma10Sq,
					L"gpu quiet pedestal: axis 10-bit N must track planted white-noise variance sigma10^2 = 0.25");
				Assert::IsTrue(allNaiveInflated16x,
					L"gpu negative control: naive sourceBits = 10 on 12-bit plane must inflate ten-bit-equivalent N by 16x on every block");
				Assert::IsTrue(meanNaiveTenN > 2.5 * sigma10Sq,
					L"gpu negative control: naive sourceBits = 10 ten-bit-equivalent N must violate the planted 10-bit grain bound");
			}
		}

		// Part 2: two-pedestal 12-bit plane (1280/sigma 2.0 + 3328/sigma 12.0) proving live GPU NLF bin-collapse corruption.
		{
			const double quietSigma12 = 2.0, brightSigma12 = 12.0;
			const double brightSigma10Sq = (brightSigma12 / kScaleCode) * (brightSigma12 / kScaleCode);
			const std::vector<int> codes = MakeTwoPedestal12(w, h, 1280, quietSigma12, 3328, brightSigma12, 208u);
			PlTexGuard texGuard(gpu, UploadQ6(gpu, codes, w, h));
			Assert::IsNotNull(texGuard.tex, L"upload two-pedestal 12-bit texture");

			for (int st = 0; st < 2; ++st)
			{
				const Storage storage = st == 0 ? Storage::Full : Storage::Compact;
				const Product a = Run(dev, texGuard.tex, B, scale, storage, kAxisUnit, 10);
				const Product n = Run(dev, texGuard.tex, B, scale, storage, kNativeUnit, 12);
				const Product naive = Run(dev, texGuard.tex, B, scale, storage, kNativeUnit, 10);
				Assert::IsTrue(a.ok, Wide(a.error).c_str());
				Assert::IsTrue(n.ok, Wide(n.error).c_str());
				Assert::IsTrue(naive.ok, Wide(naive.error).c_str());
				Assert::IsTrue(!a.a.empty() && !n.a.empty() && !naive.a.empty() &&
					!a.blocks.empty() && !n.blocks.empty() && !naive.blocks.empty(), L"downloaded two-pedestal products");
				if (a.compact)
					Assert::IsTrue(!a.b.empty() && !n.b.empty() && !naive.b.empty(), L"downloaded compact two-pedestal splitB");
				Assert::AreEqual(a.bw, n.bw, L"gpu two-pedestal bw");
				Assert::AreEqual(a.bh, n.bh, L"gpu two-pedestal bh");
				Assert::AreEqual(a.bw, naive.bw, L"gpu two-pedestal naive bw");
				Assert::AreEqual(a.bh, naive.bh, L"gpu two-pedestal naive bh");

				double wf = 0.0, wBlk = 0.0;
				bool ok = true;
				for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
					ok &= Near(Decode(n, i, kNativeUnit).f, Decode(a, i, kAxisUnit).f, 1.0, tol, wf);
				for (size_t i = 0; i < static_cast<size_t>(a.bw) * a.bh; ++i)
					ok &= Near(n.blocks[i * 4], a.blocks[i * 4], kScaleEnergy, tol, wBlk);
				Assert::IsTrue(ok, L"gpu two-pedestal: native 12-bit law must match Q6 axis law");

				double sumNativeBrightN = 0.0, sumNaiveBrightN = 0.0;
				int cntBrightBlocks = 0;
				for (int by = 0; by < a.bh; ++by)
					for (int bx = 11; bx < a.bw; ++bx)
					{
						const size_t bi = static_cast<size_t>(by) * a.bw + bx;
						sumNativeBrightN += n.blocks[bi * 4];
						sumNaiveBrightN += naive.blocks[bi * 4];
						++cntBrightBlocks;
					}
				const double meanNativeBrightN = sumNativeBrightN / static_cast<double>(cntBrightBlocks);
				const double meanNaiveBrightN = sumNaiveBrightN / static_cast<double>(cntBrightBlocks);

				double sumNativeBrightF = 0.0, sumNaiveBrightF = 0.0;
				int cntBrightPx = 0;
				for (int y = 0; y < h; ++y)
					for (int x = 11 * B; x < w; ++x)
					{
						const size_t pi = static_cast<size_t>(y) * w + x;
						sumNativeBrightF += Decode(n, pi, kNativeUnit).f;
						sumNaiveBrightF += Decode(naive, pi, kNativeUnit).f;
						++cntBrightPx;
					}
				const double meanNativeBrightF = sumNativeBrightF / static_cast<double>(cntBrightPx);
				const double meanNaiveBrightF = sumNaiveBrightF / static_cast<double>(cntBrightPx);

				LogF("gpu two-pedestal bright (storage %.0f): nativeN %.3f naiveN %.3f",
					static_cast<double>(st), meanNativeBrightN, meanNaiveBrightN);
				LogF("gpu two-pedestal bright f: nativeF %.4f naiveF %.4f", meanNativeBrightF, meanNaiveBrightF);

				const double meanNativeBrightTenN = meanNativeBrightN / es12;
				Assert::IsTrue(meanNativeBrightTenN >= 0.25 * brightSigma10Sq && meanNativeBrightTenN <= 2.5 * brightSigma10Sq,
					L"gpu two-pedestal: sourceBits = 12 must preserve true bright-zone grain level");
				Assert::IsTrue(meanNativeBrightF < 0.25,
					L"gpu two-pedestal: sourceBits = 12 must suppress pure bright grain (low Wiener share f)");
				Assert::IsTrue(meanNaiveBrightN < 0.20 * meanNativeBrightN,
					L"gpu negative control: naive sourceBits = 10 collapses bin 2 and bin 6 into bin 7, crushing bright N by >5x");
				Assert::IsTrue(meanNaiveBrightF > meanNativeBrightF + 0.40,
					L"gpu negative control: naive sourceBits = 10 causes pure bright grain to leak through Wiener share f");
			}
		}
	}
};
