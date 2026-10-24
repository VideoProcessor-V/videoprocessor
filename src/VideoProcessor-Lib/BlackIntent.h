#pragma once

// Intent interpreter item 4: black protection (Precision Black analog).
// docs/VP-intent-interpreter-design.md section 2. Pure logic.
//
// Report: how many shadow steps between the DV L1 minimum and the measured
// p10 fall below display black + one 8-bit step. Never feeds L1 min into
// libplacebo min_luma (that would map the scene floor to black and crush it):
// nothing here returns a value for the renderer's tone curve.
// led_protect: the LED dimming may not push the scene's p5 below one 8-bit
// code after compensation. Code 0, BTB and the floor never move: everything
// here only reads codes (BTB counts as black in the statistics).
//
// Definitions used throughout:
// - "One 8-bit step" is one code of the 8-bit limited-range video grid
//   (219 steps black..white, the FL35's 8-bit link), i.e. a normalised signal
//   step of 1/219 at black.
// - "Shadow steps" between two light levels are the 8-bit codes of the
//   SOURCE (its own transfer, on the 8-bit grid of its own range) strictly
//   above its black code (16 limited / 0 full; black itself is not a step and
//   never moves) whose light lies within [low, high]. A step is "under the
//   threshold" when its light, as the display shows it, is below display
//   black + one 8-bit step of the display: it cannot be told from black.
//   Shadows sit far below any tone-mapping knee, so the source light of a
//   shadow code is taken as its displayed light (PQ: ST 2084 nits; gamma:
//   BT.1886 onto the display's black and white).

#include <DisplayProfile.h>
#include <IntentDescriptor.h>
#include <SceneDescriptor.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace BlackIntent
{
    // Transfer of the codes the shadow statistics are measured on.
    enum class Transfer : uint8_t { Pq, Gamma, Unknown };

    // One 8-bit limited-range step at black (normalised signal).
    constexpr double kEightBitStep = 1.0 / 219.0;

    struct ShadowStats
    {
        bool valid = false;
        float p1Nits = 0.0f, p5Nits = 0.0f, p10Nits = 0.0f;
        uint32_t samples = 0;
        // How the statistics were measured (for the step count in Evaluate).
        Transfer transfer = Transfer::Unknown;
        bool limitedRange = true;
        double gamma = 2.4;
        double sdrWhiteNits = 100.0;
        double blackNits = 0.0;
    };

    // BT.1886 EOTF: normalised signal v (0 = black) -> nits on a display of
    // white lw, black lb, exponent gamma.
    inline double Bt1886Nits(double v, double gamma, double lw, double lb)
    {
        if (!(gamma > 0.0)) gamma = 2.4;
        lw = (std::max)(lw, 0.0);
        lb = (std::max)(0.0, (std::min)(lb, lw));
        const double w = std::pow(lw, 1.0 / gamma);
        const double k = std::pow(lb, 1.0 / gamma);
        if (!(w > k))
            return lb;
        const double a = std::pow(w - k, gamma);
        const double b = k / (w - k);
        return a * std::pow((std::max)(0.0, v + b), gamma);
    }

    // The light of a normalised signal v (0 = black) in the statistics' transfer.
    inline double SignalNits(const ShadowStats& stats, double v)
    {
        v = (std::max)(0.0, v);
        if (stats.transfer == Transfer::Pq)
            return SceneSt2084::PqToNits(static_cast<float>(v));
        return Bt1886Nits(v, stats.gamma, stats.sdrWhiteNits, stats.blackNits);
    }

    // Shadow percentiles from luma codes of the active picture (bars
    // excluded by the caller). `bitDepth` 8..16; limited range codes below
    // black (BTB, including code 0) count as black for the percentiles (they
    // are never moved). Gamma: `gamma` (e.g. 2.4) over [black, white] =
    // [displayBlackNits, sdrWhiteNits] (BT.1886). PQ: SMPTE ST 2084.
    // Percentile p = the light of the lowest code at or below which at least
    // p % of the samples lie. The codes are only read.
    inline ShadowStats ShadowStatsFromCodes(const uint16_t* codes, size_t count, int bitDepth, bool limitedRange,
        Transfer transfer, double gamma, double sdrWhiteNits, double displayBlackNits)
    {
        ShadowStats stats;
        stats.transfer = transfer;
        stats.limitedRange = limitedRange;
        stats.gamma = gamma > 0.0 ? gamma : 2.4;
        stats.sdrWhiteNits = sdrWhiteNits;
        stats.blackNits = displayBlackNits;
        if (!codes || count == 0 || bitDepth < 8 || bitDepth > 16 || transfer == Transfer::Unknown ||
            (transfer == Transfer::Gamma && !(sdrWhiteNits > 0.0)))
            return stats;
        const uint32_t levels = 1u << bitDepth;
        std::vector<uint32_t> histogram(levels, 0u);
        for (size_t i = 0; i < count; ++i)
            ++histogram[(std::min)(static_cast<uint32_t>(codes[i]), levels - 1u)];
        const double scale = static_cast<double>(1u << (bitDepth - 8));
        auto normalised = [&](uint32_t code)
        {
            if (limitedRange)
                return (std::max)(0.0, (static_cast<double>(code) - 16.0 * scale) / (219.0 * scale));
            return static_cast<double>(code) / static_cast<double>(levels - 1u);
        };
        auto percentile = [&](double p)
        {
            const double needed = std::ceil(p / 100.0 * static_cast<double>(count) - 1e-9);
            uint64_t seen = 0;
            for (uint32_t code = 0; code < levels; ++code)
            {
                seen += histogram[code];
                if (static_cast<double>(seen) >= needed && seen > 0)
                    return code;
            }
            return levels - 1u;
        };
        stats.p1Nits = static_cast<float>(SignalNits(stats, normalised(percentile(1.0))));
        stats.p5Nits = static_cast<float>(SignalNits(stats, normalised(percentile(5.0))));
        stats.p10Nits = static_cast<float>(SignalNits(stats, normalised(percentile(10.0))));
        stats.samples = static_cast<uint32_t>((std::min)(count, static_cast<size_t>(UINT32_MAX)));
        stats.valid = true;
        return stats;
    }

    // The visibility threshold: display black + one 8-bit step at black on a
    // display with the given EOTF, peak and black. BT.1886-like displays
    // (bt1886, gamma24 and anything unknown) use exponent 2.4, gamma22 / sRGB
    // 2.2; a PQ display: one 8-bit step above black's PQ value.
    inline double ThresholdNits(double displayPeakNits, double displayBlackNits, DisplayEotf eotf)
    {
        const double black = (std::max)(0.0, displayBlackNits);
        if (!(displayPeakNits > black))
            return black;
        if (eotf == DisplayEotf::Pq)
        {
            const double pq = SceneSt2084::NitsToPq(static_cast<float>(black));
            return (std::max)(black, static_cast<double>(SceneSt2084::PqToNits(static_cast<float>(pq + kEightBitStep))));
        }
        const double gamma = eotf == DisplayEotf::Gamma22 || eotf == DisplayEotf::Srgb ? 2.2 : 2.4;
        return Bt1886Nits(kEightBitStep, gamma, displayPeakNits, black);
    }

    // The visibility threshold: display black + one 8-bit step at black on a
    // BT.1886-like display of the given peak and black.
    inline double ThresholdNits(double displayPeakNits, double displayBlackNits)
    {
        return ThresholdNits(displayPeakNits, displayBlackNits, DisplayEotf::Bt1886);
    }

    // led_protect: the lowest light share (0..1 of the LED's full drive) at
    // which, after the gain compensation, the scene's p5 still lands at least
    // one 8-bit code above black. 0 = no cap (no valid statistics, or p5 is
    // black itself / already under the threshold, so there is nothing to keep).
    //
    // Derivation from the drive's gain model (VP-V/modules/drive/projector_control/LedController.h,
    // CompensationGainForLed; LedDriveModule): the projector's light above
    // black scales with the LED's share s (flux(level) / flux(ceiling)). VP's
    // compensation gain is 1/s only while it is free; it is capped by the
    // highlight cap (1/highlight, >= 1), held through fades, shaped after
    // bright cuts, and exactly 1 with adaptive_gamma off (the command_source
    // default) and in internal_script mode. The floor must hold in that worst
    // case, gain 1: p5 shows s x (L5 - B) above black, which must stay at least
    // one 8-bit step (T - B, the full-drive display's finest visible step at
    // black, an absolute light the eye must still see). So
    //     s_min = (T - B) / (L5 - B), clamped to 0..1.
    // With the full gain 1/s the cap costs nothing (p5 is restored anyway).
    inline double LedMinimumShare(const ShadowStats& shadows, double displayPeakNits, double displayBlackNits,
        DisplayEotf eotf = DisplayEotf::Bt1886)
    {
        if (!shadows.valid || !(displayPeakNits > displayBlackNits))
            return 0.0;
        const double black = (std::max)(0.0, displayBlackNits);
        const double threshold = ThresholdNits(displayPeakNits, black, eotf);
        const double p5 = shadows.p5Nits;
        if (!(p5 > threshold) || !(threshold > black))
            return 0.0;
        return (std::max)(0.0, (std::min)(1.0, (threshold - black) / (p5 - black)));
    }

    // The report (design: steps between L1 min and p10 under the threshold).
    // Without an L1 minimum the measured p1 stands for the scene floor.
    // `eotf`: the display's (DisplayProfile.eotf) for the threshold.
    inline BlackIntentStatus Evaluate(BlackIntentMode mode, const ShadowStats& shadows,
        const IntentField<float>& l1MinNits, double displayPeakNits, double displayBlackNits,
        DisplayEotf eotf = DisplayEotf::Bt1886)
    {
        BlackIntentStatus s;
        s.mode = mode;
        s.displayBlackNits = static_cast<float>(displayBlackNits);
        const double threshold = ThresholdNits(displayPeakNits, displayBlackNits, eotf);
        s.thresholdNits = static_cast<float>(threshold);
        if (l1MinNits.valid)
        {
            s.l1MinNits = l1MinNits.value;
            s.l1Source = l1MinNits.source;
        }
        s.p1Nits = shadows.p1Nits;
        s.p5Nits = shadows.p5Nits;
        s.p10Nits = shadows.p10Nits;
        if (mode == BlackIntentMode::Off)
        {
            CopyIntentText(s.reason, "off");
            return s;
        }
        if (!shadows.valid || shadows.transfer == Transfer::Unknown)
        {
            CopyIntentText(s.reason, "no shadow statistics");
            return s;
        }
        if (!(displayPeakNits > displayBlackNits))
        {
            CopyIntentText(s.reason, "no display peak");
            return s;
        }
        double low = l1MinNits.valid ? static_cast<double>(l1MinNits.value) : static_cast<double>(shadows.p1Nits);
        double high = shadows.p10Nits;
        if (low > high) std::swap(low, high);
        const int maxCode = shadows.limitedRange ? 235 : 255;
        const int blackCode = shadows.limitedRange ? 16 : 0;
        const double span = shadows.limitedRange ? 219.0 : 255.0;
        // The percentiles are floats: compare with a relative tolerance.
        constexpr double kTolerance = 1e-6;
        for (int code = blackCode + 1; code <= maxCode; ++code)
        {
            const double nits = SignalNits(shadows, (code - blackCode) / span);
            if (nits < low * (1.0 - kTolerance)) continue;
            if (nits > high * (1.0 + kTolerance)) break;
            ++s.stepsTotal;
            if (nits < threshold * (1.0 - kTolerance)) ++s.stepsBelow;
        }
        if (mode == BlackIntentMode::LedProtect)
            s.ledMinimumShare = static_cast<float>(LedMinimumShare(shadows, displayPeakNits, displayBlackNits, eotf));
        s.valid = true;
        char text[64];
        std::snprintf(text, sizeof(text), "%d of %d shadow steps under black+1 (%s)", s.stepsBelow, s.stepsTotal,
            l1MinNits.valid ? "L1 min" : "p1 floor");
        CopyIntentText(s.reason, text);
        return s;
    }
}
