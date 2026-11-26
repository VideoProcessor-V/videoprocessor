// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// S-3f b, owner 2026-11-12 Q4 and 2026-11-15 21:28: the report-a-problem key (state_report, default
// Ctrl+Alt+D) acts while another application has focus BY DEFAULT ([shortcuts] global_actions absent;
// VP-V pathways only, R14-34), and VP-V code applies that default. Bill's file keeps its text: the
// dialog's ReadShortcutsGlobalActions reads only an explicit value, and its observer
// (StartGlobalShortcutObserver) serves an explicit list and global_hotkeys, as before. No dialog
// payload: the existing DialogExtension::OnTimer() seam gives the one-second tick and nothing else.
//
// - Facts (FromConfig, FileFactsReader): what the VP side reads itself, from the configuration file
//   the dialog reads, with the shared readers: DefaultGlobalActionFactsFor (ShortcutDefaults.h: the
//   absent-key default and its pathway gate, and global_hotkeys / foreground_only through
//   ConfigFile::TryGetBool, an unreadable value off as the dialog's readers keep it) and
//   StateReportSettings for the report key's chord. The pathway in force is read once (a [vpv]
//   change takes effect at the next start); the [shortcuts] keys follow the file.
// - Decide: the default key is wanted while the default applies, global_hotkeys is off (the dialog's
//   observer then makes every binding global itself), foreground_only is off (the user's choice that
//   keys act only while VP has focus) and the report key has a chord.
// - The hook (ShortcutObserverStatus.cpp, GUI executable only): a low-level keyboard hook on its own
//   thread for that one chord, the pattern of the dialog's GlobalShortcutObserver (which a VP-V file
//   cannot reach). Matches / Dispatches are ConfigurationLiveApply's shared rules: AltGr is not Ctrl+Alt;
//   with VP or VP's Config in front the key is theirs. It posts the report command
//   (ID_COMMAND_STATE_REPORT) to the dialog and never takes the key.
// - Extension: every tick (UI thread) reads the facts, restarts the hook when the plan changes (an
//   explicit global_actions value, global_hotkeys, foreground_only or a new chord applies within a
//   second; a pathway switch at the next start) and sets the "global shortcuts" Fallbacks entry while the hook is wanted but not running (rule 5). VpvGui::Poll
//   (VpStatusAdapter.h) adds it to the snapshot's Fallbacks line: the Config status and the one-time
//   notice always; Ctrl+I, the crash state and the Ctrl+Alt+D bundle while a renderer runs.
//
// Not reachable without a seam payload (owner: no widened hook): the dialog's interface mode and Video
// Only state, and whether the dialog's own observer runs. So the default key does not depend on the
// interface mode, and a failed start of the dialog's observer for an explicit list stays log-only, as
// at integ ab32bc33b.
//
// Header-only, UI thread only (the DialogExtension contract). The registration and the real hook live in
// ShortcutObserverStatus.cpp; tests construct their own Extension with a fake observer.

#include "../../VideoProcessor-GUI/DialogExtension.h"
#include <ConfigFile.h>
#include <ConfigurationLiveApply.h>
#include <ShortcutDefaults.h>
#include <VP-V/core/FallbackStatus.h>
#include <VP-V/modules/control/diagnostics/StateReportSettings.h>

#include <functional>
#include <sstream>
#include <string>
#include <utility>

namespace ShortcutObserverStatus
{
	constexpr const char* kId = "global shortcuts";
	constexpr const char* kNote =
		"the background keyboard hook is not running; global keys act only while VP has focus";

	// The facts the VP side reads itself (the dialog hands none over).
	struct Facts
	{
		bool defaultApplies = false;   // global_actions absent and its default list names state_report (VP-V pathways)
		bool globalHotkeys = false;    // [shortcuts] global_hotkeys on
		bool foregroundOnly = false;   // [shortcuts] foreground_only on
		bool keyBound = false;         // the report key has a chord
		unsigned char modifiers = 0;   // FCONTROL | FALT | FSHIFT of that chord
		unsigned short key = 0;        // its virtual key
	};

	// True when a comma-separated action list names `action` (each item as the dialog reads it).
	inline bool ListNames(const std::string& list, const std::string& action)
	{
		std::istringstream stream(list);
		std::string item;
		while (std::getline(stream, item, ','))
			if (ConfigFile::NormalizeName(ConfigFile::Trim(item)) == action)
				return true;
		return false;
	}

	// The file's pathway (ShortcutDefaults.h OriginalPathwayForConfig: [vpv] then [vprenderer]
	// pipeline_original_pathway, NormalizeName == "on").
	inline bool OriginalPathwayOf(const ConfigFile& config)
	{
		return OriginalPathwayForConfig([&config](const char* section, const char* key, std::string& out)
			{ return config.TryGetString(section, key, out); });
	}

	// originalPathway: the pathway in force (FileFactsReader reads it once; see there).
	inline Facts FromConfig(const ConfigFile& config, bool originalPathway)
	{
		Facts facts;
		const DefaultGlobalActionFacts shortcuts = DefaultGlobalActionFactsFor(config, originalPathway);
		facts.defaultApplies = ListNames(shortcuts.defaultActions, StateReportSettings::kShortcutKey);
		facts.globalHotkeys = shortcuts.globalHotkeys;
		facts.foregroundOnly = shortcuts.foregroundOnly;
		ACCEL chord = {};
		std::string error;
		facts.keyBound = StateReportSettings::ResolveShortcut(&config, chord, error);
		if (!facts.keyBound && !error.empty())
		{
			// A chord that does not parse: the dialog binds the compiled default (Bug hunt 3 SH10).
			facts.keyBound = StateReportSettings::TryParseChord(
				ShortcutDefaultChord(StateReportSettings::kShortcutKey), chord);
		}
		if (facts.keyBound)
		{
			facts.modifiers = static_cast<unsigned char>(chord.fVirt & (FCONTROL | FALT | FSHIFT));
			facts.key = chord.key;
		}
		return facts;
	}

	// The same with the file's own pathway.
	inline Facts FromConfig(const ConfigFile& config)
	{
		return FromConfig(config, OriginalPathwayOf(config));
	}

	// Executable reader: evaluates pathway & [shortcuts] every tick (Owner Option A live in-process switch).
	class FileFactsReader
	{
	public:
		using Loader = std::function<bool(ConfigFile&)>;
		explicit FileFactsReader(Loader load) : m_load(std::move(load)) {}
		bool operator()(Facts& facts)
		{
			ConfigFile config;
			if (!m_load || !m_load(config)) return false;
			facts = FromConfig(config, OriginalPathwayOf(config));
			return true;
		}
	private:
		Loader m_load;
	};

	// What the hook does: observe the report key's chord, or nothing.
	struct Plan
	{
		bool wanted = false;
		unsigned char modifiers = 0;
		unsigned short key = 0;
	};

	inline bool operator==(const Plan& a, const Plan& b)
	{
		return a.wanted == b.wanted && a.modifiers == b.modifiers && a.key == b.key;
	}

	inline bool operator!=(const Plan& a, const Plan& b) { return !(a == b); }

	inline Plan Decide(const Facts& facts)
	{
		Plan plan;
		plan.wanted = facts.defaultApplies && !facts.globalHotkeys && !facts.foregroundOnly && facts.keyBound;
		if (plan.wanted)
		{
			plan.modifiers = facts.modifiers;
			plan.key = facts.key;
		}
		return plan;
	}

	// A key-down of the plan's chord, from the physical modifier state read the way the dialog's observer
	// reads it (leftAlt: Left Alt, or Alt with Right Alt up). AltGr is not Ctrl+Alt.
	inline bool Matches(const Plan& plan, unsigned short virtualKey, bool control, bool leftAlt,
		bool rightAlt, bool shift)
	{
		return plan.wanted && virtualKey == plan.key &&
			ConfigurationLiveApply::KeyboardShortcutMatches((plan.modifiers & FCONTROL) != 0,
				(plan.modifiers & FALT) != 0, (plan.modifiers & FSHIFT) != 0, control, leftAlt, rightAlt, shift);
	}

	// GEM-041: pure state transition for the low-level keyboard hook. Only latches `keyDown = true`
	// when Matches(...) is true, so a bare press of the key (e.g. typing 'D' without Ctrl+Alt) never
	// latches keyDown and never blocks a subsequent Ctrl+Alt+D press. Returns true when a new matched
	// chord press should be dispatched (auto-repeat while held returns false until key-up).
	inline bool HandleKeyEvent(const Plan& plan, bool& keyDown, unsigned short virtualKey,
		bool isKeyDown, bool isKeyUp, bool control, bool leftAlt, bool rightAlt, bool shift)
	{
		if (!plan.wanted || virtualKey != plan.key)
			return false;
		if (isKeyUp)
		{
			keyDown = false;
			return false;
		}
		if (isKeyDown && !keyDown && Matches(plan, virtualKey, control, leftAlt, rightAlt, shift))
		{
			keyDown = true;
			return true;
		}
		return false;
	}

	// A matched key: post the report command? VP's own window or fullscreen output in front: the dialog or
	// the fullscreen host handles the key itself. VP's Config in front: the key is Config's. The key is
	// never taken (the report command does not own its key).
	inline bool Dispatches(bool vpForeground, bool configurationForeground)
	{
		ConfigurationLiveApply::BackgroundShortcutContext context;
		context.matched = true;
		context.vpForeground = vpForeground;
		context.configurationForeground = configurationForeground;
		context.explicitlyGlobal = true;   // the default makes the report key global
		return ConfigurationLiveApply::DecideBackgroundShortcut(context) ==
			ConfigurationLiveApply::BackgroundShortcutAction::DispatchAndPass;
	}

	// The entry's reason for one tick ("" = no entry): the hook is wanted and does not run.
	inline std::string Note(bool wanted, bool running)
	{
		return wanted && !running ? std::string(kNote) : std::string();
	}

	// A "Fallbacks engaged" heading with the entry ("" or "none" is replaced by it); unchanged without one.
	inline std::string WithEntry(const std::string& heading, const std::string& note)
	{
		return note.empty() ? heading : FallbackStatus::AppendEntry(heading, { kId, note });
	}

	// The note in force (written by the registered Extension each tick).
	inline std::string& CurrentNote()
	{
		static std::string note;
		return note;
	}

	// The background hook: ShortcutObserverStatus.cpp in the executable, a fake in the tests. UI thread.
	class IObserver
	{
	public:
		virtual ~IObserver() = default;
		virtual bool Start(const Plan& plan) = 0;   // true = the hook runs for the plan's chord
		virtual void Stop() = 0;                    // nothing runs afterwards (a no-op when nothing ran)
		virtual bool Running() const = 0;
	};

	class Extension : public DialogExtension::IDialogExtension
	{
	public:
		static constexpr int kRetryIntervalTicks = 5;   // GEM-041: back-off between Start() retries when wanted && !Running()

		// Fills the facts; false = the configuration is not readable this tick (the plan in force stays).
		using FactsReader = std::function<bool(Facts&)>;

		// `note` is where each tick's entry goes (CurrentNote() in the executable).
		Extension(FactsReader read, IObserver& observer, std::string& note)
			: m_read(std::move(read)), m_observer(observer), m_note(note) {}

		void OnTimer() override
		{
			Facts facts;
			bool planChanged = false;
			if (m_read && m_read(facts))
			{
				const Plan plan = Decide(facts);
				if (!m_planKnown || plan != m_plan)
				{
					m_observer.Stop();
					if (plan.wanted)
						m_observer.Start(plan);
					m_plan = plan;
					m_planKnown = true;
					m_retryTicks = 0;
					planChanged = true;
				}
			}
			// GEM-041: if the hook is wanted but failed to start (e.g. no dialog HWND yet on first tick),
			// retry with a back-off every kRetryIntervalTicks ticks until it runs or the plan changes.
			if (!planChanged && m_planKnown && m_plan.wanted)
			{
				if (m_observer.Running())
					m_retryTicks = 0;
				else if (++m_retryTicks >= kRetryIntervalTicks)
				{
					m_retryTicks = 0;
					m_observer.Start(m_plan);
				}
			}
			m_note = Note(m_plan.wanted, m_observer.Running());
		}

		const Plan& PlanInForce() const { return m_plan; }

	private:
		FactsReader m_read;
		IObserver& m_observer;
		std::string& m_note;
		Plan m_plan;
		bool m_planKnown = false;
		int m_retryTicks = 0;
	};
}
