// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/control/config/VpvProfileQueueAdvice.h>
#include <VP-V/modules/motion/estimation/MotionAhead.h>   // the expected depths (the advice header declares only)
#include <VP-V/modules/input/queue_timing/QueueLatency.h>
#include <ConfigFile.h>
#include <RendererProfileConfig.h>
#include <UnifiedProfileRuntime.h>

#include <fstream>
#include <map>
#include <string>
#include <windows.h>

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

	Vpv::ProfileQueueAdvice::Advice RunOn(const std::map<std::string, std::string>& values, double sourceHz, bool originalPathway)
	{
		return Vpv::ProfileQueueAdvice::Compute(
			[&](const char* key, const std::string& fallback) {
				const auto found = values.find(key);
				return found == values.end() ? fallback : found->second;
			},
			sourceHz, originalPathway);
	}

	// A configuration file in %TEMP%, removed afterwards.
	struct TempConfig
	{
		std::string path;
		explicit TempConfig(const char* text)
		{
			char directory[MAX_PATH]{}, file[MAX_PATH]{};
			Assert::IsTrue(GetTempPathA(MAX_PATH, directory) != 0);
			Assert::IsTrue(GetTempFileNameA(directory, "vpq", 0, file) != 0);
			path = file;
			std::ofstream(path, std::ios::trunc) << text;
		}
		~TempConfig() { DeleteFileA(path.c_str()); }
	};

	// GEM-032 (Gemini's test, config-schema MUST-2): target_frames 3, target_latency_ms 267, the look-ahead unset.
	const char* kQueueText =
		"[general]\nrenderer: VP Renderer\n"
		"[queue]\ntarget_frames: 3\ntarget_latency_ms: 267\nlow_latency_path: on\n";

	std::shared_ptr<const UnifiedProfileRuntime::Snapshot> SnapshotOf(const std::string& text)
	{
		TempConfig file(text.c_str());
		ConfigFile config;
		Assert::IsTrue(config.Load(file.path), L"the fixture loads");
		UnifiedProfileRuntime::Runtime runtime;
		std::string error;
		Assert::IsTrue(runtime.Initialize(config, {}, error), std::wstring(error.begin(), error.end()).c_str());
		const auto snapshot = runtime.GetSnapshot();
		Assert::IsTrue(static_cast<bool>(snapshot), L"no snapshot");
		Assert::IsFalse(snapshot->queue.profile.empty(), L"the [queue] profile is not selected");
		return snapshot;
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

		// GEM-034 (config-schema MUST-4): vector_demand off computes nothing ahead.
		TEST_METHOD(VectorDemandOffGivesZeroLookAhead)
		{
			const auto on = Run({ { "dejudder_strength", "0.5" } }, 24.0);
			Assert::IsTrue(on.lookAheadQueueFrames > 0, L"the control case has a look-ahead");
			const auto off = Run({ { "dejudder_strength", "0.5" }, { "vector_demand", "off" } }, 24.0);
			Assert::AreEqual(size_t(0), off.lookAheadQueueFrames, L"vector_demand off still asks for a look-ahead");
			Assert::IsTrue(off.generalRateFixes, L"vector_demand does not switch general_rate_fixes");
			Assert::AreEqual(on.lookAheadQueueFrames,
				Run({ { "dejudder_strength", "0.5" }, { "vector_demand", "on" } }, 24.0).lookAheadQueueFrames, L"vector_demand on changed the depth");
		}

		// GEM-034: the Original pathway is Bill's queue: no look-ahead, no general-rate fixes, whatever the profile says.
		TEST_METHOD(OriginalPathwayDisablesLookAheadAndGeneralRateFixes)
		{
			for (const double hz : { 0.0, 24.0, 120.0 })
			{
				const auto advice = RunOn({ { "dejudder_strength", "1" }, { "interpolation", "smooth" }, { "general_rate_fixes", "on" } }, hz, true);
				Assert::AreEqual(size_t(0), advice.lookAheadQueueFrames, L"a look-ahead on the Original pathway");
				Assert::IsFalse(advice.generalRateFixes, L"general-rate fixes on the Original pathway");
			}
			const auto vp = RunOn({ { "dejudder_strength", "1" } }, 24.0, false);
			Assert::IsTrue(vp.lookAheadQueueFrames > 0 && vp.generalRateFixes, L"the VP pathway lost its look-ahead");
		}

		// GEM-032 (Gemini's test): through the real snapshot builder, the Original pathway resolves to Bill's
		// queue: 3 frames (target_latency_ms ignored), look-ahead 0 (not Auto), no low-latency path, no rate fixes.
		TEST_METHOD(OriginalPathwaySnapshotIsBillsQueue)
		{
			const auto original = SnapshotOf(std::string(kQueueText) + "[vpv]\npipeline_original_pathway: on\n");
			const auto& q = original->queue;
			Assert::IsTrue(q.hasTargetFrames && q.targetFrames == 3, L"Bill's target_frames changed");
			Assert::IsTrue(q.hasTargetLatencyMs && q.targetLatencyMs == 0, L"target_latency_ms reaches the Original pathway");
			Assert::IsFalse(q.lowLatencyPath, L"the low-latency path reaches the Original pathway");
			Assert::IsTrue(q.hasActivePictureLookaheadFrames && q.activePictureLookaheadFrames == 0, L"the unset look-ahead is not Bill's 0");
			Assert::AreEqual(size_t(0), original->lookAheadQueueFrames, L"motion look-ahead depth on the Original pathway");
			Assert::IsFalse(original->generalRateFixes, L"general-rate fixes on the Original pathway");
			// The host's resolve (VideoProcessorDlg) on these values: Bill's 3 frames at every rate, base latency ignored.
			for (const double hz : { 23.976, 60.0 })
			{
				const QueueLatency::TargetDecision d = QueueLatency::ResolveTargetFrames(q.hasTargetLatencyMs, q.targetLatencyMs,
					q.hasTargetFrames, q.targetFrames, 267, 5, hz, original->generalRateFixes);
				Assert::AreEqual(size_t(3), d.frames, L"the Original pathway's target is not Bill's 3 frames");
				Assert::IsFalse(d.fromLatency, L"the Original pathway's target came from a time");
			}
		}

		// The VP pathway keeps every VP queue key as configured (the neutralising is Original-only).
		TEST_METHOD(VpPathwaySnapshotKeepsTheVpQueueKeys)
		{
			for (const char* pathway : { "", "[vpv]\npipeline_original_pathway: off\n" })
			{
				const auto vp = SnapshotOf(std::string(kQueueText) + pathway);
				const auto& q = vp->queue;
				Assert::IsTrue(q.hasTargetLatencyMs && q.targetLatencyMs == 267, L"target_latency_ms lost on the VP pathway");
				Assert::IsTrue(q.lowLatencyPath, L"low_latency_path lost on the VP pathway");
				Assert::IsFalse(q.hasActivePictureLookaheadFrames, L"the VP pathway's Auto look-ahead was replaced");
				Assert::IsTrue(vp->generalRateFixes, L"general-rate fixes lost on the VP pathway");
			}
		}
	};
}
