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

		// GEM-W1-LED split restore (owner GEM-Q15 rulings 2026-11-21, register 115 #17). True 10-bit limited
		// target, gains 0.6 / 0.8 / 1.3 / 2.5 in both domains and the gain == 1 exit, boost 2.2/2.4 and off:
		// - partly out of range: out = (the GPU's own compensation of the in-range part p, rendered as an
		//   in-range colour) + k f e within 1.5 codes; under a gain below 1 an above-white excursion is
		//   scaled by the compensated white C(1); k < 1 only to stay inside codes 4..1019;
		// - below black exact: the below-black channel of a colour with nothing above white keeps its code;
		// - hue at gains below 1 (BT.709 Cb/Cr angle): within 0.5 deg of the input with the boost off; with
		//   the boost on, within 0.3 deg of the in-range part's own (neighbour) boost tilt;
		// - luma out of range (grey WTW/BTB, a colour with negative luma): exactly the input;
		// - in range (white 940 and black 64 included), or with the passthrough off: every channel the
		//   same code as that channel's grey, i.e. the per-channel path, byte for byte;
		// - edge pairs, one code past black/white vs the in-range neighbour: in-range channels within 8
		//   codes, all channels within 10 (model 7 / 9);
		// - no pulse: over gains 0.85 -> 1.01 in steps of 0.0025, each channel's step equals its in-range
		//   part's step within 4 codes (model 3), and out - in-range part stays k f e within 1.5 codes.
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
			// (luma 0.998), a red one code above white, two colours with one channel below black, and a red
			// with blue below black.
			const Codes partly[] = { { 1010, 700, 300 }, { 980, 1000, 200 }, { 941, 200, 100 }, { 40, 600, 300 },
				{ 300, 20, 700 }, { 900, 70, 40 } };
			// Luma out of range: grey WTW (1019, 941 just above white), grey BTB (63, 8), and a colour with a
			// channel below black and a negative luma.
			const Codes extended[] = { { 1019, 1019, 1019 }, { 941, 941, 941 }, { 63, 63, 63 }, { 8, 8, 8 },
				{ 100, 30, 64 } };
			auto aboveWhite = [](const Codes& c) { return c[0] > 940 || c[1] > 940 || c[2] > 940; };
			// CPU mirror of the split (preserve_extended, both passthroughs on) for a colour with luma in (0, 1]:
			// p = the in-range part, e = the excursion already scaled by f, k = the encode-range factor.
			struct Split { Rgb p{}, e{}; double k = 1.0, f = 1.0; };
			auto split = [&curve, &luma](const Rgb& o, double gain, double b, double domain)
			{
				Split s;
				const double y = luma(o);
				bool anyAbove = false;
				double t = 1.0;
				for (int k = 0; k < 3; ++k)
				{
					const double d = o[k] - y;
					anyAbove = anyAbove || o[k] > 1.0;
					if (o[k] > 1.0)
						t = std::min(t, (1.0 - y) / std::max(d, 1e-6));
					if (o[k] < 0.0)
						t = std::min(t, y / std::max(-d, 1e-6));
				}
				t = std::min(std::max(t, 0.0), 1.0);   // the test project is C++14: no std::clamp
				s.f = anyAbove && gain < 1.0 ? curve(1.0, gain, b, domain) : 1.0;
				Rgb c{};
				for (int k = 0; k < 3; ++k)
				{
					s.p[k] = std::min(std::max(y + t * (o[k] - y), 0.0), 1.0);
					c[k] = curve(s.p[k], gain, b, domain);
					s.e[k] = (o[k] - s.p[k]) * s.f;
				}
				const double lowest = -60.0 / 876.0, highest = 955.0 / 876.0;   // codes 4 and 1019
				for (int k = 0; k < 3; ++k)
				{
					if (s.e[k] > 1e-6)
						s.k = std::min(s.k, (highest - c[k]) / s.e[k]);
					if (s.e[k] < -1e-6)
						s.k = std::min(s.k, (c[k] - lowest) / -s.e[k]);
				}
				s.k = std::min(std::max(s.k, 0.0), 1.0);
				return s;
			};
			// In range, white and black included (the per-channel path).
			const Codes inRange[] = { { 900, 500, 200 }, { 930, 940, 64 } };
			struct Setting { double gain, boost, domain; };
			std::vector<Setting> settings;
			for (const double b : { boost, 1.0 })
				for (const double domain : { 1.0, 0.0 })
					for (const double g : { 0.6, 0.8, 1.3, 2.5 })
						settings.push_back({ g, b, domain });
			settings.push_back({ 1.0, boost, 1.0 });   // the gain == 1 exit with the boost

			// Idle (gain 1, no boost): every test colour leaves as it came in.
			set(1.0, 1.0, 1.0);
			for (const Codes& c : partly)
				Assert::IsTrue(render(toSignal(c)) == c, (L"Idle hook moved " + text(c)).c_str());
			for (const Codes& c : extended)
				Assert::IsTrue(render(toSignal(c)) == c, (L"Idle hook moved " + text(c)).c_str());

			std::wstring problems;
			double worstHueOff = 0.0, worstHueExcess = 0.0;
			int scaledExcursions = 0, rangeLimited = 0, exactBelow = 0;
			for (const Setting& s : settings)
			{
				set(s.gain, s.boost, s.domain);
				const std::wstring where = L" at gain " + std::to_wstring(s.gain) + L", boost " + std::to_wstring(s.boost) +
					L", domain " + std::to_wstring(static_cast<int>(s.domain)) + L"; ";
				// The compensated white C(1), as the GPU renders it (grey 1.0 is in range).
				const double whiteGpu = signal(render(Rgb{ 1.0, 1.0, 1.0 })[0]);
				for (const Codes& c : partly)
				{
					const Rgb in = toSignal(c);
					const Codes out = render(in);
					const Split sp = split(in, s.gain, s.boost, s.domain);
					// The in-range part, rendered as an in-range colour: its neighbour's compensation.
					const Codes part = render(sp.p);
					for (int k = 0; k < 3; ++k)
					{
						const double expected = part[k] + 876.0 * sp.k * sp.e[k];
						if (std::abs(out[k] - expected) > 1.5)
							problems += L"channel " + std::to_wstring(k) + L" of " + text(c) + L": " + std::to_wstring(out[k]) +
								L" for in-range part " + std::to_wstring(part[k]) + L" + k f e = " + std::to_wstring(expected) + where;
					}
					if (sp.f < 1.0)
					{
						++scaledExcursions;   // headroom follows the compensated white (the GPU's own C(1))
						if (std::abs(sp.f - whiteGpu) > 1.0 / 876.0)
							problems += L"C(1) " + std::to_wstring(sp.f) + L" vs rendered white " + std::to_wstring(whiteGpu) + where;
					}
					if (sp.k < 1.0)
					{
						++rangeLimited;
						for (int k = 0; k < 3; ++k)
							if (out[k] < 4 || out[k] > 1019)
								problems += L"range-limited " + text(c) + L" left codes 4..1019: " + text(out) + where;
					}
					if (!aboveWhite(c) && sp.k == 1.0)
						for (int k = 0; k < 3; ++k)
							if (c[k] < 64)
							{
								++exactBelow;
								if (out[k] != c[k])
									problems += L"below black " + text(c) + L" channel " + std::to_wstring(k) + L" became " +
										std::to_wstring(out[k]) + where;
							}
					if (s.gain < 1.0)
					{
						const double error = hueError(in, toSignal(out));
						if (s.boost == 1.0)
						{
							worstHueOff = std::max(worstHueOff, error);
							if (error > 0.5)
								problems += L"hue of " + text(c) + L" moved " + std::to_wstring(error) + L" deg to " + text(out) + where;
						}
						else
						{
							const double excess = error - hueError(in, toSignal(part));
							worstHueExcess = std::max(worstHueExcess, excess);
							if (excess > 0.3)
								problems += L"hue of " + text(c) + L" moved " + std::to_wstring(excess) +
									L" deg beyond its in-range part's boost tilt, to " + text(out) + where;
						}
					}
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
			// Non-vacuous: the C(1) excursion scale, the encode-range factor and the exact below-black channel
			// all ran (model: 980/1000/200 at encoded gain 0.6 with the boost has k = 0.80).
			if (scaledExcursions == 0 || rangeLimited == 0 || exactBelow == 0)
				problems += L"paths not exercised: scaled " + std::to_wstring(scaledExcursions) + L", range-limited " +
					std::to_wstring(rangeLimited) + L", exact below black " + std::to_wstring(exactBelow) + L"; ";

			// Edge continuity: a pixel one code past black or white against its neighbour just inside range,
			// at gains 0.6 / 0.8 / 1.3 / 2.5 in both domains and the gain == 1 exit, boost on and off.
			// In-range channels within 8 codes, all channels within 10 (model: 7 and 9). For comparison,
			// 71e7b8322 stepped up to 281 codes and the W1B per-channel restore 168 codes (R of 941/700/300
			// held at 941 against 773 at linear gain 0.6).
			const std::pair<Codes, Codes> edges[] = { { { 900, 70, 65 }, { 900, 70, 63 } },
				{ { 200, 65, 600 }, { 200, 63, 600 } }, { { 66, 70, 68 }, { 63, 70, 68 } },
				{ { 940, 700, 300 }, { 941, 700, 300 } }, { { 939, 700, 300 }, { 941, 700, 300 } },
				{ { 939, 200, 100 }, { 941, 200, 100 } } };
			int worstEdgeIn = 0, worstEdgeAll = 0;
			for (const Setting& s : settings)
			{
				set(s.gain, s.boost, s.domain);
				for (const auto& pair : edges)
				{
					const Codes inside = render(toSignal(pair.first)), outside = render(toSignal(pair.second));
					int stepIn = 0, stepAll = 0;
					for (int k = 0; k < 3; ++k)
					{
						const int step = std::abs(inside[k] - outside[k]);
						stepAll = std::max(stepAll, step);
						if (pair.second[k] >= 64 && pair.second[k] <= 940)
							stepIn = std::max(stepIn, step);
					}
					worstEdgeIn = std::max(worstEdgeIn, stepIn);
					worstEdgeAll = std::max(worstEdgeAll, stepAll);
					if (stepIn > 8 || stepAll > 10)
						problems += L"edge " + text(pair.first) + L" -> " + text(inside) + L" vs " + text(pair.second) +
							L" -> " + text(outside) + L": steps " + std::to_wstring(stepIn) + L" / " + std::to_wstring(stepAll) +
							L" at gain " + std::to_wstring(s.gain) + L", boost " + std::to_wstring(s.boost) + L", domain " +
							std::to_wstring(static_cast<int>(s.domain)) + L"; ";
				}
			}

			// No pulse: the LED ramping through 1 (gains 0.85 -> 1.01 in steps of 0.0025, with the boost). A
			// partly out-of-range pixel's in-range part tracks its in-range neighbour step for step: each
			// channel's step equals the in-range part's step within 4 codes (model 3), and out - in-range part
			// stays k f e within 1.5 codes at every gain.
			int worstStep = 0;
			for (const Codes& c : { Codes{ 1010, 700, 300 }, Codes{ 980, 1000, 200 }, Codes{ 40, 600, 300 },
				Codes{ 300, 20, 700 } })
				for (const double domain : { 1.0, 0.0 })
				{
					const Rgb in = toSignal(c);
					Codes previous{}, previousPart{};
					for (int i = 0; i <= 64; ++i)
					{
						const double g = static_cast<float>(0.85 + 0.0025 * i);   // the value the shader sees
						set(g, boost, domain);
						const Split sp = split(in, g, boost, domain);
						const Codes out = render(in), part = render(sp.p);
						for (int k = 0; k < 3; ++k)
						{
							if (std::abs(out[k] - (part[k] + 876.0 * sp.k * sp.e[k])) > 1.5)
								problems += L"sweep " + text(c) + L" channel " + std::to_wstring(k) + L": " + std::to_wstring(out[k]) +
									L" vs in-range part " + std::to_wstring(part[k]) + L" at gain " + std::to_wstring(g) + L", domain " +
									std::to_wstring(static_cast<int>(domain)) + L"; ";
							if (i > 0)
							{
								const int step = std::abs((out[k] - previous[k]) - (part[k] - previousPart[k]));
								worstStep = std::max(worstStep, step);
								if (step > 4)
									problems += L"pulse " + text(c) + L" channel " + std::to_wstring(k) + L": " +
										std::to_wstring(previous[k]) + L" -> " + std::to_wstring(out[k]) + L" while its in-range part " +
										std::to_wstring(previousPart[k]) + L" -> " + std::to_wstring(part[k]) + L" at gain " +
										std::to_wstring(g) + L", domain " + std::to_wstring(static_cast<int>(domain)) + L"; ";
							}
						}
						previous = out;
						previousPart = part;
					}
				}

			// Passthrough off: never flagged, the per-channel path as before (WTW off for the WTW colours,
			// BTB off for the BTB colours).
			set(0.6, boost, 1.0);
			for (const char* name : { "wtw_passthrough", "btb_passthrough" })
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, name, 0.0));
				const bool wtwOff = std::string(name) == "wtw_passthrough";
				for (const Codes& c : partly)
				{
					const bool above = aboveWhite(c);
					const bool below = c[0] < 64 || c[1] < 64 || c[2] < 64;
					if (above != wtwOff || (above && below))
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
			Logger::WriteMessage((L"GEM-W1-LED gain < 1 hue: worst " + std::to_wstring(worstHueOff) +
				L" deg (boost off), worst excess over the in-range part's boost tilt " + std::to_wstring(worstHueExcess) +
				L" deg; edge pairs worst in-range / all-channel step " + std::to_wstring(worstEdgeIn) + L" / " +
				std::to_wstring(worstEdgeAll) + L" codes; sweep worst step against the in-range part " +
				std::to_wstring(worstStep) + L" codes\n").c_str());
			fixture.CaptureLog(nullptr);
			Assert::IsTrue(problems.empty(), (problems + L"log: " + std::wstring(log.begin(), log.end())).c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}
	};
}
