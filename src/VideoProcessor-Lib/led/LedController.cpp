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
	int ScriptController::LightTargetFor(int apl) const
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
		const int target = static_cast<int>(std::floor(level));
		return std::max(config.ledFloor, std::min(target, config.ledCeiling));
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
}
