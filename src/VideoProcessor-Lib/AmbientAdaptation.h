#pragma once

// Intent interpreter item 3: ambient adaptation (Light Sense analog, living
// rooms). docs/VP-intent-interpreter-design.md section 4. Pure logic: no
// renderer, GPU or configuration state.
//
// Model: veil V = lux x ambient_screen_reflect; effective black
// B_eff = B + strength x V, capped at ambient_max_black_nits (never below B),
// quantised to 1/6-stop steps of effective black (with hysteresis), applied at
// cuts (or slewed below 1 %/s on static pictures). Strength 0 (or the switch
// off) = the owner's black exactly (bit-identical).
//
// What it changes: only the target black the renderer renders to through the
// BT.1886 black point (LibplaceboRenderParameters::ResolveTargetBlack). BT.1886
// with a raised black lifts the bottom of the curve and converges on the same
// white, so code order (BTB < black < shadows < ... < WTW) is kept and the
// change falls off towards zero above the midtones; peak white never moves.
//
// Open (not built): the optional BT.2390 surround gamma between owner-set
// dark and lit endpoints (design section 4, "Adaptation").

#include <IntentDescriptor.h>

#include <algorithm>
#include <cmath>

namespace AmbientAdapt
{
    struct Settings
    {
        bool enabled = false;               // ambient_adapt
        double strength = 1.0;              // ambient_adapt_strength 0..1
        double screenReflectNitsPerLux = 0.01;  // ambient_screen_reflect (cd/m2 per lux; owner calibration)
        double maxBlackNits = 1.0;          // ambient_max_black_nits (cap on B_eff)
        double tauUpSeconds = 15.0;         // ambient_tau_up_s (room getting brighter)
        double tauDownSeconds = 90.0;       // ambient_tau_down_s (room getting darker)
    };

    // ---- Documented constants (taste / plumbing, never fitted) ----
    // The median window (design: median of 5 rejects flicker and a person
    // walking past: up to two outliers in five readings change nothing).
    constexpr int kMedianWindow = 5;
    // The log-lux filter works on log2(lux + kLuxOffset) so 0 lux (a dark
    // room, manual mode "dark") is a finite state and maps back to exactly 0.
    // 0.5 lux is darker than any lit living room; the veil there is far below
    // one quantisation step for any plausible ambient_screen_reflect.
    constexpr double kLuxOffset = 0.5;
    // No reading for this long = sensor lost (the GUI feeds about 1 Hz).
    constexpr double kSensorLossSeconds = 5.0;
    // After a loss the last value is held this long (a reconnecting USB
    // sensor, a brief GUI stall), then decays toward off (0 lux) with
    // kDecayTauSeconds; below kOffLux it is off (invalid, lux 0).
    constexpr double kHoldSeconds = 60.0;
    constexpr double kDecayTauSeconds = 60.0;
    constexpr double kOffLux = 0.01;
    // Quantisation: steps of 1/6 stop of effective black, B x 2^(k/6).
    constexpr double kStepsPerStop = 6.0;
    // Hysteresis: the step only changes once the continuous value is this far
    // (in steps) outside the current step, so a boundary never flickers.
    constexpr double kHysteresisSteps = 0.25;
    // Slew without cuts: below 1 %/s of the value (design).
    constexpr double kSlewPerSecond = 0.009;
    // The smallest black the step grid / slew is relative to (a zero display
    // black; libplacebo's PL_COLOR_HDR_BLACK is 1e-6).
    constexpr double kMinGridNits = 1e-4;

    // The lux filter: median of 5, asymmetric EMA in log-lux, hold then decay
    // toward off on sensor loss. Times are seconds on one monotonic clock.
    class Filter
    {
    public:
        void Configure(const Settings& settings) { m_settings = settings; }
        const Settings& GetSettings() const { return m_settings; }

        // A reading (lux >= 0; negative is clamped to 0). Non-finite readings
        // are ignored (they do not count as a reading for loss handling).
        void AddReading(double lux, double now)
        {
            if (!std::isfinite(lux) || !std::isfinite(now))
                return;
            lux = (std::max)(0.0, lux);
            m_window[m_windowNext] = lux;
            m_windowNext = (m_windowNext + 1) % kMedianWindow;
            if (m_windowCount < kMedianWindow)
                ++m_windowCount;
            const double target = std::log2(Median() + kLuxOffset);
            if (!m_haveState)
            {
                m_state = target;
                m_haveState = true;
            }
            else
            {
                const double dt = (std::max)(0.0, now - m_lastUpdate);
                const double tau = target > m_state ? m_settings.tauUpSeconds : m_settings.tauDownSeconds;
                m_state += (target - m_state) * Alpha(dt, tau);
            }
            m_lastUpdate = now;
            m_lastReading = now;
            m_valid = true;
            m_phase = Phase::Tracking;
        }

        // Advance time without a reading (sensor loss handling).
        void Tick(double now)
        {
            if (!m_haveState || !std::isfinite(now))
                return;
            if (now - m_lastReading <= kSensorLossSeconds)
                return;
            const double decayStart = m_lastReading + kSensorLossSeconds + kHoldSeconds;
            if (now <= decayStart)
            {
                m_phase = Phase::Holding;
                return;
            }
            m_phase = Phase::Decaying;
            const double from = (std::max)(m_lastUpdate, decayStart);
            const double dt = (std::max)(0.0, now - from);
            const double off = std::log2(kLuxOffset);
            m_state += (off - m_state) * Alpha(dt, kDecayTauSeconds);
            m_lastUpdate = now;
            if (SmoothedLux() < kOffLux)
            {
                // Off: forget everything; the next reading starts afresh.
                Reset();
                m_phase = Phase::Lost;
            }
        }

        bool Valid() const { return m_valid; }
        // The smoothed room light; exactly 0 while invalid or dark.
        double SmoothedLux() const
        {
            if (!m_haveState)
                return 0.0;
            const double lux = std::exp2(m_state) - kLuxOffset;
            return lux < 1e-9 ? 0.0 : lux;
        }
        bool Holding() const { return m_phase == Phase::Holding; }
        bool Decaying() const { return m_phase == Phase::Decaying; }
        // The sensor went away and the value decayed to off.
        bool Lost() const { return m_phase == Phase::Lost; }
        void Reset()
        {
            m_valid = false;
            m_haveState = false;
            m_state = 0.0;
            m_windowCount = 0;
            m_windowNext = 0;
            m_phase = Phase::None;
        }

    private:
        enum class Phase { None, Tracking, Holding, Decaying, Lost };

        static double Alpha(double dt, double tau)
        {
            if (!(tau > 0.0))
                return 1.0;
            return 1.0 - std::exp(-dt / tau);
        }

        double Median() const
        {
            double sorted[kMedianWindow];
            for (int i = 0; i < m_windowCount; ++i)
                sorted[i] = m_window[i];
            std::sort(sorted, sorted + m_windowCount);
            if (m_windowCount % 2 == 1)
                return sorted[m_windowCount / 2];
            return 0.5 * (sorted[m_windowCount / 2 - 1] + sorted[m_windowCount / 2]);
        }

        Settings m_settings;
        bool m_valid = false;
        bool m_haveState = false;
        double m_state = 0.0;           // log2(lux + kLuxOffset)
        double m_lastUpdate = 0.0;
        double m_lastReading = 0.0;
        double m_window[kMedianWindow] = {};
        int m_windowCount = 0;
        int m_windowNext = 0;
        Phase m_phase = Phase::None;
    };

    // The function changes anything at all.
    inline bool Active(const Settings& s)
    {
        return s.enabled && std::isfinite(s.strength) && s.strength > 0.0;
    }

    // B_eff for the smoothed lux (unquantised). Monotonic in lux; never below
    // B; capped at max(B, maxBlackNits). Off / strength 0: B exactly.
    inline double EffectiveBlackNits(const Settings& s, double displayBlackNits, double smoothedLux)
    {
        if (!Active(s) || !std::isfinite(smoothedLux))
            return displayBlackNits;
        const double strength = (std::min)(1.0, s.strength);
        const double reflect = (std::max)(0.0, s.screenReflectNitsPerLux);
        const double veil = (std::max)(0.0, smoothedLux) * reflect;
        const double raised = displayBlackNits + strength * veil;
        const double cap = (std::max)(displayBlackNits, s.maxBlackNits);
        return (std::max)(displayBlackNits, (std::min)(cap, raised));
    }

    namespace Detail
    {
        inline double GridBase(double displayBlackNits)
        {
            return (std::max)(displayBlackNits, kMinGridNits);
        }

        // The continuous step position of `nits` (0 at the grid base).
        inline double StepPosition(double displayBlackNits, double nits)
        {
            const double base = GridBase(displayBlackNits);
            return nits > base ? kStepsPerStop * std::log2(nits / base) : 0.0;
        }

        // Step k on the grid; k == 0 is B itself (exactly).
        inline double GridPoint(double displayBlackNits, int k)
        {
            if (k <= 0)
                return displayBlackNits;
            return GridBase(displayBlackNits) * std::exp2(k / kStepsPerStop);
        }

        inline int StepOf(double displayBlackNits, double gridNits)
        {
            if (!(gridNits > displayBlackNits) || !std::isfinite(gridNits))
                return 0;
            return static_cast<int>(std::lround(StepPosition(displayBlackNits, gridNits)));
        }

        inline int FloorStep(double x)
        {
            return static_cast<int>(std::floor(x + 1e-9));
        }
    }

    // B_eff quantised to the step grid B x 2^(k/6), rounded DOWN (never above
    // the wanted value, so never above the cap). B itself is a grid point: off
    // / strength 0 / no veil returns B exactly. Stateless (no hysteresis).
    inline double QuantisedBlackNits(const Settings& s, double displayBlackNits, double smoothedLux)
    {
        const double wanted = EffectiveBlackNits(s, displayBlackNits, smoothedLux);
        if (!(wanted > displayBlackNits))
            return displayBlackNits;
        return Detail::GridPoint(displayBlackNits,
            Detail::FloorStep(Detail::StepPosition(displayBlackNits, wanted)));
    }

    // The same with hysteresis against the previously quantised value (what
    // the filter wanted last time; pass B the first time): the step changes
    // only when the continuous position leaves [k_prev - h, k_prev + 1 + h)
    // (h = kHysteresisSteps), so a value hovering at a boundary keeps its step.
    // Never above the cap. Off / strength 0 returns B exactly.
    inline double QuantisedBlackNits(const Settings& s, double displayBlackNits, double smoothedLux,
        double previousQuantisedNits)
    {
        if (!Active(s))
            return displayBlackNits;
        const double wanted = EffectiveBlackNits(s, displayBlackNits, smoothedLux);
        const int previous = Detail::StepOf(displayBlackNits, previousQuantisedNits);
        const double x = wanted > displayBlackNits ? Detail::StepPosition(displayBlackNits, wanted) : 0.0;
        int k = previous;
        if (x < previous - kHysteresisSteps || x >= previous + 1.0 + kHysteresisSteps)
            k = Detail::FloorStep(x);
        const double cap = (std::max)(displayBlackNits, s.maxBlackNits);
        if (Detail::GridPoint(displayBlackNits, k) > cap * (1.0 + 1e-12))
            k = Detail::FloorStep(Detail::StepPosition(displayBlackNits, cap));
        return Detail::GridPoint(displayBlackNits, k);
    }

    // When the renderer may move from `appliedNits` to `wantedNits`: at a cut
    // at once; on a picture without cuts a slew below 1 %/s (kSlewPerSecond of
    // the smaller of the two values per second of `secondsSinceLastStep`, the
    // time since this was last evaluated). Returns the value to apply now
    // (== appliedNits, bit for bit, when nothing should change).
    inline double NextAppliedBlackNits(double appliedNits, double wantedNits, bool atCut, double secondsSinceLastStep)
    {
        if (!std::isfinite(wantedNits) || appliedNits == wantedNits)
            return appliedNits;
        if (atCut || !std::isfinite(appliedNits))
            return wantedNits;
        const double dt = std::isfinite(secondsSinceLastStep) ? (std::max)(0.0, secondsSinceLastStep) : 0.0;
        const double reference = (std::max)(kMinGridNits, (std::min)(appliedNits, wantedNits));
        const double step = kSlewPerSecond * reference * dt;
        if (wantedNits > appliedNits)
            return (std::min)(wantedNits, appliedNits + step);
        return (std::max)(wantedNits, appliedNits - step);
    }

    // Fills the plain-data status.
    inline void FillStatus(const Settings& s, const Filter& f, double displayBlackNits, double appliedNits,
        AmbientIntentStatus& out)
    {
        out = AmbientIntentStatus();
        out.enabled = s.enabled;
        out.sensorValid = f.Valid();
        const double lux = f.Valid() ? f.SmoothedLux() : 0.0;
        out.smoothedLux = static_cast<float>(lux);
        out.veilNits = static_cast<float>(lux * (std::max)(0.0, s.screenReflectNitsPerLux));
        out.displayBlackNits = static_cast<float>(displayBlackNits);
        const double wanted = EffectiveBlackNits(s, displayBlackNits, lux);
        const double target = QuantisedBlackNits(s, displayBlackNits, lux);
        out.targetBlackNits = static_cast<float>(target);
        out.appliedBlackNits = static_cast<float>(appliedNits);
        const char* reason = "tracking";
        if (!s.enabled)
            reason = "off";
        else if (!Active(s))
            reason = "strength 0: display black";
        else if (f.Lost())
            reason = "sensor lost: decayed to off";
        else if (!f.Valid())
            reason = "no lux source";
        else if (f.Holding())
            reason = "sensor lost: holding";
        else if (f.Decaying())
            reason = "sensor lost: decaying toward off";
        else if (wanted > displayBlackNits && wanted >= (std::max)(displayBlackNits, s.maxBlackNits))
            reason = "capped at ambient_max_black_nits";
        else if (target == displayBlackNits)
            reason = "veil below one step: display black";
        else if (appliedNits != target)
            reason = "waiting for a cut / slewing";
        CopyIntentText(out.reason, reason);
    }
}
