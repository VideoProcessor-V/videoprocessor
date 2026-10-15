#include "pch.h"
#include "CppUnitTest.h"

#include <DisplayMode.h>
#include <IRenderer.h>
#include <ITimingClock.h>
#include <VideoFrame.h>
#include <VideoState.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <cstring>
#include <sstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

	// One moving frame: vertical bars in luma, a chroma ramp, shifted by n.
	void FillV210(std::vector<BYTE>& frame, int width, int height, size_t rowBytes, int n)
	{
		for (int y = 0; y < height; ++y)
		{
			uint32_t* row = reinterpret_cast<uint32_t*>(frame.data() + rowBytes * y);
			for (int pack = 0; pack < (width + 5) / 6; ++pack)
			{
				auto luma = [&](int x) { return static_cast<uint32_t>(64 + ((x + 4 * n) / 32 % 2) * 700 + (y % 64)); };
				auto chroma = [&](int x) { return static_cast<uint32_t>(512 + ((x + y) % 128) - 64); };
				const int x = pack * 6;
				row[pack * 4 + 0] = chroma(x) | (luma(x) << 10) | (chroma(x + 1) << 20);
				row[pack * 4 + 1] = luma(x + 1) | (chroma(x + 2) << 10) | (luma(x + 2) << 20);
				row[pack * 4 + 2] = chroma(x + 3) | (luma(x + 3) << 10) | (chroma(x + 4) << 20);
				row[pack * 4 + 3] = luma(x + 4) | (chroma(x + 5) << 10) | (luma(x + 5) << 20);
			}
		}
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
		HWND hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (onScreen ? WS_EX_TOPMOST : 0), wc.lpszClassName,
			"VP capture harness", WS_POPUP, onScreen ? screenW - 660 : -4000, onScreen ? screenH - 420 : -4000, 640, 360,
			nullptr, nullptr, wc.hInstance, nullptr);
		Assert::IsNotNull(hwnd);
		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		Callback callback;
		Clock clock;
		IVideoRenderer* renderer = create(&callback, 1, hwnd, &clock, true, 8,
			VideoConversionOverride::VIDEOCONVERSION_NONE, configPath.c_str(), &CaptureLog);
		result.created = renderer != nullptr;
		if (renderer)
		{
			const int width = 1920, height = 1080;
			VideoStateComPtr state = new VideoState();
			state->valid = true;
			const int rateNumerator = plan && plan->rateNumerator > 0 ? plan->rateNumerator : film ? 24000 : 60000;
			const int rateDenominator = plan && plan->rateNumerator > 0 ? plan->rateDenominator : 1001;
			state->displayMode = std::make_shared<DisplayMode>(width, height, false, rateNumerator, rateDenominator);
			state->videoFrameEncoding = encoding;
			state->colorspace = ColorSpace::REC_709;
			state->eotf = EOTF::SDR;
			renderer->OnVideoState(state);
			renderer->Build();
			renderer->SetQueueFramePolicy(2, film ? 4 : 3, true);
			renderer->Start();
			// A ring of frames that outlives anything the queue holds.
			const size_t rowBytes = static_cast<size_t>(state->BytesPerRow());
			std::vector<std::vector<BYTE>> ring(24, std::vector<BYTE>(static_cast<size_t>(state->BytesPerFrame())));
			std::vector<SourceBuffer> buffers(ring.size());
			const auto start = std::chrono::steady_clock::now();
			const auto period = std::chrono::microseconds(static_cast<int64_t>(
				1e6 * rateDenominator / rateNumerator * (plan ? plan->paceScale : 1.0)));
			const double ticksPerUs = static_cast<double>(clock.TimingClockTicksPerSecond()) / 1e6;
			bool resetDone = false;
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
				if (step.send)
				{
					std::vector<BYTE>& buffer = ring[n % ring.size()];
					if (encoding == VideoFrameEncoding::V210) FillV210(buffer, width, height, rowBytes, step.content);
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
			result.presented = renderer->HasPresentedLiveFrame();
			CString status;
			if (renderer->GetDejudderStatus(status)) result.dejudderStatus = CStringA(status).GetString();
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
				failures += r.presented ? 0 : 1;
			}
			Assert::AreEqual(0, failures, L"every on-screen run presented frames");
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
	};
}
