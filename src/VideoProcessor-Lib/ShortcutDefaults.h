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
//
// Bug hunt 3 (SH7, owner rule "modifier combinations only", 2026-10-18):
// every compiled default is a Ctrl and/or Alt combination. The bare and
// Shift-only defaults (R, Shift+R, V, Shift+V, F4) moved to modifier
// combinations; a user's explicit [shortcuts] binding is kept as written.
// The one exception is fullscreen_exit = Esc, marked fullscreenFocusOnly: it
// acts only while fullscreen is on and VP itself has the keyboard (its
// fullscreen output, a renderer window in it, or VP's own window - Windows
// can raise the topmost output without moving the foreground to it). That is
// the existing behaviour: keys reach VP only while a VP window is in front.
// In VP's window without fullscreen VP consumes Esc and does nothing, so the
// dialog manager cannot turn it into a close; the background observer never
// sees it unless the user marks it global.
//
// Bug hunt 3 (SH6): no default is global. While another application (MPC-HC,
// a browser) has focus VP acts on - and swallows - no key, unless the user
// lists the action in [shortcuts] global_actions (or turns global_hotkeys on,
// which marks every binding global). For MPC file playback, keep VP's keys off
// MPC's own (Alt+Enter, Ctrl+1..4 and Ctrl+I are MPC keys too) or leave
// them non-global.
#include <array>
#include <cstddef>
#include <cstring>

struct ShortcutDefault
{
	const char* key;            // [shortcuts] key
	const char* chord;          // compiled default ("" = none)
	bool rendererSpecific;      // read from the separate renderer file (legacy layout)
	bool fullscreenFocusOnly = false; // SH7: a bare default allowed only while fullscreen and VP has focus
};

constexpr ShortcutDefault kShortcutDefaults[] = {
	{ "auto_set",                "Ctrl+Shift+A", false },
	{ "fullscreen_exit",         "Esc",          false, true },   // only while fullscreen and VP has focus
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
	{ "renderer_restart",        "Ctrl+Alt+Shift+R", false },   // was Shift+R (SH7)
	{ "renderer_reset",          "Ctrl+Shift+R", false },       // was R (SH7)
	{ "capture_1",               "Ctrl+1",       false },
	{ "capture_2",               "Ctrl+2",       false },
	{ "capture_3",               "Ctrl+3",       false },
	{ "capture_4",               "Ctrl+4",       false },
	{ "video_conversion_off",    "Ctrl+Alt+V",   false },       // was V (SH7)
	{ "video_conversion_p010",   "Ctrl+Shift+V", false },       // was Shift+V (SH7)
	{ "config_editor",           "Ctrl+Shift+S", false },
	{ "toggle_noui",             "Ctrl+Shift+U", false },   // ConfigurationLiveApply::VideoOnlyToggleDefault*
	// SH8: Config shows it; VP reads it from the main file's [shortcuts]
	// first (the separate renderer file is the legacy fallback).
	{ "display_rules_auto",      "Ctrl+Shift+F4", true },       // was F4 (SH7)
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

// SH7: a default that needs no Ctrl or Alt. Only fullscreenFocusOnly
// entries may (ShortcutDefaultsTests).
inline bool ShortcutChordHasCtrlOrAlt(const char* chord)
{
	// Canonical form: modifiers first, each followed by '+'.
	return std::strstr(chord, "Ctrl+") != nullptr || std::strstr(chord, "Alt+") != nullptr;
}
