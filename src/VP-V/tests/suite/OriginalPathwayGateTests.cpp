// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// PHASE 1 of the host seams plan (refs/107 s2.4 items 1 and 3; owner E3 2026-11-11: a VP-V call site in a Bill file does
// NOTHING in the Original pathway). Module side, CPU only, table-driven over both pathways: the hub's per-host pathway
// fact; IngressPolicy R15 / R19; VectorEngineExtension::OnQueue (+ the one invalidation when the settings leave
// Original); DejudderExtension::OnQueue / OnPlanOutputs; RenderAhead::Presenter reporting (+ the hand-over on entering
// Original); the capture-thread fix switches synced from the active settings at the first arrival (the FIRST-FRAMES
// defect); ReleaseVpTimeline at the settings seam. The Impl bodies in PhaseTimeline.inl (VpStageBFrame,
// DrawVpPhasePicture, PlanVpOutputs, RegisterDejudderCacheFrame, NoteTimelinePlanned) need the real renderer:
// OriginalPathwayByteIdenticalToBill (RendererCaptureHarnessTests, GPU) covers them.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/OriginalPathway.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/input/queue_timing/IngressPolicy.h>
#include <VP-V/modules/input/queue_timing/IngressState.h>
#include <VP-V/modules/motion/consumers/DejudderExtension.h>
#include <VP-V/modules/motion/consumers/TimelinePlanner.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineExtension.h>
#include <VP-V/modules/output/presentation/RenderAheadPolicy.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		namespace RE = RendererExtension;

		Vpv::Hub::ExtensionHub& Hub() { return Vpv::Hub::ExtensionHub::Instance(); }

		// The hook layer forwarding to the hub, as the renderer has it; a clean registry before and after.
		struct HubScope
		{
			struct Fwd : RE::IRendererExtension
			{
				void OnLifecycle(RE::LifecycleFacts& f) override { Hub().OnLifecycle(f); }
				void OnQueue(const RE::QueueFacts& f) override { Hub().OnQueue(f); }
				bool OnFrame(RE::FrameContext& c) override { return Hub().OnFrame(c); }
			};
			HubScope()
			{
				RE::ResetForTesting();
				Hub().ResetModulesForTesting();
				RE::Register(std::unique_ptr<RE::IRendererExtension>(new Fwd()));
			}
			~HubScope()
			{
				RE::ResetForTesting();
				Hub().ResetModulesForTesting();
			}
		};

		struct Row { bool originalPathway; bool acts; const wchar_t* name; };
		const Row kRows[] = {
			{ true, false, L"Original pathway: the call site does nothing" },
			{ false, true, L"VP-V pathway: the module acts" },
			{ true, false, L"Original pathway again: nothing" },
		};

		struct FakeRenderer
		{
			int calls = 0;
			static bool Decide(void* self, RE::IngressContext& ic)
			{
				++static_cast<FakeRenderer*>(self)->calls;
				ic.drop = false;
				ic.filmHz = 23.976;
				ic.gapBefore = true;
				ic.gapCause = "pulldown lock change";
				ic.arrivalQpc = 1234;
				return true;
			}
			static void TargetFive(const void*, RE::QueueTargetContext& qc) { qc.target = 5; }
		};

		void SettingsApplied(const void* host, bool original)
		{
			RE::NotifyLifecycle(RE::Lifecycle::SettingsApplied, host, nullptr, nullptr, nullptr, nullptr, original ? 1 : 0);
		}
	}

	TEST_CLASS(OriginalPathwayGateTests)
	{
	public:
		// The hub keeps one pathway fact per host: set at SettingsApplied (code != 0) after the modules saw the call (a
		// module's own handler reads the PREVIOUS fact), dropped with the host; no fact = VP-V.
		TEST_METHOD(TheHubRecordsThePathwayAtSettingsAppliedAfterTheModulesSawIt)
		{
			HubScope scope;
			struct Probe : Vpv::Hub::ISkeletonModule
			{
				std::vector<bool> seen;
				const char* Name() const override { return "probe"; }
				void OnLifecycle(RE::LifecycleFacts& f) override
				{
					if (f.kind == RE::Lifecycle::SettingsApplied)
						seen.push_back(Hub().OriginalPathway(f.host));
				}
			};
			Probe* probe = new Probe();
			Hub().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(probe));
			int host = 0, other = 0;
			Assert::IsFalse(Hub().OriginalPathway(&host), L"no fact: VP-V");
			SettingsApplied(&host, true);
			Assert::IsTrue(Hub().OriginalPathway(&host));
			Assert::IsFalse(Hub().OriginalPathway(&other), L"another host keeps its own fact");
			SettingsApplied(&host, false);
			Assert::IsFalse(Hub().OriginalPathway(&host));
			SettingsApplied(&host, true);
			Assert::IsTrue(Hub().OriginalPathway(&host));
			Assert::AreEqual<size_t>(3, probe->seen.size());
			Assert::IsTrue(!probe->seen[0] && probe->seen[1] && !probe->seen[2], L"a module sees the previous fact during SettingsApplied");
			RE::NotifyLifecycle(RE::Lifecycle::GpuReleased, &host);
			Assert::IsFalse(Hub().OriginalPathway(&host), L"dropped with the host");
			Assert::IsFalse(Hub().NoteOriginalPathway(nullptr, true), L"no host, no fact");
		}

		// R15 DecideIngress: Original = not handled (Bill's pre-filled facts stand, the renderer's decision never runs);
		// R19 ResolveQueueTarget: Original = Bill's own target stands.
		TEST_METHOD(IngressDecideAndQueueTargetDoNothingInTheOriginalPathway)
		{
			HubScope scope;
			Hub().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new Vpv::Ingress::IngressPolicy()));
			FakeRenderer a;
			Vpv::Ingress::HostBinding bind(&a, &FakeRenderer::Decide, &a, &FakeRenderer::TargetFive);
			Assert::IsTrue(bind.Registered());
			Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&a), L"a fresh binding: VP-V until the renderer says otherwise");
			for (const Row& row : kRows)
			{
				Vpv::Ingress::NoteOriginalPathway(&a, row.originalPathway);
				Assert::AreEqual(row.originalPathway, Vpv::Ingress::OriginalPathwayOf(&a), row.name);
				const int callsBefore = a.calls;
				RE::IngressContext ic;
				ic.host = &a;
				ic.gapBefore = true;
				ic.gapCause = "missed frames";
				ic.arrivalQpc = 77;
				Assert::AreEqual(row.acts, RE::DecideIngress(ic), row.name);
				Assert::AreEqual(callsBefore + (row.acts ? 1 : 0), a.calls, row.name);
				if (row.acts)
				{
					Assert::AreEqual<int64_t>(1234, ic.arrivalQpc, row.name);
					Assert::AreEqual(23.976, ic.filmHz, row.name);
				}
				else
				{
					Assert::IsFalse(ic.drop, row.name);
					Assert::IsTrue(ic.gapBefore, row.name);
					Assert::AreEqual(std::string("missed frames"), std::string(ic.gapCause), row.name);
					Assert::AreEqual<int64_t>(77, ic.arrivalQpc, L"Original: the host's arrival stands");
					Assert::AreEqual(0.0, ic.filmHz, L"Original: no film clock");
				}
				RE::QueueTargetContext qc;
				qc.capacity = 8;
				Assert::AreEqual<size_t>(row.acts ? 5 : 4, RE::ResolveQueueTarget(&a, qc, 4), row.name);
			}
			int unbound = 0;
			Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&unbound), L"no binding: VP-V (as before)");
		}

		// FIRST-FRAMES defect (refs/107 s2.4 item 3): as built, every fix switch starts TRUE and only the render loop
		// refreshed it. The first arrival now syncs them from the active settings before any reader: in the Original
		// pathway the five switches are Bill's (off, the OriginalPathway.h rows) from frame one, and the pathway fact
		// changes exactly when the settings do (the binding is told then).
		TEST_METHOD(TheCaptureThreadSwitchesFollowTheActiveSettingsFromTheFirstArrival)
		{
			struct Settings
			{
				bool pulldownFixes, timingContinuityFixes, cadenceClassFixes, queueTimingFixes, motionHistoryFixes;
				bool pipelineOriginalPathway;
			};
			const char* const keys[] = { "queue_timing_fixes", "pulldown_fixes", "timing_continuity_fixes", "cadence_class_fixes",
				"motion_history_fixes" };
			for (const char* key : keys)
			{
				bool found = false;
				for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
					if (std::strcmp(o.key, key) == 0)
					{
						found = true;
						Assert::AreEqual(std::string("off"), std::string(o.value), L"the Original table forces the switch off");
					}
				Assert::IsTrue(found, L"every fix switch is an Original table row");
			}
			const Settings original = { false, false, false, false, false, true };   // the table applied (ApplyOriginal)
			const Settings vpv = { true, true, true, true, true, false };             // the defaults on the VP-V pathway
			Vpv::Ingress::IngressState st;
			std::atomic<bool> queueTimingFixes{ true }, motionHistoryFixes{ true };   // the renderer's two, as declared
			Assert::IsTrue(st.m_pulldownFixes.load() && st.m_timingContinuityFixes.load() && st.m_cadenceClassFixes.load(),
				L"as built: on before any arrival");
			Assert::IsFalse(st.m_originalPathway.load());
			Assert::IsTrue(st.SyncIngressSwitches(Vpv::Ingress::IngressState::FixSwitchesOf(original), queueTimingFixes, motionHistoryFixes),
				L"the first Original arrival: the pathway fact changed");
			Assert::IsFalse(st.m_pulldownFixes.load() || st.m_timingContinuityFixes.load() || st.m_cadenceClassFixes.load() ||
				queueTimingFixes.load() || motionHistoryFixes.load(), L"Original: Bill's switches from the first frame");
			Assert::IsTrue(st.m_originalPathway.load());
			Assert::IsFalse(st.SyncIngressSwitches(Vpv::Ingress::IngressState::FixSwitchesOf(original), queueTimingFixes, motionHistoryFixes),
				L"the next arrival: unchanged, the binding is not told again");
			Assert::IsTrue(st.SyncIngressSwitches(Vpv::Ingress::IngressState::FixSwitchesOf(vpv), queueTimingFixes, motionHistoryFixes),
				L"back on VP-V: changed");
			Assert::IsTrue(st.m_pulldownFixes.load() && st.m_timingContinuityFixes.load() && st.m_cadenceClassFixes.load() &&
				queueTimingFixes.load() && motionHistoryFixes.load(), L"VP-V: the user's switches");
			Assert::IsFalse(st.m_originalPathway.load());
		}

		// The renderer's queue events are not motion invalidations in Bill's pathway; leaving Original at SettingsApplied
		// requests ONE Resync invalidation (the events of the Original stretch were ignored); other applies request none.
		TEST_METHOD(QueueEventsDoNothingInTheOriginalPathwayAndLeavingItInvalidatesOnce)
		{
			HubScope scope;
			Hub().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new VpMotion::VectorEngineExtension()));
			int host = 0;
			VpMotion::VectorEngine engine(&host);
			MotionValidity::Pending pending;
			for (const Row& row : kRows)
			{
				Hub().NoteOriginalPathway(&host, row.originalPathway);
				const uint64_t before = engine.motionValidity.Epoch();
				RE::NotifyQueue(RE::QueueEvent::Reset, &host);
				RE::NotifyQueue(RE::QueueEvent::Gap, &host, -1, 0, 0, 0, "missed frames");
				RE::NotifyQueue(RE::QueueEvent::FormatChange, &host);
				Assert::AreEqual(row.acts, engine.motionValidity.Epoch() != before, row.name);
				Assert::AreEqual(row.acts, engine.TakeInvalidation(pending), row.name);
				for (int i = 0; i < 4 && engine.TakeInvalidation(pending); ++i) {}
			}
			// Original -> VP-V: one Resync invalidation.
			Assert::IsTrue(Hub().OriginalPathway(&host));
			uint64_t before = engine.motionValidity.Epoch();
			SettingsApplied(&host, false);
			Assert::IsTrue(engine.motionValidity.Epoch() != before, L"leaving Original moves the epoch once");
			Assert::IsTrue(engine.TakeInvalidation(pending));
			Assert::IsTrue(pending.cause == MotionValidity::Cause::Resync);
			Assert::IsFalse(Hub().OriginalPathway(&host));
			// VP-V -> VP-V and VP-V -> Original: nothing.
			before = engine.motionValidity.Epoch();
			SettingsApplied(&host, false);
			SettingsApplied(&host, true);
			Assert::IsTrue(engine.motionValidity.Epoch() == before, L"other applies request nothing");
			Assert::IsFalse(engine.TakeInvalidation(pending));
			Assert::IsTrue(Hub().OriginalPathway(&host));
		}

		// The backlog flush steers no VP timeline and the frame is not planned (Bill's list is drawn) in Original.
		TEST_METHOD(DejudderQueueFlushAndPlanOutputsDoNothingInTheOriginalPathway)
		{
			HubScope scope;
			Hub().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new VpMotion::DejudderExtension()));
			struct FakeImpl { int flushes = 0, plans = 0; } impl;
			VpMotion::DejudderBindingSpec spec;
			spec.self = &impl;
			spec.queueFlush = [](void* self) { ++static_cast<FakeImpl*>(self)->flushes; };
			spec.planOutputs = [](void* self, RE::OutputPlan& plan)
			{
				++static_cast<FakeImpl*>(self)->plans;
				plan.skip = true;
				return true;
			};
			VpMotion::DejudderHostBinding binding(&impl, spec);
			Assert::IsTrue(binding.Registered());
			int flushes = 0, plans = 0;
			for (const Row& row : kRows)
			{
				Hub().NoteOriginalPathway(&impl, row.originalPathway);
				RE::NotifyQueue(RE::QueueEvent::BacklogFlush, &impl);
				flushes += row.acts ? 1 : 0;
				Assert::AreEqual(flushes, impl.flushes, row.name);
				RE::OutputPlan plan;
				plan.seq = 7;
				Assert::AreEqual(row.acts, RE::PlanOutputs(&impl, plan), row.name);
				plans += row.acts ? 1 : 0;
				Assert::AreEqual(plans, impl.plans, row.name);
				Assert::AreEqual(row.acts, plan.skip, row.name);
			}
		}

		// The Presenter's reports (Break / ResetGate / UpdateGate / TakeMainInterval) return at once in Original; entering
		// Original hands over once (chain broken, credit returned, gate reset); leaving it, the reports act again.
		TEST_METHOD(PresenterReportingDoesNothingInTheOriginalPathwayAndEnteringItHandsOver)
		{
			RenderAhead::Presenter p;
			p.creditFrames.store(p.credit.CommitFrames(1, 2.5));
			p.mainInterval = 3;
			p.targetLeadMs = 12.0;
			p.estimatedLeadMs.store(9.0);
			for (int i = 0; i < 20; ++i)
				p.newFrameCost.Add(i * 0.01, 30.0);
			Assert::IsTrue(p.creditFrames.load() > 0, L"a VP-V plan in flight");
			const unsigned long long resets = p.gateResets;
			Assert::IsTrue(p.SetOriginalPathway(true, 10.0), L"entered");
			Assert::AreEqual<size_t>(0, p.creditFrames.load(), L"hand-over: the credit is returned at once");
			Assert::AreEqual(1, p.mainInterval);
			Assert::AreEqual(0.0, p.targetLeadMs);
			Assert::AreEqual(0.0, p.estimatedLeadMs.load());
			Assert::IsFalse(p.schedule.Active());
			Assert::AreEqual(resets + 1, p.gateResets, L"hand-over: the gate epoch starts again");
			Assert::AreEqual<size_t>(0, p.newFrameCost.Count());
			Assert::IsFalse(p.SetOriginalPathway(true, 11.0), L"already in: no second hand-over");
			Assert::AreEqual(resets + 1, p.gateResets);
			// In Original every report does nothing.
			p.mainInterval = 3;
			p.Break(12.0);
			Assert::AreEqual(3, p.mainInterval, L"Break: nothing");
			Assert::AreEqual(0, p.ResetGate(), L"ResetGate: nothing, level 0");
			Assert::AreEqual(resets + 1, p.gateResets);
			for (int i = 0; i < 20; ++i)
				p.newFrameCost.Add(i * 0.01, 30.0);
			RenderAhead::PresenterGateFrame frame;
			frame.sourcePeriod = 1.0 / 24.0;
			frame.displayPeriod = 1.0 / 60.0;
			frame.late = true;
			frame.maxExtras = 2;
			const RenderAhead::PresenterGateResult gate = p.UpdateGate(13.0, frame);
			Assert::IsFalse(gate.evaluated, L"UpdateGate: not evaluated");
			Assert::AreEqual(0, gate.level);
			Assert::AreEqual<size_t>(20, p.newFrameCost.Count(), L"UpdateGate: the windows are not even trimmed");
			Assert::AreEqual(1, p.TakeMainInterval(3), L"TakeMainInterval: Bill's SyncInterval 1");
			Assert::AreEqual(1, p.lastMainInterval);
			Assert::AreEqual(0ull, p.intervalPresents);
			// Leaving Original: the reports act again.
			Assert::IsFalse(p.SetOriginalPathway(false, 14.0));
			Assert::AreEqual(3, p.TakeMainInterval(3));
			Assert::AreEqual(1ull, p.intervalPresents);
			p.mainInterval = 3;
			p.Break(15.0);
			Assert::AreEqual(1, p.mainInterval);
			Assert::AreEqual(0, p.ResetGate());
			Assert::AreEqual(resets + 2, p.gateResets);
			Assert::AreEqual<size_t>(0, p.newFrameCost.Count());
		}

		// The settings seam releases the per-vsync timeline once when Original is entered (the gated plan never runs again
		// to release it): flags cleared, the "released" line once, idempotent afterwards.
		TEST_METHOD(ReleaseVpTimelineReleasesTheTimelineOnceAtTheSettingsSeam)
		{
			VpMotion::DejudderPolicyState st;
			st.vpTimelineActive = true;
			st.vpSkipOutput = true;
			st.vpSkipIsUpdate = true;
			st.vpOutputPhase = 0.5;
			st.vpTimelineLogged = true;
			std::vector<std::string> log;
			auto take = [&](const char* what) { log.push_back(what); };
			VpMotion::ReleaseVpTimeline(st, take);
			Assert::IsFalse(st.vpTimelineActive || st.vpSkipOutput || st.vpSkipIsUpdate);
			Assert::AreEqual(0.0, st.vpOutputPhase);
			Assert::IsFalse(st.vpTimelineLogged);
			Assert::AreEqual<size_t>(1, log.size());
			Assert::AreEqual(std::string("released"), log[0]);
			VpMotion::ReleaseVpTimeline(st, take);
			Assert::AreEqual<size_t>(1, log.size(), L"idempotent: no second line");
		}
	};
}
