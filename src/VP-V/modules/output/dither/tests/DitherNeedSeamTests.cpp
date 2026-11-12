// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// output.dither / dither need host seam (DitherNeedSeam.h): off == nothing, the Original pathway == nothing, product-absent
// and stale products are VISIBLE and counted, the source quality scales the wanted amplitude, the link-noise limit is its own.

#include <VP-V/modules/output/dither/DitherNeedSeam.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace DitherNeedSeamTestsNs
{
	// Synthetic shared products: a flat plane (the sizes are the plane's own; nothing is hard-coded to a raster of the host).
	struct Products
	{
		GrainSplit::Plane luma;
		RegionEvidence::GrainProduct grain;
		RegionEvidence::Precursors pre;
		RegionEvidence::GrainView grainView;
		RegionEvidence::View view;
		LightLevel::Lut lut;
		RegionEvidence::Grid grid;
		SourceIdentity::QualityView quality;

		Products(int w = 96, int h = 64, double qualityValue = 0.0)
		{
			luma = GrainSplit::Plane(w, h);
			for (auto& v : luma.v) v = 300.0f;
			const int B = RegionEvidence::BlockSizeFor(luma.h);
			RegionEvidence::Externals ex;
			grain = RegionEvidence::ComputeGrain(luma, ex.picture, B);
			RegionEvidence::FrameParams fp;
			fp.blockOverride = B;
			pre = RegionEvidence::Compute(luma, &grain.split, ex, fp);
			grainView = RegionEvidence::GrainView(&grain);
			view = RegionEvidence::View(&pre);
			grid = RegionEvidence::MakeGrid(luma.w, luma.h, B);
			for (int i = 0; i < LightLevel::kLutSize; ++i)
			{
				lut.e[static_cast<size_t>(i)].V = 5.0;
				lut.e[static_cast<size_t>(i)].Sout = 1.0;
			}
			lut.fingerprint = "seam-test";
			quality.quality = qualityValue;
			quality.tier = 0;
		}

		DitherNeedSeam::Feed Feed() const
		{
			DitherNeedSeam::Feed f;
			f.grain = &grainView; f.evidence = &view; f.lut = &lut; f.quality = &quality;
			f.colorBits = 8; f.dtSeconds = 1.0 / 24.0;
			return f;
		}
	};

	TEST_CLASS(DitherNeedSeamTests)
	{
	public:

		// dither_need = auto with the sigma-delta not running by configuration: ONE engaged decision with its own wording
		// (same id as the product-missing one, so the registry holds it steadily), released when the condition clears.
		TEST_METHOD(InactiveDecisionIsSteadyNamedAndReleases)
		{
			const FallbackCatalog::Decision a = FallbackCatalog::DitherNeedInactive(true, "output.dither is switched off in pipeline_slots_off");
			const FallbackCatalog::Decision b = FallbackCatalog::DitherNeedInactive(true, "output.dither is switched off in pipeline_slots_off");
			Assert::IsTrue(a.engaged && b.engaged, L"engaged while the condition holds");
			Assert::IsTrue(a.id == b.id && a.reason == b.reason, L"the same decision every frame");
			Assert::IsTrue(a.id == FallbackCatalog::DitherNeed(true, "x").id, L"one id with the product-missing decision");
			Assert::IsTrue(a.reason.find("pipeline_slots_off") != std::string::npos, L"the reason is named");
			Assert::IsTrue(a.reason.find("base amplitude") == std::string::npos, L"does not claim the sigma-delta runs");
			Assert::IsFalse(FallbackCatalog::DitherNeedInactive(false, "x").engaged, L"released when not requested");
			Assert::IsFalse(FallbackCatalog::DitherNeedInactive(true, "").engaged, L"released when the reason clears");
		}

		TEST_METHOD(OffAndOriginalDoNothingAndReportNothing)
		{
			Products p;
			DitherNeedSeam::Stage stage;
			for (int original = 0; original < 2; ++original)
			{
				// Original pathway with the setting on, and the setting off (the default) with every product present or
				// absent: no status, no fallback, no count, no extra
				const DitherNeedSeam::Outcome originalOn = original != 0 ? stage.Evaluate(true, true, p.Feed(), p.grid) : DitherNeedSeam::Outcome();
				const DitherNeedSeam::Outcome offAbsent = stage.Evaluate(false, original != 0, DitherNeedSeam::Feed(), p.grid);
				const DitherNeedSeam::Outcome offPresent = stage.Evaluate(false, original != 0, p.Feed(), p.grid);
				// "off" (absent and present) is asserted on both passes; the Original on-case only when original == 1
				for (const DitherNeedSeam::Outcome* o : { &originalOn, &offAbsent, &offPresent })
				{
					Assert::IsFalse(o->active, L"inactive");
					Assert::IsFalse(o->extraAllowed, L"no extra amplitude");
					Assert::IsTrue(o->status.empty(), L"no status line");
					Assert::IsFalse(o->fallback.engaged, L"no fallback");
					Assert::IsTrue(o->result == DitherNeed::Result::Off, L"result Off");
				}
			}
			DitherNeedSeam::Stage off;
			const DitherNeedSeam::Outcome o = off.Evaluate(false, false, p.Feed(), p.grid);
			Assert::IsFalse(o.active || o.extraAllowed || o.fallback.engaged || !o.status.empty(), L"off with products: nothing");
			const DitherNeedSeam::Counters& c = stage.Count();
			Assert::AreEqual(static_cast<uint64_t>(0), c.framesOn + c.framesNotNeeded + c.framesUnavailable + c.framesStale + c.framesClassicalQuality,
				L"nothing counted when off or Original");
		}

		TEST_METHOD(ProductAbsentIsVisibleAndCounted)
		{
			Products p;
			DitherNeedSeam::Stage stage;
			const DitherNeedSeam::Outcome o = stage.Evaluate(true, false, DitherNeedSeam::Feed(), p.grid);
			Assert::IsTrue(o.active);
			Assert::IsFalse(o.extraAllowed, L"no extra amplitude without products: today's amplitude");
			Assert::IsTrue(o.result == DitherNeed::Result::Unavailable);
			Assert::IsTrue((o.missing & DitherNeed::kNoLut) && (o.missing & DitherNeed::kNoGrain) && (o.missing & DitherNeed::kNoPrecursors),
				L"every absent product is named");
			Assert::IsTrue(o.fallback.engaged && o.fallback.id == "dither need", L"the Ctrl+I / Config fallback line is engaged");
			Assert::IsTrue(o.status.find("unavailable") != std::string::npos && o.status.find("base amplitude") != std::string::npos);
			Assert::AreEqual(static_cast<uint64_t>(1), stage.Count().framesUnavailable, L"counted");
			// it recovers by itself when the products return
			const DitherNeedSeam::Outcome back = stage.Evaluate(true, false, p.Feed(), p.grid);
			Assert::IsTrue(back.result == DitherNeed::Result::On && back.extraAllowed, L"recovers");
			Assert::IsFalse(back.fallback.engaged);
			Assert::IsTrue(back.decision.reset, L"the state restarts after an unavailable frame");
		}

		TEST_METHOD(StaleProductsCountAsMissing)
		{
			Products p;
			DitherNeedSeam::Stage stage;
			DitherNeedSeam::Feed f = p.Feed();
			f.lutAgeSeconds = DitherNeedSeam::kMaxProductAgeSeconds * 2.0;
			const DitherNeedSeam::Outcome o = stage.Evaluate(true, false, f, p.grid);
			Assert::IsTrue(o.missing == DitherNeed::kStaleProduct, L"only the stale bit");
			Assert::IsTrue(o.fallback.engaged && !o.extraAllowed);
			Assert::AreEqual(static_cast<uint64_t>(1), stage.Count().framesStale);
		}

		TEST_METHOD(SourceQualityScalesTheWantedAmplitude)
		{
			Products poor(96, 64, 0.0), clean(96, 64, 1.0);
			DitherNeedSeam::Stage stage;
			const DitherNeed::Params base;
			const DitherNeedSeam::Outcome a = stage.Evaluate(true, false, poor.Feed(), poor.grid, base);
			Assert::IsTrue(a.result == DitherNeed::Result::On);
			Assert::AreEqual(base.aNeed, a.params.aNeed, 1e-12, L"a poor source wants the full designed amplitude");
			DitherNeedSeam::Stage stage2;
			const DitherNeedSeam::Outcome b = stage2.Evaluate(true, false, clean.Feed(), clean.grid, base);
			Assert::IsTrue(b.result == DitherNeed::Result::NotNeeded, L"a clean source needs nothing");
			Assert::IsFalse(b.extraAllowed);
			Products mid(96, 64, 0.5);
			DitherNeedSeam::Stage stage3;
			const DitherNeedSeam::Outcome m = stage3.Evaluate(true, false, mid.Feed(), mid.grid, base);
			Assert::AreEqual(base.aBase + (base.aNeed - base.aBase) * 0.5, m.params.aNeed, 1e-12, L"in between, linear");
			// no quality view at all: the designed amplitude, named and counted (never silent)
			DitherNeedSeam::Feed f = poor.Feed();
			f.quality = nullptr;
			DitherNeedSeam::Stage stage4;
			const DitherNeedSeam::Outcome n = stage4.Evaluate(true, false, f, poor.grid, base);
			Assert::IsTrue(n.result == DitherNeed::Result::On);
			Assert::IsTrue(n.status.find("classical quality") != std::string::npos);
			Assert::AreEqual(static_cast<uint64_t>(1), stage4.Count().framesClassicalQuality);
		}

		TEST_METHOD(UnsupportedDomainIsNamed)
		{
			Products p;
			DitherNeedSeam::Stage stage;
			DitherNeedSeam::Feed f = p.Feed();
			f.domainSupported = false;
			const DitherNeedSeam::Outcome o = stage.Evaluate(true, false, f, p.grid);
			Assert::IsTrue(o.missing == DitherNeed::kUnsupportedDomain && o.fallback.engaged);
		}

		TEST_METHOD(LinkNoiseLimitIsItsOwnAndNotTheGrainRule)
		{
			Assert::AreEqual(1.40, DitherNeedSeam::kLinkNoiseLimitShadow, 0.0);
			Assert::AreEqual(1.05, DitherNeedSeam::kLinkNoiseLimitElsewhere, 0.0);
			Assert::IsTrue(DitherNeedSeam::LinkNoiseWithinLimit(1.40, 0.10), L"shadow block, at the limit");
			Assert::IsFalse(DitherNeedSeam::LinkNoiseWithinLimit(1.41, 0.10));
			Assert::IsTrue(DitherNeedSeam::LinkNoiseWithinLimit(1.05, 0.90), L"elsewhere, at the limit");
			Assert::IsFalse(DitherNeedSeam::LinkNoiseWithinLimit(1.10, 0.90), L"over this limit elsewhere");
		}

		TEST_METHOD(DeterministicAcrossRuns)
		{
			Products p;
			DitherNeedSeam::Stage s1, s2;
			for (int i = 0; i < 3; ++i)
			{
				const DitherNeedSeam::Outcome a = s1.Evaluate(true, false, p.Feed(), p.grid);
				const DitherNeedSeam::Outcome b = s2.Evaluate(true, false, p.Feed(), p.grid);
				Assert::IsTrue(a.status == b.status && a.result == b.result && a.decision.reset == b.decision.reset);
			}
		}
	};
}
