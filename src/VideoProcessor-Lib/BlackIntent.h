#pragma once

// Intent interpreter item 4: black protection (Precision Black analog).
// docs/VP-intent-interpreter-design.md section 2. Pure logic.
//
// CONTRACT SCAFFOLD (lead, 2026-10-17): the API below is fixed for the
// integration agent; the bodies are placeholders the logic agent replaces.
//
// Report: how many shadow steps between the DV L1 minimum and the measured
// p10 fall below display black + one 8-bit step. Never feeds L1 min into
// libplacebo min_luma (that would map the scene floor to black and crush it).
// led_protect: the LED dimming may not push the scene's p5 below one 8-bit
// code after compensation. Code 0, BTB and the floor never move.

#include <IntentDescriptor.h>

#include <cstddef>
#include <cstdint>

namespace BlackIntent
{
    // Transfer of the codes the shadow statistics are measured on.
    enum class Transfer : uint8_t { Pq, Gamma, Unknown };

    struct ShadowStats
    {
        bool valid = false;
        float p1Nits = 0.0f, p5Nits = 0.0f, p10Nits = 0.0f;
        uint32_t samples = 0;
    };

    // Shadow percentiles from luma codes of the active picture (bars
    // excluded by the caller). `bitDepth` 8..16; limited range codes below
    // black (BTB) count as black for the percentiles (they are never moved).
    // Gamma: `gamma` (e.g. 2.4) over [black, white] = [displayBlackNits, sdrWhiteNits].
    inline ShadowStats ShadowStatsFromCodes(const uint16_t* codes, size_t count, int bitDepth, bool limitedRange,
        Transfer transfer, double gamma, double sdrWhiteNits, double displayBlackNits)
    {
        (void)codes; (void)count; (void)bitDepth; (void)limitedRange; (void)transfer; (void)gamma;
        (void)sdrWhiteNits; (void)displayBlackNits;
        return ShadowStats();
    }

    // The visibility threshold: display black + one 8-bit step at black on a
    // BT.1886-like display of the given peak and black.
    inline double ThresholdNits(double displayPeakNits, double displayBlackNits)
    {
        (void)displayPeakNits;
        return displayBlackNits;
    }

    // The report (design: steps between L1 min and p10 under the threshold).
    inline BlackIntentStatus Evaluate(BlackIntentMode mode, const ShadowStats& shadows,
        const IntentField<float>& l1MinNits, double displayPeakNits, double displayBlackNits)
    {
        BlackIntentStatus s;
        s.mode = mode;
        (void)shadows; (void)l1MinNits; (void)displayPeakNits; (void)displayBlackNits;
        return s;
    }

    // led_protect: the lowest light share (0..1 of the LED's full drive) at
    // which, after the gain compensation 1/share, the scene's p5 still lands
    // at least one 8-bit code above black. 0 = no cap (no valid statistics,
    // or p5 is black itself).
    inline double LedMinimumShare(const ShadowStats& shadows, double displayPeakNits, double displayBlackNits)
    {
        (void)shadows; (void)displayPeakNits; (void)displayBlackNits;
        return 0.0;
    }
}
