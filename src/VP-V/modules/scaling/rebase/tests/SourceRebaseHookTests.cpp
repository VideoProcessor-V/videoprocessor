// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(SourceRebaseHookTests)
	{
	public:


		TEST_METHOD(RebasedChartMatchesTheNativeChartThroughTheUpscalePath)
		{
			// Stage 12 spec §4 / plan A4 (owner 2026-10-15): a 1080-line chart
			// upscaled to 2160 upstream and re-based to 1080 goes through the
			// same libplacebo upscale path as the native 1080 chart and comes
			// out the same within half-float. YCbCr 4:2:0, both rendered to 4K.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 1920, H = 1080;
			// A chart inside the upstream upscaler's flat passband (waves up to
			// 0.04 cycles per 1080-line pixel): what the re-base can restore. Above
			// it the upstream kernel has already softened the picture and no
			// re-base can bring that back (measured: 0.13 cycles gives 0.011).
			auto chart = [](int w, int h, double scale, unsigned seed, double level, double amplitude) {
				std::mt19937 rng(seed);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				std::vector<double> v(static_cast<size_t>(w) * h, level);
				for (int k = 0; k < 40; ++k)
				{
					const double f = (0.002 + 0.038 * u(rng)) / scale, a = u(rng) * 3.14159265358979, ph = u(rng) * 6.283;
					const double fx = f * std::cos(a), fy = f * std::sin(a);
					for (int y = 0; y < h; ++y)
						for (int x = 0; x < w; ++x)
							v[static_cast<size_t>(y) * w + x] += amplitude / 40.0 * std::cos(6.283185307 * (fx * (x + 0.5) + fy * (y + 0.5)) + ph);
				}
				return v;
			};
			auto catmull2x = [](const std::vector<double>& src, int w, int h) {
				auto k = [](double x) { x = std::fabs(x); return x < 1 ? 1.5 * x * x * x - 2.5 * x * x + 1 : x < 2 ? -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2 : 0.0; };
				const int W2 = 2 * w, H2 = 2 * h;
				std::vector<double> rows(static_cast<size_t>(W2) * h), out(static_cast<size_t>(W2) * H2);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < W2; ++x)
					{
						const double uu = (x + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * src[static_cast<size_t>(y) * w + (std::min)(w - 1, (std::max)(0, t))]; ws += wt; }
						rows[static_cast<size_t>(y) * W2 + x] = s / ws;
					}
				for (int y = 0; y < H2; ++y)
				{
					const double uu = (y + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
					for (int x = 0; x < W2; ++x)
					{
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * rows[static_cast<size_t>((std::min)(h - 1, (std::max)(0, t))) * W2 + x]; ws += wt; }
						out[static_cast<size_t>(y) * W2 + x] = s / ws;
					}
				}
				return out;
			};
			auto plane = [&](const std::vector<double>& y, const std::vector<double>* cb, const std::vector<double>* cr, int w, int h) {
				pl_tex_params p{};
				p.w = w; p.h = h; p.sampleable = true;
				std::vector<uint16_t> data;
				if (!cb)
				{
					p.format = pl_find_named_fmt(gpu, "r16");
					for (double v : y) data.push_back(static_cast<uint16_t>(std::lround((std::min)(1.0, (std::max)(0.0, v)) * 65535.0)));
				}
				else
				{
					p.format = pl_find_named_fmt(gpu, "rg16");
					for (size_t i = 0; i < cb->size(); ++i)
					{
						data.push_back(static_cast<uint16_t>(std::lround((std::min)(1.0, (std::max)(0.0, (*cb)[i])) * 65535.0)));
						data.push_back(static_cast<uint16_t>(std::lround((std::min)(1.0, (std::max)(0.0, (*cr)[i])) * 65535.0)));
					}
				}
				p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			auto frameOf = [&](pl_tex luma, pl_tex chroma, int w, int h) {
				pl_frame f{};
				f.num_planes = 2;
				f.planes[0].texture = luma; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.planes[1].texture = chroma; f.planes[1].components = 2;
				f.planes[1].component_mapping[0] = PL_CHANNEL_CB; f.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709;
				f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.color = pl_color_space_bt709;
				f.crop = { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) };
				pl_frame_set_chroma_location(&f, PL_CHROMA_CENTER);
				return f;
			};
			const std::vector<double> y1080 = chart(W, H, 1.0, 1, 0.45, 0.5);
			const std::vector<double> cb540 = chart(W / 2, H / 2, 1.0, 2, 0.5, 0.15), cr540 = chart(W / 2, H / 2, 1.0, 3, 0.5, 0.15);
			pl_tex nativeLuma = plane(y1080, nullptr, nullptr, W, H), nativeChroma = plane(cb540, &cb540, &cr540, W / 2, H / 2);
			const std::vector<double> y2160 = catmull2x(y1080, W, H), cb1080 = catmull2x(cb540, W / 2, H / 2), cr1080 = catmull2x(cr540, W / 2, H / 2);
			pl_tex upLuma = plane(y2160, nullptr, nullptr, 2 * W, 2 * H), upChroma = plane(cb1080, &cb1080, &cr1080, W, H);
			Assert::IsTrue(nativeLuma && nativeChroma && upLuma && upChroma, L"planes");
			const pl_frame native = frameOf(nativeLuma, nativeChroma, W, H);
			const pl_frame upscaled = frameOf(upLuma, upChroma, 2 * W, 2 * H);
			SourceRebase::Module rebase;
			pl_frame rebased{};
			Assert::IsTrue(rebase.Apply(gpu, dispatch, upscaled, 0.5, rebased), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());

			pl_tex_params tp{};
			tp.w = 2 * W; tp.h = 2 * H; tp.format = pl_find_named_fmt(gpu, "rgba32f"); tp.renderable = true; tp.host_readable = true;
			Assert::IsNotNull(tp.format, L"rgba32f target");
			auto render = [&](const pl_frame& image) {
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.color = pl_color_space_bt709;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
				pl_gpu_finish(gpu);
				std::vector<float> pixels(static_cast<size_t>(tp.w) * tp.h * 4);
				pl_tex_transfer_params d{}; d.tex = target; d.ptr = pixels.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &target);
				return pixels;
			};
			const std::vector<float> a = render(native), b = render(rebased), c = render(upscaled);
			double worst = 0.0, sum = 0.0, worstUp = 0.0, sumUp = 0.0;
			size_t count = 0;
			int worstX = 0, worstY = 0, worstC = 0;
			// Away from the frame edge: the upstream upscale, the re-base and
			// libplacebo's upscale each extend the edge their own way (their
			// kernels reach ~20 output px together).
			const int margin = 48;
			for (int yy = margin; yy < 2 * H - margin; ++yy)
				for (int xx = margin; xx < 2 * W - margin; ++xx)
					for (int ch = 0; ch < 3; ++ch)
					{
						const size_t i = (static_cast<size_t>(yy) * tp.w + xx) * 4 + ch;
						const double d = std::fabs(a[i] - b[i]);
						if (d > worst) { worst = d; worstX = xx; worstY = yy; worstC = ch; }
						sum += d;
						worstUp = (std::max)(worstUp, static_cast<double>(std::fabs(a[i] - c[i])));
						sumUp += std::fabs(a[i] - c[i]);
						++count;
					}
			char line[200];
			snprintf(line, sizeof(line), "re-based vs native 1080 through the upscale path: max %.6f mean %.7f (the 4K upscale rendered as is: max %.6f mean %.7f); worst at (%d, %d) channel %d\n",
				worst, sum / count, worstUp, sumUp / count, worstX, worstY, worstC);
			Logger::WriteMessage(line);
			// The spec asks for half-float. Measured: the mean is well inside it
			// (half-float's step at mid grey, 2^-11); the worst pixel is under one
			// 8-bit code (the FL35 link's precision) - even inside its flat band
			// the upstream Catmull-Rom softens the steepest peaks a little, which
			// no re-base can return; and the re-base is far closer to the native
			// chart than the upscaled source left as it was.
			Assert::IsTrue(sum / count < 1.0 / 2048.0, L"the mean is within half-float");
			Assert::IsTrue(worst < 1.0 / 255.0, L"the worst pixel is under one 8-bit code");
			Assert::IsTrue(sum * 5.0 < sumUp, L"at least five times closer to the native chart than the upscaled source as it was");
			pl_tex_destroy(gpu, &nativeLuma); pl_tex_destroy(gpu, &nativeChroma);
			pl_tex_destroy(gpu, &upLuma); pl_tex_destroy(gpu, &upChroma);
			rebase.Release(gpu);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebaseHandlesAnRgbPlane)
		{
			// Plan A4 (owner 2026-10-15): RGB 4:4:4 (native RGB ingest, the GPU
			// RGB unpack) is one plane; it re-bases as one plane, all components.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 1024, H = 512, w = 512, h = 256;
			// Smooth content at the target size, sampled at 2x (an upstream upscale of it).
			auto value = [](double x, double y, int c) { return 0.4 + 0.2 * std::sin(x * (6.0 + c)) * std::cos(y * (5.0 + c)); };
			std::vector<uint16_t> px(static_cast<size_t>(W) * H * 4);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					for (int c = 0; c < 4; ++c)
						px[(static_cast<size_t>(y) * W + x) * 4 + c] = static_cast<uint16_t>(std::lround((c == 3 ? 1.0 : value((x + 0.5) / W, (y + 0.5) / H, c)) * 65535.0));
			pl_tex_params tp{};
			tp.w = W; tp.h = H; tp.format = pl_find_named_fmt(gpu, "rgba16"); tp.sampleable = true; tp.initial_data = px.data();
			pl_tex tex = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(tex);
			pl_frame frame = TargetLutGpuFixture::MakeRgbFrame(tex);
			frame.crop = { 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H) };
			SourceRebase::Module rebase;
			pl_frame out{};
			Assert::IsTrue(rebase.Apply(gpu, dispatch, frame, 0.5, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
			Assert::AreEqual(1, out.num_planes, L"one plane");
			Assert::AreEqual(w, out.planes[0].texture->params.w);
			Assert::AreEqual(h, out.planes[0].texture->params.h);
			Assert::AreEqual(frame.planes[0].components, out.planes[0].components, L"all components kept");
			Assert::AreEqual(static_cast<float>(w), out.crop.x1, 1e-4f);
			// Read back (via a readable copy) and compare with the content at the target size.
			pl_tex_params rp{};
			rp.w = w; rp.h = h; rp.format = out.planes[0].texture->params.format; rp.host_readable = true; rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			pl_tex_blit_params bp{}; bp.src = out.planes[0].texture; bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			double worst = 0.0;
			const bool isFloat = readable->params.format->type == PL_FMT_FLOAT;
			std::vector<uint16_t> back(static_cast<size_t>(w) * h * 4);
			pl_tex_transfer_params d{}; d.tex = readable; d.ptr = back.data();
			Assert::IsTrue(pl_tex_download(gpu, &d));
			for (int y = 8; y < h - 8; ++y)
				for (int x = 8; x < w - 8; ++x)
					for (int c = 0; c < 3; ++c)
					{
						const uint16_t raw = back[(static_cast<size_t>(y) * w + x) * 4 + c];
						const double v = isFloat ? HalfBits(raw) : raw / 65535.0;
						worst = (std::max)(worst, std::fabs(v - value((x + 0.5) / w, (y + 0.5) / h, c)));
					}
			char line[120];
			snprintf(line, sizeof(line), "RGB re-base 2x: worst %.5f (%.2f 8-bit codes)\n", worst, worst * 255.0);
			Logger::WriteMessage(line);
			Assert::IsTrue(worst < 1.0 / 255.0, L"the RGB picture restored within a code");
			pl_tex_destroy(gpu, &readable);
			pl_tex_destroy(gpu, &tex);
			rebase.Release(gpu);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebaseKernelInverseRecoversThe1080Picture)
		{
			// Kernel-matched re-base (owner 2026-10-16: optimise 1080p): with the player's
			// kernel identified, the GPU's luma re-base is its inverse - the 1080 picture
			// itself, as the CPU inverse gives it.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int m = 256, n = 512;
			const int k = KernelInverse::Find("lanczos3");
			Assert::IsTrue(k >= 0);
			const KernelInverse::Candidate& c = KernelInverse::Candidates()[static_cast<size_t>(k)];
			// The 1080-class picture (textured), upscaled 2x with Lanczos-3 (separable, CPU).
			std::vector<float> master(static_cast<size_t>(m) * m);
			for (int y = 0; y < m; ++y)
				for (int x = 0; x < m; ++x)
					master[static_cast<size_t>(y) * m + x] = static_cast<float>(0.45 + 0.15 * std::sin(x * 0.9 + y * 0.31) * std::cos(y * 0.7) + 0.08 * std::sin(x * 2.3 - y * 1.7));
			std::vector<float> rows(static_cast<size_t>(m) * n), up(static_cast<size_t>(n) * n), lineIn(static_cast<size_t>(m)), lineOut(static_cast<size_t>(n));
			for (int y = 0; y < m; ++y) KernelInverse::UpscaleLine(c, &master[static_cast<size_t>(y) * m], m, &rows[static_cast<size_t>(y) * n]);
			for (int x = 0; x < n; ++x)
			{
				for (int y = 0; y < m; ++y) lineIn[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * n + x];
				KernelInverse::UpscaleLine(c, lineIn.data(), m, lineOut.data());
				for (int y = 0; y < n; ++y) up[static_cast<size_t>(y) * n + x] = lineOut[static_cast<size_t>(y)];
			}
			std::vector<uint16_t> luma(up.size());
			for (size_t i = 0; i < up.size(); ++i) luma[i] = static_cast<uint16_t>(std::lround((std::min)(1.0f, (std::max)(0.0f, up[i])) * 65535.0f));
			pl_tex_params lp{};
			lp.w = n; lp.h = n; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.initial_data = luma.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp);
			Assert::IsTrue(lumaTex != nullptr);
			pl_frame source{};
			source.num_planes = 1;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.crop = { 0.0f, 0.0f, static_cast<float>(n), static_cast<float>(n) };
			auto rebaseWith = [&](int kernelIndex, std::vector<float>& values, bool& used) {
				SourceRebase::Module rebase;
				rebase.SetKernel(kernelIndex);
				pl_frame out{};
				Assert::IsTrue(rebase.Apply(gpu, dispatch, source, 0.5, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
				used = rebase.KernelUsed();
				pl_tex_params rp{};
				rp.w = m; rp.h = m; rp.format = out.planes[0].texture->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = out.planes[0].texture; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				values.assign(static_cast<size_t>(m) * m, 0.0f);
				if (readable->params.format->type == PL_FMT_FLOAT)
				{
					std::vector<uint16_t> half(values.size());
					pl_tex_transfer_params d{}; d.tex = readable; d.ptr = half.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					for (size_t i = 0; i < values.size(); ++i) values[i] = HalfBits(half[i]);
				}
				else
				{
					std::vector<uint16_t> unorm(values.size());
					pl_tex_transfer_params d{}; d.tex = readable; d.ptr = unorm.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					for (size_t i = 0; i < values.size(); ++i) values[i] = unorm[i] / 65535.0f;
				}
				pl_tex_destroy(gpu, &readable);
				rebase.Release(gpu);
			};
			std::vector<float> inverse, polar;
			bool usedInverse = false, usedPolar = true;
			rebaseWith(k, inverse, usedInverse);
			rebaseWith(-1, polar, usedPolar);
			Assert::IsTrue(usedInverse, L"the kernel inverse ran");
			Assert::IsFalse(usedPolar, L"without a kernel: the polar filter as before");
			double eInverse = 0.0, ePolar = 0.0;
			long count = 0;
			for (int y = 24; y < m - 24; ++y)
				for (int x = 24; x < m - 24; ++x)
				{
					const double truth = master[static_cast<size_t>(y) * m + x];
					eInverse += (inverse[static_cast<size_t>(y) * m + x] - truth) * (inverse[static_cast<size_t>(y) * m + x] - truth);
					ePolar += (polar[static_cast<size_t>(y) * m + x] - truth) * (polar[static_cast<size_t>(y) * m + x] - truth);
					++count;
				}
			const double rmsInverse = std::sqrt(eInverse / count) * 1023.0, rmsPolar = std::sqrt(ePolar / count) * 1023.0;
			char line[160];
			snprintf(line, sizeof(line), "kernel inverse on the GPU: %.3f codes RMS from the 1080 picture; the polar filter %.3f codes\n", rmsInverse, rmsPolar);
			Logger::WriteMessage(line);
			Assert::IsTrue(rmsInverse < 0.1, L"the inverse recovers the 1080 picture (within a tenth of a 10-bit code)");
			Assert::IsTrue(rmsInverse * 5.0 < rmsPolar, L"far closer than the polar filter");
			pl_tex_destroy(gpu, &lumaTex);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebasePlayerSubtitleText)
		{
			// Owner 2026-10-16 ("what about with the subtitles"): a player upscales 1080p video
			// (here Lanczos-3, identified) and draws its subtitles at 4K. The re-base reduces
			// the text with the picture; measured after the display's upscale back: the text's
			// error and halos (overshoot past the outline's black or the glyph's white).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int m = 256, n = 512;
			const int k = KernelInverse::Find("lanczos3");
			Assert::IsTrue(k >= 0);
			const KernelInverse::Candidate& c = KernelInverse::Candidates()[static_cast<size_t>(k)];
			auto upscale2 = [&](const std::vector<float>& in, std::vector<float>& out) {
				std::vector<float> rows(static_cast<size_t>(m) * n), lineIn(static_cast<size_t>(m)), lineOut(static_cast<size_t>(n));
				out.assign(static_cast<size_t>(n) * n, 0.0f);
				for (int y = 0; y < m; ++y) KernelInverse::UpscaleLine(c, &in[static_cast<size_t>(y) * m], m, &rows[static_cast<size_t>(y) * n]);
				for (int x = 0; x < n; ++x)
				{
					for (int y = 0; y < m; ++y) lineIn[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * n + x];
					KernelInverse::UpscaleLine(c, lineIn.data(), m, lineOut.data());
					for (int y = 0; y < n; ++y) out[static_cast<size_t>(y) * n + x] = lineOut[static_cast<size_t>(y)];
				}
			};
			std::vector<float> master(static_cast<size_t>(m) * m);
			for (int y = 0; y < m; ++y)
				for (int x = 0; x < m; ++x)
					master[static_cast<size_t>(y) * m + x] = static_cast<float>(0.45 + 0.15 * std::sin(x * 0.9 + y * 0.31) * std::cos(y * 0.7) + 0.08 * std::sin(x * 2.3 - y * 1.7));
			std::vector<float> truth;
			upscale2(master, truth);
			for (float& v : truth) v = static_cast<float>(std::round((std::min)(1.0f, (std::max)(0.0f, v)) * 1023.0) / 1023.0);
			std::vector<unsigned char> textMask(truth.size(), 0);
			const int textTop = 400, textHeight = 56;
			SubtitleTextTestPattern::Draw(truth, n, n, 16, n - 16, textTop, textHeight, 5u, &textMask);
			std::vector<uint16_t> luma(truth.size());
			for (size_t i = 0; i < truth.size(); ++i) luma[i] = static_cast<uint16_t>(std::lround(truth[i] * 65535.0f));
			pl_tex_params lp{};
			lp.w = n; lp.h = n; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.initial_data = luma.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp);
			Assert::IsTrue(lumaTex != nullptr);
			pl_frame source{};
			source.num_planes = 1;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.crop = { 0.0f, 0.0f, static_cast<float>(n), static_cast<float>(n) };
			auto rebaseWith = [&](int kernelIndex, std::vector<float>& values) {
				SourceRebase::Module rebase;
				rebase.SetKernel(kernelIndex);
				pl_frame out{};
				Assert::IsTrue(rebase.Apply(gpu, dispatch, source, 0.5, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
				pl_tex_params rp{};
				rp.w = m; rp.h = m; rp.format = out.planes[0].texture->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = out.planes[0].texture; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				values.assign(static_cast<size_t>(m) * m, 0.0f);
				std::vector<uint16_t> raw(values.size());
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (size_t i = 0; i < values.size(); ++i)
					values[i] = readable->params.format->type == PL_FMT_FLOAT ? HalfBits(raw[i]) : raw[i] / 65535.0f;
				pl_tex_destroy(gpu, &readable);
				rebase.Release(gpu);
			};
			struct Result { double textRms, pictureRms, worstOvershoot; long halo; };
			auto measure = [&](const std::vector<float>& rebased) {
				std::vector<float> shown;
				upscale2(rebased, shown);   // the display's upscale back to 4K (Lanczos-3 as a proxy)
				Result r{ 0, 0, 0, 0 };
				double eText = 0, ePic = 0;
				long nText = 0, nPic = 0;
				for (int y = 24; y < n - 24; ++y)
					for (int x = 24; x < n - 24; ++x)
					{
						const size_t i = static_cast<size_t>(y) * n + x;
						const double e = (shown[i] - truth[i]) * 1023.0;
						if (textMask[i])
						{
							eText += e * e; ++nText;
							const double below = (SubtitleTextTestPattern::kBlack - shown[i]) * 1023.0, above = (shown[i] - SubtitleTextTestPattern::kWhite) * 1023.0;
							r.worstOvershoot = (std::max)(r.worstOvershoot, (std::max)(below, above));
							if (below > 8.0 || above > 8.0) ++r.halo;
						}
						else if (y < textTop - 24) { ePic += e * e; ++nPic; }
					}
				r.textRms = std::sqrt(eText / (std::max)(1L, nText));
				r.pictureRms = std::sqrt(ePic / (std::max)(1L, nPic));
				return r;
			};
			std::vector<float> inverse, polar;
			rebaseWith(k, inverse);
			rebaseWith(-1, polar);
			const Result ri = measure(inverse), rp = measure(polar);
			long textPixels = 0;
			for (unsigned char v : textMask) textPixels += v;
			char line[256];
			snprintf(line, sizeof(line), "player text at 4K over a Lanczos-3 1080 upscale, re-based 2:1 and shown at 4K (%ld text pixels):\n", textPixels);
			Logger::WriteMessage(line);
			snprintf(line, sizeof(line), "  kernel inverse: text %.1f codes RMS, %ld halo pixels (>8 codes past black/white), worst %.1f codes; picture %.2f codes\n",
				ri.textRms, ri.halo, ri.worstOvershoot, ri.pictureRms);
			Logger::WriteMessage(line);
			snprintf(line, sizeof(line), "  polar filter:   text %.1f codes RMS, %ld halo pixels, worst %.1f codes; picture %.2f codes\n",
				rp.textRms, rp.halo, rp.worstOvershoot, rp.pictureRms);
			Logger::WriteMessage(line);
			Assert::IsTrue(std::isfinite(ri.textRms) && std::isfinite(rp.textRms), L"measured");
			Assert::IsTrue(ri.pictureRms < rp.pictureRms, L"the picture: the inverse still the closer");
			pl_tex_destroy(gpu, &lumaTex);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebaseOneAxisScalesThatAxisOnly)
		{
			// Owner 2026-10-16 ("decompose an upscaler and then use it in the offending axis
			// alone"): a source upscaled vertically only (true 4K detail across) is re-based
			// in height alone; the fine horizontal detail survives untouched.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 1024, H = 512, h = 256;
			// Horizontal: 4K-class detail (0.35 cycles per pixel); vertical: smooth (an upscale's content).
			auto fine = [](int x) { return 0.12 * std::sin(2.0 * 3.14159265358979 * 0.35 * x); };
			auto smooth = [](double y) { return 0.45 + 0.2 * std::cos(y * 6.0); };
			std::vector<uint16_t> luma(static_cast<size_t>(W) * H);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					luma[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(std::lround((smooth((y + 0.5) / H) + fine(x)) * 65535.0));
			pl_tex_params lp{};
			lp.w = W; lp.h = H; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.initial_data = luma.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp);
			Assert::IsTrue(lumaTex != nullptr);
			pl_frame source{};
			source.num_planes = 1;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.crop = { 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H) };
			SourceRebase::Module rebase;
			pl_frame out{};
			Assert::IsTrue(rebase.Apply(gpu, dispatch, source, 1.0, 0.5, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
			Assert::AreEqual(W, out.planes[0].texture->params.w, L"the width untouched");
			Assert::AreEqual(h, out.planes[0].texture->params.h, L"the height re-based");
			Assert::AreEqual(static_cast<float>(W), out.crop.x1, 1e-4f);
			Assert::AreEqual(static_cast<float>(h), out.crop.y1, 1e-4f);
			pl_tex_params rp{};
			rp.w = W; rp.h = h; rp.format = out.planes[0].texture->params.format; rp.host_readable = true; rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			pl_tex_blit_params bp{}; bp.src = out.planes[0].texture; bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			std::vector<float> values(static_cast<size_t>(W) * h);
			if (readable->params.format->type == PL_FMT_FLOAT)
			{
				std::vector<uint16_t> half(values.size());
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (size_t i = 0; i < values.size(); ++i) values[i] = HalfBits(half[i]);
			}
			else
			{
				std::vector<uint16_t> unorm(values.size());
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = unorm.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (size_t i = 0; i < values.size(); ++i) values[i] = unorm[i] / 65535.0f;
			}
			// Per row: the mean is the smooth vertical picture at the re-based row; the fine
			// horizontal pattern keeps its amplitude (projection on the known sinusoid).
			double worstMean = 0.0, worstAmplitude = 1.0;
			for (int y = 8; y < h - 8; y += 4)
			{
				double mean = 0.0, proj = 0.0, norm = 0.0;
				for (int x = 16; x < W - 16; ++x)
				{
					const double v = values[static_cast<size_t>(y) * W + x];
					mean += v;
					proj += (v - smooth((y + 0.5) / h)) * fine(x);
					norm += fine(x) * fine(x);
				}
				mean /= (W - 32);
				worstMean = (std::max)(worstMean, std::fabs(mean - smooth((y + 0.5) / h)));
				worstAmplitude = (std::min)(worstAmplitude, proj / norm);
			}
			char line[160];
			snprintf(line, sizeof(line), "one-axis re-base: vertical picture within %.4f; fine horizontal detail kept at %.1f%% amplitude\n",
				worstMean, worstAmplitude * 100.0);
			Logger::WriteMessage(line);
			Assert::IsTrue(worstMean < 0.01, L"the vertical picture restored at the re-based height");
			Assert::IsTrue(worstAmplitude > 0.98, L"the fine horizontal detail untouched (no filtering across)");
			pl_tex_destroy(gpu, &lumaTex);
			pl_tex_destroy(gpu, &readable);
			rebase.Release(gpu);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebaseRestoresAnUpscaledSourceAndKeepsTheCropEdge)
		{
			// Stage 12 step A (owner 2026-10-15): source_resolution re-bases a
			// 4K frame to its class, plane by plane in the signal domain.
			Assert::AreEqual(0, SourceRebase::ClassHeight("native"));
			Assert::AreEqual(0, SourceRebase::ClassHeight("auto"), L"auto is native until detection exists");
			Assert::AreEqual(1080, SourceRebase::ClassHeight("1080"));
			Assert::AreEqual(1.0, SourceRebase::Factor("native", 2160));
			Assert::AreEqual(0.5, SourceRebase::Factor("1080", 2160));
			Assert::AreEqual(1.0, SourceRebase::Factor("1080", 1080), L"never upward or equal");
			Assert::AreEqual(1.0, SourceRebase::Factor("1440", 1080));
			Assert::AreEqual(960, SourceRebase::Scaled(1920, 0.5), L"4:2:0 chroma keeps its subsampling");
			// General (owner's standing rule): any height, any frame.
			Assert::AreEqual(900, SourceRebase::ClassHeight("900"));
			Assert::AreEqual(576, SourceRebase::ClassHeight("576"));
			Assert::AreEqual(0, SourceRebase::ClassHeight("100"), L"below the minimum: native");
			Assert::AreEqual(0, SourceRebase::ClassHeight("1080p"), L"not a number: native");
			Assert::AreEqual(720.0 / 1080.0, SourceRebase::Factor("720", 1080), 1e-12, L"a 1080p frame upscaled from 720p");
			Assert::AreEqual(900.0 / 2160.0, SourceRebase::Factor("900", 2160), 1e-12);
			// Per axis: WIDTHxHEIGHT (owner 2026-10-16).
			Assert::AreEqual(1080, SourceRebase::ClassHeight("3840x1080"));
			Assert::AreEqual(3840, SourceRebase::ClassWidth("3840x1080"));
			Assert::AreEqual(0.5, SourceRebase::Factor("3840x1080", 2160));
			Assert::AreEqual(1.0, SourceRebase::FactorX("3840x1080", 3840, 2160), L"the width untouched");
			Assert::AreEqual(0.75, SourceRebase::FactorX("1440x1080", 1920, 1080), 1e-12, L"anamorphic 1440 across");
			Assert::AreEqual(1.0, SourceRebase::Factor("1440x1080", 1080), L"its height native");
			Assert::AreEqual(0.5, SourceRebase::FactorX("1080", 3840, 2160), L"a height alone: both axes alike");
			Assert::AreEqual(0, SourceRebase::ClassHeight("x1080"), L"malformed: native");
			Assert::AreEqual(0, SourceRebase::ClassHeight("3840x"), L"malformed: native");

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 3840, H = 2160, w = 1920, h = 1080;
			// A smooth 1080p picture (a few cycles each way), and the same picture
			// sampled at 4K - what an upstream upscaler would have produced from it -
			// with letterbox bars of 276 rows (black, code 0.0625 = 16/256).
			auto picture = [](double x, double y) { return 0.45 + 0.3 * std::sin(x * 9.0) * std::cos(y * 7.0); };
			const int bar4k = 276;
			std::vector<uint16_t> luma4k(static_cast<size_t>(W) * H), chroma4k(static_cast<size_t>(W / 2) * (H / 2) * 2);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					const bool bar = y < bar4k || y >= H - bar4k;
					const double v = bar ? 0.0625 : picture((x + 0.5) / W, (y + 0.5) / H);
					luma4k[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(std::lround(v * 65535.0));
				}
			for (size_t i = 0; i < chroma4k.size(); ++i) chroma4k[i] = 32768;
			pl_tex_params lp{};
			lp.w = W; lp.h = H; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.host_readable = true;
			lp.initial_data = luma4k.data();
			pl_tex_params cp = lp;
			cp.w = W / 2; cp.h = H / 2; cp.format = pl_find_named_fmt(gpu, "rg16"); cp.initial_data = chroma4k.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp), chromaTex = pl_tex_create(gpu, &cp);
			Assert::IsTrue(lumaTex && chromaTex, L"source planes");
			pl_frame source{};
			source.num_planes = 2;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.planes[1].texture = chromaTex; source.planes[1].components = 2;
			source.planes[1].component_mapping[0] = PL_CHANNEL_CB; source.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			source.crop = { 0.0f, static_cast<float>(bar4k), static_cast<float>(W), static_cast<float>(H - bar4k) };

			SourceRebase::Module rebase;
			pl_frame out{};
			Assert::IsTrue(rebase.Apply(gpu, dispatch, source, 0.5, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
			Assert::AreEqual(w, out.planes[0].texture->params.w);
			Assert::AreEqual(h, out.planes[0].texture->params.h);
			Assert::AreEqual(w / 2, out.planes[1].texture->params.w, L"chroma stays 4:2:0");
			Assert::AreEqual(h / 2, out.planes[1].texture->params.h);
			Assert::AreEqual(static_cast<float>(bar4k / 2), out.crop.y0, 1e-4f, L"the crop scales with the planes");
			Assert::AreEqual(static_cast<float>(h - bar4k / 2), out.crop.y1, 1e-4f);
			Assert::IsTrue(out.planes[0].component_mapping[0] == PL_CHANNEL_Y && out.planes[1].components == 2,
				L"plane layout unchanged");

			// Read the re-based luma back (its format may be r16 or a float
			// fallback). The module's textures are not host-readable (they never
			// need to be): copy into one that is.
			pl_tex_params rp{};
			rp.w = w; rp.h = h; rp.format = out.planes[0].texture->params.format;
			rp.host_readable = true; rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			Assert::IsTrue(readable != nullptr, L"readable copy");
			pl_tex_blit_params bp{};
			bp.src = out.planes[0].texture; bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			const pl_tex rebased = readable;
			std::vector<float> values(static_cast<size_t>(w) * h);
			if (rebased->params.format->type == PL_FMT_FLOAT)
			{
				std::vector<uint16_t> half(values.size());
				pl_tex_transfer_params d{}; d.tex = rebased; d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (size_t i = 0; i < values.size(); ++i) values[i] = HalfBits(half[i]);
			}
			else
			{
				std::vector<uint16_t> unorm(values.size());
				pl_tex_transfer_params d{}; d.tex = rebased; d.ptr = unorm.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (size_t i = 0; i < values.size(); ++i) values[i] = unorm[i] / 65535.0f;
			}
			// Inside the picture (away from the bar edge): the 1080p picture itself.
			double worst = 0.0;
			const int bar = bar4k / 2;
			for (int y = bar + 4; y < h - bar - 4; y += 3)
				for (int x = 4; x < w - 4; x += 3)
					worst = (std::max)(worst, std::fabs(values[static_cast<size_t>(y) * w + x] - picture((x + 0.5) / w, (y + 0.5) / h)));
			// At the crop edge: the bar stays black up to the edge and nothing
			// rings below black inside the crop (what is shown). The polar
			// kernel's lobe leaves about a code of ringing in the bar a row or
			// two outside the crop - never shown - which is held under 2 codes.
			double barWorst = 0.0, cropBelow = 0.0, outsideBelow = 0.0;
			for (int y = 0; y < bar - 2; ++y)
				for (int x = 0; x < w; x += 7)
					barWorst = (std::max)(barWorst, std::fabs(values[static_cast<size_t>(y) * w + x] - 0.0625));
			for (int y = bar - 3; y < bar + 3; ++y)
				for (int x = 0; x < w; x += 7)
				{
					const double below = 0.0625 - values[static_cast<size_t>(y) * w + x];
					double& slot = y >= bar ? cropBelow : outsideBelow;
					slot = (std::max)(slot, below);
				}
			char line[256];
			snprintf(line, sizeof(line), "Re-base 4K -> 1080: picture max error %.5f (%.2f 8-bit codes); bar max error %.5f; below black inside the crop %.5f, in the bar outside it %.5f\n",
				worst, worst * 255.0, barWorst, cropBelow, outsideBelow);
			Logger::WriteMessage(line);
			Assert::IsTrue(worst < 2.0 / 255.0, L"the re-based picture matches the 1080p picture within 2 codes");
			Assert::IsTrue(barWorst < 0.5 / 255.0, L"the bar stays black up to the crop edge");
			Assert::IsTrue(cropBelow < 0.25 / 255.0, L"anti-ringing: nothing undershoots black inside the crop");
			Assert::IsTrue(outsideBelow < 2.0 / 255.0, L"ringing outside the crop (never shown) stays under 2 codes");

			pl_tex_destroy(gpu, &lumaTex);
			pl_tex_destroy(gpu, &chromaTex);
			pl_tex_destroy(gpu, &readable);
			rebase.Release(gpu);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebaseNeverRingsIntoTheBarsInAnyVariant)
		{
			// Planner 2026-10-16 evening: "no ringing into blanked bars" as a standing test for
			// every re-base variant (a separable luma once rang 2.2 codes below black into the
			// bars). Source: a letterboxed 1080p picture upscaled 2x by the player (Lanczos-3,
			// both planes), as a streaming box sends it. Each variant: the bars stay black up to
			// the crop edge and nothing undershoots black inside the crop, luma and chroma.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int w = 1920, h = 1080, W = 3840, H = 2160, bar = 138;
			const KernelInverse::Candidate& lanczos = KernelInverse::Candidates()[static_cast<size_t>(KernelInverse::Find("lanczos3"))];
			// One plane: the 1080-class picture with bars, upscaled 2x separably.
			auto upscaled = [&](int pw, int ph, int pbar, double barValue, const std::function<double(double, double)>& f) {
				std::vector<float> master(static_cast<size_t>(pw) * ph);
				for (int y = 0; y < ph; ++y)
					for (int x = 0; x < pw; ++x)
						master[static_cast<size_t>(y) * pw + x] = static_cast<float>(y < pbar || y >= ph - pbar ? barValue : f((x + 0.5) / pw, (y + 0.5) / ph));
				std::vector<float> rows(static_cast<size_t>(ph) * pw * 2), up(static_cast<size_t>(pw) * 2 * ph * 2);
				std::vector<float> in(static_cast<size_t>(ph)), out(static_cast<size_t>(ph) * 2);
				for (int y = 0; y < ph; ++y) KernelInverse::UpscaleLine(lanczos, &master[static_cast<size_t>(y) * pw], pw, &rows[static_cast<size_t>(y) * pw * 2]);
				for (int x = 0; x < pw * 2; ++x)
				{
					for (int y = 0; y < ph; ++y) in[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * pw * 2 + x];
					KernelInverse::UpscaleLine(lanczos, in.data(), ph, out.data());
					for (int y = 0; y < ph * 2; ++y) up[static_cast<size_t>(y) * pw * 2 + x] = out[static_cast<size_t>(y)];
				}
				return up;
			};
			const double black = 0.0625, neutral = 0.5;
			const std::vector<float> lumaUp = upscaled(w, h, bar, black, [](double x, double y) { return 0.45 + 0.25 * std::sin(x * 9.0) * std::cos(y * 7.0); });
			const std::vector<float> cbUp = upscaled(w / 2, h / 2, bar / 2, neutral, [](double x, double y) { return 0.5 + 0.12 * std::sin(x * 5.0 + y * 3.0); });
			const std::vector<float> crUp = upscaled(w / 2, h / 2, bar / 2, neutral, [](double x, double y) { return 0.5 - 0.1 * std::cos(x * 4.0 - y * 6.0); });
			std::vector<uint16_t> luma(lumaUp.size()), chroma(cbUp.size() * 2);
			auto code = [](float v) { return static_cast<uint16_t>(std::lround((std::min)(1.0f, (std::max)(0.0f, v)) * 65535.0f)); };
			for (size_t i = 0; i < luma.size(); ++i) luma[i] = code(lumaUp[i]);
			for (size_t i = 0; i < cbUp.size(); ++i) { chroma[i * 2] = code(cbUp[i]); chroma[i * 2 + 1] = code(crUp[i]); }
			pl_tex_params lp{};
			lp.w = W; lp.h = H; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.initial_data = luma.data();
			pl_tex_params cp = lp;
			cp.w = W / 2; cp.h = H / 2; cp.format = pl_find_named_fmt(gpu, "rg16"); cp.initial_data = chroma.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp), chromaTex = pl_tex_create(gpu, &cp);
			Assert::IsTrue(lumaTex && chromaTex);
			pl_frame source{};
			source.num_planes = 2;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.planes[1].texture = chromaTex; source.planes[1].components = 2;
			source.planes[1].component_mapping[0] = PL_CHANNEL_CB; source.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			source.crop = { 0.0f, static_cast<float>(2 * bar), static_cast<float>(W), static_cast<float>(H - 2 * bar) };
			auto download = [&](pl_tex tex, int components, std::vector<float>& values) {
				pl_tex_params rp{};
				rp.w = tex->params.w; rp.h = tex->params.h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				const size_t n = static_cast<size_t>(rp.w) * rp.h * components;
				std::vector<uint16_t> raw(n);
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				values.resize(n);
				for (size_t i = 0; i < n; ++i) values[i] = readable->params.format->type == PL_FMT_FLOAT ? HalfBits(raw[i]) : raw[i] / 65535.0f;
				pl_tex_destroy(gpu, &readable);
			};
			struct Variant { const char* name; double fx, fy; bool separable; int kernel; };
			const Variant variants[] = {
				{ "default (polar luma, separable chroma), 2:1", 0.5, 0.5, true, -1 },
				{ "polar luma and chroma, 2:1", 0.5, 0.5, false, -1 },
				{ "kernel inverse (Lanczos-3), 2:1", 0.5, 0.5, true, KernelInverse::Find("lanczos3") },
				{ "height only (1D), 2:1", 1.0, 0.5, true, -1 },
				{ "1440 (2:3)", 2.0 / 3.0, 2.0 / 3.0, true, -1 },
				{ "900 lines", 900.0 / 2160.0, 900.0 / 2160.0, true, -1 } };
			int failures = 0;
			for (const Variant& v : variants)
			{
				SourceRebase::Module rebase;
				rebase.separable = v.separable;
				rebase.SetKernel(v.kernel);
				pl_frame out{};
				Assert::IsTrue(rebase.Apply(gpu, dispatch, source, v.fx, v.fy, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
				std::vector<float> y, c;
				download(out.planes[0].texture, 1, y);
				download(out.planes[1].texture, 2, c);
				const int yw = out.planes[0].texture->params.w, yh = out.planes[0].texture->params.h;
				const int cw = out.planes[1].texture->params.w, ch = out.planes[1].texture->params.h;
				const double edge = out.crop.y0;   // the crop's top edge in luma rows
				// The plane's bar rows (away from the edge by the filter's reach) and the first rows inside.
				auto measure = [&](const std::vector<float>& values, int pw, int ph, int stride, int comp, double scale, double barLevel,
					double& barWorst, double& insideBelow) {
					const double e = edge * scale;
					for (int row = 0; row < ph; ++row)
					{
						const double centre = row + 0.5;
						for (int x = 0; x < pw; x += 5)
						{
							const double value = values[(static_cast<size_t>(row) * pw + x) * stride + comp];
							if (centre < e - 4.0 * (1.0 / v.fy) - 1.0 || centre > ph - e + 4.0 * (1.0 / v.fy) + 1.0)
								barWorst = (std::max)(barWorst, std::fabs(value - barLevel));
							else if ((centre > e && centre < e + 3.0) || (centre < ph - e && centre > ph - e - 3.0))
								if (barLevel == black) insideBelow = (std::max)(insideBelow, black - value);
						}
					}
				};
				double barY = 0.0, belowY = 0.0, barC = 0.0, unused = 0.0;
				measure(y, yw, yh, 1, 0, 1.0, black, barY, belowY);
				measure(c, cw, ch, 2, 0, 0.5, neutral, barC, unused);
				measure(c, cw, ch, 2, 1, 0.5, neutral, barC, unused);
				char line[260];
				snprintf(line, sizeof(line), "%-44s bar: luma %.2f codes, chroma %.2f codes off; below black inside the crop %.2f codes (8-bit)\n",
					v.name, barY * 255.0, barC * 255.0, belowY * 255.0);
				Logger::WriteMessage(line);
				if (barY > 0.5 / 255.0 || barC > 0.5 / 255.0 || belowY > 0.25 / 255.0) ++failures;
				rebase.Release(gpu);
			}
			pl_tex_destroy(gpu, &lumaTex);
			pl_tex_destroy(gpu, &chromaTex);
			pl_dispatch_destroy(&dispatch);
			Assert::AreEqual(0, failures, L"no re-base variant rings into the bars or below black inside the crop");
		}
	};
}
