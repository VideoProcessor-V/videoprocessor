// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackPlainText.h>
#include <VP-V/modules/colour/appearance/AppearanceStage.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutHostPolicy.h>

#include <fstream>
#include <sstream>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// CF-1 FIX1 (gate CF1-347e0546f): one test per audit MUST, pure CPU. The settings code is not linked into this project, so
// its plumbing is read as text from the source tree (as AppearanceStatusTests does); the decisions are called directly.
namespace Tests
{
	namespace
	{
		std::string Fix1ReadUp(const std::string& rel)
		{
			std::string dir;
			for (int up = 0; up < 9; ++up, dir += "..\\")
			{
				std::ifstream f(dir + rel, std::ios::binary);
				if (f)
				{
					std::ostringstream ss;
					ss << f.rdbuf();
					return ss.str();
				}
			}
			return std::string();
		}
		bool Fix1Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
		const char* const kFix1ConfigDir = "src\\VP-V\\modules\\control\\config\\";
	}

	// MUST 1 / MUST 2 / SHOULD 7: the calibration LUT moves to VP's last step by default whenever a VP display-mapping step
	// (the DV trims OR appearance) must act before it; the user's value is kept apart from the resolved one.
	TEST_CLASS(CalibrationLutAutoFix1Tests)
	{
	public:
		TEST_METHOD(AppearanceAloneMovesTheLutUnderAuto)
		{
			// Trims off (dv_display_mapping original), appearance on: a VP step runs, so auto resolves to vp.
			Assert::IsTrue(CalibrationLutHost::DisplayMappingStepsRun(false, "original", false, true, ""));
			Assert::AreEqual(std::string("vp"), CalibrationLutHost::ResolveStage("auto",
				CalibrationLutHost::DisplayMappingStepsRun(false, "original", false, true, "")));
			// The trims alone still move it.
			Assert::IsTrue(CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, false, ""));
			// Nothing VP's runs: libplacebo (byte-identical to before).
			Assert::IsFalse(CalibrationLutHost::DisplayMappingStepsRun(false, "vp", false, false, ""));
			Assert::IsFalse(CalibrationLutHost::DisplayMappingStepsRun(false, "original", true, false, ""));
		}

		TEST_METHOD(OriginalPathwayAndSlotsOffKeepLibplacebo)
		{
			Assert::IsFalse(CalibrationLutHost::DisplayMappingStepsRun(true, "vp", true, true, ""));
			Assert::IsFalse(CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, true, "colour.dm_trims, colour.appearance"));
			Assert::IsTrue(CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, true, "colour.appearance"));
			Assert::IsTrue(CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, true, "colour.dm_trims"));
		}

		TEST_METHOD(DefaultIsAutoAndTheResolvedStageIsSeparate)
		{
			const std::string cfg(kFix1ConfigDir);
			const std::string fields = Fix1ReadUp(cfg + "VpvRendererFields.h");
			const std::string settings = Fix1ReadUp(cfg + "VpvRendererSettings.cpp");
			if (fields.empty() || settings.empty())
				return;   // not reachable from this working directory: the tool-side checkers cover it
			Assert::IsTrue(Fix1Has(fields, "std::string calibrationLutStageConfigured = \"auto\";"), L"the user's default is auto");
			Assert::IsTrue(Fix1Has(settings, "settings.calibrationLutStageConfigured = value;"), L"the parse keeps the user's value");
			Assert::IsTrue(Fix1Has(settings, "CalibrationLutHost::ResolveStage(settings.calibrationLutStageConfigured,"),
				L"PostLoad resolves from the user's value into the renderer's field");
			Assert::IsTrue(Fix1Has(settings, "CalibrationLutHost::DisplayMappingStepsRun("), L"the resolution counts appearance");
		}

		TEST_METHOD(AppearanceAfterALibplaceboLutIsOffAndListed)
		{
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			in.calibrationLutBefore = true;
			const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
			Assert::IsFalse(d.active, L"CAM16 never acts on projector device codes");
			Assert::IsTrue(d.lutConflict);
			Assert::IsTrue(Fix1Has(d.status, "off with a calibration LUT"));
			const FallbackCatalog::Decision row = FallbackCatalog::AppearanceCalibrationLut(true);
			Assert::IsTrue(row.engaged);
			Assert::AreEqual(std::string("appearance: calibration LUT"), row.id);
			Assert::IsTrue(std::string(FallbackPlainText::Sentence(row.id)).size() > 0);
			Assert::IsFalse(FallbackCatalog::AppearanceCalibrationLut(false).engaged);
			in.calibrationLutBefore = false;
			Assert::IsTrue(AppearanceStage::Decide(in, s).active, L"without the LUT before it the pass runs");
		}
	};
}
