#include <pch.h>
#include "CrashState.h"

#include <Windows.h>
#include <Psapi.h>

#include <DebugLog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <regex>
#include <sstream>

namespace CrashState
{
	namespace
	{
		std::string Join(const std::string& directory, const std::string& name)
		{
			if (directory.empty()) return name;
			const char last = directory.back();
			return directory + (last == '\\' || last == '/' ? "" : "\\") + name;
		}

		bool Exists(const std::string& path)
		{
			return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
		}

		std::string Now()
		{
			SYSTEMTIME t{};
			GetLocalTime(&t);
			char text[64];
			std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay,
				t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
			return text;
		}

		std::string KeptName(int number, const char* extension)
		{
			return "crash state (" + std::to_string(number) + ")" + extension;
		}

		Recorder* g_handlerRecorder = nullptr;
		LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
		std::terminate_handler g_previousTerminate = nullptr;
		// Bug hunt 2026-10-17: abort() on any thread (assert, an explicit abort
		// in a library, a std::terminate on a worker thread, whose per-thread
		// terminate handler is the CRT default) bypassed both handlers above and
		// left no crash state. Hunt 2 LOW-3: CRT invalid-parameter and pure-call
		// failures do not call abort() in Release (they fail fast, bypassing every
		// handler here); they have their own handlers below. /GS and heap
		// corruption fail-fast terminations still leave no crash: line (use WER
		// LocalDumps for those).
		using AbortSignalHandler = void(__cdecl*)(int);
		AbortSignalHandler g_previousAbort = SIG_DFL;
		// Set once a terminate has recorded, so the abort() it raises does not
		// record the same crash a second time.
		std::atomic<bool> g_terminateRecorded{ false };

		// audit 2026-10-16: a crash handler must never block on a lock the
		// crashed thread (or a stalled one) holds: try for a short, bounded
		// time and give up instead.
		bool TryLockFor(std::mutex& lock, DWORD milliseconds)
		{
			const ULONGLONG deadline = GetTickCount64() + milliseconds;
			for (;;)
			{
				if (lock.try_lock()) return true;
				if (GetTickCount64() >= deadline) return false;
				Sleep(1);
			}
		}

		// audit 2026-10-16: a fault inside a handler (or terminate during the
		// filter) on the same thread does not run the handler's write again.
		thread_local bool t_inCrashHandler = false;

		LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* pointers)
		{
			Recorder* recorder = g_handlerRecorder;
			if (recorder && !t_inCrashHandler)
			{
				char code[16];
				std::snprintf(code, sizeof(code), "%08lX",
					pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0ul);
				recorder->WriteFromHandler(std::string("unhandled exception 0x") + code);
				t_inCrashHandler = true;   // the dump is guarded against re-entry too
				recorder->WriteMinidump(pointers);
				t_inCrashHandler = false;
			}
			return g_previousFilter ? g_previousFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
		}

		void TerminateHandler()
		{
			Recorder* recorder = g_handlerRecorder;
			if (recorder && !t_inCrashHandler)
			{
				recorder->WriteFromHandler("std::terminate");
				t_inCrashHandler = true;   // the dump is guarded against re-entry too
				recorder->WriteMinidump(nullptr);
				t_inCrashHandler = false;
				g_terminateRecorded.store(true);
			}
			if (g_previousTerminate) g_previousTerminate();
			std::abort();
		}

		// Hunt 2 LOW-3: process-wide in the shared UCRT (every /MD module). Record,
		// then end the process with abort() as before (the abort handler sees
		// g_terminateRecorded and does not record twice).
		void RecordFatalFromHandler(const char* label)
		{
			Recorder* recorder = g_handlerRecorder;
			if (recorder && !t_inCrashHandler && !g_terminateRecorded.load())
			{
				recorder->WriteFromHandler(label);
				t_inCrashHandler = true;
				recorder->WriteMinidump(nullptr);
				t_inCrashHandler = false;
				g_terminateRecorded.store(true);
			}
		}

		void __cdecl InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*,
			unsigned int, uintptr_t)
		{
			RecordFatalFromHandler("CRT invalid parameter");
			std::abort();
		}

		void __cdecl PurecallHandler()
		{
			RecordFatalFromHandler("pure virtual call");
			std::abort();
		}

		// Bug hunt 2026-10-17: SIGABRT is process-wide in the MSVC CRT, so this
		// covers worker threads. Records the crash state as the terminate
		// handler does, then chains to any previous handler; abort() then ends
		// the process (with _CALL_REPORTFAULT cleared, without WER).
		void __cdecl AbortHandler(int signalNumber)
		{
			Recorder* recorder = g_handlerRecorder;
			if (recorder && !t_inCrashHandler && !g_terminateRecorded.load())
			{
				recorder->WriteFromHandler("abort (SIGABRT)");
				t_inCrashHandler = true;   // the dump is guarded against re-entry too
				recorder->WriteMinidump(nullptr);
				t_inCrashHandler = false;
			}
			const AbortSignalHandler previous = g_previousAbort;
			if (previous != SIG_DFL && previous != SIG_IGN && previous != SIG_ERR &&
				previous != AbortHandler)
				previous(signalNumber);
		}
	}

	std::vector<int> KeptNumbers(const std::string& directory)
	{
		std::vector<int> numbers;
		WIN32_FIND_DATAA found{};
		const HANDLE search = FindFirstFileA(Join(directory, "crash state (*).txt").c_str(), &found);
		if (search == INVALID_HANDLE_VALUE) return numbers;
		static const std::regex pattern(R"(^crash state \((\d+)\)\.txt$)", std::regex::icase);
		do
		{
			std::cmatch match;
			if (std::regex_match(found.cFileName, match, pattern))
				numbers.push_back(std::stoi(match[1].str()));
		} while (FindNextFileA(search, &found));
		FindClose(search);
		std::sort(numbers.begin(), numbers.end());
		return numbers;
	}

	std::string InstanceFileName(unsigned long instanceId, const char* extension)
	{
		return "crash-state-" + std::to_string(instanceId) + extension;
	}

	namespace
	{
		// SH12: an instance runs while its lock file is held open (exclusively).
		bool InstanceRunning(const std::string& directory, unsigned long instanceId)
		{
			const std::string lock = Join(directory, InstanceFileName(instanceId, ".lock"));
			if (!Exists(lock))
				return false;
			const HANDLE probe = CreateFileA(lock.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (probe == INVALID_HANDLE_VALUE)
				return GetLastError() == ERROR_SHARING_VIOLATION;
			CloseHandle(probe);
			DeleteFileA(lock.c_str());   // a stale lock (cannot normally remain)
			return false;
		}

		unsigned long long WriteTime(const std::string& path)
		{
			WIN32_FILE_ATTRIBUTE_DATA data{};
			if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
				return 0;
			return (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
				data.ftLastWriteTime.dwLowDateTime;
		}

		struct Leftover
		{
			std::string text;   // full paths ("" when absent)
			std::string dump;
			unsigned long long time = 0;
		};
	}

	std::vector<std::string> PreserveLeftover(const std::string& directory, int keep)
	{
		std::vector<std::string> created;
		std::vector<Leftover> leftovers;
		const auto addLeftover = [&leftovers](const std::string& text, const std::string& dump)
		{
			Leftover leftover;
			if (Exists(text)) leftover.text = text;
			if (Exists(dump)) leftover.dump = dump;
			if (leftover.text.empty() && leftover.dump.empty())
				return;
			leftover.time = (std::max)(WriteTime(text), WriteTime(dump));
			leftovers.push_back(leftover);
		};
		// Earlier versions' single live name.
		addLeftover(Join(directory, kLiveName), Join(directory, kLiveDumpName));
		// Per-instance live files whose instance is gone (SH12).
		{
			std::vector<unsigned long> instances;
			WIN32_FIND_DATAA found{};
			const HANDLE search = FindFirstFileA(Join(directory, "crash-state-*").c_str(), &found);
			if (search != INVALID_HANDLE_VALUE)
			{
				static const std::regex pattern(R"(^crash-state-(\d+)\.(txt|dmp|txt\.tmp)$)", std::regex::icase);
				do
				{
					std::cmatch match;
					if (std::regex_match(found.cFileName, match, pattern))
					{
						const unsigned long id = std::stoul(match[1].str());
						if (std::find(instances.begin(), instances.end(), id) == instances.end())
							instances.push_back(id);
					}
				} while (FindNextFileA(search, &found));
				FindClose(search);
			}
			for (const unsigned long id : instances)
			{
				if (InstanceRunning(directory, id))
					continue;
				DeleteFileA(Join(directory, InstanceFileName(id, ".txt.tmp")).c_str());
				addLeftover(Join(directory, InstanceFileName(id, ".txt")), Join(directory, InstanceFileName(id, ".dmp")));
			}
		}
		std::sort(leftovers.begin(), leftovers.end(),
			[](const Leftover& a, const Leftover& b) { return a.time < b.time; });
		for (const Leftover& leftover : leftovers)
		{
			const std::vector<int> kept = KeptNumbers(directory);
			const int next = kept.empty() ? 1 : kept.back() + 1;
			const bool haveText = !leftover.text.empty(), haveDump = !leftover.dump.empty();
			if (haveText && MoveFileExA(leftover.text.c_str(), Join(directory, KeptName(next, ".txt")).c_str(), MOVEFILE_REPLACE_EXISTING))
				created.push_back(KeptName(next, ".txt"));
			if (haveDump && MoveFileExA(leftover.dump.c_str(), Join(directory, KeptName(next, ".dmp")).c_str(), MOVEFILE_REPLACE_EXISTING))
				created.push_back(KeptName(next, ".dmp"));
			// A dump without its text (the text write failed): give it an empty
			// companion so the pair keeps one number.
			if (!haveText && haveDump)
				std::ofstream(Join(directory, KeptName(next, ".txt"))) << "(no state text was written)\n";
		}
		std::vector<int> kept = KeptNumbers(directory);
		const int limit = (std::max)(1, keep);
		while (static_cast<int>(kept.size()) > limit)
		{
			DeleteFileA(Join(directory, KeptName(kept.front(), ".txt")).c_str());
			DeleteFileA(Join(directory, KeptName(kept.front(), ".dmp")).c_str());
			kept.erase(kept.begin());
		}
		return created;
	}

	Recorder::~Recorder()
	{
		if (g_handlerRecorder == this) g_handlerRecorder = nullptr;
		Stop(false);
	}

	bool Recorder::Start(const std::string& directoryIn, LinesSource recentIn, int periodMsIn,
		unsigned long instanceIdIn)
	{
		if (running.load()) return true;
		directory = directoryIn;
		instanceId = instanceIdIn ? instanceIdIn : GetCurrentProcessId();
		recent = std::move(recentIn);
		periodMs = (std::max)(100, periodMsIn);
		startedTick = GetTickCount64();
		startedAt = Now();
		CreateDirectoryA(directory.c_str(), nullptr);
		// SH12: held for the life of the instance (the OS deletes it when the
		// handle closes, also when the process is killed), so another start
		// never takes this instance's live file for a crash leftover.
		// Module findings 2026-10-24 item 5: a lock that cannot be taken is not ignored.
		// A short retry covers a transient hold (an antivirus scan, a probe by another
		// instance's InstanceRunning); after that the live files take the ".unlocked"
		// names, which PreserveLeftover never treats as a crash leftover.
		HANDLE lock = INVALID_HANDLE_VALUE;
		DWORD lockError = 0;
		for (int attempt = 0; attempt < 3 && lock == INVALID_HANDLE_VALUE; ++attempt)
		{
			if (attempt) Sleep(50);
			lock = CreateFileA(Join(directory, InstanceFileName(instanceId, ".lock")).c_str(),
				GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
			if (lock == INVALID_HANDLE_VALUE) lockError = GetLastError();
		}
		instanceLock = lock == INVALID_HANDLE_VALUE ? nullptr : lock;
		unlocked = !instanceLock;
		if (unlocked)
			DebugLog::Log("Crash state: the instance lock could not be taken (error %lu); live state kept as %s, "
				"which is not kept as a crash state after an unclean exit", lockError, LivePath().c_str());
		stopping = false;
		running = true;
		WriteNow();
		worker = std::thread([this] { Loop(); });
		return true;
	}

	void Recorder::SetValue(const std::string& key, const std::string& value)
	{
		std::lock_guard<std::mutex> guard(mutex);
		values[key] = value;
	}

	std::string Recorder::LivePath() const
	{
		return Join(directory, InstanceFileName(instanceId, unlocked ? ".unlocked.txt" : ".txt"));
	}

	std::string Recorder::LiveDumpPath() const
	{
		return Join(directory, InstanceFileName(instanceId, unlocked ? ".unlocked.dmp" : ".dmp"));
	}

	std::string Recorder::Text() const
	{
		std::map<std::string, std::string> valuesCopy;
		{
			std::lock_guard<std::mutex> guard(mutex);
			valuesCopy = values;
		}
		std::vector<std::string> lines;
		if (recent)
			lines = recent();
		return Compose(valuesCopy, lines);
	}

	std::string Recorder::Compose(const std::map<std::string, std::string>& valuesIn,
		const std::vector<std::string>& recentLines) const
	{
		std::ostringstream text;
		text << "VideoProcessor rolling state - rewritten about once a second and deleted on a clean exit.\n"
			"If you find this as \"crash state (N).txt\", VP did not exit cleanly: please send it (and the .dmp\n"
			"with the same number, if there is one) to whoever looks after your VP.\n\n";
		text << "written: " << Now() << "\n";
		text << "started: " << startedAt << "\n";
		text << "uptime: " << (GetTickCount64() - startedTick) / 1000.0 << " s\n";
		text << "pid: " << GetCurrentProcessId() << "\n";
		PROCESS_MEMORY_COUNTERS memory{};
		if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
			text << "working set: " << memory.WorkingSetSize / (1024 * 1024) << " MB (peak "
				<< memory.PeakWorkingSetSize / (1024 * 1024) << " MB)\n";
		if (!valuesIn.empty())
		{
			text << "\n[state]\n";
			for (const auto& value : valuesIn)
				text << value.first << ": " << value.second << "\n";
		}
		text << "\n[recent log]\n";
		for (const std::string& line : recentLines)
			text << line << "\n";
		return text.str();
	}

	// The caller holds writeMutex.
	bool Recorder::WriteAtomically(const std::string& content)
	{
		const std::string live = LivePath();
		const std::string temporary = live + ".tmp";
		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			if (!out) return false;
			out << content;
			out.flush();   // audit 2026-10-16
			if (!out.good()) return false;
		}
		// audit 2026-10-16: the data reaches the disk before the rename, so the
		// replace never commits an empty file after a power loss.
		const HANDLE flushHandle = CreateFileA(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (flushHandle != INVALID_HANDLE_VALUE)
		{
			FlushFileBuffers(flushHandle);
			CloseHandle(flushHandle);
		}
		// The replace is atomic on NTFS: a reader (or a crash) sees the old
		// file or the new one, never half of one.
		return MoveFileExA(temporary.c_str(), live.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
	}

	bool Recorder::WriteNow()
	{
		if (!running.load() && !stopping.load()) return false;
		std::lock_guard<std::mutex> guard(writeMutex);
		std::vector<std::string> lines;
		if (recent)
			lines = recent();
		std::map<std::string, std::string> valuesCopy;
		{
			std::lock_guard<std::mutex> lock(mutex);
			valuesCopy = values;
			// audit 2026-10-16: kept for the crash handler, which must not call
			// the log source (the crashed thread may hold the log's lock).
			lastRecent = lines;
		}
		return WriteAtomically(Compose(valuesCopy, lines));
	}

	bool Recorder::WriteFromHandler(const std::string& crash)
	{
		// audit 2026-10-16: handler context. Re-entry on the same thread is
		// ignored and every lock is tried with a bounded wait: a busy state
		// lock still writes a minimal snapshot, a busy writer skips the write.
		if (t_inCrashHandler) return false;
		t_inCrashHandler = true;
		bool written = false;
		if (running.load() || stopping.load())
		{
			std::map<std::string, std::string> valuesCopy;
			std::vector<std::string> lines;
			if (TryLockFor(mutex, 200))
			{
				values["crash"] = crash;
				valuesCopy = values;
				lines = lastRecent;
				mutex.unlock();
			}
			else
			{
				valuesCopy["crash"] = crash;
				lines.push_back("(state values and recent log unavailable: locked when the crash handler ran)");
			}
			if (TryLockFor(writeMutex, 500))
			{
				written = WriteAtomically(Compose(valuesCopy, lines));
				writeMutex.unlock();
			}
		}
		t_inCrashHandler = false;
		return written;
	}

	void Recorder::Loop()
	{
		while (!stopping.load())
		{
			{
				std::unique_lock<std::mutex> lock(wakeMutex);
				wake.wait_for(lock, std::chrono::milliseconds(periodMs), [this] { return stopping.load(); });
			}
			if (stopping.load()) break;
			WriteNow();
		}
	}

	void Recorder::Stop(bool clean)
	{
		if (!running.exchange(false)) return;
		stopping = true;
		wake.notify_all();
		if (worker.joinable()) worker.join();
		std::lock_guard<std::mutex> guard(writeMutex);
		DeleteFileA((LivePath() + ".tmp").c_str());
		if (clean)
		{
			DeleteFileA(LivePath().c_str());
			DeleteFileA(LiveDumpPath().c_str());
		}
		else
		{
			// Leave a final snapshot for the next start. audit 2026-10-16:
			// written atomically (temporary + replace), like WriteNow.
			WriteAtomically(Text());
		}
		// SH12: the instance is over; its leftover (if any) may now be kept.
		if (instanceLock)
		{
			CloseHandle(static_cast<HANDLE>(instanceLock));
			instanceLock = nullptr;
		}
		stopping = false;
	}

	void Recorder::InstallCrashHandler()
	{
		g_handlerRecorder = this;
		g_previousFilter = SetUnhandledExceptionFilter(UnhandledFilter);
		g_previousTerminate = std::set_terminate(TerminateHandler);
		// Bug hunt 2026-10-17: abort() on any thread records the crash state.
		const AbortSignalHandler previousAbort = std::signal(SIGABRT, AbortHandler);
		if (previousAbort != SIG_ERR && previousAbort != AbortHandler)
			g_previousAbort = previousAbort;
		_set_abort_behavior(0, _CALL_REPORTFAULT);
		// Hunt 2 LOW-3: these fail fast in Release instead of calling abort().
		_set_invalid_parameter_handler(InvalidParameterHandler);
		_set_purecall_handler(PurecallHandler);
	}

	bool Recorder::WriteMinidump(void* exceptionPointers)
	{
		// dbghelp is loaded only when needed (no link dependency).
		using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
		const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
		if (!dbghelp) return false;
		const auto write = reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
		bool ok = false;
		if (write)
		{
			const HANDLE file = CreateFileA(LiveDumpPath().c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file != INVALID_HANDLE_VALUE)
			{
				struct ExceptionInformation { DWORD threadId; void* pointers; BOOL clientPointers; };
				ExceptionInformation information{ GetCurrentThreadId(), exceptionPointers, FALSE };
				// MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpNormal: small,
				// enough for the call stacks.
				const int type = 0x00001000 | 0x00000040;
				ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type,
					exceptionPointers ? &information : nullptr, nullptr, nullptr) != FALSE;
				CloseHandle(file);
			}
		}
		FreeLibrary(dbghelp);
		return ok;
	}

	Recorder& Global()
	{
		static Recorder recorder;
		return recorder;
	}
}
