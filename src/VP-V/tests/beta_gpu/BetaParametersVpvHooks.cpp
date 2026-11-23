// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <cstdlib>
#include <iostream>
#include <string>

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
bool RunBetaCameraAccuracyIfRequested(); // BetaGpuTests.cpp (R1-CAMACC, VP_TEST_CAMERA_ACCURACY=1)
void RunBetaRealFootage(); // BetaGpuTests.cpp
void RunBetaChainBiasRegression(); // BetaGpuTests.cpp
void RunBetaSigmaBias(); // BetaGpuTests.cpp
void RunBetaFeatureGpuTests(); // BetaGpuTests.cpp
void RunBetaDitherVectorsTests(); // BetaGpuTests.cpp
void RunBetaDitherNeedWarpForkTests(); // BetaGpuTests.cpp (GEM-S10G: fork-only DitherNeedWarp cases, BETA-RAN evidence)
void RunBetaExtendedRangeGpuTests(); // BetaGpuTests.cpp
void RunBetaChainGpuTest(); // BetaGpuTests.cpp

namespace Vpv::BetaTests
{
namespace
{
constexpr int kBetaReportModeExitCode = 3;

int BetaReportMode(const char* name)
{
    std::cout << "MODE " << name << ": gate not run\n";
    std::cout.flush();
    return kBetaReportModeExitCode;
}

bool EnvEqualsOne(const char* name)
{
    char* value = nullptr;
    size_t length = 0;
    const bool match = _dupenv_s(&value, &length, name) == 0 && value && std::string(value) == "1";
    std::free(value);
    return match;
}

bool EnvNonEmpty(const char* name)
{
    char* value = nullptr;
    size_t length = 0;
    const bool match = _dupenv_s(&value, &length, name) == 0 && value && *value;
    std::free(value);
    return match;
}
}

int RunPreParameterChecks()
{
    // The live matcher's timing alone (VP_TEST_MATCHER_TIMING=1): skips the suite.
    if (EnvEqualsOne("VP_TEST_MATCHER_TIMING"))
    {
        RunBetaMatcherTiming();
        return BetaReportMode("VP_TEST_MATCHER_TIMING");
    }
    // Gate verify 2026-10-17: the sigma-delta bias probe alone (VP_BETA_SIGMA_BIAS=1).
    if (EnvEqualsOne("VP_BETA_SIGMA_BIAS"))
    {
        RunBetaSigmaBias();
        return BetaReportMode("VP_BETA_SIGMA_BIAS");
    }
    return -1;
}

int RunPostParameterChecks()
{
    RunBetaGpuTests();
    std::cout << "PASS Beta GPU: output clamp keeps footroom/headroom; encoding correct after PREOUTPUT hooks without the repair\n";
    RunBetaOutputFloat32Tests();
    {
        if (EnvEqualsOne("VP_TEST_TIMING"))
        {
            RunBetaFboTiming();
            RunBetaPassTiming();
        }
        const bool runScenario = EnvEqualsOne("VP_TEST_SCENARIO");
        const bool runQuality = EnvEqualsOne("VP_TEST_QUALITY");
        if (EnvEqualsOne("VP_TEST_DEJUDDER_QUALITY"))
        {
            RunBetaDejudderQuality();
            return BetaReportMode("VP_TEST_DEJUDDER_QUALITY");
        }
        if (EnvEqualsOne("VP_TEST_DITHER_COMPARE"))
        {
            RunBetaDitherCompare();
            return BetaReportMode("VP_TEST_DITHER_COMPARE");
        }
        if (EnvEqualsOne("VP_TEST_TIMING_GATE"))
        {
            RunBetaTimingGate();
            return BetaReportMode("VP_TEST_TIMING_GATE");
        }
        if (EnvEqualsOne("VP_TEST_MOTION_CORPUS"))
        {
            RunBetaMotionCorpus();
            return BetaReportMode("VP_TEST_MOTION_CORPUS");
        }
        if (RunBetaCameraAccuracyIfRequested())
            return BetaReportMode("VP_TEST_CAMERA_ACCURACY");
        if (EnvNonEmpty("VP_TEST_REAL_FOOTAGE"))
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
    RunBetaDitherNeedWarpForkTests();
    std::cout << "PASS Beta features: sigma-delta (8-in-10, true 10-bit), LED compensation, occlusion mask, de-judder\n";
    RunBetaExtendedRangeGpuTests();
    std::cout << "PASS Beta extended range: BTB/WTW kept through scaling, in-range edges and colours as without the switch\n";
    RunBetaChainGpuTest();
    std::cout << "PASS Beta whole chain: every hook at once (still, cut, motion, de-judder extra, LED gain sequence, upscale, matrix), BTB/WTW and in-range kept, nothing disabled\n";
    return 0;
}
}
