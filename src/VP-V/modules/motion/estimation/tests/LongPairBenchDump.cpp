// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Bench dump for tools/eval/motion_bench/longpair_bench.py (rows: hint benefit, long-pair accuracy on I-to-I spans).
// Real GPU, run by the lead only: VP_LONGPAIR_BENCH_DIR=<root>\<scene>\longpair and VP_TEST_GPU=hardware.
// Reads half/f_%06d.y8 (640x360 8-bit), pairs.csv (span,from,to) and hint/h_<from>_<to>.bin (int16 S10.5, 4x4 grid, forward);
// writes out/<mode>/l_<from>_<to>.bin (int16 fwd, int16 bwd, u8 fwd cost, u8 bwd cost; little endian, no header) and
// out/<mode>/path.txt for mode nohint (no hint buffer) and hint (hints on). Without the env variable the test only reports.
#include <VP-V/modules/motion/estimation/longpair/LongPairService.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		bool ReadAll(const std::string& path, std::vector<uint8_t>& out)
		{
			std::ifstream f(path, std::ios::binary);
			if (!f)
				return false;
			out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
			return true;
		}

		void MakeDir(const std::string& path) { CreateDirectoryA(path.c_str(), nullptr); }

		void WriteBin(const std::string& path, const VpLongPair::Field& f)
		{
			std::ofstream o(path, std::ios::binary);
			o.write(reinterpret_cast<const char*>(f.forward.data()), static_cast<std::streamsize>(f.forward.size() * 2));
			o.write(reinterpret_cast<const char*>(f.backward.data()), static_cast<std::streamsize>(f.backward.size() * 2));
			o.write(reinterpret_cast<const char*>(f.forwardCost.data()), static_cast<std::streamsize>(f.forwardCost.size()));
			o.write(reinterpret_cast<const char*>(f.backwardCost.data()), static_cast<std::streamsize>(f.backwardCost.size()));
		}
	}

	TEST_CLASS(LongPairBenchDumpTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(LongPairBenchDump)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LongPairBenchDump)
		{
			char* env = nullptr;
			size_t envLen = 0;
			_dupenv_s(&env, &envLen, "VP_LONGPAIR_BENCH_DIR");
			if (!env || !*env)
			{
				Logger::WriteMessage("LONGPAIR_BENCH skipped: VP_LONGPAIR_BENCH_DIR is not set\n");
				return;
			}
			const std::string dir = env;
			free(env);
			std::ifstream pairs(dir + "\\pairs.csv");
			Assert::IsTrue(static_cast<bool>(pairs), L"pairs.csv not found");
			std::string line;
			std::getline(pairs, line);   // header
			std::vector<std::pair<int, int>> list;
			while (std::getline(pairs, line))
			{
				int span = 0, from = 0, to = 0;
				if (sscanf_s(line.c_str(), "%d,%d,%d", &span, &from, &to) == 3)
					list.push_back({ from, to });
			}
			const int w = 640, h = 360;
			std::map<int, std::vector<uint8_t>> frames;
			auto frame = [&](int t) -> const std::vector<uint8_t>&
			{
				auto it = frames.find(t);
				if (it == frames.end())
				{
					char name[64];
					snprintf(name, sizeof(name), "\\half\\f_%06d.y8", t);
					std::vector<uint8_t> v;
					Assert::IsTrue(ReadAll(dir + name, v) && v.size() == static_cast<size_t>(w) * h, L"half frame missing or wrong size");
					it = frames.emplace(t, std::move(v)).first;
				}
				return it->second;
			};
			MakeDir(dir + "\\out");
			int nvofPairs = 0, fallbackPairs = 0;
			for (const char* mode : { "nohint", "hint" })
			{
				MakeDir(dir + "\\out\\" + mode);
				VpLongPair::Config cfg;
				cfg.useNvof = true;
				VpLongPair::Service svc(cfg);
				VpLongPair::Path last = VpLongPair::Path::None;
				for (const auto& p : list)
				{
					VpLongPair::HintField hf;
					bool useHint = std::string(mode) == "hint";
					if (useHint)
					{
						char name[96];
						snprintf(name, sizeof(name), "\\hint\\h_%d_%d.bin", p.first, p.second);
						std::vector<uint8_t> raw;
						Assert::IsTrue(ReadAll(dir + name, raw) && raw.size() == static_cast<size_t>(160) * 90 * 4, L"hint file missing or wrong size");
						hf.gridW = 160; hf.gridH = 90; hf.cellPx = 4;
						hf.vx.resize(160 * 90); hf.vy.resize(160 * 90);
						const int16_t* s = reinterpret_cast<const int16_t*>(raw.data());
						for (size_t i = 0; i < hf.vx.size(); ++i)
						{
							hf.vx[i] = VpLongPair::FromS105(s[i * 2]);
							hf.vy[i] = VpLongPair::FromS105(s[i * 2 + 1]);
						}
					}
					const VpLongPair::Result r = svc.RunNow(frame(p.first).data(), frame(p.second).data(), w, h, useHint ? &hf : nullptr);
					Assert::IsTrue(r.ok, L"no path produced a field");
					last = r.path;
					(r.path == VpLongPair::Path::SeededSearch ? fallbackPairs : nvofPairs) += 1;
					char name[96];
					snprintf(name, sizeof(name), "\\out\\%s\\l_%d_%d.bin", mode, p.first, p.second);
					WriteBin(dir + name, r.field);
					if (r.path == VpLongPair::Path::SeededSearch)
						Logger::WriteMessage((std::string("LONGPAIR_BENCH fallback: ") + svc.StatusLine() + "\n").c_str());
				}
				std::ofstream(dir + "\\out\\" + mode + "\\path.txt") << VpLongPair::PathName(last) << "\n";
			}
			char msg[160];
			snprintf(msg, sizeof(msg), "LONGPAIR_BENCH pairs=%zu nvof=%d seeded_fallback=%d\n", list.size(), nvofPairs, fallbackPairs);
			Logger::WriteMessage(msg);
		}
	};
}
