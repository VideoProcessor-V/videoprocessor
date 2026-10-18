#pragma once

// Settings of the report-a-problem feature (StateReport.h), shared by VP, the
// Config app and tests. Every one has a Config control (owner rule: nothing
// keyboard-only): Shortcuts page "Save problem report", Logs page
// "Save a problem report automatically" and "Include a small picture".
//
//   [shortcuts] state_report      default Ctrl+Alt+D; empty = no shortcut
//   [logging]   auto_report       default false: VP saves a report itself on
//                                 renderer recovery resets and render stalls,
//                                 at most one per 10 minutes, 5 per run
//   [logging]   report_thumbnail  default false: add a small picture of the
//                                 next rendered frame (the only pixels a
//                                 report can hold)
//
// C++14, header-only.

#include <ConfigFile.h>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace StateReportSettings
{
	constexpr const char* kShortcutKey = "state_report";
	constexpr const char* kShortcutDefault = "Ctrl+Alt+D";
	constexpr BYTE kShortcutDefaultModifiers = FCONTROL | FALT;
	constexpr WORD kShortcutDefaultKey = 'D';
	constexpr const char* kAutoReportKey = "auto_report";
	constexpr const char* kThumbnailKey = "report_thumbnail";

	struct Settings
	{
		bool autoReport = false;
		bool thumbnail = false;
	};

	// Reads [logging] auto_report / report_thumbnail. Absent = default (off).
	// A present value that is not a Boolean keeps the default and is reported.
	inline bool Read(const ConfigFile& config, Settings& settings, std::string& error)
	{
		settings = Settings();
		error.clear();
		std::string raw;
		bool value = false;
		if (config.TryGetString("logging", kAutoReportKey, raw))
		{
			if (config.TryGetBool("logging", kAutoReportKey, value))
				settings.autoReport = value;
			else
				error = "unsupported value for logging.auto_report: " + raw;
		}
		if (config.TryGetString("logging", kThumbnailKey, raw))
		{
			if (config.TryGetBool("logging", kThumbnailKey, value))
				settings.thumbnail = value;
			else if (error.empty())
				error = "unsupported value for logging.report_thumbnail: " + raw;
		}
		return error.empty();
	}

	// The chord grammar VP's accelerators accept: modifiers Ctrl/Control,
	// Alt, Shift joined with '+', then one key: A-Z, 0-9, F1-F24, Esc/Escape,
	// Enter/Return. Moved here from VideoProcessorDlg.cpp (report-a-problem,
	// 2026-10-17) so tests and the report shortcut share it; behaviour is
	// unchanged.
	inline bool TryParseChord(const std::string& value, ACCEL& accelerator)
	{
		BYTE modifiers = FVIRTKEY;
		std::string keyToken;
		size_t start = 0;
		while (start <= value.size())
		{
			const size_t end = value.find('+', start);
			const std::string token = ConfigFile::Trim(value.substr(start, end - start));
			if (token.empty())
				return false;

			const std::string normalizedToken = ConfigFile::NormalizeName(token);
			if (normalizedToken == "ctrl" || normalizedToken == "control")
				modifiers |= FCONTROL;
			else if (normalizedToken == "alt")
				modifiers |= FALT;
			else if (normalizedToken == "shift")
				modifiers |= FSHIFT;
			else if (keyToken.empty())
				keyToken = token;
			else
				return false;

			if (end == std::string::npos)
				break;
			start = end + 1;
		}

		if (keyToken.empty())
			return false;

		const std::string normalizedKey = ConfigFile::NormalizeName(keyToken);
		WORD key = 0;
		if (normalizedKey == "escape" || normalizedKey == "esc")
			key = VK_ESCAPE;
		else if (normalizedKey == "enter" || normalizedKey == "return")
			key = VK_RETURN;
		else if (normalizedKey.size() >= 2 && normalizedKey[0] == 'f')
		{
			const std::string numberToken = normalizedKey.substr(1);
			if (numberToken.empty() || numberToken.size() > 2 ||
				!std::all_of(numberToken.begin(), numberToken.end(),
					[](unsigned char character) { return std::isdigit(character) != 0; }))
				return false;

			const int functionNumber = std::stoi(numberToken);
			if (functionNumber < 1 || functionNumber > 24)
				return false;
			key = static_cast<WORD>(VK_F1 + functionNumber - 1);
		}
		else if (keyToken.size() == 1 && std::isalnum(static_cast<unsigned char>(keyToken[0])))
		{
			key = static_cast<WORD>(std::toupper(static_cast<unsigned char>(keyToken[0])));
		}
		else
			return false;

		accelerator = { modifiers, key, 0 };
		return true;
	}

	// The report shortcut VP binds: the configured chord, the default when the
	// key is absent, none (false, error empty) when it is present but empty,
	// none with an error when it does not parse.
	inline bool ResolveShortcut(const ConfigFile* config, ACCEL& accelerator, std::string& error)
	{
		error.clear();
		std::string configured;
		if (!config || !config->TryGetString("shortcuts", kShortcutKey, configured))
		{
			accelerator = { static_cast<BYTE>(FVIRTKEY | kShortcutDefaultModifiers), kShortcutDefaultKey, 0 };
			return true;
		}
		if (ConfigFile::Trim(configured).empty())
			return false;
		if (!TryParseChord(configured, accelerator))
		{
			error = "invalid [shortcuts] state_report: " + configured;
			return false;
		}
		return true;
	}
}
