// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Renderer failure retry, fullscreen teardown, shutdown deadline/watchdog, forced exit,
// LED shutdown restore, Windows session-end handling and bounded shutdown worker waits,
// moved out of Bill's VideoProcessorDlg.cpp into compiled VP-V GUI code.
#include "pch.h"

#include "VideoProcessorDlg.h"
#include "VideoProcessorApp.h"

#include <GuiLifecyclePolicy.h>
#include <SessionEndShutdown.h>
#include <VP-V/modules/control/diagnostics/CrashState.h>
#include <VP-V/modules/drive/projector_control/LedShutdownRestore.h>
#include <VP-V/modules/drive/projector_control/SerialPort.h>
// Bug hunt 2026-10-17: every renderer-failure site used to clear the
// fullscreen check, after which UpdateState destroyed the fullscreen host and
// restored the pre-session display topology, and nothing retried. Keep the
// intent (the check stays set; the host and topology stay) and schedule a
// bounded retry. renderer_failure_keep_fullscreen=false keeps the old path.
void CVideoProcessorDlg::HandleRendererFailureFullscreen(const char* site)
{
	if (!m_rendererFailureKeepFullscreen)
	{
		m_rendererFullscreenCheck.SetCheck(FALSE);
		return;
	}
	const bool fullscreenIntent = m_rendererFullscreenCheck.GetCheck() != FALSE;
	if (fullscreenIntent && !m_fullscreenSuspendedByRendererFailure)
	{
		m_fullscreenSuspendedByRendererFailure = true;
		DebugLog::Log(
			"Renderer failure fullscreen suspended: site=%s intent=kept "
			"topology=retained host=%p",
			site,
			m_fullScreenVideoWindow ? m_fullScreenVideoWindow->GetHWND() : nullptr);
	}
	m_rendererRenderingSinceTick = 0;
	if (m_queueLaunchContractTerminalFailure)
	{
		// That failure asks for Restart Renderer explicitly; do not retry it.
		m_rendererFailureNextRetryTick = 0;
		return;
	}
	// Bug hunt 2 (M5): several sites report one failure; schedule once. A
	// second report must not push the due retry later.
	if (!RendererFailureRetryShouldSchedule(m_rendererFailureNextRetryTick))
	{
		DebugLog::Log(
			"Renderer failure retry already scheduled: site=%s attempt=%u",
			site, m_rendererFailureRetryAttempt + 1);
		return;
	}
	if (!RendererFailureRetryAllowed(m_rendererFailureRetryAttempt))
	{
		// Post-merge audit 2026-10-17: bounded; Restart Renderer stays available.
		// Logged once: the count then moves past the cap.
		if (m_rendererFailureRetryAttempt == RendererFailureRetryMaxAttempts)
		{
			DebugLog::Log(
				"Renderer failure retry stopped: site=%s after %u automatic attempt(s); "
				"use Restart Renderer", site, m_rendererFailureRetryAttempt);
			++m_rendererFailureRetryAttempt;
		}
		m_rendererFailureNextRetryTick = 0;
		return;
	}
	const uint32_t delayMs =
		RendererFailureRetryDelayMs(m_rendererFailureRetryAttempt);
	m_rendererFailureNextRetryTick = GetTickCount64() + delayMs;
	DebugLog::Log(
		"Renderer failure retry scheduled: site=%s attempt=%u delay_ms=%u "
		"fullscreen_intent=%d",
		site, m_rendererFailureRetryAttempt + 1, delayMs,
		fullscreenIntent ? 1 : 0);
}


void CVideoProcessorDlg::ResetRendererFailureRetry(const char* reason)
{
	if (m_rendererFailureRetryAttempt == 0 &&
		m_rendererFailureNextRetryTick == 0 &&
		!m_fullscreenSuspendedByRendererFailure)
		return;
	DebugLog::Log(
		"Renderer failure retry cleared: reason=%s attempts=%u "
		"fullscreen_suspended=%d",
		reason, m_rendererFailureRetryAttempt,
		m_fullscreenSuspendedByRendererFailure ? 1 : 0);
	m_rendererFailureRetryAttempt = 0;
	m_rendererFailureNextRetryTick = 0;
	m_fullscreenSuspendedByRendererFailure = false;
}


// Called from the 1-second UI timer. Retries only while the renderer is FAILED
// with no renderer object, no lifecycle work in flight and a valid capture
// state; otherwise the retry stays due and is re-evaluated on the next tick.
void CVideoProcessorDlg::PollRendererFailureRetry(ULONGLONG now)
{
	const bool rendering = m_videoRenderer &&
		m_rendererState == RendererState::RENDERSTATE_RENDERING;
	if (!rendering)
		m_rendererRenderingSinceTick = 0;
	else if (RendererFailureRetryShouldClear(
		rendering, m_rendererRenderingSinceTick, now))
		ResetRendererFailureRetry("renderer-rendering-sustained");
	if (m_rendererFailureNextRetryTick == 0 || now < m_rendererFailureNextRetryTick)
		return;
	if (!m_rendererFailureKeepFullscreen || m_wantToTerminate ||
		m_queueLaunchContractTerminalFailure)
	{
		m_rendererFailureNextRetryTick = 0;
		return;
	}
	if (m_rendererState != RendererState::RENDERSTATE_FAILED)
	{
		// Something else (new capture state, Restart) already moved on.
		m_rendererFailureNextRetryTick = 0;
		return;
	}
	if (m_videoRenderer || m_rendererConstructionActive ||
		m_rendererRetirementPending || m_failedRendererRetirement ||
		!m_captureDevice ||
		m_captureDeviceState != CaptureDeviceState::CAPTUREDEVICESTATE_CAPTURING ||
		!m_captureDeviceVideoState || !m_captureDeviceVideoState->valid)
		return;

	m_rendererFailureNextRetryTick = 0;
	++m_rendererFailureRetryAttempt;
	DebugLog::Log(
		"Renderer failure automatic retry: attempt=%u fullscreen_intent=%d "
		"fullscreen_suspended=%d",
		m_rendererFailureRetryAttempt,
		m_rendererFullscreenCheck.GetCheck() ? 1 : 0,
		m_fullscreenSuspendedByRendererFailure ? 1 : 0);
	// Same path as the Restart Renderer button.
	OnBnClickedRendererRestart();
}


// Bug hunt 2 (2026-10-17, H1): one shutdown deadline for every way the exit
// can block. The first version covered only m_shutdownRendererRetirementBlocked
// and was polled only from that branch, so a retirement worker that never
// returned (m_rendererRetirementPending for ever), a construction, a capture
// stop that never reached READY or a renderer that never stopped kept VP
// alive with no topology or LED restore. Now the deadline is armed when the
// close is first requested and polled from the UI timers whatever blocks.
// Bug hunt 2 (2026-10-17, M4): fullscreen off used to be honoured only past
// UpdateState's capture-state returns, so with capture lost/stopped (and a
// failed renderer keeping the host under renderer_failure_keep_fullscreen) the
// host and the target-only topology stayed until capture returned or exit.
void CVideoProcessorDlg::TeardownUnwantedFullscreenHost(const char* site)
{
	if (m_wantToTerminate || m_rendererFullscreenCheck.GetCheck() ||
		!m_fullScreenVideoWindow || m_preserveFullscreenHostForProfileRestart ||
		m_videoRenderer || m_rendererRetirementPending ||
		m_failedRendererRetirement || m_rendererConstructionActive ||
		m_fullscreenRetargetPending)
		return;
	DebugLog::Log("Fullscreen host removed: site=%s capture_state=%d",
		site, static_cast<int>(m_captureDeviceState));
	FullScreenVideoWindowDestroy();
	videoProcessorApp.RestoreDisplayTopology("fullscreen-off");
}


ShutdownDeadline::Blockers CVideoProcessorDlg::CurrentShutdownBlockers() const
{
	ShutdownDeadline::Blockers blockers;
	blockers.retirementPending = m_rendererRetirementPending;
	blockers.failedRetirement = m_failedRendererRetirement != nullptr;
	blockers.retirementShutdownBlocked = m_shutdownRendererRetirementBlocked;
	blockers.constructionActive = m_rendererConstructionActive;
	blockers.captureStopping = m_captureStopInProgress ||
		m_captureDeviceState == CaptureDeviceState::CAPTUREDEVICESTATE_STOPPING;
	blockers.rendererPresent = m_videoRenderer != nullptr;
	blockers.capturePresent = m_captureDevice != nullptr;
	return blockers;
}


void CVideoProcessorDlg::ArmShutdownDeadline(const char* site)
{
	const ULONGLONG now = GetTickCount64();
	if (!m_shutdownDeadline.Arm(now, ShutdownDeadline::ForcedExitDeadlineMs))
		return;
	DebugLog::Log(
		"Application shutdown deadline armed: site=%s deadline_ms=%u "
		"watchdog_ms=%u",
		site, ShutdownDeadline::ForcedExitDeadlineMs,
		ShutdownDeadline::ForcedExitDeadlineMs + ShutdownDeadline::WatchdogGraceMs);

	// Backstop for a blocked UI thread (no timer runs then): a detached thread
	// forces the exit after the deadline plus a grace. The destructor cancels
	// it under the same mutex, so it never touches a destroyed dialog; while it
	// runs the forced exit, the destructor waits for the process to end.
	auto state = std::make_shared<ShutdownWatchdogState>();
	state->dialog = this;
	try
	{
		std::thread([state]()
			{
				std::unique_lock<std::mutex> lock(state->mutex);
				if (state->wake.wait_for(lock,
					std::chrono::milliseconds(ShutdownDeadline::ForcedExitDeadlineMs +
						ShutdownDeadline::WatchdogGraceMs),
					[&state]() { return state->cancelled; }))
					return;
				DebugLog::Log(
					"Application shutdown watchdog expired: the UI thread did not "
					"complete or force the exit");
				state->dialog->ForceExitProcess(
					"shutdown-watchdog", "ui-thread-unresponsive");
			}).detach();
		m_shutdownWatchdog = std::move(state);
	}
	catch (...)
	{
		DebugLog::Log("Application shutdown watchdog could not start; "
			"the UI-timer deadline remains");
	}
}


void CVideoProcessorDlg::PollShutdownDeadline(ULONGLONG now)
{
	if (!m_wantToTerminate)
		return;
	if (!m_shutdownDeadline.Armed())
		ArmShutdownDeadline("poll");
	if (m_shutdownRendererRetirementBlocked)
		m_shutdownDeadline.BringForward(now, ShutdownBlockedRetirementDeadlineMs);
	if (m_shutdownDeadline.Evaluate(now) != ShutdownDeadline::Decision::Expire)
		return;
	const ShutdownDeadline::Blockers blockers = CurrentShutdownBlockers();
	DebugLog::Log(
		"Application shutdown deadline expired: blocking=%s elapsed_ms=%llu "
		"retirement_pending=%d retirement_failed=%d retirement_blocked=%d "
		"construction=%d capture_state=%d capture_stop_in_progress=%d "
		"renderer=%d renderer_state=%d retiring=%S action=forced-exit",
		ShutdownDeadline::Describe(blockers),
		static_cast<unsigned long long>(now - m_shutdownDeadline.ArmedAt()),
		blockers.retirementPending ? 1 : 0, blockers.failedRetirement ? 1 : 0,
		blockers.retirementShutdownBlocked ? 1 : 0,
		blockers.constructionActive ? 1 : 0,
		static_cast<int>(m_captureDeviceState),
		m_captureStopInProgress ? 1 : 0,
		blockers.rendererPresent ? 1 : 0, static_cast<int>(m_rendererState),
		static_cast<LPCTSTR>(m_retiringRendererName));
	ForceExitProcess("shutdown-deadline", ShutdownDeadline::Describe(blockers));
}


// Forced exit, done in place (it does not depend on DoModal returning: the
// UI thread may be inside a nested call that never returns). Every step is
// bounded; the process is then terminated, so no destructor re-enters a
// blocked backend and no windowless process is left behind.
void CVideoProcessorDlg::ForceExitProcess(const char* reason, const char* blocking)
{
	if (m_forcedShutdownStarted.exchange(true))
	{
		// Another thread is already forcing the exit. If it is itself stuck,
		// end the process after the grace anyway.
		Sleep(ShutdownDeadline::WatchdogGraceMs);
		TerminateProcess(GetCurrentProcess(), ShutdownDeadline::ForcedExitCode);
		for (;;)
			Sleep(INFINITE);
	}
	const bool uiThread = ::GetCurrentThreadId() == m_uiThreadId;
	const ShutdownDeadline::ForcedExitPlan plan =
		ShutdownDeadline::PlanForcedExit(ShutdownDeadline::Blockers{});
	DebugLog::Log("Forced shutdown: reason=%s blocking=%s thread=%s",
		reason, blocking, uiThread ? "ui" : "watchdog");
	try
	{
		CrashState::Global().SetValue("exit",
			std::string("forced shutdown: ") + reason + " (" + blocking + ")");
	}
	catch (...) {}

	// 0. Bug hunt 3 (SH5): a running RS-232 test is cancelled; it puts the
	//    light back itself (bounded).
	CancelProjectorTestForExit(ShutdownDeadline::ProjectorTestCancelTimeoutMs, "forced-shutdown");

	// 1. LED (Dynamic Drive) restore on every renderer that may hold the
	//    drive (RestoreLedForExit).
	if (plan.restoreLed)
	{
		std::vector<std::shared_ptr<IVideoRenderer>> renderers =
			RestoreLedForExit("Forced shutdown", uiThread);
		// Deliberately leaked: their backends may be blocked, and no destructor
		// may re-enter them. Process termination reclaims them.
		for (std::shared_ptr<IVideoRenderer>& renderer : renderers)
			new std::shared_ptr<IVideoRenderer>(std::move(renderer));
	}

	// 2. Target-only display topology restore (no-op without a session).
	//    Bug hunt 3 (SH4): bounded - the UI thread may be the one stuck in
	//    SetDisplayConfig holding the restore mutex; then the step is skipped.
	if (plan.restoreTopology)
	{
		bool skipped = false;
		const bool restored = videoProcessorApp.RestoreDisplayTopologyBounded(
			"forced-shutdown", ShutdownDeadline::TopologyLockTimeoutMs, &skipped);
		DebugLog::Log("Forced shutdown topology restore: %s",
			restored ? "done-or-not-needed" : skipped ? "skipped (restore mutex busy; kept for the next start)" :
			"pending (kept for the next start)");
	}

	// 3. Crash state kept: a forced exit is not a clean exit.
	DebugLog::Log(
		"Forced shutdown: terminating process exit_code=%u crash_state=%s",
		ShutdownDeadline::ForcedExitCode, plan.keepCrashState ? "kept" : "deleted");
	CrashState::Global().Stop(!plan.keepCrashState);
	DEBUGLOG_SHUTDOWN();
	TerminateProcess(GetCurrentProcess(), ShutdownDeadline::ForcedExitCode);
	for (;;)
		Sleep(INFINITE);
}


std::vector<std::shared_ptr<IVideoRenderer>> CVideoProcessorDlg::RestoreLedForExit(
	const char* site, bool uiThread)
{
	// The LED (Dynamic Drive) restore on every renderer that may hold the
	// drive: the current one, the one in (or blocked in) the retirement
	// worker, and a retained failed retirement. Each stop runs on its own
	// thread with a bound; a renderer that holds its locks times out.
	std::vector<std::shared_ptr<IVideoRenderer>> renderers;
	const auto add = [&renderers](std::shared_ptr<IVideoRenderer> renderer)
	{
		if (renderer && std::find(renderers.begin(), renderers.end(),
			renderer) == renderers.end())
			renderers.push_back(std::move(renderer));
	};
	add(std::atomic_load_explicit(&m_videoRenderer, std::memory_order_acquire));
	{
		std::lock_guard<std::mutex> lock(m_shutdownRetiringRendererMutex);
		add(m_shutdownRetiringRenderer.lock());
	}
	if (uiThread)
		add(m_failedRendererRetirement);
	if (renderers.empty())
		DebugLog::Log("%s LED restore: no renderer reachable "
			"(an exited renderer restored its drive when it stopped)", site);
	for (const std::shared_ptr<IVideoRenderer>& renderer : renderers)
	{
		const LedDrive::BoundedRestoreResult restore = LedDrive::RunBoundedRestore(
			[renderer]() { return VpvCalls::StopLedDriveForShutdown(renderer.get()); },
			ShutdownLedRestoreTimeoutMs);
		DebugLog::Log("%s LED restore: %s (timeout_ms=%u)", site,
			LedDrive::Name(restore), ShutdownLedRestoreTimeoutMs);
	}
	return renderers;
}


bool CVideoProcessorDlg::CancelProjectorTestForExit(DWORD timeoutMs, const char* site)
{
	std::shared_ptr<ProjectorTestShared> shared;
	{
		// m_projectorTestShared is a UI-thread member; the watchdog thread
		// reads the pointer once (the shared object itself is thread-safe).
		shared = std::atomic_load_explicit(&m_projectorTestShared, std::memory_order_acquire);
	}
	if (!shared)
		return true;
	shared->cancel->store(true);
	std::unique_lock<std::mutex> lock(shared->mutex);
	const bool done = shared->doneChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs),
		[&shared]() { return shared->done; });
	DebugLog::Log("RS-232 connection test cancelled at %s: %s (timeout_ms=%lu)", site,
		done ? "finished, light put back" : "still running", timeoutMs);
	return done;
}


// Bug hunt 3 (SH2): the dialog manager turns Esc (Shift+Esc, or Esc while
// fullscreen_exit is unbound) into IDCANCEL; CDialog::OnCancel would end the
// dialog straight away, skipping OnClose's deadline, watchdog, LED and
// topology restore. Route it through OnClose instead.
void CVideoProcessorDlg::OnCancel()
{
	if (ApplicationShutdownPolicy::RoutesCancelThroughClose(IDCANCEL))
	{
		DebugLog::Log("Dialog cancel (IDCANCEL) routed through the close path");
		OnClose();
	}
}


// Bug hunt 3 (SH1): Windows shutdown / logoff (SessionEndShutdown.h).
BOOL CVideoProcessorDlg::OnQueryEndSession()
{
	SessionEnd::Steps steps;
	steps.log = [](const char* phase) { DebugLog::Log("Windows %s", phase); };
	const SessionEnd::Outcome outcome = SessionEnd::Handle(WM_QUERYENDSESSION, 0, 0, steps, m_sessionEndRan);
	return static_cast<BOOL>(outcome.result);
}


void CVideoProcessorDlg::OnEndSession(BOOL ending)
{
	SessionEnd::Steps steps;
	steps.log = [](const char* phase) { DebugLog::Log("Windows %s", phase); };
	const bool uiThread = ::GetCurrentThreadId() == m_uiThreadId;
	steps.cancelProjectorTest = [this]()
	{
		CancelProjectorTestForExit(ShutdownDeadline::ProjectorTestCancelTimeoutMs, "session-end");
	};
	steps.restoreLed = [this, uiThread]()
	{
		// Full light, LDMM 0, CNTR 50 on every reachable renderer, each
		// bounded. The renderers are leaked: the process ends below.
		for (std::shared_ptr<IVideoRenderer>& renderer : RestoreLedForExit("Session end", uiThread))
			new std::shared_ptr<IVideoRenderer>(std::move(renderer));
	};
	steps.restoreTopology = []()
	{
		return videoProcessorApp.RestoreDisplayTopologyBounded("session-end",
			ShutdownDeadline::TopologyLockTimeoutMs);
	};
	steps.markCrashState = [](bool clean)
	{
		try { CrashState::Global().SetValue("exit", "Windows session end"); } catch (...) {}
		CrashState::Global().Stop(clean);
	};
	steps.finish = [this]()
	{
		// Windows may end the process at any moment once this returns; end
		// it here, so no renderer or destructor runs on into a half-ended
		// session (LED, topology and crash state are already done).
		m_forcedShutdownStarted.store(true);
		DebugLog::Log("Windows session end: process exits (exit_code=0)");
		DEBUGLOG_SHUTDOWN();
		TerminateProcess(GetCurrentProcess(), 0);
	};
	SessionEnd::Handle(WM_ENDSESSION, ending ? TRUE : FALSE, 0, steps, m_sessionEndRan);
}


bool CVideoProcessorDlg::WaitForShutdownWorkers(DWORD timeoutMs)
{
	const ULONGLONG deadline = GetTickCount64() + timeoutMs;
	const auto waitBounded = [deadline](HANDLE handle)
	{
		if (!handle)
			return true;
		for (;;)
		{
			const ULONGLONG now = GetTickCount64();
			const DWORD remaining = now >= deadline ? 0 :
				static_cast<DWORD>(deadline - now);
			const DWORD waitResult = MsgWaitForMultipleObjectsEx(
				1, &handle, remaining, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
			if (waitResult == WAIT_OBJECT_0)
				return true;
			if (waitResult != WAIT_OBJECT_0 + 1)
				return false;   // timeout or failure
			MSG message;
			PeekMessage(&message, nullptr, WM_NULL, WM_NULL, PM_NOREMOVE);
		}
	};
	m_rendererRetirementService.RequestClose();
	const bool retirementDone =
		waitBounded(m_rendererRetirementService.NativeThreadHandle());
	bool resetDone = true;
	if (m_rendererResetCoordinator)
	{
		m_rendererResetCoordinator->Close();
		resetDone = waitBounded(static_cast<HANDLE>(
			m_rendererResetCoordinator->NativeThreadHandle()));
	}
	// Bug hunt 3 (SH3): the destructor's other joins, under the same bound.
	// A thread that ended here is joined now, so the destructor's own joins
	// return at once; a blocked one fails the wait and the exit is forced
	// (crash state kept) instead of the destructor hanging after the crash
	// state was already marked clean.
	if (m_livenessWatchdogStopEvent)
		SetEvent(m_livenessWatchdogStopEvent);
	bool livenessDone = true;
	if (m_livenessWatchdogThread.joinable())
	{
		livenessDone = waitBounded(static_cast<HANDLE>(m_livenessWatchdogThread.native_handle()));
		if (livenessDone)
			m_livenessWatchdogThread.join();
	}
	if (m_unifiedActionCancelEvent)
		SetEvent(m_unifiedActionCancelEvent);
	m_unifiedActionCoalescer.CancelAll();
	bool unifiedDone = true;
	for (UnifiedActionWorker& worker : m_unifiedActionWorkers)
	{
		if (!worker.thread.joinable())
			continue;
		if (waitBounded(static_cast<HANDLE>(worker.thread.native_handle())))
			worker.thread.join();
		else
			unifiedDone = false;
	}
	// SH5: a running RS-232 test was cancelled in OnClose; wait for it to put
	// the light back.
	bool projectorTestDone = true;
	{
		const ULONGLONG now = GetTickCount64();
		const DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
		projectorTestDone = CancelProjectorTestForExit(
			(std::max<DWORD>)(remaining, ShutdownDeadline::ProjectorTestCancelTimeoutMs), "exit");
	}
	// Bug hunt 3 (SH11): a problem report being written is finished (its
	// thumbnail wait cut short), not killed mid-write.
	const bool reportDone = m_reportRun.WaitForExit(deadline, timeoutMs);
	if (!retirementDone || !resetDone || !livenessDone || !unifiedDone || !projectorTestDone || !reportDone)
	{
		DebugLog::Log(
			"Application shutdown workers did not finish: retirement_worker=%s "
			"reset_coordinator=%s liveness_watchdog=%s unified_actions=%s "
			"projector_test=%s state_report=%s timeout_ms=%lu",
			retirementDone ? "done" : "blocked", resetDone ? "done" : "blocked",
			livenessDone ? "done" : "blocked", unifiedDone ? "done" : "blocked",
			projectorTestDone ? "done" : "blocked", reportDone ? "done" : "blocked", timeoutMs);
		return false;
	}
	return true;
}
