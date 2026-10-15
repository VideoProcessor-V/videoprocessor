#pragma once

#include <d3d11_4.h>
#include <atlbase.h>
#include <libplacebo/gpu.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Roadmap (owner 2026-10-15, option A of the pinned-memory work): libplacebo's
// D3D11 backend has no host-mapped buffers or buffer transfers (probed:
// max_mapped_size 0, buf_transfer 0), so a raw capture frame's upload is a
// CPU copy into the driver (~2.4 ms at 4K 12-bit) plus the transfer (~3 ms),
// on the render thread. This uploads each frame on the thread that prepares
// it (the conversion thread), as it arrives, with a second D3D11 device on
// the same adapter into a ring of shared textures; the render thread then
// reads the uploaded texture (wrapped for libplacebo) after a GPU wait.
//
// Safety (anti-tearing):
//   - the render side's GPU work waits on the "uploaded" fence for that
//     slot's upload before reading it (Acquire);
//   - after each GPU use the render side signals the "released" fence
//     (Used); a slot is reused only when every use's release has completed
//     on the GPU AND the payload holding it is gone (its CPU readers done);
//   - never blocks: with no free slot, Upload returns nullptr and the frame
//     takes the normal upload.
// The pool: a slot count from the caller (frames in flight plus a margin),
// capped by bytes (1 GB by default).
class EarlyUpload
{
public:
	struct Slot;
	// Released when the last holder lets go (the payload and anything that
	// copied it): the slot goes back to the free list with its pending
	// release value.
	using SlotHandle = std::shared_ptr<Slot>;

	EarlyUpload() = default;
	~EarlyUpload();
	EarlyUpload(const EarlyUpload&) = delete;
	EarlyUpload& operator=(const EarlyUpload&) = delete;

	// Render thread: the render device (libplacebo's) and its pl_gpu; makes the
	// upload device on the same adapter and the shared fences.
	bool Open(ID3D11Device* renderDevice, pl_gpu gpu, int slots, size_t capBytes = size_t(1) << 30);
	// Render thread: everything released (call before the render device goes).
	void Close();
	bool IsOpen() const { return ready.load(std::memory_order_acquire); }

	// Conversion thread: the raw frame into a free slot (width = rowBytes / 4
	// rgba8 texels x height), then the "uploaded" signal. nullptr when no
	// slot is free or anything fails (the caller uploads normally).
	SlotHandle Upload(const void* data, size_t rowBytes, int height);

	// Render thread: the slot's texture for libplacebo, with a GPU wait for its
	// upload queued first; nullptr on failure. Call Used after queuing the GPU
	// work that reads it (every time it is read).
	pl_tex Acquire(Slot& slot);
	void Used(Slot& slot);

	// noFreeHeld / noFreeGpu: at each no-free-slot, the slots still held by a
	// payload and those free but waiting for their GPU release.
	struct Stats { uint64_t uploads = 0, noFreeSlot = 0, failures = 0, noFreeHeld = 0, noFreeGpu = 0; int slotsMade = 0; };
	Stats GetStats() const;
	const std::string& LastError() const { return error; }
	size_t CapBytes() const { return budgetCap; }   // the pool's byte cap after the memory budget

	struct Slot
	{
		EarlyUpload* owner = nullptr;
		uint64_t pool = 0;           // the pool it came from (a reopened pool refuses it)
		int index = -1;
		uint64_t uploadValue = 0;    // the "uploaded" fence value of its upload
	};

private:
	struct Entry
	{
		CComPtr<ID3D11Texture2D> uploadTex;   // on the upload device
		HANDLE shared = nullptr;              // NT handle of the texture
		CComPtr<ID3D11Texture2D> renderTex;   // opened on the render device
		pl_tex wrapped = nullptr;              // renderTex for libplacebo
		size_t rowBytes = 0;
		int height = 0;
		uint64_t generation = 0;               // bumped when the texture is remade
		uint64_t wrappedGeneration = 0;
		uint64_t releaseValue = 0;             // the last use's "released" value
		bool busy = false;                     // held by a payload
	};
	void Return(int index);
	bool MakeTexture(Entry& e, size_t rowBytes, int height);

	pl_gpu gpu = nullptr;
	CComPtr<ID3D11Device> renderDevice;
	CComPtr<ID3D11DeviceContext4> renderContext;
	CComPtr<ID3D11Device5> uploadDevice;
	CComPtr<ID3D11DeviceContext4> uploadContext;
	CComPtr<ID3D11Fence> uploadedFence, releasedFence;               // upload device side
	CComPtr<ID3D11Fence> uploadedFenceR, releasedFenceR;             // opened on the render device
	uint64_t nextUploadValue = 0;                                    // conversion thread
	uint64_t nextReleaseValue = 0;                                   // render thread
	mutable std::mutex lock;
	// Held by Upload while it uses the upload device, and by Close: the pool
	// cannot go while the conversion thread is mid-upload.
	std::mutex lifecycle;
	uint64_t poolId = 0;
	std::vector<Entry> entries;
	size_t capBytes = 0;
	size_t budgetCap = 0;
	std::atomic<bool> ready{ false };
	Stats stats;
	std::string error;
	// Shared with every handle: a handle released after Close finds no owner.
	struct Control { std::mutex m; EarlyUpload* owner = nullptr; };
	std::shared_ptr<Control> control = std::make_shared<Control>();
};
