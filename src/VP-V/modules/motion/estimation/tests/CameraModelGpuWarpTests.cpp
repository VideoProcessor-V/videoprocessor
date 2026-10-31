// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the GPU label pass and the shader accessor on the libplacebo WARP device, against
// their CPU float mirrors (CameraModelGpu.h LabelBlockMirror / CameraAccessor.h FieldVector). WARP unless
// VP_TEST_GPU=hardware (TargetLutGpuFixture).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#include <VP-V/modules/motion/estimation/global_model/CameraAccessorGlsl.h>
#include "CameraModelGpuTestSupport.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

namespace VideoProcessorTest
{
	namespace
	{
		using namespace CameraModel;

		void Check(bool ok, const std::string& what)
		{
			if (ok)
				return;
			Logger::WriteMessage((what + "\n").c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		pl_tex MakeRgba32f(pl_gpu gpu, int w, int h, const float* data, bool storable, bool readable)
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
			params.host_readable = readable;
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

		// A test shader: header (helper functions) + body, one 8x8-group compute dispatch over w x h.
		bool RunCustom(pl_dispatch dispatch, const char* header, const char* body, const pl_shader_desc* desc, int numDesc,
			const pl_shader_var* vars, int numVars, int w, int h)
		{
			pl_custom_shader custom{};
			custom.description = "VideoProcessor camera model test";
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
	}

	TEST_CLASS(CameraModelGpuWarpTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(LabelPassMatchesTheFloatMirror)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LabelPassMatchesTheFloatMirror)
		{
			using namespace CameraSynth;
			using namespace CameraGpuSupport;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			LabelPass pass(fixture.Log(), gpu, true);
			for (const Scenario& s : MakeScenarios())
			{
				const Field& f = *s.field;
				CameraModel::Config cfg;
				cfg.cpuLabels = false;   // the record carries only the grid, the excluded mask and the thresholds
				Fitter fitter(cfg);
				const PairInput in = f.Input(PairSeconds(), 5);
				const CameraMotion r = fitter.Fit(in);
				Check(r.valid && r.blocks != nullptr && r.labelsOnGpu, s.name + ": fit");
				const LabelConstants k = MakeLabelConstants(r, cfg, in);
				std::vector<float> field, aux, mirror;
				PackLabelInputs(in, *r.blocks, field, aux);
				LabelGridMirror(k, field.data(), aux.data(), cfg.overlayZeroClusterMin, mirror);
				const std::vector<uint8_t> band = NearThresholdBand(k, f, mirror);

				pl_tex fieldTex = MakeRgba32f(gpu, k.gridW, k.gridH, field.data(), false, false);
				pl_tex auxTex = MakeRgba32f(gpu, k.gridW, k.gridH, aux.data(), false, false);
				const bool ok = pass.Run(k, fieldTex, auxTex, cfg.overlayZeroClusterMin);
				Check(ok, s.name + ": label pass failed: " + pass.Error());
				const std::vector<float> got = Download(gpu, pass.Output());
				pl_tex_destroy(gpu, &fieldTex);
				pl_tex_destroy(gpu, &auxTex);

				const size_t n = f.Count();
				int codeMismatch = 0, bandBlocks = 0, vectorMismatch = 0;
				for (size_t i = 0; i < n; ++i)
				{
					bandBlocks += band[i];
					for (int c = 0; c < 3; ++c)
					{
						const double a = got[i * 4 + c], b = mirror[i * 4 + c];
						if (std::fabs(a - b) > 1e-5 * std::max(std::fabs(b), 1.0))
							++vectorMismatch;
					}
					if (!band[i] && got[i * 4 + 3] != mirror[i * 4 + 3])
						++codeMismatch;
				}
				char msg[200];
				snprintf(msg, sizeof(msg), "CAMERA_LABEL_GPU %s blocks=%zu band=%d code_mismatch=%d vector_mismatch=%d\n", s.name.c_str(), n,
					bandBlocks, codeMismatch, vectorMismatch);
				Logger::WriteMessage(msg);
				Check(codeMismatch == 0, s.name + ": codes differ from the float mirror: " + std::to_string(codeMismatch));
				Check(vectorMismatch == 0, s.name + ": vectors differ from the float mirror by more than 1e-5: " + std::to_string(vectorMismatch));
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(FieldVectorShaderMatchesTheCpuAccessor)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FieldVectorShaderMatchesTheCpuAccessor)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);

			// A small grid with every kind of block: trusted/untrusted x Camera (agreeing, disagreeing) / Object / Unknown.
			const int gw = 8, gh = 4;
			const size_t n = static_cast<size_t>(gw) * gh;
			auto blocks = std::make_shared<BlockData>();
			blocks->gridW = gw;
			blocks->gridH = gh;
			blocks->blockSize = 16;
			blocks->label.assign(n, 0);
			blocks->resX.assign(n, 0.0f);
			blocks->resY.assign(n, 0.0f);
			blocks->modelX.assign(n, 0.0f);
			blocks->modelY.assign(n, 0.0f);
			blocks->objectId.assign(n, 0);
			blocks->tinyMover.assign(n, 0);
			blocks->excluded.assign(n, 0);
			blocks->absThresholdPx = 0.75f;
			blocks->relThreshold = 0.05f;
			std::vector<float> measured(n * 4), modelTex(n * 4);
			std::vector<uint8_t> trusted(n);
			for (size_t i = 0; i < n; ++i)
			{
				const int kind = static_cast<int>(i % 8);
				const float mx = -20.0f + 0.37f * static_cast<float>(i), my = 3.0f - 0.11f * static_cast<float>(i);
				blocks->modelX[i] = mx + 1.5f;
				blocks->modelY[i] = my - 0.5f;
				Label label = Label::Camera;
				bool tr = true;
				float rx = 0.0f, ry = 0.0f;
				switch (kind)
				{
				case 0: tr = true;  label = Label::Camera;  rx = 0.2f; ry = -0.1f; break;   // agrees: Model/Blend with prefer
				case 1: tr = true;  label = Label::Camera;  rx = 3.0f; ry = 4.0f; break;    // disagrees: Measured
				case 2: tr = true;  label = Label::Object;  break;                          // Measured
				case 3: tr = false; label = Label::Camera;  break;                          // Model/Blend
				case 4: tr = false; label = Label::Object;  break;                          // Hold
				case 5: tr = false; label = Label::Unknown; break;                          // Hold
				case 6: tr = true;  label = Label::Camera;  rx = 0.0f; ry = 0.0f; break;    // agrees
				default: tr = false; label = Label::Camera; break;                          // Model/Blend
				}
				blocks->label[i] = static_cast<uint8_t>(label);
				blocks->resX[i] = rx;
				blocks->resY[i] = ry;
				trusted[i] = tr ? 1 : 0;
				measured[i * 4 + 0] = mx;
				measured[i * 4 + 1] = my;
				measured[i * 4 + 2] = tr ? 1.0f : 0.0f;
				measured[i * 4 + 3] = 0.0f;
				// The LabelPass output layout: model, |res| (float sqrt of the squares), label code.
				modelTex[i * 4 + 0] = blocks->modelX[i];
				modelTex[i * 4 + 1] = blocks->modelY[i];
				modelTex[i * 4 + 2] = std::sqrt(rx * rx + ry * ry);
				modelTex[i * 4 + 3] = static_cast<float>(static_cast<int>(label));
			}
			CameraMotion rec;
			rec.valid = true;
			rec.reason = Reason::Ok;
			rec.blocks = blocks;

			pl_tex measuredTex = MakeRgba32f(gpu, gw, gh, measured.data(), false, false);
			pl_tex modelTexture = MakeRgba32f(gpu, gw, gh, modelTex.data(), false, false);
			std::vector<float> zero(n * 4, 0.0f);
			pl_tex outTex = MakeRgba32f(gpu, gw, gh, zero.data(), true, true);

			static const char* const kBody = R"GLSL(
ivec2 blk = ivec2(gl_GlobalInvocationID.xy);
if (blk.x < camera_grid.x && blk.y < camera_grid.y) {
    vec4 mv = texelFetch(test_measured, blk, 0);
    vec2 o;
    int c = camera_field_vector(blk, mv.z > 0.5, mv.xy, o);
    imageStore(test_out, blk, vec4(o, float(c), 0.0));
}
)GLSL";

			struct Config4 { bool prefer; double confidence; bool enabled; const char* name; };
			const Config4 configs[] = {
				{ false, 0.95, true, "weight 1, measured stays" },
				{ true, 0.95, true, "weight 1, prefer model" },
				{ false, 0.75, true, "weight 0.5, measured stays" },
				{ true, 0.75, true, "weight 0.5, prefer model" },
				{ true, 0.95, false, "disabled" } };
			int outcomes[4] = { 0, 0, 0, 0 };
			for (const Config4& cfgCase : configs)
			{
				Policy p;
				p.enabled = cfgCase.enabled;
				p.preferModelWhenAgrees = cfgCase.prefer;
				p.curve = BlendCurve::Linear;
				p.rateLimit = 1.0;
				rec.confidence = cfgCase.confidence;
				BlendState state;
				state.Update(p, &rec);
				const AccessorConstants ac = MakeAccessorConstants(p, state, &rec);
				const int enabled = ac.enabled, prefer = ac.prefer;
				const float weight = ac.weight, absThr = ac.absThr, relThr = ac.relThr;
				const int grid[2] = { ac.gridW > 0 ? ac.gridW : gw, ac.gridH > 0 ? ac.gridH : gh };
				const pl_shader_var vars[6] = { { pl_var_int("camera_enabled"), &enabled, true }, { pl_var_float("camera_weight"), &weight, true },
					{ pl_var_int("camera_prefer"), &prefer, true }, { pl_var_float("camera_abs_thr"), &absThr, true },
					{ pl_var_float("camera_rel_thr"), &relThr, true }, { pl_var_ivec2("camera_grid"), grid, true } };
				const pl_shader_desc desc[3] = { Sampled("camera_model", modelTexture), Sampled("test_measured", measuredTex),
					Storage("test_out", outTex) };
				Assert::IsTrue(RunCustom(dispatch, CameraFieldVectorGlsl(), kBody, desc, 3, vars, 6, gw, gh), L"the accessor shader failed");
				const std::vector<float> got = Download(gpu, outTex);
				int mismatches = 0;
				for (size_t i = 0; i < n; ++i)
				{
					float ox = 0, oy = 0;
					const Choice c = FieldVector(p, state, &rec, static_cast<int>(i % gw), static_cast<int>(i / gw), trusted[i] != 0,
						measured[i * 4], measured[i * 4 + 1], ox, oy);
					const int gotChoice = static_cast<int>(got[i * 4 + 2] + 0.5f);
					++outcomes[static_cast<int>(c)];
					const bool okVector = std::fabs(got[i * 4] - ox) <= 1e-5f * std::max(std::fabs(ox), 1.0f) &&
						std::fabs(got[i * 4 + 1] - oy) <= 1e-5f * std::max(std::fabs(oy), 1.0f);
					if (gotChoice != static_cast<int>(c) || !okVector)
						++mismatches;
				}
				Check(mismatches == 0, std::string(cfgCase.name) + ": shader and CPU accessor differ on " + std::to_string(mismatches) + " blocks");
			}
			Check(outcomes[0] > 0 && outcomes[1] > 0 && outcomes[2] > 0 && outcomes[3] > 0,
				"the test grid must reach Measured, Model, Blend and Hold");
			pl_tex_destroy(gpu, &measuredTex);
			pl_tex_destroy(gpu, &modelTexture);
			pl_tex_destroy(gpu, &outTex);
			pl_dispatch_destroy(&dispatch);
		}
	};
}
