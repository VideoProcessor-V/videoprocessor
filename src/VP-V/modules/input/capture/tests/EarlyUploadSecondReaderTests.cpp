// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// refs/105 phase 5a (owner O3): EarlyUpload's second reader. The reuse law (SlotReusable) as a truth table; on the
// libplacebo test device (WARP unless VP_TEST_GPU=hardware) with a one-slot pool: the slot is not handed out again until
// the payload, the render side AND the reader have released it; the reader reads the uploaded bytes exactly; a reader
// that acquired a slot and never released it (dead) holds it only until DetachReader; a reader attaches again after a
// detach; Close with a reader attached leaves nothing to read.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/input/capture/EarlyUpload.h>

#include <d3d11_4.h>
#include <dxgi.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		CComPtr<ID3D11Device> ReaderDeviceOn(ID3D11Device* fixtureDevice)
		{
			CComPtr<IDXGIDevice> dxgiDevice;
			CComPtr<IDXGIAdapter> adapter;
			CComPtr<ID3D11Device> device;
			if (!fixtureDevice || FAILED(fixtureDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
				FAILED(dxgiDevice->GetAdapter(&adapter)))
				return device;
			const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr, nullptr);
			return device;
		}

		// The reader's copy of a slot, read back on the reader device (after the queued wait for its upload).
		std::vector<uint8_t> ReadBytes(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* tex)
		{
			D3D11_TEXTURE2D_DESC desc{};
			tex->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			desc.MiscFlags = 0;
			CComPtr<ID3D11Texture2D> staging;
			Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), L"staging texture");
			context->CopyResource(staging, tex);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			Assert::IsTrue(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)), L"map");
			const size_t rowBytes = size_t(desc.Width) * 4;
			std::vector<uint8_t> out(rowBytes * desc.Height);
			for (UINT y = 0; y < desc.Height; ++y)
				std::memcpy(&out[size_t(y) * rowBytes], static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, rowBytes);
			context->Unmap(staging, 0);
			return out;
		}

		// Upload until a slot comes back (GPU releases complete asynchronously), at most ~2 s.
		EarlyUpload::SlotHandle UploadWhenFree(EarlyUpload& early, const std::vector<uint8_t>& frame, size_t rowBytes, int height)
		{
			const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			for (;;)
			{
				EarlyUpload::SlotHandle slot = early.Upload(frame.data(), rowBytes, height);
				if (slot || std::chrono::steady_clock::now() > until)
					return slot;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}

		// No slot for ~150 ms (the GPU has long passed every queued release by then).
		bool StaysHeld(EarlyUpload& early, const std::vector<uint8_t>& frame, size_t rowBytes, int height)
		{
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
			while (std::chrono::steady_clock::now() < until)
			{
				if (early.Upload(frame.data(), rowBytes, height))
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return true;
		}
	}

	TEST_CLASS(EarlyUploadSecondReaderTests)
	{
	public:
		TEST_METHOD(SlotReusableTruthTable)
		{
			const uint64_t P = EarlyUpload::kReaderPending;
			// No reader: the render side's law, unchanged.
			Assert::IsTrue(EarlyUpload::SlotReusable(false, 5, 5, false, 0, 0));
			Assert::IsTrue(EarlyUpload::SlotReusable(false, 6, 5, false, 0, 9));    // reader terms ignored without a reader
			Assert::IsFalse(EarlyUpload::SlotReusable(true, 5, 5, false, 0, 0));    // held by a payload
			Assert::IsFalse(EarlyUpload::SlotReusable(false, 4, 5, false, 0, 0));   // render release pending on the GPU
			// A reader attached.
			Assert::IsTrue(EarlyUpload::SlotReusable(false, 5, 5, true, 0, 0));     // never read by the reader
			Assert::IsTrue(EarlyUpload::SlotReusable(false, 5, 5, true, 3, 3));     // reader release complete
			Assert::IsTrue(EarlyUpload::SlotReusable(false, 5, 5, true, 4, 3));
			Assert::IsFalse(EarlyUpload::SlotReusable(false, 5, 5, true, 2, 3));    // reader release pending on the GPU
			Assert::IsFalse(EarlyUpload::SlotReusable(false, 5, 5, true, P, P));    // acquired, not released (even at ~0 completed)
			Assert::IsFalse(EarlyUpload::SlotReusable(false, 5, 5, true, 0, P));
			Assert::IsFalse(EarlyUpload::SlotReusable(true, 5, 5, true, 3, 3));     // both released, still held
			Assert::IsFalse(EarlyUpload::SlotReusable(false, 4, 5, true, 3, 3));    // reader done, render pending
		}

		TEST_METHOD(SlotWaitsForBothReadersAndADeadReaderIsReleasedOnDetach)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			EarlyUpload early;
			if (!early.Open(fixture.Device(), fixture.Gpu(), 1))
			{
				Logger::WriteMessage(("EarlyUpload not available on this device: " + early.LastError() + "\n").c_str());
				return;   // the renderer then keeps the normal upload
			}
			CComPtr<ID3D11Device> reader = ReaderDeviceOn(fixture.Device());
			Assert::IsNotNull(reader.p, L"no reader device on the fixture adapter");
			CComPtr<ID3D11DeviceContext> readerContext;
			reader->GetImmediateContext(&readerContext);
			Assert::IsFalse(early.HasReader());
			Assert::IsTrue(early.AttachReader(reader), std::wstring(early.LastError().begin(), early.LastError().end()).c_str());
			Assert::IsTrue(early.HasReader());

			uint32_t seed = 0x2ead3e5u;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			const int height = 64;
			const size_t rowBytes = 1920 * 2;
			std::vector<uint8_t> frames[4];
			for (auto& f : frames) { f.resize(rowBytes * height); for (uint8_t& b : f) b = static_cast<uint8_t>(next()); }

			auto wide = [&early]() { return std::wstring(early.LastError().begin(), early.LastError().end()); };
			// 1. Both readers use the slot. The payload goes while the reader still holds its copy of the handle: not
			//    reused; the reader releases (UsedReader) and lets go: the slot comes back.
			EarlyUpload::SlotHandle a = early.Upload(frames[0].data(), rowBytes, height);
			Assert::IsTrue(a != nullptr, L"the one slot");
			Assert::IsTrue(early.Upload(frames[1].data(), rowBytes, height) == nullptr, L"held by the payload: none");
			EarlyUpload::SlotHandle readerHold = a;
			Assert::IsNotNull(early.Acquire(*a), wide().c_str());
			early.Used(*a);
			ID3D11Texture2D* readerTex = early.AcquireReader(*readerHold);
			Assert::IsNotNull(readerTex, wide().c_str());
			Assert::IsTrue(ReadBytes(reader, readerContext, readerTex) == frames[0], L"the reader reads the uploaded bytes");
			a.reset();
			pl_gpu_finish(fixture.Gpu());
			Assert::IsTrue(StaysHeld(early, frames[1], rowBytes, height), L"payload and render released, reader still reading: not reused");
			early.UsedReader(*readerHold);
			readerHold.reset();
			EarlyUpload::SlotHandle b = UploadWhenFree(early, frames[1], rowBytes, height);
			Assert::IsTrue(b != nullptr, L"both released: the slot comes back");

			// 2. A dead reader: it acquires the slot and lets go of it without UsedReader. The payload and the render
			//    side release; the slot stays held for the reader.
			Assert::IsNotNull(early.Acquire(*b), wide().c_str());
			early.Used(*b);
			readerTex = early.AcquireReader(*b);
			Assert::IsNotNull(readerTex, wide().c_str());
			Assert::IsTrue(ReadBytes(reader, readerContext, readerTex) == frames[1], L"the reused slot holds the new bytes");
			b.reset();
			pl_gpu_finish(fixture.Gpu());
			Assert::IsTrue(StaysHeld(early, frames[2], rowBytes, height), L"dead reader: the slot stays held");
			Assert::IsTrue(early.GetStats().noFreeReader >= 1, L"counted as held by the reader");

			// 3. Stop: DetachReader releases what the dead reader held.
			early.DetachReader();
			Assert::IsFalse(early.HasReader());
			EarlyUpload::SlotHandle c = UploadWhenFree(early, frames[2], rowBytes, height);
			Assert::IsTrue(c != nullptr, L"after DetachReader the slot comes back");
			Assert::IsNull(early.AcquireReader(*c), L"no reader: nothing to read");
			Assert::IsNotNull(early.Acquire(*c), wide().c_str());
			early.Used(*c);
			c.reset();

			// 4. A reader attaches again and the law holds as before.
			Assert::IsTrue(early.AttachReader(reader), wide().c_str());
			EarlyUpload::SlotHandle d = UploadWhenFree(early, frames[3], rowBytes, height);
			Assert::IsTrue(d != nullptr, L"re-attached: a slot");
			readerTex = early.AcquireReader(*d);
			Assert::IsNotNull(readerTex, wide().c_str());
			Assert::IsTrue(ReadBytes(reader, readerContext, readerTex) == frames[3], L"re-attached reader reads the bytes");
			EarlyUpload::SlotHandle dead = d;
			d.reset();
			Assert::IsTrue(StaysHeld(early, frames[0], rowBytes, height), L"re-attached: held while the reader holds it");
			early.UsedReader(*dead);
			dead.reset();
			EarlyUpload::SlotHandle e = UploadWhenFree(early, frames[0], rowBytes, height);
			Assert::IsTrue(e != nullptr, L"re-attached: released slot comes back");

			const EarlyUpload::Stats stats = early.GetStats();
			Assert::IsTrue(stats.failures == 0 && stats.readerDetachTimeouts == 0, L"no failures, detach completed in time");

			// 5. Close with a reader attached: nothing is left to read; a handle released after Close is harmless.
			early.Close();
			Assert::IsFalse(early.HasReader());
			Assert::IsNull(early.AcquireReader(*e), L"closed: nothing to read");
			e.reset();
			Logger::WriteMessage(("EarlyUpload second reader: " + std::to_string(stats.uploads) + " uploads, " +
				std::to_string(stats.noFreeSlot) + " with no free slot (" + std::to_string(stats.noFreeReader) + " held by the reader)\n").c_str());
		}
	};
}
