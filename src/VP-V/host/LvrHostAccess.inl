// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CC slice 12 step 12.2 (refs/112 P2: ONE host include in place of the per-module .inl lines): the out-of-line bodies of the
// VP-V members of LibplaceboVideoRenderer::Impl, in the SAME order and at the SAME place in the translation unit as the eleven
// include lines this replaces (right after the Impl struct; each file's own header says what it holds). It is a fragment of
// LibplaceboVideoRenderer.cpp: included exactly once, by one `// VPV-HOOK` line; no other translation unit may include it.
// Tracked follow-up (CC-2): the bodies become free functions over Vpv::HostAccess (refs/112 P2 end state).
#include <VP-V/modules/motion/estimation/VectorEngineEstimate.inl>   // P5a-5: the estimate and the engine keeper (Impl members)
#include <VP-V/modules/motion/consumers/DejudderPolicyRunner.inl>   // M2 P5b-3: the de-judder policy (Impl members)
#include <VP-V/modules/motion/consumers/PhaseTimeline.inl>   // M2 P5b-4: phase picture, timeline helpers, status, release (Impl members)
#include <VP-V/modules/input/capture/CaptureImpl.inl>   // P10: the capture code (Impl members)
#include <VP-V/segments/CoreSegments.inl>   // P4-2: UpdatePipelineSlots, OrderFrameSlotHooks (Impl members)
#include <VP-V/segments/EntranceHostMembers.inl>   // P4-5: the entrance host B3 helpers, RenderCrossfadeMix, DrainEntranceBeforeRender (Impl members)
#include <VP-V/segments/EntrancePlanner.inl>   // P4 e: the entrance planner (Impl member)
#include <VP-V/modules/motion/consumers/IntentMotionMethods.inl>   // P11a-f: the motion-intent part of intent items 2-5 (Impl members)
#include <VP-V/modules/drive/led_compensation/LedIntentMethods.inl>   // P11a-f: the ambient / black / LED-limit parts of intent items 2-5 (Impl members)
#include <VP-V/modules/drive/led_compensation/LedSdrLiftMethods.inl>   // P11a-d: the ambient SDR lift and LedDisplayGamma (Impl members)
#include <VP-V/modules/output/calibration_lut/CalibrationPresentMethods.inl>   // P11a-e: the calibration pattern present (Impl member)
