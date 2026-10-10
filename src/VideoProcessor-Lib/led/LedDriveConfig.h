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
		// Never two commands closer than this. Barco documents at least 500 ms;
		// the old script ran 200 ms gaps for years without a lost command.
		int minCommandGapMs = 200;
		// Largest LPW1 drop in one command, so the black floor never falls in
		// big steps (later commands continue the ramp); 0 = no limit.
		int maxLevelsPerCommand = 12;
		// Optional pause after every burstCommands commands (Barco: 5 s after
		// 20); 0 = no pause.
		int burstCommands = 0;
		int burstPauseMs = 5000;
		// Off: send only, never wait for or ask the projector anything (the
		// old script's way). On: wait for each reply, query range and state.
		bool readBack = false;
		bool echo = true;
		Terminator terminator = Terminator::Auto;
		// Command transmission plus projector response, as pure dead time: the
		// LED is modelled as reaching a commanded level this long after the
		// command (whole output frames). Measured once with a camera.
		int ledLatencyMs = 80;
		// lpw : relative flux, ascending lpw; empty = flux proportional to lpw.
		std::vector<std::pair<double, double>> fluxTable;
	};

	// Experimental deeper fade to black with the projector's custom LED dim
	// (LDMM 1 + LDCR / LDCG / LDCB): off, all three colours at once, or the
	// colours taking turns in small steps.
	enum class FtbCustomDim { Off, Together, Turns };

	struct LedDriveConfig
	{
		bool enabled = false;
		Mode mode = Mode::LogOnly;
		// mode: script only; mode: compensate runs per output frame.
		int tickMs = 400;
		// Lowest LED level when the look-ahead covers the LED's response time
		// (led_floor), and when it does not (led_floor_reactive: reactive
		// mode cannot see a bright scene coming, so it stays higher; the
		// higher of the two applies).
		int ledFloor = 100;
		int ledFloorReactive = 100;
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
		// The window for the sustained APL that mode: compensate aims the LED
		// at (flash rejection); a cut uses the new scene's APL at once.
		int aplPersistMs = 100;
		double highlightToleranceStops = 0.33;
		double knee = 0.08;
		int antiPumpBand = 3;
		// mode: script's highlightdetect (the script used max >= 215).
		int scriptHighlightCode = 215;
		// mode: compensate ramps: levels per second, and no movement while
		// |target - current| <= deadband. early_ramp lets the ramp start ahead
		// of a cut the lookahead has seen.
		double rateUp = 400.0;
		double rateDown = 120.0;
		int deadband = 3;
		bool earlyRamp = true;
		// A cut the look-ahead has seen: one jump to the new scene's level,
		// timed to land on the cut frame (both directions; the cut hides it).
		bool cutJump = true;
		// A dark-to-bright cut it has not seen: straight up in one command,
		// like the old script's contrast dump, instead of ramping.
		bool brightCutDump = true;
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
		int fadeFrames = 6;
		double fadeMinStep = 0.5;
		double cutThreshold = 24.0;
		// Experimental (owner 2026-10-10): once the picture has been black for
		// ftb_black_ms with the LED at led_floor_black, switch to the custom
		// LED dim and step from ftb_custom_start down to ftb_custom_floor. The
		// FL35's custom values run 0-40 (owner-tested 2026-10-10: 40 is about
		// 40/255, below LPW1's minimum of 60, and 0 turns that LED off), so
		// entering is a small step down from 60/255 and the floor can be true
		// black;
		// one command every ftb_step_ms; back up and off at the first light.
		FtbCustomDim ftbCustomDim = FtbCustomDim::Off;
		int ftbStepMs = 100;
		int ftbCustomStart = 40;
		int ftbCustomFloor = 0;
		int ftbTurnStep = 2;
		int ftbBlackMs = 500;
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

	// lpw : relative flux pairs, lpw ascending (0-255), flux positive.
	inline bool ParseFluxTable(const std::string& text, std::vector<std::pair<double, double>>& table)
	{
		std::vector<std::pair<double, double>> pairs;
		if (!Detail::ParsePairs(text, pairs)) return false;
		for (size_t i = 0; i < pairs.size(); ++i)
		{
			if (pairs[i].first < 0 || pairs[i].first > 255 || pairs[i].second <= 0.0 || pairs[i].second > 10.0)
				return false;
			if (i > 0 && pairs[i].first <= pairs[i - 1].first) return false;
		}
		table = pairs;
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
			Integer("max_levels_per_command", 0, 255),
			Integer("burst_commands", 0, 1000),
			Integer("burst_pause_ms", 0, 60000),
			Boolean("read_back"),
			Boolean("echo"),
			Choice("terminator", { "auto", "cr", "literal_cr", "script" }),
			Integer("led_latency_ms", 0, 2000),
			Custom("flux_table", [](const std::string& value)
				{ std::vector<std::pair<double, double>> t; return ParseFluxTable(value, t); },
				"ascending lpw:relative_flux pairs, e.g. 60:0.55, 80:0.78, 100:1.00"),
		};
	}

	inline std::vector<ConfigSchema::KeyRule> LedDriveRules()
	{
		using namespace ConfigSchema;
		auto table = [](const std::string& value) { StepTable t; return ParseStepTable(value, t); };
		return {
			Boolean("enabled"),
			Choice("mode", { "compensate", "script", "log_only" }),
			Custom("profile_file", [](const std::string& value) { return !ConfigFile::Trim(value).empty(); },
				"the path of an LED profile file (relative to this configuration file, or absolute)"),
			Integer("tick_ms", 50, 5000),
			Choice("ftb_custom_dim", { "off", "together", "turns" }),
			Integer("ftb_step_ms", 50, 1000),
			Integer("ftb_custom_start", 1, 40),
			Integer("ftb_custom_floor", 0, 39),
			Integer("ftb_turn_step", 1, 50),
			Integer("ftb_black_ms", 100, 10000),
			Number("rate_up", 1.0, 10000.0),
			Number("rate_down", 1.0, 10000.0),
			Integer("deadband", 0, 50),
			Boolean("early_ramp"),
			Boolean("cut_jump"),
			Boolean("bright_cut_dump"),
			Integer("led_floor_reactive", 0, 255),
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
			Integer("apl_persist_ms", 0, 10000),
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
			Integer("fade_frames", 2, 120),
			Number("fade_min_step", 0.0, 50.0),
			Number("cut_threshold", 1.0, 255.0),
		};
	}

	// Strict validation of every [projector*] / [led_drive*] section, plus the
	// cross-key rules (floors within the ceiling).
	inline std::string LedProfilePath(const ConfigFile& config);
	inline std::string LedProfileSection(const ConfigFile& profile);
	inline LedDriveConfig ReadLedDriveConfig(const ConfigFile& config);

	inline bool Validate(const ConfigFile& config, std::string& error)
	{
		const auto projectorRules = ProjectorRules();
		const auto ledRules = LedDriveRules();
		for (const std::string& section : config.GetSectionNames())
		{
			const bool projector = section == "projector" || section.rfind("projector.", 0) == 0;
			const bool led = section == "led_drive" || section.rfind("led_drive.", 0) == 0;
			if (!projector && !led) continue;
			std::string moved;
			if (led && config.TryGetString(section, "led_response_ms", moved))
			{
				error = "[" + section + "] led_response_ms has moved: use led_latency_ms in [projector.Default]";
				return false;
			}
			if (led && config.TryGetString(section, "lead_ms", moved))
			{
				error = "[" + section + "] lead_ms was removed: the lead is derived from the lookahead and led_latency_ms";
				return false;
			}
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
				int customStart = 40, customFloor = 0;
				if (config.TryGetString(section, "ftb_custom_start", value)) Detail::ParseInt(value, 1, 40, customStart);
				if (config.TryGetString(section, "ftb_custom_floor", value)) Detail::ParseInt(value, 0, 39, customFloor);
				if (customFloor >= customStart)
				{
					error = "[" + section + "] needs ftb_custom_floor < ftb_custom_start";
					return false;
				}
			}
		}
		const std::string profilePath = LedProfilePath(config);
		if (!profilePath.empty())
		{
			ConfigFile profile;
			if (!profile.Load(profilePath, ConfigFile::ReadPolicy::Fresh))
			{
				error = "[led_drive.Default] profile_file: cannot read " + profilePath;
				return false;
			}
			const std::string section = LedProfileSection(profile);
			if (section.empty())
			{
				error = "LED profile " + profilePath + " has no [led_drive.Default] section";
				return false;
			}
			std::string nested;
			if (profile.TryGetString(section, "profile_file", nested) ||
				profile.TryGetString(section, "enabled", nested))
			{
				error = "LED profile " + profilePath +
					": enabled and profile_file belong in the main configuration";
				return false;
			}
			std::string profileError;
			if (!ConfigSchema::ValidateSection(profile, section, ledRules, profileError))
			{
				error = "LED profile " + profilePath + ": " + profileError;
				return false;
			}
			// The profile and the main settings together.
			const LedDriveConfig merged = ReadLedDriveConfig(config);
			if (merged.ledFloor > merged.ledCeiling || merged.ledFloorBlack > merged.ledFloor)
			{
				error = "LED profile " + profilePath +
					" with [led_drive.Default]: needs led_floor_black <= led_floor <= led_ceiling";
				return false;
			}
			if (merged.ftbCustomFloor >= merged.ftbCustomStart)
			{
				error = "LED profile " + profilePath + ": needs ftb_custom_floor < ftb_custom_start";
				return false;
			}
			if (merged.highlightAreaSmall >= merged.highlightAreaLarge)
			{
				error = "LED profile " + profilePath + ": needs highlight_area_small < highlight_area_large";
				return false;
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
		if (config.TryGetString(section, "max_levels_per_command", value)) Detail::ParseInt(value, 0, 255, result.maxLevelsPerCommand);
		if (config.TryGetString(section, "burst_commands", value)) Detail::ParseInt(value, 0, 1000, result.burstCommands);
		if (config.TryGetString(section, "burst_pause_ms", value)) Detail::ParseInt(value, 0, 60000, result.burstPauseMs);
		config.TryGetBool(section, "read_back", result.readBack);
		config.TryGetBool(section, "echo", result.echo);
		if (config.TryGetString(section, "terminator", value)) result.terminator = ParseTerminator(value);
		if (config.TryGetString(section, "led_latency_ms", value)) Detail::ParseInt(value, 0, 2000, result.ledLatencyMs);
		if (config.TryGetString(section, "flux_table", value)) ParseFluxTable(value, result.fluxTable);
		return result;
	}

	// Reads [led_drive.default]; absent keys keep their defaults.
	// profile_file, resolved against the folder of the configuration that
	// names it ("" when not set).
	inline std::string LedProfilePath(const ConfigFile& config)
	{
		std::string value;
		if (!config.TryGetString(LED_DRIVE_SECTION, "profile_file", value)) return {};
		std::string path = ConfigFile::Trim(value);
		if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
		if (path.empty()) return {};
		const bool absolute = (path.size() > 1 && path[1] == ':') || path[0] == '\\' || path[0] == '/';
		if (absolute) return path;
		const std::string& owner = config.GetLoadedPath();
		const size_t slash = owner.find_last_of("\\/");
		return slash == std::string::npos ? path : owner.substr(0, slash + 1) + path;
	}

	// The LED profile file's own settings section: [led_drive.Default] (or
	// [led_drive]); empty when it has neither.
	inline std::string LedProfileSection(const ConfigFile& profile)
	{
		for (const std::string& section : profile.GetSectionNames())
			if (section == LED_DRIVE_SECTION) return section;
		for (const std::string& section : profile.GetSectionNames())
			if (section == "led_drive") return section;
		return {};
	}

	// Applies one [led_drive] section's values over `result`.
	inline void ApplyLedDriveSection(const ConfigFile& config, const std::string& section, LedDriveConfig& result)
	{
		std::string value;
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
		readNumber("rate_up", result.rateUp);
		readNumber("rate_down", result.rateDown);
		readInt("deadband", 0, 50, result.deadband);
		config.TryGetBool(section, "early_ramp", result.earlyRamp);
		config.TryGetBool(section, "cut_jump", result.cutJump);
		config.TryGetBool(section, "bright_cut_dump", result.brightCutDump);
		readInt("led_floor_reactive", 0, 255, result.ledFloorReactive);
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
		readInt("apl_persist_ms", 0, 10000, result.aplPersistMs);
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
		readInt("fade_frames", 2, 120, result.fadeFrames);
		readNumber("fade_min_step", result.fadeMinStep);
		readNumber("cut_threshold", result.cutThreshold);
		if (config.TryGetString(section, "ftb_custom_dim", value))
		{
			const std::string method = ConfigFile::NormalizeName(value);
			result.ftbCustomDim = method == "together" ? FtbCustomDim::Together :
				method == "turns" ? FtbCustomDim::Turns : FtbCustomDim::Off;
		}
		readInt("ftb_step_ms", 50, 1000, result.ftbStepMs);
		readInt("ftb_custom_start", 1, 40, result.ftbCustomStart);
		readInt("ftb_custom_floor", 0, 39, result.ftbCustomFloor);
		readInt("ftb_turn_step", 1, 50, result.ftbTurnStep);
		readInt("ftb_black_ms", 100, 10000, result.ftbBlackMs);
	}

	// Built-in defaults (the beta_22 script's curve and tables), then the LED
	// profile file if one is named, then the configuration's own
	// [led_drive.Default] - so the Config app's core settings always win.
	inline LedDriveConfig ReadLedDriveConfig(const ConfigFile& config)
	{
		LedDriveConfig result;
		const std::string profilePath = LedProfilePath(config);
		if (!profilePath.empty())
		{
			ConfigFile profile;
			if (profile.Load(profilePath, ConfigFile::ReadPolicy::Fresh))
			{
				const std::string section = LedProfileSection(profile);
				if (!section.empty()) ApplyLedDriveSection(profile, section, result);
			}
		}
		ApplyLedDriveSection(config, LED_DRIVE_SECTION, result);
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
