// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "LongPairService.h"
#include "NvOpticalFlowHinted.h"

#include <d3d11.h>
#include <dxgi.h>
#include <atlbase.h>

#include <chrono>
#include <cstring>

namespace VpLongPair
{
	struct Request
	{
		uint64_t ticket = 0;
		std::vector<uint8_t> a, b;
		int w = 0, h = 0;
		bool hasHint = false;
		HintField hint;
	};

	struct Service::Impl
	{
		Config config;
		mutable std::mutex mutex;         // queue, results, stats, status
		std::condition_variable wake;
		std::deque<Request> queue;
		std::map<uint64_t, Result> results;
		std::deque<uint64_t> resultOrder;
		uint64_t nextTicket = 1;
		Stats stats;
		Path lastPath = Path::None;
		Why lastWhy;
		bool stop = false;
		std::thread worker;
		ISeededBackend* backend = nullptr;
		CComPtr<IDXGIAdapter> adapter;

		// GPU state, guarded by execMutex (the worker and RunNow share it).
		std::mutex execMutex;
		CComPtr<ID3D11Device> device;
		CComPtr<ID3D11DeviceContext> context;
		std::unique_ptr<NvOpticalFlow::HintedEngine> engine;
		CComPtr<ID3D11Texture2D> luma[2], hintTex, flow[2], cost[2], flowRead[2], costRead[2];
		int sizeW = 0, sizeH = 0;
		std::string nvofLatched;          // non-empty: the hinted session cannot run for this session (visible reason)
		int consecutiveFailures = 0;
		std::chrono::steady_clock::time_point latchedAt;

		explicit Impl(const Config& c) : config(c) {}

		bool EnsureDevice(std::string& why)
		{
			if (device)
				return true;
			const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			CComPtr<IDXGIAdapter> a;
			{
				std::lock_guard<std::mutex> lock(mutex);
				a = adapter;
			}
			CComPtr<ID3D11Device> d;
			CComPtr<ID3D11DeviceContext> c;
			if (FAILED(D3D11CreateDevice(a, a ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
					D3D11_SDK_VERSION, &d, nullptr, &c)))
			{
				why = "cannot create its D3D11 device";
				return false;
			}
			device = d;
			context = c;
			return true;
		}

		void ReleaseEngine()
		{
			engine.reset();   // unregisters its textures first
			for (int i = 0; i < 2; ++i)
			{
				luma[i].Release(); flow[i].Release(); cost[i].Release(); flowRead[i].Release(); costRead[i].Release();
			}
			hintTex.Release();
			sizeW = sizeH = 0;
		}

		bool MakeTex(int w, int h, DXGI_FORMAT format, bool staging, CComPtr<ID3D11Texture2D>& tex)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = static_cast<UINT>(w); desc.Height = static_cast<UINT>(h);
			desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = format; desc.SampleDesc.Count = 1;
			desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
			desc.BindFlags = staging ? 0 : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
			desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
			return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex));
		}

		bool EnsureSize(int w, int h, std::string& why)
		{
			if (engine && engine->IsOpen() && w == sizeW && h == sizeH)
				return true;
			ReleaseEngine();
			const int gw = GridOf(w), gh = GridOf(h);
			for (int i = 0; i < 2; ++i)
				if (!MakeTex(w, h, NvOpticalFlow::Engine::InputFormat(), false, luma[i]) ||
					!MakeTex(gw, gh, NvOpticalFlow::Engine::FlowFormat(), false, flow[i]) ||
					!MakeTex(gw, gh, NvOpticalFlow::Engine::CostFormat(), false, cost[i]) ||
					!MakeTex(gw, gh, NvOpticalFlow::Engine::FlowFormat(), true, flowRead[i]) ||
					!MakeTex(gw, gh, NvOpticalFlow::Engine::CostFormat(), true, costRead[i]))
				{
					why = "cannot create its textures";
					ReleaseEngine();
					return false;
				}
			if (!MakeTex(gw, gh, NvOpticalFlow::Engine::FlowFormat(), false, hintTex))
			{
				why = "cannot create its hint texture";
				ReleaseEngine();
				return false;
			}
			engine = std::make_unique<NvOpticalFlow::HintedEngine>();
			if (!engine->Open(device, context, w, h, static_cast<NvOpticalFlow::Preset>(config.nvofPreset)))
			{
				why = engine->LastError();
				engine.reset();
				ReleaseEngine();
				return false;
			}
			sizeW = w;
			sizeH = h;
			return true;
		}

		// The hinted (or unhinted) NVOF pair; false + why = take the fallback.
		bool RunNvof(const Request& r, Field& out, std::string& why, bool& hintBound)
		{
			hintBound = false;
			if (!config.useNvof)
			{
				why = "long-pair NVOF off";
				return false;
			}
			if (!nvofLatched.empty())
			{
				// A device / driver / size problem is latched for 30 s, then the session is tried again (a busy engine or
				// one odd frame size must not disable the service for the whole process).
				if (std::chrono::steady_clock::now() - latchedAt < std::chrono::seconds(30))
				{
					why = nvofLatched;
					return false;
				}
				nvofLatched.clear();
				consecutiveFailures = 0;
			}
			if (!EnsureDevice(why) || !EnsureSize(r.w, r.h, why))
			{
				nvofLatched = why;
				latchedAt = std::chrono::steady_clock::now();
				return false;
			}
			context->UpdateSubresource(luma[0], 0, nullptr, r.a.data(), static_cast<UINT>(r.w), 0);
			context->UpdateSubresource(luma[1], 0, nullptr, r.b.data(), static_cast<UINT>(r.w), 0);
			ID3D11Texture2D* hintForRun = nullptr;
			if (r.hasHint)
			{
				std::vector<int16_t> s;
				if (ResampleHint(r.hint, r.w, r.h, s))
				{
					context->UpdateSubresource(hintTex, 0, nullptr, s.data(), static_cast<UINT>(GridOf(r.w) * 4), 0);
					hintForRun = hintTex;
					hintBound = true;
				}
			}
			if (!engine->Run(luma[0], luma[1], hintForRun, flow[0], flow[1], cost[0], cost[1]))
			{
				why = engine->LastError();
				if (++consecutiveFailures >= 3)
				{
					nvofLatched = why;
					latchedAt = std::chrono::steady_clock::now();
				}
				return false;
			}
			consecutiveFailures = 0;
			const int gw = GridOf(r.w), gh = GridOf(r.h);
			for (int i = 0; i < 2; ++i)
			{
				context->CopyResource(flowRead[i], flow[i]);
				context->CopyResource(costRead[i], cost[i]);
			}
			out.gridW = gw;
			out.gridH = gh;
			for (int i = 0; i < 2; ++i)
			{
				std::vector<int16_t>& v = i == 0 ? out.forward : out.backward;
				std::vector<uint8_t>& c = i == 0 ? out.forwardCost : out.backwardCost;
				v.resize(static_cast<size_t>(gw) * gh * 2);
				c.resize(static_cast<size_t>(gw) * gh);
				D3D11_MAPPED_SUBRESOURCE map{};
				if (FAILED(context->Map(flowRead[i], 0, D3D11_MAP_READ, 0, &map)))
				{
					why = "cannot read the flow back";
					return false;
				}
				for (int y = 0; y < gh; ++y)
					std::memcpy(&v[static_cast<size_t>(y) * gw * 2], static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
						static_cast<size_t>(gw) * 4);
				context->Unmap(flowRead[i], 0);
				if (FAILED(context->Map(costRead[i], 0, D3D11_MAP_READ, 0, &map)))
				{
					why = "cannot read the cost back";
					return false;
				}
				for (int y = 0; y < gh; ++y)
					std::memcpy(&c[static_cast<size_t>(y) * gw], static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
						static_cast<size_t>(gw));
				context->Unmap(costRead[i], 0);
			}
			return true;
		}

		Result Process(const Request& r)
		{
			const auto t0 = std::chrono::steady_clock::now();
			Result res;
			res.ticket = r.ticket;
			res.why.hintSupplied = r.hasHint;
			std::string why;
			bool nvof = false, hintBound = false;
			ISeededBackend* be;
			{
				std::lock_guard<std::mutex> lock(mutex);
				be = backend;
			}
			{
				std::lock_guard<std::mutex> lock(execMutex);
				nvof = RunNvof(r, res.field, why, hintBound);
			}
			if (nvof)
			{
				res.ok = true;
				res.path = hintBound ? Path::NvofHinted : Path::NvofUnhinted;
			}
			else
			{
				res.why.reason = why;
				const HintField* hint = r.hasHint ? &r.hint : nullptr;
				std::string err;
				bool ok = false;
				if (be)
					ok = be->Run(r.a.data(), r.b.data(), r.w, r.h, hint, res.field, err);
				else
					ok = SeededSearchPair(r.a.data(), r.b.data(), r.w, r.h, hint, res.field, config.searchRadius);
				res.ok = ok;
				res.path = ok ? Path::SeededSearch : Path::None;
				if (!ok)
				{
					res.error = err.empty() ? "seeded search failed" : err;
					res.why.reason = res.error;   // visible: a failed pair is not "idle"
				}
			}
			res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			{
				std::lock_guard<std::mutex> lock(mutex);
				++stats.done;
				if (!res.ok) ++stats.failed;
				else if (res.path == Path::NvofHinted) ++stats.nvofHinted;
				else if (res.path == Path::NvofUnhinted) ++stats.nvofUnhinted;
				else ++stats.seeded;
				lastPath = res.path;
				lastWhy = res.why;
			}
			return res;
		}

		void Worker()
		{
			for (;;)
			{
				Request r;
				{
					std::unique_lock<std::mutex> lock(mutex);
					wake.wait(lock, [&] { return stop || !queue.empty(); });
					if (stop)
						return;
					r = std::move(queue.front());
					queue.pop_front();
				}
				Result res = Process(r);
				std::lock_guard<std::mutex> lock(mutex);
				results[res.ticket] = std::move(res);
				resultOrder.push_back(r.ticket);
				while (resultOrder.size() > config.maxResults)
				{
					results.erase(resultOrder.front());
					resultOrder.pop_front();
				}
			}
		}
	};

	Service::Service(const Config& config) : impl(new Impl(config))
	{
		impl->worker = std::thread([this] { impl->Worker(); });
	}

	Service::~Service()
	{
		{
			std::lock_guard<std::mutex> lock(impl->mutex);
			impl->stop = true;
		}
		impl->wake.notify_all();
		if (impl->worker.joinable())
			impl->worker.join();
		// The engine first (it unregisters its textures), then the device: Impl members release in that order via ReleaseEngine.
		std::lock_guard<std::mutex> lock(impl->execMutex);
		impl->ReleaseEngine();
	}

	void Service::SetAdapter(IDXGIAdapter* adapter)
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->adapter = adapter;
	}

	void Service::SetBackend(ISeededBackend* backend)
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->backend = backend;
	}

	static bool Fill(Request& r, const uint8_t* a, const uint8_t* b, int w, int h, const HintField* hint)
	{
		if (!a || !b || w < 32 || h < 32)
			return false;
		const size_t n = static_cast<size_t>(w) * h;
		r.a.assign(a, a + n);
		r.b.assign(b, b + n);
		r.w = w;
		r.h = h;
		r.hasHint = hint && hint->Valid();
		if (r.hasHint)
			r.hint = *hint;
		return true;
	}

	uint64_t Service::SubmitLongPair(const uint8_t* lumaA, const uint8_t* lumaB, int w, int h, const HintField* hint)
	{
		Request r;
		const bool ok = Fill(r, lumaA, lumaB, w, h, hint);
		uint64_t ticket = 0;
		{
			std::lock_guard<std::mutex> lock(impl->mutex);
			++impl->stats.submitted;
			if (!ok || impl->queue.size() >= impl->config.maxPending)
			{
				++impl->stats.refused;
				return 0;
			}
			ticket = impl->nextTicket++;
			r.ticket = ticket;
			impl->queue.push_back(std::move(r));
		}
		impl->wake.notify_one();
		return ticket;
	}

	bool Service::TryTake(uint64_t ticket, Result& out)
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		auto it = impl->results.find(ticket);
		if (it == impl->results.end())
			return false;
		out = std::move(it->second);
		impl->results.erase(it);
		return true;
	}

	Result Service::RunNow(const uint8_t* lumaA, const uint8_t* lumaB, int w, int h, const HintField* hint)
	{
		Request r;
		if (!Fill(r, lumaA, lumaB, w, h, hint))
		{
			Result bad;
			bad.error = "bad size or null plane";
			return bad;
		}
		return impl->Process(r);
	}

	Stats Service::GetStats() const
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->stats;
	}

	std::string Service::StatusLine() const
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		return VpLongPair::StatusLine(impl->lastPath, impl->lastWhy);
	}

	FallbackCatalog::Decision Service::CurrentDecision() const
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		return VpLongPair::Decision(impl->lastPath, impl->lastWhy);
	}
}
