#pragma once

#include "LedDriveConfig.h"

#include <array>
#include <cmath>
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

	// One CNTR command of mode: script, offsetMs after the tick started in
	// the script's own timing (the script's fast dumps step every 100 ms).
	// Step 7b: sent as a real CNTR; each offset is a sleep counted from the
	// end of the previous send, as the script's time.sleep did.
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
		// contrast / 50: what the projector's CNTR does to the encoded signal
		// (step 7b sends CNTR itself; VP's gain only bridges its steps when
		// script_smoothing is on, see ScriptSmoother).
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
		// mode: compensate, 7c-1: the largest VP gain (linear) the scene's
		// sustained highlights allow within highlight_tolerance_stops; CNTR
		// assist supplies only what is needed beyond it (software first).
		double vpHeadroom = 1e9;
		bool holdGain = false;
		// This command is a cut jump or a bright-cut dump: send it whole
		// (not split by max_levels_per_command).
		bool jump = false;
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
		// The (fractional) level whose flux is fluxValue: the inverse of Flux().
		double LevelOf(double fluxValue) const;

	private:
		std::vector<std::pair<double, double>> table;
	};

	// beta_22.py ported parameter for parameter. Light: the configured curve
	// and step tables. Contrast: the script's bands and dynamics, emulated in
	// VP (never sent to the projector).
	// beta_22.py's power formulas, literally, including its rounded 0.8333
	// slope (one level lower than the curve at APL 76, 82, 88 and 94). Not
	// floored or limited.
	int ScriptLiteralLevel(int apl);
	// The curve is the built-in default (the script's breakpoints).
	bool IsBuiltInCurve(const std::vector<CurvePoint>& curve);

	// mode: script with script_smoothing: on (step 7b). The projector's CNTR
	// moves the picture in whole steps of 1/50; VP's gain bridges each step
	// so the picture changes continuously, without ever being brighter than
	// the projector's own value (gain <= 1): a decrease is ramped down
	// before it lands, an increase after it lands. One clock in ms
	// throughout; a command lands cntr_latency_ms after it was sent.
	class ScriptSmoother
	{
	public:
		explicit ScriptSmoother(double rampMs = 200.0);
		// The projector's CNTR and the picture both at this value, nothing planned.
		void Reset(int contrast = 50);
		// One script tick's CNTR commands at their planned landing times, in
		// order (added after anything still pending).
		void Plan(const std::vector<std::pair<double, int>>& landings);
		// The oldest planned command that had not gone out went out: it lands
		// at landMs (replacing its planned time).
		void Sent(double landMs);
		// VP's gain for a frame shown at timeMs (non-decreasing times).
		double Gain(double timeMs);
		int Landed() const { return landed; }
		double Smooth() const { return smooth; }
		size_t Pending() const { return pending.size(); }

	private:
		struct Landing
		{
			double timeMs;
			int value;
			bool sent;
		};
		std::vector<Landing> pending;
		double rampMs;
		int landed = 50;
		double smooth = 50.0;
		double lastMs = -1.0;
		double lastLandMs = -1e300;
		double riseFrom = 50.0;
		double riseStartMs = -1e300;
	};

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
		// New settings, same state (LED level, protection, ramp): live tuning.
		void Reconfigure(const LedDriveConfig& config, const FluxModel& flux);
		// stats: the frame the command is for (shown when the LED gets there).
		// elapsedMs: time since the previous frame. ahead: the furthest queued
		// frame, when there is one (used only with early_ramp). predicted:
		// stats come from the look-ahead (the frame the LED will land on);
		// covered: the look-ahead reaches that far (led_floor applies, else
		// led_floor_reactive too).
		ControllerOutput Tick(const FrameStats& stats, double elapsedMs, const Lookahead* ahead = nullptr,
			bool predicted = false, bool covered = false);
		int Light() const { return light; }
		double Level() const { return level; }
		double Protection() const { return protection; }
		double Area() const { return area; }
		// 7c-1: the extra light CNTR assist can add at most
		// ((cntr_max_usable / 50)^E; 1 = no assist). The highlight floor lets
		// the LED go that much lower, CNTR making up the difference.
		void SetContrastHeadroom(double light) { contrastHeadroom = light > 1.0 ? light : 1.0; }
		double ContrastHeadroom() const { return contrastHeadroom; }

	private:
		LedDriveConfig config;
		FluxModel flux;
		double contrastHeadroom = 1.0;
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

	// VP's final linear gain for a frame: the compensation the LED needs
	// (ledGain = ceiling flux / modelled LED flux), less what the projector's
	// CNTR already adds (cntrLight = (CNTR / 50)^E, 7c-1), limited by the
	// controller's highlight cap.
	inline double CompensationGainForLed(double ledGain, const ControllerOutput& out, double cntrLight = 1.0)
	{
		const double vp = cntrLight > 0.0 ? ledGain / cntrLight : ledGain;
		const double capped = vp < out.highlightCap ? vp : out.highlightCap;
		return vp * (1.0 - out.capWeight) + capped * out.capWeight;
	}

	// The final linear gain for a frame from the modelled LED level and the
	// controller's highlight limit.
	inline double CompensationGainFor(const FluxModel& flux, int ceiling, double light, const ControllerOutput& out,
		double cntrLight = 1.0)
	{
		return CompensationGainForLed(LedGain(flux, ceiling, light), out, cntrLight);
	}

	inline double CompensationGainFor(int ceiling, double light, const ControllerOutput& out)
	{
		return CompensationGainFor(FluxModel(), ceiling, light, out);
	}

	// Rate law (control policy 3.2): clamp(rateGain x |distance|, rateMin,
	// rateMax) per second; rateGain <= 0 = the fixed rateMax.
	inline double RampRate(double distance, double rateGain, double rateMin, double rateMax)
	{
		if (rateGain <= 0.0) return rateMax;
		const double rate = rateGain * (distance < 0.0 ? -distance : distance);
		return rate < rateMin ? rateMin : rate > rateMax ? rateMax : rate;
	}

	// An actuator's response after its dead time (control policy 6): the
	// value approaches the last landed command as a first-order lag with
	// time constant settleMs (0 = at once).
	class SettleModel
	{
	public:
		explicit SettleModel(double settleMsIn = 0.0, double valueIn = 1.0) : settleMs(settleMsIn), value(valueIn), target(valueIn) {}
		void SetSettle(double ms) { settleMs = ms > 0.0 ? ms : 0.0; if (settleMs == 0.0) value = target; }
		void Reset(double v) { value = target = v; }
		// A command lands (its dead time has passed).
		void Land(double v) { target = v; if (settleMs == 0.0) value = v; }
		double Step(double elapsedMs)
		{
			if (settleMs > 0.0 && elapsedMs > 0.0)
				value += (target - value) * (1.0 - std::exp(-elapsedMs / settleMs));
			return value;
		}
		// Where it will be aheadMs from now heading for `toward`.
		double Predict(double toward, double aheadMs) const
		{
			return settleMs > 0.0 ? toward + (value - toward) * std::exp(-aheadMs / settleMs) : toward;
		}
		double Value() const { return value; }
		double Target() const { return target; }

	private:
		double settleMs;
		double value;
		double target;
	};

	// mode: compensate's CNTR assist (control policy 2, 3.1, 3.2; 7c-1). CNTR
	// supplies only the light VP's gain cannot add without compressing
	// highlights beyond the tolerance (software first), as a whole CNTR
	// value; VP's gain removes the rest of the integer step. A command is
	// due only when the paired integer changes (planner's step 7 answer 1):
	// down at once to the smallest value that covers the need (never left
	// above it), up only once the need passes the current value by 2%, so a
	// need hovering at a step boundary never makes CNTR chatter.
	class CntrPairing
	{
	public:
		void Configure(double eotf, int maxUsable, bool enabled, bool softwareFirst);
		void Reset(int contrast = 50);
		// compensation: the light factor the LED needs (ceiling flux /
		// modelled LED flux where the CNTR command will land); vpHeadroom:
		// the largest VP gain the highlights allow. Returns the CNTR to send,
		// or -1 when it stays.
		int Update(double compensation, double vpHeadroom);
		int Contrast() const { return contrast; }
		// The light factor CNTR was asked to supply at the last update.
		double Required() const { return required; }
		double Light(int value) const;

	private:
		double eotf = 2.41;
		int maxUsable = 74;
		bool enabled = true;
		bool softwareFirst = true;
		int contrast = 50;
		double required = 1.0;
	};
}
