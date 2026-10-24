/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Bug hunt 3 (2026-10-18, hunter D): shutdown, shortcut and crash-state
// fixes SH1-SH12. Windows' session messages are simulated by sending them to
// a test window of this process only - nothing is broadcast to the system.
#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <ConfigurationLiveApply.h>
#include <VP-V/modules/control/diagnostics/CrashState.h>
#include <DebugLog.h>
#include <MainConfigSchema.h>
#include <RendererProfileConfig.h>
#include <SessionEndShutdown.h>
#include <ShortcutChord.h>
#include <ShortcutDefaults.h>
#include <led/ConnectionTest.h>
#include <led/LedShutdownRestore.h>
#include <led/ProjectorDriver.h>

#include "LedDriveTestSupport.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	std::mutex g_logMutex;
	std::string g_log;
	void __cdecl CollectLog(const char* message)
	{
		std::lock_guard<std::mutex> lock(g_logMutex);
		g_log += std::string(message) + "\n";
	}
	struct LogCapture
	{
		LogCapture() { { std::lock_guard<std::mutex> lock(g_logMutex); g_log.clear(); } DebugLog::SetExternalSink(CollectLog); }
		~LogCapture() { DebugLog::SetExternalSink(nullptr); }
		std::string Text() const { std::lock_guard<std::mutex> lock(g_logMutex); return g_log; }
	};

	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	LedDrive::ProjectorConfig FastProjector()
	{
		LedDrive::ProjectorConfig config;
		config.protocol = "barco_ascii";
		config.ackTimeoutMs = 60;
		config.minCommandGapMs = 0;
		config.maxLevelsPerCommand = 0;
		config.readBack = true;
		config.lightMin = 60;    // no range queries at Open
		config.lightMax = 255;
		return config;
	}

	// SH1: the session messages reach this window's procedure, which hands
	// them to SessionEnd::Handle exactly as CVideoProcessorDlg does.
	SessionEnd::Steps* g_sessionSteps = nullptr;
	bool g_sessionRan = false;
	LRESULT CALLBACK SessionWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (g_sessionSteps)
		{
			const SessionEnd::Outcome outcome = SessionEnd::Handle(message, wParam, lParam, *g_sessionSteps, g_sessionRan);
			if (outcome.handled)
				return outcome.result;
		}
		return DefWindowProcW(window, message, wParam, lParam);
	}

	HWND CreateSessionWindow()
	{
		WNDCLASSW windowClass = {};
		windowClass.lpfnWndProc = SessionWindowProc;
		windowClass.hInstance = GetModuleHandleW(nullptr);
		windowClass.lpszClassName = L"VPBugHunt3SessionEndTest";
		RegisterClassW(&windowClass);
		// A message-only window: it never appears and receives only what the
		// test sends it.
		return CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0,
			HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
	}

	std::string TempFolder(const char* name)
	{
		char base[MAX_PATH] = {};
		GetTempPathA(MAX_PATH, base);
		const std::string folder = std::string(base) + "vp-hunt3-" + name;
		CreateDirectoryA(folder.c_str(), nullptr);
		WIN32_FIND_DATAA found{};
		const HANDLE search = FindFirstFileA((folder + "\\*").c_str(), &found);
		if (search != INVALID_HANDLE_VALUE)
		{
			do
				if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
					DeleteFileA((folder + "\\" + found.cFileName).c_str());
			while (FindNextFileA(search, &found));
			FindClose(search);
		}
		return folder;
	}

	bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

	std::string Read(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::stringstream text;
		text << in.rdbuf();
		return text.str();
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(BugHunt3ShutdownTests)
	{
	public:
		// SH1: WM_QUERYENDSESSION is allowed and tears nothing down; WM_ENDSESSION
		// (TRUE) runs the bounded close path: LED restored (LDMM 0, full light,
		// CNTR 50), topology restored, crash state marked clean - in that order.
		TEST_METHOD(SessionEndRestoresTheLightTopologyAndCrashState)
		{
			LogCapture capture;
			auto projector = std::make_shared<FakeProjector>();
			projector->light = 90;
			auto driver = LedDrive::CreateProjectorDriver("barco_ascii", FastProjector(),
				[projector] { return std::make_unique<FakePort>(projector); });
			std::string error;
			Assert::IsTrue(driver->Open(error), Wide(error).c_str());
			Assert::IsTrue(driver->SendScript("CNTR", 66));
			Assert::IsTrue(driver->SendNow("LDMM", 2, 500));

			std::vector<std::string> order;
			bool crashClean = false;
			SessionEnd::Steps steps;
			steps.log = [](const char* phase) { DebugLog::Log("Windows %s", phase); };
			steps.cancelProjectorTest = [&order] { order.push_back("projector-test"); };
			steps.restoreLed = [&order, &driver]
			{
				order.push_back("led");
				LedDrive::IProjectorDriver* raw = driver.get();
				const LedDrive::BoundedRestoreResult result = LedDrive::RunBoundedRestore(
					[raw] { return LedDrive::RestoreFullLight(*raw, 255, true, true); }, 3000);
				DebugLog::Log("Session end LED restore: %s", LedDrive::Name(result));
			};
			steps.restoreTopology = [&order] { order.push_back("topology"); return true; };
			steps.markCrashState = [&order, &crashClean](bool clean) { order.push_back("crash-state"); crashClean = clean; };
			steps.finish = [&order] { order.push_back("finish"); };
			g_sessionSteps = &steps;
			g_sessionRan = false;
			const HWND window = CreateSessionWindow();
			Assert::IsNotNull(window);

			const LRESULT query = SendMessageW(window, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF);
			Assert::AreEqual<LRESULT>(TRUE, query, L"VP never vetoes the end of the session");
			Assert::IsTrue(order.empty(), L"nothing is torn down at the query (the end may be cancelled)");

			SendMessageW(window, WM_ENDSESSION, TRUE, ENDSESSION_LOGOFF);
			const std::string log = capture.Text();
			Assert::IsTrue(log.find("Dynamic Drive: light restored to 255") != std::string::npos, Wide(log).c_str());
			Assert::IsTrue(log.find("LED dim mode restored to standard") != std::string::npos, Wide(log).c_str());
			Assert::IsTrue(log.find("contrast restored to 50") != std::string::npos, Wide(log).c_str());
			Assert::AreEqual(255, projector->light);
			Assert::AreEqual(50, projector->contrast);
			const std::vector<std::string> expected = { "projector-test", "led", "topology", "crash-state", "finish" };
			Assert::IsTrue(order == expected, L"close path order");
			Assert::IsTrue(crashClean, L"a session end is a clean exit");

			// A second WM_ENDSESSION does not run it again.
			SendMessageW(window, WM_ENDSESSION, TRUE, 0);
			Assert::AreEqual(expected.size(), order.size());
			DestroyWindow(window);
			g_sessionSteps = nullptr;
			driver->Close();
		}

		TEST_METHOD(CancelledSessionEndKeepsVpRunning)
		{
			LogCapture capture;
			int ran = 0;
			SessionEnd::Steps steps;
			steps.log = [](const char* phase) { DebugLog::Log("Windows %s", phase); };
			steps.restoreLed = [&ran] { ++ran; };
			steps.markCrashState = [&ran](bool) { ++ran; };
			g_sessionSteps = &steps;
			g_sessionRan = false;
			const HWND window = CreateSessionWindow();
			Assert::AreEqual<LRESULT>(TRUE, SendMessageW(window, WM_QUERYENDSESSION, 0, 0));
			SendMessageW(window, WM_ENDSESSION, FALSE, 0);
			Assert::AreEqual(0, ran, L"a cancelled session end changes nothing");
			Assert::IsTrue(capture.Text().find("session-end cancelled") != std::string::npos);
			DestroyWindow(window);
			g_sessionSteps = nullptr;
		}

		// SH2: IDCANCEL is a close request (OnClose), never EndDialog.
		TEST_METHOD(CancelIsRoutedThroughTheClosePath)
		{
			Assert::IsTrue(ApplicationShutdownPolicy::RoutesCancelThroughClose(IDCANCEL));
			Assert::IsFalse(ApplicationShutdownPolicy::RoutesCancelThroughClose(IDOK));
		}

		// SH3: the dialog's destruction is bounded; the guard acts only when it
		// is not cancelled in time.
		TEST_METHOD(ExitGuardFiresOnlyWhenTheTeardownOverruns)
		{
			std::atomic<int> fired{ 0 };
			{
				ShutdownDeadline::ExitGuard guard(2000, [&fired] { ++fired; });
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				Assert::IsTrue(guard.Cancel(), L"finished in time");
			}
			Assert::AreEqual(0, fired.load());
			{
				ShutdownDeadline::ExitGuard guard(50, [&fired] { ++fired; });
				const auto start = std::chrono::steady_clock::now();
				while (fired.load() == 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds(3))
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				Assert::IsFalse(guard.Cancel(), L"overran: the action ran");
			}
			Assert::AreEqual(1, fired.load());
		}

		// SH4: a step behind a busy mutex is skipped within the bound.
		TEST_METHOD(TimedLockSkipsTheTopologyStepWhenTheMutexIsHeld)
		{
			std::timed_mutex mutex;
			std::atomic<bool> held{ false }, release{ false };
			std::thread holder([&] { std::lock_guard<std::timed_mutex> lock(mutex); held = true;
				while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
			while (!held) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			bool skipped = false, ran = false;
			const auto start = std::chrono::steady_clock::now();
			Assert::IsFalse(ShutdownDeadline::RunWithTimedLock(mutex, 100, [&ran] { ran = true; return true; }, skipped));
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			Assert::IsTrue(skipped && !ran);
			Assert::IsTrue(elapsed < 1500, (L"waited " + std::to_wstring(elapsed) + L" ms").c_str());
			release = true;
			holder.join();
			Assert::IsTrue(ShutdownDeadline::RunWithTimedLock(mutex, 100, [&ran] { ran = true; return true; }, skipped));
			Assert::IsTrue(ran && !skipped);
		}

		// SH5: closing during the 5 s hold cancels the test, which puts the light
		// back at once instead of leaving it at 60.
		TEST_METHOD(ConnectionTestCancelledDuringTheHoldRestoresTheLight)
		{
			auto projector = std::make_shared<FakeProjector>();
			LedDrive::ConnectionTestPlan plan;
			plan.holdMs = 5000;
			plan.commandTimeoutMs = 500;
			plan.cancel = std::make_shared<std::atomic<bool>>(false);
			LedDrive::ConnectionTestResult result;
			const auto start = std::chrono::steady_clock::now();
			std::thread test([&] {
				result = LedDrive::RunConnectionTestOnNewConnection(FastProjector(),
					[projector] { return std::make_unique<FakePort>(projector); }, plan);
			});
			// Wait for the low step (LED 60), then close.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (projector->light != 60 && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			Assert::AreEqual(60, projector->light, L"the low step ran");
			plan.cancel->store(true);
			test.join();
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			Assert::IsTrue(elapsed < 2500, (L"the hold did not end early: " + std::to_wstring(elapsed) + L" ms").c_str());
			Assert::AreEqual(255, projector->light, L"the light is back");
			Assert::IsFalse(result.pass);
			Assert::IsTrue(result.summary.find("cancelled") != std::string::npos, Wide(result.summary).c_str());
		}

		TEST_METHOD(ConnectionTestCancelledBeforeItStartsSendsNothing)
		{
			auto projector = std::make_shared<FakeProjector>();
			auto driver = LedDrive::CreateProjectorDriver("barco_ascii", FastProjector(),
				[projector] { return std::make_unique<FakePort>(projector); });
			std::string error;
			Assert::IsTrue(driver->Open(error));
			const size_t writesBefore = projector->Writes().size();
			LedDrive::ConnectionTestPlan plan;
			plan.cancel = std::make_shared<std::atomic<bool>>(true);
			const LedDrive::ConnectionTestResult result = LedDrive::RunConnectionTest(*driver, [](int) {}, plan);
			Assert::IsFalse(result.pass);
			Assert::AreEqual(writesBefore, projector->Writes().size(), L"no LED command");
			driver->Close();
		}

		// SH6: VP acts while another application has focus only on an
		// explicitly global binding, and never swallows a key it does not act on.
		TEST_METHOD(BackgroundShortcutsActOnlyWhenExplicitlyGlobal)
		{
			using namespace ConfigurationLiveApply;
			BackgroundShortcutContext c;
			// MPC-HC / a browser in front, a default (non-global) binding: untouched.
			c.matched = true;
			c.consumeWhenActing = true;    // Ctrl+Shift+S / Ctrl+Shift+U
			Assert::IsTrue(DecideBackgroundShortcut(c) == BackgroundShortcutAction::PassThrough);
			// Not VP's key: never swallowed, whatever else holds.
			BackgroundShortcutContext unmatched;
			unmatched.explicitlyGlobal = true;
			unmatched.consumeWhenActing = true;
			Assert::IsTrue(DecideBackgroundShortcut(unmatched) == BackgroundShortcutAction::PassThrough);
			// Marked global: acts; only the keys VP owns are taken.
			c.explicitlyGlobal = true;
			Assert::IsTrue(DecideBackgroundShortcut(c) == BackgroundShortcutAction::DispatchAndConsume);
			c.consumeWhenActing = false;   // e.g. fullscreen_toggle marked global
			Assert::IsTrue(DecideBackgroundShortcut(c) == BackgroundShortcutAction::DispatchAndPass);
			// Config in front: Config's own.
			c.configurationForeground = true;
			Assert::IsTrue(DecideBackgroundShortcut(c) == BackgroundShortcutAction::PassThrough);
			// VP in front: the dialog / fullscreen host handle it, not the hook,
			// except foreground_only's Config chord from the fullscreen output.
			BackgroundShortcutContext vp;
			vp.matched = true;
			vp.vpForeground = true;
			vp.explicitlyGlobal = true;
			Assert::IsTrue(DecideBackgroundShortcut(vp) == BackgroundShortcutAction::PassThrough);
			vp.sameProcessConfigRoute = true;
			Assert::IsTrue(DecideBackgroundShortcut(vp) == BackgroundShortcutAction::DispatchAndConsume);
		}

		TEST_METHOD(AltGrIsNotCtrlAlt)
		{
			using namespace ConfigurationLiveApply;
			// AltGr+W (a synthetic Ctrl with Right Alt) does not fire Ctrl+Alt+W.
			Assert::IsTrue(IsAltGr(true, false, true));
			Assert::IsFalse(KeyboardShortcutMatches(true, true, false, true, false, true, false));
			// A real Ctrl + Left Alt does.
			Assert::IsTrue(KeyboardShortcutMatches(true, true, false, true, true, false, false));
			// Right Alt alone (no AltGr layout) is Alt.
			Assert::IsTrue(KeyboardShortcutMatches(false, true, false, false, false, true, false));
			// The documented AltGr+Enter fullscreen relaxation is kept.
			Assert::IsTrue(FullscreenShortcutModifiersMatch(false, true, false, true, true, false, true));
		}

		// SH6: [shortcuts] global_actions names real actions only.
		TEST_METHOD(GlobalActionsListIsValidated)
		{
			std::string unknown;
			Assert::IsTrue(MainConfigSchema::ShortcutGlobalActionsValid("", unknown));
			Assert::IsTrue(MainConfigSchema::ShortcutGlobalActionsValid("config_editor, toggle_noui", unknown));
			Assert::IsFalse(MainConfigSchema::ShortcutGlobalActionsValid("config_editor, everything", unknown));
			Assert::AreEqual(std::string("everything"), unknown);
		}

		// SH9: a remote-editing Config's title still identifies VP's editor.
		TEST_METHOD(RemoteConfigTitlePassesTheFocusCheck)
		{
			using ConfigurationLiveApply::IsConfigurationEditorTitle;
			Assert::IsTrue(IsConfigurationEditorTitle(L"VideoProcessor Configuration"));
			Assert::IsTrue(IsConfigurationEditorTitle(std::wstring(L"VideoProcessor Configuration ") +
				static_cast<wchar_t>(0x00B7) + L" cinema-pc"));
			Assert::IsFalse(IsConfigurationEditorTitle(L"VideoProcessor Configuration helper"));
			Assert::IsFalse(IsConfigurationEditorTitle(L"Unsaved changes"));
		}

		// SH10: Config and VP accept exactly the same F-key forms.
		TEST_METHOD(ConfigAndVpShareTheFunctionKeyGrammar)
		{
			std::string canonical;
			for (const char* accepted : { "F1", "f4", "F01", "Ctrl+F24", "Shift+F12" })
			{
				ParsedShortcutChord parsed;
				Assert::IsTrue(ParseShortcutChord(accepted, parsed), Wide(accepted).c_str());
				Assert::IsTrue(RendererProfileConfig::CanonicalizeKeyChord(accepted, canonical));
			}
			for (const char* rejected : { "F0", "F25", "F004", "F 4", "F-1", "Fx" })
			{
				ParsedShortcutChord parsed;
				Assert::IsFalse(ParseShortcutChord(rejected, parsed), Wide(rejected).c_str());
				Assert::IsFalse(RendererProfileConfig::CanonicalizeKeyChord(rejected, canonical), Wide(rejected).c_str());
			}
			ParsedShortcutChord parsed;
			Assert::IsTrue(ParseShortcutChord("Ctrl+Shift+F4", parsed));
			Assert::IsTrue(parsed.control && parsed.shift && !parsed.alt);
			Assert::AreEqual<int>(0x73, parsed.virtualKey);   // VK_F4
			Assert::IsTrue(ParseShortcutChord("alt+enter", parsed));
			Assert::AreEqual<int>(0x0D, parsed.virtualKey);
			Assert::AreEqual(std::string("Alt+Enter"), parsed.canonical);
			Assert::IsTrue(ParseShortcutChord("esc", parsed));
			Assert::AreEqual<int>(0x1B, parsed.virtualKey);
			Assert::IsTrue(ParseShortcutChord("Ctrl+Alt+w", parsed));
			Assert::AreEqual<int>('W', parsed.virtualKey);
		}

		// SH12: two instances in one folder keep separate live files; one's
		// clean exit or start never touches the other's; only an ended
		// instance's leftover is kept (and counted).
		TEST_METHOD(CrashStateFilesArePerInstance)
		{
			const std::string folder = TempFolder("instances");
			CrashState::Recorder first, second;
			Assert::IsTrue(first.Start(folder, [] { return std::vector<std::string>{ "first" }; }, 100, 101));
			Assert::IsTrue(second.Start(folder, [] { return std::vector<std::string>{ "second" }; }, 100, 202));
			Assert::AreNotEqual(first.LivePath(), second.LivePath());
			Assert::IsTrue(Exists(first.LivePath()) && Exists(second.LivePath()));
			// A third start while both run keeps nothing.
			Assert::IsTrue(CrashState::PreserveLeftover(folder).empty(), L"running instances are not crashes");
			Assert::IsTrue(Exists(first.LivePath()) && Exists(second.LivePath()));
			// The first exits cleanly: only its own file goes.
			first.Stop(true);
			Assert::IsFalse(Exists(first.LivePath()));
			Assert::IsTrue(Exists(second.LivePath()));
			Assert::IsTrue(CrashState::PreserveLeftover(folder).empty());
			// The second ends uncleanly: now its leftover is kept, once.
			const std::string secondLive = second.LivePath();
			second.Stop(false);
			const auto kept = CrashState::PreserveLeftover(folder);
			Assert::AreEqual(size_t(1), kept.size());
			Assert::IsTrue(Read(folder + "\\crash state (1).txt").find("second") != std::string::npos);
			Assert::IsFalse(Exists(secondLive));
			Assert::IsTrue(CrashState::PreserveLeftover(folder).empty());
			Assert::AreEqual(size_t(1), CrashState::KeptNumbers(folder).size(), L"one real crash kept");
		}

		TEST_METHOD(KilledInstanceLeftoverIsKeptButALiveOneIsNot)
		{
			const std::string folder = TempFolder("killed");
			// A leftover of an instance that is gone (no lock held) and of the
			// single name earlier versions used.
			std::ofstream(folder + "\\" + CrashState::InstanceFileName(777, ".txt")) << "killed";
			std::ofstream(folder + "\\" + CrashState::InstanceFileName(777, ".txt.tmp")) << "torn";
			std::ofstream(folder + "\\crash-state.txt") << "old version";
			CrashState::Recorder running;
			Assert::IsTrue(running.Start(folder, [] { return std::vector<std::string>{}; }, 100, 888));
			const auto kept = CrashState::PreserveLeftover(folder);
			Assert::AreEqual(size_t(2), kept.size());
			Assert::IsTrue(Exists(running.LivePath()), L"the running instance keeps its file");
			Assert::IsFalse(Exists(folder + "\\" + CrashState::InstanceFileName(777, ".txt.tmp")));
			running.Stop(true);
			Assert::IsTrue(CrashState::PreserveLeftover(folder).empty());
		}
	};
}
