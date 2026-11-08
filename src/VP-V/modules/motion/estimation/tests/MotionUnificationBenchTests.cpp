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
// Per position n of the clip, two measurements:
//   shift   frame n as A, B = A moved by a known (dx, dy) (real texture, exact ground truth): the trusted blocks' vector error
//           (source px) and the trusted share.
//   interp  frames n and n + 2 as A and B, the picture halfway (Warp at 0.5) against the real frame n + 1: luma PSNR and SSIM
//           (60p material), plus the same for holding A (the floor the warp must beat).
// Each in the two modes. Cost columns: the producer's pair time (lastPairMs, the NVOF ms of the pair mode) and the wall time of
// the Estimate call (mode nopair before = includes the render-time engine run).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <dxgi1_2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
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

			std::vector<uint8_t> lumaA, lumaB, lumaMid, lumaNext;
			int ran = 0;
			for (int p = 0; p < pairs; ++p)
			{
				const int n = start + p * step;
				if (!clip.Luma(n, lumaA) || !clip.Luma(n + 1, lumaMid) || !clip.Luma(n + 2, lumaNext)) break;
				++ran;
				DejudderFrame frameA, frameB, frameNext;
				MvFrame(lumaA, width, height, frameA);
				// ---- shift: exact ground truth on the real texture
				static const int shifts[3][2] = { { 8, 2 }, { 16, 4 }, { 32, 8 } };
				const int* sh = shifts[p % 3];
				MvShift(lumaA, width, height, sh[0], sh[1], lumaB);
				MvFrame(lumaB, width, height, frameB);
				double pairMs = 0.0;
				const MotionAhead::FlowPtr shiftPair = producerPair(lumaA, lumaB, pairMs);
				const double shiftPairMs = pairMs;
				for (int mode = 0; mode < 2; ++mode)
				{
					const char* name = mode == 0 ? "nopair" : "pair";
					if (mode == 1 && !shiftPair) { row("shift", name, n, "no_pair", 1.0); continue; }
					DejudderGpuPlanes pa, pb;
					UploadDejudderFrame(gpu, frameA, pa);
					UploadDejudderFrame(gpu, frameB, pb);
					Dejudder::MotionWarp warp(fixture.Log(), gpu, settingsFor());
					if (mode == 1)
						warp.SetExternalFlow(shiftPair);
					const auto t0 = std::chrono::steady_clock::now();
					const bool ok = warp.Estimate(pa.planes, pb.planes);
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
					if (!ok) { row("shift", name, n, "estimate_failed", 1.0); continue; }
					const DejudderField field = DownloadField(gpu, warp);
					double sum = 0.0;
					long trusted = 0, total = 0;
					for (int by = 2; by < field.blocksY - 2; ++by)
						for (int bx = 2; bx < field.blocksX - 2; ++bx)
						{
							++total;
							if (!field.trusted(bx, by)) continue;
							sum += std::hypot(field.vx(bx, by) - sh[0], field.vy(bx, by) - sh[1]);
							++trusted;
						}
					row("shift", name, n, "epe_px", trusted ? sum / trusted : -1.0);
					row("shift", name, n, "trusted_share", total ? static_cast<double>(trusted) / total : 0.0);
					row("shift", name, n, "estimate_ms", ms);
					row("shift", name, n, "used_optical_flow_pair", warp.ExternalFlowUsed() ? 1.0 : 0.0);
					if (mode == 1) row("shift", name, n, "pair_ms", shiftPairMs);
				}
				// ---- interp: frame n+1 reconstructed from n and n+2
				MvFrame(lumaNext, width, height, frameNext);
				const MotionAhead::FlowPtr interpPair = producerPair(lumaA, lumaNext, pairMs);
				const double interpPairMs = pairMs;
				double holdPsnr = 0.0, holdSsim = 0.0;
				MvQuality(lumaA, lumaMid, width, height, holdPsnr, holdSsim);
				for (int mode = 0; mode < 2; ++mode)
				{
					const char* name = mode == 0 ? "nopair" : "pair";
					if (mode == 1 && !interpPair) { row("interp", name, n, "no_pair", 1.0); continue; }
					DejudderGpuPlanes pa, pb;
					UploadDejudderFrame(gpu, frameA, pa);
					UploadDejudderFrame(gpu, frameNext, pb);
					Dejudder::MotionWarp warp(fixture.Log(), gpu, settingsFor());
					if (mode == 1)
						warp.SetExternalFlow(interpPair);
					const auto t0 = std::chrono::steady_clock::now();
					const bool ok = warp.Estimate(pa.planes, pb.planes);
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
					Dejudder::Planes out;
					if (!ok || !warp.Warp(pa.planes, 0.5f, out)) { row("interp", name, n, "estimate_failed", 1.0); continue; }
					const std::vector<uint8_t> mid = MvLuma8(DownloadPlane(gpu, out.luma));
					double psnr = 0.0, ssim = 0.0;
					MvQuality(mid, lumaMid, width, height, psnr, ssim);
					row("interp", name, n, "psnr_db", psnr);
					row("interp", name, n, "ssim", ssim);
					row("interp", name, n, "hold_psnr_db", holdPsnr);
					row("interp", name, n, "hold_ssim", holdSsim);
					row("interp", name, n, "estimate_ms", ms);
					row("interp", name, n, "used_optical_flow_pair", warp.ExternalFlowUsed() ? 1.0 : 0.0);
					if (mode == 1) row("interp", name, n, "pair_ms", interpPairMs);
				}
			}
			csv.close();
			Assert::IsTrue(ran > 0, L"the clip gave no frames");
			Logger::WriteMessage(("MotionUnificationBench: " + std::to_string(ran) + " positions written to " + outPath + "\n").c_str());
		}
	};
}
