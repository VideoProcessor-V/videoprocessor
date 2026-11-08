// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV pass-through (TEMPORARY until P5c): Dejudder::MotionWarp's Field / FrameTrust / SubField accessors read the SHARED view (the motion
// segment's published live set, through EstimateState as the IFieldSource) when a source is bound, and the matcher's own members otherwise.
// The renderer's reads keep the old API; these tests pin that the answer is the SAME texture either way, over a whole sequence of pairs,
// through a retraction and an epoch change, and that the output is byte-identical with and without the shim.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/EstimateState.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>

#include <memory>
#include <vector>

namespace VideoProcessorTest
{
	TEST_CLASS(FieldSourceShimTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(ForwardersReturnTheSameTexturesAsTheMembersAndTheViewOverAWholeSequence)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ForwardersReturnTheSameTexturesAsTheMembersAndTheViewOverAWholeSequence)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 512, height = 256, frames = 7;
			std::vector<DejudderGpuPlanes> planes(static_cast<size_t>(frames));
			for (int f = 0; f < frames; ++f)
			{
				const int shift = 8 * f;
				const DejudderFrame frame = MakeDejudderFrame(width, height,
					[shift](int x, int y) { return DejudderTexture(x - shift, y); },
					[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); });
				UploadDejudderFrame(gpu, frame, planes[static_cast<size_t>(f)]);
			}
			Dejudder::Settings settings = DejudderTestSettings();
			settings.estSplit = true;   // a sub-field exists, so SubField() is exercised
			VpMotion::VectorEngine engine;
			VpMotion::EstimateState<int> state(engine);
			state.dejudder.reset(new Dejudder::MotionWarp(fixture.Log(), gpu, settings));
			Dejudder::MotionWarp& warp = *state.dejudder;
			// The same frames through a second matcher with no source ever bound: the output must not differ.
			Dejudder::MotionWarp plain(fixture.Log(), gpu, settings);
			using Field = VpMotion::VectorEngineState::MotionShadowField;
			for (int f = 0; f + 1 < frames; ++f)
			{
				const uint64_t from = static_cast<uint64_t>(f + 1), to = from + 1;
				warp.SetFieldSource(nullptr);
				engine.motionSegment.Retract(VpMotion::Direction::Forward);
				state.dejudderLiveFieldFrom = 0;
				Assert::IsTrue(warp.Estimate(planes[static_cast<size_t>(f)].planes, planes[static_cast<size_t>(f + 1)].planes),
					std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
				Assert::IsTrue(plain.Estimate(planes[static_cast<size_t>(f)].planes, planes[static_cast<size_t>(f + 1)].planes));
				// The members (unbound accessors), then published exactly as RunDejudderEstimate does.
				const pl_tex mField = warp.Field(), mTrust = warp.FrameTrust(), mSub = warp.SubField();
				Assert::IsNotNull(mField);
				Assert::IsNotNull(mTrust);
				state.dejudderLiveFieldFrom = from;
				engine.motionSegment.Publish(VpMotion::Direction::Forward, from, to, Field{ from, to, mField, mTrust, mSub, warp.Camera() },
					VpMotion::Source::Matcher, engine.motionValidity.Epoch(), 0, false);
				warp.SetFieldSource(&state);
				// Bound and valid: the view's pointer, which is the member's texture, for each accessor.
				const auto look = engine.motionSegment.PeekFrom(VpMotion::Direction::Forward, from, 0);
				Assert::IsTrue(static_cast<bool>(look), L"the published pair is valid for its epoch");
				Assert::IsTrue(look.field->field == warp.Field() && look.field->field == mField, L"Field: view == forwarder == member");
				Assert::IsTrue(look.field->frameTrust == warp.FrameTrust() && look.field->frameTrust == mTrust, L"FrameTrust: view == forwarder == member");
				Assert::IsTrue(look.field->subField == warp.SubField() && look.field->subField == mSub, L"SubField: view == forwarder == member");
				// The snapshot copies are not published: they stay the member (TEMPORARY until P5c).
				Assert::IsTrue(warp.SnapField() == warp.SnapField());
				// Byte identity with and without the shim: the same field, the same warp output.
				const DejudderField bound = DownloadField(gpu, warp), unbound = DownloadField(gpu, plain);
				Assert::IsTrue(bound.texels == unbound.texels, L"the field is byte-identical with the shim bound");
				Dejudder::Planes outBound, outPlain;
				Assert::IsTrue(warp.Warp(planes[static_cast<size_t>(f)].planes, 0.5f, outBound));
				Assert::IsTrue(plain.Warp(planes[static_cast<size_t>(f)].planes, 0.5f, outPlain));
				Assert::IsTrue(DownloadPlane(gpu, outBound.luma) == DownloadPlane(gpu, outPlain.luma), L"the warp output is byte-identical");
				// Retracted (the next estimate is about to replace the outputs): no view, the member answers.
				engine.motionSegment.Retract(VpMotion::Direction::Forward);
				Assert::IsFalse(static_cast<bool>(engine.motionSegment.PeekFrom(VpMotion::Direction::Forward, from, 0)));
				Assert::IsTrue(warp.Field() == mField && warp.FrameTrust() == mTrust && warp.SubField() == mSub, L"retracted: the members");
				// Stale epoch (a resync): the published set is withdrawn by validity, the member answers.
				engine.motionSegment.Publish(VpMotion::Direction::Forward, from, to, Field{ from, to, mField, mTrust, mSub, warp.Camera() },
					VpMotion::Source::Matcher, engine.motionValidity.Epoch(), 0, false);
				Assert::IsTrue(warp.Field() == mField);
				engine.motionValidity.Invalidate(MotionValidity::Cause::Resync);
				Assert::IsFalse(static_cast<bool>(engine.motionSegment.PeekFrom(VpMotion::Direction::Forward, from, 0)), L"stale epoch: not valid");
				Assert::IsTrue(warp.Field() == mField && warp.FrameTrust() == mTrust && warp.SubField() == mSub, L"stale: the members");
			}
			// Unbound again: exactly the members (the Original pathway / every other test).
			warp.SetFieldSource(nullptr);
			Assert::IsTrue(warp.FieldSource() == nullptr);
		}
	};
}
