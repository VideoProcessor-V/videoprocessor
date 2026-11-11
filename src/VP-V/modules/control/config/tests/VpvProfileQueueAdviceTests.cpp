// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/control/config/VpvProfileQueueAdvice.h>

#include <map>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// P14: the look-ahead queue rule moved out of UnifiedProfileRuntime.cpp (same rule, same defaults).
namespace
{
	Vpv::ProfileQueueAdvice::Advice Run(const std::map<std::string, std::string>& values, double sourceHz)
	{
		return Vpv::ProfileQueueAdvice::Compute(
			[&](const char* key, const std::string& fallback) {
				const auto found = values.find(key);
				return found == values.end() ? fallback : found->second;
			},
			sourceHz);
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(VpvProfileQueueAdviceTests)
	{
	public:
		TEST_METHOD(NoConsumerMeansNoLookAheadAndGeneralFixesDefaultOn)
		{
			const auto advice = Run({}, 24.0);
			Assert::AreEqual(size_t(0), advice.lookAheadQueueFrames, L"no de-judder, no interpolation: no look-ahead");
			Assert::IsTrue(advice.generalRateFixes, L"general_rate_fixes defaults on");
		}

		TEST_METHOD(FlowOffOrAheadZeroOrDejudderZeroGiveZero)
		{
			Assert::AreEqual(size_t(0), Run({ { "optical_flow", "off" }, { "dejudder_strength", "0.5" } }, 24.0).lookAheadQueueFrames);
			Assert::AreEqual(size_t(0), Run({ { "motion_ahead_frames", "0" }, { "dejudder_strength", "0.5" } }, 24.0).lookAheadQueueFrames);
			Assert::AreEqual(size_t(0), Run({ { "dejudder_strength", "0" }, { "dejudder_strength_even", "0" } }, 24.0).lookAheadQueueFrames);
		}

		TEST_METHOD(DejudderOrInterpolationUsesTheEngineDepth)
		{
			for (const double hz : { 0.0, 24.0, 60.0, 120.0 })
			{
				const size_t expected = MotionAhead::QueueFramesFor(
					MotionAhead::DepthAt(MotionAhead::kDefaultDepth, hz, true));
				Assert::AreEqual(expected, Run({ { "dejudder_strength", "0.5" } }, hz).lookAheadQueueFrames);
				Assert::AreEqual(expected, Run({ { "dejudder_strength_even", "0.2" } }, hz).lookAheadQueueFrames);
				Assert::AreEqual(expected, Run({ { "interpolation", "smooth" } }, hz).lookAheadQueueFrames);
			}
		}

		TEST_METHOD(GeneralRateFixesOffUsesTheConfiguredDepth)
		{
			const auto advice = Run({ { "dejudder_strength", "1" }, { "general_rate_fixes", "off" } }, 120.0);
			Assert::IsFalse(advice.generalRateFixes);
			Assert::AreEqual(MotionAhead::QueueFramesFor(MotionAhead::kDefaultDepth), advice.lookAheadQueueFrames);
		}
	};
}
