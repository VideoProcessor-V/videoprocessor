// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// The M2 P0b hook amendment (doc 59 section 1.1, R10-R20): declarations, hub dispatch and inert defaults.
// Per hook: with nothing registered (and with the hub registered but no module serving the phase) the helper
// returns the host's own value / "not handled" and nothing is called; with modules the dispatch reaches the typed
// method with the right payload and host, take-over phases are first-wins, fan-out phases call every module in order.

#include <vprenderer/RendererExtension.h>
#include <vprenderer/AlphaPresentationTelemetry.h>
#include "../../core/hub/ExtensionHub.h"

#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace HookAmendmentTest
{
	namespace
	{
		namespace RE = RendererExtension;
		namespace Hub = Vpv::Hub;

		std::vector<std::string>* g_log = nullptr;

		struct Scope
		{
			std::vector<std::string> log;
			Scope()
			{
				g_log = &log;
				RE::ResetForTesting();
				Hub::ExtensionHub::Instance().ResetModulesForTesting();
			}
			~Scope()
			{
				RE::ResetForTesting();
				Hub::ExtensionHub::Instance().ResetModulesForTesting();
				g_log = nullptr;
			}
		};

		// The registered form: one extension that forwards to the hub, like the production forwarder.
		struct Fwd : RE::IRendererExtension
		{
			Hub::ExtensionHub& h = Hub::ExtensionHub::Instance();
			bool OnFrame(RE::FrameContext& c) override { return h.OnFrame(c); }
			void* Query(const char* i) override { return h.Query(i); }
		};

		void RegisterHubForwarder() { RE::Register(std::unique_ptr<RE::IRendererExtension>(new Fwd())); }

		// A module that records each typed call and does whatever its switches say.
		struct Mod : Hub::ISkeletonModule
		{
			std::string n;
			std::set<std::string> takes;   // take-over hooks it returns true for
			bool throws = false;
			const void* seenHost = nullptr;
			int64_t seenSeq = -2, seenGeneration = -2;
			explicit Mod(const char* name) : n(name) {}
			const char* Name() const override { return n.c_str(); }

			bool Take(RE::FrameContext& c, const char* what)
			{
				g_log->push_back(n + ":" + what);
				seenHost = c.host; seenSeq = c.seq; seenGeneration = c.generation;
				if (throws) throw 1;
				return takes.count(what) != 0;
			}
			void Note(RE::FrameContext& c, const char* what) { Take(c, what); }

			bool OnPlanOutputs(RE::FrameContext& c, RE::OutputPlan& p) override
			{
				const bool h = Take(c, "plan_outputs");
				if (h && p.outputs) p.outputs->push_back(1000.0 + p.ptsA);
				if (h) { p.skip = true; p.schedulesRefreshes = true; }
				return h;
			}
			bool OnPlanPresent(RE::FrameContext& c, RE::PresentPlan& p) override
			{
				const bool h = Take(c, "plan_present");
				if (h) p.interval = 2;
				return h;
			}
			void OnAfterPresent(RE::FrameContext& c, RE::PresentContext& p) override
			{
				Note(c, "after_present");
				p.outcome.extrasRan = true;
				if (p.renderMs + 1.0 > p.outcome.targetLeadMs) p.outcome.targetLeadMs = p.renderMs + 1.0;
				p.outcome.spentSeconds += 0.5;
			}
			void OnPresentSample(RE::FrameContext& c, RE::PresentSampleView& v) override
			{
				Note(c, "present_sample");
				v.sample->presentCount += 1;
			}
			void OnResolveTime(RE::FrameContext& c, RE::TimeQuery& q) override
			{
				Note(c, "resolve_time");
				q.qpc = q.qpc * 2 + 1;
			}
			bool OnDecideIngress(RE::FrameContext& c, RE::IngressContext& ic) override
			{
				const bool h = Take(c, "decide_ingress");
				if (h) { ic.drop = true; ic.arrivalQpc += 5; ic.gapBefore = true; ic.gapFrames += 1; }
				return h;
			}
			bool OnConvertedFrame(RE::FrameContext& c, RE::ConvertContext& cc) override
			{
				const bool h = Take(c, "convert");
				if (cc.lockFormatter) cc.lockFormatter(cc.formatterUser);
				if (h && cc.extension) *cc.extension = std::make_shared<int>(42);
				if (cc.unlockFormatter) cc.unlockFormatter(cc.formatterUser);
				return h;
			}
			void OnAfterPublish(RE::FrameContext& c, RE::PublishView& v) override
			{
				Note(c, "after_publish");
				if (v.extension) g_log->push_back(n + ":ext=" + std::to_string(*std::static_pointer_cast<int>(v.extension)));
			}
			bool OnTryUpload(RE::FrameContext& c, RE::UploadContext& u) override
			{
				const bool h = Take(c, "upload");
				if (h && u.setIngressStatus) u.setIngressStatus(u.statusUser, "uploaded");
				return h;
			}
			void OnQueueTarget(RE::FrameContext& c, RE::QueueTargetContext& q) override
			{
				Note(c, "queue_target");
				q.target += 1;
				if (q.target > q.desired) q.target = q.desired;   // the owner rule a real handler keeps
			}
			void* Query(const char* iid) override { return std::strcmp(iid, RE::kTimedFileFramesIid) == 0 ? this : nullptr; }
		};

		void AddMod(Mod* m) { Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(m)); }

		struct StatusSink { std::string text; int locks = 0, unlocks = 0; };
		void SetStatus(void* user, const char* text) { static_cast<StatusSink*>(user)->text = text; }
		void LockFormatter(void* user) { ++static_cast<StatusSink*>(user)->locks; }
		void UnlockFormatter(void* user) { ++static_cast<StatusSink*>(user)->unlocks; }

		// Every helper's no-handler answer: the host's own value, nothing handled, nothing written.
		void CheckBillDefaults(const void* host)
		{
			std::vector<double> outs = { 1.0, 2.0 }, phases = { 0.25 };
			RE::OutputPlan plan;
			plan.seq = 5; plan.generation = 3; plan.ptsA = 10.0; plan.outputs = &outs; plan.phases = &phases;
			Assert::IsFalse(RE::PlanOutputs(host, plan));
			Assert::AreEqual<size_t>(2, outs.size());
			Assert::AreEqual<size_t>(1, phases.size());
			Assert::IsFalse(plan.skip);
			Assert::IsFalse(plan.schedulesRefreshes);

			RE::PresentPlan pp;
			Assert::IsFalse(RE::PlanPresent(host, pp));
			Assert::AreEqual<uint32_t>(1, pp.interval);   // the host's Present(1, 0)
			Assert::AreEqual<uint32_t>(0, pp.flags);

			RE::PresentContext pc;
			const RE::PresentOutcome o = RE::AfterPresent(host, 7, 1, pc);
			Assert::IsFalse(o.extrasRan);
			Assert::IsFalse(o.committed);
			Assert::AreEqual(0.0, o.targetLeadMs);
			Assert::AreEqual(0.0, o.estimatedLeadMs);
			Assert::AreEqual(0.0, o.spentSeconds);

			AlphaDxgiPresentationSample sample;
			sample.presentCount = 5; sample.syncQpc = 99;
			RE::AdjustPresentationSample(host, sample, nullptr);
			Assert::AreEqual<uint32_t>(5, sample.presentCount);
			Assert::AreEqual<int64_t>(99, sample.syncQpc);

			Assert::AreEqual<int64_t>(123456789012345LL, RE::ResolveTime(host, RE::TimeKind::FramePresentEnd, 123456789012345LL));

			RE::IngressContext ic;
			ic.host = host; ic.arrivalQpc = 77; ic.gapFrames = 2; ic.gapBefore = true; ic.filmHz = 23.976;
			Assert::IsFalse(RE::DecideIngress(ic));
			Assert::IsFalse(ic.drop);
			Assert::AreEqual<int64_t>(77, ic.arrivalQpc);
			Assert::AreEqual<uint32_t>(2, ic.gapFrames);
			Assert::IsTrue(ic.gapBefore);
			Assert::AreEqual(23.976, ic.filmHz);

			std::shared_ptr<void> slot;
			RE::ConvertContext cc;
			cc.extension = &slot;
			Assert::IsFalse(RE::OnConvertedFrame(host, cc));
			Assert::IsTrue(slot == nullptr);
			RE::PublishView pv;
			RE::AfterPublish(host, pv);

			RE::UploadContext uc;
			Assert::IsFalse(RE::TryUpload(host, uc));

			RE::QueueTargetContext qc;
			qc.desired = 4; qc.capacity = 8;
			Assert::AreEqual<size_t>(3, RE::ResolveQueueTarget(host, qc, 3));
			Assert::AreEqual<size_t>(3, qc.target);
			Assert::AreEqual<size_t>(3, qc.billTarget);
		}
	}

	TEST_CLASS(HookAmendmentTests)
	{
	public:
		TEST_METHOD(NothingRegisteredEveryHelperReturnsTheHostValue)
		{
			Scope s;
			Assert::IsFalse(RE::Any());
			int host = 0;
			CheckBillDefaults(&host);
			Assert::IsTrue(s.log.empty());
		}

		TEST_METHOD(HubRegisteredWithNoModulesReturnsTheHostValue)
		{
			Scope s;
			RegisterHubForwarder();
			Assert::AreEqual<size_t>(0, Hub::ExtensionHub::Instance().ModuleCount());
			int host = 0;
			CheckBillDefaults(&host);
			Assert::IsTrue(s.log.empty());
		}

		TEST_METHOD(AModuleThatServesNoPhaseChangesNothing)
		{
			Scope s;
			struct Bare : Hub::ISkeletonModule
			{
				const char* Name() const override { return "bare"; }
			};
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new Bare()));
			RegisterHubForwarder();
			int host = 0;
			CheckBillDefaults(&host);
			Assert::IsTrue(s.log.empty());
		}

		TEST_METHOD(PhaseValuesAndRules)
		{
			// New phases are appended: the numbering the earlier packages see does not move. All 16 are pinned.
			Assert::AreEqual(0, static_cast<int>(RE::FramePhase::Convert));
			Assert::AreEqual(1, static_cast<int>(RE::FramePhase::AfterPublish));
			Assert::AreEqual(2, static_cast<int>(RE::FramePhase::CollectAhead));
			Assert::AreEqual(3, static_cast<int>(RE::FramePhase::QueueTarget));
			Assert::AreEqual(4, static_cast<int>(RE::FramePhase::Begin));
			Assert::AreEqual(5, static_cast<int>(RE::FramePhase::PlanOutputs));
			Assert::AreEqual(6, static_cast<int>(RE::FramePhase::Entrance));
			Assert::AreEqual(7, static_cast<int>(RE::FramePhase::Upload));
			Assert::AreEqual(8, static_cast<int>(RE::FramePhase::CollectHooks));
			Assert::AreEqual(9, static_cast<int>(RE::FramePhase::Draw));
			Assert::AreEqual(10, static_cast<int>(RE::FramePhase::AfterRender));
			Assert::AreEqual(11, static_cast<int>(RE::FramePhase::PlanPresent));
			Assert::AreEqual(12, static_cast<int>(RE::FramePhase::AfterPresent));
			Assert::AreEqual(13, static_cast<int>(RE::FramePhase::PresentSample));
			Assert::AreEqual(14, static_cast<int>(RE::FramePhase::ResolveTime));
			Assert::AreEqual(15, static_cast<int>(RE::FramePhase::DecideIngress));
			// First-wins stays for Begin and the take-over phases; the five P0b fan-out phases fan out.
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::Begin));
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::Draw));
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::Convert));
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::Upload));
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::PlanOutputs));
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::PlanPresent));
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::DecideIngress));
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::AfterPublish));
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::QueueTarget));
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::AfterPresent));
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::PresentSample));
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::ResolveTime));
			// M2 P4 (70b section 9.7): the observer phases Entrance, CollectHooks and AfterRender fan out as well
			// (added assertions; none of the above changed). See EntranceHookTests.
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::Entrance));
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::CollectHooks));
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::AfterRender));
		}

		TEST_METHOD(TakeOverPhasesAreFirstWinsAndTheHostValueSurvivesAMiss)
		{
			Scope s;
			Mod* a = new Mod("a"); Mod* b = new Mod("b");
			AddMod(a); AddMod(b);
			RegisterHubForwarder();
			int host = 0;
			const char* hooks[] = { "plan_outputs", "plan_present", "decide_ingress", "convert", "upload" };

			for (const char* hook : hooks)
			{
				// Nobody takes the phase over: both are asked, in order, and the host value stands.
				s.log.clear();
				a->takes.clear(); b->takes.clear();
				std::vector<double> outs;
				RE::OutputPlan plan; plan.outputs = &outs;
				RE::PresentPlan pp;
				RE::IngressContext ic; ic.host = &host;
				RE::ConvertContext cc;
				RE::UploadContext uc;
				auto run = [&]() -> bool
				{
					if (!std::strcmp(hook, "plan_outputs")) return RE::PlanOutputs(&host, plan);
					if (!std::strcmp(hook, "plan_present")) return RE::PlanPresent(&host, pp);
					if (!std::strcmp(hook, "decide_ingress")) return RE::DecideIngress(ic);
					if (!std::strcmp(hook, "convert")) return RE::OnConvertedFrame(&host, cc);
					return RE::TryUpload(&host, uc);
				};
				Assert::IsFalse(run());
				Assert::IsTrue((std::vector<std::string>{ std::string("a:") + hook, std::string("b:") + hook }) == s.log);

				// The first takes it over: the second is not called.
				s.log.clear();
				a->takes.insert(hook);
				Assert::IsTrue(run());
				Assert::IsTrue((std::vector<std::string>{ std::string("a:") + hook }) == s.log);

				// Only the second takes it over: the first was asked first, the second decides.
				s.log.clear();
				a->takes.clear(); b->takes.insert(hook);
				Assert::IsTrue(run());
				Assert::IsTrue((std::vector<std::string>{ std::string("a:") + hook, std::string("b:") + hook }) == s.log);
			}
		}

		TEST_METHOD(PlanOutputsPayloadAndHostReachTheModule)
		{
			Scope s;
			Mod* a = new Mod("a"); a->takes.insert("plan_outputs");
			AddMod(a);
			RegisterHubForwarder();
			int host = 0;
			std::vector<double> outs = { 1.0 }, phases;
			RE::OutputPlan plan;
			plan.seq = 11; plan.nextSeq = 12; plan.generation = 4; plan.ptsA = 2.5; plan.outputs = &outs; plan.phases = &phases;
			Assert::IsTrue(RE::PlanOutputs(&host, plan));
			Assert::IsTrue(a->seenHost == &host);
			Assert::AreEqual<int64_t>(11, a->seenSeq);
			Assert::AreEqual<int64_t>(4, a->seenGeneration);
			Assert::AreEqual<size_t>(2, outs.size());
			Assert::AreEqual(1002.5, outs[1]);
			Assert::IsTrue(plan.skip);
			Assert::IsTrue(plan.schedulesRefreshes);
		}

		TEST_METHOD(PlanPresentDefaultsToIntervalOneAndAHandlerDecides)
		{
			Scope s;
			Mod* a = new Mod("a");
			AddMod(a);
			RegisterHubForwarder();
			int host = 0;
			RE::PresentPlan pp;
			pp.kind = RE::PresentKind::Repeat; pp.vpOwned = true;
			Assert::IsFalse(RE::PlanPresent(&host, pp));
			Assert::AreEqual<uint32_t>(1, pp.interval);
			a->takes.insert("plan_present");
			Assert::IsTrue(RE::PlanPresent(&host, pp));
			Assert::AreEqual<uint32_t>(2, pp.interval);
			Assert::IsTrue(a->seenHost == &host);
		}

		TEST_METHOD(AfterPresentFansOutAndTheOutcomeOnlyGrows)
		{
			Scope s;
			AddMod(new Mod("a")); AddMod(new Mod("b"));
			RegisterHubForwarder();
			int host = 0;
			RE::PresentContext pc;
			pc.renderMs = 2.0;
			const RE::PresentOutcome o = RE::AfterPresent(&host, 9, 2, pc);
			Assert::IsTrue((std::vector<std::string>{ "a:after_present", "b:after_present" }) == s.log);
			Assert::IsTrue(o.extrasRan);
			Assert::AreEqual(3.0, o.targetLeadMs);      // max, not a sum
			Assert::AreEqual(1.0, o.spentSeconds);      // sum
			Assert::IsFalse(o.committed);
		}

		TEST_METHOD(PresentSampleAndResolveTimeChainInOrder)
		{
			Scope s;
			AddMod(new Mod("a")); AddMod(new Mod("b"));
			RegisterHubForwarder();
			int host = 0;
			AlphaDxgiPresentationSample sample;
			sample.presentCount = 10;
			RE::AdjustPresentationSample(&host, sample, nullptr);
			Assert::AreEqual<uint32_t>(12, sample.presentCount);
			// (100 * 2 + 1) * 2 + 1: the second module sees the first one's result; the host value is the input.
			Assert::AreEqual<int64_t>(403, RE::ResolveTime(&host, RE::TimeKind::FramePresentEnd, 100));
			Assert::IsTrue((std::vector<std::string>{ "a:present_sample", "b:present_sample", "a:resolve_time", "b:resolve_time" }) == s.log);
		}

		TEST_METHOD(QueueTargetChainsAndAHandlerKeepingTheDesiredCapNeverExceedsIt)
		{
			Scope s;
			AddMod(new Mod("a")); AddMod(new Mod("b"));
			RegisterHubForwarder();
			int host = 0;
			RE::QueueTargetContext qc;
			qc.desired = 4; qc.capacity = 8; qc.holdActive = true; qc.holdFraction = 0.5; qc.creditFrames = 1; qc.lookaheadFloor = 2;
			Assert::AreEqual<size_t>(3, RE::ResolveQueueTarget(&host, qc, 1));   // 1 -> 2 -> 3
			Assert::AreEqual<size_t>(4, RE::ResolveQueueTarget(&host, qc, 3));   // 3 -> 4 -> capped at desired 4
			Assert::AreEqual<size_t>(3, qc.billTarget);
			Assert::AreEqual<size_t>(4, RE::ResolveQueueTarget(&host, qc, 4));   // already at desired: stays
		}

		TEST_METHOD(QueueTargetResultIsClampedToCapacity)
		{
			Scope s;
			AddMod(new Mod("a")); AddMod(new Mod("b"));
			RegisterHubForwarder();
			int host = 0;
			RE::QueueTargetContext qc;
			qc.desired = 100; qc.capacity = 2;   // the modules add 2 (1 -> 3); the helper holds the target at capacity
			Assert::AreEqual<size_t>(2, RE::ResolveQueueTarget(&host, qc, 1));
			Assert::AreEqual<size_t>(2, qc.target);
		}

		TEST_METHOD(AfterPresentOutcomeIsMergedPerModuleAndAThrowDiscardsItsPartialWrites)
		{
			Scope s;
			struct Partial : Hub::ISkeletonModule
			{
				const char* Name() const override { return "partial"; }
				void OnAfterPresent(RE::FrameContext&, RE::PresentContext& p) override { p.outcome.committed = true; p.outcome.spentSeconds = 9.0; throw 1; }
			};
			struct Clear : Hub::ISkeletonModule
			{
				const char* Name() const override { return "clear"; }
				// A module that tries to wipe the outcome only wipes its own (empty) contribution.
				void OnAfterPresent(RE::FrameContext&, RE::PresentContext& p) override { p.outcome = RE::PresentOutcome(); }
			};
			AddMod(new Mod("a"));
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new Partial()));
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new Clear()));
			AddMod(new Mod("b"));
			RegisterHubForwarder();
			int host = 0;
			RE::PresentContext pc;
			pc.renderMs = 2.0;
			const RE::PresentOutcome o = RE::AfterPresent(&host, 1, 1, pc);
			Assert::IsTrue(o.extrasRan);
			Assert::IsFalse(o.committed);               // the throwing module's write is gone
			Assert::AreEqual(1.0, o.spentSeconds);      // a + b, not 9
			Assert::AreEqual(3.0, o.targetLeadMs);
		}

		// Phase 3 (refs/108; audit SHOULD): the R12 present-lead raise. Fakes named like the production modules, added
		// in HubBootstrap's order (PresenterExtension before SkeletonModule): the Presenter's step runs first, a later
		// non-raising module never clears the raise (OR), and with no raise the host keeps its own (even negative) lead.
		struct LeadMod : Hub::ISkeletonModule
		{
			std::string n;
			bool raise = false;
			double lead = 0, spent = 0;
			LeadMod(const char* name, bool r, double l, double s) : n(name), raise(r), lead(l), spent(s) {}
			const char* Name() const override { return n.c_str(); }
			void OnAfterPresent(RE::FrameContext&, RE::PresentContext& p) override
			{
				g_log->push_back(n + ":after_present");
				p.outcome.extrasRan = true;
				p.outcome.raiseTargetLead = raise;
				p.outcome.targetLeadMs = lead;
				p.outcome.spentSeconds += spent;
			}
		};
		static void AddLead(LeadMod* m) { Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(m)); }

		// The host's application of the merged outcome (LVR R12 block): only a raise replaces the host's lead.
		static double HostAppliesLead(double hostLeadMs, const RE::PresentOutcome& o) { return o.raiseTargetLead ? o.targetLeadMs : hostLeadMs; }

		TEST_METHOD(AfterPresentRaiseIsOredAcrossModulesAndThePresenterRunsBeforeTheSkeleton)
		{
			Scope s;
			AddLead(new LeadMod("presenter", true, 7.5, 0.25));
			AddLead(new LeadMod("skeleton", false, 1.0, 0.5));
			RegisterHubForwarder();
			int host = 0;
			RE::PresentContext pc;
			const RE::PresentOutcome o = RE::AfterPresent(&host, 3, 1, pc);
			Assert::IsTrue((std::vector<std::string>{ "presenter:after_present", "skeleton:after_present" }) == s.log);
			Assert::IsTrue(o.raiseTargetLead);          // the later non-raising skeleton does not clear it
			Assert::AreEqual(7.5, o.targetLeadMs);      // max: the skeleton's smaller lead does not lower it
			Assert::AreEqual(0.75, o.spentSeconds);
			Assert::IsTrue(o.extrasRan);
			Assert::AreEqual(7.5, HostAppliesLead(2.0, o));
		}

		TEST_METHOD(AfterPresentRaiseFromALaterModuleIsAlsoKept)
		{
			Scope s;
			AddLead(new LeadMod("presenter", false, 0.0, 0.0));
			AddLead(new LeadMod("late", true, 4.0, 0.0));
			RegisterHubForwarder();
			int host = 0;
			RE::PresentContext pc;
			const RE::PresentOutcome o = RE::AfterPresent(&host, 4, 1, pc);
			Assert::IsTrue(o.raiseTargetLead);
			Assert::AreEqual(4.0, o.targetLeadMs);
		}

		TEST_METHOD(AfterPresentWithoutARaiseLeavesANegativeHostLeadAlone)
		{
			Scope s;
			// Target unknown: the Presenter does not raise and leaves the lead at 0; the skeleton does not raise either.
			AddLead(new LeadMod("presenter", false, 0.0, 0.0));
			AddLead(new LeadMod("skeleton", false, 0.0, 0.5));
			RegisterHubForwarder();
			int host = 0;
			RE::PresentContext pc;
			const RE::PresentOutcome o = RE::AfterPresent(&host, 5, 1, pc);
			Assert::IsTrue(o.extrasRan);
			Assert::IsFalse(o.raiseTargetLead);
			Assert::AreEqual(0.0, o.targetLeadMs);
			// The host's negative lead (target missed) is NOT raised to the merged 0.
			Assert::AreEqual(-3.0, HostAppliesLead(-3.0, o));
		}

		TEST_METHOD(AThrowingModuleInAChainedPhaseIsContainedAndTheChainContinues)
		{
			Scope s;
			Mod* a = new Mod("a"); a->throws = true;
			AddMod(a); AddMod(new Mod("b"));
			RegisterHubForwarder();
			int host = 0;
			Assert::AreEqual<int64_t>(201, RE::ResolveTime(&host, RE::TimeKind::FramePresentEnd, 100));   // a threw before writing; b: 100*2+1
			RE::QueueTargetContext qc;
			qc.desired = 5; qc.capacity = 8;
			Assert::AreEqual<size_t>(2, RE::ResolveQueueTarget(&host, qc, 1));                             // only b's +1
			Assert::IsTrue((std::vector<std::string>{ "a:resolve_time", "b:resolve_time", "a:queue_target", "b:queue_target" }) == s.log);
		}

		TEST_METHOD(DecideIngressOutFieldsAndCaptureThreadCallsReachTheModule)
		{
			Scope s;
			Mod* a = new Mod("a"); a->takes.insert("decide_ingress");
			AddMod(a);
			RegisterHubForwarder();
			int host = 0;
			RE::IngressContext ic;
			ic.host = &host; ic.seq = 21; ic.queueGeneration = 6; ic.arrivalQpc = 100; ic.gapFrames = 1;
			Assert::IsTrue(RE::DecideIngress(ic));
			Assert::IsTrue(ic.drop);
			Assert::AreEqual<int64_t>(105, ic.arrivalQpc);
			Assert::IsTrue(ic.gapBefore);
			Assert::AreEqual<uint32_t>(2, ic.gapFrames);
			Assert::IsTrue(a->seenHost == &host);
			Assert::AreEqual<int64_t>(21, a->seenSeq);
			Assert::AreEqual<int64_t>(6, a->seenGeneration);
		}

		TEST_METHOD(ConvertFillsThePayloadExtensionSlotAndAfterPublishSeesIt)
		{
			Scope s;
			Mod* a = new Mod("a"); a->takes.insert("convert");
			AddMod(a);
			RegisterHubForwarder();
			int host = 0;
			StatusSink sink;
			std::shared_ptr<void> slot;
			RE::ConvertContext cc;
			cc.extension = &slot; cc.formatterUser = &sink; cc.lockFormatter = &LockFormatter; cc.unlockFormatter = &UnlockFormatter;
			Assert::IsTrue(RE::OnConvertedFrame(&host, cc));
			Assert::IsTrue(slot != nullptr);
			Assert::AreEqual(1, sink.locks);
			Assert::AreEqual(1, sink.unlocks);
			s.log.clear();
			RE::PublishView pv;
			pv.extension = slot;
			RE::AfterPublish(&host, pv);
			Assert::IsTrue((std::vector<std::string>{ "a:after_publish", "a:ext=42" }) == s.log);
		}

		TEST_METHOD(TryUploadPassesTheContextAndReportsHandled)
		{
			Scope s;
			Mod* a = new Mod("a"); a->takes.insert("upload");
			AddMod(a);
			RegisterHubForwarder();
			int host = 0;
			StatusSink sink;
			RE::UploadContext uc;
			uc.statusUser = &sink; uc.setIngressStatus = &SetStatus; uc.width = 1920; uc.height = 1080;
			Assert::IsTrue(RE::TryUpload(&host, uc));
			Assert::AreEqual(std::string("uploaded"), sink.text);
			Assert::IsTrue(a->seenHost == &host);
		}

		TEST_METHOD(ANullPayloadIsSkippedByTheHub)
		{
			Scope s;
			AddMod(new Mod("a"));
			RegisterHubForwarder();
			int host = 0;
			const RE::FramePhase phases[] = { RE::FramePhase::PlanOutputs, RE::FramePhase::PlanPresent, RE::FramePhase::AfterPresent,
				RE::FramePhase::PresentSample, RE::FramePhase::ResolveTime, RE::FramePhase::DecideIngress, RE::FramePhase::Convert,
				RE::FramePhase::AfterPublish, RE::FramePhase::Upload, RE::FramePhase::QueueTarget };
			for (RE::FramePhase phase : phases)
			{
				RE::FrameContext c;
				c.host = &host; c.phase = phase; c.phaseData = nullptr;
				Assert::IsFalse(RE::Frame(c));
			}
			Assert::IsTrue(s.log.empty());
		}

		TEST_METHOD(AThrowingModuleIsContainedAndLaterModulesStillRun)
		{
			Scope s;
			Mod* a = new Mod("a"); a->throws = true;
			Mod* b = new Mod("b"); b->takes.insert("plan_outputs");
			AddMod(a); AddMod(b);
			RegisterHubForwarder();
			int host = 0;
			std::vector<double> outs;
			RE::OutputPlan plan; plan.outputs = &outs;
			Assert::IsTrue(RE::PlanOutputs(&host, plan));   // a threw: contained, b still decides
			RE::PresentContext pc;
			const RE::PresentOutcome o = RE::AfterPresent(&host, 1, 1, pc);
			Assert::IsTrue(o.extrasRan);                    // a threw before writing, b added its part
			Assert::AreEqual(0.5, o.spentSeconds);
			Assert::IsTrue((std::vector<std::string>{ "a:plan_outputs", "b:plan_outputs", "a:after_present", "b:after_present" }) == s.log);
		}

		TEST_METHOD(TheRegistryItselfHonoursFanOutAndFirstWins)
		{
			Scope s;
			struct Rec : RE::IRendererExtension
			{
				std::string n; bool handle;
				Rec(const char* name, bool h) : n(name), handle(h) {}
				bool OnFrame(RE::FrameContext& f) override { g_log->push_back(n + ":" + std::to_string(static_cast<int>(f.phase))); return handle; }
			};
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Rec("a", true)));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Rec("b", false)));
			int host = 0;
			int payload = 0;
			RE::FrameContext c;
			c.host = &host; c.phaseData = &payload;
			c.phase = RE::FramePhase::AfterPresent;   // fan-out: b is called although a handled it
			Assert::IsTrue(RE::Frame(c));
			c.phase = RE::FramePhase::PlanOutputs;    // take-over: a handles, b is not called
			Assert::IsTrue(RE::Frame(c));
			c.phase = RE::FramePhase::Begin;          // an earlier phase keeps first-wins
			Assert::IsTrue(RE::Frame(c));
			const std::vector<std::string> expected = { "a:12", "b:12", "a:5", "a:4" };
			Assert::IsTrue(expected == s.log);
		}

		TEST_METHOD(TimedFileFrameDeclarations)
		{
			Scope s;
			Assert::AreEqual(std::string("vpv.timed_file_frames.v1"), std::string(RE::kTimedFileFramesIid));
			RE::ExternalTiming t;
			Assert::AreEqual<int64_t>(0, t.presentationQpc);
			Assert::IsFalse(t.paused);
			Assert::IsFalse(t.motionOff);
			Assert::IsFalse(t.discontinuity);
			Assert::AreEqual(0.0, t.filmHz);
			struct Fake : RE::IQueueAdmission
			{
				void Enqueue(VideoFrame&, const void*, uint64_t, int64_t, int64_t, bool, const char*, uint32_t, double, bool, const RE::ExternalTiming*) override {}
				uint64_t QueueGeneration() const override { return 7; }
				bool SlotFree() const override { return true; }
				uint64_t Dropped() const override { return 3; }
			} fake;
			const RE::IQueueAdmission& view = fake;
			Assert::AreEqual<uint64_t>(7, view.QueueGeneration());
			Assert::IsTrue(view.SlotFree());
			Assert::AreEqual<uint64_t>(3, view.Dropped());
			// Nothing registered: no provider answers the iid. A module that serves it is found through the hub.
			Assert::IsTrue(RE::Query(RE::kTimedFileFramesIid) == nullptr);
			Mod* a = new Mod("a");
			AddMod(a);
			RegisterHubForwarder();
			Assert::IsTrue(RE::Query(RE::kTimedFileFramesIid) == a);
		}
	};
}
