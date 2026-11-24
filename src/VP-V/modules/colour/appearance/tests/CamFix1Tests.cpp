// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackPlainText.h>
#include <VP-V/core/ProductCatalog.h>
#include <VP-V/core/StatusText.h>
#include <VP-V/modules/colour/appearance/AppearanceStage.h>
#include <VP-V/modules/colour/display_mapping/DvHostStage.h>
#include <VP-V/modules/colour/extended_range/ExtendedRange.h>
#include <VP-V/modules/drive/led_compensation/LedDriveConfig.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutHostPolicy.h>
#include "../../../../tests/SettingsTestSupport.h"

#include <cmath>
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
		// CF1-FIN MUST 4: the source tree from this file's own path (SettingsTestSupport::RepoRoot), never the working
		// directory; an unreadable file FAILS the test (it used to return early and pass).
		std::string Fix1ReadUp(const std::string& rel)
		{
			const std::string text = SettingsTestSupport::ReadFile(SettingsTestSupport::RepoRoot() + rel);
			Assert::IsFalse(text.empty(), std::wstring(rel.begin(), rel.end()).append(L" is readable from the repository root").c_str());
			return text;
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

		// C++14 overload probe so the PQ flare test compiles and genuinely FAILS at runtime against
		// pre-fix 2-arg Cam16::FlareToDisplayed(o, f), and invokes the real 3-arg
		// Cam16::FlareToDisplayed(o, f, upper) unambiguously once added.
		namespace Flare3ArgFallback
		{
			struct FallbackResult { double value; };
			inline FallbackResult FlareToDisplayed(double o, double f, ...)
			{
				return { Cam16::FlareToDisplayed(o, f) };
			}
		}

		namespace Flare3ArgProbe
		{
			inline double Unwrap(double value, bool& usedThreeArg)
			{
				usedThreeArg = true;
				return value;
			}
			inline double Unwrap(Flare3ArgFallback::FallbackResult r, bool& usedThreeArg)
			{
				usedThreeArg = false;
				return r.value;
			}
			inline double CallCam16Flare(double o, double f, double upper, bool& usedThreeArg)
			{
				using Cam16::FlareToDisplayed;
				using Flare3ArgFallback::FlareToDisplayed;
				return Unwrap(FlareToDisplayed(o, f, upper), usedThreeArg);
			}
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

		TEST_METHOD(AutoTakesTheVpStageOnlyWhileAStepWantsIt)
		{
			// CF1-FIN SHOULD A: both resolutions of auto. Resolved vp (a step configured) and a step wanting to act this
			// frame: the VP stage takes the LUT.
			CalibrationLutHost::StageFacts facts;
			facts.vpStageWanted = CalibrationLutHost::ResolveStage("auto",
				CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, false, "")) == "vp";
			facts.slotRuns = true;
			Assert::IsTrue(facts.vpStageWanted);
			CalibrationLutHost::StageFacts effective = facts;
			effective.vpStageWanted = facts.vpStageWanted && CalibrationLutHost::AutoKeepsVpStage(true, true);
			Assert::IsTrue(CalibrationLutHost::UseVpStage(effective, false), L"auto with a DV picture or appearance: vp");
			// Resolved vp but nothing wants to act (an HDR10 or SDR picture, appearance off): libplacebo, byte-identical.
			effective.vpStageWanted = facts.vpStageWanted && CalibrationLutHost::AutoKeepsVpStage(true, false);
			Assert::IsFalse(CalibrationLutHost::UseVpStage(effective, false), L"auto with no step acting: libplacebo");
			// An explicit vp passes through whatever the frame.
			Assert::IsTrue(CalibrationLutHost::AutoKeepsVpStage(false, false));
			// Resolved libplacebo never becomes vp.
			Assert::IsFalse(CalibrationLutHost::ResolveStage("auto", false) == "vp" && CalibrationLutHost::AutoKeepsVpStage(true, true));
			// The wiring: the host applies it, the colour segment feeds it after its decisions, the DV stage says when it wants it.
			const std::string host = Fix1ReadUp("src\\VP-V\\modules\\output\\calibration_lut\\CalibrationLutHost.cpp");
			Assert::IsTrue(Fix1Has(host, "AutoKeepsVpStage(autoConfigured, autoStepWants)"));
			Assert::IsTrue(Fix1Has(host, "if (UseVpStage(effective, stageFailed))"));
			const std::string core = Fix1ReadUp("src\\VP-V\\segments\\CoreSegments.inl");
			Assert::IsTrue(Fix1Has(core, "calibrationHost.SetAutoNeed(activeSettings.calibrationLutStageConfigured == \"auto\","));
			Assert::IsTrue(Fix1Has(core, "dvStage.DisplayMappingWantsLutAfter()"));
			const std::string stage = Fix1ReadUp("src\\VP-V\\modules\\colour\\display_mapping\\DvHostStage.h");
			Assert::IsTrue(Fix1Has(stage, "m_dmWantsLutAfter = dvTonePicture && ((f.trims && f.trimsStageRuns) || dvToneDolby);"));
		}

		TEST_METHOD(DefaultIsAutoAndTheResolvedStageIsSeparate)
		{
			const std::string cfg(kFix1ConfigDir);
			const std::string fields = Fix1ReadUp(cfg + "VpvRendererFields.h");
			const std::string settings = Fix1ReadUp(cfg + "VpvRendererSettings.cpp");
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
			Assert::IsTrue(Fix1Has(glsl, "float ap_flare(float o, float f)"));
			Assert::IsFalse(Fix1Has(glsl, "(outLin - vec3(apx_veil))"), L"the subtract-and-clip is gone");
		}

		// S2-B1 (pure CPU): PQ flare must normalise at the PQ ceiling (upper = 10000 / screenWhiteNits),
		// never at SDR 1.0, so HDR highlights above diffuse white (o > 1) are never boosted above o.
		TEST_METHOD(PqFlareNormalisesAtPeakAndNeverBoostsHighlights)
		{
			const double f = 0.05;
			const double upper = 10000.0 / 100.0;   // 100-nit screen white on 10,000-nit PQ ceiling
			bool usedThreeArg = false;

			// Boundary anchors: 0 maps to 0 and upper maps to upper; zero veil is identity.
			Assert::AreEqual(0.0, Flare3ArgProbe::CallCam16Flare(0.0, f, upper, usedThreeArg), 0.0);
			Assert::AreEqual(upper, Flare3ArgProbe::CallCam16Flare(upper, f, upper, usedThreeArg), 1e-9,
				L"PQ flare must anchor at the PQ peak ceiling (g(upper) == upper)");
			Assert::AreEqual(10.0, Flare3ArgProbe::CallCam16Flare(10.0, 0.0, upper, usedThreeArg), 0.0,
				L"zero veil must remain the identity on PQ highlights");

			// Strictly monotonic from shadows through diffuse white up to 10,000 nits;
			// every interior PQ highlight (0 < o < upper) stays strictly <= o (never boosted)
			// while displayed + flare stays above the intended luminance.
			const double samples[] = { 0.0, 1e-4, 0.01, 0.05, 0.25, 1.0, 2.0, 5.0, 10.0, 40.0, 80.0, upper };
			double prev = -1.0;
			for (const double o : samples)
			{
				const double g = Flare3ArgProbe::CallCam16Flare(o, f, upper, usedThreeArg);
				Assert::IsTrue(g > prev, L"PQ flare must be strictly monotonic across [0, upper]");
				if (o > 0.0 && o < upper)
				{
					Assert::IsTrue(g < o,
						(L"PQ flare must NEVER boost highlights above o (o=" + std::to_wstring(o) +
							L", g=" + std::to_wstring(g) + L")").c_str());
					Assert::IsTrue(g + f > o, L"seen light (displayed + flare) must not fall below intent");
				}
				prev = g;
			}
			Assert::IsTrue(usedThreeArg, L"Cam16::FlareToDisplayed must accept the PQ upper ceiling parameter");

			// Mandatory in-test negative control (exact same PQ highlight inputs):
			// SDR-normalised FlareToDisplayed(o, f) (upper = 1.0) strictly BOOSTS PQ highlights above o!
			for (const double o : { 5.0, 10.0, 40.0, 80.0 })
			{
				const double sdrNormalised = Cam16::FlareToDisplayed(o, f);
				Assert::IsTrue(sdrNormalised > o * 1.02,
					(L"Negative control: SDR-normalised flare (upper=1.0) must boost PQ highlight o=" +
						std::to_wstring(o) + L" above o (got " + std::to_wstring(sdrNormalised) + L")").c_str());
			}
		}
	};
	// MUST 6: appearance replaces the gamma boost and the saturation step (doc 115 B15), visibly.
	TEST_CLASS(AppearanceReplacesFix1Tests)
	{
	public:
		TEST_METHOD(GammaBoostStepsAsideWhileAppearanceRuns)
		{
			Assert::AreEqual(1.0, LedDrive::BoostExponentUnderAppearance(2.0, 2.4, true), 0.0);
			Assert::AreEqual(1.2, LedDrive::BoostExponentUnderAppearance(2.0, 2.4, false), 1e-12);
			Assert::AreEqual(1.0, LedDrive::BoostExponentUnderAppearance(0.0, 2.4, false), 0.0);
			const FallbackCatalog::Decision row = FallbackCatalog::GammaBoostReplaced(true);
			Assert::IsTrue(row.engaged);
			Assert::AreEqual(std::string("gamma boost: replaced by appearance"), row.id);
			Assert::IsTrue(std::string(FallbackPlainText::Sentence(row.id)).size() > 0);
			Assert::IsFalse(FallbackCatalog::GammaBoostReplaced(false).engaged);
		}

		TEST_METHOD(SaturationStepsAsideAndSaysTheValue)
		{
			const FallbackCatalog::Decision row = FallbackCatalog::SaturationReplaced(true, 5);
			Assert::IsTrue(row.engaged);
			Assert::AreEqual(std::string("saturation: replaced by appearance"), row.id);
			Assert::IsTrue(Fix1Has(row.reason, "saturation +5"));
			Assert::IsTrue(std::string(FallbackPlainText::Sentence(row.id)).size() > 0);
			Assert::IsFalse(FallbackCatalog::SaturationReplaced(false, 0).engaged);
		}

		TEST_METHOD(BothAreWiredInProduction)
		{
			const std::string settings = Fix1ReadUp(std::string(kFix1ConfigDir) + "VpvRendererSettings.cpp");
			const std::string frame = Fix1ReadUp("src\\VP-V\\modules\\drive\\led_compensation\\DynamicDriveFrame.cpp");
			const std::string lookahead = Fix1ReadUp("src\\VP-V\\modules\\drive\\led_measure\\LedLookahead.cpp");
			const std::string core = Fix1ReadUp("src\\VP-V\\segments\\CoreSegments.inl");
			Assert::IsTrue(Fix1Has(settings, "settings.saturationReplaced = appearanceRuns && settings.saturation != 0;"));
			Assert::IsTrue(Fix1Has(frame, "BoostExponentUnderAppearance(configuredBoost, displayGamma, in.appearanceActive)"));
			Assert::IsTrue(Fix1Has(lookahead, "module.BoostReplaced()"), L"the measurement sees the picture without the boost");
			Assert::IsTrue(Fix1Has(core, "led.appearanceActive = dvStage.AppearanceDecision().active;"));
			Assert::IsTrue(Fix1Has(core, "FallbackCatalog::SaturationReplaced(settings.saturationReplaced"));
			// CF1-FIN SHOULD B: replaced only while appearance acts: strength 0 replaces nothing, an inactive frame gives it back.
			Assert::IsTrue(Fix1Has(settings, "settings.appearance && settings.appearanceStrength > 0.0 && !settings.pipelineOriginalPathway"));
			Assert::IsTrue(Fix1Has(core, "const bool saturationGivenBack = activeSettings.saturationReplaced && !dvStage.AppearanceDecision().active;"));
			Assert::IsTrue(Fix1Has(core, "frameColorAdjustment.saturation = 1.0f + static_cast<float>(activeSettings.saturationConfigured) / 100.0f;"));
		}
	};

	// SHOULD 1-4 and NIT 1-2 (gate CF1-347e0546f): the reference white follows the content, the trim x Hunt cap, the configured
	// anchors / knee / white / luma, the per-channel BTB / WTW pass and the published inverse matrix.
	TEST_CLASS(AppearanceShouldFix1Tests)
	{
	public:
		static double Fix1Param(const AppearanceStage::Decision& d, const char* name)
		{
			for (const auto& p : d.parameters)
				if (std::string(p.first) == name)
					return p.second;
			Assert::Fail(L"parameter missing");
			return 0.0;
		}

		TEST_METHOD(ReferenceWhiteFollowsTheSourceContentNotTheOutput)
		{
			AppearanceStage::Inputs in;
			in.sdrTargetNits = 203.0;
			in.sourceSdr = true;
			Assert::AreEqual(100.0, AppearanceStage::ReferenceWhite(in), 0.0, L"SDR content refers to 100 nits");
			in.sourceSdr = false;
			Assert::AreEqual(203.0, AppearanceStage::ReferenceWhite(in), 0.0, L"HDR10 / DV without a trim / LLDV: sdr_target_nits");
			in.sourceSdr = true;
			in.trimApplied = true;
			in.trimTargetNits = 600.0;
			Assert::AreEqual(600.0, AppearanceStage::ReferenceWhite(in), 0.0, L"the trim target comes first");
			const std::string core = Fix1ReadUp("src\\VP-V\\segments\\CoreSegments.inl");
			Assert::IsTrue(Fix1Has(core, "appearance.sourceSdr = !pl_color_space_is_hdr(&renderImage.color);"));
			Assert::IsTrue(Fix1Has(core, "appearance.sdrTargetNits = activeSettings.sdrTargetNits;"));
			Assert::IsFalse(Fix1Has(core, "!pl_color_space_is_hdr(&target.color) ? display.hdr.max_luma"), L"no longer keyed by the output");
		}

		TEST_METHOD(TrimTimesHuntStaysWithinTheLargerAlone)
		{
			DvTrimSelection::ExtParams p;
			p.base.saturationGain = 1.2;
			p.saturation[3] = 1.25;
			Assert::AreEqual(1.5, DvTrimSelection::PeakSaturationGain(p), 1e-12);
			p.base.saturationGain = 1.9;
			Assert::AreEqual(2.0, DvTrimSelection::PeakSaturationGain(p), 0.0, L"clamped as the trim clamps it");

			Assert::AreEqual(1.0, AppearanceStage::HuntScale(false, 1.5, 1.3), 0.0, L"no trim: no cap");
			const double scale = AppearanceStage::HuntScale(true, 1.5, 1.3);
			Assert::IsTrue(1.5 * 1.3 * scale <= 1.5 + 1e-12, L"T x H x scale within max(T, H)");
			Assert::IsTrue(scale < 1.0);

			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			in.screenWhiteNits = 48.0;
			const AppearanceStage::Decision uncapped = AppearanceStage::Decide(in, s);
			Assert::AreEqual(1.0, Fix1Param(uncapped, "apx_hunt_scale"), 0.0);
			Assert::IsTrue(uncapped.huntGain > 1.0, L"a dimmer screen than the reference raises chroma (Hunt)");
			in.trimApplied = true;
			in.trimTargetNits = 100.0;
			in.trimSaturationGain = 1.5;
			const AppearanceStage::Decision capped = AppearanceStage::Decide(in, s);
			Assert::IsTrue(Fix1Param(capped, "apx_hunt_scale") < 1.0, L"the cap reaches the shader");
			Assert::IsTrue(1.5 * capped.huntGain * capped.huntScale <= std::max(1.5, capped.huntGain) + 1e-9);

			const std::string core = Fix1ReadUp("src\\VP-V\\segments\\CoreSegments.inl");
			const std::string glsl = Fix1ReadUp("shaders\\vp_cam16_appearance.glsl");
			Assert::IsTrue(Fix1Has(core, "appearance.trimSaturationGain = dvStage.TrimsSaturationGain();"));
			Assert::IsTrue(Fix1Has(glsl, "jmh.y *= apx_hunt_scale;"));
		}

		TEST_METHOD(WhiteKneeAnchorsAndLumaAreConfiguredNotFixed)
		{
			// The output white is the matrix's own white (BT.709 primaries: D65).
			double white[3] = {};
			const double bt709[9] = { 0.4124, 0.3576, 0.1805, 0.2126, 0.7152, 0.0722, 0.0193, 0.1192, 0.9505 };
			AppearanceStage::WhiteXyz(bt709, white);
			Assert::AreEqual(95.05, white[0], 0.01);
			Assert::AreEqual(100.0, white[1], 1e-9);
			Assert::AreEqual(108.9, white[2], 0.01);
			const double d50[3] = { 96.42, 100.0, 82.51 };
			const Cam16::Conditions c = Cam16::MakeConditions(100.0, Cam16::kSurroundDim, 0.2, 20.0, d50);
			Assert::AreEqual(82.51, c.whiteXyz[2], 1e-9, L"the conditions carry the output's white, not a fixed D65");
			Cam16::UniformInputs a, b;
			b.whiteXyz[0] = d50[0];
			b.whiteXyz[2] = d50[2];
			Assert::AreNotEqual(Cam16::PackUniforms(a).reference.dc[2], Cam16::PackUniforms(b).reference.dc[2]);

			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			in.toeKnee = 0.1;
			Assert::AreEqual(0.1, Fix1Param(AppearanceStage::Decide(in, s), "apx_knee"), 1e-12, L"the knee is gamma_boost_knee");

			const std::string core = Fix1ReadUp("src\\VP-V\\segments\\CoreSegments.inl");
			const std::string settings = Fix1ReadUp(std::string(kFix1ConfigDir) + "VpvRendererSettings.cpp");
			const std::string glsl = Fix1ReadUp("shaders\\vp_cam16_appearance.glsl");
			Assert::IsTrue(Fix1Has(settings, "Ambient::Read(config, ambientRoom, ambientError)"), L"[ambient] lux anchors read by their owner");
			Assert::IsTrue(Fix1Has(core, "appearance.room.anchors.luxDark = activeSettings.ambientLuxDark;"));
			Assert::IsTrue(Fix1Has(core, "appearance.room.anchors.luxLit = activeSettings.ambientLuxLit;"));
			Assert::IsTrue(Fix1Has(core, "appearance.toeKnee = dynamicDrive.PublishedParams().boostKnee;"));
			Assert::IsTrue(Fix1Has(glsl, "dot(lin, vec3(apm_10, apm_11, apm_12))"), L"luma from the output primaries");
			Assert::IsFalse(Fix1Has(glsl, "dot(lin, vec3(0.2126, 0.7152, 0.0722))"));
		}

		TEST_METHOD(BtbWtwPassesPerChannelWithoutAnEdge)
		{
			const std::string glsl = Fix1ReadUp("shaders\\vp_cam16_appearance.glsl");
			Assert::IsFalse(Fix1Has(glsl, "color.r < 0.0 || color.g < 0.0 || color.b < 0.0 || color.r > 1.0"),
				L"no whole-pixel skip when one channel leaves the range");
			Assert::IsTrue(Fix1Has(glsl, "if (outside && (allBelow || allAbove))"), L"a pixel wholly outside still passes exactly");
			Assert::IsTrue(Fix1Has(glsl, "ap_to_rel(inRange.r)"), L"the model runs on the in-range colour");
			Assert::IsTrue(Fix1Has(glsl, "+ excess;"), L"each channel's excess is restored");
		}

		TEST_METHOD(InverseMatrixIsPublishedNotInvertedPerPixel)
		{
			const double bt2020[9] = { 0.6370, 0.1446, 0.1689, 0.2627, 0.6780, 0.0593, 0.0, 0.0281, 1.0610 };
			double inv[9] = {};
			Assert::IsTrue(DvMath::Invert(bt2020, inv));   // CF1-FIN MUST 1: the one home
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
				{
					double v = 0.0;
					for (int k = 0; k < 3; ++k)
						v += bt2020[r * 3 + k] * inv[k * 3 + c];
					Assert::AreEqual(r == c ? 1.0 : 0.0, v, 1e-9);
				}
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			for (int i = 0; i < 9; ++i)
				in.rgbToXyz[i] = bt2020[i];
			const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
			Assert::AreEqual(inv[0], Fix1Param(d, "apn_00"), 1e-12);
			Assert::AreEqual(inv[8], Fix1Param(d, "apn_22"), 1e-12);
			const std::string glsl = Fix1ReadUp("shaders\\vp_cam16_appearance.glsl");
			Assert::IsFalse(Fix1Has(glsl, "inverse(toXyz)"));
			Assert::IsTrue(Fix1Has(glsl, "a hard 6-step bisection"), L"NIT 2: the comment says what the compression is");
		}

		// S2-H2: CAM16 appearance gates unsupported output transfers visibly (fallback + status),
		// describes PQ / pure-power / BT.1886 / sRGB piecewise curves accurately, and wires both
		// the host (CoreSegments.inl) and shader (vp_cam16_appearance.glsl).
		TEST_METHOD(AppearanceGatesUnsupportedOutputTransfersAndDescribesPiecewiseCurves)
		{
			auto makeSpace = [](enum pl_color_primaries p, enum pl_color_transfer t)
			{
				pl_color_space c{};
				c.primaries = p;
				c.transfer = t;
				return c;
			};

			// 1. Supported transfers: PQ, pure-power GAMMA18..28, BT.1886 (kind=1, param=b), sRGB (kind=2, param=0).
			{
				const ExtendedRange::OutputTransfer pq = ExtendedRange::DescribeOutputTransfer(makeSpace(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ));
				Assert::IsTrue(pq.supported, L"PQ output transfer must be supported");
				Assert::IsTrue(pq.pq, L"PQ output transfer must set pq=true");
				Assert::AreEqual(0, pq.kind, L"PQ output transfer kind must be 0");

				const enum pl_color_transfer gammas[] = {
					PL_COLOR_TRC_GAMMA18, PL_COLOR_TRC_GAMMA20, PL_COLOR_TRC_GAMMA22,
					PL_COLOR_TRC_GAMMA24, PL_COLOR_TRC_GAMMA26, PL_COLOR_TRC_GAMMA28
				};
				for (const enum pl_color_transfer gTrc : gammas)
				{
					const ExtendedRange::OutputTransfer g = ExtendedRange::DescribeOutputTransfer(makeSpace(PL_COLOR_PRIM_BT_709, gTrc));
					Assert::IsTrue(g.supported, L"pure-power gamma output transfer must be supported");
					Assert::IsFalse(g.pq, L"pure-power gamma must set pq=false");
					Assert::AreEqual(0, g.kind, L"pure-power gamma kind must be 0");
				}

				// BT.1886: explicit 1000:1 SDR contrast -> b = q / (1 - q), q = (1/1000)^(1/2.4).
				pl_color_space bt1886Space = makeSpace(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
				bt1886Space.hdr.min_luma = PL_COLOR_SDR_WHITE / 1000.0f;
				bt1886Space.hdr.max_luma = PL_COLOR_SDR_WHITE;
				const ExtendedRange::OutputTransfer bt1886 = ExtendedRange::DescribeOutputTransfer(bt1886Space);
				Assert::IsTrue(bt1886.supported, L"BT.1886 output transfer must be supported");
				Assert::IsFalse(bt1886.pq, L"BT.1886 must set pq=false");
				Assert::AreEqual(1, bt1886.kind, L"BT.1886 kind must be 1");
				const double q1000 = std::pow(1.0 / 1000.0, 1.0 / 2.4);
				const double expectedB1000 = q1000 / (1.0 - q1000);
				Assert::AreEqual(expectedB1000, bt1886.param, 1e-4, L"BT.1886 param must match ITU-R BT.1886 black offset b");

				// Adversarial check: a higher-contrast BT.1886 display (2000:1) must yield a strictly smaller b (not hard-coded).
				pl_color_space bt1886HiContrast = makeSpace(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
				bt1886HiContrast.hdr.min_luma = PL_COLOR_SDR_WHITE / 2000.0f;
				bt1886HiContrast.hdr.max_luma = PL_COLOR_SDR_WHITE;
				const ExtendedRange::OutputTransfer bt1886Hi = ExtendedRange::DescribeOutputTransfer(bt1886HiContrast);
				Assert::IsTrue(bt1886Hi.supported && !bt1886Hi.pq && bt1886Hi.kind == 1);
				Assert::IsTrue(bt1886Hi.param > 0.0 && bt1886Hi.param < bt1886.param,
					L"BT.1886 black offset b must track the target display's black level");

				// Default-inferred BT.1886 (min_luma/max_luma unset = 0) must also succeed cleanly.
				const ExtendedRange::OutputTransfer bt1886Default = ExtendedRange::DescribeOutputTransfer(makeSpace(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886));
				Assert::IsTrue(bt1886Default.supported && !bt1886Default.pq && bt1886Default.kind == 1 && bt1886Default.param >= 0.0,
					L"default-inferred BT.1886 output transfer must be supported");

				const ExtendedRange::OutputTransfer srgb = ExtendedRange::DescribeOutputTransfer(makeSpace(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_SRGB));
				Assert::IsTrue(srgb.supported, L"sRGB output transfer must be supported");
				Assert::IsFalse(srgb.pq, L"sRGB must set pq=false");
				Assert::AreEqual(2, srgb.kind, L"sRGB kind must be 2");
				Assert::AreEqual(0.0, srgb.param, 0.0, L"sRGB param must be 0");
			}

			// 2. Negative controls: unsupported output transfers MUST be rejected (supported == false, pq == false).
			{
				const enum pl_color_transfer unsupported[] = {
					PL_COLOR_TRC_HLG, PL_COLOR_TRC_LINEAR, PL_COLOR_TRC_V_LOG,
					PL_COLOR_TRC_S_LOG1, PL_COLOR_TRC_S_LOG2, PL_COLOR_TRC_PRO_PHOTO,
					PL_COLOR_TRC_ST428, PL_COLOR_TRC_UNKNOWN
				};
				for (const enum pl_color_transfer uTrc : unsupported)
				{
					const pl_color_primaries prim = (uTrc == PL_COLOR_TRC_HLG || uTrc == PL_COLOR_TRC_V_LOG) ? PL_COLOR_PRIM_BT_2020 : PL_COLOR_PRIM_BT_709;
					const ExtendedRange::OutputTransfer trc = ExtendedRange::DescribeOutputTransfer(makeSpace(prim, uTrc));
					Assert::IsFalse(trc.supported, L"negative control: unsupported output transfer must be rejected");
					Assert::IsFalse(trc.pq, L"negative control: unsupported output transfer must not claim PQ");
				}
			}

			// 3. FallbackCatalog entry + plain-text notice sentence.
			{
				const FallbackCatalog::Decision engaged = FallbackCatalog::AppearanceOutputTransfer(true);
				Assert::IsTrue(engaged.engaged);
				Assert::AreEqual(std::string("appearance: output transfer"), engaged.id);
				Assert::IsFalse(engaged.reason.empty());
				Assert::IsTrue(std::string(FallbackPlainText::Sentence(engaged.id)).size() > 0,
					L"FallbackPlainText::Sentence must have a plain sentence for appearance: output transfer");
				Assert::IsFalse(FallbackCatalog::AppearanceOutputTransfer(false).engaged);
			}

			// 4. Real DvHostStage::PrepareAppearance + FinishAppearance sync and clean release.
			{
				Fix1Registry reg;
				DvHostStage stage(Fix1NoShaders(), reg.Engage(), reg.Release());
				CustomHookChain::TemplateValues hookTemplates;
				AppearanceStage::Inputs in;
				in.on = true;
				in.transferSupported = false;

				const AppearanceStage::Decision& off = stage.PrepareAppearance(in, hookTemplates, 0);
				stage.FinishAppearance(0);
				Assert::IsFalse(off.active, L"unsupported output transfer must keep appearance inactive");
				Assert::IsTrue(off.transferUnsupported);
				Assert::IsTrue(reg.Has("appearance: output transfer"), L"output transfer gate must engage fallback registry entry");
				Assert::IsFalse(reg.Has("Colour appearance"), L"gated transfer must not be reported as a missing shader");
				Assert::IsTrue(Fix1Has(stage.AppearanceStatus(), "output transfer"), L"render-thread status must mention output transfer");
				Assert::IsTrue(Fix1Has(stage.AppearanceStatusShown(), "output transfer"), L"Ctrl+I status must mention output transfer");

				// Negative control / clean release on the exact same stage & inputs once transfer is supported.
				in.transferSupported = true;
				in.transferKind = 1;
				in.transferParam = 0.05;
				const AppearanceStage::Decision& on = stage.PrepareAppearance(in, hookTemplates, 0);
				stage.FinishAppearance(1);
				Assert::IsTrue(on.active, L"supported transfer must re-enable appearance");
				Assert::IsFalse(on.transferUnsupported);
				Assert::IsFalse(reg.Has("appearance: output transfer"), L"fallback entry must release cleanly once transfer is supported");
				Assert::IsFalse(Fix1Has(stage.AppearanceStatus(), "off for this output transfer"));

				// Off by user choice on an unsupported transfer must NOT engage the fallback.
				in.on = false;
				in.transferSupported = false;
				stage.PrepareAppearance(in, hookTemplates, 0);
				stage.FinishAppearance(0);
				Assert::IsFalse(reg.Has("appearance: output transfer"), L"appearance off by choice must not engage output transfer fallback");
			}

			// 5. Production wiring (CoreSegments.inl) and shader piecewise transfer implementation (vp_cam16_appearance.glsl).
			{
				const std::string core = Fix1ReadUp("src\\VP-V\\segments\\CoreSegments.inl");
				Assert::IsTrue(Fix1Has(core, "ExtendedRange::DescribeOutputTransfer(target.color)"),
					L"CoreSegments.inl must describe target.color via ExtendedRange::DescribeOutputTransfer");
				Assert::IsTrue(Fix1Has(core, "appearance.transferSupported"), L"CoreSegments.inl must wire appearance.transferSupported");
				Assert::IsTrue(Fix1Has(core, "appearance.transferKind"), L"CoreSegments.inl must wire appearance.transferKind");
				Assert::IsTrue(Fix1Has(core, "appearance.transferParam"), L"CoreSegments.inl must wire appearance.transferParam");

				const std::string glsl = Fix1ReadUp("shaders\\vp_cam16_appearance.glsl");
				Assert::IsTrue(Fix1Has(glsl, "apx_trc_kind"), L"shader must declare apx_trc_kind");
				Assert::IsTrue(Fix1Has(glsl, "apx_trc_param"), L"shader must declare apx_trc_param");
				Assert::IsTrue(Fix1Has(glsl, "ap_trc_lin"), L"shader must implement ap_trc_lin for piecewise transfers");
				Assert::IsTrue(Fix1Has(glsl, "ap_trc_enc"), L"shader must implement ap_trc_enc for piecewise transfers");
			}
		}
	};

	// SHOULD 6 (gate CF1-347e0546f): room_light has a catalog reader row, the appearance stage at lag 1 (held state), with its own
	// fallback entry, so the graph checks the read instead of counting it unread.
	TEST_CLASS(AppearanceRoomLightFix1Tests)
	{
	public:
		TEST_METHOD(AppearanceReadsRoomLightAtLagOneWithItsFallback)
		{
			using namespace VpSkeleton;
			using namespace VpSkeleton::Catalog;
			const std::vector<const ConsumerRow*> readers = ConsumersOf(kRoomLight);
			Assert::AreEqual(size_t(1), readers.size(), L"one reader row: the appearance stage");
			const ConsumerRow& row = *readers[0];
			Assert::AreEqual(std::string("VP-V/modules/colour/appearance/AppearanceStage"), std::string(row.module));
			Assert::IsTrue(row.unit == PathwayUnit::Colour);
			bool read = false;
			for (size_t k = 0; k < row.decl.count; ++k)
				if (row.decl.consumes[k].product == kRoomLight)
				{
					read = true;
					Assert::IsTrue(row.decl.consumes[k].lag >= 1, L"a held-state read is lag >= 1");
				}
			Assert::IsTrue(read);
			Assert::IsNull(LiveFillOwner(kRoomLight), L"held state: nothing fills it live");
			Assert::AreEqual(std::string("product.room_light"), std::string(row.fallbackId));
			Assert::IsNotNull(FallbackCatalog::FindProductEntry("product.room_light"), L"the fallback id exists");
		}
	};

	// SHOULD 5 (gate CF1-347e0546f): appearance OFF (and every other appearance state) is a Ctrl+I row through the VP-V status
	// seam (VpStatusText "appearance" key -> StatusExtension::CtrlIRows "Appearance:"), not only the Config card.
	TEST_CLASS(AppearanceCtrlIFix1Tests)
	{
	public:
		TEST_METHOD(AppearanceOffReachesTheCtrlIRows)
		{
			VpStatusText::RendererLines<char> lines;
			lines.appearance.Set(true, "appearance: off");
			const std::string text = VpStatusText::ComposeRendererModule(lines);
			Assert::IsTrue(text.find("appearance=appearance: off") != std::string::npos, L"the renderer module carries the line");
			const VpStatusText::RendererLines<char> back = VpStatusText::ReadRendererModule(text);
			Assert::IsTrue(back.appearance.Shown());
			Assert::AreEqual(std::string("appearance: off"), back.appearance.text);

			VpStatusText::CtrlILines<char> ctrlI;
			ctrlI.appearance = back.appearance.text;
			const VpStatusText::CtrlIRows<char> rows = VpStatusText::BuildCtrlIRows(ctrlI);
			bool shown = false;
			for (const auto& row : rows.afterShaders)
				shown = shown || (row.label == "Appearance:" && row.value == "appearance: off");
			Assert::IsTrue(shown, L"Ctrl+I shows Appearance: appearance: off");

			const std::string ext = Fix1ReadUp("src\\VP-V\\modules\\control\\diagnostics\\StatusExtension.cpp");
			const std::string methods = Fix1ReadUp("src\\VP-V\\modules\\control\\diagnostics\\StatusMethods.inl");
			const std::string stage = Fix1ReadUp("src\\VP-V\\modules\\colour\\display_mapping\\DvHostStage.h");
			Assert::IsTrue(Fix1Has(ext, "lines.appearance = snapshot.vp.appearance.text;"), L"the GUI side passes it to the rows");
			Assert::IsTrue(Fix1Has(methods, "lines.appearance.Set(true, CString(m_impl->dvStage.AppearanceStatusShown().c_str()).GetString());"),
				L"the renderer fills it from the stage");
			Assert::IsTrue(Fix1Has(stage, "m_appearanceShown = m_appearanceStatus;"), L"the stage publishes its status line");
		}
	};
}
