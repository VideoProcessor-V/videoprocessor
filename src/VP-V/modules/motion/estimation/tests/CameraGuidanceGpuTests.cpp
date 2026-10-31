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
			Dejudder::MotionWarp::CameraGuidanceUse* use = nullptr, int w = kW, int h = kH, bool lowRes = false, bool still = false)
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
			return DownloadField(gpu, warp);
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
			int differ = 0;
			for (int by = 1; by + 1 < today.blocksY; ++by)
				for (int bx = 1; bx + 1 < today.blocksX; ++bx)
					if (today.trusted(bx, by) != guided.trusted(bx, by) ||
						(today.trusted(bx, by) && (std::fabs(today.vx(bx, by) - guided.vx(bx, by)) > 0.5f ||
							std::fabs(today.vy(bx, by) - guided.vy(bx, by)) > 0.5f)))
						++differ;
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
			const DejudderField guided = Run(fixture, false, PanGuidance(today.blocksX, today.blocksY, true, true), &use, w, h, true);
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
	};
}
