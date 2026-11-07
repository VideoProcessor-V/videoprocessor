// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// U15 FieldRefine v1: the search shader and the per-pixel offset accessor on the libplacebo WARP device (VP_TEST_GPU=hardware
// for a real GPU) against the CPU reference (FieldRefineRef.h). Ref == Glsl except on near-tie blocks, where float
// and the GPU's arithmetic may legitimately choose a different equal-cost candidate.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/refine/FieldRefineRef.h>
#include <VP-V/modules/motion/estimation/refine/FieldRefineGlsl.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <limits>

namespace VideoProcessorTest
{
	namespace
	{
		using namespace VpUnit::FieldRefine;

		pl_tex MakeTex(pl_gpu gpu, int comps, int w, int h, const float* data, bool storable, bool readable)
		{
			pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, comps, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | (storable ? PL_FMT_CAP_STORABLE : 0)));
			Assert::IsNotNull(fmt, L"no float32 format");
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = fmt;
			params.sampleable = true;
			params.storable = storable;
			params.host_readable = readable;
			params.initial_data = data;
			pl_tex tex = pl_tex_create(gpu, &params);
			Assert::IsNotNull(tex, L"cannot create the texture");
			return tex;
		}

		std::vector<float> Download4(pl_gpu gpu, pl_tex tex)
		{
			std::vector<float> v(static_cast<size_t>(tex->params.w) * tex->params.h * 4);
			pl_tex_transfer_params d{};
			d.tex = tex;
			d.ptr = v.data();
			Assert::IsTrue(pl_tex_download(gpu, &d), L"the texture is not host-readable");
			return v;
		}

		pl_shader_desc Sampled(const char* name, pl_tex tex)
		{
			pl_shader_desc desc{};
			desc.desc.name = name;
			desc.desc.type = PL_DESC_SAMPLED_TEX;
			desc.binding.object = tex;
			desc.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			desc.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			return desc;
		}

		pl_shader_desc Storage(const char* name, pl_tex tex)
		{
			pl_shader_desc desc{};
			desc.desc.name = name;
			desc.desc.type = PL_DESC_STORAGE_IMG;
			desc.desc.access = PL_DESC_ACCESS_WRITEONLY;
			desc.binding.object = tex;
			return desc;
		}

		bool RunCustom(pl_dispatch dispatch, const char* header, const char* body, const pl_shader_desc* desc, int numDesc,
			const pl_shader_var* vars, int numVars, int w, int h)
		{
			pl_custom_shader custom{};
			custom.description = "VideoProcessor FieldRefine test";
			custom.header = header;
			custom.body = body;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = desc;
			custom.num_descriptors = numDesc;
			custom.variables = vars;
			custom.num_variables = numVars;
			custom.compute = true;
			custom.compute_group_size[0] = 8;
			custom.compute_group_size[1] = 8;
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &custom))
			{
				pl_dispatch_abort(dispatch, &sh);
				return false;
			}
			pl_dispatch_compute_params compute{};
			compute.shader = &sh;
			compute.dispatch_size[0] = (w + 7) / 8;
			compute.dispatch_size[1] = (h + 7) / 8;
			compute.dispatch_size[2] = 1;
			return pl_dispatch_compute(dispatch, &compute);
		}

		double Tex(double x, double y)
		{
			return 120.0 + 50.0 * std::sin(0.7 * x + 0.3) + 40.0 * std::cos(0.5 * y + 0.2 * x) + 25.0 * std::sin(0.031 * x * y + y);
		}

		void Fail(const std::string& what)
		{
			Logger::WriteMessage((what + "\n").c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}
	}

	TEST_CLASS(FieldRefineWarpTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(SearchShaderMatchesTheCpuReference)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SearchShaderMatchesTheCpuReference)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);

			const int w = 48, h = 32, gw = 12, gh = 8;
			const size_t nb = static_cast<size_t>(gw) * gh;
			std::vector<float> a(static_cast<size_t>(w) * h), b(a.size()), p3(nb * 2), cam(nb * 2), p3Tex(nb * 4), camTex(nb * 4);
			std::vector<uint8_t> camMask(nb), occ(nb);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					// Two regions with different true motion (half px); B(q) = A(q - m).
					const double mx = x < 24 ? 1.3 : -0.75, my = x < 24 ? 0.4 : 1.1;
					a[static_cast<size_t>(y) * w + x] = static_cast<float>(Tex(x, y));
					b[static_cast<size_t>(y) * w + x] = static_cast<float>(Tex(x - mx, y - my));
				}
			for (size_t i = 0; i < nb; ++i)
			{
				const int bx = static_cast<int>(i % gw);
				const double mx = (bx * 4 + 2) < 24 ? 1.3 : -0.75, my = (bx * 4 + 2) < 24 ? 0.4 : 1.1;
				const float nx = static_cast<float>(std::fmod(static_cast<double>(i) * 0.37, 3.0) - 1.5);
				const float ny = static_cast<float>(std::fmod(static_cast<double>(i) * 0.53, 2.0) - 1.0);
				p3[i * 2] = static_cast<float>(2.0 * mx) + nx;
				p3[i * 2 + 1] = static_cast<float>(2.0 * my) + ny;
				const bool invalid = i % 17 == 3;
				occ[i] = i % 11 == 5 ? 1 : 0;
				if (invalid)
				{
					p3[i * 2] = std::numeric_limits<float>::quiet_NaN();
					p3[i * 2 + 1] = 0.0f;
				}
				camMask[i] = i % 3 == 0 ? 1 : 0;
				cam[i * 2] = static_cast<float>(2.0 * mx);
				cam[i * 2 + 1] = static_cast<float>(2.0 * my);
				p3Tex[i * 4] = invalid ? 0.0f : p3[i * 2];
				p3Tex[i * 4 + 1] = p3[i * 2 + 1];
				p3Tex[i * 4 + 2] = invalid ? 0.0f : 1.0f;
				p3Tex[i * 4 + 3] = static_cast<float>(occ[i]);
				camTex[i * 4] = cam[i * 2];
				camTex[i * 4 + 1] = cam[i * 2 + 1];
				camTex[i * 4 + 2] = static_cast<float>(camMask[i]);
			}
			Input in;
			in.lumaA = a.data(); in.lumaB = b.data(); in.w = w; in.h = h; in.gridW = gw; in.gridH = gh;
			in.p3 = p3.data(); in.camera = cam.data(); in.cameraMask = camMask.data(); in.occluded = occ.data();
			const Params prm;
			std::vector<Block> ref;
			RefineField(in, prm, ref);

			pl_tex aTex = MakeTex(gpu, 1, w, h, a.data(), false, false);
			pl_tex bTex = MakeTex(gpu, 1, w, h, b.data(), false, false);
			pl_tex p3t = MakeTex(gpu, 4, gw, gh, p3Tex.data(), false, false);
			pl_tex camt = MakeTex(gpu, 4, gw, gh, camTex.data(), false, false);
			std::vector<float> zero(nb * 4, 0.0f);
			pl_tex outTex = MakeTex(gpu, 4, gw, gh, zero.data(), true, true);
			const int size[2] = { w, h }, grid[2] = { gw, gh };
			const int blockHalf = prm.blockSource / 2, margin = prm.margin, useCam = 1;
			const float thr = prm.boundaryThr, eps = prm.keepEps;
			const pl_shader_var vars[7] = { { pl_var_ivec2("fr_size"), size, true }, { pl_var_ivec2("fr_grid"), grid, true },
				{ pl_var_int("fr_block_half"), &blockHalf, true }, { pl_var_int("fr_margin"), &margin, true },
				{ pl_var_float("fr_boundary_thr"), &thr, true }, { pl_var_float("fr_keep_eps"), &eps, true },
				{ pl_var_int("fr_use_cam"), &useCam, true } };
			const pl_shader_desc desc[5] = { Sampled("fr_a", aTex), Sampled("fr_b", bTex), Sampled("fr_p3", p3t), Sampled("fr_cam", camt),
				Storage("test_out", outTex) };
			static const char* const kBody = R"GLSL(
ivec2 blk = ivec2(gl_GlobalInvocationID.xy);
if (blk.x < fr_grid.x && blk.y < fr_grid.y)
    imageStore(test_out, blk, vpu_field_refine_search(blk));
)GLSL";
			Assert::IsTrue(RunCustom(dispatch, FieldRefineSearchGlsl(), kBody, desc, 5, vars, 7, gw, gh), L"the refine shader failed to compile or run");
			const std::vector<float> got = Download4(gpu, outTex);

			int compared = 0, ties = 0, bad = 0;
			bool anyRefined = false, anyBoundary = false, anyOcc = false, anyInvalid = false;
			for (size_t i = 0; i < nb; ++i)
			{
				const int bx = static_cast<int>(i % gw), by = static_cast<int>(i / gw);
				bool tie = false;
				if (std::isfinite(p3[i * 2]) && !occ[i])
				{
					Surface s3, sc;
					SearchSurface(in, bx, by, blockHalf, margin, p3[i * 2] * 0.5f, p3[i * 2 + 1] * 0.5f, s3);
					auto nearTie = [](const Surface& s)
					{
						const int bi = (s.bestY + kRange) * kSide + (s.bestX + kRange);
						for (int k = 0; k < kCandidates; ++k)
							if (k != bi && s.cost[k] - s.best <= 1e-3f * std::max(s.best, 1.0f))
								return true;
						return false;
					};
					tie = nearTie(s3);
					if (camMask[i])
					{
						SearchSurface(in, bx, by, blockHalf, margin, cam[i * 2] * 0.5f, cam[i * 2 + 1] * 0.5f, sc);
						tie = tie || nearTie(sc) || std::fabs(sc.best - s3.best) <= 1e-3f * std::max(s3.best, 1.0f);
					}
				}
				if (tie) { ++ties; continue; }
				++compared;
				const Block& r = ref[i];
				anyRefined = anyRefined || (r.flags & kRefined) != 0;
				anyBoundary = anyBoundary || (r.flags & kBoundary) != 0;
				anyOcc = anyOcc || (r.flags & kOccluded) != 0;
				anyInvalid = anyInvalid || !std::isfinite(p3[i * 2]);
				const bool ok = std::fabs(got[i * 4] - r.vx) <= 2e-3f && std::fabs(got[i * 4 + 1] - r.vy) <= 2e-3f &&
					std::fabs(got[i * 4 + 2] - static_cast<float>(r.confidence)) <= 1.0f &&
					static_cast<int>(got[i * 4 + 3] + 0.5f) == static_cast<int>(r.flags);
				if (!ok)
				{
					++bad;
					char m[200];
					snprintf(m, sizeof(m), "block %zu: gpu (%.4f %.4f c%.0f f%.0f) ref (%.4f %.4f c%d f%d)\n", i, got[i * 4], got[i * 4 + 1],
						got[i * 4 + 2], got[i * 4 + 3], r.vx, r.vy, r.confidence, r.flags);
					Logger::WriteMessage(m);
				}
			}
			char msg[160];
			snprintf(msg, sizeof(msg), "FIELD_REFINE_GPU blocks=%zu compared=%d ties=%d mismatches=%d\n", nb, compared, ties, bad);
			Logger::WriteMessage(msg);
			pl_tex_destroy(gpu, &aTex); pl_tex_destroy(gpu, &bTex); pl_tex_destroy(gpu, &p3t); pl_tex_destroy(gpu, &camt);
			pl_tex_destroy(gpu, &outTex);
			pl_dispatch_destroy(&dispatch);
			if (bad != 0)
				Fail("the refine shader differs from the CPU reference on " + std::to_string(bad) + " blocks");
			Assert::IsTrue(compared > static_cast<int>(nb / 2), L"too many near-tie blocks: the scene does not test anything");
			Assert::IsTrue(anyRefined && anyBoundary && anyOcc && anyInvalid, L"the test grid must reach refined, boundary, occluded and invalid blocks");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(OffsetShaderMatchesTheCpuAccessor)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OffsetShaderMatchesTheCpuAccessor)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);
			const int gw = 6, gh = 4, bs = 8, w = gw * bs, h = gh * bs;
			std::vector<Block> field(static_cast<size_t>(gw) * gh);
			std::vector<float> tex(field.size() * 4);
			for (size_t i = 0; i < field.size(); ++i)
			{
				field[i].vx = -9.0f + 0.9f * static_cast<float>(i) + static_cast<float>(i % 3);
				field[i].vy = 4.0f - 0.45f * static_cast<float>(i);
				field[i].confidence = 200;
				field[i].flags = i % 7 == 2 ? (kKept | kBoundary) : i % 9 == 4 ? (kOccluded | kKept) : (i % 2 ? kRefined : kKept);
				tex[i * 4] = field[i].vx; tex[i * 4 + 1] = field[i].vy;
				tex[i * 4 + 2] = field[i].confidence; tex[i * 4 + 3] = field[i].flags;
			}
			pl_tex fieldTex = MakeTex(gpu, 4, gw, gh, tex.data(), false, false);
			std::vector<float> zero(static_cast<size_t>(w) * h * 4, 0.0f);
			pl_tex outTex = MakeTex(gpu, 4, w, h, zero.data(), true, true);
			const int grid[2] = { gw, gh }, block = bs;
			const pl_shader_var vars[2] = { { pl_var_ivec2("vpu_field_refine_grid"), grid, true }, { pl_var_int("vpu_field_refine_block"), &block, true } };
			const pl_shader_desc desc[2] = { Sampled("vpu_field_refine_tex", fieldTex), Storage("test_out", outTex) };
			static const char* const kBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < 48 && p.y < 32) {
    vec2 o = vpu_field_refine_offset(vec2(p) + vec2(0.25, 0.75));
    imageStore(test_out, p, vec4(o, 0.0, 0.0));
}
)GLSL";
			Assert::IsTrue(RunCustom(dispatch, FieldRefineOffsetGlsl(), kBody, desc, 2, vars, 2, w, h), L"the offset shader failed");
			const std::vector<float> got = Download4(gpu, outTex);
			int bad = 0;
			double maxAbs = 0.0;
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					float ox = 0, oy = 0;
					Offset(field.data(), gw, gh, bs, static_cast<float>(x) + 0.25f, static_cast<float>(y) + 0.75f, ox, oy);
					const size_t k = (static_cast<size_t>(y) * w + x) * 4;
					const double e = std::max(std::fabs(got[k] - ox), std::fabs(got[k + 1] - oy));
					maxAbs = std::max(maxAbs, e);
					if (e > 1e-4)
						++bad;
				}
			char msg[120];
			snprintf(msg, sizeof(msg), "FIELD_REFINE_OFFSET_GPU pixels=%d mismatches=%d max_abs=%g\n", w * h, bad, maxAbs);
			Logger::WriteMessage(msg);
			pl_tex_destroy(gpu, &fieldTex);
			pl_tex_destroy(gpu, &outTex);
			pl_dispatch_destroy(&dispatch);
			if (bad != 0)
				Fail("the offset accessor differs from the CPU reference on " + std::to_string(bad) + " pixels");
		}
	};
}
