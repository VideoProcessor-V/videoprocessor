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

				// Tolerances: J absolute 1e-9; L, S and V relative |d| / (1 + |ref|) 1e-7 for S and V (absolute floor 1,
				// so tiny values are compared absolutely at 1e-7) and 1e-9 for L; sdrCode absolute 1e-9 (codes are 0..1023).
				double maxJ = 0.0, maxL = 0.0, maxS = 0.0, maxV = 0.0, maxC = 0.0, maxO = 0.0;
				for (int code = 0; code < LightLevel::kLutSize; ++code)
				{
					const LightLevel::Entry& e = lut.e[static_cast<std::size_t>(code)];
					const double refCode = k.sdrCode ? k.sdrCode[code] : static_cast<double>(code);
					maxJ = std::max(maxJ, std::fabs(e.J - k.J[code]));
					maxL = std::max(maxL, std::fabs(e.L - k.L[code]) / (1.0 + std::fabs(k.L[code])));
					maxS = std::max(maxS, std::fabs(e.S - k.S[code]) / (1.0 + std::fabs(k.S[code])));
					maxV = std::max(maxV, std::fabs(e.V - k.V[code]) / (1.0 + std::fabs(k.V[code])));
					maxC = std::max(maxC, std::fabs(e.sdrCode - refCode));
					maxO = std::max(maxO, std::fabs(e.Sout - (k.Sout ? k.Sout[code] : 1.0)));
				}
				char message[320];
				std::snprintf(message, sizeof(message),
					"light level cross-check %s: max |dJ| %.3e, rel dL %.3e, rel dS %.3e, rel dV %.3e, |dSdrCode| %.3e, |dSout| %.3e",
					k.name, maxJ, maxL, maxS, maxV, maxC, maxO);
				Logger::WriteMessage(message);
				Assert::IsTrue(maxJ <= 1e-9, L"J matches the python mirror within 1e-9");
				Assert::IsTrue(maxL <= 1e-9, L"L matches the python mirror within 1e-9 (relative)");
				Assert::IsTrue(maxS <= 1e-7, L"S matches the python mirror within 1e-7 (relative, floor 1)");
				Assert::IsTrue(maxV <= 1e-7, L"V matches the python mirror within 1e-7 (relative, floor 1)");
				Assert::IsTrue(maxC <= 1e-9, L"sdrCode matches the python mirror within 1e-9");
				Assert::IsTrue(maxO <= 1e-9, L"Sout matches the python mirror within 1e-9");
			}
		}
	};
}
