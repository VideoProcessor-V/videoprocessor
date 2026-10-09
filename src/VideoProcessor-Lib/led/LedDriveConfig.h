#pragma once

#include "../ConfigFile.h"
#include "../ConfigSchema.h"
#include "../DisplayRuleExpression.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

// Configuration for the LED drive module (brief Stage 7,
// docs/VP-led-drive-module.md and docs/VP-led-drive-answers-1.md).
// [projector.<name>] configures the transport, [led_drive.<name>] the
// controller. Plain application sections: only the "default" name is used.
namespace LedDrive
{
	constexpr const char* PROJECTOR_SECTION = "projector.default";
	constexpr const char* LED_DRIVE_SECTION = "led_drive.default";

	inline bool OwnsSection(const std::string& section)
	{
		return section == "projector" || section.rfind("projector.", 0) == 0 ||
			section == "led_drive" || section.rfind("led_drive.", 0) == 0;
	}

	// "distance threshold : step", evaluated from the largest threshold down.
	struct StepTable
	{
		std::vector<std::pair<int, int>> rows;

		int StepFor(int distance) const
		{
			for (const auto& row : rows)
				if (distance >= row.first) return row.second;
			return 0;
		}
	};

	struct CurvePoint
	{
		double apl = 0.0;
		double level = 0.0;
	};

	enum class Mode { Compensate, Script, LogOnly };

	enum class Terminator { Auto, Cr, LiteralCr, Script };

	struct ProjectorConfig
	{
		std::string protocol = "none";
		std::string port = "COM1";
		int baud = 19200;
		// -1 = query the projector (?N / ?M).
		int lightMin = -1;
		int lightMax = -1;
		int ackTimeoutMs = 300;
		int minCommandGapMs = 100;
		bool echo = true;
		Terminator terminator = Terminator::Auto;
	};

	struct LedDriveConfig
	{
		bool enabled = false;
		Mode mode = Mode::LogOnly;
		int tickMs = 400;
		int leadMs = 0;
		int ledFloor = 100;
		int ledFloorBlack = 60;
		int ledCeiling = 255;
		std::vector<CurvePoint> curve = {
			{ 0, 100 }, { 20, 100 }, { 40, 200 }, { 70, 230 }, { 100, 255 }, { 255, 255 } };
		StepTable stepsUpDark{ { { 100, 7 }, { 50, 5 }, { 25, 4 }, { 10, 1 }, { 0, 1 } } };
		StepTable stepsUpBright{ { { 100, 7 }, { 50, 4 }, { 25, 2 }, { 10, 1 }, { 0, 1 } } };
		int stepsUpSplitApl = 40;
		StepTable stepsDown{ { { 100, 5 }, { 50, 3 }, { 25, 2 }, { 10, 1 }, { 0, 1 } } };
		// R, G, B. The script's effective weights (BGR2GRAY on an RGB array).
		double aplWeights[3] = { 0.114, 0.587, 0.299 };
		double highlightPercentile = 99.5;
		int highlightPersistMs = 250;
		double highlightToleranceStops = 0.33;
		double knee = 0.08;
		int antiPumpBand = 3;
		// mode: script's highlightdetect (the script used max >= 215).
		int scriptHighlightCode = 215;
		double compensationEotf = 2.2;
		// "Encode as if for this display gamma" on the projector's
		// compensation_eotf: the old output_gamma 2.4 on film 2.2 lift, applied
		// by the module whether or not it drives the LED. 0 = off.
		double gammaBoost = 0.0;
		// mode: compensate's highlight policy: below highlight_area_small the
		// knee absorbs sustained highlights, above highlight_area_large the
		// global gain makes room; blended in between. Protection relaxes over
		// highlight_release_ms (it engages over highlight_persist_ms).
		double highlightAreaSmall = 0.005;
		double highlightAreaLarge = 0.05;
		int highlightReleaseMs = 2000;
		// One-pole model of the LED's response; 0 until it has been measured.
		int ledResponseMs = 0;
		int fadeFrames = 6;
		double fadeMinStep = 0.5;
		double cutThreshold = 24.0;
	};

	namespace Detail
	{
		inline bool ParseNumber(const std::string& text, double& value)
		{
			return DisplayRuleExpression::ParseNumber(ConfigFile::Trim(text), value) &&
				std::isfinite(value);
		}

		inline bool ParseInt(const std::string& text, int minimum, int maximum, int& value)
		{
			double parsed = 0.0;
			if (!ParseNumber(text, parsed) || parsed != std::floor(parsed) ||
				parsed < minimum || parsed > maximum)
				return false;
			value = static_cast<int>(parsed);
			return true;
		}

		inline std::vector<std::string> SplitList(const std::string& text)
		{
			std::vector<std::string> items;
			size_t start = 0;
			while (start <= text.size())
			{
				const size_t comma = text.find(',', start);
				const std::string item = ConfigFile::Trim(text.substr(start,
					comma == std::string::npos ? std::string::npos : comma - start));
				if (!item.empty()) items.push_back(item);
				if (comma == std::string::npos) break;
				start = comma + 1;
			}
			return items;
		}

		inline bool ParsePairs(const std::string& text,
			std::vector<std::pair<double, double>>& pairs)
		{
			pairs.clear();
			for (const std::string& item : SplitList(text))
			{
				const size_t colon = item.find(':');
				double first = 0.0, second = 0.0;
				if (colon == std::string::npos ||
					!ParseNumber(item.substr(0, colon), first) ||
					!ParseNumber(item.substr(colon + 1), second))
					return false;
				pairs.emplace_back(first, second);
			}
			return !pairs.empty();
		}
	}

	// Thresholds must be distinct whole numbers, written largest first, and end
	// at 0 so every distance has a step.
	inline bool ParseStepTable(const std::string& text, StepTable& table)
	{
		std::vector<std::pair<double, double>> pairs;
		if (!Detail::ParsePairs(text, pairs)) return false;
		StepTable parsed;
		for (const auto& pair : pairs)
		{
			if (pair.first != std::floor(pair.first) || pair.second != std::floor(pair.second) ||
				pair.first < 0 || pair.first > 255 || pair.second < 0 || pair.second > 255)
				return false;
			if (!parsed.rows.empty() && pair.first >= parsed.rows.back().first) return false;
			parsed.rows.emplace_back(static_cast<int>(pair.first), static_cast<int>(pair.second));
		}
		if (parsed.rows.back().first != 0) return false;
		table = parsed;
		return true;
	}

	// APL (code units) : light level, ascending APL, at least two points.
	inline bool ParseCurve(const std::string& text, std::vector<CurvePoint>& curve)
	{
		std::vector<std::pair<double, double>> pairs;
		if (!Detail::ParsePairs(text, pairs) || pairs.size() < 2) return false;
		std::vector<CurvePoint> parsed;
		for (const auto& pair : pairs)
		{
			if (pair.first < 0 || pair.first > 255 || pair.second < 0 || pair.second > 255)
				return false;
			if (!parsed.empty() && pair.first <= parsed.back().apl) return false;
			parsed.push_back({ pair.first, pair.second });
		}
		curve = parsed;
		return true;
	}

	inline bool ParseWeights(const std::string& text, double weights[3])
	{
		const auto items = Detail::SplitList(text);
		double parsed[3] = {};
		if (items.size() != 3) return false;
		double sum = 0.0;
		for (int i = 0; i < 3; ++i)
		{
			if (!Detail::ParseNumber(items[i], parsed[i]) || parsed[i] < 0.0) return false;
			sum += parsed[i];
		}
		if (std::abs(sum - 1.0) > 0.01) return false;
		std::copy(parsed, parsed + 3, weights);
		return true;
	}

	inline ConfigSchema::KeyRule Custom(const char* key, ConfigSchema::Validator validator,
		const char* expected)
	{
		return { key, std::move(validator), expected };
	}

	inline std::vector<ConfigSchema::KeyRule> ProjectorRules()
	{
		using namespace ConfigSchema;
		auto autoOrLevel = [](const std::string& value)
		{
			int level = 0;
			return ConfigFile::NormalizeName(value) == "auto" ||
				Detail::ParseInt(value, 0, 255, level);
		};
		return {
			Choice("protocol", { "barco_ascii", "none" }),
			Custom("port", [](const std::string& value)
				{
					const std::string port = ConfigFile::NormalizeName(value);
					int number = 0;
					return port.size() > 3 && port.compare(0, 3, "com") == 0 &&
						Detail::ParseInt(port.substr(3), 1, 256, number);
				}, "COM1 through COM256"),
			Integer("baud", 1200, 921600),
			Custom("light_min", autoOrLevel, "auto or a level from 0 to 255"),
			Custom("light_max", autoOrLevel, "auto or a level from 0 to 255"),
			Integer("ack_timeout_ms", 20, 5000),
			Integer("min_command_gap_ms", 0, 5000),
			Boolean("echo"),
			Choice("terminator", { "auto", "cr", "literal_cr", "script" }),
		};
	}

	inline std::vector<ConfigSchema::KeyRule> LedDriveRules()
	{
		using namespace ConfigSchema;
		auto table = [](const std::string& value) { StepTable t; return ParseStepTable(value, t); };
		return {
			Boolean("enabled"),
			Choice("mode", { "compensate", "script", "log_only" }),
			Integer("tick_ms", 50, 5000),
			Integer("lead_ms", 0, 2000),
			Integer("led_floor", 0, 255),
			Integer("led_floor_black", 0, 255),
			Integer("led_ceiling", 1, 255),
			Custom("curve", [](const std::string& value)
				{ std::vector<CurvePoint> c; return ParseCurve(value, c); },
				"ascending APL:level pairs (0-255), e.g. 0:100, 40:200, 100:255"),
			Custom("steps_up_dark", table, "descending distance:step pairs ending at 0"),
			Custom("steps_up_bright", table, "descending distance:step pairs ending at 0"),
			Integer("steps_up_split_apl", 0, 255),
			Custom("steps_down", table, "descending distance:step pairs ending at 0"),
			Custom("apl_weights", [](const std::string& value)
				{ double w[3]; return ParseWeights(value, w); },
				"three non-negative R, G, B weights summing to 1"),
			Number("highlight_percentile", 50.0, 100.0),
			Integer("highlight_persist_ms", 0, 10000),
			Number("highlight_tolerance_stops", 0.0, 4.0),
			Number("knee", 0.0, 0.5),
			Integer("anti_pump_band", 0, 50),
			Integer("script_highlight_code", 16, 255),
			Number("compensation_eotf", 1.0, 3.0),
			Custom("gamma_boost", [](const std::string& value)
				{
					double gamma = 0.0;
					return ConfigFile::NormalizeName(value) == "off" ||
						(Detail::ParseNumber(value, gamma) && gamma >= 1.0 && gamma <= 3.0);
				}, "off or a display gamma from 1.0 to 3.0, e.g. 2.4"),
			Number("highlight_area_small", 0.0, 1.0),
			Number("highlight_area_large", 0.0, 1.0),
			Integer("highlight_release_ms", 0, 60000),
			Integer("led_response_ms", 0, 10000),
			Integer("fade_frames", 2, 120),
			Number("fade_min_step", 0.0, 50.0),
			Number("cut_threshold", 1.0, 255.0),
		};
	}

	// Strict validation of every [projector*] / [led_drive*] section, plus the
	// cross-key rules (floors within the ceiling).
	inline bool Validate(const ConfigFile& config, std::string& error)
	{
		const auto projectorRules = ProjectorRules();
		const auto ledRules = LedDriveRules();
		for (const std::string& section : config.GetSectionNames())
		{
			const bool projector = section == "projector" || section.rfind("projector.", 0) == 0;
			const bool led = section == "led_drive" || section.rfind("led_drive.", 0) == 0;
			if (!projector && !led) continue;
			if (!ConfigSchema::ValidateSection(config, section,
				projector ? projectorRules : ledRules, error))
				return false;
			if (led)
			{
				int ceiling = 255, floor = 100, floorBlack = 60;
				std::string value;
				if (config.TryGetString(section, "led_ceiling", value)) Detail::ParseInt(value, 1, 255, ceiling);
				if (config.TryGetString(section, "led_floor", value)) Detail::ParseInt(value, 0, 255, floor);
				if (config.TryGetString(section, "led_floor_black", value)) Detail::ParseInt(value, 0, 255, floorBlack);
				if (floor > ceiling || floorBlack > floor)
				{
					error = "[" + section + "] needs led_floor_black <= led_floor <= led_ceiling";
					return false;
				}
				double areaSmall = 0.005, areaLarge = 0.05;
				if (config.TryGetString(section, "highlight_area_small", value)) Detail::ParseNumber(value, areaSmall);
				if (config.TryGetString(section, "highlight_area_large", value)) Detail::ParseNumber(value, areaLarge);
				if (areaSmall >= areaLarge)
				{
					error = "[" + section + "] needs highlight_area_small < highlight_area_large";
					return false;
				}
			}
		}
		return true;
	}

	inline Terminator ParseTerminator(const std::string& value)
	{
		const std::string normalized = ConfigFile::NormalizeName(value);
		if (normalized == "cr") return Terminator::Cr;
		if (normalized == "literal_cr") return Terminator::LiteralCr;
		if (normalized == "script") return Terminator::Script;
		return Terminator::Auto;
	}

	// Reads [projector.default]; absent keys keep their defaults. Call Validate
	// first: this reader assumes valid values.
	inline ProjectorConfig ReadProjectorConfig(const ConfigFile& config)
	{
		ProjectorConfig result;
		std::string value;
		const std::string section = PROJECTOR_SECTION;
		if (config.TryGetString(section, "protocol", value)) result.protocol = ConfigFile::NormalizeName(value);
		if (config.TryGetString(section, "port", value)) result.port = ConfigFile::Trim(value);
		if (config.TryGetString(section, "baud", value)) Detail::ParseInt(value, 1200, 921600, result.baud);
		if (config.TryGetString(section, "light_min", value) && ConfigFile::NormalizeName(value) != "auto")
			Detail::ParseInt(value, 0, 255, result.lightMin);
		if (config.TryGetString(section, "light_max", value) && ConfigFile::NormalizeName(value) != "auto")
			Detail::ParseInt(value, 0, 255, result.lightMax);
		if (config.TryGetString(section, "ack_timeout_ms", value)) Detail::ParseInt(value, 20, 5000, result.ackTimeoutMs);
		if (config.TryGetString(section, "min_command_gap_ms", value)) Detail::ParseInt(value, 0, 5000, result.minCommandGapMs);
		config.TryGetBool(section, "echo", result.echo);
		if (config.TryGetString(section, "terminator", value)) result.terminator = ParseTerminator(value);
		return result;
	}

	// Reads [led_drive.default]; absent keys keep their defaults.
	inline LedDriveConfig ReadLedDriveConfig(const ConfigFile& config)
	{
		LedDriveConfig result;
		std::string value;
		const std::string section = LED_DRIVE_SECTION;
		auto readInt = [&](const char* key, int minimum, int maximum, int& target)
		{
			if (config.TryGetString(section, key, value)) Detail::ParseInt(value, minimum, maximum, target);
		};
		auto readNumber = [&](const char* key, double& target)
		{
			if (config.TryGetString(section, key, value)) Detail::ParseNumber(value, target);
		};
		config.TryGetBool(section, "enabled", result.enabled);
		if (config.TryGetString(section, "mode", value))
		{
			const std::string mode = ConfigFile::NormalizeName(value);
			result.mode = mode == "compensate" ? Mode::Compensate :
				mode == "script" ? Mode::Script : Mode::LogOnly;
		}
		readInt("tick_ms", 50, 5000, result.tickMs);
		readInt("lead_ms", 0, 2000, result.leadMs);
		readInt("led_floor", 0, 255, result.ledFloor);
		readInt("led_floor_black", 0, 255, result.ledFloorBlack);
		readInt("led_ceiling", 1, 255, result.ledCeiling);
		if (config.TryGetString(section, "curve", value)) ParseCurve(value, result.curve);
		if (config.TryGetString(section, "steps_up_dark", value)) ParseStepTable(value, result.stepsUpDark);
		if (config.TryGetString(section, "steps_up_bright", value)) ParseStepTable(value, result.stepsUpBright);
		readInt("steps_up_split_apl", 0, 255, result.stepsUpSplitApl);
		if (config.TryGetString(section, "steps_down", value)) ParseStepTable(value, result.stepsDown);
		if (config.TryGetString(section, "apl_weights", value)) ParseWeights(value, result.aplWeights);
		readNumber("highlight_percentile", result.highlightPercentile);
		readInt("highlight_persist_ms", 0, 10000, result.highlightPersistMs);
		readNumber("highlight_tolerance_stops", result.highlightToleranceStops);
		readNumber("knee", result.knee);
		readInt("anti_pump_band", 0, 50, result.antiPumpBand);
		readInt("script_highlight_code", 16, 255, result.scriptHighlightCode);
		readNumber("compensation_eotf", result.compensationEotf);
		if (config.TryGetString(section, "gamma_boost", value))
			result.gammaBoost = ConfigFile::NormalizeName(value) == "off" ? 0.0 :
				(Detail::ParseNumber(value, result.gammaBoost) ? result.gammaBoost : 0.0);
		readNumber("highlight_area_small", result.highlightAreaSmall);
		readNumber("highlight_area_large", result.highlightAreaLarge);
		readInt("highlight_release_ms", 0, 60000, result.highlightReleaseMs);
		readInt("led_response_ms", 0, 10000, result.ledResponseMs);
		readInt("fade_frames", 2, 120, result.fadeFrames);
		readNumber("fade_min_step", result.fadeMinStep);
		readNumber("cut_threshold", result.cutThreshold);
		return result;
	}

	// Exponent the compensation hook (and the measurement) apply to the
	// encoded signal for gamma_boost: compensation_eotf / gamma_boost. 1 = off.
	inline double BoostExponent(const LedDriveConfig& led)
	{
		return led.gammaBoost > 0.0 ? led.compensationEotf / led.gammaBoost : 1.0;
	}

	// The module talks to the projector only when it is enabled and not in
	// log_only mode; otherwise it uses the dry-run "none" driver.
	inline std::string EffectiveProtocol(const ProjectorConfig& projector,
		const LedDriveConfig& led)
	{
		if (!led.enabled || led.mode == Mode::LogOnly) return "none";
		return projector.protocol;
	}
}
