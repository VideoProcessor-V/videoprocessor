// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "LongPairService.h"
#include "NvOpticalFlowHinted.h"

#include <VP-V/core/PassTimerInbox.h>
#include <VP-V/core/VramUsers.h>

#include <d3d11.h>
#include <dxgi.h>
#include <atlbase.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
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
		// The matcher backend is used ONLY under backendMutex, and SetBackend takes the same mutex: once SetBackend returns, no
		// thread is inside (or about to enter) the old backend, so its owner may destroy it (a raw pointer copied out under
		// `mutex` and used after the unlock outlived its object).
		std::mutex backendMutex;
		ISeededBackend* backend = nullptr;
		CComPtr<IDXGIAdapter> adapter;       // guarded by mutex; null = SetAdapter not called (the NVOF session refuses to run)

		// GPU state, guarded by execMutex (the worker and RunNow share it).
		std::mutex execMutex;
		CComPtr<ID3D11Device> device;
		CComPtr<ID3D11DeviceContext> context;
		std::unique_ptr<NvOpticalFlow::HintedEngine> engine;
		CComPtr<ID3D11Texture2D> luma[2], hintTex, flow[2], cost[2], flowRead[2], costRead[2];
		int sizeW = 0, sizeH = 0;
		SessionLatch latch;               // latched: the hinted session cannot run for now (visible reason; 30 s, then retried)
		std::atomic<long> injectedRemoved{ 0 };   // test seam: a pending fake GetDeviceRemovedReason() result

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
			if (!a)
			{
				why = NoAdapterReason();   // never guess a default adapter
				return false;
			}
			CComPtr<ID3D11Device> d;
			CComPtr<ID3D11DeviceContext> c;
			if (FAILED(D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
					D3D11_SDK_VERSION, &d, nullptr, &c)))
			{
				why = "cannot create its D3D11 device";
				return false;
			}
			device = d;
			context = c;
			return true;
		}

		static const char* NoAdapterReason()
		{
			return "SetAdapter was not called (no render adapter given: the long-pair NVOF session does not guess one)";
		}

		// Engine and textures first (the engine unregisters them), then the context and the device.
		void DropDevice()
		{
			ReleaseEngine();
			context.Release();
			device.Release();
		}

		// A removed / reset device: release everything on it and latch visibly (the session is retried on a new device after
		// the latch). Returns true when the device is gone; `why` is then the visible reason.
		bool CheckDeviceFault(const char* during, std::string& why)
		{
			HRESULT hr = static_cast<HRESULT>(injectedRemoved.exchange(0));
			if (hr == S_OK && device)
				hr = device->GetDeviceRemovedReason();
			if (hr == S_OK)
				return false;
			DropDevice();
			why = DeviceRemovedText(static_cast<long>(hr), during);
			latch.Latch(why, std::chrono::steady_clock::now());
			return true;
		}

		// A failed Map (staging readback): latch visibly, never retry silently, never hand out stale or half-filled data.
		bool ReadbackFailed(const char* what, HRESULT hr, std::string& why)
		{
			char hex[16];
			std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));
			why = std::string("cannot read the ") + what + " back (Map failed " + hex + ")";
			if (!CheckDeviceFault("the readback", why))
				latch.Latch(why, std::chrono::steady_clock::now());
			return false;
		}

		void ReleaseEngine()
		{
			engine.reset();   // unregisters its textures first
			for (int i = 0; i < 2; ++i)
			{
				luma[i].Release(); flow[i].Release(); cost[i].Release(); flowRead[i].Release(); costRead[i].Release();
			}
			// MV7: the long pair's ring textures leave the governor's budget with the engine.
			VpSkeleton::VramUsers::Instance().Withdraw(this, VpSkeleton::kUserNvofProducer);
			VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserNvofProducer, false);
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
			VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserNvofProducer, true);   // declared below once the engine is open
			engine = std::make_unique<NvOpticalFlow::HintedEngine>();
			if (!engine->Open(device, context, w, h, static_cast<NvOpticalFlow::Preset>(config.nvofPreset)))
			{
				why = engine->LastError();
				engine.reset();
				ReleaseEngine();
				return false;
			}
			// MV7 (the long pair is the 3rd NVOF user): its textures and hint (the readback staging copies are system RAM), counted by the governor (VramUsers.h).
			VpSkeleton::VramUsers::Instance().DeclareFixed(this, VpSkeleton::kUserNvofProducer, NvOpticalFlow::RingBytes(w, h, false, true));
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
			bool haveAdapter;
			{
				std::lock_guard<std::mutex> lock(mutex);
				haveAdapter = adapter.p != nullptr;
			}
			if (!haveAdapter)
			{
				// Not latched: it is a missing call, and SetAdapter makes the next pair run the session.
				why = NoAdapterReason();
				return false;
			}
			// A device / driver / size / readback problem is latched for 30 s, then the session is tried again (a busy engine or
			// one odd frame size must not disable the service for the whole process).
			if (latch.Blocked(std::chrono::steady_clock::now(), why))
				return false;
			if (device && CheckDeviceFault("the start of the pair", why))
				return false;
			if (!EnsureDevice(why))
			{
				latch.Latch(why, std::chrono::steady_clock::now());
				return false;
			}
			if (!EnsureSize(r.w, r.h, why))
			{
				if (!CheckDeviceFault("opening the session", why))
					latch.Latch(why, std::chrono::steady_clock::now());
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
				if (CheckDeviceFault("the NVOF execute", why))
					return false;
				latch.NoteExecuteFailure(why, std::chrono::steady_clock::now());
				return false;
			}
			latch.NoteSuccess();
			const int gw = GridOf(r.w), gh = GridOf(r.h);
			for (int i = 0; i < 2; ++i)
			{
				context->CopyResource(flowRead[i], flow[i]);
				context->CopyResource(costRead[i], cost[i]);
			}
			// Read into a local field; `out` is only set when every Map succeeded.
			Field tmp;
			tmp.gridW = gw;
			tmp.gridH = gh;
			for (int i = 0; i < 2; ++i)
			{
				std::vector<int16_t>& v = i == 0 ? tmp.forward : tmp.backward;
				std::vector<uint8_t>& c = i == 0 ? tmp.forwardCost : tmp.backwardCost;
				v.resize(static_cast<size_t>(gw) * gh * 2);
				c.resize(static_cast<size_t>(gw) * gh);
				HRESULT hr = S_OK;
				if (!NvOpticalFlow::ReadStagingRows(context, flowRead[i], gh, static_cast<size_t>(gw) * 4, reinterpret_cast<uint8_t*>(v.data()), hr))
					return ReadbackFailed("flow", hr, why);
				if (!NvOpticalFlow::ReadStagingRows(context, costRead[i], gh, static_cast<size_t>(gw), c.data(), hr))
					return ReadbackFailed("cost", hr, why);
			}
			out = std::move(tmp);
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
				res.field = Field();   // nothing from the failed NVOF attempt may leak into the fallback's result
				const HintField* hint = r.hasHint ? &r.hint : nullptr;
				std::string err;
				bool ok = false, usedBackend = false;
				{
					std::lock_guard<std::mutex> backendLock(backendMutex);
					if (backend)
					{
						usedBackend = true;
						ok = backend->Run(r.a.data(), r.b.data(), r.w, r.h, hint, res.field, err);
					}
				}
				if (!usedBackend)
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
			// MV4: the pass-timer route. Every long pair is posted (one per I frame: rare, so no sampling): the NVOF session's pair plus its
			// readback as the GPU group, the seeded-search fallback as CPU work, so the cost shows in the same readout as the producer's.
			if (res.ok && (res.path == Path::NvofHinted || res.path == Path::NvofUnhinted))
				PassTimers::Inbox::Motion().Post(PassTimers::Kind::GpuGroup, PassTimers::Rows::kLongPairNvof, res.ms);
			else if (res.ok && res.path == Path::SeededSearch)
				PassTimers::Inbox::Motion().Post(PassTimers::Kind::CpuWork, PassTimers::Rows::kLongPairSeeded, res.ms);
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
				const uint64_t ticket = res.ticket;
				std::lock_guard<std::mutex> lock(mutex);
				results[ticket] = std::move(res);
				resultOrder.push_back(ticket);
				++stats.stored;
				// resultOrder holds exactly the tickets in `results` (TryTake removes its entry), so the bound counts live
				// results and each evicted result is counted once.
				while (results.size() > config.maxResults && !resultOrder.empty())
				{
					if (results.erase(resultOrder.front()) > 0)
						++stats.evicted;
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

	// Same adapter = same LUID (a re-enumerated COM object for the same GPU is not a change); an unreadable desc counts as different.
	static bool SameAdapter(IDXGIAdapter* a, IDXGIAdapter* b)
	{
		if (a == b)
			return true;
		if (!a || !b)
			return false;
		DXGI_ADAPTER_DESC da{}, db{};
		if (FAILED(a->GetDesc(&da)) || FAILED(b->GetDesc(&db)))
			return false;
		return da.AdapterLuid.LowPart == db.AdapterLuid.LowPart && da.AdapterLuid.HighPart == db.AdapterLuid.HighPart;
	}

	void Service::SetAdapter(IDXGIAdapter* adapter)
	{
		// Same lock order as the GPU path (execMutex, then mutex). A different adapter drops the device built on the old one and
		// clears the latch, so the next pair opens the session on the new adapter at once. Null = not set: the NVOF session
		// refuses to run (visibly) rather than guessing the default adapter.
		std::lock_guard<std::mutex> exec(impl->execMutex);
		bool changed;
		{
			std::lock_guard<std::mutex> lock(impl->mutex);
			changed = !SameAdapter(impl->adapter.p, adapter);
			impl->adapter = adapter;
		}
		if (changed)
		{
			impl->DropDevice();
			impl->latch.Clear();
		}
	}

	void Service::InjectDeviceRemovedForTest(long hr)
	{
		impl->injectedRemoved = hr;
	}

	void Service::SetBackend(ISeededBackend* backend)
	{
		// Waits for a Run in flight on the old backend: when this returns the old one is no longer referenced.
		std::lock_guard<std::mutex> lock(impl->backendMutex);
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
		impl->resultOrder.erase(std::remove(impl->resultOrder.begin(), impl->resultOrder.end(), ticket), impl->resultOrder.end());
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
