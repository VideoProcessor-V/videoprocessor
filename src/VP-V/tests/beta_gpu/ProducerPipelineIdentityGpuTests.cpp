// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Slice 16 P3 (refs/105 s3.7 (i)-(iii)): Producer pipeline bit-identity GPU test (Beta test host).
// Proves:
//   (i)   Engine-input 8-bit luma hash per frame is byte-identical between single-threaded HalfLuma8
//         and persistent HalfLumaPool across 600 synthetic P010 pan frames.
//   (ii)  2-wide parallel FlowGlobal + FlowExtras matches serial FlowGlobals + FlowExtras byte-for-byte.
//   (iii) P2 payload hashes (forward, backward, forwardCost, backwardCost, global, second, costWeight,
//         fbError) are identical between serial submission and pipelined submission (3 staging sets +
//         ENGINE/BACK split) across 600 frames, 2 independent runs, with temporal hints ON and OFF.
// Missing NVIDIA / NVOF hardware logs explicit SKIP-NO-HARDWARE (never a silent pass).
#include <CppUnitTest.h>

#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>
#include <AnalysisLumaSource.h>

#include <atlbase.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ProducerPipelineIdentity
{
	using Clock = std::chrono::steady_clock;

	inline uint32_t Hash2(uint32_t x, uint32_t y)
	{
		uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ 0x9e3779b9u;
		h ^= h >> 15;
		h *= 0x2c1b3c6du;
		h ^= h >> 12;
		h *= 0x297a2d39u;
		h ^= h >> 15;
		return h;
	}

	inline uint64_t Fnv(uint64_t h, const void* data, size_t bytes)
	{
		const uint8_t* p = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < bytes; ++i)
		{
			h ^= p[i];
			h *= 1099511628211ull;
		}
		return h;
	}

	struct Canvas
	{
		int frameW = 0, frameH = 0, canvasW = 0, canvasH = 0, range = 0;
		std::shared_ptr<std::vector<uint16_t>> words;
	};

	inline Canvas MakeCanvas(int frameW, int frameH)
	{
		Canvas c;
		c.frameW = frameW;
		c.frameH = frameH;
		c.range = 192;
		c.canvasW = frameW + c.range;
		c.canvasH = frameH + c.range + frameH / 2 + 2;
		c.words = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(c.canvasW) * c.canvasH);
		std::vector<uint16_t>& w = *c.words;
		for (int y = 0; y < c.canvasH; ++y)
			for (int x = 0; x < c.canvasW; ++x)
			{
				const double v = ((Hash2(static_cast<uint32_t>(x / 8), static_cast<uint32_t>(y / 8)) & 1023) / 1023.0) * 0.7
					+ ((Hash2(static_cast<uint32_t>(x + 5), static_cast<uint32_t>(y + 3)) & 255) / 255.0) * 0.3;
				const int code = 64 + static_cast<int>(v * (940 - 64) + 0.5);
				w[static_cast<size_t>(y) * c.canvasW + x] = static_cast<uint16_t>(code << 6);
			}
		return c;
	}

	inline int Triangle(long long t, int span)
	{
		const long long period = 2LL * span;
		long long p = t % period;
		if (p < 0) p += period;
		return static_cast<int>(p <= span ? p : period - p);
	}

	inline AnalysisLumaSource FrameOf(const Canvas& c, uint64_t f)
	{
		const int x0 = Triangle(static_cast<long long>(f) * 4, c.range);
		const int y0 = Triangle(static_cast<long long>(f) * 2, c.range);
		const size_t offset = (static_cast<size_t>(y0) * c.canvasW + x0) * 2;
		const size_t total = c.words->size() * 2;
		AnalysisLumaSource s;
		s.data = reinterpret_cast<const uint8_t*>(c.words->data()) + offset;
		s.dataBytes = total - offset;
		s.width = c.frameW;
		s.height = c.frameH;
		s.rowBytes = static_cast<size_t>(c.canvasW) * 2;
		s.chromaRowBytes = static_cast<size_t>(c.frameW) * 2;
		s.format = AnalysisLumaFormat::P010;
		s.backingOwner = c.words;
		return s;
	}

	inline uint64_t HashFlow(const MotionAhead::Flow& flow)
	{
		uint64_t h = 1469598103934665603ull;
		h = Fnv(h, flow.forward.data(), flow.forward.size() * sizeof(flow.forward[0]));
		h = Fnv(h, flow.backward.data(), flow.backward.size() * sizeof(flow.backward[0]));
		h = Fnv(h, flow.forwardCost.data(), flow.forwardCost.size() * sizeof(flow.forwardCost[0]));
		h = Fnv(h, flow.backwardCost.data(), flow.backwardCost.size() * sizeof(flow.backwardCost[0]));
		h = Fnv(h, flow.global, sizeof(flow.global));
		h = Fnv(h, flow.second, sizeof(flow.second));
		h = Fnv(h, flow.costWeight.data(), flow.costWeight.size() * sizeof(flow.costWeight[0]));
		h = Fnv(h, flow.fbError.data(), flow.fbError.size() * sizeof(flow.fbError[0]));
		return h;
	}

	inline bool NvidiaDevice(CComPtr<ID3D11Device>& device)
	{
		CComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
			return false;
		for (UINT i = 0;; ++i)
		{
			CComPtr<IDXGIAdapter1> adapter;
			if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
				return false;
			DXGI_ADAPTER_DESC1 desc{};
			adapter->GetDesc1(&desc);
			if (desc.VendorId != 0x10DE)
				continue;
			const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			if (SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr, nullptr)))
				return true;
		}
	}

	inline bool CollectHashes(ID3D11Device* device, const Canvas& canvas, int frames, bool pipelined, bool hints,
		std::vector<uint64_t>& hashes, std::string& why)
	{
		MotionAhead::Producer producer;
		producer.SetDepth(6);
		producer.SetLead(6);
		producer.SetTemporalHintsForTest(hints);
		if (!producer.Start(device, NvOpticalFlow::Preset::Medium))
		{
			why = "start: " + producer.Error();
			return false;
		}
		hashes.clear();
		hashes.reserve(static_cast<size_t>((std::max)(0, frames - 1)));
		const int maxInFlight = pipelined ? 4 : 1;
		int nextSubmit = 0;
		int nextCollect = 1;
		const Clock::time_point overallDeadline = Clock::now() + std::chrono::seconds(180);
		while (nextCollect < frames)
		{
			while (nextSubmit < frames && (nextSubmit - nextCollect) < maxInFlight)
			{
				producer.Submit(static_cast<uint64_t>(nextSubmit), 1, FrameOf(canvas, static_cast<uint64_t>(nextSubmit)));
				++nextSubmit;
			}
			MotionAhead::FlowPtr flow = producer.Take(static_cast<uint64_t>(nextCollect - 1), static_cast<uint64_t>(nextCollect));
			if (flow)
			{
				hashes.push_back(HashFlow(*flow));
				producer.Prune(static_cast<uint64_t>(nextCollect));
				++nextCollect;
				continue;
			}
			if (Clock::now() > overallDeadline || !producer.Running())
			{
				why = "pair " + std::to_string(nextCollect - 1) + "->" + std::to_string(nextCollect) + ": " + producer.Error();
				producer.Stop();
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		producer.Stop();
		return true;
	}
}

TEST_CLASS(ProducerPipelineIdentityGpuTests)
{
public:
	TEST_METHOD(EngineInputHashPerFrameMatchesBetweenSerialAndHalfLumaPool)
	{
		using namespace ProducerPipelineIdentity;
		const Canvas canvas = MakeCanvas(640, 360);
		MotionAhead::HalfLumaPool pool;
		pool.Start(2);
		std::vector<uint8_t> serialBuf, pooledBuf;
		int sw = 0, sh = 0, pw = 0, ph = 0;
		for (int f = 0; f < 600; ++f)
		{
			const AnalysisLumaSource src = FrameOf(canvas, static_cast<uint64_t>(f));
			Assert::IsTrue(MotionAhead::HalfLuma8(src, serialBuf, sw, sh, nullptr));
			Assert::IsTrue(MotionAhead::HalfLuma8(src, pooledBuf, pw, ph, &pool));
			Assert::AreEqual(sw, pw);
			Assert::AreEqual(sh, ph);
			const uint64_t hSerial = Fnv(1469598103934665603ull, serialBuf.data(), serialBuf.size());
			const uint64_t hPooled = Fnv(1469598103934665603ull, pooledBuf.data(), pooledBuf.size());
			Assert::AreEqual(hSerial, hPooled, L"engine-input luma hash must be byte-identical");
		}
		pool.Stop();
	}

	TEST_METHOD(P2HashesSerialEqualsPipelined600FramesTwoRunsHintsOnAndOff)
	{
		using namespace ProducerPipelineIdentity;
		CComPtr<ID3D11Device> device;
		if (!NvidiaDevice(device))
		{
			Logger::WriteMessage("SKIP-NO-HARDWARE: ProducerPipelineIdentityGpuTests requires an NVIDIA D3D11 adapter with NVOF.\n");
			return;
		}
		const Canvas canvas = MakeCanvas(640, 360);
		constexpr int kFrames = 600;
		for (int hintsInt = 0; hintsInt <= 1; ++hintsInt)
		{
			const bool hints = hintsInt != 0;
			std::vector<uint64_t> serialA, serialB, pipeA, pipeB;
			std::string why;
			if (!CollectHashes(device, canvas, kFrames, false, hints, serialA, why))
			{
				if (why.find("nvofapi64.dll") != std::string::npos || why.find("no optical flow engine") != std::string::npos)
				{
					Logger::WriteMessage(("SKIP-NO-HARDWARE: NVOF unavailable (" + why + ").\n").c_str());
					return;
				}
				Assert::Fail(std::wstring(why.begin(), why.end()).c_str());
			}
			Assert::IsTrue(CollectHashes(device, canvas, kFrames, false, hints, serialB, why),
				std::wstring(why.begin(), why.end()).c_str());
			Assert::IsTrue(CollectHashes(device, canvas, kFrames, true, hints, pipeA, why),
				std::wstring(why.begin(), why.end()).c_str());
			Assert::IsTrue(CollectHashes(device, canvas, kFrames, true, hints, pipeB, why),
				std::wstring(why.begin(), why.end()).c_str());

			Assert::AreEqual<size_t>(kFrames - 1, serialA.size());
			Assert::AreEqual<size_t>(kFrames - 1, serialB.size());
			Assert::AreEqual<size_t>(kFrames - 1, pipeA.size());
			Assert::AreEqual<size_t>(kFrames - 1, pipeB.size());

			size_t aaMatch = 0, abMatchA = 0, abMatchB = 0;
			for (size_t i = 0; i < serialA.size(); ++i)
			{
				if (serialA[i] == serialB[i]) ++aaMatch;
				if (serialA[i] == pipeA[i]) ++abMatchA;
				if (serialA[i] == pipeB[i]) ++abMatchB;
			}
			Logger::WriteMessage((std::string("ProducerPipelineIdentity (hints=") + (hints ? "ON" : "OFF") +
				"): serial A/A " + std::to_string(aaMatch) + "/" + std::to_string(serialA.size()) +
				", serial==pipelined run1 " + std::to_string(abMatchA) + "/" + std::to_string(serialA.size()) +
				", run2 " + std::to_string(abMatchB) + "/" + std::to_string(serialA.size()) + "\n").c_str());
			Assert::AreEqual(serialA.size(), aaMatch, L"serial A/A must be deterministic");
			Assert::AreEqual(serialA.size(), abMatchA, L"pipelined run 1 must be byte-identical to serial");
			Assert::AreEqual(serialA.size(), abMatchB, L"pipelined run 2 must be byte-identical to serial");
		}
	}
};
