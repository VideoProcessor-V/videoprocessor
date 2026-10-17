#include "../VideoProcessor-Lib/vprenderer/LibplaceboRenderParameters.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace LibplaceboRenderParameters;
void RunBetaGpuTests(); // BetaGpuTests.cpp
void RunBetaOutputFloat32Tests(); // BetaGpuTests.cpp
void RunBetaFboTiming(); // BetaGpuTests.cpp
void RunBetaPassTiming(); // BetaGpuTests.cpp
void RunBetaMatcherTiming(); // BetaGpuTests.cpp
void RunBetaScenarioTiming(); // BetaGpuTests.cpp
void RunBetaOptionQuality(); // BetaGpuTests.cpp
void RunBetaDejudderQuality(); // BetaGpuTests.cpp
void RunBetaDitherCompare(); // BetaGpuTests.cpp
void RunBetaTimingGate(); // BetaGpuTests.cpp
void RunBetaMotionCorpus(); // BetaGpuTests.cpp
void RunBetaRealFootage(); // BetaGpuTests.cpp
void RunBetaChainBiasRegression(); // BetaGpuTests.cpp
void RunBetaFeatureGpuTests(); // BetaGpuTests.cpp
void RunBetaDitherVectorsTests(); // BetaGpuTests.cpp
void RunBetaExtendedRangeGpuTests(); // BetaGpuTests.cpp
void RunBetaChainGpuTest(); // BetaGpuTests.cpp
static void check(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
// Report modes (VP_TEST_MATCHER_TIMING, _DEJUDDER_QUALITY, _DITHER_COMPARE, _TIMING_GATE, _MOTION_CORPUS,
// _REAL_FOOTAGE, _QUALITY, _SCENARIO) run one report and stop before the gate's whole-chain tests: they
// return this distinct code and print "MODE <name>: gate not run", so a gate run with a stray report
// switch can never pass (the test host fails it unless VP_TEST_REPORT_MODE=1).
const int kBetaReportModeExitCode = 3;
static int BetaReportMode(const char* name)
{
    std::cout << "MODE " << name << ": gate not run\n";
    std::cout.flush();
    return kBetaReportModeExitCode;
}
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
        {
            // The live matcher's timing alone (VP_TEST_MATCHER_TIMING=1): skips the suite.
            char* mt = nullptr;
            size_t mtLength = 0;
            const bool matcherOnly = _dupenv_s(&mt, &mtLength, "VP_TEST_MATCHER_TIMING") == 0 && mt && std::string(mt) == "1";
            std::free(mt);
            if (matcherOnly)
            {
                RunBetaMatcherTiming();
                return BetaReportMode("VP_TEST_MATCHER_TIMING");
            }
        }
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
                return BetaReportMode("VP_TEST_DEJUDDER_QUALITY");
            }
            char* dc = nullptr;
            size_t dcLength = 0;
            const bool runDitherCompare = _dupenv_s(&dc, &dcLength, "VP_TEST_DITHER_COMPARE") == 0 && dc &&
                std::string(dc) == "1";
            std::free(dc);
            if (runDitherCompare)
            {
                RunBetaDitherCompare();
                return BetaReportMode("VP_TEST_DITHER_COMPARE");
            }
            char* tg = nullptr;
            size_t tgLength = 0;
            const bool runTimingGate = _dupenv_s(&tg, &tgLength, "VP_TEST_TIMING_GATE") == 0 && tg &&
                std::string(tg) == "1";
            std::free(tg);
            if (runTimingGate)
            {
                RunBetaTimingGate();
                return BetaReportMode("VP_TEST_TIMING_GATE");
            }
            char* mc = nullptr;
            size_t mcLength = 0;
            const bool runCorpus = _dupenv_s(&mc, &mcLength, "VP_TEST_MOTION_CORPUS") == 0 && mc && std::string(mc) == "1";
            std::free(mc);
            if (runCorpus)
            {
                RunBetaMotionCorpus();
                return BetaReportMode("VP_TEST_MOTION_CORPUS");
            }
            char* rf = nullptr;
            size_t rfLength = 0;
            const bool runReal = _dupenv_s(&rf, &rfLength, "VP_TEST_REAL_FOOTAGE") == 0 && rf && *rf;
            std::free(rf);
            if (runReal)
            {
                RunBetaRealFootage();
                return BetaReportMode("VP_TEST_REAL_FOOTAGE");
            }
            if (runQuality)
            {
                RunBetaOptionQuality();
                return BetaReportMode("VP_TEST_QUALITY");
            }
            if (runScenario)
            {
                // Report only, then stop (the rest of the suite is the gate's).
                RunBetaScenarioTiming();
                return BetaReportMode("VP_TEST_SCENARIO");
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
