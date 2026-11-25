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
#include <VP-V/core/Indicator.h>
#include <VP-V/modules/input/signal/ResyncDrive.h>
#include <VP-V/modules/motion/estimation/EstimateState.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/P3FieldFill.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		// Minimal fake IVectorEngine for testing P3FieldFill.h without touching a GPU device, matcher, or NVOF producer.
		struct FakeP3VectorEngine : VpMotion::IVectorEngine
		{
			VpMotion::VerifiedField field;
			uint64_t servedSeq = 0;
			uint64_t nullFwdSeq = 0;
			bool serveValid = true;
			mutable std::atomic<int> fieldOfCalls{ 0 };
			mutable std::atomic<int> unexpectedCalls{ 0 };

			explicit FakeP3VectorEngine(const VpMotion::VerifiedField& initial)
				: field(initial), servedSeq(initial.seq)
			{
			}

			uint64_t LatestVerifiedFieldSeq() const override { return serveValid ? servedSeq : 0; }
			uint64_t FieldEra() const override { return 1; }
			bool FieldOf(uint64_t seq, VpMotion::VerifiedField& out) const override
			{
				fieldOfCalls.fetch_add(1, std::memory_order_relaxed);
				if (serveValid && seq == servedSeq)
				{
					out = field;
					return true;
				}
				if (nullFwdSeq != 0 && seq == nullFwdSeq)
				{
					out = field;
					out.seq = seq;
					out.pairTo = seq;
					out.fwd = nullptr;
					return true;
				}
				out = VpMotion::VerifiedField();
				return false;
			}
			float FieldScale() const override { return field.scaleToSourcePx; }
			bool SeedFieldOf(uint64_t, VpMotion::SeedField& out) const override
			{
				unexpectedCalls.fetch_add(1, std::memory_order_relaxed);
				out = VpMotion::SeedField();
				return false;
			}
			bool GlobalFromEngine() const override { return false; }
			bool EngineGlobal(uint64_t, VpMotion::GlobalVec& out) const override
			{
				unexpectedCalls.fetch_add(1, std::memory_order_relaxed);
				out = VpMotion::GlobalVec();
				return false;
			}
			bool PairSpansSlip(uint64_t, uint64_t) const override { return false; }
			bool FlowIsCpuResident() const override { return false; }
			bool FlowOf(uint64_t, VpMotion::FlowView& out) const override
			{
				unexpectedCalls.fetch_add(1, std::memory_order_relaxed);
				out = VpMotion::FlowView();
				return false;
			}
			void SetPreEstimateHook(VpMotion::IPreEstimateHook*) override
			{
				unexpectedCalls.fetch_add(1, std::memory_order_relaxed);
			}
		};

		// RAII join guard so off-render-thread tests always join cleanly even on assertion failure (Rule 2).
		struct P3ThreadJoinGuard
		{
			std::thread thread;
			~P3ThreadJoinGuard()
			{
				if (thread.joinable())
					thread.join();
			}
			P3ThreadJoinGuard() = default;
			P3ThreadJoinGuard(const P3ThreadJoinGuard&) = delete;
			P3ThreadJoinGuard& operator=(const P3ThreadJoinGuard&) = delete;
		};

		// Local IExternalDemand double forwarding RegisterOwnerFill to a standalone SkeletonMemo (no global hub mutation).
		struct FakeP3ExternalDemand : Vpv::Skeleton::IExternalDemand
		{
			VpSkeleton::SkeletonMemo* memo = nullptr;
			int registerCalls = 0;

			explicit FakeP3ExternalDemand(VpSkeleton::SkeletonMemo* target) : memo(target) {}
			~FakeP3ExternalDemand() {}

			void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
				const std::vector<VpSkeleton::ProductId>&) override {}
			void ClearExternalDemand(const void*, const void*) override {}
			VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
				const std::string& unit, VpSkeleton::OwnerFill fill) override
			{
				++registerCalls;
				if (memo == nullptr)
					return VpSkeleton::SkeletonMemo::Registration::NotInCatalog;
				return memo->RegisterOwnerFill(id, form, unit, std::move(fill));
			}
		};
	}

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

		// T-AR3-N1 (AUDIT-R 1fa78cdb9 N1; doc 91 D "PairValidAt everywhere"): the view's pair gate is MotionValidity::PairServes (epoch
		// form AND stamp form), not the epoch-only Current(). Unbound (resync_event off) the stamps refuse nothing, so the answer is the
		// old epoch one; bound, a pair that spans a FULL / continuity stamp is refused although its epoch is still current.
		TEST_METHOD(ThePairGateIsPairServesAndKeepsTheEpochFormUnbound)
		{
			VpMotion::VectorEngine engine;
			VpMotion::EstimateState<int> state(engine);
			const uint64_t e = engine.motionValidity.Epoch();
			Assert::IsTrue(e != 0, L"a motion epoch exists");
			// Unbound: exactly Current(epoch) for every pair shape (adjacent, default B, gap pair, no pair).
			Assert::IsTrue(state.PairStillServes(3, 4, e));
			Assert::IsTrue(state.PairStillServes(3, 0, e), L"B 0 = the adjacent pair");
			Assert::IsTrue(state.PairStillServes(3, 6, e), L"a gap pair, no stamps: the epoch form");
			Assert::IsTrue(state.PairStillServes(0, 0, e), L"no pair: the old epoch form, untouched");
			Assert::IsFalse(state.PairStillServes(3, 4, e + 1), L"a stale epoch");
			Assert::IsFalse(state.PairStillServes(3, 4, 0), L"epoch 0 is never current");

			// Bound to an Indicator: a FULL at sequence 4 (the stamps of the existing PairServes test).
			std::shared_ptr<VpIndicator::Indicator> ind = std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), 64);
			engine.motionValidity.Bind(ind);
			ind->SealAndObserve(1);
			ind->SealAndObserve(2);
			ind->Advance(VpIndicator::kFull, VpResync::Cause::LiveQueueReset, 4);
			const uint64_t eb = engine.motionValidity.Epoch();
			Assert::IsTrue(engine.motionValidity.Current(eb), L"plausibility: the epoch form alone still accepts every pair below");
			Assert::IsTrue(state.PairStillServes(2, 3, eb), L"before the FULL");
			Assert::IsFalse(state.PairStillServes(3, 4, eb), L"spans the FULL at 4: the stamp form refuses, the epoch form would not");
			Assert::IsFalse(state.PairStillServes(3, 0, eb), L"the default adjacent pair is the same pair");
			Assert::IsTrue(state.PairStillServes(4, 5, eb), L"after it");
			Assert::IsTrue(state.PairStillServes(1, 3, eb), L"a gap pair clear of the FULL");
			Assert::IsFalse(state.PairStillServes(3, 6, eb), L"a gap pair across the FULL is refused (never across a FULL)");
			Assert::IsFalse(state.PairStillServes(2, 3, eb + 1), L"the stale epoch still refuses first");
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

		// M1#1 (B0#2, P3FieldFill.h): MakeP3GpuOwnerFill publishes a zero-copy, frame-scoped, render-thread-only GPU view
		// of IVectorEngine::FieldOf through SkeletonMemo without needing a GPU dispatcher or VRAM ledger grant; memoizes
		// compute-once per frame; evicts at OnSeal so stale matcher pointers never survive across frames; and declines
		// visibly (OwnerDeclined / VariantUndeclared / WrongThread) with same-input negative controls proving each hazard.
		TEST_METHOD(P3FieldOwnerFillPublishesZeroCopyViewAndDeclinesVisiblyWhenAbsent)
		{
			using namespace VpSkeleton;

			int sentinelMem[4] = { 1, 2, 3, 4 };
			const pl_tex sFwd = reinterpret_cast<pl_tex>(&sentinelMem[0]);
			const pl_tex sBwd = reinterpret_cast<pl_tex>(&sentinelMem[1]);
			const pl_tex sTrust = reinterpret_cast<pl_tex>(&sentinelMem[2]);
			const pl_tex sFwdRotated = reinterpret_cast<pl_tex>(&sentinelMem[3]);

			VpMotion::VerifiedField expected;
			expected.seq = 42;
			expected.pairFrom = 41;
			expected.pairTo = 42;
			expected.fwd = sFwd;
			expected.bwd = sBwd;
			expected.trust = sTrust;
			expected.gridW = 120;
			expected.gridH = 68;
			expected.blockSize = 16;
			expected.scaleToSourcePx = 1.0f;
			expected.stamp.era = 7u;
			expected.stamp.v[VpIndicator::kFull] = 3u;

			FakeP3VectorEngine engine(expected);
			engine.nullFwdSeq = 100;
			int hostTag = 0;
			const void* seenHost = nullptr;
			const VpMotion::P3EngineOf engineOf = [&engine, &seenHost](const void* host) -> VpMotion::IVectorEngine* {
				seenHost = host;
				return &engine;
			};

			// Negative control 1 (zero-copy view contract, Rule 3): on the EXACT same standalone SkeletonMemo
			// (no GPU dispatcher, no VRAM ledger grant) and same input, a non-view GPU fill is refused as NoGpuDispatcher.
			{
				SkeletonMemo memoNoView;
				memoNoView.SetHost(&hostTag);
				memoNoView.SetRenderThread(std::this_thread::get_id());
				OwnerFill nonViewFill = VpMotion::MakeP3GpuOwnerFill(engineOf);
				nonViewFill.view = false;
				Assert::IsTrue(memoNoView.RegisterOwnerFill(kP3AnalysisField, AcquireForm::Gpu, VpMotion::kP3FieldUnit, nonViewFill)
					== SkeletonMemo::Registration::Registered);
				VpMotion::VerifiedField vfNoView;
				AcquireResult rNoView;
				Assert::IsFalse(VpMotion::AcquireP3Field(memoNoView, 42, vfNoView, &rNoView),
					L"negative control: without view = true, standalone memo without GPU dispatcher must refuse");
				Assert::IsTrue(rNoView.status == AcquireStatus::Unavailable);
				Assert::IsTrue(rNoView.cause == AcquireCause::NoGpuDispatcher);
				Assert::AreEqual(0, engine.fieldOfCalls.load(), L"negative control must fail before calling FieldOf");
			}

			// Standalone SkeletonMemo on render thread: NO GPU dispatcher and NO VRAM ledger grant (view = true, bytes = 0).
			SkeletonMemo memo;
			memo.SetHost(&hostTag);
			memo.SetRenderThread(std::this_thread::get_id());
			const OwnerFill fill = VpMotion::MakeP3GpuOwnerFill(engineOf);
			Assert::IsTrue(fill.view, L"P3 GPU owner fill must be a zero-copy view");
			Assert::IsTrue(fill.frameScoped, L"P3 GPU owner fill must be frame-scoped");
			Assert::IsTrue(fill.renderThreadOnly, L"P3 GPU owner fill must be render-thread-only");
			Assert::AreEqual(0u, fill.variantAxes, L"P3 GPU owner fill declares default variant only");
			Assert::IsTrue(memo.RegisterOwnerFill(kP3AnalysisField, AcquireForm::Gpu, VpMotion::kP3FieldUnit, fill)
				== SkeletonMemo::Registration::Registered);
			Assert::AreEqual(uint64_t(0), memo.GpuNeedBytes(), L"zero-copy view declares 0 VRAM need");

			// Positive path 1: FilledAtRead -> exact zero-copy handle + VerifiedField sentinel facts, FieldOf called once.
			VpMotion::VerifiedField vf1;
			AcquireResult r1;
			Assert::IsTrue(VpMotion::AcquireP3Field(memo, 42, vf1, &r1), L"first AcquireP3Field must succeed");
			Assert::IsTrue(r1.status == AcquireStatus::FilledAtRead, L"first read fills at read");
			Assert::IsTrue(r1.source == AcquireSource::Memo);
			Assert::IsTrue(r1.cause == AcquireCause::None);
			Assert::IsTrue(r1.gpu.Valid());
			Assert::AreEqual(uint64_t(0), r1.gpu.bytes, L"zero-copy view allocates 0 GPU bytes");
			Assert::AreEqual(120u, r1.gpu.width);
			Assert::AreEqual(68u, r1.gpu.height);
			Assert::IsTrue(r1.gpu.resource == static_cast<const void*>(sFwd), L"gpu.resource aliases VerifiedField::fwd");
			Assert::AreEqual(uint64_t(0), memo.GpuBytes(), L"memo GPU bytes stay 0 for zero-copy views");
			Assert::IsTrue(seenHost == &hostTag, L"host pointer forwarded to engineOf");
			const VpMotion::VerifiedField* facts1 = VpMotion::P3FieldFactsOf(r1);
			Assert::IsNotNull(facts1, L"P3FieldFactsOf extracts VerifiedField metadata");
			Assert::AreEqual(uint64_t(42), vf1.seq);
			Assert::AreEqual(uint64_t(41), vf1.pairFrom);
			Assert::AreEqual(uint64_t(42), vf1.pairTo);
			Assert::IsTrue(vf1.fwd == sFwd && vf1.bwd == sBwd && vf1.trust == sTrust, L"exact sentinel textures");
			Assert::AreEqual(120, vf1.gridW);
			Assert::AreEqual(68, vf1.gridH);
			Assert::AreEqual(16, vf1.blockSize);
			Assert::AreEqual(1.0f, vf1.scaleToSourcePx);
			Assert::IsTrue(VpIndicator::SameFor(vf1.stamp, expected.stamp, VpIndicator::kAllCounters), L"stamp preserved");
			Assert::IsTrue(facts1->fwd == sFwd && facts1->bwd == sBwd && facts1->trust == sTrust
				&& facts1->pairFrom == 41 && facts1->pairTo == 42 && facts1->blockSize == 16
				&& facts1->scaleToSourcePx == 1.0f, L"P3FieldFactsOf matches exact sentinels");
			Assert::AreEqual(1, engine.fieldOfCalls.load(), L"FieldOf called exactly once");
			Assert::IsTrue(memo.LastCause(kP3AnalysisField).empty(), L"no fallback engaged when served");

			// Positive path 2: second Acquire on the same frame -> Reused, compute-once (fieldOfCalls stays 1).
			VpMotion::VerifiedField vf2;
			AcquireResult r2;
			Assert::IsTrue(VpMotion::AcquireP3Field(memo, 42, vf2, &r2), L"second AcquireP3Field must reuse memo");
			Assert::IsTrue(r2.status == AcquireStatus::Reused, L"second read must be memoized Reused");
			Assert::IsTrue(r2.source == AcquireSource::Memo);
			Assert::AreEqual(1, engine.fieldOfCalls.load(), L"compute-once: FieldOf not called a second time");
			Assert::IsTrue(vf2.fwd == vf1.fwd && vf2.bwd == vf1.bwd && vf2.trust == vf1.trust, L"same view handles reused");
			Assert::AreEqual(uint64_t(41), vf2.pairFrom);
			Assert::AreEqual(uint64_t(42), vf2.pairTo);
			Assert::AreEqual(16, vf2.blockSize);
			Assert::AreEqual(1.0f, vf2.scaleToSourcePx);
			const AcquireCounters cReuse = memo.CountersOf(kP3AnalysisField);
			Assert::AreEqual(1ull, cReuse.filledAtRead);
			Assert::AreEqual(1ull, cReuse.reusedMemo);
			Assert::AreEqual(1ull, cReuse.ownerFillCalls);

			// Undeclared variant (plane = 1): refused visibly as VariantUndeclared without calling FieldOf.
			VariantKey nonDefault;
			nonDefault.plane = 1;
			const AcquireResult rVar = memo.Acquire(kP3AnalysisField, 42, AcquireForm::Gpu, nonDefault);
			Assert::IsTrue(rVar.status == AcquireStatus::Unavailable);
			Assert::IsTrue(rVar.cause == AcquireCause::VariantUndeclared, L"non-default plane axis must be VariantUndeclared");
			Assert::IsNull(VpMotion::P3FieldFactsOf(rVar));
			Assert::AreEqual(1, engine.fieldOfCalls.load(), L"undeclared variant must never invoke owner fill");

			// Off-render-thread Acquire (worker std::thread joined via RAII guard): refused visibly as WrongThread
			// BOTH for the already-memoized frame 42 AND for an uncached frame 43, without invoking FieldOf.
			bool okOffCached = true;
			bool okOffUncached = true;
			VpMotion::VerifiedField vfOffCached;
			VpMotion::VerifiedField vfOffUncached;
			AcquireResult rOffCached;
			AcquireResult rOffUncached;
			{
				P3ThreadJoinGuard worker;
				worker.thread = std::thread([&]() {
					okOffCached = VpMotion::AcquireP3Field(memo, 42, vfOffCached, &rOffCached);
					okOffUncached = VpMotion::AcquireP3Field(memo, 43, vfOffUncached, &rOffUncached);
				});
			}
			Assert::IsFalse(okOffCached, L"off-render-thread Acquire must fail even on a cached frame");
			Assert::IsTrue(rOffCached.status == AcquireStatus::Unavailable);
			Assert::IsTrue(rOffCached.cause == AcquireCause::WrongThread, L"off-render-thread GPU Acquire must be WrongThread");
			Assert::IsNull(VpMotion::P3FieldFactsOf(rOffCached));
			Assert::IsFalse(okOffUncached, L"off-render-thread Acquire must fail on an uncached frame");
			Assert::IsTrue(rOffUncached.status == AcquireStatus::Unavailable);
			Assert::IsTrue(rOffUncached.cause == AcquireCause::WrongThread);
			Assert::AreEqual(1, engine.fieldOfCalls.load(), L"off-render-thread Acquire must never call IVectorEngine::FieldOf");

			// Negative control 2 (frame-scoped stale-view hazard, Rule 3): a naive non-frame-scoped view on the EXACT
			// same input survives OnSeal(42) and keeps returning the stale sFwd pointer after the matcher rotated to sFwdRotated!
			{
				FakeP3VectorEngine leakyEngine(expected);
				const VpMotion::P3EngineOf leakyEngineOf = [&leakyEngine](const void*) -> VpMotion::IVectorEngine* { return &leakyEngine; };
				OwnerFill baseFill = VpMotion::MakeP3GpuOwnerFill(leakyEngineOf);
				OwnerFill leakyFill = baseFill;
				leakyFill.frameScoped = false;
				leakyFill.fill = [inner = baseFill.fill](const OwnerFillRequest& req, SkeletonMemo& m, OwnerFillOutput& out) {
					const bool ok = inner(req, m, out);
					if (ok)
						out.scope = OwnerFillOutput::Scope::Window;
					return ok;
				};
				SkeletonMemo memoLeaky;
				memoLeaky.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(memoLeaky.RegisterOwnerFill(kP3AnalysisField, AcquireForm::Gpu, VpMotion::kP3FieldUnit, leakyFill)
					== SkeletonMemo::Registration::Registered);
				VpMotion::VerifiedField vfLeak1, vfLeak2;
				AcquireResult rLeak1, rLeak2;
				Assert::IsTrue(VpMotion::AcquireP3Field(memoLeaky, 42, vfLeak1, &rLeak1));
				Assert::IsTrue(vfLeak1.fwd == sFwd);
				memoLeaky.OnSeal(42);
				leakyEngine.field.fwd = sFwdRotated;
				Assert::IsTrue(VpMotion::AcquireP3Field(memoLeaky, 42, vfLeak2, &rLeak2));
				Assert::IsTrue(rLeak2.status == AcquireStatus::Reused && vfLeak2.fwd == sFwd,
					L"negative control: without frameScoped/Scope::Frame, OnSeal(42) leaks the stale matcher pointer");
			}

			// Protected path: frame-scoped eviction at OnSeal(42) drops the expired view so the next Acquire does NOT reuse it.
			Assert::AreEqual(size_t(1), memo.MemoCount(), L"memo holds frame 42 view before seal");
			memo.OnSeal(42);
			Assert::AreEqual(size_t(0), memo.MemoCount(), L"frame-scoped view must be evicted at OnSeal(42)");
			Assert::AreEqual(1ull, memo.CountersOf(kP3AnalysisField).released, L"eviction counted as released");
			engine.field.fwd = sFwdRotated;
			VpMotion::VerifiedField vf3;
			AcquireResult r3;
			Assert::IsTrue(VpMotion::AcquireP3Field(memo, 42, vf3, &r3));
			Assert::IsTrue(r3.status == AcquireStatus::FilledAtRead, L"post-OnSeal(42) Acquire must NOT reuse expired view");
			Assert::IsTrue(vf3.fwd == sFwdRotated, L"post-OnSeal(42) Acquire observes fresh pointer, never stale sFwd");
			Assert::AreEqual(2, engine.fieldOfCalls.load(), L"re-queried FieldOf after frame-scoped eviction");

			// Advancing seal to frame 43 retires frame 42 completely: FrameReleased, never recomputed.
			memo.OnSeal(43);
			VpMotion::VerifiedField vf4;
			AcquireResult r4;
			Assert::IsFalse(VpMotion::AcquireP3Field(memo, 42, vf4, &r4));
			Assert::IsTrue(r4.status == AcquireStatus::Unavailable);
			Assert::IsTrue(r4.cause == AcquireCause::FrameReleased, L"frame 42 past OnSeal(43) must be FrameReleased");
			Assert::AreEqual(2, engine.fieldOfCalls.load(), L"released frame must never recompute");

			// Negative control 3 (engine absent, Rule 4): visible OwnerDeclined + "engine absent" in LastCause & Decisions.
			{
				SkeletonMemo memoNoEngine;
				memoNoEngine.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(memoNoEngine.RegisterOwnerFill(kP3AnalysisField, AcquireForm::Gpu, VpMotion::kP3FieldUnit,
					VpMotion::MakeP3GpuOwnerFill([](const void*) -> VpMotion::IVectorEngine* { return nullptr; }))
					== SkeletonMemo::Registration::Registered);
				VpMotion::VerifiedField vfAbsent;
				AcquireResult rAbsent;
				Assert::IsFalse(VpMotion::AcquireP3Field(memoNoEngine, 42, vfAbsent, &rAbsent),
					L"engine absent must decline");
				Assert::IsTrue(rAbsent.status == AcquireStatus::Unavailable);
				Assert::IsTrue(rAbsent.cause == AcquireCause::OwnerDeclined);
				Assert::IsNull(VpMotion::P3FieldFactsOf(rAbsent));
				Assert::IsNull(vfAbsent.fwd);
				const std::string causeAbsent = memoNoEngine.LastCause(kP3AnalysisField);
				Assert::IsTrue(causeAbsent.find("engine absent") != std::string::npos,
					L"decline reason must visibly name engine absent");
				const auto decisions = memoNoEngine.Decisions();
				Assert::AreEqual(size_t(1), decisions.size(), L"absent engine must surface in Fallbacks Decisions()");
				Assert::IsTrue(decisions[0].engaged && decisions[0].reason.find("engine absent") != std::string::npos,
					L"Decisions() must carry engaged decline reason");
			}

			// Negative control 4 (missing field for seq = 99, Rule 4): visible OwnerDeclined + "no verified field", read-only.
			VpMotion::VerifiedField vfMissing;
			AcquireResult rMissing;
			Assert::IsFalse(VpMotion::AcquireP3Field(memo, 99, vfMissing, &rMissing), L"missing seq 99 must decline");
			Assert::IsTrue(rMissing.status == AcquireStatus::Unavailable);
			Assert::IsTrue(rMissing.cause == AcquireCause::OwnerDeclined);
			Assert::IsNull(VpMotion::P3FieldFactsOf(rMissing));
			Assert::IsNull(vfMissing.fwd);
			Assert::IsTrue(memo.LastCause(kP3AnalysisField).find("no verified field") != std::string::npos,
				L"missing field decline must visibly name no verified field");
			Assert::AreEqual(3, engine.fieldOfCalls.load(), L"FieldOf(99) queried once");

			// Negative control 5 (FieldOf returns true with null fwd texture): visibly declined, never published.
			VpMotion::VerifiedField vfNullFwd;
			AcquireResult rNullFwd;
			Assert::IsFalse(VpMotion::AcquireP3Field(memo, 100, vfNullFwd, &rNullFwd),
				L"FieldOf returning true with null fwd must decline");
			Assert::IsTrue(rNullFwd.status == AcquireStatus::Unavailable);
			Assert::IsTrue(rNullFwd.cause == AcquireCause::OwnerDeclined);
			Assert::IsNull(VpMotion::P3FieldFactsOf(rNullFwd));
			Assert::IsNull(vfNullFwd.fwd);
			Assert::IsTrue(memo.LastCause(kP3AnalysisField).find("no verified field") != std::string::npos,
				L"null fwd decline must visibly name no verified field");

			Assert::AreEqual(0, engine.unexpectedCalls.load(),
				L"P3FieldFill must stay strictly read-only on FieldOf (never invoke FlowOf/SeedFieldOf/EngineGlobal/hook)");
		}

		// M1#1 (B0#2, P3FieldFill.h): TryRegisterP3FieldFill enforces catalog owner unit ("motion/estimation"),
		// is idempotent on AlreadyRegistered, surfaces registration refusal as false, and P3FieldFactsOf rejects foreign tags.
		TEST_METHOD(TryRegisterP3FieldFillEnforcesOwnerUnitIdempotencyAndTagGuard)
		{
			using namespace VpSkeleton;

			int sentinelMem[2] = { 10, 20 };
			const pl_tex sFwd = reinterpret_cast<pl_tex>(&sentinelMem[0]);
			VpMotion::VerifiedField expected;
			expected.seq = 7;
			expected.pairFrom = 6;
			expected.pairTo = 7;
			expected.fwd = sFwd;
			expected.gridW = 64;
			expected.gridH = 36;
			expected.blockSize = 16;
			expected.scaleToSourcePx = 1.0f;
			FakeP3VectorEngine engine(expected);
			const VpMotion::P3EngineOf engineOf = [&engine](const void*) -> VpMotion::IVectorEngine* { return &engine; };

			SkeletonMemo memo;
			memo.SetRenderThread(std::this_thread::get_id());

			// Negative control: registering kP3AnalysisField under a non-owning unit is refused (NotOwner).
			Assert::IsTrue(memo.RegisterOwnerFill(kP3AnalysisField, AcquireForm::Gpu, "wrong/unit",
				VpMotion::MakeP3GpuOwnerFill(engineOf)) == SkeletonMemo::Registration::NotOwner,
				L"catalog must reject non-owner registration for kP3AnalysisField");

			// Refusing IExternalDemand (NotInCatalog) -> TryRegisterP3FieldFill returns false (never silently succeeds).
			FakeP3ExternalDemand refusingDemand(nullptr);
			Assert::IsFalse(VpMotion::TryRegisterP3FieldFill(&refusingDemand, engineOf),
				L"TryRegisterP3FieldFill must return false when registration is refused");
			Assert::AreEqual(1, refusingDemand.registerCalls);

			// Real memo-backed IExternalDemand -> Registered (true), then AlreadyRegistered (still true, idempotent).
			FakeP3ExternalDemand memoDemand(&memo);
			Assert::IsTrue(VpMotion::TryRegisterP3FieldFill(&memoDemand, engineOf), L"first registration succeeds");
			Assert::IsTrue(VpMotion::TryRegisterP3FieldFill(&memoDemand, engineOf), L"duplicate registration is idempotent");
			Assert::AreEqual(2, memoDemand.registerCalls);

			VpMotion::VerifiedField vf;
			AcquireResult r;
			Assert::IsTrue(VpMotion::AcquireP3Field(memo, 7, vf, &r));
			Assert::IsTrue(r.status == AcquireStatus::FilledAtRead && vf.fwd == sFwd && vf.gridW == 64 && vf.gridH == 36);

			// Tag guard negative control: a foreign AcquireResult carrying another textureSet tag must return nullptr from P3FieldFactsOf.
			AcquireResult foreign;
			foreign.status = AcquireStatus::FilledAtRead;
			foreign.gpu.resource = const_cast<void*>(static_cast<const void*>(sFwd));
			foreign.gpu.textureSet = &sentinelMem[1];
			foreign.gpu.keepAlive = std::make_shared<int>(12345);
			Assert::IsNull(VpMotion::P3FieldFactsOf(foreign),
				L"P3FieldFactsOf must return nullptr for non-P3 textureSet tags");
		}
	};
}
