#include "pch.h"
#include "CppUnitTest.h"

#include <AnalysisLumaSource.h>
#include <led/LedController.h>
#include <led/LedDriveConfig.h>
#include <led/LedLookahead.h>
#include <led/LedMeasurement.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

// The owner's LED script against VP's copy of its rules, synthetically (owner
// 2026-10-15): the real clips (VP_TEST_REAL_FOOTAGE, e.g. D:\hfr) measured as
// VP measures a frame, built into scenes that reach every brightness band
// (darkened copies, cuts, fades through black), shown at 24 frames per
// second. The real main_vp.py runs on that timeline through
// tools\led_script_sim.py (a virtual clock; fed VP's measurement of the frame
// on screen, as python_feed does, and again with its screenshots), and VP's
// ScriptController runs on the same frames:
//   A  rules: VP's copy ticked on exactly the frame each script loop decided
//      on - targets and commands, loop by loop, and the first divergence;
//   B  timing: VP's internal script mode (its own tick_ms loop) against the
//      script's command timeline - LED and CNTR levels over time;
//   C  the feed against the screenshots: the same script, the feed padded to
//      the screenshot's time - the command timelines should match.
// Opt-in: VP_TEST_LED_CORRELATION=1. Writes led-correlation.txt (and the
// timeline and the script's output) to VP_TEST_CORPUS_DIR or beside the DLL.
namespace
{
	std::string Env(const char* name)
	{
		char* value = nullptr;
		size_t length = 0;
		std::string result;
		if (_dupenv_s(&value, &length, name) == 0 && value) result = value;
		free(value);
		return result;
	}

	struct Clip
	{
		std::string name;
		std::string path;
		int width = 3840, height = 2160;
		bool y4m = false;      // 8-bit 4:2:0 with FRAME headers; else 10-bit LE planar 4:2:0
		double fps = 120.0;
		long long headerBytes = 0;
		long long frameBytes = 0;
		int frames = 0;
	};

	bool OpenClip(Clip& clip)
	{
		FILE* f = nullptr;
		if (fopen_s(&f, clip.path.c_str(), "rb") != 0 || !f) return false;
		_fseeki64(f, 0, SEEK_END);
		const long long size = _ftelli64(f);
		if (clip.y4m)
		{
			_fseeki64(f, 0, SEEK_SET);
			char header[512] = {};
			fgets(header, sizeof(header), f);
			const std::string text(header);
			clip.headerBytes = static_cast<long long>(text.size());
			std::istringstream in(text);
			for (std::string word; in >> word;)
			{
				if (word[0] == 'W') clip.width = std::stoi(word.substr(1));
				else if (word[0] == 'H') clip.height = std::stoi(word.substr(1));
				else if (word[0] == 'F')
				{
					const size_t colon = word.find(':');
					clip.fps = std::stod(word.substr(1, colon - 1)) / std::stod(word.substr(colon + 1));
				}
			}
			clip.frameBytes = 6 + static_cast<long long>(clip.width) * clip.height * 3 / 2;
		}
		else
			clip.frameBytes = static_cast<long long>(clip.width) * clip.height * 3;   // 2 bytes x 1.5
		clip.frames = static_cast<int>((size - clip.headerBytes) / clip.frameBytes);
		fclose(f);
		return clip.frames > 0;
	}

	// Every `step`-th frame measured as VP's look-ahead measures a raw frame
	// (3x3 block means on a grid of at most 96 x 54, limited-range codes; the
	// identity transfer: the frame's own signal), cached per clip.
	bool MeasureClip(const Clip& clip, int step, const std::string& cacheDir, std::vector<RawMeasurement>& out)
	{
		const std::string cache = cacheDir + clip.name + "-" + std::to_string(step) + ".ledraw";
		{
			std::ifstream in(cache, std::ios::binary);
			int count = 0;
			if (in.read(reinterpret_cast<char*>(&count), sizeof(count)) && count > 0)
			{
				out.resize(count);
				if (in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sizeof(RawMeasurement)) * count))
					return true;
			}
		}
		FILE* f = nullptr;
		if (fopen_s(&f, clip.path.c_str(), "rb") != 0 || !f) return false;
		const size_t lumaSamples = static_cast<size_t>(clip.width) * clip.height;
		std::vector<uint16_t> p010(lumaSamples * 3 / 2, 0);
		std::vector<uint8_t> bytes(clip.y4m ? lumaSamples : 0);
		AnalysisLumaSource luma;
		luma.data = reinterpret_cast<const uint8_t*>(p010.data());
		luma.dataBytes = p010.size() * 2;
		luma.width = clip.width;
		luma.height = clip.height;
		luma.rowBytes = luma.chromaRowBytes = static_cast<size_t>(clip.width) * 2;
		luma.format = AnalysisLumaFormat::P010;
		luma.encoding = VideoFrameEncoding::V210;
		luma.colorspace = ColorSpace::REC_709;
		QueuedFrameStatistics::PictureRect rect{ 0, 0, clip.width, clip.height };
		const RawTransfer transfer = IdentityRawTransfer();
		out.clear();
		for (int frame = 0; frame < clip.frames; frame += step)
		{
			const long long offset = clip.headerBytes + static_cast<long long>(frame) * clip.frameBytes + (clip.y4m ? 6 : 0);
			_fseeki64(f, offset, SEEK_SET);
			if (clip.y4m)
			{
				if (fread(bytes.data(), 1, lumaSamples, f) != lumaSamples) break;
				for (size_t i = 0; i < lumaSamples; ++i) p010[i] = static_cast<uint16_t>(bytes[i] << 8);
			}
			else
			{
				if (fread(p010.data(), 2, lumaSamples, f) != lumaSamples) break;
				for (size_t i = 0; i < lumaSamples; ++i) p010[i] = static_cast<uint16_t>(p010[i] << 6);
			}
			RawMeasurement raw;
			if (!MeasureRawFrame(luma, rect, transfer, raw)) break;
			out.push_back(raw);
		}
		fclose(f);
		if (out.empty()) return false;
		std::ofstream save(cache, std::ios::binary | std::ios::trunc);
		const int count = static_cast<int>(out.size());
		save.write(reinterpret_cast<const char*>(&count), sizeof(count));
		save.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(sizeof(RawMeasurement)) * count);
		return true;
	}

	// The picture darkened about black: code' = 16 + (code - 16) k. Exact for
	// the block means (sums, cells, the maximum); the histogram is re-binned.
	RawMeasurement Darkened(const RawMeasurement& raw, double k)
	{
		if (k == 1.0) return raw;
		RawMeasurement out = raw;
		const double black = 16.0 * SUM_SCALE;
		auto scale = [&](uint32_t value, uint32_t count)
		{
			const double base = black * count;
			return static_cast<uint32_t>(std::max(0.0, base + (static_cast<double>(value) - base) * k) + 0.5);
		};
		out.sum = scale(raw.sum, raw.blocks);
		out.maximum = raw.maximum ? scale(raw.maximum, 1) : 0;
		for (int i = 0; i < GRID_W * GRID_H; ++i)
			out.cellSum[i] = scale(raw.cellSum[i], raw.cellCount[i]);
		std::fill(std::begin(out.histogram), std::end(out.histogram), 0u);
		for (int bin = 0; bin < HISTOGRAM_BINS; ++bin)
		{
			const double code = 16.0 + (bin + 0.5 - 16.0) * k;
			out.histogram[std::max(0, std::min(static_cast<int>(code), HISTOGRAM_BINS - 1))] += raw.histogram[bin];
		}
		return out;
	}

	struct Tick { double t; int frame, mean, maximum, hl, tp, tc; };
	struct Command { double t; std::string kind; int value; };

	std::vector<std::vector<std::string>> ReadCsv(const std::string& path)
	{
		std::vector<std::vector<std::string>> rows;
		std::ifstream in(path);
		std::string line;
		std::getline(in, line);   // header
		while (std::getline(in, line))
		{
			std::vector<std::string> cells;
			std::stringstream row(line);
			for (std::string cell; std::getline(row, cell, ',');) cells.push_back(cell);
			rows.push_back(cells);
		}
		return rows;
	}

	std::vector<Tick> ReadTicks(const std::string& path)
	{
		std::vector<Tick> ticks;
		for (const auto& c : ReadCsv(path))
			if (c.size() >= 7)
				ticks.push_back({ std::stod(c[0]), std::stoi(c[1]), std::stoi(c[2]), std::stoi(c[3]), std::stoi(c[4]),
					std::stoi(c[5]), std::stoi(c[6]) });
		return ticks;
	}

	std::vector<Command> ReadCommands(const std::string& path)
	{
		std::vector<Command> commands;
		for (const auto& c : ReadCsv(path))
			if (c.size() >= 3) commands.push_back({ std::stod(c[0]), c[1], std::stoi(c[2]) });
		return commands;
	}

	// A command timeline's level at time t (ms): the last value sent at or before t.
	int LevelAt(const std::vector<Command>& commands, const char* kind, double t, int initial)
	{
		int level = initial;
		for (const Command& c : commands)
		{
			if (c.t > t) break;
			if (c.kind == kind) level = c.value;
		}
		return level;
	}

	struct LevelDifference { double meanLight = 0, meanContrast = 0; int maxLight = 0, maxContrast = 0; double sameShare = 0; };

	LevelDifference Compare(const std::vector<Command>& a, const std::vector<Command>& b, double endMs)
	{
		LevelDifference d;
		int samples = 0, same = 0;
		for (double t = 0; t < endMs; t += 50.0)
		{
			const int la = LevelAt(a, "LPW1", t, 255), lb = LevelAt(b, "LPW1", t, 255);
			const int ca = LevelAt(a, "CNTR", t, 50), cb = LevelAt(b, "CNTR", t, 50);
			d.meanLight += std::abs(la - lb);
			d.meanContrast += std::abs(ca - cb);
			d.maxLight = std::max(d.maxLight, std::abs(la - lb));
			d.maxContrast = std::max(d.maxContrast, std::abs(ca - cb));
			same += la == lb && ca == cb ? 1 : 0;
			++samples;
		}
		if (samples)
		{
			d.meanLight /= samples;
			d.meanContrast /= samples;
			d.sameShare = static_cast<double>(same) / samples;
		}
		return d;
	}

	std::string Describe(const LevelDifference& d)
	{
		char text[256];
		sprintf_s(text, "LED level |diff| mean %.2f max %d; CNTR |diff| mean %.2f max %d; identical %.1f%% of the time",
			d.meanLight, d.maxLight, d.meanContrast, d.maxContrast, d.sameShare * 100.0);
		return text;
	}

	bool RunPython(const std::string& commandLine, std::string& output)
	{
		STARTUPINFOA startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		std::string line = commandLine;
		if (!CreateProcessA(nullptr, &line[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
		{
			output = "could not start: " + commandLine;
			return false;
		}
		WaitForSingleObject(process.hProcess, 600000);
		DWORD code = 1;
		GetExitCodeProcess(process.hProcess, &code);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return code == 0;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedScriptCorrelationTests)
	{
	public:
		TEST_METHOD(ScriptAndVpCopyAgreeOnRealClips)
		{
			if (Env("VP_TEST_LED_CORRELATION") != "1")
			{
				Logger::WriteMessage(L"LED correlation: set VP_TEST_LED_CORRELATION=1 (and VP_TEST_REAL_FOOTAGE) to run");
				return;
			}
			std::string footage = Env("VP_TEST_REAL_FOOTAGE");
			if (footage.empty()) footage = "D:\\hfr";
			if (footage.back() != '\\') footage += "\\";
			char here[MAX_PATH] = {};
			HMODULE self = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&Env), &self);
			GetModuleFileNameA(self, here, MAX_PATH);
			std::string outDir = Env("VP_TEST_CORPUS_DIR");
			if (outDir.empty())
			{
				const std::string binary(here);
				outDir = binary.substr(0, binary.find_last_of("\\/") + 1) + "vp-led-correlation";
			}
			if (outDir.back() != '\\') outDir += "\\";
			CreateDirectoryA(outDir.c_str(), nullptr);
			// The repository from this source file's path: src\VideoProcessor-Test\.
			std::string repo = __FILE__;
			for (int up = 0; up < 3; ++up) repo = repo.substr(0, repo.find_last_of("\\/"));
			const std::string simulator = repo + "\\tools\\led_script_sim.py";
			const std::string script = repo + "\\packaging\\listen-script\\main_vp.py";
			char python[MAX_PATH] = {};
			Assert::IsTrue(SearchPathA(nullptr, "python.exe", nullptr, MAX_PATH, python, nullptr) != 0, L"python.exe on PATH");

			// The clips, each at about 24 source frames per second of real motion.
			std::vector<Clip> clips;
			for (const char* name : { "Bosphorus", "Jockey", "ReadySetGo", "ShakeNDry", "YachtRide" })
			{
				Clip c;
				c.name = name;
				c.path = footage + name + "_3840x2160_120fps_420_10bit_YUV.yuv";
				if (OpenClip(c)) clips.push_back(c);
			}
			for (const char* name : { "crowd_run", "park_joy" })
			{
				Clip c;
				c.name = name;
				c.path = footage + name + "_2160p50.y4m";
				c.y4m = true;
				if (OpenClip(c)) clips.push_back(c);
			}
			Assert::IsFalse(clips.empty(), L"no clips found in VP_TEST_REAL_FOOTAGE");
			std::vector<std::vector<RawMeasurement>> measured(clips.size());
			for (size_t i = 0; i < clips.size(); ++i)
			{
				const int step = std::max(1, static_cast<int>(std::lround(clips[i].fps / 24.0)));
				Assert::IsTrue(MeasureClip(clips[i], step, outDir, measured[i]), std::wstring(clips[i].name.begin(), clips[i].name.end()).c_str());
			}

			// The scenes: per clip bright, a cut to darker, a fade through black,
			// dark, very dark, and a cut back to bright - every band of the
			// script's power and contrast tables, its fast dumps and its
			// anti-pump. Played forwards, then backwards (seamless).
			struct Shown { const RawMeasurement* raw; double k; };
			std::vector<Shown> shown;
			const RawMeasurement black = Darkened(measured[0][0], 0.0);
			for (size_t i = 0; i < clips.size(); ++i)
			{
				const auto& m = measured[i];
				const int n = static_cast<int>(m.size());
				auto play = [&](double k, bool backwards)
				{
					for (int j = 0; j < n; ++j) shown.push_back({ &m[backwards ? n - 1 - j : j], k });
				};
				auto fade = [&](double from, double to, int frames, bool backwards)
				{
					for (int j = 0; j < frames; ++j)
						shown.push_back({ &m[backwards ? n - 1 - (j % n) : j % n], from + (to - from) * (j + 1) / frames });
				};
				play(1.0, false);
				play(0.4, true);
				fade(0.4, 0.0, 48, false);
				for (int j = 0; j < 24; ++j) shown.push_back({ &black, 1.0 });
				fade(0.0, 0.15, 48, true);
				play(0.15, false);
				play(0.08, true);
				play(1.0, false);
			}

			LedDriveConfig config;
			config.enabled = true;
			config.mode = Mode::Script;
			StatsBuilder builder(config);
			const double frameMs = 1000.0 / 24.0;
			std::vector<FrameStats> stats;
			stats.reserve(shown.size());
			{
				std::ofstream timeline(outDir + "led-timeline.csv", std::ios::trunc);
				timeline << "t_ms,apl,max,hl\n";
				for (size_t f = 0; f < shown.size(); ++f)
				{
					stats.push_back(builder.Build(Darkened(*shown[f].raw, shown[f].k), f * frameMs));
					const FrameStats& s = stats.back();
					timeline << f * frameMs << "," << s.apl << "," << s.maximum << ","
						<< (s.maximum >= config.scriptHighlightCode ? 1 : 0) << "\n";
				}
			}
			const double endMs = shown.size() * frameMs;

			// The real script on that timeline: fed VP's measurement, and with its screenshots.
			std::string output;
			const std::string base = std::string("\"") + python + "\" -I \"" + simulator + "\" --script \"" + script +
				"\" --timeline \"" + outDir + "led-timeline.csv\"" +
				// The script's real costs on the cinema PC (seconds): a screenshot
				// with its resize, and one cmd echo to the port.
				(Env("VP_TEST_LED_SHOT").empty() ? std::string() : " --shot " + Env("VP_TEST_LED_SHOT")) +
				(Env("VP_TEST_LED_CMD").empty() ? std::string() : " --cmd " + Env("VP_TEST_LED_CMD")) + " --out \"";
			Assert::IsTrue(RunPython(base + outDir + "fed\"", output), L"the simulator (fed) ran");
			Assert::IsTrue(RunPython(base + outDir + "shots\" --no-feed", output), L"the simulator (screenshots) ran");
			const std::vector<Tick> ticks = ReadTicks(outDir + "fed_ticks.csv");
			const std::vector<Command> scriptCommands = ReadCommands(outDir + "fed_commands.csv");
			const std::vector<Command> shotCommands = ReadCommands(outDir + "shots_commands.csv");
			Assert::IsTrue(ticks.size() > 100, L"the script ran its loop");

			// A: VP's copy on exactly the frame each loop decided on.
			ScriptController shadow(config);
			shadow.Reset(255, 50);
			int lightTargetDiffer = 0, contrastTargetDiffer = 0, commandsDiffer = 0, compared = 0;
			std::string firstDivergence;
			size_t next = 0;
			while (next < scriptCommands.size() && (ticks.empty() || scriptCommands[next].t < ticks[0].t)) ++next;   // the start's LPW1 255 / CNTR 50
			for (size_t i = 0; i < ticks.size(); ++i)
			{
				const Tick& tick = ticks[i];
				if (tick.frame < 0 || tick.frame >= static_cast<int>(stats.size())) continue;
				const ControllerOutput out = shadow.Tick(stats[tick.frame]);
				std::vector<std::pair<std::string, int>> mine, theirs;
				if (out.lightCommand >= 0) mine.push_back({ "LPW1", out.lightCommand });
				for (const ContrastStep& step : out.contrastSchedule) mine.push_back({ "CNTR", step.contrast });
				const double until = i + 1 < ticks.size() ? ticks[i + 1].t : 1e18;
				while (next < scriptCommands.size() && scriptCommands[next].t < until)
				{
					theirs.push_back({ scriptCommands[next].kind, scriptCommands[next].value });
					++next;
				}
				++compared;
				const bool targetsDiffer = out.lightTarget != tick.tp || out.contrastTarget != tick.tc;
				lightTargetDiffer += out.lightTarget != tick.tp ? 1 : 0;
				contrastTargetDiffer += out.contrastTarget != tick.tc ? 1 : 0;
				commandsDiffer += mine != theirs ? 1 : 0;
				if (firstDivergence.empty() && (targetsDiffer || mine != theirs))
				{
					std::ostringstream text;
					text << "first divergence at loop " << i << " (t=" << tick.t / 1000.0 << " s, frame " << tick.frame << ", mean "
						<< tick.mean << ", max " << tick.maximum << "): script targets " << tick.tp << "/" << tick.tc << " VP "
						<< out.lightTarget << "/" << out.contrastTarget << "; script sent";
					for (const auto& c : theirs) text << " " << c.first << " " << c.second;
					text << "; VP would send";
					for (const auto& c : mine) text << " " << c.first << " " << c.second;
					firstDivergence = text.str();
				}
			}

			// B: VP's internal script mode in its own timing on the same frames.
			ScriptController internal(config);
			internal.Reset(255, 50);
			std::vector<Command> vpCommands = { { 0.0, "LPW1", 255 }, { 0.0, "CNTR", 50 } };
			for (double t = 0.0; t < endMs;)
			{
				const int frame = std::min(static_cast<int>(t / frameMs), static_cast<int>(stats.size()) - 1);
				const ControllerOutput out = internal.Tick(stats[frame]);
				double sendsDone = t, floor = t;
				if (out.lightCommand >= 0)
				{
					vpCommands.push_back({ t, "LPW1", out.lightCommand });
					if (out.contrastSchedule.empty()) floor = t + out.durationMs;
				}
				int previous = 0;
				for (const ContrastStep& step : out.contrastSchedule)
				{
					sendsDone += step.offsetMs - previous;
					previous = step.offsetMs;
					vpCommands.push_back({ sendsDone, "CNTR", step.contrast });
				}
				t = std::max({ t + config.tickMs, sendsDone, floor });
			}

			const LevelDifference internalVsScript = Compare(vpCommands, scriptCommands, endMs);
			const LevelDifference fedVsShots = Compare(scriptCommands, shotCommands, endMs);
			std::ostringstream report;
			report << "LED script correlation (" << clips.size() << " clips, " << shown.size() << " frames at 24 fps, "
				<< endMs / 1000.0 << " s)\n"
				<< "A  rules, VP's copy on the frame each script loop used: " << compared << " loops; light target differs "
				<< lightTargetDiffer << ", contrast target differs " << contrastTargetDiffer << ", commands differ "
				<< commandsDiffer << "\n   " << (firstDivergence.empty() ? std::string("no divergence") : firstDivergence) << "\n"
				<< "B  VP internal script mode (tick_ms " << config.tickMs << ") vs the script: " << Describe(internalVsScript)
				<< "; commands " << vpCommands.size() << " vs " << scriptCommands.size() << "\n"
				<< "C  the script fed vs with screenshots: " << Describe(fedVsShots) << "; commands " << scriptCommands.size()
				<< " vs " << shotCommands.size() << "\n";
			std::ofstream(outDir + "led-correlation.txt", std::ios::trunc) << report.str();
			Logger::WriteMessage(report.str().c_str());
		}
	};
}
