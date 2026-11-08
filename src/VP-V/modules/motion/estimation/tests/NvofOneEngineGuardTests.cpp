// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV (doc 83 section 5, guard 1): the runtime ONE-ENGINE GUARD. NvOpticalFlow::Sessions (NvOpticalFlow.h) counts every open
// Engine / HintedEngine per GPU. The CPU test pins the counting rules; the hardware test (VP_TEST_GPU=hardware, NVIDIA) runs the
// look-ahead producer, the de-judder's matcher (with and without a pair) and the long-pair service and asserts that exactly one
// NvOpticalFlow::Engine is open on the GPU throughout and that no second one was ever opened.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>
#include <VP-V/modules/motion/estimation/longpair/LongPairService.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <dxgi1_2.h>
#include <libplacebo/d3d11.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace VideoProcessorTest
{
	TEST_CLASS(NvofOneEngineGuardTests)
	{
	public:
		TEST_METHOD(SessionTableCountsPerGpuAndFlagsASecondEngine)
		{
			using NvOpticalFlow::Sessions;
			using NvOpticalFlow::SessionKind;
			Sessions::ResetForTest();
			Assert::AreEqual(0, Sessions::Open(SessionKind::Engine, 0));
			Assert::IsTrue(Sessions::AdapterKey(nullptr) == Sessions::kUnknownAdapterKey, L"an unknown adapter has its own key, never 0 (all GPUs)");
			Sessions::Opened(SessionKind::Engine, 11);
			Sessions::Opened(SessionKind::Engine, 22);   // another GPU: not a second engine
			Assert::AreEqual(1, Sessions::Open(SessionKind::Engine, 11));
			Assert::AreEqual(2, Sessions::Open(SessionKind::Engine, 0), L"all GPUs");
			Assert::AreEqual<unsigned long long>(0, Sessions::Violations(), L"one engine per GPU is the rule");
			Sessions::Opened(SessionKind::Hinted, 11);   // the long-pair session is counted apart, never a violation
			Assert::AreEqual<unsigned long long>(0, Sessions::Violations());
			Assert::AreEqual(1, Sessions::Open(SessionKind::Hinted, 11));
			Assert::AreEqual(1, Sessions::MostOpenOnOneGpu(SessionKind::Engine), L"not above one yet");
			Sessions::Opened(SessionKind::Engine, 11);   // a second Engine on GPU 11
			Assert::AreEqual<unsigned long long>(1, Sessions::Violations());
			Assert::AreEqual(2, Sessions::MostOpenOnOneGpu(SessionKind::Engine), L"above one: the guard's fallback entry engages");
			Sessions::Opened(SessionKind::Engine, Sessions::kUnknownAdapterKey);   // an unknown GPU counts as a GPU of its own
			Assert::AreEqual<unsigned long long>(1, Sessions::Violations());
			Sessions::Closed(SessionKind::Engine, Sessions::kUnknownAdapterKey);
			Assert::AreEqual(2, Sessions::Peak(SessionKind::Engine, 11));
			Sessions::Closed(SessionKind::Engine, 11);
			Sessions::Closed(SessionKind::Engine, 11);
			Sessions::Closed(SessionKind::Engine, 11);   // closing more than were opened never goes below zero
			Assert::AreEqual(0, Sessions::Open(SessionKind::Engine, 11));
			Assert::AreEqual(1, Sessions::Open(SessionKind::Engine, 22));
			Assert::AreEqual(2, Sessions::Peak(SessionKind::Engine, 11), L"the peak stays");
			Sessions::ResetForTest();
			Assert::AreEqual<unsigned long long>(0, Sessions::Violations());
		}

		// SK-MV guard 3 for the long pair: an ACTIVE optical-flow user that declared nothing is flagged by the governor's population guard
		// (skeleton_vram_users), and a declared one (what the producer and the long-pair service do, RingBytes) is not.
		TEST_METHOD(AnActiveOpticalFlowUserWithoutADeclarationIsFlaggedAndADeclaredOneIsNot)
		{
			VpSkeleton::VramUsers& users = VpSkeleton::VramUsers::Instance();
			users.ResetForTesting();
			int owner = 0;
			users.MarkActive(&owner, VpSkeleton::kUserNvofProducer, true);
			VpSkeleton::VramUsers::Snapshot s = users.Take();
			Assert::AreEqual(size_t(1), s.unpopulated.size(), L"active and undeclared: flagged");
			Assert::IsTrue(s.unpopulated[0] == VpSkeleton::kUserNvofProducer);
			const uint64_t bytes = NvOpticalFlow::RingBytes(960, 540, false, true);   // the long pair's ring: input, flow, cost x2 and the hint
			Assert::IsTrue(bytes > 960u * 540u * 2u, L"the ring is more than the two inputs");
			users.DeclareFixed(&owner, VpSkeleton::kUserNvofProducer, bytes);
			s = users.Take();
			Assert::IsTrue(s.unpopulated.empty(), L"declared: not flagged");
			Assert::AreEqual(size_t(1), s.heldById.size());
			Assert::AreEqual<uint64_t>(bytes, s.heldById[0].second);
			users.Withdraw(&owner, VpSkeleton::kUserNvofProducer);
			users.MarkActive(&owner, VpSkeleton::kUserNvofProducer, false);
			Assert::IsTrue(users.Take().unpopulated.empty());
			users.ResetForTesting();
		}

		// SK-MV: when "no pair ahead" becomes visible: not a bare frame count, and a partial miss ratio shows too.
		TEST_METHOD(NoPairTrackerThresholdFollowsTheDepthAndShowsPartialMisses)
		{
			using VpMotion::NoPairTracker;
			Assert::AreEqual(8u, NoPairTracker::Threshold(0), L"the floor");
			Assert::AreEqual(8u, NoPairTracker::Threshold(2), L"24p depth 2 + margin 4 is under the floor");
			Assert::AreEqual(8u, NoPairTracker::Threshold(4), L"60p depth 4 + margin 4");
			Assert::AreEqual(20u, NoPairTracker::Threshold(16), L"240p depth 16: a refill takes that long");
			NoPairTracker t;
			for (int i = 0; i < 7; ++i) t.Note(true, 4);
			Assert::IsFalse(t.Sustained(4), L"7 in a row is below the threshold");
			t.Note(true, 4);
			Assert::IsTrue(t.Sustained(4), L"8 in a row");
			Assert::IsTrue(t.Describe(4).find("in a row") != std::string::npos);
			t.Note(false, 4);
			Assert::IsFalse(t.Sustained(4), L"a pair resets the run");
			// 7 misses in 8 (a run counter that keeps resetting): the window ratio shows it.
			NoPairTracker r;
			for (int i = 0; i < 32; ++i) r.Note(i % 8 != 7, 4);
			Assert::IsTrue(r.run < NoPairTracker::Threshold(4), L"the run never reaches the threshold");
			Assert::IsTrue(r.Sustained(4), L"but 28 of 32 estimates missed");
			Assert::IsTrue(r.Describe(4).find("28 of the last 32") != std::string::npos, r.Describe(4).empty() ? L"empty" : L"text");
			for (int i = 0; i < 32; ++i) r.Note(false, 4);
			Assert::IsFalse(r.Sustained(4), L"a clean window releases it");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ExactlyOneEngineAcrossProducerDejudderAndLongPair)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ExactlyOneEngineAcrossProducerDejudderAndLongPair)
		{
			using NvOpticalFlow::Sessions;
			using NvOpticalFlow::SessionKind;
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
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, "ExactlyOneEngineAcrossProducerDejudderAndLongPair",
					nvidia != nullptr, "no NVIDIA adapter"))
				return;
			CComPtr<ID3D11Device> device;
			Assert::IsTrue(SUCCEEDED(D3D11CreateDevice(nvidia, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
			const uint64_t gpu = Sessions::AdapterKey(device);
			Assert::IsTrue(gpu != Sessions::kUnknownAdapterKey, L"the adapter key");
			Sessions::ResetForTest();
			VpSkeleton::VramUsers::Instance().ResetForTesting();

			// A textured pair (1920 x 1080 P010 for the producer; the same texture family for the matcher).
			const int W = 1920, H = 1080, dx = 8, dy = 4, BW = W + 64, BH = H + 64;
			std::vector<uint8_t> base(static_cast<size_t>(BW) * BH);
			uint32_t seed = 777u;
			for (uint8_t& v : base) { seed = seed * 1664525u + 1013904223u; v = static_cast<uint8_t>(seed >> 24); }
			auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
			auto lumaAt = [&](int index, int x, int y) {
				return base[static_cast<size_t>(clampi(y - index * dy + 32, 0, BH - 1)) * BW + clampi(x - index * dx + 32, 0, BW - 1)];
			};
			auto source = [&](int index) {
				auto buffer = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(W) * H * 3 / 2, static_cast<uint16_t>(512 << 6));
				for (int y = 0; y < H; ++y)
					for (int x = 0; x < W; ++x)
						(*buffer)[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>((64 + lumaAt(index, x, y) * 3) << 6);
				AnalysisLumaSource s;
				s.data = reinterpret_cast<const uint8_t*>(buffer->data());
				s.dataBytes = buffer->size() * 2;
				s.width = W;
				s.height = H;
				s.rowBytes = static_cast<size_t>(W) * 2;
				s.chromaRowBytes = static_cast<size_t>(W) * 2;
				s.format = AnalysisLumaFormat::P010;
				s.backingOwner = buffer;
				return s;
			};

			MotionAhead::Producer producer;
			Assert::IsTrue(producer.Start(device, NvOpticalFlow::Preset::Slow), L"producer started");
			producer.Submit(1, 1, source(0));
			producer.Submit(2, 1, source(1));
			MotionAhead::FlowPtr flow;
			for (int k = 0; k < 600 && !flow; ++k)
			{
				flow = producer.Take(1, 2);
				if (!flow && !producer.Running()) break;
				if (!flow) std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			if (!flow)
			{
				HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, "ExactlyOneEngineAcrossProducerDejudderAndLongPair", false,
					"the optical flow engine is not available: " + producer.Error());
				return;
			}
			Assert::AreEqual(1, Sessions::Open(SessionKind::Engine, gpu), L"the producer's engine is the one session");

			{
				// The de-judder's matcher: with the pair, then without one (the render-time engine of the old code would be the second).
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
				pl_gpu plgpu = fixture.Gpu();
				// The matcher must run on the SAME GPU as the producer or the one-per-GPU count proves nothing: fail clearly otherwise
				// (VP_TEST_GPU=hardware and VP_TEST_ADAPTER=NVIDIA).
				pl_d3d11 d3dFixture = pl_d3d11_get(plgpu);
				Assert::IsTrue(d3dFixture && d3dFixture->device && Sessions::AdapterKey(d3dFixture->device) == gpu,
					L"the libplacebo fixture device is not on the NVIDIA adapter the producer uses");
				const DejudderFrame a = MakeDejudderFrame(W, H, [&](int x, int y) { return static_cast<uint16_t>(lumaAt(0, x, y) << 8); },
					[](int, int, int) { return static_cast<uint16_t>(32768); });
				const DejudderFrame b = MakeDejudderFrame(W, H, [&](int x, int y) { return static_cast<uint16_t>(lumaAt(1, x, y) << 8); },
					[](int, int, int) { return static_cast<uint16_t>(32768); });
				DejudderGpuPlanes pa, pb;
				UploadDejudderFrame(plgpu, a, pa);
				UploadDejudderFrame(plgpu, b, pb);
				Dejudder::Settings settings = DejudderTestSettings();
				settings.opticalFlow = true;
				settings.opticalFlowCandidate = true;
				settings.opticalFlowSeed = true;
				settings.estCandidates = true;
				Dejudder::MotionWarp warp(fixture.Log(), plgpu, settings);
				warp.SetExternalFlow(flow);
				Assert::IsTrue(warp.Estimate(pa.planes, pb.planes), L"estimate with the pair ahead");
				Assert::IsTrue(warp.ExternalFlowUsed(), L"the pair ahead was taken (size or device mismatch otherwise)");
				Assert::AreEqual(1, Sessions::Open(SessionKind::Engine, gpu), L"the de-judder opened no engine (pair)");
				Assert::IsTrue(warp.Estimate(pb.planes, pa.planes), L"estimate without a pair: the matcher");
				Assert::IsFalse(warp.OpticalFlowUsed(), L"no render-time engine: the block searches ran");
				Assert::IsTrue(warp.NoPairAhead(), L"and the miss is reported, not silent");
				Assert::AreEqual(1, Sessions::Open(SessionKind::Engine, gpu), L"the de-judder opened no engine (no pair)");
			}

			{
				// The long-pair service: its own (hinted) session is counted apart; the Engine count must not move.
				std::vector<uint8_t> hl(static_cast<size_t>(W / 2) * (H / 2)), hr(hl.size());
				for (int y = 0; y < H / 2; ++y)
					for (int x = 0; x < W / 2; ++x)
					{
						hl[static_cast<size_t>(y) * (W / 2) + x] = lumaAt(0, 2 * x, 2 * y);
						hr[static_cast<size_t>(y) * (W / 2) + x] = lumaAt(1, 2 * x, 2 * y);
					}
				VpLongPair::Service service;
				service.SetAdapter(nvidia);
				const VpLongPair::Result r = service.RunNow(hl.data(), hr.data(), W / 2, H / 2, nullptr);
				Assert::IsTrue(r.ok, L"the long pair produced a field (hinted session or the visible seeded search)");
				Assert::AreEqual(1, Sessions::Open(SessionKind::Engine, gpu), L"the long-pair service opened no Engine");
				// The step must PROVE a hinted session or say why not: a pair that ran on the NVOF session was counted (peak 1 on this GPU);
				// a pair that took the seeded search must carry the reason the session did not open (never an unexplained skip).
				if (r.path == VpLongPair::Path::NvofHinted || r.path == VpLongPair::Path::NvofUnhinted)
				{
					Assert::AreEqual(1, Sessions::Peak(SessionKind::Hinted, gpu), L"the long pair ran on a hinted session and the guard counted it");
					// Its textures are in the governor's ledger next to the producer's (same user id, summed): the half-size ring of each.
					const VpSkeleton::VramUsers::Snapshot held = VpSkeleton::VramUsers::Instance().Take();
					uint64_t nvof = 0;
					for (const auto& h : held.heldById)
						if (h.first == VpSkeleton::kUserNvofProducer) nvof = h.second;
					Assert::AreEqual<uint64_t>(NvOpticalFlow::RingBytes(W / 2, H / 2, false, false) + NvOpticalFlow::RingBytes(W / 2, H / 2, false, true), nvof,
						L"the producer's and the long pair's rings are both declared");
					Assert::IsTrue(held.unpopulated.empty(), L"no active optical-flow user is undeclared");
				}
				else
					Assert::IsFalse(r.why.reason.empty(), L"seeded search without a reason for the session not opening");
				Logger::WriteMessage(("long-pair path " + std::string(VpLongPair::PathName(r.path)) + ", hinted sessions at peak: " +
					std::to_string(Sessions::Peak(SessionKind::Hinted, gpu)) + "\n").c_str());
			}

			Assert::AreEqual<unsigned long long>(0, Sessions::Violations(), L"a second Engine was never opened");
			Assert::AreEqual(1, Sessions::Peak(SessionKind::Engine, gpu), L"never more than one at once");
			producer.Stop();
			Assert::AreEqual(0, Sessions::Open(SessionKind::Engine, gpu), L"the session closes with the producer");
		}
	};
}
