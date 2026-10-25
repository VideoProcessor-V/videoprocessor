// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
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
			auto set = [hook](double headroom, double strength, double recovery = 0.0)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "headroom", headroom));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee_start", 0.8));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "strength", strength));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "eotf", 2.2));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "recovery", recovery));
			};
			set(0.0, 0.0);
			const auto unity = fixture.RenderCoordinateField(hook);
			for (size_t i = 0; i < unity.size(); ++i)
				Assert::AreEqual(static_cast<int>(baseline[i].r), static_cast<int>(unity[i].r), L"neutral = untouched");
			for (const double strength : { 0.0, 0.5, 1.0 })
			{
				set(0.15, strength);
				const auto shaped = fixture.RenderCoordinateField(hook);
				for (size_t x = 0; x < shaped.size(); ++x)
				{
					const double expected = 255.0 * SdrToneShape::Map(baseline[x].r / 255.0, 2.2, 0.15, 0.8, strength);
					Assert::AreEqual(expected, static_cast<double>(shaped[x].r), 1.5,
						(L"x " + std::to_wstring(x) + L" strength " + std::to_wstring(strength)).c_str());
					if (x % 64 != 0)
						Assert::IsTrue(shaped[x].r >= shaped[x - 1].r, L"monotonic");
				}
			}
			// Contrast recovery on a smooth ramp: almost no local detail, so the
			// picture stays within a code of the knee alone.
			set(0.15, 0.5, 2.0);
			const auto recovered = fixture.RenderCoordinateField(hook);
			set(0.15, 0.5, 0.0);
			const auto plain = fixture.RenderCoordinateField(hook);
			for (size_t x = 2; x + 2 < plain.size(); ++x)
				Assert::IsTrue(std::abs(static_cast<int>(recovered[x].r) - static_cast<int>(plain[x].r)) <= 1,
					L"recovery adds nothing on a smooth ramp");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}
	};
}
