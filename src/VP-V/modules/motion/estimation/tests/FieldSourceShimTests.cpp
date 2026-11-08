// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV pass-through (TEMPORARY until P5c): Dejudder::MotionWarp's Field / FrameTrust / SubField / GrainError and Snap* accessors read the SHARED view (the motion
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
				// No snapshot was captured in this loop: the Snap* accessors answer with the members (see the snapshot test below).
				Assert::IsNull(warp.SnapField());
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

		// One source: when bound, Field / GrainError / FrameTrust / SubField are ALL the shared pair's set (one pair id), not the members.
		BEGIN_TEST_METHOD_ATTRIBUTE(BoundFieldGrainErrorFrameTrustAndSubFieldComeFromOneSharedPair)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BoundFieldGrainErrorFrameTrustAndSubFieldComeFromOneSharedPair)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 512, height = 256;
			std::vector<DejudderGpuPlanes> planes(2);
			for (int f = 0; f < 2; ++f)
			{
				const int shift = 16 * f;
				UploadDejudderFrame(gpu, MakeDejudderFrame(width, height, [shift](int x, int y) { return DejudderTexture(x - shift, y); },
					[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); }), planes[static_cast<size_t>(f)]);
			}
			Dejudder::Settings settings = DejudderTestSettings();
			settings.estSplit = true;
			VpMotion::VectorEngine engine;
			VpMotion::EstimateState<int> state(engine);
			state.dejudder.reset(new Dejudder::MotionWarp(fixture.Log(), gpu, settings));
			Dejudder::MotionWarp& warp = *state.dejudder;
			using Field = VpMotion::VectorEngineState::MotionShadowField;
			Assert::IsTrue(warp.Estimate(planes[0].planes, planes[1].planes, -1, true), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
			const pl_tex mField = warp.Field(), mTrust = warp.FrameTrust(), mSub = warp.SubField(), mGrain = warp.GrainError();
			Assert::IsNotNull(mField);
			Assert::IsNotNull(mGrain, L"the estimate asked for the grain map");
			// A shared set whose textures are NOT the members (sentinel handles: the accessors only hand them on): every accessor must
			// answer from it, the grain map included - one pair, never a field from the view with a grain map from the member.
			int tag[4] = { 0, 0, 0, 0 };
			const pl_tex sField = reinterpret_cast<pl_tex>(&tag[0]), sTrust = reinterpret_cast<pl_tex>(&tag[1]),
				sSub = reinterpret_cast<pl_tex>(&tag[2]), sGrain = reinterpret_cast<pl_tex>(&tag[3]);
			warp.SetFieldSource(&state);
			state.dejudderLiveFieldFrom = 5;
			engine.motionSegment.Publish(VpMotion::Direction::Forward, 5, 6, Field{ 5, 6, sField, sTrust, sSub, nullptr, sGrain },
				VpMotion::Source::Matcher, engine.motionValidity.Epoch(), 0, false);
			Dejudder::IFieldSource::Set set;
			Assert::IsTrue(state.LiveFieldSet(set));
			Assert::IsTrue(set.from == 5 && set.to == 6, L"one pair id for the whole set");
			Assert::IsTrue(warp.Field() == sField && warp.FrameTrust() == sTrust && warp.SubField() == sSub && warp.GrainError() == sGrain,
				L"bound: all four are the shared pair's set");
			Assert::IsTrue(sField != mField && sGrain != mGrain, L"(the sentinels are not the members)");
			// The pair the view holds is withdrawn (the next estimate retracts it): all four are the members together again.
			engine.motionSegment.Retract(VpMotion::Direction::Forward);
			Assert::IsTrue(warp.Field() == mField && warp.FrameTrust() == mTrust && warp.SubField() == mSub && warp.GrainError() == mGrain,
				L"retracted: all four are the members together");
			// A real publish of the members: the same textures, the grain map included.
			engine.motionSegment.Publish(VpMotion::Direction::Forward, 5, 6, Field{ 5, 6, mField, mTrust, mSub, warp.Camera(), mGrain },
				VpMotion::Source::Matcher, engine.motionValidity.Epoch(), 0, false);
			Assert::IsTrue(warp.Field() == mField && warp.GrainError() == mGrain && warp.FrameTrust() == mTrust && warp.SubField() == mSub);
			// Unbound: the members, no source consulted.
			warp.SetFieldSource(nullptr);
			Assert::IsTrue(warp.Field() == mField && warp.GrainError() == mGrain);
		}

		// The snapshot is the shared view captured at snapshot time: it stays that pair while the live view advances, holds a lease on the
		// matcher's copies, and a lost lease is a counted, shown fallback (the member answers, as before). Unbound = byte-identical.
		BEGIN_TEST_METHOD_ATTRIBUTE(BoundSnapshotStaysFixedWhileTheLiveViewAdvancesAndALostLeaseIsVisible)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BoundSnapshotStaysFixedWhileTheLiveViewAdvancesAndALostLeaseIsVisible)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 512, height = 256;
			const int shifts[3] = { 0, 16, 56 };   // pair 1 moves 16 px, pair 2 moves 40 px: the two fields differ
			std::vector<DejudderGpuPlanes> planes(3);
			for (int f = 0; f < 3; ++f)
			{
				const int shift = shifts[f];
				UploadDejudderFrame(gpu, MakeDejudderFrame(width, height, [shift](int x, int y) { return DejudderTexture(x - shift, y); },
					[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); }), planes[static_cast<size_t>(f)]);
			}
			Dejudder::Settings settings = DejudderTestSettings();
			settings.estSplit = true;
			VpMotion::VectorEngine engine;
			VpMotion::EstimateState<int> state(engine);
			state.dejudder.reset(new Dejudder::MotionWarp(fixture.Log(), gpu, settings));
			Dejudder::MotionWarp& warp = *state.dejudder;
			Dejudder::MotionWarp plain(fixture.Log(), gpu, settings);   // never bound: the byte-identity reference
			using Field = VpMotion::VectorEngineState::MotionShadowField;
			warp.SetFieldSource(&state);   // bound once, as RunDejudderEstimate does, before any snapshot
			auto estimateAndPublish = [&](int pair, uint64_t from, uint64_t to)
			{
				engine.motionSegment.Retract(VpMotion::Direction::Forward);   // the host's order: retract, estimate, publish
				state.dejudderLiveFieldFrom = 0;
				Assert::IsTrue(warp.Estimate(planes[static_cast<size_t>(pair)].planes, planes[static_cast<size_t>(pair + 1)].planes, -1, true),
					std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
				state.dejudderLiveFieldFrom = from;
				engine.motionSegment.Publish(VpMotion::Direction::Forward, from, to,
					Field{ from, to, warp.Field(), warp.FrameTrust(), warp.SubField(), warp.Camera(), warp.GrainError() },
					VpMotion::Source::Matcher, engine.motionValidity.Epoch(), 0, false);
			};
			// Nothing captured yet: the members (null), no lost lease.
			Assert::IsNull(warp.SnapField());
			Assert::AreEqual(0ull, state.SnapshotLeaseLostReads(), L"never captured is not a lost lease");
			estimateAndPublish(0, 1, 2);
			Assert::IsTrue(warp.SnapshotForWarp(), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
			Assert::IsTrue(warp.SnapshotGeneration() != 0);
			uint64_t from = 0, to = 0;
			state.SnapshotPair(from, to);
			Assert::IsTrue(from == 1 && to == 2, L"the snapshot carries the live pair's id");
			const DejudderField snapAtCapture = DownloadField(gpu, warp, warp.SnapField());
			const DejudderField liveAtCapture = DownloadField(gpu, warp);
			Assert::IsTrue(snapAtCapture.texels == liveAtCapture.texels, L"the snapshot is a copy of the live pair");
			// The live view advances to the next pair (estimated ahead): the snapshot stays the first pair.
			estimateAndPublish(1, 2, 3);
			Dejudder::IFieldSource::Set live;
			Assert::IsTrue(state.LiveFieldSet(live));
			Assert::IsTrue(live.from == 2 && live.to == 3, L"the live view moved on");
			state.SnapshotPair(from, to);
			Assert::IsTrue(from == 1 && to == 2, L"the snapshot is still the first pair");
			const DejudderField liveNow = DownloadField(gpu, warp);
			Assert::IsTrue(liveNow.texels != snapAtCapture.texels, L"(the two pairs' fields differ)");
			Assert::IsTrue(DownloadField(gpu, warp, warp.SnapField()).texels == snapAtCapture.texels, L"the snapshot did not change while the live view advanced");
			Assert::IsNotNull(warp.SnapFrameTrust());
			Assert::AreEqual(0ull, state.SnapshotLeaseLostReads(), L"the lease held");
			Assert::IsFalse(state.SnapshotLeaseDecision(1000).engaged);
			// Unbound is byte-identical: the same frames through a matcher with no source give the same snapshot.
			Assert::IsTrue(plain.Estimate(planes[0].planes, planes[1].planes, -1, true));
			Assert::IsTrue(plain.SnapshotForWarp());
			Assert::IsTrue(DownloadField(gpu, plain, plain.SnapField()).texels == snapAtCapture.texels, L"unbound snapshot == bound snapshot, byte for byte");
			// The lease is lost (the matcher's copies were invalidated: a snapshot replaced or failed): the member answers AND it is shown.
			warp.InvalidateSnapshot();
			Assert::IsNotNull(warp.SnapField(), L"the member answers, as before");
			Assert::AreEqual(1ull, state.SnapshotLeaseLostReads());
			Assert::IsTrue(state.SnapshotLeaseLost());
			Assert::IsTrue(state.SnapshotLeaseDecision(2000).engaged, L"a lost lease is a shown fallback, never silent");
			Assert::IsFalse(state.SnapshotLeaseDecision(2000 + 5000).engaged, L"released after the hold");
			// A complete snapshot again (the live pair is now the second): the lease is back, with the new pair.
			Assert::IsTrue(warp.SnapshotForWarp());
			Assert::IsFalse(state.SnapshotLeaseLost());
			state.SnapshotPair(from, to);
			Assert::IsTrue(from == 2 && to == 3);
			Assert::IsTrue(DownloadField(gpu, warp, warp.SnapField()).texels == liveNow.texels, L"the new snapshot is the live pair then");
			Assert::AreEqual(1ull, state.SnapshotLeaseLostReads());
			// A recreated matcher does not inherit the old matcher's capture (a fresh binding resets it).
			Dejudder::MotionWarp second(fixture.Log(), gpu, settings);
			second.SetFieldSource(&state);
			state.SnapshotPair(from, to);
			Assert::IsTrue(from == 0 && to == 0);
			Assert::IsNull(second.SnapField());
			Assert::AreEqual(1ull, state.SnapshotLeaseLostReads(), L"not a lost lease");
		}
	};
}
