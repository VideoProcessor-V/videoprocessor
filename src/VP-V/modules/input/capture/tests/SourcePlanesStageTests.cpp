// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// CB slice 18 S1 (coreplan-CB-18-edgemap.md S1; lead14/design-EDGEMAP-DOMAIN-AXIS.md rev 2 s2a): P13 STAGE PICTURES. A stage
// picture offered with SourcePlanes::OfferStage is served through Acquire(kP13SourceRing, seq, Gpu, StageKey(stage, w, h,
// ordinal)) as a VIEW (0 bytes), frame-scoped, render thread only; size and ordinal tell pictures apart; every decline says
// why; the table is bounded and a refusal is counted. CPU only (textures are plain structs), single process, bounded.

#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>

#include <string>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SourcePlanesStageTestsNs
{
	using namespace VpSkeleton;
	using Vpv::Capture::SourcePlanes;
	using Vpv::Capture::StagePicture;

	bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

	const uint8_t kStage = 5;   // any non-zero stage id (the X5 offer uses StageAfter(the PipelineSlots id))

	StagePicture Picture(uint64_t seq, uint16_t ordinal, pl_tex tex)
	{
		StagePicture p;
		p.seq = seq;
		p.stage = kStage;
		p.ordinal = ordinal;
		p.tex = tex;
		p.system = PL_COLOR_SYSTEM_BT_709;
		p.primaries = PL_COLOR_PRIM_BT_709;
		p.origin = "test stage";
		return p;
	}

	TEST_CLASS(SourcePlanesStageTests)
	{
	public:
		TEST_METHOD(StagePictureServedBySizeAndOrdinalFrameScoped)
		{
			int host = 0;
			pl_tex_t full{}, half{}, rerender{};
			full.params.w = 64;
			full.params.h = 48;
			half.params.w = 32;
			half.params.h = 24;
			rerender.params.w = 64;
			rerender.params.h = 48;

			SkeletonMemo m;
			m.SetHost(&host);
			m.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(m.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill())
				== SkeletonMemo::Registration::Registered, L"P13 owner = input/capture");
			Assert::IsTrue(SourcePlanes::OfferStage(&host, Picture(7, 0, &full)), L"the full picture offered");
			Assert::IsTrue(SourcePlanes::OfferStage(&host, Picture(7, 0, &half)), L"a half-size picture of the same frame offered");
			Assert::IsTrue(SourcePlanes::OfferStage(&host, Picture(7, 1, &rerender)), L"a re-render (ordinal 1) offered");
			Assert::IsTrue(SourcePlanes::OfferStage(&host, Picture(7, 1, &rerender)), L"a second offer of one key replaces the first");
			Assert::IsTrue(SourcePlanes::CountersOf(&host).stageCurrent == 3, L"three distinct stage pictures held");

			// Served as a view by stage + size + ordinal; no dispatcher and no ledger grant needed.
			const AcquireResult a = m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, SourcePlanes::StageKey(kStage, 64, 48, 0));
			Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && a.gpu.resource == &full && a.gpu.bytes == 0 && a.gpu.width == 64
				&& a.gpu.height == 48, L"the full picture, a 0-byte view");
			const StagePicture* facts = SourcePlanes::FactsOfStage(a);
			Assert::IsTrue(facts != nullptr && facts->seq == 7 && facts->system == PL_COLOR_SYSTEM_BT_709
				&& facts->primaries == PL_COLOR_PRIM_BT_709 && std::string(facts->origin) == "test stage", L"the stage facts come with the view");
			Assert::IsTrue(SourcePlanes::FactsOf(a) == nullptr, L"a stage result is not a source-plane set");
			Assert::IsTrue(m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, SourcePlanes::StageKey(kStage, 32, 24, 0)).gpu.resource == &half,
				L"the size tells the half-size picture from the full one");
			Assert::IsTrue(m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, SourcePlanes::StageKey(kStage, 64, 48, 1)).gpu.resource == &rerender,
				L"the ordinal tells a re-render from the first picture");
			Assert::IsTrue(m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, SourcePlanes::StageKey(kStage, 64, 48, 0)).status == AcquireStatus::Reused
				&& m.GpuBytes() == 0, L"reused, 0 bytes");

			// Declines say why.
			const AcquireResult other = m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, SourcePlanes::StageKey(kStage + 1, 64, 48, 0));
			Assert::IsTrue(other.cause == AcquireCause::OwnerDeclined && Has(m.LastCause(kP13SourceRing), "stage not offered this frame"),
				L"another stage: not offered, said so");
			const AcquireResult wrongSize = m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, SourcePlanes::StageKey(kStage, 48, 48, 0));
			Assert::IsTrue(wrongSize.cause == AcquireCause::OwnerDeclined && Has(m.LastCause(kP13SourceRing), "stage not offered this frame"),
				L"a size nobody offered: declined");
			VariantKey twoPlanes = SourcePlanes::StageKey(kStage, 64, 48, 0);
			twoPlanes.plane = 2;
			Assert::IsTrue(m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, twoPlanes).cause == AcquireCause::OwnerDeclined
				&& Has(m.LastCause(kP13SourceRing), "stage picture has one plane"), L"a second plane of a stage picture: declined");
			VariantKey sizeOnly;
			sizeOnly.width = 64;
			sizeOnly.height = 48;
			Assert::IsTrue(m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, sizeOnly).cause == AcquireCause::OwnerDeclined
				&& Has(m.LastCause(kP13SourceRing), "size / ordinal only with a stage"), L"size without a stage: declined, said so");
			VariantKey cropped = SourcePlanes::StageKey(kStage, 64, 48, 0);
			cropped.cropW = 16;
			cropped.cropH = 16;
			Assert::IsTrue(m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, cropped).cause == AcquireCause::VariantUndeclared,
				L"the crop axis is not served (a whole-picture view)");

			// Frame scope: the next seal releases the views, the next Begin drops the offers.
			SourcePlanes::BeginFrame(&host);
			m.OnSeal(8);
			Assert::IsTrue(m.MemoCount() == 0, L"frame-scoped views released at the next seal");
			Assert::IsTrue(SourcePlanes::CountersOf(&host).stageCurrent == 0, L"the offers dropped at the next Begin");
			const AcquireResult gone = m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, SourcePlanes::StageKey(kStage, 64, 48, 0));
			Assert::IsTrue(gone.cause == AcquireCause::FrameReleased || (gone.cause == AcquireCause::OwnerDeclined
				&& Has(m.LastCause(kP13SourceRing), "stage not offered this frame")), L"the previous frame's picture is not served again");

			// Off the render thread: refused.
			SourcePlanes::OfferStage(&host, Picture(9, 0, &full));
			AcquireResult off;
			std::thread t([&]() { off = m.Acquire(kP13SourceRing, 9, AcquireForm::Gpu, SourcePlanes::StageKey(kStage, 64, 48, 0)); });
			t.join();
			Assert::IsTrue(off.cause == AcquireCause::WrongThread, L"render thread only");
			SourcePlanes::Forget(&host);
		}

		TEST_METHOD(StageTableBoundedRefusalCounted)
		{
			int host = 0;
			pl_tex_t tex{};
			tex.params.w = 16;
			tex.params.h = 16;
			Assert::IsFalse(SourcePlanes::OfferStage(&host, Picture(3, 0, nullptr)), L"no texture: not offered");
			StagePicture noStage = Picture(3, 0, &tex);
			noStage.stage = 0;
			Assert::IsFalse(SourcePlanes::OfferStage(&host, noStage), L"stage 0 is not a stage picture");
			for (uint16_t i = 0; i < uint16_t(SourcePlanes::kMaxStageOffers); ++i)
				Assert::IsTrue(SourcePlanes::OfferStage(&host, Picture(3, i, &tex)), L"within the bound: offered");
			Assert::IsFalse(SourcePlanes::OfferStage(&host, Picture(3, uint16_t(SourcePlanes::kMaxStageOffers), &tex)), L"over the bound: refused");
			const SourcePlanes::Counters c = SourcePlanes::CountersOf(&host);
			Assert::IsTrue(c.stageRefused == 1 && c.stageCurrent == SourcePlanes::kMaxStageOffers && c.stageOffered == SourcePlanes::kMaxStageOffers,
				L"the refusal is counted");
			Assert::IsTrue(c.current == 0 && c.refused == 0, L"the source-frame offers are untouched");
			SourcePlanes::Forget(&host);
		}
	};
}
