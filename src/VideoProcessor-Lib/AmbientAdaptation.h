#pragma once

// Intent interpreter item 3: ambient adaptation (Light Sense analog, living
// rooms). docs/VP-intent-interpreter-design.md section 4. Pure logic: no
// renderer, GPU or configuration state.
//
// CONTRACT SCAFFOLD (lead, 2026-10-17): the API below is fixed for the
// integration agent; the bodies are placeholders the logic agent replaces.
//
// Model: veil V = lux x ambient_screen_reflect; effective black
// B_eff = B + strength x V, capped at ambient_max_black_nits, quantised to
// about 1/6-stop steps, applied at cuts (or slewed below 1 %/s on static
// pictures). Strength 0 (or the switch off) = the owner's black exactly.

#include <IntentDescriptor.h>

#include <algorithm>
#include <cmath>

namespace AmbientAdapt
{
    struct Settings
    {
        bool enabled = false;               // ambient_adapt
        double strength = 1.0;              // ambient_adapt_strength 0..1
        double screenReflectNitsPerLux = 0.0;   // ambient_screen_reflect (cd/m2 per lux; owner calibration)
        double maxBlackNits = 1.0;          // ambient_max_black_nits (cap on B_eff)
        double tauUpSeconds = 15.0;         // ambient_tau_up_s (room getting brighter)
        double tauDownSeconds = 90.0;       // ambient_tau_down_s (room getting darker)
    };

    // The lux filter: median of 5, asymmetric EMA in log-lux, hold then decay
    // toward off on sensor loss. Times are seconds on one monotonic clock.
    class Filter
    {
    public:
        void Configure(const Settings& settings) { m_settings = settings; }
        const Settings& GetSettings() const { return m_settings; }
        // A reading (lux >= 0). Non-finite readings are ignored.
        void AddReading(double lux, double now) { (void)now; if (std::isfinite(lux)) { m_lux = lux; m_valid = true; } }
        // Advance time without a reading (sensor loss handling).
        void Tick(double now) { (void)now; }
        bool Valid() const { return m_valid; }
        double SmoothedLux() const { return m_lux; }
        void Reset() { m_valid = false; m_lux = 0.0; }

    private:
        Settings m_settings;
        bool m_valid = false;
        double m_lux = 0.0;
    };

    // B_eff for the smoothed lux (unquantised).
    inline double EffectiveBlackNits(const Settings& s, double displayBlackNits, double smoothedLux)
    {
        if (!s.enabled || s.strength <= 0.0)
            return displayBlackNits;
        const double veil = (std::max)(0.0, smoothedLux) * s.screenReflectNitsPerLux;
        return (std::min)((std::max)(displayBlackNits, s.maxBlackNits), displayBlackNits + s.strength * veil);
    }

    // B_eff quantised to the step grid (about 1/6 stop of effective black).
    inline double QuantisedBlackNits(const Settings& s, double displayBlackNits, double smoothedLux)
    {
        return EffectiveBlackNits(s, displayBlackNits, smoothedLux);
    }

    // When the renderer may move from `appliedNits` to `wantedNits`: at a cut
    // at once; on a picture without cuts a slew below 1 %/s. Returns the value
    // to apply now (== appliedNits when nothing should change yet).
    inline double NextAppliedBlackNits(double appliedNits, double wantedNits, bool atCut, double secondsSinceLastStep)
    {
        (void)secondsSinceLastStep;
        return atCut ? wantedNits : appliedNits;
    }

    // Fills the plain-data status.
    inline void FillStatus(const Settings& s, const Filter& f, double displayBlackNits, double appliedNits,
        AmbientIntentStatus& out)
    {
        out = AmbientIntentStatus();
        out.enabled = s.enabled;
        out.sensorValid = f.Valid();
        out.smoothedLux = static_cast<float>(f.SmoothedLux());
        out.veilNits = static_cast<float>(f.SmoothedLux() * s.screenReflectNitsPerLux);
        out.displayBlackNits = static_cast<float>(displayBlackNits);
        out.targetBlackNits = static_cast<float>(QuantisedBlackNits(s, displayBlackNits, f.SmoothedLux()));
        out.appliedBlackNits = static_cast<float>(appliedNits);
    }
}
