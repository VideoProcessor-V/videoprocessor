#pragma once
/*
 * Copyright(C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// VP diagnostics adapter, GUI half (WP-17): the report-a-problem run out of
// VideoProcessorDlg.cpp (Bill's file). The dialog keeps the facts only it has (build identity,
// its live settings, the toast) and calls this at the state-report hook points:
//
//   TryBegin / AllowAutomatic   a report is requested (hotkey, menu, auto:<reason>)
//   NewInputs                   the shared inputs: folders, binaries, rings, crash state, configs, log
//   RequestThumbnail            optional rendered frame (report_thumbnail)
//   Start                       the worker thread (log tail, thumbnail wait, StateReport::Write)
//   Finish                      the worker's result message: log, decision, toast text
//   WaitForExit                 application exit: let a running report finish (bounded)
//
// Behaviour and report content are those of the dialog code it replaces (2026-10-17, bug
// hunt 3 C10/SH11 fixes included). Renderer-side half: DiagnosticsAdapter.h.
#include <VP-V/modules/control/diagnostics/CrashState.h>
#include <VP-V/modules/control/diagnostics/DiagnosticHistory.h>
#include <VP-V/modules/control/diagnostics/DiagnosticsAdapter.h>
#include <VP-V/modules/control/diagnostics/StateReport.h>
#include <ConfigFile.h>
#include <DebugLog.h>
#include <IRenderer.h>
#include <vprenderer/OptionalRendererLayout.h>

#include <atlstr.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace VpDiagnostics
{
	inline std::wstring ExecutableFolder()
	{
		std::vector<wchar_t> buffer(32768);
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size())
			return std::wstring();
		std::wstring path(buffer.data(), length);
		const size_t separator = path.find_last_of(L"\\/");
		return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
	}

	inline std::wstring WidenAnsi(const std::string& text)
	{
		return std::wstring(CStringW(CStringA(text.c_str())).GetString());
	}

	// The newest .png in `folder` written at or after `since`, empty if none.
	inline std::wstring NewestScreenshotSince(const std::wstring& folder, const FILETIME& since)
	{
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW((folder + L"\\*.png").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return std::wstring();
		std::wstring best;
		FILETIME bestTime = since;
		do
		{
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
				CompareFileTime(&data.ftLastWriteTime, &bestTime) >= 0)
			{
				bestTime = data.ftLastWriteTime;
				best = folder + L"\\" + data.cFileName;
			}
		} while (FindNextFileW(find, &data));
		FindClose(find);
		return best;
	}

	// The configuration files VP reads (full paths stay local; the bundle gets names).
	inline std::vector<std::wstring> ReportConfigFiles()
	{
		std::vector<std::wstring> files;
		ConfigFile main;
		if (main.Load())
			files.push_back(WidenAnsi(main.GetLoadedPath()));
		ConfigFile renderer;
		if (renderer.Load(ConfigFile::RENDERER_FILENAME) && !renderer.GetLoadedPath().empty() &&
			(files.empty() || WidenAnsi(renderer.GetLoadedPath()) != files.front()))
			files.push_back(WidenAnsi(renderer.GetLoadedPath()));
		return files;
	}

	class ReportRun
	{
	public:
		// The report worker stays detached (it never touches the dialog) but signals `done`, so
		// the exit waits for it (bounded) instead of killing it mid-write; `cancel` skips its
		// thumbnail wait (bug hunt 3 SH11).
		struct Worker
		{
			std::mutex mutex;
			std::condition_variable doneChanged;
			bool done = false;
			std::atomic<bool> cancel{ false };
		};

		bool Busy() const { return busy.load(); }
		int AutomaticCount() const { return limiter.Count(); }

		// MaybeAutoReport's gate (auto_report on): false = skipped, and the reason is logged.
		bool AllowAutomatic(bool autoReport, const char* reason, ULONGLONG nowTick)
		{
			if (!autoReport)
				return false;
			// Bug hunt 3 (SH11): a report already being written spends no slot.
			if (busy.load())
			{
				DebugLog::Log("Automatic problem report skipped (a report is being saved): reason=%s "
					"saved_this_run=%d", reason, limiter.Count());
				return false;
			}
			if (!limiter.Allow(nowTick, busy.load()))
			{
				DebugLog::Log("Automatic problem report skipped (rate limit): reason=%s saved_this_run=%d",
					reason, limiter.Count());
				return false;
			}
			return true;
		}

		// UI thread. False = a report is being saved ("already saving"). Bug hunt 3 (C10): a
		// report whose completion never came back (its post failed) frees the flag after two
		// minutes; a real report takes seconds.
		bool TryBegin(ULONGLONG nowTick)
		{
			if (busy.exchange(true) && !(busySince != 0 && nowTick - busySince > 120000))
				return false;
			busySince = nowTick;
			return true;
		}

		void End() { busy.store(false); }

		// The inputs every report shares, taken at the moment of the request: folders, the
		// binaries to hash, both rings, the crash-state text, the config files and the recent log.
		// The caller adds its build identity and live settings.
		std::shared_ptr<StateReport::Inputs> NewInputs(const char* trigger, IVideoRenderer* renderer)
		{
			const int64_t referenceQpc = DiagnosticHistory::NowQpc();
			DiagnosticHistory::Global().RecordDecision("state-report", trigger);

			auto inputs = std::make_shared<StateReport::Inputs>();
			inputs->trigger = trigger;
			inputs->vpRoot = ExecutableFolder();
			inputs->reportsDirectory = inputs->vpRoot + L"\\logs\\reports";
			inputs->binaries.push_back({ "VideoProcessor.exe", inputs->vpRoot + L"\\VideoProcessor.exe" });
			for (const bool beta : { false, true })
			{
				const std::wstring plugin = OptionalRendererLayout::PluginPath(inputs->vpRoot, beta);
				if (GetFileAttributesW(plugin.c_str()) == INVALID_FILE_ATTRIBUTES)
					continue;
				std::wstring relative = plugin.size() > inputs->vpRoot.size() + 1 ?
					plugin.substr(inputs->vpRoot.size() + 1) : plugin;
				std::replace(relative.begin(), relative.end(), L'\\', L'/');
				inputs->binaries.push_back({ std::string(CStringA(relative.c_str()).GetString()), plugin });
			}

			// The rings, at the moment of the key press.
			inputs->historyExe = DiagnosticHistory::ToJsonl(DiagnosticHistory::Global(), "exe", referenceQpc);
			if (renderer)
				inputs->rendererContributed = renderer->WriteDiagnosticHistory(
					&DiagnosticHistory::AppendToStringSink, &inputs->historyRenderer, referenceQpc);

			CrashState::Recorder& crashState = CrashState::Global();
			inputs->crashStateLive = crashState.Text();
			{
				std::string live = crashState.LivePath();
				const size_t separator = live.find_last_of("\\/");
				if (separator != std::string::npos)
					inputs->crashStateDirectory = WidenAnsi(live.substr(0, separator));
			}

			// Configuration files, then the log (in this order, as before: a config load may log).
			inputs->configFiles = ReportConfigFiles();
			pendingLogPath = WidenAnsi(DebugLog::GetLogFilePath());
			pendingRecentLog.clear();
			for (const std::string& line : DebugLog::RecentLines(~0ull, 600))
				pendingRecentLog += line + "\n";
			return inputs;
		}

		// Optional picture (report_thumbnail, off by default): the next rendered frame via the
		// existing screenshot path, scaled down by the worker.
		void RequestThumbnail(StateReport::Inputs& inputs, bool enabled, IVideoRenderer* renderer)
		{
			thumbnailRequested = false;
			GetSystemTimeAsFileTime(&requestTime);
			inputs.thumbnailEnabled = enabled;
			if (!enabled)
				return;
			CString status;
			thumbnailRequested = renderer && renderer->RequestRenderedOutputCapture(status);
			if (!thumbnailRequested)
				inputs.thumbnailNote = status.IsEmpty() ? std::string("no active VP Renderer") :
					std::string(CStringA(status).GetString());
		}

		// Starts the detached worker: log tail, thumbnail wait, StateReport::Write, then posts
		// doneMessage (LPARAM = StateReport::Result*, owned by the receiver) to window. False:
		// the thread could not start (logged; the run is ended).
		bool Start(std::shared_ptr<StateReport::Inputs> inputs, HWND window, UINT doneMessage)
		{
			const std::wstring logPath = pendingLogPath;
			const std::string recentLog = std::move(pendingRecentLog);
			pendingRecentLog.clear();
			std::wstring screenshotFolder = inputs->vpRoot + L"\\screenshots";
			{
				std::wstring testFolder;
				if (TestFolderOverride(TestFolder::Screenshots, testFolder))
					screenshotFolder = testFolder;
			}
			const bool thumbnail = thumbnailRequested;
			const FILETIME since = requestTime;
			auto state = std::make_shared<Worker>();
			std::atomic_store_explicit(&worker, state, std::memory_order_release);
			try
			{
				std::thread([inputs, window, doneMessage, logPath, recentLog, thumbnail, since, screenshotFolder, state]()
				{
					const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
					auto* result = new StateReport::Result();
					try
					{
						std::string log = StateReport::ReadFileTail(logPath, 512 * 1024);
						inputs->logExcerpt = log.empty() ? recentLog : log;
						if (thumbnail)
						{
							std::wstring shot;
							for (int attempt = 0; attempt < 40 && shot.empty() && !state->cancel.load(); ++attempt)
							{
								Sleep(100);
								shot = NewestScreenshotSince(screenshotFolder, since);
							}
							std::string error;
							if (shot.empty())
								inputs->thumbnailNote = "the rendered frame did not arrive within 4 s";
							else
							{
								Sleep(200);   // let the screenshot writer finish the file
								if (!StateReport::MakeThumbnailPng(shot, 480, inputs->thumbnailPng, &error))
									inputs->thumbnailNote = error;
							}
						}
						*result = StateReport::Write(*inputs);
					}
					catch (const std::exception& exception)
					{
						result->ok = false;
						result->error = exception.what();
					}
					if (SUCCEEDED(com))
						CoUninitialize();
					// Bug hunt 3 (C10): a failed post (a full message queue) used to leave the busy
					// flag set for good, refusing every later report. The post is retried while
					// the window lives; only a closed window drops it.
					bool posted = false;
					for (int attempt = 0; attempt < 100 && !posted && ::IsWindow(window); ++attempt)
					{
						posted = ::PostMessageW(window, doneMessage, 0, reinterpret_cast<LPARAM>(result)) != FALSE;
						if (!posted)
							Sleep(50);
					}
					if (!posted)
						delete result;
					{
						std::lock_guard<std::mutex> lock(state->mutex);
						state->done = true;
					}
					state->doneChanged.notify_all();
				}).detach();
			}
			catch (const std::exception& exception)
			{
				busy.store(false);
				// No thread: nothing to wait for at exit (audit 2026-10-25; the dialog code it
				// replaced left the published state "running", so the exit waited its full timeout).
				{
					std::lock_guard<std::mutex> lock(state->mutex);
					state->done = true;
				}
				state->doneChanged.notify_all();
				DebugLog::Log("Problem report could not start: %s", exception.what());
				return false;
			}
			return true;
		}

		// The worker's message (UI thread): ends the run; returns the toast text ("" = none).
		std::string Finish(LPARAM resultParam)
		{
			std::unique_ptr<StateReport::Result> result(reinterpret_cast<StateReport::Result*>(resultParam));
			busy.store(false);
			if (!result)
				return std::string();
			if (result->ok)
			{
				DebugLog::Log("Problem report saved: logs\\reports\\%s bytes=%zu files=%zu removed_old=%zu",
					result->fileName.c_str(), result->bytes, result->entries.size(), result->removed.size());
				DiagnosticHistory::Global().RecordDecision("state-report-saved", result->fileName.c_str(),
					static_cast<int64_t>(result->bytes));
				return "saved " + result->fileName;
			}
			DebugLog::Log("Problem report failed: %s", result->error.c_str());
			return "failed (" + result->error + ")";
		}

		// Application exit: a report being written is finished (its thumbnail wait cut short),
		// not killed mid-write. True when none runs or it finished by `deadline` (a tick).
		bool WaitForExit(ULONGLONG deadline, unsigned long timeoutMsForLog)
		{
			const auto state = std::atomic_load_explicit(&worker, std::memory_order_acquire);
			if (!state)
				return true;
			state->cancel.store(true);
			const ULONGLONG now = GetTickCount64();
			const DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
			std::unique_lock<std::mutex> lock(state->mutex);
			const bool done = state->doneChanged.wait_for(lock, std::chrono::milliseconds(remaining),
				[&state] { return state->done; });
			if (!done)
				DebugLog::Log("Problem report still being written at exit (timeout_ms=%lu)", timeoutMsForLog);
			return done;
		}

	private:
		StateReport::AutoReportLimiter limiter;
		std::atomic<bool> busy{ false };
		ULONGLONG busySince = 0;   // UI thread
		bool thumbnailRequested = false;
		FILETIME requestTime = {};
		std::wstring pendingLogPath;     // NewInputs -> Start (UI thread)
		std::string pendingRecentLog;    // the log fallback when the log file cannot be read
		std::shared_ptr<Worker> worker;
	};
}
