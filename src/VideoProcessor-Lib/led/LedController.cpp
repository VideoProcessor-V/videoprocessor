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

	int ScriptController::LightTargetFor(int apl) const
	{
		return std::max(config.ledFloor, std::min(CurveLevel(config, apl), config.ledCeiling));
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

	ControllerOutput ScriptController::Tick(const FrameStats& stats)
	{
		ControllerOutput out;
		// The script truncates the mean (mean2 = int(mean)).
		const int apl = std::max(0, static_cast<int>(stats.apl));
		const bool highlight = stats.highlight >= config.scriptHighlightCode;
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
		log << "LED script: apl=" << apl << " highlight=" << static_cast<int>(stats.highlight)
			<< (highlight ? "*" : "") << " light " << light << "/" << out.lightTarget
			<< (out.lightCommand >= 0 ? " sent" : "") << " contrast " << contrast << "/"
			<< out.contrastTarget << " gain=" << out.gain;
		out.log = log.str();
		return out;
	}

	CompensateController::CompensateController(const LedDriveConfig& configIn)
		: config(configIn), light(configIn.ledCeiling)
	{
	}

	void CompensateController::Reset(int lightIn)
	{
		light = lightIn;
		protection = 0.0;
		area = 0.0;
		previousApl = -1.0;
	}

	ControllerOutput CompensateController::Tick(const FrameStats& stats, int elapsedMs)
	{
		ControllerOutput out;
		const int ceiling = config.ledCeiling;
		const double eotf = config.compensationEotf;
		const double knee = config.knee;
		// The LED target follows the sustained APL: never chase a flash.
		const int apl = std::max(0, static_cast<int>(stats.sustainedApl));
		const double elapsed = std::max(1, elapsedMs);

		// Sustained highlight in linear light at full LED; a flash shorter than
		// highlight_persist_ms never reaches it.
		const double highlightLinear = std::pow(
			std::max(0.0, std::min((stats.sustainedHighlight - 16.0) / 219.0, 1.0)), eotf);
		const double fraction = static_cast<double>(light) / ceiling;
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
			std::ceil(highlightLinear / std::pow(2.0, allowedStops) * ceiling);

		const bool cutToBlack = stats.cut && apl < 20;
		const bool toBlack = stats.fade || cutToBlack;
		const int base = toBlack ? config.ledFloorBlack :
			std::max(CurveLevel(config, apl), config.ledFloor);
		out.lightTarget = std::max(config.ledFloorBlack, std::min(ceiling,
			std::max(base, static_cast<int>(highlightFloor))));

		const int before = light;
		if (stats.cut)
			light = out.lightTarget; // a cut masks the jump
		else if (stats.fade && light > out.lightTarget)
		{
			// Follow the fade at its own rate (its APL ratio above black), at
			// least one down-table step.
			const double ratio = previousApl > 16.0 ?
				std::max(0.0, std::min((stats.apl - 16.0) / (previousApl - 16.0), 1.0)) : 1.0;
			const int stepped = light - config.stepsDown.StepFor(light - out.lightTarget);
			light = std::max(out.lightTarget, std::min(stepped, static_cast<int>(std::floor(light * ratio))));
		}
		else if (light != out.lightTarget)
		{
			const int distance = std::abs(out.lightTarget - light);
			if (out.lightTarget > light)
				light += (apl < config.stepsUpSplitApl ? config.stepsUpDark : config.stepsUpBright)
					.StepFor(distance);
			else
				light -= config.stepsDown.StepFor(distance);
		}
		if (light != before) out.lightCommand = light;
		previousApl = stats.apl;

		out.light = light;
		out.gainDomain = GainDomain::Linear;
		out.knee = knee;
		out.highlightCap = highlightLinear > 0.0 ? 1.0 / highlightLinear : 1e9;
		out.capWeight = protection * large;
		out.holdGain = stats.fade;
		out.gain = CompensationGainFor(ceiling, light, out);
		std::ostringstream log;
		log << "LED compensate: apl=" << apl << " sustained_highlight=" << static_cast<int>(stats.sustainedHighlight)
			<< " light " << light << "/" << out.lightTarget << (out.lightCommand >= 0 ? " sent" : "")
			<< " protection=" << protection << " area=" << area << " gain=" << out.gain
			<< (stats.cut ? " cut" : "") << (stats.fade ? " fade" : "");
		out.log = log.str();
		return out;
	}
}
