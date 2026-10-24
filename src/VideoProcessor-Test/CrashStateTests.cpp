#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/control/diagnostics/CrashState.h>
#include <DebugLog.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Crash state (owner 2026-10-15): the rolling state file, kept as "crash
// state (N).txt" (newest 10) when a run does not exit cleanly.
namespace
{
	std::string TempFolder(const char* name)
	{
		char base[MAX_PATH] = {};
		GetTempPathA(MAX_PATH, base);
		const std::string folder = std::string(base) + "vp-crash-state-" + name;
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

	bool Exists(const std::string& path)
	{
		return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

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
	TEST_CLASS(CrashStateTests)
	{
	public:
		TEST_METHOD(LeftoverIsKeptNumberedAndTheOldestPruned)
		{
			const std::string folder = TempFolder("keep");
			Assert::IsTrue(CrashState::PreserveLeftover(folder).empty(), L"nothing left over: nothing kept");
			std::ofstream(folder + "\\crash-state.txt") << "first";
			std::ofstream(folder + "\\crash-state.dmp") << "dump";
			const auto kept = CrashState::PreserveLeftover(folder);
			Assert::AreEqual(size_t(2), kept.size());
			Assert::AreEqual(std::string("first"), Read(folder + "\\crash state (1).txt"));
			Assert::IsTrue(Exists(folder + "\\crash state (1).dmp"), L"the dump keeps the same number");
			Assert::IsFalse(Exists(folder + "\\crash-state.txt"));
			std::ofstream(folder + "\\crash-state.txt") << "second";
			CrashState::PreserveLeftover(folder);
			Assert::AreEqual(std::string("second"), Read(folder + "\\crash state (2).txt"));
			// Twelve kept: the next start keeps the newest ten.
			for (int n = 3; n <= 12; ++n)
				std::ofstream(folder + "\\crash state (" + std::to_string(n) + ").txt") << n;
			std::ofstream(folder + "\\crash-state.txt") << "thirteenth";
			CrashState::PreserveLeftover(folder, 10);
			const auto numbers = CrashState::KeptNumbers(folder);
			Assert::AreEqual(size_t(10), numbers.size());
			Assert::AreEqual(4, numbers.front(), L"the oldest are removed");
			Assert::AreEqual(13, numbers.back());
			Assert::AreEqual(std::string("thirteenth"), Read(folder + "\\crash state (13).txt"));
			Assert::IsFalse(Exists(folder + "\\crash state (1).dmp"), L"a pruned file's dump goes with it");
		}

		TEST_METHOD(CleanExitDeletesTheStateAnUncleanOneLeavesIt)
		{
			const std::string folder = TempFolder("rolling");
			DebugLog::Log("crash state test marker %d", 42);
			{
				CrashState::Recorder recorder;
				recorder.SetValue("build", "test build");
				Assert::IsTrue(recorder.Start(folder, [] { return DebugLog::RecentLines(5000, 300); }, 100));
				Sleep(350);
				const std::string text = Read(recorder.LivePath());
				Assert::IsTrue(text.find("build: test build") != std::string::npos, L"the state values are written");
				Assert::IsTrue(text.find("crash state test marker 42") != std::string::npos,
					L"the recent log lines are written (kept even with file logging off)");
				Assert::IsTrue(text.find("uptime:") != std::string::npos);
				recorder.Stop(true);
				Assert::IsFalse(Exists(recorder.LivePath()), L"a clean exit deletes it");
				Assert::IsFalse(Exists(recorder.LivePath() + ".tmp"), L"no temporary file is left");
			}
			{
				CrashState::Recorder recorder;
				recorder.Start(folder, {}, 100);
				recorder.Stop(false);
				Assert::IsTrue(Exists(recorder.LivePath()), L"an unclean exit leaves it");
				// audit 2026-10-16: the final snapshot is written atomically too.
				Assert::IsFalse(Exists(recorder.LivePath() + ".tmp"), L"no temporary file is left");
			}
			const auto kept = CrashState::PreserveLeftover(folder);
			Assert::AreEqual(size_t(1), kept.size());
			Assert::AreEqual(std::string("crash state (1).txt"), kept.front());
		}

		// Module findings 2026-10-24 item 5: an instance whose lock cannot be taken must not
		// have its live state filed as a crash by another instance's start.
		TEST_METHOD(UnlockedInstanceIsNeverKeptAsAFalseCrash)
		{
			const std::string folder = TempFolder("unlocked");
			const unsigned long id = 4242;
			// Something else holds the lock name exclusively: the recorder cannot take it.
			const std::string lockPath = folder + "\\" + CrashState::InstanceFileName(id, ".lock");
			const HANDLE blocker = CreateFileA(lockPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
			Assert::IsTrue(blocker != INVALID_HANDLE_VALUE);
			CrashState::Recorder recorder;
			Assert::IsTrue(recorder.Start(folder, {}, 100, id));
			Assert::IsFalse(recorder.InstanceLocked(), L"the lock failure is reported");
			Assert::IsTrue(recorder.LivePath().find(".unlocked.txt") != std::string::npos);
			Assert::IsTrue(Exists(recorder.LivePath()), L"the state is still written");
			CloseHandle(blocker);   // the lock name is now free: the running instance looks gone
			Assert::IsTrue(CrashState::PreserveLeftover(folder).empty(),
				L"another start does not take the running, unlocked instance's state for a crash");
			Assert::IsTrue(CrashState::KeptNumbers(folder).empty());
			recorder.Stop(true);
			Assert::IsFalse(Exists(recorder.LivePath()), L"a clean exit deletes the unlocked state too");
		}

		// audit 2026-10-16: the crash handler's write never waits on a held
		// lock - it gives up after a bounded wait - and otherwise writes the
		// crash with the recent lines of the last periodic write.
		TEST_METHOD(HandlerWriteDoesNotBlockOnAHeldWriterLock)
		{
			const std::string folder = TempFolder("handler");
			std::atomic<bool> block{ false };
			std::atomic<bool> entered{ false };
			CrashState::Recorder recorder;
			Assert::IsTrue(recorder.Start(folder, [&]
			{
				if (block.load())
				{
					entered = true;
					while (block.load()) Sleep(1);
				}
				return std::vector<std::string>{ "handler test recent line" };
			}, 100));
			block = true;
			const ULONGLONG waitStart = GetTickCount64();
			while (!entered.load() && GetTickCount64() - waitStart < 5000) Sleep(1);
			Assert::IsTrue(entered.load(), L"the periodic write is inside the log source, holding the writer lock");
			const ULONGLONG start = GetTickCount64();
			const bool blockedWrite = recorder.WriteFromHandler("held lock");
			const ULONGLONG elapsed = GetTickCount64() - start;
			block = false;
			Assert::IsFalse(blockedWrite, L"a held writer lock skips the handler's write");
			Assert::IsTrue(elapsed < 3000, L"the handler gives up after a bounded wait");
			// The released periodic write finishes on its own thread: poll until the lock is free
			// (bounded) instead of assuming a fixed delay is enough on a loaded machine.
			bool written = false;
			const ULONGLONG freeStart = GetTickCount64();
			while (!written && GetTickCount64() - freeStart < 10000)
			{
				written = recorder.WriteFromHandler("test crash");
				if (!written) Sleep(10);
			}
			Assert::IsTrue(written, L"a free writer lock is written");
			const std::string text = Read(recorder.LivePath());
			Assert::IsTrue(text.find("crash: test crash") != std::string::npos, L"the crash value is written");
			Assert::IsTrue(text.find("handler test recent line") != std::string::npos,
				L"the recent lines of the last periodic write are kept");
			Assert::IsFalse(Exists(recorder.LivePath() + ".tmp"), L"no temporary file is left");
			recorder.Stop(true);
		}

		TEST_METHOD(MinidumpIsWrittenBesideTheState)
		{
			const std::string folder = TempFolder("dump");
			CrashState::Recorder recorder;
			recorder.Start(folder, {}, 1000);
			Assert::IsTrue(recorder.WriteMinidump(nullptr), L"MiniDumpWriteDump succeeded");
			WIN32_FILE_ATTRIBUTE_DATA data{};
			Assert::IsTrue(GetFileAttributesExA(recorder.LiveDumpPath().c_str(), GetFileExInfoStandard, &data) != 0);
			Assert::IsTrue(data.nFileSizeLow > 1000 || data.nFileSizeHigh > 0, L"the dump has content");
			recorder.Stop(true);
			Assert::IsFalse(Exists(recorder.LiveDumpPath()), L"a clean exit removes the dump too");
		}
	};
}
