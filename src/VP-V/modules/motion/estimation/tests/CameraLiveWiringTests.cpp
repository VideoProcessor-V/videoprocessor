// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Camera live wiring, host level (CameraExtension + a fake vector engine behind a fake directory):
//   * camera_link off (or nothing set): nothing is instantiated, no engine call is made, no frame is taken over (output unchanged);
//   * camera_link on + camera_model on: the fit is wanted and the camera graph is built at the first engine field; with a
//     camera_meas_* switch also on, the guidance adapter is attached and the ONE pre-estimate hook is registered on the engine;
//   * the detach order on teardown: the engine's hook is cleared FIRST (while the core and the table still exist), and the engine
//     is left with no hook; a settings change that turns guidance off clears it too;
//   * an absent vector engine is refused visibly (Fallbacks "camera_engine"), nothing attaches.
// CPU only: no GPU (the readback ring and the label producer do not exist), a fake matcher-less engine.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraExtension.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitWorker.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLane.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceAdapter.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CameraLiveWiringTestsNs
{
	namespace RE = RendererExtension;
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using Vpv::Camera::CameraExtension;
	using Vpv::Resync::ResyncExtension;

	bool Has(const std::vector<std::string>& lines, const char* part)
	{
		for (const std::string& l : lines)
			if (l.find(part) != std::string::npos)
				return true;
		return false;
	}

	struct TempCfg
	{
		std::string path;
		explicit TempCfg(const std::string& text)
		{
			static std::atomic<int> counter{ 0 };
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			path = std::string(dir) + "vp-camlive-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + ".cfg";
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out << text;
		}
		~TempCfg() { std::remove(path.c_str()); }
		Vpv::Skeleton::ConfigLoader Loader() const
		{
			const std::string p = path;
			return [p](ConfigFile& c) { return c.Load(p, ConfigFile::ReadPolicy::Fresh); };
		}
	};

	struct FakeRenderer
	{
		VpResync::Shadow resync;
		VpResync::Drive resyncDrive;
		Vpv::Resync::HostBinding binding{ this, resync, resyncDrive };
		FrameAnalysis::Store store{ FrameAnalysis::RingDepth(4, 4) };
	};

	// An engine with verified fields up to `latest` (no textures: nothing reads them without a GPU) that records its hook changes.
	struct FakeEngine : VpMotion::IVectorEngine
	{
		uint64_t latest = 0;
		bool seeded = true;   // F2: the optical-flow seed fed every pair (the camera fit's only input)
		bool skipped = false; // ... with seeded false: the latest pair's searches were skipped (a cut)
		VpMotion::IPreEstimateHook* hook = nullptr;
		std::vector<VpMotion::IPreEstimateHook*> history;
		std::function<void(VpMotion::IPreEstimateHook*)> onSet;

		uint64_t LatestVerifiedFieldSeq() const override { return latest; }
		uint64_t FieldEra() const override { return 1; }
		bool FieldOf(uint64_t seq, VpMotion::VerifiedField& out) const override
		{
			if (seq == 0 || seq > latest)
				return false;
			out = VpMotion::VerifiedField();
			out.seq = seq;
			out.pairFrom = seq - 1;
			out.pairTo = seq;
			out.gridW = 120;
			out.gridH = 68;
			out.blockSize = 16;
			return true;
		}
		float FieldScale() const override { return 1.0f; }
		bool SeedFieldOf(uint64_t seq, VpMotion::SeedField& out) const override
		{
			out = VpMotion::SeedField();
			if (!seeded || seq == 0 || seq > latest)
			{
				if (!seeded && skipped && seq == latest)
					out.absence = VpMotion::SeedAbsence::Skipped;
				return false;
			}
			out.seq = seq;
			out.pairFrom = seq - 1;
			out.pairTo = seq;
			out.gridW = 120;
			out.gridH = 68;
			out.blockSize = 16;
			out.scaleToSourcePx = 2.0f;
			out.sourceGridW = 120;
			out.sourceGridH = 68;
			out.sadTrustMax = 3.0f;
			out.consistencyPx = 1.5f;
			return true;
		}
		bool GlobalFromEngine() const override { return false; }
		bool EngineGlobal(uint64_t, VpMotion::GlobalVec&) const override { return false; }
		bool PairSpansSlip(uint64_t, uint64_t) const override { return false; }
		bool FlowIsCpuResident() const override { return false; }
		bool FlowOf(uint64_t, VpMotion::FlowView&) const override { return false; }
		void SetPreEstimateHook(VpMotion::IPreEstimateHook* h) override
		{
			if (onSet)
				onSet(h);   // before the change: the state the caller leaves around the call is visible to the test
			hook = h;
			history.push_back(h);
		}
	};

	struct FakeDirectory : VpMotion::IVectorEngineDirectory
	{
		VpMotion::IVectorEngine* engine = nullptr;
		const void* host = nullptr;
		VpMotion::IVectorEngine* EngineFor(const void* h) override { return h == host ? engine : nullptr; }
	};

	struct Rig
	{
		explicit Rig(const std::string& text, bool engineThere = true) : cfg(text),
			ext(cfg.Loader(), [this]() -> VpMotion::IVectorEngineDirectory* { return &dir; })
		{
			ext.SetGuidanceLockForTesting(false);   // these tests pin the guidance wiring the next reset unlocks (the lock: GuidanceIsLocked...)
			dir.host = &r;
			dir.engine = engineThere ? &engine : nullptr;
			engine.latest = 8;
			Tap::SetStore(&r, &r.store);
		}
		~Rig()
		{
			Life(RE::Lifecycle::GpuReleased);
			Tap::SetStore(&r, nullptr);
		}

		TempCfg cfg;
		FakeEngine engine;
		FakeDirectory dir;
		CameraExtension ext;
		ResyncExtension resync;
		FakeRenderer r;

		const void* Host() const { return &r; }
		void Life(RE::Lifecycle kind, int code = 0)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = kind; f.code = code;
			ext.OnLifecycle(f);
		}
		void ResyncApply(const char* mode)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = RE::Lifecycle::SettingsApplied; f.subject = mode; f.code = 0; f.value = 0;
			resync.OnLifecycle(f);
		}
		void Frame(int64_t seq)
		{
			RE::IngressContext c;
			c.host = &r; c.seq = seq; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
			c.frameCounter = seq; c.timestamp = seq * 1000; c.ticksPerSecond = 24000; c.measuredRateHz = 24.0;
			resync.OnIngress(c);
			RE::FrameContext f;
			f.host = &r; f.phase = RE::FramePhase::Begin; f.seq = seq;
			resync.OnFrame(f);
			Assert::IsFalse(ext.OnFrame(f), L"the camera extension never takes a frame over");
			f.phase = RE::FramePhase::AfterRender;
			Assert::IsFalse(ext.OnFrame(f));
		}
		void Start(int frames = 4)
		{
			ResyncApply("on");
			Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= frames; ++s)
				Frame(s);
		}
		std::vector<std::string> Lines(RE::StatusKind kind)
		{
			std::vector<std::string> lines;
			RE::Context c; c.host = &r;
			ext.OnStatus(c, kind, lines, nullptr, nullptr);
			return lines;
		}
	};

	const char* const kLinkOn = "[vpv.skeleton]\ncamera_link=report\n";
}

namespace VideoProcessorTest
{
	using namespace CameraLiveWiringTestsNs;

	TEST_CLASS(CameraLiveWiringTests)
	{
	public:
		TEST_METHOD(EverythingOffInstantiatesNothingAndNeverTouchesTheEngine)
		{
			const int threadsBefore = CameraLink::CameraFitWorker::ThreadsEverStarted();
			Rig rig("[general]\nx=1\n");
			rig.Start(6);
			Assert::AreEqual<size_t>(0, rig.ext.HostStates());
			Assert::IsNull(rig.ext.CoreOf(rig.Host()));
			Assert::IsNull(rig.ext.AdapterOf(rig.Host()));
			Assert::IsTrue(rig.engine.history.empty(), L"the engine's hook was never touched");
			Assert::AreEqual(threadsBefore, CameraLink::CameraFitWorker::ThreadsEverStarted());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Fallbacks).empty());
		}

		TEST_METHOD(CameraLinkOffWithTheModelKeysSetInstantiatesNothingAndSaysSo)
		{
			// camera_model and a switch are on, camera_link is not: the keys are shown as inert, nothing exists.
			const int threadsBefore = CameraLink::CameraFitWorker::ThreadsEverStarted();
			Rig rig("[vpv.camera]\ncamera_model=on\ncamera_meas_model_global=on\n");
			rig.Start(6);
			Assert::IsFalse(rig.ext.FitWanted(rig.Host()));
			Assert::IsFalse(rig.ext.LiveBuilt(rig.Host()));
			Assert::IsNull(rig.ext.CoreOf(rig.Host()));
			Assert::IsNull(rig.ext.AdapterOf(rig.Host()), L"no adapter without camera_link");
			Assert::IsTrue(rig.engine.history.empty(), L"no hook was registered");
			Assert::AreEqual(threadsBefore, CameraLink::CameraFitWorker::ThreadsEverStarted(), L"no thread started");
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "camera_model: camera_link is off"), L"shown, never silent");
		}

		TEST_METHOD(ReportOnlyWithoutTheModelKeepsTheFitAndTheEngineOut)
		{
			Rig rig(kLinkOn);
			rig.Start(6);
			Assert::IsTrue(rig.ext.Active(rig.Host()));
			Assert::IsFalse(rig.ext.FitWanted(rig.Host()), L"camera_model is off: no fit");
			Assert::IsFalse(rig.ext.CoreOf(rig.Host())->Allocated());
			Assert::IsNull(rig.ext.AdapterOf(rig.Host()));
			Assert::IsTrue(rig.engine.history.empty());
		}

		TEST_METHOD(LinkOnModelOnAttachesTheFitButNoAdapterWithoutASwitch)
		{
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\n");
			rig.Start(6);
			Assert::IsTrue(rig.ext.FitWanted(rig.Host()));
			Assert::IsTrue(rig.ext.LiveBuilt(rig.Host()), L"the camera graph exists once the engine has a field");
			Assert::IsTrue(rig.ext.LaneOf(rig.Host())->Built());
			Assert::IsTrue(rig.ext.CoreOf(rig.Host())->Allocated(), L"the fit is allocated on that graph (p7Wanted)");
			Assert::IsNull(rig.ext.AdapterOf(rig.Host()), L"all measurement switches off: no adapter");
			Assert::IsTrue(rig.engine.history.empty(), L"... and no hook on the engine");
		}

		// GUIDANCE LOCK (owner, reset 1): a guidance switch set with the camera live leaves guidance off, registers no hook and says so.
		TEST_METHOD(GuidanceIsLockedThisResetASetSwitchRegistersNoHookAndIsShown)
		{
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\ncamera_meas_model_global=on\n");
			rig.ext.SetGuidanceLockForTesting(true);   // the shipped default (the rig unlocks for the wiring tests)
			Assert::IsTrue(rig.ext.GuidanceLocked());
			rig.Start(6);
			Assert::IsTrue(rig.ext.FitWanted(rig.Host()), L"the fit itself still runs");
			Assert::IsNull(rig.ext.AdapterOf(rig.Host()), L"no guidance adapter");
			Assert::IsTrue(rig.engine.hook == nullptr, L"no pre-estimate hook registered");
			bool line = false, entry = false;
			for (const std::string& l : rig.Lines(RE::StatusKind::Lines))
				line = line || l.find("camera guidance waits for the on-time rule") != std::string::npos;
			for (const std::string& l : rig.Lines(RE::StatusKind::Fallbacks))
				entry = entry || l.find("camera_guidance: camera guidance waits for the on-time rule") != std::string::npos;
			Assert::IsTrue(line && entry, L"shown in Ctrl+I / Config status and Fallbacks");
		}

		TEST_METHOD(LinkOnModelOnWithGlobalAttachesTheAdapterAndRegistersTheHook)
		{
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\ncamera_meas_model_global=on\n");
			rig.Start(6);
			Assert::IsTrue(rig.ext.LiveBuilt(rig.Host()));
			CameraLink::CameraGuidanceAdapter* adapter = rig.ext.AdapterOf(rig.Host());
			Assert::IsNotNull(adapter);
			Assert::IsTrue(adapter->Attached(), L"attached to this renderer's engine (directory EngineFor(host))");
			Assert::IsTrue(adapter->Hooked(), L"a switch is on: the hook is registered");
			Assert::IsTrue(rig.engine.hook == static_cast<VpMotion::IPreEstimateHook*>(&adapter->Hook()), L"the ONE hook, on the engine");
			Assert::AreEqual<size_t>(1, rig.engine.history.size(), L"registered once, not per frame");
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), "camera"), L"camera status reaches the Ctrl+I / Config lines");
		}

		TEST_METHOD(TeardownClearsTheHookFirstWhileTheCoreAndTheTableStillExist)
		{
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\ncamera_meas_model_global=on\n");
			rig.Start(6);
			Assert::IsNotNull(rig.engine.hook);
			bool laneAtClear = false, coreAtClear = false, sawClear = false;
			const void* host = rig.Host();
			CameraExtension* ext = &rig.ext;
			rig.engine.onSet = [&](VpMotion::IPreEstimateHook* h) {
				if (h == nullptr)
				{
					sawClear = true;
					laneAtClear = ext->LaneOf(host) && ext->LaneOf(host)->Built();
					coreAtClear = ext->CoreOf(host) != nullptr;
				}
			};
			rig.Life(RE::Lifecycle::GpuReleased);
			Assert::IsTrue(sawClear, L"the hook was cleared on GpuReleased");
			Assert::IsTrue(laneAtClear && coreAtClear, L"cleared FIRST: the table and the core were still there");
			Assert::IsNull(rig.engine.hook, L"the engine is left with no hook");
			Assert::AreEqual<size_t>(0, rig.ext.HostStates());
			Assert::IsNull(rig.ext.AdapterOf(rig.Host()));
		}

		TEST_METHOD(AnEngineThatIsGoneIsNotCalledAtTeardown)
		{
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\ncamera_meas_model_global=on\n");
			rig.Start(6);
			Assert::AreEqual<size_t>(1, rig.engine.history.size());
			rig.dir.engine = nullptr;   // the renderer was torn down: the directory no longer returns the engine
			rig.Life(RE::Lifecycle::GpuReleased);
			Assert::AreEqual<size_t>(1, rig.engine.history.size(), L"no call into an engine that is not known to be valid");
			Assert::AreEqual<size_t>(0, rig.ext.HostStates());
		}

		TEST_METHOD(ASettingsChangeThatTurnsGuidanceOffClearsTheHook)
		{
			// The loader re-reads the file: rewrite it without the switch, apply settings.
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\ncamera_meas_model_global=on\n");
			rig.Start(6);
			Assert::IsNotNull(rig.engine.hook);
			{
				std::ofstream out(rig.cfg.path, std::ios::binary | std::ios::trunc);
				out << std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\n";
			}
			rig.Life(RE::Lifecycle::SettingsApplied);
			Assert::IsNull(rig.engine.hook, L"hook cleared when the last switch went off");
			Assert::IsNull(rig.ext.AdapterOf(rig.Host()), L"the adapter is released");
			for (int64_t s = 7; s <= 9; ++s)
				rig.Frame(s);   // rebuilt at the next frame without guidance
			Assert::IsTrue(rig.ext.LiveBuilt(rig.Host()));
			Assert::IsNull(rig.ext.AdapterOf(rig.Host()));
			Assert::IsNull(rig.engine.hook);
		}

		TEST_METHOD(TheEnginesOutcomeIsMappedOntoTheCameraOutcome)
		{
			// M2's plain-data outcome through the registered hook: narrow idle and the rejection text reach the scheduler's counters.
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\ncamera_meas_model_global=on\n");
			rig.Start(6);
			CameraLink::CameraGuidanceAdapter* adapter = rig.ext.AdapterOf(rig.Host());
			Assert::IsNotNull(adapter);
			VpMotion::EstimateOutcome outcome;
			outcome.guidanceUsed = true;
			outcome.narrowIdle = true;
			outcome.rejection = "camera guidance: texture upload failed";
			rig.engine.hook->AfterEstimate(9, outcome);
			Assert::AreEqual<uint64_t>(1, adapter->Scheduler().Counters().narrowIdleEngine);
			Assert::AreEqual<uint64_t>(1, adapter->Scheduler().Counters().rejectedByMatcher);
			Assert::AreEqual<uint64_t>(1, adapter->Hook().UsedPairs());
		}

		TEST_METHOD(TheCameraOnWithoutResyncEventIsAVisibleFallback)
		{
			// camera_link + camera_model on (explicit), resync_event off: the seal never drives, so the fit can never run.
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\n");
			rig.ResyncApply("off");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 6; ++s)
				rig.Frame(s);
			Assert::IsTrue(rig.ext.FitWanted(rig.Host()));
			Assert::IsFalse(rig.ext.LiveBuilt(rig.Host()), L"nothing attaches without the seal");
			const std::string want = "camera_fit: camera fit waits for resync_event: turn resync_event on";
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), want.c_str()), L"a fallback entry with id camera_fit and the actionable text");
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Lines), "waits for resync_event"), L"not a separate status-line form");
			std::string reply;
			RE::Context c;
			c.host = rig.Host();
			Assert::IsTrue(rig.ext.OnCommand(c, "camera", "status", reply));
			Assert::IsTrue(reply.find(want) != std::string::npos, L"also in the Ctrl+Alt+D text");
			Assert::IsTrue(rig.engine.history.empty());
		}

		TEST_METHOD(AnAbsentVectorEngineIsRefusedVisibly)
		{
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\ncamera_meas_model_global=on\n", false);
			rig.Start(6);
			Assert::IsTrue(rig.ext.FitWanted(rig.Host()));
			Assert::IsFalse(rig.ext.LiveBuilt(rig.Host()), L"nothing attached");
			Assert::IsNull(rig.ext.AdapterOf(rig.Host()));
			Assert::IsFalse(rig.ext.EngineProblemOf(rig.Host()).empty());
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "camera_engine: the vector engine of this renderer is not available"),
				L"refused in the Fallbacks list, not log-only");
			Assert::IsTrue(rig.engine.history.empty());
		}

		TEST_METHOD(AnEngineThatHasMadeNoFieldYetIsWaitedForVisibly)
		{
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\n");
			rig.engine.latest = 0;
			rig.Start(4);
			Assert::IsFalse(rig.ext.LiveBuilt(rig.Host()));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), "waiting for the first vector field"));
			rig.engine.latest = 5;
			for (int64_t s = 5; s <= 8; ++s)
				rig.Frame(s);
			Assert::IsTrue(rig.ext.LiveBuilt(rig.Host()), L"built as soon as the engine has a field");
		}

		// F2: an engine with fields but no optical-flow seed (engine off or matcher-only): no fit, a visible camera_fit entry, never
		// the guided live field instead; the seed's return builds the live parts.
		TEST_METHOD(AnEngineWithoutTheOpticalFlowSeedIsAVisibleCameraFitFallback)
		{
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\n");
			rig.engine.seeded = false;
			rig.Start(4);
			Assert::IsFalse(rig.ext.LiveBuilt(rig.Host()), L"FieldOf answers, but the fit never reads it");
			const std::string want = "camera_fit: camera fit needs the optical-flow seed (engine off or matcher-only)";
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), want.c_str()), L"a fallback entry with id camera_fit");
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Lines), "waiting for the first vector field"), L"named, not a plain wait");
			// M2 condition 2: the Fallbacks kind is what the renderer's GetFallbacksStatus (Config status, Ctrl+I) shows; the camera
			// status command (Ctrl+Alt+D) carries it too.
			{
				std::string reply;
				RE::Context c;
				c.host = rig.Host();
				Assert::IsTrue(rig.ext.OnCommand(c, "camera", "status", reply));
				Assert::IsTrue(reply.find(want) != std::string::npos, L"also in the Ctrl+Alt+D text");
			}
			rig.engine.seeded = true;
			for (int64_t s = 5; s <= 8; ++s)
				rig.Frame(s);
			Assert::IsTrue(rig.ext.LiveBuilt(rig.Host()));
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Fallbacks), "optical-flow seed"), L"released with the seed");
			rig.engine.seeded = false;
			for (int64_t s = 9; s <= 17; ++s)   // the cached status refreshes at frames <= 8, then every 16th (frame 17)
				rig.Frame(s);
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), want.c_str()), L"also visible once live");
		}

		// F2: a pair whose searches were skipped (a cut) has no seed: its own wording, not "engine off or matcher-only".
		TEST_METHOD(ACutPairWithoutASeedSaysSo)
		{
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\n");
			rig.engine.seeded = false;
			rig.engine.skipped = true;
			rig.Start(4);
			Assert::IsFalse(rig.ext.LiveBuilt(rig.Host()));
			const std::vector<std::string> fb = rig.Lines(RE::StatusKind::Fallbacks);
			Assert::IsTrue(Has(fb, "camera_fit: camera fit skipped this pair (cut: no seed)"));
			Assert::IsFalse(Has(fb, "engine off or matcher-only"), L"a cut is not an engine fault");
			std::string reply;
			RE::Context c;
			c.host = rig.Host();
			Assert::IsTrue(rig.ext.OnCommand(c, "camera", "status", reply));
			Assert::IsTrue(reply.find("camera fit skipped this pair (cut: no seed)") != std::string::npos);
		}

		// ---- camera-live step 2: the lag bridge on the lease path -------------------------------------------------------------

		// The lag bridge is a standing state, never log-only: Ctrl+I and Fallbacks say so while it carries the fit, nothing with the camera off.
		TEST_METHOD(TheLagBridgeIsShownWhileItCarriesTheFitAndNothingWhenTheCameraIsOff)
		{
			const std::string text = CameraExtension::LagBridgeText();
			Assert::IsTrue(text.find("private lag bridge") != std::string::npos);
			Assert::IsTrue(text.find("N+6") != std::string::npos, L"the seal lag in the text (kSealLag)");
			Assert::IsTrue(text.find("until SK-W2") != std::string::npos);
			const std::string entry = std::string(CameraExtension::kLagBridgeId) + ": " + text;
			{
				Rig off("[general]\nx=1\n");
				off.Start(6);
				Assert::IsFalse(Has(off.Lines(RE::StatusKind::Lines), "lag bridge"));
				Assert::IsFalse(Has(off.Lines(RE::StatusKind::Fallbacks), CameraExtension::kLagBridgeId), L"camera off: nothing");
			}
			{
				Rig report(kLinkOn);
				report.Start(6);
				Assert::IsFalse(Has(report.Lines(RE::StatusKind::Fallbacks), CameraExtension::kLagBridgeId), L"no fit: no bridge, nothing shown");
			}
			Rig rig(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\n");
			rig.Start(6);
			Assert::IsTrue(rig.ext.LiveBuilt(rig.Host()));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), ("camera: " + text).c_str()), L"the Ctrl+I line");
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), entry.c_str()), L"the Fallbacks entry: id and text");
			// The fit turned off: the bridge is released and the entry goes.
			{
				std::ofstream out(rig.cfg.path, std::ios::binary | std::ios::trunc);
				out << std::string(kLinkOn);
			}
			rig.Life(RE::Lifecycle::SettingsApplied);
			for (int64_t s = 7; s <= 8; ++s)
				rig.Frame(s);
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Fallbacks), CameraExtension::kLagBridgeId));
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Lines), "lag bridge"));
		}

		TEST_METHOD(TheCameraRunsOnTheLagBridgeWithoutTheSkeletonLane)
		{
			// No skeleton module and no analysis lane: until SK-W2 the camera declares nothing to the skeleton and does not depend on its lane;
			// the lag bridge carries the fit exactly as before, through the lease path.
			TempCfg cfg(std::string(kLinkOn) + "[vpv.camera]\ncamera_model=on\n");
			FakeEngine engine;
			engine.latest = 8;
			FakeRenderer r;
			FakeDirectory dir;
			dir.host = &r;
			dir.engine = &engine;
			ResyncExtension resync;
			CameraExtension ext(cfg.Loader(), [&dir]() -> VpMotion::IVectorEngineDirectory* { return &dir; });
			Tap::SetStore(&r, &r.store);
			RE::LifecycleFacts lf;
			lf.host = &r; lf.kind = RE::Lifecycle::SettingsApplied; lf.subject = "on"; lf.code = 0; lf.value = 0;
			resync.OnLifecycle(lf);
			lf.kind = RE::Lifecycle::GpuCreated; lf.subject = nullptr;
			ext.OnLifecycle(lf);
			for (int64_t s = 1; s <= 6; ++s)
			{
				RE::IngressContext c;
				c.host = &r; c.seq = s; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
				c.frameCounter = s; c.timestamp = s * 1000; c.ticksPerSecond = 24000; c.measuredRateHz = 24.0;
				resync.OnIngress(c);
				RE::FrameContext f;
				f.host = &r; f.phase = RE::FramePhase::Begin; f.seq = s;
				resync.OnFrame(f);
				ext.OnFrame(f);
				f.phase = RE::FramePhase::AfterRender;
				ext.OnFrame(f);
			}
			Assert::IsTrue(ext.LiveBuilt(&r), L"the lag bridge carries the fit without the skeleton lane");
			Assert::IsTrue(ext.CoreOf(&r)->Allocated());
			Assert::IsTrue(ext.LaneOf(&r)->Lease(VpSkeleton::kP7CameraFit).Owned(), L"the fit holds an owned lease, not borrowed pointers");
			lf.kind = RE::Lifecycle::GpuReleased;
			ext.OnLifecycle(lf);
			Tap::SetStore(&r, nullptr);
		}
	};
}
