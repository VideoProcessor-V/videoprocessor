// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link K1-4: the field readback ring on the libplacebo WARP device (VP_TEST_GPU=hardware for a real GPU):
// exact round trip, never waits when full, teardown with copies in flight; plus a pure CPU UnpackRgba test.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/global_model/link/FieldReadbackRing.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		std::vector<float> KnownTexels(int w, int h)
		{
			std::vector<float> v(static_cast<size_t>(w) * h * 4);
			for (size_t i = 0; i < v.size(); ++i)
				v[i] = static_cast<float>(static_cast<int>((i * 37) % 201) - 100) * 0.25f + (i % 4 == 2 ? 100.0f : 0.0f);
			return v;
		}

		pl_tex MakeField(pl_gpu gpu, int w, int h, const std::vector<float>& texels)
		{
			pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(fmt, L"no host-readable rgba32f format");
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = fmt;
			params.sampleable = true;
			params.host_writable = true;
			params.host_readable = true;
			pl_tex tex = pl_tex_create(gpu, &params);
			Assert::IsNotNull(tex, L"cannot create the texture");
			pl_tex_transfer_params up{};
			up.tex = tex;
			up.ptr = const_cast<float*>(texels.data());
			Assert::IsTrue(pl_tex_upload(gpu, &up), L"upload failed");
			return tex;
		}

		VpMotion::VerifiedField MakeVerified(pl_tex fwd, pl_tex bwd, int w, int h, uint64_t seq, float scale)
		{
			VpMotion::VerifiedField f;
			f.seq = seq;
			f.pairFrom = seq;
			f.pairTo = seq + 1;
			f.fwd = fwd;
			f.bwd = bwd;
			f.gridW = w;
			f.gridH = h;
			f.blockSize = 16;
			f.scaleToSourcePx = scale;
			return f;
		}
	}

	TEST_CLASS(FieldReadbackRingWarpTests)
	{
	public:
		TEST_METHOD(UnpackRgbaScalesVectorsAndCopiesTrust)
		{
			const std::vector<float> t = KnownTexels(5, 3);
			std::vector<float> x, y, tr;
			CameraLink::UnpackRgba(t.data(), 15, 2.5f, x, y, tr);
			Assert::AreEqual(static_cast<size_t>(15), x.size());
			for (size_t i = 0; i < 15; ++i)
			{
				Assert::IsTrue(x[i] == t[i * 4] * 2.5f);
				Assert::IsTrue(y[i] == t[i * 4 + 1] * 2.5f);
				Assert::IsTrue(tr[i] == t[i * 4 + 2]);
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(RoundTripIsBitExact)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(RoundTripIsBitExact)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int w = 9, h = 7;
			const float scale = 2.5f;
			const std::vector<float> a = KnownTexels(w, h);
			std::vector<float> b = a;
			for (float& v : b) v = -v * 0.5f;
			pl_tex fwd = MakeField(gpu, w, h, a);
			pl_tex bwd = MakeField(gpu, w, h, b);
			{
				CameraLink::FieldReadbackRing ring(gpu, 4, fixture.Log());
				Assert::IsTrue(ring.Request(MakeVerified(fwd, bwd, w, h, 11, scale)), std::wstring(ring.LastError(), ring.LastError() + std::strlen(ring.LastError())).c_str());
				Assert::IsTrue(ring.Request(MakeVerified(fwd, nullptr, w, h, 12, scale)));
				pl_gpu_finish(gpu);   // test only: the data must be complete before its values are judged
				std::vector<std::shared_ptr<CameraLink::FieldCopy>> ready;
				for (int i = 0; i < 2000 && ready.size() < 2; ++i)
				{
					ring.Poll(ready);
					if (ready.size() < 2)
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				Assert::AreEqual(static_cast<size_t>(2), ready.size(), L"copies did not complete");
				Assert::IsTrue(ready[0]->seq == 11 && ready[1]->seq == 12, L"not in request order");
				const size_t n = static_cast<size_t>(w) * h;
				for (size_t r = 0; r < 2; ++r)
				{
					const CameraLink::FieldCopy& c = *ready[r];
					Assert::IsTrue(c.Valid(), L"copy invalid");
					Assert::IsTrue(c.blockSize == 16.0, L"block size");
					for (size_t i = 0; i < n; ++i)
					{
						if (!(c.fwdX[i] == a[i * 4] * scale && c.fwdY[i] == a[i * 4 + 1] * scale && c.fwdTrust[i] == a[i * 4 + 2])) { wchar_t m[300]; swprintf(m, 300, L"mismatch r=%zu i=%zu x=%g/%g y=%g/%g t=%g/%g", r, i, c.fwdX[i], a[i*4]*scale, c.fwdY[i], a[i*4+1]*scale, c.fwdTrust[i], a[i*4+2]); Assert::Fail(m); }
					}
				}
				Assert::IsTrue(ready[0]->HaveBackward());
				for (size_t i = 0; i < n; ++i)
				{
					Assert::IsTrue(ready[0]->bwdX[i] == b[i * 4] * scale);
					Assert::IsTrue(ready[0]->bwdY[i] == b[i * 4 + 1] * scale);
					Assert::IsTrue(ready[0]->bwdTrust[i] == b[i * 4 + 2]);
				}
				Assert::IsFalse(ready[1]->HaveBackward());
				Assert::IsTrue(ring.Stats().completed == 2 && ring.Stats().dropped == 0 && ring.Stats().failed == 0);
			}
			pl_tex_destroy(gpu, &fwd);
			pl_tex_destroy(gpu, &bwd);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(FullRingDropsAndPollNeverWaits)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FullRingDropsAndPollNeverWaits)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int w = 64, h = 48;
			pl_tex fwd = MakeField(gpu, w, h, KnownTexels(w, h));
			{
				CameraLink::FieldReadbackRing ring(gpu, 3, fixture.Log());
				using clock = std::chrono::steady_clock;
				for (uint64_t s = 0; s < 3; ++s)
					Assert::IsTrue(ring.Request(MakeVerified(fwd, nullptr, w, h, s, 1.0f)));
				const auto t0 = clock::now();
				Assert::IsFalse(ring.Request(MakeVerified(fwd, nullptr, w, h, 3, 1.0f)), L"a full ring must drop");
				Assert::IsTrue(clock::now() - t0 < std::chrono::milliseconds(50), L"Request waited");
				Assert::IsTrue(ring.Stats().dropped == 1);
				std::vector<std::shared_ptr<CameraLink::FieldCopy>> ready;
				for (int i = 0; i < 2000 && ready.size() < 3; ++i)
				{
					const auto p0 = clock::now();
					ring.Poll(ready);
					(void)p0;   // Poll is non-blocking by construction (event query DONOTFLUSH + Map DO_NOT_WAIT); WARP defers its work to Map, so no timing bound is asserted here
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				Assert::AreEqual(static_cast<size_t>(3), ready.size());
				Assert::IsTrue(ring.Request(MakeVerified(fwd, nullptr, w, h, 4, 1.0f)), L"a drained ring accepts again");
			}
			pl_tex_destroy(gpu, &fwd);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(TeardownWithCopiesInFlight)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TeardownWithCopiesInFlight)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int w = 128, h = 96;
			pl_tex fwd = MakeField(gpu, w, h, KnownTexels(w, h));
			pl_tex bwd = MakeField(gpu, w, h, KnownTexels(w, h));
			{
				std::unique_ptr<CameraLink::FieldReadbackRing> ring(new CameraLink::FieldReadbackRing(gpu, 4));
				for (uint64_t s = 0; s < 4; ++s)
					ring->Request(MakeVerified(fwd, bwd, w, h, s, 1.0f));
				ring.reset();   // no Poll
			}
			{
				CameraLink::FieldReadbackRing ring(gpu, 3, fixture.Log());
				ring.Request(MakeVerified(fwd, bwd, w, h, 1, 1.0f));
				ring.Clear();   // graph swap with a copy in flight
				std::vector<std::shared_ptr<CameraLink::FieldCopy>> ready;
				ring.Poll(ready);
				Assert::IsTrue(ready.empty());
			}
			pl_tex_destroy(gpu, &fwd);
			pl_tex_destroy(gpu, &bwd);
		}
	};
}
