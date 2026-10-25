// GPU / timing tests for the motion + queue/timing extraction (2026-10-24 draft). WRITTEN, NOT RUN:
// they need the GPU (VP_TEST_ADAPTER=NVIDIA on the laptop) and, for the renderer-level ones, the
// 2.5 h skeleton timing gates. To run next session: #include this file from
// src/VideoProcessor-BetaTests/BetaGpuTests.cpp after DrainedMs() and call RunMotionTimingGpuTests
// from its test list. Uses only BetaGpuTests' existing helpers (Gpu, Check, DrainedMs, ChainGpuProbe)
// and MotionWarp's SetPassTiming(on, drain=false) + PollPassTimers()/PassGpuMs() (PollPassTimers: efficiency commit; PassGpuMs already exists).
//
// Expectations (stated before running):
//  G1 undrained per-pass GPU times agree with the drained ones within 15% per pass >= 0.2 ms and
//     within 10% in total (same passes, no serialisation gaps; a laptop GPU may clock lower in the
//     drained gaps, so drained >= undrained is expected, not the reverse beyond 10%).
//  G2 estimate wall-clock with undrained timers armed 1 frame in 30 is within 3% of timers off
//     (the drained mode today costs ~40 pl_gpu_finish round trips per estimate: expected 1.5-4 ms
//     on the laptop at 4K, measured by G2's drained column for the record).
//  G3 the S0 segment's delivery is pixel-identical to the host hand-offs: an extra rendered with
//     the field fetched from MotionSegment equals the extra rendered from dejudder->Field() (byte
//     identity, the capture harness) - run as part of the renderer byte-identity gate after the
//     motion call-site patch, not here.
//
// Renderer-level gates (existing harnesses, after applying docs/design/motion-timing-callsites.patch):
//  T1 strict absolute delay gate (DelayGateTests + renderer delay harness) at 4/3/2 frames,
//     23.976/24/25/29.97/30/50/59.94/60: totals identical to d0212534 (+-0 ms in the model; live
//     within the gate's 25 ms tolerance and the same spread +-1 ms).
//  T2 byte identity vs r6, B3 off and on: identical (the policy move changes no decision).
//  T3 render-ahead open-loop run (65 extras reference) unchanged; held-shed counts unchanged on the
//     60p-on-60 Hz case (gate 7) and the start-up stall case (gate 5).

namespace MotionTimingGpu
{
	inline void UndrainedTimersAgreeWithDrained(Gpu& gpu, pl_gpu g, const Dejudder::Planes& a, const Dejudder::Planes& b)
	{
		Dejudder::MotionWarp drained(gpu.Log(), g, ChainGpuProbe::Refined(Dejudder::Settings{}));
		Dejudder::MotionWarp undrained(gpu.Log(), g, ChainGpuProbe::Refined(Dejudder::Settings{}));
		const int reps = 30;
		int slot = 0;
		drained.SetPassTiming(true, true);
		for (int i = 0; i < reps; ++i) { Check(drained.Estimate(a, b, slot, true), "G1 drained estimate"); slot ^= 1; }
		drained.FlushPassTimers();
		undrained.SetPassTiming(true, false);
		for (int i = 0; i < reps; ++i)
		{
			Check(undrained.Estimate(a, b, slot, true), "G1 undrained estimate");
			undrained.PollPassTimers();   // never pl_gpu_finish
			slot ^= 1;
		}
		pl_gpu_finish(g);   // the test itself may wait; production never does
		undrained.PollPassTimers();
		double totalDrained = 0.0, totalUndrained = 0.0;
		for (const auto& entry : drained.PassGpuMs())
		{
			const auto it = undrained.PassGpuMs().find(entry.first);
			Check(it != undrained.PassGpuMs().end(), "G1 pass missing undrained: " + entry.first);
			totalDrained += entry.second;
			totalUndrained += it->second;
			if (entry.second / reps >= 0.2)
				Check(std::fabs(it->second - entry.second) <= 0.15 * entry.second, "G1 pass differs >15%: " + entry.first);
		}
		Check(totalUndrained <= 1.10 * totalDrained && totalUndrained >= 0.80 * totalDrained, "G1 total outside bounds");
		std::cout << "  G1 motion passes: drained " << totalDrained / reps << " ms, undrained " << totalUndrained / reps << " ms per estimate\n";
	}

	inline void SampledTimersCostNothing(Gpu& gpu, pl_gpu g, const Dejudder::Planes& a, const Dejudder::Planes& b)
	{
		Dejudder::MotionWarp warp(gpu.Log(), g, ChainGpuProbe::Refined(Dejudder::Settings{}));
		int slot = 0, frame = 0;
		const double offMs = DrainedMs(g, 60, [&] { Check(warp.Estimate(a, b, slot, true), "G2 off"); slot ^= 1; });
		const double sampledMs = DrainedMs(g, 60, [&] {
			warp.SetPassTiming((frame++ % 30) == 0, false);
			Check(warp.Estimate(a, b, slot, true), "G2 sampled");
			warp.PollPassTimers();
			slot ^= 1;
		});
		warp.SetPassTiming(true, true);
		const double drainedMs = DrainedMs(g, 10, [&] { Check(warp.Estimate(a, b, slot, true), "G2 drained"); slot ^= 1; });
		warp.SetPassTiming(false);
		std::cout << "  G2 estimate: timers off " << offMs << " ms, sampled 1/30 undrained " << sampledMs
			<< " ms, drained every pass " << drainedMs << " ms (the stall the sampled mode removes)\n";
		Check(sampledMs <= 1.03 * offMs, "G2 sampled undrained timers cost more than 3%");
	}
}
