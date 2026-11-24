// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Red-First adversarial tests for EarlyUpload lifecycle & concurrency defects:
//   - S1-B1: EarlyUpload::Open() holds non-recursive std::mutex lifecycle and previously called Close()
//            (which re-locks lifecycle) on init failure -> guaranteed render-thread self-deadlock.
//   - S1-H3: EarlyUpload::LastError() must return std::string by value under lock (not const std::string&),
//            and poolId / error must be race-safe across concurrent Open/Close/AttachReader/AcquireReader/UsedReader.
// Pure CPU (no GPU fixture / no D3D11 runtime device created).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/input/capture/EarlyUpload.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// Minimal COM stub for ID3D11Device (pure CPU, no GPU created).
		// Owned by std::shared_ptr so AddRef/Release track COM refcount without self-deletion,
		// keeping detached pre-fix deadlock probes free of UAF on watchdog timeout.
		class StubNoContextD3D11Device final : public ID3D11Device
		{
		public:
			std::atomic<ULONG> comRefs{ 1 };
			std::atomic<int> getImmediateContextCalls{ 0 };

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
			{
				if (!ppvObject)
					return E_POINTER;
				if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11Device))
				{
					*ppvObject = static_cast<ID3D11Device*>(this);
					AddRef();
					return S_OK;
				}
				*ppvObject = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return comRefs.fetch_add(1, std::memory_order_acq_rel) + 1; }
			ULONG STDMETHODCALLTYPE Release() override { return comRefs.fetch_sub(1, std::memory_order_acq_rel) - 1; }

			HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateVertexShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void*, SIZE_T, const D3D11_SO_DECLARATION_ENTRY*, UINT, const UINT*, UINT, UINT, ID3D11ClassLinkage*, ID3D11GeometryShader**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreatePixelShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateHullShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11HullShader**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateDomainShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateComputeShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC*, ID3D11BlendState**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC*, ID3D11DepthStencilState**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC*, ID3D11RasterizerState**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC*, ID3D11SamplerState**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC*, ID3D11Query**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC*, ID3D11Predicate**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC*, ID3D11Counter**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT, ID3D11DeviceContext**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE, REFIID, void**) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT, UINT*) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT, UINT, UINT*) override { return E_NOTIMPL; }
			void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO*) override {}
			HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC*, D3D11_COUNTER_TYPE*, UINT*, LPSTR, UINT*, LPSTR, UINT*, LPSTR, UINT*) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE, void*, UINT) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
			HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
			D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override { return D3D_FEATURE_LEVEL_11_0; }
			UINT STDMETHODCALLTYPE GetCreationFlags() override { return 0; }
			HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return S_OK; }
			void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) override
			{
				getImmediateContextCalls.fetch_add(1, std::memory_order_relaxed);
				if (ppImmediateContext)
					*ppImmediateContext = nullptr;
			}
			HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT) override { return E_NOTIMPL; }
			UINT STDMETHODCALLTYPE GetExceptionMode() override { return 0; }
		};

		struct WatchdogState
		{
			std::mutex m;
			std::condition_variable cv;
			bool finished = false;
			bool openResult = true;
			bool threwSystemError = false;
			std::string exceptionWhat;
		};
	}

	TEST_CLASS(EarlyUploadLifecycleTests)
	{
	public:
		// S1-B1: EarlyUpload::Open() failure after acquiring lifecycle mutex must clean up via CloseLocked()
		// without self-deadlocking, release its renderDevice COM reference, preserve the descriptive LastError(),
		// and leave lifecycle unlocked for subsequent Close()/Open() calls.
		TEST_METHOD(OpenInitFailureCleansUpWithoutSelfDeadlock)
		{
			auto uploader = std::make_shared<EarlyUpload>();
			auto stubDevice = std::make_shared<StubNoContextD3D11Device>();
			// Non-null sentinel pl_gpu (never dereferenced when entries is empty and Open fails before MakeTexture).
			const pl_gpu sentinelGpu = reinterpret_cast<pl_gpu>(uintptr_t(0x1));

			// 1. Pre-lock argument-validation control (does not call CloseLocked): returns false immediately.
			Assert::IsFalse(uploader->Open(nullptr, nullptr, 0));
			Assert::IsFalse(uploader->IsOpen());
			Assert::AreEqual(std::string("no render device"), std::string(uploader->LastError()));
			Assert::IsFalse(uploader->Open(stubDevice.get(), sentinelGpu, 0));
			Assert::AreEqual(0, stubDevice->getImmediateContextCalls.load());
			Assert::AreEqual(ULONG(1), stubDevice->comRefs.load());

			// 2. S1-B1 failure path: valid args -> acquires lifecycle mutex -> renderDevice->GetImmediateContext returns null
			// -> hits the error cleanup branch (pre-fix calls Close() which re-locks lifecycle -> self-deadlock!).
			auto state = std::make_shared<WatchdogState>();
			std::thread worker([uploader, stubDevice, sentinelGpu, state]()
			{
				bool res = true;
				bool threw = false;
				std::string what;
				try
				{
					res = uploader->Open(stubDevice.get(), sentinelGpu, 4);
				}
				catch (const std::system_error& e)
				{
					threw = true;
					what = e.what();
				}
				{
					std::lock_guard<std::mutex> g(state->m);
					state->openResult = res;
					state->threwSystemError = threw;
					state->exceptionWhat = what;
					state->finished = true;
				}
				state->cv.notify_one();
			});

			bool completed = false;
			{
				std::unique_lock<std::mutex> lk(state->m);
				completed = state->cv.wait_for(lk, std::chrono::seconds(2), [&]() { return state->finished; });
			}
			if (!completed)
			{
				// Detach with shared_ptr owners captured inside worker so the pre-fix deadlocked thread
				// cannot cause stack-UAF before vstest records this failure cleanly.
				worker.detach();
				Assert::Fail(L"S1-B1: EarlyUpload::Open() deadlocked on init failure (re-locked non-recursive lifecycle mutex via Close())");
			}
			worker.join();

			Assert::IsFalse(state->threwSystemError,
				std::wstring(state->exceptionWhat.begin(), state->exceptionWhat.end()).c_str());
			Assert::IsFalse(state->openResult, L"Open must return false when render context has no D3D11.4 fences");
			Assert::IsFalse(uploader->IsOpen(), L"failed Open must leave pool closed");
			Assert::AreEqual(1, stubDevice->getImmediateContextCalls.load(),
				L"Open must have reached GetImmediateContext before failing");

			// Verify CloseLocked() actually released renderDevice (catches a broken fix that merely deletes Close()).
			Assert::AreEqual(ULONG(1), stubDevice->comRefs.load(),
				L"CloseLocked() on Open failure must release the renderDevice COM reference (zero leak)");

			// Verify descriptive LastError() is preserved across CloseLocked().
			const std::string err = uploader->LastError();
			Assert::IsFalse(err.empty(), L"failed Open must preserve a non-empty LastError()");
			Assert::IsTrue(err.find("fences") != std::string::npos,
				std::wstring(err.begin(), err.end()).c_str());

			// Verify lifecycle mutex is unlocked and uploader remains reusable.
			uploader->Close();
			Assert::IsFalse(uploader->Open(nullptr, nullptr, 0));
			Assert::AreEqual(std::string("no render device"), std::string(uploader->LastError()));
		}

		// S1-H3: EarlyUpload::LastError() must return std::string by value under lock (never a racy reference to
		// member `error`), and concurrent Open/AttachReader/Close vs AcquireReader/UsedReader/LastError must stay race-free.
		TEST_METHOD(LastErrorReturnsByValueAndConcurrentLifecycleReadsAreSafe)
		{
			// Runtime contract check (compiles both pre-fix and post-fix; fails pre-fix when LastError returns const std::string&).
			constexpr bool lastErrorReturnsByValue =
				!std::is_reference<decltype(std::declval<const EarlyUpload&>().LastError())>::value;
			Assert::IsTrue(lastErrorReturnsByValue,
				L"S1-H3: EarlyUpload::LastError() must return std::string by value under lock, not a reference to member error");

			EarlyUpload early;
			EarlyUpload::Slot dummySlot{ &early, 1, 0, 1 };
			std::atomic<bool> readerReady{ false };
			std::atomic<bool> stop{ false };
			std::atomic<uint64_t> readerIters{ 0 };
			std::atomic<int> badErrorObserved{ 0 };
			std::atomic<int> unexpectedNonNull{ 0 };

			std::thread reader([&]()
			{
				readerReady.store(true, std::memory_order_release);
				while (!stop.load(std::memory_order_acquire))
				{
					if (early.AcquireReader(dummySlot) != nullptr)
						unexpectedNonNull.fetch_add(1, std::memory_order_relaxed);
					early.UsedReader(dummySlot);
					const std::string err = early.LastError();
					if (!err.empty()
						&& err != "no render device"
						&& err != "no pool or no reader device")
					{
						badErrorObserved.fetch_add(1, std::memory_order_relaxed);
					}
					readerIters.fetch_add(1, std::memory_order_relaxed);
				}
			});
			while (!readerReady.load(std::memory_order_acquire))
				std::this_thread::yield();

			for (int i = 0; i < 2000; ++i)
			{
				Assert::IsFalse(early.Open(nullptr, nullptr, 0));
				Assert::IsFalse(early.AttachReader(nullptr));
				early.Close();
			}

			stop.store(true, std::memory_order_release);
			reader.join();

			Assert::IsTrue(readerIters.load() > 0u,
				L"reader thread must execute concurrently with Open/AttachReader/Close stress loop");
			Assert::AreEqual(0, badErrorObserved.load(), L"every concurrent LastError() snapshot must be a valid complete message");
			Assert::AreEqual(0, unexpectedNonNull.load(), L"closed pool must never return a texture from AcquireReader");

			Assert::IsFalse(early.Open(nullptr, nullptr, 0));
			Assert::AreEqual(std::string("no render device"), std::string(early.LastError()));
			Assert::IsFalse(early.AttachReader(nullptr));
			Assert::AreEqual(std::string("no pool or no reader device"), std::string(early.LastError()));
		}
	};
}
