#pragma once

#include <array>
#include <cstdint>

// The measurement layer's per-frame statistics (split out of LedController.h in the LED module
// re-cut, migration-m2-plan.md "LED module boundaries"): drive.led_measure produces them, the
// controllers in drive.led_compensation consume them.
namespace LedDrive
{
	// Per-frame statistics from the measurement layer, in limited-range code
	// units (black 16, white 235) like the script's screenshot.
	struct FrameStats
	{
		double apl = 16.0;
		// Brightness of the brightest part of the picture (the 99.5th
		// percentile of 3x3 block means).
		double highlight = 16.0;
		double highlightArea = 0.0;
		// The lowest highlight of the last highlight_persist_ms: a flash shorter
		// than that never raises it.
		double sustainedHighlight = 16.0;
		// The lowest APL of the last apl_persist_ms (since the last cut, when
		// that is more recent): mode: compensate aims the LED at it, so a flash
		// never makes the LED chase it; a fall, or a cut, acts at once.
		double sustainedApl = 16.0;
		// The script's max of its downscale (the largest area mean over the
		// whole picture, led_peak_full_coverage; off: the brightest 3x3
		// block): the highlight test of mode: script.
		double maximum = 16.0;
		bool cut = false;
		bool fade = false;
		// Block-mean histogram in code units and the number of blocks measured.
		std::array<uint32_t, 256> histogram{};
		uint32_t blocks = 0;
	};
}
