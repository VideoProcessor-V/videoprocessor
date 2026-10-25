// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

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
	};
}
