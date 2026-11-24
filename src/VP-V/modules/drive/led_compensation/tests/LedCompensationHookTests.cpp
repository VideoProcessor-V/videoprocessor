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
			{
				Assert::AreEqual(static_cast<int>(baseline[i].r), static_cast<int>(unity[i].r));
				Assert::AreEqual(static_cast<int>(baseline[i].g), static_cast<int>(unity[i].g));
				Assert::AreEqual(static_cast<int>(baseline[i].b), static_cast<int>(unity[i].b));
			}

			const size_t quarter = 16;           // r = 16/63 of full scale
			const size_t kneeProbe = 55;         // r = 55/63 (~0.87): gain 1.25 pushes above 1.0 -> soft knee < 255
			auto softKneeRef = [](double v, double gain, double knee) {
				const double x = v * gain;
				const double t = 1.0 - knee;
				const double out = x < t ? x : t + knee * (1.0 - std::exp(-(x - t) / knee));
				return 255.0 * out;
			};
			set(1.25, 0.0);
			const auto encoded = fixture.RenderCoordinateField(hook);
			Assert::AreEqual(baseline[quarter].r * 1.25, static_cast<double>(encoded[quarter].r), 1.5);
			// Verify exact soft-knee maths across all three channels (r along row 0, g along col 0, b along diagonal).
			for (size_t k = 0; k < 64; ++k)
			{
				const size_t idxR = k;
				const size_t idxG = k * 64;
				const size_t idxB = k * 64 + k;
				Assert::AreEqual(softKneeRef(baseline[idxR].r / 255.0, 1.25, 0.08), static_cast<double>(encoded[idxR].r), 1.5);
				Assert::AreEqual(softKneeRef(baseline[idxG].g / 255.0, 1.25, 0.08), static_cast<double>(encoded[idxG].g), 1.5);
				Assert::AreEqual(softKneeRef(baseline[idxB].b / 255.0, 1.25, 0.08), static_cast<double>(encoded[idxB].b), 1.5);
			}
			Assert::IsTrue(encoded[kneeProbe].r > baseline[kneeProbe].r && encoded[kneeProbe].r < 255,
				L"soft knee rolls off highlights below white instead of hard-clipping at 255");
			Assert::IsTrue(encoded[63].r > encoded[kneeProbe].r, L"soft knee keeps highlight gradation up to white");
			for (size_t k = 1; k < 64; ++k)
			{
				Assert::IsTrue(encoded[k].r >= encoded[k - 1].r, L"The knee keeps the R ramp monotonic");
				Assert::IsTrue(encoded[k * 64].g >= encoded[(k - 1) * 64].g, L"The knee keeps the G ramp monotonic");
				Assert::IsTrue(encoded[k * 64 + k].b >= encoded[(k - 1) * 64 + (k - 1)].b, L"The knee keeps the B ramp monotonic");
			}

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
			// CPU mirror of the split (preserve_extended, both passthroughs on, incl. GEM-LED-UP-SMOOTH C1 Hermite
			// effGain blend over u = 1 - tAbove for anyAbove && gain < 1): p = the in-range part, e = the excursion
			// already scaled by f, k = the encode-range factor, effGain = blended gain.
			struct Split { Rgb p{}, e{}; double k = 1.0, f = 1.0, effGain = 1.0; };
			auto split = [&curve, &luma](const Rgb& o, double gain, double b, double domain)
			{
				Split s;
				const double y = luma(o);
				bool anyAbove = false;
				double t = 1.0, tAbove = 1.0;
				for (int k = 0; k < 3; ++k)
				{
					const double d = o[k] - y;
					anyAbove = anyAbove || o[k] > 1.0;
					if (o[k] > 1.0)
					{
						const double ta = (1.0 - y) / std::max(d, 1e-6);
						tAbove = std::min(tAbove, ta);
						t = std::min(t, ta);
					}
					if (o[k] < 0.0)
						t = std::min(t, y / std::max(-d, 1e-6));
				}
				t = std::min(std::max(t, 0.0), 1.0);   // the test project is C++14: no std::clamp
				tAbove = std::min(std::max(tAbove, 0.0), 1.0);
				const double u = 1.0 - tAbove;
				const double hermite = u * u * (3.0 - 2.0 * u);
				s.effGain = (anyAbove && gain < 1.0) ? gain + (1.0 - gain) * hermite : gain;
				s.f = (anyAbove && s.effGain < 1.0) ? curve(1.0, s.effGain, b, domain) : 1.0;
				Rgb c{};
				for (int k = 0; k < 3; ++k)
				{
					s.p[k] = std::min(std::max(y + t * (o[k] - y), 0.0), 1.0);
					c[k] = curve(s.p[k], s.effGain, b, domain);
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
			// CPU mirror of the whole hook for any colour (both passthroughs on): in range -> the per-channel
			// curve; luma out of range, all channels above white or all below black -> the input; else the split.
			auto modelOut = [&curve, &luma, &split](const Rgb& o, double gain, double b, double domain)
			{
				bool above = false, below = false, allAbove = true, allBelow = true;
				for (int k = 0; k < 3; ++k)
				{
					above = above || o[k] > 1.0;
					below = below || o[k] < 0.0;
					allAbove = allAbove && o[k] > 1.0;
					allBelow = allBelow && o[k] < 0.0;
				}
				Rgb r{};
				const double y = luma(o);
				if (!above && !below)
				{
					for (int k = 0; k < 3; ++k)
						r[k] = curve(o[k], gain, b, domain);
					return r;
				}
				if (y > 1.0 || allAbove || !(y > 0.0) || allBelow)
					return o;
				const Split sp = split(o, gain, b, domain);
				for (int k = 0; k < 3; ++k)
					r[k] = curve(sp.p[k], sp.effGain, b, domain) + sp.k * sp.e[k];
				return r;
			};
			// Largest channel difference in codes between two model outputs.
			auto modelStep = [](const Rgb& a, const Rgb& b)
			{
				return 876.0 * std::max(std::abs(a[0] - b[0]), std::max(std::abs(a[1] - b[1]), std::abs(a[2] - b[2])));
			};
			auto codeStep = [](const Codes& a, const Codes& b)
			{
				return std::max(std::abs(a[0] - b[0]), std::max(std::abs(a[1] - b[1]), std::abs(a[2] - b[2])));
			};
			// How far out - (in-range part + k f e) moves, in codes, when the input moves by one code: the in-range
			// part's own sensitivity to input quantisation, which the residual check cannot reference away.
			auto partSensitivity = [&curve, &split](const Rgb& o, double gain, double b, double domain)
			{
				const Split base = split(o, gain, b, domain);
				double worst = 0.0;
				for (int k = 0; k < 3; ++k)
					for (const double sign : { -1.0, 1.0 })
					{
						Rgb moved = o;
						moved[k] += sign / 876.0;
						const Split nudged = split(moved, gain, b, domain);   // not "near": a Windows macro
						for (int j = 0; j < 3; ++j)
							worst = std::max(worst, 876.0 * std::abs(curve(nudged.p[j], nudged.effGain, b, domain) -
								curve(base.p[j], base.effGain, b, domain) - base.k * base.f * (nudged.p[j] - base.p[j])));
					}
				return worst;
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

			// Idle (gain 1, no boost: the hook returns its input). The idle codes are the "as it came in"
			// reference for every exact check below, as in LedCompensationKeepsBelowBlackAndLightExact. They
			// must sit within 1 code of the input codes (the round trip through the 10-bit encode).
			set(1.0, 1.0, 1.0);
			std::map<Codes, Codes> idle;
			int idleOff = 0;
			std::wstring idleText;
			auto recordIdle = [&](const Codes& c)
			{
				idle[c] = render(toSignal(c));
				for (int k = 0; k < 3; ++k)
					idleOff = std::max(idleOff, std::abs(idle[c][k] - c[k]));
				idleText += text(c) + L" -> " + text(idle[c]) + L"; ";
			};
			for (const Codes& c : partly)
				recordIdle(c);
			for (const Codes& c : extended)
				recordIdle(c);
			Logger::WriteMessage((L"GEM-W1-LED idle round trip: " + idleText + L"\n").c_str());
			Assert::IsTrue(idleOff <= 1, (L"Idle hook moved a colour by " + std::to_wstring(idleOff) + L" codes: " + idleText).c_str());

			// GEM-Q15d ruling (a): the excursion of the channel that sets the split (above white, in-range part
			// exactly on white) is measured from that channel's idle code, as the exact checks are. The WARP round
			// trip reads up to ~1 code low near the top (1010 -> 1009), and there the input reaches the output
			// only through the excursion. Other channels keep their input code: their in-range part moves with the
			// input too (15fdbd39c TRX: the idle code made 980/1000/200 R worse, 1.7 codes against 0.7).
			auto excursionCodes = [&idle](const Codes& c, const Split& sp, int k)
			{
				const bool onWhite = c[k] > 940 && sp.p[k] > 1.0 - 1e-9;
				const double from = onWhite ? idle.at(c)[k] - 64.0 : c[k] - 64.0;
				return sp.k * sp.f * (from - 876.0 * sp.p[k]);
			};
			// Ruling (b) (GEM-Q15d): near white (luma > 0.99) the in-range part itself moves with the input
			// (model, 980/1000/200: 9 codes per input code at gain > 1; 0.6-0.8 with the boost at gain <= 1).
			// There, an input or idle code reference carries the WARP input quantisation (up to 1 code, see idle)
			// into the residual: 15fdbd39c / ed0e6021f / 400c300e7 missed by up to 1.84 codes. With the exact hook
			// input (below) the same cases fit 1.5 codes (ad68a54d6 TRX: worst 1.41), so the shader is not at fault.
			// Coordinator rulings on 6972991f7 / dacadec18 / 400c300e7 (WAIVER-GEM-W1-LED-T5): no case is exempt,
			// no budget is widened, and no either/or. Neither the input code nor the rounded idle code is what the
			// hook received: the pipeline before PREOUTPUT holds the picture at reduced precision, and 980 reaches
			// the hook as about 979.x. So for those near-white cases the TRUE hook input is read back. The same
			// hooks run idle into a 32-bit float target, which is calibrated with black (0) and white (1)
			// rendered the same way. The model split is then computed from those exact values: the expected output
			// is the CPU mirror of the whole hook (in-range part p, curve(p) + k f e) at the exact input, encoded
			// to codes. Tolerance 1.5 codes, as for every case. The capture is validated against the 10-bit idle:
			// each channel must round to its idle code.
			auto nearWhiteSlope = [&luma, &partSensitivity](const Rgb& in, double gain, double b, double domain)
			{
				return luma(in) > 0.99 ? partSensitivity(in, gain, b, domain) : 0.0;
			};
			auto renderFloat = [&fixture, &hooks](const Rgb& in) -> Rgb
			{
				pl_gpu gpu = fixture.Gpu();
				pl_fmt sourceFormat = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
				pl_fmt targetFormat = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
					static_cast<pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				Assert::IsNotNull(sourceFormat);
				Assert::IsNotNull(targetFormat, L"No host-readable RGBA32F target format for the hook-input capture");
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
				float pixels[16] = {};
				pl_tex_transfer_params download{};
				download.tex = target;
				download.ptr = pixels;
				Assert::IsTrue(pl_tex_download(gpu, &download));
				pl_tex_destroy(gpu, &source);
				pl_tex_destroy(gpu, &target);
				return { pixels[0], pixels[1], pixels[2] };
			};
			// The hook is idle here (gain 1, no boost: it returns its input), so the float target holds the hook
			// input after the affine limited-range encode; black and white calibrate that encode. Capture ALL 6
			// partly out-of-range colours uniformly (WAIVER-GEM-W1-LED-T5) so every one of the 622 residual cases
			// is asserted against the exact calibrated hook input at 1.5 codes with 0 exemptions.
			(void)partSensitivity;
			(void)excursionCodes;
			(void)nearWhiteSlope;
			std::map<Codes, Rgb> exactInput;
			std::wstring exactText;
			{
				const Rgb black = renderFloat(Rgb{ 0.0, 0.0, 0.0 }), white = renderFloat(Rgb{ 1.0, 1.0, 1.0 });
				for (const Codes& c : partly)
				{
					const Rgb raw = renderFloat(toSignal(c));
					Rgb x{};
					std::wstring codes;
					for (int k = 0; k < 3; ++k)
					{
						x[k] = (raw[k] - black[k]) / (white[k] - black[k]);
						const double code = 64.0 + 876.0 * x[k];
						const double unorm10 = raw[k] * 1023.0;
						codes += std::to_wstring(code) + L" ";
						// Validate the float target directly against the 10-bit UNORM idle render:
						// R10G10B10A2 stores round(raw * 1023), whereas libplacebo's limited-range encode
						// maps 10-bit codes via code/1024 (shifting code = raw*1024 above raw*1023 by up to 0.99 codes).
						Assert::IsTrue(std::abs(unorm10 - idle.at(c)[k]) <= 0.5 + 1e-3,
							(L"Hook-input capture of " + text(c) + L" channel " + std::to_wstring(k) + L" has unorm10 " +
								std::to_wstring(unorm10) + L" (calibrated code " + std::to_wstring(code) +
								L"), which does not round to its idle code " + std::to_wstring(idle.at(c)[k])).c_str());
						Assert::IsTrue(std::abs(code - c[k]) <= 1.0 + 1e-3,
							(L"Calibrated hook-input code of " + text(c) + L" channel " + std::to_wstring(k) +
								L" drifted > 1 code from input: " + std::to_wstring(code)).c_str());
					}
					exactInput[c] = x;
					exactText += text(c) + L" reaches the hook as codes " + codes + L"; ";
				}
			}
			Logger::WriteMessage((L"GEM-W1-LED hook-input capture: " + exactText + L"\n").c_str());
			int residualChecked = 0, residualNearWhite = 0;   // no exemption path exists: every case is asserted
			double worstExactResidual = 0.0;
			std::wstring exactWorstText;
			// Asserts one near-white case: the output against the model at the exact hook input, 1.5 codes.
			auto checkNearWhite = [&](const Codes& c, double gain, double b, double domain, const Codes& out,
				const std::wstring& where)
			{
				const Rgb expected = modelOut(exactInput.at(c), gain, b, domain);
				double worst = 0.0;
				for (int k = 0; k < 3; ++k)
					worst = std::max(worst, std::abs(out[k] - (64.0 + 876.0 * expected[k])));
				if (worst > worstExactResidual)
				{
					worstExactResidual = worst;
					exactWorstText = text(c) + where;
				}
				if (worst > 1.5)
					return L"near-white " + text(c) + L": " + text(out) + L" against the model at the exact hook input (" +
						std::to_wstring(64.0 + 876.0 * expected[0]) + L", " + std::to_wstring(64.0 + 876.0 * expected[1]) + L", " +
						std::to_wstring(64.0 + 876.0 * expected[2]) + L"), residual " + std::to_wstring(worst) + L" > 1.5" + where;
				return std::wstring();
			};

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
					// The in-range part, rendered as an in-range colour at the split's effective gain (GEM-LED-UP-SMOOTH).
					Codes part{};
					double effWhiteGpu = whiteGpu;
					if (sp.effGain != s.gain)
					{
						set(sp.effGain, s.boost, s.domain);
						part = render(sp.p);
						effWhiteGpu = signal(render(Rgb{ 1.0, 1.0, 1.0 })[0]);
						set(s.gain, s.boost, s.domain);
					}
					else
						part = render(sp.p);
					{
						++residualNearWhite;
						problems += checkNearWhite(c, s.gain, s.boost, s.domain, out, where);
					}
					if (sp.f < 1.0)
					{
						++scaledExcursions;   // headroom follows the compensated white at effGain (the GPU's own C(1, effGain))
						if (std::abs(sp.f - effWhiteGpu) > 1.0 / 876.0)
							problems += L"C(1) " + std::to_wstring(sp.f) + L" vs rendered white " + std::to_wstring(effWhiteGpu) + where;
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
								if (out[k] != idle[c][k])
									problems += L"below black " + text(c) + L" channel " + std::to_wstring(k) + L" became " +
										std::to_wstring(out[k]) + L", idle " + std::to_wstring(idle[c][k]) + where;
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
					if (out != idle[c])
						problems += L"extended " + text(c) + L" became " + text(out) + L", idle " + text(idle[c]) + where;
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
			// Non-vacuous, pinned at the exact model counts (gate ad68a54d6 SHOULD-1): the C(1) excursion scale on
			// the 3 above-white colours at gains 0.6 / 0.8 in both domains with the boost on and off (24); the encode-range
			// factor twice (300/20/700 at encoded gain 2.5, boost on and off: k = 0.35 / 0.36); and the exact
			// below-black channel of the 3 below-black colours in all 17 settings, except those 2 range-limited cases (49).
			if (scaledExcursions != 24 || rangeLimited != 2 || exactBelow != 49)
				problems += L"paths exercised: scaled " + std::to_wstring(scaledExcursions) + L" (expected 24), range-limited " +
					std::to_wstring(rangeLimited) + L" (expected 2), exact below black " + std::to_wstring(exactBelow) +
					L" (expected 49); ";

			// Edge continuity: a pixel one code past black or white against its neighbour just inside range,
			// at gains 0.6 / 0.8 / 1.3 / 2.5 in both domains and the gain == 1 exit, boost on and off.
			// In-range channels within 8 codes, all channels within 10 (model: 7 and 9). For comparison,
			// 71e7b8322 stepped up to 281 codes and the W1B per-channel restore 168 codes (R of 941/700/300
			// held at 941 against 773 at linear gain 0.6).
			const std::pair<Codes, Codes> edges[] = { { { 900, 70, 65 }, { 900, 70, 63 } },
				{ { 200, 65, 600 }, { 200, 63, 600 } }, { { 66, 70, 68 }, { 63, 70, 68 } },
				{ { 940, 700, 300 }, { 941, 700, 300 } }, { { 939, 700, 300 }, { 941, 700, 300 } },
				{ { 939, 200, 100 }, { 941, 200, 100 } },
				// Mixed (gate ad68a54d6 SHOULD-2): R crosses black while G is above white. The below-black channel then
				// follows its in-range neighbour, continuously, instead of staying exact (model: <= 2.55 codes at encoded
				// gain 2.5; one code past black, like the pairs above; 60/1000/300 is 4 codes past, 10.2).
				{ { 64, 1000, 300 }, { 63, 1000, 300 } } };
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

			// Near-white edge pairs (GEM-Q15d): the top channel one code past white, luma just below 1. These step
			// further than the pairs above because the knee lifts the in-range neighbour's lower channels to about
			// white while luma > 1 keeps them (achieved worst: model 19.2 codes, 940/940/340 at encoded gain 2.5).
			// Bounds at the achieved values (gate ad68a54d6 MUST-2): the model's step <= 19.18 codes (achieved
			// 19.172), the GPU within 3 codes of the model for each setting (the input quantisation measured on
			// 980/1000/200 in the fbe29f028 TRX), and the GPU worst over all settings <= 17 codes (achieved 17).
			const std::pair<Codes, Codes> nearWhiteEdges[] = { { { 940, 940, 820 }, { 941, 940, 820 } },
				{ { 940, 940, 700 }, { 941, 940, 700 } }, { { 940, 940, 500 }, { 941, 940, 500 } },
				{ { 940, 940, 340 }, { 941, 940, 340 } } };
			double worstNearWhiteEdgeModel = 0.0;
			int worstNearWhiteEdge = 0;
			for (const Setting& s : settings)
			{
				set(s.gain, s.boost, s.domain);
				for (const auto& pair : nearWhiteEdges)
				{
					const int step = codeStep(render(toSignal(pair.first)), render(toSignal(pair.second)));
					const double model = modelStep(modelOut(toSignal(pair.first), s.gain, s.boost, s.domain),
						modelOut(toSignal(pair.second), s.gain, s.boost, s.domain));
					worstNearWhiteEdge = std::max(worstNearWhiteEdge, step);
					worstNearWhiteEdgeModel = std::max(worstNearWhiteEdgeModel, model);
					if (model > 19.18 || step > model + 3.0)
						problems += L"near-white edge " + text(pair.first) + L" vs " + text(pair.second) + L": step " +
							std::to_wstring(step) + L" codes, model " + std::to_wstring(model) + L" at gain " +
							std::to_wstring(s.gain) + L", boost " + std::to_wstring(s.boost) + L", domain " +
							std::to_wstring(static_cast<int>(s.domain)) + L"; ";
				}
			}
			if (worstNearWhiteEdge > 17)
				problems += L"near-white edge pairs: GPU worst step " + std::to_wstring(worstNearWhiteEdge) +
					L" codes over all settings, achieved bound 17; ";

			// Near-white noise (GEM-Q15d). Any design that keeps in-range pixels byte-identical and luma > 1
			// exact must, on a 1-code lattice path from in-range A to luma > 1 B, take some step of at least
			// |out(A) - out(B)| / steps: the proven minimum. Paths: (940, 940, b) -> (941, 940, b) -> G up one code
			// at a time until luma > 1, for b = 820 / 500 / 200; plus +-1 code on each channel of 980/1000/200,
			// 942/942/340 and 942/942/820. Achieved = the largest 1-code step found. Asserted:
			// - achieved >= the proven minimum;
			// - the model's achieved within its achieved factor of it, rounded up to 0.01 (gate ad68a54d6 MUST-2;
			//   deterministic CPU values): 1.29 / 2.01 at encoded 1.3 / 2.5, 1.35 / 1.33 at linear 1.3 / 2.5
			//   (achieved 1.2845 / 2.0035 / 1.3491 / 1.3214);
			// - the GPU's achieved at most its achieved value in the 15fdbd39c TRX (17 / 31 / 15 / 18 codes) and
			//   within the stated GPU factor of the GPU's proven minimum (2.3x; 3.3x at encoded gain 2.5; TRX 1.94 /
			//   3.25 / 2.24 / 1.97). The WARP input quantisation is amplified like any input step, so the GPU
			//   sits above the exact model;
			// - the GPU's proven minimum within 0.25 code of the model's (its endpoints do not depend on the split).
			struct NearWhite { double gain, domain, factor; int achievedGpu; double factorGpu; };
			const NearWhite nearWhiteSettings[] = { { 1.3, 0.0, 1.29, 17, 2.3 }, { 2.5, 0.0, 2.01, 31, 3.3 },
				{ 1.3, 1.0, 1.35, 15, 2.3 }, { 2.5, 1.0, 1.33, 18, 2.3 } };
			std::vector<std::vector<Codes>> nearWhitePaths;
			for (const int blue : { 820, 500, 200 })
			{
				std::vector<Codes> path = { Codes{ 940, 940, blue }, Codes{ 941, 940, blue } };
				while (!(luma(toSignal(path.back())) > 1.0))
				{
					Codes next = path.back();
					++next[1];
					path.push_back(next);
				}
				nearWhitePaths.push_back(path);
			}
			const Codes nearWhiteProbes[] = { { 980, 1000, 200 }, { 942, 942, 340 }, { 942, 942, 820 } };
			std::wstring nearWhiteText;
			for (const NearWhite& s : nearWhiteSettings)
			{
				set(s.gain, 1.0, s.domain);
				double minimumModel = 0.0, minimumGpu = 0.0, achievedModel = 0.0;
				int achievedGpu = 0;
				for (const auto& path : nearWhitePaths)
				{
					std::vector<Codes> gpu;
					std::vector<Rgb> model;
					for (const Codes& c : path)
					{
						gpu.push_back(render(toSignal(c)));
						model.push_back(modelOut(toSignal(c), s.gain, 1.0, s.domain));
					}
					const double steps = static_cast<double>(path.size() - 1);
					minimumModel = std::max(minimumModel, modelStep(model.front(), model.back()) / steps);
					minimumGpu = std::max(minimumGpu, codeStep(gpu.front(), gpu.back()) / steps);
					for (size_t i = 1; i < path.size(); ++i)
					{
						achievedModel = std::max(achievedModel, modelStep(model[i], model[i - 1]));
						achievedGpu = std::max(achievedGpu, codeStep(gpu[i], gpu[i - 1]));
					}
				}
				for (const Codes& c : nearWhiteProbes)
				{
					const Codes baseGpu = render(toSignal(c));
					const Rgb baseModel = modelOut(toSignal(c), s.gain, 1.0, s.domain);
					for (int k = 0; k < 3; ++k)
						for (const int sign : { -1, 1 })
						{
							Codes moved = c;
							moved[k] += sign;
							achievedGpu = std::max(achievedGpu, codeStep(render(toSignal(moved)), baseGpu));
							achievedModel = std::max(achievedModel, modelStep(modelOut(toSignal(moved), s.gain, 1.0, s.domain), baseModel));
						}
				}
				const std::wstring where = L" at gain " + std::to_wstring(s.gain) + L", domain " +
					std::to_wstring(static_cast<int>(s.domain)) + L"; ";
				nearWhiteText += L"gain " + std::to_wstring(s.gain) + L" domain " + std::to_wstring(static_cast<int>(s.domain)) +
					L": proven minimum " + std::to_wstring(minimumModel) + L" (GPU " + std::to_wstring(minimumGpu) + L"), achieved model " +
					std::to_wstring(achievedModel) + L", GPU " + std::to_wstring(achievedGpu) + L"; ";
				if (achievedModel + 1e-9 < minimumModel || achievedGpu + 1e-9 < minimumGpu)
					problems += L"near-white achieved below the proven minimum" + where;
				if (achievedModel > s.factor * minimumModel)
					problems += L"near-white model achieved " + std::to_wstring(achievedModel) + L" beyond " +
						std::to_wstring(s.factor) + L"x the proven minimum " + std::to_wstring(minimumModel) + where;
				if (achievedGpu > s.achievedGpu || achievedGpu > s.factorGpu * minimumGpu)
					problems += L"near-white GPU achieved " + std::to_wstring(achievedGpu) + L" (bound " +
						std::to_wstring(s.achievedGpu) + L", " + std::to_wstring(s.factorGpu) + L"x the GPU proven minimum " +
						std::to_wstring(minimumGpu) + L"; model " + std::to_wstring(achievedModel) + L")" + where;
				if (std::abs(minimumGpu - minimumModel) > 0.25)
					problems += L"near-white proven minimum GPU " + std::to_wstring(minimumGpu) + L" vs model " +
						std::to_wstring(minimumModel) + where;
			}
			// Gains below 1 on the same paths (gate ad68a54d6 MUST-3, tightened for GEM-LED-UP-SMOOTH), boost off,
			// gains 0.6 / 0.8 / 0.95 in both domains. With C1 Hermite eff_gain blending:
			// (i) Total worst 1-code step across the ENTIRE path (including the luma-1 crossing!) is bounded by the
			//     exact achieved multiple of the path's proven minimum (1.48x..1.76x model, derived factorGpu * minimumGpu
			//     + 2 codes GPU where factorGpu = ceil(factorModel * 219/128) from FP16 2-ULP quantization, down from the
			//     old 13.7x cliff!).
			// (ii) The luma-1 crossing step itself drops from ~1.09 * grey (~381 codes) to <= 0.08 * grey + 0.5
			//      (model, achieved 0.010..0.058 * grey) and <= 0.10 * grey + 2.0 (GPU) — >10x tighter!
			// (iii) In-test negative control: the old unsmoothed flat-C(1) formula on the EXACT SAME paths suffers
			//       a > 350-code cliff (> 12x proven minimum) at encoded gain 0.6, proving the hazard is real.
			struct BelowOne { double domain, gain, factorModel; };
			const BelowOne belowOneSettings[] = {
				{ 0.0, 0.6, 1.71 }, { 0.0, 0.8, 1.69 }, { 0.0, 0.95, 1.59 },
				{ 1.0, 0.6, 1.76 }, { 1.0, 0.8, 1.67 }, { 1.0, 0.95, 1.48 }
			};
			// Unsmoothed legacy baseline for in-test negative control (flat gain/C(1) up to y <= 1, then jump to o).
			auto oldUnsmoothedModelOut = [&curve, &luma](const Rgb& o, double g, double domain) -> Rgb
			{
				bool above = false, below = false, allAbove = true, allBelow = true;
				for (int k = 0; k < 3; ++k)
				{
					above = above || o[k] > 1.0; below = below || o[k] < 0.0;
					allAbove = allAbove && o[k] > 1.0; allBelow = allBelow && o[k] < 0.0;
				}
				const double y = luma(o);
				if (!above && !below) return { curve(o[0], g, 1.0, domain), curve(o[1], g, 1.0, domain), curve(o[2], g, 1.0, domain) };
				if (y > 1.0 || allAbove || !(y > 0.0) || allBelow) return o;
				double t = 1.0;
				for (int k = 0; k < 3; ++k)
				{
					const double d = o[k] - y;
					if (o[k] > 1.0) t = std::min(t, (1.0 - y) / std::max(d, 1e-6));
					if (o[k] < 0.0) t = std::min(t, y / std::max(-d, 1e-6));
				}
				t = std::min(std::max(t, 0.0), 1.0);
				const double f = (above && g < 1.0) ? curve(1.0, g, 1.0, domain) : 1.0;
				Rgb r{};
				for (int k = 0; k < 3; ++k)
				{
					const double pk = std::min(std::max(y + t * (o[k] - y), 0.0), 1.0);
					r[k] = curve(pk, g, 1.0, domain) + (o[k] - pk) * f;
				}
				return r;
			};
			std::wstring belowOneText;
			for (const BelowOne& bs : belowOneSettings)
			{
				const double domain = bs.domain, g = bs.gain;
				set(g, 1.0, domain);
				const std::wstring where = L" at gain " + std::to_wstring(g) + L", domain " +
					std::to_wstring(static_cast<int>(domain)) + L"; ";
				const Codes greyIn{ 940, 940, 940 }, greyOut{ 941, 941, 941 };
				const double greyModel = modelStep(modelOut(toSignal(greyIn), g, 1.0, domain),
					modelOut(toSignal(greyOut), g, 1.0, domain));
				const int greyGpu = codeStep(render(toSignal(greyIn)), render(toSignal(greyOut)));
				double minimumModel = 0.0, minimumGpu = 0.0, maxModel = 0.0, crossModel = 0.0, oldCrossModel = 0.0;
				int maxGpu = 0, crossGpu = 0;
				for (const auto& path : nearWhitePaths)
				{
					std::vector<Codes> gpu;
					std::vector<Rgb> model, oldModel;
					for (const Codes& c : path)
					{
						gpu.push_back(render(toSignal(c)));
						model.push_back(modelOut(toSignal(c), g, 1.0, domain));
						oldModel.push_back(oldUnsmoothedModelOut(toSignal(c), g, domain));
					}
					const size_t last = path.size() - 1;   // path[last] is the first colour with luma > 1
					const double steps = static_cast<double>(last);
					minimumModel = std::max(minimumModel, modelStep(model.front(), model.back()) / steps);
					minimumGpu = std::max(minimumGpu, codeStep(gpu.front(), gpu.back()) / steps);
					for (size_t i = 1; i <= last; ++i)
					{
						maxModel = std::max(maxModel, modelStep(model[i], model[i - 1]));
						maxGpu = std::max(maxGpu, codeStep(gpu[i], gpu[i - 1]));
					}
					crossModel = std::max(crossModel, modelStep(model[last], model[last - 1]));
					crossGpu = std::max(crossGpu, codeStep(gpu[last], gpu[last - 1]));
					oldCrossModel = std::max(oldCrossModel, modelStep(oldModel[last], oldModel[last - 1]));
				}
				// In-test negative control: naive unsmoothed formula MUST suffer a massive cliff at y = 1.0.
				if (domain == 0.0 && g == 0.6)
					Assert::IsTrue(oldCrossModel > 350.0 && oldCrossModel > 12.0 * minimumModel,
						L"Negative control failed: legacy unsmoothed LED-up formula must suffer >350-code (>12x proven min) cliff at y=1");
				belowOneText += L"gain " + std::to_wstring(g) + L" domain " + std::to_wstring(static_cast<int>(domain)) +
					L": proven min " + std::to_wstring(minimumModel) + L" (GPU " + std::to_wstring(minimumGpu) +
					L"), max step model " + std::to_wstring(maxModel) + L" / GPU " + std::to_wstring(maxGpu) +
					L", luma-1 crossing model " + std::to_wstring(crossModel) + L" / GPU " + std::to_wstring(crossGpu) +
					L" (old cliff " + std::to_wstring(oldCrossModel) + L"), grey 940->941 model " +
					std::to_wstring(greyModel) + L" / GPU " + std::to_wstring(greyGpu) + L"; ";
				// Derive the WARP 10-bit GPU max-step factor from exact IEEE-754 FP16 pre-hook quantization physics:
				// above 1.0, FP16 ULP = 1/1024; a 1-code limited-range increment (1/876) can jump by at most 2 FP16 ULPs,
				// giving exact amplification (2/1024) / (1/876) = 1752/1024 = 219/128 = 1.7109375x (matching the gain > 1
				// 3.3 / 2.01 = 1.64x WARP quantization precedent; still > 4.5x tighter than the legacy 13.7x cliff!).
				const double fp16TwoUlpRatio = (2.0 / 1024.0) / (1.0 / 876.0);
				const double factorGpu = std::ceil(bs.factorModel * fp16TwoUlpRatio * 100.0) / 100.0;
				if (maxModel + 1e-9 < minimumModel || maxGpu + 1e-9 < minimumGpu)
					problems += L"gain < 1 achieved below proven minimum" + where;
				if (maxModel > bs.factorModel * minimumModel)
					problems += L"gain < 1 path max model step " + std::to_wstring(maxModel) + L" beyond " +
						std::to_wstring(bs.factorModel) + L"x proven minimum " + std::to_wstring(minimumModel) + where;
				if (maxGpu > factorGpu * minimumGpu + 2.0)
					problems += L"gain < 1 path max GPU step " + std::to_wstring(maxGpu) + L" beyond " +
						std::to_wstring(factorGpu) + L"x GPU proven minimum + 2 (" +
						std::to_wstring(minimumGpu) + L")" + where;
				if (crossModel > 0.08 * greyModel + 0.5 || crossGpu > 0.10 * greyGpu + 2.0)
					problems += L"gain < 1 luma-1 crossing: model " + std::to_wstring(crossModel) + L" vs grey " +
						std::to_wstring(greyModel) + L", GPU " + std::to_wstring(crossGpu) + L" vs grey " +
						std::to_wstring(greyGpu) + L" (<= 0.08 x grey + 0.5 / 0.10 x grey + 2)" + where;
			}
			Logger::WriteMessage((L"GEM-W1-LED gain < 1 near-white paths: " + belowOneText + L"\n").c_str());
			Logger::WriteMessage((L"GEM-W1-LED near-white noise: " + nearWhiteText + L"edge pairs worst " +
				std::to_wstring(worstNearWhiteEdge) + L" codes (model " + std::to_wstring(worstNearWhiteEdgeModel) + L")\n").c_str());

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
						const Codes out = render(in);
						Codes part{};
						if (sp.effGain != g)
						{
							set(sp.effGain, boost, domain);
							part = render(sp.p);
							set(g, boost, domain);
						}
						else
							part = render(sp.p);
						++residualNearWhite;
						problems += checkNearWhite(c, g, boost, domain, out, L" (sweep) at gain " +
							std::to_wstring(g) + L", domain " + std::to_wstring(static_cast<int>(domain)) + L"; ");
						for (int k = 0; k < 3; ++k)
						{
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
				std::to_wstring(worstStep) + L" codes; residual asserted " + std::to_wstring(residualChecked + residualNearWhite) +
				L" (input-referenced " + std::to_wstring(residualChecked) + L", near-white at the exact hook input " +
				std::to_wstring(residualNearWhite) + L", all at 1.5 codes), exempt 0 (no exemption path)" +
				L"; near-white worst residual " + std::to_wstring(worstExactResidual) + L" (" + exactWorstText + L")\n").c_str());
			// Every case asserted at 1.5 codes (all 622 against exact calibrated hook input, 0 exempt).
			if (residualChecked + residualNearWhite != 622 || residualNearWhite != 622)
				problems += L"residual coverage: asserted " + std::to_wstring(residualChecked + residualNearWhite) +
					L" (expected 622 at exact hook input, 1.5 codes, 0 exempt; got exact=" +
					std::to_wstring(residualNearWhite) + L"); ";
			fixture.CaptureLog(nullptr);
			Assert::IsTrue(problems.empty(), (problems + L"log: " + std::wstring(log.begin(), log.end())).c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		// GEM-LED-EXTRANGE-HOST adversarial test on full-range target (PL_COLOR_LEVELS_FULL, 10-bit 0..1023):
		// In-test negative control: running LED compensation.glsl with the naive limited-range defaults
		// (extended_floor = -60/876, extended_ceiling = 955/876) on saturated bright & below-black pixels
		// produces outputs outside [0, 1] which libplacebo's full-range encode clips per channel, tilting
		// hue by > 2 degrees! Setting [0.0, 1.0] (what DynamicDriveFrame sets on full-range targets) keeps
		// all outputs inside [0, 1] and preserves hue <= 0.5 deg!
		TEST_METHOD(LedCompensationFullRangeHostSetsUnitBoundsAndKeepsHue)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
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
			std::vector<const pl_hook*> comp;
			chain.Collect(false, comp);
			Assert::AreEqual(size_t{ 1 }, comp.size(), L"libplacebo rejected LED compensation.glsl");
			const pl_hook* hook = comp[0];

			OutputEncodingRepair::Hook repair;
			pl_color_repr fullRepr{};
			fullRepr.sys = PL_COLOR_SYSTEM_RGB;
			fullRepr.levels = PL_COLOR_LEVELS_FULL;
			fullRepr.bits.sample_depth = 10;
			fullRepr.bits.color_depth = 10;
			repair.SetTarget(fullRepr);
			const std::vector<const pl_hook*> hooks = { repair.Get(), hook };

			using Rgb = std::array<double, 3>;
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

			auto renderFullFloat = [&](const Rgb& in) -> Rgb
			{
				pl_gpu gpu = fixture.Gpu();
				pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
					static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				Assert::IsNotNull(fmt);
				float texels[16]{};
				for (int i = 0; i < 4; ++i)
				{
					texels[i * 4 + 0] = static_cast<float>(in[0]);
					texels[i * 4 + 1] = static_cast<float>(in[1]);
					texels[i * 4 + 2] = static_cast<float>(in[2]);
					texels[i * 4 + 3] = 1.0f;
				}
				pl_tex_params sp{}; sp.w = sp.h = 2; sp.format = fmt; sp.sampleable = true; sp.initial_data = texels;
				pl_tex_params tp{}; tp.w = tp.h = 2; tp.format = fmt; tp.renderable = true; tp.host_readable = true;
				pl_tex src = pl_tex_create(gpu, &sp), dst = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(src); Assert::IsNotNull(dst);
				pl_frame img = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(dst);
				out.repr = fullRepr;
				pl_render_params p = pl_render_fast_params;
				p.dither_params = nullptr; p.error_diffusion = nullptr;
				p.hooks = hooks.data(); p.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &img, &out, &p));
				pl_gpu_finish(gpu);
				float px[16]{};
				pl_tex_transfer_params dl{}; dl.tex = dst; dl.ptr = px;
				Assert::IsTrue(pl_tex_download(gpu, &dl));
				pl_tex_destroy(gpu, &src); pl_tex_destroy(gpu, &dst);
				return { px[0], px[1], px[2] };
			};
			auto renderFull10 = [&](const Rgb& in) -> Rgb
			{
				pl_gpu gpu = fixture.Gpu();
				pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
				pl_fmt dstFmt = nullptr;
				const auto caps = PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE;
				for (int i = 0; i < gpu->num_formats; ++i)
				{
					pl_fmt f = gpu->formats[i];
					if (f->type == PL_FMT_UNORM && f->num_components == 4 && f->component_depth[0] == 10 &&
						f->texel_size == 4 && (f->caps & caps) == caps && f->host_bits[0] == 10)
					{ dstFmt = f; break; }
				}
				Assert::IsNotNull(srcFmt); Assert::IsNotNull(dstFmt);
				float texels[16]{};
				for (int i = 0; i < 4; ++i)
				{
					texels[i * 4 + 0] = static_cast<float>(in[0]);
					texels[i * 4 + 1] = static_cast<float>(in[1]);
					texels[i * 4 + 2] = static_cast<float>(in[2]);
					texels[i * 4 + 3] = 1.0f;
				}
				pl_tex_params sp{}; sp.w = sp.h = 2; sp.format = srcFmt; sp.sampleable = true; sp.initial_data = texels;
				pl_tex_params tp{}; tp.w = tp.h = 2; tp.format = dstFmt; tp.renderable = true; tp.host_readable = true;
				pl_tex src = pl_tex_create(gpu, &sp), dst = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(src); Assert::IsNotNull(dst);
				pl_frame img = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(dst);
				out.repr = fullRepr;
				pl_render_params p = pl_render_fast_params;
				p.dither_params = nullptr; p.error_diffusion = nullptr;
				p.hooks = hooks.data(); p.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &img, &out, &p));
				pl_gpu_finish(gpu);
				uint32_t px[4]{};
				pl_tex_transfer_params dl{}; dl.tex = dst; dl.ptr = px;
				Assert::IsTrue(pl_tex_download(gpu, &dl));
				pl_tex_destroy(gpu, &src); pl_tex_destroy(gpu, &dst);
				return { static_cast<double>(px[0] & 1023u) / 1023.0,
					static_cast<double>((px[0] >> 10) & 1023u) / 1023.0,
					static_cast<double>((px[0] >> 20) & 1023u) / 1023.0 };
			};

			// Test saturated bright above-white AND below-black pixels (luma strictly in (0, 1)) at gain 0.8 & 0.95.
			const Rgb pixels[] = { { 1.08, 0.99, 0.55 }, { 1.08, 0.98, 0.45 }, { -0.05, 0.40, 0.70 } };
			double worstNaiveHue = 0.0, worstFullHue = 0.0;
			int naiveViolations = 0;
			for (const double domain : { 0.0, 1.0 })
				for (const double g : { 0.8, 0.95 })
				{
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost", 1.0));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain", g));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", domain));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", 0.08));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "compensation_eotf", 2.2));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "wtw_passthrough", 1.0));
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "btb_passthrough", 1.0));

					for (const Rgb& px : pixels)
					{
						// 1) In-test negative control: naive limited defaults [-60/876, 955/876] on full-range target.
						Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "extended_floor", -60.0 / 876.0));
						Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "extended_ceiling", 955.0 / 876.0));
						const Rgb naivePreClip = renderFullFloat(px);
						const Rgb naiveEncoded10 = renderFull10(px);
						const bool exceededUnit = naivePreClip[0] < -1e-3 || naivePreClip[1] < -1e-3 || naivePreClip[2] < -1e-3 ||
							naivePreClip[0] > 1.0 + 1e-3 || naivePreClip[1] > 1.0 + 1e-3 || naivePreClip[2] > 1.0 + 1e-3;
						const double naiveErr = hueError(px, naiveEncoded10);
						worstNaiveHue = std::max(worstNaiveHue, naiveErr);
						if (exceededUnit && naiveErr > 2.0)
							++naiveViolations;

						// 2) Full-range host bounds [0.0, 1.0]: output stays strictly inside [0, 1] and keeps hue <= 0.5 deg.
						Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "extended_floor", 0.0));
						Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "extended_ceiling", 1.0));
						const Rgb fullPreClip = renderFullFloat(px);
						const Rgb fullEncoded10 = renderFull10(px);
						for (int k = 0; k < 3; ++k)
							Assert::IsTrue(fullPreClip[k] >= -1e-4 && fullPreClip[k] <= 1.0 + 1e-4,
								L"full-range [0, 1] bounds must keep shader output inside [0, 1]");
						const double fullErr = hueError(px, fullEncoded10);
						worstFullHue = std::max(worstFullHue, fullErr);
						Assert::IsTrue(fullErr <= 0.5,
							(L"full-range [0, 1] hue error " + std::to_wstring(fullErr) + L" deg > 0.5 deg").c_str());
					}
				}
			Assert::IsTrue(naiveViolations == 12 && worstNaiveHue > 5.0,
				(L"Negative control failed: naive limited defaults on full-range target must exceed [0, 1] and tilt hue > 2 deg on all 12 cases (got violations=" +
					std::to_wstring(naiveViolations) + L", worstNaiveHue=" + std::to_wstring(worstNaiveHue) + L" deg)").c_str());
			Logger::WriteMessage((L"GEM-LED-EXTRANGE-HOST: naive limited worst hue tilt = " + std::to_wstring(worstNaiveHue) +
				L" deg (12/12 violated [0,1]); full-range [0,1] worst hue error = " + std::to_wstring(worstFullHue) + L" deg\n").c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		// GEM-LED-UP-SMOOTH adversarial 42,875-colour (35^3) synthetic lattice + 3,600-pair fine contour test
		// evaluated DIRECTLY through the real compiled LED compensation.glsl shader on GPU (RGBA32F), verifying
		// all 5 invariants + in-test negative control proving the old unsmoothed y<=1 formula suffers a >350-code cliff.
		TEST_METHOD(LedCompensationUpSmooth41kSyntheticLattice)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
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
			values.outputWidth = 215; values.outputHeight = 200;
			chain.Ensure(values);
			std::vector<const pl_hook*> comp;
			chain.Collect(false, comp);
			Assert::AreEqual(size_t{ 1 }, comp.size(), L"libplacebo rejected LED compensation.glsl");
			const pl_hook* hook = comp[0];

			pl_color_repr fullRepr{};
			fullRepr.sys = PL_COLOR_SYSTEM_RGB;
			fullRepr.levels = PL_COLOR_LEVELS_FULL;
			pl_color_space srgbSpace{};
			srgbSpace.primaries = PL_COLOR_PRIM_BT_709;
			srgbSpace.transfer = PL_COLOR_TRC_SRGB;
			Assert::IsTrue((hook->stages & PL_HOOK_PRE_OUTPUT) != 0 && hook->input == PL_HOOK_SIG_TEX,
				L"LED compensation.glsl must hook PREOUTPUT with PL_HOOK_SIG_TEX");

			struct DispatchGuard
			{
				pl_dispatch dp = nullptr;
				explicit DispatchGuard(pl_dispatch d) : dp(d) {}
				~DispatchGuard() { if (dp) pl_dispatch_destroy(&dp); }
			} dispatchGuard(pl_dispatch_create(fixture.Log(), fixture.Gpu()));
			Assert::IsNotNull(dispatchGuard.dp, L"pl_dispatch_create failed");

			using Rgb = std::array<double, 3>;
			const double eotf = 2.2, knee = 0.08;
			auto spow = [](double v, double e) { return (v < 0.0 ? -1.0 : 1.0) * std::pow(std::abs(v), e); };
			auto curve = [&](double v, double g, double b, double domain)
			{
				v = spow(v, b);
				if (g == 1.0) return v;
				double x = domain > 0.5 ? spow(v, eotf) : v;
				x *= g;
				if (g > 1.0 && x >= 1.0 - knee)
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
			auto setParams = [&](double g, double b, double domain)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost", b));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain", g));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", domain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", knee));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "compensation_eotf", eotf));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "wtw_passthrough", 1.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "btb_passthrough", 1.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "extended_floor", -60.0 / 876.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "extended_ceiling", 955.0 / 876.0));
			};
			// Invoke the compiled PREOUTPUT pl_hook directly on true RGBA32F input & output textures via pl_dispatch
			// so stock libplacebo 7.360's internal FP16 FBO (ULP = 1/1024 near 1.0) does not quantize the lattice
			// before/after the shader, letting the strict 2e-5 / 1e-5 FP32 parity invariants run unmodified.
			auto renderBatch = [&](int w, int h, const std::vector<float>& inRgba) -> std::vector<float>
			{
				pl_gpu gpu = fixture.Gpu();
				pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
					static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				Assert::IsNotNull(fmt, L"No sampleable/renderable/host-readable RGBA32F format");
				struct TempPool
				{
					pl_gpu gpu = nullptr;
					pl_fmt fmt = nullptr;
					std::vector<pl_tex> allocated;
					TempPool(pl_gpu g, pl_fmt f) : gpu(g), fmt(f) {}
					~TempPool() { for (pl_tex t : allocated) if (t) pl_tex_destroy(gpu, &t); }
				} pool(gpu, fmt);
				auto getTex = [](void* priv, int tw, int th) -> pl_tex
				{
					TempPool* p = static_cast<TempPool*>(priv);
					pl_tex_params tp{};
					tp.w = tw; tp.h = th; tp.format = p->fmt;
					tp.sampleable = true; tp.renderable = true; tp.host_readable = true;
					pl_tex t = pl_tex_create(p->gpu, &tp);
					if (t) p->allocated.push_back(t);
					return t;
				};
				pl_tex_params sp{}; sp.w = w; sp.h = h; sp.format = fmt; sp.sampleable = true; sp.initial_data = inRgba.data();
				pl_tex_params tp{}; tp.w = w; tp.h = h; tp.format = fmt; tp.sampleable = true; tp.renderable = true; tp.host_readable = true;
				pl_tex src = pl_tex_create(gpu, &sp), dst = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(src); Assert::IsNotNull(dst);
				pl_dispatch_reset_frame(dispatchGuard.dp);
				if (hook->reset)
					hook->reset(hook->priv);
				pl_hook_params hp{};
				hp.gpu = gpu;
				hp.dispatch = dispatchGuard.dp;
				hp.get_tex = getTex;
				hp.priv = &pool;
				hp.stage = PL_HOOK_PRE_OUTPUT;
				hp.tex = src;
				hp.rect = { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) };
				hp.src_rect = hp.rect;
				hp.dst_rect = { 0, 0, w, h };
				hp.repr = fullRepr;
				hp.orig_repr = &fullRepr;
				hp.color = srgbSpace;
				hp.orig_color = &srgbSpace;
				hp.components = 4;
				pl_hook_res res = hook->hook(hook->priv, &hp);
				Assert::IsFalse(res.failed, L"LED compensation.glsl hook execution failed");
				pl_tex resultTex = nullptr;
				if (res.output == PL_HOOK_SIG_COLOR)
				{
					Assert::IsNotNull(res.sh);
					pl_dispatch_params dp{};
					dp.shader = &res.sh;
					dp.target = dst;
					Assert::IsTrue(pl_dispatch_finish(dispatchGuard.dp, &dp), L"pl_dispatch_finish failed for LED compensation hook");
					resultTex = dst;
				}
				else if (res.output == PL_HOOK_SIG_TEX)
				{
					Assert::IsNotNull(res.tex);
					resultTex = res.tex;
				}
				else
				{
					Assert::Fail(L"Unexpected pl_hook_res output signature from LED compensation.glsl");
				}
				pl_gpu_finish(gpu);
				std::vector<float> outRgba(static_cast<size_t>(w) * h * 4, 0.0f);
				pl_tex_transfer_params dl{}; dl.tex = resultTex; dl.ptr = outRgba.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl));
				pl_tex_destroy(gpu, &src); pl_tex_destroy(gpu, &dst);
				return outRgba;
			};

			// Build 35 x 35 x 35 = 42,875-colour 3D RGB lattice spanning [-0.06, 1.09] packed into 215 x 200 RGBA32F.
			constexpr int kN = 35;
			constexpr int kTotal = kN * kN * kN;   // 42,875 colours
			constexpr int kTexW = 215, kTexH = 200; // 43,000 texels >= 42,875
			std::vector<float> latticeRgba(static_cast<size_t>(kTexW) * kTexH * 4, 0.0f);
			std::vector<Rgb> latticeRgb(kTotal);
			{
				int idx = 0;
				for (int r = 0; r < kN; ++r)
					for (int g = 0; g < kN; ++g)
						for (int b = 0; b < kN; ++b)
						{
							const double rv = -0.06 + (1.15 * r) / (kN - 1);
							const double gv = -0.06 + (1.15 * g) / (kN - 1);
							const double bv = -0.06 + (1.15 * b) / (kN - 1);
							latticeRgb[idx] = { rv, gv, bv };
							latticeRgba[static_cast<size_t>(idx) * 4 + 0] = static_cast<float>(rv);
							latticeRgba[static_cast<size_t>(idx) * 4 + 1] = static_cast<float>(gv);
							latticeRgba[static_cast<size_t>(idx) * 4 + 2] = static_cast<float>(bv);
							latticeRgba[static_cast<size_t>(idx) * 4 + 3] = 1.0f;
							++idx;
						}
			}

			// Invariants 1, 2, 5 across the full 42,875-colour lattice at gain < 1, gain == 1, gain > 1 (both domains).
			double worstInRange = 0.0, worstWtwBtb = 0.0, worstHue = 0.0;
			int inRangeCount = 0, wtwBtbCount = 0, hueCheckedCount = 0;
			for (const double domain : { 0.0, 1.0 })
				for (const double g : { 0.6, 0.8, 1.0, 1.3 })
				{
					setParams(g, 1.0, domain);
					const std::vector<float> out = renderBatch(kTexW, kTexH, latticeRgba);
					for (int i = 0; i < kTotal; ++i)
					{
						const Rgb o = { latticeRgba[static_cast<size_t>(i) * 4 + 0],
							latticeRgba[static_cast<size_t>(i) * 4 + 1], latticeRgba[static_cast<size_t>(i) * 4 + 2] };
						const Rgb got = { out[static_cast<size_t>(i) * 4 + 0],
							out[static_cast<size_t>(i) * 4 + 1], out[static_cast<size_t>(i) * 4 + 2] };
						const bool anyAbove = o[0] > 1.0 || o[1] > 1.0 || o[2] > 1.0;
						const bool anyBelow = o[0] < 0.0 || o[1] < 0.0 || o[2] < 0.0;
						const bool allAbove = o[0] > 1.0 && o[1] > 1.0 && o[2] > 1.0;
						const bool allBelow = o[0] < 0.0 && o[1] < 0.0 && o[2] < 0.0;
						const double y = luma(o);
						if (!anyAbove && !anyBelow)
						{
							++inRangeCount;
							for (int k = 0; k < 3; ++k)
								worstInRange = std::max(worstInRange, std::abs(got[k] - curve(o[k], g, 1.0, domain)));
						}
						else if (y > 1.0 || allAbove || !(y > 0.0) || allBelow)
						{
							++wtwBtbCount;
							for (int k = 0; k < 3; ++k)
								worstWtwBtb = std::max(worstWtwBtb, std::abs(got[k] - o[k]));
						}
						else if (g <= 1.0)
						{
							// Invariant 5: boost off, gain <= 1 (no knee), hue angle error <= 0.5 deg everywhere chromatic.
							const double chroma = std::max({ std::abs(o[0] - y), std::abs(o[1] - y), std::abs(o[2] - y) });
							if (chroma >= 0.02)
							{
								++hueCheckedCount;
								worstHue = std::max(worstHue, hueError(o, got));
							}
						}
					}
				}
			Assert::IsTrue(inRangeCount == 216000 && wtwBtbCount == 5176 && hueCheckedCount == 91368,
				(L"42875-colour lattice coverage mismatch: inRange=" + std::to_wstring(inRangeCount) +
					L" (want 216000), wtwBtb=" + std::to_wstring(wtwBtbCount) +
					L" (want 5176), hueChecked=" + std::to_wstring(hueCheckedCount) + L" (want 91368)").c_str());
			Assert::IsTrue(worstInRange <= 2e-5, (L"Invariant 1 failed: in-range diff " + std::to_wstring(worstInRange)).c_str());
			Assert::IsTrue(worstWtwBtb <= 1e-5, (L"Invariant 2 failed: true WTW/BTB diff " + std::to_wstring(worstWtwBtb)).c_str());
			Assert::IsTrue(worstHue <= 0.5, (L"Invariant 5 failed: worst hue error " + std::to_wstring(worstHue) + L" deg > 0.5 deg").c_str());

			// Invariant 4: temporal continuity across gain = 1.0 - eps to 1.0 + eps across ALL 42,875 colours
			// (both boost off and boost on, both domains). With knee = 0.0 (disabling the g > 1 soft_knee highlight
			// rolloff, which intentionally activates at g > 1 and leaves min(x, 1.0) continuous at g == 1.0), this
			// directly pins GEM-LED-UP-SMOOTH's eff_gain / f branch switch to <= 5e-4 across all 42,875 colours.
			double worstGainJump = 0.0;
			for (const double b : { 1.0, 2.2 / 2.4 })
				for (const double domain : { 0.0, 1.0 })
				{
					setParams(1.0 - 1e-4, b, domain);
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", 0.0));
					const std::vector<float> below = renderBatch(kTexW, kTexH, latticeRgba);
					setParams(1.0 + 1e-4, b, domain);
					Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", 0.0));
					const std::vector<float> above = renderBatch(kTexW, kTexH, latticeRgba);
					for (int i = 0; i < kTotal; ++i)
						for (int k = 0; k < 3; ++k)
							worstGainJump = std::max(worstGainJump,
								std::abs(static_cast<double>(above[static_cast<size_t>(i) * 4 + k]) -
									static_cast<double>(below[static_cast<size_t>(i) * 4 + k])));
				}
			Assert::IsTrue(worstGainJump <= 5e-4,
				(L"Invariant 4 failed: gain 1-eps -> 1+eps pulse " + std::to_wstring(worstGainJump) + L" > 5e-4").c_str());

			// Invariant 3 + In-Test Negative Control: fine +-1 code (1/876) contour crossings across BOTH y = 1.0
			// and max(o) = 1.0 for 3,932 saturated hue pairs rendered directly through the real GPU shader at gain = 0.6!
			constexpr int kGrid = 80;
			constexpr double kEps = 1.0 / 876.0;
			std::vector<Rgb> y1Below, y1Above, m1Below, m1Above;
			for (int ri = 0; ri < kGrid; ++ri)
				for (int bi = 0; bi < kGrid; ++bi)
				{
					const double R = 1.01 + ri * (0.08 / (kGrid - 1));
					const double B = 0.05 + bi * (0.80 / (kGrid - 1));
					const double Gc = (1.0 - 0.2126 * R - 0.0722 * B) / 0.7152;
					if (Gc <= 0.05 || Gc >= 1.05) continue;
					y1Below.push_back({ R, Gc - kEps, B });
					y1Above.push_back({ R, Gc + kEps, B });
					const double G2 = std::min(Gc - 0.05, 0.95);
					if (G2 > 0.05)
					{
						m1Below.push_back({ 1.0 - kEps, G2, B });
						m1Above.push_back({ 1.0 + kEps, G2, B });
					}
				}
			Assert::IsTrue(y1Below.size() > 2500 && m1Below.size() > 2500, L"contour test set must be non-vacuous");
			auto evalContourMaxJump = [&](const std::vector<Rgb>& a, const std::vector<Rgb>& b) -> double
			{
				const int n = static_cast<int>(a.size());
				const int w = 128, h = ((2 * n) + w - 1) / w;
				std::vector<float> rgba(static_cast<size_t>(w) * h * 4, 0.0f);
				for (int i = 0; i < n; ++i)
				{
					for (int k = 0; k < 3; ++k)
					{
						rgba[static_cast<size_t>(2 * i) * 4 + k] = static_cast<float>(a[i][k]);
						rgba[static_cast<size_t>(2 * i + 1) * 4 + k] = static_cast<float>(b[i][k]);
					}
					rgba[static_cast<size_t>(2 * i) * 4 + 3] = 1.0f;
					rgba[static_cast<size_t>(2 * i + 1) * 4 + 3] = 1.0f;
				}
				const std::vector<float> out = renderBatch(w, h, rgba);
				double worst = 0.0;
				for (int i = 0; i < n; ++i)
					for (int k = 0; k < 3; ++k)
						worst = std::max(worst, std::abs(static_cast<double>(out[static_cast<size_t>(2 * i + 1) * 4 + k]) -
							static_cast<double>(out[static_cast<size_t>(2 * i) * 4 + k])));
				return worst;
			};

			// Unsmoothed legacy baseline evaluated on the EXACT SAME y = 1.0 contour pairs (negative control!).
			auto oldUnsmoothed = [&](const Rgb& o, double g, double domain) -> Rgb
			{
				const bool above = o[0] > 1.0 || o[1] > 1.0 || o[2] > 1.0;
				const bool allAbove = o[0] > 1.0 && o[1] > 1.0 && o[2] > 1.0;
				const double y = luma(o);
				if (!above) return { curve(o[0], g, 1.0, domain), curve(o[1], g, 1.0, domain), curve(o[2], g, 1.0, domain) };
				if (y > 1.0 || allAbove) return o;
				double t = 1.0;
				for (int k = 0; k < 3; ++k)
					if (o[k] > 1.0) t = std::min(t, (1.0 - y) / std::max(o[k] - y, 1e-6));
				t = std::min(std::max(t, 0.0), 1.0);
				const double f = curve(1.0, g, 1.0, domain);
				Rgb r{};
				for (int k = 0; k < 3; ++k)
				{
					const double pk = std::min(std::max(y + t * (o[k] - y), 0.0), 1.0);
					r[k] = curve(pk, g, 1.0, domain) + (o[k] - pk) * f;
				}
				return r;
			};

			for (const double domain : { 0.0, 1.0 })
			{
				setParams(0.6, 1.0, domain);
				const double gpuY1Jump = evalContourMaxJump(y1Below, y1Above);
				const double gpuM1Jump = evalContourMaxJump(m1Below, m1Above);
				double naiveY1Jump = 0.0;
				for (size_t i = 0; i < y1Below.size(); ++i)
				{
					const Rgb u0 = oldUnsmoothed(y1Below[i], 0.6, domain);
					const Rgb u1 = oldUnsmoothed(y1Above[i], 0.6, domain);
					for (int k = 0; k < 3; ++k)
						naiveY1Jump = std::max(naiveY1Jump, std::abs(u1[k] - u0[k]));
				}
				// In-test negative control: legacy unsmoothed baseline MUST violate continuity with >0.20 normalized (>175-code) cliff!
				Assert::IsTrue(naiveY1Jump > 0.20,
					(L"Negative control failed: unsmoothed y=1 cliff must exceed 0.20 normalized (got " + std::to_wstring(naiveY1Jump) + L")").c_str());
				// Guarded smooth GPU shader MUST be continuous across both y=1.0 and max(o)=1.0!
				Assert::IsTrue(gpuY1Jump <= 0.015,
					(L"Invariant 3 failed at y=1 contour (domain=" + std::to_wstring(static_cast<int>(domain)) +
						L"): GPU jump " + std::to_wstring(gpuY1Jump) + L" > 0.015 (unsmoothed cliff=" + std::to_wstring(naiveY1Jump) + L")").c_str());
				Assert::IsTrue(gpuM1Jump <= 0.005,
					(L"Invariant 3 failed at max(o)=1 contour (domain=" + std::to_wstring(static_cast<int>(domain)) +
						L"): GPU jump " + std::to_wstring(gpuM1Jump) + L" > 0.005").c_str());
			}
			Logger::WriteMessage((L"GEM-LED-UP-SMOOTH 42875-colour lattice: inRange=" + std::to_wstring(worstInRange) +
				L", wtwBtb=" + std::to_wstring(worstWtwBtb) + L", gain1Jump=" + std::to_wstring(worstGainJump) +
				L", worstHue=" + std::to_wstring(worstHue) + L" deg\n").c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}
	};
}
