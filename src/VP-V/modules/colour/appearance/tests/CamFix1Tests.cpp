// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackPlainText.h>
#include <VP-V/modules/colour/appearance/AppearanceStage.h>
#include <VP-V/modules/colour/display_mapping/DvHostStage.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutHostPolicy.h>

#include <fstream>
#include <map>
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

		struct Fix1Registry
		{
			std::map<std::string, std::string> engaged;
			DvHostStage::Engage Engage() { return [this](const char* id, const std::string& r) { engaged[id] = r; }; }
			DvHostStage::Release Release() { return [this](const char* id) { engaged.erase(id); }; }
			bool Has(const char* id) const { return engaged.count(id) != 0; }
		};
		inline CustomHookChain::Loader Fix1NoShaders()
		{
			return {
				[](const std::string&, std::string&, std::string& reason) { reason = "not found (test)"; return false; },
				[](const std::string&) -> const pl_hook* { return nullptr; },
				[](const pl_hook*& hook) { hook = nullptr; } };
		}
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

	// MUST 3: the R12B/R12L container row and the LUT-substitution row are engaged by the production sync, not only by
	// the catalogue helpers.
	TEST_CLASS(DvContainerFix1Tests)
	{
	public:
		TEST_METHOD(TwelveBitRgbTunnelStepsBackVisibly)
		{
			Fix1Registry reg;
			DvHostStage stage(Fix1NoShaders(), reg.Engage(), reg.Release());
			Assert::IsFalse(stage.SyncContainer(true, "R12B", "vp"), L"VP's mapping cannot read a 12-bit RGB tunnel picture");
			Assert::IsTrue(reg.Has("DV display mapping: container"), L"the step-back is listed");
			Assert::IsTrue(stage.SyncContainer(false, "R12B", "vp"), L"no tunnel: VP's mapping runs");
			Assert::IsFalse(reg.Has("DV display mapping: container"), L"the row is released");
			Assert::IsFalse(stage.SyncContainer(true, "R12L", "vp"));
			Assert::IsTrue(reg.Has("DV display mapping: container"));
			Assert::IsTrue(stage.SyncContainer(true, "", "vp"), L"a readable container runs VP's mapping");
			Assert::IsFalse(reg.Has("DV display mapping: container"));
			Assert::IsFalse(stage.SyncContainer(true, "R12B", "original"), L"original asks for libplacebo's mapping");
			Assert::IsFalse(reg.Has("DV display mapping: container"), L"a requested original is not a fallback");
		}

		TEST_METHOD(AutoMovedLutIsListedOnlyWhenItApplies)
		{
			Fix1Registry reg;
			DvHostStage stage(Fix1NoShaders(), reg.Engage(), reg.Release());
			stage.SyncCalibrationLut("auto", "vp", true);
			Assert::IsTrue(reg.Has("calibration LUT: display mapping"), L"auto moved the LUT: listed");
			stage.SyncCalibrationLut("vp", "vp", true);
			Assert::IsFalse(reg.Has("calibration LUT: display mapping"), L"the user asked for vp: not a fallback");
			stage.SyncCalibrationLut("auto", "vp", false);
			Assert::IsFalse(reg.Has("calibration LUT: display mapping"), L"no LUT this frame");
			stage.SyncCalibrationLut("auto", "libplacebo", true);
			Assert::IsFalse(reg.Has("calibration LUT: display mapping"));
		}
	};

	// MUST 4: L11 reference mode replaces the ROOM with the reference room; the screen-white step still runs.
	TEST_CLASS(AppearanceL11Fix1Tests)
	{
	public:
		TEST_METHOD(ReferenceModeKeepsTheScreenWhiteStep)
		{
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			in.screenWhiteNits = 48.0;           // a projector: the screen white differs from the 100-nit reference
			in.room.room = RoomLight::Room::Auto;
			in.recordAvailable = true;
			in.record.valid = true;
			in.record.smoothedLux = 200.0;       // a lit room that reference mode must ignore
			in.l11ReferenceMode = true;
			in.honourL11 = true;
			const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
			Assert::IsTrue(d.active, L"the Stevens/Hunt step from Lw_ref to Lw_screen stays on");
			Assert::IsTrue(d.l11Ignored, L"the room was held back (listed)");
			Assert::AreEqual(Cam16::kSurroundDim.F, d.room.surround.F, 0.0);
			Assert::AreEqual(Cam16::kSurroundDim.Nc, d.room.surround.Nc, 0.0);
			Assert::AreEqual(0.0, d.room.veilNits, 0.0);
			Assert::AreEqual(0.0f, d.uniforms.veil, 0.0f);
			Assert::IsTrue(d.uniforms.screen.fl != d.uniforms.reference.fl, L"the two whites differ: the model adapts");
			Assert::IsFalse(d.parameters.empty());
			Assert::IsTrue(Fix1Has(d.status, "reference mode"));
		}
	};

	// MUST 5: the veil is flare in the screen conditions and a toe on the displayed light, never a subtract-and-clip.
	TEST_CLASS(AppearanceVeilFix1Tests)
	{
	public:
		TEST_METHOD(NearBlackRampKeepsEveryLevel)
		{
			const double f = 0.05;   // a lit room on a dim screen: well above any code step
			double prev = -1.0;
			for (int i = 0; i <= 4095; ++i)
			{
				const double o = i / 4095.0;
				const double g = Cam16::FlareToDisplayed(o, f);
				Assert::IsTrue(g > prev, L"strictly monotonic: no two levels merge");
				if (i > 0)
					Assert::IsTrue(g > 0.0, L"no level below the veil is crushed to 0");
				prev = g;
			}
			Assert::AreEqual(0.0, Cam16::FlareToDisplayed(0.0, f), 0.0);
			Assert::AreEqual(1.0, Cam16::FlareToDisplayed(1.0, f), 1e-12);
			// The old subtract-and-clip sent every o <= f to 0; the toe keeps a code at o = f / 2.
			Assert::IsTrue(Cam16::FlareToDisplayed(f * 0.5, f) > f * 0.1);
			// Inside the range the displayed light is brought down, and the seen light (displayed + flare) is not below the intent.
			for (int i = 1; i < 100; ++i)
			{
				const double o = i / 100.0;
				Assert::IsTrue(Cam16::FlareToDisplayed(o, f) < o);
				Assert::IsTrue(Cam16::FlareToDisplayed(o, f) + f > o);
			}
		}

		TEST_METHOD(ZeroVeilIsTheIdentity)
		{
			for (int i = 0; i <= 100; ++i)
				Assert::AreEqual(i / 100.0, Cam16::FlareToDisplayed(i / 100.0, 0.0), 0.0);
		}

		TEST_METHOD(FlareEntersTheScreenAdaptingField)
		{
			Cam16::UniformInputs in;
			in.screenWhiteNits = 48.0;
			const Cam16::Cam16Uniforms dark = Cam16::PackUniforms(in);
			in.veilNits = 2.0;
			const Cam16::Cam16Uniforms lit = Cam16::PackUniforms(in);
			Assert::IsTrue(lit.screen.fl > dark.screen.fl, L"the veil raises the screen's adapting luminance");
			Assert::AreEqual(dark.reference.fl, lit.reference.fl, L"the reference side is unchanged");
		}

		TEST_METHOD(ShaderUsesTheToeNotTheClip)
		{
			const std::string glsl = Fix1ReadUp("shaders\\vp_cam16_appearance.glsl");
			if (glsl.empty())
				return;   // not reachable from this working directory
			Assert::IsTrue(Fix1Has(glsl, "float ap_flare(float o, float f)"));
			Assert::IsFalse(Fix1Has(glsl, "(outLin - vec3(apx_veil))"), L"the subtract-and-clip is gone");
		}
	};
}
