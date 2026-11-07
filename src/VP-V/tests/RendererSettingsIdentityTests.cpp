// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// M2 P3-0 identity kit, settings layer (doc 52 section 1.4). The renderer's own loader evaluates every
// case of the frozen corpus tests/vpv/settings-identity-corpus.txt (tools/gen_settings_identity_corpus.py:
// every renderer key at probe, boundary and invalid values, every sample configuration, every key at
// once); the settings dump (every field of RendererSettings, Bill fields first, name=value) of each case is
// compared with the checked-in golden as a DIFF against the first (empty) case, and the two effective
// fingerprints (live and restart-relevant) are compared as an equivalence PARTITION: for all i, j
// fp(i) == fp(j) is unchanged (the string order may change). Moving the VP-V settings out of the host
// file must leave this golden byte-identical.
//
// A mismatch is either a bug or an intended settings change; review every line, then regenerate with
// VP_TEST_REGOLDEN=1 and commit the golden with the change that caused it.

#include "SettingsTestSupport.h"

#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace RendererSettingsIdentityTest
{
	using namespace SettingsTestSupport;

	namespace
	{
		std::string GoldenPath() { return RepoRoot() + "src\\VP-V\\tests\\golden\\settings-identity.golden.txt"; }

		std::string Normalise(std::string text)
		{
			text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
			return text;
		}

		struct Case { std::string name, text; };

		std::vector<Case> LoadCorpus()
		{
			std::vector<Case> cases;
			const std::string corpus = Normalise(ReadFile(RepoRoot() + "tests\\vpv\\settings-identity-corpus.txt"));
			for (const std::string& line : Split(corpus, '\n'))
			{
				if (line.rfind("### ", 0) == 0) { cases.push_back({ line.substr(4), std::string() }); continue; }
				if (cases.empty() || (line.rfind("# ", 0) == 0 && cases.empty())) continue;
				cases.back().text += line + "\n";
			}
			return cases;
		}

		class DumpRenderer
		{
		public:
			DumpRenderer()
			{
				const std::string dll = Here() + "vprenderer\\VideoProcessorVPRenderer.dll";
				module_ = LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
				if (!module_) return;
				dump_ = reinterpret_cast<Export>(GetProcAddress(module_, "VideoProcessorLibplaceboSettingsDumpForTest"));
				char temp[MAX_PATH] = {};
				GetTempPathA(MAX_PATH, temp);
				folder_ = std::string(temp) + "vp-identity-tests-" + std::to_string(GetCurrentProcessId()) + "\\";
				CreateDirectoryA(folder_.c_str(), nullptr);
			}
			~DumpRenderer()
			{
				if (!folder_.empty())
				{
					for (int i = 0; i < counter_; ++i) DeleteFileA((folder_ + "c" + std::to_string(i) + ".cfg").c_str());
					RemoveDirectoryA(folder_.c_str());
				}
			}
			bool Ready() const { return module_ && dump_; }

			// "" when the configuration did not load.
			std::string Evaluate(const std::string& text)
			{
				const std::string path = folder_ + "c" + std::to_string(counter_++) + ".cfg";
				{
					std::ofstream out(path, std::ios::trunc | std::ios::binary);
					out << text;
				}
				std::vector<char> buffer(1 << 20);
				const int n = dump_(path.c_str(), buffer.data(), static_cast<int>(buffer.size()));
				if (n <= 0) return std::string();
				return Normalise(buffer.data());
			}

		private:
			using Export = int(__cdecl*)(const char*, char*, int);
			HMODULE module_ = nullptr;
			Export dump_ = nullptr;
			std::string folder_;
			int counter_ = 0;
		};

		struct Parsed
		{
			std::vector<std::string> lines;   // dump lines without the machine-dependent configuration identity / path
			std::string live, restart;       // fingerprints without their first two fields
		};

		std::string DropLeadingFields(const std::string& fingerprint)
		{
			const std::vector<std::string> fields = Split(fingerprint, '|');
			std::string out;
			for (size_t i = 2; i < fields.size(); ++i) out += (i > 2 ? "|" : "") + fields[i];
			return out;
		}

		Parsed Parse(const std::string& dump)
		{
			Parsed p;
			const size_t live = dump.find("@@fingerprint-live\n");
			const size_t restart = dump.find("@@fingerprint-restart\n");
			Assert::IsTrue(live != std::string::npos && restart != std::string::npos, L"dump sections");
			const std::string head = dump.substr(0, live);
			for (const std::string& line : Split(head, '\n'))
			{
				if (line.empty() || line.rfind("configurationIdentity=", 0) == 0 || line.rfind("configurationPath=", 0) == 0) continue;
				p.lines.push_back(line);
			}
			auto section = [&](size_t from, size_t after, size_t to) {
				std::string s = dump.substr(after, to - after);
				while (!s.empty() && s.back() == '\n') s.pop_back();
				(void)from;
				return DropLeadingFields(s);
			};
			p.live = section(live, live + 19, restart);
			p.restart = section(restart, restart + 22, dump.size());
			return p;
		}

		std::string BuildGolden(DumpRenderer& renderer)
		{
			const std::vector<Case> cases = LoadCorpus();
			Assert::IsTrue(cases.size() > 100, L"the settings identity corpus");
			std::ostringstream out;
			out << "# settings identity golden (RendererSettingsIdentityTests); regenerate with VP_TEST_REGOLDEN=1 after review\n";
			std::set<std::string> baseline;
			std::map<std::string, int> liveGroups, restartGroups;
			bool first = true;
			for (const Case& c : cases)
			{
				const std::string dump = renderer.Evaluate(c.text);
				out << "### " << c.name << "\n";
				if (dump.empty()) { out << "(not loaded)\n"; continue; }
				const Parsed p = Parse(dump);
				const int liveGroup = liveGroups.emplace(p.live, static_cast<int>(liveGroups.size())).first->second;
				const int restartGroup = restartGroups.emplace(p.restart, static_cast<int>(restartGroups.size())).first->second;
				out << "fp live=g" << liveGroup << " restart=g" << restartGroup << "\n";
				if (first)
				{
					baseline.insert(p.lines.begin(), p.lines.end());
					for (const std::string& line : p.lines) out << line << "\n";
					first = false;
					continue;
				}
				for (const std::string& line : p.lines)
					if (!baseline.count(line)) out << "+ " << line << "\n";
			}
			return out.str();
		}
	}

	TEST_CLASS(RendererSettingsIdentityTests)
	{
	public:
		TEST_METHOD(SettingsDumpAndFingerprintPartitionMatchTheGolden)
		{
			DumpRenderer renderer;
			Assert::IsTrue(renderer.Ready(), L"the renderer DLL and its settings dump export");
			const std::string actual = BuildGolden(renderer);
			Assert::IsTrue(actual.size() > 10000, L"the golden text was produced");

			char* regolden = nullptr;
			size_t regoldenLength = 0;
			const bool regenerate = _dupenv_s(&regolden, &regoldenLength, "VP_TEST_REGOLDEN") == 0 && regolden && std::string(regolden) == "1";
			std::free(regolden);
			if (regenerate)
			{
				std::ofstream file(GoldenPath(), std::ios::binary | std::ios::trunc);
				file << actual;
				Logger::WriteMessage(("REGOLDEN wrote " + GoldenPath() + "\n").c_str());
				return;
			}

			const std::string expected = Normalise(ReadFile(GoldenPath()));
			Assert::IsTrue(!expected.empty(), L"src\\VP-V\\tests\\golden\\settings-identity.golden.txt exists (generate with VP_TEST_REGOLDEN=1)");
			if (expected == actual) return;
			const std::vector<std::string> a = Split(actual, '\n'), e = Split(expected, '\n');
			std::ostringstream diff;
			int shown = 0, total = 0;
			std::string caseName;
			for (size_t i = 0; i < std::max(a.size(), e.size()); ++i)
			{
				const std::string got = i < a.size() ? a[i] : std::string("(end)");
				const std::string want = i < e.size() ? e[i] : std::string("(end)");
				if (want.rfind("### ", 0) == 0) caseName = want;
				if (got == want) continue;
				++total;
				if (shown++ < 25) diff << "\n  " << caseName << " line " << (i + 1) << ": golden '" << want << "' now '" << got << "'";
			}
			const std::string message = "settings identity differs from the golden in " + std::to_string(total) + " line(s):" + diff.str();
			Logger::WriteMessage((message + "\n").c_str());
			Assert::Fail(std::wstring(message.begin(), message.end()).c_str());
		}
	};
}
