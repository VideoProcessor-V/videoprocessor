// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Plan 58 T1 (call-site group B, B3/B4): the look-ahead walks and the vector-engine fallback sync,
// run against a fake host. Pure CPU, no GPU.
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

#include <VideoFrameEncoding.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
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
			MotionAhead::Producer::Health GetHealth() const { return health; }
			void Submit(unsigned long long, unsigned long long, const AnalysisLumaSource&, bool) {}
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
			std::unique_ptr<FakeProducer> motionProducer;
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
	}

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
			VectorEngineFallbackSync::Release(host);
			Assert::IsFalse(host.fallbacks.Engaged("vector producer"));
			Assert::IsFalse(host.fallbacks.Engaged("vector_engine_producer"));
		}
	};
}
