// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5b-0 (doc 59 section 1.5): the per-vsync interpolation timeline as recorded traces. Each scenario
// (TimelinePlanScenarios.h) replays dequeued frames through the verbatim copy of today's RenderLoop
// timeline block (TimelinePlanLegacyCopy.h): the outputs and phases, frames no refresh shows, film
// updates, the queue steering's rate trim, the display-repeat either/or and its fallback line, and the
// engaged / released log lines. The timeline never engages in low latency or while libplacebo smooth
// motion is active. P5b-5 moves the block behind PlanOutputs; the traces must stay identical. Pure CPU.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/motion/consumers/DejudderExtension.h>
#include <VP-V/modules/motion/consumers/tests/TimelinePlanRunnerHost.h>
#include <VP-V/modules/motion/consumers/tests/TimelinePlanScenarios.h>

#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace TimelinePlanTraceTest
{
	struct Recorded
	{
		const char* name;
		const char* trace;
	};

#define TIMELINE_TRACE(name, ...) { name, __VA_ARGS__ },
	static const Recorded kRecorded[] = {
#include "TimelinePlanTraceRows.inl"
	};
#undef TIMELINE_TRACE

	static std::vector<std::string> Lines(const char* text)
	{
		std::vector<std::string> lines;
		std::istringstream in(text);
		std::string line;
		while (std::getline(in, line))
			lines.push_back(line);
		return lines;
	}

	static std::wstring Wide(const std::string& v) { return std::wstring(v.begin(), v.end()); }

	TEST_CLASS(TimelinePlanTraceTests)
	{
	public:
		TEST_METHOD(EveryScenarioIsRecorded)
		{
			const std::vector<TimelinePlanLegacy::Scenario> scenarios = TimelinePlanLegacy::Scenarios();
			const size_t recorded = sizeof(kRecorded) / sizeof(kRecorded[0]);
			Assert::AreEqual(scenarios.size(), recorded, L"one recorded trace per scenario");
			size_t frames = 0;
			for (size_t i = 0; i < recorded; ++i)
			{
				Assert::AreEqual(scenarios[i].name, std::string(kRecorded[i].name), L"scenario order");
				frames += scenarios[i].frames.size();
			}
			Assert::IsTrue(frames >= 200, L"at least 200 recorded frames");
		}

		TEST_METHOD(LegacyCopyReproducesEveryRecordedTrace)
		{
			const std::vector<TimelinePlanLegacy::Scenario> scenarios = TimelinePlanLegacy::Scenarios();
			for (size_t i = 0; i < scenarios.size() && i < sizeof(kRecorded) / sizeof(kRecorded[0]); ++i)
			{
				const std::vector<std::string> expected = Lines(kRecorded[i].trace);
				const std::vector<std::string> actual = TimelinePlanLegacy::Trace(scenarios[i]);
				const std::wstring n = Wide(scenarios[i].name);
				Assert::AreEqual(expected.size(), actual.size(), (n + L": frame count").c_str());
				for (size_t k = 0; k < expected.size(); ++k)
					Assert::AreEqual(expected[k], actual[k], (n + L": frame " + std::to_wstring(k)).c_str());
			}
		}

		// M2 P5b-5: the MOVED plan (TimelinePlanner.h through an R10 OutputPlan, VpTimelineLine) over the same
		// scenarios must give the recorded traces.
		TEST_METHOD(NewPlannerReproducesEveryRecordedTrace)
		{
			const std::vector<TimelinePlanLegacy::Scenario> scenarios = TimelinePlanLegacy::Scenarios();
			for (size_t i = 0; i < scenarios.size() && i < sizeof(kRecorded) / sizeof(kRecorded[0]); ++i)
			{
				const std::vector<std::string> expected = Lines(kRecorded[i].trace);
				const std::vector<std::string> actual = TimelinePlanRunner::Trace(scenarios[i]);
				const std::wstring n = L"planner " + Wide(scenarios[i].name);
				Assert::AreEqual(expected.size(), actual.size(), (n + L": frame count").c_str());
				for (size_t k = 0; k < expected.size(); ++k)
					Assert::AreEqual(expected[k], actual[k], (n + L": frame " + std::to_wstring(k)).c_str());
			}
		}

		// The owner rule the traces pin, stated directly: never engaged in low latency or while
		// libplacebo smooth motion owns the frame.
		TEST_METHOD(TimelineNeverEngagesInLowLatencyOrSmoothMotion)
		{
			for (const TimelinePlanLegacy::Scenario& sc : TimelinePlanLegacy::Scenarios())
			{
				TimelinePlanLegacy::State st;
				if (sc.setup)
					sc.setup(st);
				for (size_t i = 0; i < sc.frames.size(); ++i)
				{
					if (sc.beforeFrame)
						sc.beforeFrame(st, i);
					st.Run(sc.frames[i]);
					if (st.lowLatency || sc.frames[i].temporalActive)
						Assert::IsFalse(st.vpTimelineActive, Wide(sc.name + " frame " + std::to_string(i)).c_str());
				}
			}
		}
	};

	// M2 P5b-5 audit F1 / F3: the no-plan guard and the real hook routes (hook layer -> hub -> DejudderExtension ->
	// the renderer's binding).
	TEST_CLASS(DejudderExtensionHookTests)
	{
	public:
		// Not planned (no binding / hub, or the plan threw): no stale timeline flags; planned: untouched.
		TEST_METHOD(NoPlanLeavesNoStaleTimelineFlags)
		{
			VpMotion::DejudderPolicyState st;
			st.vpTimelineActive = true;
			st.vpSkipOutput = true;
			st.vpSkipIsUpdate = true;
			st.vpOutputPhase = 0.5;
			Assert::IsFalse(VpMotion::TimelineNotPlanned(st, true), L"planned: no fallback");
			Assert::IsTrue(st.vpTimelineActive && st.vpSkipOutput, L"planned: the plan's flags stand");
			Assert::IsTrue(VpMotion::TimelineNotPlanned(st, false), L"not planned: the visible fallback");
			Assert::IsFalse(st.vpTimelineActive || st.vpSkipOutput || st.vpSkipIsUpdate);
			Assert::IsTrue(st.vpOutputPhase == 0.0);
		}

		// The BacklogFlush event and R10 PlanOutputs reach the registered renderer's binding through the hub; the
		// queue generation's Flush does not touch the steering; without the hub nothing is planned (the guard's case).
		TEST_METHOD(BacklogFlushAndPlanOutputsReachTheBinding)
		{
			struct FakeImpl
			{
				int flushes = 0, plans = 0;
				int64_t plannedSeq = -1;
			} impl;
			struct Fwd : RendererExtension::IRendererExtension
			{
				void OnQueue(const RendererExtension::QueueFacts& f) override { Vpv::Hub::ExtensionHub::Instance().OnQueue(f); }
				bool OnFrame(RendererExtension::FrameContext& c) override { return Vpv::Hub::ExtensionHub::Instance().OnFrame(c); }
			};
			struct Scope
			{
				Scope() { RendererExtension::ResetForTesting(); Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); }
				~Scope() { RendererExtension::ResetForTesting(); Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); }
			} scope;
			Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new VpMotion::DejudderExtension()));
			RendererExtension::Register(std::unique_ptr<RendererExtension::IRendererExtension>(new Fwd()));
			VpMotion::DejudderBindingSpec spec;
			spec.self = &impl;
			spec.queueFlush = [](void* self) { ++static_cast<FakeImpl*>(self)->flushes; };
			spec.planOutputs = [](void* self, RendererExtension::OutputPlan& plan)
			{
				FakeImpl* i = static_cast<FakeImpl*>(self);
				++i->plans;
				i->plannedSeq = plan.seq;
				plan.skip = true;
				return true;
			};
			{
				VpMotion::DejudderHostBinding binding(&impl, spec);
				Assert::IsTrue(binding.Registered());
				RendererExtension::NotifyQueue(RendererExtension::QueueEvent::BacklogFlush, &impl);
				Assert::AreEqual(1, impl.flushes, L"the backlog flush reaches the binding");
				RendererExtension::NotifyQueue(RendererExtension::QueueEvent::Flush, &impl, -1, 2, 64, 0, "live queue reset");
				Assert::AreEqual(1, impl.flushes, L"the queue generation's Flush is not the steering's event");
				int other = 0;
				RendererExtension::NotifyQueue(RendererExtension::QueueEvent::BacklogFlush, &other);
				Assert::AreEqual(1, impl.flushes, L"another renderer's flush is not ours");
				RendererExtension::OutputPlan plan;
				plan.seq = 7;
				Assert::IsTrue(RendererExtension::PlanOutputs(&impl, plan), L"planned through the hub");
				Assert::AreEqual(1, impl.plans);
				Assert::IsTrue(impl.plannedSeq == 7 && plan.skip);
				Assert::IsFalse(RendererExtension::PlanOutputs(&other, plan), L"no binding for that host: not planned");
			}
			RendererExtension::OutputPlan plan;
			Assert::IsFalse(RendererExtension::PlanOutputs(&impl, plan), L"binding gone: not planned");
			RendererExtension::ResetForTesting();
			Assert::IsFalse(RendererExtension::PlanOutputs(&impl, plan), L"no hub: not planned (the guard engages the fallback)");
		}
	};
}
