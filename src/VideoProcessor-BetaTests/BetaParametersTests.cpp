#include "../VideoProcessor-Lib/vprenderer/LibplaceboRenderParameters.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace LibplaceboRenderParameters;
void RunBetaGpuTests(); // BetaGpuTests.cpp
void RunBetaFeatureGpuTests(); // BetaGpuTests.cpp
void RunBetaExtendedRangeGpuTests(); // BetaGpuTests.cpp
void RunBetaChainGpuTest(); // BetaGpuTests.cpp
static void check(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
int main() {
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
        RunBetaFeatureGpuTests();
        std::cout << "PASS Beta features: sigma-delta (8-in-10, true 10-bit), LED compensation, occlusion mask, de-judder\n";
        RunBetaExtendedRangeGpuTests();
        std::cout << "PASS Beta extended range: BTB/WTW kept through scaling, in-range edges and colours as without the switch\n";
        RunBetaChainGpuTest();
        std::cout << "PASS Beta whole chain: every hook at once (still, cut, motion, de-judder extra, LED gain sequence, upscale, matrix), BTB/WTW and in-range kept, nothing disabled\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
}
