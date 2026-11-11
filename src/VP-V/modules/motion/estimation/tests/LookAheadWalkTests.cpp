// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Plan 58 T1 (call-site group B, B3/B4): the look-ahead walks and the vector-engine fallback sync,
// run against a fake host. Pure CPU, no GPU.
// P5a-rest hardening: ProducerRaceTests, two threads (publish / reset vs the loop's hand-off); LookAheadCopyTests (item h).
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
#include <cmath>
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
		struct FakeFrame
		{
			std::shared_ptr<int> addRefs = std::make_shared<int>(0);   // shared by the copies of one queued frame
			long long GetTimingTimestamp() const { return 0; }
			void SourceBufferAddRef() { ++*addRefs; }
		};
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
			bool gapBefore = false, filmUpdate = false, cadenceRepeat = false;
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
			unsigned long long last = 0;
			MotionAhead::Producer::Health GetHealth() const { return health; }
			int lead = -1;   // B1: the last lead the walk handed over (SetLead), -1 = never
			unsigned long long LastSubmitted() const { return last; }
			void SetLead(int l) { lead = l; }
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
			std::atomic<int> motionAheadConfigured{ 4 }, queueAheadCapacity{ 0 }, motionAheadDepth{ 0 };
			std::atomic<bool> motionAheadBackward{ false };
			bool motionAheadEngaged = false;
			unsigned long long motionAheadEngagedGeneration = 0;
			VectorDemand::Decision vectorDemand;
			std::shared_ptr<FakeProducer> motionProducer;   // the walk loads it with std::atomic_load (hardening)
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

	// P5a-rest item h: the look-ahead copy (render loop, queue lock), moved verbatim into CollectQueueAhead. The rule:
	// the current frame and up to `depth` more non-repeat frames with a state are counted; those after the producer's
	// last submitted sequence are copied and AddRef'd once; nothing without a producer or with neither a depth nor
	// backward pairs.
	TEST_CLASS(LookAheadCopyTests)
	{
	public:
		TEST_METHOD(CopiesTheUnsubmittedFramesUpToTheDepth)
		{
			FakeImpl impl;
			std::vector<FakeQueued> queue;
			for (int i = 0; i < 6; ++i)
			{
				queue.push_back(OneFrame());
				queue.back().sourceSequence = static_cast<unsigned long long>(i + 1);
			}
			queue[1].cadenceRepeat = true;   // seq 2: a repeat, not counted
			queue[4].state.reset();          // seq 5: no state, not counted
			std::vector<FakeQueued> out;
			MotionAhead::CollectQueueAhead(&impl, queue, out);
			Assert::AreEqual<size_t>(0, out.size(), L"no producer");
			impl.motionProducer = std::make_shared<FakeProducer>();
			MotionAhead::CollectQueueAhead(&impl, queue, out);
			Assert::AreEqual<size_t>(0, out.size(), L"no depth and no backward pairs");
			MotionAhead::CollectQueueAhead(static_cast<FakeImpl*>(nullptr), queue, out);
			Assert::AreEqual<size_t>(0, out.size(), L"no renderer impl");
			impl.motionAheadDepth.store(2);
			impl.motionProducer->last = 1;   // seq 1 already submitted
			MotionAhead::CollectQueueAhead(&impl, queue, out);
			// counted: seq 1 (taken 1, submitted), seq 3 (2), seq 4 (3 = depth + 1); seq 6 is beyond the depth
			Assert::AreEqual<size_t>(2, out.size());
			Assert::AreEqual<unsigned long long>(3, out[0].sourceSequence);
			Assert::AreEqual<unsigned long long>(4, out[1].sourceSequence);
			Assert::AreEqual(1, *queue[2].frame.addRefs);
			Assert::AreEqual(1, *queue[3].frame.addRefs);
			Assert::AreEqual(0, *queue[0].frame.addRefs);
			out.clear();
			impl.motionAheadDepth.store(0);
			impl.motionAheadBackward.store(true);   // backward pairs: the current frame only
			impl.motionProducer->last = 0;
			MotionAhead::CollectQueueAhead(&impl, queue, out);
			Assert::AreEqual<size_t>(1, out.size());
			Assert::AreEqual<unsigned long long>(1, out[0].sourceSequence);
		}
	};

	// B1 (owner 2026-11-10, doc 91 C): the walk's lead is every frame queued ahead, in both queue modes and at every rate; the
	// configured depth (DepthAt of motion_ahead_frames 4) is only its minimum; the P2 ring bounds it (kMaxLead).
	TEST_CLASS(MotionAheadLeadTests)
	{
		// A queue of the current frame plus `ahead` frames, the render loop's count stored as it does, then the copy.
		static size_t Collect(FakeImpl& impl, size_t ahead, int depth, std::vector<FakeQueued>& queue)
		{
			queue.clear();
			for (size_t i = 0; i <= ahead; ++i)
			{
				queue.push_back(OneFrame());
				queue.back().sourceSequence = static_cast<unsigned long long>(i + 1);
			}
			impl.queuedFutureFrames.store(ahead);
			impl.motionAheadDepth.store(depth);
			std::vector<FakeQueued> out;
			MotionAhead::CollectQueueAhead(&impl, queue, out);
			return out.size();
		}

	public:
		TEST_METHOD(LeadEqualsTheQueuedFramesByFramesAndByTimeAt24_30_60)
		{
			const double rates[] = { 24.0, 30.0, 60.0 };
			for (double hz : rates)
			{
				const int depth = MotionAhead::DepthAt(4, hz);
				// By time: a 200 ms queue holds ceil(200 ms / period) frames ahead (24p 5, 30p 6, 60p 12).
				const size_t byTime = static_cast<size_t>(std::ceil(200.0 * hz / 1000.0 - 1e-9));
				// By frames: a 6-frame queue, the current frame plus 5 ahead, at every rate.
				const size_t byFrames = 5;
				const size_t modes[] = { byTime, byFrames };
				for (size_t queued : modes)
				{
					FakeImpl impl;
					impl.motionProducer = std::make_shared<FakeProducer>();
					std::vector<FakeQueued> queue;
					const size_t copied = Collect(impl, queued, depth, queue);
					Assert::AreEqual(static_cast<int>(queued), impl.motionProducer->lead, L"lead == the queued frames ahead");
					Assert::AreEqual(queued + 1, copied, L"the current frame plus every frame ahead");
				}
			}
			const size_t expected[] = { 5, 6, 12 };
			for (int i = 0; i < 3; ++i)
				Assert::AreEqual(expected[i], static_cast<size_t>(std::ceil(200.0 * rates[i] / 1000.0 - 1e-9)));
		}

		TEST_METHOD(OneTwentyHzLeadAndTheDecideBound)
		{
			// 120p: DepthAt(4, 120) = 8 (66.7 ms). 12 queued ahead: the lead is 12 (above the depth). 6 queued ahead: Decide bounds the
			// depth to the 6 there (ShallowQueue) before the walk reads it, and the lead is those 6.
			const int depth = MotionAhead::DepthAt(4, 120.0);
			Assert::AreEqual(8, depth);
			const MotionAhead::Decision deep = MotionAhead::Decide(depth, false, true, 12);
			Assert::AreEqual(8, deep.depth);
			FakeImpl impl;
			impl.motionProducer = std::make_shared<FakeProducer>();
			std::vector<FakeQueued> queue;
			Assert::AreEqual<size_t>(13, Collect(impl, 12, deep.depth, queue));
			Assert::AreEqual(12, impl.motionProducer->lead);
			const MotionAhead::Decision shallow = MotionAhead::Decide(depth, false, true, 6);
			Assert::AreEqual(6, shallow.depth);
			Assert::IsTrue(shallow.reason == MotionAhead::Reason::ShallowQueue);
			Assert::AreEqual<size_t>(7, Collect(impl, 6, shallow.depth, queue));
			Assert::AreEqual(6, impl.motionProducer->lead);
			// The ring cap: depth 4 with 20 queued -> 13; 240p depth 16 -> 13 too (WAIVER-B1-01), 14 frames copied.
			Assert::AreEqual<size_t>(14, Collect(impl, 20, 4, queue));
			Assert::AreEqual(MotionAhead::kMaxLead, impl.motionProducer->lead);
			Assert::AreEqual<size_t>(14, Collect(impl, 20, MotionAhead::DepthAt(4, 240.0), queue));
			Assert::AreEqual(MotionAhead::kMaxLead, impl.motionProducer->lead);
		}

		TEST_METHOD(SixtyHzWithTheDepthQueuedIsUnchanged)
		{
			// 60p, 4 queued ahead (the old fixed depth): lead 4, 5 frames copied, the producer capacity 6 as before.
			FakeImpl impl;
			impl.motionProducer = std::make_shared<FakeProducer>();
			std::vector<FakeQueued> queue;
			Assert::AreEqual(4, MotionAhead::DepthAt(4, 60.0));
			Assert::AreEqual<size_t>(5, Collect(impl, 4, 4, queue));
			Assert::AreEqual(4, impl.motionProducer->lead);
			MotionAhead::Producer producer;
			producer.SetDepth(4);
			producer.SetLead(4);
			Assert::AreEqual<size_t>(6, producer.PendingCapacity());
		}

		TEST_METHOD(LeadNeverBelowTheDepthAndCappedByTheRing)
		{
			Assert::AreEqual(13, MotionAhead::kMaxLead);
			Assert::AreEqual(0, MotionAhead::LeadFor(0, 12), L"look-ahead off: no lead");
			Assert::AreEqual(2, MotionAhead::LeadFor(2, 1), L"a shallow queue keeps the depth (Decide bounds it before)");
			Assert::AreEqual(12, MotionAhead::LeadFor(4, 12));
			Assert::AreEqual(13, MotionAhead::LeadFor(2, 40), L"the P2 ring cap");
			Assert::AreEqual(16, MotionAhead::DepthAt(4, 240.0));
			Assert::AreEqual(13, MotionAhead::LeadFor(MotionAhead::DepthAt(4, 240.0), 20), L"240p: the ring cap holds even a larger depth");
			Assert::AreEqual(13, MotionAhead::LeadFor(MotionAhead::DepthAt(4, 240.0), 2), L"240p, short queue: still at most kMaxLead");
			FakeImpl impl;
			impl.motionProducer = std::make_shared<FakeProducer>();
			std::vector<FakeQueued> queue;
			Assert::AreEqual<size_t>(14, Collect(impl, 30, 2, queue), L"current + kMaxLead");
			Assert::AreEqual(13, impl.motionProducer->lead);
			// Depth 0 (off, low latency): the lead is handed back as 0 and nothing is copied.
			Assert::AreEqual<size_t>(0, Collect(impl, 30, 0, queue));
			Assert::AreEqual(0, impl.motionProducer->lead);
		}

		TEST_METHOD(ProducerCapacityFollowsTheLargerOfDepthAndLead)
		{
			MotionAhead::Producer producer;
			producer.SetDepth(2);
			Assert::AreEqual<size_t>(6, producer.PendingCapacity());
			producer.SetLead(12);   // 60p, 200 ms By time: 13 frames in one engage pass
			Assert::AreEqual<size_t>(MotionAhead::PendingJobsFor(12), producer.PendingCapacity());
			Assert::AreEqual<size_t>(14, producer.PendingCapacity());
			producer.SetLead(0);    // the look-ahead stopped: back to the depth's
			Assert::AreEqual<size_t>(6, producer.PendingCapacity());
		}
	};

	// Hardening (P5a-rest): in the renderer the producer's writer and the loop are one OS thread (or the writer runs
	// after the join); these two threads exercise the shared_ptr publish / copy pattern itself: a writer creates / stops /
	// resets the producer while a reader
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
