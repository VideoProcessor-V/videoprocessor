// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleStage.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapDispatch.h>
#include <VP-V/tests/SettingsTestSupport.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <set>

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
			// FIX-X5-CUT-P4: the cut event is the shared one (P4 through U7), not a private mean luma step. This harness has no
			// skeleton memo, so it states the cuts of its own scenes (dark -> grain, grain -> brighter) as the P4 verdict.
			std::set<uint64_t> cutSequences;
			module.SetCutSource([&cutSequences](uint64_t, uint64_t seq)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = cutSequences.count(seq) != 0;
				return v;
			});
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
			int texMax = 0, texBlendedVsFaithful = 0, texSsimVsFaithful = 0;
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
					if (x >= 136)   // low-contrast texture half: must follow SSIM, not faithful
					{
						texMax = std::max(texMax, std::abs(blended[i].g - ssimOnly[i].g));
						texBlendedVsFaithful = std::max(texBlendedVsFaithful, std::abs(blended[i].g - faithfulPx[i].g));
						texSsimVsFaithful = std::max(texSsimVsFaithful, std::abs(ssimOnly[i].g - faithfulPx[i].g));
					}
				}
			std::snprintf(line, sizeof(line),
				"w=1 vs SSIM max %d; edges vs faithful max %d (SSIM vs faithful %d); texture vs SSIM max %d (blended vs faithful %d, SSIM vs faithful %d)\n",
				ssimMax, edgeMax, edgeSsimVsFaithful, texMax, texBlendedVsFaithful, texSsimVsFaithful);
			report += line;
			// Edges: the edge map is interpolated between source pixels at a
			// transition, so up to 2 codes of SSIM remain there.
			// Texture half: blended must stay within 1 code of SSIM and closer to SSIM than faithful.
			Assert::IsTrue(ssimMax <= 1 && edgeMax <= 2 && edgeSsimVsFaithful >= 4 &&
				texMax <= 1 && (texSsimVsFaithful <= 1 || texMax < texBlendedVsFaithful),
				ExtendedRangeGpuProbe::ToWide(report).c_str());

			// 2. Grain: after a cut (dark -> grey noise) it snaps to the floor;
			// a clean field of the same mean recovers smoothly; a luma jump
			// (grey -> bright noise) snaps again.
			AdaptiveDownscale::Settings grain;
			render(dark, 256, 160, true, grain, false, true);
			cutSequences.insert(sequence + 1);
			render(grainy, 256, 160, true, grain, false, true);
			const auto noisy = grainNow(grainy, grain);
			render(clean, 256, 160, true, grain, false, true);
			const auto oneClean = grainNow(clean, grain);
			for (int i = 0; i < 120; ++i)
				render(clean, 256, 160, true, grain, false, true);
			const auto manyClean = grainNow(clean, grain);
			cutSequences.insert(sequence + 1);
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
			// State the brighter -> scene cut for P4 (FIX-X5-CUT-P4: matches Step 2's injected cut verdicts).
			cutSequences.insert(sequence + 1);
			render(scene, 256, 160, true, noGrain, false, true);
			const auto linearStats = grainNow(scene, noGrain);
			// Bug hunt 34 item 1: on D3D11 the statistics come through the non-blocking staging
			// ring (libplacebo's buffer readback blocks there), and still arrive one frame later.
			Assert::IsTrue(module.NativeStatsReadback(), L"D3D11: the statistics must be read back without waiting");
			Assert::AreEqual<uint64_t>(0, module.StatsDropped(), L"statistics frames dropped by the staging ring");
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
		// CF-2 B1 (HybridBlend.h): w = clamp(w_base (1 - e) g) x m, out = sharp + (1 - w)(alias_safe - sharp)
		// on luma. The identity / grain assertions run with the edge ramp forced to e = 0 on every pixel
		// (unchanged tolerances); the edge term is then checked with the ramp forced to e = 1 and e = 0.5.
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
			// CF-2 switch-over (WAIVER): the upscale form's grain term is kP10gGrainLevel only (no private scalar): a stand-in
			// level, N_b = 1 in every 16 x 16 source block (y/z/w unused by the blend), handed to the module as the stage would.
			// sqrt(1) x GrainLevelEnergyScale() (1.0, FIX-UPS-grain) = 1 sits inside every ramp below exactly where the old
			// private energy did: above 0.001 and -1, below 1000. Without the level the term is grain_floor (checked below).
			pl_fmt rgba32f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(rgba32f);
			const int levelW = (width + 15) / 16, levelH = (height + 15) / 16;
			std::vector<float> levelTexels(static_cast<size_t>(levelW) * levelH * 4, 0.0f);
			for (size_t i = 0; i < levelTexels.size(); i += 4)
				levelTexels[i] = 1.0f;
			pl_tex_params lp{};
			lp.w = levelW;
			lp.h = levelH;
			lp.format = rgba32f;
			lp.sampleable = true;
			lp.initial_data = levelTexels.data();
			pl_tex grainLevelTex = pl_tex_create(gpu, &lp);
			Assert::IsNotNull(grainLevelTex);
			bool publishGrainLevel = true;
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
			// e = 0 everywhere: smoothstep(1e6, 2e6, grad) = 0 for any 8-bit gradient. The ramps below force g
			// through the stand-in level (energy 1: > 0.001, and far below 1000).
			const float edgeOffLo = 1.0e6f, edgeOffHi = 2.0e6f;
			settings.edgeLo = edgeOffLo;
			settings.edgeHi = edgeOffHi;
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
				module.ClearGrainLevel();
				if (publishGrainLevel)
				{
					AdaptiveDownscale::GrainLevelSource level;
					level.level = grainLevelTex;
					level.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
					module.SetGrainLevel(level);
				}
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

			// Clean (g = 1, e = 0, w = 1): exactly the CNN branch, edges included.
			AdaptiveDownscale::Module module;
			const std::vector<uint16_t> clean = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh, { cnn });
			Assert::AreEqual(2, module.LastUpscaleRatio(), L"the doubler's output should be averaged to the source");
			Assert::IsTrue(module.PublishedGrainUsed(), L"the blend read the stand-in grain level");
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
			// No grain level (the switch-over's alias-safe lean): g = grain_floor whatever the ramp: the clean ramp
			// (g = 1 if published) gives the grain picture.
			publishGrainLevel = false;
			const std::vector<uint16_t> unpublished = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh, { cnn });
			publishGrainLevel = true;
			Assert::IsFalse(module.PublishedGrainUsed(), L"no level: not published");
			int floorDiff = 0;
			for (size_t i = 0; i < unpublished.size(); ++i)
			{
				if (i % 4 == 3)
					continue;
				const double expected = cnnBranch[i] + 0.6 * (static_cast<double>(faithful[i]) - cnnBranch[i]);
				floorDiff = (std::max)(floorDiff, static_cast<int>(std::lround(std::fabs(unpublished[i] - expected))));
			}
			Assert::IsTrue(floorDiff <= 96, (L"no grain level vs CNN + 0.6 (faithful - CNN): " + std::to_wstring(floorDiff)).c_str());
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

			// CF-2 B1 edge term (FIX-UPS-edge change (a)): edge pixels move from the sharp branch (here the CNN)
			// toward the alias-safe branch (`faithful`, w_base 0) by 1 - w, w = clamp(w_base (1 - e) g) x m (m = 1:
			// no vectors, no floor). The ramp forces e on every pixel:
			//   e = 1:   smoothstep(-2, -1, grad) = 1 for any gradient >= 0, so w = 0 whatever g: out = alias-safe.
			//   e = 0.5: smoothstep(-1e6, 1e6, grad) = 0.5 within 3e-4 (16-bit float edge texture: within 1e-3).
			// The stand-in level's energy 1: grain ramp 1000..2000 gives g = 1, -2..-1 gives g = the floor 0.4
			// (switch-over WAIVER: was the private energy 0 with no flat pixel). Predicted 1 - w (w_base 1): e = 1 -> 1 (g = 1 and g = 0.4);
			// e = 0.5 -> 0.5 (g = 1), 0.8 (g = 0.4). With the edge term removed these would be 0, 0.6, 0 and 0.6.
			{
				auto withEdge = [&](float edgeLo, float edgeHi, float grainLo, float grainHi)
				{
					settings.edgeLo = edgeLo;
					settings.edgeHi = edgeHi;
					const std::vector<uint16_t> out = withModule(module, grainLo, grainHi, 1.0f, static_cast<float>(height), ow, oh, { cnn });
					settings.edgeLo = edgeOffLo;
					settings.edgeHi = edgeOffHi;
					return out;
				};
				// out against sharp + toward (alias_safe - sharp), every colour channel (the luma difference moves all three).
				auto checkToward = [&](const std::vector<uint16_t>& out, double toward, int tolerance, const wchar_t* label)
				{
					int worst = 0, wrongWay = 0, overshoot = 0, moving = 0;
					double num = 0.0, den = 0.0;
					for (size_t i = 0; i < out.size(); ++i)
					{
						if (i % 4 == 3)
							continue;
						const double sharp = cnnBranch[i], safe = faithful[i];
						const double moved = out[i] - sharp, span = safe - sharp;
						worst = std::max(worst, static_cast<int>(std::lround(std::fabs(out[i] - (sharp + toward * span)))));
						num += moved * span;
						den += span * span;
						if (std::fabs(span) >= 512.0)
						{
							++moving;
							if (moved * span <= 0.0)
								++wrongWay;       // not moved, or moved away from the alias-safe branch
							if (std::fabs(moved) > std::fabs(span) + tolerance)
								++overshoot;      // moved past the alias-safe branch
						}
					}
					const double fraction = den > 0.0 ? num / den : 0.0;
					const std::wstring m = std::wstring(label) + L": predicted 1 - w " + std::to_wstring(toward) + L", measured " +
						std::to_wstring(fraction) + L", max vs prediction " + std::to_wstring(worst) + L" (tolerance " +
						std::to_wstring(tolerance) + L"), moving channels " + std::to_wstring(moving) + L", wrong way " +
						std::to_wstring(wrongWay) + L", overshoot " + std::to_wstring(overshoot) + L" (16-bit codes)";
					Assert::IsTrue(moving > 0 && den > 0.0, m.c_str());
					Assert::IsTrue(worst <= tolerance, m.c_str());
					Assert::IsTrue(std::fabs(fraction - toward) < 0.02, m.c_str());
					Assert::AreEqual(0, wrongWay, m.c_str());
					Assert::AreEqual(0, overshoot, m.c_str());
				};
				// e = 1, w = 0 (as w_base 0): the alias-safe branch, at the identity tolerance.
				checkToward(withEdge(-2.0f, -1.0f, 1000.0f, 2000.0f), 1.0, 32, L"e = 1, g = 1");
				checkToward(withEdge(-2.0f, -1.0f, -2.0f, -1.0f), 1.0, 32, L"e = 1, g = 0.4");
				// e = 0.5: a mixed expectation, at the grain assertion's tolerance.
				checkToward(withEdge(-1.0e6f, 1.0e6f, 1000.0f, 2000.0f), 0.5, 96, L"e = 0.5, g = 1 (w = 0.5)");
				checkToward(withEdge(-1.0e6f, 1.0e6f, -2.0f, -1.0f), 0.8, 96, L"e = 0.5, g = 0.4 (w = 0.2)");
			}

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
			pl_tex_destroy(gpu, &grainLevelTex);
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

		// ---- X5-GRAIN-P10 (gate cb-u1 r1 MUST 4 / 5 / 6, SHOULD 5): the shared grain source on the adaptive downscale ----------
		// A downscale through the real hook chain (measure, SSIM, blend) with white-noise sources, so the private Laplacian
		// energy and the P10-derived energy have a known common value. The grain ramp is turned into an energy readout: the
		// smoothing is off (grain_smooth_ms 1) and a cut (dark -> noise) snaps, so grain_scale = the ramp's target and the
		// energy is recovered by inverting grain_scale = floor + (1 - floor) (1 - smoothstep(lo, hi, energy)).
		struct GrainRig
		{
			static constexpr int kSw = 384, kSh = 240, kBw = 24, kBh = 15;   // source size and its 16 px block grid
			TargetLutGpuFixture fixture;
			pl_gpu gpu = nullptr;
			const pl_hook* ssim = nullptr;
			pl_fmt srcFmt = nullptr, dstFmt = nullptr, gridFmt = nullptr;
			std::vector<pl_tex> owned;

			~GrainRig()
			{
				for (pl_tex& t : owned)
					pl_tex_destroy(gpu, &t);
				if (ssim)
					pl_mpv_user_shader_destroy(&ssim);
			}

			bool Init()
			{
				if (!fixture.Create())
					return false;
				gpu = fixture.Gpu();
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				if (source.empty())
					return false;
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
				dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				gridFmt = pl_find_named_fmt(gpu, "rgba32f");
				return ssim && srcFmt && dstFmt && gridFmt;
			}

			pl_tex Grey(const std::vector<double>& codes)
			{
				std::vector<uint16_t> px(static_cast<size_t>(kSw) * kSh * 4);
				for (size_t i = 0; i < codes.size(); ++i)
				{
					const double c = (std::min)(255.0, (std::max)(0.0, codes[i]));
					const uint16_t v = static_cast<uint16_t>(std::lround(c * 257.0));
					px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
					px[i * 4 + 3] = 65535;
				}
				pl_tex_params tp{};
				tp.w = kSw; tp.h = kSh; tp.format = srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				pl_tex t = pl_tex_create(gpu, &tp);
				owned.push_back(t);
				return t;
			}

			pl_tex Grid(const std::vector<float>& rgba, int w, int h)
			{
				pl_tex_params tp{};
				tp.w = w; tp.h = h; tp.format = gridFmt; tp.sampleable = true; tp.initial_data = rgba.data();
				pl_tex t = pl_tex_create(gpu, &tp);
				owned.push_back(t);
				return t;
			}

			// Per block: N (the grain variance in 8-bit codes squared, P10 .x), meanY, coverage x off-edge share.
			AdaptiveDownscale::GrainSource Source(double sigma, int w = kBw, int h = kBh, int s4w = kBw, int s4h = kBh)
			{
				const size_t n = static_cast<size_t>(w) * h;
				std::vector<float> p10(n * 4, 0.0f), s0(n * 4, 0.0f), s4(static_cast<size_t>(s4w) * s4h * 4, 0.0f);
				for (size_t i = 0; i < n; ++i)
				{
					p10[i * 4] = static_cast<float>(sigma * sigma);
					s0[i * 4] = 128.0f;
				}
				for (size_t i = 0; i < static_cast<size_t>(s4w) * s4h; ++i)
					s4[i * 4] = s4[i * 4 + 1] = 1.0f;
				AdaptiveDownscale::GrainSource g;
				g.p10Blocks = Grid(p10, w, h);
				g.p11Stats0 = Grid(s0, w, h);
				g.p11Stats4 = Grid(s4, s4w, s4h);
				g.blockPx = 16;
				g.codeToEightBit = 1.0f;
				g.sourceCrop = pl_rect2df{ 0.0f, 0.0f, static_cast<float>(kSw), static_cast<float>(kSh) };
				return g;
			}

			static AdaptiveDownscale::Settings Ramp()
			{
				AdaptiveDownscale::Settings st;
				st.edgeLo = 250.0f; st.edgeHi = 255.0f;   // every pixel is flat: the private energy is the mean |Laplacian|
				st.shadowBias = 0.0f;                     // no shadow weighting in either estimate
				st.grainLo = 2.0f; st.grainHi = 16.0f; st.grainFloor = 0.4f;
				st.grainSmoothMs = 1.0f;
				return st;
			}

			void Render(AdaptiveDownscale::Module& module, uint64_t& sequence, pl_tex src, const AdaptiveDownscale::Settings& st,
				bool gammaLight, bool advance, const AdaptiveDownscale::GrainSource* grain)
			{
				pl_tex_params tp{};
				tp.w = 256; tp.h = 160; tp.format = dstFmt; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = gammaLight;   // true: the hook sees the gamma code domain (mode 0)
				if (advance)
					++sequence;
				module.SetFrame(st, sequence, 1.0f / 24.0f);
				if (grain)
					module.SetGrainSource(*grain);
				else
					module.ClearGrainSource();
				const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
				params.hooks = hooks;
				params.num_hooks = 3;
				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				pl_tex_destroy(gpu, &target);
				Assert::IsTrue(ok, L"the render ran");
			}

			struct Result
			{
				AdaptiveDownscale::Stats stats;
				AdaptiveDownscale::GrainSourceUse use = AdaptiveDownscale::GrainSourceUse::NotSupplied;
				bool fromShared = false;
			};

			// dark -> noise (a cut: grain_scale snaps to the target), two more frames for the read-back to arrive.
			Result Measure(pl_tex dark, pl_tex noisy, const AdaptiveDownscale::Settings& st, bool gammaLight,
				const AdaptiveDownscale::GrainSource* grain)
			{
				AdaptiveDownscale::Module module;
				uint64_t sequence = 0;
				Render(module, sequence, dark, st, gammaLight, true, grain);
				Render(module, sequence, noisy, st, gammaLight, true, grain);
				Render(module, sequence, noisy, st, gammaLight, false, grain);
				Render(module, sequence, noisy, st, gammaLight, false, grain);
				Assert::IsTrue(module.Active() && module.Failures() == 0, L"the blend ran without a GPU failure");
				Assert::IsTrue(module.LastStats().valid, L"the statistics arrived");
				Result r;
				r.stats = module.LastStats();
				r.use = module.LastGrainSourceUse();
				r.fromShared = module.LastGrainFromSharedProducts();
				module.Release();
				Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"no hook was disabled");
				return r;
			}
		};

		// Seeded white Gaussian noise around 128 codes (Box-Muller on an LCG); sigmaOut = the field's own standard deviation.
		static std::vector<double> WhiteNoiseField(double sigma, uint32_t seed, double& sigmaOut)
		{
			std::vector<double> v(static_cast<size_t>(GrainRig::kSw) * GrainRig::kSh);
			uint32_t state = seed;
			auto uniform = [&state]() { state = state * 1664525u + 1013904223u; return (static_cast<double>(state >> 8) + 0.5) / 16777216.0; };
			double sum = 0.0;
			for (double& x : v)
			{
				x = 128.0 + sigma * std::sqrt(-2.0 * std::log(uniform())) * std::cos(6.283185307179586 * uniform());
				sum += x;
			}
			const double mean = sum / static_cast<double>(v.size());
			double ss = 0.0;
			for (double x : v) ss += (x - mean) * (x - mean);
			sigmaOut = std::sqrt(ss / static_cast<double>(v.size()));
			return v;
		}

		// The energy the grain ramp saw: grain_scale = floor + (1 - floor) (1 - smoothstep(lo, hi, energy)), inverted.
		static double EnergyOfScale(float scale, const AdaptiveDownscale::Settings& st)
		{
			const double u = (std::min)(1.0, (std::max)(0.0, 1.0 - (static_cast<double>(scale) - st.grainFloor) / (1.0 - st.grainFloor)));
			double lo = 0.0, hi = 1.0;
			for (int i = 0; i < 60; ++i)
			{
				const double t = 0.5 * (lo + hi);
				if (t * t * (3.0 - 2.0 * t) < u) lo = t; else hi = t;
			}
			return st.grainLo + (st.grainHi - st.grainLo) * 0.5 * (lo + hi);
		}

		TEST_METHOD(AdaptiveGrainSourceUseIsReportedForEveryCase)
		{
			using AdaptiveDownscale::GrainSourceUse;
			GrainRig rig;
			Assert::IsTrue(rig.Init(), L"the WARP test device, formats and SSimDownscaler.glsl are available");
			double sigma = 0.0;
			pl_tex noisy = rig.Grey(WhiteNoiseField(2.0, 12345u, sigma));
			pl_tex dark = rig.Grey(std::vector<double>(static_cast<size_t>(GrainRig::kSw) * GrainRig::kSh, 20.0));
			const AdaptiveDownscale::Settings st = GrainRig::Ramp();
			const AdaptiveDownscale::GrainSource good = rig.Source(sigma);

			// NotSupplied: no source at all (the design when the host offers none).
			const GrainRig::Result own = rig.Measure(dark, noisy, st, true, nullptr);
			Assert::IsTrue(own.use == GrainSourceUse::NotSupplied && !own.fromShared, L"no source: NotSupplied");

			// Shared: a valid source in the gamma code domain.
			const GrainRig::Result shared = rig.Measure(dark, noisy, st, true, &good);
			Assert::IsTrue(shared.use == GrainSourceUse::Shared && shared.fromShared, L"a valid source in the code domain: Shared");

			// NoUsableBlock: bound, but every block is outside the crop (an empty crop) ...
			AdaptiveDownscale::GrainSource outside = good;
			outside.sourceCrop = pl_rect2df{};
			const GrainRig::Result outsideResult = rig.Measure(dark, noisy, st, true, &outside);
			Assert::IsTrue(outsideResult.use == GrainSourceUse::NoUsableBlock && !outsideResult.fromShared,
				L"every block outside the crop: NoUsableBlock, never Shared");
			Assert::IsTrue(std::fabs(outsideResult.stats.grainScale - own.stats.grainScale) < 1e-3f,
				L"no usable block: the energy is the private estimate (the same grain scale as the unsupplied run)");
			// ... or every block vetoed (zero flat coverage).
			AdaptiveDownscale::GrainSource vetoed = rig.Source(sigma);
			{
				const std::vector<float> zero(static_cast<size_t>(GrainRig::kBw) * GrainRig::kBh * 4, 0.0f);
				vetoed.p11Stats4 = rig.Grid(zero, GrainRig::kBw, GrainRig::kBh);
			}
			const GrainRig::Result vetoedResult = rig.Measure(dark, noisy, st, true, &vetoed);
			Assert::IsTrue(vetoedResult.use == GrainSourceUse::NoUsableBlock && !vetoedResult.fromShared,
				L"every block vetoed (no flat coverage): NoUsableBlock, never Shared");
			Assert::IsTrue(std::fabs(vetoedResult.stats.grainScale - own.stats.grainScale) < 1e-3f,
				L"all blocks vetoed: the energy is the private estimate");

			// MeasureDomain: a valid source, but the measure domain is linear light (P10 N is in source codes).
			const GrainRig::Result ownLinear = rig.Measure(dark, noisy, st, false, nullptr);
			const GrainRig::Result linear = rig.Measure(dark, noisy, st, false, &good);
			Assert::IsTrue(ownLinear.use == GrainSourceUse::NotSupplied, L"linear light, no source: NotSupplied");
			Assert::IsTrue(linear.use == GrainSourceUse::MeasureDomain && !linear.fromShared,
				L"a valid source in a linear measure domain is refused: MeasureDomain");
			Assert::IsTrue(std::fabs(linear.stats.grainScale - ownLinear.stats.grainScale) < 1e-3f,
				L"the refused source leaves the private estimate in force");

			// Grid mismatch: the three textures do not share one block grid. Valid() is false and the module ignores it.
			const AdaptiveDownscale::GrainSource mismatched = rig.Source(sigma, GrainRig::kBw, GrainRig::kBh, GrainRig::kBw / 2, GrainRig::kBh / 2);
			Assert::IsFalse(mismatched.SameGrid(), L"the coverage texture has another block grid");
			Assert::IsFalse(mismatched.Valid(), L"a grid mismatch is not a valid source");
			const GrainRig::Result mismatchedResult = rig.Measure(dark, noisy, st, true, &mismatched);
			Assert::IsTrue(mismatchedResult.use == GrainSourceUse::NotSupplied && !mismatchedResult.fromShared,
				L"a source with mismatched grids is not bound: NotSupplied");
			Assert::IsTrue(std::fabs(mismatchedResult.stats.grainScale - own.stats.grainScale) < 1e-3f,
				L"a grid mismatch leaves the private estimate in force");
		}

		TEST_METHOD(AdaptiveGrainSourceNonFiniteBlocksAreVetoed)
		{
			// A NaN, an infinity or a negative level in a P10 block, or a NaN in P11's mean luma or coverage, vetoes that block
			// (the energy is built from the other blocks); it never reaches the ramp. The planted blocks lie inside the crop.
			using AdaptiveDownscale::GrainSourceUse;
			GrainRig rig;
			Assert::IsTrue(rig.Init(), L"the WARP test device, formats and SSimDownscaler.glsl are available");
			double sigma = 0.0;
			pl_tex noisy = rig.Grey(WhiteNoiseField(2.0, 777u, sigma));
			pl_tex dark = rig.Grey(std::vector<double>(static_cast<size_t>(GrainRig::kSw) * GrainRig::kSh, 20.0));
			const AdaptiveDownscale::Settings st = GrainRig::Ramp();
			const double level = 1.5 * sigma;   // a level that differs from the private estimate, so the P10 path is what is read
			const size_t nb = static_cast<size_t>(GrainRig::kBw) * GrainRig::kBh;
			auto block = [](int bx, int by) { return (static_cast<size_t>(by) * GrainRig::kBw + bx) * 4; };

			const AdaptiveDownscale::GrainSource control = rig.Source(level);
			const GrainRig::Result own = rig.Measure(dark, noisy, st, true, nullptr);
			const GrainRig::Result clean = rig.Measure(dark, noisy, st, true, &control);
			Assert::IsTrue(clean.use == GrainSourceUse::Shared, L"control: the P10 path is read");
			Assert::IsTrue(std::fabs(clean.stats.grainScale - own.stats.grainScale) > 0.02f, L"control: the P10 level changes the energy (the path is really used)");

			const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
			std::vector<float> p10(nb * 4, 0.0f), s0(nb * 4, 0.0f), s4(nb * 4, 0.0f);
			for (size_t i = 0; i < nb; ++i)
			{
				p10[i * 4] = static_cast<float>(level * level);
				s0[i * 4] = 128.0f;
				s4[i * 4] = s4[i * 4 + 1] = 1.0f;
			}
			p10[block(3, 3)] = nan;              // NaN grain level
			p10[block(4, 4)] = inf;              // +inf grain level
			p10[block(5, 5)] = -inf;             // -inf grain level
			p10[block(6, 6)] = -4.0f;            // negative variance
			s0[block(7, 7)] = nan;               // NaN mean luma
			s0[block(8, 8)] = inf;               // +inf mean luma
			s4[block(9, 9)] = nan;               // NaN coverage
			s4[block(10, 10) + 1] = -inf;        // -inf off-edge share
			// Gate cb-u1 r2 SHOULD 1: the P11-planted blocks carry a distinct P10 level (4 x the level, 16 x the variance), so a
			// block whose mean-luma or coverage veto failed would move the energy by far more than the 1e-4 tolerance below
			// (with shadow_bias 0 a uniform level made a non-vetoed P11 block invisible).
			for (const size_t b : { block(7, 7), block(8, 8), block(9, 9), block(10, 10) })
				p10[b] = static_cast<float>(16.0 * level * level);
			AdaptiveDownscale::GrainSource planted = control;
			planted.p10Blocks = rig.Grid(p10, GrainRig::kBw, GrainRig::kBh);
			planted.p11Stats0 = rig.Grid(s0, GrainRig::kBw, GrainRig::kBh);
			planted.p11Stats4 = rig.Grid(s4, GrainRig::kBw, GrainRig::kBh);
			Assert::IsTrue(planted.Valid(), L"the planted source is a valid-shaped source");
			const GrainRig::Result r = rig.Measure(dark, noisy, st, true, &planted);
			Assert::IsTrue(r.use == GrainSourceUse::Shared, L"the unplanted blocks are still usable: Shared");
			Assert::IsTrue(std::isfinite(r.stats.grainScale) && r.stats.grainScale >= st.grainFloor - 1e-4f && r.stats.grainScale <= 1.0f + 1e-4f,
				L"the grain scale stays finite and inside the ramp");
			Assert::IsTrue(std::fabs(r.stats.grainScale - clean.stats.grainScale) < 1e-4f,
				L"the vetoed blocks do not move the energy (a uniform level over the other blocks; the P11-vetoed ones carry 4 x the level)");
		}

		TEST_METHOD(AdaptiveGrainSourceEnergyMatchesThePrivateEstimateOnWhiteNoise)
		{
			// FIX-X5-GRAIN-P10: on i.i.d. Gaussian noise of standard deviation s the 4-neighbour Laplacian 4e - (b + d + f + h)
			// has variance 20 s^2 and E|x| = sqrt(2/pi) std, so the private mean |Laplacian| is kEdgeEnergyPerSigma x s
			// (3.5682482 s). The P10 path is that constant times N^(1/2); with N = s^2 both must read the same energy.
			// Tolerance from the derivation: the relative standard error of a mean of |L| is sqrt(pi/2 - 1) / sqrt(N_eff) with
			// N_eff >= N / 5 (each Laplacian shares its 5 taps with at most 4 neighbours) = 0.755 / sqrt(92160 / 5) = 0.56%,
			// so 5 standard errors = 2.8% < 3%. The P10 path itself is exact up to float rounding of the readout (0.5%).
			using AdaptiveDownscale::GrainSourceUse;
			GrainRig rig;
			Assert::IsTrue(rig.Init(), L"the WARP test device, formats and SSimDownscaler.glsl are available");
			pl_tex dark = rig.Grey(std::vector<double>(static_cast<size_t>(GrainRig::kSw) * GrainRig::kSh, 20.0));
			const AdaptiveDownscale::Settings st = GrainRig::Ramp();
			for (double target : { 2.0, 3.0 })
			{
				double sigma = 0.0;
				pl_tex noisy = rig.Grey(WhiteNoiseField(target, static_cast<uint32_t>(1000.0 * target) + 7u, sigma));
				const double expected = AdaptiveDownscale::kEdgeEnergyPerSigma * sigma;
				const AdaptiveDownscale::GrainSource source = rig.Source(sigma);
				const GrainRig::Result own = rig.Measure(dark, noisy, st, true, nullptr);
				const GrainRig::Result shared = rig.Measure(dark, noisy, st, true, &source);
				Assert::IsTrue(shared.use == GrainSourceUse::Shared, L"the P10 path was read");
				const double ePrivate = EnergyOfScale(own.stats.grainScale, st), eShared = EnergyOfScale(shared.stats.grainScale, st);
				const std::string report = "sigma " + std::to_string(sigma) + ": expected " + std::to_string(expected) + ", private " +
					std::to_string(ePrivate) + ", shared " + std::to_string(eShared);
				Logger::WriteMessage(report.c_str());
				Assert::IsTrue(expected > st.grainLo + 2.0 && expected < st.grainHi - 2.0, L"the energy sits inside the ramp (readable)");
				Assert::IsTrue(std::fabs(ePrivate - expected) <= 0.03 * expected, ExtendedRangeGpuProbe::ToWide("private Laplacian energy vs sqrt(20) sqrt(2/pi) sigma: " + report).c_str());
				Assert::IsTrue(std::fabs(eShared - expected) <= 0.005 * expected, ExtendedRangeGpuProbe::ToWide("P10 energy vs the constant: " + report).c_str());
				Assert::IsTrue(std::fabs(eShared - ePrivate) <= 0.03 * expected, ExtendedRangeGpuProbe::ToWide("P10 energy vs private energy: " + report).c_str());
			}
		}

		TEST_METHOD(AdaptiveGrainSourceStageStatusAndFallbackForEveryCase)
		{
			// Gate cb-u1 r2 MUST 4 (r1 MUST 5 / SHOULD 6): the stage, driven as the host drives it (Stage::Register with a Host, the
			// OutputGlueStagesGpuTests pattern), shows exactly one grain status line and the GrainSourceFallback decision of each
			// case after a real scalar grain pass: no callback, a valid source, an empty crop (no usable block), a grid
			// mismatch, and a valid source in a linear measure domain. The grain decision owns the bare id (a classical cause);
			// the temporal-detail read is a separate, cause-keyed entry (gate cb-u1 r3 MUST 1).
			GrainRig rig;
			Assert::IsTrue(rig.Init(), L"the WARP test device, formats and SSimDownscaler.glsl are available");
			double sigma = 0.0;
			pl_tex noisy = rig.Grey(WhiteNoiseField(2.0, 4242u, sigma));
			pl_tex dark = rig.Grey(std::vector<double>(static_cast<size_t>(GrainRig::kSw) * GrainRig::kSh, 20.0));
			const AdaptiveDownscale::Settings st = GrainRig::Ramp();
			const AdaptiveDownscale::GrainSource good = rig.Source(sigma);
			AdaptiveDownscale::GrainSource outside = good;
			outside.sourceCrop = pl_rect2df{};
			const AdaptiveDownscale::GrainSource mismatched = rig.Source(sigma, GrainRig::kBw, GrainRig::kBh, GrainRig::kBw / 2, GrainRig::kBh / 2);
			Assert::IsFalse(mismatched.Valid(), L"precondition: the mismatched source is not valid");

			struct Outcome
			{
				std::vector<std::string> grainLines;
				FallbackCatalog::Decision grain, td;
				bool active = false;
				bool ran = false;   // the scalar grain pass ran without a failure (r3 SHOULD 1: no read-back needed)
				AdaptiveDownscale::GrainSourceUse use = AdaptiveDownscale::GrainSourceUse::NotSupplied;
			};
			// dark -> noise (a cut), then two repeats of the noisy frame for the read-back, as GrainRig::Measure.
			// frameCount 1 (r3 SHOULD 1): one bound frame only, so no read-back of a bound frame exists yet (Pending).
			auto run = [&](const std::function<AdaptiveDownscale::GrainSource()>& source, bool gammaLight, int frameCount = 4) -> Outcome
			{
				AdaptiveDownscale::Module module;
				AdaptiveDownscaleStage::Stage stage{ module };
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping(); };
				host.ssimInChain = []() { return true; };
				host.motion = []() { return AdaptiveDownscaleStage::MotionSource(); };
				host.publishStatus = []() {};
				host.grainSource = source;   // empty = the host offers no shared products
				double weight = 1.0;
				bool grainWanted = false, noSsim = false;
				const pl_tex inputs[4] = { dark, noisy, noisy, noisy };
				const uint64_t sequences[4] = { 1, 2, 2, 2 };
				for (int i = 0; i < frameCount && i < 4; ++i)
				{
					pl_tex_params tp{};
					tp.w = 256; tp.h = 160; tp.format = rig.dstFmt; tp.renderable = true; tp.host_readable = true;
					pl_tex target = pl_tex_create(rig.gpu, &tp);
					pl_frame image = TargetLutGpuFixture::MakeRgbFrame(inputs[i]);
					image.color.transfer = PL_COLOR_TRC_GAMMA22;
					pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
					out.color.transfer = PL_COLOR_TRC_GAMMA22;
					pl_render_params params = pl_render_default_params;
					params.dither_params = nullptr;
					params.error_diffusion = nullptr;
					params.downscaler = &pl_filter_mitchell;
					params.disable_linear_scaling = gammaLight;   // true: the hook sees the gamma code domain (mode 0)
					AdaptiveDownscaleStage::Frame f;
					f.adaptiveDownscale = true;
					f.adaptive = &st;
					f.lowBitrateOff = true;
					f.fboFloat32 = 0;
					f.sourceSequence = sequences[i];
					f.nominalSourceRateHz = 24.0;
					f.sourceCrop = { 0.0f, 0.0f, static_cast<float>(GrainRig::kSw), static_cast<float>(GrainRig::kSh) };
					f.targetCrop = { 0.0f, 0.0f, 256.0f, 160.0f };
					std::vector<const pl_hook*> list = { rig.ssim };
					stage.Register(f, host, { weight, grainWanted, noSsim }, list);
					Assert::IsTrue(list.size() == 3 && list.front() == module.Measure() && list.back() == module.Blend(),
						L"the stage registered measure, SSIM, blend");
					params.hooks = list.data();
					params.num_hooks = static_cast<int>(list.size());
					const bool ok = pl_render_image(rig.fixture.Renderer(), &image, &out, &params);
					pl_gpu_finish(rig.gpu);
					pl_tex_destroy(rig.gpu, &target);
					Assert::IsTrue(ok, L"the render ran");
				}
				Outcome o;
				o.active = module.Active() && module.Failures() == 0 && module.LastStats().valid;
				o.ran = module.Active() && module.Failures() == 0;
				o.use = module.LastGrainSourceUse();
				for (const std::string& line : stage.StatusLines())
					if (line.rfind("Adaptive grain:", 0) == 0) o.grainLines.push_back(line);
				o.grain = stage.GrainSourceFallback();
				o.td = stage.TemporalDetailFallback();
				module.Release();
				Assert::AreEqual(0, pl_renderer_get_errors(rig.fixture.Renderer()).num_disabled_hooks, L"no hook was disabled");
				return o;
			};
			struct Case
			{
				const char* name;
				std::function<AdaptiveDownscale::GrainSource()> source;
				bool gammaLight;
				const char* line;
				bool engaged;
				const char* reason;
			};
			const std::vector<Case> cases = {
				{ "no callback", nullptr, true,
					"Adaptive grain: own estimate (the shared grain products are not supplied to the scaler)", false, "" },
				{ "valid", [&]() { return good; }, true,
					"Adaptive grain: from the shared grain split (P10) and region statistics (P11)", false, "" },
				{ "empty crop", [&]() { return outside; }, true,
					"Adaptive grain: own estimate (no usable shared grain block on the last read-back frame)", true,
					"grain: no usable shared block" },
				{ "grid mismatch", [&]() { return mismatched; }, true,
					"Adaptive grain: own estimate (the shared grain products are not supplied to the scaler)", true,
					"grain: the offered shared products are invalid (missing or grid mismatch)" },
				{ "linear measure domain", [&]() { return good; }, false,
					"Adaptive grain: own estimate (measure domain is linear / HDR; the shared products are in source codes)", true,
					"grain: measure domain is linear / HDR" },
			};
			for (const Case& c : cases)
			{
				const Outcome o = run(c.source, c.gammaLight);
				const std::wstring name = ExtendedRangeGpuProbe::ToWide(c.name);
				Assert::IsTrue(o.active, (name + L": the scalar grain pass ran and its statistics were read back").c_str());
				Assert::AreEqual(size_t(1), o.grainLines.size(), (name + L": exactly one grain status line").c_str());
				Assert::AreEqual(std::string(c.line), o.grainLines[0], (name + L": the grain status line").c_str());
				Assert::AreEqual(std::string("product.adaptive_downscale"), o.grain.id, (name + L": the fallback id").c_str());
				Assert::IsTrue(c.engaged == o.grain.engaged, (name + L": GrainSourceFallback engaged / released").c_str());
				Assert::AreEqual(std::string(c.reason), o.grain.reason, (name + L": the GrainSourceFallback reason").c_str());
				// Two entries: the temporal-detail read is not engaged in reset 1, and it never uses the bare id (r3 MUST 1).
				Assert::IsFalse(o.td.engaged, (name + L": the temporal-detail read path is not engaged").c_str());
			}
			{
				// r3 SHOULD 1: a valid source on the FIRST frame. The products are bound but no frame that bound them has been read
				// back yet (PollStats runs before the pass; a fresh module has no statistics): Pending, never a transient
				// NoUsableBlock, and no downgrade is claimed. Before f41a2c1c5 this read NoUsableBlock and engaged the fallback.
				const Outcome o = run([&]() { return good; }, true, 1);
				Assert::IsTrue(o.ran, L"valid, first frame: the scalar grain pass ran");
				Assert::IsTrue(o.use == AdaptiveDownscale::GrainSourceUse::Pending, L"valid, first frame: LastGrainSourceUse is Pending");
				Assert::AreEqual(size_t(1), o.grainLines.size(), L"valid, first frame: exactly one grain status line");
				Assert::AreEqual(std::string("Adaptive grain: shared grain split (P10) bound; waiting for the first read-back of a bound frame"),
					o.grainLines[0], L"valid, first frame: the Pending status line");
				Assert::AreEqual(std::string("product.adaptive_downscale"), o.grain.id, L"valid, first frame: the fallback id");
				Assert::IsFalse(o.grain.engaged, L"valid, first frame: GrainSourceFallback is Released (nothing dropped yet)");
			}
			Assert::IsTrue(TdProducts::FallbackKey(TdProducts::Consumer::AdaptiveScaler, TdProducts::kCauseAbsent) != std::string("product.adaptive_downscale"),
				L"the temporal-detail read is keyed by cause, never under the grain decision's bare id");
		}

		TEST_METHOD(UpscaleDoesNotEmitDownscaleGrainSourceFallbackOrStatusLine)
		{
			// S4-H2: the upscale form's grain term comes from kP10gGrainLevel via ReadGrain (never the downscale scalar
			// GrainSource P10/P11 seam). Even when the host always supplies host.grainSource (as LibplaceboVideoRenderer does),
			// an active upscale frame must NOT invoke host.grainSource, must NOT engage GrainSourceFallback()
			// ("product.adaptive_downscale"), and must NOT emit downscale's "Adaptive grain: ..." status line.
			GrainRig rig;
			Assert::IsTrue(rig.Init(), L"the WARP test device, formats and SSimDownscaler.glsl are available");

			static constexpr int kUpSw = 64, kUpSh = 64, kUpDw = 128, kUpDh = 128;
			static constexpr int kDownDw = 256, kDownDh = 160;
			pl_tex upSrc = nullptr;
			{
				std::vector<uint16_t> px(static_cast<size_t>(kUpSw) * kUpSh * 4);
				for (int y = 0; y < kUpSh; ++y)
					for (int x = 0; x < kUpSw; ++x)
					{
						const uint16_t v = static_cast<uint16_t>(std::lround((110.0 + ((x + y) % 32)) * 257.0));
						uint16_t* p = &px[(static_cast<size_t>(y) * kUpSw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = kUpSw; tp.h = kUpSh; tp.format = rig.srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				upSrc = pl_tex_create(rig.gpu, &tp);
				Assert::IsNotNull(upSrc, L"precondition: upscale source texture created");
				rig.owned.push_back(upSrc);
			}
			double sigma = 0.0;
			pl_tex downSrc = rig.Grey(WhiteNoiseField(2.0, 9001u, sigma));
			Assert::IsNotNull(downSrc, L"precondition: downscale source texture created");

			const AdaptiveDownscale::Settings st = GrainRig::Ramp();
			const AdaptiveDownscale::GrainSource invalidSource{};
			const AdaptiveDownscale::GrainSource validSource = rig.Source(sigma);
			Assert::IsFalse(invalidSource.Valid(), L"precondition: empty GrainSource is invalid");
			Assert::IsTrue(validSource.Valid(), L"precondition: shared GrainSource is valid");

			auto countDownscaleGrainLines = [](const std::vector<std::string>& lines) -> size_t
			{
				size_t count = 0;
				for (const std::string& line : lines)
					if (line.rfind("Adaptive grain:", 0) == 0)
						++count;
				return count;
			};

			AdaptiveDownscale::Module module;
			AdaptiveDownscaleStage::Stage stage{ module };
			int grainSourceCalls = 0;
			bool returnValidSource = false;
			AdaptiveDownscaleStage::Host host;
			host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
			host.bitrate = [](double& q) { q = 1.0; return false; };
			host.nlsMapping = []() { return NlsMaskMapping::Mapping(); };
			host.ssimInChain = []() { return true; };
			host.motion = []() { return AdaptiveDownscaleStage::MotionSource(); };
			host.publishStatus = []() {};
			host.grainSource = [&]() -> AdaptiveDownscale::GrainSource
			{
				++grainSourceCalls;
				return returnValidSource ? validSource : invalidSource;
			};

			double weight = 1.0;
			bool grainWanted = false, noSsim = false;
			uint64_t sequence = 0;
			auto renderFrame = [&](pl_tex src, int sw, int sh, int dw, int dh, bool adaptiveUpscale, bool adaptiveDownscale)
			{
				AdaptiveDownscaleStage::Frame f;
				f.adaptiveUpscale = adaptiveUpscale;
				f.adaptiveDownscale = adaptiveDownscale;
				f.adaptive = &st;
				f.lowBitrateOff = true;
				f.fboFloat32 = 0;
				f.sourceSequence = ++sequence;
				f.nominalSourceRateHz = 24.0;
				f.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				f.targetCrop = { 0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh) };

				std::vector<const pl_hook*> list;
				if (!adaptiveUpscale && adaptiveDownscale)
					list.push_back(rig.ssim);
				stage.Register(f, host, { weight, grainWanted, noSsim }, list);
				if (adaptiveUpscale)
					Assert::IsTrue(list.size() == 2 && list.front() == module.Measure() && list.back() == module.Blend(),
						L"upscale registered measure and blend");
				else
					Assert::IsTrue(list.size() == 3 && list.front() == module.Measure() && list.back() == module.Blend(),
						L"downscale registered measure, SSIM, blend");

				pl_tex_params tp{};
				tp.w = dw; tp.h = dh; tp.format = rig.dstFmt; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(rig.gpu, &tp);
				Assert::IsNotNull(target, L"target texture created");
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.upscaler = &pl_filter_ewa_lanczossharp;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;
				params.sigmoid_params = nullptr;
				params.hooks = list.data();
				params.num_hooks = static_cast<int>(list.size());
				const bool ok = pl_render_image(rig.fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(rig.gpu);
				pl_tex_destroy(rig.gpu, &target);
				Assert::IsTrue(ok, L"the WARP render ran");
			};

			// 1. Upscale frame (64x64 -> 128x128) with host.grainSource returning an invalid/empty GrainSource.
			returnValidSource = false;
			grainSourceCalls = 0;
			renderFrame(upSrc, kUpSw, kUpSh, kUpDw, kUpDh, true, false);
			Assert::IsTrue(stage.UpscaleThisFrame(), L"upscale (invalid offered GrainSource): stage must be in upscale mode");
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"upscale (invalid offered GrainSource): module must be active with zero failures");
			Assert::AreEqual(1, module.LastUpscaleRatio(), L"upscale (invalid offered GrainSource): prescale ratio must be 1");
			Assert::AreEqual(0, grainSourceCalls, L"upscale (invalid offered GrainSource): host.grainSource must not be invoked on upscale");
			{
				const FallbackCatalog::Decision d = stage.GrainSourceFallback();
				Assert::AreEqual(std::string("product.adaptive_downscale"), d.id, L"upscale (invalid offered GrainSource): fallback id");
				Assert::IsFalse(d.engaged, L"upscale (invalid offered GrainSource): downscale GrainSourceFallback must NOT engage on upscale");
				Assert::AreEqual(std::string(), d.reason, L"upscale (invalid offered GrainSource): released fallback reason must be empty");
				Assert::AreEqual(size_t(0), countDownscaleGrainLines(stage.StatusLines()),
					L"upscale (invalid offered GrainSource): StatusLines must contain zero downscale 'Adaptive grain:' lines");
			}

			// 2. Upscale frame (64x64 -> 128x128) with host.grainSource returning a valid GrainSource.
			returnValidSource = true;
			grainSourceCalls = 0;
			renderFrame(upSrc, kUpSw, kUpSh, kUpDw, kUpDh, true, false);
			Assert::IsTrue(stage.UpscaleThisFrame(), L"upscale (valid offered GrainSource): stage must be in upscale mode");
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"upscale (valid offered GrainSource): module must be active with zero failures");
			Assert::AreEqual(0, grainSourceCalls, L"upscale (valid offered GrainSource): host.grainSource must not be invoked on upscale");
			{
				const FallbackCatalog::Decision d = stage.GrainSourceFallback();
				Assert::AreEqual(std::string("product.adaptive_downscale"), d.id, L"upscale (valid offered GrainSource): fallback id");
				Assert::IsFalse(d.engaged, L"upscale (valid offered GrainSource): downscale GrainSourceFallback must NOT engage on upscale");
				Assert::AreEqual(std::string(), d.reason, L"upscale (valid offered GrainSource): released fallback reason must be empty");
				Assert::AreEqual(size_t(0), countDownscaleGrainLines(stage.StatusLines()),
					L"upscale (valid offered GrainSource): StatusLines must contain zero downscale 'Adaptive grain:' lines");
			}

			// 3. MANDATORY IN-TEST NEGATIVE CONTROL (exact same stage, module, and host with the exact same invalid
			// host.grainSource callback, switched to downscale 384x240 -> 256x160 with SSIM in chain): proves the downscale
			// GrainSourceFallback detector and "Adaptive grain:" status line are genuinely active and NOT disabled globally.
			returnValidSource = false;
			grainSourceCalls = 0;
			renderFrame(downSrc, GrainRig::kSw, GrainRig::kSh, kDownDw, kDownDh, false, true);
			Assert::IsFalse(stage.UpscaleThisFrame(), L"negative control (downscale): stage must be in downscale mode");
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"negative control (downscale): module must be active with zero failures");
			Assert::IsTrue(grainSourceCalls > 0, L"negative control (downscale): host.grainSource must be queried on downscale");
			{
				const FallbackCatalog::Decision d = stage.GrainSourceFallback();
				Assert::AreEqual(std::string("product.adaptive_downscale"), d.id, L"negative control (downscale): fallback id");
				Assert::IsTrue(d.engaged, L"negative control (downscale): invalid offered GrainSource must engage GrainSourceFallback on downscale");
				Assert::AreEqual(std::string("grain: the offered shared products are invalid (missing or grid mismatch)"), d.reason,
					L"negative control (downscale): expected invalid shared products fallback reason");
				Assert::AreEqual(size_t(1), countDownscaleGrainLines(stage.StatusLines()),
					L"negative control (downscale): StatusLines must contain exactly one 'Adaptive grain:' line on downscale");
			}

			// 4. Downscale -> Upscale transition on the exact same stage & module (both adaptiveUpscale and adaptiveDownscale
			// enabled, as in typical user settings): switching back to upscale must clear the downscale fallback and status line.
			returnValidSource = false;
			grainSourceCalls = 0;
			renderFrame(upSrc, kUpSw, kUpSh, kUpDw, kUpDh, true, true);
			Assert::IsTrue(stage.UpscaleThisFrame(), L"downscale->upscale transition: stage must be in upscale mode");
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"downscale->upscale transition: module must be active with zero failures");
			Assert::AreEqual(0, grainSourceCalls, L"downscale->upscale transition: host.grainSource must not be invoked on upscale");
			{
				const FallbackCatalog::Decision d = stage.GrainSourceFallback();
				Assert::AreEqual(std::string("product.adaptive_downscale"), d.id, L"downscale->upscale transition: fallback id");
				Assert::IsFalse(d.engaged, L"downscale->upscale transition: stale downscale fallback must be released on upscale");
				Assert::AreEqual(size_t(0), countDownscaleGrainLines(stage.StatusLines()),
					L"downscale->upscale transition: stale 'Adaptive grain:' status line must disappear on upscale");
			}

			module.Release();
			Assert::AreEqual(0, pl_renderer_get_errors(rig.fixture.Renderer()).num_disabled_hooks, L"no hook was disabled");
		}

		// Q#1 (GEM-069 / ADS-STATE): PQ inverse luminance (GuardLinear / MeasureLumaCode) must stay finite and
		// non-negative for all valid [0, 1] codes AND for overshoot WTW / ringing codes above 1.0, including
		// codes past the SMPTE ST 2084 rational pole c_pole = (18.8515625 / 18.6875)^78.84375 ~= 1.99206008
		// where e = c^(1 / 78.84375) crosses 18.8515625 / 18.6875 ~= 1.00877926 and the unclamped denominator
		// goes negative (pow(negative, 1 / m1) -> NaN).
		TEST_METHOD(GuardLinearIsFiniteAbovePqOne)
		{
			pl_color_space pqSpace{};
			pqSpace.primaries = PL_COLOR_PRIM_BT_2020;
			pqSpace.transfer = PL_COLOR_TRC_PQ;
			const AdaptiveDownscale::GuardTransferParams pq = AdaptiveDownscale::GuardTransfer(pqSpace);
			Assert::AreEqual(1.0f, pq.mode, 1e-6f, L"precondition: PQ GuardTransfer must select mode 1");
			const AdaptiveDownscale::MeasureDomain domain =
				AdaptiveDownscale::MeasureDomainFor(pqSpace, PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_2020);
			Assert::AreEqual(2.0f, domain.mode, 1e-6f, L"precondition: PQ MeasureDomainFor must select HDR decode mode 2");

			auto unclampedPqLinear = [](double c) -> double
			{
				const double e = std::pow(c, 1.0 / 78.84375);
				const double denom = 18.8515625 - 18.6875 * e;
				const double n = (std::max)(e - 0.8359375, 0.0) / denom;
				return std::pow(n, 1.0 / 0.1593017578125) * (10000.0 / 203.0);
			};

			// 1. Valid PQ domain [0, 1] must match exact SMPTE ST 2084 EOTF (clamping must not disturb valid codes).
			const double validCodes[] = { 0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0 };
			for (double c : validCodes)
			{
				const double got = AdaptiveDownscale::GuardLinear(pq, c);
				const double expected = c <= 0.0 ? 0.0 : unclampedPqLinear(c);
				const std::wstring msg = L"valid PQ code c=" + std::to_wstring(c) +
					L": got=" + std::to_wstring(got) + L", expected=" + std::to_wstring(expected);
				Assert::IsTrue(std::isfinite(got), msg.c_str());
				Assert::IsTrue(std::fabs(got - expected) <= 1e-9 * (1.0 + expected), msg.c_str());
			}
			const double atOne = AdaptiveDownscale::GuardLinear(pq, 1.0);
			Assert::AreEqual(10000.0 / 203.0, atOne, 1e-6, L"PQ c=1.0 must equal 10000/203 relative linear");
			const double btb = AdaptiveDownscale::GuardLinear(pq, -0.05);
			Assert::IsTrue(std::isfinite(btb) && btb == 0.0, L"negative BTB code must clamp cleanly to 0");

			// 2. Overshoot WTW codes above PQ 1.0 — both mild (1.005..1.5) and pole-crossing (2.0..4.0, past
			// c_pole ~= 1.99206008) — must stay finite, non-negative, and >= peak PQ white in GuardLinear and MeasureLumaCode.
			const double overshootCodes[] = { 1.005, 1.01, 1.02, 1.05, 1.2, 1.5, 2.0, 2.05, 2.5, 4.0 };
			for (double c : overshootCodes)
			{
				const double got = AdaptiveDownscale::GuardLinear(pq, c);
				const std::wstring msg = L"overshoot PQ code c=" + std::to_wstring(c) + L": GuardLinear=" + std::to_wstring(got);
				Assert::IsTrue(std::isfinite(got), (L"GuardLinear must stay finite: " + msg).c_str());
				Assert::IsTrue(got >= atOne - 1e-6, (L"GuardLinear above 1.0 must not drop below peak PQ: " + msg).c_str());

				const double rgb[3] = { c, c, c };
				const double lumaCode = AdaptiveDownscale::MeasureLumaCode(domain, rgb);
				Assert::IsTrue(std::isfinite(lumaCode) && lumaCode >= 0.0,
					(L"MeasureLumaCode must stay finite and non-negative for overshoot PQ code c=" + std::to_wstring(c)).c_str());
			}

			// 3. MANDATORY IN-TEST NEGATIVE CONTROL on the exact same pole-crossing overshoot codes (> c_pole ~= 1.99206):
			// the unclamped ST 2084 formula crosses denominator 0 and produces NaN in both double and float32, proving
			// the overshoot hazard is real and the assertions above are non-vacuous.
			const double poleCrossingCodes[] = { 2.0, 2.05, 2.5, 4.0 };
			for (double c : poleCrossingCodes)
			{
				const double e = std::pow(c, 1.0 / 78.84375);
				const double denom = 18.8515625 - 18.6875 * e;
				const double unclamped = unclampedPqLinear(c);
				Assert::IsTrue(denom < 0.0,
					(L"negative control: PQ inverse denominator must be negative at c=" + std::to_wstring(c)).c_str());
				Assert::IsFalse(std::isfinite(unclamped),
					(L"negative control (double): unclamped PQ inverse must produce NaN at c=" + std::to_wstring(c)).c_str());

				const float cf = static_cast<float>(c);
				const float ef = std::pow(cf, 1.0f / 78.84375f);
				const float unclampedF = std::pow((std::max)(ef - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * ef),
					1.0f / 0.1593017578125f);
				Assert::IsFalse(std::isfinite(unclampedF),
					(L"negative control (float32): unclamped PQ inverse must produce NaN at c=" + std::to_wstring(c)).c_str());
			}
		}

		// Q#1 (GEM-069 / ADS-STATE, WARP GPU):
		//  0. Direct WARP GPU test of MeasureDomainGlsl.h (ad_lin / ad_luma via DispatchEdgeMapStage) on overshootTex
		//     containing valid PQ 1.0 and pole-crossing PQ codes {2.05, 2.50, 4.00}: every output RG32F texel must be
		//     strictly finite and every overshoot pixel's luma must be >= MeasureLumaCode(pqDomain, {1, 1, 1}) - 1e-2f
		//     (defeats FXC SM5.0 smoothstep(NaN)->0.0 masking).
		//  1. An RGBA32F PL_COLOR_TRC_PQ frame with a high-grain background plus overshoot pixels above PQ 1.0 and past
		//     the ST 2084 pole (c = 1.00, 1.05, 1.20, 1.50, 2.05, 2.50, 4.00) rendered through AdaptiveDownscale::Module
		//     + SSimDownscaler must keep both persistent grain state (LastStats) and every float32 output pixel strictly
		//     finite AND drive stats2.grainScale < 0.90f (catching DXBC smoothstep(NaN) -> 0.0 -> target = 1.0).
		//  2. Poisoned-state recovery on WARP: both end-to-end (clean -> NaN -> clean) AND direct WARP dispatch of the
		//     real production FINAL_BODY_B from AdaptiveDownscale.cpp on a 1x1 RGBA32F state_io texture pre-uploaded with
		//     { quiet_NaN(), 0, 0, 1 } (cut_in = 0, smooth_alpha = 0.1) must snap cleanly to finite target (with an
		//     in-test WARP negative control proving pre-fix ordered prev.r < 0.0 stays NaN), plus structural guards on
		//     FINAL_BODY_B, MAP_BODY, and STABILISE_GUARD_DEFINE's rs_lin.
		TEST_METHOD(MeasureStateStaysFiniteOnAnOvershootPixel)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size());
			Assert::IsNotNull(ssim, L"SSimDownscaler.glsl must parse");

			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch, L"pl_dispatch_create failed on WARP");

			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt rg32fFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 2, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt rgba32fRwFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt != nullptr && dstFmt != nullptr && rg32fFmt != nullptr && rgba32fRwFmt != nullptr,
				L"WARP must provide sampleable, storable, renderable, and host-readable RG32F/RGBA32F");

			static constexpr int sw = 128, sh = 96, dw = 64, dh = 48;
			auto makeFloatTex = [&](const std::vector<float>& rgba) -> pl_tex
			{
				pl_tex_params tp{};
				tp.w = sw;
				tp.h = sh;
				tp.format = srcFmt;
				tp.sampleable = true;
				tp.initial_data = rgba.data();
				pl_tex tex = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(tex, L"RGBA32F source texture creation failed");
				return tex;
			};

			// Genuinely smooth, grain-free surface (energy << grainLo = 2.0 in both PQ and gamma22) so
			// Frame 1 initializes state.r = 1.0 (catching DXBC smoothstep(NaN) -> 0.0 -> target = 1.0).
			std::vector<float> cleanData(static_cast<size_t>(sw) * sh * 4, 0.0f);
			for (int y = 0; y < sh; ++y)
			{
				for (int x = 0; x < sw; ++x)
				{
					const float v = 0.40f + 0.002f * std::sin(x * 0.15f) * std::cos(y * 0.15f);
					float* p = &cleanData[(static_cast<size_t>(y) * sw + x) * 4];
					p[0] = p[1] = p[2] = v;
					p[3] = 1.0f;
				}
			}

			// High-grain deterministic PQ background (energy ~= 19.4 >> grainHi = 8.0 -> clean target = grainFloor = 0.40)
			// plus valid PQ peak (1.0), mild WTW (> 1.0), and pole-crossing (> c_pole ~= 1.99206) PQ overshoot pixels.
			const float overshootValues[] = { 1.00f, 1.05f, 1.20f, 1.50f, 2.05f, 2.50f, 4.00f };
			static constexpr size_t kNumOvershootValues = sizeof(overshootValues) / sizeof(overshootValues[0]);
			std::vector<float> overshootData(static_cast<size_t>(sw) * sh * 4, 0.0f);
			{
				uint32_t grainSeed = 12345u;
				for (int y = 0; y < sh; ++y)
				{
					for (int x = 0; x < sw; ++x)
					{
						grainSeed = grainSeed * 1664525u + 1013904223u;
						const float n = (static_cast<float>(grainSeed >> 8) / 16777216.0f - 0.5f) * 2.0f;
						const float v = 0.40f + 0.015f * n;
						float* p = &overshootData[(static_cast<size_t>(y) * sw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 1.0f;
					}
				}
			}
			for (size_t k = 0; k < kNumOvershootValues; ++k)
			{
				const int px = sw / 2 - 3 + static_cast<int>(k);
				const int py = sh / 2;
				float* p = &overshootData[(static_cast<size_t>(py) * sw + px) * 4];
				p[0] = p[1] = p[2] = overshootValues[k];
				p[3] = 1.0f;
			}

			std::vector<float> nanData = cleanData;
			{
				const float qnan = std::numeric_limits<float>::quiet_NaN();
				float* p = &nanData[(static_cast<size_t>(sh / 2) * sw + (sw / 2)) * 4];
				p[0] = p[1] = p[2] = qnan;
				p[3] = 1.0f;
			}

			pl_tex cleanTex = makeFloatTex(cleanData);
			pl_tex overshootTex = makeFloatTex(overshootData);
			pl_tex nanTex = makeFloatTex(nanData);

			auto assertAllFinite = [](const std::vector<float>& px, const wchar_t* label)
			{
				size_t nonFiniteCount = 0;
				size_t firstBad = 0;
				for (size_t i = 0; i < px.size(); ++i)
				{
					if (!std::isfinite(px[i]))
					{
						if (nonFiniteCount == 0)
							firstBad = i;
						++nonFiniteCount;
					}
				}
				const std::wstring msg = std::wstring(label) + L": found " + std::to_wstring(nonFiniteCount) +
					L" non-finite output components (first at index " + std::to_wstring(firstBad) + L")";
				Assert::AreEqual(size_t(0), nonFiniteCount, msg.c_str());
			};

			// 0. Direct WARP GPU test of MeasureDomainGlsl.h (ad_lin / ad_luma) via DispatchEdgeMapStage on overshootTex.
			//    No smoothstep sits between ad_lin and em_out, so pre-fix unclamped ad_lin writes literal NaN texels
			//    on WARP even under FXC SM5.0, and even a flush-to-zero bug would fail gotLuma >= pq1Luma - 1e-2f.
			{
				pl_tex_params edgeTp{};
				edgeTp.w = sw;
				edgeTp.h = sh;
				edgeTp.format = rg32fFmt;
				edgeTp.sampleable = true;
				edgeTp.storable = true;
				edgeTp.host_readable = true;
				pl_tex edgeDst = pl_tex_create(gpu, &edgeTp);
				Assert::IsNotNull(edgeDst, L"RG32F edge-map output texture creation failed");

				pl_color_space pqColor{};
				pqColor.primaries = PL_COLOR_PRIM_BT_2020;
				pqColor.transfer = PL_COLOR_TRC_PQ;
				std::string dispatchWhy;
				const bool dispatched = Vpv::Regions::DispatchEdgeMapStage(dispatch, overshootTex, pqColor,
					PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_2020, edgeDst, dispatchWhy);
				Assert::IsTrue(dispatched,
					(L"DispatchEdgeMapStage(PQ) failed on WARP: " + std::wstring(dispatchWhy.begin(), dispatchWhy.end())).c_str());
				pl_gpu_finish(gpu);

				std::vector<float> edgeRg(static_cast<size_t>(sw) * sh * 2, 0.0f);
				pl_tex_transfer_params edgeDl{};
				edgeDl.tex = edgeDst;
				edgeDl.ptr = edgeRg.data();
				Assert::IsTrue(pl_tex_download(gpu, &edgeDl), L"RG32F edge-map download failed");
				pl_tex_destroy(gpu, &edgeDst);

				assertAllFinite(edgeRg, L"Direct MeasureDomainGlsl (DispatchEdgeMapStage PQ) RG32F output");

				const VpMeasure::MeasureDomain pqDomain = VpMeasure::MeasureDomainFor(pqColor,
					PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_2020);
				const double pqWhiteRgb[3] = { 1.0, 1.0, 1.0 };
				const float pq1Luma = static_cast<float>(VpMeasure::MeasureLumaCode(pqDomain, pqWhiteRgb));

				for (size_t k = 0; k < kNumOvershootValues; ++k)
				{
					const int px = sw / 2 - 3 + static_cast<int>(k);
					const int py = sh / 2;
					const size_t idx = (static_cast<size_t>(py) * sw + px) * 2;
					const float gotSobel = edgeRg[idx + 0];
					const float gotLuma = edgeRg[idx + 1];
					Assert::IsTrue(std::isfinite(gotSobel) && std::isfinite(gotLuma),
						(L"Direct MeasureDomainGlsl: non-finite RG32F texel at PQ c=" + std::to_wstring(overshootValues[k])).c_str());
					Assert::IsTrue(gotLuma >= pq1Luma - 1e-2f,
						(L"Direct MeasureDomainGlsl: PQ overshoot luma must be >= PQ 1.0 luma (" + std::to_wstring(pq1Luma) +
							L") at c=" + std::to_wstring(overshootValues[k]) + L", got " + std::to_wstring(gotLuma)).c_str());
					if (k == 0)
					{
						Assert::IsTrue(std::fabs(gotLuma - pq1Luma) <= 1.0f,
							(L"Direct MeasureDomainGlsl: valid PQ 1.0 pixel must match CPU MeasureLumaCode (" +
								std::to_wstring(pq1Luma) + L"), got " + std::to_wstring(gotLuma)).c_str());
					}
				}
			}

			AdaptiveDownscale::Settings st;
			st.grainMap = false;
			st.grainSmoothMs = 500.0f;   // alpha < 1: subsequent frames genuinely mix prev.r unless reset

			auto renderFrame = [&](AdaptiveDownscale::Module& module, uint64_t& sequence, pl_tex src,
				enum pl_color_transfer trc, enum pl_color_primaries prim, bool advance, std::vector<float>* outPx)
			{
				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target, L"RGBA32F target texture creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				image.color.transfer = out.color.transfer = trc;
				image.color.primaries = out.color.primaries = prim;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;   // keep PRE_KERNEL in source PQ/gamma domain so ad_lin runs on PQ

				if (advance)
					++sequence;
				module.SetFrame(st, sequence, 1.0f / 24.0f);
				const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
				params.hooks = hooks;
				params.num_hooks = 3;

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");

				if (outPx != nullptr)
				{
					outPx->assign(static_cast<size_t>(dw) * dh * 4, 0.0f);
					pl_tex_transfer_params dl{};
					dl.tex = target;
					dl.ptr = outPx->data();
					Assert::IsTrue(pl_tex_download(gpu, &dl), L"RGBA32F target download failed");
				}
				pl_tex_destroy(gpu, &target);
			};

			auto noCutSource = [](uint64_t, uint64_t)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = false;
				return v;
			};

			// 1. PQ overshoot test: smooth clean frame (state.r = 1.0) -> high-grain PQ overshoot frames
			//    (with c = 1.00..4.00 past c_pole ~= 1.99206) -> clean frame (no cut).
			{
				AdaptiveDownscale::Module module;
				module.SetCutSource(noCutSource);
				uint64_t sequence = 0;
				std::vector<float> out2, out3;

				// Frame 1: initialize state cleanly near 1.0 on the smooth PQ surface.
				renderFrame(module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, true, nullptr);
				renderFrame(module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				renderFrame(module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				const AdaptiveDownscale::Stats stats1 = module.LastStats();
				Assert::IsTrue(stats1.valid && std::isfinite(stats1.grainScale) && stats1.grainScale > 0.99f,
					(L"clean PQ Frame 1 must initialize state grainScale near 1.0, got " + std::to_wstring(stats1.grainScale)).c_str());

				// Frames 2..9: high-grain PQ background + overshoot PQ pixels (including 2.05, 2.50, 4.00 past c_pole).
				// Over 8 advancing frames (alpha ~= 0.08 toward grainFloor = 0.40), clean grainScale drops to ~0.71 < 0.90;
				// pre-fix unclamped ad_lin poisons t.x -> NaN -> DXBC smoothstep(NaN) == 0 -> target = 1.0 (stuck at 1.0!).
				for (int f = 0; f < 8; ++f)
					renderFrame(module, sequence, overshootTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, true, f == 7 ? &out2 : nullptr);
				renderFrame(module, sequence, overshootTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				renderFrame(module, sequence, overshootTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				const AdaptiveDownscale::Stats stats2 = module.LastStats();
				Assert::IsTrue(module.Active() && module.Failures() == 0, L"PQ overshoot frame: module must stay active with zero failures");
				Assert::IsTrue(stats2.valid, L"PQ overshoot frame: grain state statistics must be read back");
				Assert::IsTrue(std::isfinite(stats2.grainScale) && stats2.grainScale >= st.grainFloor - 1e-4f && stats2.grainScale <= 1.0f + 1e-4f,
					(L"PQ overshoot frame: state grainScale must stay finite and in [grainFloor, 1], got " + std::to_wstring(stats2.grainScale)).c_str());
				Assert::IsTrue(stats2.grainScale < 0.90f,
					(L"PQ overshoot frame: high-grain background must pull grainScale < 0.90 (catches DXBC smoothstep(NaN)->0->target=1), got " +
						std::to_wstring(stats2.grainScale)).c_str());
				Assert::IsTrue(std::isfinite(stats2.meanW) && stats2.meanW >= 0.0f && stats2.meanW <= st.wBase + 1e-4f,
					(L"PQ overshoot frame: state meanW must stay finite and in [0, wBase], got " + std::to_wstring(stats2.meanW)).c_str());
				assertAllFinite(out2, L"PQ overshoot frame output");

				// Subsequent clean PQ frame (no cut) must stay finite and uncorrupted.
				renderFrame(module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, true, &out3);
				renderFrame(module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				renderFrame(module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				const AdaptiveDownscale::Stats stats3 = module.LastStats();
				Assert::IsTrue(module.Active() && module.Failures() == 0, L"post-overshoot clean PQ frame: module must stay active with zero failures");
				Assert::IsTrue(stats3.valid, L"post-overshoot clean PQ frame: statistics must be read back");
				Assert::IsTrue(std::isfinite(stats3.grainScale) && stats3.grainScale >= st.grainFloor - 1e-4f && stats3.grainScale <= 1.0f + 1e-4f,
					(L"post-overshoot clean PQ frame: state grainScale must stay finite, got " + std::to_wstring(stats3.grainScale)).c_str());
				Assert::IsTrue(std::isfinite(stats3.meanW) && stats3.meanW >= 0.0f && stats3.meanW <= st.wBase + 1e-4f,
					(L"post-overshoot clean PQ frame: state meanW must stay finite, got " + std::to_wstring(stats3.meanW)).c_str());
				assertAllFinite(out3, L"post-overshoot clean PQ frame output");

				module.Release();
			}

			// 2. Poisoned-state recovery on WARP:
			// (a) End-to-end AdaptiveDownscale::Module: clean frame -> NaN-injected frame -> clean frame with cut = false.
			{
				AdaptiveDownscale::Module module;
				module.SetCutSource(noCutSource);
				uint64_t sequence = 0;
				std::vector<float> recoveredOut;

				renderFrame(module, sequence, cleanTex, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_709, true, nullptr);
				renderFrame(module, sequence, nanTex, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_709, true, nullptr);
				renderFrame(module, sequence, cleanTex, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_709, true, &recoveredOut);
				renderFrame(module, sequence, cleanTex, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_709, false, nullptr);
				renderFrame(module, sequence, cleanTex, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_709, false, nullptr);
				const AdaptiveDownscale::Stats recovered = module.LastStats();
				Assert::IsTrue(module.Active() && module.Failures() == 0, L"NaN recovery frame: module must stay active with zero failures");
				Assert::IsTrue(recovered.valid, L"NaN recovery frame: statistics must be read back");
				Assert::IsTrue(std::isfinite(recovered.grainScale) && recovered.grainScale >= st.grainFloor - 1e-4f && recovered.grainScale <= 1.0f + 1e-4f,
					(L"NaN recovery frame: FINAL_BODY_B must recover finite grainScale, got " + std::to_wstring(recovered.grainScale)).c_str());
				Assert::IsTrue(std::isfinite(recovered.meanW) && recovered.meanW >= 0.0f && recovered.meanW <= st.wBase + 1e-4f,
					(L"NaN recovery frame: meanW must recover finite, got " + std::to_wstring(recovered.meanW)).c_str());
				assertAllFinite(recoveredOut, L"NaN recovery clean frame output");

				module.Release();
			}

			// (b) Direct WARP GPU poisoned-state proof of the real production FINAL_BODY_B extracted from
			//     AdaptiveDownscale.cpp, plus structural guards on FINAL_BODY_B, MAP_BODY, and rs_lin, plus an
			//     in-test WARP GPU negative control proving pre-fix ordered prev.r < 0.0 stays NaN on the same input.
			{
				const std::string adsSource = SettingsTestSupport::ReadFile(
					SettingsTestSupport::RepoRoot() + "src\\VP-V\\modules\\scaling\\output\\AdaptiveDownscale.cpp");
				Assert::IsFalse(adsSource.empty(), L"Failed to read AdaptiveDownscale.cpp for shader guard verification");

				auto extractGlslRaw = [&](const char* marker) -> std::string
				{
					const size_t pos = adsSource.find(marker);
					Assert::IsTrue(pos != std::string::npos,
						(L"AdaptiveDownscale.cpp missing shader literal: " + std::wstring(marker, marker + std::strlen(marker))).c_str());
					const size_t open = adsSource.find("R\"GLSL(", pos);
					Assert::IsTrue(open != std::string::npos, L"Missing R\"GLSL( opening delimiter in AdaptiveDownscale.cpp");
					const size_t start = open + 7;
					const size_t close = adsSource.find(")GLSL\"", start);
					Assert::IsTrue(close != std::string::npos, L"Missing )GLSL\" closing delimiter in AdaptiveDownscale.cpp");
					return adsSource.substr(start, close - start);
				};

				std::string nonfiniteGlsl;
				{
					const size_t nfPos = adsSource.find("const char* const NONFINITE_GLSL =");
					if (nfPos != std::string::npos)
					{
						const size_t q0 = adsSource.find('"', nfPos);
						const size_t q1 = q0 != std::string::npos ? adsSource.find('"', q0 + 1) : std::string::npos;
						Assert::IsTrue(q0 != std::string::npos && q1 != std::string::npos, L"Malformed NONFINITE_GLSL literal");
						std::string raw = adsSource.substr(q0 + 1, q1 - q0 - 1);
						size_t esc = 0;
						while ((esc = raw.find("\\n", esc)) != std::string::npos)
						{
							raw.replace(esc, 2, "\n");
							++esc;
						}
						nonfiniteGlsl = raw;
					}
				}

				const std::string finalHeader = extractGlslRaw("const char* const FINAL_HEADER =");
				const std::string finalBodyA = extractGlslRaw("const char* const FINAL_BODY_A =");
				const std::string finalBodyB = extractGlslRaw("const char* const FINAL_BODY_B =");
				const std::string mapBody = extractGlslRaw("const char* const MAP_BODY =");
				const std::string stabiliseGuard = extractGlslRaw("const char* const STABILISE_GUARD_DEFINE =");

				// Structural invariants: unordered !(prev.r >= 0.0), zero isnan() (FXC folds it), clamped rs_lin pole.
				Assert::IsTrue(finalBodyB.find("!(prev.r >= 0.0)") != std::string::npos,
					L"FINAL_BODY_B must guard state.r with !(prev.r >= 0.0) so NaN triggers immediate self-heal");
				Assert::IsTrue(finalBodyB.find("isnan(") == std::string::npos,
					L"FINAL_BODY_B must not use isnan() (D3DCompile/FXC can fold it)");
				Assert::IsTrue(mapBody.find("!(prev.r >= 0.0)") != std::string::npos,
					L"MAP_BODY must guard prev.r with !(prev.r >= 0.0) so NaN triggers immediate self-heal");
				Assert::IsTrue(mapBody.find("isnan(") == std::string::npos,
					L"MAP_BODY must not use isnan() (D3DCompile/FXC can fold it)");
				Assert::IsTrue(stabiliseGuard.find("max(18.8515625 - 18.6875 * e, 1e-6)") != std::string::npos,
					L"STABILISE_GUARD_DEFINE rs_lin must clamp ST 2084 PQ denominator via max(18.8515625 - 18.6875 * e, 1e-6)");
				Assert::IsTrue(stabiliseGuard.find("isnan(") == std::string::npos,
					L"STABILISE_GUARD_DEFINE must not use isnan()");

				// Direct WARP dispatch of production FINAL_BODY_B against 1x1 RGBA32F state_io = { quiet_NaN(), 0, 0, 1 }.
				const float partialInit[4] = { 100.0f, 10.0f, 10.0f, 0.0f };   // energy = 10.0 > grainHi (8.0) -> target = grainFloor (0.40)
				pl_tex_params partialTp{};
				partialTp.w = 1;
				partialTp.h = 1;
				partialTp.format = srcFmt;
				partialTp.sampleable = true;
				partialTp.initial_data = partialInit;
				pl_tex partialTex = pl_tex_create(gpu, &partialTp);
				Assert::IsNotNull(partialTex, L"1x1 RGBA32F partial_in texture creation failed");

				pl_tex_params stateTp{};
				stateTp.w = 1;
				stateTp.h = 1;
				stateTp.format = rgba32fRwFmt;
				stateTp.sampleable = true;
				stateTp.storable = true;
				stateTp.host_writable = true;
				stateTp.host_readable = true;
				pl_tex stateTex = pl_tex_create(gpu, &stateTp);
				Assert::IsNotNull(stateTex, L"1x1 RGBA32F state_io texture creation failed");

				pl_tex_params statsTp{};
				statsTp.w = 1;
				statsTp.h = 1;
				statsTp.format = rgba32fRwFmt;
				statsTp.sampleable = true;
				statsTp.storable = true;
				statsTp.host_readable = true;
				pl_tex statsTex = pl_tex_create(gpu, &statsTp);
				Assert::IsNotNull(statsTex, L"1x1 RGBA32F ad_stats_img texture creation failed");

				auto dispatchFinalOnPoisonedState = [&](const std::string& headerText, const std::string& bodyText,
					const char* desc, float outState[4], float outStats[4])
				{
					float poisonedState[4] = { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 1.0f };
					pl_tex_transfer_params up{};
					up.tex = stateTex;
					up.ptr = poisonedState;
					Assert::IsTrue(pl_tex_upload(gpu, &up), L"RGBA32F poisoned state upload failed");

					pl_shader_desc d[3]{};
					d[0].desc.name = "partial_in";
					d[0].desc.type = PL_DESC_SAMPLED_TEX;
					d[0].binding.object = partialTex;
					d[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
					d[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
					d[1].desc.name = "state_io";
					d[1].desc.type = PL_DESC_STORAGE_IMG;
					d[1].desc.access = PL_DESC_ACCESS_READWRITE;
					d[1].binding.object = stateTex;
					d[2].desc.name = "ad_stats_img";
					d[2].desc.type = PL_DESC_STORAGE_IMG;
					d[2].desc.access = PL_DESC_ACCESS_WRITEONLY;
					d[2].binding.object = statsTex;

					const float writeStatsF = 1.0f;
					const float advanceF = 1.0f;
					const float cutInF = 0.0f;
					const float totalPixelsF = 10.0f;
					const float smoothAlphaF = 0.1f;
					const pl_shader_var v[8] = {
						{ pl_var_float("write_stats"), &writeStatsF, true },
						{ pl_var_float("advance"), &advanceF, true },
						{ pl_var_float("cut_in"), &cutInF, true },
						{ pl_var_float("total_pixels"), &totalPixelsF, true },
						{ pl_var_float("grain_lo"), &st.grainLo, true },
						{ pl_var_float("grain_hi"), &st.grainHi, true },
						{ pl_var_float("grain_floor"), &st.grainFloor, true },
						{ pl_var_float("smooth_alpha"), &smoothAlphaF, true } };

					pl_custom_shader custom{};
					custom.description = desc;
					custom.header = headerText.c_str();
					custom.body = bodyText.c_str();
					custom.input = PL_SHADER_SIG_NONE;
					custom.output = PL_SHADER_SIG_NONE;
					custom.descriptors = d;
					custom.num_descriptors = 3;
					custom.variables = v;
					custom.num_variables = 8;
					custom.compute = true;
					custom.compute_group_size[0] = 16;
					custom.compute_group_size[1] = 16;

					pl_shader sh = pl_dispatch_begin(dispatch);
					const bool customOk = pl_shader_custom(sh, &custom);
					if (!customOk)
						pl_dispatch_abort(dispatch, &sh);
					Assert::IsTrue(customOk, L"pl_shader_custom failed for poisoned-state FINAL_BODY_B test");

					pl_dispatch_compute_params compute{};
					compute.shader = &sh;
					compute.dispatch_size[0] = 1;
					compute.dispatch_size[1] = 1;
					compute.dispatch_size[2] = 1;
					Assert::IsTrue(pl_dispatch_compute(dispatch, &compute), L"pl_dispatch_compute failed for poisoned-state FINAL_BODY_B test");
					pl_gpu_finish(gpu);

					pl_tex_transfer_params stateDl{};
					stateDl.tex = stateTex;
					stateDl.ptr = outState;
					Assert::IsTrue(pl_tex_download(gpu, &stateDl), L"RGBA32F state_io download failed");

					pl_tex_transfer_params statsDl{};
					statsDl.tex = statsTex;
					statsDl.ptr = outStats;
					Assert::IsTrue(pl_tex_download(gpu, &statsDl), L"RGBA32F ad_stats_img download failed");
				};

				const std::string prodHeader = "#define AD_STATS_IMG 1\n" + nonfiniteGlsl + finalHeader;
				const std::string prodBody = finalBodyA + finalBodyB;
				float healedState[4] = {};
				float healedStats[4] = {};
				dispatchFinalOnPoisonedState(prodHeader, prodBody, "Q#1 production FINAL_BODY_B NaN self-heal", healedState, healedStats);
				Assert::IsTrue(std::isfinite(healedState[0]) && std::fabs(healedState[0] - st.grainFloor) < 1e-4f,
					(L"Production FINAL_BODY_B on WARP must snap poisoned prev.r=NaN cleanly to target=" +
						std::to_wstring(st.grainFloor) + L", got " + std::to_wstring(healedState[0])).c_str());
				Assert::IsTrue(healedState[3] > 1.5f,
					(L"Production FINAL_BODY_B on WARP must set state.a=2.0 (cut/self-heal flag), got " +
						std::to_wstring(healedState[3])).c_str());
				Assert::IsTrue(std::isfinite(healedStats[0]) && std::fabs(healedStats[0] - st.grainFloor) < 1e-4f,
					(L"Production FINAL_BODY_B on WARP must publish finite healed stats=" +
						std::to_wstring(st.grainFloor) + L", got " + std::to_wstring(healedStats[0])).c_str());

				// In-test WARP GPU negative control on the exact same poisoned { quiet_NaN(), 0, 0, 1 } texture:
				// pre-fix ordered comparison (prev.r < 0.0) returns false for NaN, leaving state.r stuck at NaN!
				const std::string naiveHeader = "#define AD_STATS_IMG 1\n" + finalHeader;
				const std::string naiveBody = finalBodyA + R"GLSL(    bool cut = prev.a < 0.5 || cut_in > 0.5 || prev.r < 0.0;
    float scale = cut ? target : mix(prev.r, target, smooth_alpha);
    imageStore(state_io, ivec2(0), vec4(scale, free_mean, 0.0, cut ? 2.0 : 1.0));
    if (write_stats > 0.5) {
        imageStore(ad_stats_img, ivec2(0), vec4(scale, free_mean, 0.0, stats_tag));
    }
}
)GLSL";
				float unhealedState[4] = {};
				float unhealedStats[4] = {};
				dispatchFinalOnPoisonedState(naiveHeader, naiveBody, "Q#1 negative control pre-fix ordered prev.r < 0.0", unhealedState, unhealedStats);
				Assert::IsFalse(std::isfinite(unhealedState[0]),
					(L"WARP negative control: pre-fix ordered prev.r < 0.0 must fail to heal NaN on WARP, got " +
						std::to_wstring(unhealedState[0])).c_str());
				Assert::IsTrue(unhealedState[3] < 1.5f,
					(L"WARP negative control: pre-fix ordered prev.r < 0.0 must not signal cut on NaN, got state.a=" +
						std::to_wstring(unhealedState[3])).c_str());

				for (pl_tex* t : { &partialTex, &stateTex, &statsTex })
					pl_tex_destroy(gpu, t);
			}

			// 3. MANDATORY IN-TEST NEGATIVE CONTROLS (CPU float32 math):
			// (a) On the exact pole-crossing overshoot pixels uploaded into overshootTex (2.05, 2.50, 4.00), the
			//     unclamped float32 PQ inverse formula genuinely produces NaN (proving the GPU test input is hazardous).
			for (float c : { 2.05f, 2.50f, 4.00f })
			{
				const float e = std::pow(c, 1.0f / 78.84375f);
				const float unclamped = std::pow((std::max)(e - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * e),
					1.0f / 0.1593017578125f);
				Assert::IsFalse(std::isfinite(unclamped),
					(L"negative control: unclamped PQ inverse must produce NaN for overshootTex pixel c=" + std::to_wstring(c)).c_str());
			}
			// (b) On NaN, !(nan >= 0.0) is true (triggers FINAL_BODY_B state reset) whereas ordered (nan < 0.0) is false
			//     (would fail to detect poisoned state, leaving mix(prev.r, target, alpha) stuck at NaN).
			const volatile float vNan = std::numeric_limits<float>::quiet_NaN();
			const float nan = vNan;
			Assert::IsTrue(!(nan >= 0.0f), L"!(NaN >= 0.0) must be true to detect and reset poisoned state");
			Assert::IsFalse(nan < 0.0f, L"negative control: ordered comparison (NaN < 0.0) is false and fails to detect NaN");
			Assert::IsFalse(std::isfinite(nan + 0.1f * (1.0f - nan)),
				L"negative control: unguarded one-pole mix(NaN, target, alpha) remains non-finite");

			pl_dispatch_destroy(&dispatch);
			for (pl_tex* t : { &cleanTex, &overshootTex, &nanTex })
				pl_tex_destroy(gpu, t);
			pl_mpv_user_shader_destroy(&ssim);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Q#2 (GEM-070 / ADS-STATE, WARP GPU):
		// Residual stabilisation must stop holding Frame 1's luma residual across a confirmed scene cut
		// (SetCutSource -> CutVerdict::State::Known, cut = true), for both plain (residualGrainGuard = false)
		// and guarded (residualGrainGuard = true) paths, including on a repeat of the cut frame.
		// Mandatory in-test negative control: the exact same Frame 1 -> Frame 2 inputs with cut = false
		// MUST hold Frame 1's residual (> 0.01 delta vs fresh Frame 2), proving the hazard is real.
		TEST_METHOD(ResidualHoldStopsAtAConfirmedCut)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size());
			Assert::IsNotNull(ssim, L"SSimDownscaler.glsl must parse");

			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt fieldFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt trustFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsTrue(srcFmt != nullptr && dstFmt != nullptr && fieldFmt != nullptr && trustFmt != nullptr,
				L"WARP must provide float32 source, target, motion field, and frame-trust formats");

			static constexpr int sw = 128, sh = 96, dw = 64, dh = 48;
			static constexpr int bw = sw / 16, bh = sh / 16;

			auto makeSourceTex = [&](const std::function<float(int, int)>& pattern) -> pl_tex
			{
				std::vector<float> rgba(static_cast<size_t>(sw) * sh * 4, 0.0f);
				for (int y = 0; y < sh; ++y)
				{
					for (int x = 0; x < sw; ++x)
					{
						const float v = pattern(x, y);
						float* p = &rgba[(static_cast<size_t>(y) * sw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 1.0f;
					}
				}
				pl_tex_params tp{};
				tp.w = sw;
				tp.h = sh;
				tp.format = srcFmt;
				tp.sampleable = true;
				tp.initial_data = rgba.data();
				pl_tex tex = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(tex, L"RGBA32F source texture creation failed");
				return tex;
			};

			// Frame 1: high-contrast 2x2 checkerboard (left) + 2-px vertical stripes (right) so SSIM and
			// faithful ewa_lanczossharp diverge strongly at 2x downscale (large Frame-1 residual).
			pl_tex frame1Tex = makeSourceTex([](int x, int y) -> float
			{
				if (x < sw / 2)
					return (((x / 2) + (y / 2)) % 2) != 0 ? 0.90f : 0.10f;
				return ((x / 2) % 2) != 0 ? 0.90f : 0.10f;
			});

			// Frame 2: completely different scene — uniform flat 0.50 (top half, zero own residual)
			// plus horizontal stripes (bottom half, orthogonal non-zero own residual).
			pl_tex frame2Tex = makeSourceTex([](int, int y) -> float
			{
				if (y < sh / 2)
					return 0.50f;
				return ((y / 2) % 2) != 0 ? 0.72f : 0.28f;
			});

			// Valid zero-motion field: v = (0, 0), trust = 1.0 on every block; frameTrust = 1.0.
			std::vector<float> fieldData(static_cast<size_t>(bw) * bh * 4, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(bw) * bh; ++i)
			{
				fieldData[i * 4 + 0] = 0.0f;
				fieldData[i * 4 + 1] = 0.0f;
				fieldData[i * 4 + 2] = 1.0f;
				fieldData[i * 4 + 3] = 0.0f;
			}
			pl_tex_params fp{};
			fp.w = bw;
			fp.h = bh;
			fp.format = fieldFmt;
			fp.sampleable = true;
			fp.initial_data = fieldData.data();
			pl_tex fieldTex = pl_tex_create(gpu, &fp);
			Assert::IsNotNull(fieldTex, L"motion field texture creation failed");

			const float trustVal = 1.0f;
			pl_tex_params trp{};
			trp.w = 1;
			trp.h = 1;
			trp.format = trustFmt;
			trp.sampleable = true;
			trp.initial_data = &trustVal;
			pl_tex trustTex = pl_tex_create(gpu, &trp);
			Assert::IsNotNull(trustTex, L"frame trust texture creation failed");

			AdaptiveDownscale::Motion motion;
			motion.field = fieldTex;
			motion.frameTrust = trustTex;
			motion.minTrustedShare = 0.25f;
			motion.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };

			auto renderFrame = [&](AdaptiveDownscale::Module& module, const AdaptiveDownscale::Settings& st,
				uint64_t seq, pl_tex src, bool withMotion) -> std::vector<float>
			{
				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target, L"RGBA32F target texture creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				image.color.transfer = out.color.transfer = PL_COLOR_TRC_GAMMA22;
				image.color.primaries = out.color.primaries = PL_COLOR_PRIM_BT_709;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;

				module.SetFrame(st, seq, 1.0f / 24.0f);
				if (withMotion)
					module.SetMotion(motion);
				else
					module.ClearMotion();

				const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
				params.hooks = hooks;
				params.num_hooks = 3;

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");

				std::vector<float> px(static_cast<size_t>(dw) * dh * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = target;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"RGBA32F target download failed");
				pl_tex_destroy(gpu, &target);
				return px;
			};

			auto maxRgbAbsDiff = [](const std::vector<float>& a, const std::vector<float>& b) -> float
			{
				Assert::AreEqual(a.size(), b.size(), L"output frame sizes must match");
				float worst = 0.0f;
				for (size_t i = 0; i < a.size(); ++i)
				{
					Assert::IsTrue(std::isfinite(a[i]) && std::isfinite(b[i]), L"output pixels must remain finite");
					if (i % 4 == 3)
						continue;
					const float d = std::fabs(a[i] - b[i]);
					if (d > worst)
						worst = d;
				}
				return worst;
			};

			auto makeSettings = [](bool stabilise, bool grainGuard)
			{
				AdaptiveDownscale::Settings st;
				st.wBase = 1.0f;
				// Hold e = 0 and g = 1 everywhere regardless of high-contrast stripes/checkerboard
				// so the only difference between Frame 2 runs is the residual hold.
				st.edgeLo = 10000.0f;
				st.edgeHi = 20000.0f;
				st.grainLo = 10000.0f;
				st.grainHi = 20000.0f;
				st.shadowBias = 0.0f;
				st.grainSmoothMs = 0.0f;
				st.grainMap = false;
				st.ssimResidualStabilise = stabilise;
				st.residualGrainGuard = grainGuard;
				st.residualCodingNoiseCodes = 0.0f;
				st.float32Picture = true;
				return st;
			};

			auto constantCutSource = [](bool cutOnSeq2)
			{
				return [cutOnSeq2](uint64_t, uint64_t seq)
				{
					AdaptiveDownscale::CutVerdict v;
					v.state = AdaptiveDownscale::CutVerdict::State::Known;
					v.cut = cutOnSeq2 && (seq == 2);
					return v;
				};
			};

			// Precondition: verify Frame 1 has a large SSIM-vs-faithful residual (> 0.05).
			{
				AdaptiveDownscale::Module preMod;
				preMod.SetCutSource(constantCutSource(false));
				AdaptiveDownscale::Settings ssimSt = makeSettings(false, false);
				AdaptiveDownscale::Settings faithfulSt = ssimSt;
				faithfulSt.wBase = 0.0f;
				const std::vector<float> f1Ssim = renderFrame(preMod, ssimSt, 1, frame1Tex, false);
				const std::vector<float> f1Faithful = renderFrame(preMod, faithfulSt, 2, frame1Tex, false);
				const float f1Residual = maxRgbAbsDiff(f1Ssim, f1Faithful);
				Assert::IsTrue(f1Residual > 0.05f,
					(L"precondition: Frame 1 must have strong SSIM-vs-faithful residual (> 0.05), got " +
						std::to_wstring(f1Residual)).c_str());
				preMod.Release();
			}

			for (bool grainGuard : { false, true })
			{
				const std::wstring modeLabel = grainGuard ? L"[residualGrainGuard=true] " : L"[residualGrainGuard=false] ";
				const AdaptiveDownscale::Settings stStab = makeSettings(true, grainGuard);
				const AdaptiveDownscale::Settings stNoStab = makeSettings(false, grainGuard);

				// Fresh non-stabilised Frame 2 reference.
				std::vector<float> freshFrame2;
				{
					AdaptiveDownscale::Module refMod;
					refMod.SetCutSource(constantCutSource(false));
					freshFrame2 = renderFrame(refMod, stNoStab, 1, frame2Tex, false);
					Assert::IsTrue(refMod.Active() && refMod.Failures() == 0,
						(modeLabel + L"fresh Frame 2 reference must render cleanly").c_str());
					refMod.Release();
				}

				// 1. Confirmed cut on Frame 2 (seq = 2): Frame 1's residual must NOT bleed into Frame 2
				//    (nor into a repeat presentation of Frame 2).
				{
					AdaptiveDownscale::Module cutMod;
					cutMod.SetCutSource(constantCutSource(true));
					renderFrame(cutMod, stStab, 1, frame1Tex, true);
					const std::vector<float> cutFrame2 = renderFrame(cutMod, stStab, 2, frame2Tex, true);
					const std::vector<float> cutFrame2Repeat = renderFrame(cutMod, stStab, 2, frame2Tex, true);
					Assert::IsTrue(cutMod.Active() && cutMod.Failures() == 0,
						(modeLabel + L"confirmed-cut module must stay active with zero failures").c_str());

					const float cutDiff = maxRgbAbsDiff(cutFrame2, freshFrame2);
					const float repeatDiff = maxRgbAbsDiff(cutFrame2Repeat, freshFrame2);
					Assert::IsTrue(cutDiff <= 1e-4f,
						(modeLabel + L"confirmed cut must stop Frame-1 residual bleed (expected <= 1e-4, got " +
							std::to_wstring(cutDiff) + L")").c_str());
					Assert::IsTrue(repeatDiff <= 1e-4f,
						(modeLabel + L"repeat of confirmed-cut frame must have zero Frame-1 residual bleed (expected <= 1e-4, got " +
							std::to_wstring(repeatDiff) + L")").c_str());
					cutMod.Release();
				}

				// 2. MANDATORY IN-TEST NEGATIVE CONTROL (exact same Frame 1 -> Frame 2 inputs with cut = false):
				//    Frame 2 MUST hold Frame 1's residual (diverging from fresh Frame 2 by > 0.01), proving
				//    residual hold is genuinely active and only stopped by the confirmed cut.
				{
					AdaptiveDownscale::Module noCutMod;
					noCutMod.SetCutSource(constantCutSource(false));
					renderFrame(noCutMod, stStab, 1, frame1Tex, true);
					const std::vector<float> noCutFrame2 = renderFrame(noCutMod, stStab, 2, frame2Tex, true);
					Assert::IsTrue(noCutMod.Active() && noCutMod.Failures() == 0,
						(modeLabel + L"no-cut negative control module must stay active with zero failures").c_str());
					Assert::IsTrue(noCutMod.StabilisedFrames() >= 1ull,
						(modeLabel + L"no-cut negative control must actually stabilise Frame 2").c_str());

					const float noCutDiff = maxRgbAbsDiff(noCutFrame2, freshFrame2);
					Assert::IsTrue(noCutDiff > 0.01f,
						(modeLabel + L"negative control (cut=false): Frame 2 must hold Frame-1 residual (> 0.01), got " +
							std::to_wstring(noCutDiff)).c_str());
					noCutMod.Release();
				}
			}

			for (pl_tex* t : { &frame1Tex, &frame2Tex, &fieldTex, &trustTex })
				pl_tex_destroy(gpu, t);
			pl_mpv_user_shader_destroy(&ssim);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Q#2 (GEM-070 / ADS-STATE, WARP GPU):
		// Residual stabilisation must invalidate the previous residual when motion.sourceCrop shifts
		// even if the output dimensions (dw x dh) stay unchanged — both on a pure crop-origin shift
		// ({0, 0, 128, 96} -> {16, 16, 144, 112}) and on a crop-extent shift ({0, 0, 160, 128} -> {0, 0, 128, 96}),
		// including on a subsequent repeat of Frame 2.
		// Mandatory in-test negative control: exact same Frame 1 -> Frame 2 with UNCHANGED sourceCrop
		// MUST blend Frame 1's residual (> 0.01 difference from fresh Frame 2).
		TEST_METHOD(ResidualHoldStopsOnASourceCropShift)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size());
			Assert::IsNotNull(ssim, L"SSimDownscaler.glsl must parse");

			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt fieldFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt trustFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsTrue(srcFmt != nullptr && dstFmt != nullptr && fieldFmt != nullptr && trustFmt != nullptr,
				L"WARP must provide float32 source, target, motion field, and frame-trust formats");

			static constexpr int sw = 160, sh = 128, dw = 64, dh = 48;
			static constexpr int bw = sw / 16, bh = sh / 16;

			const pl_rect2df cropBase  = { 0.0f,  0.0f, 128.0f,  96.0f };
			const pl_rect2df cropShift = { 16.0f, 16.0f, 144.0f, 112.0f };
			const pl_rect2df cropZoom  = { 0.0f,  0.0f, 160.0f, 128.0f };

			auto makeSourceTex = [&](const std::function<float(int, int)>& pattern) -> pl_tex
			{
				std::vector<float> rgba(static_cast<size_t>(sw) * sh * 4, 0.0f);
				for (int y = 0; y < sh; ++y)
				{
					for (int x = 0; x < sw; ++x)
					{
						const float v = pattern(x, y);
						float* p = &rgba[(static_cast<size_t>(y) * sw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 1.0f;
					}
				}
				pl_tex_params tp{};
				tp.w = sw;
				tp.h = sh;
				tp.format = srcFmt;
				tp.sampleable = true;
				tp.initial_data = rgba.data();
				pl_tex tex = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(tex, L"RGBA32F source texture creation failed");
				return tex;
			};

			pl_tex frame1Tex = makeSourceTex([](int x, int y) -> float
			{
				if (x < sw / 2)
					return (((x / 2) + (y / 2)) % 2) != 0 ? 0.90f : 0.10f;
				return ((x / 2) % 2) != 0 ? 0.90f : 0.10f;
			});

			pl_tex frame2Tex = makeSourceTex([](int, int y) -> float
			{
				if (y < sh / 2)
					return 0.50f;
				return ((y / 2) % 2) != 0 ? 0.72f : 0.28f;
			});

			std::vector<float> fieldData(static_cast<size_t>(bw) * bh * 4, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(bw) * bh; ++i)
			{
				fieldData[i * 4 + 0] = 0.0f;
				fieldData[i * 4 + 1] = 0.0f;
				fieldData[i * 4 + 2] = 1.0f;
				fieldData[i * 4 + 3] = 0.0f;
			}
			pl_tex_params fp{};
			fp.w = bw;
			fp.h = bh;
			fp.format = fieldFmt;
			fp.sampleable = true;
			fp.initial_data = fieldData.data();
			pl_tex fieldTex = pl_tex_create(gpu, &fp);
			Assert::IsNotNull(fieldTex, L"motion field texture creation failed");

			const float trustVal = 1.0f;
			pl_tex_params trp{};
			trp.w = 1;
			trp.h = 1;
			trp.format = trustFmt;
			trp.sampleable = true;
			trp.initial_data = &trustVal;
			pl_tex trustTex = pl_tex_create(gpu, &trp);
			Assert::IsNotNull(trustTex, L"frame trust texture creation failed");

			auto noCutSource = [](uint64_t, uint64_t)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = false;
				return v;
			};

			auto renderCrop = [&](AdaptiveDownscale::Module& module, const AdaptiveDownscale::Settings& st,
				uint64_t seq, pl_tex src, const pl_rect2df& crop, bool withMotion) -> std::vector<float>
			{
				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target, L"RGBA32F target texture creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.crop = crop;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				image.color.transfer = out.color.transfer = PL_COLOR_TRC_GAMMA22;
				image.color.primaries = out.color.primaries = PL_COLOR_PRIM_BT_709;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;

				module.SetFrame(st, seq, 1.0f / 24.0f);
				if (withMotion)
				{
					AdaptiveDownscale::Motion m;
					m.field = fieldTex;
					m.frameTrust = trustTex;
					m.minTrustedShare = 0.25f;
					m.sourceCrop = crop;
					module.SetMotion(m);
				}
				else
				{
					module.ClearMotion();
				}

				const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
				params.hooks = hooks;
				params.num_hooks = 3;

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");

				std::vector<float> px(static_cast<size_t>(dw) * dh * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = target;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"RGBA32F target download failed");
				pl_tex_destroy(gpu, &target);
				return px;
			};

			auto maxRgbAbsDiff = [](const std::vector<float>& a, const std::vector<float>& b) -> float
			{
				Assert::AreEqual(a.size(), b.size(), L"output frame sizes must match");
				float worst = 0.0f;
				for (size_t i = 0; i < a.size(); ++i)
				{
					Assert::IsTrue(std::isfinite(a[i]) && std::isfinite(b[i]), L"output pixels must remain finite");
					if (i % 4 == 3)
						continue;
					const float d = std::fabs(a[i] - b[i]);
					if (d > worst)
						worst = d;
				}
				return worst;
			};

			auto makeSettings = [](bool stabilise, bool grainGuard)
			{
				AdaptiveDownscale::Settings st;
				st.wBase = 1.0f;
				st.edgeLo = 10000.0f;
				st.edgeHi = 20000.0f;
				st.grainLo = 10000.0f;
				st.grainHi = 20000.0f;
				st.shadowBias = 0.0f;
				st.grainSmoothMs = 0.0f;
				st.grainMap = false;
				st.ssimResidualStabilise = stabilise;
				st.residualGrainGuard = grainGuard;
				st.residualCodingNoiseCodes = 0.0f;
				st.float32Picture = true;
				return st;
			};

			for (bool grainGuard : { false, true })
			{
				const std::wstring modeLabel = grainGuard ? L"[residualGrainGuard=true] " : L"[residualGrainGuard=false] ";
				const AdaptiveDownscale::Settings stStab = makeSettings(true, grainGuard);
				const AdaptiveDownscale::Settings stNoStab = makeSettings(false, grainGuard);

				std::vector<float> freshAtBase;
				std::vector<float> freshAtShift;
				{
					AdaptiveDownscale::Module refMod;
					refMod.SetCutSource(noCutSource);
					freshAtBase = renderCrop(refMod, stNoStab, 1, frame2Tex, cropBase, false);
					freshAtShift = renderCrop(refMod, stNoStab, 2, frame2Tex, cropShift, false);
					Assert::IsTrue(refMod.Active() && refMod.Failures() == 0,
						(modeLabel + L"fresh Frame 2 crop references must render cleanly").c_str());
					refMod.Release();
				}

				// 1. Pure origin shift (same crop WxH 128x96 and same output WxH 64x48):
				//    {0, 0, 128, 96} -> {16, 16, 144, 112} with cut = false.
				{
					AdaptiveDownscale::Module shiftMod;
					shiftMod.SetCutSource(noCutSource);
					renderCrop(shiftMod, stStab, 1, frame1Tex, cropBase, true);
					const std::vector<float> shiftedFrame2 = renderCrop(shiftMod, stStab, 2, frame2Tex, cropShift, true);
					const std::vector<float> shiftedFrame2Repeat = renderCrop(shiftMod, stStab, 2, frame2Tex, cropShift, true);
					Assert::IsTrue(shiftMod.Active() && shiftMod.Failures() == 0,
						(modeLabel + L"crop-shift module must stay active with zero failures").c_str());

					const float shiftDiff = maxRgbAbsDiff(shiftedFrame2, freshAtShift);
					const float repeatDiff = maxRgbAbsDiff(shiftedFrame2Repeat, freshAtShift);
					Assert::IsTrue(shiftDiff <= 1e-4f,
						(modeLabel + L"sourceCrop origin shift must stop Frame-1 residual bleed (expected <= 1e-4, got " +
							std::to_wstring(shiftDiff) + L")").c_str());
					Assert::IsTrue(repeatDiff <= 1e-4f,
						(modeLabel + L"repeat after sourceCrop shift must have zero Frame-1 residual bleed (expected <= 1e-4, got " +
							std::to_wstring(repeatDiff) + L")").c_str());
					shiftMod.Release();
				}

				// 2. Crop extent/zoom shift (same origin (0, 0) and same output WxH 64x48):
				//    {0, 0, 160, 128} -> {0, 0, 128, 96} with cut = false.
				{
					AdaptiveDownscale::Module zoomMod;
					zoomMod.SetCutSource(noCutSource);
					renderCrop(zoomMod, stStab, 1, frame1Tex, cropZoom, true);
					const std::vector<float> zoomedFrame2 = renderCrop(zoomMod, stStab, 2, frame2Tex, cropBase, true);
					Assert::IsTrue(zoomMod.Active() && zoomMod.Failures() == 0,
						(modeLabel + L"crop-extent shift module must stay active with zero failures").c_str());

					const float zoomDiff = maxRgbAbsDiff(zoomedFrame2, freshAtBase);
					Assert::IsTrue(zoomDiff <= 1e-4f,
						(modeLabel + L"sourceCrop extent shift must stop Frame-1 residual bleed (expected <= 1e-4, got " +
							std::to_wstring(zoomDiff) + L")").c_str());
					zoomMod.Release();
				}

				// 3. MANDATORY IN-TEST NEGATIVE CONTROL (exact same Frame 1 -> Frame 2, cut = false,
				//    UNCHANGED sourceCrop = cropBase): Frame 2 MUST blend in Frame 1's residual (> 0.01).
				{
					AdaptiveDownscale::Module steadyMod;
					steadyMod.SetCutSource(noCutSource);
					renderCrop(steadyMod, stStab, 1, frame1Tex, cropBase, true);
					const std::vector<float> steadyFrame2 = renderCrop(steadyMod, stStab, 2, frame2Tex, cropBase, true);
					Assert::IsTrue(steadyMod.Active() && steadyMod.Failures() == 0,
						(modeLabel + L"unchanged-crop negative control must stay active with zero failures").c_str());
					Assert::IsTrue(steadyMod.StabilisedFrames() >= 1ull,
						(modeLabel + L"unchanged-crop negative control must stabilise Frame 2").c_str());

					const float steadyDiff = maxRgbAbsDiff(steadyFrame2, freshAtBase);
					Assert::IsTrue(steadyDiff > 0.01f,
						(modeLabel + L"negative control (unchanged sourceCrop): Frame 2 must hold Frame-1 residual (> 0.01), got " +
							std::to_wstring(steadyDiff)).c_str());
					steadyMod.Release();
				}
			}

			for (pl_tex* t : { &frame1Tex, &frame2Tex, &fieldTex, &trustTex })
				pl_tex_destroy(gpu, t);
			pl_mpv_user_shader_destroy(&ssim);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Q#2 absorbed W1B MUST-6 test (WARP GPU):
		// When 32-bit float picture precision is requested (float32Picture = true) and the GPU has no renderable
		// 32-bit float format, ScalerPass falls back to 16-bit float, AdaptiveDownscale::Module::HalfFloatPictureFallback()
		// turns true, and AdaptiveDownscaleStage::Stage surfaces the fallback visibly in BOTH StatusLines() and
		// Fallback() ("output scaling" catalog entry).
		// Mandatory negative controls:
		//  (a) Switching the feature off (requested = false, either adaptiveDownscale = false or slotRuns = false)
		//      while module.HalfFloatPictureFallback() is still latched true suppresses BOTH StatusLines() and Fallback()
		//      (no stale fallback).
		//  (b) Explicitly requesting 16-bit float (float32Picture = false) does not raise the fallback.
		//  (c) Restoring renderable 32-bit float on the GPU clears HalfFloatPictureFallback(), StatusLines(), and Fallback().
		TEST_METHOD(HalfFloatPictureFallbackIsVisible)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size());
			Assert::IsNotNull(ssim, L"SSimDownscaler.glsl must parse");

			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt != nullptr && dstFmt != nullptr, L"WARP must provide rgba16 source and rgba8 target formats");

			static constexpr int sw = 128, sh = 96, dw = 64, dh = 48;
			std::vector<uint16_t> srcPixels(static_cast<size_t>(sw) * sh * 4);
			for (int y = 0; y < sh; ++y)
			{
				for (int x = 0; x < sw; ++x)
				{
					const uint16_t v = static_cast<uint16_t>(((x + y) % 2) != 0 ? 48000u : 16000u);
					uint16_t* p = &srcPixels[(static_cast<size_t>(y) * sw + x) * 4];
					p[0] = p[1] = p[2] = v;
					p[3] = 65535u;
				}
			}
			pl_tex_params stp{};
			stp.w = sw;
			stp.h = sh;
			stp.format = srcFmt;
			stp.sampleable = true;
			stp.initial_data = srcPixels.data();
			pl_tex srcTex = pl_tex_create(gpu, &stp);
			Assert::IsNotNull(srcTex, L"source texture creation failed");

			struct ScopedDisableFloat32Renderable
			{
				struct Saved { struct pl_fmt_t* fmt; enum pl_fmt_caps caps; };
				std::vector<Saved> saved;
				explicit ScopedDisableFloat32Renderable(pl_gpu g)
				{
					for (int i = 0; i < g->num_formats; ++i)
					{
						struct pl_fmt_t* f = const_cast<struct pl_fmt_t*>(g->formats[i]);
						if (f && f->type == PL_FMT_FLOAT && f->component_depth[0] >= 32 &&
							(f->caps & PL_FMT_CAP_RENDERABLE) != 0)
						{
							Saved entry;
							entry.fmt = f;
							entry.caps = f->caps;
							saved.push_back(entry);
							f->caps = static_cast<enum pl_fmt_caps>(
								static_cast<unsigned>(f->caps) &
								~static_cast<unsigned>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_BLENDABLE));
						}
					}
				}
				void Restore()
				{
					for (size_t i = 0; i < saved.size(); ++i)
						saved[i].fmt->caps = saved[i].caps;
					saved.clear();
				}
				~ScopedDisableFloat32Renderable() { Restore(); }
			};

			const std::string kExpectedStatusLine =
				"Adaptive scaler: 16-bit float picture (32-bit float not available)";
			const std::string kExpectedFallbackReason =
				"adaptive scaler: 16-bit float picture (32-bit float not available)";

			auto hasStatusLine = [](const std::vector<std::string>& lines, const std::string& target) -> bool
			{
				return std::find(lines.begin(), lines.end(), target) != lines.end();
			};

			AdaptiveDownscale::Module module;
			// Known cut source so CutAvailable() is true and never shadows HalfFloatPictureFallback() in Fallback().
			module.SetCutSource([](uint64_t, uint64_t)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = false;
				return v;
			});

			AdaptiveDownscaleStage::Stage stage{ module };
			AdaptiveDownscaleStage::Host host;
			host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
			host.bitrate = [](double& q) { q = 1.0; return false; };
			host.nlsMapping = []() { return NlsMaskMapping::Mapping(); };
			host.ssimInChain = []() { return true; };
			host.motion = []() { return AdaptiveDownscaleStage::MotionSource(); };
			host.publishStatus = []() {};

			double weight = 1.0;
			bool grainWanted = false, noSsim = false;
			uint64_t sequence = 0;

			auto runStageFrame = [&](const AdaptiveDownscale::Settings& st, bool adaptiveDownscale,
				bool slotRuns, int fboFloat32, bool render)
			{
				AdaptiveDownscaleStage::Frame f;
				f.adaptiveDownscale = adaptiveDownscale;
				f.adaptiveUpscale = false;
				f.slotRuns = slotRuns;
				f.adaptive = &st;
				f.lowBitrateOff = true;
				f.fboFloat32 = fboFloat32;
				f.sourceSequence = ++sequence;
				f.nominalSourceRateHz = 24.0;
				f.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				f.targetCrop = { 0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh) };

				std::vector<const pl_hook*> hooks = { ssim };
				stage.Register(f, host, { weight, grainWanted, noSsim }, hooks);
				if (!render)
				{
					Assert::AreEqual(size_t(1), hooks.size(), L"feature off: stage must not register adaptive hooks");
					return;
				}
				Assert::IsTrue(hooks.size() == 3 && hooks.front() == module.Measure() && hooks.back() == module.Blend(),
					L"downscale stage must register measure, SSIM, blend");

				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target, L"target texture creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(srcTex);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				image.color.transfer = out.color.transfer = PL_COLOR_TRC_GAMMA22;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;
				params.hooks = hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				pl_tex_destroy(gpu, &target);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");
			};

			AdaptiveDownscale::Settings stF32;
			stF32.float32Picture = true;
			stF32.grainMap = false;
			stF32.ssimResidualStabilise = false;

			{
				ScopedDisableFloat32Renderable hideFloat32(gpu);
				Assert::IsFalse(hideFloat32.saved.empty(),
					L"precondition: WARP must initially expose at least one renderable 32-bit float format");

				// 1. Positive case: 32-bit float requested, GPU lacks renderable 32-bit float -> ScalerPass
				//    falls back to 16-bit float -> module.HalfFloatPictureFallback() is true -> visible in
				//    both StatusLines() and Fallback() ("output scaling").
				runStageFrame(stF32, true, true, 1, true);
				Assert::IsTrue(module.Active() && module.Failures() == 0,
					L"half-float fallback frame must succeed with zero failures");
				Assert::IsTrue(module.CutAvailable(), L"precondition: cut verdict must remain available");
				Assert::IsTrue(module.HalfFloatPictureFallback(),
					L"module.HalfFloatPictureFallback() must be true when 32-bit float is unavailable");
				Assert::IsTrue(hasStatusLine(stage.StatusLines(), kExpectedStatusLine),
					L"StatusLines() must emit the 16-bit float picture fallback line when requested");
				{
					const FallbackCatalog::Decision fb = stage.Fallback();
					Assert::IsTrue(fb.engaged, L"Fallback() must be engaged when HalfFloatPictureFallback() is true and requested");
					Assert::AreEqual(std::string("output scaling"), fb.id, L"Fallback() catalog id must be 'output scaling'");
					Assert::AreEqual(kExpectedFallbackReason, fb.reason, L"Fallback() reason must match half-float picture fallback");
				}

				// 2. MANDATORY NEGATIVE CONTROL (requested = false, feature off while module flag is still true):
				//    neither StatusLines() nor Fallback() may emit a stale half-float fallback.
				runStageFrame(stF32, false, true, 1, false);
				Assert::IsTrue(module.HalfFloatPictureFallback(),
					L"precondition: module flag remains latched until next measure (proving stage gating is necessary)");
				Assert::IsFalse(hasStatusLine(stage.StatusLines(), kExpectedStatusLine),
					L"negative control (adaptiveDownscale=false): StatusLines() must not emit stale half-float fallback");
				Assert::IsFalse(stage.Fallback().engaged,
					L"negative control (adaptiveDownscale=false): Fallback() must not emit stale half-float fallback");

				// Also check slotRuns = false (requested = false while adaptiveDownscale = true).
				runStageFrame(stF32, true, false, 1, false);
				Assert::IsTrue(module.HalfFloatPictureFallback(),
					L"precondition: module flag still latched when slot is off");
				Assert::IsFalse(hasStatusLine(stage.StatusLines(), kExpectedStatusLine),
					L"negative control (slotRuns=false): StatusLines() must not emit stale half-float fallback");
				Assert::IsFalse(stage.Fallback().engaged,
					L"negative control (slotRuns=false): Fallback() must not emit stale half-float fallback");

				// 3. Negative control: explicitly requesting 16-bit float (float32Picture = false) must NOT trigger fallback.
				AdaptiveDownscale::Settings stF16 = stF32;
				stF16.float32Picture = false;
				runStageFrame(stF16, true, true, 0, true);
				Assert::IsTrue(module.Active() && module.Failures() == 0, L"explicit 16-bit float frame must succeed");
				Assert::IsFalse(module.HalfFloatPictureFallback(),
					L"negative control (float32Picture=false): explicit 16-bit float request is not a fallback");
				Assert::IsFalse(hasStatusLine(stage.StatusLines(), kExpectedStatusLine),
					L"negative control (float32Picture=false): StatusLines() must not emit fallback line");
				Assert::IsFalse(stage.Fallback().engaged,
					L"negative control (float32Picture=false): Fallback() must not be engaged");

				// Restore renderable 32-bit float formats on WARP.
				hideFloat32.Restore();
			}

			// 4. Negative control: with renderable 32-bit float available on WARP, requesting 32-bit float succeeds
			//    without fallback and clears StatusLines() and Fallback().
			runStageFrame(stF32, true, true, 1, true);
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"32-bit float frame on WARP must succeed");
			Assert::IsFalse(module.HalfFloatPictureFallback(),
				L"negative control (32-bit float available): HalfFloatPictureFallback() must be false");
			Assert::IsFalse(hasStatusLine(stage.StatusLines(), kExpectedStatusLine),
				L"negative control (32-bit float available): StatusLines() must not emit fallback line");
			Assert::IsFalse(stage.Fallback().engaged,
				L"negative control (32-bit float available): Fallback() must not be engaged");

			module.Release();
			pl_tex_destroy(gpu, &srcTex);
			pl_mpv_user_shader_destroy(&ssim);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}
	};
}
