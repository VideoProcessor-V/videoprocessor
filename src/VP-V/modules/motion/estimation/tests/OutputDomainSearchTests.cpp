// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// W2-C8 step 1: the sigma-delta's own hierarchical block search (LUMAQ / LUMAC / MV passes) moved into motion.estimation
// (fallback/OutputDomainSearchGlsl.h) and is named by the shader with one `// VP_INCLUDE motion.output_domain_search` line.
// These CPU tests pin that the expanded shader is BYTE-IDENTICAL to Temporal sigma-delta.glsl at b5ae6eda5 (the file the
// passes were cut from), for LF and CRLF copies, that the shader file itself keeps no search pass, and that an unknown
// include id fails the load (no silent shader without its search). The GPU proof (dither_vectors own capture hashes) is
// RendererCaptureHarnessTests; this golden is the text form of the same claim (rule 28: never edit it to pass).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/output/dither/VpShaderInclude.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// FNV-1a 64 of `git show b5ae6eda5:"shaders/Temporal sigma-delta.glsl"` (LF line ends, 46000 bytes).
		constexpr uint64_t kOriginalFnv1a64 = 0xd631910eb881f914ull;
		constexpr size_t kOriginalBytes = 46000;

		uint64_t Fnv1a64(const std::string& text)
		{
			uint64_t h = 0xcbf29ce484222325ull;
			for (const unsigned char c : text)
			{
				h ^= c;
				h *= 0x100000001b3ull;
			}
			return h;
		}

		std::string ReadRepoShader(const char* fileName)
		{
			// src/VP-V/modules/motion/estimation/tests/<this file> -> the repository root (seven levels).
			std::string path = __FILE__;
			for (int level = 0; level < 7; ++level)
			{
				const size_t separator = path.find_last_of("\\/");
				if (separator == std::string::npos)
					return {};
				path.resize(separator);
			}
			std::ifstream input(path + "\\shaders\\" + fileName, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		}

		std::string ToLf(std::string text)
		{
			std::string out;
			out.reserve(text.size());
			for (size_t i = 0; i < text.size(); ++i)
				if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n'))
					out += text[i];
			return out;
		}

		std::string ToCrlf(const std::string& lf)
		{
			std::string out;
			out.reserve(lf.size() + lf.size() / 32);
			for (const char c : lf)
			{
				if (c == '\n')
					out += '\r';
				out += c;
			}
			return out;
		}
	}

	TEST_CLASS(OutputDomainSearchTests)
	{
	public:
		TEST_METHOD(ExpandedSigmaDeltaIsTheOriginalFileByteForByte)
		{
			const std::string file = ReadRepoShader("Temporal sigma-delta.glsl");
			Assert::IsFalse(file.empty(), L"shaders/Temporal sigma-delta.glsl not found");
			for (const bool crlf : { false, true })
			{
				std::string source = crlf ? ToCrlf(ToLf(file)) : ToLf(file);
				std::string reason;
				Assert::IsTrue(CustomHookChain::ExpandVpIncludes(source, reason), L"expansion failed");
				Assert::IsTrue(reason.empty());
				if (crlf)
				{
					Assert::AreEqual(std::string::npos, ToLf(source).find('\r'), L"a stray CR: the inserted text took the wrong line ends");
					Assert::AreEqual(ToCrlf(ToLf(source)), source, L"CRLF copy: every line ends in CRLF");
				}
				const std::string lf = ToLf(source);
				Assert::AreEqual(kOriginalBytes, lf.size(), crlf ? L"CRLF copy: size" : L"LF copy: size");
				Assert::IsTrue(Fnv1a64(lf) == kOriginalFnv1a64, crlf ? L"CRLF copy: not the b5ae6eda5 text" : L"LF copy: not the b5ae6eda5 text");
			}
		}

		TEST_METHOD(ShaderFileKeepsNoSearchPassAndNamesTheModuleOnce)
		{
			const std::string file = ToLf(ReadRepoShader("Temporal sigma-delta.glsl"));
			Assert::IsFalse(file.empty());
			const std::string include = std::string("// VP_INCLUDE ") + VpMotion::OutputDomainSearch::kIncludeId + "\n";
			const size_t at = file.find(include);
			Assert::AreNotEqual(std::string::npos, at, L"the include line is missing");
			Assert::AreEqual(std::string::npos, file.find(include, at + 1), L"the search is named once");
			for (const char* pass : { "//!SAVE LUMAQ", "//!SAVE LUMAC", "//!SAVE MV", "block motion search" })
				Assert::AreEqual(std::string::npos, file.find(pass), L"a search pass is still in the sigma-delta file");
			// The module text carries all three, in hook order.
			const std::string glsl = VpMotion::OutputDomainSearch::Glsl();
			const size_t q = glsl.find("//!SAVE LUMAQ"), c = glsl.find("//!SAVE LUMAC"), m = glsl.find("//!SAVE MV");
			Assert::IsTrue(q != std::string::npos && c != std::string::npos && m != std::string::npos && q < c && c < m);
			Assert::AreEqual(std::string::npos, glsl.find('\r'), L"the module text is LF");
		}

		TEST_METHOD(UnknownIncludeFailsAndNoIncludeIsUntouched)
		{
			std::string unknown = "//!HOOK OUTPUT\n// VP_INCLUDE motion.nothing_here\nvoid hook() {}\n";
			std::string reason;
			Assert::IsFalse(CustomHookChain::ExpandVpIncludes(unknown, reason));
			Assert::AreNotEqual(std::string::npos, reason.find("motion.nothing_here"));
			const std::string plain = "//!HOOK OUTPUT\n// VP_INCLUDEX not a directive\n//  VP_INCLUDE indented\nvoid hook() {}";
			std::string copy = plain;
			reason.clear();
			Assert::IsTrue(CustomHookChain::ExpandVpIncludes(copy, reason));
			Assert::AreEqual(plain, copy);
			// Last line without a line end, and a trailing-space id.
			std::string last = "a\n// VP_INCLUDE motion.output_domain_search  ";
			Assert::IsTrue(CustomHookChain::ExpandVpIncludes(last, reason));
			Assert::AreEqual("a\n" + VpMotion::OutputDomainSearch::Glsl(), last);
		}
	};
}
