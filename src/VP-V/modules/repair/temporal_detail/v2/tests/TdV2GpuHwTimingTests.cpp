// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-U6 Real NVIDIA GeForce RTX 3050 Ti GPU Timing & VRAM Harness
// (2026-11-25_td-v2-upgrade-plan-FABLE52-draft.md Phase 5 / Fable H-6 / BUILD.READY D6-D7):
//
// Opt-in hardware benchmark (`Category=Bench`, `VP_TEST_BENCH=1`, `VP_TEST_ADAPTER=NVIDIA`) —
// NEVER included in default CPU or default `--gpu` unit counts.
//   - Explicitly enumerates DXGI adapters, binds NVIDIA hardware D3D11 device (`allow_software=false`),
//     and ASSERTS `VendorId == 0x10DE`, `!d3d11->software`, and `"NVIDIA"` + `"3050 Ti"` in adapter description
//     (failing closed if it ever binds Intel iGPU or Microsoft WARP!).
//   - Measures real per-pass D3D11 hardware timestamp ms (via `pl_dispatch_callback`) AND end-to-end
//     `pl_gpu_finish`-bracketed frame GPU ms (p50 / p95 over 20 warmup + 200 timed frames, ABAB repeats)
//     plus actual ScratchArena + persistent + DXGI process VRAM at 1920x1080 and 3840x2160 across:
//       * `Preset::RealTime` and `Preset::Reference` for `K = 1, 2, 3`
//       * Chroma `4:2:0` and `4:2:2`
//       * Plan `6.6` `chromaReuseLumaWeights` OFF vs ON (ABAB interleaved repeats)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <dxgi1_4.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ChromaGpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2GpuLateCarry.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
namespace TdV2GpuHwTimingNs
{
	using namespace TdV2;
	using namespace TdV2::Gpu;

	static std::string ReadEnvString(const char* name)
	{
		char* buf = nullptr;
		size_t len = 0;
		std::string s;
		if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr)
			s = buf;
		std::free(buf);
		return s;
	}

	static int ReadEnvIntClamped(const char* name, int defVal, int minVal, int maxVal)
	{
		const std::string raw = ReadEnvString(name);
		if (raw.empty())
			return defVal;
		char* endPtr = nullptr;
		const long v = std::strtol(raw.c_str(), &endPtr, 10);
		if (endPtr == raw.c_str())
			return defVal;
		return (std::max)(minVal, (std::min)(maxVal, static_cast<int>(v)));
	}

	static std::string ToLowerAscii(std::string s)
	{
		for (char& c : s)
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
		return s;
	}

	static bool ContainsCaseInsensitive(const std::string& hay, const std::string& needle)
	{
		return ToLowerAscii(hay).find(ToLowerAscii(needle)) != std::string::npos;
	}

	struct NvidiaD3d11Context
	{
		pl_log log = nullptr;
		IDXGIAdapter1* boundAdapter = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		std::string adapterDescription;
		uint32_t vendorId = 0;
		uint32_t deviceId = 0;
		uint64_t dedicatedVramBytes = 0;

		void InitStrictNvidia3050Ti()
		{
			const std::string reqAdapter = ReadEnvString("VP_TEST_ADAPTER");
			Assert::IsTrue(
				ContainsCaseInsensitive(reqAdapter, "NVIDIA"),
				L"TD-U6 requires VP_TEST_ADAPTER=NVIDIA so hardware timing never runs on an unintended GPU");

			IDXGIFactory1* factory = nullptr;
			const HRESULT hrFac = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
			Assert::IsTrue(SUCCEEDED(hrFac) && factory != nullptr, L"CreateDXGIFactory1 failed");

			IDXGIAdapter1* chosen = nullptr;
			for (UINT idx = 0;; ++idx)
			{
				IDXGIAdapter1* cand = nullptr;
				if (factory->EnumAdapters1(idx, &cand) == DXGI_ERROR_NOT_FOUND)
					break;
				DXGI_ADAPTER_DESC1 desc{};
				if (SUCCEEDED(cand->GetDesc1(&desc)))
				{
					char utf8Name[256] = {};
					WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8Name, sizeof(utf8Name) - 1, nullptr, nullptr);
					const bool isSw = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
					                  (desc.VendorId == 0x1414 && desc.DeviceId == 0x8C);
					if (!isSw && desc.VendorId == 0x10DE && ContainsCaseInsensitive(utf8Name, reqAdapter))
					{
						chosen = cand;
						break;
					}
				}
				cand->Release();
			}
			factory->Release();
			Assert::IsNotNull(chosen, L"No non-software NVIDIA DXGI adapter (VendorId 0x10DE) found on this machine");
			boundAdapter = chosen;

			pl_log_params lp = pl_log_default_params;
			lp.log_level = PL_LOG_ERR;
			log = pl_log_create(PL_API_VER, &lp);
			Assert::IsNotNull(log, L"pl_log_create failed");

			pl_d3d11_params dp = pl_d3d11_default_params;
			dp.adapter = boundAdapter;
			dp.allow_software = false;
			dp.force_software = false;
			dp.min_feature_level = D3D_FEATURE_LEVEL_11_0;
			d3d11 = pl_d3d11_create(log, &dp);
			Assert::IsNotNull(d3d11, L"pl_d3d11_create on NVIDIA hardware adapter failed");
			Assert::IsFalse(d3d11->software, L"TD-U6 bound software/WARP adapter instead of hardware GPU");
			Assert::IsNotNull(d3d11->device, L"pl_d3d11->device is null");
			Assert::IsNotNull(d3d11->gpu, L"pl_d3d11->gpu is null");
			gpu = d3d11->gpu;

			// Re-verify the ACTUAL bound ID3D11Device's underlying DXGI adapter (fail-closed!).
			IDXGIDevice* dxgiDev = nullptr;
			Assert::IsTrue(SUCCEEDED(d3d11->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) && dxgiDev != nullptr);
			IDXGIAdapter* rawAd = nullptr;
			Assert::IsTrue(SUCCEEDED(dxgiDev->GetAdapter(&rawAd)) && rawAd != nullptr);
			IDXGIAdapter1* verAd1 = nullptr;
			Assert::IsTrue(SUCCEEDED(rawAd->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&verAd1))) && verAd1 != nullptr);
			DXGI_ADAPTER_DESC1 verDesc{};
			Assert::IsTrue(SUCCEEDED(verAd1->GetDesc1(&verDesc)));
			verAd1->Release();
			rawAd->Release();
			dxgiDev->Release();

			char nameBuf[256] = {};
			WideCharToMultiByte(CP_UTF8, 0, verDesc.Description, -1, nameBuf, sizeof(nameBuf) - 1, nullptr, nullptr);
			adapterDescription = nameBuf;
			vendorId = verDesc.VendorId;
			deviceId = verDesc.DeviceId;
			dedicatedVramBytes = static_cast<uint64_t>(verDesc.DedicatedVideoMemory);

			Assert::IsTrue((verDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0, L"Bound D3D11 device has DXGI_ADAPTER_FLAG_SOFTWARE set");
			Assert::AreEqual(0x10DEu, vendorId, L"Bound D3D11 device VendorId is not NVIDIA (0x10DE)");
			Assert::IsTrue(
				ContainsCaseInsensitive(adapterDescription, "NVIDIA") &&
				ContainsCaseInsensitive(adapterDescription, "3050 Ti"),
				L"Bound D3D11 adapter description must contain 'NVIDIA' and '3050 Ti'");

			dispatch = pl_dispatch_create(log, gpu);
			Assert::IsNotNull(dispatch, L"pl_dispatch_create failed on NVIDIA GPU");
		}

		uint64_t QueryProcessLocalVramUsageBytes() const
		{
			if (boundAdapter == nullptr)
				return 0;
			IDXGIAdapter3* ad3 = nullptr;
			if (FAILED(boundAdapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&ad3))) || ad3 == nullptr)
				return 0;
			DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			uint64_t usage = 0;
			if (SUCCEEDED(ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
				usage = info.CurrentUsage;
			ad3->Release();
			return usage;
		}

		~NvidiaD3d11Context()
		{
			if (dispatch != nullptr)
				pl_dispatch_destroy(&dispatch);
			if (d3d11 != nullptr)
				pl_d3d11_destroy(&d3d11);
			if (boundAdapter != nullptr)
			{
				boundAdapter->Release();
				boundAdapter = nullptr;
			}
			if (log != nullptr)
				pl_log_destroy(&log);
		}
	};

	struct DispatchPassCollector
	{
		bool recording = false;
		double frameKernelSumMs = 0.0;
		int frameDispatches = 0;
		std::vector<double> slotAccumulatorMs;
		std::vector<std::string> slotNames;
		std::vector<std::vector<double>> perSlotSamplesMs;

		void ResetAll()
		{
			recording = false;
			frameKernelSumMs = 0.0;
			frameDispatches = 0;
			slotAccumulatorMs.clear();
			slotNames.clear();
			perSlotSamplesMs.clear();
		}

		void BeginFrame(bool rec)
		{
			recording = rec;
			frameKernelSumMs = 0.0;
			frameDispatches = 0;
			slotAccumulatorMs.assign(slotNames.size(), 0.0);
		}

		void EndFrame()
		{
			if (!recording)
				return;
			if (perSlotSamplesMs.size() < slotAccumulatorMs.size())
				perSlotSamplesMs.resize(slotAccumulatorMs.size());
			for (size_t i = 0; i < slotAccumulatorMs.size(); ++i)
				perSlotSamplesMs[i].push_back(slotAccumulatorMs[i]);
		}

		static void Callback(void* priv, const pl_dispatch_info* info)
		{
			auto* self = static_cast<DispatchPassCollector*>(priv);
			if (self == nullptr || info == nullptr)
				return;
			const size_t idx = static_cast<size_t>(self->frameDispatches++);
			const double ms = static_cast<double>(info->last) / 1.0e6;
			self->frameKernelSumMs += ms;
			if (idx >= self->slotNames.size())
			{
				const char* desc = (info->shader && info->shader->description) ? info->shader->description : "compute_pass";
				self->slotNames.push_back(std::string("#") + std::to_string(idx) + ":" + desc);
				self->slotAccumulatorMs.resize(self->slotNames.size(), 0.0);
			}
			if (idx < self->slotAccumulatorMs.size())
				self->slotAccumulatorMs[idx] += ms;
		}
	};

	struct PercentilePair
	{
		double p50 = 0.0;
		double p95 = 0.0;
		double mean = 0.0;
	};

	static PercentilePair ComputePercentiles(std::vector<double> v)
	{
		PercentilePair out;
		if (v.empty())
			return out;
		std::sort(v.begin(), v.end());
		double sum = 0.0;
		for (double x : v) sum += x;
		out.mean = sum / static_cast<double>(v.size());
		out.p50 = v[v.size() / 2u];
		const size_t i95 = (std::min)(v.size() - 1u, static_cast<size_t>(std::floor(0.95 * static_cast<double>(v.size() - 1u))));
		out.p95 = v[i95];
		return out;
	}

	static pl_tex CreateGpuTexFilled(pl_gpu gpu, VpSkeleton::TexFormat fmt, int w, int h, bool storable, uint32_t seed)
	{
		pl_tex_params tp{};
		tp.w = w;
		tp.h = h;
		tp.format = VpSkeleton::PlTexBackend::FindFormat(gpu, fmt);
		tp.sampleable = true;
		tp.storable = storable;
		tp.host_writable = true;
		tp.host_readable = false;
		Assert::IsNotNull(tp.format, L"Required FP32/R16 texture format missing on NVIDIA GPU");
		pl_tex tex = pl_tex_create(gpu, &tp);
		Assert::IsNotNull(tex, L"pl_tex_create failed on NVIDIA GPU");

		const int comps = tp.format->num_components;
		std::vector<float> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(comps), 0.0f);
		for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i)
		{
			seed = seed * 1664525u + 1013904223u;
			const int x = static_cast<int>(i % static_cast<size_t>(w));
			const int y = static_cast<int>(i / static_cast<size_t>(w));
			const float base = 240.0f + 320.0f * (static_cast<float>(x) / static_cast<float>((std::max)(1, w))) +
			                   80.0f * std::sin(0.05f * static_cast<float>(x) + 0.03f * static_cast<float>(y));
			const float jitter = static_cast<float>(static_cast<int>((seed >> 16) % 17u) - 8);
			buf[i * comps + 0] = base + jitter;
			if (comps >= 2) buf[i * comps + 1] = 512.0f + 40.0f * std::cos(0.04f * static_cast<float>(x)) + 0.5f * jitter;
			if (comps >= 3) buf[i * comps + 2] = 1.0f;
			if (comps >= 4) buf[i * comps + 3] = 0.1f;
		}
		pl_tex_transfer_params xfer{};
		xfer.tex = tex;
		xfer.ptr = buf.data();
		Assert::IsTrue(pl_tex_upload(gpu, &xfer), L"pl_tex_upload failed on NVIDIA GPU");
		return tex;
	}

	static void BuildPrepackedMotionAndPriorTextures(
		pl_gpu gpu, int cw, int ch, pl_tex& outMotionVec, pl_tex& outMotionMeta, pl_tex& outCellPrior)
	{
		pl_tex_params tpMv{};
		tpMv.w = 3 * cw; tpMv.h = ch;
		tpMv.format = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
		tpMv.sampleable = true; tpMv.host_writable = true;
		outMotionVec = pl_tex_create(gpu, &tpMv);
		outMotionMeta = pl_tex_create(gpu, &tpMv);

		pl_tex_params tpCp{};
		tpCp.w = cw; tpCp.h = ch;
		tpCp.format = tpMv.format;
		tpCp.sampleable = true; tpCp.host_writable = true;
		outCellPrior = pl_tex_create(gpu, &tpCp);
		Assert::IsTrue(outMotionVec && outMotionMeta && outCellPrior, L"Failed to allocate prepacked motion/prior textures");

		std::vector<float> mvBuf(static_cast<size_t>(3 * cw) * ch * 4u, 0.0f);
		std::vector<float> mmBuf(static_cast<size_t>(3 * cw) * ch * 4u, 0.0f);
		for (int slot = 0; slot < 3; ++slot)
		{
			const float sign = (slot == 1) ? -1.0f : (slot == 0 ? 1.0f : 2.0f);
			for (int cy = 0; cy < ch; ++cy)
			{
				for (int cx = 0; cx < cw; ++cx)
				{
					const size_t dst = (static_cast<size_t>(cy) * (3 * cw) + static_cast<size_t>(slot * cw + cx)) * 4u;
					mvBuf[dst + 0] = sign * 0.35f;
					mvBuf[dst + 1] = sign * -0.20f;
					mvBuf[dst + 2] = sign * 0.35f;
					mvBuf[dst + 3] = sign * -0.20f;
					mmBuf[dst + 0] = static_cast<float>(static_cast<int>(RegionMotionClass::Camera));
					mmBuf[dst + 1] = 1.0f; // valid
					mmBuf[dst + 2] = 0.95f; // confidence
					mmBuf[dst + 3] = 0.15f; // fbErrorPx <= 1.0
				}
			}
		}
		std::vector<float> cpBuf(static_cast<size_t>(cw) * ch * 4u, 0.0f);
		for (size_t i = 0; i < static_cast<size_t>(cw) * ch; ++i)
		{
			cpBuf[i * 4u + 0u] = 0.6f; // retainedGrainVar
			cpBuf[i * 4u + 1u] = 0.0f; // focusBits
			cpBuf[i * 4u + 2u] = 1.0f; // stepScale
			cpBuf[i * 4u + 3u] = 0.6f; // Cr grain
		}
		pl_tex_transfer_params x1{}; x1.tex = outMotionVec; x1.ptr = mvBuf.data();
		pl_tex_transfer_params x2{}; x2.tex = outMotionMeta; x2.ptr = mmBuf.data();
		pl_tex_transfer_params x3{}; x3.tex = outCellPrior; x3.ptr = cpBuf.data();
		Assert::IsTrue(pl_tex_upload(gpu, &x1) && pl_tex_upload(gpu, &x2) && pl_tex_upload(gpu, &x3), L"Prepacked upload failed");
	}
} // namespace TdV2GpuHwTimingNs

TEST_CLASS(TdV2GpuHwTimingTests)
{
public:
	BEGIN_TEST_METHOD_ATTRIBUTE(NvidiaRtx3050TiPassAndFrameTimings)
		TEST_METHOD_ATTRIBUTE(L"Category", L"Bench")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(NvidiaRtx3050TiPassAndFrameTimings)
	{
		using namespace TdV2GpuHwTimingNs;
		const bool hwOptIn =
			HardwarePrerequisites::BenchOptedIn() ||
			(ContainsCaseInsensitive(ReadEnvString("VP_TEST_GPU"), "hardware") &&
			 ContainsCaseInsensitive(ReadEnvString("VP_TEST_ADAPTER"), "NVIDIA"));
		if (!HardwarePrerequisites::RequireOrSkip(
				HardwarePrerequisites::Kind::Bench,
				"TdV2GpuHwTimingTests::NvidiaRtx3050TiPassAndFrameTimings",
				hwOptIn,
				"set VP_TEST_BENCH=1 (or VP_TEST_GPU=hardware) and VP_TEST_ADAPTER=NVIDIA"))
		{
			return;
		}

		NvidiaD3d11Context ctx;
		ctx.InitStrictNvidia3050Ti();

		const int nWarmup = ReadEnvIntClamped("VP_TD_HW_WARMUP", 20, 5, 100);
		const int nTimed = ReadEnvIntClamped("VP_TD_HW_FRAMES", 200, 40, 600);

		DispatchPassCollector collector;
		pl_dispatch_callback(ctx.dispatch, &collector, &DispatchPassCollector::Callback);

		std::ostringstream outLog;
		char hdrLine[512];
		std::snprintf(
			hdrLine, sizeof(hdrLine),
			"\n=== [TD-U6] REAL HARDWARE GPU TIMING & VRAM REPORT ===\n"
			"Bound D3D11 Hardware Adapter: %s (Vendor=0x%04X, Device=0x%04X, DedicatedVRAM=%.1f MiB, Warmup=%d, Timed=%d)\n",
			ctx.adapterDescription.c_str(),
			ctx.vendorId,
			ctx.deviceId,
			static_cast<double>(ctx.dedicatedVramBytes) / (1024.0 * 1024.0),
			nWarmup,
			nTimed);
		outLog << hdrLine;

		outLog << "\n### 1. Luma Zero-Copy GPU Timings (1080p & 4K, RealTime vs Reference, K = 1, 2, 3)\n"
		       << "| Resolution | Preset | K | Dispatches | KernelSum p50 (ms) | KernelSum p95 (ms) | SyncedWall p50 (ms) | SyncedWall p95 (ms) | Arena MiB | Persist MiB | DXGI Process MiB |\n"
		       << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";

		std::ostringstream passBreakdownLog;
		passBreakdownLog << "\n### 2. Representative 4K Per-Pass D3D11 Timestamp Breakdown (K=2)\n"
		                 << "| Preset | Pass Slot | Pass p50 (ms) | Pass p95 (ms) |\n"
		                 << "|---|---|---:|---:|\n";

		std::ostringstream chromaLog;
		chromaLog << "\n### 3. Chroma GPU Timings & Plan 6.6 `chromaReuseLumaWeights` ABAB Comparison (1080p & 4K)\n"
		          << "| Resolution | Format | Preset | ReuseLuma (ABAB) | Dispatches | KernelSum p50 (ms) | KernelSum p95 (ms) | SyncedWall p50 (ms) | SyncedWall p95 (ms) | Chroma Arena MiB | Chroma Persist MiB |\n"
		          << "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|\n";

		const int resolutions[2][2] = { { 1920, 1080 }, { 3840, 2160 } };

		for (const auto& res : resolutions)
		{
			const int w = res[0];
			const int h = res[1];
			const char* resName = (w == 3840) ? "3840x2160" : "1920x1080";
			const int cw = (w + kCanonicalBlockPx - 1) / kCanonicalBlockPx;
			const int ch = (h + kCanonicalBlockPx - 1) / kCanonicalBlockPx;

			pl_tex recY = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::R32F, w, h, false, 101u);
			pl_tex cleanY = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::R32F, w, h, false, 102u);
			pl_tex prevPeakY = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::R32F, w, h, false, 103u);
			pl_tex nextPeakY = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::R32F, w, h, false, 104u);
			pl_tex outY = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::R32F, w, h, true, 105u);
			pl_tex outClean = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::R32F, w, h, true, 106u);
			pl_tex outDelta = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::RG32F, w, h, true, 107u);
			pl_tex outMaps = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::RGBA32F, cw, 3 * ch, true, 108u);
			pl_tex outLw = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::RGBA32F, 3 * cw, ch, true, 109u);
			pl_tex outDiagC = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::RGBA32F, 3 * cw, ch, true, 110u);
			pl_tex mvTex = nullptr, mmTex = nullptr, cpTex = nullptr;
			BuildPrepackedMotionAndPriorTextures(ctx.gpu, cw, ch, mvTex, mmTex, cpTex);

			for (Preset preset : { Preset::RealTime, Preset::Reference })
			{
				VpSkeleton::PlTexBackend backend(ctx.gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
				const auto reqs = TdV2GpuEngine::ArenaRequests(ctx.gpu, w, h, kCanonicalBlockPx, preset);
				const auto plan = VpSkeleton::PlanArena(reqs, VpSkeleton::Tier::T2);
				Assert::IsTrue(arena.Reserve(plan), L"Failed to allocate luma ScratchArena on NVIDIA GPU");

				for (int K : { 1, 2, 3 })
				{
					Config cfg = MakePreset(preset, Mode::On);
					cfg.enableAnchoredCarry = (K == 3);
					cfg.chromaReuseLumaWeights = true;
					TdV2GpuEngine eng(cfg);

					// Seed confirmed peak anchor at seq=0 and advance to age 2 when K == 3 so anchored carry serves slot 2.
					if (K == 3)
					{
						GpuFrameInput seedIn{};
						seedIn.seq = 0; seedIn.w = w; seedIn.h = h;
						seedIn.receivedYTex = recY; seedIn.cleanYTex = cleanY; seedIn.hasCleanSplit = true;
						seedIn.isConfirmedPeak = true;
						GpuFrameOutput seedOut{}; seedOut.outputYTex = outY;
						Assert::IsTrue(eng.ProcessFrameGpu(ctx.gpu, ctx.dispatch, seedIn, seedOut, &arena));

						GpuFrameInput step1In = seedIn;
						step1In.seq = 1; step1In.isConfirmedPeak = false;
						step1In.prevPeak.cleanTex = prevPeakY;
						step1In.motionVecTex = mvTex; step1In.motionMetaTex = mmTex; step1In.cellPriorTex = cpTex;
						GpuFrameOutput step1Out{}; step1Out.outputYTex = outY; step1Out.outputCleanTex = outClean;
						step1Out.tdDeltaTex = outDelta; step1Out.tdMapsTex = outMaps; step1Out.lumaCellWeightsTex = outLw;
						Assert::IsTrue(eng.ProcessFrameGpu(ctx.gpu, ctx.dispatch, step1In, step1Out, &arena));
					}

					collector.ResetAll();
					std::vector<double> kernelMs, wallMs;
					kernelMs.reserve(static_cast<size_t>(nTimed));
					wallMs.reserve(static_cast<size_t>(nTimed));
					int dispatchesObserved = 0;

					for (int f = -nWarmup; f < nTimed; ++f)
					{
						GpuFrameInput gIn{};
						gIn.seq = static_cast<uint64_t>(f + nWarmup + 10);
						gIn.w = w; gIn.h = h;
						gIn.receivedYTex = recY;
						gIn.cleanYTex = cleanY;
						gIn.hasCleanSplit = true;
						gIn.prevPeak.cleanTex = prevPeakY;
						if (K >= 2)
							gIn.nextPeak.cleanTex = nextPeakY;
						gIn.motionVecTex = mvTex;
						gIn.motionMetaTex = mmTex;
						gIn.cellPriorTex = cpTex;
						gIn.hasAnchorComposedMotion = (K == 3);
						gIn.frameRole = QualityPattern::FrameRole::NonReference;

						GpuFrameOutput gOut{};
						gOut.outputYTex = outY;
						gOut.outputCleanTex = outClean;
						gOut.tdDeltaTex = outDelta;
						gOut.tdMapsTex = outMaps;
						gOut.lumaCellWeightsTex = outLw;
						gOut.readbackCellDiagnostics = false;

						pl_dispatch_reset_frame(ctx.dispatch);
						pl_gpu_finish(ctx.gpu);
						collector.BeginFrame(f >= 0);
						const auto t0 = std::chrono::steady_clock::now();
						const bool ok = eng.ProcessFrameGpu(ctx.gpu, ctx.dispatch, gIn, gOut, &arena);
						pl_gpu_finish(ctx.gpu);
						const double wMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
						collector.EndFrame();
						Assert::IsTrue(ok && gOut.succeeded, L"TdV2GpuEngine::ProcessFrameGpu failed during timing loop");
						dispatchesObserved = gOut.dispatchCount;
						if (f >= 0)
						{
							kernelMs.push_back(collector.frameKernelSumMs);
							wallMs.push_back(wMs);
						}
					}

					const PercentilePair kp = ComputePercentiles(kernelMs);
					const PercentilePair wp = ComputePercentiles(wallMs);
					const double arenaMib = static_cast<double>(plan.bytes) / (1024.0 * 1024.0);
					const double persistMib = static_cast<double>(eng.AllocatedPersistentVramBytes()) / (1024.0 * 1024.0);
					const double dxgiMib = static_cast<double>(ctx.QueryProcessLocalVramUsageBytes()) / (1024.0 * 1024.0);

					char row[384];
					std::snprintf(
						row, sizeof(row),
						"| %s | %s | %d | %d | %.3f | %.3f | %.3f | %.3f | %.2f | %.2f | %.1f |\n",
						resName, PresetToken(preset), K, dispatchesObserved,
						kp.p50, kp.p95, wp.p50, wp.p95, arenaMib, persistMib, dxgiMib);
					outLog << row;

					if (w == 3840 && K == 2)
					{
						for (size_t s = 0; s < collector.slotNames.size() && s < collector.perSlotSamplesMs.size(); ++s)
						{
							const PercentilePair sp = ComputePercentiles(collector.perSlotSamplesMs[s]);
							char pRow[256];
							std::snprintf(pRow, sizeof(pRow), "| %s | %s | %.3f | %.3f |\n",
								PresetToken(preset), collector.slotNames[s].c_str(), sp.p50, sp.p95);
							passBreakdownLog << pRow;
						}
					}

					eng.Release(ctx.gpu);
				}
				arena.Release();
			}

			// Chroma 4:2:0 & 4:2:2 + ABAB interleaved repeats for Plan 6.6 `chromaReuseLumaWeights` OFF vs ON (K=2).
			for (DeliveredChromaFormat delivFmt : { DeliveredChromaFormat::Yuv420, DeliveredChromaFormat::Yuv422 })
			{
				const ChromaSubsampling codedSub =
					(delivFmt == DeliveredChromaFormat::Yuv420) ? ChromaSubsampling::Yuv420 : ChromaSubsampling::Yuv422;
				const char* fmtLabel = (delivFmt == DeliveredChromaFormat::Yuv420) ? "4:2:0" : "4:2:2";
				const int cW = w / 2;
				const int cH = (delivFmt == DeliveredChromaFormat::Yuv420) ? (h / 2) : h;

				pl_tex recUv = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::RG32F, cW, cH, false, 201u);
				pl_tex cleanUv = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::RG32F, cW, cH, false, 202u);
				pl_tex prevUv = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::RG32F, cW, cH, false, 203u);
				pl_tex nextUv = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::RG32F, cW, cH, false, 204u);
				pl_tex outCodedDelta = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::RG32F, cW, cH, true, 205u);
				pl_tex outDelivUv = CreateGpuTexFilled(ctx.gpu, VpSkeleton::TexFormat::RGBA32F, cW, cH, true, 206u);

				VpSkeleton::PlTexBackend cBackend(ctx.gpu);
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> cArena(cBackend);
				const auto cReqs = TdV2ChromaGpuEngine::ArenaRequests(ctx.gpu, w, h, codedSub, delivFmt, kCanonicalBlockPx);
				const auto cPlan = VpSkeleton::PlanArena(cReqs, VpSkeleton::Tier::T2);
				Assert::IsTrue(cArena.Reserve(cPlan), L"Failed to allocate chroma ScratchArena on NVIDIA GPU");

				Config cfgOff = MakePreset(Preset::RealTime, Mode::On);
				cfgOff.chromaReuseLumaWeights = false;
				Config cfgOn = cfgOff;
				cfgOn.chromaReuseLumaWeights = true;

				TdV2ChromaGpuEngine engOff(cfgOff);
				TdV2ChromaGpuEngine engOn(cfgOn);
				const RefRole kRoles[2] = { RefRole::PrevPeak, RefRole::NextPeak };

				std::vector<double> kernOff, wallOff, kernOn, wallOn;
				int dispOff = 0, dispOn = 0;

				// ABAB 4-block interleaved schedule: A(100) -> B(100) -> A(100) -> B(100) (or nTimed/2 per block)
				// canceling laptop GPU boost/thermal drift identically between ReuseLuma OFF and ON!
				const int halfTimed = (std::max)(20, nTimed / 2);
				const bool ababOrder[4] = { false, true, false, true };
				for (int blk = 0; blk < 4; ++blk)
				{
					const bool reuseOn = ababOrder[blk];
					TdV2ChromaGpuEngine& cEng = reuseOn ? engOn : engOff;
					const int warm = (blk < 2) ? nWarmup : 4;
					for (int f = -warm; f < halfTimed; ++f)
					{
						ChromaGpuInput cIn{};
						cIn.seq = static_cast<uint64_t>(blk * 1000 + f + warm + 1);
						cIn.lumaW = w; cIn.lumaH = h;
						cIn.deliveredFormat = delivFmt;
						cIn.codedSubsampling = codedSub;
						cIn.gridStatus = ChromaGridStatus::Identified;
						cIn.receivedTex = recUv;
						cIn.cleanUvTex = cleanUv;
						cIn.hasCleanSplit = true;
						cIn.prevPeak.cleanUvTex = prevUv;
						cIn.nextPeak.cleanUvTex = nextUv;
						cIn.motionVecTex = mvTex;
						cIn.motionMetaTex = mmTex;
						cIn.cellPriorTex = cpTex;
						if (reuseOn)
						{
							cIn.lumaCellWeightsTex = outLw;
							cIn.lumaRefRoles = kRoles;
							cIn.lumaActiveRefCount = 2;
						}

						ChromaGpuOutput cOut{};
						cOut.outCodedDeltaTex = outCodedDelta;
						cOut.outDeliveredUvTex = outDelivUv;
						cOut.readbackCellDiagnostics = false;

						pl_dispatch_reset_frame(ctx.dispatch);
						pl_gpu_finish(ctx.gpu);
						collector.BeginFrame(f >= 0);
						const auto t0 = std::chrono::steady_clock::now();
						const bool ok = cEng.ProcessChromaGpu(ctx.gpu, ctx.dispatch, cIn, cOut, &cArena);
						pl_gpu_finish(ctx.gpu);
						const double wMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
						collector.EndFrame();
						Assert::IsTrue(ok && cOut.succeeded, L"TdV2ChromaGpuEngine::ProcessChromaGpu failed in timing loop");

						if (reuseOn) dispOn = cOut.dispatchCount;
						else dispOff = cOut.dispatchCount;
						if (f >= 0)
						{
							if (reuseOn) { kernOn.push_back(collector.frameKernelSumMs); wallOn.push_back(wMs); }
							else { kernOff.push_back(collector.frameKernelSumMs); wallOff.push_back(wMs); }
						}
					}
				}

				const PercentilePair kOff = ComputePercentiles(kernOff), wOff = ComputePercentiles(wallOff);
				const PercentilePair kOn = ComputePercentiles(kernOn), wOn = ComputePercentiles(wallOn);
				const double cArenaMib = static_cast<double>(cPlan.bytes) / (1024.0 * 1024.0);
				const double cPersOff = static_cast<double>(engOff.AllocatedPersistentVramBytes()) / (1024.0 * 1024.0);
				const double cPersOn = static_cast<double>(engOn.AllocatedPersistentVramBytes()) / (1024.0 * 1024.0);

				char r1[384], r2[384];
				std::snprintf(r1, sizeof(r1), "| %s | %s | realtime | OFF (independent) | %d | %.3f | %.3f | %.3f | %.3f | %.2f | %.2f |\n",
					resName, fmtLabel, dispOff, kOff.p50, kOff.p95, wOff.p50, wOff.p95, cArenaMib, cPersOff);
				std::snprintf(r2, sizeof(r2), "| %s | %s | realtime | ON (reuse_luma) | %d | %.3f | %.3f | %.3f | %.3f | %.2f | %.2f |\n",
					resName, fmtLabel, dispOn, kOn.p50, kOn.p95, wOn.p50, wOn.p95, cArenaMib, cPersOn);
				chromaLog << r1 << r2;

				engOff.Release(ctx.gpu);
				engOn.Release(ctx.gpu);
				cArena.Release();
				for (pl_tex* t : { &recUv, &cleanUv, &prevUv, &nextUv, &outCodedDelta, &outDelivUv })
					pl_tex_destroy(ctx.gpu, t);
			}

			for (pl_tex* t : { &recY, &cleanY, &prevPeakY, &nextPeakY, &outY, &outClean, &outDelta, &outMaps, &outLw, &outDiagC, &mvTex, &mmTex, &cpTex })
				pl_tex_destroy(ctx.gpu, t);
		}

		// Also log Owner D5 GPU Late-Carry Controller exact VRAM budget at 1080p & 4K.
		char lcLine[320];
		std::snprintf(
			lcLine, sizeof(lcLine),
			"\n### 4. Owner D5 `TdV2GpuLateCarryController` Persistent VRAM Budget\n"
			"- 1920x1080: %.2f MiB (`EstimatedPersistentVramBytes`)\n"
			"- 3840x2160: %.2f MiB (`EstimatedPersistentVramBytes`)\n",
			static_cast<double>(TdV2GpuLateCarryController::EstimatedPersistentVramBytes(1920, 1080)) / (1024.0 * 1024.0),
			static_cast<double>(TdV2GpuLateCarryController::EstimatedPersistentVramBytes(3840, 2160)) / (1024.0 * 1024.0));

		pl_dispatch_callback(ctx.dispatch, nullptr, nullptr);
		const std::string fullReport = outLog.str() + passBreakdownLog.str() + chromaLog.str() + lcLine;
		Logger::WriteMessage(fullReport.c_str());
	}
};

} // namespace Tests
