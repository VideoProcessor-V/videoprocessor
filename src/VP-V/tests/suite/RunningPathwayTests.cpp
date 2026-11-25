// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CC W4 (B1, GEM-221 / MUST-8, SHOULD-W4-1): the process-wide running-pathway latch (VP-V/core/RunningPathway.h) and the
// two consumers that read it: QueueLookahead::ResolveActivePictureLookahead (an unset look-ahead is Bill's 0 on
// Original) and CaptureDisplayFixes::Enabled (off on Original whatever the switch). With nothing latched, the first
// read latches from the start-up configuration file (VideoProcessor.cfg; ConfigFile::Load's default candidates end with
// the working directory), so the start-up target-only session and a process whose profile runtime failed are covered.
// B2 (the profile runtime latches and uses the latch) is tested through the real snapshot builder in
// VpvProfileQueueAdviceTests.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/RunningPathwayLatch.h>
#include <VP-V/modules/input/queue_timing/QueueLookahead.h>
#include <CaptureDisplayFixes.h>

#include <atomic>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// The latch is process-wide in the test DLL: every test leaves it unknown (start-up read re-armed) and the
		// capture switch on, its default.
		struct LatchReset
		{
			LatchReset() { Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown); }
			~LatchReset()
			{
				Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown);
				CaptureDisplayFixes::Set(true);
			}
		};

		// A fresh working directory in %TEMP% holding VideoProcessor.cfg with `text` (none when null); the previous
		// working directory is restored and the folder removed afterwards.
		struct StartupFolder
		{
			std::string previous, folder, file;
			explicit StartupFolder(const char* text)
			{
				char current[MAX_PATH]{}, temp[MAX_PATH]{};
				Assert::IsTrue(GetCurrentDirectoryA(MAX_PATH, current) != 0);
				Assert::IsTrue(GetTempPathA(MAX_PATH, temp) != 0);
				previous = current;
				folder = std::string(temp) + "vp-running-pathway-" + std::to_string(GetCurrentProcessId()) + "-" +
					std::to_string(GetTickCount64());
				Assert::IsTrue(CreateDirectoryA(folder.c_str(), nullptr) != 0, L"the start-up folder cannot be made");
				file = folder + "\\VideoProcessor.cfg";
				if (text)
					std::ofstream(file, std::ios::trunc) << text;
				Assert::IsTrue(SetCurrentDirectoryA(folder.c_str()) != 0);
			}
			~StartupFolder()
			{
				SetCurrentDirectoryA(previous.c_str());
				DeleteFileA(file.c_str());
				RemoveDirectoryA(folder.c_str());
			}
		};

		const char* const kOriginalFile = "[general]\nrenderer: VP Renderer\n[vpv]\npipeline_original_pathway: on\n";
		const char* const kVpFile = "[general]\nrenderer: VP Renderer\n[vpv]\npipeline_original_pathway: off\n";
	}

	TEST_CLASS(RunningPathwayTests)
	{
	public:
		TEST_METHOD(TheFirstLatchWinsAndUnknownIsNotOriginal)
		{
			LatchReset reset;
			StartupFolder none(nullptr);   // no start-up file: the first read latches nothing
			Assert::IsFalse(Vpv::RunningPathway::Original(), L"unknown reads as Original");
			Assert::AreEqual(int(Vpv::RunningPathway::kUnknown), Vpv::RunningPathway::Value().load(), L"a missing file latched");
			Assert::IsTrue(Vpv::RunningPathway::Latch(true), L"the first latch (Original) is not reported");
			Assert::IsTrue(Vpv::RunningPathway::Original());
			Assert::IsTrue(Vpv::RunningPathway::Latch(false), L"a later VP-V latch replaced Original");
			Assert::IsTrue(Vpv::RunningPathway::Original(), L"a later latch changed the running pathway");

			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown);
			Assert::IsFalse(Vpv::RunningPathway::Latch(false), L"the first latch (VP-V) reads as Original");
			Assert::IsFalse(Vpv::RunningPathway::Latch(true), L"a later Original latch replaced VP-V");
			Assert::IsFalse(Vpv::RunningPathway::Original());
			Assert::AreEqual(int(Vpv::RunningPathway::kVpv), Vpv::RunningPathway::Value().load());
		}

		// SHOULD-W4-1 / start-up gap: nothing latched (before the profile runtime's first snapshot, or it failed to
		// start), the first read latches from the start-up configuration file; once, and a later file does not change it.
		TEST_METHOD(TheFirstReadLatchesFromTheStartupConfiguration)
		{
			LatchReset reset;
			{
				StartupFolder original(kOriginalFile);
				Assert::IsTrue(Vpv::RunningPathway::Original(), L"an Original start-up file did not latch Original");
				Assert::AreEqual(int(Vpv::RunningPathway::kOriginal), Vpv::RunningPathway::Value().load());
				Assert::IsFalse(Vpv::RunningPathway::Latch(false), L"a later latch replaced the start-up file's Original") ;
			}
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown);
			{
				StartupFolder vp(kVpFile);
				Assert::IsFalse(Vpv::RunningPathway::Original(), L"a VP-V start-up file latched Original");
				Assert::AreEqual(int(Vpv::RunningPathway::kVpv), Vpv::RunningPathway::Value().load(), L"a VP-V start-up file latched nothing");
			}
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown);
			{
				StartupFolder none(nullptr);
				Assert::IsFalse(Vpv::RunningPathway::Original());
				std::ofstream(none.file, std::ios::trunc) << kOriginalFile;   // appears after the one start-up read
				Assert::IsFalse(Vpv::RunningPathway::Original(), L"the start-up file was read more than once");
				Assert::AreEqual(int(Vpv::RunningPathway::kUnknown), Vpv::RunningPathway::Value().load());
				// An explicit path latches (the profile runtime's later first snapshot latches the same way).
				Assert::IsTrue(Vpv::RunningPathway::LatchFromConfigurationFile(none.file), L"an explicit Original file did not latch");
			}
		}

		// B1: unset on Original = Bill's application value 0 (Bill has no automatic look-ahead); a set value is kept.
		TEST_METHOD(OriginalHasNoAutomaticLookahead)
		{
			LatchReset reset;
			StartupFolder none(nullptr);
			using QueueLookahead::ResolveActivePictureLookahead;
			for (const int state : { int(Vpv::RunningPathway::kUnknown), int(Vpv::RunningPathway::kVpv) })
			{
				Vpv::RunningPathway::SetForTesting(state);
				Assert::AreEqual(size_t(4), ResolveActivePictureLookahead(false, 0, false, 5, 32), L"VP-V / unknown: target 5 -> 4 (auto)");
			}
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kOriginal);
			Assert::AreEqual(size_t(0), ResolveActivePictureLookahead(false, 0, false, 5, 32), L"Original: unset is not Bill's 0");
			Assert::AreEqual(size_t(0), ResolveActivePictureLookahead(false, 0, false, 20, 32), L"Original: unset is not Bill's 0 (long queue)");
			Assert::AreEqual(size_t(3), ResolveActivePictureLookahead(true, 3, false, 5, 32), L"Original: a set look-ahead was dropped");
			Assert::AreEqual(size_t(8), ResolveActivePictureLookahead(true, 12, false, 5, 32), L"Original: a set look-ahead lost the renderer's cap");
			// Unlatched with an Original start-up file (the runtime not started): Bill's 0 too.
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown);
			std::ofstream(none.file, std::ios::trunc) << kOriginalFile;
			Assert::AreEqual(size_t(0), ResolveActivePictureLookahead(false, 0, false, 5, 32), L"an Original start-up file kept the automatic look-ahead");
		}

		// GEM-221 / MUST-8: capture_display_fixes is off on the Original pathway; on VP-V (and with no start-up file) it is the switch.
		TEST_METHOD(CaptureDisplayFixesAreOffOnOriginal)
		{
			LatchReset reset;
			{
				StartupFolder none(nullptr);
				CaptureDisplayFixes::Set(true);
				Assert::IsTrue(CaptureDisplayFixes::Enabled(), L"unknown: the switch (on) does not apply");
			}
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kVpv);
			Assert::IsTrue(CaptureDisplayFixes::Enabled(), L"VP-V: the switch (on) does not apply");
			CaptureDisplayFixes::Set(false);
			Assert::IsFalse(CaptureDisplayFixes::Enabled(), L"VP-V: the switch (off) does not apply");
			CaptureDisplayFixes::Set(true);
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kOriginal);
			Assert::IsFalse(CaptureDisplayFixes::Enabled(), L"Original: the capture / display fixes still run");
			Assert::IsTrue(CaptureDisplayFixes::Flag().load(), L"Original changed the saved switch itself");
			// The start-up target-only session: nothing latched yet, the start-up file says Original.
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown);
			StartupFolder original(kOriginalFile);
			Assert::IsFalse(CaptureDisplayFixes::Enabled(), L"before the first snapshot an Original process ran the capture / display fixes");
		}

		// S1-M2: concurrent cold-startup callers of Original() must serialize on the one-time start-up file read so
		// losing threads never observe false (VP-V) while the winning thread is loading VideoProcessor.cfg.
		TEST_METHOD(ConcurrentColdStartupOriginalCallsNeverObserveVpvOnOriginalConfig)
		{
			constexpr size_t kThreadCount = 8;
			constexpr int kOriginalRounds = 20;

			LatchReset reset;
			StartupFolder folder(kVpFile);

			auto runConcurrentColdStart = [&]() -> std::vector<int>
			{
				Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown);
				std::vector<int> observed(kThreadCount, 0);
				std::atomic<size_t> ready{ 0 };
				std::atomic<bool> go{ false };
				struct WorkerGroup
				{
					std::atomic<bool>& releaseFlag;
					std::vector<std::thread> threads;
					WorkerGroup(std::atomic<bool>& flag, size_t count) : releaseFlag(flag)
					{
						threads.reserve(count);
					}
					~WorkerGroup()
					{
						releaseFlag.store(true, std::memory_order_release);
						for (auto& worker : threads)
						{
							if (worker.joinable())
								worker.join();
						}
					}
				};
				WorkerGroup workers(go, kThreadCount);
				for (size_t i = 0; i < kThreadCount; ++i)
				{
					workers.threads.emplace_back([&, i]()
					{
						ready.fetch_add(1, std::memory_order_acq_rel);
						while (!go.load(std::memory_order_acquire))
							std::this_thread::yield();
						observed[i] = Vpv::RunningPathway::Original() ? 1 : 0;
					});
				}
				while (ready.load(std::memory_order_acquire) < kThreadCount)
					std::this_thread::yield();
				go.store(true, std::memory_order_release);
				for (auto& worker : workers.threads)
					worker.join();
				return observed;
			};

			// Negative control (same concurrent barrier harness with pipeline_original_pathway: off): every thread
			// must observe false and the latch must record kVpv, proving VideoProcessor.cfg is genuinely loaded and
			// Original() is not hardcoded to true.
			{
				const std::vector<int> vpObserved = runConcurrentColdStart();
				size_t trueObservations = 0;
				for (const int value : vpObserved)
				{
					if (value != 0)
						++trueObservations;
				}
				Assert::AreEqual(size_t(0), trueObservations,
					L"negative control (pipeline_original_pathway = off): concurrent cold-startup Original() returned true");
				Assert::AreEqual(int(Vpv::RunningPathway::kVpv), Vpv::RunningPathway::Value().load(),
					L"negative control (pipeline_original_pathway = off): start-up config did not latch VP-V");
			}

			// Fix proof: repeat 20 cold-startup rounds against an Original config. Each round writes a distinct
			// trailing comment length so ConfigFile's revision cache misses and performs a real disk read, exposing
			// the full pre-fix race window where losing threads skipped loading and returned Latched() == false.
			for (int round = 0; round < kOriginalRounds; ++round)
			{
				{
					std::ofstream out(folder.file, std::ios::trunc);
					Assert::IsTrue(out.good(), L"failed to open VideoProcessor.cfg for Original round");
					out << kOriginalFile << "# round " << round << " "
						<< std::string(static_cast<size_t>(round + 1), 'x') << "\n";
					out.flush();
					Assert::IsTrue(out.good(), L"failed to write VideoProcessor.cfg for Original round");
				}

				const std::vector<int> originalObserved = runConcurrentColdStart();
				size_t falseObservations = 0;
				for (const int value : originalObserved)
				{
					if (value == 0)
						++falseObservations;
				}
				const std::wstring message = L"round " + std::to_wstring(round) +
					L": concurrent cold-startup Original() observed false (VP-V) on " +
					std::to_wstring(falseObservations) + L" of " + std::to_wstring(kThreadCount) + L" threads";
				Assert::AreEqual(size_t(0), falseObservations, message.c_str());
				Assert::AreEqual(int(Vpv::RunningPathway::kOriginal), Vpv::RunningPathway::Value().load(),
					L"concurrent cold-startup on Original config did not latch kOriginal");
			}
		}
	};
}
