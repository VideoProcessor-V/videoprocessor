#pragma once

// Intent interpreter item 5: display mapping readout (one-way mapping).
// docs/VP-intent-interpreter-design.md section 3. Pure logic; report only.
//
// CONTRACT SCAFFOLD (lead, 2026-10-17): the API below is fixed for the
// integration agent; the body is a placeholder the logic agent replaces.
//
// Compression need = PQ(L1 max) - PQ(display peak); the trim choice reads
// DisplayProfile.peakNits; never expand above the master. Output: a DV-card
// line, e.g. "scene 1000 nit -> 48 nit (-4.4 stops), trim L2 100 nit".

#include <DisplayProfile.h>
#include <IntentDescriptor.h>
#include <SceneDescriptor.h>

namespace DisplayIntent
{
    // `measuredSceneMaxNits` > 0: VP's own measurement, used (as Inferred)
    // when the descriptor carries no L1 max.
    inline DisplayIntentStatus Evaluate(bool enabled, const SceneDescriptor& scene, const DisplayProfile& display,
        double measuredSceneMaxNits)
    {
        DisplayIntentStatus s;
        s.enabled = enabled;
        (void)scene; (void)display; (void)measuredSceneMaxNits;
        return s;
    }
}
