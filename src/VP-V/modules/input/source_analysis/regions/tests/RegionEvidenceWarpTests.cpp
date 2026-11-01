// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: the stats pass and the fused consumer accessor on the libplacebo WARP device, against
// their CPU float mirrors (StatsBlock<float>, DeriveBlock<float>). WARP unless VP_TEST_GPU=hardware (TargetLutGpuFixture).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidenceGlsl.h>
#include "RegionEvidenceSynthetic.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <cstring>
#include <memory>
#include <string>

namespace VideoProcessorTest
{
	namespace
	{
		using namespace RegionEvidence;
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
			custom.description = "VideoProcessor region evidence test";
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

		constexpr int kW = 256, kH = 144, kB = 16;

		// A frame with every kind of evidence: letterbox, a ringing step, texture, a smooth ramp, grain, a subtitle box and
		// a camera record. transfer selects the visibility law.
		struct Scene
		{
			Plane y;
			GrainSplit::Split split;
			Externals ex;
			std::shared_ptr<CameraModel::BlockData> blocks;
			CameraModel::CameraMotion cam;
		};

		void Build(Scene& s, int transfer)
		{
			s.y = Step(kW, kH, 120, 250.0, 650.0, 25.0);
			for (int yy = 0; yy < kH; ++yy)
				for (int x = 180; x < kW; ++x) s.y.at(x, yy) = 300.0 + 0.4 * x;
			AddTexture(s.y, 20, 60, 100, 110, 30.0, 5.0);
			AddGrain(s.y, 1.5, 91);
			s.ex.picture.left = 0; s.ex.picture.top = 24; s.ex.picture.right = kW; s.ex.picture.bottom = 120;
			Bars(s.y, s.ex.picture);
			Rect box; box.left = 200; box.top = 90; box.right = 240; box.bottom = 110;
			s.ex.overlays.push_back(box);
			s.ex.transfer = transfer;
			s.split = GrainSplit::Compute(ExtendPicture(s.y, s.ex.picture), 1.0, kB);
			s.blocks = std::make_shared<CameraModel::BlockData>();
			s.blocks->gridW = 8; s.blocks->gridH = 5; s.blocks->blockSize = 32;
			s.blocks->label.assign(40, 1);
			s.blocks->tinyMover.assign(40, 0);
			s.blocks->label[9] = 2; s.blocks->tinyMover[9] = 1;
			s.blocks->label[0] = 3;
			s.cam.valid = true;
			s.cam.confidence = 0.72;
			s.cam.blocks = s.blocks;
			s.ex.camera = &s.cam;
		}
	}

	TEST_CLASS(RegionEvidenceWarpTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(StatsPassMatchesTheFloatMirror)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StatsPassMatchesTheFloatMirror)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);
			const int cases[4][2] = { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 0, 0 } };   // (transfer, split bound)
			for (const auto& cs : cases)
			{
				const int transfer = cs[0], haveSplit = cs[1];
				Scene s;
				Build(s, transfer);
				const Grid g = MakeGrid(kW, kH, kB);
				const Rect pic = PictureOrFrame(s.ex.picture, kW, kH);
				const StatsConstants k = MakeStatsConstants(kW, kH, kB, pic, s.ex, haveSplit != 0, VisibilityParams());
				std::vector<float> luma, bands, mirror;
				PackLuma(s.y, luma);
				BandTexelGrid(s.split, pic, bands);
				StatsGridMirror(k, luma.data(), haveSplit ? bands.data() : nullptr, mirror);

				std::vector<float> luma4(luma.size() * 4, 0.0f);   // the luma plane in .x (as a sampled source plane)
				for (size_t i = 0; i < luma.size(); ++i) luma4[i * 4] = luma[i];
				pl_tex src = Rgba32f(gpu, kW, kH, luma4.data(), false);
				pl_tex bandTex = Rgba32f(gpu, g.gw, g.gh, bands.data(), false);
				std::vector<float> zero(g.Count() * 4, 0.0f);
				pl_tex out[kStatsTextures];
				for (int t = 0; t < kStatsTextures; ++t) out[t] = Rgba32f(gpu, g.gw, g.gh, zero.data(), true);
				const int frame[2] = { kW, kH }, lo[2] = { pic.left, pic.top }, hi[2] = { pic.right, pic.bottom };
				const int block = kB, tr = transfer;
				const float vis[4] = { static_cast<float>(k.sdrWhite), static_cast<float>(k.vis.displayPeakNits),
					static_cast<float>(k.vis.kneeShare), static_cast<float>(k.vis.weber) };
				const float dark = static_cast<float>(k.vis.darkNits);
				const pl_shader_var vars[8] = { { pl_var_ivec2("re_frame"), frame, true }, { pl_var_ivec2("re_rect_lo"), lo, true },
					{ pl_var_ivec2("re_rect_hi"), hi, true }, { pl_var_int("re_block"), &block, true },
					{ pl_var_int("re_transfer"), &tr, true }, { pl_var_int("re_have_split"), &haveSplit, true },
					{ pl_var_vec4("re_vis"), vis, true }, { pl_var_float("re_dark"), &dark, true } };
				const pl_shader_desc desc[7] = { Sampled("re_src", src), Sampled("re_bands", bandTex), Storage("re_s0", out[0]),
					Storage("re_s1", out[1]), Storage("re_s2", out[2]), Storage("re_s3", out[3]), Storage("re_s4", out[4]) };
				const std::string header = StatsPassHeaderGlsl();
				std::vector<float> got[2][kStatsTextures];
				for (int run = 0; run < 2; ++run)
				{
					Check(Run(dispatch, header.c_str(), StatsPassGlsl(), desc, 7, vars, 8, g.gw, g.gh), "the stats pass failed");
					for (int t = 0; t < kStatsTextures; ++t) got[run][t] = Download(gpu, out[t]);
				}
				int bad = 0, offFlips = 0, worstChannel = -1;
				double worst = 0.0;
				for (size_t i = 0; i < g.Count(); ++i)
					for (int c = 0; c < kStatsChannels; ++c)
					{
						const double gv = got[0][c / 4][i * 4 + (c % 4)], ref = mirror[i * kStatsChannels + c];
						// Transcendental channels (mean nits, mean PQ, visibility): GPU pow/exp differ by ulps.
						const bool trans = c == kMeanNits || c == kMeanPq || c == kVisibilityStat;
						const double err = std::fabs(gv - ref) / std::max(std::fabs(ref), 1.0);
						if (err > worst) { worst = err; worstChannel = c; }
						if (err > (trans ? 2e-3 : 1e-4))
						{
							// A pixel within an ulp of the off-edge threshold may flip (GPU contraction): tolerated twice.
							if (c == kOffLap || c == kOffShare) ++offFlips;
							else ++bad;
						}
					}
				bool repeat = true;
				for (int t = 0; t < kStatsTextures; ++t)
					repeat = repeat && std::memcmp(got[0][t].data(), got[1][t].data(), got[0][t].size() * sizeof(float)) == 0;
				char msg[200];
				snprintf(msg, sizeof(msg), "REGION_STATS_GPU transfer=%d split=%d blocks=%zu bad=%d off_flips=%d worst=%.2e (channel %d) repeat=%d\n",
					transfer, haveSplit, g.Count(), bad, offFlips, worst, worstChannel, repeat ? 1 : 0);
				Logger::WriteMessage(msg);
				Check(bad == 0 && offFlips <= 4, "stats pass differs from the float mirror on " + std::to_string(bad) + " channels (" +
					std::to_string(offFlips) + " off-edge flips)");
				Check(repeat, "the stats pass is not bit-identical across runs");
				pl_tex_destroy(gpu, &src);
				pl_tex_destroy(gpu, &bandTex);
				for (int t = 0; t < kStatsTextures; ++t) pl_tex_destroy(gpu, &out[t]);
			}
			pl_dispatch_destroy(&dispatch);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(FusedAccessorMatchesTheCpuDerivation)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FusedAccessorMatchesTheCpuDerivation)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);
			Scene s;
			Build(s, static_cast<int>(Transfer::Pq));
			const Grid g = MakeGrid(kW, kH, kB);
			const Rect pic = PictureOrFrame(s.ex.picture, kW, kH);
			const StatsConstants k = MakeStatsConstants(kW, kH, kB, pic, s.ex, true, VisibilityParams());
			std::vector<float> luma, bands, mirror, aux;
			PackLuma(s.y, luma);
			BandTexelGrid(s.split, pic, bands);
			StatsGridMirror(k, luma.data(), bands.data(), mirror);
			PackAux(g, s.ex, aux);
			const size_t n = g.Count();
			std::vector<float> tex[kPrecursorTextures];
			for (int t = 0; t < kPrecursorTextures; ++t)
			{
				tex[t].assign(n * 4, 0.0f);
				for (size_t i = 0; i < n; ++i)
					for (int c = 0; c < 4; ++c)
						tex[t][i * 4 + c] = t < kStatsTextures ? mirror[i * kStatsChannels + t * 4 + c] : aux[i * 4 + c];
			}
			pl_tex in[kPrecursorTextures];
			for (int t = 0; t < kPrecursorTextures; ++t) in[t] = Rgba32f(gpu, g.gw, g.gh, tex[t].data(), false);
			// One column beyond the grid: the accessor must return an invalid (neutral) sample there.
			const int ow = g.gw + 1;
			std::vector<float> zero(static_cast<size_t>(ow) * g.gh * 4, 0.0f);
			pl_tex out[3];
			for (int t = 0; t < 3; ++t) out[t] = Rgba32f(gpu, ow, g.gh, zero.data(), true);

			static const char* const kBody = R"GLSL(
ivec2 blk = ivec2(gl_GlobalInvocationID.xy);
if (blk.x < re_grid.x + 1 && blk.y < re_grid.y) {
    RegionSample r = region_evidence(blk);
    imageStore(test_m0, blk, vec4(r.grain, r.edge, r.edge_contrast, r.ringing));
    imageStore(test_m1, blk, vec4(r.banding, r.texture_energy, r.brightness, r.visibility));
    imageStore(test_m2, blk, vec4(float(r.flags), r.camera_confidence, float(r.camera_label), r.valid ? 1.0 : 0.0));
}
)GLSL";
			const DeriveParams params;
			const AccessorUniforms u = Uniforms(params);
			const int grid[2] = { g.gw, g.gh }, block = kB;
			const pl_shader_var vars[5] = { { pl_var_ivec2("re_grid"), grid, true }, { pl_var_int("re_block"), &block, true },
				{ pl_var_vec4("re_d0"), u.d0, true }, { pl_var_vec4("re_d1"), u.d1, true }, { pl_var_vec4("re_d2"), u.d2, true } };
			const pl_shader_desc desc[9] = { Sampled("re_s0", in[0]), Sampled("re_s1", in[1]), Sampled("re_s2", in[2]),
				Sampled("re_s3", in[3]), Sampled("re_s4", in[4]), Sampled("re_aux", in[5]), Storage("test_m0", out[0]),
				Storage("test_m1", out[1]), Storage("test_m2", out[2]) };
			Check(Run(dispatch, RegionEvidenceGlsl(), kBody, desc, 9, vars, 5, ow, g.gh), "the accessor shader failed");
			std::vector<float> got[3];
			for (int t = 0; t < 3; ++t) got[t] = Download(gpu, out[t]);

			int bad = 0, flagBad = 0, overlay = 0, ringing = 0, banding = 0, bars = 0;
			for (int by = 0; by < g.gh; ++by)
				for (int bx = 0; bx < ow; ++bx)
				{
					const size_t o = (static_cast<size_t>(by) * ow + bx) * 4;
					if (bx == g.gw)
					{
						if (got[2][o + 3] != 0.0f || got[0][o] != 0.0f) ++flagBad;
						continue;
					}
					const size_t i = static_cast<size_t>(by) * g.gw + bx;
					float pre[kPrecursorChannels], m[kMapChannels];
					for (int t = 0; t < kPrecursorTextures; ++t)
						for (int c = 0; c < 4; ++c) pre[t * 4 + c] = tex[t][i * 4 + c];
					DeriveBlock<float>(params, pre, m);
					for (int c = 0; c < kMapChannels; ++c)
					{
						if (c == kFlags || c == kCameraCode || c == kMapPad) continue;
						const double gv = got[c / 4][o + (c % 4)];
						if (std::fabs(gv - m[c]) > 1e-5 * std::max(std::fabs(static_cast<double>(m[c])), 1.0)) ++bad;
					}
					if (got[2][o] != m[kFlags] || got[2][o + 2] != m[kCameraCode] || got[2][o + 3] != 1.0f) ++flagBad;
					const uint32_t f = static_cast<uint32_t>(m[kFlags] + 0.5f);
					overlay += (f & kOverlay) ? 1 : 0;
					bars += (f & kInPicture) ? 0 : 1;
					ringing += m[kRinging] > 0.5f ? 1 : 0;
					banding += m[kBanding] > 0.5f ? 1 : 0;
				}
			char msg[200];
			snprintf(msg, sizeof(msg), "REGION_ACCESSOR_GPU blocks=%zu bad=%d flag_bad=%d overlay=%d bars=%d ringing=%d banding=%d\n", n, bad,
				flagBad, overlay, bars, ringing, banding);
			Logger::WriteMessage(msg);
			Check(bad == 0, "accessor values differ from the CPU derivation on " + std::to_string(bad) + " channels");
			Check(flagBad == 0, "accessor flags / labels / validity differ on " + std::to_string(flagBad) + " blocks");
			Check(overlay > 0 && bars > 0 && ringing > 0 && banding > 0, "the scene must reach overlay, bars, ringing and banding");
			for (int t = 0; t < kPrecursorTextures; ++t) pl_tex_destroy(gpu, &in[t]);
			for (int t = 0; t < 3; ++t) pl_tex_destroy(gpu, &out[t]);
			pl_dispatch_destroy(&dispatch);
		}
	};
}
