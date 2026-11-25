// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CC W4 T1 (GEM-031 wiring, config-schema MUST-1, owner Q4): VpvRendererSettings::PostLoad itself (compiled into this
// test project) returns the host's chroma_upscaler override: on the Original pathway a saved one of Bill's five filters
// stays (nullptr = keep the host's), an unset / VP-only value becomes bicubic; on the VP-V pathway nothing is overridden.
#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <VP-V/modules/control/config/VpvRendererSettings.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
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

		// S4-L1: PostLoad matches pipeline_slots_off case-insensitively and by exact slot token (no substring match).
		TEST_METHOD(PipelineSlotsOffIsCaseInsensitiveAndExactTokenMatched)
		{
			// Mixed-case disable: Colour.Appearance disables the appearance slot so ambient_adapt is NOT replaced.
			{
				VpvRenderer::Fields fields;
				fields.pipelineOriginalPathway = false;
				fields.appearance = true;
				fields.appearanceStrength = 1.0;
				fields.ambientAdapt = true;
				fields.pipelineSlotsOff = "Colour.Appearance";

				VpvRendererSettings::PostLoad(fields, false, nullptr);

				Assert::IsFalse(fields.ambientAdaptReplaced,
					L"S4-L1: mixed-case 'Colour.Appearance' in pipeline_slots_off must disable appearance and not replace ambient_adapt");
				Assert::IsTrue(fields.ambientAdapt,
					L"S4-L1: mixed-case 'Colour.Appearance' in pipeline_slots_off must keep ambient_adapt enabled");
			}

			// Negative control (substring trap): 'colour.appearance_v2' is NOT 'colour.appearance',
			// so appearance still runs and replaces ambient_adapt.
			{
				VpvRenderer::Fields fields;
				fields.pipelineOriginalPathway = false;
				fields.appearance = true;
				fields.appearanceStrength = 1.0;
				fields.ambientAdapt = true;
				fields.pipelineSlotsOff = "colour.appearance_v2";

				VpvRendererSettings::PostLoad(fields, false, nullptr);

				Assert::IsTrue(fields.ambientAdaptReplaced,
					L"S4-L1 negative control: 'colour.appearance_v2' must NOT match 'colour.appearance' (ambientAdaptReplaced must be true)");
				Assert::IsFalse(fields.ambientAdapt,
					L"S4-L1 negative control: 'colour.appearance_v2' must NOT disable appearance (ambientAdapt must be forced false)");
			}
		}

		// S4-L1: ReadGlobal normalizes pipeline_slots_off and pipeline_order to lowercase.
		TEST_METHOD(ReadGlobalNormalizesPipelineSlotListsToLowercase)
		{
			static std::atomic<unsigned> seq{ 0 };
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			char tempDir[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(MAX_PATH, tempDir) != 0, L"S4-L1: GetTempPathA failed");
			const std::string tempPath = std::string(tempDir) +
				"vpv_s4_l1_readglobal_" + std::to_string(stamp) + "_" + std::to_string(seq.fetch_add(1)) + ".cfg";
			struct RemoveOnExit
			{
				std::string p;
				~RemoveOnExit()
				{
					DeleteFileA(p.c_str());
				}
			} cleanup{ tempPath };

			{
				std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
				Assert::IsTrue(out.is_open(), L"S4-L1: failed to write temporary config fixture");
				out << "[vpv]\n"
					<< "pipeline_slots_off: Colour.Appearance\n"
					<< "pipeline_order: Repair.Encoding\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(tempPath, ConfigFile::ReadPolicy::Fresh),
				L"S4-L1: failed to load temporary config fixture");

			VpvRenderer::Fields fields;
			VpvRendererSettings::ReadGlobal(config, fields);

			Assert::AreEqual(std::string("colour.appearance"), fields.pipelineSlotsOff,
				L"S4-L1: ReadGlobal must normalize pipeline_slots_off to lowercase");
			Assert::AreEqual(std::string("repair.encoding"), fields.pipelineOrder,
				L"S4-L1: ReadGlobal must normalize pipeline_order to lowercase");
		}
	};
}
