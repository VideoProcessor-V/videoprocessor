#include "pch.h"
#include "CppUnitTest.h"

#include <CrashState.h>
#include <DebugLog.h>

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
				const std::string text = Read(folder + "\\crash-state.txt");
				Assert::IsTrue(text.find("build: test build") != std::string::npos, L"the state values are written");
				Assert::IsTrue(text.find("crash state test marker 42") != std::string::npos,
					L"the recent log lines are written (kept even with file logging off)");
				Assert::IsTrue(text.find("uptime:") != std::string::npos);
				recorder.Stop(true);
				Assert::IsFalse(Exists(folder + "\\crash-state.txt"), L"a clean exit deletes it");
				Assert::IsFalse(Exists(folder + "\\crash-state.txt.tmp"), L"no temporary file is left");
			}
			{
				CrashState::Recorder recorder;
				recorder.Start(folder, {}, 100);
				recorder.Stop(false);
				Assert::IsTrue(Exists(folder + "\\crash-state.txt"), L"an unclean exit leaves it");
			}
			const auto kept = CrashState::PreserveLeftover(folder);
			Assert::AreEqual(size_t(1), kept.size());
			Assert::AreEqual(std::string("crash state (1).txt"), kept.front());
		}

		TEST_METHOD(MinidumpIsWrittenBesideTheState)
		{
			const std::string folder = TempFolder("dump");
			CrashState::Recorder recorder;
			recorder.Start(folder, {}, 1000);
			Assert::IsTrue(recorder.WriteMinidump(nullptr), L"MiniDumpWriteDump succeeded");
			WIN32_FILE_ATTRIBUTE_DATA data{};
			Assert::IsTrue(GetFileAttributesExA((folder + "\\crash-state.dmp").c_str(), GetFileExInfoStandard, &data) != 0);
			Assert::IsTrue(data.nFileSizeLow > 1000 || data.nFileSizeHigh > 0, L"the dump has content");
			recorder.Stop(true);
			Assert::IsFalse(Exists(folder + "\\crash-state.dmp"), L"a clean exit removes the dump too");
		}
	};
}
