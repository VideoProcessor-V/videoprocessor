// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// L-B6 U3 slice (C3 port of 063ac5899): byte identity of the two passes that moved onto the shared unit U3
// (lane/shared/HalfLuma.h). Each test runs the OLD inline box (the verbatim loop from 5b43fc22a~1 DejudderWarp.cpp
// DOWNSAMPLE_BODY, and from 7db0b0631~1 AdaptiveDownscale.cpp HALVE_BODY) and the NEW unit call (vpu_half_luma, and
// vpu_half_luma_rgba_at as in 7db0b0631's HALVE_BODY) on the same synthetic texture through libplacebo compute, and
// compares the output bytes exactly. A CPU reference (VpUnit::HalfLuma::MeanF, tolerance) guards against a vacuous pass
// (both passes writing nothing or the same wrong value). Odd and even sizes, crop origins on both clamp sides, 1 and 4
// channels. WARP unless VP_TEST_GPU=hardware; the review gate asks for the hardware run (category GpuHardware).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		// The OLD de-judder half-luma loop and the NEW unit call, each as the statement that produces `v`.
		const char* const kOldMatcherBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (p.x < size.x && p.y < size.y) {
    float v;
    {
        ivec2 full = textureSize(src, 0);
        float sum = 0.0;
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++)
                sum += texelFetch(src, min(p * 2 + ivec2(x, y), full - ivec2(1)), 0).r;
        v = sum * 0.25 * codes;
    }
    imageStore(dst, p, vec4(v));
}
)GLSL";
		const char* const kNewMatcherBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (p.x < size.x && p.y < size.y) {
    float v;
    {
        v = vpu_half_luma(src, p) * codes;
    }
    imageStore(dst, p, vec4(v));
}
)GLSL";
		// The OLD doubler-to-source halving body (7db0b0631~1) and the NEW production body (7db0b0631).
		const char* const kOldHalveBody = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    ivec2 sz = textureSize(src, 0);
    ivec2 p = ivec2(origin) + q * 2;
    vec4 s = vec4(0.0);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
            s += texelFetch(src, clamp(p + ivec2(x, y), ivec2(0), sz - ivec2(1)), 0);
    imageStore(dst, q, s * 0.25);
}
)GLSL";
		const char* const kNewHalveBody = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y)
    imageStore(dst, q, vpu_half_luma_rgba_at(src, ivec2(origin), q));
)GLSL";

		// Values that are not exactly representable sums, so a changed association would change bits.
		std::vector<float> Texels(int w, int h, int channels, uint32_t seed)
		{
			std::vector<float> v(size_t(w) * h * channels);
			for (size_t i = 0; i < v.size(); ++i)
			{
				seed = seed * 1664525u + 1013904223u;
				v[i] = float((seed >> 8) & 0xFFFFu) / 65535.0f * 0.9f + 0.013f;
			}
			return v;
		}

		pl_fmt FloatFormat(pl_gpu gpu, int channels)
		{
			const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			return pl_find_fmt(gpu, PL_FMT_FLOAT, channels, 32, 32, caps);
		}

		pl_tex MakeTex(pl_gpu gpu, pl_fmt fmt, int w, int h)
		{
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = fmt;
			tp.sampleable = true;
			tp.storable = true;
			tp.host_writable = true;
			tp.host_readable = true;
			pl_tex t = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(t, L"cannot create the texture");
			return t;
		}

		std::vector<uint8_t> Bytes(pl_gpu gpu, pl_tex tex)
		{
			std::vector<uint8_t> v(size_t(tex->params.w) * tex->params.h * tex->params.format->texel_size);
			pl_tex_transfer_params d{};
			d.tex = tex;
			d.ptr = v.data();
			Assert::IsTrue(pl_tex_download(gpu, &d), L"download failed");
			return v;
		}

		std::vector<float> Floats(const std::vector<uint8_t>& bytes)
		{
			std::vector<float> f(bytes.size() / sizeof(float));
			std::memcpy(f.data(), bytes.data(), f.size() * sizeof(float));
			return f;
		}

		// Non-vacuity guard: every output sample within a small tolerance of the CPU form of the unit's mean
		// (VpUnit::HalfLuma::MeanF, same taps and order); the exact identity is the memcmp above it.
		bool NearRef(float got, float ref) { return std::fabs(got - ref) <= 1e-5f * (std::max)(1.0f, std::fabs(ref)); }

		bool Run(pl_dispatch dispatch, const std::string& header, const char* body, pl_tex src, pl_tex dst, const char* varName,
			bool vec2Var, const float* varData)
		{
			pl_shader_desc d[2]{};
			d[0].desc.name = "src";
			d[0].desc.type = PL_DESC_SAMPLED_TEX;
			d[0].binding.object = src;
			d[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			d[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			d[1].desc.name = "dst";
			d[1].desc.type = PL_DESC_STORAGE_IMG;
			d[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
			d[1].binding.object = dst;
			pl_shader_var v{ vec2Var ? pl_var_vec2(varName) : pl_var_float(varName), varData, true };
			pl_custom_shader custom{};
			custom.description = "VideoProcessor U3 identity test";
			custom.header = header.c_str();
			custom.body = body;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = d;
			custom.num_descriptors = 2;
			custom.variables = &v;
			custom.num_variables = 1;
			custom.compute = true;
			custom.compute_group_size[0] = 16;
			custom.compute_group_size[1] = 16;
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &custom))
			{
				pl_dispatch_abort(dispatch, &sh);
				return false;
			}
			pl_dispatch_compute_params compute{};
			compute.shader = &sh;
			compute.dispatch_size[0] = (dst->params.w + 15) / 16;
			compute.dispatch_size[1] = (dst->params.h + 15) / 16;
			compute.dispatch_size[2] = 1;
			return pl_dispatch_compute(dispatch, &compute);
		}

		void Upload(pl_gpu gpu, pl_tex tex, const std::vector<float>& texels)
		{
			pl_tex_transfer_params u{};
			u.tex = tex;
			u.ptr = const_cast<float*>(texels.data());
			Assert::IsTrue(pl_tex_upload(gpu, &u), L"upload failed");
		}
	}

	TEST_CLASS(HalfLumaPassesIdentityGpuTests)
	{
	public:
		// The matcher's half-resolution luma (old 2x2 loop vs vpu_half_luma), odd and even source sizes (the odd ones clamp the
		// last tap), two code scales.
		BEGIN_TEST_METHOD_ATTRIBUTE(MatcherHalfLumaOldEqualsUnit)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MatcherHalfLumaOldEqualsUnit)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
			pl_fmt r32 = FloatFormat(gpu, 1), rgba32 = FloatFormat(gpu, 4);
			Assert::IsTrue(r32 && rgba32, L"no 32-bit float storage format");
			const int sizes[][2] = { { 64, 48 }, { 65, 49 }, { 33, 32 }, { 32, 33 }, { 7, 5 }, { 2, 2 }, { 1, 1 } };
			const float scales[2] = { 1.0f, 219.0f };
			const std::string header = VpUnit::HalfLuma::Glsl();
			uint32_t seed = 11;
			for (const auto& s : sizes)
				for (float codes : scales)
				{
					const int w = s[0], h = s[1], hw = (w + 1) / 2, hh = (h + 1) / 2;
					pl_tex src = MakeTex(gpu, r32, w, h);
					pl_tex oldOut = MakeTex(gpu, rgba32, hw, hh), newOut = MakeTex(gpu, rgba32, hw, hh);
					const std::vector<float> in = Texels(w, h, 1, ++seed);
					Upload(gpu, src, in);
					Assert::IsTrue(Run(dispatch, "", kOldMatcherBody, src, oldOut, "codes", false, &codes), L"old pass");
					Assert::IsTrue(Run(dispatch, header, kNewMatcherBody, src, newOut, "codes", false, &codes), L"new pass");
					const std::vector<uint8_t> a = Bytes(gpu, oldOut), b = Bytes(gpu, newOut);
					const std::wstring at = std::to_wstring(w) + L"x" + std::to_wstring(h);
					Assert::IsTrue(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0,
						(L"matcher half luma differs at " + at).c_str());
					const std::vector<float> got = Floats(b);
					Assert::AreEqual(size_t(hw) * hh * 4, got.size(), (L"matcher output size at " + at).c_str());
					for (int y = 0; y < hh; ++y)
						for (int x = 0; x < hw; ++x)
						{
							const int x0 = VpUnit::HalfLuma::Tap0(x, w), x1 = VpUnit::HalfLuma::Tap1(x, w);
							const int y0 = VpUnit::HalfLuma::Tap0(y, h), y1 = VpUnit::HalfLuma::Tap1(y, h);
							const float ref = VpUnit::HalfLuma::MeanF(in[size_t(y0) * w + x0], in[size_t(y0) * w + x1],
								in[size_t(y1) * w + x0], in[size_t(y1) * w + x1]) * codes;
							Assert::IsTrue(ref > 0.0f, L"reference must be non-zero (non-vacuous)");
							for (int c = 0; c < 4; ++c)
								Assert::IsTrue(NearRef(got[(size_t(y) * hw + x) * 4 + c], ref),
									(L"matcher half luma off the unit's mean at " + at + L" sample " + std::to_wstring(x) + L"," +
										std::to_wstring(y)).c_str());
						}
					pl_tex_destroy(gpu, &src);
					pl_tex_destroy(gpu, &oldOut);
					pl_tex_destroy(gpu, &newOut);
				}
			pl_dispatch_destroy(&dispatch);
		}

		// The adaptive upscaler's doubler-to-source halving (old 4-channel loop vs vpu_half_luma_rgba_at): even and odd
		// doubler sizes, crop origin 0 (low clamp side), mid-texture, and ones that force the high clamp, four channels.
		BEGIN_TEST_METHOD_ATTRIBUTE(DoublerHalveOldEqualsUnit)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DoublerHalveOldEqualsUnit)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
			pl_fmt rgba32 = FloatFormat(gpu, 4);
			Assert::IsTrue(rgba32 != nullptr, L"no 32-bit float storage format");
			const int sizes[][2] = { { 64, 48 }, { 65, 49 }, { 33, 17 }, { 9, 7 } };
			const float origins[][2] = { { 0, 0 }, { 2, 4 }, { 3, 1 }, { 1, 5 }, { -2, -2 }, { 30, 20 } };
			const std::string header = VpUnit::HalfLuma::GlslRgbaAt();
			uint32_t seed = 101;
			for (const auto& s : sizes)
				for (const auto& o : origins)
				{
					const int w = s[0], h = s[1];
					const int lw = (w + 1) / 2 + 1, lh = (h + 1) / 2 + 1;   // a little past the texture: the high clamp
					pl_tex src = MakeTex(gpu, rgba32, w, h);
					pl_tex oldOut = MakeTex(gpu, rgba32, lw, lh), newOut = MakeTex(gpu, rgba32, lw, lh);
					const std::vector<float> in = Texels(w, h, 4, ++seed);
					Upload(gpu, src, in);
					Assert::IsTrue(Run(dispatch, "", kOldHalveBody, src, oldOut, "origin", true, o), L"old pass");
					Assert::IsTrue(Run(dispatch, header, kNewHalveBody, src, newOut, "origin", true, o), L"new pass");
					const std::vector<uint8_t> a = Bytes(gpu, oldOut), b = Bytes(gpu, newOut);
					const std::wstring at = std::to_wstring(w) + L"x" + std::to_wstring(h) + L" origin " +
						std::to_wstring(int(o[0])) + L"," + std::to_wstring(int(o[1]));
					Assert::IsTrue(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0,
						(L"doubler halve differs at " + at).c_str());
					const std::vector<float> got = Floats(b);
					Assert::AreEqual(size_t(lw) * lh * 4, got.size(), (L"doubler output size at " + at).c_str());
					const int ox = int(o[0]), oy = int(o[1]);
					auto cl = [](int v, int n) { return (std::min)((std::max)(v, 0), n - 1); };
					for (int y = 0; y < lh; ++y)
						for (int x = 0; x < lw; ++x)
						{
							const int x0 = cl(ox + 2 * x, w), x1 = cl(ox + 2 * x + 1, w);
							const int y0 = cl(oy + 2 * y, h), y1 = cl(oy + 2 * y + 1, h);
							for (int c = 0; c < 4; ++c)
							{
								const float ref = VpUnit::HalfLuma::MeanF(in[(size_t(y0) * w + x0) * 4 + c],
									in[(size_t(y0) * w + x1) * 4 + c], in[(size_t(y1) * w + x0) * 4 + c],
									in[(size_t(y1) * w + x1) * 4 + c]);
								Assert::IsTrue(ref > 0.0f, L"reference must be non-zero (non-vacuous)");
								Assert::IsTrue(NearRef(got[(size_t(y) * lw + x) * 4 + c], ref),
									(L"doubler halve off the unit's mean at " + at + L" sample " + std::to_wstring(x) + L"," +
										std::to_wstring(y) + L" channel " + std::to_wstring(c)).c_str());
							}
						}
					pl_tex_destroy(gpu, &src);
					pl_tex_destroy(gpu, &oldOut);
					pl_tex_destroy(gpu, &newOut);
				}
			pl_dispatch_destroy(&dispatch);
		}
	};
}
