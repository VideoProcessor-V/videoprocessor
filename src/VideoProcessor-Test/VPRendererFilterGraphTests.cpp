#include "pch.h"
#include "CppUnitTest.h"

// VP Renderer DirectShow filter, end to end on real hardware (owner-run, gated):
//   File Source (Async) -> splitter / LAV Splitter (Intelligent Connect)
//     -> LAV Video Decoder when installed (else whatever DirectShow picks)
//     -> VP Renderer (VPRendererFilter.ax next to this test DLL, created from
//        the module directly: nothing is registered)
// The renderer loads x64\<Configuration>\vprenderer\VideoProcessorVPRenderer.dll,
// opens the GPU and presents into a small window on the desktop.
//
// Gate (as RendererCaptureHarnessTests):
//   VP_TEST_GPU=hardware          run (otherwise "SKIP-NO-GPU <name>" and pass)
//   VP_TEST_REQUIRE_GPU=1         a skip fails instead
//   VP_TEST_FILTER_CLIP=<path>    the clip to play (required when running;
//                                 "SKIP-NO-CLIP <name>" without it)
//   VP_TEST_FILTER_SECONDS=<n>    play time per phase (default 5)
// The renderer's log goes to %LOCALAPPDATA%\VideoProcessor\logs\vprfilter.log
// (or VP_RENDERER_FILTER_LOG).

#include <dshow.h>
#include <atlbase.h>

#include "../VideoProcessor-VPRendererFilter/VPRendererFilterRegistration.h"

#include <algorithm>
#include <cstdlib>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;


namespace
{
	// LAV Video Decoder's CLSID (LAV Filters, public).
	const CLSID kClsidLavVideo = { 0xEE30215D, 0x164F, 0x4A92,
		{ 0xA4, 0xEB, 0x9D, 0x4C, 0x13, 0x39, 0x0F, 0x9F } };

	std::string Env(const char* name)
	{
		char* v = nullptr;
		size_t n = 0;
		std::string value;
		if (_dupenv_s(&v, &n, name) == 0 && v)
			value = v;
		std::free(v);
		return value;
	}

	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// The gate; false = the caller returns (the test passes with the marker).
	bool GateOrSkip(const char* name, std::wstring& clip)
	{
		const bool require = Env("VP_TEST_REQUIRE_GPU") == "1";
		std::string marker;
		if (Env("VP_TEST_GPU") != "hardware")
			marker = std::string("SKIP-NO-GPU ") + name;
		else
		{
			const std::string path = Env("VP_TEST_FILTER_CLIP");
			if (path.empty() || GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
				marker = std::string("SKIP-NO-CLIP ") + name + " (set VP_TEST_FILTER_CLIP to a video file)";
			else
				clip = Wide(path);
		}
		if (marker.empty())
			return true;
		if (require)
			Assert::Fail(Wide(marker + ": VP_TEST_REQUIRE_GPU=1").c_str());
		Logger::WriteMessage((marker + "\n").c_str());
		return false;
	}

	std::wstring TestDirectory()
	{
		wchar_t path[MAX_PATH * 2] = {};
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&TestDirectory), &self);
		GetModuleFileNameW(self, path, static_cast<DWORD>(_countof(path)));
		std::wstring dir(path);
		return dir.substr(0, dir.find_last_of(L"\\/"));
	}

	// The filter straight from its module (DllGetClassObject), so no
	// registration is needed. The module stays loaded for the process.
	HRESULT CreateVPRenderer(IBaseFilter** filter, std::wstring& detail)
	{
		const std::wstring path = TestDirectory() + L"\\VPRendererFilter.ax";
		HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!module)
		{
			detail = L"cannot load " + path;
			return HRESULT_FROM_WIN32(GetLastError());
		}
		typedef HRESULT(STDAPICALLTYPE* GetClassObjectFn)(REFCLSID, REFIID, void**);
		const GetClassObjectFn getClassObject = reinterpret_cast<GetClassObjectFn>(
			GetProcAddress(module, "DllGetClassObject"));
		if (!getClassObject)
		{
			detail = L"DllGetClassObject missing";
			return E_NOINTERFACE;
		}
		CComPtr<IClassFactory> factory;
		const HRESULT hr = getClassObject(VPRendererFilter::kClsidVPRendererFilter, IID_IClassFactory, reinterpret_cast<void**>(&factory));
		if (FAILED(hr))
		{
			detail = L"DllGetClassObject failed";
			return hr;
		}
		return factory->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void**>(filter));
	}

	HWND CreateOwnerWindow()
	{
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"VPRendererFilterGraphTestOwner";
		RegisterClassW(&wc);
		HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VP Renderer graph test",
			WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 960, 540, nullptr, nullptr, wc.hInstance, nullptr);
		return hwnd;
	}

	void PumpFor(DWORD ms, IMediaEventEx* events, long* abortCode)
	{
		const DWORD end = GetTickCount() + ms;
		while (static_cast<long>(end - GetTickCount()) > 0)
		{
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			long code = 0;
			LONG_PTR p1 = 0, p2 = 0;
			while (events && SUCCEEDED(events->GetEvent(&code, &p1, &p2, 0)))
			{
				if (code == EC_ERRORABORT || code == EC_USERABORT)
					*abortCode = code;
				events->FreeEventParams(code, p1, p2);
			}
			Sleep(10);
		}
	}

	bool WaitForEvent(IMediaEventEx* events, long wanted, DWORD ms)
	{
		const DWORD end = GetTickCount() + ms;
		while (static_cast<long>(end - GetTickCount()) > 0)
		{
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
				DispatchMessageW(&msg);
			long code = 0;
			LONG_PTR p1 = 0, p2 = 0;
			if (SUCCEEDED(events->GetEvent(&code, &p1, &p2, 10)))
			{
				events->FreeEventParams(code, p1, p2);
				if (code == wanted)
					return true;
			}
		}
		return false;
	}
}


namespace VideoProcessorTest
{
	TEST_CLASS(VPRendererFilterGraphTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(FileSourceLavVpRendererPlaysSeeksStepsAndStops)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FileSourceLavVpRendererPlaysSeeksStepsAndStops)
		{
			std::wstring clip;
			if (!GateOrSkip("FileSourceLavVpRendererPlaysSeeksStepsAndStops", clip))
				return;
			const std::string secondsText = Env("VP_TEST_FILTER_SECONDS");
			const DWORD phaseMs = static_cast<DWORD>(
				(secondsText.empty() ? 5 : (std::max)(1, std::atoi(secondsText.c_str()))) * 1000);

			// S_FALSE / RPC_E_CHANGED_MODE: COM is already up on this thread.
			const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			{
				CComPtr<IGraphBuilder> graph;
				Assert::IsTrue(SUCCEEDED(graph.CoCreateInstance(CLSID_FilterGraph)), L"FilterGraph");

				CComPtr<IBaseFilter> renderer;
				std::wstring detail;
				const HRESULT created = CreateVPRenderer(&renderer, detail);
				Assert::IsTrue(SUCCEEDED(created), (L"VP Renderer: " + detail).c_str());
				Assert::IsTrue(SUCCEEDED(graph->AddFilter(renderer, L"VP Renderer")));

				CComPtr<IBaseFilter> source;
				Assert::IsTrue(SUCCEEDED(graph->AddSourceFilter(clip.c_str(), L"Source", &source)),
					(L"AddSourceFilter " + clip).c_str());

				CComPtr<IBaseFilter> lav;
				const bool haveLav = SUCCEEDED(lav.CoCreateInstance(kClsidLavVideo)) &&
					SUCCEEDED(graph->AddFilter(lav, L"LAV Video Decoder"));
				Logger::WriteMessage(haveLav ? "LAV Video Decoder: installed, used\n" :
					"LAV Video Decoder: not installed; Intelligent Connect picks the decoder\n");

				CComPtr<ICaptureGraphBuilder2> builder;
				Assert::IsTrue(SUCCEEDED(builder.CoCreateInstance(CLSID_CaptureGraphBuilder2)));
				Assert::IsTrue(SUCCEEDED(builder->SetFiltergraph(graph)));
				const HRESULT connected = builder->RenderStream(nullptr, &MEDIATYPE_Video, source,
					haveLav ? lav.p : nullptr, renderer);
				Assert::IsTrue(SUCCEEDED(connected),
					L"source -> decoder -> VP Renderer did not connect (see vprfilter.log: media type / plugin refusal)");

				// The player's role: own the video window.
				HWND owner = CreateOwnerWindow();
				CComQIPtr<IVideoWindow> window(renderer);
				Assert::IsTrue(window != nullptr, L"IVideoWindow");
				window->put_Owner(reinterpret_cast<OAHWND>(owner));
				window->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
				RECT rc = {};
				GetClientRect(owner, &rc);
				window->SetWindowPosition(0, 0, rc.right, rc.bottom);
				window->put_Visible(OATRUE);

				CComQIPtr<IBasicVideo2> basic(renderer);
				long width = 0, height = 0;
				Assert::IsTrue(basic && SUCCEEDED(basic->GetVideoSize(&width, &height)));
				Assert::IsTrue(width > 0 && height > 0, L"zero video size (MPC would treat it as audio only)");

				CComQIPtr<IMediaControl> control(graph);
				CComQIPtr<IMediaEventEx> events(graph);
				CComQIPtr<IMediaSeeking> seeking(graph);
				CComQIPtr<IQualProp> quality(renderer);
				Assert::IsTrue(control && events && quality);
				long abortCode = 0;

				// Play.
				Assert::IsTrue(SUCCEEDED(control->Run()));
				PumpFor(phaseMs, events, &abortCode);
				int drawn = 0, dropped = 0;
				quality->get_FramesDrawn(&drawn);
				quality->get_FramesDroppedInRenderer(&dropped);
				Logger::WriteMessage(("play: drawn " + std::to_string(drawn) + " dropped " +
					std::to_string(dropped) + "\n").c_str());
				Assert::AreEqual(0L, abortCode, L"the graph aborted while playing");
				Assert::IsTrue(drawn > 0, L"no frame reached VP");
				Assert::AreEqual(0, dropped, L"the VP renderer dropped file frames");

				// Seek to the start (flush + new segment: a discontinuity for VP).
				if (seeking)
				{
					LONGLONG zero = 0;
					Assert::IsTrue(SUCCEEDED(seeking->SetPositions(&zero, AM_SEEKING_AbsolutePositioning,
						nullptr, AM_SEEKING_NoPositioning)));
					PumpFor(phaseMs / 2, events, &abortCode);
					int afterSeek = 0;
					quality->get_FramesDrawn(&afterSeek);
					Assert::IsTrue(afterSeek > drawn, L"no frames after the seek");
					drawn = afterSeek;
				}

				// Pause and step one frame.
				Assert::IsTrue(SUCCEEDED(control->Pause()));
				PumpFor(500, events, &abortCode);
				CComQIPtr<IVideoFrameStep> step(graph);
				if (step && step->CanStep(0, nullptr) == S_OK)
				{
					Assert::IsTrue(SUCCEEDED(step->Step(1, nullptr)));
					Assert::IsTrue(WaitForEvent(events, EC_STEP_COMPLETE, 5000), L"no EC_STEP_COMPLETE");
				}
				else
					Logger::WriteMessage("frame step: the graph does not offer IVideoFrameStep stepping\n");

				// Run again at a different rate when the source allows it (motion off in VP).
				if (seeking && SUCCEEDED(seeking->SetRate(2.0)))
				{
					Assert::IsTrue(SUCCEEDED(control->Run()));
					PumpFor(phaseMs / 2, events, &abortCode);
					seeking->SetRate(1.0);
				}

				Assert::IsTrue(SUCCEEDED(control->Stop()));
				Assert::AreEqual(0L, abortCode, L"the graph aborted");
				quality->get_FramesDroppedInRenderer(&dropped);
				Assert::AreEqual(0, dropped, L"the VP renderer dropped file frames");

				window->put_Visible(OAFALSE);
				window->put_Owner(0);
				DestroyWindow(owner);
			}
			if (SUCCEEDED(com))
				CoUninitialize();
		}
	};
}
