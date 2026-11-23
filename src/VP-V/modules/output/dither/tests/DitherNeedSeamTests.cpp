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
#include <VP-V/modules/output/dither/DitherNeedGpu.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
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

		// W23-4p (G-T1) structural pin: BlockPass::Step/Release and HookGuard::Apply/Release must have
		// genuine non-comment production callers in src/VP-V/segments/ (not tests/ or commented-out dead code).
		TEST_METHOD(ProductionCodeCallsBlockPassAndHookGuard)
		{
			auto stripComments = [](const std::string& src) -> std::string
			{
				std::string out;
				out.reserve(src.size());
				bool inLine = false, inBlock = false, inStr = false, inChar = false;
				for (size_t i = 0; i < src.size(); ++i)
				{
					const char c = src[i];
					const char next = (i + 1 < src.size()) ? src[i + 1] : '\0';
					if (inLine)
					{
						if (c == '\n') { inLine = false; out.push_back(c); }
						continue;
					}
					if (inBlock)
					{
						if (c == '*' && next == '/') { inBlock = false; ++i; }
						continue;
					}
					if (inStr)
					{
						out.push_back(c);
						if (c == '\\' && next != '\0') { out.push_back(next); ++i; }
						else if (c == '"') inStr = false;
						continue;
					}
					if (inChar)
					{
						out.push_back(c);
						if (c == '\\' && next != '\0') { out.push_back(next); ++i; }
						else if (c == '\'') inChar = false;
						continue;
					}
					if (c == '/' && next == '/') { inLine = true; ++i; continue; }
					if (c == '/' && next == '*') { inBlock = true; ++i; continue; }
					if (c == '"') { inStr = true; out.push_back(c); continue; }
					if (c == '\'') { inChar = true; out.push_back(c); continue; }
					out.push_back(c);
				}
				return out;
			};
			auto hasLiveCallers = [&](const std::string& text)
			{
				const std::string code = stripComments(text);
				return code.find("DitherNeed::Gpu::BlockPass") != std::string::npos &&
					code.find(".Step(") != std::string::npos &&
					code.find(".Release(") != std::string::npos &&
					code.find("DitherNeed::Gpu::HookGuard") != std::string::npos &&
					code.find(".Apply(") != std::string::npos;
			};
			// In-test negative control: commented-out references MUST NOT satisfy the structural pin.
			const std::string commentedOnly =
				"// DitherNeed::Gpu::BlockPass pass; pass.Step(g, d, a, fa); pass.Release(g);\n"
				"/* DitherNeed::Gpu::HookGuard guard; guard.Apply(h, n, t, p); guard.Release(); */\n"
				"void NoOp() {}\n";
			Assert::IsFalse(hasLiveCallers(commentedOnly),
				L"Negative control failed: commented-out dead code must not satisfy production-caller pin");

			// Locate repo root from this test file: src/VP-V/modules/output/dither/tests/DitherNeedSeamTests.cpp
			namespace fs = std::filesystem;
			fs::path dir = fs::path(__FILE__).parent_path();
			fs::path root;
			for (int up = 0; up < 10; ++up)
			{
				if (fs::exists(dir / "src" / "VP-V" / "segments" / "OutputSegments.cpp"))
				{
					root = dir;
					break;
				}
				if (!dir.has_parent_path() || dir == dir.parent_path()) break;
				dir = dir.parent_path();
			}
			Assert::IsFalse(root.empty(), L"Could not locate repository root containing src/VP-V/segments/OutputSegments.cpp");

			auto readFile = [&](const fs::path& rel) -> std::string
			{
				std::ifstream in(root / rel, std::ios::binary);
				Assert::IsTrue(in.good(), (L"Failed to open production file: " + rel.wstring()).c_str());
				std::ostringstream ss;
				ss << in.rdbuf();
				return ss.str();
			};
			const std::string outputSegCpp = readFile(fs::path("src") / "VP-V" / "segments" / "OutputSegments.cpp");
			const std::string coreSegInl = readFile(fs::path("src") / "VP-V" / "segments" / "CoreSegments.inl");
			const std::string vectorsH = readFile(fs::path("src") / "VP-V" / "modules" / "output" / "dither" / "SigmaDeltaVectors.h");

			Assert::IsTrue(hasLiveCallers(outputSegCpp),
				L"OutputSegments.cpp must contain live non-comment DitherNeed::Gpu::BlockPass::Step/Release and HookGuard::Apply/Release callers");
			const std::string coreCode = stripComments(coreSegInl);
			Assert::IsTrue(coreCode.find("RunDitherNeedGpu(") != std::string::npos,
				L"CoreSegments.inl must invoke RunDitherNeedGpu (not default empty Feed)");
			const std::string vecCode = stripComments(vectorsH);
			Assert::IsTrue(vecCode.find("AttachRelease(") != std::string::npos,
				L"SigmaDeltaVectors.h NextVectorGuard must provide AttachRelease for RAII post-render HookGuard unbind");
		}

		// GEM-224 adversarial test: NLS active visibly declines dither_need with kNonLinearStretch, increments
		// framesNlsDeclined, engages FallbackCatalog::DitherNeed, and recovers automatically when NLS clears.
		// In-test negative control proves naive affine placement produces > 1 block coordinate error under NLS.
		TEST_METHOD(NlsGuardDeclinesVisiblyRecoversAndProvesAffineMisalignmentHazard)
		{
			// Negative control ("Prove the Hazard Is Real"): under a non-linear stretch warp fixing endpoints
			// (u = 0, 0.5, 1) while stretching edges (x_src = W * (u + k*u*(1-u)*(1-2u)), k = 0.25),
			// naive affine PlacementFromCrops misaligns mid-frame (u = 0.25) by > 1 full 16px block.
			const float W = 1920.0f, H = 1080.0f;
			const int B = 16;
			DitherNeed::Gpu::Placement affine;
			Assert::IsTrue(DitherNeed::Gpu::PlacementFromCrops(0.0f, 0.0f, W, H, 0.0f, 0.0f, W, H, B, affine));
			const float u = 0.25f;
			const float posX = u * W - 0.5f;   // output pixel whose centre sits at u = 0.25
			const float affineSrcX = (posX + 0.5f) * affine.scaleX + affine.offX;
			const float nlsStretchK = 0.25f;
			const float trueNlsSrcX = W * (u + nlsStretchK * u * (1.0f - u) * (1.0f - 2.0f * u));
			const float misalignmentPx = std::fabs(affineSrcX - trueNlsSrcX);
			Assert::IsTrue(misalignmentPx > static_cast<float>(B),
				L"Negative control failed: naive affine placement must exhibit > 1 block error under non-linear stretch");

			// Guarded path: DitherNeedSeam::Stage declines visibly under nlsActive and recovers on clear.
			Products p;
			DitherNeedSeam::Stage stage;
			const DitherNeedSeam::Outcome healthy = stage.Evaluate(true, false, p.Feed(), p.grid);
			Assert::IsTrue(healthy.result == DitherNeed::Result::On && healthy.extraAllowed);
			Assert::AreEqual(static_cast<uint64_t>(0), stage.Count().framesNlsDeclined);

			DitherNeedSeam::Feed nls = p.Feed();
			nls.nlsActive = true;
			const DitherNeedSeam::Outcome declined = stage.Evaluate(true, false, nls, p.grid);
			Assert::IsTrue(declined.active);
			Assert::IsFalse(declined.extraAllowed, L"NLS active must forbid extra dither binding");
			Assert::IsTrue(declined.result == DitherNeed::Result::Unavailable);
			Assert::AreEqual(static_cast<unsigned>(DitherNeed::kNonLinearStretch), declined.missing);
			Assert::IsTrue(declined.fallback.engaged && declined.fallback.id == "dither need");
			Assert::IsTrue(declined.fallback.reason.find("non-linear stretch active") != std::string::npos);
			Assert::IsTrue(declined.status.find("non-linear stretch active") != std::string::npos);
			Assert::AreEqual(static_cast<uint64_t>(1), stage.Count().framesNlsDeclined);
			Assert::AreEqual(static_cast<uint64_t>(1), stage.Count().framesUnavailable);

			// Recovery when NLS turns off: reset == true, fallback released, On restored.
			const DitherNeedSeam::Outcome recovered = stage.Evaluate(true, false, p.Feed(), p.grid);
			Assert::IsTrue(recovered.result == DitherNeed::Result::On && recovered.extraAllowed);
			Assert::IsTrue(recovered.decision.reset, L"recovery after NLS decline must reset temporal state");
			Assert::IsFalse(recovered.fallback.engaged, L"fallback must release once NLS clears");
			Assert::AreEqual(static_cast<uint64_t>(2), stage.Count().framesOn);
			Assert::AreEqual(static_cast<uint64_t>(1), stage.Count().framesNlsDeclined);
		}

		// PlacementFromCrops: exact affine mapping on valid letterboxed/scaled crops, and strict fail-closed
		// rejection of every degenerate or non-finite input (zero/negative dst or src span, bad blockPx, NaN/Inf).
		TEST_METHOD(PlacementFromCropsDerivedExactAndRejectsDegenerateInputs)
		{
			DitherNeed::Gpu::Placement pl;
			// Cropped source [100..1820] x [40..1040] rendered into letterboxed target [160..3680] x [120..2040].
			Assert::IsTrue(DitherNeed::Gpu::PlacementFromCrops(
				100.0f, 40.0f, 1820.0f, 1040.0f, 160.0f, 120.0f, 3680.0f, 2040.0f, 32, pl));
			Assert::AreEqual(32, pl.blockPx);
			// Independent analytical check at target top-left edge, centre, and bottom-right edge:
			auto mapX = [&](float dstPos) { return (dstPos + 0.5f) * pl.scaleX + pl.offX; };
			auto mapY = [&](float dstPos) { return (dstPos + 0.5f) * pl.scaleY + pl.offY; };
			Assert::AreEqual(100.0, static_cast<double>(mapX(159.5f)), 1e-3, L"target left edge maps to srcX0");
			Assert::AreEqual(1820.0, static_cast<double>(mapX(3679.5f)), 1e-3, L"target right edge maps to srcX1");
			Assert::AreEqual(960.0, static_cast<double>(mapX(1919.5f)), 1e-3, L"target centre maps to src centre");
			Assert::AreEqual(40.0, static_cast<double>(mapY(119.5f)), 1e-3, L"target top edge maps to srcY0");
			Assert::AreEqual(1040.0, static_cast<double>(mapY(2039.5f)), 1e-3, L"target bottom edge maps to srcY1");
			// In-test negative control: naive zero-offset scaling (offX = 0) misses target left edge by > 10 px.
			const float naiveLeftX = (159.5f + 0.5f) * pl.scaleX;
			Assert::IsTrue(std::fabs(naiveLeftX - 100.0f) > 10.0f,
				L"Negative control failed: ignoring crop offset must produce significant misalignment");

			// Degenerate / adversarial cases MUST fail closed (return false):
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const float inf = std::numeric_limits<float>::infinity();
			DitherNeed::Gpu::Placement bad;
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(0, 0, 1920, 1080, 100, 0, 100, 1080, 16, bad), L"zero dstW");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(0, 0, 1920, 1080, 200, 0, 100, 1080, 16, bad), L"negative dstW");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(0, 0, 1920, 1080, 0, 50, 1920, 50, 16, bad), L"zero dstH");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(0, 0, 1920, 1080, 0, 100, 1920, 50, 16, bad), L"negative dstH");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(100, 0, 100, 1080, 0, 0, 1920, 1080, 16, bad), L"zero srcW");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(200, 0, 100, 1080, 0, 0, 1920, 1080, 16, bad), L"negative srcW");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(0, 50, 1920, 50, 0, 0, 1920, 1080, 16, bad), L"zero srcH");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(0, 100, 1920, 50, 0, 0, 1920, 1080, 16, bad), L"negative srcH");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(0, 0, 1920, 1080, 0, 0, 1920, 1080, 0, bad), L"zero blockPx");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(0, 0, 1920, 1080, 0, 0, 1920, 1080, -16, bad), L"negative blockPx");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(nan, 0, 1920, 1080, 0, 0, 1920, 1080, 16, bad), L"NaN srcX0");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(0, 0, 1920, 1080, 0, 0, inf, 1080, 16, bad), L"+Inf dstX1");
			Assert::IsFalse(DitherNeed::Gpu::PlacementFromCrops(0, -inf, 1920, 1080, 0, 0, 1920, 1080, 16, bad), L"-Inf srcY0");
		}

		// Live feed staleness across every product age field (grain, evidence, lut), exact boundary, unknown age,
		// automatic reset on recovery, and RecordGpuFailure rollback of On/ClassicalQuality counters.
		TEST_METHOD(LiveFeedStaleAgesBoundaryAndRecordGpuFailureRollback)
		{
			Products p;
			DitherNeedSeam::Stage stage;
			// Exact boundary (== kMaxProductAgeSeconds) and unknown negative age (-1.0) stay fresh.
			{
				DitherNeedSeam::Feed atLimit = p.Feed();
				atLimit.grainAgeSeconds = DitherNeedSeam::kMaxProductAgeSeconds;
				atLimit.evidenceAgeSeconds = -1.0;
				atLimit.lutAgeSeconds = DitherNeedSeam::kMaxProductAgeSeconds;
				const DitherNeedSeam::Outcome o = stage.Evaluate(true, false, atLimit, p.grid);
				Assert::IsTrue(o.result == DitherNeed::Result::On && o.extraAllowed && !o.fallback.engaged,
					L"age == kMaxProductAgeSeconds and negative unknown age must stay fresh");
			}
			// Each product age field exceeding kMaxProductAgeSeconds individually triggers kStaleProduct + recovery.
			for (int which = 0; which < 3; ++which)
			{
				DitherNeedSeam::Feed stale = p.Feed();
				const double over = DitherNeedSeam::kMaxProductAgeSeconds + 1e-3;
				if (which == 0) stale.grainAgeSeconds = over;
				else if (which == 1) stale.evidenceAgeSeconds = over;
				else stale.lutAgeSeconds = over;
				const DitherNeedSeam::Outcome o = stage.Evaluate(true, false, stale, p.grid);
				Assert::IsTrue(o.result == DitherNeed::Result::Unavailable && !o.extraAllowed);
				Assert::AreEqual(static_cast<unsigned>(DitherNeed::kStaleProduct), o.missing);
				Assert::IsTrue(o.fallback.engaged && o.fallback.reason.find("stale") != std::string::npos);
				const DitherNeedSeam::Outcome rec = stage.Evaluate(true, false, p.Feed(), p.grid);
				Assert::IsTrue(rec.result == DitherNeed::Result::On && rec.decision.reset && !rec.fallback.engaged,
					L"stale frame must recover with reset == true");
			}
			Assert::AreEqual(static_cast<uint64_t>(3), stage.Count().framesStale);

			// RecordGpuFailure rollback: an On + classicalQuality outcome converted to Unavailable rolls back counters.
			DitherNeedSeam::Stage gpuStage;
			DitherNeedSeam::Feed classical = p.Feed();
			classical.quality = nullptr;
			DitherNeedSeam::Outcome o = gpuStage.Evaluate(true, false, classical, p.grid);
			Assert::IsTrue(o.result == DitherNeed::Result::On && o.classicalQuality);
			Assert::AreEqual(static_cast<uint64_t>(1), gpuStage.Count().framesOn);
			Assert::AreEqual(static_cast<uint64_t>(1), gpuStage.Count().framesClassicalQuality);
			gpuStage.RecordGpuFailure(o, DitherNeed::kNoSetTexture);
			Assert::IsTrue(o.result == DitherNeed::Result::Unavailable && !o.extraAllowed && !o.classicalQuality);
			Assert::IsTrue((o.missing & DitherNeed::kNoSetTexture) != 0);
			Assert::IsTrue(o.fallback.engaged && o.fallback.reason.find("set_texture") != std::string::npos);
			Assert::AreEqual(static_cast<uint64_t>(0), gpuStage.Count().framesOn, L"framesOn rolled back");
			Assert::AreEqual(static_cast<uint64_t>(0), gpuStage.Count().framesClassicalQuality, L"framesClassicalQuality rolled back");
			Assert::AreEqual(static_cast<uint64_t>(1), gpuStage.Count().framesUnavailable);
			// Next healthy frame resets tracker after GPU failure.
			const DitherNeedSeam::Outcome afterFail = gpuStage.Evaluate(true, false, p.Feed(), p.grid);
			Assert::IsTrue(afterFail.result == DitherNeed::Result::On && afterFail.decision.reset);
		}
	};
}
