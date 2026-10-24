// VpScaleRender: renders still luma pictures through the real VP renderer DLL (the shipped
// VideoProcessorVPRenderer.dll, driven as the player drives it, as in
// VideoProcessor-Test/RendererCaptureHarnessTests.cpp) to an off-screen window of the output
// size and saves VP's rendered-output capture (VP-0133: 16-bit PNG of the R10 output + JSON
// sidecar) per picture. Used to make the scaling-repair net's training inputs from VP's own
// hybrid scaling chain. Data preparation only: it never changes VP's deployed configuration.
//
// Usage: VpScaleRender.exe <job.txt>
// job.txt lines (space separated, paths without spaces or quoted not supported):
//   dll <VideoProcessorVPRenderer.dll>
//   config <cfg file the renderer loads>
//   shots <capture output folder>
//   source <w> <h> <rateNum> <rateDen>
//   window <w> <h>
//   hold <seconds per picture>       (capture requested at 90 % of it)
//   picture_seq <tag> <raw> <frames> [first]  (sequence mode: plays once, captures every frame it can from frame [first]; one line per capture)
//   result <results.txt>
//   shaders <profile> ...            (shader profiles selected after Build, e.g. standard.ssim_downscaler)
//   picture <tag> <raw file: n frames of w*h uint16 little-endian 10-bit luma codes> <n>
// Writes one line per picture: "<captured tag> <frame index> <png> <json> <source_seq> <requested tag> <ok|fail>".

#include "pch.h"

#include <DisplayMode.h>
#include <IRenderer.h>
#include <ITimingClock.h>
#include <VideoFrame.h>
#include <VideoState.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
	std::mutex g_logMutex;
	std::vector<std::string> g_log;
	FILE* g_logFile = nullptr;

	void __cdecl CaptureLog(const char* message)
	{
		std::lock_guard<std::mutex> guard(g_logMutex);
		g_log.emplace_back(message ? message : "");
		if (g_logFile) { fprintf(g_logFile, "%s\n", message ? message : ""); fflush(g_logFile); }
	}

	struct Callback : IRendererCallback
	{
		std::atomic<int> state{ -1 };
		void OnRendererState(RendererState s, uint32_t) override { state = static_cast<int>(s); }
		void OnRendererDetailString(const CString&, uint32_t) override {}
	};

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
		timingclocktime_t TimingClockNow() override { LARGE_INTEGER n{}; QueryPerformanceCounter(&n); return n.QuadPart; }
		timingclocktime_t TimingClockTicksPerSecond() const override { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }
		const TCHAR* TimingClockDescription() override { return TEXT("VpScaleRender QPC"); }
	};

	// V210 with the given luma (10-bit codes) and neutral chroma.
	void FillV210(std::vector<BYTE>& frame, const std::vector<uint16_t>& luma, int width, int height, size_t rowBytes)
	{
		for (int y = 0; y < height; ++y)
		{
			uint32_t* row = reinterpret_cast<uint32_t*>(frame.data() + rowBytes * y);
			const uint16_t* src = luma.data() + static_cast<size_t>(y) * width;
			auto L = [&](int x) { return static_cast<uint32_t>(src[x < width ? x : width - 1] & 1023); };
			const uint32_t c = 512;
			for (int pack = 0; pack < (width + 5) / 6; ++pack)
			{
				const int x = pack * 6;
				row[pack * 4 + 0] = c | (L(x) << 10) | (c << 20);
				row[pack * 4 + 1] = L(x + 1) | (c << 10) | (L(x + 2) << 20);
				row[pack * 4 + 2] = c | (L(x + 3) << 10) | (c << 20);
				row[pack * 4 + 3] = L(x + 4) | (c << 10) | (L(x + 5) << 20);
			}
		}
	}

	void Pump()
	{
		MSG msg;
		while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: VpScaleRender job.txt\n"); return 2; }
	std::string dllPath, configPath, shots, resultPath, logPath;
	int sw = 0, sh = 0, rn = 24000, rd = 1001, ww = 0, wh = 0;
	double hold = 2.0;
	bool noCapture = false;
	std::vector<std::string> requireHooks;
	std::string requireAdapter;
	struct Picture { std::string tag, path; int frames = 1; bool seq = false; int start = 1; };
	std::vector<Picture> pictures;
	std::vector<std::string> shaderProfiles;
	{
		std::ifstream job(argv[1]);
		std::string line;
		while (std::getline(job, line))
		{
			std::istringstream s(line);
			std::string k;
			s >> k;
			if (k == "dll") s >> dllPath;
			else if (k == "config") s >> configPath;
			else if (k == "shots") s >> shots;
			else if (k == "result") s >> resultPath;
			else if (k == "log") s >> logPath;
			else if (k == "source") s >> sw >> sh >> rn >> rd;
			else if (k == "window") s >> ww >> wh;
			else if (k == "hold") s >> hold;
			else if (k == "nocapture") noCapture = true;
			else if (k == "require_hook") { std::string v; s >> v; requireHooks.push_back(v); }
			else if (k == "require_adapter") s >> requireAdapter;
			else if (k == "picture") { Picture p; s >> p.tag >> p.path; if (!(s >> p.frames)) p.frames = 1; pictures.push_back(p); }
			else if (k == "picture_seq") { Picture p; p.seq = true; s >> p.tag >> p.path >> p.frames; if (!(s >> p.start)) p.start = 1; pictures.push_back(p); }
			else if (k == "shaders") { std::string v; while (s >> v) shaderProfiles.push_back(v); }
		}
	}
	if (dllPath.empty() || configPath.empty() || shots.empty() || resultPath.empty() || !sw || !sh || !ww || !wh || pictures.empty())
	{
		fprintf(stderr, "incomplete job\n");
		return 2;
	}
	if (!logPath.empty()) fopen_s(&g_logFile, logPath.c_str(), "a");
	CreateDirectoryA(shots.c_str(), nullptr);
	SetEnvironmentVariableA("VP_TEST_SCREENSHOT_DIR", shots.c_str());
	_putenv_s("VP_TEST_SCREENSHOT_DIR", shots.c_str());
	{
		// A shader cache beside this tool (the renderer reads VP_TEST_SHADER_CACHE_DIR).
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		std::string folder(exe);
		folder = folder.substr(0, folder.find_last_of("\\/") + 1) + "shader-cache";
		CreateDirectoryA(folder.c_str(), nullptr);
		CreateDirectoryA((folder + "\\vprenderer").c_str(), nullptr);
		SetEnvironmentVariableA("VP_TEST_SHADER_CACHE_DIR", folder.c_str());
		_putenv_s("VP_TEST_SHADER_CACHE_DIR", folder.c_str());
	}
	HMODULE dll = LoadLibraryExA(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!dll) { fprintf(stderr, "cannot load %s (%lu)\n", dllPath.c_str(), GetLastError()); return 3; }
	using Create = IVideoRenderer* (__cdecl*)(IRendererCallback*, uint32_t, HWND, ITimingClock*, bool, size_t,
		VideoConversionOverride, const char*, VideoProcessorLibplaceboLogSink);
	using Destroy = void(__cdecl*)(IVideoRenderer*);
	const auto create = reinterpret_cast<Create>(GetProcAddress(dll, "VideoProcessorLibplaceboCreateRenderer"));
	const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(dll, "VideoProcessorLibplaceboDestroyRenderer"));
	if (!create || !destroy) { fprintf(stderr, "renderer factory missing\n"); return 3; }

	WNDCLASSA wc{};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = "VpScaleRender";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "VpScaleRender", WS_POPUP,
		-6000, -6000, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
	if (!hwnd) { fprintf(stderr, "no window\n"); return 3; }
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);

	Callback callback;
	Clock clock;
	IVideoRenderer* renderer = create(&callback, 1, hwnd, &clock, true, 8, VideoConversionOverride::VIDEOCONVERSION_NONE,
		configPath.c_str(), &CaptureLog);
	if (!renderer) { fprintf(stderr, "renderer not created\n"); DestroyWindow(hwnd); return 4; }
	VideoStateComPtr state = new VideoState();
	state->valid = true;
	state->displayMode = std::make_shared<DisplayMode>(sw, sh, false, rn, rd);
	state->videoFrameEncoding = VideoFrameEncoding::V210;
	state->colorspace = ColorSpace::REC_709;
	state->eotf = EOTF::SDR;
	renderer->OnVideoState(state);
	renderer->Build();
	{
		DEVMODEW mode{};
		mode.dmSize = sizeof(mode);
		if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
			renderer->SetRenderLoadDisplayRefreshRate(static_cast<double>(mode.dmDisplayFrequency));
	}
	if (!shaderProfiles.empty())
	{
		CString active;
		bool restart = false;
		const bool selected = renderer->SelectShaderProfiles(shaderProfiles, active, restart);
		CaptureLog(("VpScaleRender: shader profiles " + std::string(selected ? "selected: " : "NOT selected: ") + std::string(CStringA(active).GetString())).c_str());
		printf("shader profiles %s: %s\n", selected ? "selected" : "NOT selected", CStringA(active).GetString());
		if (!selected) { fprintf(stderr, "shader profiles not selected\n"); }
		if (restart) renderer->Build();
	}
	renderer->SetQueueFramePolicy(2, 4, true);
	renderer->Start();

	const size_t rowBytes = static_cast<size_t>(state->BytesPerRow());
	const size_t frameBytes = static_cast<size_t>(state->BytesPerFrame());
	std::vector<std::vector<BYTE>> ring(24, std::vector<BYTE>(frameBytes));
	std::vector<SourceBuffer> buffers(ring.size());
	const auto period = std::chrono::microseconds(static_cast<int64_t>(1e6 * rd / rn));
	const double ticksPerUs = static_cast<double>(clock.TimingClockTicksPerSecond()) / 1e6;
	const timingclocktime_t startTicks = clock.TimingClockNow();
	auto start = std::chrono::steady_clock::now();
	std::ofstream result(resultPath, std::ios::app);
	int n = 0, failures = 0;
	std::vector<uint16_t> luma(static_cast<size_t>(sw) * sh);
	std::map<unsigned long long, std::pair<std::string, int>> sent;   // sequence -> (picture, frame index)
	for (const auto& pic : pictures)
	{
		// A picture is a short clip of consecutive frames (real motion and grain for the temporal
		// parts of the chain); the capture is requested 4 frames before its end and the clip's last
		// frame repeats until the capture completes. The captured sequence says which frame it was.
		std::ifstream raw(pic.path, std::ios::binary);
		std::vector<std::vector<BYTE>> packed;
		for (int k = 0; k < pic.frames; ++k)
		{
			raw.read(reinterpret_cast<char*>(luma.data()), static_cast<std::streamsize>(luma.size() * 2));
			if (!raw) break;
			packed.emplace_back(frameBytes);
			FillV210(packed.back(), luma, sw, sh, rowBytes);
		}
		if (packed.empty()) { result << pic.tag << " -1 - - 0 " << pic.tag << " fail\n"; result.flush(); ++failures; continue; }
		const int nf = static_cast<int>(packed.size());
		// Real time from here: loading this picture took time; without re-anchoring, its frames would be
		// sent as fast as possible to catch up (time-based detectors such as VP's source monitor would see
		// a burst). The source timestamps stay continuous (period * n).
		start = std::chrono::steady_clock::now() - period * n;
		if (pic.seq)
		{
			// Sequence mode: the frames play once, continuously (VP's temporal state - motion vectors, grain
			// map - sees genuine motion), and a capture is requested again as soon as the previous one
			// completes; at a low source rate every frame is captured. One result line per capture.
			size_t mark = 0;
			{ std::lock_guard<std::mutex> g(g_logMutex); mark = g_log.size(); }
			bool pending = false;
			int lastIdx = -1, captured = 0;
			const int tail = static_cast<int>(4.0 * rn / rd) + 4;
			for (int k = 0; k < nf + tail; ++k, ++n)
			{
				if (!pending && k >= (std::max)(1, pic.start) && k < nf)   // no new requests on the repeated tail
				{
					CString status;
					pending = renderer->RequestRenderedOutputCapture(status);
				}
				const int idx = (std::min)(k, nf - 1);
				std::vector<BYTE>& buffer = ring[n % ring.size()];
				std::copy(packed[idx].begin(), packed[idx].end(), buffer.begin());
				const timingclocktime_t stamp = startTicks + static_cast<timingclocktime_t>(
					static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(period * n).count()) * ticksPerUs);
				sent[static_cast<unsigned long long>(n + 1)] = std::make_pair(pic.tag, k < nf ? idx : -1);   // -1: the repeated tail (no motion)
				VideoFrame frame(buffer.data(), static_cast<uint64_t>(n + 1), stamp, &buffers[n % ring.size()], stamp);
				renderer->OnVideoFrame(frame);
				Pump();
				if (pending)
				{
					std::lock_guard<std::mutex> g(g_logMutex);
					for (size_t i = mark; i < g_log.size(); ++i)
					{
						const std::string& line = g_log[i];
						const size_t at = line.find("Rendered-output capture complete: source=");
						if (at == std::string::npos) continue;
						const unsigned long long cs = std::strtoull(line.c_str() + at + 41, nullptr, 10);
						std::string cpng = "-", cjson = "-";
						const size_t p = line.find(" png="), m = line.find(" metadata="), r = line.find(" raw_bytes=");
						if (p != std::string::npos && m != std::string::npos)
						{
							cpng = line.substr(p + 5, m - p - 5);
							cjson = line.substr(m + 10, r == std::string::npos ? std::string::npos : r - m - 10);
						}
						const auto it = sent.find(cs);
						const std::string ctag = it != sent.end() ? it->second.first : std::string("-");
						const int cidx = it != sent.end() ? it->second.second : -1;
						result << ctag << " " << cidx << " " << cpng << " " << cjson << " " << cs << " " << pic.tag << (ctag == pic.tag ? " ok" : " fail") << "\n";
						result.flush();
						lastIdx = (std::max)(lastIdx, cidx);
						++captured;
						mark = i + 1;
						pending = false;
						break;
					}
				}
				std::this_thread::sleep_until(start + period * (n + 1));
				if (k >= nf && !pending) break;
			}
			printf("%s: sequence of %d frames, %d captures, last frame %d\n", pic.tag.c_str(), nf, captured, lastIdx);
			fflush(stdout);
			continue;
		}
		bool requested = noCapture, done = noCapture;
		std::string png, json;
		unsigned long long seq = 0;
		size_t logMark = 0;
		{ std::lock_guard<std::mutex> g(g_logMutex); logMark = g_log.size(); }
		const int requestAt = (std::max)(0, nf - 4);
		const int giveUp = nf + static_cast<int>(10.0 * rn / rd);
		for (int k = 0; ; ++k, ++n)
		{
			if (done && k >= nf) break;
			if (k > giveUp) break;
			if (!requested && k >= requestAt)
			{
				CString status;
				requested = renderer->RequestRenderedOutputCapture(status);
				if (!requested) CaptureLog(("VpScaleRender: capture refused: " + std::string(CStringA(status).GetString())).c_str());
			}
			const int idx = (std::min)(k, nf - 1);
			std::vector<BYTE>& buffer = ring[n % ring.size()];
			std::copy(packed[idx].begin(), packed[idx].end(), buffer.begin());
			const timingclocktime_t stamp = startTicks + static_cast<timingclocktime_t>(
				static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(period * n).count()) * ticksPerUs);
			sent[static_cast<unsigned long long>(n + 1)] = std::make_pair(pic.tag, idx);
			VideoFrame frame(buffer.data(), static_cast<uint64_t>(n + 1), stamp, &buffers[n % ring.size()], stamp);
			renderer->OnVideoFrame(frame);
			Pump();
			if (requested && !done)
			{
				std::lock_guard<std::mutex> g(g_logMutex);
				for (size_t i = logMark; i < g_log.size(); ++i)
				{
					const std::string& line = g_log[i];
					const size_t at = line.find("Rendered-output capture complete: source=");
					if (at == std::string::npos) continue;
					seq = std::strtoull(line.c_str() + at + 41, nullptr, 10);
					const size_t p = line.find(" png="), m = line.find(" metadata="), r = line.find(" raw_bytes=");
					if (p != std::string::npos && m != std::string::npos)
					{
						png = line.substr(p + 5, m - p - 5);
						json = line.substr(m + 10, r == std::string::npos ? std::string::npos : r - m - 10);
					}
					done = true;
				}
			}
			std::this_thread::sleep_until(start + period * (n + 1));
		}
		std::string tag = "-";
		int idx = -1;
		const auto it = sent.find(seq);
		if (done && it != sent.end()) { tag = it->second.first; idx = it->second.second; }
		const bool ok = done && tag == pic.tag;
		if (!ok) ++failures;
		result << tag << " " << idx << " " << (png.empty() ? "-" : png) << " " << (json.empty() ? "-" : json) << " " << seq << " "
			<< pic.tag << (ok ? " ok" : " fail") << "\n";
		result.flush();
		printf("%s: captured %s frame %d (seq %llu) %s\n", pic.tag.c_str(), tag.c_str(), idx, seq, ok ? "ok" : "FAIL");
		fflush(stdout);
		++n;
	}
	CString status;
	if (renderer->GetSourceResolutionStatus(status)) CaptureLog(("VpScaleRender: source resolution: " + std::string(CStringA(status).GetString())).c_str());
	renderer->Stop();
	destroy(renderer);
	DestroyWindow(hwnd);
	// Assertions: the renderer ran on the required adapter and every required hybrid pass (custom
	// hook file) was loaded into the chain; otherwise the captures are not of VP's real scaler.
	int assertFail = 0;
	{
		std::lock_guard<std::mutex> g(g_logMutex);
		auto logged = [&](const std::string& a, const std::string& b) {
			for (const std::string& line : g_log)
				if (line.find(a) != std::string::npos && (b.empty() || line.find(b) != std::string::npos)) return true;
			return false;
		};
		if (!requireAdapter.empty() && !logged("libplacebo initialized: D3D11 on", requireAdapter))
		{
			fprintf(stderr, "ASSERT: the renderer did not run on an adapter containing '%s'\n", requireAdapter.c_str());
			assertFail = 1;
		}
		for (const std::string& hook : requireHooks)
			if (!logged("Custom hooks: loaded", hook) || logged("not loaded", hook))
			{
				fprintf(stderr, "ASSERT: required pass '%s' not loaded into the chain\n", hook.c_str());
				assertFail = 1;
			}
		if (logged("Adaptive downscale: on, but the SSimDownscaler shader is not in the chain", ""))
		{
			fprintf(stderr, "ASSERT: adaptive downscale without SSimDownscaler\n");
			assertFail = 1;
		}
	}
	if (assertFail)
	{
		CaptureLog("VpScaleRender: ASSERTION FAILED (adapter / hybrid passes); captures invalid");
		if (g_logFile) fclose(g_logFile);
		return 6;
	}
	CaptureLog("VpScaleRender: assertions passed (adapter and hybrid passes present)");
	if (g_logFile) fclose(g_logFile);
	return failures ? 1 : 0;
}
