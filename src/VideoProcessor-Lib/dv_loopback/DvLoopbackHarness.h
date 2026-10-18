#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// DV tunnel loopback (2026-10-17): the harness. Sends a known pattern +
// metadata sequence (DvLoopbackPattern.h) through an output, receives it back
// through VP's capture -> tunnel decode -> presentation decision
// (DvLoopbackAnalyzer.h), and reports what came back.
//
// RunSimulatedLoop: the in-memory loop (DvSimulatedLoop.h), deterministic,
// used by the CPU tests now. The hardware loop runs the same pattern and the
// same analyzer in two processes: the VPV sender (src/VideoProcessor-
// DvLoopback) logs what it sent and when, VP records the capture
// (RawCaptureRecorder.h), and AnalyzeRecordedLoop() replays the recording
// (DvLoopbackCorpus.h) - see docs/VP-dv-tunnel-autotest.md.
#include "DvLoopbackAnalyzer.h"
#include "DvSimulatedLoop.h"
#include <cmath>
#include <functional>
#include <map>
#include <memory>

namespace DvLoopback
{
	struct HarnessConfig
	{
		SequenceConfig sequence;
		uint32_t frames = 48;
		unsigned rateNumerator = 24000, rateDenominator = 1001;
		LoopFaults faults;
		// A fault in the sender's packer (applied to every sent picture, and
		// to the record of what was sent, as a real packer bug would be).
		std::function<void(LoopFrame&)> packerFault;
		AnalyzerOptions analyzer;   // sequenceId / expected / sentQpc are filled in
		bool cacheFrames = true;
	};

	constexpr int64_t kVirtualTicksPerSecond = 10000000;   // 100 ns

	inline LoopReport RunSimulatedLoop(const HarnessConfig& config)
	{
		const PatternSequence sequence(config.sequence);
		auto cache = std::make_shared<std::map<uint32_t, LoopFrame>>();
		auto make = [&config, &sequence, cache](uint32_t index, LoopFrame& frame) -> bool
		{
			if (index >= config.frames)
				return false;
			if (config.cacheFrames)
			{
				const auto it = cache->find(index);
				if (it != cache->end())
				{
					frame = it->second;
					return true;
				}
			}
			frame = sequence.Make(index);
			if (config.packerFault)
				config.packerFault(frame);
			if (config.cacheFrames)
				(*cache)[index] = frame;
			return true;
		};
		const double periodTicks = static_cast<double>(kVirtualTicksPerSecond) *
			config.rateDenominator / config.rateNumerator;
		AnalyzerOptions options = config.analyzer;
		options.sequenceId = config.sequence.sequenceId;
		options.expected = make;
		options.sentQpc = [periodTicks](uint32_t index) {
			// +1: tick 0 means "unknown" to the analyzer.
			return static_cast<int64_t>(std::llround(periodTicks * index)) + 1; };
		options.qpcFrequency = kVirtualTicksPerSecond;
		DvLoopbackAnalyzer analyzer(options);
		DvSimulatedLoop loop(analyzer, config.faults);
		loop.SetPeriodTicks(static_cast<uint64_t>(std::llround(periodTicks)));
		loop.Begin(config.sequence.width, config.sequence.height, config.rateNumerator, config.rateDenominator);
		for (uint32_t index = 0; index < config.frames; ++index)
		{
			LoopFrame frame;
			make(index, frame);
			loop.Send(frame, options.sentQpc(index));
		}
		loop.End();
		return analyzer.Finish();
	}
}
