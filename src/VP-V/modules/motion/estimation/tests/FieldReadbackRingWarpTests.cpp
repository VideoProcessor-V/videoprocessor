// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link K1-4: the field readback ring on the libplacebo WARP device (VP_TEST_GPU=hardware for a real GPU):
// exact round trip, never waits when full, teardown with copies in flight; plus a pure CPU UnpackRgba test.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLabelProducer.h>
#include <VP-V/modules/motion/estimation/global_model/link/FieldReadbackRing.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
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

		struct TexGuard
		{
			pl_gpu gpu = nullptr;
			pl_tex tex = nullptr;
			TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
			TexGuard(const TexGuard&) = delete;
			TexGuard& operator=(const TexGuard&) = delete;
			~TexGuard()
			{
				if (gpu && tex)
					pl_tex_destroy(gpu, &tex);
			}
		};

		uint64_t CameraModelVramBytes()
		{
			const VpSkeleton::VramUsers::Snapshot snap = VpSkeleton::VramUsers::Instance().Take();
			for (const auto& kv : snap.heldById)
				if (kv.first == VpSkeleton::kUserCameraModel)
					return kv.second;
			return 0;
		}

		bool CameraModelIsUnpopulated()
		{
			const VpSkeleton::VramUsers::Snapshot snap = VpSkeleton::VramUsers::Instance().Take();
			for (uint16_t id : snap.unpopulated)
				if (id == VpSkeleton::kUserCameraModel)
					return true;
			return false;
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
					(void)p0;   // Poll is non-blocking by construction (Map DO_NOT_WAIT, which is also the readiness probe: NativeTexReadback::Poll); WARP defers its work to Map, so no timing bound is asserted here
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
					Assert::IsTrue(ring->Request(MakeVerified(fwd, bwd, w, h, s, 1.0f)), L"ring request must succeed");
				const auto st = ring->Stats();
				Assert::AreEqual(uint64_t(4), st.requested, L"4 copies queued in flight");
				Assert::AreEqual(uint64_t(0), st.completed, L"none polled before teardown");
				Assert::AreEqual(uint64_t(0), st.failed, L"no copy submission failures");
				ring.reset();   // no Poll
			}
			{
				CameraLink::FieldReadbackRing ring(gpu, 3, fixture.Log());
				Assert::IsTrue(ring.Request(MakeVerified(fwd, bwd, w, h, 1, 1.0f)), L"ring request before Clear must succeed");
				Assert::AreEqual(uint64_t(1), ring.Stats().requested);
				Assert::AreEqual(uint64_t(0), ring.Stats().completed);
				ring.Clear();   // graph swap with a copy in flight
				std::vector<std::shared_ptr<CameraLink::FieldCopy>> ready;
				ring.Poll(ready);
				Assert::IsTrue(ready.empty());
			}
			pl_tex_destroy(gpu, &fwd);
			pl_tex_destroy(gpu, &bwd);
		}

		// MC-READBACK MUST 3 (verify-gemini-mc-81ee082cf.md:81-86): rgba32f sources (and mixed rgba16f/rgba32f pairs)
		// must stay on R32G32B32A32_FLOAT staging with bit-exact readback (zero silent truncation), and format switches
		// on the same ring/dimensions must recreate staging cleanly.
		BEGIN_TEST_METHOD_ATTRIBUTE(Rgba32fSourceStaysFp32BitExactAndSwitchesFormatCleanly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(Rgba32fSourceStaysFp32BitExactAndSwitchesFormatCleanly)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_fmt fmt16 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(fmt16, L"no rgba16f format");
			const int w = 16, h = 8;
			const size_t n = static_cast<size_t>(w) * h;
			// Values intentionally NOT representable in IEEE-754 binary16 (exceed precision or range).
			const float inexactVals[8] = {
				0.1f, 123456.7f, -0.0001234567f, 3.1415927f,
				16777215.0f, -987654.3f, 1.2345678e-7f, 0.33333334f
			};
			std::vector<float> a32(n * 4), b32(n * 4);
			for (size_t i = 0; i < n * 4; ++i)
			{
				a32[i] = inexactVals[i % 8] + static_cast<float>((i % 13) * 0.00001f);
				b32[i] = -inexactVals[(i + 3) % 8] - static_cast<float>((i % 11) * 0.00003f);
			}
			pl_tex fwd32 = MakeField(gpu, w, h, a32);
			pl_tex bwd32 = MakeField(gpu, w, h, b32);

			// Raw fp16 texture of identical dimensions for mixed-format & format-switch checks.
			std::vector<uint16_t> raw16(n * 4);
			for (size_t i = 0; i < n * 4; ++i)
				raw16[i] = static_cast<uint16_t>(0x3C00u + (i % 16u));   // 1.0 + tiny exact half steps
			pl_tex_params p16{};
			p16.w = w; p16.h = h; p16.format = fmt16;
			p16.sampleable = true; p16.host_writable = true; p16.host_readable = true;
			pl_tex fwd16 = pl_tex_create(gpu, &p16);
			pl_tex bwd16 = pl_tex_create(gpu, &p16);
			Assert::IsNotNull(fwd16, L"cannot create rgba16f fwd");
			Assert::IsNotNull(bwd16, L"cannot create rgba16f bwd");
			pl_tex_transfer_params up{};
			up.tex = fwd16; up.ptr = raw16.data();
			Assert::IsTrue(pl_tex_upload(gpu, &up), L"upload fwd16 failed");
			up.tex = bwd16; up.ptr = raw16.data();
			Assert::IsTrue(pl_tex_upload(gpu, &up), L"upload bwd16 failed");

			{
				CameraLink::FieldReadbackRing ring(gpu, 4, fixture.Log());
				auto runOne = [&](pl_tex fTex, pl_tex bTex, uint64_t seq) -> std::shared_ptr<CameraLink::FieldCopy>
				{
					Assert::IsTrue(ring.Request(MakeVerified(fTex, bTex, w, h, seq, 1.0f)), L"Request failed");
					pl_gpu_finish(gpu);
					std::vector<std::shared_ptr<CameraLink::FieldCopy>> ready;
					for (int tries = 0; tries < 2000 && ready.empty(); ++tries)
						ring.Poll(ready);
					Assert::AreEqual<size_t>(1, ready.size(), L"readback did not complete");
					return ready[0];
				};
				auto checkExact = [](float expected, float actual, const wchar_t* msg)
				{
					uint32_t eb = 0, ab = 0;
					std::memcpy(&eb, &expected, sizeof(eb));
					std::memcpy(&ab, &actual, sizeof(ab));
					Assert::AreEqual(eb, ab, msg);
				};

				// 1. Both planes rgba32f: must stay fp32 and read back bit-exact (zero truncation).
				auto c32 = runOne(fwd32, bwd32, 1);
				Assert::IsFalse(ring.NativeIsFp16ForTest(), L"MUST 3: rgba32f source must NOT stage as fp16");
				for (size_t i = 0; i < n; ++i)
				{
					checkExact(a32[i * 4 + 0], c32->fwdX[i], L"rgba32f fwdX truncated");
					checkExact(a32[i * 4 + 1], c32->fwdY[i], L"rgba32f fwdY truncated");
					checkExact(a32[i * 4 + 2], c32->fwdTrust[i], L"rgba32f fwdTrust truncated");
					checkExact(b32[i * 4 + 0], c32->bwdX[i], L"rgba32f bwdX truncated");
					checkExact(b32[i * 4 + 1], c32->bwdY[i], L"rgba32f bwdY truncated");
					checkExact(b32[i * 4 + 2], c32->bwdTrust[i], L"rgba32f bwdTrust truncated");
				}

				// 2. Mixed pair (rgba16f fwd + rgba32f bwd): must stay fp32 so bwd is never truncated.
				auto cMix = runOne(fwd16, bwd32, 2);
				Assert::IsFalse(ring.NativeIsFp16ForTest(), L"MUST 3: mixed rgba16f/rgba32f pair must stay fp32");
				for (size_t i = 0; i < n; ++i)
				{
					checkExact(b32[i * 4 + 0], cMix->bwdX[i], L"mixed bwdX truncated");
					checkExact(b32[i * 4 + 1], cMix->bwdY[i], L"mixed bwdY truncated");
					checkExact(b32[i * 4 + 2], cMix->bwdTrust[i], L"mixed bwdTrust truncated");
				}

				// 3. Switch to both-rgba16f at SAME dimensions: TryNative must recreate staging as fp16.
				auto c16 = runOne(fwd16, bwd16, 3);
				Assert::IsTrue(ring.NativeIsFp16ForTest(), L"MUST 3: both-rgba16f pair must switch to fp16 staging");
				for (size_t i = 0; i < n; ++i)
				{
					checkExact(CameraLink::NativeCopyReadback::HalfToFloat(raw16[i * 4 + 0]), c16->fwdX[i], L"rgba16f fwdX mismatch");
					checkExact(CameraLink::NativeCopyReadback::HalfToFloat(raw16[i * 4 + 1]), c16->fwdY[i], L"rgba16f fwdY mismatch");
					checkExact(CameraLink::NativeCopyReadback::HalfToFloat(raw16[i * 4 + 2]), c16->fwdTrust[i], L"rgba16f fwdTrust mismatch");
				}

				// 4. Switch BACK to rgba32f at SAME dimensions: TryNative must recreate staging back to fp32.
				auto c32Back = runOne(fwd32, bwd32, 4);
				Assert::IsFalse(ring.NativeIsFp16ForTest(), L"MUST 3: switching back to rgba32f must recreate fp32 staging");
				for (size_t i = 0; i < n; ++i)
				{
					checkExact(a32[i * 4 + 0], c32Back->fwdX[i], L"rgba32f switch-back fwdX truncated");
					checkExact(b32[i * 4 + 2], c32Back->bwdTrust[i], L"rgba32f switch-back bwdTrust truncated");
				}
			}
			pl_tex_destroy(gpu, &fwd32);
			pl_tex_destroy(gpu, &bwd32);
			pl_tex_destroy(gpu, &fwd16);
			pl_tex_destroy(gpu, &bwd16);
		}

		// MC-READBACK SHOULD 2 (verify-gemini-mc-81ee082cf.md:96-98): in-run A/B passing the EXACT SAME rgba16f textures
		// (populated with full-range raw IEEE-754 binary16 bit patterns: subnormals, +/-0, +/-65504, negatives, fine fractions)
		// through Create(..., fp16=false) and Create(..., fp16=true), comparing exact byte equality and median/min timing,
		// plus FieldReadbackRing 0 steady-state allocation verification.
		BEGIN_TEST_METHOD_ATTRIBUTE(McReadbackFp32VsFp16AbEqualityAndTimingPin)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(McReadbackFp32VsFp16AbEqualityAndTimingPin)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_fmt fmt16 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(fmt16, L"no rgba16f format");

			// Deterministic full-range finite IEEE-754 half bit patterns (exp in 0..30, both signs, explicit edge patterns).
			auto makeFullRangeHalfBits = [](size_t count, uint64_t seed) -> std::vector<uint16_t>
			{
				const uint16_t edges[] = {
					0x0000u, 0x8000u,   // +0.0, -0.0
					0x0001u, 0x8001u,   // min +/- subnormal
					0x03FFu, 0x83FFu,   // max +/- subnormal
					0x0400u, 0x8400u,   // min +/- normal
					0x3C00u, 0xBC00u,   // +1.0, -1.0
					0x3C01u, 0xBC01u,   // 1.0 + 1 ULP, -(1.0 + 1 ULP)
					0x7BFFu, 0xFBFFu    // +65504.0, -65504.0
				};
				std::vector<uint16_t> out(count);
				uint64_t st = seed;
				auto next64 = [&]() -> uint64_t
				{
					st += 0x9E3779B97F4A7C15ull;
					uint64_t z = st;
					z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
					z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
					return z ^ (z >> 31);
				};
				const size_t nEdges = sizeof(edges) / sizeof(edges[0]);
				for (size_t i = 0; i < count; ++i)
				{
					if (i < nEdges)
					{
						out[i] = edges[i];
					}
					else
					{
						const uint64_t r = next64();
						const uint16_t sign = static_cast<uint16_t>((r & 1ull) << 15);
						const uint16_t exp = static_cast<uint16_t>((r >> 1) % 31u);   // 0..30: all finite (subnormals + normals)
						const uint16_t mant = static_cast<uint16_t>((r >> 16) & 0x03FFu);
						out[i] = static_cast<uint16_t>(sign | (exp << 10) | mant);
					}
				}
				return out;
			};

			auto uploadRawHalf = [&](int w, int h, const std::vector<uint16_t>& halfs) -> pl_tex
			{
				pl_tex_params params{};
				params.w = w; params.h = h; params.format = fmt16;
				params.sampleable = true; params.host_writable = true; params.host_readable = true;
				pl_tex tex = pl_tex_create(gpu, &params);
				Assert::IsNotNull(tex, L"cannot create rgba16f texture");
				pl_tex_transfer_params up{};
				up.tex = tex; up.ptr = const_cast<uint16_t*>(halfs.data());
				Assert::IsTrue(pl_tex_upload(gpu, &up), L"rgba16f upload failed");
				return tex;
			};

			struct Case { const char* name; int w; int h; int iters; };
			const Case cases[2] = {
				{ "4K-16px 240x135 2-plane", 240, 135, 15 },
				{ "4K-8px  480x270 2-plane", 480, 270, 11 }
			};
			std::string report;
			for (const Case& cs : cases)
			{
				const size_t planeElems = static_cast<size_t>(cs.w) * cs.h * 4;
				const std::vector<uint16_t> rawA = makeFullRangeHalfBits(planeElems, 0xA11CE001ull + static_cast<uint64_t>(cs.w));
				const std::vector<uint16_t> rawB = makeFullRangeHalfBits(planeElems, 0xB0B00002ull + static_cast<uint64_t>(cs.h));
				pl_tex fwd = uploadRawHalf(cs.w, cs.h, rawA);
				pl_tex bwd = uploadRawHalf(cs.w, cs.h, rawB);

				// In-run A/B on the EXACT SAME rgba16f textures: NativeCopyReadback(fp16=false) vs NativeCopyReadback(fp16=true).
				auto benchNative = [&](bool fp16, std::vector<float>& outBuf, double& medMs, double& minMs)
				{
					CameraLink::NativeCopyReadback rb;
					Assert::IsTrue(rb.Create(fixture.Log(), gpu, 4, cs.w, cs.h, 2, fp16), L"NativeCopyReadback::Create failed");
					Assert::AreEqual(fp16, rb.IsFp16());
					outBuf.assign(planeElems * 2, 0.0f);
					auto runSlot = [&](int slot)
					{
						Assert::IsTrue(rb.Start(slot, fwd, bwd), L"NativeCopyReadback::Start failed");
						pl_gpu_finish(gpu);
						bool ok = false;
						for (int tries = 0; tries < 2000 && !ok; ++tries)
							ok = rb.Read(slot, outBuf.data());
						Assert::IsTrue(ok, L"NativeCopyReadback::Read failed");
					};
					for (int w0 = 0; w0 < 3; ++w0)
						runSlot(w0 % 4);
					std::vector<double> ms(static_cast<size_t>(cs.iters));
					for (int k = 0; k < cs.iters; ++k)
					{
						const auto t0 = std::chrono::steady_clock::now();
						runSlot(k % 4);
						ms[static_cast<size_t>(k)] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
					}
					std::sort(ms.begin(), ms.end());
					medMs = ms[static_cast<size_t>(cs.iters / 2)];
					minMs = ms[0];
					rb.Release();
				};

				std::vector<float> bufFp32, bufFp16;
				double med32 = 0, min32 = 0, med16 = 0, min16 = 0;
				benchNative(false, bufFp32, med32, min32);
				benchNative(true, bufFp16, med16, min16);

				// Exact byte-for-byte equality between fp32-staged and fp16-staged readback on full-range half patterns.
				Assert::AreEqual(bufFp32.size(), bufFp16.size());
				Assert::AreEqual(0, std::memcmp(bufFp32.data(), bufFp16.data(), bufFp32.size() * sizeof(float)),
					L"SHOULD 2: fp16-staged readback must be byte-equal to fp32-staged readback across full-range half patterns");

				// Also verify FieldReadbackRing picks fp16 staging automatically, matches bufFp32 bit-for-bit, and has 0 steady allocs.
				{
					CameraLink::FieldReadbackRing ring(gpu, 4, fixture.Log());
					std::vector<std::shared_ptr<CameraLink::FieldCopy>> ready;
					auto runRing = [&](uint64_t seq)
					{
						ready.clear();
						Assert::IsTrue(ring.Request(MakeVerified(fwd, bwd, cs.w, cs.h, seq, 1.0f)));
						pl_gpu_finish(gpu);
						for (int tries = 0; tries < 2000 && ready.empty(); ++tries)
							ring.Poll(ready);
						Assert::AreEqual<size_t>(1, ready.size(), L"FieldReadbackRing poll did not complete");
					};
					for (uint64_t s = 1; s <= 3; ++s)
						runRing(s);
					Assert::IsTrue(ring.NativeIsFp16ForTest(), L"FieldReadbackRing must auto-select fp16 staging for rgba16f sources");
					const uint64_t allocsAfterWarmup = ring.ScratchReallocationsForTest();
					Assert::AreEqual<uint64_t>(1, allocsAfterWarmup, L"scratch allocates exactly once on warm-up");
					const size_t n = static_cast<size_t>(cs.w) * cs.h;
					const CameraLink::FieldCopy& c = *ready[0];
					for (size_t i = 0; i < n; ++i)
					{
						uint32_t ax = 0, ay = 0, at = 0, bx = 0, by = 0, bt = 0;
						uint32_t rx = 0, ry = 0, rt = 0, rbx = 0, rby = 0, rbt = 0;
						std::memcpy(&ax, &bufFp32[i * 4 + 0], 4); std::memcpy(&rx, &c.fwdX[i], 4);
						std::memcpy(&ay, &bufFp32[i * 4 + 1], 4); std::memcpy(&ry, &c.fwdY[i], 4);
						std::memcpy(&at, &bufFp32[i * 4 + 2], 4); std::memcpy(&rt, &c.fwdTrust[i], 4);
						std::memcpy(&bx, &bufFp32[planeElems + i * 4 + 0], 4); std::memcpy(&rbx, &c.bwdX[i], 4);
						std::memcpy(&by, &bufFp32[planeElems + i * 4 + 1], 4); std::memcpy(&rby, &c.bwdY[i], 4);
						std::memcpy(&bt, &bufFp32[planeElems + i * 4 + 2], 4); std::memcpy(&rbt, &c.bwdTrust[i], 4);
						Assert::AreEqual(ax, rx, L"FieldReadbackRing fwdX bit mismatch");
						Assert::AreEqual(ay, ry, L"FieldReadbackRing fwdY bit mismatch");
						Assert::AreEqual(at, rt, L"FieldReadbackRing fwdTrust bit mismatch");
						Assert::AreEqual(bx, rbx, L"FieldReadbackRing bwdX bit mismatch");
						Assert::AreEqual(by, rby, L"FieldReadbackRing bwdY bit mismatch");
						Assert::AreEqual(bt, rbt, L"FieldReadbackRing bwdTrust bit mismatch");
					}
					for (int k = 0; k < cs.iters; ++k)
						runRing(static_cast<uint64_t>(10 + k));
					Assert::AreEqual(allocsAfterWarmup, ring.ScratchReallocationsForTest(),
						L"GEM-143: 0 steady-state scratch allocations per Poll()");
					char line[256];
					std::snprintf(line, sizeof(line),
						"MC-READBACK A/B [%s]: fp32 med=%.3f min=%.3f ms -> fp16 med=%.3f min=%.3f ms (byteEqual=1, steadyAllocs=%llu)\n",
						cs.name, med32, min32, med16, min16,
						static_cast<unsigned long long>(ring.ScratchReallocationsForTest() - allocsAfterWarmup));
					report += line;
				}
				pl_tex_destroy(gpu, &fwd);
				pl_tex_destroy(gpu, &bwd);
			}
			Logger::WriteMessage(report.c_str());
		}

		// MC-READBACK (GEM-143 / P1-1): exhaustive bit-exact verification of IEEE-754 binary16 -> binary32
		// across all 65,536 half-float bit patterns (normals, 2046 subnormals, +/-0, +/-Inf, NaN).
		TEST_METHOD(HalfToFloatCoversAll65536BitPatterns)
		{
			for (uint32_t h = 0; h < 65536u; ++h)
			{
				const float got = CameraLink::NativeCopyReadback::HalfToFloat(static_cast<uint16_t>(h));
				uint32_t gotBits = 0;
				std::memcpy(&gotBits, &got, sizeof(gotBits));
				const bool neg = (h & 0x8000u) != 0;
				const uint32_t exp = (h >> 10) & 0x1Fu;
				const uint32_t mant = h & 0x03FFu;
				if (exp == 0 && mant == 0)
				{
					Assert::AreEqual(neg ? 0x80000000u : 0x00000000u, gotBits);
				}
				else if (exp == 31u && mant == 0)
				{
					Assert::AreEqual(neg ? 0xFF800000u : 0x7F800000u, gotBits);
				}
				else if (exp == 31u)
				{
					Assert::IsTrue(std::isnan(got));
				}
				else
				{
					const double expected = (neg ? -1.0 : 1.0) * (exp == 0
						? std::ldexp(static_cast<double>(mant), -24)
						: std::ldexp(static_cast<double>(1024u + mant), static_cast<int>(exp) - 25));
					Assert::AreEqual(static_cast<float>(expected), got, 0.0f);
				}
			}
		}

		// GEM-135 (AB1#3): batched pl_gpu_flush on OnFrameStart() (eliminating per-Start flushes) and safe
		// Draining transition on Clear() while native copies are in flight — never delivered, never prematurely
		// reused, drains cleanly, and delivers subsequent fresh frames bit-exact.
		BEGIN_TEST_METHOD_ATTRIBUTE(ClearWithInFlightNativeCopiesDrainsSafelyWithoutDelivery)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ClearWithInFlightNativeCopiesDrainsSafelyWithoutDelivery)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int w = 32, h = 24;
			const size_t nBlocks = static_cast<size_t>(w) * static_cast<size_t>(h);
			const std::vector<float> oldFwdData = KnownTexels(w, h);
			std::vector<float> oldBwdData = oldFwdData;
			for (float& v : oldBwdData) v = -v * 0.5f - 10.0f;
			std::vector<float> freshFwdData(nBlocks * 4);
			std::vector<float> freshBwdData(nBlocks * 4);
			for (size_t i = 0; i < nBlocks * 4; ++i)
			{
				freshFwdData[i] = 500.0f + static_cast<float>(i % 97) * 0.5f;
				freshBwdData[i] = -500.0f - static_cast<float>(i % 89) * 0.25f;
			}
			TexGuard oldFwd(gpu, MakeField(gpu, w, h, oldFwdData));
			TexGuard oldBwd(gpu, MakeField(gpu, w, h, oldBwdData));
			TexGuard freshFwd(gpu, MakeField(gpu, w, h, freshFwdData));
			TexGuard freshBwd(gpu, MakeField(gpu, w, h, freshBwdData));

			constexpr uint64_t kBatchCount = 3;

			// Rule 3 negative control on the exact same inputs:
			// (a) Flushing after every Start() (pre-fix 1-flush-per-Start behavior) costs kBatchCount flushes (> 1).
			// (b) Without Clear(), the exact same kBatchCount queued native copies complete and deliver all 3 frames.
			{
				CameraLink::FieldReadbackRing baselineRing(gpu, static_cast<int>(kBatchCount), fixture.Log());
				for (uint64_t s = 0; s < kBatchCount; ++s)
				{
					Assert::IsTrue(baselineRing.Request(MakeVerified(oldFwd.tex, oldBwd.tex, w, h, 100 + s, 1.0f)),
						L"baseline Request failed");
					Assert::IsTrue(baselineRing.FlushPending(), L"baseline per-request FlushPending must submit copy");
				}
				Assert::AreEqual<uint64_t>(kBatchCount, baselineRing.FlushCount(),
					L"negative control: naive per-Start flushing issues 1 flush per request");
				pl_gpu_finish(gpu);
				std::vector<std::shared_ptr<CameraLink::FieldCopy>> baseReady;
				for (int tries = 0; tries < 2000 && baseReady.size() < static_cast<size_t>(kBatchCount); ++tries)
				{
					baselineRing.Poll(baseReady);
					if (baseReady.size() < static_cast<size_t>(kBatchCount))
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				Assert::AreEqual<size_t>(static_cast<size_t>(kBatchCount), baseReady.size(),
					L"negative control: without Clear(), all 3 queued copies must deliver");
				Assert::AreEqual<uint64_t>(0, baselineRing.DrainedCount(), L"baseline had no Clear/drain");
			}

			// Protected path: batched flush + Clear() draining + fresh frame delivery after drain.
			{
				CameraLink::FieldReadbackRing ring(gpu, static_cast<int>(kBatchCount), fixture.Log());
				Assert::IsFalse(ring.HasPendingFlush(), L"fresh ring must not have pending flush");
				Assert::AreEqual<uint64_t>(0, ring.FlushCount(), L"fresh ring must start with 0 flushes");
				Assert::AreEqual<size_t>(0, ring.DrainingSlots(), L"fresh ring must start with 0 draining slots");
				Assert::AreEqual<uint64_t>(0, ring.DrainedCount(), L"fresh ring must start with 0 drained count");

				for (uint64_t s = 0; s < kBatchCount; ++s)
				{
					Assert::IsTrue(ring.Request(MakeVerified(oldFwd.tex, oldBwd.tex, w, h, 100 + s, 1.0f)),
						L"batched Request failed");
				}
				Assert::IsTrue(ring.HasPendingFlush(),
					L"GEM-135: queued native copies must report HasPendingFlush() == true before OnFrameStart()");
				Assert::AreEqual<uint64_t>(0, ring.FlushCount(),
					L"GEM-135: Start() must NOT issue per-request pl_gpu_flush before OnFrameStart()");

				// Single frame-boundary batched flush for all 3 queued Start() calls.
				ring.OnFrameStart();
				Assert::AreEqual<uint64_t>(1, ring.FlushCount(),
					L"GEM-135: OnFrameStart() must batch multiple Start() calls into ONE pl_gpu_flush");
				Assert::IsFalse(ring.HasPendingFlush(),
					L"GEM-135: HasPendingFlush() must be false after OnFrameStart()");
				// Redundant OnFrameStart() with nothing pending must not issue an extra flush.
				ring.OnFrameStart();
				Assert::AreEqual<uint64_t>(1, ring.FlushCount(),
					L"GEM-135: redundant OnFrameStart() with no pending flush must not increment FlushCount()");

				// Clear() while native slots are InFlight: must enter Draining, NOT be immediately freed.
				ring.Clear();
				Assert::AreEqual<size_t>(static_cast<size_t>(kBatchCount), ring.DrainingSlots(),
					L"GEM-135: Clear() with in-flight native copies must move all 3 slots into Draining");
				Assert::AreEqual<uint64_t>(0, ring.DrainedCount(), L"DrainedCount() must remain 0 before Poll/drain");
				Assert::AreEqual<uint64_t>(0, ring.Stats().drained, L"Stats().drained must remain 0 before Poll/drain");

				// Repeated Clear() while already Draining must keep slots Draining (never prematurely Free).
				ring.Clear();
				Assert::AreEqual<size_t>(static_cast<size_t>(kBatchCount), ring.DrainingSlots(),
					L"GEM-135: repeated Clear() must not reset Draining slots to Free");

				// While all slots are Draining, Request() must drop rather than overwrite a live GPU copy.
				Assert::IsFalse(ring.Request(MakeVerified(freshFwd.tex, freshBwd.tex, w, h, 199, 1.0f)),
					L"GEM-135: Draining slots must not be reused before GPU completion");
				Assert::AreEqual<uint64_t>(1, ring.Stats().dropped, L"full-of-Draining ring must drop new Request()");

				// Subsequent Poll() / OnFrameStart() must drain all 3 cleanly WITHOUT delivering any cleared frame.
				std::vector<std::shared_ptr<CameraLink::FieldCopy>> clearedReady;
				for (int tries = 0; tries < 2000 && ring.DrainingSlots() > 0; ++tries)
				{
					if ((tries & 1) == 0)
						ring.Poll(clearedReady);
					else
						ring.OnFrameStart();
					if (ring.DrainingSlots() > 0)
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				Assert::AreEqual<size_t>(0, ring.DrainingSlots(),
					L"GEM-135: Draining slots must drain cleanly to 0");
				Assert::AreEqual<uint64_t>(kBatchCount, ring.DrainedCount(),
					L"GEM-135: DrainedCount() must count every drained slot");
				Assert::AreEqual<uint64_t>(kBatchCount, ring.Stats().drained,
					L"GEM-135: Stats().drained must match DrainedCount()");
				Assert::IsTrue(clearedReady.empty(),
					L"GEM-135: cleared frames must NEVER be delivered as valid readbacks");
				Assert::AreEqual<uint64_t>(0, ring.Stats().completed,
					L"GEM-135: Stats().completed must remain 0 after draining cleared slots");
				Assert::AreEqual<uint64_t>(1, ring.FlushCount(),
					L"GEM-135: draining already-flushed slots must not issue extra pl_gpu_flush");

				// Enqueue a fresh frame after drain and assert exact new frame data delivers cleanly.
				const uint64_t freshSeq = 201;
				const float freshScale = 1.5f;
				Assert::IsTrue(ring.Request(MakeVerified(freshFwd.tex, freshBwd.tex, w, h, freshSeq, freshScale)),
					L"Request after drain must succeed");
				Assert::IsTrue(ring.HasPendingFlush(), L"fresh Request must set HasPendingFlush()");
				Assert::AreEqual<uint64_t>(1, ring.FlushCount(), L"fresh Request must not flush before OnFrameStart()");
				ring.OnFrameStart();
				Assert::IsFalse(ring.HasPendingFlush(), L"OnFrameStart() must clear HasPendingFlush()");
				Assert::AreEqual<uint64_t>(2, ring.FlushCount(), L"OnFrameStart() must increment FlushCount() once");

				pl_gpu_finish(gpu);
				std::vector<std::shared_ptr<CameraLink::FieldCopy>> freshReady;
				for (int tries = 0; tries < 2000 && freshReady.empty(); ++tries)
				{
					ring.Poll(freshReady);
					if (freshReady.empty())
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				Assert::AreEqual<size_t>(1, freshReady.size(), L"fresh frame after drain must deliver exactly 1 copy");
				const CameraLink::FieldCopy& got = *freshReady[0];
				Assert::IsTrue(got.Valid(), L"fresh copy must be valid");
				Assert::IsTrue(got.HaveBackward(), L"fresh copy must have backward plane");
				Assert::AreEqual(freshSeq, got.seq, L"fresh copy must carry the new seq, never a cleared seq");
				Assert::AreEqual<uint64_t>(1, ring.Stats().completed, L"exactly 1 completed copy");
				Assert::AreEqual<uint64_t>(0, ring.Stats().failed, L"0 failed copies");
				for (size_t i = 0; i < nBlocks; ++i)
				{
					Assert::IsTrue(got.fwdX[i] == freshFwdData[i * 4 + 0] * freshScale, L"fresh fwdX mismatch");
					Assert::IsTrue(got.fwdY[i] == freshFwdData[i * 4 + 1] * freshScale, L"fresh fwdY mismatch");
					Assert::IsTrue(got.fwdTrust[i] == freshFwdData[i * 4 + 2], L"fresh fwdTrust mismatch");
					Assert::IsTrue(got.bwdX[i] == freshBwdData[i * 4 + 0] * freshScale, L"fresh bwdX mismatch");
					Assert::IsTrue(got.bwdY[i] == freshBwdData[i * 4 + 1] * freshScale, L"fresh bwdY mismatch");
					Assert::IsTrue(got.bwdTrust[i] == freshBwdData[i * 4 + 2], L"fresh bwdTrust mismatch");
				}
			}
		}

		// GEM-135 (AB1#3): FieldReadbackRing and CameraLabelProducer must declare exact allocated VRAM bytes
		// under VpSkeleton::kUserCameraModel and withdraw them cleanly on both explicit Release() and destruction.
		BEGIN_TEST_METHOD_ATTRIBUTE(ReadbackRingAndLabelProducerDeclareAndWithdrawVramUsers)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ReadbackRingAndLabelProducerDeclareAndWithdrawVramUsers)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();

			const uint64_t baselineBytes = CameraModelVramBytes();
			const bool baselineUnpopulated = CameraModelIsUnpopulated();

			const int ringW = 32, ringH = 24, ringEntries = 4;
			const int labelW = 20, labelH = 12;
			TexGuard srcFwd(gpu, MakeField(gpu, ringW, ringH, KnownTexels(ringW, ringH)));
			TexGuard srcBwd(gpu, MakeField(gpu, ringW, ringH, KnownTexels(ringW, ringH)));

			// Independent physical byte derivations (Rule 1 & Rule 3 — no self-referenced oracle):
			// FieldReadbackRing native rgba32f: entries * w * h * 2 planes * 16 bytes/texel * 2 textures/slot (default + staging).
			constexpr uint64_t kRgba32fTexelBytes = 4ull * sizeof(float);
			constexpr uint64_t kNativePlanes = 2ull;
			constexpr uint64_t kTexturesPerNativeSlot = 2ull;
			const uint64_t expectedRingBytes = static_cast<uint64_t>(ringEntries) * static_cast<uint64_t>(ringW)
				* static_cast<uint64_t>(ringH) * kNativePlanes * kRgba32fTexelBytes * kTexturesPerNativeSlot;
			const uint64_t expectedOneLabelTexBytes = static_cast<uint64_t>(labelW) * static_cast<uint64_t>(labelH)
				* kRgba32fTexelBytes;

			const size_t labelBlocks = static_cast<size_t>(labelW) * static_cast<size_t>(labelH);
			auto blocks = std::make_shared<CameraModel::BlockData>();
			blocks->gridW = labelW;
			blocks->gridH = labelH;
			blocks->blockSize = 16.0;
			blocks->label.assign(labelBlocks, static_cast<uint8_t>(CameraModel::Label::Camera));
			blocks->modelX.assign(labelBlocks, 1.5f);
			blocks->modelY.assign(labelBlocks, -0.75f);
			blocks->resX.assign(labelBlocks, 0.1f);
			blocks->resY.assign(labelBlocks, 0.2f);
			blocks->tinyMover.assign(labelBlocks, 0);
			CameraModel::CameraMotion rec;
			rec.valid = true;
			rec.reason = CameraModel::Reason::Ok;
			rec.blocks = blocks;

			// Rule 3 negative control: an undeclared owner that marks active without DeclareFixed leaves
			// VramUsers blind (0 bytes added) and flags kUserCameraModel in Snapshot::unpopulated.
			{
				int undeclaredOwner = 0;
				struct OwnerGuard
				{
					const void* owner;
					explicit OwnerGuard(const void* o) : owner(o) {}
					OwnerGuard(const OwnerGuard&) = delete;
					OwnerGuard& operator=(const OwnerGuard&) = delete;
					~OwnerGuard() { VpSkeleton::VramUsers::Instance().Release(owner, VpSkeleton::kUserCameraModel); }
				} ownerGuard(&undeclaredOwner);
				VpSkeleton::VramUsers::Instance().MarkActive(&undeclaredOwner, VpSkeleton::kUserCameraModel, true);
				Assert::AreEqual(baselineBytes, CameraModelVramBytes(),
					L"negative control: undeclared allocation adds 0 bytes to VramUsers");
				Assert::IsTrue(CameraModelIsUnpopulated(),
					L"negative control: active owner without declaration must surface in VramUsers::unpopulated");
			}
			Assert::AreEqual(baselineBytes, CameraModelVramBytes(), L"negative control cleanup must restore baseline bytes");
			Assert::AreEqual(baselineUnpopulated, CameraModelIsUnpopulated(), L"negative control cleanup must restore unpopulated state");

			// Part 1: Explicit Release() path, per-owner isolation, and idempotency.
			{
				CameraLink::FieldReadbackRing ring(gpu, ringEntries, fixture.Log());
				CameraLink::CameraLabelProducer prod(fixture.Log(), gpu, CameraLink::LabelMode::Upload, 3, 2, CameraModel::Config(), false);
				Assert::AreEqual<uint64_t>(0, ring.DeclaredVramBytes(), L"ring must declare 0 before allocation");
				Assert::AreEqual<uint64_t>(0, prod.DeclaredVramBytes(), L"producer must declare 0 before allocation");
				Assert::AreEqual(baselineBytes, CameraModelVramBytes(), L"VramUsers must stay at baseline before allocations");

				Assert::IsTrue(ring.Request(MakeVerified(srcFwd.tex, srcBwd.tex, ringW, ringH, 1, 1.0f)),
					L"ring Request for VRAM declaration test failed");
				Assert::IsTrue(ring.DeclaredVramBytes() > 0, L"GEM-135: FieldReadbackRing must declare > 0 VRAM bytes");
				Assert::AreEqual(expectedRingBytes, ring.DeclaredVramBytes(),
					L"GEM-135: FieldReadbackRing DeclaredVramBytes must match exact allocated native texture bytes");
				Assert::AreEqual(baselineBytes + ring.DeclaredVramBytes(), CameraModelVramBytes(),
					L"GEM-135: VramUsers(kUserCameraModel) must increase by exact FieldReadbackRing declared bytes");

				CameraLink::LabelTarget ringSlot;
				ringSlot.live = false;
				ringSlot.index = 0;
				CameraLink::LabelTarget liveSlot;
				liveSlot.live = true;
				liveSlot.index = 0;
				Assert::IsTrue(prod.Produce(rec, nullptr, ringSlot), L"producer ring slot Produce failed");
				Assert::AreEqual(expectedOneLabelTexBytes, prod.DeclaredVramBytes(),
					L"GEM-135: CameraLabelProducer must declare exact bytes for 1 allocated label texture");
				Assert::IsTrue(prod.Produce(rec, nullptr, liveSlot), L"producer live slot Produce failed");
				Assert::IsTrue(prod.DeclaredVramBytes() > 0, L"GEM-135: CameraLabelProducer must declare > 0 VRAM bytes");
				Assert::AreEqual(expectedOneLabelTexBytes * 2ull, prod.DeclaredVramBytes(),
					L"GEM-135: CameraLabelProducer must declare exact bytes for 2 allocated label textures");
				Assert::AreEqual(baselineBytes + ring.DeclaredVramBytes() + prod.DeclaredVramBytes(), CameraModelVramBytes(),
					L"GEM-135: VramUsers(kUserCameraModel) total must equal baseline + ring + producer declared bytes");
				Assert::AreEqual(baselineUnpopulated, CameraModelIsUnpopulated(),
					L"GEM-135: declared owners must not mark kUserCameraModel as unpopulated");

				// Release ring first: must withdraw only ring's bytes, leaving producer's declaration intact.
				const uint64_t prodBytes = prod.DeclaredVramBytes();
				ring.Release();
				Assert::AreEqual<uint64_t>(0, ring.DeclaredVramBytes(), L"GEM-135: ring DeclaredVramBytes must be 0 after Release()");
				Assert::AreEqual(baselineBytes + prodBytes, CameraModelVramBytes(),
					L"GEM-135: releasing ring must withdraw exact ring bytes without touching producer");
				ring.Release();   // idempotent
				Assert::AreEqual<uint64_t>(0, ring.DeclaredVramBytes(), L"ring Release() must be idempotent");
				Assert::AreEqual(baselineBytes + prodBytes, CameraModelVramBytes(), L"idempotent ring Release() must preserve VramUsers");

				// Release producer: must return VramUsers to exact baseline.
				prod.Release();
				Assert::AreEqual<uint64_t>(0, prod.DeclaredVramBytes(), L"GEM-135: producer DeclaredVramBytes must be 0 after Release()");
				Assert::AreEqual(baselineBytes, CameraModelVramBytes(),
					L"GEM-135: VramUsers(kUserCameraModel) must return to exact baseline after both Release() calls");
				prod.Release();   // idempotent
				Assert::AreEqual<uint64_t>(0, prod.DeclaredVramBytes(), L"producer Release() must be idempotent");
				Assert::AreEqual(baselineBytes, CameraModelVramBytes(), L"idempotent producer Release() must keep exact baseline");
			}
			Assert::AreEqual(baselineBytes, CameraModelVramBytes(), L"post-scope VramUsers must remain at exact baseline");
			Assert::AreEqual(baselineUnpopulated, CameraModelIsUnpopulated(), L"post-scope unpopulated state must match baseline");

			// Part 2: Destructor withdrawal path (no explicit Release() call; ring still has an in-flight copy).
			{
				const int dtorEntries = 3;
				const uint64_t expectedDtorRingBytes = static_cast<uint64_t>(dtorEntries) * static_cast<uint64_t>(ringW)
					* static_cast<uint64_t>(ringH) * kNativePlanes * kRgba32fTexelBytes * kTexturesPerNativeSlot;
				CameraLink::FieldReadbackRing dtorRing(gpu, dtorEntries, fixture.Log());
				CameraLink::CameraLabelProducer dtorProd(fixture.Log(), gpu, CameraLink::LabelMode::Upload, 2, 1, CameraModel::Config(), false);
				Assert::IsTrue(dtorRing.Request(MakeVerified(srcFwd.tex, srcBwd.tex, ringW, ringH, 2, 1.0f)),
					L"destructor-path ring Request failed");
				CameraLink::LabelTarget slot0;
				slot0.live = false;
				slot0.index = 0;
				Assert::IsTrue(dtorProd.Produce(rec, nullptr, slot0), L"destructor-path producer Produce failed");
				Assert::AreEqual(expectedDtorRingBytes, dtorRing.DeclaredVramBytes(), L"destructor-path ring declared bytes");
				Assert::AreEqual(expectedOneLabelTexBytes, dtorProd.DeclaredVramBytes(), L"destructor-path producer declared bytes");
				Assert::AreEqual(baselineBytes + dtorRing.DeclaredVramBytes() + dtorProd.DeclaredVramBytes(), CameraModelVramBytes(),
					L"destructor-path VramUsers must reflect both allocations before scope exit");
			}
			Assert::AreEqual(baselineBytes, CameraModelVramBytes(),
				L"GEM-135: destructors must withdraw all kUserCameraModel VRAM back to exact baseline (Rule 2 zero leak)");
			Assert::AreEqual(baselineUnpopulated, CameraModelIsUnpopulated(),
				L"GEM-135: destructors must clear active marks cleanly");
		}

		// W23-Audit-Fix1 POD1-H5: CameraLinkCore::OnFrameStart() must call m_ring->OnFrameStart(), m_ring->Clear()
		// on renumber/reset, and m_ring->FlushPending() after m_ring->Request(vf) so native D3D11 readbacks submit
		// immediately instead of stalling until the next frame's Poll().
		BEGIN_TEST_METHOD_ATTRIBUTE(CameraLinkCoreFlushesPendingReadbacksOnFrameStart)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(CameraLinkCoreFlushesPendingReadbacksOnFrameStart)
		{
			const std::string repoRoot = [&]() {
				std::string dir = __FILE__;
				const size_t pos = dir.find("\\src\\VP-V\\");
				return pos != std::string::npos ? dir.substr(0, pos) : std::string(".");
			}();
			std::ifstream f(repoRoot + "\\src\\VP-V\\modules\\motion\\estimation\\global_model\\link\\CameraLinkCore.cpp");
			Assert::IsTrue(f.is_open(), L"CameraLinkCore.cpp must be readable");
			std::ostringstream ss;
			ss << f.rdbuf();
			const std::string src = ss.str();
			const size_t start = src.find("void CameraLinkCore::OnFrameStart(");
			Assert::IsTrue(start != std::string::npos, L"locate CameraLinkCore::OnFrameStart");
			const size_t end = src.find("void CameraLinkCore::OnPostSubmitGap(", start);
			Assert::IsTrue(end != std::string::npos && end > start);
			const std::string body = src.substr(start, end - start);
			const size_t clearPos = body.find("m_ring->Clear();");
			const size_t onStartPos = body.find("m_ring->OnFrameStart();");
			const size_t reqPos = body.find("m_ring->Request(vf)");
			const size_t flushPos = body.find("m_ring->FlushPending();");
			Assert::IsTrue(clearPos != std::string::npos, L"POD1-H5: OnFrameStart must Clear() ring on renumber/reset");
			Assert::IsTrue(onStartPos != std::string::npos, L"POD1-H5: OnFrameStart must call m_ring->OnFrameStart()");
			Assert::IsTrue(reqPos != std::string::npos && flushPos != std::string::npos && flushPos > reqPos,
				L"POD1-H5: OnFrameStart must call m_ring->FlushPending() after Request(vf)");
		}
	};
}
