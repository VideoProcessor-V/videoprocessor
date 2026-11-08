// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// M2 P4 (doc 52 section 4, 70b section 9): the Entrance / CollectHooks / AfterRender phases fan out (every module is
// called in order although an earlier one returns true), the Entrance helper delivers its facts, a throwing module
// does not skip the next one, nothing registered is inert, and Draw / Begin keep the first-wins rule.

#include <vprenderer/RendererExtension.h>
#include "../../core/hub/ExtensionHub.h"

#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace EntranceHookTest
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
		};

		struct Mod : Hub::ISkeletonModule
		{
			std::string n;
			bool handles = false;
			bool throws = false;
			RE::FrameContext seen;
			RE::DrawJob seenJob;
			bool sawJob = false;
			explicit Mod(const char* name) : n(name) {}
			const char* Name() const override { return n.c_str(); }
			bool OnFrame(RE::FrameContext& c) override
			{
				g_log->push_back(n + ":" + std::to_string(static_cast<int>(c.phase)));
				seen = c;
				if (c.phase == RE::FramePhase::AfterRender && c.phaseData) { seenJob = *static_cast<RE::DrawJob*>(c.phaseData); sawJob = true; }
				if (throws) throw 1;
				return handles;
			}
		};

		struct Rec : RE::IRendererExtension
		{
			std::string n; bool handle;
			Rec(const char* name, bool h) : n(name), handle(h) {}
			bool OnFrame(RE::FrameContext& f) override { g_log->push_back(n + ":" + std::to_string(static_cast<int>(f.phase))); return handle; }
		};
	}

	TEST_CLASS(EntranceHookTests)
	{
	public:
		TEST_METHOD(TheThreeObserverPhasesFanOutAndTheTakeOverPhasesDoNot)
		{
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::Entrance));
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::CollectHooks));
			Assert::IsTrue(RE::PhaseFansOut(RE::FramePhase::AfterRender));
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::Begin));
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::Draw));
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::Upload));
			Assert::IsFalse(RE::PhaseFansOut(RE::FramePhase::Convert));
		}

		TEST_METHOD(TheRegistryCallsEveryExtensionAtTheThreePhasesAndStopsAtDraw)
		{
			Scope s;
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Rec("a", true)));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Rec("b", false)));
			int host = 0;
			RE::FrameContext c;
			c.host = &host;
			c.phase = RE::FramePhase::Entrance;     Assert::IsTrue(RE::Frame(c));
			c.phase = RE::FramePhase::CollectHooks; Assert::IsTrue(RE::Frame(c));
			c.phase = RE::FramePhase::AfterRender;  Assert::IsTrue(RE::Frame(c));
			c.phase = RE::FramePhase::Draw;         Assert::IsTrue(RE::Frame(c));   // first-wins: b is not called
			const std::vector<std::string> expected = { "a:6", "b:6", "a:8", "b:8", "a:10", "b:10", "a:9" };
			Assert::IsTrue(expected == s.log);
		}

		TEST_METHOD(TheHubReachesEveryModuleAtEntranceAlthoughTheFirstReturnsTrue)
		{
			Scope s;
			Mod* a = new Mod("a"); a->handles = true;
			Mod* b = new Mod("b");
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(a));
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(b));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Fwd()));
			int host = 0;
			pl_frame image{}, target{};
			RE::NotifyEntrance(&host, 41, 42, 7, &image, &target, 1920, 1080, true, 23.976);
			const std::vector<std::string> expected = { "a:6", "b:6" };
			Assert::IsTrue(expected == s.log);
			for (Mod* m : { a, b })
			{
				Assert::IsTrue(m->seen.host == &host);
				Assert::AreEqual<int64_t>(41, m->seen.seq);
				Assert::AreEqual<int64_t>(42, m->seen.nextSeq);
				Assert::AreEqual<int64_t>(7, m->seen.generation);
				Assert::IsTrue(m->seen.image == &image && m->seen.target == &target);
				Assert::AreEqual(1920, m->seen.width);
				Assert::AreEqual(1080, m->seen.height);
				Assert::IsTrue(m->seen.cadenceRepeat);
				Assert::AreEqual(23.976, m->seen.captureRateHz);
				Assert::IsTrue(m->seen.phase == RE::FramePhase::Entrance);
			}
		}

		TEST_METHOD(AThrowingModuleDoesNotSkipTheNextOneAtEntrance)
		{
			Scope s;
			Mod* a = new Mod("a"); a->throws = true;
			Mod* b = new Mod("b");
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(a));
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(b));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Fwd()));
			int host = 0;
			RE::NotifyEntrance(&host, 1, 2, 1, nullptr, nullptr, 0, 0, false, 0.0);
			const std::vector<std::string> expected = { "a:6", "b:6" };
			Assert::IsTrue(expected == s.log);
		}

		TEST_METHOD(NothingRegisteredAndAnEmptyHubAreInert)
		{
			Scope s;
			int host = 0;
			RE::NotifyEntrance(&host, 1, 2, 1, nullptr, nullptr, 0, 0, false, 0.0);   // no extension at all
			RE::NotifyAfterRender(&host, 1, 1, 0, 0, true);
			Assert::IsTrue(s.log.empty());
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Fwd()));       // the hub without a module
			RE::NotifyEntrance(&host, 1, 2, 1, nullptr, nullptr, 0, 0, false, 0.0);
			RE::NotifyAfterRender(&host, 1, 1, 0, 0, true);
			Assert::IsTrue(s.log.empty());
		}

		TEST_METHOD(AfterRenderCarriesTheErrorReportAndEveryModuleSeesIt)
		{
			Scope s;
			Mod* a = new Mod("a"); a->handles = true;
			Mod* b = new Mod("b");
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(a));
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(b));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Fwd()));
			int host = 0;
			pl_render_errors errors{};
			RE::NotifyAfterRender(&host, 9, 3, 5, 1, true, &errors);
			Assert::IsTrue((std::vector<std::string>{ "a:10", "b:10" }) == s.log);
			for (Mod* m : { a, b })
			{
				Assert::IsTrue(m->sawJob);
				Assert::AreEqual(5, m->seenJob.errors);
				Assert::AreEqual(1, m->seenJob.retried);
				Assert::IsTrue(m->seenJob.rendered);
				Assert::IsTrue(m->seenJob.renderErrors == &errors);
			}
			RE::NotifyAfterRender(&host, 9, 3, 0, 0, false);   // the default: no report
			Assert::IsTrue(b->seenJob.renderErrors == nullptr);
		}

		TEST_METHOD(DrawJobDefaultsAreInert)
		{
			RE::DrawJob job;
			Assert::AreEqual(0, job.errors);
			Assert::AreEqual(0, job.retried);
			Assert::IsFalse(job.rendered);
			Assert::IsTrue(job.renderErrors == nullptr && job.source == nullptr && job.frame == nullptr &&
				job.destination == nullptr && job.params == nullptr);
			Assert::IsTrue(job.result == RE::DrawResult::Continue);
		}
	};
}
