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
		// The lowest highlight of the last highlight_persist_ms: a flash shorter
		// than that never raises it.
		double sustainedHighlight = 16.0;
		// The lowest APL of the same window: mode: compensate aims the LED at it,
		// so a flash never makes the LED chase it (a fall still acts at once).
		double sustainedApl = 16.0;
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
		// mode: compensate. The per-frame gain is ceiling / modelled LED level
		// (the module computes it), limited towards highlightCap by capWeight
		// (large sustained highlights), or held at its last value while
		// holdGain is set (fades to black).
		double highlightCap = 1e9;
		double capWeight = 0.0;
		bool holdGain = false;
		std::string log;
	};

	// The light level the configured curve gives for an APL (code units),
	// truncated like the script; not floored or limited.
	int CurveLevel(const LedDriveConfig& config, int apl);

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

	// mode: compensate (docs/VP-led-drive-module.md 4.2-4.4). The projector's
	// contrast stays at 50; the picture is compensated in linear light so only
	// the black floor moves. Highlight policy: persistence gates protection
	// (via FrameStats::sustainedHighlight), area decides between the knee and
	// the global gain, everything continuous; one taste parameter,
	// highlight_tolerance_stops.
	class CompensateController
	{
	public:
		explicit CompensateController(const LedDriveConfig& config);
		void Reset(int light);
		// elapsedMs: time since the previous tick.
		ControllerOutput Tick(const FrameStats& stats, int elapsedMs);
		int Light() const { return light; }
		double Protection() const { return protection; }
		double Area() const { return area; }

	private:
		LedDriveConfig config;
		int light;
		double protection = 0.0;
		double area = 0.0;
		double previousApl = -1.0;
	};

	// gain_led = ceiling / LED level, in linear light.
	inline double LedGain(int ceiling, double light)
	{
		return light > 0.0 ? static_cast<double>(ceiling) / light : 1.0;
	}

	// The final linear gain for a frame from the modelled LED level and the
	// controller's highlight limit.
	inline double CompensationGainFor(int ceiling, double light, const ControllerOutput& out)
	{
		const double ledGain = LedGain(ceiling, light);
		const double capped = ledGain < out.highlightCap ? ledGain : out.highlightCap;
		return ledGain * (1.0 - out.capWeight) + capped * out.capWeight;
	}
}
