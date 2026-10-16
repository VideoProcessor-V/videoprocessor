#include <pch.h>
#include "CrashState.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
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

		LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* pointers)
		{
			if (Recorder* recorder = g_handlerRecorder)
			{
				recorder->SetValue("crash", "unhandled exception 0x" + [&]
				{
					char code[16];
					std::snprintf(code, sizeof(code), "%08lX",
						pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0ul);
					return std::string(code);
				}());
				recorder->WriteNow();
				recorder->WriteMinidump(pointers);
			}
			return g_previousFilter ? g_previousFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
		}

		void TerminateHandler()
		{
			if (Recorder* recorder = g_handlerRecorder)
			{
				recorder->SetValue("crash", "std::terminate");
				recorder->WriteNow();
				recorder->WriteMinidump(nullptr);
			}
			if (g_previousTerminate) g_previousTerminate();
			std::abort();
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
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (!values.empty())
			{
				text << "\n[state]\n";
				for (const auto& value : values)
					text << value.first << ": " << value.second << "\n";
			}
		}
		text << "\n[recent log]\n";
		if (recent)
			for (const std::string& line : recent())
				text << line << "\n";
		return text.str();
	}

	bool Recorder::WriteNow()
	{
		if (!running.load() && !stopping.load()) return false;
		std::lock_guard<std::mutex> guard(writeMutex);
		const std::string live = LivePath();
		const std::string temporary = live + ".tmp";
		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			if (!out) return false;
			out << Text();
			if (!out.good()) return false;
		}
		// The replace is atomic on NTFS: a reader (or a crash) sees the old
		// file or the new one, never half of one.
		return MoveFileExA(temporary.c_str(), live.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
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
			// Leave a final snapshot for the next start.
			std::ofstream(LivePath(), std::ios::binary | std::ios::trunc) << Text();
		}
		stopping = false;
	}

	void Recorder::InstallCrashHandler()
	{
		g_handlerRecorder = this;
		g_previousFilter = SetUnhandledExceptionFilter(UnhandledFilter);
		g_previousTerminate = std::set_terminate(TerminateHandler);
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
