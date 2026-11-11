// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5c (MV1 reader switch): the P3 field view in EstimateState is the only reader of the matcher's outputs outside motion\estimation.
// These tests pin its validity rules (R1 LiveFor, R2 LiveFrom; R0 Live and R4 Snapshot carry no rule of their own), that the view
// answers nothing without a matcher, and on the GPU that it hands back the SAME textures as the matcher's accessors: bound to the shared
// pair (one pair id) and unbound (the delivery slot off / the Original pathway: the members), and that a snapshot read counts a lost
// lease once per accessor read, exactly as the direct reads did.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/EstimateState.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>

#include <vector>

namespace VideoProcessorTest
{
	TEST_CLASS(P3FieldViewTests)
	{
	public:
		// R1 (DejudderLiveFieldFor): every conjunct matters; motion_history_fixes adds the live-field sequence.
		TEST_METHOD(LiveForIsTheRendererLiveFieldRule)
		{
			using R = VpMotion::P3Rules;
			Assert::IsTrue(R::LiveFor(true, 7, 7, true, false, 7));
			Assert::IsTrue(R::LiveFor(true, 7, 7, true, true, 7));
			Assert::IsFalse(R::LiveFor(false, 7, 7, true, false, 7), L"no matcher");
			Assert::IsFalse(R::LiveFor(true, 0, 0, true, false, 0), L"sequence 0 is never a frame");
			Assert::IsFalse(R::LiveFor(true, 7, 6, true, false, 7), L"the estimate was another pair");
			Assert::IsFalse(R::LiveFor(true, 7, 7, false, false, 7), L"a stale motion epoch");
			// An estimate ahead moved the live outputs to B (live field from 8) while A = 7 still shows:
			Assert::IsTrue(R::LiveFor(true, 7, 7, true, false, 8), L"motion_history_fixes off: today's rule, the outputs count as A's");
			Assert::IsFalse(R::LiveFor(true, 7, 7, true, true, 8), L"motion_history_fixes on: not A's live field");
		}

		// R2 (the entrance segment's FieldOf): the pair that starts at the sequence; no estimated-sequence and no zero test.
		TEST_METHOD(LiveFromIsTheEntranceFieldOfRule)
		{
			using R = VpMotion::P3Rules;
			Assert::IsTrue(R::LiveFrom(true, 8, 8, true));
			Assert::IsFalse(R::LiveFrom(false, 8, 8, true), L"no matcher");
			Assert::IsFalse(R::LiveFrom(true, 7, 8, true), L"the live outputs are another pair");
			Assert::IsFalse(R::LiveFrom(true, 8, 8, false), L"a stale motion epoch");
			Assert::IsTrue(R::LiveFrom(true, 0, 0, true), L"(kept exactly: the rule has no sequence-0 test; FieldOf is never asked for 0)");
		}

		// No matcher (engine off, de-judder never created): every answer is empty, the side-effecting reads do nothing.
		TEST_METHOD(WithoutAMatcherTheViewAnswersNothing)
		{
			VpMotion::VectorEngine engine;
			VpMotion::EstimateState<int> state(engine);
			state.dejudderEstimatedSequence = 3;
			state.dejudderLiveFieldFrom = 3;
			state.dejudderEstimatedEpoch = engine.motionValidity.Epoch();
			Assert::IsFalse(state.Live().valid);
			Assert::IsFalse(state.LiveValidFor(3, false));
			Assert::IsFalse(state.LiveFor(3, true).valid);
			Assert::IsFalse(state.LiveFrom(3).valid);
			Assert::IsFalse(state.Snapshot().Usable());
			Assert::IsNull(state.Snapshot().Camera());
			Assert::IsNull(state.LookAheadTrust());
			Assert::IsNull(state.Occlusion(0));
			Assert::AreEqual(0.0f, state.MinTrustedShare());
			Assert::AreEqual(0ull, state.SnapshotLeaseLostReads());
		}

		// The view returns the matcher accessors' textures: bound to the shared pair (one pair id), unbound the members; the rules on the
		// real state; the snapshot handle is the Snap* accessors with one lease check per read.
		BEGIN_TEST_METHOD_ATTRIBUTE(ViewReturnsTheAccessorTexturesBoundAndUnboundUnderEachRule)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ViewReturnsTheAccessorTexturesBoundAndUnboundUnderEachRule)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 512, height = 256;
			const int shifts[3] = { 0, 16, 56 };
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
			using Field = VpMotion::VectorEngineState::MotionShadowField;
			auto same = [&](const VpMotion::P3FieldSet& s)
			{
				return s.valid && s.field == warp.Field() && s.frameTrust == warp.FrameTrust() && s.subField == warp.SubField() &&
					s.grainError == warp.GrainError() && s.camera == warp.Camera() && s.pixelMask == warp.PixelMask() && s.vectorOk == warp.VectorOk();
			};
			// Unbound (the delivery slot off / the Original pathway): the members.
			Assert::IsTrue(warp.Estimate(planes[0].planes, planes[1].planes, 0, true), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
			state.dejudderEstimatedSequence = 1;
			state.dejudderEstimatedEpoch = engine.motionValidity.Epoch();
			state.dejudderLiveFieldFrom = 1;
			const VpMotion::P3FieldSet members = state.Live();
			Assert::IsTrue(same(members));
			Assert::IsNotNull(members.field);
			Assert::IsNotNull(members.grainError, L"the estimate asked for the grain map");
			Assert::IsTrue(state.Occlusion(0) == warp.Occlusion(0) && state.Occlusion(0) != nullptr);
			Assert::AreEqual(warp.GetSettings().minTrustedShare, state.MinTrustedShare());
			Assert::IsTrue(same(state.LiveFor(1, true)) && same(state.LiveFor(1, false)) && same(state.LiveFrom(1)));
			// Bound to the shared pair whose textures are sentinels (not the members): the view answers the shared set, one pair id.
			int tag[4] = { 0, 0, 0, 0 };
			const pl_tex sField = reinterpret_cast<pl_tex>(&tag[0]), sTrust = reinterpret_cast<pl_tex>(&tag[1]),
				sSub = reinterpret_cast<pl_tex>(&tag[2]), sGrain = reinterpret_cast<pl_tex>(&tag[3]);
			warp.SetFieldSource(&state);
			engine.motionSegment.Publish(VpMotion::Direction::Forward, 1, 2, Field{ 1, 2, sField, sTrust, sSub, nullptr, sGrain },
				VpMotion::Source::Matcher, engine.motionValidity.Epoch(), 0, false);
			const VpMotion::P3FieldSet bound = state.LiveFor(1, true);
			Assert::IsTrue(same(bound));
			Assert::IsTrue(bound.field == sField && bound.frameTrust == sTrust && bound.subField == sSub && bound.grainError == sGrain,
				L"bound: field, trust, sub-field and grain map from the one shared pair");
			// R1 with an ahead estimate: the live outputs moved to B = 2 while A = 1 shows.
			state.dejudderLiveFieldFrom = 2;
			Assert::IsTrue(state.LiveFor(1, false).valid, L"motion_history_fixes off: the outputs still count as A's");
			Assert::IsFalse(state.LiveFor(1, true).valid, L"motion_history_fixes on: not A's live field");
			Assert::IsFalse(state.LiveFrom(1).valid);
			Assert::IsTrue(state.LiveFrom(2).valid);
			// A resync: every rule fails.
			state.dejudderLiveFieldFrom = 1;
			engine.motionValidity.Invalidate(MotionValidity::Cause::Resync);
			Assert::IsFalse(state.LiveFor(1, false).valid || state.LiveFrom(1).valid, L"a stale epoch");
			Assert::IsTrue(same(state.Live()), L"R0 has no rule: the accessors (now the members again)");
			Assert::IsTrue(state.Live().field == members.field);
			// Snapshot: the handle is the Snap* accessors.
			engine.motionSegment.Retract(VpMotion::Direction::Forward);
			Assert::IsTrue(warp.Estimate(planes[1].planes, planes[2].planes, 1, true));
			state.dejudderEstimatedEpoch = engine.motionValidity.Epoch();
			state.dejudderLiveFieldFrom = 2;
			engine.motionSegment.Publish(VpMotion::Direction::Forward, 2, 3,
				Field{ 2, 3, warp.Field(), warp.FrameTrust(), warp.SubField(), warp.Camera(), warp.GrainError() },
				VpMotion::Source::Matcher, engine.motionValidity.Epoch(), 0, false);
			Assert::IsTrue(warp.SnapshotForWarp(), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
			const auto snap = state.Snapshot();
			Assert::IsTrue(snap.Usable());
			Assert::IsTrue(snap.Field() == warp.SnapField() && snap.FrameTrust() == warp.SnapFrameTrust() && snap.SubField() == warp.SnapSubField() &&
				snap.Camera() == warp.SnapCamera() && snap.PixelMask() == warp.SnapPixelMask() && snap.VectorOk() == warp.SnapVectorOk());
			Assert::AreEqual(0ull, state.SnapshotLeaseLostReads(), L"the lease held");
			// A lost lease: one count per accessor read, as the direct reads counted (Usable = field + trust = two reads).
			warp.InvalidateSnapshot();
			Assert::IsTrue(snap.Usable(), L"the member answers, as before");
			Assert::AreEqual(2ull, state.SnapshotLeaseLostReads());
			(void)snap.SubField();
			Assert::AreEqual(3ull, state.SnapshotLeaseLostReads());
			warp.SetFieldSource(nullptr);
		}
	};
}
