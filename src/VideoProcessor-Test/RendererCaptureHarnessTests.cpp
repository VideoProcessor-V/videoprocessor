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
#include <cctype>
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
#include <calibration/CalibrationController.h>
#include <calibration/CalibrationSimulation.h>
#include <calibration/PatternCodes.h>
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

	bool EnvIs(const char* name, const char* want)
	{
		char* v = nullptr;
		size_t n = 0;
		const bool is = _dupenv_s(&v, &n, name) == 0 && v && std::string(v) == want;
		std::free(v);
		return is;
	}

	// VP_TEST_REQUIRE_GPU=1: a gate run that must exercise the GPU; a hardware test that would
	// skip fails instead.
	bool RequireGpu()
	{
		return EnvIs("VP_TEST_REQUIRE_GPU", "1");
	}

	// The hardware tests' guard: true when the test can run (VP_TEST_GPU=hardware). Otherwise,
	// under VP_TEST_REQUIRE_GPU=1 the test fails; without it the test logs the fixed marker
	// "SKIP-NO-GPU <name>" (searchable in the TRX) and returns false, so the caller returns.
	bool RequireHardwareOrSkip(const char* name)
	{
		if (OnHardware())
			return true;
		const std::string marker = std::string("SKIP-NO-GPU ") + name;
		if (RequireGpu())
		{
			const std::string message = marker + ": VP_TEST_REQUIRE_GPU=1 but VP_TEST_GPU is not hardware";
			Assert::Fail(std::wstring(message.begin(), message.end()).c_str());
		}
		Logger::WriteMessage((marker + "\n").c_str());
		return false;
	}

	std::wstring HarnessWide(const std::string& text)
	{
		return std::wstring(text.begin(), text.end());
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
	// The picture moving (2026-10-17, the mixed-screen rule test): shifted left by this many
	// pixels per content step (0 = still), except the top g_harnessLumaStaticRows rows (a static
	// overlay); g_harnessShotFrames > 0 adds a brightness cut every that many content steps.
	int g_harnessLumaShift = 0;
	int g_harnessLumaStaticRows = 0;
	int g_harnessShotFrames = 0;

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
					if (picture)
					{
						const bool moving = y >= g_harnessLumaStaticRows;
						long long sx = (std::min)(x, width - 1);
						if (moving && g_harnessLumaShift != 0)
						{
							sx = (sx + static_cast<long long>(g_harnessLumaShift) * n) % width;
							if (sx < 0) sx += width;
						}
						int value = g_harnessLumaPicture[static_cast<size_t>(y) * width + static_cast<size_t>(sx)];
						if (moving && g_harnessShotFrames > 0)
						{
							static const int cut[3] = { 0, 100, -100 };
							value = (std::min)(1000, (std::max)(16, value + cut[(n / g_harnessShotFrames) % 3]));
						}
						return static_cast<uint32_t>(value);
					}
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
		// Sampled once per sent frame through the second half of the run: the renderer's queue depth
		// (GetFrameQueueSize) and its capture-to-output delay (ExitLatencyMs).
		std::vector<size_t> queueDepths;
		std::vector<double> exitLatencyMs;
		// Sampled once per sent frame through the whole run (render-ahead latency trend):
		// seconds since the start, ExitLatencyMs, the queue depth and the dropped-frame count.
		struct TrendSample { double seconds; double exitLatencyMs; size_t queueDepth; uint64_t dropped; };
		std::vector<TrendSample> trend;
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
	// On-screen window mode for Run(): -1 = VP_TEST_HARNESS_FULLSCREEN (1 = fullscreen), 0 =
	// windowed, 1 = fullscreen. A test that runs both modes (OnScreenModes) sets it per pass.
	int g_harnessFullscreen = -1;

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
		// Called once per loop iteration with the seconds since the start (automatic
		// calibration: patterns shown through the running renderer).
		std::function<void(IVideoRenderer&, double)> tick;
	};

	// Every harness global and the environment variables the tests set, saved and restored
	// (HarnessScope, RAII), and reset to the defaults after every test (TEST_METHOD_CLEANUP), so a
	// test that fails half-way never leaves a 4K frame size, a DV stream, a forced unpack failure
	// or a screenshot folder to the next test.
	struct HarnessGlobals
	{
		int barRows = 0;
		std::vector<uint16_t> lumaPicture;
		int lumaShift = 0;
		int lumaStaticRows = 0;
		int shotFrames = 0;
		bool dolbyVision = false;
		std::vector<std::vector<BYTE>> dvFrames;
		int width = 1920, height = 1080;
		int windowWidth = 640, windowHeight = 360;
		size_t queueTarget = 0;
		bool applySnapshot = false;
		std::vector<std::string> shaderProfiles;
		bool shadersSelected = false;
		int fullscreen = -1;
	};

	HarnessGlobals CaptureHarnessGlobals()
	{
		HarnessGlobals g;
		g.barRows = g_harnessBarRows;
		g.lumaPicture = g_harnessLumaPicture;
		g.lumaShift = g_harnessLumaShift;
		g.lumaStaticRows = g_harnessLumaStaticRows;
		g.shotFrames = g_harnessShotFrames;
		g.dolbyVision = g_harnessDolbyVision;
		g.dvFrames = g_harnessDvFrames;
		g.width = g_harnessWidth;
		g.height = g_harnessHeight;
		g.windowWidth = g_harnessWindowWidth;
		g.windowHeight = g_harnessWindowHeight;
		g.queueTarget = g_harnessQueueTarget;
		g.applySnapshot = g_harnessApplySnapshot;
		g.shaderProfiles = g_harnessShaderProfiles;
		g.shadersSelected = g_harnessShadersSelected;
		g.fullscreen = g_harnessFullscreen;
		return g;
	}

	void RestoreHarnessGlobals(const HarnessGlobals& g)
	{
		g_harnessBarRows = g.barRows;
		g_harnessLumaPicture = g.lumaPicture;
		g_harnessLumaShift = g.lumaShift;
		g_harnessLumaStaticRows = g.lumaStaticRows;
		g_harnessShotFrames = g.shotFrames;
		g_harnessDolbyVision = g.dolbyVision;
		g_harnessDvFrames = g.dvFrames;
		g_harnessWidth = g.width;
		g_harnessHeight = g.height;
		g_harnessWindowWidth = g.windowWidth;
		g_harnessWindowHeight = g.windowHeight;
		g_harnessQueueTarget = g.queueTarget;
		g_harnessApplySnapshot = g.applySnapshot;
		g_harnessShaderProfiles = g.shaderProfiles;
		g_harnessShadersSelected = g.shadersSelected;
		g_harnessFullscreen = g.fullscreen;
	}

	// The window modes an on-screen test runs (render-ahead follow-up 2026-10-17):
	// VP_TEST_HARNESS_FULLSCREEN=1 fullscreen only, =both windowed then fullscreen, otherwise
	// windowed only. Each pass sets g_harnessFullscreen; the report names the mode.
	std::vector<int> OnScreenModes()
	{
		char* v = nullptr;
		size_t n = 0;
		const std::string value = _dupenv_s(&v, &n, "VP_TEST_HARNESS_FULLSCREEN") == 0 && v ? v : "";
		std::free(v);
		if (value == "both") return { 0, 1 };
		if (value == "1") return { 1 };
		return { 0 };
	}

	const char* OnScreenModeName(int fullscreen) { return fullscreen == 1 ? "fullscreen" : "windowed"; }

	// "De-judder: hold (<reason>; <n> holds since the last line, ...)" -> per reason, the holds
	// (the line is rate-limited, so the count, not the lines).
	std::map<std::string, long long> HoldCounts(const std::vector<std::string>& log)
	{
		std::map<std::string, long long> holds;
		for (const std::string& line : log)
		{
			const size_t at = line.find("De-judder: hold (");
			if (at == std::string::npos)
				continue;
			const size_t semi = line.find(';', at);
			if (semi == std::string::npos)
				continue;
			const long long count = std::atoll(line.c_str() + semi + 1);
			holds[line.substr(at + 17, semi - at - 17)] += count > 0 ? count : 1;
		}
		return holds;
	}

	const wchar_t* const kHarnessEnvironment[] = { L"VP_TEST_SCREENSHOT_DIR", L"VP_TEST_CAPTURE_UNPACK_FAIL",
		L"VP_TEST_DV_UNPACK_FAIL", L"VP_TEST_SHADER_DIR" };

	// Both copies of the environment: the tests set these through the Win32 block
	// (SetEnvironmentVariable) and through the CRT (_putenv_s).
	void SetHarnessEnvironment(const wchar_t* name, const std::wstring* value)
	{
		if (value)
		{
			_wputenv_s(name, value->c_str());
			SetEnvironmentVariableW(name, value->c_str());
		}
		else
		{
			_wputenv_s(name, L"");
			SetEnvironmentVariableW(name, nullptr);
		}
	}

	class HarnessScope
	{
	public:
		HarnessScope() : globals(CaptureHarnessGlobals())
		{
			for (const wchar_t* name : kHarnessEnvironment)
			{
				const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
				if (size == 0)
				{
					environment.push_back(std::make_pair(false, std::wstring()));
					continue;
				}
				std::wstring value(size, L'\0');
				const DWORD got = GetEnvironmentVariableW(name, &value[0], size);
				value.resize(got < size ? got : size - 1);
				environment.push_back(std::make_pair(true, value));
			}
		}
		HarnessScope(const HarnessScope&) = delete;
		HarnessScope& operator=(const HarnessScope&) = delete;
		~HarnessScope()
		{
			RestoreHarnessGlobals(globals);
			for (size_t i = 0; i < environment.size(); ++i)
				SetHarnessEnvironment(kHarnessEnvironment[i], environment[i].first ? &environment[i].second : nullptr);
		}

	private:
		HarnessGlobals globals;
		std::vector<std::pair<bool, std::wstring>> environment;
	};

	void ResetHarnessDefaults()
	{
		RestoreHarnessGlobals(HarnessGlobals());
		for (const wchar_t* name : kHarnessEnvironment)
			SetHarnessEnvironment(name, nullptr);
	}

	// Releases what Run() created even when an Assert fails part-way (Asserts throw): the
	// renderer stopped and destroyed, then its window, so the next run starts clean.
	class RunResources
	{
	public:
		using Destroy = void(__cdecl*)(IVideoRenderer*);
		RunResources() = default;
		RunResources(const RunResources&) = delete;
		RunResources& operator=(const RunResources&) = delete;
		~RunResources()
		{
			try { Release(); }
			catch (...) {}
		}
		void SetWindow(HWND window) { hwnd = window; }
		void SetRenderer(IVideoRenderer* created, Destroy destroyFunction) { renderer = created; destroy = destroyFunction; }
		void Started() { started = true; }
		void ReleaseRenderer()
		{
			if (renderer)
			{
				if (started)
					renderer->Stop();
				if (destroy)
					destroy(renderer);
			}
			renderer = nullptr;
			started = false;
		}
		void Release()
		{
			ReleaseRenderer();
			if (hwnd)
				DestroyWindow(hwnd);
			hwnd = nullptr;
		}

	private:
		IVideoRenderer* renderer = nullptr;
		Destroy destroy = nullptr;
		bool started = false;
		HWND hwnd = nullptr;
	};

	// A writable folder for rendered-output captures (the test runner lives in Program Files).
	std::wstring HarnessTempFolder(const wchar_t* name)
	{
		wchar_t tempPath[MAX_PATH] = {};
		GetTempPathW(MAX_PATH, tempPath);
		const std::wstring folder = std::wstring(tempPath) + name;
		CreateDirectoryW(folder.c_str(), nullptr);
		return folder;
	}

	// The capture's raw_r10_sha256 (VP-0133 sidecar); empty when missing.
	std::string CaptureHash(const RunResult& r)
	{
		if (r.captureMetadata.empty())
			return std::string();
		std::ifstream in(std::wstring(r.captureMetadata.begin(), r.captureMetadata.end()));
		std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const size_t k = text.find("\"raw_r10_sha256\": \"");
		return k == std::string::npos ? std::string() : text.substr(k + 19, 64);
	}

	// The capture's PNG (16-bit, R10 code c stored as (c << 6) | (c >> 4)) as R10 codes, three per
	// pixel (R, G, B); empty when it cannot be read.
	std::vector<int> LoadCaptureRgb(const std::string& json, int& w, int& h)
	{
		std::vector<int> codes;
		w = h = 0;
		if (json.size() < 5)
			return codes;
		const std::string png = json.substr(0, json.size() - 5) + ".png";
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		CComPtr<IWICImagingFactory> factory;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return codes;
		CComPtr<IWICBitmapDecoder> decoder;
		const std::wstring wpng(png.begin(), png.end());
		if (FAILED(factory->CreateDecoderFromFilename(wpng.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return codes;
		CComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame))) return codes;
		CComPtr<IWICFormatConverter> converter;
		if (FAILED(factory->CreateFormatConverter(&converter))) return codes;
		if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
			WICBitmapPaletteTypeCustom))) return codes;
		UINT uw = 0, uh = 0;
		converter->GetSize(&uw, &uh);
		std::vector<uint16_t> rgba(static_cast<size_t>(uw) * uh * 4);
		if (FAILED(converter->CopyPixels(nullptr, uw * 8, static_cast<UINT>(rgba.size() * 2), reinterpret_cast<BYTE*>(rgba.data()))))
			return codes;
		w = static_cast<int>(uw);
		h = static_cast<int>(uh);
		codes.resize(static_cast<size_t>(w) * h * 3);
		for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
			for (size_t c = 0; c < 3; ++c)
				codes[i * 3 + c] = rgba[i * 4 + c] >> 6;
		return codes;
	}

	template <typename T>
	double Median(std::vector<T> values)
	{
		if (values.empty())
			return -1.0;
		std::sort(values.begin(), values.end());
		return static_cast<double>(values[values.size() / 2]);
	}

	// The renderer from its DLL, a profile with capture_path, frames for
	// `seconds`, then stopped and destroyed (also when an Assert fails part-way: RunResources).
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
		const bool fullscreenEnv = _dupenv_s(&fs, &fsn, "VP_TEST_HARNESS_FULLSCREEN") == 0 && fs && std::string(fs) == "1";
		std::free(fs);
		const bool fullscreen = onScreen && (g_harnessFullscreen >= 0 ? g_harnessFullscreen == 1 : fullscreenEnv);
		// Fullscreen: an ordinary activated window (foreground, covering the
		// screen) so presentation can bypass composition and report statistics.
		RunResources resources;
		HWND hwnd = CreateWindowExA(fullscreen ? WS_EX_TOPMOST : (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (onScreen ? WS_EX_TOPMOST : 0)), wc.lpszClassName,
			"VP capture harness", WS_POPUP, fullscreen ? 0 : onScreen ? screenW - 660 : -4000, fullscreen ? 0 : onScreen ? screenH - 420 : -4000,
			fullscreen ? screenW : g_harnessWindowWidth, fullscreen ? screenH : g_harnessWindowHeight, nullptr, nullptr, wc.hInstance, nullptr);
		resources.SetWindow(hwnd);
		Assert::IsNotNull(hwnd);
		ShowWindow(hwnd, fullscreen ? SW_SHOW : SW_SHOWNOACTIVATE);
		if (fullscreen)
			SetForegroundWindow(hwnd);
		Callback callback;
		Clock clock;
		IVideoRenderer* renderer = create(&callback, 1, hwnd, &clock, true, 8,
			VideoConversionOverride::VIDEOCONVERSION_NONE, configPath.c_str(), &CaptureLog);
		result.created = renderer != nullptr;
		resources.SetRenderer(renderer, destroy);
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
			resources.Started();
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
				if (plan && plan->tick)
					plan->tick(*renderer, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
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
				result.trend.push_back({ std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
					renderer->ExitLatencyMs(), renderer->GetFrameQueueSize(), renderer->DroppedFrameCount() });
				if (std::chrono::steady_clock::now() - start >= std::chrono::duration<double>(seconds * 0.5))
				{
					result.queueDepths.push_back(renderer->GetFrameQueueSize());
					result.exitLatencyMs.push_back(renderer->ExitLatencyMs());
				}
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
			resources.ReleaseRenderer();
		}
		resources.Release();
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
		// Whatever a test left (a failed test skips its own restores): the harness defaults and no
		// test environment variables for the next test.
		TEST_METHOD_CLEANUP(ResetHarness)
		{
			ResetHarnessDefaults();
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(OnScreenLiveExtrasAndEstimateAhead)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuOnScreen")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OnScreenLiveExtrasAndEstimateAhead)
		{
			const HarnessScope harnessScope;
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
			if (!on)
			{
				Logger::WriteMessage("SKIP-OPT-IN OnScreenLiveExtrasAndEstimateAhead: VP_TEST_HARNESS_ONSCREEN=1 only; not run\n");
				return;
			}
			if (!RequireHardwareOrSkip("OnScreenLiveExtrasAndEstimateAhead")) return;
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
			// Render-ahead follow-up 2026-10-17: this test runs the per-refresh planner (render_ahead
			// off), which needs DXGI frame statistics. On a display without them (the laptop's
			// internal panel) every film case held with "timing not known: display rate" and the
			// test passed vacuously. Such a run is now reported INCONCLUSIVE with the marker
			// SKIP-NO-DISPLAY-TIMING (run_chain_gate.ps1 fails on it when on-screen is requested).
			// VP_TEST_HARNESS_FULLSCREEN=1|both: the fullscreen pass (reported separately).
			int failures = 0;
			for (const int mode : OnScreenModes())
			{
			g_harnessFullscreen = mode;
			const std::string modeName = OnScreenModeName(mode);
			int noTiming = 0, filmCases = 0, filmWithExtras = 0;
			for (const Case& c : cases)
			{
				std::string extra = c.extra;
				for (char& ch : extra) if (ch == '@') ch = '\n';
				const RunResult r = Run(VideoFrameEncoding::V210, c.path, seconds > 5.0 ? seconds : 60.0, c.film, extra, true);
				Logger::WriteMessage((std::string("== [") + modeName + "] " + c.name + ": presented " + (r.presented ? "yes" : "NO")).c_str());
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
				if (c.film)
				{
					++filmCases;
					filmWithExtras += extrasLines > 0 ? 1 : 0;
					// No statistics: the display rate never became known, so nothing was tested.
					if (extrasLines == 0 && holds.count("timing not known: display rate") > 0)
					{
						++noTiming;
						Logger::WriteMessage("   INCONCLUSIVE: the display gave no frame statistics (timing not known: display rate)");
					}
				}
				failures += r.presented ? 0 : 1;
			}
			Logger::WriteMessage(("-- [" + modeName + "] film cases with extras " + std::to_string(filmWithExtras) + " of " +
				std::to_string(filmCases) + ", without display timing " + std::to_string(noTiming)).c_str());
			if (noTiming > 0)
				Logger::WriteMessage(("SKIP-NO-DISPLAY-TIMING OnScreenLiveExtrasAndEstimateAhead: [" + modeName + "] " +
					std::to_string(noTiming) + " film case(s) held for want of DXGI frame statistics; outcome INCONCLUSIVE "
					"(the per-refresh planner needs statistics; render_ahead runs open loop: see OnScreenRenderAheadExtras)\n").c_str());
			}
			Assert::AreEqual(0, failures, L"every on-screen run presented frames");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(RecordedStreamReplaysThroughTheRenderer)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(RecordedStreamReplaysThroughTheRenderer)
		{
			const HarnessScope harnessScope;
			// Owner 2026-10-15: a recording from VideoProcessorStreamRecorder
			// (frames.csv: hardware times, periods since the previous frame,
			// bit-exact repeats) replayed through the real renderer with
			// pulldown on - its timing, gaps, jitter and repeat cadence as
			// captured. VP_TEST_STREAM_DIR=<recording folder>; without it a
			// synthetic recording (24p in 29.97 Hz with a 2-frame gap) checks
			// the replay itself. Hardware only.
			if (!RequireHardwareOrSkip("RecordedStreamReplaysThroughTheRenderer")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(OpticalFlowIsTheDefaultMotionSource)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OpticalFlowIsTheDefaultMotionSource)
		{
			const HarnessScope harnessScope;
			// docs/VP-nvof-ahead.md (owner 2026-10-16): the optical-flow engine is the
			// established default motion source; optical_flow: off = the block searches.
			// Without the engine (not NVIDIA) the default falls back to the searches.
			if (!RequireHardwareOrSkip("OpticalFlowIsTheDefaultMotionSource")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(StreamFaultsKeepTheDejudderSafeAndRecovering)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StreamFaultsKeepTheDejudderSafeAndRecovering)
		{
			const HarnessScope harnessScope;
			// Stream robustness and pulldown (owner 2026-10-14): each fault
			// class injected into a 23.976p de-judder run, plus the pulldown
			// cadences, through the real renderer. Hardware only.
			if (!RequireHardwareOrSkip("StreamFaultsKeepTheDejudderSafeAndRecovering")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(Stress4K60Rgb12EarlyUpload)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(Stress4K60Rgb12EarlyUpload)
		{
			const HarnessScope harnessScope;
			// The maximum stress case (owner 2026-10-15): 3840x2160 12-bit RGB (R12B,
			// the largest capture frame, 37 MB) at 59.94 Hz through the renderer, with
			// early upload on and off. Checked: frames presented, no unpack failures;
			// reported: frames read early, no-free-slot counts, the most frames held
			// at once and the presentation stats (drops) of both runs. Whether the
			// whole chain holds 4K60 in real time is the RTX 3090's to show; the
			// laptop shows the upload pipeline under the heaviest load.
			if (!RequireHardwareOrSkip("Stress4K60Rgb12EarlyUpload")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(SourceResolutionRebasesThroughTheRenderer)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SourceResolutionRebasesThroughTheRenderer)
		{
			const HarnessScope harnessScope;
			// Stage 12 step A (owner 2026-10-15): a 4K source set to 1080 is
			// re-based for libplacebo (the log names the sizes) and presents; any
			// height works (900); native (the default) never re-bases.
			if (!RequireHardwareOrSkip("SourceResolutionRebasesThroughTheRenderer")) return;
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
			const HarnessScope harnessScope;
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
				// Integration 2026-10-17: the engine's extras, the grain switches, dv_tunnel_fixes.
				"optical_flow_global_fallback: off\n", "optical_flow_cost_trust: on\n", "optical_flow_fb_check: on\n",
				"interpolation_grain: blend\n", "dejudder_source_warp_filter: bilinear\n", "deband_vp_grain_guard: off\n",
				"deband_grain: on\n", "residual_stabilise_grain_guard: off\n", "dv_tunnel_fixes: off\n",
				"deband_vp_grain_guard: veto\n", "deband_vp_exclusive: off\n",
				"a_key_not_invented_yet: 1\n" };
			for (const char* change : changes)
				Assert::AreNotEqual(base, fingerprint(change),
					(L"no fingerprint change for: " + std::wstring(change, change + strlen(change))).c_str());
			DeleteFileA(path.c_str());
		}

		// Integration 2026-10-17: the keys the merge brought (the engine's extras, the
		// grain switches, dv_tunnel_fixes) are parsed, dumped and change the live and the
		// rebuild-independent settings; at profile defaults the interpolator keeps grain
		// (preserve) and the source warp is Catmull-Rom; the sample configuration's
		// [vprenderer.motion.Default] reaches the motion settings.
		TEST_METHOD(IntegrationKeysReachTheSettingsAndTheirDefaultsAreFaithful)
		{
			const HarnessScope harnessScope;
			const std::string folder = Here();
			HMODULE dll = LoadLibraryExA((folder + "vprenderer\\VideoProcessorVPRenderer.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			Assert::IsNotNull(dll, L"the renderer DLL");
			using Export = int(__cdecl*)(const char*, char*, int);
			const auto fingerprintOf = reinterpret_cast<Export>(GetProcAddress(dll, "VideoProcessorLibplaceboSettingsFingerprintForTest"));
			const auto describe = reinterpret_cast<Export>(GetProcAddress(dll, "VideoProcessorLibplaceboDescribeSettingsForTest"));
			Assert::IsNotNull(reinterpret_cast<void*>(fingerprintOf), L"the fingerprint export");
			Assert::IsNotNull(reinterpret_cast<void*>(describe), L"the describe export");
			const std::string path = folder + "vp-integration-keys-test.cfg";
			auto call = [&](Export function, const std::string& text) {
				{
					std::ofstream config(path, std::ios::trunc | std::ios::binary);
					config << text;
				}
				std::vector<char> buffer(1 << 20);
				const int length = function(path.c_str(), buffer.data(), static_cast<int>(buffer.size()));
				Assert::IsTrue(length > 0, L"the export ran");
				return std::string(buffer.data());
			};
			const std::string head = "[general]\npersist_profile_selection: false\n[vprenderer.Default]\nquality: high\n";
			const std::string baseDump = call(describe, head);
			const std::string baseFingerprint = call(fingerprintOf, head);
			auto line = [](const std::string& dump, const std::string& key) {
				const size_t at = dump.find("\n" + key + "=");
				if (at == std::string::npos) return std::string();
				const size_t start = at + key.size() + 2;
				return dump.substr(start, dump.find('\n', start) - start);
			};
			// Profile defaults: the faithful grain paths and the restore switches on.
			const std::pair<const char*, const char*> defaults[] = {
				{ "interpolation_grain", "preserve" }, { "dejudder_source_warp_filter", "catmull" },
				{ "deband_vp_grain_guard", "on" }, { "deband_grain", "off" }, { "residual_stabilise_grain_guard", "on" },
				{ "deband_vp_exclusive", "on" },
				{ "optical_flow_global_fallback", "on" }, { "optical_flow_cost_trust", "off" }, { "optical_flow_fb_check", "off" },
				{ "dv_tunnel_fixes", "on" } };
			for (const auto& entry : defaults)
				Assert::AreEqual(std::string(entry.second), line(baseDump, entry.first),
					(L"default of " + std::wstring(entry.first, entry.first + strlen(entry.first))).c_str());
			// Each key flipped: its dump line and the fingerprint change.
			const std::pair<const char*, const char*> flips[] = {
				{ "interpolation_grain", "blend" }, { "dejudder_source_warp_filter", "bilinear" },
				{ "deband_vp_grain_guard", "off" }, { "deband_grain", "on" }, { "residual_stabilise_grain_guard", "off" },
				{ "deband_vp_grain_guard", "veto" }, { "deband_vp_exclusive", "off" },
				{ "optical_flow_global_fallback", "off" }, { "optical_flow_cost_trust", "on" }, { "optical_flow_fb_check", "on" },
				{ "dv_tunnel_fixes", "off" } };
			for (const auto& flip : flips)
			{
				const std::string text = head + flip.first + ": " + flip.second + "\n";
				const std::wstring name(flip.first, flip.first + strlen(flip.first));
				Assert::AreEqual(std::string(flip.second), line(call(describe, text), flip.first), (L"parsed: " + name).c_str());
				Assert::AreNotEqual(baseFingerprint, call(fingerprintOf, text), (L"fingerprint: " + name).c_str());
			}
			// The sample's renamed motion section reaches the motion settings (a value
			// changed from the default so the read is visible).
			std::string repository = __FILE__;
			std::replace(repository.begin(), repository.end(), '/', '\\');
			repository.resize(repository.rfind("\\src\\VideoProcessor-Test\\"));
			std::ifstream sampleFile(repository + "\\VideoProcessor.cfg", std::ios::binary);
			std::string sample((std::istreambuf_iterator<char>(sampleFile)), std::istreambuf_iterator<char>());
			Assert::IsFalse(sample.empty(), L"the sample configuration");
			const std::string sampleDump = call(describe, sample);
			Assert::IsFalse(line(sampleDump, "motion").empty(), L"motion settings dumped");
			const std::string motionSection = "[vprenderer.motion.Default]";
			const size_t at = sample.find(motionSection);
			Assert::IsTrue(at != std::string::npos, L"the sample's motion profile");
			std::string changed = sample;
			const size_t smooth = changed.find("smooth_motion: off", at);
			Assert::IsTrue(smooth != std::string::npos, L"the sample's smooth_motion");
			changed.replace(smooth, strlen("smooth_motion: off"), "smooth_motion: always");
			const std::string motion = line(call(describe, changed), "motion");
			Assert::IsTrue(motion.find("|always|") != std::string::npos,
				(L"the motion profile's smooth_motion was read: " + std::wstring(motion.begin(), motion.end())).c_str());
			DeleteFileA(path.c_str());
		}

		// Hunt 2 (picture C): nls_sampling and source_coding_fixes are parsed and dumped, default
		// to the faithful / fixed paths, and change the fingerprint (applied live).
		TEST_METHOD(Hunt2PictureKeysReachTheSettings)
		{
			const HarnessScope harnessScope;
			const std::string folder = Here();
			HMODULE dll = LoadLibraryExA((folder + "vprenderer\\VideoProcessorVPRenderer.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			Assert::IsNotNull(dll, L"the renderer DLL");
			using Export = int(__cdecl*)(const char*, char*, int);
			const auto fingerprintOf = reinterpret_cast<Export>(GetProcAddress(dll, "VideoProcessorLibplaceboSettingsFingerprintForTest"));
			const auto describe = reinterpret_cast<Export>(GetProcAddress(dll, "VideoProcessorLibplaceboDescribeSettingsForTest"));
			Assert::IsNotNull(reinterpret_cast<void*>(fingerprintOf), L"the fingerprint export");
			Assert::IsNotNull(reinterpret_cast<void*>(describe), L"the describe export");
			const std::string path = folder + "vp-hunt2-picture-keys-test.cfg";
			auto call = [&](Export function, const std::string& text) {
				{
					std::ofstream config(path, std::ios::trunc | std::ios::binary);
					config << text;
				}
				std::vector<char> buffer(1 << 20);
				const int length = function(path.c_str(), buffer.data(), static_cast<int>(buffer.size()));
				Assert::IsTrue(length > 0, L"the export ran");
				return std::string(buffer.data());
			};
			auto line = [](const std::string& dump, const std::string& key) {
				const size_t at = dump.find("\n" + key + "=");
				if (at == std::string::npos) return std::string();
				const size_t start = at + key.size() + 2;
				return dump.substr(start, dump.find('\n', start) - start);
			};
			const std::string head = "[general]\npersist_profile_selection: false\n[vprenderer.Default]\nquality: high\n";
			const std::string baseDump = call(describe, head);
			const std::string baseFingerprint = call(fingerprintOf, head);
			Assert::AreEqual(std::string("catmull"), line(baseDump, "nls_sampling"), L"nls_sampling defaults to Catmull-Rom");
			Assert::AreEqual(std::string("on"), line(baseDump, "source_coding_fixes"), L"source_coding_fixes defaults on");
			const std::pair<const char*, const char*> flips[] = { { "nls_sampling", "bilinear" }, { "source_coding_fixes", "off" } };
			for (const auto& flip : flips)
			{
				const std::string text = head + flip.first + ": " + flip.second + "\n";
				const std::wstring name(flip.first, flip.first + strlen(flip.first));
				Assert::AreEqual(std::string(flip.second), line(call(describe, text), flip.first), (L"parsed: " + name).c_str());
				Assert::AreNotEqual(baseFingerprint, call(fingerprintOf, text), (L"fingerprint: " + name).c_str());
			}
			DeleteFileA(path.c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(NativeAndNewSettingsOffArePixelIdentical)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(NativeAndNewSettingsOffArePixelIdentical)
		{
			const HarnessScope harnessScope;
			// Stage 12 (owner 2026-10-16): the exact output codes (VP-0133's
			// raw_r10_sha256 of the presented frame) of a still picture with no
			// Stage 12 keys, with source_resolution native, with auto (a short
			// run stays native) and with every new setting off are identical; a
			// 1080 re-base differs (the check can see a change).
			if (!RequireHardwareOrSkip("NativeAndNewSettingsOffArePixelIdentical")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(OneAxisRebaseKeepsTheAspectRatio)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OneAxisRebaseKeepsTheAspectRatio)
		{
			const HarnessScope harnessScope;
			// Owner 2026-10-16 ("and that the aspect ratio doesn't change"): a re-base of the
			// height alone (3840x1080: a source upscaled vertically) and a uniform one leave
			// the picture's geometry where it is - the strong edges across and down land
			// at the same output positions as without a re-base.
			if (!RequireHardwareOrSkip("OneAxisRebaseKeepsTheAspectRatio")) return;
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
			// The harness pattern is periodic (bars every 32 source pixels, a 64-row ramp), so its
			// edges all have the same height: ranking "the strongest edges" picks an arbitrary
			// subset decided by sub-code differences, and a different subset in each capture reads
			// as a move of a whole bar (the false "26 px" of 2026-10-17, with the pictures
			// identical to 0.00 px). Instead: (1) the local shift, per quarter of the profile, by
			// normalised correlation at sub-pixel steps within +-3 px (under half the output period,
			// so never a whole-period alias) - a moved or stretched picture moves the quarters;
			// (2) the profile's period (first autocorrelation peak, sub-pixel) - the scale on that
			// axis, so a changed aspect ratio changes it directly.
			auto correlation = [](const std::vector<double>& p0, const std::vector<double>& p1, int lo, int hi, double s) {
				double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
				int n = 0;
				for (int i = lo; i < hi; ++i)
				{
					const double x = i + s;
					const int i0 = static_cast<int>(std::floor(x));
					if (i0 < 0 || i0 + 1 >= static_cast<int>(p1.size())) continue;
					const double f = x - i0, a = p0[static_cast<size_t>(i)], b = p1[static_cast<size_t>(i0)] * (1.0 - f) + p1[static_cast<size_t>(i0) + 1] * f;
					sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b; ++n;
				}
				if (n < 2) return -1.0;
				const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
				return (va > 1e-12 && vb > 1e-12) ? (sab - sa * sb / n) / std::sqrt(va * vb) : -1.0;
			};
			// The worst local shift (px) and the lowest best correlation over four windows.
			auto localShifts = [&](const std::vector<double>& ref, const std::vector<double>& got, double& worstShift, double& lowestCorr) {
				worstShift = 0.0;
				lowestCorr = 1.0;
				const int n = static_cast<int>(ref.size()), lo = 16, hi = n - 16, span = (hi - lo) / 4;
				for (int k = 0; k < 4; ++k)
				{
					double bestC = -2.0, bestS = 0.0;
					for (int t = -60; t <= 60; ++t)
					{
						const double c = correlation(ref, got, lo + k * span, lo + (k + 1) * span, t * 0.05);
						if (c > bestC) { bestC = c; bestS = t * 0.05; }
					}
					worstShift = (std::max)(worstShift, std::fabs(bestS));
					lowestCorr = (std::min)(lowestCorr, bestC);
				}
			};
			// The period (px): the first autocorrelation peak above 0.5 after it has gone negative,
			// refined by a parabola; 0 when the profile is not periodic.
			auto period = [&](const std::vector<double>& p) {
				const int n = static_cast<int>(p.size());
				std::vector<double> ac(static_cast<size_t>(n / 3) + 2, 0.0);
				for (int lag = 1; lag < static_cast<int>(ac.size()); ++lag) ac[static_cast<size_t>(lag)] = correlation(p, p, 0, n - lag, lag);
				bool wentNegative = false;
				for (int lag = 2; lag + 1 < static_cast<int>(ac.size()); ++lag)
				{
					const double a = ac[static_cast<size_t>(lag) - 1], b = ac[static_cast<size_t>(lag)], c = ac[static_cast<size_t>(lag) + 1];
					wentNegative = wentNegative || b < 0.0;
					if (wentNegative && b > 0.5 && b >= a && b >= c)
					{
						const double d = a - 2.0 * b + c;
						return lag + (std::fabs(d) > 1e-12 ? 0.5 * (a - c) / d : 0.0);
					}
				}
				return 0.0;
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
			const double pa0 = period(a0), pd0 = period(d0);
			// One re-based capture against native: true when its geometry is kept on both axes.
			auto kept = [&](const char* name, const std::vector<double>& a, const std::vector<double>& d) {
				double shiftA = 0, corrA = 0, shiftD = 0, corrD = 0;
				localShifts(a0, a, shiftA, corrA);
				localShifts(d0, d, shiftD, corrD);
				const double pa = period(a), pd = period(d);
				const double scaleA = pa0 > 0 && pa > 0 ? pa / pa0 : 0.0, scaleD = pd0 > 0 && pd > 0 ? pd / pd0 : 0.0;
				char line[320];
				snprintf(line, sizeof(line), "%s: moved across %.2f px (corr %.4f), down %.2f px (corr %.4f); period across %.3f/%.3f (x%.4f), down %.3f/%.3f (x%.4f) (output %dx%d)\n",
					name, shiftA, corrA, shiftD, corrD, pa, pa0, scaleA, pd, pd0, scaleD, w0, h0);
				Logger::WriteMessage(line);
				const bool periodsKept = (pa0 <= 0 || std::fabs(scaleA - 1.0) <= 0.01) && (pd0 <= 0 || std::fabs(scaleD - 1.0) <= 0.01);
				return shiftA <= 1.0 && shiftD <= 1.0 && corrA >= 0.97 && corrD >= 0.97 && periodsKept;
			};
			Assert::IsTrue(pa0 > 0 && pd0 > 0, L"the native capture's period found on both axes (the scale check runs)");
			const bool oneKept = kept("one-axis", a1, d1);
			const bool uniKept = kept("uniform", a2, d2);
			Assert::IsTrue(oneKept, L"a one-axis re-base keeps the geometry (aspect ratio)");
			Assert::IsTrue(uniKept, L"a uniform re-base keeps the geometry");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(RebaseKeepsTheLayoutWithCropZoomNlsAndBlanking)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(RebaseKeepsTheLayoutWithCropZoomNlsAndBlanking)
		{
			const HarnessScope harnessScope;
			// Owner 2026-10-16 ("black bar detection and the resizing ... touch inside ... edge
			// blanking"): the re-base must not move the picture. Per setup, the output with
			// source_resolution native and 1080: the picture's edges and the strong edges in it
			// at the same output pixels. NLS: the stretched sides' sharpness against the centre's
			// (NLS stretches the source before the upscale, so on the re-based grid).
			if (!RequireHardwareOrSkip("RebaseKeepsTheLayoutWithCropZoomNlsAndBlanking")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(TemporalDitherChoiceControlsTheSigmaDelta)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TemporalDitherChoiceControlsTheSigmaDelta)
		{
			const HarnessScope harnessScope;
			// Owner 2026-10-16: temporal_dither sigma_delta loads VP's sigma-delta whatever the shader list
			// says; libplacebo (or off) leaves it out even when the list selects it.
			if (!RequireHardwareOrSkip("TemporalDitherChoiceControlsTheSigmaDelta")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(MotionAheadEngagesAtFourQueuedAheadAndFollowsTheQueue)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MotionAheadEngagesAtFourQueuedAheadAndFollowsTheQueue)
		{
			const HarnessScope harnessScope;
			// Owner 2026-10-16: the look-ahead engages once 4 frames are queued ahead; the queue
			// target that guarantees it (5 or 6) is measured here, then a reset disengages it and
			// it re-engages at 4. NVIDIA (VP_TEST_ADAPTER=NVIDIA).
			if (!RequireHardwareOrSkip("MotionAheadEngagesAtFourQueuedAheadAndFollowsTheQueue")) return;
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
			if (!engine)
			{
				// A GPU gate run (VP_TEST_REQUIRE_GPU=1, NVIDIA) must exercise the look-ahead.
				Assert::IsFalse(RequireGpu(), L"VP_TEST_REQUIRE_GPU=1: no optical-flow engine, the look-ahead was not tested");
				Logger::WriteMessage("SKIP-NO-ENGINE MotionAheadEngagesAtFourQueuedAheadAndFollowsTheQueue\n");
				return;
			}
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

		BEGIN_TEST_METHOD_ATTRIBUTE(LowLatencyQueueFromTheFirstFrame)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LowLatencyQueueFromTheFirstFrame)
		{
			const HarnessScope harnessScope;
			// The bug found 2026-10-16: the low-latency flag was lost on a Build. With the queue profile
			// applied before Build (as the app does), the renderer starts in the low-latency path: the
			// de-judder off and the producer never started.
			if (!RequireHardwareOrSkip("LowLatencyQueueFromTheFirstFrame")) return;
			g_harnessApplySnapshot = true;
			const RunResult low = Run(VideoFrameEncoding::V210, "gpu", 4.0, true,
				"dejudder_strength: 0.25\n[queue.Low_Latency]\nqueue_size: 4\ntarget_frames: 1\n");
			const RunResult normal = Run(VideoFrameEncoding::V210, "gpu", 4.0, true,
				"dejudder_strength: 0.25\n[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n");
			g_harnessApplySnapshot = false;
			for (const std::string& line : low.log)
				if (line.find("De-judder:") != std::string::npos || line.find("Motion ahead") != std::string::npos)
					Logger::WriteMessage(("  low: " + line + "\n").c_str());
			Assert::IsTrue(low.presented && normal.presented, L"both runs presented");
			Assert::IsTrue(low.Logged("low-latency queue"), (L"the de-judder off for the low-latency queue\n" + low.Tail()).c_str());
			Assert::IsFalse(low.Logged("Motion ahead: engaged"), (L"no look-ahead in low latency\n" + low.Tail()).c_str());
			// Anchor: the Normal run's log is there and its de-judder ran (so "not low latency" is seen, not missed).
			Assert::IsTrue(normal.Logged("De-judder: motion estimated"), (L"the Normal queue runs the de-judder\n" + normal.Tail()).c_str());
			Assert::IsFalse(normal.Logged("low-latency queue"), (L"the Normal queue is not low latency\n" + normal.Tail()).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(LowLatencyPathPropertyAndAutoEarlyUpload)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LowLatencyPathPropertyAndAutoEarlyUpload)
		{
			const HarnessScope harnessScope;
			// Owner 2026-10-16: low_latency_path on any queue profile takes the low-latency path; with
			// capture_upload auto (the default) early upload opens there and not on the regular path.
			if (!RequireHardwareOrSkip("LowLatencyPathPropertyAndAutoEarlyUpload")) return;
			g_harnessApplySnapshot = true;
			const RunResult game = Run(VideoFrameEncoding::V210, "gpu", 4.0, true,
				"dejudder_strength: 0.25\n[queue.Game]\nqueue_size: 4\ntarget_frames: 1\nlow_latency_path: on\n");
			const RunResult normal = Run(VideoFrameEncoding::V210, "gpu", 4.0, true,
				"dejudder_strength: 0.25\n[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n");
			g_harnessApplySnapshot = false;
			Assert::IsTrue(game.Logged("low-latency queue"), (L"low_latency_path on: the low-latency path\n" + game.Tail()).c_str());
			Assert::IsTrue(game.Logged("Early upload: on"), (L"capture_upload auto: early upload in low latency\n" + game.Tail()).c_str());
			// Anchor: the regular run presented and its de-judder ran.
			Assert::IsTrue(normal.presented, (L"the Normal run presented\n" + normal.Tail()).c_str());
			Assert::IsTrue(normal.Logged("De-judder: motion estimated"), (L"the Normal queue runs the de-judder\n" + normal.Tail()).c_str());
			Assert::IsFalse(normal.Logged("Early upload: on"), (L"auto: no early upload on the regular path\n" + normal.Tail()).c_str());
			Assert::IsFalse(normal.Logged("low-latency queue"), (L"the Normal queue is regular\n" + normal.Tail()).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(RebaseDebandAndCnnBlendKeepTheRecoveredDetail)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(RebaseDebandAndCnnBlendKeepTheRecoveredDetail)
		{
			const HarnessScope harnessScope;
			// Owner 2026-10-16: the adaptive scaling must not destroy the detail recovery (the re-base)
			// or the debanding. A 1080p master with fine texture and a sky banded in 8-bit steps; the
			// reference is the master fed at 1080 through the chain; the test feeds the master upscaled
			// 2x by a player (Lanczos-3) at 4K with source_resolution 1080. Per variant (plain, + VP
			// deband, + CNN upscaler with its blend) the 4K outputs are compared (picture and sky), and
			// the sky's band edges counted.
			if (!RequireHardwareOrSkip("RebaseDebandAndCnnBlendKeepTheRecoveredDetail")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(VpDebanderRunsThroughTheRenderer)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(VpDebanderRunsThroughTheRenderer)
		{
			const HarnessScope harnessScope;
			// Stage 12 §2b/§2d (owner 2026-10-15): VP's debander through the
			// renderer - on its own, with the de-judder (vectors held), and on a
			// re-based source; never a failure; off is the default.
			if (!RequireHardwareOrSkip("VpDebanderRunsThroughTheRenderer")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(MotionEnginesCoexistWithVpShaders)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MotionEnginesCoexistWithVpShaders)
		{
			const HarnessScope harnessScope;
			// v1.4.02 merge (owner 2026-10-15): libplacebo's smooth motion (Bill's)
			// and VP's motion handling are one or the other (motion_engine), and
			// VP's shaders keep working with either: the sigma-delta dither, the
			// SSIM downscaler with the adaptive blend, the de-judder settings. Off
			// the screen: the harness gives the renderer the monitor's refresh rate
			// as the app does.
			if (!RequireHardwareOrSkip("MotionEnginesCoexistWithVpShaders")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(EarlyUploadRunsThroughTheRenderer)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(EarlyUploadRunsThroughTheRenderer)
		{
			const HarnessScope harnessScope;
			// capture_upload: early (owner 2026-10-15, experimental): frames uploaded
			// on the conversion thread are read by the render (V210 and 12-bit RGB,
			// both capture paths' raw route), and the 24p de-judder's next frames
			// come from the same early textures. Each run logs how many frames were
			// read early at teardown.
			if (!RequireHardwareOrSkip("EarlyUploadRunsThroughTheRenderer")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(EarlyUploadAndTheOpticalFlowProducerTogether)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(EarlyUploadAndTheOpticalFlowProducerTogether)
		{
			const HarnessScope harnessScope;
			// Owner 2026-10-16: the two second D3D11 devices - early upload (capture_upload: early) and the
			// optical-flow producer - must play nicely: 4K 24p film with the de-judder, each combination,
			// then the low-latency path at 60 Hz with early upload on and off. Reports the presentation
			// timing; asserts no failures, frames read early, the producer engaged and never stopped.
			if (!RequireHardwareOrSkip("EarlyUploadAndTheOpticalFlowProducerTogether")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(NativeRgbIngestRunsThroughTheRenderer)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(NativeRgbIngestRunsThroughTheRenderer)
		{
			const HarnessScope harnessScope;
			// Roadmap item 2 (owner 2026-10-15): DeckLink 12-bit RGB (both byte
			// orders) and r210 reach the renderer as native RGB, unpacked on the
			// GPU, on the standard capture path as well as the gpu one; with
			// native_rgb_ingest: off the CPU conversion as before.
			if (!RequireHardwareOrSkip("NativeRgbIngestRunsThroughTheRenderer")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(DolbyVisionGpuUnpackMatchesTheCpuUnpackThroughTheRenderer)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DolbyVisionGpuUnpackMatchesTheCpuUnpackThroughTheRenderer)
		{
			const HarnessScope harnessScope;
			// dv_tunnel_gpu_unpack (owner 2026-10-16): a synthetic 1080p23.976 DV
			// tunnel (sender picture + CRC-valid DM) through the renderer. The
			// presented codes (VP-0133 raw_r10_sha256 of a still picture, dither and
			// deband off) are identical with the GPU unpack off, on, and on with the
			// GPU unpack forced to fail (VP_TEST_DV_UNPACK_FAIL=1: the CPU fallback).
			// Then a stream that loses its DM packets (CRC damaged) goes black in
			// every mode and comes back. Hardware only.
			if (!RequireHardwareOrSkip("DolbyVisionGpuUnpackMatchesTheCpuUnpackThroughTheRenderer")) return;
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
			// dv_trims on: this DM carries no L2 trim, so the stage reports neutral and
			// the picture is unchanged (the wiring, not the maths: DolbyVisionTrimsTests).
			{
				RunResult trims;
				const std::string trimHash = hashOf("dv_tunnel_gpu_unpack: on\ndv_trims: on\n", L"GPU unpack + dv_trims", trims);
				Assert::IsTrue(trims.Logged("Dolby Vision trims (non-certified): neutral: no L2 trim"),
					(L"dv_trims: neutral without L2\n" + trims.Tail()).c_str());
				Assert::AreEqual(cpuHash, trimHash, L"dv_trims without L2: unchanged");
			}
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

		BEGIN_TEST_METHOD_ATTRIBUTE(CapturePathRunsThroughTheRendererWithFakeDeckLinkFrames)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CapturePathRunsThroughTheRendererWithFakeDeckLinkFrames)
		{
			const HarnessScope harnessScope;
			if (!RequireHardwareOrSkip("CapturePathRunsThroughTheRendererWithFakeDeckLinkFrames")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(InterpolationRunsAtAMeasuredRateMismatch)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(InterpolationRunsAtAMeasuredRateMismatch)
		{
			const HarnessScope harnessScope;
			// auto at a non-integer ratio interpolates (the estimate runs for a video-rate
			// source); at the display's own rate it passes through; off logs nothing.
			if (!RequireHardwareOrSkip("InterpolationRunsAtAMeasuredRateMismatch")) return;
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
				// Anchor: the run presented and its log was captured (the GPU capture path ran).
				Assert::IsTrue(r.presented, (L"off: frames presented\n" + r.Tail()).c_str());
				Assert::IsTrue(r.Logged("Capture path gpu: V210 unpacked on the GPU"), (L"off: the run's log is there\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("Interpolation:"), (L"off by default\n" + r.Tail()).c_str());
				Assert::IsFalse(r.Logged("De-judder: motion estimated"), (L"off: no estimate\n" + r.Tail()).c_str());
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(InterpolationLowLatencyFromTheFirstFrame)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(InterpolationLowLatencyFromTheFirstFrame)
		{
			const HarnessScope harnessScope;
			// Zero queue / low latency: never interpolate, never wait for B (today's output),
			// from the first frame (the queue profile applied before Build, as the app does).
			if (!RequireHardwareOrSkip("InterpolationLowLatencyFromTheFirstFrame")) return;
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
			// interpolated=1 lines come from the live extras planner (on screen only); off screen the
			// anchor is the capture path's line, so the negative checks read a real log.
			Assert::IsTrue(r.Logged("Capture path gpu: V210 unpacked on the GPU"), (L"the run's log is there\n" + r.Tail()).c_str());
			Assert::IsFalse(r.Logged("interpolated=1"), (L"no interpolated picture\n" + r.Tail()).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(InterpolationStreamFaultsFallBackAndRecover)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(InterpolationStreamFaultsFallBackAndRecover)
		{
			const HarnessScope harnessScope;
			// Missed frames and a resync during interpolation: the vectors are invalidated
			// (MotionValidity), the interpolator holds, and it estimates again on a fresh pair.
			if (!RequireHardwareOrSkip("InterpolationStreamFaultsFallBackAndRecover")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(InterpolationIsExclusiveWithSmoothMotion)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(InterpolationIsExclusiveWithSmoothMotion)
		{
			const HarnessScope harnessScope;
			// One motion engine (owner 2026-10-15): with libplacebo's smooth motion selected,
			// VP's interpolation stays off (no estimate, no vectors).
			if (!RequireHardwareOrSkip("InterpolationIsExclusiveWithSmoothMotion")) return;
			const FramePlan plan = RatePlan(HarnessMonitorHz() * 5.0 / 6.0);
			const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 4.0, false,
				"interpolation: auto\n[vprenderer.motion]\nmotion_engine: smooth_motion\nsmooth_motion: always\n", false, &plan);
			ReportInterpolation("smooth motion selected", r);
			Assert::IsTrue(r.presented, (L"frames presented\n" + r.Tail()).c_str());
			Assert::IsTrue(r.Logged("Motion engine: libplacebo smooth motion"), (L"smooth motion is the engine\n" + r.Tail()).c_str());
			Assert::IsFalse(r.Logged("Interpolation: interpolating"), (L"VP's interpolation off\n" + r.Tail()).c_str());
			Assert::IsFalse(r.Logged("De-judder: motion estimated"), (L"no estimate\n" + r.Tail()).c_str());
			Assert::IsTrue(r.presented);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(InterpolationTimelineAtEveryRatio)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(InterpolationTimelineAtEveryRatio)
		{
			const HarnessScope harnessScope;
			// Stage B: the per-refresh timeline engages above and below 1 (5/6 and 6/5 of the
			// display rate: 50p on 60 Hz, 60p on 50 Hz, whatever the display); below 1 it skips
			// source frames instead of letting drift correction drop them; no failures.
			if (!RequireHardwareOrSkip("InterpolationTimelineAtEveryRatio")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(InterpolationGapFillPairsAcrossMissedFrames)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(InterpolationGapFillPairsAcrossMissedFrames)
		{
			const HarnessScope harnessScope;
			// interpolation_fill_gaps: on - a pair across one or two missed capture frames
			// (the timeline spans them); off (default) - no pair across the gap.
			if (!RequireHardwareOrSkip("InterpolationGapFillPairsAcrossMissedFrames")) return;
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

		BEGIN_TEST_METHOD_ATTRIBUTE(InterpolationOnScreenFillsTheRepeatRefreshes)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuOnScreen")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(InterpolationOnScreenFillsTheRepeatRefreshes)
		{
			const HarnessScope harnessScope;
			// On the screen only (VP_TEST_HARNESS_ONSCREEN=1, real refresh statistics): at
			// 5/6 of the display rate one refresh in six would repeat a frame; Stage A shows
			// an interpolated picture there (the extras planner's interpolated=1 lines).
			char* v = nullptr;
			size_t n = 0;
			const bool on = _dupenv_s(&v, &n, "VP_TEST_HARNESS_ONSCREEN") == 0 && v && std::string(v) == "1";
			std::free(v);
			if (!on)
			{
				Logger::WriteMessage("SKIP-OPT-IN InterpolationOnScreenFillsTheRepeatRefreshes: VP_TEST_HARNESS_ONSCREEN=1 only; not run\n");
				return;
			}
			if (!RequireHardwareOrSkip("InterpolationOnScreenFillsTheRepeatRefreshes")) return;
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

		// The opt-in for the on-screen runs (VP_TEST_HARNESS_ONSCREEN=1).
		static bool OnScreenOptIn(const char* name)
		{
			char* v = nullptr;
			size_t n = 0;
			const bool on = _dupenv_s(&v, &n, "VP_TEST_HARNESS_ONSCREEN") == 0 && v && std::string(v) == "1";
			std::free(v);
			if (!on)
				Logger::WriteMessage((std::string("SKIP-OPT-IN ") + name + ": VP_TEST_HARNESS_ONSCREEN=1 only; not run\n").c_str());
			return on;
		}

		// The count before a word in a log line ("..., 3 late, ..." -> 3); -1 when absent.
		static long long CountBefore(const std::string& line, const std::string& word)
		{
			const size_t at = line.find(" " + word);
			if (at == std::string::npos || at == 0)
				return -1;
			size_t begin = at;
			while (begin > 0 && std::isdigit(static_cast<unsigned char>(line[begin - 1])))
				--begin;
			return begin == at ? -1 : std::atoll(line.substr(begin, at - begin).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(OnScreenRenderAheadExtras)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuOnScreen")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OnScreenRenderAheadExtras)
		{
			const HarnessScope harnessScope;
			// docs/VP-render-ahead-design.md (on-screen, Claude on the GPU, overnight): 23.976p with
			// the de-judder and render_ahead: on - the gate per source period and the old per-refresh
			// rule, standard and gpu paths, plus the VP-owned DXGI presenter (the deeper swapchain
			// applies only there), and render ahead off as the reference. A report per run: the
			// "Render-ahead:" and "Render-ahead gate:" lines, the swapchain line, late frames and the
			// presentation status. VP_TEST_HARNESS_ONSCREEN=1 with VP_TEST_GPU=hardware only.
			if (!OnScreenOptIn("OnScreenRenderAheadExtras")) return;
			if (!RequireHardwareOrSkip("OnScreenRenderAheadExtras")) return;
			char* sv = nullptr;
			size_t n = 0;
			const double seconds = _dupenv_s(&sv, &n, "VP_TEST_HARNESS_SECONDS") == 0 && sv ? std::atof(sv) : 30.0;
			std::free(sv);
			const std::string dejudder = "dejudder_strength: 0.25\n";
			// The unified section: a legacy [vpvr.display] next to [vprenderer.input.*] made the
			// renderer ignore the whole unified configuration (VP-0079), so that case ran without
			// the de-judder or render_ahead at all (render-ahead follow-up 2026-10-17).
			const std::string vpOwned = "[vprenderer]\ndiagnostic_vp_owned_dxgi_presenter: true\n";
			struct Case { const char* name; const char* path; std::string extra; bool renderAhead; };
			const Case cases[] = {
				{ "23.976p standard, render ahead, gate period", "standard", dejudder + "render_ahead: on\ndejudder_gate: period\n", true },
				{ "23.976p gpu, render ahead, gate period", "gpu", dejudder + "render_ahead: on\ndejudder_gate: period\n", true },
				{ "23.976p standard, render ahead, gate refresh", "standard", dejudder + "render_ahead: on\ndejudder_gate: refresh\n", true },
				{ "23.976p gpu, render ahead, gate refresh", "gpu", dejudder + "render_ahead: on\ndejudder_gate: refresh\n", true },
				{ "23.976p gpu, render ahead, gate auto, VP-owned presenter", "gpu",
					dejudder + "render_ahead: on\ndejudder_gate: auto\n" + vpOwned, true },
				{ "23.976p gpu, render ahead off (reference)", "gpu", dejudder + "render_ahead: off\n", false } };
			// Strict (render-ahead follow-up 2026-10-17; the laptop's internal panel passed vacuously
			// with every frame held "timing not known: display rate"). A render_ahead case FAILS unless:
			// "Render-ahead:" lines > 0 with the gate level in them; queued extras > 0, or the gate
			// dropped/held them (reported with its reason); no "timing not known: display rate" hold
			// at all (render-ahead takes an open-loop rate without statistics), and other
			// "timing not known" holds at most max(24, 10% of the frames) (start-up only). The
			// render_ahead off reference keeps today's checks. VP_TEST_HARNESS_FULLSCREEN=1|both:
			// the fullscreen pass, reported separately.
			int failures = 0;
			const double runSeconds = seconds > 5.0 ? seconds : 30.0;
			// VP_TEST_RENDER_AHEAD_CASE: only the cases whose name contains it (diagnostics; a
			// filtered run is not the gate and says so).
			std::string caseFilter;
			{
				char* cf = nullptr;
				size_t cfn = 0;
				if (_dupenv_s(&cf, &cfn, "VP_TEST_RENDER_AHEAD_CASE") == 0 && cf) caseFilter = cf;
				std::free(cf);
			}
			if (!caseFilter.empty())
				Logger::WriteMessage(("FILTERED RUN (not the gate): cases containing \"" + caseFilter + "\"").c_str());
			for (const int mode : OnScreenModes())
			{
			g_harnessFullscreen = mode;
			const std::string modeName = OnScreenModeName(mode);
			int modeFailures = 0;
			for (const Case& c : cases)
			{
				if (!caseFilter.empty() && std::string(c.name).find(caseFilter) == std::string::npos)
					continue;
				const RunResult r = Run(VideoFrameEncoding::V210, c.path, runSeconds, true, c.extra, true);
				std::string trendReport;
				// Latency trend (report): the capture-to-present delay's median per fifth of the run
				// after the first, the queue depth and the frames dropped in the second half.
				{
					std::string trendLine = "   exit latency ms by fifth:";
					uint64_t droppedMid = 0, droppedEnd = 0;
					for (int fifth = 1; fifth < 5; ++fifth)
					{
						std::vector<double> latency;
						std::vector<size_t> depth;
						for (const RunResult::TrendSample& t : r.trend)
							if (t.seconds >= runSeconds * fifth / 5.0 && t.seconds < runSeconds * (fifth + 1) / 5.0)
							{
								latency.push_back(t.exitLatencyMs);
								depth.push_back(t.queueDepth);
							}
						char text[96];
						sprintf_s(text, " %.1f (queue %.1f)", Median(latency), Median(depth));
						trendLine += text;
					}
					for (const RunResult::TrendSample& t : r.trend)
					{
						if (t.seconds < runSeconds * 0.5) droppedMid = t.dropped;
						droppedEnd = t.dropped;
					}
					trendReport = trendLine + "; dropped frames, second half " + std::to_string(droppedEnd - droppedMid) +
						" (total " + std::to_string(droppedEnd) + ")";
				}
				Logger::WriteMessage((std::string("== [") + modeName + "] " + c.name + ": presented " + (r.presented ? "yes" : "NO")).c_str());
				Logger::WriteMessage(("   de-judder: " + r.dejudderStatus).c_str());
				Logger::WriteMessage(("   presentation: " + r.presentationStatus).c_str());
				Logger::WriteMessage(trendReport.c_str());
				std::string lastRenderAhead;
				int gateChanges = 0, renderAheadLines = 0, renderFailures = 0;
				for (const std::string& line : r.log)
				{
					if (line.find("Render-ahead gate:") != std::string::npos)
					{
						++gateChanges;
						Logger::WriteMessage(("   " + line).c_str());
					}
					else if (line.find("Render-ahead swapchain:") != std::string::npos || line.find("VP_TEST_ADAPTER") != std::string::npos ||
						line.find("VP-owned DXGI presenter") != std::string::npos)
						Logger::WriteMessage(("   " + line).c_str());
					else if (line.find("Render-ahead:") != std::string::npos)
					{
						++renderAheadLines;
						lastRenderAhead = line;
					}
					if (line.find("extra render failed") != std::string::npos || line.find("extra present failed") != std::string::npos)
						++renderFailures;
				}
				Logger::WriteMessage(("   last " + (lastRenderAhead.empty() ? std::string("Render-ahead: (none)") : lastRenderAhead)).c_str());
				Logger::WriteMessage(("   render-ahead lines " + std::to_string(renderAheadLines) + ", gate changes " +
					std::to_string(gateChanges) + ", late " + std::to_string(CountBefore(lastRenderAhead, "late,")) +
					", extras dropped " + std::to_string(CountBefore(lastRenderAhead, "extras dropped")) +
					", render/present failures " + std::to_string(renderFailures)).c_str());
				if (!c.renderAhead && renderAheadLines > 0)
					Logger::WriteMessage("   UNEXPECTED: render-ahead lines with render_ahead off");
				int caseFailures = (r.presented ? 0 : 1) + (renderFailures > 0 ? 1 : 0) + (!c.renderAhead && renderAheadLines > 0 ? 1 : 0);
				if (c.renderAhead)
				{
					int queuedExtras = 0;
					std::string rateLine, lastGateLine;
					for (const std::string& line : r.log)
					{
						if (line.find("De-judder extra:") != std::string::npos && line.find("queued=1") != std::string::npos)
							++queuedExtras;
						if (line.find("Render-ahead: open loop") != std::string::npos ||
							line.find("Render-ahead: closed loop") != std::string::npos ||
							line.find("Render-ahead: no display rate") != std::string::npos)
							rateLine = line;
						if (line.find("Render-ahead gate:") != std::string::npos)
							lastGateLine = line;
					}
					const std::map<std::string, long long> holds = HoldCounts(r.log);
					long long displayRateHolds = 0, otherTimingHolds = 0;
					for (const auto& hold : holds)
					{
						Logger::WriteMessage(("   hold: " + hold.first + " x" + std::to_string(hold.second)).c_str());
						if (hold.first == "timing not known: display rate")
							displayRateHolds += hold.second;
						else if (hold.first.find("timing not known") == 0)
							otherTimingHolds += hold.second;
					}
					const auto refreshGateHolds = holds.find("frame work too long for extras (the next frame could be late)");
					const long long timingHoldLimit = std::max(24LL, static_cast<long long>(0.1 * runSeconds * 24000.0 / 1001.0));
					const bool engaged = renderAheadLines > 0;
					const bool gateLogged = engaged && lastRenderAhead.find(", gate ") != std::string::npos;
					const long long dropped = CountBefore(lastRenderAhead, "extras dropped");
					const bool gateHold = lastRenderAhead.find(", gate hold") != std::string::npos ||
						lastGateLine.find("-> hold") != std::string::npos;
					const bool refreshRuleHeld = std::string(c.extra).find("dejudder_gate: refresh") != std::string::npos &&
						refreshGateHolds != holds.end() && refreshGateHolds->second > 0;
					const bool gateExplains = dropped > 0 || gateHold || refreshRuleHeld;
					Logger::WriteMessage(("   rate: " + (rateLine.empty() ? std::string("(no display-rate line)") : rateLine)).c_str());
					Logger::WriteMessage(("   queued extras logged " + std::to_string(queuedExtras) + ", display-rate holds " +
						std::to_string(displayRateHolds) + ", other timing holds " + std::to_string(otherTimingHolds) +
						" (limit " + std::to_string(timingHoldLimit) + ")").c_str());
					std::vector<std::string> why;
					if (displayRateHolds > 0)
						why.push_back("held for want of a display rate (" + std::to_string(displayRateHolds) + " holds)");
					if (otherTimingHolds > timingHoldLimit)
						why.push_back("timing-not-known hold storm (" + std::to_string(otherTimingHolds) + " holds)");
					if (!engaged && !refreshRuleHeld)
						why.push_back("no Render-ahead: line (render-ahead never engaged)");
					if (c.extra.find(vpOwned) != std::string::npos && !r.Logged("VP-owned DXGI swapchain: created"))
						why.push_back("the VP-owned presenter was not created");
					if (engaged && !gateLogged)
						why.push_back("the gate level is not in the Render-ahead: line");
					if (queuedExtras == 0 && !gateExplains)
						why.push_back("no queued extras and the gate did not drop or hold them");
					if (queuedExtras == 0 && gateExplains)
						Logger::WriteMessage(("   GATE-DROPPED: no queued extras; reason: " + (refreshRuleHeld ?
							std::string("dejudder_gate refresh held (frame work too long for extras), ") +
								std::to_string(refreshGateHolds->second) + " holds" :
							!lastGateLine.empty() ? lastGateLine : "extras dropped " + std::to_string(dropped))).c_str());
					for (const std::string& w : why)
						Logger::WriteMessage(("   FAIL: " + w).c_str());
					caseFailures += why.empty() ? 0 : 1;
				}
				Logger::WriteMessage(("   [" + modeName + "] " + (caseFailures == 0 ? "PASS" : "FAIL")).c_str());
				modeFailures += caseFailures;
			}
			Logger::WriteMessage(("-- [" + modeName + "] OnScreenRenderAheadExtras: " + std::to_string(modeFailures) +
				" failure(s)").c_str());
			failures += modeFailures;
			}
			Assert::AreEqual(0, failures, L"render_ahead engaged with extras (or a gate reason) in every mode, no timing-hold storm, "
				L"no extra failed, off logs no render-ahead");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(OnScreenHiccupRepairWithoutInterpolation)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuOnScreen")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OnScreenHiccupRepairWithoutInterpolation)
		{
			const HarnessScope harnessScope;
			// Standalone hiccup repair (render-ahead 2026-10-17): interpolation off and
			// interpolation_fill_gaps on, a stream that misses one and then two frames - at 5/6 of
			// the display rate and at 23.976p with the de-judder - and the fill off as the
			// reference. A report: the "gap pair across" lines, the timeline lines and the
			// presentation status. VP_TEST_HARNESS_ONSCREEN=1 with VP_TEST_GPU=hardware only.
			if (!OnScreenOptIn("OnScreenHiccupRepairWithoutInterpolation")) return;
			if (!RequireHardwareOrSkip("OnScreenHiccupRepairWithoutInterpolation")) return;
			const auto missed = [](int n) { FrameStep f; f.content = n; f.send = !(n == 90 || (n >= 150 && n <= 151)); return f; };
			FramePlan video = RatePlan(HarnessMonitorHz() * 5.0 / 6.0);
			video.step = missed;
			FramePlan film;
			film.rateNumerator = 24000;
			film.rateDenominator = 1001;
			film.step = missed;
			struct Case { const char* name; const FramePlan* plan; bool film; std::string extra; bool fill; };
			const Case cases[] = {
				{ "5/6 display rate, fill on", &video, false, "interpolation: off\ninterpolation_fill_gaps: on\n", true },
				{ "5/6 display rate, fill off", &video, false, "interpolation: off\ninterpolation_fill_gaps: off\n", false },
				{ "23.976p de-judder, fill on", &film, true, "interpolation: off\ninterpolation_fill_gaps: on\ndejudder_strength: 0.25\n", true },
				{ "23.976p de-judder, fill off", &film, true, "interpolation: off\ninterpolation_fill_gaps: off\ndejudder_strength: 0.25\n", false } };
			int failures = 0;
			for (const Case& c : cases)
			{
				const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 10.0, c.film, c.extra, true, c.plan);
				ReportInterpolation(c.name, r);
				Logger::WriteMessage(("   presented " + std::string(r.presented ? "yes" : "NO") + "; presentation: " +
					r.presentationStatus).c_str());
				int gapPairs = 0, missedLines = 0;
				for (const std::string& line : r.log)
				{
					if (line.find("gap pair across") != std::string::npos) ++gapPairs;
					if (line.find("capture missed") != std::string::npos)
					{
						++missedLines;
						Logger::WriteMessage(("   " + line).c_str());
					}
				}
				Logger::WriteMessage(("   gap pair lines " + std::to_string(gapPairs) + ", capture missed lines " +
					std::to_string(missedLines)).c_str());
				if (!c.fill && gapPairs > 0)
					Logger::WriteMessage("   UNEXPECTED: gap pairs with interpolation_fill_gaps off");
				if (c.fill && gapPairs == 0)
					Logger::WriteMessage("   NOTE: no gap pair with the fill on (report for the owner's judgement)");
				failures += (r.presented ? 0 : 1) + (missedLines > 0 ? 0 : 1) + (!c.fill && gapPairs > 0 ? 1 : 0) +
					(r.Logged("Interpolation: failed") ? 1 : 0);
			}
			Assert::AreEqual(0, failures, L"every run presented, the gaps were counted, off made no pair, no failure");
		}

		// --- Rule tests (test hardening 2026-10-17) ---

		BEGIN_TEST_METHOD_ATTRIBUTE(InterpolationOffIsPixelIdenticalAtAWholeRatio)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(InterpolationOffIsPixelIdenticalAtAWholeRatio)
		{
			// Rule: interpolation off changes nothing. A still picture at the display's own rate (a
			// whole ratio, where auto passes through): the presented codes (VP-0133 raw_r10_sha256,
			// dither and deband off) with no interpolation key, with interpolation: off and with
			// interpolation: auto are identical.
			const HarnessScope harnessScope;
			if (!RequireHardwareOrSkip("InterpolationOffIsPixelIdenticalAtAWholeRatio")) return;
			const std::wstring shots = HarnessTempFolder(L"vp-interpolation-off");
			SetEnvironmentVariableW(L"VP_TEST_SCREENSHOT_DIR", shots.c_str());
			FramePlan plan = RatePlan(HarnessMonitorHz());
			plan.captureAtSeconds = 2.5;
			plan.step = [](int) { return FrameStep{ true, 0, 0 }; };   // one still picture
			auto hashOf = [&](const char* extra, const wchar_t* name, RunResult& r) {
				r = Run(VideoFrameEncoding::V210, "gpu", 3.5, false,
					std::string(extra) + "[vprenderer.Default]\ndithering: off\ndeband: off\n", false, &plan);
				Assert::IsTrue(r.presented, (std::wstring(name) + L": frames presented\n" + r.Tail()).c_str());
				const std::string hash = CaptureHash(r);
				Assert::IsFalse(hash.empty(), (std::wstring(name) + L": capture written with its hash\n" + r.Tail()).c_str());
				Logger::WriteMessage((std::string(name, name + wcslen(name)) + ": " + hash + "\n").c_str());
				return hash;
			};
			RunResult none, again, off, autoRun;
			const std::string base = hashOf("", L"no key", none);
			Assert::AreEqual(base, hashOf("", L"no key again", again), L"the capture is deterministic");
			Assert::AreEqual(base, hashOf("interpolation: off\n", L"interpolation: off", off), L"off: identical codes");
			Assert::AreEqual(base, hashOf("interpolation: auto\n", L"interpolation: auto (whole ratio)", autoRun),
				L"auto at a whole ratio: identical codes");
			Assert::IsTrue(autoRun.Logged("Interpolation: whole ratio: passes through"),
				(L"auto: the whole ratio was recognised (anchor)\n" + autoRun.Tail()).c_str());
			Assert::IsFalse(off.Logged("Interpolation: interpolating") || autoRun.Logged("Interpolation: interpolating"),
				L"nothing interpolated");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(BtbAndWtwSurviveTheRendererEndToEnd)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BtbAndWtwSurviveTheRendererEndToEnd)
		{
			// Owner rule: below black and above white are kept in all cases. A V210 picture of four
			// flat bars - codes 4 and 40 (below black), 980 and 1019 (above white), chroma neutral -
			// through the whole renderer (Run) to a limited-range output: the captured R10 codes of
			// every channel inside each bar are within 2 codes of the input.
			const HarnessScope harnessScope;
			if (!RequireHardwareOrSkip("BtbAndWtwSurviveTheRendererEndToEnd")) return;
			const int w = g_harnessWidth, h = g_harnessHeight;
			const int codes[4] = { 4, 40, 980, 1019 };
			g_harnessLumaPicture.assign(static_cast<size_t>(w) * h, 0);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					g_harnessLumaPicture[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(codes[(std::min)(3, x * 4 / w)]);
			const std::wstring shots = HarnessTempFolder(L"vp-btb-wtw");
			SetEnvironmentVariableW(L"VP_TEST_SCREENSHOT_DIR", shots.c_str());
			FramePlan plan;
			plan.captureAtSeconds = 2.5;
			plan.step = [](int) { return FrameStep{ true, 0, 0 }; };
			const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 3.5, false,
				"[vprenderer.Default]\ndithering: off\ndeband: off\noutput_range: limited\n", false, &plan);
			Assert::IsTrue(r.presented, (L"frames presented\n" + r.Tail()).c_str());
			Assert::IsFalse(r.captureMetadata.empty(), (L"capture written\n" + r.Tail()).c_str());
			int cw = 0, ch = 0;
			const std::vector<int> rgb = LoadCaptureRgb(r.captureMetadata, cw, ch);
			Assert::IsFalse(rgb.empty(), L"capture PNG read");
			std::string report;
			int worst = 0;
			for (int bar = 0; bar < 4; ++bar)
			{
				// The bar's inside (away from the scaler's edges), the middle half of the rows.
				const int x0 = cw * bar / 4 + cw / 16, x1 = cw * (bar + 1) / 4 - cw / 16;
				int lo[3] = { 1 << 20, 1 << 20, 1 << 20 }, hi[3] = { -1, -1, -1 };
				for (int y = ch / 4; y < ch * 3 / 4; ++y)
					for (int x = x0; x < x1; ++x)
						for (int c = 0; c < 3; ++c)
						{
							const int v = rgb[(static_cast<size_t>(y) * cw + x) * 3 + c];
							lo[c] = (std::min)(lo[c], v);
							hi[c] = (std::max)(hi[c], v);
							worst = (std::max)(worst, std::abs(v - codes[bar]));
						}
				char line[200];
				snprintf(line, sizeof(line), "bar %d (in %d): R %d..%d G %d..%d B %d..%d\n", bar, codes[bar], lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
				report += line;
			}
			Logger::WriteMessage(report.c_str());
			Assert::IsTrue(worst <= 2, HarnessWide("BTB/WTW not kept end to end (worst " + std::to_string(worst) + " codes)\n" + report).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(LowLatencyQueueDepthAndDelayAreMeasured)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LowLatencyQueueDepthAndDelayAreMeasured)
		{
			// Low latency, measured (not only log lines): with the low-latency queue profile (target 1
			// frame, as the app hands target_frames to the renderer) the renderer's queue depth and its
			// capture-to-output delay (ExitLatencyMs), sampled through the second half of the run, are
			// lower than with the Normal profile (target 5): at most 2 frames queued, and at least one
			// source frame period less delay.
			const HarnessScope harnessScope;
			if (!RequireHardwareOrSkip("LowLatencyQueueDepthAndDelayAreMeasured")) return;
			g_harnessApplySnapshot = true;
			g_harnessQueueTarget = 1;
			const RunResult low = Run(VideoFrameEncoding::V210, "gpu", 6.0, true,
				"dejudder_strength: 0.25\n[queue.Low_Latency]\nqueue_size: 4\ntarget_frames: 1\n");
			g_harnessQueueTarget = 5;
			const RunResult normal = Run(VideoFrameEncoding::V210, "gpu", 6.0, true,
				"dejudder_strength: 0.25\n[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n");
			const double period = 1001.0 / 24.0;   // the run's source frame period (23.976p), ms
			const double lowQueue = Median(low.queueDepths), normalQueue = Median(normal.queueDepths);
			const double lowDelay = Median(low.exitLatencyMs), normalDelay = Median(normal.exitLatencyMs);
			char line[300];
			snprintf(line, sizeof(line), "queue depth median: low %.1f, normal %.1f (%zu / %zu samples); exit latency median: low %.1f ms, normal %.1f ms\n",
				lowQueue, normalQueue, low.queueDepths.size(), normal.queueDepths.size(), lowDelay, normalDelay);
			Logger::WriteMessage(line);
			Assert::IsTrue(low.presented && normal.presented, L"both runs presented");
			Assert::IsTrue(low.Logged("low-latency queue"), (L"the low-latency path ran (anchor)\n" + low.Tail()).c_str());
			Assert::IsFalse(normal.Logged("low-latency queue"), (L"the Normal queue is regular\n" + normal.Tail()).c_str());
			Assert::IsTrue(low.queueDepths.size() > 10 && normal.queueDepths.size() > 10, L"samples taken");
			Assert::IsTrue(lowQueue <= 2.0, HarnessWide(std::string("low latency: queue depth too deep\n") + line).c_str());
			Assert::IsTrue(normalQueue > lowQueue, HarnessWide(std::string("the measure sees the deeper Normal queue\n") + line).c_str());
			Assert::IsTrue(lowDelay > 0.0 && normalDelay > 0.0, HarnessWide(std::string("exit latency measured\n") + line).c_str());
			Assert::IsTrue(lowDelay + period <= normalDelay,
				HarnessWide(std::string("low latency: at least one frame period less delay\n") + line).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(MixedScreenNativeUiOverAnUpscaleIsNotRebased)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MixedScreenNativeUiOverAnUpscaleIsNotRebased)
		{
			// Mixed screen through the renderer (source_resolution auto, evidence shortened to 1
			// shot / 2 s): a moving 1080p picture upscaled 2x to 4K with cuts is re-based (the
			// positive control: the check can see a re-base); the same picture with a static native
			// 4K UI over its top half (a menu over the video) is never re-based.
			const HarnessScope harnessScope;
			if (!RequireHardwareOrSkip("MixedScreenNativeUiOverAnUpscaleIsNotRebased")) return;
			const int mw = 1920, mh = 1080, w = 3840, h = 2160;
			std::vector<double> master(static_cast<size_t>(mw) * mh);
			{
				std::mt19937 rng(11);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				struct Wave { double fx, fy, phase, amp; };
				std::vector<Wave> waves;
				for (int i = 0; i < 40; ++i)
				{
					const double f = 0.01 * std::pow(0.45 / 0.01, u(rng)), a = u(rng) * 3.14159265;
					waves.push_back({ f * std::cos(a), f * std::sin(a), u(rng) * 6.2831853, 0.02 / std::sqrt(f) });
				}
				for (int y = 0; y < mh; ++y)
					for (int x = 0; x < mw; ++x)
					{
						double v = 0.45;
						for (const Wave& wv : waves) v += wv.amp * std::cos(6.2831853 * (wv.fx * x + wv.fy * y) + wv.phase);
						master[static_cast<size_t>(y) * mw + x] = (std::min)(0.95, (std::max)(0.05, v));
					}
			}
			// The player's 2x Lanczos-3 (separable), as limited-range 10-bit codes.
			std::vector<uint16_t> upscaled(static_cast<size_t>(w) * h);
			{
				auto lanczos = [](double x) { x = std::fabs(x); if (x < 1e-9) return 1.0; if (x >= 3.0) return 0.0;
					const double px = 3.14159265358979 * x; return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px); };
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
				std::vector<double> rows(static_cast<size_t>(w) * mh), up(static_cast<size_t>(w) * h);
				for (int y = 0; y < mh; ++y) line(&master[static_cast<size_t>(y) * mw], mw, 1, &rows[static_cast<size_t>(y) * w], 1);
				for (int x = 0; x < w; ++x) line(&rows[x], mh, w, &up[x], w);
				for (size_t i = 0; i < up.size(); ++i)
					upscaled[i] = static_cast<uint16_t>(std::lround(64.0 + (std::min)(1.0, (std::max)(0.0, up[i])) * 876.0));
			}
			// The native UI: a panel with rows of 1-2 pixel glyph strokes (4K detail), top half.
			std::vector<uint16_t> mixed = upscaled;
			{
				std::mt19937 rng(23);
				for (int y = 0; y < h / 2; ++y)
					for (int x = 0; x < w; ++x)
						mixed[static_cast<size_t>(y) * w + x] = 180;
				for (int row = 0; row * 48 + 60 < h / 2; ++row)
					for (int glyph = 0; glyph * 18 + 80 < w - 80; ++glyph)
					{
						const unsigned long long bits = (static_cast<unsigned long long>(rng()) << 32) | rng();
						for (int gy = 0; gy < 7; ++gy)
							for (int gx = 0; gx < 5; ++gx)
							{
								if (!((bits >> (gy * 5 + gx)) & 1ull)) continue;
								for (int py = 0; py < 3; ++py)
									for (int px = 0; px < 2; ++px)
									{
										const int x = 80 + glyph * 18 + gx * 2 + px, y = 60 + row * 48 + gy * 3 + py;
										mixed[static_cast<size_t>(y) * w + x] = 900;
									}
							}
					}
			}
			g_harnessWidth = w;
			g_harnessHeight = h;
			g_harnessLumaShift = 2;     // 1 master pixel per frame: still an exact 2x upscale
			g_harnessShotFrames = 36;   // a cut (brightness jump) every 1.5 s at 24p
			const FramePlan plan = RatePlan(24.0);
			const std::string settings = "source_resolution: auto\nsource_resolution_evidence_shots: 1\nsource_resolution_evidence_seconds: 2\n";
			auto report = [](const char* name, const RunResult& r) {
				Logger::WriteMessage((std::string(name) + ": " + r.sourceStatus + "\n").c_str());
				for (const std::string& line : r.log)
					if (line.find("Re-base") != std::string::npos || line.find("Source resolution") != std::string::npos)
						Logger::WriteMessage(("   " + line + "\n").c_str());
			};
			g_harnessLumaPicture = upscaled;
			g_harnessLumaStaticRows = 0;
			const RunResult plain = Run(VideoFrameEncoding::V210, "gpu", 14.0, false, settings, false, &plan);
			report("upscale", plain);
			g_harnessLumaPicture = mixed;
			g_harnessLumaStaticRows = h / 2;
			const RunResult menu = Run(VideoFrameEncoding::V210, "gpu", 14.0, false, settings, false, &plan);
			report("native UI over the upscale", menu);
			Assert::IsTrue(plain.presented && menu.presented, L"both runs presented");
			Assert::IsTrue(plain.Logged("Re-base: 3840x2160 ->"),
				(L"positive control: the plain 2x upscale is re-based under auto\n" + plain.Tail()).c_str());
			Assert::IsTrue(menu.sourceStatus.find("source_resolution auto") != std::string::npos &&
				menu.sourceStatus.find("not read") == std::string::npos, (L"the mixed screen was read (anchor)\n" + menu.Tail()).c_str());
			Assert::IsFalse(menu.Logged("Re-base:"), (L"native UI over half the screen: never re-based\n" + menu.Tail()).c_str());
		}

		// Automatic calibration (docs/calibration/AUTO-CALIBRATION.md), GPU only
		// (VP_TEST_GPU=hardware): patches through the running renderer. The
		// backbuffer is read back (PatternReadback, 10-bit swapchains) to prove the
		// codes are exact - no sigma-delta, dither, LUT or tone mapping on the patch -
		// for full field, a 10% window with its surround, and a BTB code; then video
		// resumes.
		BEGIN_TEST_METHOD_ATTRIBUTE(CalibrationPatternsAreExactCodesInTheBackbuffer)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CalibrationPatternsAreExactCodesInTheBackbuffer)
		{
			const HarnessScope harnessScope;
			if (!RequireHardwareOrSkip("CalibrationPatternsAreExactCodesInTheBackbuffer")) return;
			struct Shot
			{
				Calibration::PatternRequest request;
				Calibration::PatternOutputInfo after;
				bool shown = false, checked = false;
				std::string error;
			};
			Calibration::PatternOutputInfo format;
			bool formatRead = false;
			Shot shots[3];
			bool ended = false;
			FramePlan plan;
			plan.tick = [&](IVideoRenderer& renderer, double t)
			{
				if (!formatRead && t >= 1.0)
				{
					formatRead = renderer.GetCalibrationOutput(format);
					if (!formatRead) return;
					std::string reason;
					if (!Calibration::PatternFormatSupported(format, reason)) return;
					const double grey = 0.5, btb = Calibration::LimitedCodeToSignal(4);
					const Calibration::PatchCodes white = Calibration::CodesForSignal(format, { 1.0, 1.0, 1.0 });
					const Calibration::PatchCodes mid = Calibration::CodesForSignal(format, { grey, grey * 0.5, grey * 0.25 });
					const Calibration::PatchCodes black = Calibration::CodesForSignal(format, { 0.0, 0.0, 0.0 });
					const Calibration::PatchCodes below = Calibration::CodesForSignal(format, { btb, btb, btb });
					for (int c = 0; c < 3; ++c)
					{
						shots[0].request.patch[c] = white.container[c];
						shots[1].request.patch[c] = mid.container[c];
						shots[1].request.background[c] = black.container[c];
						shots[2].request.patch[c] = format.limitedRange ? below.container[c] : black.container[c];
					}
					shots[1].request.windowAreaPercent = 10.0f;
				}
				for (int i = 0; i < 3; ++i)
				{
					const double at = 1.2 + 0.8 * i;
					if (formatRead && !shots[i].shown && t >= at)
					{
						shots[i].request.active = true;
						shots[i].request.generation = static_cast<uint64_t>(i + 1);
						shots[i].request.flags = Calibration::PatternReadback;
						CString reason;
						shots[i].shown = renderer.SetCalibrationPattern(shots[i].request, reason);
						shots[i].error = CStringA(reason).GetString();
					}
					if (shots[i].shown && !shots[i].checked && t >= at + 0.6)
					{
						renderer.GetCalibrationOutput(shots[i].after);
						shots[i].checked = true;
					}
				}
				if (!ended && shots[2].checked && t >= 4.0)
				{
					Calibration::PatternRequest off;
					CString reason;
					ended = renderer.SetCalibrationPattern(off, reason);
				}
			};
			const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 6.0, false, std::string(), false, &plan);
			Logger::WriteMessage(("format: container " + std::to_string(format.containerBits) + " code " +
				std::to_string(format.codeBits) + (format.limitedRange ? " limited" : " full") + " " +
				std::to_string(format.width) + "x" + std::to_string(format.height) + " " +
				std::to_string(format.refreshHz) + " Hz\n").c_str());
			Assert::IsTrue(r.presented, (L"video presented before the patterns\n" + r.Tail()).c_str());
			Assert::IsTrue(formatRead && format.available, L"GetCalibrationOutput reports the output");
			std::string reason;
			Assert::IsTrue(Calibration::PatternFormatSupported(format, reason), std::wstring(reason.begin(), reason.end()).c_str());
			for (int i = 0; i < 3; ++i)
			{
				const Shot& s = shots[i];
				Assert::IsTrue(s.shown, std::wstring(s.error.begin(), s.error.end()).c_str());
				Assert::AreEqual(static_cast<uint64_t>(i + 1), s.after.presentedGeneration, L"the patch was presented");
				Assert::IsTrue(s.after.presentsOfGeneration >= 2, L"presented every refresh");
				if (format.containerBits != 10)
				{
					Logger::WriteMessage("SKIP-READBACK 8-bit swapchain: presentation checked, codes not read back\n");
					continue;
				}
				Assert::AreEqual(static_cast<uint64_t>(i + 1), s.after.readbackGeneration, L"read back");
				for (int c = 0; c < 3; ++c)
				{
					Assert::AreEqual(static_cast<int>(s.request.patch[c]), static_cast<int>(s.after.readbackCenter[c]),
						L"the centre pixel holds the exact patch code");
					const int cornerWant = i == 1 ? s.request.background[c] : s.request.patch[c];
					Assert::AreEqual(cornerWant, static_cast<int>(s.after.readbackCorner[c]),
						i == 1 ? L"the corner holds the surround" : L"full field reaches the corner");
				}
			}
			Assert::IsTrue(ended, L"patterns ended");
			Assert::IsTrue(r.Logged("Calibration patterns: started"), r.Tail().c_str());
			Assert::IsTrue(r.Logged("Calibration patterns: ended"), r.Tail().c_str());
		}

		// The whole session (Quick plan) through the running renderer with a
		// simulated meter reading the codes the renderer was asked to show: the
		// wizard's patch timing (present twice, then settle) and its end state
		// (patterns off, video back) on the real GPU. GPU only.
		BEGIN_TEST_METHOD_ATTRIBUTE(CalibrationDryRunThroughTheRenderer)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CalibrationDryRunThroughTheRenderer)
		{
			const HarnessScope harnessScope;
			if (!RequireHardwareOrSkip("CalibrationDryRunThroughTheRenderer")) return;
			char base[MAX_PATH];
			GetTempPathA(MAX_PATH, base);
			const std::string folder = std::string(base) + "vp-calibration-gpu-" + std::to_string(GetTickCount64());
			CreateDirectoryA(folder.c_str(), nullptr);
			const std::string config = folder + "\\VideoProcessor.cfg";
			{
				std::ofstream file(config, std::ios::binary);
				file << "[vprenderer.color.Rec709]\ncalibration_lut_enabled: false\ncalibration_lut_bt709: none\n";
			}
			std::shared_ptr<Calibration::RendererPatternSink> sink;
			std::shared_ptr<Calibration::CalibrationSession> session;
			std::unique_ptr<Calibration::ISessionClock> clock = Calibration::CreateSystemClock();
			std::thread worker;
			bool started = false;
			Calibration::SessionStatus final;
			FramePlan plan;
			plan.tick = [&](IVideoRenderer& renderer, double t)
			{
				if (!started && t >= 1.0)
				{
					started = true;
					IVideoRenderer* raw = &renderer;
					sink = std::make_shared<Calibration::RendererPatternSink>(
						[raw]() { return std::shared_ptr<IVideoRenderer>(raw, [](IVideoRenderer*) {}); });
					Calibration::CalibrationSettings settings;
					settings.plan = Calibration::PlanChoice::Quick;
					settings.warmupMinutes = 0;
					settings.settleMs = 40;
					settings.verifyAfter = false;
					settings.lutSize = 17;
					Calibration::SessionEnvironment env;
					env.vpRoot = folder;
					env.configPath = config;
					env.displayName = "gpu-dry-run";
					Calibration::RendererPatternSink* s = sink.get();
					std::unique_ptr<Calibration::IMeter> meter(new Calibration::SimulatedScreenMeter(
						[s](Calibration::Vec3& device) { return s->Showing(device); },
						Calibration::SimulatedDisplayParams(), Calibration::SimulatedMeterParams(), nullptr));
					session = std::make_shared<Calibration::CalibrationSession>(settings, env, std::move(meter), 0.002,
						*sink, *clock);
					session->Start();
					worker = std::thread([session]()
					{
						while (session->Step())
							if (session->Status().step == Calibration::SessionStep::Placement && session->Status().haveReading)
								session->Continue();
					});
				}
				if (started && worker.joinable() && (session->Finished() ||
					session->Status().step == Calibration::SessionStep::Review || t >= 28.0))
				{
					if (session->Status().step != Calibration::SessionStep::Review) session->Cancel();
					else session->Discard();
					worker.join();
					final = session->Status();
				}
			};
			const RunResult r = Run(VideoFrameEncoding::V210, "gpu", 30.0, false, std::string(), false, &plan);
			if (worker.joinable())
			{
				session->Cancel();
				worker.join();
			}
			Logger::WriteMessage(("dry run: " + std::string(Calibration::ToString(final.step)) + " measured " +
				std::to_string(final.measured) + " skipped " + std::to_string(final.skipped) + " format " + final.format +
				"\n" + final.message + "\n" + final.review + "\n").c_str());
			Assert::IsTrue(r.presented, r.Tail().c_str());
			Assert::IsTrue(final.step == Calibration::SessionStep::Discarded,
				(L"the dry run reached the review\n" + std::wstring(final.message.begin(), final.message.end())).c_str());
			Assert::IsTrue(final.measured > 50);
			Assert::IsTrue(r.Logged("Calibration patterns: ended"), r.Tail().c_str());
		}
	};
}
