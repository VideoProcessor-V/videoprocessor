// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// The generic hook layer (M2 P0): the renderer extension registry (H1), the capture observer
// registry (H6) and the log observer (H8). Empty = Bill's behaviour; registered = dispatch in
// registration order; a throwing extension never reaches the caller.

#include <vprenderer/RendererExtension.h>
#include <CaptureDeliveryHook.h>
#include <DebugLog.h>
#include "../../core/hub/ExtensionHub.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace HookLayerTest
{
	namespace
	{
		namespace RE = RendererExtension;

		std::vector<std::string>* g_log = nullptr;

		struct Recorder : RE::IRendererExtension
		{
			std::string name;
			double rateAdd = 0;
			bool throwOnQueue = false;
			bool throwOnFault = false;
			bool handleFrame = false;
			void* iface = nullptr;
			explicit Recorder(const char* n) : name(n) {}
			void OnQueue(const RE::QueueFacts& f) override
			{
				g_log->push_back(name + ":queue:" + std::to_string(f.seq));
				if (throwOnQueue) throw std::runtime_error("boom");
			}
			void OnLifecycle(RE::LifecycleFacts& f) override
			{
				g_log->push_back(name + ":life:" + std::to_string(static_cast<int>(f.kind)) + ":" + std::to_string(reinterpret_cast<intptr_t>(f.host)));
			}
			bool OnFrame(RE::FrameContext& f) override
			{
				g_log->push_back(name + ":frame:" + std::to_string(f.seq));
				return handleFrame;
			}
			double OnResolve(RE::Resolve, const RE::Context&, double v) override { g_log->push_back(name + ":rate"); return v + rateAdd; }
			void OnStatus(const RE::Context&, RE::StatusKind k, std::vector<std::string>& out, const char* w, const char* what) override
			{
				if (k == RE::StatusKind::WorkerFault)
				{
					g_log->push_back(name + ":fault:" + w + ":" + what);
					if (throwOnFault) throw std::runtime_error("fault boom");
				}
				else out.push_back(name);
			}
			void* Query(const char*) override { return iface; }
		};

		struct CaptureRecorder : ICaptureObserver
		{
			std::string name;
			bool throws = false;
			explicit CaptureRecorder(const char* n) : name(n) {}
			void OnCaptureFrame(const CaptureFrameFacts& f) override
			{
				g_log->push_back(name + ":frame:" + std::to_string(f.sequence));
				if (throws) throw 1;
			}
			void OnSourceFormatChange(const CaptureFrameFacts& f) override { g_log->push_back(name + ":format:" + std::to_string(f.width)); }
		};

		int g_observerCalls = 0;
		std::string g_observerText;
		void __cdecl LogSeen(int, const char* text) { ++g_observerCalls; g_observerText = text; }

		struct Scope
		{
			std::vector<std::string> log;
			Scope() { g_log = &log; RE::ResetForTesting(); CaptureDeliveryHook::ResetForTesting(); DebugLog::SetLogObserver(nullptr); }
			~Scope() { RE::ResetForTesting(); CaptureDeliveryHook::ResetForTesting(); DebugLog::SetLogObserver(nullptr); g_log = nullptr; }
		};
	}

	TEST_CLASS(HookLayerTests)
	{
	public:
		TEST_METHOD(RendererExtensionEmptyIsBillBehaviour)
		{
			Scope s;
			Assert::IsFalse(RE::Any());
			RE::Context ctx;
			Assert::AreEqual(23.976, RE::ResolveValue(RE::Resolve::SourceSignalRate, ctx, 23.976));
			Assert::IsTrue(RE::Query("x") == nullptr);
			std::vector<std::string> status;
			RE::Status(ctx, RE::StatusKind::Lines, status);
			RE::Queue(RE::QueueFacts());
			RE::LifecycleFacts life;
			RE::Notify(life);
			RE::FrameContext fc;
			Assert::IsFalse(RE::Frame(fc));
			Assert::AreEqual(0u, fc.requests);
			RE::IngressContext ic;
			RE::Ingress(ic);
			Assert::IsFalse(ic.drop);
			std::string reply;
			Assert::IsFalse(RE::Command(ctx, "m", "c", reply));
			Assert::IsTrue(status.empty());
			Assert::IsTrue(s.log.empty());
		}

		TEST_METHOD(RendererExtensionDispatchesInRegistrationOrder)
		{
			Scope s;
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Recorder("a")));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Recorder("b")));
			RE::Register(nullptr);
			Assert::IsTrue(RE::Any());
			RE::QueueFacts f;
			f.seq = 7;
			RE::Queue(f);
			std::vector<std::string> status;
			RE::Status(RE::Context(), RE::StatusKind::Fallbacks, status);
			const std::vector<std::string> expected = { "a:queue:7", "b:queue:7" };
			Assert::IsTrue(expected == s.log);
			Assert::IsTrue((std::vector<std::string>{ "a", "b" }) == status);
		}

		TEST_METHOD(RendererExtensionFrameFirstHandlerStopsChain)
		{
			Scope s;
			Recorder* a = new Recorder("a"); a->handleFrame = true;
			RE::Register(std::unique_ptr<RE::IRendererExtension>(a));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Recorder("b")));
			RE::FrameContext fc;
			fc.seq = 4;
			Assert::IsTrue(RE::Frame(fc));
			Assert::IsTrue((std::vector<std::string>{ "a:frame:4" }) == s.log);
		}

		TEST_METHOD(RendererExtensionHostKeyedLifecycle)
		{
			Scope s;
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Recorder("a")));
			int hostA = 0, hostB = 0;
			RE::LifecycleFacts f;
			f.kind = RE::Lifecycle::GpuReleased;
			f.host = &hostA;
			RE::Notify(f);
			f.host = &hostB;
			RE::Notify(f);
			Assert::AreEqual<size_t>(2, s.log.size());
			Assert::IsTrue(s.log[0] != s.log[1]); // each call carries its own host identity
		}

		TEST_METHOD(RendererExtensionResolveChainsInOrder)
		{
			Scope s;
			Recorder* a = new Recorder("a"); a->rateAdd = 1.0;
			Recorder* b = new Recorder("b"); b->rateAdd = 10.0;
			RE::Register(std::unique_ptr<RE::IRendererExtension>(a));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(b));
			Assert::AreEqual(111.0, RE::ResolveValue(RE::Resolve::DisplayRefreshRate, RE::Context(), 100.0));
			Assert::IsTrue((std::vector<std::string>{ "a:rate", "b:rate" }) == s.log);
		}

		TEST_METHOD(RendererExtensionThrowIsContainedAndReportedOnce)
		{
			Scope s;
			Recorder* a = new Recorder("a"); a->throwOnQueue = true;
			Recorder* b = new Recorder("b"); b->throwOnFault = true;
			RE::Register(std::unique_ptr<RE::IRendererExtension>(a));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(b));
			RE::QueueFacts f;
			f.seq = 3;
			RE::Queue(f); // must not throw
			const std::vector<std::string> expected = {
				"a:queue:3", "a:fault:extension:boom", "b:fault:extension:boom",
				"b:queue:3" };
			// b's own throw from OnStatus(WorkerFault) is swallowed; b still receives the event.
			Assert::IsTrue(expected == s.log);
		}

		TEST_METHOD(RendererExtensionQueryReturnsFirstNonNull)
		{
			Scope s;
			int one = 1, two = 2;
			Recorder* a = new Recorder("a");
			Recorder* b = new Recorder("b"); b->iface = &one;
			Recorder* c = new Recorder("c"); c->iface = &two;
			RE::Register(std::unique_ptr<RE::IRendererExtension>(a));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(b));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(c));
			Assert::IsTrue(RE::Query("iid") == &one);
		}

		TEST_METHOD(RendererExtensionResetRestoresEmpty)
		{
			Scope s;
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Recorder("a")));
			Assert::IsTrue(RE::Any());
			RE::ResetForTesting();
			Assert::IsFalse(RE::Any());
			Assert::AreEqual(5.0, RE::ResolveValue(RE::Resolve::PresentClockRate, RE::Context(), 5.0));
		}

		TEST_METHOD(HubIsInertWithNoModulesAndKeysStateByHost)
		{
			Scope s;
			Vpv::Hub::ExtensionHub& hub = Vpv::Hub::ExtensionHub::Instance();
			hub.ResetModulesForTesting();
			Assert::AreEqual<size_t>(0, hub.ModuleCount());
			int hostA = 0, hostB = 0;
			RE::LifecycleFacts f;
			f.host = &hostA; f.kind = RE::Lifecycle::GpuCreated; hub.OnLifecycle(f);
			f.host = &hostB; hub.OnLifecycle(f);
			Assert::AreEqual<size_t>(2, hub.LiveHostCount());
			f.host = &hostA; f.kind = RE::Lifecycle::GpuReleased; hub.OnLifecycle(f);
			Assert::IsFalse(hub.HasHost(&hostA));
			Assert::IsTrue(hub.HasHost(&hostB)); // releasing A leaves B alone
			RE::FrameContext fc;
			Assert::IsFalse(hub.OnFrame(fc));
			Assert::AreEqual(7.0, hub.OnResolve(RE::Resolve::DisplayRefreshRate, RE::Context(), 7.0));
			Assert::IsTrue(hub.Query("x") == nullptr);
			hub.ResetModulesForTesting();
		}

		TEST_METHOD(HubFansOutToModulesInOrder)
		{
			Scope s;
			struct Mod : Vpv::Hub::ISkeletonModule
			{
				std::string n; double add;
				Mod(const char* name, double a) : n(name), add(a) {}
				const char* Name() const override { return n.c_str(); }
				void OnQueue(const RE::QueueFacts& f) override { g_log->push_back(n + ":queue:" + std::to_string(f.seq)); }
				double OnResolve(RE::Resolve, const RE::Context&, double v) override { return v + add; }
			};
			Vpv::Hub::ExtensionHub& hub = Vpv::Hub::ExtensionHub::Instance();
			hub.ResetModulesForTesting();
			hub.AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new Mod("m1", 1.0)));
			hub.AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new Mod("m2", 10.0)));
			RE::QueueFacts q; q.seq = 5;
			hub.OnQueue(q);
			Assert::IsTrue((std::vector<std::string>{ "m1:queue:5", "m2:queue:5" }) == s.log);
			Assert::AreEqual(111.0, hub.OnResolve(RE::Resolve::PresentClockRate, RE::Context(), 100.0));
			hub.ResetModulesForTesting();
		}

		TEST_METHOD(CaptureObserverEmptyIsNoCall)
		{
			Scope s;
			Assert::IsFalse(CaptureDeliveryHook::Any());
			CaptureDeliveryHook::Frame(CaptureFrameFacts());
			CaptureDeliveryHook::FormatChange(CaptureFrameFacts());
			Assert::IsTrue(s.log.empty());
		}

		TEST_METHOD(CaptureObserverDispatchOrderAndContainment)
		{
			Scope s;
			CaptureRecorder* a = new CaptureRecorder("a"); a->throws = true;
			CaptureDeliveryHook::Register(std::unique_ptr<ICaptureObserver>(a));
			CaptureDeliveryHook::Register(std::unique_ptr<ICaptureObserver>(new CaptureRecorder("b")));
			CaptureDeliveryHook::Register(nullptr);
			Assert::IsTrue(CaptureDeliveryHook::Any());
			CaptureFrameFacts f;
			f.sequence = 9;
			f.width = 1920;
			CaptureDeliveryHook::Frame(f); // a throws; must not escape, b still runs
			CaptureDeliveryHook::FormatChange(f);
			const std::vector<std::string> expected = { "a:frame:9", "b:frame:9", "a:format:1920", "b:format:1920" };
			Assert::IsTrue(expected == s.log);
			CaptureDeliveryHook::ResetForTesting();
			Assert::IsFalse(CaptureDeliveryHook::Any());
		}

		TEST_METHOD(LogObserverIsCalledAfterTheSinkAndNullIsNothing)
		{
			Scope s;
			g_observerCalls = 0;
			g_observerText.clear();
			DebugLog::Log("hook layer %d", 1);
			Assert::AreEqual(0, g_observerCalls);
			DebugLog::SetLogObserver(&LogSeen);
			DebugLog::Log("hook layer %d", 2);
			Assert::AreEqual(1, g_observerCalls);
			Assert::AreEqual(std::string("hook layer 2"), g_observerText);
			DebugLog::SetLogObserver(nullptr);
			DebugLog::Log("hook layer %d", 3);
			Assert::AreEqual(1, g_observerCalls);
		}
	};
}
