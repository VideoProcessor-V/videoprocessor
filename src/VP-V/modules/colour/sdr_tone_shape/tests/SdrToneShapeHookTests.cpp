// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		// GEM-015 / GEM-008 (G1 tests): a float readback of a hook render, the shader as a hook, and the pre-change SDR tone shape
		// shader (tests/fixtures) for the A/B checks. The fixture renders 8-bit; the headroom-only short-circuit and the recovery
		// sum order are float-level properties, so these render a float source into a float target.
		inline pl_frame FloatRgbFrame(pl_tex texture)
		{
			pl_frame frame{};
			frame.num_planes = 1;
			frame.planes[0].texture = texture;
			frame.planes[0].components = 4;
			frame.planes[0].component_mapping[0] = 0;
			frame.planes[0].component_mapping[1] = 1;
			frame.planes[0].component_mapping[2] = 2;
			frame.planes[0].component_mapping[3] = 3;
			frame.repr.sys = PL_COLOR_SYSTEM_RGB;
			frame.repr.levels = PL_COLOR_LEVELS_FULL;
			frame.color.primaries = PL_COLOR_PRIM_BT_709;
			frame.color.transfer = PL_COLOR_TRC_SRGB;
			return frame;
		}

		// The pre-change shader the A/B tests compare against: tests/fixtures next to this file. A missing file FAILS the test.
		inline std::string LoadToneShapeFixture(const std::string& name)
		{
			std::string path = __FILE__;
			const size_t separator = path.find_last_of("\\/");
			Assert::IsTrue(separator != std::string::npos);
			path.resize(separator);
			path += "\\fixtures\\" + name;
			std::ifstream input(path, std::ios::binary);
			std::string source{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
			Assert::IsFalse(source.empty(), (std::wstring(L"fixture shader is readable: ") + std::wstring(name.begin(), name.end())).c_str());
			return source;
		}

		// One bundled (or fixture) shader as a libplacebo hook on the fixture's device. Fixture files are named "pre-GEM-015 ...".
		struct ShaderHook
		{
			CustomHookChain::Chain chain;
			const pl_hook* hook = nullptr;

			ShaderHook(TargetLutGpuFixture& fixture, const std::string& file, const char* ruleName)
				: chain({
					[](const std::string& name, std::string& source, std::string& reason)
					{
						source = name.rfind("pre-GEM-015", 0) == 0 ? LoadToneShapeFixture(name) : LoadBundledShader(name.c_str());
						reason = "shader not found";
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
				values.outputWidth = values.outputHeight = 64;
				chain.Ensure(values);
				std::vector<const pl_hook*> hooks;
				chain.Collect(false, hooks);
				Assert::AreEqual(size_t{ 1 }, hooks.size(), (std::wstring(L"libplacebo rejected ") + std::wstring(file.begin(), file.end())).c_str());
				Assert::IsTrue((hooks[0]->stages & PL_HOOK_PRE_OUTPUT) != 0);
				hook = hooks[0];
			}
		};

		// A deterministic textured float RGBA field in [lo, hi] (alpha 1): below-black and above-white values included when the
		// range reaches past 0 / 1.
		inline std::vector<float> TexturedField(int width, int height, uint32_t seed, float lo, float hi)
		{
			std::vector<float> rgba(static_cast<size_t>(width) * height * 4);
			uint32_t state = seed;
			auto next = [&state]() { state = state * 1664525u + 1013904223u; return static_cast<float>((state >> 8) & 0xFFFF) / 65535.0f; };
			for (size_t i = 0; i < rgba.size(); i += 4)
			{
				for (int c = 0; c < 3; ++c)
					rgba[i + c] = lo + (hi - lo) * next();
				rgba[i + 3] = 1.0f;
			}
			return rgba;
		}

		// Renders the float field through `hook` (null: no hook) into a 32-bit float target and reads it back (RGBA floats).
		inline std::vector<float> RenderFloatField(TargetLutGpuFixture& fixture, const pl_hook* hook, int width, int height,
			const std::vector<float>& rgba)
		{
			pl_gpu gpu = fixture.Gpu();
			pl_fmt format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, static_cast<enum pl_fmt_caps>(
				PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(format, L"No host-readable RGBA32F render format is available");
			pl_tex_params sourceParams{};
			sourceParams.w = width;
			sourceParams.h = height;
			sourceParams.format = format;
			sourceParams.sampleable = true;
			sourceParams.initial_data = rgba.data();
			pl_tex source = pl_tex_create(gpu, &sourceParams);
			Assert::IsNotNull(source);
			pl_tex_params targetParams{};
			targetParams.w = width;
			targetParams.h = height;
			targetParams.format = format;
			targetParams.renderable = true;
			targetParams.host_readable = true;
			pl_tex target = pl_tex_create(gpu, &targetParams);
			Assert::IsNotNull(target);
			pl_frame image = FloatRgbFrame(source);
			pl_frame out = FloatRgbFrame(target);
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;   // deterministic readback
			if (hook)
			{
				params.hooks = &hook;
				params.num_hooks = 1;
			}
			Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
			pl_gpu_finish(gpu);
			std::vector<float> result(rgba.size());
			pl_tex_transfer_params download{};
			download.tex = target;
			download.ptr = result.data();
			Assert::IsTrue(pl_tex_download(gpu, &download));
			pl_tex_destroy(gpu, &target);
			pl_tex_destroy(gpu, &source);
			return result;
		}

		inline void SetToneShape(const pl_hook* hook, double headroom, double kneeStart, double strength, double eotf, double recovery)
		{
			Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "headroom", headroom));
			Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee_start", kneeStart));
			Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "strength", strength));
			Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "eotf", eotf));
			Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "recovery", recovery));
		}
	}

	TEST_CLASS(SdrToneShapeHookTests)
	{
	public:


		// Delta 13: the SDR tone shape's reference curve.
		TEST_METHOD(SdrToneShapeReferencePlacesWhiteAndRollsOffIntoTheHeadroom)
		{
			const double g = 2.2;
			auto code = [](double c) { return (c - 16.0) / 219.0; };   // limited-range code -> signal
			// Off: identity (no hook is installed either).
			SdrToneShape::Settings off;
			Assert::IsFalse(SdrToneShape::Active(off));
			for (double c : { 16.0, 100.0, 235.0, 255.0 })
				Assert::AreEqual(code(c), SdrToneShape::Map(code(c), g, 0.0, 0.8, 0.0), 1e-12);
			// Headroom 0.15, no knee: code 235 at 85% of peak in light, 255
			// above it, monotonic, below-black kept (sign-preserving).
			const double white = SdrToneShape::Map(code(235.0), g, 0.15, 0.8, 0.0);
			Assert::AreEqual(0.85, std::pow(white, g), 1e-9);
			const double over = SdrToneShape::Map(code(255.0), g, 0.15, 0.8, 0.0);
			Assert::IsTrue(over > white);
			Assert::IsTrue(SdrToneShape::Map(code(4.0), g, 0.15, 0.8, 0.0) < 0.0, L"below-black kept");
			double last = -1.0;
			for (int c = 0; c <= 255; ++c)
			{
				const double v = SdrToneShape::Map(code(c), g, 0.15, 0.8, 0.0);
				Assert::IsTrue(v > last, L"monotonic");
				last = v;
			}
			// The knee (strength 0.5, headroom 0.15): a 200 -> 255 ramp is smooth
			// (no step larger than its neighbours'), monotonic, and nothing clips:
			// 235 and 255 both stay below peak.
			std::vector<double> ramp;
			for (int c = 200; c <= 255; ++c)
				ramp.push_back(SdrToneShape::Map(code(c), g, 0.15, 0.8, 0.5));
			for (size_t i = 1; i < ramp.size(); ++i)
			{
				Assert::IsTrue(ramp[i] > ramp[i - 1], L"knee monotonic");
				if (i >= 2)
					Assert::IsTrue(ramp[i] - ramp[i - 1] <= (ramp[i - 1] - ramp[i - 2]) * 1.05 + 1e-9,
						L"the knee's steps only shrink (no kink, no clip)");
			}
			Assert::IsTrue(ramp.back() > SdrToneShape::Map(code(235.0), g, 0.15, 0.8, 0.5) && ramp.back() <= SdrToneShape::Map(code(255.0), g, 0.15, 0.8, 0.0) + 1e-12, L"WTW above the shaped white, nothing created");
			Assert::IsTrue(SdrToneShape::Map(code(235.0), g, 0.15, 0.8, 0.5) < white, L"the knee lowers white a little");
			// Slope 1 at the knee start: continuous with the straight line.
			double slope = 0.0;
			SdrToneShape::Knee(0.8, 0.8, 0.5, &slope);
			Assert::AreEqual(1.0, slope, 1e-12);
			SdrToneShape::Knee(0.8 + 1e-6, 0.8, 0.5, &slope);
			Assert::AreEqual(1.0, slope, 1e-5);
			// Strength and recovery as the renderer passes them.
			SdrToneShape::Settings knee;
			knee.knee = true;
			knee.kneeStrength = 0.5;
			Assert::IsTrue(SdrToneShape::Active(knee));
			Assert::AreEqual(0.5, SdrToneShape::EffectiveStrength(knee), 1e-12);
			SdrToneShape::Settings headroomOnly;
			headroomOnly.headroom = 0.15;
			Assert::IsTrue(SdrToneShape::Active(headroomOnly));
			Assert::AreEqual(0.0, SdrToneShape::EffectiveStrength(headroomOnly), 1e-12);
			Assert::AreEqual(0.3, SdrToneShape::EffectiveRecovery(knee, 0.3), 1e-12, L"auto follows the profile");
			knee.recovery = 0.0;
			Assert::AreEqual(0.0, SdrToneShape::EffectiveRecovery(knee, 0.3), 1e-12, L"off");
		}

		// dv_trims (2026-10-17): DV trims.glsl matches DolbyVisionTrims::ApplyPixel
		// on the GPU (PQ and gamma signals, neutral = untouched).
		TEST_METHOD(DolbyVisionTrimShaderMatchesItsReference)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			const std::vector<RgbaPixel> baseline = fixture.RenderCoordinateField(nullptr);
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			ConfiguredShaderRule rule;
			rule.name = "dv_trims";
			rule.filename = "DV trims.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 64;
			chain.Ensure(values);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected DV trims.glsl");
			Assert::IsTrue((hooks[0]->stages & PL_HOOK_PRE_OUTPUT) != 0);
			const pl_hook* hook = hooks[0];
			auto set = [hook](const DolbyVisionTrims::Params& p, const DolbyVisionTrims::Signal& s)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_slope", p.slope));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_offset", p.offset));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_power", p.power));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_chroma_weight", p.chromaWeight));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_sat_gain", p.saturationGain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_signal_pq", s.pq ? 1.0 : 0.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_gamma", s.gamma));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_peak_nits", s.peakNits));
				// GEM-012: the per-pixel constants are host parameters now (the host pushes them next to dv_peak_nits).
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_peak_pq", DolbyVisionTrims::PqFromNits(s.peakNits)));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_near_black_nits", DolbyVisionTrims::NitsFromPq(DolbyVisionTrims::kNearBlackPq)));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kr", s.kr));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kb", s.kb));
			};
			int checked = 0;
			for (const bool pq : { true, false })
			{
				DolbyVisionTrims::Signal signal;
				signal.pq = pq;
				signal.peakNits = pq ? 600.0 : 100.0;
				if (!pq) { signal.kr = 0.2126; signal.kg = 0.7152; signal.kb = 0.0722; }
				else { signal.kr = 0.2627; signal.kg = 0.6780; signal.kb = 0.0593; }
				set(DolbyVisionTrims::Params(), signal);
				const auto unity = fixture.RenderCoordinateField(hook);
				// Neutral = untouched, exactly, on every channel.
				for (size_t i = 0; i < unity.size(); ++i)
					Assert::IsTrue(baseline[i].r == unity[i].r && baseline[i].g == unity[i].g && baseline[i].b == unity[i].b,
						(L"neutral = untouched (exact) at " + std::to_wstring(i) + L": " +
							std::to_wstring(baseline[i].r) + L"," + std::to_wstring(baseline[i].g) + L"," + std::to_wstring(baseline[i].b) + L" vs " +
							std::to_wstring(unity[i].r) + L"," + std::to_wstring(unity[i].g) + L"," + std::to_wstring(unity[i].b)).c_str());
				// A darkening trim, and a lifting one past 1 (the offset fade-in near
				// black and the shoulder below the peak, reviews C2 / S3).
				for (int variant = 0; variant < 2; ++variant)
				{
				DolbyVisionTrims::Params trim;
				if (variant == 0) { trim.slope = 0.85; trim.offset = 0.02; trim.power = 1.15; trim.chromaWeight = 0.2; trim.saturationGain = 1.2; }
				else { trim.slope = 1.3; trim.offset = 0.1; trim.power = 0.9; trim.chromaWeight = -0.3; trim.saturationGain = 1.15; }
				set(trim, signal);
				const auto trimmed = fixture.RenderCoordinateField(hook);
				int changed = 0, loose = 0;
				for (size_t i = 0; i < trimmed.size(); ++i)
				{
					// libplacebo hands PRE_OUTPUT an rgba16f intermediate: the reference
					// takes the same half-precision input (11-bit significand).
					auto half = [](double x)
					{
						if (x == 0.0) return 0.0;
						int e = 0;
						const double m = std::frexp(x, &e);
						return std::ldexp(std::round(m * 2048.0) / 2048.0, e);
					};
					double v[3] = { half(baseline[i].r / 255.0), half(baseline[i].g / 255.0), half(baseline[i].b / 255.0) };
					double scale = 0.0;   // the pixel's light scale (largest channel, nits)
					for (int c = 0; c < 3; ++c)
						scale = (std::max)(scale, std::fabs(DolbyVisionTrims::ToNits(v[c], signal)));
					DolbyVisionTrims::ApplyPixel(v, trim, signal);
					const uint8_t got[3] = { trimmed[i].r, trimmed[i].g, trimmed[i].b };
					for (int c = 0; c < 3; ++c)
					{
						// Within 1.5 codes, or (where out = Y' + gain (L - Y) cancels large
						// numbers into a channel of a few nits, at a gamut edge or under a
						// strong saturation gain, and PQ is steep near black) within
						// 5e-4 of the pixel's light in linear light: the GPU's float PQ
						// maths cannot resolve that cancellation more exactly.
						const double expected = 255.0 * (std::min)(1.0, (std::max)(0.0, v[c]));
						const double diff = std::fabs(expected - static_cast<double>(got[c]));
						if (diff <= 1.5)
							continue;
						const double nitsDiff = std::fabs(DolbyVisionTrims::ToNits(expected / 255.0, signal) -
							DolbyVisionTrims::ToNits(got[c] / 255.0, signal));
						Assert::IsTrue(nitsDiff <= 2e-3 * scale + 1e-3, (std::wstring(pq ? L"PQ " : L"gamma ") + L"variant " +
							std::to_wstring(variant) + L" pixel " + std::to_wstring(i) + L" channel " + std::to_wstring(c) + L": " +
							std::to_wstring(expected) + L" vs " + std::to_wstring(got[c])).c_str());
						++loose;
					}
					changed += got[0] != baseline[i].r || got[1] != baseline[i].g || got[2] != baseline[i].b;
					++checked;
				}
				Assert::IsTrue(changed > static_cast<int>(trimmed.size()) / 2, L"the trim changes the picture");
				Assert::IsTrue(loose * 100 <= static_cast<int>(trimmed.size()) * 3 * 5, L"at most 5% of samples beyond 1.5 codes");
				Logger::WriteMessage((std::string(pq ? "PQ" : "gamma") + " variant " + std::to_string(variant) + ": " +
					std::to_string(loose) + " samples beyond 1.5 codes\n").c_str());
				}
			}
			// libplacebo's known PRE_OUTPUT log noise (the output-encoding bug the
			// renderer repairs), as in the SDR tone shape test.
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
			Logger::WriteMessage(("DV trims shader: " + std::to_string(checked) + " pixels matched the reference\n").c_str());
		}

		// DV trims.glsl: the default inverse-LMS parameters are the inverse of the default
		// forward LMS parameters (CPU only: parses the //!PARAM defaults from the shader text).
		TEST_METHOD(DolbyVisionTrimShaderInverseLmsDefaultsInvertForwardDefaults)
		{
			const std::string src = LoadBundledShader("DV trims.glsl");
			Assert::IsFalse(src.empty(), L"bundled DV trims.glsl not found");
			auto def = [&src](const std::string& name)
			{
				const size_t at = src.find("//!PARAM " + name + "\r\n");
				const size_t at2 = at == std::string::npos ? src.find("//!PARAM " + name + "\n") : at;
				Assert::IsTrue(at2 != std::string::npos, L"PARAM missing");
				size_t pos = at2;
				for (;;)
				{
					pos = src.find('\n', pos) + 1;
					Assert::IsTrue(pos != 0 && pos < src.size(), L"default missing");
					if (src.compare(pos, 3, "//!") != 0)
						break;
				}
				return std::stod(src.substr(pos, 32));
			};
			double f[3][3], g[3][3];
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
				{
					const std::string idx = std::to_string(r) + std::to_string(c);
					f[r][c] = def("dv_lms" + idx);
					g[r][c] = def("dv_ilms" + idx);
				}
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
				{
					double sum = 0.0;
					for (int k = 0; k < 3; ++k)
						sum += g[r][k] * f[k][c];
					Assert::AreEqual(r == c ? 1.0 : 0.0, sum, 1e-5);
				}
		}

		// feat/dv-l8 (audit 2026-10-24): DV trims.glsl's L8 path (dv_ext = 1: mid contrast, clip
		// trim, per-hue saturation / hue) matches DvTrimSelection::ApplyPixel, and its ICtCp path
		// (dv_space = 1, the Dolby-style mode) matches DvDolbyStyle::ApplyPixelIctcp, on the GPU,
		// PQ and gamma outputs, with the same tolerance rule as the L2 test above.
		TEST_METHOD(DolbyVisionTrimShaderL8AndIctcpPathsMatchTheirReferences)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			const std::vector<RgbaPixel> baseline = fixture.RenderCoordinateField(nullptr);
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			ConfiguredShaderRule rule;
			rule.name = "dv_trims";
			rule.filename = "DV trims.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 64;
			chain.Ensure(values);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected DV trims.glsl");
			const pl_hook* hook = hooks[0];
			DvTrimSelection::ExtParams ext;
			ext.base.slope = 1.1; ext.base.offset = 0.02; ext.base.power = 0.95; ext.base.chromaWeight = 0.15; ext.base.saturationGain = 1.1;
			ext.midContrast = 0.2; ext.clipTrim = -0.15;
			const double sat[6] = { 1.2, 1.0, 0.85, 1.0, 1.15, 1.0 }, hue[6] = { 0.0, 8.0, 0.0, -6.0, 0.0, 4.0 };
			for (int k = 0; k < 6; ++k) { ext.saturation[k] = sat[k]; ext.hueDegrees[k] = hue[k]; }
			int checked = 0;
			for (const bool ictcp : { false, true })
				for (const bool pq : { true, false })
				{
					DolbyVisionTrims::Signal signal;
					signal.pq = pq;
					signal.peakNits = pq ? 600.0 : 100.0;
					const pl_raw_primaries* prim = pl_raw_primaries_get(pq ? PL_COLOR_PRIM_BT_2020 : PL_COLOR_PRIM_BT_709);
					const double r[2] = { prim->red.x, prim->red.y }, g[2] = { prim->green.x, prim->green.y };
					const double b[2] = { prim->blue.x, prim->blue.y }, w[2] = { prim->white.x, prim->white.y };
					DolbyVisionTrims::LuminanceWeights(r, g, b, w, signal.kr, signal.kg, signal.kb);
					for (const auto& kv : DvTrimSelection::ShaderParameters(ext))
						Assert::IsTrue(CustomHookChain::SetHookParameter(hook, kv.first, kv.second));
					for (const auto& kv : DvDolbyStyle::ShaderParameters(ictcp, prim))
						Assert::IsTrue(CustomHookChain::SetHookParameter(hook, kv.first, kv.second));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_signal_pq", pq ? 1.0 : 0.0));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_gamma", signal.gamma));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_peak_nits", signal.peakNits));
					// GEM-012: the per-pixel constants are host parameters now (the host pushes them next to dv_peak_nits).
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_peak_pq", DolbyVisionTrims::PqFromNits(signal.peakNits)));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_near_black_nits", DolbyVisionTrims::NitsFromPq(DolbyVisionTrims::kNearBlackPq)));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kr", signal.kr));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kb", signal.kb));
					const DvDolbyStyle::IctcpSignal is = DvDolbyStyle::MakeSignal(signal, r, g, b, w);
					const auto trimmed = fixture.RenderCoordinateField(hook);
					int changed = 0, loose = 0;
					for (size_t i = 0; i < trimmed.size(); ++i)
					{
						auto half = [](double x)
						{
							if (x == 0.0) return 0.0;
							int e = 0;
							const double m = std::frexp(x, &e);
							return std::ldexp(std::round(m * 2048.0) / 2048.0, e);
						};
						double v[3] = { half(baseline[i].r / 255.0), half(baseline[i].g / 255.0), half(baseline[i].b / 255.0) };
						double scale = 0.0;
						for (int c = 0; c < 3; ++c)
							scale = (std::max)(scale, std::fabs(DolbyVisionTrims::ToNits(v[c], signal)));
						if (ictcp)
							DvDolbyStyle::ApplyPixelIctcp(v, ext, is);
						else
							DvTrimSelection::ApplyPixel(v, ext, signal);
						const uint8_t got[3] = { trimmed[i].r, trimmed[i].g, trimmed[i].b };
						for (int c = 0; c < 3; ++c)
						{
							const double expected = 255.0 * (std::min)(1.0, (std::max)(0.0, v[c]));
							const double diff = std::fabs(expected - static_cast<double>(got[c]));
							if (diff <= 1.5)
								continue;
							const double nitsDiff = std::fabs(DolbyVisionTrims::ToNits(expected / 255.0, signal) -
								DolbyVisionTrims::ToNits(got[c] / 255.0, signal));
							Assert::IsTrue(nitsDiff <= 2e-3 * scale + 1e-3, (std::wstring(ictcp ? L"ICtCp " : L"L8 ") + (pq ? L"PQ" : L"gamma") +
								L" pixel " + std::to_wstring(i) + L" channel " + std::to_wstring(c) + L": " +
								std::to_wstring(expected) + L" vs " + std::to_wstring(got[c])).c_str());
							++loose;
						}
						changed += got[0] != baseline[i].r || got[1] != baseline[i].g || got[2] != baseline[i].b;
						++checked;
					}
					Assert::IsTrue(changed > static_cast<int>(trimmed.size()) / 2, L"the trim changes the picture");
					Assert::IsTrue(loose * 100 <= static_cast<int>(trimmed.size()) * 3 * 5, L"at most 5% of samples beyond 1.5 codes");
					Logger::WriteMessage((std::string(ictcp ? "ICtCp " : "L8 ") + (pq ? "PQ" : "gamma") + ": " + std::to_string(loose) +
						" samples beyond 1.5 codes\n").c_str());
				}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
			Logger::WriteMessage(("DV trims shader (L8 / ICtCp): " + std::to_string(checked) + " pixels matched the references\n").c_str());
		}

		// Delta 13: the shader matches its reference on the GPU.
		TEST_METHOD(SdrToneShapeShaderMatchesItsReference)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			const std::vector<RgbaPixel> baseline = fixture.RenderCoordinateField(nullptr);
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			ConfiguredShaderRule rule;
			rule.name = "sdr_tone_shape";
			rule.filename = "SDR tone shape.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 64;
			chain.Ensure(values);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected SDR tone shape.glsl");
			Assert::IsTrue((hooks[0]->stages & PL_HOOK_PRE_OUTPUT) != 0);
			const pl_hook* hook = hooks[0];
			auto set = [hook](double headroom, double strength, double recovery = 0.0, double kneeStart = 0.8)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "headroom", headroom));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee_start", kneeStart));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "strength", strength));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "eotf", 2.2));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "recovery", recovery));
			};
			set(0.0, 0.0);
			const auto unity = fixture.RenderCoordinateField(hook);
			for (size_t i = 0; i < unity.size(); ++i)
			{
				Assert::AreEqual(static_cast<int>(baseline[i].r), static_cast<int>(unity[i].r), L"neutral R = untouched");
				Assert::AreEqual(static_cast<int>(baseline[i].g), static_cast<int>(unity[i].g), L"neutral G = untouched");
				Assert::AreEqual(static_cast<int>(baseline[i].b), static_cast<int>(unity[i].b), L"neutral B = untouched");
			}
			for (const double strength : { 0.0, 0.5, 1.0 })
			{
				set(0.15, strength);
				const auto shaped = fixture.RenderCoordinateField(hook);
				for (size_t x = 0; x < shaped.size(); ++x)
				{
					const double expectedR = 255.0 * SdrToneShape::Map(baseline[x].r / 255.0, 2.2, 0.15, 0.8, strength);
					const double expectedG = 255.0 * SdrToneShape::Map(baseline[x].g / 255.0, 2.2, 0.15, 0.8, strength);
					const double expectedB = 255.0 * SdrToneShape::Map(baseline[x].b / 255.0, 2.2, 0.15, 0.8, strength);
					Assert::AreEqual(expectedR, static_cast<double>(shaped[x].r), 1.5,
						(L"R x " + std::to_wstring(x) + L" strength " + std::to_wstring(strength)).c_str());
					Assert::AreEqual(expectedG, static_cast<double>(shaped[x].g), 1.5,
						(L"G x " + std::to_wstring(x) + L" strength " + std::to_wstring(strength)).c_str());
					Assert::AreEqual(expectedB, static_cast<double>(shaped[x].b), 1.5,
						(L"B x " + std::to_wstring(x) + L" strength " + std::to_wstring(strength)).c_str());
					if (x % 64 != 0)
						Assert::IsTrue(shaped[x].r >= shaped[x - 1].r, L"R monotonic");
					if (x % 64 != 0)
						Assert::IsTrue(shaped[x].b >= shaped[x - 1].b, L"B monotonic");
					if (x >= 64)
						Assert::IsTrue(shaped[x].g >= shaped[x - 64].g, L"G monotonic");
				}
			}
			// Contrast recovery on a smooth ramp: almost no local detail, so the
			// picture stays within a code of the knee alone.
			set(0.15, 0.5, 2.0);
			const auto recovered = fixture.RenderCoordinateField(hook);
			set(0.15, 0.5, 0.0);
			const auto plain = fixture.RenderCoordinateField(hook);
			for (size_t x = 2; x + 2 < plain.size(); ++x)
			{
				Assert::IsTrue(std::abs(static_cast<int>(recovered[x].r) - static_cast<int>(plain[x].r)) <= 1,
					L"recovery adds nothing on a smooth ramp (R)");
				Assert::IsTrue(std::abs(static_cast<int>(recovered[x].g) - static_cast<int>(plain[x].g)) <= 1,
					L"recovery adds nothing on a smooth ramp (G)");
				Assert::IsTrue(std::abs(static_cast<int>(recovered[x].b) - static_cast<int>(plain[x].b)) <= 1,
					L"recovery adds nothing on a smooth ramp (B)");
			}
			// Contrast recovery on a 2-pixel vertical stripe pattern inside the knee (knee_start=0.5):
			// the 5x5 stride-2 blur sees opposite stripes at +-2 px, so recovery=2.0 must raise local contrast.
			{
				pl_gpu gpu = fixture.Gpu();
				const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(
					PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
				pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, caps);
				Assert::IsNotNull(fmt);
				std::vector<RgbaPixel> stripes(64 * 64);
				for (int y = 0; y < 64; ++y)
					for (int x = 0; x < 64; ++x)
					{
						const uint8_t v = ((x / 2) & 1) ? 245 : 215;
						stripes[static_cast<size_t>(y) * 64 + x] = { v, v, v, 255 };
					}
				pl_tex_params sp{}; sp.w = 64; sp.h = 64; sp.format = fmt; sp.sampleable = true; sp.initial_data = stripes.data();
				pl_tex_params tp{}; tp.w = 64; tp.h = 64; tp.format = fmt; tp.renderable = true; tp.host_readable = true;
				pl_tex srcTex = pl_tex_create(gpu, &sp), dstTex = pl_tex_create(gpu, &tp);
				Assert::IsTrue(srcTex && dstTex);
				auto renderStripes = [&](double rec) {
					set(0.15, 0.8, rec, 0.5);
					pl_frame img = TargetLutGpuFixture::MakeRgbFrame(srcTex);
					pl_frame out = TargetLutGpuFixture::MakeRgbFrame(dstTex);
					pl_render_params rp = pl_render_fast_params;
					rp.hooks = &hook;
					rp.num_hooks = 1;
					Assert::IsTrue(pl_render_image(fixture.Renderer(), &img, &out, &rp));
					pl_gpu_finish(gpu);
					std::vector<RgbaPixel> px(64 * 64);
					pl_tex_transfer_params dl{}; dl.tex = dstTex; dl.ptr = px.data();
					Assert::IsTrue(pl_tex_download(gpu, &dl));
					return px;
				};
				const auto sPlain = renderStripes(0.0);
				const auto sRec = renderStripes(2.0);
				pl_tex_destroy(gpu, &srcTex);
				pl_tex_destroy(gpu, &dstTex);
				double plainContrast = 0.0, recContrast = 0.0;
				int pairs = 0;
				for (int y = 8; y < 56; ++y)
					for (int x = 8; x < 56; x += 4)
					{
						const size_t lo = static_cast<size_t>(y) * 64 + x;
						const size_t hi = static_cast<size_t>(y) * 64 + (x + 2);
						plainContrast += static_cast<double>(sPlain[hi].r) - static_cast<double>(sPlain[lo].r);
						recContrast += static_cast<double>(sRec[hi].r) - static_cast<double>(sRec[lo].r);
						++pairs;
					}
				plainContrast /= pairs;
				recContrast /= pairs;
				Assert::IsTrue(recContrast > plainContrast + 1.0,
					(L"contrast recovery must raise stripe contrast inside the knee: plain=" +
						std::to_wstring(plainContrast) + L" rec=" + std::to_wstring(recContrast)).c_str());
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

		// GEM-008: DV trims.glsl on a BT.1886 or sRGB output (dv_trc_kind 1 / 2, dv_trc_param = the BT.1886 b) matches
		// DolbyVisionTrims::ApplyPixel with the same transfer Signal on the GPU, with the tolerance rule of the other DV trims
		// reference tests, and the transfer parameters are what makes the difference (the same trim with dv_trc_kind 0, the
		// pure-power path of the old shader, lands several codes away). The display black is 5 nits on a 100-nit peak (b about
		// 0.4) so the difference is large against the tolerance. Before the fix the shader had no dv_trc_kind / dv_trc_param.
		TEST_METHOD(DolbyVisionTrimShaderBt1886MatchesItsReference)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			const std::vector<RgbaPixel> baseline = fixture.RenderCoordinateField(nullptr);
			ShaderHook trims(fixture, "DV trims.glsl", "dv_trims");
			const pl_hook* hook = trims.hook;
			auto half = [](double x)
			{
				if (x == 0.0) return 0.0;
				int e = 0;
				const double m = std::frexp(x, &e);
				return std::ldexp(std::round(m * 2048.0) / 2048.0, e);   // libplacebo hands PRE_OUTPUT an rgba16f intermediate
			};
			int checked = 0;
			for (const enum pl_color_transfer trc : { PL_COLOR_TRC_BT_1886, PL_COLOR_TRC_SRGB })
			{
				const bool bt1886 = trc == PL_COLOR_TRC_BT_1886;
				const std::wstring name = bt1886 ? L"BT.1886" : L"sRGB";
				pl_color_space colour{};
				colour.primaries = PL_COLOR_PRIM_BT_709;
				colour.transfer = trc;
				colour.hdr.max_luma = 100.0f;
				colour.hdr.min_luma = 5.0f;
				pl_color_space_infer(&colour);
				DolbyVisionTrims::Signal signal;
				signal.pq = false;
				signal.gamma = bt1886 ? 2.4 : 2.2;
				signal.peakNits = 100.0;
				signal.kr = 0.2126; signal.kg = 0.7152; signal.kb = 0.0722;
				Assert::IsTrue(ExtendedRange::DescribeTransfer(colour, true, signal.transfer));
				Assert::IsTrue(signal.transfer.kind == (bt1886 ? ExtendedRange::TransferKind::Bt1886 : ExtendedRange::TransferKind::Srgb));
				if (bt1886)
					Assert::IsTrue(signal.transfer.param > 0.3f, L"a 5-nit black on a 100-nit peak gives a large offset b");
				auto set = [hook](const DolbyVisionTrims::Params& p, const DolbyVisionTrims::Signal& s, double kind)
				{
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_slope", p.slope));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_offset", p.offset));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_power", p.power));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_chroma_weight", p.chromaWeight));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_sat_gain", p.saturationGain));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_signal_pq", 0.0));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_gamma", s.gamma));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_trc_kind", kind));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_trc_param", kind > 0.5 ? static_cast<double>(s.transfer.param) : 0.0));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_peak_nits", s.peakNits));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_peak_pq", DolbyVisionTrims::PqFromNits(s.peakNits)));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_near_black_nits", DolbyVisionTrims::NitsFromPq(DolbyVisionTrims::kNearBlackPq)));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kr", s.kr));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kb", s.kb));
				};
				const double kind = static_cast<double>(static_cast<int>(signal.transfer.kind));
				// Neutral = untouched (within a code: the round trip through the transfer is not bit exact).
				set(DolbyVisionTrims::Params(), signal, kind);
				const auto unity = fixture.RenderCoordinateField(hook);
				for (size_t i = 0; i < unity.size(); ++i)
					Assert::IsTrue(std::abs(static_cast<int>(unity[i].r) - baseline[i].r) <= 1 && std::abs(static_cast<int>(unity[i].g) - baseline[i].g) <= 1 &&
						std::abs(static_cast<int>(unity[i].b) - baseline[i].b) <= 1, (name + L": neutral = untouched at " + std::to_wstring(i)).c_str());
				for (int variant = 0; variant < 2; ++variant)
				{
					DolbyVisionTrims::Params trim;
					if (variant == 0) { trim.slope = 0.85; trim.offset = 0.02; trim.power = 1.15; trim.chromaWeight = 0.2; trim.saturationGain = 1.2; }
					else { trim.slope = 1.3; trim.offset = 0.1; trim.power = 0.9; trim.chromaWeight = -0.3; trim.saturationGain = 1.15; }
					set(trim, signal, kind);
					const auto trimmed = fixture.RenderCoordinateField(hook);
					int changed = 0, loose = 0;
					for (size_t i = 0; i < trimmed.size(); ++i)
					{
						double v[3] = { half(baseline[i].r / 255.0), half(baseline[i].g / 255.0), half(baseline[i].b / 255.0) };
						double scale = 0.0;
						for (int c = 0; c < 3; ++c)
							scale = (std::max)(scale, std::fabs(DolbyVisionTrims::ToNits(v[c], signal)));
						DolbyVisionTrims::ApplyPixel(v, trim, signal);
						const uint8_t got[3] = { trimmed[i].r, trimmed[i].g, trimmed[i].b };
						for (int c = 0; c < 3; ++c)
						{
							const double expected = 255.0 * (std::min)(1.0, (std::max)(0.0, v[c]));
							const double diff = std::fabs(expected - static_cast<double>(got[c]));
							if (diff <= 1.5)
								continue;
							const double nitsDiff = std::fabs(DolbyVisionTrims::ToNits(expected / 255.0, signal) -
								DolbyVisionTrims::ToNits(got[c] / 255.0, signal));
							Assert::IsTrue(nitsDiff <= 2e-3 * scale + 1e-3, (name + L" variant " +
								std::to_wstring(variant) + L" pixel " + std::to_wstring(i) + L" channel " + std::to_wstring(c) + L": " +
								std::to_wstring(expected) + L" vs " + std::to_wstring(got[c])).c_str());
							++loose;
						}
						changed += got[0] != baseline[i].r || got[1] != baseline[i].g || got[2] != baseline[i].b;
						++checked;
					}
					Assert::IsTrue(changed > static_cast<int>(trimmed.size()) / 2, L"the trim changes the picture");
					Assert::IsTrue(loose * 100 <= static_cast<int>(trimmed.size()) * 3 * 5, L"at most 5% of samples beyond 1.5 codes");
					// The transfer parameters are what matters (BT.1886 with its large black: the pure-power path, dv_trc_kind 0, of the
					// same trim is far from it; sRGB is close to a 2.2 power away from its toe, so only the reference match applies).
					int apart = 0;
					if (bt1886)
					{
						set(trim, signal, 0.0);
						const auto asPower = fixture.RenderCoordinateField(hook);
						for (size_t i = 0; i < trimmed.size(); ++i)
							apart += std::abs(static_cast<int>(asPower[i].r) - trimmed[i].r) > 3 || std::abs(static_cast<int>(asPower[i].g) - trimmed[i].g) > 3 ||
								std::abs(static_cast<int>(asPower[i].b) - trimmed[i].b) > 3;
						Assert::IsTrue(apart * 100 >= static_cast<int>(trimmed.size()) * 10, (name +
							L": the transfer curve is what the shader uses, not the pure power (pixels more than 3 codes apart: " + std::to_wstring(apart) + L")").c_str());
					}
					Logger::WriteMessage(((bt1886 ? std::string("BT.1886") : std::string("sRGB")) + " variant " + std::to_string(variant) + ": " +
						std::to_string(loose) + " samples beyond 1.5 codes, " + std::to_string(apart) + " pixels differ from the power path\n").c_str());
				}
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
			Logger::WriteMessage(("DV trims shader (BT.1886 / sRGB): " + std::to_string(checked) + " pixels matched the reference\n").c_str());
		}


		// GEM-015 ([C]:166): the headroom-only short-circuit (strength 0: one factor, (1 - headroom)^(1/eotf), in the signal) equals the
		// full form within one half-float ulp (the FBO is rgba16f: derivation at the check). "The full form" is the pre-change shader (tests/fixtures, the fixture copy: the same
		// maths through the linear-light round trip); both run on the same float source through the same libplacebo pipeline and
		// are read back as 32-bit floats, so what differs is only the algebra. The result also matches the CPU reference
		// SdrToneShape::Map within the fp16 intermediate's precision (the shader acted: it is not an identity). Below-black and
		// above-white source values are included (sign kept).
		TEST_METHOD(SdrToneShapeHeadroomOnlyMatchesTheFullFormWithinAnUlp)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			ShaderHook current(fixture, "SDR tone shape.glsl", "sdr_tone_shape");
			ShaderHook full(fixture, "pre-GEM-015 SDR tone shape.glsl", "sdr_tone_shape");
			const int width = 64, height = 64;
			const std::vector<float> source = TexturedField(width, height, 20261121u, -0.05f, 1.1f);
			const std::vector<float> baseline = RenderFloatField(fixture, nullptr, width, height, source);
			size_t compared = 0, rgb = 0, moved = 0, differing = 0;
			double worst = 0.0;   // in half-float ulps
			for (const double headroom : { 0.05, 0.15, 0.3 })
				for (const double eotf : { 2.2, 2.4 })
					for (const double recovery : { 0.0, 1.5 })   // recovery is ignored without a knee: both forms
					{
						SetToneShape(current.hook, headroom, 0.8, 0.0, eotf, recovery);
						SetToneShape(full.hook, headroom, 0.8, 0.0, eotf, recovery);
						const std::vector<float> a = RenderFloatField(fixture, current.hook, width, height, source);
						const std::vector<float> b = RenderFloatField(fixture, full.hook, width, height, source);
						Assert::AreEqual(b.size(), a.size());
						for (size_t i = 0; i < a.size(); ++i)
						{
							// Tolerance, derived (gate G1 MUST-3): a PRE_OUTPUT hook renders into libplacebo's FBO, which is FLOAT 16 first
							// (an rgba16f intermediate, 11-bit significand), so each output is rounded to a half float. The two shaders differ
							// algebraically by a few float32 ulps (about 1e-7 relative) BEFORE that rounding; after it they either agree or
							// land on neighbouring half floats. The error is therefore at most ONE half-float ulp of the value:
							// ulp16(x) = 2^(floor(log2 |x|) - 10), with the subnormal floor 2^-24 below |x| = 2^-14. The 1e-6 of Gemini's
							// float32 reading cannot survive that rounding, and one half ulp (about 1e-3 relative) is what "within an ulp"
							// means in the precision this pipeline really has. The share of samples that differ AT ALL is bounded below.
							const double reference = static_cast<double>(b[i]);
							const double magnitude = (std::max)(std::fabs(reference), std::fabs(static_cast<double>(a[i])));
							const int exponent = magnitude > 0.0 ? (std::max)(std::ilogb(magnitude), -14) : -14;
							const double halfUlp = std::ldexp(1.0, exponent - 10);
							const double error = std::fabs(static_cast<double>(a[i]) - reference);
							worst = (std::max)(worst, error / halfUlp);
							Assert::IsTrue(error <= halfUlp * 1.0001, (L"headroom " + std::to_wstring(headroom) + L" eotf " + std::to_wstring(eotf) +
								L" sample " + std::to_wstring(i) + L": " + std::to_wstring(a[i]) + L" vs the full form " + std::to_wstring(b[i]) +
								L" (one half-float ulp here is " + std::to_wstring(halfUlp) + L")").c_str());
							differing += error > 0.0 ? 1 : 0;
							++compared;
							if (i % 4 != 3)
							{
								// The short-circuit really shaped the picture, and as the CPU reference says (fp16 intermediate: 3e-3).
								const double expected = SdrToneShape::Map(static_cast<double>(baseline[i]), eotf, headroom, 0.8, 0.0);
								Assert::AreEqual(expected, static_cast<double>(a[i]), 3e-3 * (std::max)(1.0, std::fabs(expected)),
									(L"the CPU reference at sample " + std::to_wstring(i)).c_str());
								++rgb;
								moved += std::fabs(static_cast<double>(a[i]) - static_cast<double>(baseline[i])) > 0.005 ? 1 : 0;
							}
						}
					}
			Assert::IsTrue(compared > 100000, L"the sweep compared real samples");
			Assert::IsTrue(moved * 2 > rgb, L"the shader changed most of the picture (not an identity on both sides)");
			// The short-circuit and the full form agree except where the float32 difference flips a half-float rounding: rare.
			Assert::IsTrue(differing * 100 <= compared, (L"at most 1 percent of the samples differ at all (" + std::to_wstring(differing) + L" of " +
				std::to_wstring(compared) + L")").c_str());
			// Non-vacuity: the shader under test IS the short-circuit form, and the reference is the form without it, so a revert of
			// the fix (current == the pre-change shader) cannot pass this test.
			Assert::IsTrue(LoadBundledShader("SDR tone shape.glsl").find("if (strength <= 0.0) { color.rgb *= pow(1.0 - headroom, 1.0 / eotf); return color; }") != std::string::npos,
				L"the bundled shader carries the headroom-only short-circuit");
			Assert::IsTrue(LoadToneShapeFixture("pre-GEM-015 SDR tone shape.glsl").find("if (strength <= 0.0) { color.rgb *=") == std::string::npos,
				L"the reference shader is the form without it");
			Logger::WriteMessage(("SDR tone shape headroom-only vs the full form: worst error " + std::to_string(worst) + " half-float ulps; " +
				std::to_string(differing) + " of " + std::to_string(compared) + " samples differ at all\n").c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}


		// GEM-015 (AMEND): the recovery blur reuses the centre tap (`(i == 0 && j == 0) ? L : to_linear(fetch)`) and keeps the SUM
		// ORDER (the centre stays the fifth addend; Gemini's `vec3 blur = L` seed moved it to the first and changed the low bits).
		// The output must be bit-identical to the pre-change shader that re-fetches the centre (tests/fixtures: the fixture copy,
		// an opus-reviewed WAIVER), on a textured float field with plenty of values over the knee, for several recovery strengths.
		// The recovery path must actually run (recovery on differs from recovery off), or the identity would be vacuous.
		TEST_METHOD(SdrToneShapeRecoveryIsBitIdenticalToTheRefetch)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			ShaderHook current(fixture, "SDR tone shape.glsl", "sdr_tone_shape");
			ShaderHook refetch(fixture, "pre-GEM-015 SDR tone shape.glsl", "sdr_tone_shape");
			const int width = 64, height = 64;
			const std::vector<float> source = TexturedField(width, height, 7777u, 0.2f, 1.1f);   // a non-flat 3x3 everywhere, most values over the knee
			size_t compared = 0;
			for (const double headroom : { 0.0, 0.05, 0.15 })
				for (const double recovery : { 0.5, 1.0, 2.0 })
				{
					SetToneShape(current.hook, headroom, 0.5, 0.7, 2.2, recovery);
					SetToneShape(refetch.hook, headroom, 0.5, 0.7, 2.2, recovery);
					const std::vector<float> a = RenderFloatField(fixture, current.hook, width, height, source);
					const std::vector<float> b = RenderFloatField(fixture, refetch.hook, width, height, source);
					Assert::AreEqual(b.size(), a.size());
					for (size_t i = 0; i < a.size(); ++i)
					{
						Assert::IsTrue(a[i] == b[i], (L"headroom " + std::to_wstring(headroom) + L" recovery " + std::to_wstring(recovery) +
							L" sample " + std::to_wstring(i) + L": " + std::to_wstring(a[i]) + L" vs the re-fetch " + std::to_wstring(b[i])).c_str());
						++compared;
					}
				}
			Assert::IsTrue(compared >= 9 * 64 * 64 * 4);
			// Non-vacuity: with recovery on, the picture differs from recovery off on many samples (the blur path ran).
			SetToneShape(current.hook, 0.05, 0.5, 0.7, 2.2, 0.0);
			const std::vector<float> off = RenderFloatField(fixture, current.hook, width, height, source);
			SetToneShape(current.hook, 0.05, 0.5, 0.7, 2.2, 1.0);
			const std::vector<float> on = RenderFloatField(fixture, current.hook, width, height, source);
			size_t differing = 0;
			for (size_t i = 0; i < on.size(); ++i)
				differing += i % 4 != 3 && std::fabs(on[i] - off[i]) > 1e-4f ? 1 : 0;
			Assert::IsTrue(differing * 10 > on.size() * 3 / 4, L"contrast recovery acted on at least 10 percent of the samples");
			Logger::WriteMessage(("SDR tone shape recovery: " + std::to_string(compared) + " samples bit-identical to the re-fetch; " +
				std::to_string(differing) + " samples moved by recovery\n").c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

	};
}
