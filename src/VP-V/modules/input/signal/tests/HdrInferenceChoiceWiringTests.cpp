/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// F-8 wiring (2026-10-25; part of docs/design/hdr-inference-choice-callsites.patch): [vpv]
// hdr_inference through the configuration validator and the renderer's settings reader - its
// default, both values, an invalid value (kept, reported), the Original pathway forcing original,
// the settings dump line and the live-apply fingerprint. CPU only (the renderer DLL's test exports).
#include "pch.h"
#include "CppUnitTest.h"

#include <RendererProfileConfig.h>

#include <Windows.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace HdrChoiceWiring
	{
		inline std::string Here()
		{
			char here[MAX_PATH] = {};
			HMODULE self = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&Here), &self);
			GetModuleFileNameA(self, here, MAX_PATH);
			const std::string binary(here);
			return binary.substr(0, binary.find_last_of("\\/") + 1);
		}
	}

	TEST_CLASS(HdrInferenceChoiceWiringTests)
	{
	public:
		TEST_METHOD(ValidatorAcceptsTheChoiceInVpv)
		{
			Assert::IsTrue(RendererProfileConfig::ValidateBaseSetting("hdr_inference", "vp"));
			Assert::IsTrue(RendererProfileConfig::ValidateBaseSetting("hdr_inference", "original"));
			Assert::IsFalse(RendererProfileConfig::ValidateBaseSetting("hdr_inference", "bill"));
		}

		TEST_METHOD(RendererReadsDumpsAndFingerprintsHdrInference)
		{
			using namespace HdrChoiceWiring;
			const std::string folder = Here();
			HMODULE dll = LoadLibraryExA((folder + "vprenderer\\VideoProcessorVPRenderer.dll").c_str(), nullptr,
				LOAD_WITH_ALTERED_SEARCH_PATH);
			Assert::IsNotNull(dll, L"the renderer DLL next to the tests");
			using Export = int(__cdecl*)(const char*, char*, int);
			const auto describe = reinterpret_cast<Export>(GetProcAddress(dll, "VideoProcessorLibplaceboDescribeSettingsForTest"));
			const auto fingerprintOf = reinterpret_cast<Export>(GetProcAddress(dll, "VideoProcessorLibplaceboSettingsFingerprintForTest"));
			Assert::IsNotNull(reinterpret_cast<void*>(describe));
			Assert::IsNotNull(reinterpret_cast<void*>(fingerprintOf));
			int callIndex = 0;
			std::vector<std::string> written;
			auto call = [&](Export function, const std::string& keys) {
				const std::string path = folder + "vp-hdr-inference-wiring-" + std::to_string(callIndex++) + ".cfg";
				written.push_back(path);
				{
					std::ofstream config(path, std::ios::trunc | std::ios::binary);
					config << "[general]\npersist_profile_selection: false\n[vprenderer.Default]\nquality: high\n" << keys;
				}
				std::vector<char> buffer(1 << 20);
				const int length = function(path.c_str(), buffer.data(), static_cast<int>(buffer.size()));
				Assert::IsTrue(length > 0, L"the export ran");
				return std::string(buffer.data());
			};
			auto line = [](const std::string& dump) {
				const std::string key = "\nhdr_inference=";
				const size_t at = dump.find(key);
				if (at == std::string::npos) return std::string("(missing)");
				const size_t start = at + key.size();
				return dump.substr(start, dump.find('\n', start) - start);
			};
			Assert::AreEqual(std::string("vp"), line(call(describe, "")), L"default vp");
			Assert::AreEqual(std::string("original"), line(call(describe, "[vpv]\nhdr_inference: original\n")));
			Assert::AreEqual(std::string("vp"), line(call(describe, "[vpv]\nhdr_inference: vp\n")));
			Assert::AreEqual(std::string("vp"), line(call(describe, "[vpv]\nhdr_inference: maybe\n")), L"invalid: kept");
			Assert::AreEqual(std::string("original"), line(call(describe, "[vpv]\nhdr_inference: vp\npipeline_original_pathway: on\n")),
				L"the Original pathway selects Bill's heuristic");
			Assert::AreNotEqual(call(fingerprintOf, ""), call(fingerprintOf, "[vpv]\nhdr_inference: original\n"),
				L"the choice changes the fingerprint");
			for (const std::string& path : written)
				DeleteFileA(path.c_str());
		}
	};
}
