#include "pch.h"
#include "CppUnitTest.h"

// audit 2026-10-16: display-rule expression and VP-0079 target-shader key
// fixes (three-valued logic, bare Boolean words, exponent numbers, the
// nominal ${source_rate}, key-declared target-shader rules, atomic commit).

#include <ConfigFile.h>
#include <DisplayRuleExpression.h>
#include <StateVariables.h>
#include <VideoState.h>
#include <microsoft_directshow/MadVRShaderLoader.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <windows.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;


namespace
{
	using Values = std::vector<std::pair<std::string, std::string>>;

	DisplayRuleExpression::ValueLookup LookupOf(const Values& values)
	{
		return [values](const std::string& name, std::string& value)
		{
			for (const auto& item : values)
				if (item.first == name) { value = item.second; return true; }
			return false;
		};
	}

	bool Matches(const std::string& source, const Values& values, bool strict = false)
	{
		DisplayRuleExpression::Expression expression;
		std::string error;
		const bool compiled = expression.Compile(source, error, strict);
		Assert::IsTrue(compiled, std::wstring(error.begin(), error.end()).c_str());
		int specificity = 0;
		const bool matched = expression.Matches(LookupOf(values), specificity, error);
		Assert::IsTrue(error.empty(), std::wstring(error.begin(), error.end()).c_str());
		return matched;
	}

	struct TemporaryConfig
	{
		std::string path;
		explicit TemporaryConfig(const char* name, const std::string& contents)
		{
			char directory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(directory), directory) > 0);
			path = std::string(directory) + name;
			std::ofstream(path, std::ios::out | std::ios::trunc) << contents;
		}
		~TemporaryConfig() { DeleteFileA(path.c_str()); }
	};
}


namespace Tests
{
	TEST_CLASS(DisplayRuleExpressionAuditTests)
	{
	public:
		// Item 3: an unavailable variable is Unknown, and Unknown propagates
		// through !, && and || (Kleene); an Unknown rule does not match.
		TEST_METHOD(UnknownVariableIsConsistentAcrossNotEqualAndNegation)
		{
			const Values unknown = { { "width", "1920" }, { "hdr_metadata", "true" } };
			Assert::IsFalse(Matches("${eotf} == pq", unknown));
			Assert::IsFalse(Matches("${eotf} != pq", unknown));
			Assert::IsFalse(Matches("!(${eotf} == pq)", unknown),
				L"!(==) must agree with != while the EOTF is unknown");
			Assert::IsFalse(Matches("!!(${eotf} == pq)", unknown));
			Assert::IsFalse(Matches("!(${eotf} != pq)", unknown));
			// An empty value is unavailable too.
			Assert::IsFalse(Matches("!(${eotf} == pq)", { { "eotf", "" } }));

			// Known operands still decide the result.
			Assert::IsTrue(Matches("${eotf} == pq || ${width} > 0", unknown));
			Assert::IsTrue(Matches("!(${eotf} == pq) || ${hdr_metadata}", unknown));
			Assert::IsFalse(Matches("${eotf} == pq && ${width} > 0", unknown));
			Assert::IsTrue(Matches("!(${eotf} == pq && ${width} < 0)", unknown),
				L"Unknown && false is false, so its negation matches");
			Assert::IsFalse(Matches("!(${eotf} == pq || ${width} < 0)", unknown),
				L"Unknown || false stays unknown under !");

			// With the value known, != and !(==) agree and match.
			const Values sdr = { { "eotf", "SDR" } };
			Assert::IsTrue(Matches("${eotf} != pq", sdr));
			Assert::IsTrue(Matches("!(${eotf} == pq)", sdr));
			Assert::IsFalse(Matches("${eotf} == pq", sdr));

			// An unparseable number is unavailable, not false.
			const Values badWidth = { { "width", "abc" } };
			Assert::IsFalse(Matches("${width} > 5", badWidth));
			Assert::IsFalse(Matches("!(${width} > 5)", badWidth));
			Assert::IsFalse(Matches("${width} != 5", badWidth));

			// No key pressed is a known state: ${key} comparisons are false
			// and their negation matches (unchanged behaviour).
			Assert::IsTrue(Matches("!(${key} == \"F5\")", {}));
			Assert::IsTrue(Matches("!(${key} == \"F5\")", { { "key", "" } }));
			Assert::IsFalse(Matches("${key} == \"F5\"", {}));
			Assert::IsTrue(Matches("${eotf} == pq || ${key} == \"F5\"", { { "key", "F5" } }));
		}

		// Item 4: a Boolean variable followed by a bare word without an
		// operator is no longer read as `== word`.
		TEST_METHOD(BareBooleanAcceptsOnlyTheImplicitTrue)
		{
			std::string error;
			Assert::IsFalse(DisplayRuleExpression::Validate("${hdr_metadata} false", error));
			Assert::IsTrue(error.find("expected comparison operator") != std::string::npos);
			Assert::IsFalse(DisplayRuleExpression::Validate("${interlaced} no", error));
			Assert::IsFalse(DisplayRuleExpression::Validate("${hdr_metadata} true|false", error));
			Assert::IsFalse(DisplayRuleExpression::Validate("${hdr_metadata} pq", error));

			DisplayRuleExpression::Expression expression;
			Assert::IsFalse(expression.Compile("${hdr_metadata} false", error, true),
				L"strict mode must reject the bare word too");
			Assert::IsFalse(expression.Compile("${hdr_metadata} true", error, true));

			// Still accepted: the bare variable, the redundant non-strict
			// `true`, and full comparisons.
			Assert::IsTrue(DisplayRuleExpression::Validate("${hdr_metadata}", error));
			Assert::IsTrue(DisplayRuleExpression::Validate("${hdr_metadata} true", error));
			Assert::IsTrue(DisplayRuleExpression::Validate("${hdr_metadata} == false", error));
			Assert::IsTrue(DisplayRuleExpression::Validate("!${hdr_metadata} && ${interlaced}", error));
			Assert::IsTrue(expression.Compile("${hdr_metadata} && ${interlaced} == false", error, true),
				std::wstring(error.begin(), error.end()).c_str());

			const Values hdr = { { "hdr_metadata", "true" }, { "interlaced", "false" } };
			Assert::IsTrue(Matches("${hdr_metadata}", hdr));
			Assert::IsTrue(Matches("${hdr_metadata} true", hdr));
			Assert::IsFalse(Matches("${interlaced}", hdr));
			Assert::IsTrue(Matches("${hdr_metadata} && ${interlaced} == false", hdr, true));
		}

		// Item 5: a '-' after an exponent marker belongs to the number.
		TEST_METHOD(ExponentNumbersAreNotSplitAsRanges)
		{
			double minimum = 0.0, maximum = 0.0;
			Assert::IsTrue(DisplayRuleExpression::ParseNumberOrRange("1e-3", minimum, maximum));
			Assert::AreEqual(0.001, minimum, 1e-15);
			Assert::AreEqual(0.001, maximum, 1e-15);
			Assert::IsTrue(DisplayRuleExpression::ParseNumberOrRange("2.5E-4", minimum, maximum));
			Assert::AreEqual(0.00025, minimum, 1e-15);
			Assert::IsTrue(DisplayRuleExpression::ParseNumberOrRange("1e-3-2e-3", minimum, maximum));
			Assert::AreEqual(0.001, minimum, 1e-15);
			Assert::AreEqual(0.002, maximum, 1e-15);
			Assert::IsTrue(DisplayRuleExpression::ParseNumberOrRange("23-24", minimum, maximum));
			Assert::AreEqual(23.0, minimum);
			Assert::AreEqual(24.0, maximum);
			Assert::IsTrue(DisplayRuleExpression::ParseNumberOrRange("-5", minimum, maximum));
			Assert::AreEqual(-5.0, minimum);
			Assert::IsTrue(DisplayRuleExpression::ParseNumberOrRange("1e3-2e3", minimum, maximum));
			Assert::AreEqual(1000.0, minimum);
			Assert::AreEqual(2000.0, maximum);
			Assert::IsFalse(DisplayRuleExpression::ParseNumberOrRange("24-23", minimum, maximum));

			std::string error;
			Assert::IsTrue(DisplayRuleExpression::Validate("${cadence} == 1e-3", error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(DisplayRuleExpression::Validate("${width} >= 1e3", error));
			Assert::IsTrue(Matches("${width} == 1e3-2e3", { { "width", "1920" } }));
			Assert::IsFalse(Matches("${width} == 1e3-1.5e3", { { "width", "1920" } }));
		}

		// Item 6: ${source_rate} is the nominal rate rounded to the nearest
		// whole number; ${cadence} stays exact.
		TEST_METHOD(SourceRateRoundsToTheNearestWholeRate)
		{
			using DisplayRuleExpression::NominalSourceRateText;
			Assert::AreEqual("60", NominalSourceRateText(60000.0 / 1001.0).c_str());
			Assert::AreEqual("60", NominalSourceRateText(59.94).c_str());
			Assert::AreEqual("60", NominalSourceRateText(60.0).c_str());
			Assert::AreEqual("24", NominalSourceRateText(24000.0 / 1001.0).c_str());
			Assert::AreEqual("24", NominalSourceRateText(23.976).c_str());
			Assert::AreEqual("24", NominalSourceRateText(24.0).c_str());
			Assert::AreEqual("30", NominalSourceRateText(29.97).c_str());
			Assert::AreEqual("25", NominalSourceRateText(25.0).c_str());
			Assert::AreEqual("50", NominalSourceRateText(50.0).c_str());
			Assert::AreEqual("48", NominalSourceRateText(48000.0 / 1001.0).c_str());
			Assert::AreEqual("120", NominalSourceRateText(120000.0 / 1001.0).c_str());
			Assert::AreEqual("0", NominalSourceRateText(0.0).c_str());
			Assert::AreEqual("0", NominalSourceRateText(-1.0).c_str());
			Assert::AreEqual("0", NominalSourceRateText(
				std::numeric_limits<double>::quiet_NaN()).c_str());

			// StateVariables publishes the rounded rate and the exact cadence.
			VideoStateComPtr state = new VideoState();
			state->valid = true;
			state->displayMode = std::make_shared<DisplayMode>(1920, 1080, false, 60000, 1001);
			std::string value;
			Assert::IsTrue(StateVariables::LookupVideoState(*state, "source_rate", value));
			Assert::AreEqual("60", value.c_str());
			Assert::IsTrue(StateVariables::LookupVideoState(*state, "cadence", value));
			Assert::AreEqual(60000.0 / 1001.0, std::stod(value), 1e-9);
			state->displayMode = std::make_shared<DisplayMode>(1920, 1080, false, 24000, 1001);
			Assert::IsTrue(StateVariables::LookupVideoState(*state, "source_rate", value));
			Assert::AreEqual("24", value.c_str());

			// A 59.94p source now meets `${source_rate} > 59` and no longer
			// meets `${source_rate} == 59`; 23.976p is 24, not 23.
			const auto lookup = StateVariables::VideoStateLookup(state);
			state->displayMode = std::make_shared<DisplayMode>(1920, 1080, false, 60000, 1001);
			int specificity = 0;
			std::string error;
			Assert::IsTrue(DisplayRuleExpression::Matches("${source_rate} > 59", lookup, specificity, error));
			Assert::IsFalse(DisplayRuleExpression::Matches("${source_rate} == 59", lookup, specificity, error));
			Assert::IsTrue(DisplayRuleExpression::Matches("${cadence} == 60000/1001", lookup, specificity, error));
			Assert::IsFalse(DisplayRuleExpression::Matches("${cadence} == 60", lookup, specificity, error),
				L"${cadence} stays the exact rate");
		}

		// Items 1 and 2: a key press acts only on members/reset rules that
		// declare the pressed chord, and a failed press changes nothing.
		TEST_METHOD(TargetShaderKeyIgnoresRulesThatDoNotDeclareTheKey)
		{
			MadVRShaderLoader::ResetTargetShaderSelections();
			TemporaryConfig file("VideoProcessor-audit-target-shader-key.cfg",
				"[shader.nls]\n"
				"[shader.nls.classic]\n"
				"when: ${transfer} == \"HDR\"\nshortcut: Ctrl+N\n"
				"shader_type: custom\nhlsl_file: Classic.hlsl\n"
				"[shader.nls.protected]\n"
				"when: ${transfer} == \"HDR\"\nshortcut: Ctrl+P\n"
				"shader_type: custom\nhlsl_file: Protected.hlsl\n"
				"[shader.extra]\n"
				"when: ${transfer} == \"HDR\"\nshortcut: Ctrl+E\n"
				"[shader.extra.boost]\n"
				"shortcut: Ctrl+B\nshader_type: custom\nhlsl_file: Boost.hlsl\n");
			ConfigFile config;
			Assert::IsTrue(config.Load(file.path));
			const auto hdr = [](const std::string& name, std::string& value)
			{
				if (name == "transfer") { value = "HDR"; return true; }
				return false;
			};
			std::vector<std::string> names;
			std::string reason;
			// Before the fix both members' automatic conditions were true, so
			// Ctrl+N "selected multiple members" of the single group, and the
			// extra group's reset rule (true for HDR) was consumed as well.
			Assert::IsTrue(MadVRShaderLoader::ResolveTargetShaderKeyWithSource(
				config, "Ctrl+N", hdr, names, reason),
				std::wstring(reason.begin(), reason.end()).c_str());
			Assert::IsTrue(std::find(names.begin(), names.end(), "nls.classic") != names.end());
			Assert::IsTrue(std::find(names.begin(), names.end(), "nls.protected") == names.end());

			// Select the extra member by key; an unrelated key (Ctrl+N again)
			// must not run the extra group's HDR-true reset rule.
			Assert::IsTrue(MadVRShaderLoader::ResolveTargetShaderKeyWithSource(
				config, "Ctrl+B", hdr, names, reason),
				std::wstring(reason.begin(), reason.end()).c_str());
			Assert::IsTrue(std::find(names.begin(), names.end(), "extra.boost") != names.end());
			Assert::IsTrue(MadVRShaderLoader::ResolveTargetShaderKeyWithSource(
				config, "ctrl+n", hdr, names, reason),
				std::wstring(reason.begin(), reason.end()).c_str());
			Assert::IsTrue(std::find(names.begin(), names.end(), "extra.boost") != names.end(),
				L"a reset rule that does not declare the key must not clear the group");
			// Its own chord still resets it.
			Assert::IsTrue(MadVRShaderLoader::ResolveTargetShaderKeyWithSource(
				config, "Ctrl+E", hdr, names, reason),
				std::wstring(reason.begin(), reason.end()).c_str());
			Assert::IsTrue(std::find(names.begin(), names.end(), "extra.boost") == names.end());

			// A key that no rule declares does not select a group.
			Assert::IsFalse(MadVRShaderLoader::ResolveTargetShaderKeyWithSource(
				config, "Ctrl+Z", hdr, names, reason));
			MadVRShaderLoader::ResetTargetShaderSelections();
		}

		TEST_METHOD(TargetShaderKeyFailureLeavesSelectionsUnchanged)
		{
			MadVRShaderLoader::ResetTargetShaderSelections();
			// Ctrl+K selects a member of group a (listed first) and two
			// members of the single group b, which fails.
			TemporaryConfig file("VideoProcessor-audit-target-shader-atomic.cfg",
				"[shader.a]\n"
				"[shader.a.one]\nshortcut: Ctrl+K\nshader_type: custom\nhlsl_file: One.hlsl\n"
				"[shader.a.two]\nshortcut: Ctrl+T\nshader_type: custom\nhlsl_file: Two.hlsl\n"
				"[shader.b]\n"
				"[shader.b.left]\nshortcut: Ctrl+K\nshader_type: custom\nhlsl_file: Left.hlsl\n"
				"[shader.b.right]\nshortcut: Ctrl+K\nshader_type: custom\nhlsl_file: Right.hlsl\n");
			ConfigFile config;
			Assert::IsTrue(config.Load(file.path));
			const DisplayRuleExpression::ValueLookup none;
			std::vector<std::string> names;
			std::string reason;
			Assert::IsTrue(MadVRShaderLoader::ResolveTargetShaderKeyWithSource(
				config, "Ctrl+T", none, names, reason),
				std::wstring(reason.begin(), reason.end()).c_str());
			Assert::IsTrue(std::find(names.begin(), names.end(), "a.two") != names.end());

			Assert::IsFalse(MadVRShaderLoader::ResolveTargetShaderKeyWithSource(
				config, "Ctrl+K", none, names, reason));
			Assert::IsTrue(reason.find("multiple members") != std::string::npos);

			// Group a must still hold a.two, not the a.one the failed press
			// computed before group b failed.
			Assert::IsTrue(MadVRShaderLoader::ResolveTargetShaderKeyWithSource(
				config, std::string(), none, names, reason),
				std::wstring(reason.begin(), reason.end()).c_str());
			Assert::IsTrue(std::find(names.begin(), names.end(), "a.two") != names.end());
			Assert::IsTrue(std::find(names.begin(), names.end(), "a.one") == names.end());
			MadVRShaderLoader::ResetTargetShaderSelections();
		}
	};
}
