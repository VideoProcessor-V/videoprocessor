// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/focus_map: the GPU pass (FocusMapGlsl.h) on the libplacebo WARP device against its CPU float mirror
// (DefocusBlock<float>, ChangeBlock<float>). WARP unless VP_TEST_GPU=hardware (TargetLutGpuFixture).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/input/source_analysis/focus_map/FocusMap.h>
#include <VP-V/modules/input/source_analysis/focus_map/FocusMapGlsl.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <cstring>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		using namespace FocusMap;

		void Check(bool ok, const std::string& what)
		{
			if (ok) return;
			Logger::WriteMessage((what + "\n").c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		pl_tex Rgba32f(pl_gpu gpu, int w, int h, const float* data, bool storable)
		{
			pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | (storable ? PL_FMT_CAP_STORABLE : 0)));
			Assert::IsNotNull(fmt, L"no rgba32f format");
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = fmt;
			params.sampleable = true;
			params.storable = storable;
			params.host_readable = storable;
			params.initial_data = data;
			pl_tex tex = pl_tex_create(gpu, &params);
			Assert::IsNotNull(tex, L"cannot create the texture");
			return tex;
		}

		std::vector<float> Download(pl_gpu gpu, pl_tex tex)
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

		bool Run(pl_dispatch dispatch, const char* header, const char* body, const pl_shader_desc* desc, int numDesc,
			const pl_shader_var* vars, int numVars, int w, int h)
		{
			pl_custom_shader custom{};
			custom.description = "VideoProcessor focus map test";
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

		uint32_t Lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s >> 8; }
		float Unit(uint32_t& s) { return (Lcg(s) & 0xFFFF) / 65535.0f; }

		TEST_CLASS(FocusMapWarpTests)
		{
		public:
			BEGIN_TEST_METHOD_ATTRIBUTE(PassMatchesTheCpuMirror)
				TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
			END_TEST_METHOD_ATTRIBUTE()
			TEST_METHOD(PassMatchesTheCpuMirror)
			{
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
				pl_gpu gpu = fixture.Gpu();
				pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
				Assert::IsNotNull(dispatch);

				const int gw = 40, gh = 24, block = 16;
				const size_t n = static_cast<size_t>(gw) * gh;
				uint32_t seed = 0x1234567u;
				// Random P11 terms S2 / S4 for this frame and the previous one: every kind of block (uncovered, no split, flat, sharp, blurred).
				std::vector<float> s2(n * 4), s4(n * 4), p2(n * 4), p4(n * 4), motion(n * 4);
				auto fillTerms = [&](std::vector<float>& t2, std::vector<float>& t4) {
					for (size_t i = 0; i < n; ++i)
					{
						const float kind = Unit(seed);
						const bool sharp = Unit(seed) > 0.5f;
						const float scale = kind < 0.15f ? 0.2f : 30.0f;   // flat blocks: below the confidence floor
						t2[i * 4 + 0] = scale * (sharp ? 1.0f : 0.1f) * (0.5f + Unit(seed)) + 2.0f;
						t2[i * 4 + 1] = scale * 0.3f * Unit(seed);
						t2[i * 4 + 2] = scale * (sharp ? 0.1f : 1.0f) * (0.5f + Unit(seed));
						t2[i * 4 + 3] = Unit(seed);
						t4[i * 4 + 0] = kind > 0.93f ? 0.2f : 1.0f;                // coverage
						t4[i * 4 + 1] = Unit(seed);
						t4[i * 4 + 2] = Unit(seed);
						t4[i * 4 + 3] = kind > 0.80f && kind <= 0.88f ? -1.0f : 2.0f * Unit(seed);   // N (-1 = no split)
					}
				};
				fillTerms(s2, s4);
				fillTerms(p2, p4);
				for (size_t i = 0; i < n; ++i)
				{
					const float kind = Unit(seed);
					motion[i * 4 + 0] = (Unit(seed) - 0.5f) * 80.0f;   // reaches outside the grid at the borders
					motion[i * 4 + 1] = (Unit(seed) - 0.5f) * 80.0f;
					motion[i * 4 + 2] = kind < 0.1f ? 0.0f : (kind < 0.3f ? 2.0f : 1.0f);
					motion[i * 4 + 3] = 0.0f;
				}
				Params params;
				const float a[4] = { static_cast<float>(params.ratioLo), static_cast<float>(params.ratioHi), static_cast<float>(params.eps),
					static_cast<float>(params.minCoverage) };
				const float b[4] = { static_cast<float>(params.energyLo), static_cast<float>(params.energyHi), static_cast<float>(params.changeThreshold),
					static_cast<float>(params.minConfidence) };

				pl_tex t2 = Rgba32f(gpu, gw, gh, s2.data(), false), t4 = Rgba32f(gpu, gw, gh, s4.data(), false);
				pl_tex tp2 = Rgba32f(gpu, gw, gh, p2.data(), false), tp4 = Rgba32f(gpu, gw, gh, p4.data(), false);
				pl_tex tm = Rgba32f(gpu, gw, gh, motion.data(), false);
				const std::vector<float> zero(n * 4, 0.0f);
				pl_tex out = Rgba32f(gpu, gw, gh, zero.data(), true);
				const std::string header = FocusMapPassHeaderGlsl();

				int totalBad = 0;
				uint32_t seen = 0;
				for (int mode = 0; mode < 3; ++mode)   // 0: previous + split; 1: no previous; 2: P10 unavailable (forced no split)
				{
					const int grid[2] = { gw, gh }, hasPrev = mode == 1 ? 0 : 1, noSplit = mode == 2 ? 1 : 0;
					const pl_shader_var vars[6] = { { pl_var_ivec2("fm_grid"), grid, true }, { pl_var_int("fm_block"), &block, true },
						{ pl_var_int("fm_has_prev"), &hasPrev, true }, { pl_var_int("fm_no_split"), &noSplit, true },
						{ pl_var_vec4("fm_a"), a, true }, { pl_var_vec4("fm_b"), b, true } };
					const pl_shader_desc desc[6] = { Sampled("fm_s2", t2), Sampled("fm_s4", t4), Sampled("fm_p2", tp2), Sampled("fm_p4", tp4),
						Sampled("fm_motion", tm), Storage("fm_out", out) };
					std::vector<float> got[2];
					for (int run = 0; run < 2; ++run)
					{
						Check(Run(dispatch, header.c_str(), FocusMapPassGlsl(), desc, 6, vars, 6, gw, gh), "the focus pass failed");
						got[run] = Download(gpu, out);
					}
					Check(std::memcmp(got[0].data(), got[1].data(), got[0].size() * sizeof(float)) == 0, "the pass is not bit-identical across runs");
					int bad = 0, flagBad = 0;
					for (int by = 0; by < gh; ++by)
						for (int bx = 0; bx < gw; ++bx)
						{
							const size_t i = static_cast<size_t>(by) * gw + bx;
							auto defocusOf = [&](const std::vector<float>& t2v, const std::vector<float>& t4v, size_t k, float& d, float& c, uint32_t& f) {
								float s[RegionEvidence::kStatsChannels] = {};
								s[RegionEvidence::kE1] = t2v[k * 4]; s[RegionEvidence::kE2] = t2v[k * 4 + 1]; s[RegionEvidence::kE3] = t2v[k * 4 + 2];
								s[RegionEvidence::kCoverage] = t4v[k * 4]; s[RegionEvidence::kStatN] = noSplit ? -1.0f : t4v[k * 4 + 3];
								DefocusBlock<float>(params, s, d, c, f);
							};
							float d, c, delta = 0;
							uint32_t f, fo;
							defocusOf(s2, s4, i, d, c, f);
							const int src = static_cast<int>(motion[i * 4 + 2] + 0.5f);
							if (src == 0) f |= kNoMotion; else if (src == 2) f |= kCameraMotion;
							const float px = (bx + 0.5f) * block + motion[i * 4], py = (by + 0.5f) * block + motion[i * 4 + 1];
							const int sx = static_cast<int>(std::floor(px / block)), sy = static_cast<int>(std::floor(py / block));
							bool pok = hasPrev && sx >= 0 && sy >= 0 && sx < gw && sy < gh;
							float pd = 0, pc = 0;
							if (pok)
							{
								uint32_t pf;
								defocusOf(p2, p4, static_cast<size_t>(sy) * gw + sx, pd, pc, pf);
								pok = (pf & kValid) != 0;
							}
							ChangeBlock<float>(params, d, c, f, pok, pd, pc, delta, fo);
							seen |= fo;
							const float gd = got[0][i * 4], gdelta = got[0][i * 4 + 1], gc = got[0][i * 4 + 2];
							const uint32_t gf = static_cast<uint32_t>(got[0][i * 4 + 3] + 0.5f);
							if (std::fabs(gd - d) > 1e-5f || std::fabs(gdelta - delta) > 1e-5f || std::fabs(gc - c) > 1e-5f) ++bad;
							if (gf != fo)
							{
								// A change decision within an ulp of its threshold may flip (GPU contraction): tolerated.
								const bool nearEdge = std::fabs(std::fabs(delta) - static_cast<float>(params.changeThreshold)) < 1e-4f
									|| std::fabs(std::min(c, pc) - static_cast<float>(params.minConfidence)) < 1e-4f;
								if (!nearEdge) ++flagBad;
							}
						}
					char msg[200];
					snprintf(msg, sizeof(msg), "FOCUS_MAP_GPU mode=%d blocks=%zu bad=%d flag_bad=%d\n", mode, n, bad, flagBad);
					Logger::WriteMessage(msg);
					totalBad += bad + flagBad;
				}
				Check(totalBad == 0, "the focus pass differs from the CPU mirror on " + std::to_string(totalBad) + " values");
				const uint32_t need = kValid | kNoSplit | kNoMotion | kCameraMotion | kNoPrevious | kChanged | kSharper | kLowConfidence;
				Check((seen & need) == need, "the scene must reach every flag (reached " + std::to_string(seen) + ")");
				pl_tex_destroy(gpu, &t2);
				pl_tex_destroy(gpu, &t4);
				pl_tex_destroy(gpu, &tp2);
				pl_tex_destroy(gpu, &tp4);
				pl_tex_destroy(gpu, &tm);
				pl_tex_destroy(gpu, &out);
				pl_dispatch_destroy(&dispatch);
			}
		};
	}
}
