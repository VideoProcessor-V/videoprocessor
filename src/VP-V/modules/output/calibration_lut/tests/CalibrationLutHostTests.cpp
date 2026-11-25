// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutHostPolicy.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// WP-12: the decisions of the calibration host adapter (CalibrationLutHostPolicy.h). The adapter
// itself (libplacebo, D3D11) is exercised by the renderer; these pin what it decides.
namespace CalibrationLutHostTests
{
	using namespace CalibrationLutHost;

	TEST_CLASS(CalibrationLutHostPolicyTests)
	{
	public:
		TEST_METHOD(VpStageNeedsTheSettingTheSlotAndNoOriginalPathway)
		{
			StageFacts f;
			f.vpStageWanted = true;
			f.slotRuns = true;
			Assert::IsTrue(UseVpStage(f, false));
			Assert::IsFalse(UseVpStage(f, true), L"a failed stage stays on libplacebo's target LUT");
			StageFacts original = f;
			original.originalPathway = true;
			Assert::IsFalse(UseVpStage(original, false), L"the Original pathway keeps libplacebo's target LUT");
			StageFacts slotOff = f;
			slotOff.slotRuns = false;
			Assert::IsFalse(UseVpStage(slotOff, false));
			StageFacts libplacebo = f;
			libplacebo.vpStageWanted = false;
			Assert::IsFalse(UseVpStage(libplacebo, false));
		}

		TEST_METHOD(VpStageStatusIsWrittenOnlyWhenItChanges)
		{
			std::string status = "Loaded";
			Assert::IsTrue(VpStageStatus(status, "cal_bt709.cube"));
			Assert::AreEqual(std::string("Active (VP stage): cal_bt709.cube"), status);
			Assert::IsFalse(VpStageStatus(status, "cal_bt709.cube"), L"unchanged: no rewrite every frame");
			Assert::IsTrue(VpStageStatus(status, "cal_p3.cube"));
			Assert::AreEqual(std::string("Active (VP stage): cal_p3.cube"), status);
		}

		// Module findings 2026-10-24 part 3: the VP stage overwrote "Active (reload rejected: ...)"
		// every frame, so a rejected reload was never shown in the VP stage.
		TEST_METHOD(VpStageKeepsAReloadRejection)
		{
			std::string status = "Active (reload rejected: invalid cube)";
			Assert::IsTrue(VpStageStatus(status, "a.cube"));
			Assert::AreEqual(std::string("Active (VP stage; reload rejected: invalid cube)"), status);
			Assert::IsFalse(VpStageStatus(status, "a.cube"), L"kept on the next frames");
			Assert::AreEqual(std::string("Active (VP stage; reload rejected: invalid cube)"), status);
			// Back on libplacebo's target LUT: Bill's text again (his branch keeps it).
			Assert::IsTrue(LibplaceboStageStatus(status));
			Assert::AreEqual(std::string("Active (reload rejected: invalid cube)"), status);
			Assert::IsFalse(LibplaceboStageStatus(status));
			// A successful (re)load writes "Loaded": the VP stage text returns.
			status = "Loaded";
			Assert::IsTrue(VpStageStatus(status, "a.cube"));
			Assert::AreEqual(std::string("Active (VP stage): a.cube"), status);
			std::string plain = "Active: a.cube (33^3)";
			Assert::IsFalse(LibplaceboStageStatus(plain));
		}

		// Module findings 2026-10-24 part 3: "attached" read only libplacebo's target.lut, which the
		// VP stage leaves empty on purpose.
		TEST_METHOD(AttachedCountsTheVpStage)
		{
			Assert::IsTrue(LutAttached(true, true, false), L"libplacebo's target LUT");
			Assert::IsTrue(LutAttached(true, false, true), L"the VP stage");
			Assert::IsFalse(LutAttached(true, false, false));
			Assert::IsFalse(LutAttached(false, true, true), L"nothing rendered");
		}

		// Module README "Cost" item 1: while patterns show, a filled cache answers the output query
		// (the format cannot change with video paused); otherwise the renderer is asked.
		TEST_METHOD(OutputQueryUsesTheCacheWhilePatternsShow)
		{
			Assert::AreEqual(100, FreshOutputAttempts(false, 10));
			Assert::AreEqual(100, FreshOutputAttempts(true, 0), L"nothing cached yet");
			Assert::AreEqual(1, FreshOutputAttempts(true, 10), L"one try-lock, no sleep: a free lock still refreshes");
		}

		// ADR-0001: the two calibration downgrades the adapter reports (the renderer engages them).
		TEST_METHOD(CalibrationFallbackEntriesNameWhatRunsInstead)
		{
			const FallbackCatalog::Decision dropped = FallbackCatalog::CalibrationLutDropped(true, FileName("C:/vp/a.cube"));
			Assert::IsTrue(dropped.engaged);
			Assert::AreEqual(std::string("calibration LUT"), dropped.id);
			Assert::AreEqual(std::string("dropped after a render error: a.cube; no projector LUT for this renderer"), dropped.reason);
			const FallbackCatalog::Decision patch = FallbackCatalog::CalibrationPatchFullField(true, "the surface takes no blit");
			Assert::IsTrue(patch.engaged);
			Assert::AreEqual(std::string("calibration patch"), patch.id);
			Assert::IsTrue(patch.reason.find("full field") == 0);
		}

		TEST_METHOD(FileNameTakesTheLastComponent)
		{
			Assert::AreEqual(std::string("a.cube"), FileName("C:\\vp\\luts\\a.cube"));
			Assert::AreEqual(std::string("a.cube"), FileName("luts/a.cube"));
			Assert::AreEqual(std::string("a.cube"), FileName("a.cube"));
		}

		// VP's automatic reload (today's default) is the probe once a second of the LUT file the
		// renderer loaded; byte-for-byte the renderer's old MaybeReloadDisplayLut schedule.
		TEST_METHOD(AutoReloadProbesOnceASecondAndReloadsOnAChangedFile)
		{
			ReloadPolicy policy;
			Assert::IsTrue(policy.Mode() == ReloadMode::Auto);
			policy.NoteLoaded(true, 100, 5, 10000);
			Assert::IsFalse(policy.ProbeDue(true, true, 10500), L"not before a second");
			Assert::IsTrue(policy.ProbeDue(true, true, 11000));
			Assert::IsFalse(policy.ProbeDue(true, true, 11500));
			Assert::IsTrue(policy.ProbeDue(true, true, 12000));
			Assert::IsFalse(policy.ProbeDue(false, true, 99999), L"no file selected");
			Assert::IsFalse(policy.ProbeDue(true, false, 99999), L"the LUT is disabled");
			Assert::IsFalse(policy.Changed(true, 100, 5));
			Assert::IsTrue(policy.Changed(true, 101, 5));
			Assert::IsTrue(policy.Changed(true, 100, 6));
			Assert::IsTrue(policy.Changed(false, 0, 0), L"the file disappeared");
		}

		// Bill's explicit Apply (the other choice, either/or): the file is never probed; only VP's own
		// calibration may load its finished LUT without a Config Apply, once per request, with a notice.
		TEST_METHOD(ExplicitApplyNeverProbesButTakesACalibrationReloadOnce)
		{
			ReloadPolicy policy;
			policy.SetMode(ReloadMode::ExplicitApply);
			policy.NoteLoaded(true, 100, 5, 0);
			Assert::IsFalse(policy.ProbeDue(true, true, 100000));
			Assert::IsFalse(policy.TakeCalibrationReload());
			policy.RequestCalibrationReload();
			Assert::IsTrue(policy.TakeCalibrationReload());
			Assert::IsFalse(policy.TakeCalibrationReload(), L"once per request");
			Assert::IsTrue(std::string(ReloadPolicy::CalibrationReloadNotice()).find("no Config Apply") != std::string::npos);
		}

		TEST_METHOD(OutputFactsMatchTheSwapchainFormat)
		{
			Assert::AreEqual(10u, ContainerBits(DXGI_FORMAT_R10G10B10A2_UNORM));
			Assert::AreEqual(16u, ContainerBits(DXGI_FORMAT_R16G16B16A16_FLOAT));
			Assert::AreEqual(8u, ContainerBits(DXGI_FORMAT_B8G8R8A8_UNORM));
			Assert::AreEqual(8u, ContainerBits(DXGI_FORMAT_R8G8B8A8_UNORM));
			Assert::AreEqual(0u, ContainerBits(DXGI_FORMAT_UNKNOWN));
			Assert::AreEqual(8u, CodeBits(10, "8"), L"8-in-10");
			Assert::AreEqual(10u, CodeBits(10, "10"));
			Assert::AreEqual(8u, CodeBits(8, "10"));
			Assert::AreEqual(10u, CodeBits(10, "auto"));
			Assert::AreEqual(16u, CodeBits(16, "8"), L"float containers are not limited (patterns refuse them)");
			Assert::AreEqual(1023.0, PatchMaxCode(DXGI_FORMAT_R10G10B10A2_UNORM));
			Assert::AreEqual(255.0, PatchMaxCode(DXGI_FORMAT_B8G8R8A8_UNORM));
			Assert::AreEqual(0.0, PatchMaxCode(DXGI_FORMAT_R16G16B16A16_FLOAT));
		}

		// GEM-188: RequestCalibrationReload() (any thread) and TakeCalibrationReload() (render thread)
		// must synchronize atomically so every single request is consumed by exactly one racing consumer
		// (never 0, never 2 via a non-atomic read-then-write race).
		TEST_METHOD(CalibrationReloadFlagIsAtomicAcrossThreadsAndConsumedExactlyOnce)
		{
			struct ConsumersJoinGuard
			{
				std::atomic<int>& epoch;
				std::vector<std::thread>& threads;
				ConsumersJoinGuard(std::atomic<int>& epochIn, std::vector<std::thread>& threadsIn)
					: epoch(epochIn), threads(threadsIn)
				{
				}
				ConsumersJoinGuard(const ConsumersJoinGuard&) = delete;
				ConsumersJoinGuard& operator=(const ConsumersJoinGuard&) = delete;
				~ConsumersJoinGuard()
				{
					epoch.store(-1, std::memory_order_release);
					for (std::thread& t : threads)
					{
						if (t.joinable())
							t.join();
					}
				}
			};

			ReloadPolicy policy;
			constexpr int kNegativeControlRounds = 64;
			constexpr int kStressRounds = 10000;

			std::atomic<int> roundEpoch{ 0 };
			std::atomic<int> roundDone{ 0 };
			std::atomic<int> consumersReady{ 0 };
			std::atomic<int> consumerResult0{ 0 };
			std::atomic<int> consumerResult1{ 0 };
			std::atomic<bool> consumerThrew{ false };

			std::vector<std::thread> consumers;
			consumers.reserve(2);
			ConsumersJoinGuard joinGuard(roundEpoch, consumers);

			for (int idx = 0; idx < 2; ++idx)
			{
				consumers.emplace_back([&, idx]()
				{
					std::atomic<int>* const resultSlot = (idx == 0) ? &consumerResult0 : &consumerResult1;
					try
					{
						int lastRound = 0;
						consumersReady.fetch_add(1, std::memory_order_acq_rel);
						for (;;)
						{
							int r = roundEpoch.load(std::memory_order_acquire);
							int spins = 0;
							while (r == lastRound)
							{
								if (++spins >= 1024)
								{
									std::this_thread::yield();
									spins = 0;
								}
								r = roundEpoch.load(std::memory_order_acquire);
							}
							if (r < 0)
								break;
							lastRound = r;
							const bool taken = policy.TakeCalibrationReload();
							resultSlot->store(taken ? 1 : 0, std::memory_order_release);
							roundDone.fetch_add(1, std::memory_order_acq_rel);
						}
					}
					catch (...)
					{
						consumerThrew.store(true, std::memory_order_release);
						roundDone.fetch_add(1, std::memory_order_acq_rel);
					}
				});
			}

			while (consumersReady.load(std::memory_order_acquire) < 2 &&
				!consumerThrew.load(std::memory_order_acquire))
				std::this_thread::yield();

			int currentEpoch = 0;
			auto runRound = [&](bool requestReload) -> int
			{
				roundDone.store(0, std::memory_order_relaxed);
				consumerResult0.store(0, std::memory_order_relaxed);
				consumerResult1.store(0, std::memory_order_relaxed);
				if (requestReload)
					policy.RequestCalibrationReload();
				++currentEpoch;
				roundEpoch.store(currentEpoch, std::memory_order_release);
				int spins = 0;
				while (roundDone.load(std::memory_order_acquire) < 2 &&
					!consumerThrew.load(std::memory_order_acquire))
				{
					if (++spins >= 1024)
					{
						std::this_thread::yield();
						spins = 0;
					}
				}
				return consumerResult0.load(std::memory_order_acquire) +
					consumerResult1.load(std::memory_order_acquire);
			};

			// Negative control: with no RequestCalibrationReload() call, both racing consumers must return false.
			int negativeControlTrueCount = 0;
			for (int i = 0; i < kNegativeControlRounds; ++i)
				negativeControlTrueCount += runRound(false);
			Assert::AreEqual(0, negativeControlTrueCount,
				L"negative control: without RequestCalibrationReload(), both racing consumers must return false");

			// Synchronized 10,000-round stress: producer calls RequestCalibrationReload() once per round and
			// two consumer threads race on TakeCalibrationReload() -> strictly 1 consumer must return true.
			int zeroTakenRounds = 0;
			int doubleTakenRounds = 0;
			int exactOnceRounds = 0;
			for (int i = 0; i < kStressRounds; ++i)
			{
				const int taken = runRound(true);
				if (taken == 0)
					++zeroTakenRounds;
				else if (taken == 1)
					++exactOnceRounds;
				else
					++doubleTakenRounds;
			}

			// Trailing negative control: after the final round is consumed, no stale reload flag remains.
			Assert::AreEqual(0, runRound(false),
				L"no stale reload flag may remain after synchronized consumption");

			roundEpoch.store(-1, std::memory_order_release);
			for (std::thread& t : consumers)
			{
				if (t.joinable())
					t.join();
			}

			Assert::IsFalse(consumerThrew.load(std::memory_order_acquire),
				L"consumer threads must not throw during concurrent TakeCalibrationReload() stress");
			Assert::AreEqual(0, zeroTakenRounds,
				L"every RequestCalibrationReload() round must be observed by a consumer (never 0)");
			Assert::AreEqual(0, doubleTakenRounds,
				L"each RequestCalibrationReload() round must be consumed by at most one consumer (never 2)");
			Assert::AreEqual(kStressRounds, exactOnceRounds,
				L"all 10,000 rounds must be consumed by exactly 1 of the 2 racing consumers");
		}

		// GEM-197: SlopeFallbackDecision must be strictly zero-gated on CalibrationLutStage::SlopeFallbacks()
		// so "BTB/WTW carried at slope 1" is never surfaced when edge-slope extrapolation succeeded (count == 0),
		// engages visibly when slopeFallbacks > 0, stays idempotent without heading churn, and releases cleanly at 0.
		TEST_METHOD(SlopeFallbackDecisionIsZeroGatedAndSurfacesOnlyWhenSlopeFallbacksOccur)
		{
			FallbackStatus::Registry registry;

			// 1. Negative control (slopeFallbacks == 0): decision must remain released and leave heading at none.
			const FallbackCatalog::Decision zero = SlopeFallbackDecision(0);
			Assert::IsFalse(zero.engaged,
				L"negative control: SlopeFallbackDecision(0) must NOT be engaged");
			Assert::AreEqual(std::string("calibration LUT BTB/WTW"), zero.id,
				L"negative control: SlopeFallbackDecision(0) must carry stable id");
			Assert::IsTrue(zero.reason.empty(),
				L"negative control: released SlopeFallbackDecision(0) must have empty reason");
			Assert::IsFalse(FallbackCatalog::Sync(registry, zero),
				L"negative control: syncing released decision into clean registry changes nothing");
			Assert::IsFalse(registry.Engaged("calibration LUT BTB/WTW"),
				L"negative control: registry must not engage calibration LUT BTB/WTW at 0");
			Assert::IsTrue(registry.Entries().empty(),
				L"negative control: registry entries must remain empty at 0");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), registry.Heading(),
				L"negative control: heading must remain \"Fallbacks engaged: none\" at 0");
			Assert::IsTrue(registry.Heading().find("BTB/WTW carried at slope 1") == std::string::npos,
				L"negative control: heading must not mention slope 1 at 0");

			// 2. Fix proof (slopeFallbacks == 1 and 42): decision must engage with slope-1 reason and surface in Heading().
			const FallbackCatalog::Decision one = SlopeFallbackDecision(1);
			Assert::IsTrue(one.engaged,
				L"SlopeFallbackDecision(1) must be engaged");
			Assert::AreEqual(std::string("calibration LUT BTB/WTW"), one.id,
				L"SlopeFallbackDecision(1) must carry stable id");
			Assert::IsTrue(one.reason.find("BTB/WTW carried at slope 1") != std::string::npos,
				L"SlopeFallbackDecision(1) reason must contain \"BTB/WTW carried at slope 1\"");
			Assert::IsTrue(FallbackCatalog::Sync(registry, one),
				L"syncing engaged decision into clean registry must report heading change");
			Assert::IsTrue(registry.Engaged("calibration LUT BTB/WTW"),
				L"registry must engage calibration LUT BTB/WTW when slopeFallbacks == 1");
			Assert::IsTrue(registry.Heading().find("calibration LUT BTB/WTW") != std::string::npos,
				L"registry.Heading() must surface \"calibration LUT BTB/WTW\"");
			Assert::IsTrue(registry.Heading().find("BTB/WTW carried at slope 1") != std::string::npos,
				L"registry.Heading() must include \"BTB/WTW carried at slope 1\"");

			const FallbackCatalog::Decision many = SlopeFallbackDecision(42);
			Assert::IsTrue(many.engaged,
				L"SlopeFallbackDecision(42) must be engaged");
			Assert::AreEqual(std::string("calibration LUT BTB/WTW"), many.id,
				L"SlopeFallbackDecision(42) must carry stable id");
			Assert::IsTrue(many.reason.find("BTB/WTW carried at slope 1") != std::string::npos,
				L"SlopeFallbackDecision(42) reason must contain \"BTB/WTW carried at slope 1\"");
			Assert::IsFalse(FallbackCatalog::Sync(registry, many),
				L"subsequent non-zero counts must not churn the heading");
			Assert::IsTrue(registry.Engaged("calibration LUT BTB/WTW"),
				L"registry must remain engaged at slopeFallbacks == 42");

			// 3. Release round-trip: syncing SlopeFallbackDecision(0) must release back to "Fallbacks engaged: none".
			Assert::IsTrue(FallbackCatalog::Sync(registry, SlopeFallbackDecision(0)),
				L"syncing SlopeFallbackDecision(0) after engagement must release and report heading change");
			Assert::IsFalse(registry.Engaged("calibration LUT BTB/WTW"),
				L"calibration LUT BTB/WTW must be released when count returns to 0");
			Assert::IsTrue(registry.Entries().empty(),
				L"registry entries must be empty after release");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), registry.Heading(),
				L"heading must return to \"Fallbacks engaged: none\" after release");
			Assert::IsFalse(FallbackCatalog::Sync(registry, SlopeFallbackDecision(0)),
				L"releasing twice changes nothing");
		}
	};
}
