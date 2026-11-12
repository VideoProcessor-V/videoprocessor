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

#include <algorithm>
#include <fstream>
#include <iterator>
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
			int faults = 0;                 // WorkerFault reports this module received
			std::string faultWorker, faultWhat;
			explicit Mod(const char* name) : n(name) {}
			const char* Name() const override { return n.c_str(); }
			void OnStatus(const RE::Context&, RE::StatusKind kind, std::vector<std::string>&, const char* worker, const char* what) override
			{
				if (kind != RE::StatusKind::WorkerFault) return;
				++faults;
				faultWorker = worker ? worker : "";
				faultWhat = what ? what : "";
			}
			bool OnFrame(RE::FrameContext& c) override
			{
				g_log->push_back(n + ":" + std::to_string(static_cast<int>(c.phase)));
				seen = c;
				if (c.phase == RE::FramePhase::AfterRender && c.phaseData) { seenJob = *static_cast<RE::DrawJob*>(c.phaseData); sawJob = true; }
				if (throws) throw 1;
				return handles;
			}
		};

		std::string ReadText(const std::string& path)
		{
			std::ifstream in(path, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}

		std::string RepositoryRoot()
		{
			std::string root = __FILE__;
			std::replace(root.begin(), root.end(), '/', '\\');
			root.resize(root.rfind("\\src\\VP-V\\tests\\"));
			return root;
		}

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
			// The fault is reported, once, with the throwing module's name (audit 2).
			Assert::AreEqual(1, b->faults);
			Assert::AreEqual(std::string("a"), b->faultWorker);
			Assert::AreEqual(std::string("exception"), b->faultWhat);
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

		// Source-order pin (70b section 9.5-3): ONE Entrance call, right before the entrance-decision block, before the
		// source-monitor feed, the hook list and the render call, and outside the draw callback (it is not re-run on an
		// NLS retry because it precedes RenderOwned, whose callback is the only place the draw runs twice).
		TEST_METHOD(TheEntranceSiteSitsBeforeTheEntranceDecisionsAndOutsideTheDrawCallback)
		{
			const std::string text = ReadText(RepositoryRoot() + "\\src\\VideoProcessor-Lib\\vprenderer\\LibplaceboVideoRenderer.cpp");
			Assert::IsFalse(text.empty(), L"the renderer source");
			const size_t site = text.find("RendererExtension::NotifyEntrance(");
			Assert::IsTrue(site != std::string::npos, L"the Entrance site");
			Assert::IsTrue(text.find("RendererExtension::NotifyEntrance(", site + 1) == std::string::npos, L"exactly one site");
			const size_t stepA = text.find("// Stage 12 step A: the re-base factor for this frame", site);
			Assert::IsTrue(stepA != std::string::npos && stepA - site < 700, L"right before the entrance-decision block");
			const size_t monitor = text.find("sourceMonitor.Frame(", site);
			const size_t hooks = text.find("customHooks.Ensure(hookTemplates)", site);
			const size_t render = text.find("NlsHookRecovery::RenderOwned(", site);
			const size_t after = text.find("RendererExtension::NotifyAfterRender(", site);
			Assert::IsTrue(monitor != std::string::npos && hooks != std::string::npos && render != std::string::npos &&
				after != std::string::npos, L"the later calls exist");
			Assert::IsTrue(site < stepA && stepA < monitor && monitor < hooks && hooks < render && render < after,
				L"Entrance, then the entrance decisions, the hook list, the render call, AfterRender");
		}

		// M2 P4 h (owner E3, lead ruling 2026-11-11): the after-render reports run through ONE call-site line that is guarded
		// off in the Original pathway, sits right before NotifyAfterRender, keeps the statement order (reports, then the Dynamic
		// Drive stats), and the moved body in CoreSegments.inl stays a pure move (no pathway check of its own).
		TEST_METHOD(TheAfterRenderReportsAreGuardedAtTheCallSiteAndTheBodyIsAPureMove)
		{
			const std::string text = ReadText(RepositoryRoot() + "\src\VideoProcessor-Lib\vprenderer\LibplaceboVideoRenderer.cpp");
			Assert::IsFalse(text.empty(), L"the renderer source");
			const std::string guard = "if (!pipelineSlots.OriginalPathway()) { ReportRenderErrors(); dynamicDrive.CollectMeasurements(); }";
			const size_t site = text.find(guard);
			Assert::IsTrue(site != std::string::npos, L"the guarded call-site line");
			Assert::IsTrue(text.find(guard, site + 1) == std::string::npos, L"exactly one call site");
			const size_t after = text.find("RendererExtension::NotifyAfterRender(", site);
			Assert::IsTrue(after != std::string::npos && after - site < 600, L"right before AfterRender");
			Assert::IsTrue(text.find("void ReportRenderErrors();") != std::string::npos, L"the declaration stays in Impl");
			Assert::IsTrue(text.find("pl_renderer_get_errors(renderer)", site - 3000) == std::string::npos ||
				text.find("pl_renderer_get_errors(renderer)", site - 3000) > site, L"the report block left the render function");

			const std::string body = ReadText(RepositoryRoot() + "\src\VP-V\segments\CoreSegments.inl");
			const size_t def = body.find("LibplaceboVideoRenderer::Impl::ReportRenderErrors()");
			Assert::IsTrue(def != std::string::npos, L"the member definition");
			Assert::IsTrue(body.find("OriginalPathway", def) == std::string::npos, L"no pathway check inside the moved body");
			const size_t errors = body.find("pl_renderer_get_errors(renderer)", def);
			const size_t drive = body.find("dynamicDrive.OnHookErrors(", def);
			const size_t calibration = body.find("calibrationHost.AfterRender(hookErrors)", def);
			Assert::IsTrue(errors != std::string::npos && drive != std::string::npos && calibration != std::string::npos &&
				errors < drive && drive < calibration, L"the body keeps its order");
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
