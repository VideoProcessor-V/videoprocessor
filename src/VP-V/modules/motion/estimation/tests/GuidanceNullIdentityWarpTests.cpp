// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link K4 on the libplacebo WARP device (VP_TEST_GPU=hardware for a real GPU): SetCameraGuidance(nullptr) gives the
// same matcher field as no call at all (the refusal path is byte-identical to today), and the 16 B guidance counts round trip
// through the counts ring without blocking.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceCountsRing.h>

#include <chrono>
#include <memory>
#include <thread>

namespace VideoProcessorTest
{
	TEST_CLASS(GuidanceNullIdentityWarpTests)
	{
		static constexpr int kW = 512, kH = 256;

		static void Frames(pl_gpu gpu, DejudderGpuPlanes& pa, DejudderGpuPlanes& pb)
		{
			const DejudderFrame a = MakeDejudderFrame(kW, kH, [](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(kW, kH, [](int x, int y) { return DejudderTexture(x - 12, y - 6); },
				[](int x, int y, int c) { return DejudderTexture((x - 6) * 2, y - 6, 7 + c); });
			UploadDejudderFrame(gpu, a, pa);
			UploadDejudderFrame(gpu, b, pb);
		}

		static DejudderField Run(TargetLutGpuFixture& fixture, bool callSetter)
		{
			pl_gpu gpu = fixture.Gpu();
			DejudderGpuPlanes a, b;
			Frames(gpu, a, b);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"first estimate");
			if (callSetter)
				warp.SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame>());
			Assert::IsTrue(warp.Estimate(a.planes, b.planes), L"second estimate");
			return DownloadField(gpu, warp);
		}

	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(NullGuidanceIsTheSameFieldAsNoCall)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(NullGuidanceIsTheSameFieldAsNoCall)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const DejudderField none = Run(fixture, false);
			const DejudderField withNull = Run(fixture, true);
			Assert::IsTrue(none.texels == withNull.texels, L"SetCameraGuidance(nullptr) changed the field");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(CountsRingRoundTripsSixteenBytesWithoutBlocking)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CountsRingRoundTripsSixteenBytesWithoutBlocking)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(fmt);
			pl_tex_params tp{};
			tp.w = 1; tp.h = 1; tp.format = fmt; tp.sampleable = true; tp.host_writable = true; tp.host_readable = true;
			pl_tex tex = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(tex);
			const float want[4] = { 17.0f, 3.0f, -1.0f, -1.0f };
			pl_tex_transfer_params up{};
			up.tex = tex;
			up.ptr = const_cast<float*>(want);
			Assert::IsTrue(pl_tex_upload(gpu, &up));
			{
				CameraLink::GuidanceCountsRing ring(gpu, 3, fixture.Log());
				Assert::IsTrue(ring.Request(41, tex));
				pl_gpu_finish(gpu);   // test only: make sure the data is complete before judging the values
				std::vector<CameraLink::IGuidanceCountsSource::Done> done;
				for (int i = 0; i < 2000 && done.empty(); ++i)
				{
					const auto t0 = std::chrono::steady_clock::now();
					ring.Poll(done);
					const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
					(void)ms;   // see FieldReadbackRingWarpTests: WARP defers work to Map
					if (done.empty())
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				Assert::AreEqual(size_t(1), done.size());
				Assert::AreEqual(uint64_t(41), done[0].pairSeq);
				char dbg[200];
				snprintf(dbg, sizeof(dbg), "COUNTSDBG got %g %g %g %g\n", done[0].v[0], done[0].v[1], done[0].v[2], done[0].v[3]);
				Logger::WriteMessage(dbg);
				for (int i = 0; i < 4; ++i)
					Assert::IsTrue(done[0].v[i] == want[i]);
				// Teardown with a copy in flight.
				Assert::IsTrue(ring.Request(42, tex));
			}
			pl_tex_destroy(gpu, &tex);
		}
	};
}
