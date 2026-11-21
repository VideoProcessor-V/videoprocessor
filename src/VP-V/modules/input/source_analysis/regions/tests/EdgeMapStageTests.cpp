// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// CB slice 18 S2b (coreplan-CB-18-edgemap.md S2b; lead14/design-EDGEMAP-DOMAIN-AXIS.md rev 2 s2a): the kEdgeMap STAGE VARIANT
// (EdgeMapFill.h FillEdgeMapStage) on the CPU tier, no GPU object touched (textures are plain structs, the device is a test double
// that makes no texture):
//   1. the law: R = Sobel |G| of the measure luma (BusyEdge::SobelMagnitude on VpMeasure::MeasureLumaCode codes) equals X5's former
//      private Sobel (AdaptiveDownscale MEASURE_BODY, transcribed here) times 4 exactly, and G = the measure luma, so X5's
//      Laplacian and energy read from G are its own; for the SDR, linear and HDR measure domains and BT.709 / BT.2020 weights;
//      taps clamp to the WHOLE picture (a crop border reads beyond the crop, as X5 did);
//   2. the fill: its axes and defaults, the stage key (code unit 8 kept by canonicalisation), the reserve (8 B/px at the source
//      luma size, twice the declared default), the RG32F texture at the stage picture's own size and ordinal;
//   3. the visible declines: code unit, no size, a picture not offered (P13's cause), a size without a stage, above the reserve,
//      a refused device, the crop axis undeclared.
// The GPU parity (fill vs this reference, GPU-QUIET) is the separate EdgeMapStageGpuParityTests (102b EDGEMAP-GPU-PARITY-TEST).

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdge.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdgeGlsl.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapFill.h>
#include <VP-V/modules/input/source_analysis/regions/MeasureDomain.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace EdgeMapStageTestsNs
{
	using namespace VpSkeleton;
	using Vpv::Capture::SourcePlanes;
	using Vpv::Capture::StagePicture;

	bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

	const uint8_t kStage = 5;   // any non-zero stage id (X5 offers StageAfter(the PipelineSlots id))

	// Non-null opaque handles (the fill only tests them before acquiring its input); makes no texture.
	struct RefusingDevice : IOwnerGpuDevice
	{
		int gpuTag = 0;
		int dispatchTag = 0;
		int asked = 0;
		TexShape lastShape;
		void* OwnerGpu() const override { return const_cast<int*>(&gpuTag); }
		void* OwnerDispatch() const override { return const_cast<int*>(&dispatchTag); }
		bool CreateOwnerTexture(const TexShape& shape, GpuMemoHandle&, std::string& why) override
		{
			++asked;
			lastShape = shape;
			why = "test device makes no texture";
			return false;
		}
	};

	Dims DimsOf(uint32_t w, uint32_t h)
	{
		Dims d;
		d.width = w;
		d.height = h;
		return d;
	}

	StagePicture Picture(uint64_t seq, uint16_t ordinal, pl_tex tex)
	{
		StagePicture p;
		p.seq = seq;
		p.stage = kStage;
		p.ordinal = ordinal;
		p.tex = tex;
		p.color.primaries = PL_COLOR_PRIM_BT_709;
		p.color.transfer = PL_COLOR_TRC_BT_1886;
		p.system = PL_COLOR_SYSTEM_BT_709;
		p.primaries = PL_COLOR_PRIM_BT_709;
		p.origin = "test stage";
		return p;
	}

	// A deterministic RGB picture in [0, 1] (row-major, 3 floats per pixel) with an edge, a ramp and noise.
	std::vector<double> TestPicture(int w, int h)
	{
		std::vector<double> rgb(static_cast<size_t>(w) * h * 3);
		uint32_t s = 12345u;
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				for (int c = 0; c < 3; ++c)
				{
					s = s * 1664525u + 1013904223u;
					const double noise = double(s >> 8) / double(1u << 24) * 0.05;
					const double base = (x > w / 2 ? 0.7 : 0.2) + 0.002 * y + 0.1 * c;
					rgb[(static_cast<size_t>(y) * w + x) * 3 + c] = (std::min)(1.0, base + noise);
				}
		return rgb;
	}

	// The stage variant's G: the measure luma of every pixel, as float (the shader's precision).
	std::vector<float> MeasureCodes(const VpMeasure::MeasureDomain& d, const std::vector<double>& rgb, int w, int h)
	{
		std::vector<float> codes(static_cast<size_t>(w) * h);
		for (size_t i = 0; i < codes.size(); ++i)
			codes[i] = static_cast<float>(VpMeasure::MeasureLumaCode(d, &rgb[i * 3]));
		return codes;
	}

	// X5's FORMER private measure (AdaptiveDownscale MEASURE_BODY before S3), transcribed: at region pixel q of a crop at
	// (ox, oy), the 9 taps ad_luma(origin + q + offset) clamp to the WHOLE picture; grad = length(gx, gy) * 0.25; lap = |4e - b - d
	// - f - h|; e = the centre luma.
	struct X5Taps
	{
		float grad = 0.0f, lap = 0.0f, e = 0.0f;
	};
	X5Taps X5Measure(const std::vector<float>& codes, int w, int h, int px, int py)
	{
		auto at = [&](int x, int y) {
			x = (std::max)(0, (std::min)(w - 1, x));
			y = (std::max)(0, (std::min)(h - 1, y));
			return codes[static_cast<size_t>(y) * w + x];
		};
		const float a = at(px - 1, py - 1), b = at(px, py - 1), c = at(px + 1, py - 1);
		const float d = at(px - 1, py), e = at(px, py), f = at(px + 1, py);
		const float g = at(px - 1, py + 1), hh = at(px, py + 1), k = at(px + 1, py + 1);
		const float gx = (c + 2.0f * f + k) - (a + 2.0f * d + g);
		const float gy = (g + 2.0f * hh + k) - (a + 2.0f * b + c);
		X5Taps t;
		t.grad = std::sqrt(gx * gx + gy * gy) * 0.25f;
		t.lap = std::fabs(4.0f * e - b - d - f - hh);
		t.e = e;
		return t;
	}

	// The Laplacian X5 will read from the stage variant's G (S3): its four neighbours, clamped to the picture.
	float LapFromG(const std::vector<float>& g, int w, int h, int px, int py)
	{
		auto at = [&](int x, int y) {
			x = (std::max)(0, (std::min)(w - 1, x));
			y = (std::max)(0, (std::min)(h - 1, y));
			return g[static_cast<size_t>(y) * w + x];
		};
		return std::fabs(4.0f * at(px, py) - at(px, py - 1) - at(px - 1, py) - at(px + 1, py) - at(px, py + 1));
	}

	pl_color_space Space(pl_color_primaries prim, pl_color_transfer trc)
	{
		pl_color_space c{};
		c.primaries = prim;
		c.transfer = trc;
		return c;
	}

	OwnerFillRequest StageRequest(const void* host, uint64_t seq, const OwnerFill& f, const VariantKey& key, OwnerGpuContext* ctx)
	{
		OwnerFillRequest req;
		req.host = host;
		req.id = kEdgeMap;
		req.seq = seq;
		req.form = AcquireForm::Gpu;
		req.variant = key.Canonical(f.defaults);
		req.gpu = ctx;
		return req;
	}

	TEST_CLASS(EdgeMapStageTests)
	{
	public:
		TEST_METHOD(StageLawIsX5sFormerSobelAndGIsItsLuma)
		{
			const int w = 37, h = 29;   // not a multiple of the 16 x 16 group
			const std::vector<double> rgb = TestPicture(w, h);
			struct Case { pl_color_space color; pl_color_system sys; pl_color_primaries prim; float mode; };
			const Case cases[4] = {
				{ Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886), PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709, 0.0f },
				{ Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_LINEAR), PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709, 1.0f },
				{ Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ), PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_2020, 2.0f },
				{ Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_HLG), PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_2020, 2.0f } };
			for (const Case& c : cases)
			{
				const VpMeasure::MeasureDomain d = VpMeasure::MeasureDomainFor(c.color, c.sys, c.prim);
				Assert::AreEqual(c.mode, d.mode, L"the measure domain mode of the picture's transfer");
				const std::vector<float> g = MeasureCodes(d, rgb, w, h);
				const std::vector<float> r = RegionEvidence::BusyEdge::SobelMagnitude(g.data(), w, h);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						const X5Taps t = X5Measure(g, w, h, x, y);
						const size_t i = static_cast<size_t>(y) * w + x;
						Assert::AreEqual(t.grad, r[i] * 0.25f, L"R x 0.25 = X5's grad, exactly (a power of two)");
						Assert::AreEqual(t.e, g[i], L"G = X5's centre luma");
						Assert::AreEqual(t.lap, LapFromG(g, w, h, x, y), L"X5's Laplacian from G, exactly");
					}
			}
			const VpMeasure::MeasureDomain d709 = VpMeasure::MeasureDomainFor(cases[0].color, cases[0].sys, cases[0].prim);
			const VpMeasure::MeasureDomain d2020 = VpMeasure::MeasureDomainFor(Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_BT_1886),
				PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_2020);
			Assert::IsTrue(d709.bt709 && !d2020.bt709, L"the source's own weights: BT.709 and BT.2020 differ");
			const double grey[3] = { 0.5, 0.5, 0.5 };
			Assert::AreEqual(VpMeasure::MeasureLumaCode(d709, grey), VpMeasure::MeasureLumaCode(d2020, grey), 1e-6,
				L"a neutral grey has one luma under either weights");
		}

		TEST_METHOD(StageTapsClampToTheWholePictureNotACrop)
		{
			const int w = 40, h = 30;
			const std::vector<double> rgb = TestPicture(w, h);
			const VpMeasure::MeasureDomain d = VpMeasure::MeasureDomainFor(Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886),
				PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709);
			const std::vector<float> g = MeasureCodes(d, rgb, w, h);
			const std::vector<float> r = RegionEvidence::BusyEdge::SobelMagnitude(g.data(), w, h);
			// A crop (8, 6) 24 x 18 (X5's src_rect): its border pixels read luma beyond the crop, so the whole-picture map is exact
			// there and a crop-clamped map would not be.
			const int ox = 8, oy = 6, cw = 24, ch = 18;
			std::vector<float> crop(static_cast<size_t>(cw) * ch);
			for (int y = 0; y < ch; ++y)
				for (int x = 0; x < cw; ++x)
					crop[static_cast<size_t>(y) * cw + x] = g[static_cast<size_t>(y + oy) * w + x + ox];
			const std::vector<float> cropped = RegionEvidence::BusyEdge::SobelMagnitude(crop.data(), cw, ch);
			int differ = 0;
			for (int y = 0; y < ch; ++y)
				for (int x = 0; x < cw; ++x)
				{
					const X5Taps t = X5Measure(g, w, h, ox + x, oy + y);
					Assert::AreEqual(t.grad, r[static_cast<size_t>(y + oy) * w + x + ox] * 0.25f, L"whole-picture R = X5 at every crop pixel");
					if (t.grad != cropped[static_cast<size_t>(y) * cw + x] * 0.25f)
						++differ;
				}
			Assert::IsTrue(differ > 0, L"a crop-clamped map differs at the crop border (why the variant has no crop axis)");
			// The picture's own border clamps to itself (X5's ad_luma clamp).
			const X5Taps corner = X5Measure(g, w, h, 0, 0);
			Assert::AreEqual(corner.grad, r[0] * 0.25f, L"the picture corner clamps as X5 did");
		}

		TEST_METHOD(FillAxesDefaultsKeyAndReserve)
		{
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(Vpv::Regions::EdgeMapFillReserveBytes(DimsOf(1920, 1080)));
			Assert::AreEqual(uint32_t(VariantKey::kAxisStage | VariantKey::kAxisSize | VariantKey::kAxisCodeUnit | VariantKey::kAxisParam1),
				f.variantAxes, L"stage, size, code unit, ordinal");
			Assert::AreEqual(uint32_t(0), f.variantAxes & uint32_t(VariantKey::kAxisCrop), L"no crop axis (whole picture)");
			Assert::IsTrue(f.defaults.codeUnit == 10, L"the default variant's code unit is 10");
			Assert::IsTrue(f.defaults.stage == 0, L"the default is the source planes");
			Assert::IsTrue(f.frameScoped, L"a per-frame memo");

			const VariantKey k = Vpv::Regions::EdgeMapStageKey(kStage, 64, 48, 1).Canonical(f.defaults);
			Assert::IsTrue(k.stage == kStage && k.width == 64 && k.height == 48 && k.codeUnit == 8 && k.param1 == 1,
				L"the stage key: P13's stage key plus code unit 8, kept by canonicalisation");
			VariantKey ten = Vpv::Regions::EdgeMapStageKey(kStage, 64, 48, 0);
			ten.codeUnit = 10;
			Assert::IsTrue(ten.Canonical(f.defaults).codeUnit == 0, L"code unit 10 is the default: canonical 0");

			const Dims sizes[3] = { DimsOf(7680, 4320), DimsOf(3840, 2160), DimsOf(1919, 1081) };
			for (const Dims& d : sizes)
			{
				TexShape rg;
				rg.w = d.width;
				rg.h = d.height;
				rg.fmt = TexFormat::RG32F;
				Assert::AreEqual(uint64_t(d.width) * d.height * 8u, Vpv::Regions::EdgeMapFillReserveBytes(d), L"8 B per source pixel");
				Assert::AreEqual(ShapeBytes(rg), Vpv::Regions::EdgeMapFillReserveBytes(d), L"an RG32F stage picture of the luma size fits");
				Assert::AreEqual(Vpv::Regions::EdgeMapReserveBytes(d), Vpv::Regions::EdgeMapFillReserveBytes(d),
					L"the declared size itself: ledger and fill reserve are one figure (CB-18 SHOULD-2)");
			}
			Assert::AreEqual(uint64_t(0), Vpv::Regions::EdgeMapFillReserveBytes(DimsOf(0, 0)), L"no size known: no reserve");
		}

		TEST_METHOD(StageFillMakesRg32fAtThePictureSizeAndOrdinal)
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
				== SkeletonMemo::Registration::Registered, L"P13 owner");
			Assert::IsTrue(SourcePlanes::OfferStage(&host, Picture(7, 0, &full)), L"the full picture");
			Assert::IsTrue(SourcePlanes::OfferStage(&host, Picture(7, 0, &half)), L"the ratio-2 half picture");
			Assert::IsTrue(SourcePlanes::OfferStage(&host, Picture(7, 1, &rerender)), L"a re-render");

			const uint64_t need = Vpv::Regions::EdgeMapFillReserveBytes(DimsOf(64, 48));
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(need);
			struct Want { uint32_t w, h; uint16_t ordinal; };
			const Want wants[3] = { { 64, 48, 0 }, { 32, 24, 0 }, { 64, 48, 1 } };
			for (const Want& want : wants)
			{
				RefusingDevice dev;
				OwnerGpuContext ctx(&dev, need);
				OwnerFillOutput out;
				Assert::IsFalse(f.fill(StageRequest(&host, 7, f, Vpv::Regions::EdgeMapStageKey(kStage, want.w, want.h, want.ordinal), &ctx),
					m, out), L"the device refused: declined");
				Assert::AreEqual(std::string("edge_map: test device makes no texture"), out.declineReason, L"the device's reason");
				Assert::AreEqual(1, dev.asked, L"asked once");
				Assert::IsTrue(dev.lastShape.w == want.w && dev.lastShape.h == want.h && dev.lastShape.fmt == TexFormat::RG32F,
					L"RG32F at the stage picture's own size");
			}
			SourcePlanes::Forget(&host);
		}

		TEST_METHOD(StageFillDeclinesVisibly)
		{
			int host = 0;
			pl_tex_t full{};
			full.params.w = 64;
			full.params.h = 48;
			SkeletonMemo m;
			m.SetHost(&host);
			m.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(m.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill())
				== SkeletonMemo::Registration::Registered, L"P13 owner");
			Assert::IsTrue(SourcePlanes::OfferStage(&host, Picture(7, 0, &full)), L"the full picture");
			const uint64_t need = Vpv::Regions::EdgeMapFillReserveBytes(DimsOf(64, 48));
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(need);

			{   // A stage key in the default code unit (canonical 0): declined, said so.
				RefusingDevice dev;
				OwnerGpuContext ctx(&dev, need);
				OwnerFillOutput out;
				Assert::IsFalse(f.fill(StageRequest(&host, 7, f, SourcePlanes::StageKey(kStage, 64, 48, 0), &ctx), m, out), L"declined");
				Assert::IsTrue(Has(out.declineReason, "edge_map: a stage variant is served in 8-bit measure codes only"), L"worded");
				Assert::AreEqual(0, dev.asked, L"no texture asked for");
			}
			{   // A stage key without its size.
				RefusingDevice dev;
				OwnerGpuContext ctx(&dev, need);
				OwnerFillOutput out;
				Assert::IsFalse(f.fill(StageRequest(&host, 7, f, Vpv::Regions::EdgeMapStageKey(kStage, 0, 0, 0), &ctx), m, out), L"declined");
				Assert::AreEqual(std::string("edge_map: a stage variant needs its picture size"), out.declineReason, L"worded");
				Assert::AreEqual(0, dev.asked, L"no texture asked for");
			}
			{   // A picture nobody offered: P13's own cause carried.
				RefusingDevice dev;
				OwnerGpuContext ctx(&dev, need);
				OwnerFillOutput out;
				Assert::IsFalse(f.fill(StageRequest(&host, 7, f, Vpv::Regions::EdgeMapStageKey(kStage, 48, 48, 0), &ctx), m, out), L"declined");
				Assert::IsTrue(Has(out.declineReason, "edge_map: no stage picture (") && Has(out.declineReason, "stage not offered this frame"),
					L"worded with P13's cause");
				Assert::AreEqual(0, dev.asked, L"no texture asked for");
			}
			{   // A size / code unit / ordinal without a stage.
				RefusingDevice dev;
				OwnerGpuContext ctx(&dev, need);
				VariantKey sizeOnly;
				sizeOnly.width = 64;
				sizeOnly.height = 48;
				OwnerFillOutput out;
				Assert::IsFalse(f.fill(StageRequest(&host, 7, f, sizeOnly, &ctx), m, out), L"declined");
				Assert::AreEqual(std::string("edge_map: size, code unit and ordinal are axes of a stage variant only"), out.declineReason,
					L"worded");
				Assert::AreEqual(0, dev.asked, L"no texture asked for");
			}
			{   // A reserve below the RG32F picture: visible, the device never asked.
				RefusingDevice dev;
				OwnerGpuContext under(&dev, need - 1u);
				OwnerFillOutput out;
				Assert::IsFalse(f.fill(StageRequest(&host, 7, f, Vpv::Regions::EdgeMapStageKey(kStage, 64, 48, 0), &under), m, out),
					L"above the reserve: declined");
				Assert::IsTrue(out.declineReason.find("edge_map: ") == 0 && Has(out.declineReason, "reserve"), L"worded with the reserve");
				Assert::AreEqual(0, dev.asked, L"the device is never asked");
				Assert::AreEqual(uint64_t(1), under.Refused(), L"counted as refused");
			}
			// The crop axis is not declared: the memo refuses it before any fill (VariantUndeclared).
			Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Gpu, Vpv::Regions::kEdgeMapUnit, f) == SkeletonMemo::Registration::Registered,
				L"registered by its unit");
			VariantKey cropped = Vpv::Regions::EdgeMapStageKey(kStage, 64, 48, 0);
			cropped.cropW = 16;
			cropped.cropH = 16;
			Assert::IsTrue(m.Acquire(kEdgeMap, 7, AcquireForm::Gpu, cropped).cause == AcquireCause::VariantUndeclared, L"no crop axis");
			SourcePlanes::Forget(&host);
		}

		TEST_METHOD(OneBodyOneLawBothVariants)
		{
			const std::string body = RegionEvidence::EdgeMapBodyGlsl();
			const std::string stage = RegionEvidence::EdgeMapStageHeaderGlsl();
			const std::string plain = RegionEvidence::EdgeMapHeaderGlsl();
			Assert::IsTrue(Has(body, "length(vec2(gx, gy))"), L"the law: length, as X5's former Sobel");
			Assert::IsTrue(Has(body, "em_tap(") && Has(body, "em_store("), L"the body reads and writes through the variant's header");
			Assert::IsTrue(Has(stage, "ad_luma(p)") && Has(stage, "vec4(r, e, 0.0, 1.0)"), L"stage: measure luma taps, G = the luma");
			Assert::IsTrue(Has(plain, "em_scale") && Has(plain, "vec4(r, 0.0, 0.0, 1.0)"), L"default: source taps in codes, R only");
		}
	};
}
