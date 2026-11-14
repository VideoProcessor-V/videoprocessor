// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.shared: the faithful-ortho migration identity against the REAL AdaptiveDownscale module (the brief's
// "ScalerPass(ortho faithful) == AdaptiveDownscale faithfulOrtho"). AdaptiveDownscale is only driven here, never
// edited. Kept in its own file because it needs the shared libplacebo render fixture (VpLutGpuTestSupport.h).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>
#include <VP-V/modules/scaling/shared/ScalerPass.h>

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ScalerPassAdaptiveIdentityNs
{
	TEST_CLASS(ScalerPassAdaptiveIdentityTests)
	{
	public:
		// AdaptiveDownscale (faithful_ortho, w_base 0 so the output is the faithful branch, grey picture so the
		// luma-only blend is exact) rendered through libplacebo equals ScalerPass(lanczos3, both, down first,
		// antiring 0.5) on the same picture, within one 8-bit code (the pipeline writes an 8-bit target).
		TEST_METHOD(ScalerGpuMigrationIdentityFaithfulOrtho)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				Assert::IsFalse(source.empty());
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			const int sw = 384, sh = 240, dw = 256, dh = 160;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			std::vector<uint16_t> px(static_cast<size_t>(sw) * sh * 4);
			for (int y = 0; y < sh; ++y)
				for (int x = 0; x < sw; ++x)
				{
					// Hard stripes on the left, fine texture on the right (both exercise the lobes).
					const double c = x < sw / 2 ? (((x / 12) % 2) ? 220.0 : 30.0) : 128.0 + 25.0 * std::sin(x * 1.7) * std::cos(y * 1.3);
					const uint16_t v = static_cast<uint16_t>(std::lround((std::min)(255.0, (std::max)(0.0, c)) * 257.0));
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
			pl_tex scene = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(scene);

			// 1. The real module.
			AdaptiveDownscale::Module module;
			AdaptiveDownscale::Settings st;
			st.faithfulOrtho = true;
			st.faithfulAuto = false;
			st.antiring = 0.5f;
			st.wBase = 0.0f;
			st.grainMap = false;
			st.float32Picture = true;
			module.SetFrame(st, 1, 1.0f / 24.0f);
			pl_tex_params op{};
			op.w = dw;
			op.h = dh;
			op.format = dstFmt;
			op.renderable = true;
			op.host_readable = true;
			pl_tex target = pl_tex_create(gpu, &op);
			Assert::IsNotNull(target);
			pl_frame image = TargetLutGpuFixture::MakeRgbFrame(scene);
			image.color.transfer = PL_COLOR_TRC_GAMMA22;
			pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
			out.color.transfer = PL_COLOR_TRC_GAMMA22;
			pl_render_params params = pl_render_default_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			params.downscaler = &pl_filter_mitchell;
			params.disable_linear_scaling = true;
			const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
			params.hooks = hooks;
			params.num_hooks = 3;
			Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
			pl_gpu_finish(gpu);
			Assert::IsTrue(module.Active(), L"the module should have taken the downscale");
			std::vector<uint8_t> real(static_cast<size_t>(dw) * dh * 4);
			pl_tex_transfer_params d{};
			d.tex = target;
			d.ptr = real.data();
			Assert::IsTrue(pl_tex_download(gpu, &d));

			// 2. ScalerPass on the same picture (the whole texture).
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);
			ScalerPass::Module pass;
			ScalerPass::Settings ss;
			ss.filter.kind = ScalerKernels::Kind::Lanczos;
			ss.filter.radius = 3;
			ss.axis = ScalerPass::Axis::Both;
			ss.order = ScalerPass::Order::DownFirst;
			ss.antiring = 0.5f;
			ss.precision = ScalerPass::Precision::F32;
			ScalerPass::Source s;
			s.tex = scene;
			pl_tex got = nullptr;
			Assert::IsTrue(pass.Apply(gpu, dispatch, s, dw, dh, ss, got),
				std::wstring(pass.LastError().begin(), pass.LastError().end()).c_str());
			pl_tex_params rp{};
			rp.w = dw;
			rp.h = dh;
			rp.format = got->params.format;
			rp.host_readable = true;
			rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			Assert::IsNotNull(readable);
			pl_tex_blit_params bp{};
			bp.src = got;
			bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			std::vector<float> f(static_cast<size_t>(dw) * dh * 4);
			Assert::AreEqual(static_cast<size_t>(rp.format->texel_size), sizeof(float) * 4);
			pl_tex_transfer_params d2{};
			d2.tex = readable;
			d2.ptr = f.data();
			Assert::IsTrue(pl_tex_download(gpu, &d2));

			int worst = 0;
			size_t worstAt = 0;
			for (size_t i = 0; i < static_cast<size_t>(dw) * dh; ++i)
			{
				for (int c = 0; c < 3; ++c)
				{
					const int want = static_cast<int>(std::lround((std::min)(1.0f, (std::max)(0.0f, f[i * 4 + c])) * 255.0f));
					const int diff = std::abs(want - static_cast<int>(real[i * 4 + c]));
					if (diff > worst)
					{
						worst = diff;
						worstAt = i;
					}
				}
			}
			char line[160];
			std::snprintf(line, sizeof line, "faithful ortho: real AdaptiveDownscale vs ScalerPass, worst %d code(s) at pixel %zu\n", worst, worstAt);
			Logger::WriteMessage(line);
			Assert::IsTrue(worst <= 1, L"ScalerPass == AdaptiveDownscale faithful ortho within one 8-bit code");

			pass.Release(gpu);
			pl_tex_destroy(gpu, &readable);
			pl_dispatch_destroy(&dispatch);
			module.Release();
			pl_tex_destroy(gpu, &target);
			pl_tex_destroy(gpu, &scene);
		}
	};
}
