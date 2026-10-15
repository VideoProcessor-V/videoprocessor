#include "pch.h"
#include "CppUnitTest.h"

#include <DisplayMode.h>
#include <IRenderer.h>
#include <ITimingClock.h>
#include <VideoFrame.h>
#include <VideoState.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
			fullscreen ? screenW : 640, fullscreen ? screenH : 360, nullptr, nullptr, wc.hInstance, nullptr);
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
