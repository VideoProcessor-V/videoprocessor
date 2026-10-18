#include <pch.h>
#include "LedController.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace LedDrive
{
	namespace
	{
		// beta_22.py's sleeps around its serial writes.
		constexpr int SETTLE_MS = 200;
		constexpr int FAST_STEP_MS = 100;
		constexpr int CONTRAST_UNITY = 50;
	}

	ScriptController::ScriptController(const LedDriveConfig& configIn)
		: config(configIn), light(configIn.ledCeiling), contrast(CONTRAST_UNITY)
	{
	}

	void ScriptController::Reset(int lightIn, int contrastIn)
	{
		light = lightIn;
		contrast = contrastIn;
	}

	// The script's power bands are piecewise linear in APL, truncated to an
	// integer; the configurable curve reproduces them (see the brief: the
	// script's rounded 0.8333 slope is one level lower at APL 76/82/88/94).
	int CurveLevel(const LedDriveConfig& config, int apl)
	{
		const auto& curve = config.curve;
		double level = curve.back().level;
		if (apl <= curve.front().apl)
			level = curve.front().level;
		else
			for (size_t i = 0; i + 1 < curve.size(); ++i)
				if (apl < curve[i + 1].apl)
				{
					// Multiply before dividing so integer breakpoints stay exact.
					level = curve[i].level + (apl - curve[i].apl) *
						(curve[i + 1].level - curve[i].level) / (curve[i + 1].apl - curve[i].apl);
					break;
				}
		return static_cast<int>(std::floor(level));
	}

	int ScriptLiteralLevel(int apl)
	{
		if (apl >= 100) return 255;
		if (apl >= 70) return static_cast<int>(((apl - 70) * 0.8333) + 230);
		if (apl >= 40) return static_cast<int>(((apl - 40) * 1) + 200);
		if (apl >= 20) return static_cast<int>(((apl - 20) * 5) + 100);
		return 100;
	}

	bool IsBuiltInCurve(const std::vector<CurvePoint>& curve)
	{
		const std::vector<CurvePoint> builtIn = LedDriveConfig().curve;
		return curve.size() == builtIn.size() && std::equal(curve.begin(), curve.end(), builtIn.begin(),
			[](const CurvePoint& a, const CurvePoint& b) { return a.apl == b.apl && a.level == b.level; });
	}

	// Step 7b: with the built-in curve, script mode uses the script's own
	// formulas (the owner's "stay as the script has them"); a custom curve is
	// interpolated as before.
	int ScriptController::LightTargetFor(int apl) const
	{
		int level = IsBuiltInCurve(config.curve) ? ScriptLiteralLevel(apl) : CurveLevel(config, apl);
		int lowest = config.ledFloor;
		if (floor == ScriptFloor::Reactive)
			lowest = std::max(config.ledFloor, config.ledFloorReactive);
		else if (floor == ScriptFloor::Covered && config.curve.size() >= 3 &&
			config.curve[0].level == config.curve[1].level && config.curve[2].level > config.curve[1].level &&
			config.curve[2].apl > config.curve[1].apl && apl < config.curve[1].apl)
		{
			// script_lookahead, covered (owner 2026-10-17): the flat bottom band
			// continues the first rising segment's slope down to led_floor.
			const CurvePoint& a = config.curve[1];
			const CurvePoint& b = config.curve[2];
			level = static_cast<int>(std::floor(a.level - static_cast<double>(a.apl - apl) *
				(b.level - a.level) / (b.apl - a.apl)));
		}
		int target = std::max(lowest, std::min(level, config.ledCeiling));
		if (lightHold >= 0) target = std::max(target, std::min(lightHold, config.ledCeiling));
		// Intent limits (2026-10-17): never below the intent floor.
		if (intentFloor > 0) target = std::max(target, std::min(intentFloor, config.ledCeiling));
		return target;
	}

	int ScriptTickBoundMs(const LedDriveConfig& config)
	{
		const int sends = 2 * SETTLE_MS + 6 * FAST_STEP_MS;
		return config.scriptPacing ? config.scriptMeasureMs + 6 * config.scriptEchoMs + sends :
			std::max(config.tickMs, sends);
	}

	bool ScriptBlackForPreposition(const LedDriveConfig& config, const FrameStats& stats)
	{
		return config.prepositionBlackOnly ? IsTrueBlack(stats) : stats.apl < 20.0;
	}

	ScriptLookaheadPlan PlanScriptLookahead(const LedDriveConfig& config, const ScriptController& controllerIn,
		const FrameStats& current, const ScriptLookaheadInput& in)
	{
		ScriptLookaheadPlan plan;
		plan.stats = current;
		plan.floor = ScriptFloor::Reactive;
		if (in.ahead.empty() || in.sourcePeriodMs <= 0.0) return plan;   // no queue: as before
		const double period = in.sourcePeriodMs;
		// The source frame on screen at a frame-clock time.
		auto frameAt = [&](double ms) -> uint64_t
		{
			const double frames = std::floor((ms - in.onScreenMs) / period + 1e-6);
			return frames <= 0.0 ? in.onScreenSequence : in.onScreenSequence + static_cast<uint64_t>(frames);
		};
		auto displayMs = [&](uint64_t sequence)
		{
			return in.onScreenMs + static_cast<double>(sequence - in.onScreenSequence) * period;
		};
		const double sendMs = in.nowMs + std::max(0.0, in.sendOffsetMs);
		const uint64_t landSequence = frameAt(sendMs + in.ledLatencyMs);
		// The frame by which a rise has settled (led_settle_ms after it lands).
		const uint64_t settledSequence = frameAt(sendMs + in.ledLatencyMs + std::max(0, in.ledSettleMs));
		const uint64_t furthest = in.ahead.back().first;
		plan.covered = furthest >= settledSequence;
		plan.floor = plan.covered ? ScriptFloor::Covered : ScriptFloor::Reactive;
		// The script's targets under this tick's floor rule.
		ScriptController controller = controllerIn;
		controller.SetFloor(plan.floor);
		controller.SetLightHold(-1);
		auto highlightOf = [&](const FrameStats& stats) { return stats.maximum >= config.scriptHighlightCode; };
		auto lightOf = [&](const FrameStats& stats)
		{
			return controller.LightTargetFor(std::max(0, static_cast<int>(stats.apl)));
		};
		// A cut's commands need no measuring time: the look-ahead already holds the
		// scene's statistics (the script's screenshot delay was a coding limitation).
		const double jumpSendMs = in.nowMs;

		// The scene's targets straight away, each landing on the scene's first
		// frame (cutMs) or as soon as the projector allows.
		auto cutJump = [&](uint64_t sequence, const FrameStats& stats, double cutMs, int lightTarget)
		{
			const double lightAt = std::max(jumpSendMs, cutMs - in.ledLatencyMs);
			plan.kind = ScriptLookaheadPlan::Kind::CutJump;
			plan.targetSequence = sequence;
			plan.cutMs = cutMs;
			plan.stats = stats;
			const int contrastTarget = ScriptController::ContrastTargetFor(
				std::max(0, static_cast<int>(stats.apl)), highlightOf(stats));
			plan.sends.push_back({ lightAt, true, lightTarget });
			if (contrastTarget != in.contrast)
			{
				double contrastAt = std::max(jumpSendMs, cutMs - in.cntrLatencyMs);
				// One command at a time: a CNTR coming down goes first (early
				// is below need, late would be above it); one going up
				// follows the LPW1.
				if (std::abs(contrastAt - lightAt) < in.commandGapMs)
					contrastAt = contrastTarget < in.contrast ? lightAt - in.commandGapMs : lightAt + in.commandGapMs;
				if (contrastAt < jumpSendMs)
				{
					// No room before the LPW1: CNTR at once, the LPW1 one gap
					// later (still no earlier than its landing time).
					contrastAt = jumpSendMs;
					plan.sends.front().atMs = std::max(lightAt, jumpSendMs + in.commandGapMs);
				}
				plan.sends.push_back({ contrastAt, false, contrastTarget });
			}
			std::stable_sort(plan.sends.begin(), plan.sends.end(),
				[](const ScriptTimedSend& a, const ScriptTimedSend& b) { return a.atMs < b.atMs; });
		};
		// A cut to a brighter scene since the last tick, already on screen (the
		// queue was too short, or the tick too long, to see it coming): its
		// targets at once rather than a step a tick.
		if (current.cut && !ScriptBlackForPreposition(config, current) && lightOf(current) > in.light)
		{
			cutJump(in.onScreenSequence, current, in.onScreenMs, lightOf(current));
			return plan;
		}

		// A cut to a brighter scene in the queue (a cut, or black to picture):
		// the script would only start climbing once it is on screen, one step a
		// tick. Its first frame gets the scene's targets, landing on it. A queue
		// shorter than the landing time still sees the cut early: its commands go
		// out at once and land as close to it as the projector allows.
		bool blackRun = ScriptBlackForPreposition(config, current);
		bool previousBlack = blackRun;
		uint64_t entrySequence = 0;   // the first frame of a brighter scene further away (0 none)
		for (const auto& frame : in.ahead)
		{
			const FrameStats& stats = frame.second;
			const bool black = ScriptBlackForPreposition(config, stats);
			const bool entering = stats.cut || (previousBlack && !black);
			const int lightTarget = lightOf(stats);
			if (entering && !black && lightTarget > in.light)
			{
				const double cutMs = displayMs(frame.first);
				// Sent now (waiting within this tick) when the next tick's first
				// command would land after the cut.
				if (std::max(jumpSendMs, cutMs - in.ledLatencyMs) - in.commandGapMs < in.nowMs + in.nextTickMs)
				{
					cutJump(frame.first, stats, cutMs, lightTarget);
					return plan;
				}
				// Further away: black until then (the picture on screen and every
				// queued frame before it), so the light may come up to the
				// pre-lift cap (black_prelift_max; 255 = no pre-positioning).
				if (blackRun && config.blackPreliftMax < 255)
				{
					plan.kind = ScriptLookaheadPlan::Kind::PreLift;
					plan.targetSequence = frame.first;
					plan.cutMs = cutMs;
					plan.stats = current;
					plan.lightHold = std::min(lightTarget, config.blackPreliftMax);
					if (plan.lightHold > in.light) plan.preliftTarget = plan.lightHold;
					return plan;
				}
				entrySequence = frame.first;
				break;
			}
			blackRun = blackRun && black;
			previousBlack = black;
		}

		// The script's tick on the frame its command lands on (or, with a queue
		// short of it, the furthest queued frame: the nearest to it there is).
		// A rise is taken from the frame where it will have settled (latency +
		// led_settle_ms), so the light has arrived by then; a fall lands on its
		// frame (never below the need before it). One frame each, not a window's
		// maximum, so frame-to-frame noise near a band edge biases nothing.
		// Neither crosses into a brighter scene further away: its cut lands on
		// its frame.
		uint64_t last = std::min(settledSequence, furthest);
		if (entrySequence != 0 && last >= entrySequence) last = entrySequence - 1;
		const uint64_t landing = std::min(landSequence, furthest);
		const FrameStats* chosen = nullptr;
		uint64_t chosenSequence = in.onScreenSequence;
		if (landing > in.onScreenSequence && (entrySequence == 0 || landing < entrySequence))
		{
			const auto at = std::find_if(in.ahead.begin(), in.ahead.end(),
				[&](const std::pair<uint64_t, FrameStats>& frame) { return frame.first >= landing; });
			if (at != in.ahead.end())
			{
				chosen = &at->second;
				chosenSequence = at->first;
			}
		}
		const int bestLight = lightOf(chosen ? *chosen : current);
		const std::pair<uint64_t, FrameStats>* settled = nullptr;
		for (const auto& frame : in.ahead)
			if (frame.first > std::max(landing, in.onScreenSequence) && frame.first <= last) settled = &frame;
		if (settled && lightOf(settled->second) > bestLight)
		{
			chosen = &settled->second;
			chosenSequence = settled->first;
		}
		// A command that lands on the frame on screen, no rise: that frame, as before.
		if (!chosen) return plan;
		plan.kind = ScriptLookaheadPlan::Kind::Ahead;
		plan.targetSequence = chosenSequence;
		plan.stats = *chosen;
		return plan;
	}

	ScriptSmoother::ScriptSmoother(double rampMsIn) : rampMs(std::max(1.0, rampMsIn))
	{
	}

	void ScriptSmoother::Reset(int contrast)
	{
		pending.clear();
		landed = contrast;
		smooth = contrast;
		riseFrom = contrast;
		lastMs = -1.0;
		lastLandMs = riseStartMs = -1e300;
	}

	void ScriptSmoother::Plan(const std::vector<std::pair<double, int>>& landings)
	{
		for (const auto& landing : landings)
			pending.push_back({ landing.first, landing.second, false });
	}

	void ScriptSmoother::Sent(double landMs)
	{
		for (Landing& landing : pending)
			if (!landing.sent)
			{
				landing.sent = true;
				landing.timeMs = landMs;
				break;
			}
		// A late send can land after the next one's planned time: keep order.
		for (size_t i = 1; i < pending.size(); ++i)
			pending[i].timeMs = std::max(pending[i].timeMs, pending[i - 1].timeMs);
	}

	double ScriptSmoother::Gain(double timeMs)
	{
		const double previous = lastMs < 0.0 ? timeMs : std::min(lastMs, timeMs);
		lastMs = std::max(lastMs, timeMs);
		// Commands that have landed: the projector is at their value now.
		while (!pending.empty() && pending.front().timeMs <= timeMs)
		{
			const Landing landing = pending.front();
			pending.erase(pending.begin());
			if (landing.value > landed)
			{
				// An increase: the picture rises to it from where it is, over
				// rampMs from the moment it landed.
				riseFrom = std::min(smooth, static_cast<double>(landed));
				riseStartMs = landing.timeMs;
			}
			landed = landing.value;
			lastLandMs = landing.timeMs;
		}
		smooth = std::min(smooth, static_cast<double>(landed));
		const Landing* next = pending.empty() ? nullptr : &pending.front();
		if (next && next->value < smooth)
		{
			// A decrease is coming: the picture goes down to it linearly so it
			// arrives as the projector does, starting no earlier than rampMs
			// before it and no earlier than the last landing.
			const double start = std::max(next->timeMs - rampMs, lastLandMs);
			const double from = std::max(previous, start);
			if (timeMs > from && next->timeMs > from)
				smooth += (next->value - smooth) * std::min(1.0, (timeMs - from) / (next->timeMs - from));
		}
		else if (smooth < landed)
		{
			// Rise towards the projector's value, but no higher than a
			// decrease about to land (so it never has to drop at once).
			double cap = landed;
			if (next && next->timeMs - timeMs <= rampMs) cap = std::min(cap, static_cast<double>(next->value));
			const double rise = riseFrom + (landed - riseFrom) * std::min(1.0, (timeMs - riseStartMs) / rampMs);
			smooth = std::max(smooth, std::min(cap, rise));
		}
		return landed > 0 ? std::min(1.0, smooth / landed) : 1.0;
	}

	// The script's contrast bands, unchanged (50 is unity).
	int ScriptController::ContrastTargetFor(int apl, bool highlight)
	{
		if (apl >= 60) return 50;
		if (apl >= 45) return highlight ? 50 : 51;
		if (apl >= 30) return static_cast<int>(std::abs(apl - 45) * 0.7333 + 51);
		if (apl >= 20) return static_cast<int>(std::abs(apl - 30) * 1.2 + 62);
		return 74;
	}

	int PaceScriptTick(const ControllerOutput& out, int measureMs, int echoMs, std::vector<ScriptSend>& sends)
	{
		// The script's loop: the screenshot, then LPW1 (an echo to the port and
		// one to its console) and its sleep, then each CNTR after its sleep
		// (the schedule's offsets are those sleeps), each echo taking its time.
		sends.clear();
		int t = measureMs;
		if (out.lightCommand >= 0)
		{
			sends.push_back({ t, true, out.lightCommand });
			t += 2 * echoMs;
		}
		int echoes = 0;
		for (const ContrastStep& step : out.contrastSchedule)
		{
			sends.push_back({ t + step.offsetMs + echoes * echoMs, false, step.contrast });
			++echoes;
		}
		return t + out.durationMs + echoes * echoMs;
	}

	ControllerOutput ScriptController::Tick(const FrameStats& stats)
	{
		ControllerOutput out;
		// The script truncates the mean (mean2 = int(mean)).
		const int apl = std::max(0, static_cast<int>(stats.apl));
		// The script tested the brightest pixel of its 854x480 downscale; the
		// maximum is the same measure (led_peak_full_coverage: the largest area
		// mean over boxes of that share of the screen, picture only; off: the
		// brightest 3x3 sample block).
		const bool highlight = stats.maximum >= config.scriptHighlightCode;
		out.lightTarget = LightTargetFor(apl);
		out.contrastTarget = ContrastTargetFor(apl, highlight);
		int elapsed = 0;

		// Light: one step per tick from the tables; brightening is faster in
		// dark scenes (APL below the split), dimming uses one table.
		if (light != out.lightTarget)
		{
			const int distance = std::abs(out.lightTarget - light);
			if (out.lightTarget > light)
				light += (apl < config.stepsUpSplitApl ? config.stepsUpDark : config.stepsUpBright)
					.StepFor(distance);
			else
				light -= config.stepsDown.StepFor(distance);
			out.lightCommand = light;
			elapsed += SETTLE_MS;
		}

		// Contrast: the script's dynamics, including both fast dumps and the
		// fall-through from the 7-9 dump into the anti-pump band.
		if (contrast != out.contrastTarget)
		{
			const int target = out.contrastTarget;
			int distance = std::abs(target - contrast);
			auto fastStep = [&]
			{
				--contrast;
				elapsed += FAST_STEP_MS;
				out.contrastSchedule.push_back({ elapsed, contrast });
				distance = std::abs(target - contrast);
			};
			if (distance >= 10)
			{
				if (target > contrast)
					++contrast;
				else if (highlight)
				{
					// Fast dump with a highlight on screen: down to 10 above
					// target at 100 ms per step, then one more step.
					while (distance > 10) fastStep();
					--contrast;
				}
				else
					--contrast;
				// The script does not recompute the distance here.
			}
			if (distance >= 7 && distance < 10)
			{
				if (target > contrast)
					++contrast;
				else
					// Second fast dump, highlight or not: down to 6 above target.
					while (distance >= 7 && distance < 10) fastStep();
			}
			if (distance >= 1 && distance < 7)
			{
				const bool up = target > contrast;
				const bool down = target < contrast;
				// Anti-pump: within the band, a highlight blocks increases and
				// its absence blocks decreases.
				const bool inBand = distance <= config.antiPumpBand;
				if (up && !(inBand && highlight)) ++contrast;
				if (down && !(inBand && !highlight)) --contrast;
			}
			elapsed += SETTLE_MS;
			out.contrastSchedule.push_back({ elapsed, contrast });
		}

		out.light = light;
		out.contrast = contrast;
		out.gain = static_cast<double>(contrast) / CONTRAST_UNITY;
		out.gainDomain = GainDomain::Encoded;
		out.knee = config.knee;
		out.durationMs = elapsed;
		std::ostringstream log;
		log << "LED script: apl=" << apl << " max=" << static_cast<int>(stats.maximum)
			<< (highlight ? "*" : "") << " light " << light << "/" << out.lightTarget
			<< (out.lightCommand >= 0 ? " sent" : "") << " contrast " << contrast << "/"
			<< out.contrastTarget << " gain=" << out.gain;
		out.log = log.str();
		return out;
	}

	FluxModel::FluxModel(const std::vector<std::pair<double, double>>& tableIn) : table(tableIn)
	{
	}

	double FluxModel::Flux(double value) const
	{
		if (table.empty()) return value;
		if (value <= table.front().first) return table.front().second;
		if (value >= table.back().first) return table.back().second;
		for (size_t i = 1; i < table.size(); ++i)
			if (value <= table[i].first)
			{
				const auto& a = table[i - 1];
				const auto& b = table[i];
				return a.second + (b.second - a.second) * (value - a.first) / (b.first - a.first);
			}
		return table.back().second;
	}

	double FluxModel::Gain(int ceiling, double value) const
	{
		const double actual = Flux(value);
		return actual > 0.0 ? Flux(ceiling) / actual : 1.0;
	}

	double FluxModel::LevelOf(double fluxValue) const
	{
		if (table.empty()) return fluxValue;
		if (fluxValue <= table.front().second) return table.front().first;
		if (fluxValue >= table.back().second) return table.back().first;
		for (size_t i = 1; i < table.size(); ++i)
			if (fluxValue <= table[i].second)
			{
				const auto& a = table[i - 1];
				const auto& b = table[i];
				return b.second > a.second ?
					a.first + (b.first - a.first) * (fluxValue - a.second) / (b.second - a.second) : b.first;
			}
		return table.back().first;
	}

	int FluxModel::LevelFor(double fraction, int low, int ceiling) const
	{
		const double needed = fraction * Flux(ceiling);
		for (int candidate = std::max(0, low); candidate < ceiling; ++candidate)
			if (Flux(candidate) >= needed - 1e-12) return candidate;
		return ceiling;
	}

	CompensateController::CompensateController(const LedDriveConfig& configIn, const FluxModel& fluxIn)
		: config(configIn), flux(fluxIn), level(configIn.ledCeiling), light(configIn.ledCeiling)
	{
	}

	void CompensateController::Reconfigure(const LedDriveConfig& configIn, const FluxModel& fluxIn)
	{
		config = configIn;
		flux = fluxIn;
	}

	void CompensateController::Reset(int lightIn)
	{
		light = lightIn;
		level = lightIn;
		protection = 0.0;
		area = 0.0;
		previousApl = -1.0;
		ramping = false;
		cutLeadSent = false;
	}

	ControllerOutput CompensateController::Tick(const FrameStats& stats, double elapsedMs, const Lookahead* ahead,
		bool predicted, bool covered)
	{
		// Without look-ahead covering the LED's response the floor is higher.
		// 7c-2: the scene floor is also kept where a full dump would start a
		// fade-up no darker than max_cut_dip: f x 2^tolerance (white
		// highlights) >= max_cut_dip (planner's 7c-2 answer 3).
		const int dipFloor = config.maxCutDip > 0.0 ? flux.LevelFor(
			config.maxCutDip / std::pow(2.0, config.highlightToleranceStops), 0, config.ledCeiling) : 0;
		const int floor = std::max(dipFloor,
			covered ? config.ledFloor : std::max(config.ledFloor, config.ledFloorReactive));
		ControllerOutput out;
		const int ceiling = config.ledCeiling;
		const double eotf = config.compensationEotf;
		const double knee = config.knee;
		// The LED target follows the sustained APL: never chase a flash.
		const int apl = std::max(0, static_cast<int>(stats.sustainedApl));
		const double elapsed = std::max(1.0, elapsedMs);
		auto linearOf = [eotf](double code)
		{
			return std::pow(std::max(0.0, std::min((code - 16.0) / 219.0, 1.0)), eotf);
		};

		// Sustained highlight in linear light at full LED; a flash shorter than
		// highlight_persist_ms never reaches it.
		const double highlightLinear = linearOf(stats.sustainedHighlight);
		// The LED's share of full light, by flux (flux_table), not by level.
		const double fraction = flux.Flux(level) / flux.Flux(ceiling);
		// Would it reach the knee once compensated for the current LED level?
		const bool exceeds = highlightLinear / fraction > 1.0 - knee;
		// Engage within ~100 ms of a sustained highlight, relax over
		// highlight_release_ms: continuous, never a threshold to toggle.
		protection = exceeds ? std::min(1.0, protection + elapsed / 100.0) :
			std::max(0.0, protection - elapsed / std::max(1, config.highlightReleaseMs));

		// Area: blocks that would reach the knee after compensation.
		const double thresholdCode = 16.0 + 219.0 * std::pow(
			std::max(0.0, std::min((1.0 - knee) * fraction, 1.0)), 1.0 / eotf);
		uint32_t bright = 0;
		for (int code = std::max(0, std::min(255, static_cast<int>(std::ceil(thresholdCode)))); code < 256; ++code)
			bright += stats.histogram[code];
		const double areaNow = stats.blocks > 0 ? static_cast<double>(bright) / stats.blocks : 0.0;
		area = std::max(areaNow, area * std::exp(-elapsed / std::max(1, config.highlightReleaseMs)));
		const double span = std::max(1e-9, config.highlightAreaLarge - config.highlightAreaSmall);
		double large = std::max(0.0, std::min((area - config.highlightAreaSmall) / span, 1.0));
		large = large * large * (3.0 - 2.0 * large); // smoothstep

		// Highlight floor: the lowest LED that keeps compression within the
		// tolerance (small highlights, knee) or avoids it (large, global gain).
		// Tiny highlights cost nothing: the 99.5th-percentile statistic does not
		// see anything under 0.5% of the picture, so a small lamp in a dark
		// scene never raises the floor.
		const double allowedStops = config.highlightToleranceStops * (1.0 - large);
		const double highlightFloor = protection *
			flux.LevelFor(highlightLinear / std::pow(2.0, allowedStops) / contrastHeadroom, 0, ceiling);

		const bool cutToBlack = stats.cut && apl < 20;
		const bool toBlack = stats.fade || cutToBlack;
		const int base = toBlack ? config.ledFloorBlack :
			std::max(CurveLevel(config, apl), floor);
		out.lightTarget = std::max(config.ledFloorBlack, std::min(ceiling,
			std::max(base, static_cast<int>(highlightFloor))));
		// Intent limits (2026-10-17): never below the intent floor (black too:
		// dimming below the room's veil cannot be seen).
		if (intentFloor > 0) out.lightTarget = std::max(out.lightTarget, std::min(ceiling, intentFloor));

		// Early ramp: a cut or a large APL change in the queue. Brightening
		// ahead of it is free (headroom only grows); dimming ahead of it stops
		// where the current scene's highlights would pass the tolerance.
		int target = out.lightTarget;
		bool early = false;
		bool preposition = false;
		if (ahead && config.earlyRamp &&
			(ahead->cutAhead || std::abs(ahead->stats.apl - stats.apl) >= config.cutThreshold))
		{
			const int aheadApl = std::max(0, static_cast<int>(ahead->stats.apl));
			const int aheadTarget = std::max(config.ledFloorBlack, std::min(ceiling, aheadApl < 20 ?
				config.ledFloorBlack : std::max(CurveLevel(config, aheadApl), floor)));
			if (aheadTarget > target)
			{
				// 7c-3 (policy 3.5; the owner's black-frames note and correction):
				// on black or a very dark picture the LED goes at once to at most
				// black_prelift_max, never further while the picture is still
				// dark; the full jump comes with the cut. 255 = before 7c-3.
				// Not during a fade's gain hold unless the picture is truly
				// black: the held gain would not compensate the LED's rise
				// (audit stage7-step7c, Should fix 2).
				// preposition_black_only (review 2026-10-17, default on): only
				// while truly black, so a visible fade-in is never carried at
				// cut_cntr_level and released in one step; off = APL < 20.
				if (config.blackPreliftMax < 255 && !stats.cut && stats.apl < 20.0 &&
					(!stats.fade || IsTrueBlack(stats)) &&
					(!config.prepositionBlackOnly || IsTrueBlack(stats)))
				{
					preposition = true;
					target = std::max(target, std::min(aheadTarget,
						std::max(config.blackPreliftMax, static_cast<int>(std::lround(level)))));
				}
				else
					target = aheadTarget;
				early = true;
			}
			else if (aheadTarget < target)
			{
				const int currentLimit = std::max(config.ledFloorBlack, flux.LevelFor(
					linearOf(stats.highlight) / std::pow(2.0, config.highlightToleranceStops) / contrastHeadroom,
					0, ceiling));
				const int bounded = std::max(aheadTarget, currentLimit);
				if (bounded < target)
				{
					target = bounded;
					early = true;
				}
			}
		}

		// Rate law (7c-1): proportional to the distance, between rate_min and
		// rate_max (rate_gain 0 = the fixed rate_up / rate_down).
		const double distance = std::abs(target - level);
		// rate_gain 0 is the regression recipe's fixed rate_up / rate_down,
		// whatever rate_max says.
		const double maxUp = (config.rateGainUp > 0.0 ? RampRate(distance, config.rateGainUp, config.rateMinUp,
			config.rateMaxUp > 0.0 ? config.rateMaxUp : config.rateUp) : config.rateUp) * elapsed / 1000.0;
		const double maxDown = (config.rateGainDown > 0.0 ? RampRate(distance, config.rateGainDown,
			config.rateMinDown, config.rateMaxDown > 0.0 ? config.rateMaxDown : config.rateDown) :
			config.rateDown) * elapsed / 1000.0;
		// cut_lead: window (review 2026-10-17, experimental): a bright cut in
		// the window beyond the command's frame - its jump now, then hold
		// until the cut reaches the command's frame (the seen-cut jump below
		// then has nothing left to do). The furthest frame stands for the new
		// scene. On a dark picture the jump keeps black_prelift_max's cap.
		bool leadHold = false;
		if (!config.cutLeadWindow || !predicted || !ahead || !ahead->cutAhead || stats.cut)
			cutLeadSent = false;
		else if (!preposition)
		{
			const int aheadApl = std::max(0, static_cast<int>(ahead->stats.apl));
			int leadTarget = std::max(config.ledFloorBlack, std::min(ceiling, aheadApl < 20 ?
				config.ledFloorBlack : std::max(CurveLevel(config, aheadApl), floor)));
			if (config.blackPreliftMax < 255 && stats.apl < 20.0)
				leadTarget = std::min(leadTarget, std::max(config.blackPreliftMax, static_cast<int>(std::lround(level))));
			leadHold = cutLeadSent || leadTarget > level + 0.5;
			if (leadHold && !cutLeadSent)
			{
				target = leadTarget;
				early = true;
			}
		}
		// Intent limits (2026-10-17): the early ramp, the pre-lift and the cut
		// lead never take the target below the intent floor either.
		if (intentFloor > 0) target = std::max(target, std::min(ceiling, intentFloor));
		// A seen cut lands now: jump to the new scene (the cut hides it). An
		// unseen dark-to-bright cut: straight up, like the script's dump, so
		// highlights are squashed only for the LED's response time.
		const bool seenCut = predicted && stats.cut && config.cutJump;
		const bool brightDump = !predicted && stats.cut && config.brightCutDump && out.lightTarget > level;
		// 7c-2: a cut to a brighter or darker target (policy 3.3 / 3.4).
		out.preposition = preposition;
		out.brightCut = stats.cut && out.lightTarget > level + 0.5;
		out.darkCut = stats.cut && out.lightTarget < level - 0.5;
		if (out.brightCut)
		{
			out.cutHighlight = linearOf(stats.highlight);
			out.cutCeiling = out.cutHighlight > 0.0 ?
				std::max(1.0, std::pow(2.0, config.highlightToleranceStops) / out.cutHighlight) : 1e9;
		}
		if (seenCut || brightDump)
		{
			level = seenCut ? out.lightTarget : std::max(static_cast<double>(target), static_cast<double>(out.lightTarget));
			ramping = false;
			out.jump = true;
		}
		else if (leadHold)
		{
			// One command as the cut enters the window, then nothing more
			// until it arrives.
			if (!cutLeadSent)
			{
				level = target;
				out.jump = true;
				cutLeadSent = true;
			}
			ramping = false;
			out.cutLeadHold = true;
		}
		else if (preposition)
		{
			// One command, no rate law on black (only the LED's own settle).
			if (target > level)
			{
				level = target;
				out.jump = true;
			}
			ramping = false;
		}
		else if (stats.fade && level > target)
		{
			// Follow the fade at its own rate (its APL ratio above black), never
			// faster than rate_down.
			const double ratio = previousApl > 16.0 ?
				std::max(0.0, std::min((stats.apl - 16.0) / (previousApl - 16.0), 1.0)) : 1.0;
			level = std::max(static_cast<double>(target), level - std::min(maxDown, level * (1.0 - ratio)));
		}
		else
		{
			// The deadband only keeps a resting LED still; a ramp that has
			// started runs on to its target.
			if (std::abs(target - level) > config.deadband) ramping = true;
			if (ramping)
			{
				if (target > level) level = std::min(static_cast<double>(target), level + maxUp);
				else level = std::max(static_cast<double>(target), level - maxDown);
				ramping = level != target;
			}
		}
		const int commanded = static_cast<int>(std::lround(level));
		if (commanded != light) out.lightCommand = commanded;
		light = commanded;
		previousApl = stats.apl;

		out.light = light;
		out.gainDomain = GainDomain::Linear;
		out.knee = knee;
		out.highlightCap = highlightLinear > 0.0 ? 1.0 / highlightLinear : 1e9;
		out.capWeight = protection * large;
		// Software first: VP's gain may lift the sustained highlight by up to
		// the allowed stops into the knee; beyond that, CNTR (7c-1).
		out.vpHeadroom = protection > 0.0 && highlightLinear > 0.0 ?
			std::pow(2.0, allowedStops) / (highlightLinear * protection) : 1e9;
		out.holdGain = stats.fade;
		out.gain = CompensationGainFor(flux, ceiling, light, out);
		std::ostringstream log;
		log << "LED compensate: apl=" << apl << " sustained_highlight=" << static_cast<int>(stats.sustainedHighlight)
			<< " light " << light << "/" << out.lightTarget << (early ? " early:" + std::to_string(target) : "")
			<< (out.lightCommand >= 0 ? " sent" : "")
			<< " protection=" << protection << " area=" << area << " gain=" << out.gain
			<< (stats.cut ? " cut" : "") << (stats.fade ? " fade" : "")
			<< (seenCut ? " cut-jump" : brightDump ? " bright-dump" : "") << (out.cutLeadHold ? " cut-lead" : "")
			<< (covered ? "" : " reactive");
		out.log = log.str();
		return out;
	}

	void CntrPairing::Configure(double eotfIn, int maxUsableIn, bool enabledIn, bool softwareFirstIn)
	{
		eotf = eotfIn;
		maxUsable = std::max(50, maxUsableIn);
		enabled = enabledIn;
		softwareFirst = softwareFirstIn;
	}

	void CntrPairing::Reset(int contrastIn)
	{
		contrast = contrastIn;
		required = 1.0;
	}

	double CntrPairing::Light(int value) const
	{
		return std::pow(value / 50.0, eotf);
	}

	int CntrPairing::DumpTarget(double ledFraction, double gainCeiling, double maxCutDip, int cutLevel) const
	{
		int value = std::max(50, cutLevel);
		if (maxCutDip > 0.0)
			while (value < contrast && ledFraction * Light(value) * gainCeiling < maxCutDip) ++value;
		return std::min(value, contrast);
	}

	void CutRecovery::Configure(const LedDriveConfig& configIn, const FluxModel& fluxIn)
	{
		config = configIn;
		flux = fluxIn;
	}

	void CutRecovery::Start(double ledFraction, double contrastLight, double gainCeilingIn)
	{
		gainCeiling = std::max(1.0, gainCeilingIn);
		const double start = std::max(0.0, std::min(1.0, ledFraction * contrastLight * gainCeiling));
		rampLevel = flux.LevelOf(start * flux.Flux(config.ledCeiling));
		active = start < 1.0;
	}

	double CutRecovery::Displayed() const
	{
		const double full = flux.Flux(config.ledCeiling);
		return full > 0.0 ? std::min(1.0, flux.Flux(rampLevel) / full) : 1.0;
	}

	double CutRecovery::Step(double elapsedMs, double ledFraction, double contrastLight)
	{
		if (!active) return 1e9;
		const double ceiling = config.ledCeiling;
		const double distance = ceiling - rampLevel;
		// The fast recovery rates (owner correction): the paired LED / CNTR
		// keep the script-matched rates; this ramp only covers the shortfall.
		const double rate = RampRate(distance, config.recoveryRateGain, config.recoveryRateMin,
			config.recoveryRateMax);
		rampLevel = std::min(ceiling, rampLevel + rate * std::max(0.0, elapsedMs) / 1000.0);
		const double light = std::max(1e-6, ledFraction * contrastLight);
		const double displayed = Displayed();
		// Done once the ramp is at full and full compensation fits under the
		// ceiling: the limit then equals the normal gain, so nothing jumps.
		if (displayed >= 1.0 - 1e-9 && 1.0 / light <= gainCeiling + 1e-9)
		{
			active = false;
			return 1e9;
		}
		return std::min(gainCeiling, displayed / light);
	}

	int CntrPairing::Update(double compensation, double vpHeadroom, int limit)
	{
		const double software = softwareFirst ? std::max(1.0, vpHeadroom) : 1.0;
		// The light CNTR is needed for; below 1 when VP's gain has room to
		// spare (0 with the assist off), so CNTR can always come back to 50.
		const double need = enabled ? compensation / software * (1.0 + bias) : 0.0;
		required = std::max(1.0, need);
		// The smallest whole CNTR that supplies it (CNTR never below 50 here).
		int wanted = 50;
		while (wanted < maxUsable && Light(wanted) < required - 1e-12) ++wanted;
		// Down at once to the smallest value that covers the need: CNTR is
		// never left above what the pairing needs (audit stage7-step7c1: a
		// stuck 51 is visible on near-black). The anti-chatter margin is on
		// the way up instead: rise only once the need passes the current
		// value's light by 2% (VP's gain covers that 2% meanwhile).
		if (ceiling >= 50) wanted = std::min(wanted, ceiling);
		// Post-merge audit 2026-10-17: the limit (cut_lead window, DMD clip) caps the
		// pre-position hold too; before, the hold was applied last and overrode it.
		const int effectiveHold = hold >= 50 && limit >= 50 ? std::min(hold, limit) : hold;
		if (effectiveHold >= 50) wanted = std::max(wanted, effectiveHold);
		if (limit >= 50) wanted = std::min(wanted, limit);
		int next = contrast;
		if (wanted < contrast)
			next = wanted;
		else if (wanted > contrast && (need > Light(contrast) * 1.02 || wanted <= effectiveHold))
			next = wanted;
		if (next == contrast) return -1;
		contrast = next;
		return contrast;
	}
}
