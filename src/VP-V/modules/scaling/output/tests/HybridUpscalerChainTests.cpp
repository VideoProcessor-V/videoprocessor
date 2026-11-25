// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Q2-B3 + G12 + B4 (CPU): adversarial verification of hybrid upscaler config keys,
// cnn_residual_stabilise -> sharp_residual_stabilise migration, G12 VP-V default Lanczos downscaler
// vs Original Hermite negative control, and Rebase -> Route4k -> AdaptiveDownscaleStage chain
// no-double-upscale invariant.
#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <MainConfigSchema.h>
#include <RendererProfileConfig.h>
#include <VP-V/core/OriginalPathway.h>
#include <VP-V/modules/control/config/VpvRendererFields.h>
#include <VP-V/modules/control/config/VpvRendererSettings.h>
#include <VP-V/modules/control/config/editor/SharpResidualMigration.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleStage.h>
#include <VP-V/modules/scaling/route_4k/Route4k.h>

#include <libplacebo/filters.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace HybridUpscalerChainDetail
	{
		inline std::wstring Wide(const std::string& s)
		{
			return std::wstring(s.begin(), s.end());
		}

		inline bool HasDelta(const std::vector<const char*>& delta, const char* key)
		{
			for (const char* k : delta)
				if (k && std::strcmp(k, key) == 0)
					return true;
			return false;
		}

		struct TempCfgScope
		{
			std::string path;
			TempCfgScope()
			{
				char dir[MAX_PATH] = {};
				GetTempPathA(MAX_PATH, dir);
				static unsigned long counter = 0;
				++counter;
				path = std::string(dir) + "vpv_hybrid_chain_" + std::to_string(GetCurrentProcessId()) + "_"
					+ std::to_string(counter) + ".cfg";
			}
			~TempCfgScope()
			{
				if (!path.empty())
					std::remove(path.c_str());
			}
			TempCfgScope(const TempCfgScope&) = delete;
			TempCfgScope& operator=(const TempCfgScope&) = delete;
		};
	}

	TEST_CLASS(HybridUpscalerChainTests)
	{
	public:
		TEST_METHOD(B3SchemaValidationAndConservativeM4Defaults)
		{
			using namespace HybridUpscalerChainDetail;

			auto schemaValid = [](const char* key, const char* val) {
				std::string expected;
				return RendererProfileConfig::ValidateProfileSetting("input", key, val, expected);
			};

			// upscale_route
			for (const char* ok : { "off", "rebased_only", "always_4k" })
				Assert::IsTrue(schemaValid("upscale_route", ok), (L"upscale_route must accept " + Wide(ok)).c_str());
			for (const char* bad : { "on", "2x", "zzz", "" })
				Assert::IsFalse(schemaValid("upscale_route", bad), (L"negative control: upscale_route must reject " + Wide(bad)).c_str());

			// upscale_sharp
			for (const char* ok : { "lanczos3", "ewa_lanczossharp", "catmull_rom", "lanczos2", "lanczos4", "bicubic" })
				Assert::IsTrue(schemaValid("upscale_sharp", ok), (L"upscale_sharp must accept " + Wide(ok)).c_str());
			for (const char* bad : { "hermite", "bilinear", "box", "invalid_k", "" })
				Assert::IsFalse(schemaValid("upscale_sharp", bad), (L"negative control: upscale_sharp must reject " + Wide(bad)).c_str());

			// upscale_safe
			for (const char* ok : { "hermite", "lanczos3", "catmull_rom", "ewa_lanczossharp", "bicubic", "bilinear" })
				Assert::IsTrue(schemaValid("upscale_safe", ok), (L"upscale_safe must accept " + Wide(ok)).c_str());
			for (const char* bad : { "lanczos2", "lanczos4", "box", "invalid_k", "" })
				Assert::IsFalse(schemaValid("upscale_safe", bad), (L"negative control: upscale_safe must reject " + Wide(bad)).c_str());

			// upscale_sharp_antiring
			for (const char* ok : { "0.0", "0.5", "1.0", "0", "1" })
				Assert::IsTrue(schemaValid("upscale_sharp_antiring", ok), (L"upscale_sharp_antiring must accept " + Wide(ok)).c_str());
			for (const char* bad : { "-0.1", "1.5", "abc", "" })
				Assert::IsFalse(schemaValid("upscale_sharp_antiring", bad), (L"negative control: upscale_sharp_antiring must reject " + Wide(bad)).c_str());

			// sharp_residual_stabilise + legacy alias cnn_residual_stabilise
			for (const char* ok : { "on", "off" })
			{
				Assert::IsTrue(schemaValid("sharp_residual_stabilise", ok), (L"sharp_residual_stabilise must accept " + Wide(ok)).c_str());
				Assert::IsTrue(schemaValid("cnn_residual_stabilise", ok), (L"legacy cnn_residual_stabilise must still accept " + Wide(ok)).c_str());
			}
			for (const char* bad : { "auto", "1", "zzz", "" })
			{
				Assert::IsFalse(schemaValid("sharp_residual_stabilise", bad), (L"negative control: sharp_residual_stabilise must reject " + Wide(bad)).c_str());
				Assert::IsFalse(schemaValid("cnn_residual_stabilise", bad), (L"negative control: cnn_residual_stabilise must reject " + Wide(bad)).c_str());
			}

			// Conservative M4 defaults on VpvRenderer::Fields
			const VpvRenderer::Fields def{};
			Assert::IsFalse(def.adaptiveUpscale, L"M4 default: adaptiveUpscale == false");
			Assert::AreEqual(0.0f, def.adaptive.motionStrength, 1e-7f, L"M4 default: motionStrength == 0.0f");
			Assert::AreEqual(std::string("off"), def.upscaleRoute, L"M4 default: upscaleRoute == off");
			Assert::AreEqual(std::string("lanczos3"), def.upscaleSharp, L"M4 default: upscaleSharp == lanczos3");
			Assert::AreEqual(std::string("hermite"), def.upscaleSafe, L"M4 default: upscaleSafe == hermite");
			Assert::AreEqual(0.5, def.upscaleSharpAntiring, 1e-9, L"M4 default: upscaleSharpAntiring == 0.5");
			Assert::IsTrue(def.adaptive.cnnResidualStabilise, L"M4 default: cnnResidualStabilise == true");
		}

		TEST_METHOD(B3RendererSettingsLifecycleOriginalResetAndDelta)
		{
			using namespace HybridUpscalerChainDetail;

			TempCfgScope tmp;
			auto writeAndLoad = [&](const std::string& body, VpvRenderer::Fields& outFields, bool checkSchema) {
				{
					std::ofstream out(tmp.path, std::ios::trunc | std::ios::binary);
					out << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n[vprenderer.Default]\nquality: high\n"
						<< body;
				}
				ConfigFile cfg;
				Assert::IsTrue(cfg.Load(tmp.path, ConfigFile::ReadPolicy::Fresh), L"temp config load failed");
				if (checkSchema)
				{
					std::string err;
					Assert::IsTrue(MainConfigSchema::Validate(cfg, err),
						(L"MainConfigSchema::Validate rejected valid B3 keys: " + Wide(err)).c_str());
				}
				VpvRendererSettings::ReadProfile(cfg, "vprenderer.Default", "Default", false, outFields);
			};

			// 1. Non-default load of all 5 B3 keys.
			VpvRenderer::Fields loaded{};
			writeAndLoad(
				"upscale_route: always_4k\n"
				"upscale_sharp: ewa_lanczossharp\n"
				"upscale_safe: catmull_rom\n"
				"upscale_sharp_antiring: 0.75\n"
				"sharp_residual_stabilise: off\n",
				loaded, true);
			Assert::AreEqual(std::string("always_4k"), loaded.upscaleRoute);
			Assert::AreEqual(std::string("ewa_lanczossharp"), loaded.upscaleSharp);
			Assert::AreEqual(std::string("catmull_rom"), loaded.upscaleSafe);
			Assert::AreEqual(0.75, loaded.upscaleSharpAntiring, 1e-9);
			Assert::IsFalse(loaded.adaptive.cnnResidualStabilise);

			// 2. sharp_residual_stabilise takes precedence over legacy cnn_residual_stabilise when both appear.
			{
				VpvRenderer::Fields precOn{};
				writeAndLoad("cnn_residual_stabilise: off\nsharp_residual_stabilise: on\n", precOn, true);
				Assert::IsTrue(precOn.adaptive.cnnResidualStabilise,
					L"sharp_residual_stabilise=on must override legacy cnn_residual_stabilise=off");

				VpvRenderer::Fields precOff{};
				writeAndLoad("sharp_residual_stabilise: off\ncnn_residual_stabilise: on\n", precOff, true);
				Assert::IsFalse(precOff.adaptive.cnnResidualStabilise,
					L"sharp_residual_stabilise=off must override legacy cnn_residual_stabilise=on regardless of line order");

				VpvRenderer::Fields legacyOnly{};
				writeAndLoad("cnn_residual_stabilise: off\n", legacyOnly, true);
				Assert::IsFalse(legacyOnly.adaptive.cnnResidualStabilise,
					L"legacy cnn_residual_stabilise alone must still set adaptive.cnnResidualStabilise");
			}

			// 3. Negative control: invalid values retain prior valid settings untouched.
			{
				VpvRenderer::Fields retain = loaded;
				writeAndLoad(
					"upscale_route: 2x\n"
					"upscale_sharp: bogus_sharp\n"
					"upscale_safe: bogus_safe\n"
					"upscale_sharp_antiring: 1.5\n",
					retain, false);
				Assert::AreEqual(std::string("always_4k"), retain.upscaleRoute, L"invalid upscale_route must retain prior value");
				Assert::AreEqual(std::string("ewa_lanczossharp"), retain.upscaleSharp, L"invalid upscale_sharp must retain prior value");
				Assert::AreEqual(std::string("catmull_rom"), retain.upscaleSafe, L"invalid upscale_safe must retain prior value");
				Assert::AreEqual(0.75, retain.upscaleSharpAntiring, 1e-9, L"invalid upscale_sharp_antiring must retain prior value");
			}

			// 4. PostLoad: Original pathway resets all 5 keys while VP-V pathway preserves them (same-input negative control).
			{
				VpvRenderer::Fields vpv = loaded;
				vpv.adaptive.cnnResidualStabilise = true;
				VpvRenderer::Fields orig = vpv;

				vpv.pipelineOriginalPathway = false;
				VpvRendererSettings::PostLoad(vpv, false, nullptr);
				Assert::AreEqual(std::string("always_4k"), vpv.upscaleRoute, L"VP-V pathway preserves upscaleRoute");
				Assert::AreEqual(std::string("ewa_lanczossharp"), vpv.upscaleSharp, L"VP-V pathway preserves upscaleSharp");
				Assert::AreEqual(std::string("catmull_rom"), vpv.upscaleSafe, L"VP-V pathway preserves upscaleSafe");
				Assert::AreEqual(0.75, vpv.upscaleSharpAntiring, 1e-9, L"VP-V pathway preserves upscaleSharpAntiring");
				Assert::IsTrue(vpv.adaptive.cnnResidualStabilise, L"VP-V pathway preserves cnnResidualStabilise");

				orig.pipelineOriginalPathway = true;
				VpvRendererSettings::PostLoad(orig, false, nullptr);
				Assert::AreEqual(std::string("off"), orig.upscaleRoute, L"Original pathway resets upscaleRoute to off");
				Assert::AreEqual(std::string("lanczos3"), orig.upscaleSharp, L"Original pathway resets upscaleSharp to lanczos3");
				Assert::AreEqual(std::string("hermite"), orig.upscaleSafe, L"Original pathway resets upscaleSafe to hermite");
				Assert::AreEqual(0.5, orig.upscaleSharpAntiring, 1e-9, L"Original pathway resets upscaleSharpAntiring to 0.5");
				Assert::IsFalse(orig.adaptive.cnnResidualStabilise, L"Original pathway resets cnnResidualStabilise to false");

				// Verify OriginalPathway::Overrides() table carries every B3 key.
				const auto& ov = OriginalPathway::Overrides();
				auto findOv = [&](const char* key) -> const OriginalPathway::Override* {
					for (const auto& entry : ov)
						if (std::strcmp(entry.key, key) == 0)
							return &entry;
					return nullptr;
				};
				Assert::IsTrue(findOv("upscale_route") && std::strcmp(findOv("upscale_route")->value, "off") == 0);
				Assert::IsTrue(findOv("upscale_sharp") && std::strcmp(findOv("upscale_sharp")->value, "lanczos3") == 0);
				Assert::IsTrue(findOv("upscale_safe") && std::strcmp(findOv("upscale_safe")->value, "hermite") == 0);
				Assert::IsTrue(findOv("upscale_sharp_antiring") && std::strcmp(findOv("upscale_sharp_antiring")->value, "0.5") == 0);
				Assert::IsTrue(findOv("sharp_residual_stabilise") && std::strcmp(findOv("sharp_residual_stabilise")->value, "off") == 0);
			}

			// 5. Fingerprint, AppendDelta, CopyLive, and Describe reflect every B3 field.
			{
				const VpvRenderer::Fields base{};
				const std::string baseFp = VpvRendererSettings::Fingerprint(base, true);

				auto checkSensitivity = [&](const VpvRenderer::Fields& mod, const char* expectedDeltaKey) {
					Assert::AreNotEqual(baseFp, VpvRendererSettings::Fingerprint(mod, true),
						(L"Fingerprint must reflect change in " + Wide(expectedDeltaKey)).c_str());
					std::vector<const char*> delta;
					VpvRendererSettings::AppendDelta(base, mod, delta);
					Assert::IsTrue(HasDelta(delta, expectedDeltaKey),
						(L"AppendDelta must report " + Wide(expectedDeltaKey)).c_str());
				};

				{ VpvRenderer::Fields m = base; m.upscaleRoute = "rebased_only"; checkSensitivity(m, "upscale_route"); }
				{ VpvRenderer::Fields m = base; m.upscaleSharp = "catmull_rom"; checkSensitivity(m, "upscale_sharp"); }
				{ VpvRenderer::Fields m = base; m.upscaleSafe = "bicubic"; checkSensitivity(m, "upscale_safe"); }
				{ VpvRenderer::Fields m = base; m.upscaleSharpAntiring = 0.25; checkSensitivity(m, "upscale_sharp_antiring"); }
				{ VpvRenderer::Fields m = base; m.adaptive.cnnResidualStabilise = false; checkSensitivity(m, "adaptive_downscale"); }

				VpvRenderer::Fields dst{};
				VpvRendererSettings::CopyLive(dst, loaded);
				Assert::AreEqual(loaded.upscaleRoute, dst.upscaleRoute);
				Assert::AreEqual(loaded.upscaleSharp, dst.upscaleSharp);
				Assert::AreEqual(loaded.upscaleSafe, dst.upscaleSafe);
				Assert::AreEqual(loaded.upscaleSharpAntiring, dst.upscaleSharpAntiring, 1e-9);
				Assert::AreEqual(loaded.adaptive.cnnResidualStabilise, dst.adaptive.cnnResidualStabilise);

				const Dejudder::Settings ms{};
				const std::string desc = VpvRendererSettings::Describe(loaded, "", ms, "Default");
				Assert::IsTrue(desc.find("\nupscale_route=always_4k\n") != std::string::npos);
				Assert::IsTrue(desc.find("\nupscale_sharp=ewa_lanczossharp\n") != std::string::npos);
				Assert::IsTrue(desc.find("\nupscale_safe=catmull_rom\n") != std::string::npos);
				Assert::IsTrue(desc.find("\nupscale_sharp_antiring=0.75\n") != std::string::npos);
				Assert::IsTrue(desc.find("\nsharp_residual_stabilise=off\n") != std::string::npos);
				Assert::IsTrue(desc.find("\ncnn_residual_stabilise=off\n") != std::string::npos);
			}
		}

		TEST_METHOD(B3SharpResidualMigrationHelperAndPrecedenceNegativeControl)
		{
			// 1. Legacy cnn_residual_stabilise migrates cleanly to sharp_residual_stabilise and removes legacy key.
			{
				std::map<std::string, std::string> sec;
				sec["cnn_residual_stabilise"] = "off";
				sec["quality"] = "high";
				Assert::IsTrue(Vpv::MigrateSharpResidualStabiliseSection(sec),
					L"legacy cnn_residual_stabilise must migrate and return true");
				Assert::IsTrue(sec.find("cnn_residual_stabilise") == sec.end(),
					L"legacy cnn_residual_stabilise must be removed after migration");
				Assert::IsTrue(sec.find("sharp_residual_stabilise") != sec.end() && sec["sharp_residual_stabilise"] == "off",
					L"migrated sharp_residual_stabilise must preserve legacy value 'off'");
				Assert::AreEqual(std::string("high"), sec["quality"], L"unrelated keys untouched");
			}

			// 2. Precedence vs legacy-only contrast: explicit sharp_residual_stabilise=on MUST survive
			//    stale cnn_residual_stabilise=off, whereas an otherwise-identical section carrying only
			//    cnn_residual_stabilise=off migrates to sharp_residual_stabilise=off.
			{
				std::map<std::string, std::string> withExplicit;
				withExplicit["sharp_residual_stabilise"] = "on";
				withExplicit["cnn_residual_stabilise"] = "off";

				std::map<std::string, std::string> legacyOnly;
				legacyOnly["cnn_residual_stabilise"] = "off";

				Assert::IsTrue(Vpv::MigrateSharpResidualStabiliseSection(legacyOnly),
					L"legacy-only section must report a migration");
				Assert::AreEqual(std::string("off"), legacyOnly["sharp_residual_stabilise"],
					L"contrast baseline: legacy-only cnn_residual_stabilise=off migrates to off");

				Assert::IsTrue(Vpv::MigrateSharpResidualStabiliseSection(withExplicit),
					L"removing stale legacy key must report a change (true)");
				Assert::IsTrue(withExplicit.find("cnn_residual_stabilise") == withExplicit.end(),
					L"stale cnn_residual_stabilise must be removed");
				Assert::AreEqual(std::string("on"), withExplicit["sharp_residual_stabilise"],
					L"existing sharp_residual_stabilise=on must take precedence over stale cnn_residual_stabilise=off");
			}

			// 3. Already-clean section with no cnn_residual_stabilise is a no-op (returns false).
			{
				std::map<std::string, std::string> clean;
				clean["sharp_residual_stabilise"] = "on";
				Assert::IsFalse(Vpv::MigrateSharpResidualStabiliseSection(clean),
					L"already-clean section must return false (no-op)");
				Assert::AreEqual(std::string("on"), clean["sharp_residual_stabilise"]);
			}

			// 4. Case-insensitive legacy key handling.
			{
				std::map<std::string, std::string> mixed;
				mixed["CNN_Residual_Stabilise"] = "off";
				Assert::IsTrue(Vpv::MigrateSharpResidualStabiliseSection(mixed));
				Assert::IsTrue(mixed.find("CNN_Residual_Stabilise") == mixed.end());
				Assert::AreEqual(std::string("off"), mixed["sharp_residual_stabilise"]);
			}
		}

		TEST_METHOD(G12VpvDefaultDownscalerLanczosVsOriginalHermiteNegativeControl)
		{
			AdaptiveDownscale::Module module;
			AdaptiveDownscaleStage::Stage stage{ module };

			// Premise: upstream libplacebo default downscaler is Hermite.
			pl_render_params paramsVpv = pl_render_default_params;
			pl_render_params paramsOrig = pl_render_default_params;
			Assert::IsTrue(paramsVpv.downscaler == &pl_filter_hermite,
				L"premise: pl_render_default_params.downscaler must start as &pl_filter_hermite");

			AdaptiveDownscaleStage::Light light;
			light.sourceCrop = { 0.0f, 0.0f, 3840.0f, 2160.0f };
			light.targetCrop = { 0.0f, 0.0f, 1920.0f, 1080.0f };
			light.slotRuns = true;

			// VP-V pathway (downscaler == auto && quality != fast): switches to Lanczos.
			light.vpvDefaultDownscaler = true;
			stage.ApplyLight(paramsVpv, light);
			Assert::IsTrue(paramsVpv.downscaler == &pl_filter_lanczos,
				L"G12: VP-V default downscaler must select &pl_filter_lanczos");

			// Mandatory same-input negative control (Original pathway, vpvDefaultDownscaler == false):
			// keeps upstream &pl_filter_hermite untouched (never switched to Lanczos)!
			light.vpvDefaultDownscaler = false;
			stage.ApplyLight(paramsOrig, light);
			Assert::IsTrue(paramsOrig.downscaler == &pl_filter_hermite,
				L"negative control (Original pathway): vpvDefaultDownscaler=false must keep &pl_filter_hermite");
			Assert::IsFalse(paramsOrig.downscaler == &pl_filter_lanczos,
				L"negative control: Original pathway must never switch downscaler to Lanczos");

			// Explicit non-auto user choice (e.g. Mitchell with vpvDefaultDownscaler == false): preserved untouched.
			pl_render_params paramsCustom = pl_render_default_params;
			paramsCustom.downscaler = &pl_filter_mitchell;
			stage.ApplyLight(paramsCustom, light);
			Assert::IsTrue(paramsCustom.downscaler == &pl_filter_mitchell,
				L"explicit user downscaler choice must remain &pl_filter_mitchell");

			// Slot switch off (pipeline_slots_off: scaling.output): even with vpvDefaultDownscaler == true,
			// ApplyLight must respect slotRuns == false and keep upstream &pl_filter_hermite.
			pl_render_params paramsSlotOff = pl_render_default_params;
			light.slotRuns = false;
			light.vpvDefaultDownscaler = true;
			stage.ApplyLight(paramsSlotOff, light);
			Assert::IsTrue(paramsSlotOff.downscaler == &pl_filter_hermite,
				L"G12: slotRuns == false must leave downscaler as &pl_filter_hermite even when vpvDefaultDownscaler == true");
		}

		TEST_METHOD(B4EndToEndScalingChainRebaseRoute4kOutputNoDoubleUpscale)
		{
			// Simulate a 4K-delivered (3840x2160) stream re-based to 1920x1080, output target 3840x2160.
			Route4k::Config rCfg;
			rCfg.mode = Route4k::Mode::RebasedOnly;
			const Route4k::PlanResult plan = Route4k::Plan(1920, 1080, 3840, 2160, 3840, 2160, true, true, false, rCfg);
			Assert::IsTrue(plan.shouldRun && plan.geometry.dstW == 3840 && plan.geometry.dstH == 2160,
				L"Route4k(rebased_only) must plan 1920x1080 -> 3840x2160");

			AdaptiveDownscale::Settings settings;
			AdaptiveDownscaleStage::Host host;
			host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
			host.bitrate = [](double& q) { q = 1.0; return false; };
			host.nlsMapping = []() { return NlsMaskMapping::Mapping{}; };
			host.ssimInChain = []() { return false; };
			host.motion = []() { return AdaptiveDownscaleStage::MotionSource{}; };
			host.publishStatus = []() {};

			// 1. Routed chain (Rebase -> Route4k -> AdaptiveDownscaleStage): source crop is now 3840x2160,
			//    matching target 3840x2160 -> AdaptiveDownscaleStage MUST abstain (zero double-upscale!).
			{
				AdaptiveDownscale::Module module;
				AdaptiveDownscaleStage::Stage stage{ module };
				AdaptiveDownscaleStage::Frame f;
				f.adaptive = &settings;
				f.adaptiveUpscale = true;
				f.lowBitrateOff = true;
				f.sourceSequence = 1;
				f.nominalSourceRateHz = 24.0;
				f.sourceCrop = { 0.0f, 0.0f, static_cast<float>(plan.geometry.dstW), static_cast<float>(plan.geometry.dstH) };
				f.targetCrop = { 0.0f, 0.0f, 3840.0f, 2160.0f };
				f.rebaseFactor = 1.0;
				f.rebaseFactorX = 1.0;

				double weight = 1.0;
				bool grainWanted = false, noSsim = false;
				std::vector<const pl_hook*> hooks;
				stage.Register(f, host, { weight, grainWanted, noSsim }, hooks);
				Assert::IsFalse(stage.UpscaleThisFrame(),
					L"B4 chain: after Route4k brings 1080p to 4K, AdaptiveDownscaleStage must NOT upscale a second time");
				Assert::AreEqual(size_t(0), hooks.size(),
					L"B4 chain: zero upscale hooks registered downstream after Route4k");
			}

			// 2. Mandatory same-input negative control (unrouted 1920x1080 passed straight to AdaptiveDownscaleStage):
			//    UpscaleThisFrame() IS true and Measure+Blend hooks ARE registered!
			{
				AdaptiveDownscale::Module module;
				AdaptiveDownscaleStage::Stage unroutedStage{ module };
				AdaptiveDownscaleStage::Frame f;
				f.adaptive = &settings;
				f.adaptiveUpscale = true;
				f.lowBitrateOff = true;
				f.sourceSequence = 1;
				f.nominalSourceRateHz = 24.0;
				f.sourceCrop = { 0.0f, 0.0f, 1920.0f, 1080.0f };
				f.targetCrop = { 0.0f, 0.0f, 3840.0f, 2160.0f };
				f.rebaseFactor = 1.0;
				f.rebaseFactorX = 1.0;

				double weight = 1.0;
				bool grainWanted = false, noSsim = false;
				std::vector<const pl_hook*> hooks;
				unroutedStage.Register(f, host, { weight, grainWanted, noSsim }, hooks);
				Assert::IsTrue(unroutedStage.UpscaleThisFrame(),
					L"negative control: without Route4k, AdaptiveDownscaleStage DOES trigger upscale on 1920x1080 -> 3840x2160");
				Assert::AreEqual(size_t(2), hooks.size(),
					L"negative control: unrouted 1920x1080 registers Measure + Blend hooks");
			}
		}
	};
}
