// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// FIX-LED-CUT-U7 (owner answer 2026-11-15 (2), plan lead18/coreplan-C10-15c-PC-regions.md C8): the LED's cut is the shared U7
// scene verdict, read per source sequence for the measured frame AND every walked (ahead) frame. These tests cover what the
// reader seam adds: a genuine failure judges no cut and shows a visible entry with its cause (one for the measured frame,
// one for the look-ahead, judged once per walk); the walk asks the verdict once per queued frame and an ahead cut reaches
// the controller (the LED lands on the cut frame; the cut-lead window jumps); the module declares its P4 demand on the
// ONE registered renderer once, and again after a renderer change (never per frame), and withdraws it on stop.
// CPU only: no projector (a fake port), no GPU, no skeleton module (a fake demand entry behind the hub's query).

#include "pch.h"
#include "CppUnitTest.h"

#include <AnalysisLumaSource.h>
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/drive/led_compensation/LedController.h>
#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>
#include <VP-V/modules/drive/led_measure/LedLookahead.h>
#include <VP-V/modules/drive/led_measure/LedMeasurement.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <VP-V/tests/support/LedDriveTestSupport.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

namespace
{
	constexpr double CUT_FRAME_MS = 1000.0 / 60.0;

	// One frame: every block at `code` (the cut cells too).
	RawMeasurement CutFrame(double code, uint32_t blocks = 10000)
	{
		RawMeasurement raw{};
		raw.histogram[static_cast<int>(code)] += blocks;
		raw.blocks = blocks;
		raw.sum = static_cast<uint32_t>(code * blocks * SUM_SCALE);
		for (int i = 0; i < GRID_W * GRID_H; ++i)
		{
			raw.cellCount[i] = 4;
			raw.cellSum[i] = static_cast<uint32_t>(4 * code * SUM_SCALE);
		}
		return raw;
	}

	LedDriveConfig CutCompensate()
	{
		LedDriveConfig config;
		config.enabled = true;
		config.mode = Mode::Compensate;
		config.ledFloor = 60;
		config.ledFloorReactive = 60;
		ParseCurve("0:60, 20:60, 40:200, 70:230, 100:255, 255:255", config.curve);
		config.rateGainUp = config.rateGainDown = 0.0;
		config.cntrAssist = false;
		config.maxCutDip = 0.0;
		config.shapedRecovery = false;
		config.blackPreliftMax = 255;
		return config;
	}

	ProjectorConfig CutPort(int latencyMs)
	{
		ProjectorConfig port;
		port.protocol = "barco_ascii";
		port.ackTimeoutMs = 60;
		port.minCommandGapMs = 0;
		port.readBack = true;
		port.ledLatencyMs = latencyMs;
		port.ledSettleMs = 0;
		return port;
	}

	// A captured UYVY frame at luma `y` everywhere (the walk measures it through the raw path).
	struct CutLumaFrame
	{
		static constexpr int W = 64, H = 32;
		std::vector<uint8_t> pixels;
		AnalysisLumaSource luma;
		QueuedFrameStatistics::PictureRect rect;

		explicit CutLumaFrame(uint8_t y)
			: pixels(static_cast<size_t>(W) * H * 2, 128)
		{
			for (int row = 0; row < H; ++row)
				for (int x = 0; x < W; ++x)
					pixels[static_cast<size_t>(row) * W * 2 + x * 2 + 1] = y;
			luma.data = pixels.data();
			luma.dataBytes = pixels.size();
			luma.width = W;
			luma.height = H;
			luma.rowBytes = W * 2;
			luma.format = AnalysisLumaFormat::NativeYuv422;
			luma.encoding = VideoFrameEncoding::UYVY;
			rect.right = W;
			rect.bottom = H;
		}
	};

	// Real-time waits only for what needs no time on the drive's clock (a port write and its echo).
	bool CutWaitFor(const std::function<bool()>& condition, int timeoutMs = 10000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (condition()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return condition();
	}

	const FallbackStatus::Entry* EntryOf(const std::vector<FallbackStatus::Entry>& entries, const char* id)
	{
		for (const FallbackStatus::Entry& e : entries)
			if (e.id == id)
				return &e;
		return nullptr;
	}

	// The skeleton's external-demand entry, faked: it records every declaration and withdrawal.
	class RecordingDemand : public Vpv::Skeleton::IExternalDemand
	{
	public:
		struct Declared
		{
			const void* host = nullptr;
			const void* owner = nullptr;
			size_t consumers = 0;
			std::string name;
			uint8_t reads = 0;
			VpSkeleton::ProductId firstProduct{};
			uint8_t firstLag = 0, secondLag = 0;
		};

		void SetExternalDemand(const void* host, const void* owner, const std::vector<VpSkeleton::ConsumerDecl>& consumers,
			const std::vector<VpSkeleton::ProductId>&) override
		{
			std::lock_guard<std::mutex> guard(mutex);
			Declared d;
			d.host = host;
			d.owner = owner;
			d.consumers = consumers.size();
			if (!consumers.empty())
			{
				d.name = consumers[0].name != nullptr ? consumers[0].name : "";
				d.reads = consumers[0].count;
				d.firstProduct = consumers[0].consumes[0].product;
				d.firstLag = consumers[0].consumes[0].lag;
				d.secondLag = consumers[0].consumes[1].lag;
			}
			declared.push_back(d);
		}
		void ClearExternalDemand(const void* host, const void* owner) override
		{
			std::lock_guard<std::mutex> guard(mutex);
			cleared.push_back(std::make_pair(host, owner));
		}
		std::vector<Declared> Declarations()
		{
			std::lock_guard<std::mutex> guard(mutex);
			return declared;
		}
		std::vector<std::pair<const void*, const void*>> Withdrawals()
		{
			std::lock_guard<std::mutex> guard(mutex);
			return cleared;
		}

	private:
		std::mutex mutex;
		std::vector<Declared> declared;
		std::vector<std::pair<const void*, const void*>> cleared;
	};

	class DemandModule : public Vpv::Hub::ISkeletonModule
	{
	public:
		explicit DemandModule(RecordingDemand* demandIn) : demand(demandIn) {}
		const char* Name() const override { return "test.led_cut_judge.demand"; }
		void* Query(const char* iid) override
		{
			return iid != nullptr && std::strcmp(iid, Vpv::Skeleton::kExternalDemandQueryId) == 0
				? static_cast<Vpv::Skeleton::IExternalDemand*>(demand) : nullptr;
		}

	private:
		RecordingDemand* demand;
	};

	// The fake demand entry is the hub's only module for the test (the IngressPolicyTests pattern); the hub is left empty after.
	struct DemandHubScope
	{
		explicit DemandHubScope(RecordingDemand* demand)
		{
			Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new DemandModule(demand)));
		}
		~DemandHubScope() { Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); }
	};

	// Takes a store registration off the tap again however the test ends.
	struct StoreRegistration
	{
		const void* owner = nullptr;
		~StoreRegistration()
		{
			if (owner != nullptr)
				VpFrameAnalysis::SourceFrameTap::SetStore(owner, nullptr);
		}
		void Set(const void* ownerIn, FrameAnalysis::Store* store)
		{
			if (owner != nullptr && owner != ownerIn)
				VpFrameAnalysis::SourceFrameTap::SetStore(owner, nullptr);
			owner = ownerIn;
			VpFrameAnalysis::SourceFrameTap::SetStore(owner, store);
		}
	};
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedCutJudgeTests)
	{
	public:
		// T-1 (builder): a frame whose verdict could not be got judges NO cut, however large the picture change, and names why.
		TEST_METHOD(AFrameWithoutAVerdictJudgesNoCutAndSaysWhy)
		{
			StatsBuilder builder{ CutCompensate() };
			SceneVerdicts verdicts;
			verdicts.Cut(2).Unknown(2, "test: no analysis lane");
			builder.SetSceneReader(verdicts.Reader());
			const FrameStats first = builder.Build(CutFrame(16.0), 0.0, 0.0, 1);
			Assert::IsTrue(first.sceneKnown);
			const FrameStats unknown = builder.Build(CutFrame(200.0), CUT_FRAME_MS, 0.0, 2);
			Assert::IsFalse(unknown.cut, L"no verdict: no cut (never a private re-judgement of the 16 -> 200 change)");
			Assert::IsFalse(unknown.sceneKnown);
			Assert::AreEqual(std::string("test: no analysis lane"), std::string(unknown.sceneWhy));
			Assert::IsFalse(builder.LastSceneFromRecord());
			Assert::AreEqual(std::string("test: no analysis lane"), std::string(builder.LastSceneWhy()));
			Assert::AreEqual<uint64_t>(1, builder.SceneMissing());
			Assert::AreEqual<uint64_t>(1, builder.SceneFromRecord());
			// Sequence 0 (no frame key): nothing to ask, judged unknown with its own cause; the reader is not called.
			const size_t calls = verdicts.Calls();
			const FrameStats unkeyed = builder.Build(CutFrame(16.0), 2 * CUT_FRAME_MS);
			Assert::IsFalse(unkeyed.cut);
			Assert::IsFalse(unkeyed.sceneKnown);
			Assert::AreEqual(std::string("no frame sequence"), std::string(unkeyed.sceneWhy));
			Assert::AreEqual(calls, verdicts.Calls(), L"no verdict asked without a sequence");
			// The verdict is back: answered (and the 200 -> 16 change judged by it alone: no cut here).
			const FrameStats back = builder.Build(CutFrame(16.0), 3 * CUT_FRAME_MS, 0.0, 3);
			Assert::IsTrue(back.sceneKnown);
			Assert::AreEqual(std::string(), std::string(back.sceneWhy));
			Assert::IsTrue(builder.LastSceneFromRecord());
		}

		// T-1 (measured frame): only a genuine failure engages "LED cut judge", with the cause in its text; a known frame
		// releases it.
		TEST_METHOD(AMeasuredFrameWithoutASceneShowsTheCutJudgeEntryUntilOneIsGot)
		{
			LedDriveModule module(ProjectorConfig(), CutCompensate());
			SceneVerdicts verdicts;
			verdicts.Unknown(3, "test cause").Unknown(4, "other cause");
			module.SetSceneReaderForTesting(verdicts.Reader());
			module.SubmitMeasurement(CutFrame(40.0), 1);
			module.SubmitMeasurement(CutFrame(40.0), 2);
			Assert::IsNull(EntryOf(module.FallbackEntries(), LedDriveModule::kCutJudgeFallbackId),
				L"every scene got: no entry (genuine failure only)");
			module.SubmitMeasurement(CutFrame(40.0), 3);
			const std::vector<FallbackStatus::Entry> engaged = module.FallbackEntries();
			const FallbackStatus::Entry* entry = EntryOf(engaged, LedDriveModule::kCutJudgeFallbackId);
			Assert::IsNotNull(entry, L"no scene for the measured frame: visible");
			Assert::IsTrue(entry->reason.find("(test cause)") != std::string::npos, std::wstring(entry->reason.begin(), entry->reason.end()).c_str());
			Assert::IsNull(EntryOf(engaged, LedDriveModule::kCutJudgeAheadFallbackId), L"the look-ahead entry is its own");
			module.SubmitMeasurement(CutFrame(40.0), 4);
			Assert::IsNotNull(EntryOf(module.FallbackEntries(), LedDriveModule::kCutJudgeFallbackId), L"still unknown: still shown");
			module.SubmitMeasurement(CutFrame(40.0), 5);
			Assert::IsNull(EntryOf(module.FallbackEntries(), LedDriveModule::kCutJudgeFallbackId), L"a scene got again: released");
			Assert::IsTrue(module.FallbackEntries().empty());
		}

		// T-1 (look-ahead, per-walk aggregation): the first failing frame of a walk engages "LED look-ahead cut judge" (its cause
		// kept); known frames never release it mid-walk; it is released only at the next walk's start after a whole walk with
		// predictions was known; a walk with no predictions releases nothing.
		TEST_METHOD(TheLookaheadEntryIsJudgedOncePerWalk)
		{
			LedDriveModule module(ProjectorConfig(), CutCompensate());
			StatsBuilder builder(module.Config());
			SceneVerdicts verdicts;
			verdicts.Unknown(3, "first cause").Unknown(4, "second cause");
			builder.SetSceneReader(verdicts.Reader());
			auto predict = [&](uint64_t sequence)
			{
				module.SubmitPrediction(sequence, builder.Build(CutFrame(40.0), sequence * CUT_FRAME_MS, 0.0, sequence));
			};
			auto aheadEntry = [&] { return EntryOf(module.FallbackEntries(), LedDriveModule::kCutJudgeAheadFallbackId); };

			module.SetLookaheadDepth(6);   // walk 1
			predict(1);
			predict(2);
			Assert::IsNull(aheadEntry(), L"every walked scene got: no entry");
			predict(3);
			Assert::IsNotNull(aheadEntry(), L"the first failing frame of the walk engages it");
			predict(4);
			predict(5);
			const FallbackStatus::Entry* engaged = aheadEntry();
			Assert::IsNotNull(engaged, L"a known frame later in the walk does not release it");
			Assert::IsTrue(engaged->reason.find("(first cause)") != std::string::npos, L"the first cause of the walk is kept");
			Assert::IsNull(EntryOf(module.FallbackEntries(), LedDriveModule::kCutJudgeFallbackId), L"the measured entry is its own");

			module.SetLookaheadDepth(6);   // walk 2: walk 1 failed, nothing released
			Assert::IsNotNull(aheadEntry());
			module.SetLookaheadDepth(6);   // walk 3: walk 2 had no predictions, nothing released
			Assert::IsNotNull(aheadEntry(), L"a walk without predictions does not release");
			predict(6);
			predict(7);
			Assert::IsNotNull(aheadEntry(), L"never released mid-walk");
			module.SetLookaheadDepth(6);   // walk 4: walk 3 was wholly known
			Assert::IsNull(aheadEntry(), L"released after one whole walk with every scene known");
			Assert::IsTrue(module.FallbackEntries().empty());
		}

		// The walk (LookaheadObserver, the module's reader): each queued frame's verdict is asked ONCE, the predicted cut is the
		// verdict of exactly that frame, and a second walk over the same queue asks nothing again.
		TEST_METHOD(TheWalkAsksEachQueuedFrameOnceAndPredictsTheVerdict)
		{
			LedDriveModule module(ProjectorConfig(), CutCompensate());
			const uint64_t cut = 20;
			SceneVerdicts verdicts;
			verdicts.Cut(cut);
			module.SetSceneReaderForTesting(verdicts.Reader());
			const CutLumaFrame bright(120), dark(18);
			module.Lookahead().BeginWalk(40);
			for (uint64_t s = 1; s < 40; ++s)
				module.Lookahead().ObserveFrame(s, static_cast<size_t>(s - 1), s < cut ? bright.luma : dark.luma, bright.rect);
			for (uint64_t s = 1; s < 40; ++s)
			{
				FrameStats predicted;
				Assert::IsTrue(module.PredictionFor(s, predicted), std::to_wstring(s).c_str());
				Assert::IsTrue(predicted.sceneKnown);
				Assert::AreEqual(s == cut, predicted.cut, (L"the walked verdict of frame " + std::to_wstring(s)).c_str());
				Assert::AreEqual<size_t>(1, verdicts.CallsFor(s), L"asked once per queued frame");
			}
			const size_t calls = verdicts.Calls();
			module.Lookahead().BeginWalk(40);
			for (uint64_t s = 1; s < 40; ++s)
				module.Lookahead().ObserveFrame(s, static_cast<size_t>(s - 1), s < cut ? bright.luma : dark.luma, bright.rect);
			Assert::AreEqual(calls, verdicts.Calls(), L"a frame already walked is not judged again");
			module.Lookahead().BeginWalk(40);
			Assert::IsNull(EntryOf(module.FallbackEntries(), LedDriveModule::kCutJudgeAheadFallbackId));
		}

		// T-3: an ahead cut end to end. The walk's verdict (cut at `cut`, 3 frames of LED latency ahead) lands the LED on the cut
		// frame; nothing lands before it (the same law as WithLookaheadTheCommandPrecedesItsFrameByLedLatency, here through the
		// real walk and the module's reader rather than hand-made predictions).
		TEST_METHOD(AnAheadCutFromTheWalkLandsTheLedOnTheCutFrame)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			TestClock clock;
			LedDriveModule module(CutPort(50), CutCompensate(),
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			const uint64_t cut = 20;
			SceneVerdicts verdicts;
			verdicts.Cut(cut);
			module.SetSceneReaderForTesting(verdicts.Reader());
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(CutWaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(CutFrame(120.0), 1);
			const CutLumaFrame bright(120), dark(18);
			module.Lookahead().BeginWalk(3);
			for (uint64_t s = 1; s < 40; ++s)
				module.Lookahead().ObserveFrame(s, static_cast<size_t>(s - 1), s < cut ? bright.luma : dark.luma, bright.rect);
			FrameStats atCut;
			Assert::IsTrue(module.PredictionFor(cut, atCut) && atCut.cut, L"the walk predicted the cut");
			std::vector<double> levels(1, 255.0), gains;
			for (uint64_t s = 1; s <= cut + 2; ++s)
			{
				clock.To(s * CUT_FRAME_MS);
				const CompensationGain gain = module.BeginFrameAt(s, s * CUT_FRAME_MS);
				levels.push_back(module.ModelledLight());
				gains.push_back(gain.gain);
				clock.Settle();
			}
			Assert::AreEqual(3, module.LatencyFrames());
			for (uint64_t s = 1; s < cut; ++s)
				Assert::AreEqual(255.0, levels[s], 1e-12, (L"nothing lands before the cut, frame " + std::to_wstring(s)).c_str());
			Assert::IsTrue(levels[cut] < 255.0, L"sent latency frames ahead, the LED lands on the cut frame");
			Assert::AreEqual(255.0 / levels[cut], gains[cut - 1], 1e-9, L"and so does the gain");
			Assert::IsNull(EntryOf(module.FallbackEntries(), LedDriveModule::kCutJudgeAheadFallbackId));
			module.Stop();
		}

		// T-3 (cut-lead window, LedController.cpp cut-lead): the walked verdict is what opens the window. The same queued bright
		// frame with the judge's cut opens it (the jump goes out as the cut enters the window); judged no cut, it does not.
		TEST_METHOD(TheWalkedVerdictOpensTheCutLeadWindow)
		{
			for (const bool judgedCut : { true, false })
			{
				LedDriveConfig config = CutCompensate();
				config.cutLeadWindow = true;
				LedDriveModule module(ProjectorConfig(), config);
				const uint64_t cut = 9;
				SceneVerdicts verdicts;
				if (judgedCut) verdicts.Cut(cut);
				module.SetSceneReaderForTesting(verdicts.Reader());
				const CutLumaFrame dim(30), bright(120);
				module.Lookahead().BeginWalk(8);
				for (uint64_t s = 1; s <= cut; ++s)
					module.Lookahead().ObserveFrame(s, static_cast<size_t>(s - 1), s < cut ? dim.luma : bright.luma, dim.rect);
				FrameStats ahead;
				Assert::IsTrue(module.PredictionFor(cut, ahead));
				Assert::AreEqual(judgedCut, ahead.cut);
				Lookahead look;
				look.stats = ahead;
				look.cutAhead = ahead.cut;   // as BeginFrameAt ORs the predicted cuts up to the horizon
				look.framesAhead = 8;
				StatsBuilder scene(config);
				scene.SetSceneReader(NoSceneCuts());
				FrameStats onScreen;
				for (uint64_t s = 1; s <= 60; ++s)
					onScreen = scene.Build(CutFrame(30.0), s * CUT_FRAME_MS, 0.0, s);
				CompensateController controller(config);
				controller.Reset(130);   // the APL-30 scene's level
				const ControllerOutput out = controller.Tick(onScreen, 16.7, &look, true, true);
				if (judgedCut)
				{
					Assert::AreEqual(255, out.lightCommand, L"the judged cut: the jump as it enters the window");
					Assert::IsTrue(out.jump && out.cutLeadHold);
				}
				else
					Assert::IsFalse(out.cutLeadHold, L"judged no cut: no cut-lead window");
			}
		}

		// T-5: the P4 demand (lag 0 + lag 1) is declared once on the ONE registered renderer, never again per frame; after a
		// renderer change (the same address registered again = a new generation, or another renderer) it is declared once more on
		// the current one (SetExternalDemand count 1 per change); Stop withdraws it from the renderer it was declared on.
		TEST_METHOD(TheDemandIsDeclaredOncePerRendererAndAgainAfterAChange)
		{
			Assert::IsTrue(VpFrameAnalysis::SourceFrameTap::StoreOwners().empty(),
				L"precondition: no store registered by an earlier test");
			RecordingDemand demand;
			DemandHubScope hub(&demand);
			int rendererA = 0, rendererB = 0;
			FrameAnalysis::Store storeA(FrameAnalysis::RingDepth(4, 4)), storeB(FrameAnalysis::RingDepth(4, 4));
			StoreRegistration registration;
			registration.Set(&rendererA, &storeA);

			auto projector = std::make_shared<FakeProjector>();
			projector->openFails = true;   // a dry run: no port
			LedDriveModule module(ProjectorConfig(), CutCompensate(),
				[projector] { return std::make_unique<FakePort>(projector); });
			module.SetSceneReaderForTesting(NoSceneCuts());
			module.Start();
			std::vector<RecordingDemand::Declared> declared = demand.Declarations();
			Assert::AreEqual<size_t>(1, declared.size(), L"declared with the module");
			Assert::IsTrue(declared[0].host == &rendererA);
			Assert::IsTrue(declared[0].owner == static_cast<const void*>(&module));
			Assert::AreEqual<size_t>(1, declared[0].consumers);
			Assert::AreEqual(std::string("LED look-ahead cut"), declared[0].name);
			Assert::AreEqual<int>(2, declared[0].reads, L"P4 at lag 0 and lag 1");
			Assert::IsTrue(declared[0].firstProduct == VpSkeleton::kP4SourceStats);
			Assert::AreEqual<int>(0, declared[0].firstLag);
			Assert::AreEqual<int>(1, declared[0].secondLag);

			for (uint64_t s = 1; s <= 5; ++s)
				module.SubmitMeasurement(CutFrame(40.0), s);
			Assert::AreEqual<size_t>(1, demand.Declarations().size(), L"never per frame");

			// The renderer goes away and one is made at the same address: a new registration generation.
			registration.Set(&rendererA, nullptr);
			registration.Set(&rendererA, &storeA);
			for (uint64_t s = 6; s <= 8; ++s)
				module.SubmitMeasurement(CutFrame(40.0), s);
			declared = demand.Declarations();
			Assert::AreEqual<size_t>(2, declared.size(), L"declared again once after the re-registration");
			Assert::IsTrue(declared[1].host == &rendererA);

			// Another renderer.
			registration.Set(&rendererB, &storeB);
			for (uint64_t s = 9; s <= 11; ++s)
				module.SubmitMeasurement(CutFrame(40.0), s);
			declared = demand.Declarations();
			Assert::AreEqual<size_t>(3, declared.size(), L"declared once on the new renderer");
			Assert::IsTrue(declared[2].host == &rendererB);
			Assert::IsTrue(demand.Withdrawals().empty(), L"nothing withdrawn while running");

			module.Stop();
			const std::vector<std::pair<const void*, const void*>> withdrawn = demand.Withdrawals();
			Assert::AreEqual<size_t>(1, withdrawn.size(), L"withdrawn on stop");
			Assert::IsTrue(withdrawn[0].first == &rendererB, L"from the renderer it was last declared on");
			Assert::IsTrue(withdrawn[0].second == static_cast<const void*>(&module));
		}

		// GF-P3 MUST-10: a module that was never started declares nothing, even when measurements reach it, and so leaves nothing to
		// withdraw when it is destroyed.
		TEST_METHOD(ANeverStartedModuleDeclaresNothingAndLeavesNoDemand)
		{
			Assert::IsTrue(VpFrameAnalysis::SourceFrameTap::StoreOwners().empty(),
				L"precondition: no store registered by an earlier test");
			RecordingDemand demand;
			DemandHubScope hub(&demand);
			int rendererA = 0;
			FrameAnalysis::Store storeA(FrameAnalysis::RingDepth(4, 4));
			StoreRegistration registration;
			registration.Set(&rendererA, &storeA);

			auto projector = std::make_shared<FakeProjector>();
			projector->openFails = true;   // a dry run: no port
			{
				LedDriveModule module(ProjectorConfig(), CutCompensate(),
					[projector] { return std::make_unique<FakePort>(projector); });
				module.SetSceneReaderForTesting(NoSceneCuts());
				for (uint64_t s = 1; s <= 3; ++s)
					module.SubmitMeasurement(CutFrame(40.0), s);
			}
			Assert::AreEqual<size_t>(0, demand.Declarations().size(), L"a module that never started declares nothing");
			Assert::AreEqual<size_t>(0, demand.Withdrawals().size(), L"nothing declared, so nothing to withdraw");
		}

		// GF-P3 MUST-10: a measurement submitted after Stop (the drive frame holds the module by shared_ptr) declares nothing again,
		// and destroying the stopped module withdraws nothing more.
		TEST_METHOD(AMeasurementAfterStopDeclaresNothing)
		{
			Assert::IsTrue(VpFrameAnalysis::SourceFrameTap::StoreOwners().empty(),
				L"precondition: no store registered by an earlier test");
			RecordingDemand demand;
			DemandHubScope hub(&demand);
			int rendererA = 0;
			FrameAnalysis::Store storeA(FrameAnalysis::RingDepth(4, 4));
			StoreRegistration registration;
			registration.Set(&rendererA, &storeA);

			auto projector = std::make_shared<FakeProjector>();
			projector->openFails = true;   // a dry run: no port
			{
				LedDriveModule module(ProjectorConfig(), CutCompensate(),
					[projector] { return std::make_unique<FakePort>(projector); });
				module.SetSceneReaderForTesting(NoSceneCuts());
				module.Start();
				Assert::AreEqual<size_t>(1, demand.Declarations().size(), L"declared with the module");
				module.Stop();
				Assert::AreEqual<size_t>(1, demand.Withdrawals().size(), L"withdrawn on stop");
				for (uint64_t s = 10; s <= 12; ++s)
					module.SubmitMeasurement(CutFrame(40.0), s);
				Assert::AreEqual<size_t>(1, demand.Declarations().size(), L"a late measurement declares nothing");
				Assert::AreEqual<size_t>(1, demand.Withdrawals().size(), L"and withdraws nothing");
			}
			Assert::AreEqual<size_t>(1, demand.Withdrawals().size(), L"destroying the stopped module withdraws nothing more");
		}
	};
}
