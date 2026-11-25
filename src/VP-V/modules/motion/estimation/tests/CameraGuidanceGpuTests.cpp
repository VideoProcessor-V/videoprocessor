// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19, phase 2): the matcher's camera guidance on the libplacebo test device
// (WARP unless VP_TEST_GPU=hardware; TargetLutGpuFixture). GpuWarp: run by the coordinator.
// Off = today's field bitwise; narrow window / E1 skip on a pan = the full search's field; an object that
// the guidance wrongly calls camera is found by the escalation; a cut skips the searches and holds.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/global_model/CameraGuidanceGpu.h>

#include <memory>

namespace VideoProcessorTest
{
	TEST_CLASS(CameraGuidanceGpuTests)
	{
		static constexpr int kW = 512, kH = 256, kDx = 12, kDy = 6;

		// An object (blocks [ox0, ox1) x [oy0, oy1) of 16 px) moving by (odx, ody) over a pan of (kDx, kDy).
		static void Frames(pl_gpu gpu, DejudderGpuPlanes& pa, DejudderGpuPlanes& pb, bool object, int kW = CameraGuidanceGpuTests::kW,
			int kH = CameraGuidanceGpuTests::kH, bool still = false)
		{
			const int ox0 = 12 * 16, ox1 = 16 * 16, oy0 = 6 * 16, oy1 = 10 * 16, odx = -10, ody = 4;
			const DejudderFrame a = MakeDejudderFrame(kW, kH,
				[&](int x, int y) {
					if (object && x >= ox0 && x < ox1 && y >= oy0 && y < oy1) return DejudderTexture(x, y, 5);
					return DejudderTexture(x, y);
				},
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(kW, kH,
				[&](int x, int y) {
					const int sx = x - odx, sy = y - ody;
					if (object && sx >= ox0 && sx < ox1 && sy >= oy0 && sy < oy1) return DejudderTexture(sx, sy, 5);
					return still ? DejudderTexture(x, y) : DejudderTexture(x - kDx, y - kDy);
				},
				[still](int x, int y, int c) { return still ? DejudderTexture(x * 2, y, 7 + c) : DejudderTexture((x - kDx / 2) * 2, y - kDy, 7 + c); });
			UploadDejudderFrame(gpu, a, pa);
			UploadDejudderFrame(gpu, b, pb);
		}

		static Dejudder::Settings Settings(bool lowRes = false)
		{
			Dejudder::Settings s = DejudderTestSettings();
			s.lowRes = lowRes;
			s.lowResAll = lowRes;   // as the renderer's default (dejudder_low_res all)
			// The synthetic texture's 4-px detail aliases at the inner's quarter size (match error ~11 codes on the
			// exact vector), so the low-resolution case uses a tolerance that trusts the exact match.
			if (lowRes)
				s.matchTolerance = 16.0f;
			s.globalPrior = true;
			s.estCandidates = true;
			s.estGate = true;
			return s;
		}

		// A pan plan by hand: every block narrow + eligible, seed = prediction = the pan.
		static std::shared_ptr<CameraModel::GuidanceFrame> PanGuidance(int gw, int gh, bool narrow, bool e1)
		{
			auto g = std::make_shared<CameraModel::GuidanceFrame>();
			g->active = true;
			g->narrow = narrow;
			g->e1Skip = e1;
			g->gridW = gw;
			g->gridH = gh;
			g->blockSize = 16;
			g->narrowRadiusPx = 2;
			g->e1AgreePx = 0.5;
			const size_t n = static_cast<size_t>(gw) * gh;
			g->guide.resize(n * 4);
			g->code.assign(n, static_cast<float>(1 + 4 * 1));
			for (size_t i = 0; i < n; ++i)
			{
				g->guide[i * 4 + 0] = static_cast<float>(kDx);
				g->guide[i * 4 + 1] = static_cast<float>(kDy);
				g->guide[i * 4 + 2] = static_cast<float>(kDx);
				g->guide[i * 4 + 3] = static_cast<float>(kDy);
			}
			return g;
		}

		static DejudderField Run(TargetLutGpuFixture& fixture, bool object, std::shared_ptr<CameraModel::GuidanceFrame> guidance,
			Dejudder::MotionWarp::CameraGuidanceUse* use = nullptr, int w = kW, int h = kH, bool lowRes = false, bool still = false, float* counts = nullptr)
		{
			pl_gpu gpu = fixture.Gpu();
			DejudderGpuPlanes a, b;
			Frames(gpu, a, b, object, w, h, still);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, Settings(lowRes));
			// One unguided pair first so the temporal history (previous searches, E4's frame trust) exists.
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
			warp.SetCameraGuidance(guidance);
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
			if (use)
				*use = warp.CameraGuidanceUsed();
			if (counts && warp.CameraGuidanceCounts())
			{
				pl_tex_transfer_params d{};
				d.tex = warp.CameraGuidanceCounts();
				d.ptr = counts;
				Assert::IsTrue(pl_tex_download(gpu, &d), L"the guidance counts are not host-readable");
			}
			DejudderField f = DownloadField(gpu, warp);
			const int mx = f.blocksX / 2, my = f.blocksY / 2;
			char line[200];
			snprintf(line, sizeof(line), "CAMERA_GPU_FIELD %dx%d blocks %dx%d fmt %s centre (%.2f, %.2f, %.2f, %.2f)\n", w, h, f.blocksX, f.blocksY,
				warp.Field()->params.format->name, f.texels[(my * f.blocksX + mx) * 4], f.texels[(my * f.blocksX + mx) * 4 + 1],
				f.texels[(my * f.blocksX + mx) * 4 + 2], f.texels[(my * f.blocksX + mx) * 4 + 3]);
			Logger::WriteMessage(line);
			return f;
		}

	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(GuidanceOffIsTodaysFieldBitwise)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GuidanceOffIsTodaysFieldBitwise)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const DejudderField none = Run(fixture, true, nullptr);
			const DejudderField inactive = Run(fixture, true, std::make_shared<CameraModel::GuidanceFrame>());
			Assert::IsTrue(none.texels == inactive.texels, L"an inactive guidance frame changed the field");
			// An ACTIVE frame that enables nothing goes through the guided code path (cg non-null) and must
			// still bind and dispatch nothing new: the field bitwise as without guidance.
			auto empty = std::make_shared<CameraModel::GuidanceFrame>();
			empty->active = true;
			Dejudder::MotionWarp::CameraGuidanceUse use;
			const DejudderField active = Run(fixture, true, empty, &use);
			Assert::IsFalse(use.narrowPass || use.e1Pass || use.modelGlobal || use.staticFromModel || use.matcherSkipped);
			Assert::IsTrue(none.texels == active.texels, L"an active but empty guidance frame changed the field");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(NarrowWindowAndE1SkipKeepThePanField)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(NarrowWindowAndE1SkipKeepThePanField)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const DejudderField today = Run(fixture, false, nullptr);
			Dejudder::MotionWarp::CameraGuidanceUse use;
			const DejudderField guided = Run(fixture, false, PanGuidance(today.blocksX, today.blocksY, true, true), &use);
			Assert::IsTrue(use.narrowPass && use.e1Pass, L"the guided passes did not run");
			int trustedToday = 0, trustedGuided = 0, differ = 0;
			for (int by = 1; by + 1 < today.blocksY; ++by)
				for (int bx = 1; bx + 1 < today.blocksX; ++bx)
				{
					trustedToday += today.trusted(bx, by) ? 1 : 0;
					trustedGuided += guided.trusted(bx, by) ? 1 : 0;
					if (today.trusted(bx, by) && (std::fabs(today.vx(bx, by) - guided.vx(bx, by)) > 0.5f ||
						std::fabs(today.vy(bx, by) - guided.vy(bx, by)) > 0.5f))
						++differ;
				}
			Logger::WriteMessage(("CAMERA_GPU_GUIDED pan trusted today=" + std::to_string(trustedToday) + " guided=" +
				std::to_string(trustedGuided) + " differ=" + std::to_string(differ) + "\n").c_str());
			Assert::IsTrue(trustedToday > 0, L"the full search must trust interior pan blocks (test setup)");
			Assert::AreEqual(0, differ, L"a guided vector moved more than half a pixel from the full search");
			Assert::IsTrue(trustedGuided >= trustedToday, L"the guidance lost trusted blocks on a pan");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ObjectWronglyCalledCameraIsFoundByTheEscalation)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ObjectWronglyCalledCameraIsFoundByTheEscalation)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const DejudderField today = Run(fixture, true, nullptr);
			const DejudderField guided = Run(fixture, true, PanGuidance(today.blocksX, today.blocksY, true, true));
			int objectBlocks = 0, matched = 0;
			for (int by = 7; by < 9; ++by)          // the object's interior blocks (away from its edges)
				for (int bx = 13; bx < 15; ++bx)
				{
					if (!today.trusted(bx, by))
						continue;
					++objectBlocks;
					if (guided.trusted(bx, by) && std::fabs(today.vx(bx, by) - guided.vx(bx, by)) <= 0.5f &&
						std::fabs(today.vy(bx, by) - guided.vy(bx, by)) <= 0.5f)
						++matched;
				}
			Logger::WriteMessage(("CAMERA_GPU_GUIDED object blocks=" + std::to_string(objectBlocks) + " matched=" +
				std::to_string(matched) + "\n").c_str());
			Assert::IsTrue(objectBlocks > 0, L"the full search did not trust the object (test setup)");
			Assert::AreEqual(objectBlocks, matched, L"the escalation lost the object's motion");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(CutSkipsTheSearchesAndHolds)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CutSkipsTheSearchesAndHolds)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			auto g = std::make_shared<CameraModel::GuidanceFrame>();
			g->active = true;
			g->skipMatcher = true;
			Dejudder::MotionWarp::CameraGuidanceUse use;
			const DejudderField f = Run(fixture, false, g, &use);
			Assert::IsTrue(use.matcherSkipped);
			for (int by = 0; by < f.blocksY; ++by)
				for (int bx = 0; bx < f.blocksX; ++bx)
					Assert::IsFalse(f.trusted(bx, by), L"a block was trusted across a skipped cut");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ModelAsD1KeepsThePanField)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ModelAsD1KeepsThePanField)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const DejudderField today = Run(fixture, false, nullptr);
			auto g = std::make_shared<CameraModel::GuidanceFrame>();
			g->active = true;
			g->useModelGlobal = true;
			g->globalFwd[0] = static_cast<float>(kDx);
			g->globalFwd[1] = static_cast<float>(kDy);
			g->globalBwd[0] = -static_cast<float>(kDx);
			g->globalBwd[1] = -static_cast<float>(kDy);
			Dejudder::MotionWarp::CameraGuidanceUse use;
			const DejudderField guided = Run(fixture, false, g, &use);
			Assert::IsTrue(use.modelGlobal);
			int trusted = 0, differ = 0;
			for (int by = 1; by + 1 < today.blocksY; ++by)
				for (int bx = 1; bx + 1 < today.blocksX; ++bx)
				{
					if (today.trusted(bx, by))
						++trusted;
					if (today.trusted(bx, by) != guided.trusted(bx, by) ||
						(today.trusted(bx, by) && (std::fabs(today.vx(bx, by) - guided.vx(bx, by)) > 0.5f ||
							std::fabs(today.vy(bx, by) - guided.vy(bx, by)) > 0.5f)))
						++differ;
				}
			Assert::IsTrue(trusted > 0, L"the full search must trust interior pan blocks (test setup)");
			Assert::AreEqual(0, differ, L"the model as D1 changed the pan field");
		}
		BEGIN_TEST_METHOD_ATTRIBUTE(StaticFromTheModelKeepsTheStillField)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StaticFromTheModelKeepsTheStillField)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const DejudderField today = Run(fixture, false, nullptr, nullptr, kW, kH, false, true);
			auto g = std::make_shared<CameraModel::GuidanceFrame>();
			g->active = true;
			g->staticScene = true;
			Dejudder::MotionWarp::CameraGuidanceUse use;
			const DejudderField guided = Run(fixture, false, g, &use, kW, kH, false, true);
			Assert::IsTrue(use.staticFromModel, L"E4 from the model did not apply");
			for (int by = 0; by < today.blocksY; ++by)
				for (int bx = 0; bx < today.blocksX; ++bx)
				{
					Assert::AreEqual(today.trusted(bx, by), guided.trusted(bx, by));
					Assert::IsTrue(std::fabs(guided.vx(bx, by)) <= 0.5f && std::fabs(guided.vy(bx, by)) <= 0.5f, L"a still block moved");
				}
		}

		// The low-resolution inner (an input of >= sqrt(2) x 1080 lines searches on the half-size copy):
		// the guidance reaches it at its own pixel scale (cgInputScale 0.5).
		BEGIN_TEST_METHOD_ATTRIBUTE(LowResInnerReceivesTheGuidanceAtItsScale)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LowResInnerReceivesTheGuidanceAtItsScale)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const int w = 768, h = 1600;
			const DejudderField today = Run(fixture, false, nullptr, nullptr, w, h, true);
			Dejudder::MotionWarp::CameraGuidanceUse use;
			float counts[4] = { -2, -2, -2, -2 };
			const DejudderField guided = Run(fixture, false, PanGuidance(today.blocksX, today.blocksY, true, true), &use, w, h, true, false, counts);
			Logger::WriteMessage(("CAMERA_GPU_GUIDED lowres counts narrow " + std::to_string(counts[0]) + "/" + std::to_string(counts[1]) +
				" e1 " + std::to_string(counts[2]) + "/" + std::to_string(counts[3]) + "\n").c_str());
			Assert::IsTrue(counts[0] > 0 && counts[1] > 0, L"no narrow block was accepted in the inner (scale or mapping wrong)");
			Assert::IsTrue(use.narrowPass, L"the inner did not run the narrow pass");
			Assert::IsTrue(use.rejected[0] == 0, L"the inner rejected the guidance");
			int differ = 0, trusted = 0;
			for (int by = 2; by + 2 < today.blocksY; ++by)
				for (int bx = 2; bx + 2 < today.blocksX; ++bx)
					if (today.trusted(bx, by))
					{
						++trusted;
						if (std::fabs(today.vx(bx, by) - guided.vx(bx, by)) > 1.0f || std::fabs(today.vy(bx, by) - guided.vy(bx, by)) > 1.0f)
							++differ;
					}
			Logger::WriteMessage(("CAMERA_GPU_GUIDED lowres trusted=" + std::to_string(trusted) + " differ=" + std::to_string(differ) + "\n").c_str());
			Assert::IsTrue(trusted > 0);
			Assert::AreEqual(0, differ, L"the inner's guided vectors moved from its full search");
		}

		// J#1 (MC-SEARCH GEM-216): hoisting cg_from_mean(o) once per block under CG_FADE must produce the
		// exact same narrow-search and E1-skip outputs as the pre-hoist per-candidate (mf, mt) reference
		// on a fading/dimming pan pair (including half-pel refinement, clamped border blocks, and COST_W),
		// while the exact same fading inputs WITHOUT CG_FADE (plain SAD negative control) escalate.
		BEGIN_TEST_METHOD_ATTRIBUTE(GuidedNarrowAndE1FadeHoistedFromMeanMatchesCpuReferenceBitExactly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GuidedNarrowAndE1FadeHoistedFromMeanMatchesCpuReferenceBitExactly)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			struct LogScope
			{
				TargetLutGpuFixture& f;
				LogScope(TargetLutGpuFixture& fx, std::string* s) : f(fx) { f.CaptureLog(s); }
				LogScope(const LogScope&) = delete;
				LogScope& operator=(const LogScope&) = delete;
				~LogScope() { f.CaptureLog(nullptr); }
			} logScope(fixture, &log);

			struct DispatchGuard
			{
				pl_dispatch d = nullptr;
				explicit DispatchGuard(pl_dispatch dispatchIn) : d(dispatchIn) {}
				DispatchGuard(const DispatchGuard&) = delete;
				DispatchGuard& operator=(const DispatchGuard&) = delete;
				~DispatchGuard() { if (d) pl_dispatch_destroy(&d); }
			} dispatch(pl_dispatch_create(fixture.Log(), gpu));
			Assert::IsNotNull(dispatch.d, L"Could not create libplacebo dispatch");

			struct TexGuard
			{
				pl_gpu g = nullptr;
				pl_tex tex = nullptr;
				TexGuard(pl_gpu gpuIn, pl_tex texIn) : g(gpuIn), tex(texIn) {}
				TexGuard(const TexGuard&) = delete;
				TexGuard& operator=(const TexGuard&) = delete;
				TexGuard(TexGuard&& other) noexcept : g(other.g), tex(other.tex) { other.tex = nullptr; }
				TexGuard& operator=(TexGuard&&) = delete;
				~TexGuard() { if (tex) pl_tex_destroy(g, &tex); }
			};

			// Power-of-two half-resolution dimensions so (p + 0.5) / cg_half_size is IEEE-754 exact for
			// both whole-pixel and half-pixel candidates in cg_to().
			constexpr int kHalfW = 128;
			constexpr int kHalfH = 64;
			constexpr int kBlocksX = kHalfW / 8;
			constexpr int kBlocksY = kHalfH / 8;
			constexpr float kTrueVx = 3.5f;
			constexpr float kTrueVy = -1.5f;
			constexpr float kFadeGain = 0.85f;
			constexpr float kFadeBias = -0.04f;
			constexpr float kCgScale = 0.5f;
			constexpr float kCgRatio = 1.0f;
			constexpr float kCgSign = 1.0f;
			constexpr int kCgRadius = 2;
			// Derived from the synthetic fade pair: zero-mean gain residual + bilinear half-pel curvature
			// is <= ~0.042 on interior blocks, whereas plain SAD faces the luminance-fade floor
			// (1 - 0.85) * 0.58 + 0.04 ~= 0.127 (min plain cost across the grid > 0.104).
			constexpr float kCgTol = 0.06f;
			constexpr float kCgAgree = 2.0f;
			constexpr float kMaxSadDiff = 1e-5f;

			auto clampInt = [](int v, int lo, int hi) -> int { return v < lo ? lo : (v > hi ? hi : v); };
			auto clampFloat = [](float v, float lo, float hi) -> float { return v < lo ? lo : (v > hi ? hi : v); };
			auto synthLuma = [](double x, double y) -> double
			{
				const double v = 0.58
					+ 0.14 * std::sin(0.57 * x + 0.29 * y + 0.4)
					+ 0.11 * std::cos(0.41 * x - 0.63 * y + 1.1)
					+ 0.07 * std::sin(0.019 * x * y + 0.47 * y + 0.2 * x)
					+ 0.05 * std::cos(0.89 * x + 0.71 * y);
				return v < 0.08 ? 0.08 : (v > 0.95 ? 0.95 : v);
			};

			std::vector<float> fromData(static_cast<size_t>(kHalfW) * kHalfH);
			std::vector<float> toData(static_cast<size_t>(kHalfW) * kHalfH);
			for (int y = 0; y < kHalfH; ++y)
				for (int x = 0; x < kHalfW; ++x)
				{
					const size_t idx = static_cast<size_t>(y) * kHalfW + x;
					fromData[idx] = static_cast<float>(synthLuma(static_cast<double>(x), static_cast<double>(y)));
					toData[idx] = static_cast<float>(
						static_cast<double>(kFadeGain) * synthLuma(static_cast<double>(x) - kTrueVx, static_cast<double>(y) - kTrueVy)
						+ static_cast<double>(kFadeBias));
				}

			// Guidance grid: seed intentionally offset from the true half-pel pan (c = (2, -1) vs true (3.5, -1.5))
			// so narrow search must walk both coarse candidates and half-pel neighbours using the hoisted mean.
			// Special blocks exercise non-narrow, ineligible, disagreeing-E1, and high-cost-E1 branches.
			const size_t blockCount = static_cast<size_t>(kBlocksX) * kBlocksY;
			std::vector<float> guideData(blockCount * 4, 0.0f);
			std::vector<float> codeData(blockCount, 0.0f);
			std::vector<float> sweepData(blockCount * 4, 0.0f);
			for (int by = 0; by < kBlocksY; ++by)
				for (int bx = 0; bx < kBlocksX; ++bx)
				{
					const size_t idx = static_cast<size_t>(by) * kBlocksX + bx;
					const int hint = (bx == 2 && by == 2) ? 0 : 1;
					const int eligible = (bx == 3 && by == 2) ? 0 : 1;
					codeData[idx] = static_cast<float>(hint + 4 * eligible);
					guideData[idx * 4 + 0] = 4.0f;            // seed x (source px) -> c.x = 2
					guideData[idx * 4 + 1] = -2.0f;           // seed y (source px) -> c.y = -1
					guideData[idx * 4 + 2] = kTrueVx / kCgScale;
					guideData[idx * 4 + 3] = kTrueVy / kCgScale;

					float swx = kTrueVx;
					float swy = kTrueVy;
					if (bx == 4 && by == 2)
						swx = kTrueVx + 3.0f;                 // outside cg_agree (3.0 > 2.0) -> E1 must escalate
					else if (bx == 5 && by == 2)
						swx = kTrueVx + 1.75f;                // within cg_agree (1.75 <= 2.0), bad match -> E1 must escalate
					sweepData[idx * 4 + 0] = swx;
					sweepData[idx * 4 + 1] = swy;
					sweepData[idx * 4 + 2] = 0.0f;
					sweepData[idx * 4 + 3] = 0.25f + 0.5f * static_cast<float>((bx + by) & 1);
				}

			pl_fmt r32Linear = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt r32Sample = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt rgba32Sample = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt rgba32Dst = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(r32Linear, L"No linear-sampleable R32F format");
			Assert::IsNotNull(r32Sample, L"No sampleable R32F format");
			Assert::IsNotNull(rgba32Sample, L"No sampleable RGBA32F format");
			Assert::IsNotNull(rgba32Dst, L"No storable host-readable RGBA32F format");

			auto makeSampledTex = [&](int w, int h, pl_fmt fmt, const float* initial) -> TexGuard
			{
				pl_tex_params p{};
				p.w = w;
				p.h = h;
				p.format = fmt;
				p.sampleable = true;
				p.initial_data = initial;
				TexGuard tg{ gpu, pl_tex_create(gpu, &p) };
				Assert::IsNotNull(tg.tex, L"Failed to create test texture");
				return tg;
			};

			TexGuard fromTex = makeSampledTex(kHalfW, kHalfH, r32Linear, fromData.data());
			TexGuard toTex = makeSampledTex(kHalfW, kHalfH, r32Linear, toData.data());
			TexGuard guideTex = makeSampledTex(kBlocksX, kBlocksY, rgba32Sample, guideData.data());
			TexGuard codeTex = makeSampledTex(kBlocksX, kBlocksY, r32Sample, codeData.data());
			TexGuard sweepTex = makeSampledTex(kBlocksX, kBlocksY, rgba32Sample, sweepData.data());

			pl_tex_params dstParams{};
			dstParams.w = kBlocksX;
			dstParams.h = kBlocksY;
			dstParams.format = rgba32Dst;
			dstParams.storable = true;
			dstParams.host_readable = true;
			TexGuard dstTex{ gpu, pl_tex_create(gpu, &dstParams) };
			Assert::IsNotNull(dstTex.tex, L"Failed to create dst texture");

			auto sampledDesc = [](const char* name, pl_tex tex, bool linear) -> pl_shader_desc
			{
				pl_shader_desc d{};
				d.desc.name = name;
				d.desc.type = PL_DESC_SAMPLED_TEX;
				d.binding.object = tex;
				d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
				d.binding.sample_mode = linear ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
				return d;
			};
			auto storageDesc = [](const char* name, pl_tex tex) -> pl_shader_desc
			{
				pl_shader_desc d{};
				d.desc.name = name;
				d.desc.type = PL_DESC_STORAGE_IMG;
				d.desc.access = PL_DESC_ACCESS_WRITEONLY;
				d.binding.object = tex;
				return d;
			};

			auto runGpuPass = [&](bool e1, bool fade, bool costWeight) -> std::vector<float>
			{
				float scale = kCgScale;
				float ratio = kCgRatio;
				float sign = kCgSign;
				int radius = kCgRadius;
				float tol = kCgTol;
				float agree = kCgAgree;
				const pl_shader_var vars[6] = {
					{ pl_var_float("cg_scale"), &scale, true },
					{ pl_var_float("cg_ratio"), &ratio, true },
					{ pl_var_float("cg_sign"), &sign, true },
					{ pl_var_int("cg_radius"), &radius, false },
					{ pl_var_float("cg_tol"), &tol, true },
					{ pl_var_float("cg_agree"), &agree, true },
				};
				std::vector<pl_shader_desc> descs = {
					sampledDesc("from_half", fromTex.tex, false),
					sampledDesc("to_half", toTex.tex, true),
					sampledDesc("camera_guide", guideTex.tex, false),
					sampledDesc("camera_code", codeTex.tex, false),
					storageDesc("dst", dstTex.tex),
				};
				if (e1)
					descs.push_back(sampledDesc("sweep", sweepTex.tex, false));

				const std::string header =
					std::string(e1 ? "#define CG_E1 1\n" : "") +
					(fade ? "#define CG_FADE 1\n" : "") +
					(e1 && costWeight ? "#define COST_W 1\n" : "") +
					CameraModel::GuidedPassHeaderGlsl();

				pl_custom_shader custom{};
				custom.description = "CameraGuidanceGpu CG_FADE hoisted from_mean parity test";
				custom.header = header.c_str();
				custom.body = CameraModel::GuidedPassBodyGlsl();
				custom.input = PL_SHADER_SIG_NONE;
				custom.output = PL_SHADER_SIG_NONE;
				custom.descriptors = descs.data();
				custom.num_descriptors = static_cast<int>(descs.size());
				custom.variables = vars;
				custom.num_variables = 6;
				custom.compute = true;
				custom.compute_group_size[0] = 8;
				custom.compute_group_size[1] = 8;

				pl_shader sh = pl_dispatch_begin(dispatch.d);
				if (!pl_shader_custom(sh, &custom))
				{
					pl_dispatch_abort(dispatch.d, &sh);
					Assert::Fail(L"pl_shader_custom rejected guided pass shader");
				}
				pl_dispatch_compute_params compute{};
				compute.shader = &sh;
				compute.dispatch_size[0] = (kBlocksX + 7) / 8;
				compute.dispatch_size[1] = (kBlocksY + 7) / 8;
				compute.dispatch_size[2] = 1;
				Assert::IsTrue(pl_dispatch_compute(dispatch.d, &compute), L"pl_dispatch_compute failed for guided pass");
				pl_gpu_finish(gpu);

				std::vector<float> out(blockCount * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = dstTex.tex;
				dl.ptr = out.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"Failed to download guided pass output");
				return out;
			};

			// Independent pre-hoist CPU reference: recomputes both mf and mt from scratch inside every
			// candidate cost call on the exact same fromData / toData arrays (pre-GEM-216 algorithm).
			auto cpuFrom = [&](int px, int py) -> float
			{
				const int x = clampInt(px, 0, kHalfW - 1);
				const int y = clampInt(py, 0, kHalfH - 1);
				return fromData[static_cast<size_t>(y) * kHalfW + x];
			};
			auto cpuTo = [&](float px, float py) -> float
			{
				const float cx = clampFloat(px, 0.0f, static_cast<float>(kHalfW - 1));
				const float cy = clampFloat(py, 0.0f, static_cast<float>(kHalfH - 1));
				const int x0 = static_cast<int>(std::floor(cx));
				const int y0 = static_cast<int>(std::floor(cy));
				const int x1 = x0 + 1 < kHalfW ? x0 + 1 : kHalfW - 1;
				const int y1 = y0 + 1 < kHalfH ? y0 + 1 : kHalfH - 1;
				const float fx = cx - static_cast<float>(x0);
				const float fy = cy - static_cast<float>(y0);
				const auto at = [&](int x, int y) { return toData[static_cast<size_t>(y) * kHalfW + x]; };
				const float r0 = at(x0, y0) + fx * (at(x1, y0) - at(x0, y0));
				const float r1 = at(x0, y1) + fx * (at(x1, y1) - at(x0, y1));
				return r0 + fy * (r1 - r0);
			};
			auto cpuPreHoistCost = [&](int ox, int oy, float vx, float vy, bool fade) -> float
			{
				float mf = 0.0f;
				float mt = 0.0f;
				if (fade)
				{
					for (int y = -2; y < 10; ++y)
						for (int x = -2; x < 10; ++x)
						{
							const int px = ox + x;
							const int py = oy + y;
							mf += cpuFrom(px, py);
							mt += cpuTo(static_cast<float>(px) + vx, static_cast<float>(py) + vy);
						}
					mf /= 144.0f;
					mt /= 144.0f;
				}
				float s = 0.0f;
				for (int y = -2; y < 10; ++y)
					for (int x = -2; x < 10; ++x)
					{
						const int px = ox + x;
						const int py = oy + y;
						const float fv = cpuFrom(px, py);
						const float tv = cpuTo(static_cast<float>(px) + vx, static_cast<float>(py) + vy);
						s += fade ? std::fabs((fv - mf) - (tv - mt)) : std::fabs(fv - tv);
					}
				return s / 144.0f;
			};

			auto runCpuRef = [&](bool e1, bool fade, bool costWeight) -> std::vector<float>
			{
				std::vector<float> out(blockCount * 4, 0.0f);
				for (int by = 0; by < kBlocksY; ++by)
					for (int bx = 0; bx < kBlocksX; ++bx)
					{
						const size_t idx = static_cast<size_t>(by) * kBlocksX + bx;
						const int gx = clampInt(static_cast<int>(std::floor((static_cast<float>(bx) + 0.5f) * kCgRatio)), 0, kBlocksX - 1);
						const int gy = clampInt(static_cast<int>(std::floor((static_cast<float>(by) + 0.5f) * kCgRatio)), 0, kBlocksY - 1);
						const size_t gidx = static_cast<size_t>(gy) * kBlocksX + gx;
						const int code = static_cast<int>(codeData[gidx] + 0.5f);
						const int hint = code % 4;
						const int eligible = code / 4;
						const int ox = bx * 8;
						const int oy = by * 8;

						float rx = 0.0f, ry = 0.0f, rz = -1.0f, rw = 0.0f;
						if (e1)
						{
							const float swx = sweepData[idx * 4 + 0];
							const float swy = sweepData[idx * 4 + 1];
							const float sww = sweepData[idx * 4 + 3];
							const float predx = kCgSign * guideData[gidx * 4 + 2] * kCgScale;
							const float predy = kCgSign * guideData[gidx * 4 + 3] * kCgScale;
							const float dist = std::hypot(swx - predx, swy - predy);
							if (eligible == 1 && dist <= kCgAgree)
							{
								const float s = cpuPreHoistCost(ox, oy, swx, swy, fade);
								if (s <= kCgTol)
								{
									rx = swx;
									ry = swy;
									rz = s;
									rw = costWeight ? sww : 1.0f;
								}
							}
						}
						else if (hint == 1)
						{
							const int cx = static_cast<int>(std::floor(kCgSign * guideData[gidx * 4 + 0] * kCgScale + 0.5f));
							const int cy = static_cast<int>(std::floor(kCgSign * guideData[gidx * 4 + 1] * kCgScale + 0.5f));
							float bestX = static_cast<float>(cx - kCgRadius);
							float bestY = static_cast<float>(cy - kCgRadius);
							float bestS = 3.0e38f;
							for (int dy = -kCgRadius; dy <= kCgRadius; ++dy)
								for (int dx = -kCgRadius; dx <= kCgRadius; ++dx)
								{
									const float vx = static_cast<float>(cx + dx);
									const float vy = static_cast<float>(cy + dy);
									const float s = cpuPreHoistCost(ox, oy, vx, vy, fade);
									if (s < bestS) { bestS = s; bestX = vx; bestY = vy; }
								}
							const float baseX = bestX;
							const float baseY = bestY;
							for (int dy = -1; dy <= 1; ++dy)
								for (int dx = -1; dx <= 1; ++dx)
								{
									if (dx == 0 && dy == 0) continue;
									const float vx = baseX + 0.5f * static_cast<float>(dx);
									const float vy = baseY + 0.5f * static_cast<float>(dy);
									const float s = cpuPreHoistCost(ox, oy, vx, vy, fade);
									if (s < bestS) { bestS = s; bestX = vx; bestY = vy; }
								}
							if (bestS <= kCgTol)
							{
								rx = bestX;
								ry = bestY;
								rz = bestS;
								rw = 1.0f;
							}
						}
						out[idx * 4 + 0] = rx;
						out[idx * 4 + 1] = ry;
						out[idx * 4 + 2] = rz;
						out[idx * 4 + 3] = rw;
					}
				return out;
			};

			auto verifyAgainstCpu = [&](const char* label, const std::vector<float>& gpuOut, const std::vector<float>& cpuOut,
				int& outAccepted, int& outBorderAccepted, float& outMaxAbsDiff)
			{
				outAccepted = 0;
				outBorderAccepted = 0;
				outMaxAbsDiff = 0.0f;
				for (int by = 0; by < kBlocksY; ++by)
					for (int bx = 0; bx < kBlocksX; ++bx)
					{
						const size_t i = (static_cast<size_t>(by) * kBlocksX + bx) * 4;
						const bool gpuAcc = gpuOut[i + 2] >= 0.0f;
						const bool cpuAcc = cpuOut[i + 2] >= 0.0f;
						if (gpuAcc)
						{
							++outAccepted;
							if (bx == 0 || by == 0 || bx == kBlocksX - 1 || by == kBlocksY - 1)
								++outBorderAccepted;
						}
						const float dx = std::fabs(gpuOut[i + 0] - cpuOut[i + 0]);
						const float dy = std::fabs(gpuOut[i + 1] - cpuOut[i + 1]);
						const float dz = std::fabs(gpuOut[i + 2] - cpuOut[i + 2]);
						const float dw = std::fabs(gpuOut[i + 3] - cpuOut[i + 3]);
						const float blockDiff = (dx > dy ? dx : dy) > (dz > dw ? dz : dw)
							? (dx > dy ? dx : dy) : (dz > dw ? dz : dw);
						if (blockDiff > outMaxAbsDiff)
							outMaxAbsDiff = blockDiff;
						if (gpuAcc != cpuAcc || gpuOut[i + 0] != cpuOut[i + 0] || gpuOut[i + 1] != cpuOut[i + 1] ||
							gpuOut[i + 3] != cpuOut[i + 3] || dz > kMaxSadDiff)
						{
							char msg[256];
							snprintf(msg, sizeof(msg),
								"%s mismatch at block (%d,%d): gpu=(%.6f,%.6f,%.6f,%.6f) cpu=(%.6f,%.6f,%.6f,%.6f) dz=%.7f\n",
								label, bx, by,
								gpuOut[i + 0], gpuOut[i + 1], gpuOut[i + 2], gpuOut[i + 3],
								cpuOut[i + 0], cpuOut[i + 1], cpuOut[i + 2], cpuOut[i + 3], dz);
							Logger::WriteMessage(msg);
							Assert::Fail(L"GPU guided pass diverged from pre-hoist CPU reference");
						}
					}
			};

			// 1. Narrow search with CG_FADE (hoisted cg_from_mean across 25 coarse + 8 half-pel candidates).
			const std::vector<float> narrowFadeGpu = runGpuPass(false, true, false);
			const std::vector<float> narrowFadeCpu = runCpuRef(false, true, false);
			int narrowFadeAcc = 0, narrowFadeBorderAcc = 0;
			float narrowFadeDiff = 0.0f;
			verifyAgainstCpu("narrow+CG_FADE", narrowFadeGpu, narrowFadeCpu, narrowFadeAcc, narrowFadeBorderAcc, narrowFadeDiff);
			Assert::IsTrue(narrowFadeBorderAcc > 0, L"CG_FADE narrow search must also accept clamped border blocks");
			Assert::IsTrue(narrowFadeGpu[(static_cast<size_t>(2) * kBlocksX + 2) * 4 + 2] < 0.0f,
				L"hint==0 block must escalate in narrow search");
			int narrowInteriorExact = 0;
			for (int by = 1; by + 1 < kBlocksY; ++by)
				for (int bx = 1; bx + 1 < kBlocksX; ++bx)
				{
					if (bx == 2 && by == 2) continue;
					const size_t i = (static_cast<size_t>(by) * kBlocksX + bx) * 4;
					Assert::IsTrue(narrowFadeGpu[i + 2] >= 0.0f, L"Interior fading pan block must be accepted under CG_FADE");
					Assert::IsTrue(narrowFadeGpu[i + 0] == kTrueVx && narrowFadeGpu[i + 1] == kTrueVy,
						L"CG_FADE narrow search must recover exact half-pel pan vector");
					++narrowInteriorExact;
				}
			Assert::AreEqual((kBlocksX - 2) * (kBlocksY - 2) - 1, narrowInteriorExact);

			// 2. Mandatory same-input negative control (Rule 3): narrow search WITHOUT CG_FADE (plain SAD).
			const std::vector<float> narrowPlainGpu = runGpuPass(false, false, false);
			const std::vector<float> narrowPlainCpu = runCpuRef(false, false, false);
			int narrowPlainAcc = 0, narrowPlainBorderAcc = 0;
			float narrowPlainDiff = 0.0f;
			verifyAgainstCpu("narrow+plain", narrowPlainGpu, narrowPlainCpu, narrowPlainAcc, narrowPlainBorderAcc, narrowPlainDiff);
			Assert::AreEqual(0, narrowPlainAcc,
				L"Negative control failed: plain SAD without CG_FADE must escalate on the fading pan pair");

			// 3. E1 skip with CG_FADE + COST_W, and E1 skip with CG_FADE without COST_W.
			const std::vector<float> e1FadeWGpu = runGpuPass(true, true, true);
			const std::vector<float> e1FadeWCpu = runCpuRef(true, true, true);
			int e1FadeWAcc = 0, e1FadeWBorderAcc = 0;
			float e1FadeWDiff = 0.0f;
			verifyAgainstCpu("E1+CG_FADE+COST_W", e1FadeWGpu, e1FadeWCpu, e1FadeWAcc, e1FadeWBorderAcc, e1FadeWDiff);
			Assert::IsTrue(e1FadeWBorderAcc > 0, L"CG_FADE E1 skip must also accept clamped border blocks");
			Assert::IsTrue(e1FadeWGpu[(static_cast<size_t>(2) * kBlocksX + 2) * 4 + 2] >= 0.0f,
				L"hint==0, eligible==1 block must still be accepted by E1 under CG_FADE");
			Assert::IsTrue(e1FadeWGpu[(static_cast<size_t>(3) * kBlocksX + 2) * 4 + 2] < 0.0f,
				L"eligible==0 block must escalate in E1");
			Assert::IsTrue(e1FadeWGpu[(static_cast<size_t>(4) * kBlocksX + 2) * 4 + 2] < 0.0f,
				L"sweep vector outside cg_agree must escalate in E1");
			Assert::IsTrue(e1FadeWGpu[(static_cast<size_t>(5) * kBlocksX + 2) * 4 + 2] < 0.0f,
				L"agreeing sweep vector exceeding cg_tol must escalate in E1");

			const std::vector<float> e1FadeNoWGpu = runGpuPass(true, true, false);
			const std::vector<float> e1FadeNoWCpu = runCpuRef(true, true, false);
			int e1FadeNoWAcc = 0, e1FadeNoWBorderAcc = 0;
			float e1FadeNoWDiff = 0.0f;
			verifyAgainstCpu("E1+CG_FADE", e1FadeNoWGpu, e1FadeNoWCpu, e1FadeNoWAcc, e1FadeNoWBorderAcc, e1FadeNoWDiff);
			Assert::AreEqual(e1FadeWAcc, e1FadeNoWAcc, L"COST_W must not change E1 acceptance count");

			// 4. Mandatory same-input negative control (Rule 3): E1 skip WITHOUT CG_FADE (plain SAD).
			const std::vector<float> e1PlainGpu = runGpuPass(true, false, true);
			const std::vector<float> e1PlainCpu = runCpuRef(true, false, true);
			int e1PlainAcc = 0, e1PlainBorderAcc = 0;
			float e1PlainDiff = 0.0f;
			verifyAgainstCpu("E1+plain", e1PlainGpu, e1PlainCpu, e1PlainAcc, e1PlainBorderAcc, e1PlainDiff);
			Assert::AreEqual(0, e1PlainAcc,
				L"Negative control failed: E1 plain SAD without CG_FADE must escalate on the fading pan pair");

			char summary[256];
			snprintf(summary, sizeof(summary),
				"CAMERA_GPU_CG_FADE_HOIST narrowFade=%d (border=%d maxDiff=%.2e) narrowPlain=%d "
				"e1FadeW=%d (border=%d maxDiff=%.2e) e1FadeNoW=%d (maxDiff=%.2e) e1Plain=%d\n",
				narrowFadeAcc, narrowFadeBorderAcc, narrowFadeDiff, narrowPlainAcc,
				e1FadeWAcc, e1FadeWBorderAcc, e1FadeWDiff, e1FadeNoWAcc, e1FadeNoWDiff, e1PlainAcc);
			Logger::WriteMessage(summary);
			AssertCleanLibplaceboLog(log, "GuidedNarrowAndE1FadeHoistedFromMeanMatchesCpuReferenceBitExactly");
		}
	};
}
