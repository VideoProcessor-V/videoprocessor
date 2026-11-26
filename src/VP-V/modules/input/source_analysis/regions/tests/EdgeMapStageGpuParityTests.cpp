// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CB slice 18 (coreplan-CB-18-edgemap.md S2b; 102b EDGEMAP-GPU-PARITY-TEST, owed before S3's reader row): the kEdgeMap owner fill
// (EdgeMapFill.h MakeEdgeMapOwnerFill) run on a libplacebo device against its CPU reference (EdgeMapStageTests's law):
//   1. the STAGE variant: R = BusyEdge::SobelMagnitude of the measure luma (VpMeasure::MeasureLumaCode) and G = that luma, at the
//      stage picture's own size, for the SDR, linear, PQ and HLG measure domains; R is also exactly the Sobel of the fill's own G
//      (the body's law on the GPU input, no tolerance for the transfer decode);
//   2. the DEFAULT variant: R = SobelMagnitude of the source luma plane in 10-bit codes (kSourceTapCodesPerUnit), R32F.
// Odd picture sizes cross the 16x16 work-group tiles. The fill gets its inputs through the memo (P13 stage / plane views) as in the
// renderer; the device double here differs from OwnerGpuDevice only in asking for host-readable textures (the test reads them back).
// Category GpuWarp: WARP in the CPU tier; on hardware with VP_TEST_GPU=hardware (GPU-QUIET, VP_TEST_ADAPTER=NVIDIA).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/control/skeleton/GpuProductTexBackend.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdge.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdgeGlsl.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapFill.h>
#include <VP-V/modules/input/source_analysis/regions/MeasureDomain.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		using namespace VpSkeleton;
		using Vpv::Capture::SourcePlanes;
		using Vpv::Capture::SourcePlaneSet;
		using Vpv::Capture::StagePicture;

		const uint8_t kParityStage = 5;   // any non-zero stage id

		void Check(bool ok, const std::string& what)
		{
			if (ok) return;
			Logger::WriteMessage((what + "\n").c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		// The owner-fill device of the renderer (OwnerGpuDevice) with host-readable textures: the render pl_gpu, a dispatch of its
		// own and textures through the skeleton's texture backend. Textures are destroyed with the device.
		struct ReadableOwnerDevice : IOwnerGpuDevice
		{
			explicit ReadableOwnerDevice(pl_gpu g) : gpu(g), backend(g, true), dispatch(pl_dispatch_create(g->log, g)) {}
			~ReadableOwnerDevice() override
			{
				for (pl_tex& t : made)
					pl_tex_destroy(gpu, &t);
				if (dispatch != nullptr)
					pl_dispatch_destroy(&dispatch);
			}
			ReadableOwnerDevice(const ReadableOwnerDevice&) = delete;
			ReadableOwnerDevice& operator=(const ReadableOwnerDevice&) = delete;

			void* OwnerGpu() const override { return const_cast<void*>(static_cast<const void*>(gpu)); }
			void* OwnerDispatch() const override { return static_cast<void*>(dispatch); }
			bool CreateOwnerTexture(const TexShape& shape, GpuMemoHandle& out, std::string& why) override
			{
				out = GpuMemoHandle();
				const pl_tex tex = backend.Create(shape);
				if (tex == nullptr)
				{
					why = backend.lastError;
					return false;
				}
				made.push_back(tex);
				out.resource = const_cast<void*>(static_cast<const void*>(tex));
				out.width = shape.w;
				out.height = shape.h;
				out.format = uint32_t(shape.fmt);
				out.bytes = ShapeBytes(shape);
				out.keepAlive = std::shared_ptr<void>(out.resource, [](void*) {});
				return true;
			}

			pl_gpu gpu;
			VpLaneProducts::GpuTexBackend backend;
			pl_dispatch dispatch;
			std::vector<pl_tex> made;
		};

		pl_tex Upload(pl_gpu gpu, enum pl_fmt_type type, int comps, int depth, int w, int h, const void* data)
		{
			pl_fmt fmt = pl_find_fmt(gpu, type, comps, depth, depth, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(fmt, L"no sampleable input format");
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = fmt;
			params.sampleable = true;
			params.initial_data = data;
			pl_tex tex = pl_tex_create(gpu, &params);
			Assert::IsNotNull(tex, L"cannot create the input texture");
			return tex;
		}

		std::vector<float> Download(pl_gpu gpu, pl_tex tex, int comps)
		{
			std::vector<float> v(static_cast<size_t>(tex->params.w) * tex->params.h * comps);
			pl_tex_transfer_params d{};
			d.tex = tex;
			d.ptr = v.data();
			Assert::IsTrue(pl_tex_download(gpu, &d), L"the product texture is not host-readable");
			return v;
		}

		// A deterministic RGBA picture in [0, 1] with an edge, a ramp and noise (alpha 1).
		std::vector<float> TestRgba(int w, int h)
		{
			std::vector<float> rgba(static_cast<size_t>(w) * h * 4);
			uint32_t s = 12345u;
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					float* px = &rgba[(static_cast<size_t>(y) * w + x) * 4];
					for (int c = 0; c < 3; ++c)
					{
						s = s * 1664525u + 1013904223u;
						const float noise = float(s >> 8) / float(1u << 24) * 0.05f;
						const float base = (x > w / 2 ? 0.7f : 0.2f) + 0.002f * y + 0.1f * c;
						px[c] = base + noise < 1.0f ? base + noise : 1.0f;
					}
					px[3] = 1.0f;
				}
			return rgba;
		}

		bool Near(float got, float want, float rel)
		{
			return std::fabs(got - want) <= rel * (1.0f + std::fabs(want));
		}

		OwnerFillRequest Request(const void* host, uint64_t seq, const OwnerFill& f, const VariantKey& key, OwnerGpuContext* ctx)
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

		void RegisterP13(SkeletonMemo& m, const void* host)
		{
			m.SetHost(host);
			m.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(m.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill())
				== SkeletonMemo::Registration::Registered, L"P13 owner");
		}

		uint64_t ReserveOf(uint32_t w, uint32_t h)
		{
			Dims d;
			d.width = w;
			d.height = h;
			return Vpv::Regions::EdgeMapFillReserveBytes(d);
		}
	}

	TEST_CLASS(EdgeMapStageGpuParityTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(StageVariantMatchesTheCpuLawInEveryMeasureDomain)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StageVariantMatchesTheCpuLawInEveryMeasureDomain)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const pl_gpu gpu = fixture.Gpu();
			const int w = 61, h = 37;   // odd: partial work-group tiles on both axes
			const std::vector<float> rgba = TestRgba(w, h);
			pl_tex picture = Upload(gpu, PL_FMT_FLOAT, 4, 32, w, h, rgba.data());

			struct Domain { pl_color_primaries prim; pl_color_transfer trc; pl_color_system sys; const char* name; };
			const Domain domains[4] = {
				{ PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886, PL_COLOR_SYSTEM_BT_709, "SDR BT.709" },
				{ PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_LINEAR, PL_COLOR_SYSTEM_BT_709, "linear BT.709" },
				{ PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ, PL_COLOR_SYSTEM_BT_2020_NC, "PQ BT.2020" },
				{ PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_HLG, PL_COLOR_SYSTEM_BT_2020_NC, "HLG BT.2020" } };
			for (const Domain& dm : domains)
			{
				int host = 0;
				SkeletonMemo m;
				RegisterP13(m, &host);
				StagePicture pic;
				pic.seq = 7;
				pic.stage = kParityStage;
				pic.ordinal = 0;
				pic.tex = picture;
				pic.color.primaries = dm.prim;
				pic.color.transfer = dm.trc;
				pic.system = dm.sys;
				pic.primaries = dm.prim;
				pic.origin = "parity test stage";
				Assert::IsTrue(SourcePlanes::OfferStage(&host, pic), L"offered");

				const uint64_t need = ReserveOf(uint32_t(w), uint32_t(h));
				const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(need);
				ReadableOwnerDevice dev(gpu);
				std::vector<float> rg;
				{
					OwnerGpuContext ctx(&dev, need);
					OwnerFillOutput out;
					const bool ok = f.fill(Request(&host, 7, f, Vpv::Regions::EdgeMapStageKey(kParityStage, uint32_t(w), uint32_t(h), 0), &ctx),
						m, out);
					Check(ok, std::string(dm.name) + ": the stage fill declined: " + out.declineReason);
					const pl_tex result = static_cast<pl_tex>(out.gpu.resource);
					Check(result != nullptr && result->params.w == w && result->params.h == h, std::string(dm.name) + ": RG32F at the picture size");
					rg = Download(gpu, result, 2);
				}

				const VpMeasure::MeasureDomain d = VpMeasure::MeasureDomainFor(pic.color, pic.system, pic.primaries);
				std::vector<float> codes(static_cast<size_t>(w) * h), gpuG(codes.size());
				for (size_t i = 0; i < codes.size(); ++i)
				{
					const double px[3] = { rgba[i * 4 + 0], rgba[i * 4 + 1], rgba[i * 4 + 2] };
					codes[i] = static_cast<float>(VpMeasure::MeasureLumaCode(d, px));
					gpuG[i] = rg[i * 2 + 1];
				}
				const std::vector<float> wantR = RegionEvidence::BusyEdge::SobelMagnitude(codes.data(), w, h);
				const std::vector<float> ownR = RegionEvidence::BusyEdge::SobelMagnitude(gpuG.data(), w, h);
				int badG = 0, badR = 0, badOwn = 0;
				float worstG = 0.0f, worstR = 0.0f;
				for (size_t i = 0; i < codes.size(); ++i)
				{
					const float gotR = rg[i * 2 + 0];
					if (!Near(gpuG[i], codes[i], 1e-3f)) ++badG;
					if (!Near(gotR, wantR[i], 4e-3f)) ++badR;
					if (!Near(gotR, ownR[i], 1e-5f)) ++badOwn;
					worstG = (std::fmax)(worstG, std::fabs(gpuG[i] - codes[i]));
					worstR = (std::fmax)(worstR, std::fabs(gotR - wantR[i]));
				}
				Logger::WriteMessage((std::string("EDGEMAP_STAGE_PARITY ") + dm.name + " worst |dG| " + std::to_string(worstG) + " worst |dR| " +
					std::to_string(worstR) + "\n").c_str());
				Check(badG == 0, std::string(dm.name) + ": G is the measure luma (" + std::to_string(badG) + " pixels off)");
				Check(badR == 0, std::string(dm.name) + ": R is the Sobel of the measure luma (" + std::to_string(badR) + " pixels off)");
				Check(badOwn == 0, std::string(dm.name) + ": R is the Sobel of the fill's own G, clamped to the whole picture (" +
					std::to_string(badOwn) + " pixels off)");
				SourcePlanes::Forget(&host);
			}
			pl_tex_destroy(gpu, &picture);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(DefaultVariantMatchesSobelOfTheSourceLuma)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DefaultVariantMatchesSobelOfTheSourceLuma)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const pl_gpu gpu = fixture.Gpu();
			const int w = 45, h = 29;
			std::vector<uint16_t> luma(static_cast<size_t>(w) * h);
			uint32_t s = 777u;
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					s = s * 1664525u + 1013904223u;
					const uint32_t base = (x > w / 3 ? 40000u : 12000u) + uint32_t(y) * 200u;
					luma[static_cast<size_t>(y) * w + x] = uint16_t(base + (s >> 22));
				}
			pl_tex plane = Upload(gpu, PL_FMT_UNORM, 1, 16, w, h, luma.data());

			int host = 0;
			SkeletonMemo m;
			RegisterP13(m, &host);
			SourcePlaneSet set;
			set.seq = 9;
			set.planes = 1;
			set.tex[0] = plane;
			set.components[0] = 1;
			set.repr.sys = PL_COLOR_SYSTEM_BT_709;
			set.origin = "parity test planes";
			Assert::IsTrue(SourcePlanes::Offer(&host, set), L"offered");

			const uint64_t need = ReserveOf(uint32_t(w), uint32_t(h));
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(need);
			ReadableOwnerDevice dev(gpu);
			std::vector<float> r;
			{
				OwnerGpuContext ctx(&dev, need);
				OwnerFillOutput out;
				const bool ok = f.fill(Request(&host, 9, f, VariantKey(), &ctx), m, out);
				Check(ok, "the default fill declined: " + out.declineReason);
				const pl_tex result = static_cast<pl_tex>(out.gpu.resource);
				Check(result != nullptr && result->params.w == w && result->params.h == h, "R32F at the luma size");
				r = Download(gpu, result, 1);
			}

			std::vector<float> codes(luma.size());
			for (size_t i = 0; i < codes.size(); ++i)
				codes[i] = float(luma[i]) / 65535.0f * RegionEvidence::kSourceTapCodesPerUnit;
			const std::vector<float> want = RegionEvidence::BusyEdge::SobelMagnitude(codes.data(), w, h);
			int bad = 0;
			float worst = 0.0f;
			for (size_t i = 0; i < want.size(); ++i)
			{
				if (!Near(r[i], want[i], 1e-4f)) ++bad;
				worst = (std::fmax)(worst, std::fabs(r[i] - want[i]));
			}
			Logger::WriteMessage(("EDGEMAP_DEFAULT_PARITY worst |dR| " + std::to_string(worst) + "\n").c_str());
			Check(bad == 0, "R is the Sobel of the 10-bit source luma (" + std::to_string(bad) + " pixels off)");
			SourcePlanes::Forget(&host);
			pl_tex_destroy(gpu, &plane);
		}

		// W23-C2-S3-H3 (bff541a87): FillEdgeMapDefault derives em_scale = ldexp(1.0, 10 - bits) / CodeUnit(in, repr.bits, bits)
		// from P13 SourcePlaneSet repr.bits instead of hardcoding kSourceTapCodesPerUnit (65535/64), so LSB-aligned 10-bit
		// (bit_shift=0, color_depth=10) and 12-bit (bit_shift=0, color_depth=12) r16 UNORM source planes produce exact
		// 10-bit-code Sobel magnitudes; same-input hardcoded kSourceTapCodesPerUnit negative control is 64x / 16x wrong.
		BEGIN_TEST_METHOD_ATTRIBUTE(LsbAligned10And12BitSourcePlanesMatchSobelInTenBitCodeUnits)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LsbAligned10And12BitSourcePlanesMatchSobelInTenBitCodeUnits)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const pl_gpu gpu = fixture.Gpu();
			const int w = 40, h = 24;

			for (const int depth : { 10, 12 })
			{
				const uint32_t maxCode = (1u << depth) - 1u;
				const float toTenBit = (depth == 10) ? 1.0f : 0.25f;
				std::vector<uint16_t> luma(static_cast<size_t>(w) * h);
				std::vector<float> trueTenBitCodes(luma.size()), naiveTapCodes(luma.size());
				uint32_t s = 0x13579bdfu + static_cast<uint32_t>(depth);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						s = s * 1664525u + 1013904223u;
						const uint32_t step = (x >= w / 2) ? (depth == 10 ? 700u : 2800u) : (depth == 10 ? 200u : 800u);
						const uint16_t code = static_cast<uint16_t>((std::min)(maxCode, step + (s >> 26)));
						const size_t idx = static_cast<size_t>(y) * w + x;
						luma[idx] = code; // LSB-aligned (bit_shift == 0) in 16-bit UNORM texture!
						trueTenBitCodes[idx] = static_cast<float>(code) * toTenBit;
						naiveTapCodes[idx] = (static_cast<float>(code) / 65535.0f) * RegionEvidence::kSourceTapCodesPerUnit;
					}
				}

				pl_tex plane = Upload(gpu, PL_FMT_UNORM, 1, 16, w, h, luma.data());
				int host = 0;
				SkeletonMemo m;
				RegisterP13(m, &host);
				SourcePlaneSet set;
				set.seq = static_cast<uint64_t>(20 + depth);
				set.planes = 1;
				set.tex[0] = plane;
				set.components[0] = 1;
				set.repr.sys = PL_COLOR_SYSTEM_BT_709;
				set.repr.levels = PL_COLOR_LEVELS_LIMITED;
				set.repr.bits.sample_depth = 16;
				set.repr.bits.color_depth = depth;
				set.repr.bits.bit_shift = 0; // LSB-aligned!
				set.origin = "lsb-aligned edge_map parity test";
				Assert::IsTrue(SourcePlanes::Offer(&host, set), L"LSB-aligned SourcePlaneSet offered");

				const uint64_t need = ReserveOf(uint32_t(w), uint32_t(h));
				const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(need);
				ReadableOwnerDevice dev(gpu);
				std::vector<float> gotR;
				{
					OwnerGpuContext ctx(&dev, need);
					OwnerFillOutput out;
					const bool ok = f.fill(Request(&host, set.seq, f, VariantKey(), &ctx), m, out);
					Check(ok, "LSB-aligned default fill declined: " + out.declineReason);
					const pl_tex result = static_cast<pl_tex>(out.gpu.resource);
					Check(result != nullptr && result->params.w == w && result->params.h == h, "R32F at luma size");
					gotR = Download(gpu, result, 1);
				}

				const std::vector<float> wantR = RegionEvidence::BusyEdge::SobelMagnitude(trueTenBitCodes.data(), w, h);
				const std::vector<float> naiveR = RegionEvidence::BusyEdge::SobelMagnitude(naiveTapCodes.data(), w, h);

				// Same-input negative control: hardcoded kSourceTapCodesPerUnit under-scales Sobel by 64x (10-bit) / 16x (12-bit)!
				float maxEdgeWant = 0.0f, maxNaiveErr = 0.0f, maxGpuErr = 0.0f;
				for (size_t i = 0; i < wantR.size(); ++i)
				{
					maxEdgeWant = (std::fmax)(maxEdgeWant, wantR[i]);
					maxNaiveErr = (std::fmax)(maxNaiveErr, std::fabs(naiveR[i] - wantR[i]));
					maxGpuErr = (std::fmax)(maxGpuErr, std::fabs(gotR[i] - wantR[i]));
				}
				Assert::IsTrue(maxEdgeWant > 400.0f && maxNaiveErr > 350.0f,
					L"negative control: hardcoded kSourceTapCodesPerUnit severely under-scales LSB-aligned Sobel (>350 codes off)");
				Assert::IsTrue(maxGpuErr <= 2e-3f,
					(L"W23-C2-S3-H3: LSB-aligned " + std::to_wstring(depth) + L"-bit GPU Sobel worst error " +
						std::to_wstring(maxGpuErr) + L" > 2e-3").c_str());

				SourcePlanes::Forget(&host);
				pl_tex_destroy(gpu, &plane);
			}
		}
	};
}
