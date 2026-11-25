// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// VP-V rows that used to sit inside Bill's pages (P8 step 2, owner rule 2026-11-01: Bill's pages stay
// exactly his). They are global keys that act on BOTH pathways (the Original pathway does not switch
// them off), so they live in cards below the VP-V page's unit tabs, outside the part the Original
// pathway greys. Built through the editor's own field helpers (EditorKit): same object names, same
// keys, defaults and save/load round trip as before the move. Labels and tooltips are unchanged.
//
// Compiled in VpvGeneralPageCards.cpp. UI thread only.

#include <vector>

class QWidget;

namespace ConfigPageHost
{
	class EditorKit;
}

namespace Vpv
{
	// Updates the placeholder of [shortcuts] global_actions to reflect the active pathway's default
	// ("state_report" in VP-V, none in Original), keeping the text empty when the key is absent so
	// an absent key is never written (GEM-037).
	void RefreshGlobalActionsPlaceholder(QWidget* root, bool vpvOn);

	// The cards, in page order. Each control is created once per editor shell.
	std::vector<QWidget*> BothPathwayCards(ConfigPageHost::EditorKit& kit);
}
