// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(DejudderHookTests)
	{
	public:


        // Hunt 2 10-layout T6 (M1): a failing NLS hook makes libplacebo draw a plain stretch
        // into the NLS screen rectangle; the de-judder cache copied that picture at
        // PRE_OUTPUT. The recovery's redraw into the linear fit must leave no cached picture
        // (the renderer invalidates the cache in the fallback, so this frame's extras hold).
        // Without the invalidation the stale full-width stretch stays cached (the defect).
        BEGIN_TEST_METHOD_ATTRIBUTE(NlsHookFailureRetryLeavesNoStaleDejudderCache)
            TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
        END_TEST_METHOD_ATTRIBUTE()
        TEST_METHOD(NlsHookFailureRetryLeavesNoStaleDejudderCache) {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const pl_gpu gpu=fixture.Gpu();
            const pl_fmt format=pl_find_fmt(gpu,PL_FMT_UNORM,4,8,8,static_cast<pl_fmt_caps>(
                PL_FMT_CAP_SAMPLEABLE|PL_FMT_CAP_RENDERABLE|PL_FMT_CAP_HOST_READABLE|PL_FMT_CAP_BLITTABLE));
            Assert::IsNotNull(format);
            std::vector<RgbaPixel> white(16*16,RgbaPixel{255,255,255,255});
            pl_tex_params sourceParams{};sourceParams.w=16;sourceParams.h=16;sourceParams.format=format;
            sourceParams.sampleable=true;sourceParams.initial_data=white.data();
            pl_tex source=pl_tex_create(gpu,&sourceParams);Assert::IsNotNull(source);
            pl_tex_params targetParams{};targetParams.w=32;targetParams.h=16;targetParams.format=format;
            targetParams.renderable=true;targetParams.host_readable=true;targetParams.blit_dst=true;
            pl_tex output=pl_tex_create(gpu,&targetParams);Assert::IsNotNull(output);
            pl_hook broken{};broken.stages=PL_HOOK_RGB;broken.input=PL_HOOK_SIG_NONE;
            broken.signature=0x0070197;
            broken.hook=[](void*,const pl_hook_params*) {pl_hook_res r{};r.failed=true;return r;};
            const pl_rect2df nlsScreen{0,0,32,16}, linearFit{8,0,24,16};
            for(const bool fixed:{false,true}) {
                fixture.ResetFailedHook(broken.signature);
                DejudderRendered::Cache cache;
                cache.Arm(7);
                std::vector<const pl_hook*> hooks={&broken,cache.Hook()};
                pl_render_params params=pl_render_fast_params;
                params.hooks=hooks.data();params.num_hooks=static_cast<int>(hooks.size());
                pl_frame image=TargetLutGpuFixture::MakeRgbFrame(source);
                pl_frame target=TargetLutGpuFixture::MakeRgbFrame(output);
                const pl_frame fullTarget=target;
                target.crop=nlsScreen;
                const auto result=NlsHookRecovery::RenderOwned(fixture.Renderer(),gpu,image,target,fullTarget,params,hooks,&broken,
                    [&]() {target.crop=linearFit;if(fixed) cache.Invalidate();},
                    [&](const pl_frame& s,const pl_frame& d,pl_render_params& p) {
                        return pl_render_image(fixture.Renderer(),&s,&d,&p);});
                pl_gpu_finish(gpu);
                Assert::IsTrue(result.retried,L"the failed NLS hook was retried");
                Assert::IsTrue(result.rendered);
                const pl_tex cached=cache.Texture(7);
                if(fixed)
                    Assert::IsNull(cached,L"after the recovery no picture of the failed draw is cached");
                else {
                    Assert::IsNotNull(cached,L"control: without the invalidation the failed draw's picture stays");
                    Assert::AreEqual(32,cached->params.w,L"control: it is the full-width NLS-rectangle stretch");
                }
                cache.Release();
            }
            pl_tex_destroy(gpu,&output);pl_tex_destroy(gpu,&source);
        }

		TEST_METHOD(DejudderIdenticalFramesGiveExactZeroVectors)
		{
			// Bug hunt 2026-10-25: a repeated frame (A and B bit-identical, separate textures) must give
			// exactly the zero vector in every block, trusted - the estimate left sub-pixel noise vectors
			// (up to 1.26 px) which the interpolator resampled. A block that changes keeps its search.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			auto luma = [](int x, int y) { return DejudderTexture(x, y); };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, luma, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height, luma, chroma);
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			std::string log;
			fixture.CaptureLog(&log);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.estStillZero = true;   // dejudder_est_still_zero (the renderer's default)
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
			const DejudderField field = DownloadField(gpu, warp);
			int nonZero = 0, untrusted = 0;
			for (int by = 0; by < field.blocksY; ++by)
				for (int bx = 0; bx < field.blocksX; ++bx)
				{
					nonZero += (field.vx(bx, by) != 0.0f || field.vy(bx, by) != 0.0f) ? 1 : 0;
					untrusted += field.trusted(bx, by) ? 0 : 1;
				}
			char line[160];
			snprintf(line, sizeof(line), "identical frames: %d of %d blocks with a non-zero vector, %d untrusted\n", nonZero, field.blocksX * field.blocksY, untrusted);
			Logger::WriteMessage(line);
			Assert::AreEqual(0, nonZero, L"identical blocks give exactly the zero vector");
			Assert::AreEqual(0, untrusted, L"identical blocks are trusted");

			// Audit F1: one 8x8 sub-block of a 16x16 block changes, its three sibling sub-blocks are
			// identical. With the split on, the siblings must read exactly (0, 0) through the consumers'
			// rule (w > 0.5: the sub-block's own vector, else the parent block's).
			DejudderFrame moved = b;
			const int bx0 = 64, by0 = 32;   // block (4, 2); its top-left sub-block changes
			for (int y = by0; y < by0 + 8; ++y)
				for (int x = bx0; x < bx0 + 8; ++x)
					moved.luma[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(DejudderTexture(x + 5, y + 3));
			DejudderGpuPlanes planesMoved;
			UploadDejudderFrame(gpu, moved, planesMoved);
			Dejudder::Settings split = settings;
			split.estSplit = true;
			Dejudder::MotionWarp splitWarp(fixture.Log(), gpu, split);
			Assert::IsTrue(splitWarp.Estimate(planesA.planes, planesMoved.planes), std::wstring(splitWarp.LastError().begin(), splitWarp.LastError().end()).c_str());
			pl_tex sub = splitWarp.SubField();
			Assert::IsNotNull(sub, L"the sub-field (est_split)");
			std::vector<uint16_t> subHalf(static_cast<size_t>(sub->params.w) * sub->params.h * 4);
			pl_tex_transfer_params subDownload{};
			subDownload.tex = sub;
			subDownload.ptr = subHalf.data();
			Assert::IsTrue(pl_tex_download(gpu, &subDownload), L"sub-field readable");
			const DejudderField parent = DownloadField(gpu, splitWarp);
			auto halfToFloat = [](uint16_t h) {
				const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
				const float v = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24) :
					std::ldexp(static_cast<float>(mantissa | 1024), exponent - 25);
				return sign ? -v : v; };
			int siblingsWrong = 0;
			for (int sy = 0; sy < 2; ++sy)
				for (int sx = 0; sx < 2; ++sx)
				{
					if (sx == 0 && sy == 0) continue;   // the changed sub-block
					const size_t at = (static_cast<size_t>(4 + sy) * sub->params.w + (8 + sx)) * 4;   // sub-block (8 + sx, 4 + sy)
					const bool own = halfToFloat(subHalf[at + 3]) > 0.5f;
					const float vx = own ? halfToFloat(subHalf[at]) : parent.vx(4, 2);
					const float vy = own ? halfToFloat(subHalf[at + 1]) : parent.vy(4, 2);
					if (vx != 0.0f || vy != 0.0f || !own) ++siblingsWrong;
				}
			Assert::AreEqual(0, siblingsWrong, L"identical sub-blocks of a changed block read exactly (0, 0) as their own vector");

			// dejudder_est_still_zero off: the pass does not run (the searches' vectors as before).
			Dejudder::Settings off = settings;
			off.estStillZero = false;
			Dejudder::MotionWarp offWarp(fixture.Log(), gpu, off);
			Assert::IsTrue(offWarp.Estimate(planesA.planes, planesB.planes), L"the estimate with the switch off");
		}

		TEST_METHOD(DejudderFollowsAMovingPictureAndWarpsBySDelta)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			// Content moves (12, 6) source pixels from A to B.
			static constexpr int dx = 12, dy = 6;
			auto chromaA = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chromaA);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);

			std::string log;
			fixture.CaptureLog(&log);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			const bool estimated = warp.Estimate(planesA.planes, planesB.planes);
			Assert::IsTrue(estimated, std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());

			// Interior blocks: trusted, vector (12, 6).
			const DejudderField field = DownloadField(gpu, warp);
			int interior = 0, good = 0;
			for (int by = 2; by < field.blocksY - 2; ++by)
				for (int bx = 2; bx < field.blocksX - 2; ++bx)
				{
					++interior;
					if (field.trusted(bx, by) && std::abs(field.vx(bx, by) - dx) < 0.6f &&
						std::abs(field.vy(bx, by) - dy) < 0.6f)
						++good;
				}
			// Both searches, for the message: the trust needs them to agree.
			const DejudderField fwdField = DownloadField(gpu, warp, warp.Forward());
			const DejudderField bwdField = DownloadField(gpu, warp, warp.Backward());
			int fwdOk = 0, bwdOk = 0;
			for (int by = 2; by < field.blocksY - 2; ++by)
				for (int bx = 2; bx < field.blocksX - 2; ++bx)
				{
					fwdOk += (std::abs(fwdField.vx(bx, by) - 6) < 0.3f && std::abs(fwdField.vy(bx, by) - 3) < 0.3f) ? 1 : 0;
					bwdOk += (std::abs(bwdField.vx(bx, by) + 6) < 0.3f && std::abs(bwdField.vy(bx, by) + 3) < 0.3f) ? 1 : 0;
				}
			Assert::IsTrue(good * 100 >= interior * 95,
				(L"Trusted (12, 6) vectors: " + std::to_wstring(good) + L" of " + std::to_wstring(interior) +
					L" (forward right " + std::to_wstring(fwdOk) + L", backward right " + std::to_wstring(bwdOk) +
					L"); log: " + std::wstring(log.begin(), log.end())).c_str());

			// shift 0.5: the picture lands half way, (6, 3).
			Dejudder::Planes out;
			Assert::IsTrue(warp.Warp(planesA.planes, 0.5f, out));
			Assert::IsTrue(out.luma != planesA.planes.luma, L"A warp writes new planes");
			const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
			int checked = 0, exact = 0;
			for (int y = 24; y < height - 24; ++y)
				for (int x = 24; x < width - 24; ++x)
				{
					++checked;
					if (std::abs(static_cast<int>(luma[y * width + x]) - DejudderTexture(x - 6, y - 3)) <= 2)
						++exact;
				}
			Assert::IsTrue(exact * 100 >= checked * 95, // held (untrusted) blocks excepted
				(L"Warped luma at (6, 3): " + std::to_wstring(exact) + L" of " + std::to_wstring(checked)).c_str());

			// CbCr (4:2:2): the horizontal motion is half in chroma pixels.
			const std::vector<uint16_t> chroma = DownloadPlane(gpu, out.chroma);
			int cchecked = 0, cexact = 0;
			for (int y = 24; y < height - 24; ++y)
				for (int x = 12; x < width / 2 - 12; ++x)
				{
					++cchecked;
					const int expected = DejudderTexture((x - 3) * 2, y - 3, 7);
					if (std::abs(static_cast<int>(chroma[(y * (width / 2) + x) * 2]) - expected) <= 2)
						++cexact;
				}
			Assert::IsTrue(cexact * 100 >= cchecked * 95,
				(L"Warped chroma at (3, 3): " + std::to_wstring(cexact) + L" of " + std::to_wstring(cchecked)).c_str());

			// Edges clamp to A's edge pixel, never black: the left columns of a
			// picture moving right sample beyond the edge.
			for (int y = 24; y < height - 24; y += 7)
				for (int x = 0; x < 6; ++x)
					Assert::IsTrue(luma[y * width + x] > 4000,
						L"A sample beyond the edge must clamp to the picture, not black");

			// shift 0 (the refresh A starts on) is A itself.
			Assert::IsTrue(warp.Warp(planesA.planes, 0.0f, out));
			Assert::IsTrue(out.luma == planesA.planes.luma && out.chroma == planesA.planes.chroma);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// 2026-10-16 (the D1 taste report): at 4K the global moves in 8-px steps (the low-resolution
		// matcher), coarser than the roll-off's 16-px ramp, so a 12 or 20 px pan read 8 / 16 / 24 by
		// rounding luck. The roll-off now reads the refined camera (the trusted blocks near the
		// global): its strength follows the true speed, from the search and from the engine alike.
		TEST_METHOD(RolloffFollowsTheTruePanSpeedAt4K)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 3840, height = 2160, frames = 8;
			auto expected = [](double v) {
				const double t = (std::min)(1.0, (std::max)(0.0, (v - 8.0) / 16.0));
				const double sm = t * t * (3.0 - 2.0 * t);
				return 1.0 + (0.25 - 1.0) * sm;
			};
			int failures = 0;
			bool engineSeen = true;
			for (const int dx : { 12, 20 })
			{
				std::vector<DejudderGpuPlanes> planes(static_cast<size_t>(frames));
				for (int f = 0; f < frames; ++f)
				{
					const int shift = dx * f;
					const DejudderFrame frame = MakeDejudderFrame(width, height,
						[shift](int x, int y) { return DejudderTexture(x - shift, y); },
						[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); });
					UploadDejudderFrame(gpu, frame, planes[static_cast<size_t>(f)]);
				}
				for (int mode = 0; mode < 2; ++mode)
				{
					Dejudder::Settings settings = DejudderTestSettings();
					settings.globalPrior = true;
					settings.estCandidates = true;
					settings.globalTwo = true;
					settings.lowRes = true;
					settings.lowResAll = true;
					settings.motionRolloff = true;
					settings.opticalFlow = mode == 1;
					settings.opticalFlowCandidate = true;
					settings.opticalFlowSeed = true;
					settings.opticalFlowGlobal = mode == 1;
					settings.opticalFlowGlobalReadback = mode == 1;
					Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
					for (int f = 0; f + 1 < frames; ++f)
						Assert::IsTrue(warp.Estimate(planes[static_cast<size_t>(f)].planes, planes[static_cast<size_t>(f + 1)].planes),
							std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
					if (mode == 1 && !warp.GlobalFromEngine()) { engineSeen = false; continue; }
					pl_tex trust = warp.FrameTrust();
					std::vector<uint8_t> raw(trust->params.format->texel_size);
					pl_tex_transfer_params d{}; d.tex = trust; d.ptr = raw.data();
					Assert::IsTrue(pl_tex_download(gpu, &d), L"the frame trust is not host-readable");
					float scale = 0.0f;
					if (raw.size() == 16) std::memcpy(&scale, &raw[4], 4);
					else { uint16_t h; std::memcpy(&h, &raw[2], 2); scale = HalfBits(h); }
					char line[200];
					snprintf(line, sizeof(line), "4K pan %d px, %s: roll-off scale %.3f (at the true speed %.3f)\n", dx,
						mode == 1 ? "engine global" : "search D1", scale, expected(dx));
					Logger::WriteMessage(line);
					if (std::fabs(scale - expected(dx)) > 0.05) ++failures;
				}
			}
			if (!engineSeen) Logger::WriteMessage("note: the optical-flow engine was not available; the search only\n");
			Assert::AreEqual(0, failures, L"the roll-off follows the true pan speed at 4K (within 0.05)");
		}

		TEST_METHOD(DejudderToleranceFollowsTheSourceRange)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y)
				{
					return static_cast<uint16_t>(std::min(65535, DejudderTexture(x - 8, y - 4) + 822));   // 3.2 x 257
				}, chroma);
			auto trustedShare = [&](bool fullRange)
			{
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				planesA.planes.fullRange = planesB.planes.fullRange = fullRange;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				const DejudderField field = DownloadField(gpu, warp);
				int interior = 0, trusted = 0;
				for (int by = 2; by < field.blocksY - 2; ++by)
					for (int bx = 2; bx < field.blocksX - 2; ++bx)
					{
						++interior;
						trusted += field.trusted(bx, by) ? 1 : 0;
					}
				return interior > 0 ? static_cast<double>(trusted) / interior : 0.0;
			};
			const double limited = trustedShare(false);
			const double full = trustedShare(true);
			Assert::IsTrue(limited <= 0.1, (L"limited range: 3.2 codes trusted in " + std::to_wstring(limited)).c_str());
			Assert::IsTrue(full >= 0.9, (L"full range: 2.75 codes trusted in " + std::to_wstring(full)).c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderRefinesToHalfAPixel)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			// (7, 3) source pixels = (3.5, 1.5) at half resolution.
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - 7, y - 3); }, chroma);
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
			const DejudderField field = DownloadField(gpu, warp);
			int interior = 0, good = 0;
			for (int by = 2; by < field.blocksY - 2; ++by)
				for (int bx = 2; bx < field.blocksX - 2; ++bx)
				{
					++interior;
					// Within half a source pixel: a matcher stopping at whole
					// half-resolution pixels (6 or 8, 2 or 4) would fail this.
					if (std::abs(field.vx(bx, by) - 7.0f) <= 0.5f && std::abs(field.vy(bx, by) - 3.0f) <= 0.5f)
						++good;
				}
			Assert::IsTrue(good * 100 >= interior * 90,
				(L"(7, 3) within half a pixel: " + std::to_wstring(good) + L" of " + std::to_wstring(interior)).c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderHoldsAtACutAndAtOcclusions)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };

			// A cut: B is an unrelated picture. Nothing is trusted, so the warp
			// is a hard hold of A everywhere.
			{
				const DejudderFrame a = MakeDejudderFrame(width, height,
					[](int x, int y) { return DejudderTexture(x, y); }, chroma);
				const DejudderFrame b = MakeDejudderFrame(width, height,
					[](int x, int y) { return DejudderTexture(x, y, 99); }, chroma);
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				const DejudderField field = DownloadField(gpu, warp);
				int trusted = 0;
				for (int by = 0; by < field.blocksY; ++by)
					for (int bx = 0; bx < field.blocksX; ++bx)
						trusted += field.trusted(bx, by) ? 1 : 0;
				Assert::IsTrue(trusted * 100 <= field.blocksX * field.blocksY * 2,
					(L"Trusted blocks across a cut: " + std::to_wstring(trusted)).c_str());
				// Frame trust (6b-2): the GPU's own count matches, and is below
				// the 25% whole-frame hold.
				const float share = DownloadField(gpu, warp, warp.FrameTrust()).texels[0];
				Assert::IsTrue(std::abs(share - static_cast<float>(trusted) / (field.blocksX * field.blocksY)) < 0.002f,
					(L"Frame trust " + std::to_wstring(share)).c_str());
				Assert::IsTrue(share < 0.25f);
				Dejudder::Planes out;
				Assert::IsTrue(warp.Warp(planesA.planes, 0.3f, out));
				Assert::IsTrue(out.luma != planesA.planes.luma, L"The GPU hold still writes the warped planes");
				// Below the frame trust the whole frame is held: every luma and
				// chroma sample is exactly A's, including the few blocks the
				// matcher trusted by chance.
				const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
				const std::vector<uint16_t> cbcr = DownloadPlane(gpu, out.chroma);
				Assert::IsTrue(luma == a.luma, L"Across a cut the luma must be exactly A");
				Assert::IsTrue(cbcr == a.chroma, L"Across a cut the chroma must be exactly A");
				// The caller's own cut flag holds everything.
				Assert::IsTrue(warp.Warp(planesA.planes, 0.3f, out, false));
				Assert::IsTrue(out.luma == planesA.planes.luma);
			}

			// Occlusion: a textured square moves 16 px right over a still
			// background. Blocks at its leading and trailing edges are covered
			// or revealed and must hold; the square and the far background
			// keep their own motion.
			{
				auto scene = [](int shift)
				{
					return [shift](int x, int y)
					{
						const bool inside = x >= 80 + shift && x < 144 + shift && y >= 32 && y < 96;
						return inside ? DejudderTexture(x - shift, y, 5) : DejudderTexture(x, y);
					};
				};
				const DejudderFrame a = MakeDejudderFrame(width, height, scene(0), chroma);
				const DejudderFrame b = MakeDejudderFrame(width, height, scene(16), chroma);
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes, 1));
				// 6c: B's occlusion mask. In B the square has moved to x 96..159,
				// so the background it uncovered (x 80..95, bx 5) is new; the
				// square's inside and the far background were seen in A.
				const DejudderField occlusion = DownloadField(gpu, warp, warp.Occlusion(1));
				auto seen = [&](int bx, int by) { return occlusion.texels[(by * occlusion.blocksX + bx) * 4] > 0.5f; };
				Assert::IsFalse(seen(5, 3), L"Uncovered background in B must be marked new");
				Assert::IsTrue(seen(7, 3), L"The square's inside was seen in A");
				Assert::IsTrue(seen(1, 1) && seen(14, 6), L"The still background was seen in A");
				Assert::IsNull(warp.Occlusion(2));
				const DejudderField field = DownloadField(gpu, warp);
				// Blocks are 16x16 source pixels: the square covers bx 5..8,
				// by 2..5. bx 9 is about to be covered (leading edge).
				Assert::IsFalse(field.trusted(9, 3), L"A block about to be covered must not be trusted");
				Assert::IsTrue(field.trusted(6, 3) && std::abs(field.vx(6, 3) - 16.0f) < 0.6f,
					L"The square's inside keeps its motion");
				Assert::IsTrue(field.trusted(1, 1) && std::abs(field.vx(1, 1)) < 0.6f,
					L"The still background far away is trusted with no motion");
				Assert::IsTrue(DownloadField(gpu, warp, warp.FrameTrust()).texels[0] > 0.5f,
					L"An occlusion is local: the frame stays trusted");
				Dejudder::Planes out;
				Assert::IsTrue(warp.Warp(planesA.planes, 0.4f, out));
				const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
				// Inside an untrusted block the output is A, unchanged.
				for (int y = 50; y < 60; ++y)
					for (int x = 150; x < 158; ++x)
						Assert::AreEqual(static_cast<int>(a.luma[y * width + x]),
							static_cast<int>(luma[y * width + x]), L"Occluded blocks are a hard hold of A");
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Stage 6b-2: an extra present renders A's frame with the warped planes
		// swapped in, through the normal pl_render_image. The rendered
		// picture must move by shift x the motion, and a zero shift (the
		// hold) must render exactly A's own picture.
		TEST_METHOD(DejudderExtraRenderMovesThePictureBySDelta)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));

			pl_fmt targetFormat = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(targetFormat);
			pl_tex_params targetParams{};
			targetParams.w = width;
			targetParams.h = height;
			targetParams.format = targetFormat;
			targetParams.renderable = true;
			targetParams.host_readable = true;
			pl_tex targetTexture = pl_tex_create(gpu, &targetParams);
			Assert::IsNotNull(targetTexture);
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			// The renderer's source frame: Y and interleaved CbCr, 4:2:2.
			auto render = [&](const Dejudder::Planes& planes)
			{
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planes.luma;
				image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planes.chroma;
				image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame target = TargetLutGpuFixture::MakeRgbFrame(targetTexture);
				target.color.transfer = PL_COLOR_TRC_BT_1886;
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &target, &params));
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> output(static_cast<size_t>(width) * height);
				pl_tex_transfer_params download{};
				download.tex = targetTexture;
				download.ptr = output.data();
				Assert::IsTrue(pl_tex_download(gpu, &download));
				return output;
			};
			const std::vector<RgbaPixel> own = render(planesA.planes);

			// The hold (shift 0) renders A's own picture exactly.
			Dejudder::Planes held;
			Assert::IsTrue(warp.Warp(planesA.planes, 0.0f, held));
			const std::vector<RgbaPixel> heldPixels = render(held);
			for (size_t i = 0; i < own.size(); ++i)
				Assert::IsTrue(own[i].r == heldPixels[i].r && own[i].g == heldPixels[i].g &&
					own[i].b == heldPixels[i].b, L"A zero shift must render A's own picture");

			// shift 0.5: the picture moves (6, 3) px. Find the best whole-pixel
			// displacement of the rendered output over the interior.
			Dejudder::Planes warped;
			Assert::IsTrue(warp.Warp(planesA.planes, 0.5f, warped));
			const std::vector<RgbaPixel> moved = render(warped);
			int bestX = 0, bestY = 0;
			double bestError = 1e30;
			for (int oy = -2; oy <= 6; ++oy)
				for (int ox = -2; ox <= 10; ++ox)
				{
					double error = 0.0;
					for (int y = 24; y < height - 24; ++y)
						for (int x = 32; x < width - 32; ++x)
						{
							const RgbaPixel& m = moved[static_cast<size_t>(y) * width + x];
							const RgbaPixel& o = own[static_cast<size_t>(y - oy) * width + (x - ox)];
							error += std::abs(m.r - o.r) + std::abs(m.g - o.g) + std::abs(m.b - o.b);
						}
					if (error < bestError) { bestError = error; bestX = ox; bestY = oy; }
				}
			Assert::IsTrue(bestX == dx / 2 && bestY == dy / 2,
				(L"The extra moved the picture by (" + std::to_wstring(bestX) + L", " +
					std::to_wstring(bestY) + L"), expected (6, 3)").c_str());
			pl_tex_destroy(gpu, &targetTexture);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Stage 6d: an extra made by warping A's cached PRE_OUTPUT picture
		// against one made by re-rendering a warped source (same vectors): at
		// 1:1 and 2x the picture moves by the same amount, the hold (shift 0)
		// and a cut are A exactly, and the interior differs only by the
		// kernels (bilinear on the source vs Catmull-Rom on the output) and,
		// on under 1% of samples, by the source path deciding chroma's hold
		// at chroma positions (measured: mean 0.10 / 0.15 codes, 0.28% /
		// 0.63% over 2 codes). Full-range target: on 7.360 a limited target
		// goes through VP's encoding repair for A but not for the extra (no
		// PRE_OUTPUT hook there), which rounds a code differently; the Beta
		// test checks the limited target on 377 (no repair).
		TEST_METHOD(DejudderRenderedWarpHoldsAMovedSubtitleStill)
		{
			// Owner 2026-10-13: with a moved subtitle the extras still run; its
			// rectangles (Geometry::holds, source pixels) show A exactly while
			// the rest of the picture moves, and the dither's vectors are zero
			// there.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			DejudderRendered::Cache cache;
			DejudderRendered::Warp rendered(fixture.Log(), gpu);
			DejudderRendered::VectorField field(fixture.Log(), gpu);
			pl_tex_params targetParams{};
			targetParams.w = width;
			targetParams.h = height;
			targetParams.format = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			targetParams.renderable = true;
			targetParams.host_readable = true;
			pl_tex targetTexture = pl_tex_create(gpu, &targetParams);
			Assert::IsNotNull(targetTexture);
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			auto target = [&]()
			{
				pl_frame t = TargetLutGpuFixture::MakeRgbFrame(targetTexture);
				t.repr.levels = PL_COLOR_LEVELS_FULL;
				t.color.transfer = PL_COLOR_TRC_BT_1886;
				return t;
			};
			auto download = [&]()
			{
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> output(static_cast<size_t>(width) * height);
				pl_tex_transfer_params d{};
				d.tex = targetTexture;
				d.ptr = output.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				return output;
			};
			// A rendered with the cache armed.
			{
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planesA.planes.luma;
				image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planesA.planes.chroma;
				image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame t = target();
				std::vector<const pl_hook*> hooks = { cache.Hook() };
				OutputEncodingRepair::Hook repair;
				if (OutputEncodingRepair::Needed(hooks, nullptr, PL_LUT_UNKNOWN))
				{
					repair.SetTarget(t.repr);
					hooks.insert(hooks.begin(), repair.Get());
				}
				pl_render_params p = params;
				p.hooks = hooks.data();
				p.num_hooks = static_cast<int>(hooks.size());
				cache.Arm(7);
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
			}
			const std::vector<RgbaPixel> own = download();
			Assert::IsNotNull(cache.Texture(7));
			// Twice, so the hysteresis lets the pan's blocks warp.
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes) && warp.Estimate(planesA.planes, planesB.planes));
			DejudderRendered::Geometry g;
			g.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
			g.targetCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
			// The subtitle's two rectangles: where it was and where it is shown.
			g.holds[0] = { 64.0f, 32.0f, 160.0f, 56.0f };
			g.holds[1] = { 48.0f, 96.0f, 208.0f, 120.0f };
			g.numHolds = 2;
			auto extra = [&](const DejudderRendered::Geometry& geometry)
			{
				pl_tex out = nullptr;
				Assert::IsTrue(rendered.Run(cache.Texture(7), warp.Field(), warp.FrameTrust(),
					warp.GetSettings().minTrustedShare, 0.5f, geometry, out), ExtendedRangeGpuProbe::ToWide(rendered.Error() + "; log: " + log).c_str());
				pl_frame image = DejudderRendered::Frame(out, cache.Repr(), cache.Color());
				pl_frame t = target();
				std::vector<const pl_hook*> kept;
				const pl_render_params p = DejudderRendered::OutputOnly(params, {}, kept);
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
				return download();
			};
			const std::vector<RgbaPixel> withHolds = extra(g);
			const std::vector<RgbaPixel> without = extra(DejudderRendered::Geometry{ g.sourceCrop, g.targetCrop });
			auto inside = [&](int x, int y, int i)
			{
				const pl_rect2df& r = g.holds[i];
				return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
			};
			int heldDiff = 0;
			long long movedOutside = 0, sameAsWithout = 0, outside = 0;
			for (int y = 8; y < height - 8; ++y)
				for (int x = 8; x < width - 8; ++x)
				{
					const size_t i = static_cast<size_t>(y) * width + x;
					auto diff = [](const RgbaPixel& p, const RgbaPixel& q)
					{
						return std::max({ std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b) });
					};
					if (inside(x, y, 0) || inside(x, y, 1))
						heldDiff = std::max(heldDiff, diff(withHolds[i], own[i]));
					else
					{
						// Away from the rectangles by more than the motion: as without holds.
						bool closeBy = false;
						for (int k = 0; k < 2; ++k)
						{
							const pl_rect2df& r = g.holds[k];
							closeBy = closeBy || (x >= r.x0 - 16 && x < r.x1 + 16 && y >= r.y0 - 16 && y < r.y1 + 16);
						}
						if (closeBy) continue;
						++outside;
						movedOutside += diff(withHolds[i], own[i]) > 2 ? 1 : 0;
						sameAsWithout += diff(withHolds[i], without[i]) == 0 ? 1 : 0;
					}
				}
			Assert::AreEqual(0, heldDiff, L"inside the held rectangles the extra shows A exactly");
			Assert::IsTrue(movedOutside > outside / 2, L"elsewhere the picture moves");
			Assert::AreEqual(outside, sameAsWithout, L"away from the rectangles the extra is as without holds");
			// The dither's vectors: zero in the blocks inside a rectangle.
			pl_tex vectors = nullptr;
			Assert::IsTrue(field.Run(warp.Field(), warp.FrameTrust(), warp.GetSettings().minTrustedShare, 0.5f, g,
				width, height, vectors), ExtendedRangeGpuProbe::ToWide(field.Error()).c_str());
			Assert::IsNotNull(vectors);
		}

		// Bug hunt 2026-10-17: the cached picture (PRE_OUTPUT) already has the
		// target's normalized calibration LUT applied (colour management), so an
		// extra rendered from it must not have the LUT on its target: then it
		// shows A exactly; with the LUT left on (the bug) it is applied twice.
		TEST_METHOD(DejudderCachedExtraAppliesTheTargetLutOnce)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 128, height = 64;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			DejudderGpuPlanes planesA;
			UploadDejudderFrame(gpu, a, planesA);
			// A strong, non-identity calibration LUT: out = 0.75 in + 0.15 (per channel).
			std::string cube = "LUT_3D_SIZE 2\n";
			for (int b = 0; b < 2; ++b)
				for (int g = 0; g < 2; ++g)
					for (int r = 0; r < 2; ++r)
						cube += std::to_string(0.15 + 0.75 * r) + " " + std::to_string(0.15 + 0.75 * g) + " " +
							std::to_string(0.15 + 0.75 * b) + "\n";
			pl_custom_lut* lut = pl_lut_parse_cube(fixture.Log(), cube.data(), cube.size());
			Assert::IsNotNull(lut, L"the test LUT did not parse");
			struct LutOwner { pl_custom_lut** l; ~LutOwner() { pl_lut_free(l); } } lutOwner{ &lut };
			pl_tex_params targetParams{};
			targetParams.w = width;
			targetParams.h = height;
			targetParams.format = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			targetParams.renderable = true;
			targetParams.host_readable = true;
			pl_tex targetTexture = pl_tex_create(gpu, &targetParams);
			Assert::IsNotNull(targetTexture);
			struct TexOwner { pl_gpu g; pl_tex* t; ~TexOwner() { pl_tex_destroy(g, t); } } texOwner{ gpu, &targetTexture };
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			auto target = [&](bool withLut)
			{
				pl_frame t = TargetLutGpuFixture::MakeRgbFrame(targetTexture);
				t.repr.levels = PL_COLOR_LEVELS_FULL;
				t.color.transfer = PL_COLOR_TRC_BT_1886;
				t.lut = withLut ? lut : nullptr;
				t.lut_type = withLut ? PL_LUT_NORMALIZED : PL_LUT_UNKNOWN;
				return t;
			};
			auto download = [&]()
			{
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> output(static_cast<size_t>(width) * height);
				pl_tex_transfer_params d{};
				d.tex = targetTexture;
				d.ptr = output.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				return output;
			};
			DejudderRendered::Cache cache;
			{
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planesA.planes.luma;
				image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planesA.planes.chroma;
				image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame t = target(true);
				std::vector<const pl_hook*> hooks = { cache.Hook() };
				OutputEncodingRepair::Hook repair;
				if (OutputEncodingRepair::Needed(hooks, nullptr, PL_LUT_NORMALIZED))
				{
					repair.SetTarget(t.repr);
					hooks.insert(hooks.begin(), repair.Get());
				}
				pl_render_params p = params;
				p.hooks = hooks.data();
				p.num_hooks = static_cast<int>(hooks.size());
				cache.Arm(11);
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
			}
			const std::vector<RgbaPixel> own = download();
			Assert::IsNotNull(cache.Texture(11));
			auto extra = [&](bool withLut)
			{
				pl_frame image = DejudderRendered::Frame(cache.Texture(11), cache.Repr(), cache.Color());
				pl_frame t = target(withLut);
				std::vector<const pl_hook*> kept;
				const pl_render_params p = DejudderRendered::OutputOnly(params, {}, kept);
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
				return download();
			};
			const std::vector<RgbaPixel> fixed = extra(false);
			const std::vector<RgbaPixel> twice = extra(true);
			int fixedMax = 0, twiceMax = 0;
			for (size_t i = 0; i < own.size(); ++i)
			{
				auto diff = [](const RgbaPixel& p, const RgbaPixel& q)
				{
					return std::max({ std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b) });
				};
				fixedMax = std::max(fixedMax, diff(fixed[i], own[i]));
				twiceMax = std::max(twiceMax, diff(twice[i], own[i]));
			}
			Assert::IsTrue(fixedMax <= 1, (L"the extra without the target LUT differs from A by " +
				std::to_wstring(fixedMax) + L" codes").c_str());
			Assert::IsTrue(twiceMax >= 8, (L"with the LUT on the extra's target it should apply twice; max diff " +
				std::to_wstring(twiceMax)).c_str());
		}

		TEST_METHOD(DejudderRenderedWarpMatchesTheSourceWarp)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			const DejudderFrame cut = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y, 99); }, chroma);
			DejudderGpuPlanes planesA, planesB, planesCut;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			UploadDejudderFrame(gpu, cut, planesCut);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			DejudderRendered::Cache cache;
			DejudderRendered::Warp rendered(fixture.Log(), gpu);
			std::string report;

			for (int scale : { 1, 2 })
			{
				const int ow = width * scale, oh = height * scale;
				pl_fmt targetFormat = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				pl_tex_params targetParams{};
				targetParams.w = ow;
				targetParams.h = oh;
				targetParams.format = targetFormat;
				targetParams.renderable = true;
				targetParams.host_readable = true;
				pl_tex targetTexture = pl_tex_create(gpu, &targetParams);
				Assert::IsNotNull(targetTexture);
				pl_render_params params = pl_render_fast_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				auto target = [&]()
				{
					pl_frame t = TargetLutGpuFixture::MakeRgbFrame(targetTexture);
					t.repr.levels = PL_COLOR_LEVELS_FULL;
					t.color.transfer = PL_COLOR_TRC_BT_1886;
					return t;
				};
				auto download = [&]()
				{
					pl_gpu_finish(gpu);
					std::vector<RgbaPixel> output(static_cast<size_t>(ow) * oh);
					pl_tex_transfer_params d{};
					d.tex = targetTexture;
					d.ptr = output.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					return output;
				};
				// The renderer's source frame, with the given hooks.
				auto renderSource = [&](const Dejudder::Planes& planes, std::vector<const pl_hook*> hooks)
				{
					pl_frame image{};
					image.num_planes = 2;
					image.planes[0].texture = planes.luma;
					image.planes[0].components = 1;
					image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
					image.planes[1].texture = planes.chroma;
					image.planes[1].components = 2;
					image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
					image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
					image.repr.sys = PL_COLOR_SYSTEM_BT_709;
					image.repr.levels = PL_COLOR_LEVELS_FULL;
					image.color.primaries = PL_COLOR_PRIM_BT_709;
					image.color.transfer = PL_COLOR_TRC_BT_1886;
					pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
					pl_frame t = target();
					// libplacebo 7.360.1's PRE_OUTPUT bug: the renderer adds the
					// repair when a texture-input PRE_OUTPUT hook is present.
					OutputEncodingRepair::Hook repair;
					if (OutputEncodingRepair::Needed(hooks, nullptr, PL_LUT_UNKNOWN))
					{
						repair.SetTarget(t.repr);
						hooks.insert(hooks.begin(), repair.Get());
					}
					pl_render_params p = params;
					p.hooks = hooks.empty() ? nullptr : hooks.data();
					p.num_hooks = static_cast<int>(hooks.size());
					Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
					return download();
				};
				auto renderCached = [&](float shift, uint64_t sequence)
				{
					pl_tex out = nullptr;
					DejudderRendered::Geometry g;
					g.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
					g.targetCrop = { 0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh) };
					Assert::IsTrue(rendered.Run(cache.Texture(sequence), warp.Field(), warp.FrameTrust(),
						warp.GetSettings().minTrustedShare, shift, g, out), ExtendedRangeGpuProbe::ToWide(rendered.Error()).c_str());
					pl_frame image = DejudderRendered::Frame(out, cache.Repr(), cache.Color());
					pl_frame t = target();
					std::vector<const pl_hook*> kept;
					const pl_render_params p = DejudderRendered::OutputOnly(params, {}, kept);
					Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
					return download();
				};
				auto best = [&](const std::vector<RgbaPixel>& moved, const std::vector<RgbaPixel>& own, int& bx, int& by)
				{
					double bestError = 1e30;
					for (int oy = -2; oy <= 4 * scale; ++oy)
						for (int ox = -2; ox <= 7 * scale; ++ox)
						{
							double error = 0.0;
							for (int y = 24 * scale; y < oh - 24 * scale; ++y)
								for (int x = 32 * scale; x < ow - 32 * scale; ++x)
								{
									const RgbaPixel& m = moved[static_cast<size_t>(y) * ow + x];
									const RgbaPixel& o = own[static_cast<size_t>(y - oy) * ow + (x - ox)];
									error += std::abs(m.r - o.r) + std::abs(m.g - o.g) + std::abs(m.b - o.b);
								}
							if (error < bestError) { bestError = error; bx = ox; by = oy; }
						}
				};

				// A with the cache armed: the picture is unchanged by the hook.
				const std::vector<RgbaPixel> own = renderSource(planesA.planes, {});
				cache.Arm(7);
				const std::vector<RgbaPixel> cachedOwn = renderSource(planesA.planes, { cache.Hook() });
				Assert::IsNotNull(cache.Texture(7), L"The cache hook did not keep A's picture");
				Assert::IsNull(cache.Texture(8));
				// The hook makes libplacebo keep A's picture in its 16-bit float
				// intermediate (as the LED measurement hook already does): at
				// most a code of rounding, and the encoding unchanged.
				{
					int maxDiff = 0;
					for (size_t i = 0; i < own.size(); ++i)
						maxDiff = std::max({ maxDiff, std::abs(own[i].r - cachedOwn[i].r), std::abs(own[i].g - cachedOwn[i].g),
							std::abs(own[i].b - cachedOwn[i].b) });
					report += "scale " + std::to_string(scale) + ": cache hook max " + std::to_string(maxDiff) + "; ";
					Assert::IsTrue(maxDiff <= 1, ExtendedRangeGpuProbe::ToWide("The cache hook changed A's picture: " + report).c_str());
				}

				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				// Hold: A exactly.
				const std::vector<RgbaPixel> held = renderCached(0.0f, 7);
				int heldMax = 0;
				for (size_t i = 0; i < own.size(); ++i)
					heldMax = std::max({ heldMax, std::abs(cachedOwn[i].r - held[i].r), std::abs(cachedOwn[i].g - held[i].g),
						std::abs(cachedOwn[i].b - held[i].b) });
				report += "hold max " + std::to_string(heldMax);
				Assert::AreEqual(0, heldMax, ExtendedRangeGpuProbe::ToWide("A zero shift must show A exactly\n" + report).c_str());

				// shift 0.5 by both paths.
				Dejudder::Planes warpedPlanes;
				Assert::IsTrue(warp.Warp(planesA.planes, 0.5f, warpedPlanes));
				const std::vector<RgbaPixel> bySource = renderSource(warpedPlanes, {});
				const std::vector<RgbaPixel> byRendered = renderCached(0.5f, 7);
				int sx = 0, sy = 0, rx = 0, ry = 0;
				best(bySource, own, sx, sy);
				best(byRendered, own, rx, ry);
				double sum = 0.0;
				int maxDiff = 0, count = 0, over2 = 0;
				for (int y = 24 * scale; y < oh - 24 * scale; ++y)
					for (int x = 32 * scale; x < ow - 32 * scale; ++x)
					{
						const RgbaPixel& m = bySource[static_cast<size_t>(y) * ow + x];
						const RgbaPixel& r = byRendered[static_cast<size_t>(y) * ow + x];
						for (int d : { std::abs(m.r - r.r), std::abs(m.g - r.g), std::abs(m.b - r.b) })
						{
							sum += d;
							maxDiff = std::max(maxDiff, d);
							over2 += d > 2 ? 1 : 0;
							++count;
						}
					}
				char line[200];
				std::snprintf(line, sizeof(line), "; moved source (%d,%d) rendered (%d,%d); interior diff mean %.3f max %d, %.2f%% over 2 codes\n",
					sx, sy, rx, ry, sum / count, maxDiff, 100.0 * over2 / count);
				report += line;
				Assert::IsTrue(rx == dx * scale / 2 && ry == dy * scale / 2 && sx == rx && sy == ry,
					ExtendedRangeGpuProbe::ToWide("The rendered extra moved differently\n" + report).c_str());
				Assert::IsTrue(sum / count <= 0.5 && over2 <= count / 100,
					ExtendedRangeGpuProbe::ToWide("The rendered extra differs from the source extra: " + report).c_str());

				// A cut (nothing trusted): A exactly.
				Assert::IsTrue(warp.Estimate(planesA.planes, planesCut.planes));
				const std::vector<RgbaPixel> atCut = renderCached(0.5f, 7);
				for (size_t i = 0; i < own.size(); ++i)
					Assert::IsTrue(cachedOwn[i].r == atCut[i].r && cachedOwn[i].g == atCut[i].g && cachedOwn[i].b == atCut[i].b,
						L"A cut must show A exactly");
				pl_tex_destroy(gpu, &targetTexture);
			}
			// Audit stage10-6d item 2: a fractional destination rectangle.
			// libplacebo renders onto it rounded; the extra uses the same
			// rounding, so a hold is still A exactly (not a resample).
			{
				const auto r = DejudderRendered::Rounded({ 0.0f, 0.0f, 256.0f, 128.0f },
					{ 10.4f, 5.6f, 250.3f, 120.5f }, 256.0f, 128.0f);
				Assert::IsTrue(r.target.x0 == 10.0f && r.target.y0 == 6.0f && r.target.x1 == 250.0f && r.target.y1 == 121.0f);
				Assert::AreEqual(-0.4f * 256.0f / 239.9f, r.source.x0, 1e-4f);
				pl_fmt targetFormat = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				pl_tex_params tp{};
				tp.w = width;
				tp.h = height;
				tp.format = targetFormat;
				tp.renderable = true;
				tp.host_readable = true;
				tp.blit_dst = true;
				pl_tex targetTexture = pl_tex_create(gpu, &tp);
				pl_render_params params = pl_render_fast_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				const pl_rect2df fractional = { 10.4f, 5.6f, 250.3f, 120.5f };
				auto frameTarget = [&](const pl_rect2df& crop)
				{
					pl_frame t = TargetLutGpuFixture::MakeRgbFrame(targetTexture);
					t.color.transfer = PL_COLOR_TRC_BT_1886;
					t.crop = crop;
					return t;
				};
				auto download = [&]()
				{
					pl_gpu_finish(gpu);
					std::vector<RgbaPixel> output(static_cast<size_t>(width) * height);
					pl_tex_transfer_params d{};
					d.tex = targetTexture;
					d.ptr = output.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					return output;
				};
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planesA.planes.luma;
				image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planesA.planes.chroma;
				image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				std::vector<const pl_hook*> hooks = { cache.Hook() };
				OutputEncodingRepair::Hook repair;
				pl_frame t = frameTarget(fractional);
				if (OutputEncodingRepair::Needed(hooks, nullptr, PL_LUT_UNKNOWN))
				{
					repair.SetTarget(t.repr);
					hooks.insert(hooks.begin(), repair.Get());
				}
				pl_render_params p = params;
				p.hooks = hooks.data();
				p.num_hooks = static_cast<int>(hooks.size());
				const float black[] = { 0.0f, 0.0f, 0.0f };
				pl_frame_clear(gpu, &t, black);
				cache.Arm(9);
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
				const std::vector<RgbaPixel> shown = download();
				Assert::IsNotNull(cache.Texture(9));
				Assert::IsTrue(cache.Texture(9)->params.w == 240 && cache.Texture(9)->params.h == 115,
					L"The cache is not libplacebo's rounded rectangle");
				const auto rounded = DejudderRendered::Rounded(image.crop.x1 > 0 ? image.crop :
					pl_rect2df{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) }, fractional,
					static_cast<float>(width), static_cast<float>(height));
				DejudderRendered::Geometry g;
				g.sourceCrop = rounded.source;
				g.targetCrop = rounded.target;
				pl_tex out = nullptr;
				Assert::IsTrue(rendered.Run(cache.Texture(9), warp.Field(), warp.FrameTrust(),
					warp.GetSettings().minTrustedShare, 0.0f, g, out));
				pl_frame held = DejudderRendered::Frame(out, cache.Repr(), cache.Color());
				pl_frame ht = frameTarget(rounded.target);
				std::vector<const pl_hook*> kept;
				const pl_render_params hp = DejudderRendered::OutputOnly(params, {}, kept);
				pl_frame_clear(gpu, &ht, black);
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &held, &ht, &hp));
				const std::vector<RgbaPixel> heldPixels = download();
				int maxDiff = 0;
				for (int y = 6; y < 121; ++y)
					for (int x = 10; x < 250; ++x)
					{
						const RgbaPixel& a1 = shown[static_cast<size_t>(y) * width + x];
						const RgbaPixel& b1 = heldPixels[static_cast<size_t>(y) * width + x];
						maxDiff = std::max({ maxDiff, std::abs(a1.r - b1.r), std::abs(a1.g - b1.g), std::abs(a1.b - b1.b) });
					}
				report += "fractional crop: hold max " + std::to_string(maxDiff) + "\n";
				Assert::AreEqual(0, maxDiff, ExtendedRangeGpuProbe::ToWide("A fractional crop resampled the extra: " + report).c_str());
				pl_tex_destroy(gpu, &targetTexture);
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			// 7.360.1 logs its PRE_OUTPUT encoding bug for the cache hook (a
			// texture-input PRE_OUTPUT hook); VP's repair is in A's chain.
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
			Assert::IsTrue(cache.Copies() == 3 && cache.Failed() == 0);
			cache.Release();
			Assert::IsNull(cache.Texture(9), L"Release must drop the cached picture");
		}

		TEST_METHOD(DejudderGlobalPriorFindsAPanBeyondTheCoarseRange)
		{
			// D1: a pan of (140, -20) source px per frame is beyond the coarse
			// search (about +-126 px with the refinements): without the prior the
			// blocks miss it; with it they are within half a pixel, and both warps
			// move the picture by shift x the pan.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			static constexpr int dx = 140, dy = -20;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			// Blocks whose content stays in the picture in both frames.
			std::wstring misses;
			auto countRight = [&](const DejudderField& field)
			{
				int total = 0, right = 0;
				misses.clear();
				for (int by = 3; by < field.blocksY - 3; ++by)
					for (int bx = 1; bx * 16 + 16 + dx < width - 16; ++bx)
					{
						++total;
						const bool ok = field.trusted(bx, by) && std::fabs(field.vx(bx, by) - dx) <= 0.5f &&
							std::fabs(field.vy(bx, by) - dy) <= 0.5f;
						right += ok ? 1 : 0;
						if (!ok && misses.size() < 600)
							misses += L" (" + std::to_wstring(bx) + L"," + std::to_wstring(by) + L": " +
								std::to_wstring(field.vx(bx, by)) + L"," + std::to_wstring(field.vy(bx, by)) +
								(field.trusted(bx, by) ? L" t)" : L" u)");
					}
				return std::make_pair(right, total);
			};
			Dejudder::Settings settings = DejudderTestSettings();
			Dejudder::MotionWarp without(fixture.Log(), gpu, settings);
			Assert::IsTrue(without.Estimate(planesA.planes, planesB.planes));
			const auto off = countRight(DownloadField(gpu, without));
			settings.globalPrior = true;
			Dejudder::MotionWarp with(fixture.Log(), gpu, settings);
			Assert::IsTrue(with.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(with.LastError()).c_str());
			const auto on = countRight(DownloadField(gpu, with));
			const DejudderField global = DownloadField(gpu, with, with.GlobalVector());
			const std::wstring message = L"right blocks without the prior " + std::to_wstring(off.first) + L", with " +
				std::to_wstring(on.first) + L" of " + std::to_wstring(on.second) + L"; global (1/8 texels) " +
				std::to_wstring(global.texels[0]) + L", " + std::to_wstring(global.texels[1]) + L" ratio " +
				std::to_wstring(global.texels[2]) + L"; misses:" + misses + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(off.first * 100 < off.second * 20, (L"the pan should be beyond the plain search: " + message).c_str());
			Assert::IsTrue(on.first * 100 >= on.second * 95, message.c_str());
			Assert::IsTrue(std::fabs(global.texels[0] - dx / 8.0f) <= 0.5f && std::fabs(global.texels[1] - dy / 8.0f) <= 0.5f &&
				global.texels[2] < 0.9f, message.c_str());

			// shift 0.5: (70, -10), both paths, on blocks whose content stays in the
			// picture (from x 352 on it leaves at 140 px per frame: those hold).
			const BothPaths warped = WarpBothPaths(fixture, with, a, planesA.planes, 0.5f);
			auto expected = [](int x, int y) { return DejudderTexture(x - 70, y + 10); };
			const double sourceOk = MatchingPercent(warped.source, width, 96, 40, 340, height - 40, expected, kSourceTolerance);
			const double renderedOk = MatchingPercent(warped.rendered, width, 96, 40, 340, height - 40, expected, kRenderedTolerance);
			Assert::IsTrue(sourceOk >= 90.0 && renderedOk >= 90.0, (L"warped by (70, -10): source " +
				std::to_wstring(sourceOk) + L"%, rendered " + std::to_wstring(renderedOk) + L"%; " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderMotionScaledStrength)
		{
			// D2: a static scene gives scale 0; a pan of 8 px with lo 0 / hi 16
			// gives 0.5, and both warps then move the picture by shift x 0.5 x 8.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame pan = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - 8, y); },
				[](int x, int y, int c) { return DejudderTexture((x - 4) * 2, y, 7 + c); });
			DejudderGpuPlanes planesA, planesPan;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, pan, planesPan);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.motionScaleLo = 0.0f;
			settings.motionScaleHi = 16.0f;
			settings.strengthRate = 1.0f;   // the rate limit has its own test
			{
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesA.planes));
				const auto trust = DownloadFrameTrust(gpu, warp);
				Assert::IsTrue(trust[0] > 0.9f && trust[1] == 0.0f,
					(L"static: share " + std::to_wstring(trust[0]) + L", scale " + std::to_wstring(trust[1])).c_str());
			}
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesPan.planes));
			const auto trust = DownloadFrameTrust(gpu, warp);
			Assert::IsTrue(std::fabs(trust[2] - 8.0f) < 0.3f && std::fabs(trust[1] - 0.5f) < 0.02f,
				(L"pan 8: mean |v| " + std::to_wstring(trust[2]) + L", scale " + std::to_wstring(trust[1])).c_str());
			// shift = 0.5 / scale: the warps apply the GPU's scale, so the picture
			// lands exactly 4 px on (8 px if the scale were ignored).
			const BothPaths warped = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f / trust[1]);
			auto expected = [](int x, int y) { return DejudderTexture(x - 4, y); };
			const double sourceOk = MatchingPercent(warped.source, width, 24, 24, width - 24, height - 24, expected, kSourceTolerance);
			const double renderedOk = MatchingPercent(warped.rendered, width, 24, 24, width - 24, height - 24, expected, kRenderedTolerance);
			auto unscaled = [](int x, int y) { return DejudderTexture(x - 8, y); };
			const double sourceUnscaled = MatchingPercent(warped.source, width, 24, 24, width - 24, height - 24, unscaled, kSourceTolerance);
			Assert::IsTrue(sourceOk >= 90.0 && renderedOk >= 90.0 && sourceUnscaled < 50.0, (L"shift x scale 0.5 = 4 px: source " +
				std::to_wstring(sourceOk) + L"%, rendered " + std::to_wstring(renderedOk) + L"% (unscaled 8 px " +
				std::to_wstring(sourceUnscaled) + L"%)").c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderTrustAdaptiveStrengthAndRateLimit)
		{
			// D4: half the picture cut (salt-99) gives a share between the hold
			// and full strength, so the scale is between 0 and 1 and both warps
			// move the trusted half by shift x scale; a full cut holds (share
			// below the minimum, scale 0). Then the rate limit: static -> pan
			// with D2 ramps by 0.25 per source frame.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame half = MakeDejudderFrame(width, height,
				[](int x, int y) { return x < 128 ? DejudderTexture(x, y, 99) : DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return x < 64 ? DejudderTexture(x * 2, y, 99 + c) : DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			const DejudderFrame cut = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y, 99); }, chroma);
			DejudderGpuPlanes planesA, planesHalf, planesCut;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, half, planesHalf);
			UploadDejudderFrame(gpu, cut, planesCut);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.trustScaleFull = 0.9f;
			settings.strengthRate = 1.0f;
			{
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesHalf.planes));
				const auto trust = DownloadFrameTrust(gpu, warp);
				const float expectedScale = (trust[0] - 0.25f) / (0.9f - 0.25f);
				Assert::IsTrue(trust[0] > 0.3f && trust[0] < 0.6f && std::fabs(trust[1] - expectedScale) < 0.01f &&
					trust[1] > 0.05f && trust[1] < 0.95f,
					(L"half cut: share " + std::to_wstring(trust[0]) + L", scale " + std::to_wstring(trust[1])).c_str());
				// The trusted (right) half moves by shift x scale x (12, 6): with
				// shift = 1 / scale it lands at the full motion.
				const BothPaths warped = WarpBothPaths(fixture, warp, a, planesA.planes, 1.0f / trust[1]);
				auto expected = [](int x, int y) { return DejudderTexture(x - dx, y - dy); };
				const double sourceOk = MatchingPercent(warped.source, width, 160, 24, width - 24, height - 24, expected, kSourceTolerance);
				const double renderedOk = MatchingPercent(warped.rendered, width, 160, 24, width - 24, height - 24, expected, kRenderedTolerance);
				Assert::IsTrue(sourceOk >= 85.0 && renderedOk >= 85.0, (L"half cut, trusted half: source " +
					std::to_wstring(sourceOk) + L"%, rendered " + std::to_wstring(renderedOk) + L"%").c_str());
				Assert::IsTrue(warp.Estimate(planesA.planes, planesCut.planes));
				const auto cutTrust = DownloadFrameTrust(gpu, warp);
				Assert::IsTrue(cutTrust[0] < 0.25f && cutTrust[1] == 0.0f,
					(L"full cut: share " + std::to_wstring(cutTrust[0]) + L", scale " + std::to_wstring(cutTrust[1])).c_str());
				const BothPaths held = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
				auto own = [](int x, int y) { return DejudderTexture(x, y); };
				Assert::IsTrue(MatchingPercent(held.source, width, 0, 0, width, height, own, kSourceTolerance) == 100.0 &&
					MatchingPercent(held.rendered, width, 0, 0, width, height, own, kRenderedTolerance) == 100.0,
					L"a full cut holds A in both paths");
			}
			// The rate limit (D2 on: static, then three pan frames).
			Dejudder::Settings ramp = DejudderTestSettings();
			ramp.motionScaleLo = 0.25f;
			ramp.motionScaleHi = 1.5f;
			ramp.strengthRate = 0.25f;
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesB;
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, ramp);
			std::wstring sequence;
			float scales[4] = {};
			for (int frame = 0; frame < 4; ++frame)
			{
				Assert::IsTrue(warp.Estimate(planesA.planes, frame == 0 ? planesA.planes : planesB.planes));
				scales[frame] = DownloadFrameTrust(gpu, warp)[1];
				sequence += std::to_wstring(scales[frame]) + L" ";
			}
			Assert::IsTrue(scales[0] == 0.0f && std::fabs(scales[1] - 0.25f) < 0.01f && std::fabs(scales[2] - 0.5f) < 0.01f &&
				std::fabs(scales[3] - 0.75f) < 0.01f, (L"static then pan: " + sequence).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderVectorSmoothing)
		{
			// D3 on constructed fields over real planes (A: a pan of (12, 6)
			// with a 48x48 object that is gone in B). A trusted isolated outlier
			// cannot be made from real frames (the search window's margin breaks
			// its neighbours' trust too), so the raw field is built from the
			// real one and passed to Smooth():
			// - block (2, 2) set to a trusted (20, 6): replaced by the median (12, 6);
			// - block (12, 5) set untrusted; its content matches at (12, 6): promoted;
			// - the object's centre block (7, 3) untrusted with its 8 neighbours
			//   set to a trusted (12, 6): its own content (the object) does not
			//   match there (planner answer 4), so it stays held.
			// Then the whole Estimate with D3 on keeps the pan, and both warps
			// move the former outlier with the pan.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, ox = 96, oy = 32;
			auto inA = [](int x, int y) { return x >= ox && x < ox + 48 && y >= oy && y < oy + 48; };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inA(x, y) ? DejudderTexture(x, y, 55) : DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::Settings settings = DejudderTestSettings();
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
			const DejudderField real = DownloadField(gpu, warp);
			Assert::IsTrue(real.trusted(2, 2) && real.trusted(12, 5) && !real.trusted(7, 3),
				L"the real field: (2, 2) and (12, 5) trusted, the object's centre held");

			std::vector<float> texels = real.texels;
			auto set = [&](int bx, int by, float vx, float vy, float trust)
			{
				float* t = &texels[(by * real.blocksX + bx) * 4];
				t[0] = vx; t[1] = vy; t[2] = trust;
			};
			set(2, 2, 20.0f, 6.0f, 1.0f);
			set(12, 5, 0.0f, 0.0f, 0.0f);
			for (int by = 2; by <= 4; ++by)
				for (int bx = 6; bx <= 8; ++bx)
					if (bx != 7 || by != 3)
						set(bx, by, static_cast<float>(dx), static_cast<float>(dy), 1.0f);
			std::vector<uint16_t> halfs(texels.size());
			for (size_t i = 0; i < texels.size(); ++i)
				halfs[i] = FloatToHalf(texels[i]);
			pl_tex_params rp{};
			rp.w = real.blocksX;
			rp.h = real.blocksY;
			rp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16, PL_FMT_CAP_SAMPLEABLE);
			rp.sampleable = true;
			rp.initial_data = halfs.data();
			pl_tex raw = rp.format ? pl_tex_create(gpu, &rp) : nullptr;
			Assert::IsNotNull(raw);
			const bool smoothed = warp.Smooth(raw);
			pl_tex_destroy(gpu, &raw);
			Assert::IsTrue(smoothed, ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			const DejudderField after = DownloadField(gpu, warp);
			auto describe = [&](int bx, int by)
			{
				return L"(" + std::to_wstring(after.vx(bx, by)) + L", " + std::to_wstring(after.vy(bx, by)) +
					(after.trusted(bx, by) ? L", trusted)" : L", held)");
			};
			const std::wstring message = L"outlier " + describe(2, 2) + L"; promoted " + describe(12, 5) +
				L"; object centre " + describe(7, 3);
			auto isPan = [&](int bx, int by)
			{
				return after.trusted(bx, by) && std::fabs(after.vx(bx, by) - dx) < 0.6f && std::fabs(after.vy(bx, by) - dy) < 0.6f;
			};
			Assert::IsTrue(isPan(2, 2), (L"the outlier takes the median: " + message).c_str());
			Assert::IsTrue(isPan(12, 5), (L"a matching block is promoted: " + message).c_str());
			Assert::IsFalse(after.trusted(7, 3), (L"new content is not promoted: " + message).c_str());

			// Both warps move the former outlier block with the pan (shift 0.5).
			const BothPaths warped = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
			auto expected = [](int x, int y) { return DejudderTexture(x - 6, y - 3); };
			const double sourceOk = MatchingPercent(warped.source, width, 40, 38, 52, 50, expected, kSourceTolerance);
			const double renderedOk = MatchingPercent(warped.rendered, width, 40, 38, 52, 50, expected, kRenderedTolerance);
			Assert::IsTrue(sourceOk >= 90.0 && renderedOk >= 90.0, (L"the former outlier warped with the pan: source " +
				std::to_wstring(sourceOk) + L"%, rendered " + std::to_wstring(renderedOk) + L"%").c_str());

			// The whole Estimate with D3 on: the pan unchanged away from the object.
			settings.vectorSmooth = true;
			Dejudder::MotionWarp smooth(fixture.Log(), gpu, settings);
			Assert::IsTrue(smooth.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(smooth.LastError()).c_str());
			const DejudderField full = DownloadField(gpu, smooth);
			int interior = 0, right = 0;
			for (int by = 2; by < full.blocksY - 2; ++by)
				for (int bx = 2; bx < full.blocksX - 2; ++bx)
				{
					if (bx >= 5 && bx <= 9 && by >= 1 && by <= 5)
						continue;   // the object and its margin
					++interior;
					right += (full.trusted(bx, by) && std::fabs(full.vx(bx, by) - dx) < 0.6f && std::fabs(full.vy(bx, by) - dy) < 0.6f) ? 1 : 0;
				}
			Assert::IsTrue(right * 100 >= interior * 95, (L"the pan with D3 on: " + std::to_wstring(right) + L" of " +
				std::to_wstring(interior)).c_str());
			Assert::IsFalse(full.trusted(7, 3), L"the object's centre stays held with D3 on");
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderOcclusionFillFromB)
		{
			// D5 (source warp): an object in A that is gone in B leaves its
			// place as new content in B. With the fill on, the held place shows
			// B there; off (and in the rendered path, not wired yet) it holds A.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, ox = 96, oy = 32;
			auto inA = [](int x, int y) { return x >= ox && x < ox + 48 && y >= oy && y < oy + 48; };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inA(x, y) ? DejudderTexture(x, y, 55) : DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			auto inner = [&](const std::vector<float>& image, bool fromB)
			{
				// The object's interior, away from its edges (blocks of 16).
				return MatchingPercent(image, width, ox + 16, oy + 16, ox + 32, oy + 32,
					[&](int x, int y) { return fromB ? b.luma[y * width + x] : a.luma[y * width + x]; },
					kSourceTolerance + 1.0f);
			};
			Dejudder::Settings settings = DejudderTestSettings();
			Dejudder::MotionWarp off(fixture.Log(), gpu, settings);
			Assert::IsTrue(off.Estimate(planesA.planes, planesB.planes));
			const BothPaths held = WarpBothPaths(fixture, off, a, planesA.planes, 0.5f);
			settings.occlusionFill = true;
			Dejudder::MotionWarp on(fixture.Log(), gpu, settings);
			Assert::IsTrue(on.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(on.LastError()).c_str());
			const BothPaths filled = WarpBothPaths(fixture, on, a, planesA.planes, 0.5f);
			const double heldA = inner(held.source, false), filledB = inner(filled.source, true);
			Assert::IsTrue(heldA >= 95.0 && filledB >= 95.0, (L"object place: off holds A " + std::to_wstring(heldA) +
				L"%, on shows B " + std::to_wstring(filledB) + L"%").c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderGlobalTwoFindsTwoMotionsBeyondTheCoarseRange)
		{
			// M2 (owner 2026-10-13 night): the top half pans +140 px and the
			// bottom half -140 px, both beyond the coarse search; D1 alone is not
			// confident (two equal candidates) so neither half is found. With
			// the second candidate both halves are within half a pixel.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			static constexpr int dx = 140;
			auto lumaA = [](int x, int y) { return y < 128 ? DejudderTexture(x, y, 3) : DejudderTexture(x, y, 9); };
			auto lumaB = [](int x, int y) { return y < 128 ? DejudderTexture(x - dx, y, 3) : DejudderTexture(x + dx, y, 9); };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, lumaA, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height, lumaB,
				[](int x, int y, int c) { return DejudderTexture((x - (y < 128 ? dx : -dx) / 2) * 2, y, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			auto count = [&](const DejudderField& field, bool top)
			{
				int total = 0, right = 0;
				const int want = top ? dx : -dx;
				for (int by = top ? 2 : 10; by < (top ? 6 : field.blocksY - 2); ++by)
					for (int bx = 1; bx < field.blocksX - 1; ++bx)
					{
						const int x0 = bx * 16;
						if (top ? x0 + 16 + dx >= width - 16 : x0 - dx < 16) continue;   // leaves the picture
						++total;
						right += field.trusted(bx, by) && std::fabs(field.vx(bx, by) - want) <= 0.5f && std::fabs(field.vy(bx, by)) <= 0.5f ? 1 : 0;
					}
				return std::make_pair(right, total);
			};
			auto run = [&](bool two)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.globalTwo = two;
				settings.minTrustedShare = 0.0f;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const DejudderField field = DownloadField(gpu, warp);
				return std::make_pair(count(field, true), count(field, false));
			};
			const auto one = run(false), two = run(true);
			const std::wstring message = L"right blocks, one candidate: top " + std::to_wstring(one.first.first) + L"/" +
				std::to_wstring(one.first.second) + L", bottom " + std::to_wstring(one.second.first) + L"/" + std::to_wstring(one.second.second) +
				L"; two: top " + std::to_wstring(two.first.first) + L"/" + std::to_wstring(two.first.second) + L", bottom " +
				std::to_wstring(two.second.first) + L"/" + std::to_wstring(two.second.second) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(one.first.first * 100 < one.first.second * 50 || one.second.first * 100 < one.second.second * 50,
				(L"one candidate should miss a half: " + message).c_str());
			Assert::IsTrue(two.first.first * 100 >= two.first.second * 90 && two.second.first * 100 >= two.second.second * 90, message.c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderRegionalHoldWarpsTheMatchedAreaUnlessACut)
		{
			// M3 (planner Q9): a band pans 8 px while the rest of the picture is
			// new noise every frame (hard motion: few blocks match, the picture's
			// brightness distribution unchanged). Off: the whole frame holds. On:
			// the band's blocks warp (FrameTrust().r at least min_share, the
			// band trusted) - and no cut. A real cut (new, darker content) still
			// holds the whole frame with the regional hold on.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 512;
			static constexpr int dx = 8;
			auto noise = [](int x, int y, unsigned seed)
			{
				const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u) ^ (seed * 83492791u);
				return static_cast<uint16_t>(8000 + ((h * 2654435761u) >> 16) % 40000u);
			};
			auto band = [](int y) { return y >= 192 && y < 288; };   // 6 block rows of 16 (under a quarter)
			auto chroma = [](int x, int y, int c) { return static_cast<uint16_t>(32768 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return band(y) ? DejudderTexture(x, y, 5) : noise(x, y, 1); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[&](int x, int y) { return band(y) ? DejudderTexture(x - dx, y, 5) : noise(x, y, 2); }, chroma);
			const DejudderFrame cut = MakeDejudderFrame(width, height,
				[&](int x, int y) { return static_cast<uint16_t>(2000 + (DejudderTexture(x, y, 77) - 8000) / 8); }, chroma);
			DejudderGpuPlanes planesA, planesB, planesCut;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			UploadDejudderFrame(gpu, cut, planesCut);
			struct Result { float r = 0, share = 0; int bandTrusted = 0, bandTotal = 0; };
			auto run = [&](bool regional, const Dejudder::Planes& second)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.regionalHold = regional;
				settings.minTrustedShare = 0.25f;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, second), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				Result result;
				result.r = DownloadFrameTrust(gpu, warp)[0];
				const DejudderField field = DownloadField(gpu, warp);
				int trusted = 0;
				for (int by = 0; by < field.blocksY; ++by)
					for (int bx = 0; bx < field.blocksX; ++bx)
					{
						trusted += field.trusted(bx, by) ? 1 : 0;
						if (by >= 13 && by < 17 && bx >= 1 && bx < field.blocksX - 2)
						{
							++result.bandTotal;
							result.bandTrusted += field.trusted(bx, by) && std::fabs(field.vx(bx, by) - dx) <= 0.5f ? 1 : 0;
						}
					}
				result.share = static_cast<float>(trusted) / (field.blocksX * field.blocksY);
				return result;
			};
			const Result off = run(false, planesB.planes), on = run(true, planesB.planes), cutOn = run(true, planesCut.planes);
			const std::wstring message = L"off: r " + std::to_wstring(off.r) + L" share " + std::to_wstring(off.share) + L"; on: r " +
				std::to_wstring(on.r) + L" share " + std::to_wstring(on.share) + L" band " + std::to_wstring(on.bandTrusted) + L"/" +
				std::to_wstring(on.bandTotal) + L"; cut (on): r " + std::to_wstring(cutOn.r) + L" share " + std::to_wstring(cutOn.share) +
				L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(off.share < 0.25f && off.r < 0.25f, (L"hard motion: few blocks match, off holds the frame: " + message).c_str());
			Assert::IsTrue(on.r >= 0.25f, (L"on: not a cut, the holds stay per block: " + message).c_str());
			Assert::IsTrue(on.bandTrusted * 100 >= on.bandTotal * 80, (L"on: the band is trusted: " + message).c_str());
			Assert::IsTrue(cutOn.r < 0.25f, (L"a real cut still holds the whole frame: " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderTextureToleranceTrustsGrainyTextureWithoutFlatFalseMatches)
		{
			// M1: a pan of 6 px with fresh grain on a detailed texture: the
			// correct match differs by more than the fixed tolerance, so few
			// blocks are trusted; with the texture allowance most are, with the
			// right vector. Off is today exactly (the same field).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			static constexpr int dx = 6;
			auto grain = [](int x, int y, unsigned seed)
			{
				const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u) ^ (seed * 83492791u);
				return static_cast<int>(((h * 2654435761u) >> 16) % 8000u) - 4000;
			};
			auto chroma = [](int x, int y, int c) { return static_cast<uint16_t>(32768 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return static_cast<uint16_t>(DejudderTexture(x, y, 11) + grain(x, y, 1)); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[&](int x, int y) { return static_cast<uint16_t>(DejudderTexture(x - dx, y, 11) + grain(x, y, 2)); }, chroma);
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			auto run = [&](float textureTolerance)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.matchTolerance = 3.0f;
				settings.textureTolerance = textureTolerance;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const DejudderField field = DownloadField(gpu, warp);
				int trusted = 0, right = 0, total = 0;
				for (int by = 1; by < field.blocksY - 1; ++by)
					for (int bx = 1; bx < field.blocksX - 2; ++bx)
					{
						++total;
						trusted += field.trusted(bx, by) ? 1 : 0;
						right += field.trusted(bx, by) && std::fabs(field.vx(bx, by) - dx) <= 0.5f && std::fabs(field.vy(bx, by)) <= 0.5f ? 1 : 0;
					}
				return std::make_tuple(trusted, right, total);
			};
			const auto off = run(0.0f), on = run(0.5f);
			const std::wstring message = L"trusted / right / blocks: off " + std::to_wstring(std::get<0>(off)) + L"/" +
				std::to_wstring(std::get<1>(off)) + L"/" + std::to_wstring(std::get<2>(off)) + L", allowance 0.5 " +
				std::to_wstring(std::get<0>(on)) + L"/" + std::to_wstring(std::get<1>(on)) + L"/" + std::to_wstring(std::get<2>(on)) +
				L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(std::get<0>(off) * 100 < std::get<2>(off) * 50, (L"grain: the fixed tolerance trusts few: " + message).c_str());
			Assert::IsTrue(std::get<1>(on) * 100 >= std::get<2>(on) * 80, (L"the allowance trusts most, rightly: " + message).c_str());
			Assert::IsTrue(std::get<0>(on) - std::get<1>(on) <= std::get<2>(on) / 50, (L"and no wrong trusted vectors: " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderFadeMatchFollowsASmoothFadeAndHoldsAFlash)
		{
			// M4 (planner Q11): a pan of 6 px while the picture darkens 6% a
			// frame. Without the fade match the blocks do not match (held).
			// With it, from the second pair of the fade on, the blocks match with
			// the right vector and the strength is the floor; a one-frame flash
			// (a bright frame between two normal ones) is not a smooth fade and
			// holds.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			auto frame = [&](int shift, double level)
			{
				return MakeDejudderFrame(width, height, [=](int x, int y) {
					return static_cast<uint16_t>(4096 + (DejudderTexture(x - shift, y, 13) - 4096) * level); },
					[](int x, int y, int c) { return static_cast<uint16_t>(32768 + c); });
			};
			std::vector<DejudderFrame> fade;
			for (int f = 0; f < 4; ++f) fade.push_back(frame(6 * f, 1.0 - 0.06 * f));
			std::vector<std::unique_ptr<DejudderGpuPlanes>> up;
			for (const DejudderFrame& f : fade) { up.push_back(std::make_unique<DejudderGpuPlanes>()); UploadDejudderFrame(gpu, f, *up.back()); }
			struct Result { int right = 0, total = 0; float scale = 0; };
			auto run = [&](bool fadeMatch, const std::vector<Dejudder::Planes>& seq)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.fadeMatch = fadeMatch;
				settings.fadeFloor = 0.25f;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				for (size_t k = 0; k + 1 < seq.size(); ++k)
				{
					const bool estimated = warp.Estimate(seq[k], seq[k + 1]);
					Assert::IsTrue(estimated, (L"estimate " + std::to_wstring(k) + (fadeMatch ? L" (fade match): " : L": ") +
						ExtendedRangeGpuProbe::ToWide(warp.LastError()) + L"; log: " + std::wstring(log.begin(), log.end())).c_str());
				}
				const DejudderField field = DownloadField(gpu, warp);
				Result r;
				for (int by = 1; by < field.blocksY - 1; ++by)
					for (int bx = 1; bx < field.blocksX - 2; ++bx)
					{
						++r.total;
						r.right += field.trusted(bx, by) && std::fabs(field.vx(bx, by) - 6.0f) <= 0.5f ? 1 : 0;
					}
				r.scale = DownloadFrameTrust(gpu, warp)[1];
				return r;
			};
			std::vector<Dejudder::Planes> seq;
			for (auto& p : up) seq.push_back(p->planes);
			const Result off = run(false, seq), on = run(true, seq);
			// A flash: normal, bright (x1.4), normal again; the last pair is
			// bright -> normal after normal -> bright: not the same direction.
			const DejudderFrame bright = frame(6, 1.4), normal2 = frame(12, 1.0);
			DejudderGpuPlanes pBright, pNormal2;
			UploadDejudderFrame(gpu, bright, pBright);
			UploadDejudderFrame(gpu, normal2, pNormal2);
			const Result flash = run(true, { up[0]->planes, pBright.planes, pNormal2.planes });
			const std::wstring message = L"right / blocks (strength): off " + std::to_wstring(off.right) + L"/" + std::to_wstring(off.total) +
				L" (" + std::to_wstring(off.scale) + L"), fade match " + std::to_wstring(on.right) + L"/" + std::to_wstring(on.total) + L" (" +
				std::to_wstring(on.scale) + L"), flash " + std::to_wstring(flash.right) + L"/" + std::to_wstring(flash.total) + L" (" +
				std::to_wstring(flash.scale) + L"); log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(off.right * 100 < off.total * 30, (L"a fade: the plain match fails: " + message).c_str());
			Assert::IsTrue(on.right * 100 >= on.total * 80, (L"the fade match follows the pan: " + message).c_str());
			Assert::IsTrue(std::fabs(on.scale - 0.25f) < 0.02f, (L"at the floor strength: " + message).c_str());
			Assert::IsTrue(flash.right * 100 < flash.total * 30, (L"a flash holds: " + message).c_str());
			// A pan whose mean brightness changes as brighter content enters (a
			// vertical gradient panned 8 px a frame) is not a fade: full strength.
			std::vector<std::unique_ptr<DejudderGpuPlanes>> pan;
			std::vector<Dejudder::Planes> panSeq;
			for (int f = 0; f < 5; ++f)
			{
				const DejudderFrame frame = MakeDejudderFrame(width, height, [f](int x, int y) {
					return static_cast<uint16_t>(std::min(65535, 2000 + (DejudderTexture(x, y - 8 * f, 13) - 8000) / 2 + (y - 8 * f + 64) * 150)); },
					[](int x, int y, int c) { return static_cast<uint16_t>(32768 + c); });
				pan.push_back(std::make_unique<DejudderGpuPlanes>());
				UploadDejudderFrame(gpu, frame, *pan.back());
				panSeq.push_back(pan.back()->planes);
			}
			const Result panned = run(true, panSeq);
			Assert::IsTrue(panned.scale > 0.99f, (L"a pan that changes the mean is not a fade (strength " + std::to_wstring(panned.scale) + L")").c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderRolloffStartsOneFrameEarlierWithTheEstimateAhead)
		{
			// Planner Q7: a pan that accelerates past roll_lo (6 px, then 30 px).
			// After the fast pair is estimated ahead, the slow pair's extras use
			// the lower strength (the roll-off starts one frame earlier), at most
			// one rate step below the slow pair's; without R-A the snapshot is
			// returned as is. Both stay rate-limited.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			auto at = [&](int shift)
			{
				return MakeDejudderFrame(width, height, [shift](int x, int y) { return DejudderTexture(x - shift, y); },
					[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); });
			};
			const DejudderFrame f0 = at(0), f1 = at(6), f2 = at(36);
			DejudderGpuPlanes p0, p1, p2;
			UploadDejudderFrame(gpu, f0, p0);
			UploadDejudderFrame(gpu, f1, p1);
			UploadDejudderFrame(gpu, f2, p2);
			auto read = [&](pl_tex t)
			{
				std::array<float, 4> out{};
				std::vector<uint16_t> half(4);
				pl_tex_transfer_params d{};
				d.tex = t;
				d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (int k = 0; k < 4; ++k)
				{
					const uint16_t h = half[k];
					const int e = (h >> 10) & 31, m = h & 1023;
					out[k] = (e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(1.0f + m / 1024.0f, e - 15)) * ((h & 0x8000) ? -1.0f : 1.0f);
				}
				return out;
			};
			auto run = [&](bool rolloff)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.motionRolloff = rolloff;
				settings.rollLo = 8.0f;
				settings.rollHi = 24.0f;
				settings.rollFloor = 0.25f;
				settings.strengthRate = 0.25f;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				for (int i = 0; i < 4; ++i)   // the slow pair settles
					Assert::IsTrue(warp.Estimate(p0.planes, p1.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				Assert::IsTrue(warp.SnapshotForWarp());
				const float slow = read(warp.SnapFrameTrust())[1];
				Assert::IsTrue(warp.Estimate(p1.planes, p2.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const float next = read(warp.FrameTrust())[1];
				const float ahead = read(warp.LookAheadTrust())[1];
				return std::make_tuple(slow, next, ahead);
			};
			const auto on = run(true), off = run(false);
			const std::wstring message = L"R-A on: slow " + std::to_wstring(std::get<0>(on)) + L", next " + std::to_wstring(std::get<1>(on)) +
				L", ahead " + std::to_wstring(std::get<2>(on)) + L"; off: slow " + std::to_wstring(std::get<0>(off)) + L", ahead " +
				std::to_wstring(std::get<2>(off)) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(std::get<0>(on) > 0.95f, (L"the slow pan: full strength: " + message).c_str());
			Assert::IsTrue(std::get<2>(on) < std::get<0>(on) - 0.1f, (L"the roll-off starts with the estimate ahead: " + message).c_str());
			Assert::IsTrue(std::get<0>(on) - std::get<2>(on) <= 0.25f + 1e-3f, (L"one rate step at most: " + message).c_str());
			Assert::IsTrue(std::fabs(std::get<2>(off) - std::get<0>(off)) < 1e-3f, (L"R-A off: the snapshot as is: " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderVerifyWithdrawsTrustFromBlocksWhosePixelsDoNotFollow)
		{
			// M5: a pan of 8 px with small static patches (8x8 source px, +30
			// codes) in some blocks: at a loose tolerance (8) those blocks match
			// on average and are trusted; their patch pixels mispredict, so the
			// pixel check withdraws their trust; blocks without a patch stay.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			static constexpr int dx = 8;
			auto patch = [](int x, int y) { return (x / 16) % 4 == 2 && (y / 16) % 3 == 1 && x % 16 < 8 && y % 16 < 8; };
			auto smooth = [](int x, int y) { return static_cast<int>(DejudderTexture(x, y, 21)) / 2 + 12000; };
			auto chroma = [](int x, int y, int c) { return static_cast<uint16_t>(32768 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return static_cast<uint16_t>(smooth(x, y) + (patch(x, y) ? 10280 : 0)); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[&](int x, int y) { return static_cast<uint16_t>(smooth(x - dx, y) + (patch(x, y) ? 10280 : 0)); }, chroma);
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			auto run = [&](bool verify)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.matchTolerance = 8.0f;
				settings.consistencyPixels = 2.0f;
				settings.minTrustedShare = 0.0f;
				settings.verifyPixels = verify;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				const bool estimated = warp.Estimate(planesA.planes, planesB.planes);
				Assert::IsTrue(estimated, ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const DejudderField field = DownloadField(gpu, warp);
				int patchTrusted = 0, patchTotal = 0, cleanTrusted = 0, cleanTotal = 0;
				for (int by = 1; by < field.blocksY - 1; ++by)
					for (int bx = 1; bx < field.blocksX - 2; ++bx)
					{
						const bool hasPatch = patch(bx * 16, by * 16);
						// Clean: neither a patch nor one landing in it from a neighbour.
						const bool clean = !hasPatch && !patch(bx * 16 + 16, by * 16) && !patch(bx * 16 - 16, by * 16);
						if (hasPatch) { ++patchTotal; patchTrusted += field.trusted(bx, by) ? 1 : 0; }
						if (clean) { ++cleanTotal; cleanTrusted += field.trusted(bx, by) ? 1 : 0; }
					}
				return std::make_tuple(patchTrusted, patchTotal, cleanTrusted, cleanTotal);
			};
			const auto off = run(false), on = run(true);
			const std::wstring message = L"patch blocks trusted: off " + std::to_wstring(std::get<0>(off)) + L"/" + std::to_wstring(std::get<1>(off)) +
				L", verify " + std::to_wstring(std::get<0>(on)) + L"/" + std::to_wstring(std::get<1>(on)) + L"; clean blocks trusted: off " +
				std::to_wstring(std::get<2>(off)) + L"/" + std::to_wstring(std::get<3>(off)) + L", verify " + std::to_wstring(std::get<2>(on)) +
				L"/" + std::to_wstring(std::get<3>(on)) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(std::get<0>(off) * 100 >= std::get<1>(off) * 60, (L"loose tolerance: the patch blocks pass on average: " + message).c_str());
			Assert::IsTrue(std::get<0>(on) * 100 <= std::get<1>(on) * 10, (L"the pixel check withdraws them: " + message).c_str());
			Assert::IsTrue(std::get<2>(on) * 100 >= std::get<3>(on) * 95 && std::get<2>(on) >= std::get<2>(off) - 1,
				(L"clean blocks keep their trust: " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderPixelTrustMovesBothPathsAlike)
		{
			// M6: a pan of (10, 4) where a band of blocks fails the block test
			// (fresh noise in A's band... only in B: the band's match fails, its
			// trusted neighbours carry it). Both warps with pixel trust move
			// the trusted area by s x pan and agree with each other; a cut
			// still holds the whole frame in both.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			static constexpr int dx = 10, dy = 4;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.globalPrior = true;
			settings.pixelTrust = true;
			settings.pixelTrustLo = 3.0f;
			settings.pixelTrustHi = 8.0f;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes) && warp.Estimate(planesA.planes, planesB.planes),
				ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			Assert::IsNotNull(warp.PixelMask(), L"the pixel weights exist");
			const BothPaths warped = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
			auto expected = [](int x, int y) { return DejudderTexture(x - 5, y - 2); };
			const double sourceOk = MatchingPercent(warped.source, width, 48, 32, width - 48, height - 32, expected, kSourceTolerance);
			const double renderedOk = MatchingPercent(warped.rendered, width, 48, 32, width - 48, height - 32, expected, kRenderedTolerance);
			Assert::IsTrue(sourceOk >= 90.0 && renderedOk >= 90.0, (L"pixel trust moves the pan by half: source " +
				std::to_wstring(sourceOk) + L"%, rendered " + std::to_wstring(renderedOk) + L"%; log: " + std::wstring(log.begin(), log.end())).c_str());
			// A cut (B another picture): both hold.
			const DejudderFrame c = MakeDejudderFrame(width, height,
				[](int x, int y) { return static_cast<uint16_t>(2000 + (DejudderTexture(x, y, 99) - 8000) / 6); }, chroma);
			DejudderGpuPlanes planesC;
			UploadDejudderFrame(gpu, c, planesC);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesC.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			const BothPaths held = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
			auto same = [](int x, int y) { return DejudderTexture(x, y); };
			const double sourceHeld = MatchingPercent(held.source, width, 16, 16, width - 16, height - 16, same, kSourceTolerance);
			const double renderedHeld = MatchingPercent(held.rendered, width, 16, 16, width - 16, height - 16, same, kRenderedTolerance);
			Assert::IsTrue(sourceHeld >= 99.0 && renderedHeld >= 99.0, (L"a cut holds in both: source " + std::to_wstring(sourceHeld) +
				L"%, rendered " + std::to_wstring(renderedHeld) + L"%").c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderMotionRolloffKeepsFastMotionsStrobing)
		{
			// Delta 14 R-A: pans at 2, 12 and 30 source px per frame give the
			// strength 0 (below D2's rise), full, and the floor (above the
			// roll-off); off, the fast pan keeps full strength (D2 alone).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			DejudderGpuPlanes planesA;
			UploadDejudderFrame(gpu, a, planesA);
			auto scaleFor = [&](int dx, bool rolloff)
			{
				const DejudderFrame b = MakeDejudderFrame(width, height, [dx](int x, int y) { return DejudderTexture(x - dx, y); },
					[dx](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y, 7 + c); });
				DejudderGpuPlanes planesB;
				UploadDejudderFrame(gpu, b, planesB);
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.motionScaleLo = 3.0f;
				settings.motionScaleHi = 6.0f;
				settings.trustScaleFull = 0.0f;
				settings.motionRolloff = rolloff;
				settings.rollLo = 16.0f;
				settings.rollHi = 26.0f;
				settings.rollFloor = 0.25f;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				// The first frame starts at its own value; more settle within D2's rate.
				for (int i = 0; i < 6; ++i)
					Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const auto trust = DownloadFrameTrust(gpu, warp);
				return std::make_pair(trust[1], trust[2]);
			};
			const auto slow = scaleFor(2, true), medium = scaleFor(12, true), fast = scaleFor(30, true), fastOff = scaleFor(30, false);
			const std::wstring report = L"scale (mean |v|): 2 px " + std::to_wstring(slow.first) + L" (" + std::to_wstring(slow.second) +
				L"), 12 px " + std::to_wstring(medium.first) + L" (" + std::to_wstring(medium.second) + L"), 30 px " +
				std::to_wstring(fast.first) + L" (" + std::to_wstring(fast.second) + L"), 30 px off " + std::to_wstring(fastOff.first);
			Assert::IsTrue(slow.first < 0.02f, (L"a slow pan: below D2's rise; " + report).c_str());
			Assert::IsTrue(medium.first > 0.98f, (L"a medium pan: full; " + report).c_str());
			Assert::IsTrue(std::fabs(fast.first - 0.25f) < 0.03f, (L"a fast pan: the floor; " + report).c_str());
			Assert::IsTrue(fastOff.first > 0.98f, (L"off: D2 alone, full; " + report).c_str());
		}

		TEST_METHOD(DejudderObjectStrengthSplitsTheCameraFromObjects)
		{
			// Delta 14 R-B: a pan of (12, 6) with an object moving (-10, 0)
			// against it. At object_strength 0.5 the background moves by the
			// full s x pan and the object by s x (pan + 0.5 x (object - pan)),
			// in the dither's vectors and in both warps; 1.0 is today exactly.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, ox = 96, oy = 40, ow = 64, oh = 48, odx = -10;
			auto inA = [](int x, int y) { return x >= ox && x < ox + ow && y >= oy && y < oy + oh; };
			auto inB = [](int x, int y) { return x >= ox + odx && x < ox + odx + ow && y >= oy && y < oy + oh; };
			auto object = [](int x, int y) { return DejudderTexture(x, y, 41); };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inA(x, y) ? object(x - ox, y - oy) : DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inB(x, y) ? object(x - ox - odx, y - oy) : DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			struct Run { std::vector<float> vectors; BothPaths paths; float camX = 0, camY = 0; };
			auto run = [&](float objectStrength)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.objectStrength = objectStrength;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes) && warp.Estimate(planesA.planes, planesB.planes),
					ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				Run r;
				// The dither's vectors (output px per block, 1:1), shift 0.5.
				DejudderRendered::VectorField field(fixture.Log(), gpu, true);
				DejudderRendered::Geometry g;
				g.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
				g.targetCrop = g.sourceCrop;
				pl_tex vectors = nullptr;
				Assert::IsTrue(field.Run(warp.Field(), warp.FrameTrust(), warp.GetSettings().minTrustedShare, 0.5f, g, width, height,
					vectors, warp.SubField(), &warp.GetSettings(), warp.Camera()), ExtendedRangeGpuProbe::ToWide(field.Error()).c_str());
				std::vector<uint16_t> half(static_cast<size_t>(vectors->params.w) * vectors->params.h * 4);
				pl_tex_transfer_params d{};
				d.tex = vectors;
				d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (uint16_t h : half) r.vectors.push_back(HalfBits(h));
				uint16_t gv[4] = {};
				d.tex = warp.Camera() ? warp.Camera() : warp.GlobalVector();
				d.ptr = gv;
				const float unit = warp.Camera() ? 1.0f : 8.0f;
				if (pl_tex_download(gpu, &d)) { r.camX = HalfBits(gv[0]) * unit; r.camY = HalfBits(gv[1]) * unit; }
				r.paths = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
				return r;
			};
			const Run split = run(0.5f), today = run(1.0f);
			const int bw = (width + 15) / 16;
			auto vectorAt = [&](const Run& r, int x, int y)
			{
				const size_t i = (static_cast<size_t>(y / 16) * bw + x / 16) * 4;
				return std::make_pair(r.vectors[i], r.vectors[i + 1]);
			};
			const auto bgSplit = vectorAt(split, 24, 104), objSplit = vectorAt(split, ox + 32, oy + 24);
			const auto bgToday = vectorAt(today, 24, 104), objToday = vectorAt(today, ox + 32, oy + 24);
			auto text = [](std::pair<float, float> v) { return L"(" + std::to_wstring(v.first) + L", " + std::to_wstring(v.second) + L")"; };
			const std::wstring report = L"camera (" + std::to_wstring(split.camX) + L", " + std::to_wstring(split.camY) +
				L"); 0.5: background " + text(bgSplit) + L", object " + text(objSplit) + L"; 1.0: background " + text(bgToday) +
				L", object " + text(objToday);
			// Today: the object at s x its own motion; the background at s x pan.
			Assert::IsTrue(std::fabs(objToday.first - 0.5f * odx) < 0.75f && std::fabs(bgToday.first - 0.5f * dx) < 0.75f &&
				std::fabs(bgToday.second - 0.5f * dy) < 0.75f, (L"today; " + report).c_str());
			// Split: the background unchanged, the object half-way to the camera.
			const float expectX = 0.5f * (dx + 0.5f * (odx - dx)), expectY = 0.5f * (dy + 0.5f * (0 - dy));
			Assert::IsTrue(std::fabs(bgSplit.first - bgToday.first) < 0.05f && std::fabs(bgSplit.second - bgToday.second) < 0.05f,
				(L"0.5: the camera smoothed fully; " + report).c_str());
			Assert::IsTrue(std::fabs(objSplit.first - expectX) < 0.75f && std::fabs(objSplit.second - expectY) < 0.75f,
				(L"0.5: the object at s x (pan + 0.5 x (object - pan)); " + report).c_str());
			// Both warps: the object's interior moves differently, the far
			// background not at all; the two paths agree with each other.
			auto region = [&](const std::vector<float>& image, int x0, int y0, int x1, int y1, const std::vector<float>& other)
			{
				double sum = 0.0;
				int n = 0;
				for (int y = y0; y < y1; ++y)
					for (int x = x0; x < x1; ++x)
					{
						sum += std::fabs(image[static_cast<size_t>(y) * width + x] - other[static_cast<size_t>(y) * width + x]);
						++n;
					}
				return sum / n / 256.0;   // 8-bit codes
			};
			for (int path = 0; path < 2; ++path)
			{
				const std::vector<float>& s05 = path == 0 ? split.paths.source : split.paths.rendered;
				const std::vector<float>& s10 = path == 0 ? today.paths.source : today.paths.rendered;
				const wchar_t* name = path == 0 ? L"source warp" : L"rendered warp";
				Assert::IsTrue(region(s05, ox + 16, oy + 12, ox + ow - 16, oy + oh - 12, s10) > 2.0,
					(std::wstring(name) + L": the object's interior moves by the split; " + report).c_str());
				Assert::IsTrue(region(s05, 8, 100, 72, 120, s10) < 0.05, (std::wstring(name) + L": the far background as today; " + report).c_str());
			}
			Assert::IsTrue(region(split.paths.source, ox + 16, oy + 12, ox + ow - 16, oy + oh - 12, split.paths.rendered) < 2.0,
				(L"the two paths agree on the split object; " + report).c_str());
		}

		TEST_METHOD(DejudderEstimateAheadFallsBackForEachReason)
		{
			// Estimate ahead (owner 2026-10-13): every reason not to estimate
			// ahead falls back to the estimate before the frame (Consume false),
			// and none of them runs the matcher early.
			using Schedule = Dejudder::AheadSchedule;
			using Reason = Schedule::Reason;
			int estimates = 0, snapshots = 0, uploads = 0;
			Schedule::Steps steps;
			steps.snapshot = [&]() { ++snapshots; return true; };
			steps.uploadAfter = [&]() { ++uploads; return true; };
			steps.estimate = [&](uint64_t, uint64_t) { ++estimates; return true; };
			auto ok = []() { Schedule::Conditions c; c.source = 10; c.next = 11; c.after = 12; c.estimated = 10; return c; };
			struct Case { const wchar_t* name; Schedule::Conditions c; Reason expected; };
			std::vector<Case> cases;
			{ auto c = ok(); c.enabled = false; cases.push_back({ L"off", c, Reason::Off }); }
			{ auto c = ok(); c.renderedWarp = false; cases.push_back({ L"source warp", c, Reason::SourceWarp }); }
			{ auto c = ok(); c.active = false; cases.push_back({ L"not active", c, Reason::NotActive }); }
			{ auto c = ok(); c.next = 0; cases.push_back({ L"no next", c, Reason::NoNext }); }
			{ auto c = ok(); c.after = 0; cases.push_back({ L"frame after next not supplied", c, Reason::NoAfter }); }
			{ auto c = ok(); c.after = 11; cases.push_back({ L"after = next", c, Reason::AfterNotNew }); }
			{ auto c = ok(); c.estimated = 9; cases.push_back({ L"pair not estimated", c, Reason::NotEstimated }); }
			{ auto c = ok(); c.layoutMatches = false; cases.push_back({ L"layout", c, Reason::Layout }); }
			for (const Case& k : cases)
			{
				Schedule schedule;
				Assert::IsFalse(schedule.TryAhead(k.c, steps), k.name);
				Assert::IsTrue(schedule.LastReason() == k.expected, k.name);
				Assert::IsFalse(schedule.Consume(11, 12), k.name);   // B's frame: estimated now
				Assert::AreEqual(1ull, schedule.Sequential(), k.name);
			}
			Assert::AreEqual(0, estimates + snapshots + uploads, L"no reason to skip ran any step");
			// Failed steps: a failed copy leaves the extras on the matcher's
			// own field; a failed upload or estimate keeps the copy in use and
			// estimates at B's frame.
			{
				Schedule schedule;
				Schedule::Steps failing = steps;
				failing.snapshot = []() { return false; };
				Assert::IsFalse(schedule.TryAhead(ok(), failing));
				Assert::IsTrue(schedule.LastReason() == Reason::SnapshotFailed);
				Assert::IsFalse(schedule.Consume(11, 12));
			}
			for (int which = 0; which < 2; ++which)
			{
				Schedule schedule;
				Schedule::Steps failing = steps;
				if (which == 0) failing.uploadAfter = []() { return false; };
				else failing.estimate = [](uint64_t, uint64_t) { return false; };
				Assert::IsTrue(schedule.TryAhead(ok(), failing), L"the copy stays in use");
				Assert::IsTrue(schedule.LastReason() == (which == 0 ? Reason::UploadFailed : Reason::EstimateFailed));
				Assert::IsFalse(schedule.Consume(11, 12), L"nothing estimated ahead: estimated now");
			}
			// The path that works: estimated ahead, then taken at B's frame once.
			{
				Schedule schedule;
				estimates = 0;
				Assert::IsTrue(schedule.TryAhead(ok(), steps));
				Assert::IsTrue(schedule.LastReason() == Reason::Ahead);
				Assert::AreEqual(1, estimates);
				Assert::IsTrue(schedule.Consume(11, 12), L"B's frame takes the estimate made ahead");
				Assert::IsFalse(schedule.Consume(11, 12), L"only once");
				Assert::AreEqual(1ull, schedule.Hits());
			}
			// Estimated ahead for a C that did not come next: re-estimated.
			{
				Schedule schedule;
				Assert::IsTrue(schedule.TryAhead(ok(), steps));
				Assert::IsFalse(schedule.Consume(11, 13), L"another next frame: estimated now");
				Assert::AreEqual(1ull, schedule.Misses());
			}
		}

		TEST_METHOD(DejudderEstimateAheadMatchesTheSequentialPath)
		{
			// Estimate ahead (owner 2026-10-13), driven by the renderer's own
			// schedule over a seven-frame sequence with changing motion. The
			// frame after next is withheld at frame 2's extras (the fallback is
			// forced) and replaced by another frame at frame 4's (estimated ahead
			// for a pair that does not come). Every frame's path is recorded and
			// checked; up to the replaced frame every output (field, trust,
			// sub-field, the dither's mask, the grain) equals the sequential
			// estimate byte for byte, and the extras' copy equals the outputs they
			// replace.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128, frames = 7;
			static const int panX[frames] = { 0, 6, 13, 18, 22, 29, 33 };
			static const int panY[frames] = { 0, 2, 3, 6, 9, 10, 12 };
			std::vector<DejudderGpuPlanes> planes(frames);
			for (int f = 0; f < frames; ++f)
			{
				const int px = panX[f], py = panY[f];
				const DejudderFrame frame = MakeDejudderFrame(width, height,
					[px, py](int x, int y) { return DejudderTexture(x - px, y - py); },
					[px, py](int x, int y, int c) { return DejudderTexture((x - px / 2) * 2, y - py, 7 + c); });
				UploadDejudderFrame(gpu, frame, planes[f]);
			}
			Dejudder::Settings settings = DejudderTestSettings();
			settings.estSplit = true;
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> half;
				if (!tex) return half;
				half.resize(static_cast<size_t>(tex->params.w) * tex->params.h * 4);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"not host-readable");
				return half;
			};
			struct Outputs { std::vector<uint16_t> field, trust, sub, mask, grain; };
			auto outputs = [&](Dejudder::MotionWarp& w, int slot)
			{
				return Outputs{ download(w.Field()), download(w.FrameTrust()), download(w.SubField()),
					download(w.Occlusion(slot)), download(w.GrainError()) };
			};
			auto same = [](const Outputs& a, const Outputs& b)
			{
				return a.field == b.field && a.trust == b.trust && a.sub == b.sub && a.mask == b.mask && a.grain == b.grain;
			};
			// The pair (n, n+1) writes its mask into slot n % 2 on both paths.
			auto estimate = [&](Dejudder::MotionWarp& w, int a, int b)
			{
				return w.Estimate(planes[a].planes, planes[b].planes, a % 2, true);
			};
			// Sequential: every frame estimated before its render.
			Dejudder::MotionWarp sequential(fixture.Log(), gpu, settings);
			std::vector<Outputs> expected;
			for (int n = 0; n + 1 < frames; ++n)
			{
				Assert::IsTrue(estimate(sequential, n, n + 1), ExtendedRangeGpuProbe::ToWide(sequential.LastError()).c_str());
				expected.push_back(outputs(sequential, n % 2));
			}
			// Estimate ahead through the schedule. Frame ids are the frame
			// indices + 1 (0 = none); frame 99 is the replacement.
			Dejudder::MotionWarp ahead(fixture.Log(), gpu, settings);
			Dejudder::AheadSchedule schedule;
			std::wstring paths;
			int estimated = -1;
			int replacedAt = -1;
			for (int n = 0; n + 1 < frames; ++n)
			{
				// Frame n: its estimate, or the one made ahead.
				const uint64_t idA = n + 1, idB = n + 2;
				if (schedule.Consume(idA, idB))
					paths += L"ahead ";
				else
				{
					Assert::IsTrue(estimate(ahead, n, n + 1), ExtendedRangeGpuProbe::ToWide(ahead.LastError()).c_str());
					paths += L"before ";
				}
				estimated = n;
				const Outputs got = outputs(ahead, n % 2);
				if (replacedAt < 0 || n < replacedAt + 1)
					Assert::IsTrue(same(got, expected[n]),
						(L"frame " + std::to_wstring(n) + L" differs from the sequential estimate; paths: " + paths).c_str());
				// Frame n's first extra: the next pair, ahead when C is supplied.
				if (n + 2 >= frames)
					continue;
				Dejudder::AheadSchedule::Conditions c;
				c.source = idA;
				c.next = idB;
				c.after = n == 2 ? 0 : (n == 4 ? 99 : static_cast<uint64_t>(n + 3));
				c.estimated = static_cast<uint64_t>(estimated + 1);
				Dejudder::AheadSchedule::Steps steps;
				steps.snapshot = [&]() { return ahead.SnapshotForWarp(); };
				steps.uploadAfter = []() { return true; };
				steps.estimate = [&](uint64_t b, uint64_t after)
				{
					const int bi = static_cast<int>(b) - 1;
					const int ci = after == 99 ? 0 : static_cast<int>(after) - 1;   // the replacement: another picture
					return ahead.Estimate(planes[bi].planes, planes[ci].planes, bi % 2, true);
				};
				const bool copy = schedule.TryAhead(c, steps);
				if (n == 4)
					replacedAt = n;
				Assert::IsTrue(copy == (n != 2), (L"frame " + std::to_wstring(n) + L": the copy is used exactly when C is supplied").c_str());
				if (copy)
				{
					// The extras read the copy: frame n's own outputs.
					Assert::IsTrue(download(ahead.SnapField()) == got.field && download(ahead.SnapFrameTrust()) == got.trust &&
						download(ahead.SnapSubField()) == got.sub,
						(L"frame " + std::to_wstring(n) + L": the extras' copy is the frame's own field").c_str());
				}
			}
			// 0 before (the start), 1 2 ahead, 3 before (C withheld at 2), 4
			// ahead, 5 before (C replaced at 4: estimated again).
			Assert::AreEqual(std::wstring(L"before ahead ahead before ahead before "), paths, paths.c_str());
			Assert::AreEqual(3ull, schedule.Hits());
			Assert::AreEqual(3ull, schedule.Sequential());
			Assert::AreEqual(1ull, schedule.Misses());
			Assert::AreEqual(1ull, schedule.Count(Dejudder::AheadSchedule::Reason::NoAfter));
		}

		TEST_METHOD(DejudderSnapshotKeepsTheWarpInputsWhileTheNextPairIsEstimated)
		{
			// Estimate ahead (owner 2026-10-13): the copy of (A, B)'s field,
			// trust and sub-field is exact, and survives the estimate of (B, C)
			// (a different pan) that overwrites the matcher's own.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x - 12, y - 6); },
				[](int x, int y, int c) { return DejudderTexture((x - 6) * 2, y - 6, 7 + c); });
			const DejudderFrame c = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x - 4, y - 10); },
				[](int x, int y, int c) { return DejudderTexture((x - 2) * 2, y - 10, 7 + c); });
			DejudderGpuPlanes planesA, planesB, planesC;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			UploadDejudderFrame(gpu, c, planesC);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.estSplit = true;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			auto download = [&](pl_tex tex)
			{
				Assert::IsNotNull(tex);
				std::vector<uint16_t> half(static_cast<size_t>(tex->params.w) * tex->params.h * 4);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"not host-readable");
				return half;
			};
			// Twice, so the hysteresis lets the pan's blocks warp.
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes) && warp.Estimate(planesA.planes, planesB.planes),
				ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			const std::vector<uint16_t> field = download(warp.Field()), trust = download(warp.FrameTrust()),
				sub = download(warp.SubField());
			Assert::IsTrue(warp.SnapshotForWarp(), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			Assert::IsTrue(download(warp.SnapField()) == field && download(warp.SnapFrameTrust()) == trust &&
				download(warp.SnapSubField()) == sub, L"the copy is exact");
			Assert::IsTrue(warp.Estimate(planesB.planes, planesC.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			Assert::IsTrue(download(warp.Field()) != field, L"the next pair's field differs (a different pan)");
			Assert::IsTrue(download(warp.SnapField()) == field && download(warp.SnapFrameTrust()) == trust &&
				download(warp.SnapSubField()) == sub, L"the copy survives the next estimate");
		}

		// ---- Delta 9, synthesis-lite (docs/VP-delta-8-9-10.md).

		TEST_METHOD(HoldFeatherSoftensTheHeldBoundary)
		{
			// H1: an object in A gone in B leaves held blocks inside a pan.
			// With a 4 px feather, every pixel of a warped block is
			// A + smoothstep(distance to a held neighbour block) x (the
			// unfeathered warp - A), in both warp paths; off is the hard edge.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, ox = 96, oy = 32;
			auto inA = [](int x, int y) { return x >= ox && x < ox + 48 && y >= oy && y < oy + 48; };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inA(x, y) ? DejudderTexture(x, y, 55) : DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			auto run = [&](float feather, DejudderField& field)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.holdFeatherPx = feather;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				field = DownloadField(gpu, warp);
				return WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
			};
			DejudderField field, fieldOn;
			const BothPaths hard = run(0.0f, field);
			const BothPaths soft = run(4.0f, fieldOn);
			Assert::IsTrue(field.texels == fieldOn.texels, L"the feather does not change the field");
			int held = 0;
			for (int by = 0; by < field.blocksY; ++by)
				for (int bx = 0; bx < field.blocksX; ++bx)
					held += field.trusted(bx, by) ? 0 : 1;
			Assert::IsTrue(held > 0, L"some blocks should be held");
			auto clampd = [](double v, double lo, double hi) { return std::min(std::max(v, lo), hi); };
			auto smooth = [&](double e, double x) { const double t = clampd(x / e, 0.0, 1.0); return t * t * (3.0 - 2.0 * t); };
			double worstSource = 0.0, worstRendered = 0.0;
			int banded = 0;
			for (int y = 8; y < height - 8; ++y)
				for (int x = 8; x < width - 8; ++x)
				{
					const int bx = x / 16, by = y / 16;
					if (!field.trusted(bx, by))
						continue;
					double d = 1e9;
					for (int ny = -1; ny <= 1; ++ny)
						for (int nx = -1; nx <= 1; ++nx)
						{
							const int cx = bx + nx, cy = by + ny;
							if ((nx == 0 && ny == 0) || cx < 0 || cy < 0 || cx >= field.blocksX || cy >= field.blocksY ||
								field.trusted(cx, cy))
								continue;
							const double px = x + 0.5, py = y + 0.5;
							const double qx = clampd(px, cx * 16.0, cx * 16.0 + 16.0), qy = clampd(py, cy * 16.0, cy * 16.0 + 16.0);
							d = std::min(d, std::hypot(px - qx, py - qy));
						}
					const double w = smooth(4.0, d);
					if (w < 1.0) ++banded;
					const size_t i = static_cast<size_t>(y) * width + x;
					const double av = a.luma[i];
					worstSource = std::max(worstSource, std::fabs(soft.source[i] - (av + w * (hard.source[i] - av))));
					worstRendered = std::max(worstRendered, std::fabs(soft.rendered[i] - (av + w * (hard.rendered[i] - av))));
				}
			const std::wstring message = L"pixels in the band " + std::to_wstring(banded) + L"; worst deviation from the feather: source " +
				std::to_wstring(worstSource) + L", rendered " + std::to_wstring(worstRendered) + L" (16-bit codes); log: " +
				std::wstring(log.begin(), log.end());
			Assert::IsTrue(banded > 50, message.c_str());
			Assert::IsTrue(worstSource <= 2.0 && worstRendered <= 96.0, message.c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(HoldHysteresisNeedsTwoFrames)
		{
			// H2 (2 frames): block (5, 3) is matchable in P and not in Q (its
			// landing in B replaced). P, P: it warps. Q, P, Q, P: it stays
			// warping. Q, Q: it holds after the second. P then a cut: every
			// block holds at once.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, bx = 5, by = 3;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			auto landing = [](int x, int y)
			{
				return x >= bx * 16 + dx - 4 && x < bx * 16 + dx + 20 && y >= by * 16 + dy - 4 && y < by * 16 + dy + 20;
			};
			const DejudderFrame p = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			const DejudderFrame q = MakeDejudderFrame(width, height,
				[&](int x, int y) { return landing(x, y) ? DejudderTexture(x, y, 33) : DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			const DejudderFrame cut = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y, 99); }, chroma);
			DejudderGpuPlanes pa, pp, pq, pc;
			UploadDejudderFrame(gpu, a, pa);
			UploadDejudderFrame(gpu, p, pp);
			UploadDejudderFrame(gpu, q, pq);
			UploadDejudderFrame(gpu, cut, pc);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.holdHysteresis = 2;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			std::wstring sequence;
			auto step = [&](const Dejudder::Planes& b, const wchar_t* name)
			{
				Assert::IsTrue(warp.Estimate(pa.planes, b), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const DejudderField f = DownloadField(gpu, warp);
				int trustedOthers = 0;
				for (int y = 2; y < f.blocksY - 2; ++y)
					for (int x = 2; x < f.blocksX - 3; ++x)
						if ((x != bx || y != by) && std::abs(x - bx) > 1 && std::abs(y - by) > 1)
							trustedOthers += f.trusted(x, y) ? 1 : 0;
				sequence += std::wstring(name) + (f.trusted(bx, by) ? L"=warp " : L"=hold ");
				return std::make_pair(f.trusted(bx, by), trustedOthers);
			};
			const bool first = step(pp.planes, L"P").first;
			const bool second = step(pp.planes, L"P").first;
			bool alternating = true;
			for (int i = 0; i < 2; ++i)
			{
				alternating = step(pq.planes, L"Q").first && alternating;
				alternating = step(pp.planes, L"P").first && alternating;
			}
			const bool q1 = step(pq.planes, L"Q").first;
			const bool q2 = step(pq.planes, L"Q").first;
			const int othersBefore = step(pp.planes, L"P").second;
			const auto afterCut = step(pc.planes, L"cut");
			const std::wstring message = sequence + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(!first && second, (L"warps after two trusted frames: " + message).c_str());
			Assert::IsTrue(alternating, (L"an alternating block keeps warping: " + message).c_str());
			Assert::IsTrue(q1 && !q2, (L"holds after two untrusted frames: " + message).c_str());
			Assert::IsTrue(othersBefore > 0 && afterCut.second == 0 && !afterCut.first, (L"a cut holds at once: " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(BlockTaperWarpsMarginalBlocksAtARampedStrength)
		{
			// H3: block (5, 3)'s landing in B carries noise. With the tolerance
			// at 0.8 x its SAD and the taper's end at 1.2 x, it is held without
			// the taper and warps at half strength with it (the ramp's middle):
			// at shift 1 it moves by (6, 3), not (12, 6), in both warp paths.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, bx = 5, by = 3;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			DejudderFrame b = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			for (int y = by * 16 - 4 + dy; y < by * 16 + 20 + dy; ++y)
				for (int x = bx * 16 - 4 + dx; x < bx * 16 + 20 + dx; ++x)
					b.luma[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(
						b.luma[static_cast<size_t>(y) * width + x] + NoiseCode(x, y, 5, 1500));
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			float sad = 0.0f;
			{
				Dejudder::MotionWarp probe(fixture.Log(), gpu, DejudderTestSettings());
				Assert::IsTrue(probe.Estimate(planesA.planes, planesB.planes));
				const DejudderField f = DownloadField(gpu, probe, probe.Forward());
				sad = f.texels[(by * f.blocksX + bx) * 4 + 2];
			}
			Assert::IsTrue(sad > 0.5f, (L"the noise should cost the block's match: SAD " + std::to_wstring(sad)).c_str());
			auto run = [&](bool taper, bool& trusted)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.matchTolerance = 0.8f * sad;
				settings.blockTaper = taper;
				settings.taperSad = 1.2f * sad;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				trusted = DownloadField(gpu, warp).trusted(bx, by);
				return WarpBothPaths(fixture, warp, a, planesA.planes, 1.0f);
			};
			bool heldTrusted = true, taperTrusted = false;
			const BothPaths held = run(false, heldTrusted);
			const BothPaths tapered = run(true, taperTrusted);
			auto inBlock = [&](const std::vector<float>& image, int sx, int sy, float tolerance)
			{
				return MatchingPercent(image, width, bx * 16 + 3, by * 16 + 3, bx * 16 + 13, by * 16 + 13,
					[&](int x, int y) { return a.luma[static_cast<size_t>(y - sy) * width + x - sx]; }, tolerance);
			};
			const double heldSource = inBlock(held.source, 0, 0, kSourceTolerance), heldRendered = inBlock(held.rendered, 0, 0, kRenderedTolerance);
			const double halfSource = inBlock(tapered.source, 6, 3, kSourceTolerance), halfRendered = inBlock(tapered.rendered, 6, 3, kRenderedTolerance);
			const std::wstring message = L"SAD " + std::to_wstring(sad) + L"; off: trusted " + std::to_wstring(heldTrusted) +
				L", held " + std::to_wstring(heldSource) + L"% / " + std::to_wstring(heldRendered) + L"%; on: trusted " +
				std::to_wstring(taperTrusted) + L", moved (6, 3) " + std::to_wstring(halfSource) + L"% / " +
				std::to_wstring(halfRendered) + L"%; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(!heldTrusted && heldSource >= 95.0 && heldRendered >= 95.0, (L"off: held; " + message).c_str());
			Assert::IsTrue(taperTrusted && halfSource >= 90.0 && halfRendered >= 90.0, (L"on: half strength; " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderGrainErrorSeparatesNoiseFromTexture)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			std::string log;
			fixture.CaptureLog(&log);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			std::string report;
			for (int fade = 0; fade <= 2; fade += 2)
			{
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, GrainFrame(width, height, 0, 8, 4, 0), planesA);
				UploadDejudderFrame(gpu, GrainFrame(width, height, 1, 8, 4, fade), planesB);
				Assert::IsNull(warp.GrainError(), L"no grain error before an estimate asks for it");
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes, -1, true),
					std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
				const DejudderField field = DownloadField(gpu, warp);
				const DejudderField grain = DownloadField(gpu, warp, warp.GrainError());
				double clean = 0.0, noisy = 0.0;
				int nClean = 0, nNoisy = 0, untrusted = 0;
				for (int by = 1; by < field.blocksY - 1; ++by)
					for (int bx = 1; bx < field.blocksX - 1; ++bx)
					{
						if (bx == field.blocksX / 2 - 1 || bx == field.blocksX / 2)
							continue;   // the boundary blocks see both
						if (!field.trusted(bx, by)) { ++untrusted; continue; }
						const float e = grain.texels[(by * grain.blocksX + bx) * 4];
						if (bx < field.blocksX / 2) { clean += e; ++nClean; }
						else { noisy += e; ++nNoisy; }
					}
				Assert::IsTrue(nClean > 10 && nNoisy > 10, L"too few trusted blocks");
				clean /= nClean;
				noisy /= nNoisy;
				char line[200];
				std::snprintf(line, sizeof(line), "fade %d: clean %.3f noisy %.3f (expected ~%.2f) untrusted %d\n",
					fade, clean, noisy, 0.564 * 3.0, untrusted);
				report += line;
				Assert::IsTrue(clean < 0.35 && noisy > 1.2 && noisy < 2.2, ExtendedRangeGpuProbe::ToWide(report).c_str());
				// Without asking, the next estimate drops it.
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				Assert::IsNull(warp.GrainError());
			}
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderRenderedWarpKeepsGrainRms)
		{
			// The de-judder's rendered warp (Catmull-Rom at the output resolution; no switch):
			// grain rendered at 2x keeps 0.95x or more at fractional output shifts (audit:
			// 0.96-0.98x). The source warp (dejudder_source_warp_filter): catmull keeps more than
			// the hardware's bilinear at a fractional shift; bilinear is the never-switched
			// matcher's output byte for byte.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 128, height = 64, ow = width * 2, oh = height * 2;
			// The rendered picture: white grain at the source resolution, upscaled 2x (Catmull-Rom).
			std::mt19937 rng(51u);
			const GrainModel::Image g = GrainModel::Grain(width, height, 2.0, false, rng);
			GrainModel::Image up(ow, oh);
			for (int y = 0; y < oh; ++y)
				for (int x = 0; x < ow; ++x)
					up.at(x, y) = GrainModel::Catmull(g, (x + 0.5) / 2.0, (y + 0.5) / 2.0);
			std::vector<uint16_t> rgba(static_cast<size_t>(ow) * oh * 4);
			for (size_t i = 0; i < up.v.size(); ++i)
			{
				const uint16_t v = static_cast<uint16_t>(std::lround(30000.0 + kGrainCode16 * up.v[i]));
				rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = v;
				rgba[i * 4 + 3] = 65535;
			}
			pl_tex_params cp{};
			cp.w = ow; cp.h = oh;
			cp.format = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			cp.sampleable = true;
			cp.initial_data = rgba.data();
			pl_tex cached = pl_tex_create(gpu, &cp);
			Assert::IsNotNull(cached);
			const double vx = 12.0, vy = 6.0;   // source px per frame
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[&](int, int) { return std::array<float, 4>{ static_cast<float>(vx), static_cast<float>(vy), 1.0f, 0.0f }; }, field);
			DejudderRendered::Warp rendered(fixture.Log(), gpu, true);
			DejudderRendered::Geometry geometry;
			geometry.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
			geometry.targetCrop = { 0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh) };
			const double source = GrainModel::Rms(up, 24);
			for (float shift : { 0.15f, 0.3f, 0.55f })
			{
				pl_tex out = nullptr;
				Assert::IsTrue(rendered.Run(cached, field.field, field.trust, 0.25f, shift, geometry, out),
					ExtendedRangeGpuProbe::ToWide(rendered.Error()).c_str());
				std::vector<float> px(static_cast<size_t>(out->params.w) * out->params.h * out->params.format->num_components);
				pl_tex_transfer_params d{}; d.tex = out; d.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				const int nc = out->params.format->num_components;
				GrainModel::Image w(ow, oh);
				for (size_t i = 0; i < w.v.size(); ++i) w.v[i] = (px[i * nc] * 65535.0 - 30000.0) / kGrainCode16;
				const double r = GrainModel::Rms(w, 24) / source;
				char text[120];
				snprintf(text, sizeof(text), "rendered warp shift %.2f (output px %.2f, %.2f): grain %.3f", shift, shift * vx * 2, shift * vy * 2, r);
				Logger::WriteMessage(text);
				Assert::IsTrue(r >= 0.95 && r < 1.05, std::wstring(text, text + strlen(text)).c_str());
			}
			pl_tex_destroy(gpu, &cached);

			// The source warp: two matchers on the same pair (B = A moved by (12, 6), the same grain).
			const GrainFrames gf = MakeGrainFrames(256, 128, 1, 0.0, 0.0, 3.0, true, 52u);
			const DejudderFrame& a = gf.frames[0];
			const DejudderFrame b = MakeDejudderFrame(256, 128,
				[&](int x, int y) { return a.luma[static_cast<size_t>((std::max)(0, y - 6)) * 256 + (std::max)(0, x - 12)]; },
				[&](int x, int y, int c) { return a.chroma[(static_cast<size_t>((std::max)(0, y - 6)) * 128 + (std::max)(0, x - 6)) * 2 + c]; });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::Settings bilinearSettings = DejudderTestSettings();
			Dejudder::Settings catmullSettings = DejudderTestSettings();
			catmullSettings.sourceWarpCatmull = true;
			bilinearSettings.sourceWarpCatmull = false;
			Assert::IsTrue(Dejudder::Settings().sourceWarpCatmull, L"the object's default is the faithful Catmull-Rom (integration 2026-10-17)");
			Dejudder::MotionWarp never(fixture.Log(), gpu, DejudderTestSettings());
			Dejudder::MotionWarp bilinear(fixture.Log(), gpu, bilinearSettings);
			Dejudder::MotionWarp catmull(fixture.Log(), gpu, catmullSettings);
			for (Dejudder::MotionWarp* m : { &never, &bilinear, &catmull })
				Assert::IsTrue(m->Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(m->LastError()).c_str());
			Dejudder::Planes outNever, outBilinear, outCatmull;
			Assert::IsTrue(never.Warp(planesA.planes, 0.3f, outNever));
			Assert::IsTrue(bilinear.Warp(planesA.planes, 0.3f, outBilinear));
			Assert::IsTrue(catmull.Warp(planesA.planes, 0.3f, outCatmull));
			const std::vector<uint16_t> lumaNever = DownloadPlane(gpu, outNever.luma);
			Assert::IsTrue(DownloadPlane(gpu, outBilinear.luma) == lumaNever, L"bilinear: the established warp byte for byte");
			Assert::IsTrue(DownloadPlane(gpu, outBilinear.chroma) == DownloadPlane(gpu, outNever.chroma), L"bilinear chroma byte for byte");
			// The warped picture minus the picture at (3.6, 1.8): its grain.
			auto grainOf = [&](const std::vector<uint16_t>& luma) {
				GrainModel::Image w(256, 128);
				for (int y = 0; y < 128; ++y)
					for (int x = 0; x < 256; ++x)
						w.at(x, y) = (luma[static_cast<size_t>(y) * 256 + x] - GrainTexture16(x + 0.5 - 3.6, y + 0.5 - 1.8)) / kGrainCode16;
				return w;
			};
			const double src = GrainModel::Rms(gf.lumaGrain[0], 32);
			const double rBil = GrainModel::Rms(grainOf(lumaNever), 32) / src;
			const double rCat = GrainModel::Rms(grainOf(DownloadPlane(gpu, outCatmull.luma)), 32) / src;
			char text[160];
			snprintf(text, sizeof(text), "source warp at (3.6, 1.8), soft grain: catmull %.3f, bilinear %.3f", rCat, rBil);
			Logger::WriteMessage(text);
			const std::wstring wtext(text, text + strlen(text));
			Assert::IsTrue(rCat > rBil + 0.05, wtext.c_str());
			Assert::IsTrue(rCat >= 0.9 && rCat < 1.05, wtext.c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}
	};
}
