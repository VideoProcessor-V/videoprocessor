// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// CB slice 18 SHOULD-1 / SHOULD-2 (review-cb-s18-r1): X5's measure edge map through the seam MeasureEdgeMap.h, on the CPU tier
// (textures are plain structs; the owner fill is either a fake that records its key or the real fill on a device that makes no
// texture):
//   1. with a skeleton memo, X5 reads THE owner's product: kEdgeMap acquired at StageAfter(ScalingGeometry), the measured picture's
//      own size, the per-seq ordinal, code unit 8; the picture is offered to P13 first; the status line is cleared;
//   2. a genuine Unavailable is visible: null and "Adaptive downscale: edge map unavailable (<the memo's cause>)", exactly;
//   3. one compute per (seq, picture): a second ask of the same key is a memo hit; a second picture of the seq (ordinal 1) is its
//      own fill, and the ahead budget then counts the reserve once per fill (SHOULD-2 pin: bytesPerFrame = reserve x fills);
//   4. without a memo the module's own dispatch runs once, the status says so, and a failed dispatch appends its why.
// The GPU result of the fill itself is EdgeMapStageGpuParityTests (GPU tier).

#include <VP-V/core/QueueWindow.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapFill.h>
#include <VP-V/modules/scaling/output/EdgeMapStageOrdinal.h>
#include <VP-V/modules/scaling/output/MeasureEdgeMap.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace MeasureEdgeMapTestsNs
{
	using namespace VpSkeleton;
	using Vpv::Capture::SourcePlanes;

	bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

	Dims DimsOf(uint32_t w, uint32_t h)
	{
		Dims d;
		d.width = w;
		d.height = h;
		return d;
	}

	// Non-null opaque handles; makes no texture (the real fill then declines with the device's reason).
	struct RefusingDevice : IOwnerGpuDevice
	{
		int gpuTag = 0;
		int dispatchTag = 0;
		void* OwnerGpu() const override { return const_cast<int*>(&gpuTag); }
		void* OwnerDispatch() const override { return const_cast<int*>(&dispatchTag); }
		bool CreateOwnerTexture(const TexShape&, GpuMemoHandle&, std::string& why) override
		{
			why = "test device makes no texture";
			return false;
		}
	};

	// The fake kEdgeMap owner: the real fill's axes, defaults and thread contract, a fill that records every key it is asked
	// for and serves `tex` at `bytes`.
	struct FakeEdgeMap
	{
		pl_tex_t tex{};
		uint64_t bytes = 0;
		std::vector<VariantKey> asked;
	};

	OwnerFill FakeEdgeMapFill(FakeEdgeMap& fake, uint64_t reserve)
	{
		OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(reserve);
		FakeEdgeMap* const p = &fake;
		f.fill = [p](const OwnerFillRequest& req, SkeletonMemo&, OwnerFillOutput& out) {
			p->asked.push_back(req.variant);
			out.gpu.resource = &p->tex;
			out.gpu.bytes = p->bytes;
			return true;
		};
		return f;
	}

	// A memo set up as X5's render thread sees it: host, render thread, GPU dispatcher and a ledger grant, P13 registered.
	std::shared_ptr<SkeletonMemo> RenderMemo(const void* host)
	{
		std::shared_ptr<SkeletonMemo> m = std::make_shared<SkeletonMemo>();
		m->SetHost(host);
		m->SetRenderThread(std::this_thread::get_id());
		m->SetGpuDispatcherPresent(true);
		m->SetGpuBudget(true, uint64_t(1) << 30);
		m->RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill());
		return m;
	}

	Vpv::Skeleton::FrameMemo Frame(const std::shared_ptr<SkeletonMemo>& m, const void* host, uint64_t seq)
	{
		Vpv::Skeleton::FrameMemo fm;
		fm.memo = m;
		fm.host = host;
		fm.seq = seq;
		return fm;
	}

	pl_color_space Bt709()
	{
		pl_color_space c{};
		c.primaries = PL_COLOR_PRIM_BT_709;
		c.transfer = PL_COLOR_TRC_BT_1886;
		return c;
	}

	// The no-memo dispatch must never run on the memo path.
	pl_tex NeverLocal(std::string& why)
	{
		why = "must not be called";
		Assert::Fail(L"the module's own dispatch ran with a memo present");
		return nullptr;
	}

	TEST_CLASS(MeasureEdgeMapTests)
	{
	public:
		TEST_METHOD(MemoPathReadsTheOwnersEdgeMapAtTheX5Key)
		{
			int host = 0;
			pl_tex_t base{};
			base.params.w = 64;
			base.params.h = 48;
			const uint64_t reserve = Vpv::Regions::EdgeMapFillReserveBytes(DimsOf(64, 48));
			std::shared_ptr<SkeletonMemo> m = RenderMemo(&host);
			FakeEdgeMap fake;
			fake.bytes = reserve;
			Assert::IsTrue(m->RegisterOwnerFill(kEdgeMap, AcquireForm::Gpu, Vpv::Regions::kEdgeMapUnit, FakeEdgeMapFill(fake, reserve))
				== SkeletonMemo::Registration::Registered, L"kEdgeMap owner");

			AdaptiveDownscale::EdgeMapStageOrdinal ordinals;
			std::string status = "stale";
			const pl_tex em = AdaptiveDownscale::AcquireMeasureEdgeMap(Frame(m, &host, 7), ordinals, &base, Bt709(),
				PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709, NeverLocal, status);

			Assert::IsTrue(em == &fake.tex, L"the owner's texture is the one the measure reads");
			Assert::IsTrue(status.empty(), L"served by the memo: the status line is cleared");
			Assert::AreEqual(size_t(1), fake.asked.size(), L"the owner filled once");
			const VariantKey& k = fake.asked[0];
			Assert::IsTrue(AdaptiveDownscale::MeasureEdgeMapStage() ==
				VariantKey::StageAfter(static_cast<uint8_t>(PipelineSlots::Id::ScalingGeometry)), L"X5's stage: after scaling.geometry");
			Assert::IsTrue(k.stage == AdaptiveDownscale::MeasureEdgeMapStage(), L"the key's stage is X5's");
			Assert::IsTrue(k.width == 64 && k.height == 48, L"the measured picture's own (base) size");
			Assert::IsTrue(k.param1 == 0, L"the first picture of the seq: ordinal 0");
			Assert::IsTrue(k.codeUnit == Vpv::Regions::kEdgeMapStageCodeUnit, L"the stage variant's code unit 8");
			Assert::IsTrue(k == Vpv::Regions::EdgeMapStageKey(AdaptiveDownscale::MeasureEdgeMapStage(), 64, 48, 0).Canonical(
				Vpv::Regions::MakeEdgeMapOwnerFill(reserve).defaults), L"exactly the owner's EdgeMapStageKey");

			const std::shared_ptr<const Vpv::Capture::StagePicture> offered =
				SourcePlanes::FindStage(&host, 7, AdaptiveDownscale::MeasureEdgeMapStage(), 64, 48, 0);
			Assert::IsTrue(offered != nullptr && offered->tex == &base, L"the measured picture was offered to P13 under the same key");
			Assert::AreEqual(std::string("scaling.output (X5)"), std::string(offered->origin), L"offered as X5's picture");
			SourcePlanes::Forget(&host);
		}

		TEST_METHOD(UnavailableIsVisibleWithTheMemosCause)
		{
			int host = 0;
			pl_tex_t base{};
			base.params.w = 64;
			base.params.h = 48;
			const uint64_t reserve = Vpv::Regions::EdgeMapFillReserveBytes(DimsOf(64, 48));
			AdaptiveDownscale::EdgeMapStageOrdinal ordinals;
			{   // The real fill on a device that makes no texture: the device's reason reaches the status line.
				std::shared_ptr<SkeletonMemo> m = RenderMemo(&host);
				RefusingDevice dev;
				m->SetGpuDevice(&dev);
				Assert::IsTrue(m->RegisterOwnerFill(kEdgeMap, AcquireForm::Gpu, Vpv::Regions::kEdgeMapUnit,
					Vpv::Regions::MakeEdgeMapOwnerFill(reserve)) == SkeletonMemo::Registration::Registered, L"the real owner");
				std::string status;
				const pl_tex em = AdaptiveDownscale::AcquireMeasureEdgeMap(Frame(m, &host, 7), ordinals, &base, Bt709(),
					PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709, NeverLocal, status);
				Assert::IsTrue(em == nullptr, L"not served: the measure takes its failure path");
				const std::string cause = m->LastCause(kEdgeMap);
				Assert::IsFalse(cause.empty(), L"the memo says why");
				Assert::AreEqual("Adaptive downscale: edge map unavailable (" + cause + ")", status, L"the status line, worded");
				Assert::IsTrue(Has(status, "test device makes no texture"), L"the device's own reason is visible");
				m->SetGpuDevice(nullptr);
				SourcePlanes::Forget(&host);
			}
			{   // No kEdgeMap owner registered: the memo's NoOwnerFill cause, same wording.
				std::shared_ptr<SkeletonMemo> m = RenderMemo(&host);
				std::string status;
				const pl_tex em = AdaptiveDownscale::AcquireMeasureEdgeMap(Frame(m, &host, 8), ordinals, &base, Bt709(),
					PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709, NeverLocal, status);
				Assert::IsTrue(em == nullptr, L"no owner: not served");
				const std::string cause = m->LastCause(kEdgeMap);
				Assert::IsFalse(cause.empty(), L"the memo says why");
				Assert::AreEqual("Adaptive downscale: edge map unavailable (" + cause + ")", status, L"the status line, worded");
				SourcePlanes::Forget(&host);
			}
		}

		TEST_METHOD(OneComputePerSeqAndPictureAndAheadCountsEachFill)
		{
			int host = 0;
			pl_tex_t base{};
			base.params.w = 1920;
			base.params.h = 1080;
			const uint64_t reserve = Vpv::Regions::EdgeMapFillReserveBytes(DimsOf(1920, 1080));
			Assert::AreEqual(uint64_t(1920) * 1080u * 8u, reserve, L"one reserve figure: 8 B per pixel (SHOULD-2)");
			std::shared_ptr<SkeletonMemo> m = RenderMemo(&host);
			FakeEdgeMap fake;
			fake.bytes = reserve;
			m->RegisterOwnerFill(kEdgeMap, AcquireForm::Gpu, Vpv::Regions::kEdgeMapUnit, FakeEdgeMapFill(fake, reserve));
			AdaptiveDownscale::EdgeMapStageOrdinal ordinals;
			const VariantKey first = Vpv::Regions::EdgeMapStageKey(AdaptiveDownscale::MeasureEdgeMapStage(), 1920, 1080, 0);

			std::string status;
			Assert::IsTrue(AdaptiveDownscale::AcquireMeasureEdgeMap(Frame(m, &host, 7), ordinals, &base, Bt709(), PL_COLOR_SYSTEM_BT_709,
				PL_COLOR_PRIM_BT_709, NeverLocal, status) == &fake.tex, L"the main render's picture");
			Assert::AreEqual(uint64_t(1), m->CountersOf(kEdgeMap).ownerFillCalls, L"computed once");

			// Another reader of the same (seq, picture): a memo hit, no second compute.
			const AcquireResult again = m->Acquire(kEdgeMap, 7, AcquireForm::Gpu, first);
			Assert::IsTrue(again.status == AcquireStatus::Reused, L"the same key is a memo hit");
			Assert::IsTrue(again.gpu.resource == &fake.tex, L"the same texture");
			Assert::AreEqual(uint64_t(1), m->CountersOf(kEdgeMap).ownerFillCalls, L"still computed once");
			Assert::AreEqual(size_t(1), fake.asked.size(), L"the owner was asked once");

			const QueueWindow w = DeriveWindow(QueueWindowTerms(), 7, 0);
			const auto edgeCost = [&]() -> uint64_t {
				const AheadNeed n = m->MakeAheadNeed(w, QueueMode::Normal);
				for (const AheadCost& c : n.costs)
					if (c.id == kEdgeMap && c.form == 1)
						return c.bytesPerFrame;
				Assert::Fail(L"kEdgeMap is costed in the ahead budget");
				return 0;
			};
			Assert::AreEqual(reserve, edgeCost(), L"one fill this frame so far: the reserve once");

			// A second picture of the same seq (a de-judder extras re-render): ordinal 1, its own fill, never the first one's map.
			Assert::IsTrue(AdaptiveDownscale::AcquireMeasureEdgeMap(Frame(m, &host, 7), ordinals, &base, Bt709(), PL_COLOR_SYSTEM_BT_709,
				PL_COLOR_PRIM_BT_709, NeverLocal, status) == &fake.tex, L"the extras picture");
			Assert::AreEqual(uint64_t(2), m->CountersOf(kEdgeMap).ownerFillCalls, L"a second picture is a second compute");
			Assert::AreEqual(size_t(2), fake.asked.size(), L"the owner was asked for the second picture");
			Assert::IsTrue(fake.asked[1].param1 == 1 && fake.asked[0].param1 == 0, L"ordinals 0 then 1");
			Assert::AreEqual(2u * reserve, edgeCost(), L"SHOULD-2 pin: the ahead spot holds reserve x the measured fills per frame");
			SourcePlanes::Forget(&host);
		}

		// r2 MUST-1 / SHOULD-1: the multiplier is what one render frame HOLDS. A cadence repeat seals the same source seq again,
		// which releases the earlier render's frame-scoped edge map; its re-measure (ordinal 1) is then the only one held, so the
		// ahead cost stays at one reserve. A discontinuity (Clear, BumpEpoch) measures the occupancy again.
		TEST_METHOD(CadenceRepeatSealDoesNotMultiplyTheAheadCost)
		{
			int host = 0;
			pl_tex_t base{};
			base.params.w = 1920;
			base.params.h = 1080;
			const uint64_t reserve = Vpv::Regions::EdgeMapFillReserveBytes(DimsOf(1920, 1080));
			std::shared_ptr<SkeletonMemo> m = RenderMemo(&host);
			FakeEdgeMap fake;
			fake.bytes = reserve;
			m->RegisterOwnerFill(kEdgeMap, AcquireForm::Gpu, Vpv::Regions::kEdgeMapUnit, FakeEdgeMapFill(fake, reserve));
			AdaptiveDownscale::EdgeMapStageOrdinal ordinals;
			const QueueWindow w = DeriveWindow(QueueWindowTerms(), 7, 0);
			const auto edgeCost = [&]() -> uint64_t {
				const AheadNeed n = m->MakeAheadNeed(w, QueueMode::Normal);
				for (const AheadCost& c : n.costs)
					if (c.id == kEdgeMap && c.form == 1)
						return c.bytesPerFrame;
				Assert::Fail(L"kEdgeMap is costed in the ahead budget");
				return 0;
			};
			const auto measure = [&]() -> pl_tex {
				std::string status;
				return AdaptiveDownscale::AcquireMeasureEdgeMap(Frame(m, &host, 7), ordinals, &base, Bt709(), PL_COLOR_SYSTEM_BT_709,
					PL_COLOR_PRIM_BT_709, NeverLocal, status);
			};

			m->OnSeal(7);   // the render of source frame 7
			Assert::IsTrue(measure() == &fake.tex, L"the first render's picture");
			Assert::AreEqual(reserve, edgeCost(), L"one fill held: one reserve");

			m->OnSeal(7);   // a cadence repeat: the same source seq rendered again (the earlier frame-scoped map is released)
			Assert::IsTrue(measure() == &fake.tex, L"the repeat's picture");
			Assert::AreEqual(size_t(2), fake.asked.size(), L"the repeat is its own compute");
			Assert::IsTrue(fake.asked[1].param1 == 1, L"under the next ordinal");
			Assert::AreEqual(reserve, edgeCost(), L"MUST-1: still one fill held at once: reserve x 1, not x 2");

			// Within one render interval, two pictures are held together: x 2 (the other side of the pin).
			Assert::IsTrue(measure() == &fake.tex, L"an extras picture in the same render");
			Assert::AreEqual(2u * reserve, edgeCost(), L"two fills held at once: reserve x 2");

			m->BumpEpoch(SkeletonMemo::InvalidateCause::Discontinuity);
			Assert::AreEqual(reserve, edgeCost(), L"a new epoch measures its occupancy again");
			m->OnSeal(7);
			Assert::IsTrue(measure() == &fake.tex && measure() == &fake.tex, L"two pictures again");
			Assert::AreEqual(2u * reserve, edgeCost(), L"measured again in the new epoch");
			m->Clear();
			Assert::AreEqual(reserve, edgeCost(), L"Clear(): a new graph measures its occupancy again");
			SourcePlanes::Forget(&host);
		}

		TEST_METHOD(NoMemoComputesInTheModuleVisibly)
		{
			pl_tex_t base{};
			base.params.w = 64;
			base.params.h = 48;
			pl_tex_t local{};
			const Vpv::Skeleton::FrameMemo none;
			Assert::IsFalse(none.Valid(), L"no skeleton memo on this thread");
			AdaptiveDownscale::EdgeMapStageOrdinal ordinals;
			{   // The module's own dispatch succeeds: used once, the status line says so.
				int calls = 0;
				std::string status;
				const pl_tex em = AdaptiveDownscale::AcquireMeasureEdgeMap(none, ordinals, &base, Bt709(), PL_COLOR_SYSTEM_BT_709,
					PL_COLOR_PRIM_BT_709, [&](std::string&) -> pl_tex { ++calls; return &local; }, status);
				Assert::IsTrue(em == &local, L"the module's own texture");
				Assert::AreEqual(1, calls, L"dispatched once");
				Assert::AreEqual(std::string("Adaptive downscale: edge map computed in the module (no skeleton memo)"), status,
					L"visible, worded");
			}
			{   // The owner's dispatch without a dispatch object: null, and its why appended to the status line.
				std::string status;
				const pl_tex em = AdaptiveDownscale::AcquireMeasureEdgeMap(none, ordinals, &base, Bt709(), PL_COLOR_SYSTEM_BT_709,
					PL_COLOR_PRIM_BT_709,
					[&](std::string& why) -> pl_tex {
						if (!Vpv::Regions::DispatchEdgeMapStage(nullptr, &base, Bt709(), PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709,
							&local, why))
							return nullptr;
						return &local;
					},
					status);
				Assert::IsTrue(em == nullptr, L"not computed: the measure takes its failure path");
				Assert::AreEqual(std::string("Adaptive downscale: edge map computed in the module (no skeleton memo)"
					" - edge_map: no dispatch, picture or output texture"), status, L"the dispatch's why is visible");
			}
			Assert::IsTrue(ordinals.Next(7) == 0, L"the no-memo path takes no picture ordinal");
		}
	};
}
