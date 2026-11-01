// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Cross-check of the C++ light-level LUT against the Python mirror tools/eval/light_level_ref.py:
// 1024 codes per case (LightLevelCrossCheck.generated.h, written by tools/eval/gen_light_level_golden.py).
#include "pch.h"
#include "CppUnitTest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include <VP-V/modules/input/source_analysis/light_level/LightLevelLut.h>
#include "LightLevelCrossCheck.generated.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// "k=v;k=v" (generator format) -> key map.
		std::map<std::string, std::string> ParseKeys(const std::string& text)
		{
			std::map<std::string, std::string> m;
			std::size_t pos = 0;
			while (pos < text.size())
			{
				std::size_t end = text.find(';', pos);
				if (end == std::string::npos) end = text.size();
				const std::string item = text.substr(pos, end - pos);
				const std::size_t eq = item.find('=');
				if (eq != std::string::npos) m[item.substr(0, eq)] = item.substr(eq + 1);
				pos = end + 1;
			}
			return m;
		}
	}

	TEST_CLASS(LightLevelCrossCheckTests)
	{
	public:

		TEST_METHOD(LightLevelCppMatchesPythonMirror)
		{
			for (int n = 0; n < LightLevelCrossCheck::kCaseCount; ++n)
			{
				const LightLevelCrossCheck::Case& k = LightLevelCrossCheck::kCases[n];
				const std::map<std::string, std::string> keys = ParseKeys(k.keys);
				const LightLevel::Settings s = LightLevel::Read([&keys](const std::string& key, std::string& raw)
				{
					const auto it = keys.find(key);
					if (it == keys.end()) return false;
					raw = it->second;
					return true;
				});
				Assert::IsTrue(s.notes.empty(), L"every cross-check key parses without a fallback note");

				LightLevel::Context c;
				c.kind = static_cast<LightLevel::Context::Kind>(k.kind);
				c.fullRange = false;
				c.sourcePeakNits = k.sourcePeak;
				c.sourceMinNits = 0.0;
				c.displayPeakNits = k.displayPeak;
				c.dynamicPeakNits = 0.0;
				const LightLevel::Lut lut = LightLevel::Build(s, c);

				double maxJ = 0.0, maxL = 0.0;
				for (int code = 0; code < LightLevel::kLutSize; ++code)
				{
					const LightLevel::Entry& e = lut.e[static_cast<std::size_t>(code)];
					maxJ = std::max(maxJ, std::fabs(e.J - k.J[code]));
					maxL = std::max(maxL, std::fabs(e.L - k.L[code]) / (1.0 + std::fabs(k.L[code])));
				}
				char message[200];
				std::snprintf(message, sizeof(message), "light level cross-check %s: max |J_cpp - J_py| %.3e, max relative L error %.3e",
					k.name, maxJ, maxL);
				Logger::WriteMessage(message);
				Assert::IsTrue(maxJ <= 1e-9, L"J matches the python mirror within 1e-9");
				Assert::IsTrue(maxL <= 1e-9, L"L matches the python mirror within 1e-9 (relative)");
			}
		}
	};
}
