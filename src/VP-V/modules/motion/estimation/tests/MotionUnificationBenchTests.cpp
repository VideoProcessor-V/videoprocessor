// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV (doc 83, owner decision 2026-11-08 #1): the before / after harness of the single-NVOF-source change.
// The de-judder used to open its own NvOpticalFlow session (preset medium, the matcher's GPU luma) whenever no pair came from the
// look-ahead producer; now the producer (preset slow, CPU half luma) is the only session and a miss means the block searches (P2m).
// The SAME test runs in the "before" tree (integ without SK-MV) and the "after" tree (SK-MV); the mode "nopair" is the render-time
// engine in the first and the matcher in the second, the mode "pair" (a pair computed by the producer, SetExternalFlow) is the same
// in both. Compare the two csv files with tools/eval/motion_bench/mv_compare.py.
//
// Hardware only (VP_TEST_GPU=hardware, an NVIDIA adapter) and env-gated; elsewhere it logs and passes. No film frame is ever written:
// the csv holds numbers only; the clip is read from the folder in the environment and stays there.
//   VP_MV_BENCH_CLIP    a .y4m (8-bit 4:2:0), e.g. D:\testclips\film\meridian_1920x800_60p.y4m  (required)
//   VP_MV_BENCH_OUT     the csv to write (required)
//   VP_MV_BENCH_LABEL   "before" or "after" (default "run")
//   VP_MV_BENCH_PAIRS   number of frame positions (default 16), VP_MV_BENCH_START the first frame (default 0), VP_MV_BENCH_STEP the
//                       spacing of the positions in frames (default 12)
//
// Per position n of the clip, these measurements (kind column of the csv; tools/eval/motion_bench/mv_compare.py reads shift and interp
// and ignores kinds it does not know, so the extra kinds below do not disturb its table):
//   shift   frame n as A, B = A moved by a known INTEGER (dx, dy) (real texture, exact by construction: the error is ~0 and the trust 1
//           for any decent matcher, so it does not discriminate quality; kept for the comparison with the older runs): the trusted
//           blocks' vector error (source px) and the trusted share.
//   subpx   the same with a FRACTIONAL global shift (bilinear resampling, ground truth = the shift).
//   nonrigid  B = A under a smooth displacement field (translation + zoom + shear + a small wave), the ground truth vector of each block
//           taken at its centre (fixed-point inverse of the field): the vector error here is a real figure of merit.
//           Both extra kinds also write epe_p95_px (the 95th percentile of the trusted blocks' error).
//   interp  frames n and n + 2 as A and B, the picture halfway (Warp at 0.5) against the real frame n + 1: luma PSNR and SSIM
//           (60p material), plus the same for holding A (the floor the warp must beat).
// Each in the two modes. Cost columns: the producer's pair time (lastPairMs, the NVOF ms of the pair mode) and the wall time of
// the Estimate call (mode nopair before = includes the render-time engine run).
// Measurement hygiene (doc 93 W2-C5a): ONE MotionWarp is built once and reused for every mode and position (a fresh warp per mode
// charged shader compilation / texture allocation to whichever mode ran on it first); ForgetHistory() before each estimate keeps the
// positions independent. The first use of each mode runs one untimed warm-up estimate (its wall ms is written as cold_estimate_ms).
// The GPU is drained (pl_gpu_finish) before the clock starts and again before it stops, so estimate_ms is the GPU wall time, not the CPU
// submit time. The order of the two modes alternates from group to group, so neither always pays the first-use costs.
// Notes on reading the rows:
//   - estimate_ms includes the re-creation of the fade / strength state textures that ForgetHistory() destroys (EnsureTextures builds
//     them again inside the timed Estimate): a small constant in every row, not part of a steady-state estimate.
//   - cold_estimate_ms is written only under kind "shift", on the first group, once per mode. The first mode of the run pays the shader
//     compilation; the second mode's figure is shader-warm (pipelines are shared inside the process), so it is NOT a compile cost.
//   - Rows whose meaning changed against mvbench\COMPARE.txt (older runs): estimate_ms (BeforeAfter, was "estimate wall") is now the
//     warmed GPU wall time, so it is NOT comparable with the old 340 ms pair / 1600 ms nopair; in the steady csv render_ms_mean /
//     render_ms_p95 are steady-state with ONE warp per mode, render_estimates is n - 1 (the first estimate is excluded), render_first_ms
//     is new (that first estimate, shader compile and texture allocation included) and vram_render_mb is the persistent warp's footprint.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/OnDemandIssue.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <libplacebo/d3d11.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <atomic>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
	std::string MvEnv(const char* name)
	{
		char* v = nullptr;
		size_t n = 0;
		std::string out;
		if (_dupenv_s(&v, &n, name) == 0 && v)
			out = v;
		std::free(v);
		return out;
	}

	int MvEnvInt(const char* name, int fallback)
	{
		const std::string v = MvEnv(name);
		return v.empty() ? fallback : std::atoi(v.c_str());
	}

	// A .y4m, luma only, 8-bit 4:2:0 (C420*, or no C tag).
	struct Y4mClip
	{
		std::ifstream file;
		int width = 0, height = 0;
		double fps = 0.0;   // from the F tag (num:den); 0 when absent
		std::streamoff firstFrame = 0;
		size_t frameBytes = 0;
		bool Open(const std::string& path, std::string& why)
		{
			file.open(path, std::ios::binary);
			if (!file) { why = "cannot open " + path; return false; }
			std::string header;
			std::getline(file, header);
			if (header.compare(0, 9, "YUV4MPEG2") != 0) { why = "not a y4m"; return false; }
			std::istringstream words(header);
			std::string w;
			bool chromaOk = true;
			while (words >> w)
			{
				if (w[0] == 'W') width = std::atoi(w.c_str() + 1);
				else if (w[0] == 'H') height = std::atoi(w.c_str() + 1);
				else if (w[0] == 'F')
				{
					const size_t colon = w.find(':');
					const double num = std::atof(w.c_str() + 1), den = colon == std::string::npos ? 1.0 : std::atof(w.c_str() + colon + 1);
					fps = den > 0.0 ? num / den : 0.0;
				}
				else if (w[0] == 'C') chromaOk = w.compare(0, 4, "C420") == 0;
			}
			if (width <= 0 || height <= 0 || !chromaOk) { why = "needs 8-bit 4:2:0 (C420) with W and H"; return false; }
			firstFrame = file.tellg();
			frameBytes = static_cast<size_t>(width) * height * 3 / 2;
			return true;
		}
		bool Luma(int index, std::vector<uint8_t>& out)
		{
			const std::streamoff at = firstFrame + static_cast<std::streamoff>(index) * (static_cast<std::streamoff>(frameBytes) + 6);   // "FRAME\n"
			file.clear();
			file.seekg(at + 6);
			out.resize(static_cast<size_t>(width) * height);
			file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
			return static_cast<bool>(file);
		}
	};

	// 8-bit limited-range codes into the 16-bit codes the matcher takes (v << 8, as P010's high bits), neutral chroma.
	void MvFrame(const std::vector<uint8_t>& luma, int width, int height, DejudderFrame& frame)
	{
		frame.width = width;
		frame.height = height;
		frame.luma.resize(luma.size());
		for (size_t i = 0; i < luma.size(); ++i)
			frame.luma[i] = static_cast<uint16_t>(luma[i] << 8);
		frame.chroma.assign(static_cast<size_t>(width / 2) * height * 2, static_cast<uint16_t>(32768));
	}

	AnalysisLumaSource MvSource(const std::vector<uint8_t>& luma, int width, int height)
	{
		auto buffer = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(width) * height * 3 / 2, static_cast<uint16_t>(512 << 6));
		for (size_t i = 0; i < luma.size(); ++i)
			(*buffer)[i] = static_cast<uint16_t>(luma[i] << 8);   // 10-bit codes (v * 4) << 6
		AnalysisLumaSource source;
		source.data = reinterpret_cast<const uint8_t*>(buffer->data());
		source.dataBytes = buffer->size() * 2;
		source.width = width;
		source.height = height;
		source.rowBytes = static_cast<size_t>(width) * 2;
		source.chromaRowBytes = static_cast<size_t>(width) * 2;
		source.format = AnalysisLumaFormat::P010;
		source.backingOwner = buffer;
		return source;
	}

	void MvShift(const std::vector<uint8_t>& a, int width, int height, int dx, int dy, std::vector<uint8_t>& b)
	{
		b.resize(a.size());
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const int sx = (std::min)((std::max)(x - dx, 0), width - 1), sy = (std::min)((std::max)(y - dy, 0), height - 1);
				b[static_cast<size_t>(y) * width + x] = a[static_cast<size_t>(sy) * width + sx];
			}
	}

	// A smooth displacement model for the ground-truth cases: B(x, y) = A(x - dx, y - dy), (dx, dy) = At(x, y), in source px.
	// Pure translation when zoom = shear = wave = 0 (the fractional "subpx" case); otherwise the "nonrigid" case.
	struct MvDisplacement
	{
		double tx = 0.0, ty = 0.0;      // translation
		double zoom = 0.0;              // displacement at the picture edge from a zoom about the centre
		double shear = 0.0;             // displacement at the edge from a shear
		double wave = 0.0, period = 256.0;   // a sinusoidal ripple (amplitude, period in px)
		int width = 1, height = 1;
		void At(double x, double y, double& dx, double& dy) const
		{
			const double u = (x - 0.5 * width) / (0.5 * width), v = (y - 0.5 * height) / (0.5 * height);
			const double twoPi = 6.283185307179586;
			dx = tx + zoom * u + shear * v + wave * std::sin(twoPi * y / period);
			dy = ty + zoom * v - 0.5 * shear * u + wave * std::sin(twoPi * x / period);
		}
		// The motion of A's content at (x, y): the vector v with v = d(position + v) (B(p + v) = A(p)); eight fixed-point steps (converged well below 1e-6 px).
		void VectorAt(double x, double y, double& vx, double& vy) const
		{
			At(x, y, vx, vy);
			for (int i = 0; i < 8; ++i)
			{
				double nx = 0.0, ny = 0.0;
				At(x + vx, y + vy, nx, ny);
				vx = nx;
				vy = ny;
			}
		}
	};

	// B(x, y) = A(x - dx, y - dy) by bilinear sampling, edges clamped.
	void MvWarpBilinear(const std::vector<uint8_t>& a, int width, int height, const MvDisplacement& d, std::vector<uint8_t>& b)
	{
		b.resize(a.size());
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				double dx = 0.0, dy = 0.0;
				d.At(x, y, dx, dy);
				const double sx = (std::min)((std::max)(x - dx, 0.0), width - 1.0), sy = (std::min)((std::max)(y - dy, 0.0), height - 1.0);
				const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
				const int x1 = (std::min)(x0 + 1, width - 1), y1 = (std::min)(y0 + 1, height - 1);
				const double fx = sx - x0, fy = sy - y0;
				const double top = a[static_cast<size_t>(y0) * width + x0] * (1.0 - fx) + a[static_cast<size_t>(y0) * width + x1] * fx;
				const double bottom = a[static_cast<size_t>(y1) * width + x0] * (1.0 - fx) + a[static_cast<size_t>(y1) * width + x1] * fx;
				b[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(top * (1.0 - fy) + bottom * fy + 0.5);
			}
	}

	// Like MvFieldError, but the ground truth is the displacement model at each block's centre (16 source px blocks), and the
	// 95th percentile of the trusted blocks' error is returned as well. -1 when no block is trusted.
	double MvFieldErrorVsModel(const DejudderField& field, const MvDisplacement& model, long& trusted, long& total, double& p95)
	{
		std::vector<double> errors;
		double sum = 0.0;
		trusted = total = 0;
		p95 = -1.0;
		for (int by = 2; by < field.blocksY - 2; ++by)
			for (int bx = 2; bx < field.blocksX - 2; ++bx)
			{
				++total;
				if (!field.trusted(bx, by)) continue;
				double gx = 0.0, gy = 0.0;
				model.VectorAt(bx * 16.0 + 8.0, by * 16.0 + 8.0, gx, gy);
				const double e = std::hypot(field.vx(bx, by) - gx, field.vy(bx, by) - gy);
				sum += e;
				errors.push_back(e);
				++trusted;
			}
		if (!trusted) return -1.0;
		std::sort(errors.begin(), errors.end());
		p95 = errors[static_cast<size_t>(0.95 * (errors.size() - 1))];
		return sum / trusted;
	}

	// One Estimate on a warmed matcher, timed with the GPU drained on both sides (pl_gpu_finish), history forgotten first so
	// that every call is an independent pair.
	double MvTimedEstimate(pl_gpu gpu, Dejudder::MotionWarp& warp, const Dejudder::Planes& a, const Dejudder::Planes& b, bool& ok)
	{
		warp.ForgetHistory();
		pl_gpu_finish(gpu);
		const auto t0 = std::chrono::steady_clock::now();
		ok = warp.Estimate(a, b);
		pl_gpu_finish(gpu);
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}

	// Luma PSNR (dB, peak 219: limited range) and the mean SSIM of 8 x 8 windows over the interior (a 16 px border excluded).
	void MvQuality(const std::vector<uint8_t>& x, const std::vector<uint8_t>& y, int width, int height, double& psnr, double& ssim)
	{
		const int border = 16;
		double se = 0.0;
		long count = 0;
		for (int j = border; j < height - border; ++j)
			for (int i = border; i < width - border; ++i)
			{
				const double d = static_cast<double>(x[static_cast<size_t>(j) * width + i]) - y[static_cast<size_t>(j) * width + i];
				se += d * d;
				++count;
			}
		const double mse = count ? se / count : 0.0;
		psnr = mse > 1e-9 ? 10.0 * std::log10(219.0 * 219.0 / mse) : 99.0;
		const double c1 = (0.01 * 219.0) * (0.01 * 219.0), c2 = (0.03 * 219.0) * (0.03 * 219.0);
		double sum = 0.0;
		long windows = 0;
		for (int j = border; j + 8 <= height - border; j += 8)
			for (int i = border; i + 8 <= width - border; i += 8)
			{
				double mx = 0, my = 0, vx = 0, vy = 0, cxy = 0;
				for (int v = 0; v < 8; ++v)
					for (int u = 0; u < 8; ++u)
					{
						mx += x[static_cast<size_t>(j + v) * width + i + u];
						my += y[static_cast<size_t>(j + v) * width + i + u];
					}
				mx /= 64.0; my /= 64.0;
				for (int v = 0; v < 8; ++v)
					for (int u = 0; u < 8; ++u)
					{
						const double ax = x[static_cast<size_t>(j + v) * width + i + u] - mx, ay = y[static_cast<size_t>(j + v) * width + i + u] - my;
						vx += ax * ax; vy += ay * ay; cxy += ax * ay;
					}
				vx /= 63.0; vy /= 63.0; cxy /= 63.0;
				sum += ((2 * mx * my + c1) * (2 * cxy + c2)) / ((mx * mx + my * my + c1) * (vx + vy + c2));
				++windows;
			}
		ssim = windows ? sum / windows : 0.0;
	}

	// The mean vector error of the trusted blocks (the 2-block border excluded) against a known motion in the FIELD's own unit.
	// UNIT (checked against the existing tests, which settle the audit's doubt): Field() - what DownloadField reads and the harness scores -
	// is the VERIFIED field in SOURCE pixels (DejudderWarp.h: "rgba = (vx, vy) in source pixels"; DejudderHookTests compare field.vx with
	// the 12 px source shift). Forward() / Backward() (the raw searches) are in half-size pixels (the same tests expect 6 and 3 there).
	// The harness scores Field(), so the shift is compared as it is. Returns -1 when no block is trusted; trusted / total count blocks.
	double MvFieldError(const DejudderField& field, double dxField, double dyField, long& trusted, long& total)
	{
		double sum = 0.0;
		trusted = total = 0;
		for (int by = 2; by < field.blocksY - 2; ++by)
			for (int bx = 2; bx < field.blocksX - 2; ++bx)
			{
				++total;
				if (!field.trusted(bx, by)) continue;
				sum += std::hypot(field.vx(bx, by) - dxField, field.vy(bx, by) - dyField);
				++trusted;
			}
		return trusted ? sum / trusted : -1.0;
	}

	// The LUID of the adapter a D3D11 device runs on, as one word (0 = not known). Local to the harness so the SAME test builds in the
	// before tree (which has no NvOpticalFlow::Sessions).
	uint64_t MvAdapterKey(ID3D11Device* device)
	{
		if (!device)
			return 0;
		CComPtr<IDXGIDevice> dxgiDevice;
		CComPtr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC desc{};
		if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
			FAILED(adapter->GetDesc(&desc)))
			return 0;
		return (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
	}
	uint64_t MvGpuKey(pl_gpu gpu)
	{
		pl_d3d11 d3d = pl_d3d11_get(gpu);
		return d3d && d3d->device ? MvAdapterKey(d3d->device) : 0;
	}

	// warp.NoPairAhead() exists only in the tree with the single NVOF source; the before tree reports -1 (not applicable). SFINAE keeps one
	// source file for both trees.
	template <class W>
	auto MvNoPairAhead(const W& warp, int) -> decltype(static_cast<double>(warp.NoPairAhead()))
	{
		return warp.NoPairAhead() ? 1.0 : 0.0;
	}
	template <class W>
	double MvNoPairAhead(const W&, long)
	{
		return -1.0;
	}

	std::vector<uint8_t> MvLuma8(const std::vector<uint16_t>& codes)
	{
		std::vector<uint8_t> out(codes.size());
		for (size_t i = 0; i < codes.size(); ++i)
			out[i] = static_cast<uint8_t>((std::min)(255u, (static_cast<unsigned>(codes[i]) + 128u) >> 8));
		return out;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(MotionUnificationBenchTests)
	{
	public:
		// The scorer itself (CPU): exact vectors in the field's unit (source px) score ~0, a wrong unit scores far off, an untrusted block
		// is not counted.
		TEST_METHOD(FieldErrorScorerUnitsAndPerfectVectors)
		{
			DejudderField f;
			f.blocksX = 8; f.blocksY = 8;
			f.texels.assign(static_cast<size_t>(f.blocksX) * f.blocksY * 4, 0.0f);
			for (int i = 0; i < f.blocksX * f.blocksY; ++i)
			{
				f.texels[static_cast<size_t>(i) * 4] = 8.0f;       // an 8 source px shift, as Field() holds it
				f.texels[static_cast<size_t>(i) * 4 + 1] = 2.0f;   // a 2 source px shift
				f.texels[static_cast<size_t>(i) * 4 + 2] = 1.0f;   // trusted
			}
			long trusted = 0, total = 0;
			Assert::AreEqual(0.0, MvFieldError(f, 8.0, 2.0, trusted, total), 1e-9, L"perfect vectors in the field's unit");
			Assert::AreEqual<long>(16, total, L"the interior 4 x 4 blocks");
			Assert::AreEqual<long>(16, trusted);
			Assert::IsTrue(MvFieldError(f, 4.0, 1.0, trusted, total) > 4.0, L"the wrong unit (a half-size shift against source-pixel vectors) is caught");
			f.texels[(2 * 8 + 2) * 4 + 2] = 0.0f;   // one interior block untrusted
			MvFieldError(f, 8.0, 2.0, trusted, total);
			Assert::AreEqual<long>(15, trusted, L"an untrusted block is not scored");
		}

		// The ground-truth model and its scorer (CPU): bilinear warping at an integer shift equals the integer shift, the block-centre
		// ground truth of the non-rigid field inverts the displacement (a field built from it scores ~0), a biased field scores the bias,
		// an untrusted block is not counted.
		TEST_METHOD(GroundTruthModelAndScorer)
		{
			const int width = 96, height = 64;
			std::vector<uint8_t> a(static_cast<size_t>(width) * height), viaShift, viaBilinear;
			for (size_t i = 0; i < a.size(); ++i)
				a[i] = static_cast<uint8_t>((i * 37u + (i / width) * 11u) & 255u);
			MvDisplacement integer;
			integer.tx = 5.0; integer.ty = 2.0; integer.width = width; integer.height = height;
			MvShift(a, width, height, 5, 2, viaShift);
			MvWarpBilinear(a, width, height, integer, viaBilinear);
			Assert::IsTrue(viaShift == viaBilinear, L"bilinear warping at an integer shift is the integer shift");

			MvDisplacement model;
			model.tx = 4.3; model.ty = 1.2; model.zoom = 3.0; model.shear = 1.5; model.wave = 1.5; model.period = 320.0;
			model.width = 1920; model.height = 1080;
			double vx = 0.0, vy = 0.0, dx = 0.0, dy = 0.0;
			model.VectorAt(968.0, 548.0, vx, vy);
			model.At(968.0 + vx, 548.0 + vy, dx, dy);
			Assert::AreEqual(vx, dx, 1e-6, L"the ground-truth vector satisfies v = d(p + v) (x)");
			Assert::AreEqual(vy, dy, 1e-6, L"the ground-truth vector satisfies v = d(p + v) (y)");
			Assert::IsTrue(std::fabs(vx - 4.3) < 6.0 && std::fabs(vy - 1.2) < 6.0, L"the field stays in the stated magnitude");

			DejudderField f;
			f.blocksX = 12; f.blocksY = 8;
			f.texels.assign(static_cast<size_t>(f.blocksX) * f.blocksY * 4, 0.0f);
			for (int by = 0; by < f.blocksY; ++by)
				for (int bx = 0; bx < f.blocksX; ++bx)
				{
					double gx = 0.0, gy = 0.0;
					model.VectorAt(bx * 16.0 + 8.0, by * 16.0 + 8.0, gx, gy);
					const size_t i = (static_cast<size_t>(by) * f.blocksX + bx) * 4;
					f.texels[i] = static_cast<float>(gx);
					f.texels[i + 1] = static_cast<float>(gy);
					f.texels[i + 2] = 1.0f;
				}
			long trusted = 0, total = 0;
			double p95 = 0.0;
			Assert::AreEqual(0.0, MvFieldErrorVsModel(f, model, trusted, total, p95), 1e-4, L"a field built from the ground truth scores ~0");
			Assert::AreEqual<long>(32, total, L"the interior 8 x 4 blocks");
			Assert::AreEqual<long>(32, trusted);
			for (int i = 0; i < f.blocksX * f.blocksY; ++i)
				f.texels[static_cast<size_t>(i) * 4] += 0.5f;   // a 0.5 px bias on x
			Assert::AreEqual(0.5, MvFieldErrorVsModel(f, model, trusted, total, p95), 1e-3, L"a 0.5 px bias scores 0.5");
			Assert::AreEqual(0.5, p95, 1e-3);
			f.texels[(2 * f.blocksX + 2) * 4 + 2] = 0.0f;
			MvFieldErrorVsModel(f, model, trusted, total, p95);
			Assert::AreEqual<long>(31, trusted, L"an untrusted block is not scored");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(BeforeAfterSingleNvofSource)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BeforeAfterSingleNvofSource)
		{
			const std::string clipPath = MvEnv("VP_MV_BENCH_CLIP"), outPath = MvEnv("VP_MV_BENCH_OUT");
			if (clipPath.empty() || outPath.empty())
			{
				Logger::WriteMessage("MotionUnificationBench: VP_MV_BENCH_CLIP / VP_MV_BENCH_OUT not set: not run\n");
				return;
			}
			CComPtr<IDXGIFactory1> factory;
			CComPtr<IDXGIAdapter1> nvidia;
			if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
				for (UINT i = 0;; ++i)
				{
					CComPtr<IDXGIAdapter1> adapter;
					if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
					DXGI_ADAPTER_DESC1 desc{};
					adapter->GetDesc1(&desc);
					if (desc.VendorId == 0x10DE) nvidia = adapter;
				}
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, "BeforeAfterSingleNvofSource", nvidia != nullptr, "no NVIDIA adapter"))
				return;
			Y4mClip clip;
			std::string why;
			Assert::IsTrue(clip.Open(clipPath, why), std::wstring(why.begin(), why.end()).c_str());
			const int width = clip.width, height = clip.height;
			const int pairs = MvEnvInt("VP_MV_BENCH_PAIRS", 16), start = MvEnvInt("VP_MV_BENCH_START", 0), step = MvEnvInt("VP_MV_BENCH_STEP", 12);
			const std::string label = MvEnv("VP_MV_BENCH_LABEL").empty() ? "run" : MvEnv("VP_MV_BENCH_LABEL");

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			CComPtr<ID3D11Device> device;
			Assert::IsTrue(SUCCEEDED(D3D11CreateDevice(nvidia, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
			// The producer and the matcher must run on the SAME GPU, or "one session per GPU" and the timings mean nothing: a WARP or
			// integrated-GPU fixture fails here, clearly (set VP_TEST_GPU=hardware and VP_TEST_ADAPTER=NVIDIA).
			const uint64_t nvidiaKey = MvAdapterKey(device), fixtureKey = MvGpuKey(gpu);
			Assert::IsTrue(nvidiaKey != 0 && fixtureKey == nvidiaKey,
				L"the libplacebo fixture device is not on the NVIDIA adapter the producer uses (VP_TEST_GPU=hardware, VP_TEST_ADAPTER=NVIDIA)");

			std::ofstream csv(outPath, std::ios::trunc);
			Assert::IsTrue(static_cast<bool>(csv), L"cannot write the csv");
			csv << "label,clip,kind,mode,position,metric,value\n";
			const std::string clipName = clipPath.substr(clipPath.find_last_of("\\/") == std::string::npos ? 0 : clipPath.find_last_of("\\/") + 1);
			auto row = [&](const char* kind, const char* mode, int position, const char* metric, double value) {
				csv << label << ',' << clipName << ',' << kind << ',' << mode << ',' << position << ',' << metric << ',' << value << '\n';
			};

			// pair computed by the producer for the two frames (sequence 1 then 2), or null.
			auto producerPair = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, double& pairMs) {
				MotionAhead::Producer producer;
				MotionAhead::FlowPtr flow;
				pairMs = 0.0;
				if (!producer.Start(device, NvOpticalFlow::Preset::Slow))
					return flow;
				producer.Submit(1, 1, MvSource(a, width, height));
				producer.Submit(2, 1, MvSource(b, width, height));
				for (int k = 0; k < 600 && !flow; ++k)
				{
					flow = producer.Take(1, 2);
					if (!flow && !producer.Running()) break;
					if (!flow) std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
				pairMs = producer.GetStats().lastPairMs;
				producer.Stop();
				return flow;
			};
			auto settingsFor = []() {
				Dejudder::Settings settings = DejudderTestSettings();
				settings.opticalFlow = true;
				settings.opticalFlowCandidate = true;
				settings.opticalFlowSeed = true;
				settings.globalPrior = true;   // as live
				settings.estCandidates = true;
				settings.globalTwo = true;
				settings.opticalFlowGlobal = true;
				return settings;
			};

			// ONE matcher for every mode, kind and position (see the header): shaders compiled and textures allocated once.
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settingsFor());
			bool warmed[2] = { false, false };
			int rotation = 0;   // alternates the order of the two modes from group to group
			std::vector<uint8_t> lumaA, lumaB, lumaMid, lumaNext;
			int ran = 0;
			for (int p = 0; p < pairs; ++p)
			{
				const int n = start + p * step;
				if (!clip.Luma(n, lumaA) || !clip.Luma(n + 1, lumaMid) || !clip.Luma(n + 2, lumaNext)) break;
				++ran;
				DejudderFrame frameA, frameB, frameNext;
				MvFrame(lumaA, width, height, frameA);

				// One group of the two modes on frame A against frame B (the producer pair is made from the same two pictures).
				// ground != null: score the field against that displacement model; else against the constant (constX, constY).
				// interpolate: score the halfway picture against `reference` instead of the field.
				auto runGroup = [&](const char* kind, const DejudderFrame& frameB2, const MotionAhead::FlowPtr& pairFlow, double pairMs, const MvDisplacement* ground,
									double constX, double constY, bool interpolate, const std::vector<uint8_t>& reference, double holdPsnr, double holdSsim) {
					const int first = (rotation++) & 1;
					for (int k = 0; k < 2; ++k)
					{
						const int mode = k == 0 ? first : 1 - first;
						const char* name = mode == 0 ? "nopair" : "pair";
						if (mode == 1 && !pairFlow) { row(kind, name, n, "no_pair", 1.0); continue; }
						DejudderGpuPlanes pa, pb;
						UploadDejudderFrame(gpu, frameA, pa);
						UploadDejudderFrame(gpu, frameB2, pb);
						bool ok = false;
						if (!warmed[mode])
						{
							// Untimed warm-up of this mode (the pair / no-pair branch compiles its pipelines on first use); the cold figure is kept.
							if (mode == 1) warp.SetExternalFlow(pairFlow);
							const double cold = MvTimedEstimate(gpu, warp, pa.planes, pb.planes, ok);
							Assert::IsTrue(ok, L"the untimed warm-up estimate must succeed (otherwise the warm figures are not warm)");
							row(kind, name, n, "cold_estimate_ms", cold);
							warp.SetExternalFlow(nullptr);
							warmed[mode] = true;
						}
						if (mode == 1)
							warp.SetExternalFlow(pairFlow);
						const double ms = MvTimedEstimate(gpu, warp, pa.planes, pb.planes, ok);
						Dejudder::Planes out;
						if (!ok || (interpolate && !warp.Warp(pa.planes, 0.5f, out))) { row(kind, name, n, "estimate_failed", 1.0); continue; }
						if (mode == 1)
							Assert::IsTrue(warp.ExternalFlowUsed(), L"the pair estimate must have taken the pair ahead (size or device mismatch otherwise)");
						if (interpolate)
						{
							const std::vector<uint8_t> mid = MvLuma8(DownloadPlane(gpu, out.luma));
							double psnr = 0.0, ssim = 0.0;
							MvQuality(mid, reference, width, height, psnr, ssim);
							row(kind, name, n, "psnr_db", psnr);
							row(kind, name, n, "ssim", ssim);
							row(kind, name, n, "hold_psnr_db", holdPsnr);
							row(kind, name, n, "hold_ssim", holdSsim);
						}
						else
						{
							const DejudderField field = DownloadField(gpu, warp);
							long trusted = 0, total = 0;
							double p95 = -1.0;
							MvDisplacement constant;
							constant.tx = constX; constant.ty = constY; constant.width = width; constant.height = height;
							const double epe = MvFieldErrorVsModel(field, ground ? *ground : constant, trusted, total, p95);   // Field() is in source px
							row(kind, name, n, "epe_px", epe);
							row(kind, name, n, "trusted_share", total ? static_cast<double>(trusted) / total : 0.0);
							if (ground) row(kind, name, n, "epe_p95_px", p95);
						}
						row(kind, name, n, "estimate_ms", ms);
						row(kind, name, n, "used_optical_flow_pair", warp.ExternalFlowUsed() ? 1.0 : 0.0);
						row(kind, name, n, "engine_used", warp.OpticalFlowUsed() ? 1.0 : 0.0);   // OpticalFlowUsed: the pair was the optical-flow input
						row(kind, name, n, "no_pair_ahead", MvNoPairAhead(warp, 0));
						if (mode == 1) row(kind, name, n, "pair_ms", pairMs);
					}
				};

				// ---- shift: exact ground truth (integer, real texture) - kept as the older runs had it
				static const int shifts[3][2] = { { 8, 2 }, { 16, 4 }, { 32, 8 } };
				const int* sh = shifts[p % 3];
				MvShift(lumaA, width, height, sh[0], sh[1], lumaB);
				MvFrame(lumaB, width, height, frameB);
				double pairMs = 0.0;
				{
					const MotionAhead::FlowPtr pair = producerPair(lumaA, lumaB, pairMs);
					runGroup("shift", frameB, pair, pairMs, nullptr, sh[0], sh[1], false, lumaMid, 0.0, 0.0);
				}
				// ---- subpx: a fractional global shift
				{
					static const double fractional[3][2] = { { 3.5, 1.25 }, { 9.25, 2.5 }, { 17.75, -3.25 } };
					MvDisplacement d;
					d.tx = fractional[p % 3][0]; d.ty = fractional[p % 3][1]; d.width = width; d.height = height;
					MvWarpBilinear(lumaA, width, height, d, lumaB);
					MvFrame(lumaB, width, height, frameB);
					const MotionAhead::FlowPtr pair = producerPair(lumaA, lumaB, pairMs);
					runGroup("subpx", frameB, pair, pairMs, &d, 0.0, 0.0, false, lumaMid, 0.0, 0.0);
				}
				// ---- nonrigid: a smooth displacement field (translation, zoom, shear, ripple)
				{
					static const double params[3][6] = {   // tx, ty, zoom, shear, wave, period
						{ 4.3, 1.2, 3.0, 1.5, 1.5, 320.0 }, { 9.6, -2.4, -5.0, 2.5, 2.5, 256.0 }, { 1.7, 0.6, 8.0, -4.0, 3.0, 384.0 } };
					MvDisplacement d;
					d.tx = params[p % 3][0]; d.ty = params[p % 3][1]; d.zoom = params[p % 3][2]; d.shear = params[p % 3][3];
					d.wave = params[p % 3][4]; d.period = params[p % 3][5]; d.width = width; d.height = height;
					MvWarpBilinear(lumaA, width, height, d, lumaB);
					MvFrame(lumaB, width, height, frameB);
					const MotionAhead::FlowPtr pair = producerPair(lumaA, lumaB, pairMs);
					runGroup("nonrigid", frameB, pair, pairMs, &d, 0.0, 0.0, false, lumaMid, 0.0, 0.0);
				}
				// ---- interp: frame n+1 reconstructed from n and n+2
				MvFrame(lumaNext, width, height, frameNext);
				{
					const MotionAhead::FlowPtr pair = producerPair(lumaA, lumaNext, pairMs);
					double holdPsnr = 0.0, holdSsim = 0.0;
					MvQuality(lumaA, lumaMid, width, height, holdPsnr, holdSsim);
					runGroup("interp", frameNext, pair, pairMs, nullptr, 0.0, 0.0, true, lumaMid, holdPsnr, holdSsim);
				}
			}
			csv.close();
			Assert::IsTrue(ran > 0, L"the clip gave no frames");
			Logger::WriteMessage(("MotionUnificationBench: " + std::to_string(ran) + " positions written to " + outPath + "\n").c_str());
		}

		// Steady-state cost PER MODE (doc 83 / coordinator 2026-11-08), real time at the clip's own rate, for 1080p60 and 4K30 clips
		// (pick the clip: this test measures the size and rate it is given). Modes:
		//   a_steady_lane   queue >= lead: the producer (the lane) makes the pairs ahead; the render path runs the matcher WITH the
		//                   pair. render_ms = the matcher with a pair (GPU drained), lane_pair_ms = the NVOF pair on the producer.
		//   b_lowlatency    no look-ahead, backward vectors on the render path: the matcher WITHOUT a pair on (n-1, n) at once;
		//                   pair_latency_ms = how late the producer's pair (n-1, n) arrives after the frame was handed over.
		//   c_warm_only     the always-warm de-judder demand with nothing rendered: frame-pair products only (the producer alone).
		// Every mode runs the producer in real time (VP_MV_BENCH_STEADY_FRAMES frames, spaced 1 / fps); render_ms is 0 in mode c.
		// nvof_occupancy = the producer's summed pair time / the wall time; vram_*_mb = this process's video memory on the NVIDIA
		// adapter (DXGI QueryVideoMemoryInfo) after the producer's engine opened and after the matcher estimated, over the start.
		BEGIN_TEST_METHOD_ATTRIBUTE(SteadyStateCostPerMode)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SteadyStateCostPerMode)
		{
			const std::string clipPath = MvEnv("VP_MV_BENCH_CLIP"), outPath = MvEnv("VP_MV_BENCH_OUT");
			const int frames = MvEnvInt("VP_MV_BENCH_STEADY_FRAMES", 0), start = MvEnvInt("VP_MV_BENCH_START", 0);
			if (clipPath.empty() || outPath.empty() || frames <= 0)
			{
				Logger::WriteMessage("MotionUnificationBench steady: VP_MV_BENCH_CLIP / VP_MV_BENCH_OUT / VP_MV_BENCH_STEADY_FRAMES not set: not run\n");
				return;
			}
			CComPtr<IDXGIFactory1> factory;
			CComPtr<IDXGIAdapter1> nvidia;
			if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
				for (UINT i = 0;; ++i)
				{
					CComPtr<IDXGIAdapter1> adapter;
					if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
					DXGI_ADAPTER_DESC1 desc{};
					adapter->GetDesc1(&desc);
					if (desc.VendorId == 0x10DE) nvidia = adapter;
				}
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, "SteadyStateCostPerMode", nvidia != nullptr, "no NVIDIA adapter"))
				return;
			Y4mClip clip;
			std::string why;
			Assert::IsTrue(clip.Open(clipPath, why), std::wstring(why.begin(), why.end()).c_str());
			Assert::IsTrue(clip.fps > 1.0, L"the y4m has no F tag (frame rate): the steady run is paced at the clip's rate");
			const int width = clip.width, height = clip.height;
			const double periodMs = 1000.0 / clip.fps;
			const std::string label = MvEnv("VP_MV_BENCH_LABEL").empty() ? "run" : MvEnv("VP_MV_BENCH_LABEL");
			const std::string clipName = clipPath.substr(clipPath.find_last_of("\\/") == std::string::npos ? 0 : clipPath.find_last_of("\\/") + 1);

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			CComPtr<ID3D11Device> device;
			Assert::IsTrue(SUCCEEDED(D3D11CreateDevice(nvidia, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
			Assert::IsTrue(MvAdapterKey(device) != 0 && MvGpuKey(gpu) == MvAdapterKey(device),
				L"the libplacebo fixture device is not on the NVIDIA adapter the producer uses (VP_TEST_GPU=hardware, VP_TEST_ADAPTER=NVIDIA)");
			CComPtr<IDXGIAdapter3> adapter3;
			nvidia->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3));
			auto vramMb = [&]() {
				DXGI_QUERY_VIDEO_MEMORY_INFO info{};
				if (adapter3 && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
					return static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0);
				return -1.0;
			};

			std::ofstream csv(outPath + ".steady.csv", std::ios::trunc);
			Assert::IsTrue(static_cast<bool>(csv), L"cannot write the csv");
			csv << "label,clip,kind,mode,position,metric,value\n";
			auto row = [&](const char* mode, const char* metric, double value) {
				csv << label << ',' << clipName << ",steady," << mode << ",0," << metric << ',' << value << '\n';
			};

			struct Mode { const char* name; bool renderWithPair, renderNoPair; };
			const Mode modes[3] = { { "a_steady_lane", true, false }, { "b_lowlatency", false, true }, { "c_warm_only", false, false } };
			for (const Mode& mode : modes)
			{
				const double vramStart = vramMb();
				MotionAhead::Producer producer;
				Assert::IsTrue(producer.Start(device, NvOpticalFlow::Preset::Slow), L"producer started");
				std::vector<double> renderMs, pairMs, latencyMs;
				std::vector<uint8_t> older, prev, cur;
				// ONE matcher per mode (doc 93 W2-C5a): a fresh warp per frame measured shader compilation and texture allocation on every
				// frame, not the steady cost. Its first estimate is the cold figure (render_first_ms) and is not in the mean / p95.
				std::unique_ptr<Dejudder::MotionWarp> steadyWarp;
				double firstRenderMs = 0.0;
				bool firstRenderDone = false;
				double vramProducer = -1.0, vramRender = -1.0;
				unsigned long long pairsSeen = 0;
				const auto wallStart = std::chrono::steady_clock::now();
				auto nowMs = [&]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wallStart).count(); };
				int used = 0;
				for (int k = 0; k < frames; ++k)
				{
					if (!clip.Luma(start + k, cur)) break;
					++used;
					const double slotStart = static_cast<double>(k) * periodMs;
					while (nowMs() < slotStart) std::this_thread::sleep_for(std::chrono::milliseconds(1));
					const double handedOver = nowMs();
					producer.Submit(static_cast<uint64_t>(k + 1), 1, MvSource(cur, width, height));
					const uint64_t seq = static_cast<uint64_t>(k + 1);
					// The render path. Steady lane (a): the render is a frame behind the lane, so it uses the pair of the frames before this
					// one, (seq - 2, seq - 1), already made ahead. Low latency (b): the pair (seq - 1, seq) of the frame just handed over,
					// with no look-ahead, at once, matcher only.
					const bool haveFrames = mode.renderWithPair ? k > 1 : k > 0;
					if (haveFrames && (mode.renderWithPair || mode.renderNoPair))
					{
						DejudderFrame fa, fb;
						MvFrame(mode.renderWithPair ? older : prev, width, height, fa);
						MvFrame(mode.renderWithPair ? prev : cur, width, height, fb);
						DejudderGpuPlanes pa, pb;
						UploadDejudderFrame(gpu, fa, pa);
						UploadDejudderFrame(gpu, fb, pb);
						if (!steadyWarp)
						{
							Dejudder::Settings settings = DejudderTestSettings();
							settings.opticalFlow = true;
							settings.opticalFlowCandidate = true;
							settings.opticalFlowSeed = true;
							settings.globalPrior = true;
							settings.estCandidates = true;
							settings.globalTwo = true;
							settings.opticalFlowGlobal = true;
							settings.hostReadable = false;
							steadyWarp.reset(new Dejudder::MotionWarp(fixture.Log(), gpu, settings));
						}
						Dejudder::MotionWarp& warp = *steadyWarp;
						MotionAhead::FlowPtr flow;
						if (mode.renderWithPair)
						{
							for (int w = 0; w < 400 && !flow; ++w)
							{
								flow = producer.Take(seq - 2, seq - 1);
								if (!flow) std::this_thread::sleep_for(std::chrono::milliseconds(1));
							}
							if (flow) warp.SetExternalFlow(flow);
						}
						pl_gpu_finish(gpu);   // drained before the clock starts: the upload's queued work is not this estimate's
						const auto t0 = std::chrono::steady_clock::now();
						const bool ok = warp.Estimate(pa.planes, pb.planes);
						pl_gpu_finish(gpu);
						const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
						if (!firstRenderDone)
						{
							firstRenderDone = true;
							firstRenderMs = ms;
						}
						else if (ok && (!mode.renderWithPair || warp.ExternalFlowUsed()))
							renderMs.push_back(ms);
						vramRender = vramMb();
					}
					// The lane: poll the producer for the pair (seq - 1, seq) and its time; its lateness after the hand-over.
					for (int w = 0; w < static_cast<int>(3 * periodMs) + 5; ++w)
					{
						const auto st = producer.GetStats();
						if (st.pairs > pairsSeen) { pairsSeen = st.pairs; pairMs.push_back(st.lastPairMs); }
						if (k > 0 && producer.Take(seq - 1, seq)) { latencyMs.push_back(nowMs() - handedOver); break; }
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
					if (vramProducer < 0.0 && producer.EngineReady()) vramProducer = vramMb();
					older.swap(prev);
					prev.swap(cur);
				}
				const double wallMs = nowMs();
				producer.Stop();
				auto avg = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0.0 : s / v.size(); };
				auto p95 = [](std::vector<double> v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[static_cast<size_t>(0.95 * (v.size() - 1))]; };
				double pairSum = 0.0;
				for (double x : pairMs) pairSum += x;
				row(mode.name, "width", width);
				row(mode.name, "height", height);
				row(mode.name, "fps", clip.fps);
				row(mode.name, "frames", used);
				row(mode.name, "render_ms_mean", avg(renderMs));
				row(mode.name, "render_ms_p95", p95(renderMs));
				row(mode.name, "render_estimates", static_cast<double>(renderMs.size()));
				row(mode.name, "render_first_ms", firstRenderMs);
				row(mode.name, "lane_pair_ms_mean", avg(pairMs));
				row(mode.name, "lane_pairs", static_cast<double>(pairMs.size()));
				row(mode.name, "nvof_occupancy", wallMs > 0.0 ? pairSum / wallMs : 0.0);
				row(mode.name, "pair_latency_ms_mean", avg(latencyMs));
				row(mode.name, "vram_lane_mb", vramProducer >= 0.0 && vramStart >= 0.0 ? vramProducer - vramStart : -1.0);
				row(mode.name, "vram_render_mb", vramRender >= 0.0 && vramProducer >= 0.0 ? vramRender - vramProducer : -1.0);
				{
					// The engine ring's textures by the formats' sizes: two sets of the half-size input (R8) + flow (4 B) + cost (1 B) per 4 x 4 cell.
					const double hw = (width + 1) / 2, hh = (height + 1) / 2, cells = std::ceil(hw / 4.0) * std::ceil(hh / 4.0);
					row(mode.name, "ring_bytes_mb", 2.0 * (hw * hh + cells * 5.0) / (1024.0 * 1024.0));
				}
			}
			csv.close();
			Logger::WriteMessage(("MotionUnificationBench steady: written to " + outPath + ".steady.csv\n").c_str());
		}
		// W2-C5b commit 6 (owner answer 4: measure the latency impact AND the benefit; the owner decides the default): the on-demand
		// path end to end at the clip's own rate, through the REAL pieces - a live Producer (one NVOF session), the VectorEngine, the
		// dequeue hook (MotionAhead::IssueOnDemandAtDequeue, timed as hook_us: it runs under the queue mutex in the renderer; the post after the block as post_us) and the estimate's fill
		// (VectorEngine::OnDemandFill, timed: the render thread waits there) - with the real matcher on the render path (with the pair when
		// one came, else the block searches). Every VP_MV_BENCH_DROP_EVERY-th frame (default 25) is dropped, so hiccup gap pairs occur.
		// Modes (one fresh engine + producer each; the first VP_MV_BENCH_ONDEMAND_WARM cycles, default 30, are not in the figures):
		//   std_off     standard: the walk submits every frame, the render is one frame behind; optical_flow_on_demand=off (today).
		//   std_on      the same with on: gap pairs and walk misses are asked of the session. The pacing wait (the swap chain / present
		//               wait in the renderer: PassTimers CpuWait) is NOT given to the hook, as the renderer does today: the render-time
		//               sample counts it as render time.
		//   std_on_xw   the same, the pacing wait given to the hook as wait (= excluding CpuWait from the render-time EWMA).
		//   ll_off      low latency with a queue ahead (capacity 1): no walk (depth 0), the render takes the newest pair at once; off.
		//   ll_on       the same with on: every pair (current + last frame, no look-ahead) is asked of the session at the dequeue (the
		//               hook's request carries both frames' luma; the bench's caches always hold it, so `uncached` is a renderer-only
		//               figure, see Ctrl+I).
		//   ll_competitive_on  low latency with no queue ahead (capacity 0) and on: competitive stays motion-free (nothing asked).
		// The wait until the next frame's slot is the dequeue wait in low latency (the frame has not arrived).
		// Figures (csv kind "ondemand"): the share of render pairs ahead / served / late / no budget / unavailable / none (nothing asked),
		// fill wait, matcher ms with and without a pair, render-thread ms (hand-over -> matcher done: the in-process latency proxy) p50 / p99,
		// the hook's time p50 / p99 / max (us), the budget and the measured render time (the engine's EWMA), the engine's counters.
		BEGIN_TEST_METHOD_ATTRIBUTE(OnDemandServedShareAndStallPerMode)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OnDemandServedShareAndStallPerMode)
		{
			const std::string clipPath = MvEnv("VP_MV_BENCH_CLIP"), outPath = MvEnv("VP_MV_BENCH_OUT");
			const int frames = MvEnvInt("VP_MV_BENCH_ONDEMAND_FRAMES", 0), start = MvEnvInt("VP_MV_BENCH_START", 0);
			const int dropEvery = std::max(2, MvEnvInt("VP_MV_BENCH_DROP_EVERY", 25)), warmCycles = std::max(0, MvEnvInt("VP_MV_BENCH_ONDEMAND_WARM", 30));
			if (clipPath.empty() || outPath.empty() || frames <= 0)
			{
				Logger::WriteMessage("MotionUnificationBench on demand: VP_MV_BENCH_CLIP / VP_MV_BENCH_OUT / VP_MV_BENCH_ONDEMAND_FRAMES not set: not run\n");
				return;
			}
			CComPtr<IDXGIFactory1> factory;
			CComPtr<IDXGIAdapter1> nvidia;
			if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
				for (UINT i = 0;; ++i)
				{
					CComPtr<IDXGIAdapter1> adapter;
					if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
					DXGI_ADAPTER_DESC1 desc{};
					adapter->GetDesc1(&desc);
					if (desc.VendorId == 0x10DE) nvidia = adapter;
				}
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, "OnDemandServedShareAndStallPerMode", nvidia != nullptr, "no NVIDIA adapter"))
				return;
			Y4mClip clip;
			std::string why;
			Assert::IsTrue(clip.Open(clipPath, why), std::wstring(why.begin(), why.end()).c_str());
			Assert::IsTrue(clip.fps > 1.0, L"the y4m has no F tag (frame rate): the run is paced at the clip's rate");
			const int width = clip.width, height = clip.height;
			const double periodMs = 1000.0 / clip.fps;
			const std::string label = MvEnv("VP_MV_BENCH_LABEL").empty() ? "run" : MvEnv("VP_MV_BENCH_LABEL");
			const std::string clipName = clipPath.substr(clipPath.find_last_of("\\/") == std::string::npos ? 0 : clipPath.find_last_of("\\/") + 1);

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			CComPtr<ID3D11Device> device;
			Assert::IsTrue(SUCCEEDED(D3D11CreateDevice(nvidia, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
			Assert::IsTrue(MvAdapterKey(device) != 0 && MvGpuKey(gpu) == MvAdapterKey(device),
				L"the libplacebo fixture device is not on the NVIDIA adapter the producer uses (VP_TEST_GPU=hardware, VP_TEST_ADAPTER=NVIDIA)");

			std::ofstream csv(outPath + ".ondemand.csv", std::ios::trunc);
			Assert::IsTrue(static_cast<bool>(csv), L"cannot write the csv");
			csv << "label,clip,kind,mode,position,metric,value\n";
			auto row = [&](const char* mode, const char* metric, double value) {
				csv << label << ',' << clipName << ",ondemand," << mode << ",0," << metric << ',' << value << '\n';
			};

			// The renderer's shapes the hook template reads (QueuedFrame::AnalysisPayload, the queued frame, the host's impl).
			struct BenchPrepared
			{
				AnalysisLumaSource source;
				bool IsValid() const { return source.IsValid(); }
			};
			struct BenchCache
			{
				std::mutex mutex;
				BenchPrepared queued;
			};
			struct BenchIdentity { uint64_t sourceFormatGeneration = 0; };
			struct BenchQueued
			{
				std::shared_ptr<BenchCache> analysis;
				BenchIdentity activePictureIdentity;
				uint64_t sourceSequence = 0;
			};
			struct BenchHost
			{
				VpMotion::VectorEngine vectorEngine;
				std::shared_ptr<MotionAhead::Producer>& motionProducer = vectorEngine.motionProducer;
				MotionValidity::State& motionValidity = vectorEngine.motionValidity;
				std::atomic<int>& queueAheadCapacity = vectorEngine.queueAheadCapacity;
				std::atomic<bool> lowLatencyQueueActive{ false };
			};
			struct Delivered
			{
				uint64_t seq = 0;
				std::shared_ptr<std::vector<uint8_t>> luma;
				std::shared_ptr<BenchCache> cache;
			};
			struct Mode { const char* name; bool lowLatency; int capacity; VpMotion::OnDemand::Setting setting; bool waitExcluded; };
			const Mode modes[] = {
				{ "std_off", false, 1, VpMotion::OnDemand::Setting::Off, false },
				{ "std_on", false, 1, VpMotion::OnDemand::Setting::On, false },
				{ "std_on_xw", false, 1, VpMotion::OnDemand::Setting::On, true },
				{ "ll_off", true, 1, VpMotion::OnDemand::Setting::Off, false },
				{ "ll_on", true, 1, VpMotion::OnDemand::Setting::On, false },
				{ "ll_competitive_on", true, 0, VpMotion::OnDemand::Setting::On, false } };
			auto pct = [](std::vector<double> v, double q) {
				if (v.empty()) return 0.0;
				std::sort(v.begin(), v.end());
				return v[static_cast<size_t>(q * (v.size() - 1))];
			};
			auto avg = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0.0 : s / v.size(); };
			Dejudder::Settings settings = DejudderTestSettings();
			settings.opticalFlow = true;
			settings.opticalFlowCandidate = true;
			settings.opticalFlowSeed = true;
			settings.globalPrior = true;
			settings.estCandidates = true;
			settings.globalTwo = true;
			settings.opticalFlowGlobal = true;
			settings.hostReadable = false;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);   // ONE matcher for every mode (doc 93 W2-C5a)
			for (const Mode& mode : modes)
			{
				using FallbackCatalog::NoPairCause;
				std::unique_ptr<BenchHost> host(new BenchHost());
				VpMotion::VectorEngine& engine = host->vectorEngine;
				engine.SetOnDemandSetting(mode.setting);
				host->lowLatencyQueueActive = mode.lowLatency;
				host->queueAheadCapacity = mode.capacity;
				auto producer = std::make_shared<MotionAhead::Producer>();
				producer->SetSlot(engine.p2Slot);   // as PlanProducer: the producer publishes into the engine's P2 slot
				Assert::IsTrue(producer->Start(device, NvOpticalFlow::Preset::Slow), L"producer started");
				engine.PublishProducer(producer);
				const uint64_t generation = host->motionValidity.StreamGeneration();
				std::vector<Delivered> delivered;
				unsigned long long counts[6] = {};   // ahead, served, late, no budget, unavailable, none
				unsigned long long gapPairs = 0, gapServed = 0;
				std::vector<double> fillMs, matcherPairMs, matcherNoPairMs, renderMs, hookUs, postUs, budgetMs;
				double waitBeforeMs = 0.0;
				int cycles = 0;
				const auto wallStart = std::chrono::steady_clock::now();
				auto nowMs = [&]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wallStart).count(); };
				for (int k = 0; k < frames; ++k)
				{
					const double slotStart = static_cast<double>(k) * periodMs;
					const double waitStart = nowMs();
					while (nowMs() < slotStart) std::this_thread::sleep_for(std::chrono::milliseconds(1));
					waitBeforeMs = nowMs() - waitStart;   // the pacing wait before this cycle (present in standard, frame arrival in low latency)
					if (k % dropEvery == dropEvery - 1)
						continue;   // dropped: never queued, never submitted (the next render pair spans it)
					Delivered entry;
					entry.seq = static_cast<uint64_t>(k + 1);
					entry.luma = std::make_shared<std::vector<uint8_t>>();
					if (!clip.Luma(start + k, *entry.luma)) break;
					entry.cache = std::make_shared<BenchCache>();
					entry.cache->queued.source = MvSource(*entry.luma, width, height);
					if (!mode.lowLatency)
						producer->Submit(entry.seq, generation, entry.cache->queued.source);   // the walk (depth > 0)
					delivered.push_back(entry);
					if (delivered.size() > 4) delivered.erase(delivered.begin());
					const size_t behind = mode.lowLatency ? 0 : 1;   // standard renders one frame behind the newest
					if (delivered.size() < 2 + behind)
						continue;
					const Delivered& from = delivered[delivered.size() - 2 - behind];
					const Delivered& to = delivered[delivered.size() - 1 - behind];
					const double handedOver = nowMs();
					// The dequeue: the hook under (what is in the renderer) the queue mutex.
					BenchQueued next;
					next.analysis = to.cache;
					next.sourceSequence = to.seq;
					const double dequeueWaitMs = mode.lowLatency || mode.waitExcluded ? waitBeforeMs : 0.0;
					const auto h0 = std::chrono::steady_clock::now();
					MotionAhead::IssueOnDemandAtDequeue(host.get(), from.cache, uint64_t(0), from.seq, next, clip.fps, dequeueWaitMs);
					const double hook = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - h0).count();
					const auto p0 = std::chrono::steady_clock::now();   // the second hook line: after the queue block (no queue mutex)
					MotionAhead::PostStagedOnDemand(host.get());
					const double post = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - p0).count();
					// The estimate: the pair computed ahead (no wait: the P2 read is a lookup), else the fill.
					MotionAhead::FlowPtr ahead = mode.lowLatency ? nullptr : producer->Take(from.seq, to.seq);
					NoPairCause cause = NoPairCause::None;
					const auto f0 = std::chrono::steady_clock::now();
					MotionAhead::FlowPtr flow = engine.OnDemandFill(ahead, from.seq, to.seq, true, cause);
					const double fill = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
					const bool served = !ahead && flow && engine.OnDemandServed(from.seq, to.seq);
					const bool gap = to.seq > from.seq + 1;
					DejudderFrame fa, fb;
					MvFrame(*from.luma, width, height, fa);
					MvFrame(*to.luma, width, height, fb);
					DejudderGpuPlanes pa, pb;
					UploadDejudderFrame(gpu, fa, pa);
					UploadDejudderFrame(gpu, fb, pb);
					warp.SetExternalFlow(flow);
					pl_gpu_finish(gpu);
					const auto m0 = std::chrono::steady_clock::now();
					const bool ok = warp.Estimate(pa.planes, pb.planes);
					pl_gpu_finish(gpu);
					const double matcher = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m0).count();
					const bool pairUsed = ok && flow && warp.ExternalFlowUsed();
					warp.SetExternalFlow(nullptr);
					const double render = nowMs() - handedOver;
					if (++cycles <= warmCycles)
						continue;
					const int category = ahead ? 0 : served ? 1 : cause == NoPairCause::OnDemandLate ? 2 : cause == NoPairCause::OnDemandNoBudget ? 3
						: cause == NoPairCause::OnDemandUnavailable ? 4 : 5;
					++counts[category];
					if (gap) { ++gapPairs; if (served) ++gapServed; }
					fillMs.push_back(fill);
					(pairUsed ? matcherPairMs : matcherNoPairMs).push_back(matcher);
					renderMs.push_back(render);
					hookUs.push_back(hook);
					postUs.push_back(post);
					budgetMs.push_back(engine.OnDemandBudgetMs(clip.fps));
				}
				const VpMotion::VectorEngine::OnDemandCounters c = engine.OnDemandReads();
				engine.PublishProducer(nullptr);
				producer->Stop();
				unsigned long long measured = 0;
				for (unsigned long long n : counts) measured += n;
				const double scale = measured ? 100.0 / static_cast<double>(measured) : 0.0;
				row(mode.name, "width", width);
				row(mode.name, "height", height);
				row(mode.name, "fps", clip.fps);
				row(mode.name, "render_pairs", static_cast<double>(measured));
				row(mode.name, "ahead_pct", counts[0] * scale);
				row(mode.name, "served_pct", counts[1] * scale);
				row(mode.name, "late_pct", counts[2] * scale);
				row(mode.name, "nobudget_pct", counts[3] * scale);
				row(mode.name, "unavailable_pct", counts[4] * scale);
				row(mode.name, "none_pct", counts[5] * scale);
				row(mode.name, "gap_pairs", static_cast<double>(gapPairs));
				row(mode.name, "gap_served", static_cast<double>(gapServed));
				row(mode.name, "fill_wait_ms_p50", pct(fillMs, 0.5));
				row(mode.name, "fill_wait_ms_p99", pct(fillMs, 0.99));
				row(mode.name, "matcher_pair_ms_mean", avg(matcherPairMs));
				row(mode.name, "matcher_pair_n", static_cast<double>(matcherPairMs.size()));
				row(mode.name, "matcher_nopair_ms_mean", avg(matcherNoPairMs));
				row(mode.name, "matcher_nopair_n", static_cast<double>(matcherNoPairMs.size()));
				row(mode.name, "render_thread_ms_p50", pct(renderMs, 0.5));
				row(mode.name, "render_thread_ms_p99", pct(renderMs, 0.99));
				row(mode.name, "hook_us_p50", pct(hookUs, 0.5));
				row(mode.name, "hook_us_p99", pct(hookUs, 0.99));
				row(mode.name, "hook_us_max", pct(hookUs, 1.0));
				row(mode.name, "post_us_p50", pct(postUs, 0.5));
				row(mode.name, "post_us_p99", pct(postUs, 0.99));
				row(mode.name, "post_us_max", pct(postUs, 1.0));
				row(mode.name, "budget_ms_mean", avg(budgetMs));
				row(mode.name, "engine_render_ms_ewma", c.renderMs);
				// Re-check MUST 2 caveat: 0 = the budget never adapted and stayed the fixed half period (RenderSample rejects intervals
				// beyond two source periods, and this bench renders slower than real time), so served / late in that mode do NOT test
				// "period minus the measured render time"; 1 = the measured EWMA set the budget.
				row(mode.name, "budget_adapted", c.renderMs > 0.0 ? 1.0 : 0.0);
				row(mode.name, "c_requests", static_cast<double>(c.requests));
				row(mode.name, "c_issued", static_cast<double>(c.issued));
				row(mode.name, "c_served", static_cast<double>(c.served));
				row(mode.name, "c_late", static_cast<double>(c.late));
				row(mode.name, "c_nobudget", static_cast<double>(c.noBudget));
				row(mode.name, "c_unavailable", static_cast<double>(c.unavailable));
				row(mode.name, "c_unclaimed", static_cast<double>(c.unclaimed));
				row(mode.name, "c_ahead_covered", static_cast<double>(c.aheadCovered));
				row(mode.name, "c_gap_served", static_cast<double>(c.gapServed));
				row(mode.name, "c_idle_skips", static_cast<double>(c.idleSkips));
				row(mode.name, "c_uncached", static_cast<double>(c.uncached));
			}
			csv.close();
			Logger::WriteMessage(("MotionUnificationBench on demand: written to " + outPath + ".ondemand.csv\n").c_str());
		}
	};
}
