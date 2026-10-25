// Uses the test project's precompiled header (CustomHookChain.h needs MFC's CString via the
// madVR shader loader).
#include "pch.h"
#include "CppUnitTest.h"

#include <ExtendedRangeGpuProbe.h>
#include <VP-V/modules/colour/extended_range/ExtendedRangeStage.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleStage.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/renderer.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// WP-13 GPU byte identity (written, NOT run by the WP-13 agent; Category GpuHardware keeps them out
// of the filtered CPU suite). Each test renders the same frame twice on one device: once with the
// hook list today's host code builds (a copy of the replaced statements, each on its own module
// objects) and once with the WP-13 stage, and expects 0 codes difference. The BTB (code 4) and WTW
// (code 1019) probe columns must survive both. Runs on WARP by default; VP_TEST_GPU=hardware runs on
// the NVIDIA adapter. Plan: docs/design/m2-output-callsites.md, "GPU byte identity".
namespace VideoProcessorTest
{
	namespace
	{
		using Prepare = std::function<void(pl_frame& image, const pl_frame& out, std::vector<const pl_hook*>& list)>;

		struct Rendered
		{
			bool ok = false;
			std::string error;
			int width = 0, height = 0;
			std::vector<uint32_t> pixels;   // R10G10B10A2
			int Green(int x, int y) const { return static_cast<int>((pixels[static_cast<size_t>(y) * width + x] >> 10) & 1023u); }
		};

		// The probe's frame (10-bit limited 4:2:2 YCbCr -> 10-bit limited RGB), hooks from `prepare`.
		Rendered RenderWith(pl_gpu gpu, pl_renderer renderer, const ExtendedRangeGpuProbe::Planes& planes,
			enum pl_color_primaries srcPrim, enum pl_color_transfer srcTrc, enum pl_color_primaries outPrim,
			enum pl_color_transfer outTrc, double scale, const Prepare& prepare)
		{
			Rendered r;
			const auto sampleCaps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
			pl_fmt r16 = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, sampleCaps);
			pl_fmt rg16 = pl_find_fmt(gpu, PL_FMT_UNORM, 2, 16, 16, sampleCaps);
			pl_fmt t10 = nullptr;
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
			for (int i = 0; i < gpu->num_formats; ++i)
			{
				pl_fmt f = gpu->formats[i];
				if (f->type == PL_FMT_UNORM && f->num_components == 4 && f->component_depth[0] == 10 &&
					f->texel_size == 4 && (f->caps & caps) == caps && f->host_bits[0] == 10) { t10 = f; break; }
			}
			if (!r16 || !rg16 || !t10) { r.error = "formats"; return r; }
			pl_tex_params lp{}; lp.w = planes.width; lp.h = planes.height; lp.format = r16; lp.sampleable = true;
			lp.initial_data = planes.luma.data();
			pl_tex_params cp{}; cp.w = planes.width / 2; cp.h = planes.height; cp.format = rg16; cp.sampleable = true;
			cp.initial_data = planes.chroma.data();
			r.width = static_cast<int>(planes.width * scale);
			r.height = static_cast<int>(planes.height * scale);
			pl_tex_params tp{}; tp.w = r.width; tp.h = r.height; tp.format = t10; tp.renderable = true; tp.host_readable = true;
			tp.blit_dst = (t10->caps & PL_FMT_CAP_BLITTABLE) != 0;
			pl_tex lt = pl_tex_create(gpu, &lp), ct = pl_tex_create(gpu, &cp), tt = pl_tex_create(gpu, &tp);
			if (lt && ct && tt)
			{
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = lt; image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = ct; image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_LIMITED;
				image.repr.bits.sample_depth = 16; image.repr.bits.color_depth = 10; image.repr.bits.bit_shift = 6;
				image.color.primaries = srcPrim;
				image.color.transfer = srcTrc;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame out{};
				out.num_planes = 1;
				out.planes[0].texture = tt;
				out.planes[0].components = 3;
				for (int i = 0; i < 3; ++i) out.planes[0].component_mapping[i] = i;
				out.repr.sys = PL_COLOR_SYSTEM_RGB;
				out.repr.levels = PL_COLOR_LEVELS_LIMITED;
				out.repr.bits.sample_depth = 10; out.repr.bits.color_depth = 10;
				out.color.primaries = outPrim;
				out.color.transfer = outTrc;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				std::vector<const pl_hook*> list;
				prepare(image, out, list);
				params.hooks = list.empty() ? nullptr : list.data();
				params.num_hooks = static_cast<int>(list.size());
				const float black[3] = { 0.0f, 0.0f, 0.0f };
				pl_frame_clear(gpu, &out, black);
				if (pl_render_image(renderer, &image, &out, &params))
				{
					pl_gpu_finish(gpu);
					r.pixels.resize(static_cast<size_t>(r.width) * r.height);
					pl_tex_transfer_params d{}; d.tex = tt; d.ptr = r.pixels.data();
					r.ok = pl_tex_download(gpu, &d);
					if (!r.ok) r.error = "download";
				}
				else
					r.error = "pl_render_image";
			}
			else
				r.error = "textures";
			pl_tex_destroy(gpu, &lt);
			pl_tex_destroy(gpu, &ct);
			pl_tex_destroy(gpu, &tt);
			return r;
		}

		int MaxDifference(const Rendered& a, const Rendered& b)
		{
			if (!a.ok || !b.ok || a.pixels.size() != b.pixels.size()) return 1 << 20;
			int worst = 0;
			for (size_t i = 0; i < a.pixels.size(); ++i)
				for (int ch = 0; ch < 3; ++ch)
					worst = (std::max)(worst, std::abs(static_cast<int>((a.pixels[i] >> (10 * ch)) & 1023u) -
						static_cast<int>((b.pixels[i] >> (10 * ch)) & 1023u)));
			return worst;
		}

		std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		struct Device
		{
			pl_log log = nullptr;
			pl_d3d11 d3d = nullptr;
			pl_renderer renderer = nullptr;
			Device()
			{
				pl_log_params logParams{};
				logParams.log_level = PL_LOG_NONE;
				log = pl_log_create(PL_API_VER, &logParams);
				std::string name;
				const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&name);
				Logger::WriteMessage(("GPU tests on: " + name + "\n").c_str());
				d3d = pl_d3d11_create(log, &deviceParams);
				if (d3d) renderer = pl_renderer_create(log, d3d->gpu);
			}
			~Device()
			{
				pl_renderer_destroy(&renderer);
				pl_d3d11_destroy(&d3d);
				pl_log_destroy(&log);
			}
		};
	}

	TEST_CLASS(OutputGlueStagesGpuTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(ExtendedRangeStageRendersTodaysCodes)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ExtendedRangeStageRendersTodaysCodes)
		{
			Device dev;
			Assert::IsTrue(dev.d3d && dev.renderer, L"Could not create the libplacebo test device");
			const std::vector<int> codes = { 4, 16, 40, 64, 100, 300, 600, 900, 940, 980, 1019 };
			const ExtendedRangeGpuProbe::Planes planes = ExtendedRangeGpuProbe::Greys(codes, 32, 16);
			struct Case { enum pl_color_primaries sp; enum pl_color_transfer st; enum pl_color_primaries op; enum pl_color_transfer ot; double scale; };
			const Case cases[] = {
				{ PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22, 1.0 },
				{ PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22, 2.0 },
				{ PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_GAMMA22, 0.5 },
				{ PL_COLOR_PRIM_BT_601_625, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA24, 1.0 } };
			for (const Case& c : cases)
				for (int moreCases = 0; moreCases < 2; ++moreCases)
				{
					ExtendedRangeStage::Frame f;
					f.preserve = true;
					f.moreCases = moreCases != 0;
					// Today's host code (29c3843d RenderLocked, Stage 10) on its own hooks.
					ExtendedRange::Hooks todays;
					const Rendered a = RenderWith(dev.d3d->gpu, dev.renderer, planes, c.sp, c.st, c.op, c.ot, c.scale,
						[&](pl_frame& image, const pl_frame& out, std::vector<const pl_hook*>& list) {
							ExtendedRange::Settings s;
							s.piecewiseTransfers = s.wideSourcePrimaries = s.calibrationLut = s.hdrPassthrough = f.moreCases;
							const ExtendedRange::Decision d = ExtendedRange::Decide(true, s, image.color, out.color, nullptr, PL_LUT_UNKNOWN);
							ExtendedRange::Prepare(d, todays, image.color, out.color, list, image.num_planes == 1);
						});
					ExtendedRangeStage::Stage stage;
					const Rendered b = RenderWith(dev.d3d->gpu, dev.renderer, planes, c.sp, c.st, c.op, c.ot, c.scale,
						[&](pl_frame& image, const pl_frame& out, std::vector<const pl_hook*>& list) {
							f.singlePlaneRgb = image.num_planes == 1;
							stage.Prepare(f, image.color, out.color, list);
						});
					todays.Release();
					stage.Release();
					Assert::IsTrue(a.ok && b.ok, W("render failed: " + a.error + " / " + b.error).c_str());
					Assert::AreEqual(0, MaxDifference(a, b), L"the stage must render today's codes exactly");
					if (stage.LastDecision().Active())
					{
						// BTB / WTW carried: the code-4 column stays below black, the code-1019 column above white.
						const int y = b.height / 2;
						const int col = static_cast<int>(32 * c.scale);
						Assert::IsTrue(b.Green(col / 2, y) < 64, L"BTB (code 4) clipped");
						Assert::IsTrue(b.Green(static_cast<int>((codes.size() - 0.5) * col), y) > 940, L"WTW (code 1019) clipped");
					}
				}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(AdaptiveUpscaleStageRendersTodaysCodes)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(AdaptiveUpscaleStageRendersTodaysCodes)
		{
			Device dev;
			Assert::IsTrue(dev.d3d && dev.renderer, L"Could not create the libplacebo test device");
			const ExtendedRangeGpuProbe::Planes planes = ExtendedRangeGpuProbe::Edges();
			// Today's host code (29c3843d RenderLocked, Stage 8b upscale branch, no motion, no NLS) on
			// its own module; the stage on another. Three frames: the module's temporal state.
			AdaptiveDownscale::Module todays;
			AdaptiveDownscale::Module moved;
			AdaptiveDownscaleStage::Stage stage{ moved };
			const AdaptiveDownscale::Settings settings;
			for (int frameIndex = 1; frameIndex <= 3; ++frameIndex)
			{
				const Rendered a = RenderWith(dev.d3d->gpu, dev.renderer, planes, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22,
					PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22, 2.0,
					[&](pl_frame& image, const pl_frame&, std::vector<const pl_hook*>& list) {
						AdaptiveDownscale::Settings now = settings;
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
						now.float32Picture = true;
#endif
						now.residualCodingNoiseCodes = 0.0f;
						todays.SetFrame(now, static_cast<uint64_t>(frameIndex), 1.0f / 24.0f);
						AdaptiveDownscale::Upscale up;
						up.on = true;
						up.wBase = 1.0f;
						up.superresAlpha = false;
						up.nativeHeight = static_cast<float>(image.planes[0].texture->params.h);
						todays.SetUpscale(up);
						todays.ClearGrainField();
						todays.ClearMotion();
						list.insert(list.begin(), todays.Measure());
						list.push_back(todays.Blend());
					});
				double weight = 1.0;
				bool grainWanted = false, noSsim = false;
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping(); };
				host.ssimInChain = []() { return false; };
				host.motion = []() { return AdaptiveDownscaleStage::MotionSource(); };
				host.publishStatus = []() {};
				const Rendered b = RenderWith(dev.d3d->gpu, dev.renderer, planes, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22,
					PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22, 2.0,
					[&](pl_frame& image, const pl_frame& out, std::vector<const pl_hook*>& list) {
						AdaptiveDownscaleStage::Frame f;
						f.adaptiveUpscale = true;
						f.adaptive = &settings;
						f.lowBitrateOff = true;
						f.fboFloat32 = 1;
						f.sourceSequence = static_cast<uint64_t>(frameIndex);
						f.nominalSourceRateHz = 24.0;
						f.sourceCrop = { 0, 0, static_cast<float>(image.planes[0].texture->params.w),
							static_cast<float>(image.planes[0].texture->params.h) };
						f.targetCrop = { 0, 0, static_cast<float>(out.planes[0].texture->params.w),
							static_cast<float>(out.planes[0].texture->params.h) };
						stage.Register(f, host, { weight, grainWanted, noSsim }, list);
					});
				Assert::IsTrue(a.ok && b.ok, W("render failed: " + a.error + " / " + b.error).c_str());
				Assert::AreEqual(0, MaxDifference(a, b), L"the stage must render today's codes exactly");
			}
			todays.Release();
			moved.Release();
		}
	};
}
