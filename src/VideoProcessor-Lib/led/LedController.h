#pragma once

#include "LedDriveConfig.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Layer 2 of the LED drive module: pure, deterministic control logic. No
// hardware, no renderer, no clock: the caller supplies the statistics and
// runs Tick() every tick_ms. See docs/VP-led-drive-module.md section 4 and
// docs/VP-led-drive-answers-r1.md.
namespace LedDrive
{
	// Per-frame statistics from the measurement layer, in limited-range code
	// units (black 16, white 235) like the script's screenshot.
	struct FrameStats
	{
		double apl = 16.0;
		// Brightness of the brightest part of the picture (the 99.5th
		// percentile of 3x3 block means).
		double highlight = 16.0;
		double highlightArea = 0.0;
		bool cut = false;
		bool fade = false;
		// Block-mean histogram in code units and the number of blocks measured.
		std::array<uint32_t, 256> histogram{};
		uint32_t blocks = 0;
	};

	// One step of the emulated projector contrast, offsetMs after the tick
	// started (the script's fast dumps step every 100 ms).
	struct ContrastStep
	{
		int offsetMs = 0;
		int contrast = 50;
	};

	enum class GainDomain { Encoded = 0, Linear = 1 };

	struct ControllerOutput
	{
		// Light level to command this tick; -1 when it does not change.
		int lightCommand = -1;
		int lightTarget = 255;
		int light = 255;
		int contrastTarget = 50;
		int contrast = 50;
		// Contrast changes within this tick, in order; empty when unchanged.
		std::vector<ContrastStep> contrastSchedule;
		// For the compensation hook. mode: script emulates CNTR as a gain on the
		// encoded signal: contrast / 50.
		double gain = 1.0;
		GainDomain gainDomain = GainDomain::Encoded;
		double knee = 0.08;
		// How long this tick took in the script (its sleeps); the runner waits
		// at least this long before the next tick.
		int durationMs = 0;
		std::string log;
	};

	// beta_22.py ported parameter for parameter. Light: the configured curve
	// and step tables. Contrast: the script's bands and dynamics, emulated in
	// VP (never sent to the projector).
	class ScriptController
	{
	public:
		explicit ScriptController(const LedDriveConfig& config);

		// The projector state after startup (light at the ceiling, contrast 50).
		void Reset(int light, int contrast = 50);
		ControllerOutput Tick(const FrameStats& stats);

		int LightTargetFor(int apl) const;
		static int ContrastTargetFor(int apl, bool highlight);
		int Light() const { return light; }
		int Contrast() const { return contrast; }

	private:
		LedDriveConfig config;
		int light;
		int contrast;
	};
}
