// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// GF-P1 MUST-1 (union proof): the matcher pyramid (kP1HalfLuma, Gpu, MatcherPyramidKey(L)) is served THROUGH THE SKELETON MODULE,
// not through a hand-built memo. The module is the production SkeletonModule (its memo, its seal at Begin, its window derived at
// Begin, its owner GPU device attached at GpuCreated, its ledger grant), with the production P1 owner fill and the real P13 source
// plane owner registered module-wide as the bootstrap does. Over 10 render frames N the matcher's reads are the render pair B =
// N + 1 (levels 1..6: the outer matcher's 1 / 3 / 5 and the lowRes inner's 2 / 4 / 6) and the EstimateAhead pair C = N + 2 (1 / 3 / 5),
// with A = N the product kept from the frame before. Every one is served (an Ok Acquire), and the matcher's visible fallback
// (row product.motion_warp) has no reason to appear: 0 Unavailable on the P1 key and no Fallbacks line for it. Two legs: the LED raise
// off (no lane: the memo only) and on (the LED drive's raising demand turns the analysis lane on without a lane GPU dispatcher; the
// memo's owner device must still serve the non-view GPU fill).
//
// It goes green ONLY with P2b: the SkeletonModule's attach line (the memo keeps GPU presence while a lane is bound, MUST-3) and the
// ledger grant that covers the declared ahead (MUST-4). FINAL UNION CHECK: this test green, plus the live check that
// product.motion_warp is released with the LED drive on and off. WARP unless VP_TEST_GPU=hardware; category GpuHardware.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/matcher/MatcherPyramidSource.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/lane/P1GpuFill.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/VariantShape.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		namespace RE = RendererExtension;
		namespace P1Gpu = VpFrameAnalysis::P1Gpu;
		namespace Tap = VpFrameAnalysis::SourceFrameTap;
		using VpSkeleton::AcquireForm;

		constexpr int kSrcW = 256;   // the GPU source luma the matcher reads
		constexpr int kSrcH = 144;
		constexpr uint32_t kLaneW = 64;   // the lane's own CPU source frames (the LED look-ahead reads P4 of these)
		constexpr uint32_t kLaneH = 48;
		constexpr int kFrames = 10;

		struct FakeRenderer
		{
			VpResync::Shadow resync;
			VpResync::Drive resyncDrive;
			Vpv::Resync::HostBinding binding{ this, resync, resyncDrive };
		};

		// One renderer driven through the resync extension (the seal) and the production skeleton module.
		struct ModuleRig
		{
			ModuleRig()
				: sk([](ConfigFile&) { return false; }, [this]() { return base.load() + calls.fetch_add(1); })
				, store(size_t(64) * kLaneW * kLaneH)
			{
				uint32_t x = 777u;
				for (size_t i = 0; i < store.size(); ++i)
				{
					x = x * 1664525u + 1013904223u;
					store[i] = uint16_t((x >> 12) & 1023u);
				}
			}
			~ModuleRig()
			{
				Life(RE::Lifecycle::GpuReleased);
				Vpv::Capture::SourcePlanes::Forget(Host());
			}
			ModuleRig(const ModuleRig&) = delete;
			ModuleRig& operator=(const ModuleRig&) = delete;

			std::atomic<uint64_t> base{ 1000 };
			std::atomic<uint64_t> calls{ 0 };
			Vpv::Skeleton::SkeletonModule sk;
			Vpv::Resync::ResyncExtension resync;
			FakeRenderer r;
			std::vector<uint16_t> store;

			const void* Host() const { return &r; }

			void Life(RE::Lifecycle kind, pl_gpu gpu = nullptr, int code = 0, double value = 0.0)
			{
				RE::LifecycleFacts f;
				f.host = &r;
				f.kind = kind;
				f.gpu = gpu;
				f.code = code;
				f.value = value;
				sk.OnLifecycle(f);
			}

			Vpv::Skeleton::IExternalDemand* Demand()
			{
				Vpv::Skeleton::IExternalDemand* d = static_cast<Vpv::Skeleton::IExternalDemand*>(sk.Query(Vpv::Skeleton::kExternalDemandQueryId));
				Assert::IsNotNull(d, L"the skeleton's demand entry");
				return d;
			}

			VpSkeleton::SourceFrameView View(uint64_t seq) const
			{
				VpSkeleton::SourceFrameView v;
				v.seq = seq;
				v.luma = store.data() + size_t(seq % 64u) * kLaneW * kLaneH;
				v.width = kLaneW;
				v.height = kLaneH;
				v.strideSamples = kLaneW;
				v.msbShift = 6;
				return v;
			}

			// The render frame's Begin as the renderer drives it: the source frame ahead arrives, the shared seal, the skeleton's Begin.
			void Begin(int64_t seq)
			{
				RE::IngressContext in;
				in.host = &r;
				in.seq = seq;
				in.queueGeneration = 1;
				in.stateKey = 0x1000;
				in.formatKey = 7;
				in.frameCounter = seq;
				in.timestamp = seq * 1000;
				in.ticksPerSecond = 24000;
				in.measuredRateHz = 24.0;
				resync.OnIngress(in);
				if (seq == 1)
					Tap::Deliver(&r, View(1));
				Tap::Deliver(&r, View(uint64_t(seq) + 1));
				base += 40000;   // 25 fps
				RE::FrameContext c;
				c.host = &r;
				c.phase = RE::FramePhase::Begin;
				c.seq = seq;
				resync.OnFrame(c);
				Assert::IsFalse(sk.OnFrame(c), L"the skeleton never takes a frame over");
			}

			std::vector<std::string> Fallbacks()
			{
				std::vector<std::string> lines;
				RE::Context c;
				c.host = &r;
				sk.OnStatus(c, RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
				return lines;
			}
		};

		bool AnyLineHas(const std::vector<std::string>& lines, const char* part)
		{
			for (const std::string& l : lines)
				if (l.find(part) != std::string::npos)
					return true;
			return false;
		}

		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		// One leg: `ledRaise` = the LED drive's raising demand registered (the lane on without a lane GPU dispatcher).
		void RunLeg(TargetLutGpuFixture& fixture, bool ledRaise)
		{
			const std::wstring leg = ledRaise ? L"LED raise on: " : L"LED raise off: ";
			pl_gpu gpu = fixture.Gpu();
			struct DispatchHolder
			{
				pl_dispatch d;
				~DispatchHolder() { pl_dispatch_destroy(&d); }
			} dispatch{ pl_dispatch_create(gpu->log, gpu) };

			// The source frames (real textures; the P13 offers of the matcher's uploads): frames 1 .. kFrames + 2.
			std::vector<std::unique_ptr<DejudderGpuPlanes>> frames;
			for (int i = 0; i <= kFrames + 2; ++i)
			{
				const int dx = 12 * i, dy = 6 * i;
				const DejudderFrame f = MakeDejudderFrame(kSrcW, kSrcH, [dx, dy](int x, int y) { return DejudderTexture(x - dx, y - dy); },
					[dx, dy](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
				frames.emplace_back(new DejudderGpuPlanes());
				UploadDejudderFrame(gpu, f, *frames.back());
				frames.back()->planes.sequence = uint64_t(i);
				frames.back()->planes.fullRange = false;
			}

			ModuleRig rig;
			// The bootstrap's module-wide registrations: the P13 source-plane owner and the production P1 GPU owner fill (the structural
			// ahead of 2 declared, as the P1 module does while the matcher is active).
			VpSkeleton::Dims luma;
			luma.width = uint32_t(kSrcW);
			luma.height = uint32_t(kSrcH);
			Vpv::Skeleton::IExternalDemand* demand = rig.Demand();
			Assert::IsTrue(demand->RegisterOwnerFill(VpSkeleton::kP13SourceRing, AcquireForm::Gpu, Vpv::Capture::SourcePlanes::kUnit,
				Vpv::Capture::SourcePlanes::MakeOwnerFill()) == VpSkeleton::SkeletonMemo::Registration::Registered, (leg + L"P13 owner").c_str());
			Assert::IsTrue(demand->RegisterOwnerFill(VpSkeleton::kP1HalfLuma, AcquireForm::Gpu, "input/source_analysis/lane",
				P1Gpu::MatcherPyramidOwnerFill(luma)) == VpSkeleton::SkeletonMemo::Registration::Registered, (leg + L"the production P1 GPU owner fill").c_str());

			// The renderer's lifecycle: the pathway (VP-V), the device (its pl_gpu), the motion look-ahead walk of 2 frames (the queue
			// ahead the window needs: L + W), and the memo made by the first MemoFor.
			rig.Life(RE::Lifecycle::SettingsApplied, nullptr, 0, 2.0);
			rig.Life(RE::Lifecycle::GpuCreated, gpu);
			Assert::IsTrue(rig.sk.MemoFor(rig.Host(), 1).memo != nullptr, (leg + L"the renderer's memo").c_str());
			if (ledRaise)
			{
				VpSkeleton::ConsumerDecl led;
				led.id = 0x0600;
				led.name = "LED look-ahead cut";
				VpSkeleton::AddConsume(led, VpSkeleton::kP4SourceStats, 0);
				VpSkeleton::AddConsume(led, VpSkeleton::kP4SourceStats, 1);
				static int owner = 0;
				demand->SetRaisingExternalDemand(rig.Host(), &owner, std::vector<VpSkeleton::ConsumerDecl>(1, led), {}, "LED drive");
			}

			uint64_t served = 0;
			for (int n = 1; n <= kFrames; ++n)
			{
				rig.Begin(n);
				// The render frame's offers: B = N + 1 and C = N + 2 (and A = N as the frame whose levels were made as B).
				Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
				for (int offered : { n + 1, n + 2 })
				{
					const Dejudder::Planes& p = frames[size_t(offered)]->planes;
					pl_frame image{};
					image.num_planes = 2;
					image.planes[0].texture = p.luma;
					image.planes[0].components = 1;
					image.planes[1].texture = p.chroma;
					image.planes[1].components = 2;
					image.repr.sys = PL_COLOR_SYSTEM_BT_709;
					image.repr.levels = PL_COLOR_LEVELS_LIMITED;
					Assert::IsTrue(Vpv::Capture::SourcePlanes::OfferFrame(rig.Host(), uint64_t(offered), int64_t(1), image),
						(leg + L"the matcher's offer of frame " + std::to_wstring(offered)).c_str());
				}
				const Vpv::Skeleton::FrameMemo fm = rig.sk.MemoFor(rig.Host(), uint64_t(n));
				Assert::IsTrue(fm.memo != nullptr, (leg + L"the memo at frame " + std::to_wstring(n)).c_str());
				VpSkeleton::SkeletonMemo& memo = *fm.memo;
				Assert::IsTrue(memo.Window().wired != 0 && memo.Window().ahead >= 2 && memo.Window().aheadCompute >= 2,
					(leg + L"the window is active with the queue ahead the matcher needs (SetWindow at Begin)").c_str());
				struct Read { uint64_t seq; uint8_t level; const wchar_t* who; };
				std::vector<Read> reads;
				for (uint8_t lv = 1; lv <= 6; ++lv)
					reads.push_back(Read{ uint64_t(n) + 1u, lv, L"B (render pair, outer 1/3/5 + lowRes inner 2/4/6)" });
				for (uint8_t lv : { uint8_t(1), uint8_t(3), uint8_t(5) })
					reads.push_back(Read{ uint64_t(n) + 2u, lv, L"C (EstimateAhead pair)" });
				if (n >= 2)
					for (uint8_t lv : { uint8_t(1), uint8_t(3), uint8_t(5) })
						reads.push_back(Read{ uint64_t(n), lv, L"A (the kept product of the frame before)" });
				for (const Read& rd : reads)
				{
					const VpSkeleton::AcquireResult a = memo.Acquire(VpSkeleton::kP1HalfLuma, rd.seq, AcquireForm::Gpu, P1Gpu::MatcherPyramidKey(rd.level));
					const std::wstring at = leg + L"frame " + std::to_wstring(n) + L", " + rd.who + L", level " + std::to_wstring(int(rd.level)) + L": ";
					Assert::IsTrue(a.Ok() && a.gpu.Valid(), (at + L"served; cause " + Wide(memo.LastCause(VpSkeleton::kP1HalfLuma))).c_str());
					++served;
				}
			}
			const Vpv::Skeleton::FrameMemo last = rig.sk.MemoFor(rig.Host(), uint64_t(kFrames));
			Assert::IsTrue(last.memo != nullptr, (leg + L"the memo at the end").c_str());
			const VpSkeleton::AcquireCounters c = last.memo->CountersOf(VpSkeleton::kP1HalfLuma);
			Assert::IsTrue(served > 0, (leg + L"non-vacuous: pyramid levels were read").c_str());
			Assert::AreEqual(uint64_t(0), c.Unavailable(), (leg + L"0 Unavailable on the pyramid: the matcher has no reason to fall back").c_str());
			Assert::IsTrue(c.gpuAllocMade > 0, (leg + L"the levels were made through the owner GPU context").c_str());
			const std::vector<std::string> fb = rig.Fallbacks();
			Assert::IsFalse(AnyLineHas(fb, "product.motion_warp"), (leg + L"0 product.motion_warp").c_str());
			Assert::IsFalse(AnyLineHas(fb, "no GPU dispatcher"), (leg + L"no GPU-dispatcher refusal on the Fallbacks status").c_str());
		}
	}

	TEST_CLASS(MatcherPyramidUnionTests)
	{
	public:
		// 10 frames, the LED raise off and on: B and C served, 0 product.motion_warp.
		BEGIN_TEST_METHOD_ATTRIBUTE(MatcherPyramidIsServedThroughTheSkeleton)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MatcherPyramidIsServedThroughTheSkeleton)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			RunLeg(fixture, false);
			RunLeg(fixture, true);
		}
	};
}
