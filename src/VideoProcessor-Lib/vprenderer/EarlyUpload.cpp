#include <pch.h>
#include "EarlyUpload.h"

#include <libplacebo/d3d11.h>
#include <dxgi1_4.h>

#pragma comment(lib, "d3d11.lib")

EarlyUpload::~EarlyUpload()
{
	Close();
}

bool EarlyUpload::Open(ID3D11Device* renderDeviceIn, pl_gpu gpuIn, int slots, size_t capBytesIn)
{
	Close();
	std::lock_guard<std::mutex> life(lifecycle);
	++poolId;
	error.clear();
	if (!renderDeviceIn || !gpuIn || slots <= 0)
	{
		error = "no render device";
		return false;
	}
	gpu = gpuIn;
	renderDevice = renderDeviceIn;
	CComPtr<ID3D11DeviceContext> immediate;
	renderDevice->GetImmediateContext(&immediate);
	if (!immediate || FAILED(immediate->QueryInterface(IID_PPV_ARGS(&renderContext))))
	{
		error = "the render context has no fences (D3D11.4)";
		Close();
		return false;
	}
	// The upload device on the render device's adapter.
	CComPtr<IDXGIDevice> dxgiDevice;
	CComPtr<IDXGIAdapter> adapter;
	if (FAILED(renderDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)))
	{
		error = "no adapter for the render device";
		Close();
		return false;
	}
	// The pool's bytes: the cap, and at most a quarter of the adapter's video
	// memory budget (Windows' own figure: local memory on a discrete card, the
	// shared budget on an integrated one, whose dedicated size is tiny); a
	// 4 GB laptop GPU keeps room for the render.
	CComPtr<IDXGIAdapter3> adapter3;
	DXGI_QUERY_VIDEO_MEMORY_INFO budget{};
	DXGI_ADAPTER_DESC adapterDesc{};
	if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) &&
		SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &budget)) && budget.Budget > 0)
		capBytesIn = std::min<size_t>(capBytesIn, static_cast<size_t>(budget.Budget / 4));
	else if (SUCCEEDED(adapter->GetDesc(&adapterDesc)))
		capBytesIn = std::min<size_t>(capBytesIn,
			std::max<size_t>(adapterDesc.DedicatedVideoMemory, adapterDesc.SharedSystemMemory) / 4);
	budgetCap = capBytesIn;
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	CComPtr<ID3D11Device> device;
	CComPtr<ID3D11DeviceContext> context;
	if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
			&device, nullptr, &context)) ||
		FAILED(device->QueryInterface(IID_PPV_ARGS(&uploadDevice))) ||
		FAILED(context->QueryInterface(IID_PPV_ARGS(&uploadContext))))
	{
		error = "cannot create the upload device (D3D11.4)";
		Close();
		return false;
	}
	// The shared fences, made on the upload device and opened on the render one.
	CComPtr<ID3D11Device5> render5;
	auto openFence = [&](CComPtr<ID3D11Fence>& made, CComPtr<ID3D11Fence>& opened) -> bool
	{
		HANDLE handle = nullptr;
		const bool ok = SUCCEEDED(uploadDevice->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&made))) &&
			SUCCEEDED(made->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)) &&
			SUCCEEDED(render5->OpenSharedFence(handle, IID_PPV_ARGS(&opened)));
		if (handle)
			CloseHandle(handle);
		return ok;
	};
	if (FAILED(renderDevice->QueryInterface(IID_PPV_ARGS(&render5))) ||
		!openFence(uploadedFence, uploadedFenceR) || !openFence(releasedFence, releasedFenceR))
	{
		error = "cannot share fences between the devices";
		Close();
		return false;
	}
	{
		std::lock_guard<std::mutex> guard(lock);
		entries.assign(static_cast<size_t>(slots), Entry{});
		capBytes = capBytesIn;
		nextUploadValue = 0;
		nextReleaseValue = 0;
		stats = Stats{};
	}
	{
		std::lock_guard<std::mutex> guard(control->m);
		control->owner = this;
	}
	ready.store(true, std::memory_order_release);
	return true;
}

void EarlyUpload::Close()
{
	ready.store(false, std::memory_order_release);
	std::lock_guard<std::mutex> life(lifecycle);
	{
		// Handles still held (payloads in a queue) find no owner from here on.
		std::lock_guard<std::mutex> guard(control->m);
		control->owner = nullptr;
	}
	control = std::make_shared<Control>();
	std::lock_guard<std::mutex> guard(lock);
	for (Entry& e : entries)
	{
		if (e.wrapped && gpu)
			pl_tex_destroy(gpu, &e.wrapped);
		e.renderTex.Release();
		if (e.shared)
			CloseHandle(e.shared);
		e.uploadTex.Release();
	}
	entries.clear();
	uploadedFenceR.Release();
	releasedFenceR.Release();
	uploadedFence.Release();
	releasedFence.Release();
	uploadContext.Release();
	uploadDevice.Release();
	renderContext.Release();
	renderDevice.Release();
	gpu = nullptr;
}

bool EarlyUpload::MakeTexture(Entry& e, size_t rowBytes, int height)
{
	if (e.uploadTex && e.rowBytes == rowBytes && e.height == height)
		return true;
	e.uploadTex.Release();
	if (e.shared)
	{
		CloseHandle(e.shared);
		e.shared = nullptr;
	}
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(rowBytes / 4);
	desc.Height = static_cast<UINT>(height);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	CComPtr<IDXGIResource1> resource;
	if (FAILED(uploadDevice->CreateTexture2D(&desc, nullptr, &e.uploadTex)) ||
		FAILED(e.uploadTex->QueryInterface(IID_PPV_ARGS(&resource))) ||
		FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &e.shared)))
	{
		e.uploadTex.Release();
		return false;
	}
	e.rowBytes = rowBytes;
	e.height = height;
	++e.generation;
	++stats.slotsMade;
	return true;
}

EarlyUpload::SlotHandle EarlyUpload::Upload(const void* data, size_t rowBytes, int height)
{
	if (!data || rowBytes == 0 || rowBytes % 4 != 0 || height <= 0)
		return nullptr;
	std::lock_guard<std::mutex> life(lifecycle);
	if (!ready.load(std::memory_order_acquire))
		return nullptr;
	int index = -1;
	{
		std::lock_guard<std::mutex> guard(lock);
		// The pool's bytes, capped (a bigger frame than the cap allows: none).
		const size_t frameBytes = rowBytes * static_cast<size_t>(height);
		const size_t allowed = capBytes / std::max<size_t>(frameBytes, 1);
		// A free slot whose every GPU use has been released.
		const uint64_t released = releasedFence->GetCompletedValue();
		for (size_t i = 0; i < entries.size() && i < allowed; ++i)
		{
			Entry& e = entries[i];
			if (!e.busy && released >= e.releaseValue)
			{
				index = static_cast<int>(i);
				e.busy = true;
				break;
			}
		}
		if (index < 0)
		{
			++stats.noFreeSlot;
			for (size_t i = 0; i < entries.size() && i < allowed; ++i)
				++(entries[i].busy ? stats.noFreeHeld : stats.noFreeGpu);
			return nullptr;
		}
		if (!MakeTexture(entries[static_cast<size_t>(index)], rowBytes, height))
		{
			entries[static_cast<size_t>(index)].busy = false;
			++stats.failures;
			return nullptr;
		}
	}
	// The copy and the signal on this thread's own device (no lock needed:
	// the slot is ours until returned).
	Entry& e = entries[static_cast<size_t>(index)];
	uploadContext->UpdateSubresource(e.uploadTex, 0, nullptr, data, static_cast<UINT>(rowBytes), 0);
	const uint64_t value = ++nextUploadValue;
	uploadContext->Signal(uploadedFence, value);
	uploadContext->Flush();
	{
		std::lock_guard<std::mutex> guard(lock);
		++stats.uploads;
	}
	std::shared_ptr<Control> c = control;
	Slot* raw = new Slot{ this, poolId, index, value };
	return SlotHandle(raw, [c](Slot* s)
	{
		{
			std::lock_guard<std::mutex> guard(c->m);
			if (c->owner)
				c->owner->Return(s->index);
		}
		delete s;
	});
}

void EarlyUpload::Return(int index)
{
	std::lock_guard<std::mutex> guard(lock);
	if (index >= 0 && static_cast<size_t>(index) < entries.size())
		entries[static_cast<size_t>(index)].busy = false;
}

pl_tex EarlyUpload::Acquire(Slot& slot)
{
	if (!ready.load(std::memory_order_acquire) || slot.owner != this || slot.pool != poolId || slot.index < 0 ||
		static_cast<size_t>(slot.index) >= entries.size())
		return nullptr;
	Entry& e = entries[static_cast<size_t>(slot.index)];
	if (e.wrappedGeneration != e.generation || !e.wrapped)
	{
		if (e.wrapped)
			pl_tex_destroy(gpu, &e.wrapped);
		e.renderTex.Release();
		CComPtr<ID3D11Device1> render1;
		if (FAILED(renderDevice->QueryInterface(IID_PPV_ARGS(&render1))) ||
			FAILED(render1->OpenSharedResource1(e.shared, IID_PPV_ARGS(&e.renderTex))))
		{
			error = "cannot open the shared texture on the render device";
			return nullptr;
		}
		pl_d3d11_wrap_params wrap{};
		wrap.tex = e.renderTex;
		e.wrapped = pl_d3d11_wrap(gpu, &wrap);
		if (!e.wrapped)
		{
			error = "cannot wrap the shared texture for libplacebo";
			return nullptr;
		}
		e.wrappedGeneration = e.generation;
	}
	// The render side's GPU work waits for this slot's upload.
	renderContext->Wait(uploadedFenceR, slot.uploadValue);
	return e.wrapped;
}

void EarlyUpload::Used(Slot& slot)
{
	if (!ready.load(std::memory_order_acquire) || slot.owner != this || slot.pool != poolId || slot.index < 0 ||
		static_cast<size_t>(slot.index) >= entries.size())
		return;
	const uint64_t value = ++nextReleaseValue;
	renderContext->Signal(releasedFenceR, value);
	std::lock_guard<std::mutex> guard(lock);
	entries[static_cast<size_t>(slot.index)].releaseValue = value;
}

EarlyUpload::Stats EarlyUpload::GetStats() const
{
	std::lock_guard<std::mutex> guard(lock);
	return stats;
}
