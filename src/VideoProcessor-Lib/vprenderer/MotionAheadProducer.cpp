#include <pch.h>
#include "MotionAheadProducer.h"

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
		auto decodeRow = [&](int y, uint16_t* codes) {
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
				decodeRow((std::min)(2 * y, h - 1), r0.data());
				decodeRow((std::min)(2 * y + 1, h - 1), r1.data());
				uint8_t* dst = &out[static_cast<size_t>(y) * halfW];
				for (int x = 0; x < halfW; ++x)
				{
					const int x0 = (std::min)(2 * x, w - 1), x1 = (std::min)(2 * x + 1, w - 1);
					const unsigned sum = r0[x0] + r0[x1] + r1[x0] + r1[x1];
					dst[x] = static_cast<uint8_t>((std::min)(255u, sum >> 4));   // the mean code / 4
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
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		device.Release();
		context.Release();
		if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
				&device, nullptr, &context)))
		{
			error = "cannot create its D3D11 device";
			return false;
		}
		stopping = false;
		jobs.clear();
		havePrevious = false;
		engineReady = false;
		sizeW = sizeH = 0;
		running = true;
		worker = std::thread([this] { Worker(); });
		return true;
	}

	void Producer::Stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			jobs.clear();
		}
		wake.notify_all();
		if (worker.joinable()) worker.join();
		running = false;
		// The engine first (it unregisters its textures), then the textures, then the device.
		engine.reset();
		for (int i = 0; i < 2; ++i)
		{
			lumaTex[i].Release(); flowTex[i].Release(); costTex[i].Release(); flowRead[i].Release(); costRead[i].Release();
		}
		context.Release();
		device.Release();
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

	void Producer::Submit(uint64_t sequence, uint64_t generation, const AnalysisLumaSource& luma)
	{
		if (!running.load(std::memory_order_acquire)) return;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping) return;
			// Never let the work pile up: at most a few frames waiting (the oldest dropped).
			while (jobs.size() >= 6)
			{
				jobs.pop_front();
				++stats.dropped;
			}
			jobs.push_back(Job{ sequence, generation, luma });
		}
		lastSubmitted.store(sequence, std::memory_order_release);
		wake.notify_all();
	}

	FlowPtr Producer::Take(uint64_t from, uint64_t to)
	{
		VectorStore<FlowPtr>::Entry entry;
		return store.Get(from, to, entry) ? entry.payload : nullptr;
	}

	void Producer::Worker()
	{
		for (;;)
		{
			Job job;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [this] { return stopping || !jobs.empty(); });
				if (stopping) return;
				job = std::move(jobs.front());
				jobs.pop_front();
			}
			if (!Process(job))
			{
				std::lock_guard<std::mutex> lock(mutex);
				++stats.failures;
				if (stats.failures >= 3)
				{
					// Persistent failure: stop for the session (the matcher covers everything).
					running = false;
					engineReady = false;
					return;
				}
			}
		}
	}

	bool Producer::EnsureSize(int halfW, int halfH)
	{
		if (halfW == sizeW && halfH == sizeH && engine && engine->IsOpen()) return true;
		engine.reset();
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
		return true;
	}

	bool Producer::Process(const Job& job)
	{
		const auto started = std::chrono::steady_clock::now();
		int halfW = 0, halfH = 0;
		if (!HalfLuma8(job.luma, lumaBuffer, halfW, halfH))
			return true;   // not a readable frame: nothing to do, not a failure
		// The de-judder's engine size: the half size, or half again (its inner matcher).
		const int tw = targetW.load(std::memory_order_acquire), th = targetH.load(std::memory_order_acquire);
		if (tw > 0 && th > 0 && !(tw == halfW && th == halfH))
		{
			if (tw != (halfW + 1) / 2 || th != (halfH + 1) / 2)
				return true;   // a size this producer does not make: nothing stored (the engine runs at render time)
			quarterBuffer.resize(static_cast<size_t>(tw) * th);
			for (int y = 0; y < th; ++y)
			{
				const uint8_t* r0 = &lumaBuffer[static_cast<size_t>((std::min)(2 * y, halfH - 1)) * halfW];
				const uint8_t* r1 = &lumaBuffer[static_cast<size_t>((std::min)(2 * y + 1, halfH - 1)) * halfW];
				for (int x = 0; x < tw; ++x)
				{
					const int x0 = (std::min)(2 * x, halfW - 1), x1 = (std::min)(2 * x + 1, halfW - 1);
					quarterBuffer[static_cast<size_t>(y) * tw + x] = static_cast<uint8_t>((r0[x0] + r0[x1] + r1[x0] + r1[x1] + 2) / 4);
				}
			}
			lumaBuffer.swap(quarterBuffer);
			halfW = tw;
			halfH = th;
		}
		const double lumaMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (!EnsureSize(halfW, halfH)) return false;
		// A new continuity generation clears the store and the chain.
		if (store.Generation() != job.generation)
		{
			store.SetGeneration(job.generation);
			havePrevious = false;
		}
		const int slot = havePrevious ? 1 - current : current;
		context->UpdateSubresource(lumaTex[slot], 0, nullptr, lumaBuffer.data(), static_cast<UINT>(halfW), 0);
		bool ok = true;
		if (havePrevious && previousGeneration == job.generation && previousSequence < job.sequence)
		{
			const int prev = current;
			if (!engine->Run(lumaTex[prev], lumaTex[slot], flowTex[0], flowTex[1], costTex[0], costTex[1], nullptr, true))
			{
				std::lock_guard<std::mutex> lock(mutex);
				error = engine->LastError();
				ok = false;
			}
			else
			{
				for (int i = 0; i < 2; ++i)
				{
					context->CopyResource(flowRead[i], flowTex[i]);
					context->CopyResource(costRead[i], costTex[i]);
				}
				auto flow = std::make_shared<Flow>();
				flow->halfW = halfW; flow->halfH = halfH;
				flow->gridW = (halfW + 3) / 4; flow->gridH = (halfH + 3) / 4;
				const size_t cells = static_cast<size_t>(flow->gridW) * flow->gridH;
				for (int i = 0; i < 2 && ok; ++i)
				{
					std::vector<int16_t>& vectors = i == 0 ? flow->forward : flow->backward;
					std::vector<uint8_t>& costs = i == 0 ? flow->forwardCost : flow->backwardCost;
					vectors.resize(cells * 2);
					costs.resize(cells);
					D3D11_MAPPED_SUBRESOURCE map{};
					if (FAILED(context->Map(flowRead[i], 0, D3D11_MAP_READ, 0, &map))) { ok = false; break; }
					for (int y = 0; y < flow->gridH; ++y)
						std::memcpy(&vectors[static_cast<size_t>(y) * flow->gridW * 2], static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
							static_cast<size_t>(flow->gridW) * 4);
					context->Unmap(flowRead[i], 0);
					if (FAILED(context->Map(costRead[i], 0, D3D11_MAP_READ, 0, &map))) { ok = false; break; }
					for (int y = 0; y < flow->gridH; ++y)
						std::memcpy(&costs[static_cast<size_t>(y) * flow->gridW], static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
							static_cast<size_t>(flow->gridW));
					context->Unmap(costRead[i], 0);
				}
				if (ok)
				{
					// Planner Q1: the whole frame's motion, here on the worker (no render cost).
					FlowGlobal(flow->forward, flow->forwardCost, flow->gridW, flow->gridH, flow->global[0], flow->second[0]);
					FlowGlobal(flow->backward, flow->backwardCost, flow->gridW, flow->gridH, flow->global[1], flow->second[1]);
					store.Put(previousSequence, job.sequence, job.generation, flow);
					engineReady = true;
					std::lock_guard<std::mutex> lock(mutex);
					++stats.pairs;
					stats.lastPairMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
				}
			}
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			++stats.frames;
			stats.lumaMs = lumaMs;
		}
		current = slot;
		havePrevious = true;
		previousSequence = job.sequence;
		previousGeneration = job.generation;
		return ok;
	}
}
