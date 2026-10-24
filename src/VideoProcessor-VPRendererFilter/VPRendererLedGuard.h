#pragma once

// VP Renderer DirectShow filter: ends a held LED drive when the player closes.
//
// Between files the renderer DLL parks its LED drive for file_led_holdover_ms
// (VP-V/modules/drive/led_compensation/LedDriveHoldover.h) so MPC's per-file graph rebuild does not flash the
// projector to full light. The holder is deliberately never destroyed (no
// thread join or projector command under the loader lock), so a player that
// exits inside the hold-over window would leave the projector at the held LED
// level (CNTR above need). This guard watches the player's top-level window
// (the root of the IVideoWindow owner): when it is destroyed - before the
// process exits - the plugin's optional VideoProcessorLibplaceboEndHeldLedDrive
// export stops the held drive through its normal path (full light restored).
//
// One process-wide watcher thread with its own message loop and an
// out-of-context WinEvent hook; it is never joined (the process ends it).

#include <Windows.h>


namespace VPRendererFilter
{
	// Remember the player's top-level window for `owner` (null: ignored) and
	// start the watcher on first use. Cheap; called from put_Owner.
	void WatchPlayerWindowForLedHoldover(HWND owner);

	// Pure decision (tests): the event is the destruction of the watched root.
	inline bool IsRootDestroyed(HWND watchedRoot, DWORD event, HWND hwnd, LONG idObject, LONG idChild)
	{
		return watchedRoot != nullptr && event == EVENT_OBJECT_DESTROY && hwnd == watchedRoot &&
			idObject == OBJID_WINDOW && idChild == CHILDID_SELF;
	}
}
