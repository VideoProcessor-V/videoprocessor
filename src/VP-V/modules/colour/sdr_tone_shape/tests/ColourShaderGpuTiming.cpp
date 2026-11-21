// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// G1 (GEM-DV-PERF) measured before / after speed-up of the two colour shaders the DV-PERF fixes change: DV trims.glsl (GEM-012,
// the L8 and ICtCp paths) and SDR tone shape.glsl (GEM-015, the headroom-only short-circuit). Category GpuHardware: they run on a
// gpu=yes line (VP_TEST_GPU=hardware, VP_TEST_ADAPTER=NVIDIA), same laptop, on mains, GPU otherwise idle. Method (plan 3): median
// of 40 frames with pl_gpu_finish around each, one 3840x2160 hook pass at a time, plus the same render without the hook; the
// pass cost is the difference.
//
// BEFORE / AFTER procedure (gate G1 MUST-6, coordinator condition: a speed-up claim needs a measured before and after, and the test
// fails if the speed-up is not there):
//   1. BEFORE: build a SCRATCH (not landed) tree of the commit before the fix (012: c32d00b2e; 015: e41eab639^) with THIS test file
//      copied in, and run the class on a gpu=yes line with VP_TEST_SPEEDUP_RECORD=1. The test is parameter-tolerant (it sets
//      dv_peak_pq / dv_near_black_nits only when the shader declares them and logs which form ran) and logs lines
//        SPEEDUP-RECORD dvtrims_l8_pass_ms=<ms> / dvtrims_ictcp_pass_ms=<ms> / toneshape_headroom_pass_ms=<ms>
//      without asserting anything about speed.
//   2. AFTER: on the fixed tip run the class without VP_TEST_SPEEDUP_RECORD, supplying the recorded numbers as environment variables
//      VP_TEST_BEFORE_DVTRIMS_L8_PASS_MS, VP_TEST_BEFORE_DVTRIMS_ICTCP_PASS_MS, VP_TEST_BEFORE_TONESHAPE_HEADROOM_PASS_MS (or one file
//      named by VP_TEST_SPEEDUP_BEFORE_FILE with key=value lines, same keys lower case). The test ASSERTS the measured pass cost is at
//      least VP_TEST_SPEEDUP_MIN_PCT percent (default 3, the plan's threshold) below the BEFORE number and FAILS otherwise.
//   3. A missing BEFORE number FAILS under VP_TEST_REQUIRE_GPU=1 (RequireOrSkip) and otherwise logs SKIP-NO-GPU. Never a silent pass.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

		// ---- the measured speed-up check (BEFORE supplied by environment / file, see the header) ----
		inline bool EnvText(const char* name, std::string& out)
		{
			char* value = nullptr;
			size_t length = 0;
			const bool have = _dupenv_s(&value, &length, name) == 0 && value && *value;
			if (have) out = value;
			std::free(value);
			return have;
		}

		inline bool BeforeNumber(const std::string& key, double& out)
		{
			std::string upper = key;
			std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			std::string text;
			if (EnvText(("VP_TEST_BEFORE_" + upper).c_str(), text))
			{
				out = std::atof(text.c_str());
				return true;
			}
			std::string path;
			if (EnvText("VP_TEST_SPEEDUP_BEFORE_FILE", path))
			{
				std::ifstream file(path);
				std::string line;
				while (std::getline(file, line))
					if (line.compare(0, key.size() + 1, key + "=") == 0)
					{
						out = std::atof(line.c_str() + key.size() + 1);
						return true;
					}
			}
			return false;
		}

		// Record mode logs the number for the BEFORE run; otherwise the measured pass cost must be at least the minimum percent below
		// the recorded BEFORE number (a missing number FAILS under VP_TEST_REQUIRE_GPU=1).
		inline void CheckSpeedup(const char* test, const std::string& key, double afterPassMs)
		{
			char line[200];
			if (HardwarePrerequisites::EnvIsOne("VP_TEST_SPEEDUP_RECORD"))
			{
				std::snprintf(line, sizeof line, "SPEEDUP-RECORD %s=%.4f\n", key.c_str(), afterPassMs);
				Logger::WriteMessage(line);
				return;
			}
			double before = 0.0;
			const bool have = BeforeNumber(key, before);
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, test, have,
				"no BEFORE number for " + key + " (record it from the pre-fix scratch build: VP_TEST_BEFORE_<KEY> or VP_TEST_SPEEDUP_BEFORE_FILE)"))
				return;
			Assert::IsTrue(before > 0.0, (std::wstring(L"the BEFORE number is positive: ") + std::wstring(key.begin(), key.end())).c_str());
			std::string minimum;
			const double minPercent = EnvText("VP_TEST_SPEEDUP_MIN_PCT", minimum) ? std::atof(minimum.c_str()) : 3.0;
			const double gain = (before - afterPassMs) / before * 100.0;
			std::snprintf(line, sizeof line, "SPEEDUP %s: before %.4f ms, after %.4f ms, gain %.2f %% (required %.2f %%)\n", key.c_str(), before, afterPassMs, gain, minPercent);
			Logger::WriteMessage(line);
			Assert::IsTrue(gain >= minPercent, (std::wstring(L"the measured speed-up is there: ") + std::wstring(key.begin(), key.end()) + L" gained " +
				std::to_wstring(gain) + L" % against the required " + std::to_wstring(minPercent) + L" %").c_str());
		}

		inline bool Declares(const pl_hook* hook, const char* name)
		{
			for (int i = 0; i < hook->num_parameters; ++i)
				if (hook->parameters[i].name && std::strcmp(hook->parameters[i].name, name) == 0)
					return true;
			return false;
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
		// skip cos / sin. Both pass costs are checked against the BEFORE numbers recorded from the c32d00b2e scratch build (see the header).
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
				// Parameter-tolerant: the pre-GEM-012 shader does not declare these two (it recomputes them per pixel), and this test must be
				// able to time that BEFORE shader. They are set when declared (the host does) and the form that ran is logged.
				const bool hostConstants = Declares(hook, "dv_peak_pq") && Declares(hook, "dv_near_black_nits");
				if (hostConstants)
				{
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_peak_pq", DolbyVisionTrims::PqFromNits(signal.peakNits)));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_near_black_nits", DolbyVisionTrims::NitsFromPq(DolbyVisionTrims::kNearBlackPq)));
				}
				Logger::WriteMessage(hostConstants ? "DvTrimsIctcpAndL8GpuTiming: shader form: host constants (dv_peak_pq, dv_near_black_nits)\n" :
					"DvTrimsIctcpAndL8GpuTiming: shader form: pre-GEM-012 (per-pixel constants)\n");
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kr", signal.kr));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kb", signal.kb));
				AssertHookActs(fixture, hook, ictcp ? L"ICtCp" : L"L8");
				const Timing withHook = TimeRender(fixture, hook);
				LogTiming("DvTrimsIctcpAndL8GpuTiming", ictcp ? "ICtCp (Dolby-style)" : "L8 (dv_ext)", withHook, without);
				CheckSpeedup("DvTrimsIctcpAndL8GpuTiming", ictcp ? "dvtrims_ictcp_pass_ms" : "dvtrims_l8_pass_ms", withHook.median - without.median);
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

		// GEM-015: SDR tone shape.glsl, the headroom-only short-circuit (strength 0) next to the knee-only and knee + recovery
		// forms for context (those two paths change only by the centre-tap reuse). The headroom-only pass cost is checked against the
		// BEFORE number recorded from the e41eab639^ scratch build (see the header).
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
				const Timing withHook = TimeRender(fixture, hook);
				LogTiming("SdrToneShapeHeadroomOnlyGpuTiming", v.name, withHook, without);
				if (v.strength == 0.0)   // the headroom-only short-circuit is the speed-up claim; the other two rows are context
					CheckSpeedup("SdrToneShapeHeadroomOnlyGpuTiming", "toneshape_headroom_pass_ms", withHook.median - without.median);
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}
	};
}
