// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(ResidualHoldHookTests)
	{
	public:


		TEST_METHOD(CnnResidualHeldAlongTrustedVectors)
		{
			// Stage 12 §2c (owner 2026-10-15): the CNN upscaler's added detail held
			// along the de-judder's trusted vectors. A stand-in CNN (nearest 2x
			// doubler) on a texture whose fine detail changes every frame: the
			// residual (output minus the faithful branch) flickers less when held;
			// off and an untrusted field are today's output exactly.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			const int width = 128, height = 64, ow = width * 3 / 2, oh = height * 3 / 2;
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
			auto makeField = [&](float trust) {
				std::vector<float> f(static_cast<size_t>((width + 15) / 16) * ((height + 15) / 16) * 4);
				for (size_t i = 0; i < f.size(); i += 4) { f[i] = 0.0f; f[i + 1] = 0.0f; f[i + 2] = trust; f[i + 3] = 0.0f; }
				pl_tex_params p{};
				p.w = (width + 15) / 16; p.h = (height + 15) / 16; p.format = pl_find_named_fmt(gpu, "rgba32f"); p.sampleable = true; p.initial_data = f.data();
				return pl_tex_create(gpu, &p);
			};
			const float trustValue[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
			pl_tex_params tp1{};
			tp1.w = 1; tp1.h = 1; tp1.format = pl_find_named_fmt(gpu, "rgba32f"); tp1.sampleable = true; tp1.initial_data = trustValue;
			pl_tex frameTrust = pl_tex_create(gpu, &tp1);
			pl_tex trusted = makeField(1.0f), untrusted = makeField(0.0f);
			Assert::IsTrue(frameTrust && trusted && untrusted);
			// Frame f: the texture plus fine detail that changes every frame.
			auto frameOf = [&](int f) {
				return MakeDejudderFrame(width, height,
					[f](int x, int y) { return static_cast<uint16_t>((DejudderTexture(x, y) * 7 + DejudderTexture(x, y, 1000u + static_cast<unsigned>(f))) / 8); },
					[](int, int, int) { return static_cast<uint16_t>(32768); });
			};
			auto render = [&](const DejudderFrame& frame, std::vector<const pl_hook*> hooks) {
				DejudderGpuPlanes planes;
				UploadDejudderFrame(gpu, frame, planes);
				pl_tex_params tp{};
				tp.w = ow; tp.h = oh; tp.format = rgba16; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planes.planes.luma; image.planes[0].components = 1; image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planes.planes.chroma; image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB; image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709; image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709; image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame t = TargetLutGpuFixture::MakeRgbFrame(target);
				t.repr.levels = PL_COLOR_LEVELS_FULL; t.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_render_params params = pl_render_fast_params;
				params.dither_params = nullptr; params.error_diffusion = nullptr; params.sigmoid_params = nullptr;
				params.disable_linear_scaling = true; params.upscaler = &pl_filter_ewa_lanczossharp; params.antiringing_strength = 0.5f;
				params.hooks = hooks.empty() ? nullptr : hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &params));
				pl_gpu_finish(gpu);
				std::vector<uint16_t> px(static_cast<size_t>(ow) * oh * 4);
				pl_tex_transfer_params d{}; d.tex = target; d.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			const int frames = 10;
			std::vector<std::vector<uint16_t>> faithful;
			for (int f = 0; f < frames; ++f) faithful.push_back(render(frameOf(f), {}));
			auto run = [&](bool stabilise, pl_tex field) {
				AdaptiveDownscale::Module module;
				AdaptiveDownscale::Settings st;
				st.grainMap = false;
				st.grainLo = 1000.0f; st.grainHi = 2000.0f;   // no grain: w = w_base
				st.grainSmoothMs = 0.0f;
				st.cnnResidualStabilise = stabilise;
				st.residualGrainGuard = false;   // the 2026-10-15 one-pole (SsimResidualStabiliseLeavesGrainBlocks: the guard)
				std::vector<std::vector<uint16_t>> out;
				for (int f = 0; f < frames; ++f)
				{
					module.SetFrame(st, static_cast<uint64_t>(500 + f), 1.0f / 24.0f);
					AdaptiveDownscale::Upscale up;
					up.on = true; up.wBase = 1.0f; up.nativeHeight = static_cast<float>(height);
					module.SetUpscale(up);
					module.ClearMotion();
					if (field)
					{
						AdaptiveDownscale::Motion m;
						m.field = field; m.frameTrust = frameTrust; m.minTrustedShare = 0.25f;
						m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
						module.SetMotion(m);
					}
					out.push_back(render(frameOf(f), { module.Measure(), cnn, module.Blend() }));
				}
				const unsigned long long held = module.StabilisedFrames();
				module.Release();
				return std::make_pair(out, held);
			};
			auto flickerOf = [&](const std::vector<std::vector<uint16_t>>& o) {
				double sum = 0; size_t n = 0;
				for (int f = 3; f < frames; ++f)
					for (size_t i = 0; i < o[f].size(); i += 4)
					{
						const double now = double(o[f][i]) - double(faithful[f][i]), before = double(o[f - 1][i]) - double(faithful[f - 1][i]);
						sum += std::fabs(now - before);
						++n;
					}
				return sum / n / 257.0;
			};
			auto maxDiff = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
				int m = 0;
				for (size_t i = 0; i < a.size(); ++i) if (i % 4 != 3) m = (std::max)(m, std::abs(int(a[i]) - int(b[i])));
				return m;
			};
			const auto today = run(false, nullptr);
			const auto held = run(true, trusted);
			const auto untrustedRun = run(true, untrusted);
			const auto offWithField = run(false, trusted);
			char line[200];
			snprintf(line, sizeof(line), "CNN residual flicker %.3f (today) -> %.3f (held, %llu frames); untrusted %.3f\n",
				flickerOf(today.first), flickerOf(held.first), held.second, flickerOf(untrustedRun.first));
			Logger::WriteMessage(line);
			Assert::IsTrue(held.second >= 8, L"held on the consecutive frames");
			Assert::IsTrue(flickerOf(held.first) < 0.7 * flickerOf(today.first), L"the CNN residual flickers less when held");
			Assert::AreEqual(0, maxDiff(untrustedRun.first.back(), today.first.back()), L"untrusted: today's output exactly");
			Assert::AreEqual(0, maxDiff(offWithField.first.back(), today.first.back()), L"off: today's output exactly");
			pl_tex_destroy(gpu, &frameTrust); pl_tex_destroy(gpu, &trusted); pl_tex_destroy(gpu, &untrusted);
			pl_mpv_user_shader_destroy(&cnn);
		}

		TEST_METHOD(SsimResidualHeldAlongTrustedVectors)
		{
			// Delta 15 S8-1 / Stage 12 §2c (owner 2026-10-15): the SSIM downscaler's
			// added contrast held along the de-judder's trusted vectors. A
			// flickering texture on a trusted (still) field flickers less; a still
			// texture keeps today's output; an untrusted field is today's output
			// exactly; off is today's output exactly.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			static constexpr int width = 256, height = 128;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			// Texture: noise plus a gentle pattern; `flicker` changes the fine noise.
			auto picture = [&](unsigned seed, double flickerAmount) {
				std::mt19937 rng(77), flick(seed);
				std::normal_distribution<double> base(0.0, 0.04), change(0.0, 1.0);
				std::vector<uint16_t> px(static_cast<size_t>(width) * height * 4);
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width; ++x)
					{
						double v = 0.45 + 0.1 * std::sin(x * 0.11) * std::cos(y * 0.07) + base(rng) + flickerAmount * change(flick);
						v = (std::min)(1.0, (std::max)(0.0, v));
						const size_t i = (static_cast<size_t>(y) * width + x) * 4;
						px[i] = px[i + 1] = px[i + 2] = static_cast<uint16_t>(std::lround(v * 65535.0));
						px[i + 3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = width; tp.h = height; tp.format = srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			};
			// A still field (zero motion) and the frame trust: trusted or not.
			auto makeField = [&](float trust) {
				std::vector<float> f(static_cast<size_t>(width / 16) * (height / 16) * 4);
				for (size_t i = 0; i < f.size(); i += 4) { f[i] = 0.0f; f[i + 1] = 0.0f; f[i + 2] = trust; f[i + 3] = 0.0f; }
				pl_tex_params p{};
				p.w = width / 16; p.h = height / 16; p.format = pl_find_named_fmt(gpu, "rgba32f"); p.sampleable = true; p.initial_data = f.data();
				return pl_tex_create(gpu, &p);
			};
			const float trustValue[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
			pl_tex_params tp1{};
			tp1.w = 1; tp1.h = 1; tp1.format = pl_find_named_fmt(gpu, "rgba32f"); tp1.sampleable = true; tp1.initial_data = trustValue;
			pl_tex frameTrust = pl_tex_create(gpu, &tp1);
			pl_tex trusted = makeField(1.0f), untrusted = makeField(0.0f);
			Assert::IsTrue(frameTrust && trusted && untrusted);

			auto run = [&](bool stabilise, pl_tex field, bool flicker, int frames, float wBase = -1.0f) {
				AdaptiveDownscale::Module module;
				AdaptiveDownscale::Settings st;
				st.grainSmoothMs = 0.0f;
				st.grainMap = false;
				st.ssimResidualStabilise = stabilise;
				st.residualGrainGuard = false;   // the 2026-10-15 one-pole (SsimResidualStabiliseLeavesGrainBlocks: the guard)
				if (wBase >= 0.0f) st.wBase = wBase;   // 0: the faithful branch alone
				std::vector<std::vector<uint16_t>> outputs;
				for (int f = 0; f < frames; ++f)
				{
					pl_tex src = picture(static_cast<unsigned>(1000 + f), flicker ? 0.03 : 0.0);
					module.SetFrame(st, static_cast<uint64_t>(10 + f), 1.0f / 24.0f);
					module.ClearMotion();
					if (field)
					{
						AdaptiveDownscale::Motion m;
						m.field = field;
						m.frameTrust = frameTrust;
						m.minTrustedShare = 0.25f;
						m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
						module.SetMotion(m);
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
					params.hooks = hooks;
					params.num_hooks = 3;
					Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
					pl_gpu_finish(gpu);
					std::vector<uint16_t> px(static_cast<size_t>(width / 2) * (height / 2) * 4);
					pl_tex_transfer_params dl{}; dl.tex = target; dl.ptr = px.data();
					Assert::IsTrue(pl_tex_download(gpu, &dl));
					outputs.push_back(px);
					pl_tex_destroy(gpu, &target);
					pl_tex_destroy(gpu, &src);
				}
				const unsigned long long held = module.StabilisedFrames();
				module.Release();
				return std::make_pair(outputs, held);
			};
			// The faithful branch alone on the same frames: the residual is the
			// output minus it. Frame-to-frame change of the residual (8-bit codes).
			const auto faithfulFlicker = run(false, nullptr, true, 10, 0.0f);
			auto flickerOf = [&](const std::vector<std::vector<uint16_t>>& o) {
				const auto& fa = faithfulFlicker.first;
				double sum = 0; size_t n = 0;
				for (size_t f = 3; f < o.size(); ++f)
					for (size_t i = 0; i < o[f].size(); i += 4)
					{
						const double now = double(o[f][i]) - double(fa[f][i]), before = double(o[f - 1][i]) - double(fa[f - 1][i]);
						sum += std::fabs(now - before);
						++n;
					}
				return sum / n / 257.0;
			};
			auto maxDiff = [&](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
				double m = 0;
				size_t at = 0, over = 0;
				for (size_t i = 0; i < a.size(); ++i)
				{
					const double d = std::fabs(double(a[i]) - double(b[i]));
					if (d > m) { m = d; at = i; }
					over += d > 257.0;
				}
				if (m > 257.0)
				{
					char where[160];
					snprintf(where, sizeof(where), "  max at pixel (%zu, %zu) channel %zu: %u vs %u; %zu values differ by more than a code\n",
						(at / 4) % (width / 2), (at / 4) / (width / 2), at % 4, a[at], b[at], over);
					Logger::WriteMessage(where);
				}
				return m / 257.0;
			};
			const auto todayFlicker = run(false, nullptr, true, 10);
			const auto heldFlicker = run(true, trusted, true, 10);
			const auto untrustedFlicker = run(true, untrusted, true, 10);
			const auto offWithField = run(false, trusted, true, 10);
			const auto todayStill = run(false, nullptr, false, 8);
			const auto heldStill = run(true, trusted, false, 8);
			char line[240];
			snprintf(line, sizeof(line), "SSIM residual flicker %.3f (today) -> %.3f (held, %llu frames held); untrusted %.3f; still picture held vs today max %.3f 8-bit codes\n",
				flickerOf(todayFlicker.first), flickerOf(heldFlicker.first), heldFlicker.second, flickerOf(untrustedFlicker.first),
				maxDiff(heldStill.first.back(), todayStill.first.back()));
			Logger::WriteMessage(line);
			Assert::IsTrue(heldFlicker.second >= 8, L"held on the consecutive frames");
			Assert::IsTrue(flickerOf(heldFlicker.first) < 0.7 * flickerOf(todayFlicker.first), L"the residual flickers less when held");
			// (The count is frames with a previous residual; trust is decided per pixel in the pass.)
			Assert::AreEqual(0.0, maxDiff(untrustedFlicker.first.back(), todayFlicker.first.back()), L"untrusted: today's output exactly");
			Assert::AreEqual(0.0, maxDiff(offWithField.first.back(), todayFlicker.first.back()), L"off: today's output exactly");
			Assert::IsTrue(maxDiff(heldStill.first.back(), todayStill.first.back()) <= 1.0, L"a still picture keeps today's output (within a code)");
			pl_tex_destroy(gpu, &frameTrust); pl_tex_destroy(gpu, &trusted); pl_tex_destroy(gpu, &untrusted);
			pl_mpv_user_shader_destroy(&ssim);
		}

		TEST_METHOD(SsimResidualStabiliseLeavesGrainBlocks)
		{
			// residual_stabilise_grain_guard: on a picture whose grain is fresh every frame, the
			// SSIM residual held along trusted (still) vectors passes this frame's residual
			// through where the grain term marks grain - today's (unheld) output within a code and
			// the same frame-to-frame change; off is the established one-pole (it steadies the
			// residual, averaging the grain), twice the same bytes.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			static constexpr int width = 256, height = 128;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			auto picture = [&](unsigned seed) {
				std::mt19937 grain(seed);
				std::normal_distribution<double> n(0.0, 0.03);   // fresh grain, about 8 codes
				std::vector<uint16_t> px(static_cast<size_t>(width) * height * 4);
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width; ++x)
					{
						const double v = (std::min)(1.0, (std::max)(0.0, 0.45 + 0.1 * std::sin(x * 0.11) * std::cos(y * 0.07) + n(grain)));
						const size_t i = (static_cast<size_t>(y) * width + x) * 4;
						px[i] = px[i + 1] = px[i + 2] = static_cast<uint16_t>(std::lround(v * 65535.0));
						px[i + 3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = width; tp.h = height; tp.format = srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			};
			std::vector<float> f(static_cast<size_t>(width / 16) * (height / 16) * 4);
			for (size_t i = 0; i < f.size(); i += 4) { f[i] = 0.0f; f[i + 1] = 0.0f; f[i + 2] = 1.0f; f[i + 3] = 0.0f; }
			pl_tex_params fp{};
			fp.w = width / 16; fp.h = height / 16; fp.format = pl_find_named_fmt(gpu, "rgba32f"); fp.sampleable = true; fp.initial_data = f.data();
			pl_tex trusted = pl_tex_create(gpu, &fp);
			const float trustValue[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
			pl_tex_params tp1{};
			tp1.w = 1; tp1.h = 1; tp1.format = pl_find_named_fmt(gpu, "rgba32f"); tp1.sampleable = true; tp1.initial_data = trustValue;
			pl_tex frameTrust = pl_tex_create(gpu, &tp1);
			Assert::IsTrue(trusted && frameTrust);
			auto run = [&](bool stabilise, bool guard, int frames) {
				AdaptiveDownscale::Module module;
				AdaptiveDownscale::Settings st;
				st.grainSmoothMs = 0.0f;
				st.grainMap = false;
				st.ssimResidualStabilise = stabilise;
				st.residualGrainGuard = guard;
				std::vector<std::vector<uint16_t>> outputs;
				for (int k = 0; k < frames; ++k)
				{
					pl_tex src = picture(static_cast<unsigned>(2000 + k));
					module.SetFrame(st, static_cast<uint64_t>(10 + k), 1.0f / 24.0f);
					module.ClearMotion();
					AdaptiveDownscale::Motion m;
					m.field = trusted;
					m.frameTrust = frameTrust;
					m.minTrustedShare = 0.25f;
					m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
					module.SetMotion(m);
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
					params.hooks = hooks;
					params.num_hooks = 3;
					Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
					pl_gpu_finish(gpu);
					std::vector<uint16_t> px(static_cast<size_t>(width / 2) * (height / 2) * 4);
					pl_tex_transfer_params dl{}; dl.tex = target; dl.ptr = px.data();
					Assert::IsTrue(pl_tex_download(gpu, &dl));
					outputs.push_back(px);
					pl_tex_destroy(gpu, &target);
					pl_tex_destroy(gpu, &src);
				}
				module.Release();
				return outputs;
			};
			auto change = [](const std::vector<std::vector<uint16_t>>& o) {
				double sum = 0; size_t n = 0;
				for (size_t k = 3; k < o.size(); ++k)
					for (size_t i = 0; i < o[k].size(); i += 4) { sum += std::fabs(double(o[k][i]) - double(o[k - 1][i])); ++n; }
				return sum / n / 257.0;
			};
			auto maxDiff = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
				int m = 0;
				for (size_t i = 0; i < a.size(); ++i) if (i % 4 != 3) m = (std::max)(m, std::abs(int(a[i]) - int(b[i])));
				return m / 257.0;
			};
			const int frames = 8;
			const auto today = run(false, true, frames);
			const auto guarded = run(true, true, frames);
			const auto legacy = run(true, false, frames);
			const auto legacyAgain = run(true, false, frames);
			char text[200];
			snprintf(text, sizeof(text), "frame-to-frame change: today %.3f, guard %.3f, legacy one-pole %.3f; guard vs today max %.2f codes",
				change(today), change(guarded), change(legacy), maxDiff(guarded.back(), today.back()));
			Logger::WriteMessage(text);
			const std::wstring wtext(text, text + strlen(text));
			Assert::IsTrue(maxDiff(guarded.back(), today.back()) <= 1.0, wtext.c_str());
			Assert::IsTrue(std::fabs(change(guarded) - change(today)) <= 0.05 * change(today), wtext.c_str());
			Assert::IsTrue(change(legacy) < change(today), (L"off: the one-pole holds (averages) the residual: " + wtext).c_str());
			for (int k = 0; k < frames; ++k)
				Assert::IsTrue(legacy[k] == legacyAgain[k], L"off: deterministic, the established pass");
			pl_tex_destroy(gpu, &frameTrust);
			pl_tex_destroy(gpu, &trusted);
			pl_mpv_user_shader_destroy(&ssim);
		}
	};
}
