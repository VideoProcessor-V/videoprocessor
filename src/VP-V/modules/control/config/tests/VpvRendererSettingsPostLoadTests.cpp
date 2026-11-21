// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CC W4 T1 (GEM-031 wiring, config-schema MUST-1, owner Q4): VpvRendererSettings::PostLoad itself (compiled into this
// test project) returns the host's chroma_upscaler override: on the Original pathway a saved one of Bill's five filters
// stays (nullptr = keep the host's), an unset / VP-only value becomes bicubic; on the VP-V pathway nothing is overridden.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/control/config/VpvRendererSettings.h>

#include <cstring>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		const char* OverrideFor(bool originalPathway, const std::string* host)
		{
			VpvRenderer::Fields fields;
			fields.pipelineOriginalPathway = originalPathway;
			return VpvRendererSettings::PostLoad(fields, false, host).chromaUpscaler;
		}
	}

	TEST_CLASS(VpvRendererSettingsPostLoadTests)
	{
	public:
		TEST_METHOD(OriginalKeepsBillsChromaFilterAndMapsTheRestToBicubic)
		{
			for (const char* bills : { "ewa_lanczossharp", "ewa_lanczos", "lanczos", "bicubic", "bilinear" })
			{
				const std::string host(bills);
				Assert::IsNull(OverrideFor(true, &host), (std::wstring(L"Original replaced Bill's filter ") + std::wstring(host.begin(), host.end())).c_str());
			}
			for (const char* vpOnly : { "auto", "catmull_rom", "" })
			{
				const std::string host(vpOnly);
				const char* chosen = OverrideFor(true, &host);
				Assert::IsNotNull(chosen, L"Original kept a VP-only / unset chroma_upscaler");
				Assert::AreEqual(0, std::strcmp(chosen, "bicubic"), L"Original's replacement is not bicubic");
			}
			const char* unknown = OverrideFor(true, nullptr);
			Assert::IsNotNull(unknown, L"Original without a host value kept it");
			Assert::AreEqual(0, std::strcmp(unknown, "bicubic"), L"Original without a host value is not bicubic");
		}

		TEST_METHOD(VpPathwayOverridesNothing)
		{
			for (const char* value : { "ewa_lanczos", "auto", "catmull_rom" })
			{
				const std::string host(value);
				Assert::IsNull(OverrideFor(false, &host), L"the VP-V pathway overrode the host's chroma_upscaler");
			}
			Assert::IsNull(OverrideFor(false, nullptr), L"the VP-V pathway overrode an unknown chroma_upscaler");
		}
	};
}
