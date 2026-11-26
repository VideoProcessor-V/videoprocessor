// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Red-Team GPU hook verification for shaders/vp_cam16_appearance.glsl (S2-B1, S2-H1, S2-H2, CT-H6):
// compiles and dispatches the real bundled PREOUTPUT shader through libplacebo (pl_mpv_user_shader_parse
// + pl_render_image into RGBA32F on WARP unless VP_TEST_GPU=hardware) and checks it against an
// independent CPU CAM16 reference with same-input negative controls.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/colour/appearance/AppearanceStage.h>
#include <VP-V/modules/colour/appearance/Cam16.h>
#include <VP-V/modules/colour/appearance/Cam16Uniforms.h>
#include <VP-V/modules/colour/display_mapping/DvMath.h>
#include <VP-V/modules/colour/extended_range/ExtendedRange.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		// ITU-R BT.2020-2 Table 4 CIE xy primaries (D65 white from DvMath::kD65).
		constexpr double kBt2020Red[2] = { 0.708, 0.292 };
		constexpr double kBt2020Green[2] = { 0.170, 0.797 };
		constexpr double kBt2020Blue[2] = { 0.131, 0.046 };

		inline pl_frame FloatRgbFrame(pl_tex texture, enum pl_color_primaries primaries, enum pl_color_transfer transfer)
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
			frame.color.primaries = primaries;
			frame.color.transfer = transfer;
			return frame;
		}

		struct AppearanceHook
		{
			CustomHookChain::Chain chain;
			const pl_hook* hook = nullptr;

			explicit AppearanceHook(TargetLutGpuFixture& fixture)
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
				rule.name = "cam16_appearance";
				rule.filename = "vp_cam16_appearance.glsl";
				chain.Configure({ rule });
				CustomHookChain::TemplateValues values;
				values.outputWidth = values.outputHeight = 64;
				chain.Ensure(values);
				std::vector<const pl_hook*> hooks;
				chain.Collect(false, hooks);
				Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected vp_cam16_appearance.glsl");
				Assert::IsTrue((hooks[0]->stages & PL_HOOK_PRE_OUTPUT) != 0, L"vp_cam16_appearance.glsl must hook PREOUTPUT");
				hook = hooks[0];
			}
		};

		inline void ApplyParameters(const pl_hook* hook, const AppearanceStage::ParameterList& params)
		{
			Assert::IsFalse(params.empty(), L"AppearanceStage::Decide produced no shader parameters");
			for (const auto& p : params)
			{
				const std::wstring name(p.first, p.first + std::strlen(p.first));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, p.first, p.second),
					(L"SetHookParameter failed for " + name).c_str());
			}
		}

		// Renders a 1D strip of RGBA32F pixels (duplicated to 2 rows) through `hook` with identical source
		// and target colour spaces so libplacebo itself is identity and only the PREOUTPUT hook acts.
		inline std::vector<std::array<float, 3>> RenderFloatPixels(
			TargetLutGpuFixture& fixture,
			const pl_hook* hook,
			enum pl_color_primaries primaries,
			enum pl_color_transfer transfer,
			const std::vector<std::array<float, 3>>& pixels)
		{
			const int width = static_cast<int>(pixels.size());
			const int height = 2;
			Assert::IsTrue(width > 0);

			std::vector<float> rgba(static_cast<size_t>(width) * height * 4, 1.0f);
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
					rgba[idx + 0] = pixels[static_cast<size_t>(x)][0];
					rgba[idx + 1] = pixels[static_cast<size_t>(x)][1];
					rgba[idx + 2] = pixels[static_cast<size_t>(x)][2];
					rgba[idx + 3] = 1.0f;
				}
			}

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
			Assert::IsNotNull(source, L"Failed to create RGBA32F source texture");

			pl_tex_params targetParams{};
			targetParams.w = width;
			targetParams.h = height;
			targetParams.format = format;
			targetParams.renderable = true;
			targetParams.host_readable = true;
			pl_tex target = pl_tex_create(gpu, &targetParams);
			Assert::IsNotNull(target, L"Failed to create RGBA32F target texture");

			pl_frame image = FloatRgbFrame(source, primaries, transfer);
			pl_frame out = FloatRgbFrame(target, primaries, transfer);
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			if (hook)
			{
				params.hooks = &hook;
				params.num_hooks = 1;
			}
			Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params), L"pl_render_image failed");
			pl_gpu_finish(gpu);

			std::vector<float> downloaded(rgba.size(), 0.0f);
			pl_tex_transfer_params download{};
			download.tex = target;
			download.ptr = downloaded.data();
			Assert::IsTrue(pl_tex_download(gpu, &download), L"pl_tex_download failed");

			pl_tex_destroy(gpu, &target);
			pl_tex_destroy(gpu, &source);

			std::vector<std::array<float, 3>> result(static_cast<size_t>(width));
			for (int x = 0; x < width; ++x)
			{
				const size_t idx = static_cast<size_t>(x) * 4;
				result[static_cast<size_t>(x)] = { { downloaded[idx + 0], downloaded[idx + 1], downloaded[idx + 2] } };
			}
			return result;
		}

		inline std::array<double, 3> MulRowMajor3(const double m[9], const std::array<double, 3>& v)
		{
			return { {
				m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
				m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
				m[6] * v[0] + m[7] * v[1] + m[8] * v[2]
			} };
		}

		inline double FlareReference(double o, double f, double upper)
		{
			if (!(f > 0.0))
				return o;
			o = (std::max)(0.0, o);
			return (o * (o + f) / (o + 2.0 * f)) * ((upper + 2.0 * f) / (upper + f));
		}

		inline double Smoothstep(double edge0, double edge1, double x)
		{
			if (!(edge1 > edge0))
				return x >= edge1 ? 1.0 : 0.0;
			const double t = (std::min)(1.0, (std::max)(0.0, (x - edge0) / (edge1 - edge0)));
			return t * t * (3.0 - 2.0 * t);
		}

		// Independent CPU reference for the CAM16 appearance stage using Cam16::Forward / Inverse
		// and DvMath PQ / matrix maths directly. When `simulatePreFixSdrCeiling` is true, runs the
		// broken pre-fix baseline (upper = 1.0 gamut bisection + SDR-normalized flare) on the exact
		// same input pixel for in-test negative controls.
		inline std::array<double, 3> CpuReferenceAppearancePixel(
			const AppearanceStage::Inputs& in,
			const AppearanceStage::Decision& d,
			const std::array<double, 3>& rgbIn,
			double veilOverride = -1.0,
			bool simulatePreFixSdrCeiling = false,
			double huntScaleOverride = -1.0)
		{
			if (!(in.strength > 0.0))
				return rgbIn;

			const std::array<double, 3> inRange = { {
				(std::min)(1.0, (std::max)(0.0, rgbIn[0])),
				(std::min)(1.0, (std::max)(0.0, rgbIn[1])),
				(std::min)(1.0, (std::max)(0.0, rgbIn[2]))
			} };
			const std::array<double, 3> excess = { {
				rgbIn[0] - inRange[0],
				rgbIn[1] - inRange[1],
				rgbIn[2] - inRange[2]
			} };
			const bool outside = excess[0] != 0.0 || excess[1] != 0.0 || excess[2] != 0.0;
			const bool allBelow = rgbIn[0] <= 0.0 && rgbIn[1] <= 0.0 && rgbIn[2] <= 0.0;
			const bool allAbove = rgbIn[0] >= 1.0 && rgbIn[1] >= 1.0 && rgbIn[2] >= 1.0;
			// W23-NEON-H3 (3ad95bc94): only wholly-BTB bypasses raw; wholly-WTW adapts (1,1,1) and adds excess for C0 corner continuity.
			if (outside && (allBelow || (simulatePreFixSdrCeiling && allAbove)))
				return rgbIn;

			const double refNits = d.referenceWhiteNits;
			const double screenNits = in.screenWhiteNits > 0.0 ? in.screenWhiteNits : refNits;
			const double veil = veilOverride >= 0.0 ? veilOverride : static_cast<double>(d.uniforms.veil);

			ExtendedRange::Transfer trc;
			trc.kind = static_cast<ExtendedRange::TransferKind>(in.transferKind);
			trc.param = static_cast<float>(in.transferParam);

			auto toRel = [&](double v) -> double
			{
				if (in.signalPq)
					return DvMath::NitsFromPq(v) / (std::max)(refNits, 1e-6);
				if (in.transferKind > 0)
					return ExtendedRange::ToLinear(trc, v);
				return std::pow(v, in.outputGamma);
			};
			auto fromRel = [&](double r) -> double
			{
				r = (std::max)(r, 0.0);
				if (in.signalPq)
					return DvMath::PqFromNits(r * (simulatePreFixSdrCeiling ? screenNits : refNits));
				if (in.transferKind > 0)
					return ExtendedRange::FromLinear(trc, r);
				return std::pow(r, 1.0 / in.outputGamma);
			};

			double fromXyz[9] = {};
			Assert::IsTrue(DvMath::Invert(in.rgbToXyz, fromXyz), L"Output primaries matrix must be invertible");

			Cam16::UniformInputs ui;
			ui.referenceWhiteNits = refNits;
			ui.screenWhiteNits = screenNits;
			ui.referenceSurround = Cam16::kSurroundDim;
			ui.screenSurround = d.room.surround;
			ui.veilNits = d.room.veilNits;
			ui.strength = in.strength;
			AppearanceStage::WhiteXyz(in.rgbToXyz, ui.whiteXyz);

			const Cam16::Prepared refPrep = Cam16::Prepare(Cam16::ReferenceConditions(ui));
			const Cam16::Prepared scrPrep = Cam16::Prepare(Cam16::ScreenConditions(ui));

			const std::array<double, 3> lin = { { toRel(inRange[0]), toRel(inRange[1]), toRel(inRange[2]) } };
			const std::array<double, 3> xyz100 = MulRowMajor3(in.rgbToXyz, { { lin[0] * 100.0, lin[1] * 100.0, lin[2] * 100.0 } });
			Cam16::Appearance jmh = Cam16::Forward(refPrep, { { xyz100[0], xyz100[1], xyz100[2] } });
			const double huntScale = huntScaleOverride >= 0.0 ? huntScaleOverride : d.huntScale;
			jmh.M *= huntScale;

			auto invLin = [&](double m) -> std::array<double, 3>
			{
				const Cam16::Vec3 outXyz = Cam16::Inverse(scrPrep, jmh.J, m, jmh.h);
				const std::array<double, 3> scaled = MulRowMajor3(fromXyz, { { outXyz[0], outXyz[1], outXyz[2] } });
				return { { scaled[0] * 0.01, scaled[1] * 0.01, scaled[2] * 0.01 } };
			};

			// POD2-H2 (432818710): Cam16::Inverse outputs XYZ_scr with Yw_scr = 100, which maps to refNits-relative units (not screenNits).
			const double upper = (!simulatePreFixSdrCeiling && in.signalPq) ? (10000.0 / (std::max)(refNits, 1e-6)) : 1.0;
			auto outOfRange = [upper](const std::array<double, 3>& c) -> bool
			{
				return !(c[0] >= -1e-4 && c[0] <= upper + 1e-4 &&
					c[1] >= -1e-4 && c[1] <= upper + 1e-4 &&
					c[2] >= -1e-4 && c[2] <= upper + 1e-4);
			};

			std::array<double, 3> outLin = invLin(jmh.M);
			if (outOfRange(outLin))
			{
				double lo = 0.0;
				double hi = jmh.M;
				bool foundInRange = !outOfRange(invLin(0.0));
				for (int i = 0; i < 6; ++i)
				{
					const double mid = 0.5 * (lo + hi);
					if (outOfRange(invLin(mid)))
						hi = mid;
					else
					{
						lo = mid;
						foundInRange = true;
					}
				}
				const std::array<double, 3> bisected = invLin(lo);
				if (simulatePreFixSdrCeiling || foundInRange)
				{
					outLin = bisected;
				}
				else
				{
					const bool origFinite = std::isfinite(outLin[0]) && std::isfinite(outLin[1]) && std::isfinite(outLin[2]);
					const std::array<double, 3>& src = origFinite ? outLin : bisected;
					for (int c = 0; c < 3; ++c)
						outLin[c] = (std::min)(upper, (std::max)(0.0, src[c]));
				}
			}

			const double veilRel = (!simulatePreFixSdrCeiling && in.signalPq) ? veil * (screenNits / (std::max)(refNits, 1e-6)) : veil;
			for (int c = 0; c < 3; ++c)
				outLin[c] = FlareReference(outLin[c], veilRel, upper);

			const double luma = in.rgbToXyz[3] * lin[0] + in.rgbToXyz[4] * lin[1] + in.rgbToXyz[5] * lin[2];
			const double knee = (std::min)(0.5, (std::max)(0.0, in.toeKnee));
			// W23-C2-S3-L2 (bff541a87): apx_knee is an SDR code fraction converted via apx_gamma even on PQ outputs.
			const double kneeRel = (!simulatePreFixSdrCeiling && in.signalPq) ? std::pow(knee, (std::max)(in.outputGamma, 1e-6)) : toRel(knee);
			const double toe = knee > 0.0 ? Smoothstep(0.0, kneeRel, luma) : 1.0;
			const double w = (std::min)(1.0, (std::max)(0.0, in.strength)) * toe;

			std::array<double, 3> res{};
			for (int c = 0; c < 3; ++c)
			{
				const double inScreen = (simulatePreFixSdrCeiling && in.signalPq) ? lin[c] * (refNits / (std::max)(screenNits, 1e-6)) : lin[c];
				res[c] = fromRel(inScreen + (outLin[c] - inScreen) * w) + excess[c];
			}
			return res;
		}

		inline double PqChromaSpread(const std::array<float, 3>& px)
		{
			const double mn = (std::min)(static_cast<double>(px[0]), (std::min)(static_cast<double>(px[1]), static_cast<double>(px[2])));
			const double mx = (std::max)(static_cast<double>(px[0]), (std::max)(static_cast<double>(px[1]), static_cast<double>(px[2])));
			return mx - mn;
		}

		inline double PqChromaSpread(const std::array<double, 3>& px)
		{
			const double mn = (std::min)(px[0], (std::min)(px[1], px[2]));
			const double mx = (std::max)(px[0], (std::max)(px[1], px[2]));
			return mx - mn;
		}

		inline double PqLinearLumaNits(const double rgbToXyz[9], const std::array<float, 3>& pq)
		{
			return rgbToXyz[3] * DvMath::NitsFromPq(pq[0]) +
				rgbToXyz[4] * DvMath::NitsFromPq(pq[1]) +
				rgbToXyz[5] * DvMath::NitsFromPq(pq[2]);
		}

		inline double PqLinearLumaNits(const double rgbToXyz[9], const std::array<double, 3>& pq)
		{
			return rgbToXyz[3] * DvMath::NitsFromPq(pq[0]) +
				rgbToXyz[4] * DvMath::NitsFromPq(pq[1]) +
				rgbToXyz[5] * DvMath::NitsFromPq(pq[2]);
		}

		inline std::array<float, 3> PqPixelFromNits(double rNits, double gNits, double bNits)
		{
			return { {
				static_cast<float>(DvMath::PqFromNits(rNits)),
				static_cast<float>(DvMath::PqFromNits(gNits)),
				static_cast<float>(DvMath::PqFromNits(bNits))
			} };
		}

		inline void AssertAllFinite(const std::vector<std::array<float, 3>>& pixels, const wchar_t* label)
		{
			for (size_t i = 0; i < pixels.size(); ++i)
			{
				for (int c = 0; c < 3; ++c)
				{
					Assert::IsTrue(std::isfinite(pixels[i][c]),
						(std::wstring(label) + L": non-finite value at pixel " + std::to_wstring(i) +
							L" ch " + std::to_wstring(c)).c_str());
				}
			}
		}
	}

	TEST_CLASS(AppearanceShaderGpuTests)
	{
	public:
		// S2-B1 + CT-H6: PQ HDR highlights above screen white must preserve their chroma (gamut ceiling
		// is 10000 / screenWhiteNits, never SDR 1.0) and must NEVER be boosted by flare when veil > 0.
		BEGIN_TEST_METHOD_ATTRIBUTE(PqHdrHighlightsPreserveChromaAndDoNotBoostUnderVeil)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PqHdrHighlightsPreserveChromaAndDoNotBoostUnderVeil)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			AppearanceHook apHook(fixture);

			// Saturated HDR highlights above screen white (100 nits) + diffuse white + mid-tones + black.
			// Pixel [0] (500, 80, 80) keeps Pass-2 Lit PQ chroma spread ~0.181 (generous headroom above 0.15).
			// Pixel [1] (1500, 150, 50) collapses under the pre-fix SDR ceiling (upper = 1.0) to monochrome
			// L ~ 3.300 (relative to 100-nit screen white), so SDR-normalized flare at f = 0.10 boosts it by
			// ((3.300 + 0.10) / (3.300 + 0.20)) * (1.20 / 1.10) ~ 1.0588 (+5.88%, cleanly clearing > 1.05).
			const std::vector<std::array<float, 3>> pqInputs = {
				PqPixelFromNits(500.0, 80.0, 80.0),      // [0] red highlight
				PqPixelFromNits(1500.0, 150.0, 50.0),    // [1] intense orange-red highlight (L ~ 3.300 -> +5.88% SDR flare boost)
				PqPixelFromNits(4000.0, 800.0, 200.0),   // [2] 4000-nit bright highlight (L ~ 11.801 -> +8.15% SDR flare boost)
				PqPixelFromNits(50.0, 200.0, 1000.0),    // [3] 1000-nit cyan-blue highlight
				PqPixelFromNits(100.0, 100.0, 100.0),    // [4] diffuse white (screen white)
				PqPixelFromNits(30.0, 10.0, 5.0),        // [5] dark saturated mid-tone
				PqPixelFromNits(60.0, 45.0, 25.0),       // [6] warm mid-tone
				{ { 0.0f, 0.0f, 0.0f } }                 // [7] exact black
			};
			constexpr size_t kHighlightCount = 4;

			// Pass 1: Cinema (dark surround, veil == 0).
			AppearanceStage::Inputs inDark;
			inDark.on = true;
			inDark.strength = 1.0;
			inDark.signalPq = true;
			inDark.sourceSdr = false;
			inDark.screenWhiteNits = 100.0;
			inDark.sdrTargetNits = 203.0;
			inDark.room.room = RoomLight::Room::Cinema;
			Assert::IsTrue(AppearanceStage::RgbToXyz(kBt2020Red, kBt2020Green, kBt2020Blue, DvMath::kD65, inDark.rgbToXyz));

			RoomLight::Smoother smootherDark;
			const AppearanceStage::Decision dDark = AppearanceStage::Decide(inDark, smootherDark);
			Assert::IsTrue(dDark.active);
			Assert::AreEqual(0.0f, dDark.uniforms.veil, 0.0f);
			ApplyParameters(apHook.hook, dDark.parameters);
			const auto gpuDark = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ, pqInputs);
			AssertAllFinite(gpuDark, L"PQ Cinema pass");

			for (size_t i = 0; i < pqInputs.size(); ++i)
			{
				const std::array<double, 3> ref = CpuReferenceAppearancePixel(
					inDark, dDark, { { pqInputs[i][0], pqInputs[i][1], pqInputs[i][2] } });
				for (int c = 0; c < 3; ++c)
				{
					Assert::AreEqual(ref[c], static_cast<double>(gpuDark[i][c]), 2e-3,
						(L"PQ Cinema GPU vs CPU mismatch at pixel " + std::to_wstring(i) + L" ch " + std::to_wstring(c)).c_str());
				}
			}
			for (size_t i = 0; i < kHighlightCount; ++i)
			{
				const double spread = PqChromaSpread(gpuDark[i]);
				Assert::IsTrue(spread > 0.15,
					(L"PQ Cinema HDR highlight " + std::to_wstring(i) + L" lost chroma (spread=" + std::to_wstring(spread) + L")").c_str());
			}

			// Pass 2: Lit room via Room::Auto sensor record (average surround + veil > 0 through AppearanceStage::Decide).
			AppearanceStage::Inputs inLit = inDark;
			inLit.room.room = RoomLight::Room::Auto;
			inLit.room.reflectNitsPerLux = 0.01;
			inLit.room.maxBlackNits = 5.0;
			inLit.recordAvailable = true;
			inLit.record.valid = true;
			inLit.record.smoothedLux = 300.0;   // veilNits = 3.0 -> veil = 0.03

			RoomLight::Smoother smootherLit;
			const AppearanceStage::Decision dLit = AppearanceStage::Decide(inLit, smootherLit);
			Assert::IsTrue(dLit.active);
			Assert::IsTrue(dLit.uniforms.veil > 0.02f, L"Lit room must produce non-zero veil");
			ApplyParameters(apHook.hook, dLit.parameters);
			const auto gpuLit = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ, pqInputs);
			AssertAllFinite(gpuLit, L"PQ Lit pass");

			for (size_t i = 0; i < pqInputs.size(); ++i)
			{
				const std::array<double, 3> ref = CpuReferenceAppearancePixel(
					inLit, dLit, { { pqInputs[i][0], pqInputs[i][1], pqInputs[i][2] } });
				for (int c = 0; c < 3; ++c)
				{
					Assert::AreEqual(ref[c], static_cast<double>(gpuLit[i][c]), 2e-3,
						(L"PQ Lit GPU vs CPU mismatch at pixel " + std::to_wstring(i) + L" ch " + std::to_wstring(c)).c_str());
				}
			}
			for (size_t i = 0; i < kHighlightCount; ++i)
			{
				const double spread = PqChromaSpread(gpuLit[i]);
				Assert::IsTrue(spread > 0.15,
					(L"PQ Lit HDR highlight " + std::to_wstring(i) + L" lost chroma (spread=" + std::to_wstring(spread) + L")").c_str());
				const double yDark = PqLinearLumaNits(inDark.rgbToXyz, gpuDark[i]);
				const double yLit = PqLinearLumaNits(inLit.rgbToXyz, gpuLit[i]);
				Assert::IsTrue(yLit <= yDark + 1e-2,
					(L"PQ Lit highlight " + std::to_wstring(i) + L" boosted above dark pass (yLit=" +
						std::to_wstring(yLit) + L" > yDark=" + std::to_wstring(yDark) + L")").c_str());
			}

			// Pass 3: Isolated flare check on GPU — hold all CAM16 viewing uniforms identical to Pass 1
			// and raise ONLY apx_veil to 0.10 so any SDR-normalized flare boost cannot be masked by surround.
			const double kIsolatedVeil = 0.10;
			ApplyParameters(apHook.hook, dDark.parameters);
			Assert::IsTrue(CustomHookChain::SetHookParameter(apHook.hook, "apx_veil", kIsolatedVeil));
			const auto gpuVeiled = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ, pqInputs);
			AssertAllFinite(gpuVeiled, L"PQ isolated-veil pass");

			for (size_t i = 0; i < kHighlightCount; ++i)
			{
				const double yDark = PqLinearLumaNits(inDark.rgbToXyz, gpuDark[i]);
				const double yVeiled = PqLinearLumaNits(inDark.rgbToXyz, gpuVeiled[i]);
				// Allow 0.5% for rgba16f PREOUTPUT FBO quantization; broken SDR flare boosts [1]/[2] by +5.88%/+8.15%.
				Assert::IsTrue(yVeiled <= yDark * 1.005,
					(L"PQ isolated flare boosted HDR highlight " + std::to_wstring(i) + L" (yVeiled=" +
						std::to_wstring(yVeiled) + L" vs yDark=" + std::to_wstring(yDark) + L")").c_str());
				for (int c = 0; c < 3; ++c)
				{
					Assert::IsTrue(gpuVeiled[i][c] <= gpuDark[i][c] + 1e-3f,
						(L"PQ isolated flare boosted channel " + std::to_wstring(c) + L" of highlight " + std::to_wstring(i)).c_str());
				}
			}

			// Mandatory in-test negative control (exact same PQ highlight pixels):
			// the pre-fix shader baseline (upper = 1.0 bisection + SDR-normalized (1+2f)/(1+f) flare)
			// BOTH crushes 1500-nit & 4000-nit highlights to monochrome grey (< 0.01 PQ spread; L ~ 3.300 and L ~ 11.801)
			// AND boosts highlight luminance strictly above veil == 0 by > +5% (+5.88% and +8.15%) under veil = 0.10.
			for (const size_t idx : { size_t{ 1 }, size_t{ 2 } })
			{
				const std::array<double, 3> px = { { pqInputs[idx][0], pqInputs[idx][1], pqInputs[idx][2] } };
				const std::array<double, 3> brokenDark = CpuReferenceAppearancePixel(inDark, dDark, px, 0.0, true);
				const std::array<double, 3> brokenVeiled = CpuReferenceAppearancePixel(inDark, dDark, px, kIsolatedVeil, true);
				const double brokenSpread = PqChromaSpread(brokenDark);
				Assert::IsTrue(brokenSpread < 0.01,
					(L"Negative control expected pre-fix SDR ceiling to strip chroma to monochrome (spread=" +
						std::to_wstring(brokenSpread) + L")").c_str());
				const double yBrokenDark = PqLinearLumaNits(inDark.rgbToXyz, brokenDark);
				const double yBrokenVeiled = PqLinearLumaNits(inDark.rgbToXyz, brokenVeiled);
				Assert::IsTrue(yBrokenVeiled > yBrokenDark * 1.05,
					(L"Negative control expected pre-fix SDR flare to boost PQ highlight above veil==0 (veiled=" +
						std::to_wstring(yBrokenVeiled) + L" vs dark=" + std::to_wstring(yBrokenDark) + L")").c_str());
			}

			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

		// S2-H1 + CT-H6: SDR dim-screen adaptation, exact achromatic greys (atan(0,0) safety),
		// flare toe near-black monotonicity vs naive subtract-and-clip negative control,
		// wholly-BTB/WTW passthrough, and single-channel BTB/WTW excess preservation.
		BEGIN_TEST_METHOD_ATTRIBUTE(SdrDimAdaptationNeutralGreysVeilAndBtbWtwMatchCpuReference)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SdrDimAdaptationNeutralGreysVeilAndBtbWtwMatchCpuReference)
		{
			// S2-H1 non-vacuity guard: GLSL 4.60 §8.1 leaves atan(0.0, 0.0) undefined even when a
			// particular software driver (WARP) happens to return 0 instead of NaN; ap_forward must
			// explicitly guard atan(b, a) against a == 0 && b == 0.
			{
				const std::string glsl = LoadBundledShader("vp_cam16_appearance.glsl");
				Assert::IsFalse(glsl.empty(), L"vp_cam16_appearance.glsl is readable");
				const size_t fwdPos = glsl.find("vec3 ap_forward(");
				Assert::IsTrue(fwdPos != std::string::npos, L"ap_forward found");
				const size_t invPos = glsl.find("vec3 ap_inverse(", fwdPos);
				Assert::IsTrue(invPos != std::string::npos, L"ap_inverse found");
				const std::string fwdBody = glsl.substr(fwdPos, invPos - fwdPos);
				const size_t atanPos = fwdBody.find("atan(");
				Assert::IsTrue(atanPos != std::string::npos, L"S2-H1: ap_forward atan call not found");
				const size_t lineNl = fwdBody.rfind('\n', atanPos);
				const size_t stmtStart = (lineNl != std::string::npos) ? (lineNl + 1) : 0;
				const size_t prevNl = (lineNl != std::string::npos && lineNl > 0) ? fwdBody.rfind('\n', lineNl - 1) : std::string::npos;
				const size_t windowStart = (prevNl != std::string::npos) ? (prevNl + 1) : 0;
				const std::string stmtPrefix = fwdBody.substr(stmtStart, atanPos - stmtStart);
				const std::string atanWindow = fwdBody.substr(windowStart, atanPos - windowStart);
				const bool explicitZeroCheckOnAB =
					(atanWindow.find("a == 0.0") != std::string::npos && atanWindow.find("b == 0.0") != std::string::npos) ||
					(atanWindow.find("a != 0.0") != std::string::npos && atanWindow.find("b != 0.0") != std::string::npos);
				const bool guardsAtanCall = stmtPrefix.find('?') != std::string::npos ||
					atanWindow.find("if (") != std::string::npos ||
					atanWindow.find("if(") != std::string::npos;
				Assert::IsTrue(explicitZeroCheckOnAB && guardsAtanCall,
					L"S2-H1: ap_forward must explicitly guard atan(b, a) against a == 0.0 && b == 0.0");
			}

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			AppearanceHook apHook(fixture);

			// Dim-screen SDR (48 nits vs 100-nit reference), Dim surround via geometric-mid lux + real veil (~0.05)
			// so CAM16 degree-of-adaptation F matches the Dim reference and neutral greys remain achromatic.
			AppearanceStage::Inputs in;
			in.on = true;
			in.strength = 1.0;
			in.sourceSdr = true;
			in.signalPq = false;
			in.outputGamma = 2.4;
			in.screenWhiteNits = 48.0;
			in.room.room = RoomLight::Room::Auto;
			in.room.reflectNitsPerLux = 0.2;
			in.room.maxBlackNits = 5.0;
			in.recordAvailable = true;
			in.record.valid = true;
			const double midLux = std::sqrt((in.room.anchors.luxDark + AmbientAdapt::kLuxOffset) *
				(in.room.anchors.luxLit + AmbientAdapt::kLuxOffset)) - AmbientAdapt::kLuxOffset;
			in.record.smoothedLux = midLux;

			RoomLight::Smoother smoother;
			const AppearanceStage::Decision d = AppearanceStage::Decide(in, smoother);
			Assert::IsTrue(d.active);
			Assert::IsTrue(d.uniforms.veil > 0.04f, L"Veil must be well above near-black ramp levels");
			ApplyParameters(apHook.hook, d.parameters);

			// Build deterministic pixel strip with indexed regions.
			std::vector<std::array<float, 3>> pixels;
			const size_t idxGreyBegin = pixels.size();
			for (const float g : { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f })
				pixels.push_back({ { g, g, g } });
			const size_t idxGreyEnd = pixels.size();

			const size_t idxColorBegin = pixels.size();
			pixels.push_back({ { 0.9f, 0.2f, 0.2f } });
			pixels.push_back({ { 0.2f, 0.85f, 0.2f } });
			pixels.push_back({ { 0.2f, 0.25f, 0.9f } });
			pixels.push_back({ { 0.15f, 0.8f, 0.85f } });
			pixels.push_back({ { 0.85f, 0.2f, 0.8f } });
			pixels.push_back({ { 0.85f, 0.8f, 0.15f } });
			const size_t idxColorEnd = pixels.size();

			const size_t idxRampBegin = pixels.size();
			for (int k = 1; k <= 16; ++k)
			{
				const float v = static_cast<float>(k) / 255.0f;
				pixels.push_back({ { v, v, v } });
			}
			const size_t idxRampEnd = pixels.size();

			// Exact fp16-representable dyadic BTB / WTW values so PREOUTPUT rgba16f FBO adds zero quantization error.
			const size_t idxWholeBtb = pixels.size();
			pixels.push_back({ { -0.0625f, -0.03125f, -0.015625f } });
			const size_t idxWholeWtw = pixels.size();
			pixels.push_back({ { 1.0625f, 1.03125f, 1.09375f } });

			const size_t idxSingleWtw = pixels.size();
			pixels.push_back({ { 1.0625f, 0.5f, 0.25f } });
			const size_t idxSingleWtwBase = pixels.size();
			pixels.push_back({ { 1.0f, 0.5f, 0.25f } });

			const size_t idxSingleBtb = pixels.size();
			pixels.push_back({ { -0.0625f, 0.5f, 0.25f } });
			const size_t idxSingleBtbBase = pixels.size();
			pixels.push_back({ { 0.0f, 0.5f, 0.25f } });

			const auto gpu = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24, pixels);
			AssertAllFinite(gpu, L"SDR dim-adaptation pass");

			// 1. Neutral greys: achromatic + CPU reference parity + exact black at 0.
			for (size_t i = idxGreyBegin; i < idxGreyEnd; ++i)
			{
				Assert::IsTrue(std::fabs(gpu[i][0] - gpu[i][1]) < 1e-3f && std::fabs(gpu[i][1] - gpu[i][2]) < 1e-3f,
					(L"Neutral grey " + std::to_wstring(i - idxGreyBegin) + L" drifted from achromatic").c_str());
				const std::array<double, 3> ref = CpuReferenceAppearancePixel(
					in, d, { { pixels[i][0], pixels[i][1], pixels[i][2] } });
				for (int c = 0; c < 3; ++c)
					Assert::AreEqual(ref[c], static_cast<double>(gpu[i][c]), 2e-3, L"Neutral grey GPU vs CPU mismatch");
			}
			for (int c = 0; c < 3; ++c)
				Assert::AreEqual(0.0, static_cast<double>(gpu[idxGreyBegin][c]), 1e-5, L"Exact black must remain 0");

			// 2. Saturated RGB / CMY: CPU reference parity + non-vacuity (adaptation actually moved colours).
			double maxColorMove = 0.0;
			for (size_t i = idxColorBegin; i < idxColorEnd; ++i)
			{
				const std::array<double, 3> ref = CpuReferenceAppearancePixel(
					in, d, { { pixels[i][0], pixels[i][1], pixels[i][2] } });
				for (int c = 0; c < 3; ++c)
				{
					Assert::AreEqual(ref[c], static_cast<double>(gpu[i][c]), 2e-3,
						(L"Saturated colour " + std::to_wstring(i - idxColorBegin) + L" GPU vs CPU mismatch").c_str());
					maxColorMove = (std::max)(maxColorMove, std::fabs(static_cast<double>(gpu[i][c]) - pixels[i][c]));
				}
			}
			Assert::IsTrue(maxColorMove > 0.01, L"SDR appearance adaptation must actively transform saturated colours");

			// 3. Near-black ramp 1/255 .. 16/255 under veil: strictly positive, strictly increasing, matches CPU.
			float prev = 0.0f;
			for (size_t i = idxRampBegin; i < idxRampEnd; ++i)
			{
				const std::array<double, 3> ref = CpuReferenceAppearancePixel(
					in, d, { { pixels[i][0], pixels[i][1], pixels[i][2] } });
				for (int c = 0; c < 3; ++c)
				{
					Assert::IsTrue(gpu[i][c] > 0.0f, L"Near-black level must not be crushed to zero under veil");
					Assert::AreEqual(ref[c], static_cast<double>(gpu[i][c]), 2e-3, L"Near-black ramp GPU vs CPU mismatch");
				}
				Assert::IsTrue(gpu[i][1] > prev,
					(L"Near-black ramp must be strictly increasing at step " + std::to_wstring(i - idxRampBegin + 1)).c_str());
				prev = gpu[i][1];
			}

			// Mandatory in-test negative control (exact same near-black ramp inputs):
			// naive subtract-and-clip max(o - f, 0) crushes every near-black level with o <= f to 0.0.
			{
				const double f = static_cast<double>(d.uniforms.veil);
				int crushedCount = 0;
				for (size_t i = idxRampBegin; i < idxRampEnd; ++i)
				{
					const std::array<double, 3> noVeil = CpuReferenceAppearancePixel(
						in, d, { { pixels[i][0], pixels[i][1], pixels[i][2] } }, 0.0, false);
					const double oLin = std::pow((std::max)(0.0, noVeil[1]), in.outputGamma);
					const double naiveDisplayed = (std::max)(oLin - f, 0.0);
					if (oLin <= f)
					{
						Assert::AreEqual(0.0, naiveDisplayed, 0.0, L"Naive subtract-and-clip crushes o <= f to 0");
						++crushedCount;
					}
				}
				Assert::IsTrue(crushedCount >= 8,
					L"Negative control must verify that naive subtract-and-clip crushes at least 8 near-black levels to 0");
			}

			// 4. Wholly-BTB passes through untouched within 1e-5; wholly-WTW adapts (1,1,1) + excess for C0 corner continuity (W23-NEON-H3).
			const std::array<double, 3> refWholeWtw = CpuReferenceAppearancePixel(
				in, d, { { pixels[idxWholeWtw][0], pixels[idxWholeWtw][1], pixels[idxWholeWtw][2] } });
			const std::array<double, 3> refWhite = CpuReferenceAppearancePixel(in, d, { { 1.0, 1.0, 1.0 } });
			for (int c = 0; c < 3; ++c)
			{
				Assert::AreEqual(static_cast<double>(pixels[idxWholeBtb][c]), static_cast<double>(gpu[idxWholeBtb][c]), 1e-5,
					L"Wholly-BTB pixel must pass through unchanged");
				Assert::AreEqual(refWholeWtw[c], static_cast<double>(gpu[idxWholeWtw][c]), 2e-3,
					L"W23-NEON-H3: Wholly-WTW pixel must match adapted (1,1,1) + excess CPU reference");
				Assert::AreEqual(static_cast<double>(pixels[idxWholeWtw][c]) - 1.0,
					static_cast<double>(gpu[idxWholeWtw][c]) - refWhite[c], 2e-3,
					L"W23-NEON-H3: Wholly-WTW pixel must preserve exact WTW excess over adapted white");
			}

			// 5. Single-channel WTW and single-channel BTB preserve exact excess while adapting in-range channels identically.
			Assert::AreEqual(0.0625, static_cast<double>(gpu[idxSingleWtw][0] - gpu[idxSingleWtwBase][0]), 2e-3,
				L"Single-channel WTW excess must be preserved");
			Assert::AreEqual(static_cast<double>(gpu[idxSingleWtwBase][1]), static_cast<double>(gpu[idxSingleWtw][1]), 1e-3,
				L"Single-channel WTW must adapt in-range G identically to its in-range neighbour");
			Assert::AreEqual(static_cast<double>(gpu[idxSingleWtwBase][2]), static_cast<double>(gpu[idxSingleWtw][2]), 1e-3,
				L"Single-channel WTW must adapt in-range B identically to its in-range neighbour");

			Assert::AreEqual(-0.0625, static_cast<double>(gpu[idxSingleBtb][0] - gpu[idxSingleBtbBase][0]), 2e-3,
				L"Single-channel BTB excess must be preserved");
			Assert::AreEqual(static_cast<double>(gpu[idxSingleBtbBase][1]), static_cast<double>(gpu[idxSingleBtb][1]), 1e-3,
				L"Single-channel BTB must adapt in-range G identically to its in-range neighbour");
			Assert::AreEqual(static_cast<double>(gpu[idxSingleBtbBase][2]), static_cast<double>(gpu[idxSingleBtb][2]), 1e-3,
				L"Single-channel BTB must adapt in-range B identically to its in-range neighbour");

			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

		// CT-H6: apx_strength = 0.0 must be bit-exact identity (<= 1e-6) on both SDR and PQ frames,
		// including BTB and WTW codes (with non-vacuity check that strength = 1.0 actively transforms the frame).
		BEGIN_TEST_METHOD_ATTRIBUTE(StrengthZeroIsBitExactIdentityOnSdrAndPq)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StrengthZeroIsBitExactIdentityOnSdrAndPq)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			AppearanceHook apHook(fixture);

			// Exact fp16-representable dyadic values covering BTB, black, mid-tones, saturated colours, white, and WTW.
			const std::vector<std::array<float, 3>> pixels = {
				{ { -0.0625f, -0.03125f, -0.015625f } },
				{ { 0.0f, 0.0f, 0.0f } },
				{ { 0.125f, 0.25f, 0.5f } },
				{ { 0.875f, 0.125f, 0.25f } },
				{ { 0.125f, 0.75f, 0.875f } },
				{ { 0.5f, 0.5f, 0.5f } },
				{ { 1.0f, 1.0f, 1.0f } },
				{ { 1.0625f, 0.5f, 0.25f } },
				{ { -0.0625f, 0.5f, 0.25f } },
				{ { 1.0625f, 1.03125f, 1.09375f } }
			};

			// Sub-case A: SDR BT.709 GAMMA24.
			{
				AppearanceStage::Inputs in;
				in.on = true;
				in.strength = 0.0;
				in.sourceSdr = true;
				in.signalPq = false;
				in.outputGamma = 2.4;
				in.screenWhiteNits = 48.0;
				in.room.room = RoomLight::Room::Cinema;

				RoomLight::Smoother s;
				const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
				Assert::IsFalse(d.active);
				ApplyParameters(apHook.hook, d.parameters);

				const auto outZero = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24, pixels);
				AssertAllFinite(outZero, L"SDR strength=0 pass");
				for (size_t i = 0; i < pixels.size(); ++i)
				{
					for (int c = 0; c < 3; ++c)
					{
						Assert::AreEqual(static_cast<double>(pixels[i][c]), static_cast<double>(outZero[i][c]), 1e-6,
							(L"SDR strength=0 changed pixel " + std::to_wstring(i) + L" ch " + std::to_wstring(c)).c_str());
					}
				}

				// Non-vacuity control: strength=1.0 on the exact same pixels must visibly transform in-range colours.
				Assert::IsTrue(CustomHookChain::SetHookParameter(apHook.hook, "apx_strength", 1.0));
				const auto outOne = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24, pixels);
				double maxDelta = 0.0;
				for (size_t i = 0; i < pixels.size(); ++i)
					for (int c = 0; c < 3; ++c)
						maxDelta = (std::max)(maxDelta, std::fabs(static_cast<double>(outOne[i][c]) - pixels[i][c]));
				Assert::IsTrue(maxDelta > 0.01, L"SDR non-vacuity control: strength=1.0 must transform the frame");
			}

			// Sub-case B: PQ BT.2020.
			{
				AppearanceStage::Inputs in;
				in.on = true;
				in.strength = 0.0;
				in.sourceSdr = false;
				in.signalPq = true;
				in.screenWhiteNits = 100.0;
				in.sdrTargetNits = 203.0;
				in.room.room = RoomLight::Room::Cinema;
				Assert::IsTrue(AppearanceStage::RgbToXyz(kBt2020Red, kBt2020Green, kBt2020Blue, DvMath::kD65, in.rgbToXyz));

				RoomLight::Smoother s;
				const AppearanceStage::Decision d = AppearanceStage::Decide(in, s);
				Assert::IsFalse(d.active);
				ApplyParameters(apHook.hook, d.parameters);

				const auto outZero = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ, pixels);
				AssertAllFinite(outZero, L"PQ strength=0 pass");
				for (size_t i = 0; i < pixels.size(); ++i)
				{
					for (int c = 0; c < 3; ++c)
					{
						Assert::AreEqual(static_cast<double>(pixels[i][c]), static_cast<double>(outZero[i][c]), 1e-6,
							(L"PQ strength=0 changed pixel " + std::to_wstring(i) + L" ch " + std::to_wstring(c)).c_str());
					}
				}

				// Non-vacuity control: strength=1.0 on the exact same pixels must visibly transform in-range colours.
				Assert::IsTrue(CustomHookChain::SetHookParameter(apHook.hook, "apx_strength", 1.0));
				const auto outOne = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ, pixels);
				double maxDelta = 0.0;
				for (size_t i = 0; i < pixels.size(); ++i)
					for (int c = 0; c < 3; ++c)
						maxDelta = (std::max)(maxDelta, std::fabs(static_cast<double>(outOne[i][c]) - pixels[i][c]));
				Assert::IsTrue(maxDelta > 0.01, L"PQ non-vacuity control: strength=1.0 must transform the frame");
			}

			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

		// S2-H1: ap_out_of_range must use a negated in-range conjunction !(c >= -1e-4 && c <= upper + 1e-4 && ...)
		// so any NaN channel produced by float32 ap_inverse overflow evaluates true and enters M-bisection rather
		// than silently bypassing bisection via IEEE-754 false < / > comparisons.
		BEGIN_TEST_METHOD_ATTRIBUTE(ApOutOfRangeCatchesInverseOverflowAndRejectsNanBypass)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ApOutOfRangeCatchesInverseOverflowAndRejectsNanBypass)
		{
			// (a) Structural check: ap_out_of_range in vp_cam16_appearance.glsl must use a negated in-range
			// conjunction over c.r, c.g, c.b rather than bare < / > comparisons (which return false on NaN).
			{
				const std::string glsl = LoadBundledShader("vp_cam16_appearance.glsl");
				Assert::IsFalse(glsl.empty(), L"vp_cam16_appearance.glsl is readable");
				const size_t oorPos = glsl.find("bool ap_out_of_range(");
				Assert::IsTrue(oorPos != std::string::npos, L"S2-H1: ap_out_of_range not found");
				const size_t openBrace = glsl.find('{', oorPos);
				const size_t closeBrace = openBrace != std::string::npos ? glsl.find('}', openBrace) : std::string::npos;
				Assert::IsTrue(openBrace != std::string::npos && closeBrace != std::string::npos, L"S2-H1: ap_out_of_range body malformed");
				const std::string oorBody = glsl.substr(openBrace, closeBrace - openBrace);
				const size_t retPos = oorBody.find("return");
				const size_t semiPos = retPos != std::string::npos ? oorBody.find(';', retPos) : std::string::npos;
				Assert::IsTrue(retPos != std::string::npos && semiPos != std::string::npos, L"S2-H1: ap_out_of_range return statement missing");
				const std::string retExpr = oorBody.substr(retPos, semiPos - retPos);
				const bool negatedInRange = retExpr.find("!(") != std::string::npos &&
					retExpr.find("c.r >=") != std::string::npos && retExpr.find("c.r <=") != std::string::npos &&
					retExpr.find("c.g >=") != std::string::npos && retExpr.find("c.g <=") != std::string::npos &&
					retExpr.find("c.b >=") != std::string::npos && retExpr.find("c.b <=") != std::string::npos &&
					retExpr.find("&&") != std::string::npos &&
					retExpr.find("||") == std::string::npos;
				Assert::IsTrue(negatedInRange,
					L"S2-H1: ap_out_of_range must use negated in-range conjunction !(c >= -1e-4 && c <= upper + 1e-4 && ...) instead of < || >");
			}

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			AppearanceHook apHook(fixture);

			constexpr double kMaxHuntScale = 4.0;   // valid //!MAXIMUM in vp_cam16_appearance.glsl

			// (b1) Real WARP GPU SDR BT.709 extreme overflow / float32 NaN trigger (dim 20-nit screen, 4x M):
			// Exact fp16 pixels [0] (0.6875, 0.125, 0.8125) and [1] (0.75, 0.0625, 0.875) push ap_inverse
			// cone responses far past +/-400 (ra/ga/ba ~ +10463/-29410/+169912 and +864/-2368/+13046),
			// overflowing float32 ap_uncompress to (+Inf, -Inf, +Inf) and producing exact (NaN, NaN, NaN)
			// in outLin prior to M-bisection.
			{
				AppearanceStage::Inputs inSdr;
				inSdr.on = true;
				inSdr.strength = 1.0;
				inSdr.sourceSdr = true;
				inSdr.signalPq = false;
				inSdr.outputGamma = 2.4;
				inSdr.screenWhiteNits = 20.0;
				inSdr.room.room = RoomLight::Room::Cinema;

				RoomLight::Smoother smootherSdr;
				const AppearanceStage::Decision dSdr = AppearanceStage::Decide(inSdr, smootherSdr);
				Assert::IsTrue(dSdr.active);
				ApplyParameters(apHook.hook, dSdr.parameters);
				Assert::IsTrue(CustomHookChain::SetHookParameter(apHook.hook, "apx_hunt_scale", kMaxHuntScale));

				const std::vector<std::array<float, 3>> sdrExtremes = {
					{ { 0.6875f, 0.125f, 0.8125f } },   // [0] float32 ap_inverse -> (NaN, NaN, NaN) before bisection
					{ { 0.75f, 0.0625f, 0.875f } },     // [1] float32 ap_inverse -> (NaN, NaN, NaN) before bisection
					{ { 1.0f, 0.0f, 1.0f } },           // [2] bright saturated magenta primary overflow
					{ { 1.0f, 0.0f, 0.0f } },           // [3] bright saturated red primary overflow
					{ { 0.0f, 0.0f, 1.0f } }            // [4] bright saturated blue primary overflow
				};

				const auto gpuSdr = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24, sdrExtremes);
				AssertAllFinite(gpuSdr, L"SDR ap_out_of_range overflow/NaN pass");
				for (size_t i = 0; i < sdrExtremes.size(); ++i)
				{
					const std::array<double, 3> ref = CpuReferenceAppearancePixel(
						inSdr, dSdr, { { sdrExtremes[i][0], sdrExtremes[i][1], sdrExtremes[i][2] } }, -1.0, false, kMaxHuntScale);
					for (int c = 0; c < 3; ++c)
					{
						Assert::IsTrue(gpuSdr[i][c] >= -1e-3f && gpuSdr[i][c] <= 1.0f + 1e-3f,
							(L"SDR bisected pixel " + std::to_wstring(i) + L" ch " + std::to_wstring(c) +
								L" escaped [0, 1] gamut (val=" + std::to_wstring(gpuSdr[i][c]) + L")").c_str());
						Assert::AreEqual(ref[c], static_cast<double>(gpuSdr[i][c]), 2e-3,
							(L"SDR overflow/NaN bisection GPU vs CPU mismatch at pixel " + std::to_wstring(i) +
								L" ch " + std::to_wstring(c)).c_str());
					}
				}
			}

			// (b2) Real WARP GPU PQ BT.2020 extreme overflow / float32 NaN trigger (dim 20-nit screen, 4x M):
			// Exact fp16 PQ pixels [0] (0.9453125, 0, 1) and [1] (0.9462890625, 0, 1) push ap_inverse
			// cone responses far past +/-400 (-4255/+10648/-41127 and +2059/-5003/+19320), producing exact
			// (NaN, NaN, NaN) in float32 outLin prior to M-bisection.
			{
				AppearanceStage::Inputs inPq;
				inPq.on = true;
				inPq.strength = 1.0;
				inPq.sourceSdr = false;
				inPq.signalPq = true;
				inPq.screenWhiteNits = 20.0;
				inPq.sdrTargetNits = 203.0;
				inPq.room.room = RoomLight::Room::Cinema;
				Assert::IsTrue(AppearanceStage::RgbToXyz(kBt2020Red, kBt2020Green, kBt2020Blue, DvMath::kD65, inPq.rgbToXyz));

				RoomLight::Smoother smootherPq;
				const AppearanceStage::Decision dPq = AppearanceStage::Decide(inPq, smootherPq);
				Assert::IsTrue(dPq.active);
				ApplyParameters(apHook.hook, dPq.parameters);
				Assert::IsTrue(CustomHookChain::SetHookParameter(apHook.hook, "apx_hunt_scale", kMaxHuntScale));

				const std::vector<std::array<float, 3>> pqExtremes = {
					{ { 0.9453125f, 0.0f, 1.0f } },      // [0] float32 ap_inverse -> (NaN, NaN, NaN) before bisection
					{ { 0.9462890625f, 0.0f, 1.0f } },   // [1] float32 ap_inverse -> (NaN, NaN, NaN) before bisection
					{ { 1.0f, 0.0f, 0.0f } }             // [2] 10000-nit pure BT.2020 red overflow
				};

				const auto gpuPq = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ, pqExtremes);
				AssertAllFinite(gpuPq, L"PQ ap_out_of_range overflow/NaN pass");
				for (size_t i = 0; i < pqExtremes.size(); ++i)
				{
					const std::array<double, 3> ref = CpuReferenceAppearancePixel(
						inPq, dPq, { { pqExtremes[i][0], pqExtremes[i][1], pqExtremes[i][2] } }, -1.0, false, kMaxHuntScale);
					for (int c = 0; c < 3; ++c)
					{
						Assert::IsTrue(gpuPq[i][c] >= -1e-3f && gpuPq[i][c] <= 1.0f + 1e-3f,
							(L"PQ bisected pixel " + std::to_wstring(i) + L" ch " + std::to_wstring(c) +
								L" escaped [0, 1] PQ range (val=" + std::to_wstring(gpuPq[i][c]) + L")").c_str());
						Assert::AreEqual(ref[c], static_cast<double>(gpuPq[i][c]), 2e-3,
							(L"PQ overflow/NaN bisection GPU vs CPU mismatch at pixel " + std::to_wstring(i) +
								L" ch " + std::to_wstring(c)).c_str());
					}
				}
			}

			// (c) Mandatory in-test negative control: naive un-guarded min/max < || > and channel-wise < || >
			// silently return false on quiet_NaN (skipping M-bisection), while the negated in-range conjunction
			// !(c >= -1e-4 && c <= upper + 1e-4 && ...) returns true on NaN and false on in-range colours.
			{
				const double qnan = std::numeric_limits<double>::quiet_NaN();
				const std::array<double, 3> allNan = { { qnan, qnan, qnan } };
				const std::array<double, 3> oneNan = { { qnan, 0.5, 0.5 } };
				const std::array<double, 3> inRangePx = { { 0.5, 0.5, 0.5 } };
				for (const double upper : { 1.0, 10000.0 / 20.0 })
				{
					auto naiveMinMax = [upper](const std::array<double, 3>& c) -> bool
					{
						const double mn = (std::min)(c[0], (std::min)(c[1], c[2]));
						const double mx = (std::max)(c[0], (std::max)(c[1], c[2]));
						return mn < -1e-4 || mx > upper + 1e-4;
					};
					auto naiveChannelOr = [upper](const std::array<double, 3>& c) -> bool
					{
						return c[0] < -1e-4 || c[0] > upper + 1e-4 ||
							c[1] < -1e-4 || c[1] > upper + 1e-4 ||
							c[2] < -1e-4 || c[2] > upper + 1e-4;
					};
					auto negatedInRange = [upper](const std::array<double, 3>& c) -> bool
					{
						return !(c[0] >= -1e-4 && c[0] <= upper + 1e-4 &&
							c[1] >= -1e-4 && c[1] <= upper + 1e-4 &&
							c[2] >= -1e-4 && c[2] <= upper + 1e-4);
					};
					Assert::IsFalse(naiveMinMax(allNan), L"Negative control: naive min/max < || > silently returns false on all-NaN");
					Assert::IsFalse(naiveMinMax(oneNan), L"Negative control: naive min/max < || > silently returns false on single-NaN");
					Assert::IsFalse(naiveChannelOr(allNan), L"Negative control: naive channel < || > silently returns false on all-NaN");
					Assert::IsFalse(naiveChannelOr(oneNan), L"Negative control: naive channel < || > silently returns false on single-NaN");
					Assert::IsTrue(negatedInRange(allNan), L"Negated in-range conjunction must catch all-NaN as out-of-range");
					Assert::IsTrue(negatedInRange(oneNan), L"Negated in-range conjunction must catch single-NaN as out-of-range");
					Assert::IsFalse(negatedInRange(inRangePx), L"Negated in-range conjunction must accept valid in-range pixels");
				}
			}

			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

		// S2-H2: real compiled shaders/vp_cam16_appearance.glsl on WARP GPU must execute the
		// piecewise BT.1886 (apx_trc_kind = 1, apx_trc_param = b) and sRGB (apx_trc_kind = 2)
		// output transfers in ap_to_rel / ap_from_rel, matching the independent CPU CAM16 +
		// ExtendedRange::ToLinear/FromLinear reference within 2e-3 and diverging measurably
		// from pure power-law gamma 2.4 (transferKind = 0) on the exact same inputs.
		BEGIN_TEST_METHOD_ATTRIBUTE(Bt1886AndSrgbPiecewiseTransfersMatchCpuReferenceOnGpu)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(Bt1886AndSrgbPiecewiseTransfersMatchCpuReferenceOnGpu)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			AppearanceHook apHook(fixture);

			// Sub-case A: BT.1886 (1000:1 contrast target -> black offset b ~ 0.0596 > 0.05).
			{
				pl_color_space bt1886Target{};
				bt1886Target.primaries = PL_COLOR_PRIM_BT_709;
				bt1886Target.transfer = PL_COLOR_TRC_BT_1886;
				bt1886Target.hdr.min_luma = 0.1f;
				bt1886Target.hdr.max_luma = 100.0f;

				const ExtendedRange::OutputTransfer bt1886Trc = ExtendedRange::DescribeOutputTransfer(bt1886Target);
				Assert::IsTrue(bt1886Trc.supported, L"BT.1886 1000:1 output transfer must be supported");
				Assert::IsFalse(bt1886Trc.pq, L"BT.1886 output transfer must not set pq=true");
				Assert::AreEqual(static_cast<int>(ExtendedRange::TransferKind::Bt1886), bt1886Trc.kind,
					L"BT.1886 output transfer must report TransferKind::Bt1886 (1)");
				Assert::IsTrue(bt1886Trc.param > 0.05,
					L"1000:1 BT.1886 target must produce black offset b > 0.05");

				AppearanceStage::Inputs inBt;
				inBt.on = true;
				inBt.strength = 1.0;
				inBt.sourceSdr = true;
				inBt.signalPq = false;
				inBt.outputGamma = 2.4;
				inBt.screenWhiteNits = 48.0;
				inBt.room.room = RoomLight::Room::Cinema;
				inBt.transferSupported = bt1886Trc.supported;
				inBt.transferKind = bt1886Trc.kind;
				inBt.transferParam = bt1886Trc.param;

				RoomLight::Smoother smootherBt;
				const AppearanceStage::Decision dBt = AppearanceStage::Decide(inBt, smootherBt);
				Assert::IsTrue(dBt.active, L"BT.1886 appearance decision must be active");
				Assert::IsFalse(dBt.transferUnsupported);
				ApplyParameters(apHook.hook, dBt.parameters);

				const std::vector<std::array<float, 3>> btPixels = {
					{ { 0.02f, 0.02f, 0.02f } },
					{ { 0.05f, 0.05f, 0.05f } },
					{ { 0.08f, 0.08f, 0.08f } },
					{ { 0.12f, 0.12f, 0.12f } },
					{ { 0.25f, 0.25f, 0.25f } },
					{ { 0.50f, 0.50f, 0.50f } },
					{ { 0.08f, 0.04f, 0.02f } },
					{ { 0.70f, 0.22f, 0.14f } }
				};
				constexpr size_t kBtShadowCount = 7;

				const auto gpuBt = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886, btPixels);
				AssertAllFinite(gpuBt, L"BT.1886 piecewise GPU pass");

				for (size_t i = 0; i < btPixels.size(); ++i)
				{
					const std::array<double, 3> ref = CpuReferenceAppearancePixel(
						inBt, dBt, { { btPixels[i][0], btPixels[i][1], btPixels[i][2] } });
					for (int c = 0; c < 3; ++c)
					{
						Assert::AreEqual(ref[c], static_cast<double>(gpuBt[i][c]), 2e-3,
							(L"BT.1886 GPU vs CPU mismatch at pixel " + std::to_wstring(i) +
								L" ch " + std::to_wstring(c)).c_str());
					}
				}

				// Mandatory in-test negative control (exact same inputs rendered with transferKind = 0,
				// pure power-law gamma = 2.4): proves ap_to_rel / ap_from_rel actually execute the
				// BT.1886 piecewise curve on GPU rather than falling back to pure power-law gamma 2.4.
				AppearanceStage::Inputs inPow = inBt;
				inPow.transferKind = static_cast<int>(ExtendedRange::TransferKind::Power);
				inPow.transferParam = 0.0;
				RoomLight::Smoother smootherPow;
				const AppearanceStage::Decision dPow = AppearanceStage::Decide(inPow, smootherPow);
				Assert::IsTrue(dPow.active);
				ApplyParameters(apHook.hook, dPow.parameters);

				const auto gpuPow = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24, btPixels);
				AssertAllFinite(gpuPow, L"BT.1886 negative-control pure power-2.4 GPU pass");

				double maxDelta = 0.0;
				double maxShadowDelta = 0.0;
				for (size_t i = 0; i < btPixels.size(); ++i)
				{
					const std::array<double, 3> refPow = CpuReferenceAppearancePixel(
						inPow, dPow, { { btPixels[i][0], btPixels[i][1], btPixels[i][2] } });
					for (int c = 0; c < 3; ++c)
					{
						Assert::AreEqual(refPow[c], static_cast<double>(gpuPow[i][c]), 2e-3,
							(L"BT.1886 negative-control power-2.4 GPU vs CPU mismatch at pixel " + std::to_wstring(i) +
								L" ch " + std::to_wstring(c)).c_str());
						const double delta = std::fabs(static_cast<double>(gpuBt[i][c]) - static_cast<double>(gpuPow[i][c]));
						maxDelta = (std::max)(maxDelta, delta);
						if (i < kBtShadowCount)
							maxShadowDelta = (std::max)(maxShadowDelta, delta);
					}
				}
				Assert::IsTrue(maxShadowDelta > 0.004,
					(L"Negative control: BT.1886 GPU output must diverge from pure power-2.4 on shadow pixels (maxShadowDelta=" +
						std::to_wstring(maxShadowDelta) + L")").c_str());
				Assert::IsTrue(maxDelta > 0.015,
					(L"Negative control: BT.1886 GPU output must diverge measurably from pure power-2.4 (maxDelta=" +
						std::to_wstring(maxDelta) + L")").c_str());
			}

			// Sub-case B: sRGB (IEC 61966-2-1 piecewise transfer, linear toe below 0.04045).
			{
				pl_color_space srgbTarget{};
				srgbTarget.primaries = PL_COLOR_PRIM_BT_709;
				srgbTarget.transfer = PL_COLOR_TRC_SRGB;

				const ExtendedRange::OutputTransfer srgbTrc = ExtendedRange::DescribeOutputTransfer(srgbTarget);
				Assert::IsTrue(srgbTrc.supported, L"sRGB output transfer must be supported");
				Assert::IsFalse(srgbTrc.pq, L"sRGB output transfer must not set pq=true");
				Assert::AreEqual(static_cast<int>(ExtendedRange::TransferKind::Srgb), srgbTrc.kind,
					L"sRGB output transfer must report TransferKind::Srgb (2)");
				Assert::AreEqual(0.0, srgbTrc.param, 0.0, L"sRGB output transfer param must be 0");

				AppearanceStage::Inputs inSrgb;
				inSrgb.on = true;
				inSrgb.strength = 1.0;
				inSrgb.sourceSdr = true;
				inSrgb.signalPq = false;
				inSrgb.outputGamma = 2.4;
				inSrgb.screenWhiteNits = 48.0;
				inSrgb.room.room = RoomLight::Room::Cinema;
				inSrgb.transferSupported = srgbTrc.supported;
				inSrgb.transferKind = srgbTrc.kind;
				inSrgb.transferParam = srgbTrc.param;

				RoomLight::Smoother smootherSrgb;
				const AppearanceStage::Decision dSrgb = AppearanceStage::Decide(inSrgb, smootherSrgb);
				Assert::IsTrue(dSrgb.active, L"sRGB appearance decision must be active");
				Assert::IsFalse(dSrgb.transferUnsupported);
				ApplyParameters(apHook.hook, dSrgb.parameters);

				const std::vector<std::array<float, 3>> srgbPixels = {
					{ { 0.015f, 0.015f, 0.015f } },
					{ { 0.03f, 0.03f, 0.03f } },
					{ { 0.04045f, 0.04045f, 0.04045f } },
					{ { 0.06f, 0.06f, 0.06f } },
					{ { 0.12f, 0.12f, 0.12f } },
					{ { 0.50f, 0.50f, 0.50f } },
					{ { 0.05f, 0.02f, 0.01f } },
					{ { 0.70f, 0.22f, 0.14f } }
				};
				constexpr size_t kSrgbToeShadowCount = 7;

				const auto gpuSrgb = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_SRGB, srgbPixels);
				AssertAllFinite(gpuSrgb, L"sRGB piecewise GPU pass");

				for (size_t i = 0; i < srgbPixels.size(); ++i)
				{
					const std::array<double, 3> ref = CpuReferenceAppearancePixel(
						inSrgb, dSrgb, { { srgbPixels[i][0], srgbPixels[i][1], srgbPixels[i][2] } });
					for (int c = 0; c < 3; ++c)
					{
						Assert::AreEqual(ref[c], static_cast<double>(gpuSrgb[i][c]), 2e-3,
							(L"sRGB GPU vs CPU mismatch at pixel " + std::to_wstring(i) +
								L" ch " + std::to_wstring(c)).c_str());
					}
				}

				// Mandatory in-test negative control (exact same inputs rendered with transferKind = 0,
				// pure power-law gamma = 2.4): proves the sRGB piecewise curve genuinely ran on GPU.
				AppearanceStage::Inputs inPow = inSrgb;
				inPow.transferKind = static_cast<int>(ExtendedRange::TransferKind::Power);
				inPow.transferParam = 0.0;
				RoomLight::Smoother smootherPow;
				const AppearanceStage::Decision dPow = AppearanceStage::Decide(inPow, smootherPow);
				Assert::IsTrue(dPow.active);
				ApplyParameters(apHook.hook, dPow.parameters);

				const auto gpuPow = RenderFloatPixels(fixture, apHook.hook, PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24, srgbPixels);
				AssertAllFinite(gpuPow, L"sRGB negative-control pure power-2.4 GPU pass");

				double maxDelta = 0.0;
				double maxToeShadowDelta = 0.0;
				for (size_t i = 0; i < srgbPixels.size(); ++i)
				{
					const std::array<double, 3> refPow = CpuReferenceAppearancePixel(
						inPow, dPow, { { srgbPixels[i][0], srgbPixels[i][1], srgbPixels[i][2] } });
					for (int c = 0; c < 3; ++c)
					{
						Assert::AreEqual(refPow[c], static_cast<double>(gpuPow[i][c]), 2e-3,
							(L"sRGB negative-control power-2.4 GPU vs CPU mismatch at pixel " + std::to_wstring(i) +
								L" ch " + std::to_wstring(c)).c_str());
						const double delta = std::fabs(static_cast<double>(gpuSrgb[i][c]) - static_cast<double>(gpuPow[i][c]));
						maxDelta = (std::max)(maxDelta, delta);
						if (i < kSrgbToeShadowCount)
							maxToeShadowDelta = (std::max)(maxToeShadowDelta, delta);
					}
				}
				Assert::IsTrue(maxToeShadowDelta > 0.007,
					(L"Negative control: sRGB GPU output must diverge from pure power-2.4 across the linear toe/shadows (maxToeShadowDelta=" +
						std::to_wstring(maxToeShadowDelta) + L")").c_str());
				Assert::IsTrue(maxDelta > 0.015,
					(L"Negative control: sRGB GPU output must diverge measurably from pure power-2.4 (maxDelta=" +
						std::to_wstring(maxDelta) + L")").c_str());
			}

			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}
	};
}
