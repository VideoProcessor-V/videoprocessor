// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Rule 25 (new file): the one pointer C++ needs from Bill's object to VP-V's per-renderer state
// (refs/107-host-seams-plan.md s1, seam S8 STATE ANCHOR; refs/110 s2.1, P11a-0).
//
// `Vpv::HostState` is the S8 state anchor: ONE member of the renderer's Impl (`Vpv::HostState vpv;`,
// declared directly after `captureBinding`, in the same VPV-HOOK run). The VP-V members that P11a moves
// out of Bill's class become FIELDS of the sub-struct of their owning module and are reached from the
// VP .inl bodies as `vpv.led.x`, `vpv.calibration.x`, ... The anchor holds state only: no behaviour, no
// registration, no hub entry. It exists in both pathways; behaviour stays gated where it is today.
// Motion and Led now hold state (P11a-f: the intent state); the anchor is therefore NOT trivially
// destructible, by design (Led::Intent holds a mutex, Motion::Intent a shared_ptr) and is destroyed
// before captureBinding.
//
// Not the hub's internal per-host map entry `Vpv::Hub::HostState` (ExtensionHub.h) and not the skeleton
// module's `Vpv::Skeleton::HostState` (SkeletonModule.h / .cpp): those are nested-namespace types, so an
// unqualified `HostState` inside Vpv::Hub / Vpv::Skeleton still names their own type (no `using namespace
// Vpv...` directive exists in src at b6bdfc9fb).
//
// P11a-0 declared the sub-structs EMPTY; P11a-f filled motion.intent and led.intent, and each later move
// (refs/110 s2.3-2.6) fills exactly one of the rest (the list below is the plan, not the current content):
//   motion.intent   s2.2 motion-intent part of intent items 2-5 (switches/motion configure, ShotEase)  -> motion\consumers
//   led.intent      s2.2 ambient, black intent and LED-limit parts of intent items 2-5                 -> drive\led_compensation
//   led.sdrLift     s2.4 (P11a-d, landed) ambient SDR lift state; LedDisplayGamma and the target-black frame call
//                   move with it (LedSdrLiftMethods.inl). dynamicDrive STAYS an Impl member: see refs/110 follow-ons
//                   (init order: outputColour holds references into it).
//   status          s2.3 state behind the public status methods (C14 StatusExtension)                  -> control\diagnostics
//   calibration     s2.5 calibrationHost and the calibration-pattern state                             -> output\calibration_lut
//   sourceAnalysis  s2.6 sourceMonitor, startupCropHold, edgeBlanking (srcres latch)                   -> input\source_analysis
// Initialisation order: the anchor is constructed after captureBinding registers and destroyed before it
// is removed; a field that a capture callback can reach must account for that when it moves in.
//
// C++14 (VideoProcessor-Lib / renderer / Test projects): no inline variables, no if constexpr.

#include <VP-V/modules/drive/led_compensation/LedIntentState.h>
#include <VP-V/modules/drive/led_compensation/LedSdrLiftState.h>
#include <VP-V/modules/motion/consumers/IntentMotionState.h>

namespace Vpv
{
	struct HostState
	{
		// motion\consumers (s2.2)
		struct Motion
		{
			using Intent = MotionIntentState;   // P11a-f: s2.2 motion-intent state (IntentMotionState.h)
			Intent intent;
		};

		// drive\led_compensation (s2.2 intent part as `led.intent`; s2.4 LED, ambient, target black)
		struct Led
		{
			using Intent = LedIntentState;   // P11a-f: s2.2 ambient / black / LED-limit state (LedIntentState.h)
			Intent intent;
			using SdrLift = LedSdrLiftState;   // P11a-d: s2.4 the ambient SDR black lift (LedSdrLiftState.h)
			SdrLift sdrLift;
		};

		// output\calibration_lut (s2.5)
		struct Calibration
		{
		};

		// input\source_analysis (s2.6)
		struct SourceAnalysis
		{
		};

		// control\diagnostics (s2.3)
		struct Status
		{
		};

		Motion motion;
		Led led;
		Calibration calibration;
		SourceAnalysis sourceAnalysis;
		Status status;
	};
}
