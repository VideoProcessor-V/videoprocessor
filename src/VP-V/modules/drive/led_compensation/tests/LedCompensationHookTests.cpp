// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(LedCompensationHookTests)
	{
	public:


		TEST_METHOD(LedBoostToeAndShadowLift)
		{
			// Delta 11. The boost toe: knee 0 is today's boost; knee 0.10 is the
			// identity below the knee, today's boost above knee + width,
			// monotonic; below black untouched; the gain path unchanged. The
			// ANSI shadow lift: none without a veil; with veil 1 and strength
			// 0.3 the darks rise by 0.3 x 0.1 x 1.6875 x u(1 - u)^2.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			CustomHookChain::Chain chain(SigmaLoader(fixture));
			ConfiguredShaderRule rule;
			rule.name = "led_compensation";
			rule.filename = "LED compensation.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 2;
			chain.Ensure(values);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected LED compensation.glsl");
			const pl_hook* hook = hooks[0];
			const double boost = 2.2 / 2.4;
			auto set = [&](double gain, double knee, double lift, double veil)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost", boost));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain", gain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", 1.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", 0.08));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "compensation_eotf", 2.2));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost_knee", knee));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost_knee_width", 0.05));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "shadow_lift", lift));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "veil", veil));
			};
			// 7.360 drops the output encoding after a PRE_OUTPUT hook; the
			// renderer then puts VP's repair first, as here.
			OutputEncodingRepair::Hook repair;
			auto code = [&](double v, enum pl_color_levels levels = PL_COLOR_LEVELS_FULL)
			{
				pl_color_repr target{};
				target.sys = PL_COLOR_SYSTEM_RGB;
				target.levels = levels;
				target.bits.sample_depth = 10;
				target.bits.color_depth = 10;
				repair.SetTarget(target);
				std::vector<const pl_hook*> list = { hook };
				if (OutputEncodingRepair::Needed(list))
					list.insert(list.begin(), repair.Get());
				return fixture.RenderTenBitSequence(list, levels, static_cast<float>(v), 1, nullptr, 10)[0];
			};
			std::wstring problems;
			// Knee 0 = today's global boost.
			set(1.0, 0.0, 0.0, 0.0);
			for (double v : { 0.03, 0.12, 0.5, 0.9 })
				if (std::abs(code(v) - std::lround(1023.0 * std::pow(v, boost))) > 1)
					problems += L"knee 0 at " + std::to_wstring(v) + L"; ";
			// Knee 0.10: identity below, the boost above 0.15, monotonic.
			std::vector<int> toe;
			set(1.0, 0.10, 0.0, 0.0);
			for (int i = 0; i <= 200; ++i)
				toe.push_back(code(i * 0.005));
			for (int i = 0; i <= 200; ++i)
			{
				const double v = i * 0.005;
				if (v < 0.0999 && std::abs(toe[i] - std::lround(1023.0 * v)) > 1)
					problems += L"toe not identity at " + std::to_wstring(v) + L"; ";
				if (v > 0.1501 && std::abs(toe[i] - std::lround(1023.0 * std::pow(v, boost))) > 1)
					problems += L"toe not the boost at " + std::to_wstring(v) + L"; ";
				if (i > 0 && toe[i] < toe[i - 1])
					problems += L"toe not monotonic at " + std::to_wstring(v) + L"; ";
			}
			// Below black untouched (limited range code 40).
			const int btb = code((40.0 - 64.0) / 876.0, PL_COLOR_LEVELS_LIMITED);
			if (btb != 40)
				problems += L"code 40 became " + std::to_wstring(btb) + L"; ";
			// The gain path unchanged with the toe on (above the toe).
			set(2.0, 0.0, 0.0, 0.0);
			const int gainPlain = code(0.4);
			set(2.0, 0.10, 0.0, 0.0);
			if (code(0.4) != gainPlain)
				problems += L"the gain changed with the toe on; ";
			// Shadow lift.
			set(1.0, 0.10, 0.3, 0.0);
			if (code(0.0333) != toe[7] && std::abs(code(0.0333) - std::lround(1023.0 * 0.0333)) > 1)
				problems += L"a lift without a veil; ";
			set(1.0, 0.10, 0.3, 1.0);
			const double u = 0.0333 / 0.10;
			const double expectedLift = 0.3 * 0.10 * 1.6875 * u * (1.0 - u) * (1.0 - u);
			const int lifted = code(0.0333);
			if (std::abs(lifted - 1023.0 * (0.0333 + expectedLift)) > 1.5)
				problems += L"lift at 0.0333: " + std::to_wstring(lifted) + L" for " + std::to_wstring(1023.0 * (0.0333 + expectedLift)) + L"; ";
			if (code(0.0) != 0 || std::abs(code(0.5) - std::lround(1023.0 * std::pow(0.5, boost))) > 1)
				problems += L"the lift reached black or the mid-tones; ";
			fixture.CaptureLog(nullptr);
			Assert::IsTrue(problems.empty(), (problems + L"log: " + std::wstring(log.begin(), log.end())).c_str());
		}

		TEST_METHOD(LedRawTransferMatchesTheRenderedOutput)
		{
			// The lookahead's CPU transfer against libplacebo rendering the same
			// grey levels with the same colour spaces and colour mapping.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			pl_render_params params = pl_render_default_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			params.deband_params = nullptr;
			params.peak_detect_params = nullptr;
			struct Case { pl_color_transfer source; float sourceNits; pl_color_transfer target; float targetNits; };
			const Case cases[] = {
				{ PL_COLOR_TRC_BT_1886, 0.0f, PL_COLOR_TRC_GAMMA22, 0.0f },
				{ PL_COLOR_TRC_BT_1886, 0.0f, PL_COLOR_TRC_GAMMA24, 0.0f },
				{ PL_COLOR_TRC_PQ, 1000.0f, PL_COLOR_TRC_GAMMA22, 100.0f },
				{ PL_COLOR_TRC_PQ, 4000.0f, PL_COLOR_TRC_GAMMA24, 48.0f } };
			std::wstring failures;
			for (const Case& c : cases)
			{
				pl_color_space source{}, target{};
				source.primaries = target.primaries = PL_COLOR_PRIM_BT_709;
				source.transfer = c.source;
				target.transfer = c.target;
				source.hdr.max_luma = source.hdr.max_cll = source.hdr.max_fall = c.sourceNits;
				if (c.sourceNits > 0.0f) source.hdr.min_luma = PL_COLOR_HDR_BLACK;
				target.hdr.max_luma = c.targetNits;
				if (c.targetNits > 0.0f) target.hdr.min_luma = PL_COLOR_HDR_BLACK;
				const LedDrive::RawTransfer transfer =
					LedDrive::BuildRawTransfer(source, target, params.color_map_params);
				double worst = 0.0;
				int worstLevel = 0;
				for (int level = 16; level <= 240; level += 16)
				{
					const uint8_t v = static_cast<uint8_t>(level);
					const RgbaPixel out = fixture.Render(nullptr, params, PL_COLOR_LEVELS_FULL,
						PL_LUT_NORMALIZED, { v, v, v, 255 }, c.source, c.target,
						PL_COLOR_PRIM_BT_709, PL_COLOR_PRIM_BT_709, c.sourceNits, c.targetNits);
					const double predicted = transfer[std::lround(level / 255.0 * (LedDrive::RAW_TRANSFER_SIZE - 1))];
					const double difference = std::abs(out.r / 255.0 - predicted);
					if (difference > worst) { worst = difference; worstLevel = level; }
				}
				const std::wstring evidence = L"transfer " + std::to_wstring(static_cast<int>(c.source)) +
					L"->" + std::to_wstring(static_cast<int>(c.target)) + L" worst " +
					std::to_wstring(worst * 255.0) + L" codes at " + std::to_wstring(worstLevel);
				Logger::WriteMessage(evidence.c_str());
				if (worst > 3.0 / 255.0) failures += evidence + L"; ";
			}
			Assert::IsTrue(failures.empty(), failures.c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog);
		}

		TEST_METHOD(LedCompensationShaderAppliesGainWithASoftKnee)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
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
			rule.name = "led_compensation";
			rule.filename = "LED compensation.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 64;
			chain.Ensure(values);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected LED compensation.glsl");
			Assert::IsTrue((hooks[0]->stages & PL_HOOK_PRE_OUTPUT) != 0);
			const pl_hook* hook = hooks[0];
			auto set = [hook](double gain, double domain, double boost = 1.0)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost", boost));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain", gain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", domain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", 0.08));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "compensation_eotf", 2.2));
			};

			set(1.0, 0.0);
			const auto unity = fixture.RenderCoordinateField(hook);
			for (size_t i = 0; i < unity.size(); ++i)
				Assert::AreEqual(static_cast<int>(baseline[i].r), static_cast<int>(unity[i].r));

			const size_t quarter = 16;           // r = 16/63 of full scale
			const size_t bright = 62;            // r close to white
			set(1.25, 0.0);
			const auto encoded = fixture.RenderCoordinateField(hook);
			Assert::AreEqual(baseline[quarter].r * 1.25, static_cast<double>(encoded[quarter].r), 1.5);
			Assert::IsTrue(encoded[bright].r >= baseline[bright].r && encoded[bright].r <= 255);
			for (size_t x = 1; x < 64; ++x)
				Assert::IsTrue(encoded[x].r >= encoded[x - 1].r, L"The knee keeps the ramp monotonic");

			set(2.0, 1.0);
			const auto linear = fixture.RenderCoordinateField(hook);
			const double expected = 255.0 * std::pow(2.0 * std::pow(baseline[quarter].r / 255.0, 2.2), 1.0 / 2.2);
			Assert::AreEqual(expected, static_cast<double>(linear[quarter].r), 2.0);

			// Gamma boost alone (gain 1): the old output_gamma 2.4 on film 2.2 lift.
			set(1.0, 0.0, 2.2 / 2.4);
			const auto boosted = fixture.RenderCoordinateField(hook);
			for (size_t x : { size_t{ 8 }, quarter, size_t{ 32 }, size_t{ 48 } })
				Assert::AreEqual(255.0 * std::pow(baseline[x].r / 255.0, 2.2 / 2.4),
					static_cast<double>(boosted[x].r), 1.5);
			Assert::AreEqual(static_cast<int>(baseline[63].r), static_cast<int>(boosted[63].r),
				L"White stays white");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

		// Stage 7 step 7a on the real shader and a 10-bit limited target set up
		// like VP's swapchain (colour depth 8, bit shift 2), with the
		// PRE_OUTPUT encoding repair as in VP:
		// - sign fix (BTB/WTW answers item 5): a below-black grey (code 40)
		//   keeps its below-black value through the boost and a gain of 1.3;
		// - delta 4: a mid grey (8-bit 126) at LED 100 with the compensation
		//   (gain 2.55 in linear light, E = 2.41) gives the same light as at
		//   LED 255 with no gain, within 1%, with the boost on and off.
		TEST_METHOD(LedCompensationKeepsBelowBlackAndLightExact)
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
			ConfiguredShaderRule rule;
			rule.name = "led_compensation";
			rule.filename = "LED compensation.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 2;
			chain.Ensure(values);
			std::vector<const pl_hook*> compensation;
			chain.Collect(false, compensation);
			Assert::AreEqual(size_t{ 1 }, compensation.size(), L"libplacebo rejected LED compensation.glsl");
			const pl_hook* hook = compensation[0];
			OutputEncodingRepair::Hook repair;
			pl_color_repr limited{};
			limited.sys = PL_COLOR_SYSTEM_RGB;
			limited.levels = PL_COLOR_LEVELS_LIMITED;
			limited.bits.sample_depth = 10;
			limited.bits.color_depth = 8;
			limited.bits.bit_shift = 2;
			repair.SetTarget(limited);
			const std::vector<const pl_hook*> hooks = { repair.Get(), hook };
			auto set = [hook](double gain, double boost)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost", boost));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain", gain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", 1.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", 0.08));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "compensation_eotf", 2.41));
			};
			// 10-bit limited code k <-> signal (k - 64) / 876 before the encode.
			auto signal = [](double code) { return (code - 64.0) / 876.0; };
			auto code = [&fixture, &hooks](double value)
			{
				return fixture.RenderTenBitSequence(hooks, PL_COLOR_LEVELS_LIMITED, static_cast<float>(value), 1)[0];
			};
			auto spow = [](double v, double e) { return (v < 0.0 ? -1.0 : 1.0) * std::pow(std::abs(v), e); };
			const double boost = 2.2 / 2.4;

			// Bug hunt 2 2026-10-17, led_btb_passthrough (default on): below
			// black leaves the hook exactly as captured - the same code as with
			// the hook idle (gain 1, no boost) - under the boost and gain, in
			// both gain domains, and a deep code (8) is no longer pushed to the
			// encode floor by a large gain.
			const double btb = signal(40.0);
			set(1.0, 1.0);
			const int idle40 = code(btb), idle8 = code(signal(8.0)), idleBlack = code(signal(64.0)),
				idle80 = code(signal(80.0));
			Assert::IsTrue(idle40 < 64 && idle8 < idle40, L"the idle hook does not keep below black");
			set(1.3, boost);
			Assert::AreEqual(idle40, code(btb), L"Below black did not pass through (linear gain 1.3, boost)");
			set(2.5, boost);
			Assert::AreEqual(idle8, code(signal(8.0)), L"Deep below black did not pass through (linear gain 2.5)");
			// Continuous and monotonic at black: black stays black, above black
			// rises with the gain.
			Assert::AreEqual(idleBlack, code(signal(64.0)), L"Black moved");
			Assert::IsTrue(code(signal(80.0)) > idle80, L"Above black was not lifted by the gain");
			Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", 0.0));
			Assert::AreEqual(idle40, code(btb), L"Below black did not pass through (encoded gain 2.5)");
			// Off (the old behaviour): the sign-preserving boost and gain act on
			// below black too, so code 40 moves by the gain (still below black).
			Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "btb_passthrough", 0.0));
			set(1.3, boost);
			const int btbOut = code(btb);
			const double btbExpected = 64.0 + 876.0 * spow(spow(spow(btb, boost), 2.41) * 1.3, 1.0 / 2.41);
			Assert::IsTrue(btbOut < 64, (L"Below black was cut to black: " + std::to_wstring(btbOut)).c_str());
			Assert::AreEqual(btbExpected, static_cast<double>(btbOut), 1.5,
				(L"Below black (passthrough off), expected " + std::to_wstring(btbExpected)).c_str());
			Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "btb_passthrough", 1.0));
			// Above white keeps its order against white (the knee rolls it off).
			const int white = code(signal(940.0));
			const int wtw = code(signal(1019.0));
			Assert::IsTrue(wtw >= white && wtw <= 1023, (L"White " + std::to_wstring(white) +
				L", above white " + std::to_wstring(wtw)).c_str());

			// GEM-W1B V6 MUST-2 (register 115 #17): true whiter-than-white (and true below black) leave the
			// hook exactly as captured under the boost and any gain, above AND below 1, in both gain domains.
			set(1.0, 1.0);
			const int idleWtw = code(signal(1019.0)), idleWtw2 = code(signal(990.0)), idleBtb = code(signal(40.0));
			Assert::IsTrue(idleWtw > code(signal(940.0)), L"the idle hook does not keep whiter than white");
			for (const double domain : { 1.0, 0.0 })
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", domain));
				for (const double g : { 0.6, 1.3, 2.5 })
				{
					set(g, boost);
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", domain));
					Assert::AreEqual(idleWtw, code(signal(1019.0)), L"Whiter than white moved (1019)");
					Assert::AreEqual(idleWtw2, code(signal(990.0)), L"Whiter than white moved (990)");
					Assert::AreEqual(idleBtb, code(signal(40.0)), L"Below black moved");
				}
			}
			set(1.0, boost);   // the gain == 1 exit with a boost: the same preservation
			Assert::AreEqual(idleWtw, code(signal(1019.0)), L"Whiter than white moved under the boost alone");
			Assert::AreEqual(idleBtb, code(signal(40.0)), L"Below black moved under the boost alone");

			// Delta 4: light constant through LED 255 -> 100 (linear flux, 2.55x).
			const double grey = (126.0 - 16.0) / 219.0;   // 8-bit 126 as the signal
			for (const double b : { boost, 1.0 })
			{
				set(1.0, b);
				const double reference = std::pow(signal(code(grey)), 2.41);           // LED 255
				set(255.0 / 100.0, b);
				const double compensated = std::pow(signal(code(grey)), 2.41) * 100.0 / 255.0;   // LED 100
				Assert::AreEqual(1.0, compensated / reference, 0.01,
					(L"Light at LED 100 vs 255, boost " + std::to_wstring(b)).c_str());
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		// GEM-W1-LED (owner GEM-Q15, register 115 #17): a colour with only some channels above white or
		// below black keeps its hue through the LED compensation (luma-ratio restore, not per channel),
		// under the boost and gains 0.6 / 1.3 / 2.5 in both domains, on a true 10-bit limited target:
		// - out of range in part: every channel = input x min(curve(luma) / luma, 1) within 1 code, hue
		//   angle (BT.709 Cb/Cr) within 0.75 deg; exactly the input where the ratio caps at 1;
		// - luma out of range (grey WTW/BTB, a colour with negative luma): exactly the input;
		// - in range (white 940 and black 64 included), or with the passthrough off: every channel the
		//   same code as that channel's grey, i.e. the per-channel path, byte for byte.
		TEST_METHOD(LedCompensationKeepsTheHueOfPartlyOutOfRangeColours)
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
			ConfiguredShaderRule rule;
			rule.name = "led_compensation";
			rule.filename = "LED compensation.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 2;
			chain.Ensure(values);
			std::vector<const pl_hook*> compensation;
			chain.Collect(false, compensation);
			Assert::AreEqual(size_t{ 1 }, compensation.size(), L"libplacebo rejected LED compensation.glsl");
			const pl_hook* hook = compensation[0];
			OutputEncodingRepair::Hook repair;
			pl_color_repr limited{};
			limited.sys = PL_COLOR_SYSTEM_RGB;
			limited.levels = PL_COLOR_LEVELS_LIMITED;
			limited.bits.sample_depth = 10;
			limited.bits.color_depth = 10;
			repair.SetTarget(limited);
			const std::vector<const pl_hook*> hooks = { repair.Get(), hook };
			const double boost = 2.2 / 2.4, knee = 0.08, eotf = 2.41;
			auto set = [hook, knee, eotf](double gain, double b, double domain)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost", b));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain", gain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", domain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", knee));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "compensation_eotf", eotf));
			};
			using Rgb = std::array<double, 3>;
			using Codes = std::array<int, 3>;
			// 10-bit limited code k <-> signal (k - 64) / 876 before the encode.
			auto signal = [](int code) { return (code - 64.0) / 876.0; };
			auto toSignal = [&signal](const Codes& c) { return Rgb{ signal(c[0]), signal(c[1]), signal(c[2]) }; };
			// A constant colour through the hooks into a true 10-bit limited R10G10B10A2 target.
			auto render = [&fixture, &hooks](const Rgb& in) -> Codes
			{
				pl_gpu gpu = fixture.Gpu();
				pl_fmt sourceFormat = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
				pl_fmt targetFormat = nullptr;
				const auto caps = PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE;
				for (int i = 0; i < gpu->num_formats; ++i)
				{
					pl_fmt f = gpu->formats[i];
					if (f->type == PL_FMT_UNORM && f->num_components == 4 && f->component_depth[0] == 10 &&
						f->texel_size == 4 && (f->caps & caps) == caps && f->host_bits[0] == 10)
					{ targetFormat = f; break; }
				}
				Assert::IsNotNull(sourceFormat);
				Assert::IsNotNull(targetFormat, L"No host-readable RGB10A2 target format");
				std::vector<float> texels(2 * 2 * 4);
				for (int i = 0; i < 4; ++i)
				{
					for (int c = 0; c < 3; ++c)
						texels[i * 4 + c] = static_cast<float>(in[c]);
					texels[i * 4 + 3] = 1.0f;
				}
				pl_tex_params sp{};
				sp.w = sp.h = 2;
				sp.format = sourceFormat;
				sp.sampleable = true;
				sp.initial_data = texels.data();
				pl_tex source = pl_tex_create(gpu, &sp);
				pl_tex_params tp{};
				tp.w = tp.h = 2;
				tp.format = targetFormat;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(source);
				Assert::IsNotNull(target);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(source);
				pl_frame output = TargetLutGpuFixture::MakeRgbFrame(target);
				output.repr.levels = PL_COLOR_LEVELS_LIMITED;
				output.repr.bits.sample_depth = 10;
				output.repr.bits.color_depth = 10;
				output.repr.bits.bit_shift = 0;
				pl_render_params params = pl_render_fast_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.hooks = hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &output, &params));
				pl_gpu_finish(gpu);
				uint32_t pixels[4] = {};
				pl_tex_transfer_params download{};
				download.tex = target;
				download.ptr = pixels;
				Assert::IsTrue(pl_tex_download(gpu, &download));
				pl_tex_destroy(gpu, &source);
				pl_tex_destroy(gpu, &target);
				return { static_cast<int>(pixels[0] & 1023u), static_cast<int>((pixels[0] >> 10) & 1023u),
					static_cast<int>((pixels[0] >> 20) & 1023u) };
			};
			// CPU mirror of the shader's curve on one value (boost, gain in its domain, knee; no toe, no lift).
			auto spow = [](double v, double e) { return (v < 0.0 ? -1.0 : 1.0) * std::pow(std::abs(v), e); };
			auto curve = [&spow, knee, eotf](double v, double gain, double b, double domain)
			{
				v = spow(v, b);
				if (gain == 1.0)
					return v;
				double x = domain > 0.5 ? spow(v, eotf) : v;
				x *= gain;
				if (gain > 1.0 && x >= 1.0 - knee)
					x = 1.0 - knee + knee * (1.0 - std::exp(-(x - (1.0 - knee)) / knee));
				return domain > 0.5 ? spow(x, 1.0 / eotf) : x;
			};
			auto luma = [](const Rgb& c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; };
			auto hueDegrees = [&luma](const Rgb& c)
			{
				const double y = luma(c);
				return std::atan2((c[0] - y) / 1.5748, (c[2] - y) / 1.8556) * 180.0 / 3.14159265358979323846;
			};
			auto hueError = [&hueDegrees](const Rgb& a, const Rgb& b)
			{
				const double d = std::fmod(std::abs(hueDegrees(a) - hueDegrees(b)), 360.0);
				return std::min(d, 360.0 - d);
			};
			auto text = [](const Codes& c)
			{
				return L"(" + std::to_wstring(c[0]) + L", " + std::to_wstring(c[1]) + L", " + std::to_wstring(c[2]) + L")";
			};

			// Partly out of range, luma in range: orange above white, yellow with two channels above white
			// (luma 0.998), and two colours with one channel below black.
			const Codes partly[] = { { 1010, 700, 300 }, { 980, 1000, 200 }, { 40, 600, 300 }, { 300, 20, 700 } };
			// Luma out of range: grey WTW (1019, 941 just above white), grey BTB (63, 8), and a colour with
			// one channel below black and a negative luma.
			const Codes extended[] = { { 1019, 1019, 1019 }, { 941, 941, 941 }, { 63, 63, 63 }, { 8, 8, 8 }, { 100, 30, 64 } };
			// In range, white and black included (the per-channel path).
			const Codes inRange[] = { { 900, 500, 200 }, { 930, 940, 64 } };
			struct Setting { double gain, boost, domain; };
			std::vector<Setting> settings;
			for (const double domain : { 1.0, 0.0 })
				for (const double g : { 0.6, 1.3, 2.5 })
					settings.push_back({ g, boost, domain });
			settings.push_back({ 1.0, boost, 1.0 });   // the gain == 1 exit with the boost

			// Idle (gain 1, no boost): every test colour leaves as it came in.
			set(1.0, 1.0, 1.0);
			for (const Codes& c : partly)
				Assert::IsTrue(render(toSignal(c)) == c, (L"Idle hook moved " + text(c)).c_str());
			for (const Codes& c : extended)
				Assert::IsTrue(render(toSignal(c)) == c, (L"Idle hook moved " + text(c)).c_str());

			std::wstring problems;
			double worstHue = 0.0;
			int scaledCases = 0;
			for (const Setting& s : settings)
			{
				set(s.gain, s.boost, s.domain);
				const std::wstring where = L" at gain " + std::to_wstring(s.gain) + L", domain " +
					std::to_wstring(static_cast<int>(s.domain)) + L"; ";
				for (const Codes& c : partly)
				{
					const Rgb in = toSignal(c);
					const Codes out = render(in);
					const double y = luma(in);
					const double ratio = std::min(curve(y, s.gain, s.boost, s.domain) / y, 1.0);
					if (ratio >= 1.0)
					{
						if (out != c)
							problems += L"capped " + text(c) + L" became " + text(out) + where;
					}
					else
					{
						++scaledCases;
						for (int k = 0; k < 3; ++k)
						{
							const double expected = 64.0 + 876.0 * in[k] * ratio;
							if (std::abs(out[k] - expected) > 1.0)
								problems += L"channel " + std::to_wstring(k) + L" of " + text(c) + L": " +
									std::to_wstring(out[k]) + L" for " + std::to_wstring(expected) + where;
						}
					}
					const double error = hueError(in, toSignal(out));
					worstHue = std::max(worstHue, error);
					if (error > 0.75)
						problems += L"hue of " + text(c) + L" moved " + std::to_wstring(error) + L" deg to " + text(out) + where;
				}
				for (const Codes& c : extended)
				{
					const Codes out = render(toSignal(c));
					if (out != c)
						problems += L"extended " + text(c) + L" became " + text(out) + where;
				}
				for (const Codes& c : inRange)
				{
					const Codes out = render(toSignal(c));
					for (int k = 0; k < 3; ++k)
					{
						const int grey = render(Rgb{ signal(c[k]), signal(c[k]), signal(c[k]) })[k];
						if (out[k] != grey)
							problems += L"in-range " + text(c) + L" channel " + std::to_wstring(k) + L": " +
								std::to_wstring(out[k]) + L", grey " + std::to_wstring(grey) + where;
					}
				}
			}
			// The ratio path ran: gain 0.6 scales 7 of its 8 cases (the dark blue 300/20/700 in linear light
			// caps at 1, its boost outweighing the gain); the gains above 1 and the boost alone cap.
			if (scaledCases != 7)
				problems += L"scaled cases " + std::to_wstring(scaledCases) + L" (expected 7); ";

			// Passthrough off: never flagged, the per-channel path as before (WTW off for the WTW colours,
			// BTB off for the BTB colours).
			set(0.6, boost, 1.0);
			for (const char* name : { "wtw_passthrough", "btb_passthrough" })
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, name, 0.0));
				const bool wtwOff = std::string(name) == "wtw_passthrough";
				for (const Codes& c : partly)
				{
					const bool aboveWhite = c[0] > 940 || c[1] > 940 || c[2] > 940;
					if (aboveWhite != wtwOff)
						continue;
					const Codes out = render(toSignal(c));
					for (int k = 0; k < 3; ++k)
					{
						const int grey = render(Rgb{ signal(c[k]), signal(c[k]), signal(c[k]) })[k];
						if (out[k] != grey)
							problems += std::wstring(L"passthrough off: ") + text(c) + L" channel " + std::to_wstring(k) +
								L": " + std::to_wstring(out[k]) + L", grey " + std::to_wstring(grey) + L"; ";
					}
				}
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, name, 1.0));
			}
			Logger::WriteMessage((L"GEM-W1-LED worst hue error over the partly out-of-range colours: " +
				std::to_wstring(worstHue) + L" deg\n").c_str());
			fixture.CaptureLog(nullptr);
			Assert::IsTrue(problems.empty(), (problems + L"log: " + std::wstring(log.begin(), log.end())).c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}
	};
}
