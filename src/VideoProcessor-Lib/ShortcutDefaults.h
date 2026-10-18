#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// VP's compiled [shortcuts] defaults, the one table both the player
// (VideoProcessorDlg's accelerators) and Config (the Shortcuts page and its
// clash check) read (DV input fixes 2026-10-18). Before this the two programs
// each held their own copy, and the raw-capture recorder's default
// (Ctrl+Alt+D) was added to both while the report-a-problem shortcut
// (state_report, feat/state-report) already used it. ShortcutDefaultsTests
// checks that no two defaults (nor a reserved chord below) collide.
//
// Chords use Config's text form (CanonicalizeKeyChord): one key (A-Z, 0-9,
// F1-F24, Esc, Enter) with optional Ctrl, Alt, Shift. An empty chord means
// "no compiled default".
#include <array>
#include <cstddef>
#include <cstring>

struct ShortcutDefault
{
	const char* key;            // [shortcuts] key
	const char* chord;          // compiled default ("" = none)
	bool rendererSpecific;      // read from the separate renderer file (legacy layout)
};

constexpr ShortcutDefault kShortcutDefaults[] = {
	{ "auto_set",                "Ctrl+Shift+A", false },
	{ "fullscreen_exit",         "Esc",          false },
	{ "fullscreen_toggle",       "Alt+Enter",    false },   // ConfigurationLiveApply::ViewToggleDefault*
	{ "toggle_stats_overlay",    "Ctrl+I",       false },
	{ "capture_rendered_output", "Ctrl+Alt+S",   false },
	// Report-a-problem (StateReport.h; StateReportSettings::kShortcutDefault*).
	{ "state_report",            "Ctrl+Alt+D",   false },
	// Raw capture recording (RawCaptureRecorder.h; needs VP_RAW_CAPTURE_PATH).
	// Ctrl+Alt+D is the report-a-problem shortcut (state_report); Ctrl+Alt+R
	// is free among the defaults but is a documented user binding for
	// reapply_rules (Config tests), which a default would then take over.
	{ "raw_capture",             "Ctrl+Alt+W",   false },
	{ "reapply_rules",           "",             false },
	{ "room_lights",             "",             false },
	{ "show_profiles",           "Ctrl+Alt+I",   false },
	{ "pq_set",                  "Ctrl+Shift+P", false },
	{ "renderer_restart",        "Shift+R",      false },
	{ "renderer_reset",          "R",            false },
	{ "capture_1",               "Ctrl+1",       false },
	{ "capture_2",               "Ctrl+2",       false },
	{ "capture_3",               "Ctrl+3",       false },
	{ "capture_4",               "Ctrl+4",       false },
	{ "video_conversion_off",    "V",            false },
	{ "video_conversion_p010",   "Shift+V",      false },
	{ "config_editor",           "Ctrl+Shift+S", false },
	{ "toggle_noui",             "Ctrl+Shift+U", false },   // ConfigurationLiveApply::VideoOnlyToggleDefault*
	{ "display_rules_auto",      "F4",           true },
};

// Chords other branches already use for compiled defaults that are not on
// this branch yet. A new default must not take them; when the feature is
// merged its entry moves into kShortcutDefaults.
struct ReservedShortcut
{
	const char* owner;
	const char* chord;
};
// None at present: state_report (Ctrl+Alt+D) merged in integration round 4
// and is now in kShortcutDefaults.
constexpr std::array<ReservedShortcut, 0> kReservedShortcuts{};

inline constexpr bool ShortcutKeyEquals(const char* a, const char* b)
{
	while (*a && *b && *a == *b) { ++a; ++b; }
	return *a == *b;
}

// The compiled default for a [shortcuts] key, nullptr when the key has none
// in this table (unknown key).
inline constexpr const ShortcutDefault* FindShortcutDefault(const char* key)
{
	for (const ShortcutDefault& entry : kShortcutDefaults)
		if (ShortcutKeyEquals(entry.key, key))
			return &entry;
	return nullptr;
}

inline constexpr const char* ShortcutDefaultChord(const char* key)
{
	const ShortcutDefault* entry = FindShortcutDefault(key);
	return entry ? entry->chord : "";
}
