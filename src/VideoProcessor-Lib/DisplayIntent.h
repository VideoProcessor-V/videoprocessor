#pragma once

// Intent interpreter item 5: display mapping readout (one-way mapping).
// docs/VP-intent-interpreter-design.md section 3. Pure logic; report only
// (nothing here feeds the renderer).
//
// Compression need = PQ(scene max) - PQ(display peak) and
// stops = log2(scene max / display peak), both clamped at 0: never expand
// above the master (that would invent highlights). The trim choice is the
// transmitted L2 trim whose target is closest to DisplayProfile.peakNits
// (closest in PQ, the perceptual distance; L8 trims refer to an L10 target
// VP does not parse, so they carry no target and are not chosen). Scene max:
// DV L1 max when the descriptor has it (DvL1, or Dv2 from a future DV2
// parser), else VP's own measurement (Inferred), else invalid.
// Output: a DV-card line, e.g.
//   "scene 1000 nit -> 48 nit (-4.4 stops), trim L2 100 nit"
//   "scene 1000 nit -> 48 nit (-4.4 stops), no trim"
// with " [inferred]" appended when the scene max is VP's measurement.

#include <DisplayProfile.h>
#include <IntentDescriptor.h>
#include <SceneDescriptor.h>

#include <cmath>
#include <cstdio>

namespace DisplayIntent
{
    // Nits as the DV card shows them: whole nits from 10 up, 3 significant
    // digits below (a dim scene's 0.5 nit max must not read "0 nit").
    inline void FormatNits(char* buffer, size_t size, double nits)
    {
        if (nits >= 9.5)
            std::snprintf(buffer, size, "%.0f", nits);
        else
            std::snprintf(buffer, size, "%.3g", nits);
    }

    // The L2 trim whose target is closest (in PQ) to `peakNits`; -1 when none.
    inline int ChooseTrim(const SceneDescriptor& scene, double peakNits)
    {
        if (!(peakNits > 0.0))
            return -1;
        const float peakPq = SceneSt2084::NitsToPq(static_cast<float>(peakNits));
        int best = -1;
        double bestDistance = 0.0;
        const int count = scene.trimCount < SceneDescriptor::kMaxTrims ? scene.trimCount : SceneDescriptor::kMaxTrims;
        for (int i = 0; i < count; ++i)
        {
            const SceneTrim& trim = scene.trims[i];
            if (trim.level != 2 || !trim.target.present || !(trim.target.nits > 0.0f))
                continue;
            const double distance = std::fabs(static_cast<double>(trim.target.pq) - peakPq);
            if (best < 0 || distance < bestDistance)
            {
                best = i;
                bestDistance = distance;
            }
        }
        return best;
    }

    // `measuredSceneMaxNits` > 0: VP's own measurement, used (as Inferred)
    // when the descriptor carries no L1 max.
    inline DisplayIntentStatus Evaluate(bool enabled, const SceneDescriptor& scene, const DisplayProfile& display,
        double measuredSceneMaxNits)
    {
        DisplayIntentStatus s;
        s.enabled = enabled;
        s.displayPeakNits = display.peakNits;
        if (!enabled)
            return s;
        if (scene.sceneMax.present && scene.sceneMax.nits > 0.0f)
        {
            s.sceneMaxNits = scene.sceneMax.nits;
            s.sceneMaxSource = scene.source == SceneMetadataSource::Dv2 ? IntentSource::Dv2 : IntentSource::DvL1;
        }
        else if (measuredSceneMaxNits > 0.0 && std::isfinite(measuredSceneMaxNits))
        {
            s.sceneMaxNits = static_cast<float>(measuredSceneMaxNits);
            s.sceneMaxSource = IntentSource::Inferred;
        }
        if (s.sceneMaxSource == IntentSource::Off)
        {
            CopyIntentText(s.line, "no scene max (no L1, no measurement)");
            return s;
        }
        if (!(display.peakNits > 0.0f))
        {
            CopyIntentText(s.line, "no display peak");
            return s;
        }
        const double sceneMax = s.sceneMaxNits;
        const double peak = display.peakNits;
        const double pqDifference = static_cast<double>(SceneSt2084::NitsToPq(static_cast<float>(sceneMax))) -
            static_cast<double>(SceneSt2084::NitsToPq(static_cast<float>(peak)));
        s.compressionPq = static_cast<float>(pqDifference > 0.0 ? pqDifference : 0.0);
        const double stops = std::log2(sceneMax / peak);
        s.compressionStops = static_cast<float>(stops > 0.0 ? stops : 0.0);
        const int trim = ChooseTrim(scene, peak);
        if (trim >= 0)
            s.trimNits = scene.trims[trim].target.nits;
        s.valid = true;

        char sceneText[24], peakText[24], trimText[40];
        FormatNits(sceneText, sizeof(sceneText), sceneMax);
        FormatNits(peakText, sizeof(peakText), peak);
        if (trim >= 0)
        {
            char trimNits[24];
            FormatNits(trimNits, sizeof(trimNits), s.trimNits);
            std::snprintf(trimText, sizeof(trimText), "trim L2 %s nit", trimNits);
        }
        else
            std::snprintf(trimText, sizeof(trimText), "no trim");
        char line[sizeof(s.line)];
        // Compression shows as a negative stop count; none as "0.0 stops".
        if (s.compressionStops > 0.0f)
            std::snprintf(line, sizeof(line), "scene %s nit -> %s nit (-%.1f stops), %s%s", sceneText, peakText,
                s.compressionStops, trimText, s.sceneMaxSource == IntentSource::Inferred ? " [inferred]" : "");
        else
            std::snprintf(line, sizeof(line), "scene %s nit -> %s nit (0.0 stops), %s%s", sceneText, peakText,
                trimText, s.sceneMaxSource == IntentSource::Inferred ? " [inferred]" : "");
        CopyIntentText(s.line, line);
        return s;
    }
}
