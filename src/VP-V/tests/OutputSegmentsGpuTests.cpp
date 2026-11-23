#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <VP-V/segments/OutputSegments.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutStage.h>
#include <VP-V/modules/drive/led_measure/LedMeasurementHook.h>
#include <VP-V/modules/output/dither/CustomHookChain.h>
#include <VP-V/modules/output/dither/OutputEncodingRepair.h>
#include <VP-V/modules/output/dither/SigmaDeltaDomain.h>
#include <VP-V/modules/motion/consumers/OcclusionMaskHook.h>
#include <vprenderer/HookSegment.h>

#include <libplacebo/d3d11.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// DRAFT (2026-10-24): GPU fixture for the output segments. WRITTEN, NOT RUN (no GPU tests today:
// the timing gates and the DV suite own the machine). Category "GpuHardware" (the repo's trait name); run it single-threaded with
// /TestCaseFilter:"FullyQualifiedName~OutputSegmentsGpu" (VP_TEST_GPU=hardware) /Logger:trx.
//
// It renders a grey ramp with BTB (code 4) and WTW (code 1019) columns through VP's real S2
// stages (the SDR tone shape and LED compensation GLSL files from shaders\, the LED measurement
// hook and the VP calibration LUT stage with an affine 17^3 LUT):
//   1. commit 1: today's hook list vs the S2 segment's list -> 0 codes (byte-identical);
//   2. commit 2: the separate list vs the merged list (one PRE_OUTPUT hook via HookSegment)
//      -> at most 1 code at 10 bits, and the BTB/WTW columns unchanged.
namespace OutputSegmentsGpuTestsNs
{
	using PipelineSlots::Id;
	using ExtendedRangeGpuProbe::ToWide;

	std::string ShaderDir()
	{
		std::string file = __FILE__;   // ...\src\VP-V\tests\OutputSegmentsGpuTests.cpp
		for (int up = 0; up < 4; ++up)
			file = file.substr(0, file.find_last_of("\\/"));
		return file + "\\shaders\\";
	}

	CustomHookChain::Loader RealLoader(pl_gpu gpu)
	{
		return {
			[](const std::string& name, std::string& source, std::string& reason)
			{
				std::ifstream in(ShaderDir() + name, std::ios::binary);
				if (!in) { reason = "cannot open " + name; return false; }
				std::stringstream ss;
				ss << in.rdbuf();
				source = ss.str();
				return true;
			},
			[gpu](const std::string& source) { return pl_mpv_user_shader_parse(gpu, source.c_str(), source.size()); },
			[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); }
		};
	}

	std::vector<float> AffineLut(int n)
	{
		std::vector<float> d(static_cast<size_t>(n) * n * n * 3);
		size_t k = 0;
		for (int b = 0; b < n; ++b)
			for (int g = 0; g < n; ++g)
				for (int r = 0; r < n; ++r)
				{
					d[k++] = 0.02f + 0.95f * r / (n - 1);
					d[k++] = 0.02f + 0.95f * g / (n - 1);
					d[k++] = 0.02f + 0.95f * b / (n - 1);
				}
		return d;
	}

	ConfiguredShaderRule Rule(const char* name, const char* file)
	{
		ConfiguredShaderRule r;
		r.name = name;
		r.label = name;
		r.filename = file;
		return r;
	}

	TEST_CLASS(OutputSegmentsGpuTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(OutputSegmentsGpuSeparateIsByteIdenticalMergedWithinOneCode)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OutputSegmentsGpuSeparateIsByteIdenticalMergedWithinOneCode)
		{
			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			pl_log log = pl_log_create(PL_API_VER, &logParams);
			std::string device;
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			Logger::WriteMessage(("GPU tests on: " + device + "\n").c_str());
			pl_d3d11 d3d = pl_d3d11_create(log, &deviceParams);
			Assert::IsNotNull(d3d, L"Could not create the libplacebo test device");
			pl_gpu gpu = d3d->gpu;
			pl_renderer renderer = pl_renderer_create(log, gpu);
			{
				CustomHookChain::Chain tone(RealLoader(gpu)), dv(RealLoader(gpu)), comp(RealLoader(gpu));
				tone.Configure({ Rule("sdr_tone_shape", "SDR tone shape.glsl") });
				comp.Configure({ Rule("led_compensation", "LED compensation.glsl") });
				CustomHookChain::TemplateValues values;
				values.outputWidth = 11 * 32;
				values.outputHeight = 16;
				tone.Ensure(values);
				comp.Ensure(values);
				LedDrive::MeasurementHook measure;
				CalibrationLutStage::Hook lut;
				const std::vector<float> lutData = AffineLut(17);
				pl_custom_lut customLut{};
				customLut.signature = 0x5347;
				customLut.size[0] = customLut.size[1] = customLut.size[2] = 17;
				customLut.data = lutData.data();
				lut.SetLut(&customLut);

				OutputSegments::ColourFrame f;
				f.toneShape = true;
				f.tone = { 1.2, 0.6, 0.8, 2.2, 0.0 };
				f.ledBlock = true;
				f.ledMeasure = true;
				f.ledCompensation = true;
				f.compensation.gain = 1.4;
				f.compensation.boost = 1.03;
				f.calibrationLut = true;
				std::vector<const pl_hook*> user;
				f.userPreOutput = &user;

				// Today's assembly (the same modules, in the order RenderLocked pushes them).
				std::vector<const pl_hook*> legacy;
				tone.Collect(false, legacy);
				legacy.push_back(measure.Hook());
				comp.Collect(false, legacy);
				legacy.push_back(lut.Get());
				// The segment's list (sets the same parameters on the same hooks).
				std::vector<const pl_hook*> segmentList;
				OutputSegments::OutputColourSegment s2(tone, dv, measure, comp, lut);
				PipelineSlots::Registry registry;
				s2.Collect(f, registry, segmentList, [](size_t, Id) {});
				Assert::IsTrue(segmentList == legacy, L"commit 1: the segment's hook list differs from today's");

				const std::vector<int> codes = { 4, 16, 40, 64, 100, 300, 600, 900, 940, 980, 1019 };
				const ExtendedRangeGpuProbe::Planes planes = ExtendedRangeGpuProbe::Greys(codes, 32, 16);
				ExtendedRangeGpuProbe::Case c;
				c.outputPrimaries = PL_COLOR_PRIM_BT_709;
				c.extraHooks = legacy;
				const ExtendedRangeGpuProbe::Result separate = ExtendedRangeGpuProbe::Render(gpu, renderer, planes, c);
				c.extraHooks = segmentList;
				const ExtendedRangeGpuProbe::Result moved = ExtendedRangeGpuProbe::Render(gpu, renderer, planes, c);
				Assert::IsTrue(separate.ok && moved.ok, ToWide(separate.error + " / " + moved.error).c_str());
				Assert::AreEqual(0, ExtendedRangeGpuProbe::Compare(separate, moved).max, L"commit 1 must be byte-identical");

				// Note (audit M5): this commit-1 render comparison adds little, since the lists are already
				// asserted equal above; the list equality IS the byte-identity proof. It stays as a smoke
				// test of the segment's parameters on the real shaders.

				// Commit 2: one PRE_OUTPUT hook, internal hand-offs. Both output-pass formats: the fork's
				// output_fbo_float32 on (rgba32f) and off (libplacebo's default), audit M1: the merged
				// hand-offs must take libplacebo's own format either way.
				HookSegment::Runner runner(gpu);
				runner.SetSampleEvery(1);
				for (int outputFloat32 = 1; outputFloat32 >= 0; --outputFloat32)
				{
					c.outputFloat32 = outputFloat32 != 0;
					c.extraHooks = legacy;
					const ExtendedRangeGpuProbe::Result base = ExtendedRangeGpuProbe::Render(gpu, renderer, planes, c);
					std::vector<const pl_hook*> merged;
					runner.Fuse(segmentList, merged, [&](const pl_hook* h) { return s2.Member(h); });
					Assert::AreEqual<size_t>(1, merged.size(), L"the S2 run must become one hook");
					c.extraHooks = merged;
					const ExtendedRangeGpuProbe::Result one = ExtendedRangeGpuProbe::Render(gpu, renderer, planes, c);
					Assert::IsTrue(base.ok && one.ok, ToWide(base.error + " / " + one.error).c_str());
					Assert::IsTrue(runner.TakeFailures().empty(), L"an inner stage failed in the merged hook");
					const pl_render_errors errors = pl_renderer_get_errors(renderer);
					Assert::IsTrue(runner.DisabledRuns(&errors).empty(), L"libplacebo disabled the merged hook (audit H2)");
					const ExtendedRangeGpuProbe::Difference d = ExtendedRangeGpuProbe::Compare(base, one);
					const std::vector<int> a = ExtendedRangeGpuProbe::Columns(base, int(codes.size()), 1.0);
					const std::vector<int> b = ExtendedRangeGpuProbe::Columns(one, int(codes.size()), 1.0);
					const std::string text = std::string(outputFloat32 ? "output fp32: " : "output default: ") +
						"separate " + ExtendedRangeGpuProbe::Join(a) + " | merged " + ExtendedRangeGpuProbe::Join(b) +
						" | max " + std::to_string(d.max) + " mean " + std::to_string(d.mean);
					Logger::WriteMessage(text.c_str());
					Assert::IsTrue(d.max <= 1, ToWide("commit 2 tolerance (<= 1 code): " + text).c_str());
					Assert::AreEqual(a.front(), b.front(), ToWide("BTB column changed: " + text).c_str());
					Assert::AreEqual(a.back(), b.back(), ToWide("WTW column changed: " + text).c_str());
				}
				// S3 merged coverage (CS-E4): see OutputSegmentsGpuMergedS3SigmaDeltaMatchesSeparate below.
				runner.Release();
				lut.Release();
			}
			pl_renderer_destroy(&renderer);
			pl_d3d11_destroy(&d3d);
			pl_log_destroy(&log);
		}

		// Bug hunt 2026-10-31 F2 (WRITTEN, NOT RUN: for the coordinator): the VP-stage calibration
		// LUT carries BTB/WTW through the LUT's edge slope, as the libplacebo stage does. With the
		// affine LUT 0.02 + 0.95 x every column, in range or not, must follow 0.02 + 0.95 x of the
		// render without the LUT (within 1 code). The old slope-1 restore put WTW (code 1019) about
		// 4 codes high and BTB (code 4) about 3 codes low.
		BEGIN_TEST_METHOD_ATTRIBUTE(OutputSegmentsGpuCalibrationLutCarriesBtbWtwThroughItsEdgeSlope)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OutputSegmentsGpuCalibrationLutCarriesBtbWtwThroughItsEdgeSlope)
		{
			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			pl_log log = pl_log_create(PL_API_VER, &logParams);
			std::string device;
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			pl_d3d11 d3d = pl_d3d11_create(log, &deviceParams);
			Assert::IsNotNull(d3d, L"Could not create the libplacebo test device");
			pl_gpu gpu = d3d->gpu;
			pl_renderer renderer = pl_renderer_create(log, gpu);
			{
				CalibrationLutStage::Hook lut;
				const std::vector<float> lutData = AffineLut(17);
				pl_custom_lut customLut{};
				customLut.signature = 0x5348;
				customLut.size[0] = customLut.size[1] = customLut.size[2] = 17;
				customLut.data = lutData.data();
				lut.SetLut(&customLut);
				const std::vector<int> codes = { 4, 16, 40, 64, 300, 600, 940, 980, 1019 };
				const ExtendedRangeGpuProbe::Planes planes = ExtendedRangeGpuProbe::Greys(codes, 32, 16);
				ExtendedRangeGpuProbe::Case c;
				c.outputPrimaries = PL_COLOR_PRIM_BT_709;
				const ExtendedRangeGpuProbe::Result base = ExtendedRangeGpuProbe::Render(gpu, renderer, planes, c);
				c.extraHooks = { lut.Get() };
				// As the renderer does: a texture-input PRE_OUTPUT hook makes libplacebo 7.360 skip the output encoding, so the
				// repair runs first whenever Needed() (never on Beta 377). Without it the hook's result went out unencoded.
				OutputEncodingRepair::Hook repair;
				pl_color_repr target{};
				target.sys = PL_COLOR_SYSTEM_RGB;
				target.levels = PL_COLOR_LEVELS_LIMITED;
				target.bits.sample_depth = 10; target.bits.color_depth = 10;
				repair.SetTarget(target);
				if (OutputEncodingRepair::Needed(c.extraHooks)) c.firstHook = repair.Get();
				const ExtendedRangeGpuProbe::Result mapped = ExtendedRangeGpuProbe::Render(gpu, renderer, planes, c);
				Assert::IsTrue(base.ok && mapped.ok, ToWide(base.error + " / " + mapped.error).c_str());
				Assert::IsTrue(lut.SlopeApplied() > 0 && lut.SlopeFallbacks() == 0, L"the edge slope was not used");
				const std::vector<int> a = ExtendedRangeGpuProbe::Columns(base, int(codes.size()), 1.0);
				const std::vector<int> b = ExtendedRangeGpuProbe::Columns(mapped, int(codes.size()), 1.0);
				std::vector<int> expected;
				for (int code : a)
					expected.push_back(static_cast<int>(std::lround(64.0 + 876.0 * (0.02 + 0.95 * (code - 64) / 876.0))));
				const std::string text = "base " + ExtendedRangeGpuProbe::Join(a) + " | LUT " + ExtendedRangeGpuProbe::Join(b) +
					" | expected " + ExtendedRangeGpuProbe::Join(expected);
				Logger::WriteMessage(text.c_str());
				Assert::IsTrue(a.front() < 64 && a.back() > 940, ToWide("BTB/WTW not carried to the hook: " + text).c_str());
				for (size_t i = 0; i < b.size(); ++i)
					Assert::IsTrue(std::abs(b[i] - expected[i]) <= 1, ToWide("column " + std::to_string(i) + ": " + text).c_str());
				lut.Release();
			}
			pl_renderer_destroy(&renderer);
			pl_d3d11_destroy(&d3d);
			pl_log_destroy(&log);
		}

		// CS-E4: merged S3 (OutputFinishSegment: encoding repair + occlusion mask + real sigma-delta)
		// keeps dither last, runs without disabled hooks/failures, and matches the separate S3 chain.
		BEGIN_TEST_METHOD_ATTRIBUTE(OutputSegmentsGpuMergedS3SigmaDeltaMatchesSeparate)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OutputSegmentsGpuMergedS3SigmaDeltaMatchesSeparate)
		{
			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			pl_log log = pl_log_create(PL_API_VER, &logParams);
			std::string device;
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			Logger::WriteMessage(("GPU tests on: " + device + "\n").c_str());
			pl_d3d11 d3d = pl_d3d11_create(log, &deviceParams);
			Assert::IsNotNull(d3d, L"Could not create the libplacebo test device");
			pl_gpu gpu = d3d->gpu;
			pl_renderer renderer = pl_renderer_create(log, gpu);
			{
				CustomHookChain::Chain sigma(RealLoader(gpu));
				sigma.Configure({ Rule("temporal_sigma_delta", "Temporal sigma-delta.glsl") });
				CustomHookChain::TemplateValues values;
				values.outputWidth = 11 * 32;
				values.outputHeight = 16;
				sigma.Ensure(values);

				OutputEncodingRepair::Hook repair;
				OcclusionMask::Hook mask;
				OutputSegments::OutputFinishSegment s3(repair, mask);
				PipelineSlots::Registry registry;

				std::vector<const pl_hook*> separateHooks;
				sigma.Collect(false, separateHooks);
				Assert::AreEqual<size_t>(1, separateHooks.size(), L"Temporal sigma-delta.glsl must load");
				const pl_hook* sigmaHook = separateHooks[0];

				pl_color_repr targetRepr{};
				targetRepr.sys = PL_COLOR_SYSTEM_RGB;
				targetRepr.levels = PL_COLOR_LEVELS_LIMITED;
				targetRepr.bits.sample_depth = 10;
				targetRepr.bits.color_depth = 10;

				SigmaDeltaDomain::Domain domain{};
				domain.supported = true;
				domain.full = false;
				domain.sample = 10;
				domain.color = 10;

				s3.BeginFrame();
				const OutputSegments::DitherPrep prep = s3.PrepareDither(separateHooks, domain, true, registry);
				Assert::IsTrue(prep.found && !prep.domainDropped && !prep.slotOff, L"sigma-delta prepared");
				const size_t ditherIdx = s3.MarkMask(separateHooks, true, false, 1.0);
				Assert::IsTrue(ditherIdx < separateHooks.size(), L"sigma-delta located before mask insert");
				s3.InsertMask(separateHooks, ditherIdx);
				s3.KeepDitherLast(separateHooks, false);
				Assert::IsTrue(separateHooks.back() == sigmaHook, L"dither must remain last at OUTPUT");
				Assert::IsTrue(s3.Member(mask.Get()) && s3.Member(sigmaHook), L"mask and sigma-delta are S3 members");
				Assert::AreEqual(std::string("occlusion_mask"), std::string(s3.StageName(mask.Get())));
				Assert::AreEqual(std::string("sigma_delta"), std::string(s3.StageName(sigmaHook)));

				const std::vector<int> codes = { 4, 16, 40, 64, 100, 300, 600, 900, 940, 980, 1019 };
				const ExtendedRangeGpuProbe::Planes planes = ExtendedRangeGpuProbe::Greys(codes, 32, 16);
				ExtendedRangeGpuProbe::Case c;
				c.outputPrimaries = PL_COLOR_PRIM_BT_709;
				c.outputFloat32 = true;
				c.extraHooks = separateHooks;
				const ExtendedRangeGpuProbe::Result separate = ExtendedRangeGpuProbe::Render(gpu, renderer, planes, c);

				HookSegment::Runner runner(gpu);
				runner.SetSampleEvery(1);
				std::vector<const pl_hook*> merged;
				runner.Fuse(separateHooks, merged, [&](const pl_hook* h) { return s3.Member(h); });
				Assert::AreEqual<size_t>(1, merged.size(), L"S3 OUTPUT hooks must fuse into one hook");
				c.extraHooks = merged;
				const ExtendedRangeGpuProbe::Result one = ExtendedRangeGpuProbe::Render(gpu, renderer, planes, c);
				Assert::IsTrue(separate.ok && one.ok, ToWide(separate.error + " / " + one.error).c_str());
				Assert::IsTrue(runner.TakeFailures().empty(), L"an S3 inner stage failed in the merged hook");
				const pl_render_errors errors = pl_renderer_get_errors(renderer);
				Assert::IsTrue(runner.DisabledRuns(&errors).empty(), L"libplacebo disabled the merged S3 hook");
				const ExtendedRangeGpuProbe::Difference d = ExtendedRangeGpuProbe::Compare(separate, one);
				const std::vector<int> a = ExtendedRangeGpuProbe::Columns(separate, int(codes.size()), 1.0);
				const std::vector<int> b = ExtendedRangeGpuProbe::Columns(one, int(codes.size()), 1.0);
				const std::string text = "S3 separate " + ExtendedRangeGpuProbe::Join(a) + " | merged " +
					ExtendedRangeGpuProbe::Join(b) + " | max " + std::to_string(d.max) + " mean " + std::to_string(d.mean);
				Logger::WriteMessage((text + "\n").c_str());
				Assert::IsTrue(d.max <= 2, ToWide("S3 merged vs separate within 2 codes (dither tolerance): " + text).c_str());
				Assert::IsTrue(std::abs(a.front() - b.front()) <= 1, ToWide("S3 BTB column preserved: " + text).c_str());
				Assert::IsTrue(std::abs(a.back() - b.back()) <= 1, ToWide("S3 WTW column preserved: " + text).c_str());
				runner.Release();
			}
			pl_renderer_destroy(&renderer);
			pl_d3d11_destroy(&d3d);
			pl_log_destroy(&log);
		}
	};
}
