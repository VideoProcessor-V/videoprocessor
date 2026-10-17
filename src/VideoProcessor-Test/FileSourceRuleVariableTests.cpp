// VP Renderer DirectShow filter (2026-10-17): the profile rules' `source`
// variable - capture (VideoProcessor.exe) or file (the filter's FileHost) -
// parses and selects profiles, is published in the snapshot, and leaves the
// evaluation of configurations that never name it unchanged.
#include "pch.h"
#include "CppUnitTest.h"
#include <ConfigFile.h>
#include <DisplayRuleExpression.h>
#include <MainConfigSchema.h>
#include <RendererProfileConfig.h>
#include <SourceRuleVariable.h>
#include <UnifiedProfileRuntime.h>
#include <Windows.h>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace
	{
		struct SourceRuleTempConfig
		{
			std::string path;
			explicit SourceRuleTempConfig(const char* text)
			{
				char directory[MAX_PATH]{}, file[MAX_PATH]{};
				Assert::IsTrue(GetTempPathA(MAX_PATH, directory) != 0);
				Assert::IsTrue(GetTempFileNameA(directory, "vps", 0, file) != 0);
				path = file;
				std::ofstream out(path, std::ios::trunc);
				out << text;
			}
			~SourceRuleTempConfig() { DeleteFileA(path.c_str()); }
		};

		std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

		bool ReadModel(const char* text, RendererProfileConfig::Model& model, std::string& error)
		{
			SourceRuleTempConfig file(text);
			ConfigFile config;
			Assert::IsTrue(config.Load(file.path, ConfigFile::ReadPolicy::Fresh), L"load");
			return RendererProfileConfig::Read(config, model, error);
		}

		// The display group's automatic choice for these values.
		std::string Pick(const RendererProfileConfig::Model& model, const DisplayRuleExpression::ValueLookup& lookup)
		{
			std::vector<RendererProfileConfig::AutomaticSelection> automatic;
			std::string error;
			Assert::IsTrue(RendererProfileConfig::SelectAutomatic(model, lookup, automatic, error), Wide(error).c_str());
			for (const auto& selection : automatic)
				if (selection.group == "display" && !selection.profiles.empty())
					return selection.profiles.front();
			return std::string();
		}

		bool NoValues(const std::string&, std::string&) { return false; }

		const char* const kSourceRules =
			"[general]\nrenderer: VP Renderer\n"
			"[vprenderer.everyday]\nquality: high\n"
			"[vprenderer.files]\nwhen: $source==\"file\"\nsdr_black_nits: 0.05\n"
			"[vprenderer.live]\nwhen: $source==\"capture\"\nsdr_black_nits: 0.1\n";
	}

	TEST_CLASS(FileSourceRuleVariableTests)
	{
	public:
		TEST_METHOD(SourceIsATextRuleVariable)
		{
			DisplayRuleExpression::ValueType type = DisplayRuleExpression::ValueType::Number;
			Assert::IsTrue(DisplayRuleExpression::GetVariableType("source", type), L"source is known");
			Assert::IsTrue(type == DisplayRuleExpression::ValueType::Text, L"source is text");
			Assert::IsTrue(DisplayRuleExpression::GetVariableType("previous.source", type), L"previous.source");
			Assert::AreEqual("source", SourceRuleVariable::kName);
			Assert::AreEqual("capture", SourceRuleVariable::kCapture);
			Assert::AreEqual("file", SourceRuleVariable::kFile);
		}

		TEST_METHOD(SourceRulesSelectFileAndCaptureProfiles)
		{
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsTrue(ReadModel(kSourceRules, model, error), Wide(error).c_str());
			Assert::AreEqual(std::string("files"),
				Pick(model, SourceRuleVariable::WithSource(NoValues, SourceRuleVariable::kFile)));
			Assert::AreEqual(std::string("live"),
				Pick(model, SourceRuleVariable::WithSource(NoValues, SourceRuleVariable::kCapture)));
			Assert::AreEqual(std::string("everyday"), Pick(model, NoValues),
				L"no source reported: neither source rule matches");
		}

		TEST_METHOD(SourceCombinesWithOtherVariables)
		{
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsTrue(ReadModel(
				"[vprenderer.everyday]\nquality: high\n"
				"[vprenderer.file_films]\nwhen: $source==\"file\" && $content==\"film\"\nsdr_black_nits: 0.05\n",
				model, error), Wide(error).c_str());
			auto withContent = [](const char* content) -> DisplayRuleExpression::ValueLookup
			{
				const std::string value(content);
				return [value](const std::string& name, std::string& result)
				{
					if (name != "content") return false;
					result = value;
					return true;
				};
			};
			Assert::AreEqual(std::string("file_films"),
				Pick(model, SourceRuleVariable::WithSource(withContent("film"), SourceRuleVariable::kFile)));
			Assert::AreEqual(std::string("everyday"),
				Pick(model, SourceRuleVariable::WithSource(withContent("film"), SourceRuleVariable::kCapture)));
			Assert::AreEqual(std::string("everyday"),
				Pick(model, SourceRuleVariable::WithSource(withContent("sports"), SourceRuleVariable::kFile)));
		}

		TEST_METHOD(ConfigurationsWithoutSourceAreUnchanged)
		{
			// The same rules pick the same profiles whether or not a source is reported.
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsTrue(ReadModel(
				"[vprenderer.everyday]\nquality: high\n"
				"[vprenderer.match_day]\nwhen: $content==\"sports\"\nsdr_black_nits: 0.05\n",
				model, error), Wide(error).c_str());
			for (const char* content : { "sports", "film", "" })
			{
				const std::string value(content);
				const DisplayRuleExpression::ValueLookup plain = [value](const std::string& name, std::string& result)
				{
					if (name != "content" || value.empty()) return false;
					result = value;
					return true;
				};
				const std::string expected = Pick(model, plain);
				Assert::AreEqual(expected, Pick(model, SourceRuleVariable::WithSource(plain, SourceRuleVariable::kFile)));
				Assert::AreEqual(expected, Pick(model, SourceRuleVariable::WithSource(plain, SourceRuleVariable::kCapture)));
			}
			Assert::AreEqual(std::string("match_day"), Pick(model, [](const std::string& name, std::string& result)
			{
				if (name != "content") return false;
				result = "sports";
				return true;
			}));
		}

		TEST_METHOD(WithSourceForwardsEveryOtherName)
		{
			const DisplayRuleExpression::ValueLookup inner = [](const std::string& name, std::string& result)
			{
				if (name == "eotf") { result = "pq"; return true; }
				if (name == "source") { result = "inner"; return true; }
				return false;
			};
			const auto lookup = SourceRuleVariable::WithSource(inner, SourceRuleVariable::kFile);
			std::string value;
			Assert::IsTrue(lookup("source", value));
			Assert::AreEqual(std::string("file"), value, L"the host's source wins");
			Assert::IsTrue(lookup("eotf", value));
			Assert::AreEqual(std::string("pq"), value);
			Assert::IsFalse(lookup("width", value));
			Assert::IsFalse(SourceRuleVariable::WithSource(inner, "")("source", value) && value == "file",
				L"an empty source is not reported as file");
		}

		TEST_METHOD(MisspelledSourceVariableIsRejected)
		{
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsFalse(ReadModel(
				"[vprenderer.everyday]\nquality: high\n"
				"[vprenderer.files]\nwhen: $sources==\"file\"\nsdr_black_nits: 0.05\n",
				model, error), L"an unknown variable is still refused");
		}

		TEST_METHOD(RuntimePublishesSourceInTheSnapshot)
		{
			SourceRuleTempConfig file(
				"[general]\npersist_profile_selection: false\n"
				"[vprenderer.everyday]\nquality: high\n"
				"[vprenderer.files]\nwhen: $source==\"file\"\nsdr_black_nits: 0.05\n");
			ConfigFile config;
			Assert::IsTrue(config.Load(file.path, ConfigFile::ReadPolicy::Fresh));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error), Wide(error).c_str());
			for (const char* source : { SourceRuleVariable::kFile, SourceRuleVariable::kCapture })
			{
				UnifiedProfileRuntime::Runtime runtime;
				Assert::IsTrue(runtime.Initialize(config, SourceRuleVariable::WithSource(NoValues, source), error),
					Wide(error).c_str());
				const auto snapshot = runtime.GetSnapshot();
				Assert::IsTrue(snapshot != nullptr);
				std::string published;
				Assert::IsTrue(snapshot->LookupVariable("source", published), L"source is published");
				Assert::AreEqual(std::string(source), published);
				const auto display = snapshot->effectiveSelections.find("display");
				Assert::IsTrue(display != snapshot->effectiveSelections.end() && !display->second.empty());
				Assert::AreEqual(std::string(std::string(source) == "file" ? "files" : "everyday"),
					display->second.front());
			}
			UnifiedProfileRuntime::Runtime unreported;
			Assert::IsTrue(unreported.Initialize(config, NoValues, error), Wide(error).c_str());
			std::string published;
			Assert::IsFalse(unreported.GetSnapshot()->LookupVariable("source", published),
				L"nothing is published when no source is reported");
		}
	};
}
