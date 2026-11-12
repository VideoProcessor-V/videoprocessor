#include <pch.h>
#include "MotionAheadProducer.h"

#include <DebugLog.h>
#include <VP-V/core/PassTimerInbox.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>

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

	bool HalfLuma8(const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& halfW, int& halfH)
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
		const unsigned cores = (std::max)(1u, std::thread::hardware_concurrency());
		const int threads = halfH >= 256 ? static_cast<int>((std::min)(4u, (std::max)(1u, cores / 2))) : 1;
		if (threads <= 1)
			rows(0, halfH);
		else
		{
			std::vector<std::thread> pool;
			const int chunk = (halfH + threads - 1) / threads;
			for (int t = 1; t < threads; ++t)
				pool.emplace_back(rows, (std::min)(halfH, t * chunk), (std::min)(halfH, (t + 1) * chunk));
			rows(0, (std::min)(halfH, chunk));
			for (std::thread& th : pool) th.join();
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
		requestStreak = 0;           // W2-C5b
		requestRunning = requestOwnsGeneration = false;
		lastJobMs = 0;
		adoptOverWalk = requestResetHints = false;
		recovery = ProducerRecovery();
		stopError.clear();
		slot->Clear();   // a fresh producer starts from an empty P2 slot (the pairs of an older one are not its own)
		renderAdapter = adapter;
		running = true;
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
		engine.reset();
		VpSkeleton::VramUsers::Instance().Withdraw(this, VpSkeleton::kUserNvofProducer);   // MV7: the ring textures go with the engine
		VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserNvofProducer, false);
		for (int i = 0; i < 2; ++i)
		{
			lumaTex[i].Release(); flowTex[i].Release(); costTex[i].Release(); flowRead[i].Release(); costRead[i].Release();
		}
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
		wake.notify_all();
		if (worker.joinable()) worker.join();
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
			jobs.push_back(Job{ sequence, generation, luma, breakBefore });
		}
		lastSubmitted.store(sequence, std::memory_order_release);
		wake.notify_all();
	}

	void Producer::Worker()
	{
		for (;;)
		{
			Job job;
			OnDemand::Pending request;   // W2-C5b: an on-demand request is taken before the look-ahead jobs
			bool isRequest = false;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [this] { return stopping || !requests.Empty() || !jobs.empty(); });
				if (stopping) return;
				// Requests first, but at most kMaxRequestStreak in a row while a job waits (W2-C5b audit: the walk is never starved).
				if (!requests.Empty() && (jobs.empty() || requestStreak < kMaxRequestStreak))
				{
					isRequest = requests.Pop(request);
					++requestStreak;
					requestRunning = isRequest;
				}
				if (!isRequest)
				{
					if (!requests.Empty())
						++requestStats.fairnessJobs;
					requestStreak = 0;
					job = std::move(jobs.front());
					jobs.pop_front();
				}
			}
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
		engine.reset();
		VpSkeleton::VramUsers::Instance().Withdraw(this, VpSkeleton::kUserNvofProducer);   // MV7: re-declared below for the new size
		VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserNvofProducer, true);   // an open that fails below leaves it active without a declaration: shown by the skeleton
		for (int i = 0; i < 2; ++i)
		{
			lumaTex[i].Release(); flowTex[i].Release(); costTex[i].Release(); flowRead[i].Release(); costRead[i].Release();
		}
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
		for (int i = 0; i < 2; ++i)
			if (!make(halfW, halfH, NvOpticalFlow::Engine::InputFormat(), false, lumaTex[i]) ||
				!make(gw, gh, NvOpticalFlow::Engine::FlowFormat(), false, flowTex[i]) ||
				!make(gw, gh, NvOpticalFlow::Engine::CostFormat(), false, costTex[i]) ||
				!make(gw, gh, NvOpticalFlow::Engine::FlowFormat(), true, flowRead[i]) ||
				!make(gw, gh, NvOpticalFlow::Engine::CostFormat(), true, costRead[i]))
			{
				std::lock_guard<std::mutex> lock(mutex);
				error = "cannot create its textures";
				return false;
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
		VpSkeleton::VramUsers::Instance().DeclareFixed(this, VpSkeleton::kUserNvofProducer, NvOpticalFlow::RingBytes(halfW, halfH, false, false));
		return true;
	}

	bool Producer::Process(const Job& job)
	{
		const auto started = std::chrono::steady_clock::now();
		int halfW = 0, halfH = 0;
		if (!LumaAtTarget(job.luma, lumaBuffer, halfW, halfH))
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
			slot->SetGeneration(job.generation);
			havePrevious = false;
		}
		// A break the sequence does not show: no pair across it (MotionValidity step 1 F).
		if (job.breakBefore)
			havePrevious = false;
		NoteWalkJob(job);   // W2-C5b
		const int texIndex = havePrevious ? 1 - current : current;
		context->UpdateSubresource(lumaTex[texIndex], 0, nullptr, lumaBuffer.data(), static_cast<UINT>(halfW), 0);
		bool ok = true;
		if (havePrevious && previousGeneration == job.generation && previousSequence + 1 == job.sequence)
		{
			const int prev = current;
			// The temporal hints only when the engine's last pair ended at this pair's first frame
			// (audit 2026-10-16: they carried across seeks, dropped jobs and size skips).
			const bool continuous = hintSequence == previousSequence && hintGeneration == job.generation;
			hintSequence = UINT64_MAX;
			bool ran = false;
			std::shared_ptr<Flow> flow;
			double extrasMs = 0.0;
			ok = RunPair(lumaTex[prev], lumaTex[texIndex], halfW, halfH, continuous, job.sequence, ran, flow, extrasMs);
			if (ran)
			{
				hintSequence = job.sequence;
				hintGeneration = job.generation;
			}
			if (ok && flow)
			{
				slot->Publish(previousSequence, job.sequence, job.generation, flow);   // P2: the pair, for the readers' P2View
				engineReady = true;
				std::lock_guard<std::mutex> lock(mutex);
				++stats.pairs;
				stats.extrasMs = extrasMs;
				stats.lastPairMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
				stats.pairMsSum += stats.lastPairMs;
			}
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			++stats.frames;
			stats.lumaMs = lumaMs;
		}
		current = texIndex;
		havePrevious = true;
		previousSequence = job.sequence;
		previousGeneration = job.generation;
		return ok;
	}
	bool Producer::LumaAtTarget(const AnalysisLumaSource& luma, std::vector<uint8_t>& buffer, int& halfW, int& halfH)
	{
		if (!HalfLuma8(luma, buffer, halfW, halfH))
			return false;   // not a readable frame: nothing to do, not a failure
		// The de-judder's engine size: the half size, or half again (its inner matcher).
		const int tw = targetW.load(std::memory_order_acquire), th = targetH.load(std::memory_order_acquire);
		if (tw > 0 && th > 0 && !(tw == halfW && th == halfH))
		{
			if (tw != (halfW + 1) / 2 || th != (halfH + 1) / 2)
				return false;   // a size this producer does not make: nothing published (the consumer takes the matcher path, shown as a fallback)
			quarterBuffer.resize(static_cast<size_t>(tw) * th);
			for (int y = 0; y < th; ++y)
			{
				const uint8_t* r0 = &buffer[static_cast<size_t>((std::min)(2 * y, halfH - 1)) * halfW];
				const uint8_t* r1 = &buffer[static_cast<size_t>((std::min)(2 * y + 1, halfH - 1)) * halfW];
				for (int x = 0; x < tw; ++x)
				{
					const int x0 = (std::min)(2 * x, halfW - 1), x1 = (std::min)(2 * x + 1, halfW - 1);
					quarterBuffer[static_cast<size_t>(y) * tw + x] = static_cast<uint8_t>((r0[x0] + r0[x1] + r1[x0] + r1[x1] + 2) / 4);
				}
			}
			buffer.swap(quarterBuffer);
			halfW = tw;
			halfH = th;
		}
		return true;
	}

	bool Producer::RunPair(ID3D11Texture2D* a, ID3D11Texture2D* b, int halfW, int halfH, bool continuous, uint64_t timedSequence, bool& ran,
		std::shared_ptr<Flow>& flowOut, double& extrasMs)
	{
		ran = false;
		flowOut.reset();
		extrasMs = 0.0;
		// MV7: 1 pair in N is timed (the GPU idle first, then the engine's completion, on this worker's own device) and posted to the
		// pass-timer inbox the render thread drains; the other pairs run untimed exactly as before.
		NvOpticalFlow::Result timing;
		const bool timed = PassTimers::Sampler().Take(timedSequence);
		if (!engine->Run(a, b, flowTex[0], flowTex[1], costTex[0], costTex[1], timed ? &timing : nullptr, continuous))
		{
			std::lock_guard<std::mutex> lock(mutex);
			error = engine->LastError();
			return false;
		}
		ran = true;
		if (timed && timing.gpuMs > 0.0)
			PassTimers::Inbox::Motion().Post(PassTimers::Kind::GpuGroup, "motion: NVOF producer engine (idle to completion)", timing.gpuMs);
		for (int i = 0; i < 2; ++i)
		{
			context->CopyResource(flowRead[i], flowTex[i]);
			context->CopyResource(costRead[i], costTex[i]);
		}
		auto flow = std::make_shared<Flow>();
		flow->halfW = halfW; flow->halfH = halfH;
		flow->gridW = (halfW + 3) / 4; flow->gridH = (halfH + 3) / 4;
		const size_t cells = static_cast<size_t>(flow->gridW) * flow->gridH;
		for (int i = 0; i < 2; ++i)
		{
			std::vector<int16_t>& vectors = i == 0 ? flow->forward : flow->backward;
			std::vector<uint8_t>& costs = i == 0 ? flow->forwardCost : flow->backwardCost;
			vectors.resize(cells * 2);
			costs.resize(cells);
			D3D11_MAPPED_SUBRESOURCE map{};
			if (FAILED(context->Map(flowRead[i], 0, D3D11_MAP_READ, 0, &map))) return false;
			for (int y = 0; y < flow->gridH; ++y)
				std::memcpy(&vectors[static_cast<size_t>(y) * flow->gridW * 2], static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
					static_cast<size_t>(flow->gridW) * 4);
			context->Unmap(flowRead[i], 0);
			if (FAILED(context->Map(costRead[i], 0, D3D11_MAP_READ, 0, &map))) return false;
			for (int y = 0; y < flow->gridH; ++y)
				std::memcpy(&costs[static_cast<size_t>(y) * flow->gridW], static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
					static_cast<size_t>(flow->gridW));
			context->Unmap(costRead[i], 0);
		}
		// Planner Q1: the whole frame's motion, here on the worker (no render cost).
		const auto extrasStart = std::chrono::steady_clock::now();
		const bool general = generalRates.load(std::memory_order_acquire);   // fixed-constant batch B4 2026-10-18
		FlowGlobals(*flow, general);   // both directions, flagged for the FlowOf view (W1-C3)
		// The cost and the engine's own forward/backward consistency as trust inputs.
		FlowExtras(*flow);
		extrasMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - extrasStart).count();
		flowOut = std::move(flow);
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
			if (!LumaAtTarget(rq.lumaTo, lumaBuffer, w, h))
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
			slot->SetGeneration(rq.generation);
			havePrevious = false;
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
		double extrasMs = 0.0;
		bool ran = false;
		if (testEngine)
		{
			flow = std::make_shared<Flow>();
			ok = testEngine->request ? testEngine->request(rq, mode, *flow) : true;
			ran = ok;
			if (!ok) flow.reset();
		}
		else
		{
			if (mode != Mode::Chain)
			{
				int fw = 0, fh = 0;
				if (!LumaAtTarget(rq.lumaFrom, requestLumaBuffer, fw, fh) || fw != w || fh != h)
				{
					engineRan = false;        // nothing ran: leaves the failure count alone
					answer(Status::Failed);   // the two frames are not one size: no pair
					return true;
				}
			}
			ID3D11Texture2D* texFrom = nullptr;
			ID3D11Texture2D* texTo = nullptr;
			int toIndex = current;
			if (mode == Mode::Chain)
			{
				toIndex = 1 - current;
				context->UpdateSubresource(lumaTex[toIndex], 0, nullptr, lumaBuffer.data(), static_cast<UINT>(w), 0);
				texFrom = lumaTex[current];
				texTo = lumaTex[toIndex];
			}
			else if (mode == Mode::Adopt)
			{
				toIndex = 1 - current;
				context->UpdateSubresource(lumaTex[current], 0, nullptr, requestLumaBuffer.data(), static_cast<UINT>(w), 0);
				context->UpdateSubresource(lumaTex[toIndex], 0, nullptr, lumaBuffer.data(), static_cast<UINT>(w), 0);
				texFrom = lumaTex[current];
				texTo = lumaTex[toIndex];
			}
			else
			{
				if (!EnsureRequestTextures(w, h))
				{
					answer(Status::Failed);
					return false;
				}
				context->UpdateSubresource(requestTex[0], 0, nullptr, requestLumaBuffer.data(), static_cast<UINT>(w), 0);
				context->UpdateSubresource(requestTex[1], 0, nullptr, lumaBuffer.data(), static_cast<UINT>(w), 0);
				texFrom = requestTex[0];
				texTo = requestTex[1];
			}
			// The temporal hints only for an adjacent Chain pair right after the engine's last pair ended at `from`.
			const bool continuous = mode == Mode::Chain && rq.Adjacent() && hintSequence == rq.from && hintGeneration == rq.generation;
			hintSequence = UINT64_MAX;   // any run here moves the engine's internal hint state
			requestResetHints = true;    // a walk pair that would have used them runs without (counted in NoteWalkJob)
			ok = RunPair(texFrom, texTo, w, h, continuous, rq.to, ran, flow, extrasMs);
			if (ran && mode != Mode::OffChain && rq.Adjacent())
			{
				hintSequence = rq.to;
				hintGeneration = rq.generation;
			}
			if (mode != Mode::OffChain)
				current = toIndex;
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
		if (!ok || !flow)
		{
			answer(Status::Failed);
			return false;   // counted like a failed job (three in a row stop and restart the producer)
		}
		// Adjacent pairs into P2 as well (any later reader hits); gap pairs are returned only (P2 stays adjacency-only).
		// A forced off-chain pair is of a generation the slot has not reached: returned only.
		if (rq.Adjacent() && !forceOffChain)
			slot->Publish(rq.from, rq.to, rq.generation, flow);
		engineReady = true;
		result.flow = flow;
		answer(Status::Ready);
		return true;
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
		requestStreak = 0;
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
		const bool pairs = havePrevious && previousGeneration == job.generation && previousSequence + 1 == job.sequence;
		const bool ok = testEngine->job ? testEngine->job(job.sequence, job.generation) : true;
		if (ok && pairs)
			slot->Publish(previousSequence, job.sequence, job.generation, std::make_shared<Flow>());
		{
			std::lock_guard<std::mutex> lock(mutex);
			++stats.frames;
			if (ok && pairs) ++stats.pairs;
		}
		havePrevious = true;
		previousSequence = job.sequence;
		previousGeneration = job.generation;
		hintSequence = UINT64_MAX;
		return ok;
	}
}
