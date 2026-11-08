// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10-6 (doc 59 section 1.3, owner rule): queue depth is never added for vectors. Games mode uses its own queued
// frame for the one-ahead vectors and competitive mode is backward-only, so no vector demand, mode, cadence,
// look-ahead or credit may take the queue target above the configured (capacity-clamped) depth. The rule is pinned on
// the real pieces of PrefillTargetLocked: LockedFilmQueueTarget's floor (kLockedQueuePairFrames, the optical-flow
// look-ahead's MotionAhead::QueueFramesFor, the explicit active-picture look-ahead), Pulldown::LockedQueueTarget and
// the VP policy's QueueTarget (VpQueueRules::CombineQueueTarget), with the hold inactive (the delay budget's own
// target is time-based and capacity-clamped; checked separately). CPU only.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/input/queue_timing/PulldownDetector.h>
#include <VP-V/modules/input/queue_timing/QueueLookahead.h>
#include <VP-V/modules/input/queue_timing/VpFixedBudgetPolicy.h>
#include <VP-V/modules/motion/estimation/MotionAhead.h>

#include <algorithm>
#include <cstddef>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// LockedFilmQueueTarget's floor, as the renderer composes it (QueueDelayHold.inl / LibplaceboVideoRenderer.cpp).
		size_t FilmFloor(int motionDepth, size_t lookahead, bool automatic)
		{
			size_t minimum = Pulldown::kLockedQueuePairFrames;
			if (motionDepth > 0)
				minimum = (std::max)(minimum, MotionAhead::QueueFramesFor(motionDepth));
			return (std::max)(minimum, QueueLookahead::LockedFilmLookaheadFloor(lookahead, automatic));
		}

		struct Cadence { int cycle, unique; };
	}

	TEST_CLASS(QueueTargetNeverExceedsDesiredTests)
	{
	public:
		TEST_METHOD(NoVectorDemandModeOrCadenceRaisesTheTargetAboveTheConfiguredDepth)
		{
			// Cadences: unlocked (0/0), 3:2 (24 in 60), 2:2 (25 in 50, 30 in 60: unique == cycle / 2), 2:3:3:2-style
			// 5:2, 24 in 50 (25:12), and a cycle the detector reports without a repeat (unique == cycle: no scaling).
			const Cadence cadences[] = { { 0, 0 }, { 5, 2 }, { 2, 1 }, { 25, 12 }, { 4, 4 }, { 5, 4 } };
			uint64_t rows = 0, scaledBelow = 0;
			for (size_t capacity = 1; capacity <= 16; ++capacity)
				for (size_t configured = 1; configured <= 16; ++configured)
				{
					const size_t desired = AlphaQueuePolicy::ClampDesiredDepthToCapacity(configured, capacity);
					for (const Cadence& c : cadences)
						for (int motionDepth = 0; motionDepth <= 8; ++motionDepth)          // vector demand (any mode)
							for (size_t lookahead = 0; lookahead <= 8; ++lookahead)
								for (int automatic = 0; automatic < 2; ++automatic)
									for (size_t credit = 0; credit <= 4; ++credit)
										for (int fixes = 0; fixes < 2; ++fixes)
										{
											const size_t scaled = c.cycle == 0 ? desired : Pulldown::LockedQueueTarget(desired,
												Pulldown::FilmToSignalRatio(c.cycle, c.unique), FilmFloor(motionDepth, lookahead, automatic != 0));
											QueueTimingPolicy::TargetContext ctx;
											ctx.useFrameQueue = true;
											ctx.configuredDepth = configured;
											ctx.capacity = capacity;
											ctx.cadenceScaledDepth = scaled;
											ctx.renderAheadCreditFrames = credit;
											ctx.lookaheadFloor = FilmFloor(motionDepth, lookahead, automatic != 0);
											VpQueueTiming::Options options;
											options.queueTimingFixes = fixes != 0;
											const VpQueueTiming::VpFixedBudgetPolicy policy(options);
											const size_t target = policy.QueueTarget(ctx);
											if (scaled > desired || target > desired)
												Assert::Fail((L"target above the configured depth: desired " + std::to_wstring(desired) +
													L" scaled " + std::to_wstring(scaled) + L" target " + std::to_wstring(target) +
													L" motion " + std::to_wstring(motionDepth)).c_str());
											++rows;
											scaledBelow += scaled < desired ? 1 : 0;
										}
				}
			// Plausibility: the sweep is the full product, and the film lock does scale some targets down.
			Assert::AreEqual<uint64_t>(16ull * 16ull * 6ull * 9ull * 9ull * 2ull * 5ull * 2ull, rows);
			Assert::IsTrue(scaledBelow > 0);
		}

		TEST_METHOD(TheHoldTargetIsClampedToCapacityWhateverTheVectorDemand)
		{
			for (size_t capacity = 1; capacity <= 16; ++capacity)
				for (size_t holdFrames = 0; holdFrames <= 32; ++holdFrames)
					for (size_t floor = 0; floor <= 12; ++floor)
					{
						QueueTimingPolicy::TargetContext ctx;
						ctx.capacity = capacity;
						ctx.hold.active = true;
						ctx.hold.targetFrames = holdFrames;
						ctx.lookaheadFloor = floor;
						const size_t clamped = AlphaQueuePolicy::ClampDesiredDepthToCapacity(holdFrames, capacity);
						Assert::IsTrue(VpQueueTiming::VpFixedBudgetPolicy().QueueTarget(ctx) <= clamped,
							L"the look-ahead floor never raises a delay budget's target");
					}
		}
	};
}
