// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// VP-V rows that used to sit on Bill's General page (CC-GEM P2: GEM-060 / GEM-222, owner Q13; owner rule
// 2026-11-01: Bill's pages stay exactly his). Built through the editor's own field helpers (EditorKit):
// same object names, keys, defaults and save / load round trip as on the General page; labels and
// tooltips unchanged. The VP-V page shows them as unit cards (Host::unitCards):
//
//   unit "input"   Capture fixes under test: [general] magewell_recover_faults, capture_display_fixes
//                  (were in General > General behavior). On the Input tab, so the Original pathway greys
//                  them (their runtime is off on Original).
//   "both"         Room lights: [ambient] mode, room, lux_lit, lux_dark, hold_s, port, baud (were the
//                  General > Room lights card); room lights still steer the profile rules on Original.
//                  File playback (VP Renderer filter): [general] file_av_offset_ms, file_led_holdover_ms
//                  (were the General > File playback card).
//
// Compiled in VpvGeneralPageCards.cpp. UI thread only.

#include <QString>
#include <VP-V/modules/control/config/config_pages/VpvSection.h>
#include <vector>

namespace ConfigPageHost
{
	class EditorKit;
}

namespace Vpv
{
	// The unit cards of `unitId`: `taken` (the cards Bill's pages built, VpvConfigPages.h) followed by this
	// file's cards for that unit (none for other units).
	std::vector<UnitCard> WithGeneralPageCards(ConfigPageHost::EditorKit& kit, const QString& unitId, std::vector<UnitCard> taken);
}
