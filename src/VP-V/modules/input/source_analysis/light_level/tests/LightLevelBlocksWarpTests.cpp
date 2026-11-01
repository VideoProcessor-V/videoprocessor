// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Light-level block product P12 and the LUT-driven stats pass on the libplacebo WARP device, against their CPU float mirrors
// (StatsBlock<float> with lutTexels, BlockOut<float>, EffectiveAt<float>). WARP unless VP_TEST_GPU=hardware
// (TargetLutGpuFixture); the coordinator runs the hardware tier.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelBlocks.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelBlocksGlsl.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidenceGlsl.h>
#include "../../regions/tests/RegionEvidenceSynthetic.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <cstring>
#include <string>

namespace VideoProcessorTest
{
	namespace
	{
		namespace RE = RegionEvidence;
		namespace LB = LightLevel::Blocks;
		using namespace RegionSynth;

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
			custom.description = "VideoProcessor light level test";
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

		constexpr int kW = 256, kH = 160, kB = 16;

		LightLevel::Settings Defaults()
		{
			return LightLevel::Read([](const std::string&, std::string&) { return false; });
		}

		// A letterboxed frame with a bright/dark layout, a ramp, texture and grain, plus its split and picture rect.
		struct Scene
		{
			Plane y;
			GrainSplit::Split split;
			RE::Externals ex;
			RE::Rect pic;
		};

		void Build(Scene& s)
		{
			Plane c(kW, 96);
			for (int yy = 0; yy < 96; ++yy)
				for (int x = 0; x < kW; ++x)
					c.at(x, yy) = (x < kW / 2 ? 150.0 + 2.2 * x : 520.0 + 1.1 * (x - kW / 2)) + (yy < 48 ? 0.0 : 60.0);
			AddTexture(c, 20, 10, 90, 60, 18.0, 6.0);
			AddGrain(c, 1.2, 77);
			s.y = Plane(kW, kH, 64.0);
			for (int yy = 0; yy < 96; ++yy)
				for (int x = 0; x < kW; ++x) s.y.at(x, yy + 32) = c.at(x, yy);
			s.pic.left = 0; s.pic.top = 32; s.pic.right = kW; s.pic.bottom = 128;
			s.ex.picture = s.pic;
			s.split = GrainSplit::Compute(RE::ExtendPicture(s.y, s.pic), 1.0, kB);
		}

		LightLevel::Context Ctx(LightLevel::Context::Kind kind, double peak)
		{
			LightLevel::Context c;
			c.kind = kind;
			c.sourcePeakNits = peak;
			return c;
		}
	}

	TEST_CLASS(LightLevelBlocksWarpTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(StatsPassWithTheLutMatchesTheFloatMirror)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StatsPassWithTheLutMatchesTheFloatMirror)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);
			const LightLevel::Context::Kind kinds[2] = { LightLevel::Context::Sdr, LightLevel::Context::HdrToSdr };
			for (const auto kind : kinds)
			{
				Scene s;
				Build(s);
				const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(kind, 1000.0));
				const std::vector<float> texels = LightLevel::PackTexels(lut);
				const RE::Grid g = RE::MakeGrid(kW, kH, kB);
				const RE::StatsConstants k = RE::MakeStatsConstants(kW, kH, kB, s.pic, s.ex, true, RE::VisibilityParams(), texels.data());
				std::vector<float> luma, bands, mirror;
				RE::PackLuma(s.y, luma);
				RE::BandTexelGrid(s.split, s.pic, bands);
				RE::StatsGridMirror(k, luma.data(), bands.data(), mirror);

				std::vector<float> luma4(luma.size() * 4, 0.0f);
				for (size_t i = 0; i < luma.size(); ++i) luma4[i * 4] = luma[i];
				pl_tex src = Rgba32f(gpu, kW, kH, luma4.data(), false);
				pl_tex bandTex = Rgba32f(gpu, g.gw, g.gh, bands.data(), false);
				pl_tex lutTex = Rgba32f(gpu, LightLevel::kLutSize, 1, texels.data(), false);
				std::vector<float> zero(g.Count() * 4, 0.0f);
				pl_tex out[RE::kStatsTextures];
				for (int t = 0; t < RE::kStatsTextures; ++t) out[t] = Rgba32f(gpu, g.gw, g.gh, zero.data(), true);
				const int frame[2] = { kW, kH }, lo[2] = { s.pic.left, s.pic.top }, hi[2] = { s.pic.right, s.pic.bottom };
				const int block = kB, haveSplit = 1;
				const pl_shader_var vars[5] = { { pl_var_ivec2("re_frame"), frame, true }, { pl_var_ivec2("re_rect_lo"), lo, true },
					{ pl_var_ivec2("re_rect_hi"), hi, true }, { pl_var_int("re_block"), &block, true },
					{ pl_var_int("re_have_split"), &haveSplit, true } };
				const pl_shader_desc desc[8] = { Sampled("re_src", src), Sampled("re_bands", bandTex), Sampled("light_level_lut", lutTex),
					Storage("re_s0", out[0]), Storage("re_s1", out[1]), Storage("re_s2", out[2]), Storage("re_s3", out[3]),
					Storage("re_s4", out[4]) };
				const std::string header = RE::StatsPassLutHeaderGlsl();
				std::vector<float> got[2][RE::kStatsTextures];
				for (int run = 0; run < 2; ++run)
				{
					Check(Run(dispatch, header.c_str(), RE::StatsPassGlsl(), desc, 8, vars, 5, g.gw, g.gh), "the LUT stats pass failed");
					for (int t = 0; t < RE::kStatsTextures; ++t) got[run][t] = Download(gpu, out[t]);
				}
				int bad = 0, offFlips = 0;
				double worst = 0.0;
				for (size_t i = 0; i < g.Count(); ++i)
					for (int c = 0; c < RE::kStatsChannels; ++c)
					{
						const double gv = got[0][c / 4][i * 4 + (c % 4)], ref = mirror[i * RE::kStatsChannels + c];
						const bool lutChannel = c == RE::kMeanNits || c == RE::kMeanPq || c == RE::kVisibilityStat;
						const double err = std::fabs(gv - ref) / std::max(std::fabs(ref), 1.0);
						if (lutChannel) worst = std::max(worst, err);
						if (err > (lutChannel ? 2e-4 : 1e-4))
						{
							if (c == RE::kOffLap || c == RE::kOffShare) ++offFlips;
							else ++bad;
						}
					}
				bool repeat = true;
				for (int t = 0; t < RE::kStatsTextures; ++t)
					repeat = repeat && std::memcmp(got[0][t].data(), got[1][t].data(), got[0][t].size() * sizeof(float)) == 0;
				char msg[200];
				snprintf(msg, sizeof(msg), "LIGHT_STATS_GPU kind=%d blocks=%zu bad=%d off_flips=%d lut_worst=%.2e repeat=%d\n",
					static_cast<int>(kind), g.Count(), bad, offFlips, worst, repeat ? 1 : 0);
				Logger::WriteMessage(msg);
				Check(bad == 0 && offFlips <= 4, "LUT stats pass differs from the float mirror on " + std::to_string(bad) + " channels");
				Check(repeat, "the LUT stats pass is not bit-identical across runs");
				pl_tex_destroy(gpu, &src);
				pl_tex_destroy(gpu, &bandTex);
				pl_tex_destroy(gpu, &lutTex);
				for (int t = 0; t < RE::kStatsTextures; ++t) pl_tex_destroy(gpu, &out[t]);
			}
			pl_dispatch_destroy(&dispatch);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(BlockPassAndAccessorMatchTheCpuMirror)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BlockPassAndAccessorMatchTheCpuMirror)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);
			Scene s;
			Build(s);
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::HdrToSdr, 1000.0));
			const std::vector<float> texels = LightLevel::PackTexels(lut);
			const RE::Grid g = RE::MakeGrid(kW, kH, kB);
			const RE::StatsConstants k = RE::MakeStatsConstants(kW, kH, kB, s.pic, s.ex, true, RE::VisibilityParams(), texels.data());
			std::vector<float> luma, bands, stats;
			RE::PackLuma(s.y, luma);
			RE::BandTexelGrid(s.split, s.pic, bands);
			RE::StatsGridMirror(k, luma.data(), bands.data(), stats);
			const size_t n = g.Count();

			// P11 textures S0, S3, S4 as the pass reads them.
			std::vector<float> tex[RE::kStatsTextures];
			pl_tex in[RE::kStatsTextures] = {};
			for (int t : { 0, 3, 4 })
			{
				tex[t].assign(n * 4, 0.0f);
				for (size_t i = 0; i < n; ++i)
					for (int c = 0; c < 4; ++c) tex[t][i * 4 + c] = stats[i * RE::kStatsChannels + t * 4 + c];
				in[t] = Rgba32f(gpu, g.gw, g.gh, tex[t].data(), false);
			}
			pl_tex lutTex = Rgba32f(gpu, LightLevel::kLutSize, 1, texels.data(), false);

			LB::Params p = LB::ParamsFor(kW, kH, s.pic, kB);
			p.frameRateHz = 50.0;
			const LB::Kernel kern = LB::KernelFor(p);
			const float alpha = static_cast<float>(LB::TemporalAlpha(p));
			const float frameJ = LB::FrameLevelJ<float>(g.gw, g.gh, stats.data(), RE::kStatsChannels);

			struct Case { const char* name; int mode; int reset; float gate; };
			const Case cases[4] = { { "reset", 0, 1, 1.0f }, { "smoothed", 0, 0, 1.0f }, { "gated", 0, 1, 0.0f }, { "frame level", 1, 1, 1.0f } };
			std::vector<float> prevOut;
			std::vector<float> lastBlocks;
			for (const Case& cs : cases)
			{
				// The previous frame's slot: the reset case's mirror output (any texture of the grid size when resetting).
				std::vector<float> prev(n * 4, 0.0f);
				if (!prevOut.empty() && cs.reset == 0)
					for (size_t i = 0; i < n * 4; ++i) prev[i] = prevOut[i] * 0.8f;   // a different earlier level
				pl_tex prevTex = Rgba32f(gpu, g.gw, g.gh, prev.data(), false);
				std::vector<float> zero(n * 4, 0.0f);
				pl_tex outTex = Rgba32f(gpu, g.gw, g.gh, zero.data(), true);
				const int grid[2] = { g.gw, g.gh }, radius = kern.radius;
				const float sigma = static_cast<float>(kern.sigmaBlocks), frameV = 1.5f, frameConf = static_cast<float>(p.frameLevelConf);
				const pl_shader_var vars[10] = { { pl_var_ivec2("ll_grid"), grid, true }, { pl_var_float("ll_sigma"), &sigma, true },
					{ pl_var_int("ll_radius"), &radius, true }, { pl_var_float("ll_frame_j"), &frameJ, true },
					{ pl_var_float("ll_gate"), &cs.gate, true }, { pl_var_int("ll_reset"), &cs.reset, true },
					{ pl_var_float("ll_alpha"), &alpha, true }, { pl_var_int("ll_mode"), &cs.mode, true },
					{ pl_var_float("ll_frame_v"), &frameV, true }, { pl_var_float("ll_frame_conf"), &frameConf, true } };
				const pl_shader_desc desc[6] = { Sampled("re_s0", in[0]), Sampled("re_s3", in[3]), Sampled("re_s4", in[4]),
					Sampled("ll_prev", prevTex), Sampled("light_level_lut", lutTex), Storage("ll_out", outTex) };
				const std::string header = LB::LightLevelBlocksHeaderGlsl();
				std::vector<float> got[2];
				for (int run = 0; run < 2; ++run)
				{
					Check(Run(dispatch, header.c_str(), LB::LightLevelBlocksPassGlsl(), desc, 6, vars, 10, g.gw, g.gh),
						std::string("the block pass failed: ") + cs.name);
					got[run] = Download(gpu, outTex);
				}
				std::vector<float> mirror(n * 4, 0.0f);
				for (int by = 0; by < g.gh; ++by)
					for (int bx = 0; bx < g.gw; ++bx)
					{
						float o[4];
						if (cs.mode == 1) { o[0] = o[1] = frameJ; o[2] = frameV; o[3] = frameConf; }
						else LB::BlockOut<float>(p, kern, g.gw, g.gh, stats.data(), RE::kStatsChannels, texels.data(), frameJ, cs.gate,
							prev.data(), cs.reset != 0, alpha, bx, by, o);
						for (int c = 0; c < 4; ++c) mirror[(static_cast<size_t>(by) * g.gw + bx) * 4 + c] = o[c];
					}
				int bad = 0, confBlocks = 0, barBlocks = 0;
				double worst = 0.0;
				for (size_t i = 0; i < n * 4; ++i)
				{
					const double err = std::fabs(got[0][i] - mirror[i]) / std::max(std::fabs(static_cast<double>(mirror[i])), 1.0);
					worst = std::max(worst, err);
					if (err > 2e-4) ++bad;
				}
				for (size_t i = 0; i < n; ++i)
				{
					confBlocks += got[0][i * 4 + 3] > 0.0f ? 1 : 0;
					barBlocks += (cs.mode == 0 && got[0][i * 4 + 3] == 0.0f) ? 1 : 0;
				}
				const bool repeat = std::memcmp(got[0].data(), got[1].data(), got[0].size() * sizeof(float)) == 0;
				char msg[220];
				snprintf(msg, sizeof(msg), "LIGHT_BLOCKS_GPU case=%s blocks=%zu bad=%d worst=%.2e conf_blocks=%d zero_conf=%d repeat=%d\n",
					cs.name, n, bad, worst, confBlocks, barBlocks, repeat ? 1 : 0);
				Logger::WriteMessage(msg);
				Check(bad == 0, std::string("block pass differs from the CPU mirror: ") + cs.name);
				Check(repeat, std::string("block pass is not bit-identical across runs: ") + cs.name);
				if (cs.mode == 0 && cs.gate > 0.0f)
					Check(confBlocks > 0 && barBlocks > 0, "the scene must reach picture and bar blocks");
				if (cs.gate == 0.0f && cs.mode == 0) Check(confBlocks == 0, "a gated frame must have confidence 0 everywhere");
				if (cs.reset == 1 && cs.mode == 0 && cs.gate > 0.0f) { prevOut = got[0]; lastBlocks = got[0]; }
				pl_tex_destroy(gpu, &prevTex);
				pl_tex_destroy(gpu, &outTex);
			}

			// The consumer accessor, over pixels inside and outside the frame.
			constexpr int kOw = 80, kOh = 48;
			std::vector<float> blocksIn = lastBlocks;
			pl_tex blockTex = Rgba32f(gpu, g.gw, g.gh, blocksIn.data(), false);
			std::vector<float> zeroOut(static_cast<size_t>(kOw) * kOh * 4, 0.0f);
			pl_tex outTex = Rgba32f(gpu, kOw, kOh, zeroOut.data(), true);
			static const char* const kBody = R"GLSL(
ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
if (gid.x < 80 && gid.y < 48) {
    vec2 px = vec2(gid) * vec2(4.1, 4.3) + vec2(0.25, 0.25);
    float code = float((gid.x * 13 + gid.y * 29) % 1100) * 0.93 + 0.37;
    imageStore(test_out, gid, vpu_light_jeff(px, code));
}
)GLSL";
			const std::string header = LB::LightLevelBlocksHeaderGlsl() + LB::LightLevelBlocksAccessorGlsl();
			const int grid[2] = { g.gw, g.gh }, block = kB;
			const float mix = static_cast<float>(p.surroundMix);
			const pl_shader_var vars[3] = { { pl_var_ivec2("ll_grid"), grid, true }, { pl_var_int("ll_block"), &block, true },
				{ pl_var_float("ll_mix"), &mix, true } };
			const pl_shader_desc desc[3] = { Sampled("ll_blocks", blockTex), Sampled("light_level_lut", lutTex), Storage("test_out", outTex) };
			Check(Run(dispatch, header.c_str(), kBody, desc, 3, vars, 3, kOw, kOh), "the accessor shader failed");
			const std::vector<float> got = Download(gpu, outTex);
			int bad = 0, withConf = 0;
			double worst = 0.0;
			for (int y = 0; y < kOh; ++y)
				for (int x = 0; x < kOw; ++x)
				{
					const float px = static_cast<float>(x) * 4.1f + 0.25f, py = static_cast<float>(y) * 4.3f + 0.25f;
					const float code = static_cast<float>((x * 13 + y * 29) % 1100) * 0.93f + 0.37f;
					const LB::Eff<float> e = LB::EffectiveAt<float>(g.gw, g.gh, kB, blocksIn.data(), texels.data(), px, py, code, mix);
					const float* o = &got[(static_cast<size_t>(y) * kOw + x) * 4];
					const double d0 = std::fabs(o[0] - e.jEff), d1 = std::fabs(o[1] - e.vPixel) / std::max(std::fabs(static_cast<double>(e.vPixel)), 1.0),
						d2 = std::fabs(o[2] - e.conf);
					worst = std::max(worst, std::max(d0, std::max(d1, d2)));
					if (d0 > 5e-4 || d1 > 5e-4 || d2 > 5e-4 || o[3] != 1.0f) ++bad;
					withConf += e.conf > 0.5f ? 1 : 0;
				}
			char msg[160];
			snprintf(msg, sizeof(msg), "LIGHT_ACCESSOR_GPU pixels=%d bad=%d worst=%.2e with_conf=%d\n", kOw * kOh, bad, worst, withConf);
			Logger::WriteMessage(msg);
			Check(bad == 0, "the accessor differs from the CPU mirror on " + std::to_string(bad) + " pixels");
			Check(withConf > 0, "the accessor test must reach confident blocks");
			for (int t : { 0, 3, 4 }) pl_tex_destroy(gpu, &in[t]);
			pl_tex_destroy(gpu, &lutTex);
			pl_tex_destroy(gpu, &blockTex);
			pl_tex_destroy(gpu, &outTex);
			pl_dispatch_destroy(&dispatch);
		}
	};
}
