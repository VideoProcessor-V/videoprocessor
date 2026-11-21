// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Package D: Camera Test Seams (CPU-only deterministic tests using production-inert atomic seams in CameraTestSeams.h).
#include "CameraLinkTestSupport.h"
#include "PhotometricRefineScenes.h"
#include <VP-V/modules/motion/estimation/global_model/CameraTestSeams.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelFit.h>
#include <VP-V/modules/motion/estimation/global_model/PhotometricVerdict.h>
#include <VP-V/modules/motion/estimation/global_model/PhotometricWiring.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitFieldPod.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLabelFill.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLane.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>

#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		struct SeamFakeLabelDevice : CameraLink::ILabelDevice
		{
			int produced = 0;
			bool Produce(const CameraModel::CameraMotion&, const CameraLink::FieldCopy*, CameraLink::LabelTarget) override
			{
				++produced;
				return true;
			}
			pl_tex TextureOf(CameraLink::LabelTarget t) const override
			{
				return reinterpret_cast<pl_tex>(static_cast<uintptr_t>(0x2000 + (t.live ? 0x100 : 0) + t.index));
			}
		};

		struct ReadFitFieldRig
		{
			CameraLink::CameraLane lane;
			std::shared_ptr<VpIndicator::Indicator> indicator = std::make_shared<VpIndicator::Indicator>(1, 256);
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core{ store };
			SeamFakeLabelDevice device;
			std::unique_ptr<CameraLink::CameraLabelFill> fill;

			ReadFitFieldRig()
			{
				Assert::IsTrue(lane.Build(1920, 1080, true, indicator), L"CameraLane::Build with labels must succeed");
				CameraLink::CameraLinkSettings s;
				s.p7Wanted = true;
				s.deterministic = true;
				core.OnSettings(s);
				core.SetFieldSources(lane.Source(VpSkeleton::kCameraFitField), VpSkeleton::LaneLeaseSource());
				core.AttachLane(lane.Source(VpSkeleton::kP7CameraFit));
				CameraLink::TimingFacts tf;
				tf.measuredSourceHz = 24;
				core.SetTimingFacts(tf);
				core.Producer()->NoteTiming();
				fill.reset(new CameraLink::CameraLabelFill(lane.Source(VpSkeleton::kP8CameraLabels), *core.Link(), device));
				fill->SetFieldSource(lane.Source(VpSkeleton::kCameraFitField));
			}

			void PrepareAndFit(uint64_t seq)
			{
				indicator->Seal(seq);
				CameraLink::CameraLane::Sealed sealed;
				Assert::IsTrue(lane.OnFrame(seq, *indicator, sealed), L"lane.OnFrame must accept sequential frames");
				CameraLink::FramePhaseFacts f;
				f.seq = seq;
				Assert::IsTrue(core.SubmitField(CameraLinkTest::PanJob(seq, 8).field, f), L"SubmitField must succeed");
				core.ProcessPending();
			}
		};
	}

	TEST_CLASS(CameraReadFitFieldSeamTests)
	{
	public:
		// Seam 1: if the fit-field ring slot is sealed and released between DecodeFitFieldPod and the re-read check,
		// ReadFitField rejects the copy, increments fieldUnreadable, and never passes the recycled field to ILabelDevice.
		TEST_METHOD(ReadFitFieldRereadRejectsSlotRecycledMidRead)
		{
			ReadFitFieldRig rig;

			// Control (unarmed seam): frame 1 decodes cleanly and produces P8 labels.
			rig.PrepareAndFit(1);
			VpSkeleton::IGapWork* gap = rig.fill->GapWork();
			Assert::IsTrue(gap->HasWork(), L"frame 1 must be ready for P8 lane fill");
			gap->RunChunk(1000);
			Assert::AreEqual(1, rig.device.produced, L"control frame must reach label device");
			Assert::AreEqual<uint64_t>(1, rig.fill->Counters().lane.load(), L"control frame must publish P8");
			Assert::AreEqual<uint64_t>(0, rig.fill->Counters().fieldUnreadable.load(), L"control frame must be readable");

			// Armed seam: frame 2's fit-field slot is sealed + released mid-read (between DecodeFitFieldPod and re-read).
			rig.PrepareAndFit(2);
			VpSkeleton::SlotSet* pf = rig.lane.Table()->Find(VpSkeleton::kCameraFitField);
			VpSkeleton::SlotSet* p8 = rig.lane.Table()->Find(VpSkeleton::kP8CameraLabels);
			Assert::IsNotNull(pf);
			Assert::IsNotNull(p8);

			struct HookCtx
			{
				VpSkeleton::SlotSet* pf = nullptr;
				int calls = 0;
				uint64_t lastSeq = 0;
			} ctx{ pf, 0, 0 };

			auto hook = [](uint64_t seq, void* raw) {
				HookCtx* c = static_cast<HookCtx*>(raw);
				++c->calls;
				c->lastSeq = seq;
				c->pf->Seal(seq, [](const VpAncestry::Header&) { return VpSkeleton::SealReason::Accepted; });
				c->pf->Release(seq);
			};

			{
				CameraModel::CameraTestSeams::ScopedReadFitFieldHook guard(hook, &ctx);
				Assert::IsTrue(gap->HasWork(), L"frame 2 must be ready before hook fires");
				gap->RunChunk(1000);
			}

			Assert::AreEqual(1, ctx.calls, L"pre-reread hook must fire exactly once");
			Assert::AreEqual<uint64_t>(2, ctx.lastSeq, L"hook must observe seq 2");
			Assert::AreEqual(1, rig.device.produced, L"recycled fit field must never reach label device");
			Assert::AreEqual<uint64_t>(1, rig.fill->Counters().fieldUnreadable.load(), L"mid-read recycle must count fieldUnreadable");
			Assert::AreEqual<uint64_t>(1, rig.fill->Counters().laneBlocked.load(), L"mid-read recycle must block P8 lane publish");
			Assert::IsTrue(p8->StateOf(2) != VpSkeleton::SlotState::Published, L"P8 slot must be abandoned on unreadable field");
		}

		// Seam 1: if P7's slot ceases to match the fit-field stamp mid-read while the fit-field ring index itself stays
		// valid, ReadFitField's FitFieldMatchesP7 re-check still rejects the copy and counts fieldUnreadable.
		TEST_METHOD(ReadFitFieldRereadRejectsStampDriftMidRead)
		{
			ReadFitFieldRig rig;
			rig.PrepareAndFit(1);
			VpSkeleton::SlotSet* p7 = rig.lane.Table()->Find(VpSkeleton::kP7CameraFit);
			VpSkeleton::SlotSet* pf = rig.lane.Table()->Find(VpSkeleton::kCameraFitField);
			Assert::IsNotNull(p7);
			Assert::IsNotNull(pf);
			const VpSkeleton::ReadInfo pre = pf->Read(1, true);
			Assert::IsTrue(pre.kind == VpSkeleton::ReadInfo::Ring, L"premise: fit-field slot is Ring before hook");

			struct HookCtx
			{
				VpSkeleton::SlotSet* p7 = nullptr;
				int calls = 0;
			} ctx{ p7, 0 };

			auto hook = [](uint64_t seq, void* raw) {
				HookCtx* c = static_cast<HookCtx*>(raw);
				++c->calls;
				// Reject P7 mid-read while leaving pf untouched on the same ring index: b.kind == Ring && b.index == a.index
				// holds, so only FitFieldMatchesP7 catches the mid-read stamp/state drift.
				c->p7->Seal(seq, [](const VpAncestry::Header&) { return VpSkeleton::SealReason::StampMismatch; });
			};

			VpSkeleton::IGapWork* gap = rig.fill->GapWork();
			{
				CameraModel::CameraTestSeams::ScopedReadFitFieldHook guard(hook, &ctx);
				Assert::IsTrue(gap->HasWork(), L"frame 1 must be ready before hook fires");
				gap->RunChunk(1000);
			}

			Assert::AreEqual(1, ctx.calls, L"pre-reread hook must fire once");
			const VpSkeleton::ReadInfo post = pf->Read(1, true);
			Assert::IsTrue(post.kind == VpSkeleton::ReadInfo::Ring, L"premise: fit-field slot itself stayed Ring");
			Assert::AreEqual(pre.index, post.index, L"premise: fit-field ring index stayed unchanged so FitFieldMatchesP7 is the rejecting check");
			Assert::AreEqual(0, rig.device.produced, L"stamp-drifted fit field must never reach label device");
			Assert::AreEqual<uint64_t>(1, rig.fill->Counters().fieldUnreadable.load(), L"FitFieldMatchesP7 re-check failure must count fieldUnreadable");
			Assert::AreEqual<uint64_t>(1, rig.fill->Counters().laneBlocked.load(), L"P8 publish must be blocked");
		}
	};

	TEST_CLASS(CameraLaneSlotSizeSeamTests)
	{
	public:
		// Seam 2: when the fit-field product slot is even 1 byte below WorstCaseFitFieldPodBytes(w, h),
		// CameraLane::Build refuses visibly with an error naming slot bytes, required bytes, and dimensions.
		TEST_METHOD(CameraLaneBuildRejectsUndersizedFitFieldSlotVisibly)
		{
			const uint32_t w = 1920, h = 1080;
			const size_t worst = CameraLink::WorstCaseFitFieldPodBytes(w, h);
			Assert::IsTrue(worst > 64, L"premise: worst-case 1080p fit-field pod exceeds 64 bytes");

			// 1 byte short of the exact worst-case bound.
			{
				CameraModel::CameraTestSeams::ScopedFitFieldSlotBytesOverride guard(worst - 1);
				CameraLink::CameraLane lane;
				Assert::IsFalse(lane.Build(w, h, true), L"1-byte-short slot must fail Build visibly");
				Assert::IsFalse(lane.Built(), L"failed Build must leave lane unbuilt");
				Assert::IsNull(lane.Table(), L"failed Build must publish no SlotTable");
				Assert::IsTrue(lane.Error().find("camera fit field product too small") != std::string::npos, L"error must name undersized fit-field product");
				Assert::IsTrue(lane.Error().find("slot " + std::to_string(worst - 1) + " B") != std::string::npos, L"error must report actual slot bytes");
				Assert::IsTrue(lane.Error().find("worst-case field " + std::to_string(worst) + " B") != std::string::npos, L"error must report required worst-case bytes");
				Assert::IsTrue(lane.Error().find(std::to_string(w) + "x" + std::to_string(h)) != std::string::npos, L"error must report source dimensions");
			}

			// Severely undersized slot (64 B).
			{
				CameraModel::CameraTestSeams::ScopedFitFieldSlotBytesOverride guard(64);
				CameraLink::CameraLane lane;
				Assert::IsFalse(lane.Build(w, h, true), L"64-byte slot must fail Build visibly");
				Assert::IsTrue(lane.Error().find("slot 64 B") != std::string::npos, L"error must include overridden 64 B slot size");
			}
		}

		// Seam 2: exact worst-case slot size succeeds (non-strict inequality), and disarming restores declared sizing.
		TEST_METHOD(CameraLaneBuildAcceptsExactWorstCaseSlotSizeAndDisarmsCleanly)
		{
			const uint32_t w = 1920, h = 1080;
			const size_t worst = CameraLink::WorstCaseFitFieldPodBytes(w, h);

			{
				CameraModel::CameraTestSeams::ScopedFitFieldSlotBytesOverride guard(worst);
				CameraLink::CameraLane lane;
				Assert::IsTrue(lane.Build(w, h, true), L"exact worst-case slot size must be accepted");
				Assert::IsTrue(lane.Built());
				Assert::IsTrue(lane.Error().empty(), L"successful Build must leave Error empty");
				const VpSkeleton::LaneLease fl = lane.Lease(VpSkeleton::kCameraFitField);
				Assert::IsNotNull(fl.bytes, L"fit-field CpuBuffers must be allocated");
				Assert::AreEqual<size_t>(worst, fl.bytes->Bytes(), L"allocated slot size must equal exact override");
			}

			// Guard exited -> seam disarmed -> declared product size (>= worst) used.
			Assert::AreEqual<size_t>(0, CameraModel::CameraTestSeams::TakeFitFieldSlotBytesOverride(), L"RAII guard must disarm to 0");
			CameraLink::CameraLane lane;
			Assert::IsTrue(lane.Build(w, h, true), L"unarmed Build must succeed with declared size");
			const VpSkeleton::LaneLease fl = lane.Lease(VpSkeleton::kCameraFitField);
			Assert::IsNotNull(fl.bytes);
			Assert::IsTrue(fl.bytes->Bytes() >= worst, L"declared slot size must cover worst-case fit-field pod");
		}
	};

	TEST_CLASS(CameraVerdictCounterSeamTests)
	{
	private:
		static CameraSynth::Field MakePanField640x320(double tx, double ty, bool withBackward = false)
		{
			CameraSynth::GridSpec g;
			g.w = 40;
			g.h = 20;
			g.blockSize = 16;
			CameraSynth::Field f(g, R3Scenes::Pic(640, 320));
			f.FromModel(CameraSynth::PanModel(tx, ty), withBackward);
			return f;
		}

		static CameraModel::PairInput MakePairInput(const CameraSynth::Field& f, uint64_t seq)
		{
			return f.Input(1.0 / 24.0, seq);
		}

	public:
		// Seam 4: PhotometricVerdict is invoked exactly once when R3 photometric refinement actually runs (both on
		// Refined and on FellBack), and zero times when photometric input is absent, skipped (no luma / resolution), or cut.
		TEST_METHOD(PhotometricVerdictCalledOnceWhenRefineRunsAndZeroWhenSkipped)
		{
			using namespace CameraModel;
			const int w = 640, h = 320;
			const double s = 1.0;
			Assert::IsTrue(PhotometricFormPasses(s, static_cast<double>(h)), L"premise: s=1 passes resolution rule at 320 rows");
			Assert::IsFalse(PhotometricFormPasses(2.0, static_cast<double>(h)), L"premise: s=2 fails resolution rule at 320 rows");

			const std::vector<R3Scenes::Wave> waves = R3Scenes::MakeWaves(42, s, 45.0);
			const std::vector<uint8_t> imgA = R3Scenes::To8(R3Scenes::RenderD(w, h, s, { R3Scenes::Shifted(waves, 0.0, 0.0) }, 1.0, 0.0, 101, 0.0));
			const std::vector<uint8_t> imgB = R3Scenes::To8(R3Scenes::RenderD(w, h, s, { R3Scenes::Shifted(waves, 8.0, 0.0) }, 1.0, 0.0, 102, 0.0));
			const std::vector<uint8_t> imgOpposite = R3Scenes::To8(R3Scenes::RenderD(w, h, s, { R3Scenes::Shifted(waves, -8.0, 0.0) }, 1.0, 0.0, 103, 0.0));

			const CameraSynth::Field biasedField = MakePanField640x320(8.4, 0.0);

			CameraTestSeams::ScopedVerdictCallCounter counter;
			Fitter fitter;

			// (A) No PhotometricInput -> 0 verdict calls.
			{
				PairInput in = MakePairInput(biasedField, 1);
				const CameraMotion r = fitter.Fit(in);
				Assert::IsTrue(r.valid && r.reason == Reason::Ok);
				Assert::AreEqual<uint64_t>(0, counter.Calls(), L"no photometric input: 0 verdict calls");
			}

			// (B) Empty LumaPairView -> SkippedNoLuma, 0 verdict calls.
			{
				PairInput in = MakePairInput(biasedField, 2);
				PhotometricInput pi;
				in.photometric = &pi;
				fitter.Fit(in);
				Assert::IsTrue(pi.outcome == PhotometricOutcome::SkippedNoLuma);
				Assert::AreEqual<uint64_t>(0, counter.Calls(), L"SkippedNoLuma: 0 verdict calls");
			}

			// (C) Coarse luma (s = 2 at 320 rows) -> SkippedResolution, 0 verdict calls.
			{
				PairInput in = MakePairInput(biasedField, 3);
				PhotometricInput pi;
				pi.luma = R3Scenes::View8(imgA, imgB, w, h, 2.0);
				in.photometric = &pi;
				fitter.Fit(in);
				Assert::IsTrue(pi.outcome == PhotometricOutcome::SkippedResolution);
				Assert::AreEqual<uint64_t>(0, counter.Calls(), L"SkippedResolution: 0 verdict calls");
			}

			// (D) Valid luma pair matching true pan (8.0, 0.0) vs biased vectors (8.4, 0.0) -> Refined, +1 verdict call.
			{
				PairInput in = MakePairInput(biasedField, 4);
				PhotometricInput pi;
				pi.luma = R3Scenes::View8(imgA, imgB, w, h, s);
				in.photometric = &pi;
				const CameraMotion r = fitter.Fit(in);
				Assert::IsTrue(r.valid && pi.outcome == PhotometricOutcome::Refined, L"biased vectors must be refined by true luma pair");
				Assert::AreEqual<uint64_t>(1, counter.Calls(), L"Refined path must invoke PhotometricVerdict exactly once");
			}

			// (E) Opposite-pan luma pair -> FellBack, +1 verdict call (total = 2).
			{
				PairInput in = MakePairInput(biasedField, 5);
				PhotometricInput pi;
				pi.luma = R3Scenes::View8(imgA, imgOpposite, w, h, s);
				in.photometric = &pi;
				const CameraMotion r = fitter.Fit(in);
				Assert::IsTrue(r.valid && pi.outcome == PhotometricOutcome::FellBack, L"opposite luma must fall back");
				Assert::AreEqual<uint64_t>(2, counter.Calls(), L"FellBack path must still invoke PhotometricVerdict exactly once");
			}

			// (F) Cut frame without next pair -> NotAttempted, +0 verdict calls (total stays 2).
			{
				PairInput in = MakePairInput(biasedField, 6);
				in.cut = true;
				PhotometricInput pi;
				pi.luma = R3Scenes::View8(imgA, imgB, w, h, s);
				in.photometric = &pi;
				const CameraMotion r = fitter.Fit(in);
				Assert::IsTrue(r.reason == Reason::Cut && pi.outcome == PhotometricOutcome::NotAttempted);
				Assert::AreEqual<uint64_t>(2, counter.Calls(), L"cut frame must not invoke PhotometricVerdict");
			}
		}

		// Seam 4: when ScopedVerdictCallCounter is not armed (production), PhotometricVerdict does not touch the counter
		// and returns the refine result unchanged.
		TEST_METHOD(PhotometricVerdictCounterIsInertWhenDisarmed)
		{
			using namespace CameraModel;
			Assert::IsFalse(CameraTestSeams::VerdictCounter().armed.load(std::memory_order_acquire), L"must start disarmed");
			const uint64_t before = CameraTestSeams::VerdictCounter().calls.load(std::memory_order_acquire);
			RefineResult in;
			in.status = RefineStatus::Refined;
			in.model = R3Scenes::Translation(3.25, -1.5);
			const RefineResult out = PhotometricVerdict(R3Scenes::Translation(3.0, -1.0), in, 0.25);
			Assert::IsTrue(out.status == RefineStatus::Refined);
			Assert::AreEqual(3.25, out.model.h[2]);
			Assert::AreEqual(-1.5, out.model.h[5]);
			Assert::AreEqual(before, CameraTestSeams::VerdictCounter().calls.load(std::memory_order_acquire),
				L"disarmed PhotometricVerdict must not increment counter");
		}
	};
}
