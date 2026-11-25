// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// O#2 (L1): Red Team adversarial tests for LED fade migration to shared U7 SceneOf verdict.
// Pure CPU; proves:
//   1. Default StatsBuilder::Build acquires cut/fade/fadeKeep via SceneRecordReader::AcquireScene -> IExternalDemand::SceneOf
//      in both store-on and store-off (live) modes, with clean RAII teardown of ExtensionHub & SourceFrameTap.
//   2. Private APL-slope fade detection is completely gone (steep 120 -> 20 drop over 600 ms with U7 fade=false stays false).
//   3. U7 fade=true + fadeKeep applies on frame 1 with zero history.
//   4. Unavailable U7 (sequence == 0 or SceneOf !known) reports visible sceneKnown=false, sceneWhy, SceneMissing(), and fade=false.
//   5. CompensateController::Tick with U7 fade=true on brightening fade-in never targets ledFloorBlack or holds gain, whereas
//      the exact same U7 fade=true on darkening fade-out targets ledFloorBlack and sets holdGain=true.

#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SceneRecordReader.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/drive/led_compensation/LedController.h>
#include <VP-V/modules/drive/led_measure/LedMeasurement.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <VP-V/tests/support/LedDriveTestSupport.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

namespace
{
	RawMeasurement UniformRaw(double code, uint32_t blocks = 1000)
	{
		RawMeasurement raw{};
		const int bin = (std::max)(0, (std::min)(static_cast<int>(code), HISTOGRAM_BINS - 1));
		raw.histogram[bin] = blocks;
		raw.blocks = blocks;
		raw.sum = static_cast<uint32_t>(code * static_cast<double>(SUM_SCALE) * static_cast<double>(blocks) + 0.5);
		raw.maximum = static_cast<uint32_t>(code * static_cast<double>(SUM_SCALE) + 0.5);
		return raw;
	}

	LedDriveConfig FadeTestConfig()
	{
		LedDriveConfig config;
		config.enabled = true;
		config.mode = Mode::Compensate;
		config.ledFloor = 60;
		config.ledFloorReactive = 60;
		config.ledFloorBlack = 20;
		ParseCurve("0:60, 20:60, 40:120, 60:175, 80:220, 100:255, 255:255", config.curve);
		config.rateGainUp = config.rateGainDown = 0.0;
		config.cntrAssist = false;
		config.maxCutDip = 0.0;
		config.shapedRecovery = false;
		config.blackPreliftMax = 255;
		config.aplPersistMs = 0;
		return config;
	}

	class FakeSceneDemand : public Vpv::Skeleton::IExternalDemand
	{
	public:
		void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
			const std::vector<VpSkeleton::ProductId>&) override {}
		void ClearExternalDemand(const void*, const void*) override {}

		void SetStore(const void* host, FrameAnalysis::Store* store)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			storeHost_ = host;
			store_ = store;
		}

		void SetLiveAnswer(uint64_t seq, const Vpv::Skeleton::SceneAnswer& answer)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			live_[seq] = answer;
		}

		Vpv::Skeleton::SceneAnswer SceneOf(const void* host, uint64_t seq) const override
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (store_ != nullptr && host == storeHost_)
			{
				FrameAnalysis::Scene scene;
				if (store_->PeekScene(seq, scene))
				{
					Vpv::Skeleton::SceneAnswer a;
					a.known = true;
					a.cut = scene.cut;
					a.fade = scene.fade;
					a.fadeKeep = scene.fadeKeep;
					a.confirmed = scene.confirmed;
					a.from = Vpv::Skeleton::SceneAnswer::From::Store;
					return a;
				}
			}
			const auto it = live_.find(seq);
			if (it != live_.end())
				return it->second;
			Vpv::Skeleton::SceneAnswer missing;
			missing.known = false;
			missing.why = "test: p4 pair not sealed";
			return missing;
		}

	private:
		mutable std::mutex mutex_;
		const void* storeHost_ = nullptr;
		FrameAnalysis::Store* store_ = nullptr;
		std::map<uint64_t, Vpv::Skeleton::SceneAnswer> live_;
	};

	class FakeSceneDemandModule : public Vpv::Hub::ISkeletonModule
	{
	public:
		explicit FakeSceneDemandModule(FakeSceneDemand* demandIn) : demand_(demandIn) {}
		const char* Name() const override { return "test.led_fade_u7.demand"; }
		void* Query(const char* iid) override
		{
			return iid != nullptr && std::strcmp(iid, Vpv::Skeleton::kExternalDemandQueryId) == 0
				? static_cast<Vpv::Skeleton::IExternalDemand*>(demand_) : nullptr;
		}

	private:
		FakeSceneDemand* demand_;
	};

	struct HubScope
	{
		explicit HubScope(FakeSceneDemand* demand)
		{
			Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			Vpv::Hub::ExtensionHub::Instance().AddModule(
				std::unique_ptr<Vpv::Hub::ISkeletonModule>(new FakeSceneDemandModule(demand)));
		}
		~HubScope()
		{
			Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
		}
	};

	struct TapOwnerScope
	{
		const void* owner = nullptr;
		TapOwnerScope(const void* ownerIn, FrameAnalysis::Store* store) : owner(ownerIn)
		{
			VpFrameAnalysis::SourceFrameTap::SetStore(owner, store);
		}
		~TapOwnerScope()
		{
			Clear();
		}
		void Clear()
		{
			if (owner != nullptr)
			{
				VpFrameAnalysis::SourceFrameTap::SetStore(owner, nullptr);
				owner = nullptr;
			}
		}
	};
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedFadeU7Tests)
	{
	public:
		// Real IExternalDemand::SceneOf hub integration (both store-on and store-off/live).
		TEST_METHOD(DefaultStatsBuilderReadsSharedU7SceneThroughHubStoreOnAndOff)
		{
			FakeSceneDemand demand;
			HubScope hub(&demand);
			FrameAnalysis::Store store(16);
			const int ownerTag = 0;
			const void* const owner = &ownerTag;
			demand.SetStore(owner, &store);
			TapOwnerScope tap(owner, &store);

			// 1. Store ON: publish U7 cut/fade/fadeKeep into FrameAnalysis::Store.
			store.SetEnabled(true);
			{
				FrameAnalysis::Identity id;
				id.seq = 10;
				Assert::IsTrue(store.Begin(id, FrameAnalysis::kScene) != 0);
				FrameAnalysis::Scene scene;
				scene.cut = false;
				scene.fade = true;
				scene.fadeKeep = 0.78f;
				scene.confirmed = true;
				Assert::IsTrue(store.PublishScene(10, scene));

				id.seq = 11;
				Assert::IsTrue(store.Begin(id, FrameAnalysis::kScene) != 0);
				FrameAnalysis::Scene cutScene;
				cutScene.cut = true;
				cutScene.fade = false;
				cutScene.fadeKeep = 1.0f;
				cutScene.confirmed = true;
				Assert::IsTrue(store.PublishScene(11, cutScene));
			}

			// Peek-only SceneRecordReader::SceneOf reads the store when enabled.
			{
				VpSkeleton::SceneRecordReader::SceneVerdict peeked;
				Assert::IsTrue(VpSkeleton::SceneRecordReader::SceneOf(10, peeked));
				Assert::IsFalse(peeked.cut);
				Assert::IsTrue(peeked.fade);
				Assert::AreEqual(0.78f, peeked.fadeKeep, 1e-6f);
			}

			StatsBuilder builder(FadeTestConfig());
			const FrameStats s10 = builder.Build(UniformRaw(80.0), 0.0, 0.0, 10);
			Assert::IsTrue(s10.sceneKnown, L"store-on AcquireScene must succeed");
			Assert::IsFalse(s10.cut);
			Assert::IsTrue(s10.fade);
			Assert::AreEqual(0.78f, s10.fadeKeep, 1e-6f);

			const FrameStats s11 = builder.Build(UniformRaw(80.0), 1000.0 / 24.0, 0.0, 11);
			Assert::IsTrue(s11.sceneKnown);
			Assert::IsTrue(s11.cut);
			Assert::IsFalse(s11.fade);
			Assert::AreEqual(1.0f, s11.fadeKeep, 1e-6f);

			// 2. Store OFF (live): PeekScene returns false, but default StatsBuilder::Build uses AcquireScene -> SceneOf live path.
			store.SetEnabled(false);
			{
				VpSkeleton::SceneRecordReader::SceneVerdict peeked;
				Assert::IsFalse(VpSkeleton::SceneRecordReader::SceneOf(20, peeked), L"store-off PeekScene must return false");
			}
			Vpv::Skeleton::SceneAnswer liveFade;
			liveFade.known = true;
			liveFade.cut = false;
			liveFade.fade = true;
			liveFade.fadeKeep = 0.64f;
			liveFade.from = Vpv::Skeleton::SceneAnswer::From::Live;
			demand.SetLiveAnswer(20, liveFade);

			const FrameStats s20 = builder.Build(UniformRaw(60.0), 2000.0 / 24.0, 0.0, 20);
			Assert::IsTrue(s20.sceneKnown, L"store-off live AcquireScene must still succeed");
			Assert::IsFalse(s20.cut);
			Assert::IsTrue(s20.fade);
			Assert::AreEqual(0.64f, s20.fadeKeep, 1e-6f);
			Assert::AreEqual<uint64_t>(3, builder.SceneFromRecord());
			Assert::AreEqual<uint64_t>(0, builder.SceneMissing());

			// Negative control: remove single store owner -> AcquireScene fails visibly with "no store owner".
			tap.Clear();
			const FrameStats missing = builder.Build(UniformRaw(40.0), 3000.0 / 24.0, 0.0, 20);
			Assert::IsFalse(missing.sceneKnown, L"without registered store owner, sceneKnown must be false");
			Assert::IsFalse(missing.fade);
			Assert::IsFalse(missing.cut);
			Assert::AreEqual(-1.0f, missing.fadeKeep, 1e-6f);
			Assert::AreEqual(std::string("no store owner"), std::string(missing.sceneWhy));
			Assert::AreEqual<uint64_t>(1, builder.SceneMissing());
		}

		// Negative control 1: steep monotonic APL drop (120 -> 20 over 600 ms) with U7 fade=false must NEVER trigger stats.fade.
		TEST_METHOD(PrivateAplDeclineNeverTriggersFadeWithoutU7Verdict)
		{
			const LedDriveConfig config = FadeTestConfig();
			const int totalFrames = 15; // 600 ms at 24p (far past fadeMs=250 ms, slope ~166 APL/s >> fadeMinSlope=12)
			const double stepMs = 600.0 / static_cast<double>(totalFrames - 1);

			// Unprotected/opposing inputs: exact same steep APL drop fed to two builders.
			StatsBuilder noFadeBuilder(config);
			SceneVerdicts noFadeVerdicts;
			noFadeBuilder.SetSceneVerdictReader(noFadeVerdicts.VerdictReader());

			StatsBuilder u7FadeBuilder(config);
			SceneVerdicts u7FadeVerdicts;
			for (uint64_t seq = 1; seq <= static_cast<uint64_t>(totalFrames); ++seq)
				u7FadeVerdicts.Fade(seq, 0.82f);
			u7FadeBuilder.SetSceneVerdictReader(u7FadeVerdicts.VerdictReader());

			for (int i = 0; i < totalFrames; ++i)
			{
				const double apl = 120.0 - 100.0 * static_cast<double>(i) / static_cast<double>(totalFrames - 1);
				const uint64_t seq = static_cast<uint64_t>(i + 1);
				const RawMeasurement raw = UniformRaw(apl);

				const FrameStats withoutU7 = noFadeBuilder.Build(raw, i * stepMs, stepMs, seq);
				Assert::IsTrue(withoutU7.sceneKnown);
				Assert::IsFalse(withoutU7.fade, L"Steep monotonic APL drop without U7 fade must never trip a private fade detector");
				Assert::AreEqual(-1.0f, withoutU7.fadeKeep, 1e-6f);

				// Opposing control on the EXACT same raw frame & timestamp: U7 fade=true DOES fire.
				const FrameStats withU7 = u7FadeBuilder.Build(raw, i * stepMs, stepMs, seq);
				Assert::IsTrue(withU7.sceneKnown);
				Assert::IsTrue(withU7.fade, L"Opposing control: U7 fade=true must propagate to FrameStats");
				Assert::AreEqual(0.82f, withU7.fadeKeep, 1e-6f);
			}
		}

		// Negative control 2: U7 fade=true + fadeKeep=0.85f on frame 1 (zero prior history).
		TEST_METHOD(U7FadeVerdictAppliesOnFirstFrameWithZeroHistory)
		{
			const LedDriveConfig config = FadeTestConfig();
			const RawMeasurement frame1 = UniformRaw(80.0);

			StatsBuilder verdictBuilder(config);
			SceneVerdicts verdicts;
			verdicts.Fade(1, 0.85f);
			verdictBuilder.SetSceneVerdictReader(verdicts.VerdictReader());

			const FrameStats stats = verdictBuilder.Build(frame1, 0.0, 0.0, 1);
			Assert::IsTrue(stats.sceneKnown);
			Assert::IsFalse(stats.cut);
			Assert::IsTrue(stats.fade, L"U7 fade=true must fire on frame 1 with zero APL history");
			Assert::AreEqual(0.85f, stats.fadeKeep, 1e-6f);
			Assert::AreEqual<uint64_t>(1, verdictBuilder.SceneFromRecord());

			// Negative control on exact same frame 1: legacy cut-only SceneCutFn never reports fade.
			StatsBuilder cutOnlyBuilder(config);
			cutOnlyBuilder.SetSceneReader(verdicts.Reader());
			const FrameStats cutOnly = cutOnlyBuilder.Build(frame1, 0.0, 0.0, 1);
			Assert::IsTrue(cutOnly.sceneKnown);
			Assert::IsFalse(cutOnly.fade, L"Legacy cut-only reader must leave fade=false");
			Assert::AreEqual(-1.0f, cutOnly.fadeKeep, 1e-6f);
		}

		// Negative control 3: unavailable U7 (sequence == 0 AND SceneOf returning !known with explicit cause).
		TEST_METHOD(UnavailableU7SceneReportsVisibleCauseAndZeroFade)
		{
			FakeSceneDemand demand;
			HubScope hub(&demand);
			FrameAnalysis::Store store(8);
			const int ownerTag = 0;
			demand.SetStore(&ownerTag, &store);
			TapOwnerScope tap(&ownerTag, &store);

			StatsBuilder builder(FadeTestConfig());

			// Case A: sequence == 0 -> visible "no frame sequence", fade=false, SceneMissing incremented.
			const FrameStats seqZero = builder.Build(UniformRaw(120.0), 0.0, 0.0, 0);
			Assert::IsFalse(seqZero.sceneKnown);
			Assert::IsFalse(seqZero.fade);
			Assert::IsFalse(seqZero.cut);
			Assert::AreEqual(-1.0f, seqZero.fadeKeep, 1e-6f);
			Assert::AreEqual(std::string("no frame sequence"), std::string(seqZero.sceneWhy));
			Assert::AreEqual<uint64_t>(1, builder.SceneMissing());

			// Case B: sequence != 0, SceneOf returns !known with explicit cause even across a 100-code APL plunge.
			const FrameStats unknown = builder.Build(UniformRaw(20.0), 100.0, 0.0, 12);
			Assert::IsFalse(unknown.sceneKnown);
			Assert::IsFalse(unknown.fade, L"Unavailable U7 must not fabricate fade");
			Assert::IsFalse(unknown.cut, L"Unavailable U7 must not fabricate cut");
			Assert::AreEqual(-1.0f, unknown.fadeKeep, 1e-6f);
			Assert::AreEqual(std::string("test: p4 pair not sealed"), std::string(unknown.sceneWhy));
			Assert::AreEqual(std::string("test: p4 pair not sealed"), std::string(builder.LastSceneWhy()));
			Assert::IsFalse(builder.LastSceneFromRecord());
			Assert::AreEqual<uint64_t>(2, builder.SceneMissing());
		}

		// Negative control 4: CompensateController::Tick with U7 fade=true on brightening fade-in vs darkening fade-out.
		TEST_METHOD(CompensateControllerBrighteningFadeInDoesNotTargetBlackOrHoldGain)
		{
			const LedDriveConfig config = FadeTestConfig();

			// Leg A (brightening fade-in): APL rises 20 -> 40 -> 60 -> 80 with U7 stats.fade=true on every frame.
			CompensateController fadeInController(config);
			StatsBuilder fadeInBuilder(config);
			SceneVerdicts fadeInVerdicts;
			for (uint64_t s = 11; s <= 14; ++s)
				fadeInVerdicts.Fade(s, 0.85f);
			fadeInBuilder.SetSceneVerdictReader(fadeInVerdicts.VerdictReader());

			// Settle controller at APL 20.
			for (uint64_t s = 1; s <= 10; ++s)
				fadeInController.Tick(fadeInBuilder.Build(UniformRaw(20.0), static_cast<double>(s) * 40.0, 0.0, s), 40);

			const double risingApls[4] = { 40.0, 60.0, 80.0, 100.0 };
			for (int i = 0; i < 4; ++i)
			{
				const uint64_t seq = static_cast<uint64_t>(11 + i);
				const FrameStats stats = fadeInBuilder.Build(UniformRaw(risingApls[i]), static_cast<double>(seq) * 40.0, 0.0, seq);
				Assert::IsTrue(stats.fade, L"U7 fade=true must be active on fade-in frame");
				const ControllerOutput out = fadeInController.Tick(stats, 40);
				Assert::IsFalse(out.holdGain, L"Brightening fade-in must NOT hold compensation gain");
				Assert::IsTrue(out.lightTarget > config.ledFloorBlack,
					L"Brightening fade-in must follow curve target and NEVER target ledFloorBlack");
			}

			// Leg B (opposing negative control: darkening fade-out): exact same config & U7 stats.fade=true, APL drops 120 -> 100 -> 80 -> 60.
			CompensateController fadeOutController(config);
			StatsBuilder fadeOutBuilder(config);
			SceneVerdicts fadeOutVerdicts;
			for (uint64_t s = 11; s <= 14; ++s)
				fadeOutVerdicts.Fade(s, 0.85f);
			fadeOutBuilder.SetSceneVerdictReader(fadeOutVerdicts.VerdictReader());

			// Settle controller at APL 120.
			for (uint64_t s = 1; s <= 10; ++s)
				fadeOutController.Tick(fadeOutBuilder.Build(UniformRaw(120.0), static_cast<double>(s) * 40.0, 0.0, s), 40);

			const double fallingApls[4] = { 100.0, 80.0, 60.0, 40.0 };
			for (int i = 0; i < 4; ++i)
			{
				const uint64_t seq = static_cast<uint64_t>(11 + i);
				const FrameStats stats = fadeOutBuilder.Build(UniformRaw(fallingApls[i]), static_cast<double>(seq) * 40.0, 0.0, seq);
				Assert::IsTrue(stats.fade, L"U7 fade=true must be active on fade-out frame");
				const ControllerOutput out = fadeOutController.Tick(stats, 40);
				Assert::IsTrue(out.holdGain, L"Darkening fade-out MUST hold compensation gain");
				Assert::AreEqual(config.ledFloorBlack, out.lightTarget,
					L"Darkening fade-out MUST target ledFloorBlack");
			}
		}
	};
}
