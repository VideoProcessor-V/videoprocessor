#include "../VideoProcessor-Lib/vprenderer/LibplaceboRenderParameters.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace LibplaceboRenderParameters;
void RunBetaGpuTests(); // BetaGpuTests.cpp
void RunBetaOutputFloat32Tests(); // BetaGpuTests.cpp
void RunBetaFboTiming(); // BetaGpuTests.cpp
void RunBetaPassTiming(); // BetaGpuTests.cpp
void RunBetaScenarioTiming(); // BetaGpuTests.cpp
void RunBetaOptionQuality(); // BetaGpuTests.cpp
void RunBetaDejudderQuality(); // BetaGpuTests.cpp
void RunBetaDitherCompare(); // BetaGpuTests.cpp
void RunBetaTimingGate(); // BetaGpuTests.cpp
void RunBetaChainBiasRegression(); // BetaGpuTests.cpp
void RunBetaFeatureGpuTests(); // BetaGpuTests.cpp
void RunBetaDitherVectorsTests(); // BetaGpuTests.cpp
void RunBetaExtendedRangeGpuTests(); // BetaGpuTests.cpp
void RunBetaChainGpuTest(); // BetaGpuTests.cpp
static void check(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
// The suite's entry: the program's main, or (VP_BETA_TEST_DLL) a function the
// native test host calls - Smart App Control on the development laptop blocks
// new builds of this program but not test DLLs loaded by Microsoft's signed
// test host (owner 2026-10-13).
#ifdef VP_BETA_TEST_DLL
int RunBetaTestMain() {
#else
int main() {
#endif
    try {
        Settings settings; Projection p; std::string error;
        for (const char* quality : {"fast", "balanced", "high"}) {
            settings.quality = quality;
            check(Build(settings, false, p, error), error.c_str());
            check(!p.colorMapParams.vp_gamut_compression, "Original master must remain default");
            check(p.colorMapParams.dynamic_target, "Default dynamic target must be on");
            check(std::fabs(p.colorMapParams.film_strength - .3f) < 1e-6f, "Film default");
            check(p.colorMapParams.mastering_clip == 1.f, "Mastering clip default");
        }
        settings.dynamicTarget = false;
        settings.gamutMapping = "relative";
        check(Build(settings, false, p, error), error.c_str());
        const auto* originalGamut = p.colorMapParams.gamut_mapping;
        check(originalGamut && p.gamutMappingExport == "pl_gamut_map_relative", "Selected upstream gamut must reach Beta API");
        settings.customToneMapping = true;
        settings.toneTuning[ToneMappingTuning::FilmStrength] = .75;
        settings.toneTuning[ToneMappingTuning::MasteringClip] = .25;
        settings.toneTuning[ToneMappingTuning::BlackCutoff] = 3.;
        check(Build(settings, false, p, error), error.c_str());
        check(p.colorMapParams.vp_gamut_compression, "Gamut overflow correction must reach compatible Beta API");
        check(p.colorMapParams.gamut_mapping == originalGamut, "Residual correction must preserve selected upstream gamut");
        check(!p.colorMapParams.dynamic_target, "Toggle must reach master API");
        check(p.colorMapParams.film_strength == .75f && p.colorMapParams.mastering_clip == .25f, "Beta overrides");
        check(p.peakDetectParams.black_cutoff == 3.f, "Peak override must remain independent");
        settings.peakDetection = PeakDetection::Off;
        check(Build(settings, false, p, error), error.c_str());
        check(!p.renderParams.peak_detect_params && p.colorMapParams.film_strength == .75f, "Film must survive peak-off");
        pl_color_space source = pl_color_space_hdr10;
        ApplySourceLuminance(true, source);
        check(source.hdr.max_luma == PL_COLOR_SDR_WHITE, "SDR reference invariant");
        pl_color_space target = pl_color_space_srgb;
        ApplyTargetLuminance(false, 150.f, .01f, target);
        check(target.hdr.max_luma == 150.f, "Physical target nits invariant");
        check(MakeSwapchainColorHint(target).transfer == target.transfer, "SDR transfer hint invariant");
        std::cout << "PASS Beta defaults, residual correction/gamut independence, overrides, peak independence and luminance projection\n";
        RunBetaGpuTests();
        std::cout << "PASS Beta GPU: output clamp keeps footroom/headroom; encoding correct after PREOUTPUT hooks without the repair\n";
        RunBetaOutputFloat32Tests();
        {
            char* value = nullptr;
            size_t length = 0;
            const bool timing = _dupenv_s(&value, &length, "VP_TEST_TIMING") == 0 && value && std::string(value) == "1";
            std::free(value);
            if (timing)
            {
                RunBetaFboTiming();
                RunBetaPassTiming();
            }
            char* scenario = nullptr;
            size_t scenarioLength = 0;
            const bool runScenario = _dupenv_s(&scenario, &scenarioLength, "VP_TEST_SCENARIO") == 0 && scenario &&
                std::string(scenario) == "1";
            std::free(scenario);
            char* quality = nullptr;
            size_t qualityLength = 0;
            const bool runQuality = _dupenv_s(&quality, &qualityLength, "VP_TEST_QUALITY") == 0 && quality &&
                std::string(quality) == "1";
            std::free(quality);
            char* djq = nullptr;
            size_t djqLength = 0;
            const bool runDejudderQuality = _dupenv_s(&djq, &djqLength, "VP_TEST_DEJUDDER_QUALITY") == 0 && djq &&
                std::string(djq) == "1";
            std::free(djq);
            if (runDejudderQuality)
            {
                RunBetaDejudderQuality();
                return 0;
            }
            char* dc = nullptr;
            size_t dcLength = 0;
            const bool runDitherCompare = _dupenv_s(&dc, &dcLength, "VP_TEST_DITHER_COMPARE") == 0 && dc &&
                std::string(dc) == "1";
            std::free(dc);
            if (runDitherCompare)
            {
                RunBetaDitherCompare();
                return 0;
            }
            char* tg = nullptr;
            size_t tgLength = 0;
            const bool runTimingGate = _dupenv_s(&tg, &tgLength, "VP_TEST_TIMING_GATE") == 0 && tg &&
                std::string(tg) == "1";
            std::free(tg);
            if (runTimingGate)
            {
                RunBetaTimingGate();
                return 0;
            }
            if (runQuality)
            {
                RunBetaOptionQuality();
                return 0;
            }
            if (runScenario)
            {
                // Report only, then stop (the rest of the suite is the gate's).
                RunBetaScenarioTiming();
                return 0;
            }
        }
        RunBetaChainBiasRegression();
        std::cout << "PASS Beta S2: output pass in rgba32f, highlights within 1/16 step, extended codes kept, the chain bias A/B\n";
        RunBetaFeatureGpuTests();
        RunBetaDitherVectorsTests();
        std::cout << "PASS Beta features: sigma-delta (8-in-10, true 10-bit), LED compensation, occlusion mask, de-judder\n";
        RunBetaExtendedRangeGpuTests();
        std::cout << "PASS Beta extended range: BTB/WTW kept through scaling, in-range edges and colours as without the switch\n";
        RunBetaChainGpuTest();
        std::cout << "PASS Beta whole chain: every hook at once (still, cut, motion, de-judder extra, LED gain sequence, upscale, matrix), BTB/WTW and in-range kept, nothing disabled\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
}
