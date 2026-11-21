#include <pch.h>
#include "MotionAheadProducer.h"

#include <DebugLog.h>
#include "ProducerJobs.h"
#include <VP-V/core/PassTimerInbox.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>
#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/GpuAttribution.h>

#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#pragma comment(lib, "d3d11.lib")

namespace MotionAhead
{
	namespace
	{
		uint32_t BigEndian32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }
		uint32_t LittleEndian32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
		// Motion only needs the structure: luma as (R + 2G + B) / 4 in 10-bit codes.
		inline uint16_t LumaOf(unsigned r, unsigned g, unsigned b) { return static_cast<uint16_t>((r + 2 * g + b + 2) >> 2); }
	}

	void HalfLumaPool::Start(int workers)
	{
		Stop();
		if (workers <= 0)
			return;
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = false;
		totalWorkers_ = workers + 1;
		for (int i = 0; i < workers; ++i)
		{
			const int workerIdx = i + 1;
			threads_.emplace_back([this, workerIdx] {
				uint64_t seen = 0;
				for (;;)
				{
					const std::function<void(int, int)>* fn = nullptr;
					int rows = 0;
					int total = 1;
					{
						std::unique_lock<std::mutex> lk(mutex_);
						workCv_.wait(lk, [this, seen] { return stop_ || generation_ != seen; });
						if (stop_)
							return;
						seen = generation_;
						fn = fn_;
						rows = totalRows_;
						total = totalWorkers_;
					}
					if (fn && rows > 0)
					{
						const int chunk = (rows + total - 1) / total;
						const int fromY = (std::min)(rows, workerIdx * chunk);
						const int toY = (std::min)(rows, (workerIdx + 1) * chunk);
						if (fromY < toY)
							(*fn)(fromY, toY);
					}
					std::lock_guard<std::mutex> lk(mutex_);
					if (--remaining_ == 0)
						doneCv_.notify_one();
				}
			});
		}
	}

	void HalfLumaPool::Stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
		}
		workCv_.notify_all();
		for (std::thread& t : threads_)
			if (t.joinable()) t.join();
		threads_.clear();
		std::lock_guard<std::mutex> lock(mutex_);
		fn_ = nullptr;
		remaining_ = 0;
	}

	void HalfLumaPool::Run(int totalRows, const std::function<void(int, int)>& fn)
	{
		if (threads_.empty() || totalRows < 256)
		{
			fn(0, totalRows);
			return;
		}
		int total = 1;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			fn_ = &fn;
			totalRows_ = totalRows;
			remaining_ = static_cast<int>(threads_.size());
			total = totalWorkers_;
			++generation_;
		}
		workCv_.notify_all();
		const int chunk = (totalRows + total - 1) / total;
		fn(0, (std::min)(totalRows, chunk));
		std::unique_lock<std::mutex> lock(mutex_);
		doneCv_.wait(lock, [this] { return remaining_ == 0 || stop_; });
		fn_ = nullptr;
	}

	bool HalfLuma8(const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& halfW, int& halfH, HalfLumaPool* pool)
	{
		if (!source.IsValid()) return false;
		const int w = source.width, h = source.height;
		halfW = (w + 1) / 2;
		halfH = (h + 1) / 2;
		out.resize(static_cast<size_t>(halfW) * halfH);
		// A row of 10-bit luma codes, decoded directly for the capture formats (P010 /
		// P210 planes, v210, UYVY / HDYC, R12B, R12L, r210 / R10b / R10l, 8-bit RGB);
		// the sampler only for anything else.
		const AnalysisLumaFormat format = source.format;
		const VideoFrameEncoding encoding = source.encoding;
		const bool planar16 = format == AnalysisLumaFormat::P010 || format == AnalysisLumaFormat::P210;
		const bool yuv422 = format == AnalysisLumaFormat::NativeYuv422;
		const bool rgb = format == AnalysisLumaFormat::NativeRgb;
		// WP-23: the frame's shared luma decode (Sample().luma) replaces this decode where the
		// codes are the same - native 4:2:2 at limited range, and the sampler's own formats
		// (packed / decoded DV, SDR tunnel). RGB keeps its (R + 2G + B) / 4 structure luma and
		// full-range 4:2:2 its raw codes (the shared plane holds mapped codes): vectors unchanged.
		const bool rgbOwnDecode = rgb && (encoding == VideoFrameEncoding::R12B || encoding == VideoFrameEncoding::R12L ||
			encoding == VideoFrameEncoding::R210 || encoding == VideoFrameEncoding::R10b || encoding == VideoFrameEncoding::R10l ||
			encoding == VideoFrameEncoding::BGRA_8BIT || encoding == VideoFrameEncoding::ARGB_8BIT);
		const bool sharedSameCodes = !planar16 && !rgbOwnDecode && !(yuv422 && source.fullRangeYuv);
		auto decodeRow = [&](int y, uint16_t* codes) {
			if (sharedSameCodes)
				if (const uint16_t* shared = source.SharedLumaRow(y))
				{
					std::memcpy(codes, shared, static_cast<size_t>(w) * sizeof(uint16_t));
					return;
				}
			const uint8_t* row = source.data + static_cast<size_t>(y) * source.rowBytes;
			if (planar16)
			{
				const uint16_t* words = reinterpret_cast<const uint16_t*>(row);
				for (int x = 0; x < w; ++x) codes[x] = static_cast<uint16_t>(words[x] >> 6);
				return;
			}
			if (yuv422 && encoding == VideoFrameEncoding::V210)
			{
				// Six pixels per 16 bytes: Y0 = w0>>10, Y1 = w1, Y2 = w1>>20, Y3 = w2>>10, Y4 = w3, Y5 = w3>>20.
				for (int x = 0; x < w; x += 6)
				{
					uint32_t word[4];
					std::memcpy(word, row + static_cast<size_t>(x / 6) * 16, 16);
					const uint16_t y6[6] = {
						static_cast<uint16_t>((word[0] >> 10) & 0x3ff), static_cast<uint16_t>(word[1] & 0x3ff),
						static_cast<uint16_t>((word[1] >> 20) & 0x3ff), static_cast<uint16_t>((word[2] >> 10) & 0x3ff),
						static_cast<uint16_t>(word[3] & 0x3ff), static_cast<uint16_t>((word[3] >> 20) & 0x3ff) };
					for (int k = 0; k < 6 && x + k < w; ++k) codes[x + k] = y6[k];
				}
				return;
			}
			if (yuv422)   // UYVY / HDYC, 8-bit
			{
				for (int x = 0; x < w; ++x) codes[x] = static_cast<uint16_t>(row[static_cast<size_t>(x / 2) * 4 + ((x & 1) ? 3 : 1)] << 2);
				return;
			}
			if (rgb && encoding == VideoFrameEncoding::R12B)
			{
				// Eight pixels per 36 bytes (nine big-endian-ordered words), 12-bit RGB.
				for (int x = 0; x < w; x += 8)
				{
					const uint8_t* bk = row + static_cast<size_t>(x / 8) * 36;
					auto B = [bk](int word, int index) -> unsigned { return bk[word * 4 + index]; };
					auto L = [&B](int word, int index) -> unsigned { return B(word, index) & 0x0f; };
					auto Hh = [&B](int word, int index) -> unsigned { return B(word, index) >> 4; };
					const unsigned rgb8[8][3] = {
						{ B(0, 3) | (L(0, 2) << 8), Hh(0, 2) | (B(0, 1) << 4), B(0, 0) | (L(1, 3) << 8) },
						{ Hh(1, 3) | (B(1, 2) << 4), B(1, 1) | (L(1, 0) << 8), Hh(1, 0) | (B(2, 3) << 4) },
						{ B(2, 2) | (L(2, 1) << 8), Hh(2, 1) | (B(2, 0) << 4), B(3, 3) | (L(3, 2) << 8) },
						{ Hh(3, 2) | (B(3, 1) << 4), B(3, 0) | (L(4, 3) << 8), Hh(4, 3) | (B(4, 2) << 4) },
						{ B(4, 1) | (L(4, 0) << 8), Hh(4, 0) | (B(5, 3) << 4), B(5, 2) | (L(5, 1) << 8) },
						{ Hh(5, 1) | (B(5, 0) << 4), B(6, 3) | (L(6, 2) << 8), Hh(6, 2) | (B(6, 1) << 4) },
						{ B(6, 0) | (L(7, 3) << 8), Hh(7, 3) | (B(7, 2) << 4), B(7, 1) | (L(7, 0) << 8) },
						{ Hh(7, 0) | (B(8, 3) << 4), B(8, 2) | (L(8, 1) << 8), Hh(8, 1) | (B(8, 0) << 4) } };
					for (int k = 0; k < 8 && x + k < w; ++k)
						codes[x + k] = static_cast<uint16_t>(LumaOf(rgb8[k][0], rgb8[k][1], rgb8[k][2]) >> 2);   // 12 -> 10 bits
				}
				return;
			}
			if (rgb && encoding == VideoFrameEncoding::R12L)
			{
				for (int x = 0; x < w; x += 2)
				{
					const uint8_t* pr = row + static_cast<size_t>(x / 2) * 9;
					const unsigned r0 = pr[0] | ((pr[1] & 0x0f) << 8), g0 = (pr[1] >> 4) | (pr[2] << 4), b0 = pr[3] | ((pr[4] & 0x0f) << 8);
					const unsigned r1 = (pr[4] >> 4) | (pr[5] << 4), g1 = pr[6] | ((pr[7] & 0x0f) << 8), b1 = (pr[7] >> 4) | (pr[8] << 4);
					codes[x] = static_cast<uint16_t>(LumaOf(r0, g0, b0) >> 2);
					if (x + 1 < w) codes[x + 1] = static_cast<uint16_t>(LumaOf(r1, g1, b1) >> 2);
				}
				return;
			}
			if (rgb && (encoding == VideoFrameEncoding::R210 || encoding == VideoFrameEncoding::R10b || encoding == VideoFrameEncoding::R10l))
			{
				const int shift = encoding == VideoFrameEncoding::R210 ? 0 : 2;
				for (int x = 0; x < w; ++x)
				{
					const uint8_t* px = row + static_cast<size_t>(x) * 4;
					const uint32_t word = encoding == VideoFrameEncoding::R10l ? LittleEndian32(px) : BigEndian32(px);
					codes[x] = LumaOf((word >> (20 + shift)) & 0x3ff, (word >> (10 + shift)) & 0x3ff, (word >> shift) & 0x3ff);
				}
				return;
			}
			if (rgb && (encoding == VideoFrameEncoding::BGRA_8BIT || encoding == VideoFrameEncoding::ARGB_8BIT))
			{
				const bool bgra = encoding == VideoFrameEncoding::BGRA_8BIT;
				for (int x = 0; x < w; ++x)
				{
					const uint8_t* px = row + static_cast<size_t>(x) * 4;
					const unsigned r = bgra ? px[2] : px[1], g = bgra ? px[1] : px[2], b = bgra ? px[0] : px[3];
					codes[x] = static_cast<uint16_t>(LumaOf(r, g, b) << 2);
				}
				return;
			}
			AnalysisLumaSample sample;   // anything else: the sampler
			for (int x = 0; x < w; ++x)
				codes[x] = source.Sample(x, y, sample) ? sample.luma : 0;
		};
		// Rows in parallel (4K RGB at 60 frames: several ms per frame on one core).
		auto rows = [&](int fromY, int toY) {
			std::vector<uint16_t> r0(static_cast<size_t>(w)), r1(static_cast<size_t>(w));
			for (int y = fromY; y < toY; ++y)
			{
				decodeRow(VpUnit::HalfLuma::Tap0(y, h), r0.data());
				decodeRow(VpUnit::HalfLuma::Tap1(y, h), r1.data());
				uint8_t* dst = &out[static_cast<size_t>(y) * halfW];
				for (int x = 0; x < halfW; ++x)
				{
					// The 2x2 box of the shared half-luma unit (VpUnit::HalfLuma, U3) with its engine-view finish.
					const int x0 = VpUnit::HalfLuma::Tap0(x, w), x1 = VpUnit::HalfLuma::Tap1(x, w);
					dst[x] = VpUnit::HalfLuma::FinishEngine8(VpUnit::HalfLuma::Box4(r0[x0], r0[x1], r1[x0], r1[x1]));   // the mean code / 4
				}
			}
		};
		if (pool && pool->Active())
		{
			pool->Run(halfH, rows);
			return true;
		}
		const unsigned cores = (std::max)(1u, std::thread::hardware_concurrency());
		const int threads = halfH >= 256 ? static_cast<int>((std::min)(4u, (std::max)(1u, cores / 2))) : 1;
		if (threads <= 1)
			rows(0, halfH);
		else
		{
			std::vector<std::thread> poolThreads;
			const int chunk = (halfH + threads - 1) / threads;
			for (int t = 1; t < threads; ++t)
				poolThreads.emplace_back(rows, (std::min)(halfH, t * chunk), (std::min)(halfH, (t + 1) * chunk));
			rows(0, (std::min)(halfH, chunk));
			for (std::thread& th : poolThreads) th.join();
		}
		return true;
	}

	Producer::Producer() = default;

	Producer::~Producer()
	{
		Stop();
	}

	bool Producer::Start(ID3D11Device* renderDevice, NvOpticalFlow::Preset presetIn)
	{
		Stop();
		std::lock_guard<std::mutex> lock(mutex);
		error.clear();
		preset = presetIn;
		if (!renderDevice) { error = "no render device"; return false; }
		// A device of its own on the same adapter (the engine runs beside the render).
		CComPtr<IDXGIDevice> dxgiDevice;
		CComPtr<IDXGIAdapter> adapter;
		if (FAILED(renderDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) ||
			FAILED(dxgiDevice->GetAdapter(&adapter)))
		{
			error = "cannot find the render GPU";
			return false;
		}
		// Owner 2026-10-16 (the startup queue fill): the device is created on the worker, so a
		// start never stalls the render thread (driver calls can take tens of ms).
		device.Release();
		context.Release();
		stopping = false;
		jobs.clear();
		requests.FailAll(OnDemand::Status::Unavailable);   // W2-C5b (Stop answered them already; never a broken promise)
		testEngine.reset();
		havePrevious = false;
		engineReady = false;
		sizeW = sizeH = 0;
		consecutiveFailures = 0;
		requestRunning = requestOwnsGeneration = false;
		lastJobMs = 0;
		adoptOverWalk = requestResetHints = false;
		recovery = ProducerRecovery();
		stopError.clear();
		slot->Clear();   // a fresh producer starts from an empty P2 slot (the pairs of an older one are not its own)
		backQueue.Reopen();
		publishGate.Reset();
		renderAdapter = adapter;
		running = true;
		StartPipelineThreads();
		worker = std::thread([this] {
			std::string why;
			if (!CreateDevice(why))
			{
				std::lock_guard<std::mutex> lock(mutex);
				error = why;
				running = false;
				requestStats.unavailable += requests.FailAll(OnDemand::Status::Unavailable);   // W2-C5b: never a broken promise
				return;
			}
			Worker();
		});
		return true;
	}

	void Producer::StartPipelineThreads()
	{
		frontPool.Start(2);
		{
			std::lock_guard<std::mutex> lock(voteMutex);
			voteFlow = nullptr;
			votePending = false;
			voteStop = false;
		}
		voteThread = std::thread([this] { VoteWorker(); });
		backThread = std::thread([this] { BackWorker(); });
	}

	void Producer::StopPipelineThreads()
	{
		backQueue.Close();
		if (backThread.joinable())
			backThread.join();
		{
			std::lock_guard<std::mutex> lock(voteMutex);
			voteStop = true;
		}
		voteWorkCv.notify_all();
		if (voteThread.joinable())
			voteThread.join();
		frontPool.Stop();
	}

	void Producer::VoteWorker()
	{
		for (;;)
		{
			Flow* flow = nullptr;
			bool general = true;
			{
				std::unique_lock<std::mutex> lock(voteMutex);
				voteWorkCv.wait(lock, [this] { return voteStop || votePending; });
				if (voteStop && !votePending)
					return;
				flow = voteFlow;
				general = voteGeneral;
			}
			if (flow)
				FlowGlobal(flow->backward, flow->backwardCost, flow->gridW, flow->gridH, flow->global[1], flow->second[1], general);
			{
				std::lock_guard<std::mutex> lock(voteMutex);
				voteFlow = nullptr;
				votePending = false;
			}
			voteDoneCv.notify_one();
		}
	}

	void Producer::RunFlowGlobalsParallel(Flow& flow, bool general)
	{
		if (voteThread.joinable())
		{
			{
				std::lock_guard<std::mutex> lock(voteMutex);
				voteFlow = &flow;
				voteGeneral = general;
				votePending = true;
			}
			voteWorkCv.notify_one();
			FlowGlobal(flow.forward, flow.forwardCost, flow.gridW, flow.gridH, flow.global[0], flow.second[0], general);
			std::unique_lock<std::mutex> lock(voteMutex);
			voteDoneCv.wait(lock, [this] { return !votePending; });
			flow.globalSet = true;
			flow.globalGeneral = general;
		}
		else
		{
			FlowGlobals(flow, general);
		}
	}

	void Producer::BackWorker()
	{
		Pipeline::BackQueue<BackItem>::Entry entry;
		while (backQueue.Pop(entry))
		{
			backQueue.MarkBusy(true);
			BackItem& item = entry.item;
			const bool liveStart = backQueue.IsLive(entry.generation, entry.epoch, entry.ignoreGeneration);
			if (!liveStart || !item.flow)
			{
				if (item.isRequest && item.pendingRequest.state)
				{
					OnDemand::PairResult res;
					res.status = OnDemand::Status::Unavailable;
					res.queuedMs = item.requestQueuedMs;
					res.runMs = OnDemand::MsBetween(item.requestTaken, OnDemand::Clock::now());
					Answer(item.pendingRequest, std::move(res));
				}
				backQueue.MarkBusy(false);
				continue;
			}
			const auto voteStart = std::chrono::steady_clock::now();
			VPV_COMPUTE_ONCE_AT("U10", item.toSeq, 0);
			RunFlowGlobalsParallel(*item.flow, item.generalRates);
			const auto extrasStart = std::chrono::steady_clock::now();
			FlowExtras(*item.flow);
			const auto extrasEnd = std::chrono::steady_clock::now();
			const double voteMs = std::chrono::duration<double, std::milli>(extrasStart - voteStart).count();
			const double flowExtrasMs = std::chrono::duration<double, std::milli>(extrasEnd - extrasStart).count();
			const double combinedExtrasMs = voteMs + flowExtrasMs;

			if (!backQueue.IsLive(entry.generation, entry.epoch, entry.ignoreGeneration))
			{
				if (item.isRequest && item.pendingRequest.state)
				{
					OnDemand::PairResult res;
					res.status = OnDemand::Status::Unavailable;
					res.queuedMs = item.requestQueuedMs;
					res.runMs = OnDemand::MsBetween(item.requestTaken, OnDemand::Clock::now());
					Answer(item.pendingRequest, std::move(res));
				}
				backQueue.MarkBusy(false);
				continue;
			}

			if (!item.isRequest)
			{
				if (publishGate.Accept(item.generation, item.fromSeq, item.toSeq))
				{
					slot->Publish(item.fromSeq, item.toSeq, item.generation, item.flow);   // P2: ordered publish
					const auto published = std::chrono::steady_clock::now();
					item.stage.voteMs = voteMs;
					item.stage.extrasMs = flowExtrasMs;
					item.stage.serviceMs = item.stage.lumaMs + item.stage.engineMs + item.stage.mapMs + combinedExtrasMs;
					item.stage.submitToPublishMs = item.submitTime.time_since_epoch().count() != 0 ?
						(std::max)(0.0, std::chrono::duration<double, std::milli>(published - item.submitTime).count()) : item.stage.serviceMs;
					engineReady = true;
					std::lock_guard<std::mutex> lock(mutex);
					++stats.pairs;
					stats.extrasMs = combinedExtrasMs;
					stats.lastPairMs = item.stage.serviceMs;
					stats.pairMsSum += stats.lastPairMs;
					stats.RecordStage(item.stage);
				}
			}
			else
			{
				if (item.publishToSlot && !item.forceOffChain)
					slot->Publish(item.fromSeq, item.toSeq, item.generation, item.flow);
				engineReady = true;
				OnDemand::PairResult res;
				res.status = OnDemand::Status::Ready;
				res.flow = item.flow;
				res.queuedMs = item.requestQueuedMs;
				res.runMs = OnDemand::MsBetween(item.requestTaken, OnDemand::Clock::now());
				Answer(item.pendingRequest, std::move(res));
			}
			backQueue.MarkBusy(false);
		}
	}

	bool Producer::CreateDevice(std::string& why)
	{
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		CComPtr<IDXGIAdapter> workerAdapter;
		{
			std::lock_guard<std::mutex> lock(mutex);
			workerAdapter = renderAdapter;
		}
		CComPtr<ID3D11Device> created;
		CComPtr<ID3D11DeviceContext> createdContext;
		if (!workerAdapter || FAILED(D3D11CreateDevice(workerAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
				&created, nullptr, &createdContext)))
		{
			why = "cannot create its D3D11 device";
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex);
		device = created;
		context = createdContext;
		return true;
	}

	// The engine first (it unregisters its textures), then the textures.
	void Producer::ReleaseEngine()
	{
		for (int k = 0; k < kStagingSets; ++k)
		{
			FailStagingSetRequest(k, OnDemand::Status::Unavailable);
			ReleaseStagingSetMeta(k);
		}
		stagingRing.UnbindEngineThread();
		stagingRing.Clear();
		engine.reset();
		VpSkeleton::VramUsers::Instance().Withdraw(this, VpSkeleton::kUserNvofProducer);   // MV7: the ring textures go with the engine
		VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserNvofProducer, false);
		for (int i = 0; i < 2; ++i)
		{
			flowTex[i].Release(); costTex[i].Release();
		}
		for (int k = 0; k < kStagingSets; ++k)
		{
			for (int i = 0; i < 2; ++i)
			{
				stagingSets[k].flowRead[i].Release();
				stagingSets[k].costRead[i].Release();
			}
			stagingSets[k].eventQuery.Release();
		}
		for (int i = 0; i < kLumaRingSlots; ++i)
			lumaTex[i].Release();
		lumaRing.Clear();
		ReleaseRequestTextures();   // W2-C5b
		sizeW = sizeH = 0;
		havePrevious = false;
		hintSequence = UINT64_MAX;
	}

	uint64_t Producer::NowMs()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	Producer::Health Producer::GetHealth() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		Health h;
		h.recovering = recovery.Recovering();
		h.stops = recovery.Stops();
		h.restarts = recovery.Restarts();
		h.retryInMs = recovery.RemainingMs(NowMs());
		h.error = stopError;
		return h;
	}

	void Producer::Stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			jobs.clear();
			requestStats.unavailable += requests.FailAll(OnDemand::Status::Unavailable);   // W2-C5b: every waiting request answered
		}
		backQueue.Close();   // wake BACK and unblock any Push on ENGINE before joining
		wake.notify_all();
		if (worker.joinable()) worker.join();
		StopPipelineThreads();
		running = false;
		// The engine first (it unregisters its textures), then the textures, then the device.
		ReleaseEngine();
		context.Release();
		device.Release();
		std::lock_guard<std::mutex> lock(mutex);
		renderAdapter.Release();
	}

	std::string Producer::Error() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return error;
	}

	Producer::Stats Producer::GetStats() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return stats;
	}

	void Producer::Submit(uint64_t sequence, uint64_t generation, const AnalysisLumaSource& luma, bool breakBefore)
	{
		if (!running.load(std::memory_order_acquire)) return;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping) return;
			// Never let the work pile up: at most a few frames waiting (the oldest dropped).
			while (jobs.size() >= maxPendingJobs.load(std::memory_order_acquire))
			{
				jobs.pop_front();
				++stats.dropped;
				if (!requests.Empty() || requestRunning)   // W2-C5b: dropped while requests were served - never silent
				{
					const unsigned long long n = ++requestStats.droppedBehindRequests;
					if (n == 1 || n % 10 == 0)
						DebugLog::Log("Motion ahead: %llu look-ahead job(s) dropped while on-demand requests were served", n);
				}
			}
			jobs.push_back(Job{ sequence, generation, luma, breakBefore, std::chrono::steady_clock::now() });
		}
		lastSubmitted.store(sequence, std::memory_order_release);
		wake.notify_all();
	}

	void Producer::Worker()
	{
		stagingRing.BindEngineThread();
		struct ThreadGuard
		{
			Producer* p;
			~ThreadGuard()
			{
				p->DrainStagingSets(true);
				p->stagingRing.UnbindEngineThread();
			}
		} guard{ this };

		for (;;)
		{
			// 1. Poll any completed staging sets (non-blocking GetData(DONOTFLUSH)) and forward to BACK.
			if (!testEngine && context && stagingRing.SubmittedCount() > 0)
			{
				bool anyCompleted = false;
				if (!PollReadyStagingSets(anyCompleted))
				{
					std::unique_lock<std::mutex> lock(mutex);
					++stats.failures;
					if (++consecutiveFailures >= 3)
					{
						lock.unlock();
						DrainStagingSets(true);
						backQueue.DiscardAllAndBumpEpoch();
						backQueue.WaitIdle();
						publishGate.Reset();
						lock.lock();
						running = false;
						engineReady = false;
						jobs.clear();
						requestStats.unavailable += requests.FailAll(OnDemand::Status::Unavailable);
						stopError = error.empty() ? std::string("three failed jobs in a row") : error;
						for (;;)
						{
							const uint64_t delay = recovery.OnStopped(NowMs());
							const unsigned divisor = recoveryDivisor.load(std::memory_order_acquire);
							error = stopError + "; restarting in " + std::to_string((delay + 999) / 1000) + " s (stop " +
								std::to_string(recovery.Stops()) + ")";
							DebugLog::Log("Motion ahead: the producer stopped (%s)", error.c_str());
							if (wake.wait_for(lock, std::chrono::milliseconds(delay / divisor), [this] { return stopping; }))
								return;
							lock.unlock();
							ReleaseEngine();
							stagingRing.BindEngineThread();
							{
								std::lock_guard<std::mutex> deviceLock(mutex);
								context.Release();
								device.Release();
							}
							std::string why;
							const bool created = CreateDevice(why);
							lock.lock();
							if (stopping) return;
							if (created)
								break;
							stopError = why;
						}
						jobs.clear();
						requestStats.unavailable += requests.FailAll(OnDemand::Status::Unavailable);
						consecutiveFailures = 0;
						recovery.OnRestarted(NowMs());
						error.clear();
						DebugLog::Log("Motion ahead: the producer restarted on a new device (restart %u after: %s)",
							recovery.Restarts(), stopError.c_str());
						running = true;
					}
					continue;
				}
			}

			Job job;
			OnDemand::Pending request;   // W2-C5b: an on-demand request, taken after the waiting look-ahead (core) jobs
			bool isRequest = false;
			bool hasWork = false;
			{
				std::unique_lock<std::mutex> lock(mutex);
				const bool canSubmit = testEngine || stagingRing.AcquireFree() != Pipeline::StagingRing<kStagingSets>::kNoSlot;
				if (!stopping && !(canSubmit && (!requests.Empty() || !jobs.empty())))
				{
					if (!testEngine && stagingRing.SubmittedCount() > 0)
					{
						// Wait on whichever comes first: new job (if a staging slot is free) or oldest GPU event.
						// Spin-then-sleep: brief polls before a short timed wait so we neither stall nor burn CPU.
						lock.unlock();
						bool completed = false;
						for (int spin = 0; spin < 8 && !completed; ++spin)
						{
							if (!PollReadyStagingSets(completed))
								break;
							if (!completed)
								std::this_thread::yield();
						}
						if (completed)
							continue;
						lock.lock();
						if (!stopping && !(canSubmit && (!requests.Empty() || !jobs.empty())))
							wake.wait_for(lock, std::chrono::microseconds(500), [this, canSubmit] {
								return stopping || (canSubmit && (!requests.Empty() || !jobs.empty()));
							});
						continue;
					}
					wake.wait(lock, [this] { return stopping || !requests.Empty() || !jobs.empty(); });
				}
				if (stopping) return;
				if (!canSubmit)
					continue;
				// One pure chooser (ProducerJobs.h, 105 s3.3): core pair > on-demand request > long pair (register 115 #57).
				ProducerJobs::Queues queues;
				queues.corePairs = jobs.size();
				queues.onDemand = requests.Size();
				const ProducerJobs::Choice choice = ProducerJobs::NextJob(queues);
				if (!choice.any)
					continue;   // woken with nothing to do: never touch an empty queue
				if (choice.cls == ProducerJobs::JobClass::OnDemand)
				{
					isRequest = requests.Pop(request);
					requestRunning = isRequest;
					if (!isRequest)
						continue;
					hasWork = true;
				}
				else
				{
					if (choice.requestWaited)
						++requestStats.coreAheadOfRequests;
					job = std::move(jobs.front());
					jobs.pop_front();
					hasWork = true;
				}
			}
			if (!hasWork)
				continue;
			// An exception escaping the worker would end the process: counted as a failure instead.
			// Three failures IN A ROW stop the producer (audit 2026-10-16: three over a whole session did).
			bool ok = false;
			bool engineRan = true;   // W2-C5b: a request answered without engine work leaves the failure count alone
			try { ok = isRequest ? ProcessRequest(request, engineRan) : testEngine ? ProcessJobForTest(job) : Process(job); }
			catch (...) { ok = false; }
			if (isRequest && request.state && !request.state->fulfilled.load(std::memory_order_acquire))
			{
				OnDemand::PairResult failed;   // an exception before the answer: never a broken promise
				failed.status = OnDemand::Status::Failed;
				Answer(request, failed);
			}
			std::unique_lock<std::mutex> lock(mutex);
			requestRunning = false;
			if (ok)
			{
				if (engineRan)
					consecutiveFailures = 0;
				continue;
			}
			++stats.failures;
			if (++consecutiveFailures < 3)
				continue;
			// Persistent failure: stop (the matcher covers everything meanwhile). Bug hunt 35 #1: no
			// longer for the session - restart with a back-off on a fresh device while still wanted.
			lock.unlock();
			DrainStagingSets(true);
			backQueue.DiscardAllAndBumpEpoch();
			backQueue.WaitIdle();
			publishGate.Reset();
			lock.lock();
			running = false;
			engineReady = false;
			jobs.clear();
			requestStats.unavailable += requests.FailAll(OnDemand::Status::Unavailable);   // W2-C5b
			stopError = error.empty() ? std::string("three failed jobs in a row") : error;
			for (;;)
			{
				const uint64_t delay = recovery.OnStopped(NowMs());
				const unsigned divisor = recoveryDivisor.load(std::memory_order_acquire);
				error = stopError + "; restarting in " + std::to_string((delay + 999) / 1000) + " s (stop " +
					std::to_string(recovery.Stops()) + ")";
				DebugLog::Log("Motion ahead: the producer stopped (%s)", error.c_str());
				if (wake.wait_for(lock, std::chrono::milliseconds(delay / divisor), [this] { return stopping; }))
					return;
				lock.unlock();
				// The old engine, textures and device go (a removed device cannot recover in place).
				ReleaseEngine();
				stagingRing.BindEngineThread();
				{
					std::lock_guard<std::mutex> deviceLock(mutex);
					context.Release();
					device.Release();
				}
				std::string why;
				const bool created = CreateDevice(why);
				lock.lock();
				if (stopping) return;
				if (created)
					break;
				stopError = why;   // no device yet: another stop, a longer wait
			}
			jobs.clear();
			requestStats.unavailable += requests.FailAll(OnDemand::Status::Unavailable);   // W2-C5b
			consecutiveFailures = 0;
			recovery.OnRestarted(NowMs());
			error.clear();   // the next stop names its own failure
			DebugLog::Log("Motion ahead: the producer restarted on a new device (restart %u after: %s)",
				recovery.Restarts(), stopError.c_str());
			running = true;
		}
	}

	bool Producer::EnsureSize(int halfW, int halfH)
	{
		if (halfW == sizeW && halfH == sizeH && engine && engine->IsOpen()) return true;
		// Drain all in-flight staging sets and wait for BACK before destroying the old staging textures.
		DrainStagingSets(false);
		backQueue.WaitIdle();
		stagingRing.Clear();
		engine.reset();
		VpSkeleton::VramUsers::Instance().Withdraw(this, VpSkeleton::kUserNvofProducer);   // MV7: re-declared below for the new size
		VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserNvofProducer, true);   // an open that fails below leaves it active without a declaration: shown by the skeleton
		for (int i = 0; i < 2; ++i)
		{
			flowTex[i].Release(); costTex[i].Release();
		}
		for (int k = 0; k < kStagingSets; ++k)
		{
			for (int i = 0; i < 2; ++i)
			{
				stagingSets[k].flowRead[i].Release();
				stagingSets[k].costRead[i].Release();
			}
			stagingSets[k].eventQuery.Release();
			ReleaseStagingSetMeta(k);
		}
		for (int i = 0; i < kLumaRingSlots; ++i)
			lumaTex[i].Release();
		lumaRing.Clear();
		havePrevious = false;
		const int gw = (halfW + 3) / 4, gh = (halfH + 3) / 4;
		auto make = [&](int tw, int th, DXGI_FORMAT format, bool staging, CComPtr<ID3D11Texture2D>& tex) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = static_cast<UINT>(tw); desc.Height = static_cast<UINT>(th);
			desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = format; desc.SampleDesc.Count = 1;
			desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
			desc.BindFlags = staging ? 0 : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
			desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
			return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex));
		};
		for (int i = 0; i < kLumaRingSlots; ++i)
			if (!make(halfW, halfH, NvOpticalFlow::Engine::InputFormat(), false, lumaTex[i]))
			{
				std::lock_guard<std::mutex> lock(mutex);
				error = "cannot create its textures";
				return false;
			}
		for (int i = 0; i < 2; ++i)
			if (!make(gw, gh, NvOpticalFlow::Engine::FlowFormat(), false, flowTex[i]) ||
				!make(gw, gh, NvOpticalFlow::Engine::CostFormat(), false, costTex[i]))
			{
				std::lock_guard<std::mutex> lock(mutex);
				error = "cannot create its textures";
				return false;
			}
		D3D11_QUERY_DESC qd{};
		qd.Query = D3D11_QUERY_EVENT;
		for (int k = 0; k < kStagingSets; ++k)
		{
			for (int i = 0; i < 2; ++i)
				if (!make(gw, gh, NvOpticalFlow::Engine::FlowFormat(), true, stagingSets[k].flowRead[i]) ||
					!make(gw, gh, NvOpticalFlow::Engine::CostFormat(), true, stagingSets[k].costRead[i]))
				{
					std::lock_guard<std::mutex> lock(mutex);
					error = "cannot create its textures";
					return false;
				}
			if (FAILED(device->CreateQuery(&qd, &stagingSets[k].eventQuery)))
			{
				std::lock_guard<std::mutex> lock(mutex);
				error = "cannot create its event query";
				return false;
			}
		}
		engine = std::make_unique<NvOpticalFlow::Engine>();
		if (!engine->Open(device, context, halfW, halfH, preset, true))
		{
			std::lock_guard<std::mutex> lock(mutex);
			error = engine->LastError();
			engine.reset();
			return false;
		}
		sizeW = halfW;
		sizeH = halfH;
		// MV7: the ring textures (2 sets) are this producer's VRAM; the governor counts them (VramUsers.h). The readback staging copies are system RAM: not counted.
		// The engine-luma ring holds kLumaRingSlots luma textures (RingBytes counts two): the extra ones are declared here too.
		VpSkeleton::VramUsers::Instance().DeclareFixed(this, VpSkeleton::kUserNvofProducer,
			NvOpticalFlow::RingBytes(halfW, halfH, false, false) + static_cast<uint64_t>(kLumaRingSlots - 2) * static_cast<uint64_t>(halfW) * static_cast<uint64_t>(halfH));
		return true;
	}

	bool Producer::Process(const Job& job)
	{
		const auto started = std::chrono::steady_clock::now();
		const double queueWaitMs = job.submitTime.time_since_epoch().count() != 0 ?
			(std::max)(0.0, std::chrono::duration<double, std::milli>(started - job.submitTime).count()) : 0.0;
		int halfW = 0, halfH = 0;
		if (!LumaAtTarget(job.sequence, job.luma, lumaBuffer, halfW, halfH, "motion_ahead"))
			return true;   // not a readable frame, or a size this producer does not make: nothing published (the consumer takes the matcher path, shown as a fallback)
		const double lumaMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (!EnsureSize(halfW, halfH)) return false;
		ReleaseStaleRequestTextures(halfW, halfH);   // W2-C5b
		// A new continuity generation clears the P2 slot and the chain.
		// Re-check S1: never back below a generation a no-walk request already started (it would wipe the request's pair).
		if (requestOwnsGeneration && job.generation < slot->Generation())
		{
			std::lock_guard<std::mutex> lock(mutex);
			++requestStats.staleJobs;
			return true;
		}
		requestOwnsGeneration = false;
		if (slot->Generation() != job.generation)
		{
			stagingRing.MarkDiscardedOtherThan(job.generation);
			backQueue.SetActiveGeneration(job.generation);
			backQueue.WaitIdle();
			slot->SetGeneration(job.generation);
			publishGate.SetGeneration(job.generation);
			havePrevious = false;
			lumaRing.ClearGenerationsOtherThan(job.generation);   // a new generation: no frame of the old one stays (pins too)
		}
		// A break the sequence does not show: no pair across it (MotionValidity step 1 F).
		if (job.breakBefore)
			havePrevious = false;
		NoteWalkJob(job);   // W2-C5b
		// The engine-luma ring (105 s3.5): this frame's slot by key; the pair's first frame is looked up by key, never "the other one".
		// The chain says (N-1, N) is a pair but the ring lost N-1: counted and named (register 115 #13 / #35), never a silent skip;
		// no pair, and the chain continues from this frame.
		const PairStart start = FindPairStart(lumaRing, havePrevious, previousGeneration, previousSequence, job.generation, job.sequence);
		if (start.Lost())
			NoteRingMiss(PairStartLostText());
		const int prevSlot = start.slot;
		const int texIndex = lumaRing.Acquire(job.generation, job.sequence);
		if (texIndex == EngineLumaRing::kNoSlot)
		{
			NoteRingMiss("the engine-luma ring could not hold the frame");
			return false;
		}
		if (!WaitForLumaSlotFree(texIndex))
			return false;
		context->UpdateSubresource(lumaTex[texIndex], 0, nullptr, lumaBuffer.data(), static_cast<UINT>(halfW), 0);
		bool ok = true;
		if (prevSlot != EngineLumaRing::kNoSlot)
		{
			const int prev = prevSlot;
			// The temporal hints only when the engine's last pair ended at this pair's first frame
			// (audit 2026-10-16: they carried across seeks, dropped jobs and size skips).
			const bool continuous = temporalHintsForTest.load(std::memory_order_acquire) &&
				hintSequence == previousSequence && hintGeneration == job.generation;
			hintSequence = UINT64_MAX;
			bool ran = false;
			int setIdx = Pipeline::StagingRing<kStagingSets>::kNoSlot;
			ok = SubmitPairAsync(lumaTex[prev], lumaTex[texIndex], halfW, halfH, continuous, job.sequence, ran, setIdx);
			if (ran)
			{
				hintSequence = job.sequence;
				hintGeneration = job.generation;
			}
			if (ok && setIdx != Pipeline::StagingRing<kStagingSets>::kNoSlot)
			{
				StagingSet& s = stagingSets[setIdx];
				s.halfW = halfW;
				s.halfH = halfH;
				s.lumaSlotA = prev;
				s.lumaSlotB = texIndex;
				s.usesRequestTex = false;
				s.fromSeq = previousSequence;
				s.toSeq = job.sequence;
				s.generation = job.generation;
				s.epoch = backQueue.Epoch();
				s.isRequest = false;
				s.publishToSlot = true;
				s.forceOffChain = false;
				s.generalRates = generalRates.load(std::memory_order_acquire);
				s.submitTime = job.submitTime;
				s.workStartTime = started;
				s.queueWaitMs = queueWaitMs;
				s.lumaMs = lumaMs;
				const Pipeline::JobKey key{ job.generation, previousSequence, job.sequence, false, job.breakBefore };
				stagingRing.MarkSubmitted(setIdx, key, s.epoch, std::chrono::steady_clock::now());
			}
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			++stats.frames;
			stats.lumaMs = lumaMs;
		}
		havePrevious = true;
		previousSequence = job.sequence;
		previousGeneration = job.generation;
		return ok;
	}
	bool Producer::LumaAtTarget(uint64_t sequence, const AnalysisLumaSource& luma, std::vector<uint8_t>& buffer, int& halfW, int& halfH,
		const char* privateSite)
	{
		// The engine half: the skeleton's P1 (one decode per frame, Acquire from this thread) when it has the frame and the
		// codes are the engine's; else the producer's own decode (RGB / full-range: FIX-B4-RGB; no source or a miss: shown).
		std::shared_ptr<VpSkeleton::IP1Source> p1;
		{
			std::lock_guard<std::mutex> lock(p1Mutex);
			p1 = p1Source;
		}
		VpSkeleton::P1ReadKind how = VpSkeleton::P1ReadKind::None;
		if (!(p1 && luma.IsValid() && p1->ReadEngine8(sequence, luma, buffer, halfW, halfH, how) && how != VpSkeleton::P1ReadKind::None))
		{
			if (!HalfLuma8(luma, buffer, halfW, halfH, &frontPool))
				return false;   // not a readable frame: nothing to do, not a failure
			// GF-P4 MUST-12: U3 is a private copy only when the producer decoded the half luma itself (a shared P1 read is not).
			VPV_COMPUTE_ONCE_PRIVATE_AT("U3", sequence, privateSite);
		}
		// The de-judder's engine size: the half size, or half again (its inner matcher).
		const int tw = targetW.load(std::memory_order_acquire), th = targetH.load(std::memory_order_acquire);
		if (tw > 0 && th > 0 && !(tw == halfW && th == halfH))
		{
			if (tw != (halfW + 1) / 2 || th != (halfH + 1) / 2)
				return false;   // a size this producer does not make: nothing published (the consumer takes the matcher path, shown as a fallback)
			quarterBuffer.resize(static_cast<size_t>(tw) * th);
			VpLaneProducts::Engine8Quarter(buffer.data(), static_cast<uint32_t>(halfW), static_cast<uint32_t>(halfH), quarterBuffer.data());   // the shared quarter law
			buffer.swap(quarterBuffer);
			halfW = tw;
			halfH = th;
		}
		return true;
	}

	void Producer::ReleaseStagingSetMeta(int setIndex)
	{
		if (setIndex < 0 || setIndex >= kStagingSets)
			return;
		StagingSet& s = stagingSets[setIndex];
		s.halfW = s.halfH = 0;
		s.lumaSlotA = s.lumaSlotB = EngineLumaRing::kNoSlot;
		s.usesRequestTex = false;
		s.fromSeq = s.toSeq = s.generation = s.epoch = 0;
		s.isRequest = s.publishToSlot = s.forceOffChain = false;
		s.queueWaitMs = s.lumaMs = s.requestQueuedMs = 0.0;
		s.pendingRequest = OnDemand::Pending{};
	}

	void Producer::FailStagingSetRequest(int setIndex, OnDemand::Status status)
	{
		if (setIndex < 0 || setIndex >= kStagingSets)
			return;
		StagingSet& s = stagingSets[setIndex];
		if (s.isRequest && s.pendingRequest.state && !s.pendingRequest.state->fulfilled.load(std::memory_order_acquire))
		{
			OnDemand::PairResult res;
			res.status = status;
			res.queuedMs = s.requestQueuedMs;
			res.runMs = OnDemand::MsBetween(s.requestTaken, OnDemand::Clock::now());
			Answer(s.pendingRequest, std::move(res));
		}
	}

	bool Producer::SubmitPairAsync(ID3D11Texture2D* a, ID3D11Texture2D* b, int halfW, int halfH, bool continuous, uint64_t timedSequence,
		bool& ran, int& outSetIndex)
	{
		ran = false;
		outSetIndex = Pipeline::StagingRing<kStagingSets>::kNoSlot;
		stagingRing.AssertEngineThread();
		VPV_COMPUTE_ONCE_AT("nvof.pair", timedSequence, 0);
		int setIdx = stagingRing.AcquireFree();
		if (setIdx == Pipeline::StagingRing<kStagingSets>::kNoSlot)
		{
			const int oldest = stagingRing.OldestSubmitted();
			if (oldest == Pipeline::StagingRing<kStagingSets>::kNoSlot || !DrainStagingSets(false, oldest))
				return false;
			setIdx = stagingRing.AcquireFree();
			if (setIdx == Pipeline::StagingRing<kStagingSets>::kNoSlot)
				return false;
		}
		// Slice 16 P2 (105 s3.6): steady path never drains inside NvOpticalFlow::Engine::Run (nullptr timing);
		// wall time is measured non-blockingly from submit -> stagingSets[setIdx].eventQuery signalling S_OK.
		if (!engine->Run(a, b, flowTex[0], flowTex[1], costTex[0], costTex[1], nullptr, continuous))
		{
			std::lock_guard<std::mutex> lock(mutex);
			error = engine->LastError();
			return false;
		}
		ran = true;
		StagingSet& s = stagingSets[setIdx];
		for (int i = 0; i < 2; ++i)
		{
			context->CopyResource(s.flowRead[i], flowTex[i]);
			context->CopyResource(s.costRead[i], costTex[i]);
		}
		context->End(s.eventQuery);
		context->Flush();
		outSetIndex = setIdx;
		(void)halfW;
		(void)halfH;
		return true;
	}

	bool Producer::MapAndForwardStagingSet(int setIndex, double engineMs)
	{
		stagingRing.AssertEngineThread();
		StagingSet& s = stagingSets[setIndex];
		const auto mapStart = std::chrono::steady_clock::now();
		auto flow = std::make_shared<Flow>();
		flow->halfW = s.halfW;
		flow->halfH = s.halfH;
		flow->gridW = (s.halfW + 3) / 4;
		flow->gridH = (s.halfH + 3) / 4;
		const size_t cells = static_cast<size_t>(flow->gridW) * flow->gridH;
		for (int i = 0; i < 2; ++i)
		{
			std::vector<int16_t>& vectors = i == 0 ? flow->forward : flow->backward;
			std::vector<uint8_t>& costs = i == 0 ? flow->forwardCost : flow->backwardCost;
			vectors.resize(cells * 2);
			costs.resize(cells);
			D3D11_MAPPED_SUBRESOURCE map{};
			if (FAILED(context->Map(s.flowRead[i], 0, D3D11_MAP_READ, 0, &map)))
				return false;
			for (int y = 0; y < flow->gridH; ++y)
				std::memcpy(&vectors[static_cast<size_t>(y) * flow->gridW * 2],
					static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
					static_cast<size_t>(flow->gridW) * 4);
			context->Unmap(s.flowRead[i], 0);
			if (FAILED(context->Map(s.costRead[i], 0, D3D11_MAP_READ, 0, &map)))
				return false;
			for (int y = 0; y < flow->gridH; ++y)
				std::memcpy(&costs[static_cast<size_t>(y) * flow->gridW],
					static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
					static_cast<size_t>(flow->gridW));
			context->Unmap(s.costRead[i], 0);
		}
		const double mapMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mapStart).count();

		if (PassTimers::Sampler().Take(s.toSeq) && engineMs > 0.0)
			PassTimers::Inbox::Motion().Post(PassTimers::Kind::GpuGroup, "motion: NVOF producer engine (wall)", engineMs);
		if (engineMs > 0.0)
			GpuAttribution::NoteExternalTimed("nvof execute (wall)", "motion_engine_nvof",
				static_cast<uint64_t>(engineMs * 1e6 + 0.5));

		BackItem item;
		item.flow = std::move(flow);
		item.fromSeq = s.fromSeq;
		item.toSeq = s.toSeq;
		item.generation = s.generation;
		item.epoch = s.epoch;
		item.isRequest = s.isRequest;
		item.publishToSlot = s.publishToSlot;
		item.forceOffChain = s.forceOffChain;
		item.generalRates = s.generalRates;
		item.submitTime = s.submitTime;
		item.workStartTime = s.workStartTime;
		item.stage.queueWaitMs = s.queueWaitMs;
		item.stage.lumaMs = s.lumaMs;
		item.stage.engineMs = engineMs;
		item.stage.mapMs = mapMs;
		item.pendingRequest = std::move(s.pendingRequest);
		item.requestTaken = s.requestTaken;
		item.requestQueuedMs = s.requestQueuedMs;

		const uint64_t gen = item.generation;
		const uint64_t ep = item.epoch;
		const bool ignoreGen = item.forceOffChain;
		backQueue.Push(std::move(item), gen, ep, ignoreGen);
		return true;
	}

	bool Producer::PollReadyStagingSets(bool& anyCompleted)
	{
		anyCompleted = false;
		if (!context)
			return true;
		stagingRing.AssertEngineThread();
		for (;;)
		{
			const int oldest = stagingRing.OldestSubmitted();
			if (oldest == Pipeline::StagingRing<kStagingSets>::kNoSlot)
				return true;
			StagingSet& s = stagingSets[oldest];
			const HRESULT hr = s.eventQuery ? context->GetData(s.eventQuery, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) : S_OK;
			if (hr == S_FALSE)
				return true;
			if (FAILED(hr))
			{
				FailStagingSetRequest(oldest, OnDemand::Status::Failed);
				ReleaseStagingSetMeta(oldest);
				stagingRing.Release(oldest);
				std::lock_guard<std::mutex> lock(mutex);
				error = "staging event query failed";
				return false;
			}
			const auto now = std::chrono::steady_clock::now();
			const auto& slotMeta = stagingRing.At(oldest);
			const double engineMs = (std::max)(0.0, std::chrono::duration<double, std::milli>(now - slotMeta.submittedAt).count());
			const bool discarded = slotMeta.discarded;
			stagingRing.MarkReady(oldest);
			bool ok = true;
			if (discarded)
			{
				FailStagingSetRequest(oldest, OnDemand::Status::Late);
			}
			else
			{
				ok = MapAndForwardStagingSet(oldest, engineMs);
				if (!ok)
					FailStagingSetRequest(oldest, OnDemand::Status::Failed);
			}
			ReleaseStagingSetMeta(oldest);
			stagingRing.Release(oldest);
			anyCompleted = true;
			if (!ok)
				return false;
		}
	}

	bool Producer::DrainStagingSets(bool discard, int targetSlot)
	{
		if (!context)
		{
			for (int k = 0; k < kStagingSets; ++k)
			{
				if (targetSlot == -1 || targetSlot == k)
				{
					FailStagingSetRequest(k, OnDemand::Status::Unavailable);
					ReleaseStagingSetMeta(k);
				}
			}
			if (targetSlot == -1)
				stagingRing.Clear();
			return true;
		}
		stagingRing.AssertEngineThread();
		if (discard)
			stagingRing.MarkAllDiscarded();
		for (;;)
		{
			if (targetSlot != -1 && stagingRing.At(targetSlot).state == Pipeline::SlotState::Free)
				return true;
			if (targetSlot == -1 && stagingRing.SubmittedCount() == 0)
				return true;
			bool any = false;
			if (!PollReadyStagingSets(any))
				return false;
			if (targetSlot != -1 && stagingRing.At(targetSlot).state == Pipeline::SlotState::Free)
				return true;
			if (targetSlot == -1 && stagingRing.SubmittedCount() == 0)
				return true;
			if (!any)
				std::this_thread::yield();
		}
	}

	bool Producer::WaitForLumaSlotFree(int lumaSlot)
	{
		if (lumaSlot == EngineLumaRing::kNoSlot || stagingRing.SubmittedCount() == 0)
			return true;
		for (int k = 0; k < kStagingSets; ++k)
		{
			if (stagingRing.At(k).state == Pipeline::SlotState::Submitted &&
				(stagingSets[k].lumaSlotA == lumaSlot || stagingSets[k].lumaSlotB == lumaSlot))
			{
				if (!DrainStagingSets(false, k))
					return false;
			}
		}
		return true;
	}

	bool Producer::WaitForRequestTexFree()
	{
		if (stagingRing.SubmittedCount() == 0)
			return true;
		for (int k = 0; k < kStagingSets; ++k)
		{
			if (stagingRing.At(k).state == Pipeline::SlotState::Submitted && stagingSets[k].usesRequestTex)
			{
				if (!DrainStagingSets(false, k))
					return false;
			}
		}
		return true;
	}

	// ---- W2-C5b: on-demand requests (OnDemandEstimate.h) ---------------------------------------------------------------

	bool Producer::EnsureRequestTextures(int w, int h)
	{
		if (requestTex[0] && requestTex[1] && requestTexW == w && requestTexH == h) return true;
		ReleaseRequestTextures();
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<UINT>(w); desc.Height = static_cast<UINT>(h);
		desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = NvOpticalFlow::Engine::InputFormat(); desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;   // as the chain's luma textures
		for (int i = 0; i < 2; ++i)
			if (FAILED(device->CreateTexture2D(&desc, nullptr, &requestTex[i])))
			{
				ReleaseRequestTextures();
				std::lock_guard<std::mutex> lock(mutex);
				error = "cannot create its request textures";
				return false;
			}
		requestTexW = w;
		requestTexH = h;
		// Their own declaration (owner key: the pair's address), beside the ring's; made on the first off-chain request only,
		// so a producer that never serves one declares nothing more.
		VpSkeleton::VramUsers::Instance().DeclareFixed(&requestTex[0], VpSkeleton::kUserNvofProducer,
			2ull * static_cast<uint64_t>(w) * static_cast<uint64_t>(h));
		return true;
	}

	void Producer::ReleaseRequestTextures()
	{
		if (requestTex[0] || requestTex[1])
			VpSkeleton::VramUsers::Instance().Withdraw(&requestTex[0], VpSkeleton::kUserNvofProducer);
		requestTex[0].Release();
		requestTex[1].Release();
		requestTexW = requestTexH = 0;
	}

	OnDemand::PairFuture Producer::Request(OnDemand::PairRequest request)
	{
		using OnDemand::Status;
		OnDemand::PairFuture future;
		{
			std::lock_guard<std::mutex> lock(mutex);
			++requestStats.requests;
			if (!running.load(std::memory_order_acquire) || stopping || recovery.Recovering())
			{
				++requestStats.unavailable;
				return OnDemand::PairFuture::Answered(Status::Unavailable);
			}
			if (request.budgetMs <= 0.0)
			{
				++requestStats.noBudget;
				return OnDemand::PairFuture::Answered(Status::NoBudget);
			}
			if (request.to <= request.from)
			{
				++requestStats.refused;
				return OnDemand::PairFuture::Answered(Status::Refused);
			}
			// Compute once: the pair is already in the slot (the walk made it, or an earlier request).
			if (slot->Generation() == request.generation && slot->Has(request.from, request.to))
				if (MotionAhead::FlowPtr flow = slot->Find(request.from, request.to))
				{
					++requestStats.fromSlot;
					return OnDemand::PairFuture::Answered(Status::FromSlot, std::move(flow));
				}
			future = requests.Push(std::move(request), OnDemand::Clock::now(), &requestStats.refused);
		}
		wake.notify_all();
		return future;
	}

	Producer::RequestStats Producer::GetRequestStats() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return requestStats;
	}

	void Producer::Answer(OnDemand::Pending& pending, OnDemand::PairResult result)
	{
		using OnDemand::Status;
		const Status status = result.status;
		const double queuedMs = result.queuedMs, runMs = result.runMs;
		if (!OnDemand::Fulfil(*pending.state, std::move(result)))
			return;
		std::lock_guard<std::mutex> lock(mutex);
		switch (status)
		{
		case Status::Ready: ++requestStats.ready; break;
		case Status::FromSlot: ++requestStats.fromSlot; break;
		case Status::Late: ++requestStats.late; break;
		case Status::Refused: ++requestStats.refused; break;
		case Status::Failed: ++requestStats.failed; break;
		case Status::Unavailable: ++requestStats.unavailable; break;
		case Status::NoBudget: ++requestStats.noBudget; break;
		default: break;
		}
		requestStats.lastQueuedMs = queuedMs;
		if (status == Status::Ready)
		{
			requestStats.lastRunMs = runMs;
			requestStats.runMsSum += runMs;
		}
	}

	bool Producer::ProcessRequest(OnDemand::Pending& pending, bool& engineRan)
	{
		engineRan = false;
		using OnDemand::Status;
		using OnDemand::Mode;
		const auto taken = OnDemand::Clock::now();
		const OnDemand::PairRequest& rq = pending.request;
		OnDemand::PairResult result;
		result.queuedMs = OnDemand::MsBetween(pending.state->issued, taken);
		auto answer = [&](Status status) {
			result.status = status;
			result.runMs = OnDemand::MsBetween(taken, OnDemand::Clock::now());
			Answer(pending, std::move(result));
		};
		// The asker gave up, or the deadline passed while it waited: no GPU work.
		if (pending.state->abandoned.load(std::memory_order_acquire) || taken > pending.state->deadline)
		{
			answer(Status::Late);
			return true;
		}
		// The walk may have made it meanwhile (compute once).
		if (slot->Generation() == rq.generation && slot->Has(rq.from, rq.to))
			if ((result.flow = slot->Find(rq.from, rq.to)) != nullptr)
			{
				answer(Status::FromSlot);
				return true;
			}
		// The generation (audit MUST 1): the stream's generation only moves forward and the walk owns its changes.
		// Older than the slot's: the stream moved on after the request was issued -> Late, no GPU work. Newer while the
		// walk is present: run off the chain (no slot, chain or generation change, nothing published). Newer with no walk
		// (low latency without look-ahead jobs): the request starts the new generation, as a job would.
		const uint64_t slotGeneration = slot->Generation();
		bool walkActive = false;
		{
			std::lock_guard<std::mutex> lock(mutex);
			walkActive = WalkActiveLocked();
			if (rq.generation < slotGeneration)
				++requestStats.staleGeneration;
		}
		if (rq.generation < slotGeneration)
		{
			answer(Status::Late);
			return true;
		}
		const bool forceOffChain = rq.generation > slotGeneration && walkActive;
		int w = 0, h = 0;
		if (!testEngine)
		{
			if (!LumaAtTarget(rq.to, rq.lumaTo, lumaBuffer, w, h, "motion_ahead_request"))
			{
				answer(Status::Failed);   // not a readable frame / a size not made: nothing to run, not an engine failure
				return true;
			}
			engineRan = true;   // EnsureSize may open the engine: from here on a failure is the engine's
			if (!EnsureSize(w, h))
			{
				answer(Status::Failed);
				return false;
			}
			ReleaseStaleRequestTextures(w, h);
		}
		engineRan = true;
		// A newer generation with no walk clears the P2 slot and the chain, exactly as for a job.
		if (rq.generation > slotGeneration && !forceOffChain)
		{
			if (!testEngine)
			{
				stagingRing.MarkDiscardedOtherThan(rq.generation);
				backQueue.SetActiveGeneration(rq.generation);
				backQueue.WaitIdle();
				publishGate.SetGeneration(rq.generation);
			}
			slot->SetGeneration(rq.generation);
			havePrevious = false;
			lumaRing.ClearGenerationsOtherThan(rq.generation);   // a new generation: no frame of the old one stays
			requestOwnsGeneration = true;
		}
		OnDemand::ChainState chain;
		chain.havePrevious = havePrevious;
		chain.previousSequence = previousSequence;
		chain.previousGeneration = previousGeneration;
		chain.slotGeneration = slot->Generation();
		const Mode mode = forceOffChain ? Mode::OffChain : OnDemand::ChooseMode(chain, rq);
		if (forceOffChain)
		{
			std::lock_guard<std::mutex> lock(mutex);
			++requestStats.forcedOffChain;
		}
		if (mode == Mode::Adopt && walkActive)
			adoptOverWalk = true;   // the walk's next job may now sit behind the chain (counted in NoteWalkJob)
		bool ok = true;
		std::shared_ptr<Flow> flow;
		bool ran = false;
		if (testEngine)
		{
			flow = std::make_shared<Flow>();
			ok = testEngine->request ? testEngine->request(rq, mode, *flow) : true;
			ran = ok;
			if (!ok) flow.reset();
			// The chain now holds `to` (Chain / Adopt); an off-chain run leaves it for the walk.
			if (mode != Mode::OffChain)
			{
				havePrevious = true;
				previousSequence = rq.to;
				previousGeneration = rq.generation;
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (mode == Mode::Chain) ++requestStats.chainRuns;
				else if (mode == Mode::Adopt) ++requestStats.adoptRuns;
				else ++requestStats.offChainRuns;
			}
			if (!ok || !flow)
			{
				answer(Status::Failed);
				return false;
			}
			if (rq.Adjacent() && !forceOffChain)
				slot->Publish(rq.from, rq.to, rq.generation, flow);
			engineReady = true;
			result.flow = flow;
			answer(Status::Ready);
			return true;
		}
		if (mode != Mode::Chain)
		{
			int fw = 0, fh = 0;
			if (!LumaAtTarget(rq.from, rq.lumaFrom, requestLumaBuffer, fw, fh, "motion_ahead_request") || fw != w || fh != h)
			{
				engineRan = false;        // nothing ran: leaves the failure count alone
				answer(Status::Failed);   // the two frames are not one size: no pair
				return true;
			}
		}
		ID3D11Texture2D* texFrom = nullptr;
		ID3D11Texture2D* texTo = nullptr;
		int lumaSlotA = EngineLumaRing::kNoSlot;
		int lumaSlotB = EngineLumaRing::kNoSlot;
		bool usesReqTex = false;
		// The engine-luma ring (105 s3.5): frames by key. Chain pairs the chain's last frame with `to`; Adopt puts `from` and `to` in.
		if (mode == Mode::Chain)
		{
			const int fromIndex = lumaRing.Find(previousGeneration, previousSequence);
			const int toIndex = lumaRing.Acquire(rq.generation, rq.to);
			if (fromIndex == EngineLumaRing::kNoSlot || toIndex == EngineLumaRing::kNoSlot || fromIndex == toIndex)
			{
				NoteRingMiss("the engine-luma ring lost the chain's last frame");
				engineRan = false;        // nothing ran: leaves the failure count alone
				answer(Status::Failed);   // the chain's last frame is not held: no pair (cannot happen while the chain is valid)
				return true;
			}
			if (!WaitForLumaSlotFree(toIndex))
			{
				answer(Status::Failed);
				return false;
			}
			context->UpdateSubresource(lumaTex[toIndex], 0, nullptr, lumaBuffer.data(), static_cast<UINT>(w), 0);
			texFrom = lumaTex[fromIndex];
			texTo = lumaTex[toIndex];
			lumaSlotA = fromIndex;
			lumaSlotB = toIndex;
		}
		else if (mode == Mode::Adopt)
		{
			const int fromIndex = lumaRing.Acquire(rq.generation, rq.from);
			const int toIndex = lumaRing.Acquire(rq.generation, rq.to);
			if (fromIndex == EngineLumaRing::kNoSlot || toIndex == EngineLumaRing::kNoSlot || fromIndex == toIndex)
			{
				NoteRingMiss("the engine-luma ring could not hold the adopted pair");
				engineRan = false;
				answer(Status::Failed);
				return true;
			}
			if (!WaitForLumaSlotFree(fromIndex) || !WaitForLumaSlotFree(toIndex))
			{
				answer(Status::Failed);
				return false;
			}
			context->UpdateSubresource(lumaTex[fromIndex], 0, nullptr, requestLumaBuffer.data(), static_cast<UINT>(w), 0);
			context->UpdateSubresource(lumaTex[toIndex], 0, nullptr, lumaBuffer.data(), static_cast<UINT>(w), 0);
			texFrom = lumaTex[fromIndex];
			texTo = lumaTex[toIndex];
			lumaSlotA = fromIndex;
			lumaSlotB = toIndex;
		}
		else
		{
			if (!WaitForRequestTexFree() || !EnsureRequestTextures(w, h))
			{
				answer(Status::Failed);
				return false;
			}
			context->UpdateSubresource(requestTex[0], 0, nullptr, requestLumaBuffer.data(), static_cast<UINT>(w), 0);
			context->UpdateSubresource(requestTex[1], 0, nullptr, lumaBuffer.data(), static_cast<UINT>(w), 0);
			texFrom = requestTex[0];
			texTo = requestTex[1];
			usesReqTex = true;
		}
		// The temporal hints only for an adjacent Chain pair right after the engine's last pair ended at `from`.
		const bool continuous = mode == Mode::Chain && rq.Adjacent() && hintSequence == rq.from && hintGeneration == rq.generation;
		hintSequence = UINT64_MAX;   // any run here moves the engine's internal hint state
		requestResetHints = true;    // a walk pair that would have used them runs without (counted in NoteWalkJob)
		int setIdx = Pipeline::StagingRing<kStagingSets>::kNoSlot;
		ok = SubmitPairAsync(texFrom, texTo, w, h, continuous, rq.to, ran, setIdx);
		if (ran && mode != Mode::OffChain && rq.Adjacent())
		{
			hintSequence = rq.to;
			hintGeneration = rq.generation;
		}
		// The chain now holds `to` (Chain / Adopt); an off-chain run leaves it for the walk.
		if (mode != Mode::OffChain)
		{
			havePrevious = true;
			previousSequence = rq.to;
			previousGeneration = rq.generation;
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (mode == Mode::Chain) ++requestStats.chainRuns;
			else if (mode == Mode::Adopt) ++requestStats.adoptRuns;
			else ++requestStats.offChainRuns;
		}
		if (!ok || setIdx == Pipeline::StagingRing<kStagingSets>::kNoSlot)
		{
			answer(Status::Failed);
			return false;   // counted like a failed job (three in a row stop and restart the producer)
		}
		// Requests ride the same staging + BackQueue stages so their publish and answer stay strictly ordered.
		StagingSet& s = stagingSets[setIdx];
		s.halfW = w;
		s.halfH = h;
		s.lumaSlotA = lumaSlotA;
		s.lumaSlotB = lumaSlotB;
		s.usesRequestTex = usesReqTex;
		s.fromSeq = rq.from;
		s.toSeq = rq.to;
		s.generation = rq.generation;
		s.epoch = backQueue.Epoch();
		s.isRequest = true;
		s.publishToSlot = rq.Adjacent() && !forceOffChain;
		s.forceOffChain = forceOffChain;
		s.generalRates = generalRates.load(std::memory_order_acquire);
		s.requestTaken = taken;
		s.requestQueuedMs = result.queuedMs;
		s.pendingRequest = std::move(pending);
		const Pipeline::JobKey key{ rq.generation, rq.from, rq.to, true, false };
		stagingRing.MarkSubmitted(setIdx, key, s.epoch, std::chrono::steady_clock::now());
		return true;
	}

	void Producer::NoteRingMiss(const char* what)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			++requestStats.ringMisses;
			requestStats.lastRingMiss = what;   // the Ctrl+I segment (VectorEngine::ProducerStatusSegment) shows the count and these words
			error = what;
		}
		DebugLog::Log("Motion ahead: %s", what);
	}

	bool Producer::WalkActiveLocked() const
	{
		return !jobs.empty() || (lastJobMs != 0 && NowMs() - lastJobMs < kWalkIdleMs);
	}

	void Producer::NoteWalkJob(const Job& job)
	{
		// Called after the generation / break handling, before the job pairs against the chain.
		const bool adjacent = havePrevious && previousGeneration == job.generation && previousSequence + 1 == job.sequence;
		const bool lostToAdopt = adoptOverWalk && havePrevious && previousGeneration == job.generation && job.sequence <= previousSequence;
		const bool lostHints = requestResetHints && adjacent && hintSequence == UINT64_MAX;
		adoptOverWalk = false;
		requestResetHints = false;   // counted once: the walk's next job either pairs (and may lose the hints) or re-seeds
		std::lock_guard<std::mutex> lock(mutex);
		lastJobMs = NowMs();
		if (lostToAdopt) ++requestStats.adoptLostPairs;
		if (lostHints) ++requestStats.hintsLost;
	}

	void Producer::ReleaseStaleRequestTextures(int w, int h)
	{
		if ((requestTex[0] || requestTex[1]) && (requestTexW != w || requestTexH != h))
			ReleaseRequestTextures();
	}

	bool Producer::StartForTest(std::shared_ptr<TestEngine> seam)
	{
		Stop();
		std::lock_guard<std::mutex> lock(mutex);
		error.clear();
		stopping = false;
		jobs.clear();
		requests.FailAll(OnDemand::Status::Unavailable);
		testEngine = seam ? std::move(seam) : std::make_shared<TestEngine>();
		havePrevious = false;
		engineReady = false;
		sizeW = sizeH = 0;
		consecutiveFailures = 0;
		requestRunning = requestOwnsGeneration = false;
		lastJobMs = 0;
		adoptOverWalk = requestResetHints = false;
		recovery = ProducerRecovery();
		stopError.clear();
		slot->Clear();
		requestStats = RequestStats();
		running = true;
		worker = std::thread([this] { Worker(); });
		return true;
	}

	bool Producer::ProcessJobForTest(const Job& job)
	{
		const auto started = std::chrono::steady_clock::now();
		const double queueWaitMs = job.submitTime.time_since_epoch().count() != 0 ?
			(std::max)(0.0, std::chrono::duration<double, std::milli>(started - job.submitTime).count()) : 0.0;
		// Process's chain bookkeeping without a device (the seam's engine stands in for the GPU work).
		// Re-check S1: never back below a generation a no-walk request already started (it would wipe the request's pair).
		if (requestOwnsGeneration && job.generation < slot->Generation())
		{
			std::lock_guard<std::mutex> lock(mutex);
			++requestStats.staleJobs;
			return true;
		}
		requestOwnsGeneration = false;
		if (slot->Generation() != job.generation)
		{
			slot->SetGeneration(job.generation);
			havePrevious = false;
		}
		if (job.breakBefore)
			havePrevious = false;
		NoteWalkJob(job);
		bool pairs = havePrevious && previousGeneration == job.generation && previousSequence + 1 == job.sequence;
		if (pairs && testEngine->lostPairStart && testEngine->lostPairStart(job.sequence, job.generation))
		{
			NoteRingMiss(PairStartLostText());   // Process's FindPairStart(...).Lost() path: counted, named, no pair
			pairs = false;
		}
		const auto engineStart = std::chrono::steady_clock::now();
		const bool ok = testEngine->job ? testEngine->job(job.sequence, job.generation) : true;
		const auto engineEnd = std::chrono::steady_clock::now();
		if (ok && pairs)
		{
			slot->Publish(previousSequence, job.sequence, job.generation, std::make_shared<Flow>());
			const auto published = std::chrono::steady_clock::now();
			StageSample stage;
			stage.queueWaitMs = queueWaitMs;
			stage.engineMs = std::chrono::duration<double, std::milli>(engineEnd - engineStart).count();
			stage.serviceMs = std::chrono::duration<double, std::milli>(published - started).count();
			stage.submitToPublishMs = job.submitTime.time_since_epoch().count() != 0 ?
				(std::max)(0.0, std::chrono::duration<double, std::milli>(published - job.submitTime).count()) : stage.serviceMs;
			std::lock_guard<std::mutex> lock(mutex);
			++stats.frames;
			++stats.pairs;
			stats.lastPairMs = stage.serviceMs;
			stats.pairMsSum += stage.serviceMs;
			stats.RecordStage(stage);
		}
		else
		{
			std::lock_guard<std::mutex> lock(mutex);
			++stats.frames;
		}
		havePrevious = true;
		previousSequence = job.sequence;
		previousGeneration = job.generation;
		hintSequence = UINT64_MAX;
		return ok;
	}
}
