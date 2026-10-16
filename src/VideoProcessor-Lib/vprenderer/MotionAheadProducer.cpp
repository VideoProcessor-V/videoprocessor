#include <pch.h>
#include "MotionAheadProducer.h"

#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#pragma comment(lib, "d3d11.lib")

namespace MotionAhead
{
	bool HalfLuma8(const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& halfW, int& halfH)
	{
		if (!source.IsValid()) return false;
		const int w = source.width, h = source.height;
		halfW = (w + 1) / 2;
		halfH = (h + 1) / 2;
		out.resize(static_cast<size_t>(halfW) * halfH);
		// A row of 10-bit luma codes, decoded directly for the common capture formats
		// (P010 / P210 planes, v210, UYVY / HDYC); false = use the sampler (RGB).
		const bool planar16 = source.format == AnalysisLumaFormat::P010 || source.format == AnalysisLumaFormat::P210;
		const bool v210 = source.format == AnalysisLumaFormat::NativeYuv422 && source.encoding == VideoFrameEncoding::V210;
		const bool uyvy = source.format == AnalysisLumaFormat::NativeYuv422 &&
			(source.encoding == VideoFrameEncoding::UYVY || source.encoding == VideoFrameEncoding::HDYC);
		auto decodeRow = [&](int y, uint16_t* codes) {
			const uint8_t* row = source.data + static_cast<size_t>(y) * source.rowBytes;
			if (planar16)
			{
				const uint16_t* words = reinterpret_cast<const uint16_t*>(row);
				for (int x = 0; x < w; ++x) codes[x] = static_cast<uint16_t>(words[x] >> 6);
			}
			else if (v210)
			{
				// Six pixels per 16 bytes: Y0 = w0>>10, Y1 = w1, Y2 = w1>>20, Y3 = w2>>10, Y4 = w3, Y5 = w3>>20.
				for (int x = 0; x < w; x += 6)
				{
					const uint8_t* g = row + static_cast<size_t>(x / 6) * 16;
					uint32_t word[4];
					std::memcpy(word, g, 16);
					const uint16_t y6[6] = {
						static_cast<uint16_t>((word[0] >> 10) & 0x3ff), static_cast<uint16_t>(word[1] & 0x3ff),
						static_cast<uint16_t>((word[1] >> 20) & 0x3ff), static_cast<uint16_t>((word[2] >> 10) & 0x3ff),
						static_cast<uint16_t>(word[3] & 0x3ff), static_cast<uint16_t>((word[3] >> 20) & 0x3ff) };
					for (int k = 0; k < 6 && x + k < w; ++k) codes[x + k] = y6[k];
				}
			}
			else
			{
				for (int x = 0; x < w; ++x) codes[x] = static_cast<uint16_t>(row[static_cast<size_t>(x / 2) * 4 + ((x & 1) ? 3 : 1)] << 2);
			}
		};
		if (planar16 || v210 || uyvy)
		{
			std::vector<uint16_t> r0(static_cast<size_t>(w)), r1(static_cast<size_t>(w));
			for (int y = 0; y < halfH; ++y)
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
			return true;
		}
		// Other formats (RGB): the sampler, one sample per output pixel.
		AnalysisLumaSample sample;
		for (int y = 0; y < halfH; ++y)
		{
			uint8_t* dst = &out[static_cast<size_t>(y) * halfW];
			for (int x = 0; x < halfW; ++x)
				dst[x] = source.Sample((std::min)(2 * x, w - 1), (std::min)(2 * y, h - 1), sample) ?
					static_cast<uint8_t>((std::min)(255, sample.luma >> 2)) : 0;
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
