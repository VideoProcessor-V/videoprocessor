// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Rule 25 (new file): the one pointer C++ needs from Bill's object to VP-V's per-renderer state
// (refs/107-host-seams-plan.md s1, seam S8 STATE ANCHOR; refs/110 s2.1, P11a-0).
//
// `Vpv::HostState` is the S8 state anchor: ONE member of the renderer's Impl (`Vpv::HostState vpv;`,
// declared directly after `captureBinding`, in the same VPV-HOOK run). The VP-V members that P11a moves
// out of Bill's class become FIELDS of the sub-struct of their owning module and are reached from the
// VP .inl bodies as `vpv.led.x`, `vpv.calibration.x`, ... The anchor holds data only: no behaviour, no
// registration, no hub entry. It exists in both pathways; behaviour stays gated where it is today.
//
// Not the hub's internal per-host map entry `Vpv::Hub::HostState` (ExtensionHub.h) and not the skeleton
// module's `Vpv::Skeleton::HostState` (SkeletonModule.h / .cpp): those are nested-namespace types, so an
// unqualified `HostState` inside Vpv::Hub / Vpv::Skeleton still names their own type (no `using namespace
// Vpv...` directive exists in src at b6bdfc9fb).
//
// P11a-0 declares the sub-structs EMPTY; each later move (refs/110 s2.2-2.6) fills exactly one of them:
//   motion.intent   s2.2 motion-intent part of intent items 2-5 (switches/motion configure, ShotEase)  -> motion\consumers
//   led.intent      s2.2 ambient, black intent and LED-limit parts of intent items 2-5                 -> drive\led_compensation
//   led             s2.4 ambient SDR lift, LedDisplayGamma, dynamicDrive, the LED/SDR target-black share -> drive\led_compensation
//   status          s2.3 state behind the public status methods (C14 StatusExtension)                  -> control\diagnostics
//   calibration     s2.5 calibrationHost and the calibration-pattern state                             -> output\calibration_lut
//   sourceAnalysis  s2.6 sourceMonitor, startupCropHold, edgeBlanking (srcres latch)                   -> input\source_analysis
// Initialisation order: the anchor is constructed after captureBinding registers and destroyed before it
// is removed; a field that a capture callback can reach must account for that when it moves in.
//
// C++14 (VideoProcessor-Lib / renderer / Test projects): no inline variables, no if constexpr.

namespace Vpv
{
	struct HostState
	{
		// motion\consumers (s2.2)
		struct Motion
		{
			struct Intent
			{
			};
			Intent intent;
		};

		// drive\led_compensation (s2.2 intent part as `led.intent`; s2.4 LED, ambient, target black)
		struct Led
		{
			struct Intent
			{
			};
			Intent intent;
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
