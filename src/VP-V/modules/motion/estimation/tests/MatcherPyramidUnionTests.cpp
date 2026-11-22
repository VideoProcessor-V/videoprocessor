// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// GF-P1 MUST-1 (union proof): the matcher pyramid (kP1HalfLuma, Gpu, MatcherPyramidKey(L)) is served THROUGH THE SKELETON MODULE to
// the REAL matcher. The hub holds the production SkeletonModule (its memo, its seal at Begin, its window derived at Begin, its
// owner GPU device attached at GpuCreated, its ledger grant) and the production P1 module (RegisterP1GpuModule), which registers the
// (kP1HalfLuma, Gpu) owner fill itself from the matcher's offers (VectorEngine::OfferMatcherPlanes -> NoteP1GpuSourceLuma, generation
// -1, as in production), so the gating of the declared structural ahead (1a72644bb) is in the path. The real matcher
// (Dejudder::MotionWarp, the engine's pyramid seam) then runs 10 render frames N:
//   - the render estimate (A = N, B = N + 1; levels 1 / 3 / 5): every estimate after the first pair reads the product
//     (PyramidFromProduct) and its fallback reason is EMPTY, which is exactly what releases the row product.motion_warp
//     (VectorEngineEstimate.inl: pyramidWhy.empty() -> FallbackCatalog::Released("product.motion_warp"));
//   - the EstimateAhead pair (B = N + 1, C = N + 2, both offered): every estimate reads the product with an empty reason;
//   - then the pyramid levels themselves: B levels 1..6 (the outer matcher's 1 / 3 / 5 and the lowRes inner's 2 / 4 / 6), C 1 / 3 / 5
//     and A 1 / 3 / 5 are Acquired from the memo: served, 0 refusals for budget / grant / dispatcher / window, and no no-dispatcher
//     line on the skeleton's Fallbacks status.
// Two legs: the LED raise off (no lane: the memo only) and on (the LED drive's raising demand turns the analysis lane on without a
// lane GPU dispatcher; the memo's owner device must still serve the non-view GPU fill).
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
#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/core/hub/ExtensionHub.h>

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

		constexpr int kSrcW = 512;   // the GPU source luma the matcher reads (as MatcherPyramidGpuIdentityTests' normal arm)
		constexpr int kSrcH = 256;
		constexpr uint32_t kLaneW = 64;   // the lane's own CPU source frames (the LED look-ahead reads P4 of these)
		constexpr uint32_t kLaneH = 48;
		constexpr int kFrames = 10;

		struct FakeRenderer
		{
			VpResync::Shadow resync;
			VpResync::Drive resyncDrive;
			Vpv::Resync::HostBinding binding{ this, resync, resyncDrive };
		};

		// One renderer driven through the resync extension (the seal) and the production skeleton module, which the HUB owns (the
		// engine and the P1 module find the skeleton through the hub's query slot, as in production).
		struct ModuleRig
		{
			static Vpv::Skeleton::SkeletonModule& Install(ModuleRig* self)
			{
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				std::unique_ptr<Vpv::Skeleton::SkeletonModule> module(new Vpv::Skeleton::SkeletonModule(
					[](ConfigFile&) { return false; }, [self]() { return self->base.load() + self->calls.fetch_add(1); }));
				Vpv::Skeleton::SkeletonModule& ref = *module;
				Vpv::Hub::ExtensionHub::Instance().AddModule(std::move(module));
				Vpv::FrameAnalysis::RegisterP1GpuModule();   // after the skeleton, as HubBootstrap does
				return ref;
			}

			ModuleRig() : sk(Install(this)), store(size_t(64) * kLaneW * kLaneH)
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
				Vpv::Hub::ForgetCurrentFrame(Host());
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();   // destroys the skeleton module `sk` refers to
			}
			ModuleRig(const ModuleRig&) = delete;
			ModuleRig& operator=(const ModuleRig&) = delete;

			std::atomic<uint64_t> base{ 1000 };
			std::atomic<uint64_t> calls{ 0 };
			Vpv::Skeleton::SkeletonModule& sk;
			Vpv::Resync::ResyncExtension resync;
			FakeRenderer r;
			std::vector<uint16_t> store;

			const void* Host() const { return &r; }

			void Life(RE::Lifecycle kind, pl_gpu gpu = nullptr, int code = 0, double value = 0.0, ID3D11Device* device = nullptr)
			{
				RE::LifecycleFacts f;
				f.host = &r;
				f.kind = kind;
				f.gpu = gpu;
				f.device = device;   // the renderer's D3D11 device, as GpuCreated carries it in production (the card's video memory query)
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

			// The source frames (real textures): frame i has sequence i + 1; render frame N reads A = frames[N - 1], B = frames[N] and
			// the ahead pair's C = frames[N + 1].
			std::vector<std::unique_ptr<DejudderGpuPlanes>> frames;
			for (int i = 0; i <= kFrames + 1; ++i)
			{
				const int dx = 12 * i, dy = 6 * i;
				const DejudderFrame f = MakeDejudderFrame(kSrcW, kSrcH, [dx, dy](int x, int y) { return DejudderTexture(x - dx, y - dy); },
					[dx, dy](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
				frames.emplace_back(new DejudderGpuPlanes());
				UploadDejudderFrame(gpu, f, *frames.back());
				frames.back()->planes.sequence = uint64_t(i + 1);
				frames.back()->planes.fullRange = false;   // YUV limited
			}

			ModuleRig rig;
			Vpv::Skeleton::IExternalDemand* demand = rig.Demand();
			// The bootstrap's module-wide P13 source-plane owner. The P1 owner fill is NOT registered here: the production P1 module
			// registers it from the matcher's offers (declared structural ahead while the matcher is active).
			Assert::IsTrue(demand->RegisterOwnerFill(VpSkeleton::kP13SourceRing, AcquireForm::Gpu, Vpv::Capture::SourcePlanes::kUnit,
				Vpv::Capture::SourcePlanes::MakeOwnerFill()) == VpSkeleton::SkeletonMemo::Registration::Registered, (leg + L"P13 owner").c_str());

			// The renderer's lifecycle: the pathway (VP-V), the device (its pl_gpu), the motion look-ahead walk of 2 frames (the queue
			// ahead the window needs: L + W), and the memo made by the first MemoFor.
			rig.Life(RE::Lifecycle::SettingsApplied, nullptr, 0, 2.0);
			rig.Life(RE::Lifecycle::GpuCreated, gpu, 0, 0.0, fixture.Device());
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

			// The real matcher, as MatcherPyramidGpuIdentityTests' RunArm builds it: the engine's pyramid seam, a host-unreadable matcher
			// (the product path), a global prior. A second MotionWarp makes the EstimateAhead pair (B, C).
			VpMotion::VectorEngine engine(rig.Host());
			Dejudder::Settings settings = DejudderTestSettings();
			settings.hostReadable = false;
			settings.globalPrior = true;
			settings.lowRes = false;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Dejudder::MotionWarp aheadWarp(fixture.Log(), gpu, settings);
			const VpMotion::MatcherPyramid::Source seam = engine.MatcherPyramidSource();
			warp.SetPyramidSource(seam);
			aheadWarp.SetPyramidSource(seam);

			// Warm-up (R1 re-review): the production P1 module registers the fill at the first matcher offer, which would come AFTER
			// Begin(1) has derived the window, so frame 1 would be refused. The matcher's offer of the source size is told first, exactly as
			// OfferMatcherPlanes tells it, so the fill (with its declared structural ahead) is in the memo before the first Begin.
			Vpv::FrameAnalysis::NoteP1GpuSourceLuma(rig.Host(), uint32_t(kSrcW), uint32_t(kSrcH));
			uint64_t served = 0;
			for (int n = 1; n <= kFrames; ++n)
			{
				rig.Begin(n);
				Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
				const Dejudder::Planes& a = frames[size_t(n - 1)]->planes;
				const Dejudder::Planes& b = frames[size_t(n)]->planes;
				const Dejudder::Planes& c = frames[size_t(n + 1)]->planes;
				const std::wstring at = leg + L"frame " + std::to_wstring(n) + L": ";
				if (n >= 2)
				{
					const VpSkeleton::QueueWindow w = rig.sk.MemoFor(rig.Host(), uint64_t(n)).memo->Window();
					Assert::IsTrue(w.wired != 0 && w.ahead >= 2 && w.aheadCompute >= 2,
						(at + L"the window is active with the queue ahead the matcher declared (SetWindow at Begin)").c_str());
				}
				// RunDejudderEstimate's order: the epoch, the offers (B only: the render pair), the seam, the estimate; the render
				// frame's own Entrance image is offered only AFTER the estimate.
				engine.NoteMatcherEpoch(1);
				Assert::IsTrue(engine.OfferMatcherPlanes(b.sequence, b.luma, b.chroma, b.fullRange), (at + L"B offered").c_str());
				Assert::IsTrue(warp.Estimate(a, b), (at + L"render estimate").c_str());
				{
					pl_frame image{};
					image.num_planes = 2;
					image.planes[0].texture = a.luma;
					image.planes[0].components = 1;
					image.planes[1].texture = a.chroma;
					image.planes[1].components = 2;
					image.repr.sys = PL_COLOR_SYSTEM_BT_709;
					image.repr.levels = PL_COLOR_LEVELS_LIMITED;
					Assert::IsTrue(Vpv::Capture::SourcePlanes::OfferFrame(rig.Host(), a.sequence, int64_t(n), image), (at + L"Entrance offer after the estimate").c_str());
				}
				if (n >= 2)
				{
					Assert::IsTrue(warp.PyramidFromProduct(), (at + L"the render estimate read the product; fallback: " + Wide(warp.PyramidFallbackWhy())).c_str());
					Assert::IsTrue(warp.PyramidFallbackWhy().empty(), (at + L"0 product.motion_warp: the fallback reason is empty, so the row is released").c_str());
				}
				// The EstimateAhead pair: A' = B and B' = C both offered.
				Assert::IsTrue(engine.OfferMatcherPlanes(b.sequence, b.luma, b.chroma, b.fullRange), (at + L"ahead pair: B offered").c_str());
				Assert::IsTrue(engine.OfferMatcherPlanes(c.sequence, c.luma, c.chroma, c.fullRange), (at + L"ahead pair: C offered").c_str());
				Assert::IsTrue(aheadWarp.Estimate(b, c), (at + L"ahead estimate").c_str());
				Assert::IsTrue(aheadWarp.PyramidFromProduct(), (at + L"the ahead estimate read the product (B and C served); fallback: " + Wide(aheadWarp.PyramidFallbackWhy())).c_str());
				Assert::IsTrue(aheadWarp.PyramidFallbackWhy().empty(), (at + L"0 product.motion_warp on the ahead estimate").c_str());

				// The levels themselves, asked of the memo as the matcher's seam asks (lowRes inner's 2 / 4 / 6 included).
				VpSkeleton::SkeletonMemo& memo = *rig.sk.MemoFor(rig.Host(), uint64_t(n)).memo;
				struct Read { uint64_t seq; uint8_t level; const wchar_t* who; };
				std::vector<Read> reads;
				for (uint8_t lv = 1; lv <= 6; ++lv)
					reads.push_back(Read{ b.sequence, lv, L"B (render pair, outer 1/3/5 + lowRes inner 2/4/6)" });
				for (uint8_t lv : { uint8_t(1), uint8_t(3), uint8_t(5) })
					reads.push_back(Read{ c.sequence, lv, L"C (EstimateAhead pair)" });
				if (n >= 2)
					for (uint8_t lv : { uint8_t(1), uint8_t(3), uint8_t(5) })
						reads.push_back(Read{ a.sequence, lv, L"A (the kept product of the frame before)" });
				for (const Read& rd : reads)
				{
					const VpSkeleton::AcquireResult r = memo.Acquire(VpSkeleton::kP1HalfLuma, rd.seq, AcquireForm::Gpu, P1Gpu::MatcherPyramidKey(rd.level));
					Assert::IsTrue(r.Ok() && r.gpu.Valid(), (at + rd.who + L", level " + std::to_wstring(int(rd.level)) + L": served; cause " +
						Wide(memo.LastCause(VpSkeleton::kP1HalfLuma))).c_str());
					++served;
				}
			}
			const VpSkeleton::AcquireCounters cnt = rig.sk.MemoFor(rig.Host(), uint64_t(kFrames)).memo->CountersOf(VpSkeleton::kP1HalfLuma);
			Assert::IsTrue(served > 0, (leg + L"non-vacuous: pyramid levels were read").c_str());
			Assert::IsTrue(cnt.gpuAllocMade > 0, (leg + L"the levels were made through the owner GPU context").c_str());
			Assert::AreEqual(uint64_t(0), cnt.unavailable[size_t(VpSkeleton::AcquireCause::OverBudget)] +
				cnt.unavailable[size_t(VpSkeleton::AcquireCause::NoLedgerGrant)] + cnt.unavailable[size_t(VpSkeleton::AcquireCause::NoGpuDispatcher)] +
				cnt.unavailable[size_t(VpSkeleton::AcquireCause::NotPlannedAhead)] + cnt.unavailable[size_t(VpSkeleton::AcquireCause::NotQueued)],
				(leg + L"no budget, grant, dispatcher or window refusal on the pyramid").c_str());
			const std::vector<std::string> fb = rig.Fallbacks();
			Assert::IsFalse(AnyLineHas(fb, "no GPU dispatcher"), (leg + L"no GPU-dispatcher refusal on the skeleton's Fallbacks status").c_str());
			Assert::IsFalse(AnyLineHas(fb, "product.motion_warp"), (leg + L"no product.motion_warp on the Fallbacks status").c_str());
		}
	}

	TEST_CLASS(MatcherPyramidUnionTests)
	{
	public:
		// 10 frames, the LED raise off and on: the real matcher reads B and C through the skeleton, 0 product.motion_warp.
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
