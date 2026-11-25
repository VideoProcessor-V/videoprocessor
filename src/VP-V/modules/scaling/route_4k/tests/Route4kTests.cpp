// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Q2-B2 (scaling.route_4k): adversarial contract verification for Route4k.
//   - Route4kCpuTests (CPU): pipeline chain order & graduation, off / slot-off / Original byte-identical
//     zero-cost bypass, rebased_only vs always_4k routing, derived SourceGeometrySet ratios (no fixed x2),
//     no-double-upscale invariant with same-input unrouted negative control, HybridBlend single-law CPU
//     parity, and visible failure reporting (LastError / Failures / Fallback).
//   - Route4kGpuTests (GpuWarp): BTB/WTW extended-range preservation through hybrid f32 compute blend
//     with same-input [0, 1] UNORM clipping negative control, plus GPU HybridBlend law parity against
//     independent ScalerPass branches and chroma siting preservation.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/ChromaSiting.h>
#include <VP-V/core/PipelineSlots.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>
#include <VP-V/modules/scaling/route_4k/Route4k.h>
#include <VP-V/modules/scaling/shared/HybridBlend.h>
#include <VP-V/modules/scaling/shared/ScalerPass.h>
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Route4kTestsNs
{
	std::wstring Wide(const std::string& s)
	{
		return std::wstring(s.begin(), s.end());
	}

	struct GpuDevice
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_dispatch dispatch = nullptr;

		~GpuDevice()
		{
			if (dispatch) pl_dispatch_destroy(&dispatch);
			if (d3d11) pl_d3d11_destroy(&d3d11);
			if (log) pl_log_destroy(&log);
		}
		bool Create()
		{
			pl_log_params lp{};
			lp.log_level = PL_LOG_WARN;
			log = pl_log_create(PL_API_VER, &lp);
			std::string name;
			const pl_d3d11_params params = ExtendedRangeGpuProbe::TestDeviceParams(&name);
			d3d11 = pl_d3d11_create(log, &params);
			if (!d3d11)
				return false;
			dispatch = pl_dispatch_create(log, d3d11->gpu);
			return dispatch != nullptr;
		}
		pl_gpu Gpu() const { return d3d11 ? d3d11->gpu : nullptr; }
	};

	struct TexGuard
	{
		pl_gpu gpu = nullptr;
		pl_tex tex = nullptr;
		TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
		~TexGuard() { if (tex && gpu) pl_tex_destroy(gpu, &tex); }
		TexGuard(const TexGuard&) = delete;
		TexGuard& operator=(const TexGuard&) = delete;
	};

	struct Route4kGuard
	{
		pl_gpu gpu = nullptr;
		Route4k::Module& mod;
		Route4kGuard(pl_gpu g, Route4k::Module& m) : gpu(g), mod(m) {}
		~Route4kGuard() { mod.Release(gpu); }
		Route4kGuard(const Route4kGuard&) = delete;
		Route4kGuard& operator=(const Route4kGuard&) = delete;
	};

	struct ScalerGuard
	{
		pl_gpu gpu = nullptr;
		ScalerPass::Module& mod;
		ScalerGuard(pl_gpu g, ScalerPass::Module& m) : gpu(g), mod(m) {}
		~ScalerGuard() { mod.Release(gpu); }
		ScalerGuard(const ScalerGuard&) = delete;
		ScalerGuard& operator=(const ScalerGuard&) = delete;
	};

	bool DownloadFloats(pl_gpu gpu, pl_tex tex, std::vector<float>& out)
	{
		if (!gpu || !tex || !tex->params.format)
			return false;
		const pl_fmt fmt = tex->params.format;
		if (fmt->type != PL_FMT_FLOAT || fmt->component_depth[0] != 32 || fmt->num_components != 1)
			return false;
		pl_tex_params rp{};
		rp.w = tex->params.w;
		rp.h = tex->params.h;
		rp.format = fmt;
		rp.host_readable = true;
		rp.blit_dst = true;
		TexGuard readable(gpu, pl_tex_create(gpu, &rp));
		if (!readable.tex)
			return false;
		pl_tex_blit_params bp{};
		bp.src = tex;
		bp.dst = readable.tex;
		pl_tex_blit(gpu, &bp);
		out.assign(static_cast<size_t>(rp.w) * rp.h, 0.0f);
		pl_tex_transfer_params dp{};
		dp.tex = readable.tex;
		dp.ptr = out.data();
		return pl_tex_download(gpu, &dp);
	}

	TEST_CLASS(Route4kCpuTests)
	{
	public:
		TEST_METHOD(ChainOrderAndGraduation)
		{
			const PipelineSlots::Info& route = PipelineSlots::Get(PipelineSlots::Id::ScalingRoute4k);
			const PipelineSlots::Info& rebase = PipelineSlots::Get(PipelineSlots::Id::ScalingRebase);
			const PipelineSlots::Info& restore = PipelineSlots::Get(PipelineSlots::Id::RepairTextureRestore);
			const PipelineSlots::Info& refine = PipelineSlots::Get(PipelineSlots::Id::ScalingRefine);
			const PipelineSlots::Info& output = PipelineSlots::Get(PipelineSlots::Id::ScalingOutput);

			Assert::AreEqual(std::string("scaling.route_4k"), std::string(route.name));
			Assert::IsTrue(route.status == PipelineSlots::Status::Built, L"scaling.route_4k must be graduated to Built");
			Assert::AreEqual(100, route.order, L"scaling.route_4k designed order must be 100");
			Assert::IsTrue(route.origin == PipelineSlots::Origin::Vp, L"scaling.route_4k must be Origin::Vp");
			Assert::IsTrue(route.hook == PipelineSlots::HookPoint::Prepass, L"scaling.route_4k runs at Prepass");
			Assert::IsTrue(route.order > rebase.order && rebase.order == 60,
				L"scaling.route_4k must run strictly after scaling.rebase (60)");
			Assert::IsTrue(route.order > restore.order && restore.order == 90,
				L"scaling.route_4k must run strictly after repair.texture_restore (90)");
			Assert::IsTrue(route.order < refine.order && refine.order == 110,
				L"scaling.route_4k must run strictly before scaling.refine (110)");
			Assert::IsTrue(route.order < output.order && output.order == 160,
				L"scaling.route_4k must run strictly before scaling.output (160)");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(OffSlotOffOriginalByteIdenticalAndZeroGpuCost)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OffSlotOffOriginalByteIdenticalAndZeroGpuCost)
		{
			Route4k::Config cfg;
			cfg.mode = Route4k::Mode::Off;
			const Route4k::PlanResult offPlan = Route4k::Plan(1920, 1080, 3840, 2160, 3840, 2160, true, true, false, cfg);
			Assert::IsFalse(offPlan.shouldRun, L"Mode::Off must not run");

			cfg.mode = Route4k::Mode::Always4k;
			const Route4k::PlanResult slotOffPlan = Route4k::Plan(1920, 1080, 3840, 2160, 3840, 2160, true, false, false, cfg);
			Assert::IsFalse(slotOffPlan.shouldRun, L"!slotRuns must not run");

			const Route4k::PlanResult origPlan = Route4k::Plan(1920, 1080, 3840, 2160, 3840, 2160, true, true, true, cfg);
			Assert::IsFalse(origPlan.shouldRun, L"originalPathway == true must not run");

			// Byte-identical pass-through with zero GPU work (nullptr gpu & dispatch succeed without failure).
			static int dummyY = 0;
			static int dummyUV = 0;
			pl_frame src{};
			src.num_planes = 2;
			src.planes[0].texture = reinterpret_cast<pl_tex>(&dummyY);
			src.planes[0].shift_x = 0.0f;
			src.planes[0].shift_y = 0.0f;
			src.planes[1].texture = reinterpret_cast<pl_tex>(&dummyUV);
			src.planes[1].shift_x = -0.5f;
			src.planes[1].shift_y = 0.25f;
			src.crop = { 12.0f, 8.0f, 1908.0f, 1072.0f };

			Route4k::Module mod;
			for (const Route4k::PlanResult* p : { &offPlan, &slotOffPlan, &origPlan })
			{
				pl_frame out{};
				Assert::IsTrue(mod.Apply(nullptr, nullptr, src, *p, cfg, out),
					L"bypass Apply with !plan.shouldRun must succeed with null GPU/dispatch");
				Assert::AreEqual(src.num_planes, out.num_planes);
				Assert::IsTrue(out.planes[0].texture == src.planes[0].texture, L"plane 0 pointer unchanged");
				Assert::IsTrue(out.planes[1].texture == src.planes[1].texture, L"plane 1 pointer unchanged");
				Assert::AreEqual(src.planes[1].shift_x, out.planes[1].shift_x);
				Assert::AreEqual(src.planes[1].shift_y, out.planes[1].shift_y);
				Assert::AreEqual(src.crop.x0, out.crop.x0);
				Assert::AreEqual(src.crop.y0, out.crop.y0);
				Assert::AreEqual(src.crop.x1, out.crop.x1);
				Assert::AreEqual(src.crop.y1, out.crop.y1);
				Assert::AreEqual(0ull, mod.Failures(), L"bypass must incur zero failures");
				Assert::IsTrue(mod.LastError().empty(), L"bypass must leave LastError empty");
				Assert::IsFalse(mod.Fallback().engaged, L"bypass must not engage Fallback()");
			}
		}

		TEST_METHOD(RebasedOnlyVsAlways4kAndDerivedGeometryRatios)
		{
			// Mode & kernel token parsers + negative controls on invalid tokens.
			bool ok = false;
			Assert::IsTrue(Route4k::ParseMode("off", &ok) == Route4k::Mode::Off && ok);
			Assert::IsTrue(Route4k::ParseMode("rebased_only", &ok) == Route4k::Mode::RebasedOnly && ok);
			Assert::IsTrue(Route4k::ParseMode("always_4k", &ok) == Route4k::Mode::Always4k && ok);
			Assert::IsTrue(Route4k::ParseMode("2x", &ok) == Route4k::Mode::Off && !ok,
				L"negative control: unknown mode token must set ok=false");
			ScalerKernels::Spec parsed;
			Assert::IsTrue(Route4k::ParseKernel("lanczos3", parsed) && parsed.kind == ScalerKernels::Kind::Lanczos && parsed.radius == 3);
			Assert::IsTrue(Route4k::ParseKernel("hermite", parsed) && parsed.kind == ScalerKernels::Kind::Hermite);
			Assert::IsFalse(Route4k::ParseKernel("bogus_kernel", parsed),
				L"negative control: unknown kernel token must return false");

			Route4k::Config rebasedCfg;
			rebasedCfg.mode = Route4k::Mode::RebasedOnly;
			Route4k::Config alwaysCfg;
			alwaysCfg.mode = Route4k::Mode::Always4k;

			// 1. Re-based 1080p on 4K delivered/output: runs in BOTH modes.
			const Route4k::PlanResult rBoth = Route4k::Plan(1920, 1080, 3840, 2160, 3840, 2160, true, true, false, rebasedCfg);
			const Route4k::PlanResult aBoth = Route4k::Plan(1920, 1080, 3840, 2160, 3840, 2160, true, true, false, alwaysCfg);
			Assert::IsTrue(rBoth.shouldRun && !rBoth.oneAxis && rBoth.geometry.dstW == 3840 && rBoth.geometry.dstH == 2160);
			Assert::IsTrue(aBoth.shouldRun && !aBoth.oneAxis && aBoth.geometry.dstW == 3840 && aBoth.geometry.dstH == 2160);

			// 2. Native (non-rebased) 1080p on 4K output: RebasedOnly abstains, Always4k runs.
			const Route4k::PlanResult rNative = Route4k::Plan(1920, 1080, 1920, 1080, 3840, 2160, false, true, false, rebasedCfg);
			const Route4k::PlanResult aNative = Route4k::Plan(1920, 1080, 1920, 1080, 3840, 2160, false, true, false, alwaysCfg);
			Assert::IsFalse(rNative.shouldRun, L"native 1080p must abstain in RebasedOnly");
			Assert::IsTrue(aNative.shouldRun && aNative.geometry.dstW == 3840 && aNative.geometry.dstH == 2160,
				L"native 1080p must run in Always4k");

			// 3. Native 4K (already at/above target): BOTH abstain, never downscales!
			Assert::IsFalse(Route4k::Plan(3840, 2160, 3840, 2160, 3840, 2160, true, true, false, rebasedCfg).shouldRun);
			Assert::IsFalse(Route4k::Plan(3840, 2160, 3840, 2160, 3840, 2160, false, true, false, alwaysCfg).shouldRun);
			Assert::IsFalse(Route4k::Plan(4096, 2160, 3840, 2160, 3840, 2160, false, true, false, alwaysCfg).shouldRun,
				L"4096x2160 on 3840x2160 target must abstain (never downscales)");

			// 4. Non-2x geometries: exact per-axis derived ratios (no fixed-x2 assumption).
			// 720p -> 4K (3x)
			const Route4k::DerivedGeometry d720 = Route4k::DerivedGeometryRatios(1280, 720, 3840, 2160);
			const Route4k::PlanResult p720 = Route4k::Plan(1280, 720, 3840, 2160, 3840, 2160, true, true, false, rebasedCfg);
			Assert::IsTrue(p720.shouldRun && p720.geometry.dstW == 3840 && p720.geometry.dstH == 2160);
			Assert::AreEqual(3.0, d720.ratioX, 1e-9);
			Assert::AreEqual(3.0, d720.ratioY, 1e-9);
			Assert::AreEqual(3.0, p720.geometry.factorX, 1e-9);
			Assert::AreEqual(3.0, p720.geometry.factorY, 1e-9);
			Assert::IsTrue(std::fabs(d720.ratioX - 2.0) > 0.5, L"negative control: 720p->4K is 3x, not fixed 2x");

			// Non-square SD 720x576 -> 3840x2160 (unequal per-axis ratios: 5.333... x 3.75)
			const Route4k::DerivedGeometry dPal = Route4k::DerivedGeometryRatios(720, 576, 3840, 2160);
			const Route4k::PlanResult pPal = Route4k::Plan(720, 576, 3840, 2160, 3840, 2160, true, true, false, rebasedCfg);
			Assert::IsTrue(pPal.shouldRun && pPal.geometry.dstW == 3840 && pPal.geometry.dstH == 2160);
			Assert::AreEqual(3840.0 / 720.0, dPal.ratioX, 1e-9);
			Assert::AreEqual(2160.0 / 576.0, dPal.ratioY, 1e-9);
			Assert::AreEqual(3840.0 / 720.0, pPal.geometry.factorX, 1e-9);
			Assert::AreEqual(2160.0 / 576.0, pPal.geometry.factorY, 1e-9);
			Assert::IsTrue(std::fabs(dPal.ratioX - dPal.ratioY) > 1.0,
				L"non-square SD must keep distinct per-axis ratios (no isotropic/fixed-2x assumption)");

			// 1440p -> 4K (1.5x)
			const Route4k::DerivedGeometry d1440 = Route4k::DerivedGeometryRatios(2560, 1440, 3840, 2160);
			const Route4k::PlanResult p1440 = Route4k::Plan(2560, 1440, 3840, 2160, 3840, 2160, true, true, false, rebasedCfg);
			Assert::IsTrue(p1440.shouldRun && p1440.geometry.dstW == 3840 && p1440.geometry.dstH == 2160);
			Assert::AreEqual(1.5, d1440.ratioX, 1e-9);
			Assert::AreEqual(1.5, d1440.ratioY, 1e-9);
			Assert::AreEqual(1.5, p1440.geometry.factorX, 1e-9);
			Assert::AreEqual(1.5, p1440.geometry.factorY, 1e-9);
			Assert::IsTrue(std::fabs(d1440.ratioX - 2.0) > 0.4, L"negative control: 1440p->4K is 1.5x, not fixed 2x");
		}

		TEST_METHOD(NoDoubleUpscaleInvariantAndUnroutedNegativeControl)
		{
			Route4k::Config cfg;
			cfg.mode = Route4k::Mode::RebasedOnly;
			const Route4k::PlanResult plan = Route4k::Plan(1920, 1080, 3840, 2160, 3840, 2160, true, true, false, cfg);
			Assert::IsTrue(plan.shouldRun && plan.geometry.dstW == 3840 && plan.geometry.dstH == 2160);

			// Routed path: zero downstream upscale (single upscale only).
			Assert::IsFalse(Route4k::WillDownstreamUpscale(plan, 1920, 1080, 3840, 2160),
				L"after Route4k 1080p->4K, WillDownstreamUpscale must be false");
			Assert::IsFalse(AdaptiveDownscale::UpscaleAxesOf(
				static_cast<float>(plan.geometry.dstW), static_cast<float>(plan.geometry.dstH),
				3840.0f, 2160.0f).any,
				L"AdaptiveDownscale::UpscaleAxesOf must see zero upscale after Route4k");

			// Mandatory same-input negative control: naive unrouted 1920x1080 passed downstream to 3840x2160
			// DOES trigger downstream upscale!
			Route4k::Config offCfg;
			offCfg.mode = Route4k::Mode::Off;
			const Route4k::PlanResult offPlan = Route4k::Plan(1920, 1080, 3840, 2160, 3840, 2160, true, true, false, offCfg);
			Assert::IsTrue(Route4k::WillDownstreamUpscale(offPlan, 1920, 1080, 3840, 2160),
				L"negative control: without Route4k, downstream WillDownstreamUpscale triggers");
			Assert::IsTrue(AdaptiveDownscale::UpscaleAxesOf(1920.0f, 1080.0f, 3840.0f, 2160.0f).any,
				L"negative control: unrouted 1920x1080 triggers AdaptiveDownscale::UpscaleAxesOf");
		}

		TEST_METHOD(HybridBlendSingleLawParityAndFailureVisibility)
		{
			const float probes[] = { -0.0625f, 0.0f, 0.5f, 1.0f, 1.09375f };
			const float weights[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
			for (float s : probes)
				for (float f : probes)
					for (float wb : weights)
						for (float e : { 0.0f, 0.4f, 1.0f })
							for (float g : { 0.4f, 1.0f })
								for (float m : { 0.25f, 1.0f })
								{
									HybridBlend::Terms t;
									t.wBase = wb;
									t.edge = e;
									t.grain = g;
									t.motion = m;
									const float direct = HybridBlend::BlendLuma(s, f, HybridBlend::Weight(t));
									const float viaRoute = Route4k::HybridBlendLumaReference(s, f, wb, e, g, m);
									Assert::IsTrue(viaRoute == direct,
										L"Route4k::HybridBlendLumaReference must equal HybridBlend bit-for-bit");
								}

			// Visible failure reporting when plan.shouldRun == true and gpu == nullptr.
			Route4k::Config cfg;
			cfg.mode = Route4k::Mode::Always4k;
			const Route4k::PlanResult activePlan = Route4k::Plan(1920, 1080, 3840, 2160, 3840, 2160, true, true, false, cfg);
			Assert::IsTrue(activePlan.shouldRun);

			pl_frame src{};
			pl_frame out{};
			out.crop = { 7.0f, 11.0f, 13.0f, 17.0f };
			Route4k::Module mod;
			Assert::IsFalse(mod.Apply(nullptr, nullptr, src, activePlan, cfg, out),
				L"Apply with null GPU on active plan must fail");
			Assert::AreEqual(7.0f, out.crop.x0, L"failed Apply must leave out untouched");
			Assert::AreEqual(17.0f, out.crop.y1, L"failed Apply must leave out untouched");
			Assert::AreEqual(1ull, mod.Failures(), L"failed Apply must increment Failures()");
			Assert::IsFalse(mod.LastError().empty(), L"failed Apply must populate LastError()");
			const FallbackCatalog::Decision fb = mod.Fallback();
			Assert::IsTrue(fb.engaged, L"failed Apply must engage Fallback()");
			Assert::AreEqual(std::string("4K route"), fb.id, L"Fallback id must be '4K route'");
			Assert::IsTrue(fb.reason.rfind("4K route: off (", 0) == 0,
				(L"Fallback reason must start with '4K route: off (', got: " + Wide(fb.reason)).c_str());
		}
	};

	TEST_CLASS(Route4kGpuTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(BtbWtwExtendedRangePreservedOnGpuWithUnormNegativeControl)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BtbWtwExtendedRangePreservedOnGpuWithUnormNegativeControl)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create libplacebo WARP test device");
			pl_gpu gpu = dev.Gpu();

			const int sw = 64, sh = 48;
			const int dw = 128, dh = 96;
			const float kBtb = -0.0625f;
			const float kMid = 0.50f;
			const float kWtw = 1.09375f;

			std::vector<float> srcPixels(static_cast<size_t>(sw) * sh, kMid);
			for (int y = 0; y < sh; ++y)
			{
				const float val = (y < 16) ? kBtb : (y < 32 ? kMid : kWtw);
				for (int x = 0; x < sw; ++x)
					srcPixels[static_cast<size_t>(y) * sw + x] = val;
			}

			// Mandatory in-test negative control on the exact same input: [0, 1] UNORM clamp clips BTB & WTW!
			const float clampedBtb = (std::min)(1.0f, (std::max)(0.0f, srcPixels[static_cast<size_t>(8) * sw + 32]));
			const float clampedWtw = (std::min)(1.0f, (std::max)(0.0f, srcPixels[static_cast<size_t>(40) * sw + 32]));
			Assert::IsTrue(std::fabs(clampedBtb - kBtb) >= 0.06f,
				L"negative control: [0,1] UNORM clamp clips BTB -0.0625 to 0.0 (error >= 0.06)");
			Assert::IsTrue(std::fabs(clampedWtw - kWtw) >= 0.09f,
				L"negative control: [0,1] UNORM clamp clips WTW +1.09375 to 1.0 (error >= 0.09)");

			pl_fmt r32f = pl_find_named_fmt(gpu, "r32f");
			Assert::IsNotNull(r32f, L"r32f format required");
			pl_tex_params sp{};
			sp.w = sw;
			sp.h = sh;
			sp.format = r32f;
			sp.sampleable = true;
			sp.initial_data = srcPixels.data();
			TexGuard srcTex(gpu, pl_tex_create(gpu, &sp));
			Assert::IsNotNull(srcTex.tex, L"source r32f upload failed");

			pl_frame srcFrame{};
			srcFrame.num_planes = 1;
			srcFrame.planes[0].texture = srcTex.tex;
			srcFrame.planes[0].components = 1;
			srcFrame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			srcFrame.crop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };

			Route4k::Config cfg;
			cfg.mode = Route4k::Mode::Always4k;
			cfg.wBase = 0.5f;
			Assert::IsTrue(Route4k::ParseKernel("lanczos3", cfg.sharpFilter));
			Assert::IsTrue(Route4k::ParseKernel("hermite", cfg.safeFilter));
			cfg.sharpAntiring = 0.5f;
			cfg.safeAntiring = 0.5f;
			cfg.preferFloat32 = true;

			const Route4k::PlanResult plan = Route4k::Plan(sw, sh, dw, dh, dw, dh, true, true, false, cfg);
			Assert::IsTrue(plan.shouldRun && plan.geometry.dstW == dw && plan.geometry.dstH == dh);

			Route4k::Module mod;
			Route4kGuard modGuard(gpu, mod);
			pl_frame outFrame{};
			Assert::IsTrue(mod.Apply(gpu, dev.dispatch, srcFrame, plan, cfg, outFrame, 0),
				Wide("Route4k::Apply failed: " + mod.LastError()).c_str());
			Assert::AreEqual(0ull, mod.Failures());
			Assert::IsFalse(mod.Fallback().engaged);
			Assert::IsNotNull(outFrame.planes[0].texture);
			Assert::AreEqual(dw, outFrame.planes[0].texture->params.w);
			Assert::AreEqual(dh, outFrame.planes[0].texture->params.h);

			std::vector<float> got;
			Assert::IsTrue(DownloadFloats(gpu, outFrame.planes[0].texture, got), L"r32f download failed");

			// Interior of each band (margin >= 8 dest rows = 4 source rows > Lanczos3 radius 3).
			const float kTol = 1e-4f;
			for (int y = 8; y < 24; ++y)
				for (int x = 8; x < dw - 8; ++x)
				{
					const float v = got[static_cast<size_t>(y) * dw + x];
					Assert::IsTrue(v < -0.05f && std::fabs(v - kBtb) <= kTol,
						(L"BTB clipped or distorted at (" + std::to_wstring(x) + L"," + std::to_wstring(y) + L"): " + std::to_wstring(v)).c_str());
				}
			for (int y = 40; y < 56; ++y)
				for (int x = 8; x < dw - 8; ++x)
				{
					const float v = got[static_cast<size_t>(y) * dw + x];
					Assert::IsTrue(std::fabs(v - kMid) <= kTol,
						(L"mid-range distorted at (" + std::to_wstring(x) + L"," + std::to_wstring(y) + L"): " + std::to_wstring(v)).c_str());
				}
			for (int y = 72; y < 88; ++y)
				for (int x = 8; x < dw - 8; ++x)
				{
					const float v = got[static_cast<size_t>(y) * dw + x];
					Assert::IsTrue(v > 1.08f && std::fabs(v - kWtw) <= kTol,
						(L"WTW clipped or distorted at (" + std::to_wstring(x) + L"," + std::to_wstring(y) + L"): " + std::to_wstring(v)).c_str());
				}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(HybridBlendGpuMatchesIndependentScalerPassBranchesAndSiting)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(HybridBlendGpuMatchesIndependentScalerPassBranchesAndSiting)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create libplacebo WARP test device");
			pl_gpu gpu = dev.Gpu();

			const int sw = 64, sh = 48;
			const int dw = 128, dh = 96;
			std::vector<uint16_t> lumaRaw(static_cast<size_t>(sw) * sh);
			for (int y = 0; y < sh; ++y)
				for (int x = 0; x < sw; ++x)
				{
					const bool hi = ((x / 5 + y / 4) & 1) != 0;
					const double v = hi ? 0.85 : 0.15;
					lumaRaw[static_cast<size_t>(y) * sw + x] = static_cast<uint16_t>(std::lround(v * 65535.0));
				}
			std::vector<uint16_t> chromaRaw(static_cast<size_t>(sw / 2) * (sh / 2) * 2, 32768u);

			pl_tex_params lp{};
			lp.w = sw; lp.h = sh; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.initial_data = lumaRaw.data();
			TexGuard lumaTex(gpu, pl_tex_create(gpu, &lp));
			Assert::IsNotNull(lumaTex.tex, L"luma r16 upload failed");

			pl_tex_params cp{};
			cp.w = sw / 2; cp.h = sh / 2; cp.format = pl_find_named_fmt(gpu, "rg16"); cp.sampleable = true; cp.initial_data = chromaRaw.data();
			TexGuard chromaTex(gpu, pl_tex_create(gpu, &cp));
			Assert::IsNotNull(chromaTex.tex, L"chroma rg16 upload failed");

			pl_frame src{};
			src.num_planes = 2;
			src.planes[0].texture = lumaTex.tex;
			src.planes[0].components = 1;
			src.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			src.planes[0].shift_x = 0.0f;
			src.planes[0].shift_y = 0.0f;
			src.planes[1].texture = chromaTex.tex;
			src.planes[1].components = 2;
			src.planes[1].component_mapping[0] = PL_CHANNEL_CB;
			src.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			src.planes[1].shift_x = -0.5f;
			src.planes[1].shift_y = 0.0f;
			src.crop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };

			Route4k::Config cfg;
			cfg.mode = Route4k::Mode::RebasedOnly;
			cfg.wBase = 0.5f;
			Assert::IsTrue(Route4k::ParseKernel("lanczos3", cfg.sharpFilter));
			Assert::IsTrue(Route4k::ParseKernel("hermite", cfg.safeFilter));
			cfg.sharpAntiring = 0.5f;
			cfg.safeAntiring = 0.5f;
			cfg.preferFloat32 = true;

			const Route4k::PlanResult plan = Route4k::Plan(sw, sh, dw, dh, dw, dh, true, true, false, cfg);
			Assert::IsTrue(plan.shouldRun);

			Route4k::Module mod;
			Route4kGuard modGuard(gpu, mod);
			pl_frame out{};
			Assert::IsTrue(mod.Apply(gpu, dev.dispatch, src, plan, cfg, out, 0),
				Wide("Route4k::Apply failed: " + mod.LastError()).c_str());

			// Independent ground truth: run ScalerPass sharp (lanczos3) and safe (hermite) separately
			// and blend via CPU HybridBlend::BlendLuma(s, f, 0.5f).
			ScalerPass::Module sharpRef, safeRef;
			ScalerGuard sharpGuard(gpu, sharpRef), safeGuard(gpu, safeRef);
			const ScalerPass::Source spSrc = { lumaTex.tex, 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
			ScalerPass::Settings sharpSt;
			sharpSt.filter = cfg.sharpFilter;
			sharpSt.axis = ScalerPass::Axis::Both;
			sharpSt.plane = ScalerPass::Plane::Luma;
			sharpSt.precision = ScalerPass::Precision::F32;
			sharpSt.order = ScalerPass::Order::AcrossFirst;
			sharpSt.antiring = cfg.sharpAntiring;
			pl_tex sharpOut = nullptr;
			Assert::IsTrue(sharpRef.Apply(gpu, dev.dispatch, spSrc, dw, dh, sharpSt, sharpOut, 0));

			ScalerPass::Settings safeSt = sharpSt;
			safeSt.filter = cfg.safeFilter;
			safeSt.antiring = cfg.safeAntiring;
			pl_tex safeOut = nullptr;
			Assert::IsTrue(safeRef.Apply(gpu, dev.dispatch, spSrc, dw, dh, safeSt, safeOut, 0));

			std::vector<float> gotBlend, refSharp, refSafe;
			Assert::IsTrue(DownloadFloats(gpu, out.planes[0].texture, gotBlend));
			Assert::IsTrue(DownloadFloats(gpu, sharpOut, refSharp));
			Assert::IsTrue(DownloadFloats(gpu, safeOut, refSafe));
			Assert::AreEqual(refSharp.size(), gotBlend.size());

			float maxBranchDiff = 0.0f;
			float maxBlendErr = 0.0f;
			for (size_t i = 0; i < gotBlend.size(); ++i)
			{
				maxBranchDiff = (std::max)(maxBranchDiff, std::fabs(refSharp[i] - refSafe[i]));
				const float expected = HybridBlend::BlendLuma(refSharp[i], refSafe[i], 0.5f);
				maxBlendErr = (std::max)(maxBlendErr, std::fabs(gotBlend[i] - expected));
			}
			// Negative control: sharp (lanczos3) and safe (hermite) branches must genuinely differ on edges!
			Assert::IsTrue(maxBranchDiff > 0.01f,
				(L"negative control: sharp vs safe branches must differ on sharp edges, maxBranchDiff=" + std::to_wstring(maxBranchDiff)).c_str());
			// GPU compute blend must match CPU HybridBlend law within float32 rounding (1e-5).
			Assert::IsTrue(maxBlendErr <= 1e-5f,
				(L"Route4k GPU blend diverged from HybridBlend CPU law: maxErr=" + std::to_wstring(maxBlendErr)).c_str());

			// Chroma siting shift and crop updated via ChromaSiting::ShiftAfterResample.
			Assert::AreEqual(ChromaSiting::ShiftAfterResample(src.planes[1].shift_x, 2.0), out.planes[1].shift_x, 1e-6f);
			Assert::AreEqual(ChromaSiting::ShiftAfterResample(src.planes[1].shift_y, 2.0), out.planes[1].shift_y, 1e-6f);
			Assert::AreEqual(static_cast<float>(dw), out.crop.x1, 1e-5f);
			Assert::AreEqual(static_cast<float>(dh), out.crop.y1, 1e-5f);
		}
	};
}
