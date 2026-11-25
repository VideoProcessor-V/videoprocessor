// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// O#1 (L0 One Scene Reader): contract tests for Vpv::Skeleton::IExternalDemand::SceneFactsOf,
// IExternalDemand::SceneOf, and VpSkeleton::SceneRecordReader::AcquireScene agreeing through
// the ONE IExternalDemand* facade across Store-ON, Store-OFF (live), cut, fade, fadeKeep,
// and negative controls (unknown renderer, no analysis lane, seq < 2, default stub, store-off peek).
// Pure CPU; built without PCH.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SceneRecordReader.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SceneReaderContractTestsNs
{
	namespace RE = RendererExtension;
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using Vpv::Resync::ResyncExtension;
	using Vpv::Skeleton::IExternalDemand;
	using Vpv::Skeleton::SceneAnswer;
	using Vpv::Skeleton::SceneFacts;
	using Vpv::Skeleton::SkeletonModule;
	using VpSkeleton::SourceFrameView;

	constexpr uint32_t kW = 64;
	constexpr uint32_t kH = 48;

	struct TempCfg
	{
		std::string path;
		explicit TempCfg(const std::string& text)
		{
			static std::atomic<int> counter{ 0 };
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			path = std::string(dir) + "vp-scene-reader-contract-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + ".cfg";
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

	std::string LaneCfg(const char* mode)
	{
		return std::string("[vpv.skeleton]\nanalysis_lane=") + mode + "\n";
	}

	struct FakeRenderer
	{
		VpResync::Shadow resync;
		VpResync::Drive resyncDrive;
		Vpv::Resync::HostBinding binding{ this, resync, resyncDrive };
	};

	struct SceneRig
	{
		explicit SceneRig(const std::string& cfgText, size_t frames = 64)
			: cfg(cfgText)
			, sk(cfg.Loader(), [this]() { return base.load() + calls.fetch_add(1); })
			, store(frames * kW * kH)
			, count(frames)
		{
			uint32_t x = 777u;
			for (size_t i = 0; i < store.size(); ++i)
			{
				x = x * 1664525u + 1013904223u;
				store[i] = uint16_t((x >> 12) & 1023u);
			}
		}

		~SceneRig()
		{
			Life(RE::Lifecycle::GpuReleased);
		}

		TempCfg cfg;
		std::atomic<uint64_t> base{ 1000 };
		std::atomic<uint64_t> calls{ 0 };
		SkeletonModule sk;
		ResyncExtension resync;
		FakeRenderer r;
		std::vector<uint16_t> store;
		size_t count;

		const void* Host() const { return &r; }

		void Life(RE::Lifecycle kind, int code = 0, double value = 0.0)
		{
			RE::LifecycleFacts f;
			f.host = &r;
			f.kind = kind;
			f.code = code;
			f.value = value;
			sk.OnLifecycle(f);
		}

		void ResyncApply(const char* mode)
		{
			RE::LifecycleFacts f;
			f.host = &r;
			f.kind = RE::Lifecycle::SettingsApplied;
			f.subject = mode;
			resync.OnLifecycle(f);
		}

		void Ingress(int64_t seq)
		{
			RE::IngressContext c;
			c.host = &r;
			c.seq = seq;
			c.queueGeneration = 1;
			c.stateKey = 0x1000;
			c.formatKey = 7;
			c.frameCounter = seq;
			c.timestamp = seq * 1000;
			c.ticksPerSecond = 24000;
			c.measuredRateHz = 24.0;
			resync.OnIngress(c);
		}

		SourceFrameView View(uint64_t seq) const
		{
			SourceFrameView v;
			v.seq = seq;
			v.luma = store.data() + size_t(seq % count) * kW * kH;
			v.width = kW;
			v.height = kH;
			v.strideSamples = kW;
			v.msbShift = 6;
			return v;
		}

		bool Deliver(uint64_t seq) { return Tap::Deliver(&r, View(seq)); }

		void Begin(int64_t seq)
		{
			base += 40000;
			RE::FrameContext c;
			c.host = &r;
			c.phase = RE::FramePhase::Begin;
			c.seq = seq;
			resync.OnFrame(c);
			Assert::IsFalse(sk.OnFrame(c));
		}

		void After(int64_t seq)
		{
			RE::FrameContext c;
			c.host = &r;
			c.phase = RE::FramePhase::AfterRender;
			c.seq = seq;
			Assert::IsFalse(sk.OnFrame(c));
		}
	};

	// Non-owning hub forwarder so Vpv::Skeleton::Demand() resolves to our local SkeletonModule.
	class ForwardingDemandModule : public Vpv::Hub::ISkeletonModule
	{
	public:
		explicit ForwardingDemandModule(SkeletonModule* sk) : sk_(sk) {}
		const char* Name() const override { return "scene_reader_test_forwarder"; }
		void* Query(const char* iid) override { return sk_ ? sk_->Query(iid) : nullptr; }

	private:
		SkeletonModule* sk_ = nullptr;
	};

	struct DemandHubScope
	{
		explicit DemandHubScope(SkeletonModule* sk)
		{
			Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			if (sk)
				Vpv::Hub::ExtensionHub::Instance().AddModule(
					std::unique_ptr<Vpv::Hub::ISkeletonModule>(new ForwardingDemandModule(sk)));
		}
		~DemandHubScope()
		{
			Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
		}
	};

	struct StoreTapGuard
	{
		const void* host = nullptr;
		StoreTapGuard(const void* h, FrameAnalysis::Store* s) : host(h)
		{
			Tap::SetStore(host, s);
		}
		~StoreTapGuard()
		{
			if (host)
				Tap::SetStore(host, nullptr);
		}
	};

	// Minimal IExternalDemand stub leaving SceneOf and SceneFactsOf at their base defaults.
	class DefaultStubDemand : public IExternalDemand
	{
	public:
		void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
			const std::vector<VpSkeleton::ProductId>&) override {}
		void ClearExternalDemand(const void*, const void*) override {}
	};

	FrameAnalysis::Identity IdOf(uint64_t seq)
	{
		FrameAnalysis::Identity id;
		id.seq = seq;
		return id;
	}

	// Synthetic two-still-shots sequence with a hard cut at frame kCutFrame (12).
	constexpr uint64_t kCutFrame = 12;
	void ShapeTwoStillShots(SceneRig& rig)
	{
		for (size_t s = 0; s < rig.count; ++s)
			for (uint32_t y = 0; y < kH; ++y)
				for (uint32_t x = 0; x < kW; ++x)
				{
					const bool second = s >= kCutFrame;
					const uint32_t v = second
						? 900u - (y * 700u) / kH + ((x * 3u + y) & 7u)
						: 150u + (x * 700u) / kW + ((x + y) & 7u);
					rig.store[s * kW * kH + size_t(y) * kW + x] = uint16_t(v);
				}
	}

	// Synthetic 50-frame slow fade starting after frame 4 (matching SkeletonModuleTests).
	void ShapeSlowFade(SceneRig& rig)
	{
		for (int s = 0; s < int(rig.count); ++s)
		{
			const double g = s <= 4 ? 1.0 : (std::max)(0.25, 1.0 - 0.75 * (s - 4) / 50.0);
			for (uint32_t y = 0; y < kH; ++y)
				for (uint32_t x = 0; x < kW; ++x)
				{
					const uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (uint32_t(s) * 83492791u);
					const double v = 64.0 + g * (double(128 + 12 * x) - 64.0) + double(int(h % 17u) - 8);
					rig.store[size_t(s) * kW * kH + y * kW + x] =
						uint16_t((std::min)(1023.0, (std::max)(0.0, std::floor(v + 0.5))));
				}
		}
	}

	TEST_CLASS(SceneReaderContractTests)
	{
	public:
		TEST_METHOD(CutParityAcrossExternalDemandSceneOfSceneFactsOfAndAcquireSceneStoreOnAndOff)
		{
			// ---- 1. Store ON ----
			SceneAnswer storeA12, storeA13, storeA6;
			SceneFacts storeF12, storeF6;
			VpSkeleton::SceneRecordReader::SceneVerdict storeV13, storeV6;
			{
				SceneRig rig(LaneCfg("on"));
				ShapeTwoStillShots(rig);
				FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
				StoreTapGuard tapGuard(rig.Host(), &store);
				DemandHubScope hub(&rig.sk);

				IExternalDemand* demand = Vpv::Skeleton::Demand();
				Assert::IsNotNull(demand, L"Demand() must return the skeleton's IExternalDemand*");

				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				for (int64_t s = 1; s <= int64_t(kCutFrame + 1); ++s)
				{
					rig.Ingress(s);
					if (s == 1)
						rig.Deliver(1);
					rig.Deliver(uint64_t(s) + 1);
					store.Begin(IdOf(uint64_t(s)), FrameAnalysis::kScene);
					rig.Begin(s);
					if (s == 6)
					{
						storeA6 = demand->SceneOf(rig.Host(), 6);
						Assert::IsTrue(demand->SceneFactsOf(rig.Host(), 6, storeF6));
						const char* why = nullptr;
						Assert::IsTrue(VpSkeleton::SceneRecordReader::AcquireScene(6, storeV6, &why));
					}
					if (s == int64_t(kCutFrame + 1))
					{
						storeA12 = demand->SceneOf(rig.Host(), kCutFrame);
						storeA13 = demand->SceneOf(rig.Host(), kCutFrame + 1);
						Assert::IsTrue(demand->SceneFactsOf(rig.Host(), kCutFrame, storeF12));
						const char* why = nullptr;
						Assert::IsTrue(VpSkeleton::SceneRecordReader::AcquireScene(kCutFrame + 1, storeV13, &why));
					}
					rig.After(s);
				}

				Assert::IsTrue(storeA12.known && storeA12.from == SceneAnswer::From::Store);
				Assert::IsTrue(storeF12.valid && storeF12.cut && storeF12.confirmed,
					L"IExternalDemand::SceneFactsOf must report confirmed cut at frame 12");
				Assert::AreEqual(storeF12.cut, storeA12.cutHere);
				Assert::AreEqual(storeF12.confirmed, storeA12.confirmed);
				Assert::AreEqual(storeF12.fade, storeA12.fade);
				Assert::AreEqual(double(storeF12.fadeKeep), double(storeA12.fadeKeep), 1e-6);

				Assert::IsTrue(storeA13.known && storeA13.from == SceneAnswer::From::Store);
				Assert::IsTrue(storeA13.cut && storeA13.cutLate,
					L"confirmed cut is delivered on frame 13");
				Assert::AreEqual(storeA13.cut, storeV13.cut);
				Assert::AreEqual(storeA13.fade, storeV13.fade);
				Assert::AreEqual(double(storeA13.fadeKeep), double(storeV13.fadeKeep), 1e-6);

				// Negative control (still frame inside shot 1): cut must be false across all three readers.
				Assert::IsTrue(storeA6.known && !storeA6.cut && !storeA6.cutHere);
				Assert::IsTrue(storeF6.valid && !storeF6.cut);
				Assert::IsFalse(storeV6.cut);
			}

			// ---- 2. Store OFF (Live path through the exact same IExternalDemand* facade) ----
			{
				SceneRig rig(LaneCfg("on"));
				ShapeTwoStillShots(rig);
				FrameAnalysis::Store disabledStore(FrameAnalysis::RingDepth(4, 4));
				disabledStore.SetEnabled(false);
				StoreTapGuard tapGuard(rig.Host(), &disabledStore);
				DemandHubScope hub(&rig.sk);

				IExternalDemand* demand = Vpv::Skeleton::Demand();
				Assert::IsNotNull(demand);

				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				SceneAnswer liveA12, liveA13;
				SceneFacts liveF12;
				VpSkeleton::SceneRecordReader::SceneVerdict liveV13;
				for (int64_t s = 1; s <= int64_t(kCutFrame + 1); ++s)
				{
					rig.Ingress(s);
					if (s == 1)
						rig.Deliver(1);
					rig.Deliver(uint64_t(s) + 1);
					rig.Begin(s);
					if (s == int64_t(kCutFrame))
						(void)demand->SceneOf(rig.Host(), kCutFrame);   // prime render-order fill as store-on seal did
					if (s == int64_t(kCutFrame + 1))
					{
						// In-test negative control: peek-only store reader fails when store is disabled.
						VpSkeleton::SceneRecordReader::SceneVerdict peeked;
						Assert::IsFalse(VpSkeleton::SceneRecordReader::SceneOf(kCutFrame + 1, peeked),
							L"negative control: store-off peek SceneRecordReader::SceneOf must return false");

						liveA13 = demand->SceneOf(rig.Host(), kCutFrame + 1);
						liveA12 = demand->SceneOf(rig.Host(), kCutFrame);
						Assert::IsTrue(demand->SceneFactsOf(rig.Host(), kCutFrame, liveF12));
						const char* why = nullptr;
						Assert::IsTrue(VpSkeleton::SceneRecordReader::AcquireScene(kCutFrame + 1, liveV13, &why),
							L"AcquireScene must succeed via live IExternalDemand::SceneOf when store is off");
					}
					rig.After(s);
				}

				Assert::IsTrue(liveA13.known && liveA13.from == SceneAnswer::From::Live);
				Assert::IsTrue(liveA12.known && liveA12.from == SceneAnswer::From::Live);
				Assert::AreEqual(storeA13.cut, liveA13.cut, L"Store-OFF live SceneOf must agree with Store-ON cut delivery");
				Assert::AreEqual(storeF12.cut, liveF12.cut, L"Store-OFF SceneFactsOf must agree with Store-ON cut verdict");
				Assert::AreEqual(storeA12.cutHere, liveA12.cutHere);
				Assert::AreEqual(storeV13.cut, liveV13.cut, L"AcquireScene must agree across Store-ON and Store-OFF");
				Assert::AreEqual(double(storeV13.fadeKeep), double(liveV13.fadeKeep), 1e-6);
			}
		}

		TEST_METHOD(FadeAndFadeKeepParityBetweenStoreLiveAndSceneFactsOf)
		{
			constexpr int64_t kFrames = 26;
			std::vector<bool> storeFade(size_t(kFrames + 1), false);
			std::vector<float> storeKeep(size_t(kFrames + 1), 1.0f);
			int detectedFadeFrames = 0;

			// ---- Store ON ----
			{
				SceneRig rig(LaneCfg("on"));
				ShapeSlowFade(rig);
				FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
				StoreTapGuard tapGuard(rig.Host(), &store);
				DemandHubScope hub(&rig.sk);
				IExternalDemand* demand = Vpv::Skeleton::Demand();
				Assert::IsNotNull(demand);

				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				for (int64_t s = 1; s <= kFrames; ++s)
				{
					rig.Ingress(s);
					if (s == 1)
						rig.Deliver(1);
					rig.Deliver(uint64_t(s) + 1);
					store.Begin(IdOf(uint64_t(s)), FrameAnalysis::kScene);
					rig.Begin(s);
					if (s >= 2)
					{
						const SceneAnswer a = demand->SceneOf(rig.Host(), uint64_t(s));
						Assert::IsTrue(a.known && a.from == SceneAnswer::From::Store);
						SceneFacts f;
						Assert::IsTrue(demand->SceneFactsOf(rig.Host(), uint64_t(s), f));
						VpSkeleton::SceneRecordReader::SceneVerdict v;
						const char* why = nullptr;
						Assert::IsTrue(VpSkeleton::SceneRecordReader::AcquireScene(uint64_t(s), v, &why));

						Assert::AreEqual(f.fade, a.fade, L"SceneFactsOf and SceneOf must agree on fade");
						Assert::AreEqual(double(f.fadeKeep), double(a.fadeKeep), 1e-6,
							L"SceneFactsOf and SceneOf must agree on fadeKeep");
						Assert::AreEqual(a.fade, v.fade, L"AcquireScene must agree on fade");
						Assert::AreEqual(double(a.fadeKeep), double(v.fadeKeep), 1e-6,
							L"AcquireScene must agree on fadeKeep");

						storeFade[size_t(s)] = a.fade;
						storeKeep[size_t(s)] = a.fadeKeep;
						if (a.fade && a.fadeKeep < 1.0f)
							++detectedFadeFrames;
					}
					rig.After(s);
				}
			}

			// Non-vacuous check: the synthetic slow fade actually fired with fadeKeep < 1, while static frame 3 did not.
			Assert::IsTrue(detectedFadeFrames > 0, L"synthetic slow fade must trigger fade with partial fadeKeep < 1");
			Assert::IsFalse(storeFade[3], L"negative control: static frame 3 before fade must not trigger fade");
			Assert::AreEqual(1.0, double(storeKeep[3]), 1e-6, L"negative control: static frame 3 fadeKeep must be 1.0");

			// ---- Store OFF (Live) parity ----
			{
				SceneRig rig(LaneCfg("on"));
				ShapeSlowFade(rig);
				FrameAnalysis::Store disabledStore(FrameAnalysis::RingDepth(4, 4));
				disabledStore.SetEnabled(false);
				StoreTapGuard tapGuard(rig.Host(), &disabledStore);
				DemandHubScope hub(&rig.sk);
				IExternalDemand* demand = Vpv::Skeleton::Demand();
				Assert::IsNotNull(demand);

				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				for (int64_t s = 1; s <= kFrames; ++s)
				{
					rig.Ingress(s);
					if (s == 1)
						rig.Deliver(1);
					rig.Deliver(uint64_t(s) + 1);
					rig.Begin(s);
					if (s >= 2)
					{
						VpSkeleton::SceneRecordReader::SceneVerdict peeked;
						Assert::IsFalse(VpSkeleton::SceneRecordReader::SceneOf(uint64_t(s), peeked),
							L"negative control: store-off peek returns false");

						const SceneAnswer live = demand->SceneOf(rig.Host(), uint64_t(s));
						Assert::IsTrue(live.known && live.from == SceneAnswer::From::Live);
						SceneFacts f;
						Assert::IsTrue(demand->SceneFactsOf(rig.Host(), uint64_t(s), f));
						VpSkeleton::SceneRecordReader::SceneVerdict v;
						const char* why = nullptr;
						Assert::IsTrue(VpSkeleton::SceneRecordReader::AcquireScene(uint64_t(s), v, &why));

						Assert::AreEqual(static_cast<bool>(storeFade[size_t(s)]), live.fade, L"live SceneOf fade must equal Store-ON fade");
						Assert::AreEqual(static_cast<bool>(storeFade[size_t(s)]), f.fade, L"live SceneFactsOf fade must equal Store-ON fade");
						Assert::AreEqual(static_cast<bool>(storeFade[size_t(s)]), v.fade, L"live AcquireScene fade must equal Store-ON fade");
						Assert::AreEqual(double(storeKeep[size_t(s)]), double(live.fadeKeep), 1e-6);
						Assert::AreEqual(double(storeKeep[size_t(s)]), double(f.fadeKeep), 1e-6);
						Assert::AreEqual(double(storeKeep[size_t(s)]), double(v.fadeKeep), 1e-6);
					}
					rig.After(s);
				}
			}
		}

		TEST_METHOD(NegativeControlsMatchAcrossUnknownRendererNoLaneFirstFrameAndDefaultStub)
		{
			// 1. Default IExternalDemand stub returns false with "no skeleton module".
			{
				DefaultStubDemand stub;
				const int hostTag = 0;
				SceneFacts f;
				f.valid = true;   // prove SceneFactsOf resets output on failure
				Assert::IsFalse(stub.SceneFactsOf(&hostTag, 4, f));
				Assert::IsFalse(f.valid);
				Assert::AreEqual(std::string("no skeleton module"), std::string(f.why));
				const SceneAnswer a = stub.SceneOf(&hostTag, 4);
				Assert::IsFalse(a.known);
			}

			// 2. Real SkeletonModule accessed strictly through IExternalDemand*.
			SceneRig rig(LaneCfg("on"));
			DemandHubScope hub(&rig.sk);
			IExternalDemand* demand = Vpv::Skeleton::Demand();
			Assert::IsNotNull(demand);

			// Negative control: unknown renderer reports exact "unknown renderer" on BOTH SceneOf and SceneFactsOf.
			// (Pre-O#1 SceneFactsOf conflated unknown renderer with "no analysis lane".)
			{
				const int unknownHost = 0;
				const SceneAnswer a = demand->SceneOf(&unknownHost, 4);
				SceneFacts f;
				const bool ok = demand->SceneFactsOf(&unknownHost, 4, f);
				Assert::IsFalse(a.known);
				Assert::IsFalse(ok);
				Assert::IsFalse(f.valid);
				Assert::AreEqual(std::string("unknown renderer"), std::string(a.why));
				Assert::AreEqual(std::string("unknown renderer"), std::string(f.why),
					L"O#1: unknown renderer must report 'unknown renderer' on SceneFactsOf (matching SceneOf)");
			}

			// Negative control: registered host with NO analysis lane/bundle yet reports "no analysis lane" on BOTH.
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			{
				const SceneAnswer a = demand->SceneOf(rig.Host(), 4);
				SceneFacts f;
				const bool ok = demand->SceneFactsOf(rig.Host(), 4, f);
				Assert::IsFalse(a.known);
				Assert::IsFalse(ok);
				Assert::IsFalse(f.valid);
				Assert::AreEqual(std::string("no analysis lane"), std::string(a.why));
				Assert::AreEqual(std::string("no analysis lane"), std::string(f.why));
			}

			// Positive contrast + seq < 2 negative control after lane bundle exists.
			for (int64_t s = 1; s <= 3; ++s)
			{
				rig.Ingress(s);
				if (s == 1)
					rig.Deliver(1);
				rig.Deliver(uint64_t(s) + 1);
				rig.Begin(s);
				rig.After(s);
			}
			{
				// seq == 1 and seq == 0 both report "no previous frame" on SceneOf and SceneFactsOf.
				for (uint64_t s = 0; s < 2; ++s)
				{
					const SceneAnswer a = demand->SceneOf(rig.Host(), s);
					SceneFacts f;
					const bool ok = demand->SceneFactsOf(rig.Host(), s, f);
					Assert::IsFalse(a.known);
					Assert::IsFalse(ok);
					Assert::AreEqual(std::string("no previous frame"), std::string(a.why));
					Assert::AreEqual(std::string("no previous frame"), std::string(f.why));
				}

				// Positive contrast on seq == 3 proves the lane is genuinely live and answering.
				SceneFacts f3;
				Assert::IsTrue(demand->SceneFactsOf(rig.Host(), 3, f3));
				Assert::IsTrue(f3.valid);
				const SceneAnswer a3 = demand->SceneOf(rig.Host(), 3);
				Assert::IsTrue(a3.known);
			}
		}
	};
}
