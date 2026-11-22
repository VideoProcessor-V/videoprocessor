// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link K1-4: the field readback ring on the libplacebo WARP device (VP_TEST_GPU=hardware for a real GPU):
// exact round trip, never waits when full, teardown with copies in flight; plus a pure CPU UnpackRgba test.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/global_model/link/FieldReadbackRing.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
	};
}
