#pragma once

#include <atomic>

// Post-merge audit 2026-10-17: [general] capture_display_fixes (default on), the
// restore switch for two behaviour changes in Bill's capture / display code made
// by the 2026-10-17 bug hunt:
//  - the target-only display session's maximum mode ranks by area, then refresh,
//    and skips interlaced modes (DisplayTopologySession); off = refresh first,
//    then area, interlaced accepted (the earlier ranking);
//  - DeckLink Rec.601 by active lines: 486 / 480 / 525 -> REC_601_525, 576 / 625
//    -> REC_601_625 (BlackMagicDeckLinkTranslate); off = the earlier mapping, where
//    486 lines (a mode the card reports for a moment, Dennis Fleurbaaij's guard)
//    gives UNKNOWN and 576 the analog PAL entry.
// Process-wide; set by the GUI from the configuration (start-up and Apply).
namespace CaptureDisplayFixes
{
	inline std::atomic<bool>& Flag()
	{
		static std::atomic<bool> on{ true };
		return on;
	}
	inline bool Enabled() { return Flag().load(std::memory_order_relaxed); }
	// True when the value changed.
	inline bool Set(bool on) { return Flag().exchange(on, std::memory_order_relaxed) != on; }
}
