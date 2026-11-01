// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Every id in src/VP-V/core/FallbackCatalog.h has its own plain sentence.

#include <VP-V/core/FallbackPlainText.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace FallbackPlainTextTest
{
	TEST_CLASS(FallbackPlainTextTests)
	{
	public:
		TEST_METHOD(EveryCatalogIdHasItsOwnSentence)
		{
			const char* ids[] = { "dither", "BTB/WTW", "re-base", "detail pulse", "texture restore", "deband", "motion",
				"motion consumers", "presentation", "refresh rate", "calibration LUT", "calibration patch", "output range",
				"output request", "geometry", "subtitle detection", "SDR tone shape", "output scaling", "early upload",
				"early upload frames", "vector_engine", "vector_engine_producer", "vector_engine_global" };
			const std::string unknownPrefix = "A processing step was switched off";
			for (const char* id : ids)
			{
				const std::string text = FallbackNoticeText(id, std::string());
				Assert::IsFalse(text.empty(), L"sentence is empty");
				Assert::IsTrue(text.compare(0, unknownPrefix.size(), unknownPrefix) != 0, L"id fell through to the default");
				Assert::IsTrue(text != id, L"sentence is the raw id");
				Assert::IsTrue(text.find(' ') != std::string::npos, L"sentence is not a sentence");
			}
		}

		TEST_METHOD(ReasonIsAppendedInBrackets)
		{
			const std::string plain = FallbackNoticeText("deband", std::string());
			Assert::AreEqual(plain + " (shader failed)", FallbackNoticeText("deband", "shader failed"));
		}

		TEST_METHOD(UnknownIdUsesTheDefaultSentence)
		{
			Assert::AreEqual(std::string("A processing step was switched off: nope"), FallbackNoticeText("nope", "x"));
		}
	};
}
