// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// VP-V tests split out of Bill's LibplaceboRenderParametersTests.cpp by tools/m2/move_tests.py.
#include "pch.h"
#include "CppUnitTest.h"

#include "ConfigFile.h"
#include "RendererProfileConfig.h"
#include "QualityProfileMigration.h"
#include <vprenderer/LibplaceboRenderParameters.h>

#include <Windows.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LibplaceboRenderParameters;

namespace
{
	std::wstring Wide(const std::string& text)
	{
		return std::wstring(text.begin(), text.end());
	}

	template<typename T>
	const T* NativeData(const char* exportName)
	{
		HMODULE module = GetModuleHandleW(L"libplacebo-360.dll");
		if (!module)
			module = LoadLibraryW(L"libplacebo-360.dll");
		Assert::IsTrue(module != nullptr,
			L"The test project must load the libplacebo runtime.");
		const T* data = reinterpret_cast<const T*>(
			GetProcAddress(module, exportName));
		Assert::IsTrue(data != nullptr,
			Wide(std::string("Missing libplacebo data export: ") + exportName).c_str());
		return data;
	}

	void BuildOrFail(const Settings& settings, bool hasDisplayLut,
		Projection& projection)
	{
		std::string error;
		Assert::IsTrue(Build(settings, hasDisplayLut, projection, error),
			Wide(error).c_str());
	}

	void AssertSameData(const void* expected, const void* actual, size_t size,
		const wchar_t* message)
	{
		Assert::AreEqual(0, std::memcmp(expected, actual, size), message);
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(LibplaceboRenderParametersTestsVpv)
	{
	public:

        // FL35 merge check (RC3): the owner runs static HDR->SDR with LLDV doing
        // the dynamic mapping. With peak_detection off and every tuning key on
        // Auto, nothing is dynamic and the static curve's constants are exactly
        // libplacebo's defaults: RC3's tuning changes nothing until a key is set.
        TEST_METHOD(PeakDetectionOffWithTuningAtAutoKeepsTheStaticCurve)
        {
            Settings settings;
            settings.peakDetection = PeakDetection::Off;
            Projection projection;
            BuildOrFail(settings, false, projection);
            Assert::IsTrue(projection.renderParams.peak_detect_params == nullptr);
            const auto* defaults = NativeData<pl_color_map_params>("pl_color_map_default_params");
            AssertSameData(&defaults->tone_constants, &projection.colorMapParams.tone_constants,
                sizeof(defaults->tone_constants), L"Auto tuning must keep libplacebo's tone constants");
        }

		TEST_METHOD(SaturationOffsetProjectsToColorAdjustmentOnlyWhenNonZero)
		{
			Settings settings;
			Projection neutral;
			BuildOrFail(settings, false, neutral);
			Assert::IsTrue(neutral.renderParams.color_adjustment == nullptr,
				L"Saturation 0 must leave libplacebo's colour adjustment untouched");

			settings.saturation = 5;
			Projection boosted;
			BuildOrFail(settings, false, boosted);
			Assert::IsTrue(boosted.renderParams.color_adjustment == &boosted.colorAdjustment);
			Assert::AreEqual(1.05f, boosted.colorAdjustment.saturation, 1e-6f);
			Assert::AreEqual(pl_color_adjustment_neutral.brightness, boosted.colorAdjustment.brightness);
			Assert::AreEqual(pl_color_adjustment_neutral.contrast, boosted.colorAdjustment.contrast);
			Assert::AreEqual(pl_color_adjustment_neutral.hue, boosted.colorAdjustment.hue);
			Assert::AreEqual(pl_color_adjustment_neutral.gamma, boosted.colorAdjustment.gamma);

			settings.saturation = -100;
			Projection grey;
			BuildOrFail(settings, false, grey);
			Assert::AreEqual(0.0f, grey.colorAdjustment.saturation, 1e-6f);
		}
	};
}
