// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(WholeHookChainHookTests)
	{
	public:


		TEST_METHOD(AllBundledStandardShadersLoadAndUpscalersEngageByHeight)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			const char* files[] = { "CfL_Prediction.glsl", "FSRCNNX_x2_16-0-4-1.glsl",
				"ArtCNN_C4F16.glsl", "SSimDownscaler.glsl", "SSimSuperRes.glsl",
				"Temporal sigma-delta.glsl" };
			std::vector<ConfiguredShaderRule> rules;
			unsigned order = 0;
			for (const char* file : files)
			{
				ConfiguredShaderRule rule;
				rule.name = file;
				rule.filename = file;
				rule.profileSelectionOrder = order++;
				rules.push_back(rule);
			}
			chain.Configure(rules);
			CustomHookChain::TemplateValues values;
			values.outputWidth = 2560;
			values.outputHeight = 1600;
			values.sourceWidth = 1920;
			values.sourceHeight = 1080;
			values.refreshHz = 60.0;
			values.sourceFps = 24.0;
			std::string libplaceboLog;
			fixture.CaptureLog(&libplaceboLog);
			chain.Ensure(values);
			fixture.CaptureLog(nullptr);
			cleanLog += libplaceboLog;
			fixture.CaptureLog(&cleanLog);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			const std::wstring rejected = L"A bundled standard shader did not load: " +
				std::wstring(libplaceboLog.begin(), libplaceboLog.end());
			Assert::AreEqual(size_t{ 6 }, hooks.size(), rejected.c_str());
			Assert::IsTrue((hooks[1]->stages & PL_HOOK_LUMA_INPUT) != 0, L"FSRCNNX doubles luma");
			Assert::IsTrue((hooks[2]->stages & PL_HOOK_LUMA_INPUT) != 0, L"ArtCNN doubles luma");
			Assert::IsTrue((hooks[4]->stages & PL_HOOK_POST_KERNEL) != 0, L"SSimSuperRes refines after scaling");

			// With the 1.33x lens a 1080p 16:9 picture is enlarged ~1.48x
			// vertically but ~1.11x horizontally: the upscalers must engage on
			// height alone, and each keeps the authors' 1.3x minimum.
			for (const char* file : { "FSRCNNX_x2_16-0-4-1.glsl", "ArtCNN_C4F16.glsl" })
			{
				const std::string source = LoadBundledShader(file);
				// Directives start a line; the header comment quotes the old rule.
				Assert::IsTrue(source.find("\n//!WHEN OUTPUT.w LUMA.w") == std::string::npos,
					L"The upstream both-axes rule must be gone");
				Assert::IsTrue(source.find("\n//!WHEN OUTPUT.h LUMA.h / 1.3") != std::string::npos);
			}
			for (const char* file : { "SSimDownscaler.glsl", "SSimSuperRes.glsl" })
				Assert::IsTrue(LoadBundledShader(file).find("NATIVE_CROPPED.h") != std::string::npos,
					L"The SSIM pair engages from the live source and output heights");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog);
		}

		TEST_METHOD(BundledCustomHooksParseAndSigmaDeltaRunsOnTheRealGpuPath)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			auto rule = [](const char* name, const char* file, unsigned order)
			{
				ConfiguredShaderRule configured;
				configured.name = name;
				configured.filename = file;
				configured.stageOrder = order;
				return configured;
			};
			ConfiguredShaderRule sigma = rule("sigma_delta", "Temporal sigma-delta.glsl", 90);
			sigma.parameters = { { "enabled", "1" }, { "levels_full", "1" },
				{ "swapchain_bits", "8" }, { "noise_amplitude", "0" }, { "test_ramp", "1" } };
			chain.Configure({ rule("cfl", "CfL_Prediction.glsl", 10),
				rule("ssim", "SSimDownscaler.glsl", 20), sigma });
			CustomHookChain::TemplateValues values;
			values.outputWidth = 64;
			values.outputHeight = 64;
			values.sourceWidth = 64;
			values.sourceHeight = 64;
			values.refreshHz = 60.0;
			values.sourceFps = 24.0;
			std::string libplaceboLog;
			fixture.CaptureLog(&libplaceboLog);
			chain.Ensure(values);
			fixture.CaptureLog(nullptr);
			cleanLog += libplaceboLog;
			fixture.CaptureLog(&cleanLog);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			const std::wstring rejected = L"libplacebo rejected a bundled custom shader: " +
				std::wstring(libplaceboLog.begin(), libplaceboLog.end());
			Assert::AreEqual(size_t{ 3 }, hooks.size(), rejected.c_str());
			Assert::IsTrue((hooks[0]->stages & PL_HOOK_CHROMA_INPUT) != 0, L"CfL must hook CHROMA");
			Assert::IsTrue((hooks[1]->stages & PL_HOOK_POST_KERNEL) != 0, L"SSimDownscaler must hook POSTKERNEL");
			Assert::IsTrue(CustomHookChain::HooksOutput(hooks[2]), L"Sigma-delta must hook OUTPUT");
			Assert::IsTrue(chain.SuppressesDither(false));

			// The ramp replaces the picture, so the readback proves the compute
			// pass, its storage textures and the parameter values all ran.
			const std::vector<RgbaPixel> pixels = fixture.RenderCoordinateField(hooks[2]);
			const size_t row = 32 * 64;
			Assert::IsTrue(pixels[row].r <= 2, L"Ramp must start at black");
			Assert::IsTrue(pixels[row + 63].r >= 253, L"Ramp must end at white");
			for (int x = 1; x < 64; ++x)
				Assert::IsTrue(pixels[row + x].r >= pixels[row + x - 1].r, L"Ramp must be monotonic");
			Assert::AreEqual(static_cast<int>(pixels[5 * 64 + 20].r),
				static_cast<int>(pixels[50 * 64 + 20].r), L"Ramp must not depend on the picture");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog);
		}

		TEST_METHOD(Stage12PassCostsAt4K)
		{
			// Planner gate (2026-10-16): the GPU cost of the Stage 12 passes at 4K on this GPU
			// (wall clock, GPU drained, mean of 20 after a warm-up). Report only; the 3090
			// numbers come from the cinema PC.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 3840, H = 2160;
			std::vector<uint16_t> luma(static_cast<size_t>(W) * H), chroma(static_cast<size_t>(W / 2) * (H / 2) * 2, 32768);
			std::mt19937 rng(3);
			std::uniform_int_distribution<int> u(8000, 56000);
			for (uint16_t& v : luma) v = static_cast<uint16_t>(u(rng));
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
			source.crop = { 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H) };
			source.repr = pl_color_repr_uhdtv;
			source.color = pl_color_space_bt709;
			auto timed = [&](const char* name, const std::function<bool()>& pass) {
				for (int i = 0; i < 3; ++i) Assert::IsTrue(pass(), ExtendedRangeGpuProbe::ToWide(std::string(name) + ": failed").c_str());
				pl_gpu_finish(gpu);
				const auto start = std::chrono::steady_clock::now();
				for (int i = 0; i < 20; ++i) pass();
				pl_gpu_finish(gpu);
				const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 20.0;
				char line[160];
				snprintf(line, sizeof(line), "  %-58s %.3f ms\n", name, ms);
				Logger::WriteMessage(line);
				return ms;
			};
			Logger::WriteMessage("Stage 12 pass costs at 4K (this GPU):\n");
			SourceRebase::Module rebase;
			pl_frame out{};
			rebase.separable = false;
			timed("re-base 2:1, polar (luma + chroma)", [&] { return rebase.Apply(gpu, dispatch, source, 0.5, out); });
			rebase.separable = true;
			timed("re-base 2:1, polar luma + separable chroma (default)", [&] { return rebase.Apply(gpu, dispatch, source, 0.5, out); });
			rebase.SetKernel(KernelInverse::Find("lanczos3"));
			timed("re-base 2:1, kernel inverse (luma) + separable (chroma)", [&] { return rebase.Apply(gpu, dispatch, source, 0.5, out); });
			rebase.SetKernel(-1);
			timed("re-base height only (1D, luma + chroma)", [&] { return rebase.Apply(gpu, dispatch, source, 1.0, 0.5, out); });
			timed("re-base 1440 (polar luma + separable chroma)", [&] { return rebase.Apply(gpu, dispatch, source, 1440.0 / 2160.0, out); });
			VpDebander::Params params;
			params.thresholdCodes = VpDebander::Strength(8.0, 10, 1.0, true);
			params.colorDepth = 10;
			params.radiusScale = 1.0;
			VpDebander::Module deband;
			pl_frame lumaOnly = source;
			lumaOnly.num_planes = 1;
			timed("VP debander (luma, 4K), grain split (default)", [&] { return deband.Apply(gpu, dispatch, lumaOnly, params, out); });
			{
				VpDebander::Params veto = params;
				veto.grainSplit = false;
				timed("VP debander (luma, 4K), spatial-share veto", [&] { return deband.Apply(gpu, dispatch, lumaOnly, veto, out); });
				VpDebander::Params none = params;
				none.grainGuard = false;
				timed("VP debander (luma, 4K), no guard", [&] { return deband.Apply(gpu, dispatch, lumaOnly, none, out); });
			}
			// The monitor's GPU side: its 8 tile copies into the readback buffer (2 Hz in use).
			SourceResolutionMonitor::Monitor monitor;
			uint64_t sequence = 0;
			double seconds = 0.0;
			timed("source-resolution monitor: tile copy (per analysed frame)", [&] {
				monitor.Frame(gpu, dispatch, source, ++sequence, seconds);
				seconds += 0.6;   // past its 2 Hz spacing: every call copies
				return true;
			});
			monitor.Release(gpu);
			rebase.Release(gpu);
			deband.Release(gpu);
			pl_tex_destroy(gpu, &lumaTex);
			pl_tex_destroy(gpu, &chromaTex);
			pl_dispatch_destroy(&dispatch);
		}

		// Efficiency round 2026-10-18 (efficiency_fixes, VP-V/modules/input/capture/UploadReuse.h): the frame shown
		// takes the planes the de-judder uploaded for it as the next frame. That is exact because
		// an upload is a pure function of the payload and the layout: the same bytes through the
		// same route (the CPU route's pl_upload_plane of P010 / P210, or capture_path gpu's raw
		// upload + GpuCaptureUnpack) give the same planes in whichever texture pair they land,
		// including a pair that held another size or format before. Checked byte for byte here
		// (WARP by default; VP_TEST_GPU=hardware on the GeForce).
		TEST_METHOD(UploadReuseRoutesAreBitIdentical)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 1920, height = 1080;
			uint32_t seed = 0x9e3779b9u;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			auto readable = [&](pl_tex src) {
				// Gate r3 (2026-10-18, on the GeForce): a plane the GPU unpack wrote with imageStore and
				// then blitted into a host-readable copy read back stale texels (wrong for both
				// unpacks, differently); downloaded directly it is exact (GpuCaptureUnpackMatchesTheCpu-
				// Formatters reads the same way). So the unpack's planes are made host-readable
				// (SetHostReadableForTest) and downloaded directly; the CPU route's uploads keep the copy.
				if (src->params.host_readable)
				{
					std::vector<uint8_t> direct(static_cast<size_t>(src->params.w) * src->params.h * src->params.format->texel_size);
					pl_tex_transfer_params dd{};
					dd.tex = src;
					dd.ptr = direct.data();
					Assert::IsTrue(pl_tex_download(gpu, &dd), L"download");
					return direct;
				}
				pl_tex_params p{};
				p.w = src->params.w;
				p.h = src->params.h;
				p.format = src->params.format;
				p.host_readable = true;
				p.blit_dst = true;
				pl_tex copy = pl_tex_create(gpu, &p);
				Assert::IsNotNull(copy, L"a host-readable copy");
				pl_tex_blit_params b{};
				b.src = src;
				b.dst = copy;
				pl_tex_blit(gpu, &b);
				std::vector<uint8_t> bytes(static_cast<size_t>(p.w) * p.h * src->params.format->texel_size);
				pl_tex_transfer_params d{};
				d.tex = copy;
				d.ptr = bytes.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"download");
				pl_tex_destroy(gpu, &copy);
				return bytes;
			};
			auto samePlanes = [&](pl_tex (&x)[2], pl_tex (&y)[2], const wchar_t* what) {
				for (int plane = 0; plane < 2; ++plane)
				{
					Assert::IsTrue(x[plane] && y[plane], what);
					Assert::IsTrue(x[plane]->params.format == y[plane]->params.format &&
						x[plane]->params.w == y[plane]->params.w && x[plane]->params.h == y[plane]->params.h,
						(std::wstring(what) + L": the same format and size").c_str());
					const std::vector<uint8_t> bx = readable(x[plane]), by = readable(y[plane]);
					if (bx != by)
					{
						size_t differ = 0, first = bx.size(), last = 0;
						for (size_t i = 0; i < bx.size() && i < by.size(); ++i)
							if (bx[i] != by[i]) { ++differ; first = (std::min)(first, i); last = i; }
						const size_t rowBytes = static_cast<size_t>(x[plane]->params.w) * x[plane]->params.format->texel_size;
						char text[256];
						snprintf(text, sizeof(text), "plane %d (%dx%d %s): %zu bytes differ, first at row %zu col byte %zu, last at row %zu col byte %zu",
							plane, x[plane]->params.w, x[plane]->params.h, x[plane]->params.format->name, differ,
							first / rowBytes, first % rowBytes, last / rowBytes, last % rowBytes);
						Logger::WriteMessage(text);
					}
					Assert::IsTrue(bx == by, (std::wstring(what) + L": the same bytes").c_str());
				}
			};
			// A pair that held something else before (another size and format): recreated alike.
			auto stale = [&](pl_tex (&pair)[2]) {
				pl_tex_params p{};
				p.w = 64;
				p.h = 32;
				p.format = pl_find_named_fmt(gpu, "rgba8");
				p.sampleable = true;
				for (pl_tex& t : pair) t = pl_tex_create(gpu, &p);
			};
			// The CPU route (P010 4:2:0 and P210 4:2:2), as RenderLocked and UploadDejudderPlanes upload it.
			for (const bool fullHeightChroma : { false, true })
			{
				const int chromaHeight = fullHeightChroma ? height : (height + 1) / 2;
				const size_t rowBytes = static_cast<size_t>(width) * 2;
				std::vector<uint8_t> payload(rowBytes * height + rowBytes * chromaHeight);
				for (size_t i = 0; i + 1 < payload.size(); i += 2)
				{
					const uint16_t code = static_cast<uint16_t>((next() % 1024) << 6);
					payload[i] = static_cast<uint8_t>(code & 0xff);
					payload[i + 1] = static_cast<uint8_t>(code >> 8);
				}
				auto upload = [&](pl_tex (&dst)[2], pl_plane (&out)[2]) {
					pl_plane_data planes[2]{};
					planes[0].type = PL_FMT_UNORM;
					planes[0].width = width;
					planes[0].height = height;
					planes[0].component_size[0] = 16;
					planes[0].component_map[0] = PL_CHANNEL_Y;
					planes[0].pixel_stride = sizeof(uint16_t);
					planes[0].row_stride = rowBytes;
					planes[0].pixels = payload.data();
					planes[1] = planes[0];
					planes[1].width = (width + 1) / 2;
					planes[1].height = chromaHeight;
					planes[1].component_size[1] = 16;
					planes[1].component_map[0] = PL_CHANNEL_CB;
					planes[1].component_map[1] = PL_CHANNEL_CR;
					planes[1].pixel_stride = sizeof(uint16_t) * 2;
					planes[1].pixels = payload.data() + rowBytes * static_cast<size_t>(height);
					for (int plane = 0; plane < 2; ++plane)
						Assert::IsTrue(pl_upload_plane(gpu, &out[plane], &dst[plane], &planes[plane]), L"pl_upload_plane");
				};
				pl_tex shown[2] = {}, next2[2] = {};
				stale(next2);
				pl_plane shownPlanes[2]{}, nextPlanes[2]{};
				upload(shown, shownPlanes);
				upload(next2, nextPlanes);
				const wchar_t* what = fullHeightChroma ? L"P210 CPU route" : L"P010 CPU route";
				samePlanes(shown, next2, what);
				for (int plane = 0; plane < 2; ++plane)
				{
					// The description the reuse hands on (TakeNextPlanes) is the upload's own.
					Assert::AreEqual(shownPlanes[plane].components, nextPlanes[plane].components, what);
					for (int c = 0; c < 4; ++c)
						Assert::AreEqual(shownPlanes[plane].component_mapping[c], nextPlanes[plane].component_mapping[c], what);
				}
				for (pl_tex* t : { &shown[0], &shown[1], &next2[0], &next2[1] }) pl_tex_destroy(gpu, t);
			}
			// capture_path gpu: the raw V210 / UYVY frame up and unpacked (GpuCaptureUnpack).
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::V210, VideoFrameEncoding::UYVY })
			{
				const size_t rowBytes = encoding == VideoFrameEncoding::V210 ?
					static_cast<size_t>((width + 47) / 48) * 128 : static_cast<size_t>(width) * 2;
				std::vector<uint8_t> raw(rowBytes * height);
				for (uint8_t& b : raw) b = static_cast<uint8_t>(next());
				GpuCaptureUnpack shownUnpack(fixture.Log(), gpu), nextUnpack(fixture.Log(), gpu);
				shownUnpack.SetHostReadableForTest(true);
				nextUnpack.SetHostReadableForTest(true);
				pl_tex shown[2] = {}, next2[2] = {};
				stale(next2);
				Assert::IsTrue(shownUnpack.Unpack(encoding, raw.data(), rowBytes, width, height, shown), L"unpack (shown)");
				Assert::IsTrue(nextUnpack.Unpack(encoding, raw.data(), rowBytes, width, height, next2), L"unpack (next)");
				samePlanes(shown, next2, encoding == VideoFrameEncoding::V210 ? L"V210 GPU unpack" : L"UYVY GPU unpack");
				for (pl_tex* t : { &shown[0], &shown[1], &next2[0], &next2[1] }) pl_tex_destroy(gpu, t);
			}
		}

		// The efficiency round's GPU benchmark (Claude runs it on the GeForce / the 3090):
		// VP_TEST_GPU=hardware (VP_TEST_REQUIRE_HARDWARE=1 makes a missing GPU fail). Report only:
		// per source frame at 4K and 1080p, wall clock with the GPU drained (mean of 12 after a
		// warm-up), the stages the round touched or ranked:
		//   - the source upload on each route (what efficiency_fixes saves per new frame while the
		//     de-judder estimates: the frame shown no longer uploads its payload a second time);
		//   - the de-judder's PRE_OUTPUT cache of the rendered picture (a full render with and
		//     without the cache hook armed: the candidate "fused cache" of the report);
		//   - re-base 2:1 and the source-resolution monitor for scale.
		TEST_METHOD(EfficiencyStageCostsOnThisGpu)
		{
			if (!HardwarePrerequisites::RequireConditionOrSkip("EfficiencyStageCostsOnThisGpu",
				ExtendedRangeGpuProbe::OnHardwareGpu(), "needs VP_TEST_GPU=hardware (WARP times mean nothing)"))
				return;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			auto timed = [&](const std::string& name, const std::function<bool()>& pass) {
				for (int i = 0; i < 3; ++i)
					Assert::IsTrue(pass(), ExtendedRangeGpuProbe::ToWide(name + ": failed").c_str());
				pl_gpu_finish(gpu);
				const int runs = 12;
				const auto start = std::chrono::steady_clock::now();
				for (int i = 0; i < runs; ++i) pass();
				pl_gpu_finish(gpu);
				const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / runs;
				char line[200];
				snprintf(line, sizeof(line), "  %-66s %7.3f ms\n", name.c_str(), ms);
				Logger::WriteMessage(line);
				return ms;
			};
			uint32_t seed = 0x2545f491u;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			Logger::WriteMessage("Efficiency stage costs (this GPU, per source frame):\n");
			for (const auto size : { std::make_pair(3840, 2160), std::make_pair(1920, 1080) })
			{
				const int width = size.first, height = size.second;
				const std::string tag = std::to_string(width) + "x" + std::to_string(height);
				// The CPU route (P010 4:2:0 / P210 4:2:2).
				double cpuRouteMs[2] = {};
				for (const bool fullHeightChroma : { false, true })
				{
					const int chromaHeight = fullHeightChroma ? height : (height + 1) / 2;
					const size_t rowBytes = static_cast<size_t>(width) * 2;
					std::vector<uint8_t> payload(rowBytes * height + rowBytes * chromaHeight);
					for (size_t i = 0; i + 1 < payload.size(); i += 2)
					{
						const uint16_t code = static_cast<uint16_t>((64 + next() % 877) << 6);
						payload[i] = static_cast<uint8_t>(code & 0xff);
						payload[i + 1] = static_cast<uint8_t>(code >> 8);
					}
					pl_tex dst[2] = {};
					cpuRouteMs[fullHeightChroma ? 1 : 0] = timed(tag + (fullHeightChroma ? " P210" : " P010") +
						" upload (CPU route; saved per new frame)", [&] {
						pl_plane_data planes[2]{};
						planes[0].type = PL_FMT_UNORM;
						planes[0].width = width;
						planes[0].height = height;
						planes[0].component_size[0] = 16;
						planes[0].component_map[0] = PL_CHANNEL_Y;
						planes[0].pixel_stride = sizeof(uint16_t);
						planes[0].row_stride = rowBytes;
						planes[0].pixels = payload.data();
						planes[1] = planes[0];
						planes[1].width = (width + 1) / 2;
						planes[1].height = chromaHeight;
						planes[1].component_size[1] = 16;
						planes[1].component_map[0] = PL_CHANNEL_CB;
						planes[1].component_map[1] = PL_CHANNEL_CR;
						planes[1].pixel_stride = sizeof(uint16_t) * 2;
						planes[1].pixels = payload.data() + rowBytes * static_cast<size_t>(height);
						pl_plane out[2]{};
						return pl_upload_plane(gpu, &out[0], &dst[0], &planes[0]) && pl_upload_plane(gpu, &out[1], &dst[1], &planes[1]);
					});
					pl_tex_destroy(gpu, &dst[0]);
					pl_tex_destroy(gpu, &dst[1]);
				}
				// capture_path gpu (the default): the raw V210 frame up and unpacked.
				const size_t v210Row = static_cast<size_t>((width + 47) / 48) * 128;
				std::vector<uint8_t> raw(v210Row * height);
				for (uint8_t& b : raw) b = static_cast<uint8_t>(next());
				GpuCaptureUnpack unpack(fixture.Log(), gpu);
				pl_tex planes[2] = {};
				const double rawMs = timed(tag + " V210 raw upload + GPU unpack (default route; saved)", [&] {
					return unpack.Unpack(VideoFrameEncoding::V210, raw.data(), v210Row, width, height, planes);
				});
				// The de-judder's rendered-picture cache: a render of the frame to an output of the
				// same size, with and without the PRE_OUTPUT cache hook armed (fbo_float32 as the
				// renderer's default: rgba32f intermediates).
				pl_tex_params tp{};
				tp.w = width;
				tp.h = height;
				tp.format = pl_find_named_fmt(gpu, "rgb10a2");
				tp.renderable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target, L"render target");
				pl_frame source{};
				source.num_planes = 2;
				source.planes[0].texture = planes[0];
				source.planes[0].components = 1;
				source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				source.planes[1].texture = planes[1];
				source.planes[1].components = 2;
				source.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				source.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				source.repr = pl_color_repr_hdtv;
				source.repr.bits.sample_depth = 16;
				source.repr.bits.color_depth = 10;
				source.color = pl_color_space_bt709;
				source.crop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
				pl_frame out{};
				out.num_planes = 1;
				out.planes[0].texture = target;
				out.planes[0].components = 3;
				out.planes[0].component_mapping[0] = PL_CHANNEL_R;
				out.planes[0].component_mapping[1] = PL_CHANNEL_G;
				out.planes[0].component_mapping[2] = PL_CHANNEL_B;
				out.repr = pl_color_repr_rgb;
				out.repr.bits.color_depth = 10;
				out.color = pl_color_space_bt709;
				out.crop = source.crop;
				pl_render_params params = pl_render_high_quality_params;
				params.dither_params = nullptr;
				DejudderRendered::Cache cache;
				const pl_hook* hooks[1] = { cache.Hook() };
				uint64_t sequence = 0;
				const double renderMs = timed(tag + " render (high quality, no hooks)", [&] {
					return pl_render_image(fixture.Renderer(), &source, &out, &params);
				});
				pl_render_params cached = params;
				cached.hooks = hooks;
				cached.num_hooks = 1;
				const double cachedMs = timed(tag + " render with the de-judder cache armed", [&] {
					cache.Arm(++sequence);
					return pl_render_image(fixture.Renderer(), &source, &out, &cached);
				});
				char line[260];
				snprintf(line, sizeof(line), "  %s: efficiency_fixes saves %.3f ms (default route) / %.3f ms (P010) / %.3f ms (P210) per new "
					"frame with the de-judder on; the cache costs %.3f ms per new frame with extras (fused-cache candidate)\n",
					tag.c_str(), rawMs, cpuRouteMs[0], cpuRouteMs[1], cachedMs - renderMs);
				Logger::WriteMessage(line);
				cache.Release();
				// For scale: the re-base and the monitor's tile copy.
				SourceRebase::Module rebase;
				pl_frame rebased{};
				timed(tag + " re-base 2:1 (polar luma + separable chroma)", [&] { return rebase.Apply(gpu, dispatch, source, 0.5, rebased); });
				rebase.Release(gpu);
				SourceResolutionMonitor::Monitor monitor;
				double seconds = 0.0;
				timed(tag + " source-resolution monitor tile copy (2 Hz)", [&] {
					monitor.Frame(gpu, dispatch, source, ++sequence, seconds);
					seconds += 0.6;
					return true;
				});
				monitor.Release(gpu);
				pl_tex_destroy(gpu, &target);
				pl_tex_destroy(gpu, &planes[0]);
				pl_tex_destroy(gpu, &planes[1]);
			}
			pl_dispatch_destroy(&dispatch);
		}

		// Every hook at once, in the renderer's order, over frames with a cut
		// (ChainGpuProbe.h): libplacebo 7.360 with the encoding repair.
		TEST_METHOD(WholeHookChainKeepsThePictureOnEveryFrame)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log;
			fixture.CaptureLog(&log);
			const ChainGpuProbe::Outcome outcome = ChainGpuProbe::Run(fixture.Gpu(), fixture.Renderer(), fixture.Log(),
				[](const char* file) { return LoadBundledShader(file); }, true);
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(outcome.report.c_str());
			Assert::IsTrue(outcome.ok, ExtendedRangeGpuProbe::ToWide(outcome.failure).c_str());
			// The 7.360 PRE_OUTPUT note (repaired) and shader-compiler timing notes only.
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		// The combination matrix (ChainGpuProbe::RunMatrix): a spread of 54
		// combinations by default, all 336 with VP_TEST_MATRIX=full.
		TEST_METHOD(WholeHookChainCombinationMatrix)
		{
			char* value = nullptr;
			size_t length = 0;
			const bool full = _dupenv_s(&value, &length, "VP_TEST_MATRIX") == 0 && value && std::string(value) == "full";
			std::free(value);
			// VP_TEST_MATRIX_ONLY: run only the combinations whose name contains this text.
			std::string only;
			value = nullptr;
			if (_dupenv_s(&value, &length, "VP_TEST_MATRIX_ONLY") == 0 && value)
				only = value;
			std::free(value);
			std::string report;
			int passed = 0;
			for (const ChainGpuProbe::Combo& combo : ChainGpuProbe::MatrixCombos(full))
			{
				if (!only.empty() && combo.Name().find(only) == std::string::npos)
					continue;
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create());
				std::string log;
				fixture.CaptureLog(&log);
				const ChainGpuProbe::Outcome outcome = ChainGpuProbe::RunMatrix(fixture.Gpu(), fixture.Renderer(), fixture.Log(),
					[](const char* file) { return LoadBundledShader(file); }, true, combo);
				fixture.CaptureLog(nullptr);
				report += outcome.report;
				Assert::IsTrue(outcome.ok, ExtendedRangeGpuProbe::ToWide(outcome.failure).c_str());
				AssertCleanLibplaceboLogExceptPreOutputBug(log, combo.Name());
				++passed;
			}
			Logger::WriteMessage((report + std::to_string(passed) + " combinations passed\n").c_str());
		}

		TEST_METHOD(WholeHookChainUpscales)
		{
			for (const char* prescaler : { "FSRCNNX_x2_16-0-4-1.glsl", "ArtCNN_C4F16.glsl" })
			{
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create());
				std::string log;
				fixture.CaptureLog(&log);
				const ChainGpuProbe::Outcome outcome = ChainGpuProbe::RunUpscale(fixture.Gpu(), fixture.Renderer(), fixture.Log(),
					[](const char* file) { return LoadBundledShader(file); }, true, prescaler);
				fixture.CaptureLog(nullptr);
				Logger::WriteMessage(outcome.report.c_str());
				Assert::IsTrue(outcome.ok, ExtendedRangeGpuProbe::ToWide(outcome.failure).c_str());
				AssertCleanLibplaceboLogExceptPreOutputBug(log);
			}
		}

		TEST_METHOD(WholeHookChainHandlesMotionAndTheLookAheads)
		{
			// Twice: as built, and with every refinement on (D1-D4, S1).
			for (bool refinements : { false, true })
			{
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create());
				std::string log;
				fixture.CaptureLog(&log);
				const ChainGpuProbe::Outcome outcome = ChainGpuProbe::RunMotion(fixture.Gpu(), fixture.Renderer(), fixture.Log(),
					[](const char* file) { return LoadBundledShader(file); }, true, 63u, refinements);
				fixture.CaptureLog(nullptr);
				Logger::WriteMessage((refinements ? "-- refinements on\n" : "-- refinements off\n"));
				Logger::WriteMessage(outcome.report.c_str());
				Assert::IsTrue(outcome.ok, ExtendedRangeGpuProbe::ToWide((refinements ? "refinements on: " : "") +
					outcome.failure).c_str());
				AssertCleanLibplaceboLogExceptPreOutputBug(log);
			}
		}

		// Every warning or error libplacebo logs fails the test (design review 4, N-6): the clean-log
		// check that used to sit inside Bill's BundledNlsGlSlHooksMovePixelsOnTheRealGpuPath.
		TEST_METHOD(BundledNlsHooksLogCleanly)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			fixture.RenderCoordinateField(nullptr);
			const pl_hook* balanced = ParseBundledNlsShader(fixture.Gpu(), "NLSPlus.glsl", 0.25, 0.8);
			BindNlsShader(balanced, 1.32f, 0.0f);
			fixture.RenderCoordinateField(balanced);
			const pl_hook* oneAxisPlus = ParseBundledNlsShader(fixture.Gpu(), "NLSPlus.glsl", 0.0);
			const pl_hook* established = ParseBundledNlsShader(fixture.Gpu(), "NLS.glsl", 0.0);
			BindNlsShader(oneAxisPlus, 1.32f, 0.0f);
			BindNlsShader(established, 1.32f, 0.0f);
			fixture.RenderCoordinateField(oneAxisPlus);
			fixture.RenderCoordinateField(established);
			BindNlsShader(established, 1.125f, 1.0f);
			fixture.RenderCoordinateField(established);
			pl_mpv_user_shader_destroy(&balanced);
			pl_mpv_user_shader_destroy(&oneAxisPlus);
			pl_mpv_user_shader_destroy(&established);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog);
		}
	};
}
