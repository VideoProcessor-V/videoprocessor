// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/colour/appearance/AppearanceStage.h>
#include "../../../../tests/SettingsTestSupport.h"

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
			in.trimTargetNits = 150.0;   // below BT.2408 diffuse white (203 nits): trim target takes precedence over sdrTargetNits
			Assert::AreEqual(150.0, AppearanceStage::ReferenceWhite(in), 0.0);
			// POD2-H2 (432818710): ReferenceWhite caps peak trimTargetNits to ITU-R BT.2408 diffuse white (203 nits),
			// never conflating peak display luminance (700 nits) with paper white or falling back to sdrTargetNits (120 nits).
			in.sdrTargetNits = 120.0;
			in.trimTargetNits = 700.0;
			Assert::AreEqual(203.0, AppearanceStage::ReferenceWhite(in), 0.0);
			in.sdrTargetNits = 203.0;
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
			// FIX1 MUST 4: the room is replaced by the reference room (no veil); the screen-white step still runs.
			Assert::IsTrue(d.active);
			Assert::IsTrue(d.l11Ignored);
			Assert::AreEqual(0.0, d.room.veilNits, 0.0);
			Assert::IsTrue(d.status.find("reference mode") != std::string::npos);
			in.honourL11 = false;
			d = AppearanceStage::Decide(in, s);
			Assert::IsTrue(d.active);
			Assert::IsFalse(d.l11Ignored);   // CF1-FIN SHOULD D: not honoured = the room is used
		}

		TEST_METHOD(OnBuildsTheShaderParameterListAndNamesTheUnmeasuredScreen)
		{
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
			Assert::IsTrue(d.active);
			Assert::AreEqual<size_t>(10 + 10 + 9 + 9 + 9 + 1, d.parameters.size());   // + apx_trc_kind, apx_trc_param + apn_* (inverse) + apx_hunt_scale
			Assert::IsTrue(d.status.find("screen white not measured") != std::string::npos);
			in.screenWhiteMeasured = true;
			Assert::IsTrue(AppearanceStage::Decide(in, s).status.find("not measured") == std::string::npos);
		}

		// S2-H2: unsupported output transfers (HLG, linear, log, unknown) must disable appearance
		// visibly instead of applying power-law gamma to non-gamma codes, while supported piecewise
		// transfers (BT.1886, sRGB) emit apx_trc_kind / apx_trc_param accurately.
		TEST_METHOD(UnsupportedOutputTransferDisablesAppearanceVisibly)
		{
			auto findParam = [](const AppearanceStage::Decision& dec, const char* name) -> double
			{
				for (const auto& p : dec.parameters)
					if (std::string(p.first) == name)
						return p.second;
				Assert::Fail(L"expected shader parameter missing");
				return 0.0;
			};

			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			in.room.room = RoomLight::Room::Auto;
			in.recordAvailable = true;
			in.record.valid = true;
			in.record.smoothedLux = 15.0;
			in.now = 1.0;

			// Seed the smoother on a supported transfer first so we prove the unsupported gate resets it.
			in.transferSupported = true;
			const AppearanceStage::Decision seeded = AppearanceStage::Decide(in, s);
			Assert::IsTrue(seeded.active);
			Assert::IsFalse(seeded.transferUnsupported);
			Assert::IsTrue(s.Initialised(), L"smoother must be initialised after an active auto-room decision");

			// Unsupported output transfer on the exact same inputs: must disable visibly, emit no parameters, reset smoother.
			in.transferSupported = false;
			const AppearanceStage::Decision unsupported = AppearanceStage::Decide(in, s);
			Assert::IsFalse(unsupported.active, L"unsupported output transfer must disable CAM16 appearance");
			Assert::IsTrue(unsupported.transferUnsupported, L"decision must flag transferUnsupported");
			Assert::IsFalse(unsupported.lutConflict, L"transfer gate must not be mislabelled as lutConflict");
			Assert::IsTrue(unsupported.parameters.empty(), L"inactive decision must not emit shader parameters");
			Assert::IsFalse(s.Initialised(), L"unsupported output transfer must reset the room-light smoother");
			Assert::IsTrue(unsupported.status.find("off for this output transfer") != std::string::npos,
				L"status line must visibly report the output transfer gate");

			// Off by user choice with an unsupported transfer stays plain off (not an unsupported-transfer fallback).
			AppearanceStage::Inputs offIn = in;
			offIn.on = false;
			const AppearanceStage::Decision userOff = AppearanceStage::Decide(offIn, s);
			Assert::IsFalse(userOff.active);
			Assert::IsFalse(userOff.transferUnsupported);
			Assert::AreEqual(std::string("appearance: off"), userOff.status);

			// Negative control (exact same inputs with transferSupported = true): appearance activates
			// and publishes apx_trc_kind / apx_trc_param for BT.1886, sRGB, and pure-power outputs.
			in.transferSupported = true;
			in.transferKind = 1;
			in.transferParam = 0.05;
			const AppearanceStage::Decision bt1886 = AppearanceStage::Decide(in, s);
			Assert::IsTrue(bt1886.active, L"negative control: supported BT.1886 transfer must activate appearance");
			Assert::IsFalse(bt1886.transferUnsupported);
			Assert::AreEqual<size_t>(10 + 10 + 9 + 9 + 9 + 1, bt1886.parameters.size());
			Assert::AreEqual(1.0, findParam(bt1886, "apx_trc_kind"), 0.0, L"BT.1886 kind must be emitted as 1");
			Assert::AreEqual(0.05, findParam(bt1886, "apx_trc_param"), 1e-12, L"BT.1886 black offset b must be emitted");

			in.transferKind = 2;
			in.transferParam = 0.0;
			const AppearanceStage::Decision srgb = AppearanceStage::Decide(in, s);
			Assert::IsTrue(srgb.active, L"negative control: supported sRGB transfer must activate appearance");
			Assert::IsFalse(srgb.transferUnsupported);
			Assert::AreEqual(2.0, findParam(srgb, "apx_trc_kind"), 0.0, L"sRGB kind must be emitted as 2");
			Assert::AreEqual(0.0, findParam(srgb, "apx_trc_param"), 0.0, L"sRGB param must be emitted as 0");

			in.transferKind = 0;
			in.transferParam = 0.0;
			const AppearanceStage::Decision power = AppearanceStage::Decide(in, s);
			Assert::IsTrue(power.active, L"negative control: supported pure-power gamma transfer must activate appearance");
			Assert::IsFalse(power.transferUnsupported);
			Assert::AreEqual(0.0, findParam(power, "apx_trc_kind"), 0.0, L"pure-power kind must be emitted as 0");
			Assert::AreEqual(0.0, findParam(power, "apx_trc_param"), 0.0);
		}

		TEST_METHOD(StrengthZeroIsTheIdentityAndNotActive)
		{
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			in.strength = 0.0;
			const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
			Assert::IsFalse(d.active);
			bool strengthRow = false;   // CF1-FIN SHOULD D: the row must exist (an empty list passed before)
			for (const auto& p : d.parameters)
				if (std::string(p.first) == "apx_strength")
				{
					strengthRow = true;
					Assert::AreEqual(0.0, p.second, 0.0);
				}
			Assert::IsTrue(strengthRow, L"apx_strength is in the parameter list");
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
			// CF1-FIN MUST 4: the shader at <repo>\shaders, found from this file's own path; unreadable FAILS.
			const std::string text = SettingsTestSupport::ReadFile(SettingsTestSupport::RepoRoot() + "shaders\\vp_cam16_appearance.glsl");
			Assert::IsFalse(text.empty(), L"shaders\\vp_cam16_appearance.glsl is readable from the repository root");
			for (const std::string& n : names)
				Assert::IsTrue(text.find(n) != std::string::npos, std::wstring(n.begin(), n.end()).c_str());
		}
	};
}
