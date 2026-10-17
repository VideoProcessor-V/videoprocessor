#include "pch.h"
#include <wincodec.h>
#include <atlbase.h>
#pragma comment(lib, "windowscodecs.lib")
#include "CppUnitTest.h"

#include <DisplayMode.h>
#include <IRenderer.h>
#include <ITimingClock.h>
#include <VideoFrame.h>
#include <VideoState.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>
#include <ConfigFile.h>
#include <UnifiedProfileRuntime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <cstring>
#include <sstream>
#include <map>
#include <random>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "DolbyVisionTunnelSender.h"
using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// capture_path (owner 2026-10-13): the shipped VP renderer DLL driven like the
// player drives it, with fake DeckLink frames (moving synthetic V210 / UYVY at
// 59.94 Hz through OnVideoFrame), a rule selecting the low-latency input
// profile, and its log read back to see which capture path ran. Hardware
// only (VP_TEST_GPU=hardware): the renderer opens the real GPU, a swapchain on
// a small off-screen window.
namespace
{
	std::mutex g_logMutex;
	std::vector<std::string> g_log;

	void __cdecl CaptureLog(const char* message)
	{
		std::lock_guard<std::mutex> guard(g_logMutex);
		g_log.emplace_back(message ? message : "");
		// Diagnostics: VP_TEST_LOG_FILE also appends every line to a file, flushed
		// per line, so the last lines survive a crash of the test host.
		static FILE* file = [] {
			char* v = nullptr;
			size_t n = 0;
			FILE* f = nullptr;
			if (_dupenv_s(&v, &n, "VP_TEST_LOG_FILE") == 0 && v && v[0]) fopen_s(&f, v, "a");
			std::free(v);
			return f;
		}();
		if (file)
		{
			fprintf(file, "%s\n", message ? message : "");
			fflush(file);
		}
	}

	struct Callback : IRendererCallback
	{
		std::atomic<int> state{ -1 };
		void OnRendererState(RendererState s, uint32_t) override { state = static_cast<int>(s); }
		void OnRendererDetailString(const CString&, uint32_t) override {}
	};

	// The frames' source buffer (DeckLink's frame object stands here): VP
	// takes references while it holds a frame; the ring keeps the bytes.
	struct SourceBuffer : IUnknown
	{
		std::atomic<long> references{ 1 };
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
		{
			if (riid == __uuidof(IUnknown)) { *out = this; AddRef(); return S_OK; }
			*out = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++references); }
		ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(--references); }
	};

	struct Clock : ITimingClock
	{
		timingclocktime_t TimingClockNow() override
		{
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}
		timingclocktime_t TimingClockTicksPerSecond() const override
		{
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			return frequency.QuadPart;
		}
		const TCHAR* TimingClockDescription() override { return TEXT("test QPC"); }
	};

	bool OnHardware()
	{
		char* v = nullptr;
		size_t n = 0;
		const bool on = _dupenv_s(&v, &n, "VP_TEST_GPU") == 0 && v && std::string(v) == "hardware";
		std::free(v);
		return on;
	}

	std::string Here()
	{
		char here[MAX_PATH] = {};
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&CaptureLog), &self);
		GetModuleFileNameA(self, here, MAX_PATH);
		const std::string binary(here);
		return binary.substr(0, binary.find_last_of("\\/") + 1);
	}

	// Letterbox bars (black rows at the top and bottom) in the V210 frames; 0 = none.
	int g_harnessBarRows = 0;
	// A picture for the V210 frames (10-bit luma codes, width x height; chroma neutral), the same
	// every frame; empty = the moving test pattern (2026-10-16: the chain test).
	std::vector<uint16_t> g_harnessLumaPicture;

	// One moving frame: vertical bars in luma, a chroma ramp, shifted by n.
	void FillV210(std::vector<BYTE>& frame, int width, int height, size_t rowBytes, int n)
	{
		for (int y = 0; y < height; ++y)
		{
			uint32_t* row = reinterpret_cast<uint32_t*>(frame.data() + rowBytes * y);
			const bool bar = y < g_harnessBarRows || y >= height - g_harnessBarRows;
			for (int pack = 0; pack < (width + 5) / 6; ++pack)
			{
				const bool picture = g_harnessLumaPicture.size() == static_cast<size_t>(width) * height;
				auto luma = [&](int x) {
					if (picture) return static_cast<uint32_t>(g_harnessLumaPicture[static_cast<size_t>(y) * width + (std::min)(x, width - 1)]);
					return bar ? 64u : static_cast<uint32_t>(64 + ((x + 4 * n) / 32 % 2) * 700 + (y % 64)); };
				auto chroma = [&](int x) { return bar || picture ? 512u : static_cast<uint32_t>(512 + ((x + y) % 128) - 64); };
				const int x = pack * 6;
				row[pack * 4 + 0] = chroma(x) | (luma(x) << 10) | (chroma(x + 1) << 20);
				row[pack * 4 + 1] = luma(x + 1) | (chroma(x + 2) << 10) | (luma(x + 2) << 20);
				row[pack * 4 + 2] = chroma(x + 3) | (luma(x + 3) << 10) | (chroma(x + 4) << 20);
				row[pack * 4 + 3] = luma(x + 4) | (chroma(x + 5) << 10) | (luma(x + 5) << 20);
			}
		}
	}

	// Packed RGB (R12B / R12L / r210): a moving byte pattern (any bytes are
	// legal codes; the content only has to change from frame to frame).
	void FillPackedRgb(std::vector<BYTE>& frame, int height, size_t rowBytes, int n)
	{
		for (int y = 0; y < height; ++y)
			for (size_t x = 0; x < rowBytes; ++x)
				frame[static_cast<size_t>(y) * rowBytes + x] = static_cast<BYTE>(x * 7 + y * 3 + n * 5);
	}

	void FillUyvy(std::vector<BYTE>& frame, int width, int height, size_t rowBytes, int n)
	{
		for (int y = 0; y < height; ++y)
		{
			BYTE* row = frame.data() + rowBytes * y;
			for (int x = 0; x < width; x += 2)
			{
				const BYTE luma = static_cast<BYTE>(16 + ((x + 4 * n) / 32 % 2) * 180 + (y % 16));
				row[x * 2 + 0] = static_cast<BYTE>(128 + ((x + y) % 64) - 32);
				row[x * 2 + 1] = luma;
				row[x * 2 + 2] = static_cast<BYTE>(128 - ((x + y) % 64) + 32);
				row[x * 2 + 3] = luma;
			}
		}
	}

	struct RunResult
	{
		bool created = false, presented = false;
		std::vector<std::string> log;
		std::string dejudderStatus, presentationStatus;   // the stats lines at the end
		std::string ledStatus;   // the crash state's LED drive line (GetLedDriveStatus)
		std::string sourceStatus;   // the crash state's source resolution line (Stage 12)
		std::string captureMetadata;   // the rendered-output capture's JSON sidecar (plan->captureAtSeconds)
		bool Logged(const std::string& text) const
		{
			for (const std::string& line : log)
				if (line.find(text) != std::string::npos) return true;
			return false;
		}
		std::wstring Tail() const
		{
			std::string text;
			for (size_t i = log.size() > 25 ? log.size() - 25 : 0; i < log.size(); ++i) text += log[i] + "\n";
			return std::wstring(text.begin(), text.end());
		}
	};

	// dv_tunnel_gpu_unpack (2026-10-16): a Dolby Vision tunnel signal. The frames
	// are the given r210 pictures (FrameStep::content picks one, modulo the count),
	// each exactly BytesPerFrame long.
	bool g_harnessDolbyVision = false;
	std::vector<std::vector<BYTE>> g_harnessDvFrames;

	// The harness's frame size (1080p unless a test sets it; the 4K stress run).
	int g_harnessWidth = 1920, g_harnessHeight = 1080;
	// The window (the output) off the screen: 640 x 360 unless a test sets it.
	int g_harnessWindowWidth = 640, g_harnessWindowHeight = 360;
	// The queue target (0: 4 for film, 3 otherwise) and, as the app does, the profile snapshot
	// applied before Build (the queue profile reaches the renderer that way).
	size_t g_harnessQueueTarget = 0;
	bool g_harnessApplySnapshot = false;
	// Shader profile sections the run selects after Build (e.g. "standard.sigma_delta";
	// their [shader.*] sections go in the configuration's extra text).
	std::vector<std::string> g_harnessShaderProfiles;
	bool g_harnessShadersSelected = false;

	// Stream faults (owner 2026-10-14): what each captured frame does. The
	// default plan is the plain stream at the run's rate.
	struct FrameStep
	{
		bool send = true;                // false = the capture missed this frame
		int content = 0;                 // the picture: equal content = a bit-exact repeat
		int64_t timestampOffsetUs = 0;   // <= 0: the hardware timestamp earlier (jitter)
	};
	struct FramePlan
	{
		int rateNumerator = 0;           // the signal's rate (0 = the run's own)
		int rateDenominator = 1001;
		double paceScale = 1.0;          // < 1: the source clock runs fast (drift)
		double resetAtSeconds = -1.0;    // an HDMI resync (renderer Reset) at this time
		double captureAtSeconds = -1.0;  // a rendered-output capture (VP-0133) at this time
		std::function<FrameStep(int)> step;
	};

	// The renderer from its DLL, a profile with capture_path, frames for
	// `seconds`, then stopped and destroyed.
	RunResult Run(VideoFrameEncoding encoding, const std::string& capturePath, double seconds,
		bool film = false, const std::string& extra = std::string(), bool onScreen = false,
		const FramePlan* plan = nullptr)
	{
		RunResult result;
		{
			std::lock_guard<std::mutex> guard(g_logMutex);
			g_log.clear();
		}
		const std::string folder = Here();
		const std::string dllPath = folder + "vprenderer\\VideoProcessorVPRenderer.dll";
		HMODULE dll = LoadLibraryExA(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		Assert::IsNotNull(dll, (L"cannot load " + std::wstring(dllPath.begin(), dllPath.end())).c_str());
		using Create = IVideoRenderer* (__cdecl*)(IRendererCallback*, uint32_t, HWND, ITimingClock*, bool, size_t,
			VideoConversionOverride, const char*, VideoProcessorLibplaceboLogSink);
		using Destroy = void(__cdecl*)(IVideoRenderer*);
		const auto create = reinterpret_cast<Create>(GetProcAddress(dll, "VideoProcessorLibplaceboCreateRenderer"));
		const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(dll, "VideoProcessorLibplaceboDestroyRenderer"));
		Assert::IsTrue(create && destroy, L"the renderer DLL's factory");
		// The configuration: the low-latency input profile chosen by the rule.
		const std::string configPath = folder + "vp-capture-harness.cfg";
		{
			std::ofstream config(configPath, std::ios::trunc);
			config << "[general]\npersist_profile_selection: false\n"
				"[vprenderer.input.low_latency]\nwhen: $source_rate > " << (film || plan ? 0 : 59) << "\ncapture_path: " << capturePath << "\n"
				<< extra;
		}
		// A small window off the screen.
		WNDCLASSA wc{};
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = GetModuleHandleA(nullptr);
		wc.lpszClassName = "VpCaptureHarness";
		RegisterClassA(&wc);
		// Off the screen, or (onScreen) in the primary screen's bottom-right
		// corner so the swapchain gets real refresh statistics.
		const int screenW = GetSystemMetrics(SM_CXSCREEN), screenH = GetSystemMetrics(SM_CYSCREEN);
		// VP_TEST_HARNESS_FULLSCREEN=1 (owner allows overnight): the whole
		// primary screen, so the swapchain gets display statistics and the
		// live extras planner runs.
		char* fs = nullptr;
		size_t fsn = 0;
		const bool fullscreen = onScreen && _dupenv_s(&fs, &fsn, "VP_TEST_HARNESS_FULLSCREEN") == 0 && fs && std::string(fs) == "1";
		std::free(fs);
		// Fullscreen: an ordinary activated window (foreground, covering the
		// screen) so presentation can bypass composition and report statistics.
		HWND hwnd = CreateWindowExA(fullscreen ? WS_EX_TOPMOST : (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (onScreen ? WS_EX_TOPMOST : 0)), wc.lpszClassName,
			"VP capture harness", WS_POPUP, fullscreen ? 0 : onScreen ? screenW - 660 : -4000, fullscreen ? 0 : onScreen ? screenH - 420 : -4000,
			fullscreen ? screenW : g_harnessWindowWidth, fullscreen ? screenH : g_harnessWindowHeight, nullptr, nullptr, wc.hInstance, nullptr);
		Assert::IsNotNull(hwnd);
		ShowWindow(hwnd, fullscreen ? SW_SHOW : SW_SHOWNOACTIVATE);
		if (fullscreen)
			SetForegroundWindow(hwnd);
		Callback callback;
		Clock clock;
		IVideoRenderer* renderer = create(&callback, 1, hwnd, &clock, true, 8,
			VideoConversionOverride::VIDEOCONVERSION_NONE, configPath.c_str(), &CaptureLog);
		result.created = renderer != nullptr;
		if (renderer)
		{
			const int width = g_harnessWidth, height = g_harnessHeight;
			VideoStateComPtr state = new VideoState();
			state->valid = true;
			const int rateNumerator = plan && plan->rateNumerator > 0 ? plan->rateNumerator : film ? 24000 : 60000;
			const int rateDenominator = plan && plan->rateNumerator > 0 ? plan->rateDenominator : 1001;
			state->displayMode = std::make_shared<DisplayMode>(width, height, false, rateNumerator, rateDenominator);
			state->videoFrameEncoding = encoding;
			state->colorspace = ColorSpace::REC_709;
			state->eotf = EOTF::SDR;
			state->dolbyVisionTunnel = g_harnessDolbyVision;
			renderer->OnVideoState(state);
			if (g_harnessApplySnapshot)
			{
				ConfigFile profileConfig;
				std::string error;
				UnifiedProfileRuntime::Runtime runtime;
				if (profileConfig.Load(configPath) && runtime.Initialize(profileConfig, {}, error))
				{
					const auto snapshot = runtime.GetSnapshot();
					CString activeState;
					bool restart = false, live = false;
					if (snapshot) renderer->ApplyApplicationState(*snapshot, activeState, restart, live);
				}
				else
					CaptureLog(("harness: the profile snapshot failed: " + error).c_str());
			}
			renderer->Build();
			// As the app does: the monitor's refresh rate (smooth motion needs it;
			// it does not come from the window's frame statistics).
			{
				DEVMODEW mode{};
				mode.dmSize = sizeof(mode);
				if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
					renderer->SetRenderLoadDisplayRefreshRate(static_cast<double>(mode.dmDisplayFrequency));
			}
			g_harnessShadersSelected = false;
			if (!g_harnessShaderProfiles.empty())
			{
				CString activeShaders;
				bool restart = false;
				g_harnessShadersSelected = renderer->SelectShaderProfiles(g_harnessShaderProfiles, activeShaders, restart);
				if (restart)
					renderer->Build();
			}
			renderer->SetQueueFramePolicy(2, g_harnessQueueTarget ? g_harnessQueueTarget : (film ? 4 : 3), true);
			renderer->Start();
			// A ring of frames that outlives anything the queue holds.
			const size_t rowBytes = static_cast<size_t>(state->BytesPerRow());
			std::vector<std::vector<BYTE>> ring(24, std::vector<BYTE>(static_cast<size_t>(state->BytesPerFrame())));
			std::vector<SourceBuffer> buffers(ring.size());
			const auto start = std::chrono::steady_clock::now();
			const auto period = std::chrono::microseconds(static_cast<int64_t>(
				1e6 * rateDenominator / rateNumerator * (plan ? plan->paceScale : 1.0)));
			const double ticksPerUs = static_cast<double>(clock.TimingClockTicksPerSecond()) / 1e6;
			bool resetDone = false, captureRequested = false;
			const timingclocktime_t startTicks = clock.TimingClockNow();
			for (int n = 0; std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds); ++n)
			{
				const FrameStep step = plan && plan->step ? plan->step(n) : FrameStep{ true, n, 0 };
				if (plan && !resetDone && plan->resetAtSeconds >= 0.0 &&
					std::chrono::steady_clock::now() - start >= std::chrono::duration<double>(plan->resetAtSeconds))
				{
					// The player's HDMI re-sync: Reset, then the (same) signal again.
					renderer->Reset();
					renderer->OnVideoState(state);
					resetDone = true;
				}
				if (plan && !captureRequested && plan->captureAtSeconds >= 0.0 &&
					std::chrono::steady_clock::now() - start >= std::chrono::duration<double>(plan->captureAtSeconds))
				{
					CString captureStatus;
					const bool accepted = renderer->RequestRenderedOutputCapture(captureStatus);
					Logger::WriteMessage(("capture request: " + std::string(accepted ? "accepted - " : "refused - ") +
						CStringA(captureStatus).GetString() + "\n").c_str());
					captureRequested = true;
				}
				if (step.send)
				{
					std::vector<BYTE>& buffer = ring[n % ring.size()];
					if (!g_harnessDvFrames.empty())
					{
						const std::vector<BYTE>& picture = g_harnessDvFrames[static_cast<size_t>(step.content) % g_harnessDvFrames.size()];
						std::copy(picture.begin(), picture.begin() + (std::min)(picture.size(), buffer.size()), buffer.begin());
					}
					else if (encoding == VideoFrameEncoding::V210) FillV210(buffer, width, height, rowBytes, step.content);
					else if (encoding == VideoFrameEncoding::R12B || encoding == VideoFrameEncoding::R12L ||
						encoding == VideoFrameEncoding::R210 || encoding == VideoFrameEncoding::R10b ||
						encoding == VideoFrameEncoding::R10l || encoding == VideoFrameEncoding::ARGB_8BIT ||
						encoding == VideoFrameEncoding::BGRA_8BIT)
						FillPackedRgb(buffer, height, rowBytes, step.content);
					else FillUyvy(buffer, width, height, rowBytes, step.content);
					// The hardware timestamp: the frame's scheduled time, as a
					// capture card stamps it, not when this (busy) loop sent it.
					const timingclocktime_t stamp = startTicks +
						static_cast<timingclocktime_t>(static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
							period * n).count() + step.timestampOffsetUs) * ticksPerUs);
					VideoFrame frame(buffer.data(), static_cast<uint64_t>(n + 1), stamp, &buffers[n % ring.size()], stamp);
					renderer->OnVideoFrame(frame);
				}
				MSG msg;
				while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
				std::this_thread::sleep_until(start + period * (n + 1));
			}
			if (captureRequested)
			{
				// The capture's file is written asynchronously: wait for its log line.
				const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				while (std::chrono::steady_clock::now() < until && result.captureMetadata.empty())
				{
					MSG msg;
					while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
					{
						std::lock_guard<std::mutex> guard(g_logMutex);
						for (const std::string& line : g_log)
						{
							const size_t at = line.find("Rendered-output capture complete");
							const size_t m = line.find("metadata=");
							if (at != std::string::npos && m != std::string::npos)
							{
								const size_t end = line.find(" raw_bytes=", m);
								result.captureMetadata = line.substr(m + 9, end == std::string::npos ? std::string::npos : end - m - 9);
							}
						}
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
			}
			result.presented = renderer->HasPresentedLiveFrame();
			CString status;
			if (renderer->GetDejudderStatus(status)) result.dejudderStatus = CStringA(status).GetString();
			if (renderer->GetLedDriveStatus(status)) result.ledStatus = CStringA(status).GetString();
			if (renderer->GetSourceResolutionStatus(status)) result.sourceStatus = CStringA(status).GetString();
			status.Empty();
			if (renderer->GetPresentationTimingStatus(status)) result.presentationStatus = CStringA(status).GetString();
			renderer->Stop();
			destroy(renderer);
		}
		DestroyWindow(hwnd);
		{
			std::lock_guard<std::mutex> guard(g_logMutex);
			result.log = g_log;
		}
		return result;
	}
}

namespace Tests
{
	TEST_CLASS(RendererCaptureHarnessTests)
	{
	public:
		TEST_METHOD(OnScreenLiveExtrasAndEstimateAhead)
		{
			// Overnight only (owner 2026-10-13): VP_TEST_HARNESS_ONSCREEN=1 with
			// VP_TEST_GPU=hardware. A small window on the screen gets real
			// refresh statistics, so VP's live extras planner and estimate ahead
			// run: 23.976p with the de-judder (standard and gpu, ahead on and
			// off) and 59.94p (standard and gpu). A report: the de-judder and
			// presentation lines and the log's de-judder summary per run.
			char* v = nullptr;
			size_t n = 0;
			const bool on = _dupenv_s(&v, &n, "VP_TEST_HARNESS_ONSCREEN") == 0 && v && std::string(v) == "1";
			std::free(v);
			if (!on || !OnHardware())
			{
				Logger::WriteMessage(L"on-screen harness: VP_TEST_HARNESS_ONSCREEN=1 and VP_TEST_GPU=hardware only; not run");
				return;
			}
			char* sv = nullptr;
			const double seconds = _dupenv_s(&sv, &n, "VP_TEST_HARNESS_SECONDS") == 0 && sv ? std::atof(sv) : 60.0;
			std::free(sv);
			struct Case { const char* name; const char* path; bool film; const char* extra; };
			const Case cases[] = {
				{ "23.976p standard, ahead on", "standard", true, "dejudder_strength: 0.25@occlusion_mask: on@" },
				{ "23.976p gpu, ahead on", "gpu", true, "dejudder_strength: 0.25@occlusion_mask: on@" },
				{ "23.976p standard, ahead off", "standard", true, "dejudder_strength: 0.25@occlusion_mask: on@dejudder_estimate_ahead: off@" },
				{ "23.976p gpu, ahead off", "gpu", true, "dejudder_strength: 0.25@occlusion_mask: on@dejudder_estimate_ahead: off@" },
				{ "59.94p standard", "standard", false, "" },
				{ "59.94p gpu", "gpu", false, "" } };
			int failures = 0;
			for (const Case& c : cases)
			{
				std::string extra = c.extra;
				for (char& ch : extra) if (ch == '@') ch = '\n';
				const RunResult r = Run(VideoFrameEncoding::V210, c.path, seconds > 5.0 ? seconds : 60.0, c.film, extra, true);
				Logger::WriteMessage((std::string("== ") + c.name + ": presented " + (r.presented ? "yes" : "NO")).c_str());
				Logger::WriteMessage(("   de-judder: " + r.dejudderStatus).c_str());
				Logger::WriteMessage(("   presentation: " + r.presentationStatus).c_str());
				std::string lastAhead, lastHold, phase;
				int extrasLines = 0;
				for (const std::string& line : r.log)
				{
					if (line.find("De-judder estimate ahead:") != std::string::npos) lastAhead = line;
					if (line.find("De-judder: hold") != std::string::npos) lastHold = line;
					if (line.find("De-judder phase:") != std::string::npos) phase = line;
					if (line.find("De-judder extra:") != std::string::npos) ++extrasLines;
				}
				Logger::WriteMessage(("   " + phase).c_str());
				Logger::WriteMessage(("   " + lastAhead).c_str());
				Logger::WriteMessage(("   " + lastHold).c_str());
				Logger::WriteMessage(("   extras logged: " + std::to_string(extrasLines)).c_str());
				// The GPU used, the dither's vector source and every hold reason.
				std::map<std::string, int> holds;
				for (const std::string& line : r.log)
				{
					if (line.find("VP_TEST_ADAPTER") != std::string::npos || line.find("Sigma-delta vectors") != std::string::npos)
						Logger::WriteMessage(("   " + line).c_str());
					const size_t at = line.find("De-judder: hold (");
					if (at != std::string::npos)
						++holds[line.substr(at + 17, line.find(';', at) - at - 17)];
				}
				for (const auto& hold : holds)
					Logger::WriteMessage(("   hold: " + hold.first + " x" + std::to_string(hold.second)).c_str());
				failures += r.presented ? 0 : 1;
			}
			Assert::AreEqual(0, failures, L"every on-screen run presented frames");
		}

		TEST_METHOD(RecordedStreamReplaysThroughTheRenderer)
		{
			// Owner 2026-10-15: a recording from VideoProcessorStreamRecorder
			// (frames.csv: hardware times, periods since the previous frame,
			// bit-exact repeats) replayed through the real renderer with
			// pulldown on - its timing, gaps, jitter and repeat cadence as
			// captured. VP_TEST_STREAM_DIR=<recording folder>; without it a
			// synthetic recording (24p in 29.97 Hz with a 2-frame gap) checks
			// the replay itself. Hardware only.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"stream replay: VP_TEST_GPU=hardware only; not run");
				return;
			}
			char* dirValue = nullptr;
			size_t dn = 0;
			std::string dir = _dupenv_s(&dirValue, &dn, "VP_TEST_STREAM_DIR") == 0 && dirValue ? dirValue : "";
			std::free(dirValue);
			const bool synthetic = dir.empty();
			if (synthetic)
			{
				// 29.97 Hz, 24p carried with one repeat in five, frames 90-91 missed.
				dir = Here() + "stream-replay-synthetic";
				CreateDirectoryA(dir.c_str(), nullptr);
				std::ofstream csv(dir + "\\frames.csv", std::ios::trunc);
				csv << "index,hw_time_100ns,hw_duration_100ns,stream_time,stream_duration,callback_qpc,periods_since_previous,buffered,flags,no_input,hash,exact_repeat,callback_us\n";
				const long long duration = 333667;   // 100 ns units at 29.97 Hz
				int index = 0;
				long long lastSlot = -1;
				for (long long slot = 0; slot < 300; ++slot)
				{
					if (slot == 90 || slot == 91)
						continue;
					const bool repeat = slot % 5 == 4;
					csv << index++ << "," << slot * duration + (slot * 7919 % 2000) << "," << duration << ",0,0,0,"
						<< (lastSlot < 0 ? 0 : slot - lastSlot) << ",0,0x0,0,0," << (repeat ? 1 : 0) << ",0\n";
					lastSlot = slot;
				}
			}
			// The recording: per frame its hardware time, periods and repeat.
			struct Row { long long time = 0, duration = 0; int periods = 0; bool repeat = false; };
			std::vector<Row> rows;
			{
				std::ifstream csv(dir + "\\frames.csv");
				Assert::IsTrue(csv.good(), L"stream replay: no frames.csv");
				std::string line;
				std::getline(csv, line);   // header
				while (std::getline(csv, line))
				{
					std::vector<std::string> f;
					std::stringstream ss(line);
					std::string cell;
					while (std::getline(ss, cell, ',')) f.push_back(cell);
					if (f.size() < 12) continue;
					Row r;
					r.time = std::atoll(f[1].c_str());
					r.duration = std::atoll(f[2].c_str());
					r.periods = std::atoi(f[6].c_str());
					r.repeat = f[11] == "1";
					if (f.size() > 9 && f[9] == "1") continue;   // no input
					rows.push_back(r);
				}
			}
			Assert::IsTrue(rows.size() > 30, L"stream replay: too few frames");
			std::vector<long long> durations;
			for (const Row& r : rows) if (r.duration > 0) durations.push_back(r.duration);
			std::sort(durations.begin(), durations.end());
			const long long duration = durations[durations.size() / 2];
			const double signalHz = 1.0e7 / static_cast<double>(duration);
			// Signal slots: a frame's slot from the periods; missed slots are
			// not sent; the jitter is the hardware time against the slot.
			struct Slot { bool send = false; int content = 0; long long offset100ns = 0; };
			std::vector<Slot> slots;
			int content = 0;
			for (size_t i = 0; i < rows.size(); ++i)
			{
				const int step = i == 0 ? 1 : std::max(1, rows[i].periods);
				for (int k = 1; k < step; ++k) slots.push_back(Slot{});   // missed
				if (!rows[i].repeat) ++content;
				Slot s;
				s.send = true;
				s.content = content;
				const long long ideal = rows[0].time + static_cast<long long>(slots.size()) * duration;
				s.offset100ns = rows[i].time - ideal;
				slots.push_back(s);
			}
			long long maxOffset = 0;
			for (const Slot& s : slots) if (s.send) maxOffset = std::max(maxOffset, s.offset100ns);
			FramePlan plan;
			plan.rateNumerator = static_cast<int>(std::lround(signalHz * 1000.0));
			plan.rateDenominator = 1000;
			plan.step = [slots, maxOffset](int n)
			{
				FrameStep f;
				if (n >= static_cast<int>(slots.size())) { f.send = false; return f; }
				f.send = slots[n].send;
				f.content = slots[n].content;
				f.timestampOffsetUs = (slots[n].offset100ns - maxOffset) / 10;   // never ahead of its slot
				return f;
			};
			const double seconds = std::min(60.0, slots.size() / signalHz);
			const RunResult r = Run(VideoFrameEncoding::V210, "standard", seconds, false,
				"dejudder_strength: 0.25\ndejudder_pulldown: on\n", false, &plan);
			char head[160];
			std::snprintf(head, sizeof(head), "== stream replay (%s): %zu frames at %.3f Hz, %.1f s",
				synthetic ? "synthetic" : dir.c_str(), rows.size(), signalHz, seconds);
			Logger::WriteMessage(head);
			Logger::WriteMessage(("   de-judder: " + r.dejudderStatus).c_str());
			int locks = 0, missed = 0, invalidations = 0;
			for (const std::string& line : r.log)
			{
				if (line.find("Pulldown:") != std::string::npos) { ++locks; Logger::WriteMessage(("   " + line).c_str()); }
				if (line.find("capture missed") != std::string::npos) { ++missed; Logger::WriteMessage(("   " + line).c_str()); }
				if (line.find("motion invalidated") != std::string::npos) ++invalidations;
			}
			Logger::WriteMessage(("   pulldown lines " + std::to_string(locks) + ", missed lines " + std::to_string(missed) +
				", invalidations " + std::to_string(invalidations)).c_str());
			Assert::IsTrue(r.presented, (L"stream replay: frames presented\n" + r.Tail()).c_str());
			if (synthetic)
			{
				Assert::IsTrue(r.Logged("(4 new of 5)"), (L"synthetic replay: pulldown locks\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("capture missed"), (L"synthetic replay: the gap is counted\n" + r.Tail()).c_str());
			}
		}

		TEST_METHOD(OpticalFlowIsTheDefaultMotionSource)
		{
			// docs/VP-nvof-ahead.md (owner 2026-10-16): the optical-flow engine is the
			// established default motion source; optical_flow: off = the block searches.
			// Without the engine (not NVIDIA) the default falls back to the searches.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"optical flow default: VP_TEST_GPU=hardware only; not run");
				return;
			}
			auto source = [](const RunResult& r) {
				std::string last;
				for (const std::string& line : r.log)
					if (line.find("De-judder motion:") != std::string::npos) last = line;
				return last;
			};
			const RunResult on = Run(VideoFrameEncoding::V210, "gpu", 4.0, true, "dejudder_strength: 0.25\n");
			const RunResult off = Run(VideoFrameEncoding::V210, "gpu", 4.0, true, "dejudder_strength: 0.25\noptical_flow: off\n");
			Logger::WriteMessage(("default: " + source(on) + "\n").c_str());
			Logger::WriteMessage(("off: " + source(off) + "\n").c_str());
			// The producer ahead (docs/VP-nvof-ahead.md): its decisions and statistics.
			for (const std::string& line : on.log)
				if (line.find("Motion ahead") != std::string::npos || line.find("De-judder motion:") != std::string::npos)
					Logger::WriteMessage(("  " + line + "\n").c_str());
			Assert::IsTrue(on.presented && off.presented, L"both runs presented");
			Assert::IsFalse(source(on).empty(), (L"the default run reports its motion source\n" + on.Tail()).c_str());
			Assert::IsTrue(source(on).find("optical_flow off") == std::string::npos, L"the default is not off");
			Assert::IsTrue(source(off).find("block searches (optical_flow off)") != std::string::npos,
				(L"optical_flow: off runs the searches\n" + off.Tail()).c_str());
			if (source(on).find("optical-flow engine") == std::string::npos)
				Logger::WriteMessage("note: no optical-flow engine on this GPU - the default fell back to the searches\n");
			// Planner Q1: with the pair computed ahead, the camera motion comes from the engine.
			bool ahead = false, cameraFromEngine = false;
			for (const std::string& line : on.log)
			{
				ahead = ahead || line.find("optical-flow engine (computed ahead") != std::string::npos;
				cameraFromEngine = cameraFromEngine || line.find("camera from the engine") != std::string::npos;
			}
			if (ahead)
				Assert::IsTrue(cameraFromEngine, (L"pairs computed ahead: the camera from the engine\n" + on.Tail()).c_str());
		}

		TEST_METHOD(StreamFaultsKeepTheDejudderSafeAndRecovering)
		{
			// Stream robustness and pulldown (owner 2026-10-14): each fault
			// class injected into a 23.976p de-judder run, plus the pulldown
			// cadences, through the real renderer. Hardware only.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"stream faults: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const std::string dejudder = "dejudder_strength: 0.25\n";
			auto after = [](const RunResult& r, const std::string& marker, const std::string& text)
			{
				bool seen = false;
				for (const std::string& line : r.log)
				{
					if (line.find(marker) != std::string::npos) seen = true;
					else if (seen && line.find(text) != std::string::npos) return true;
				}
				return false;
			};
			auto report = [](const char* name, const RunResult& r)
			{
				Logger::WriteMessage((std::string("== ") + name + ": " + r.dejudderStatus).c_str());
				for (const std::string& line : r.log)
					if (line.find("missed") != std::string::npos || line.find("discontinuity") != std::string::npos ||
						line.find("motion invalidated") != std::string::npos || line.find("Pulldown") != std::string::npos)
						Logger::WriteMessage(("   " + line).c_str());
			};
			// Missed frames: two, then three, lost mid-stream. Counted (no rate
			// reset), the motion history forgotten, and the de-judder estimates again.
			{
				FramePlan plan;
				plan.step = [](int n) { FrameStep f; f.content = n; f.send = !(n == 60 || n == 61 || (n >= 90 && n <= 92)); return f; };
				const RunResult r = Run(VideoFrameEncoding::V210, "standard", 5.0, true, dejudder, false, &plan);
				report("missed frames", r);
				Assert::IsTrue(r.Logged("capture missed"), (L"missed: counted\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("capture timestamp discontinuity"), (L"missed: no rate reset\n" + r.Tail()).c_str());
				// The history is forgotten after the gap (the cause may read
				// "backlog flush" when an off-screen stall flushes the queue then).
				Assert::IsTrue(after(r, "capture missed", "motion invalidated"),(L"missed: history forgotten\n" + r.Tail()).c_str());
				Assert::IsTrue(after(r, "motion invalidated", "De-judder: estimating again"), (L"missed: recovers\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented);
			}
			// A duplicated picture (same content, on time): not a gap.
			{
				FramePlan plan;
				plan.step = [](int n) { FrameStep f; f.content = n == 50 ? 49 : n; return f; };
				const RunResult r = Run(VideoFrameEncoding::V210, "standard", 4.0, true, dejudder, false, &plan);
				report("duplicate", r);
				Assert::IsFalse(r.Logged("capture missed"), (L"duplicate: not a gap\n" + r.Tail()).c_str());
				// (An off-screen run may flush a backlog while shaders compile;
				// only a duplicate-caused invalidation would be wrong.)
				Assert::IsFalse(r.Logged("motion invalidated (missed frames") || r.Logged("motion invalidated (frame gap"),
					(L"duplicate: no invalidation from it\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("De-judder: motion estimated"));
				Assert::IsTrue(r.presented);
			}
			// Timestamp jitter up to 3 ms: no gap, no rate reset.
			{
				FramePlan plan;
				plan.step = [](int n) { FrameStep f; f.content = n; f.timestampOffsetUs = -static_cast<int64_t>((n * 7919) % 3000); return f; };
				const RunResult r = Run(VideoFrameEncoding::V210, "standard", 4.0, true, dejudder, false, &plan);
				report("jitter", r);
				Assert::IsFalse(r.Logged("capture missed"), (L"jitter: not a gap\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("capture timestamp discontinuity"), (L"jitter: no rate reset\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("motion invalidated (missed frames") || r.Logged("motion invalidated (frame gap"),
					(L"jitter: no invalidation from it\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("De-judder: motion estimated"));
			}
			// Clock drift: the source 0.1% fast (far beyond real ppm).
			{
				FramePlan plan;
				plan.paceScale = 0.999;
				plan.step = [](int n) { FrameStep f; f.content = n; return f; };
				const RunResult r = Run(VideoFrameEncoding::V210, "standard", 4.0, true, dejudder, false, &plan);
				report("drift", r);
				Assert::IsFalse(r.Logged("capture missed"), (L"drift: not a gap\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("motion invalidated (missed frames") || r.Logged("motion invalidated (frame gap"),
					(L"drift: no invalidation from it\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("De-judder: motion estimated"));
			}
			// A mid-stream HDMI re-sync: the de-judder released, then back.
			{
				FramePlan plan;
				plan.resetAtSeconds = 2.0;
				plan.step = [](int n) { FrameStep f; f.content = n; return f; };
				const RunResult r = Run(VideoFrameEncoding::V210, "standard", 5.0, true, dejudder, false, &plan);
				report("resync", r);
				Assert::IsTrue(r.Logged("motion invalidated (resync"), (L"resync: released\n" + r.Tail()).c_str());
				Assert::IsTrue(after(r, "motion invalidated (resync", "De-judder: estimating again"), (L"resync: recovers\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented);
			}
			// Pulldown: 24p carried in 29.97 (one repeat in five), in 59.94 (2:3)
			// and 30p in 59.94 (2:2): locked, the repeats skipped, the de-judder
			// on the film frames.
			struct Cadence { const char* name; int rate; const char* pattern; const char* lockText; const char* cycleText; const char* extra; };
			const Cadence cadences[] = {
				{ "24p in 29.97", 30000, "NNNNR", "p film in 29.9", "(4 new of 5)", "dejudder_strength: 0.25\n" },
				{ "24p in 59.94", 60000, "NRNRR", "p film in 59.9", "(2 new of 5)", "dejudder_strength: 0.25\n" },
				{ "30p in 59.94", 60000, "NR", "p film in 59.9", "(1 new of 2)", "dejudder_strength: 0.25\ndejudder_strength_even: 0.5\ndejudder_even_rates: 30_and_29.97\n" } };
			for (const Cadence& c : cadences)
			{
				FramePlan plan;
				plan.rateNumerator = c.rate;
				const std::string pattern = c.pattern;
				plan.step = [pattern](int n)
				{
					// The film frame index: advances on each 'N' slot.
					FrameStep f;
					int film = 0;
					for (int i = 1; i <= n; ++i) film += pattern[i % pattern.size()] == 'N' ? 1 : 0;
					f.content = film;
					return f;
				};
				const RunResult on = Run(VideoFrameEncoding::V210, "standard", 5.0, false,
					std::string(c.extra) + "dejudder_pulldown: on\n", false, &plan);
				report(c.name, on);
				Assert::IsTrue(on.Logged(c.lockText) && on.Logged(c.cycleText), (std::wstring(L"pulldown locks: ") +
					std::wstring(c.name, c.name + std::strlen(c.name)) + L"\n" + on.Tail()).c_str());
				Assert::IsTrue(on.dejudderStatus.find("repeats skipped") != std::string::npos,
					(L"pulldown on the stats line\n" + on.Tail()).c_str());
				Assert::IsTrue(after(on, "Pulldown: ", "De-judder: motion estimated"),
					(L"the de-judder runs on the film frames\n" + on.Tail()).c_str());
				Assert::IsTrue(on.presented);
				// Off (the default): nothing is detected or skipped.
				const RunResult off = Run(VideoFrameEncoding::V210, "standard", 3.0, false, c.extra, false, &plan);
				Assert::IsFalse(off.Logged("Pulldown:"), (L"pulldown off by default\n" + off.Tail()).c_str());
			}
			// Film then video: the lock is lost and no new picture is skipped.
			{
				FramePlan plan;
				plan.rateNumerator = 30000;
				plan.step = [](int n)
				{
					FrameStep f;
					const int switchAt = 90;   // 3 s of 24-in-30, then 30p video
					int film = 0;
					for (int i = 1; i <= (n < switchAt ? n : switchAt); ++i) film += i % 5 != 4 ? 1 : 0;
					f.content = n < switchAt ? film : film + (n - switchAt) + 1;
					return f;
				};
				const RunResult r = Run(VideoFrameEncoding::V210, "standard", 5.0, false,
					dejudder + "dejudder_pulldown: on\n", false, &plan);
				report("film then video", r);
				Assert::IsTrue(r.Logged("(4 new of 5)"), (L"locks on the film\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Pulldown: lost"), (L"loses the lock on video\n" + r.Tail()).c_str());
			}
		}

		TEST_METHOD(Stress4K60Rgb12EarlyUpload)
		{
			// The maximum stress case (owner 2026-10-15): 3840x2160 12-bit RGB (R12B,
			// the largest capture frame, 37 MB) at 59.94 Hz through the renderer, with
			// early upload on and off. Checked: frames presented, no unpack failures;
			// reported: frames read early, no-free-slot counts, the most frames held
			// at once and the presentation stats (drops) of both runs. Whether the
			// whole chain holds 4K60 in real time is the RTX 3090's to show; the
			// laptop shows the upload pipeline under the heaviest load.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			g_harnessWidth = 3840;
			g_harnessHeight = 2160;
			for (const bool early : { true, false })
			{
				const RunResult r = Run(VideoFrameEncoding::R12B, "gpu", 6.0, false, early ? "capture_upload: early\n" : "");
				const std::wstring name = early ? L"4K60 R12B early" : L"4K60 R12B render upload";
				Logger::WriteMessage((std::string(early ? "[early] " : "[render] ") + "presentation: " + r.presentationStatus + "\n").c_str());
				for (const std::string& line : r.log)
					if (line.find("Early upload:") != std::string::npos || line.find("Capture raw route") != std::string::npos ||
						line.find("Native RGB ingest") != std::string::npos || line.find("VP_TEST_ADAPTER selects") != std::string::npos ||
						line.find("libplacebo initialized") != std::string::npos)
						Logger::WriteMessage(line.c_str());
				Assert::IsTrue(r.created, (name + L": renderer not created\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("GPU unpack failed"), (name + L": no unpack failure\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Early upload: not available"), (name + L": the pool opened\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (name + L": frames presented\n" + r.Tail()).c_str());
			}
			g_harnessWidth = 1920;
			g_harnessHeight = 1080;
		}

		TEST_METHOD(SourceResolutionRebasesThroughTheRenderer)
		{
			// Stage 12 step A (owner 2026-10-15): a 4K source set to 1080 is
			// re-based for libplacebo (the log names the sizes) and presents; any
			// height works (900); native (the default) never re-bases.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const int savedW = g_harnessWidth, savedH = g_harnessHeight;
			g_harnessWidth = 3840;
			g_harnessHeight = 2160;
			struct Case { const char* extra; const char* expected; const wchar_t* name; };
			for (const Case& c : { Case{ "source_resolution: 1080\n", "Re-base: 3840x2160 -> 1920x1080", L"1080" },
				Case{ "source_resolution: 900\n", "Re-base: 3840x2160 -> 1600x900", L"900" },
				Case{ "source_resolution: 1080\ndejudder_strength: 0.25\n", "Re-base: 3840x2160 -> 1920x1080", L"1080 with the de-judder" },
				Case{ "source_resolution: 1080\ncapture_upload: early\n", "Re-base: 3840x2160 -> 1920x1080", L"1080 with the early upload" } })
			{
				const bool film = std::string(c.extra).find("dejudder") != std::string::npos;
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 4.0, film, c.extra);
				Assert::IsTrue(r.Logged(c.expected), (std::wstring(c.name) + L": re-based\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Re-base failed"), (std::wstring(c.name) + L": no failure\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (std::wstring(c.name) + L": frames presented\n" + r.Tail()).c_str());
			}
			{
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 2.0);
				Assert::IsFalse(r.Logged("Re-base: 3840x2160"), (L"native: never re-based\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (L"native: frames presented\n" + r.Tail()).c_str());
				// Step B reads every source (the crash-state line).
				Logger::WriteMessage(("native: " + r.sourceStatus + "\n").c_str());
				Assert::IsTrue(r.sourceStatus.find("Source resolution:") == 0, (L"native: status line\n" + r.Tail()).c_str());
				// The default is auto (owner 2026-10-16: the Picture slider's Standard); the 4K
				// harness picture stays native under it.
				Assert::IsTrue(r.sourceStatus.find("source_resolution auto") != std::string::npos, (L"default: auto\n" + r.Tail()).c_str());
			}
			{
				// Auto through the renderer (owner 2026-10-15): the harness picture
				// is generated at 4K, so auto reads it and stays native within a
				// short run (it needs several shots and a minute before acting).
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 4.0, false, "source_resolution: auto\n");
				Logger::WriteMessage(("auto: " + r.sourceStatus + "\n").c_str());
				Assert::IsTrue(r.presented, (L"auto: frames presented\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Source resolution: the tile") || r.Logged("Source resolution: cannot") ||
					r.Logged("Source resolution: the readback"), (L"auto: no GPU failure\n" + r.Tail()).c_str());
				Assert::IsTrue(r.sourceStatus.find("source_resolution auto") != std::string::npos, (L"auto: status line\n" + r.Tail()).c_str());
				Assert::IsTrue(r.sourceStatus.find("not read") == std::string::npos, (L"auto: the source is read\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Re-base: 3840x2160"), (L"auto: not re-based within a short run\n" + r.Tail()).c_str());
				// Look-ahead verification (owner 2026-10-16): the queued frames reach the monitor.
				Assert::IsTrue(r.Logged("Source resolution look-ahead: watching"), (L"auto: the frames ahead are watched\n" + r.Tail()).c_str());
			}
			g_harnessWidth = savedW;
			g_harnessHeight = savedH;
		}

		TEST_METHOD(EveryProfileKeyChangesTheLiveSettingsFingerprint)
		{
			// Planner Q4 (2026-10-16): a profile edit is applied live only when it changes the
			// renderer's effective-settings fingerprint. Structural: every key of an applied profile
			// enters it, so the keys once missed (source_range, capture_upload, the Stage 12 keys) and
			// a key not invented yet all change it; the same file twice gives the same fingerprint.
			const std::string folder = Here();
			HMODULE dll = LoadLibraryExA((folder + "vprenderer\\VideoProcessorVPRenderer.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			Assert::IsNotNull(dll, L"the renderer DLL");
			using Fingerprint = int(__cdecl*)(const char*, char*, int);
			const auto fingerprintOf = reinterpret_cast<Fingerprint>(GetProcAddress(dll, "VideoProcessorLibplaceboSettingsFingerprintForTest"));
			Assert::IsNotNull(reinterpret_cast<void*>(fingerprintOf), L"the test export");
			const std::string path = folder + "vp-fingerprint-test.cfg";
			auto fingerprint = [&](const std::string& extra) {
				{
					std::ofstream config(path, std::ios::trunc);
					config << "[general]\npersist_profile_selection: false\n[vprenderer.Default]\nquality: high\n" << extra;
				}
				std::vector<char> buffer(1 << 20);
				const int length = fingerprintOf(path.c_str(), buffer.data(), static_cast<int>(buffer.size()));
				Assert::IsTrue(length > 0, L"fingerprint computed");
				return std::string(buffer.data());
			};
			const std::string base = fingerprint("");
			Assert::AreEqual(base, fingerprint(""), L"the same file gives the same fingerprint");
			const char* const changes[] = { "source_range: full\n", "capture_upload: early\n", "source_resolution: 1080\n",
				"source_resolution_auto_min_confidence: 0.9\n", "source_resolution_text_guard: off\n", "source_resolution_evidence_shots: 5\n", "source_resolution_evidence_seconds: 30\n", "optical_flow_global: off\n", "temporal_dither: off\n", "deband_vp: auto\n", "low_bitrate_handling: off\n",
				"cnn_residual_stabilise: off\n", "ssim_residual_stabilise: off\n", "ssim_bitrate_aware: off\n",
				"ssim_bitrate_floor: 0.5\n", "dejudder_strength: 0.25\n", "dejudder_object_strength: 0.5\n",
				"a_key_not_invented_yet: 1\n" };
			for (const char* change : changes)
				Assert::AreNotEqual(base, fingerprint(change),
					(L"no fingerprint change for: " + std::wstring(change, change + strlen(change))).c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(NativeAndNewSettingsOffArePixelIdentical)
		{
			// Stage 12 (owner 2026-10-16): the exact output codes (VP-0133's
			// raw_r10_sha256 of the presented frame) of a still picture with no
			// Stage 12 keys, with source_resolution native, with auto (a short
			// run stays native) and with every new setting off are identical; a
			// 1080 re-base differs (the check can see a change).
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const int savedW = g_harnessWidth, savedH = g_harnessHeight;
			g_harnessWidth = 3840;
			g_harnessHeight = 2160;
			// Captures into a writable temporary folder (the test runner lives in Program Files).
			wchar_t tempPath[MAX_PATH] = {};
			GetTempPathW(MAX_PATH, tempPath);
			const std::wstring shots = std::wstring(tempPath) + L"vp-pixel-identity";
			CreateDirectoryW(shots.c_str(), nullptr);
			SetEnvironmentVariableW(L"VP_TEST_SCREENSHOT_DIR", shots.c_str());
			FramePlan plan;
			plan.captureAtSeconds = 2.5;
			plan.step = [](int) { return FrameStep{ true, 0, 0 }; };   // one still picture
			auto hashOf = [&](const char* extra, const wchar_t* name) {
				// Dithering off in the rendering profile: VP's dither is temporal (its noise changes every
				// frame, so captures of different frames differ by design). A [vprenderer] / [display]
				// section would switch the file to the display-level layout and the input profile with the
				// settings under test would not apply (measured).
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 3.5, false, std::string(extra) + "[vprenderer.Default]\ndithering: off\ndeband: off\n", false, &plan);
				if (r.captureMetadata.empty())
					for (const std::string& line : r.log)
						if (line.find("apture") != std::string::npos && line.find("capture_") == std::string::npos)
							Logger::WriteMessage(("  " + line + "\n").c_str());
				Assert::IsFalse(r.captureMetadata.empty(), (std::wstring(name) + L": capture written\n" + r.Tail()).c_str());
				std::ifstream in(std::wstring(r.captureMetadata.begin(), r.captureMetadata.end()));
				std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				const size_t k = text.find("\"raw_r10_sha256\": \"");
				Assert::IsTrue(k != std::string::npos, (std::wstring(name) + L": hash in the sidecar").c_str());
				const std::string hash = text.substr(k + 19, 64);
				Logger::WriteMessage((std::string(name, name + wcslen(name)) + ": " + hash + "\n").c_str());
				return hash;
			};
			const std::string base = hashOf("", L"default");
			const std::string again = hashOf("", L"default again");
			Assert::AreEqual(base, hashOf("", L"default third"), L"and a third time");
			Assert::AreEqual(base, again, L"the capture is deterministic (same settings, same codes)");
			Assert::AreEqual(base, hashOf("source_resolution: native\n", L"native"), L"native: identical");
			Assert::AreEqual(base, hashOf("source_resolution: auto\n", L"auto (short run)"), L"auto before it acts: identical");
			Assert::AreEqual(base, hashOf("deband_vp: off\nlow_bitrate_handling: off\ncnn_residual_stabilise: off\n"
				"ssim_residual_stabilise: off\nssim_bitrate_aware: off\n", L"new settings off"), L"every new setting off: identical");
			Assert::AreNotEqual(base, hashOf("source_resolution: 1080\n", L"re-based 1080"), L"a re-base changes the codes (the check sees changes)");
			SetEnvironmentVariableW(L"VP_TEST_SCREENSHOT_DIR", nullptr);
			g_harnessWidth = savedW;
			g_harnessHeight = savedH;
		}

		TEST_METHOD(OneAxisRebaseKeepsTheAspectRatio)
		{
			// Owner 2026-10-16 ("and that the aspect ratio doesn't change"): a re-base of the
			// height alone (3840x1080: a source upscaled vertically) and a uniform one leave
			// the picture's geometry where it is - the strong edges across and down land
			// at the same output positions as without a re-base.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const int savedW = g_harnessWidth, savedH = g_harnessHeight;
			g_harnessWidth = 3840;
			g_harnessHeight = 2160;
			wchar_t tempPath[MAX_PATH] = {};
			GetTempPathW(MAX_PATH, tempPath);
			const std::wstring shots = std::wstring(tempPath) + L"vp-aspect";
			CreateDirectoryW(shots.c_str(), nullptr);
			SetEnvironmentVariableW(L"VP_TEST_SCREENSHOT_DIR", shots.c_str());
			FramePlan plan;
			plan.captureAtSeconds = 2.5;
			plan.step = [](int) { return FrameStep{ true, 0, 0 }; };
			// The capture's PNG (16-bit, R10 code << 6): the green channel as codes.
			auto load = [](const std::string& json, int& w, int& h) {
				std::vector<int> codes;
				std::string png = json.substr(0, json.size() - 5) + ".png";
				CComPtr<IWICImagingFactory> factory;
				if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return codes;
				CComPtr<IWICBitmapDecoder> decoder;
				const std::wstring wpng(png.begin(), png.end());
				if (FAILED(factory->CreateDecoderFromFilename(wpng.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return codes;
				CComPtr<IWICBitmapFrameDecode> frame;
				decoder->GetFrame(0, &frame);
				CComPtr<IWICFormatConverter> converter;
				factory->CreateFormatConverter(&converter);
				converter->Initialize(frame, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
				UINT uw = 0, uh = 0;
				converter->GetSize(&uw, &uh);
				w = static_cast<int>(uw); h = static_cast<int>(uh);
				std::vector<uint16_t> rgba(static_cast<size_t>(w) * h * 4);
				converter->CopyPixels(nullptr, static_cast<UINT>(w * 8), static_cast<UINT>(rgba.size() * 2), reinterpret_cast<BYTE*>(rgba.data()));
				codes.resize(static_cast<size_t>(w) * h);
				for (size_t i = 0; i < codes.size(); ++i) codes[i] = rgba[i * 4 + 1] >> 6;
				return codes;
			};
			auto capture = [&](const char* extra, int& w, int& h) {
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 3.5, false, std::string(extra) + "[vprenderer.Default]\ndithering: off\ndeband: off\n", false, &plan);
				Assert::IsFalse(r.captureMetadata.empty(), (L"capture written\n" + r.Tail()).c_str());
				for (const std::string& line : r.log)
					if (line.find("Re-base:") != std::string::npos) Logger::WriteMessage(("  " + line + "\n").c_str());
				return load(r.captureMetadata, w, h);
			};
			// The strongest edges of a profile (positions of the largest steps), sorted.
			auto edges = [](const std::vector<double>& profile, int count) {
				std::vector<std::pair<double, int>> steps;
				for (size_t i = 1; i < profile.size(); ++i) steps.push_back({ std::fabs(profile[i] - profile[i - 1]), static_cast<int>(i) });
				std::sort(steps.rbegin(), steps.rend());
				std::vector<int> out;
				for (const auto& st : steps)
				{
					bool close = false;
					for (int e : out) close = close || std::abs(e - st.second) < 8;
					if (!close) out.push_back(st.second);
					if (static_cast<int>(out.size()) == count) break;
				}
				std::sort(out.begin(), out.end());
				return out;
			};
			auto profiles = [](const std::vector<int>& c, int w, int h, std::vector<double>& across, std::vector<double>& down) {
				across.assign(w, 0.0); down.assign(h, 0.0);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x) { across[x] += c[static_cast<size_t>(y) * w + x]; down[y] += c[static_cast<size_t>(y) * w + x]; }
				for (double& v : across) v /= h;
				for (double& v : down) v /= w;
			};
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			int w0 = 0, h0 = 0, w1 = 0, h1 = 0, w2 = 0, h2 = 0;
			const std::vector<int> native = capture("source_resolution: native\n", w0, h0);
			const std::vector<int> oneAxis = capture("source_resolution: 3840x1080\n", w1, h1);
			const std::vector<int> uniform = capture("source_resolution: 1080\n", w2, h2);
			g_harnessWidth = savedW;
			g_harnessHeight = savedH;
			Assert::IsTrue(!native.empty() && !oneAxis.empty() && !uniform.empty(), L"captures read");
			Assert::IsTrue(w0 == w1 && h0 == h1 && w0 == w2 && h0 == h2, L"the same output size");
			std::vector<double> a0, d0, a1, d1, a2, d2;
			profiles(native, w0, h0, a0, d0);
			profiles(oneAxis, w1, h1, a1, d1);
			profiles(uniform, w2, h2, a2, d2);
			const std::vector<int> ea0 = edges(a0, 6), ed0 = edges(d0, 6);
			auto worst = [](const std::vector<int>& ref, const std::vector<int>& got) {
				int w = 0;
				for (int e : ref)
				{
					int best = 1 << 20;
					for (int g : got) best = (std::min)(best, std::abs(g - e));
					w = (std::max)(w, best);
				}
				return w;
			};
			const int oneAcross = worst(ea0, edges(a1, 12)), oneDown = worst(ed0, edges(d1, 12));
			const int uniAcross = worst(ea0, edges(a2, 12)), uniDown = worst(ed0, edges(d2, 12));
			char line[200];
			snprintf(line, sizeof(line), "edges moved (px): one-axis across %d, down %d; uniform across %d, down %d (output %dx%d)\n",
				oneAcross, oneDown, uniAcross, uniDown, w0, h0);
			Logger::WriteMessage(line);
			Assert::IsTrue(oneAcross <= 2 && oneDown <= 2, L"a one-axis re-base keeps the geometry (aspect ratio)");
			Assert::IsTrue(uniAcross <= 2 && uniDown <= 2, L"a uniform re-base keeps the geometry");
		}

		TEST_METHOD(RebaseKeepsTheLayoutWithCropZoomNlsAndBlanking)
		{
			// Owner 2026-10-16 ("black bar detection and the resizing ... touch inside ... edge
			// blanking"): the re-base must not move the picture. Per setup, the output with
			// source_resolution native and 1080: the picture's edges and the strong edges in it
			// at the same output pixels. NLS: the stretched sides' sharpness against the centre's
			// (NLS stretches the source before the upscale, so on the re-based grid).
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const int savedW = g_harnessWidth, savedH = g_harnessHeight, savedWW = g_harnessWindowWidth, savedWH = g_harnessWindowHeight, savedBars = g_harnessBarRows;
			const std::vector<std::string> savedShaders = g_harnessShaderProfiles;
			g_harnessWidth = 3840;
			g_harnessHeight = 2160;
			g_harnessWindowWidth = 3840;
			g_harnessWindowHeight = 2160;
			wchar_t tempPath[MAX_PATH] = {};
			GetTempPathW(MAX_PATH, tempPath);
			const std::wstring shots = std::wstring(tempPath) + L"vp-layout";
			CreateDirectoryW(shots.c_str(), nullptr);
			SetEnvironmentVariableW(L"VP_TEST_SCREENSHOT_DIR", shots.c_str());
			// NLS.glsl from the build's shaders folder (the runner's executable lives elsewhere).
			const std::string here = Here();
			SetEnvironmentVariableA("VP_TEST_SHADER_DIR", here.substr(0, here.size() - 1).c_str());
			FramePlan plan;
			plan.captureAtSeconds = 3.0;
			plan.step = [](int) { return FrameStep{ true, 0, 0 }; };
			auto load = [](const std::string& json, int& w, int& h) {
				std::vector<int> codes;
				std::string png = json.substr(0, json.size() - 5) + ".png";
				CComPtr<IWICImagingFactory> factory;
				if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return codes;
				CComPtr<IWICBitmapDecoder> decoder;
				const std::wstring wpng(png.begin(), png.end());
				if (FAILED(factory->CreateDecoderFromFilename(wpng.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return codes;
				CComPtr<IWICBitmapFrameDecode> frame;
				decoder->GetFrame(0, &frame);
				CComPtr<IWICFormatConverter> converter;
				factory->CreateFormatConverter(&converter);
				converter->Initialize(frame, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
				UINT uw = 0, uh = 0;
				converter->GetSize(&uw, &uh);
				w = static_cast<int>(uw); h = static_cast<int>(uh);
				std::vector<uint16_t> rgba(static_cast<size_t>(w) * h * 4);
				converter->CopyPixels(nullptr, static_cast<UINT>(w * 8), static_cast<UINT>(rgba.size() * 2), reinterpret_cast<BYTE*>(rgba.data()));
				codes.resize(static_cast<size_t>(w) * h);
				for (size_t i = 0; i < codes.size(); ++i) codes[i] = rgba[i * 4 + 1] >> 6;
				return codes;
			};
			auto profiles = [](const std::vector<int>& c, int w, int h, std::vector<double>& across, std::vector<double>& down) {
				across.assign(w, 0.0); down.assign(h, 0.0);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x) { across[x] += c[static_cast<size_t>(y) * w + x]; down[y] += c[static_cast<size_t>(y) * w + x]; }
				for (double& v : across) v /= h;
				for (double& v : down) v /= w;
			};
			// The picture's extent: the first and last line whose mean stands clear of the black matte.
			auto extent = [](const std::vector<double>& profile, int& first, int& last) {
				const double black = (std::min)(profile.front(), profile.back());
				first = -1; last = -1;
				for (int i = 0; i < static_cast<int>(profile.size()); ++i)
					if (profile[static_cast<size_t>(i)] > black + 20.0) { if (first < 0) first = i; last = i; }
			};
			// Sharpness across: the mean absolute step between neighbouring columns, rows in the picture.
			auto sharpness = [](const std::vector<int>& c, int w, int x0, int x1, int y0, int y1) {
				double sum = 0.0;
				long count = 0;
				for (int y = y0; y < y1; y += 4)
					for (int x = x0 + 1; x < x1; ++x) { sum += std::abs(c[static_cast<size_t>(y) * w + x] - c[static_cast<size_t>(y) * w + x - 1]); ++count; }
				return count ? sum / count : 0.0;
			};
			const std::string nlsShader =
				"[shader.nls.standard]\nshader_type: nls\nlabel: Nonlinear Stretch\nstage: pre_resize\norder: 10\nglsl_file: NLS.glsl\nquality: high\n"
				"geometry: classic\nstrength: 1.0\ncurve: 2.0\ntolerance_percent: 5\nmax_stretch_ratio: 1.4\naspect_direction: narrower_only\n"
				"vprenderer_max_crop_percent: 0\nvprenderer_crop_preference: preserve_image\n";
			const std::string blanking = "blank_left: 48\nblank_top: 24\nblank_right: 16\n";
			struct Case { const char* name; std::string viewport; int bars; bool nls; };
			const Case cases[] = {
				{ "scope screen, 16:9 picture, touch inside", "[vprenderer.viewport]\nscreen_aspect: 2.35:1\ncrop_narrower_content_to_fill_screen: true\n" + blanking, 0, false },
				{ "scope screen, 16:9 picture, NLS", "[vprenderer.viewport]\nscreen_aspect: 2.35:1\n" + blanking, 0, true },
				{ "scope screen, letterboxed 2.39 source, auto crop", "[vprenderer.viewport]\nscreen_aspect: 2.35:1\nautomatic_crop: true\n" + blanking, 276, false } };
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			int failures = 0;
			for (const Case& c : cases)
			{
				g_harnessBarRows = c.bars;
				g_harnessShaderProfiles.clear();
				if (c.nls) g_harnessShaderProfiles.push_back("nls.standard");
				int w[2] = {}, h[2] = {};
				std::vector<int> shot[2];
				const char* resolutions[2] = { "source_resolution: native\n", "source_resolution: 1080\n" };
				for (int i = 0; i < 2; ++i)
				{
					const std::string extra = std::string(resolutions[i]) + "[vprenderer.Default]\ndithering: off\ndeband: off\n" + c.viewport + (c.nls ? nlsShader : std::string());
					const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 4.0, false, extra, false, &plan);
					Assert::IsFalse(r.captureMetadata.empty(), (L"capture written\n" + r.Tail()).c_str());
					int shown = 0;
					for (const std::string& line : r.log)
						if ((line.find("Re-base:") != std::string::npos || line.find("NLS") != std::string::npos || line.find("blank") != std::string::npos) && shown < 8)
						{
							Logger::WriteMessage(("  " + line + "\n").c_str());
							++shown;
						}
					if (c.nls)
						Logger::WriteMessage((std::string("  NLS selected: ") + (g_harnessShadersSelected ? "yes" : "no") + "\n").c_str());
					shot[i] = load(r.captureMetadata, w[i], h[i]);
				}
				Assert::IsTrue(!shot[0].empty() && !shot[1].empty() && w[0] == w[1] && h[0] == h[1], L"two captures of one size");
				std::vector<double> a[2], d[2];
				int x0[2], x1[2], y0[2], y1[2];
				for (int i = 0; i < 2; ++i)
				{
					profiles(shot[i], w[i], h[i], a[i], d[i]);
					extent(a[i], x0[i], x1[i]);
					extent(d[i], y0[i], y1[i]);
				}
				const int boundMove = (std::max)((std::max)(std::abs(x0[0] - x0[1]), std::abs(x1[0] - x1[1])), (std::max)(std::abs(y0[0] - y0[1]), std::abs(y1[0] - y1[1])));
				// The shift that best matches the two profiles inside the picture (the stripes are
				// periodic, so strongest-edge matching is ambiguous); 0 = not moved.
				auto bestShift = [](const std::vector<double>& p0, const std::vector<double>& p1, int lo, int hi) {
					int best = 0;
					double bestError = 1e300;
					for (int sft = -12; sft <= 12; ++sft)
					{
						double e = 0.0;
						for (int i = lo + 16; i < hi - 16; ++i) e += std::fabs(p0[static_cast<size_t>(i)] - p1[static_cast<size_t>(i + sft)]);
						if (e < bestError) { bestError = e; best = sft; }
					}
					return best;
				};
				const int edgeAcross = x0[0] >= 0 ? std::abs(bestShift(a[0], a[1], x0[0], x1[0])) : 99;
				const int edgeDown = y0[0] >= 0 ? std::abs(bestShift(d[0], d[1], y0[0], y1[0])) : 99;
				char line[320];
				snprintf(line, sizeof(line), "%s (output %dx%d): picture x %d-%d y %d-%d native, x %d-%d y %d-%d re-based; bounds moved %d px, edges moved across %d down %d\n",
					c.name, w[0], h[0], x0[0], x1[0], y0[0], y1[0], x0[1], x1[1], y0[1], y1[1], boundMove, edgeAcross, edgeDown);
				Logger::WriteMessage(line);
				if (x0[0] < 0 || boundMove > 1 || edgeAcross > 2 || edgeDown > 2) ++failures;
				// NLS really ran: the 16:9 picture stretched across most of the scope screen
				// (the linear fallback pillarboxes it at 16:9).
				if (c.nls && (x0[0] < 0 || (x1[0] - x0[0]) < (y1[0] - y0[0]) * 2.0))
				{
					Logger::WriteMessage("  NLS did not stretch the picture (the fallback ran)\n");
					++failures;
				}
				if (c.nls && x0[0] >= 0)
				{
					const int pw = x1[0] - x0[0], my0 = y0[0] + (y1[0] - y0[0]) / 4, my1 = y1[0] - (y1[0] - y0[0]) / 4;
					const int side = pw / 8, centre0 = x0[0] + pw * 2 / 5, centre1 = x0[0] + pw * 3 / 5;
					double ratioSide = 0.0, ratioCentre = 0.0;
					const double sideN = sharpness(shot[0], w[0], x0[0] + 4, x0[0] + side, my0, my1) + sharpness(shot[0], w[0], x1[0] - side, x1[0] - 4, my0, my1);
					const double sideR = sharpness(shot[1], w[1], x0[0] + 4, x0[0] + side, my0, my1) + sharpness(shot[1], w[1], x1[0] - side, x1[0] - 4, my0, my1);
					const double centreN = sharpness(shot[0], w[0], centre0, centre1, my0, my1), centreR = sharpness(shot[1], w[1], centre0, centre1, my0, my1);
					ratioSide = sideN > 0 ? sideR / sideN : 0.0;
					ratioCentre = centreN > 0 ? centreR / centreN : 0.0;
					snprintf(line, sizeof(line), "  NLS sharpness re-based / native: sides %.3f, centre %.3f (sides %.1f%% of the centre's)\n",
						ratioSide, ratioCentre, ratioCentre > 0 ? 100.0 * ratioSide / ratioCentre : 0.0);
					Logger::WriteMessage(line);
				}
			}
			g_harnessWidth = savedW;
			g_harnessHeight = savedH;
			g_harnessWindowWidth = savedWW;
			g_harnessWindowHeight = savedWH;
			g_harnessBarRows = savedBars;
			g_harnessShaderProfiles = savedShaders;
			SetEnvironmentVariableA("VP_TEST_SHADER_DIR", nullptr);
			Assert::AreEqual(0, failures, L"the re-base keeps the layout in every setup");
		}

		TEST_METHOD(TemporalDitherChoiceControlsTheSigmaDelta)
		{
			// Owner 2026-10-16: temporal_dither sigma_delta loads VP's sigma-delta whatever the shader list
			// says; libplacebo (or off) leaves it out even when the list selects it.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const std::string here = Here();
			SetEnvironmentVariableA("VP_TEST_SHADER_DIR", here.substr(0, here.size() - 1).c_str());
			const std::vector<std::string> saved = g_harnessShaderProfiles;
			auto customCount = [](const RunResult& r) {
				for (auto it = r.log.rbegin(); it != r.log.rend(); ++it)
				{
					const size_t at = it->find("custom hook(s)");
					if (at != std::string::npos)
					{
						const size_t open = it->rfind(", ", at);
						return std::atoi(it->c_str() + (open == std::string::npos ? 0 : open + 2));
					}
				}
				return -1;
			};
			auto logged = [](const RunResult& r, const char* text) { return r.Logged(text); };
			// No shader list selection, temporal_dither: sigma_delta.
			g_harnessShaderProfiles.clear();
			const RunResult added = Run(VideoFrameEncoding::V210, "gpu", 3.0, true, "temporal_dither: sigma_delta\n");
			// The list selects the sigma-delta, temporal_dither: libplacebo.
			const std::string shaders =
				"[shader.standard]\ntype: multi\nlabel: Off\n"
				"[shader.standard.sigma_delta]\nshader_type: custom\nlabel: Temporal sigma-delta\nstage: post_resize\norder: 90\nglsl_file: Temporal sigma-delta.glsl\n";
			g_harnessShaderProfiles = { "standard.sigma_delta" };
			const RunResult listed = Run(VideoFrameEncoding::V210, "gpu", 3.0, true, std::string("temporal_dither: auto\n") + shaders);
			const RunResult removed = Run(VideoFrameEncoding::V210, "gpu", 3.0, true, std::string("temporal_dither: libplacebo\n") + shaders);
			g_harnessShaderProfiles = saved;
			SetEnvironmentVariableA("VP_TEST_SHADER_DIR", nullptr);
			char line[200];
			snprintf(line, sizeof(line), "custom hooks: sigma_delta with no list %d; list + auto %d; list + libplacebo %d\n",
				customCount(added), customCount(listed), customCount(removed));
			Logger::WriteMessage(line);
			Assert::IsTrue(logged(added, "Temporal dither: sigma_delta"), (L"sigma_delta logged\n" + added.Tail()).c_str());
			Assert::IsTrue(logged(added, "custom hook"), (L"sigma_delta: the sigma-delta loaded with no shader list\n" + added.Tail()).c_str());
			Assert::IsTrue(logged(removed, "Temporal dither: libplacebo"), (L"libplacebo logged\n" + removed.Tail()).c_str());
			Assert::IsFalse(logged(removed, "custom hook"), (L"libplacebo: the listed sigma-delta left out\n" + removed.Tail()).c_str());
			Assert::IsTrue(logged(listed, "custom hook"), (L"auto: the listed sigma-delta loads as before\n" + listed.Tail()).c_str());
		}

		TEST_METHOD(MotionAheadEngagesAtFourQueuedAheadAndFollowsTheQueue)
		{
			// Owner 2026-10-16: the look-ahead engages once 4 frames are queued ahead; the queue
			// target that guarantees it (5 or 6) is measured here, then a reset disengages it and
			// it re-engages at 4. NVIDIA (VP_TEST_ADAPTER=NVIDIA).
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			auto lines = [](const RunResult& r, const char* text) {
				std::string out;
				for (const std::string& line : r.log)
					if (line.find(text) != std::string::npos) out += "  " + line + "\n";
				return out;
			};
			bool engine = true;
			for (const size_t target : { size_t{ 5 }, size_t{ 6 } })
			{
				g_harnessQueueTarget = target;
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 8.0, true, "dejudder_strength: 0.25\n");
				g_harnessQueueTarget = 0;
				const std::string source = lines(r, "De-judder motion:");
				if (source.find("optical-flow engine") == std::string::npos) { engine = false; Logger::WriteMessage(("no engine: " + source).c_str()); break; }
				Logger::WriteMessage(("queue target " + std::to_string(target) + ":\n" + lines(r, "Motion ahead:")).c_str());
				Assert::IsTrue(r.Logged("Motion ahead: engaged"), (L"target " + std::to_wstring(target) + L": the look-ahead engaged\n" + r.Tail()).c_str());
			}
			if (!engine) return;
			// A reset (resync) mid-run: disengaged, then engaged again at 4.
			FramePlan plan;
			plan.rateNumerator = 24000;
			plan.resetAtSeconds = 3.0;
			plan.step = [](int) { return FrameStep{ true, 0, 0 }; };
			g_harnessQueueTarget = 5;
			const RunResult reset = Run(VideoFrameEncoding::V210, "gpu", 7.0, true, "dejudder_strength: 0.25\n", false, &plan);
			g_harnessQueueTarget = 0;
			Logger::WriteMessage(("reset at 3 s:\n" + lines(reset, "Motion ahead: engaged") + lines(reset, "waiting for 4")).c_str());
			int engaged = 0;
			for (const std::string& line : reset.log) engaged += line.find("Motion ahead: engaged") != std::string::npos ? 1 : 0;
			Assert::IsTrue(engaged >= 2 && reset.Logged("waiting for 4"), (L"a reset disengages and it re-engages at 4\n" + reset.Tail()).c_str());
		}

		TEST_METHOD(LowLatencyQueueFromTheFirstFrame)
		{
			// The bug found 2026-10-16: the low-latency flag was lost on a Build. With the queue profile
			// applied before Build (as the app does), the renderer starts in the low-latency path: the
			// de-judder off and the producer never started.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			g_harnessApplySnapshot = true;
			const RunResult low = Run(VideoFrameEncoding::V210, "gpu", 4.0, true,
				"dejudder_strength: 0.25\n[queue.Low_Latency]\nqueue_size: 4\ntarget_frames: 1\n");
			const RunResult normal = Run(VideoFrameEncoding::V210, "gpu", 4.0, true,
				"dejudder_strength: 0.25\n[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n");
			g_harnessApplySnapshot = false;
			for (const std::string& line : low.log)
				if (line.find("De-judder:") != std::string::npos || line.find("Motion ahead") != std::string::npos)
					Logger::WriteMessage(("  low: " + line + "\n").c_str());
			Assert::IsTrue(low.Logged("low-latency queue"), (L"the de-judder off for the low-latency queue\n" + low.Tail()).c_str());
			Assert::IsFalse(low.Logged("Motion ahead: engaged"), (L"no look-ahead in low latency\n" + low.Tail()).c_str());
			Assert::IsFalse(normal.Logged("low-latency queue"), (L"the Normal queue is not low latency\n" + normal.Tail()).c_str());
		}

		TEST_METHOD(LowLatencyPathPropertyAndAutoEarlyUpload)
		{
			// Owner 2026-10-16: low_latency_path on any queue profile takes the low-latency path; with
			// capture_upload auto (the default) early upload opens there and not on the regular path.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			g_harnessApplySnapshot = true;
			const RunResult game = Run(VideoFrameEncoding::V210, "gpu", 4.0, true,
				"dejudder_strength: 0.25\n[queue.Game]\nqueue_size: 4\ntarget_frames: 1\nlow_latency_path: on\n");
			const RunResult normal = Run(VideoFrameEncoding::V210, "gpu", 4.0, true,
				"dejudder_strength: 0.25\n[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n");
			g_harnessApplySnapshot = false;
			Assert::IsTrue(game.Logged("low-latency queue"), (L"low_latency_path on: the low-latency path\n" + game.Tail()).c_str());
			Assert::IsTrue(game.Logged("Early upload: on"), (L"capture_upload auto: early upload in low latency\n" + game.Tail()).c_str());
			Assert::IsFalse(normal.Logged("Early upload: on"), (L"auto: no early upload on the regular path\n" + normal.Tail()).c_str());
			Assert::IsFalse(normal.Logged("low-latency queue"), (L"the Normal queue is regular\n" + normal.Tail()).c_str());
		}

		TEST_METHOD(RebaseDebandAndCnnBlendKeepTheRecoveredDetail)
		{
			// Owner 2026-10-16: the adaptive scaling must not destroy the detail recovery (the re-base)
			// or the debanding. A 1080p master with fine texture and a sky banded in 8-bit steps; the
			// reference is the master fed at 1080 through the chain; the test feeds the master upscaled
			// 2x by a player (Lanczos-3) at 4K with source_resolution 1080. Per variant (plain, + VP
			// deband, + CNN upscaler with its blend) the 4K outputs are compared (picture and sky), and
			// the sky's band edges counted.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const int w = 1920, h = 1080, sky = h / 4;
			std::vector<double> master(static_cast<size_t>(w) * h);
			{
				std::mt19937 rng(5);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				struct Wave { double fx, fy, phase, amp; };
				std::vector<Wave> waves;
				for (int i = 0; i < 60; ++i)
				{
					const double f = 0.01 * std::pow(0.45 / 0.01, u(rng)), a = u(rng) * 3.14159265;
					waves.push_back({ f * std::cos(a), f * std::sin(a), u(rng) * 6.2831853, 0.02 / std::sqrt(f) });
				}
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						double v;
						if (y < sky)
							v = std::round((0.35 + 0.25 * y / static_cast<double>(sky)) * 255.0) / 255.0;   // 8-bit steps
						else
						{
							v = 0.45;
							for (const Wave& wv : waves) v += wv.amp * std::cos(6.2831853 * (wv.fx * x + wv.fy * y) + wv.phase);
						}
						master[static_cast<size_t>(y) * w + x] = (std::min)(0.95, (std::max)(0.05, v));
					}
			}
			auto toCodes = [](const std::vector<double>& v) {
				std::vector<uint16_t> c(v.size());
				for (size_t i = 0; i < v.size(); ++i) c[i] = static_cast<uint16_t>(std::lround(64.0 + v[i] * 876.0));   // limited range
				return c;
			};
			// The player's 2x Lanczos-3 (separable).
			std::vector<double> up(static_cast<size_t>(w) * 2 * h * 2);
			{
				auto lanczos = [](double x) { x = std::fabs(x); if (x < 1e-9) return 1.0; if (x >= 3.0) return 0.0;
					const double px = 3.14159265358979 * x; return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px); };
				std::vector<double> rows(static_cast<size_t>(w) * 2 * h);
				auto line = [&](const double* src, int n, int stride, double* dst, int dstStride) {
					for (int o = 0; o < 2 * n; ++o)
					{
						const double centre = (o + 0.5) / 2.0 - 0.5;
						double sum = 0.0, wsum = 0.0;
						for (int k = static_cast<int>(std::floor(centre)) - 2; k <= static_cast<int>(std::floor(centre)) + 3; ++k)
						{
							const int kk = (std::min)(n - 1, (std::max)(0, k));
							const double wt = lanczos(centre - k);
							sum += wt * src[static_cast<size_t>(kk) * stride];
							wsum += wt;
						}
						dst[static_cast<size_t>(o) * dstStride] = sum / wsum;
					}
				};
				for (int y = 0; y < h; ++y) line(&master[static_cast<size_t>(y) * w], w, 1, &rows[static_cast<size_t>(y) * 2 * w], 1);
				for (int x = 0; x < 2 * w; ++x) line(&rows[x], h, 2 * w, &up[x], 2 * w);
			}
			for (double& v : up) v = (std::min)(1.0, (std::max)(0.0, v));
			const std::vector<uint16_t> masterCodes = toCodes(master), upCodes = toCodes(up);
			const int savedW = g_harnessWidth, savedH = g_harnessHeight, savedWW = g_harnessWindowWidth, savedWH = g_harnessWindowHeight;
			const std::vector<std::string> savedShaders = g_harnessShaderProfiles;
			g_harnessWindowWidth = 3840;
			g_harnessWindowHeight = 2160;
			wchar_t tempPath[MAX_PATH] = {};
			GetTempPathW(MAX_PATH, tempPath);
			const std::wstring shots = std::wstring(tempPath) + L"vp-chain";
			CreateDirectoryW(shots.c_str(), nullptr);
			SetEnvironmentVariableW(L"VP_TEST_SCREENSHOT_DIR", shots.c_str());
			const std::string here = Here();
			SetEnvironmentVariableA("VP_TEST_SHADER_DIR", here.substr(0, here.size() - 1).c_str());
			FramePlan plan;
			plan.captureAtSeconds = 3.0;
			plan.step = [](int) { return FrameStep{ true, 0, 0 }; };
			auto load = [](const std::string& json, int& lw, int& lh) {
				std::vector<int> codes;
				std::string png = json.substr(0, json.size() - 5) + ".png";
				CComPtr<IWICImagingFactory> factory;
				if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return codes;
				CComPtr<IWICBitmapDecoder> decoder;
				const std::wstring wpng(png.begin(), png.end());
				if (FAILED(factory->CreateDecoderFromFilename(wpng.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return codes;
				CComPtr<IWICBitmapFrameDecode> frame;
				decoder->GetFrame(0, &frame);
				CComPtr<IWICFormatConverter> converter;
				factory->CreateFormatConverter(&converter);
				converter->Initialize(frame, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
				UINT uw = 0, uh = 0;
				converter->GetSize(&uw, &uh);
				lw = static_cast<int>(uw); lh = static_cast<int>(uh);
				std::vector<uint16_t> rgba(static_cast<size_t>(lw) * lh * 4);
				converter->CopyPixels(nullptr, static_cast<UINT>(lw * 8), static_cast<UINT>(rgba.size() * 2), reinterpret_cast<BYTE*>(rgba.data()));
				codes.resize(static_cast<size_t>(lw) * lh);
				for (size_t i = 0; i < codes.size(); ++i) codes[i] = rgba[i * 4 + 1] >> 6;
				return codes;
			};
			const std::string cnnShader =
				"[shader.standard]\ntype: multi\nlabel: Off\n"
				"[shader.standard.cnn]\nshader_type: custom\nlabel: FSRCNNX\nstage: pre_resize\norder: 10\nglsl_file: FSRCNNX_x2_16-0-4-1.glsl\n";
			struct Variant { const char* name; std::string extra; bool cnn; };
			const Variant variants[] = {
				{ "plain", "deband_vp: off\nadaptive_upscale: off\n", false },
				{ "VP deband", "deband_vp: on\nadaptive_upscale: off\n", false },
				{ "VP deband + CNN + blend", "deband_vp: on\nadaptive_upscale: on\n", true } };
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			auto capture = [&](bool source4k, const Variant& v, int& cw, int& ch) {
				g_harnessWidth = source4k ? 2 * w : w;
				g_harnessHeight = source4k ? 2 * h : h;
				g_harnessLumaPicture = source4k ? upCodes : masterCodes;
				g_harnessShaderProfiles.clear();
				if (v.cnn) g_harnessShaderProfiles.push_back("standard.cnn");
				const std::string extra = std::string(source4k ? "source_resolution: 1080\n" : "source_resolution: native\n") +
					"[vprenderer.Default]\ndithering: off\ndeband: off\n" + v.extra + (v.cnn ? cnnShader : std::string());
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 4.0, false, extra, false, &plan);
				for (const std::string& line : r.log)
					if (line.find("Re-base:") != std::string::npos || line.find("Re-base kernel") != std::string::npos ||
						line.find("VP deband") != std::string::npos || line.find("Adaptive upscale") != std::string::npos ||
						line.find("rejected") != std::string::npos)
						Logger::WriteMessage(("    " + line + "\n").c_str());
				Assert::IsFalse(r.captureMetadata.empty(), (L"capture written\n" + r.Tail()).c_str());
				return load(r.captureMetadata, cw, ch);
			};
			for (const Variant& v : variants)
			{
				int aw = 0, ah = 0, bw = 0, bh = 0;
				const std::vector<int> ref = capture(false, v, aw, ah);
				const std::vector<int> got = capture(true, v, bw, bh);
				Assert::IsTrue(!ref.empty() && aw == bw && ah == bh, L"two captures of one size");
				const int skyOut = ah / 4;
				double ePic = 0.0, eSky = 0.0;
				long nPic = 0, nSky = 0;
				auto bandEdges = [&](const std::vector<int>& c) {
					long edges = 0, count = 0;
					for (int y = 8; y < skyOut - 8; ++y)
						for (int x = 16; x < aw - 16; x += 4)
						{
							edges += std::abs(c[static_cast<size_t>(y) * aw + x] - c[static_cast<size_t>(y - 1) * aw + x]) >= 3 ? 1 : 0;
							++count;
						}
					return count ? 100.0 * edges / count : 0.0;
				};
				for (int y = 8; y < ah - 8; ++y)
					for (int x = 16; x < aw - 16; x += 2)
					{
						const double e = static_cast<double>(got[static_cast<size_t>(y) * aw + x] - ref[static_cast<size_t>(y) * aw + x]);
						if (y < skyOut - 8) { eSky += e * e; ++nSky; }
						else if (y > skyOut + 24) { ePic += e * e; ++nPic; }
					}
				char line[300];
				snprintf(line, sizeof(line), "%-26s re-based vs the 1080 master through the same chain: picture %.2f codes RMS, sky %.2f; sky band edges: reference %.2f%%, re-based %.2f%%\n",
					v.name, std::sqrt(ePic / (std::max)(1L, nPic)), std::sqrt(eSky / (std::max)(1L, nSky)), bandEdges(ref), bandEdges(got));
				Logger::WriteMessage(line);
			}
			g_harnessLumaPicture.clear();
			g_harnessWidth = savedW; g_harnessHeight = savedH;
			g_harnessWindowWidth = savedWW; g_harnessWindowHeight = savedWH;
			g_harnessShaderProfiles = savedShaders;
			SetEnvironmentVariableA("VP_TEST_SHADER_DIR", nullptr);
		}

		TEST_METHOD(VpDebanderRunsThroughTheRenderer)
		{
			// Stage 12 §2b/§2d (owner 2026-10-15): VP's debander through the
			// renderer - on its own, with the de-judder (vectors held), and on a
			// re-based source; never a failure; off is the default.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const int savedW = g_harnessWidth, savedH = g_harnessHeight;
			g_harnessWidth = 3840;
			g_harnessHeight = 2160;
			struct Case { const char* extra; bool film; const wchar_t* name; };
			for (const Case& c : { Case{ "deband_vp: on\n", false, L"on" },
				Case{ "deband_vp: on\ndejudder_strength: 0.25\n", true, L"on with the de-judder" },
				Case{ "deband_vp: on\nsource_resolution: 1080\n", false, L"on, re-based" },
				Case{ "deband_vp: auto\nlow_bitrate_handling: on\n", false, L"auto, low-bitrate on" } })
			{
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 3.0, c.film, c.extra);
				Assert::IsTrue(r.presented, (std::wstring(c.name) + L": frames presented\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("VP deband: threshold"), (std::wstring(c.name) + L": the debander ran\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("VP deband failed"), (std::wstring(c.name) + L": no failure\n" + r.Tail()).c_str());
			}
			{
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 2.0);
				Assert::IsFalse(r.Logged("VP deband: threshold"), (L"default: off\n" + r.Tail()).c_str());
			}
			g_harnessWidth = savedW;
			g_harnessHeight = savedH;
		}

		TEST_METHOD(MotionEnginesCoexistWithVpShaders)
		{
			// v1.4.02 merge (owner 2026-10-15): libplacebo's smooth motion (Bill's)
			// and VP's motion handling are one or the other (motion_engine), and
			// VP's shaders keep working with either: the sigma-delta dither, the
			// SSIM downscaler with the adaptive blend, the de-judder settings. Off
			// the screen: the harness gives the renderer the monitor's refresh rate
			// as the app does.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const std::string shaders =
				"[shader.standard]\ntype: multi\nlabel: Off\n"
				"[shader.standard.ssim_downscaler]\nshader_type: custom\nlabel: SSimDownscaler\nstage: post_resize\norder: 20\nglsl_file: SSimDownscaler.glsl\n"
				"[shader.standard.sigma_delta]\nshader_type: custom\nlabel: Temporal sigma-delta\nstage: post_resize\norder: 90\nglsl_file: Temporal sigma-delta.glsl\n";
			g_harnessShaderProfiles = { "standard.ssim_downscaler", "standard.sigma_delta" };
			struct Case { const char* name; bool film; const char* profile; const char* motion; bool smooth; };
			const Case cases[] = {
				{ "23.976p, smooth motion selected", true, "dejudder_strength: 0.25\nadaptive_downscale: on\n",
					"motion_engine: smooth_motion\nsmooth_motion: always\n", true },
				{ "23.976p, VP selected", true, "dejudder_strength: 0.25\nadaptive_downscale: on\n",
					"motion_engine: vp\nsmooth_motion: always\n", false },
				{ "23.976p, auto with only smooth motion set", true, "adaptive_downscale: on\n",
					"smooth_motion: always\n", true },
				{ "59.94p, smooth motion selected", false, "adaptive_downscale: on\n",
					"motion_engine: smooth_motion\nsmooth_motion: always\n", true } };
			int failures = 0;
			for (const Case& c : cases)
			{
				const std::string extra = std::string(c.profile) + "[vprenderer.motion]\n" + c.motion + shaders;
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 5.0, c.film, extra);
				const std::wstring name(c.name, c.name + strlen(c.name));
				Logger::WriteMessage((std::string(c.name) + ": shaders selected " + (g_harnessShadersSelected ? "yes" : "no")).c_str());
				for (const std::string& line : r.log)
					if (line.find("Motion engine:") != std::string::npos || line.find("Motion: smooth motion mixing") != std::string::npos ||
						line.find("De-judder: smooth motion") != std::string::npos || line.find("Sigma-delta vectors") != std::string::npos)
						Logger::WriteMessage(("   " + line).c_str());
				Assert::IsTrue(g_harnessShadersSelected, (name + L": the shaders were selected\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (name + L": frames presented\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("extra render failed") || r.Logged("extra present failed"),
					(name + L": no render failure\n" + r.Tail()).c_str());
				if (c.smooth)
				{
					Assert::IsTrue(r.Logged("Motion engine: libplacebo smooth motion"), (name + L": smooth motion is the engine\n" + r.Tail()).c_str());
					Assert::IsTrue(r.Logged("Motion: smooth motion mixing on"), (name + L": frames were mixed\n" + r.Tail()).c_str());
					Assert::IsFalse(r.Logged("De-judder: motion estimated"), (name + L": no de-judder estimate (no vectors)\n" + r.Tail()).c_str());
					Assert::IsFalse(r.Logged("De-judder: active"), (name + L": the de-judder never active\n" + r.Tail()).c_str());
				}
				else
				{
					Assert::IsTrue(r.Logged("Motion engine: VP"), (name + L": VP is the engine\n" + r.Tail()).c_str());
					Assert::IsFalse(r.Logged("Motion: smooth motion mixing on"), (name + L": no mixing\n" + r.Tail()).c_str());
					Assert::IsTrue(r.Logged("De-judder: motion estimated"), (name + L": the de-judder ran\n" + r.Tail()).c_str());
				}
				// The crash state's LED line (no LED drive configured here).
				Assert::AreEqual(std::string("off"), r.ledStatus, (name + L": the LED drive status line").c_str());
				failures += r.presented ? 0 : 1;
			}
			g_harnessShaderProfiles.clear();
			Assert::AreEqual(0, failures);
		}

		TEST_METHOD(EarlyUploadRunsThroughTheRenderer)
		{
			// capture_upload: early (owner 2026-10-15, experimental): frames uploaded
			// on the conversion thread are read by the render (V210 and 12-bit RGB,
			// both capture paths' raw route), and the 24p de-judder's next frames
			// come from the same early textures. Each run logs how many frames were
			// read early at teardown.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			struct Case { VideoFrameEncoding encoding; bool film; const char* extra; const wchar_t* name; };
			for (const Case& c : { Case{ VideoFrameEncoding::V210, false, "capture_upload: early\n", L"V210 early" },
				Case{ VideoFrameEncoding::R12B, false, "capture_upload: early\n", L"R12B early" },
				Case{ VideoFrameEncoding::V210, true, "capture_upload: early\ndejudder_strength: 0.25\n", L"film V210 early" },
				Case{ VideoFrameEncoding::R12B, true, "capture_upload: early\ndejudder_strength: 0.25\n", L"film R12B early" } })
			{
				const RunResult r = Run(c.encoding, "gpu", c.film ? 5.0 : 3.0, c.film, c.extra);
				Assert::IsTrue(r.Logged("Early upload: on"), (std::wstring(c.name) + L": the pool opened\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Early upload: not available"), (std::wstring(c.name) + L": available\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("GPU unpack failed"), (std::wstring(c.name) + L": no unpack failure\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (std::wstring(c.name) + L": frames presented\n" + r.Tail()).c_str());
				if (c.film)
					Assert::IsTrue(r.Logged("De-judder: motion estimated"), (std::wstring(c.name) + L": the de-judder estimated\n" + r.Tail()).c_str());
				// The CPU analysis keeps reading the captured frame (the early upload
				// copies it to the GPU; the original stays the CPU's).
				Assert::IsFalse(r.Logged("Alpha analysis unavailable"), (std::wstring(c.name) + L": the CPU analysis ran\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Alpha raw-frame analysis: the CPU reads the captured frame"),
					(std::wstring(c.name) + L": the CPU analysis read the captured frame\n" + r.Tail()).c_str());
				bool readEarly = false;
				for (const std::string& line : r.log)
					if (line.find("Capture raw route") != std::string::npos)
						Logger::WriteMessage(line.c_str());
					else if (line.find("Early upload: closed") != std::string::npos)
					{
						Logger::WriteMessage(line.c_str());
						readEarly = line.find("(0 frames read early") == std::string::npos;
					}
				Assert::IsTrue(readEarly, (std::wstring(c.name) + L": frames were read early\n" + r.Tail()).c_str());
			}
			// Off (the default): the pool never opens.
			{
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 2.0);
				Assert::IsFalse(r.Logged("Early upload: on"), (L"default: no early upload\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Alpha analysis unavailable"), (L"default: the CPU analysis ran\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Alpha raw-frame analysis: the CPU reads the captured frame"),
					(L"default: the CPU analysis read the captured frame\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (L"default: frames presented\n" + r.Tail()).c_str());
			}
		}

		TEST_METHOD(EarlyUploadAndTheOpticalFlowProducerTogether)
		{
			// Owner 2026-10-16: the two second D3D11 devices - early upload (capture_upload: early) and the
			// optical-flow producer - must play nicely: 4K 24p film with the de-judder, each combination,
			// then the low-latency path at 60 Hz with early upload on and off. Reports the presentation
			// timing; asserts no failures, frames read early, the producer engaged and never stopped.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const int savedW = g_harnessWidth, savedH = g_harnessHeight;
			g_harnessWidth = 3840;
			g_harnessHeight = 2160;
			auto report = [](const std::string& name, const RunResult& r) {
				std::string out = name + ":\n  presentation: " + r.presentationStatus + "\n";
				for (const std::string& line : r.log)
					if (line.find("Early upload: closed") != std::string::npos || line.find("Motion ahead: engaged") != std::string::npos ||
						line.find("pairs taken") != std::string::npos || line.find("producer stopped") != std::string::npos ||
						line.find("producer did not start") != std::string::npos)
						out += "  " + line + "\n";
				Logger::WriteMessage(out.c_str());
			};
			int failures = 0;
			for (const bool early : { false, true })
				for (const bool ahead : { false, true })
				{
					std::string extra = "dejudder_strength: 0.25\n";
					extra += early ? "capture_upload: early\n" : "capture_upload: render\n";
					extra += ahead ? "motion_ahead_frames: 4\n" : "motion_ahead_frames: 0\n";
					g_harnessQueueTarget = 5;
					const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 8.0, true, extra);
					g_harnessQueueTarget = 0;
					const std::string name = std::string("early upload ") + (early ? "on" : "off") + ", look-ahead " + (ahead ? "on" : "off");
					report(name, r);
					bool readEarly = !early;
					for (const std::string& line : r.log)
						if (line.find("Early upload: closed") != std::string::npos)
							readEarly = line.find("(0 frames read early") == std::string::npos;
					const bool engineHere = r.Logged("optical-flow engine");
					const bool ok = r.presented && !r.Logged("GPU unpack failed") && !r.Logged("Early upload: not available") &&
						readEarly && !r.Logged("producer stopped") && r.Logged("De-judder: motion estimated") &&
						(!ahead || !engineHere || r.Logged("Motion ahead: engaged"));
					if (!ok) { ++failures; Logger::WriteMessage(("  FAILED: " + name + "\n" + std::string()).c_str()); }
				}
			// The low-latency path at 60 Hz (the queue profile applied before Build), early upload off / on.
			FramePlan plan60;
			plan60.rateNumerator = 60000;
			plan60.step = [](int) { return FrameStep{ true, 0, 0 }; };
			g_harnessApplySnapshot = true;
			for (const bool early : { false, true })
			{
				const std::string extra = std::string(early ? "capture_upload: early\n" : "capture_upload: render\n") +
					"[queue.Low_Latency]\nqueue_size: 4\ntarget_frames: 1\n";
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 6.0, false, extra, false, &plan60);
				const std::string name = std::string("low latency 60 Hz, early upload ") + (early ? "on" : "off");
				report(name, r);
				bool readEarly = !early;
				for (const std::string& line : r.log)
					if (line.find("Early upload: closed") != std::string::npos)
						readEarly = line.find("(0 frames read early") == std::string::npos;
				if (!r.presented || r.Logged("GPU unpack failed") || !readEarly || r.Logged("Motion ahead: engaged"))
				{
					++failures;
					Logger::WriteMessage(("  FAILED: " + name + "\n").c_str());
				}
			}
			g_harnessApplySnapshot = false;
			g_harnessWidth = savedW;
			g_harnessHeight = savedH;
			Assert::AreEqual(0, failures, L"early upload and the optical-flow producer work in every combination and in low latency");
		}

		TEST_METHOD(NativeRgbIngestRunsThroughTheRenderer)
		{
			// Roadmap item 2 (owner 2026-10-15): DeckLink 12-bit RGB (both byte
			// orders) and r210 reach the renderer as native RGB, unpacked on the
			// GPU, on the standard capture path as well as the gpu one; with
			// native_rgb_ingest: off the CPU conversion as before.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const std::string failed = "Native RGB ingest: the GPU unpack failed";
			struct Case { VideoFrameEncoding encoding; const char* path; const char* line; };
			for (const Case& c : { Case{ VideoFrameEncoding::R12B, "standard", "Native RGB ingest: R12B unpacked on the GPU" },
				Case{ VideoFrameEncoding::R12L, "gpu", "Native RGB ingest: R12L unpacked on the GPU" },
				Case{ VideoFrameEncoding::R210, "standard", "Native RGB ingest: r210 unpacked on the GPU" },
				Case{ VideoFrameEncoding::R10b, "standard", "Native RGB ingest: R10b unpacked on the GPU" },
				Case{ VideoFrameEncoding::R10l, "gpu", "Native RGB ingest: R10l unpacked on the GPU" },
				Case{ VideoFrameEncoding::ARGB_8BIT, "standard", "Native RGB ingest: ARGB unpacked on the GPU" },
				Case{ VideoFrameEncoding::BGRA_8BIT, "standard", "Native RGB ingest: BGRA unpacked on the GPU" } })
			{
				const RunResult r = Run(c.encoding, c.path, 2.0);
				const std::wstring name = std::wstring(c.line, c.line + strlen(c.line));
				Assert::IsTrue(r.created, (name + L": renderer not created\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged(c.line), (name + L": the native path ran\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged(failed), (name + L": no fallback\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Alpha conversion failed"), (name + L": no conversion failure\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (name + L": frames presented\n" + r.Tail()).c_str());
			}
			// native_rgb_ingest: off: 8-bit ARGB (e.g. a receiver's 8-bit RGB
			// 4:4:4) and BGRA on libplacebo's native upload (Bill's path, unchanged).
			struct Existing { VideoFrameEncoding encoding; const char* label; };
			std::string existingFailures;
			for (const Existing& c : { Existing{ VideoFrameEncoding::ARGB_8BIT, "ingress=Native ARGB -> RGB" },
				Existing{ VideoFrameEncoding::BGRA_8BIT, "ingress=Native BGRA -> RGB" } })
			{
				// Every format tried; the failures listed together.
				const RunResult r = Run(c.encoding, "standard", 2.0, false, "native_rgb_ingest: off\n");
				const bool ok = r.presented && r.Logged(c.label) && !r.Logged("Native RGB ingest:") && !r.Logged("Alpha conversion failed") &&
					!r.Logged("Failed picking any compatible texture format");
				Logger::WriteMessage((std::string("existing native RGB ") + c.label + ": " + (ok ? "presented" : "NOT presented") +
					" (presented " + std::to_string(r.presented) + ", native label " + std::to_string(r.Logged(c.label)) +
					", new unpack " + std::to_string(r.Logged("Native RGB ingest:")) + ")\n").c_str());
				if (!ok)
					for (const std::string& line : r.log)
						if (line.find("RGB") != std::string::npos || line.find("onversion") != std::string::npos ||
							line.find("profile") != std::string::npos)
							Logger::WriteMessage(("    " + line).c_str());
				if (!ok)
					existingFailures += std::string(c.label) + "; ";
			}
			Assert::IsTrue(existingFailures.empty(), (L"existing native RGB formats not presented: " +
				std::wstring(existingFailures.begin(), existingFailures.end())).c_str());
			// Off: the CPU conversion, the native path never runs (R10b/R10l too:
			// the P010 formatter, now that libplacebo's upload cannot take them).
			for (const VideoFrameEncoding e : { VideoFrameEncoding::R10b, VideoFrameEncoding::R10l })
			{
				const RunResult r = Run(e, "standard", 2.0, false, "native_rgb_ingest: off\n");
				Assert::IsFalse(r.Logged("Native RGB ingest:"), (L"R10 off: no native path\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (L"R10 off: frames presented (CPU conversion)\n" + r.Tail()).c_str());
			}
			{
				const RunResult r = Run(VideoFrameEncoding::R12B, "standard", 2.0, false, "native_rgb_ingest: off\n");
				Assert::IsFalse(r.Logged("Native RGB ingest:"), (L"off: no native path\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (L"off: frames presented\n" + r.Tail()).c_str());
			}
			// 23.976p with the de-judder on (roadmap item 2): on RGB the matcher runs
			// on the unpack's luma, its next and ahead frames unpacked luma-only,
			// with the rendered warp (the default) - 12-bit RGB and 8-bit ARGB (a
			// receiver's RGB 4:4:4). With the source warp it holds (RGB capture).
			for (const VideoFrameEncoding e : { VideoFrameEncoding::R12B, VideoFrameEncoding::ARGB_8BIT })
			{
				const std::wstring name = e == VideoFrameEncoding::R12B ? L"film R12B" : L"film ARGB";
				const RunResult r = Run(e, "standard", 5.0, true, "dejudder_strength: 0.25\n");
				Assert::IsTrue(r.Logged("Native RGB ingest:"), (name + L": the native path ran\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("De-judder: motion estimated"), (name + L": the de-judder estimated on RGB\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("uploading the next frame failed"), (name + L": the next frames unpacked\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("de-judder frame's GPU unpack failed"), (name + L": no next-frame unpack failure\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Capture path gpu: the GPU unpack failed"), (name + L": the V210/UYVY unpack untouched\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (name + L": frames presented\n" + r.Tail()).c_str());
				for (const std::string& line : r.log)
					if (line.find("De-judder") != std::string::npos)
						Logger::WriteMessage(line.c_str());
			}
			{
				const RunResult r = Run(VideoFrameEncoding::R12B, "standard", 3.0, true,
					"dejudder_strength: 0.25\ndejudder_warp_stage: source\n");
				Assert::IsTrue(r.Logged("De-judder: RGB capture"), (L"film R12B, source warp: holds (RGB capture)\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("De-judder: motion estimated"), (L"film R12B, source warp: no estimate\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (L"film R12B, source warp: frames presented\n" + r.Tail()).c_str());
			}
		}

		TEST_METHOD(DolbyVisionGpuUnpackMatchesTheCpuUnpackThroughTheRenderer)
		{
			// dv_tunnel_gpu_unpack (owner 2026-10-16): a synthetic 1080p23.976 DV
			// tunnel (sender picture + CRC-valid DM) through the renderer. The
			// presented codes (VP-0133 raw_r10_sha256 of a still picture, dither and
			// deband off) are identical with the GPU unpack off, on, and on with the
			// GPU unpack forced to fail (VP_TEST_DV_UNPACK_FAIL=1: the CPU fallback).
			// Then a stream that loses its DM packets (CRC damaged) goes black in
			// every mode and comes back. Hardware only.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const int savedW = g_harnessWidth, savedH = g_harnessHeight;
			g_harnessWidth = 1920;
			g_harnessHeight = 1080;
			// The pictures: 0 = a tunnel picture with CRC-valid DM, 1 = the same
			// with every copy of the first packet damaged (no trusted header).
			Tests::DvSender::SenderPicture picture(false, 1920, 1080);
			for (size_t i = 0; i < picture.intensity.size(); ++i)
			{
				// A smooth picture (intensity ramp, near-neutral chroma) so the
				// render is meaningful; the sender embeds the DM's parity bits.
				const size_t x = i % 1920, y = i / 1920;
				picture.intensity[i] = static_cast<uint16_t>(256 + (x * 3000) / 1920 + (y & 63));
				picture.chroma[i] = static_cast<uint16_t>(2048 + ((x / 8 + y / 8) & 255) - 128);
				picture.Pack(i);
			}
			picture.Embed(Tests::DvSender::Packetize(Tests::DvSender::DmBody()));
			Tests::DvSender::SenderPicture damaged = picture;
			damaged.CorruptAllCopies(0);
			g_harnessDolbyVision = true;
			g_harnessDvFrames = { picture.input, damaged.input };
			wchar_t tempPath[MAX_PATH] = {};
			GetTempPathW(MAX_PATH, tempPath);
			const std::wstring shots = std::wstring(tempPath) + L"vp-dv-gpu-unpack";
			CreateDirectoryW(shots.c_str(), nullptr);
			SetEnvironmentVariableW(L"VP_TEST_SCREENSHOT_DIR", shots.c_str());
			const std::string settings = "[vprenderer.Default]\ndithering: off\ndeband: off\n";
			const std::string gpuLine = "Dolby Vision RGB tunnel: picture unpacked on the GPU";
			const std::string failLine = "Dolby Vision RGB tunnel: the GPU unpack failed";
			FramePlan still;
			still.rateNumerator = 24000;
			still.captureAtSeconds = 2.5;
			still.step = [](int) { return FrameStep{ true, 0, 0 }; };
			auto hashOf = [&](const std::string& extra, const wchar_t* name, RunResult& r) {
				r = Run(VideoFrameEncoding::R210, "standard", 3.5, true, extra + settings, false, &still);
				Assert::IsTrue(r.presented, (std::wstring(name) + L": frames presented\n" + r.Tail()).c_str());
				Assert::IsFalse(r.captureMetadata.empty(), (std::wstring(name) + L": capture written\n" + r.Tail()).c_str());
				std::ifstream in(std::wstring(r.captureMetadata.begin(), r.captureMetadata.end()));
				std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				const size_t k = text.find("\"raw_r10_sha256\": \"");
				Assert::IsTrue(k != std::string::npos, (std::wstring(name) + L": hash in the sidecar").c_str());
				const std::string hash = text.substr(k + 19, 64);
				Logger::WriteMessage((std::string(name, name + wcslen(name)) + ": " + hash + "\n").c_str());
				return hash;
			};
			RunResult off, on, fallback;
			const std::string cpuHash = hashOf("dv_tunnel_gpu_unpack: off\n", L"CPU unpack", off);
			Assert::IsFalse(off.Logged(gpuLine), (L"off: no GPU unpack\n" + off.Tail()).c_str());
			const std::string gpuHash = hashOf("dv_tunnel_gpu_unpack: on\n", L"GPU unpack", on);
			Assert::IsTrue(on.Logged(gpuLine), (L"on: the GPU unpack ran\n" + on.Tail()).c_str());
			Assert::IsFalse(on.Logged(failLine), (L"on: no fallback\n" + on.Tail()).c_str());
			Assert::AreEqual(cpuHash, gpuHash, L"the GPU unpack presents the CPU unpack's codes");
			SetEnvironmentVariableW(L"VP_TEST_DV_UNPACK_FAIL", L"1");
			const std::string fallbackHash = hashOf("dv_tunnel_gpu_unpack: on\n", L"GPU unpack forced to fail", fallback);
			Assert::IsTrue(fallback.Logged(failLine), (L"fail: the fallback ran\n" + fallback.Tail()).c_str());
			Assert::IsFalse(fallback.Logged(gpuLine), (L"fail: no GPU picture\n" + fallback.Tail()).c_str());
			Assert::AreEqual(cpuHash, fallbackHash, L"the CPU fallback presents the same codes");
			// DM loss in every mode: black while the header is untrusted, the
			// picture back after it (never the packed pixels or a stale picture).
			FramePlan loss;
			loss.rateNumerator = 24000;
			loss.step = [](int n) { return FrameStep{ true, n >= 36 && n < 60 ? 1 : 0, 0 }; };
			for (int mode = 0; mode < 3; ++mode)
			{
				SetEnvironmentVariableW(L"VP_TEST_DV_UNPACK_FAIL", mode == 2 ? L"1" : nullptr);
				const wchar_t* name = mode == 0 ? L"loss, CPU" : mode == 1 ? L"loss, GPU" : L"loss, GPU failing";
				const RunResult r = Run(VideoFrameEncoding::R210, "standard", 4.0, true,
					std::string(mode == 0 ? "dv_tunnel_gpu_unpack: off\n" : "dv_tunnel_gpu_unpack: on\n") + settings, false, &loss);
				Assert::IsTrue(r.presented, (std::wstring(name) + L": frames presented\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Dolby Vision transition: presenting black"), (std::wstring(name) + L": black on DM loss\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Dolby Vision transition: trusted DV picture restored"), (std::wstring(name) + L": picture restored\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Alpha conversion failed"), (std::wstring(name) + L": no conversion failure\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("could not be unpacked on the GPU or the CPU"), (std::wstring(name) + L": no unrecoverable picture\n" + r.Tail()).c_str());
			}
			SetEnvironmentVariableW(L"VP_TEST_DV_UNPACK_FAIL", nullptr);
			SetEnvironmentVariableW(L"VP_TEST_SCREENSHOT_DIR", nullptr);
			g_harnessDolbyVision = false;
			g_harnessDvFrames.clear();
			g_harnessWidth = savedW;
			g_harnessHeight = savedH;
		}

		TEST_METHOD(CapturePathRunsThroughTheRendererWithFakeDeckLinkFrames)
		{
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const std::string gpuLine = "Capture path gpu: ", failed = "Capture path gpu: the GPU unpack failed";
			// gpu, V210: unpacked on the GPU, never failed, frames presented.
			{
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 3.0);
				Assert::IsTrue(r.created, (L"V210 gpu: renderer not created\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Capture path gpu: V210 unpacked on the GPU"), (L"V210 gpu: the GPU path ran\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged(failed), (L"V210 gpu: no fallback\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Alpha conversion failed"), (L"V210 gpu: no conversion failure\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (L"V210 gpu: frames presented\n" + r.Tail()).c_str());
			}
			// gpu, UYVY.
			{
				const RunResult r = Run(VideoFrameEncoding::UYVY, "gpu", 2.0);
				Assert::IsTrue(r.Logged("Capture path gpu: UYVY/HDYC unpacked on the GPU"), (L"UYVY gpu: the GPU path ran\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged(failed), (L"UYVY gpu: no fallback\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (L"UYVY gpu: frames presented\n" + r.Tail()).c_str());
			}
			// standard: the CPU formatters, the GPU path never runs.
			{
				const RunResult r = Run(VideoFrameEncoding::V210, "standard", 2.0);
				Assert::IsFalse(r.Logged(gpuLine), (L"standard: no GPU path\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (L"standard: frames presented\n" + r.Tail()).c_str());
			}
			// 23.976p with the de-judder (and estimate ahead, a queue of 4):
			// its next and ahead frames come from raw payloads, unpacked on the
			// GPU too.
			{
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 5.0, true,
					"dejudder_strength: 0.25\nocclusion_mask: on\n");
				Assert::IsTrue(r.Logged("Capture path gpu: V210 unpacked on the GPU"), (L"film gpu: the GPU path ran\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("De-judder: motion estimated"), (L"film gpu: the de-judder estimated from GPU planes\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("uploading the next frame failed"), (L"film gpu: the next frames unpacked\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged(failed), (L"film gpu: no fallback\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented, (L"film gpu: frames presented\n" + r.Tail()).c_str());
				for (const std::string& line : r.log)
					if (line.find("De-judder") != std::string::npos || line.find("Capture path") != std::string::npos)
						Logger::WriteMessage(line.c_str());
			}
			// gpu with the unpack forced to fail: the fallback, logged once, and
			// the CPU formatters keep the picture going.
			{
				_putenv_s("VP_TEST_CAPTURE_UNPACK_FAIL", "1");
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 3.0);
				_putenv_s("VP_TEST_CAPTURE_UNPACK_FAIL", "");
				Assert::IsTrue(r.Logged(failed), (L"forced failure: the fallback is logged\n" + r.Tail()).c_str());
				int failures = 0;
				for (const std::string& line : r.log) failures += line.find(failed) != std::string::npos ? 1 : 0;
				Assert::AreEqual(1, failures, L"logged once");
				Assert::IsTrue(r.presented, (L"forced failure: frames still presented (CPU path)\n" + r.Tail()).c_str());
			}
		}

		// --- VP's 50/60p interpolation through the renderer (docs/VP-interpolator-50-60-plan.md) ---
		// The source rate is derived from the monitor's refresh (5/6 of it: 50p on a 60 Hz
		// display, whatever the display), never a fixed rate (owner [[vp-general-not-fixed]]).
		static double HarnessMonitorHz()
		{
			DEVMODEA mode{};
			mode.dmSize = sizeof(mode);
			return EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1 ?
				static_cast<double>(mode.dmDisplayFrequency) : 60.0;
		}

		static FramePlan RatePlan(double sourceHz)
		{
			FramePlan plan;
			plan.rateNumerator = static_cast<int>(std::lround(sourceHz * 1000.0));
			plan.rateDenominator = 1000;
			plan.step = [](int n) { FrameStep f; f.content = n; return f; };
			return plan;
		}

		static void ReportInterpolation(const char* name, const RunResult& r)
		{
			Logger::WriteMessage((std::string("== ") + name + ": " + r.dejudderStatus).c_str());
			for (const std::string& line : r.log)
				if (line.find("Interpolation") != std::string::npos || line.find("motion invalidated") != std::string::npos ||
					line.find("interpolated=1") != std::string::npos || line.find("Motion engine") != std::string::npos)
					Logger::WriteMessage(("   " + line).c_str());
		}

		TEST_METHOD(InterpolationRunsAtAMeasuredRateMismatch)
		{
			// auto at a non-integer ratio interpolates (the estimate runs for a video-rate
			// source); at the display's own rate it passes through; off logs nothing.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const double display = HarnessMonitorHz();
			{
				const FramePlan plan = RatePlan(display * 5.0 / 6.0);
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 4.0, false, "interpolation: auto\n", false, &plan);
				ReportInterpolation("mismatch, auto", r);
				Assert::IsTrue(r.presented, (L"mismatch: frames presented\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Interpolation: interpolating"), (L"mismatch: interpolating\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("De-judder: motion estimated"), (L"mismatch: the motion estimated for the video source\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Interpolation: failed"), (L"mismatch: no interpolation failure\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("extra render failed") || r.Logged("extra present failed"), (L"mismatch: no render failure\n" + r.Tail()).c_str());
			}
			{
				const FramePlan plan = RatePlan(display);
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 3.0, false, "interpolation: auto\n", false, &plan);
				ReportInterpolation("display rate, auto", r);
				Assert::IsTrue(r.Logged("Interpolation: whole ratio: passes through"), (L"1:1: untouched\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("De-judder: motion estimated"), (L"1:1: no estimate\n" + r.Tail()).c_str());
			}
			{
				const FramePlan plan = RatePlan(display * 5.0 / 6.0);
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 3.0, false, "", false, &plan);
				Assert::IsFalse(r.Logged("Interpolation:"), (L"off by default\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("De-judder: motion estimated"), (L"off: no estimate\n" + r.Tail()).c_str());
			}
		}

		TEST_METHOD(InterpolationLowLatencyFromTheFirstFrame)
		{
			// Zero queue / low latency: never interpolate, never wait for B (today's output),
			// from the first frame (the queue profile applied before Build, as the app does).
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const FramePlan plan = RatePlan(HarnessMonitorHz() * 5.0 / 6.0);
			g_harnessApplySnapshot = true;
			const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 4.0, false,
				"interpolation: auto\n[queue.Low_Latency]\nqueue_size: 4\ntarget_frames: 1\n", false, &plan);
			g_harnessApplySnapshot = false;
			ReportInterpolation("low latency", r);
			Assert::IsTrue(r.presented, (L"frames presented\n" + r.Tail()).c_str());
			Assert::IsTrue(r.Logged("Interpolation: low-latency queue"), (L"off in low latency\n" + r.Tail()).c_str());
			Assert::IsFalse(r.Logged("Interpolation: interpolating"), (L"never interpolating\n" + r.Tail()).c_str());
			Assert::IsFalse(r.Logged("Motion ahead: engaged"), (L"no look-ahead\n" + r.Tail()).c_str());
			Assert::IsFalse(r.Logged("interpolated=1"), (L"no interpolated picture\n" + r.Tail()).c_str());
		}

		TEST_METHOD(InterpolationStreamFaultsFallBackAndRecover)
		{
			// Missed frames and a resync during interpolation: the vectors are invalidated
			// (MotionValidity), the interpolator holds, and it estimates again on a fresh pair.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const double source = HarnessMonitorHz() * 5.0 / 6.0;
			auto after = [](const RunResult& r, const std::string& marker, const std::string& text)
			{
				bool seen = false;
				for (const std::string& line : r.log)
				{
					if (line.find(marker) != std::string::npos) seen = true;
					else if (seen && line.find(text) != std::string::npos) return true;
				}
				return false;
			};
			{
				FramePlan plan = RatePlan(source);
				plan.step = [](int n) { FrameStep f; f.content = n; f.send = !(n == 80 || n == 81 || (n >= 120 && n <= 122)); return f; };
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 5.0, false, "interpolation: auto\n", false, &plan);
				ReportInterpolation("missed frames", r);
				Assert::IsTrue(r.Logged("capture missed"), (L"missed: counted\n" + r.Tail()).c_str());
				Assert::IsTrue(after(r, "capture missed", "motion invalidated"), (L"missed: vectors invalidated\n" + r.Tail()).c_str());
				Assert::IsTrue(after(r, "motion invalidated", "De-judder: estimating again"), (L"missed: recovers\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Interpolation: failed"), (L"missed: no failure\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented);
			}
			{
				FramePlan plan = RatePlan(source);
				plan.resetAtSeconds = 2.0;
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 5.0, false, "interpolation: auto\n", false, &plan);
				ReportInterpolation("resync", r);
				Assert::IsTrue(r.Logged("motion invalidated (resync"), (L"resync: released\n" + r.Tail()).c_str());
				Assert::IsTrue(after(r, "motion invalidated (resync", "De-judder: estimating again"), (L"resync: recovers\n" + r.Tail()).c_str());
				Assert::IsTrue(r.presented);
			}
		}

		TEST_METHOD(InterpolationIsExclusiveWithSmoothMotion)
		{
			// One motion engine (owner 2026-10-15): with libplacebo's smooth motion selected,
			// VP's interpolation stays off (no estimate, no vectors).
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const FramePlan plan = RatePlan(HarnessMonitorHz() * 5.0 / 6.0);
			const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 4.0, false,
				"interpolation: auto\n[vprenderer.motion]\nmotion_engine: smooth_motion\nsmooth_motion: always\n", false, &plan);
			ReportInterpolation("smooth motion selected", r);
			Assert::IsTrue(r.Logged("Motion engine: libplacebo smooth motion"), (L"smooth motion is the engine\n" + r.Tail()).c_str());
			Assert::IsFalse(r.Logged("Interpolation: interpolating"), (L"VP's interpolation off\n" + r.Tail()).c_str());
			Assert::IsFalse(r.Logged("De-judder: motion estimated"), (L"no estimate\n" + r.Tail()).c_str());
			Assert::IsTrue(r.presented);
		}

		TEST_METHOD(InterpolationTimelineAtEveryRatio)
		{
			// Stage B: the per-refresh timeline engages above and below 1 (5/6 and 6/5 of the
			// display rate: 50p on 60 Hz, 60p on 50 Hz, whatever the display); below 1 it skips
			// source frames instead of letting drift correction drop them; no failures.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const double display = HarnessMonitorHz();
			struct Case { double factor; const char* name; bool skips; };
			for (const Case& c : { Case{ 5.0 / 6.0, "slower than the display", false }, Case{ 6.0 / 5.0, "faster than the display", true },
				Case{ 0.4, "film-like rate without the de-judder", false } })
			{
				const FramePlan plan = RatePlan(display * c.factor);
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 5.0, false, "interpolation: auto\n", false, &plan);
				ReportInterpolation(c.name, r);
				const std::wstring name(c.name, c.name + strlen(c.name));
				Assert::IsTrue(r.presented, (name + L": frames presented\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Interpolation: interpolating"), (name + L": interpolating\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Interpolation timeline: engaged"), (name + L": the timeline engaged\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Interpolation timeline: pictures"), (name + L": pictures between frames\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Interpolation: failed"), (name + L": no failure\n" + r.Tail()).c_str());
				Assert::AreEqual(c.skips, r.Logged("first source frame skipped"),
					(name + (c.skips ? L": source frames skipped\n" : L": no source frame skipped\n") + r.Tail()).c_str());
			}
		}

		TEST_METHOD(InterpolationGapFillPairsAcrossMissedFrames)
		{
			// interpolation_fill_gaps: on - a pair across one or two missed capture frames
			// (the timeline spans them); off (default) - no pair across the gap.
			if (!OnHardware())
			{
				Logger::WriteMessage(L"renderer capture harness: VP_TEST_GPU=hardware only; not run");
				return;
			}
			const double source = HarnessMonitorHz() * 5.0 / 6.0;
			for (const bool fill : { true, false })
			{
				FramePlan plan = RatePlan(source);
				plan.step = [](int n) { FrameStep f; f.content = n; f.send = !(n == 90 || (n >= 150 && n <= 151)); return f; };
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 5.0, false,
					std::string("interpolation: auto\ninterpolation_fill_gaps: ") + (fill ? "on" : "off") + "\n", false, &plan);
				ReportInterpolation(fill ? "gap fill on" : "gap fill off", r);
				Assert::IsTrue(r.Logged("capture missed"), (L"the gaps were counted\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Interpolation: failed"), (L"no failure\n" + r.Tail()).c_str());
				Assert::AreEqual(fill, r.Logged("Interpolation: gap pair across"),
					(std::wstring(fill ? L"on: gap pairs made\n" : L"off: no gap pair\n") + r.Tail()).c_str());
				Assert::IsTrue(r.presented);
			}
		}

		TEST_METHOD(InterpolationOnScreenFillsTheRepeatRefreshes)
		{
			// On the screen only (VP_TEST_HARNESS_ONSCREEN=1, real refresh statistics): at
			// 5/6 of the display rate one refresh in six would repeat a frame; Stage A shows
			// an interpolated picture there (the extras planner's interpolated=1 lines).
			char* v = nullptr;
			size_t n = 0;
			const bool on = _dupenv_s(&v, &n, "VP_TEST_HARNESS_ONSCREEN") == 0 && v && std::string(v) == "1";
			std::free(v);
			if (!on || !OnHardware())
			{
				Logger::WriteMessage(L"on-screen harness: VP_TEST_HARNESS_ONSCREEN=1 and VP_TEST_GPU=hardware only; not run");
				return;
			}
			const FramePlan plan = RatePlan(HarnessMonitorHz() * 5.0 / 6.0);
			for (const char* fallback : { "hold", "blend" })
			{
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 8.0, false,
					std::string("interpolation: auto\ninterpolation_fallback: ") + fallback + "\n", true, &plan);
				ReportInterpolation(fallback, r);
				const std::wstring name(fallback, fallback + strlen(fallback));
				Assert::IsTrue(r.Logged("Interpolation timeline: pictures"), (name + L": interpolated pictures presented\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Interpolation: failed"), (name + L": no failure\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("extra render failed") || r.Logged("extra present failed"), (name + L": no render failure\n" + r.Tail()).c_str());
			}
		}
	};
}
