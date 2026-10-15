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
#include <QueueConfiguration.h>
#include <QueueLatency.h>
#include <QueueProfileRestartPolicy.h>
#include <ProfileChangeOverlay.h>
#include <blackmagic_decklink/BlackMagicDeckLinkTranslate.h>
#include <guid.h>
#include <microsoft_directshow/DirectShowTranslations.h>
#include <microsoft_directshow/MadVRShaderLoader.h>
#include <microsoft_directshow/video_renderers/DirectShowVideoRenderers.h>
#include <RendererConfigView.h>
#include <RendererProfileConfig.h>
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

	TEST_CLASS(ConfigurationReadCacheTests)
	{
	public:
        TEST_METHOD(CurrentBetaScreenEdgePaddingSurvivesAcceptedConfiguration)
        {
            CachedConfigTestFile file;
            std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n"
                "[vprenderer.viewport.scope]\nscreen_aspect: 47:20\n"
                "vertical_alignment: bottom\nscreen_edge_padding: 50\n";
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
            Assert::AreEqual(50, snapshot->viewport.screenEdgePadding);
            Assert::AreEqual(std::string("bottom"), snapshot->viewport.verticalAlignment);
            std::string padding;
            Assert::IsTrue(snapshot->configuration->TryGetString(
                "vprenderer.viewport.scope", "screen_edge_padding", padding));
            Assert::AreEqual(std::string("50"), padding);
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
                    for (const auto& selection : automatic)
                        if (selection.group == "display")
                        {
                            const auto profile = model.profiles.find("display." + selection.profiles.front());
                            Assert::IsTrue(profile != model.profiles.end());
                            const auto deband = profile->second.settings.find("deband");
                            return deband == profile->second.settings.end() ? std::string("auto") : deband->second;
                        }
                    return std::string("none");
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
            // floored to a whole number, cadence the exact rate.
            auto source = [](const char* rate)
            {
                const double exact = std::stod(rate);
                return [exact](const std::string& variable, std::string& value)
                {
                    if (variable == "source_rate")
                        value = std::to_string(static_cast<int>(std::floor(exact)));
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
            // Audit round 2: 59.94 Hz (source_rate 59) must select the game queue.
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
                    if (variable == "source_rate")
                        text << static_cast<int>(std::floor(exact));
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
                Case{ "adaptive_faithful", "ortho", true }, Case{ "adaptive_faithful", "polar", true },
                Case{ "adaptive_faithful", "auto", false },
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
            Assert::IsTrue(with([](auto& i) { i.sourceHz = 40.0; }) == Reason::IneligibleCadence); // 1.5
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
            // 8-3: the adaptive downscale's grain map runs the estimate alone,
            // like the mask: film rates, any cadence, never in Low_Latency, no
            // extras.
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.grainMap = true; }) == Reason::Active);
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.grainMap = true; i.sourceHz = 30.0; }) == Reason::Active);
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.grainMap = true; i.sourceHz = 23.976; i.displayHz = 47.952; }) == Reason::Active);
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.grainMap = true; i.sourceHz = 50.0; }) == Reason::VideoRateMaskOnly);
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.grainMap = true; i.sourceHz = 59.94; }) == Reason::VideoRateMaskOnly);
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.grainMap = true; i.lowLatency = true; }) == Reason::LowLatency);
            Assert::IsTrue(with([](auto& i) { i.strength = 0.0; i.grainMap = true; i.haveNext = false; }) == Reason::NoNextFrame);
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

		TEST_METHOD(RestartReadersReuseOneReadAndExplicitReloadSamplesIndependently)
		{
			CachedConfigTestFile file;
			file.Write("one");
			const auto before = ConfigFile::GetLoadCount();
			for (int restart = 0; restart < 51; ++restart)
			{
				ConfigFile reader;
				Assert::IsTrue(reader.Load(file.path));
				std::string value;
				Assert::IsTrue(reader.TryGetString("general", "value", value));
				Assert::AreEqual(std::string("one"), value);
			}
			Assert::IsTrue(ConfigFile::GetLoadCount() == before + 1);
			ConfigFile first, second;
			Assert::IsTrue(first.Load(file.path, ConfigFile::ReadPolicy::Fresh));
			Assert::IsTrue(second.Load(file.path, ConfigFile::ReadPolicy::Fresh));
			Assert::IsTrue(ConfigFile::GetLoadCount() == before + 3);
		}

		TEST_METHOD(RendererPathAliasSharesCachedContentAndWarnings)
		{
			CachedConfigTestFile file;
			std::ofstream(file.path) << "[general]\nvalue: one\nvalue: two\n";
			struct RestorePath
			{
				std::string path = ConfigFile::GetRendererConfigurationPath();
				~RestorePath() { ConfigFile::SetRendererConfigurationPath(path); }
			} restore;
			ConfigFile::SetRendererConfigurationPath(file.path);
			ConfigFile main, renderer;
			Assert::IsTrue(main.Load(file.path));
			const auto before = ConfigFile::GetLoadCount();
			Assert::IsTrue(renderer.Load(ConfigFile::RENDERER_FILENAME));
			Assert::IsTrue(ConfigFile::GetLoadCount() == before);
			Assert::IsFalse(main.GetWarnings().empty());
			Assert::IsTrue(main.GetWarnings() == renderer.GetWarnings());
			Assert::IsTrue(main.GetContentIdentity() == renderer.GetContentIdentity());
		}

		TEST_METHOD(SameSizeEditWithRestoredWriteTimeInvalidatesCache)
		{
			CachedConfigTestFile file;
			file.Write("one");
			ConfigFile original;
			Assert::IsTrue(original.Load(file.path));
			WIN32_FILE_ATTRIBUTE_DATA attributes{};
			Assert::IsTrue(GetFileAttributesExA(file.path.c_str(), GetFileExInfoStandard, &attributes) != 0);
			file.Write("two");
			HANDLE handle = CreateFileA(file.path.c_str(), FILE_WRITE_ATTRIBUTES,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			Assert::IsTrue(handle != INVALID_HANDLE_VALUE);
			const BOOL restored = SetFileTime(handle, nullptr, nullptr, &attributes.ftLastWriteTime);
			CloseHandle(handle);
			Assert::IsTrue(restored != 0);
			const auto before = ConfigFile::GetLoadCount();
			ConfigFile changed;
			Assert::IsTrue(changed.Load(file.path));
			std::string value;
			Assert::IsTrue(changed.TryGetString("general", "value", value));
			Assert::AreEqual(std::string("two"), value);
			Assert::IsTrue(ConfigFile::GetLoadCount() == before + 1);
			Assert::IsTrue(original.TryGetString("general", "value", value));
			Assert::AreEqual(std::string("one"), value);
		}

		TEST_METHOD(ReplacementDeletionAndRecreationNeverReturnStaleConfig)
		{
			CachedConfigTestFile file, replacement;
			file.Write("one"); replacement.Write("two");
			ConfigFile config;
			Assert::IsTrue(config.Load(file.path));
			Assert::IsTrue(MoveFileExA(replacement.path.c_str(), file.path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0);
			Assert::IsTrue(config.Load(file.path));
			std::string value;
			Assert::IsTrue(config.TryGetString("general", "value", value));
			Assert::AreEqual(std::string("two"), value);
			Assert::IsTrue(DeleteFileA(file.path.c_str()) != 0);
			Assert::IsFalse(config.Load(file.path));
			Assert::IsFalse(config.IsLoaded());
			Assert::IsFalse(config.TryGetString("general", "value", value));
			file.Write("new");
			Assert::IsTrue(config.Load(file.path));
			Assert::IsTrue(config.TryGetString("general", "value", value));
			Assert::AreEqual(std::string("new"), value);
		}
	};

	TEST_CLASS(ConfigFileTests)
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
                Assert::AreEqual(static_cast<size_t>(1), automatic.size());
                return automatic.front().profiles.front();
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

        TEST_METHOD(SubstringRuleRuntimeRefreshAndManualSelection)
        {
            CachedConfigTestFile file;
            std::ofstream(file.path) << "[general]\npersist_profile_selection: false\n"
                "[queue.Default]\nqueue_size: 16\ntarget_frames: 2\n"
                "[queue.VP_Low_Rate]\nqueue_size: 24\ntarget_frames: 3\n"
                "when: ${renderer} ~= \"VP Renderer\" && ${source_rate} <= 30\n";
            ConfigFile config;
            Assert::IsTrue(config.Load(file.path));
            std::string renderer = "VP Renderer", rate = "24", error;
            const auto lookup = [&](const std::string& name, std::string& value) {
                if (name == "renderer") value = renderer;
                else if (name == "source_rate") value = rate;
                else return false;
                return true;
            };
            UnifiedProfileRuntime::Runtime runtime;
            Assert::IsTrue(runtime.Initialize(config, lookup, error));
            Assert::AreEqual(std::string("vp_low_rate"), runtime.GetSnapshot()->queue.profile);
            UnifiedProfileRuntime::RefreshResult refreshed;
            renderer = "VP Renderer (Beta)";
            Assert::IsTrue(runtime.Refresh(lookup, refreshed, error));
            Assert::AreEqual(std::string("vp_low_rate"), runtime.GetSnapshot()->queue.profile);
            rate = "60";
            Assert::IsTrue(runtime.Refresh(lookup, refreshed, error));
            Assert::AreEqual(std::string("default"), runtime.GetSnapshot()->queue.profile);
            renderer = "madVR"; rate = "24";
            Assert::IsTrue(runtime.Refresh(lookup, refreshed, error));
            Assert::AreEqual(std::string("default"), runtime.GetSnapshot()->queue.profile);
            UnifiedProfileRuntime::SelectionResult selected;
            Assert::IsTrue(runtime.SelectProfile("queue", "vp_low_rate", true, lookup, selected, error));
            Assert::AreEqual(std::string("vp_low_rate"), selected.snapshot->queue.profile);
        }

		TEST_METHOD(ManualQueueSelectionUsesSavedSectionIdWithoutShortcut)
		{
			CachedConfigTestFile file;
			std::ofstream(file.path) <<
				"[queue.Default]\nqueue_size: 16\ntarget_frames: 2\n"
				"[queue.VP_60]\nqueue_size: 24\ntarget_frames: 3\n";
			ConfigFile config;
			Assert::IsTrue(config.Load(file.path));
			UnifiedProfileRuntime::Runtime runtime;
			std::string error;
			Assert::IsTrue(runtime.Initialize(config, {}, error),
				std::wstring(error.begin(), error.end()).c_str());
			UnifiedProfileRuntime::SelectionResult selected;
			Assert::IsTrue(runtime.SelectProfile("QUEUE", "VP_60", true,
				{}, selected, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(selected.changed);
			Assert::AreEqual(std::string("vp_60"),
				selected.snapshot->queue.profile);
		}

		TEST_METHOD(ManualProfileAndActionCommandsUseSavedRuntimeModel)
		{
			CachedConfigTestFile file;
			std::ofstream(file.path) <<
				"[general]\npersist_profile_selection: false\n"
				"[vprenderer.viewport.scope]\nscreen_aspect: 47:20\n"
				"[shader.standard]\ntype: multi\n"
				"[shader.standard.deband]\nshader_type: custom\n"
				"stage: pre_resize\nhlsl_file: Deband.hlsl\n"
				"[shader.standard.sharpen]\nshader_type: custom\n"
				"stage: pre_resize\nhlsl_file: Sharpen.hlsl\n"
				"[actions.do_it]\non: profile.viewport.changed\n"
				"run: C:\\Windows\\System32\\cmd.exe /c echo ${event}\n"
				"delay_seconds: 20\n"
				"[actions.draft]\nenabled: false\n";
			ConfigFile config;
			Assert::IsTrue(config.Load(file.path));
			UnifiedProfileRuntime::Runtime runtime;
			std::string error;
			Assert::IsTrue(runtime.Initialize(config, {}, error),
				std::wstring(error.begin(), error.end()).c_str());
			UnifiedProfileRuntime::SelectionResult selected;
			Assert::IsTrue(runtime.SelectProfile("viewport", "scope", true,
				{}, selected, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(selected.changed);
			Assert::AreEqual(std::string("scope"),
				selected.snapshot->viewport.profile);
			Assert::IsFalse(runtime.SelectProfile("viewport", "unknown", true,
				{}, selected, error));
			Assert::IsTrue(runtime.SelectProfile("standard_shaders", "deband",
				true, {}, selected, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(std::string("deband"),
				RendererProfileConfig::FormatSelection(
					selected.snapshot->effectiveSelections.at("standard_shaders")));
			Assert::IsTrue(runtime.SelectProfile("standard_shaders", "sharpen",
				true, {}, selected, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(std::string("deband|sharpen"),
				RendererProfileConfig::FormatSelection(
					selected.snapshot->effectiveSelections.at("standard_shaders")));
			Assert::IsTrue(runtime.SelectProfile("standard_shaders", "deband",
				false, {}, selected, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(std::string("sharpen"),
				RendererProfileConfig::FormatSelection(
					selected.snapshot->effectiveSelections.at("standard_shaders")));
			Assert::IsTrue(runtime.SelectProfile("standard_shaders", "sharpen",
				false, {}, selected, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(std::string("base"),
				RendererProfileConfig::FormatSelection(
					selected.snapshot->effectiveSelections.at("standard_shaders")));
			UnifiedProfileRuntime::ActionInvocation invocation;
			Assert::IsTrue(runtime.PrepareManualAction("Do_It", invocation, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(0, invocation.action.delaySeconds);
			Assert::IsTrue(invocation.action.arguments.find("manual") !=
				std::string::npos);
			Assert::IsFalse(runtime.PrepareManualAction("draft", invocation, error));
		}

        TEST_METHOD(UnifiedColorOutputMigrationPreservesEveryBaselineSettingAndArchivesSelectors)
        {
            using namespace ColorOutputProfileMigration;
            Settings baseline = {
                {"output_presentation","direct"}, {"output_range","limited"},
                {"output_transport_gamma","2.2"}, {"output_path_profile","custom"},
                {"output_diagnostics","true"}, {"diagnostic_disable_shader_cache","true"},
                {"diagnostic_disable_compute","true"}, {"diagnostic_force_8bit_sdr_swapchain","true"},
                {"diagnostic_allow_limited_g22","true"}, {"diagnostic_allow_full_g22","false"},
                {"diagnostic_vp_owned_dxgi_presenter","true"}, {"shortcut","F9"},
                {"when","${width} > 1"}, {"cycle_shortcut","F10"} };
            Sections sections = {{"vprenderer.output.first",baseline},
                {"vprenderer.output.second",{{"output_range","full"},{"shortcut","F11"}}},
                {"vprenderer.color.rec709",{{"output_gamma","2.2"},{"shortcut","F5"}}},
                {"vprenderer.color.bt2020",{{"sdr_target_primaries","bt2020"},{"shortcut","F6"}}}};
            std::vector<std::string> order = {"vprenderer.output.first","vprenderer.output.second",
                "vprenderer.color.rec709","vprenderer.color.bt2020"};
            Assert::IsTrue(Apply(sections, order));
            for (const auto* color : {"vprenderer.color.rec709","vprenderer.color.bt2020"})
                for (const auto& setting : baseline)
                    if (!IsSelector(setting.first)) Assert::AreEqual(setting.second,
                        sections.at(color).at(setting.first));
            Assert::AreEqual(std::string("F5"), sections.at("vprenderer.color.rec709").at("shortcut"));
            Assert::IsTrue(sections.at("legacy_output.first") == baseline);
            Assert::AreEqual(std::string("F11"), sections.at("legacy_output.second").at("shortcut"));
            Assert::IsFalse(Apply(sections, order));
            Sections only = {{"vprenderer.output",baseline}};
            std::vector<std::string> onlyOrder = {"vprenderer.output"};
            Assert::IsTrue(Apply(only, onlyOrder));
            Assert::AreEqual(std::string("limited"), only.at("vprenderer.color.default").at("output_range"));
        }

        TEST_METHOD(UnifiedColorOutputRuntimeMigrationRetainsRulesAndRejectsMalformedProfiles)
        {
            char directory[MAX_PATH] = {};
            Assert::IsTrue(GetTempPathA(ARRAYSIZE(directory), directory) > 0);
            const std::string path = std::string(directory) + "VP0174-runtime-migration.cfg";
            {
                std::ofstream file(path);
                file << "[vprenderer.color.Rec709]\noutput_gamma: 2.2\nshortcut: F5\n"
                    "[vprenderer.color.BT2020]\nwhen: ${width} > 1920\nsdr_target_primaries: bt2020\n"
                    "[vprenderer.output.First]\noutput_range: limited\noutput_transport_gamma: 2.2\n"
                    "diagnostic_allow_limited_g22: true\nshortcut: F9\n"
                    "[vprenderer.output.First_2]\noutput_range: full\n"
                    "[legacy_output.first]\noriginal: retained\n";
            }
            ConfigFile config;
            Assert::IsTrue(config.Load(path));
            RendererProfileConfig::Model model;
            std::string error;
            Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
                std::wstring(error.begin(), error.end()).c_str());
            Assert::IsTrue(RendererProfileConfig::ValidateOwnedSections(config, error),
                L"Startup rejected editor-compatible migrated Output archives");
            for (const auto& group : model.groups) Assert::IsTrue(group.name != "output");
            const auto& second = model.profiles.at("color.bt2020");
            Assert::AreEqual(std::string("limited"), second.settings.at("output_range"));
            Assert::AreEqual(std::string("2.2"), second.settings.at("output_gamma"));
            Assert::AreEqual(std::string("${width} > 1920"), second.when);
            Assert::IsTrue(config.HasSection("legacy_output.first_2"));
            Assert::IsTrue(config.HasSection("legacy_output.first_2_2"));
            Assert::AreEqual(std::string("retained"), config.GetSectionValues("legacy_output.first")->at("original"));
            {
                std::ofstream file(path);
                file << "[vprenderer.output.bad.nested]\noutput_range: limited\n";
            }
            Assert::IsTrue(config.Load(path));
            Assert::IsFalse(RendererProfileConfig::Read(config, model, error));
            DeleteFileA(path.c_str());
        }

        TEST_METHOD(StartupOwnershipAcceptsOldAndSavedOutputArchivesButRejectsUnknownSections)
        {
            char directory[MAX_PATH] = {};
            Assert::IsTrue(GetTempPathA(ARRAYSIZE(directory), directory) > 0);
            const std::string path = std::string(directory) + "VP0174-startup-compatibility.cfg";
            for (const char* section : {"vprenderer.output.Default", "legacy_output.default",
                "legacy_output", "legacy_output_2", "legacy_output.default_2"})
            {
                { std::ofstream file(path); file << "[vprenderer.color.Rec709]\noutput_gamma: 2.2\n["
                    << section << "]\noutput_range: limited\n"; }
                ConfigFile config;
                Assert::IsTrue(config.Load(path));
                std::string error;
                Assert::IsTrue(RendererProfileConfig::ValidateOwnedSections(config, error));
                RendererProfileConfig::Model model;
                Assert::IsTrue(RendererProfileConfig::Read(config, model, error));
            }
            // Only the reserved archive namespace is accepted, not arbitrary unknown owners.
            { std::ofstream file(path); file << "[unknown_settings]\nvalue: kept\n"; }
            ConfigFile config;
            Assert::IsTrue(config.Load(path));
            std::string error;
            Assert::IsFalse(RendererProfileConfig::ValidateOwnedSections(config, error));
            DeleteFileA(path.c_str());
        }

		TEST_METHOD(BackendInputSettingsRemainScoped)
		{
			char dir[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(MAX_PATH, dir) > 0);
			const std::string path = std::string(dir) + "vp0188-input.cfg";
			for (const char* key : { "video_conversion", "container_colorspace", "hdr_colorspace", "hdr_luminance" })
			{
				std::ofstream(path) << "[directshow]\n" << key << ": direct\n";
				ConfigFile config;
				Assert::IsTrue(config.Load(path));
				std::string value;
				Assert::IsTrue(BackendInputConfig::TryGet(config, false, key, value));
				Assert::AreEqual("direct", value.c_str());
				Assert::IsFalse(BackendInputConfig::TryGet(config, true, key, value));
				std::ofstream(path, std::ios::app) << "[general]\n" << key << ": shared\n";
				Assert::IsTrue(config.Load(path));
				Assert::IsTrue(BackendInputConfig::TryGet(config, true, key, value));
				Assert::AreEqual("shared", value.c_str());
				Assert::IsTrue(BackendInputConfig::TryGet(config, false, key, value));
				Assert::AreEqual("direct", value.c_str());
				std::ofstream(path, std::ios::app) << "[vprenderer.input_processing]\n" << key << ": vp\n";
				Assert::IsTrue(config.Load(path));
				Assert::IsTrue(BackendInputConfig::TryGet(config, true, key, value));
				Assert::AreEqual("vp", value.c_str());
			}
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(SharedRefreshPolicyDoesNotWarnAsLegacyRendererPolicy)
		{
			char dir[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(MAX_PATH, dir) > 0);
			const std::string path = std::string(dir) + "vp0188-refresh.cfg";
			std::ofstream(path) << "[general]\nswitch_refresh_rate: fullscreen_only\n";
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error, value;
			std::vector<std::string> warnings;
			Assert::IsTrue(RendererConfigView(config).Validate(error, warnings));
			Assert::IsTrue(warnings.empty());
			Assert::IsTrue(RendererConfigView(config).TryGetPolicyString("switch_refresh_rate", value));
			Assert::AreEqual("fullscreen_only", value.c_str());
			std::ofstream(path, std::ios::app) << "output_diagnostics: true\n";
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(RendererConfigView(config).Validate(error, warnings));
			Assert::IsFalse(warnings.empty());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(ProfileSnapshotsReuseAcceptedConfigurationUntilExplicitReload)
		{
			char dir[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(MAX_PATH, dir) > 0);
			const std::string path = std::string(dir) + "vp0188-snapshot.cfg";
			const std::string original = "[general]\npersist_profile_selection: false\n"
				"[vprenderer]\nquality: high\n";
			std::ofstream(path) << original;
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			UnifiedProfileRuntime::Runtime runtime;
			std::string error;
			Assert::IsTrue(runtime.Initialize(config, {}, error));
			const auto accepted = runtime.GetSnapshot()->configuration;
			Assert::IsTrue(accepted != nullptr);
			const auto reads = ConfigFile::GetLoadCount();
			// Remove the file entirely: source/profile evaluation must not need it.
			DeleteFileA(path.c_str());
			for (int n = 0; n < 20; ++n)
			{
				UnifiedProfileRuntime::RefreshResult result;
				Assert::IsTrue(runtime.Refresh({}, result, error));
				Assert::IsTrue(result.snapshot->configuration == accepted);
				UnifiedProfileRuntime::SelectionResult selection;
				Assert::IsTrue(runtime.SelectKey("F4", {}, selection, error));
				Assert::IsTrue(selection.snapshot->configuration == accepted);
			}
			Assert::AreEqual(reads, ConfigFile::GetLoadCount());
			std::ofstream(path) << "[general]\npersist_profile_selection: false\n[vprenderer]\nquality: balanced\n";
			Assert::IsTrue(config.Load(path));
			UnifiedProfileRuntime::RefreshResult reload;
			Assert::IsTrue(runtime.Reload(config, {}, reload, error));
			Assert::IsTrue(reload.snapshot->configuration != accepted);
			std::string quality;
			Assert::IsTrue(reload.snapshot->configuration->TryGetString("vprenderer", "quality", quality));
			Assert::AreEqual("balanced", quality.c_str());
			const auto updated = reload.snapshot->configuration;
			std::ofstream(path) << "[vprenderer]\nquality: invalid-quality\n";
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(runtime.Reload(config, {}, reload, error));
			Assert::IsTrue(runtime.GetSnapshot()->configuration == updated);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(VideoConversionOverrideAcceptsConfiguredDisabledValues)
		{
			VideoConversionOverride parsed =
				VideoConversionOverride::VIDEOCONVERSION_V210_TO_P010;
			Assert::IsTrue(TryParseVideoConversionOverride(TEXT("NONE"), parsed));
			Assert::IsTrue(parsed ==
				VideoConversionOverride::VIDEOCONVERSION_NONE);
			Assert::IsTrue(TryParseVideoConversionOverride(TEXT("off"), parsed));
			Assert::IsTrue(parsed ==
				VideoConversionOverride::VIDEOCONVERSION_NONE);
			Assert::IsTrue(TryParseVideoConversionOverride(
				TEXT("V210_TO_P010"), parsed));
			Assert::IsTrue(parsed ==
				VideoConversionOverride::VIDEOCONVERSION_V210_TO_P010);
			Assert::IsTrue(TryParseVideoConversionOverride(
				TEXT("uyvy_to_p010"), parsed));
			Assert::IsFalse(TryParseVideoConversionOverride(
				TEXT("NOT_A_CONVERSION"), parsed));
		}

		TEST_METHOD(ModernOperatorBuildIdentityUsesBranchForCleanBuildsAndCommitForDirtyBuilds)
		{
			Assert::AreEqual(std::wstring(L"v1.3.003-beta"),
				BuildIdentityPolicy::Format(
					L" v1.3.003-beta ", L"9a5dd0b",
					L"v1.1.016-beta-326-g9a5dd0b", false));
			Assert::AreEqual(std::wstring(L"ci/merge @ abc1234"),
				BuildIdentityPolicy::Format(L"ci/merge", L"abc1234", L"tag", true));
			Assert::AreEqual(std::wstring(L"v1.2.3-gabc1234"),
				BuildIdentityPolicy::Format(L"", L"abc1234",
					L"v1.2.3-gabc1234", false));
			Assert::AreEqual(std::wstring(L"v1.2.3-gabc1234 @ abc1234"),
				BuildIdentityPolicy::Format(L"", L"abc1234",
					L"v1.2.3-gabc1234", true));
			Assert::AreEqual(std::wstring(L"detached"),
				BuildIdentityPolicy::Format(L"", L"", L"", false));
		}

		TEST_METHOD(FullscreenActivationNeverStealsAnotherProcessForeground)
		{
			Assert::IsTrue(ConfigurationLiveApply::MayActivateFullscreen(
				42, 42, true));
			Assert::IsTrue(ConfigurationLiveApply::MayActivateFullscreen(
				42, 0, false));
			Assert::IsFalse(ConfigurationLiveApply::MayActivateFullscreen(
				42, 84, true));
			const auto editorPopup =
				ConfigurationLiveApply::ResolvePresentationFocus(42, 84, true);
			Assert::IsFalse(editorPopup.mayActivatePresentation);
			Assert::IsTrue(editorPopup.preserveForeground);
			Assert::IsFalse(editorPopup.consumeKeyboardMessage);
			const auto vpForeground =
				ConfigurationLiveApply::ResolvePresentationFocus(42, 42, true);
			Assert::IsTrue(vpForeground.mayActivatePresentation);
			Assert::IsFalse(vpForeground.preserveForeground);
			Assert::IsFalse(vpForeground.consumeKeyboardMessage);
			Assert::IsTrue(ConfigurationLiveApply::MayDispatchGlobalShortcut(
				42, 84, false));
			Assert::IsTrue(
				ConfigurationLiveApply::MayDispatchForegroundPresentationShortcut(
					true, true));
		}

		TEST_METHOD(ConfigurationEditorShortcutAlwaysStartsFreshReveal)
		{
			using Action = ConfigurationLiveApply::ConfigurationEditorToggleAction;
			Assert::IsTrue(Action::RevealOrActivate ==
				ConfigurationLiveApply::ResolveConfigurationEditorToggle(
					true, true, false));
			Assert::IsTrue(Action::RevealOrActivate ==
				ConfigurationLiveApply::ResolveConfigurationEditorToggle(
					true, false, false));
			Assert::IsTrue(Action::RevealOrActivate ==
				ConfigurationLiveApply::ResolveConfigurationEditorToggle(
					false, false, false));
			Assert::IsTrue(Action::RevealOrActivate ==
				ConfigurationLiveApply::ResolveConfigurationEditorToggle(
					false, false, true));
		}

		TEST_METHOD(ConfigurationEditorStaleHandleRediscoveryIsBounded)
		{
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldRediscoverConfigurationEditor(true, 0, false));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldRediscoverConfigurationEditor(true, 0, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldRediscoverConfigurationEditor(true, 1, false));
			Assert::IsTrue(ConfigurationLiveApply::
				ConfigurationEditorRevealAcknowledged(true, 1));
			Assert::IsFalse(ConfigurationLiveApply::
				ConfigurationEditorRevealAcknowledged(true, 0));
			Assert::IsTrue(ConfigurationLiveApply::
				ConfigurationEditorToggleAction::RevealOrActivate ==
				ConfigurationLiveApply::ResolveConfigurationEditorToggle(
					true, false, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ResolvePresentationFocus(42, 84, true).consumeKeyboardMessage);
		}

		TEST_METHOD(ConfigurationEditorRevealIntentSurvivesColdStartupTransitions)
		{
			using Outcome =
				ConfigurationLiveApply::ConfigurationEditorRevealOutcome;
			Assert::IsTrue(Outcome::Pending ==
				ConfigurationLiveApply::ResolveConfigurationEditorReveal(
					true, false, false, 1500, 20000));
			// A denied foreground fallback and arbitrary renderer/fullscreen
			// transitions do not alter the durable reveal state.
			Assert::IsTrue(Outcome::Pending ==
				ConfigurationLiveApply::ResolveConfigurationEditorReveal(
					true, false, false, 5000, 20000));
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldRetryRevealForAssociation(true, true));
			Assert::IsTrue(Outcome::Complete ==
				ConfigurationLiveApply::ResolveConfigurationEditorReveal(
					true, true, true, 7000, 20000));
			Assert::IsTrue(Outcome::Expired ==
				ConfigurationLiveApply::ResolveConfigurationEditorReveal(
					true, false, false, 20000, 20000));
			Assert::IsTrue(Outcome::Expired ==
				ConfigurationLiveApply::ResolveConfigurationEditorReveal(
					true, false, false, 10, 20000, true));
		}

		TEST_METHOD(ConfigurationEditorActivationAckBreaksReverseCallbackCycle)
		{
			Assert::IsFalse(ConfigurationLiveApply::
				AssociationPublicationMayBlockActivateHandler());
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldRetryConfigurationEditorActivate(
					true, true, 250, 1500, true));
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldRetryConfigurationEditorActivate(
					true, true, 1500, 1500, true));
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldRetryConfigurationEditorActivate(
					true, false, 0, 1500, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldRetryConfigurationEditorActivate(
					false, false, 0, 1500, true));
		}

		TEST_METHOD(SessionRendererSelectionSurvivesLifecyclePublication)
		{
			using Source =
				ConfigurationLiveApply::RendererPublicationSource;
			auto decision = ConfigurationLiveApply::ResolveRendererPublication(
				L"DirectShow - madVR", L"VP Renderer", L"DirectShow - madVR",
				false, true);
			Assert::AreEqual(std::wstring(L"VP Renderer"), decision.renderer);
			Assert::IsTrue(Source::SessionOverride == decision.source);

			decision = ConfigurationLiveApply::ResolveRendererPublication(
				L"VP Renderer", L"DirectShow - madVR", L"VP Renderer",
				false, true);
			Assert::AreEqual(std::wstring(L"DirectShow - madVR"),
				decision.renderer);
			Assert::IsTrue(Source::SessionOverride == decision.source);

			decision = ConfigurationLiveApply::ResolveRendererPublication(
				L"VP Renderer", L"DirectShow - madVR", L"DirectShow - madVR",
				false, false);
			Assert::AreEqual(std::wstring(L"DirectShow - madVR"),
				decision.renderer);
			Assert::IsTrue(Source::SessionOverride == decision.source);

			decision = ConfigurationLiveApply::ResolveRendererPublication(
				L"VP Renderer", L"DirectShow - madVR", L"DirectShow - madVR",
				true, true);
			Assert::AreEqual(std::wstring(L"VP Renderer"), decision.renderer);
			Assert::IsTrue(Source::SavedConfiguration == decision.source);

			decision = ConfigurationLiveApply::ResolveRendererPublication(
				L"VP Renderer", L"", L"DirectShow - madVR", false, false);
			Assert::AreEqual(std::wstring(L"DirectShow - madVR"),
				decision.renderer);
			Assert::IsTrue(Source::AcceptedRuntime == decision.source);
		}

		TEST_METHOD(GlobalShortcutPolicyAllowsBareKeysOutsideConfig)
		{
			Assert::IsTrue(ConfigurationLiveApply::MayDispatchGlobalShortcut(
				42, 84, false));
			Assert::IsFalse(ConfigurationLiveApply::MayDispatchGlobalShortcut(
				42, 42, false));
			Assert::IsFalse(ConfigurationLiveApply::MayDispatchGlobalShortcut(
				42, 84, true));
			Assert::IsTrue(
				ConfigurationLiveApply::MayDispatchBackgroundAccelerator(
					false, false, false));
			Assert::IsTrue(
				ConfigurationLiveApply::MayDispatchBackgroundAccelerator(
					true, false, false));
			Assert::IsTrue(
				ConfigurationLiveApply::MayDispatchBackgroundAccelerator(
					false, true, false));
			Assert::IsTrue(
				ConfigurationLiveApply::MayDispatchBackgroundAccelerator(
					false, false, true));
			// Delta 6, global_hotkeys off: only chords that cannot be typing
			// act while another application has focus.
			Assert::IsTrue(ConfigurationLiveApply::IsGlobalSafeShortcut(true, false, false, 'I'));
			Assert::IsTrue(ConfigurationLiveApply::IsGlobalSafeShortcut(false, true, false, 0x0D));
			Assert::IsTrue(ConfigurationLiveApply::IsGlobalSafeShortcut(false, false, false, 0x74));   // F5
			Assert::IsTrue(ConfigurationLiveApply::IsGlobalSafeShortcut(false, false, true, 0x71));    // Shift+F2
			Assert::IsFalse(ConfigurationLiveApply::IsGlobalSafeShortcut(false, false, false, 'R'));
			Assert::IsFalse(ConfigurationLiveApply::IsGlobalSafeShortcut(false, false, true, 'L'));    // a capital L
			Assert::IsFalse(ConfigurationLiveApply::IsGlobalSafeShortcut(false, false, false, 0x1B));  // Esc
			Assert::IsTrue(ConfigurationLiveApply::ShortcutModifiersMatch(
				true, false, true, true, false, true));
			Assert::IsFalse(ConfigurationLiveApply::ShortcutModifiersMatch(
				true, false, true, true, false, false));
			Assert::IsTrue(ConfigurationLiveApply::MayDispatchInjectedShortcut(
				true, 42, 84));
			Assert::IsFalse(ConfigurationLiveApply::MayDispatchInjectedShortcut(
				false, 42, 84));
			Assert::IsFalse(ConfigurationLiveApply::MayDispatchInjectedShortcut(
				true, 42, 42));
		}

		TEST_METHOD(ModernBackgroundAndConfigurationModalPoliciesAreFailSafe)
		{
			Assert::IsTrue(
				ConfigurationLiveApply::ShouldEnableBackgroundShortcuts(true, false, false));
			Assert::IsFalse(
				ConfigurationLiveApply::ShouldEnableBackgroundShortcuts(false, false, false));
			Assert::IsFalse(
				ConfigurationLiveApply::ShouldEnableBackgroundShortcuts(true, true, false));
			Assert::IsFalse(
				ConfigurationLiveApply::ShouldEnableBackgroundShortcuts(true, false, true));
			Assert::IsTrue(ConfigurationLiveApply::ShouldRequestPresentationFocus(
				true, false, true));
			Assert::IsFalse(ConfigurationLiveApply::ShouldRequestPresentationFocus(
				false, false, true));
			Assert::IsFalse(ConfigurationLiveApply::ShouldRequestPresentationFocus(
				true, true, true));
			Assert::IsTrue(ConfigurationLiveApply::ShouldReturnPresentationFocus(
				true, true, true));
			Assert::IsFalse(ConfigurationLiveApply::ShouldReturnPresentationFocus(
				false, true, true));
			Assert::IsTrue(
				ConfigurationLiveApply::ShouldSuppressFullscreenTopmost(false, true, false));
			Assert::IsTrue(
				ConfigurationLiveApply::ShouldSuppressFullscreenTopmost(false, false, true));
			Assert::IsTrue(ConfigurationLiveApply::MayEnterConfigurationModal(
				true, false, 84, 84));
			Assert::IsFalse(ConfigurationLiveApply::MayEnterConfigurationModal(
				true, false, 84, 42));
			Assert::IsFalse(ConfigurationLiveApply::MayEnterConfigurationModal(
				true, true, 84, 84));
			Assert::IsTrue(ConfigurationLiveApply::IsDuplicateBackgroundShortcut(
				33200, 33200, 100, 250));
			Assert::IsFalse(ConfigurationLiveApply::IsDuplicateBackgroundShortcut(
				33200, 33201, 100, 250));
			Assert::IsFalse(ConfigurationLiveApply::IsDuplicateBackgroundShortcut(
				33200, 33200, 251, 250));
			Assert::IsTrue(
				ConfigurationLiveApply::MayDispatchWhileConfigurationModal(
					true, true));
			Assert::IsTrue(
				ConfigurationLiveApply::MayDispatchWhileConfigurationModal(
					true, false));
			Assert::IsFalse(
				ConfigurationLiveApply::ShouldPromoteFullscreenAfterLiveFrame(
					true, true, false, false));
			Assert::IsTrue(
				ConfigurationLiveApply::ShouldPromoteFullscreenAfterLiveFrame(
					true, true, true, false));
			Assert::IsFalse(
				ConfigurationLiveApply::ShouldPromoteFullscreenAfterLiveFrame(
					true, true, true, true));
			Assert::IsTrue(
				ConfigurationLiveApply::ShouldRecreateFullscreenHostForBackendHandoff(
					true, false, true, true, false));
			Assert::IsFalse(
				ConfigurationLiveApply::ShouldRecreateFullscreenHostForBackendHandoff(
					true, true, true, true, false));
		}

		TEST_METHOD(AlphaHostReconstructionRevealsOnlyCurrentGenerationAndTarget)
		{
			using ConfigurationLiveApply::
				ShouldRetireWaitingSurfaceAfterLiveFrame;
			constexpr uintptr_t windowedTarget = 0x1001;
			constexpr uintptr_t fullscreenTarget = 0x2002;

			Assert::IsFalse(ShouldRetireWaitingSurfaceAfterLiveFrame(
				3, windowedTarget, 4, fullscreenTarget, 4, fullscreenTarget,
				true, true, true, false, false));
			Assert::IsFalse(ShouldRetireWaitingSurfaceAfterLiveFrame(
				4, windowedTarget, 4, fullscreenTarget, 4, fullscreenTarget,
				true, true, true, false, false));
			Assert::IsTrue(ShouldRetireWaitingSurfaceAfterLiveFrame(
				4, fullscreenTarget, 4, fullscreenTarget, 4, fullscreenTarget,
				true, true, true, false, false));

			// Editor visibility suppresses focus promotion, never retirement of a
			// verified current waiting surface.
			Assert::IsTrue(ShouldRetireWaitingSurfaceAfterLiveFrame(
				4, fullscreenTarget, 4, fullscreenTarget, 4, fullscreenTarget,
				true, true, true, false, true));
			Assert::IsFalse(
				ConfigurationLiveApply::ShouldPromoteFullscreenAfterLiveFrame(
					true, true, true, true));

			// The inverse fullscreen-to-windowed reconstruction uses the same
			// generation/target contract.
			Assert::IsFalse(ShouldRetireWaitingSurfaceAfterLiveFrame(
				4, fullscreenTarget, 5, windowedTarget, 5, windowedTarget,
				true, true, true, false, false));
			Assert::IsTrue(ShouldRetireWaitingSurfaceAfterLiveFrame(
				5, windowedTarget, 5, windowedTarget, 5, windowedTarget,
				true, true, true, false, false));
		}

#if 0
		TEST_METHOD(ConfigurationEditorPresentationLeaseUsesActualWindowZOrder)
		{
			struct Windows
			{
				HWND host = nullptr;
				HWND editor = nullptr;
				HWND popup = nullptr;
				~Windows()
				{
					if (popup) DestroyWindow(popup);
					if (editor) DestroyWindow(editor);
					if (host) DestroyWindow(host);
				}
			} windows;
			windows.host = CreateWindowExW(WS_EX_TOPMOST, L"STATIC",
				L"VP fullscreen test host", WS_POPUP | WS_VISIBLE,
				0, 0, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
			windows.editor = CreateWindowExW(0, L"STATIC",
				L"VP configuration test editor", WS_POPUP | WS_VISIBLE,
				16, 16, 160, 90, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
			Assert::IsNotNull(windows.host);
			Assert::IsNotNull(windows.editor);
			windows.popup = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
				L"VP configuration combo popup", WS_POPUP | WS_VISIBLE,
				32, 32, 120, 80, windows.editor, nullptr,
				GetModuleHandleW(nullptr), nullptr);
			Assert::IsNotNull(windows.popup);
			std::vector<RECT> monitorRects;
			EnumDisplayMonitors(nullptr, nullptr,
				[](HMONITOR, HDC, LPRECT rect, LPARAM context) -> BOOL
				{
					reinterpret_cast<std::vector<RECT>*>(context)->push_back(*rect);
					return TRUE;
				}, reinterpret_cast<LPARAM>(&monitorRects));
			if (monitorRects.size() > 1)
			{
				const RECT& second = monitorRects[1];
				SetWindowPos(windows.host, nullptr, second.left, second.top,
					320, 180, SWP_NOACTIVATE | SWP_NOZORDER);
			}
			RECT hostRectBefore = {};
			GetWindowRect(windows.host, &hostRectBefore);
			const LONG_PTR hostStyleBefore =
				GetWindowLongPtrW(windows.host, GWL_STYLE);
			constexpr bool fullscreenRequested = true;
			constexpr uint32_t rendererGeneration = 17;
			const uintptr_t selectedTarget = ConfigurationLiveApply::
				SelectConfigurationEditorPresentationTarget(true,
					reinterpret_cast<uintptr_t>(windows.host), 0, 0);
			Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.host),
				selectedTarget);
			Assert::AreEqual(
				reinterpret_cast<uintptr_t>(MonitorFromWindow(windows.host,
					MONITOR_DEFAULTTONEAREST)),
				reinterpret_cast<uintptr_t>(MonitorFromWindow(
					reinterpret_cast<HWND>(selectedTarget),
					MONITOR_DEFAULTTONEAREST)));
			Assert::IsTrue(ConfigurationLiveApply::
				IsValidatedPresentationTargetProcess(42, 42, true));
			Assert::IsFalse(ConfigurationLiveApply::
				IsValidatedPresentationTargetProcess(42, 84, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ConfigurationEditorRuntimeUsesRecurringLease());
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldRequestConfigurationEditorOneShotReassert(true, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldRequestConfigurationEditorOneShotReassert(false, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldRequestConfigurationEditorOneShotReassert(true, false));
			SetActiveWindow(windows.editor);
			const bool editorBecameForeground =
				SetForegroundWindow(windows.editor) != FALSE;

			Assert::IsTrue(ConfigurationEditorZOrder::Apply(
				windows.host, windows.editor));
			Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.editor),
				reinterpret_cast<uintptr_t>(GetActiveWindow()));
			if (editorBecameForeground)
				Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.editor),
					reinterpret_cast<uintptr_t>(GetForegroundWindow()));
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldRestoreConfigurationEditorForeground(true, 42, 42));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldRestoreConfigurationEditorForeground(true, 42, 84));
			Assert::IsTrue(ConfigurationEditorZOrder::IsAbove(
				windows.editor, windows.host));
			Assert::IsTrue((GetWindowLongPtrW(
				windows.editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
			Assert::IsTrue((GetWindowLongPtrW(
				windows.host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
			RECT hostRectAfter = {};
			GetWindowRect(windows.host, &hostRectAfter);
			Assert::IsTrue(EqualRect(&hostRectBefore, &hostRectAfter) != FALSE);
			Assert::AreEqual(hostStyleBefore,
				GetWindowLongPtrW(windows.host, GWL_STYLE));
			Assert::IsTrue(fullscreenRequested);
			Assert::AreEqual<uint32_t>(17, rendererGeneration);

			// A same-process combo popup owns Config interaction. Lease maintenance
			// must not collapse it back to the main window; only a VP foreground
			// takeover causes the main editor to be reasserted.
			SetActiveWindow(windows.popup);
			const LONG_PTR editorExStyleBeforeWatchdog =
				GetWindowLongPtrW(windows.editor, GWL_EXSTYLE);
			const LONG_PTR popupExStyleBeforeWatchdog =
				GetWindowLongPtrW(windows.popup, GWL_EXSTYLE);
			const bool popupAboveEditorBeforeWatchdog =
				ConfigurationEditorZOrder::IsAbove(
					windows.popup, windows.editor);
			for (int tick = 0; tick < 5; ++tick)
			{
				Assert::IsTrue(ConfigurationEditorZOrder::
					MaintainPresentationHost(windows.host));
			}
			Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.popup),
				reinterpret_cast<uintptr_t>(GetActiveWindow()));
			Assert::AreEqual(editorExStyleBeforeWatchdog,
				GetWindowLongPtrW(windows.editor, GWL_EXSTYLE));
			Assert::AreEqual(popupExStyleBeforeWatchdog,
				GetWindowLongPtrW(windows.popup, GWL_EXSTYLE));
			Assert::AreEqual(popupAboveEditorBeforeWatchdog,
				ConfigurationEditorZOrder::IsAbove(
					windows.popup, windows.editor));
			SetActiveWindow(windows.host);
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldRestoreConfigurationEditorForeground(true, 42, 42));
			SetActiveWindow(windows.editor);
			Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.editor),
				reinterpret_cast<uintptr_t>(GetActiveWindow()));

			// Simulate renderer reconstruction or the delayed fullscreen pass
			// attempting to reassert the presentation host.
			SetWindowPos(windows.host, HWND_TOPMOST, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			Assert::IsTrue(ConfigurationEditorZOrder::Apply(
				windows.host, windows.editor));
			Assert::IsTrue(ConfigurationEditorZOrder::IsAbove(
				windows.editor, windows.host));
			Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.editor),
				reinterpret_cast<uintptr_t>(GetActiveWindow()));
			if (editorBecameForeground)
				Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.editor),
					reinterpret_cast<uintptr_t>(GetForegroundWindow()));

			ShowWindow(windows.host, SW_MINIMIZE);
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldExecuteDeferredPresentationActivation(
					true, true, true, true, true, true));
			ShowWindow(windows.editor, SW_HIDE);
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldExecuteDeferredPresentationActivation(
					true, false, true, true, true, true));
			ShowWindow(windows.host, SW_RESTORE);
			SetActiveWindow(windows.host);
			Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.host),
				reinterpret_cast<uintptr_t>(GetActiveWindow()));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldExecuteDeferredPresentationActivation(
					false, false, true, true, true, true));

			Assert::IsTrue(ConfigurationEditorZOrder::Release(windows.editor));
			Assert::IsFalse((GetWindowLongPtrW(
				windows.editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
			RECT hostRectAfterHide = {};
			GetWindowRect(windows.host, &hostRectAfterHide);
			Assert::IsTrue(EqualRect(&hostRectBefore, &hostRectAfterHide) != FALSE);
			Assert::AreEqual(hostStyleBefore,
				GetWindowLongPtrW(windows.host, GWL_STYLE));
			Assert::IsTrue((GetWindowLongPtrW(
				windows.host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
			Assert::AreEqual<uint32_t>(17, rendererGeneration);
		}
#endif

		TEST_METHOD(ConfigurationEditorUsesNativeOwnerWithoutRuntimePolling)
		{
			struct Windows
			{
				HWND host = nullptr;
				HWND editor = nullptr;
				HWND popup = nullptr;
				~Windows()
				{
					if (popup) DestroyWindow(popup);
					if (editor) DestroyWindow(editor);
					if (host) DestroyWindow(host);
				}
			} windows;
			windows.host = CreateWindowExW(WS_EX_TOPMOST, L"STATIC",
				L"VP presentation owner", WS_POPUP | WS_VISIBLE,
				0, 0, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
			windows.editor = CreateWindowExW(0, L"STATIC", L"Config",
				WS_POPUP | WS_VISIBLE, 16, 16, 160, 90,
				nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
			Assert::IsNotNull(windows.host);
			Assert::IsNotNull(windows.editor);
			SetWindowLongPtrW(windows.editor, GWLP_HWNDPARENT,
				reinterpret_cast<LONG_PTR>(windows.host));
			windows.popup = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
				L"Config combo", WS_POPUP | WS_VISIBLE, 32, 32, 120, 80,
				windows.editor, nullptr, GetModuleHandleW(nullptr), nullptr);
			Assert::IsNotNull(windows.popup);

			Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.host),
				reinterpret_cast<uintptr_t>(GetWindow(windows.editor, GW_OWNER)));
			Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.editor),
				reinterpret_cast<uintptr_t>(GetWindow(windows.popup, GW_OWNER)));
			Assert::IsFalse(ConfigurationLiveApply::
				ConfigurationEditorRuntimeUsesRecurringLease());

			RECT hostBefore = {};
			GetWindowRect(windows.host, &hostBefore);
			const LONG_PTR hostStyle = GetWindowLongPtrW(windows.host, GWL_STYLE);
			const LONG_PTR editorExStyle =
				GetWindowLongPtrW(windows.editor, GWL_EXSTYLE);
			const LONG_PTR popupExStyle =
				GetWindowLongPtrW(windows.popup, GWL_EXSTYLE);
			SetActiveWindow(windows.popup);
			for (int formerWatchdogTick = 0; formerWatchdogTick < 5;
				++formerWatchdogTick)
			{
				// Native-owner architecture intentionally performs no runtime call.
			}
			Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.popup),
				reinterpret_cast<uintptr_t>(GetActiveWindow()));
			Assert::AreEqual(editorExStyle,
				GetWindowLongPtrW(windows.editor, GWL_EXSTYLE));
			Assert::AreEqual(popupExStyle,
				GetWindowLongPtrW(windows.popup, GWL_EXSTYLE));
			RECT hostAfter = {};
			GetWindowRect(windows.host, &hostAfter);
			Assert::IsTrue(EqualRect(&hostBefore, &hostAfter) != FALSE);
			Assert::AreEqual(hostStyle,
				GetWindowLongPtrW(windows.host, GWL_STYLE));
			Assert::AreEqual(reinterpret_cast<uintptr_t>(windows.host),
				ConfigurationLiveApply::SelectConfigurationEditorPresentationTarget(
					true, reinterpret_cast<uintptr_t>(windows.host), 0, 0));
			Assert::IsTrue(ConfigurationLiveApply::
				IsValidatedPresentationTargetProcess(42, 42, true));
			Assert::IsFalse(ConfigurationLiveApply::
				IsValidatedPresentationTargetProcess(42, 84, true));
		}

		TEST_METHOD(ConfigurationBoundaryDefersRendererForNewerCaptureState)
		{
			Assert::IsTrue(ConfigurationLiveApply::
				MayConstructRendererAfterConfigurationBoundary(41, 41));
			Assert::IsFalse(ConfigurationLiveApply::
				MayConstructRendererAfterConfigurationBoundary(41, 42));
			Assert::IsFalse(ConfigurationLiveApply::
				MayConstructRendererAfterConfigurationBoundary(0, 1));
		}

		TEST_METHOD(ConfigurationEditorOwnerSurvivesMissingOrChangingRenderer)
		{
			constexpr uintptr_t host = 0x1000;
			Assert::AreEqual(static_cast<uintptr_t>(0), ConfigurationLiveApply::
				SelectConfigurationEditorNativeOwner(host, 0));
			Assert::AreEqual(static_cast<uintptr_t>(0), ConfigurationLiveApply::
				SelectConfigurationEditorNativeOwner(host, 0x2000));
			Assert::AreEqual(static_cast<uintptr_t>(0), ConfigurationLiveApply::
				SelectConfigurationEditorNativeOwner(host, 0x3000));
		}

		TEST_METHOD(FreshRendererConsumesLatchedRestartIntent)
		{
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldConsumeRestartForFreshRenderer(false, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldConsumeRestartForFreshRenderer(true, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldConsumeRestartForFreshRenderer(false, false));
		}

        TEST_METHOD(FrameOffsetAutoAndOmissionUseStableDefault)
        {
            for (const char* raw : { "", "auto", "0", "90", "135" })
            {
                bool automatic = false;
                int ms = -1;
                Assert::IsTrue(ConfigurationLiveApply::ParseFrameOffset(raw, automatic, ms));
                const bool isAuto = std::string(raw).empty() || std::string(raw) == "auto";
                Assert::AreEqual(isAuto, automatic);
                Assert::AreEqual(isAuto ? 90 : std::stoi(raw), ms);
            }
            Assert::AreEqual(90, ConfigurationLiveApply::AutomaticFrameOffsetMs(false));
            Assert::AreEqual(0, ConfigurationLiveApply::AutomaticFrameOffsetMs(true));
            for (const char* raw : { "-1", "90oops", "2147483648" })
            {
                bool automatic = true; int ms = 90;
                Assert::IsFalse(ConfigurationLiveApply::ParseFrameOffset(raw, automatic, ms));
                Assert::IsTrue(automatic); Assert::AreEqual(90, ms);
            }
        }

        TEST_METHOD(NamedBaseIdentityAndRootCollision)
        {
            for (const char* root : { "vprenderer", "vprenderer.color" })
            {
                CachedConfigTestFile file;
                const std::string setting = std::string(root) == "vprenderer" ? "quality: fast\n" : "output_range: limited\n";
                std::ofstream(file.path) << "[" << root << ".BaSe]\n" << setting
                    << "[" << root << ".Child]\n" << setting;
                ConfigFile config; Assert::IsTrue(config.Load(file.path));
                RendererProfileConfig::Model model; std::string error;
                Assert::IsTrue(RendererProfileConfig::Read(config, model, error), std::wstring(error.begin(),error.end()).c_str());
                Assert::AreEqual((std::string(root)+".base").c_str(),
                    ActiveProfileStatus::SectionFor(root, "base", &config).c_str());
                Assert::AreEqual((std::string(root)+".base").c_str(),
                    ProfileSectionIdentity::Resolve(config,root,"BASE").c_str());
                std::ofstream(file.path) << "[" << root << "]\n" << setting << "[" << root << ".Base]\n" << setting;
                Assert::IsTrue(config.Load(file.path, ConfigFile::ReadPolicy::Fresh));
                Assert::IsFalse(RendererProfileConfig::Read(config, model, error));
                Assert::IsTrue(error.find("rename") != std::string::npos);
            }
        }

        TEST_METHOD(PpmNumericSentinelDoesNotEnableAutomaticMode)
        {
            for (const char* raw : { "auto", "999999", "0", "-12" })
            {
                CachedConfigTestFile file;
                std::ofstream(file.path) << "[directshow.ppm]\nppm: " << raw << "\n";
                ConfigFile config; Assert::IsTrue(config.Load(file.path));
                PPMCorrectionLoader loader; Assert::IsTrue(loader.LoadCorrections(config));
                for (double rate : { 23.976, 50.0, 59.94 })
                {
                    const bool automatic = std::string(raw) == "auto";
                    Assert::AreEqual(automatic, loader.IsAutomatic(rate));
                    Assert::AreEqual(automatic ? 0 : std::stoi(raw), loader.GetPPMCorrection(rate));
                }
                loader.Clear(); Assert::IsFalse(loader.IsAutomatic(60));
            }
        }

		TEST_METHOD(OmittedHardwareSelectionsUseFirstDiscoveredValues)
		{
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldSelectFirstDiscoveredValue(false, -1, 2));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldSelectFirstDiscoveredValue(true, -1, 2));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldSelectFirstDiscoveredValue(false, 0, 2));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldSelectFirstDiscoveredValue(false, -1, 0));
		}

		TEST_METHOD(FailedReloadRestoresAcceptedRendererSelection)
		{
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldRestoreAcceptedRendererAfterReload(false, true, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldRestoreAcceptedRendererAfterReload(true, true, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldRestoreAcceptedRendererAfterReload(false, false, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldRestoreAcceptedRendererAfterReload(false, true, false));
		}

		TEST_METHOD(ShortcutTableStagesWithoutRendererRestart)
		{
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldStageShortcutTable(true, false));
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldStageShortcutTable(false, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldStageShortcutTable(false, false));
		}

		TEST_METHOD(ConfigurationPublicationPreservesTransientPresentation)
		{
			const auto windowedVideoOnly = ConfigurationLiveApply::
				PreserveSessionPresentation(true, false);
			Assert::IsTrue(windowedVideoOnly.videoOnly);
			Assert::IsFalse(windowedVideoOnly.fullscreen);

			const auto fullscreenWithUi = ConfigurationLiveApply::
				PreserveSessionPresentation(false, true);
			Assert::IsFalse(fullscreenWithUi.videoOnly);
			Assert::IsTrue(fullscreenWithUi.fullscreen);
			Assert::IsFalse(ConfigurationLiveApply::
				SessionPresentationControlIsPersistent());
		}

		TEST_METHOD(FullscreenToggleUsesRequestedAndEffectiveState)
		{
			using Action = ConfigurationLiveApply::FullscreenToggleAction;
			using Direction = ConfigurationLiveApply::
				FullscreenTransitionDirection;
			const auto pendingRequest = ConfigurationLiveApply::
				ResolveFullscreenToggle(true, false, Direction::None);
			const auto pendingEnter = ConfigurationLiveApply::
				ResolveFullscreenToggle(true, false, Direction::Entering);
			const auto pendingExit = ConfigurationLiveApply::
				ResolveFullscreenToggle(false, false, Direction::Exiting);
			const auto active = ConfigurationLiveApply::
				ResolveFullscreenToggle(true, true, Direction::None);
			const auto inactive = ConfigurationLiveApply::
				ResolveFullscreenToggle(false, false, Direction::None);
			Assert::IsTrue(pendingRequest == Action::ExitFullscreen);
			Assert::IsTrue(pendingEnter == Action::CancelPending);
			Assert::IsTrue(pendingExit == Action::EnterFullscreen);
			Assert::IsTrue(active == Action::ExitFullscreen);
			Assert::IsTrue(inactive == Action::EnterFullscreen);
			Assert::IsFalse(ConfigurationLiveApply::
				FullscreenRequestedAfterToggle(pendingEnter));
			Assert::IsFalse(ConfigurationLiveApply::
				FullscreenRequestedAfterToggle(active));
			Assert::IsTrue(ConfigurationLiveApply::
				FullscreenRequestedAfterToggle(inactive));
			Assert::IsFalse(ConfigurationLiveApply::
				EffectiveFullscreenToggleActive(true, false));
			Assert::IsTrue(ConfigurationLiveApply::
				EffectiveFullscreenToggleActive(false, true));
			Assert::IsFalse(ConfigurationLiveApply::
				EffectiveFullscreenToggleActive(false, false));
		}

		TEST_METHOD(RightAltFullscreenAndModifiedEnterAreFailSafe)
		{
			Assert::IsTrue(ConfigurationLiveApply::
				FullscreenShortcutModifiersMatch(
					false, true, false, false, true, false, false));
			Assert::IsTrue(ConfigurationLiveApply::
				FullscreenShortcutModifiersMatch(
					false, true, false, true, true, false, true));
			Assert::IsFalse(ConfigurationLiveApply::
				FullscreenShortcutModifiersMatch(
					false, true, false, true, true, false, false));
			Assert::IsTrue(ConfigurationLiveApply::
				FullscreenShortcutModifiersMatch(
					true, true, false, true, true, false, true));
			Assert::IsFalse(ConfigurationLiveApply::
				FullscreenShortcutModifiersMatch(
					false, true, true, true, true, false, true));

			Assert::IsTrue(ConfigurationLiveApply::
				ShouldConsumeUnmatchedModifiedEnter(
					true, 0x0d, true, true, false));
			Assert::IsTrue(ConfigurationLiveApply::
				ShouldConsumeUnmatchedModifiedEnter(
					true, 0x0d, false, false, true));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldConsumeUnmatchedModifiedEnter(
					true, 0x0d, false, false, false));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldConsumeUnmatchedModifiedEnter(
					false, 0x0d, true, true, false));
			Assert::IsFalse(ConfigurationLiveApply::
				ShouldConsumeUnmatchedModifiedEnter(
					true, 'R', true, true, false));
		}

		TEST_METHOD(VideoOnlyRetainsForegroundEscapeShortcuts)
		{
			Assert::AreEqual(static_cast<int>('U'), static_cast<int>(
				ConfigurationLiveApply::VideoOnlyToggleDefaultKey));
			Assert::AreEqual(0x0c, static_cast<int>(
				ConfigurationLiveApply::VideoOnlyToggleDefaultModifiers));
			Assert::AreEqual(0x0d, static_cast<int>(
				ConfigurationLiveApply::ViewToggleDefaultKey));
			Assert::AreEqual(0x10, static_cast<int>(
				ConfigurationLiveApply::ViewToggleDefaultModifiers));
			Assert::IsTrue(ConfigurationLiveApply::
				MayDispatchForegroundPresentationShortcut(true, true));
			Assert::IsFalse(ConfigurationLiveApply::
				MayDispatchForegroundPresentationShortcut(false, true));
		}

		TEST_METHOD(WindowCloseAlwaysAdvancesSafeShutdown)
		{
			Assert::IsTrue(ApplicationShutdownPolicy::
				IsCloseSystemCommand(0xf060));
			Assert::IsTrue(ApplicationShutdownPolicy::
				IsCloseSystemCommand(0xf061));
			Assert::IsFalse(ApplicationShutdownPolicy::
				IsCloseSystemCommand(0xf020));
			Assert::IsTrue(ApplicationShutdownPolicy::
				MayFinalizeCaptureAfterStopReturns(true, true));
			Assert::IsFalse(ApplicationShutdownPolicy::
				MayFinalizeCaptureAfterStopReturns(false, true));
			Assert::IsFalse(ApplicationShutdownPolicy::
				MayFinalizeCaptureAfterStopReturns(true, false));

			using Source = ApplicationShutdownPolicy::CloseSource;
			using Lifecycle = ApplicationShutdownPolicy::Lifecycle;
			for (const Source source : {
				Source::MainDialog, Source::OperatorView,
				Source::WindowedVideoHost, Source::FullscreenHost,
				Source::RendererSurface, Source::StatsOverlay,
				Source::OwnedTopLevel })
			{
				for (const Lifecycle lifecycle : {
					Lifecycle::Running, Lifecycle::StoppingRenderer,
					Lifecycle::RetiringRenderer, Lifecycle::Terminating })
				{
					const auto decision = ApplicationShutdownPolicy::
						ResolveClose(source, lifecycle);
					Assert::IsTrue(decision.routeToCoordinator);
					Assert::IsTrue(decision.consumeOriginal);
					Assert::IsTrue(decision.preserveSourceSurface);
					Assert::AreEqual(
						lifecycle == Lifecycle::Terminating,
						decision.advanceExistingTermination);
				}
			}
			Assert::IsTrue(ApplicationShutdownPolicy::
				IsAltF4(0x0104, 0x73, true));
			Assert::IsFalse(ApplicationShutdownPolicy::
				IsAltF4(0x0104, 0x73, false));
			Assert::IsFalse(ApplicationShutdownPolicy::
				IsAltF4(0x0104, 'R', true));
		}

		TEST_METHOD(ApplicationInterfaceDefaultsToModern)
		{
			const auto selection = ApplicationInterface::Resolve(
				false, {}, {});
			Assert::IsTrue(selection.mode == ApplicationInterface::Mode::Modern);
			Assert::IsTrue(selection.source ==
				ApplicationInterface::Source::DefaultModern);
			Assert::IsTrue(selection.warning.empty());
		}

		TEST_METHOD(ApplicationInterfaceConfigurationSelectsEitherUi)
		{
			const auto classic = ApplicationInterface::Resolve(false, {},
				ApplicationInterface::ParsePreference(
					true, " CLASSIC ", "configured"));
			const auto modern = ApplicationInterface::Resolve(false, {},
				ApplicationInterface::ParsePreference(
					true, "MoDeRn", "configured"));
			Assert::IsTrue(classic.mode == ApplicationInterface::Mode::Classic);
			Assert::IsTrue(modern.mode == ApplicationInterface::Mode::Modern);
			Assert::IsTrue(modern.source ==
				ApplicationInterface::Source::Configuration);
		}

		TEST_METHOD(ApplicationInterfaceCommandLineOverridesConfiguration)
		{
			const auto commandLine = ApplicationInterface::ParseCommandLine(
				{ L"VideoProcessor.exe", L"/InTeRfAcE", L"modern" });
			const auto configuration = ApplicationInterface::ParsePreference(
				true, "classic", "configured");
			const auto selection = ApplicationInterface::Resolve(
				false, commandLine, configuration);
			Assert::IsTrue(selection.mode == ApplicationInterface::Mode::Modern);
			Assert::IsTrue(selection.source ==
				ApplicationInterface::Source::CommandLine);
		}

		TEST_METHOD(ApplicationInterfaceInvalidCommandLineFallsBackToConfiguration)
		{
			const auto commandLine = ApplicationInterface::ParseCommandLine(
				{ L"VideoProcessor.exe", L"/interface", L"future" });
			const auto configuration = ApplicationInterface::ParsePreference(
				true, "modern", "configured");
			const auto selection = ApplicationInterface::Resolve(
				false, commandLine, configuration);
			Assert::IsTrue(selection.mode == ApplicationInterface::Mode::Modern);
			Assert::IsTrue(selection.source ==
				ApplicationInterface::Source::Configuration);
			Assert::IsTrue(selection.warning.find("expected classic or modern") !=
				std::string::npos);
		}

		TEST_METHOD(ApplicationInterfaceMissingCommandLineValueFallsBackToModern)
		{
			const auto commandLine = ApplicationInterface::ParseCommandLine(
				{ L"VideoProcessor.exe", L"/interface", L"/fullscreen" });
			const auto selection = ApplicationInterface::Resolve(
				false, commandLine, {});
			Assert::IsTrue(selection.mode == ApplicationInterface::Mode::Modern);
			Assert::IsTrue(selection.warning.find("missing value") !=
				std::string::npos);
		}

		TEST_METHOD(ApplicationInterfaceNoUiAlwaysWins)
		{
			const auto commandLine = ApplicationInterface::ParseCommandLine(
				{ L"VideoProcessor.exe", L"/interface", L"modern" });
			const auto selection = ApplicationInterface::Resolve(
				true, commandLine, ApplicationInterface::ParsePreference(
					true, "classic", "configured"));
			Assert::IsTrue(selection.mode == ApplicationInterface::Mode::None);
			Assert::IsTrue(selection.source == ApplicationInterface::Source::NoUi);
		}

		TEST_METHOD(ApplicationInterfaceValuesAreCaseInsensitiveAndTrimmed)
		{
			const auto classic = ApplicationInterface::ParsePreference(
				true, "  ClAsSiC\t", "configured");
			const auto modern = ApplicationInterface::ParseCommandLine(
				{ L"VideoProcessor.exe", L"/INTERFACE", L"MoDeRn" });
			Assert::IsTrue(classic.valid);
			Assert::IsTrue(classic.mode == ApplicationInterface::Mode::Classic);
			Assert::IsTrue(modern.valid);
			Assert::IsTrue(modern.mode == ApplicationInterface::Mode::Modern);
		}

		TEST_METHOD(ApplicationInterfaceDuplicateOptionWarnsAndUsesConfiguration)
		{
			const auto commandLine = ApplicationInterface::ParseCommandLine(
				{ L"VideoProcessor.exe", L"/interface", L"modern",
				  L"/interface", L"classic" });
			const auto selection = ApplicationInterface::Resolve(false,
				commandLine, ApplicationInterface::ParsePreference(
					true, "modern", "configured"));
			Assert::IsFalse(commandLine.valid);
			Assert::IsTrue(selection.mode == ApplicationInterface::Mode::Modern);
			Assert::IsTrue(selection.source ==
				ApplicationInterface::Source::Configuration);
			Assert::IsFalse(selection.warning.empty());
		}

		TEST_METHOD(ApplicationInterfaceNoUiSuppressesInvalidSelection)
		{
			const auto commandLine = ApplicationInterface::ParseCommandLine(
				{ L"VideoProcessor.exe", L"/interface", L"future" });
			const auto selection = ApplicationInterface::Resolve(true,
				commandLine, ApplicationInterface::ParsePreference(
					true, "modern", "configured"));
			Assert::IsTrue(selection.mode == ApplicationInterface::Mode::None);
			Assert::IsTrue(selection.source == ApplicationInterface::Source::NoUi);
			Assert::IsFalse(selection.warning.empty());
		}

		TEST_METHOD(ModernOperatorLayoutMatchesApprovedDefaultGeometry)
		{
			const auto layout = ModernOperatorLayout::Calculate(1600, 671);
			Assert::AreEqual(16, layout.information.x);
			Assert::AreEqual(70, layout.information.y);
			Assert::AreEqual(512, layout.information.width);
			Assert::AreEqual(544, layout.preview.x);
			Assert::AreEqual(70, layout.preview.y);
			Assert::AreEqual(1040, layout.preview.width);
			Assert::AreEqual(585, layout.preview.height);
			Assert::AreEqual(655,
				layout.preview.y + layout.preview.height);
		}

		TEST_METHOD(ModernOperatorLayoutScalesApprovedGeometryWithDpi)
		{
			const auto layout = ModernOperatorLayout::Calculate(2400, 1007, 144);
			Assert::AreEqual(24, layout.information.x);
			Assert::AreEqual(105, layout.information.y);
			Assert::AreEqual(768, layout.information.width);
			Assert::AreEqual(820, layout.preview.x);
			Assert::AreEqual(107, layout.preview.y);
			Assert::AreEqual(1552, layout.preview.width);
			Assert::AreEqual(873, layout.preview.height);
		}

		TEST_METHOD(ModernOperatorHeaderControlsAreCompactAndDpiAligned)
		{
			const auto normal = ModernOperatorLayout::
				CalculateHeaderControls(1600);
			Assert::AreEqual(16, 1600 -
				(normal.configuration.x + normal.configuration.width));
			Assert::AreEqual(8, normal.configuration.x -
				(normal.fullscreen.x + normal.fullscreen.width));
			Assert::AreEqual(8, normal.fullscreen.x -
				(normal.videoOnly.x + normal.videoOnly.width));
			Assert::AreEqual(32, normal.configuration.width);
			Assert::AreEqual(90, normal.fullscreen.width);
			Assert::AreEqual(86, normal.videoOnly.width);

			const auto highDpi = ModernOperatorLayout::
				CalculateHeaderControls(2400, 144);
			Assert::AreEqual(24, 2400 -
				(highDpi.configuration.x + highDpi.configuration.width));
			Assert::AreEqual(12, highDpi.configuration.x -
				(highDpi.fullscreen.x + highDpi.fullscreen.width));
			Assert::AreEqual(highDpi.configuration.y, highDpi.fullscreen.y);
			Assert::AreEqual(highDpi.fullscreen.y, highDpi.videoOnly.y);
		}

		TEST_METHOD(ModernCapturedVideoNeverLeaksTemplatePlaceholders)
		{
			for (const wchar_t* placeholder : {
				L"", L"<display mode>", L"<pixel format>",
				L"<color space>", L"<colorspace>", L"<eotf>",
				L"<encoding>", L"<bd>" })
			Assert::AreEqual(std::wstring(L"---"),
				ModernOperatorStatusPolicy::NormalizeCapturedValue(placeholder));
			Assert::AreEqual(std::wstring(L"3840x2160p - 4K UHDTV"),
				ModernOperatorStatusPolicy::NormalizeCapturedValue(
					L"3840x2160p - 4K UHDTV"));
			Assert::AreEqual(std::wstring(L"P010"),
				ModernOperatorStatusPolicy::NormalizeCapturedValue(L"P010"));
			Assert::AreEqual(std::wstring(L"BT.2020"),
				ModernOperatorStatusPolicy::NormalizeCapturedValue(L"BT.2020"));
			Assert::AreEqual(std::wstring(L"PQ"),
				ModernOperatorStatusPolicy::NormalizeCapturedValue(L"PQ"));
		}

		TEST_METHOD(ModernOperatorPreviewRemainsSixteenByNineWhenResized)
		{
			const auto layout = ModernOperatorLayout::Calculate(1900, 900);
			Assert::IsTrue(layout.preview.x >= 544);
			Assert::IsTrue(layout.preview.y >= 70);
			Assert::IsTrue(layout.preview.x + layout.preview.width <= 1884);
			Assert::IsTrue(layout.preview.y + layout.preview.height <= 884);
			Assert::AreEqual(
				layout.preview.width * 9,
				layout.preview.height * 16);
		}

		TEST_METHOD(ModernOperatorPreviewStaysBoundedAtTwoHundredPercentDpi)
		{
			const auto layout = ModernOperatorLayout::Calculate(3200, 1342, 192);
			Assert::AreEqual(layout.preview.width * 9,
				layout.preview.height * 16);
			Assert::IsTrue(layout.preview.x >= 1088);
			Assert::IsTrue(layout.preview.y >= 140);
			Assert::IsTrue(layout.preview.x + layout.preview.width <= 3168);
			Assert::IsTrue(layout.preview.y + layout.preview.height <= 1310);
		}

		TEST_METHOD(NoUiVideoGeometryIsSixteenByNineAtDefaultAndMinimum)
		{
			Assert::AreEqual(NoUiLayout::DefaultClientWidth * 9,
				NoUiLayout::DefaultClientHeight * 16);
			Assert::AreEqual(NoUiLayout::MinimumClientWidth * 9,
				NoUiLayout::MinimumClientHeight * 16);
			Assert::AreEqual(960, NoUiLayout::DefaultClientWidth);
			Assert::AreEqual(540, NoUiLayout::DefaultClientHeight);
			Assert::AreEqual(320, NoUiLayout::MinimumClientWidth);
			Assert::AreEqual(180, NoUiLayout::MinimumClientHeight);
		}

		TEST_METHOD(VideoOnlyFillsClientWithoutChangingOuterWindowBounds)
		{
			const ModernOperatorLayout::Rect outer = { 37, 52, 1600, 900 };
			const auto video = NoUiLayout::FillClient(1584, 839);
			Assert::AreEqual(0, video.x);
			Assert::AreEqual(0, video.y);
			Assert::AreEqual(1584, video.width);
			Assert::AreEqual(839, video.height);
			Assert::IsTrue(NoUiLayout::PreservesOuterBounds(outer, outer));
			Assert::IsFalse(NoUiLayout::PreservesOuterBounds(
				outer, { 37, 52, 960, 540 }));
		}

		TEST_METHOD(VideoOnlyImmediatelyReflowsVideoBoundsInBothDirections)
		{
			const ModernOperatorLayout::Rect outer = { 37, 52, 1600, 900 };
			const ModernOperatorLayout::Rect operatorPreview = {
				544, 70, 1040, 585 };
			const auto videoOnly = NoUiLayout::ResolveVideoBounds(
				true, 1600, 839, operatorPreview);
			const auto operatorView = NoUiLayout::ResolveVideoBounds(
				false, 1600, 839, operatorPreview);
			Assert::AreEqual(0, videoOnly.x);
			Assert::AreEqual(0, videoOnly.y);
			Assert::AreEqual(1600, videoOnly.width);
			Assert::AreEqual(839, videoOnly.height);
			Assert::AreEqual(operatorPreview.x, operatorView.x);
			Assert::AreEqual(operatorPreview.y, operatorView.y);
			Assert::AreEqual(operatorPreview.width, operatorView.width);
			Assert::AreEqual(operatorPreview.height, operatorView.height);
			Assert::IsTrue(NoUiLayout::PreservesOuterBounds(outer, outer));
		}

		TEST_METHOD(DeckLinkEightBitYuvUsesHdycOnlyForRec709)
		{
			Assert::AreEqual(static_cast<int>(VideoFrameEncoding::HDYC),
				static_cast<int>(Translate(
					bmdFormat8BitYUV, ColorSpace::REC_709)));
			for (const ColorSpace colorSpace : {
				ColorSpace::UNKNOWN, ColorSpace::REC_601_525,
				ColorSpace::REC_601_576, ColorSpace::REC_601_625,
				ColorSpace::BT_2020 })
			{
				Assert::AreEqual(static_cast<int>(VideoFrameEncoding::UYVY),
					static_cast<int>(Translate(bmdFormat8BitYUV, colorSpace)));
			}
			Assert::IsTrue(IsEqualGUID(
				TranslateToMediaSubType(VideoFrameEncoding::HDYC),
				MEDIASUBTYPE_HDYC));
			Assert::IsTrue(IsEqualGUID(
				TranslateToMediaSubType(VideoFrameEncoding::UYVY),
				MEDIASUBTYPE_UYVY));
		}

		TEST_METHOD(DeckLinkCapturePackingDefaultsRemainCanonical)
		{
			const DeckLinkCaptureFormatPreferences preferences;
			const auto rgb8 = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputRGB444 | bmdDetectedVideoInput8BitDepth);
			const auto rgb10 = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputRGB444 | bmdDetectedVideoInput10BitDepth);
			const auto rgb12 = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputRGB444 | bmdDetectedVideoInput12BitDepth);
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat8BitARGB),
				static_cast<unsigned int>(PreferredDeckLinkCapturePixelFormat(
					rgb8, preferences)));
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat10BitRGB),
				static_cast<unsigned int>(PreferredDeckLinkCapturePixelFormat(
					rgb10, preferences)));
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat12BitRGB),
				static_cast<unsigned int>(PreferredDeckLinkCapturePixelFormat(
					rgb12, preferences)));
		}

		TEST_METHOD(DeckLinkCapturePackingConfigurationExposesEveryRgbVariant)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-decklink-packing.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[decklink]\n"
					"rgb_8bit_packing: BGRA\n"
					"rgb_10bit_packing: R10L\n"
					"rgb_12bit_packing: R12L\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error));
			DeckLinkCaptureFormatPreferences preferences;
			Assert::IsTrue(ReadDeckLinkCaptureFormatPreferences(
				config, preferences, error));
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat8BitBGRA),
				static_cast<unsigned int>(preferences.rgb8));
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat10BitRGBXLE),
				static_cast<unsigned int>(preferences.rgb10));
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat12BitRGBLE),
				static_cast<unsigned int>(preferences.rgb12));
			Assert::AreEqual(static_cast<int>(VideoFrameEncoding::BGRA_8BIT),
				static_cast<int>(Translate(preferences.rgb8, ColorSpace::REC_709)));
			Assert::AreEqual(static_cast<int>(VideoFrameEncoding::R10l),
				static_cast<int>(Translate(preferences.rgb10, ColorSpace::REC_709)));
			Assert::AreEqual(static_cast<int>(VideoFrameEncoding::R12L),
				static_cast<int>(Translate(preferences.rgb12, ColorSpace::REC_709)));
			const auto rgb8 = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputRGB444 | bmdDetectedVideoInput8BitDepth);
			const auto rgb10 = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputRGB444 | bmdDetectedVideoInput10BitDepth);
			const auto rgb12 = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputRGB444 | bmdDetectedVideoInput12BitDepth);
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat8BitBGRA),
				static_cast<unsigned int>(PreferredDeckLinkCapturePixelFormat(
					rgb8, preferences)));
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat10BitRGBXLE),
				static_cast<unsigned int>(PreferredDeckLinkCapturePixelFormat(
					rgb10, preferences)));
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat12BitRGBLE),
				static_cast<unsigned int>(PreferredDeckLinkCapturePixelFormat(
					rgb12, preferences)));

			// The other 10-bit alternative is independently accepted.
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[decklink]\nrgb_10bit_packing: R10B\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(ReadDeckLinkCaptureFormatPreferences(
				config, preferences, error));
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat10BitRGBX),
				static_cast<unsigned int>(preferences.rgb10));
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(DeckLinkCapturePackingFallsBackWhenDeviceRejectsPreference)
		{
			DeckLinkCaptureFormatPreferences preferences;
			preferences.rgb8 = bmdFormat8BitBGRA;
			preferences.rgb10 = bmdFormat10BitRGBX;
			preferences.rgb12 = bmdFormat12BitRGBLE;
			const auto rgb10 = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputRGB444 | bmdDetectedVideoInput10BitDepth);

			bool usedFallback = false;
			BMDPixelFormat selected = ResolveDeckLinkCapturePixelFormat(
				rgb10, preferences,
				[](BMDPixelFormat format)
					{ return format == bmdFormat10BitRGBX; }, usedFallback);
			Assert::IsFalse(usedFallback);
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat10BitRGBX),
				static_cast<unsigned int>(selected));

			selected = ResolveDeckLinkCapturePixelFormat(rgb10, preferences,
				[](BMDPixelFormat) { return false; }, usedFallback);
			Assert::IsTrue(usedFallback);
			Assert::AreEqual(static_cast<unsigned int>(bmdFormat10BitRGB),
				static_cast<unsigned int>(selected));
		}

		TEST_METHOD(DeckLinkCapturePackingSchemaRejectsInvalidDepthVariant)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-decklink-invalid-packing.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[decklink]\nrgb_8bit_packing: R12L\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("rgb_8bit_packing") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(IndexedShortcutKeyParsesOneBasedIndex)
		{
			unsigned int index = 0;
			Assert::IsTrue(ConfigFile::TryParseIndexedKey(
				"Render.12",
				"render",
				index));
			Assert::AreEqual(12u, index);
		}

		TEST_METHOD(LegacyRendererPolicyDefaultsToHiddenAndAcceptsExplicitFalse)
		{
			ConfigFile missingConfiguration;
			Assert::IsTrue(DirectShowHideLegacyRenderers(missingConfiguration));

			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path =
				std::string(temporaryDirectory) + "VideoProcessor-legacy-renderer-policy-test.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nhide_legacy_renderers: false\n";
			}

			ConfigFile configuration;
			Assert::IsTrue(configuration.Load(path));
			Assert::IsFalse(DirectShowHideLegacyRenderers(configuration));
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(LegacyRendererPolicyFallsBackToHiddenForMalformedValue)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path =
				std::string(temporaryDirectory) + "VideoProcessor-legacy-renderer-invalid-policy-test.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nhide_legacy_renderers: unexpected\n";
			}

			ConfigFile configuration;
			Assert::IsTrue(configuration.Load(path));
			Assert::IsTrue(DirectShowHideLegacyRenderers(configuration));
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(IndexedShortcutKeyRejectsZeroAndMalformedValues)
		{
			unsigned int index = 99;
			Assert::IsFalse(ConfigFile::TryParseIndexedKey(
				"render.0",
				"render",
				index));
			Assert::IsFalse(ConfigFile::TryParseIndexedKey(
				"render.external",
				"render",
				index));
			Assert::IsFalse(ConfigFile::TryParseIndexedKey(
				"renderer.1",
				"render",
				index));
		}

		TEST_METHOD(IndexedShortcutKeyRejectsOverflow)
		{
			unsigned int index = 0;
			Assert::IsFalse(ConfigFile::TryParseIndexedKey(
				"render.999999999999999999999999999",
				"render",
				index));
		}

		TEST_METHOD(DuplicateKeyWarnsAndLastValueWins)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path =
				std::string(temporaryDirectory) + "VideoProcessor-duplicate-config-test.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[display]\ncontrast_recovery: 0\ncontrast_recovery: AUTO\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string value;
			Assert::IsTrue(config.TryGetString("display", "contrast_recovery", value));
			Assert::AreEqual("AUTO", value.c_str());
			Assert::AreEqual(static_cast<size_t>(1), config.GetWarnings().size());
			Assert::IsTrue(
				config.GetWarnings().front().find("duplicate [display] key 'contrast_recovery'") !=
				std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(DisplayRuleLutValuesPreserveBasePathAndBlankOverride)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path =
				std::string(temporaryDirectory) + "VideoProcessor-display-lut-config-test.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[display]\n"
					"lut: lut\\base-calibration.cube\n"
					"[display_rules.rec709]\n"
					"lut:\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string baseLut;
			std::string ruleLut;
			Assert::IsTrue(config.TryGetString("display", "lut", baseLut));
			Assert::IsTrue(config.TryGetString("display_rules.rec709", "lut", ruleLut));
			Assert::AreEqual("lut\\base-calibration.cube", baseLut.c_str());
			Assert::IsTrue(ruleLut.empty());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(CommandLineConfigOptionAcceptsSeparateAndEqualsValues)
		{
			std::string value;
			std::string error;
			Assert::IsTrue(ConfigFile::TryParseCommandLineOption(
				{ "VideoProcessor.exe", "--config", "C:\\VP Test\\main.cfg" },
				"--config",
				value,
				error));
			Assert::AreEqual("C:\\VP Test\\main.cfg", value.c_str());
			Assert::IsTrue(error.empty());

			Assert::IsTrue(ConfigFile::TryParseCommandLineOption(
				{ "VideoProcessor.exe", "--vr_config=renderer-test.cfg" },
				"--vr_config",
				value,
				error));
			Assert::AreEqual("renderer-test.cfg", value.c_str());
			Assert::IsTrue(error.empty());
		}

		TEST_METHOD(CommandLineConfigOptionIsCaseInsensitive)
		{
			std::string value;
			std::string error;
			Assert::IsTrue(ConfigFile::TryParseCommandLineOption(
				{ "VideoProcessor.exe", "/VR_CONFIG", "renderer.cfg" },
				"--vr_config",
				value,
				error));
			Assert::AreEqual("renderer.cfg", value.c_str());
			Assert::IsTrue(error.empty());
		}

		TEST_METHOD(CommandLineConfigOptionRejectsMissingValue)
		{
			std::string value;
			std::string error;
			Assert::IsFalse(ConfigFile::TryParseCommandLineOption(
				{ "VideoProcessor.exe", "--vr_config", "--fullscreen" },
				"--vr_config",
				value,
				error));
			Assert::IsTrue(error.find("Missing value") != std::string::npos);
		}

		TEST_METHOD(RendererConfigSelectionDefaultsToPrimaryConfig)
		{
			std::string filename;
			std::string error;
			bool explicitSelection = true;
			bool compatibilityOverride = true;
			Assert::IsTrue(ConfigFile::TryResolveRendererConfigSelection(
				{ "VideoProcessor.exe" },
				filename,
				explicitSelection,
				compatibilityOverride,
				error));
			Assert::AreEqual(
				ConfigFile::DEFAULT_FILENAME, filename.c_str());
			Assert::IsFalse(explicitSelection);
			Assert::IsFalse(compatibilityOverride);
			Assert::IsTrue(error.empty());
		}

		TEST_METHOD(RendererConfigSelectionFollowsPrimaryConfig)
		{
			std::string filename;
			std::string error;
			bool explicitSelection = false;
			bool compatibilityOverride = true;
			Assert::IsTrue(ConfigFile::TryResolveRendererConfigSelection(
				{ "VideoProcessor.exe", "--config", "combined.cfg" },
				filename,
				explicitSelection,
				compatibilityOverride,
				error));
			Assert::AreEqual("combined.cfg", filename.c_str());
			Assert::IsTrue(explicitSelection);
			Assert::IsFalse(compatibilityOverride);
		}

		TEST_METHOD(RendererConfigSelectionExplicitOverrideWins)
		{
			std::string filename;
			std::string error;
			bool explicitSelection = false;
			bool compatibilityOverride = false;
			Assert::IsTrue(ConfigFile::TryResolveRendererConfigSelection(
				{ "VideoProcessor.exe", "--config=combined.cfg",
				  "--vr_config", "renderer.cfg" },
				filename,
				explicitSelection,
				compatibilityOverride,
				error));
			Assert::AreEqual("renderer.cfg", filename.c_str());
			Assert::IsTrue(explicitSelection);
			Assert::IsTrue(compatibilityOverride);
		}

		TEST_METHOD(CommandLineConfigOptionRejectsDuplicates)
		{
			std::string value;
			std::string error;
			Assert::IsFalse(ConfigFile::TryParseCommandLineOption(
				{
					"VideoProcessor.exe",
					"--config", "first.cfg",
					"--config=second.cfg"
				},
				"--config",
				value,
				error));
			Assert::IsTrue(error.find("Duplicate") != std::string::npos);
		}

		TEST_METHOD(ColonAssignmentsPreserveEqualityExpressions)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-colon-config-test.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[profile]\n"
					"when: $key==\"F5\"\n"
					"quality: high\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string value;
			Assert::IsTrue(config.TryGetString("profile", "when", value));
			Assert::AreEqual("$key==\"F5\"", value.c_str());
			Assert::IsTrue(config.TryGetString("profile", "quality", value));
			Assert::AreEqual("high", value.c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(LegacyEqualsAssignmentsRemainReadable)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-equals-compatibility-test.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[command_line]\nqueue_size=32\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string value;
			Assert::IsTrue(config.TryGetString(
				"command_line", "queue_size", value));
			Assert::AreEqual("32", value.c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(MainConfigSchemaUsesSharedTypedValidation)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-main-schema-valid.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[command_line]\n"
					"renderer: VideoProcessor Renderer (Alpha)\n"
					"queue_size: 32\n"
					"fullscreen: true\n"
					"disable_detection_features: true\n"
					"scene_correction_basic: false\n"
					"frame_offset: AUTO\n"
					"[queue]\n"
					"queue_size: 32\n"
					"lead_frames: 1\n"
					"target_frames: 2\n"
					"[queue_recovery]\n"
					"reset_after_render_restart_seconds: 3\n"
					"reset_queue_too_large_percent: 70\n"
					"[lldv]\n"
					"max_cll: 1000\n"
					"mastering_min_luminance: 0.001\n"
					"mastering_max_luminance: 4000\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.empty());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(GeneralRecoveryToggleDefaultsBoundedAndValidatesBothModes)
		{
			ConfigFile config;
			Assert::IsTrue(MainConfigSchema::BoundedInvalidCaptureRecoveryEnabled(config));
			Assert::IsTrue(MainConfigSchema::OwnsSection("general"));
			char directory[MAX_PATH] = {}, path[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(directory), directory) > 0);
			Assert::IsTrue(GetTempFileNameA(directory, "vpi", 0, path) != 0);
			for (const char* value : { "false", "true", "invalid" })
			{
				{
					std::ofstream file(path, std::ios::out | std::ios::trunc);
					file << "[general]\nbounded_invalid_capture_recovery: " << value << "\n";
				}
				Assert::IsTrue(config.Load(path));
				std::string error;
				const bool valid = std::string(value) != "invalid";
				Assert::AreEqual(valid, MainConfigSchema::Validate(config, error));
				if (valid)
					Assert::AreEqual(std::string(value) == "true",
						MainConfigSchema::BoundedInvalidCaptureRecoveryEnabled(config));
				if (!valid)
					Assert::IsTrue(error.find("bounded_invalid_capture_recovery") != std::string::npos);
			}
			DeleteFileA(path);
		}

		TEST_METHOD(MainConfigSchemaValidatesTargetOnlyDisplaySessionMode)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-display-session-schema.cfg";
			ConfigFile config;
			std::string error;
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\n"
					"fullscreen_monitor_name: EPSON PJ\n"
					"fullscreen_monitor_session_mode: target-only\n"
					"[vprenderer]\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(MainConfigSchema::Validate(config, error));
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\n"
					"fullscreen_monitor_session_mode: dangerous\n"
					"[vprenderer]\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("fullscreen_monitor_session_mode") !=
				std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(DisplayRecoveryDetectionIsStateFileScoped)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-display-recovery-detection.state";
			DeleteFileA(path.c_str());
			Assert::IsFalse(
				DisplayTopologySession::HasPendingRecovery(path));
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "# Managed by VideoProcessor.\n"
					"profile.viewport: scope\n"
					"display_recovery.v1: DEADBEEF\n";
			}
			Assert::IsTrue(
				DisplayTopologySession::HasPendingRecovery(path));
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(MainAndRendererSchemasAcceptOneCombinedConfig)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-combined-schema.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[command_line]\n"
					"renderer: VideoProcessor Renderer (Alpha)\n"
					"[shortcuts]\nrender.6: A\n"
					"[general]\npersist_profile_selection: true\n"
					"[vpvr.display]\nquality: high\n"
					"[vpvr.general]\nswitch_refresh_rate: true\n";
				for (const char* group :
					{ "input", "scaling", "display", "viewport" })
				{
					file << "[profile_groups." << group << "]\n"
						"profiles: base\n"
						"default: base\n"
						"[profiles." << group << ".base]\n";
				}
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error));
			RendererProfileConfig::Model model;
			Assert::IsTrue(
				RendererProfileConfig::Read(config, model, error));
			Assert::AreEqual(static_cast<size_t>(4), model.groups.size());
			Assert::IsTrue(model.warnings.empty());
			Assert::IsTrue(MainConfigSchema::OwnsSection("command_line"));
			Assert::IsTrue(MainConfigSchema::OwnsSection("queue"));
			Assert::IsTrue(MainConfigSchema::OwnsSection("directshow"));
			Assert::IsTrue(RendererProfileConfig::OwnsSection(
				"profiles.display.base"));
			Assert::IsFalse(MainConfigSchema::OwnsSection("unknown"));
			Assert::IsFalse(RendererProfileConfig::OwnsSection("unknown"));
			Assert::AreEqual(
				(path.substr(0, path.size() - 4) + ".state").c_str(),
				RendererProfileConfig::StatePath(config).c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0079OwnerVariantsPersistAndRestoreProfileState)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0079-config.cfg";
			const std::string statePath = path.substr(0, path.size() - 4) +
				".state";
			DeleteFileA(statePath.c_str());
			DeleteFileA((statePath + ".tmp").c_str());
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\npersist_profile_selection: true\n"
					"renderer: VideoProcessor Renderer (Alpha)\n"
					"fullscreen: true\n"
					"[renderer_alias]\nvp: 1\nmadvr: 2\n"
					"[queue]\nwhen: $key==\"l\"\nqueue_size: 32\nlead_frames: 4\ntarget_frames: 3\nactive_picture_lookahead_frames: 2\n"
					"[queue.low_latency]\nwhen: $key==\"Shift+L\"\nqueue_size: 1\ntarget_frames: 1\n"
					"[directshow]\nvideo_conversion: V210_TO_P010\nframe_offset: 90\n"
					"[directshow.conversion]\nconversion_method: SIMD\nmin_core_count: 1\nmax_core_count: 2\n"
					"[directshow.ppm]\nppm: -17\n"
					"[vprenderer]\nwhen: $key==\"F4\"\nquality: high\nswitch_refresh_rate: true\n"
					"[vprenderer.rec709]\nwhen: $key==\"F5\"\ntone_mapping: spline\n"
					"[vprenderer.viewport]\nwhen: $key==\"F3\"\n"
					"[vprenderer.viewport.scope]\nwhen: $key==\"F2\"\nscreen_aspect: 2.1:1\nvertical_alignment: TOP\nautomatic_crop: true\nsubtitle_fit: true\n"
					"[actions.audio_delay_film]\non: refresh.applied,refresh.confirmed\nwhen: $actual_refresh<=30\nrun: C:\\Videoprocessor\\audio\\audio_delay.bat 100\n"
					"[shader.nls]\nwhen: $key==\"n\"\n"
					"[shader.nls.standard]\nwhen: $key==\"Shift+n\"\nshader_type: nls\nglsl_file: NLS.glsl\n"
					"[shader.cleanup]\ntype: multi\nwhen: $key==\"d\"\n"
					"[shader.cleanup.deband]\nwhen: $key==\"Shift+D\"\nshader_type: custom\nhlsl_file: Deband.hlsl\nstage: pre_resize\norder: 30\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(model.persistSelection);
			Assert::AreEqual(static_cast<size_t>(4), model.groups.size());

			std::vector<RendererProfileConfig::KeySelection> selections;
			Assert::IsTrue(RendererProfileConfig::SelectForKey(model, "Shift+L",
				[](const std::string&, std::string&) { return false; },
				selections, error));
			Assert::AreEqual(static_cast<size_t>(1), selections.size());
			Assert::AreEqual("queue", selections.front().group.c_str());
			Assert::AreEqual("low_latency", selections.front().profiles.front().c_str());

			Assert::IsTrue(RendererProfileConfig::SelectForKey(model, "F2",
				[](const std::string&, std::string&) { return false; },
				selections, error));
			Assert::AreEqual("viewport", selections.front().group.c_str());
			Assert::AreEqual("scope", selections.front().profiles.front().c_str());
			RendererProfileConfig::ResolvedViewport viewport;
			Assert::IsTrue(RendererProfileConfig::ResolveViewport(model,
				"scope", 1, viewport, error));
			Assert::AreEqual(21ull, viewport.screenAspect.numerator);
			Assert::AreEqual(10ull, viewport.screenAspect.denominator);
			Assert::IsTrue(viewport.hasScreenAspect);
			Assert::AreEqual("top", viewport.verticalAlignment.c_str());
			Assert::IsTrue(viewport.automaticCrop);
			Assert::IsFalse(viewport.subtitleFit); // Retired Screen setting is ignored.

			UnifiedProfileRuntime::Runtime runtime;
			Assert::IsTrue(runtime.Initialize(config,
				[](const std::string&, std::string&) { return false; }, error));
			Assert::AreEqual(statePath.c_str(), runtime.StatePath().c_str());
			UnifiedProfileRuntime::SelectionResult result;
			Assert::IsTrue(runtime.SelectKey("L",
				[](const std::string&, std::string&) { return false; },
				result, error));
			Assert::IsTrue(result.changed);
			Assert::IsTrue(result.snapshot->queue.hasQueueSize);
			Assert::AreEqual(static_cast<size_t>(32),
				result.snapshot->queue.queueSize);

			Assert::IsTrue(runtime.SelectKey("F2",
				[](const std::string&, std::string&) { return false; },
				result, error));
			Assert::IsTrue(result.changed);
			Assert::AreEqual("scope", result.snapshot->viewport.profile.c_str());

			std::ifstream state(statePath);
			const std::string stateContents(
				(std::istreambuf_iterator<char>(state)),
				std::istreambuf_iterator<char>());
			Assert::IsTrue(stateContents.find("profile.viewport: scope") !=
				std::string::npos);

			ConfigFile restoredConfig;
			Assert::IsTrue(restoredConfig.Load(path));
			UnifiedProfileRuntime::Runtime restoredRuntime;
			Assert::IsTrue(restoredRuntime.Initialize(restoredConfig,
				[](const std::string&, std::string&) { return false; }, error));
			Assert::AreEqual("scope",
				restoredRuntime.GetSnapshot()->viewport.profile.c_str());
			DeleteFileA(path.c_str());
			DeleteFileA(statePath.c_str());
			DeleteFileA((statePath + ".tmp").c_str());
		}

		TEST_METHOD(UnifiedQueueProfileRestartPolicyUsesCommittedLatestQueueSelection)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-queue-profile-restart.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\n"
					"when: $key==\"F1\"\n"
					"queue_size: 32\n"
					"[queue.low_latency]\n"
					"when: $key==\"F2\"\n"
					"queue_size: 4\n"
					"[vprenderer.viewport]\n"
					"screen_aspect: 16:9\n"
					"[vprenderer.viewport.scope]\n"
					"when: $key==\"F3\"\n"
					"screen_aspect: 2.35:1\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			UnifiedProfileRuntime::Runtime runtime;
			Assert::IsTrue(runtime.Initialize(config,
				[](const std::string&, std::string&) { return false; }, error),
				std::wstring(error.begin(), error.end()).c_str());

			UnifiedProfileRuntime::SelectionResult queueSelection;
			Assert::IsTrue(runtime.SelectKey("F2",
				[](const std::string&, std::string&) { return false; },
				queueSelection, error));
			Assert::IsTrue(queueSelection.changed);
			Assert::IsTrue(runtime.GetSnapshot() == queueSelection.snapshot);
			Assert::IsTrue(QueueProfileRestartPolicy::
				RequiresResetAfterManualSelection(true,
					queueSelection.snapshot->queue.profile,
					queueSelection.changed));

			QueueProfileRestartPolicy::PendingRequest pending;
			Assert::IsTrue(QueueProfileRestartPolicy::EnqueueResult::Queued ==
				QueueProfileRestartPolicy::Enqueue(pending,
					queueSelection.snapshot->generation,
					queueSelection.snapshot->queue.profile, "shortcut:F2"));

			UnifiedProfileRuntime::SelectionResult reselect;
			Assert::IsTrue(runtime.SelectKey("F2",
				[](const std::string&, std::string&) { return false; }, reselect,
				error));
			Assert::IsFalse(reselect.changed);
			Assert::IsFalse(QueueProfileRestartPolicy::
				RequiresResetAfterManualSelection(true,
					reselect.snapshot->queue.profile, reselect.changed));

			UnifiedProfileRuntime::SelectionResult viewportSelection;
			Assert::IsTrue(runtime.SelectKey("F3",
				[](const std::string&, std::string&) { return false; },
				viewportSelection, error));
			Assert::IsTrue(viewportSelection.changed);
			Assert::IsFalse(QueueProfileRestartPolicy::
				RequiresResetAfterManualSelection(false,
					viewportSelection.snapshot->queue.profile,
					viewportSelection.changed));
			Assert::IsFalse(QueueProfileRestartPolicy::
				RequiresResetAfterManualSelection(false, "base", true));

			UnifiedProfileRuntime::SelectionResult firstRapidSelection;
			Assert::IsTrue(runtime.SelectKey("F1",
				[](const std::string&, std::string&) { return false; },
				firstRapidSelection, error));
			Assert::IsTrue(QueueProfileRestartPolicy::
				RequiresResetAfterManualSelection(true,
					firstRapidSelection.snapshot->queue.profile,
					firstRapidSelection.changed));
			Assert::IsTrue(QueueProfileRestartPolicy::EnqueueResult::Coalesced ==
				QueueProfileRestartPolicy::Enqueue(pending,
					firstRapidSelection.snapshot->generation,
					firstRapidSelection.snapshot->queue.profile, "shortcut:F1"));

			UnifiedProfileRuntime::SelectionResult finalRapidSelection;
			Assert::IsTrue(runtime.SelectKey("F2",
				[](const std::string&, std::string&) { return false; },
				finalRapidSelection, error));
			Assert::IsTrue(QueueProfileRestartPolicy::
				RequiresResetAfterManualSelection(true,
					finalRapidSelection.snapshot->queue.profile,
					finalRapidSelection.changed));
			Assert::IsTrue(QueueProfileRestartPolicy::EnqueueResult::Coalesced ==
				QueueProfileRestartPolicy::Enqueue(pending,
					finalRapidSelection.snapshot->generation,
					finalRapidSelection.snapshot->queue.profile, "shortcut:F2"));

			QueueProfileRestartPolicy::PendingRequest finalRequest;
			Assert::IsTrue(QueueProfileRestartPolicy::Consume(pending,
				finalRequest));
			Assert::AreEqual(finalRapidSelection.snapshot->queue.profile.c_str(),
				finalRequest.profile.c_str());
			Assert::IsFalse(QueueProfileRestartPolicy::Consume(pending,
				finalRequest));
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(FreshAlphaConstructionConsumesMatchingPendingProfileReset)
		{
			QueueProfileRestartPolicy::PendingRequest pending;
			Assert::IsTrue(QueueProfileRestartPolicy::EnqueueResult::Queued ==
				QueueProfileRestartPolicy::Enqueue(
					pending, 8, "vp_60", "shortcut:Shift+V"));

			QueueProfileRestartPolicy::FreshConstructionEvidence evidence;
			evidence.freshConstruction = true;
			evidence.running = true;
			evidence.rendererGeneration = 4;
			evidence.appliedSnapshotGeneration = 10;
			evidence.appliedProfile = "vp_60";
			evidence.currentSnapshotGeneration = 10;
			evidence.currentProfile = "vp_60";

			QueueProfileRestartPolicy::PendingRequest satisfied;
			Assert::IsTrue(QueueProfileRestartPolicy::
				ConsumeIfSatisfiedByFreshConstruction(
					pending, evidence, satisfied));
			Assert::IsFalse(pending.pending);
			Assert::IsTrue(satisfied.pending);
			Assert::AreEqual<uint64_t>(8, satisfied.snapshotGeneration);
			Assert::AreEqual("vp_60", satisfied.profile.c_str());
			Assert::AreEqual("shortcut:Shift+V", satisfied.source.c_str());
		}

		TEST_METHOD(RejectedQueueProfileResetSubmissionRestoresExactIdentity)
		{
			QueueProfileRestartPolicy::PendingRequest pending;
			QueueProfileRestartPolicy::Enqueue(
				pending, 23, "madvr_queue", "shortcut:Shift+M");
			QueueProfileRestartPolicy::PendingRequest dispatched;
			Assert::IsTrue(QueueProfileRestartPolicy::Consume(
				pending, dispatched));
			Assert::IsTrue(QueueProfileRestartPolicy::RestoreConsumedIfEmpty(
				pending, dispatched));
			Assert::AreEqual<uint64_t>(23, pending.snapshotGeneration);
			Assert::AreEqual("madvr_queue", pending.profile.c_str());
			Assert::AreEqual("shortcut:Shift+M", pending.source.c_str());

			QueueProfileRestartPolicy::PendingRequest newer;
			QueueProfileRestartPolicy::Enqueue(
				newer, 24, "vp_queue", "shortcut:Shift+V");
			Assert::IsFalse(QueueProfileRestartPolicy::RestoreConsumedIfEmpty(
				newer, dispatched));
			Assert::AreEqual<uint64_t>(24, newer.snapshotGeneration);
			Assert::AreEqual("vp_queue", newer.profile.c_str());
		}

		TEST_METHOD(FreshDirectShowConstructionRequiresCompleteConsistentAudit)
		{
			QueueProfileRestartPolicy::PendingRequest pending;
			QueueProfileRestartPolicy::Enqueue(
				pending, 14, "madvr_queue", "shortcut:Shift+M");

			QueueProfileRestartPolicy::FreshConstructionEvidence evidence;
			evidence.freshConstruction = true;
			evidence.running = true;
			evidence.directShow = true;
			evidence.rendererGeneration = 5;
			evidence.appliedSnapshotGeneration = 14;
			evidence.appliedProfile = "madvr_queue";
			evidence.currentSnapshotGeneration = 14;
			evidence.currentProfile = "madvr_queue";

			QueueProfileRestartPolicy::PendingRequest satisfied;
			Assert::IsFalse(QueueProfileRestartPolicy::
				ConsumeIfSatisfiedByFreshConstruction(
					pending, evidence, satisfied));
			Assert::IsTrue(pending.pending);
			Assert::AreEqual("shortcut:Shift+M", pending.source.c_str());

			evidence.directShowAuditComplete = true;
			Assert::IsFalse(QueueProfileRestartPolicy::
				ConsumeIfSatisfiedByFreshConstruction(
					pending, evidence, satisfied));
			Assert::IsTrue(pending.pending);

			evidence.directShowAuditConsistent = true;
			Assert::IsTrue(QueueProfileRestartPolicy::
				ConsumeIfSatisfiedByFreshConstruction(
					pending, evidence, satisfied));
			Assert::AreEqual("shortcut:Shift+M", satisfied.source.c_str());
		}

		TEST_METHOD(FreshConstructionDoesNotConsumeNewerOrChangedProfileIntent)
		{
			QueueProfileRestartPolicy::PendingRequest pending;
			QueueProfileRestartPolicy::Enqueue(
				pending, 15, "madvr_queue", "rule-context:renderer-shortcut");
			const QueueProfileRestartPolicy::PendingRequest original = pending;

			QueueProfileRestartPolicy::FreshConstructionEvidence evidence;
			evidence.rendererGeneration = 5;
			evidence.appliedSnapshotGeneration = 15;
			evidence.appliedProfile = "madvr_queue";
			evidence.currentSnapshotGeneration = 15;
			evidence.currentProfile = "madvr_queue";

			QueueProfileRestartPolicy::PendingRequest satisfied;
			Assert::IsFalse(QueueProfileRestartPolicy::
				ConsumeIfSatisfiedByFreshConstruction(
					pending, evidence, satisfied));
			Assert::AreEqual(original.source.c_str(), pending.source.c_str());

			evidence.freshConstruction = true;
			Assert::IsFalse(QueueProfileRestartPolicy::
				ConsumeIfSatisfiedByFreshConstruction(
					pending, evidence, satisfied));
			Assert::AreEqual(original.source.c_str(), pending.source.c_str());

			evidence.running = true;
			evidence.appliedSnapshotGeneration = 14;
			evidence.currentSnapshotGeneration = 14;
			Assert::IsFalse(QueueProfileRestartPolicy::
				ConsumeIfSatisfiedByFreshConstruction(
					pending, evidence, satisfied));
			Assert::AreEqual(original.snapshotGeneration,
				pending.snapshotGeneration);
			Assert::AreEqual(original.profile.c_str(), pending.profile.c_str());
			Assert::AreEqual(original.source.c_str(), pending.source.c_str());

			evidence.appliedSnapshotGeneration = 15;
			evidence.currentSnapshotGeneration = 16;
			Assert::IsFalse(QueueProfileRestartPolicy::
				ConsumeIfSatisfiedByFreshConstruction(
					pending, evidence, satisfied));
			Assert::IsTrue(pending.pending);

			evidence.appliedSnapshotGeneration = 16;
			evidence.appliedProfile = "vp_60";
			evidence.currentProfile = "vp_60";
			Assert::IsFalse(QueueProfileRestartPolicy::
				ConsumeIfSatisfiedByFreshConstruction(
					pending, evidence, satisfied));
			Assert::IsTrue(pending.pending);
			Assert::AreEqual("rule-context:renderer-shortcut",
				pending.source.c_str());
		}

		TEST_METHOD(UnifiedProfileRuntimeReloadsEditedViewportAndKeepsSelection)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-live-profile-reload.cfg";
			auto writeConfiguration = [&path](const char* scopeAspect)
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[vprenderer.viewport]\n"
					"when: $key==\"F3\"\n"
					"screen_aspect: 16:9\n"
					"[vprenderer.viewport.scope]\n"
					"when: $key==\"F2\"\n"
					"screen_aspect: " << scopeAspect << "\n";
			};

			writeConfiguration("2.35:1");
			ConfigFile initialConfig;
			Assert::IsTrue(initialConfig.Load(path));
			std::string error;
			UnifiedProfileRuntime::Runtime runtime;
			Assert::IsTrue(runtime.Initialize(initialConfig,
				[](const std::string&, std::string&) { return false; }, error),
				std::wstring(error.begin(), error.end()).c_str());
			UnifiedProfileRuntime::SelectionResult selected;
			Assert::IsTrue(runtime.SelectKey("F2",
				[](const std::string&, std::string&) { return false; },
				selected, error));
			Assert::AreEqual("scope",
				selected.snapshot->viewport.profile.c_str());

			writeConfiguration("2.4:1");   // a different size: same-size rewrites hit the ConfigFile cache weakness (SameSizeEdit... test)
			ConfigFile editedConfig;
			Assert::IsTrue(editedConfig.Load(path));
			UnifiedProfileRuntime::RefreshResult reloaded;
			Assert::IsTrue(runtime.Reload(editedConfig,
				[](const std::string&, std::string&) { return false; },
				reloaded, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(reloaded.changed);
			Assert::AreEqual("scope",
				reloaded.snapshot->viewport.profile.c_str());
			Assert::AreEqual(12ull,
				reloaded.snapshot->viewport.screenAspect.numerator);
			Assert::AreEqual(5ull,
				reloaded.snapshot->viewport.screenAspect.denominator);
			DeleteFileA(path.c_str());
		}


        TEST_METHOD(SubtitleHdrAnalysisOverridesLegacyZoomForEveryStyle)
        {
            for(const char* style : {"classic","generated_gray","black","off"})
                for(bool explicitOverride : {false,true}) {
                    CachedConfigTestFile fixture;
                    {
                        std::ofstream file(fixture.path);
                        file << "[vprenderer.zoom.Legacy]\nhdr_peak_analysis_picture_only: true\n"
                            "hdr_peak_analysis_motion_compensation: false\nhdr_peak_analysis_height_percent: 68\n"
                            "hdr_peak_analysis_position: bottom\n[vprenderer.subtitles.Current]\ntype: " << style << "\n";
                        if(explicitOverride) file << "hdr_peak_analysis_picture_only: false\n"
                            "hdr_peak_analysis_motion_compensation: true\nhdr_peak_analysis_height_percent: 80\n"
                            "hdr_peak_analysis_position: top\n";
                    }
                    ConfigFile config;Assert::IsTrue(config.Load(fixture.path));
                    UnifiedProfileRuntime::Runtime runtime;std::string error;
                    Assert::IsTrue(runtime.Initialize(config,[](const std::string&,std::string&){return false;},error),std::wstring(error.begin(),error.end()).c_str());
                    const auto& view=runtime.GetSnapshot()->viewport;
                    Assert::AreEqual(!explicitOverride,view.hdrPeakAnalysisPictureOnly);
                    Assert::AreEqual(explicitOverride,view.hdrPeakAnalysisMotionCompensation);
                    Assert::AreEqual(explicitOverride?80:68,view.hdrPeakAnalysisHeightPercent);
                    Assert::AreEqual(std::string(explicitOverride?"top":"bottom"),view.hdrPeakAnalysisPosition);
                }
            std::string expected;
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","hdr_peak_analysis_height_percent","9",expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","hdr_peak_analysis_picture_only","maybe",expected));
        }

        TEST_METHOD(ClassicSubtitleDefaultsRestoreDriftAndPreserveExplicitZero)
        {
            for (bool explicitZero : { false, true }) {
                CachedConfigTestFile fixture;
                {
                    std::ofstream file(fixture.path);
                    // Legacy values must not leak into the new independent profile.
                    file << "[vprenderer.zoom.Old]\nsubtitle_engage_drift_ms: 999\nsubtitle_padding_pixels: 99\n"
                        "[vprenderer.subtitles.Classic]\ntype: classic\n";
                    if (explicitZero) file << "offset_pixels: 0\nsubtitle_engage_drift_ms: 0\nsubtitle_release_drift_ms: 0\nsubtitle_target_buffer_pixels: 0\n";
                }
                ConfigFile config; Assert::IsTrue(config.Load(fixture.path));
                std::string error; UnifiedProfileRuntime::Runtime runtime;
                Assert::IsTrue(runtime.Initialize(config,[](const std::string&,std::string&){return false;},error),std::wstring(error.begin(),error.end()).c_str());
                const auto& view=runtime.GetSnapshot()->viewport;
                Assert::IsTrue(view.subtitleFit);
                Assert::AreEqual(uint64_t(2000),view.subtitleHoldMilliseconds);
                Assert::AreEqual(uint64_t(explicitZero ? 0 : 500),view.subtitleEngageDriftMilliseconds);
                Assert::AreEqual(uint64_t(explicitZero ? 0 : 2000),view.subtitleReleaseDriftMilliseconds);
                Assert::AreEqual(explicitZero ? 0 : 20,view.subtitlePaddingPixels);
                Assert::AreEqual(explicitZero ? 0 : 10,view.subtitleTargetBufferPixels);
            }
        }

        TEST_METHOD(SubtitleBackgroundLayoutDefaultsValidationAndLiveProfilePolicy)
        {
            std::string expected;
            for(const char* value:{"true","false","on","off","1","0"})
                Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("subtitles","rounded_corners",value,expected));
            for(const char* value:{"extend","float"})
                Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("subtitles","background_edge",value,expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","rounded_corners","maybe",expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","background_edge","auto",expected));
            for(const char* type:{"off","classic","generated_gray","black"}) {
                std::map<std::string,std::string> values{{"type",type}};
                auto layout=RendererProfileConfig::ResolveSubtitleBackgroundLayout(values);
                Assert::IsTrue(layout.roundedCorners && !layout.floatBackground);
                values["rounded_corners"]="false";values["background_edge"]="float";
                layout=RendererProfileConfig::ResolveSubtitleBackgroundLayout(values);
                const bool moved=std::string(type)=="generated_gray" || std::string(type)=="black";
                Assert::IsTrue(layout.roundedCorners==!moved && layout.floatBackground==moved);
                values["rounded_corners"]="bad";values["background_edge"]="bad";
                layout=RendererProfileConfig::ResolveSubtitleBackgroundLayout(values);
                Assert::IsTrue(layout.roundedCorners && !layout.floatBackground);
            }
            for(const char* type:{"generated_gray","black"}) {
                CachedConfigTestFile fixture;
                { std::ofstream file(fixture.path);file<<"[vprenderer.subtitles.Shape]\ntype: "<<type<<"\nrounded_corners: false\nbackground_edge: float\n"; }
                ConfigFile config;Assert::IsTrue(config.Load(fixture.path));
                RendererProfileConfig::Model model;std::string error;
                Assert::IsTrue(RendererProfileConfig::Read(config,model,error));
                const auto layout=RendererProfileConfig::ResolveSubtitleBackgroundLayout(model.profiles.at("subtitles.shape").settings);
                Assert::IsTrue(!layout.roundedCorners && layout.floatBackground);
            }
            for(const char* key:{"rounded_corners","background_edge"})
                Assert::IsTrue(ConfigurationApplyPolicy::ClassifyChange({"vprenderer.subtitles.Shape",key})==ConfigurationApplyPolicy::Action::ApplyProfiles);
        }

        TEST_METHOD(SubtitleTextReductionIsOptionalBoundedAndMovedProfileOnly)
        {
            std::string expected;
            for(const char* value:{"0","1","25","50","75"})
                Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("subtitles","text_reduction_percent",value,expected));
            for(const char* value:{"-1","76","100","25.5","none","","999999999999999"})
                Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","text_reduction_percent",value,expected));
            for(const char* type:{"off","classic","generated_gray","black"}) {
                std::map<std::string,std::string> settings{{"type",type}};
                Assert::AreEqual(0,RendererProfileConfig::ResolveSubtitleTextReduction(settings));
                settings["text_reduction_percent"]="75";
                const bool moved=std::string(type)=="generated_gray" || std::string(type)=="black";
                Assert::AreEqual(moved?75:0,RendererProfileConfig::ResolveSubtitleTextReduction(settings));
                settings["text_reduction_percent"]="0";
                Assert::AreEqual(0,RendererProfileConfig::ResolveSubtitleTextReduction(settings));
                settings["text_reduction_percent"]="76";
                Assert::AreEqual(0,RendererProfileConfig::ResolveSubtitleTextReduction(settings));
            }
            for(const char* type:{"generated_gray","black"}) {
                CachedConfigTestFile fixture;
                { std::ofstream file(fixture.path);file<<"[vprenderer.subtitles.Small]\ntype: "<<type<<"\ntext_reduction_percent: 75\n"; }
                ConfigFile config;Assert::IsTrue(config.Load(fixture.path));
                RendererProfileConfig::Model model;std::string error;
                Assert::IsTrue(RendererProfileConfig::Read(config,model,error));
                Assert::AreEqual(75,RendererProfileConfig::ResolveSubtitleTextReduction(model.profiles.at("subtitles.small").settings));
            }
        }

        TEST_METHOD(ClassicSubtitleControlsResolveFromTheirOwnProfile)
        {
            CachedConfigTestFile fixture;
            {
                std::ofstream file(fixture.path);
                file << "[vprenderer.subtitles.Classic]\ntype: classic\noffset_pixels: 23\n"
                    "subtitle_hold_seconds: 1.5\nsubtitle_engage_drift_ms: 500\nsubtitle_release_drift_ms: 1700\nsubtitle_target_buffer_pixels: 12\n";
            }
            ConfigFile config; Assert::IsTrue(config.Load(fixture.path));
            std::string error; UnifiedProfileRuntime::Runtime runtime;
            Assert::IsTrue(runtime.Initialize(config,[](const std::string&,std::string&){return false;},error),std::wstring(error.begin(),error.end()).c_str());
            const auto& view=runtime.GetSnapshot()->viewport;
            Assert::IsTrue(view.subtitleFit);
            Assert::AreEqual(uint64_t(1500),view.subtitleHoldMilliseconds);
            Assert::AreEqual(uint64_t(500),view.subtitleEngageDriftMilliseconds);
            Assert::AreEqual(uint64_t(1700),view.subtitleReleaseDriftMilliseconds);
            Assert::AreEqual(23,view.subtitlePaddingPixels);
            Assert::AreEqual(12,view.subtitleTargetBufferPixels);
            std::string expected;
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","subtitle_hold_seconds","0.1",expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","subtitle_target_buffer_pixels","51",expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","subtitle_engage_drift_ms","30001",expected));
        }

        TEST_METHOD(SubtitleProfilesCycleIndependentlyAndIgnoreLegacyKnobs)
        {
            CachedConfigTestFile fixture;
            {
                std::ofstream file(fixture.path);
                file << "[general]\npersist_profile_selection: false\n"
                    "[vprenderer]\nsubtitle_move_inset: broken-old-value\nsubtitle_cut_paste_test: true\n"
                    "[vprenderer.viewport.Scope]\nscreen_aspect: 2.35:1\nsubtitle_fit: false\n"
                    "[vprenderer.zoom.Fill]\nautomatic_crop: true\nsubtitle_padding_pixels: 499\n"
                    "[vprenderer.subtitles.Classic]\ntype: classic\noffset_pixels: 15\ncycle_shortcut: Ctrl+Shift+T\n"
                    "[vprenderer.subtitles.Gray]\ntype: generated_gray\ncycle_shortcut: Ctrl+Shift+T\n"
                    "[vprenderer.subtitles.Black]\ntype: black\ncycle_shortcut: Ctrl+Shift+T\n"
                    "[vprenderer.subtitles.Off]\ntype: off\ncycle_shortcut: Ctrl+Shift+T\n";
            }
            ConfigFile config; Assert::IsTrue(config.Load(fixture.path));
            std::string error;
            RendererProfileConfig::Model model;
            Assert::IsTrue(RendererProfileConfig::Read(config,model,error),std::wstring(error.begin(),error.end()).c_str());
            Assert::IsTrue(model.profiles.at("display.base").settings.count("subtitle_move_inset")==0);
            Assert::IsTrue(model.profiles.at("zoom.fill").settings.count("subtitle_padding_pixels")==0);
            Assert::AreEqual(std::string("15"),model.profiles.at("subtitles.gray").settings.at("offset_pixels"));
            UnifiedProfileRuntime::Runtime runtime;
            const auto lookup=[](const std::string&,std::string&){return false;};
            Assert::IsTrue(runtime.Initialize(config,lookup,error),std::wstring(error.begin(),error.end()).c_str());
            Assert::IsTrue(runtime.GetSnapshot()->viewport.subtitleFit);
            UnifiedProfileRuntime::SelectionResult selection;
            for(const char* name:{"gray","black","off","classic"}) {
                Assert::IsTrue(runtime.SelectCycleKey("Ctrl+Shift+T",lookup,selection,error));
                Assert::AreEqual(std::string(name),selection.snapshot->effectiveSelections.at("subtitles").front());
                Assert::AreEqual(std::string("scope"),selection.snapshot->viewport.profile);
                Assert::AreEqual(std::string("fill"),selection.snapshot->viewport.zoomProfile);
                Assert::IsTrue(selection.snapshot->viewport.automaticCrop);
                Assert::AreEqual(std::string(name)=="classic",selection.snapshot->viewport.subtitleFit);
                Assert::AreEqual(15,selection.snapshot->viewport.subtitlePaddingPixels);
            }
            Assert::AreEqual(std::string("vprenderer.subtitles"),ConfigurationApplyPolicy::OrderedProfileGroup("vprenderer.subtitles.gray"));
            std::string expected;
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","type","blend",expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","offset_pixels","501",expected));
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("subtitles","subtitle_move_inset","15",expected));
            const auto osd=ProfileChangeOverlay::CollectAll({{"subtitles","gray"}});
            Assert::AreEqual(size_t(1),osd.size());
            Assert::AreEqual(std::string("Subtitles"),osd.front().label);
        }

		TEST_METHOD(ZoomShortcutNeverChangesSelectedScreenProfile)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-independent-zoom.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[vprenderer.viewport]\n"
					"screen_aspect: 16:9\n"
					"[vprenderer.viewport.scope]\n"
					"when: $key==\"F2\"\n"
					"screen_aspect: 2.35:1\n"
					"[vprenderer.zoom]\n"
					"crop_narrower_content_to_fill_screen: true\n"
					"[vprenderer.zoom.scope]\n"
					"when: $key==\"F2\"\n"
					"crop_narrower_content_to_fill_screen: false\n"
					"[vprenderer.zoom.scope_and_crop]\n"
					"when: $key==\"Shift+2\"\n"
					"crop_narrower_content_aspect_limit: 2.20:1\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			UnifiedProfileRuntime::Runtime runtime;
			Assert::IsTrue(runtime.Initialize(config,
				[](const std::string&, std::string&) { return false; }, error),
				std::wstring(error.begin(), error.end()).c_str());
			UnifiedProfileRuntime::SelectionResult selected;
			Assert::IsTrue(runtime.SelectKey("F2",
				[](const std::string&, std::string&) { return false; },
				selected, error));
			Assert::AreEqual("scope", selected.snapshot->viewport.profile.c_str());
			Assert::AreEqual("scope", selected.snapshot->viewport.zoomProfile.c_str());
			Assert::AreEqual(2.35, selected.snapshot->viewport.screenAspect.value,
				0.000001);
			Assert::IsFalse(selected.snapshot->viewport.cropNarrowerContentToFillScreen);

			Assert::IsTrue(runtime.SelectKey("Shift+2",
				[](const std::string&, std::string&) { return false; },
				selected, error));
			Assert::AreEqual("scope", selected.snapshot->viewport.profile.c_str());
			Assert::AreEqual("scope_and_crop",
				selected.snapshot->viewport.zoomProfile.c_str());
			Assert::AreEqual(2.35, selected.snapshot->viewport.screenAspect.value,
				0.000001);
			Assert::IsTrue(selected.snapshot->viewport.cropNarrowerContentToFillScreen);
			Assert::IsTrue(selected.snapshot->viewport.hasCropNarrowerContentAspectLimit);
			Assert::AreEqual(2.20,
				selected.snapshot->viewport.cropNarrowerContentAspectLimit.value,
				0.000001);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0103VerticalAlignmentDefaultsValidatesAndPublishes)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0103-vertical-alignment.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"profile_update_mode: never\n"
					"[vprenderer.viewport]\nscreen_aspect: 16:9\n"
					"[vprenderer.viewport.scope]\nwhen: $key==\"F2\"\n"
					"screen_aspect: 16:9\nvertical_alignment: BOTTOM\n"
					"crop_narrower_content_to_fill_screen: true\n"
					"crop_narrower_content_aspect_limit: 2.20:1\n"
					"crop_wider_content_to_fill_screen: true\n"
					"crop_wider_content_aspect_limit: 2.76:1\n"
					"subtitle_fit: true\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::ResolvedViewport base;
			Assert::IsTrue(RendererProfileConfig::ResolveViewport(
				model, "base", 1, base, error));
			Assert::AreEqual("center", base.verticalAlignment.c_str());
			RendererProfileConfig::ResolvedViewport scope;
			Assert::IsTrue(RendererProfileConfig::ResolveViewport(
				model, "scope", 2, scope, error));
			Assert::AreEqual("bottom", scope.verticalAlignment.c_str());
			Assert::IsTrue(scope.cropNarrowerContentToFillScreen);
			Assert::IsTrue(scope.hasCropNarrowerContentAspectLimit);
			Assert::AreEqual(2.20,
				scope.cropNarrowerContentAspectLimit.value, 0.000001);
			Assert::IsTrue(scope.cropWiderContentToFillScreen);
			Assert::IsTrue(scope.hasCropWiderContentAspectLimit);
			Assert::AreEqual(2.76,
				scope.cropWiderContentAspectLimit.value, 0.000001);

			UnifiedProfileRuntime::Runtime runtime;
			Assert::IsTrue(runtime.Initialize(config,
				[](const std::string&, std::string&) { return false; }, error));
			UnifiedProfileRuntime::SelectionResult selected;
			Assert::IsTrue(runtime.SelectKey("F2",
				[](const std::string&, std::string&) { return false; },
				selected, error));
			std::string published;
			Assert::IsTrue(selected.snapshot->variables.Lookup(
				"vertical_alignment", published));
			Assert::AreEqual("bottom", published.c_str());
			const StateVariables::Value* narrowerAspectLimit =
				selected.snapshot->variables.Find(
					"$crop_narrower_content_aspect_limit");
			Assert::IsNotNull(narrowerAspectLimit);
			Assert::IsTrue(narrowerAspectLimit->type ==
				StateVariables::ValueType::Aspect);
			Assert::AreEqual(2.20, narrowerAspectLimit->number, 0.000001);
			const StateVariables::Value* widerAspectLimit =
				selected.snapshot->variables.Find(
					"$crop_wider_content_aspect_limit");
			Assert::IsNotNull(widerAspectLimit);
			Assert::IsTrue(widerAspectLimit->type ==
				StateVariables::ValueType::Aspect);
			Assert::AreEqual(2.76, widerAspectLimit->number, 0.000001);

			// Optional limits are best-effort. A legacy or malformed value must not
			// reject the complete renderer configuration.
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[vprenderer.viewport]\n"
					"screen_aspect: 2.35:1\n"
					"crop_narrower_content_aspect_limit: 2.20:1\n"
					"crop_wider_content_aspect_limit: 2.76:1\n"
					"[vprenderer.viewport.scope]\nwhen: $key==\"F2\"\n"
					"crop_narrower_content_aspect_limit: legacy-missing\n"
					"crop_wider_content_aspect_limit: not-a-ratio\n";
			}
			Assert::IsTrue(config.Load(path));
			error.clear();
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(RendererProfileConfig::ResolveViewport(
				model, "scope", 3, scope, error));
			Assert::IsFalse(scope.hasCropNarrowerContentAspectLimit);
			Assert::IsFalse(scope.hasCropWiderContentAspectLimit);

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[vprenderer.viewport]\n"
					"crop_narrower_content_aspect_limit: not-a-ratio\n"
					"crop_wider_content_aspect_limit: 4.1:1\n";
			}
			Assert::IsTrue(config.Load(path));
			error.clear();
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(RendererProfileConfig::ResolveViewport(
				model, "base", 4, base, error));
			Assert::IsFalse(base.hasCropNarrowerContentAspectLimit);
			Assert::IsFalse(base.hasCropWiderContentAspectLimit);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0098RejectsRemovedSecondScreenAspect)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0098-single-screen-aspect.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[vprenderer.viewport.scope]\n"
					"when: $key==\"F2\"\n"
					"screen_aspect: 2.35:1\n"
					"physical_screen_aspect: 2.35:1\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsFalse(RendererProfileConfig::Read(config, model, error));
			Assert::IsTrue(error.find("physical_screen_aspect") !=
				std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(UnifiedActionsPublishCommittedSourceAndProfileEvents)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-unified-actions.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[queue]\nqueue_size: 16\n"
					"[queue.low_latency]\nwhen: $eotf==\"pq\"\nqueue_size: 1\n"
					"[vprenderer]\nquality: high\n"
					"[vprenderer.color.rec709]\nshortcut: F5\n"
					"sdr_target_primaries: REC709\n"
					"[vprenderer.color.bt2020]\nshortcut: F6\n"
					"sdr_target_primaries: BT2020\n"
					"[vprenderer.viewport]\nlabel: 16x9\nscreen_aspect: 16:9\n"
					"[vprenderer.viewport.scope]\nwhen: $eotf==\"pq\"\n"
					"label: Scope\nscreen_aspect: 2.35:1\nautomatic_crop: true\n"
					"[actions.committed_scope_pq]\n"
					"on: state.committed\n"
					"when: $eotf==\"pq\" && $profile.viewport==\"scope\" && $previous.eotf==\"sdr\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.eotf_pq]\n"
					"on: source.eotf.changed\n"
					"when: $eotf==\"pq\" && $previous.eotf==\"sdr\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.eotf_context]\n"
					"on: source.eotf.changed\n"
					"when: $primaries==\"rec709\"\n"
					"run: C:\\Windows\\System32\\cmd.exe ${eotf} ${primaries} ${previous.eotf} ${profile.viewport} ${event} ${event_reason}\n"
					"[actions.rec709]\n"
					"on: source.primaries.changed\n"
					"when: $primaries==\"rec709\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.scope_entered]\n"
					"on: profile.viewport.changed\n"
					"when: $profile.viewport==\"scope\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.scope_visible_name]\n"
					"on: profile.viewport.changed\n"
					"when: $screen_config==\"Scope\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.scope_left]\n"
					"on: profile.viewport.changed\n"
					"when: $previous_profile.viewport==\"scope\" && $profile.viewport==\"base\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.queue_canonical]\n"
					"on: profile.queue.changed\n"
					"when: ${profile.queue}==\"low_latency\"\n"
					"run: C:\\Windows\\System32\\cmd.exe ${profile.queue}\n"
					"[actions.queue_ui_label]\n"
					"on: profile.queue.changed\n"
					"when: ${profile.queue}==\"Low Latency\"\n"
					"run: C:\\Windows\\System32\\cmd.exe ${profile.queue}\n"
					"[actions.queue_ui_label_left]\n"
					"on: profile.queue.changed\n"
					"when: ${previous_profile.queue}==\"Low Latency\" && ${profile.queue}==\"base\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.color_bt2020]\n"
					"on: profile.color.changed\n"
					"when: $profile.color==\"bt2020\" && $previous_profile.color==\"rec709\"\n"
					"coalesce_role: color-state\n"
					"delay_seconds: 0\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.renderer_scope_ready]\n"
					"on: renderer.ready\n"
					"when: $event_reason==\"renderer_ready\" && $profile.viewport==\"scope\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.refresh_values]\n"
					"on: refresh.applied\n"
					"when: $requested_refresh==\"59.94\"\n"
					"run: C:\\Windows\\System32\\cmd.exe ${actual_refresh} ${requested_refresh} ${previous_refresh}\n"
					"[actions.any_commit]\n"
					"on: state.committed\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.incomplete_draft]\n"
					"enabled: false\n"
					"on: not.a.real.event\n"
					"when: (\n"
					"run: sd\n"
					"renderer: unavailable\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(14), model.actions.size());
			const auto configuredColorAction = std::find_if(model.actions.begin(),
				model.actions.end(), [](const auto& action)
				{
					return action.name == "color_bt2020";
				});
			Assert::IsTrue(configuredColorAction != model.actions.end());
			Assert::AreEqual("color-state",
				configuredColorAction->coalesceRole.c_str());
			Assert::AreEqual(0, configuredColorAction->delaySeconds);

			auto source = [](const char* eotf, const char* primaries)
			{
				return [eotf, primaries](const std::string& name,
					std::string& value)
				{
					if (name == "eotf" || name == "transfer")
					{
						value = eotf;
						return true;
					}
					if (name == "primaries" || name == "colorspace")
					{
						value = primaries;
						return true;
					}
					return false;
				};
			};
			auto hasInvocation = [](const std::vector<
				UnifiedProfileRuntime::ActionInvocation>& actions,
				const char* actionName, const char* event)
			{
				return std::any_of(actions.begin(), actions.end(),
					[actionName, event](const UnifiedProfileRuntime::ActionInvocation& action)
					{
						return action.action.name == actionName &&
							action.event == event;
					});
			};

			UnifiedProfileRuntime::Runtime runtime;
			Assert::IsTrue(runtime.Initialize(config, source("sdr", "bt2020"), error),
				std::wstring(error.begin(), error.end()).c_str());
			UnifiedProfileRuntime::RefreshResult entered;
			Assert::IsTrue(runtime.Refresh(source("pq", "rec709"), entered, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(entered.changed);
			Assert::AreEqual("scope", entered.snapshot->viewport.profile.c_str());
			Assert::IsTrue(hasInvocation(entered.actions,
				"committed_scope_pq", "state.committed"));
			Assert::IsTrue(hasInvocation(entered.actions,
				"any_commit", "state.committed"));
			Assert::IsTrue(hasInvocation(entered.actions,
				"eotf_pq", "source.eotf.changed"));
			const auto context = std::find_if(entered.actions.begin(),
				entered.actions.end(), [](const UnifiedProfileRuntime::ActionInvocation& action)
				{
					return action.action.name == "eotf_context" &&
						action.event == "source.eotf.changed";
				});
			Assert::IsTrue(context != entered.actions.end());
			Assert::AreEqual(
				"pq rec709 sdr scope source.eotf.changed source",
				context->action.arguments.c_str());
			Assert::IsTrue(hasInvocation(entered.actions,
				"rec709", "source.primaries.changed"));
			Assert::IsTrue(hasInvocation(entered.actions,
				"scope_entered", "profile.viewport.changed"));
			Assert::IsTrue(hasInvocation(entered.actions,
				"scope_visible_name", "profile.viewport.changed"));
			std::string visibleScreenConfig;
			Assert::IsTrue(entered.snapshot->LookupVariable(
				"screen_config", visibleScreenConfig));
			Assert::AreEqual("Scope", visibleScreenConfig.c_str());
			Assert::IsTrue(hasInvocation(entered.actions,
				"queue_canonical", "profile.queue.changed"));
			const auto queueUiLabel = std::find_if(entered.actions.begin(),
				entered.actions.end(), [](const UnifiedProfileRuntime::ActionInvocation& action)
				{
					return action.action.name == "queue_ui_label" &&
						action.event == "profile.queue.changed";
				});
			Assert::IsTrue(queueUiLabel != entered.actions.end());
			Assert::AreEqual("low_latency", queueUiLabel->action.arguments.c_str());

			std::vector<UnifiedProfileRuntime::ActionInvocation> ready;
			Assert::IsTrue(runtime.CollectActionInvocations("renderer.ready",
				"renderer_ready", nullptr, entered.snapshot, ready, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(hasInvocation(ready, "renderer_scope_ready",
				"renderer.ready"));

			std::vector<UnifiedProfileRuntime::ActionInvocation> refresh;
			const EventActionLauncher::ActionValueLookup refreshValues =
				[](const std::string& variable, std::string& value)
				{
					const std::map<std::string, std::string> values = {
						{ "actual_refresh", "59.9401" },
						{ "requested_refresh", "59.94" },
						{ "previous_refresh", "23.976" }
					};
					const auto found = values.find(variable);
					if (found == values.end()) return false;
					value = found->second;
					return true;
				};
			Assert::IsTrue(runtime.CollectActionInvocations("refresh.applied",
				"refresh", nullptr, entered.snapshot, refresh, error, refreshValues),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(hasInvocation(refresh, "refresh_values",
				"refresh.applied"));
			Assert::AreEqual("59.9401 59.94 23.976",
				refresh.front().action.arguments.c_str());

			UnifiedProfileRuntime::RefreshResult left;
			Assert::IsTrue(runtime.Refresh(source("sdr", "bt2020"), left, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(left.changed);
			Assert::AreEqual("base", left.snapshot->viewport.profile.c_str());
			Assert::IsTrue(hasInvocation(left.actions, "scope_left",
				"profile.viewport.changed"));
			Assert::IsTrue(hasInvocation(left.actions, "queue_ui_label_left",
				"profile.queue.changed"));

			// Establish the color selection explicitly before exercising the
			// Rec.709-to-BT.2020 transition. Persisted selections from a previous
			// test run are intentionally a fallback and must not make this action
			// assertion dependent on the temporary state file.
			UnifiedProfileRuntime::SelectionResult rec709Selection;
			Assert::IsTrue(runtime.SelectKey("F5", source("sdr", "bt2020"),
				rec709Selection, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(rec709Selection.changed);
			Assert::AreEqual("rec709", RendererProfileConfig::FormatSelection(
				rec709Selection.snapshot->effectiveSelections.at("color")).c_str());

			UnifiedProfileRuntime::SelectionResult colorSelection;
			Assert::IsTrue(runtime.SelectKey("F6", source("sdr", "bt2020"),
				colorSelection, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(colorSelection.changed);
			Assert::AreEqual("bt2020", RendererProfileConfig::FormatSelection(
				colorSelection.snapshot->effectiveSelections.at("color")).c_str());
			Assert::IsTrue(hasInvocation(colorSelection.actions, "color_bt2020",
				"profile.color.changed"));
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(PendingEventActionsCoalesceToNewestTrigger)
		{
			RendererProfileConfig::Model::EventAction action;
			action.name = "Renderer Nits";
			action.renderer = "VP Renderer";
			action.rendererSelectorIndex = 2;
			const std::string identity =
				EventActionLauncher::ActionIdentity(action);

			EventActionLauncher::PendingActionCoalescer coalescer;
			const uint64_t first = coalescer.Schedule(identity);
			const uint64_t second = coalescer.Schedule(identity);
			Assert::IsFalse(coalescer.Claim(identity, first));
			Assert::IsTrue(coalescer.Claim(identity, second));
			Assert::IsFalse(coalescer.Claim(identity, second));

			RendererProfileConfig::Model::EventAction sameAction = action;
			sameAction.rendererSelectorIndex = 3;
			const std::string sameIdentity =
				EventActionLauncher::ActionIdentity(sameAction);
			Assert::AreEqual(identity.c_str(), sameIdentity.c_str());
			const uint64_t other = coalescer.Schedule(sameIdentity);
			coalescer.CancelAll();
			Assert::IsFalse(coalescer.Claim(sameIdentity, other));

			RendererProfileConfig::Model::EventAction rec709 = action;
			rec709.name = "Rec709";
			rec709.coalesceRole = "Color State";
			RendererProfileConfig::Model::EventAction bt2020 = rec709;
			bt2020.name = "BT2020";
			const std::string colorIdentity =
				EventActionLauncher::ActionIdentity(rec709);
			Assert::AreEqual(colorIdentity.c_str(),
				EventActionLauncher::ActionIdentity(bt2020).c_str());
			const uint64_t oldColor = coalescer.Schedule(colorIdentity);
			const uint64_t newColor = coalescer.Schedule(colorIdentity);
			Assert::IsFalse(coalescer.Claim(colorIdentity, oldColor));
			Assert::IsTrue(coalescer.Claim(colorIdentity, newColor));
		}


        TEST_METHOD(ScreenIntentSurvivesRunningProfileScript)
        {
            for (const auto event : { "profile.viewport.changed", "profile.zoom.changed",
                "profile.color.changed", "profile.queue.changed", "profile.nls.changed" })
                Assert::IsFalse(EventActionLauncher::IsProfileActionFeedback(true, event, "manual"));
        }

        TEST_METHOD(RenderingFeedbackRemainsSuppressedDuringScriptOnly)
        {
            Assert::IsTrue(EventActionLauncher::IsProfileActionFeedback(true,
                "profile.display.changed", "manual"));
            Assert::IsFalse(EventActionLauncher::IsProfileActionFeedback(false,
                "profile.display.changed", "manual"));
            Assert::IsFalse(EventActionLauncher::IsProfileActionFeedback(true,
                "profile.display.changed", "cycle"));
            Assert::IsFalse(EventActionLauncher::IsProfileActionFeedback(true,
                "renderer.ready", "renderer_ready"));
        }

        TEST_METHOD(DelayedActionCannotClaimReusedGenerationAfterCompletion)
        {
            EventActionLauncher::PendingActionCoalescer queue;
            const auto stale = queue.Schedule("screen-state");
            const auto settled = queue.Schedule("screen-state");
            Assert::IsTrue(queue.Claim("screen-state", settled));
            const auto latest = queue.Schedule("screen-state");
            Assert::IsFalse(queue.Claim("screen-state", stale));
            Assert::IsTrue(queue.Claim("screen-state", latest));
        }

        TEST_METHOD(CanceledActionCannotClaimNewSelectionAfterReload)
        {
            EventActionLauncher::PendingActionCoalescer queue;
            const auto canceled = queue.Schedule("renderer-nits");
            queue.CancelAll();
            const auto current = queue.Schedule("renderer-nits");
            Assert::IsFalse(queue.Claim("renderer-nits", canceled));
            Assert::IsTrue(queue.Claim("renderer-nits", current));
        }


        TEST_METHOD(ScopeActionSurvivesOldRenderingFeedbackAndRepeatedSelections)
        {
            char directory[MAX_PATH] = {};
            Assert::IsTrue(GetTempPathA(ARRAYSIZE(directory), directory) > 0);
            const std::string path = std::string(directory) + "VP-action-latest-regression.cfg";
            {
                std::ofstream file(path);
                file << "[vprenderer.rec709_169]\nshortcut: Q\nsdr_target_nits: 113\n"
                    "[vprenderer.rec709_scope]\nshortcut: Ctrl+Q\nsdr_target_nits: 117\n"
                    "[vprenderer.viewport.wide]\nlabel: 16x9\nshortcut: F3\nscreen_aspect: 16:9\n"
                    "[vprenderer.viewport.scope]\nlabel: Scope\nshortcut: F2\nscreen_aspect: 2.35:1\n"
                    "[actions.screen_wide]\non: profile.viewport.changed\n"
                    "when: ${screen_config} == \"16x9\"\ncoalesce_role: screen-state\n"
                    "run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
                    "[actions.screen_scope]\non: profile.viewport.changed\n"
                    "when: ${screen_config} == \"Scope\"\ncoalesce_role: screen-state\n"
                    "run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
                    "[actions.rendering_feedback]\non: profile.display.changed\n"
                    "run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
                    "[actions.generic_feedback]\non: state.committed\n"
                    "run: C:\\Windows\\System32\\cmd.exe /c exit 0\n";
            }
            ConfigFile config;
            Assert::IsTrue(config.Load(path));
            UnifiedProfileRuntime::Runtime runtime;
            std::string error;
            const auto source = [](const std::string&, std::string&) { return false; };
            Assert::IsTrue(runtime.Initialize(config, source, error),
                std::wstring(error.begin(), error.end()).c_str());
            UnifiedProfileRuntime::SelectionResult selected;
            Assert::IsTrue(runtime.SelectKey("Ctrl+Q", source, selected, error));
            EventActionLauncher::PendingActionCoalescer queue;
            for (int cycle = 0; cycle < 3; ++cycle)
            {
                Assert::IsTrue(runtime.SelectKey("F3", source, selected, error));
                const auto old = queue.Schedule("screen-state");
                Assert::IsTrue(queue.Claim("screen-state", old)); // 16:9 script running
                Assert::IsTrue(runtime.SelectKey("F2", source, selected, error));
                Assert::AreEqual("scope", selected.snapshot->viewport.profile.c_str());
                const auto scope = std::find_if(selected.actions.begin(), selected.actions.end(),
                    [](const auto& a) { return a.action.name == "screen_scope"; });
                Assert::IsTrue(scope != selected.actions.end());
                Assert::IsFalse(EventActionLauncher::IsProfileActionFeedback(true, scope->event, scope->reason));
                const auto latest = queue.Schedule("screen-state");
                Assert::IsTrue(runtime.SelectKey("Q", source, selected, error)); // old script's key
                Assert::IsFalse(selected.actions.empty());
                Assert::IsTrue(EventActionLauncher::IsRenderingSelectionFeedback(true, selected.selections, false));
                Assert::IsTrue(std::any_of(selected.actions.begin(), selected.actions.end(),
                    [](const auto& a) { return a.event == "state.committed"; }));
                Assert::IsTrue(queue.Claim("screen-state", latest)); // queued Scope still runs
                Assert::IsTrue(runtime.SelectKey("Ctrl+Q", source, selected, error));
                Assert::AreEqual("scope", selected.snapshot->viewport.profile.c_str());
                Assert::IsTrue(runtime.SelectKey("Ctrl+Q", source, selected, error));
                Assert::IsFalse(selected.changed);
                Assert::IsTrue(selected.actions.empty()); // idempotent script does not loop
            }
            DeleteFileA(path.c_str());
        }

        TEST_METHOD(CoalescerReportsOnlyActualPendingReplacement)
        {
            EventActionLauncher::PendingActionCoalescer queue;
            bool replaced = true;
            const auto first = queue.Schedule("screen", &replaced);
            Assert::IsFalse(replaced);
            const auto next = queue.Schedule("screen", &replaced);
            Assert::IsTrue(replaced);
            Assert::IsFalse(queue.Claim("screen", first));
            Assert::IsTrue(queue.Claim("screen", next));
            const auto fresh = queue.Schedule("screen", &replaced);
            Assert::IsFalse(replaced);
            const auto color = queue.Schedule("color", &replaced);
            Assert::IsFalse(replaced);
            Assert::IsTrue(queue.Claim("color", color));
            Assert::IsTrue(queue.Claim("screen", fresh));
        }

        TEST_METHOD(RenderingFeedbackClassificationKeepsMixedAndCycleSelections)
        {
            std::vector<RendererProfileConfig::KeySelection> selection(1);
            selection[0].group = "display";
            Assert::IsTrue(EventActionLauncher::IsRenderingSelectionFeedback(true, selection, false));
            Assert::IsFalse(EventActionLauncher::IsRenderingSelectionFeedback(false, selection, false));
            Assert::IsFalse(EventActionLauncher::IsRenderingSelectionFeedback(true, selection, true));
            selection.emplace_back();
            selection.back().group = "viewport";
            Assert::IsFalse(EventActionLauncher::IsRenderingSelectionFeedback(true, selection, false));
            selection.erase(selection.begin());
            Assert::IsFalse(EventActionLauncher::IsRenderingSelectionFeedback(true, selection, false));
            selection.clear();
            Assert::IsFalse(EventActionLauncher::IsRenderingSelectionFeedback(true, selection, false));
        }

		TEST_METHOD(ProfileActionCircuitBreakerBoundsRecursiveLaunches)
		{
			using Decision = EventActionLauncher::ProfileActionCircuitBreaker::Decision;
			EventActionLauncher::ProfileActionCircuitBreaker breaker;
			for (uint64_t now : { 1000ull, 2000ull, 3000ull, 4000ull })
				Assert::AreEqual(static_cast<int>(Decision::Allow),
					static_cast<int>(breaker.BeginLaunch(now)));
			Assert::AreEqual(static_cast<int>(Decision::Tripped),
				static_cast<int>(breaker.BeginLaunch(5000)));
			Assert::AreEqual(static_cast<int>(Decision::Suppressed),
				static_cast<int>(breaker.BeginLaunch(12000)));
			Assert::AreEqual(static_cast<int>(Decision::Allow),
				static_cast<int>(breaker.BeginLaunch(15001)));
		}

		TEST_METHOD(ProfileChangeOverlayUsesFriendlyOrderedLabels)
		{
			const auto defaultTiming = ProfileChangeOverlay::ResolveTiming(5);
			Assert::IsTrue(defaultTiming.Enabled());
			Assert::AreEqual(5000ull, defaultTiming.totalMilliseconds);
			Assert::AreEqual(4125ull, defaultTiming.holdMilliseconds);
			Assert::AreEqual(875ull, defaultTiming.fadeMilliseconds);
			const auto disabledTiming = ProfileChangeOverlay::ResolveTiming(0);
			Assert::IsFalse(disabledTiming.Enabled());
			Assert::AreEqual(0ull, disabledTiming.totalMilliseconds);

			const std::map<std::string, std::string> previous = {
				{ "display", "rec709_169_med" },
				{ "color", "bt2020" },
				{ "viewport", "viewport_16x9" }
			};
			const std::map<std::string, std::string> current = {
				{ "display", "rec709_scope_med" },
				{ "color", "rec709" },
				{ "viewport", "scope" }
			};
			const auto items = ProfileChangeOverlay::CollectChanges(
				previous, current, "Scope");
			Assert::AreEqual(static_cast<size_t>(3), items.size());
			Assert::AreEqual("Rendering", items[0].label.c_str());
			Assert::AreEqual("Rec709 Scope Med", items[0].value.c_str());
			Assert::AreEqual("Color / Output", items[1].label.c_str());
			Assert::AreEqual("Rec709", items[1].value.c_str());
			Assert::AreEqual("Screen", items[2].label.c_str());
			Assert::AreEqual("Scope", items[2].value.c_str());
		}

		TEST_METHOD(ProfileOverlayIncludesQueueAndConfiguredNlsState)
		{
			const std::map<std::string, std::string> previous = {
				{ "display", "rec709_scope_med" },
				{ "queue", "standard" },
				{ "nls", "off" }
			};
			const std::map<std::string, std::string> current = {
				{ "display", "rec709_scope_med" },
				{ "queue", "low_latency" },
				{ "nls", "on:Nonlinear Stretch Protected" }
			};
			const auto changed = ProfileChangeOverlay::CollectChanges(
				previous, current);
			Assert::AreEqual(static_cast<size_t>(2), changed.size());
			Assert::AreEqual("Queue", changed[0].label.c_str());
			Assert::AreEqual("Low Latency", changed[0].value.c_str());
			Assert::AreEqual("NLS", changed[1].label.c_str());
			Assert::AreEqual("Nonlinear Stretch Protected",
				changed[1].value.c_str());
			Assert::IsTrue(changed[1].indicator ==
				ProfileChangeOverlay::Item::Indicator::On);

			const auto all = ProfileChangeOverlay::CollectAll(current);
			Assert::AreEqual(static_cast<size_t>(3), all.size());
			Assert::AreEqual("Rendering", all[0].label.c_str());
			Assert::AreEqual("Queue", all[1].label.c_str());
			Assert::AreEqual("NLS", all[2].label.c_str());

			const auto disabled = ProfileChangeOverlay::CollectChanges(
				current, previous);
			Assert::AreEqual(static_cast<size_t>(2), disabled.size());
			Assert::AreEqual("Off", disabled[1].value.c_str());
			Assert::IsTrue(disabled[1].indicator ==
				ProfileChangeOverlay::Item::Indicator::Off);

			const auto removed = ProfileChangeOverlay::CollectChanges(
				current, { { "display", "rec709_scope_med" },
					{ "queue", "low_latency" } });
			Assert::AreEqual(static_cast<size_t>(1), removed.size());
			Assert::AreEqual("NLS", removed.front().label.c_str());
			Assert::AreEqual("Off", removed.front().value.c_str());
			Assert::IsTrue(removed.front().indicator ==
				ProfileChangeOverlay::Item::Indicator::Off);
		}

		TEST_METHOD(ProfileOverlayDerivesShaderOffFromFilesNotNames)
		{
			RendererProfileConfig::Model model;
			RendererProfileConfig::Profile empty;
			empty.group = "nls";
			empty.name = "cinema";
			empty.label = "Cinema stretch";
			model.profiles.emplace("nls.cinema", empty);
			Assert::AreEqual("off", ProfileChangeOverlay::ShaderSelectionStatus(
				model, "nls", { "cinema" }).c_str(),
				L"A named profile with no shader files must be Off");

			RendererProfileConfig::Profile namedOff;
			namedOff.group = "nls";
			namedOff.name = "off";
			namedOff.label = "Off";
			namedOff.settings["hlsl_file"] = "NLS.hlsl";
			model.profiles.emplace("nls.off", namedOff);
			Assert::AreEqual("on:Off", ProfileChangeOverlay::ShaderSelectionStatus(
				model, "nls", { "off" }).c_str(),
				L"The word Off must not disable a configured shader profile");

			RendererProfileConfig::Profile sharpen;
			sharpen.group = "standard_shaders";
			sharpen.name = "adaptive_sharpen";
			sharpen.settings["glsl_file"] = "sharpen.glsl";
			model.profiles.emplace("standard_shaders.empty", empty);
			model.profiles.emplace("standard_shaders.adaptive_sharpen", sharpen);
			Assert::AreEqual("on:Adaptive Sharpen",
				ProfileChangeOverlay::ShaderSelectionStatus(model,
					"standard_shaders",
					{ "empty", "adaptive_sharpen" }).c_str());
		}

		TEST_METHOD(ProfileOverlayOmitsGroupsWithoutAChoice)
		{
			const std::map<std::string, std::string> selections = {
				{ "display", "cinema" },
				{ "queue", "normal" },
				{ "scaling", "sharp" },
				{ "nls", "on:Protected" }
			};
			const std::map<std::string, size_t> optionCounts = {
				{ "display", 3 },
				{ "queue", 1 },
				{ "scaling", 0 },
				{ "nls", 2 }
			};
			const auto filtered =
				ProfileChangeOverlay::FilterSingleOptionGroups(
					selections, optionCounts);
			Assert::AreEqual(static_cast<size_t>(2), filtered.size());
			Assert::IsTrue(filtered.find("display") != filtered.end());
			Assert::IsTrue(filtered.find("nls") != filtered.end());
			Assert::IsTrue(filtered.find("queue") == filtered.end());
			Assert::IsTrue(filtered.find("scaling") == filtered.end());
		}

		TEST_METHOD(ProfileOverlaySkipsResolutionWhenSelectionsAreUnchanged)
		{
			const RendererProfileConfig::Selections selections = {
				{ "display", { "cinema" } },
				{ "queue", { "low_latency" } }
			};
			Assert::IsTrue(ProfileChangeOverlay::NeedsSelectionResolution(
				false, selections, selections));
			Assert::IsFalse(ProfileChangeOverlay::NeedsSelectionResolution(
				true, selections, selections));

			auto changed = selections;
			changed["queue"] = { "normal" };
			Assert::IsTrue(ProfileChangeOverlay::NeedsSelectionResolution(
				true, selections, changed));
			Assert::IsTrue(ProfileChangeOverlay::NeedsSelectionResolution(
				true, selections, selections, true));
		}

		TEST_METHOD(LibplaceboPluginApiVersionCoversConfigurationSnapshots)
		{
			// 25: IVideoRenderer::GetDejudderStatus (stage 6b-2b) or rc4's Beta
			// flavor export; 26: both (rc4 merge); 27: the LED drive's
			// projector connection test (Suspend/ResumeLedDriveForTest).
			Assert::AreEqual(static_cast<uint32_t>(27),
				VP_LIBPLACEBO_PLUGIN_API_VERSION);
		}

		TEST_METHOD(ProfileChangeDisplayDurationIsBoundedAndLive)
		{
			CachedConfigTestFile temporaryFile;
			const auto& path = temporaryFile.path;
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				Assert::IsTrue(file.is_open());
				file << "[general]\nprofile_change_display_seconds: 60\n"
					"[vprenderer]\nquality: high\n";
				file.close();
				Assert::IsFalse(file.fail());
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(ConfigurationApplyPolicy::Action::ApplyInterface ==
				ConfigurationApplyPolicy::ClassifyChange(
					{ "general", "profile_change_display_seconds" }));
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				Assert::IsTrue(file.is_open());
				file << "[general]\nprofile_change_display_seconds: 61\n"
					"[vprenderer]\nquality: high\n";
				file.close();
				Assert::IsFalse(file.fail());
			}
			// Explicit reload validation uses Fresh in production; a same-length
			// edit must not depend on filesystem timestamp resolution in this test.
			Assert::IsTrue(config.Load(path, ConfigFile::ReadPolicy::Fresh));
			std::string reloadedDuration;
			Assert::IsTrue(config.TryGetString("general", "profile_change_display_seconds", reloadedDuration));
			Assert::AreEqual(std::string("61"), reloadedDuration);
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("profile_change_display_seconds") !=
				std::string::npos, std::wstring(error.begin(), error.end()).c_str());
		}

		TEST_METHOD(EventActionArgumentsExpandAllSupportedValues)
		{
			RendererProfileConfig::Model::EventAction action;
			action.arguments =
				"${eotf}|${previous.eotf}|${profile.viewport}|${event}|${event_reason}";
			RendererProfileConfig::Model::EventAction expanded;
			std::string error;
			Assert::IsTrue(EventActionLauncher::ExpandArgumentVariables(action,
				[](const std::string& variable, std::string& value)
				{
					const std::map<std::string, std::string> values = {
						{ "eotf", "pq" },
						{ "previous.eotf", "sdr" },
						{ "profile.viewport", "scope" },
						{ "event", "source.eotf.changed" },
						{ "event_reason", "source" }
					};
					const auto found = values.find(variable);
					if (found == values.end()) return false;
					value = found->second;
					return true;
				}, expanded, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual("pq|sdr|scope|source.eotf.changed|source",
				expanded.arguments.c_str());

			action.arguments = "${missing}";
			Assert::IsFalse(EventActionLauncher::ExpandArgumentVariables(action,
				[](const std::string&, std::string&) { return false; },
				expanded, error));
			Assert::IsTrue(error.find("unavailable") != std::string::npos);
		}

		TEST_METHOD(UnifiedActionsRejectArgumentVariableForWrongEvent)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-invalid-unified-action-argument.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[actions.invalid]\n"
					"on: source.eotf.changed\n"
					"when: $eotf==\"pq\"\n"
					"run: C:\\Windows\\System32\\cmd.exe ${actual_refresh}\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsFalse(RendererProfileConfig::Read(config, model, error));
			Assert::IsTrue(error.find("cannot expand variable") !=
				std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(UnifiedActionsRejectValueEncodedEventNames)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-invalid-unified-action.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[actions.invalid]\n"
					"on: source.eotf.pq\n"
					"when: $eotf==\"pq\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsFalse(RendererProfileConfig::Read(config, model, error));
			Assert::IsTrue(error.find("unsupported event") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(UnifiedActionsRouteByBuiltInRendererAliasOrWildcard)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-unified-action-renderer.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[renderer_alias]\ncinema_renderer: 2\n"
					"[actions.built_in]\n"
					"on: renderer.ready\n"
					"when: $event_reason==\"renderer_ready\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.named]\n"
					"renderer: cinema_renderer\n"
					"on: source.eotf.changed\n"
					"when: $eotf==\"pq\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.indexed]\n"
					"renderer: 3\n"
					"on: renderer.ready\n"
					"when: $event_reason==\"renderer_ready\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n"
					"[actions.all]\n"
					"renderer: *\n"
					"on: state.committed\n"
					"when: $eotf==\"sdr\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(4), model.actions.size());
			Assert::AreEqual("vprenderer", model.actions[0].renderer.c_str());
			Assert::AreEqual(0, model.actions[0].rendererSelectorIndex);
			Assert::AreEqual("cinema_renderer", model.actions[1].renderer.c_str());
			Assert::AreEqual(2, model.actions[1].rendererSelectorIndex);
			Assert::AreEqual("3", model.actions[2].renderer.c_str());
			Assert::AreEqual(3, model.actions[2].rendererSelectorIndex);
			Assert::AreEqual("*", model.actions[3].renderer.c_str());
			Assert::AreEqual(0, model.actions[3].rendererSelectorIndex);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(UnifiedActionsRejectUnknownRendererAliasAndLegacyScope)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-invalid-unified-action-renderer.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[actions.unknown_renderer]\n"
					"renderer: not_configured\n"
					"on: renderer.ready\n"
					"when: $event_reason==\"renderer_ready\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsFalse(RendererProfileConfig::Read(config, model, error));
			Assert::IsTrue(error.find("renderer") != std::string::npos);

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[actions.legacy_scope]\n"
					"scope: vprenderer\n"
					"on: renderer.ready\n"
					"when: $event_reason==\"renderer_ready\"\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n";
			}
			Assert::IsTrue(config.Load(path));
			error.clear();
			Assert::IsFalse(RendererProfileConfig::Read(config, model, error));
			Assert::IsTrue(error.find("unknown key 'scope'") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0079FirstNamedVariantIsDefaultAndInheritedBaseline)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0079-first-variant.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[vprenderer.rec709]\nwhen: $key==\"F4\"\nquality: high\n"
					"sdr_target_primaries: REC709\nreport_bt2020_to_display: false\n"
					"switch_refresh_rate: true\n"
					"[vprenderer.bt2020]\nwhen: $key==\"F5\"\n"
					"sdr_target_primaries: BT2020\nreport_bt2020_to_display: true\n"
					"[directshow.ppm]\nppm: AUTO\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			const std::vector<std::string> sections = config.GetSectionNames();
			Assert::AreEqual(static_cast<size_t>(4), sections.size());
			Assert::AreEqual("general", sections[0].c_str());
			Assert::AreEqual("vprenderer.rec709", sections[1].c_str());
			Assert::AreEqual("vprenderer.bt2020", sections[2].c_str());
			Assert::AreEqual("directshow.ppm", sections[3].c_str());

			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(1), model.groups.size());
			const RendererProfileConfig::Group& display = model.groups.front();
			Assert::AreEqual("display", display.name.c_str());
			Assert::AreEqual("rec709", display.defaultSelection.c_str());
			Assert::AreEqual("rec709", display.profiles[0].c_str());
			Assert::AreEqual("bt2020", display.profiles[1].c_str());

			const auto bt2020 = model.profiles.find("display.bt2020");
			Assert::IsTrue(bt2020 != model.profiles.end());
			Assert::AreEqual("high", bt2020->second.settings.at("quality").c_str());
			Assert::AreEqual("bt2020",
				ConfigFile::NormalizeName(
					bt2020->second.settings.at("target_primaries")).c_str());
			Assert::AreEqual("true",
				ConfigFile::NormalizeName(
					bt2020->second.settings.at("report_bt2020_to_display")).c_str());

			std::vector<RendererProfileConfig::AutomaticSelection> automatic;
			Assert::IsTrue(RendererProfileConfig::SelectAutomatic(model,
				[](const std::string&, std::string&) { return false; },
				automatic, error));
			Assert::AreEqual(static_cast<size_t>(1), automatic.size());
			Assert::AreEqual("rec709", automatic.front().profiles.front().c_str());
			Assert::IsTrue(automatic.front().configuredDefault);

			std::vector<RendererProfileConfig::KeySelection> selections;
			Assert::IsTrue(RendererProfileConfig::SelectForKey(model, "F4",
				[](const std::string&, std::string&) { return false; },
				selections, error));
			Assert::AreEqual("rec709", selections.front().profiles.front().c_str());
			Assert::IsTrue(RendererProfileConfig::SelectForKey(model, "F5",
				[](const std::string&, std::string&) { return false; },
				selections, error));
			Assert::AreEqual("bt2020", selections.front().profiles.front().c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0097NamedViewportsUseFileOrderAndIgnoreLabels)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0097-named-viewports.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[vprenderer]\nquality: balanced\n"
					"[vprenderer.viewport.scope_cinema]\n"
					"label: Scope Cinema 2.35:1\n"
					"mode: scope\n"
					"screen_aspect: 2.35:1\n"
					"automatic_crop: true\n"
					"[vprenderer.viewport.flat]\n"
					"label: Flat View\n"
					"when: $key==\"F2\"\n"
					"subtitle_fit: true\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model model;
			const bool read = RendererProfileConfig::Read(config, model, error);
			if (!read)
				Logger::WriteMessage(("RendererProfileConfig::Read: " + error).c_str());
			Assert::IsTrue(read, std::wstring(error.begin(), error.end()).c_str());
			const auto viewport = std::find_if(model.groups.begin(), model.groups.end(),
				[](const RendererProfileConfig::Group& group)
				{ return group.name == "viewport"; });
			Assert::IsTrue(viewport != model.groups.end());
			Assert::AreEqual("scope_cinema", viewport->defaultSelection.c_str());
			Assert::AreEqual("scope_cinema", viewport->profiles[0].c_str());
			Assert::AreEqual("flat", viewport->profiles[1].c_str());
			const auto flat = model.profiles.find("viewport.flat");
			Assert::IsTrue(flat != model.profiles.end());
			Assert::AreEqual("2.35:1", flat->second.settings.at("screen_aspect").c_str());
			Assert::IsTrue(flat->second.settings.find("label") == flat->second.settings.end());
			const auto scopeCinema = model.profiles.find("viewport.scope_cinema");
			Assert::IsTrue(scopeCinema != model.profiles.end());
			Assert::IsTrue(scopeCinema->second.settings.find("mode") ==
				scopeCinema->second.settings.end(),
				L"Legacy viewport mode must never masquerade as an applied setting.");
			Assert::IsTrue(std::any_of(model.warnings.begin(), model.warnings.end(),
				[](const std::string& warning)
				{ return warning.find("key 'mode' is deprecated and ignored") != std::string::npos; }),
				L"The model must warn that screen_aspect replaces viewport mode.");

			std::vector<RendererProfileConfig::AutomaticSelection> automatic;
			Assert::IsTrue(RendererProfileConfig::SelectAutomatic(model,
				[](const std::string&, std::string&) { return false; }, automatic, error));
			const auto automaticViewport = std::find_if(automatic.begin(), automatic.end(),
				[](const RendererProfileConfig::AutomaticSelection& selection)
				{ return selection.group == "viewport"; });
			Assert::IsTrue(automaticViewport != automatic.end());
			Assert::AreEqual("scope_cinema",
				automaticViewport->profiles.front().c_str());
			Assert::IsTrue(automaticViewport->configuredDefault);

			std::vector<RendererProfileConfig::KeySelection> selections;
			Assert::IsTrue(RendererProfileConfig::SelectForKey(model, "F2",
				[](const std::string&, std::string&) { return false; }, selections, error));
			const auto selectedViewport = std::find_if(selections.begin(), selections.end(),
				[](const RendererProfileConfig::KeySelection& selection)
				{ return selection.group == "viewport"; });
			Assert::IsTrue(selectedViewport != selections.end());
			Assert::AreEqual("flat", selectedViewport->profiles.front().c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0123InputProcessingChildIsNotADisplayProfile)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0123-input-processing-child.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[vprenderer]\nquality: balanced\n"
					"[vprenderer.input_processing]\n"
					"video_conversion: none\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			const auto display = std::find_if(model.groups.begin(), model.groups.end(),
				[](const RendererProfileConfig::Group& group)
				{ return group.name == "display"; });
			Assert::IsTrue(display != model.groups.end());
			Assert::AreEqual(static_cast<size_t>(1), display->profiles.size());
			Assert::AreEqual("base", display->profiles.front().c_str());
			const auto base = model.profiles.find("display.base");
			Assert::IsTrue(base != model.profiles.end());
			Assert::IsTrue(base->second.settings.find("video_conversion") ==
				base->second.settings.end(),
				L"Input processing was misclassified as display-profile state.");
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(RenderingProfilesUseExactParsedNames)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-rendering-profile-names.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[vprenderer.Rec709_Scope_Med]\nquality: high\n"
					"[vprenderer.BT2020_Scope_High]\nquality: high\n"
					"[vprenderer.viewport.scope]\nscreen_aspect: 2.35:1\n"
					"[vprenderer.output.Default]\n"
					"[vprenderer.input_processing]\nvideo_conversion: none\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::vector<std::string> profiles;
			std::string error;
			Assert::IsTrue(RendererProfileConfig::CollectRenderingProfileNames(
				config, profiles, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(2), profiles.size());
			// ConfigFile defines profile identifiers as normalized, case-insensitive
			// names. Consumers must use the model's identifiers verbatim instead
			// of deriving a second list from section text.
			Assert::AreEqual("rec709_scope_med", profiles[0].c_str());
			Assert::AreEqual("bt2020_scope_high", profiles[1].c_str());
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error));
			Assert::IsTrue(model.profiles.find("display." + profiles[0]) !=
				model.profiles.end());
			Assert::IsTrue(model.profiles.find("display." + profiles[1]) !=
				model.profiles.end());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0097ShortcutKeyCombinesWithOptionalProfileRule)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0097-shortcut-profile.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[queue]\nshortcut: l\nqueue_size: 32\nlead_frames: 4\ntarget_frames: 3\n"
					"[queue.low_latency]\nwhen: $source_rate > 30\nshortcut: Shift+L\nqueue_size: 1\ntarget_frames: 1\n"
					"[vprenderer]\nshortcut: F4\nquality: high\n"
					"[vprenderer.cinema]\nwhen: $transfer == \"PQ\"\nshortcut: F6\n"
					"sdr_target_primaries: BT2020\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());

			std::vector<RendererProfileConfig::KeySelection> selections;
			Assert::IsTrue(RendererProfileConfig::SelectForKey(model, "F6",
				[](const std::string&, std::string&) { return false; }, selections, error));
			Assert::IsTrue(std::any_of(selections.begin(), selections.end(),
				[](const RendererProfileConfig::KeySelection& selected)
					{ return selected.group == "display" &&
						selected.profiles == RendererProfileConfig::Selection{ "cinema" }; }));
			Assert::IsTrue(RendererProfileConfig::SelectForKey(model, "Shift+L",
				[](const std::string&, std::string&) { return false; }, selections, error));
			Assert::IsTrue(std::any_of(selections.begin(), selections.end(),
				[](const RendererProfileConfig::KeySelection& selected)
					{ return selected.group == "queue" &&
						selected.profiles == RendererProfileConfig::Selection{ "low_latency" }; }));
			std::string canonical;
			Assert::IsTrue(RendererProfileConfig::CanonicalizeKeyChord(
				"Control+f2", canonical));
			Assert::AreEqual("Ctrl+F2", canonical.c_str());
			Assert::IsTrue(RendererProfileConfig::CanonicalizeKeyChord(
				"ShIfT+l", canonical));
			Assert::AreEqual("Shift+L", canonical.c_str());
			Assert::IsTrue(RendererProfileConfig::CanonicalizeKeyChord("l", canonical));
			Assert::AreEqual("L", canonical.c_str());
			Assert::IsTrue(RendererProfileConfig::CanonicalizeKeyChord("L", canonical));
			Assert::AreEqual("L", canonical.c_str());
			Assert::IsTrue(RendererProfileConfig::CanonicalizeKeyChord(
				"cTrL+sHiFt+l", canonical));
			Assert::AreEqual("Ctrl+Shift+L", canonical.c_str());

			std::vector<RendererProfileConfig::AutomaticSelection> automatic;
			Assert::IsTrue(RendererProfileConfig::SelectAutomatic(model,
				[](const std::string& variable, std::string& value)
				{
					if (variable == "transfer") { value = "PQ"; return true; }
					if (variable == "source_rate") { value = "24"; return true; }
					return false;
				}, automatic, error));
			Assert::IsTrue(std::any_of(automatic.begin(), automatic.end(),
				[](const RendererProfileConfig::AutomaticSelection& selected)
					{ return selected.group == "display" &&
						selected.profiles == RendererProfileConfig::Selection{ "cinema" }; }));

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[vprenderer]\nquality: high\n"
					"[vprenderer.bad]\nshortcut: Ctrl+Shift\nquality: balanced\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(RendererProfileConfig::Read(config, model, error));
			Assert::IsTrue(error.find("shortcut") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(AutomaticQueueProfilesMayUseRendererAndActualRefresh)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory),
				temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-queue-renderer-refresh.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\nqueue_size: 16\n"
					"[queue.madvr]\nwhen: ${renderer}==\"madVR\"\nqueue_size: 32\n"
					"[queue.vp_60]\nwhen: ${renderer}==\"VP Renderer\" && ${actual_refresh}>=59\nqueue_size: 8\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			auto selectQueue = [&](const char* renderer, const char* refresh)
			{
				std::vector<RendererProfileConfig::AutomaticSelection> selections;
				Assert::IsTrue(RendererProfileConfig::SelectAutomatic(model,
					[renderer, refresh](const std::string& variable, std::string& value)
					{
						if (variable == "renderer") { value = renderer; return true; }
						if (variable == "actual_refresh") { value = refresh; return true; }
						return false;
					}, selections, error));
				const auto queue = std::find_if(selections.begin(), selections.end(),
					[](const RendererProfileConfig::AutomaticSelection& selected)
					{ return selected.group == "queue"; });
				Assert::IsTrue(queue != selections.end());
				return queue->profiles.front();
			};
			Assert::AreEqual("madvr", selectQueue("madVR", "59.94").c_str());
			Assert::AreEqual("vp_60", selectQueue("VP Renderer", "59.94").c_str());
			Assert::AreEqual("base", selectQueue("VP Renderer", "23.976").c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(InvalidSourceContextRetainsCompatibleQueueInsteadOfPersistedMadVRFallback)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory),
				temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-invalid-source-retains-queue.cfg";
			const std::string statePath = path.substr(0,
				path.find_last_of('.')) + ".state";
			DeleteFileA(path.c_str());
			DeleteFileA(statePath.c_str());
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\npersist_profile_selection: true\n[queue.base]\nqueue_size: 16\n"
					"[queue.madvr_queue]\nwhen: ${renderer}==\"DirectShow - madVR\"\nqueue_size: 3\n"
					"[queue.vp_60]\nwhen: ${renderer}==\"VP Renderer\" && ${source_rate}>30\nqueue_size: 32\n"
					"[queue.vp_24]\nwhen: ${renderer}==\"VP Renderer\" && ${source_rate}<=30\nqueue_size: 8\n";
			}
			{
				std::ofstream file(statePath, std::ios::out | std::ios::trunc);
				file << "# Managed by VideoProcessor.\nprofile.queue: madvr_queue\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			auto source = [](const char* renderer, const char* sourceRate)
			{
				return [renderer, sourceRate](const std::string& variable,
					std::string& value)
				{
					if (variable == "renderer")
					{
						value = renderer;
						return true;
					}
					if (variable == "source_rate" && sourceRate)
					{
						value = sourceRate;
						return true;
					}
					return false;
				};
			};

			UnifiedProfileRuntime::Runtime runtime;
			std::string error;
			Assert::IsTrue(runtime.Initialize(config, source("VP Renderer", "59"), error),
				std::wstring(error.begin(), error.end()).c_str());
			const auto steady = runtime.GetSnapshot();
			Assert::IsTrue(steady != nullptr);
			Assert::AreEqual("madvr_queue", RendererProfileConfig::FormatSelection(
				steady->manualSelections.at("queue")).c_str());
			Assert::AreEqual("vp_60", RendererProfileConfig::FormatSelection(
				steady->effectiveSelections.at("queue")).c_str());

			UnifiedProfileRuntime::RefreshResult invalid;
			Assert::IsTrue(runtime.Refresh(source("VP Renderer", nullptr), false,
				invalid, error), std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(invalid.deferred);
			Assert::IsFalse(invalid.changed);
			Assert::IsTrue(invalid.actions.empty());
			Assert::IsTrue(invalid.snapshot == steady);
			Assert::AreEqual("vp_60", RendererProfileConfig::FormatSelection(
				invalid.snapshot->effectiveSelections.at("queue")).c_str());

			UnifiedProfileRuntime::RefreshResult recovered;
			Assert::IsTrue(runtime.Refresh(source("VP Renderer", "24"), recovered,
				error), std::wstring(error.begin(), error.end()).c_str());
			Assert::IsFalse(recovered.deferred);
			Assert::IsTrue(recovered.changed);
			Assert::AreEqual("vp_24", RendererProfileConfig::FormatSelection(
				recovered.snapshot->effectiveSelections.at("queue")).c_str());

			std::ifstream persistedState(statePath);
			const std::string persistedText((std::istreambuf_iterator<char>(persistedState)),
				std::istreambuf_iterator<char>());
			Assert::IsTrue(persistedText.find("profile.queue: madvr_queue") !=
				std::string::npos);
			DeleteFileA(statePath.c_str());
			DeleteFileA(path.c_str());
		}


		TEST_METHOD(AutomaticQueueRuleOverridesPersistedQueueSelection)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory),
				temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-automatic-queue-overrides-state.cfg";
			const std::string statePath = path.substr(0,
				path.find_last_of('.')) + ".state";
			DeleteFileA(path.c_str());
			DeleteFileA(statePath.c_str());
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\npersist_profile_selection: true\n[queue.base]\nqueue_size: 16\n"
					"[queue.vp_24]\nwhen: ${renderer}==\"VP Renderer\" && ${source_rate}<=30\nqueue_size: 8\n"
					"[queue.vp_60]\nwhen: ${renderer}==\"VP Renderer\" && ${source_rate}>30\nqueue_size: 32\n"
					"[queue.low_latency]\nshortcut: Shift+L\nqueue_size: 1\n";
			}
			{
				std::ofstream file(statePath, std::ios::out | std::ios::trunc);
				file << "# Managed by VideoProcessor.\nprofile.queue: vp_24\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			UnifiedProfileRuntime::Runtime runtime;
			std::string error;
			Assert::IsTrue(runtime.Initialize(config,
				[](const std::string& variable, std::string& value)
				{
					if (variable == "renderer") { value = "VP Renderer"; return true; }
					if (variable == "source_rate") { value = "59"; return true; }
					return false;
				}, error), std::wstring(error.begin(), error.end()).c_str());
			const std::shared_ptr<const UnifiedProfileRuntime::Snapshot> snapshot =
				runtime.GetSnapshot();
			Assert::IsTrue(snapshot != nullptr);
			Assert::AreEqual("vp_24", RendererProfileConfig::FormatSelection(
				snapshot->manualSelections.at("queue")).c_str());
			Assert::AreEqual("vp_60", RendererProfileConfig::FormatSelection(
				snapshot->effectiveSelections.at("queue")).c_str());

			UnifiedProfileRuntime::Runtime fallbackRuntime;
			Assert::IsTrue(fallbackRuntime.Initialize(config,
				[](const std::string& variable, std::string& value)
				{
					if (variable == "renderer") { value = "VP Renderer"; return true; }
					return false;
				}, error), std::wstring(error.begin(), error.end()).c_str());
			const std::shared_ptr<const UnifiedProfileRuntime::Snapshot> fallback =
				fallbackRuntime.GetSnapshot();
			Assert::IsTrue(fallback != nullptr);
			Assert::AreEqual("vp_24", RendererProfileConfig::FormatSelection(
				fallback->effectiveSelections.at("queue")).c_str());

			UnifiedProfileRuntime::SelectionResult selection;
			Assert::IsTrue(runtime.SelectKey("Shift+L",
				[](const std::string& variable, std::string& value)
				{
					if (variable == "renderer") { value = "VP Renderer"; return true; }
					if (variable == "source_rate") { value = "59"; return true; }
					return false;
				}, selection, error), std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(selection.snapshot != nullptr);
			Assert::AreEqual("low_latency",
				RendererProfileConfig::FormatSelection(
					selection.snapshot->effectiveSelections.at("queue")).c_str());
			UnifiedProfileRuntime::RefreshResult refreshed;
			Assert::IsTrue(runtime.Refresh(
				[](const std::string& variable, std::string& value)
				{
					if (variable == "renderer") { value = "VP Renderer"; return true; }
					if (variable == "source_rate") { value = "59"; return true; }
					return false;
				}, refreshed, error), std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual("low_latency",
				RendererProfileConfig::FormatSelection(
					refreshed.snapshot->effectiveSelections.at("queue")).c_str());

			UnifiedProfileRuntime::RefreshResult reapplied;
			std::vector<std::string> clearedGroups;
			Assert::IsTrue(runtime.ReapplyRules(
				[](const std::string& variable, std::string& value)
				{
					if (variable == "renderer") { value = "VP Renderer"; return true; }
					if (variable == "source_rate") { value = "59"; return true; }
					return false;
				}, reapplied, clearedGroups, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(reapplied.changed);
			Assert::AreEqual(static_cast<size_t>(1), clearedGroups.size());
			Assert::AreEqual("queue", clearedGroups.front().c_str());
			Assert::AreEqual("vp_60",
				RendererProfileConfig::FormatSelection(
					reapplied.snapshot->effectiveSelections.at("queue")).c_str());
			Assert::AreEqual("low_latency",
				RendererProfileConfig::FormatSelection(
					reapplied.snapshot->manualSelections.at("queue")).c_str());

			std::ifstream persistedState(statePath);
			const std::string persistedText((std::istreambuf_iterator<char>(persistedState)),
				std::istreambuf_iterator<char>());
			Assert::IsTrue(persistedText.find("profile.queue: low_latency") !=
				std::string::npos);

			UnifiedProfileRuntime::RefreshResult idempotent;
			clearedGroups.clear();
			Assert::IsTrue(runtime.ReapplyRules(
				[](const std::string& variable, std::string& value)
				{
					if (variable == "renderer") { value = "VP Renderer"; return true; }
					if (variable == "source_rate") { value = "59"; return true; }
					return false;
				}, idempotent, clearedGroups, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsFalse(idempotent.changed);
			Assert::IsTrue(clearedGroups.empty());
			Assert::AreEqual("vp_60",
				RendererProfileConfig::FormatSelection(
					idempotent.snapshot->effectiveSelections.at("queue")).c_str());

			UnifiedProfileRuntime::RefreshResult rememberedFallback;
			Assert::IsTrue(runtime.Refresh(
				[](const std::string& variable, std::string& value)
				{
					if (variable == "renderer") { value = "VP Renderer"; return true; }
					return false;
				}, rememberedFallback, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(rememberedFallback.changed);
			Assert::AreEqual("low_latency", RendererProfileConfig::FormatSelection(
				rememberedFallback.snapshot->effectiveSelections.at("queue")).c_str());

			UnifiedProfileRuntime::Runtime restartedRuntime;
			Assert::IsTrue(restartedRuntime.Initialize(config,
				[](const std::string& variable, std::string& value)
				{
					if (variable == "renderer") { value = "VP Renderer"; return true; }
					if (variable == "source_rate") { value = "59"; return true; }
					return false;
				}, error), std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual("vp_60", RendererProfileConfig::FormatSelection(
				restartedRuntime.GetSnapshot()->effectiveSelections.at("queue")).c_str());
			DeleteFileA(statePath.c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0097LegacyViewportRootRetainsPrecedenceAndRejectsDefaultId)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0097-legacy-viewport.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[vprenderer]\nquality: balanced\n"
					"[vprenderer.viewport.scope]\nwhen: $key==\"F2\"\n"
					"screen_aspect: 2.35:1\n"
					"[vprenderer.viewport]\nlabel: Legacy base\n"
					"screen_aspect: 16:9\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			const auto viewport = std::find_if(model.groups.begin(), model.groups.end(),
				[](const RendererProfileConfig::Group& group)
				{ return group.name == "viewport"; });
			Assert::IsTrue(viewport != model.groups.end());
			Assert::AreEqual("base", viewport->defaultSelection.c_str());
			Assert::AreEqual("base", viewport->profiles[0].c_str());

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[vprenderer]\nquality: balanced\n"
					"[vprenderer.viewport.default]\nscreen_aspect: 16:9\n";
			}
			Assert::IsTrue(config.Load(path));
			error.clear();
			Assert::IsFalse(RendererProfileConfig::Read(config, model, error));
			Assert::IsTrue(error.find("default") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0097ProfileFileOrderIsPriorityAndRootShortcutSelectsDefault)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0097-ordered-profiles.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[vprenderer]\nshortcut: F4\nquality: balanced\n"
					"[vprenderer.first]\nwhen: ${transfer} == \"PQ\"\n"
					"priority: -100\nquality: fast\n"
					"[vprenderer.second]\nwhen: ${transfer} == \"PQ\"\n"
					"priority: 100\nquality: high\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsFalse(model.warnings.empty());

			std::vector<RendererProfileConfig::AutomaticSelection> automatic;
			Assert::IsTrue(RendererProfileConfig::SelectAutomatic(model,
				[](const std::string& variable, std::string& value)
				{
					if (variable == "transfer") { value = "PQ"; return true; }
					return false;
				}, automatic, error));
			const auto display = std::find_if(automatic.begin(), automatic.end(),
				[](const RendererProfileConfig::AutomaticSelection& selection)
					{ return selection.group == "display"; });
			Assert::IsTrue(display != automatic.end());
			Assert::AreEqual("first", display->profiles.front().c_str());

			std::vector<RendererProfileConfig::KeySelection> keys;
			Assert::IsTrue(RendererProfileConfig::SelectForKey(model, "f4",
				[](const std::string&, std::string&) { return false; }, keys, error));
			const auto root = std::find_if(keys.begin(), keys.end(),
				[](const RendererProfileConfig::KeySelection& selection)
					{ return selection.group == "display"; });
			Assert::IsTrue(root != keys.end());
			Assert::AreEqual("base", root->profiles.front().c_str());
			Assert::IsFalse(root->resetToAutomatic);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(CheckedInVp0079ConfigurationPassesStartupSchemas)
		{
			std::string path = __FILE__;
			std::replace(path.begin(), path.end(), '/', '\\');
			const std::string marker =
				"\\src\\VideoProcessor-Test\\ConfigFileTests.cpp";
			const size_t markerPosition = path.rfind(marker);
			Assert::IsTrue(markerPosition != std::string::npos);
			path.resize(markerPosition);
			path += "\\VideoProcessor.cfg";

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(config.GetWarnings().empty());
			std::ifstream shippedFile(path, std::ios::in | std::ios::binary);
			std::ostringstream shippedText;
			shippedText << shippedFile.rdbuf();
			const std::string shipped = shippedText.str();
			// The sample ships one rule only: HDR debanding off (planner
			// 2026-10-12, ShippedConfigurationsTurnDebandingOffForHdr).
			const std::string hdrRule = "\n[vprenderer.HDR]\nwhen: $eotf==\"pq\" || $eotf==\"hlg\"\n";
			std::string normalized = shipped;
			normalized.erase(std::remove(normalized.begin(), normalized.end(), '\r'), normalized.end());
			const size_t hdrAt = normalized.find(hdrRule);
			Assert::IsTrue(hdrAt != std::string::npos);
			normalized.erase(hdrAt, hdrRule.size());
			Assert::IsTrue(normalized.find("\nwhen:") == std::string::npos);
			Assert::IsTrue(shipped.find("[actions.") == std::string::npos);
			Assert::IsTrue(shipped.find("[event_actions.") == std::string::npos);
			Assert::IsTrue(shipped.find("[display_rules.") == std::string::npos);

			std::string value;
			Assert::IsTrue(config.TryGetString(
				"general", "capture_device", value));
			Assert::AreEqual(
				"DeckLink Quad HDMI Recorder (1)", value.c_str());
			Assert::IsTrue(config.TryGetString("general", "renderer", value));
			Assert::AreEqual("VP Renderer", value.c_str());
			bool enabled = true;
			Assert::IsTrue(config.TryGetBool("general", "fullscreen", enabled));
			Assert::IsFalse(enabled);
			Assert::IsTrue(config.TryGetBool(
				"general", "windowed_fullscreen_mode", enabled));
			Assert::IsFalse(enabled);
			Assert::IsTrue(config.TryGetBool("general", "noui", enabled));
			Assert::IsFalse(enabled);
			Assert::IsTrue(config.TryGetBool(
				"general", "startminimized", enabled));
			Assert::IsFalse(enabled);
			Assert::IsTrue(config.TryGetBool(
				"general", "scene_detect", enabled));
			Assert::IsFalse(enabled);
			const std::pair<const char*, const char*> shortcuts[] = {
				{ "queue.normal", "Shift+Q" },
				{ "queue.low_latency", "Shift+L" },
				{ "vprenderer.color.rec709", "F5" },
				{ "vprenderer.color.bt2020", "F6" },
				{ "vprenderer.viewport.viewport_16x9", "F3" },
				{ "vprenderer.viewport.scope", "F2" },
				{ "shader.nls.off", "N" },
				{ "shader.nls.standard", "Shift+N" },
				{ "shader.nls.protected", "Shift+P" }
			};
			for (const auto& expected : shortcuts)
			{
				Assert::IsTrue(config.TryGetString(
					expected.first, "shortcut", value));
				Assert::AreEqual(expected.second, value.c_str());
			}
			Assert::IsTrue(config.TryGetString(
				"shader.nls.standard", "stage", value));
			Assert::AreEqual("pre_resize", value.c_str(),
				L"Bundled madVR NLS must run before madVR enlarges the presentation surface");
			Assert::IsTrue(config.TryGetString(
				"shader.nls.standard", "order", value));
			Assert::AreEqual("10", value.c_str());
			Assert::IsTrue(config.TryGetString("shader.standard", "type", value));
			Assert::AreEqual("multi", value.c_str());
			const std::pair<const char*, const char*> standardShaders[] = {
				{ "shader.standard.debanding_mild", "Debanding mild.hlsl" },
				{ "shader.standard.denoise", "Denoise.hlsl" },
				{ "shader.standard.adaptive_sharpen", "Adaptive sharpen.hlsl" },
				{ "shader.standard.invert", "Invert.hlsl" }
			};
			for (const auto& expected : standardShaders)
			{
				Assert::IsTrue(config.TryGetString(expected.first, "hlsl_file", value));
				Assert::AreEqual(expected.second, value.c_str());
				Assert::IsFalse(config.TryGetString(expected.first, "shortcut", value),
					L"Shipped standard shaders must remain opt-in");
				Assert::IsFalse(config.TryGetString(expected.first, "when", value),
					L"Shipped standard shaders must not auto-select");
			}
			std::string shaderError;
			Assert::IsTrue(ShaderConfigValidation::Validate(config, shaderError),
				std::wstring(shaderError.begin(), shaderError.end()).c_str());
			std::vector<ConfiguredShaderRule> configuredShaders;
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				config, "@shader-key:", ShaderRendererBackend::MADVR,
				configuredShaders,
				shaderError),
				std::wstring(shaderError.begin(), shaderError.end()).c_str());
			Assert::IsFalse(std::any_of(configuredShaders.begin(), configuredShaders.end(),
				[](const ConfiguredShaderRule& rule)
				{ return rule.name.rfind("standard.", 0) == 0; }),
				L"An unselected shipped standard shader must not enter the renderer chain");
			Assert::IsTrue(config.TryGetString(
				"shortcuts", "render.1", value));
			Assert::AreEqual("Shift+A", value.c_str());
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsFalse(model.persistSelection, L"Delta 6: key choices last until restart by default");
			// The shipped sample must not invoke machine-local commands.
			Assert::AreEqual(static_cast<size_t>(0), model.actions.size());

			const auto viewportGroup = std::find_if(
				model.groups.begin(), model.groups.end(),
				[](const RendererProfileConfig::Group& group)
				{
					return group.name == "viewport";
				});
			Assert::IsTrue(viewportGroup != model.groups.end());
			Assert::AreEqual("viewport_16x9",
				viewportGroup->defaultSelection.c_str());
			Assert::AreEqual("viewport_16x9",
				viewportGroup->profiles.front().c_str());
			RendererProfileConfig::ResolvedViewport viewport;
			Assert::IsTrue(RendererProfileConfig::ResolveViewport(
				model, viewportGroup->defaultSelection, 1, viewport, error));
			Assert::AreEqual(16ull, viewport.screenAspect.numerator);
			Assert::AreEqual(9ull, viewport.screenAspect.denominator);
			Assert::IsFalse(viewport.automaticCrop);
			Assert::IsFalse(viewport.subtitleFit);
			Assert::IsTrue(RendererProfileConfig::ResolveViewport(
				model, "scope", 2, viewport, error));
			Assert::AreEqual(47ull, viewport.screenAspect.numerator);
			Assert::AreEqual(20ull, viewport.screenAspect.denominator);
			Assert::IsTrue(viewport.automaticCrop);
			Assert::IsFalse(viewport.subtitleFit); // Retired Screen setting is ignored.
		}

		TEST_METHOD(Vp0079EmptyShaderRootResolvesAsExplicitOff)
		{
			Assert::IsTrue(MadVRShaderLoader::RuleSelectorsEqual(
				"@shader-key:N", "@shader-key:n"));
			Assert::IsFalse(MadVRShaderLoader::RuleSelectorsEqual(
				"@shader-key:Shift+n", "@shader-key:n"));
			Assert::IsTrue(MadVRShaderLoader::RuleSelectorsEqual(
				"Legacy_NLS", "legacy_nls"));
			Assert::AreEqual("@shader-key:P",
				MadVRShaderLoader::CanonicalizeRuleSelector(
					" @shader-key:P ").c_str());
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0079-empty-shader-root.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[shader.nls]\n"
					"shortcut: n\n"
					"[shader.nls.standard]\n"
					"shortcut: Shift+n\n"
					"shader_type: nls\n"
					"glsl_file: NLS.glsl\n"
					"[shader.nls.unbound]\n"
					"shader_type: custom\n"
					"hlsl_file: Unbound.hlsl\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::vector<ConfiguredShaderRule> selection;
			std::vector<std::string> activeSections;
			std::string error;
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				config, "@shader-key:n", ShaderRendererBackend::LIBPLACEBO,
				selection, activeSections, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(1), selection.size());
			Assert::IsTrue(selection.front().none,
				L"The empty shader root must explicitly turn Alpha NLS off");
			Assert::AreEqual(static_cast<size_t>(1), activeSections.size());
			Assert::AreEqual("shader.nls", activeSections.front().c_str());

			selection.clear();
			activeSections.clear();
			error.clear();
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				config, "@shader-key:Shift+N", ShaderRendererBackend::LIBPLACEBO,
				selection, activeSections, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(1), selection.size());
			Assert::IsTrue(selection.front().nls,
				L"A shader member shortcut must select its NLS effect");
			Assert::AreEqual(static_cast<size_t>(1), activeSections.size());
			Assert::AreEqual("shader.nls.standard",
				activeSections.front().c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0159ShaderProfilesShareRulesWithoutInheritance)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0159-shader-profiles.cfg";
			const std::string statePath = std::string(temporaryDirectory) +
				"VideoProcessor-vp0159-shader-profiles.state";
			DeleteFileA(statePath.c_str());
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\npersist_profile_selection: true\n[shader.nls]\n"
					"label: Default NLS\n"
					"shader_type: nls\n"
					"strength: 0.4\n"
					"[shader.nls.classic]\n"
					"label: Classic\n"
					"when: ${transfer} == \"HDR\"\n"
					"shortcut: Ctrl+N\n"
					"shader_type: nls\n"
					"[shader.nls.protected]\n"
					"label: Protected\n"
					"when: ${transfer} == \"HDR\"\n"
					"shortcut: Ctrl+P\n"
					"shader_type: nls\n"
					"[shader.standard]\n"
					"type: multi\n"
					"shortcut: Ctrl+O\n"
					"[shader.standard.deband]\n"
					"when: ${transfer} == \"HDR\"\n"
					"shortcut: Ctrl+S\n"
					"shader_type: custom\n"
					"stage: pre_resize\n"
					"order: 90\n"
					"hlsl_file: Deband.hlsl\n"
					"[shader.standard.sharpen]\n"
					"when: ${transfer} == \"HDR\"\n"
					"shortcut: Ctrl+S\n"
					"shader_type: custom\n"
					"stage: pre_resize\n"
					"order: 10\n"
					"hlsl_file: Sharpen.hlsl\n"
					"[actions.shader_changed]\n"
					"on: profile.nls.changed, profile.standard_shaders.changed\n"
					"run: C:\\Windows\\System32\\cmd.exe /c exit 0\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(ShaderConfigValidation::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			std::vector<ConfiguredShaderRule> offSelection;
			std::vector<std::string> activeSections;
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				config, "@shader-profiles:nls|standard",
				ShaderRendererBackend::LIBPLACEBO, offSelection,
				activeSections, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(1), offSelection.size());
			Assert::IsTrue(offSelection.front().none,
				L"Profiles without shader files must resolve to Off");
			Assert::AreEqual(static_cast<size_t>(2), activeSections.size());
			std::vector<ConfiguredShaderRule> orderedStandard;
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				config, "@shader-profiles:standard.deband|standard.sharpen",
				ShaderRendererBackend::MADVR, orderedStandard, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(2), orderedStandard.size());
			Assert::AreEqual("standard.deband",
				orderedStandard[0].name.c_str());
			Assert::AreEqual("standard.sharpen",
				orderedStandard[1].name.c_str());
			Assert::AreEqual(0u,
				orderedStandard[0].profileSelectionOrder);
			Assert::AreEqual(1u,
				orderedStandard[1].profileSelectionOrder);
			Assert::AreEqual(90u, orderedStandard[0].stageOrder);
			Assert::AreEqual(10u, orderedStandard[1].stageOrder,
				L"Legacy stage order must not override Standard profile-list order");
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			const auto classic = model.profiles.find("nls.classic");
			Assert::IsTrue(classic != model.profiles.end());
			Assert::IsTrue(classic->second.settings.find("strength") ==
				classic->second.settings.end(),
				L"Shader profiles must not inherit root settings");

			auto sdr = [](const std::string& name, std::string& value)
			{
				if (name == "transfer") { value = "SDR"; return true; }
				return false;
			};
			auto hdr = [](const std::string& name, std::string& value)
			{
				if (name == "transfer") { value = "HDR"; return true; }
				return false;
			};
			UnifiedProfileRuntime::Runtime runtime;
			Assert::IsTrue(runtime.Initialize(config, sdr, error),
				std::wstring(error.begin(), error.end()).c_str());
			const auto initial = runtime.GetSnapshot();
			Assert::AreEqual("base", RendererProfileConfig::FormatSelection(
				initial->effectiveSelections.at("nls")).c_str());
			Assert::AreEqual("base",
				RendererProfileConfig::FormatSelection(
					initial->effectiveSelections.at("standard_shaders")).c_str());
			UnifiedProfileRuntime::SelectionResult selected;
			Assert::IsTrue(runtime.SelectKey("Ctrl+P", sdr, selected, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual("protected",
				RendererProfileConfig::FormatSelection(
					selected.snapshot->effectiveSelections.at("nls")).c_str());
			Assert::IsTrue(runtime.SelectKey("Ctrl+S", sdr, selected, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual("deband|sharpen",
				RendererProfileConfig::FormatSelection(
					selected.snapshot->effectiveSelections.at("standard_shaders")).c_str(),
				L"A shared standard shortcut replaces the complete selection");
			Assert::IsTrue(runtime.SelectKey("Ctrl+O", sdr, selected, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual("base",
				RendererProfileConfig::FormatSelection(
					selected.snapshot->effectiveSelections.at("standard_shaders")).c_str(),
				L"An empty ordinary profile can replace standard shaders with Off");
			UnifiedProfileRuntime::RefreshResult reapplied;
			std::vector<std::string> clearedGroups;
			Assert::IsTrue(runtime.ReapplyRules(sdr, reapplied, clearedGroups, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual("protected",
				RendererProfileConfig::FormatSelection(
					reapplied.snapshot->effectiveSelections.at("nls")).c_str());

			// A fresh runtime has no live shortcut override. It restores the manual
			// choices, then lets matching rules replace the configured defaults and
			// persisted fallbacks.
			UnifiedProfileRuntime::Runtime restoredRuntime;
			Assert::IsTrue(restoredRuntime.Initialize(config, sdr, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual("protected", RendererProfileConfig::FormatSelection(
				restoredRuntime.GetSnapshot()->effectiveSelections.at("nls")).c_str());
			UnifiedProfileRuntime::RefreshResult refresh;
			Assert::IsTrue(restoredRuntime.Refresh(hdr, refresh, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(refresh.changed);
			Assert::AreEqual("classic",
				RendererProfileConfig::FormatSelection(
					refresh.snapshot->effectiveSelections.at("nls")).c_str());
			Assert::AreEqual("deband|sharpen",
				RendererProfileConfig::FormatSelection(
					refresh.snapshot->effectiveSelections.at("standard_shaders")).c_str());
			Assert::AreEqual(static_cast<size_t>(2), refresh.actions.size(),
				L"Profile-change actions must run for both shader profile groups");

			{
				std::ofstream state(statePath,
					std::ios::out | std::ios::trunc);
				state << "profile.nls: classic|protected\n"
					"profile.standard_shaders: deband|deband\n";
			}
			UnifiedProfileRuntime::Runtime invalidStateRuntime;
			Assert::IsTrue(invalidStateRuntime.Initialize(config, sdr, error),
				std::wstring(error.begin(), error.end()).c_str());
			const auto invalidState = invalidStateRuntime.GetSnapshot();
			Assert::IsTrue(invalidState->manualSelections.find("nls") ==
				invalidState->manualSelections.end());
			Assert::IsTrue(invalidState->manualSelections.find(
				"standard_shaders") == invalidState->manualSelections.end());
			Assert::AreEqual("base", RendererProfileConfig::FormatSelection(
				invalidState->effectiveSelections.at("nls")).c_str(),
				L"A corrupt multi-NLS state must fall back to the configured profile");
			Assert::AreEqual("base", RendererProfileConfig::FormatSelection(
				invalidState->effectiveSelections.at(
					"standard_shaders")).c_str(),
				L"A duplicate Standard state must fall back to the configured profile");

			// Removing the final configured shader profile must still send an
			// explicit empty typed selection to clear a previously active chain.
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\n";
			}
			ConfigFile clearedConfig;
			Assert::IsTrue(clearedConfig.Load(path));
			offSelection.clear();
			activeSections.clear();
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				clearedConfig, "@shader-profiles:",
				ShaderRendererBackend::LIBPLACEBO, offSelection,
				activeSections, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(1), offSelection.size());
			Assert::IsTrue(offSelection.front().none);
			Assert::IsTrue(activeSections.empty());
			DeleteFileA(statePath.c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0159ShaderProfilesResolveRootBaseSelection)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0159-root-base-selection.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[shader.nls]\n"
					"[shader.nls.standard]\n"
					"shader_type: nls\n"
					"glsl_file: NLS.glsl\n"
					"[shader.nls.plus]\n"
					"shader_type: nls\n"
					"glsl_file: NLSPlus.glsl\n"
					"[shader.standard]\n"
					"type: multi\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(ShaderConfigValidation::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			std::vector<ConfiguredShaderRule> selection;
			std::vector<std::string> activeSections;
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				config, "@shader-profiles:nls.plus|standard.base",
				ShaderRendererBackend::LIBPLACEBO, selection, activeSections,
				error), std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(1), selection.size());
			Assert::IsTrue(selection.front().nls);
			Assert::AreEqual("NLSPlus.glsl", selection.front().filename.c_str());
			Assert::AreEqual(static_cast<size_t>(2), activeSections.size());
			Assert::AreEqual("shader.nls.plus", activeSections[0].c_str());
			Assert::AreEqual("shader.standard", activeSections[1].c_str());
			selection.clear();
			activeSections.clear();
			error.clear();
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				config, "@shader-profiles:nls.standard|standard.base",
				ShaderRendererBackend::LIBPLACEBO, selection, activeSections,
				error), std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(1), selection.size());
			Assert::IsTrue(selection.front().nls);
			Assert::AreEqual("NLS.glsl", selection.front().filename.c_str());
			Assert::AreEqual(static_cast<size_t>(2), activeSections.size());
			Assert::AreEqual("shader.nls.standard", activeSections[0].c_str());
			Assert::AreEqual("shader.standard", activeSections[1].c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0159ReorderedNlsFallbackSelectsFirstProfile)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0159-reordered-nls.cfg";
			const std::string statePath = std::string(temporaryDirectory) +
				"VideoProcessor-vp0159-reordered-nls.state";
			DeleteFileA(statePath.c_str());
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[shader.nls.stretch]\n"
					"label: Nonlinear Stretch\n"
					"shader_type: nls\n"
					"hlsl_file: NLS.hlsl\n"
					"glsl_file: NLS.glsl\n"
					"[shader.nls.off]\n"
					"label: Off\n"
					"shader_type: nls\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(ShaderConfigValidation::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			auto noRules = [](const std::string&, std::string&) { return false; };
			UnifiedProfileRuntime::Runtime runtime;
			Assert::IsTrue(runtime.Initialize(config, noRules, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual("stretch", RendererProfileConfig::FormatSelection(
				runtime.GetSnapshot()->effectiveSelections.at("nls")).c_str(),
				L"The first reordered NLS profile must become the fallback");

			std::vector<ConfiguredShaderRule> selection;
			std::vector<std::string> activeSections;
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				config, "@shader-profiles:nls.stretch",
				ShaderRendererBackend::LIBPLACEBO, selection, activeSections,
				error), std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(1), selection.size());
			Assert::IsTrue(selection.front().nls);
			Assert::IsFalse(selection.front().none);
			Assert::AreEqual("NLS.glsl", selection.front().filename.c_str());
			Assert::AreEqual(static_cast<size_t>(1), activeSections.size());
			Assert::AreEqual("shader.nls.stretch",
				activeSections.front().c_str());
			DeleteFileA(statePath.c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0099MaximumStretchRatioIsValidatedAndPublished)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0099-max-stretch.cfg";

			auto resolve = [&](const char* configured,
				double expected, bool valid)
			{
				{
					std::ofstream file(path, std::ios::out | std::ios::trunc);
					file << "[shader.nls]\n"
						"when: $key==\"n\"\n"
						"[shader.nls.standard]\n"
						"when: $key==\"Shift+n\"\n"
						"shader_type: nls\n"
						"glsl_file: NLS.glsl\n"
						"hlsl_file: NLS.hlsl\n";
					if (configured)
						file << "max_stretch_ratio: " << configured << "\n";
				}
				ConfigFile config;
				Assert::IsTrue(config.Load(path));
				std::vector<ConfiguredShaderRule> selection;
				std::string error;
				const bool resolved =
					MadVRShaderLoader::ResolveConfiguredRuleSelection(
						config, "@shader-key:Shift+n", ShaderRendererBackend::LIBPLACEBO,
						selection, error);
				Assert::AreEqual(valid, resolved);
				if (valid)
				{
					Assert::AreEqual(static_cast<size_t>(1), selection.size());
					Assert::AreEqual(expected,
						selection.front().maximumStretchRatio, 0.000001);
					std::vector<ConfiguredShaderRule> madvrSelection;
					Assert::IsTrue(
						MadVRShaderLoader::ResolveConfiguredRuleSelection(
							config, "@shader-key:Shift+n", ShaderRendererBackend::MADVR,
							madvrSelection, error));
					Assert::AreEqual(static_cast<size_t>(1),
						madvrSelection.size());
					Assert::AreEqual(
						selection.front().maximumStretchRatio,
						madvrSelection.front().maximumStretchRatio, 0.000001);
				}
			};

			resolve(nullptr, NLS_DEFAULT_MAXIMUM_STRETCH_RATIO, true);
			resolve("1.0", 1.0, true);
			resolve("1.35", 1.35, true);
			resolve("1.5", 1.5, true);
			resolve("0.99", 0.0, false);
			resolve("1.5001", 0.0, false);
			resolve("not-a-number", 0.0, false);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0099TypedNlsDefaultsToExpansionOnly)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0099-expansion-only.cfg";

			auto resolve = [&](const char* direction,
				NlsAspectDirection expectedDirection)
			{
				{
					std::ofstream file(path, std::ios::out | std::ios::trunc);
					file << "[shader.nls]\n"
						"when: $key==\"n\"\n"
						"[shader.nls.standard]\n"
						"when: $key==\"Shift+n\"\n"
						"shader_type: nls\n"
						"glsl_file: NLS.glsl\n"
						"hlsl_file: NLS.hlsl\n";
					if (direction)
						file << "aspect_direction: " << direction << "\n";
				}
				ConfigFile config;
				Assert::IsTrue(config.Load(path));
				for (const ShaderRendererBackend backend : {
					ShaderRendererBackend::LIBPLACEBO,
					ShaderRendererBackend::MADVR })
				{
					std::vector<ConfiguredShaderRule> selection;
					std::string error;
					Assert::IsTrue(
						MadVRShaderLoader::ResolveConfiguredRuleSelection(
							config, "@shader-key:Shift+n", backend,
							selection, error));
					Assert::AreEqual(static_cast<size_t>(1), selection.size());
					Assert::AreEqual(static_cast<int>(expectedDirection),
						static_cast<int>(selection.front().aspectDirection));
				}
			};

			resolve(nullptr, NlsAspectDirection::NARROWER_ONLY);
			resolve("narrower_only", NlsAspectDirection::NARROWER_ONLY);
			resolve("wider_only", NlsAspectDirection::WIDER_ONLY);
			resolve("any", NlsAspectDirection::ANY);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0089AndVp0131TypedAdvancedNlsSettingsResolveForAlpha)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0089-vp0131-advanced-nls.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[shader.nls]\nshortcut: n\n"
					"[shader.nls.plus]\nshortcut: b\n"
					"shader_type: nls\n"
					"glsl_file: NLSPlus.glsl\n"
					"axis_balance: 0.25\n"
					"max_center_zoom: 1.08\n"
					"horizontal_center_protection: 0.35\n"
					"vertical_center_protection: 0.25\n"
					"aspect_direction: wider_only\n"
					"vprenderer_max_crop_percent: 6\n"
					"vprenderer_crop_preference: minimize_distortion\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(ShaderConfigValidation::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			std::vector<ConfiguredShaderRule> selection;
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				config, "@shader-key:b", ShaderRendererBackend::LIBPLACEBO,
				selection, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(1), selection.size());
			Assert::AreEqual("NLSPlus.glsl",
				selection.front().filename.c_str());
			Assert::AreEqual(static_cast<int>(NlsAspectDirection::WIDER_ONLY),
				static_cast<int>(selection.front().aspectDirection));
			Assert::AreEqual(6.0,
				selection.front().vpRendererMaximumCropPercent, 0.000001);
			Assert::AreEqual(static_cast<int>(
				NlsPresentationCropPreference::MINIMIZE_DISTORTION),
				static_cast<int>(selection.front().vpRendererCropPreference));
			Assert::AreEqual(0.25, std::stod(selection.front().parameters.at(
				"axis_balance")), 0.000001);
			Assert::AreEqual(1.08, std::stod(selection.front().parameters.at(
				"max_center_zoom")), 0.000001);
			Assert::AreEqual(0.35, std::stod(selection.front().parameters.at(
				"horizontal_center_protection")), 0.000001);
			Assert::AreEqual(0.25, std::stod(selection.front().parameters.at(
				"vertical_center_protection")), 0.000001);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0089NlsPlusNewParametersHaveBackwardCompatibleDefaults)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0089-nls-plus-upgrade.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[shader.nls]\nshortcut: n\n"
					"[shader.nls.plus]\nshortcut: b\n"
					"shader_type: nls\n"
					"glsl_file: NLSPlus.glsl\n"
					"axis_balance: 0.5\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			std::vector<ConfiguredShaderRule> selection;
			Assert::IsTrue(MadVRShaderLoader::ResolveConfiguredRuleSelection(
				config, "@shader-key:b", ShaderRendererBackend::LIBPLACEBO,
				selection, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(static_cast<size_t>(1), selection.size());
			Assert::AreEqual(1.08, std::stod(selection.front().parameters.at(
				"max_center_zoom")), 0.000001);
			Assert::AreEqual(0.35, std::stod(selection.front().parameters.at(
				"horizontal_center_protection")), 0.000001);
			Assert::AreEqual(0.25, std::stod(selection.front().parameters.at(
				"vertical_center_protection")), 0.000001);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(Vp0089AndVp0131InvalidAdvancedNlsSettingsAreRejected)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0089-vp0131-invalid-nls.cfg";
			auto rejects = [&](const char* setting)
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[shader.nls]\nshortcut: n\n"
					"[shader.nls.plus]\nshortcut: b\n"
					"shader_type: nls\n"
					"glsl_file: NLSPlus.glsl\n" << setting << "\n";
				file.close();
				ConfigFile config;
				Assert::IsTrue(config.Load(path));
				std::string error;
				Assert::IsFalse(ShaderConfigValidation::Validate(config, error));
			};
			rejects("axis_balance: 1.01");
			rejects("max_center_zoom: 1.251");
			rejects("horizontal_center_protection: 0.451");
			rejects("vertical_center_protection: -0.001");
			rejects("aspect_direction: sideways");
			rejects("vprenderer_max_crop_percent: 10.01");
			rejects("vprenderer_crop_preference: automatic");
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(MainConfigSchemaAcceptsLegacyQueueNamesButRejectsAmbiguousAliases)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-legacy-queue-schema.cfg";
			ConfigFile config;
			std::string error;

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\n"
					"startup_preroll_frames: 0\n"
					"steady_reserve_frames: 2\n"
					"[directshow]\n"
					"presentation_lead_frames: 1\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(MainConfigSchema::Validate(config, error));

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\n"
					"target_frames: 4\n"
					"steady_reserve_frames: 2\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("both") != std::string::npos);

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\nlead_frames: 1\n"
					"[directshow]\npresentation_lead_frames: 1\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("both") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(DirectShowSchemaOwnsAllRendererOverridesAndRejectsStartupKeys)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-directshow-schema.cfg";
			ConfigFile config;
			std::string error;

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[directshow]\n"
					"frame_offset: AUTO\n"
					"renderer_transfer_matrix: BT2020_10\n"
					"renderer_primaries: BT2020\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\n"
					"capture_input: HDMI\n"
					"video_conversion: V210_TO_P010\n"
					"container_colorspace: BT2020\n"
					"hdr_colorspace: FOLLOW_INPUT_LLDV\n"
					"hdr_luminance: FOLLOW_INPUT_LLDV\n"
					"[vprenderer]\nquality: high\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());

			// General is the shared default and DirectShow is now an explicit
			// override, so both fields are valid together.
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\nvideo_conversion: V210_TO_P010\n"
					"container_colorspace: BT2020\n"
					"[directshow]\nvideo_conversion: NONE\n"
					"container_colorspace: REC709\n"
					"[vprenderer]\nquality: high\n"
					"video_conversion: V210_TO_P010\n"
					"hdr_colorspace: FOLLOW_INPUT_LLDV\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model rendererModel;
			Assert::IsTrue(RendererProfileConfig::Read(config, rendererModel, error),
				std::wstring(error.begin(), error.end()).c_str());

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[directshow]\ncapture_device: not-valid-here\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("capture_device") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(QueueRecoveryUsesDefaultQueueAndRejectsLegacyDuplicates)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-queue-recovery-migration.cfg";
			ConfigFile config;
			std::string error;
			std::string value;
			std::string section;

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue.normal]\n"
					"queue_size: 32\n"
					"reset_after_render_restart_seconds: 7\n"
					"[queue.low_latency]\n"
					"queue_size: 1\n"
					"[queue_recovery]\n"
					"reset_queue_too_large_percent: 70\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			const auto queue = std::find_if(model.groups.begin(), model.groups.end(),
				[](const RendererProfileConfig::Group& group)
				{ return group.name == "queue"; });
			Assert::IsTrue(queue != model.groups.end());
			Assert::AreEqual("normal", queue->defaultSelection.c_str());
			Assert::IsTrue(QueueConfiguration::TryGetRecoveryString(config,
				"reset_after_render_restart_seconds", value, &section));
			Assert::AreEqual("7", value.c_str());
			Assert::AreEqual("queue.normal", section.c_str());
			Assert::IsTrue(QueueConfiguration::TryGetRecoveryString(config,
				"reset_queue_too_large_percent", value, &section));
			Assert::AreEqual("70", value.c_str());
			Assert::AreEqual("queue_recovery", section.c_str());

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue.named]\nqueue_size: 1\n"
					"[queue]\n"
					"reset_after_render_restart_seconds: 4\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(QueueConfiguration::TryGetRecoveryString(config,
				"reset_after_render_restart_seconds", value, &section));
			Assert::AreEqual("4", value.c_str());
			Assert::AreEqual("queue", section.c_str());

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\n"
					"reset_after_render_restart_seconds: 4\n"
					"[queue_recovery]\n"
					"reset_after_render_restart_seconds: 8\n";
			}
			Assert::IsTrue(config.Load(path));
			error.clear();
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("queue_recovery") != std::string::npos);
			Assert::IsTrue(error.find("reset_after_render_restart_seconds") !=
				std::string::npos);

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\n"
					"reset_after_render_restart_seconds: 4\n"
					"[queue.low_latency]\n"
					"lead_frames: 0\n"
					"startup_preroll_frames: 1\n"
					"target_frames: 1\n"
					"active_picture_lookahead_frames: 2\n"
					"reset_after_render_restart_seconds: 2\n"
					"reset_queue_too_large_percent: 200\n";
			}
			Assert::IsTrue(config.Load(path));
			error.clear();
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::ResolvedQueue resolved;
			Assert::IsTrue(RendererProfileConfig::ResolveQueue(
				model, "low_latency", resolved, error));
			Assert::AreEqual(size_t{ 0 }, resolved.leadFrames);
			Assert::AreEqual(size_t{ 1 }, resolved.startupPrerollFrames);
			Assert::AreEqual(size_t{ 1 }, resolved.targetFrames);
			Assert::AreEqual(size_t{ 2 }, resolved.activePictureLookaheadFrames);
			Assert::AreEqual(2, resolved.resetAfterRendererRestartSeconds);
			Assert::AreEqual(200, resolved.resetQueueTooLargePercent);

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\nreset_queue_too_large_percent: 201\n";
			}
			Assert::IsTrue(config.Load(path));
			error.clear();
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("1 to 200") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(LldvProfilesUseOrderedSelectionAndInheritBaselineMetadata)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-lldv-profiles.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[lldv.cinema]\n"
					"max_cll: 1000\n"
					"mastering_min_luminance: 0.001\n"
					"mastering_max_luminance: 4000\n"
					"[lldv.hdr]\n"
					"when: ${eotf}==\"HDR\"\n"
					"max_fall: 401\n"
					"[lldv.pq]\n"
					"when: ${transfer}==\"PQ\"\n"
					"shortcut: Shift+l\n"
					"max_fall: 500\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			const auto lldvGroup = std::find_if(model.groups.begin(),
				model.groups.end(), [](const RendererProfileConfig::Group& group)
				{ return group.name == "lldv"; });
			Assert::IsTrue(lldvGroup != model.groups.end());
			Assert::AreEqual("cinema", lldvGroup->defaultSelection.c_str());
			Assert::AreEqual(static_cast<size_t>(3), lldvGroup->profiles.size());
			Assert::AreEqual("hdr", lldvGroup->profiles[1].c_str());
			std::vector<std::string> chords;
			Assert::IsTrue(RendererProfileConfig::CollectKeyChords(model,
				chords, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(std::find(chords.begin(), chords.end(), "Shift+L") !=
				chords.end());

			UnifiedProfileRuntime::Runtime runtime;
			const auto hdrAndPq = [](const std::string& variable,
				std::string& value)
			{
				if (variable == "eotf") { value = "HDR"; return true; }
				if (variable == "transfer") { value = "PQ"; return true; }
				return false;
			};
			Assert::IsTrue(runtime.Initialize(config, hdrAndPq, error),
				std::wstring(error.begin(), error.end()).c_str());
			const auto initial = runtime.GetSnapshot();
			Assert::IsTrue(initial != nullptr);
			Assert::AreEqual("hdr", initial->lldv.profile.c_str());
			Assert::IsTrue(initial->lldv.hasMaxCll);
			Assert::IsTrue(std::abs(initial->lldv.maxCll - 1000.0) < 1e-12);
			Assert::IsTrue(initial->lldv.hasMaxFall);
			Assert::IsTrue(std::abs(initial->lldv.maxFall - 401.0) < 1e-12);
			Assert::IsTrue(std::abs(initial->lldv.masteringMaxLuminance -
				4000.0) < 1e-12);

			UnifiedProfileRuntime::SelectionResult selection;
			Assert::IsTrue(runtime.SelectKey("Shift+l", hdrAndPq,
				selection, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(selection.changed);
			Assert::AreEqual("pq", selection.snapshot->lldv.profile.c_str());
			Assert::IsTrue(std::abs(selection.snapshot->lldv.maxCll -
				1000.0) < 1e-12);
			Assert::IsTrue(std::abs(selection.snapshot->lldv.maxFall -
				500.0) < 1e-12);
			Assert::IsTrue(std::abs(selection.snapshot->lldv.
				masteringMinLuminance - 0.001) < 1e-12);
			Assert::AreEqual("pq",
				RendererProfileConfig::FormatSelection(
					selection.snapshot->effectiveSelections.at("lldv")).c_str());

			const RendererProfileConfig::LldvMetadata legacyDefaults =
				RendererProfileConfig::DefaultLldvMetadata(false);
			const RendererProfileConfig::LldvMetadata newDefaults =
				RendererProfileConfig::DefaultLldvMetadata(true);
			Assert::IsTrue(std::abs(legacyDefaults.maxFall - 1000.0) < 1e-12);
			Assert::IsTrue(std::abs(legacyDefaults.masteringMinLuminance -
				0.0001) < 1e-12);
			Assert::IsTrue(std::abs(newDefaults.maxFall - 401.0) < 1e-12);
			Assert::IsTrue(std::abs(newDefaults.masteringMaxLuminance -
				4000.0) < 1e-12);

			DeleteFileA(path.c_str());
		}

		TEST_METHOD(LegacyLldvRootRemainsTheBaselineProfile)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-lldv-root.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[lldv]\nmax_cll: 900\n"
					"mastering_max_luminance: 3000\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error));
			UnifiedProfileRuntime::Runtime runtime;
			Assert::IsTrue(runtime.Initialize(config,
				[](const std::string&, std::string&) { return false; }, error),
				std::wstring(error.begin(), error.end()).c_str());
			const auto snapshot = runtime.GetSnapshot();
			Assert::AreEqual("base", snapshot->lldv.profile.c_str());
			Assert::IsTrue(std::abs(snapshot->lldv.maxCll - 900.0) < 1e-12);
			Assert::IsTrue(std::abs(snapshot->lldv.masteringMaxLuminance -
				3000.0) < 1e-12);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(MainConfigSchemaRejectsForeignAndIllTypedKeys)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-main-schema-invalid.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[command_line]\nalpha_queue_size: 1\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("alpha_queue_size") != std::string::npos);

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[command_line]\nqueue_size: 0\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("queue_size") != std::string::npos);

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\nqueue_size: 0\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("queue_size") != std::string::npos);

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\ntarget_frames: 17\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("target_frames") != std::string::npos);

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\nactive_picture_lookahead_frames: 9\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("active_picture_lookahead_frames") !=
				std::string::npos);

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[queue]\nlead_frames: 17\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(
				error.find("lead_frames") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(MainConfigSchemaValidatesForegroundOnlyShortcutPolicy)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-shortcut-focus-schema.cfg";
			ConfigFile config;
			std::string error;

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[shortcuts]\nforeground_only: true\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsTrue(MainConfigSchema::Validate(config, error));

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[shortcuts]\nforeground_only: sometimes\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(error.find("foreground_only") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(MainConfigSchemaOwnsOnlyTheKnownLoggingKey)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-logging-schema.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				// Value validation intentionally belongs to the non-fatal
				// startup resolver, not the strict main schema.
				file << "[logging]\ndebug_log_retention: invalid\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::OwnsSection("logging"));
			Assert::IsTrue(MainConfigSchema::Validate(config, error));

			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[logging]\nunknown_logging_key: 10\n";
			}
			Assert::IsTrue(config.Load(path));
			Assert::IsFalse(MainConfigSchema::Validate(config, error));
			Assert::IsTrue(
				error.find("unknown_logging_key") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(RendererConfigViewReadsCanonicalNamespace)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vpvr-canonical.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\n"
					"persist_profile_selection: true\n"
					"event_action_delay_seconds: 5\n"
					"[vpvr.display]\n"
					"quality: high\n"
					"sdr_target_nits: 100\n"
					"[vpvr.general]\n"
					"profile_update_mode: never\n"
					"live_profile_updates: true\n"
					"switch_refresh_rate: false\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			RendererConfigView view(config);
			std::string error;
			std::vector<std::string> warnings;
			Assert::IsTrue(view.Validate(error, warnings));
			Assert::IsTrue(error.empty());
			Assert::IsTrue(warnings.empty());
			std::string quality;
			std::string profileUpdateMode;
			bool liveProfileUpdates = false;
			bool switchRefreshRate = true;
			Assert::IsTrue(view.TryGetDisplayString("quality", quality));
			Assert::AreEqual("high", quality.c_str());
			Assert::IsTrue(view.TryGetPolicyString(
				"profile_update_mode", profileUpdateMode));
			Assert::AreEqual("never", profileUpdateMode.c_str());
			Assert::IsTrue(view.TryGetPolicyBool(
				"live_profile_updates", liveProfileUpdates));
			Assert::IsTrue(liveProfileUpdates);
			Assert::IsTrue(view.TryGetPolicyBool(
				"switch_refresh_rate", switchRefreshRate));
			Assert::IsFalse(switchRefreshRate);
			Assert::IsTrue(RendererProfileConfig::OwnsSection(
				"vpvr.display"));
			Assert::IsTrue(RendererProfileConfig::OwnsSection(
				"vpvr.general"));
			Assert::IsFalse(RendererProfileConfig::OwnsSection(
				"vpvr.unknown"));
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(RendererConfigViewRejectsCanonicalDisplayCoexistence)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vpvr-display-conflict.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[vpvr.display]\nquality: high\n"
					"[display]\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			std::vector<std::string> warnings;
			Assert::IsFalse(
				RendererConfigView(config).Validate(error, warnings));
			Assert::IsTrue(error.find("vpvr.display") != std::string::npos);
			Assert::IsTrue(error.find("[display]") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(RendererConfigViewRejectsPolicyKeyDuplicate)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vpvr-policy-conflict.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[general]\n"
					"persist_profile_selection: true\n"
					"output_diagnostics: true\n"
					"[vpvr.general]\n"
					"output_diagnostics: true\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			std::vector<std::string> warnings;
			Assert::IsFalse(
				RendererConfigView(config).Validate(error, warnings));
			Assert::IsTrue(
				error.find("output_diagnostics") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(RendererConfigViewPreservesLegacyPrecedence)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vpvr-legacy-precedence.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[libplacebo]\n"
					"quality: fast\n"
					"sdr_target_nits: 80\n"
					"switch_refresh_rate: false\n"
					"[display]\n"
					"quality: high\n"
					"[general]\n"
					"switch_refresh_rate: true\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			RendererConfigView view(config);
			std::string error;
			std::vector<std::string> warnings;
			Assert::IsTrue(view.Validate(error, warnings));
			Assert::AreEqual(static_cast<size_t>(2), warnings.size());
			std::string value;
			bool policy = false;
			Assert::IsTrue(view.TryGetDisplayString("quality", value));
			Assert::AreEqual("high", value.c_str());
			Assert::IsTrue(view.TryGetDisplayString(
				"sdr_target_nits", value));
			Assert::AreEqual("80", value.c_str());
			Assert::IsTrue(view.TryGetPolicyBool(
				"switch_refresh_rate", policy));
			Assert::IsTrue(policy);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(RendererConfigViewWarnsOnlyForConsumedLegacySections)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vpvr-unused-legacy.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[display]\n"
					"quality: high\n"
					"[libplacebo]\n"
					"quality: fast\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			std::string error;
			std::vector<std::string> warnings;
			Assert::IsTrue(
				RendererConfigView(config).Validate(error, warnings));
			Assert::AreEqual(static_cast<size_t>(1), warnings.size());
			Assert::IsTrue(
				warnings.front().find("[display]") != std::string::npos);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(RendererConfigRejectsWrongCanonicalOwnership)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vpvr-wrong-owner.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[vpvr.display]\n"
					"switch_refresh_rate: true\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsFalse(
				RendererProfileConfig::Read(config, model, error));
			Assert::IsTrue(
				error.find("switch_refresh_rate") != std::string::npos);
			DeleteFileA(path.c_str());
		}

        TEST_METHOD(HdrToneMapTargetGammaAndTargetPrimariesValidateCanonicalAndOldKeys)
        {
            for (const std::string value : { "bt1886", "srgb", "1.8", "2.0", "2.2", "2.4", "2.6", "2.8" })
            {
                Assert::IsFalse(RendererProfileConfig::ValidateTargetRendererSetting("hdr_tone_map_target_gamma", value));
                Assert::IsTrue(RendererProfileConfig::ValidateColorConfigSetting("hdr_tone_map_target_gamma", value));
            }
            for (const std::string value : { "auto", "display", "pq", "2.5", "" })
                Assert::IsFalse(RendererProfileConfig::ValidateColorConfigSetting("hdr_tone_map_target_gamma", value));
            for (const std::string value : { "rec709", "p3_d65", "bt2020" })
                for (const std::string key : { "target_primaries", "sdr_target_primaries" })
                {
                    Assert::IsTrue(RendererProfileConfig::ValidateTargetRendererSetting(key, value));
                    Assert::IsTrue(RendererProfileConfig::ValidateColorConfigSetting(key, value));
                }
            Assert::IsFalse(RendererProfileConfig::ValidateColorConfigSetting("target_primaries", "auto"));
        }

        TEST_METHOD(ColorCalibrationAliasesResolveBeforeInheritance)
        {
            char directory[MAX_PATH] = {};
            Assert::IsTrue(GetTempPathA(ARRAYSIZE(directory), directory) > 0);
            const std::string path = std::string(directory) + "VP0174-color-calibration-inheritance.cfg";
            for (const char* root : { "vprenderer.color", "vprenderer.color.Default" })
            {
                {
                    std::ofstream file(path);
                    file << "[vprenderer.Default]\nquality: high\n"
                        "[vprenderer.Fast]\nquality: fast\nwhen: ${width} > 2000\n"
                        "[" << root << "]\nhdr_tone_map_target_gamma: 2.6\ntarget_primaries: rec709\n"
                        "calibration_lut_enabled: true\ncalibration_lut_bt709: root.cube\n"
                        "[vprenderer.color.OldChild]\ncalibration_lut_input_transfer: 2.4\nsdr_target_primaries: bt2020\n"
                        "[vprenderer.color.DisplayChild]\ncalibration_lut_input_transfer: display\n"
                        "[vprenderer.color.BothChild]\nhdr_tone_map_target_gamma: 2.0\ncalibration_lut_input_transfer: 2.8\n"
                        "target_primaries: p3_d65\nsdr_target_primaries: bt2020\n"
                        "[vprenderer.color.Inherited]\noutput_gamma: 2.2\n"
                        "[vprenderer.color.Clear]\ncalibration_lut_enabled: false\ncalibration_lut_bt709: none\n";
                }
                ConfigFile config;
                Assert::IsTrue(config.Load(path));
                RendererProfileConfig::Model model;
                std::string error;
                Assert::IsTrue(RendererProfileConfig::Read(config, model, error),
                    std::wstring(error.begin(), error.end()).c_str());
                Assert::AreEqual(std::string("2.4"), model.profiles.at("color.oldchild").settings.at("hdr_tone_map_target_gamma"));
                Assert::AreEqual(std::string("2.2"), model.profiles.at("color.displaychild").settings.at("hdr_tone_map_target_gamma"));
                Assert::AreEqual(std::string("2.0"), model.profiles.at("color.bothchild").settings.at("hdr_tone_map_target_gamma"));
                Assert::AreEqual(std::string("2.6"), model.profiles.at("color.inherited").settings.at("hdr_tone_map_target_gamma"));
                Assert::AreEqual(std::string("root.cube"), model.profiles.at("color.inherited").settings.at("calibration_lut_bt709"));
                Assert::AreEqual(std::string("true"), model.profiles.at("color.inherited").settings.at("calibration_lut_enabled"));
                Assert::AreEqual(std::string("none"), model.profiles.at("color.clear").settings.at("calibration_lut_bt709"));
                Assert::AreEqual(std::string("false"), model.profiles.at("color.clear").settings.at("calibration_lut_enabled"));
                Assert::AreEqual(std::string("bt2020"), model.profiles.at("color.oldchild").settings.at("target_primaries"));
                Assert::AreEqual(std::string("p3_d65"), model.profiles.at("color.bothchild").settings.at("target_primaries"));
                for (const char* rendering : { "display.default", "display.fast" })
                    for (const auto& value : model.profiles.at(rendering).settings)
                        Assert::IsFalse(CalibrationProfileMigration::IsKey(value.first));
                std::string colorSelection;
                for (const char* width : { "1920", "3840" })
                {
                    std::vector<RendererProfileConfig::AutomaticSelection> selected;
                    Assert::IsTrue(RendererProfileConfig::SelectAutomatic(model,
                        [width](const std::string& key, std::string& value)
                        { if (key != "width") return false; value = width; return true; }, selected, error));
                    for (const auto& item : selected)
                        if (item.group == "color")
                        {
                            Assert::IsFalse(item.profiles.empty());
                            if (colorSelection.empty()) colorSelection = item.profiles.front();
                            Assert::AreEqual(colorSelection, item.profiles.front());
                        }
                }
                Assert::IsFalse(colorSelection.empty());
            }
            DeleteFileA(path.c_str());
        }

        TEST_METHOD(CalibrationArchivesPassRuntimeValidationForTargetAndOlderFormats)
        {
            char directory[MAX_PATH] = {};
            Assert::IsTrue(GetTempPathA(ARRAYSIZE(directory), directory) > 0);
            const std::string path = std::string(directory) + "VP0174-calibration-archive-ownership.cfg";
            for (const char* section : { "vprenderer.color.default", "display" })
                for (const char* archive : { "calibration_archive", "calibration_archive.default", "calibration_archive.default_2" })
                {
                    {
                        std::ofstream file(path);
                        file << "[" << section << "]\noutput_gamma: 2.2\n[" << archive << "]\n"
                            "calibration_lut_input_transfer: display\nunknown_original_setting: preserved\n"
                            "when: original selector retained verbatim\n";
                    }
                    ConfigFile config;
                    Assert::IsTrue(config.Load(path));
                    RendererProfileConfig::Model model;
                    std::string error;
                    Assert::IsTrue(RendererProfileConfig::ValidateOwnedSections(config, error),
                        std::wstring(error.begin(), error.end()).c_str());
                    const bool read = RendererProfileConfig::Read(config, model, error);
                    Assert::IsTrue(read, std::wstring(error.begin(), error.end()).c_str());
                    Assert::AreEqual(std::string("preserved"), config.GetSectionValues(archive)->at("unknown_original_setting"));
                }
            DeleteFileA(path.c_str());
        }

        TEST_METHOD(CalibrationMigrationPreservesDistinctSetsSelectorsAndArchiveCollisions)
        {
            using namespace CalibrationProfileMigration;
            Sections sections = {
                {"vprenderer.default", {{"quality","high"},{"calibration_lut_enabled","true"},{"calibration_lut_bt709","a.cube"},{"hdr_tone_map_target_gamma","2.2"},{"shortcut","F8"}}},
                {"vprenderer.cinema", {{"quality","balanced"},{"calibration_lut_bt709","b.cube"},{"hdr_tone_map_target_gamma","2.4"},{"when","${width} > 2000"}}},
                {"vprenderer.duplicate", {{"calibration_lut_bt709","b.cube"},{"calibration_lut_input_transfer","2.4"}}},
                {"vprenderer.color.rec709", {{"target_primaries","rec709"},{"output_gamma","2.2"},{"shortcut","F5"}}},
                {"vprenderer.color.bt2020", {{"target_primaries","bt2020"},{"output_gamma","2.4"},{"when","${width} > 2000"}}},
                {"calibration_archive.cinema", {{"prior","retained"}}} };
            std::vector<std::string> order = {"vprenderer.default","vprenderer.cinema","vprenderer.duplicate",
                "vprenderer.color.rec709","vprenderer.color.bt2020","calibration_archive.cinema"};
            Assert::IsTrue(Apply(sections, order));
            for (const char* color : {"vprenderer.color.rec709","vprenderer.color.bt2020"})
            {
                Assert::AreEqual(std::string("a.cube"), sections.at(color).at("calibration_lut_bt709"));
                Assert::AreEqual(std::string("none"), sections.at(color).at("calibration_lut_p3_d65"));
                const auto& extra = sections.at(std::string(color)+"_lut_cinema");
                Assert::AreEqual(std::string("b.cube"), extra.at("calibration_lut_bt709"));
                Assert::AreEqual(std::string("2.4"), extra.at("hdr_tone_map_target_gamma"));
                Assert::AreEqual(sections.at(color).at("target_primaries"), extra.at("target_primaries"));
                Assert::AreEqual(sections.at(color).at("output_gamma"), extra.at("output_gamma"));
                for (const auto& setting : extra) Assert::IsFalse(ColorOutputProfileMigration::IsSelector(setting.first));
                Assert::IsTrue(sections.count(std::string(color)+"_lut_duplicate") == 0);
            }
            Assert::AreEqual(std::string("F5"), sections.at("vprenderer.color.rec709").at("shortcut"));
            Assert::AreEqual(std::string("balanced"), sections.at("vprenderer.cinema").at("quality"));
            Assert::AreEqual(std::string("${width} > 2000"), sections.at("vprenderer.cinema").at("when"));
            Assert::AreEqual(std::string("b.cube"), sections.at("calibration_archive.cinema_2").at("calibration_lut_bt709"));
            Assert::AreEqual(std::string("retained"), sections.at("calibration_archive.cinema").at("prior"));
            for (const char* name : {"vprenderer.default","vprenderer.cinema","vprenderer.duplicate"})
                for (const auto& value : sections.at(name)) Assert::IsFalse(IsKey(value.first));
            const auto savedSections = sections;
            const auto savedOrder = order;
            Assert::IsFalse(Apply(sections, order));
            Assert::IsTrue(savedSections == sections && savedOrder == order);
        }

        TEST_METHOD(CalibrationMigrationUsesLiteralRootAndPreservesMixedColorOverrides)
        {
            using namespace CalibrationProfileMigration;
            Sections sections = {
                {"vprenderer.first", {{"calibration_lut_enabled","true"},{"calibration_lut_bt709","first.cube"}}},
                {"vprenderer", {{"calibration_lut_enabled","false"},{"calibration_lut_input_transfer","display"}}},
                {"vprenderer.color.default", {{"calibration_lut_enabled","true"},{"calibration_lut_bt709","color.cube"},{"hdr_tone_map_target_gamma","2.6"}}} };
            std::vector<std::string> order = {"vprenderer.first","vprenderer","vprenderer.color.default"};
            Assert::IsTrue(Apply(sections, order));
            Assert::AreEqual(std::string("color.cube"), sections.at("vprenderer.color.default").at("calibration_lut_bt709"));
            Assert::AreEqual(std::string("2.6"), sections.at("vprenderer.color.default").at("hdr_tone_map_target_gamma"));
            const auto& root = sections.at("vprenderer.color.default_lut_default");
            Assert::AreEqual(std::string("false"), root.at("calibration_lut_enabled"));
            Assert::AreEqual(std::string("none"), root.at("calibration_lut_bt709"));
            Assert::AreEqual(std::string("2.2"), root.at("hdr_tone_map_target_gamma"));
            Assert::AreEqual(std::string("first.cube"), sections.at("vprenderer.color.default_lut_first").at("calibration_lut_bt709"));
            Sections noColor = {{"vprenderer.default",{{"calibration_lut_enabled","false"}}}};
            std::vector<std::string> noColorOrder = {"vprenderer.default"};
            Assert::IsTrue(Apply(noColor, noColorOrder));
            Assert::IsTrue(noColor.at("vprenderer.color.default") == Defaults());
            Sections oldFormat = {{"display",{{"calibration_lut_bt709","old.cube"}}}};
            std::vector<std::string> oldOrder = {"display"};
            Assert::IsFalse(Apply(oldFormat, oldOrder));
            Assert::AreEqual(std::string("old.cube"), oldFormat.at("display").at("calibration_lut_bt709"));
        }

        TEST_METHOD(OldLutInputKeysRemainLoadableInTheirOriginalSections)
        {
            for (const std::string value : { "display", "bt1886", "srgb", "1.8", "2.0", "2.2", "2.4", "2.6", "2.8" })
            {
                Assert::IsFalse(RendererProfileConfig::ValidateTargetRendererSetting("calibration_lut_input_transfer", value));
                Assert::IsTrue(RendererProfileConfig::ValidateColorConfigSetting("calibration_lut_input_transfer", value));
                Assert::IsTrue(RendererProfileConfig::ValidateColorConfigSetting("calibration_lut_input_gamma", value));
            }
            Assert::IsFalse(RendererProfileConfig::ValidateTargetRendererSetting("calibration_lut_input_transfer", "auto"));
        }

		TEST_METHOD(CanonicalRendererAcceptsDocumentedPeakAndCalibrationLutValues)
		{
			char temporaryDirectory[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(
				ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
			const std::string path = std::string(temporaryDirectory) +
				"VideoProcessor-vp0049-documented-renderer-values.cfg";
			{
				std::ofstream file(path, std::ios::out | std::ios::trunc);
				file << "[vpvr.display]\n"
					"peak_detection: high_quality\n"
					"calibration_lut_enabled: true\n"
					"calibration_lut_p3_d65: luts/projector-p3.cube\n";
			}

			ConfigFile config;
			Assert::IsTrue(config.Load(path));
			RendererProfileConfig::Model model;
			std::string error;
			Assert::IsTrue(
				RendererProfileConfig::Read(config, model, error),
				std::wstring(error.begin(), error.end()).c_str());
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(ConfigurationReferenceMatchesPublicFieldInventory)
		{
			auto readFile = [](const std::string& path)
			{
				std::ifstream file(path, std::ios::in | std::ios::binary);
				std::ostringstream contents;
				contents << file.rdbuf();
				std::string text = contents.str();
				text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
				return text;
			};
			auto sourceRoot = []()
			{
				std::string path = __FILE__;
				std::replace(path.begin(), path.end(), '/', '\\');
				const std::string marker =
					"\\src\\VideoProcessor-Test\\ConfigFileTests.cpp";
				const size_t markerPosition = path.rfind(marker);
				if (markerPosition != std::string::npos)
					return path.substr(0, markerPosition);

				char currentDirectory[MAX_PATH] = {};
				GetCurrentDirectoryA(ARRAYSIZE(currentDirectory), currentDirectory);
				path = currentDirectory;
				for (int parent = 0; parent < 8; ++parent)
				{
					if (GetFileAttributesA(
						(path + "\\CONFIGURATION.html").c_str()) !=
						INVALID_FILE_ATTRIBUTES)
						return path;
					const size_t separator = path.find_last_of("\\/");
					if (separator == std::string::npos)
						break;
					path.resize(separator);
				}
				return std::string();
			}();
			Assert::IsFalse(sourceRoot.empty(),
				L"Could not locate the VideoProcessor source root");

			const std::string inventoryText = readFile(
				sourceRoot + "\\docs\\configuration-public-fields.tsv");
			const std::string valueInventoryText = readFile(
				sourceRoot + "\\docs\\configuration-public-values.tsv");
			const std::string html = readFile(
				sourceRoot + "\\CONFIGURATION.html");
			Assert::IsFalse(inventoryText.empty());
			Assert::IsFalse(valueInventoryText.empty());
			Assert::IsFalse(html.empty());

			struct InventoryField
			{
				std::string token;
				std::string anchor;
				bool requiresAutoExplanation = false;
			};
			std::vector<InventoryField> inventory;
			std::set<std::string> inventoryTokens;
			std::set<std::string> inventoryAnchors;
			std::map<std::string, std::string> inventoryAnchorsByToken;
			std::istringstream inventoryStream(inventoryText);
			std::string line;
			while (std::getline(inventoryStream, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty() || line.front() == '#')
					continue;
				std::vector<std::string> columns;
				std::istringstream lineStream(line);
				std::string column;
				while (std::getline(lineStream, column, '\t'))
					columns.push_back(column);
				Assert::AreEqual(static_cast<size_t>(4), columns.size(),
					L"Every public-field inventory line needs four columns");
				Assert::IsTrue(inventoryTokens.insert(columns[0]).second,
					std::wstring(columns[0].begin(), columns[0].end()).c_str());
				inventoryAnchors.insert(columns[1]);
				inventoryAnchorsByToken.emplace(columns[0], columns[1]);
				inventory.push_back(
					{ columns[0], columns[1], columns[3] == "yes" });
			}
			Assert::IsTrue(inventory.size() > 50,
				L"The public inventory unexpectedly lost the core VP-0079 fields");

			struct InventoryValue
			{
				std::string field;
				std::string value;
			};
			std::vector<InventoryValue> valueInventory;
			std::set<std::string> valueTokens;
			std::istringstream valueInventoryStream(valueInventoryText);
			while (std::getline(valueInventoryStream, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty() || line.front() == '#')
					continue;
				const size_t separator = line.find('\t');
				Assert::IsTrue(separator != std::string::npos &&
					line.find('\t', separator + 1) == std::string::npos,
					L"Every public-value inventory line needs two columns");
				const std::string field = line.substr(0, separator);
				std::string value = line.substr(separator + 1);
				if (!value.empty() && value.back() == '\r')
					value.pop_back();
				Assert::IsTrue(inventoryTokens.find(field) != inventoryTokens.end(),
					std::wstring(field.begin(), field.end()).c_str());
				const std::string token = field + "\t" + value;
				Assert::IsTrue(valueTokens.insert(token).second,
					std::wstring(token.begin(), token.end()).c_str());
				valueInventory.push_back({ field, value });
			}
			Assert::IsTrue(valueInventory.size() > 50,
				L"The public-value inventory unexpectedly lost enum coverage");

			std::set<std::string> documentedTokens;
			size_t attribute = 0;
			while ((attribute = html.find("data-fields=\"", attribute)) !=
				std::string::npos)
			{
				attribute += strlen("data-fields=\"");
				const size_t end = html.find('"', attribute);
				Assert::IsTrue(end != std::string::npos);
				std::string values = html.substr(attribute, end - attribute);
				auto replaceAll = [&values](
					const std::string& from, const std::string& to)
				{
					size_t position = 0;
					while ((position = values.find(from, position)) !=
						std::string::npos)
					{
						values.replace(position, from.size(), to);
						position += to.size();
					}
				};
				replaceAll("&lt;", "<");
				replaceAll("&gt;", ">");
				replaceAll("&amp;", "&");
				std::istringstream valueStream(values);
				std::string value;
				while (valueStream >> value)
					Assert::IsTrue(documentedTokens.insert(value).second,
						std::wstring(value.begin(), value.end()).c_str());
				attribute = end + 1;
			}
			Assert::IsTrue(documentedTokens == inventoryTokens,
				L"CONFIGURATION.html data-fields differ from the public inventory");

			for (const std::string& anchor : inventoryAnchors)
			{
				const std::string marker = "id=\"" + anchor + "\"";
				const size_t first = html.find(marker);
				Assert::IsTrue(first != std::string::npos,
					std::wstring(anchor.begin(), anchor.end()).c_str());
				Assert::IsTrue(html.find(marker, first + marker.size()) ==
					std::string::npos,
					L"Configuration anchors must be unique");
			}
			for (const InventoryField& field : inventory)
			{
				if (!field.requiresAutoExplanation)
					continue;
				const size_t anchor = html.find(
					"id=\"" + field.anchor + "\"");
				const size_t articleEnd = html.find("</article>", anchor);
				Assert::IsTrue(anchor != std::string::npos &&
					articleEnd != std::string::npos &&
					html.substr(anchor, articleEnd - anchor).find(
						"AUTO behavior:") != std::string::npos,
					std::wstring(field.token.begin(), field.token.end()).c_str());
			}
			for (const InventoryValue& documentedValue : valueInventory)
			{
				const std::string& anchor =
					inventoryAnchorsByToken.at(documentedValue.field);
				const size_t article = html.find("id=\"" + anchor + "\"");
				const size_t articleEnd = html.find("</article>", article);
				const std::string expected = "<code>" + documentedValue.value +
					"</code>";
				const std::string diagnostic = documentedValue.field + "=" +
					documentedValue.value;
				Assert::IsTrue(article != std::string::npos &&
					articleEnd != std::string::npos &&
					html.substr(article, articleEnd - article).find(expected) !=
						std::string::npos,
					std::wstring(diagnostic.begin(), diagnostic.end()).c_str());
			}

			size_t link = 0;
			while ((link = html.find("href=\"#", link)) != std::string::npos)
			{
				link += strlen("href=\"#");
				const size_t end = html.find('"', link);
				Assert::IsTrue(end != std::string::npos);
				const std::string target = html.substr(link, end - link);
				Assert::IsTrue(
					html.find("id=\"" + target + "\"") != std::string::npos,
					std::wstring(target.begin(), target.end()).c_str());
				link = end + 1;
			}

			size_t example = 0;
			size_t exampleCount = 0;
			while ((example = html.find(
				"<pre data-ini-example=\"", example)) != std::string::npos)
			{
				const size_t contentStart = html.find('>', example);
				const size_t contentEnd = html.find("</pre>", contentStart);
				Assert::IsTrue(contentStart != std::string::npos &&
					contentEnd != std::string::npos);
				std::string configuration = html.substr(
					contentStart + 1, contentEnd - contentStart - 1);
				auto decode = [&configuration](
					const std::string& entity, const std::string& value)
				{
					size_t position = 0;
					while ((position = configuration.find(entity, position)) !=
						std::string::npos)
					{
						configuration.replace(position, entity.size(), value);
						position += value.size();
					}
				};
				decode("&lt;", "<");
				decode("&gt;", ">");
				decode("&amp;", "&");

				char temporaryDirectory[MAX_PATH] = {};
				Assert::IsTrue(GetTempPathA(
					ARRAYSIZE(temporaryDirectory), temporaryDirectory) > 0);
				const std::string path = std::string(temporaryDirectory) +
					"VideoProcessor-vp0049-html-example-" +
					std::to_string(exampleCount) + ".cfg";
				{
					std::ofstream file(path, std::ios::out | std::ios::trunc);
					file << configuration;
				}

				ConfigFile config;
				Assert::IsTrue(config.Load(path));
				std::string error;
				Assert::IsTrue(
					MainConfigSchema::Validate(config, error),
					std::wstring(error.begin(), error.end()).c_str());
				RendererProfileConfig::Model model;
				if (!RendererProfileConfig::Read(config, model, error))
				{
					Logger::WriteMessage((L"Configuration reference example " +
						std::to_wstring(exampleCount) + L" is invalid: " +
						std::wstring(error.begin(), error.end())).c_str());
					Assert::Fail(std::wstring(error.begin(), error.end()).c_str());
				}
				DeleteFileA(path.c_str());
				++exampleCount;
				example = contentEnd + strlen("</pre>");
			}
			Assert::IsTrue(exampleCount >= 1,
				L"No configuration example is marked for validation");

			for (const char* excluded :
				{ "[profile_groups", "[profiles.", "[event_actions]",
				  "[shaders]" })
				Assert::IsTrue(html.find(excluded) == std::string::npos,
					std::wstring(excluded, excluded + strlen(excluded)).c_str());
		}
	};
}
