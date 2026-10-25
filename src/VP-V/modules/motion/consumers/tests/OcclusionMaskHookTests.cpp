// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(OcclusionMaskHookTests)
	{
	public:


		TEST_METHOD(OcclusionMaskReachesTheSigmaDeltaThroughAlpha)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			pl_gpu gpu = fixture.Gpu();
			std::vector<const pl_hook*> sigmaHooks;
			CustomHookChain::Chain chain(SigmaLoader(fixture));
			ConfigureSigma(chain, 2, 2, sigmaHooks);
			Assert::AreEqual(size_t{ 1 }, sigmaHooks.size(), L"libplacebo rejected the sigma-delta");
			pl_tex seen = MakeMaskTexture(gpu, 1, 1, { 65535 });
			pl_tex fresh = MakeMaskTexture(gpu, 1, 1, { 0 });
			OcclusionMask::Hook mask;
			const pl_rect2df whole{ 0.0f, 0.0f, 2.0f, 2.0f };
			auto describe = [](const std::vector<int>& codes)
			{
				std::wstring text;
				for (int code : codes) text += std::to_wstring(code) + L" ";
				return text;
			};

			// 1. The mask hook alone does not change the picture.
			const std::vector<int> plain = fixture.RenderTenBitSequence({}, PL_COLOR_LEVELS_LIMITED, 0.5f, 1);
			mask.SetFrame(fresh, whole, whole);
			const std::vector<int> masked = fixture.RenderTenBitSequence({ mask.Get() }, PL_COLOR_LEVELS_LIMITED, 0.5f, 1);
			Assert::AreEqual(plain[0], masked[0], L"Writing the mask into alpha changed the picture");
			Assert::IsTrue(mask.Applied() > 0, L"The mask hook did not run");

			// 2. Mid grey (8-bit 125.5): the sigma-delta alternates 125/126. Mask 1
			// keeps that carry; mask 0 stops it (one code every refresh). Alpha out is 1.
			Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "occlusion_mask", 1.0));
			mask.SetFrame(seen, whole, whole);
			std::vector<int> alphas;
			const std::vector<int> carry = fixture.RenderTenBitSequence({ mask.Get(), sigmaHooks[0] },
				PL_COLOR_LEVELS_LIMITED, 0.5f, 8, &alphas);
			Assert::IsTrue(std::find(carry.begin(), carry.end(), 500) != carry.end() &&
				std::find(carry.begin(), carry.end(), 504) != carry.end(),
				(L"Mask 1 must keep the carry (125/126): " + describe(carry)).c_str());
			for (int alpha : alphas)
				Assert::AreEqual(3, alpha, L"The sigma-delta must write alpha back to 1");
			mask.SetFrame(fresh, whole, whole);
			alphas.clear();
			const std::vector<int> stopped = fixture.RenderTenBitSequence({ mask.Get(), sigmaHooks[0] },
				PL_COLOR_LEVELS_LIMITED, 0.5f, 8, &alphas);
			for (int code : stopped)
				Assert::AreEqual(stopped[0], code, (L"Mask 0 must stop the carry (one code every refresh): " +
					describe(stopped)).c_str());
			for (int alpha : alphas)
				Assert::AreEqual(3, alpha, L"The sigma-delta must write alpha back to 1");

			// 3. Without the parameter the sigma-delta ignores alpha (mask 0 present).
			Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "occlusion_mask", 0.0));
			const std::vector<int> ignored = fixture.RenderTenBitSequence({ mask.Get(), sigmaHooks[0] },
				PL_COLOR_LEVELS_LIMITED, 0.5f, 8);
			Assert::IsTrue(std::find(ignored.begin(), ignored.end(), 500) != ignored.end(),
				(L"occlusion_mask 0 must ignore alpha: " + describe(ignored)).c_str());
			pl_tex_destroy(gpu, &seen);
			pl_tex_destroy(gpu, &fresh);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Delta 5 (D5-4): the occlusion mask with full-range output. Mid grey
		// is 8-bit 127.5 there (10-bit 508/512): mask 1 keeps the carry, mask 0
		// stops it, and alpha goes back to 1.
		TEST_METHOD(OcclusionMaskWorksWithFullRangeOutput)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			pl_gpu gpu = fixture.Gpu();
			std::vector<const pl_hook*> sigmaHooks;
			CustomHookChain::Chain chain(SigmaLoader(fixture));
			ConfigureSigma(chain, 2, 2, sigmaHooks);
			Assert::AreEqual(size_t{ 1 }, sigmaHooks.size(), L"libplacebo rejected the sigma-delta");
			Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "levels_full", 1.0));
			Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "occlusion_mask", 1.0));
			pl_tex seen = MakeMaskTexture(gpu, 1, 1, { 65535 });
			pl_tex fresh = MakeMaskTexture(gpu, 1, 1, { 0 });
			OcclusionMask::Hook mask;
			const pl_rect2df whole{ 0.0f, 0.0f, 2.0f, 2.0f };
			auto describe = [](const std::vector<int>& codes)
			{
				std::wstring text;
				for (int code : codes) text += std::to_wstring(code) + L" ";
				return text;
			};
			mask.SetFrame(seen, whole, whole);
			std::vector<int> alphas;
			const std::vector<int> carry = fixture.RenderTenBitSequence({ mask.Get(), sigmaHooks[0] },
				PL_COLOR_LEVELS_FULL, 0.5f, 8, &alphas);
			Assert::IsTrue(std::find(carry.begin(), carry.end(), 508) != carry.end() &&
				std::find(carry.begin(), carry.end(), 512) != carry.end(),
				(L"Full range, mask 1 must keep the carry (127/128): " + describe(carry)).c_str());
			for (int alpha : alphas)
				Assert::AreEqual(3, alpha, L"The sigma-delta must write alpha back to 1");
			mask.SetFrame(fresh, whole, whole);
			const std::vector<int> stopped = fixture.RenderTenBitSequence({ mask.Get(), sigmaHooks[0] },
				PL_COLOR_LEVELS_FULL, 0.5f, 8);
			for (int code : stopped)
				Assert::AreEqual(stopped[0], code, (L"Full range, mask 0 must stop the carry: " +
					describe(stopped)).c_str());
			pl_tex_destroy(gpu, &seen);
			pl_tex_destroy(gpu, &fresh);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// The mask lands on the right output pixels after scaling (1.5x) and
		// edge blanking (16 px each side): source block column 1 (x 16..31)
		// is "new", so output x 40..63 (16 + 1.5 * [16, 32)) must stop the
		// carry while the rest of the picture keeps it.
		TEST_METHOD(OcclusionMaskAlignsWithThePictureAfterScalingAndBlanking)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			pl_gpu gpu = fixture.Gpu();
			const int srcW = 64, srcH = 32, outW = 128, outH = 64;
			std::vector<const pl_hook*> sigmaHooks;
			CustomHookChain::Chain chain(SigmaLoader(fixture));
			ConfigureSigma(chain, outW, outH, sigmaHooks);
			Assert::AreEqual(size_t{ 1 }, sigmaHooks.size());
			Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "occlusion_mask", 1.0));
			// 4 x 2 blocks; column 1 is new content.
			pl_tex maskTex = MakeMaskTexture(gpu, 4, 2, { 65535, 0, 65535, 65535, 65535, 0, 65535, 65535 });
			OcclusionMask::Hook mask;
			const pl_rect2df sourceCrop{ 0.0f, 0.0f, static_cast<float>(srcW), static_cast<float>(srcH) };
			const pl_rect2df targetCrop{ 16.0f, 0.0f, 112.0f, static_cast<float>(outH) };
			mask.SetFrame(maskTex, sourceCrop, targetCrop);

			pl_fmt sourceFormat = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt targetFormat = nullptr;
			const auto caps = PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE;
			for (int i = 0; i < gpu->num_formats; ++i)
			{
				pl_fmt f = gpu->formats[i];
				if (f->type == PL_FMT_UNORM && f->num_components == 4 && f->component_depth[0] == 10 &&
					f->texel_size == 4 && (f->caps & caps) == caps && f->host_bits[0] == 10)
				{ targetFormat = f; break; }
			}
			Assert::IsNotNull(targetFormat);
			std::vector<float> texels(static_cast<size_t>(srcW) * srcH * 4);
			for (size_t i = 0; i < texels.size(); i += 4)
			{
				texels[i] = texels[i + 1] = texels[i + 2] = 0.5f;
				texels[i + 3] = 1.0f;
			}
			pl_tex_params sp{};
			sp.w = srcW; sp.h = srcH; sp.format = sourceFormat; sp.sampleable = true; sp.initial_data = texels.data();
			pl_tex source = pl_tex_create(gpu, &sp);
			pl_tex_params tp{};
			tp.w = outW; tp.h = outH; tp.format = targetFormat; tp.renderable = true; tp.host_readable = true;
			tp.blit_dst = true;   // libplacebo clears outside the destination rectangle
			pl_tex target = pl_tex_create(gpu, &tp);
			pl_frame image = TargetLutGpuFixture::MakeRgbFrame(source);
			image.crop = sourceCrop;
			pl_frame output = TargetLutGpuFixture::MakeRgbFrame(target);
			output.repr.levels = PL_COLOR_LEVELS_LIMITED;
			output.repr.bits.sample_depth = 10;
			output.repr.bits.color_depth = 8;
			output.repr.bits.bit_shift = 2;
			output.crop = targetCrop;
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			const pl_hook* hooks[2] = { mask.Get(), sigmaHooks[0] };
			params.hooks = hooks;
			params.num_hooks = 2;
			const int frames = 8;
			std::vector<std::vector<uint32_t>> rendered;
			for (int frame = 0; frame < frames; ++frame)
			{
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &output, &params));
				pl_gpu_finish(gpu);
				std::vector<uint32_t> pixels(static_cast<size_t>(outW) * outH);
				pl_tex_transfer_params download{};
				download.tex = target;
				download.ptr = pixels.data();
				Assert::IsTrue(pl_tex_download(gpu, &download));
				rendered.push_back(std::move(pixels));
			}
			auto constant = [&](int x, int y)
			{
				const uint32_t first = rendered[0][static_cast<size_t>(y) * outW + x] & 1023u;
				for (int frame = 1; frame < frames; ++frame)
					if ((rendered[frame][static_cast<size_t>(y) * outW + x] & 1023u) != first)
						return false;
				return true;
			};
			for (int y : { 8, 40 })
			{
				for (int x = 42; x < 62; ++x)
					Assert::IsTrue(constant(x, y), (L"Masked block not at x " + std::to_wstring(x)).c_str());
				for (int x : { 20, 30, 37, 66, 80, 100, 110 })
					Assert::IsFalse(constant(x, y), (L"Carry stopped outside the masked block at x " + std::to_wstring(x)).c_str());
			}
			pl_tex_destroy(gpu, &source);
			pl_tex_destroy(gpu, &target);
			pl_tex_destroy(gpu, &maskTex);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Stage 6c-2: with NLS active the mask follows the NLS shader's own map.
		// A white square at source block (13, 1), off centre where the map is
		// non-linear, is rendered through the real NLS shader; the mask marks
		// that block new, so the sigma-delta's carry must stop exactly where the
		// square appears, in both axes. The owner's four NLS profiles
		// (standard, protected, NLS+, NLS-V) with a centred and an off-centre source
		// crop: the NLS coordinates span the crop (the off-centre case tells the
		// crop from the whole texture by 5 px).
		TEST_METHOD(OcclusionMaskFollowsTheNlsMap)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			pl_gpu gpu = fixture.Gpu();
			const int srcW = 256, srcH = 128, outW = 340, outH = 128;
			std::vector<const pl_hook*> sigmaHooks;
			CustomHookChain::Chain chain(SigmaLoader(fixture));
			ConfigureSigma(chain, outW, outH, sigmaHooks);
			Assert::AreEqual(size_t{ 1 }, sigmaHooks.size());
			// A square at source block (13, 1): x 208..223, y 16..31.
			std::vector<uint16_t> maskValues(16 * 8, 65535);
			maskValues[1 * 16 + 13] = 0;
			pl_tex maskTex = MakeMaskTexture(gpu, 16, 8, maskValues);
			pl_fmt sourceFormat = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt targetFormat = nullptr;
			const auto caps = PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE;
			for (int i = 0; i < gpu->num_formats; ++i)
			{
				pl_fmt f = gpu->formats[i];
				if (f->type == PL_FMT_UNORM && f->num_components == 4 && f->component_depth[0] == 10 &&
					f->texel_size == 4 && (f->caps & caps) == caps && f->host_bits[0] == 10)
				{ targetFormat = f; break; }
			}
			Assert::IsNotNull(targetFormat);
			auto makeSource = [&](bool square)
			{
				std::vector<float> texels(static_cast<size_t>(srcW) * srcH * 4);
				for (int y = 0; y < srcH; ++y)
					for (int x = 0; x < srcW; ++x)
					{
						float* t = &texels[(static_cast<size_t>(y) * srcW + x) * 4];
						t[0] = t[1] = t[2] = square && x >= 208 && x < 224 && y >= 16 && y < 32 ? 1.0f : 0.5f;
						t[3] = 1.0f;
					}
				pl_tex_params sp{};
				sp.w = srcW; sp.h = srcH; sp.format = sourceFormat; sp.sampleable = true; sp.initial_data = texels.data();
				return pl_tex_create(gpu, &sp);
			};
			pl_tex squareSource = makeSource(true);
			pl_tex flatSource = makeSource(false);
			pl_tex_params tp{};
			tp.w = outW; tp.h = outH; tp.format = targetFormat; tp.renderable = true; tp.host_readable = true;
			tp.blit_dst = true;
			pl_tex target = pl_tex_create(gpu, &tp);
			const pl_rect2df targetCrop{ 0.0f, 0.0f, static_cast<float>(outW), static_cast<float>(outH) };
			OcclusionMask::Hook mask;

			// The owner's three NLS profiles (VideoProcessor.cfg), with the
			// shader loader's defaults filled in as the renderer sees them.
			struct Profile { const wchar_t* name; const char* file; bool plus; std::map<std::string, std::string> rule;
				bool vertical = false; };
			const Profile profiles[] = {
				{ L"standard", "NLS.glsl", false, { { "strength", "1" }, { "curve", "2" }, { "geometry", "0" },
					{ "center_protection", "0.35" } } },
				{ L"protected", "NLS.glsl", false, { { "strength", "1" }, { "curve", "2" }, { "geometry", "1" },
					{ "center_protection", "0.35" } } },
				{ L"plus", "NLSPlus.glsl", true, { { "strength", "1" }, { "curve", "2" }, { "geometry", "1" },
					{ "axis_balance", "0.25" }, { "max_center_zoom", "1.08" },
					{ "horizontal_center_protection", "0.35" }, { "vertical_center_protection", "0.25" } } },
				// NLS-V (VideoProcessor.cfg [shader.nls.vertical]): NLS.glsl warping
				// the vertical axis (warp_axis 1).
				{ L"vertical", "NLS.glsl", false, { { "strength", "1" }, { "curve", "2" }, { "geometry", "1" },
					{ "center_protection", "0.35" } }, true } };
			for (const Profile& profile : profiles)
			{
				std::string source = LoadBundledShader(profile.file);
				Assert::IsFalse(source.empty());
				std::map<std::string, std::string> tokens = profile.rule;
				tokens.emplace("center_protection", "0.35");
				tokens.emplace("axis_balance", "0");
				tokens.emplace("max_center_zoom", "1.08");
				tokens.emplace("horizontal_center_protection", "0.35");
				tokens.emplace("vertical_center_protection", "0.25");
				for (const auto& token : tokens)
					ReplaceShaderToken(source, token.first, token.second);
				const pl_hook* nls = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(nls);
				BindNlsShader(nls, 1.33f, profile.vertical ? 1.0f : 0.0f);
				const NlsMaskMapping::Mapping mapping = NlsMaskMapping::FromParameters(profile.rule, 1.33,
					profile.vertical, profile.plus);
				Assert::IsTrue(mapping.valid, profile.name);
				for (const pl_rect2df sourceCrop : { pl_rect2df{ 0.0f, 0.0f, 256.0f, 128.0f },
					pl_rect2df{ 32.0f, 8.0f, 256.0f, 128.0f } })
				{
					auto render = [&](pl_tex sourceTex, const std::vector<const pl_hook*>& hooks)
					{
						pl_frame image = TargetLutGpuFixture::MakeRgbFrame(sourceTex);
						image.crop = sourceCrop;
						pl_frame output = TargetLutGpuFixture::MakeRgbFrame(target);
						output.repr.levels = PL_COLOR_LEVELS_LIMITED;
						output.repr.bits.sample_depth = 10;
						output.repr.bits.color_depth = 8;
						output.repr.bits.bit_shift = 2;
						output.crop = targetCrop;
						pl_render_params params = pl_render_fast_params;
						params.dither_params = nullptr;
						params.error_diffusion = nullptr;
						params.hooks = hooks.data();
						params.num_hooks = static_cast<int>(hooks.size());
						Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &output, &params));
						pl_gpu_finish(gpu);
						std::vector<uint32_t> pixels(static_cast<size_t>(outW) * outH);
						pl_tex_transfer_params download{};
						download.tex = target;
						download.ptr = pixels.data();
						Assert::IsTrue(pl_tex_download(gpu, &download));
						return pixels;
					};
					struct Box { int x0 = outW, y0 = outH, x1 = -1, y1 = -1; void Add(int x, int y)
						{ x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); } };
					// Where NLS puts the square: red above mid grey (502).
					const std::vector<uint32_t> square = render(squareSource, { nls });
					Box seen;
					for (int y = 0; y < outH; ++y)
						for (int x = 0; x < outW; ++x)
							if ((square[static_cast<size_t>(y) * outW + x] & 1023u) > 700)
								seen.Add(x, y);
					// Where the mask stops the carry.
					Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "occlusion_mask", 1.0));
					mask.SetFrame(maskTex, sourceCrop, targetCrop);
					mask.SetNls(mapping, sourceCrop);
					std::vector<std::vector<uint32_t>> frames;
					for (int frame = 0; frame < 8; ++frame)
						frames.push_back(render(flatSource, { nls, mask.Get(), sigmaHooks[0] }));
					Box masked;
					for (int y = 0; y < outH; ++y)
						for (int x = 0; x < outW; ++x)
						{
							const uint32_t first = frames[0][static_cast<size_t>(y) * outW + x] & 1023u;
							bool constant = true;
							for (const auto& f : frames)
								constant = constant && (f[static_cast<size_t>(y) * outW + x] & 1023u) == first;
							if (constant)
								masked.Add(x, y);
						}
					const std::wstring where = std::wstring(profile.name) + L", crop " +
						std::to_wstring(static_cast<int>(sourceCrop.x0)) + L"," + std::to_wstring(static_cast<int>(sourceCrop.y0)) +
						L": square " + std::to_wstring(seen.x0) + L"," + std::to_wstring(seen.y0) + L".." +
						std::to_wstring(seen.x1) + L"," + std::to_wstring(seen.y1) + L", mask " +
						std::to_wstring(masked.x0) + L"," + std::to_wstring(masked.y0) + L".." +
						std::to_wstring(masked.x1) + L"," + std::to_wstring(masked.y1);
					Assert::IsTrue(seen.x1 >= 0 && masked.x1 >= 0 &&
						std::abs(masked.x0 - seen.x0) <= 2 && std::abs(masked.x1 - seen.x1) <= 2 &&
						std::abs(masked.y0 - seen.y0) <= 2 && std::abs(masked.y1 - seen.y1) <= 2, where.c_str());
				}
				pl_mpv_user_shader_destroy(&nls);
			}
			pl_tex_destroy(gpu, &squareSource);
			pl_tex_destroy(gpu, &flatSource);
			pl_tex_destroy(gpu, &target);
			pl_tex_destroy(gpu, &maskTex);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}
	};
}
