// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// PHASE 2 of the Original gating (refs/113 AUDIT-R of b6bdfc9fb: F2, F3, L8; owner E3 2026-11-11, rule 32): CPU tests for
// the module-side facts behind the four fix-r26 call sites that had only the checker and the GPU harness
// (VpStageBFrame, rebaseWarmup, DrawVpPhasePicture, CollectQueueAhead / frameAnalysis.OnArrival). Every one of those Bill
// lines reads ONE predicate, `pipelineSlots.OriginalPathway()` (F3: an atomic now, read off the render thread by the
// conversion loop), and the settings seam hands the VP-V state over when Original is entered (F2: the re-base warm-up,
// whose per-frame call is gated in Original, is idled there). Style: SnapshotLeaseGateTests (table-driven, both pathways).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysisHost.h>
#include <VP-V/modules/motion/consumers/DejudderState.h>
#include <VP-V/modules/motion/consumers/DejudderViews.h>
#include <VP-V/modules/motion/consumers/TimelinePlanner.h>
#include <VP-V/modules/scaling/rebase/RebaseWarmupJob.h>
#include "../SyntheticAnalysisFrames.h"

#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(OriginalPathwaySiteGateTests)
	{
	public:
		struct Row { bool originalPathway; bool acts; const wchar_t* name; };

		// F3 + the predicate of the five fix-r26 sites (LVR :12256, :19180, :23245, :24542, :30738 / :31142): the registry's
		// Original fact survives the wholesale copy / assignment the settings seam does (`pipelineSlots = next;`), and in
		// Original the re-base slot (origin Vp) does not run, so the warm-up's `applies` term is false there as well.
		TEST_METHOD(TheRegistryFactSurvivesCopyAndAssignmentAndStopsTheVpSlots)
		{
			using PipelineSlots::Id;
			const Row rows[] = {
				{ true, false, L"Original pathway: the fact is set, the VP slots are off" },
				{ false, true, L"VP-V pathway: the fact is clear" },
			};
			for (const Row& row : rows)
			{
				PipelineSlots::Registry built;
				if (row.originalPathway)
					built.ApplyOriginalPathway();
				Assert::AreEqual(row.originalPathway, built.OriginalPathway(), row.name);
				if (row.originalPathway)
				{
					Assert::IsFalse(built.Enabled(Id::ScalingRebase), L"Original: scaling.rebase (origin Vp) is disabled");
					Assert::IsFalse(built.Runs(Id::ScalingRebase), L"Original: the warm-up's `applies` term is false");
				}
				// The seam's copy and assignment (an atomic member needs explicit ones).
				const PipelineSlots::Registry copied(built);
				Assert::AreEqual(row.originalPathway, copied.OriginalPathway(), L"copy constructor");
				Assert::AreEqual(built.Runs(Id::ScalingRebase), copied.Runs(Id::ScalingRebase));
				PipelineSlots::Registry assigned;
				assigned.ApplyOriginalPathway();   // the opposite state first, so the assignment is seen to overwrite it
				Assert::IsTrue(assigned.OriginalPathway());
				assigned = built;
				Assert::AreEqual(row.originalPathway, assigned.OriginalPathway(), L"copy assignment");
				Assert::AreEqual(built.Runs(Id::ScalingRebase), assigned.Runs(Id::ScalingRebase));
				Assert::AreEqual(built.IsDefaultOrder(), assigned.IsDefaultOrder());
				const PipelineSlots::Registry& self = assigned;
				assigned = self;   // self-assignment keeps the state
				Assert::AreEqual(row.originalPathway, assigned.OriginalPathway(), L"self-assignment");
			}
		}

		// F2: the host's per-frame warm-up call (LVR :23245) is gated in Original, so a warm-up announced on the VP-V pathway
		// would keep `Pending()` (the "Preparing shaders..." status) and its fallback entry until Stop(). The settings seam
		// entering Original idles it instead: pending off, the entry released once, idempotent; back on the VP-V pathway the
		// candidate is evaluated afresh and held again (the factors in force were forgotten).
		TEST_METHOD(EnteringOriginalIdlesAPendingRebaseWarmup)
		{
			struct pl_fmt_t format{};
			format.name = "r16";
			format.num_components = 1;
			struct pl_tex_t sourceTexture{}, targetTexture{};
			sourceTexture.params.w = 1920;
			sourceTexture.params.h = 1080;
			sourceTexture.params.format = &format;
			targetTexture.params.w = 3840;
			targetTexture.params.h = 2160;
			targetTexture.params.format = &format;
			pl_frame source{}, target{};
			source.num_planes = 1;
			source.planes[0].texture = &sourceTexture;
			source.planes[0].components = 1;
			target.num_planes = 1;
			target.planes[0].texture = &targetTexture;
			target.planes[0].components = 1;
			pl_render_params params{};
			// No render device: the worker's job is refused at once (recorded as a failed warm-up of the key); the gate's
			// first answer is the deterministic part: the candidate is Missing, so the frame is held and `pending` is set.
			const VpRebaseWarmup::Context context{ source, target, params, nullptr, nullptr, true, 1ULL, false, 0 };

			VpRebaseWarmup::Controller warmup;
			Assert::IsFalse(warmup.Pending(), L"nothing announced yet");
			const VpRebaseWarmup::Controller::Outcome first = warmup.Frame(true, context, 0.5, 0.5, 1.0, 1.0);
			Assert::IsTrue(first.hold, L"VP-V: the frame is held on the current path while the shaders warm");
			Assert::AreEqual(1.0, first.x);
			Assert::AreEqual(1.0, first.y);
			Assert::IsTrue(warmup.Pending(), L"VP-V: the warm-up is pending (\"Preparing shaders...\")");
			Assert::IsTrue(first.fallbackChanged && first.fallback.engaged, L"VP-V: the fallback entry is engaged");
			Assert::AreEqual(std::string("re-base warm-up"), first.fallback.id);

			// The seam enters Original: idle at once, the entry released, nothing else.
			const VpRebaseWarmup::Controller::Outcome entered = warmup.EnterOriginalPathway();
			Assert::IsFalse(warmup.Pending(), L"Original: no pending warm-up outlives the switch");
			Assert::IsTrue(entered.fallbackChanged, L"Original: the entry changes (released)");
			Assert::IsFalse(entered.fallback.engaged);
			Assert::AreEqual(std::string("re-base warm-up"), entered.fallback.id);
			Assert::IsFalse(entered.hold);
			const VpRebaseWarmup::Controller::Outcome again = warmup.EnterOriginalPathway();
			Assert::IsFalse(again.fallbackChanged, L"idempotent: a second entry changes nothing");
			Assert::IsFalse(warmup.Pending());

			// Back on the VP-V pathway the candidate is evaluated afresh: held again, the entry engaged again (whether the
			// worker has already recorded the refused job or not, the re-base does not run on cold shaders).
			const VpRebaseWarmup::Controller::Outcome resumed = warmup.Frame(true, context, 0.5, 0.5, 1.0, 1.0);
			Assert::IsTrue(resumed.hold, L"VP-V again: held, not let through on cold shaders");
			Assert::IsTrue(resumed.fallbackChanged && resumed.fallback.engaged, L"VP-V again: the entry is engaged again");
			// And an ordinary idle frame on the VP-V pathway releases it as before (the seam changed nothing there).
			const VpRebaseWarmup::Controller::Outcome idle = warmup.Frame(false, context, 1.0, 1.0, 1.0, 1.0);
			Assert::IsFalse(warmup.Pending());
			Assert::IsTrue(idle.fallbackChanged && !idle.fallback.engaged);
			warmup.Stop();
		}

		// The settings seam entering Original on a controller that never announced anything: nothing to release.
		TEST_METHOD(EnteringOriginalOnAnIdleWarmupChangesNothing)
		{
			VpRebaseWarmup::Controller warmup;
			const VpRebaseWarmup::Controller::Outcome entered = warmup.EnterOriginalPathway();
			Assert::IsFalse(entered.fallbackChanged);
			Assert::IsFalse(entered.hold);
			Assert::IsFalse(warmup.Pending());
		}

		// The conversion-loop site (LVR :30738, gated on the atomic fact): the store the seam configures for Original
		// (`Configure(store && !Original)`) records nothing on an arrival, no job runs and no shared plane is attached.
		TEST_METHOD(TheFrameAnalysisStoreConfiguredForOriginalRecordsNoArrival)
		{
			const Row rows[] = {
				{ true, false, L"Original pathway: the store is configured off, the arrival leaves nothing" },
				{ false, true, L"VP-V pathway: the store is on, the arrival leaves a record" },
			};
			for (const Row& row : rows)
			{
				VpFrameAnalysis::Host host;
				host.Configure(!row.originalPathway, false, true);   // a record reader only: no shared decode either way
				host.Worker().Stop();   // the producer runs inline when drained (no thread)
				SyntheticAnalysisFrames::Frame frame = SyntheticAnalysisFrames::Make(SyntheticAnalysisFrames::Kind::Uyvy, 1);
				VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
				Assert::IsTrue(frame.source.sharedLuma == nullptr, L"no full-luma reader: no shared plane");
				host.OnArrival(1, frame.source, FrameAnalysis::Rect{}, 0, false);
				host.Worker().Drain();
				Assert::AreEqual(row.acts, host.Store().HasRecord(1), row.name);
				FrameAnalysis::Record record;
				const FrameAnalysis::ReadStatus read = host.Store().Read(1, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kIdentity, record);
				Assert::AreEqual(row.acts, read == FrameAnalysis::ReadStatus::Ok, row.name);
				if (!row.acts)
					Assert::AreEqual(uint64_t(0), host.Worker().Processed(), L"Original: no job ran");
				host.Configure(false);
			}
		}

		// Stage B (LVR :19180) and the phase picture (:24542) are gated on the registry fact in the host AND in their module
		// bodies; what they would act on is the per-vsync timeline, which the seam releases when Original is entered: the
		// view the phase-timeline consumers read says "not active, no skip" afterwards.
		TEST_METHOD(TheTimelineViewBehindStageBAndThePhasePictureIsClearedAtTheSeam)
		{
			VpMotion::DejudderPolicyState st;
			const VpMotion::PhaseTimelineView view(st);
			st.vpTimelineActive = true;
			st.vpSkipOutput = true;
			st.vpSkipIsUpdate = true;
			st.vpOutputPhase = 0.25;
			Assert::IsTrue(view.Active() && view.SkipOutput(), L"VP-V: a timeline in flight would skip this frame's output");
			std::vector<std::string> log;
			VpMotion::ReleaseVpTimeline(st, [&](const char* what) { log.push_back(what); });
			Assert::IsFalse(view.Active(), L"Original: no timeline");
			Assert::IsFalse(view.SkipOutput(), L"Original: Stage B has nothing to skip");
			Assert::AreEqual(0.0, view.OutputPhase());
			Assert::IsFalse(st.vpSkipIsUpdate);
			Assert::IsTrue(log.empty(), L"nothing was logged as engaged: nothing to log as released");
		}
	};
}
