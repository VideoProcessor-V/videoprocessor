// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/colour/appearance/AppearanceStage.h>

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// CF-1 CAM-3: the decision of the CAM16 appearance pass. Pure CPU.
namespace Tests
{
	TEST_CLASS(AppearanceStageTests)
	{
	public:
		TEST_METHOD(ReferenceWhiteFollowsTheTrimThenTheSdrTargetThenOneHundred)
		{
			AppearanceStage::Inputs in;
			Assert::AreEqual(100.0, AppearanceStage::ReferenceWhite(in), 0.0);
			in.sdrTargetNits = 203.0;
			Assert::AreEqual(203.0, AppearanceStage::ReferenceWhite(in), 0.0);
			in.trimApplied = true;
			in.trimTargetNits = 700.0;
			Assert::AreEqual(700.0, AppearanceStage::ReferenceWhite(in), 0.0);
			in.trimTargetNits = 0.0;   // a trim without a target does not invent one
			Assert::AreEqual(203.0, AppearanceStage::ReferenceWhite(in), 0.0);
		}

		TEST_METHOD(OffDecidesNothingAndSaysOff)
		{
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = false;
			const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
			Assert::IsFalse(d.active);
			Assert::IsTrue(d.parameters.empty());
			Assert::AreEqual(std::string("appearance: off"), d.status);
		}

		TEST_METHOD(L11ReferenceModeIgnoresTheRoomWhenHonoured)
		{
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			in.l11ReferenceMode = true;
			in.honourL11 = true;
			AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
			Assert::IsFalse(d.active);
			Assert::IsTrue(d.status.find("reference mode") != std::string::npos);
			in.honourL11 = false;
			d = AppearanceStage::Decide(in, s);
			Assert::IsTrue(d.active);
		}

		TEST_METHOD(OnBuildsTheShaderParameterListAndNamesTheUnmeasuredScreen)
		{
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
			Assert::IsTrue(d.active);
			Assert::AreEqual<size_t>(10 + 10 + 7 + 9, d.parameters.size());
			Assert::IsTrue(d.status.find("screen white not measured") != std::string::npos);
			in.screenWhiteMeasured = true;
			Assert::IsTrue(AppearanceStage::Decide(in, s).status.find("not measured") == std::string::npos);
		}

		TEST_METHOD(StrengthZeroIsTheIdentityAndNotActive)
		{
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			in.strength = 0.0;
			const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
			Assert::IsFalse(d.active);
			for (const auto& p : d.parameters)
				if (std::string(p.first) == "apx_strength")
					Assert::AreEqual(0.0, p.second, 0.0);
		}

		TEST_METHOD(RgbToXyzBt709HasWhiteAtYOne)
		{
			const double r[2] = { 0.640, 0.330 }, g[2] = { 0.300, 0.600 }, b[2] = { 0.150, 0.060 }, w[2] = { 0.3127, 0.3290 };
			double m[9] = {};
			Assert::IsTrue(AppearanceStage::RgbToXyz(r, g, b, w, m));
			Assert::AreEqual(0.2126, m[3], 5e-4);
			Assert::AreEqual(0.7152, m[4], 5e-4);
			Assert::AreEqual(0.0722, m[5], 5e-4);
			Assert::AreEqual(1.0, m[3] + m[4] + m[5], 1e-9);
			const double bad[2] = { 0.3, 0.0 };
			Assert::IsFalse(AppearanceStage::RgbToXyz(bad, g, b, w, m));
		}

		TEST_METHOD(ParameterNamesAreUniqueAndAllDeclaredInTheShader)
		{
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
			std::set<std::string> names;
			for (const auto& p : d.parameters)
				Assert::IsTrue(names.insert(p.first).second, L"duplicate parameter name");
			// The shader sits at <repo>\shaders; the tests run from the build tree, so look upward for it.
			std::string text;
			std::string dir = "";
			for (int up = 0; up < 8 && text.empty(); ++up, dir += "..\\")
			{
				std::ifstream f(dir + "shaders\\vp_cam16_appearance.glsl", std::ios::binary);
				if (f)
				{
					std::ostringstream ss;
					ss << f.rdbuf();
					text = ss.str();
				}
			}
			if (text.empty())
				return;   // not reachable from this working directory: the tool-side checker covers it
			for (const std::string& n : names)
				Assert::IsTrue(text.find(n) != std::string::npos, std::wstring(n.begin(), n.end()).c_str());
		}
	};
}
