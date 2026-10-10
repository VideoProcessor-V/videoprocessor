#pragma once

#include "LedDriveConfig.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Layer 2 of the LED drive module: pure, deterministic control logic. No
// hardware, no renderer, no clock: the caller supplies the statistics and
// runs Tick() every tick_ms. See docs/VP-led-drive-module.md section 4 and
// docs/VP-led-drive-answers-1.md.
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
		// The lowest APL of the last apl_persist_ms (since the last cut, when
		// that is more recent): mode: compensate aims the LED at it, so a flash
		// never makes the LED chase it; a fall, or a cut, acts at once.
		double sustainedApl = 16.0;
		// The brightest 3x3 block (the script's max of its downscale): the
		// highlight test of mode: script.
		double maximum = 16.0;
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

	// Relative light output against the LPW1 level ([projector] flux_table).
	// Without a table the flux is proportional to the level.
	class FluxModel
	{
	public:
		FluxModel() = default;
		explicit FluxModel(const std::vector<std::pair<double, double>>& table);
		bool Linear() const { return table.empty(); }
		// Piecewise-linear between the table's points, clamped to its end values.
		double Flux(double level) const;
		// flux(ceiling) / flux(level): the linear gain that restores the picture.
		double Gain(int ceiling, double level) const;
		// The lowest whole level in [low, ceiling] giving at least `fraction` of
		// the ceiling's flux; the ceiling when none does.
		int LevelFor(double fraction, int low, int ceiling) const;

	private:
		std::vector<std::pair<double, double>> table;
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

	// A queued frame further ahead than the one a command is for: lets
	// mode: compensate start a ramp early (docs/VP-led-drive-delta-1.md 4).
	struct Lookahead
	{
		FrameStats stats;
		// A cut was seen between now and that frame.
		bool cutAhead = false;
		int framesAhead = 0;
	};

	// mode: compensate (docs/VP-led-drive-module.md 4.2-4.4, delta-1 3-4). The
	// projector's contrast stays at 50; the picture is compensated in linear
	// light so only the black floor moves. Runs once per output frame and
	// ramps the LED by at most rate_up / rate_down levels per second, holding
	// still within the deadband. Highlight policy: persistence gates
	// protection (via FrameStats::sustainedHighlight), area decides between
	// the knee and the global gain, everything continuous; one taste
	// parameter, highlight_tolerance_stops.
	class CompensateController
	{
	public:
		explicit CompensateController(const LedDriveConfig& config, const FluxModel& flux = FluxModel());
		void Reset(int light);
		// stats: the frame the command is for (shown when the LED gets there).
		// elapsedMs: time since the previous frame. ahead: the furthest queued
		// frame, when there is one (used only with early_ramp).
		ControllerOutput Tick(const FrameStats& stats, double elapsedMs, const Lookahead* ahead = nullptr);
		int Light() const { return light; }
		double Level() const { return level; }
		double Protection() const { return protection; }
		double Area() const { return area; }

	private:
		LedDriveConfig config;
		FluxModel flux;
		double level;
		int light;
		double protection = 0.0;
		double area = 0.0;
		double previousApl = -1.0;
		bool ramping = false;
	};

	// gain_led = flux(ceiling) / flux(LED level), in linear light.
	inline double LedGain(const FluxModel& flux, int ceiling, double light)
	{
		return flux.Gain(ceiling, light);
	}

	inline double LedGain(int ceiling, double light)
	{
		return LedGain(FluxModel(), ceiling, light);
	}

	// The final linear gain for a frame from the modelled LED level and the
	// controller's highlight limit.
	inline double CompensationGainFor(const FluxModel& flux, int ceiling, double light, const ControllerOutput& out)
	{
		const double ledGain = LedGain(flux, ceiling, light);
		const double capped = ledGain < out.highlightCap ? ledGain : out.highlightCap;
		return ledGain * (1.0 - out.capWeight) + capped * out.capWeight;
	}

	inline double CompensationGainFor(int ceiling, double light, const ControllerOutput& out)
	{
		return CompensationGainFor(FluxModel(), ceiling, light, out);
	}
}
