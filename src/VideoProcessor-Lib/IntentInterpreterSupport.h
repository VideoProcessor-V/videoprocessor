#pragma once

// Intent interpreter items 2-5 (2026-10-17): small pure helpers shared by the
// renderer and the GUI, kept here so they are CPU-tested:
// - the lux the GUI hands the renderer (IRenderer::SetAmbientLux) from the
//   [ambient] detector (contract: sensor = newest reading; manual lit =
//   lux_lit, dark = 0 lux; off or detector lost = invalid);
// - the ambient target-black step (applied at cuts, or the slew on a static
//   picture) with the owner's black returned EXACTLY while ambient_adapt is
//   off or its strength is 0 (bit-identical rendering);
// - change detection for the LED intent limits.

#include <VP-V/modules/colour/appearance/AmbientAdaptation.h>
#include <AmbientState.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace IntentInterpreter
{
    // The renderer's ambient input from the GUI's room detector.
    // manualRoomOverride: the room a key override set (manual mode only).
    inline bool AmbientLuxFromDetector(const Ambient::State& state, double nowSeconds, double& lux,
        const Ambient::Room* manualRoomOverride = nullptr)
    {
        lux = 0.0;
        const Ambient::Settings& s = state.GetSettings();
        switch (s.mode)
        {
        case Ambient::Mode::Manual:
            lux = (manualRoomOverride ? *manualRoomOverride : s.room) == Ambient::Room::Lit ? s.luxLit : 0.0;
            return true;
        case Ambient::Mode::Sensor:
            if (!state.Sensed() || state.DetectorLost(nowSeconds) || !std::isfinite(state.LastLux()))
                return false;
            lux = state.LastLux() < 0.0 ? 0.0 : state.LastLux();
            return true;
        default:
            return false;
        }
    }

    // The black the renderer renders to (B_eff), stepped only at cuts (or by
    // AmbientAdapt::NextAppliedBlackNits' slew on a picture without cuts).
    struct AmbientApplyState
    {
        bool valid = false;
        double applied = 0.0;
        double ownerBlack = 0.0;
        double lastStepSeconds = 0.0;
    };

    inline double AmbientStep(AmbientApplyState& st, const AmbientAdapt::Settings& s, bool filterValid,
        double smoothedLux, double ownerBlackNits, bool atCut, double nowSeconds)
    {
        if (!s.enabled || !(s.strength > 0.0))
        {
            st.valid = false;
            return ownerBlackNits;   // exactly the owner's black: bit-identical
        }
        if (!st.valid || st.ownerBlack != ownerBlackNits)
        {
            st.valid = true;
            st.applied = ownerBlackNits;
            st.ownerBlack = ownerBlackNits;
            st.lastStepSeconds = nowSeconds;
        }
        const double wanted = filterValid ?
            AmbientAdapt::QuantisedBlackNits(s, ownerBlackNits, smoothedLux) : ownerBlackNits;
        const double next = AmbientAdapt::NextAppliedBlackNits(st.applied, wanted, atCut,
            nowSeconds - st.lastStepSeconds);
        if (std::isfinite(next) && next != st.applied)
        {
            // Never below the owner's black (BTB and the floor never move down).
            st.applied = next < ownerBlackNits ? ownerBlackNits : next;
            st.lastStepSeconds = nowSeconds;
        }
        return st.applied;
    }

    // What the renderer last handed LedDriveModule::SetIntentLimits.
    struct LedIntentLimits
    {
        double shadowMinimumShare = 0.0;
        double ambientBlackNits = 0.0;
    };

    inline bool LedLimitsDiffer(const LedIntentLimits& a, const LedIntentLimits& b)
    {
        return std::fabs(a.shadowMinimumShare - b.shadowMinimumShare) >= 0.005 ||
            std::fabs(a.ambientBlackNits - b.ambientBlackNits) > 1e-6 ||
            ((a.shadowMinimumShare == 0.0) != (b.shadowMinimumShare == 0.0));
    }

    // ---- the GUI's text (stats overlay lines, crash state, log; empty = off) ----
    inline std::string MotionIntentLine(const MotionIntentStatus& m)
    {
        if (!m.enabled) return std::string();
        char text[256];
        std::snprintf(text, sizeof(text), "%s x%.2f (%s), shot pan %.3f h/s spread %.3f edge %.2f J %.2f (%s), next x%.2f; %s",
            ToString(m.policy), m.appliedScale, DisplayName(m.source), m.panPictureHeightsPerSecond, m.panSpread,
            m.edgeContrast, m.visibility, m.vectorsUsed ? "vectors" : "cpu", m.candidateScale, m.reason);
        return text;
    }

    inline std::string AmbientIntentLine(const AmbientIntentStatus& a)
    {
        if (!a.enabled) return std::string();
        char text[256];
        if (!a.sensorValid)
            std::snprintf(text, sizeof(text), "black %.4f nits (owner %.4f); %s", a.appliedBlackNits, a.displayBlackNits,
                a.reason[0] ? a.reason : "no room reading");
        else
            std::snprintf(text, sizeof(text), "black %.4f nits (target %.4f, owner %.4f), %.1f lux, veil %.4f nits%s%s",
                a.appliedBlackNits, a.targetBlackNits, a.displayBlackNits, a.smoothedLux, a.veilNits,
                a.reason[0] ? "; " : "", a.reason);
        return text;
    }

    inline std::string BlackIntentLine(const BlackIntentStatus& b)
    {
        if (b.mode == BlackIntentMode::Off) return std::string();
        char text[256];
        if (!b.valid)
            std::snprintf(text, sizeof(text), "%s: no shadow statistics%s%s", ToString(b.mode),
                b.reason[0] ? "; " : "", b.reason);
        else
            std::snprintf(text, sizeof(text), "%s: %d of %d shadow steps below %.4f nits, p5 %.4f nits, L1 min %s%s",
                ToString(b.mode), b.stepsBelow, b.stepsTotal, b.thresholdNits, b.p5Nits,
                b.l1Source == IntentSource::Off ? "absent" : std::to_string(b.l1MinNits).c_str(),
                b.mode == BlackIntentMode::LedProtect ? (std::string(", LED min share ") + std::to_string(b.ledMinimumShare)).c_str() : "");
        return text;
    }

    inline std::string DisplayIntentLine(const DisplayIntentStatus& d)
    {
        if (!d.enabled) return std::string();
        if (!d.valid || !d.line[0]) return "no scene maximum";
        return d.line;
    }

    // What a change log keys on (not the per-second measurements).
    inline std::string MotionLogKey(const MotionIntentStatus& m)
    {
        if (!m.enabled) return std::string();
        char text[160];
        std::snprintf(text, sizeof(text), "%s x%.2f (%s) %s", ToString(m.policy), m.appliedScale, DisplayName(m.source), m.reason);
        return text;
    }

    inline std::string AmbientLogKey(const AmbientIntentStatus& a)
    {
        if (!a.enabled) return std::string();
        char text[96];
        std::snprintf(text, sizeof(text), "black %.4f nits valid=%d", a.appliedBlackNits, a.sensorValid ? 1 : 0);
        return text;
    }

    inline std::string BlackLogKey(const BlackIntentStatus& b)
    {
        if (b.mode == BlackIntentMode::Off) return std::string();
        char text[96];
        std::snprintf(text, sizeof(text), "%s valid=%d steps=%d/%d", ToString(b.mode), b.valid ? 1 : 0, b.stepsBelow, b.stepsTotal);
        return text;
    }
}
