// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Red-Team adversarial tests for CF-DV P5 (DmPreDeband policy + shaders/vp_dm_pre_deband.glsl)
// and CF-DV P6 (12-bit 4096-code monotonic GPU ramp at PRE_OUTPUT with fbo_float32).
// Every invariant includes an in-test naive/unprotected negative control on the exact same inputs
// and independent CPU reference math (DvMath / DvDolbyStyle / DmCurve).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/colour/display_mapping/DmPreDeband.h>
#include <VP-V/modules/colour/display_mapping/DmCurve.h>
#include <VP-V/modules/colour/display_mapping/DvDolbyStyle.h>
#include <VP-V/modules/colour/display_mapping/DvMath.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		constexpr double kCode12Max = 4095.0;
		constexpr double kEightBitStep12 = 16.0; // 2^(12 - 8) = 16 codes per 8-bit step in 12-bit space

		inline std::string ToLowerAscii(std::string s)
		{
			for (char& ch : s)
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			return s;
		}

		inline bool StartsWith(const std::string& text, const char* prefix)
		{
			const size_t len = std::strlen(prefix);
			return text.size() >= len && text.compare(0, len, prefix) == 0;
		}

		inline const double* FindParam(const std::vector<std::pair<const char*, double>>& list, const char* name)
		{
			for (const auto& entry : list)
				if (entry.first && std::strcmp(entry.first, name) == 0)
					return &entry.second;
			return nullptr;
		}

		// Independent CPU BT.2020 PQ RGB <-> BT.2100 ICtCp helpers built directly on DvMath matrices.
		inline void Bt2020PqRgbToIctcp(const double rgbPq[3], double ictcp[3])
		{
			double lin[3] = {
				DvMath::NitsFromPq(rgbPq[0]) / 10000.0,
				DvMath::NitsFromPq(rgbPq[1]) / 10000.0,
				DvMath::NitsFromPq(rgbPq[2]) / 10000.0
			};
			double lms[3] = {};
			DvMath::Mul(DvMath::kRgb2020ToLms, lin, lms);
			double lmsPq[3] = {
				DvMath::PqFromNits(lms[0] * 10000.0),
				DvMath::PqFromNits(lms[1] * 10000.0),
				DvMath::PqFromNits(lms[2] * 10000.0)
			};
			DvMath::Mul(DvMath::kLmsToIctcp, lmsPq, ictcp);
		}

		inline void IctcpToBt2020PqRgb(const double ictcp[3], double rgbPq[3])
		{
			double invIctcp[3][3] = {};
			double invLms[3][3] = {};
			Assert::IsTrue(DvMath::Invert(DvMath::kLmsToIctcp, invIctcp), L"kLmsToIctcp must invert");
			Assert::IsTrue(DvMath::Invert(DvMath::kRgb2020ToLms, invLms), L"kRgb2020ToLms must invert");

			double lmsPq[3] = {};
			DvMath::Mul(invIctcp, ictcp, lmsPq);
			double lms[3] = {
				DvMath::NitsFromPq(lmsPq[0]) / 10000.0,
				DvMath::NitsFromPq(lmsPq[1]) / 10000.0,
				DvMath::NitsFromPq(lmsPq[2]) / 10000.0
			};
			double lin[3] = {};
			DvMath::Mul(invLms, lms, lin);
			for (int c = 0; c < 3; ++c)
				rgbPq[c] = DvMath::PqFromNits(lin[c] * 10000.0);
		}

		// RAII guards for libplacebo textures, parsed hooks, and log capture so assertions never leak device state.
		struct ScopedTex
		{
			pl_gpu gpu = nullptr;
			pl_tex tex = nullptr;
			~ScopedTex()
			{
				if (gpu && tex)
					pl_tex_destroy(gpu, &tex);
			}
		};

		struct ScopedHook
		{
			const pl_hook* hook = nullptr;
			~ScopedHook()
			{
				if (hook)
					pl_mpv_user_shader_destroy(&hook);
			}
		};

		struct ScopedLogCapture
		{
			TargetLutGpuFixture* fixture = nullptr;
			ScopedLogCapture(TargetLutGpuFixture& f, std::string& sink) : fixture(&f)
			{
				fixture->CaptureLog(&sink);
			}
			~ScopedLogCapture()
			{
				if (fixture)
					fixture->CaptureLog(nullptr);
			}
		};

		inline pl_frame MakeFloatRgbFrame(pl_tex tex, enum pl_color_primaries prim, enum pl_color_transfer trc)
		{
			pl_frame f{};
			f.num_planes = 1;
			f.planes[0].texture = tex;
			f.planes[0].components = 4;
			f.planes[0].component_mapping[0] = 0;
			f.planes[0].component_mapping[1] = 1;
			f.planes[0].component_mapping[2] = 2;
			f.planes[0].component_mapping[3] = 3;
			f.repr.sys = PL_COLOR_SYSTEM_RGB;
			f.repr.levels = PL_COLOR_LEVELS_FULL;
			f.color.primaries = prim;
			f.color.transfer = trc;
			return f;
		}

		inline void LoadBundledHook(TargetLutGpuFixture& fixture, const char* fileName,
			enum pl_hook_stage expectedStage, ScopedHook& out)
		{
			const std::string source = LoadBundledShader(fileName);
			const std::wstring wName(fileName, fileName + std::strlen(fileName));
			Assert::IsFalse(source.empty(), (L"Bundled shader not found: " + wName).c_str());
			out.hook = pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
			Assert::IsNotNull(out.hook, (L"libplacebo rejected bundled shader: " + wName).c_str());
			Assert::IsTrue((out.hook->stages & expectedStage) != 0,
				(L"Bundled shader did not bind expected hook stage: " + wName).c_str());
		}

		inline void SetHookFloatParam(const pl_hook* hook, const char* name, double value)
		{
			const std::wstring wName(name, name + std::strlen(name));
			Assert::IsTrue(CustomHookChain::SetHookParameter(hook, name, value),
				(L"SetHookParameter failed for " + wName).c_str());
		}

		inline void BindPreDebandUniforms(const pl_hook* hook, bool active, double thresholdNorm,
			double radius, bool grainGuard, bool ictcpInput)
		{
			SetHookFloatParam(hook, "dm_deband_active", active ? 1.0 : 0.0);
			SetHookFloatParam(hook, "dm_deband_thr", thresholdNorm);
			SetHookFloatParam(hook, "dm_deband_radius", radius);
			SetHookFloatParam(hook, "dm_deband_grain_guard", grainGuard ? 1.0 : 0.0);
			SetHookFloatParam(hook, "dm_deband_ictcp_input", ictcpInput ? 1.0 : 0.0);
		}

		inline void BindNeutralDvTrims(const pl_hook* trimsHook, double targetPeakNits)
		{
			SetHookFloatParam(trimsHook, "dv_slope", 1.0);
			SetHookFloatParam(trimsHook, "dv_offset", 0.0);
			SetHookFloatParam(trimsHook, "dv_power", 1.0);
			SetHookFloatParam(trimsHook, "dv_chroma_weight", 0.0);
			SetHookFloatParam(trimsHook, "dv_sat_gain", 1.0);
			SetHookFloatParam(trimsHook, "dv_signal_pq", 1.0);
			SetHookFloatParam(trimsHook, "dv_peak_nits", targetPeakNits);
			SetHookFloatParam(trimsHook, "dv_peak_pq", DvMath::PqFromNits(targetPeakNits));
			SetHookFloatParam(trimsHook, "dv_near_black_nits", DvMath::NitsFromPq(64.0 / kCode12Max));
			SetHookFloatParam(trimsHook, "dv_ext", 0.0);
			SetHookFloatParam(trimsHook, "dv_rotate", 0.0);
			const auto spaceParams = DvDolbyStyle::ShaderParameters(true, pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020));
			for (const auto& kv : spaceParams)
				SetHookFloatParam(trimsHook, kv.first, kv.second);
		}

		// Renders an RGBA32F image 1:1 through a single hook into an RGBA32F target with float32 FBOs
		// and identical source/target colour space so only the hook acts.
		inline std::vector<float> RenderRgba32fWithHook(
			TargetLutGpuFixture& fixture,
			int width,
			int height,
			const std::vector<float>& rgbaIn,
			const pl_hook* hook,
			enum pl_color_primaries primaries,
			enum pl_color_transfer transfer)
		{
			Assert::AreEqual(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, rgbaIn.size());
			pl_gpu gpu = fixture.Gpu();
			const pl_fmt fmt32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, static_cast<enum pl_fmt_caps>(
				PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(fmt32, L"No host-readable RGBA32F format is available");

			ScopedTex src{ gpu, nullptr };
			pl_tex_params sp{};
			sp.w = width;
			sp.h = height;
			sp.format = fmt32;
			sp.sampleable = true;
			sp.initial_data = rgbaIn.data();
			src.tex = pl_tex_create(gpu, &sp);
			Assert::IsNotNull(src.tex, L"Failed to create RGBA32F source texture");

			ScopedTex dst{ gpu, nullptr };
			pl_tex_params dp{};
			dp.w = width;
			dp.h = height;
			dp.format = fmt32;
			dp.renderable = true;
			dp.host_readable = true;
			dst.tex = pl_tex_create(gpu, &dp);
			Assert::IsNotNull(dst.tex, L"Failed to create RGBA32F target texture");

			pl_frame image = MakeFloatRgbFrame(src.tex, primaries, transfer);
			pl_frame out = MakeFloatRgbFrame(dst.tex, primaries, transfer);
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
			params.fbo_float32 = true;
#endif
#if defined(PL_RENDER_PARAMS_OUTPUT_FBO_FLOAT32)
			params.output_fbo_float32 = true;
#endif
			if (hook)
			{
				params.hooks = &hook;
				params.num_hooks = 1;
			}
			Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params), L"pl_render_image failed");
			pl_gpu_finish(gpu);

			std::vector<float> rgbaOut(rgbaIn.size(), 0.0f);
			pl_tex_transfer_params dl{};
			dl.tex = dst.tex;
			dl.ptr = rgbaOut.data();
			Assert::IsTrue(pl_tex_download(gpu, &dl), L"pl_tex_download failed");
			return rgbaOut;
		}

		inline void AssertAllRgbFinite(const std::vector<float>& rgba, const wchar_t* label)
		{
			for (size_t i = 0; i + 3 < rgba.size(); i += 4)
			{
				for (int c = 0; c < 3; ++c)
				{
					Assert::IsTrue(std::isfinite(rgba[i + static_cast<size_t>(c)]),
						(std::wstring(label) + L": non-finite output at pixel " + std::to_wstring(i / 4) +
							L" ch " + std::to_wstring(c)).c_str());
				}
			}
		}
	}

	// =========================================================================
	// Pure CPU policy & 12-bit tap-law tests (CF-DV P5)
	// =========================================================================
	TEST_CLASS(DmPreDebandPolicyTests)
	{
	public:
		TEST_METHOD(DefaultOffAndOriginalPathwayStayInactiveWithoutFallback)
		{
			Assert::IsTrue(DmPreDeband::ParseMode("off") == DmPreDeband::Mode::Off);
			Assert::IsTrue(DmPreDeband::ParseMode("") == DmPreDeband::Mode::Off);
			Assert::IsTrue(DmPreDeband::ParseMode("unexpected") == DmPreDeband::Mode::Off);
			Assert::IsTrue(DmPreDeband::ParseMode("auto") == DmPreDeband::Mode::Auto);

			// Shared baseline that would strongly activate pre-deband when enabled.
			DmPreDeband::Inputs base;
			base.dolbyVisionPicture = true;
			base.slotRuns = true;
			base.originalPathway = false;
			base.tapAvailable = true;
			base.qualityAvailable = true;
			base.effectiveBits = 8.0;
			base.bitrateQuality = 0.4;

			// 1. Default mode = "off": inactive, no fallback, unified "DV deband: off (" status.
			{
				DmPreDeband::Inputs in = base;
				Assert::AreEqual(std::string("off"), in.mode, L"default mode must be off");
				const DmPreDeband::Decision d = DmPreDeband::Decide(in);
				Assert::IsFalse(d.active, L"mode=off must stay inactive");
				Assert::IsFalse(d.fallbackEngaged, L"mode=off is user choice, never a fallback");
				Assert::AreEqual(0.0, d.thresholdCodes, 0.0);
				Assert::AreEqual(0.0, d.thresholdNorm, 0.0);
				Assert::IsTrue(StartsWith(d.status, "DV deband: off ("), L"unified off status prefix");
				const auto params = DmPreDeband::ShaderParameters(d, 1080, false);
				const double* activeParam = FindParam(params, "dm_deband_active");
				Assert::IsNotNull(activeParam, L"ShaderParameters must emit dm_deband_active");
				Assert::AreEqual(0.0, *activeParam, 0.0, L"inactive decision emits dm_deband_active = 0");
			}

			// 2. Original pathway with mode = "auto": inactive, NO fallback (even if tap/quality absent).
			{
				DmPreDeband::Inputs in = base;
				in.mode = "auto";
				in.originalPathway = true;
				const DmPreDeband::Decision d = DmPreDeband::Decide(in);
				Assert::IsFalse(d.active, L"originalPathway=true must stay inactive");
				Assert::IsFalse(d.fallbackEngaged, L"originalPathway=true must never engage fallback");
				Assert::IsTrue(StartsWith(d.status, "DV deband: off ("), L"originalPathway off status prefix");

				in.tapAvailable = false;
				in.qualityAvailable = false;
				const DmPreDeband::Decision dNoTap = DmPreDeband::Decide(in);
				Assert::IsFalse(dNoTap.active);
				Assert::IsFalse(dNoTap.fallbackEngaged, L"originalPathway stays off, never fallback");
				Assert::IsTrue(StartsWith(dNoTap.status, "DV deband: off ("));
			}

			// 3. Slot disabled or non-DV picture: inactive, no fallback.
			{
				DmPreDeband::Inputs slotOff = base;
				slotOff.mode = "auto";
				slotOff.slotRuns = false;
				const DmPreDeband::Decision dSlotOff = DmPreDeband::Decide(slotOff);
				Assert::IsFalse(dSlotOff.active);
				Assert::IsFalse(dSlotOff.fallbackEngaged);
				Assert::IsTrue(StartsWith(dSlotOff.status, "DV deband: off ("));

				DmPreDeband::Inputs nonDv = base;
				nonDv.mode = "auto";
				nonDv.dolbyVisionPicture = false;
				const DmPreDeband::Decision dNonDv = DmPreDeband::Decide(nonDv);
				Assert::IsFalse(dNonDv.active);
				Assert::IsFalse(dNonDv.fallbackEngaged);
				Assert::IsTrue(StartsWith(dNonDv.status, "DV deband: off ("));
			}

			// Mandatory in-test negative control: exact same inputs with mode="auto" and originalPathway=false
			// MUST activate with a strictly positive 12-bit threshold and normalized threshold == codes / 4095.
			{
				DmPreDeband::Inputs activeIn = base;
				activeIn.mode = "auto";
				activeIn.originalPathway = false;
				const DmPreDeband::Decision d = DmPreDeband::Decide(activeIn);
				Assert::IsTrue(d.active, L"Negative control: mode=auto on banded DV picture must activate");
				Assert::IsFalse(d.fallbackEngaged, L"Active decision must not engage fallback");
				Assert::IsTrue(d.thresholdCodes > 0.0, L"Active decision must produce positive thresholdCodes");
				Assert::AreEqual(d.thresholdCodes / kCode12Max, d.thresholdNorm, 1e-12,
					L"thresholdNorm must equal thresholdCodes / 4095.0");
				Assert::IsFalse(StartsWith(d.status, "DV deband: off ("), L"Active status must not say off");

				const auto paramsRgb = DmPreDeband::ShaderParameters(d, 1080, false);
				const double* pActive = FindParam(paramsRgb, "dm_deband_active");
				const double* pThr = FindParam(paramsRgb, "dm_deband_thr");
				const double* pRad = FindParam(paramsRgb, "dm_deband_radius");
				const double* pGuard = FindParam(paramsRgb, "dm_deband_grain_guard");
				const double* pIctcp = FindParam(paramsRgb, "dm_deband_ictcp_input");
				Assert::IsTrue(pActive && pThr && pRad && pGuard && pIctcp,
					L"ShaderParameters must publish all five dm_deband_* uniforms");
				Assert::AreEqual(1.0, *pActive, 0.0);
				Assert::AreEqual(d.thresholdNorm, *pThr, 1e-12);
				Assert::IsTrue(*pRad > 0.0);
				Assert::AreEqual(1.0, *pGuard, 0.0);
				Assert::AreEqual(0.0, *pIctcp, 0.0);

				const auto paramsIctcp = DmPreDeband::ShaderParameters(d, 1080, true);
				const double* pIctcpOn = FindParam(paramsIctcp, "dm_deband_ictcp_input");
				Assert::IsNotNull(pIctcpOn);
				Assert::AreEqual(1.0, *pIctcpOn, 0.0);
			}
		}

		TEST_METHOD(AbsentTapOrQualityShowsVisibleStatusAndEngagesFallback)
		{
			DmPreDeband::Inputs base;
			base.dolbyVisionPicture = true;
			base.slotRuns = true;
			base.originalPathway = false;
			base.mode = "auto";
			base.effectiveBits = 8.0;
			base.bitrateQuality = 0.4;

			// Case 1: tap absent -> inactive, visible fallback mentioning tap / no private decode.
			{
				DmPreDeband::Inputs in = base;
				in.tapAvailable = false;
				in.qualityAvailable = true;
				const DmPreDeband::Decision d = DmPreDeband::Decide(in);
				Assert::IsFalse(d.active, L"absent tap must not activate pre-deband");
				Assert::IsTrue(d.fallbackEngaged, L"absent tap must engage visible fallback");
				Assert::IsFalse(d.reason.empty(), L"absent tap must carry explicit reason");
				Assert::IsTrue(StartsWith(d.status, "DV deband: off ("), L"unified off status prefix");
				const std::string lowerStatus = ToLowerAscii(d.status);
				Assert::IsTrue(
					lowerStatus.find("tap") != std::string::npos ||
					lowerStatus.find("no private decode") != std::string::npos,
					L"absent tap status must visibly mention tap / no private decode");
			}

			// Case 2: tap present, P14 quality absent -> inactive, visible fallback mentioning P14.
			{
				DmPreDeband::Inputs in = base;
				in.tapAvailable = true;
				in.qualityAvailable = false;
				const DmPreDeband::Decision d = DmPreDeband::Decide(in);
				Assert::IsFalse(d.active, L"absent P14 quality must not activate pre-deband");
				Assert::IsTrue(d.fallbackEngaged, L"absent P14 quality must engage visible fallback");
				Assert::IsFalse(d.reason.empty(), L"absent P14 quality must carry explicit reason");
				Assert::IsTrue(StartsWith(d.status, "DV deband: off ("), L"unified off status prefix");
				const std::string lowerStatus = ToLowerAscii(d.status);
				Assert::IsTrue(lowerStatus.find("p14") != std::string::npos,
					L"absent quality status must visibly mention P14");
			}

			// Case 3: both absent -> still visible fallback, never silent.
			{
				DmPreDeband::Inputs in = base;
				in.tapAvailable = false;
				in.qualityAvailable = false;
				const DmPreDeband::Decision d = DmPreDeband::Decide(in);
				Assert::IsFalse(d.active);
				Assert::IsTrue(d.fallbackEngaged);
				Assert::IsTrue(StartsWith(d.status, "DV deband: off ("));
			}

			// Mandatory in-test negative control: exact same inputs with BOTH tapAvailable=true and
			// qualityAvailable=true clears fallbackEngaged and activates pre-deband.
			{
				DmPreDeband::Inputs in = base;
				in.tapAvailable = true;
				in.qualityAvailable = true;
				const DmPreDeband::Decision d = DmPreDeband::Decide(in);
				Assert::IsTrue(d.active, L"Negative control: tap+quality present must activate");
				Assert::IsFalse(d.fallbackEngaged, L"Negative control: tap+quality present clears fallback");
				Assert::IsTrue(d.thresholdCodes > 0.0);
				Assert::AreEqual(d.thresholdCodes / kCode12Max, d.thresholdNorm, 1e-12);
				Assert::IsFalse(StartsWith(d.status, "DV deband: off ("));
			}
		}

		TEST_METHOD(TapLawRoundTripsAll4096TwelveBitCodes)
		{
			for (uint16_t c = 0; c <= 4095u; ++c)
			{
				const uint16_t shifted = DmPreDeband::Tap16FromCode12(c);
				Assert::AreEqual(static_cast<unsigned int>(c << 4), static_cast<unsigned int>(shifted),
					L"Tap16FromCode12 must place 12-bit code in MSBs (c << 4)");
				Assert::AreEqual(static_cast<unsigned int>(c),
					static_cast<unsigned int>(DmPreDeband::TapCode12FromIntensity16(shifted)),
					L"TapCode12FromIntensity16 must round-trip every 12-bit code");
				// Lower 4 padding bits must not perturb the 12-bit code.
				Assert::AreEqual(static_cast<unsigned int>(c),
					static_cast<unsigned int>(DmPreDeband::TapCode12FromIntensity16(static_cast<uint16_t>(shifted | 0x000Fu))),
					L"low 4 bits must shift out cleanly");
				// High 4 out-of-range bits above 12-bit must be masked cleanly by Tap16FromCode12.
				Assert::AreEqual(static_cast<unsigned int>(c << 4),
					static_cast<unsigned int>(DmPreDeband::Tap16FromCode12(static_cast<uint16_t>(c | 0xF000u))),
					L"Tap16FromCode12 must mask to 12 bits");
			}

			// Mandatory in-test negative control: 10-bit truncation (c & 0x0FFCu) << 4 on the exact
			// same 4096 codes destroys 3 out of every 4 codes (exact 3072 lost codes).
			int lost10Bit = 0;
			for (uint16_t c = 0; c <= 4095u; ++c)
			{
				const uint16_t truncated10 = static_cast<uint16_t>((c & 0x0FFCu) << 4);
				if (DmPreDeband::TapCode12FromIntensity16(truncated10) != c)
					++lost10Bit;
			}
			Assert::AreEqual(3072, lost10Bit,
				L"Negative control: 10-bit truncation loses exactly 3072 of 4096 twelve-bit codes");
		}
	};

	// =========================================================================
	// WARP GPU tests (CF-DV P5 shader invariants + CF-DV P6 4096-code ramp row)
	// =========================================================================
	TEST_CLASS(DmPreDebandGpuTests)
	{
	public:
		// CF-DV P6 required GPU row: all 4096 consecutive 12-bit PQ codes (0..4095) rendered through
		// libplacebo's DV display-mapping path (DvDolbyStyle::Function() + neutral DV trims.glsl at
		// PRE_OUTPUT) with float32 FBOs into rgba32f stay strictly monotonic with steps bounded by
		// the analytic derivative (<= 1.05 codes on identity; <= analytic + 2*kMaxErrorCodes on active
		// 4000->600 nit curve). Same-input 8-bit rgba8 negative control jumps by >= 15 12-bit codes.
		BEGIN_TEST_METHOD_ATTRIBUTE(TwelveBitMonotonic4096CodeRampAtPreOutputWithFloat32Fbo)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TwelveBitMonotonic4096CodeRampAtPreOutputWithFloat32Fbo)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create libplacebo WARP GPU fixture");
			std::string cleanLog;
			ScopedLogCapture logGuard(fixture, cleanLog);

			ScopedHook trimsHook;
			LoadBundledHook(fixture, "DV trims.glsl", PL_HOOK_PRE_OUTPUT, trimsHook);

			constexpr int kWidth = 4096;
			constexpr int kHeight = 4;
			std::vector<float> rampRgba(static_cast<size_t>(kWidth) * kHeight * 4u, 1.0f);
			for (int y = 0; y < kHeight; ++y)
			{
				for (int c = 0; c < kWidth; ++c)
				{
					const float pq = static_cast<float>(static_cast<double>(c) / kCode12Max);
					const size_t idx = (static_cast<size_t>(y) * kWidth + static_cast<size_t>(c)) * 4u;
					rampRgba[idx + 0] = pq;
					rampRgba[idx + 1] = pq;
					rampRgba[idx + 2] = pq;
					rampRgba[idx + 3] = 1.0f;
				}
			}

			pl_gpu gpu = fixture.Gpu();
			const pl_fmt fmt32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, static_cast<enum pl_fmt_caps>(
				PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(fmt32, L"No host-readable RGBA32F format is available");

			ScopedTex src{ gpu, nullptr };
			pl_tex_params sp{};
			sp.w = kWidth;
			sp.h = kHeight;
			sp.format = fmt32;
			sp.sampleable = true;
			sp.initial_data = rampRgba.data();
			src.tex = pl_tex_create(gpu, &sp);
			Assert::IsNotNull(src.tex, L"Failed to create 4096x4 RGBA32F ramp source texture");

			ScopedTex dst32{ gpu, nullptr };
			pl_tex_params dp32{};
			dp32.w = kWidth;
			dp32.h = kHeight;
			dp32.format = fmt32;
			dp32.renderable = true;
			dp32.host_readable = true;
			dst32.tex = pl_tex_create(gpu, &dp32);
			Assert::IsNotNull(dst32.tex, L"Failed to create 4096x4 RGBA32F ramp target texture");

			auto renderRampFloat32 = [&](float srcMaxNits, float dstMaxNits, int lutSize) -> std::vector<float>
			{
				BindNeutralDvTrims(trimsHook.hook, dstMaxNits);

				pl_frame image = MakeFloatRgbFrame(src.tex, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
				image.color.hdr.min_luma = 0.005f;
				image.color.hdr.max_luma = srcMaxNits;
				image.color.hdr.max_pq_y = static_cast<float>(DvMath::PqFromNits(srcMaxNits));
				image.color.hdr.avg_pq_y = 0.0f;

				pl_frame out = MakeFloatRgbFrame(dst32.tex, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
				out.color.hdr.min_luma = 0.005f;
				out.color.hdr.max_luma = dstMaxNits;

				pl_color_map_params cmap = pl_color_map_default_params;
				cmap.tone_mapping_function = &DvDolbyStyle::Function();
				cmap.contrast_recovery = 0.0f;
				if (lutSize > 0)
					cmap.lut_size = lutSize;

				pl_render_params params = pl_render_fast_params;
				params.color_map_params = &cmap;
				params.peak_detect_params = nullptr;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
				params.fbo_float32 = true;
#endif
#if defined(PL_RENDER_PARAMS_OUTPUT_FBO_FLOAT32)
				params.output_fbo_float32 = true;
#endif
				params.hooks = &trimsHook.hook;
				params.num_hooks = 1;

				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params),
					L"pl_render_image failed on 4096-code DV ramp");
				pl_gpu_finish(gpu);

				std::vector<float> dlBuf(rampRgba.size(), 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = dst32.tex;
				dl.ptr = dlBuf.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"pl_tex_download failed on 4096-code DV ramp");
				AssertAllRgbFinite(dlBuf, L"4096-code DV ramp float32");

				std::vector<float> row(kWidth, 0.0f);
				const size_t rowOffset = static_cast<size_t>(2) * kWidth * 4u;
				for (int c = 0; c < kWidth; ++c)
				{
					const size_t idx = rowOffset + static_cast<size_t>(c) * 4u;
					Assert::IsTrue(std::fabs(dlBuf[idx + 0] - dlBuf[idx + 1]) < 1e-4f &&
						std::fabs(dlBuf[idx + 1] - dlBuf[idx + 2]) < 1e-4f,
						L"4096-code ramp drifted off achromatic neutral");
					row[static_cast<size_t>(c)] = dlBuf[idx + 0];
				}
				return row;
			};

			// Pass A: Identity display mapping (1000 -> 1000 nits) + neutral ICtCp DV trims at PRE_OUTPUT.
			// Exact theoretical derivative is 1.0 -> step = 1/4095 per adjacent 12-bit code (+5% fp32 margin).
			{
				const std::vector<float> out = renderRampFloat32(1000.0f, 1000.0f, 0);
				Assert::IsTrue(out[4095] > out[0] + 0.5f, L"Identity ramp must span > 0.5 full PQ range");

				float maxStep = 0.0f;
				int advancingSteps = 0;
				const float identityStepBound = static_cast<float>((1.0 / kCode12Max) * 1.05);
				for (int c = 0; c < kWidth - 1; ++c)
				{
					const float step = out[static_cast<size_t>(c + 1)] - out[static_cast<size_t>(c)];
					Assert::IsTrue(step >= -1e-6f,
						(L"Identity 12-bit ramp non-monotonic at code " + std::to_wstring(c)).c_str());
					maxStep = (std::max)(maxStep, step);
					if (step > static_cast<float>(0.5 / kCode12Max))
						++advancingSteps;
				}
				Assert::IsTrue(maxStep <= identityStepBound,
					(L"Identity 12-bit ramp step exceeded 1.05 codes: maxStep=" +
						std::to_wstring(maxStep * kCode12Max)).c_str());
				Assert::IsTrue(advancingSteps >= 4090,
					(L"Identity 12-bit ramp collapsed adjacent codes: advancing=" +
						std::to_wstring(advancingSteps)).c_str());
			}

			// Pass B: Active compressing DvDolbyStyle::Function() curve (4000 -> 600 nits) with LUT size
			// selected by DmCurve::ChooseLutSize + neutral ICtCp DV trims at PRE_OUTPUT.
			// Step bound is derived from the exact ST 2094-10 analytic derivative plus DmCurve LUT budget.
			{
				const DvDolbyStyle::Curve curve = DvDolbyStyle::Build(
					DvMath::PqFromNits(0.005), 0.0, DvMath::PqFromNits(4000.0),
					DvMath::PqFromNits(0.005), DvMath::PqFromNits(600.0));
				Assert::IsFalse(curve.identity, L"4000->600 nit DV curve must actively compress");
				const DmCurve::LutChoice choice = DmCurve::ChooseLutSize(curve);
				Assert::IsTrue(choice.size >= 256, L"DmCurve::ChooseLutSize must select a valid LUT size");

				double maxAnalyticStep = 0.0;
				for (int c = 0; c < kWidth - 1; ++c)
				{
					const double s = curve.Map(static_cast<double>(c + 1) / kCode12Max) -
						curve.Map(static_cast<double>(c) / kCode12Max);
					maxAnalyticStep = (std::max)(maxAnalyticStep, s);
				}
				const double physicsStepBound = maxAnalyticStep + (2.0 * DmCurve::kMaxErrorCodes) / kCode12Max;

				const std::vector<float> out = renderRampFloat32(4000.0f, 600.0f, static_cast<int>(choice.size));
				Assert::IsTrue(out[4095] > out[0] + 0.5f, L"Compressing DV ramp must span > 0.5 PQ range");
				// Verify the tone curve actually compressed 4000 nits (code 3696) down toward 600 nits.
				Assert::IsTrue(out[3696] < static_cast<float>(3696.0 / kCode12Max) - 0.10f,
					L"Active DvDolbyStyle::Function() must compress 4000-nit highlight toward 600-nit target");

				double maxStep = 0.0;
				for (int c = 0; c < kWidth - 1; ++c)
				{
					const double step = static_cast<double>(out[static_cast<size_t>(c + 1)]) -
						static_cast<double>(out[static_cast<size_t>(c)]);
					Assert::IsTrue(step >= -1e-5,
						(L"Active DV ramp non-monotonic at code " + std::to_wstring(c)).c_str());
					maxStep = (std::max)(maxStep, step);
				}
				Assert::IsTrue(maxStep <= physicsStepBound,
					(L"Active DV ramp step exceeded analytic + LUT budget: observed=" +
						std::to_wstring(maxStep * kCode12Max) + L" codes, bound=" +
						std::to_wstring(physicsStepBound * kCode12Max)).c_str());
			}

			// Pass C (Mandatory in-test negative control): render the EXACT SAME 4096-code ramp through
			// the same pipeline into an 8-bit rgba8 target (no dither) and prove step jumps exceed >= 15
			// twelve-bit codes (> 15/4095) and collapse >= 3800 adjacent steps.
			{
				const pl_fmt fmt8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, static_cast<enum pl_fmt_caps>(
					PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				Assert::IsNotNull(fmt8, L"No host-readable RGBA8 format available for negative control");

				ScopedTex dst8{ gpu, nullptr };
				pl_tex_params dp8{};
				dp8.w = kWidth;
				dp8.h = kHeight;
				dp8.format = fmt8;
				dp8.renderable = true;
				dp8.host_readable = true;
				dst8.tex = pl_tex_create(gpu, &dp8);
				Assert::IsNotNull(dst8.tex, L"Failed to create 4096x4 RGBA8 target texture");

				BindNeutralDvTrims(trimsHook.hook, 1000.0);
				pl_frame image = MakeFloatRgbFrame(src.tex, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
				image.color.hdr.min_luma = 0.005f;
				image.color.hdr.max_luma = 1000.0f;
				pl_frame out8 = MakeFloatRgbFrame(dst8.tex, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
				out8.color.hdr.min_luma = 0.005f;
				out8.color.hdr.max_luma = 1000.0f;

				pl_color_map_params cmap = pl_color_map_default_params;
				cmap.tone_mapping_function = &DvDolbyStyle::Function();
				cmap.contrast_recovery = 0.0f;

				pl_render_params params = pl_render_fast_params;
				params.color_map_params = &cmap;
				params.peak_detect_params = nullptr;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.hooks = &trimsHook.hook;
				params.num_hooks = 1;

				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out8, &params),
					L"Negative control rgba8 pl_render_image failed");
				pl_gpu_finish(gpu);

				std::vector<uint8_t> raw8(static_cast<size_t>(kWidth) * kHeight * 4u, 0);
				pl_tex_transfer_params dl{};
				dl.tex = dst8.tex;
				dl.ptr = raw8.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"Negative control rgba8 download failed");

				float maxStep8 = 0.0f;
				int collapsedSteps = 0;
				const size_t rowOffset = static_cast<size_t>(2) * kWidth * 4u;
				for (int c = 0; c < kWidth - 1; ++c)
				{
					const float a = static_cast<float>(raw8[rowOffset + static_cast<size_t>(c) * 4u]) / 255.0f;
					const float b = static_cast<float>(raw8[rowOffset + static_cast<size_t>(c + 1) * 4u]) / 255.0f;
					const float step = b - a;
					maxStep8 = (std::max)(maxStep8, step);
					if (step == 0.0f)
						++collapsedSteps;
				}
				Assert::IsTrue(maxStep8 > static_cast<float>(15.0 / kCode12Max),
					(L"Negative control expected 8-bit step jump > 15 twelve-bit codes, got " +
						std::to_wstring(maxStep8 * kCode12Max)).c_str());
				Assert::IsTrue(collapsedSteps >= 3800,
					(L"Negative control expected >= 3800 collapsed steps in 8-bit, got " +
						std::to_wstring(collapsedSteps)).c_str());
			}

			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog, "TwelveBitMonotonic4096CodeRampAtPreOutputWithFloat32Fbo");
		}

		// CF-DV P5 GPU invariant: vp_dm_pre_deband.glsl smooths banded intensity I while leaving
		// Ct and Cp BIT-FOR-BIT UNTOUCHED in ICtCp mode (and preserved within < 1e-5 in BT.2020 PQ
		// RGB mode). Negative control: dm_deband_active = 0.0 is byte-identical pass-through.
		BEGIN_TEST_METHOD_ATTRIBUTE(PreDebandModifiesIntensityOnlyAndLeavesCtCpUntouched)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PreDebandModifiesIntensityOnlyAndLeavesCtCpUntouched)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create libplacebo WARP GPU fixture");
			std::string cleanLog;
			ScopedLogCapture logGuard(fixture, cleanLog);

			ScopedHook debandHook;
			LoadBundledHook(fixture, "vp_dm_pre_deband.glsl", PL_HOOK_RGB, debandHook);

			constexpr int kW = 128;
			constexpr int kH = 64;
			constexpr int kMargin = 16;
			constexpr double kThrNorm = 32.0 / kCode12Max;
			constexpr double kRadius = 12.0;

			// Shallow 6-band staircase across x (~21 px per band, exact 8-bit step = 16/4095 in 12-bit PQ I).
			auto smoothI = [&](int x) -> double {
				return (1600.0 + 96.0 * (static_cast<double>(x) / static_cast<double>(kW - 1))) / kCode12Max;
			};
			auto bandedI = [&](int x) -> double {
				const double codes = smoothI(x) * kCode12Max;
				return std::floor(codes / kEightBitStep12 + 0.5) * kEightBitStep12 / kCode12Max;
			};

			// Sub-test A: ICtCp input mode (dm_deband_ictcp_input = 1.0) with high-contrast sharp Ct/Cp edges.
			std::vector<float> ictcpIn(static_cast<size_t>(kW) * kH * 4u, 1.0f);
			for (int y = 0; y < kH; ++y)
			{
				for (int x = 0; x < kW; ++x)
				{
					const float iVal = static_cast<float>(bandedI(x));
					// High-contrast sharp checkerboard + 1 px stripe in Ct, quadrant steps in Cp.
					float ctVal = (((x / 8) + (y / 8)) % 2 == 0) ? -0.18f : 0.22f;
					if (x == 40)
						ctVal = 0.31f;
					const float cpVal = (x < kW / 2 ? 0.15f : -0.24f) + (y < kH / 2 ? 0.05f : -0.05f);
					const size_t idx = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
					ictcpIn[idx + 0] = iVal;
					ictcpIn[idx + 1] = ctVal;
					ictcpIn[idx + 2] = cpVal;
					ictcpIn[idx + 3] = 1.0f;
				}
			}

			BindPreDebandUniforms(debandHook.hook, true, kThrNorm, kRadius, true, true);
			const std::vector<float> ictcpOut = RenderRgba32fWithHook(
				fixture, kW, kH, ictcpIn, debandHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
			AssertAllRgbFinite(ictcpOut, L"ICtCp active deband");

			double bandedMae = 0.0;
			double debandedMae = 0.0;
			double maxDiffI = 0.0;
			int interiorCount = 0;
			for (int y = kMargin; y < kH - kMargin; ++y)
			{
				for (int x = kMargin; x < kW - kMargin; ++x)
				{
					const size_t idx = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
					const double gt = smoothI(x);
					bandedMae += std::fabs(static_cast<double>(ictcpIn[idx + 0]) - gt);
					debandedMae += std::fabs(static_cast<double>(ictcpOut[idx + 0]) - gt);
					maxDiffI = (std::max)(maxDiffI,
						std::fabs(static_cast<double>(ictcpOut[idx + 0]) - static_cast<double>(ictcpIn[idx + 0])));
					++interiorCount;
				}
			}
			bandedMae /= static_cast<double>(interiorCount);
			debandedMae /= static_cast<double>(interiorCount);

			float maxDiffCt = 0.0f;
			float maxDiffCp = 0.0f;
			for (size_t i = 0; i + 3 < ictcpIn.size(); i += 4)
			{
				maxDiffCt = (std::max)(maxDiffCt, std::fabs(ictcpOut[i + 1] - ictcpIn[i + 1]));
				maxDiffCp = (std::max)(maxDiffCp, std::fabs(ictcpOut[i + 2] - ictcpIn[i + 2]));
			}

			Assert::IsTrue(bandedMae > 2.0 / kCode12Max, L"Test setup: banded ramp must carry measurable banding MAE");
			Assert::IsTrue(debandedMae < bandedMae * 0.85,
				(L"ICtCp deband must reduce banding MAE below 0.85x (in=" +
					std::to_wstring(bandedMae * kCode12Max) + L", out=" +
					std::to_wstring(debandedMae * kCode12Max) + L" codes)").c_str());
			Assert::IsTrue(maxDiffI > 2.0 / kCode12Max,
				(L"ICtCp deband must actively move I > 2 codes (maxDiffI=" +
					std::to_wstring(maxDiffI * kCode12Max) + L")").c_str());
			Assert::AreEqual(0.0f, maxDiffCt, 0.0f, L"ICtCp mode must leave Ct bit-for-bit untouched");
			Assert::AreEqual(0.0f, maxDiffCp, 0.0f, L"ICtCp mode must leave Cp bit-for-bit untouched");

			// Mandatory in-test negative control: exact same banded ICtCp texture with dm_deband_active = 0.0
			// MUST pass through bit-identically (maxDiffI == 0.0f).
			{
				BindPreDebandUniforms(debandHook.hook, false, kThrNorm, kRadius, true, true);
				const std::vector<float> passThrough = RenderRgba32fWithHook(
					fixture, kW, kH, ictcpIn, debandHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
				float maxDiffOffI = 0.0f;
				float maxDiffOffCt = 0.0f;
				float maxDiffOffCp = 0.0f;
				for (size_t i = 0; i + 3 < ictcpIn.size(); i += 4)
				{
					maxDiffOffI = (std::max)(maxDiffOffI, std::fabs(passThrough[i + 0] - ictcpIn[i + 0]));
					maxDiffOffCt = (std::max)(maxDiffOffCt, std::fabs(passThrough[i + 1] - ictcpIn[i + 1]));
					maxDiffOffCp = (std::max)(maxDiffOffCp, std::fabs(passThrough[i + 2] - ictcpIn[i + 2]));
				}
				Assert::AreEqual(0.0f, maxDiffOffI, 0.0f, L"Negative control: active=0 must leave I byte-identical");
				Assert::AreEqual(0.0f, maxDiffOffCt, 0.0f, L"Negative control: active=0 must leave Ct byte-identical");
				Assert::AreEqual(0.0f, maxDiffOffCp, 0.0f, L"Negative control: active=0 must leave Cp byte-identical");
			}

			// Sub-test B: BT.2020 PQ RGB input mode (dm_deband_ictcp_input = 0.0).
			// Synthesize an in-gamut BT.2020 PQ RGB banded frame with sharp chroma quadrant steps,
			// run vp_dm_pre_deband.glsl, convert back to ICtCp via independent DvMath matrices, and
			// verify I is smoothed while Ct/Cp stay preserved (< 1e-5).
			{
				std::vector<float> rgbIn(static_cast<size_t>(kW) * kH * 4u, 1.0f);
				for (int y = 0; y < kH; ++y)
				{
					for (int x = 0; x < kW; ++x)
					{
						const double cIn[3] = {
							bandedI(x),
							(x < kW / 2) ? -0.04 : 0.04,
							(y < kH / 2) ? 0.03 : -0.03
						};
						double rgb[3] = {};
						IctcpToBt2020PqRgb(cIn, rgb);
						for (int ch = 0; ch < 3; ++ch)
							Assert::IsTrue(rgb[ch] > 0.05 && rgb[ch] < 0.95, L"RGB test pixel must stay strictly in-gamut");
						const size_t idx = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
						rgbIn[idx + 0] = static_cast<float>(rgb[0]);
						rgbIn[idx + 1] = static_cast<float>(rgb[1]);
						rgbIn[idx + 2] = static_cast<float>(rgb[2]);
						rgbIn[idx + 3] = 1.0f;
					}
				}

				BindPreDebandUniforms(debandHook.hook, true, kThrNorm, kRadius, true, false);
				const std::vector<float> rgbOut = RenderRgba32fWithHook(
					fixture, kW, kH, rgbIn, debandHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
				AssertAllRgbFinite(rgbOut, L"RGB mode active deband");

				double bandedMaeRgb = 0.0;
				double debandedMaeRgb = 0.0;
				double maxDiffIRgb = 0.0;
				double maxDiffCtRgb = 0.0;
				double maxDiffCpRgb = 0.0;
				int rgbInteriorCount = 0;
				for (int y = 0; y < kH; ++y)
				{
					for (int x = 0; x < kW; ++x)
					{
						const size_t idx = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
						const double inPx[3] = { rgbIn[idx + 0], rgbIn[idx + 1], rgbIn[idx + 2] };
						const double outPx[3] = { rgbOut[idx + 0], rgbOut[idx + 1], rgbOut[idx + 2] };
						double inIctcp[3] = {};
						double outIctcp[3] = {};
						Bt2020PqRgbToIctcp(inPx, inIctcp);
						Bt2020PqRgbToIctcp(outPx, outIctcp);

						maxDiffCtRgb = (std::max)(maxDiffCtRgb, std::fabs(outIctcp[1] - inIctcp[1]));
						maxDiffCpRgb = (std::max)(maxDiffCpRgb, std::fabs(outIctcp[2] - inIctcp[2]));

						if (x >= kMargin && x < kW - kMargin && y >= kMargin && y < kH - kMargin)
						{
							const double gt = smoothI(x);
							bandedMaeRgb += std::fabs(inIctcp[0] - gt);
							debandedMaeRgb += std::fabs(outIctcp[0] - gt);
							maxDiffIRgb = (std::max)(maxDiffIRgb, std::fabs(outIctcp[0] - inIctcp[0]));
							++rgbInteriorCount;
						}
					}
				}
				bandedMaeRgb /= static_cast<double>(rgbInteriorCount);
				debandedMaeRgb /= static_cast<double>(rgbInteriorCount);

				Assert::IsTrue(debandedMaeRgb < bandedMaeRgb * 0.85,
					(L"RGB mode deband must reduce ICtCp I banding MAE below 0.85x (in=" +
						std::to_wstring(bandedMaeRgb * kCode12Max) + L", out=" +
						std::to_wstring(debandedMaeRgb * kCode12Max) + L" codes)").c_str());
				Assert::IsTrue(maxDiffIRgb > 2.0 / kCode12Max,
					L"RGB mode deband must actively smooth I > 2 codes");
				Assert::IsTrue(maxDiffCtRgb < 1e-5,
					(L"RGB mode drifted Ct beyond 1e-5: " + std::to_wstring(maxDiffCtRgb)).c_str());
				Assert::IsTrue(maxDiffCpRgb < 1e-5,
					(L"RGB mode drifted Cp beyond 1e-5: " + std::to_wstring(maxDiffCpRgb)).c_str());

				// Negative control on the exact same RGB frame: naive RGB-channel blur across the chroma step
				// boundary at x = kW / 2 corrupts Ct by > 0.01 (> 1000x the 1e-5 bound).
				const size_t leftIdx = (static_cast<size_t>(kH / 4) * kW + static_cast<size_t>(kW / 2 - 4)) * 4u;
				const size_t rightIdx = (static_cast<size_t>(kH / 4) * kW + static_cast<size_t>(kW / 2 + 4)) * 4u;
				const double naiveAvgRgb[3] = {
					0.5 * (static_cast<double>(rgbIn[leftIdx + 0]) + static_cast<double>(rgbIn[rightIdx + 0])),
					0.5 * (static_cast<double>(rgbIn[leftIdx + 1]) + static_cast<double>(rgbIn[rightIdx + 1])),
					0.5 * (static_cast<double>(rgbIn[leftIdx + 2]) + static_cast<double>(rgbIn[rightIdx + 2]))
				};
				const double origLeftRgb[3] = { rgbIn[leftIdx + 0], rgbIn[leftIdx + 1], rgbIn[leftIdx + 2] };
				double naiveIctcp[3] = {}, origIctcp[3] = {};
				Bt2020PqRgbToIctcp(naiveAvgRgb, naiveIctcp);
				Bt2020PqRgbToIctcp(origLeftRgb, origIctcp);
				Assert::IsTrue(std::fabs(naiveIctcp[1] - origIctcp[1]) > 0.01,
					L"Negative control: naive RGB blur across chroma edge must distort Ct > 0.01");

				// Active = 0.0 negative control on the exact same RGB frame.
				BindPreDebandUniforms(debandHook.hook, false, kThrNorm, kRadius, true, false);
				const std::vector<float> rgbOff = RenderRgba32fWithHook(
					fixture, kW, kH, rgbIn, debandHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
				float maxRgbOffDiff = 0.0f;
				for (size_t i = 0; i + 3 < rgbIn.size(); i += 4)
					for (int c = 0; c < 3; ++c)
						maxRgbOffDiff = (std::max)(maxRgbOffDiff,
							std::fabs(rgbOff[i + static_cast<size_t>(c)] - rgbIn[i + static_cast<size_t>(c)]));
				Assert::AreEqual(0.0f, maxRgbOffDiff, 0.0f, L"Negative control: RGB mode active=0 must be byte-identical");
			}

			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog, "PreDebandModifiesIntensityOnlyAndLeavesCtCpUntouched");
		}

		// CF-DV P5 GPU invariant: grain guard (dm_deband_grain_guard = 1.0) preserves fine deterministic
		// film grain (amplitude = 10/4095, within threshold = 36/4095) at >= 0.90 RMS retention, while
		// the unprotected baseline (dm_deband_grain_guard = 0.0) on the exact same texture smears grain
		// below 0.75 (strictly lower by >= 0.15).
		BEGIN_TEST_METHOD_ATTRIBUTE(PreDebandGrainGuardPreservesGrainAgainstUnprotectedNegativeControl)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PreDebandGrainGuardPreservesGrainAgainstUnprotectedNegativeControl)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create libplacebo WARP GPU fixture");
			std::string cleanLog;
			ScopedLogCapture logGuard(fixture, cleanLog);

			ScopedHook debandHook;
			LoadBundledHook(fixture, "vp_dm_pre_deband.glsl", PL_HOOK_RGB, debandHook);

			constexpr int kW = 128;
			constexpr int kH = 128;
			constexpr int kMargin = 16;
			constexpr double kBaseI = 0.40;
			constexpr double kGrainAmp = 10.0 / kCode12Max;
			constexpr double kThrNorm = 36.0 / kCode12Max;
			constexpr double kRadius = 12.0;

			// Deterministic zero-mean high-frequency grain bounded in [-10/4095, +10/4095].
			// Every pairwise neighbor delta is <= 20/4095 < thr = 36/4095, so an unprotected debander
			// smears it heavily while the grain guard recognizes local extrema / Laplacian energy.
			std::mt19937 rng(20260512u);
			std::uniform_real_distribution<double> dist(-1.0, 1.0);
			std::vector<double> grain(static_cast<size_t>(kW) * kH, 0.0);
			double mean = 0.0;
			for (double& g : grain)
			{
				g = dist(rng) * kGrainAmp;
				mean += g;
			}
			mean /= static_cast<double>(grain.size());
			for (double& g : grain)
				g -= mean;

			std::vector<float> noisyIctcp(static_cast<size_t>(kW) * kH * 4u, 1.0f);
			for (int y = 0; y < kH; ++y)
			{
				for (int x = 0; x < kW; ++x)
				{
					const size_t p = static_cast<size_t>(y) * kW + static_cast<size_t>(x);
					noisyIctcp[p * 4u + 0] = static_cast<float>(kBaseI + grain[p]);
					noisyIctcp[p * 4u + 1] = 0.06f;
					noisyIctcp[p * 4u + 2] = -0.05f;
					noisyIctcp[p * 4u + 3] = 1.0f;
				}
			}

			auto interiorGrainRms = [&](const std::vector<float>& rgba) -> double
			{
				double sumSq = 0.0;
				int count = 0;
				for (int y = kMargin; y < kH - kMargin; ++y)
				{
					for (int x = kMargin; x < kW - kMargin; ++x)
					{
						const size_t idx = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
						const double residual = static_cast<double>(rgba[idx + 0]) - kBaseI;
						sumSq += residual * residual;
						++count;
					}
				}
				return std::sqrt(sumSq / static_cast<double>(count));
			};

			const double inputRms = interiorGrainRms(noisyIctcp);
			Assert::IsTrue(inputRms > 4.0 / kCode12Max, L"Test setup: input grain RMS must be measurable");

			// Guarded run: dm_deband_grain_guard = 1.0.
			BindPreDebandUniforms(debandHook.hook, true, kThrNorm, kRadius, true, true);
			const std::vector<float> guardedOut = RenderRgba32fWithHook(
				fixture, kW, kH, noisyIctcp, debandHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
			AssertAllRgbFinite(guardedOut, L"Grain guard enabled");
			const double guardedRetention = interiorGrainRms(guardedOut) / inputRms;

			// Mandatory in-test negative control (exact same noisy texture): dm_deband_grain_guard = 0.0.
			BindPreDebandUniforms(debandHook.hook, true, kThrNorm, kRadius, false, true);
			const std::vector<float> unprotectedOut = RenderRgba32fWithHook(
				fixture, kW, kH, noisyIctcp, debandHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
			AssertAllRgbFinite(unprotectedOut, L"Grain guard disabled negative control");
			const double unprotectedRetention = interiorGrainRms(unprotectedOut) / inputRms;

			Assert::IsTrue(guardedRetention >= 0.90 && guardedRetention <= 1.05,
				(L"Grain guard must retain >= 0.90 of input grain RMS (guarded=" +
					std::to_wstring(guardedRetention) + L", unprotected=" +
					std::to_wstring(unprotectedRetention) + L")").c_str());
			Assert::IsTrue(unprotectedRetention < 0.75,
				(L"Negative control: unprotected debander must smear grain below 0.75 RMS retention (got " +
					std::to_wstring(unprotectedRetention) + L")").c_str());
			Assert::IsTrue(guardedRetention - unprotectedRetention >= 0.15,
				(L"Grain guard must beat unprotected baseline by >= 0.15 (guarded=" +
					std::to_wstring(guardedRetention) + L", unprotected=" +
					std::to_wstring(unprotectedRetention) + L")").c_str());

			// Chroma stays untouched in both passes.
			for (size_t i = 0; i + 3 < noisyIctcp.size(); i += 4)
			{
				Assert::AreEqual(noisyIctcp[i + 1], guardedOut[i + 1], 0.0f, L"Guarded pass must not touch Ct");
				Assert::AreEqual(noisyIctcp[i + 2], guardedOut[i + 2], 0.0f, L"Guarded pass must not touch Cp");
			}

			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog, "PreDebandGrainGuardPreservesGrainAgainstUnprotectedNegativeControl");
		}

		// CF-DV P5 GPU invariant: BTB (-0.05) and WTW (+1.08) regions pass through vp_dm_pre_deband.glsl
		// without clipping in both ICtCp and RGB modes. Negative control: [0, 1] clamping yields 0.05 BTB
		// error and 0.08 WTW error (> 100x pass-through tolerance).
		BEGIN_TEST_METHOD_ATTRIBUTE(PreDebandPreservesBtbAndWtwWithoutClipping)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PreDebandPreservesBtbAndWtwWithoutClipping)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create libplacebo WARP GPU fixture");
			std::string cleanLog;
			ScopedLogCapture logGuard(fixture, cleanLog);

			ScopedHook debandHook;
			LoadBundledHook(fixture, "vp_dm_pre_deband.glsl", PL_HOOK_RGB, debandHook);

			constexpr int kW = 64;
			constexpr int kH = 64;
			constexpr float kBtb = -0.05f;
			constexpr float kMid = 0.45f;
			constexpr float kWtw = 1.08f;
			constexpr float kPassThroughTol = 4e-4f;
			constexpr double kThrNorm = 32.0 / kCode12Max;
			constexpr double kRadius = 8.0;

			auto runProbe = [&](bool ictcpMode, const wchar_t* label)
			{
				std::vector<float> probe(static_cast<size_t>(kW) * kH * 4u, 1.0f);
				for (int y = 0; y < kH; ++y)
				{
					for (int x = 0; x < kW; ++x)
					{
						const size_t idx = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
						if (x < 21)
						{
							probe[idx + 0] = kBtb;
							probe[idx + 1] = ictcpMode ? 0.0f : kBtb;
							probe[idx + 2] = ictcpMode ? 0.0f : kBtb;
						}
						else if (x < 43)
						{
							probe[idx + 0] = kMid;
							probe[idx + 1] = ictcpMode ? 0.02f : kMid;
							probe[idx + 2] = ictcpMode ? -0.02f : kMid;
						}
						else
						{
							probe[idx + 0] = kWtw;
							probe[idx + 1] = ictcpMode ? 0.0f : kWtw;
							probe[idx + 2] = ictcpMode ? 0.0f : kWtw;
						}
						probe[idx + 3] = 1.0f;
					}
				}

				BindPreDebandUniforms(debandHook.hook, true, kThrNorm, kRadius, true, ictcpMode);
				const std::vector<float> out = RenderRgba32fWithHook(
					fixture, kW, kH, probe, debandHook.hook, PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
				AssertAllRgbFinite(out, label);

				float maxBtbErr = 0.0f;
				float maxWtwErr = 0.0f;
				float maxMidErr = 0.0f;
				const int channelsToCheck = ictcpMode ? 1 : 3;

				for (int y = 4; y < kH - 4; ++y)
				{
					for (int x = 2; x < 19; ++x)
					{
						const size_t idx = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
						for (int c = 0; c < channelsToCheck; ++c)
						{
							const float v = out[idx + static_cast<size_t>(c)];
							Assert::IsTrue(v < -0.04f,
								(std::wstring(label) + L": BTB was clipped (got " + std::to_wstring(v) + L")").c_str());
							maxBtbErr = (std::max)(maxBtbErr, std::fabs(v - kBtb));
						}
					}
					for (int x = 23; x < 41; ++x)
					{
						const size_t idx = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
						for (int c = 0; c < channelsToCheck; ++c)
							maxMidErr = (std::max)(maxMidErr, std::fabs(out[idx + static_cast<size_t>(c)] - kMid));
					}
					for (int x = 45; x < 62; ++x)
					{
						const size_t idx = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
						for (int c = 0; c < channelsToCheck; ++c)
						{
							const float v = out[idx + static_cast<size_t>(c)];
							Assert::IsTrue(v > 1.07f,
								(std::wstring(label) + L": WTW was clipped (got " + std::to_wstring(v) + L")").c_str());
							maxWtwErr = (std::max)(maxWtwErr, std::fabs(v - kWtw));
						}
					}
				}

				Assert::IsTrue(maxBtbErr <= kPassThroughTol,
					(std::wstring(label) + L": BTB drifted beyond tolerance: " + std::to_wstring(maxBtbErr)).c_str());
				Assert::IsTrue(maxWtwErr <= kPassThroughTol,
					(std::wstring(label) + L": WTW drifted beyond tolerance: " + std::to_wstring(maxWtwErr)).c_str());
				Assert::IsTrue(maxMidErr <= kPassThroughTol,
					(std::wstring(label) + L": Mid-tone drifted beyond tolerance: " + std::to_wstring(maxMidErr)).c_str());

				// Mandatory in-test negative control: clamping the exact same BTB (-0.05) and WTW (+1.08)
				// values to [0.0, 1.0] produces 0.05 BTB error and 0.08 WTW error (> 100x pass-through tolerance).
				const float clampedBtb = (std::min)(1.0f, (std::max)(0.0f, kBtb));
				const float clampedWtw = (std::min)(1.0f, (std::max)(0.0f, kWtw));
				const float btbClipErr = std::fabs(clampedBtb - kBtb);
				const float wtwClipErr = std::fabs(clampedWtw - kWtw);
				Assert::AreEqual(0.05f, btbClipErr, 1e-6f);
				Assert::AreEqual(0.08f, wtwClipErr, 1e-6f);
				Assert::IsTrue(btbClipErr > 100.0f * kPassThroughTol && btbClipErr > 100.0f * maxBtbErr,
					L"Negative control: [0,1] clamp BTB error must exceed 100x pass-through tolerance");
				Assert::IsTrue(wtwClipErr > 100.0f * kPassThroughTol && wtwClipErr > 100.0f * maxWtwErr,
					L"Negative control: [0,1] clamp WTW error must exceed 100x pass-through tolerance");
			};

			runProbe(true, L"ICtCp BTB/WTW probe");
			runProbe(false, L"RGB BTB/WTW probe");

			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog, "PreDebandPreservesBtbAndWtwWithoutClipping");
		}
	};
}
