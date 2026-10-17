#include <pch.h>
#include "CrashState.h"

#include <Windows.h>
#include <Psapi.h>

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
		// Bug hunt 2026-10-17: abort() on any thread (CRT invalid-parameter /
		// pure-call paths, assert, an explicit abort in a library) bypassed
		// both handlers above and left no crash state.
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

	std::vector<std::string> PreserveLeftover(const std::string& directory, int keep)
	{
		std::vector<std::string> created;
		const std::string live = Join(directory, kLiveName);
		const std::string liveDump = Join(directory, kLiveDumpName);
		const bool haveText = Exists(live), haveDump = Exists(liveDump);
		if (haveText || haveDump)
		{
			const std::vector<int> kept = KeptNumbers(directory);
			const int next = kept.empty() ? 1 : kept.back() + 1;
			if (haveText && MoveFileExA(live.c_str(), Join(directory, KeptName(next, ".txt")).c_str(), MOVEFILE_REPLACE_EXISTING))
				created.push_back(KeptName(next, ".txt"));
			if (haveDump && MoveFileExA(liveDump.c_str(), Join(directory, KeptName(next, ".dmp")).c_str(), MOVEFILE_REPLACE_EXISTING))
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

	bool Recorder::Start(const std::string& directoryIn, LinesSource recentIn, int periodMsIn)
	{
		if (running.load()) return true;
		directory = directoryIn;
		recent = std::move(recentIn);
		periodMs = (std::max)(100, periodMsIn);
		startedTick = GetTickCount64();
		startedAt = Now();
		CreateDirectoryA(directory.c_str(), nullptr);
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
		return Join(directory, kLiveName);
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
			DeleteFileA(Join(directory, kLiveDumpName).c_str());
		}
		else
		{
			// Leave a final snapshot for the next start. audit 2026-10-16:
			// written atomically (temporary + replace), like WriteNow.
			WriteAtomically(Text());
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
			const HANDLE file = CreateFileA(Join(directory, kLiveDumpName).c_str(), GENERIC_WRITE, 0, nullptr,
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
