#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Bug hunt 3 (2026-10-18, hunter D): the shutdown paths the player's dialog
// did not cover, kept as small pieces the CPU tests can drive.
//
//  SH1  Windows shutdown / logoff. WM_QUERYENDSESSION is answered "yes" (VP
//       never blocks the session from ending) and nothing is torn down yet,
//       because the end can still be cancelled. WM_ENDSESSION with wParam
//       TRUE runs the bounded close path in place - the process may be ended
//       at any moment after the handler returns: the RS-232 test is cancelled
//       (it puts the light back), the LED drive is stopped on every renderer
//       (full light, LDMM 0, CNTR 50 - each bounded), the target-only display
//       topology is restored, and the crash state is marked clean. With
//       wParam FALSE (the end was cancelled) nothing happens.
//  SH2  IDCANCEL (Esc / Shift+Esc reaching the dialog manager) is a close
//       request routed through OnClose, never CDialog::OnCancel's EndDialog.
//  SH3  ExitGuard bounds the dialog's destruction: if it does not finish in
//       time the guard's action ends the process with the crash state kept.
//  SH4  A step guarded by a timed mutex is skipped, not waited for, when
//       another thread holds the mutex past the bound.
#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace SessionEnd
{
	struct Steps
	{
		std::function<void(const char* phase)> log;
		std::function<void()> cancelProjectorTest;  // bounded; restores the light itself
		std::function<void()> restoreLed;           // bounded LED drive stop on every renderer
		std::function<bool()> restoreTopology;      // bounded (timed lock)
		std::function<void(bool clean)> markCrashState;
		std::function<void()> finish;               // e.g. flush the log, end the process
	};

	struct Outcome
	{
		bool handled = false;   // the message was WM_QUERYENDSESSION / WM_ENDSESSION
		LRESULT result = 0;
		bool ranClosePath = false;
	};

	// The order of the close path (also the order the test checks).
	inline void RunClosePath(Steps& steps)
	{
		if (steps.log) steps.log("session-end close path: begin");
		if (steps.cancelProjectorTest) steps.cancelProjectorTest();
		if (steps.restoreLed) steps.restoreLed();
		bool topology = true;
		if (steps.restoreTopology) topology = steps.restoreTopology();
		if (steps.log) steps.log(topology ? "session-end close path: topology restored or not needed" :
			"session-end close path: topology restore pending (kept for the next start)");
		if (steps.markCrashState) steps.markCrashState(true);
		if (steps.log) steps.log("session-end close path: crash state marked clean");
		if (steps.finish) steps.finish();
	}

	// Handles the two session messages; anything else is not handled. A close
	// path that already ran is not run twice.
	inline Outcome Handle(UINT message, WPARAM wParam, LPARAM lParam, Steps& steps, bool& alreadyRan)
	{
		(void)lParam;
		Outcome outcome;
		if (message == WM_QUERYENDSESSION)
		{
			outcome.handled = true;
			outcome.result = TRUE;   // never veto the end of the session
			if (steps.log) steps.log("session-end query: allowed (no teardown until WM_ENDSESSION)");
			return outcome;
		}
		if (message != WM_ENDSESSION)
			return outcome;
		outcome.handled = true;
		outcome.result = 0;
		if (!wParam)
		{
			if (steps.log) steps.log("session-end cancelled: VP keeps running");
			return outcome;
		}
		if (alreadyRan)
			return outcome;
		alreadyRan = true;
		RunClosePath(steps);
		outcome.ranClosePath = true;
		return outcome;
	}
}

namespace ApplicationShutdownPolicy
{
	// SH2: every IDCANCEL the dialog receives (the dialog manager turns Esc,
	// Shift+Esc or an unbound Esc into one) is a close request: it goes
	// through OnClose (deadline, watchdog, LED and topology restore), never
	// straight to EndDialog.
	inline bool RoutesCancelThroughClose(UINT commandId)
	{
		return commandId == IDCANCEL;
	}
}

namespace ShutdownDeadline
{
	// SH3: the dialog's destructor (capture release, member destructors) after
	// the bounded worker joins. Past this the process ends with the crash
	// state kept.
	constexpr uint32_t DialogDestroyTimeoutMs = 10000u;
	// SH4: how long a forced exit waits for the display-topology mutex before
	// skipping that step (another thread may be blocked in SetDisplayConfig).
	constexpr uint32_t TopologyLockTimeoutMs = 2000u;
	// SH5: how long the close paths wait for a cancelled RS-232 test to put the
	// light back (its read-back command timeout plus margin).
	constexpr uint32_t ProjectorTestCancelTimeoutMs = 4000u;

	// SH4: runs `step` holding `mutex` when it can be taken within timeoutMs;
	// otherwise skips it (skipped = true) and returns false.
	inline bool RunWithTimedLock(std::timed_mutex& mutex, uint32_t timeoutMs,
		const std::function<bool()>& step, bool& skipped)
	{
		skipped = false;
		std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
		if (!lock.try_lock_for(std::chrono::milliseconds(timeoutMs)))
		{
			skipped = true;
			return false;
		}
		return step ? step() : true;
	}

	// SH3: runs `onExpiry` on its own thread unless Cancel() is called within
	// timeoutMs. Destroying the guard cancels it.
	class ExitGuard
	{
	public:
		ExitGuard(uint32_t timeoutMs, std::function<void()> onExpiry)
			: m_state(std::make_shared<State>())
		{
			auto state = m_state;
			try
			{
				m_thread = std::thread([state, timeoutMs, onExpiry]()
				{
					std::unique_lock<std::mutex> lock(state->mutex);
					if (state->wake.wait_for(lock, std::chrono::milliseconds(timeoutMs),
						[&state] { return state->cancelled; }))
						return;
					state->fired = true;
					lock.unlock();
					if (onExpiry) onExpiry();
				});
			}
			catch (...) {}
		}
		~ExitGuard() { Cancel(); }
		ExitGuard(const ExitGuard&) = delete;
		ExitGuard& operator=(const ExitGuard&) = delete;

		// True when cancelled before the action ran.
		bool Cancel()
		{
			bool fired = false;
			{
				std::lock_guard<std::mutex> lock(m_state->mutex);
				m_state->cancelled = true;
				fired = m_state->fired;
			}
			m_state->wake.notify_all();
			if (m_thread.joinable())
			{
				if (fired) m_thread.detach();   // the action may end the process
				else m_thread.join();
			}
			return !fired;
		}

	private:
		struct State
		{
			std::mutex mutex;
			std::condition_variable wake;
			bool cancelled = false;
			bool fired = false;
		};
		std::shared_ptr<State> m_state;
		std::thread m_thread;
	};
}
