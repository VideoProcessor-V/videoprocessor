#pragma once

// VpScaleRender's renderer driver (fix/vpscalerender-motion, 2026-10-24): renders luma pictures
// and picture sequences through the real VP renderer DLL (driven as the player drives it, as in
// VideoProcessor-Test/RendererCaptureHarnessTests.cpp) to an off-screen window of the output size
// and saves VP's rendered-output captures (VP-0133: 16-bit PNG of the R10 output + JSON sidecar).
// Data preparation and tests only: it never changes VP's deployed configuration.
//
// Header-only so VideoProcessor-Test runs exactly this code (VpScaleRenderMotionTests.cpp);
// VpScaleRender.cpp is its command-line wrapper. Job keys: docs/VP-scale-render-motion.md.

#include <DisplayMode.h>
#include <IRenderer.h>
#include <ITimingClock.h>
#include <VideoFrame.h>
#include <VideoState.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "VpScaleRenderSequence.h"

namespace VpScaleRenderCore
{
	namespace Seq = VpScaleRenderSequence;

	inline std::mutex& LogMutex() { static std::mutex m; return m; }
	inline std::vector<std::string>& LogLines() { static std::vector<std::string> v; return v; }
	inline FILE*& LogFile() { static FILE* f = nullptr; return f; }

	inline void __cdecl CaptureLog(const char* message)
	{
		std::lock_guard<std::mutex> guard(LogMutex());
		LogLines().emplace_back(message ? message : "");
		if (LogFile()) { fprintf(LogFile(), "%s\n", message ? message : ""); fflush(LogFile()); }
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

	// The capture card's clock: QPC, less the time the tool itself stalled (loading, a slow
	// disk). A stall pauses this clock, so the frames after it are neither "late" (a growing
	// hardware age reads as a stale capture delivery: a gap) nor sent in a catch-up burst, and
	// their timestamps stay one period apart (a jump would be a timestamp discontinuity).
	struct Clock : ITimingClock
	{
		std::atomic<long long> offset{ 0 };
		timingclocktime_t TimingClockNow() override { LARGE_INTEGER n{}; QueryPerformanceCounter(&n); return n.QuadPart - offset.load(); }
		timingclocktime_t TimingClockTicksPerSecond() const override { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }
		const TCHAR* TimingClockDescription() override { return TEXT("VpScaleRender QPC (paused over tool stalls)"); }
	};

	// V210 with the given luma (10-bit codes) and neutral chroma.
	inline void FillV210(std::vector<BYTE>& frame, const std::vector<uint16_t>& luma, int width, int height, size_t rowBytes)
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

	inline void Pump()
	{
		MSG msg;
		while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
	}

	struct Picture { std::string tag, path; int frames = 1; bool seq = false; int start = 1; };

	struct Job
	{
		std::string dllPath, configPath, shots, resultPath, logPath;
		int sw = 0, sh = 0, rn = 24000, rd = 1001, ww = 0, wh = 0;
		double hold = 2.0;
		bool noCapture = false;
		std::vector<std::string> requireHooks, shaderProfiles;
		std::string requireAdapter;
		std::vector<Picture> pictures;
		// fix/vpscalerender-motion
		bool motion = false;            // `motion vectors`: the estimate runs (Seq::MotionOverrides)
		bool perFrame = false;          // `capture per_frame` (default with motion): one request per source frame
		bool captureModeSet = false;
		double settle = -1.0;           // `settle <s>`: the first sequence frame repeated first (default 3 s with motion)
		double requireMotion = -1.0;    // `require_motion <share> [max_invalidations]`
		long long maxInvalidations = 0;

		bool Parse(std::istream& job)
		{
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
				else if (k == "motion") { std::string v; s >> v; motion = v == "vectors" || v == "on"; }
				else if (k == "capture") { std::string v; s >> v; perFrame = v == "per_frame"; captureModeSet = true; }
				else if (k == "settle") s >> settle;
				else if (k == "require_motion") { s >> requireMotion; if (!(s >> maxInvalidations)) maxInvalidations = 0; }
			}
			if (!captureModeSet) perFrame = motion;
			if (settle < 0.0) settle = motion ? 3.0 : 0.0;
			return !(dllPath.empty() || configPath.empty() || shots.empty() || resultPath.empty() || !sw || !sh || !ww || !wh || pictures.empty());
		}
	};

	// Streams the packed frames of consecutive pictures on a worker thread (a few frames ahead), so
	// a sequence of pictures plays continuously without loading pauses between them.
	class Loader
	{
	public:
		struct Frame { int picture = -1; int index = -1; bool last = false; std::vector<BYTE> data; };

		Loader(const std::vector<Picture>& pics, size_t first, size_t end, int w, int h, size_t rowBytes, size_t frameBytes)
		{
			worker = std::thread([=]()
			{
				std::vector<uint16_t> luma(static_cast<size_t>(w) * h);
				for (size_t p = first; p < end && !stop; ++p)
				{
					std::ifstream raw(pics[p].path, std::ios::binary);
					Frame pending;
					bool have = false;
					for (int k = 0; k < pics[p].frames && !stop; ++k)
					{
						raw.read(reinterpret_cast<char*>(luma.data()), static_cast<std::streamsize>(luma.size() * 2));
						if (!raw) break;
						Frame f;
						f.picture = static_cast<int>(p);
						f.index = k;
						f.data.resize(frameBytes);
						FillV210(f.data, luma, w, h, rowBytes);
						if (have) Push(std::move(pending));
						pending = std::move(f);
						have = true;
					}
					if (have) { pending.last = true; Push(std::move(pending)); }
					else { Frame missing; missing.picture = static_cast<int>(p); missing.last = true; Push(std::move(missing)); }
				}
				std::lock_guard<std::mutex> g(mutex);
				done = true;
				changed.notify_all();
			});
		}
		~Loader()
		{
			{ std::lock_guard<std::mutex> g(mutex); stop = true; changed.notify_all(); }
			if (worker.joinable()) worker.join();
		}
		// The next frame (false at the end); waits while the worker loads.
		bool Next(Frame& out)
		{
			std::unique_lock<std::mutex> lock(mutex);
			changed.wait(lock, [&] { return !queue.empty() || done; });
			if (queue.empty()) return false;
			out = std::move(queue.front());
			queue.pop_front();
			changed.notify_all();
			return true;
		}
		bool Ready()
		{
			std::lock_guard<std::mutex> g(mutex);
			return !queue.empty() || done;
		}

	private:
		void Push(Frame&& f)
		{
			std::unique_lock<std::mutex> lock(mutex);
			changed.wait(lock, [&] { return queue.size() < kAhead || stop; });
			if (stop) return;
			queue.push_back(std::move(f));
			changed.notify_all();
		}
		static constexpr size_t kAhead = 6;
		std::thread worker;
		std::mutex mutex;
		std::condition_variable changed;
		std::deque<Frame> queue;
		bool done = false, stop = false;
	};

	inline std::string ReadText(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	struct Outcome
	{
		int rc = 0;
		Seq::MotionCheck check;
		long long invalidationsTotal = 0;
		std::string invalidationCauses;   // after the settle
		std::vector<Seq::Capture> captures;
		std::string summary;
	};

	// Runs one job. `console` receives the progress lines (stdout for the tool, the test log for tests).
	inline Outcome RunJob(Job job, void (*console)(const std::string&) = nullptr)
	{
		Outcome outcome;
		auto say = [&](const std::string& s) { if (console) console(s); };
		{
			std::lock_guard<std::mutex> g(LogMutex());
			LogLines().clear();
		}
		if (!job.logPath.empty()) fopen_s(&LogFile(), job.logPath.c_str(), "a");
		struct LogCloser { ~LogCloser() { std::lock_guard<std::mutex> g(LogMutex()); if (LogFile()) fclose(LogFile()); LogFile() = nullptr; } } logCloser;
		CreateDirectoryA(job.shots.c_str(), nullptr);
		SetEnvironmentVariableA("VP_TEST_SCREENSHOT_DIR", job.shots.c_str());
		_putenv_s("VP_TEST_SCREENSHOT_DIR", job.shots.c_str());
		if (job.motion)
		{
			// The motion overrides in a configuration of the tool's own (the job's file is not changed).
			const std::string motionConfig = job.shots + "\\vp-scale-render-motion.cfg";
			std::ofstream(motionConfig, std::ios::binary | std::ios::trunc) << Seq::WithMotionOverrides(ReadText(job.configPath));
			job.configPath = motionConfig;
			CaptureLog(("VpScaleRender: motion vectors: configuration with the motion overrides " + motionConfig).c_str());
		}
		{
			// A persistent shader cache beside this tool (the renderer reads VP_TEST_SHADER_CACHE_DIR),
			// unless one is set. Both flavours' sub-folders must exist or the cache is never saved
			// (round 1 had only vprenderer\: the Beta DLL recompiled every shader in every job).
			char existing[MAX_PATH] = {};
			std::string folder;
			if (GetEnvironmentVariableA("VP_TEST_SHADER_CACHE_DIR", existing, MAX_PATH))
				folder = existing;
			else
			{
				char exe[MAX_PATH] = {};
				GetModuleFileNameA(nullptr, exe, MAX_PATH);
				folder = exe;
				folder = folder.substr(0, folder.find_last_of("\\/") + 1) + "shader-cache";
				SetEnvironmentVariableA("VP_TEST_SHADER_CACHE_DIR", folder.c_str());
				_putenv_s("VP_TEST_SHADER_CACHE_DIR", folder.c_str());
			}
			CreateDirectoryA(folder.c_str(), nullptr);
			CreateDirectoryA((folder + "\\vprenderer").c_str(), nullptr);
			CreateDirectoryA((folder + "\\vprenderer-beta").c_str(), nullptr);
		}
		HMODULE dll = LoadLibraryExA(job.dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!dll) { say("cannot load " + job.dllPath); outcome.rc = 3; return outcome; }
		using Create = IVideoRenderer* (__cdecl*)(IRendererCallback*, uint32_t, HWND, ITimingClock*, bool, size_t,
			VideoConversionOverride, const char*, VideoProcessorLibplaceboLogSink);
		using Destroy = void(__cdecl*)(IVideoRenderer*);
		const auto create = reinterpret_cast<Create>(GetProcAddress(dll, "VideoProcessorLibplaceboCreateRenderer"));
		const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(dll, "VideoProcessorLibplaceboDestroyRenderer"));
		if (!create || !destroy) { say("renderer factory missing"); outcome.rc = 3; return outcome; }

		WNDCLASSA wc{};
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = GetModuleHandleA(nullptr);
		wc.lpszClassName = "VpScaleRender";
		RegisterClassA(&wc);
		HWND hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "VpScaleRender", WS_POPUP,
			-6000, -6000, job.ww, job.wh, nullptr, nullptr, wc.hInstance, nullptr);
		if (!hwnd) { say("no window"); outcome.rc = 3; return outcome; }
		ShowWindow(hwnd, SW_SHOWNOACTIVATE);

		Callback callback;
		Clock clock;
		IVideoRenderer* renderer = create(&callback, 1, hwnd, &clock, true, 8, VideoConversionOverride::VIDEOCONVERSION_NONE,
			job.configPath.c_str(), &CaptureLog);
		if (!renderer) { say("renderer not created"); DestroyWindow(hwnd); outcome.rc = 4; return outcome; }
		VideoStateComPtr state = new VideoState();
		state->valid = true;
		state->displayMode = std::make_shared<DisplayMode>(job.sw, job.sh, false, job.rn, job.rd);
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
		if (!job.shaderProfiles.empty())
		{
			CString active;
			bool restart = false;
			const bool selected = renderer->SelectShaderProfiles(job.shaderProfiles, active, restart);
			CaptureLog(("VpScaleRender: shader profiles " + std::string(selected ? "selected: " : "NOT selected: ") + std::string(CStringA(active).GetString())).c_str());
			say(std::string("shader profiles ") + (selected ? "selected: " : "NOT selected: ") + CStringA(active).GetString());
			if (restart) renderer->Build();
		}
		renderer->SetQueueFramePolicy(2, 4, true);
		renderer->Start();
		// Frames sent before the renderer is rendering are not accepted (no sequence): wait for it.
		{
			const auto waitStart = std::chrono::steady_clock::now();
			while (callback.state.load() != static_cast<int>(RendererState::RENDERSTATE_RENDERING) &&
				std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(15))
			{
				Pump();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}

		const size_t rowBytes = static_cast<size_t>(state->BytesPerRow());
		const size_t frameBytes = static_cast<size_t>(state->BytesPerFrame());
		std::vector<std::vector<BYTE>> ring(24, std::vector<BYTE>(frameBytes));
		std::vector<SourceBuffer> buffers(ring.size());
		using Steady = std::chrono::steady_clock;
		const auto period = std::chrono::duration_cast<Steady::duration>(std::chrono::duration<double>(static_cast<double>(job.rd) / job.rn));
		Seq::SessionClock session;
		session.ticksPerSecond = clock.TimingClockTicksPerSecond();
		session.rateNumerator = job.rn;
		session.rateDenominator = job.rd;
		session.startTicks = clock.TimingClockNow();
		Steady::time_point anchor = Steady::now();
		long long anchorN = 0;
		long long n = 0;   // frames sent in this session: the one counter for sequences and timestamps
		std::ofstream result(job.resultPath, std::ios::app);
		int failures = 0;
		std::map<unsigned long long, std::pair<std::string, int>> sent;   // sequence -> (picture, frame index)
		size_t logMark = 0;
		long long sequenceOffset = 0;   // the tool's sequence - the renderer's source= (pacing lines)
		long long acceptedRequests = 0, completed = 0;

		auto due = [&](long long frame) { return anchor + period * (frame - anchorN); };
		// Sends frame n at its due time. A stall of the tool (more than half a period late) pauses the
		// capture clock and moves the schedule instead of sending a burst (Clock).
		// Back-pressure (motion / per-frame jobs): a GPU slower than the declared rate (a laptop at 4K
		// with the motion estimate and a capture per frame) would let the queue grow until the
		// renderer flushes it as stale - a backlog flush, which invalidates the vectors. The tool
		// holds the next frame while the renderer's queue is at its target (4), so it never exceeds it, and counts the
		// wait as a stall: the clock pauses, timestamps stay one period apart, nothing is dropped.
		const bool backPressure = job.motion || job.perFrame;
		long long heldFrames = 0;
		long long heldForCapture = 0;
		auto send = [&](const std::vector<BYTE>& packed, const std::string& tag, int idx)
		{
			if (backPressure && renderer->GetFrameQueueSize() >= 4)
			{
				++heldFrames;
				const auto waitStart = Steady::now();
				while (renderer->GetFrameQueueSize() >= 4 && Steady::now() - waitStart < std::chrono::seconds(3))
				{
					Pump();
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
			}
			const auto now = Steady::now();
			if (now > due(n) + period / 2)
			{
				const auto late = now - due(n);
				anchor += late;
				clock.offset += static_cast<long long>(std::chrono::duration<double>(late).count() * static_cast<double>(session.ticksPerSecond));
			}
			std::this_thread::sleep_until(due(n));
			std::vector<BYTE>& buffer = ring[static_cast<size_t>(n) % ring.size()];
			std::copy(packed.begin(), packed.end(), buffer.begin());
			const timingclocktime_t stamp = session.Stamp(n);
			const unsigned long long sequence = Seq::SessionClock::Sequence(n);
			sent[sequence] = std::make_pair(tag, idx);
			VideoFrame frame(buffer.data(), sequence, stamp, &buffers[static_cast<size_t>(n) % ring.size()], stamp);
			renderer->OnVideoFrame(frame);
			Pump();
			++n;
		};
		// Completed captures since the last call, in order.
		auto drain = [&](std::vector<Seq::Capture>& out)
		{
			std::lock_guard<std::mutex> g(LogMutex());
			auto& lines = LogLines();
			for (; logMark < lines.size(); ++logMark)
			{
				const std::string& line = lines[logMark];
				const size_t pacing = line.find(" source=");
				const size_t frameNo = line.find(" source_frame=");
				if (pacing != std::string::npos && frameNo != std::string::npos && line.rfind("Alpha frame pacing trace", 0) == 0)
					sequenceOffset = std::strtoll(line.c_str() + frameNo + 14, nullptr, 10) - std::strtoll(line.c_str() + pacing + 8, nullptr, 10);
				const size_t at = line.find("Rendered-output capture complete: source=");
				if (at == std::string::npos) continue;
				Seq::Capture c;
				c.sourceSequence = std::strtoull(line.c_str() + at + 41, nullptr, 10);
				const size_t p = line.find(" png="), m = line.find(" metadata="), r = line.find(" raw_bytes=");
				if (p != std::string::npos && m != std::string::npos)
				{
					c.png = line.substr(p + 5, m - p - 5);
					c.json = line.substr(m + 10, r == std::string::npos ? std::string::npos : r - m - 10);
				}
				// The renderer's source= counts the frames it accepted, not the tool's sequence (frames
				// sent before it was rendering, or rejected, are not counted). The capture's own frame
				// number comes from the sidecar (source_frame_counter, this branch's DLLs), else from
				// the offset its pacing lines show (source=A source_frame=B), else source= as is.
				const std::string json = c.json.empty() ? std::string() : ReadText(c.json);
				const std::string counter = Seq::JsonField(json, "source_frame_counter");
				unsigned long long key = counter.empty() ? 0ull : std::strtoull(counter.c_str(), nullptr, 10);
				if (key == 0) key = static_cast<unsigned long long>(static_cast<long long>(c.sourceSequence) + sequenceOffset);
				c.sourceSequence = key;
				const auto it = sent.find(key);
				c.tag = it != sent.end() ? it->second.first : std::string("-");
				c.frame = it != sent.end() ? it->second.second : -1;
				c.motionMode = Seq::JsonField(json, "hybrid_motion_mode");
				const std::string e = Seq::JsonField(json, "motion_estimates"), v = Seq::JsonField(json, "motion_invalidations");
				c.estimates = e.empty() ? -1 : std::strtoll(e.c_str(), nullptr, 10);
				c.invalidations = v.empty() ? -1 : std::strtoll(v.c_str(), nullptr, 10);
				++completed;
				out.push_back(c);
			}
		};
		auto request = [&]()
		{
			CString status;
			const bool accepted = renderer->RequestRenderedOutputCapture(status);
			if (accepted) ++acceptedRequests;
			return accepted;
		};

		// The motion check's window: from the end of the settle (sequence, log line) to the end.
		unsigned long long settleEndSequence = 0;
		size_t settleEndLog = 0;
		bool settled = false;

		for (size_t pi = 0; pi < job.pictures.size();)
		{
			const Picture& pic = job.pictures[pi];
			if (pic.seq && job.perFrame)
			{
				// Per-frame sequence mode: every consecutive picture_seq plays as one continuous stream
				// (pictures streamed by the loader; no tail between them), one capture request per
				// source frame at mid-period. Which frame a capture shows is mapped by its sequence
				// (and by content in the TrainKit); the result line names that frame's picture.
				size_t end = pi;
				while (end < job.pictures.size() && job.pictures[end].seq) ++end;
				Loader loader(job.pictures, pi, end, job.sw, job.sh, rowBytes, frameBytes);
				Loader::Frame f;
				bool have = loader.Next(f);
				std::vector<BYTE> last;
				std::vector<Seq::Capture> done;
				auto emit = [&]()
				{
					for (Seq::Capture& c : done)
					{
						c.requested = c.tag;
						c.ok = c.tag != "-" && c.frame >= 0 && !c.png.empty();
						result << Seq::ResultLine(c) << "\n";
						outcome.captures.push_back(c);
					}
					result.flush();
					done.clear();
				};
				// The first frames of the sequence (up to 8 of its first picture), kept for the settle.
				std::deque<Loader::Frame> head;
				while (have)
				{
					const bool more = !f.data.empty() && !f.last && head.size() < 7;
					head.push_back(std::move(f));
					have = more && loader.Next(f);
					if (!more) { have = false; break; }
				}
				const bool streamLeft = !head.empty() && !(head.back().last && head.back().picture + 1 == static_cast<int>(end));
				// Settle: the first frames played forwards and backwards (real motion of this content,
				// valid pairs) while the engine, its shaders and the queue start: at least `settle` s,
				// then until 2 s pass with no render stall, no motion invalidation and no slow shader
				// compile (and, with motion, with the estimate running), at most 30 s. It ends on frame
				// 0, so the measured frames follow it continuously.
				if (!head.empty() && !head.front().data.empty() && job.settle > 0.0)
				{
					int m = 0;
					while (m < static_cast<int>(head.size()) && !head[m].data.empty() && head[m].picture == head.front().picture) ++m;
					const auto started = Steady::now();
					auto noisy = started;
					bool estimating = !job.motion;
					size_t scanned = 0;
					{
						// The estimate may have started before (a warm-up picture): its first line is logged once.
						std::lock_guard<std::mutex> g(LogMutex());
						scanned = LogLines().size();
						for (const std::string& line : LogLines())
							estimating = estimating || line.find("De-judder: motion estimated") != std::string::npos;
					}
					int pos = 0, dir = 1;
					for (;;)
					{
						send(head[pos].data, "settle", -1);
						{
							std::lock_guard<std::mutex> g(LogMutex());
							const auto& lines = LogLines();
							for (; scanned < lines.size(); ++scanned)
							{
								const std::string& line = lines[scanned];
								if (line.find("Alpha backlog recovery") != std::string::npos || line.find("motion invalidated") != std::string::npos ||
									line.find("(slow!)") != std::string::npos || line.find("queue hard overflow") != std::string::npos)
									noisy = Steady::now();
								if (line.find("De-judder: motion estimated") != std::string::npos || line.find("De-judder: estimating again") != std::string::npos)
									estimating = true;
							}
						}
						const auto now = Steady::now();
						const bool quiet = now - started >= std::chrono::duration<double>(job.settle) &&
							now - noisy >= std::chrono::seconds(2) && estimating;
						if (pos == 0 && (quiet || now - started >= std::chrono::seconds(30)))
							break;
						if (m > 1)
						{
							if (pos + dir < 0 || pos + dir >= m) dir = -dir;
							pos += dir;
						}
					}
					char text[160];
					snprintf(text, sizeof(text), "VpScaleRender: settled after %.1f s (%d lead frames played back and forth)",
						std::chrono::duration<double>(Steady::now() - started).count(), m);
					CaptureLog(text);
				}
				{
					std::lock_guard<std::mutex> g(LogMutex());
					if (!settled) { settleEndLog = LogLines().size(); settleEndSequence = Seq::SessionClock::Sequence(n); settled = true; }
				}
				int frames = 0;
				auto nextFrame = [&](Loader::Frame& out)
				{
					if (!head.empty()) { out = std::move(head.front()); head.pop_front(); return true; }
					return streamLeft && loader.Next(out);
				};
				while (nextFrame(f))
				{
					if (!f.data.empty())
					{
						send(f.data, job.pictures[f.picture].tag, f.index);
						last = f.data;
						++frames;
						std::this_thread::sleep_until(due(n - 1) + period / 2);
						// One capture per source frame: while the previous request has not been taken yet (a
						// slow render), the stream waits for it (the clock pauses) instead of skipping a frame.
						if (!job.noCapture && !request())
						{
							++heldForCapture;
							const auto waitStart = Steady::now();
							while (!request() && Steady::now() - waitStart < std::chrono::seconds(2))
							{
								Pump();
								std::this_thread::sleep_for(std::chrono::milliseconds(2));
							}
						}
						drain(done);
						emit();
					}
					else
					{
						Seq::Capture miss;
						miss.tag = job.pictures[f.picture].tag;
						miss.requested = miss.tag;
						result << Seq::ResultLine(miss) << "\n";
						++failures;
					}
				}
				// The tail: the last frame repeated until the captures still in flight complete and the
				// display has caught up with the last real frame (requests continue until a capture
				// shows the tail), at most 6 s.
				if (!last.empty())
				{
					const long long tailMax = static_cast<long long>(6.0 / session.PeriodSeconds()) + 4;
					bool reachedTail = false;
					for (long long k = 0; k < tailMax; ++k)
					{
						send(last, "tail", -1);
						std::this_thread::sleep_until(due(n - 1) + period / 2);
						if (!job.noCapture && !reachedTail) request();
						drain(done);
						for (const Seq::Capture& c : done) reachedTail = reachedTail || c.tag == "tail";
						emit();
						if ((reachedTail || job.noCapture) && completed >= acceptedRequests) break;
					}
				}
				char text[200];
				snprintf(text, sizeof(text), "VpScaleRender: per-frame sequence of %zu picture(s), %d frames, %lld captures so far",
					end - pi, frames, completed);
				CaptureLog(text);
				say(text);
				pi = end;
				continue;
			}

			// The legacy modes: one picture (a short clip, captured once near its end) or a sequence
			// whose capture is requested again as soon as the previous one completes.
			std::vector<std::vector<BYTE>> packed;
			{
				Loader loader(job.pictures, pi, pi + 1, job.sw, job.sh, rowBytes, frameBytes);
				Loader::Frame f;
				while (loader.Next(f))
					if (!f.data.empty()) packed.push_back(std::move(f.data));
			}
			if (packed.empty())
			{
				Seq::Capture miss;
				miss.tag = pic.tag;
				miss.requested = pic.tag;
				result << Seq::ResultLine(miss) << "\n";
				result.flush();
				++failures;
				++pi;
				continue;
			}
			const int nf = static_cast<int>(packed.size());
			std::vector<Seq::Capture> done;
			if (pic.seq)
			{
				bool pending = false;
				int lastIdx = -1, captured = 0;
				const int tail = static_cast<int>(4.0 * job.rn / job.rd) + 4;
				for (int k = 0; k < nf + tail; ++k)
				{
					if (!pending && k >= (std::max)(1, pic.start) && k < nf && !job.noCapture)
						pending = request();
					const int idx = (std::min)(k, nf - 1);
					send(packed[idx], pic.tag, k < nf ? idx : -1);   // -1: the repeated tail (no motion)
					drain(done);
					for (Seq::Capture& c : done)
					{
						c.requested = pic.tag;
						c.ok = c.tag == pic.tag;
						result << Seq::ResultLine(c) << "\n";
						outcome.captures.push_back(c);
						lastIdx = (std::max)(lastIdx, c.frame);
						++captured;
						pending = false;
					}
					result.flush();
					done.clear();
					if (k >= nf && !pending) break;
				}
				say(pic.tag + ": sequence of " + std::to_string(nf) + " frames, " + std::to_string(captured) + " captures, last frame " + std::to_string(lastIdx));
				++pi;
				continue;
			}
			bool requested = job.noCapture, got = job.noCapture;
			Seq::Capture capture;
			const int requestAt = (std::max)(0, nf - 4);
			const int giveUp = nf + static_cast<int>(10.0 * job.rn / job.rd);
			for (int k = 0; ; ++k)
			{
				if (got && k >= nf) break;
				if (k > giveUp) break;
				if (!requested && k >= requestAt)
				{
					CString status;
					requested = renderer->RequestRenderedOutputCapture(status);
					if (requested) ++acceptedRequests;
					else CaptureLog(("VpScaleRender: capture refused: " + std::string(CStringA(status).GetString())).c_str());
				}
				const int idx = (std::min)(k, nf - 1);
				send(packed[idx], pic.tag, idx);
				if (requested && !got)
				{
					drain(done);
					if (!done.empty()) { capture = done.back(); got = true; }
					done.clear();
				}
			}
			capture.requested = pic.tag;
			capture.ok = got && !job.noCapture && capture.tag == pic.tag;
			if (job.noCapture) capture.ok = true;
			if (!capture.ok) ++failures;
			if (!job.noCapture)
			{
				result << Seq::ResultLine(capture) << "\n";
				result.flush();
				outcome.captures.push_back(capture);
			}
			say(pic.tag + ": captured " + capture.tag + " frame " + std::to_string(capture.frame) + " (seq " +
				std::to_string(capture.sourceSequence) + ") " + (capture.ok ? "ok" : "FAIL"));
			++pi;
		}
		CString status;
		if (renderer->GetSourceResolutionStatus(status)) CaptureLog(("VpScaleRender: source resolution: " + std::string(CStringA(status).GetString())).c_str());
		renderer->Stop();
		destroy(renderer);
		DestroyWindow(hwnd);

		// The motion summary: estimates and invalidations after the settle, and the captures' grain-map mode.
		{
			std::lock_guard<std::mutex> g(LogMutex());
			const auto& lines = LogLines();
			long long atSettle = 0, total = 0;
			std::map<std::string, int> causes;
			for (size_t i = 0; i < lines.size(); ++i)
			{
				std::string cause;
				const long long t = Seq::InvalidationTotal(lines[i], &cause);
				if (t < 0) continue;
				total = (std::max)(total, t);
				if (settled && i < settleEndLog) atSettle = (std::max)(atSettle, t);
				else if (settled) ++causes[cause];
			}
			outcome.invalidationsTotal = total;
			outcome.check.invalidations = settled ? total - atSettle : 0;
			for (const auto& c : causes) outcome.invalidationCauses += c.first + " x" + std::to_string(c.second) + "; ";
		}
		const Seq::Capture* first = nullptr;
		const Seq::Capture* lastCapture = nullptr;
		for (const Seq::Capture& c : outcome.captures)
		{
			if (!settled || c.sourceSequence < settleEndSequence || c.tag == "settle" || c.tag == "tail") continue;
			++outcome.check.captures;
			if (!c.motionMode.empty() && c.motionMode != "-") outcome.check.metadataKnown = true;
			if (c.motionMode == "vectors") ++outcome.check.vectors;
			else if (c.motionMode == "no-vectors") ++outcome.check.noVectors;
			else ++outcome.check.other;
			if (c.estimates >= 0) { if (!first) first = &c; lastCapture = &c; }
		}
		if (first && lastCapture && lastCapture != first)
		{
			outcome.check.estimates = lastCapture->estimates - first->estimates;
			outcome.check.framesSent = static_cast<long long>(lastCapture->sourceSequence) - static_cast<long long>(first->sourceSequence);
		}
		{
			std::ostringstream s;
			s << "VpScaleRender: motion summary: mode=" << (job.motion ? "vectors" : "off") << " capture=" << (job.perFrame ? "per_frame" : "after_complete")
				<< " frames_sent=" << n << " held_for_queue=" << heldFrames << " held_for_capture=" << heldForCapture << " clock_paused_ms=" << clock.offset.load() * 1000 / (std::max)(1LL, session.ticksPerSecond) << " settle_end_sequence=" << settleEndSequence
				<< " estimates_after_settle=" << outcome.check.estimates << " over_frames=" << outcome.check.framesSent
				<< " invalidations_total=" << outcome.invalidationsTotal << " invalidations_after_settle=" << outcome.check.invalidations
				<< " (" << outcome.invalidationCauses << ") captures_after_settle=" << outcome.check.captures
				<< " vectors=" << outcome.check.vectors << " no_vectors=" << outcome.check.noVectors << " other=" << outcome.check.other;
			outcome.summary = s.str();
			CaptureLog(outcome.summary.c_str());
			say(outcome.summary);
		}

		// Assertions: the renderer ran on the required adapter and every required hybrid pass (custom
		// hook file) was loaded into the chain; otherwise the captures are not of VP's real scaler.
		int assertFail = 0;
		{
			std::lock_guard<std::mutex> g(LogMutex());
			auto logged = [&](const std::string& a, const std::string& b) {
				for (const std::string& line : LogLines())
					if (line.find(a) != std::string::npos && (b.empty() || line.find(b) != std::string::npos)) return true;
				return false;
			};
			if (!job.requireAdapter.empty() && !logged("libplacebo initialized: D3D11 on", job.requireAdapter))
			{
				say("ASSERT: the renderer did not run on an adapter containing '" + job.requireAdapter + "'");
				assertFail = 1;
			}
			for (const std::string& hook : job.requireHooks)
				if (!logged("Custom hooks: loaded", hook) || logged("not loaded", hook))
				{
					say("ASSERT: required pass '" + hook + "' not loaded into the chain");
					assertFail = 1;
				}
			if (logged("Adaptive downscale: on, but the SSimDownscaler shader is not in the chain", ""))
			{
				say("ASSERT: adaptive downscale without SSimDownscaler");
				assertFail = 1;
			}
		}
		if (assertFail)
		{
			CaptureLog("VpScaleRender: ASSERTION FAILED (adapter / hybrid passes); captures invalid");
			outcome.rc = 6;
			return outcome;
		}
		CaptureLog("VpScaleRender: assertions passed (adapter and hybrid passes present)");
		if (job.requireMotion >= 0.0)
		{
			std::string why;
			if (!outcome.check.Passes(job.requireMotion, job.maxInvalidations, why))
			{
				CaptureLog(("VpScaleRender: MOTION CHECK FAILED: " + why).c_str());
				say("ASSERT: motion check failed: " + why);
				outcome.rc = 7;
				return outcome;
			}
			CaptureLog("VpScaleRender: motion check passed (continuous vectors after the settle)");
		}
		outcome.rc = failures ? 1 : 0;
		return outcome;
	}

	inline Outcome RunJobFile(const std::string& path, void (*console)(const std::string&) = nullptr)
	{
		Job job;
		std::ifstream in(path);
		if (!job.Parse(in))
		{
			if (console) console("incomplete job");
			Outcome o;
			o.rc = 2;
			return o;
		}
		return RunJob(job, console);
	}
}
