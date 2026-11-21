// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Package D: Camera Test Seams (CPU-only deterministic tests using production-inert atomic seams in CameraTestSeams.h).
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/CameraTestSeams.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitFieldPod.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLabelFill.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLane.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>

#include <memory>
#include <string>

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
}
