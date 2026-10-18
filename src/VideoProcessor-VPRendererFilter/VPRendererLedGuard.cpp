#include <pch.h>

#include "VPRendererLedGuard.h"
#include "VPRendererPlugin.h"

#include <DebugLog.h>

#include <atomic>
#include <mutex>
#include <thread>


namespace VPRendererFilter
{
	namespace
	{
		std::atomic<HWND> g_watchedRoot{ nullptr };
		std::once_flag g_watcherStarted;

		void EndHeldDrive(const char* why)
		{
			const LoadedPlugin& plugin = GetVPRendererPlugin();
			if (!plugin.IsLoaded())
				return;
			const auto endHeld = reinterpret_cast<VideoProcessorLibplaceboEndHeldLedDriveFn>(
				GetProcAddress(plugin.module, VP_LIBPLACEBO_END_HELD_LED_DRIVE_EXPORT));
			if (!endHeld)
				return;   // an older plugin without hold-over: nothing is ever held
			const int ended = endHeld();
			DebugLog::Log("VP Renderer filter: %s; held LED drive %s", why,
				ended ? "ended (full light restored)" : "none");
		}

		void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
			LONG idChild, DWORD, DWORD)
		{
			if (!IsRootDestroyed(g_watchedRoot.load(), event, hwnd, idObject, idChild))
				return;
			g_watchedRoot = nullptr;
			EndHeldDrive("the player window closed");
		}

		void WatcherThread()
		{
			// Out of context: the callback runs on this thread's message loop,
			// for this process's windows only.
			const HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY,
				nullptr, &OnWinEvent, GetCurrentProcessId(), 0,
				WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD);
			if (!hook)
			{
				DebugLog::Log("VP Renderer filter: cannot watch the player window (error %lu); a player "
					"closed during the LED hold-over keeps the held LED level", GetLastError());
				return;
			}
			MSG msg;
			while (GetMessageW(&msg, nullptr, 0, 0) > 0)
				DispatchMessageW(&msg);
			UnhookWinEvent(hook);
		}
	}


	void WatchPlayerWindowForLedHoldover(HWND owner)
	{
		if (!owner)
			return;
		const HWND root = GetAncestor(owner, GA_ROOT);
		g_watchedRoot = root ? root : owner;
		std::call_once(g_watcherStarted, []()
		{
			try
			{
				// The watcher's code lives in this module: pin it so a
				// CoFreeUnusedLibraries between graphs cannot unload it.
				HMODULE pinned = nullptr;
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
					reinterpret_cast<LPCWSTR>(&WatcherThread), &pinned);
				std::thread(&WatcherThread).detach();
			}
			catch (const std::exception& e)
			{
				DebugLog::Log("VP Renderer filter: player window watcher not started: %s", e.what());
			}
		});
	}
}
