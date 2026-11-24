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
// Missing NVIDIA / NVOF hardware routes through HardwarePrerequisites::RequireOrSkip(Kind::Gpu)
// (logs SKIP-NO-GPU, and fails under VP_TEST_REQUIRE_GPU=1 / VP_TEST_REQUIRE_ALL=1).
#include <CppUnitTest.h>

#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>
#include <AnalysisLumaSource.h>

#include <atlbase.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
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

	// Synthetic non-trivial multi-quadrant / multi-layer flow for (ii):
	// - Top 55% of rows (low-cost dominant layer, top 35% <= median cost): coherent dominant pan,
	//   opposite sign between forward and backward so channel-swap bugs are caught immediately.
	// - Bottom-left quadrant (low-cost second layer, ~22.5% of cells): distinct secondary motion
	//   with asymmetric forward/backward vectors -> populates second[0..1] and non-zero fbError.
	// - Bottom-right quadrant (~22.5% of cells): top half intermediate cost (smoothstep costWeight
	//   strictly in (0, 1)), bottom half high cost (costWeight == 0.0f), plus noisy/out-of-range
	//   vectors exercising vote range clipping and occlusion fbError.
	inline MotionAhead::Flow MakeMultiQuadrantFlow(int halfW, int halfH, uint32_t seed)
	{
		MotionAhead::Flow f;
		f.halfW = halfW;
		f.halfH = halfH;
		f.gridW = (halfW + 3) / 4;
		f.gridH = (halfH + 3) / 4;
		const size_t cells = static_cast<size_t>(f.gridW) * static_cast<size_t>(f.gridH);
		f.forward.resize(cells * 2);
		f.backward.resize(cells * 2);
		f.forwardCost.resize(cells);
		f.backwardCost.resize(cells);

		const int lowY = (f.gridH * 7) / 20;
		const int splitY = (f.gridH * 11) / 20;
		const int splitX = f.gridW / 2;
		const int occMidY = splitY + (f.gridH - splitY) / 2;

		// Units: 1 engine cell = 4 half-size px = 128 S10.5 units (0.5 cell = 64 units).
		const int fwdDomX = 384 + static_cast<int>(seed % 5) * 64;             // +3.0 .. +5.0 cells
		const int fwdDomY = -192 - static_cast<int>((seed / 5) % 4) * 64;      // -1.5 .. -3.0 cells
		const int bwdDomX = -fwdDomX;
		const int bwdDomY = -fwdDomY;

		const int fwdSecX = -704 - static_cast<int>(seed % 3) * 64;            // -5.5 .. -6.5 cells
		const int fwdSecY = 384 + static_cast<int>((seed / 3) % 3) * 64;       // +3.0 .. +4.0 cells
		const int bwdSecX = 896 + static_cast<int>(seed % 3) * 64;             // +7.0 .. +8.0 cells (asymmetric)
		const int bwdSecY = -512 - static_cast<int>((seed / 3) % 3) * 64;      // -4.0 .. -5.0 cells (asymmetric)

		for (int y = 0; y < f.gridH; ++y)
		{
			for (int x = 0; x < f.gridW; ++x)
			{
				const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(f.gridW) + static_cast<size_t>(x);
				const uint32_t h1 = Hash2(static_cast<uint32_t>(x) ^ (seed * 17u + 3u), static_cast<uint32_t>(y) ^ (seed * 31u + 7u));
				const uint32_t h2 = Hash2(static_cast<uint32_t>(x) + seed * 13u + 101u, static_cast<uint32_t>(y) + seed * 29u + 53u);
				const int jx = static_cast<int>(h1 % 17u) - 8;                 // [-8..+8] S10.5 = [-0.25..+0.25] half px
				const int jy = static_cast<int>((h1 >> 8) % 17u) - 8;
				const int bjx = static_cast<int>(h2 % 17u) - 8;
				const int bjy = static_cast<int>((h2 >> 8) % 17u) - 8;

				int fx = 0, fy = 0, bx = 0, by = 0;
				uint8_t fc = 10, bc = 10;
				if (y < splitY)
				{
					fx = fwdDomX + jx;
					fy = fwdDomY + jy;
					bx = bwdDomX + bjx;
					by = bwdDomY + bjy;
					const uint8_t baseCost = (y < lowY) ? 10u : 18u;
					fc = static_cast<uint8_t>(baseCost + (h1 & 3u));
					bc = static_cast<uint8_t>(baseCost + (h2 & 3u));
				}
				else if (x < splitX)
				{
					fx = fwdSecX + jx;
					fy = fwdSecY + jy;
					bx = bwdSecX + bjx;
					by = bwdSecY + bjy;
					fc = static_cast<uint8_t>(10u + (h1 & 3u));
					bc = static_cast<uint8_t>(10u + (h2 & 3u));
				}
				else
				{
					// Occluded / high-cost quadrant including vectors beyond +-40 cells (+-5120 S10.5).
					fx = static_cast<int>(h1 % 12801u) - 6400;
					fy = static_cast<int>((h1 >> 11) % 12801u) - 6400;
					bx = static_cast<int>(h2 % 12801u) - 6400;
					by = static_cast<int>((h2 >> 11) % 12801u) - 6400;
					fc = (y < occMidY) ? 35u : 220u;
					bc = (y < occMidY) ? 35u : 220u;
				}
				f.forward[idx * 2] = static_cast<int16_t>(fx);
				f.forward[idx * 2 + 1] = static_cast<int16_t>(fy);
				f.backward[idx * 2] = static_cast<int16_t>(bx);
				f.backward[idx * 2 + 1] = static_cast<int16_t>(by);
				f.forwardCost[idx] = fc;
				f.backwardCost[idx] = bc;
			}
		}
		return f;
	}

	// RAII persistent 2-wide vote runner mirroring Producer::VoteWorker / Producer::RunFlowGlobalsParallel:
	// caller runs forward FlowGlobal while persistent worker thread runs backward FlowGlobal concurrently.
	class TwoWideVoteRunner
	{
	public:
		TwoWideVoteRunner()
		{
			worker_ = std::thread([this] { Loop(); });
		}
		~TwoWideVoteRunner()
		{
			{
				std::lock_guard<std::mutex> lock(mutex_);
				stop_ = true;
			}
			workCv_.notify_all();
			if (worker_.joinable())
				worker_.join();
		}
		TwoWideVoteRunner(const TwoWideVoteRunner&) = delete;
		TwoWideVoteRunner& operator=(const TwoWideVoteRunner&) = delete;

		void RunGlobalsAndExtras(MotionAhead::Flow& flow, bool general)
		{
			{
				std::lock_guard<std::mutex> lock(mutex_);
				flow_ = &flow;
				general_ = general;
				pending_ = true;
			}
			workCv_.notify_one();
			MotionAhead::FlowGlobal(flow.forward, flow.forwardCost, flow.gridW, flow.gridH, flow.global[0], flow.second[0], general);
			{
				std::unique_lock<std::mutex> lock(mutex_);
				doneCv_.wait(lock, [this] { return !pending_; });
			}
			flow.globalSet = true;
			flow.globalGeneral = general;
			MotionAhead::FlowExtras(flow);
		}

	private:
		void Loop()
		{
			for (;;)
			{
				MotionAhead::Flow* flow = nullptr;
				bool general = true;
				{
					std::unique_lock<std::mutex> lock(mutex_);
					workCv_.wait(lock, [this] { return stop_ || pending_; });
					if (stop_ && !pending_)
						return;
					flow = flow_;
					general = general_;
				}
				if (flow)
					MotionAhead::FlowGlobal(flow->backward, flow->backwardCost, flow->gridW, flow->gridH,
						flow->global[1], flow->second[1], general);
				{
					std::lock_guard<std::mutex> lock(mutex_);
					flow_ = nullptr;
					pending_ = false;
				}
				doneCv_.notify_one();
			}
		}

		std::mutex mutex_;
		std::condition_variable workCv_;
		std::condition_variable doneCv_;
		std::thread worker_;
		MotionAhead::Flow* flow_ = nullptr;
		bool general_ = true;
		bool pending_ = false;
		bool stop_ = false;
	};

	// Verifies that a Flow published by Producer::BackWorker (which invokes the private
	// Producer::RunFlowGlobalsParallel + FlowExtras) is bit-identical to serial FlowGlobals + FlowExtras.
	inline bool VerifyFlowGlobalsAndExtrasMatchSerial(const MotionAhead::Flow& produced, std::string& mismatch)
	{
		if (!produced.globalSet)
		{
			mismatch = "producer published Flow with globalSet == false";
			return false;
		}
		MotionAhead::Flow serial;
		serial.halfW = produced.halfW;
		serial.halfH = produced.halfH;
		serial.gridW = produced.gridW;
		serial.gridH = produced.gridH;
		serial.forward = produced.forward;
		serial.backward = produced.backward;
		serial.forwardCost = produced.forwardCost;
		serial.backwardCost = produced.backwardCost;
		MotionAhead::FlowGlobals(serial, produced.globalGeneral);
		MotionAhead::FlowExtras(serial);

		if (serial.blocksX != produced.blocksX || serial.blocksY != produced.blocksY)
		{
			mismatch = "blocksX/blocksY mismatch between parallel producer and serial FlowExtras";
			return false;
		}
		if (std::memcmp(serial.global, produced.global, sizeof(serial.global)) != 0)
		{
			mismatch = "global[2][4] mismatch between Producer::RunFlowGlobalsParallel and serial FlowGlobals";
			return false;
		}
		if (std::memcmp(serial.second, produced.second, sizeof(serial.second)) != 0)
		{
			mismatch = "second[2][4] mismatch between Producer::RunFlowGlobalsParallel and serial FlowGlobals";
			return false;
		}
		if (serial.costWeight.size() != produced.costWeight.size() ||
			(!serial.costWeight.empty() &&
				std::memcmp(serial.costWeight.data(), produced.costWeight.data(), serial.costWeight.size() * sizeof(float)) != 0))
		{
			mismatch = "costWeight mismatch between parallel producer and serial FlowExtras";
			return false;
		}
		if (serial.fbError.size() != produced.fbError.size() ||
			(!serial.fbError.empty() &&
				std::memcmp(serial.fbError.data(), produced.fbError.data(), serial.fbError.size() * sizeof(float)) != 0))
		{
			mismatch = "fbError mismatch between parallel producer and serial FlowExtras";
			return false;
		}
		return true;
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
				std::string mismatch;
				if (!VerifyFlowGlobalsAndExtrasMatchSerial(*flow, mismatch))
				{
					why = "pair " + std::to_string(nextCollect - 1) + "->" + std::to_string(nextCollect) + ": " + mismatch;
					producer.Stop();
					return false;
				}
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
	BEGIN_TEST_METHOD_ATTRIBUTE(EngineInputHashPerFrameMatchesBetweenSerialAndHalfLumaPool)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
	END_TEST_METHOD_ATTRIBUTE()
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
			Assert::AreEqual(serialBuf.size(), pooledBuf.size());
			const uint64_t hSerial = Fnv(1469598103934665603ull, serialBuf.data(), serialBuf.size());
			const uint64_t hPooled = Fnv(1469598103934665603ull, pooledBuf.data(), pooledBuf.size());
			Assert::AreEqual(hSerial, hPooled, L"engine-input luma hash must be byte-identical");
			Assert::AreEqual(0, std::memcmp(serialBuf.data(), pooledBuf.data(), serialBuf.size()),
				L"engine-input luma buffer must be byte-identical");
		}

		// Also exercise halfH >= 256 (960x540 -> halfH = 270) so HalfLumaPool::Run actually dispatches
		// across worker threads rather than taking the totalRows < 256 single-thread fast path.
		const Canvas wideCanvas = MakeCanvas(960, 540);
		for (int f = 0; f < 60; ++f)
		{
			const AnalysisLumaSource src = FrameOf(wideCanvas, static_cast<uint64_t>(f));
			Assert::IsTrue(MotionAhead::HalfLuma8(src, serialBuf, sw, sh, nullptr));
			Assert::IsTrue(MotionAhead::HalfLuma8(src, pooledBuf, pw, ph, &pool));
			Assert::AreEqual(480, sw);
			Assert::AreEqual(270, sh);
			Assert::AreEqual(sw, pw);
			Assert::AreEqual(sh, ph);
			Assert::AreEqual(serialBuf.size(), pooledBuf.size());
			const uint64_t hSerial = Fnv(1469598103934665603ull, serialBuf.data(), serialBuf.size());
			const uint64_t hPooled = Fnv(1469598103934665603ull, pooledBuf.data(), pooledBuf.size());
			Assert::AreEqual(hSerial, hPooled, L"multi-threaded HalfLumaPool hash must be byte-identical");
			Assert::AreEqual(0, std::memcmp(serialBuf.data(), pooledBuf.data(), serialBuf.size()),
				L"multi-threaded HalfLumaPool buffer must be byte-identical");
		}

		// Same-input negative control: perturbing one P010 pixel in a copy of frame 0 changes the hash and bytes.
		{
			Canvas perturbed = wideCanvas;
			perturbed.words = std::make_shared<std::vector<uint16_t>>(*wideCanvas.words);
			const size_t probeIdx = static_cast<size_t>(wideCanvas.frameH / 2) * static_cast<size_t>(wideCanvas.canvasW) +
				static_cast<size_t>(wideCanvas.frameW / 2);
			const uint16_t origCode = static_cast<uint16_t>((*perturbed.words)[probeIdx] >> 6);
			const uint16_t shiftedCode = origCode > 512u ? static_cast<uint16_t>(origCode - 64u) : static_cast<uint16_t>(origCode + 64u);
			(*perturbed.words)[probeIdx] = static_cast<uint16_t>(shiftedCode << 6);
			const AnalysisLumaSource baseSrc = FrameOf(wideCanvas, 0);
			const AnalysisLumaSource pertSrc = FrameOf(perturbed, 0);
			Assert::IsTrue(MotionAhead::HalfLuma8(baseSrc, serialBuf, sw, sh, nullptr));
			Assert::IsTrue(MotionAhead::HalfLuma8(pertSrc, pooledBuf, pw, ph, &pool));
			const uint64_t hBase = Fnv(1469598103934665603ull, serialBuf.data(), serialBuf.size());
			const uint64_t hPert = Fnv(1469598103934665603ull, pooledBuf.data(), pooledBuf.size());
			Assert::AreNotEqual(hBase, hPert, L"negative control: single-pixel perturbation must change engine-input hash");
			Assert::AreNotEqual(0, std::memcmp(serialBuf.data(), pooledBuf.data(), serialBuf.size()),
				L"negative control: single-pixel perturbation must change engine-input bytes");
		}
		pool.Stop();
	}

	BEGIN_TEST_METHOD_ATTRIBUTE(TwoWideParallelFlowGlobalsAndExtrasMatchesSerialBitIdentically)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(TwoWideParallelFlowGlobalsAndExtrasMatchesSerialBitIdentically)
	{
		using namespace ProducerPipelineIdentity;
		TwoWideVoteRunner parallelRunner;

		struct CaseSpec { int halfW; int halfH; uint32_t seed; bool general; };
		std::vector<CaseSpec> cases;
		for (uint32_t s = 0; s < 24; ++s)
			cases.push_back(CaseSpec{ 320, 180, s, (s & 1u) == 0u });
		// Edge-block odd grid dimensions and >2160-line height (8 * gridH = 2400 > 2160) where general scales FlowVoteRange.
		cases.push_back(CaseSpec{ 326, 182, 101u, true });
		cases.push_back(CaseSpec{ 326, 182, 102u, false });
		cases.push_back(CaseSpec{ 240, 1200, 201u, true });
		cases.push_back(CaseSpec{ 240, 1200, 202u, false });
		Assert::IsTrue(MotionAhead::FlowVoteRange(300, true) > MotionAhead::FlowVoteRange(300, false),
			L"high-res gridH=300 must exercise distinct general=true vs general=false vote ranges");

		for (const CaseSpec& c : cases)
		{
			const MotionAhead::Flow baseFlow = MakeMultiQuadrantFlow(c.halfW, c.halfH, c.seed);
			MotionAhead::Flow serialFlow = baseFlow;
			MotionAhead::Flow parallelFlow = baseFlow;

			MotionAhead::FlowGlobals(serialFlow, c.general);
			MotionAhead::FlowExtras(serialFlow);
			parallelRunner.RunGlobalsAndExtras(parallelFlow, c.general);

			// Non-triviality invariants: prove every output field carries real varied signal, never vacuous zeros.
			for (int d = 0; d < 2; ++d)
			{
				Assert::IsTrue(serialFlow.global[d][3] > 0.5f && serialFlow.global[d][2] < 0.9f,
					L"dominant global layer must be valid and usable");
				Assert::IsTrue(std::fabs(serialFlow.global[d][0]) >= 2.0f && std::fabs(serialFlow.global[d][1]) >= 1.0f,
					L"dominant global motion must be non-zero");
				Assert::IsTrue(serialFlow.second[d][3] > 0.5f,
					L"second motion layer must be populated");
				Assert::IsTrue(std::fabs(serialFlow.second[d][0] - serialFlow.global[d][0]) >= 2.0f,
					L"second layer must be distinct from dominant global layer");
			}
			Assert::IsTrue(serialFlow.global[0][0] > 0.0f && serialFlow.global[1][0] < 0.0f,
				L"forward and backward global motion must be distinct");
			Assert::IsTrue(!serialFlow.costWeight.empty() && !serialFlow.fbError.empty(),
				L"costWeight and fbError must be populated");

			bool sawTrusted = false, sawSmooth = false, sawZero = false;
			for (float w : serialFlow.costWeight)
			{
				if (w >= 0.9999f) sawTrusted = true;
				else if (w <= 0.0001f) sawZero = true;
				else sawSmooth = true;
			}
			Assert::IsTrue(sawTrusted && sawSmooth && sawZero,
				L"costWeight must span trusted (1.0), smoothstep (0, 1), and untrusted (0.0) regimes");

			bool sawConsistent = false, sawInconsistent = false;
			for (float e : serialFlow.fbError)
			{
				if (e < 0.5f) sawConsistent = true;
				if (e > 2.0f) sawInconsistent = true;
			}
			Assert::IsTrue(sawConsistent && sawInconsistent,
				L"fbError must span both consistent (<0.5 px) and inconsistent (>2.0 px) cells");

			// Bit-for-bit identity between 2-wide parallel FlowGlobal + FlowExtras and serial FlowGlobals + FlowExtras.
			Assert::AreEqual(serialFlow.globalSet, parallelFlow.globalSet);
			Assert::AreEqual(serialFlow.globalGeneral, parallelFlow.globalGeneral);
			Assert::AreEqual(serialFlow.blocksX, parallelFlow.blocksX);
			Assert::AreEqual(serialFlow.blocksY, parallelFlow.blocksY);
			Assert::AreEqual(0, std::memcmp(serialFlow.global, parallelFlow.global, sizeof(serialFlow.global)),
				L"2-wide parallel global[2][4] must match serial bit-for-bit");
			Assert::AreEqual(0, std::memcmp(serialFlow.second, parallelFlow.second, sizeof(serialFlow.second)),
				L"2-wide parallel second[2][4] must match serial bit-for-bit");
			Assert::AreEqual(serialFlow.costWeight.size(), parallelFlow.costWeight.size());
			Assert::AreEqual(0, std::memcmp(serialFlow.costWeight.data(), parallelFlow.costWeight.data(),
				serialFlow.costWeight.size() * sizeof(float)),
				L"2-wide parallel costWeight must match serial bit-for-bit");
			Assert::AreEqual(serialFlow.fbError.size(), parallelFlow.fbError.size());
			Assert::AreEqual(0, std::memcmp(serialFlow.fbError.data(), parallelFlow.fbError.data(),
				serialFlow.fbError.size() * sizeof(float)),
				L"2-wide parallel fbError must match serial bit-for-bit");
			Assert::AreEqual(HashFlow(serialFlow), HashFlow(parallelFlow),
				L"full P2 payload hash must match bit-for-bit between 2-wide parallel and serial");
		}

		// Same-input negative controls on the exact same multi-quadrant flow (prove mismatches are detected).
		const MotionAhead::Flow baseFlow = MakeMultiQuadrantFlow(320, 180, 7u);
		MotionAhead::Flow serialRef = baseFlow;
		MotionAhead::FlowGlobals(serialRef, true);
		MotionAhead::FlowExtras(serialRef);
		const uint64_t refHash = HashFlow(serialRef);

		// Negative control 1a: perturbing a single forward vector cell must produce non-zero fbError memcmp & hash diff.
		{
			MotionAhead::Flow singleVec = baseFlow;
			const size_t probeCell = static_cast<size_t>(singleVec.gridW) + 1u;
			singleVec.forward[probeCell * 2] = static_cast<int16_t>(singleVec.forward[probeCell * 2] + 64);
			MotionAhead::FlowGlobals(singleVec, true);
			MotionAhead::FlowExtras(singleVec);
			Assert::AreNotEqual(0, std::memcmp(serialRef.fbError.data(), singleVec.fbError.data(),
				serialRef.fbError.size() * sizeof(float)),
				L"negative control: single-cell vector perturbation must change fbError");
			Assert::AreNotEqual(refHash, HashFlow(singleVec),
				L"negative control: single-cell vector perturbation must change HashFlow");
		}

		// Negative control 1b: shifting dominant cluster vectors must change global[0] and HashFlow.
		{
			MotionAhead::Flow shiftedVec = baseFlow;
			const int lowY = (shiftedVec.gridH * 7) / 20;
			for (int y = 0; y < lowY; ++y)
				for (int x = 0; x < shiftedVec.gridW; ++x)
				{
					const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(shiftedVec.gridW) + static_cast<size_t>(x);
					shiftedVec.forward[idx * 2] = static_cast<int16_t>(shiftedVec.forward[idx * 2] + 128);
				}
			MotionAhead::FlowGlobals(shiftedVec, true);
			MotionAhead::FlowExtras(shiftedVec);
			Assert::AreNotEqual(0, std::memcmp(serialRef.global, shiftedVec.global, sizeof(serialRef.global)),
				L"negative control: dominant cluster shift must change global[2][4]");
			Assert::AreNotEqual(refHash, HashFlow(shiftedVec),
				L"negative control: dominant cluster shift must change HashFlow");
		}

		// Negative control 2: perturbing forwardCost on one trusted 2x2 cell block must change costWeight & HashFlow.
		{
			MotionAhead::Flow perturbedCost = baseFlow;
			for (int cy = 0; cy < 2; ++cy)
				for (int cx = 0; cx < 2; ++cx)
					perturbedCost.forwardCost[static_cast<size_t>(cy) * static_cast<size_t>(perturbedCost.gridW) + static_cast<size_t>(cx)] = 250u;
			MotionAhead::FlowGlobals(perturbedCost, true);
			MotionAhead::FlowExtras(perturbedCost);
			Assert::AreNotEqual(0, std::memcmp(serialRef.costWeight.data(), perturbedCost.costWeight.data(),
				serialRef.costWeight.size() * sizeof(float)),
				L"negative control: block cost perturbation must change costWeight");
			Assert::AreNotEqual(refHash, HashFlow(perturbedCost),
				L"negative control: block cost perturbation must change HashFlow");
		}

		// Negative control 3: broken 2-wide dispatch that erroneously votes forward into both lanes must fail memcmp & hash.
		{
			MotionAhead::Flow brokenParallel = baseFlow;
			MotionAhead::FlowGlobal(brokenParallel.forward, brokenParallel.forwardCost, brokenParallel.gridW, brokenParallel.gridH,
				brokenParallel.global[0], brokenParallel.second[0], true);
			MotionAhead::FlowGlobal(brokenParallel.forward, brokenParallel.forwardCost, brokenParallel.gridW, brokenParallel.gridH,
				brokenParallel.global[1], brokenParallel.second[1], true);
			brokenParallel.globalSet = true;
			brokenParallel.globalGeneral = true;
			MotionAhead::FlowExtras(brokenParallel);
			Assert::AreNotEqual(0, std::memcmp(serialRef.global, brokenParallel.global, sizeof(serialRef.global)),
				L"negative control: broken 2-wide forward/backward channel bug must change global[2][4]");
			Assert::AreNotEqual(0, std::memcmp(serialRef.second, brokenParallel.second, sizeof(serialRef.second)),
				L"negative control: broken 2-wide forward/backward channel bug must change second[2][4]");
			Assert::AreNotEqual(refHash, HashFlow(brokenParallel),
				L"negative control: broken 2-wide channel bug must change HashFlow");
		}
	}

	BEGIN_TEST_METHOD_ATTRIBUTE(P2HashesSerialEqualsPipelined600FramesTwoRunsHintsOnAndOff)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(P2HashesSerialEqualsPipelined600FramesTwoRunsHintsOnAndOff)
	{
		using namespace ProducerPipelineIdentity;
		CComPtr<ID3D11Device> device;
		if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu,
				"P2HashesSerialEqualsPipelined600FramesTwoRunsHintsOnAndOff",
				NvidiaDevice(device),
				"requires an NVIDIA D3D11 adapter with NVOF"))
			return;
		const Canvas canvas = MakeCanvas(640, 360);
		constexpr int kFrames = 600;
		for (int hintsInt = 0; hintsInt <= 1; ++hintsInt)
		{
			const bool hints = hintsInt != 0;
			std::vector<uint64_t> serialA, serialB, pipeA, pipeB;
			std::string why;
			if (!CollectHashes(device, canvas, kFrames, false, hints, serialA, why))
			{
				if (hintsInt == 0 &&
					(why.find("nvofapi64.dll") != std::string::npos || why.find("no optical flow engine") != std::string::npos))
				{
					if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu,
							"P2HashesSerialEqualsPipelined600FramesTwoRunsHintsOnAndOff",
							false,
							"NVOF unavailable (" + why + ")"))
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
