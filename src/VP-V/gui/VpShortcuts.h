#pragma once

#include <cstddef>
#include <cstring>
#include <vector>

// VP's keyboard shortcuts (WP-11): the command ids, the [shortcuts] keys and where each sits in
// the dialog's binding order, out of VideoProcessorDlg.cpp and Resource.h (Bill's files).
//
// The ids keep their old values (tools/test_state_report_onscreen.ps1 posts 33207). They sit in
// a block reserved for VP below ID_COMMAND_UNIFIED_PROFILE_FIRST (33300); the dialog routes the
// whole block to VpvGui::DispatchCommand with one ON_COMMAND_RANGE. Compiled defaults stay in
// ShortcutDefaults.h (one table for VP and Config, collision-checked by ShortcutDefaultsTests).
//
// The binding order matters: on a duplicate user binding the EARLIER command keeps the chord. So
// each VP shortcut names the key it follows, and Merge() rebuilds exactly the old single table.
//
// Header-only, pure C++14 (no Windows): the dialog and the CPU tests share it.
#define ID_COMMAND_VP_FIRST             33206
#define ID_COMMAND_ROOM_LIGHTS          33206
#define ID_COMMAND_STATE_REPORT         33207
#define ID_COMMAND_RAW_CAPTURE_TOGGLE   33208
#define ID_COMMAND_DV_COMPARE_CAPTURE   33209   // feat/dv-l8 WIP: handled by its own ON_COMMAND, ahead of the VP range
#define ID_COMMAND_VP_LAST              33299

namespace VpvShortcuts
{
	struct Definition
	{
		const char* configKey;
		unsigned short command;
		const char* after;   // the [shortcuts] key this one follows in the binding order
	};

	// In the order they are inserted (a later entry may follow an earlier VP entry).
	constexpr Definition kDefinitions[] = {
		// Report-a-problem (StateReport.h); default Ctrl+Alt+D.
		{ "state_report", ID_COMMAND_STATE_REPORT, "capture_rendered_output" },
		// Raw capture recording (RawCaptureRecorder.h; needs VP_RAW_CAPTURE_PATH).
		{ "raw_capture",  ID_COMMAND_RAW_CAPTURE_TOGGLE, "state_report" },
		// Stage 9 room lights backup key (settings first).
		{ "room_lights",  ID_COMMAND_ROOM_LIGHTS, "reapply_rules" },
	};

	inline bool IsVpCommand(unsigned int command)
	{
		return command >= ID_COMMAND_VP_FIRST && command <= ID_COMMAND_VP_LAST;
	}

	// The host's table with VP's shortcuts inserted after their anchors (appended when an anchor
	// is missing). Def is the host's {configKey, command} struct.
	template <class Def, size_t N>
	std::vector<Def> Merge(const Def (&host)[N])
	{
		std::vector<Def> all(host, host + N);
		for (const Definition& vp : kDefinitions)
		{
			Def def = {};
			def.configKey = vp.configKey;
			def.command = static_cast<decltype(def.command)>(vp.command);
			size_t at = all.size();
			for (size_t i = 0; i < all.size(); ++i)
				if (std::strcmp(all[i].configKey, vp.after) == 0)
				{
					at = i + 1;
					break;
				}
			all.insert(all.begin() + static_cast<std::ptrdiff_t>(at), def);
		}
		return all;
	}
}
