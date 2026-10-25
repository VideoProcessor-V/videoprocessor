// VP-V forwarding header (M2 step, feat/m2-motion): MotionValidity.h moved to src/VP-V/modules/motion/estimation/.
// It stays only for includers that keep the old path (Bill's files incl. the renderer host, and
// RendererCaptureHarnessTests.cpp); new code includes <VP-V/modules/motion/estimation/MotionValidity.h>.
// Listed in the module's MANIFEST.json "forwarding_headers"; removed at M2 step 0.
#pragma once
#include <VP-V/modules/motion/estimation/MotionValidity.h>
