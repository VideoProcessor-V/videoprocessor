// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(AdaptiveScalingHookTests)
	{
	public:


		// Stage 8-2: the adaptive downscale blend (SSIM / faithful, luma only)
		// on a 1.5x downscale. Hard edges come out as the faithful branch,
		// low-contrast texture as SSIM; the grain term falls to the floor on
		// noise, recovers smoothly, snaps on a cut; nothing happens on an
		// upscale; both light domains give the same weight map; and the Stage
		// 10 carry's BTB/WTW codes survive the measure, SSIM and the blend.
		TEST_METHOD(AdaptiveDownscaleBlendsEdgesTextureAndGrain)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				Assert::IsFalse(source.empty());
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			static constexpr int sw = 384, sh = 240;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			auto makeSource = [&](auto code)
			{
				std::vector<uint16_t> px(static_cast<size_t>(sw) * sh * 4);
				for (int y = 0; y < sh; ++y)
					for (int x = 0; x < sw; ++x)
					{
						const double c = std::min(255.0, std::max(0.0, static_cast<double>(code(x, y))));
						const uint16_t v = static_cast<uint16_t>(std::lround(c * 257.0));
						uint16_t* p = &px[(static_cast<size_t>(y) * sw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = sw;
				tp.h = sh;
				tp.format = srcFmt;
				tp.sampleable = true;
				tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			};
			// Hard stripes on the left, low-contrast fine texture on the right.
			pl_tex scene = makeSource([](int x, int y)
			{
				if (x < sw / 2)
					return ((x / 12) % 2) ? 220.0 : 30.0;
				return 128.0 + 5.0 * std::sin(x * 1.7) * std::cos(y * 1.3);
			});
			uint32_t noiseState = 12345u;
			auto noise = [&noiseState]()
			{
				noiseState = noiseState * 1664525u + 1013904223u;
				return (static_cast<double>(noiseState >> 8) / 16777216.0 - 0.5) * 2.0;
			};
			std::vector<double> noiseField(static_cast<size_t>(sw) * sh);
			for (double& n : noiseField) n = noise() * 7.0;
			pl_tex grainy = makeSource([&](int x, int y) { return 128.0 + noiseField[static_cast<size_t>(y) * sw + x]; });
			pl_tex clean = makeSource([](int, int) { return 128.0; });
			pl_tex dark = makeSource([](int, int) { return 20.0; });
			pl_tex brighter = makeSource([&](int x, int y) { return 200.0 + noiseField[static_cast<size_t>(y) * sw + x]; });

			AdaptiveDownscale::Module module;
			uint64_t sequence = 0;
			auto render = [&](pl_tex src, int ow, int oh, bool withModule, const AdaptiveDownscale::Settings& st,
				bool gammaLight, bool advance)
			{
				pl_tex_params tp{};
				tp.w = ow;
				tp.h = oh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = gammaLight;
				std::vector<const pl_hook*> hooks;
				if (advance) ++sequence;
				if (withModule)
				{
					module.SetFrame(st, sequence, 1.0f / 24.0f);
					hooks.push_back(module.Measure());
				}
				hooks.push_back(ssim);
				if (withModule)
					hooks.push_back(module.Blend());
				params.hooks = hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> px(static_cast<size_t>(ow) * oh);
				pl_tex_transfer_params d{};
				d.tex = target;
				d.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			// The stats of the last advancing frame (polled by the next render).
			auto grainNow = [&](pl_tex src, const AdaptiveDownscale::Settings& st)
			{
				render(src, 256, 160, true, st, false, false);
				render(src, 256, 160, true, st, false, false);
				return module.LastStats();
			};
			std::string report;
			char line[200];

			// 1. Edges faithful, texture SSIM (grain term held at 1).
			AdaptiveDownscale::Settings noGrain;
			noGrain.grainLo = 63.0f;
			noGrain.grainHi = 64.0f;
			AdaptiveDownscale::Settings faithfulOnly = noGrain;
			faithfulOnly.wBase = 0.0f;
			// With edges switched off (thresholds above any gradient) w is 1
			// everywhere: the blend must be the SSIM picture (grey content: the
			// luma blend is the whole picture).
			AdaptiveDownscale::Settings allSsim = noGrain;
			allSsim.edgeLo = 254.0f;
			allSsim.edgeHi = 255.0f;
			const auto ssimOnly = render(scene, 256, 160, false, noGrain, false, true);
			const auto faithfulPx = render(scene, 256, 160, true, faithfulOnly, false, true);
			const auto asSsim = render(scene, 256, 160, true, allSsim, false, true);
			const auto blended = render(scene, 256, 160, true, noGrain, false, true);
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"The blend did not run on a downscale");
			int ssimMax = 0, edgeMax = 0, edgeSsimVsFaithful = 0;
			for (int y = 20; y < 140; ++y)
				for (int x = 4; x < 252; ++x)
				{
					const size_t i = static_cast<size_t>(y) * 256 + x;
					ssimMax = std::max(ssimMax, std::abs(asSsim[i].g - ssimOnly[i].g));
					const int sx = static_cast<int>((x + 0.5) * 1.5);
					if (x < 120 && (sx % 12 == 0 || sx % 12 == 11))   // stripe transitions
					{
						edgeMax = std::max(edgeMax, std::abs(blended[i].g - faithfulPx[i].g));
						edgeSsimVsFaithful = std::max(edgeSsimVsFaithful, std::abs(ssimOnly[i].g - faithfulPx[i].g));
					}
				}
			std::snprintf(line, sizeof(line), "w=1 vs SSIM max %d; edges vs faithful max %d (SSIM vs faithful there %d)\n",
				ssimMax, edgeMax, edgeSsimVsFaithful);
			report += line;
			// Edges: the edge map is interpolated between source pixels at a
			// transition, so up to 2 codes of SSIM remain there.
			Assert::IsTrue(ssimMax <= 1 && edgeMax <= 2 && edgeSsimVsFaithful >= 4,
				ExtendedRangeGpuProbe::ToWide(report).c_str());

			// 2. Grain: after a cut (dark -> grey noise) it snaps to the floor;
			// a clean field of the same mean recovers smoothly; a luma jump
			// (grey -> bright noise) snaps again.
			AdaptiveDownscale::Settings grain;
			render(dark, 256, 160, true, grain, false, true);
			render(grainy, 256, 160, true, grain, false, true);
			const auto noisy = grainNow(grainy, grain);
			render(clean, 256, 160, true, grain, false, true);
			const auto oneClean = grainNow(clean, grain);
			for (int i = 0; i < 120; ++i)
				render(clean, 256, 160, true, grain, false, true);
			const auto manyClean = grainNow(clean, grain);
			render(brighter, 256, 160, true, grain, false, true);
			const auto afterCut = grainNow(brighter, grain);
			std::snprintf(line, sizeof(line), "grain: noise %.3f (valid %d, luma %.1f, w %.3f), clean 1 frame %.3f, 120 frames %.3f, cut to noise %.3f (luma %.1f)\n",
				noisy.grainScale, noisy.valid ? 1 : 0, noisy.meanLuma, noisy.meanW, oneClean.grainScale,
				manyClean.grainScale, afterCut.grainScale, afterCut.meanLuma);
			report += line;
			Assert::IsTrue(noisy.valid && noisy.grainScale <= grain.grainFloor + 0.05f, ExtendedRangeGpuProbe::ToWide(report).c_str());
			Assert::IsTrue(oneClean.grainScale < 0.5f, ExtendedRangeGpuProbe::ToWide("no smoothing: " + report).c_str());
			Assert::IsTrue(manyClean.grainScale > 0.95f, ExtendedRangeGpuProbe::ToWide("no recovery: " + report).c_str());
			Assert::IsTrue(afterCut.grainScale <= grain.grainFloor + 0.05f, ExtendedRangeGpuProbe::ToWide("no snap: " + report).c_str());

			// 3. No effect on an upscale (the SSIM shader's own condition).
			const auto upPlain = render(scene, 576, 360, false, noGrain, false, true);
			const auto upModule = render(scene, 576, 360, true, noGrain, false, true);
			Assert::IsFalse(module.Active(), L"The blend ran on an upscale");
			for (size_t i = 0; i < upPlain.size(); ++i)
				Assert::IsTrue(upPlain[i].r == upModule[i].r && upPlain[i].g == upModule[i].g, L"An upscale changed");
			// 3b. The source's own height decides, as SSIM's `NATIVE_CROPPED.h >
			// POSTKERNEL.h`: a picture a prescaler made taller than the output,
			// from a source that is not, is left alone (audit stage8-rs232 item 3).
			module.SetSourceDownscales(false);
			render(scene, 256, 160, true, noGrain, false, true);
			Assert::IsFalse(module.Active(), L"The blend ran although the source is not downscaled");
			module.SetSourceDownscales(true);

			// 4. Both light domains, the same weight map (edges from the source).
			render(scene, 256, 160, true, noGrain, false, true);
			const auto linearStats = grainNow(scene, noGrain);
			render(scene, 256, 160, true, noGrain, true, true);
			render(scene, 256, 160, true, noGrain, true, false);
			render(scene, 256, 160, true, noGrain, true, false);
			const auto gammaStats = module.LastStats();
			std::snprintf(line, sizeof(line), "mean w linear %.4f gamma %.4f\n", linearStats.meanW, gammaStats.meanW);
			report += line;
			Assert::IsTrue(std::abs(linearStats.meanW - gammaStats.meanW) < 0.02f, ExtendedRangeGpuProbe::ToWide(report).c_str());

			// 5. The carry through measure, SSIM and blend (BT.709 -> P3, down 0.5).
			{
				using namespace ExtendedRangeGpuProbe;
				const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
				Case c;
				c.scale = 0.5;
				c.mitchell = true;
				module.SetFrame(noGrain, ++sequence, 1.0f / 24.0f);
				c.extraHooks = { module.Measure(), ssim, module.Blend() };
				// With the SSIM downscaler (the owner's 4K chain) every code is
				// kept, below black too, from the first carry frame on. (The
				// restore renders its own texture: as an inline colour step it was
				// disabled after any texture-output hook at POST_KERNEL.)
				auto check = [&](const std::vector<int>& got, const char* what)
				{
					for (size_t i = 0; i < codes.size(); ++i)
						Assert::IsTrue(std::abs(got[i] - codes[i]) <= (codes[i] > 940 || codes[i] < 64 ? 2 : 1),
							ToWide(std::string(what) + " changed the picture\n" + report).c_str());
				};
				const Result first = Render(gpu, fixture.Renderer(), Greys(codes, 64), c);
				const auto firstGot = Columns(first, static_cast<int>(codes.size()), 0.5, 64);
				report += "first carry frame after SSIM without it: " + Join(firstGot) + "\n";
				check(firstGot, "the first carry frame");
				{
					Case plain;
					plain.scale = 0.5;
					plain.mitchell = true;
					plain.extraHooks = { ssim };
					const auto ssimGot = Columns(Render(gpu, fixture.Renderer(), Greys(codes, 64), plain),
						static_cast<int>(codes.size()), 0.5, 64);
					report += "SSIM alone with the carry: " + Join(ssimGot) + "\n";
					check(ssimGot, "SSIM with the carry");
				}
				module.SetFrame(noGrain, ++sequence, 1.0f / 24.0f);
				const Result r = Render(gpu, fixture.Renderer(), Greys(codes, 64), c);
				Assert::IsTrue(r.ok);
				const auto got = Columns(r, static_cast<int>(codes.size()), 0.5, 64);
				report += "carry through the blend: " + Join(got) + "\n";
				check(got, "the blend");
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			for (pl_tex* t : { &scene, &grainy, &clean, &dark, &brighter })
				pl_tex_destroy(gpu, t);
			module.Release();
			pl_mpv_user_shader_destroy(&ssim);
			// Only shader-compiler timing notes may appear (a loaded machine).
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Stage 8b (docs/VP-stage8b-upscale-blend.md), with a nearest-neighbour
		// luma doubler standing in for the CNN (its 2x2 box average is the
		// source exactly, so the faithful branch must equal libplacebo's own
		// ewa_lanczossharp render). The grain term is forced by the ramp:
		// grain_lo 1000 (clean, g = 1) or grain_hi 0.001 (grain, g = floor 0.4).
		TEST_METHOD(AdaptiveUpscaleBlendsCnnAndFaithfulByGrain)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 128, height = 64;
			// Neutral grey: the blend is luma-only, so the branches are compared
			// exactly where R = G = B.
			auto chroma = [](int, int, int) { return static_cast<uint16_t>(32768); };
			const DejudderFrame frame = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chroma);
			DejudderGpuPlanes planes;
			UploadDejudderFrame(gpu, frame, planes);
			static const char doubler[] =
				"//!HOOK LUMA\n"
				"//!BIND HOOKED\n"
				"//!WIDTH HOOKED.w 2 *\n"
				"//!HEIGHT HOOKED.h 2 *\n"
				"//!WHEN OUTPUT.h HOOKED.h / 1.300 >\n"
				"//!DESC test nearest luma doubler\n"
				"vec4 hook() { return HOOKED_tex((floor(HOOKED_pos * HOOKED_size) + 0.5) / HOOKED_size); }\n";
			const pl_hook* cnn = pl_mpv_user_shader_parse(gpu, doubler, sizeof(doubler) - 1);
			Assert::IsNotNull(cnn);
			pl_fmt rgba16 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(rgba16);
			auto render = [&](int ow, int oh, std::vector<const pl_hook*> hooks, const pl_filter_config* upscaler)
			{
				pl_tex_params tp{};
				tp.w = ow;
				tp.h = oh;
				tp.format = rgba16;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target);
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planes.planes.luma;
				image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planes.planes.chroma;
				image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame t = TargetLutGpuFixture::MakeRgbFrame(target);
				t.repr.levels = PL_COLOR_LEVELS_FULL;
				t.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_render_params params = pl_render_fast_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				// upscale_light gamma for the exact comparisons: with linear light
				// libplacebo scales a doubler's 2x output DOWN to 1.5x in linear
				// light but upscales the plain source in gamma light (no linear
				// upscaling without sigmoid), so the references would differ in
				// domain; in gamma light every path is gamma.
				params.sigmoid_params = nullptr;
				params.disable_linear_scaling = true;
				params.upscaler = upscaler;
				params.antiringing_strength = 0.5f;   // as the module's faithful branch
				params.hooks = hooks.empty() ? nullptr : hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &params));
				pl_gpu_finish(gpu);
				std::vector<uint16_t> px(static_cast<size_t>(ow) * oh * 4);
				pl_tex_transfer_params d{};
				d.tex = target;
				d.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			AdaptiveDownscale::Settings settings;
			settings.grainMap = false;
			auto withModule = [&](AdaptiveDownscale::Module& module, float grainLo, float grainHi, float wBase,
				float nativeH, int ow, int oh, std::vector<const pl_hook*> hooks, bool superresAlpha = false,
				size_t blendAt = static_cast<size_t>(-1))
			{
				AdaptiveDownscale::Settings s2 = settings;
				s2.grainLo = grainLo;
				s2.grainHi = grainHi;
				s2.grainSmoothMs = 0.0f;
				// A new source frame per call: the grain state advances once per frame.
				static uint64_t sequence = 0;
				module.SetFrame(s2, ++sequence, 1.0f / 24.0f);
				AdaptiveDownscale::Upscale up;
				up.on = true;   // the module's own gate must refuse native and downscaled frames
				up.wBase = wBase;
				up.superresAlpha = superresAlpha;
				up.nativeHeight = nativeH;
				module.SetUpscale(up);
				hooks.insert(hooks.begin(), module.Measure());
				if (blendAt < hooks.size())
					hooks.insert(hooks.begin() + static_cast<std::ptrdiff_t>(blendAt) + 1, module.Blend());
				else
					hooks.push_back(module.Blend());
				return render(ow, oh, hooks, &pl_filter_ewa_lanczossharp);
			};
			auto maxDiff = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b)
			{
				int m = 0;
				for (size_t i = 0; i < a.size(); ++i)
					if (i % 4 != 3)
						m = std::max(m, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
				return m;
			};
			const int ow = width * 3 / 2, oh = height * 3 / 2;   // 1.5x: the doubler runs (2x > 1.3x)
			const std::vector<uint16_t> cnnBranch = render(ow, oh, { cnn }, &pl_filter_ewa_lanczossharp);
			const std::vector<uint16_t> faithfulRef = render(ow, oh, {}, &pl_filter_ewa_lanczossharp);
			Assert::IsTrue(maxDiff(cnnBranch, faithfulRef) > 1000, L"the stand-in CNN should differ from the faithful upscale");
			{
				// The stand-in at exactly 2x: the nearest-neighbour source (diagnostic).
				const std::vector<uint16_t> twice = render(width * 2, height * 2, { cnn }, &pl_filter_ewa_lanczossharp);
				int worst = 0, wx = 0, wy = 0;
				for (int y = 4; y < height * 2 - 4; ++y)
					for (int x = 4; x < width * 2 - 4; ++x)
					{
						const int d = std::abs(static_cast<int>(twice[(static_cast<size_t>(y) * width * 2 + x) * 4]) -
							static_cast<int>(frame.luma[static_cast<size_t>(y / 2) * width + x / 2]));
						if (d > worst) { worst = d; wx = x; wy = y; }
					}
				Assert::IsTrue(worst <= 64, ExtendedRangeGpuProbe::ToWide("doubler at 2x vs nearest source: max " + std::to_string(worst) + " at " +
					std::to_string(wx) + "," + std::to_string(wy) + ": got " +
					std::to_string(twice[(static_cast<size_t>(wy) * width * 2 + wx) * 4]) + ", source " +
					std::to_string(frame.luma[static_cast<size_t>(wy / 2) * width + wx / 2]) + ", left " +
					std::to_string(frame.luma[static_cast<size_t>(wy / 2) * width + wx / 2 - 1]) + ", right " +
					std::to_string(frame.luma[static_cast<size_t>(wy / 2) * width + wx / 2 + 1])).c_str());
			}

			// Clean (g = 1, w = 1): exactly the CNN branch, edges included.
			AdaptiveDownscale::Module module;
			const std::vector<uint16_t> clean = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh, { cnn });
			Assert::AreEqual(2, module.LastUpscaleRatio(), L"the doubler's output should be averaged to the source");
			const int cleanDiff = maxDiff(clean, cnnBranch);
			// w_base 0: the luma of the faithful branch (chroma: the same scaler in both).
			const std::vector<uint16_t> faithful = withModule(module, 1000.0f, 2000.0f, 0.0f, static_cast<float>(height), ow, oh, { cnn });
			const int faithfulDiff = maxDiff(faithful, faithfulRef);
			std::wstring where;
			{
				double sum = 0.0;
				int mx = 0, at = 0;
				for (size_t i = 0; i < faithful.size(); i += 4)
				{
					const int d = std::abs(static_cast<int>(faithful[i]) - static_cast<int>(faithfulRef[i]));
					sum += d;
					if (d > mx) { mx = d; at = static_cast<int>(i / 4); }
				}
				where = L" (mean " + std::to_wstring(sum / (faithful.size() / 4)) + L", max at " + std::to_wstring(at % ow) +
					L"," + std::to_wstring(at / ow) + L": " + std::to_wstring(faithful[static_cast<size_t>(at) * 4]) + L" vs " +
					std::to_wstring(faithfulRef[static_cast<size_t>(at) * 4]) + L", CNN " + std::to_wstring(cnnBranch[static_cast<size_t>(at) * 4]) + L")";
			}
			// Grain (g = 0.4): luma = CNN + 0.6 (faithful - CNN).
			const std::vector<uint16_t> grainy = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh, { cnn });
			int grainDiff = 0;
			for (size_t i = 0; i < grainy.size(); ++i)
			{
				if (i % 4 == 3)
					continue;
				const double expected = cnnBranch[i] + 0.6 * (static_cast<double>(faithful[i]) - cnnBranch[i]);
				grainDiff = std::max(grainDiff, static_cast<int>(std::lround(std::fabs(grainy[i] - expected))));
			}
			// Without the doubler the base is the source picture itself.
			const std::vector<uint16_t> plain = withModule(module, 1000.0f, 2000.0f, 0.0f, static_cast<float>(height), ow, oh, {});
			const int plainRatio = module.LastUpscaleRatio();
			const int plainDiff = maxDiff(plain, faithfulRef);
			// Gate: native size and a downscale are left alone.
			const std::vector<uint16_t> native = withModule(module, 0.0f, 0.001f, 0.0f, static_cast<float>(height), width, height, { cnn });
			const std::vector<uint16_t> nativeRef = render(width, height, { cnn }, &pl_filter_ewa_lanczossharp);
			const std::vector<uint16_t> down = withModule(module, 0.0f, 0.001f, 0.0f, static_cast<float>(height), width / 2, height / 2, { cnn });
			const std::vector<uint16_t> downRef = render(width / 2, height / 2, { cnn }, &pl_filter_ewa_lanczossharp);
			const std::wstring message = L"clean vs CNN " + std::to_wstring(cleanDiff) + L", faithful vs ewa " +
				std::to_wstring(faithfulDiff) + where + L", grain vs CNN + 0.6 (faithful - CNN) " + std::to_wstring(grainDiff) +
				L", no doubler: ratio " + std::to_wstring(plainRatio) + L" vs ewa " + std::to_wstring(plainDiff) +
				L" (16-bit codes); log: " + std::wstring(log.begin(), log.end());
			// The blend's output and the faithful branch are 16-bit float
			// textures: about 1/2048 of the value (32 of 65535 near white).
			Assert::IsTrue(cleanDiff <= 32, message.c_str());
			Assert::IsTrue(faithfulDiff <= 96 && plainDiff <= 96 && plainRatio == 1, message.c_str());
			Assert::IsTrue(grainDiff <= 96, message.c_str());
			Assert::AreEqual(0, maxDiff(native, nativeRef), (L"native size must be untouched; " + message).c_str());
			Assert::AreEqual(0, maxDiff(down, downRef), (L"a downscale must be untouched; " + message).c_str());

			// SuperRes strength follows the grain term (its vp_grain_strength,
			// alpha from the blend): with g = 0.4 its correction is scaled by 0.4.
			std::string superSource = LoadBundledShader("SSimSuperRes.glsl");
			Assert::IsFalse(superSource.empty());
			const pl_hook* superres = pl_mpv_user_shader_parse(gpu, superSource.data(), superSource.size());
			Assert::IsNotNull(superres);
			// On the plain upscale (no doubler), where SuperRes has work to do;
			// the blend sits before it (both POST_KERNEL).
			const std::vector<uint16_t> blendOnly = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh, {});
			const int superAlone = maxDiff(render(ow, oh, { superres }, &pl_filter_ewa_lanczossharp), faithfulRef);
			Assert::IsTrue(CustomHookChain::SetHookParameter(superres, "vp_grain_strength", 0.0));
			const std::vector<uint16_t> fullSuper = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, false, 0);
			Assert::IsTrue(CustomHookChain::SetHookParameter(superres, "vp_grain_strength", 1.0));
			const std::vector<uint16_t> scaledSuper = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, true, 0);
			double num = 0.0, den = 0.0;
			for (size_t i = 0; i < blendOnly.size(); ++i)
			{
				if (i % 4 == 3)
					continue;
				const double full = static_cast<double>(fullSuper[i]) - blendOnly[i];
				const double scaled = static_cast<double>(scaledSuper[i]) - blendOnly[i];
				num += full * scaled;
				den += full * full;
			}
			const double ratio = den > 0.0 ? num / den : 0.0;
			// Clean (g = 1): the scaled SuperRes is the full one.
			const std::vector<uint16_t> cleanScaled = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, true, 0);
			Assert::IsTrue(CustomHookChain::SetHookParameter(superres, "vp_grain_strength", 0.0));
			const std::vector<uint16_t> cleanFull = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, false, 0);
			// SuperRes after a doubler is covered by the chain matrix, not here
			// (planner 2026-10-12, not a gap): on these synthetic pictures its
			// correction after a doubler is at half-float rounding level (31 of
			// 65535 even after a sharpening doubler, without the blend), so it
			// cannot be measured; the order and the alpha handoff are checked
			// above on the plain upscale, and the matrix's refinements axis runs
			// real FSRCNNX + SSimSuperRes with the blend at the upscale scales.
			pl_mpv_user_shader_destroy(&superres);
			pl_mpv_user_shader_destroy(&cnn);
			const std::wstring superMessage = L"SuperRes correction with g = 0.4: " + std::to_wstring(ratio) +
				L" of the full one (correction energy " + std::to_wstring(den) + L"; SuperRes alone changes the plain upscale by " +
				std::to_wstring(superAlone) + L"); clean scaled vs full " +
				std::to_wstring(maxDiff(cleanScaled, cleanFull)) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(den > 0.0 && std::fabs(ratio - 0.4) < 0.05, superMessage.c_str());
			Assert::IsTrue(maxDiff(cleanScaled, cleanFull) <= 1, superMessage.c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(BitrateScaleRampAndAntiring)
		{
			// Delta 15 S8-2 / 2c: clean (0.8 and above) is today's output exactly.
			Assert::AreEqual(1.0, AdaptiveDownscale::BitrateScale(1.0, 0.4));
			Assert::AreEqual(1.0, AdaptiveDownscale::BitrateScale(0.8, 0.4));
			Assert::AreEqual(0.4, AdaptiveDownscale::BitrateScale(0.0, 0.4), 1e-9);
			Assert::AreEqual(0.4, AdaptiveDownscale::BitrateScale(0.2, 0.4), 1e-9);
			Assert::AreEqual(0.7, AdaptiveDownscale::BitrateScale(0.5, 0.4), 1e-9, L"halfway");
			Assert::AreEqual(0.7, AdaptiveDownscale::BitrateScale(0.51, 0.4), 1e-9, L"steps of 0.05: no churn");
			Assert::AreEqual(0.5f, AdaptiveDownscale::BitrateAntiring(0.5f, 1.0, 0.4), L"clean: anti-ringing unchanged");
			Assert::AreEqual(0.75f, AdaptiveDownscale::BitrateAntiring(0.5f, 0.4, 0.4), 1e-6f, L"floor: halfway to 1");
		}

		TEST_METHOD(AdaptiveDownscaleGrainMapFromTheVectors)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			static constexpr int width = 256, height = 128, dx = 8, dy = 4;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			AdaptiveDownscale::Module module;
			module.hostReadableMap = true;
			AdaptiveDownscale::Settings st;
			st.grainSmoothMs = 0.0f;       // the scalar follows each frame
			st.grainMapSmoothMs = 100.0f;  // the map settles in a few frames
			// The picture as RGB (grey from the luma), the render's source.
			auto rgbOf = [&](const DejudderFrame& f)
			{
				std::vector<uint16_t> px(static_cast<size_t>(width) * height * 4);
				for (size_t i = 0; i < f.luma.size(); ++i)
				{
					px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = f.luma[i];
					px[i * 4 + 3] = 65535;
				}
				pl_tex_params tp{};
				tp.w = width; tp.h = height; tp.format = srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			};
			auto render = [&](pl_tex src, uint64_t sequence, bool withField, int mode = 1)
			{
				// mode 1: measure + SSIM + blend; 0: SSIM alone.
				module.SetFrame(st, sequence, 1.0f / 24.0f);
				module.ClearGrainField();
				if (withField)
				{
					AdaptiveDownscale::GrainField gf;
					gf.error = warp.GrainError();
					gf.field = warp.Field();
					gf.frameTrust = warp.FrameTrust();
					gf.minTrustedShare = warp.GetSettings().minTrustedShare;
					gf.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
					module.SetGrainField(gf);
				}
				pl_tex_params tp{};
				tp.w = width / 2; tp.h = height / 2; tp.format = dstFmt; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
				const pl_hook* ssimOnly[1] = { ssim };
				params.hooks = mode == 1 ? hooks : ssimOnly;
				params.num_hooks = mode == 1 ? 3 : 1;
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> px(static_cast<size_t>(width / 2) * (height / 2));
				pl_tex_transfer_params dl{};
				dl.tex = target;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			std::string report;
			// Frames 0..7; each pair (f, f+1) estimated, frame f rendered with its field.
			for (int f = 0; f < 8; ++f)
			{
				const DejudderFrame a = GrainFrame(width, height, f, dx, dy, 0);
				const DejudderFrame b = GrainFrame(width, height, f + 1, dx, dy, 0);
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes, -1, true));
				pl_tex src = rgbOf(a);
				render(src, static_cast<uint64_t>(f + 1), true);
				render(src, static_cast<uint64_t>(f + 1), true);   // a repeat: the map stands
				pl_tex_destroy(gpu, &src);
			}
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"the blend did not run");
			Assert::IsTrue(module.LastMapState() == AdaptiveDownscale::MapState::Used, L"the map was not used");
			pl_tex map = module.GrainMap();
			Assert::IsNotNull(map);
			std::vector<float> texels(static_cast<size_t>(map->params.w) * map->params.h * 4);
			pl_tex_transfer_params d{};
			d.tex = map;
			d.ptr = texels.data();
			Assert::IsTrue(pl_tex_download(gpu, &d), L"map not readable");
			double clean = 0.0, noisy = 0.0;
			int nClean = 0, nNoisy = 0;
			const int bw = map->params.w, bh = map->params.h;
			for (int by = 1; by < bh - 1; ++by)
				for (int bx = 1; bx < bw - 1; ++bx)
				{
					if (bx == bw / 2 - 1 || bx == bw / 2)
						continue;
					const float* t = &texels[(static_cast<size_t>(by) * bw + bx) * 4];
					if (t[1] < 0.5f)
						continue;   // untrusted
					if (bx < bw / 2) { clean += t[0]; ++nClean; }
					else { noisy += t[0]; ++nNoisy; }
				}
			Assert::IsTrue(nClean > 10 && nNoisy > 10, L"too few trusted blocks in the map");
			clean /= nClean;
			noisy /= nNoisy;
			char line[200];
			std::snprintf(line, sizeof(line), "map: texture %.3f (%d blocks), noise %.3f (%d blocks), floor %.2f\n",
				clean, nClean, noisy, nNoisy, st.grainFloor);
			report += line;
			Assert::IsTrue(clean > 0.9 && noisy < st.grainFloor + 0.1, ExtendedRangeGpuProbe::ToWide(report).c_str());
			// The blend follows the map where the output shows it: on the
			// texture half (map 1) it is SSIM except at edges; on the noise
			// half (map at the floor) it moves towards faithful. Faithful is the
			// blend with w_base 0. Measured as the mean distance to SSIM per
			// half, against the scalar-only blend of the same frame.
			{
				const DejudderFrame a = GrainFrame(width, height, 7, dx, dy, 0);
				pl_tex src = rgbOf(a);
				const auto withMap = render(src, 8, true);          // the map stands for frame 8
				const auto ssimPx = render(src, 8, true, 0);
				AdaptiveDownscale::Settings saved = st;
				st.grainMap = false;
				const auto scalarPx = render(src, 8, true);
				st = saved;
				pl_tex_destroy(gpu, &src);
				double mapTex = 0.0, scalarTex = 0.0, mapNoise = 0.0, scalarNoise = 0.0;
				int nTex = 0, nNoise = 0;
				const int ow = width / 2, oh = height / 2;
				for (int y = 8; y < oh - 8; ++y)
					for (int x = 8; x < ow - 8; ++x)
					{
						if (x >= ow / 2 - 12 && x < ow / 2 + 12)
							continue;   // the boundary (bilinear between blocks)
						const size_t i = static_cast<size_t>(y) * ow + x;
						const double m = std::abs(withMap[i].g - ssimPx[i].g), sc = std::abs(scalarPx[i].g - ssimPx[i].g);
						if (x < ow / 2) { mapTex += m; scalarTex += sc; ++nTex; }
						else { mapNoise += m; scalarNoise += sc; ++nNoise; }
					}
				mapTex /= nTex; scalarTex /= nTex; mapNoise /= nNoise; scalarNoise /= nNoise;
				std::snprintf(line, sizeof(line), "blend vs SSIM: texture map %.3f scalar %.3f; noise map %.3f scalar %.3f (scalar g %.3f)\n",
					mapTex, scalarTex, mapNoise, scalarNoise, module.LastStats().grainScale);
				report += line;
				// Texture: the map keeps SSIM at least as much as the scalar;
				// noise: it leans away from SSIM at least as much; and the two
				// halves differ (the map is not one number).
				Assert::IsTrue(mapTex <= scalarTex + 0.05 && mapNoise >= scalarNoise - 0.05 && mapNoise > mapTex + 0.2,
					ExtendedRangeGpuProbe::ToWide(report).c_str());
			}
			// No field for the next source frame: the scalar.
			{
				const DejudderFrame a = GrainFrame(width, height, 8, dx, dy, 0);
				pl_tex src = rgbOf(a);
				render(src, 9, false);
				pl_tex_destroy(gpu, &src);
				Assert::IsTrue(module.LastMapState() == AdaptiveDownscale::MapState::NoField, L"no field: not the scalar");
			}
			// Map off: the scalar, nothing else.
			st.grainMap = false;
			{
				const DejudderFrame a = GrainFrame(width, height, 9, dx, dy, 0);
				pl_tex src = rgbOf(a);
				render(src, 10, true);
				pl_tex_destroy(gpu, &src);
				Assert::IsTrue(module.LastMapState() == AdaptiveDownscale::MapState::Off);
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			module.Release();
			pl_mpv_user_shader_destroy(&ssim);
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}
	};
}
