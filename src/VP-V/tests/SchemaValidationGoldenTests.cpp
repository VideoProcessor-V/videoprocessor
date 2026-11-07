// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// M2 P3-0 identity kit, schema layer (doc 52 section 1.4): every renderer key of the frozen settings corpus
// (tests/vpv/settings-identity-corpus.txt, "key=value" cases) through RendererProfileConfig::ValidateBaseSetting
// and ValidateProfileSetting for every profile group, and every rule key of the startup schema
// (MainConfigSchema, [general] and the other rule lists) at probe values through MainConfigSchema::Validate,
// with the error text. Moving the VP-V validators out of RendererProfileConfig.h / MainConfigSchema.h
// (a registered SettingsExtension) must leave src\VP-V\tests\golden\schema-validation.golden.txt unchanged.
//
// The checked keys are the ones recorded in the golden (not the current rule lists), so a key that
// stops being validated shows up as a differing line. Regenerate with VP_TEST_REGOLDEN=1 after review.

#include "SettingsTestSupport.h"

#include <ConfigFile.h>
#include <MainConfigSchema.h>
#include <RendererProfileConfig.h>

#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SchemaValidationGoldenTest
{
	using namespace SettingsTestSupport;

	namespace
	{
		std::string GoldenPath() { return RepoRoot() + "src\\VP-V\\tests\\golden\\schema-validation.golden.txt"; }

		std::string Normalise(std::string text)
		{
			text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
			return text;
		}

		const char* const kGroups[] = { "input", "scaling", "motion", "display", "output", "viewport" };
		const char* const kMainProbes[] = { "on", "off", "true", "0", "1", "-1", "2.5", "1000", "auto", "zzz_invalid", "" };

		std::string OneLine(std::string s)
		{
			for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
			return s;
		}

		std::string ProfileLine(const std::string& key, const std::string& value)
		{
			std::ostringstream o;
			o << key << "=" << value << " | base=" << (RendererProfileConfig::ValidateBaseSetting(key, value) ? 1 : 0);
			for (const char* group : kGroups)
			{
				std::string expected;
				const bool ok = RendererProfileConfig::ValidateProfileSetting(group, key, value, expected);
				o << " | " << group << "=" << (ok ? 1 : 0);
				if (!ok) o << ":" << OneLine(expected);
			}
			return o.str();
		}

		class MainEvaluator
		{
		public:
			MainEvaluator()
			{
				char temp[MAX_PATH] = {};
				GetTempPathA(MAX_PATH, temp);
				folder_ = std::string(temp) + "vp-schema-tests-" + std::to_string(GetCurrentProcessId()) + "\\";
				CreateDirectoryA(folder_.c_str(), nullptr);
			}
			~MainEvaluator()
			{
				for (int i = 0; i < counter_; ++i) DeleteFileA((folder_ + "c" + std::to_string(i) + ".cfg").c_str());
				RemoveDirectoryA(folder_.c_str());
			}
			std::string Line(const std::string& section, const std::string& key, const std::string& value)
			{
				const std::string path = folder_ + "c" + std::to_string(counter_++) + ".cfg";
				{
					std::ofstream out(path, std::ios::trunc | std::ios::binary);
					out << "[" << section << "]\n" << key << ": " << value << "\n";
				}
				ConfigFile config;
				std::ostringstream o;
				o << "[" << section << "] " << key << "=" << value << " -> ";
				if (!config.Load(path, ConfigFile::ReadPolicy::Fresh)) { o << "not loaded"; return o.str(); }
				std::string error;
				const bool ok = MainConfigSchema::Validate(config, error);
				o << (ok ? "ok" : OneLine(error));
				return o.str();
			}
		private:
			std::string folder_;
			int counter_ = 0;
		};

		std::vector<std::pair<std::string, std::vector<ConfigSchema::KeyRule>>> MainRuleLists()
		{
			return {
				{ "general", MainConfigSchema::WithVpKeys("general", MainConfigSchema::GeneralRules()) },
				{ "command_line", MainConfigSchema::WithVpKeys("general", MainConfigSchema::GeneralRules()) },
				{ "queue", MainConfigSchema::WithVpKeys("queue", MainConfigSchema::QueueRules()) },
				{ "directshow", MainConfigSchema::DirectShowRules() },
				{ "vprenderer.input_processing", MainConfigSchema::VpRendererInputRules() },
				{ "decklink", MainConfigSchema::WithVpKeys("decklink", MainConfigSchema::DeckLinkRules()) },
				{ "directshow.conversion", MainConfigSchema::ConversionRules() },
				{ "directshow.ppm", MainConfigSchema::PpmRules() },
				{ "queue_recovery", MainConfigSchema::RecoveryRules() },
				{ "lldv", MainConfigSchema::LldvRules() },
				{ "logging", MainConfigSchema::WithVpKeys("logging", MainConfigSchema::LoggingRules()) },
			};
		}

		// A line of a case list: "profile-key" (key=value) or "main-key" ([section] key=value).
		std::string Regenerate()
		{
			std::ostringstream out;
			out << "# schema validation golden (SchemaValidationGoldenTests); regenerate with VP_TEST_REGOLDEN=1 after review\n";
			out << "### profile\n";
			std::set<std::string> seen;
			for (const std::string& line : Split(Normalise(ReadFile(RepoRoot() + "tests\\vpv\\settings-identity-corpus.txt")), '\n'))
			{
				if (line.rfind("### ", 0) != 0) continue;
				const std::string name = line.substr(4);
				const size_t eq = name.find('=');
				if (eq == std::string::npos || name.find(' ') != std::string::npos) continue;
				if (seen.insert(name).second) out << ProfileLine(name.substr(0, eq), name.substr(eq + 1)) << "\n";
			}
			out << "### main\n";
			MainEvaluator main;
			for (const auto& list : MainRuleLists())
				for (const ConfigSchema::KeyRule& rule : list.second)
					for (const char* probe : kMainProbes)
						out << main.Line(list.first, rule.key, probe) << "\n";
			return out.str();
		}

		// Re-evaluates the keys recorded in `golden` with today's code.
		std::string Recompute(const std::string& golden)
		{
			std::ostringstream out;
			MainEvaluator main;
			std::string mode;
			for (const std::string& line : Split(golden, '\n'))
			{
				if (line.empty() || line[0] == '#') { if (!line.empty()) out << line << "\n"; continue; }
				if (line.rfind("### ", 0) == 0) { mode = line.substr(4); out << line << "\n"; continue; }
				if (mode == "profile")
				{
					const size_t bar = line.find(" | ");
					const std::string kv = line.substr(0, bar);
					const size_t eq = kv.find('=');
					out << ProfileLine(kv.substr(0, eq), kv.substr(eq + 1)) << "\n";
				}
				else if (mode == "main")
				{
					const size_t close = line.find("] ");
					const size_t arrow = line.rfind(" -> ");
					const std::string section = line.substr(1, close - 1);
					const std::string kv = line.substr(close + 2, arrow - close - 2);
					const size_t eq = kv.find('=');
					out << main.Line(section, kv.substr(0, eq), kv.substr(eq + 1)) << "\n";
				}
			}
			return out.str();
		}
	}

	TEST_CLASS(SchemaValidationGoldenTests)
	{
	public:
		TEST_METHOD(ValidationOfEveryKnownKeyMatchesTheCheckedInGolden)
		{
			char* regolden = nullptr;
			size_t regoldenLength = 0;
			const bool regenerate = _dupenv_s(&regolden, &regoldenLength, "VP_TEST_REGOLDEN") == 0 && regolden && std::string(regolden) == "1";
			std::free(regolden);
			if (regenerate)
			{
				const std::string text = Regenerate();
				Assert::IsTrue(text.size() > 10000, L"the golden text was produced");
				std::ofstream file(GoldenPath(), std::ios::binary | std::ios::trunc);
				file << text;
				Logger::WriteMessage(("REGOLDEN wrote " + GoldenPath() + "\n").c_str());
				return;
			}
			const std::string expected = Normalise(ReadFile(GoldenPath()));
			Assert::IsTrue(!expected.empty(), L"src\\VP-V\\tests\\golden\\schema-validation.golden.txt exists (generate with VP_TEST_REGOLDEN=1)");
			const std::string actual = Recompute(expected);
			if (actual == expected) return;
			const std::vector<std::string> a = Split(actual, '\n'), e = Split(expected, '\n');
			std::ostringstream diff;
			int shown = 0, total = 0;
			for (size_t i = 0; i < std::max(a.size(), e.size()); ++i)
			{
				const std::string got = i < a.size() ? a[i] : std::string("(end)");
				const std::string want = i < e.size() ? e[i] : std::string("(end)");
				if (got == want) continue;
				++total;
				if (shown++ < 25) diff << "\n  golden '" << want << "'\n  now    '" << got << "'";
			}
			const std::string message = "schema validation differs from the golden in " + std::to_string(total) + " line(s):" + diff.str();
			Logger::WriteMessage((message + "\n").c_str());
			Assert::Fail(std::wstring(message.begin(), message.end()).c_str());
		}
	};
}
