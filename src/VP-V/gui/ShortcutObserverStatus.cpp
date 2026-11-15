// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S-3f b (owner 2026-11-15 21:28: the default lives in VP-V code): the default report key's background
// hook and the static registration of its extension (refs/112 pattern P4, as ExeLogRing.cpp). Compiled
// into the VideoProcessor-GUI executable through src/VP-V/build/VideoProcessor-GUI.items.props with the
// GUI's precompiled header (MFC: AfxGetMainWnd is the dialog the report command goes to), so the dialog
// needs no call line of its own. DialogExtension::Register only appends to a function-local static
// registry, so running it during static initialization (before the dialog creates its timers) keeps the
// registration contract; the configuration is read on the ticks, never during static initialization.
#include "pch.h"

#include "ShortcutObserverStatus.h"

#include <VP-V/gui/VpShortcuts.h>

#include <atomic>

namespace
{
	// The one hook of the default report key. A low-level keyboard hook needs a thread with a message
	// loop, so it runs on its own thread, never on the UI thread (the dialog's GlobalShortcutObserver
	// pattern). The UI thread writes target, plan and keyDown only while no hook thread runs; the hook
	// thread reads them, and keyDown is its own while it runs.
	struct HookState
	{
		HWND target = nullptr;
		ShortcutObserverStatus::Plan plan;
		bool keyDown = false;   // the plan's key is held: auto-repeat acts once
		std::atomic<HHOOK> hook{ nullptr };
	};
	HookState g_hook;

	// VP's Config in front: a Qt top-level window with Config's title, or a window it owns (the dialog's
	// IsConfigurationEditorTopLevel test without its installation-path match, which only narrows it).
	bool ConfigurationEditorInFront(HWND foreground)
	{
		const HWND root = foreground ? ::GetAncestor(foreground, GA_ROOTOWNER) : nullptr;
		wchar_t className[128] = {};
		wchar_t title[512] = {};
		return root != nullptr &&
			::GetClassNameW(root, className, ARRAYSIZE(className)) > 0 && _wcsnicmp(className, L"Qt", 2) == 0 &&
			::GetWindowTextW(root, title, ARRAYSIZE(title)) > 0 &&
			ConfigurationLiveApply::IsConfigurationEditorTitle(title);
	}

	// No logging on this thread (it may still run while statics are destroyed at exit); the report that
	// the command starts is logged by the dialog.
	LRESULT CALLBACK HookProcedure(int code, WPARAM message, LPARAM parameter)
	{
		if (code == HC_ACTION)
		{
			const auto* keyEvent = reinterpret_cast<const KBDLLHOOKSTRUCT*>(parameter);
			const unsigned short virtualKey = static_cast<unsigned short>(keyEvent->vkCode);
			if (virtualKey == g_hook.plan.key)
			{
				if (message == WM_KEYUP || message == WM_SYSKEYUP)
					g_hook.keyDown = false;
				else if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !g_hook.keyDown)
				{
					g_hook.keyDown = true;
					const bool control = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
					const bool leftAlt = (::GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
					const bool rightAlt = (::GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
					const bool alt = leftAlt || rightAlt || (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
					const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
					if (ShortcutObserverStatus::Matches(g_hook.plan, virtualKey, control,
						leftAlt || (alt && !rightAlt), rightAlt, shift))
					{
						const HWND foreground = ::GetForegroundWindow();
						DWORD foregroundProcessId = 0;
						if (foreground)
							::GetWindowThreadProcessId(foreground, &foregroundProcessId);
						const bool vpForeground = foregroundProcessId == ::GetCurrentProcessId();
						if (ShortcutObserverStatus::Dispatches(vpForeground,
							!vpForeground && ConfigurationEditorInFront(foreground)))
							::PostMessageW(g_hook.target, WM_COMMAND, MAKEWPARAM(ID_COMMAND_STATE_REPORT, 0), 0);
					}
				}
			}
		}
		return ::CallNextHookEx(g_hook.hook.load(), code, message, parameter);
	}

	DWORD WINAPI HookThread(void* ready)
	{
		MSG message = {};
		// The thread's queue exists before the ready signal.
		::PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
		const HHOOK hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, HookProcedure, ::GetModuleHandleW(nullptr), 0);
		g_hook.hook.store(hook);
		::SetEvent(static_cast<HANDLE>(ready));
		if (!hook)
			return 0;
		while (::GetMessageW(&message, nullptr, 0, 0) > 0)
		{
			::TranslateMessage(&message);
			::DispatchMessageW(&message);
		}
		::UnhookWindowsHookEx(hook);
		g_hook.hook.store(nullptr);
		return 0;
	}

	class ReportKeyHook final : public ShortcutObserverStatus::IObserver
	{
	public:
		~ReportKeyHook() override { Join(); }   // no log: statics may be gone at exit

		bool Start(const ShortcutObserverStatus::Plan& plan) override
		{
			Stop();
			if (!plan.wanted)
				return false;
			CWnd* dialog = AfxGetMainWnd();
			const HWND target = dialog ? dialog->GetSafeHwnd() : nullptr;
			if (!target)
			{
				DebugLog::Log("Default report key: background hook not started (no dialog window)");
				return false;
			}
			g_hook.target = target;
			g_hook.plan = plan;
			g_hook.keyDown = false;
			const HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
			m_thread = ready ? ::CreateThread(nullptr, 0, HookThread, ready, 0, &m_threadId) : nullptr;
			const bool running = m_thread != nullptr &&
				::WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0 && g_hook.hook.load() != nullptr;
			if (!running)
				Join();   // the thread has ended afterwards, so it no longer uses `ready`
			if (ready)
				::CloseHandle(ready);
			// VPV-FALLBACK global shortcuts: an unavailable hook is the Extension's "global shortcuts" Fallbacks entry (ShortcutObserverStatus::Note)
			DebugLog::Log("Default report key: background hook %s (modifiers=0x%02x key=0x%02x; "
				"[shortcuts] global_actions absent, VP-V pathway)", running ? "started" : "unavailable",
				static_cast<unsigned>(plan.modifiers), static_cast<unsigned>(plan.key));
			return running;
		}

		void Stop() override
		{
			const bool wasRunning = Running();
			Join();
			if (wasRunning)
				DebugLog::Log("Default report key: background hook stopped");
		}

		bool Running() const override { return m_thread != nullptr && g_hook.hook.load() != nullptr; }

	private:
		// Ends the hook thread. WM_QUIT is posted again until the thread ends (a post can fail while the
		// thread is still starting), then the shared state is cleared.
		void Join()
		{
			if (m_thread)
			{
				while (::WaitForSingleObject(m_thread, 0) == WAIT_TIMEOUT)
				{
					::PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
					::WaitForSingleObject(m_thread, 50);
				}
				::CloseHandle(m_thread);
			}
			m_thread = nullptr;
			m_threadId = 0;
			g_hook.hook.store(nullptr);
			g_hook.target = nullptr;
			g_hook.plan = ShortcutObserverStatus::Plan();
			g_hook.keyDown = false;
		}

		HANDLE m_thread = nullptr;
		DWORD m_threadId = 0;
	};

	// Every tick: the configuration file the dialog reads (ConfigFile returns an unchanged file from its
	// cache). False = not readable now (none yet, or being replaced): the plan in force stays.
	bool LoadConfiguration(ConfigFile& config)
	{
		return config.Load();
	}

	struct ShortcutObserverStatusRegistration
	{
		ReportKeyHook hook;
		ShortcutObserverStatus::Extension extension{
			ShortcutObserverStatus::FileFactsReader(LoadConfiguration), hook, ShortcutObserverStatus::CurrentNote() };
		ShortcutObserverStatusRegistration() { DialogExtension::Register(&extension); }
	};
	ShortcutObserverStatusRegistration g_shortcutObserverStatusRegistration;
}
