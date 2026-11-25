// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link K4 on the libplacebo WARP device (VP_TEST_GPU=hardware for a real GPU): SetCameraGuidance(nullptr) gives the
// same matcher field as no call at all (the refusal path is byte-identical to today), and the 16 B guidance counts round trip
// through the counts ring without blocking.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceCountsRing.h>
#include <VP-V/modules/motion/estimation/global_model/CameraGuidanceGpu.h>

#include <chrono>
#include <memory>
#include <thread>

namespace VideoProcessorTest
{
	TEST_CLASS(GuidanceNullIdentityWarpTests)
	{
		static constexpr int kW = 512, kH = 256;

		static void Frames(pl_gpu gpu, DejudderGpuPlanes& pa, DejudderGpuPlanes& pb)
		{
			const DejudderFrame a = MakeDejudderFrame(kW, kH, [](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(kW, kH, [](int x, int y) { return DejudderTexture(x - 12, y - 6); },
				[](int x, int y, int c) { return DejudderTexture((x - 6) * 2, y - 6, 7 + c); });
			UploadDejudderFrame(gpu, a, pa);
			UploadDejudderFrame(gpu, b, pb);
		}

		static DejudderField Run(TargetLutGpuFixture& fixture, bool callSetter)
		{
			pl_gpu gpu = fixture.Gpu();
			DejudderGpuPlanes a, b;
			Frames(gpu, a, b);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"first estimate");
			if (callSetter)
				warp.SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame>());
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"second estimate");
			return DownloadField(gpu, warp);
		}

	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(NullGuidanceIsTheSameFieldAsNoCall)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(NullGuidanceIsTheSameFieldAsNoCall)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const DejudderField none = Run(fixture, false);
			const DejudderField withNull = Run(fixture, true);
			Assert::IsTrue(none.texels == withNull.texels, L"SetCameraGuidance(nullptr) changed the field");
		}

		// F2 (M2 condition 1): every Estimate clears the "seeded" record, for the matcher and its lowRes inner, even an Estimate
		// that fails before the inner runs; the seed is never fed here (optical flow off, on any device), so after any Estimate
		// LastSeed must be false. A stale record is planted with the test setter first.
		BEGIN_TEST_METHOD_ATTRIBUTE(EveryEstimateClearsAStaleSeedRecord)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(EveryEstimateClearsAStaleSeedRecord)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			for (int tall = 0; tall < 2; ++tall)
			{
				// tall: 1536 lines halve the search (lowRes): the seed would be the inner's.
				const int w = tall ? 128 : kW, h = tall ? 1536 : kH;
				const DejudderFrame fa = MakeDejudderFrame(w, h, [](int x, int y) { return DejudderTexture(x, y); },
					[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
				const DejudderFrame fb = MakeDejudderFrame(w, h, [](int x, int y) { return DejudderTexture(x - 12, y - 6); },
					[](int x, int y, int c) { return DejudderTexture((x - 6) * 2, y - 6, 7 + c); });
				const DejudderFrame other = MakeDejudderFrame(w, h / 2, [](int x, int y) { return DejudderTexture(x, y); },
					[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
				DejudderGpuPlanes a, b, otherSize;
				UploadDejudderFrame(gpu, fa, a);
				UploadDejudderFrame(gpu, fb, b);
				UploadDejudderFrame(gpu, other, otherSize);
				Dejudder::Settings settings = DejudderTestSettings();
				settings.opticalFlow = false;
				settings.lowRes = true;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Dejudder::MotionWarp::SeedState seed;
				Assert::IsFalse(warp.LastSeed(seed), L"before any Estimate");
				Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"first estimate");
				Assert::AreEqual(tall ? 1 : 0, warp.SearchHalvings());
				Assert::IsFalse(warp.LastSeed(seed), L"optical flow off: never seeded");
				warp.SetSeedFedForTest(true);
				Assert::IsTrue(warp.LastSeed(seed), L"the planted record is seen (the setter works)");
				Assert::AreEqual(tall ? 1 : 0, seed.halvings);
				Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"second estimate");
				Assert::IsFalse(warp.LastSeed(seed), L"a later unseeded estimate clears it");
				warp.SetSeedFedForTest(true);
				Assert::IsFalse(warp.Estimate(a.planes, otherSize.planes), L"A and B differ in size: the estimate fails early");
				Assert::IsFalse(warp.LastSeed(seed), L"a failed estimate clears it too (the inner's as well)");
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(CountsRingRoundTripsSixteenBytesWithoutBlocking)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CountsRingRoundTripsSixteenBytesWithoutBlocking)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(fmt);
			pl_tex_params tp{};
			tp.w = 1; tp.h = 1; tp.format = fmt; tp.sampleable = true; tp.host_writable = true; tp.host_readable = true;
			pl_tex tex = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(tex);
			const float want[4] = { 17.0f, 3.0f, -1.0f, -1.0f };
			pl_tex_transfer_params up{};
			up.tex = tex;
			up.ptr = const_cast<float*>(want);
			Assert::IsTrue(pl_tex_upload(gpu, &up));
			{
				CameraLink::GuidanceCountsRing ring(gpu, 3, fixture.Log());
				Assert::IsTrue(ring.Request(41, tex));
				pl_gpu_finish(gpu);   // test only: make sure the data is complete before judging the values
				std::vector<CameraLink::IGuidanceCountsSource::Done> done;
				for (int i = 0; i < 2000 && done.empty(); ++i)
				{
					const auto t0 = std::chrono::steady_clock::now();
					ring.Poll(done);
					const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
					(void)ms;   // see FieldReadbackRingWarpTests: WARP defers work to Map
					if (done.empty())
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				Assert::AreEqual(size_t(1), done.size());
				Assert::AreEqual(uint64_t(41), done[0].pairSeq);
				for (int i = 0; i < 4; ++i)
					Assert::IsTrue(done[0].v[i] == want[i]);
				// Teardown with a copy in flight.
				Assert::IsTrue(ring.Request(42, tex));
			}
			pl_tex_destroy(gpu, &tex);
		}

		// S3-H5: StillZero must not overwrite CONSISTENCY_BODY's trust=0 mask on bit-identical
		// black letterbox bars outside the active picture rect, and FRAME_TRUST_BODY_A's CAMERA_OUT
		// refinement loop must ignore out-of-picture blocks so (0, 0) bars never drag the refined
		// camera vector toward 0. Pan (4, 0) stays within camera_window (6 px at 256p) of (0, 0),
		// so unguarded bar blocks genuinely enter the refinement window (proven by the same-input
		// pictureArea=false negative control).
		BEGIN_TEST_METHOD_ATTRIBUTE(StillZeroAndCameraOutIgnoreLetterboxBarsOutsidePictureRect)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StillZeroAndCameraOutIgnoreLetterboxBarsOutsidePictureRect)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			constexpr int kBarH = 48;   // 2.39:1-style bars: [0, 48) and [208, 256), 6 of 16 block rows (37.5%)
			constexpr int kPanX = 4;    // exact 2-px half-res pan, within camera_window (6 px) of (0, 0)
			const DejudderFrame fa = MakeDejudderFrame(kW, kH,
				[kBarH](int x, int y) -> uint16_t { return (y < kBarH || y >= kH - kBarH) ? uint16_t(0) : DejudderTexture(x, y); },
				[kBarH](int x, int y, int c) -> uint16_t { return (y < kBarH || y >= kH - kBarH) ? uint16_t(32768) : DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame fb = MakeDejudderFrame(kW, kH,
				[kBarH, kPanX](int x, int y) -> uint16_t { return (y < kBarH || y >= kH - kBarH) ? uint16_t(0) : DejudderTexture(x - kPanX, y); },
				[kBarH, kPanX](int x, int y, int c) -> uint16_t { return (y < kBarH || y >= kH - kBarH) ? uint16_t(32768) : DejudderTexture((x - kPanX / 2) * 2, y, 7 + c); });
			DejudderGpuPlanes a, b;
			UploadDejudderFrame(gpu, fa, a);
			UploadDejudderFrame(gpu, fb, b);

			auto downloadCamera = [gpu](const Dejudder::MotionWarp& warp) -> std::array<float, 4>
			{
				pl_tex cam = warp.Camera();
				Assert::IsNotNull(cam, L"MotionWarp::Camera() must exist when objectStrength < 1 and globalPrior is enabled");
				uint16_t texel[4] = {};
				pl_tex_transfer_params download{};
				download.tex = cam;
				download.ptr = texel;
				Assert::IsTrue(pl_tex_download(gpu, &download), L"Camera texture is not host-readable");
				return { HalfBits(texel[0]), HalfBits(texel[1]), HalfBits(texel[2]), HalfBits(texel[3]) };
			};

			auto verifyProtected = [&](bool explicitRect, const wchar_t* label) -> std::array<float, 4>
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.estStillZero = true;
				settings.pictureArea = true;
				settings.globalPrior = true;
				settings.objectStrength = 0.5f;   // enables CAMERA_OUT refinement
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				if (explicitRect)
					warp.SetPictureArea(0, kBarH, kW, kH - kBarH);
				Assert::IsTrue(warp.Estimate(a.planes, b.planes), label);
				pl_gpu_finish(gpu);
				const DejudderField field = DownloadField(gpu, warp);
				const std::array<float, 4> cam = downloadCamera(warp);

				int barBlocks = 0;
				int activeInterior = 0, activeMatched = 0;
				for (int by = 0; by < field.blocksY; ++by)
				{
					const int cy = by * 16 + 8;
					const bool inBar = cy < kBarH || cy >= kH - kBarH;
					const bool inActiveInterior = cy >= kBarH + 16 && cy < kH - kBarH - 16;
					for (int bx = 0; bx < field.blocksX; ++bx)
					{
						if (inBar)
						{
							++barBlocks;
							Assert::IsFalse(field.trusted(bx, by),
								(std::wstring(label) + L": letterbox bar block was resurrected as trusted by StillZero").c_str());
						}
						else if (inActiveInterior && bx >= 2 && bx + 2 < field.blocksX)
						{
							++activeInterior;
							if (field.trusted(bx, by) &&
								std::fabs(field.vx(bx, by) - static_cast<float>(kPanX)) <= 0.5f &&
								std::fabs(field.vy(bx, by)) <= 0.5f)
								++activeMatched;
						}
					}
				}
				Assert::AreEqual((kW / 16) * (2 * kBarH / 16), barBlocks, L"expected 192 letterbox bar blocks");
				Assert::IsTrue(activeInterior > 0, L"must inspect interior active-picture blocks");
				Assert::AreEqual(activeInterior, activeMatched,
					(std::wstring(label) + L": interior active-picture blocks did not track the (4, 0) pan").c_str());
				Assert::IsTrue(cam[2] > 0.5f, (std::wstring(label) + L": refined Camera() must be reliable").c_str());
				Assert::IsTrue(cam[0] > 3.5f && cam[0] < 4.5f,
					(std::wstring(label) + L": refined Camera().x was dragged away from the (4, 0) pan").c_str());
				Assert::IsTrue(std::fabs(cam[1]) < 0.5f,
					(std::wstring(label) + L": refined Camera().y drifted from 0").c_str());
				return cam;
			};

			// Both explicit SetPictureArea and auto-detected black bars must protect bar blocks and Camera().
			const std::array<float, 4> camExplicit = verifyProtected(true, L"explicit SetPictureArea");
			const std::array<float, 4> camAuto = verifyProtected(false, L"auto-detected pictureArea");

			// Mandatory same-input negative control (Rule 3): run on the EXACT SAME frames with pictureArea = false
			// to prove StillZero genuinely stamps all 192 black-bar blocks (0, 0) trusted and drags Camera().x toward 0.
			Dejudder::Settings unguarded = DejudderTestSettings();
			unguarded.estStillZero = true;
			unguarded.pictureArea = false;
			unguarded.globalPrior = true;
			unguarded.objectStrength = 0.5f;
			Dejudder::MotionWarp negWarp(fixture.Log(), gpu, unguarded);
			Assert::IsTrue(negWarp.Estimate(a.planes, b.planes), L"negative control letterbox estimate");
			pl_gpu_finish(gpu);
			const DejudderField negField = DownloadField(gpu, negWarp);
			const std::array<float, 4> negCam = downloadCamera(negWarp);

			int negBarTrustedZero = 0, negBarBlocks = 0;
			for (int by = 0; by < negField.blocksY; ++by)
			{
				const int cy = by * 16 + 8;
				if (cy >= kBarH && cy < kH - kBarH)
					continue;
				for (int bx = 0; bx < negField.blocksX; ++bx)
				{
					++negBarBlocks;
					if (negField.trusted(bx, by) &&
						std::fabs(negField.vx(bx, by)) < 1e-4f &&
						std::fabs(negField.vy(bx, by)) < 1e-4f)
						++negBarTrustedZero;
				}
			}
			Assert::AreEqual(negBarBlocks, negBarTrustedZero,
				L"negative control: unguarded StillZero must stamp every bit-identical letterbox bar block (0, 0), trust = 1");
			Assert::IsTrue(negCam[2] > 0.5f, L"negative control: Camera() must be reliable");
			Assert::IsTrue(negCam[0] < 3.0f,
				L"negative control: unguarded (0, 0) letterbox bar blocks must drag refined Camera().x toward 0");
			Assert::IsTrue(camExplicit[0] - negCam[0] > 0.8f && camAuto[0] - negCam[0] > 0.8f,
				L"pictureArea guard must eliminate the bar-induced drag on refined Camera().x");
		}

		// S3-H5: when camera guidance skips the matcher across a cut/flash (skipMatcher = true),
		// estStillZero = true must NOT run StillZero and stamp bit-identical active-picture blocks
		// with (0, 0), trust = 1.0 (neither directly nor when a lowRes inner performed the skip).
		BEGIN_TEST_METHOD_ATTRIBUTE(StillZeroDoesNotResurrectIdenticalBlocksOnCutSkippedGuidance)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StillZeroDoesNotResurrectIdenticalBlocksOnCutSkippedGuidance)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			for (int tall = 0; tall < 2; ++tall)
			{
				// tall == 0: full-res search (SearchHalvings == 0); tall == 1: 1536 lines -> lowRes inner performs the skip.
				const int w = tall ? 128 : kW;
				const int h = tall ? 1536 : kH;
				const int halfW = w / 2;
				// Left half [0, halfW) is bit-identical textured picture (inside active picture, not black bars!);
				// right half [halfW, w) cuts to an unrelated texture.
				const DejudderFrame fa = MakeDejudderFrame(w, h,
					[](int x, int y) { return DejudderTexture(x, y); },
					[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
				const DejudderFrame fb = MakeDejudderFrame(w, h,
					[halfW](int x, int y) { return x < halfW ? DejudderTexture(x, y) : DejudderTexture(x, y, 99); },
					[halfW](int x, int y, int c) { return x * 2 < halfW ? DejudderTexture(x * 2, y, 7 + c) : DejudderTexture(x * 2, y, 107 + c); });
				DejudderGpuPlanes a, b;
				UploadDejudderFrame(gpu, fa, a);
				UploadDejudderFrame(gpu, fb, b);

				Dejudder::Settings settings = DejudderTestSettings();
				settings.estStillZero = true;
				settings.pictureArea = true;
				settings.lowRes = (tall != 0);
				settings.lowResAll = false;   // outer runs StillZero after inner's lowRes search skip

				// Protected run: cut/flash guidance with skipMatcher = true.
				auto cutGuidance = std::make_shared<CameraModel::GuidanceFrame>();
				cutGuidance->active = true;
				cutGuidance->skipMatcher = true;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				warp.SetCameraGuidance(cutGuidance);
				Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"cut-skipped estimate");
				pl_gpu_finish(gpu);
				Assert::AreEqual(tall ? 1 : 0, warp.SearchHalvings(), L"unexpected search halvings");
				Assert::IsTrue(warp.CameraGuidanceUsed().matcherSkipped, L"guidance must report matcherSkipped");
				const DejudderField field = DownloadField(gpu, warp);
				const std::array<float, 3> ft = DownloadFrameTrust(gpu, warp);
				for (int by = 0; by < field.blocksY; ++by)
					for (int bx = 0; bx < field.blocksX; ++bx)
						Assert::IsFalse(field.trusted(bx, by),
							L"StillZero must not resurrect bit-identical blocks as trusted on a cut-skipped frame");
				Assert::IsTrue(ft[0] < 1e-4f, L"cut-skipped frame trusted share must remain 0");

				// Mandatory same-input negative control (Rule 3): run on the EXACT SAME frames WITHOUT cut-skip
				// guidance to prove the bit-identical left half genuinely triggers StillZero (trust = 1, v = 0)
				// and lifts FrameTrust share well above minTrustedShare (0.25).
				Dejudder::MotionWarp negWarp(fixture.Log(), gpu, settings);
				Assert::IsTrue(negWarp.Estimate(a.planes, b.planes), L"negative control unskipped estimate");
				pl_gpu_finish(gpu);
				Assert::IsFalse(negWarp.CameraGuidanceUsed().matcherSkipped, L"negative control must not skip matcher");
				const DejudderField negField = DownloadField(gpu, negWarp);
				const std::array<float, 3> negFt = DownloadFrameTrust(gpu, negWarp);
				int leftBlocks = 0, leftStillTrusted = 0;
				for (int by = 0; by < negField.blocksY; ++by)
					for (int bx = 0; bx < negField.blocksX / 2; ++bx)
					{
						++leftBlocks;
						if (negField.trusted(bx, by) &&
							std::fabs(negField.vx(bx, by)) < 1e-4f &&
							std::fabs(negField.vy(bx, by)) < 1e-4f)
							++leftStillTrusted;
					}
				Assert::IsTrue(leftBlocks > 0, L"must inspect bit-identical left-half blocks");
				Assert::AreEqual(leftBlocks, leftStillTrusted,
					L"negative control: unskipped StillZero must stamp every bit-identical left-half block (0, 0), trust = 1");
				Assert::IsTrue(negFt[0] >= 0.45f,
					L"negative control: bit-identical half-frame must raise FrameTrust share >= 0.45 without cut-skip");
			}
		}

		// GEM-118 (VE-MATCHER-FIXES): STILL_ZERO_BODY must require at least one in-bounds pixel (any_px)
		// before stamping (0, 0, 1, 1) onto subField. On a 64x24 frame (height not a multiple of 16 ->
		// blocksX = 4, blocksY = 2 -> subField is 8x4 with block_px = 8), row by = 3 covers source
		// y = 24..31, ALL >= psz.y = 24. With pictureArea = false (in_picture_() == true), only any_px
		// prevents vacuous same == true from clobbering row by = 3 with (0, 0, 1, 1).
		BEGIN_TEST_METHOD_ATTRIBUTE(StillZeroDoesNotStampOutOfBoundsSubFieldRowOnNonMultipleOf16Height)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StillZeroDoesNotStampOutOfBoundsSubFieldRowOnNonMultipleOf16Height)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			constexpr int kSubW = 64, kSubH = 24;   // 24 % 16 == 8 -> blocksY = 2, subField height = 4, row 3 is y=24..31 (OOB)
			constexpr int kPanX = 4;                // exact 2-px half-res pan aligned to 4-px texture cells
			const DejudderFrame fa = MakeDejudderFrame(kSubW, kSubH,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame fb = MakeDejudderFrame(kSubW, kSubH,
				[kPanX](int x, int y) { return DejudderTexture(x - kPanX, y); },
				[kPanX](int x, int y, int c) { return DejudderTexture((x - kPanX / 2) * 2, y, 7 + c); });
			DejudderGpuPlanes a, b;
			UploadDejudderFrame(gpu, fa, a);
			UploadDejudderFrame(gpu, fb, b);

			auto downloadSubField = [gpu](const Dejudder::MotionWarp& warp, int& sw, int& sh) -> std::vector<float>
			{
				pl_tex sf = warp.SubField();
				Assert::IsNotNull(sf, L"MotionWarp::SubField() must exist when estSplit is enabled");
				sw = sf->params.w;
				sh = sf->params.h;
				Assert::AreEqual(warp.BlocksX() * 2, sw, L"SubField width must be 2 * BlocksX");
				Assert::AreEqual(warp.BlocksY() * 2, sh, L"SubField height must be 2 * BlocksY");
				std::vector<uint16_t> half(static_cast<size_t>(sw) * sh * 4);
				pl_tex_transfer_params download{};
				download.tex = sf;
				download.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &download), L"SubField texture is not host-readable");
				std::vector<float> out(half.size());
				for (size_t i = 0; i < half.size(); ++i)
					out[i] = HalfBits(half[i]);
				return out;
			};

			auto isStillZeroStamp = [](float vx, float vy, float trust, float w) -> bool
			{
				return std::fabs(vx) < 1e-4f && std::fabs(vy) < 1e-4f && trust > 0.5f && w > 0.5f;
			};

			Dejudder::Settings settings = DejudderTestSettings();
			settings.estStillZero = true;
			settings.estSplit = true;
			settings.pictureArea = false;   // in_picture_() is always true; ONLY any_px guards OOB row by == 3
			settings.hostReadable = true;

			// 1. Panning pair (A, B): out-of-bounds row by == 3 must NOT be stamped (0, 0, 1, 1),
			// and interior unsplit sub-blocks on row by == 3 must preserve the parent's (4, 0) pan with w == 0.
			{
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"panning 64x24 estimate");
				pl_gpu_finish(gpu);
				Assert::AreEqual(4, warp.BlocksX(), L"64 px width must produce 4 parent blocks");
				Assert::AreEqual(2, warp.BlocksY(), L"24 px height must produce 2 parent blocks");
				int sw = 0, sh = 0;
				const std::vector<float> sf = downloadSubField(warp, sw, sh);
				Assert::AreEqual(8, sw, L"expected 8 sub-blocks across");
				Assert::AreEqual(4, sh, L"expected 4 sub-block rows (row 3 covers y = 24..31 >= 24)");
				for (int bx = 0; bx < sw; ++bx)
				{
					const size_t idx = (static_cast<size_t>(3) * sw + bx) * 4;
					const float vx = sf[idx], vy = sf[idx + 1], trust = sf[idx + 2], w = sf[idx + 3];
					Assert::IsFalse(isStillZeroStamp(vx, vy, trust, w),
						L"GEM-118: StillZero must not stamp out-of-bounds subField row by=3 with (0, 0, 1, 1)");
					if (bx >= 2 && bx <= 5)
					{
						Assert::IsTrue(w < 0.5f,
							L"GEM-118: interior unsplit sub-block on OOB row by=3 must keep split flag w=0");
						Assert::IsTrue(trust > 0.5f &&
							std::fabs(vx - static_cast<float>(kPanX)) <= 0.5f &&
							std::fabs(vy) <= 0.5f,
							L"GEM-118: interior sub-block on OOB row by=3 must preserve parent pan (4, 0)");
					}
				}
			}

			// 2. Mandatory same-input negative control (Rule 3): identical frames (A == A, exact same 64x24
			// size and settings) prove StillZero genuinely runs on subField and stamps every in-bounds
			// sub-block (by in 0..2) with (0, 0, 1, 1), while OOB row by == 3 still is NOT stamped.
			{
				Dejudder::MotionWarp stillWarp(fixture.Log(), gpu, settings);
				Assert::IsTrue(stillWarp.Estimate(a.planes, a.planes), L"identical 64x24 estimate (negative control)");
				pl_gpu_finish(gpu);
				int sw = 0, sh = 0;
				const std::vector<float> sf = downloadSubField(stillWarp, sw, sh);
				Assert::AreEqual(8, sw);
				Assert::AreEqual(4, sh);
				for (int by = 0; by < 3; ++by)
					for (int bx = 0; bx < sw; ++bx)
					{
						const size_t idx = (static_cast<size_t>(by) * sw + bx) * 4;
						Assert::IsTrue(isStillZeroStamp(sf[idx], sf[idx + 1], sf[idx + 2], sf[idx + 3]),
							L"negative control: StillZero must stamp every in-bounds sub-block (by=0..2) on A==A with (0, 0, 1, 1)");
					}
				for (int bx = 0; bx < sw; ++bx)
				{
					const size_t idx = (static_cast<size_t>(3) * sw + bx) * 4;
					Assert::IsFalse(isStillZeroStamp(sf[idx], sf[idx + 1], sf[idx + 2], sf[idx + 3]),
						L"GEM-118: even on identical A==A, StillZero must not stamp out-of-bounds row by=3 with (0, 0, 1, 1)");
					Assert::IsTrue(sf[idx + 3] < 0.5f,
						L"GEM-118: out-of-bounds row by=3 on identical A==A must keep incoming w=0");
				}
			}
		}

		// GEM-115 (VE-MATCHER-FIXES): OpticalFlowSearch must verify ahead->forward.size() and
		// ahead->backward.size() cover gridW * gridH * 2 int16s before calling UpdateSubresource.
		// A truncated or empty vector buffer with matching halfW/halfH/gridW/gridH must be rejected
		// cleanly with NoPairAhead() == true and PairSizeMismatch() == true (falling back to block
		// search), whereas a nullptr ahead pair reports NoPairAhead() == true and PairSizeMismatch() == false.
		BEGIN_TEST_METHOD_ATTRIBUTE(OpticalFlowSearchRejectsTruncatedBackwardVectorsWithVisibleDiagnostics)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OpticalFlowSearchRejectsTruncatedBackwardVectorsWithVisibleDiagnostics)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			constexpr int kOfW = 128, kOfH = 64;
			constexpr int kPanX = 4;
			const DejudderFrame fa = MakeDejudderFrame(kOfW, kOfH,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame fb = MakeDejudderFrame(kOfW, kOfH,
				[kPanX](int x, int y) { return DejudderTexture(x - kPanX, y); },
				[kPanX](int x, int y, int c) { return DejudderTexture((x - kPanX / 2) * 2, y, 7 + c); });
			DejudderGpuPlanes a, b;
			UploadDejudderFrame(gpu, fa, a);
			UploadDejudderFrame(gpu, fb, b);

			const std::shared_ptr<MotionAhead::Flow> completeFlow =
				SyntheticAheadFlow(kOfW, kOfH, kPanX / 2, 10, 0, 0, 0, 0);
			const size_t expectedInts = static_cast<size_t>(completeFlow->gridW) *
				static_cast<size_t>(completeFlow->gridH) * 2u;
			Assert::IsTrue(expectedInts >= 4u, L"synthetic flow grid must be non-empty");
			Assert::AreEqual(expectedInts, completeFlow->forward.size(), L"complete forward vector count");
			Assert::AreEqual(expectedInts, completeFlow->backward.size(), L"complete backward vector count");

			Dejudder::Settings settings = DejudderTestSettings();
			settings.opticalFlow = true;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);

			// 1. Complete flow baseline on exact same inputs: proves dimsOk and D3D11 upload succeed
			// when both forward and backward arrays hold gridW * gridH * 2 elements.
			warp.SetExternalFlow(completeFlow);
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"complete external flow estimate");
			Assert::IsFalse(warp.NoPairAhead(), L"complete matching flow must not report NoPairAhead");
			Assert::IsFalse(warp.PairSizeMismatch(), L"complete matching flow must not report PairSizeMismatch");
			Assert::IsTrue(warp.ExternalFlowUsed(), L"complete matching flow must be accepted and uploaded");
			Assert::IsTrue(warp.OpticalFlowUsed(), L"complete matching flow must mark OpticalFlowUsed");

			// 2. Truncated backward (one vector short: expectedInts - 2) with matching metadata and full forward.
			auto truncBackward = std::make_shared<MotionAhead::Flow>(*completeFlow);
			truncBackward->backward.resize(expectedInts - 2u);
			warp.SetExternalFlow(truncBackward);
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"truncated backward flow must fall back cleanly");
			Assert::IsTrue(warp.NoPairAhead(), L"GEM-115: truncated backward flow must report NoPairAhead");
			Assert::IsTrue(warp.PairSizeMismatch(), L"GEM-115: truncated backward flow must report PairSizeMismatch");
			Assert::IsFalse(warp.ExternalFlowUsed(), L"GEM-115: truncated backward flow must not be uploaded");
			Assert::IsFalse(warp.OpticalFlowUsed(), L"GEM-115: truncated backward flow must fall back to block search");

			// 3. Empty backward with matching metadata and full forward (pre-fix passed nullptr/empty to UpdateSubresource).
			auto emptyBackward = std::make_shared<MotionAhead::Flow>(*completeFlow);
			emptyBackward->backward.clear();
			warp.SetExternalFlow(emptyBackward);
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"empty backward flow must fall back cleanly without OOB read");
			Assert::IsTrue(warp.NoPairAhead(), L"GEM-115: empty backward flow must report NoPairAhead");
			Assert::IsTrue(warp.PairSizeMismatch(), L"GEM-115: empty backward flow must report PairSizeMismatch");
			Assert::IsFalse(warp.ExternalFlowUsed(), L"GEM-115: empty backward flow must not be uploaded");
			Assert::IsFalse(warp.OpticalFlowUsed(), L"GEM-115: empty backward flow must fall back to block search");

			// 4. Symmetric guard check: truncated forward (one vector short: expectedInts - 2) with full backward.
			auto truncForward = std::make_shared<MotionAhead::Flow>(*completeFlow);
			truncForward->forward.resize(expectedInts - 2u);
			warp.SetExternalFlow(truncForward);
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"truncated forward flow must fall back cleanly");
			Assert::IsTrue(warp.NoPairAhead(), L"GEM-115: truncated forward flow must report NoPairAhead");
			Assert::IsTrue(warp.PairSizeMismatch(), L"GEM-115: truncated forward flow must report PairSizeMismatch");
			Assert::IsFalse(warp.ExternalFlowUsed(), L"GEM-115: truncated forward flow must not be uploaded");
			Assert::IsFalse(warp.OpticalFlowUsed(), L"GEM-115: truncated forward flow must fall back to block search");

			// 5. Mandatory same-input negative control (Rule 3): nullptr ahead pair on the exact same frames
			// must report NoPairAhead() == true AND PairSizeMismatch() == false (distinguishing a missing pair
			// from a truncated/mismatched pair, and proving PairSizeMismatch resets cleanly).
			warp.SetExternalFlow(nullptr);
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"null external flow estimate (negative control)");
			Assert::IsTrue(warp.NoPairAhead(), L"negative control: null ahead pair must report NoPairAhead");
			Assert::IsFalse(warp.PairSizeMismatch(),
				L"negative control: null ahead pair must report PairSizeMismatch == false");
			Assert::IsFalse(warp.ExternalFlowUsed(), L"negative control: null ahead pair must not mark ExternalFlowUsed");
			Assert::IsFalse(warp.OpticalFlowUsed(), L"negative control: null ahead pair must not mark OpticalFlowUsed");
			pl_gpu_finish(gpu);
		}

		// GEM-114 (I#3 VE-MATCHER-FIXES + Pass-A MED-2 0315bb8d1): on lowRes frames (SearchHalvings() == 1,
		// lowResAll == false), FadeMatch runs inside `inner` while FRAME_TRUST runs on the outer MotionWarp.
		// Outer FRAME_TRUST must bind inner->FadeStateTexture() both on a fresh tall instance (where outer
		// fadeState is nullptr) and after an in-place non-halved -> lowRes resize (where outer fadeState
		// remains allocated with stale non-fading z == 0 from the 256p estimate).
		BEGIN_TEST_METHOD_ATTRIBUTE(LowResFadePropagatesInnerFadeStateToOuterFrameTrustAndAfterResize)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LowResFadePropagatesInnerFadeStateToOuterFrameTrustAndAfterResize)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			struct LogScope
			{
				TargetLutGpuFixture& f;
				LogScope(TargetLutGpuFixture& fx, std::string* s) : f(fx) { f.CaptureLog(s); }
				~LogScope() { f.CaptureLog(nullptr); }
			} logScope(fixture, &log);

			constexpr int kFadeW = 128;
			constexpr int kSmallH = 256;    // 256^2 < 2 * 1080^2 -> SearchHalvings() == 0
			constexpr int kTallH = 1536;    // 1536^2 >= 2 * 1080^2 -> SearchHalvings() == 1
			constexpr float kFadeFloor = 0.25f;

			// Uniform multiplicative dimming about limited-range black (16 codes = 4096 in 16-bit):
			// -6% per frame keeps |d| ~ 6.5 codes in (0.75, 24), |1 - g| ~ 0.06 > 0.01, and implied black ~ 16 in (-12, 48).
			auto makeDimFrame = [](int w, int h, double level) -> DejudderFrame
			{
				return MakeDejudderFrame(w, h,
					[level](int x, int y) -> uint16_t {
						const double v = 4096.0 + (static_cast<double>(DejudderTexture(x, y, 13)) - 4096.0) * level;
						return static_cast<uint16_t>(std::lround((std::min)(65535.0, (std::max)(0.0, v))));
					},
					[](int, int, int c) -> uint16_t { return static_cast<uint16_t>(32768 + c); });
			};

			DejudderGpuPlanes tall0, tall1, tall2, smallA, smallB;
			UploadDejudderFrame(gpu, makeDimFrame(kFadeW, kTallH, 1.00), tall0);
			UploadDejudderFrame(gpu, makeDimFrame(kFadeW, kTallH, 0.94), tall1);
			UploadDejudderFrame(gpu, makeDimFrame(kFadeW, kTallH, 0.88), tall2);
			UploadDejudderFrame(gpu, makeDimFrame(kFadeW, kSmallH, 1.00), smallA);
			UploadDejudderFrame(gpu, makeDimFrame(kFadeW, kSmallH, 1.00), smallB);

			auto makeSettings = [kFadeFloor](bool fadeMatch) -> Dejudder::Settings
			{
				Dejudder::Settings s = DejudderTestSettings();
				s.lowRes = true;
				s.lowResAll = false;        // FadeMatch runs on inner; FRAME_TRUST runs on outer
				s.fadeMatch = fadeMatch;
				s.fadeFloor = kFadeFloor;
				s.strengthRate = 1.0f;      // settle to fadeFloor on the first confirmed fade pair
				s.motionScaleHi = 0.0f;     // disable D2 ramp so ONLY fade_floor drives FrameTrust().g
				s.trustScaleFull = 0.0f;    // disable D4 ramp so ONLY fade_floor drives FrameTrust().g
				s.motionRolloff = false;    // disable R-A roll-off so ONLY fade_floor drives FrameTrust().g
				s.hostReadable = true;
				return s;
			};

			// 1. Fresh lowRes MotionWarp (outer fadeState == nullptr): inner FadeMatch must propagate to outer FRAME_TRUST.
			{
				Dejudder::MotionWarp warp(fixture.Log(), gpu, makeSettings(true));
				Assert::IsTrue(warp.Estimate(tall0.planes, tall1.planes), L"fresh lowRes dimming pair 0->1");
				Assert::AreEqual(1, warp.SearchHalvings(), L"128x1536 lowRes must run with SearchHalvings() == 1");
				Assert::IsTrue(warp.Estimate(tall1.planes, tall2.planes), L"fresh lowRes dimming pair 1->2");
				pl_gpu_finish(gpu);
				Assert::AreEqual(1, warp.SearchHalvings(), L"128x1536 lowRes must stay at SearchHalvings() == 1");
				Assert::IsNotNull(warp.FadeStateForTest(),
					L"GEM-114 fresh lowRes: FadeStateForTest() must expose inner->FadeStateTexture()");
				const std::array<float, 3> ft = DownloadFrameTrust(gpu, warp);
				Assert::IsTrue(ft[0] >= 0.9f,
					(L"GEM-114 fresh lowRes: fade-corrected pair 1->2 must not hold as a cut (share=" +
						std::to_wstring(ft[0]) + L")").c_str());
				Assert::IsTrue(ft[1] >= kFadeFloor - 0.05f && ft[1] <= kFadeFloor + 0.05f,
					(L"GEM-114 fresh lowRes: outer FrameTrust().g must be attenuated to fadeFloor=0.25 (got " +
						std::to_wstring(ft[1]) + L")").c_str());
			}

			// 2. In-place resize from 128x256 (SearchHalvings() == 0, allocates outer fadeState with z == 0)
			// to 128x1536 (SearchHalvings() == 1): outer FRAME_TRUST must prefer inner->FadeStateTexture()
			// over the stale outer fadeState (Pass-A MED-2 fix 0315bb8d1).
			{
				Dejudder::MotionWarp warp(fixture.Log(), gpu, makeSettings(true));
				Assert::IsTrue(warp.Estimate(smallA.planes, smallB.planes), L"256p non-fading pre-resize estimate");
				pl_gpu_finish(gpu);
				Assert::AreEqual(0, warp.SearchHalvings(), L"128x256 pre-resize must run with SearchHalvings() == 0");
				const pl_tex preResizeFade = warp.FadeStateForTest();
				Assert::IsNotNull(preResizeFade, L"256p pre-resize fadeMatch=true must allocate outer fadeState");
				const std::array<float, 3> ftSmall = DownloadFrameTrust(gpu, warp);
				Assert::IsTrue(ftSmall[0] >= 0.9f, L"256p identical pair must have high trusted share");
				Assert::IsTrue(ftSmall[1] > 0.9f,
					(L"256p non-fading pair must keep unattenuated FrameTrust().g > 0.9 (got " +
						std::to_wstring(ftSmall[1]) + L")").c_str());

				Assert::IsTrue(warp.Estimate(tall0.planes, tall1.planes), L"post-resize lowRes dimming pair 0->1");
				Assert::AreEqual(1, warp.SearchHalvings(), L"post-resize 128x1536 must switch to SearchHalvings() == 1");
				Assert::IsTrue(warp.Estimate(tall1.planes, tall2.planes), L"post-resize lowRes dimming pair 1->2");
				pl_gpu_finish(gpu);
				Assert::AreEqual(1, warp.SearchHalvings(), L"post-resize 128x1536 must remain at SearchHalvings() == 1");
				const pl_tex postResizeFade = warp.FadeStateForTest();
				Assert::IsNotNull(postResizeFade,
					L"GEM-114 post-resize lowRes: FadeStateForTest() must remain non-null after in-place resize");
				Assert::IsTrue(postResizeFade != preResizeFade,
					L"GEM-114 post-resize lowRes: FadeStateForTest() must switch from stale outer fadeState to inner->FadeStateTexture()");
				const std::array<float, 3> ftTall = DownloadFrameTrust(gpu, warp);
				Assert::IsTrue(ftTall[0] >= 0.9f,
					(L"GEM-114 post-resize lowRes: fade-corrected pair 1->2 must not hold as a cut (share=" +
						std::to_wstring(ftTall[0]) + L")").c_str());
				Assert::IsTrue(ftTall[1] >= kFadeFloor - 0.05f && ftTall[1] <= kFadeFloor + 0.05f,
					(L"GEM-114 post-resize lowRes: stale outer fadeState shadowed inner->FadeStateTexture() (FrameTrust().g=" +
						std::to_wstring(ftTall[1]) + L")").c_str());
			}

			// 3. Mandatory same-input negative control (Rule 3): run the EXACT SAME 3-frame 128x1536 dimming
			// sequence with fadeMatch = false. Verifies (a) plain matching genuinely breaks under -6%/frame
			// dimming (ftNeg[0] < 0.25 vs ft[0] >= 0.9 with fadeMatch = true), proving the hazard is real on
			// these planes, and (b) FrameTrust().g stays > 0.9 without fadeMatch.
			{
				Dejudder::MotionWarp negWarp(fixture.Log(), gpu, makeSettings(false));
				Assert::IsTrue(negWarp.Estimate(tall0.planes, tall1.planes), L"negative control dimming pair 0->1");
				Assert::IsTrue(negWarp.Estimate(tall1.planes, tall2.planes), L"negative control dimming pair 1->2");
				pl_gpu_finish(gpu);
				Assert::AreEqual(1, negWarp.SearchHalvings(), L"negative control must also run at SearchHalvings() == 1");
				const std::array<float, 3> ftNeg = DownloadFrameTrust(gpu, negWarp);
				Assert::IsTrue(ftNeg[0] < 0.25f,
					(L"negative control (fadeMatch=false): -6%/frame dimming must break plain matching below minTrustedShare=0.25 (share=" +
						std::to_wstring(ftNeg[0]) + L")").c_str());
				Assert::IsTrue(ftNeg[1] > 0.9f,
					(L"negative control (fadeMatch=false): FrameTrust().g must remain > 0.9 (got " +
						std::to_wstring(ftNeg[1]) + L")").c_str());
			}

			// 4. Mandatory lowRes non-fading control with fadeMatch = true (exact same makeSettings(true), so
			// #define FADE 1 is compiled into outer FRAME_TRUST and reads inner->FadeStateTexture()): verifies
			// fadeFloor = 0.25 is genuinely gated on inner->FadeStateTexture().z > 0.5 and does not attenuate
			// a non-fading 128x1536 (SearchHalvings() == 1) pair.
			{
				Dejudder::MotionWarp plainWarp(fixture.Log(), gpu, makeSettings(true));
				Assert::IsTrue(plainWarp.Estimate(tall0.planes, tall0.planes), L"non-fading lowRes control pair 1");
				Assert::AreEqual(1, plainWarp.SearchHalvings(), L"non-fading lowRes control must run at SearchHalvings() == 1");
				Assert::IsTrue(plainWarp.Estimate(tall0.planes, tall0.planes), L"non-fading lowRes control pair 2");
				pl_gpu_finish(gpu);
				Assert::AreEqual(1, plainWarp.SearchHalvings(), L"non-fading lowRes control must remain at SearchHalvings() == 1");
				Assert::IsNotNull(plainWarp.FadeStateForTest(),
					L"non-fading lowRes control (fadeMatch=true): FadeStateForTest() must expose inner->FadeStateTexture()");
				const std::array<float, 3> ftPlain = DownloadFrameTrust(gpu, plainWarp);
				Assert::IsTrue(ftPlain[0] >= 0.9f,
					(L"non-fading lowRes control (fadeMatch=true): trusted share must be >= 0.9 (got " +
						std::to_wstring(ftPlain[0]) + L")").c_str());
				Assert::IsTrue(ftPlain[1] > 0.9f,
					(L"non-fading lowRes control (fadeMatch=true): non-fading FrameTrust().g must remain > 0.9 (got " +
						std::to_wstring(ftPlain[1]) + L")").c_str());
			}

			AssertCleanLibplaceboLog(log, "LowResFadePropagatesInnerFadeStateToOuterFrameTrustAndAfterResize");
		}

		// GEM-121 (I#6 VE-MATCHER-PERF): when both occlusionFill and dither occlusionSlot (0/1) are active
		// with matching SPLIT defines (subOk == settings.estSplit), Estimate dispatches OCCLUSION_BODY once
		// into occlusion[occlusionSlot] and aliases activeFillOcclusion to it. Verifies single-dispatch reuse,
		// bit-for-bit parity against standalone dither occlusion (occlusionFill=false) and standalone fillOcclusion
		// Warp (occlusionSlot=-1) across both alternating slots, preservation of Occlusion(0) when writing
		// Occlusion(1), and same-input negative controls proving non-trivial occlusion and active D5 fill.
		BEGIN_TEST_METHOD_ATTRIBUTE(OcclusionFillSingleDispatchReuseMatchesSeparateDispatchAndPreservesAlternatingSlot)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OcclusionFillSingleDispatchReuseMatchesSeparateDispatchAndPreservesAlternatingSlot)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			struct LogScope
			{
				TargetLutGpuFixture& f;
				LogScope(TargetLutGpuFixture& fx, std::string* s) : f(fx) { f.CaptureLog(s); }
				~LogScope() { f.CaptureLog(nullptr); }
			} logScope(fixture, &log);

			constexpr int kOccW = 256, kOccH = 128;
			constexpr int kDx1 = 8;
			constexpr int kDx2 = -8;
			auto inObj1 = [](int x, int y) { return x >= 96 && x < 144 && y >= 32 && y < 80; };
			auto inObj2 = [](int x, int y) { return x >= 32 && x < 80 && y >= 48 && y < 96; };

			// Pair 1 (slot 0): dx = +8, dy = 0 pan with a 48x48 occluding object removed in B -> genuine occluded
			// blocks in Occlusion(0) and genuine D5 fill pixels in Warp(a1, 0.5f, out).
			const DejudderFrame fa1 = MakeDejudderFrame(kOccW, kOccH,
				[&](int x, int y) { return inObj1(x, y) ? DejudderTexture(x, y, 55) : DejudderTexture(x, y); },
				[&](int x, int y, int c) { return inObj1(x * 2, y) ? DejudderTexture(x * 2, y, 57 + c) : DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame fb1 = MakeDejudderFrame(kOccW, kOccH,
				[kDx1](int x, int y) { return DejudderTexture(x - kDx1, y); },
				[kDx1](int x, int y, int c) { return DejudderTexture((x - kDx1 / 2) * 2, y, 7 + c); });

			// Pair 2 (slot 1): opposite pan dx = -8, dy = 0 and a disjoint 48x48 occluded region so Occlusion(1)
			// is strictly distinct from Occlusion(0).
			const DejudderFrame fa2 = MakeDejudderFrame(kOccW, kOccH,
				[&](int x, int y) { return inObj2(x, y) ? DejudderTexture(x, y, 77) : DejudderTexture(x, y, 21); },
				[&](int x, int y, int c) { return inObj2(x * 2, y) ? DejudderTexture(x * 2, y, 79 + c) : DejudderTexture(x * 2, y, 27 + c); });
			const DejudderFrame fb2 = MakeDejudderFrame(kOccW, kOccH,
				[kDx2](int x, int y) { return DejudderTexture(x - kDx2, y, 21); },
				[kDx2](int x, int y, int c) { return DejudderTexture((x - kDx2 / 2) * 2, y, 27 + c); });

			DejudderGpuPlanes a1, b1, a2, b2;
			UploadDejudderFrame(gpu, fa1, a1);
			UploadDejudderFrame(gpu, fb1, b1);
			UploadDejudderFrame(gpu, fa2, a2);
			UploadDejudderFrame(gpu, fb2, b2);

			auto downloadOcclusionRaw = [gpu](const Dejudder::MotionWarp& warp, int slot) -> std::vector<uint16_t>
			{
				pl_tex occ = warp.Occlusion(slot);
				Assert::IsNotNull(occ, L"MotionWarp::Occlusion(slot) must be allocated");
				Assert::AreEqual(warp.BlocksX(), occ->params.w, L"Occlusion width must match BlocksX");
				Assert::AreEqual(warp.BlocksY(), occ->params.h, L"Occlusion height must match BlocksY");
				std::vector<uint16_t> raw(static_cast<size_t>(occ->params.w) * occ->params.h * 4u);
				pl_tex_transfer_params download{};
				download.tex = occ;
				download.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &download), L"Occlusion texture is not host-readable");
				return raw;
			};

			auto countOcclusion = [](const std::vector<uint16_t>& raw, int& occluded, int& visible)
			{
				occluded = 0;
				visible = 0;
				for (size_t i = 0; i + 3 < raw.size(); i += 4)
				{
					if (HalfBits(raw[i]) < 0.5f)
						++occluded;
					else
						++visible;
				}
			};

			auto makeSettings = [](bool occlusionFill) -> Dejudder::Settings
			{
				Dejudder::Settings s = DejudderTestSettings();
				s.occlusionFill = occlusionFill;
				s.estSplit = true;
				s.hostReadable = true;
				return s;
			};

			// 1. Shared single-dispatch path (occlusionFill = true, estSplit = true, alternating slots 0 -> 1).
			Dejudder::MotionWarp sharedWarp(fixture.Log(), gpu, makeSettings(true));
			sharedWarp.SetPassTiming(true, true);
			Assert::IsTrue(sharedWarp.Estimate(a1.planes, b1.planes, 0), L"GEM-121 shared Estimate #1 (slot 0)");
			Assert::AreEqual(size_t(1), sharedWarp.PassMs().count("VideoProcessor de-judder: occlusion for the dither"),
				L"GEM-121: shared Estimate #1 must dispatch the unified occlusion pass once");
			Assert::AreEqual(size_t(0), sharedWarp.PassMs().count("VideoProcessor de-judder: occlusion for the fill (D5)"),
				L"GEM-121: shared Estimate #1 must not dispatch a redundant standalone fill occlusion pass");
			pl_gpu_finish(gpu);

			const std::vector<uint16_t> sharedOcc0Before = downloadOcclusionRaw(sharedWarp, 0);
			int occ0Occluded = 0, occ0Visible = 0;
			countOcclusion(sharedOcc0Before, occ0Occluded, occ0Visible);
			Assert::IsTrue(occ0Occluded > 0 && occ0Visible > 0,
				(L"negative control: Occlusion(0) must be non-trivial (occluded=" + std::to_wstring(occ0Occluded) +
					L", visible=" + std::to_wstring(occ0Visible) + L")").c_str());

			Dejudder::Planes sharedWarped1;
			Assert::IsTrue(sharedWarp.Warp(a1.planes, 0.5f, sharedWarped1), L"GEM-121 shared Warp #1 (slot 0)");
			pl_gpu_finish(gpu);
			const std::vector<uint16_t> sharedLuma1 = DownloadPlane(gpu, sharedWarped1.luma);
			const std::vector<uint16_t> sharedChroma1 = DownloadPlane(gpu, sharedWarped1.chroma);

			// Estimate #2 into slot 1: must preserve Occlusion(0) bit-for-bit while writing distinct Occlusion(1).
			Assert::IsTrue(sharedWarp.Estimate(a2.planes, b2.planes, 1), L"GEM-121 shared Estimate #2 (slot 1)");
			Assert::AreEqual(size_t(0), sharedWarp.PassMs().count("VideoProcessor de-judder: occlusion for the fill (D5)"),
				L"GEM-121: shared Estimate #2 (slot 1) must not dispatch a redundant standalone fill occlusion pass");
			pl_gpu_finish(gpu);

			const std::vector<uint16_t> sharedOcc0After = downloadOcclusionRaw(sharedWarp, 0);
			const std::vector<uint16_t> sharedOcc1After = downloadOcclusionRaw(sharedWarp, 1);
			Assert::AreEqual(sharedOcc0Before.size(), sharedOcc0After.size());
			Assert::AreEqual(0, std::memcmp(sharedOcc0Before.data(), sharedOcc0After.data(),
				sharedOcc0Before.size() * sizeof(uint16_t)),
				L"GEM-121: Estimate #2 into slot 1 must preserve Occlusion(0) bit-for-bit");
			int occ1Occluded = 0, occ1Visible = 0;
			countOcclusion(sharedOcc1After, occ1Occluded, occ1Visible);
			Assert::IsTrue(occ1Occluded > 0 && occ1Visible > 0,
				(L"GEM-121: Occlusion(1) must be non-trivial (occluded=" + std::to_wstring(occ1Occluded) +
					L", visible=" + std::to_wstring(occ1Visible) + L")").c_str());
			Assert::IsTrue(std::memcmp(sharedOcc0After.data(), sharedOcc1After.data(),
				sharedOcc0After.size() * sizeof(uint16_t)) != 0,
				L"GEM-121: Occlusion(1) from distinct pair 2 must differ from preserved Occlusion(0)");

			Dejudder::Planes sharedWarped2;
			Assert::IsTrue(sharedWarp.Warp(a2.planes, 0.5f, sharedWarped2), L"GEM-121 shared Warp #2 (slot 1)");
			pl_gpu_finish(gpu);
			const std::vector<uint16_t> sharedLuma2 = DownloadPlane(gpu, sharedWarped2.luma);
			const std::vector<uint16_t> sharedChroma2 = DownloadPlane(gpu, sharedWarped2.chroma);

			// 2. Reference standalone dither occlusion (occlusionFill = false, occlusionSlot = 0):
			// Occlusion(0) must match GEM-121 bit-for-bit, and Warp without occlusionFill serves as a same-input
			// negative control proving activeFillOcclusion genuinely drove D5 fill in luma and chroma.
			Dejudder::MotionWarp ditherRef(fixture.Log(), gpu, makeSettings(false));
			Assert::IsTrue(ditherRef.Estimate(a1.planes, b1.planes, 0), L"standalone dither occlusion Estimate #1");
			pl_gpu_finish(gpu);
			const std::vector<uint16_t> refOcc0 = downloadOcclusionRaw(ditherRef, 0);
			Assert::AreEqual(refOcc0.size(), sharedOcc0Before.size());
			Assert::AreEqual(0, std::memcmp(refOcc0.data(), sharedOcc0Before.data(),
				refOcc0.size() * sizeof(uint16_t)),
				L"GEM-121: shared Occlusion(0) must match standalone occlusionFill=false Occlusion(0) bit-for-bit");

			Dejudder::Planes noFillWarped1;
			Assert::IsTrue(ditherRef.Warp(a1.planes, 0.5f, noFillWarped1), L"negative control Warp without occlusionFill");
			pl_gpu_finish(gpu);
			const std::vector<uint16_t> noFillLuma1 = DownloadPlane(gpu, noFillWarped1.luma);
			const std::vector<uint16_t> noFillChroma1 = DownloadPlane(gpu, noFillWarped1.chroma);
			Assert::IsTrue(sharedLuma1.size() == noFillLuma1.size() &&
				std::memcmp(sharedLuma1.data(), noFillLuma1.data(), sharedLuma1.size() * sizeof(uint16_t)) != 0,
				L"negative control: activeFillOcclusion D5 fill must alter warped luma vs occlusionFill=false");
			Assert::IsTrue(sharedChroma1.size() == noFillChroma1.size() &&
				std::memcmp(sharedChroma1.data(), noFillChroma1.data(), sharedChroma1.size() * sizeof(uint16_t)) != 0,
				L"negative control: activeFillOcclusion D5 fill must alter warped chroma vs occlusionFill=false");

			// 3. Reference non-reused fillOcclusion path (occlusionFill = true, occlusionSlot = -1):
			// warped luma and chroma for both pairs must match GEM-121 aliased activeFillOcclusion bit-for-bit.
			Dejudder::MotionWarp separateFillRef(fixture.Log(), gpu, makeSettings(true));
			separateFillRef.SetPassTiming(true, true);
			Assert::IsTrue(separateFillRef.Estimate(a1.planes, b1.planes, -1), L"non-reused fillOcclusion Estimate #1");
			Assert::AreEqual(size_t(1), separateFillRef.PassMs().count("VideoProcessor de-judder: occlusion for the fill (D5)"),
				L"non-dither occlusionSlot=-1 must dispatch dedicated fill occlusion pass");
			Assert::AreEqual(size_t(0), separateFillRef.PassMs().count("VideoProcessor de-judder: occlusion for the dither"),
				L"non-dither occlusionSlot=-1 must not dispatch dither occlusion pass");
			Dejudder::Planes sepWarped1;
			Assert::IsTrue(separateFillRef.Warp(a1.planes, 0.5f, sepWarped1), L"non-reused fillOcclusion Warp #1");
			pl_gpu_finish(gpu);
			const std::vector<uint16_t> sepLuma1 = DownloadPlane(gpu, sepWarped1.luma);
			const std::vector<uint16_t> sepChroma1 = DownloadPlane(gpu, sepWarped1.chroma);
			Assert::AreEqual(sepLuma1.size(), sharedLuma1.size());
			Assert::AreEqual(0, std::memcmp(sepLuma1.data(), sharedLuma1.data(),
				sepLuma1.size() * sizeof(uint16_t)),
				L"GEM-121: Warp #1 luma via aliased Occlusion(0) must match non-reused fillOcclusion bit-for-bit");
			Assert::AreEqual(sepChroma1.size(), sharedChroma1.size());
			Assert::AreEqual(0, std::memcmp(sepChroma1.data(), sharedChroma1.data(),
				sepChroma1.size() * sizeof(uint16_t)),
				L"GEM-121: Warp #1 chroma via aliased Occlusion(0) must match non-reused fillOcclusion bit-for-bit");

			Assert::IsTrue(separateFillRef.Estimate(a2.planes, b2.planes, -1), L"non-reused fillOcclusion Estimate #2");
			Dejudder::Planes sepWarped2;
			Assert::IsTrue(separateFillRef.Warp(a2.planes, 0.5f, sepWarped2), L"non-reused fillOcclusion Warp #2");
			pl_gpu_finish(gpu);
			const std::vector<uint16_t> sepLuma2 = DownloadPlane(gpu, sepWarped2.luma);
			const std::vector<uint16_t> sepChroma2 = DownloadPlane(gpu, sepWarped2.chroma);
			Assert::AreEqual(sepLuma2.size(), sharedLuma2.size());
			Assert::AreEqual(0, std::memcmp(sepLuma2.data(), sharedLuma2.data(),
				sepLuma2.size() * sizeof(uint16_t)),
				L"GEM-121: Warp #2 luma via aliased Occlusion(1) must match non-reused fillOcclusion bit-for-bit");
			Assert::AreEqual(sepChroma2.size(), sharedChroma2.size());
			Assert::AreEqual(0, std::memcmp(sepChroma2.data(), sharedChroma2.data(),
				sepChroma2.size() * sizeof(uint16_t)),
				L"GEM-121: Warp #2 chroma via aliased Occlusion(1) must match non-reused fillOcclusion bit-for-bit");

			AssertCleanLibplaceboLog(log, "OcclusionFillSingleDispatchReuseMatchesSeparateDispatchAndPreservesAlternatingSlot");
		}

		// GEM-139 (J#1 MC-SEARCH): verifies bit-exact parity of the optimized parallel SEARCH_BODY
		// (l2_from/l1_from/to_win[324], thread-local L1 reduction, parallel half-pel into l1_tab summed
		// in exact (y, x) order) and REFINE_BODY (lead-thread rf_cand/rf_med/rf_nc/rf_dup duplicate skip)
		// against the untouched SEARCH_BODY_SERIAL / REFINE_BODY_SERIAL references across:
		//   1. globalTwo = false (L1_NSTARTS = 3, ZGROUP = 6), pairs 1 (unseeded) and 2 (use_temporal != 0),
		//   2. globalTwo = true  (L1_NSTARTS = 4, ZGROUP = 7), both seeded full-field bit-exactness vs serial
		//      and unseeded active-second-global interior parity + genuine beyond-coarse-range two-global
		//      (+128 / -128) tracking with same-input globalTwo = false negative control,
		//   3. same-input non-triviality checks (pan + counter-moving object + half-pel subpixel region) and
		//      static (F0, F0) negative control proving DownloadField + texel equality detects field changes.
		BEGIN_TEST_METHOD_ATTRIBUTE(McSearchParallelMatchesSerialSearchBitIdenticallyAcrossSingleAndTwoGlobal)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(McSearchParallelMatchesSerialSearchBitIdenticallyAcrossSingleAndTwoGlobal)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			struct LogScope
			{
				TargetLutGpuFixture& f;
				LogScope(TargetLutGpuFixture& fx, std::string* s) : f(fx) { f.CaptureLog(s); }
				~LogScope() { f.CaptureLog(nullptr); }
			} logScope(fixture, &log);

			constexpr int kMcW = 256, kMcH = 128;
			constexpr int kPanX = 10, kPanY = 4;       // dominant uniform pan (+5, +2) half-res -> dense REFINE_BODY duplicates
			constexpr int kObj1Dx = -6, kObj1Dy = 6;   // distinct moving object (-3, +3) half-res
			constexpr int kObj2Dx = 5, kObj2Dy = 3;    // odd source shift (+2.5, +1.5) half-res -> genuine half-pel refinement

			auto inObj1 = [](int x, int y, int t)
			{
				return x >= 48 + kObj1Dx * t && x < 112 + kObj1Dx * t &&
					y >= 16 + kObj1Dy * t && y < 80 + kObj1Dy * t;
			};
			auto inObj2 = [](int x, int y, int t)
			{
				return x >= 144 + kObj2Dx * t && x < 208 + kObj2Dx * t &&
					y >= 52 + kObj2Dy * t && y < 116 + kObj2Dy * t;
			};
			// Band-limited 2D trigonometric texture for Object 2 so odd-pixel (+5, +3) shifts have
			// < 1 code bilinear half-pel error (trusted in Field()) while whole-pel offsets have >> 10 codes.
			auto smoothHalfPelTexture = [](int x, int y) -> uint16_t
			{
				const double fx = static_cast<double>(x);
				const double fy = static_cast<double>(y);
				const double v = 32768.0 +
					8000.0 * std::sin(0.21 * fx + 0.13 * fy) +
					7000.0 * std::cos(0.17 * fx - 0.23 * fy) +
					5000.0 * std::sin(0.31 * fx + 0.29 * fy);
				return static_cast<uint16_t>(std::lround((std::min)(65535.0, (std::max)(0.0, v))));
			};

			auto makeMultiMotionFrame = [&](int t) -> DejudderFrame
			{
				return MakeDejudderFrame(kMcW, kMcH,
					[&](int x, int y) -> uint16_t
					{
						if (inObj1(x, y, t))
							return DejudderTexture(x - kObj1Dx * t, y - kObj1Dy * t, 37);
						if (inObj2(x, y, t))
							return smoothHalfPelTexture(x - kObj2Dx * t, y - kObj2Dy * t);
						return DejudderTexture(x - kPanX * t, y - kPanY * t, 1);
					},
					[&](int x, int y, int c) -> uint16_t
					{
						const int sx = x * 2;
						if (inObj1(sx, y, t))
							return DejudderTexture(sx - kObj1Dx * t, y - kObj1Dy * t, 41 + c);
						if (inObj2(sx, y, t))
							return static_cast<uint16_t>(32768 + c * 64);
						return DejudderTexture(sx - kPanX * t, y - kPanY * t, 7 + c);
					});
			};

			DejudderGpuPlanes f0, f1, f2;
			UploadDejudderFrame(gpu, makeMultiMotionFrame(0), f0);
			UploadDejudderFrame(gpu, makeMultiMotionFrame(1), f1);
			UploadDejudderFrame(gpu, makeMultiMotionFrame(2), f2);

			auto assertBitExact = [](const DejudderField& par, const DejudderField& ser, const wchar_t* label)
			{
				Assert::AreEqual(ser.blocksX, par.blocksX, (std::wstring(label) + L": blocksX mismatch").c_str());
				Assert::AreEqual(ser.blocksY, par.blocksY, (std::wstring(label) + L": blocksY mismatch").c_str());
				Assert::AreEqual(ser.texels.size(), par.texels.size(), (std::wstring(label) + L": texel count mismatch").c_str());
				Assert::IsFalse(par.texels.empty(), (std::wstring(label) + L": empty field").c_str());
				float maxAbsDiff = 0.0f;
				size_t firstBad = par.texels.size();
				for (size_t i = 0; i < par.texels.size(); ++i)
				{
					const float p = par.texels[i], s = ser.texels[i];
					Assert::IsTrue(std::isfinite(p) && std::isfinite(s),
						(std::wstring(label) + L": non-finite texel at index " + std::to_wstring(i)).c_str());
					const float d = std::fabs(p - s);
					if (d > maxAbsDiff)
						maxAbsDiff = d;
					if (p != s && firstBad == par.texels.size())
						firstBad = i;
				}
				if (firstBad != par.texels.size() || maxAbsDiff != 0.0f)
				{
					const size_t blk = firstBad / 4u;
					const size_t bx = blk % static_cast<size_t>(par.blocksX);
					const size_t by = blk / static_cast<size_t>(par.blocksX);
					const size_t ch = firstBad % 4u;
					const std::wstring msg = std::wstring(label) +
						L": parallel vs serial mismatch (maxAbsDiff=" + std::to_wstring(maxAbsDiff) +
						L", first at block (" + std::to_wstring(bx) + L"," + std::to_wstring(by) +
						L") ch=" + std::to_wstring(ch) + L": par=" + std::to_wstring(par.texels[firstBad]) +
						L", ser=" + std::to_wstring(ser.texels[firstBad]) + L")";
					Assert::Fail(msg.c_str());
				}
				Assert::IsTrue(par.texels == ser.texels, (std::wstring(label) + L": texels vector inequality").c_str());
			};

			auto assertNonTrivialRegions = [&](const DejudderField& field, const wchar_t* label)
			{
				int panBlocks = 0, obj1Blocks = 0, obj2HalfPelBlocks = 0;
				for (int by = 0; by < field.blocksY; ++by)
				{
					for (int bx = 0; bx < field.blocksX; ++bx)
					{
						if (!field.trusted(bx, by))
							continue;
						const float vx = field.vx(bx, by), vy = field.vy(bx, by);
						if (std::fabs(vx - static_cast<float>(kPanX)) <= 0.5f &&
							std::fabs(vy - static_cast<float>(kPanY)) <= 0.5f)
							++panBlocks;
						if (std::fabs(vx - static_cast<float>(kObj1Dx)) <= 0.5f &&
							std::fabs(vy - static_cast<float>(kObj1Dy)) <= 0.5f)
							++obj1Blocks;
						if (std::fabs(vx - static_cast<float>(kObj2Dx)) <= 0.5f &&
							std::fabs(vy - static_cast<float>(kObj2Dy)) <= 0.5f &&
							(std::fabs(vx - std::round(vx * 0.5f) * 2.0f) >= 0.5f ||
								std::fabs(vy - std::round(vy * 0.5f) * 2.0f) >= 0.5f))
							++obj2HalfPelBlocks;
					}
				}
				Assert::IsTrue(panBlocks >= 32,
					(std::wstring(label) + L": negative control failed - expected >= 32 trusted pan (+10,+4) blocks, got " +
						std::to_wstring(panBlocks)).c_str());
				Assert::IsTrue(obj1Blocks >= 2,
					(std::wstring(label) + L": negative control failed - expected >= 2 trusted object-1 (-6,+6) blocks, got " +
						std::to_wstring(obj1Blocks)).c_str());
				Assert::IsTrue(obj2HalfPelBlocks >= 2,
					(std::wstring(label) + L": negative control failed - expected >= 2 trusted half-pel (+5,+3) blocks, got " +
						std::to_wstring(obj2HalfPelBlocks)).c_str());
			};

			auto makeSettings = [](bool globalTwo) -> Dejudder::Settings
			{
				Dejudder::Settings s = DejudderTestSettings();
				s.globalPrior = true;
				s.globalTwo = globalTwo;
				s.estCandidates = true;   // enables REFINE_BODY second sweep + temporal prior on pair 2
				s.estLambda = 0.5f;
				s.estGate = false;
				s.estQpel = false;
				s.opticalFlow = false;
				s.hostReadable = true;
				return s;
			};

			// 1. globalTwo = false (L1_NSTARTS = 3, ZGROUP = 6): bit-exact parity of parallel SEARCH_BODY +
			// REFINE_BODY vs SEARCH_BODY_SERIAL + REFINE_BODY_SERIAL across pair 1 (unseeded) and pair 2 (use_temporal != 0).
			DejudderField parPair1Field, parPair1Fwd, serPair1Field, serPair2Field;
			{
				const Dejudder::Settings s = makeSettings(false);
				Dejudder::MotionWarp parallelWarp(fixture.Log(), gpu, s);
				Dejudder::MotionWarp serialWarp(fixture.Log(), gpu, s);
				parallelWarp.SetSerialSearchForTest(false);
				serialWarp.SetSerialSearchForTest(true);
				parallelWarp.SetPassTiming(true, true);
				serialWarp.SetPassTiming(true, true);

				// Pair 1 (f0 -> f1, use_temporal == 0)
				Assert::IsTrue(parallelWarp.Estimate(f0.planes, f1.planes), L"GEM-139 single-global parallel pair 1");
				Assert::IsTrue(serialWarp.Estimate(f0.planes, f1.planes), L"GEM-139 single-global serial pair 1");
				Assert::AreEqual(size_t(1), parallelWarp.PassMs().count("VideoProcessor de-judder: block search"),
					L"parallelWarp must dispatch parallel SEARCH_BODY");
				Assert::AreEqual(size_t(1), parallelWarp.PassMs().count("VideoProcessor de-judder: second sweep (11a E1)"),
					L"parallelWarp must dispatch parallel REFINE_BODY");
				Assert::AreEqual(size_t(1), serialWarp.PassMs().count("VideoProcessor de-judder: block search (serial reference)"),
					L"serialWarp must dispatch SEARCH_BODY_SERIAL");
				Assert::AreEqual(size_t(1), serialWarp.PassMs().count("VideoProcessor de-judder: second sweep (11a E1, serial reference)"),
					L"serialWarp must dispatch REFINE_BODY_SERIAL");
				pl_gpu_finish(gpu);

				parPair1Field = DownloadField(gpu, parallelWarp);
				serPair1Field = DownloadField(gpu, serialWarp);
				parPair1Fwd = DownloadField(gpu, parallelWarp, parallelWarp.Forward());
				const DejudderField serPair1Fwd = DownloadField(gpu, serialWarp, serialWarp.Forward());
				const DejudderField parPair1Bwd = DownloadField(gpu, parallelWarp, parallelWarp.Backward());
				const DejudderField serPair1Bwd = DownloadField(gpu, serialWarp, serialWarp.Backward());

				assertBitExact(parPair1Field, serPair1Field, L"GEM-139 globalTwo=false pair 1 Field");
				assertBitExact(parPair1Fwd, serPair1Fwd, L"GEM-139 globalTwo=false pair 1 Forward");
				assertBitExact(parPair1Bwd, serPair1Bwd, L"GEM-139 globalTwo=false pair 1 Backward");
				assertNonTrivialRegions(parPair1Field, L"GEM-139 globalTwo=false pair 1");

				// Pair 2 (f1 -> f2, use_temporal != 0 in both SEARCH_BODY and REFINE_BODY)
				Assert::IsTrue(parallelWarp.Estimate(f1.planes, f2.planes), L"GEM-139 single-global parallel pair 2");
				Assert::IsTrue(serialWarp.Estimate(f1.planes, f2.planes), L"GEM-139 single-global serial pair 2");
				pl_gpu_finish(gpu);

				const DejudderField parPair2Field = DownloadField(gpu, parallelWarp);
				serPair2Field = DownloadField(gpu, serialWarp);
				const DejudderField parPair2Fwd = DownloadField(gpu, parallelWarp, parallelWarp.Forward());
				const DejudderField serPair2Fwd = DownloadField(gpu, serialWarp, serialWarp.Forward());
				const DejudderField parPair2Bwd = DownloadField(gpu, parallelWarp, parallelWarp.Backward());
				const DejudderField serPair2Bwd = DownloadField(gpu, serialWarp, serialWarp.Backward());

				assertBitExact(parPair2Field, serPair2Field, L"GEM-139 globalTwo=false pair 2 Field (temporal)");
				assertBitExact(parPair2Fwd, serPair2Fwd, L"GEM-139 globalTwo=false pair 2 Forward (temporal)");
				assertBitExact(parPair2Bwd, serPair2Bwd, L"GEM-139 globalTwo=false pair 2 Backward (temporal)");
				assertNonTrivialRegions(parPair2Field, L"GEM-139 globalTwo=false pair 2 (temporal)");
			}

			// 2a. globalTwo = true (L1_NSTARTS = 4, ZGROUP = 7) seeded full-field bit-exactness vs serial reference:
			// SEARCH_BODY_SERIAL has only one global candidate (two = settings.globalTwo && settings.globalPrior && !serialSearchForTest).
			// Supplying the matching pan seed on a clean 100%-pan 3-frame 256x128 sequence (so GapSeedHolds() deterministically
			// accepts panSeed with forwardSad, backwardSad << 0.25 * zeroSad) gives both paths the exact same globalVector while
			// parallelWarp compiles and runs the #define GLOBAL_TWO 1 (L1_NSTARTS = 4, ZGROUP = 7) shader with globalSecond = 0
			// across pairs 1 and 2. (f0, f1, f2 stay untouched for Sections 1, 2b, and 3.)
			{
				auto makePurePanFrame = [&](int t) -> DejudderFrame
				{
					return MakeDejudderFrame(kMcW, kMcH,
						[&](int x, int y) -> uint16_t { return DejudderTexture(x - kPanX * t, y - kPanY * t, 1); },
						[&](int x, int y, int c) -> uint16_t { return DejudderTexture(x * 2 - kPanX * t, y - kPanY * t, 7 + c); });
				};
				DejudderGpuPlanes seedF0, seedF1, seedF2;
				UploadDejudderFrame(gpu, makePurePanFrame(0), seedF0);
				UploadDejudderFrame(gpu, makePurePanFrame(1), seedF1);
				UploadDejudderFrame(gpu, makePurePanFrame(2), seedF2);

				const Dejudder::Settings sTwo = makeSettings(true);
				Dejudder::MotionWarp parTwoSeeded(fixture.Log(), gpu, sTwo);
				Dejudder::MotionWarp serSeeded(fixture.Log(), gpu, sTwo);
				parTwoSeeded.SetSerialSearchForTest(false);
				serSeeded.SetSerialSearchForTest(true);
				const float panSeed[2][4] = {
					{ static_cast<float>(kPanX) / 8.0f, static_cast<float>(kPanY) / 8.0f, 0.5f, 1.0f },
					{ -static_cast<float>(kPanX) / 8.0f, -static_cast<float>(kPanY) / 8.0f, 0.5f, 1.0f }
				};

				for (int pair = 0; pair < 2; ++pair)
				{
					const Dejudder::Planes& pa = pair == 0 ? seedF0.planes : seedF1.planes;
					const Dejudder::Planes& pb = pair == 0 ? seedF1.planes : seedF2.planes;
					parTwoSeeded.SetSearchSeed(panSeed);
					serSeeded.SetSearchSeed(panSeed);
					Assert::IsTrue(parTwoSeeded.Estimate(pa, pb), L"GEM-139 globalTwo=true seeded parallel estimate");
					Assert::IsTrue(serSeeded.Estimate(pa, pb), L"GEM-139 globalTwo=true seeded serial estimate");
					Assert::IsTrue(parTwoSeeded.SearchSeedUsed(), L"seeded parallel GLOBAL_TWO must accept pan seed");
					Assert::IsTrue(serSeeded.SearchSeedUsed(), L"seeded serial reference must accept pan seed");
					Assert::IsFalse(parTwoSeeded.SearchSeedRejected(), L"seeded parallel GLOBAL_TWO must not reject clean pan seed");
					Assert::IsFalse(serSeeded.SearchSeedRejected(), L"seeded serial reference must not reject clean pan seed");
					pl_gpu_finish(gpu);

					const std::wstring tag = pair == 0 ? L"GEM-139 globalTwo=true seeded pair 1" : L"GEM-139 globalTwo=true seeded pair 2 (temporal)";
					const DejudderField parField = DownloadField(gpu, parTwoSeeded);
					const DejudderField serField = DownloadField(gpu, serSeeded);
					assertBitExact(parField, serField, (tag + L" Field").c_str());
					assertBitExact(DownloadField(gpu, parTwoSeeded, parTwoSeeded.Forward()),
						DownloadField(gpu, serSeeded, serSeeded.Forward()), (tag + L" Forward").c_str());
					assertBitExact(DownloadField(gpu, parTwoSeeded, parTwoSeeded.Backward()),
						DownloadField(gpu, serSeeded, serSeeded.Backward()), (tag + L" Backward").c_str());

					int seededPanBlocks = 0;
					for (int by = 0; by < parField.blocksY; ++by)
						for (int bx = 0; bx < parField.blocksX; ++bx)
							if (parField.trusted(bx, by) &&
								std::fabs(parField.vx(bx, by) - static_cast<float>(kPanX)) <= 0.5f &&
								std::fabs(parField.vy(bx, by) - static_cast<float>(kPanY)) <= 0.5f)
								++seededPanBlocks;
					Assert::IsTrue(seededPanBlocks >= 64,
						(tag + L": expected >= 64 trusted pan (+10,+4) blocks on clean pan sequence, got " +
							std::to_wstring(seededPanBlocks)).c_str());
				}
			}

			// 2b. globalTwo = true (L1_NSTARTS = 4, ZGROUP = 7) unseeded on the exact same 3-frame sequence:
			// globalSecond is active (have_g2 == true, exercising L1 k == 3, l1_tab[3*361..], pk_s[3*NT..], ZGROUP = 7);
			// all three regions must track cleanly and interior true-match blocks must remain bit-exact with serialWarp.
			{
				const Dejudder::Settings sTwo = makeSettings(true);
				Dejudder::MotionWarp parTwoUnseeded(fixture.Log(), gpu, sTwo);
				parTwoUnseeded.SetSerialSearchForTest(false);

				const std::pair<int, int> interiorCheckBlocks[] = {
					{ 9, 1 }, { 10, 1 }, { 11, 1 }, { 12, 1 },   // background pan (+10, +4) interior
					{ 4, 3 },                                    // Object 1 (-6, +6) interior
					{ 10, 5 }                                    // Object 2 (+5, +3) half-pel interior
				};
				for (int pair = 0; pair < 2; ++pair)
				{
					const Dejudder::Planes& pa = pair == 0 ? f0.planes : f1.planes;
					const Dejudder::Planes& pb = pair == 0 ? f1.planes : f2.planes;
					Assert::IsTrue(parTwoUnseeded.Estimate(pa, pb), L"GEM-139 globalTwo=true unseeded estimate");
					pl_gpu_finish(gpu);
					const DejudderField parField = DownloadField(gpu, parTwoUnseeded);
					const DejudderField& serRef = pair == 0 ? serPair1Field : serPair2Field;
					const std::wstring tag = pair == 0 ? L"GEM-139 globalTwo=true unseeded pair 1" : L"GEM-139 globalTwo=true unseeded pair 2 (temporal)";
					assertNonTrivialRegions(parField, tag.c_str());
					for (const auto& bc : interiorCheckBlocks)
					{
						const size_t base = (static_cast<size_t>(bc.second) * parField.blocksX + bc.first) * 4u;
						for (size_t ch = 0; ch < 4u; ++ch)
						{
							Assert::IsTrue(parField.texels[base + ch] == serRef.texels[base + ch],
								(tag + L": interior block (" + std::to_wstring(bc.first) + L"," + std::to_wstring(bc.second) +
									L") ch=" + std::to_wstring(ch) + L" diverged from serial (" +
									std::to_wstring(parField.texels[base + ch]) + L" vs " +
									std::to_wstring(serRef.texels[base + ch]) + L")").c_str());
						}
					}
				}
			}

			// 2c. Genuine two-global beyond-coarse-range (512x128, top +128 px / bottom -128 px, coarseRange = 3 -> +-96 px):
			// proves GLOBAL_TWO's 4th L1 start (k == 3, l1_tab[3*361..], pk_s[3*NT..], s_centres[++nstarts], ZGROUP = 7)
			// genuinely wins in GEM-139's refactored SEARCH_BODY across pairs 1 & 2, with same-input globalTwo = false negative control.
			{
				constexpr int kTwoW = 512, kTwoH = 128, kSplitY = 64, kBigDx = 128;
				auto makeSplitFrame = [&](int t) -> DejudderFrame
				{
					return MakeDejudderFrame(kTwoW, kTwoH,
						[&](int x, int y) { return y < kSplitY ? DejudderTexture(x - kBigDx * t, y, 3) : DejudderTexture(x + kBigDx * t, y, 9); },
						[&](int x, int y, int c) { return DejudderTexture((x - (y < kSplitY ? kBigDx : -kBigDx) * t / 2) * 2, y, 7 + c); });
				};
				DejudderGpuPlanes t0, t1, t2;
				UploadDejudderFrame(gpu, makeSplitFrame(0), t0);
				UploadDejudderFrame(gpu, makeSplitFrame(1), t1);
				UploadDejudderFrame(gpu, makeSplitFrame(2), t2);

				auto countHalfMatched = [&](const DejudderField& field, bool top) -> std::pair<int, int>
				{
					int matched = 0, total = 0;
					const float want = top ? static_cast<float>(kBigDx) : -static_cast<float>(kBigDx);
					const int by0 = top ? 1 : 5;
					const int by1 = top ? 3 : 7;
					for (int by = by0; by < by1; ++by)
						for (int bx = 9; bx <= 22; ++bx)
						{
							++total;
							if (field.trusted(bx, by) &&
								std::fabs(field.vx(bx, by) - want) <= 0.5f &&
								std::fabs(field.vy(bx, by)) <= 0.5f)
								++matched;
						}
					return { matched, total };
				};

				Dejudder::Settings sTwoOn = makeSettings(true);
				sTwoOn.minTrustedShare = 0.0f;
				Dejudder::MotionWarp twoOn(fixture.Log(), gpu, sTwoOn);
				for (int pair = 0; pair < 2; ++pair)
				{
					Assert::IsTrue(twoOn.Estimate(pair == 0 ? t0.planes : t1.planes, pair == 0 ? t1.planes : t2.planes),
						L"GEM-139 beyond-coarse globalTwo=true estimate");
					pl_gpu_finish(gpu);
					const DejudderField field = DownloadField(gpu, twoOn);
					const auto top = countHalfMatched(field, true);
					const auto bot = countHalfMatched(field, false);
					Assert::IsTrue(top.first * 100 >= top.second * 90 && bot.first * 100 >= bot.second * 90,
						(L"GEM-139 globalTwo=true must track both +128/-128 halves (pair " + std::to_wstring(pair + 1) +
							L": top " + std::to_wstring(top.first) + L"/" + std::to_wstring(top.second) +
							L", bottom " + std::to_wstring(bot.first) + L"/" + std::to_wstring(bot.second) + L")").c_str());
				}

				// Mandatory same-input negative control (Rule 3): globalTwo = false on exact same planes must miss at least one half.
				Dejudder::Settings sTwoOff = makeSettings(false);
				sTwoOff.minTrustedShare = 0.0f;
				Dejudder::MotionWarp twoOff(fixture.Log(), gpu, sTwoOff);
				Assert::IsTrue(twoOff.Estimate(t0.planes, t1.planes), L"GEM-139 beyond-coarse globalTwo=false negative control");
				pl_gpu_finish(gpu);
				const DejudderField offField = DownloadField(gpu, twoOff);
				const auto offTop = countHalfMatched(offField, true);
				const auto offBot = countHalfMatched(offField, false);
				Assert::IsTrue(offTop.first * 100 < offTop.second * 50 || offBot.first * 100 < offBot.second * 50,
					(L"negative control: globalTwo=false must miss at least one beyond-coarse half (top " +
						std::to_wstring(offTop.first) + L"/" + std::to_wstring(offTop.second) +
						L", bottom " + std::to_wstring(offBot.first) + L"/" + std::to_wstring(offBot.second) + L")").c_str());
			}

			// 3. Mandatory same-input negative control (Rule 3): static (f0, f0) on the exact same f0 plane
			// must produce a strictly different Field() and Forward() from moving pair 1 (f0, f1), proving
			// DownloadField + exact texel comparison catches real field differences and is not vacuous.
			{
				Dejudder::MotionWarp staticWarp(fixture.Log(), gpu, makeSettings(false));
				Assert::IsTrue(staticWarp.Estimate(f0.planes, f0.planes), L"negative control static (f0, f0) estimate");
				pl_gpu_finish(gpu);
				const DejudderField staticField = DownloadField(gpu, staticWarp);
				const DejudderField staticFwd = DownloadField(gpu, staticWarp, staticWarp.Forward());
				Assert::IsTrue(parPair1Field.texels != staticField.texels,
					L"negative control: moving pair (f0, f1) Field() must differ from static (f0, f0) Field()");
				Assert::IsTrue(parPair1Fwd.texels != staticFwd.texels,
					L"negative control: moving pair (f0, f1) Forward() must differ from static (f0, f0) Forward()");
			}

			AssertCleanLibplaceboLog(log, "McSearchParallelMatchesSerialSearchBitIdenticallyAcrossSingleAndTwoGlobal");
		}
	};
}
