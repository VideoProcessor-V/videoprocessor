// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// COMBINATION RUNNER (settings agent, 2026-10-24): runs the pairwise / high-risk / random
// combinations written by tools/vpv_tests (one per line: name=..;dim=value;...), CPU only.
//
// Per combination:
//   1. the renderer profile it implies (the unit tier presets from the VP-V table + the toggle
//      values) must be accepted by VP's own config reader (RendererProfileConfig::Read);
//   2. the queue/timing logic runs it (QueueStressHarness: rate, queue frames, low latency,
//      render-ahead, hiccup repair, look-ahead), 90 simulated seconds, with the seed's random
//      faults for rnd-* lines; any invariant failure fails the combination;
//   3. the delay planner gives the combination's queue frames a constant total.
// Failing lines are appended verbatim + " # reason" to VP_COMBO_LOG for replay
// (tools/run_combinations.ps1 -Replay). Without VP_COMBO_FILE a small built-in set runs, so the
// test always exercises the path. GPU combinations are the BenchKit's (VP_COMBO_GPU=1 is
// reserved; nothing here touches the GPU).

#include <ConfigFile.h>
#include <VP-V/core/DelayBudgetLadder.h>
#include <RendererProfileConfig.h>

#include <VP-V/config/VpvTierTable.generated.h>
#include "../../VideoProcessor-Test/QueueStressHarness.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CombinationRunnerTest
{
	namespace
	{
		std::string Env(const char* name)
		{
			char* value = nullptr;
			size_t length = 0;
			if (_dupenv_s(&value, &length, name) != 0 || !value) return {};
			std::string text = value;
			free(value);
			return text;
		}

		std::map<std::string, std::string> ParseLine(const std::string& line)
		{
			std::map<std::string, std::string> dims;
			std::stringstream in(line);
			std::string item;
			while (std::getline(in, item, ';'))
			{
				const size_t eq = item.find('=');
				if (eq == std::string::npos) continue;
				dims[Vpv::Trim(item.substr(0, eq))] = Vpv::Trim(item.substr(eq + 1));
			}
			return dims;
		}

		double RateHz(const std::string& text)
		{
			const double v = std::atof(text.c_str());
			// 23.976 / 29.97 / 59.94 are the 1000/1001 rates exactly.
			if (std::abs(v - 23.976) < 0.01) return 24000.0 / 1001.0;
			if (std::abs(v - 29.97) < 0.01) return 30000.0 / 1001.0;
			if (std::abs(v - 59.94) < 0.01) return 60000.0 / 1001.0;
			return v;
		}

		// The [vprenderer] profile text a combination implies.
		std::string ProfileText(const std::map<std::string, std::string>& dims)
		{
			const Vpv::Table& table = Vpv::TierTable();
			std::map<std::string, std::string> keys;
			for (const Vpv::Unit& u : table.units)
			{
				const auto tier = dims.find(std::string(u.id) + ".tier");
				if (tier != dims.end())
					for (const Vpv::Preset& p : u.presets)
						if (tier->second == p.tier)
							for (const Vpv::KeyValue& kv : p.set) keys[kv.key] = kv.value;
				for (const Vpv::Toggle& t : u.toggles)
				{
					const auto v = dims.find(t.key);
					if (v != dims.end()) keys[t.key] = v->second;
				}
			}
			std::string text = "[vprenderer]\n";
			for (const auto& kv : keys) text += kv.first + ": " + kv.second + "\n";
			return text;
		}

		bool ConfigAccepted(const std::string& text, std::string& error)
		{
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			const std::string path = std::string(dir) + "vp-combo-" + std::to_string(GetCurrentProcessId()) + ".cfg";
			std::ofstream(path, std::ios::binary) << text;
			ConfigFile config;
			bool ok = config.Load(path);
			if (!ok) error = "config did not load";
			RendererProfileConfig::Model model;
			if (ok) ok = RendererProfileConfig::Read(config, model, error);
			std::remove(path.c_str());
			return ok;
		}

		// Empty when the combination passes, else the reason.
		std::string RunCombination(const std::map<std::string, std::string>& dims)
		{
			using namespace QueueStress;
			const auto get = [&](const char* k, const char* fallback)
			{
				const auto it = dims.find(k);
				return it == dims.end() ? std::string(fallback) : it->second;
			};
			std::string error;
			if (!ConfigAccepted(ProfileText(dims), error)) return "config rejected: " + error;

			const double rate = RateHz(get("rate", "23.976"));
			const int frames = std::atoi(get("queue_frames", "4").c_str());
			const bool lowLatency = get("low_latency", "off") == "on";
			Scenario s;
			const std::string name = get("name", "combo");
			if (name.rfind("rnd-", 0) == 0)
			{
				// The random line's own faults: its index seeds the scenario's event list.
				uint64_t seed = 1469598103934665603ull;
				for (char c : name) seed = (seed ^ static_cast<unsigned char>(c)) * 1099511628211ull;
				s = Random(seed, 90.0, 90.0);
			}
			s.name = name;
			s.seconds = 90.0;
			s.signalHz = rate;
			s.pictureHz = rate;
			s.displayHz = 60.0;
			s.sw.lowLatency = lowLatency;
			s.sw.renderAhead = get("render_ahead", "off") == "on";
			s.sw.fillGaps = get("interpolation_fill_gaps", "on") == "on";
			s.sw.lookaheadAuto = get("motion.tier", "Mid") != "Classic";
			s.sw.queueFixes = true;   // queue_timing_fixes default on (off is a legacy comparison only)
			s.sw.latchFixes = true;
			RunOptions options;
			options.present.normalTarget = static_cast<size_t>(frames) + 1;
			const Result r = Run(s, options);
			std::string reasons;
			for (const std::string& sig : r.Signatures())
				reasons += sig + " ";
			if (!reasons.empty()) return "queue invariants: " + reasons;

			const double budget = DelayBudget::BudgetForFrames(frames, DelayBudget::StandardRates());
			const DelayBudget::Plan plan = DelayBudget::Compute(DelayBudget::InputsFor(budget, rate));
			if (plan.queueFrames < frames || std::abs(DelayBudget::TotalMs(plan) - budget) > 1e-6)
				return "delay plan: " + std::to_string(plan.queueFrames) + " frames, total " +
					std::to_string(DelayBudget::TotalMs(plan));
			return {};
		}
	}

	TEST_CLASS(CombinationRunner)
	{
	public:
		TEST_METHOD(RunCombinationFile)
		{
			std::vector<std::string> lines;
			const std::string file = Env("VP_COMBO_FILE");
			if (!file.empty())
			{
				std::ifstream in(file);
				Assert::IsTrue(static_cast<bool>(in), L"VP_COMBO_FILE cannot be read");
				std::string line;
				while (std::getline(in, line))
				{
					line = Vpv::Trim(line);
					if (!line.empty() && line[0] != '#') lines.push_back(line);
				}
			}
			else
			{
				lines = {
					"name=builtin-1;rate=23.976;queue_frames=4;low_latency=off;motion.tier=Upper;repair.tier=Upper;scaling.tier=Mid;output.tier=Low;render_ahead=on",
					"name=builtin-2;rate=25;queue_frames=2;low_latency=off;motion.tier=Mid;repair.tier=Mid;interpolation_fill_gaps=on;dejudder_estimate_ahead=on",
					"name=builtin-3;rate=60;queue_frames=3;low_latency=on;motion.tier=Low;repair.tier=Classic;scaling.tier=Classic;output.tier=Classic" };
			}
			const std::string logPath = Env("VP_COMBO_LOG");
			int failed = 0;
			std::string first;
			for (const std::string& line : lines)
			{
				const std::string reason = RunCombination(ParseLine(line));
				if (reason.empty()) continue;
				++failed;
				if (first.empty()) first = line + " # " + reason;
				if (!logPath.empty()) std::ofstream(logPath, std::ios::app) << line << " # " << reason << "\n";
			}
			const std::string summary = std::to_string(lines.size()) + " combinations, " + std::to_string(failed) + " failed";
			Logger::WriteMessage(summary.c_str());
			const std::string message = summary + ": " + first;
			Assert::AreEqual(0, failed, std::wstring(message.begin(), message.end()).c_str());
		}
	};
}
