// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// O#6 (SRM-FRAMEBOUND-WAIT): Red Team adversarial WARP GPU test for SRM frame-bound deterministic
// ready wait vs live-mode zero-wait guarantee over real GPU FrameBegin / FrameSubmit + AutoFactors.

#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	TEST_CLASS(SourceResolutionFrameBoundWaitWarpTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(DeterministicStillRunOnWarpHasZeroWaitsMissedWhileLiveNeverWaits)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DeterministicStillRunOnWarpHasZeroWaitsMissedWhileLiveNeverWaits)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch, L"Could not create pl_dispatch on WARP");

			constexpr int kW = 1280;
			constexpr int kH = 720;
			std::mt19937 rng(20260602u);
			std::normal_distribution<double> dist(0.50, 0.14);
			std::vector<uint16_t> luma(static_cast<size_t>(kW) * kH);
			for (uint16_t& px : luma)
			{
				const double v = (std::min)(940.0 / 1023.0, (std::max)(64.0 / 1023.0, dist(rng)));
				px = static_cast<uint16_t>(static_cast<int>(std::lround(v * 1023.0)) << 6);
			}

			pl_tex_params tp{};
			tp.w = kW;
			tp.h = kH;
			tp.format = pl_find_named_fmt(gpu, "r16");
			tp.sampleable = true;
			tp.initial_data = luma.data();
			pl_tex tex = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(tex, L"Could not upload synthetic still luma texture on WARP");

			pl_frame frame{};
			frame.num_planes = 1;
			frame.planes[0].texture = tex;
			frame.planes[0].components = 1;
			frame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			frame.repr.sys = PL_COLOR_SYSTEM_BT_709;
			frame.repr.levels = PL_COLOR_LEVELS_LIMITED;
			frame.repr.bits.sample_depth = 16;
			frame.repr.bits.color_depth = 10;
			frame.repr.bits.bit_shift = 6;
			frame.color = pl_color_space_bt709;
			frame.crop = { 0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH) };
			Assert::IsTrue(SourceResolutionMonitor::Monitor::NotReadReason(frame).empty(),
				L"Synthetic BT.709 10-bit still frame must be readable by SRM");

			// Deterministic still run across real WARP GPU FrameBegin / AutoFactors / FrameSubmit.
			{
				SourceResolutionMonitor::Monitor det;
				det.SetFrameBound(true, 8);
				det.SetDeterministicWait(true, 500000);
				std::string why;
				double fx = 1.0, fy = 1.0;
				for (uint64_t seq = 1; seq <= 32; ++seq)
				{
					const double t = static_cast<double>(seq) * 0.0625; // exact 1/16 s -> 1 copy every 8 frames (0.5 s)
					det.FrameBegin(frame, seq, t);
					det.AutoFactors(kW, kH, 0.5, why, fx, fy, t, seq);
					det.FrameSubmit(gpu, dispatch, frame, seq);
					pl_gpu_finish(gpu);
				}

				const SourceResolutionMonitor::Snapshot s = det.Read();
				Assert::AreEqual<unsigned long long>(0, det.WaitsMissed(),
					L"Deterministic still run on WARP must report 0 waits missed");
				Assert::AreEqual<unsigned long long>(0, det.SubmitSkipped(),
					L"Deterministic still run on WARP must skip 0 tile copies");
				Assert::IsTrue(det.Reproducible(),
					L"Deterministic still run on WARP must be reproducible");
				Assert::IsTrue(s.reading,
					L"Deterministic still run on WARP must produce a reading after startup");
				Assert::IsTrue(s.frames >= 2,
					L"Deterministic still run on WARP must release analysed frames");
				Assert::IsTrue(det.DeterministicWaitCalls() > 0);
				det.Release(gpu);
			}

			// Negative control on the EXACT same WARP GPU fixture & still frame in live mode (SetDeterministicWait(false)):
			// live mode MUST NEVER wait/yield on present.
			{
				SourceResolutionMonitor::Monitor live;
				live.SetFrameBound(true, 8);
				live.SetDeterministicWait(false);
				std::string why;
				double fx = 1.0, fy = 1.0;
				for (uint64_t seq = 1; seq <= 32; ++seq)
				{
					const double t = static_cast<double>(seq) * 0.0625;
					live.FrameBegin(frame, seq, t);
					live.AutoFactors(kW, kH, 0.5, why, fx, fy, t, seq);
					live.FrameSubmit(gpu, dispatch, frame, seq);
					pl_gpu_finish(gpu);
				}

				Assert::AreEqual<unsigned long long>(0, live.DeterministicWaitCalls(),
					L"Live mode on WARP must never invoke deterministic wait");
				Assert::AreEqual<unsigned long long>(0, live.DeterministicWaitYields(),
					L"Live mode on WARP must report 0 wait yields on present");
				live.Release(gpu);
			}

			pl_tex_destroy(gpu, &tex);
			pl_dispatch_destroy(&dispatch);
		}
	};
}
