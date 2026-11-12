// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// LedDrive::NativeTexReadback on the libplacebo WARP device (VP_TEST_GPU=hardware for a real GPU):
// Create, Queue, poll (the Map is DO_NOT_WAIT; this test asserts no timing and does not show a
// Pending result) until the staging copy is Ready, Read matches the source exactly; a queued slot
// is reusable; the Map result classification (only DXGI_ERROR_WAS_STILL_DRAWING is pending, every
// other error is Failed); and a REAL Failed path: the test maps the staging texture itself, so
// the readback's own Map fails with an error HRESULT (see FailedMapIsReportedLatchedAndClearedByQueue).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/drive/led_measure/NativeReadback.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		std::vector<float> Known(int w, int h, int salt)
		{
			std::vector<float> v(static_cast<size_t>(w) * static_cast<size_t>(h));
			for (size_t i = 0; i < v.size(); ++i)
				v[i] = static_cast<float>(static_cast<int>((i * 53 + static_cast<size_t>(salt) * 17) % 997) - 400) * 0.125f;
			return v;
		}

		// Polls (DO_NOT_WAIT maps; sleeps between polls) until the slot is Ready; false on Failed or after about 5 s.
		bool WaitReady(LedDrive::NativeTexReadback& rb, int slot)
		{
			for (int i = 0; i < 5000; ++i)
			{
				const LedDrive::ReadState state = rb.Poll(slot);
				if (state == LedDrive::ReadState::Ready)
					return true;
				if (state == LedDrive::ReadState::Failed)
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return false;
		}

		std::string HexOf(HRESULT hr)
		{
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hr));
			return buf;
		}
	}

	TEST_CLASS(NativeReadbackWarpTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(QueuedCopyBecomesReadyAndReadsBackTheSourceExactly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(QueuedCopyBecomesReadyAndReadsBackTheSourceExactly)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int w = 13, h = 5;
			LedDrive::NativeTexReadback rb;
			Assert::IsTrue(rb.Create(gpu, 2, w, h, DXGI_FORMAT_R32_FLOAT), L"Create");
			Assert::IsTrue(rb.Active() && rb.Slots() == 2);
			Assert::IsNotNull(rb.Target(0));
			ID3D11DeviceContext* ctx = nullptr;
			pl_d3d11_get(gpu)->device->GetImmediateContext(&ctx);
			Assert::IsNotNull(ctx);

			// Slot 0: Queue, Poll until Ready, Read. The first Poll may be Pending; none may be Failed.
			const std::vector<float> a = Known(w, h, 1);
			ctx->UpdateSubresource(rb.Texture(0), 0, nullptr, a.data(), static_cast<UINT>(w * sizeof(float)), 0);
			rb.Queue(0);
			Assert::IsTrue(WaitReady(rb, 0), L"slot 0 never became Ready");
			Assert::IsTrue(rb.Ready(0), L"a Ready slot stays Ready until it is read");
			std::vector<float> got(a.size(), -1.0f);
			Assert::IsTrue(rb.Read(0, got.data(), static_cast<size_t>(w) * sizeof(float), h), L"Read after Ready");
			Assert::IsTrue(std::memcmp(a.data(), got.data(), a.size() * sizeof(float)) == 0, L"slot 0 differs from the source");
			Assert::IsFalse(rb.Failed(), L"no error expected");

			// Slot 1: Read without a prior Poll (the Read maps for itself, false while pending).
			const std::vector<float> b = Known(w, h, 2);
			ctx->UpdateSubresource(rb.Texture(1), 0, nullptr, b.data(), static_cast<UINT>(w * sizeof(float)), 0);
			rb.Queue(1);
			std::vector<float> gotB(b.size(), -1.0f);
			bool read = false;
			for (int i = 0; i < 5000 && !read; ++i)
			{
				read = rb.Read(1, gotB.data(), static_cast<size_t>(w) * sizeof(float), h);
				if (!read) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Assert::IsTrue(read, L"slot 1 never read");
			Assert::IsTrue(std::memcmp(b.data(), gotB.data(), b.size() * sizeof(float)) == 0, L"slot 1 differs from the source");

			// Reuse: slot 0 again with other data; a Ready slot queued again (mapping dropped) shows the new copy.
			const std::vector<float> c = Known(w, h, 3);
			ctx->UpdateSubresource(rb.Texture(0), 0, nullptr, c.data(), static_cast<UINT>(w * sizeof(float)), 0);
			rb.Queue(0);
			Assert::IsTrue(WaitReady(rb, 0), L"slot 0 (second round)");
			rb.Queue(0);   // queued again while Ready and unread: the held mapping is dropped first
			Assert::IsTrue(WaitReady(rb, 0), L"slot 0 (queued over a held mapping)");
			Assert::IsTrue(rb.Read(0, got.data(), static_cast<size_t>(w) * sizeof(float), h));
			Assert::IsTrue(std::memcmp(c.data(), got.data(), c.size() * sizeof(float)) == 0, L"slot 0 second round differs");

			// Out-of-range slots are never ready and never crash.
			Assert::IsTrue(rb.Poll(-1) == LedDrive::ReadState::Pending && rb.Poll(2) == LedDrive::ReadState::Pending);
			Assert::IsFalse(rb.Read(7, got.data(), 4, 1));

			ctx->Release();
			rb.Release(gpu);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(FailedMapIsReportedLatchedAndClearedByQueue)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FailedMapIsReportedLatchedAndClearedByQueue)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int w = 7, h = 3;
			LedDrive::NativeTexReadback rb;
			Assert::IsTrue(rb.Create(gpu, 2, w, h, DXGI_FORMAT_R32_FLOAT), L"Create");
			ID3D11DeviceContext* ctx = nullptr;
			pl_d3d11_get(gpu)->device->GetImmediateContext(&ctx);
			Assert::IsNotNull(ctx);
			const std::vector<float> a = Known(w, h, 5);
			ctx->UpdateSubresource(rb.Texture(0), 0, nullptr, a.data(), static_cast<UINT>(w * sizeof(float)), 0);

			// A real failure: slot 0's staging texture is swapped for one created without CPU read access, so the
			// readback's own Map(READ) is refused by the runtime with an error HRESULT that is not 'still drawing'.
			ID3D11Device* device = pl_d3d11_get(gpu)->device;
			D3D11_TEXTURE2D_DESC stagingDesc{};
			rb.Staging(0)->GetDesc(&stagingDesc);
			D3D11_TEXTURE2D_DESC noRead = stagingDesc;
			noRead.Usage = D3D11_USAGE_DEFAULT;
			noRead.CPUAccessFlags = 0;
			ID3D11Texture2D* unreadable = nullptr;
			Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&noRead, nullptr, &unreadable)), L"unreadable staging");
			rb.ReplaceStagingForTest(0, unreadable);
			rb.Queue(0);
			ctx->Flush();
			Assert::IsFalse(rb.Failed(), L"no error before the refused Map");
			// The Map is attempted once the fence reports the copy complete (fx/dv-frame-quiet): Pending until then, never
			// a still-pending answer for the refused Map itself.
			LedDrive::ReadState refused = rb.Poll(0);
			for (int i = 0; i < 5000 && refused == LedDrive::ReadState::Pending; ++i)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				refused = rb.Poll(0);
			}
			Assert::IsTrue(refused == LedDrive::ReadState::Failed, L"a refused Map must be Failed, not Pending");
			Assert::IsTrue(rb.Failed());
			const HRESULT err = rb.LastError();
			Assert::IsTrue(FAILED(err) && err != DXGI_ERROR_WAS_STILL_DRAWING, L"LastError holds the error HRESULT");
			Logger::WriteMessage((std::string("refused Map HRESULT 0x") + HexOf(err) + "\n").c_str());

			// Latched: the same answer on every Poll, Ready() is false, Read returns false.
			Assert::IsTrue(rb.Poll(0) == LedDrive::ReadState::Failed, L"Failed is latched until Queue");
			Assert::IsFalse(rb.Ready(0));
			std::vector<float> got(a.size(), -1.0f);
			Assert::IsFalse(rb.Read(0, got.data(), static_cast<size_t>(w) * sizeof(float), h), L"Read on a Failed slot");
			Assert::IsTrue(got[0] == -1.0f, L"Read wrote nothing");
			Assert::IsTrue(rb.LastError() == err, L"LastError is the ever-failed record: not cleared by Polls");

			// Queue starts a new round: the latch is cleared and the copy reads back exactly.
			D3D11_TEXTURE2D_DESC again = stagingDesc;
			ID3D11Texture2D* readable = nullptr;
			Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&again, nullptr, &readable)), L"readable staging");
			rb.ReplaceStagingForTest(0, readable);   // the slot is healthy again; only Queue may clear the latch
			Assert::IsTrue(rb.Poll(0) == LedDrive::ReadState::Failed, L"swapping the texture back does not clear the latch; only Queue does");
			rb.Queue(0);
			Assert::IsTrue(WaitReady(rb, 0), L"slot 0 after Queue: the Failed latch must be cleared");
			Assert::IsTrue(rb.Read(0, got.data(), static_cast<size_t>(w) * sizeof(float), h), L"Read after the failed round");
			Assert::IsTrue(std::memcmp(a.data(), got.data(), a.size() * sizeof(float)) == 0, L"the new round differs from the source");

			ctx->Release();
			rb.Release(gpu);
		}

		TEST_METHOD(OnlyStillDrawingIsPendingEveryOtherErrorIsFailed)
		{
			using LedDrive::NativeTexReadback;
			using LedDrive::ReadState;
			Assert::IsTrue(NativeTexReadback::ClassifyMapResult(S_OK) == ReadState::Ready);
			Assert::IsTrue(NativeTexReadback::ClassifyMapResult(S_FALSE) == ReadState::Ready, L"a success code");
			Assert::IsTrue(NativeTexReadback::ClassifyMapResult(DXGI_ERROR_WAS_STILL_DRAWING) == ReadState::Pending);
			const HRESULT errors[] = { DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_INVALID_CALL,
				E_FAIL, E_OUTOFMEMORY, E_INVALIDARG, DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_DRIVER_INTERNAL_ERROR };
			for (const HRESULT hr : errors)
				Assert::IsTrue(NativeTexReadback::ClassifyMapResult(hr) == ReadState::Failed, L"an error HRESULT must be Failed, never Pending");
		}
	};
}
