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
		// MAP-RGB-LUMA-PROXY (OD 11-15 11:11 (3)): no private RGB decode here any more. The (R + 2G + B) / 4 proxy luma and its
		// R12B / R12L / r210 / BGRA readers are gone; RGB and full-range YCbCr read the frame's shared luma plane
		// (AnalysisLumaSource::SharedLumaRow / Sample, the one decoder the lane's P1 is built from).
	}

	void HalfLumaPool::Start(int workers)
	{
		Stop();
		if (workers <= 0)
			return;
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = false;
		workerCount_ = workers;
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
						{
							if (generation_ != seen && remaining_ > 0)
								--remaining_;
							if (remaining_ == 0 || activeCallbacks_ == 0)
								doneCv_.notify_all();
							return;
						}
						seen = generation_;
						fn = fn_;
						rows = totalRows_;
						total = totalWorkers_;
						++activeCallbacks_;
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
					if (activeCallbacks_ > 0)
						--activeCallbacks_;
					if (remaining_ > 0)
						--remaining_;
					if (remaining_ == 0 || (stop_ && activeCallbacks_ == 0))
						doneCv_.notify_all();
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
		doneCv_.notify_all();
		for (std::thread& t : threads_)
			if (t.joinable()) t.join();
		threads_.clear();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			fn_ = nullptr;
			remaining_ = 0;
			activeCallbacks_ = 0;
			workerCount_ = 0;
			totalWorkers_ = 1;
		}
		doneCv_.notify_all();
	}

	void HalfLumaPool::Run(int totalRows, const std::function<void(int, int)>& fn)
	{
		if (totalRows < 256)
		{
			fn(0, totalRows);
			return;
		}
		int total = 1;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (stop_ || workerCount_ <= 0)
			{
				total = 0;
			}
			else
			{
				fn_ = &fn;
				totalRows_ = totalRows;
				remaining_ = workerCount_;
				total = totalWorkers_;
				++generation_;
			}
		}
		if (total <= 0)
		{
			fn(0, totalRows);
			return;
		}
		workCv_.notify_all();
		const int chunk = (totalRows + total - 1) / total;
		fn(0, (std::min)(totalRows, chunk));
		std::unique_lock<std::mutex> lock(mutex_);
		doneCv_.wait(lock, [this] { return (remaining_ == 0 || stop_) && activeCallbacks_ == 0; });
		fn_ = nullptr;
	}

	bool HalfLuma8(const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& halfW, int& halfH, HalfLumaPool* pool) // PRECISION-OK NVOF-8BIT: NVOF optical-flow 8-bit luma input contract
	{
		if (!source.IsValid()) return false;
		const int w = source.width, h = source.height;
		halfW = (w + 1) / 2;
		halfH = (h + 1) / 2;
		out.resize(static_cast<size_t>(halfW) * halfH);
		// A row of 10-bit luma codes: the frame's shared luma plane first (every format, RGB and full range included: the
		// shared decoder's mapped codes are the lane's P1 codes); where the plane is not attached, a limited-range YCbCr
		// capture format is read directly (P010 / P210 planes, v210, UYVY / HDYC: the same codes as the shared decode), and
		// everything else - RGB, full-range YCbCr - goes through the one sampler (Sample().luma), never a private luma law.
		const AnalysisLumaFormat format = source.format;
		const VideoFrameEncoding encoding = source.encoding;
		const bool fullRangeYuv = source.fullRangeYuv;
		const bool planar16 = !fullRangeYuv && (format == AnalysisLumaFormat::P010 || format == AnalysisLumaFormat::P210);
		const bool yuv422 = !fullRangeYuv && format == AnalysisLumaFormat::NativeYuv422;
		auto decodeRow = [&](int y, uint16_t* codes) {
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
			if (source.SampleLumaRow(0, y, w, codes)) return;   // the sampler's row form (what DecodeSharedLuma uses)
			AnalysisLumaSample sample;   // RGB, full-range YCbCr, anything else: the one sampler (the shared decoder's law)
			for (int x = 0; x < w; ++x)
				codes[x] = source.Sample(x, y, sample) ? sample.luma : 0;
		};
		// Rows in parallel (4K RGB at 60 frames: several ms per frame on one core).
		auto rows = [&](int fromY, int toY) {
			if (planar16)
			{
				// R6 (0a): fused bit-identical planar-16 (P010/P210) 2x2 decimation (zero intermediate row copies).
				for (int y = fromY; y < toY; ++y)
				{
					const uint16_t* row0 = reinterpret_cast<const uint16_t*>(source.data + static_cast<size_t>(VpUnit::HalfLuma::Tap0(y, h)) * source.rowBytes);
					const uint16_t* row1 = reinterpret_cast<const uint16_t*>(source.data + static_cast<size_t>(VpUnit::HalfLuma::Tap1(y, h)) * source.rowBytes);
					R6::FusedPlanar16HalfLuma8Row(row0, row1, w, halfW, &out[static_cast<size_t>(y) * halfW]);
				}
				return;
			}
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

	Producer::Producer()
	{
		// Camfix R1: the temporal hints are OFF by default (owner 2026-11-12 04:52, doc 91 A/H, register 115 #40); the diagnostic
		// opt-in VP_NVOF_TEMPORAL_HINTS=1 (exactly "1") turns them on, and Ctrl+I shows it while it is in force.
		char value[8] = {};
		const DWORD n = GetEnvironmentVariableA("VP_NVOF_TEMPORAL_HINTS", value, static_cast<DWORD>(sizeof(value)));
		if (n == 1 && value[0] == '1')
		{
			hintsCamera.store(true, std::memory_order_release);
			DebugLog::Log("Motion ahead: NVOF temporal hints ON for the camera fit (VP_NVOF_TEMPORAL_HINTS=1, diagnostic opt-in)");
		}
		// R6 (0a): async extras overlap is OFF by default; VP_R6_ASYNC_EXTRAS=1 enables it for 4K throughput evaluation.
		char r6Val[8] = {};
		const DWORD r6N = GetEnvironmentVariableA("VP_R6_ASYNC_EXTRAS", r6Val, static_cast<DWORD>(sizeof(r6Val)));
		if (r6N == 1 && r6Val[0] == '1')
		{
			r6AsyncExtras.store(true, std::memory_order_release);
			DebugLog::Log("Motion ahead: R6 async extras overlap ON (VP_R6_ASYNC_EXTRAS=1, diagnostic opt-in)");
		}
	}

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
		testPrevLuma.clear();
		testPrevHalfW = testPrevHalfH = 0;
		engineReady = false;
		sizeW = sizeH = 0;
		consecutiveFailures = 0;
		onTimeRecoveryRun = 0;
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

	void Producer::RunFlowGlobalsParallelForTest(Flow& flow, bool general)
	{
		if (!voteThread.joinable())
		{
			{
				std::lock_guard<std::mutex> lock(voteMutex);
				voteFlow = nullptr;
				votePending = false;
				voteStop = false;
			}
			voteThread = std::thread([this] { VoteWorker(); });
		}
		RunFlowGlobalsParallel(flow, general);
	}

	void Producer::UpdateBehindLocked(bool wasLate, double serviceMs)
	{
		const double period = sourcePeriodMs.load(std::memory_order_acquire);
		stats.periodMs = period;
		if (serviceMs > 0.0)
			stats.serviceEmaMs = stats.serviceEmaMs == 0.0 ? serviceMs : (0.8 * stats.serviceEmaMs + 0.2 * serviceMs);
		if (wasLate)
		{
			++stats.late;
			onTimeRecoveryRun = 0;
			if (period > 0.0 && stats.serviceEmaMs > period)
				stats.behind = true;
		}
		else
		{
			if (period <= 0.0)
			{
				stats.behind = false;
				onTimeRecoveryRun = 0;
			}
			else if (stats.serviceEmaMs <= period)
			{
				if (++onTimeRecoveryRun >= 8)
					stats.behind = false;
			}
			else
			{
				onTimeRecoveryRun = 0;
			}
		}
	}

	void Producer::SetR6AsyncExtras(bool on)
	{
		r6AsyncExtras.store(on, std::memory_order_release);
	}

	bool Producer::R6AsyncExtras() const
	{
		return r6AsyncExtras.load(std::memory_order_acquire);
	}

	R6::ExtrasStats Producer::GetR6ExtrasStats() const
	{
		R6::ExtrasStats s = r6ExtrasWorker.Stats();
		const uint64_t published = r6AsyncPublished.load(std::memory_order_acquire);
		if (s.asyncCompleted < published)
			s.asyncCompleted = published;
		return s;
	}

	std::string Producer::R6StatusLine() const
	{
		return R6::ExtrasWorker::StatusLine(GetR6ExtrasStats());
	}

	void Producer::Stats::RecordStage(const StageSample& sample)
	{
		lastStage = sample;
		stageRing[stageNext] = sample;
		stageNext = (stageNext + 1) % kStageRingSize;
		if (stageCount < kStageRingSize)
			++stageCount;
	}

	double Producer::Stats::StagePercentile(double StageSample::*field, double pct) const
	{
		if (stageCount == 0)
			return 0.0;
		std::array<double, kStageRingSize> sorted{};
		for (size_t i = 0; i < stageCount; ++i)
			sorted[i] = stageRing[i].*field;
		std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(stageCount));
		const double clamped = pct < 0.0 ? 0.0 : (pct > 1.0 ? 1.0 : pct);
		const size_t idx = static_cast<size_t>(clamped * static_cast<double>(stageCount - 1) + 0.5);
		return sorted[(std::min)(idx, stageCount - 1)];
	}

	void Producer::EnsureR6ExtrasWorkerStarted()
	{
		const size_t cap = PendingCapacity();
		if (!r6ExtrasWorker.Running())
		{
			r6ExtrasWorker.Start(cap);
			r6WorkerStarted.store(true, std::memory_order_release);
		}
		else
		{
			r6ExtrasWorker.SetCapacity(cap);
		}
	}

	void Producer::FlushR6ExtrasIfStarted()
	{
		if (r6WorkerStarted.load(std::memory_order_acquire))
			r6ExtrasWorker.Flush();
	}

	void Producer::BackWorker()
	{
		for (;;)
		{
			// Pop() atomically marks backQueue busy under mutex_; BusyGuard releases busy_ on
			// every exit path (normal return, early continue, or exception) after entry is destroyed.
			struct BusyGuard
			{
				Pipeline::BackQueue<BackItem>* q = nullptr;
				~BusyGuard()
				{
					if (q)
						q->MarkBusy(false);
				}
			} busyGuard;
			Pipeline::BackQueue<BackItem>::Entry entry;
			if (!backQueue.Pop(entry))
				break;
			busyGuard.q = &backQueue;

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
				continue;
			}

			// R6 (0a, default OFF): when enabled for core pairs, offload FlowGlobals + FlowExtras +
			// ordered P2 publish onto the bounded FIFO r6ExtrasWorker. Off (default) = zero extra
			// thread started, exact synchronous BACK path below.
			const bool asyncExtras = !item.isRequest && r6AsyncExtras.load(std::memory_order_acquire);
			if (asyncExtras)
			{
				EnsureR6ExtrasWorkerStarted();
				const std::shared_ptr<Flow> flow = item.flow;
				const uint64_t fromSeq = item.fromSeq;
				const uint64_t toSeq = item.toSeq;
				const uint64_t gen = item.generation;
				const uint64_t entryGen = entry.generation;
				const uint64_t entryEpoch = entry.epoch;
				const bool entryIgnoreGen = entry.ignoreGeneration;
				const bool general = item.generalRates;
				const bool hinted = item.hinted;
				const auto submitTime = item.submitTime;
				const StageSample baseStage = item.stage;
				const std::thread::id submitter = std::this_thread::get_id();
				backQueue.AcquireExternalBusy();
				std::shared_ptr<void> externalBusyToken(nullptr, [this](void*) {
					backQueue.ReleaseExternalBusy();
				});
				r6ExtrasWorker.Submit([this, flow, fromSeq, toSeq, gen, entryGen, entryEpoch, entryIgnoreGen,
						general, hinted, submitTime, baseStage, submitter, externalBusyToken = std::move(externalBusyToken)]() mutable {
					(void)externalBusyToken;
					if (!backQueue.IsLive(entryGen, entryEpoch, entryIgnoreGen))
						return;
					const auto voteStart = std::chrono::steady_clock::now();
					VPV_COMPUTE_ONCE_AT("U10", toSeq, 0);
					FlowGlobals(*flow, general);
					const auto extrasStart = std::chrono::steady_clock::now();
					FlowExtras(*flow);
					const auto extrasEnd = std::chrono::steady_clock::now();
					const double voteMs = std::chrono::duration<double, std::milli>(extrasStart - voteStart).count();
					const double flowExtrasMs = std::chrono::duration<double, std::milli>(extrasEnd - extrasStart).count();
					const double combinedExtrasMs = voteMs + flowExtrasMs;
					if (!backQueue.IsLive(entryGen, entryEpoch, entryIgnoreGen))
						return;
					if (std::this_thread::get_id() != submitter)
						r6AsyncPublished.fetch_add(1, std::memory_order_release);
					if (publishGate.Accept(gen, fromSeq, toSeq))
					{
						slot->Publish(fromSeq, toSeq, gen, flow);   // P2: ordered publish
						const auto published = std::chrono::steady_clock::now();
						StageSample stage = baseStage;
						stage.voteMs = voteMs;
						stage.extrasMs = flowExtrasMs;
						stage.serviceMs = stage.lumaMs + stage.engineMs + stage.mapMs + combinedExtrasMs;
						stage.submitToPublishMs = submitTime.time_since_epoch().count() != 0 ?
							(std::max)(0.0, std::chrono::duration<double, std::milli>(published - submitTime).count()) : stage.serviceMs;
						engineReady = true;
						std::lock_guard<std::mutex> lock(mutex);
						++stats.pairs;
						if (hinted)
							++stats.hintedPairs;
						stats.extrasMs = combinedExtrasMs;
						stats.lastPairMs = stage.serviceMs;
						stats.pairMsSum += stats.lastPairMs;
						stats.RecordStage(stage);
						UpdateBehindLocked(false, stage.serviceMs);
					}
				});
				continue;
			}

			// Preserve strict FIFO publish ordering if R6 async extras ran earlier in this session.
			FlushR6ExtrasIfStarted();
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
					if (item.hinted)
						++stats.hintedPairs;
					stats.extrasMs = combinedExtrasMs;
					stats.lastPairMs = item.stage.serviceMs;
					stats.pairMsSum += stats.lastPairMs;
					stats.RecordStage(item.stage);
					UpdateBehindLocked(false, item.stage.serviceMs);
				}
			}
			else
			{
				if (item.publishToSlot && !item.forceOffChain &&
					publishGate.Accept(item.generation, item.fromSeq, item.toSeq))
					slot->Publish(item.fromSeq, item.toSeq, item.generation, item.flow);
				engineReady = true;
				if (item.hinted)
				{
					std::lock_guard<std::mutex> lock(mutex);
					++stats.hintedPairs;
				}
				OnDemand::PairResult res;
				res.status = OnDemand::Status::Ready;
				res.flow = item.flow;
				res.queuedMs = item.requestQueuedMs;
				res.runMs = OnDemand::MsBetween(item.requestTaken, OnDemand::Clock::now());
				Answer(item.pendingRequest, std::move(res));
			}
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
		r6ExtrasWorker.Stop();   // R6: drain and join any in-flight overlapped extras before releasing slot/engine
		r6WorkerStarted.store(false, std::memory_order_release);
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
			const auto submitNow = std::chrono::steady_clock::now();
			const int lead = pendingLead.load(std::memory_order_acquire);
			const int depth = pendingDepth.load(std::memory_order_acquire);
			const int activeLead = lead > 0 ? lead : depth;
			const auto deadline = DeadlineFor(submitNow, activeLead, sourcePeriodMs.load(std::memory_order_acquire));
			jobs.push_back(Job{ sequence, generation, luma, breakBefore, submitNow, deadline });
		}
		lastSubmitted.store(sequence, std::memory_order_release);
		wake.notify_all();
	}

	bool Producer::HandleEngineFailureLocked(std::unique_lock<std::mutex>& lock)
	{
		++stats.failures;
		if (++consecutiveFailures < 3)
			return true;
		// Persistent failure: stop (the matcher covers everything meanwhile). Bug hunt 35 #1: no
		// longer for the session - restart with a back-off on a fresh device while still wanted.
		lock.unlock();
		DrainStagingSets(true);
		backQueue.DiscardAllAndBumpEpoch();
		backQueue.WaitIdle();
		FlushR6ExtrasIfStarted();
		publishGate.Reset();
		lock.lock();
		if (stopping)
			return false;
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
				return false;
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
			if (stopping)
				return false;
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
		return true;
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
					if (!HandleEngineFailureLocked(lock))
						return;
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
					if (!testEngine && context && stagingRing.SubmittedCount() > 0)
					{
						// Wait on whichever comes first: new job (if a staging slot is free) or oldest GPU event.
						// Spin-then-sleep: brief polls before a short timed wait so we neither stall nor burn CPU.
						lock.unlock();
						bool completed = false;
						bool pollOk = true;
						for (int spin = 0; spin < 8 && !completed; ++spin)
						{
							if (!PollReadyStagingSets(completed))
							{
								pollOk = false;
								break;
							}
							if (!completed)
								std::this_thread::yield();
						}
						if (!pollOk)
						{
							lock.lock();
							if (!HandleEngineFailureLocked(lock))
								return;
							continue;
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
			if (!HandleEngineFailureLocked(lock))
				return;
		}
	}

	bool Producer::EnsureSize(int halfW, int halfH)
	{
		if (halfW == sizeW && halfH == sizeH && engine && engine->IsOpen()) return true;
		// Drain all in-flight staging sets and wait for BACK before destroying the old staging textures.
		if (!DrainStagingSets(false))
			return false;
		backQueue.WaitIdle();
		FlushR6ExtrasIfStarted();
		stagingRing.Clear();
		engine.reset();
		VpSkeleton::VramUsers::Instance().Withdraw(this, VpSkeleton::kUserNvofProducer);   // MV7: re-declared below for the new size
		VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserNvofProducer, true);
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
		// Tear down any partial allocations and withdraw/deactivate VRAM accounting on any failure
		// while preserving the exact error string and ENGINE-thread staging-ring affinity.
		auto fail = [&](std::string msg) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				error = std::move(msg);
			}
			ReleaseEngine();
			stagingRing.BindEngineThread();
			return false;
		};
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
				return fail("cannot create its textures");
		for (int i = 0; i < 2; ++i)
			if (!make(gw, gh, NvOpticalFlow::Engine::FlowFormat(), false, flowTex[i]) ||
				!make(gw, gh, NvOpticalFlow::Engine::CostFormat(), false, costTex[i]))
				return fail("cannot create its textures");
		D3D11_QUERY_DESC qd{};
		qd.Query = D3D11_QUERY_EVENT;
		for (int k = 0; k < kStagingSets; ++k)
		{
			for (int i = 0; i < 2; ++i)
				if (!make(gw, gh, NvOpticalFlow::Engine::FlowFormat(), true, stagingSets[k].flowRead[i]) ||
					!make(gw, gh, NvOpticalFlow::Engine::CostFormat(), true, stagingSets[k].costRead[i]))
					return fail("cannot create its textures");
			if (FAILED(device->CreateQuery(&qd, &stagingSets[k].eventQuery)))
				return fail("cannot create its event query");
		}
		engine = std::make_unique<NvOpticalFlow::Engine>();
		if (!engine->Open(device, context, halfW, halfH, preset, true))
			return fail(engine->LastError());
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
			FlushR6ExtrasIfStarted();   // R6: preserve strict generation boundary ordering before SetGeneration clears the slot
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
		// R3: this frame's engine luma is kept (one shared copy) only while the camera fit consumes the pair stream; the next pair takes it as
		// its `from`. No copy otherwise.
		std::shared_ptr<const std::vector<uint8_t>> currentLuma; // PRECISION-OK NVOF-8BIT: NVOF optical-flow 8-bit luma input contract
		if (CameraConsumes())
			currentLuma = std::make_shared<const std::vector<uint8_t>>(lumaBuffer); // PRECISION-OK NVOF-8BIT: NVOF optical-flow 8-bit luma input contract
		// Slice 16 D1 (refs/105 s3.4): if this core job is already past its deadline (frame already shown),
		// skip the NVOF pair run (counted `late`), keep its luma in the ring so (N, N+1) survives, and reset
		// temporal hints so the next pair runs non-continuous. Guarded by historyBehindRender (owner item 1).
		const bool pastDeadline = !historyBehindRender.load(std::memory_order_acquire) &&
			PastDeadline(started, job.deadline);
		bool ok = true;
		if (pastDeadline)
		{
			hintSequence = UINT64_MAX;
			std::lock_guard<std::mutex> lock(mutex);
			UpdateBehindLocked(true, 0.0);
		}
		else if (prevSlot != EngineLumaRing::kNoSlot)
		{
			const int prev = prevSlot;
			// The temporal hints only when the engine's last pair ended at this pair's first frame
			// (audit 2026-10-16: they carried across seeks, dropped jobs and size skips).
			// Camfix R1: and only while the temporal hints are on (SetTemporalHints / SetTemporalHintsForTest; camera default off).
			const bool continuous = HintsApply(hintSequence == previousSequence && hintGeneration == job.generation);
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
				s.hinted = continuous;
				if (previousLuma && currentLuma && previousLuma->size() == currentLuma->size() &&
					currentLuma->size() == static_cast<size_t>(halfW) * static_cast<size_t>(halfH))
				{
					s.lumaFrom = previousLuma;
					s.lumaTo = currentLuma;
				}
				else
				{
					s.lumaFrom.reset();
					s.lumaTo.reset();
				}
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
		previousLuma = currentLuma;
		return ok;
	}
	bool Producer::LumaAtTarget(uint64_t sequence, const AnalysisLumaSource& luma, std::vector<uint8_t>& buffer, int& halfW, int& halfH,
		const char* privateSite)
	{
		// The engine half: the skeleton's P1 (one decode per frame, Acquire from this thread) when it has the frame and the
		// codes are the engine's (every format since MAP-RGB-LUMA-PROXY); on no source or a miss HalfLuma8 reads the same shared luma
		// plane / sampler (no private luma law) and the miss is counted (requestStats.p1OwnReads) and shown (VectorEngine::ProducerStatusSegment).
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
			if (p1)   // a P1 reader is wired and missed: this frame's engine luma came from the producer's own read (shown, never silent)
				p1OwnReadCount.fetch_add(1, std::memory_order_relaxed);   // atomic: callers may hold `mutex`
		}
		else
			p1ReadCount.fetch_add(1, std::memory_order_relaxed);
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
		s.hinted = false;
		s.lumaFrom.reset();
		s.lumaTo.reset();
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
			{
				std::lock_guard<std::mutex> lock(mutex);
				error = "staging flow map failed";
				return false;
			}
			for (int y = 0; y < flow->gridH; ++y)
				std::memcpy(&vectors[static_cast<size_t>(y) * flow->gridW * 2],
					static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
					static_cast<size_t>(flow->gridW) * 4);
			context->Unmap(s.flowRead[i], 0);
			if (FAILED(context->Map(s.costRead[i], 0, D3D11_MAP_READ, 0, &map)))
			{
				std::lock_guard<std::mutex> lock(mutex);
				error = "staging cost map failed";
				return false;
			}
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
		flow->lumaFrom = std::move(s.lumaFrom);
		flow->lumaTo = std::move(s.lumaTo);
		item.flow = std::move(flow);
		item.fromSeq = s.fromSeq;
		item.toSeq = s.toSeq;
		item.generation = s.generation;
		item.epoch = s.epoch;
		item.isRequest = s.isRequest;
		item.publishToSlot = s.publishToSlot;
		item.forceOffChain = s.forceOffChain;
		item.generalRates = s.generalRates;
		item.hinted = s.hinted;
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
				{
					FailStagingSetRequest(oldest, OnDemand::Status::Failed);
				}
				else
				{
					std::lock_guard<std::mutex> lock(mutex);
					consecutiveFailures = 0;
				}
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
			{
				if (discard)
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
				}
				return false;
			}
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
		RequestStats s = requestStats;
		s.p1Reads = p1ReadCount.load(std::memory_order_relaxed);
		s.p1OwnReads = p1OwnReadCount.load(std::memory_order_relaxed);
		return s;
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
			}
			FlushR6ExtrasIfStarted();
			publishGate.SetGeneration(rq.generation);
			slot->SetGeneration(rq.generation);
			havePrevious = false;
			testPrevLuma.clear();
			testPrevHalfW = testPrevHalfH = 0;
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
			{
				FlushR6ExtrasIfStarted();
				if (publishGate.Accept(rq.generation, rq.from, rq.to))
					slot->Publish(rq.from, rq.to, rq.generation, flow);
			}
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
		// Camfix R1: through HintsApply like the scheduled path, so the displayed hints state is the real one.
		const bool continuous = HintsApply(mode == Mode::Chain && rq.Adjacent() && hintSequence == rq.from && hintGeneration == rq.generation);
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
		s.hinted = continuous;
		s.lumaFrom.reset();
		s.lumaTo.reset();
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
		testPrevLuma.clear();
		testPrevHalfW = testPrevHalfH = 0;
		engineReady = false;
		sizeW = sizeH = 0;
		consecutiveFailures = 0;
		onTimeRecoveryRun = 0;
		requestRunning = requestOwnsGeneration = false;
		lastJobMs = 0;
		adoptOverWalk = requestResetHints = false;
		recovery = ProducerRecovery();
		stopError.clear();
		slot->Clear();
		publishGate.Reset();
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
			FlushR6ExtrasIfStarted();   // R6: preserve strict generation boundary ordering before SetGeneration clears the slot
			slot->SetGeneration(job.generation);
			publishGate.SetGeneration(job.generation);
			havePrevious = false;
			testPrevLuma.clear();
			testPrevHalfW = testPrevHalfH = 0;
		}
		if (job.breakBefore)
		{
			havePrevious = false;
			testPrevLuma.clear();
			testPrevHalfW = testPrevHalfH = 0;
		}
		NoteWalkJob(job);
		bool pairs = havePrevious && previousGeneration == job.generation && previousSequence + 1 == job.sequence;
		if (pairs && testEngine->lostPairStart && testEngine->lostPairStart(job.sequence, job.generation))
		{
			NoteRingMiss(PairStartLostText());   // Process's FindPairStart(...).Lost() path: counted, named, no pair
			pairs = false;
		}
		std::vector<uint8_t> curTestLuma; // PRECISION-OK NVOF-8BIT: NVOF optical-flow 8-bit luma test buffer
		int curHalfW = 0, curHalfH = 0;
		const bool haveLuma = HalfLuma8(job.luma, curTestLuma, curHalfW, curHalfH);
		const bool pastDeadline = !historyBehindRender.load(std::memory_order_acquire) &&
			PastDeadline(started, job.deadline);
		if (pastDeadline)
		{
			hintSequence = UINT64_MAX;
			{
				std::lock_guard<std::mutex> lock(mutex);
				++stats.frames;
				UpdateBehindLocked(true, 0.0);
			}
			// Keep luma/chain state so (N, N+1) survives on the next frame.
			havePrevious = true;
			previousSequence = job.sequence;
			previousGeneration = job.generation;
			if (haveLuma)
			{
				testPrevLuma = std::move(curTestLuma);
				testPrevHalfW = curHalfW;
				testPrevHalfH = curHalfH;
			}
			else
			{
				testPrevLuma.clear();
				testPrevHalfW = testPrevHalfH = 0;
			}
			return true;
		}
		const bool continuous = HintsApply(hintSequence == previousSequence && hintGeneration == job.generation);
		if (pairs && testEngine->onPairContinuous)
			testEngine->onPairContinuous(job.sequence, continuous);
		const auto engineStart = std::chrono::steady_clock::now();
		const bool ok = testEngine->job ? testEngine->job(job.sequence, job.generation) : true;
		const auto engineEnd = std::chrono::steady_clock::now();
		const double engineMs = std::chrono::duration<double, std::milli>(engineEnd - engineStart).count();
		if (ok && pairs)
		{
			// Build a deterministic non-empty synthetic Flow (dimensions from HalfLuma8 or single Flow::kBlockPx
			// fallback when job.luma is empty; S10.5 vector units 1<<5 == 1 half-px; Grid == NvOpticalFlow::Engine::Grid)
			// so CPU seam runs exercise FlowGlobals, FlowExtras, OrderedPublishGate, and R6 async extras faithfully.
			auto flow = std::make_shared<Flow>();
			const int halfW = (haveLuma && curHalfW > 0 && curHalfH > 0) ? curHalfW : Flow::kBlockPx;
			const int halfH = (haveLuma && curHalfW > 0 && curHalfH > 0) ? curHalfH : Flow::kBlockPx;
			flow->halfW = halfW;
			flow->halfH = halfH;
			flow->gridW = (halfW + NvOpticalFlow::Engine::Grid - 1) / NvOpticalFlow::Engine::Grid;
			flow->gridH = (halfH + NvOpticalFlow::Engine::Grid - 1) / NvOpticalFlow::Engine::Grid;
			const size_t cells = static_cast<size_t>(flow->gridW) * static_cast<size_t>(flow->gridH);
			flow->forward.resize(cells * 2);
			flow->backward.resize(cells * 2);
			flow->forwardCost.resize(cells);
			flow->backwardCost.resize(cells);
			const bool sameLumaGrid = haveLuma && curHalfW == testPrevHalfW && curHalfH == testPrevHalfH &&
				curTestLuma.size() == testPrevLuma.size() && !curTestLuma.empty();
			const int16_t baseVx = static_cast<int16_t>(((job.sequence & 3u) + 1u) << 5);
			const int16_t baseVy = static_cast<int16_t>(-static_cast<int>((previousSequence & 3u) << 4));
			for (int cy = 0; cy < flow->gridH; ++cy)
			{
				const int py = (std::min)(halfH - 1, cy * NvOpticalFlow::Engine::Grid);
				for (int cx = 0; cx < flow->gridW; ++cx)
				{
					const int px = (std::min)(halfW - 1, cx * NvOpticalFlow::Engine::Grid);
					const size_t cellIdx = static_cast<size_t>(cy) * static_cast<size_t>(flow->gridW) + static_cast<size_t>(cx);
					int diff = 0;
					if (sameLumaGrid)
					{
						const size_t pxIdx = static_cast<size_t>(py) * static_cast<size_t>(halfW) + static_cast<size_t>(px);
						diff = static_cast<int>(curTestLuma[pxIdx]) - static_cast<int>(testPrevLuma[pxIdx]);
					}
					else
					{
						diff = static_cast<int>((cx + cy + static_cast<int>(job.sequence)) & 7) - 4;
					}
					const int16_t vx = static_cast<int16_t>(baseVx + (diff >> 3));
					const int16_t vy = static_cast<int16_t>(baseVy - (diff >> 3));
					flow->forward[cellIdx * 2] = vx;
					flow->forward[cellIdx * 2 + 1] = vy;
					flow->backward[cellIdx * 2] = static_cast<int16_t>(-vx + (diff >> 4));
					flow->backward[cellIdx * 2 + 1] = static_cast<int16_t>(-vy);
					const uint8_t cost = static_cast<uint8_t>((std::min)(255, std::abs(diff)));
					flow->forwardCost[cellIdx] = cost;
					flow->backwardCost[cellIdx] = cost;
				}
			}

			const uint64_t fromSeq = previousSequence;
			const uint64_t toSeq = job.sequence;
			const uint64_t gen = job.generation;
			const bool general = generalRates.load(std::memory_order_acquire);
			const auto submitTime = job.submitTime;
			const bool asyncExtras = r6AsyncExtras.load(std::memory_order_acquire);
			auto finishPair = [this, flow, fromSeq, toSeq, gen, general, started, submitTime, queueWaitMs, engineMs, continuous](bool fromAsyncWorker) {
				const auto extrasStart = std::chrono::steady_clock::now();
				VPV_COMPUTE_ONCE_AT("U10", toSeq, 0);
				FlowGlobals(*flow, general);
				FlowExtras(*flow);
				const auto extrasEnd = std::chrono::steady_clock::now();
				const double extrasMs = std::chrono::duration<double, std::milli>(extrasEnd - extrasStart).count();
				if (fromAsyncWorker)
					r6AsyncPublished.fetch_add(1, std::memory_order_release);
				if (publishGate.Accept(gen, fromSeq, toSeq))
				{
					slot->Publish(fromSeq, toSeq, gen, flow);
					const auto published = std::chrono::steady_clock::now();
					StageSample stage;
					stage.queueWaitMs = queueWaitMs;
					stage.engineMs = engineMs;
					stage.extrasMs = extrasMs;
					stage.serviceMs = std::chrono::duration<double, std::milli>(published - started).count();
					stage.submitToPublishMs = submitTime.time_since_epoch().count() != 0 ?
						(std::max)(0.0, std::chrono::duration<double, std::milli>(published - submitTime).count()) : stage.serviceMs;
					engineReady = true;
					std::lock_guard<std::mutex> lock(mutex);
					++stats.pairs;
					if (continuous)
						++stats.hintedPairs;
					stats.extrasMs = extrasMs;
					stats.lastPairMs = stage.serviceMs;
					stats.pairMsSum += stage.serviceMs;
					stats.RecordStage(stage);
					UpdateBehindLocked(false, stage.serviceMs);
				}
			};
			if (asyncExtras)
			{
				EnsureR6ExtrasWorkerStarted();
				const std::thread::id submitter = std::this_thread::get_id();
				r6ExtrasWorker.Submit([finishPair, submitter]() mutable {
					finishPair(std::this_thread::get_id() != submitter);
				});
			}
			else
			{
				FlushR6ExtrasIfStarted();
				finishPair(false);
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				++stats.frames;
			}
		}
		else
		{
			std::lock_guard<std::mutex> lock(mutex);
			++stats.frames;
		}
		havePrevious = true;
		previousSequence = job.sequence;
		previousGeneration = job.generation;
		if (haveLuma)
		{
			testPrevLuma = std::move(curTestLuma);
			testPrevHalfW = curHalfW;
			testPrevHalfH = curHalfH;
		}
		else
		{
			testPrevLuma.clear();
			testPrevHalfW = testPrevHalfH = 0;
		}
		if (ok && pairs)
		{
			hintSequence = job.sequence;
			hintGeneration = job.generation;
		}
		else
		{
			hintSequence = UINT64_MAX;
		}
		return ok;
	}
}
