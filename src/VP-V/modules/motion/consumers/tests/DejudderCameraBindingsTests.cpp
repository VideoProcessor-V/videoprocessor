// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/consumers/DejudderCameraBindings.h>
#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		class FakeCameraLink : public CameraLink::ICameraLink
		{
		public:
			mutable int queries = 0;
			std::map<uint64_t, std::pair<std::shared_ptr<const CameraModel::CameraMotion>, bool>> records;

			void Put(uint64_t seq, std::shared_ptr<const CameraModel::CameraMotion> rec, bool smoothed)
			{
				records[seq] = std::make_pair(std::move(rec), smoothed);
			}

			std::shared_ptr<const CameraModel::CameraMotion> SealedRecord(uint64_t seq) const override
			{
				return CausalRecord(seq);
			}
			std::shared_ptr<const CameraModel::CameraMotion> CausalRecord(uint64_t seq) const override
			{
				auto it = records.find(seq);
				return it == records.end() ? nullptr : it->second.first;
			}
			std::shared_ptr<const CameraModel::CameraMotion> SmoothedRecord(uint64_t seq, bool* smoothedOut) const override
			{
				++queries;
				auto it = records.find(seq);
				if (it == records.end())
				{
					if (smoothedOut) *smoothedOut = false;
					return nullptr;
				}
				if (smoothedOut) *smoothedOut = it->second.second;
				return it->second.first;
			}
			bool FitEntryOf(uint64_t, std::shared_ptr<const CameraModel::CameraMotion>& rec) const override
			{
				rec.reset();
				return false;
			}
			bool LabelsFor(uint64_t, CameraLink::CameraConsumer, CameraLink::CameraLabelsView& out) override
			{
				out = CameraLink::CameraLabelsView();
				return false;
			}
			std::shared_ptr<const CameraModel::GuidanceFrame> GuideFor(uint64_t) override { return nullptr; }
		};

		std::shared_ptr<CameraModel::CameraMotion> MakeSimilarityRecord(uint64_t seq, double scale, double angleRad,
			double tx, double ty, double confidence, bool smoothed, int gridW = 8, int gridH = 4, double blockSize = 16.0)
		{
			auto m = std::make_shared<CameraModel::CameraMotion>();
			m->seq = seq;
			m->pairFrom = seq > 0 ? seq - 1 : 0;
			m->pairTo = seq;
			m->pairSeconds = 1.0 / 24.0;
			m->valid = true;
			m->reason = CameraModel::Reason::Ok;
			m->confidence = confidence;
			m->picture = { 0, 0, static_cast<int>(gridW * blockSize), static_cast<int>(gridH * blockSize) };
			CameraModel::Params p;
			p.type = CameraModel::ModelType::Similarity;
			const double c = scale * std::cos(angleRad), s = scale * std::sin(angleRad);
			p.h[0] = c;   p.h[1] = -s;  p.h[2] = tx;
			p.h[3] = s;   p.h[4] = c;   p.h[5] = ty;
			p.h[6] = 0.0; p.h[7] = 0.0; p.h[8] = 1.0;
			m->params = p;
			m->smoothed = smoothed;
			if (smoothed)
				m->smoothedParams = p;
			auto bd = std::make_shared<CameraModel::BlockData>();
			bd->gridW = gridW;
			bd->gridH = gridH;
			bd->blockSize = blockSize;
			bd->label.assign(static_cast<size_t>(gridW) * static_cast<size_t>(gridH),
				static_cast<uint8_t>(CameraModel::Label::Camera));
			m->blocks = bd;
			return m;
		}
	}

	TEST_CLASS(DejudderCameraBindingsTests)
	{
	public:
		TEST_METHOD(StrictPolicyMatchesCameraPhaseAndAccessorDejudderPreset)
		{
			const CameraModel::Policy phasePol = CameraPhase::StrictPolicy(true);
			const CameraModel::Policy accPol = CameraModel::PolicyFor(CameraModel::Consumer::DejudderWarp);
			Assert::AreEqual(0.85, phasePol.threshold, 1e-12);
			Assert::AreEqual(0.70, phasePol.floor, 1e-12);
			Assert::AreEqual(0.05, phasePol.blendWidth, 1e-12);
			Assert::AreEqual(0.20, phasePol.rateLimit, 1e-12);
			Assert::AreEqual(accPol.threshold, phasePol.threshold, 1e-12);
			Assert::AreEqual(accPol.floor, phasePol.floor, 1e-12);
			Assert::AreEqual(accPol.blendWidth, phasePol.blendWidth, 1e-12);
			Assert::AreEqual(accPol.rateLimit, phasePol.rateLimit, 1e-12);
		}

		TEST_METHOD(OffSwitchIsByteIdenticalAndQueriesNothing)
		{
			FakeCameraLink link;
			link.Put(1, MakeSimilarityRecord(1, 1.02, 0.0, 12.0, -4.0, 0.96, true), true);

			VpMotion::DejudderCameraState st;
			Assert::IsFalse(st.config.enabled, L"camera_use_dejudder defaults to OFF");
			const VpMotion::DejudderCameraFrame f = st.Prepare(&link, 1, 0.5);
			Assert::IsFalse(f.active);
			Assert::IsFalse(f.wanted);
			Assert::IsFalse(f.late);
			Assert::AreEqual(0, link.queries, L"off switch never queries ICameraLink");
			Assert::AreEqual(std::string(), st.StatusSuffix(), L"off switch emits empty status suffix");

			float hx = 0.0f, hy = 0.0f;
			st.ApplyHiccup(f, 10.0, -5.0, 3.25f, -1.75f, hx, hy);
			Assert::AreEqual(3.25f, hx);
			Assert::AreEqual(-1.75f, hy);

			const double rOff = st.ApplyRollOffRA(f, 20.0);
			Assert::AreEqual(VpMotion::detail::RollOffScale(20.0, st.config.rollLo, st.config.rollHi, st.config.rollFloor), rOff, 1e-12);

			const std::vector<float> mx(32, 2.5f), my(32, -1.25f);
			const std::vector<uint8_t> tr(32, 1);
			std::vector<float> ox, oy;
			st.ApplyDejudderPhaseGrid(f, 8, 4, 16.0, mx, my, tr, ox, oy);
			Assert::AreEqual(0, std::memcmp(mx.data(), ox.data(), mx.size() * sizeof(float)));
			Assert::AreEqual(0, std::memcmp(my.data(), oy.data(), my.size() * sizeof(float)));

			st.config.objectStrength = 0.5;
			float sx = 0.0f, sy = 0.0f;
			st.ApplyObjectSplitRB(f, 0.0, 0.0, 7.5f, 2.5f, sx, sy);
			Assert::AreEqual(7.5f, sx);
			Assert::AreEqual(2.5f, sy);
		}

		TEST_METHOD(AbsentAndLateP9FallBackVisiblyWithUncompensatedOutput)
		{
			FakeCameraLink link;
			VpMotion::DejudderCameraState st;
			st.config.enabled = true;

			// 1. Absent record -> late fallback + visible status.
			const VpMotion::DejudderCameraFrame fMissing = st.Prepare(&link, 1, 0.5);
			Assert::IsFalse(fMissing.active);
			Assert::IsTrue(fMissing.late);
			Assert::AreEqual(uint64_t(1), st.stats.lateFrames);
			Assert::IsTrue(st.StatusSuffix().find("camera late") != std::string::npos);

			// 2. Degenerate model (NaN homography) -> refused visibly, never warps.
			auto bad = MakeSimilarityRecord(2, 1.0, 0.0, 8.0, 0.0, 0.98, true);
			bad->smoothedParams.h[0] = std::numeric_limits<double>::quiet_NaN();
			link.Put(2, bad, true);
			const VpMotion::DejudderCameraFrame fBad = st.Prepare(&link, 2, 0.5);
			Assert::IsFalse(fBad.active);
			Assert::IsTrue(fBad.late);
			Assert::AreEqual(uint64_t(2), st.stats.lateFrames);

			// 3. Confidence below strict floor (0.70) -> refused immediately.
			link.Put(3, MakeSimilarityRecord(3, 1.0, 0.0, 8.0, 0.0, 0.65, true), true);
			const VpMotion::DejudderCameraFrame fLow = st.Prepare(&link, 3, 0.5);
			Assert::IsFalse(fLow.active);
			Assert::IsTrue(fLow.late);
			Assert::AreEqual(uint64_t(3), st.stats.lateFrames);

			// Measured vectors pass through untouched on late frames.
			float hx = 0.0f, hy = 0.0f;
			st.ApplyHiccup(fLow, 0.0, 0.0, 4.0f, 1.5f, hx, hy);
			Assert::AreEqual(4.0f, hx);
			Assert::AreEqual(1.5f, hy);
		}

		TEST_METHOD(ShortQueueCausalP7WorksWithVisibleCausalNote)
		{
			FakeCameraLink link;
			for (uint64_t s = 1; s <= 6; ++s)
				link.Put(s, MakeSimilarityRecord(s, 1.0, 0.0, 10.0, 0.0, 0.96, false), false);

			VpMotion::DejudderCameraState st;
			st.config.enabled = true;
			VpMotion::DejudderCameraFrame f;
			for (uint64_t s = 1; s <= 6; ++s)
				f = st.Prepare(&link, s, 0.5);
			Assert::IsTrue(f.active);
			Assert::IsFalse(f.smoothed);
			Assert::AreEqual(uint64_t(6), st.stats.causalFrames);
			Assert::IsTrue(st.StatusSuffix().find(CameraLink::kCausalShortQueueText) != std::string::npos,
				L"causal short-queue mode is visible in status suffix");
		}

		TEST_METHOD(StageOrderHiccupThenRAThenDejudderThenRBIsDeterministic)
		{
			FakeCameraLink link;
			// Pure zoom (scale = 1.04) + pan (tx = 12, ty = -4) with high confidence 0.96, P9 smoothed.
			// Block (0, 0) is marked Object so stage 3 leaves it untouched while camera blocks use log-scale zoom phase.
			for (uint64_t s = 1; s <= 6; ++s)
			{
				auto rec = MakeSimilarityRecord(s, 1.04, 0.0, 12.0, -4.0, 0.96, true, 8, 4, 16.0);
				auto bd = std::make_shared<CameraModel::BlockData>(*rec->blocks);
				bd->label[0] = static_cast<uint8_t>(CameraModel::Label::Object);
				rec->blocks = bd;
				link.Put(s, rec, true);
			}

			VpMotion::DejudderCameraState st;
			st.config.enabled = true;
			st.config.rollLo = 8.0;
			st.config.rollHi = 20.0;
			st.config.rollFloor = 0.25;
			st.config.objectStrength = 0.5;

			// Ramp blend weight to 1.0 over 5 frames (rateLimit = 0.2 per frame).
			VpMotion::DejudderCameraFrame f;
			for (uint64_t s = 1; s <= 6; ++s)
				f = st.Prepare(&link, s, 0.5);
			Assert::IsTrue(f.active);
			Assert::IsTrue(f.smoothed);
			Assert::AreEqual(1.0, f.weight, 1e-9);

			// 1. Hiccup: at centre (0,0), phase 0.5 gives exact half translation (6.0, -2.0).
			float hx = 0.0f, hy = 0.0f;
			st.ApplyHiccup(f, 0.0, 0.0, 1.0f, 1.0f, hx, hy);
			Assert::AreEqual(6.0f, hx, 1e-4f);
			Assert::AreEqual(-2.0f, hy, 1e-4f);

			// 2. R-A: smoothed camera speed (~12.7 px) lies between rollLo=8 and rollHi=20, so roll-off < 1 even if measured speed = 0.
			const double raScale = st.ApplyRollOffRA(f, 0.0);
			Assert::IsTrue(raScale < 0.95 && raScale > 0.25, L"R-A roll-off responds to smoothed camera speed");

			// 3. De-judder phase grid: camera blocks match log-scale zoom at phase 0.5; object block (0) keeps measured vector.
			const std::vector<float> mx(32, -9.0f), my(32, 7.0f);
			const std::vector<uint8_t> tr(32, 1);
			std::vector<float> ox, oy;
			st.ApplyDejudderPhaseGrid(f, 8, 4, 16.0, mx, my, tr, ox, oy);
			Assert::AreEqual(-9.0f, ox[0], 1e-6f, L"object block keeps measured X");
			Assert::AreEqual(7.0f, oy[0], 1e-6f, L"object block keeps measured Y");
			double cx1 = 0.0, cy1 = 0.0;
			CameraModel::BlockCentre(1, 0, 16.0, f.record->picture, &cx1, &cy1);
			const double halfZoom = std::exp(0.5 * std::log(1.04));
			const double expectX1 = (halfZoom - 1.0) * cx1 + 6.0;
			const double expectY1 = (halfZoom - 1.0) * cy1 - 2.0;
			Assert::AreEqual(static_cast<float>(expectX1), ox[1], 1e-4f, L"camera block uses log-scale zoom interpolation");
			Assert::AreEqual(static_cast<float>(expectY1), oy[1], 1e-4f);

			// 4. R-B object split: at centre (0,0), camera disp = (6, -2), measured object = (-4, -2), objectStrength = 0.5 -> (1, -2).
			float rbx = 0.0f, rby = 0.0f;
			st.ApplyObjectSplitRB(f, 0.0, 0.0, -4.0f, -2.0f, rbx, rby);
			Assert::AreEqual(1.0f, rbx, 1e-4f);
			Assert::AreEqual(-2.0f, rby, 1e-4f);

			// Exact required stage order recorded.
			Assert::AreEqual(size_t(4), st.lastStageOrder.size());
			Assert::AreEqual(int(VpMotion::DejudderCameraStage::Hiccup), int(st.lastStageOrder[0]));
			Assert::AreEqual(int(VpMotion::DejudderCameraStage::RollOffRA), int(st.lastStageOrder[1]));
			Assert::AreEqual(int(VpMotion::DejudderCameraStage::DejudderWarp), int(st.lastStageOrder[2]));
			Assert::AreEqual(int(VpMotion::DejudderCameraStage::ObjectSplitRB), int(st.lastStageOrder[3]));
		}

		TEST_METHOD(RateLimitRampsWeightGraduallyAndHardFloorDropsImmediately)
		{
			FakeCameraLink link;
			for (uint64_t s = 1; s <= 4; ++s)
				link.Put(s, MakeSimilarityRecord(s, 1.0, 0.0, 10.0, 0.0, 0.96, true), true);
			link.Put(5, MakeSimilarityRecord(5, 1.0, 0.0, 10.0, 0.0, 0.69, true), true);   // below strict floor 0.70

			VpMotion::DejudderCameraState st;
			st.config.enabled = true;
			const auto f1 = st.Prepare(&link, 1, 0.5);
			const auto f2 = st.Prepare(&link, 2, 0.5);
			const auto f3 = st.Prepare(&link, 3, 0.5);
			Assert::AreEqual(0.20, f1.weight, 1e-9, L"strict rateLimit = 0.2 per frame");
			Assert::AreEqual(0.40, f2.weight, 1e-9);
			Assert::AreEqual(0.60, f3.weight, 1e-9);

			const auto f5 = st.Prepare(&link, 5, 0.5);
			Assert::IsFalse(f5.active, L"below strict floor 0.70 drops immediately to 0 (no rate limit)");
			Assert::AreEqual(0.0, st.stats.lastWeight, 1e-9);
			Assert::IsTrue(f5.late);
		}
	};
}
