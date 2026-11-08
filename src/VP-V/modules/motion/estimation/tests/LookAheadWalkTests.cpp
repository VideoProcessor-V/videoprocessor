// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Plan 58 T1 (call-site group B, B3/B4): the look-ahead walks and the vector-engine fallback sync,
// run against a fake host. Pure CPU, no GPU.
// P5a-rest fix (producer race): ProducerRaceTests, two threads (publish / reset vs the loop's hand-off).
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackStatus.h>
#include <VP-V/modules/input/source_analysis/SourceAheadWalk.h>
#include <VP-V/modules/motion/estimation/MotionAheadWalk.h>
#include <VP-V/modules/motion/estimation/VectorEngineFallbackSync.h>
#include <VP-V/modules/motion/estimation/VectorEngineState.h>

#include <VideoFrameEncoding.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LookAheadWalkTest
{
	namespace
	{
		struct FakeMode { double RefreshRateHz() const { return 24.0; } };
		struct FakeState { std::shared_ptr<FakeMode> displayMode; bool dolbyVisionTunnel = false; };
		struct FakeDolby
		{
			bool valid = false, requiresBlackFrame = false, allowSdrFallback = false;
			int rgbBitAlignment = 0;
			float nonlinear[9] = {};
			float nonlinearOffset[3] = {};
			float linear[9] = {};
		};
		struct FakeConverted { FakeDolby dolby; VideoFrameEncoding captureEncoding = VideoFrameEncoding::UNKNOWN; };
		struct FakeFrame { long long GetTimingTimestamp() const { return 0; } };
		struct FakeIdentity { int sourceFormatGeneration = 0; };
		struct FakeQueued
		{
			FakeFrame frame;
			std::shared_ptr<FakeState> state;
			std::shared_ptr<const FakeConverted> converted;
			std::shared_ptr<int> analysis;
			FakeIdentity activePictureIdentity;
			double filmHz = 0.0;
			long long arrivalQpc = 0;
			bool gapBefore = false, filmUpdate = false;
			unsigned long long sourceSequence = 0;
		};
		struct FakeClock { long long TimingClockTicksPerSecond() const { return 1000; } };
		struct FakeValidity
		{
			bool fail = false;
			unsigned long long StreamGeneration() const { if (fail) throw std::runtime_error("injected"); return 1; }
		};
		struct FakeProducer
		{
			MotionAhead::Producer::Health health;
			std::atomic<unsigned long long> submitted{ 0 };
			MotionAhead::Producer::Health GetHealth() const { return health; }
			void Submit(unsigned long long, unsigned long long, const AnalysisLumaSource&, bool) { submitted.fetch_add(1, std::memory_order_relaxed); }
		};
		struct FakeMonitor
		{
			template <class F> void LookAheadFrame(unsigned long long, int, int, F&&, int, int, int, int) {}
		};
		struct Prepared { AnalysisLumaSource source; };
		struct FakeImpl
		{
			FallbackStatus::Registry fallbacks;
			void SyncFallback(const FallbackCatalog::Decision& d) { FallbackCatalog::Sync(fallbacks, d); }
			FallbackCatalog::LookAheadFailures motionAheadFailures = FallbackCatalog::MotionAheadWorker();
			FallbackCatalog::LookAheadFailures sourceAheadFailures = FallbackCatalog::SourceResolutionWorker();
			FakeValidity motionValidity;
			std::atomic<size_t> queuedFutureFrames{ 0 };
			struct { bool vectorDemand = false, timingContinuityFixes = false, motionHistoryFixes = false; } activeSettings;
			std::atomic<int> motionAheadConfigured{ 4 }, queueAheadCapacity{ 0 };
			std::atomic<bool> motionAheadBackward{ false };
			bool motionAheadEngaged = false;
			unsigned long long motionAheadEngagedGeneration = 0;
			VectorDemand::Decision vectorDemand;
			std::shared_ptr<FakeProducer> motionProducer;   // the walk loads it with std::atomic_load (producer race fix)
			bool motionAheadPrevKnown = false;
			unsigned long long motionAheadPrevGeneration = 0;
			DejudderPolicy::MotionAheadFrame motionAheadPrev;
			FakeMonitor sourceMonitor;
			// The walks' PrepareFrameAnalysis always throws: reached with one queued frame.
			template <class... A> static Prepared PrepareFrameAnalysis(A&&...) { throw std::runtime_error("injected prepare"); }
		};
		FakeQueued OneFrame()
		{
			FakeQueued q;
			q.state = std::make_shared<FakeState>();
			q.state->displayMode = std::make_shared<FakeMode>();
			return q;
		}

		// A valid 4x4 luma source (packed RGB, 4 bytes a pixel), so the walk reaches the producer's Submit.
		AnalysisLumaSource ValidLuma()
		{
			static uint8_t pixels[4 * 4 * 4] = {};
			AnalysisLumaSource s;
			s.data = pixels;
			s.dataBytes = sizeof(pixels);
			s.width = 4;
			s.height = 4;
			s.rowBytes = 16;
			s.format = AnalysisLumaFormat::NativeRgb;
			return s;
		}

		// The walk's host with a preparation that succeeds (the base one throws).
		struct RaceImpl : FakeImpl
		{
			template <class... A> static Prepared PrepareFrameAnalysis(A&&...) { Prepared p; p.source = ValidLuma(); return p; }
		};
	}

	// fix (producer race): the render thread creates / stops / resets the producer while the render loop
	// null-checks it and submits to it. With the shared_ptr published by std::atomic_store and read by one
	// std::atomic_load copy per use, the loop's copy keeps the object (and the mutex Submit locks) alive.
	// Two threads for a fixed time; no crash, and the hand-off really reached Submit.
	TEST_CLASS(ProducerRaceTests)
	{
	public:
		TEST_METHOD(WalkSubmitsWhileTheProducerIsReplacedAndReset)
		{
			RaceImpl impl;
			impl.motionAheadBackward.store(true);   // frames ahead without the engage wait
			std::vector<FakeQueued> frames;
			for (int i = 0; i < 4; ++i)
			{
				frames.push_back(OneFrame());
				frames.back().sourceSequence = static_cast<unsigned long long>(i + 1);
			}
			const std::atomic<double> rate{ 24.0 };
			const FakeClock clock;
			std::atomic<bool> stop{ false };
			std::thread publisher([&] {
				unsigned long long n = 0;
				while (!stop.load(std::memory_order_acquire))
				{
					if ((++n & 1) != 0)
						std::atomic_store(&impl.motionProducer, std::make_shared<FakeProducer>());
					else
						std::atomic_store(&impl.motionProducer, std::shared_ptr<FakeProducer>());
				}
			});
			MotionAhead::AheadView<RaceImpl, FakeQueued, FakeClock> view{ impl, frames, &clock, rate };
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
			unsigned long long walks = 0;
			while (std::chrono::steady_clock::now() < until)
			{
				MotionAhead::RunQueueWalk(view);
				++walks;
			}
			stop.store(true, std::memory_order_release);
			publisher.join();
			Assert::IsTrue(walks > 0);
			Assert::IsFalse(impl.fallbacks.Engaged("motion look-ahead"), L"no walk threw");
			// One last producer, so the walk's Submit is shown reached even when the timing never overlapped.
			std::atomic_store(&impl.motionProducer, std::make_shared<FakeProducer>());
			MotionAhead::RunQueueWalk(view);
			Assert::AreEqual<unsigned long long>(4, impl.motionProducer->submitted.load(), L"the walk submits every frame ahead");
		}

		// The real producer type through the engine state's publish / loop accessors (not started: no device;
		// Submit returns early, LastSubmitted and GetStats read its atomics and lock its mutex).
		TEST_METHOD(EngineStateLoopCopyOutlivesTheRenderThreadsReset)
		{
			VpMotion::VectorEngineState state;
			std::atomic<bool> stop{ false };
			std::thread renderThread([&] {
				unsigned long long n = 0;
				while (!stop.load(std::memory_order_acquire))
				{
					if ((++n & 1) != 0)
						state.PublishProducer(std::make_shared<MotionAhead::Producer>());
					else
					{
						if (state.motionProducer)   // the writer's own thread reads directly (as PlanProducer)
							state.motionProducer->Stop();
						state.PublishProducer(nullptr);
					}
				}
			});
			const AnalysisLumaSource luma = ValidLuma();
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
			unsigned long long seen = 0, seq = 0;
			while (std::chrono::steady_clock::now() < until)
			{
				const std::shared_ptr<MotionAhead::Producer> producer = state.LoopProducer();
				if (!producer)
					continue;
				++seen;
				producer->Submit(++seq, 1, luma, false);
				(void)producer->LastSubmitted();
				(void)producer->GetStats();
				Assert::IsFalse(producer->Running(), L"never started (no device)");
			}
			stop.store(true, std::memory_order_release);
			renderThread.join();
			Assert::IsTrue(seen > 0, L"the loop saw published producers");
		}
	};

	TEST_CLASS(LookAheadWalkTests)
	{
	public:
		// B4: a throw in the motion walk engages the visible entry; eight good batches release it.
		TEST_METHOD(MotionWalkThrowEngagesAndReleasesAfterEightGoodBatches)
		{
			FakeImpl impl;
			std::vector<FakeQueued> frames;
			const std::atomic<double> rate{ 24.0 };
			const FakeClock clock;
			MotionAhead::AheadView<FakeImpl, FakeQueued, FakeClock> view{ impl, frames, &clock, rate };
			MotionAhead::RunQueueWalk(view);
			Assert::IsFalse(impl.fallbacks.Engaged("motion look-ahead"), L"a good batch engages nothing");
			impl.motionValidity.fail = true;
			MotionAhead::RunQueueWalk(view);
			Assert::IsTrue(impl.fallbacks.Engaged("motion look-ahead"));
			Assert::AreEqual<unsigned long long>(1, impl.motionAheadFailures.failures);
			impl.motionValidity.fail = false;
			for (int i = 0; i < 7; ++i)
			{
				MotionAhead::RunQueueWalk(view);
				Assert::IsTrue(impl.fallbacks.Engaged("motion look-ahead"), L"held through seven good batches");
			}
			MotionAhead::RunQueueWalk(view);
			Assert::IsFalse(impl.fallbacks.Engaged("motion look-ahead"), L"released after eight");
		}

		TEST_METHOD(SourceWalkThrowEngagesAndReleases)
		{
			FakeImpl impl;
			std::vector<FakeQueued> frames;
			SourceResolution::SourceAheadView<FakeImpl, FakeQueued> view{ impl, frames };
			SourceResolution::RunAheadWalk(view);
			Assert::IsFalse(impl.fallbacks.Engaged("source resolution look-ahead"));
			frames.push_back(OneFrame());   // PrepareFrameAnalysis throws for it
			SourceResolution::RunAheadWalk(view);
			Assert::IsTrue(impl.fallbacks.Engaged("source resolution look-ahead"));
			frames.clear();
			for (int i = 0; i < 8; ++i)
				SourceResolution::RunAheadWalk(view);
			Assert::IsFalse(impl.fallbacks.Engaged("source resolution look-ahead"));
		}

		// B3: the vector producer entry is synced with the producer's health, with the other three.
		TEST_METHOD(VectorEngineSyncAddsTheProducerEntry)
		{
			struct Host
			{
				FallbackStatus::Registry fallbacks;
				void SyncFallback(const FallbackCatalog::Decision& d) { FallbackCatalog::Sync(fallbacks, d); }
				VectorDemand::Decision vectorDemand;
				bool vectorProducerSkipSustained = false, vectorGlobalMissSustained = false;
				std::unique_ptr<FakeProducer> motionProducer;
			} host;
			host.vectorDemand.producer = true;
			VectorEngineFallbackSync::Sync(host, host);
			Assert::IsFalse(host.fallbacks.Engaged("vector producer"), L"no producer object: not recovering");
			host.motionProducer.reset(new FakeProducer());
			host.motionProducer->health.recovering = true;
			host.motionProducer->health.stops = 2;
			host.motionProducer->health.error = "no device";
			VectorEngineFallbackSync::Sync(host, host);
			Assert::IsTrue(host.fallbacks.Engaged("vector producer"));
			host.vectorProducerSkipSustained = true;
			VectorEngineFallbackSync::Sync(host, host);
			Assert::IsTrue(host.fallbacks.Engaged("vector_engine_producer"), L"the old entries are still synced");
			host.motionProducer->health.recovering = false;
			VectorEngineFallbackSync::Sync(host, host);
			Assert::IsFalse(host.fallbacks.Engaged("vector producer"));
			host.motionProducer->health.recovering = true;
			VectorEngineFallbackSync::Sync(host, host);
			Assert::IsTrue(host.fallbacks.Engaged("vector producer"));
			host.vectorDemand.producer = false;   // recovering, but the look-ahead is not wanted: nothing to show
			VectorEngineFallbackSync::Sync(host, host);
			Assert::IsFalse(host.fallbacks.Engaged("vector producer"), L"wanted == false releases a recovering producer");
			host.vectorDemand.producer = true;
			VectorEngineFallbackSync::Sync(host, host);
			Assert::IsTrue(host.fallbacks.Engaged("vector producer"));
			VectorEngineFallbackSync::Release(host);
			Assert::IsFalse(host.fallbacks.Engaged("vector producer"));
			Assert::IsFalse(host.fallbacks.Engaged("vector_engine_producer"));
		}
	};
}
