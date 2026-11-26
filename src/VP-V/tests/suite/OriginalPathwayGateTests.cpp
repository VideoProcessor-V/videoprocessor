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
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/OriginalPathway.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>
#include "../SettingsTestSupport.h"
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/input/queue_timing/IngressPolicy.h>
#include <VP-V/modules/input/queue_timing/IngressState.h>
#include <VP-V/modules/motion/consumers/DejudderExtension.h>
#include <VP-V/modules/motion/consumers/TimelinePlanner.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineExtension.h>
#include <VP-V/modules/output/presentation/RenderAheadPolicy.h>
#include <VP-V/core/FallbackStatus.h>
#include <VP-V/core/PipelineSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonSettings.h>
#include <VP-V/modules/input/queue_timing/VpFixedBudgetPolicy.h>
#include <VP-V/modules/input/signal/HdrInferenceChoice.h>
#include <ConfigFile.h>
#include <ShortcutDefaults.h>
#include <vprenderer/QueueTimingPolicy.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

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
		// GEM-031 (Gemini's test, config-schema MUST-1, owner Q4): on the Original pathway a saved one of Bill's five
		// chroma filters stays (no override); unset "auto" and the VP-only values become bicubic; no host value = bicubic.
		TEST_METHOD(OriginalChromaKeepsBillsFiltersAndForcesBicubicOtherwise)
		{
			for (const char* bill : { "ewa_lanczossharp", "ewa_lanczos", "lanczos", "bicubic", "bilinear" })
			{
				const std::string value = bill;
				Assert::IsTrue(OriginalPathway::IsBillChromaFilter(value), L"one of Bill's filters is not recognised");
				Assert::IsNull(OriginalPathway::ChromaOverride(&value), L"Bill's saved chroma filter is overridden");
			}
			for (const char* vp : { "auto", "catmull_rom", "upscaler", "" })
			{
				const std::string value = vp;
				Assert::IsFalse(OriginalPathway::IsBillChromaFilter(value), L"a VP-only chroma value counted as Bill's");
				const char* forced = OriginalPathway::ChromaOverride(&value);
				Assert::IsNotNull(forced, L"a VP-only chroma value reaches the Original pathway");
				Assert::AreEqual(std::string("bicubic"), std::string(forced));
			}
			Assert::AreEqual(std::string("bicubic"), std::string(OriginalPathway::ChromaOverride(nullptr)), L"no host value: bicubic");
			// The table still names bicubic as the Original value (the override and the table agree).
			std::string tableValue;
			for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
				if (std::string(o.key) == "chroma_upscaler") tableValue = o.value;
			Assert::AreEqual(std::string("bicubic"), tableValue);
		}

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

		// T-AR3-N2 (AUDIT-R 1fa78cdb9 N2): the capture / file threads read the six switches from a word the settings seam
		// published into the hub's per-host fact, never from the Impl's activeSettings. Round trip, per-host, dropped with
		// the host, unpublished = the caller leaves its switches alone.
		TEST_METHOD(IngressFixSwitchesArePublishedPerHostAndReadAsOneWord)
		{
			HubScope scope;
			using Vpv::Ingress::IngressState;
			struct Settings
			{
				bool pulldownFixes, timingContinuityFixes, cadenceClassFixes, queueTimingFixes, motionHistoryFixes, pipelineOriginalPathway;
			};
			const Settings original = { false, false, false, false, false, true };
			const Settings vpv = { true, true, true, true, true, false };
			const Settings mixed = { true, false, true, false, true, false };
			int host = 0, other = 0;
			uint32_t bits = 123;
			Assert::IsFalse(Hub().FixSwitchBits(&host, bits), L"nothing published yet");
			Assert::AreEqual<uint32_t>(123, bits, L"an unpublished read leaves the caller's word alone");
			Assert::IsTrue(Hub().NoteFixSwitches(&host, IngressState::FixSwitchBitsOf(IngressState::FixSwitchesOf(original))));
			Assert::IsTrue(Hub().FixSwitchBits(&host, bits));
			IngressState::FixSwitches f = IngressState::FixSwitchesFromBits(bits);
			Assert::IsTrue(f.originalPathway && !f.pulldownFixes && !f.timingContinuityFixes && !f.cadenceClassFixes &&
				!f.queueTimingFixes && !f.motionHistoryFixes, L"Original: all five off, pathway set");
			Assert::IsFalse(Hub().FixSwitchBits(&other, bits), L"another host has no word");
			Assert::IsTrue(Hub().NoteFixSwitches(&host, IngressState::FixSwitchBitsOf(IngressState::FixSwitchesOf(mixed))));
			Assert::IsTrue(Hub().FixSwitchBits(&host, bits));
			f = IngressState::FixSwitchesFromBits(bits);
			Assert::IsTrue(f.pulldownFixes && !f.timingContinuityFixes && f.cadenceClassFixes && !f.queueTimingFixes &&
				f.motionHistoryFixes && !f.originalPathway, L"each of the six bits is its own switch");
			Assert::IsFalse(Hub().OriginalPathway(&host), L"publishing the switches does not record the pathway fact");
			SettingsApplied(&host, true);
			Assert::IsTrue(Hub().OriginalPathway(&host));
			Assert::IsTrue(Hub().FixSwitchBits(&host, bits), L"the SettingsApplied fact keeps the published word");
			Assert::AreEqual<uint32_t>(IngressState::FixSwitchBitsOf(IngressState::FixSwitchesOf(mixed)), bits);
			Assert::IsTrue(Hub().NoteFixSwitches(&host, IngressState::FixSwitchBitsOf(IngressState::FixSwitchesOf(vpv))));
			Assert::IsTrue(Hub().FixSwitchBits(&host, bits));
			Assert::IsTrue(IngressState::FixSwitchBitsOf(IngressState::FixSwitchesOf(vpv)) == bits);
			Assert::IsFalse(Hub().NoteFixSwitches(nullptr, 1), L"no host, no word");
			RE::NotifyLifecycle(RE::Lifecycle::GpuReleased, &host);
			Assert::IsFalse(Hub().FixSwitchBits(&host, bits), L"dropped with the host");
		}

		// T-AR3-N4 (AUDIT-R 1fa78cdb9 N4): a SettingsApplied pathway fact the hub cannot record (slot table full) is counted
		// and a WorkerFault status line is emitted (the visible display is the host's "ingress switches" fallback), not ignored.
		TEST_METHOD(AnUnrecordablePathwayFactIsCountedAndReportedVisibly)
		{
			HubScope scope;
			struct Probe : Vpv::Hub::ISkeletonModule
			{
				std::vector<std::string> faults;
				const char* Name() const override { return "probe"; }
				void OnStatus(const RE::Context&, RE::StatusKind k, std::vector<std::string>&, const char* worker, const char* what) override
				{
					if (k == RE::StatusKind::WorkerFault)
						faults.push_back(std::string(worker ? worker : "") + "|" + (what ? what : ""));
				}
			};
			Probe* probe = new Probe();
			Hub().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(probe));
			std::vector<int> hosts(64);
			size_t filled = 0;
			while (filled < hosts.size() && Hub().NoteOriginalPathway(&hosts[filled], false))
				++filled;
			Assert::IsTrue(filled > 0 && filled < hosts.size(), L"the per-host table is bounded");
			Assert::AreEqual<uint32_t>(0, Hub().PathwayRefusals());
			SettingsApplied(&hosts[filled], true);
			Assert::AreEqual<uint32_t>(1, Hub().PathwayRefusals(), L"counted");
			Assert::AreEqual<size_t>(1, probe->faults.size(), L"and reported, once");
			Assert::IsTrue(probe->faults[0].find("hub|pathway table full") == 0, L"with the worker and the reason");
			SettingsApplied(&hosts[0], true);   // a recorded host: no refusal
			Assert::AreEqual<uint32_t>(1, Hub().PathwayRefusals());
			Assert::AreEqual<size_t>(1, probe->faults.size());
		}

		// T-AR3-N2 audit SHOULD 1: an unpublishable fix-switch word (table full) is a counted refusal the caller can surface.
		TEST_METHOD(AnUnpublishableFixSwitchWordIsRefusedAndCounted)
		{
			HubScope scope;
			std::vector<int> hosts(64);
			size_t filled = 0;
			while (filled < hosts.size() && Hub().NoteFixSwitches(&hosts[filled], 1))
				++filled;
			Assert::IsTrue(filled > 0 && filled < hosts.size(), L"the per-host table is bounded");
			Assert::AreEqual<uint32_t>(1, Hub().PathwayRefusals(), L"the refused publish is counted");
			uint32_t bits = 0;
			Assert::IsFalse(Hub().FixSwitchBits(&hosts[filled], bits), L"nothing published for the refused host");
			Assert::IsTrue(Hub().FixSwitchBits(&hosts[0], bits) && bits == 1, L"the recorded hosts keep their word");
			Assert::IsTrue(Hub().NoteFixSwitches(&hosts[0], 3), L"a recorded host still updates");
			Assert::AreEqual<uint32_t>(1, Hub().PathwayRefusals());
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

		// GEM-173: Bill has no source_geometry_detect, so the Original pathway forces it to the built-in default "all"
		// (the renderer's own loader, no device). The control arm proves the configured value reaches the field.
		TEST_METHOD(SourceGeometryDetectIsForcedToTheDefaultInTheOriginalPathway)
		{
			SettingsTestSupport::Renderer renderer;
			Assert::IsTrue(renderer.Ready(), L"the renderer DLL and its settings exports");
			const std::string profile = "[general]\npersist_profile_selection: false\n[vprenderer.Default]\nquality: high\n"
				"source_geometry_detect: zzz_unknown, 1920x1080\n";
			const SettingsTestSupport::Evaluation off = renderer.Evaluate(profile);
			Assert::IsTrue(off.ok, L"the control configuration loads");
			Assert::AreEqual(std::string("zzz_unknown, 1920x1080"),
				SettingsTestSupport::DescribeValue(off.describe, "source_geometry_detect"),
				L"control arm (pathway off): the configured value reaches the field");
			const SettingsTestSupport::Evaluation on = renderer.Evaluate(profile + "[vpv]\npipeline_original_pathway: on\n");
			Assert::IsTrue(on.ok, L"the Original configuration loads");
			Assert::AreEqual(std::string("all"),
				SettingsTestSupport::DescribeValue(on.describe, "source_geometry_detect"),
				L"Original pathway: forced to the built-in default after PostLoad");
		}

		// Entering the Original pathway releases the "source geometry detect" line a VP-V value engaged: the forced value
		// parses with no unknown id, and the monitor reports the change so the settings seam syncs the line away.
		TEST_METHOD(EnteringTheOriginalPathwayReleasesTheUnknownGeometryLine)
		{
			SettingsTestSupport::Renderer renderer;
			Assert::IsTrue(renderer.Ready(), L"the renderer DLL and its settings exports");
			const SettingsTestSupport::Evaluation on = renderer.Evaluate(
				"[general]\npersist_profile_selection: false\n[vprenderer.Default]\nquality: high\n"
				"source_geometry_detect: zzz_unknown\n[vpv]\npipeline_original_pathway: on\n");
			Assert::IsTrue(on.ok);
			const std::string originalValue = SettingsTestSupport::DescribeValue(on.describe, "source_geometry_detect");

			SourceResolutionMonitor::Monitor monitor;
			Assert::IsTrue(monitor.SetGeometryDetect("zzz_unknown"));
			const FallbackCatalog::Decision engaged = monitor.GeometryDetectFallback();
			Assert::IsTrue(engaged.engaged, L"control: an unknown id engages the line");
			Assert::AreEqual(std::string("source geometry detect"), engaged.id);
			Assert::IsTrue(engaged.reason.find("zzz_unknown") != std::string::npos);

			Assert::IsTrue(monitor.SetGeometryDetect(originalValue), L"the Original value is a change: the seam re-syncs the line");
			const FallbackCatalog::Decision released = monitor.GeometryDetectFallback();
			Assert::IsFalse(released.engaged, L"released in the Original pathway");
			Assert::IsTrue(released.reason.empty());
		}

		// W23-C2-S1-H2 / S1-H3 / S5-H1 / S5-M2 (bff541a87 + 24d188643):
		// (1) Legacy section precedence [vpv] -> [vprenderer] -> [vpvr.display] -> [display] agrees across
		//     OriginalPathwayForConfig, HdrInferenceChoice::FromConfig, and Skeleton::Parse (with explicit [vpv] off
		//     overriding legacy [vpvr.display] / [display] on).
		// (2) QueueDelayHold cleanup on live VP-V -> Original switch: passing budgetMs = 0 when original == true
		//     clears an active HoldDecision and clears a "too short" delay-budget fallback note, whereas the pre-fix
		//     early-return negative control left stale m_queueHold.active == true and stale "delay budget" engaged.
		// (3) PipelineSlots / FallbackStatus cleanup on live VP-V -> Original (and RepairDeband off): all gated VP-V
		//     fallback ids ("delay budget", "look-ahead", "dither need", "product.debander", "product.detail_pulse",
		//     "product.texture_restore", "queue analysis floor") are released cleanly.
		TEST_METHOD(LiveSwitchToOriginalClearsQueueDelayHoldGatedFallbacksAndHonoursLegacySectionPrecedence)
		{
			// 1. Legacy section precedence [vpv] -> [vprenderer] -> [vpvr.display] -> [display].
			char tempDir[MAX_PATH] = {}, tempFile[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(MAX_PATH, tempDir) != 0);
			Assert::IsTrue(GetTempFileNameA(tempDir, "opg", 0, tempFile) != 0);
			struct TempGuard { std::string p; ~TempGuard() { DeleteFileA(p.c_str()); } } tg{ tempFile };

			auto loadCfg = [&](const std::string& text, ConfigFile& out) {
				{
					std::ofstream f(tg.p, std::ios::trunc | std::ios::binary);
					f << text;
				}
				Assert::IsTrue(out.Load(tg.p));
			};

			for (const char* sec : { "vpv", "vprenderer", "vpvr.display", "display" })
			{
				ConfigFile cfg;
				loadCfg(std::string("[") + sec + "]\npipeline_original_pathway: on\n", cfg);
				const bool shortcutOrig = OriginalPathwayForConfig(
					[&cfg](const char* s, const char* k, std::string& v) { return cfg.TryGetString(s, k, v); });
				const HdrInferenceChoice::Resolved hdr = HdrInferenceChoice::FromConfig(cfg);
				const Vpv::Skeleton::Settings sk = Vpv::Skeleton::Parse(cfg);
				Assert::IsTrue(shortcutOrig && hdr.originalPathway && sk.originalPathway,
					L"S5-M2: all pathway readers honour [vpv]/[vprenderer]/[vpvr.display]/[display] = on");
				Assert::IsTrue(hdr.effective == HdrInferenceChoice::Mode::Original,
					L"S5-H1/S5-M2: Original pathway forces HdrInferenceChoice::Mode::Original");
			}
			// Precedence negative control: explicit [vpv] off overrides [vpvr.display] on and [display] on!
			{
				ConfigFile cfg;
				loadCfg("[vpv]\npipeline_original_pathway: off\n[vpvr.display]\npipeline_original_pathway: on\n[display]\npipeline_original_pathway: on\n", cfg);
				const bool shortcutOrig = OriginalPathwayForConfig(
					[&cfg](const char* s, const char* k, std::string& v) { return cfg.TryGetString(s, k, v); });
				const HdrInferenceChoice::Resolved hdr = HdrInferenceChoice::FromConfig(cfg);
				const Vpv::Skeleton::Settings sk = Vpv::Skeleton::Parse(cfg);
				Assert::IsFalse(shortcutOrig || hdr.originalPathway || sk.originalPathway,
					L"negative control: [vpv] off must override legacy [vpvr.display]/[display] on");
			}

			// 2. S1-H2 QueueDelayHold live VP-V -> Original cleanup vs pre-fix early-return negative control.
			{
				VpQueueTiming::VpFixedBudgetPolicy vpPolicy;
				QueueTimingPolicy::HoldContext vpHoldCtx{};
				vpHoldCtx.budgetMs = 267;
				vpHoldCtx.sourceRateHz = 23.976;
				vpHoldCtx.signalRateHz = 60.0;
				vpHoldCtx.displayHz = 60.0;
				vpHoldCtx.queueCapacity = 16;
				const QueueTimingPolicy::HoldPlan activeHold = vpPolicy.PlanHold(vpHoldCtx);
				Assert::IsTrue(activeHold.active && activeHold.targetFrames >= 4,
					L"precondition: VP-V 267 ms budget activates QueueDelayHold");

				// Too-short VP-V budget (80 ms at 23.976 fps) produces a non-empty "delay budget" fallback note.
				QueueTimingPolicy::HoldContext tightCtx = vpHoldCtx;
				tightCtx.budgetMs = 80;
				const QueueTimingPolicy::HoldPlan tightHold = vpPolicy.PlanHold(tightCtx);
				Assert::IsFalse(tightHold.note.empty(),
					L"precondition: 80 ms budget at 23.976 fps populates non-empty delay budget fallback note");

				// Pre-fix negative control: returning early on Original left activeHold.active == true and tightHold.note non-empty!
				Assert::IsTrue(activeHold.active && !tightHold.note.empty(),
					L"negative control: early return on Original leaked active QueueDelayHold and delay budget fallback");

				// S1-H2 fix: on Original (`context.budgetMs = (m_impl && !original) ? budgetMs : 0`), PlanHold deactivates hold & clears note!
				QueueTimingPolicy::HoldContext origHoldCtx = vpHoldCtx;
				origHoldCtx.budgetMs = 0;
				const QueueTimingPolicy::HoldPlan clearedHold = vpPolicy.PlanHold(origHoldCtx);
				Assert::IsFalse(clearedHold.active, L"S1-H2: Original pathway budgetMs=0 clears m_queueHold.active");
				Assert::IsTrue(clearedHold.note.empty(), L"S1-H2: Original pathway budgetMs=0 clears delay budget fallback note");
			}

			// 3. S1-H3 + 24d188643 LOW-2: PipelineSlots + FallbackStatus release of gated VP-V fallbacks on Original / slot-off.
			{
				FallbackStatus::Registry fallbacks;
				const char* const gatedIds[] = {
					"timeline", "calibration LUT BTB/WTW", "delay budget", "look-ahead",
					"motion look-ahead worker", "occlusion mask", "calibration LUT stage",
					"calibration LUT slope", "temporal entrance", "libplacebo", "picture precision",
					"product.debander", "product.detail_pulse", "product.texture_restore",
					"queue analysis floor", "dither need"
				};
				for (const char* id : gatedIds)
					fallbacks.Engage(id, "synthetic VP-V fallback before switch");
				Assert::IsTrue(fallbacks.Heading().find("Fallbacks engaged: none") == std::string::npos,
					L"negative control: VP-V fallbacks engaged before Original switch");

				// Disabling RepairDeband via pipeline_slots_off releases product.debander (24d188643 LOW-2).
				PipelineSlots::Registry slots;
				slots.SetEnabled(PipelineSlots::Id::RepairDeband, false);
				if (!slots.Runs(PipelineSlots::Id::RepairDeband))
					fallbacks.Release("product.debander");
				Assert::IsFalse(fallbacks.Engaged("product.debander"),
					L"24d188643 LOW-2: disabling repair.deband releases product.debander fallback");
				Assert::IsTrue(fallbacks.Engaged("delay budget"), L"other VP-V fallbacks remain while still on VP-V");

				// Entering Original releases all gated VP-V fallbacks cleanly (S1-H3).
				slots.ApplyOriginalPathway();
				Assert::IsTrue(slots.OriginalPathway());
				for (const char* id : gatedIds)
					fallbacks.Release(id);
				Assert::AreEqual(std::string("Fallbacks engaged: none"), fallbacks.Heading(),
					L"S1-H3: Original pathway releases all gated VP-V fallbacks cleanly");
			}
		}
	};
}
