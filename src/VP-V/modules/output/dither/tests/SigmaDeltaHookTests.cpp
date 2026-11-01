// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <functional>
#include <VP-V/modules/output/dither/DitherNeed.h>
#include <VP-V/modules/output/dither/SigmaDeltaDomain.h>
#include <VP-V/modules/output/dither/DitherNeedGlsl.h>
#include <VP-V/modules/output/dither/DitherNeedGpu.h>
#include "DitherNeedGolden.generated.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	TEST_CLASS(SigmaDeltaHookTests)
	{
	public:


		// Delta 8 R1: the extras' sigma-delta takes VP's vectors only with
		// dither_vectors auto, the fork patch, a field and the same source frame.
		TEST_METHOD(DitherVectorsAutoFallsBackWithoutThePatch)
		{
			Assert::IsTrue(DejudderRendered::UseExternalVectors(false, true, true, true));
			Assert::IsFalse(DejudderRendered::UseExternalVectors(true, true, true, true), L"own");
			Assert::IsFalse(DejudderRendered::UseExternalVectors(false, false, true, true), L"7.360: no patch 0004");
			Assert::IsFalse(DejudderRendered::UseExternalVectors(false, true, false, true), L"no matcher field");
			Assert::IsFalse(DejudderRendered::UseExternalVectors(false, true, true, false), L"another source frame");
			Assert::AreEqual(16, DejudderRendered::VectorField::Blocks(256));
			Assert::AreEqual(68, DejudderRendered::VectorField::Blocks(1080));
		}

		TEST_METHOD(PreOutputHooksKeepTheOutputEncodingWithTheRepair)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			LedDrive::MeasurementHook measurement;
			const double weights[3] = { 0.299, 0.587, 0.114 };
			measurement.SetFrame({}, weights);
			OutputEncodingRepair::Hook repair;
			std::string allLog;   // every render's libplacebo log (clean apart from the known message)
			for (const enum pl_color_levels levels : { PL_COLOR_LEVELS_LIMITED, PL_COLOR_LEVELS_FULL })
			{
				pl_color_repr targetRepr{};
				targetRepr.sys = PL_COLOR_SYSTEM_RGB;
				targetRepr.levels = levels;
				repair.SetTarget(targetRepr);
				for (const RgbaPixel source : { RgbaPixel{ 255, 0, 0, 255 }, RgbaPixel{ 128, 64, 200, 255 } })
				{
					std::string& log = allLog;
					const RgbaPixel plain = RenderWithHooks(fixture, {}, levels, source, log);
					std::vector<const pl_hook*> hooks = { measurement.Hook() };
					Assert::IsTrue(OutputEncodingRepair::Needed(hooks));
					hooks.insert(hooks.begin(), repair.Get());
					const RgbaPixel repaired = RenderWithHooks(fixture, hooks, levels, source, log);
					const std::wstring label = (levels == PL_COLOR_LEVELS_LIMITED ? L"limited" : L"full") +
						std::wstring(L": plain ") + std::to_wstring(plain.r) + L"/" + std::to_wstring(plain.g) +
						L"/" + std::to_wstring(plain.b) + L", with the repair " + std::to_wstring(repaired.r) + L"/" +
						std::to_wstring(repaired.g) + L"/" + std::to_wstring(repaired.b);
					Assert::IsTrue(std::abs(plain.r - repaired.r) <= 1 && std::abs(plain.g - repaired.g) <= 1 &&
						std::abs(plain.b - repaired.b) <= 1, label.c_str());
				}
			}
			Assert::IsTrue(repair.Applied() > 0, L"The repair hook never ran");
			AssertCleanLibplaceboLogExceptPreOutputBug(allLog);

			// Without the repair libplacebo 7.360.1 still drops the encoding. If
			// this ever fails, libplacebo has fixed the bug: remove the repair,
			// or it will encode twice.
			pl_color_repr limited{};
			limited.sys = PL_COLOR_SYSTEM_RGB;
			limited.levels = PL_COLOR_LEVELS_LIMITED;
			std::string log;
			const RgbaPixel plain = RenderWithHooks(fixture, {}, PL_COLOR_LEVELS_LIMITED, { 255, 0, 0, 255 }, log);
			const RgbaPixel broken = RenderWithHooks(fixture, { measurement.Hook() }, PL_COLOR_LEVELS_LIMITED,
				{ 255, 0, 0, 255 }, log);
			Assert::AreEqual(235, static_cast<int>(plain.r));
			Assert::AreEqual(255, static_cast<int>(broken.r),
				L"libplacebo no longer drops the encoding after a PRE_OUTPUT hook: remove OutputEncodingRepair");
			measurement.Release();

			// The LED compensation shader (an mpv PRE_OUTPUT hook, active whenever
			// the gamma boost is on, LED drive or not) with neutral settings:
			// limited-range output must come out as without it.
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
			rule.name = "led_compensation";
			rule.filename = "LED compensation.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 64;
			chain.Ensure(values);
			std::vector<const pl_hook*> compensation;
			chain.Collect(false, compensation);
			Assert::AreEqual(size_t{ 1 }, compensation.size(), L"libplacebo rejected LED compensation.glsl");
			for (const char* name : { "gain", "boost" })
				Assert::IsTrue(CustomHookChain::SetHookParameter(compensation[0], name, 1.0));
			Assert::IsTrue(CustomHookChain::SetHookParameter(compensation[0], "gain_domain", 0.0));
			Assert::IsTrue(CustomHookChain::SetHookParameter(compensation[0], "knee", 0.08));
			Assert::IsTrue(CustomHookChain::SetHookParameter(compensation[0], "compensation_eotf", 2.2));
			repair.SetTarget(limited);
			for (const RgbaPixel source : { RgbaPixel{ 255, 0, 0, 255 }, RgbaPixel{ 128, 64, 200, 255 } })
			{
				const RgbaPixel reference = RenderWithHooks(fixture, {}, PL_COLOR_LEVELS_LIMITED, source, log);
				const RgbaPixel repaired = RenderWithHooks(fixture, { repair.Get(), compensation[0] },
					PL_COLOR_LEVELS_LIMITED, source, log);
				Assert::IsTrue(std::abs(reference.r - repaired.r) <= 1 && std::abs(reference.g - repaired.g) <= 1 &&
					std::abs(reference.b - repaired.b) <= 1,
					(L"LED compensation, limited: plain " + std::to_wstring(reference.r) + L"/" +
						std::to_wstring(reference.g) + L"/" + std::to_wstring(reference.b) + L", with the repair " +
						std::to_wstring(repaired.r) + L"/" + std::to_wstring(repaired.g) + L"/" +
						std::to_wstring(repaired.b)).c_str());
			}

			// Domain contract (planner, 2026-10-10): the sigma-delta assumes the
			// OUTPUT stage is already encoded (limited = code/256). With the LED
			// hook, the repair first and the quantizer enabled, white must reach
			// the target as 235 (limited) or 255 (full), black as 16 or 0.
			for (const enum pl_color_levels levels : { PL_COLOR_LEVELS_LIMITED, PL_COLOR_LEVELS_FULL })
			{
				const bool isLimited = levels == PL_COLOR_LEVELS_LIMITED;
				CustomHookChain::Chain sigmaChain({
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
				ConfiguredShaderRule sigma;
				sigma.name = "sigma_delta";
				sigma.filename = "Temporal sigma-delta.glsl";
				sigma.parameters = { { "enabled", "1" }, { "levels_full", isLimited ? "0" : "1" },
					{ "swapchain_bits", "8" }, { "noise_amplitude", "0" } };
				sigmaChain.Configure({ sigma });
				CustomHookChain::TemplateValues sigmaValues;
				sigmaValues.outputWidth = sigmaValues.outputHeight = 2;
				sigmaChain.Ensure(sigmaValues);
				std::vector<const pl_hook*> sigmaHooks;
				sigmaChain.Collect(false, sigmaHooks);
				Assert::AreEqual(size_t{ 1 }, sigmaHooks.size(), L"libplacebo rejected the sigma-delta");
				pl_color_repr repr{};
				repr.sys = PL_COLOR_SYSTEM_RGB;
				repr.levels = levels;
				repair.SetTarget(repr);
				LedDrive::MeasurementHook led;
				led.SetFrame({}, weights);
				std::vector<const pl_hook*> chainHooks = { led.Hook(), sigmaHooks[0] };
				Assert::IsTrue(OutputEncodingRepair::Needed(chainHooks));
				chainHooks.insert(chainHooks.begin(), repair.Get());   // as the renderer does
				const RgbaPixel white = RenderWithHooks(fixture, chainHooks, levels, { 255, 255, 255, 255 }, log);
				const RgbaPixel black = RenderWithHooks(fixture, chainHooks, levels, { 0, 0, 0, 255 }, log);
				led.Release();
				Assert::AreEqual(isLimited ? 235 : 255, static_cast<int>(white.g),
					isLimited ? L"Limited white with LED hook, repair and sigma-delta" :
						L"Full white with LED hook, repair and sigma-delta");
				Assert::AreEqual(isLimited ? 16 : 0, static_cast<int>(black.g),
					isLimited ? L"Limited black with LED hook, repair and sigma-delta" :
						L"Full black with LED hook, repair and sigma-delta");
			}

			// No PRE_OUTPUT texture hook, no repair (an OUTPUT-stage hook alone
			// does not trigger the bug).
			Assert::IsFalse(OutputEncodingRepair::Needed({ repair.Get() }));
			// A hook libplacebo has disabled is skipped before the shader is
			// finished, so it does not trigger the bug: no repair (it would
			// encode twice). Nor with a CONVERSION target LUT.
			LedDrive::MeasurementHook disabledHook;
			const uint64_t signatures[1] = { disabledHook.Hook()->signature };
			pl_render_errors errors{};
			errors.errors = PL_RENDER_ERR_HOOKS;
			errors.disabled_hooks = signatures;
			errors.num_disabled_hooks = 1;
			Assert::IsTrue(OutputEncodingRepair::Needed({ disabledHook.Hook() }));
			Assert::IsFalse(OutputEncodingRepair::Needed({ disabledHook.Hook() }, &errors),
				L"A disabled PRE_OUTPUT hook must not bring the repair");
			Assert::IsTrue(OutputEncodingRepair::Needed({ disabledHook.Hook(), compensation[0] }, &errors),
				L"Another, active PRE_OUTPUT hook still does");
			Assert::IsFalse(OutputEncodingRepair::Needed({ compensation[0] }, nullptr, PL_LUT_CONVERSION));
		}

		// The cinema path (stage 6a re-audit): limited range on a 10-bit
		// swapchain with colour depth 8. libplacebo hands the OUTPUT stage code
		// k as 4k/1023 (= k/255.75), and the sigma-delta must read it so: with
		// the LED hook, the repair and the quantizer, white stays 940 (235) and
		// black 64 (16) on every refresh, and a 10-bit mid grey averages exactly.
		// Delta 5 (D5-2): with a true 10-bit output (colour depth 10) the
		// sigma-delta quantises to 10-bit codes, in both ranges: white and
		// black exact on every refresh, and a grey whose 10-bit code is not a
		// multiple of 4 held exactly (an 8-bit quantiser would alternate the
		// neighbouring multiples of 4).
		TEST_METHOD(SigmaDeltaKeepsCodesExactOnATrueTenBitOutput)
		{
			for (const bool full : { false, true })
			{
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
				std::string log;
				fixture.CaptureLog(&log);
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
				ConfiguredShaderRule sigma;
				sigma.name = "sigma_delta";
				sigma.filename = "Temporal sigma-delta.glsl";
				sigma.parameters = { { "enabled", "1" }, { "levels_full", full ? "1" : "0" },
					{ "swapchain_bits", "10" }, { "color_bits", "10" }, { "noise_amplitude", "0" } };
				chain.Configure({ sigma });
				CustomHookChain::TemplateValues values;
				values.outputWidth = values.outputHeight = 2;
				chain.Ensure(values);
				std::vector<const pl_hook*> hooks;
				chain.Collect(false, hooks);
				Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected the sigma-delta");
				const pl_color_levels levels = full ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
				auto describe = [](const std::vector<int>& codes)
				{
					std::wstring text;
					for (int code : codes) text += std::to_wstring(code) + L" ";
					return text;
				};
				const int whiteCode = full ? 1023 : 940, blackCode = full ? 0 : 64;
				const int libplaceboWhite = fixture.RenderTenBitSequence({}, levels, 1.0f, 1, nullptr, 10)[0];
				const int libplaceboBlack = fixture.RenderTenBitSequence({}, levels, 0.0f, 1, nullptr, 10)[0];
				for (int code : fixture.RenderTenBitSequence(hooks, levels, 1.0f, 8, nullptr, 10))
					Assert::AreEqual(whiteCode, code, (L"White on a true 10-bit output (libplacebo alone: " +
						std::to_wstring(libplaceboWhite) + L", black " + std::to_wstring(libplaceboBlack) + L")").c_str());
				for (int code : fixture.RenderTenBitSequence(hooks, levels, 0.0f, 8, nullptr, 10))
					Assert::AreEqual(blackCode, code, L"Black on a true 10-bit output");
				// A grey with a 10-bit code that is not a multiple of 4.
				float grey = 0.0f;
				int reference = 0;
				for (int target = 501; target <= 507 && grey == 0.0f; ++target)
				{
					const float candidate = full ? target / 1023.0f : (target - 64) / 876.0f;
					const int code = fixture.RenderTenBitSequence({}, levels, candidate, 1, nullptr, 10)[0];
					if (code % 4 != 0)
					{
						grey = candidate;
						reference = code;
					}
				}
				Assert::IsTrue(grey > 0.0f, L"no reference grey off the 8-bit grid");
				const std::vector<int> codes = fixture.RenderTenBitSequence(hooks, levels, grey, 8, nullptr, 10);
				for (int code : codes)
					Assert::AreEqual(reference, code,
						(L"10-bit grey " + std::to_wstring(reference) + L": " + describe(codes)).c_str());
				fixture.CaptureLog(nullptr);
				AssertCleanLibplaceboLog(log);
			}
		}

		TEST_METHOD(SigmaDeltaKeepsCodesExactOnTheTenBitSwapchain)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
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
			ConfiguredShaderRule sigma;
			sigma.name = "sigma_delta";
			sigma.filename = "Temporal sigma-delta.glsl";
			sigma.parameters = { { "enabled", "1" }, { "levels_full", "0" }, { "swapchain_bits", "10" },
				{ "noise_amplitude", "0" } };
			chain.Configure({ sigma });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 2;
			chain.Ensure(values);
			std::vector<const pl_hook*> sigmaHooks;
			chain.Collect(false, sigmaHooks);
			Assert::AreEqual(size_t{ 1 }, sigmaHooks.size(), L"libplacebo rejected the sigma-delta");
			LedDrive::MeasurementHook led;
			const double weights[3] = { 0.299, 0.587, 0.114 };
			led.SetFrame({}, weights);
			OutputEncodingRepair::Hook repair;
			pl_color_repr limited{};
			limited.sys = PL_COLOR_SYSTEM_RGB;
			limited.levels = PL_COLOR_LEVELS_LIMITED;
			limited.bits.sample_depth = 10;
			limited.bits.color_depth = 8;
			limited.bits.bit_shift = 2;
			repair.SetTarget(limited);
			const std::vector<const pl_hook*> hooks = { repair.Get(), led.Hook(), sigmaHooks[0] };
			auto describe = [](const std::vector<int>& codes)
			{
				std::wstring text;
				for (int code : codes) text += std::to_wstring(code) + L" ";
				return text;
			};
			const std::vector<int> white = fixture.RenderTenBitSequence(hooks, PL_COLOR_LEVELS_LIMITED, 1.0f, 8);
			for (int code : white)
				Assert::AreEqual(940, code, (L"White on the 10-bit swapchain: " + describe(white)).c_str());
			const std::vector<int> black = fixture.RenderTenBitSequence(hooks, PL_COLOR_LEVELS_LIMITED, 0.0f, 8);
			for (int code : black)
				Assert::AreEqual(64, code, (L"Black on the 10-bit swapchain: " + describe(black)).c_str());
			// The same grey with no hook is the reference (libplacebo's own
			// 10-bit result); the sigma-delta's 8-bit codes must average it.
			const std::vector<int> reference = fixture.RenderTenBitSequence({}, PL_COLOR_LEVELS_LIMITED, 0.5f, 1);
			const std::vector<int> grey = fixture.RenderTenBitSequence(hooks, PL_COLOR_LEVELS_LIMITED, 0.5f, 8);
			double mean = 0.0;
			for (int code : grey) mean += code / 8.0;
			Assert::AreEqual(static_cast<double>(reference[0]), mean, 0.6,
				(L"Mid grey: reference " + std::to_wstring(reference[0]) + L", sigma-delta " + describe(grey)).c_str());
			led.Release();
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		TEST_METHOD(SigmaDeltaCarriesTheResidualAlongTheMotion)
		{
			const SigmaDeltaRun gate = RunSigmaDelta(false, 4, 9);
			const SigmaDeltaRun carried = RunSigmaDelta(true, 4, 9);
			Assert::IsTrue(gate.meanError > 0.2,
				(L"With the plain gate a moving picture should round (error " +
					std::to_wstring(gate.meanError) + L")").c_str());
			Assert::IsTrue(carried.meanError < 0.05,
				(L"With motion compensation the time-average along the motion should converge (error " +
					std::to_wstring(carried.meanError) + L")").c_str());

			// Speeds that are not a multiple of 4 px (the quarter-resolution
			// step) need the full-pixel refinement (stage 4 audit).
			for (int speed : { 5, 10 })
			{
				const SigmaDeltaRun odd = RunSigmaDelta(true, speed, 9);
				Assert::IsTrue(odd.meanError < 0.05,
					(std::to_wstring(speed) + L" px/refresh error " + std::to_wstring(odd.meanError)).c_str());
			}
			// Diagonal motion.
			const SigmaDeltaRun diagonal = RunSigmaDelta(true, 6, 9, false, 3);
			Assert::IsTrue(diagonal.meanError < 0.05,
				(L"Diagonal (6, 3) px/refresh error " + std::to_wstring(diagonal.meanError)).c_str());
			// With the default decorrelation noise (0.15) the carry still
			// converges; without it a moving picture keeps its rounding bias.
			const SigmaDeltaRun noisy = RunSigmaDelta(true, 5, 9, false, 0, 0.15);
			const SigmaDeltaRun noisyGate = RunSigmaDelta(false, 5, 9, false, 0, 0.15);
			Assert::IsTrue(noisy.meanError < 0.1 && noisyGate.meanError > 0.2,
				(L"With noise 0.15: carried " + std::to_wstring(noisy.meanError) +
					L", plain gate " + std::to_wstring(noisyGate.meanError)).c_str());

			// A still picture is unchanged by motion compensation (the plain
			// gate still carries the residual in place).
			// Faster motion (20 pixels per refresh) is beyond the +-8 pixel
			// fine search around zero.
			const SigmaDeltaRun fast = RunSigmaDelta(true, 20, 9);
			Assert::IsTrue(fast.meanError < 0.05,
				(L"Fast motion error " + std::to_wstring(fast.meanError)).c_str());
			// A fast pan (48 pixels per refresh) needs the coarse level of the
			// hierarchical search.
			const SigmaDeltaRun faster = RunSigmaDelta(true, 48, 9);
			Assert::IsTrue(faster.meanError < 0.05,
				(L"48 px/refresh error " + std::to_wstring(faster.meanError)).c_str());
			const SigmaDeltaRun still = RunSigmaDelta(true, 0, 9);
			Assert::IsTrue(still.meanError < 0.05,
				(L"Still picture error " + std::to_wstring(still.meanError)).c_str());
		}

		TEST_METHOD(SigmaDeltaGrainAwareCarry)
		{
			// S1: a moving texture with integer film grain (+-2 codes, new every
			// frame). With match_tolerance 1 the grain fails the block match and
			// the per-pixel gate, so the quarter code is rounded away (the
			// time-average along the motion is 0.25 low); with the grain map in
			// alpha (g = 1) and grain_tolerance_scale 4 the residual survives and
			// the average holds. Scale 1, or a map with no grain, changes nothing.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int speed = 4, frames = 9, width = 128 + speed * 8, height = 64;
			ConfiguredShaderRule sigma;
			sigma.name = "sigma_delta";
			sigma.filename = "Temporal sigma-delta.glsl";
			sigma.stageOrder = 90;
			sigma.parameters = { { "enabled", "1" }, { "levels_full", "1" }, { "swapchain_bits", "8" },
				{ "noise_amplitude", "0" }, { "motion_threshold", "1" }, { "match_tolerance", "1" },
				{ "motion_compensation", "1" } };
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.sourceWidth = width;
			values.outputHeight = values.sourceHeight = height;
			values.refreshHz = values.sourceFps = 60.0;
			auto grain = [](int frame, int x, int y)
			{
				const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u) ^
					(static_cast<unsigned>(frame) * 83492791u);
				return static_cast<int>((h * 2654435761u >> 24) % 5u) - 2;
			};
			auto code = [&](int frame, int x, int y)
			{
				return SigmaDeltaTexture(x - speed * frame, y) + grain(frame, x, y);
			};
			// A grain map of one block: r = the floor (g = 1) or 1 (g = 0), trusted.
			auto makeMap = [&](float r)
			{
				const float texel[4] = { r, 1.0f, 0.0f, 1.0f };
				pl_tex_params mp{};
				mp.w = mp.h = 1;
				mp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
				mp.sampleable = true;
				mp.initial_data = texel;
				return mp.format ? pl_tex_create(gpu, &mp) : nullptr;
			};
			pl_tex grainy = makeMap(0.4f), clean = makeMap(1.0f);
			Assert::IsTrue(grainy && clean);
			OcclusionMask::Hook hook;
			const pl_rect2df whole{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
			// A fresh sigma-delta per run: its residual state lives in its textures.
			auto run = [&](pl_tex map, double scale, bool withHook)
			{
				CustomHookChain::Chain chain(SigmaLoader(fixture));
				chain.Configure({ sigma });
				chain.Ensure(values);
				std::vector<const pl_hook*> sigmaHooks;
				chain.Collect(false, sigmaHooks);
				Assert::AreEqual(size_t{ 1 }, sigmaHooks.size(), (L"libplacebo rejected the sigma-delta: " +
					std::wstring(log.begin(), log.end())).c_str());
				Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "grain_tolerance_scale", scale));
				hook.SetFrame(nullptr, whole, whole);
				hook.SetGrain(map, 0.4f);
				std::vector<const pl_hook*> hooks;
				if (withHook)
					hooks.push_back(hook.Get());
				hooks.push_back(sigmaHooks[0]);
				return fixture.RenderSequence(hooks, width, height, frames, code);
			};
			// The time-average along the motion over frames 1..8 against the
			// noise-free value plus the grain that was really there.
			auto meanError = [&](const std::vector<std::vector<int>>& out)
			{
				double error = 0.0;
				int count = 0;
				for (int y0 = 16; y0 < height - 16; ++y0)
					for (int x0 = 16; x0 < width - 16 - speed * 8; ++x0)
					{
						double sum = 0.0, expected = 0.0;
						for (int frame = 1; frame <= 8; ++frame)
						{
							const int x = x0 + speed * frame;
							sum += out[frame][y0 * width + x];
							expected += code(frame, x, y0);
						}
						error += std::abs((sum - expected) / 8.0);
						++count;
					}
				return error / count;
			};
			const auto today = run(nullptr, 1.0, false);
			const auto scaleOne = run(grainy, 1.0, true);
			const auto noGrain = run(clean, 4.0, true);
			const auto grainAware = run(grainy, 4.0, true);
			pl_tex_destroy(gpu, &grainy);
			pl_tex_destroy(gpu, &clean);
			const double todayError = meanError(today), awareError = meanError(grainAware);
			const std::wstring message = L"along-motion error: today " + std::to_wstring(todayError) +
				L", grain-aware " + std::to_wstring(awareError) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(scaleOne == today, (L"scale 1 must not change the output; " + message).c_str());
			Assert::IsTrue(noGrain == today, (L"a map without grain must not change the output; " + message).c_str());
			Assert::IsTrue(todayError > 0.15, (L"the grain should defeat today's carry; " + message).c_str());
			Assert::IsTrue(awareError < 0.08, (L"the grain-aware carry should hold the average; " + message).c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(SigmaDeltaDropsTheResidualAtASceneCut)
		{
			// After a cut to an unrelated picture no residual is carried: every
			// pixel of the first new frame is the plain rounded value.
			const SigmaDeltaRun run = RunSigmaDelta(true, 4, 6, true);
			const int width = 128 + 4 * 8; // as RunSigmaDelta sizes it for speed 4
			int carriedPixels = 0, total = 0;
			for (int y = 0; y < 64; ++y)
				for (int x = 0; x < width; ++x)
				{
					const double value = SigmaDeltaTexture(x * 3 + 1000, y * 5 + 777);
					if (run.frames[4][y * width + x] != static_cast<int>(std::floor(value + 0.5)))
						++carriedPixels;
					++total;
				}
			Assert::IsTrue(carriedPixels * 50 <= total,
				(L"Pixels carrying a residual across the cut: " + std::to_wstring(carriedPixels)).c_str());
		}

		TEST_METHOD(SigmaDeltaDomainFollowsTheTarget)
		{
			auto repr = [](enum pl_color_levels levels, int sample, int color)
			{
				pl_color_repr r{};
				r.sys = PL_COLOR_SYSTEM_RGB;
				r.levels = levels;
				r.bits.sample_depth = sample;
				r.bits.color_depth = color;
				return r;
			};
			const auto fl35 = SigmaDeltaDomain::FromTarget(repr(PL_COLOR_LEVELS_LIMITED, 10, 8));
			Assert::IsTrue(fl35.supported && !fl35.full && fl35.sample == 10 && fl35.color == 8);
			const auto ten = SigmaDeltaDomain::FromTarget(repr(PL_COLOR_LEVELS_FULL, 10, 10));
			Assert::IsTrue(ten.supported && ten.full && ten.color == 10);
			const auto eight = SigmaDeltaDomain::FromTarget(repr(PL_COLOR_LEVELS_UNKNOWN, 0, 0));
			Assert::IsTrue(eight.supported && eight.full && eight.sample == 8 && eight.color == 8);
			Assert::IsFalse(SigmaDeltaDomain::FromTarget(repr(PL_COLOR_LEVELS_FULL, 16, 16)).supported);
			Assert::IsFalse(SigmaDeltaDomain::FromTarget(repr(PL_COLOR_LEVELS_FULL, 16, 0)).supported);
		}

		// Hunt 2 L2: the quantiser's code limits are the codes libplacebo-beta's
		// output clamp keeps (fork patch 0002): reserved timing codes excluded,
		// footroom/headroom kept; derived from the depth, not listed per case.
		TEST_METHOD(SigmaDeltaCodeLimitsAreThePermissibleCodes)
		{
			const auto l8 = SigmaDeltaDomain::Limits(8, false);       // 8-bit, and 8-in-10 (stored as 4k)
			Assert::AreEqual(1.0, l8.min);
			Assert::AreEqual(254.0, l8.max);
			Assert::AreEqual(4.0, l8.min * 4.0);                      // 8-in-10: 4..1016
			Assert::AreEqual(1016.0, l8.max * 4.0);
			const auto l10 = SigmaDeltaDomain::Limits(10, false);
			Assert::AreEqual(4.0, l10.min);
			Assert::AreEqual(1019.0, l10.max);
			const auto l12 = SigmaDeltaDomain::Limits(12, false);
			Assert::AreEqual(16.0, l12.min);
			Assert::AreEqual(4079.0, l12.max);
			const auto f8 = SigmaDeltaDomain::Limits(8, true);
			Assert::AreEqual(0.0, f8.min);
			Assert::AreEqual(255.0, f8.max);
			const auto f10 = SigmaDeltaDomain::Limits(10, true);
			Assert::AreEqual(1023.0, f10.max);
		}

		// Hunt 2 L1: the residual bound includes the dither amplitude, so the
		// time average is unbiased at every fraction and every amplitude (the
		// old +-0.5 clamp gave up to ~0.06 code at amplitude 0.5).
		TEST_METHOD(SigmaDeltaMeanIsUnbiasedAtAnyDitherAmplitude)
		{
			const auto limits = SigmaDeltaDomain::Limits(8, false);
			const int n = 40000;
			double worst = 0.0;
			for (double amplitude : { 0.0, 0.15, 0.3, 0.5 })
				for (int i = 1; i < 40; ++i)
				{
					const double x = 100.0 + i * 0.025;
					for (float px : { 3.0f, 37.0f, 211.0f })
						worst = std::max(worst, std::fabs(SigmaDeltaMean(x, amplitude, limits, n, nullptr, nullptr, px) - x));
				}
			Logger::WriteMessage(("sigma-delta worst mean error: " + std::to_string(worst) + " code\n").c_str());
			// First-order loop with a bounded residual: |mean - x| <= bound / n.
			Assert::IsTrue(worst < 1e-3, L"the time average equals the input (no fraction-dependent bias)");

			// The old payload clamp (+-0.5) is what biased it: shown with the
			// same loop, so the test fails if the bound regresses.
			double oldWorst = 0.0;
			for (int i = 1; i < 40; ++i)
			{
				const double x = 100.0 + i * 0.025;
				double r = 0.0, sum = 0.0;
				for (int f = 0; f < n; ++f)
				{
					const double d = (SigmaDeltaDomain::Ign(37.0f, 11.0f, f) - 0.5) * 2.0 * 0.5;
					const double k = (std::min)((std::max)(std::floor(x + r + d + 0.5), limits.min), limits.max);
					r = (std::min)((std::max)(x + r - k, -0.5), 0.5);
					sum += k;
				}
				oldWorst = std::max(oldWorst, std::fabs(sum / n - x));
			}
			Assert::IsTrue(oldWorst > 0.01, L"the model reproduces the old clamp's bias (the test is meaningful)");
		}

		// Hunt 2 L2: near the outermost permissible codes the quantiser never
		// emits a reserved code, and the mean still lands on the input (the
		// feedback sees the code really emitted).
		TEST_METHOD(SigmaDeltaNeverEmitsReservedCodes)
		{
			const int n = 40000;
			struct Case { int bits; double x; };
			for (const Case c : { Case{ 8, 1.05 }, Case{ 8, 1.5 }, Case{ 8, 253.95 }, Case{ 8, 253.5 },
				Case{ 10, 4.2 }, Case{ 10, 1018.8 } })
				for (double amplitude : { 0.15, 0.5 })
				{
					const auto limits = SigmaDeltaDomain::Limits(c.bits, false);
					double lo = 0.0, hi = 0.0;
					const double mean = SigmaDeltaMean(c.x, amplitude, limits, n, &lo, &hi);
					Assert::IsTrue(lo >= limits.min && hi <= limits.max, L"no reserved timing code is emitted");
					Assert::IsTrue(std::fabs(mean - c.x) < 2e-3, L"the mean lands on the input next to the limits");
				}
			// Beyond the permissible range (an overshoot) the output pins to the
			// limit and the residual stays bounded (no wind-up): the moment the
			// input returns inside, the mean recovers.
			const auto limits = SigmaDeltaDomain::Limits(8, false);
			double r = 0.0;
			for (int f = 0; f < 1000; ++f)
				Assert::AreEqual(254.0, SigmaDeltaDomain::QuantiseStep(255.6, r, 0.0, limits, 0.15));
			Assert::IsTrue(r <= SigmaDeltaDomain::ResidualBound(0.15) + 1e-12, L"the residual is bounded at the limit");
			// Full range keeps every code, 0 and the top included.
			const auto full = SigmaDeltaDomain::Limits(8, true);
			double rf = 0.0;
			Assert::AreEqual(0.0, SigmaDeltaDomain::QuantiseStep(0.0, rf, 0.0, full, 0.15));
			rf = 0.0;
			Assert::AreEqual(255.0, SigmaDeltaDomain::QuantiseStep(255.0, rf, 0.0, full, 0.15));
		}

		// Gate verify 2026-10-17: the sigma-delta on the GPU is unbiased. Flat patches at every
		// 10-bit level and at off-grid levels (every 1/32 code around chosen codes, exactly
		// representable in the rgba16f OUTPUT texture) go through the shader for 192 static
		// refreshes in each output mode (8-in-10 limited = the FL35 link, 8-in-10 full, true 10-bit
		// limited and full), on 7.360's rgba16f output pass. Asserted for the sigma-delta alone:
		// (1) off-grid levels a quarter code or more (and more than one rgba16f step) from a code
		// (near 10-bit white the store holds only half codes): each patch's spatio-temporal mean
		// equals the value the hook receives within the first-order loop's deterministic bound
		// (2 x 0.65 / 192 code) plus float slack, 0.02 code; (2) every level: the mean is within
		// one rgba16f step (x1.05) of the level - the exact-code snap may move a value by no more
		// than the store's own uncertainty (one step below a code with a truncating store, half a
		// step above). The old fixed 0.13-code window moved levels by up to 0.13 code (half a 10-bit
		// code at 8 bits), eight steps near black. The CPU model
		// (SigmaDeltaMeanIsUnbiasedAtAnyDitherAmplitude) does not run the snap; this does.
		// VP_TEST_SIGMA_SHADER_DIR loads the shaders from another tree (A/B against a base build);
		// VP_TEST_SIGMA_BIAS_OUT writes the report to a file. The LED hook + encoding repair chain (as
		// the renderer assembles it on 7.360) is reported, not asserted: its offset is the repair's
		// and the rgba16f stores', not the quantiser's (the Beta's rgba32f output pass removes it).
		TEST_METHOD(SigmaDeltaFlatFieldMeanIsUnbiasedOnTheGpu)
		{
			std::string dir;
			{
				char* value = nullptr;
				size_t length = 0;
				if (_dupenv_s(&value, &length, "VP_TEST_SIGMA_SHADER_DIR") == 0 && value) dir = value;
				std::free(value);
			}
			const ChainGpuProbe::ShaderSource source = [dir](const char* file) -> std::string
			{
				if (dir.empty()) return LoadBundledShader(file);
				std::ifstream input(dir + "\\" + file, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
			};
			const double significant = 0.02;
			std::string report = "sigma-delta flat-field bias (shaders: " + (dir.empty() ? std::string("bundled") : dir) + ")\n";
			double worstLoop = 0.0, worstStore = 0.0;
			for (const SigmaDeltaBiasProbe::Mode& mode : SigmaDeltaBiasProbe::Modes())
				for (const bool chain : { false, true })
				{
					TargetLutGpuFixture fixture;
					Assert::IsTrue(fixture.Create());
					const SigmaDeltaBiasProbe::Report rep = SigmaDeltaBiasProbe::Run(fixture.Gpu(), fixture.Renderer(), source, mode,
						false, chain, true);
					report += SigmaDeltaBiasProbe::Summarise(std::string(mode.name) + (chain ? ", LED hook + repair + sigma-delta" :
						", sigma-delta alone"), rep, significant);
					Assert::IsTrue(rep.ok, ExtendedRangeGpuProbe::ToWide(report).c_str());
					if (chain) continue;
					worstStore = (std::max)(worstStore, SigmaDeltaBiasProbe::WorstAgainstStore(rep, significant));
					for (size_t i = 0; i < rep.levels.size(); ++i)
						if (rep.levels[i].family == 1 && std::fabs(rep.levels[i].offset) >= 0.25 &&
							std::fabs(rep.levels[i].xExact - std::round(rep.levels[i].xExact)) > 1.05 * rep.results[i].storeStep &&
							rep.levels[i].xExact >= rep.codeMin && rep.levels[i].xExact <= rep.codeMax)
							worstLoop = (std::max)(worstLoop, std::fabs(rep.results[i].errHook));
				}
			{
				char* value = nullptr;
				size_t length = 0;
				if (_dupenv_s(&value, &length, "VP_TEST_SIGMA_BIAS_OUT") == 0 && value)
					std::ofstream(value, std::ios::binary) << report;
				std::free(value);
			}
			Logger::WriteMessage(report.c_str());
			char verdict[200];
			std::snprintf(verdict, sizeof(verdict), "sigma-delta alone: loop worst %.4f code (limit %.2f); worst against the rgba16f step %.2f (limit 1)\n",
				worstLoop, significant, worstStore);
			Logger::WriteMessage(verdict);
			Assert::IsTrue(worstLoop <= significant, ExtendedRangeGpuProbe::ToWide(
				std::string("the sigma-delta's mean misses its input away from a code (a loop bias)\n") + verdict + report).c_str());
			Assert::IsTrue(worstStore <= 1.0, ExtendedRangeGpuProbe::ToWide(
				std::string("the exact-code snap moves a level by more than the rgba16f store's step (a snap bias)\n") + verdict + report).c_str());
		}

		// Cross-language fixed vectors: the same table is in tools/eval/sigma_delta_vectors.json and
		// is asserted by tools/eval/test_sigma_delta_vectors.py against the numpy port. Exact equality
		// (hex-float literals; QuantiseStep is double, Ign is float).
		TEST_METHOD(CrossLanguageFixedVectors)
		{
			struct Case
			{
				double x, amplitude;
				int bits;
				bool full;
				float px, py;
				int k[16];
				double r[16];
			};
			const Case cases[] = {
				{ 100.3, 0.15, 8, false, 3.0f, 5.0f, { 100, 101, 100, 100, 101, 100, 100, 100, 101, 100, 100, 101, 100, 100, 101, 100 }, { 0x1.3333333333300p-2, -0x1.9999999999a00p-2, -0x1.9999999999c00p-4, 0x1.9999999999800p-3, -0x1.0000000000080p-1, -0x1.9999999999c00p-3, 0x1.9999999999400p-4, 0x1.9999999999800p-2, -0x1.3333333333500p-2, -0x1.0000000000000p-45, 0x1.3333333333100p-2, -0x1.9999999999c00p-2, -0x1.999999999a400p-4, 0x1.9999999999400p-3, -0x1.0000000000180p-1, -0x1.999999999a000p-3 } },
				{ 1.05, 0.5, 8, false, 37.0f, 11.0f, { 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, { -0x1.e666666666666p-1, -0x1.cccccccccccccp-1, -0x1.b333333333332p-1, -0x1.9999999999998p-1, -0x1.7fffffffffffep-1, -0x1.6666666666664p-1, -0x1.4cccccccccccap-1, -0x1.3333333333330p-1, -0x1.1999999999996p-1, -0x1.ffffffffffff8p-2, -0x1.cccccccccccc4p-2, -0x1.9999999999990p-2, -0x1.666666666665cp-2, -0x1.3333333333328p-2, -0x1.fffffffffffe8p-3, -0x1.9999999999980p-3 } },
				{ 253.95, 0.15, 8, false, 211.0f, 2.0f, { 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 253, 254, 254, 254, 254, 254 }, { -0x1.999999999a000p-5, -0x1.999999999a000p-4, -0x1.3333333333800p-3, -0x1.999999999a000p-3, -0x1.0000000000400p-2, -0x1.3333333333800p-2, -0x1.6666666666c00p-2, -0x1.999999999a000p-2, -0x1.cccccccccd400p-2, -0x1.0000000000400p-1, 0x1.cccccccccc400p-2, 0x1.9999999999000p-2, 0x1.6666666665c00p-2, 0x1.3333333332800p-2, 0x1.fffffffffe800p-3, 0x1.9999999998000p-3 } },
				{ 255.6, 0.15, 8, false, 9.0f, 9.0f, { 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254 }, { 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1, 0x1.4cccccccccccdp-1 } },
				{ 4.2, 0.3, 10, false, 64.0f, 7.0f, { 4, 4, 4, 5, 4, 4, 4, 4, 5, 4, 4, 4, 4, 5, 4, 4 }, { 0x1.99999999999a0p-3, 0x1.99999999999a0p-2, 0x1.3333333333338p-1, -0x1.9999999999980p-3, 0x1.0000000000000p-50, 0x1.99999999999c0p-3, 0x1.99999999999b0p-2, 0x1.3333333333340p-1, -0x1.9999999999960p-3, 0x1.0000000000000p-49, 0x1.99999999999e0p-3, 0x1.99999999999c0p-2, 0x1.3333333333348p-1, -0x1.9999999999940p-3, 0x1.8000000000000p-49, 0x1.9999999999a00p-3 } },
				{ 512.5, 0.15, 10, true, 100.0f, 50.0f, { 512, 513, 512, 513, 513, 512, 513, 512, 513, 512, 512, 513, 512, 513, 513, 512 }, { 0x1.0000000000000p-1, 0x0.0p+0, 0x1.0000000000000p-1, 0x0.0p+0, -0x1.0000000000000p-1, 0x0.0p+0, -0x1.0000000000000p-1, 0x0.0p+0, -0x1.0000000000000p-1, 0x0.0p+0, 0x1.0000000000000p-1, 0x0.0p+0, 0x1.0000000000000p-1, 0x0.0p+0, -0x1.0000000000000p-1, 0x0.0p+0 } },
				{ 0.2, 0.5, 8, true, 1.0f, 1.0f, { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, { -0x1.999999999999ap-1, -0x1.3333333333334p-1, -0x1.999999999999bp-2, -0x1.999999999999cp-3, -0x1.0000000000000p-54, -0x1.999999999999ap-1, -0x1.3333333333334p-1, -0x1.999999999999bp-2, -0x1.999999999999cp-3, -0x1.0000000000000p-54, -0x1.999999999999ap-1, -0x1.3333333333334p-1, -0x1.999999999999bp-2, -0x1.999999999999cp-3, -0x1.0000000000000p-54, -0x1.999999999999ap-1 } },
				{ 1018.8, 0.5, 10, false, 500.0f, 300.0f, { 1019, 1019, 1018, 1019, 1019, 1019, 1019, 1018, 1019, 1019, 1019, 1019, 1018, 1019, 1019, 1019 }, { -0x1.999999999a000p-3, -0x1.999999999a000p-2, 0x1.9999999999000p-2, 0x1.9999999998000p-3, -0x1.0000000000000p-42, -0x1.999999999c000p-3, -0x1.999999999b000p-2, 0x1.9999999998000p-2, 0x1.9999999996000p-3, -0x1.0000000000000p-41, -0x1.999999999e000p-3, -0x1.999999999c000p-2, 0x1.9999999997000p-2, 0x1.9999999994000p-3, -0x1.8000000000000p-41, -0x1.99999999a0000p-3 } },
			};
			for (const Case& c : cases)
			{
				const auto limits = SigmaDeltaDomain::Limits(c.bits, c.full);
				double r = 0.0;
				for (int t = 0; t < 16; ++t)
				{
					const double d = (SigmaDeltaDomain::Ign(c.px, c.py, t) - 0.5) * 2.0 * c.amplitude;
					const double k = SigmaDeltaDomain::QuantiseStep(c.x, r, d, limits, c.amplitude);
					Assert::AreEqual(static_cast<double>(c.k[t]), k, L"code matches the fixed vector");
					Assert::AreEqual(c.r[t], r, L"residual matches the fixed vector");
				}
			}
			struct IgnPoint { float px, py; int frame; float value; };
			const IgnPoint points[] = {
				{ 0.0f, 0.0f, 0, 0x0.0p+0f },
				{ 3.0f, 5.0f, 1, 0x1.9fb5000000000p-1f },
				{ 37.0f, 11.0f, 7, 0x1.e718000000000p-3f },
				{ 211.0f, 2.0f, 63, 0x1.ef88000000000p-3f },
				{ 1919.0f, 1079.0f, 64, 0x1.af8e400000000p-2f },
				{ 100.5f, 200.25f, 5, 0x1.b490000000000p-2f },
				{ 0.5f, 0.5f, 33, 0x1.ce6a800000000p-1f },
				{ 4095.0f, 2159.0f, 200, 0x1.9cb4000000000p-4f },
			};
			for (const IgnPoint& p : points)
				Assert::AreEqual(p.value, SigmaDeltaDomain::Ign(p.px, p.py, p.frame), L"Ign matches the fixed vector");
		}
	};

	// ---- DitherNeed (output.dither / dither need map): CPU law, state, sampling, golden cross-check -----------------------------
	namespace
	{
		std::wstring DnWide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		bool DnClose(double actual, double expected, double rel = 1e-9)
		{
			return std::fabs(actual - expected) <= rel * std::fabs(expected) + 1e-14;
		}

		// One golden frame run through the real shared products: grain split, precursors, views, a LUT.
		struct DnFixture
		{
			GrainSplit::Plane luma;
			RegionEvidence::GrainProduct grain;
			RegionEvidence::Precursors pre;
			RegionEvidence::GrainView grainView;
			RegionEvidence::View view;
			LightLevel::Lut lut;
			RegionEvidence::Grid grid;
			void Make(int frame, int blockOverride = 0)
			{
				luma = GrainSplit::Plane(DitherNeedGolden::kW, DitherNeedGolden::kH);
				for (int i = 0; i < DitherNeedGolden::kW * DitherNeedGolden::kH; ++i)
					luma.v[static_cast<size_t>(i)] = DitherNeedGolden::kLuma[frame * DitherNeedGolden::kW * DitherNeedGolden::kH + i];
				const int B = blockOverride > 0 ? blockOverride : RegionEvidence::BlockSizeFor(luma.h);
				RegionEvidence::Externals ex;
				grain = RegionEvidence::ComputeGrain(luma, ex.picture, B);
				RegionEvidence::FrameParams fp;
				fp.blockOverride = B;
				pre = RegionEvidence::Compute(luma, &grain.split, ex, fp);
				grainView = RegionEvidence::GrainView(&grain);
				view = RegionEvidence::View(&pre);
				grid = RegionEvidence::MakeGrid(luma.w, luma.h, RegionEvidence::BlockSizeFor(luma.h));   // the expected grid
				lut = GoldenLut();
			}
			static LightLevel::Lut GoldenLut()
			{
				LightLevel::Lut l;
				for (int i = 0; i < LightLevel::kLutSize; ++i)
				{
					l.e[static_cast<size_t>(i)].V = DitherNeedGolden::kLutV[i];
					l.e[static_cast<size_t>(i)].Sout = DitherNeedGolden::kLutSout[i];
				}
				l.fingerprint = "golden";
				return l;
			}
			DitherNeed::Inputs Inputs() const
			{
				DitherNeed::Inputs in;
				in.grain = &grainView; in.evidence = &view; in.lut = &lut; in.colorBits = 8; in.domainSupported = true;
				return in;
			}
		};

		DitherNeed::FrameArgs DnArgs(const RegionEvidence::Grid& g, double dt, bool disc, const std::string& fp = "fp")
		{
			DitherNeed::FrameArgs a;
			a.grid = g; a.dtSeconds = dt; a.discontinuity = disc; a.lutFingerprint = fp;
			return a;
		}

		// A two-block frame whose first block has the given banding (the rest of the law is fully on: no grain, visible step).
		std::vector<DitherNeed::BlockIn> DnTwoBlocks(double banding0, double banding1 = 1.0)
		{
			std::vector<DitherNeed::BlockIn> b(2);
			for (auto& x : b) { x.N = 0.0; x.meanY = 300.0; x.V = 5.0; x.Sout = 1.0; x.inPicture = true; x.overlay = false; }
			b[0].banding = banding0; b[1].banding = banding1;
			return b;
		}
	}

	TEST_CLASS(DitherNeedTests)
	{
	public:

		TEST_METHOD(LawCases)
		{
			const DitherNeed::Params p;
			const double q = DitherNeed::QOut(8);
			Assert::AreEqual(4.0, q, 0.0, L"8-bit link step in 10-bit codes");
			// grain-free, banding visible: aMax - aBase
			Assert::AreEqual(0.35, DitherNeed::TargetBlock<double>(p, 0.0, 5.0, 1.0, 1.0, true, false, q), 1e-12, L"grain-free");
			// grain above the link step: the grain dithers, no extra
			Assert::AreEqual(0.0, DitherNeed::TargetBlock<double>(p, 9.0, 5.0, 1.0, 1.0, true, false, q), 0.0, L"grain above step");
			// partial grain: sigmaOut 0.25 -> aTarget 0.30 -> extra 0.15
			Assert::AreEqual(0.15, DitherNeed::TargetBlock<double>(p, 1.0, 5.0, 1.0, 1.0, true, false, q), 1e-12, L"partial grain");
			// banding weight scales it
			Assert::AreEqual(0.175, DitherNeed::TargetBlock<double>(p, 0.0, 5.0, 1.0, 0.5, true, false, q), 1e-12, L"banding 0.5");
			// overlay / outside the picture / BTB block / invisible step / flat LUT: nothing
			Assert::AreEqual(0.0, DitherNeed::TargetBlock<double>(p, 0.0, 5.0, 1.0, 1.0, true, true, q), 0.0, L"overlay");
			Assert::AreEqual(0.0, DitherNeed::TargetBlock<double>(p, 0.0, 5.0, 1.0, 1.0, false, false, q), 0.0, L"outside the picture");
			Assert::AreEqual(0.0, DitherNeed::TargetBlock<double>(p, 0.0, 0.01, 1.0, 1.0, true, false, q), 0.0, L"invisible step");
			Assert::AreEqual(0.0, DitherNeed::TargetBlock<double>(p, 0.0, 5.0, 0.0, 1.0, true, false, q), 0.0, L"flat LUT (Sout 0)");
			// V * q / |Sout| = 0.5 -> gVis 0.5; negative Sout uses |Sout|
			Assert::AreEqual(0.175, DitherNeed::TargetBlock<double>(p, 0.0, 0.25, -2.0, 1.0, true, false, q), 1e-12, L"half visible");
			// a real BTB block: the LUT gives V = 0 below code 64
			LightLevel::Lut lut = DnFixture::GoldenLut();
			const LightLevel::Entry btb = LightLevel::Fetch(lut, 30.0);
			Assert::AreEqual(0.0, DitherNeed::TargetBlock<double>(p, 0.0, btb.V, btb.Sout, 1.0, true, false, q), 0.0, L"BTB block");
			// aMax below aBase: no extra
			DitherNeed::Params hi; hi.aBase = 0.6;
			Assert::AreEqual(0.0, DitherNeed::TargetBlock<double>(hi, 0.0, 5.0, 1.0, 1.0, true, false, q), 0.0, L"aMax < aBase");
			// the float template follows the double one
			Assert::AreEqual(0.15, static_cast<double>(DitherNeed::TargetBlock<float>(p, 1.0f, 5.0f, 1.0f, 1.0f, true, false, 4.0f)), 1e-6, L"float");
		}

		TEST_METHOD(TenBitLinkUsesStepOne)
		{
			const DitherNeed::Params p;
			Assert::AreEqual(4.0, DitherNeed::QOut(8), 0.0);
			Assert::AreEqual(1.0, DitherNeed::QOut(10), 0.0);
			Assert::AreEqual(0.25, DitherNeed::QOut(12), 0.0);
			// V * q / Sout: 0.5 * 4 = 2 (fully visible at 8 bit), 0.5 * 1 = 0.5 (half visible at 10 bit)
			Assert::AreEqual(0.35, DitherNeed::TargetBlock<double>(p, 0.0, 0.5, 1.0, 1.0, true, false, DitherNeed::QOut(8)), 1e-12);
			Assert::AreEqual(0.175, DitherNeed::TargetBlock<double>(p, 0.0, 0.5, 1.0, 1.0, true, false, DitherNeed::QOut(10)), 1e-12);
			// grain rms in link codes: N = 0.0625 -> 0.25 at q 1, 0.0625 at q 4
			Assert::AreEqual(0.15 + 0.0, DitherNeed::TargetBlock<double>(p, 0.0625, 5.0, 1.0, 1.0, true, false, 1.0), 1e-12, L"q=1: sigmaOut .25");
			Assert::AreEqual(0.55 - 0.0625 - 0.15, DitherNeed::TargetBlock<double>(p, 0.0625, 5.0, 1.0, 1.0, true, false, 4.0), 1e-12, L"q=4: sigmaOut .0625");
			// 10-bit link through the Map: colorBits 10 is q 1
			DitherNeed::Map m;
			DitherNeed::FrameArgs a = DnArgs(RegionEvidence::MakeGrid(16, 8, 8), 1.0 / 60, false);
			a.colorBits = 10;
			std::vector<DitherNeed::BlockIn> b = DnTwoBlocks(1.0);
			b[0].V = 0.5;
			Assert::IsTrue(m.Update(a, b) == DitherNeed::Result::On);
			Assert::AreEqual(0.175, m.Extra()[0], 1e-12, L"10-bit link: step 1");
			Assert::AreEqual(0.35, m.Extra()[1], 1e-12);
		}

		TEST_METHOD(SmoothingResetDeadbandAndAlpha)
		{
			const RegionEvidence::Grid g = RegionEvidence::MakeGrid(16, 8, 8);   // 2 x 1 blocks
			Assert::AreEqual(1, g.gh);
			DitherNeed::Map m;
			const double dt = 1.0 / 60;
			Assert::IsTrue(m.Update(DnArgs(g, dt, false), DnTwoBlocks(1.0)) == DitherNeed::Result::On);
			Assert::AreEqual(0.35, m.Extra()[0], 1e-12, L"first frame resets to the target");

			// target 0 (banding 0): one smoothing step with alpha from dt
			Assert::IsTrue(m.Update(DnArgs(g, dt, false), DnTwoBlocks(0.0)) == DitherNeed::Result::On);
			const double alpha = 1.0 - std::exp(-dt / 0.25);
			Assert::AreEqual(DitherNeed::AlphaFor(dt, 0.25), alpha, 1e-15);
			Assert::AreEqual(0.35 * (1.0 - alpha), m.Extra()[0], 1e-12, L"one-pole step");
			Assert::AreEqual(0.35, m.Extra()[1], 1e-12, L"the other block holds its target");
			Assert::AreEqual(1.0, DitherNeed::AlphaFor(0.0, 0.25), 0.0, L"unknown dt: no smoothing");
			Assert::AreEqual(1.0, DitherNeed::AlphaFor(-1.0, 0.25), 0.0);

			// discontinuity: reset to the target
			Assert::IsTrue(m.Update(DnArgs(g, dt, true), DnTwoBlocks(0.0)) == DitherNeed::Result::On);
			Assert::AreEqual(0.0, m.Extra()[0], 0.0, L"discontinuity resets");

			// deadband: a target within 0.02 of e holds e exactly
			m.Update(DnArgs(g, dt, true), DnTwoBlocks(1.0));                     // e = 0.35
			m.Update(DnArgs(g, dt, false), DnTwoBlocks((0.35 - 0.015) / 0.35));  // target 0.335: |diff| 0.015 <= 0.02
			Assert::AreEqual(0.35, m.Extra()[0], 1e-12, L"hysteresis holds");
			m.Update(DnArgs(g, dt, false), DnTwoBlocks((0.35 - 0.03) / 0.35));   // target 0.32: moves
			Assert::IsTrue(m.Extra()[0] < 0.35 - 1e-6 && m.Extra()[0] > 0.32, L"moves beyond the deadband, not all the way");

			// every other reset reason, each with a target inside the deadband (so only the reset can move e)
			const double inside = (0.35 - 0.01) / 0.35;
			{
				DitherNeed::Map r; r.Update(DnArgs(g, dt, false), DnTwoBlocks(1.0));
				r.Update(DnArgs(g, dt, false, "other lut"), DnTwoBlocks(inside));
				Assert::AreEqual(0.34, r.Extra()[0], 1e-12, L"LUT fingerprint change resets");
			}
			{
				DitherNeed::Map r; r.Update(DnArgs(g, dt, false), DnTwoBlocks(1.0));
				DitherNeed::FrameArgs a = DnArgs(g, dt, false); a.colorBits = 10;   // visible at both depths: only the reset moves e
				r.Update(a, DnTwoBlocks(inside));
				Assert::AreEqual(0.34, r.Extra()[0], 1e-12, L"colour depth change resets");
			}
			{
				DitherNeed::Map r; r.Update(DnArgs(g, dt, false), DnTwoBlocks(1.0));
				DitherNeed::FrameArgs a = DnArgs(g, dt, false); a.params.aBase = 0.10;   // target (0.5 - 0.10) * 0.85 = 0.34
				r.Update(a, DnTwoBlocks(0.85));
				Assert::AreEqual(0.34, r.Extra()[0], 1e-12, L"aBase change resets");
			}
			{
				DitherNeed::Map r; r.Update(DnArgs(g, dt, false), DnTwoBlocks(1.0));
				const RegionEvidence::Grid g2 = RegionEvidence::MakeGrid(16, 8, 4);   // 4 x 2 blocks
				std::vector<DitherNeed::BlockIn> b(g2.Count(), DnTwoBlocks(inside)[0]);
				r.Update(DnArgs(g2, dt, false), b);
				Assert::AreEqual(g2.Count(), r.Extra().size());
				Assert::AreEqual(0.34, r.Extra()[0], 1e-12, L"grid change resets");
			}
		}

		TEST_METHOD(SameTimeConstantAtTwoFrameRates)
		{
			const RegionEvidence::Grid g = RegionEvidence::MakeGrid(16, 8, 8);
			DitherNeed::Map fast, slow;
			fast.Update(DnArgs(g, 1.0 / 60, false), DnTwoBlocks(1.0));
			slow.Update(DnArgs(g, 1.0 / 30, false), DnTwoBlocks(1.0));
			// 60 Hz, two steps toward 0  ==  30 Hz, one step toward 0
			fast.Update(DnArgs(g, 1.0 / 60, false), DnTwoBlocks(0.0));
			fast.Update(DnArgs(g, 1.0 / 60, false), DnTwoBlocks(0.0));
			slow.Update(DnArgs(g, 1.0 / 30, false), DnTwoBlocks(0.0));
			Assert::IsTrue(std::fabs(fast.Extra()[0] - slow.Extra()[0]) < 1e-12, L"same e after the same wall-clock time");
			Assert::IsTrue(fast.Extra()[0] > 0.0 && fast.Extra()[0] < 0.35);
			Assert::AreEqual(0.35 * std::exp(-2.0 / 60 / 0.25), fast.Extra()[0], 1e-12, L"exp(-t / tau)");
		}

		TEST_METHOD(MissingInputsGiveZeroNamedStatusAndRecover)
		{
			DnFixture f;
			f.Make(0);
			std::vector<DitherNeed::BlockIn> blocks;
			Assert::AreEqual(0u, DitherNeed::Gather(f.Inputs(), f.grid, blocks), L"complete inputs");
			Assert::AreEqual(static_cast<size_t>(DitherNeedGolden::kBlocks), blocks.size());

			DitherNeed::Map m;
			DitherNeed::FrameArgs a = DnArgs(f.grid, 1.0 / 60, false, "golden");
			Assert::IsTrue(m.Update(a, blocks) == DitherNeed::Result::On);
			double sum = 0.0; for (double e : m.Extra()) sum += e;
			Assert::IsTrue(sum > 1.0, L"some extra amplitude on a grain-free ramp");
			size_t raised = 0; for (double e : m.Extra()) if (e > 1e-3) ++raised;
			Assert::IsTrue(m.Status() == "dither need: on (" + std::to_string(raised) + " of " + std::to_string(DitherNeedGolden::kBlocks) +
				" blocks raised)", DnWide(m.Status()).c_str());

			{
				DitherNeed::Inputs in = f.Inputs(); in.grain = nullptr;
				std::vector<DitherNeed::BlockIn> b;
				Assert::AreEqual(static_cast<unsigned>(DitherNeed::kNoGrain), DitherNeed::Gather(in, f.grid, b), L"null grain");
				Assert::IsTrue(b.empty());
				const RegionEvidence::GrainView none(nullptr);
				in.grain = &none;
				Assert::AreEqual(static_cast<unsigned>(DitherNeed::kNoGrain), DitherNeed::Gather(in, f.grid, b), L"invalid grain view");
			}
			{
				DnFixture other; other.Make(0, 16);   // grain and precursors on 16 px blocks, expected grid 8 px
				DitherNeed::Inputs in = f.Inputs(); in.grain = &other.grainView;
				std::vector<DitherNeed::BlockIn> b;
				Assert::AreEqual(static_cast<unsigned>(DitherNeed::kGrainGridMismatch), DitherNeed::Gather(in, f.grid, b), L"grain grid");
				in = f.Inputs(); in.evidence = &other.view;
				Assert::AreEqual(static_cast<unsigned>(DitherNeed::kEvidenceGridMismatch), DitherNeed::Gather(in, f.grid, b), L"evidence grid");
			}
			{
				DitherNeed::Inputs in = f.Inputs(); in.evidence = nullptr;
				std::vector<DitherNeed::BlockIn> b;
				Assert::AreEqual(static_cast<unsigned>(DitherNeed::kNoPrecursors), DitherNeed::Gather(in, f.grid, b), L"no precursors");
				in = f.Inputs(); in.lut = nullptr;
				Assert::AreEqual(static_cast<unsigned>(DitherNeed::kNoLut), DitherNeed::Gather(in, f.grid, b), L"no lut");
				in = f.Inputs(); in.domainSupported = false;
				Assert::AreEqual(static_cast<unsigned>(DitherNeed::kUnsupportedDomain), DitherNeed::Gather(in, f.grid, b), L"domain");
				in = f.Inputs(); in.grain = nullptr; in.lut = nullptr;
				Assert::AreEqual(static_cast<unsigned>(DitherNeed::kNoGrain | DitherNeed::kNoLut), DitherNeed::Gather(in, f.grid, b));
			}
			Assert::IsTrue(DitherNeed::MissingText(DitherNeed::kNoGrain) == "no grain product");
			Assert::IsTrue(DitherNeed::MissingText(DitherNeed::kGrainGridMismatch) == "grain product on another grid");
			Assert::IsTrue(DitherNeed::MissingText(DitherNeed::kNoPrecursors) == "no region precursors");
			Assert::IsTrue(DitherNeed::MissingText(DitherNeed::kNoLut) == "no light-level table");
			Assert::IsTrue(DitherNeed::MissingText(DitherNeed::kUnsupportedDomain) == "output domain not supported");
			Assert::IsTrue(DitherNeed::MissingText(DitherNeed::kNoGrain | DitherNeed::kNoLut) == "no grain product; no light-level table");
			Assert::IsTrue(DitherNeed::MissingText(0).empty());

			// missing: target 0 everywhere, e reset, status names the input, base amplitude (today's behaviour)
			a.missing = DitherNeed::kNoGrain | DitherNeed::kUnsupportedDomain;
			Assert::IsTrue(m.Update(a, std::vector<DitherNeed::BlockIn>()) == DitherNeed::Result::Unavailable);
			for (double e : m.Extra()) Assert::AreEqual(0.0, e, 0.0);
			Assert::IsTrue(m.Status() == "dither need: unavailable (no grain product; output domain not supported); sigma-delta at its base amplitude",
				DnWide(m.Status()).c_str());
			Assert::IsFalse(m.Valid());
			// automatic recovery: the next complete frame is a reset, e = target (no leftover state)
			a.missing = 0;
			Assert::IsTrue(m.Update(a, blocks) == DitherNeed::Result::On);
			DitherNeed::Map fresh;
			fresh.Update(a, blocks);
			Assert::IsTrue(m.Extra() == fresh.Extra(), L"recovery equals a fresh start");

			// off and not-needed
			a.enabled = false;
			Assert::IsTrue(m.Update(a, blocks) == DitherNeed::Result::Off);
			Assert::IsTrue(m.Status() == "dither need: off");
			for (double e : m.Extra()) Assert::AreEqual(0.0, e, 0.0);
			a.enabled = true; a.needed = false;
			Assert::IsTrue(m.Update(a, blocks) == DitherNeed::Result::NotNeeded);
			Assert::IsTrue(m.Status() == "dither need: not needed (link step invisible)");
			for (double e : m.Extra()) Assert::AreEqual(0.0, e, 0.0);
			a.needed = true;
			Assert::IsTrue(m.Update(a, blocks) == DitherNeed::Result::On, L"recovers from not-needed");
			Assert::IsTrue(m.Extra() == fresh.Extra());
			// a block count that does not fit the grid is refused, never read out of range
			Assert::IsTrue(m.Update(a, std::vector<DitherNeed::BlockIn>(3)) == DitherNeed::Result::Unavailable);
		}

		TEST_METHOD(DeterministicTwoRunsAreBitIdentical)
		{
			std::vector<double> runs[2];
			for (int r = 0; r < 2; ++r)
			{
				DnFixture f; DitherNeed::Map m;
				for (int s = 0; s < DitherNeedGolden::kSteps; ++s)
				{
					f.Make(DitherNeedGolden::kStep[s].frame);
					std::vector<DitherNeed::BlockIn> b;
					Assert::AreEqual(0u, DitherNeed::Gather(f.Inputs(), f.grid, b));
					m.Update(DnArgs(f.grid, DitherNeedGolden::kStep[s].dt, DitherNeedGolden::kStep[s].discontinuity != 0, "golden"), b);
					runs[r].insert(runs[r].end(), m.Extra().begin(), m.Extra().end());
				}
			}
			Assert::AreEqual(runs[0].size(), runs[1].size());
			Assert::IsTrue(std::memcmp(runs[0].data(), runs[1].data(), runs[0].size() * sizeof(double)) == 0, L"bitwise identical");
		}

		TEST_METHOD(ParseDefaultsOffAndRejectsInvalid)
		{
			auto parse = [](const char* key, const char* value, DitherNeed::Settings& s, std::vector<std::string>& errs)
			{
				const std::string k = key ? key : "", v = value ? value : "";
				return DitherNeed::Parse([&](const std::string& name, std::string& out)
				{
					if (!key || name != k) return false;
					out = v;
					return true;
				}, s, errs);
			};
			DitherNeed::Settings s; std::vector<std::string> errs;
			s.on = true;
			Assert::IsTrue(parse(nullptr, nullptr, s, errs)); Assert::IsFalse(s.on, L"absent = off"); Assert::IsTrue(errs.empty());
			s.on = true; Assert::IsTrue(parse("dither_need", "", s, errs)); Assert::IsFalse(s.on, L"empty = off"); Assert::IsTrue(errs.empty());
			Assert::IsTrue(parse("dither_need", "off", s, errs)); Assert::IsFalse(s.on);
			Assert::IsTrue(parse("dither_need", "OFF", s, errs)); Assert::IsFalse(s.on);
			Assert::IsTrue(parse("dither_need", "auto", s, errs)); Assert::IsTrue(s.on);
			Assert::IsTrue(parse("dither_need", " Auto ", s, errs)); Assert::IsTrue(s.on, L"case-insensitive, blanks trimmed");
			Assert::IsTrue(errs.empty());
			Assert::IsFalse(parse("dither_need", "maybe", s, errs));
			Assert::IsFalse(s.on, L"invalid stays off");
			Assert::AreEqual(static_cast<size_t>(1), errs.size());
			Assert::IsTrue(errs[0] == "dither_need: invalid value 'maybe' (expected off or auto); using off", DnWide(errs[0]).c_str());
			s.on = true; errs.clear();
			Assert::IsFalse(parse("dither_need", "on", s, errs)); Assert::IsFalse(s.on);
			Assert::IsTrue(errs[0] == "dither_need: invalid value 'on' (expected off or auto); using off");
		}

		TEST_METHOD(AppendProductDeclaresOneNode)
		{
			std::vector<VpSkeleton::ProductDecl> decls;
			const VpSkeleton::Dims frame = { 1920, 1080 };
			const RegionEvidence::ProductIds ids = RegionEvidence::AppendProducts(decls, frame);
			const size_t before = decls.size();
			const VpSkeleton::ProductId id = DitherNeed::AppendProduct(decls, ids.grainSplit, ids.blockStats, frame);
			Assert::AreEqual(before + 1, decls.size(), L"exactly one node appended");
			int named = 0;
			for (const auto& d : decls) if (std::strcmp(d.token, "dither.need") == 0) ++named;
			Assert::AreEqual(1, named);
			const VpSkeleton::ProductDecl& d = decls.back();
			Assert::AreEqual(static_cast<int>(id), static_cast<int>(d.id));
			Assert::AreEqual(static_cast<int>(before), static_cast<int>(d.id), L"dense id");
			Assert::AreEqual(2, static_cast<int>(d.inputCount));
			Assert::AreEqual(static_cast<int>(ids.grainSplit), static_cast<int>(d.inputs[0].product));
			Assert::AreEqual(static_cast<int>(ids.blockStats), static_cast<int>(d.inputs[1].product));
			Assert::AreEqual(0, static_cast<int>(d.inputs[0].lag));
			Assert::AreEqual(0, static_cast<int>(d.inputs[1].lag));
			Assert::IsTrue(d.backend == VpSkeleton::Backend::GpuCompute);
			Assert::AreEqual(1, static_cast<int>(d.history));
			Assert::IsTrue(d.missingInputForm);
			Assert::IsFalse(d.sourceFrame, L"filled live by its consumer");
			Assert::AreEqual(static_cast<uint32_t>(RegionEvidence::CostAt(frame).blocks * 32), d.size.fixedBytes, L"2 x rgba32f per block");
			const VpSkeleton::ConsumerDecl c = DitherNeed::Consumer(id, 7);
			Assert::AreEqual(1, static_cast<int>(c.count));
			Assert::AreEqual(static_cast<int>(id), static_cast<int>(c.consumes[0].product));
		}

		// The quantiser model: with the total amplitude (noise_amplitude + extra) at 0.5 the dither stays inside (-0.5, 0.5), so
		// codes that are exact link codes are never moved, BTB / WTW survive, and the code limits still clamp.
		TEST_METHOD(QuantiserAtMaxAmplitudeKeepsExactCodesAndStaysUnbiased)
		{
			const DitherNeed::Params p;
			const double amplitude = p.aBase + (p.aMax - p.aBase);
			Assert::AreEqual(0.5, amplitude, 1e-15);
			const SigmaDeltaDomain::CodeLimits lim = SigmaDeltaDomain::Limits(8, false);
			Assert::AreEqual(1.0, lim.min, 0.0); Assert::AreEqual(254.0, lim.max, 0.0);
			// 1 = lowest kept BTB code, 16 limited black, 100, 235 white, 240 WTW, 254 highest kept WTW
			const double exact[] = { 1.0, 5.0, 16.0, 100.0, 235.0, 240.0, 254.0 };
			for (double x : exact)
			{
				double r = 0.0;
				for (int frame = 0; frame < 256; ++frame)
					for (int px = 0; px < 8; ++px)
					{
						const double d = (static_cast<double>(SigmaDeltaDomain::Ign(static_cast<float>(px * 37 + 3), static_cast<float>(px * 11 + 1), frame)) - 0.5) * 2.0 * amplitude;
						Assert::IsTrue(std::fabs(d) <= amplitude);
						const double k = SigmaDeltaDomain::QuantiseStep(x, r, d, lim, amplitude);
						Assert::AreEqual(x, k, 0.0, L"an exact code is never moved");
						Assert::AreEqual(0.0, r, 0.0, L"and carries no residual");
					}
			}
			// timing reference codes clamp into the kept range, the residual stays bounded
			for (double x : { 0.0, 255.0 })
			{
				double r = 0.0;
				for (int frame = 0; frame < 64; ++frame)
				{
					const double d = (static_cast<double>(SigmaDeltaDomain::Ign(5.0f, 9.0f, frame)) - 0.5) * 2.0 * amplitude;
					const double k = SigmaDeltaDomain::QuantiseStep(x, r, d, lim, amplitude);
					Assert::AreEqual(x == 0.0 ? lim.min : lim.max, k, 0.0);
					Assert::IsTrue(std::fabs(r) <= SigmaDeltaDomain::ResidualBound(amplitude) + 1e-12);
				}
			}
			// unbiased: the long-run mean of the output is the input (flat 100.3), at the base and at the raised amplitude
			for (double amp : { p.aBase, amplitude })
			{
				double r = 0.0, sum = 0.0;
				const int frames = 4096;
				for (int frame = 0; frame < frames; ++frame)
				{
					const double d = (static_cast<double>(SigmaDeltaDomain::Ign(17.0f, 23.0f, frame)) - 0.5) * 2.0 * amp;
					sum += SigmaDeltaDomain::QuantiseStep(100.3, r, d, lim, amp);
				}
				Assert::IsTrue(std::fabs(sum / frames - 100.3) < 0.01, L"unbiased");
			}
		}

		TEST_METHOD(NeededAnywhereAndItsCache)
		{
			const DitherNeed::Params p;
			LightLevel::Lut flat;
			for (auto& e : flat.e) { e.V = 0.01; e.Sout = 1.0; }
			flat.fingerprint = "flat";
			Assert::IsFalse(DitherNeed::NeededAnywhere(flat, 4.0, p), L"tiny V: the link step is invisible");
			// a visible step only below code 64 (BTB) does not count
			LightLevel::Lut btbOnly = flat;
			for (int c = 0; c < 64; ++c) btbOnly.e[static_cast<size_t>(c)].V = 5.0;
			Assert::IsFalse(DitherNeed::NeededAnywhere(btbOnly, 4.0, p), L"codes below 64 are not counted");
			LightLevel::Lut one = flat;
			one.e[600].V = 5.0;
			Assert::IsTrue(DitherNeed::NeededAnywhere(one, 4.0, p), L"one visible code is enough");
			Assert::IsTrue(DitherNeed::Gate(one, 4.0, p).worstCode == 600);
			Assert::IsFalse(DitherNeed::NeededAnywhere(one, 0.01, p), L"a finer link step is invisible");

			// HdrToSdr 1000 nits, defaults, 8-bit link (q 4): needed
			const LightLevel::Settings s = LightLevel::Read([](const std::string&, std::string&) { return false; });
			LightLevel::Context c;
			c.kind = LightLevel::Context::HdrToSdr;
			c.sourcePeakNits = 1000.0;
			const LightLevel::Lut hdr = LightLevel::Build(s, c);
			Assert::IsTrue(DitherNeed::NeededAnywhere(hdr, 4.0, p));

			DitherNeed::NeedCache cache;
			Assert::IsFalse(cache.Get(flat, 4.0, p)); Assert::IsTrue(cache.LastRebuilt());
			Assert::IsFalse(cache.Get(flat, 4.0, p)); Assert::IsFalse(cache.LastRebuilt(), L"cached per fingerprint");
			Assert::IsTrue(cache.Get(hdr, 4.0, p)); Assert::IsTrue(cache.LastRebuilt(), L"another LUT");
			Assert::IsTrue(cache.Get(hdr, 4.0, p)); Assert::IsFalse(cache.LastRebuilt());
			cache.Get(hdr, 1.0, p); Assert::IsTrue(cache.LastRebuilt(), L"another link step");
			DitherNeed::Params q = p; q.visHi = 0.9;
			cache.Get(hdr, 1.0, q); Assert::IsTrue(cache.LastRebuilt(), L"another visibility ramp");
		}

		TEST_METHOD(SampleExtraBilinearConstantGradientAndClamp)
		{
			const int gw = 4, gh = 3, B = 8;
			std::vector<float> constant(gw * gh, 0.3f);
			for (int y : { 0, 3, 11, 12, 23, 40, 1000 })
				for (int x : { 0, 1, 7, 8, 15, 31, 32, 1000 })
					Assert::AreEqual(0.3, static_cast<double>(DitherNeed::SampleExtra(constant.data(), gw, gh, B, 1.0f, 1.0f, 0.0f, 0.0f, x, y)), 1e-6, L"constant");
			// column index as value, row index * 10: gradients
			std::vector<float> grad(gw * gh);
			for (int j = 0; j < gh; ++j) for (int i = 0; i < gw; ++i) grad[static_cast<size_t>(j) * gw + i] = static_cast<float>(i + 10 * j);
			// offset 0.5: src = pos + 1; a block centre is src = 8 i + 4, i.e. pos = 8 i + 3
			for (int j = 0; j < gh; ++j) for (int i = 0; i < gw; ++i)
				Assert::AreEqual(static_cast<double>(i + 10 * j), static_cast<double>(DitherNeed::SampleExtra(grad.data(), gw, gh, B, 1.0f, 1.0f, 0.5f, 0.5f, 8 * i + 3, 8 * j + 3)), 1e-5, L"block centre");
			// halfway between two centres: pos = 8 i + 7
			Assert::AreEqual(1.5, static_cast<double>(DitherNeed::SampleExtra(grad.data(), gw, gh, B, 1.0f, 1.0f, 0.5f, 0.5f, 8 * 1 + 7, 3)), 1e-5, L"halfway in x");
			Assert::AreEqual(5.0, static_cast<double>(DitherNeed::SampleExtra(grad.data(), gw, gh, B, 1.0f, 1.0f, 0.5f, 0.5f, 3, 7)), 1e-5, L"halfway in y");
			Assert::AreEqual(6.5, static_cast<double>(DitherNeed::SampleExtra(grad.data(), gw, gh, B, 1.0f, 1.0f, 0.5f, 0.5f, 15, 7)), 1e-5, L"halfway in both");
			// clamped at the grid edges: before the first and beyond the last centre the edge value
			Assert::AreEqual(0.0, static_cast<double>(DitherNeed::SampleExtra(grad.data(), gw, gh, B, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0)), 1e-6, L"top-left");
			Assert::AreEqual(3.0, static_cast<double>(DitherNeed::SampleExtra(grad.data(), gw, gh, B, 1.0f, 1.0f, 0.0f, 0.0f, 1000, 0)), 1e-6, L"right of the grid");
			Assert::AreEqual(23.0, static_cast<double>(DitherNeed::SampleExtra(grad.data(), gw, gh, B, 1.0f, 1.0f, 0.0f, 0.0f, 1000, 1000)), 1e-6, L"bottom-right");
			Assert::AreEqual(20.0, static_cast<double>(DitherNeed::SampleExtra(grad.data(), gw, gh, B, 1.0f, 1.0f, 0.0f, 0.0f, -40, 1000)), 1e-6, L"left of the grid");
			// scale / offset against the formula in double
			const float sx = 0.75f, ox = 3.0f, sy = 0.75f, oy = 1.5f;
			for (int y : { 2, 9, 17 }) for (int x : { 4, 13, 22 })
			{
				const double u = ((x + 0.5) * sx + ox) / B - 0.5, v = ((y + 0.5) * sy + oy) / B - 0.5;
				const double ex = std::min<double>(std::max<double>(u, 0.0), gw - 1) , ey = std::min<double>(std::max<double>(v, 0.0), gh - 1);
				Assert::AreEqual(ex + 10 * ey, static_cast<double>(DitherNeed::SampleExtra(grad.data(), gw, gh, B, sx, sy, ox, oy, x, y)), 1e-4, L"scale and offset");
			}
			Assert::AreEqual(0.0, static_cast<double>(DitherNeed::SampleExtra(nullptr, gw, gh, B, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0)), 0.0);
		}

		TEST_METHOD(FloatTemplateFollowsTheDoubleReference)
		{
			DnFixture f; f.Make(1);
			std::vector<DitherNeed::BlockIn> bd;
			Assert::AreEqual(0u, DitherNeed::Gather(f.Inputs(), f.grid, bd));
			std::vector<DitherNeed::BlockInT<float>> bf(bd.size());
			for (size_t i = 0; i < bd.size(); ++i)
			{
				bf[i].N = static_cast<float>(bd[i].N); bf[i].meanY = static_cast<float>(bd[i].meanY);
				bf[i].banding = static_cast<float>(bd[i].banding); bf[i].V = static_cast<float>(bd[i].V);
				bf[i].Sout = static_cast<float>(bd[i].Sout); bf[i].inPicture = bd[i].inPicture; bf[i].overlay = bd[i].overlay;
			}
			DitherNeed::Map md; DitherNeed::MapF mf;
			const DitherNeed::FrameArgs a = DnArgs(f.grid, 1.0 / 60, false, "golden");
			md.Update(a, bd); mf.Update(a, bf);
			double worst = 0.0;
			for (size_t i = 0; i < bd.size(); ++i) worst = std::max(worst, std::fabs(md.Extra()[i] - static_cast<double>(mf.Extra()[i])));
			Assert::IsTrue(worst < 1e-5, L"float template vs double reference");
		}

		// The law + state + sampling against tools/eval/dither_need_ref.py (generated golden, double path 1e-9 relative).
		TEST_METHOD(GoldenCrossCheckLawStateAndSampling)
		{
			using namespace DitherNeedGolden;
			const DitherNeed::Params p;
			const LightLevel::Lut lut = DnFixture::GoldenLut();
			const double q = DitherNeed::QOut(kColorBits);
			// per-frame targets from the golden block inputs
			std::vector<std::vector<DitherNeed::BlockIn>> blocks(kFrames);
			for (int f = 0; f < kFrames; ++f)
			{
				blocks[static_cast<size_t>(f)].resize(kBlocks);
				for (int i = 0; i < kBlocks; ++i)
				{
					DitherNeed::BlockIn& b = blocks[static_cast<size_t>(f)][static_cast<size_t>(i)];
					const int k = f * kBlocks + i;
					b.N = kN[k]; b.meanY = kMeanY[k]; b.banding = kBanding[k]; b.inPicture = kInPicture[k] != 0; b.overlay = false;
					const LightLevel::Entry en = LightLevel::Fetch(lut, b.meanY);
					b.V = en.V; b.Sout = en.Sout;
					const double t = DitherNeed::TargetBlock<double>(p, b.N, b.V, b.Sout, b.banding, b.inPicture, b.overlay, q);
					Assert::IsTrue(DnClose(t, kTarget[k]), (L"target frame " + std::to_wstring(f) + L" block " + std::to_wstring(i)).c_str());
				}
			}
			// state over the step sequence, then the sampled extras
			DitherNeed::Map m;
			const RegionEvidence::Grid grid = RegionEvidence::MakeGrid(kW, kH, kB);
			Assert::AreEqual(kGw, grid.gw); Assert::AreEqual(kGh, grid.gh);
			double worstSample = 0.0;
			for (int s = 0; s < kSteps; ++s)
			{
				Assert::IsTrue(m.Update(DnArgs(grid, kStep[s].dt, kStep[s].discontinuity != 0, "golden"), blocks[static_cast<size_t>(kStep[s].frame)]) == DitherNeed::Result::On);
				for (int i = 0; i < kBlocks; ++i)
					Assert::IsTrue(DnClose(m.Extra()[static_cast<size_t>(i)], kE[s * kBlocks + i]), (L"e step " + std::to_wstring(s) + L" block " + std::to_wstring(i)).c_str());
				const std::vector<float> map = m.ExtraFloat();
				for (int k = 0; k < kSamples; ++k)
				{
					const float v = DitherNeed::SampleExtra(map.data(), kGw, kGh, kB, static_cast<float>(kScale), static_cast<float>(kScale),
						static_cast<float>(kOffX), static_cast<float>(kOffY), kSampleX[k], kSampleY[k]);
					worstSample = std::max(worstSample, std::fabs(static_cast<double>(v) - static_cast<double>(kSampled[s * kSamples + k])));
				}
			}
			char msg[120];
			std::snprintf(msg, sizeof(msg), "dither need golden: worst sampled-extra difference (float) %.3e", worstSample);
			Logger::WriteMessage(msg);
			Assert::IsTrue(worstSample <= 2e-6, L"float sampling within 2e-6 of the numpy float32 mirror");
		}

		// The python block inputs are a vectorised port of GrainSplit / StatsBlock / DeriveBlock; the C++ RegionEvidence chain on the
		// same luma must agree. Measured on the three golden frames: N relative 2.3e-16, meanY exact, banding 2.4e-8 (Sample holds it as
		// float); the tolerances below are those rounded up.
		TEST_METHOD(GoldenRegionEvidenceInputsAgreeWithThePythonMirror)
		{
			using namespace DitherNeedGolden;
			double worstRelN = 0.0, worstMean = 0.0, worstBanding = 0.0;
			for (int f = 0; f < kFrames; ++f)
			{
				DnFixture fx; fx.Make(f);
				std::vector<DitherNeed::BlockIn> b;
				Assert::AreEqual(0u, DitherNeed::Gather(fx.Inputs(), fx.grid, b));
				Assert::AreEqual(static_cast<size_t>(kBlocks), b.size());
				for (int i = 0; i < kBlocks; ++i)
				{
					const int k = f * kBlocks + i;
					const DitherNeed::BlockIn& c = b[static_cast<size_t>(i)];
					worstRelN = std::max(worstRelN, std::fabs(c.N - kN[k]) / std::max(std::fabs(kN[k]), 1e-9));
					worstMean = std::max(worstMean, std::fabs(c.meanY - kMeanY[k]));
					worstBanding = std::max(worstBanding, std::fabs(c.banding - kBanding[k]));
					Assert::AreEqual(kInPicture[k] != 0, c.inPicture);
					Assert::IsFalse(c.overlay);
				}
			}
			char msg[200];
			std::snprintf(msg, sizeof(msg), "dither need inputs vs python: N rel %.3e, meanY abs %.3e, banding abs %.3e", worstRelN, worstMean, worstBanding);
			Logger::WriteMessage(msg);
			Assert::IsTrue(worstRelN <= 1e-12, L"N");
			Assert::IsTrue(worstMean <= 1e-9, L"meanY");
			Assert::IsTrue(worstBanding <= 1e-7, L"banding (float in Sample)");
		}

		// The shared reset / off / missing decision needs no grain or evidence planes (the GPU callers use it alone).
		TEST_METHOD(StateTrackerDecidesResetOffAndMissingWithoutPlanes)
		{
			const RegionEvidence::Grid g = RegionEvidence::MakeGrid(96, 64, 8);
			DitherNeed::StateTracker t;
			DitherNeed::FrameArgs a = DnArgs(g, 1.0 / 60, false, "fp");
			a.needed = true;
			DitherNeed::Decision d = t.Decide(a);
			Assert::IsTrue(d.result == DitherNeed::Result::On && d.reset && !d.off, L"first frame resets");
			Assert::AreEqual(std::string("dither need: on"), d.status);
			d = t.Decide(a);
			Assert::IsFalse(d.reset, L"held");
			DitherNeed::FrameArgs b = a; b.discontinuity = true;
			Assert::IsTrue(t.Decide(b).reset, L"discontinuity");
			b = a; b.colorBits = 10;
			Assert::IsTrue(t.Decide(b).reset, L"colour depth");
			Assert::IsFalse(t.Decide(b).reset);
			b.params.aBase = 0.1;
			Assert::IsTrue(t.Decide(b).reset, L"aBase");
			b.lutFingerprint = "other";
			Assert::IsTrue(t.Decide(b).reset, L"LUT fingerprint");
			b.grid.B = 16;
			Assert::IsTrue(t.Decide(b).reset, L"grid");
			// missing: off, named, recovery resets
			b.missing = DitherNeed::kLutSizeMismatch | DitherNeed::kNoSetTexture | DitherNeed::kPassFailed;
			d = t.Decide(b);
			Assert::IsTrue(d.off && d.result == DitherNeed::Result::Unavailable && !t.Valid());
			Assert::IsTrue(d.status.find("light-level table of the wrong size") != std::string::npos);
			Assert::IsTrue(d.status.find("libplacebo without set_texture") != std::string::npos);
			Assert::IsTrue(d.status.find("GPU pass failed") != std::string::npos);
			b.missing = 0;
			Assert::IsTrue(t.Decide(b).reset, L"recovery");
			// not needed / off
			b.needed = false;
			d = t.Decide(b);
			Assert::IsTrue(d.off && d.result == DitherNeed::Result::NotNeeded);
			Assert::AreEqual(std::string("dither need: not needed (link step invisible)"), d.status);
			b.needed = true; b.enabled = false;
			d = t.Decide(b);
			Assert::IsTrue(d.off && d.result == DitherNeed::Result::Off);
			Assert::AreEqual(std::string("dither need: off"), d.status);
			t.Invalidate();
			b.enabled = true;
			Assert::IsTrue(t.Decide(b).reset);
		}
	};

	// ---- DitherNeed on the GPU (WARP unless VP_TEST_GPU=hardware): the block pass, the quantiser's use of the map -----------------
	//
	// Byte-identity of the quantiser with dither_need 0 (the owner's rule), by construction and by test: in `Temporal sigma-delta.glsl`
	// the only executed change at dither_need <= 0.5 is `float amp = noise_amplitude;` and its use in place of noise_amplitude in
	// the dither term and in resid_bound(a) (0.5 + a, the same float addition as before); the dn_extra() call and the VPDN fetches
	// sit inside `if (dither_need > 0.5)`. The tests compare a run with the dn_* parameters untouched with runs that set them (and
	// a bound texture) at dither_need 0, 16 refreshes at each of four levels, code for code.
	namespace
	{
		void DwCheck(bool ok, const std::string& what)
		{
			if (ok) return;
			Logger::WriteMessage((what + "\n").c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		pl_tex DwRgba32f(pl_gpu gpu, int w, int h, const float* data, bool storable = false)
		{
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | (storable ? PL_FMT_CAP_STORABLE : 0));
			pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, caps);
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

		std::vector<float> DwDownload(pl_gpu gpu, pl_tex tex)
		{
			std::vector<float> v(static_cast<size_t>(tex->params.w) * tex->params.h * 4);
			pl_tex_transfer_params d{};
			d.tex = tex;
			d.ptr = v.data();
			Assert::IsTrue(pl_tex_download(gpu, &d), L"the texture is not host-readable");
			return v;
		}

		std::string DwLf(std::string s)
		{
			size_t at = 0;
			while ((at = s.find("\r\n", at)) != std::string::npos) s.erase(at, 1);
			return s;
		}

		// ---- the quantiser with and without the map ----

		std::map<std::string, std::string> DwSigmaParams(bool full, const char* noise)
		{
			return { { "enabled", "1" }, { "levels_full", full ? "1" : "0" }, { "swapchain_bits", "10" }, { "color_bits", "10" },
				{ "noise_amplitude", noise } };
		}

		// One fresh fixture, chain and sigma-delta hook (the libplacebo log checked clean at the end); body gets both.
		void DwWithSigma(const std::map<std::string, std::string>& params,
			const std::function<void(TargetLutGpuFixture&, std::vector<const pl_hook*>&)>& body)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
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
			ConfiguredShaderRule sigma;
			sigma.name = "sigma_delta";
			sigma.filename = "Temporal sigma-delta.glsl";
			sigma.parameters = params;
			chain.Configure({ sigma });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 2;
			chain.Ensure(values);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected the sigma-delta (the dither need parameters or texture?)");
			body(fixture, hooks);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		std::vector<int> DwRenderGreys(TargetLutGpuFixture& fixture, std::vector<const pl_hook*>& hooks, const std::vector<float>& greys,
			int frames, bool full)
		{
			const pl_color_levels levels = full ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
			std::vector<int> codes;
			for (const float grey : greys)
			{
				const std::vector<int> c = fixture.RenderTenBitSequence(hooks, levels, grey, frames, nullptr, 10);
				codes.insert(codes.end(), c.begin(), c.end());
			}
			return codes;
		}

		// A fresh hook; `frames` refreshes at each grey (the hook's state carries across the greys, the same way in every run); a
		// bound map (rgba32f, e in .x) needs the fork's set_texture (the bound-map callers check DwHaveSetTexture first).
		std::vector<int> DwSigmaCodes(const std::map<std::string, std::string>& params, const std::vector<float>& greys, int frames,
			bool full, const std::vector<float>& mapData = std::vector<float>(), int mapW = 0, int mapH = 0)
		{
			std::vector<int> codes;
			DwWithSigma(params, [&](TargetLutGpuFixture& fixture, std::vector<const pl_hook*>& hooks)
			{
				pl_tex map = nullptr;
				if (!mapData.empty())
				{
#ifdef PL_VP_MPV_USER_SHADER_SET_TEXTURE
					map = DwRgba32f(fixture.Gpu(), mapW, mapH, mapData.data());
					Assert::IsTrue(pl_mpv_user_shader_set_texture(hooks[0], "VPDN", map), L"VPDN not accepted");
#else
					(void)mapW; (void)mapH;
					Assert::Fail(L"a bound map needs the libplacebo fork's pl_mpv_user_shader_set_texture");
#endif
				}
				codes = DwRenderGreys(fixture, hooks, greys, frames, full);
#ifdef PL_VP_MPV_USER_SHADER_SET_TEXTURE
				if (map)
				{
					pl_mpv_user_shader_set_texture(hooks[0], "VPDN", nullptr);
					pl_tex_destroy(fixture.Gpu(), &map);
				}
#endif
			});
			return codes;
		}

		// Greys that are exact in the rgba16f OUTPUT texture (m / 2048, m in [1024, 2047]): the quantiser's input is then
		// x = grey * 1023 exactly (full range, true 10-bit), with a fraction well away from a whole code.
		std::vector<float> DwGreys() { return { 1100.0f / 2048.0f, 1357.0f / 2048.0f, 1601.0f / 2048.0f, 1999.0f / 2048.0f }; }

		// The CPU model of `frames` refreshes of the quantiser at one grey: SigmaDeltaDomain::QuantiseStep with the shader's
		// dither (frame / 5 = firstFrame + j), amplitude `amp` (float, as the shader's), x moved by dx. True when every code matches.
		bool DwModelMatches(const int* got, int frames, float grey, int firstFrame, float amp, double dx)
		{
			const auto limits = SigmaDeltaDomain::Limits(10, true);
			const double x = static_cast<double>(grey * 1023.0f) + dx;
			double r = 0.0;
			for (int j = 0; j < frames; ++j)
			{
				const double d = static_cast<double>((SigmaDeltaDomain::Ign(0.0f, 0.0f, firstFrame + j) - 0.5f) * 2.0f * amp);
				if (static_cast<int>(SigmaDeltaDomain::QuantiseStep(x, r, d, limits, amp)) != got[j]) return false;
			}
			return true;
		}

		bool DwModelMatchesAnyDx(const int* got, int frames, float grey, int firstFrame, float amp)
		{
			for (const double dx : { 0.0, 1e-4, -1e-4, 3e-4, -3e-4 })
				if (DwModelMatches(got, frames, grey, firstFrame, amp, dx)) return true;
			return false;
		}

		std::wstring DwCodes(const std::vector<int>& c)
		{
			std::wstring s;
			for (const int v : c) s += std::to_wstring(v) + L" ";
			return s;
		}

		std::vector<float> DwConstantMap(int w, int h, float e)
		{
			std::vector<float> m(static_cast<size_t>(w) * h * 4, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) m[i * 4] = e;
			return m;
		}

		bool DwHaveSetTexture()
		{
#ifdef PL_VP_MPV_USER_SHADER_SET_TEXTURE
			return true;
#else
			return false;
#endif
		}

		// The GPU inputs of one golden frame (grain block texture, six precursors).
		struct DwFrameTex
		{
			pl_tex gs = nullptr, pre[RegionEvidence::kPrecursorTextures] = {};
			void Make(pl_gpu gpu, const DnFixture& fx)
			{
				const size_t n = fx.grid.Count();
				std::vector<float> g(n * 4, 0.0f);
				for (size_t i = 0; i < n; ++i) g[i * 4] = static_cast<float>(fx.grain.split.N[i]);
				gs = DwRgba32f(gpu, fx.grid.gw, fx.grid.gh, g.data());
				for (int t = 0; t < RegionEvidence::kPrecursorTextures; ++t)
				{
					std::vector<float> d(n * 4, 0.0f);
					for (size_t i = 0; i < n; ++i)
						for (int c = 0; c < 4; ++c)
							d[i * 4 + c] = static_cast<float>(fx.pre.data[i * RegionEvidence::kPrecursorChannels + t * 4 + c]);
					pre[t] = DwRgba32f(gpu, fx.grid.gw, fx.grid.gh, d.data());
				}
			}
			void Destroy(pl_gpu gpu)
			{
				pl_tex_destroy(gpu, &gs);
				for (pl_tex& t : pre) pl_tex_destroy(gpu, &t);
			}
		};

		// A pass argument set for frame `fx` with healthy inputs.
		DitherNeed::Gpu::Args DwArgs(const DwFrameTex& ft, const DnFixture& fx, pl_tex lut, pl_tex lutOut, const DitherNeed::FrameArgs& fa)
		{
			DitherNeed::Gpu::Args a;
			a.gsBlocks = ft.gs;
			for (int t = 0; t < RegionEvidence::kPrecursorTextures; ++t) a.precursors[t] = ft.pre[t];
			a.lut = lut; a.lutOut = lutOut;
			a.gridW = fx.grid.gw; a.gridH = fx.grid.gh; a.blockPx = fx.grid.B;
			a.derive = RegionEvidence::Uniforms(RegionEvidence::DeriveParams());
			a.params = fa.params;
			a.qOut = DitherNeed::QOut(fa.colorBits);
			a.alpha = DitherNeed::AlphaFor(fa.dtSeconds, fa.params.tauSeconds);
			return a;
		}

		std::vector<DitherNeed::BlockInT<float>> DwBlocksF(const DnFixture& fx)
		{
			std::vector<DitherNeed::BlockIn> b;
			Assert::AreEqual(0u, DitherNeed::Gather(fx.Inputs(), fx.grid, b));
			std::vector<DitherNeed::BlockInT<float>> bf(b.size());
			for (size_t i = 0; i < b.size(); ++i)
			{
				bf[i].N = static_cast<float>(b[i].N); bf[i].meanY = static_cast<float>(b[i].meanY);
				bf[i].banding = static_cast<float>(b[i].banding); bf[i].V = static_cast<float>(b[i].V);
				bf[i].Sout = static_cast<float>(b[i].Sout); bf[i].inPicture = b[i].inPicture; bf[i].overlay = b[i].overlay;
			}
			return bf;
		}

		double DwWorstDiff(pl_gpu gpu, pl_tex cur, const std::vector<float>& want)
		{
			const std::vector<float> got = DwDownload(gpu, cur);
			double worst = 0.0;
			for (size_t i = 0; i < want.size(); ++i) worst = (std::max)(worst, std::fabs(static_cast<double>(got[i * 4]) - want[i]));
			return worst;
		}
	}

	TEST_CLASS(DitherNeedWarpTests)
	{
	public:
		// The text of dn_extra in the DitherNeedGlsl.h string (run by the probe test below) is the text in the shader.
		TEST_METHOD(DitherNeedWarpAccessorTextIsInTheShader)
		{
			const std::string shader = DwLf(LoadBundledShader("Temporal sigma-delta.glsl"));
			Assert::IsFalse(shader.empty(), L"bundled shader not found");
			const std::string accessor = DwLf(DitherNeed::ExtraAccessorGlsl());
			Assert::IsTrue(shader.find(accessor) != std::string::npos, L"dn_extra in the shader differs from DitherNeedGlsl.h");
			Assert::IsTrue(shader.find(accessor, shader.find(accessor) + 1) == std::string::npos, L"dn_extra appears twice");
			for (const char* needed : { "//!PARAM dither_need\n", "//!PARAM dn_scale_x\n", "//!PARAM dn_scale_y\n", "//!PARAM dn_off_x\n",
				"//!PARAM dn_off_y\n", "//!PARAM dn_block\n", "//!TEXTURE VPDN\n//!SIZE 1 1\n//!FORMAT r8\n//!FILTER NEAREST\n//!BORDER CLAMP\n00\n",
				"//!BIND VPDN\n", "float resid_bound(float a)", "* 2.0 * amp);" })
				Assert::IsTrue(shader.find(needed) != std::string::npos, (std::wstring(L"the shader lacks: ") + DnWide(needed)).c_str());
			// The quantiser pass is the only pass that binds VPDN.
			size_t binds = 0;
			for (size_t at = shader.find("//!BIND VPDN"); at != std::string::npos; at = shader.find("//!BIND VPDN", at + 1)) ++binds;
			Assert::AreEqual(size_t{ 1 }, binds);
			Assert::IsTrue(shader.find("resid_bound(noise_amplitude)") == std::string::npos, L"resid_bound must take the pixel's amplitude");
		}

		// (a) The block pass against the float mirror (MapF) over the golden frames: a first frame, discontinuities (reset),
		// smoothing, hysteresis holds (the repeated frames), then an off frame (zeros) and the recovery (reset).
		BEGIN_TEST_METHOD_ATTRIBUTE(DitherNeedWarpBlockPassMatchesTheFloatMirror)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DitherNeedWarpBlockPassMatchesTheFloatMirror)
		{
			using namespace DitherNeedGolden;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);
			DnFixture fx[3];
			pl_tex gsTex[3] = {};
			pl_tex pre[3][RegionEvidence::kPrecursorTextures] = {};
			for (int f = 0; f < 3; ++f)
			{
				fx[f].Make(f);
				const size_t n = fx[f].grid.Count();
				std::vector<float> gs(n * 4, 0.0f);
				for (size_t i = 0; i < n; ++i) gs[i * 4] = static_cast<float>(fx[f].grain.split.N[i]);
				gsTex[f] = DwRgba32f(gpu, fx[f].grid.gw, fx[f].grid.gh, gs.data());
				for (int t = 0; t < RegionEvidence::kPrecursorTextures; ++t)
				{
					std::vector<float> d(n * 4, 0.0f);
					for (size_t i = 0; i < n; ++i)
						for (int c = 0; c < 4; ++c)
							d[i * 4 + c] = static_cast<float>(fx[f].pre.data[i * RegionEvidence::kPrecursorChannels + t * 4 + c]);
					pre[f][t] = DwRgba32f(gpu, fx[f].grid.gw, fx[f].grid.gh, d.data());
				}
			}
			const LightLevel::Lut lut = DnFixture::GoldenLut();
			const std::vector<float> lutTexels = LightLevel::PackTexels(lut), outTexels = LightLevel::PackOutputTexels(lut);
			pl_tex lutTex = DwRgba32f(gpu, LightLevel::kLutSize, 1, lutTexels.data());
			pl_tex lutOutTex = DwRgba32f(gpu, LightLevel::kLutSize, 1, outTexels.data());

			DitherNeed::Gpu::BlockPass pass;
			pass.SetHostReadable(true);
			DitherNeed::MapF mirror;
			double worst = 0.0, worstOther = 0.0;
			float raisedMax = 0.0f;
			bool afterOff = false;
			for (int s = 0; s < kSteps + 2; ++s)
			{
				const bool offStep = s == kSteps;
				const int frame = s < kSteps ? kStep[s].frame : 1;
				const bool disc = s < kSteps && kStep[s].discontinuity != 0;
				const double dt = s < kSteps ? kStep[s].dt : 1.0 / 60;
				const DnFixture& in = fx[frame];
				DitherNeed::FrameArgs fa = DnArgs(in.grid, dt, disc, "golden");
				fa.colorBits = kColorBits;
				fa.enabled = !offStep;
				fa.needed = true;
				std::vector<DitherNeed::BlockIn> b;
				Assert::AreEqual(0u, DitherNeed::Gather(in.Inputs(), in.grid, b));
				std::vector<DitherNeed::BlockInT<float>> bf(b.size());
				for (size_t i = 0; i < b.size(); ++i)
				{
					bf[i].N = static_cast<float>(b[i].N); bf[i].meanY = static_cast<float>(b[i].meanY);
					bf[i].banding = static_cast<float>(b[i].banding); bf[i].V = static_cast<float>(b[i].V);
					bf[i].Sout = static_cast<float>(b[i].Sout); bf[i].inPicture = b[i].inPicture; bf[i].overlay = b[i].overlay;
				}
				const DitherNeed::Result res = mirror.Update(fa, bf);
				Assert::IsTrue(res == (offStep ? DitherNeed::Result::Off : DitherNeed::Result::On));

				DitherNeed::Gpu::Args a;
				a.gsBlocks = gsTex[frame];
				for (int t = 0; t < RegionEvidence::kPrecursorTextures; ++t) a.precursors[t] = pre[frame][t];
				a.lut = lutTex; a.lutOut = lutOutTex;
				a.gridW = in.grid.gw; a.gridH = in.grid.gh; a.blockPx = in.grid.B;
				a.derive = RegionEvidence::Uniforms(RegionEvidence::DeriveParams());
				a.params = fa.params;
				a.qOut = DitherNeed::QOut(kColorBits);
				a.alpha = DitherNeed::AlphaFor(dt, fa.params.tauSeconds);
				// reset / off come from the shared decision (DitherNeed::StateTracker::Decide), not from this test
				const DitherNeed::Gpu::BlockPass::StepResult st = pass.Step(gpu, dispatch, a, fa);
				Assert::IsTrue(st.decision.result == res, L"the GPU decision differs from the CPU class");
				const bool expectReset = s == 0 || disc || afterOff;
				afterOff = offStep;
				if (offStep)
				{
					Assert::IsFalse(st.ran);
					Assert::IsTrue(pass.Current() == nullptr, L"a skipped frame must leave no product to bind");
					Assert::AreEqual(std::string("dither need: off"), st.decision.status);
					for (const float v : mirror.Extra()) Assert::AreEqual(0.0f, v);
					continue;
				}
				Assert::AreEqual(expectReset, st.decision.reset, L"reset decision");
				DwCheck(st.ran, "the block pass failed: " + pass.LastError());
				const std::vector<float> got = DwDownload(gpu, pass.Current());
				const std::vector<float>& want = mirror.Extra();
				Assert::AreEqual(want.size(), in.grid.Count());
				for (size_t i = 0; i < want.size(); ++i)
				{
					worst = (std::max)(worst, std::fabs(static_cast<double>(got[i * 4]) - want[i]));
					worstOther = (std::max)(worstOther, static_cast<double>(std::fabs(got[i * 4 + 1]) + std::fabs(got[i * 4 + 2]) + std::fabs(got[i * 4 + 3])));
					raisedMax = (std::max)(raisedMax, want[i]);
				}
			}
			char msg[160];
			std::snprintf(msg, sizeof(msg), "DITHER_NEED_BLOCK_PASS steps=%d worst=%.3e other_channels=%.1e max_extra=%.3f vram=%zu\n", kSteps + 2, worst,
				worstOther, raisedMax, pass.VramBytes());
			Logger::WriteMessage(msg);
			DwCheck(worst <= 1e-5, std::string("the block pass differs from the float mirror: ") + msg);
			DwCheck(worstOther == 0.0, "channels y, z, w of the state must be zero");
			DwCheck(raisedMax > 0.05f, "the golden frames must raise at least one block");
			Assert::AreEqual(static_cast<size_t>(2) * fx[0].grid.Count() * 16, pass.VramBytes());
			pass.Release(gpu);
			pl_tex_destroy(gpu, &lutTex);
			pl_tex_destroy(gpu, &lutOutTex);
			for (int f = 0; f < 3; ++f)
			{
				pl_tex_destroy(gpu, &gsTex[f]);
				for (int t = 0; t < RegionEvidence::kPrecursorTextures; ++t) pl_tex_destroy(gpu, &pre[f][t]);
			}
			pl_dispatch_destroy(&dispatch);
		}

		// (b) dither_need 0 (with the other dn_* set, and a map bound where the fork allows) is byte-identical to the dn_* untouched.
		// (c) dither_need 1 with the 1x1 placeholder, and with an all-zero map, is identical too.
		BEGIN_TEST_METHOD_ATTRIBUTE(DitherNeedWarpOffAndZeroMapAreByteIdentical)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DitherNeedWarpOffAndZeroMapAreByteIdentical)
		{
			const std::vector<float> greys = DwGreys();
			const int frames = 16;
			const std::vector<int> base = DwSigmaCodes(DwSigmaParams(true, "0.15"), greys, frames, true);
			Assert::AreEqual(greys.size() * frames, base.size());
			bool varies = false;
			for (int j = 0; j < frames; ++j) varies = varies || base[static_cast<size_t>(j)] != base[0];
			Assert::IsTrue(varies, L"the reference run must dither (noise_amplitude 0.15 at an off-grid level)");

			auto withDn = [](std::map<std::string, std::string> p, const char* need)
			{
				p["dither_need"] = need;
				p["dn_scale_x"] = "3.7"; p["dn_scale_y"] = "0.5"; p["dn_off_x"] = "11"; p["dn_off_y"] = "-4"; p["dn_block"] = "32";
				return p;
			};
			const std::vector<int> offSet = DwSigmaCodes(withDn(DwSigmaParams(true, "0.15"), "0"), greys, frames, true);
			Assert::IsTrue(offSet == base, (L"dither_need 0 with dn_* set differs: " + DwCodes(offSet) + L" vs " + DwCodes(base)).c_str());

			// (c) placeholder: dither_need 1, nothing bound (VPDN is the 1x1 zero r8 texture).
			const std::vector<int> placeholder = DwSigmaCodes(withDn(DwSigmaParams(true, "0.15"), "1"), greys, frames, true);
			Assert::IsTrue(placeholder == base, (L"dither_need 1 with the placeholder differs: " + DwCodes(placeholder) + L" vs " + DwCodes(base)).c_str());

		}

		// (b2) the bound-map cases of (b): an all-zero map, and a non-zero map with dither_need 0, are byte-identical too.
		BEGIN_TEST_METHOD_ATTRIBUTE(DitherNeedWarpBoundMapCasesAreByteIdentical)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
#ifndef PL_VP_MPV_USER_SHADER_SET_TEXTURE
			TEST_IGNORE()   // needs the beta host's libplacebo fork (pl_mpv_user_shader_set_texture)
#endif
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DitherNeedWarpBoundMapCasesAreByteIdentical)
		{
			const std::vector<float> greys = DwGreys();
			const int frames = 16;
			const std::vector<int> base = DwSigmaCodes(DwSigmaParams(true, "0.15"), greys, frames, true);
			auto withDn = [](std::map<std::string, std::string> p, const char* need)
			{
				p["dither_need"] = need;
				p["dn_scale_x"] = "3.7"; p["dn_scale_y"] = "0.5"; p["dn_off_x"] = "11"; p["dn_off_y"] = "-4"; p["dn_block"] = "32";
				return p;
			};
			const std::vector<int> zero = DwSigmaCodes(withDn(DwSigmaParams(true, "0.15"), "1"), greys, frames, true, DwConstantMap(4, 3, 0.0f), 4, 3);
			Assert::IsTrue(zero == base, (L"dither_need 1 with an all-zero map differs: " + DwCodes(zero) + L" vs " + DwCodes(base)).c_str());
			// dither_need 0 with a non-zero map bound: never read.
			const std::vector<int> unread = DwSigmaCodes(withDn(DwSigmaParams(true, "0.15"), "0"), greys, frames, true, DwConstantMap(4, 3, 0.3f), 4, 3);
			Assert::IsTrue(unread == base, (L"dither_need 0 with a bound map differs: " + DwCodes(unread) + L" vs " + DwCodes(base)).c_str());
		}

		// (d) A constant extra e: the codes equal the CPU quantiser model at amplitude aBase + e. The model's frame phase is found
		// from the plain run (the shader's frame / 5 counter has an unknown but fixed start), then used for every run.
		BEGIN_TEST_METHOD_ATTRIBUTE(DitherNeedWarpConstantExtraFollowsTheCpuQuantiserModel)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
#ifndef PL_VP_MPV_USER_SHADER_SET_TEXTURE
			TEST_IGNORE()   // needs the beta host's libplacebo fork (pl_mpv_user_shader_set_texture)
#endif
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DitherNeedWarpConstantExtraFollowsTheCpuQuantiserModel)
		{
			const std::vector<float> greys = DwGreys();
			const int frames = 16;
			const float aBase = 0.15f;
			const std::vector<int> base = DwSigmaCodes(DwSigmaParams(true, "0.15"), greys, frames, true);
			// The frame phase: the one offset at which the plain run is the model's at amplitude aBase.
			int phase = -1;
			for (int o = 0; o < 64 && phase < 0; ++o)
				if (DwModelMatchesAnyDx(base.data(), frames, greys[0], o, aBase)) phase = o;
			if (phase < 0)
			{
				std::string codes;
				for (int j = 0; j < frames; ++j) codes += std::to_string(base[static_cast<size_t>(j)]) + " ";
				DwCheck(false, "no frame phase makes the CPU model reproduce the plain run at grey 0 (codes " + codes + ")");
			}
			for (size_t g = 0; g < greys.size(); ++g)
				DwCheck(DwModelMatchesAnyDx(&base[g * frames], frames, greys[g], phase + static_cast<int>(g) * frames, aBase),
					"the plain run leaves the model at grey " + std::to_string(g));

			bool differs = false;
			for (const float e : { 0.1f, 0.25f, 0.35f })
			{
				std::map<std::string, std::string> p = DwSigmaParams(true, "0.15");
				p["dither_need"] = "1";
				p["dn_block"] = "16";
				const std::vector<int> got = DwSigmaCodes(p, greys, frames, true, DwConstantMap(4, 3, e), 4, 3);
				const float amp = (std::min)(aBase + (std::max)(e, 0.0f), 0.5f);
				for (size_t g = 0; g < greys.size(); ++g)
				{
					const int* c = &got[g * frames];
					DwCheck(DwModelMatchesAnyDx(c, frames, greys[g], phase + static_cast<int>(g) * frames, amp),
						"extra " + std::to_string(e) + ": the codes at grey " + std::to_string(g) + " leave the CPU model at amplitude " +
						std::to_string(amp));
					for (int j = 0; j < frames; ++j) differs = differs || c[j] != base[g * frames + j];
				}
			}
			DwCheck(differs, "a raised amplitude must change at least one code (the test would be vacuous)");
		}

		// (e) Total amplitude 0.5 (aBase 0.15 + e 0.35): codes that are exact without any dither stay exact: black, white, and
		// the footroom / headroom levels when libplacebo hands them through.
		BEGIN_TEST_METHOD_ATTRIBUTE(DitherNeedWarpExactCodesStayExactAtTheAmplitudeCap)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
#ifndef PL_VP_MPV_USER_SHADER_SET_TEXTURE
			TEST_IGNORE()   // needs the beta host's libplacebo fork (pl_mpv_user_shader_set_texture)
#endif
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DitherNeedWarpExactCodesStayExactAtTheAmplitudeCap)
		{
			const std::vector<float> levels = { 0.0f, 1.0f, 1.05f, -0.04f };   // black, white, WTW, BTB (limited range)
			const int frames = 16;
			const std::vector<int> exact = DwSigmaCodes(DwSigmaParams(false, "0"), levels, frames, false);
			std::map<std::string, std::string> p = DwSigmaParams(false, "0.15");
			p["dither_need"] = "1";
			p["dn_block"] = "16";
			const std::vector<int> capped = DwSigmaCodes(p, levels, frames, false, DwConstantMap(4, 3, 0.35f), 4, 3);
			int asserted = 0;
			for (size_t g = 0; g < levels.size(); ++g)
			{
				bool constant = true;
				for (int j = 1; j < frames; ++j) constant = constant && exact[g * frames + j] == exact[g * frames];
				if (!constant)
				{
					Logger::WriteMessage(("level " + std::to_string(levels[g]) + " is not constant without dither; not asserted\n").c_str());
					continue;
				}
				++asserted;
				for (int j = 0; j < frames; ++j)
					Assert::AreEqual(exact[g * frames], capped[g * frames + j],
						(L"an exact code moved at total amplitude 0.5, level index " + std::to_wstring(g) + L" refresh " + std::to_wstring(j)).c_str());
			}
			Assert::IsTrue(asserted >= 2, L"black and white must be exact without dither");
		}

		// (f) dn_extra against DitherNeed::SampleExtra: the text of DitherNeedGlsl.h in a probe compute pass (a test asserts it is the
		// shader's text), VPDN a random map, positions inside and outside the grid, three placements.
		BEGIN_TEST_METHOD_ATTRIBUTE(DitherNeedWarpAccessorMatchesSampleExtra)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DitherNeedWarpAccessorMatchesSampleExtra)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);
			const int gw = 12, gh = 8;
			std::vector<float> map(static_cast<size_t>(gw) * gh), mapTexels(map.size() * 4, 0.0f);
			for (size_t i = 0; i < map.size(); ++i)
			{
				map[i] = static_cast<float>((i * 37 + 11) % 100) / 100.0f * 0.35f;
				mapTexels[i * 4] = map[i];
			}
			std::vector<int> px, py;
			for (const int y : { 0, 1, 3, 9, 20, 31, 40, 63, 70, 79, 100 })
				for (const int x : { 0, 1, 7, 16, 33, 50, 64, 95, 100, 127, 200 })
				{
					px.push_back(x);
					py.push_back(y);
				}
			const int n = static_cast<int>(px.size());
			std::vector<float> posTexels(static_cast<size_t>(n) * 4, 0.0f);
			for (int i = 0; i < n; ++i)
			{
				posTexels[static_cast<size_t>(i) * 4] = static_cast<float>(px[static_cast<size_t>(i)]);
				posTexels[static_cast<size_t>(i) * 4 + 1] = static_cast<float>(py[static_cast<size_t>(i)]);
			}
			pl_tex mapTex = DwRgba32f(gpu, gw, gh, mapTexels.data());
			pl_tex posTex = DwRgba32f(gpu, n, 1, posTexels.data());
			pl_tex outTex = DwRgba32f(gpu, n, 1, nullptr, true);
			struct Cfg { float sx, sy, ox, oy; int block; };
			const std::string header = "#define DN_N " + std::to_string(n) + "\n" + DitherNeed::ExtraAccessorGlsl();
			const char* const body = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.y == 0 && g.x < DN_N) {
    vec4 p = texelFetch(dn_pos, ivec2(g.x, 0), 0);
    imageStore(dn_out, ivec2(g.x, 0), vec4(dn_extra(p.xy), 0.0, 0.0, 0.0));
}
)GLSL";
			double worst = 0.0;
			for (const Cfg c : { Cfg{ 0.75f, 0.75f, 3.0f, 1.5f, 8 }, Cfg{ 1.0f, 1.0f, 0.0f, 0.0f, 8 }, Cfg{ 2.5f, 1.25f, -3.5f, 7.25f, 16 } })
			{
				const float blockF = static_cast<float>(c.block);
				const pl_shader_var vars[5] = { { pl_var_float("dn_scale_x"), &c.sx, true }, { pl_var_float("dn_scale_y"), &c.sy, true },
					{ pl_var_float("dn_off_x"), &c.ox, true }, { pl_var_float("dn_off_y"), &c.oy, true },
					{ pl_var_float("dn_block"), &blockF, true } };
				const pl_shader_desc desc[3] = { VpvGpu::Sampled("VPDN", mapTex), VpvGpu::Sampled("dn_pos", posTex), VpvGpu::Storage("dn_out", outTex) };
				DwCheck(VpvGpu::Pass(dispatch, "VideoProcessor dither need: accessor probe", header, body, desc, 3, vars, 5, (n + 15) / 16, 1, 0),
					"the accessor probe pass failed");
				const std::vector<float> got = DwDownload(gpu, outTex);
				for (int i = 0; i < n; ++i)
				{
					const float want = DitherNeed::SampleExtra(map.data(), gw, gh, c.block, c.sx, c.sy, c.ox, c.oy,
						px[static_cast<size_t>(i)], py[static_cast<size_t>(i)]);
					worst = (std::max)(worst, std::fabs(static_cast<double>(got[static_cast<size_t>(i) * 4]) - want));
				}
			}
			char msg[120];
			std::snprintf(msg, sizeof(msg), "DITHER_NEED_ACCESSOR positions=%d worst=%.3e\n", n, worst);
			Logger::WriteMessage(msg);
			DwCheck(worst <= 1e-6, std::string("dn_extra differs from DitherNeed::SampleExtra: ") + msg);
			pl_tex_destroy(gpu, &mapTex);
			pl_tex_destroy(gpu, &posTex);
			pl_tex_destroy(gpu, &outTex);
			pl_dispatch_destroy(&dispatch);
		}
		// Resets caused by colour depth, aBase, the LUT fingerprint and the grid are taken by the shared decision (not hand-coded):
		// the GPU block pass equals the float mirror at every step and the decision says reset exactly there.
		BEGIN_TEST_METHOD_ATTRIBUTE(DitherNeedWarpResetsFollowTheSharedDecision)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DitherNeedWarpResetsFollowTheSharedDecision)
		{
			using namespace DitherNeedGolden;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);
			DnFixture fx; fx.Make(1);
			DwFrameTex ft; ft.Make(gpu, fx);
			const LightLevel::Lut lut = DnFixture::GoldenLut();
			const std::vector<float> lt = LightLevel::PackTexels(lut), ot = LightLevel::PackOutputTexels(lut);
			pl_tex lutTex = DwRgba32f(gpu, LightLevel::kLutSize, 1, lt.data());
			pl_tex lutOutTex = DwRgba32f(gpu, LightLevel::kLutSize, 1, ot.data());
			const std::vector<DitherNeed::BlockInT<float>> bf = DwBlocksF(fx);
			DitherNeed::Gpu::BlockPass pass;
			pass.SetHostReadable(true);
			DitherNeed::MapF mirror;
			struct Case { const char* what; bool expectReset; int colorBits; double aBase; const char* fp; int gridB; double dt; };
			// the same frame each step; only the named input changes. dt differs between held steps so a missed reset would show.
			const Case cases[] = {
				{ "first frame", true, 8, 0.15, "fpA", fx.grid.B, 1.0 / 60 },
				{ "same inputs: held", false, 8, 0.15, "fpA", fx.grid.B, 1.0 / 60 },
				{ "colour depth change", true, 10, 0.15, "fpA", fx.grid.B, 1.0 / 60 },
				{ "same again", false, 10, 0.15, "fpA", fx.grid.B, 1.0 / 30 },
				{ "aBase change", true, 10, 0.10, "fpA", fx.grid.B, 1.0 / 60 },
				{ "same again", false, 10, 0.10, "fpA", fx.grid.B, 1.0 / 30 },
				{ "LUT fingerprint change", true, 10, 0.10, "fpB", fx.grid.B, 1.0 / 60 },
				{ "same again", false, 10, 0.10, "fpB", fx.grid.B, 1.0 / 30 },
				{ "grid change", true, 10, 0.10, "fpB", fx.grid.B * 2, 1.0 / 60 } };
			double worst = 0.0;
			for (const Case& c : cases)
			{
				DitherNeed::FrameArgs fa = DnArgs(fx.grid, c.dt, false, c.fp);
				fa.colorBits = c.colorBits; fa.params.aBase = c.aBase; fa.grid.B = c.gridB; fa.needed = true;
				DitherNeed::Gpu::Args a = DwArgs(ft, fx, lutTex, lutOutTex, fa);
				a.blockPx = fa.grid.B;
				Assert::IsTrue(mirror.Update(fa, bf) == DitherNeed::Result::On);
				const DitherNeed::Gpu::BlockPass::StepResult st = pass.Step(gpu, dispatch, a, fa);
				DwCheck(st.ran, std::string(c.what) + ": " + pass.LastError());
				Assert::AreEqual(c.expectReset, st.decision.reset, (L"reset decision: " + DnWide(c.what)).c_str());
				worst = (std::max)(worst, DwWorstDiff(gpu, pass.Current(), mirror.Extra()));
			}
			char msg[100];
			std::snprintf(msg, sizeof(msg), "DITHER_NEED_RESETS steps=%d worst=%.3e\n", static_cast<int>(sizeof(cases) / sizeof(cases[0])), worst);
			Logger::WriteMessage(msg);
			DwCheck(worst <= 1e-5, std::string("the block pass leaves the float mirror after a reset cause: ") + msg);
			pass.Release(gpu);
			ft.Destroy(gpu);
			pl_tex_destroy(gpu, &lutTex);
			pl_tex_destroy(gpu, &lutOutTex);
			pl_dispatch_destroy(&dispatch);
		}

		// Each GPU input absent or mismatched: the frame is skipped (no pass, no product), the status names the input, and the
		// next healthy frame recovers with a reset (== a fresh mirror).
		BEGIN_TEST_METHOD_ATTRIBUTE(DitherNeedWarpEachMissingGpuInputSkipsAndRecovers)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DitherNeedWarpEachMissingGpuInputSkipsAndRecovers)
		{
			using namespace DitherNeedGolden;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			Assert::IsNotNull(dispatch);
			DnFixture fx; fx.Make(0);
			DwFrameTex ft; ft.Make(gpu, fx);
			const LightLevel::Lut lut = DnFixture::GoldenLut();
			const std::vector<float> lt = LightLevel::PackTexels(lut), ot = LightLevel::PackOutputTexels(lut);
			pl_tex lutTex = DwRgba32f(gpu, LightLevel::kLutSize, 1, lt.data());
			pl_tex lutOutTex = DwRgba32f(gpu, LightLevel::kLutSize, 1, ot.data());
			const std::vector<float> smallData(2 * 2 * 4, 0.0f), shortData(512 * 4, 0.0f);
			pl_tex wrongGrid = DwRgba32f(gpu, 2, 2, smallData.data());
			pl_tex shortLut = DwRgba32f(gpu, 512, 1, shortData.data());
			const std::vector<DitherNeed::BlockInT<float>> bf = DwBlocksF(fx);
			struct Case { const char* what; unsigned want; };
			const Case cases[] = { { "no grain texture", DitherNeed::kNoGrain }, { "grain texture on another grid", DitherNeed::kGrainGridMismatch },
				{ "a precursor texture missing", DitherNeed::kNoPrecursors }, { "a precursor on another grid", DitherNeed::kEvidenceGridMismatch },
				{ "no light-level table", DitherNeed::kNoLut }, { "light-level table of the wrong width", DitherNeed::kLutSizeMismatch } };
			for (const Case& c : cases)
			{
				DitherNeed::Gpu::BlockPass pass;
				pass.SetHostReadable(true);
				DitherNeed::FrameArgs fa = DnArgs(fx.grid, 1.0 / 60, false, "fp");
				fa.needed = true;
				DitherNeed::Gpu::Args good = DwArgs(ft, fx, lutTex, lutOutTex, fa);
				DitherNeed::Gpu::BlockPass::StepResult st = pass.Step(gpu, dispatch, good, fa);
				DwCheck(st.ran && pass.Current() != nullptr, std::string(c.what) + ": the healthy first frame must run");
				DitherNeed::Gpu::Args bad = good;
				switch (c.want)
				{
				case DitherNeed::kNoGrain: bad.gsBlocks = nullptr; break;
				case DitherNeed::kGrainGridMismatch: bad.gsBlocks = wrongGrid; break;
				case DitherNeed::kNoPrecursors: bad.precursors[3] = nullptr; break;
				case DitherNeed::kEvidenceGridMismatch: bad.precursors[2] = wrongGrid; break;
				case DitherNeed::kNoLut: bad.lutOut = nullptr; break;
				default: bad.lut = shortLut; break;
				}
				st = pass.Step(gpu, dispatch, bad, fa);
				Assert::IsFalse(st.ran, (L"skipped: " + DnWide(c.what)).c_str());
				Assert::IsTrue((st.missing & c.want) == c.want, (L"missing bit: " + DnWide(c.what)).c_str());
				Assert::IsTrue(st.decision.result == DitherNeed::Result::Unavailable);
				Assert::IsTrue(st.decision.status.find(DitherNeed::MissingText(c.want)) != std::string::npos, L"status names the input");
				Assert::IsTrue(pass.Current() == nullptr, L"a skipped frame leaves no product to bind");
				// recovery: a reset, equal to a fresh mirror
				DitherNeed::MapF fresh;
				Assert::IsTrue(fresh.Update(fa, bf) == DitherNeed::Result::On);
				st = pass.Step(gpu, dispatch, good, fa);
				DwCheck(st.ran, std::string(c.what) + ": recovery " + pass.LastError());
				Assert::IsTrue(st.decision.reset, L"recovery resets");
				DwCheck(DwWorstDiff(gpu, pass.Current(), fresh.Extra()) <= 1e-5, std::string(c.what) + ": the recovered map differs from a fresh reset");
				pass.Release(gpu);
			}
			ft.Destroy(gpu);
			pl_tex_destroy(gpu, &lutTex);
			pl_tex_destroy(gpu, &lutOutTex);
			pl_tex_destroy(gpu, &wrongGrid);
			pl_tex_destroy(gpu, &shortLut);
			pl_dispatch_destroy(&dispatch);
		}

		// HookGuard: a skipped frame (no product) binds nothing and renders the off path; with the fork call the guard binds the map
		// and Release() / the destructor restore the hook byte for byte. Without the fork call (standard 7.360 build) Apply returns
		// false, reports kNoSetTexture, sets nothing, and that leg is asserted in full (the bound leg runs where the fork call exists).
		BEGIN_TEST_METHOD_ATTRIBUTE(DitherNeedWarpHookGuardAppliesAndRestores)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DitherNeedWarpHookGuardAppliesAndRestores)
		{
			const std::vector<float> greys = DwGreys();
			const int frames = 16;
			const std::map<std::string, std::string> params = DwSigmaParams(true, "0.15");
			const std::vector<int> base = DwSigmaCodes(params, greys, frames, true);
			DitherNeed::Gpu::Placement place;
			place.blockPx = 16;

			// skipped frame: no product -> Apply binds nothing
			bool nullApplied = true;
			std::vector<int> skipped;
			DwWithSigma(params, [&](TargetLutGpuFixture& fixture, std::vector<const pl_hook*>& hooks)
			{
				DitherNeed::Gpu::HookGuard guard;
				nullApplied = guard.Apply(hooks.data(), static_cast<int>(hooks.size()), nullptr, place);
				skipped = DwRenderGreys(fixture, hooks, greys, frames, true);
			});
			Assert::IsFalse(nullApplied, L"a null product binds nothing");
			Assert::IsTrue(skipped == base, (L"a skipped frame must render the off path: " + DwCodes(skipped) + L" vs " + DwCodes(base)).c_str());

			// a real product: applied (rendered with the guard alive) / released before the render / destroyed before the render
			std::vector<int> bound, released, destroyed;
			bool applied = false;
			unsigned missingBit = 0;
			DwWithSigma(params, [&](TargetLutGpuFixture& fixture, std::vector<const pl_hook*>& hooks)
			{
				const std::vector<float> data = DwConstantMap(4, 3, 0.3f);
				pl_tex map = DwRgba32f(fixture.Gpu(), 4, 3, data.data());
				DitherNeed::Gpu::HookGuard guard;
				applied = guard.Apply(hooks.data(), static_cast<int>(hooks.size()), map, place);
				if (!applied) missingBit = DitherNeed::Gpu::HookGuard::MissingAfterApply();
				bound = DwRenderGreys(fixture, hooks, greys, frames, true);
				guard.Release();
				pl_tex_destroy(fixture.Gpu(), &map);
			});
			DwWithSigma(params, [&](TargetLutGpuFixture& fixture, std::vector<const pl_hook*>& hooks)
			{
				const std::vector<float> data = DwConstantMap(4, 3, 0.3f);
				pl_tex map = DwRgba32f(fixture.Gpu(), 4, 3, data.data());
				DitherNeed::Gpu::HookGuard guard;
				guard.Apply(hooks.data(), static_cast<int>(hooks.size()), map, place);
				guard.Release();
				released = DwRenderGreys(fixture, hooks, greys, frames, true);
				pl_tex_destroy(fixture.Gpu(), &map);
			});
			DwWithSigma(params, [&](TargetLutGpuFixture& fixture, std::vector<const pl_hook*>& hooks)
			{
				const std::vector<float> data = DwConstantMap(4, 3, 0.3f);
				pl_tex map = DwRgba32f(fixture.Gpu(), 4, 3, data.data());
				{
					DitherNeed::Gpu::HookGuard guard;
					guard.Apply(hooks.data(), static_cast<int>(hooks.size()), map, place);
				}   // destructor
				destroyed = DwRenderGreys(fixture, hooks, greys, frames, true);
				pl_tex_destroy(fixture.Gpu(), &map);
			});
			Assert::IsTrue(released == base, (L"after Release() the hook must render the off path: " + DwCodes(released)).c_str());
			Assert::IsTrue(destroyed == base, (L"after the destructor the hook must render the off path: " + DwCodes(destroyed)).c_str());
			if (!DwHaveSetTexture())
			{
				Assert::IsFalse(applied, L"without set_texture Apply must return false");
				Assert::AreEqual(static_cast<unsigned>(DitherNeed::kNoSetTexture), missingBit);
				Assert::IsTrue(bound == base, L"without set_texture nothing is bound: the off path");
				return;   // the unsupported leg is fully asserted above; the bound leg runs on the beta host
			}
			Assert::IsTrue(applied, L"Apply must bind the product");
			Assert::IsTrue(bound != base, L"a bound non-zero map must change the codes (the test would be vacuous)");
		}
	};
}
