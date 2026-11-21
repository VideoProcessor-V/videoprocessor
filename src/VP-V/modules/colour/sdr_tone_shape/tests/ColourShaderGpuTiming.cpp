// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// G1 (GEM-DV-PERF) before / after timing of the two colour shaders the DV-PERF fixes change: DV trims.glsl (GEM-012, the L8
// and ICtCp paths) and SDR tone shape.glsl (GEM-015, the headroom-only short-circuit). Category GpuHardware: they run on a
// gpu=yes line (VP_TEST_GPU=hardware, VP_TEST_ADAPTER=NVIDIA) and are quoted from the TRX StdOut by the gate, once BEFORE
// and once AFTER the fix commit, on the same laptop on mains with the GPU otherwise idle. Method (plan 3, the speed-up
// method): median of 40 frames with pl_gpu_finish around each, one 3840x2160 hook pass at a time, plus the same render
// without the hook so the pass's own cost is the difference. The timing is logged, never asserted against a bound; what IS
// asserted is that the hook rendered (the log is clean) and changed the picture, so a pass that silently did nothing cannot be
// timed. A missing GPU precondition FAILS under VP_TEST_REQUIRE_GPU=1 (RequireOrSkip), never passes by skipping.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		constexpr int kTimingWidth = 3840, kTimingHeight = 2160, kTimingFrames = 40;

		// Median / minimum wall milliseconds of one render (pl_render_image + pl_gpu_finish) of a 3840x2160 RGBA8 picture through
		// `hook` (null: no hook), over kTimingFrames frames, one warm-up frame first.
		struct Timing { double median = 0.0, minimum = 0.0; };

		inline Timing TimeRender(TargetLutGpuFixture& fixture, const pl_hook* hook)
		{
			pl_gpu gpu = fixture.Gpu();
			pl_fmt format = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, static_cast<enum pl_fmt_caps>(
				PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE));
			Assert::IsNotNull(format, L"No RGBA8 render format is available");
			std::vector<RgbaPixel> pixels(static_cast<size_t>(kTimingWidth) * kTimingHeight);
			for (int y = 0; y < kTimingHeight; ++y)
				for (int x = 0; x < kTimingWidth; ++x)
					pixels[static_cast<size_t>(y) * kTimingWidth + x] = { static_cast<uint8_t>(x * 255 / (kTimingWidth - 1)),
						static_cast<uint8_t>(y * 255 / (kTimingHeight - 1)), static_cast<uint8_t>(((x >> 4) + (y >> 4)) * 37 & 255), 255 };
			pl_tex_params sourceParams{};
			sourceParams.w = kTimingWidth;
			sourceParams.h = kTimingHeight;
			sourceParams.format = format;
			sourceParams.sampleable = true;
			sourceParams.initial_data = pixels.data();
			pl_tex source = pl_tex_create(gpu, &sourceParams);
			Assert::IsNotNull(source);
			pl_tex_params targetParams{};
			targetParams.w = kTimingWidth;
			targetParams.h = kTimingHeight;
			targetParams.format = format;
			targetParams.renderable = true;
			pl_tex target = pl_tex_create(gpu, &targetParams);
			Assert::IsNotNull(target);
			pl_frame image{};
			image.num_planes = 1;
			image.planes[0].texture = source;
			image.planes[0].components = 4;
			for (int c = 0; c < 4; ++c)
				image.planes[0].component_mapping[c] = c;
			image.repr.sys = PL_COLOR_SYSTEM_RGB;
			image.repr.levels = PL_COLOR_LEVELS_FULL;
			image.color.primaries = PL_COLOR_PRIM_BT_709;
			image.color.transfer = PL_COLOR_TRC_SRGB;
			pl_frame out = image;
			out.planes[0].texture = target;
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			if (hook)
			{
				params.hooks = &hook;
				params.num_hooks = 1;
			}
			Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params), L"warm-up frame");
			pl_gpu_finish(gpu);
			std::vector<double> ms;
			for (int k = 0; k < kTimingFrames; ++k)
			{
				pl_gpu_finish(gpu);
				const auto t0 = std::chrono::steady_clock::now();
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
				pl_gpu_finish(gpu);
				ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
			}
			pl_tex_destroy(gpu, &target);
			pl_tex_destroy(gpu, &source);
			std::sort(ms.begin(), ms.end());
			Timing timing;
			timing.median = ms[ms.size() / 2];
			timing.minimum = ms.front();
			return timing;
		}

		inline void LogTiming(const char* test, const char* variant, const Timing& withHook, const Timing& without)
		{
			char line[320];
			std::snprintf(line, sizeof line, "%s %s %dx%d: median %.3f ms with the hook, %.3f ms without (pass cost %.3f ms) over %d frames (min %.3f / %.3f ms)\n",
				test, variant, kTimingWidth, kTimingHeight, withHook.median, without.median, withHook.median - without.median, kTimingFrames,
				withHook.minimum, without.minimum);
			Logger::WriteMessage(line);
		}

		// The hook on the fixture's device (the bundled shader).
		struct BundledHook
		{
			CustomHookChain::Chain chain;
			const pl_hook* hook = nullptr;
			BundledHook(TargetLutGpuFixture& fixture, const char* file, const char* ruleName)
				: chain({
					[](const std::string& name, std::string& source, std::string& reason)
					{
						source = LoadBundledShader(name.c_str());
						reason = "bundled shader not found";
						return !source.empty();
					},
					[&fixture](const std::string& source) -> const pl_hook*
					{
						return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
					},
					[](const pl_hook*& h) { pl_mpv_user_shader_destroy(&h); } })
			{
				ConfiguredShaderRule rule;
				rule.name = ruleName;
				rule.filename = file;
				chain.Configure({ rule });
				CustomHookChain::TemplateValues values;
				values.outputWidth = kTimingWidth;
				values.outputHeight = kTimingHeight;
				chain.Ensure(values);
				std::vector<const pl_hook*> hooks;
				chain.Collect(false, hooks);
				Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected the shader");
				Assert::IsTrue((hooks[0]->stages & PL_HOOK_PRE_OUTPUT) != 0);
				hook = hooks[0];
			}
		};

		// The hook changed a small test picture (it really ran and did something): called before the timing.
		inline void AssertHookActs(TargetLutGpuFixture& fixture, const pl_hook* hook, const wchar_t* what)
		{
			const std::vector<RgbaPixel> before = fixture.RenderCoordinateField(nullptr);
			const std::vector<RgbaPixel> after = fixture.RenderCoordinateField(hook);
			size_t changed = 0;
			for (size_t i = 0; i < before.size(); ++i)
				changed += before[i].r != after[i].r || before[i].g != after[i].g || before[i].b != after[i].b ? 1 : 0;
			Assert::IsTrue(changed * 4 > before.size(), (std::wstring(what) + L": the hook changed the picture").c_str());
		}
	}

	TEST_CLASS(ColourShaderGpuTiming)
	{
	public:
		// GEM-012: DV trims.glsl on a PQ 600-nit BT.2020 output, the L8 path (dv_ext = 1) and the ICtCp path (dv_space = 1, the
		// Dolby-style mode): the per-pixel constants now come from the host (dv_peak_pq, dv_near_black_nits) and unrotated pixels
		// skip cos / sin. Quote BEFORE (the commit before e87f34151) and AFTER from the TRX StdOut.
		BEGIN_TEST_METHOD_ATTRIBUTE(DvTrimsIctcpAndL8GpuTiming)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DvTrimsIctcpAndL8GpuTiming)
		{
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, "DvTrimsIctcpAndL8GpuTiming",
				ExtendedRangeGpuProbe::OnHardwareGpu(), "VP_TEST_GPU=hardware is not set: the timing test runs only on a gpu=yes line"))
				return;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			BundledHook trims(fixture, "DV trims.glsl", "dv_trims");
			const pl_hook* hook = trims.hook;
			DvTrimSelection::ExtParams ext;
			ext.base.slope = 1.1; ext.base.offset = 0.02; ext.base.power = 0.95; ext.base.chromaWeight = 0.15; ext.base.saturationGain = 1.1;
			ext.midContrast = 0.2; ext.clipTrim = -0.15;
			const double sat[6] = { 1.2, 1.0, 0.85, 1.0, 1.15, 1.0 }, hue[6] = { 0.0, 8.0, 0.0, -6.0, 0.0, 4.0 };
			for (int k = 0; k < 6; ++k) { ext.saturation[k] = sat[k]; ext.hueDegrees[k] = hue[k]; }
			const pl_raw_primaries* prim = pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020);
			DolbyVisionTrims::Signal signal;
			signal.pq = true;
			signal.peakNits = 600.0;
			const double r[2] = { prim->red.x, prim->red.y }, g[2] = { prim->green.x, prim->green.y };
			const double b[2] = { prim->blue.x, prim->blue.y }, w[2] = { prim->white.x, prim->white.y };
			DolbyVisionTrims::LuminanceWeights(r, g, b, w, signal.kr, signal.kg, signal.kb);
			const Timing without = TimeRender(fixture, nullptr);
			for (const bool ictcp : { false, true })
			{
				for (const auto& kv : DvTrimSelection::ShaderParameters(ext))
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, kv.first, kv.second));
				for (const auto& kv : DvDolbyStyle::ShaderParameters(ictcp, prim))
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, kv.first, kv.second));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_signal_pq", 1.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_peak_nits", signal.peakNits));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_peak_pq", DolbyVisionTrims::PqFromNits(signal.peakNits)));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_near_black_nits", DolbyVisionTrims::NitsFromPq(DolbyVisionTrims::kNearBlackPq)));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kr", signal.kr));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kb", signal.kb));
				AssertHookActs(fixture, hook, ictcp ? L"ICtCp" : L"L8");
				LogTiming("DvTrimsIctcpAndL8GpuTiming", ictcp ? "ICtCp (Dolby-style)" : "L8 (dv_ext)", TimeRender(fixture, hook), without);
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

		// GEM-015: SDR tone shape.glsl, the headroom-only short-circuit (strength 0) next to the knee-only and knee + recovery
		// forms for context (those two paths change only by the centre-tap reuse). Quote BEFORE (the commit before e41eab639)
		// and AFTER from the TRX StdOut.
		BEGIN_TEST_METHOD_ATTRIBUTE(SdrToneShapeHeadroomOnlyGpuTiming)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SdrToneShapeHeadroomOnlyGpuTiming)
		{
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, "SdrToneShapeHeadroomOnlyGpuTiming",
				ExtendedRangeGpuProbe::OnHardwareGpu(), "VP_TEST_GPU=hardware is not set: the timing test runs only on a gpu=yes line"))
				return;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			BundledHook shape(fixture, "SDR tone shape.glsl", "sdr_tone_shape");
			const pl_hook* hook = shape.hook;
			const Timing without = TimeRender(fixture, nullptr);
			struct Variant { const char* name; double headroom, strength, recovery; };
			const Variant variants[] = {
				{ "headroom only (strength 0)", 0.15, 0.0, 0.0 },
				{ "knee only (strength 0.5)", 0.15, 0.5, 0.0 },
				{ "knee + recovery 1.0", 0.15, 0.5, 1.0 },
			};
			for (const Variant& v : variants)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "headroom", v.headroom));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee_start", 0.8));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "strength", v.strength));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "eotf", 2.2));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "recovery", v.recovery));
				const std::string name = v.name;
				AssertHookActs(fixture, hook, std::wstring(name.begin(), name.end()).c_str());
				LogTiming("SdrToneShapeHeadroomOnlyGpuTiming", v.name, TimeRender(fixture, hook), without);
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}
	};
}
