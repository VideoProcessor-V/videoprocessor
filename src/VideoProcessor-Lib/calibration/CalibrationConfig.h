#pragma once

// Automatic calibration (docs/calibration/AUTO-CALIBRATION.md): the
// [calibration] section. A plain application section, not a profile. Every
// key has a control on the Config app's Calibration page (the schema-driven
// coverage test, ConfigEditorWindowTests, lists these rules).
//
// Nothing here is a frame rate, refresh rate or resolution: the patch window
// is a share of the screen area, settle times are display physics (seconds),
// and the meter's refresh mode is measured by the meter unless set.

#include "../ConfigFile.h"
#include "../ConfigSchema.h"
#include "../DisplayRuleExpression.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace Calibration
{
	constexpr const char* CONFIG_SECTION = "calibration";

	enum class DisplayKind
	{
		Projector,
		Lcd,
		Oled,
		Other,
	};

	enum class RefreshMode
	{
		Auto,        // the meter measures the display's refresh itself
		Refresh,     // -Y r
		NonRefresh,  // -Y n
	};

	enum class PlanChoice
	{
		Quick,
		Cube5,
		Standard,
		Cube13,
		Thorough,
	};

	enum class TargetTransferChoice
	{
		Bt1886,
		Gamma,
	};

	struct CalibrationSettings
	{
		std::string argyllPath;            // "" = search (PATH, the usual install folders)
		std::string meterModel = "auto";   // MeterCatalog id; auto = from spotread's instrument list; simulated = a dry run
		int meterPort = 0;                 // spotread -c; 0 = the first instrument found
		std::string displayType;           // spotread -y; "" = the model's default
		std::string spectralCorrection;    // .ccss / .ccmx path; "" = none
		RefreshMode refreshMode = RefreshMode::Auto;
		DisplayKind displayKind = DisplayKind::Projector;
		std::string displayName;           // run folder name; "" = from the output monitor
		PlanChoice plan = PlanChoice::Standard;
		double patchWindowPercent = 0.0;   // share of the screen area; 0 = from the display kind
		double backgroundPercent = 0.0;    // the surround of a windowed patch (signal %)
		int settleMs = 0;                  // 0 = from the display kind (plus the measured latency)
		int nearBlackMaxReads = 4;
		double repeatTolerancePercent = 1.0;
		int readTimeoutSeconds = 60;
		int warmupMinutes = 30;
		int driftInterval = 50;
		TargetTransferChoice targetTransfer = TargetTransferChoice::Bt1886;
		double targetGamma = 2.4;
		double targetWhitePercent = 100.0;
		int lutSize = 33;
		bool verifyAfter = true;
	};

	inline const char* ToString(DisplayKind kind)
	{
		switch (kind)
		{
		case DisplayKind::Projector: return "projector";
		case DisplayKind::Lcd: return "lcd";
		case DisplayKind::Oled: return "oled";
		default: return "other";
		}
	}

	inline const char* ToString(PlanChoice plan)
	{
		switch (plan)
		{
		case PlanChoice::Quick: return "quick";
		case PlanChoice::Cube5: return "cube5";
		case PlanChoice::Cube13: return "cube13";
		case PlanChoice::Thorough: return "thorough";
		default: return "standard";
		}
	}

	// The window a display kind needs when patch_window_percent is 0: a
	// projector shows full field (no ABL, the meter faces the screen); an LCD
	// with local dimming and an OLED with ABL need a window so the patch
	// light does not depend on the patch area.
	inline double DefaultWindowPercent(DisplayKind kind)
	{
		switch (kind)
		{
		case DisplayKind::Projector: return 100.0;
		case DisplayKind::Lcd: return 18.0;
		case DisplayKind::Oled: return 10.0;
		default: return 18.0;
		}
	}

	inline double EffectiveWindowPercent(const CalibrationSettings& s)
	{
		return s.patchWindowPercent > 0.0 ? s.patchWindowPercent : DefaultWindowPercent(s.displayKind);
	}

	inline const std::vector<ConfigSchema::KeyRule>& ConfigRules()
	{
		static const std::vector<ConfigSchema::KeyRule> rules = {
			ConfigSchema::Any("argyll_path"),
			ConfigSchema::Choice("meter_model", { "auto", "i1d3", "i1d2", "spyder4", "spyder5", "spyderx",
				"spyderx2", "colormunki_display", "i1pro", "i1pro2", "i1pro3", "colormunki_photo", "jeti",
				"klein_k10", "dtp94", "other", "simulated" }),
			ConfigSchema::Integer("meter_port", 0, 64),
			ConfigSchema::KeyRule{ "display_type", [](const std::string& value)
				{
					const std::string t = ConfigFile::Trim(value);
					if (t.size() > 8) return false;
					for (char c : t)
						if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
							return false;
					return true;
				}, "empty or spotread's -y selector (letters / digits)" },
			ConfigSchema::Any("spectral_correction"),
			ConfigSchema::Choice("refresh_mode", { "auto", "refresh", "non_refresh" }),
			ConfigSchema::Choice("display_kind", { "projector", "lcd", "oled", "other" }),
			ConfigSchema::Any("display_name"),
			ConfigSchema::Choice("plan", { "quick", "cube5", "standard", "cube13", "thorough" }),
			ConfigSchema::Number("patch_window_percent", 0.0, 100.0),
			ConfigSchema::Number("background_percent", 0.0, 100.0),
			ConfigSchema::Integer("settle_ms", 0, 30000),
			ConfigSchema::Integer("near_black_max_reads", 1, 16),
			ConfigSchema::Number("repeat_tolerance_percent", 0.1, 20.0),
			ConfigSchema::Integer("read_timeout_s", 5, 600),
			ConfigSchema::Integer("warmup_minutes", 0, 180),
			ConfigSchema::Integer("drift_interval", 0, 1000),
			ConfigSchema::Choice("target_transfer", { "bt1886", "gamma" }),
			ConfigSchema::Number("target_gamma", 1.6, 3.0),
			ConfigSchema::Number("target_white_percent", 25.0, 100.0),
			ConfigSchema::Choice("lut_size", { "17", "33", "65" }),
			ConfigSchema::Boolean("verify_after"),
		};
		return rules;
	}

	inline bool OwnsSection(const std::string& section)
	{
		return section == CONFIG_SECTION;
	}

	inline bool ValidateConfig(const ConfigFile& config, std::string& error)
	{
		return ConfigSchema::ValidateSection(config, CONFIG_SECTION, ConfigRules(), error);
	}

	namespace Detail
	{
		inline bool ReadNumber(const std::string& raw, double& out)
		{
			double parsed = 0.0;
			if (!DisplayRuleExpression::ParseNumber(ConfigFile::Trim(raw), parsed)) return false;
			out = parsed;
			return true;
		}

		inline bool ParseBool(const std::string& raw, bool& out)
		{
			const std::string n = ConfigFile::NormalizeName(raw);
			if (n == "1" || n == "true" || n == "yes" || n == "on") { out = true; return true; }
			if (n == "0" || n == "false" || n == "no" || n == "off") { out = false; return true; }
			return false;
		}
	}

	// Defaults for absent keys. Values are validated by ConfigRules first (an
	// invalid file never reaches here); an unparsable value keeps its default.
	inline CalibrationSettings ReadSettings(const ConfigFile& config)
	{
		CalibrationSettings s;
		const char* section = CONFIG_SECTION;
		std::string raw;
		if (config.TryGetString(section, "argyll_path", raw)) s.argyllPath = ConfigFile::Trim(raw);
		if (config.TryGetString(section, "meter_model", raw)) s.meterModel = ConfigFile::NormalizeName(raw);
		if (config.TryGetString(section, "meter_port", raw)) s.meterPort = std::atoi(ConfigFile::Trim(raw).c_str());
		if (config.TryGetString(section, "display_type", raw)) s.displayType = ConfigFile::Trim(raw);
		if (config.TryGetString(section, "spectral_correction", raw))
		{
			s.spectralCorrection = ConfigFile::Trim(raw);
			if (ConfigFile::NormalizeName(s.spectralCorrection) == "none") s.spectralCorrection.clear();
		}
		if (config.TryGetString(section, "refresh_mode", raw))
		{
			const std::string n = ConfigFile::NormalizeName(raw);
			s.refreshMode = n == "refresh" ? RefreshMode::Refresh : n == "non_refresh" ? RefreshMode::NonRefresh :
				RefreshMode::Auto;
		}
		if (config.TryGetString(section, "display_kind", raw))
		{
			const std::string n = ConfigFile::NormalizeName(raw);
			s.displayKind = n == "lcd" ? DisplayKind::Lcd : n == "oled" ? DisplayKind::Oled :
				n == "other" ? DisplayKind::Other : DisplayKind::Projector;
		}
		if (config.TryGetString(section, "display_name", raw)) s.displayName = ConfigFile::Trim(raw);
		if (config.TryGetString(section, "plan", raw))
		{
			const std::string n = ConfigFile::NormalizeName(raw);
			s.plan = n == "quick" ? PlanChoice::Quick : n == "cube5" ? PlanChoice::Cube5 :
				n == "cube13" ? PlanChoice::Cube13 : n == "thorough" ? PlanChoice::Thorough : PlanChoice::Standard;
		}
		if (config.TryGetString(section, "patch_window_percent", raw)) Detail::ReadNumber(raw, s.patchWindowPercent);
		if (config.TryGetString(section, "background_percent", raw)) Detail::ReadNumber(raw, s.backgroundPercent);
		if (config.TryGetString(section, "settle_ms", raw)) s.settleMs = std::atoi(ConfigFile::Trim(raw).c_str());
		if (config.TryGetString(section, "near_black_max_reads", raw))
			s.nearBlackMaxReads = std::atoi(ConfigFile::Trim(raw).c_str());
		if (config.TryGetString(section, "repeat_tolerance_percent", raw))
			Detail::ReadNumber(raw, s.repeatTolerancePercent);
		if (config.TryGetString(section, "read_timeout_s", raw))
			s.readTimeoutSeconds = std::atoi(ConfigFile::Trim(raw).c_str());
		if (config.TryGetString(section, "warmup_minutes", raw))
			s.warmupMinutes = std::atoi(ConfigFile::Trim(raw).c_str());
		if (config.TryGetString(section, "drift_interval", raw))
			s.driftInterval = std::atoi(ConfigFile::Trim(raw).c_str());
		if (config.TryGetString(section, "target_transfer", raw))
			s.targetTransfer = ConfigFile::NormalizeName(raw) == "gamma" ? TargetTransferChoice::Gamma :
				TargetTransferChoice::Bt1886;
		if (config.TryGetString(section, "target_gamma", raw)) Detail::ReadNumber(raw, s.targetGamma);
		if (config.TryGetString(section, "target_white_percent", raw)) Detail::ReadNumber(raw, s.targetWhitePercent);
		if (config.TryGetString(section, "lut_size", raw))
		{
			const int n = std::atoi(ConfigFile::Trim(raw).c_str());
			if (n == 17 || n == 33 || n == 65) s.lutSize = n;
		}
		if (config.TryGetString(section, "verify_after", raw)) Detail::ParseBool(raw, s.verifyAfter);
		// Clamp what the schema allows but the session cannot use.
		if (s.nearBlackMaxReads < 1) s.nearBlackMaxReads = 1;
		if (s.readTimeoutSeconds < 5) s.readTimeoutSeconds = 5;
		if (s.targetGamma < 1.6 || s.targetGamma > 3.0) s.targetGamma = 2.4;
		return s;
	}
}
