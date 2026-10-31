// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19): the kCamera group of the frame analysis store. CPU only.
#include "pch.h"
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <memory>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(CameraStoreTests)
	{
	public:
		static FrameAnalysis::Identity Id(uint64_t seq)
		{
			FrameAnalysis::Identity id;
			id.seq = seq;
			id.width = 64;
			id.height = 36;
			return id;
		}

		static std::shared_ptr<const CameraModel::CameraMotion> Camera(uint64_t seq)
		{
			auto c = std::make_shared<CameraModel::CameraMotion>();
			c->seq = seq;
			c->valid = true;
			c->confidence = 0.9;
			return c;
		}

		TEST_METHOD(PublishCameraIsReadBackAsTheSamePointer)
		{
			using namespace FrameAnalysis;
			Store store;
			store.Begin(Id(1), kCamera);
			auto cam = Camera(1);
			Assert::IsTrue(store.PublishCamera(1, cam), L"published");
			Record out;
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kCamera, out) == ReadStatus::Ok, L"read ok");
			Assert::IsTrue((out.valid & kCamera) != 0, L"camera bit valid");
			Assert::IsTrue(out.camera.get() == cam.get(), L"same pointer, not a copy");
			Assert::IsTrue(cam.use_count() >= 3, L"shared ownership (test, store slot, copy read out)");
		}

		TEST_METHOD(InvalidateClearsTheCameraAndItsBit)
		{
			using namespace FrameAnalysis;
			Store store;
			store.Begin(Id(1), kCamera);
			store.PublishCamera(1, Camera(1));
			store.Invalidate(Discontinuity::SeekOrReset);
			Record out;
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kCamera, out) == ReadStatus::GroupInvalid, L"not measured after invalidation");
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kIdentity, out) == ReadStatus::Ok, L"identity still served");
			Assert::IsTrue(!out.camera && (out.valid & kCamera) == 0, L"camera cleared");
		}

		TEST_METHOD(PublishCameraFromAnOlderEpochIsRefused)
		{
			using namespace FrameAnalysis;
			Store store;
			store.Begin(Id(1), kCamera);
			store.Invalidate(Discontinuity::DroppedFrame);
			Assert::IsFalse(store.PublishCamera(1, Camera(1)), L"older epoch refused");
			Record out;
			store.Read(1, Use::SingleFrame, kIdentity, out);
			Assert::IsTrue(!out.camera && (out.valid & kCamera) == 0, L"nothing written");
			Assert::IsFalse(store.PublishCamera(99, Camera(99)), L"missing record refused");
		}

		TEST_METHOD(StoresThatNeverAskForCameraReadAsBefore)
		{
			using namespace FrameAnalysis;
			Store store;
			store.Begin(Id(1), kLuma);
			store.PublishMeasured(1, kLuma, LumaStats{}, RangeOccupancy{}, GrainEstimate{});
			Record out;
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kIdentity | kLuma, out) == ReadStatus::Ok, L"existing groups served");
			Assert::IsTrue(!out.camera, L"camera null");
			Assert::IsTrue(out.valid == (kIdentity | kLuma) && out.expected == (kIdentity | kLuma), L"bits unchanged");
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kCamera, out) == ReadStatus::GroupInvalid, L"camera not measured, not late");
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kIdentity | kLuma, out) == ReadStatus::Ok, L"status unchanged");
			// An asked-for camera that is not published yet reads as late.
			store.Begin(Id(2), kCamera);
			Assert::IsTrue(store.Read(2, Use::SingleFrame, kCamera, out) == ReadStatus::Late, L"late until published");
		}

		TEST_METHOD(GroupValuesAreStable)
		{
			using namespace FrameAnalysis;
			static_assert(kLabels == (1u << 8), "kLabels");
			static_assert(kCamera == (1u << 9), "kCamera");
			static_assert(kMotion == (1u << 5), "kMotion");
			Assert::IsTrue(kLabels == (1u << 8) && kCamera == (1u << 9), L"values");
			Assert::IsTrue((kCamera & kMeasuredGroups) == 0, L"camera is not a measured group");
		}

		TEST_METHOD(MarkRepeatDoesNotCopyTheCamera)
		{
			using namespace FrameAnalysis;
			Store store;
			store.Begin(Id(1), kLuma | kCamera);
			store.PublishMeasured(1, kLuma, LumaStats{}, RangeOccupancy{}, GrainEstimate{});
			store.PublishCamera(1, Camera(1));
			store.Begin(Id(2), kLuma);
			Assert::IsTrue(store.MarkRepeat(2, 1), L"repeat marked");
			Record out;
			Assert::IsTrue(store.Read(2, Use::SingleFrame, kIdentity | kLuma, out) == ReadStatus::Ok, L"measured copied");
			Assert::IsTrue(!out.camera && (out.valid & kCamera) == 0, L"camera not copied");
		}

		TEST_METHOD(ReleaseDropsTheCamera)
		{
			using namespace FrameAnalysis;
			Store store;
			store.Begin(Id(1), kCamera);
			auto cam = Camera(1);
			store.PublishCamera(1, cam);
			Assert::IsTrue(cam.use_count() >= 2, L"held by the slot");
			store.Release(1);
			Assert::IsTrue(cam.use_count() == 1, L"slot dropped its reference");
			Record out;
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kCamera, out) == ReadStatus::Missing, L"record gone");
		}
	};
}
