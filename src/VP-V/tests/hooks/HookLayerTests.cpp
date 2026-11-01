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
			bool throwOnEvent = false;
			bool throwOnFault = false;
			void* iface = nullptr;
			explicit Recorder(const char* n) : name(n) {}
			void OnEvent(const RE::EventFacts& f) override
			{
				g_log->push_back(name + ":event:" + std::to_string(f.sequence));
				if (throwOnEvent) throw std::runtime_error("boom");
			}
			void OnGpuReleased() override { g_log->push_back(name + ":gpuReleased"); }
			double ResolveRate(RE::RateKind, double v) override { g_log->push_back(name + ":rate"); return v + rateAdd; }
			void OnWorkerFault(const char* w, const char* what) override
			{
				g_log->push_back(name + ":fault:" + w + ":" + what);
				if (throwOnFault) throw std::runtime_error("fault boom");
			}
			void CollectStatus(RE::StatusKind, std::vector<std::string>& out) override { out.push_back(name); }
			void* QueryInterface(const char*) override { return iface; }
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
			Assert::AreEqual(23.976, RE::ResolveRate(RE::RateKind::SourceSignal, 23.976));
			Assert::IsTrue(RE::Query("x") == nullptr);
			std::vector<std::string> status;
			RE::CollectStatus(RE::StatusKind::Lines, status);
			RE::Emit(RE::EventFacts());
			RE::GpuReleased();
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
			RE::EventFacts f;
			f.sequence = 7;
			RE::Emit(f);
			RE::GpuReleased();
			std::vector<std::string> status;
			RE::CollectStatus(RE::StatusKind::Fallbacks, status);
			const std::vector<std::string> expected = { "a:event:7", "b:event:7", "a:gpuReleased", "b:gpuReleased" };
			Assert::IsTrue(expected == s.log);
			Assert::IsTrue((std::vector<std::string>{ "a", "b" }) == status);
		}

		TEST_METHOD(RendererExtensionResolveRateChainsInOrder)
		{
			Scope s;
			Recorder* a = new Recorder("a"); a->rateAdd = 1.0;
			Recorder* b = new Recorder("b"); b->rateAdd = 10.0;
			RE::Register(std::unique_ptr<RE::IRendererExtension>(a));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(b));
			Assert::AreEqual(111.0, RE::ResolveRate(RE::RateKind::DisplayRefresh, 100.0));
			Assert::IsTrue((std::vector<std::string>{ "a:rate", "b:rate" }) == s.log);
		}

		TEST_METHOD(RendererExtensionThrowIsContainedAndReportedOnce)
		{
			Scope s;
			Recorder* a = new Recorder("a"); a->throwOnEvent = true;
			Recorder* b = new Recorder("b"); b->throwOnFault = true;
			RE::Register(std::unique_ptr<RE::IRendererExtension>(a));
			RE::Register(std::unique_ptr<RE::IRendererExtension>(b));
			RE::EventFacts f;
			f.sequence = 3;
			RE::Emit(f); // must not throw
			const std::vector<std::string> expected = {
				"a:event:3", "a:fault:extension:boom", "b:fault:extension:boom",
				"b:event:3" };
			// b's own throw from OnWorkerFault is swallowed; b still receives the event.
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
			Assert::AreEqual(5.0, RE::ResolveRate(RE::RateKind::PresentClock, 5.0));
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
