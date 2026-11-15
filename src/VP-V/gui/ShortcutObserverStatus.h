// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// S-3f b (rule 5, no silent fallbacks; lead 17 MUST-1): the "global shortcuts" Fallbacks entry while
// the background keyboard hook that VP's global keys need is not running. VP-V-owned, reached through
// the EXISTING dialog seam (DialogExtension::OnTimer, whose payload carries the dialog's facts); no
// line in Bill's StartGlobalShortcutObserver or PublishActiveProfileStatus.
//
// - Decision: Note() (pure). An entry only in the VP-V pathways and only for VP's global keys
//   ([shortcuts] global_actions / global_hotkeys while background shortcuts are enabled). The Original
//   pathway and Bill's own config-only observer ([shortcuts] foreground_only) stay log-only, as Bill.
// - Surfaces: VpvGui::Poll (VpStatusAdapter.h) adds the entry to the snapshot's Fallbacks line, so the
//   ONE existing path shows it: the Config status and the one-time notice (UpdateFallbacksStatus via
//   VpvGui::FallbacksHeading), the Ctrl+I Fallbacks row, the crash state and the Ctrl+Alt+D bundle.
// - The pathway is read once (the first tick): a pathway switch applies at the next start, as the
//   default global-action set itself (owner 2026-11-15 04:44: the live switch waits).
//
// UI thread only (the DialogExtension contract). Header-only; the registration lives in
// ShortcutObserverStatus.cpp (GUI executable only), so tests construct their own Extension.

#include "../../VideoProcessor-GUI/DialogExtension.h"
#include <ConfigurationLiveApply.h>
#include <VP-V/core/FallbackStatus.h>

#include <functional>
#include <string>
#include <utility>

namespace ShortcutObserverStatus
{
	constexpr const char* kId = "global shortcuts";
	constexpr const char* kNote =
		"the background keyboard hook is not running; global keys act only while VP has focus";

	// The entry's reason for one tick ("" = no entry).
	inline std::string Note(const DialogExtension::DialogFacts& facts, bool originalPathway)
	{
		if (originalPathway)
			return std::string();   // Bill's pathway: log-only, as Bill
		const bool globalKeysWanted = ConfigurationLiveApply::ShouldEnableBackgroundShortcuts(
			facts.modernInterface, facts.noUi, facts.shortcutsForegroundOnly) &&
			(facts.shortcutsGlobalHotkeys || facts.shortcutsGlobalActions > 0);
		return globalKeysWanted && !facts.shortcutObserverRunning ? std::string(kNote) : std::string();
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

	class Extension : public DialogExtension::IDialogExtension
	{
	public:
		using PathwayReader = std::function<bool()>;   // true = pipeline_original_pathway on

		// `note` is where the tick's answer goes (CurrentNote() in the executable).
		Extension(PathwayReader readPathway, std::string& note)
			: m_readPathway(std::move(readPathway)), m_note(note) {}

		void OnTimerFacts(const DialogExtension::DialogFacts& facts) override
		{
			if (!m_pathwayRead)
			{
				m_originalPathway = m_readPathway ? m_readPathway() : false;
				m_pathwayRead = true;
			}
			m_note = Note(facts, m_originalPathway);
		}

	private:
		PathwayReader m_readPathway;
		std::string& m_note;
		bool m_pathwayRead = false;
		bool m_originalPathway = false;
	};
}
