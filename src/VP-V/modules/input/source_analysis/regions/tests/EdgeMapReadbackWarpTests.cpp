// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S#4 (R4 / S#3 R3): WARP / GPU non-blocking R32F readback tests for EdgeMapCpuReadback + MakeEdgeMapCpuOwnerFill
// and LedDrive::NativeCopyReadbackDevice (CopyFormat::R32F) behind VpLaneProducts::ReadbackRing.
// Category GpuWarp: runs on WARP in CPU/WARP tiers, or hardware when VP_TEST_GPU=hardware.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/drive/led_measure/NativeCopyReadback.h>
#include <VP-V/modules/input/source_analysis/focus_map/FocusMapFill.h>
#include <VP-V/modules/input/source_analysis/lane/LaneReadback.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapFill.h>

#include <libplacebo/gpu.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		struct VramUsersResetGuard
		{
			VramUsersResetGuard() { VpSkeleton::VramUsers::Instance().ResetForTesting(); }
			~VramUsersResetGuard() { VpSkeleton::VramUsers::Instance().ResetForTesting(); }
		};

		struct ScopedPlTex
		{
			pl_gpu gpu = nullptr;
			pl_tex tex = nullptr;
			ScopedPlTex(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
			~ScopedPlTex()
			{
				if (gpu && tex)
					pl_tex_destroy(gpu, &tex);
			}
			ScopedPlTex(const ScopedPlTex&) = delete;
			ScopedPlTex& operator=(const ScopedPlTex&) = delete;
		};

		pl_tex MakeSampleableR32f(pl_gpu gpu, int w, int h, const float* data)
		{
			pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(fmt, L"no sampleable R32F format on libplacebo device");
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = fmt;
			params.sampleable = true;
			params.initial_data = data;
			pl_tex tex = pl_tex_create(gpu, &params);
			Assert::IsNotNull(tex, L"cannot create sampleable R32F test texture");
			return tex;
		}

		bool FindVramHeldBytes(uint16_t id, uint64_t* outHeld)
		{
			const VpSkeleton::VramUsers::Snapshot snap = VpSkeleton::VramUsers::Instance().Take();
			for (const std::pair<uint16_t, uint64_t>& h : snap.heldById)
			{
				if (h.first == id)
				{
					if (outHeld) *outHeld = h.second;
					return true;
				}
			}
			return false;
		}
	}

	TEST_CLASS(EdgeMapReadbackWarpTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(EdgeMapCpuReadbackRoundTripsR32FAndDeclaresVramOnWarp)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(EdgeMapCpuReadbackRoundTripsR32FAndDeclaresVramOnWarp)
		{
			using namespace VpSkeleton;
			using namespace Vpv::Regions;

			VramUsersResetGuard vramGuard;

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_log log = fixture.Log();

			constexpr int kW = 64, kH = 48;
			constexpr size_t kElems = size_t(kW) * size_t(kH);
			constexpr size_t kPayloadBytes = kElems * sizeof(float);
			std::vector<float> src10(kElems, 0.0f);
			for (int y = 0; y < kH; ++y)
				for (int x = 0; x < kW; ++x)
					src10[size_t(y) * size_t(kW) + size_t(x)] =
						(x < kW / 2 ? 0.0f : 512.0f) + static_cast<float>((x * 7 + y * 13) % 512);

			ScopedPlTex tex10(gpu, MakeSampleableR32f(gpu, kW, kH, src10.data()));
			const uint16_t edgeVramId = ProductVramUserId(kEdgeMap);
			const uint64_t expectedVram = uint64_t(kEdgeMapReadbackSlots) * uint64_t(kW) * uint64_t(kH) * sizeof(float) * 2u;

			EdgeMapCpuReadback rb;

			// Mandatory negative control: 0 submits and 0 VRAM when Demanded() is false.
			Assert::IsFalse(rb.Demanded(), L"demand defaults to false");
			Assert::IsFalse(rb.ScheduleGpu(log, gpu, 10u, tex10.tex),
				L"negative control: ScheduleGpu returns false without demand");
			Assert::AreEqual(uint64_t(0), rb.Counters().submitted, L"negative control: 0 submits without demand");
			Assert::AreEqual(uint64_t(0), rb.DeclaredVramBytes(), L"negative control: 0 VRAM declared without demand");
			Assert::IsFalse(FindVramHeldBytes(edgeVramId, nullptr),
				L"negative control: VramUsers has no kEdgeMap entry without demand");

			// Enable demand -> ScheduleGpu submits frame 10 and declares exact staging VRAM.
			rb.SetDemand(true);
			Assert::IsTrue(rb.ScheduleGpu(log, gpu, 10u, tex10.tex), L"ScheduleGpu submits frame 10 when demanded");
			Assert::AreEqual(uint64_t(1), rb.Counters().submitted, L"1 submit recorded");
			Assert::AreEqual(expectedVram, rb.DeclaredVramBytes(),
				L"DeclaredVramBytes equals kEdgeMapReadbackSlots * 64 * 48 * sizeof(float) * 2");
			uint64_t heldBytes = 0;
			Assert::IsTrue(FindVramHeldBytes(edgeVramId, &heldBytes),
				L"VramUsers contains ProductVramUserId(kEdgeMap)");
			Assert::AreEqual(expectedVram, heldBytes, L"VramUsers heldBytes matches expectedVram");

			// Non-blocking poll loop until frame 10 is Ready.
			bool ready = false;
			for (int ms = 0; ms < 2000; ++ms)
			{
				rb.Poll();
				if (rb.HasReady(10u))
				{
					ready = true;
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Assert::IsTrue(ready, L"WARP non-blocking readback for frame 10 became Ready within 2000 ms");
			Assert::AreEqual(uint64_t(0), rb.FailedCount(), L"zero NativeCopyReadbackDevice failures");

			// Register CPU owner fill and verify lag-1 FilledAtRead + exact float32 texels + lag-2 Reused.
			SkeletonMemo memo;
			memo.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(memo.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, kEdgeMapUnit, MakeEdgeMapCpuOwnerFill(&rb))
				== SkeletonMemo::Registration::Registered, L"register kEdgeMap CPU owner fill");

			memo.OnSeal(11u);
			const AcquireResult r1 = memo.Acquire(kEdgeMap, 10u, AcquireForm::Cpu);
			Assert::IsTrue(r1.status == AcquireStatus::FilledAtRead && r1.Ok(), L"lag-1 Acquire returns FilledAtRead");
			Assert::IsNotNull(r1.cpu);
			Assert::AreEqual(kPayloadBytes, r1.cpuBytes, L"lag-1 payload equals 64 * 48 * sizeof(float)");
			for (size_t i = 0; i < kElems; ++i)
			{
				float got = 0.0f;
				std::memcpy(&got, r1.cpu + i * sizeof(float), sizeof(float));
				Assert::AreEqual(src10[i], got, 0.0f, L"exact R32F texel round-trip on WARP");
			}

			memo.OnSeal(12u);
			const AcquireResult r2 = memo.Acquire(kEdgeMap, 10u, AcquireForm::Cpu);
			Assert::IsTrue(r2.status == AcquireStatus::Reused && r2.cpu == r1.cpu && r2.cpuBytes == kPayloadBytes,
				L"lag-2 Acquire returns Reused with identical buffer");

			// Disable demand -> withdraws VRAM and releases staging textures before fixture teardown.
			rb.SetDemand(false);
			Assert::AreEqual(uint64_t(0), rb.DeclaredVramBytes(), L"SetDemand(false) withdraws DeclaredVramBytes");
			Assert::IsFalse(FindVramHeldBytes(edgeVramId, nullptr), L"SetDemand(false) removes VramUsers entry");
			rb.Release();
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(NativeCopyReadbackDeviceR32FViaReadbackRingMatchesTexelsOnWarp)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(NativeCopyReadbackDeviceR32FViaReadbackRingMatchesTexelsOnWarp)
		{
			using namespace VpSkeleton;

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_log log = fixture.Log();

			constexpr int kW = 64, kH = 48;
			constexpr size_t kElems = size_t(kW) * size_t(kH);
			constexpr size_t kPayloadBytes = kElems * sizeof(float);
			std::vector<float> src1(kElems, 0.0f), src2(kElems, 0.0f);
			for (int y = 0; y < kH; ++y)
			{
				for (int x = 0; x < kW; ++x)
				{
					const size_t idx = size_t(y) * size_t(kW) + size_t(x);
					src1[idx] = static_cast<float>((x * 5 + y * 11) % 1024);
					src2[idx] = static_cast<float>(1023 - ((x * 3 + y * 17) % 1024));
				}
			}

			ScopedPlTex tex1(gpu, MakeSampleableR32f(gpu, kW, kH, src1.data()));
			ScopedPlTex tex2(gpu, MakeSampleableR32f(gpu, kW, kH, src2.data()));

			LedDrive::NativeCopyReadbackDevice dev;
			auto resolver = [&](ProductId id, uint64_t seq) -> pl_tex {
				if (id != kEdgeMap) return nullptr;
				if (seq == 1u) return tex1.tex;
				if (seq == 2u) return tex2.tex;
				return nullptr;   // seq 3 resolves to nullptr for negative control
			};
			Assert::IsTrue(dev.Create(log, gpu, 3, kW, kH, LedDrive::CopyFormat::R32F, resolver),
				L"NativeCopyReadbackDevice creates 3 R32F staging slots");

			VpLaneProducts::ReadbackRing ring(&dev, 3);
			Assert::IsTrue(ring.Submit(kEdgeMap, 1u), L"submit seq 1");
			Assert::IsTrue(ring.Submit(kEdgeMap, 2u), L"submit seq 2");

			// Mandatory negative control: resolver returning nullptr for seq 3 makes Submit return false and counts copyFailed.
			Assert::IsFalse(ring.Submit(kEdgeMap, 3u),
				L"negative control: null resolver texture makes ReadbackRing::Submit(3) return false");
			Assert::AreEqual(uint64_t(1), ring.Counters().copyFailed,
				L"negative control: copyFailed == 1 on null resolver texture");
			Assert::IsFalse(ring.HasPending(3u), L"negative control: failed seq 3 is not pending");

			bool bothReady = false;
			for (int ms = 0; ms < 2000; ++ms)
			{
				ring.Poll();
				if (ring.HasReady(1u) && ring.HasReady(2u))
				{
					bothReady = true;
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Assert::IsTrue(bothReady, L"both seq 1 and seq 2 became Ready within 2000 ms");
			Assert::AreEqual(uint64_t(0), dev.FailedCount(), L"zero HRESULT readback failures");

			// Take in ascending order (1 then 2) so seq 1 is not dropped as stale.
			std::vector<float> got1(kElems, -1.0f), got2(kElems, -1.0f);
			size_t bytes1 = 0, bytes2 = 0;
			Assert::IsTrue(ring.Take(1u, [&](const void* p, size_t n) {
				bytes1 = n;
				if (n == kPayloadBytes) std::memcpy(got1.data(), p, n);
			}), L"Take(1) succeeds");
			Assert::IsTrue(ring.Take(2u, [&](const void* p, size_t n) {
				bytes2 = n;
				if (n == kPayloadBytes) std::memcpy(got2.data(), p, n);
			}), L"Take(2) succeeds");

			Assert::AreEqual(kPayloadBytes, bytes1, L"seq 1 payload size == 64 * 48 * 4");
			Assert::AreEqual(kPayloadBytes, bytes2, L"seq 2 payload size == 64 * 48 * 4");
			for (size_t i = 0; i < kElems; ++i)
			{
				Assert::AreEqual(src1[i], got1[i], 0.0f, L"seq 1 exact R32F texel match");
				Assert::AreEqual(src2[i], got2[i], 0.0f, L"seq 2 exact R32F texel match");
			}
			Assert::IsFalse(ring.HasPending(1u) || ring.HasPending(2u), L"both slots freed after Take");
			dev.Release();
		}

		// W23-Fix2 HIGH-2 regression: GPU per-block (sum2, sum4) reduction via ScheduleBlockSumsGpu
		// eliminates full-res 33 MB CPU readback / 199 MB VRAM (510x smaller payload at 4K), clamps strictly
		// to the active picture rect (zero letterbox bar-edge bleed on straddling blocks; same-input negative
		// control without active rect bleeds >100x larger sum2), matches CPU FocusMap::EdgeSums, and feeds FocusMapFill.
		BEGIN_TEST_METHOD_ATTRIBUTE(EdgeMapBlockSumsGpuReductionMatchesCpuActiveRectAndCutsVram500x)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(EdgeMapBlockSumsGpuReductionMatchesCpuActiveRectAndCutsVram500x)
		{
			using namespace VpSkeleton;
			using namespace Vpv::Regions;

			// 4K budget invariant proof: 65 KB payload vs 33 MB full-res (>500x reduction).
			const FocusMap::Geometry g4k = FocusMap::Geometry::Make(3840, 2160);
			const size_t full4kBytes = size_t(3840) * size_t(2160) * sizeof(float);
			const size_t block4kBytes = FocusMap::EdgeBlockSumsPayloadBytes(g4k.gw, g4k.gh);
			Assert::IsTrue(block4kBytes * 500u < full4kBytes, L"4K GPU block-sums payload is >500x smaller than full-res R32F");

			VramUsersResetGuard vramGuard;

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_log log = fixture.Log();

			constexpr int kW = 128, kH = 96;
			const FocusMap::Geometry gFull = FocusMap::Geometry::Make(kW, kH);
			FocusMap::Geometry gActive = FocusMap::Geometry::Make(kW, kH);
			gActive.SetActiveRect(0, 12, kW, kH - 12);   // rows 0..11 and 84..95 are bright letterbox bar borders

			std::vector<float> edgeVals(size_t(kW) * size_t(kH), 0.0f);
			for (int y = 0; y < kH; ++y)
				for (int x = 0; x < kW; ++x)
					edgeVals[size_t(y) * size_t(kW) + size_t(x)] = (y < 12 || y >= kH - 12) ? 200.0f : 5.0f;

			ScopedPlTex edgeTex(gpu, MakeSampleableR32f(gpu, kW, kH, edgeVals.data()));

			auto waitReady = [](EdgeMapCpuReadback& rb, uint64_t seq) {
				for (int ms = 0; ms < 2000; ++ms)
				{
					rb.Poll();
					if (rb.HasReady(seq))
						return true;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				return false;
			};

			// 1. Protected path: ScheduleBlockSumsGpu with active rect [0, 128) x [12, 84).
			EdgeMapCpuReadback rbActive;
			rbActive.SetDemand(true);
			Assert::IsTrue(rbActive.ScheduleBlockSumsGpu(log, gpu, 10u, edgeTex.tex, 0, 12, kW, kH - 12),
				L"ScheduleBlockSumsGpu submits frame 10");
			const uint64_t fullResVram = uint64_t(kEdgeMapReadbackSlots) * uint64_t(kW) * uint64_t(kH) * sizeof(float) * 2u;
			const uint64_t expectedBlockVram = uint64_t(kEdgeMapReadbackSlots) * uint64_t(gActive.gw) * uint64_t(gActive.gh) * 16u * 2u;
			Assert::AreEqual(expectedBlockVram, rbActive.DeclaredVramBytes(), L"block-sums staging VRAM is tiny (gw*gh RGBA32F)");
			Assert::IsTrue(rbActive.DeclaredVramBytes() * 10u < fullResVram, L"staging VRAM reduced >10x even at 128x96");
			Assert::IsTrue(waitReady(rbActive, 10u), L"block-sums readback became Ready on WARP");

			// Acquire through SkeletonMemo + FocusMapFill and compare every block against CPU FocusMap::EdgeSums.
			SkeletonMemo memo;
			memo.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(memo.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, kEdgeMapUnit, MakeEdgeMapCpuOwnerFill(&rbActive))
				== SkeletonMemo::Registration::Registered);

			memo.OnSeal(11u);
			const AcquireResult rEdge = memo.Acquire(kEdgeMap, 10u, AcquireForm::Cpu);
			Assert::IsTrue(rEdge.Ok() && rEdge.cpu != nullptr, L"kEdgeMap CPU Acquire returns GPU-reduced EMBS payload");
			Assert::AreEqual(FocusMap::EdgeBlockSumsPayloadBytes(gActive.gw, gActive.gh), rEdge.cpuBytes,
				L"payload size equals 16 + gw * gh * 8 bytes (NOT full-res)");

			std::vector<float> gpuS2, gpuS4, cpuS2, cpuS4;
			Assert::IsTrue(FocusMap::UnpackEdgeBlockSums(gActive, rEdge.cpu, rEdge.cpuBytes, gpuS2, gpuS4),
				L"EMBS payload unpacks cleanly");
			std::vector<uint8_t> edgeRawBytes(edgeVals.size() * sizeof(float));
			std::memcpy(edgeRawBytes.data(), edgeVals.data(), edgeRawBytes.size());
			FocusMap::EdgeSums(gActive, edgeRawBytes.data(), cpuS2, cpuS4);
			for (size_t i = 0; i < gActive.Blocks(); ++i)
			{
				Assert::AreEqual(cpuS2[i], gpuS2[i], (std::max)(1e-3f, 1e-4f * cpuS2[i]), L"GPU block sum2 matches CPU active-rect EdgeSums");
				Assert::AreEqual(cpuS4[i], gpuS4[i], (std::max)(1e-1f, 1e-4f * cpuS4[i]), L"GPU block sum4 matches CPU active-rect EdgeSums");
			}

			// Wire FocusMapFill end-to-end on the GPU-reduced EMBS payload.
			std::vector<float> p11(gActive.Blocks() * RegionEvidence::kPrecursorChannels, 0.0f);
			for (size_t i = 0; i < gActive.Blocks(); ++i)
			{
				float* b = &p11[i * RegionEvidence::kPrecursorChannels];
				b[RegionEvidence::kCoverage] = 1.0f;
				b[RegionEvidence::kStatN] = 2.0f;
				b[RegionEvidence::kE1] = 42.0f;
				b[RegionEvidence::kE2] = 10.0f;
				b[RegionEvidence::kE3] = 3.0f;
			}
			OwnerFill f11;
			f11.keepFrames = 2;
			f11.fill = [&p11](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) {
				o.bytes.resize(p11.size() * sizeof(float));
				std::memcpy(o.bytes.data(), p11.data(), o.bytes.size());
				return true;
			};
			Assert::IsTrue(memo.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kEdgeMapUnit, f11) == SkeletonMemo::Registration::Registered);
			FocusMap::FillConfig fcfg;
			fcfg.geometry = [kW, kH](uint64_t, int& w, int& h) { w = kW; h = kH; return true; };
			Assert::IsTrue(memo.RegisterOwnerFill(kFocusMap, AcquireForm::Cpu, FocusMap::kUnitPath, FocusMap::MakeOwnerFill(fcfg))
				== SkeletonMemo::Registration::Registered);

			const AcquireResult rFocus = memo.Acquire(kFocusMap, 11u, AcquireForm::Cpu);
			Assert::IsTrue(rFocus.Ok() && rFocus.cpu != nullptr, L"FocusMapFill succeeds directly from GPU-reduced EMBS payload");
			const size_t straddleIdx = static_cast<size_t>(1) * gActive.gw + 4u;
			Assert::IsTrue(FocusMap::Unpack(rFocus.cpu + straddleIdx * FocusMap::kBytesPerBlock).defocus > 0.9f,
				L"straddling block row by=1 reads defocused with zero letterbox bar bleed");

			// 2. Mandatory same-input negative control: full-frame reduction (no active rect) bleeds bar edge into by=1.
			EdgeMapCpuReadback rbUnclamped;
			rbUnclamped.SetDemand(true);
			Assert::IsTrue(rbUnclamped.ScheduleBlockSumsGpu(log, gpu, 20u, edgeTex.tex),
				L"negative control: unclamped ScheduleBlockSumsGpu submits frame 20");
			Assert::IsTrue(waitReady(rbUnclamped, 20u), L"unclamped readback became Ready");
			std::vector<uint8_t> unclampedBytes;
			bool pending = false;
			std::string why;
			Assert::IsTrue(rbUnclamped.PollAndTake(20u, unclampedBytes, pending, why), L"unclamped PollAndTake succeeded");
			std::vector<float> naiveS2, naiveS4;
			Assert::IsTrue(FocusMap::UnpackEdgeBlockSums(gFull, unclampedBytes.data(), unclampedBytes.size(), naiveS2, naiveS4));
			Assert::IsTrue(naiveS2[straddleIdx] > gpuS2[straddleIdx] * 100.0f,
				L"negative control: without active rect, letterbox bar bleeds >100x larger sum2 into straddling block");

			rbActive.SetDemand(false);
			rbUnclamped.SetDemand(false);
		}
	};
}
