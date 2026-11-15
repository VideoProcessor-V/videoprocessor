// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// L-B4 (BBP-B B4): the matcher's read seam of the shared luma pyramid (VectorEngine::MatcherPyramidSource) and its offers
// (VectorEngine::OfferMatcherPlanes), CPU only, in the REAL order of RunDejudderEstimate: SourcePlanes::BeginFrame ->
// OfferMatcherPlanes(A), (B) -> the seam per level -> the Entrance OfferFrame -> later P13 readers.
// Real parts: the skeleton memo (core/SkeletonAcquire.h), the P13 table and owner fill (SourcePlanes::MakeOwnerFill), the engine's
// seam lambda. Fakes: the skeleton module's external-demand entry (MemoFor = this memo, as SkeletonModule's; empty in the Original
// pathway, its contract) and the (kP1HalfLuma, Gpu) owner fill (no GPU here: it returns a dummy pl_tex and counts its calls; the
// real fill's GPU run is MatcherPyramidGpuIdentityTests / B5). pl_tex_t values are plain structs (as SourcePlaneProviderTests).
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/matcher/MatcherPyramidSource.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/lane/P1GpuFill.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/ExtensionHub.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using VpSkeleton::AcquireForm;
		using VpSkeleton::AcquireResult;
		using VpSkeleton::SkeletonMemo;
		using Level = VpMotion::MatcherPyramid::Level;
		using Request = VpMotion::MatcherPyramid::Request;

		bool PyrHas(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

		// The skeleton module's external-demand entry, reduced to MemoFor (the seam's only use of it).
		struct FakeDemand : Vpv::Skeleton::IExternalDemand
		{
			std::shared_ptr<SkeletonMemo> memo;
			const void* host = nullptr;
			void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
				const std::vector<VpSkeleton::ProductId>&) override {}
			void ClearExternalDemand(const void*, const void*) override {}
			Vpv::Skeleton::FrameMemo MemoFor(const void* h, uint64_t seq) override
			{
				Vpv::Skeleton::FrameMemo fm;
				if (!memo || h != host || Vpv::Hub::ExtensionHub::Instance().OriginalPathway(h))
					return fm;   // the module's contract: no memo for an unknown renderer, never in the Original pathway
				fm.memo = memo;
				fm.host = h;
				fm.seq = seq;
				return fm;
			}
		};

		struct FakeSkeleton : Vpv::Hub::ISkeletonModule
		{
			FakeDemand* demand;
			explicit FakeSkeleton(FakeDemand* d) : demand(d) {}
			const char* Name() const override { return "test.matcher_pyramid.fake_skeleton"; }
			void* Query(const char* id) override
			{
				return id != nullptr && std::string(id) == Vpv::Skeleton::kExternalDemandQueryId
					? static_cast<Vpv::Skeleton::IExternalDemand*>(demand) : nullptr;
			}
		};

		struct Rig
		{
			int hostTag = 0;
			FakeDemand demand;
			std::shared_ptr<SkeletonMemo> memo = std::make_shared<SkeletonMemo>();
			pl_tex_t lumaA{}, chromaA{}, lumaB{}, chromaB{}, other{}, level{};
			int p1Calls = 0;
			std::vector<uint8_t> p1Levels;
			std::vector<uint64_t> p1Seqs;
			bool p1Decline = false;

			const void* Host() const { return &hostTag; }

			explicit Rig(bool withMemo = true)
			{
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				for (pl_tex_t* t : { &lumaA, &lumaB, &other })
				{
					t->params.w = 64;
					t->params.h = 48;
				}
				chromaA.params.w = chromaB.params.w = 32;
				chromaA.params.h = chromaB.params.h = 48;
				level.params.w = 32;
				level.params.h = 24;
				memo->SetHost(Host());
				memo->SetRenderThread(std::this_thread::get_id());
				memo->SetGpuDispatcherPresent(true);
				memo->SetGpuBudget(true, 1ull << 30);
				Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kP13SourceRing, AcquireForm::Gpu, Vpv::Capture::SourcePlanes::kUnit,
					Vpv::Capture::SourcePlanes::MakeOwnerFill()) == SkeletonMemo::Registration::Registered, L"P13 owner = input/capture");
				VpSkeleton::OwnerFill f;   // the P1 GPU owner's declaration (P1GpuFill.cpp MakeP1GpuOwnerFill), a counting body
				f.variantAxes = VpFrameAnalysis::P1Gpu::OwnerVariantAxes();
				f.pixelData = false;
				f.keepFrames = 1;
				f.historyFrames = 1;
				f.fill = [this](const VpSkeleton::OwnerFillRequest& req, SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					++p1Calls;
					p1Levels.push_back(VpFrameAnalysis::P1Gpu::MatcherPyramidLevel(req.variant));
					p1Seqs.push_back(req.seq);
					if (p1Decline)
					{
						out.declineReason = "p1_gpu: test decline";
						return false;
					}
					out.gpu.resource = &level;
					out.gpu.width = uint32_t(level.params.w);
					out.gpu.height = uint32_t(level.params.h);
					return true;
				};
				Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kP1HalfLuma, AcquireForm::Gpu, "input/source_analysis/lane", f)
					== SkeletonMemo::Registration::Registered, L"kP1HalfLuma GPU owner = input/source_analysis/lane");
				demand.memo = withMemo ? memo : std::shared_ptr<SkeletonMemo>();
				demand.host = Host();
				Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new FakeSkeleton(&demand)));
			}
			~Rig()
			{
				Vpv::Capture::SourcePlanes::Forget(Host());
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();   // the module (and the pathway facts) go first
			}

			pl_frame Image(pl_tex_t* luma, pl_tex_t* chroma, bool fullRange) const
			{
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = luma;
				image.planes[0].components = 1;
				image.planes[1].texture = chroma;
				image.planes[1].components = 2;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = fullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
				return image;
			}
		};

		Level Ask(const VpMotion::MatcherPyramid::Source& src, uint64_t seq, int lv, const void* luma, bool fullRange = false)
		{
			Request q;
			q.seq = seq;
			q.level = lv;
			q.lumaTex = luma;
			q.fullRange = fullRange;
			return src(q);
		}
	}

	TEST_CLASS(MatcherPyramidReaderTests)
	{
	public:
		// (a) HIGH-1 + MED-2: Begin -> offers of A and B -> the seam serves A's and B's levels 1 / 3 / 5 from the P1 owner fill
		// (Product, not Unavailable) -> the Entrance offer of A still succeeds and wins Find -> a later P13 Acquire of A is Ok.
		TEST_METHOD(OfferedMatcherPlanesAreServedAsTheProductAndDoNotPoisonP13)
		{
			Rig rig;
			VpMotion::VectorEngine engine(rig.Host());
			Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
			Assert::IsTrue(engine.OfferMatcherPlanes(10, &rig.lumaA, &rig.chromaA, false), L"A offered");
			Assert::IsTrue(engine.OfferMatcherPlanes(11, &rig.lumaB, &rig.chromaB, false), L"B offered");
			const std::shared_ptr<const Vpv::Capture::SourcePlaneSet> a = Vpv::Capture::SourcePlanes::Find(rig.Host(), 10);
			Assert::IsTrue(a && a->generation == VpMotion::VectorEngine::kMatcherOfferGeneration && a->tex[0] == &rig.lumaA
				&& a->planes == 2 && !a->fullRange && !a->rgb, L"A's offer: the matcher's luma, generation -1, limited, not RGB");

			const VpMotion::MatcherPyramid::Source src = engine.MatcherPyramidSource();
			Assert::IsTrue(static_cast<bool>(src), L"a seam");
			const int levels[3] = { 1, 3, 5 };
			for (uint64_t seq : { uint64_t(10), uint64_t(11) })
				for (int lv : levels)
				{
					const Level r = Ask(src, seq, lv, seq == 10 ? &rig.lumaA : &rig.lumaB);
					Assert::IsTrue(r.state == Level::State::Product, (L"served from the product, level " + std::to_wstring(lv)
						+ L": " + std::wstring(r.cause.begin(), r.cause.end())).c_str());
					Assert::IsTrue(r.tex == &rig.level && r.pin != nullptr && r.cause.empty(), L"the owner's texture, pinned");
				}
			Assert::AreEqual(6, rig.p1Calls, L"the P1 owner filled each (seq, level) once");
			Assert::IsTrue((rig.p1Levels == std::vector<uint8_t>{ 1, 3, 5, 1, 3, 5 }), L"exactly the levels asked, in the pyramid key");
			Assert::IsTrue((rig.p1Seqs == std::vector<uint64_t>{ 10, 10, 10, 11, 11, 11 }), L"of A and B");
			Assert::IsTrue(Ask(src, 10, 3, &rig.lumaA).state == Level::State::Product && rig.p1Calls == 6, L"read again: reused, not refilled");

			// The Entrance phase of the same render frame: A's offer still succeeds and wins Find (generation 1 > -1).
			Assert::IsTrue(Vpv::Capture::SourcePlanes::OfferFrame(rig.Host(), 10, 1, rig.Image(&rig.lumaA, &rig.chromaA, false)), L"Entrance offer of A");
			const std::shared_ptr<const Vpv::Capture::SourcePlaneSet> entrance = Vpv::Capture::SourcePlanes::Find(rig.Host(), 10);
			Assert::IsTrue(entrance && entrance->generation == 1, L"the Entrance offer wins later Find calls");
			const AcquireResult p13 = rig.memo->Acquire(VpSkeleton::kP13SourceRing, 10, AcquireForm::Gpu);
			Assert::IsTrue(p13.Ok() && p13.gpu.resource == &rig.lumaA, L"a later P13 reader of A is served (no cached decline)");
			const AcquireResult p13b = rig.memo->Acquire(VpSkeleton::kP13SourceRing, 11, AcquireForm::Gpu);
			Assert::IsTrue(p13b.Ok() && p13b.gpu.resource == &rig.lumaB, L"and of B");
		}

		// (b) MED-2: a seq that was not offered is declined from the offer table, before any P13 Acquire: no decline is cached,
		// so the frame's later offer and P13 readers are served.
		TEST_METHOD(ASeqNotOfferedIsDeclinedWithoutCachingADecline)
		{
			Rig rig;
			VpMotion::VectorEngine engine(rig.Host());
			Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
			const VpMotion::MatcherPyramid::Source src = engine.MatcherPyramidSource();
			const Level r = Ask(src, 42, 1, &rig.lumaA);
			Assert::IsTrue(r.state == Level::State::Unavailable, L"asked and not served");
			Assert::AreEqual(std::string("no shared source luma for this frame"), r.cause, L"the cause");
			Assert::AreEqual(0, rig.p1Calls, L"the P1 owner was not asked");
			Assert::IsTrue(engine.OfferMatcherPlanes(42, &rig.lumaA, nullptr, false), L"offered afterwards (luma only)");
			const AcquireResult p13 = rig.memo->Acquire(VpSkeleton::kP13SourceRing, 42, AcquireForm::Gpu);
			Assert::IsTrue(p13.Ok() && p13.gpu.resource == &rig.lumaA, L"the P13 Acquire of that seq is Ok: the seam cached nothing");
			Assert::IsTrue(Ask(src, 42, 1, &rig.lumaA).state == Level::State::Product, L"and the seam now serves it");
		}

		// (c) MED-3: the shared plane's range differs from the range the matcher codes level 1 with: declined by name.
		TEST_METHOD(ARangeMismatchIsDeclinedByName)
		{
			Rig rig;
			VpMotion::VectorEngine engine(rig.Host());
			Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
			Assert::IsTrue(engine.OfferMatcherPlanes(20, &rig.lumaA, &rig.chromaA, true), L"offered full range");
			const VpMotion::MatcherPyramid::Source src = engine.MatcherPyramidSource();
			const Level r = Ask(src, 20, 1, &rig.lumaA, false);
			Assert::IsTrue(r.state == Level::State::Unavailable, L"declined");
			Assert::AreEqual(std::string("range differs from the shared plane"), r.cause, L"the cause");
			Assert::AreEqual(0, rig.p1Calls, L"no product made for a mismatched range");
			Assert::IsTrue(Ask(src, 20, 1, &rig.lumaA, true).state == Level::State::Product, L"the same range is served");
		}

		// (d) The named exceptions of plan (b) (RGB structure luma, DV unpack output): every level Unavailable with the name.
		TEST_METHOD(ANamedExceptionDeclinesEveryLevelByName)
		{
			Rig rig;
			VpMotion::VectorEngine engine(rig.Host());
			Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
			Assert::IsTrue(engine.OfferMatcherPlanes(30, &rig.lumaA, &rig.chromaA, false), L"offered");
			const VpMotion::MatcherPyramid::Source src = engine.MatcherPyramidSource("RGB source: the matcher's luma comes from the RGB unpack");
			for (int lv = 1; lv <= 6; ++lv)
			{
				const Level r = Ask(src, 30, lv, &rig.lumaA);
				Assert::IsTrue(r.state == Level::State::Unavailable, L"declined");
				Assert::AreEqual(std::string("named exception: RGB source: the matcher's luma comes from the RGB unpack"), r.cause, L"named");
			}
			Assert::AreEqual(0, rig.p1Calls, L"the P1 owner was not asked");
			Assert::IsTrue(engine.MatcherPyramidSource(nullptr) && Ask(engine.MatcherPyramidSource(nullptr), 30, 1, &rig.lumaA).state == Level::State::Product,
				L"null = no exception");
		}

		// (e) No memo for the frame (no skeleton module), the Original pathway, no host, a level outside 1..6: NotConfigured,
		// silent (nothing to fall back from).
		TEST_METHOD(NoMemoOrOriginalPathwayIsNotConfiguredAndSilent)
		{
			{
				Rig rig(false);
				VpMotion::VectorEngine engine(rig.Host());
				Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
				Assert::IsTrue(engine.OfferMatcherPlanes(40, &rig.lumaA, &rig.chromaA, false), L"offered (the table has no memo dependency)");
				const Level r = Ask(engine.MatcherPyramidSource(), 40, 1, &rig.lumaA);
				Assert::IsTrue(r.state == Level::State::NotConfigured && r.cause.empty(), L"no memo: NotConfigured, no cause");
				Assert::IsTrue(Ask(engine.MatcherPyramidSource("RGB"), 40, 1, &rig.lumaA).state == Level::State::NotConfigured,
					L"no memo: not even a named exception is shown");
			}
			{
				Rig rig;
				VpMotion::VectorEngine engine(rig.Host());
				Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
				Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().NoteOriginalPathway(rig.Host(), true), L"Original pathway recorded");
				Assert::IsFalse(engine.OfferMatcherPlanes(41, &rig.lumaA, &rig.chromaA, false), L"Original: nothing offered");
				Assert::IsTrue(!Vpv::Capture::SourcePlanes::Find(rig.Host(), 41), L"the table is untouched");
				const Level r = Ask(engine.MatcherPyramidSource(), 41, 1, &rig.lumaA);
				Assert::IsTrue(r.state == Level::State::NotConfigured && r.cause.empty(), L"Original: NotConfigured, silent");
				Assert::AreEqual(0, rig.p1Calls, L"the P1 owner was not asked");
			}
			{
				Rig rig;
				VpMotion::VectorEngine engine(rig.Host());
				Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
				Assert::IsTrue(engine.OfferMatcherPlanes(43, &rig.lumaA, &rig.chromaA, false), L"offered");
				const VpMotion::MatcherPyramid::Source src = engine.MatcherPyramidSource();
				Assert::IsTrue(Ask(src, 43, 0, &rig.lumaA).state == Level::State::NotConfigured, L"level 0");
				Assert::IsTrue(Ask(src, 43, 7, &rig.lumaA).state == Level::State::NotConfigured, L"level 7");
				Assert::IsTrue(Ask(src, 43, 1, nullptr).state == Level::State::NotConfigured, L"no luma");
				Assert::IsFalse(engine.OfferMatcherPlanes(0, &rig.lumaA, nullptr, false), L"seq 0 is never offered");
				Assert::IsFalse(engine.OfferMatcherPlanes(44, nullptr, nullptr, false), L"no luma is never offered");
				VpMotion::VectorEngine hostless;
				Assert::IsFalse(hostless.OfferMatcherPlanes(43, &rig.lumaA, nullptr, false), L"no host: nothing offered");
				Assert::IsTrue(Ask(hostless.MatcherPyramidSource(), 43, 1, &rig.lumaA).state == Level::State::NotConfigured, L"no host: NotConfigured");
			}
		}

		// (f) The matcher's luma is not the shared plane of that seq: the named exception, not a product of another picture.
		TEST_METHOD(AMatcherLumaThatIsNotTheSharedPlaneIsANamedException)
		{
			Rig rig;
			VpMotion::VectorEngine engine(rig.Host());
			Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
			Assert::IsTrue(engine.OfferMatcherPlanes(50, &rig.lumaA, &rig.chromaA, false), L"offered");
			const Level r = Ask(engine.MatcherPyramidSource(), 50, 1, &rig.other);
			Assert::IsTrue(r.state == Level::State::Unavailable, L"declined");
			Assert::AreEqual(std::string("named exception: the matcher's luma is not the shared source plane"), r.cause, L"named");
			Assert::AreEqual(0, rig.p1Calls, L"the P1 owner was not asked");
		}

		// The P1 owner declines: the seam says why (the owner's cause), so the matcher's row is worded with it.
		TEST_METHOD(AnOwnerDeclineCarriesTheOwnersCause)
		{
			Rig rig;
			rig.p1Decline = true;
			VpMotion::VectorEngine engine(rig.Host());
			Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
			Assert::IsTrue(engine.OfferMatcherPlanes(60, &rig.lumaA, &rig.chromaA, false), L"offered");
			const Level r = Ask(engine.MatcherPyramidSource(), 60, 1, &rig.lumaA);
			Assert::IsTrue(r.state == Level::State::Unavailable && rig.p1Calls == 1, L"asked and declined");
			Assert::IsTrue(PyrHas(r.cause, "p1_gpu: test decline"), (L"the owner's cause: " + std::wstring(r.cause.begin(), r.cause.end())).c_str());
		}

		// (g) The visible row of the matcher's own pyramid: the catalog's matcher row 13 fallback product.motion_warp (LOW-2: one row,
		// the former product.matcher_pyramid entry removed), engaged with the seam's cause, released when the cause is empty.
		TEST_METHOD(TheFallbackRowIsTheMotionWarpProductRow)
		{
			const char* const id = "product.motion_warp";
			const FallbackCatalog::ProductEntry* e = FallbackCatalog::FindProductEntry(id);
			Assert::IsNotNull(e, L"catalog row 13's fallback entry");
			Assert::IsNull(FallbackCatalog::FindProductEntry("product.matcher_pyramid"), L"no second row for the same downgrade");
			const std::string why = "no shared source luma for this frame";
			const FallbackCatalog::Decision on = FallbackCatalog::ProductFallback(id, why);
			Assert::IsTrue(on.engaged && on.id == id, L"engaged under the stable id");
			Assert::AreEqual(std::string(e->text) + ": " + why, on.reason, L"the entry's text + the cause");
			Assert::IsTrue(PyrHas(on.reason, "motion warp builds its own"), L"says what runs instead");
			const FallbackCatalog::Decision lowRes = FallbackCatalog::ProductFallback(id, "lowRes inner: " + why);
			Assert::IsTrue(lowRes.engaged && PyrHas(lowRes.reason, "lowRes inner: no shared source luma"), L"MED-1: the inner's decline is worded");
			const FallbackCatalog::Decision off = FallbackCatalog::Released(id);
			Assert::IsTrue(!off.engaged && off.id == id && off.reason.empty(), L"released");
		}
	};
}
