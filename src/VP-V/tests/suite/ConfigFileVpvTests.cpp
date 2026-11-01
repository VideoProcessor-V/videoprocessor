// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// VP-V tests split out of Bill's ConfigFileTests.cpp by tools/m2/move_tests.py.
#include "pch.h"
#include "BackendInputConfig.h"

#include <ApplicationInterface.h>
#include <ApplicationShutdownPolicy.h>
#include <BuildIdentityPolicy.h>
#include <ModernOperatorLayout.h>
#include <ModernOperatorStatusPolicy.h>
#include <ConfigFile.h>
#include <ActiveProfileStatus.h>
#include <PPMCorrectionLoader.h>
#include <ColorOutputProfileMigration.h>
#include <ConfigurationLiveApply.h>
#include <ConfigurationApplyPolicy.h>
#include <DisplayTopologySession.h>
#include <EventActionLauncher.h>
#include <MainConfigSchema.h>
#include <AmbientConfig.h>
#include <AmbientSensor.h>
#include <vprenderer/StatsOsdSettings.h>
#include <QueueConfiguration.h>
#include <ConfigBoolSwitches.h>
#include <HdrInference.h>
#include <QueueLatency.h>
#include <MotionAhead.h>
#include <QueueProfileRestartPolicy.h>
#include <ProfileChangeOverlay.h>
#include <blackmagic_decklink/BlackMagicDeckLinkTranslate.h>
#include <guid.h>
#include <microsoft_directshow/DirectShowTranslations.h>
#include <microsoft_directshow/MadVRShaderLoader.h>
#include <microsoft_directshow/video_renderers/DirectShowVideoRenderers.h>
#include <RendererConfigView.h>
#include <RendererProfileConfig.h>
#include <RetiredSettings.h>
#include <ShaderConfigValidation.h>
#include <UnifiedProfileRuntime.h>
#include <VideoConversionOverride.h>
#include <vprenderer/DejudderPolicy.h>
#include <vprenderer/PulldownDetector.h>
#include <vprenderer/DownscaleLight.h>
#include <vprenderer/NlsMaskMapping.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>
#include "CppUnitTest.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <vector>
using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	struct CachedConfigTestFile
	{
		std::string path;
		CachedConfigTestFile()
		{
			char directory[MAX_PATH]{}, file[MAX_PATH]{};
			Assert::IsTrue(GetTempPathA(MAX_PATH, directory) != 0);
			Assert::IsTrue(GetTempFileNameA(directory, "vpc", 0, file) != 0);
			path = file;
		}
		~CachedConfigTestFile() { DeleteFileA(path.c_str()); }
		void Write(const char* value)
		{
			std::ofstream file(path, std::ios::trunc);
			file << "[general]\nvalue: " << value << "\n";
		}
	};

	TEST_CLASS(ConfigurationReadCacheTestsVpv)
	{
	public:

        TEST_METHOD(LookAheadQueueFramesFollowTheSelectedProfiles)
        {
            // Owner 2026-10-16: the optical-flow look-ahead is on or off, and on needs 4 frames queued
            // ahead - the snapshot says 5 when the selected profiles have it on (optical flow on,
            // vectors ahead on, a de-judder strength), 0 otherwise.
            auto framesFor = [](const char* text) {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n" << text;
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                UnifiedProfileRuntime::Runtime runtime;
                Assert::IsTrue(runtime.Initialize(config, {}, error), std::wstring(error.begin(), error.end()).c_str());
                const auto snapshot = runtime.GetSnapshot();
                Assert::IsTrue(snapshot != nullptr);
                return snapshot->lookAheadQueueFrames;
            };
            Assert::AreEqual<size_t>(5, framesFor("[vprenderer.Cinema]\ndejudder_strength: 0.25\n"), L"de-judder on, defaults: 4 ahead");
            Assert::AreEqual<size_t>(0, framesFor("[vprenderer.Cinema]\ndejudder_strength: 0.25\nmotion_ahead_frames: 0\n"), L"look-ahead off");
            Assert::AreEqual<size_t>(0, framesFor("[vprenderer.Cinema]\ndejudder_strength: 0.25\noptical_flow: off\n"), L"optical flow off");
            Assert::AreEqual<size_t>(0, framesFor("[vprenderer.Cinema]\nquality: high\n"), L"no de-judder: no look-ahead");
            Assert::AreEqual<size_t>(5, framesFor("[vprenderer.Cinema]\ndejudder_strength_even: 0.5\n"), L"even-cadence strength alone");
        }

        TEST_METHOD(LookAheadQueueFramesAreTimeBasedAndNeverForceTheQueue)
        {
            // r6 fixes 2+3 (2026-10-24): the look-ahead's frames follow its time (66.7 ms) at the
            // source rate - 2 ahead (3 queued) at 23.976p, 4 ahead (5) at 60p - and the queue target
            // follows the configured depth: 200 ms at 23.976p is 5 frames, not more.
            auto framesAt = [](double rate) {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n[vprenderer.Cinema]\ndejudder_strength: 0.25\n";
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                UnifiedProfileRuntime::Runtime runtime;
                auto lookup = [rate](const std::string& variable, std::string& value) {
                    if (variable != "cadence") return false;
                    std::ostringstream text;
                    text.imbue(std::locale::classic());
                    text.precision(17);
                    text << rate;
                    value = text.str();
                    return true;
                };
                Assert::IsTrue(runtime.Initialize(config, lookup, error), std::wstring(error.begin(), error.end()).c_str());
                const auto snapshot = runtime.GetSnapshot();
                Assert::IsTrue(snapshot != nullptr);
                return snapshot->lookAheadQueueFrames;
            };
            const double film = 24000.0 / 1001.0;
            Assert::AreEqual<size_t>(3, framesAt(film), L"23.976p: 2 ahead + the frame shown");
            Assert::AreEqual<size_t>(3, framesAt(25.0), L"25p");
            Assert::AreEqual<size_t>(5, framesAt(60.0), L"60p: 4 ahead");
            Assert::AreEqual<size_t>(9, framesAt(120.0), L"120p keeps the time: 8 ahead");
            // The queue target: the configured time, never raised by the look-ahead.
            const auto target = QueueLatency::ClampToCapacity(QueueLatency::ResolveTargetFrames(true, 200, false, 0, 0, 3, film), 16);
            Assert::AreEqual<size_t>(5, target.frames, L"200 ms at 23.976p: 5 frames (208.5 ms)");
            Assert::IsTrue(target.effectiveMs <= 210.0);
            const auto twoFrames = QueueLatency::ClampToCapacity(QueueLatency::ResolveTargetFrames(false, 0, true, 2, 0, 3, film), 16);
            Assert::AreEqual<size_t>(2, twoFrames.frames, L"target_frames 2 with the look-ahead on: 2 (it degrades, the queue is not forced)");
            // The look-ahead on a short queue: what is there, never waiting.
            const auto shallow = MotionAhead::Decide(MotionAhead::DepthAt(4, film), false, true, 1);
            Assert::AreEqual(1, shallow.depth);
            Assert::IsTrue(shallow.reason == MotionAhead::Reason::ShallowQueue);
        }

        TEST_METHOD(InterpolationKeysValidateAndWantTheLookAhead)
        {
            // docs/VP-interpolator-50-60-plan.md: the 50/60p interpolation keys (owner defaults
            // off / hold / off / legacy) are validated in the renderer input group, and an
            // interpolating profile wants the producer's look-ahead like the de-judder.
            std::string expected;
            for (const char* v : { "off", "auto", "always", "Auto" })
                Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "interpolation",
                    ConfigFile::NormalizeName(v), expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "interpolation", "on", expected));
            Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "interpolation_fallback", "hold", expected));
            Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "interpolation_fallback", "blend", expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "interpolation_fallback", "mix", expected));
            Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "interpolation_fill_gaps", "off", expected));
            Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "interpolation_fill_gaps", "on", expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "interpolation_fill_gaps", "2", expected));
            Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "source_class", "legacy", expected));
            Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "source_class", "measured", expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "source_class", "film", expected));
            auto framesFor = [](const char* text) {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n" << text;
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                UnifiedProfileRuntime::Runtime runtime;
                Assert::IsTrue(runtime.Initialize(config, {}, error), std::wstring(error.begin(), error.end()).c_str());
                const auto snapshot = runtime.GetSnapshot();
                Assert::IsTrue(snapshot != nullptr);
                return snapshot->lookAheadQueueFrames;
            };
            Assert::AreEqual<size_t>(5, framesFor("[vprenderer.Cinema]\ninterpolation: auto\n"), L"interpolation alone: 4 ahead");
            Assert::AreEqual<size_t>(0, framesFor("[vprenderer.Cinema]\ninterpolation: off\n"), L"off: no look-ahead");
            Assert::AreEqual<size_t>(0, framesFor("[vprenderer.Cinema]\ninterpolation: always\noptical_flow: off\n"),
                L"optical flow off: the matcher at render time");
        }

        TEST_METHOD(RenderAheadKeysValidate)
        {
            // docs/VP-render-ahead-design.md: render_ahead and render_ahead_queue_credit on|off,
            // dejudder_gate auto|period|refresh, render_ahead_utilisation 0.5..1.0.
            std::string expected;
            auto wide = [](const char* v) { const std::string s(v); return std::wstring(s.begin(), s.end()); };
            for (const char* key : { "render_ahead", "render_ahead_queue_credit" })
            {
                Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", key, "off", expected), wide(key).c_str());
                Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", key, "on", expected), wide(key).c_str());
                Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", key,
                    ConfigFile::NormalizeName("On"), expected), wide(key).c_str());
                Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", key, "auto", expected), wide(key).c_str());
                Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", key, "1", expected), wide(key).c_str());
                Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", key, "", expected), wide(key).c_str());
            }
            for (const char* v : { "auto", "period", "refresh" })
                Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "dejudder_gate", v, expected), wide(v).c_str());
            Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "dejudder_gate",
                ConfigFile::NormalizeName("Period"), expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "dejudder_gate", "on", expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "dejudder_gate", "frame", expected));
            for (const char* v : { "0.5", "0.85", "1", "1.0", "0.75" })
                Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "render_ahead_utilisation", v, expected),
                    wide(v).c_str());
            for (const char* v : { "0.49", "1.01", "0", "-0.85", "2", "abc", "" })
                Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "render_ahead_utilisation", v, expected),
                    wide(v).c_str());
        }

        TEST_METHOD(ActivePictureLookaheadAcceptsZeroToEightAndAbsence)
        {
            // Owner 2026-10-17: absent = automatic (follows the queue); an explicit 0-8 overrides.
            std::string expected;
            for (const char* v : { "0", "1", "4", "8" })
                Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("queue", "active_picture_lookahead_frames", v, expected));
            for (const char* v : { "-1", "9", "auto", "" })
                Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("queue", "active_picture_lookahead_frames", v, expected));
            RendererProfileConfig::Model model;
            CachedConfigTestFile absent;
            std::ofstream(absent.path) << "[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n";
            ConfigFile config;
            Assert::IsTrue(config.Load(absent.path));
            std::string error;
            Assert::IsTrue(RendererProfileConfig::Read(config, model, error), std::wstring(error.begin(), error.end()).c_str());
            CachedConfigTestFile zero;
            std::ofstream(zero.path) << "[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\nactive_picture_lookahead_frames: 0\n";
            ConfigFile config2;
            Assert::IsTrue(config2.Load(zero.path));
            Assert::IsTrue(RendererProfileConfig::Read(config2, model, error), std::wstring(error.begin(), error.end()).c_str());
        }

        TEST_METHOD(IntentRemainderKeysValidate)
        {
            // Intent interpreter remainder (2026-10-17): the 12 experimental keys (contract:
            // choices and numeric ranges), all default off.
            std::string expected;
            auto wide = [](const char* v) { const std::string s(v); return std::wstring(s.begin(), s.end()); };
            struct Choice { const char* key; std::vector<const char*> good; std::vector<const char*> bad; };
            const std::vector<Choice> choices = {
                { "intent_motion", { "off", "on" }, { "auto", "true", "1", "" } },
                { "intent_motion_policy", { "display", "ease_fast", "ease_slow", "visibility", "metadata" },
                    { "on", "off", "ease", "Display only", "" } },
                { "ambient_adapt", { "off", "on" }, { "auto", "yes", "" } },
                { "intent_black", { "off", "report", "led_protect" }, { "on", "protect", "led", "" } },
                { "intent_display", { "off", "report" }, { "on", "led_protect", "" } } };
            for (const auto& c : choices)
            {
                for (const char* v : c.good)
                    Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", c.key, v, expected), wide(c.key).c_str());
                for (const char* v : c.bad)
                    Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", c.key, v, expected), wide(c.key).c_str());
            }
            struct Number { const char* key; double lo, hi; };
            const Number numbers[] = {
                { "intent_motion_min", 0.0, 1.0 }, { "intent_motion_min_shot_s", 0.5, 30.0 },
                { "intent_ease_s", 0.0, 5.0 },
                { "ambient_adapt_strength", 0.0, 1.0 }, { "ambient_screen_reflect", 0.0, 1.0 },
                { "ambient_max_black_nits", 0.0, 10.0 }, { "ambient_tau_up_s", 1.0, 600.0 },
                { "ambient_tau_down_s", 1.0, 1800.0 } };
            for (const auto& n : numbers)
            {
                const std::string key = n.key;
                for (double v : { n.lo, (n.lo + n.hi) / 2.0, n.hi })
                    Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", key, std::to_string(v), expected),
                        wide(key.c_str()).c_str());
                for (double v : { n.lo - 0.01, n.hi + 0.01 })
                    Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", key, std::to_string(v), expected),
                        wide(key.c_str()).c_str());
                for (const char* v : { "abc", "", "on" })
                    Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", key, v, expected), wide(key.c_str()).c_str());
            }
            // Unknown intent-like keys stay rejected.
            for (const char* key : { "intent_motion_policies", "ambient_adapt_mode", "intent_black_mode", "intent_displays" })
                Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", key, "on", expected), wide(key).c_str());
        }

        TEST_METHOD(LowLatencyPathAndLegacyTargetAreNotInherited)
        {
            // Audit 2026-10-16: low_latency_path on the default queue profile must not reach the
            // others (Low_Latency kept its 1-frame queue on the regular path), and a variant's
            // legacy steady_reserve_frames beats an inherited target_frames.
            auto resolve = [](const char* text, const char* name) {
                CachedConfigTestFile file;
                std::ofstream(file.path) << text;
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                RendererProfileConfig::Model model;
                Assert::IsTrue(RendererProfileConfig::Read(config, model, error), std::wstring(error.begin(), error.end()).c_str());
                RendererProfileConfig::ResolvedQueue resolved;
                Assert::IsTrue(RendererProfileConfig::ResolveQueue(model, name, resolved, error), std::wstring(error.begin(), error.end()).c_str());
                return resolved;
            };
            const char* offOnNormal = "[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\nlow_latency_path: off\n"
                "[queue.Low_Latency]\nqueue_size: 1\ntarget_frames: 1\n[queue.Other]\nqueue_size: 8\n";
            Assert::IsTrue(resolve(offOnNormal, "low_latency").lowLatencyPath, L"Low_Latency, unset: on (not Normal's off)");
            Assert::IsFalse(resolve(offOnNormal, "normal").lowLatencyPath);
            const char* onOnNormal = "[queue.Normal]\nqueue_size: 4\nlow_latency_path: on\n[queue.Other]\nqueue_size: 8\n";
            Assert::IsFalse(resolve(onOnNormal, "other").lowLatencyPath, L"another name, unset: off (not Normal's on)");
            const char* legacy = "[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n[queue.Old]\nsteady_reserve_frames: 1\n";
            Assert::AreEqual<size_t>(1, resolve(legacy, "old").targetFrames, L"the variant's steady_reserve_frames");
        }

        TEST_METHOD(LowLatencyPathIsAQueueProperty)
        {
            // Owner 2026-10-16: low_latency_path on the queue profile; unset = the Low_Latency name.
            auto pathFor = [](const char* text) {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n" << text;
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                // Config's Apply validates against the main schema too (it rejected the key, 2026-10-16).
                Assert::IsTrue(MainConfigSchema::Validate(config, error), std::wstring(error.begin(), error.end()).c_str());
                UnifiedProfileRuntime::Runtime runtime;
                Assert::IsTrue(runtime.Initialize(config, {}, error), std::wstring(error.begin(), error.end()).c_str());
                return runtime.GetSnapshot()->queue.lowLatencyPath;
            };
            Assert::IsTrue(pathFor("[queue.Low_Latency]\nqueue_size: 2\n"), L"the Low_Latency name, unset: on");
            Assert::IsFalse(pathFor("[queue.Normal]\nqueue_size: 32\n"), L"another name, unset: off");
            Assert::IsTrue(pathFor("[queue.Game]\nqueue_size: 2\nlow_latency_path: on\nwhen: ${cadence} == 49-61\n"),
                L"any name with low_latency_path: on (and the 50/60 Hz rule validates)");
            Assert::IsFalse(pathFor("[queue.Low_Latency]\nqueue_size: 2\nlow_latency_path: off\n"), L"Low_Latency with off: the regular path");
        }

        TEST_METHOD(EdgeBlankingSurvivesAcceptedConfiguration)
        {
            CachedConfigTestFile file;
            std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n"
                "[vprenderer.viewport.cinema]\nanamorphic_scale: 4:3\n"
                "blank_left: 10\nblank_right: 15\n";
            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            std::string error;
            Assert::IsTrue(MainConfigSchema::Validate(config, error),
                std::wstring(error.begin(), error.end()).c_str());
            UnifiedProfileRuntime::Runtime runtime;
            Assert::IsTrue(runtime.Initialize(config, {}, error),
                std::wstring(error.begin(), error.end()).c_str());
            const auto snapshot = runtime.GetSnapshot();
            Assert::IsTrue(snapshot != nullptr);
            Assert::AreEqual(10, snapshot->viewport.blankLeft);
            Assert::AreEqual(15, snapshot->viewport.blankRight);
            Assert::AreEqual(0, snapshot->viewport.blankTop);
            Assert::AreEqual(0, snapshot->viewport.blankBottom);
        }

        TEST_METHOD(CustomGlslStandardShaderCarriesParamsAndCinemaOnlyToVPRenderer)
        {
            CachedConfigTestFile file;
            std::ofstream(file.path) << "[shader.standard]\ntype: multi\nlabel: Off\n"
                "[shader.standard.sigma_delta]\nshader_type: custom\nlabel: Temporal sigma-delta\n"
                "stage: post_resize\norder: 90\nglsl_file: Temporal sigma-delta.glsl\n"
                "param_noise_amplitude: 0.2\nparam_test_ramp: 1\ncinema_only: true\n"
                "[shader.standard.cfl]\nshader_type: custom\nstage: pre_resize\norder: 10\n"
                "glsl_file: CfL_Prediction.glsl\n";
            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            std::string error;
            Assert::IsTrue(ShaderConfigValidation::Validate(config, error),
                std::wstring(error.begin(), error.end()).c_str());
            std::vector<ConfiguredShaderRule> selection;
            Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(config,
                "@shader-profiles:standard.sigma_delta|standard.cfl",
                ShaderRendererBackend::LIBPLACEBO, selection, error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::AreEqual(size_t{ 2 }, selection.size(),
                L"Standard GLSL shaders must resolve without an NLS rule");
            const ConfiguredShaderRule& sigma = selection[0];
            Assert::IsFalse(sigma.nls || sigma.none);
            Assert::AreEqual(std::string("Temporal sigma-delta.glsl"), sigma.filename);
            Assert::AreEqual(90u, sigma.stageOrder);
            Assert::IsTrue(sigma.cinemaOnly);
            Assert::AreEqual(0.2, std::stod(sigma.parameters.at("noise_amplitude")), 1e-12);
            Assert::AreEqual(1.0, std::stod(sigma.parameters.at("test_ramp")), 1e-12);
            Assert::IsFalse(selection[1].cinemaOnly);
            Assert::IsTrue(selection[1].parameters.empty());

            // madVR has no hlsl_file for these entries, so it simply ignores them.
            std::vector<ConfiguredShaderRule> madvr;
            Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(config,
                "@shader-profiles:standard.sigma_delta|standard.cfl",
                ShaderRendererBackend::MADVR, madvr, error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::AreEqual(size_t{ 1 }, madvr.size());
            Assert::IsTrue(madvr.front().none);
        }

        // Pruning 2026-10-17 (RetiredSettings.h): retired keys are known, migrated values map to
        // their replacements, and every other key is untouched.
        TEST_METHOD(RetiredRendererSettingsMigrate)
        {
            for (const char* key : { "fix_scene_cooldown", "dejudder_picture_area", "dither_vectors_new_frame" })
                Assert::IsNotNull(RetiredSettings::FindKey(key), std::wstring(key, key + strlen(key)).c_str());
            for (const char* key : { "native_rgb_ingest", "capture_path", "optical_flow", "dejudder_warp_stage" })
                Assert::IsNull(RetiredSettings::FindKey(key), std::wstring(key, key + strlen(key)).c_str());
            Assert::AreEqual(std::string("auto"), RetiredSettings::Migrate("adaptive_faithful", "Polar"));
            Assert::AreEqual(std::string("ortho"), RetiredSettings::Migrate("adaptive_faithful", "ortho"));
            Assert::AreEqual(std::string("auto"), RetiredSettings::Migrate("adaptive_faithful", "auto"));
            Assert::AreEqual(std::string("whole"), RetiredSettings::Migrate("dejudder_even_rates", "30"));
            Assert::AreEqual(std::string("whole_and_1001"), RetiredSettings::Migrate("dejudder_even_rates", "30_and_29.97"));
            Assert::AreEqual(std::string("whole_and_1001"), RetiredSettings::Migrate("dejudder_even_rates", "whole_and_1001"));
            Assert::AreEqual(std::string("30"), RetiredSettings::Migrate("dejudder_search_range", "30"));
            const RetiredSettings::Value* value = RetiredSettings::FindValue("adaptive_faithful", "polar");
            Assert::IsNotNull(value);
            Assert::IsTrue(RetiredSettings::ValueNote("vprenderer.Cinema", *value).find("runs as auto") != std::string::npos);
        }

        // Pruning 2026-10-17: an old file holding every retired key and value, in the base
        // renderer section and in named profiles, loads with no error.
        TEST_METHOD(OldConfigurationWithRetiredSettingsLoads)
        {
            CachedConfigTestFile file;
            std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n"
                "[vprenderer]\nquality: high\nfix_scene_cooldown: off\ndejudder_picture_area: off\n"
                "dither_vectors_new_frame: off\nadaptive_faithful: polar\ndejudder_even_rates: 30\n"
                "[vprenderer.Cinema]\nfix_scene_cooldown: on\ndejudder_even_rates: 30_and_29.97\n"
                "adaptive_faithful: polar\n"
                "[vprenderer.input.film]\ndejudder_picture_area: on\ndither_vectors_new_frame: on\n";
            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            std::string error;
            Assert::IsTrue(MainConfigSchema::Validate(config, error), std::wstring(error.begin(), error.end()).c_str());
            RendererProfileConfig::Model model;
            Assert::IsTrue(RendererProfileConfig::Read(config, model, error), std::wstring(error.begin(), error.end()).c_str());
            UnifiedProfileRuntime::Runtime runtime;
            Assert::IsTrue(runtime.Initialize(config, {}, error), std::wstring(error.begin(), error.end()).c_str());
        }

        // Pruning 2026-10-17: older shipped configurations (VP_TEST_OLD_CONFIGS = a folder of
        // .cfg files, e.g. VideoProcessor.cfg from earlier releases) still load. Skipped when unset.
        TEST_METHOD(OlderShippedConfigurationsLoad)
        {
            char folder[MAX_PATH] = {};
            if (GetEnvironmentVariableA("VP_TEST_OLD_CONFIGS", folder, MAX_PATH) == 0)
            {
                Logger::WriteMessage("VP_TEST_OLD_CONFIGS not set; skipped");
                return;
            }
            WIN32_FIND_DATAA found{};
            const std::string pattern = std::string(folder) + "\\*.cfg";
            HANDLE search = FindFirstFileA(pattern.c_str(), &found);
            Assert::IsTrue(search != INVALID_HANDLE_VALUE, L"VP_TEST_OLD_CONFIGS has no .cfg files");
            int loaded = 0;
            do
            {
                const std::string path = std::string(folder) + "\\" + found.cFileName;
                std::ifstream in(path, std::ios::binary);
                const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                CachedConfigTestFile file;
                std::ofstream(file.path, std::ios::binary) << contents;
                ConfigFile config;
                const std::wstring name(path.begin(), path.end());
                Assert::IsTrue(config.Load(file.path, ConfigFile::ReadPolicy::Fresh), name.c_str());
                std::string error;
                const bool ok = MainConfigSchema::Validate(config, error);
                Assert::IsTrue(ok, (name + L": " + std::wstring(error.begin(), error.end())).c_str());
                RendererProfileConfig::Model model;
                Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
                    (name + L": " + std::wstring(error.begin(), error.end())).c_str());
                UnifiedProfileRuntime::Runtime runtime;
                Assert::IsTrue(runtime.Initialize(config, {}, error),
                    (name + L": " + std::wstring(error.begin(), error.end())).c_str());
                Logger::WriteMessage(("loaded " + path + "\n").c_str());
                ++loaded;
            } while (FindNextFileA(search, &found));
            FindClose(search);
            Assert::IsTrue(loaded > 0);
        }

        // Planner 2026-10-12: libplacebo's debanding is off for HDR in every
        // shipped configuration (its HDR path turns ~1 bright pixel in 10^4
        // black per frame); SDR keeps it available.
        TEST_METHOD(ShippedConfigurationsTurnDebandingOffForHdr)
        {
            std::string repository = __FILE__;
            for (int level = 0; level < 3; ++level)
                repository.resize(repository.find_last_of("\\/"));
            for (const char* relative : { "\\VideoProcessor.cfg", "\\docs\\cinema\\VideoProcessor-test.cfg" })
            {
                std::ifstream shipped(repository + relative, std::ios::binary);
                const std::string contents((std::istreambuf_iterator<char>(shipped)), std::istreambuf_iterator<char>());
                Assert::IsFalse(contents.empty(), (std::wstring(L"not found: ") + std::wstring(relative, relative + strlen(relative))).c_str());
                CachedConfigTestFile file;
                std::ofstream(file.path, std::ios::binary) << contents;
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path, ConfigFile::ReadPolicy::Fresh));
                std::string error;
                RendererProfileConfig::Model model;
                Assert::IsTrue(RendererProfileConfig::Read(config, model, error), std::wstring(error.begin(), error.end()).c_str());
                auto debandFor = [&](const char* eotf)
                {
                    std::vector<RendererProfileConfig::AutomaticSelection> automatic;
                    std::string selectError;
                    Assert::IsTrue(RendererProfileConfig::SelectAutomatic(model,
                        [eotf](const std::string& name, std::string& value)
                        {
                            if (name != "eotf") return false;
                            value = eotf;
                            return true;
                        }, automatic, selectError));
                    // Merge with v1.4.02-beta (VP-0215): Scaling owns debanding; the migration moves a
                    // Rendering profile's legacy `deband` to deband_strength in the Scaling profile that
                    // carries the same `when`. A Rendering profile that kept `deband` still counts.
                    std::string found = "none";
                    for (const auto& selection : automatic)
                        if (selection.group == "scaling" || selection.group == "display")
                        {
                            const std::string key = selection.group == "scaling" ? "deband_strength" : "deband";
                            const auto profile = model.profiles.find(selection.group + "." + selection.profiles.front());
                            Assert::IsTrue(profile != model.profiles.end());
                            const auto deband = profile->second.settings.find(key);
                            if (deband != profile->second.settings.end())
                                return deband->second;
                            found = "auto";
                        }
                    return found;
                };
                const std::wstring which(relative, relative + strlen(relative));
                Assert::AreEqual(std::string("off"), debandFor("pq"), (which + L": PQ").c_str());
                Assert::AreEqual(std::string("off"), debandFor("hlg"), (which + L": HLG").c_str());
                Assert::AreNotEqual(std::string("off"), debandFor("sdr"), (which + L": SDR keeps debanding").c_str());
            }
        }

        TEST_METHOD(CinemaTestConfigurationStartsWithShadersDisabledAndEnablesThemOneAtATime)
        {
            std::string repository = __FILE__;
            for (int level = 0; level < 3; ++level)
                repository.resize(repository.find_last_of("\\/"));
            std::ifstream shipped(repository + "\\docs\\cinema\\VideoProcessor-test.cfg",
                std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(shipped)),
                std::istreambuf_iterator<char>());
            Assert::IsFalse(contents.empty(), L"docs/cinema/VideoProcessor-test.cfg not found");
            // A temporary copy keeps the selection state file out of the repository.
            CachedConfigTestFile file;
            std::ofstream(file.path, std::ios::binary) << contents;

            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            std::string error;
            Assert::IsTrue(MainConfigSchema::Validate(config, error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::IsTrue(ShaderConfigValidation::Validate(config, error),
                std::wstring(error.begin(), error.end()).c_str());
            UnifiedProfileRuntime::Runtime runtime;
            Assert::IsTrue(runtime.Initialize(config, {}, error),
                std::wstring(error.begin(), error.end()).c_str());
            const auto snapshot = runtime.GetSnapshot();
            Assert::IsTrue(snapshot != nullptr);
            Assert::AreEqual(std::string("cinema"), snapshot->viewport.profile);
            Assert::AreEqual(10, snapshot->viewport.blankLeft);
            Assert::AreEqual(15, snapshot->viewport.blankRight);
            Assert::AreEqual(std::string("base"), RendererProfileConfig::FormatSelection(
                snapshot->effectiveSelections.at("standard_shaders")),
                L"All custom shaders must start disabled");

            for (const char* shader : { "cfl_chroma", "ssim_downscaler", "fsrcnnx", "artcnn",
                "ssim_superres", "sigma_delta" })
            {
                UnifiedProfileRuntime::SelectionResult selected;
                Assert::IsTrue(runtime.SelectProfile("standard_shaders", shader, true,
                    {}, selected, error), std::wstring(error.begin(), error.end()).c_str());
                Assert::AreEqual(std::string(shader), RendererProfileConfig::FormatSelection(
                    selected.snapshot->effectiveSelections.at("standard_shaders")));
                std::vector<ConfiguredShaderRule> rules;
                Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(config,
                    std::string("@shader-profiles:standard.") + shader,
                    ShaderRendererBackend::LIBPLACEBO, rules, error),
                    std::wstring(error.begin(), error.end()).c_str());
                Assert::AreEqual(size_t{ 1 }, rules.size());
                Assert::IsFalse(rules.front().none || rules.front().nls);
                Assert::IsTrue(runtime.SelectProfile("standard_shaders", shader, false,
                    {}, selected, error), std::wstring(error.begin(), error.end()).c_str());
            }

            std::vector<ConfiguredShaderRule> sigma;
            Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(config,
                "@shader-profiles:standard.sigma_delta",
                ShaderRendererBackend::LIBPLACEBO, sigma, error));
            const auto& parameters = sigma.front().parameters;
            Assert::AreEqual(size_t{ 6 }, parameters.size());
            Assert::AreEqual(0.15, std::stod(parameters.at("noise_amplitude")), 1e-12);
            Assert::AreEqual(10.0, std::stod(parameters.at("swapchain_bits")), 1e-12);
            Assert::AreEqual(0.0, std::stod(parameters.at("levels_full")), 1e-12);
            Assert::AreEqual(0.0, std::stod(parameters.at("test_ramp")), 1e-12);
        }

        TEST_METHOD(CinemaTestConfigurationPicksLowLatencyForHighFrameRates)
        {
            std::string repository = __FILE__;
            for (int level = 0; level < 3; ++level)
                repository.resize(repository.find_last_of("\\/"));
            std::ifstream shipped(repository + "\\docs\\cinema\\VideoProcessor-test.cfg",
                std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(shipped)),
                std::istreambuf_iterator<char>());
            Assert::IsFalse(contents.empty());
            CachedConfigTestFile file;
            std::ofstream(file.path, std::ios::binary) << contents;
            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            // As VP reports them (StateVariables): source_rate is the rate
            // rounded to the nearest whole number (audit 2026-10-16: was
            // floored; the shipped rule uses ${cadence}, so the expected
            // selections are unchanged), cadence the exact rate.
            auto source = [](const char* rate)
            {
                const double exact = std::stod(rate);
                return [exact](const std::string& variable, std::string& value)
                {
                    if (variable == "source_rate")
                        value = DisplayRuleExpression::NominalSourceRateText(exact);
                    else if (variable == "cadence")
                    {
                        std::ostringstream text;
                        text.imbue(std::locale::classic());
                        text.precision(17);
                        text << exact;
                        value = text.str();
                    }
                    else
                        return false;
                    return true;
                };
            };
            UnifiedProfileRuntime::Runtime runtime;
            std::string error;
            // Audit round 2: 59.94 Hz (cadence 59.94; source_rate now 60) must select the game queue.
            Assert::IsTrue(runtime.Initialize(config, source("59.94005994005994"), error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::AreEqual(std::string("low_latency"), RendererProfileConfig::FormatSelection(
                runtime.GetSnapshot()->effectiveSelections.at("queue")),
                L"A 60p source selects the game queue");
            UnifiedProfileRuntime::RefreshResult film;
            Assert::IsTrue(runtime.Refresh(source("23.976"), film, error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::AreEqual(std::string("normal"), RendererProfileConfig::FormatSelection(
                film.snapshot->effectiveSelections.at("queue")),
                L"Film goes back to the cinema queue");
            Assert::IsTrue(film.snapshot->queue.queueSize > 1);
            // Look-ahead follows the queue: Normal looks ahead, Low_Latency (a
            // one-frame queue) does not, so the LED drive works reactively there.
            Assert::IsTrue(film.snapshot->queue.hasActivePictureLookaheadFrames);
            Assert::AreEqual<size_t>(8, film.snapshot->queue.activePictureLookaheadFrames);
            UnifiedProfileRuntime::RefreshResult game;
            Assert::IsTrue(runtime.Refresh(source("59.94"), game, error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::IsTrue(game.snapshot->queue.hasActivePictureLookaheadFrames,
                L"Low_Latency must not inherit Normal's look-ahead");
            Assert::AreEqual<size_t>(0, game.snapshot->queue.activePictureLookaheadFrames);
            // Exact 60 also switches; 50p stays in the cinema queue.
            UnifiedProfileRuntime::RefreshResult other;
            Assert::IsTrue(runtime.Refresh(source("50"), other, error));
            Assert::AreEqual(std::string("normal"), RendererProfileConfig::FormatSelection(
                other.snapshot->effectiveSelections.at("queue")), L"50p is not a game rate");
            Assert::IsTrue(runtime.Refresh(source("60"), other, error));
            Assert::AreEqual(std::string("low_latency"), RendererProfileConfig::FormatSelection(
                other.snapshot->effectiveSelections.at("queue")), L"60.000 selects the game queue");

            // Stage 5: Normal's target is time (owner: 200 ms); film gets 5
            // frames, and Low_Latency's own target_frames beats the inherited time
            // at every source rate.
            Assert::IsTrue(film.snapshot->queue.hasTargetLatencyMs);
            Assert::AreEqual(200, film.snapshot->queue.targetLatencyMs);
            const auto filmTarget = QueueLatency::ResolveTargetFrames(
                film.snapshot->queue.hasTargetLatencyMs, film.snapshot->queue.targetLatencyMs,
                film.snapshot->queue.hasTargetFrames, film.snapshot->queue.targetFrames,
                200, 5, 24000.0 / 1001.0);
            Assert::IsTrue(filmTarget.fromLatency);
            Assert::AreEqual<size_t>(5, filmTarget.frames);
            Assert::IsFalse(game.snapshot->queue.hasTargetLatencyMs);
            for (const double rate : { 24000.0 / 1001.0, 25.0, 50.0, 60000.0 / 1001.0, 120.0 })
            {
                const auto gameTarget = QueueLatency::ResolveTargetFrames(
                    game.snapshot->queue.hasTargetLatencyMs, game.snapshot->queue.targetLatencyMs,
                    game.snapshot->queue.hasTargetFrames, game.snapshot->queue.targetFrames,
                    200, 5, rate);
                Assert::IsFalse(gameTarget.fromLatency);
                Assert::AreEqual<size_t>(1, gameTarget.frames, L"Low_Latency keeps its one frame");
            }
        }

        // Delta 6 (settings first): a key's profile choice is a session
        // override until the rules select a different profile for the source;
        // nothing is persisted unless persist_profile_selection is true.
        TEST_METHOD(KeyProfileChoiceLastsUntilTheRulesChange)
        {
            std::string repository = __FILE__;
            for (int level = 0; level < 3; ++level)
                repository.resize(repository.find_last_of("\\/"));
            std::ifstream shipped(repository + "\\docs\\cinema\\VideoProcessor-test.cfg",
                std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(shipped)),
                std::istreambuf_iterator<char>());
            Assert::IsFalse(contents.empty());
            Assert::IsTrue(contents.find("persist_profile_selection") == std::string::npos,
                L"The cinema config must rely on the default");
            CachedConfigTestFile file;
            std::ofstream(file.path, std::ios::binary) << contents;
            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            auto source = [](const char* rate)
            {
                const double exact = std::stod(rate);
                return [exact](const std::string& variable, std::string& value)
                {
                    std::ostringstream text;
                    text.imbue(std::locale::classic());
                    text.precision(17);
                    if (variable == "source_rate") // audit 2026-10-16: rounded, as VP reports it
                        text << DisplayRuleExpression::NominalSourceRateText(exact);
                    else if (variable == "cadence")
                        text << exact;
                    else
                        return false;
                    value = text.str();
                    return true;
                };
            };
            auto queue = [](const std::shared_ptr<const UnifiedProfileRuntime::Snapshot>& snapshot)
            {
                return RendererProfileConfig::FormatSelection(snapshot->effectiveSelections.at("queue"));
            };
            UnifiedProfileRuntime::Runtime runtime;
            std::string error;
            Assert::IsTrue(runtime.Initialize(config, source("23.976"), error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::IsTrue(runtime.StatePath().empty(), L"No state file without persist_profile_selection: true");
            UnifiedProfileRuntime::RefreshResult refreshed;
            Assert::IsTrue(runtime.Refresh(source("23.976"), refreshed, error));
            Assert::AreEqual(std::string("normal"), queue(runtime.GetSnapshot()));

            UnifiedProfileRuntime::SelectionResult selected;
            Assert::IsTrue(runtime.SelectKey("Shift+L", source("23.976"), selected, error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::AreEqual(std::string("low_latency"), queue(runtime.GetSnapshot()), L"The key selects Low_Latency");
            Assert::AreEqual(size_t{ 1 }, runtime.SessionOverrideGroups().size());

            // The same source again: the key's choice stays.
            Assert::IsTrue(runtime.Refresh(source("23.976"), refreshed, error));
            Assert::AreEqual(std::string("low_latency"), queue(runtime.GetSnapshot()), L"Same source: the key's choice stays");
            Assert::AreEqual(size_t{ 1 }, runtime.SessionOverrideGroups().size());

            // A 60p source: the rules pick another profile, the override ends.
            Assert::IsTrue(runtime.Refresh(source("59.94"), refreshed, error));
            Assert::IsTrue(runtime.SessionOverrideGroups().empty(), L"A rules change releases the key override");
            // Back to film: the rules' choice, not the key's.
            Assert::IsTrue(runtime.Refresh(source("23.976"), refreshed, error));
            Assert::AreEqual(std::string("normal"), queue(runtime.GetSnapshot()),
                L"The released key choice must not come back (not persisted)");

            // persist_profile_selection: true is the earlier behaviour: a live
            // key override stays in force across a rules change (audit delta6).
            std::string persisted = contents;
            const size_t general = persisted.find("[general]");
            Assert::IsTrue(general != std::string::npos);
            persisted.insert(persisted.find('\n', general) + 1, "persist_profile_selection: true\n");
            CachedConfigTestFile persistedFile;
            std::ofstream(persistedFile.path, std::ios::binary) << persisted;
            ConfigFile persistedConfig;
            Assert::IsTrue(persistedConfig.Load(persistedFile.path));
            UnifiedProfileRuntime::Runtime keeping;
            Assert::IsTrue(keeping.Initialize(persistedConfig, source("23.976"), error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::IsTrue(keeping.Refresh(source("23.976"), refreshed, error));
            Assert::IsTrue(keeping.SelectKey("Shift+L", source("23.976"), selected, error));
            Assert::IsTrue(keeping.Refresh(source("59.94"), refreshed, error));
            Assert::IsTrue(keeping.Refresh(source("23.976"), refreshed, error));
            Assert::AreEqual(std::string("low_latency"), queue(keeping.GetSnapshot()),
                L"With persistence on, the key override stays across a rules change");
            Assert::AreEqual(size_t{ 1 }, keeping.SessionOverrideGroups().size());
            DeleteFileA(keeping.StatePath().c_str());
        }

        TEST_METHOD(QueueTargetLatencyBecomesWholeSourceFrames)
        {
            using QueueLatency::FramesForLatency;
            const double film = 24000.0 / 1001.0;
            Assert::AreEqual<size_t>(3, FramesForLatency(120, film));
            Assert::AreEqual<size_t>(3, FramesForLatency(120, 25.0));
            Assert::AreEqual<size_t>(4, FramesForLatency(120, 30000.0 / 1001.0));
            Assert::AreEqual<size_t>(6, FramesForLatency(120, 50.0));
            Assert::AreEqual<size_t>(8, FramesForLatency(120, 60000.0 / 1001.0));
            Assert::AreEqual<size_t>(5, FramesForLatency(200, film));
            // Exactly whole frames do not round up from floating-point noise.
            Assert::AreEqual<size_t>(3, FramesForLatency(120, 25.0));
            Assert::AreEqual<size_t>(6, FramesForLatency(100, 60.0));
            // Bounds: at least one frame, never more than target_frames allows.
            Assert::AreEqual<size_t>(1, FramesForLatency(1, film));
            Assert::AreEqual<size_t>(QueueLatency::MaxTargetFrames, FramesForLatency(1000, 60.0));
            Assert::AreEqual<size_t>(1, FramesForLatency(120, 0.0));
            Assert::AreEqual(125.125, QueueLatency::EffectiveLatencyMs(3, film), 1e-9);

            using QueueLatency::ResolveTargetFrames;
            // r6 fix 2 (2026-10-24): no forced look-ahead minimum - the target is the configured
            // depth (time or frames); the look-ahead uses what is queued.
            {
                const auto shortTime = ResolveTargetFrames(true, 80, false, 0, 0, 3, film);
                Assert::AreEqual<size_t>(2, shortTime.frames, L"80 ms at 23.976: 2 frames, not raised to 5");
                Assert::AreEqual(QueueLatency::EffectiveLatencyMs(2, film), shortTime.effectiveMs, 1e-9);
                const auto frames = ResolveTargetFrames(false, 0, true, 2, 0, 3, film);
                Assert::AreEqual<size_t>(2, frames.frames, L"an explicit frame count is the operator's choice");
                const auto framesAhead = ResolveTargetFrames(false, 0, true, 5, 0, 3, film);
                Assert::AreEqual<size_t>(5, framesAhead.frames);
                // the target never exceeds the queue capacity.
                bool clamped = false;
                const auto smallQueue = QueueLatency::ClampToCapacity(framesAhead, 3, &clamped);
                Assert::IsTrue(clamped, L"queue_size 3 < 5: clamped");
                Assert::AreEqual<size_t>(3, smallQueue.frames);
                Assert::AreEqual(QueueLatency::EffectiveLatencyMs(3, film), smallQueue.effectiveMs, 1e-9);
                const auto bigQueue = QueueLatency::ClampToCapacity(framesAhead, 8, &clamped);
                Assert::IsFalse(clamped, L"a queue that holds the target is left alone");
                Assert::AreEqual<size_t>(5, bigQueue.frames);
                const auto unknownQueue = QueueLatency::ClampToCapacity(framesAhead, 0, &clamped);
                Assert::IsFalse(clamped, L"capacity 0 (unknown) does not clamp");
                Assert::AreEqual<size_t>(5, unknownQueue.frames);
            }
            // Own time target wins over own frames and the default's values.
            auto decision = ResolveTargetFrames(true, 200, true, 2, 120, 3, film);
            Assert::IsTrue(decision.fromLatency);
            Assert::AreEqual<size_t>(5, decision.frames);
            Assert::AreEqual(200, decision.latencyMs);
            // Own frames beat an inherited time target.
            decision = ResolveTargetFrames(false, 0, true, 1, 120, 3, film);
            Assert::IsFalse(decision.fromLatency);
            Assert::AreEqual<size_t>(1, decision.frames);
            // Inherited time target when the profile says nothing.
            decision = ResolveTargetFrames(false, 0, false, 0, 120, 3, 50.0);
            Assert::IsTrue(decision.fromLatency);
            Assert::AreEqual<size_t>(6, decision.frames);
            // target_latency_ms: 0 switches an inherited time target off.
            decision = ResolveTargetFrames(true, 0, false, 0, 120, 3, 50.0);
            Assert::IsFalse(decision.fromLatency);
            Assert::AreEqual<size_t>(3, decision.frames);
            // No time target anywhere: exactly the old frame count.
            decision = ResolveTargetFrames(false, 0, false, 0, 0, 4, 50.0);
            Assert::IsFalse(decision.fromLatency);
            Assert::AreEqual<size_t>(4, decision.frames);
            // Rate not known yet: the frame count it would otherwise use.
            decision = ResolveTargetFrames(true, 200, true, 2, 120, 3, 0.0);
            Assert::IsFalse(decision.fromLatency);
            Assert::AreEqual<size_t>(2, decision.frames);
            decision = ResolveTargetFrames(false, 0, false, 0, 120, 3, 0.0);
            Assert::AreEqual<size_t>(3, decision.frames);
        }

        TEST_METHOD(QueueTargetLatencyIsValidatedAndResolvedPerProfile)
        {
            CachedConfigTestFile file;
            {
                std::ofstream out(file.path, std::ios::out | std::ios::trunc);
                out << "[queue.normal]\nqueue_size: 32\ntarget_frames: 3\ntarget_latency_ms: 120\n"
                    "[queue.low]\nwhen: ${source_rate} > 59\nqueue_size: 1\ntarget_frames: 1\n"
                    "[queue.hdr]\nwhen: ${eotf} == \"HDR\"\nqueue_size: 8\n";
            }
            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            std::string error;
            Assert::IsTrue(MainConfigSchema::Validate(config, error),
                std::wstring(error.begin(), error.end()).c_str());
            RendererProfileConfig::Model model;
            Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
                std::wstring(error.begin(), error.end()).c_str());
            RendererProfileConfig::ResolvedQueue resolved;
            Assert::IsTrue(RendererProfileConfig::ResolveQueue(model, "normal", resolved, error));
            Assert::IsTrue(resolved.hasTargetLatencyMs);
            Assert::AreEqual(120, resolved.targetLatencyMs);
            // Its own target_frames switches the inherited time target off...
            Assert::IsTrue(RendererProfileConfig::ResolveQueue(model, "low", resolved, error));
            Assert::IsFalse(resolved.hasTargetLatencyMs);
            Assert::AreEqual<size_t>(1, resolved.targetFrames);
            // ...a profile that says nothing about its target inherits it.
            Assert::IsTrue(RendererProfileConfig::ResolveQueue(model, "hdr", resolved, error));
            Assert::IsTrue(resolved.hasTargetLatencyMs);
            Assert::AreEqual(120, resolved.targetLatencyMs);

            for (const char* bad : { "1001", "-1", "fast" })
            {
                {
                    std::ofstream out(file.path, std::ios::out | std::ios::trunc);
                    out << "[queue.normal]\ntarget_latency_ms: " << bad << "\n";
                }
                Assert::IsTrue(config.Load(file.path));
                error.clear();
                Assert::IsFalse(MainConfigSchema::Validate(config, error));
                Assert::IsTrue(error.find("target_latency_ms") != std::string::npos);
            }
        }

        TEST_METHOD(CinemaTestConfigurationSwitchesBlackBarZoomFromTheConfigApp)
        {
            std::string repository = __FILE__;
            for (int level = 0; level < 3; ++level)
                repository.resize(repository.find_last_of("\\/"));
            std::ifstream shipped(repository + "\\docs\\cinema\\VideoProcessor-test.cfg",
                std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(shipped)),
                std::istreambuf_iterator<char>());
            Assert::IsFalse(contents.empty());
            Assert::IsTrue(contents.find("cycle_shortcut") == std::string::npos,
                L"Zoom is switched from the Config app, not a key");
            CachedConfigTestFile file;
            std::ofstream(file.path, std::ios::binary) << contents;
            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            const auto noSource = [](const std::string&, std::string&) { return false; };
            UnifiedProfileRuntime::Runtime runtime;
            std::string error;
            Assert::IsTrue(runtime.Initialize(config, noSource, error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::IsFalse(runtime.GetSnapshot()->viewport.automaticCrop, L"VP starts with the zoom off");

            // What the Config app's "Use in VP" button sends (RPC SelectProfile).
            UnifiedProfileRuntime::SelectionResult selected;
            Assert::IsTrue(runtime.SelectProfile("zoom", "black_bars", true, noSource, selected, error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::AreEqual("black_bars", selected.snapshot->viewport.zoomProfile.c_str());
            Assert::IsTrue(selected.snapshot->viewport.automaticCrop);
            Assert::IsFalse(selected.snapshot->viewport.cropNarrowerContentToFillScreen);
            Assert::AreEqual("cinema", selected.snapshot->viewport.profile.c_str(),
                L"The zoom leaves the blanked screen profile in place");

            Assert::IsTrue(runtime.SelectProfile("zoom", "fill", true, noSource, selected, error));
            Assert::AreEqual("fill", selected.snapshot->viewport.zoomProfile.c_str());
            Assert::IsTrue(selected.snapshot->viewport.automaticCrop);
            Assert::IsTrue(selected.snapshot->viewport.cropNarrowerContentToFillScreen);

            Assert::IsTrue(runtime.SelectProfile("zoom", "off", true, noSource, selected, error));
            Assert::AreEqual("off", selected.snapshot->viewport.zoomProfile.c_str());
            Assert::IsFalse(selected.snapshot->viewport.automaticCrop);
        }

        TEST_METHOD(CustomGlslStandardShaderRejectsNonNumericParams)
        {
            for (const char* setting : { "param_noise_amplitude: lots\n", "cinema_only: sometimes\n" })
            {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[shader.standard]\ntype: multi\n"
                    "[shader.standard.sigma_delta]\nshader_type: custom\n"
                    "glsl_file: Temporal sigma-delta.glsl\n" << setting;
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                std::vector<ConfiguredShaderRule> selection;
                Assert::IsFalse(MadVRShaderLoader::ResolveConfiguredRuleSelection(config,
                    "@shader-profiles:standard.sigma_delta",
                    ShaderRendererBackend::LIBPLACEBO, selection, error));
            }
        }

        TEST_METHOD(SaturationIsAnInputProfileSettingFromMinus100To100)
        {
            for (const char* value : { "+5", "0", "-100", "100" })
            {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n"
                    "[vprenderer.input.cinema]\nsaturation: " << value << "\n";
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                UnifiedProfileRuntime::Runtime runtime;
                Assert::IsTrue(MainConfigSchema::Validate(config, error) &&
                    runtime.Initialize(config, {}, error),
                    std::wstring(error.begin(), error.end()).c_str());
            }
            for (const char* value : { "101", "-101", "5%", "high" })
            {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n"
                    "[vprenderer.input.cinema]\nsaturation: " << value << "\n";
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                UnifiedProfileRuntime::Runtime runtime;
                Assert::IsFalse(MainConfigSchema::Validate(config, error) &&
                    runtime.Initialize(config, {}, error));
                Assert::IsTrue(error.find("saturation") != std::string::npos,
                    std::wstring(error.begin(), error.end()).c_str());
            }
        }

        // Owner 2026-10-13: chroma_upscaler is a scaling-profile choice.
        TEST_METHOD(ChromaUpscalerIsAScalingChoice)
        {
            struct Case { const char* value; bool valid; };
            for (const Case c : { Case{ "auto", true }, Case{ "upscaler", true }, Case{ "lanczos", true }, Case{ "catmull_rom", true },
                Case{ "ewa_lanczossharp", true }, Case{ "bilinear", true }, Case{ "nearest", false }, Case{ "polar", false } })
            {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n"
                    "[vprenderer.scaling.cinema]\nchroma_upscaler: " << c.value << "\n";
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                UnifiedProfileRuntime::Runtime runtime;
                const bool ok = MainConfigSchema::Validate(config, error) && runtime.Initialize(config, {}, error);
                Assert::AreEqual(c.valid, ok, std::wstring(error.begin(), error.end()).c_str());
            }
        }

        // Stage 8-1: downscale_light is a scaling-profile choice, and gamma
        // light applies to downscaling frames only.
        TEST_METHOD(DownscaleLightIsAScalingChoiceForDownscalingFrames)
        {
            struct Case { const char* value; bool valid; };
            // Stage 8b's upscale_light is the same kind of scaling choice.
            for (const char* key : { "downscale_light", "upscale_light" })
            for (const Case c : { Case{ "linear", true }, Case{ "gamma", true }, Case{ "auto", false } })
            {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n"
                    "[vprenderer.scaling.cinema]\n" << key << ": " << c.value << "\n";
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                UnifiedProfileRuntime::Runtime runtime;
                const bool ok = MainConfigSchema::Validate(config, error) && runtime.Initialize(config, {}, error);
                Assert::AreEqual(c.valid, ok, std::wstring(error.begin(), error.end()).c_str());
            }
            const pl_rect2df uhd = { 0.0f, 0.0f, 3840.0f, 2160.0f };
            const pl_rect2df hd = { 0.0f, 0.0f, 1920.0f, 1080.0f };
            const pl_rect2df scope = { 0.0f, 200.0f, 2560.0f, 1400.0f };
            const pl_rect2df full = { 0.0f, 0.0f, 2560.0f, 1600.0f };
            Assert::IsTrue(DownscaleLight::UseGammaLight("gamma", uhd, full));
            Assert::IsTrue(DownscaleLight::UseGammaLight("gamma", pl_rect2df{ 0.0f, 0.0f, 2560.0f, 2160.0f }, full), L"One axis downscaled counts");
            Assert::IsFalse(DownscaleLight::UseGammaLight("gamma", hd, scope), L"1080p into a scope rectangle is an upscale");
            Assert::IsFalse(DownscaleLight::UseGammaLight("gamma", hd, full), L"Upscaling keeps linear light");
            Assert::IsFalse(DownscaleLight::UseGammaLight("linear", uhd, full));
            Assert::IsFalse(DownscaleLight::UseGammaLight("gamma", full, full), L"1:1 is not a downscale");
        }

        TEST_METHOD(DejudderSettingsAreInputProfileNumbersWithLimits)
        {
            struct Case { const char* key; const char* value; bool valid; };
            for (const Case& c : {
                Case{ "dejudder_strength", "0", true }, Case{ "dejudder_strength", "0.25", true },
                Case{ "dejudder_strength", "1", true }, Case{ "dejudder_strength", "1.01", false },
                Case{ "dejudder_strength", "-0.1", false }, Case{ "dejudder_strength", "gentle", false },
                Case{ "dejudder_match_tolerance", "3", true }, Case{ "dejudder_match_tolerance", "0.5", true },
                Case{ "dejudder_match_tolerance", "16", true }, Case{ "dejudder_match_tolerance", "0.4", false },
                Case{ "dejudder_match_tolerance", "17", false },
                Case{ "dejudder_frame_trust", "0.25", true }, Case{ "dejudder_frame_trust", "0", true },
                Case{ "dejudder_frame_trust", "1.5", false },
                Case{ "occlusion_mask", "on", true }, Case{ "occlusion_mask", "off", true },
                Case{ "occlusion_mask", "yes", false },
                Case{ "dejudder_warp_stage", "rendered", true }, Case{ "dejudder_warp_stage", "source", true },
                Case{ "dejudder_warp_stage", "cached", false },
                Case{ "dejudder_global_prior", "on", true }, Case{ "dejudder_global_prior", "yes", false },
                Case{ "dejudder_vector_smooth", "off", true }, Case{ "dejudder_vector_smooth", "1", false },
                Case{ "dejudder_occlusion_fill", "off", true }, Case{ "dejudder_occlusion_fill", "auto", false },
                Case{ "dejudder_motion_scale_lo", "0.25", true }, Case{ "dejudder_motion_scale_lo", "17", false },
                Case{ "dejudder_motion_scale_hi", "0", true }, Case{ "dejudder_motion_scale_hi", "1.5", true },
                Case{ "dejudder_motion_scale_hi", "-1", false },
                Case{ "dejudder_trust_scale", "0.6", true }, Case{ "dejudder_trust_scale", "1.1", false },
                Case{ "dither_grain_tolerance_scale", "1", true }, Case{ "dither_grain_tolerance_scale", "3", true },
                Case{ "dither_grain_tolerance_scale", "0.5", false }, Case{ "dither_grain_tolerance_scale", "9", false },
                Case{ "fbo_float32", "auto", true }, Case{ "fbo_float32", "off", true },
                Case{ "fbo_float32", "yes", false },
                Case{ "dither_vectors", "auto", true }, Case{ "dither_vectors", "own", true },
                Case{ "dither_vectors", "external", false },
                // Hunt 2 M1 / M2-M3 (picture C).
                Case{ "nls_sampling", "catmull", true }, Case{ "nls_sampling", "bilinear", true },
                Case{ "nls_sampling", "lanczos", false },
                Case{ "source_coding_fixes", "on", true }, Case{ "source_coding_fixes", "off", true },
                Case{ "source_coding_fixes", "auto", false },
                // "polar" is retired (runs as auto) but still loads (pruning 2026-10-17).
                Case{ "adaptive_faithful", "ortho", true }, Case{ "adaptive_faithful", "polar", true },
                Case{ "adaptive_faithful", "spline", false },
                // Retired keys (RetiredSettings.h): any value loads; the renderer ignores it.
                Case{ "fix_scene_cooldown", "off", true }, Case{ "fix_scene_cooldown", "on", true },
                Case{ "dejudder_picture_area", "off", true }, Case{ "dejudder_picture_area", "anything", true },
                Case{ "dither_vectors_new_frame", "off", true },
                Case{ "adaptive_faithful", "auto", true },   // Auto is the owner-chosen default (2026-10-16)
                Case{ "dejudder_est_candidates", "off", true }, Case{ "dejudder_est_qpel", "on", true },
                Case{ "dejudder_est_gate", "maybe", false }, Case{ "dejudder_est_lambda", "0.5", true },
                Case{ "dejudder_est_lambda", "9", false },
                Case{ "dejudder_est_split", "on", true }, Case{ "dejudder_est_split_sad", "0", true },
                Case{ "dejudder_est_split_sad", "20", false },
                Case{ "dejudder_low_res", "all", true }, Case{ "dejudder_low_res", "search", true },
                Case{ "dejudder_low_res", "off", true }, Case{ "dejudder_low_res", "on", false },
                Case{ "dejudder_estimate_ahead", "on", true }, Case{ "dejudder_estimate_ahead", "off", true },
                Case{ "dejudder_estimate_ahead", "auto", false },
                Case{ "capture_path", "standard", true }, Case{ "capture_path", "gpu", true },
                Case{ "capture_path", "fast", false },
                Case{ "dejudder_motion_rolloff", "on", true }, Case{ "dejudder_motion_rolloff", "auto", false },
                Case{ "dejudder_roll_lo", "8", true }, Case{ "dejudder_roll_hi", "24", true },
                Case{ "dejudder_roll_hi", "500", false }, Case{ "dejudder_roll_floor", "0.25", true },
                Case{ "dejudder_roll_floor", "1.5", false }, Case{ "dejudder_object_strength", "0.5", true },
                Case{ "dejudder_object_strength", "2", false },
                Case{ "dejudder_texture_tolerance", "0.5", true }, Case{ "dejudder_texture_tolerance", "5", false },
                Case{ "dejudder_global_two", "on", true }, Case{ "dejudder_global_two", "auto", false },
                Case{ "dejudder_regional_hold", "off", true }, Case{ "dejudder_regional_hold", "yes", false },
                Case{ "dejudder_cut_histogram", "0.3", true }, Case{ "dejudder_cut_histogram", "3", false },
                Case{ "dejudder_search_range", "8", true }, Case{ "dejudder_search_range", "17", false },
                Case{ "dejudder_consistency_px", "2", true }, Case{ "dejudder_consistency_px", "0.1", false },
                Case{ "dejudder_fade_match", "on", true }, Case{ "dejudder_fade_match", "1", false },
                Case{ "dejudder_fade_floor", "0.25", true }, Case{ "dejudder_fade_floor", "1.5", false },
                Case{ "dejudder_verify", "on", true }, Case{ "dejudder_verify", "maybe", false },
                Case{ "dejudder_verify_share", "0.2", true }, Case{ "dejudder_verify_share", "0", false },
                Case{ "dejudder_pixel_trust", "on", true }, Case{ "dejudder_pixel_trust", "2", false },
                Case{ "dejudder_pixel_trust_lo", "3", true }, Case{ "dejudder_pixel_trust_lo", "0.1", false },
                Case{ "dejudder_pixel_trust_hi", "8", true }, Case{ "dejudder_pixel_trust_hi", "40", false },
                Case{ "dejudder_hold_feather_px", "4", true }, Case{ "dejudder_hold_feather_px", "17", false },
                Case{ "dejudder_strength_even", "1", true }, Case{ "dejudder_strength_even", "1.2", false },
                Case{ "dejudder_even_rates", "30_and_29.97", true }, Case{ "dejudder_even_rates", "29.97", false },
                Case{ "dejudder_hold_hysteresis", "2", true }, Case{ "dejudder_hold_hysteresis", "0", false },
                Case{ "dejudder_block_taper", "on", true }, Case{ "dejudder_taper_sad", "6", true },
                Case{ "adaptive_upscale", "on", true }, Case{ "adaptive_upscale", "auto", false },
                Case{ "adaptive_upscale_w_base", "0.8", true }, Case{ "adaptive_upscale_w_base", "1.5", false },
                Case{ "adaptive_upscale_superres_scale", "off", true },
                Case{ "preserve_extended_range", "on", true }, Case{ "preserve_extended_range", "off", true },
                Case{ "preserve_extended_range", "auto", false },
                Case{ "sdr_tone_shape", "knee", true }, Case{ "sdr_tone_shape", "on", false },
                Case{ "sdr_white_headroom", "0.15", true }, Case{ "sdr_white_headroom", "0.6", false },
                Case{ "sdr_knee_start", "0.8", true }, Case{ "sdr_knee_start", "0.3", false },
                Case{ "sdr_knee_strength", "1", true }, Case{ "sdr_knee_strength", "1.5", false },
                Case{ "sdr_contrast_recovery", "auto", true }, Case{ "sdr_contrast_recovery", "off", true },
                Case{ "sdr_contrast_recovery", "0.3", true }, Case{ "sdr_contrast_recovery", "3", false } })
            {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n"
                    "[vprenderer.input.cinema]\n" << c.key << ": " << c.value << "\n";
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                UnifiedProfileRuntime::Runtime runtime;
                const bool ok = MainConfigSchema::Validate(config, error) && runtime.Initialize(config, {}, error);
                Assert::AreEqual(c.valid, ok, std::wstring(error.begin(), error.end()).c_str());
                if (!c.valid)
                    Assert::IsTrue(error.find(c.key) != std::string::npos,
                        std::wstring(error.begin(), error.end()).c_str());
            }
        }

        // v1.4.02 merge (owner 2026-10-15): VP's motion handling or libplacebo's
        // smooth motion, one or the other, chosen by motion_engine (the profile
        // rules pick it); auto = whichever is configured, VP's when both.
        TEST_METHOD(MotionEngineIsOneOrTheOther)
        {
            using DejudderPolicy::MotionEngine;
            using DejudderPolicy::ResolveEngine;
            Assert::IsTrue(ResolveEngine("vp", false, true) == MotionEngine::Vp, L"vp: smooth motion off even if set");
            Assert::IsTrue(ResolveEngine("smooth_motion", true, false) == MotionEngine::SmoothMotion, L"smooth_motion: VP's off even if set");
            Assert::IsTrue(ResolveEngine("auto", true, true) == MotionEngine::Vp, L"auto, both set: VP's");
            Assert::IsTrue(ResolveEngine("auto", false, true) == MotionEngine::SmoothMotion, L"auto, only smooth motion set: it runs");
            Assert::IsTrue(ResolveEngine("auto", true, false) == MotionEngine::Vp);
            Assert::IsTrue(ResolveEngine("auto", false, false) == MotionEngine::Vp, L"nothing set: VP's (which is off)");
            Assert::IsTrue(DejudderPolicy::Configured(0.25, 0.0) && DejudderPolicy::Configured(0.0, 1.0) && !DejudderPolicy::Configured(0.0, 0.0));
            // With smooth motion running the de-judder steps aside, so no motion
            // vectors are produced for any consumer.
            DejudderPolicy::Input in;
            in.strength = 0.25; in.sourceHz = 24000.0 / 1001.0; in.displayHz = 60.0;
            in.haveNext = true; in.nextMatches = true;
            Assert::IsTrue(DejudderPolicy::Decide(in) == DejudderPolicy::Reason::Active);
            in.smoothMotion = true;
            Assert::IsTrue(DejudderPolicy::Decide(in) == DejudderPolicy::Reason::SmoothMotion);
            Assert::AreEqual(std::string("smooth motion (frame mixing) active"),
                std::string(DejudderPolicy::Name(DejudderPolicy::Reason::SmoothMotion)));
        }

        // Delta 10: the even (2:2) strength. 30.00 takes it; 29.97 only when
        // chosen; 24p keeps dejudder_strength; 50p on 60 Hz stays excluded.
        TEST_METHOD(DejudderEvenCadenceStrength)
        {
            using DejudderPolicy::Reason;
            DejudderPolicy::Input in;
            in.strength = 0.25; in.sourceHz = 30.0; in.displayHz = 60.0;
            in.haveNext = true; in.nextMatches = true;
            auto with = [&](auto change) { DejudderPolicy::Input copy = in; change(copy); return copy; };
            // Even strength 0: today (no extras, cadence not eligible).
            Assert::IsTrue(DejudderPolicy::Decide(in) == Reason::IneligibleCadence);
            Assert::IsFalse(DejudderPolicy::ExtrasAllowed(in));
            // Even strength 1.0 on 30.00: extras at 1.0.
            const DejudderPolicy::Input on = with([](auto& i) { i.strengthEven = 1.0; });
            Assert::IsTrue(DejudderPolicy::Decide(on) == Reason::Active);
            Assert::IsTrue(DejudderPolicy::ExtrasAllowed(on));
            Assert::AreEqual(1.0, DejudderPolicy::EffectiveStrength(on));
            Assert::IsTrue(DejudderPolicy::IsEvenCadence(DejudderPolicy::CadenceRatio(on.sourceHz, on.displayHz)));
            // Only the even strength set (the uneven one 0): still active on 30p.
            Assert::IsTrue(DejudderPolicy::Decide(with([](auto& i) { i.strength = 0.0; i.strengthEven = 1.0; })) == Reason::Active);
            // 29.97: only with 30_and_29.97.
            const DejudderPolicy::Input ntsc = with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 29.97; i.displayHz = 59.94; });
            Assert::IsFalse(DejudderPolicy::ExtrasAllowed(ntsc));
            Assert::IsTrue(DejudderPolicy::ExtrasAllowed(with([](auto& i)
                { i.strengthEven = 1.0; i.sourceHz = 29.97; i.displayHz = 59.94; i.evenIncludes1001 = true; })));
            Assert::IsTrue(DejudderPolicy::ExtrasAllowed(with([](auto& i)
                { i.strengthEven = 1.0; i.evenIncludes1001 = true; })), L"30.00 takes it with both choices");
            // 24p on 60 Hz: the uneven strength, whatever the even key says.
            const DejudderPolicy::Input film = with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 24.0; });
            Assert::AreEqual(0.25, DejudderPolicy::EffectiveStrength(film));
            // 25p on 60 Hz is uneven (2.4): the normal strength.
            Assert::AreEqual(0.25, DejudderPolicy::EffectiveStrength(with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 25.0; })));
            // 50p on 60 Hz (1.2) stays excluded; Low_Latency stays off.
            Assert::IsTrue(DejudderPolicy::Decide(with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 50.0; })) == Reason::IneligibleCadence);
            Assert::IsTrue(DejudderPolicy::Decide(with([](auto& i) { i.strengthEven = 1.0; i.lowLatency = true; })) == Reason::LowLatency);
            // General (owner 2026-10-14): any even cadence of a film-like
            // source takes the even strength - 25p on 50 Hz, 24p on 48 and on
            // 120 Hz, 30p on 120 Hz - and 1000/1001 sources only when chosen.
            Assert::IsTrue(DejudderPolicy::ExtrasAllowed(with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 25.0; i.displayHz = 50.0; })));
            Assert::IsTrue(DejudderPolicy::ExtrasAllowed(with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 24.0; i.displayHz = 48.0; })));
            Assert::IsTrue(DejudderPolicy::ExtrasAllowed(with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 24.0; i.displayHz = 120.0; })));
            Assert::IsTrue(DejudderPolicy::ExtrasAllowed(with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 30.0; i.displayHz = 120.0; })));
            Assert::IsFalse(DejudderPolicy::ExtrasAllowed(with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 24000.0 / 1001; i.displayHz = 120000.0 / 1001; })));
            Assert::IsTrue(DejudderPolicy::ExtrasAllowed(with([](auto& i)
                { i.strengthEven = 1.0; i.sourceHz = 24000.0 / 1001; i.displayHz = 120000.0 / 1001; i.evenIncludes1001 = true; })));
            // 50p and 60p are video on any display (owner): never.
            Assert::IsFalse(DejudderPolicy::ExtrasAllowed(with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 60.0; i.displayHz = 120.0; })));
            Assert::IsFalse(DejudderPolicy::ExtrasAllowed(with([](auto& i) { i.strength = 1.0; i.sourceHz = 60.0; i.displayHz = 144.0; })));
            Assert::IsFalse(DejudderPolicy::ExtrasAllowed(with([](auto& i) { i.strengthEven = 1.0; i.sourceHz = 50.0; i.displayHz = 100.0; })));
        }

        TEST_METHOD(DejudderPolicyIsGeneral)
        {
            // Film-like: up to about 30 fps, and 48p; 50p/60p are video.
            for (double hz : { 12.0, 23.976, 24.0, 25.0, 29.97, 30.0, 47.952, 48.0 })
                Assert::IsTrue(DejudderPolicy::IsFilmLike(hz));
            for (double hz : { 0.0, 40.0, 50.0, 59.94, 60.0, 100.0, 120.0 })
                Assert::IsFalse(DejudderPolicy::IsFilmLike(hz));
            // The 1000/1001 rates.
            for (double hz : { 24000.0 / 1001, 30000.0 / 1001, 48000.0 / 1001, 60000.0 / 1001, 23.976, 29.97 })
                Assert::IsTrue(DejudderPolicy::IsRate1001(hz));
            for (double hz : { 24.0, 25.0, 30.0, 48.0, 50.0, 60.0 })
                Assert::IsFalse(DejudderPolicy::IsRate1001(hz));
            // Uneven cadences at any rates: 24p on 60, 25p on 60, 24p on 100,
            // 25p on 120, 24p on 144 (6.0 is even), 48p on 120 (2.5).
            for (double ratio : { 2.5, 2.4, 100.0 / 24, 4.8, 120.0 / 48 })
                Assert::IsTrue(DejudderPolicy::IsUnevenCadence(ratio));
            // Even cadences N >= 2: 2:2, 24p on 72, 24p on 120 (5), 24p on 144 (6).
            Assert::AreEqual(2, DejudderPolicy::EvenCycle(2.0));
            Assert::AreEqual(3, DejudderPolicy::EvenCycle(3.0));
            Assert::AreEqual(5, DejudderPolicy::EvenCycle(120.0 / 24));
            Assert::AreEqual(5, DejudderPolicy::EvenCycle((120000.0 / 1001) / (24000.0 / 1001)));
            Assert::AreEqual(6, DejudderPolicy::EvenCycle(144.0 / 24));
            Assert::AreEqual(0, DejudderPolicy::EvenCycle(2.5));
            Assert::AreEqual(0, DejudderPolicy::EvenCycle(1.0));
            // The extras cap follows the ratio: a frame owns ceil(ratio)
            // refreshes at most (24p on 60: 3, 2 extras; on 120: 5, 4 extras;
            // 25p on 120: 5, 4 extras; 24p on 240: 10, 9 extras).
            Assert::AreEqual(2, DejudderPolicy::MaxExtras(2.5));
            Assert::AreEqual(2, DejudderPolicy::MaxExtras(60000.0 / 1001 / (24000.0 / 1001)));
            Assert::AreEqual(1, DejudderPolicy::MaxExtras(2.0));
            Assert::AreEqual(4, DejudderPolicy::MaxExtras(5.0));
            Assert::AreEqual(4, DejudderPolicy::MaxExtras(4.8));
            Assert::AreEqual(9, DejudderPolicy::MaxExtras(10.0));
            Assert::AreEqual(0, DejudderPolicy::MaxExtras(1.0));
            // Active on uneven cadences at high refresh rates.
            DejudderPolicy::Input in;
            in.strength = 0.25; in.haveNext = true; in.nextMatches = true;
            for (auto rates : { std::make_pair(24.0, 100.0), std::make_pair(25.0, 120.0), std::make_pair(48.0, 120.0) })
            {
                in.sourceHz = rates.first; in.displayHz = rates.second;
                Assert::IsTrue(DejudderPolicy::Decide(in) == DejudderPolicy::Reason::Active);
            }
        }

        TEST_METHOD(DejudderPolicyRunsOnlyForUnevenCadenceWithANextFrame)
        {
            using DejudderPolicy::Reason;
            DejudderPolicy::Input in;
            in.strength = 0.25; in.sourceHz = 24.0; in.displayHz = 60.0;
            in.haveNext = true; in.nextMatches = true;
            Assert::IsTrue(DejudderPolicy::Decide(in) == Reason::Active);
            auto with = [&](auto change) { DejudderPolicy::Input copy = in; change(copy); return DejudderPolicy::Decide(copy); };
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; }) == Reason::Off);
            Assert::IsTrue(with([](auto& i) { i.lowLatency = true; }) == Reason::LowLatency);
            Assert::IsTrue(with([](auto& i) { i.interlaced = true; }) == Reason::Interlaced);
            Assert::IsTrue(with([](auto& i) { i.displayHz = 0.0; }) == Reason::UnknownRates);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 0.0; }) == Reason::UnknownRates);
            // Uneven film cadences only (planner 6b-2 item 4, 6b-2a): refreshes per
            // source frame not a whole number (+-0.05), and at least 1.95.
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 23.976; i.displayHz = 59.94; }) == Reason::Active);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 23.976; }) == Reason::Active);    // 2.5025
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 25.0; }) == Reason::Active);      // 2.4
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 30.0; }) == Reason::IneligibleCadence); // 2:2
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 29.97; }) == Reason::IneligibleCadence);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 25.0; i.displayHz = 50.0; }) == Reason::IneligibleCadence);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 24.0; i.displayHz = 48.0; }) == Reason::IneligibleCadence);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 24.0; i.displayHz = 120.0; }) == Reason::IneligibleCadence);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 60.0; }) == Reason::IneligibleCadence); // 1:1
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 60.0; i.displayHz = 50.0; }) == Reason::IneligibleCadence);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 58.0; }) == Reason::IneligibleCadence); // ~1, nothing to fill
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 50.0; }) == Reason::IneligibleCadence); // 1.2: video
            // 40 fps on 60 Hz (1.5): fixed bands (general_rate_fixes off) called it video; the derived
            // class (below the 49 Hz video floor, fixed-constant batch 2026-10-18) makes it film.
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 40.0; i.generalRates = false; }) == Reason::IneligibleCadence);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 40.0; }) == Reason::Active);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 30.6; }) == Reason::IneligibleCadence); // 1.96: within 0.05 of 2
            Assert::IsFalse(DejudderPolicy::IsUnevenCadence(1.03));   // ~1: nothing to fill
            // Film under 2 refreshes per frame (owner 2026-10-14): 24p and 23.976p
            // on 30 Hz, 25p on 30, 20p on 30.
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 24.0; i.displayHz = 30.0; }) == Reason::Active);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 24000.0 / 1001; i.displayHz = 30000.0 / 1001; }) == Reason::Active);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 25.0; i.displayHz = 30.0; }) == Reason::Active);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 20.0; i.displayHz = 30.0; }) == Reason::Active);
            Assert::AreEqual(1, DejudderPolicy::MaxExtras(30.0 / 24.0));
            // 6c mask-only mode: the estimate runs for the occlusion mask on any
            // cadence and with strength 0, but extras need strength > 0 and an
            // uneven film cadence.
            Assert::IsTrue(DejudderPolicy::ExtrasAllowed(in));
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.occlusionMask = true; }) == Reason::Active);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 30.0; i.occlusionMask = true; }) == Reason::Active);
            // Mask alone is for film rates (<= 30 fps); with extras the mask
            // rides along on film cadences anyway.
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 50.0; i.occlusionMask = true; }) == Reason::VideoRateMaskOnly);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 59.94; i.strength = 0.0; i.occlusionMask = true; }) == Reason::VideoRateMaskOnly);
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 23.976; i.strength = 0.0; i.occlusionMask = true; }) == Reason::Active);
            Assert::IsTrue(with([](auto& i) { i.occlusionMask = true; }) == Reason::Active);   // 24p with extras
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.occlusionMask = true; i.lowLatency = true; }) == Reason::LowLatency);
            // 8-3 (owner 2026-10-14): the grain map never starts the estimate
            // on its own - it uses the motion when the de-judder or the mask
            // runs it, and falls back otherwise.
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.grainMap = true; }) == Reason::Off);
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.grainMap = true; i.sourceHz = 30.0; }) == Reason::Off);
            Assert::IsTrue(with([](auto& i) { i.grainMap = true; }) == Reason::Active);   // with the de-judder
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.grainMap = true; i.occlusionMask = true; }) == Reason::Active);
            {
                DejudderPolicy::Input mapOnly = in;
                mapOnly.strength = 0.0; mapOnly.grainMap = true;
                Assert::IsFalse(DejudderPolicy::ExtrasAllowed(mapOnly));
            }
            {
                DejudderPolicy::Input maskOnly = in;
                maskOnly.strength = 0.0; maskOnly.occlusionMask = true;
                Assert::IsFalse(DejudderPolicy::ExtrasAllowed(maskOnly));
                DejudderPolicy::Input evenWithMask = in;
                evenWithMask.sourceHz = 30.0; evenWithMask.occlusionMask = true;
                Assert::IsFalse(DejudderPolicy::ExtrasAllowed(evenWithMask));
            }
            Assert::IsFalse(DejudderPolicy::IsUnevenCadence(2.04));
            Assert::IsTrue(DejudderPolicy::IsUnevenCadence(2.06));
            Assert::IsTrue(with([](auto& i) { i.nativeRgb = true; }) == Reason::NativeRgb);
            Assert::IsTrue(with([](auto& i) { i.planeFormatsUsable = false; }) == Reason::PlaneFormat);
            Assert::IsTrue(with([](auto& i) { i.haveNext = false; }) == Reason::NoNextFrame);
            Assert::IsTrue(with([](auto& i) { i.nextMatches = false; }) == Reason::NextFrameDiffers);
            // The first failing check is the one reported: off beats everything.
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.lowLatency = true; i.haveNext = false; }) == Reason::Off);
            Assert::AreEqual("active", DejudderPolicy::Name(Reason::Active));
            Assert::AreEqual("low-latency queue", DejudderPolicy::Name(Reason::LowLatency));
        }

        TEST_METHOD(DejudderExtrasFillOnlyTheRefreshesTheFrameOwns)
        {
            using DejudderPolicy::Stop;
            // 24p on 60 Hz, times in ms / 1000. A's callback at 0, display
            // latency floor 100 ms, A's present call 90 ms after its callback
            // (so B's call is due at 41.67 + 90 = 131.67), B needs 6 ms.
            const double pd = 1.0 / 60.0, ps = 1.0 / 24.0;
            auto input = [&](double previousVsync, int done)
            {
                DejudderPolicy::ExtraInput in;
                in.previousVsync = previousVsync; in.displayPeriod = pd; in.sourcePeriod = ps;
                in.callbackA = 0.0; in.latencyFloor = 0.100; in.callToCallback = 0.090;
                in.renderAllowance = 0.006; in.extrasDone = done;
                return in;
            };
            // A first shown at 100 ms (phase 0): extras at 116.7 (0.4) and
            // 133.3 (0.8)? B can make 133.3 only if 131.67 + 6 < 133.3: no,
            // so the third refresh is A's too.
            DejudderPolicy::ExtraDecision d = DejudderPolicy::PlanExtra(input(0.100, 0));
            Assert::IsTrue(d.present);
            Assert::AreEqual(0.4, d.delta, 1e-9);
            d = DejudderPolicy::PlanExtra(input(0.100 + pd, 1));
            Assert::IsTrue(d.present);
            Assert::AreEqual(0.8, d.delta, 1e-9);
            // The next refresh (150 ms) is B's.
            d = DejudderPolicy::PlanExtra(input(0.100 + 2 * pd, 2));
            Assert::IsFalse(d.present);
            Assert::IsTrue(d.stop == Stop::NextFrameNear);
            // B arriving early, or already queued, ends the extras at once.
            DejudderPolicy::ExtraInput early = input(0.100, 0);
            early.callToCallback = 0.065;   // B's call due at 106.7: it makes 116.7
            Assert::IsTrue(DejudderPolicy::PlanExtra(early).stop == Stop::NextFrameNear);
            DejudderPolicy::ExtraInput queued = input(0.100, 0);
            queued.nextFrameArrived = true;
            Assert::IsTrue(DejudderPolicy::PlanExtra(queued).stop == Stop::NextFrameArrived);
            // A frame first shown at phase 0.2 (108.3 ms) gets one extra at 0.6.
            d = DejudderPolicy::PlanExtra(input(0.100 + 0.2 * ps, 0));
            Assert::IsTrue(d.present);
            Assert::AreEqual(0.6, d.delta, 1e-9);
            // Phase past the next frame, or the cap, stops; a negative phase clamps.
            DejudderPolicy::ExtraInput late = input(0.100, 0);
            late.latencyFloor = 0.070;
            late.callToCallback = 0.200;
            Assert::IsTrue(DejudderPolicy::PlanExtra(late).stop == Stop::PastNextFrame);
            Assert::IsTrue(DejudderPolicy::PlanExtra(input(0.100, 3)).stop == Stop::MaxExtras);
            DejudderPolicy::ExtraInput before = input(0.050, 0);
            before.callToCallback = 0.200;
            d = DejudderPolicy::PlanExtra(before);
            Assert::IsTrue(d.present);
            Assert::AreEqual(0.0, d.delta);
        }

        TEST_METHOD(DejudderTelemetrySeesTheMainPresentNotTheExtras)
        {
            // A = 100 showed on refresh 500; its extras were 101 and 102.
            // B = 103; its extra 104.
            std::deque<uint32_t> extras{ 101, 102, 104 };
            std::deque<std::pair<uint32_t, uint32_t>> mains{ { 100, 500 }, { 103, 503 } };
            uint32_t count = 102, refresh = 502;
            Assert::IsTrue(DejudderPolicy::MainPresentFor(extras, mains, count, refresh));
            Assert::AreEqual(100u, count);
            Assert::AreEqual(500u, refresh);
            count = 103; refresh = 503;   // a main present on screen: untouched
            Assert::IsFalse(DejudderPolicy::MainPresentFor(extras, mains, count, refresh));
            Assert::AreEqual(103u, count);
            count = 104; refresh = 504;
            Assert::IsTrue(DejudderPolicy::MainPresentFor(extras, mains, count, refresh));
            Assert::AreEqual(103u, count);
            Assert::AreEqual(503u, refresh);
            // No extras ran: always an identity.
            std::deque<uint32_t> none;
            count = 102; refresh = 502;
            Assert::IsFalse(DejudderPolicy::MainPresentFor(none, mains, count, refresh));
            Assert::AreEqual(102u, count);
            // Extras between two main presents (for the pacing trace's delta).
            Assert::AreEqual(2u, DejudderPolicy::ExtrasBetween(extras, 100, 103));
            Assert::AreEqual(1u, DejudderPolicy::ExtrasBetween(extras, 103, 105));
            Assert::AreEqual(0u, DejudderPolicy::ExtrasBetween(none, 100, 103));
            // Ids wrap at 2^32.
            std::deque<uint32_t> wrapped{ 0xFFFFFFFFu, 0u };
            Assert::AreEqual(2u, DejudderPolicy::ExtrasBetween(wrapped, 0xFFFFFFFEu, 1u));
        }

        TEST_METHOD(NlsMaskMappingMatchesTheShadersCentreScales)
        {
            // Classic NLS: the warped axis gets the stretch, the other is 1.
            std::map<std::string, std::string> classic = {
                { "strength", "1" }, { "curve", "2" }, { "geometry", "1" }, { "center_protection", "0.35" } };
            NlsMaskMapping::Mapping m = NlsMaskMapping::FromParameters(classic, 1.33, false, false);
            Assert::IsTrue(m.valid);
            Assert::AreEqual(1.33, m.xScale, 1e-9);
            Assert::AreEqual(1.0, m.yScale, 1e-9);
            Assert::AreEqual(0.35, m.xProtect, 1e-9);
            m = NlsMaskMapping::FromParameters(classic, 1.33, true, false);
            Assert::AreEqual(1.0, m.xScale, 1e-9);
            Assert::AreEqual(1.33, m.yScale, 1e-9);
            // NLS+: balance 0.25 x strength 0.8 = 0.2, under the 1.08 zoom limit.
            std::map<std::string, std::string> plus = {
                { "strength", "0.8" }, { "curve", "2" }, { "geometry", "1" }, { "axis_balance", "0.25" },
                { "max_center_zoom", "1.08" }, { "horizontal_center_protection", "0.35" },
                { "vertical_center_protection", "0.25" } };
            m = NlsMaskMapping::FromParameters(plus, 1.33, false, true);
            Assert::IsTrue(m.valid);
            Assert::AreEqual(std::pow(1.33, 0.8), m.xScale, 1e-9);
            Assert::AreEqual(std::pow(1.33, -0.2), m.yScale, 1e-9);
            Assert::AreEqual(0.25, m.yProtect, 1e-9);
            // geometry as the shaders read it: the fraction is cut off.
            classic["geometry"] = "0.7";
            Assert::AreEqual(0, NlsMaskMapping::FromParameters(classic, 1.33, false, false).geometry);
            classic["geometry"] = "1";
            // A parameter the shader needs is missing: mask off.
            classic.erase("curve");
            Assert::IsFalse(NlsMaskMapping::FromParameters(classic, 1.33, false, false).valid);
            plus.erase("axis_balance");   // NLS+ without its balance: mask off
            Assert::IsFalse(NlsMaskMapping::FromParameters(plus, 1.33, false, true).valid);
            // The map keeps the edges and the centre, and is monotonic.
            for (int geometry : { 0, 1 })
            {
                Assert::AreEqual(0.0, NlsMaskMapping::MapCoordinate(0.0, 1.33, 2.0, geometry, 0.35), 1e-9);
                Assert::AreEqual(0.5, NlsMaskMapping::MapCoordinate(0.5, 1.33, 2.0, geometry, 0.35), 1e-9);
                Assert::AreEqual(1.0, NlsMaskMapping::MapCoordinate(1.0, 1.33, 2.0, geometry, 0.35), 1e-9);
                double previous = -1.0;
                for (int i = 0; i <= 100; ++i)
                {
                    const double v = NlsMaskMapping::MapCoordinate(i / 100.0, 1.33, 2.0, geometry, 0.35);
                    Assert::IsTrue(v >= previous);
                    previous = v;
                }
            }
        }

        TEST_METHOD(DejudderLatencyFloorIsTheRecentMinimum)
        {
            DejudderPolicy::LatencyFloor floor;
            Assert::AreEqual(0.0, floor.Floor());
            floor.Add(0.108); floor.Add(0.100); floor.Add(0.108);
            Assert::AreEqual(0.100, floor.Floor());
            for (size_t i = 0; i < DejudderPolicy::LatencyFloor::Capacity; ++i)
                floor.Add(0.120);
            Assert::AreEqual(0.120, floor.Floor(), 0.0, L"Old values leave the window");
            // One early outlier among a full window does not move the floor
            // (stream robustness, owner 2026-10-14: the 5th percentile).
            floor.Add(0.090);
            Assert::AreEqual(0.120, floor.Floor(), 0.0, L"A single early outlier is ignored");
            for (int i = 0; i < 10; ++i) floor.Add(0.110);
            Assert::AreEqual(0.110, floor.Floor(), 0.0, L"A real, sustained earlier latency is followed");
            Assert::AreEqual(floor.Window(), floor.Count());
            // The window is five seconds at any source rate.
            floor.SetSourceRate(60.0);
            Assert::AreEqual(size_t{ 300 }, floor.Window());
            floor.SetSourceRate(24000.0 / 1001);
            Assert::AreEqual(size_t{ 120 }, floor.Window());
            floor.Reset();
            Assert::AreEqual(size_t{ 0 }, floor.Count());
        }

        // Pulldown detection (owner 2026-10-14): feed a repeat pattern, return
        // the decisions.
        static std::vector<Pulldown::Decision> FeedPulldown(Pulldown::Detector& detector, const std::string& frames,
            double signalHz, const std::string& gaps = std::string())
        {
            std::vector<Pulldown::Decision> decisions;
            for (size_t i = 0; i < frames.size(); ++i)
                decisions.push_back(detector.Next(frames[i] == 'R', i < gaps.size() && gaps[i] == 'G', signalHz));
            return decisions;
        }

        // The repeat pattern of `filmHz` carried in `signalHz` the way a source
        // does it: each signal frame shows the latest film frame due.
        static std::string CarriedPattern(double filmHz, double signalHz, size_t frames)
        {
            std::string pattern;
            long long previous = -1;
            for (size_t i = 0; i < frames; ++i)
            {
                const long long film = static_cast<long long>(std::floor(static_cast<double>(i) * filmHz / signalHz + 1e-9));
                pattern += film == previous ? 'R' : 'N';
                previous = film;
            }
            return pattern;
        }

        TEST_METHOD(PulldownLocksOntoAnyRateCombination)
        {
            // General (owner 2026-10-14): no rate is fixed. Exact rates and the
            // 1000/1001 family.
            struct Case { double film, signal; int cycle, unique; };
            const Case cases[] = {
                { 24.0, 60.0, 5, 2 }, { 24000.0 / 1001, 60000.0 / 1001, 5, 2 }, { 24.0, 30.0, 5, 4 },
                { 24000.0 / 1001, 30000.0 / 1001, 5, 4 }, { 30.0, 60.0, 2, 1 }, { 25.0, 50.0, 2, 1 },
                { 24.0, 50.0, 25, 12 }, { 25.0, 60.0, 12, 5 }, { 24.0, 120.0, 5, 1 }, { 30.0, 120.0, 4, 1 },
                { 24.0, 48.0, 2, 1 }, { 24.0, 72.0, 3, 1 }, { 50.0, 100.0, 2, 1 }, { 20.0, 60.0, 3, 1 },
                { 48.0, 60.0, 5, 4 } };
            for (const Case& c : cases)
            {
                const std::wstring name = std::to_wstring(c.film) + L" in " + std::to_wstring(c.signal);
                // Start part-way into a cycle: the phase is found, not assumed.
                const std::string frames = CarriedPattern(c.film, c.signal, static_cast<size_t>(c.signal * 4) + 3).substr(3);
                Pulldown::Detector detector;
                const auto decisions = FeedPulldown(detector, frames, c.signal);
                Assert::IsTrue(decisions.back().locked, (name + L": locked").c_str());
                Assert::AreEqual(c.cycle, decisions.back().cycle, (name + L": cycle").c_str());
                Assert::AreEqual(c.unique, decisions.back().unique, (name + L": new frames").c_str());
                Assert::AreEqual(c.film, decisions.back().filmHz, c.film * 1e-9, (name + L": film rate").c_str());
                size_t lockAt = 0;
                while (!decisions[lockAt].locked) ++lockAt;
                const size_t need = static_cast<size_t>(Pulldown::Detector::FramesToLock(c.cycle, c.signal));
                Assert::IsTrue(lockAt + 1 >= need, (name + L": not before two seconds and three cycles").c_str());
                Assert::IsTrue(lockAt + 1 <= need + static_cast<size_t>(c.cycle), (name + L": soon after").c_str());
                // From the lock on, exactly the repeats are dropped; kept frames
                // count up one by one.
                uint64_t kept = 0;
                for (size_t i = lockAt; i < decisions.size(); ++i)
                {
                    Assert::AreEqual(frames[i] == 'R', decisions[i].drop, (name + L": drops only the repeats").c_str());
                    if (!decisions[i].drop)
                        Assert::AreEqual(++kept, decisions[i].filmIndex);
                }
            }
        }

        TEST_METHOD(PulldownNeverLocksOnVideoStillsOrChanceRepeats)
        {
            auto neverLocks = [](const std::string& frames, double signalHz, const wchar_t* what)
            {
                Pulldown::Detector detector;
                for (const auto& d : FeedPulldown(detector, frames, signalHz))
                    Assert::IsFalse(d.locked || d.drop, what);
            };
            neverLocks(std::string(600, 'N'), 60.0, L"Every frame new: video");
            neverLocks(std::string(600, 'R'), 60.0, L"A still picture");
            // Chance repeats (a stuttering source): pseudo-random, 20% and 50%.
            for (int share : { 20, 50 })
            {
                std::string frames;
                uint32_t x = 12345u;
                for (int i = 0; i < 1200; ++i) { x = x * 1103515245u + 12345u; frames += (x >> 16) % 100 < static_cast<uint32_t>(share) ? 'R' : 'N'; }
                neverLocks(frames, 60.0, L"Random repeats");
            }
            // A regular pattern whose film rate is under 15 fps: animation on
            // twos at 24 (12 fps) and in 2:3 at 60 (12 fps), threes at 30 (10).
            neverLocks(CarriedPattern(12.0, 24.0, 600), 24.0, L"Twos at 24: 12 fps");
            neverLocks(CarriedPattern(12.0, 60.0, 600), 60.0, L"Twos carried 2:3: 12 fps");
            neverLocks(CarriedPattern(10.0, 30.0, 600), 30.0, L"Threes at 30: 10 fps");
            // Bunched repeats (right count, uneven): never real pulldown.
            std::string bunched;
            while (bunched.size() < 600) bunched += "NNRRR";   // 24 fps worth at 60, held 1 and 4
            neverLocks(bunched, 60.0, L"Uneven holds");
            std::string bunched30;
            while (bunched30.size() < 600) bunched30 += "NNNRR";   // 18 fps at 30, held 1,1,3
            neverLocks(bunched30, 30.0, L"Uneven holds at 30");
            // A burst of a real pattern shorter than two seconds inside video.
            for (double film : { 24.0, 30.0 })
            {
                std::string frames(60, 'N');
                frames += CarriedPattern(film, 60.0, 110);   // 1.83 s
                frames += std::string(60, 'N');
                neverLocks(frames, 60.0, L"A burst under two seconds");
            }
            // A regular pattern broken once a second (a periodic glitch).
            std::string glitched = CarriedPattern(24.0, 60.0, 600);
            for (size_t i = 37; i < glitched.size(); i += 60) glitched[i] = glitched[i] == 'R' ? 'N' : 'R';
            neverLocks(glitched, 60.0, L"Broken once a second");
        }

        TEST_METHOD(PulldownKeepsItsLockThroughStillsAndDropsItOnABreak)
        {
            // A still picture inside a lock: kept, only the cycle's own repeat
            // slots dropped (the clock stays even).
            Pulldown::Detector detector;
            Assert::IsTrue(FeedPulldown(detector, CarriedPattern(24.0, 30.0, 90), 30.0).back().locked);
            int dropped = 0;
            for (const auto& d : FeedPulldown(detector, std::string(20, 'R'), 30.0))
            {
                Assert::IsTrue(d.locked, L"A still picture does not break the lock");
                dropped += d.drop ? 1 : 0;
            }
            Assert::AreEqual(4, dropped, L"One drop per five frames, as the cycle");

            // Video after film: new pictures where repeats belong unlock it and
            // no new picture is ever dropped.
            detector.Reset();
            FeedPulldown(detector, CarriedPattern(24.0, 60.0, 180), 60.0);
            const auto video = FeedPulldown(detector, std::string(10, 'N'), 60.0);
            Assert::IsFalse(video.back().locked, L"Video after film unlocks");
            for (const auto& d : video)
                Assert::IsFalse(d.drop, L"A new picture is never dropped");
            // ... and the next lock needs a fresh two seconds.
            const auto again = FeedPulldown(detector, CarriedPattern(24.0, 60.0, 119), 60.0);
            Assert::IsFalse(again.back().locked, L"No quick re-lock");

            // A gap: the lock and history go; found again after two seconds.
            detector.Reset();
            const std::string film = CarriedPattern(24.0, 60.0, 300);
            Assert::IsTrue(FeedPulldown(detector, film, 60.0).back().locked);
            std::string gaps(film.size(), '.');
            gaps[0] = 'G';
            const auto after = FeedPulldown(detector, film, 60.0, gaps);
            Assert::IsFalse(after.front().locked, L"A gap drops the lock");
            Assert::IsTrue(after.back().locked, L"... and it is found again");

            // The signal rate moving (a new mode without a resync) drops it;
            // ppm drift of the same signal does not.
            const auto moved = FeedPulldown(detector, "N", 50.0);
            Assert::IsFalse(moved.front().locked || moved.front().drop, L"A rate change drops the lock");
            detector.Reset();
            FeedPulldown(detector, film, 60.0);
            Assert::IsTrue(FeedPulldown(detector, CarriedPattern(24.0, 60.0, 310).substr(300), 60.0 * 1.0001).back().locked,
                L"Drift keeps the lock");
        }

        TEST_METHOD(PulldownEvenPatternRule)
        {
            Assert::IsTrue(Pulldown::IsEvenPattern("NRNRR"));
            Assert::IsTrue(Pulldown::IsEvenPattern("NNNNR"));
            Assert::IsTrue(Pulldown::IsEvenPattern("RNR"));
            Assert::IsFalse(Pulldown::IsEvenPattern("NNRRR"));
            Assert::IsFalse(Pulldown::IsEvenPattern("NNNN"));
            Assert::IsFalse(Pulldown::IsEvenPattern("RRRR"));
        }

        TEST_METHOD(PulldownFilmClockIsEvenAndNeverEarly)
        {
            // 24p in 60 Hz: kept frames arrive at signal frames 0, 2, 5, 7, 10 ...
            Pulldown::FilmClock clock;
            const double period = 1.0 / 60.0;
            const int arrivals[] = { 0, 2, 5, 7, 10, 12, 15, 17, 20, 22, 25, 27 };
            double previous = -1.0;
            for (int n = 0; n < 12; ++n)
            {
                const double arrival = 3.0 + arrivals[n] * period;
                const double t = clock.Time(static_cast<uint64_t>(n + 1), arrival, 24.0);
                Assert::IsTrue(t >= arrival - 1e-12, L"Never before the frame arrived");
                if (n >= 2)
                    Assert::AreEqual(1.0 / 24.0, t - previous, 1e-9, L"Even 24 Hz spacing once settled");
                previous = t;
            }
        }

        TEST_METHOD(EdgeBlankingRejectsInvalidValues)
        {
            for (const char* value : { "-1", "4001", "ten", "1.5" })
            {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n"
                    "[vprenderer.viewport.cinema]\nblank_left: " << value << "\n";
                ConfigFile config;
                Assert::IsTrue(config.Load(file.path));
                std::string error;
                UnifiedProfileRuntime::Runtime runtime;
                Assert::IsFalse(MainConfigSchema::Validate(config, error) &&
                    runtime.Initialize(config, {}, error));
                Assert::IsTrue(error.find("blank_left") != std::string::npos,
                    std::wstring(error.begin(), error.end()).c_str());
            }
        }
	};

	TEST_CLASS(ConfigFileTestsVpv)
	{
	public:

        // Stage 9a (docs/VP-stage9-ambient.md): room lights. The state with
        // simulated readings (hysteresis, hold, a lost detector), the
        // [ambient] section, and a profile chosen by `$ambient`.
        TEST_METHOD(AmbientRoomLightsSwitchAndSelectProfiles)
        {
            using namespace Ambient;
            State state;
            Settings settings;
            Room room = Room::Dark;
            Assert::IsFalse(state.Current(0.0, room), L"off: no room for the rules");
            settings.mode = Mode::Manual;
            settings.room = Room::Lit;
            state.Configure(settings);
            Assert::IsTrue(state.Current(0.0, room) && room == Room::Lit);
            Assert::IsFalse(state.AddReading(1.0, 0.0), L"readings do nothing in manual mode");
            Assert::IsTrue(state.Current(0.0, room) && room == Room::Lit);

            settings = Settings();
            settings.mode = Mode::Sensor;
            settings.room = Room::Dark;
            settings.luxLit = 20.0;
            settings.luxDark = 8.0;
            settings.holdSeconds = 5.0;
            state.Configure(settings);
            Assert::IsTrue(state.Current(0.0, room) && room == Room::Dark, L"no reading yet: the saved room");
            Assert::IsTrue(state.AddReading(50.0, 1.0), L"the first reading decides at once");
            Assert::IsTrue(state.Current(1.0, room) && room == Room::Lit);
            Assert::IsFalse(state.AddReading(12.0, 2.0), L"between the thresholds: unchanged");
            Assert::IsFalse(state.AddReading(3.0, 3.0));
            Assert::IsFalse(state.AddReading(3.0, 7.9), L"held for less than 5 s");
            Assert::IsTrue(state.Current(7.9, room) && room == Room::Lit);
            Assert::IsTrue(state.AddReading(3.0, 8.0), L"held 5 s: dark");
            Assert::IsTrue(state.Current(8.0, room) && room == Room::Dark);
            // A brief excursion (a door, someone at the sensor) does not switch.
            Assert::IsFalse(state.AddReading(60.0, 9.0));
            Assert::IsFalse(state.AddReading(5.0, 10.0));
            Assert::IsFalse(state.AddReading(60.0, 11.0));
            Assert::IsFalse(state.AddReading(60.0, 15.9));
            Assert::IsTrue(state.AddReading(60.0, 16.0));
            // New thresholds or hold keep what was sensed (audit stage8-3-9-10b
            // item 1); a different detector (port) starts over.
            {
                Settings tuned = settings;
                tuned.luxLit = 25.0;
                tuned.holdSeconds = 3.0;
                state.Configure(tuned);
                Assert::IsTrue(state.Current(16.5, room) && room == Room::Lit, L"a threshold change kept the sensed room");
                Settings other = tuned;
                other.port = "COM9";
                Ambient::State fresh;
                fresh.Configure(tuned);
                Assert::IsTrue(fresh.AddReading(60.0, 1.0));
                fresh.Configure(other);
                Assert::IsTrue(fresh.Current(1.5, room) && room == Room::Dark, L"a new port starts over from the saved room");
                state.Configure(settings);
            }
            // A lost detector (no reading for 10 s) falls back to the saved room.
            Assert::IsTrue(state.Current(26.0, room) && room == Room::Lit);
            Assert::IsTrue(state.DetectorLost(26.1));
            Assert::IsTrue(state.Current(26.1, room) && room == Room::Dark);

            CachedConfigTestFile file;
            {
                std::ofstream out(file.path, std::ios::trunc);
                out << "[ambient]\nmode: sensor\nroom: lit\nlux_lit: 30\nlux_dark: 10\nhold_s: 2\n";
            }
            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            std::string error;
            Settings read;
            Assert::IsTrue(Read(config, read, error), std::wstring(error.begin(), error.end()).c_str());
            Assert::IsTrue(read.mode == Mode::Sensor && read.room == Room::Lit && read.luxLit == 30.0 &&
                read.luxDark == 10.0 && read.holdSeconds == 2.0);
            Assert::IsTrue(MainConfigSchema::Validate(config, error), std::wstring(error.begin(), error.end()).c_str());
            {
                std::ofstream out(file.path, std::ios::trunc);
                out << "[ambient]\nmode: dim\n";
            }
            ConfigFile invalid;
            Assert::IsTrue(invalid.Load(file.path, ConfigFile::ReadPolicy::Fresh));
            error.clear();
            Assert::IsFalse(Read(invalid, read, error));
            Assert::IsTrue(read.mode == Mode::Off, L"an invalid mode keeps the default");
            error.clear();
            Assert::IsFalse(MainConfigSchema::Validate(invalid, error));
            Assert::IsTrue(error.find("mode") != std::string::npos, std::wstring(error.begin(), error.end()).c_str());

            // A rendering profile chosen by the room.
            {
                std::ofstream out(file.path, std::ios::trunc);
                out << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
                    "[ambient]\nmode: manual\nroom: lit\n"
                    "[vprenderer.dark_room]\nquality: high\n"
                    "[vprenderer.lit_room]\nwhen: $ambient==\"lit\"\nsdr_black_nits: 0.05\n";
            }
            ConfigFile rules;
            Assert::IsTrue(rules.Load(file.path, ConfigFile::ReadPolicy::Fresh));
            error.clear();
            Assert::IsTrue(MainConfigSchema::Validate(rules, error), std::wstring(error.begin(), error.end()).c_str());
            RendererProfileConfig::Model model;
            Assert::IsTrue(RendererProfileConfig::Read(rules, model, error), std::wstring(error.begin(), error.end()).c_str());
            auto pick = [&](const char* ambient)
            {
                std::vector<RendererProfileConfig::AutomaticSelection> automatic;
                std::string selectError;
                Assert::IsTrue(RendererProfileConfig::SelectAutomatic(model,
                    [ambient](const std::string& name, std::string& value)
                    {
                        if (name != "ambient" || !ambient) return false;
                        value = ambient;
                        return true;
                    }, automatic, selectError));
                // Merge with v1.4.02-beta: `quality: high` now migrates into a Scaling profile, so the
                // Rendering (display) group is one of several automatic selections.
                for (const auto& selection : automatic)
                    if (selection.group == "display")
                        return selection.profiles.front();
                Assert::Fail(L"no Rendering selection");
                return std::string();
            };
            Assert::AreEqual("lit_room", pick("lit").c_str());
            Assert::AreEqual("dark_room", pick("dark").c_str());
            Assert::AreEqual("dark_room", pick(nullptr).c_str(), L"ambient off: the rule does not match");
        }

        // Stage 9b: the sensor's lines, the reader on a simulated port (a
        // failed open retried, a banner ignored, readings kept), and the
        // sensor never sharing the projector's port.
        TEST_METHOD(AmbientSensorReadsLuxLinesFromItsOwnPort)
        {
            double lux = -1.0;
            Assert::IsTrue(Ambient::ParseLuxLine("12.5", lux) && lux == 12.5);
            Assert::IsTrue(Ambient::ParseLuxLine("lux=3.25", lux) && lux == 3.25);
            Assert::IsTrue(Ambient::ParseLuxLine(" LUX: 40 ", lux) && lux == 40.0);
            Assert::IsTrue(Ambient::ParseLuxLine("7 lux", lux) && lux == 7.0);
            Assert::IsTrue(Ambient::ParseLuxLine("0.8lx", lux) && lux == 0.8);
            Assert::IsFalse(Ambient::ParseLuxLine("# VP light sensor ready", lux));
            Assert::IsFalse(Ambient::ParseLuxLine("", lux));
            Assert::IsFalse(Ambient::ParseLuxLine("-3", lux));
            Assert::IsFalse(Ambient::ParseLuxLine("12 volts", lux));
            Assert::IsFalse(Ambient::ParseLuxLine("lux 12", lux));

            struct Script
            {
                std::mutex mutex;
                int opens = 0;
                std::vector<std::string> lines = { "# VP light sensor ready", "lux=5.5", "garbage", "lux=60" };
                size_t next = 0;
            };
            auto script = std::make_shared<Script>();
            class FakePort : public LedDrive::ISerialPort
            {
            public:
                explicit FakePort(std::shared_ptr<Script> s) : script(std::move(s)) {}
                bool Open(const std::string& port, int baud, std::string& error) override
                {
                    std::lock_guard<std::mutex> guard(script->mutex);
                    if (++script->opens == 1) { error = "busy"; return false; }   // first try fails
                    open = port == "COM7" && baud == 115200;
                    return open;
                }
                void Close() override { open = false; }
                bool IsOpen() const override { return open; }
                void DiscardInput() override {}
                bool Write(const std::string&, std::string&) override { return true; }
                bool ReadLine(int timeoutMs, std::string& line) override
                {
                    {
                        std::lock_guard<std::mutex> guard(script->mutex);
                        if (script->next < script->lines.size())
                        {
                            line = script->lines[script->next++];
                            return true;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeoutMs, 20)));
                    return false;
                }
            private:
                std::shared_ptr<Script> script;
                bool open = false;
            };
            Ambient::SensorReader reader;
            Ambient::SensorReader::Timing timing;
            timing.readTimeoutMs = 20;
            timing.retryMs = 30;
            timing.silenceReopenMs = 100000;
            reader.Start("COM7", 115200, [script] { return std::unique_ptr<LedDrive::ISerialPort>(new FakePort(script)); }, timing);
            uint64_t sequence = 0;
            double newest = 0.0;
            for (int i = 0; i < 200 && newest != 60.0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                reader.Newest(sequence, newest);
            }
            Assert::AreEqual(60.0, newest, L"the newest reading");
            Assert::IsTrue(sequence >= 1);
            Assert::IsFalse(reader.Newest(sequence, newest), L"nothing newer");
            Assert::IsTrue(reader.Status().find("reading") != std::string::npos,
                std::wstring(reader.Status().begin(), reader.Status().end()).c_str());
            {
                std::lock_guard<std::mutex> guard(script->mutex);
                Assert::IsTrue(script->opens >= 2, L"the failed open was retried");
            }
            reader.Stop();
            Assert::IsFalse(reader.Running());

            // The projector's port cannot be the sensor's.
            CachedConfigTestFile file;
            {
                std::ofstream out(file.path, std::ios::trunc);
                out << "[ambient]\nmode: sensor\nport: com3\nbaud: 115200\n[projector.default]\nport: COM3\n";
            }
            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            std::string error;
            Assert::IsFalse(MainConfigSchema::Validate(config, error));
            Assert::IsTrue(error.find("projector") != std::string::npos, std::wstring(error.begin(), error.end()).c_str());
            {
                std::ofstream out(file.path, std::ios::trunc);
                out << "[ambient]\nmode: sensor\nport: COM5\nbaud: 115200\n[projector.default]\nport: COM3\n";
            }
            ConfigFile separate;
            Assert::IsTrue(separate.Load(file.path, ConfigFile::ReadPolicy::Fresh));
            error.clear();
            Assert::IsTrue(MainConfigSchema::Validate(separate, error), std::wstring(error.begin(), error.end()).c_str());
            Ambient::Settings settings;
            Assert::IsTrue(Ambient::Read(separate, settings, error));
            Assert::IsTrue(settings.port == "COM5" && settings.baud == 115200);
            {
                std::ofstream out(file.path, std::ios::trunc);
                out << "[ambient]\nport: LPT1\n";
            }
            ConfigFile badPort;
            Assert::IsTrue(badPort.Load(file.path, ConfigFile::ReadPolicy::Fresh));
            error.clear();
            Assert::IsFalse(MainConfigSchema::Validate(badPort, error));
        }

#if 0
#endif

		TEST_METHOD(DeckLinkYCbCr12CaptureChoice)
		{
			// Fix under test (owner 2026-10-15): a 12-bit YCbCr 4:2:2 signal was
			// refused (the SDK has no 12-bit YCbCr format). rgb12 (the default until bug hunt 2) asks
			// for 12-bit RGB, v210 for 10-bit 4:2:2, off refuses as before; other
			// signals are untouched.
			const auto ycc12 = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputYCbCr422 | bmdDetectedVideoInput12BitDepth);
			const auto ycc10 = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputYCbCr422 | bmdDetectedVideoInput10BitDepth);
			const auto rgb12 = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputRGB444 | bmdDetectedVideoInput12BitDepth);
			Assert::IsTrue(DeckLinkIsYCbCr12(ycc12));
			Assert::IsFalse(DeckLinkIsYCbCr12(ycc10) || DeckLinkIsYCbCr12(rgb12));
			Assert::AreEqual(0u, static_cast<unsigned int>(CanonicalDeckLinkCapturePixelFormat(ycc12)),
				L"without the fix the format choice finds nothing (the refusal)");
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat12BitRGB),
				static_cast<unsigned int>(CanonicalDeckLinkCapturePixelFormat(DeckLinkYCbCr12AsRgb12(ycc12))));
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat10BitYUV),
				static_cast<unsigned int>(CanonicalDeckLinkCapturePixelFormat(DeckLinkYCbCr12AsV210(ycc12))));
			// The R12 packing preference applies to the rgb12 choice.
			DeckLinkCaptureFormatPreferences le;
			le.rgb12 = bmdFormat12BitRGBLE;
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat12BitRGBLE),
				static_cast<unsigned int>(PreferredDeckLinkCapturePixelFormat(DeckLinkYCbCr12AsRgb12(ycc12), le)));
			// Other signals untouched.
			Assert::AreEqual(static_cast<unsigned int>(ycc10), static_cast<unsigned int>(DeckLinkYCbCr12AsRgb12(ycc10)));
			Assert::AreEqual(static_cast<unsigned int>(rgb12), static_cast<unsigned int>(DeckLinkYCbCr12AsV210(rgb12)));
			// The switch: default v210 (bug hunt 2); rgb12, off and a bad value from the file.
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) + "VideoProcessor-decklink-ycc12.cfg";
			auto read = [&](const char* value, DeckLinkCaptureFormatPreferences& preferences, std::string& error)
			{
				{
					std::ofstream file(path, std::ios::out | std::ios::trunc);
					file << "[decklink]\n";
					if (value)
						file << "ycbcr_12bit_capture: " << value << "\n";
				}
				ConfigFile config;
				Assert::IsTrue(config.Load(path));
				return ReadDeckLinkCaptureFormatPreferences(config, preferences, error);
			};
			DeckLinkCaptureFormatPreferences preferences;
			std::string error;
			Assert::IsTrue(read(nullptr, preferences, error));
			Assert::IsTrue(preferences.ycbcr12 == DeckLinkCaptureFormatPreferences::YCbCr12Capture::V210, L"default v210 (bug hunt 2, M4: BTB/WTW kept)");
			Assert::IsTrue(read("rgb12", preferences, error));
			Assert::IsTrue(preferences.ycbcr12 == DeckLinkCaptureFormatPreferences::YCbCr12Capture::Rgb12, L"rgb12 restores the earlier default");
			Assert::IsTrue(read("v210", preferences, error));
			Assert::IsTrue(preferences.ycbcr12 == DeckLinkCaptureFormatPreferences::YCbCr12Capture::V210);
			Assert::IsTrue(read("off", preferences, error));
			Assert::IsTrue(preferences.ycbcr12 == DeckLinkCaptureFormatPreferences::YCbCr12Capture::Off);
			Assert::IsFalse(read("maybe", preferences, error), L"a bad value is an error");
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[decklink]\nycbcr_12bit_capture: v210\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(MainConfigSchema::Validate(config, error), L"the schema accepts the key");
			DeleteFileA(path.c_str());
		}





		// Bug hunt 3 (CFG1): a configured false reaches VP for every default-on
		// boolean the configuration turns into a command-line switch; true still
		// emits the bare switch, and a default-off key's false adds nothing.
		TEST_METHOD(Hunt3DefaultOnBooleansCanBeTurnedOff)
		{
			size_t defaultOn = 0;
			for (const ConfigBoolSwitches::Option& option : ConfigBoolSwitches::Options())
			{
				const std::string key = option.keys[0];
				const std::wstring label(key.begin(), key.end());
				char directory[MAX_PATH] = {};
				Assert::IsTrue(GetTempPathA(ARRAYSIZE(directory), directory) > 0);
				const std::string path = std::string(directory) + "VideoProcessor-hunt3-bool-" + key + ".cfg";
				for (const bool configuredValue : { false, true })
				{
					{
						std::ofstream file(path, std::ios::out | std::ios::trunc);
						file << "[general]\n" << key << ": " << (configuredValue ? "true" : "false") << "\n";
					}
					ConfigFile config;
					Assert::IsTrue(config.Load(path), label.c_str());
					std::string error;
					Assert::IsTrue(MainConfigSchema::Validate(config, error), label.c_str());
					bool value = !configuredValue;
					Assert::IsTrue(config.TryGetBool("general", key, value), label.c_str());
					const std::vector<std::wstring> tokens = ConfigBoolSwitches::Tokens(option, true, value);
					if (configuredValue)
					{
						Assert::AreEqual(static_cast<size_t>(1), tokens.size(), label.c_str());
						Assert::AreEqual(std::wstring(option.commandLineSwitch), tokens[0], label.c_str());
					}
					else if (option.defaultOn)
					{
						Assert::AreEqual(static_cast<size_t>(2), tokens.size(), (label + L": false must reach VP").c_str());
						Assert::AreEqual(std::wstring(option.commandLineSwitch), tokens[0], label.c_str());
						Assert::AreEqual(std::wstring(L"false"), tokens[1], label.c_str());
					}
					else
						Assert::IsTrue(tokens.empty(), (label + L": a default-off false adds nothing").c_str());
				}
				DeleteFileA(path.c_str());
				if (option.defaultOn) ++defaultOn;
				Assert::IsTrue(ConfigBoolSwitches::Tokens(option, false, true).empty(), label.c_str());
			}
			// The audit: newlldv_sdr_veto is the default-on key (VP's own default).
			const ConfigBoolSwitches::Option* veto = ConfigBoolSwitches::Find("newlldv_sdr_veto");
			Assert::IsNotNull(veto);
			Assert::IsTrue(veto->defaultOn);
			Assert::AreEqual(HdrInferenceOptions().sdrVeto, veto->defaultOn, L"the switch table follows VP's default");
			// dv_tunnel_handoff (DV input fixes, default on; merged in integration round 5).
			const ConfigBoolSwitches::Option* handoff = ConfigBoolSwitches::Find("dv_tunnel_handoff");
			Assert::IsNotNull(handoff);
			Assert::IsTrue(handoff->defaultOn, L"dv_tunnel_handoff is default on");
			Assert::IsTrue(defaultOn >= 2);
			// Aliases resolve to their option.
			Assert::IsTrue(ConfigBoolSwitches::Find("start_minimized") == ConfigBoolSwitches::Find("startminimized"));
			Assert::IsNull(ConfigBoolSwitches::Find("not_a_switch"));
		}

		// Bug hunt 3 (CFG9): [general] queue_size is read (after the default queue
		// profile and [command_line]), not validated and ignored.
		TEST_METHOD(Hunt3GeneralQueueSizeIsRead)
		{
			char directory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(directory), directory) > 0);
			const std::string path = std::string(directory) + "VideoProcessor-hunt3-queue-size.cfg";
			auto resolve = [&](const char* contents, std::string& value, std::string& source)
			{
				{
					std::ofstream file(path, std::ios::out | std::ios::trunc);
					file << contents;
				}
				ConfigFile config;
				Assert::IsTrue(config.Load(path));
				std::string error;
				Assert::IsTrue(MainConfigSchema::Validate(config, error), std::wstring(error.begin(), error.end()).c_str());
				return QueueConfiguration::TryGetHardCapacityString(config, value, source);
			};
			std::string value, source;
			Assert::IsTrue(resolve("[general]\nqueue_size: 24\n", value, source));
			Assert::AreEqual(std::string("24"), value);
			Assert::AreEqual(std::string("general"), source);
			Assert::IsTrue(resolve("[general]\nqueue_size: 24\n[command_line]\nqueue_size: 12\n", value, source));
			Assert::AreEqual(std::string("12"), value, L"[command_line] before [general]");
			Assert::IsTrue(resolve("[general]\nqueue_size: 24\n[queue]\nqueue_size: 8\n", value, source));
			Assert::AreEqual(std::string("8"), value, L"the default queue profile first");
			Assert::AreEqual(std::string("queue"), source);
			Assert::IsFalse(resolve("[general]\nfullscreen: false\n", value, source));
			DeleteFileA(path.c_str());
		}
	};
}
