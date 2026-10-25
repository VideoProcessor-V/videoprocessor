// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(ExtendedRangeHookTests)
	{
	public:


		// Stage 10: when preserve_extended_range is active, and why not.
		TEST_METHOD(ExtendedRangeDecision)
		{
			using namespace ExtendedRange;
			auto space = [](enum pl_color_primaries p, enum pl_color_transfer t)
			{
				pl_color_space c{};
				c.primaries = p;
				c.transfer = t;
				return c;
			};
			const pl_color_space source = space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22);
			const pl_color_space p3 = space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_GAMMA24);
			Assert::IsTrue(Decide(false, source, p3, false).reason == Reason::Off);
			const Decision on = Decide(true, source, p3, false);
			Assert::IsTrue(on.Active());
			Assert::IsTrue(on.convertGamma && on.convertPrimaries);
			Assert::AreEqual(2.2f, on.sourceGamma);
			Assert::AreEqual(2.4f, on.outputGamma);
			// White stays white: each row of the BT.709 -> P3 matrix sums to 1.
			for (int row = 0; row < 3; ++row)
				Assert::AreEqual(1.0, static_cast<double>(on.matrix[row] + on.matrix[3 + row] + on.matrix[6 + row]), 1e-5);
			Assert::IsTrue(on.matrix[0] < 1.0f && on.matrix[0] > 0.7f);   // red: 0.8225
			const Decision same = Decide(true, source, space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22), false);
			Assert::IsTrue(same.Active() && !same.convertGamma && !same.convertPrimaries);
			Assert::IsTrue(Decide(true, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA22),
				space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA24), false).Active());
			Assert::IsTrue(Decide(true, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ), p3, false).reason == Reason::HdrSource);
			Assert::IsTrue(Decide(true, source, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ), false).reason == Reason::HdrOutput);
			Assert::IsTrue(Decide(true, source, p3, true).reason == Reason::CalibrationLut);
			// The Stage 10 eligibility (the audit 2026-10-16 extensions off) ...
			const ExtendedRange::Settings stage10{ false, false, false, false };
			Assert::IsTrue(Decide(true, stage10, space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886), p3, nullptr, PL_LUT_UNKNOWN).reason == Reason::SourceTransfer);
			Assert::IsTrue(Decide(true, stage10, source, space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_SRGB), nullptr, PL_LUT_UNKNOWN).reason == Reason::OutputTransfer);
			Assert::IsTrue(Decide(true, stage10, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA22), p3, nullptr, PL_LUT_UNKNOWN).reason == Reason::SourcePrimaries);
			Assert::IsTrue(Decide(true, stage10, source, space(PL_COLOR_PRIM_DCI_P3, PL_COLOR_TRC_GAMMA24), nullptr, PL_LUT_UNKNOWN).reason == Reason::OutputPrimaries);
			// ... and with them (the default): all four carried.
			Assert::IsTrue(Decide(true, space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886), p3, false).Active());
			Assert::IsTrue(Decide(true, source, space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_SRGB), false).Active());
			Assert::IsTrue(Decide(true, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA22), p3, false).Active());
			Assert::IsTrue(Decide(true, source, space(PL_COLOR_PRIM_DCI_P3, PL_COLOR_TRC_GAMMA24), false).Active());
			// Prepare: nothing for an inactive decision; for an active one the
			// source is declared in the output's space, the restore goes first
			// (first at SCALED) and the carry last (last at RGB, after NLS).
			Hooks hooks;
			std::vector<const pl_hook*> list = { nullptr };
			pl_color_space declared = source;
			Prepare(Decide(false, source, p3, false), hooks, declared, p3, list);
			Assert::AreEqual(size_t(1), list.size());
			Assert::IsTrue(declared.primaries == PL_COLOR_PRIM_BT_709);
			Prepare(on, hooks, declared, p3, list);
			Assert::AreEqual(size_t(3), list.size());
			Assert::IsTrue(list[0] == hooks.Restore(true) && list[2] == hooks.Carry());
			Assert::IsTrue(hooks.Carry()->stages == PL_HOOK_RGB && hooks.Restore()->stages == PL_HOOK_SCALED);
			Assert::IsTrue(declared.primaries == PL_COLOR_PRIM_DISPLAY_P3 && declared.transfer == PL_COLOR_TRC_GAMMA24);
			Assert::IsTrue(hooks.Owns(hooks.Carry()->signature) && !hooks.Owns(1));
		}

		// Stage 10 (probe tables, docs/VP-btb-wtw-investigation.md): BTB and
		// WTW greys survive 1:1, sigmoid upscaling and linear-light
		// downscaling with the switch on; off is libplacebo's own path.
		TEST_METHOD(ExtendedRangeKeepsBtbAndWtwThroughScaling)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log;
			fixture.CaptureLog(&log);
			const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
			const Planes greys = Greys(codes);
			std::string report;
			auto run = [&](const char* name, Case c)
			{
				const Result r = Render(fixture.Gpu(), fixture.Renderer(), greys, c);
				Assert::IsTrue(r.ok, ToWide(std::string(name) + ": " + r.error).c_str());
				report += std::string(name) + ": " + Join(Columns(r, static_cast<int>(codes.size()), c.scale)) + "\n";
				return r;
			};
			{ Case c; c.on = false; c.scale = 1.5; run("off up 1.5", c); }
			for (double scale : { 1.0, 1.5, 0.5 })
				for (bool owner : { false, true })
				{
					if (owner && scale != 1.5) continue;
					Case c;
					c.scale = scale;
					c.ownerScaling = owner;
					const Result r = run(owner ? "on up 1.5 ewa_lanczossharp" : scale == 1.0 ? "on 1:1" : scale > 1.0 ? "on up 1.5" : "on down 0.5", c);
					Assert::IsTrue(r.decision.Active() && r.restores > 0, ToWide("the hooks did not run\n" + report).c_str());
					for (size_t i = 0; i < codes.size(); ++i)
					{
						const int x = static_cast<int>((i * 32 + 16) * scale);
						for (int ch = 0; ch < 3; ++ch)
							Assert::IsTrue(std::abs(r.Channel(x, r.height / 2, ch) - codes[i]) <= 1,
								ToWide("709->P3 grey " + std::to_string(codes[i]) + " not kept\n" + report).c_str());
					}
				}
			for (double scale : { 1.0, 1.5 })
			{
				Case c;
				c.scale = scale;
				c.outputTransfer = PL_COLOR_TRC_GAMMA24;
				const auto got = Columns(run(scale == 1.0 ? "on 2.2->2.4 1:1" : "on 2.2->2.4 up 1.5", c),
					static_cast<int>(codes.size()), scale);
				for (size_t i = 0; i < codes.size(); ++i)
				{
					const int ideal = Ideal(codes[i], 2.2, 2.4);
					Assert::IsTrue(ideal < 4 ? got[i] <= 4 : std::abs(got[i] - ideal) <= 1,
						ToWide("2.2->2.4 grey " + std::to_string(codes[i]) + " expected " + std::to_string(ideal) + "\n" + report).c_str());
				}
			}
			// Audit stage10-6d: a user MAIN shader that writes alpha 1 must not
			// change the picture. Since 10b alpha carries nothing: every code,
			// below black and above white included, is kept.
			{
				// A hook at the RGB stage after the carry that writes alpha 1,
				// as a user shader returning vec4(rgb, 1.0) would.
				pl_hook writer{};
				writer.stages = PL_HOOK_RGB;
				writer.input = PL_HOOK_SIG_COLOR;
				writer.hook = [](void*, const pl_hook_params* hp)
				{
					pl_custom_shader custom{};
					custom.description = "test: user shader writing alpha";
					custom.input = PL_SHADER_SIG_COLOR;
					custom.output = PL_SHADER_SIG_COLOR;
					custom.body = "color.a = 1.0;\n";
					pl_hook_res res{};
					res.failed = !pl_shader_custom(hp->sh, &custom);
					res.output = PL_HOOK_SIG_COLOR;
					res.sh = hp->sh;
					res.repr = hp->repr;
					res.color = hp->color;
					res.components = 4;
					res.rect = hp->rect;
					return res;
				};
				Case c;
				c.extraHook = &writer;
				const auto got = Columns(run("on 1:1, alpha-writing hook", c), static_cast<int>(codes.size()), 1.0);
				for (size_t i = 0; i < codes.size(); ++i)
					Assert::IsTrue(std::abs(got[i] - codes[i]) <= 2, ToWide("alpha writer changed the picture\n" + report).c_str());
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLog(log);
		}

		// Stage 10: in-range content is scaled as without the switch (planner:
		// max 1 code, mean <= 0.03 at up 1.5 and down 0.5; 10a measured <= 0.01.
		// 10b: max 1 code kept, mean up to 0.083 measured - libplacebo stores the
		// scaled picture in its half-float texture before the texture-form
		// restore, about 1/4 code; accepted by the planner 2026-10-12),
		// also with a gamma change, and in-gamut BT.709 colours come out as
		// libplacebo's own conversion. With ewa_lanczossharp the polar
		// sampler's four-channel path rounds differently (max 1 code, mean
		// 0.034 measured): bounded at 0.05.
		TEST_METHOD(ExtendedRangeKeepsTodaysScalingAndColours)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string report;
			const Planes edges = Edges();
			auto compare = [&](const char* name, Case c)
			{
				Case off = c;
				off.on = false;
				const Result a = Render(fixture.Gpu(), fixture.Renderer(), edges, off);
				const Result b = Render(fixture.Gpu(), fixture.Renderer(), edges, c);
				Assert::IsTrue(a.ok && b.ok, ToWide(std::string(name) + ": render failed").c_str());
				const Difference d = Compare(a, b);
				char line[160];
				std::snprintf(line, sizeof(line), "%s: max %d, mean %.4f\n", name, d.max, d.mean);
				report += line;
				return d;
			};
			bool fail = false;
			for (enum pl_color_primaries prim : { PL_COLOR_PRIM_BT_709, PL_COLOR_PRIM_DISPLAY_P3 })
			for (double scale : { 1.5, 0.5, 0.75 })
			{
				Case c;
				c.scale = scale;
				c.outputPrimaries = prim;
				const Difference d = compare((std::string(prim == PL_COLOR_PRIM_BT_709 ? "709 " : "P3 ") +
					(scale == 1.5 ? "edges up 1.5" : scale == 0.5 ? "edges down 0.5" : "edges down 0.75")).c_str(), c);
				fail |= !(d.max <= 1 && d.mean <= 0.1);   // 10b: see the note above
			}
			{
				Case c;
				c.scale = 1.5;
				c.ownerScaling = true;
				const Difference d = compare("edges up 1.5 ewa_lanczossharp", c);
				fail |= !(d.max <= 1 && d.mean <= 0.1);   // 10b: see the note above
			}
			// A gamma change: the carry re-encodes to the output gamma, so
			// libplacebo's linear light is the same as without the switch.
			for (double scale : { 1.5, 0.5 })
			{
				Case c;
				c.scale = scale;
				c.outputTransfer = PL_COLOR_TRC_GAMMA24;
				const Difference d = compare(scale == 1.5 ? "edges up 1.5, 2.2->2.4" : "edges down 0.5, 2.2->2.4", c);
				fail |= !(d.max <= 1 && d.mean <= 0.1);   // 10b: see the note above
			}
			// In-gamut BT.709 colours, 1:1 into P3 with a gamma change.
			const std::vector<Patch> patches = { { 502, 512, 512 }, { 560, 440, 600 }, { 400, 450, 700 },
				{ 600, 350, 450 }, { 300, 700, 480 }, { 700, 560, 380 }, { 250, 560, 560 } };
			const Planes colours = Patches(patches);
			for (enum pl_color_transfer output : { PL_COLOR_TRC_GAMMA22, PL_COLOR_TRC_GAMMA24 })
			{
				Case c;
				c.outputTransfer = output;
				Case off = c;
				off.on = false;
				const Result a = Render(fixture.Gpu(), fixture.Renderer(), colours, off);
				const Result b = Render(fixture.Gpu(), fixture.Renderer(), colours, c);
				Assert::IsTrue(a.ok && b.ok);
				int worst = 0;
				for (size_t i = 0; i < patches.size(); ++i)
				{
					const int x = static_cast<int>(i * 32 + 16), y = a.height / 2;
					report += "colour " + std::to_string(i) + (output == PL_COLOR_TRC_GAMMA22 ? " 2.2" : " 2.4") + ": off ";
					for (int ch = 0; ch < 3; ++ch) report += std::to_string(a.Channel(x, y, ch)) + (ch < 2 ? "," : "");
					report += " on ";
					for (int ch = 0; ch < 3; ++ch)
					{
						report += std::to_string(b.Channel(x, y, ch)) + (ch < 2 ? "," : "");
						worst = std::max(worst, std::abs(a.Channel(x, y, ch) - b.Channel(x, y, ch)));
					}
					report += "\n";
				}
				fail |= worst > 2;
			}
			Assert::IsFalse(fail, ToWide(report).c_str());
			Logger::WriteMessage(report.c_str());
		}

		// Delta 5 (D5-3): the match tolerance means the same for full-range
		// sources. B is A moved by (8, 4) (whole pixels at the matcher's half
		// resolution, so no interpolation error) with every luma sample 3.2 8-bit codes
		// brighter: over the default tolerance (3 codes) as limited-range codes,
		// within it as full-range luma (3.2 x 219 / 255 = 2.75
		// limited-equivalent codes).
		// Stage 10: the first frame with the carry after frames without it is
		// right (scaling, light domain, Mitchell). After the SSIM user shader
		// too: AdaptiveDownscaleBlendsEdgesTextureAndGrain.
		TEST_METHOD(ExtendedRangeFirstFrameAfterOtherFrames)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
			std::string report;
			auto run = [&](const char* name, Case c)
			{
				const Result r = Render(gpu, fixture.Renderer(), Greys(codes, 64), c);
				Assert::IsTrue(r.ok);
				const auto got = Columns(r, static_cast<int>(codes.size()), c.scale, 64);
				report += std::string(name) + ": " + Join(got) + "\n";
				return got;
			};
			Case off;
			off.on = false;
			off.scale = 0.5;
			Case on;
			on.scale = 0.5;
			run("off down", off);
			run("on down (1st)", on);
			run("on down (2nd)", on);
			Case offUp = off;
			offUp.scale = 1.5;
			run("off up", offUp);
			run("on down after off up", on);
			Case gammaOff = off;
			gammaOff.linear = false;
			run("off down, gamma light", gammaOff);
			run("on down after gamma light", on);
			Case mitchellOff = off;
			mitchellOff.mitchell = true;
			run("off down, mitchell", mitchellOff);
			Case mitchellOn = on;
			mitchellOn.mitchell = true;
			run("on down mitchell after off mitchell", mitchellOn);
			Logger::WriteMessage(report.c_str());
			for (const std::string& row : { std::string("on down (1st): "), std::string("on down after off up: "),
				std::string("on down after gamma light: "), std::string("on down mitchell after off mitchell: ") })
				Assert::IsTrue(report.find(row + "4 16 40 64 100 502 900 940") != std::string::npos,
					ExtendedRangeGpuProbe::ToWide("a first carry frame is wrong\n" + report).c_str());
		}

		TEST_METHOD(ExtendedRangeBt1886Carry)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErTransferCarry(fixture, PL_COLOR_TRC_BT_1886, { PL_COLOR_TRC_GAMMA24, PL_COLOR_TRC_BT_1886, PL_COLOR_TRC_GAMMA22 }, report);
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		TEST_METHOD(ExtendedRangeSrgbCarry)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErTransferCarry(fixture, PL_COLOR_TRC_SRGB, { PL_COLOR_TRC_GAMMA22, PL_COLOR_TRC_SRGB, PL_COLOR_TRC_BT_1886 }, report);
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// 2: an SDR BT.2020 source on a BT.709 output keeps libplacebo's gamut
		// mapping: in-range saturated colours (out of BT.709's gamut) as
		// switch-off (<= 1 code per channel), greys incl. BTB/WTW kept (<= 1),
		// a WTW red above 940 and not below switch-off, a BTB blue below 64 and
		// not above switch-off; no SCALED restore, the late restore ran.
		TEST_METHOD(ExtendedRangeWideGamutLateRestore)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			const double kr = 0.2627, kb = 0.0593;
			const std::vector<Patch> patches = { ErPatch(0.1, 0.9, 0.1, kr, kb), ErPatch(0.9, 0.1, 0.1, kr, kb),
				ErPatch(0.1, 0.1, 0.9, kr, kb), ErPatch(0.6, 0.6, 0.6, kr, kb),
				ErPatch(1.06, 0.5, 0.5, kr, kb), ErPatch(0.5, 0.5, -0.06, kr, kb) };
			const Planes colours = Patches(patches);
			const Planes greys = Greys(ErCodes());
			for (double scale : { 1.0, 1.5 })
			{
				ErMoreCase m;
				m.c.sourcePrimaries = PL_COLOR_PRIM_BT_2020;
				m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
				m.c.scale = scale;
				m.bt2020Matrix = true;
				ErMoreCase off = m;
				off.c.on = false;
				const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), colours, m);
				const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), colours, off);
				Assert::IsTrue(on.r.ok && ref.r.ok);
				const ExtendedRange::Decision& d = on.r.decision;
				Assert::IsTrue(d.Active() && d.libplaceboMapsPrimaries && d.LateRestore() && !d.ScaledRestore() && on.r.restores > 0,
					ToWide(ExtendedRange::Describe(d)).c_str());
				const int y = on.r.height / 2;
				for (size_t i = 0; i < patches.size(); ++i)
				{
					const int x = static_cast<int>((i * 32 + 16) * scale);
					report += "x" + std::to_string(scale) + " patch " + std::to_string(i) + ": on ";
					for (int ch = 0; ch < 3; ++ch) report += std::to_string(on.r.Channel(x, y, ch)) + (ch < 2 ? "," : "");
					report += " off ";
					for (int ch = 0; ch < 3; ++ch) report += std::to_string(ref.r.Channel(x, y, ch)) + (ch < 2 ? "," : "");
					report += "\n";
				}
				for (size_t i = 0; i < 4; ++i)
				{
					const int x = static_cast<int>((i * 32 + 16) * scale);
					for (int ch = 0; ch < 3; ++ch)
						Assert::IsTrue(std::abs(on.r.Channel(x, y, ch) - ref.r.Channel(x, y, ch)) <= 1,
							ToWide("in-range colour changed\n" + report).c_str());
				}
				const int xr = static_cast<int>((4 * 32 + 16) * scale), xb = static_cast<int>((5 * 32 + 16) * scale);
				Assert::IsTrue(on.r.Channel(xr, y, 0) > 940 && on.r.Channel(xr, y, 0) >= ref.r.Channel(xr, y, 0),
					ToWide("WTW red not restored\n" + report).c_str());
				Assert::IsTrue(on.r.Channel(xb, y, 2) < 64 && on.r.Channel(xb, y, 2) <= ref.r.Channel(xb, y, 2),
					ToWide("BTB blue not restored\n" + report).c_str());
				const ErMoreResult g = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
				Assert::IsTrue(g.r.ok);
				const auto got = Columns(g.r, static_cast<int>(ErCodes().size()), scale);
				report += "greys x" + std::to_string(scale) + ": " + Join(got) + "\n";
				for (size_t i = 0; i < got.size(); ++i)
					Assert::IsTrue(std::abs(got[i] - ErCodes()[i]) <= 1, ToWide("grey not kept\n" + report).c_str());
				// Switch off: the old eligibility refuses this source.
				ErMoreCase old = m;
				old.settings.wideSourcePrimaries = false;
				Assert::IsTrue(ErRender(fixture.Gpu(), fixture.Renderer(), greys, old).r.decision.reason ==
					ExtendedRange::Reason::SourcePrimaries);
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// 2: a contained gamut (BT.601 EBU SDR on a BT.2020 output; Display P3
		// is not contained for libplacebo: its red is ~0.0017 outside) converts in
		// the carry: greys incl. BTB/WTW kept (<= 1), in-range colours as
		// switch-off (<= 2, as the BT.709 -> P3 colours test).
		TEST_METHOD(ExtendedRangeContainedGamut)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			const std::vector<Patch> patches = { { 502, 512, 512 }, { 560, 440, 600 }, { 400, 450, 700 },
				{ 600, 350, 450 }, { 300, 700, 480 }, { 700, 560, 380 } };
			const Planes colours = Patches(patches);
			const Planes greys = Greys(ErCodes());
			for (double scale : { 1.0, 1.5 })
			{
				ErMoreCase m;
				m.c.sourcePrimaries = PL_COLOR_PRIM_BT_601_625;
				m.c.outputPrimaries = PL_COLOR_PRIM_BT_2020;
				m.c.scale = scale;
				ErMoreCase off = m;
				off.c.on = false;
				const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), colours, m);
				const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), colours, off);
				Assert::IsTrue(on.r.ok && ref.r.ok);
				const ExtendedRange::Decision& d = on.r.decision;
				Assert::IsTrue(d.Active() && d.convertPrimaries && !d.libplaceboMapsPrimaries && d.ScaledRestore() && !d.LateRestore(),
					ToWide(ExtendedRange::Describe(d)).c_str());
				int worst = 0;
				const int y = on.r.height / 2;
				for (size_t i = 0; i < patches.size(); ++i)
					for (int ch = 0; ch < 3; ++ch)
					{
						const int x = static_cast<int>((i * 32 + 16) * scale);
						worst = std::max(worst, std::abs(on.r.Channel(x, y, ch) - ref.r.Channel(x, y, ch)));
					}
				report += "x" + std::to_string(scale) + ": colours worst " + std::to_string(worst) + "\n";
				Assert::IsTrue(worst <= 2, ToWide("in-range colours changed\n" + report).c_str());
				const ErMoreResult g = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
				Assert::IsTrue(g.r.ok);
				const auto got = Columns(g.r, static_cast<int>(ErCodes().size()), scale);
				report += "greys: " + Join(got) + "\n";
				for (size_t i = 0; i < got.size(); ++i)
					Assert::IsTrue(std::abs(got[i] - ErCodes()[i]) <= 1, ToWide("grey not kept\n" + report).c_str());
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// 3: a calibration LUT. Identity 17^3 LUT: the whole picture (BTB/WTW
		// greys) as without a LUT (max 1 code), in-range edges as switch-off
		// with the LUT (max 1, mean 0.1). Affine 17^3 LUT with cross-talk: every
		// grey channel (BTB/WTW included) = A v + t extrapolated (<= 1 code);
		// switch-off clamps there (logged). The SCALED and late restores ran.
		TEST_METHOD(ExtendedRangeLutLateRestore)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErLut identity;
			identity.Build(17, 0x4552494431370001ull);
			ErLut affine;
			const float a[3][3] = { { 0.90f, 0.05f, 0.01f }, { 0.03f, 0.92f, 0.02f }, { 0.02f, 0.04f, 0.88f } };
			const float t[3] = { 0.02f, 0.01f, 0.03f };
			std::memcpy(affine.a, a, sizeof(a));
			std::memcpy(affine.t, t, sizeof(t));
			affine.Build(17, 0x4552414631370001ull);
			const std::vector<int>& codes = ErCodes();
			const Planes greys = Greys(codes);
			const Planes edges = Edges();
			for (double scale : { 1.0, 1.5 })
			{
				ErMoreCase m;
				m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
				m.c.scale = scale;
				// Identity LUT vs no LUT, both on.
				ErMoreCase withLut = m;
				withLut.lut = &identity.lut;
				const ErMoreResult noLut = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
				const ErMoreResult lut = ErRender(fixture.Gpu(), fixture.Renderer(), greys, withLut);
				Assert::IsTrue(noLut.r.ok && lut.r.ok);
				Assert::IsTrue(lut.r.decision.Active() && lut.r.decision.lut == &identity.lut &&
					lut.r.decision.LateRestore() && lut.r.decision.ScaledRestore() && lut.r.restores >= 2,
					ToWide(ExtendedRange::Describe(lut.r.decision)).c_str());
				const Difference d = Compare(noLut.r, lut.r);
				report += "identity LUT x" + std::to_string(scale) + ": " + Join(Columns(lut.r, static_cast<int>(codes.size()), scale)) +
					" (max " + std::to_string(d.max) + ")\n";
				Assert::IsTrue(d.max <= 1, ToWide("identity LUT changed the picture\n" + report).c_str());
				// GPU run 2026-10-16 (360): in-range pixels differed by max 1, mean
				// 0.39 up 1.5: before a texture PRE_OUTPUT hook libplacebo stores
				// the finished picture in its half-float texture (~1/2 code near
				// white), which switch-off without such a hook never does (an
				// inline form is rejected by libplacebo). In VP the LED hooks are
				// texture PRE_OUTPUT hooks already, so the like-for-like check has
				// one in both renders: max 1, mean 0.25 (measured 0.17: what is left
				// is the texture-form SCALED restore's half-float cost, the accepted
				// 1/4 code of ExtendedRangeTransferCarry). Without it the 360 cost is
				// bounded at max 1, mean 0.5 (377 + output_fbo_float32 stores it in
				// float32). Ringing overshoot outside 64..940 is extrapolated by the
				// carry and clamped by switch-off's LUT, so only pixels switch-off
				// leaves strictly inside 64..940 are compared.
				for (int withHook = 1; withHook >= 0; --withHook)
				{
					ErMoreCase lutOn = withLut, lutOff = withLut, noLutOff = m;
					lutOff.c.on = false;
					noLutOff.c.on = false;
					if (withHook)
						lutOn.preOutputHook = lutOff.preOutputHook = noLutOff.preOutputHook = ErTexturePreOutputHook();
					const ErMoreResult ea = ErRender(fixture.Gpu(), fixture.Renderer(), edges, lutOff);
					const ErMoreResult eb = ErRender(fixture.Gpu(), fixture.Renderer(), edges, lutOn);
					const ErMoreResult en = ErRender(fixture.Gpu(), fixture.Renderer(), edges, noLutOff);
					Assert::IsTrue(ea.r.ok && eb.r.ok && en.r.ok);
					int insideMax = 0, inside = 0;
					double insideSum = 0.0;
					for (size_t i = 0; i < ea.r.pixels.size(); ++i)
						for (int ch = 0; ch < 3; ++ch)
						{
							const int a = static_cast<int>((ea.r.pixels[i] >> (10 * ch)) & 1023u);
							const int b = static_cast<int>((eb.r.pixels[i] >> (10 * ch)) & 1023u);
							if (a <= 64 || a >= 940)
								continue;
							insideMax = std::max(insideMax, std::abs(a - b));
							insideSum += std::abs(a - b);
							++inside;
						}
					const double insideMean = inside ? insideSum / inside : 0.0;
					const Difference e = Compare(en.r, eb.r);
					const double bound = withHook ? 0.25 : 0.5;
					report += std::string("identity LUT edges x") + std::to_string(scale) + (withHook ? " (texture PRE_OUTPUT hook)" : " (no other hook)") +
						": in range vs LUT off max " + std::to_string(insideMax) + " mean " + std::to_string(insideMean) +
						"; vs no LUT off max " + std::to_string(e.max) + " mean " + std::to_string(e.mean) + "\n";
					Assert::IsTrue(insideMax <= 1 && insideMean <= bound, ToWide("in-range edges changed with the LUT\n" + report).c_str());
					Assert::IsTrue(e.max <= 1 && e.mean <= bound, ToWide("identity LUT on differs from no LUT off\n" + report).c_str());
				}
				// Affine LUT: the extrapolation is exact for an affine LUT.
				ErMoreCase withAffine = m;
				withAffine.lut = &affine.lut;
				ErMoreCase offAffine = withAffine;
				offAffine.c.on = false;
				const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), greys, withAffine);
				const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), greys, offAffine);
				Assert::IsTrue(on.r.ok && ref.r.ok);
				const int y = on.r.height / 2;
				for (size_t i = 0; i < codes.size(); ++i)
				{
					const int x = static_cast<int>((i * 32 + 16) * scale);
					const double v = (codes[i] - 64) / 876.0;
					report += "affine x" + std::to_string(scale) + " grey " + std::to_string(codes[i]) + ": on ";
					for (int ch = 0; ch < 3; ++ch) report += std::to_string(on.r.Channel(x, y, ch)) + (ch < 2 ? "," : "");
					report += " off ";
					for (int ch = 0; ch < 3; ++ch) report += std::to_string(ref.r.Channel(x, y, ch)) + (ch < 2 ? "," : "");
					report += "\n";
					for (int ch = 0; ch < 3; ++ch)
					{
						const int expected = static_cast<int>(std::lround(64 + 876 * affine.Grey(v, ch)));
						Assert::IsTrue(std::abs(on.r.Channel(x, y, ch) - expected) <= 1,
							ToWide("affine LUT grey " + std::to_string(codes[i]) + " channel " + std::to_string(ch) +
								" expected " + std::to_string(expected) + "\n" + report).c_str());
					}
				}
				// Switch off: the old eligibility (CalibrationLut).
				ErMoreCase old = withAffine;
				old.settings.calibrationLut = false;
				Assert::IsTrue(ErRender(fixture.Gpu(), fixture.Renderer(), greys, old).r.decision.reason ==
					ExtendedRange::Reason::CalibrationLut);
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// 2 + 3: a BT.2020 SDR source, BT.709 output and a LUT: the carry clips
		// the gamut (logged), the LUT carry applies. Identity LUT: greys incl.
		// BTB/WTW kept (<= 1); low-saturation in-gamut colours as switch-off
		// without a LUT (<= 2).
		TEST_METHOD(ExtendedRangeLutGamutClip)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErLut identity;
			identity.Build(17, 0x4552494431370002ull);
			const double kr = 0.2627, kb = 0.0593;
			const std::vector<Patch> patches = { ErPatch(0.5, 0.45, 0.4, kr, kb), ErPatch(0.4, 0.45, 0.5, kr, kb),
				ErPatch(0.45, 0.5, 0.42, kr, kb) };
			const Planes colours = Patches(patches);
			const Planes greys = Greys(ErCodes());
			ErMoreCase m;
			m.c.sourcePrimaries = PL_COLOR_PRIM_BT_2020;
			m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
			m.bt2020Matrix = true;
			m.lut = &identity.lut;
			const ErMoreResult g = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
			Assert::IsTrue(g.r.ok);
			const ExtendedRange::Decision& d = g.r.decision;
			Assert::IsTrue(d.Active() && d.gamutClip && d.convertPrimaries && !d.libplaceboMapsPrimaries && d.LateRestore() &&
				d.ScaledRestore(), ToWide(ExtendedRange::Describe(d)).c_str());
			const auto got = Columns(g.r, static_cast<int>(ErCodes().size()), 1.0);
			report += "greys: " + Join(got) + "\n";
			for (size_t i = 0; i < got.size(); ++i)
				Assert::IsTrue(std::abs(got[i] - ErCodes()[i]) <= 1, ToWide("grey not kept\n" + report).c_str());
			ErMoreCase off = m;
			off.c.on = false;
			off.lut = nullptr;
			const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), colours, m);
			const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), colours, off);
			Assert::IsTrue(on.r.ok && ref.r.ok);
			int worst = 0;
			for (size_t i = 0; i < patches.size(); ++i)
				for (int ch = 0; ch < 3; ++ch)
					worst = std::max(worst, std::abs(on.r.Channel(static_cast<int>(i * 32 + 16), on.r.height / 2, ch) -
						ref.r.Channel(static_cast<int>(i * 32 + 16), ref.r.height / 2, ch)));
			report += "low-saturation colours worst " + std::to_string(worst) + "\n";
			Assert::IsTrue(worst <= 2, ToWide("in-gamut colours changed\n" + report).c_str());
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// 4: limited-range PQ into an identical PQ output (identity tone
		// mapping): every grey code 4..1019 kept (<= 1 at 1:1 and up 1.5, <= 2
		// down 0.5, where libplacebo linearises and switch-off clips BTB - logged).
		// Tone-mapped cases stay off: HdrSource (SDR output), HdrMapped (other
		// HDR metadata), and with hdrPassthrough off.
		TEST_METHOD(ExtendedRangeHdrPassthrough)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			const std::vector<int>& codes = ErCodes();
			const Planes greys = Greys(codes);
			for (double scale : { 1.0, 1.5, 0.5 })
			{
				ErMoreCase m;
				m.c.hdrSource = true;
				m.c.scale = scale;
				m.hdrSameOutput = true;
				// GPU run 2026-10-16: with libplacebo's default peak detection even
				// equal colour spaces are tone-mapped (502 -> 371, 1019 -> 723, on
				// = off): identity needs peak detection off.
				m.peakDetection = false;
				ErMoreCase off = m;
				off.c.on = false;
				const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
				const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), greys, off);
				Assert::IsTrue(on.r.ok && ref.r.ok);
				Assert::IsTrue(on.r.decision.Active() && on.r.decision.hdrPassthrough && on.r.restores > 0,
					ToWide(ExtendedRange::Describe(on.r.decision)).c_str());
				const auto got = Columns(on.r, static_cast<int>(codes.size()), scale);
				const auto offCodes = Columns(ref.r, static_cast<int>(codes.size()), scale);
				report += "PQ x" + std::to_string(scale) + ": on " + Join(got) + " | off " + Join(offCodes) + "\n";
				const int tolerance = scale < 1.0 ? 2 : 1;
				for (size_t i = 0; i < codes.size(); ++i)
				{
					Assert::IsTrue(std::abs(got[i] - codes[i]) <= tolerance, ToWide("PQ code not kept\n" + report).c_str());
					if (codes[i] >= 64 && codes[i] <= 940)
						Assert::IsTrue(std::abs(got[i] - offCodes[i]) <= 1, ToWide("in-range PQ differs from switch-off\n" + report).c_str());
				}
				// Peak detection on: dynamic tone mapping, the carry stays off
				// (HdrMapped) and the picture is switch-off's.
				ErMoreCase dynamic = m;
				dynamic.peakDetection = true;
				const ErMoreResult dyn = ErRender(fixture.Gpu(), fixture.Renderer(), greys, dynamic);
				Assert::IsTrue(dyn.r.ok && dyn.r.decision.reason == ExtendedRange::Reason::HdrMapped,
					ToWide(ExtendedRange::Describe(dyn.r.decision)).c_str());
				ErMoreCase old = m;
				old.settings.hdrPassthrough = false;
				Assert::IsTrue(ErRender(fixture.Gpu(), fixture.Renderer(), greys, old).r.decision.reason ==
					ExtendedRange::Reason::HdrSource);
			}
			ErMoreCase toned;
			toned.c.hdrSource = true;
			toned.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
			Assert::IsTrue(ErRender(fixture.Gpu(), fixture.Renderer(), greys, toned).r.decision.reason ==
				ExtendedRange::Reason::HdrSource);
			pl_color_space pq{};
			pq.primaries = PL_COLOR_PRIM_BT_2020;
			pq.transfer = PL_COLOR_TRC_PQ;
			pq.hdr.min_luma = 0.005f;
			pq.hdr.max_luma = 1000.0f;
			pl_color_space display = pq;
			display.hdr.max_luma = 600.0f;
			Assert::IsTrue(ExtendedRange::Decide(true, ExtendedRange::Settings{}, pq, display, nullptr, PL_LUT_UNKNOWN).reason ==
				ExtendedRange::Reason::HdrMapped);
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// The late restore with a texture-input PRE_OUTPUT hook after it (as
		// the LED shader), frames alternating with switch-off frames, 1:1, up
		// and down: libplacebo disables no hook (0 disabled), the hooks run every
		// on-frame, the limited output encoding is intact (greys 64/502/940
		// within 1 code; on 7.360 with the repair) and BTB stays below 64.
		TEST_METHOD(ExtendedRangeLateHookNotDisabled)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErLut identity;
			identity.Build(17, 0x4552494431370003ull);
			const std::vector<int>& codes = ErCodes();
			const Planes greys = Greys(codes);
			for (int mode = 0; mode < 2; ++mode)
				for (double scale : { 1.0, 1.5, 0.5 })
					for (int frame = 0; frame < 3; ++frame)
					{
						ErMoreCase m;
						m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
						m.c.scale = scale;
						m.preOutputHook = ErTexturePreOutputHook();
						if (mode == 0)
							m.lut = &identity.lut;
						else
						{
							m.c.sourcePrimaries = PL_COLOR_PRIM_BT_2020;
							m.bt2020Matrix = true;
						}
						ErMoreCase off = m;
						off.c.on = false;
						const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
						const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), greys, off);
						Assert::IsTrue(on.r.ok && ref.r.ok);
						const std::string name = std::string(mode == 0 ? "LUT" : "gamut") + " x" + std::to_string(scale) +
							" frame " + std::to_string(frame);
						const auto got = Columns(on.r, static_cast<int>(codes.size()), scale);
						report += name + ": " + Join(got) + (on.repaired ? " (repaired)" : "") + " disabled " +
							std::to_string(on.disabledHooks) + "\n";
						Assert::IsTrue(on.r.decision.Active() && on.r.decision.LateRestore(), ToWide(name).c_str());
						Assert::IsFalse(on.erHookDisabled, ToWide(name + ": an extended-range hook was disabled\n" + report + "log: " + log).c_str());
						Assert::AreEqual(0, on.disabledHooks, ToWide(name + ": a hook was disabled\n" + report).c_str());
						Assert::IsTrue(on.r.restores >= static_cast<unsigned long long>(mode == 0 ? 2 : 1),
							ToWide(name + ": the hooks did not run\n" + report).c_str());
#if PL_API_VER < 377
						Assert::IsTrue(on.repaired, ToWide(name + ": the output-encoding repair is needed on 7.360").c_str());
#endif
						for (size_t i = 0; i < codes.size(); ++i)
							if (codes[i] == 64 || codes[i] == 502 || codes[i] == 940)
								Assert::IsTrue(std::abs(got[i] - codes[i]) <= 1, ToWide(name + ": output encoding wrong\n" + report).c_str());
						Assert::IsTrue(got.front() < 64, ToWide(name + ": BTB lost\n" + report).c_str());
					}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// The late restore's position mapping with an offset target crop
		// (blanking 13/3/7/5) and narrow alternating BTB/WTW columns. LUT mode:
		// identity LUT vs no LUT (SCALED restore only), every pixel max 1 code,
		// mean 0.1, at 1:1 and up 1.5 (the late restore reads v back exactly).
		// Gamut mode: BT.2020 greys vs the same greys as BT.709 (SCALED restore
		// path), column centres <= 1 code (edges differ by the remainder's
		// linear vs. signal footprint averaging, not checked).
		TEST_METHOD(ExtendedRangeLateRestoreCropAlignment)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErLut identity;
			identity.Build(17, 0x4552494431370004ull);
			const std::vector<int> codes = { 4, 1019, 4, 1019, 64, 940, 16, 980, 502, 4, 1019, 40 };
			const int width = 16;
			const Planes greys = Greys(codes, width, 16);
			for (double scale : { 1.0, 1.5 })
			{
				ErMoreCase m;
				m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
				m.c.scale = scale;
				m.c.blank[0] = 13; m.c.blank[1] = 3; m.c.blank[2] = 7; m.c.blank[3] = 5;
				ErMoreCase withLut = m;
				withLut.lut = &identity.lut;
				const ErMoreResult a = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
				const ErMoreResult b = ErRender(fixture.Gpu(), fixture.Renderer(), greys, withLut);
				Assert::IsTrue(a.r.ok && b.r.ok && b.r.decision.LateRestore());
				const Difference d = Compare(a.r, b.r);
				report += "LUT x" + std::to_string(scale) + ": max " + std::to_string(d.max) + " mean " + std::to_string(d.mean) + "\n";
				Assert::IsTrue(d.max <= 1 && d.mean <= 0.1, ToWide("the late LUT restore is misaligned\n" + report).c_str());

				ErMoreCase wide = m;
				wide.c.sourcePrimaries = PL_COLOR_PRIM_BT_2020;
				wide.bt2020Matrix = true;
				const ErMoreResult w = ErRender(fixture.Gpu(), fixture.Renderer(), greys, wide);
				Assert::IsTrue(w.r.ok && w.r.decision.libplaceboMapsPrimaries);
				const double cropW = a.r.width - m.c.blank[0] - m.c.blank[2];
				const double cropH = a.r.height - m.c.blank[1] - m.c.blank[3];
				const int y = static_cast<int>(m.c.blank[1] + cropH / 2);
				std::string line = "gamut x" + std::to_string(scale) + ":";
				for (size_t i = 0; i < codes.size(); ++i)
				{
					const int x = static_cast<int>(m.c.blank[0] + (i * width + width / 2.0) * cropW / greys.width);
					line += " " + std::to_string(w.r.Channel(x, y, 1)) + "/" + std::to_string(a.r.Channel(x, y, 1));
					for (int ch = 0; ch < 3; ++ch)
						Assert::IsTrue(std::abs(w.r.Channel(x, y, ch) - a.r.Channel(x, y, ch)) <= 1,
							ToWide("the late gamut restore is misaligned\n" + report + line).c_str());
				}
				report += line + "\n";
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// Audit stage8-rs232 note: the owner's chain NLS (scope profile) +
		// Stage 10 + SSIM. NLS hooks the picture at RGB after the carry (so it
		// warps the offset in alpha with the picture), SSIM at POST_KERNEL (the
		// texture restore). With the carry on, the same chain keeps in-range
		// codes as with it off and keeps below black / above white; no hook is
		// disabled, on the first frame and after.
		TEST_METHOD(ExtendedRangeKeepsBtbThroughNlsAndSsim)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const pl_hook* nls = ParseBundledNlsShader(gpu, "NLS.glsl", 0.0);
			BindNlsShader(nls, 1.32f, 0.0f);
			const std::string source = LoadBundledShader("SSimDownscaler.glsl");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
			Assert::IsNotNull(ssim);
			const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
			Case c;
			c.scale = 0.5;
			c.mitchell = true;
			c.extraHooks = { nls, ssim };
			Case off = c;
			off.on = false;
			std::string report;
			for (int frame = 0; frame < 2; ++frame)
			{
				const Result on = Render(gpu, fixture.Renderer(), Greys(codes, 64), c);
				const Result plain = Render(gpu, fixture.Renderer(), Greys(codes, 64), off);
				Assert::IsTrue(on.ok && plain.ok);
				const auto got = Columns(on, static_cast<int>(codes.size()), 0.5, 64);
				const auto ref = Columns(plain, static_cast<int>(codes.size()), 0.5, 64);
				report += "frame " + std::to_string(frame) + " on:  " + Join(got) + "\n";
				report += "frame " + std::to_string(frame) + " off: " + Join(ref) + "\n";
				for (size_t i = 0; i < codes.size(); ++i)
				{
					if (codes[i] >= 64 && codes[i] <= 940)
						Assert::IsTrue(std::abs(got[i] - ref[i]) <= 2, ToWide("in-range changed\n" + report).c_str());
					else if (codes[i] < 64)
						Assert::IsTrue(got[i] <= ref[i] - 8, ToWide("below black lost\n" + report).c_str());
					else
						// Kept (the plain chain keeps it here too: NLS's float texture).
						Assert::IsTrue(got[i] > 940 && got[i] >= ref[i] - 2, ToWide("above white lost\n" + report).c_str());
				}
				Assert::IsTrue(on.restores > 0, ToWide("the restore did not run\n" + report).c_str());
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			pl_mpv_user_shader_destroy(&ssim);
			pl_mpv_user_shader_destroy(&nls);
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// The whole chain with motion (the matcher's look-ahead vectors, the
		// grain map following them, the occlusion mask, the sigma-delta's motion
		// compensation), a de-judder extra, and the LED drive's gain sequence
		// (ChainGpuProbe::RunMotion).
		// The Stage 10 carry on a single-plane RGB source (the subtitle move's
		// composed picture, an RGB capture): libplacebo 7.360 asserted
		// (renderer.c:457) after the inline colour carry; the carry's texture form
		// renders, with and without NLS / SSIM, cropped or not, 3 or 4 components.
		TEST_METHOD(ExtendedRangeOnASinglePlaneRgbSource)
		{
			for (unsigned mode : { 0u, 1u, 2u, 3u, 4u, 8u })
			{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			pl_fmt f16 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0, static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE));
			std::vector<uint16_t> px(static_cast<size_t>(640) * 96 * 4, 0x3800);   // 0.5
			pl_tex_params tp{}; tp.w = 640; tp.h = 96; tp.format = f16; tp.sampleable = true; tp.renderable = true; tp.initial_data = px.data();
			pl_tex tex = pl_tex_create(gpu, &tp);
			pl_frame rgb{};
			rgb.num_planes = 1; rgb.planes[0].texture = tex; rgb.planes[0].components = 4;
			for (int i = 0; i < 4; ++i) rgb.planes[0].component_mapping[i] = i;
			rgb.repr.sys = PL_COLOR_SYSTEM_RGB; rgb.repr.levels = PL_COLOR_LEVELS_FULL; rgb.repr.alpha = PL_ALPHA_NONE;
			rgb.color.primaries = PL_COLOR_PRIM_BT_709; rgb.color.transfer = PL_COLOR_TRC_GAMMA22;
			rgb.crop = { 0.0f, 16.0f, 640.0f, 80.0f };
			const std::string nlsText = LoadBundledShader("SSimDownscaler.glsl");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, nlsText.data(), nlsText.size());
			const pl_hook* nls = ParseBundledNlsShader(gpu, "NLS.glsl", 0.0);
			BindNlsShader(nls, 1.0f, 0.0f);
			Case c; c.scale = 0.5; c.mitchell = true; c.on = true; c.image = &rgb;
			c.sourceCrop[0] = 0; c.sourceCrop[1] = 16; c.sourceCrop[2] = 640; c.sourceCrop[3] = 80;
			if (mode & 1u) c.extraHooks.push_back(nls);
			if (mode & 2u) c.extraHooks.push_back(ssim);
			if (mode & 4u) { c.sourceCrop[1] = 0; c.sourceCrop[3] = 96; rgb.crop = { 0.0f, 0.0f, 640.0f, 96.0f }; }
			if (mode & 8u) rgb.planes[0].components = 3;
			Planes size; size.width = 640; size.height = 96;
			const Result r = Render(gpu, fixture.Renderer(), size, c);
			pl_tex_destroy(gpu, &tex);
			pl_mpv_user_shader_destroy(&ssim);
			pl_mpv_user_shader_destroy(&nls);
			Assert::IsTrue(r.ok && r.restores > 0, (L"mode " + std::to_wstring(mode)).c_str());
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks);
			}
		}

		// Upscaling with every hook (ChainGpuProbe::RunUpscale), FSRCNNX and ArtCNN.
		// The Stage 10 carry after hooks on the planes before RGB: libplacebo 7.360
		// asserted (renderer.c:457) after the inline carry with CfL chroma
		// prediction; Prepare picks the carry's texture form then. CfL, the LUMA
		// prescaler and SSimSuperRes, alone and together, on a 2x upscale.
		// Stage 10b on saturated colours (10a's open item: its one-offset shift
		// made steps that SSIM enhanced, 46 codes). 10b hands libplacebo the
		// clipped picture and converts the primaries where libplacebo does, so
		// wherever no out-of-range source pixel lies within the filters' reach
		// (12 source pixels: Mitchell at 0.5 plus SSIM's window), Stage 10 must
		// give the picture without the switch.
		// Near out-of-range content the outputs legitimately differ (the true
		// values are mixed in instead of clipped ones); that is reported.
		// Plain scaling and with SSIM, down 0.5, a strongly coloured texture.
		TEST_METHOD(ExtendedRangeOnSaturatedColourWithSsim)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			const std::vector<int> ramp = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
			Planes a = ChainGpuProbe::MovingFrame(0, ramp);
			ChainGpuProbe::Colourise(a, 0, 64);
			// Which source pixels leave 0..1 in a channel (BT.709 Y'CbCr -> R'G'B').
			std::vector<char> outOfRange(static_cast<size_t>(a.width) * a.height, 0);
			for (int y = 0; y < a.height; ++y)
				for (int x = 0; x < a.width; ++x)
				{
					const double yv = ((a.luma[static_cast<size_t>(y) * a.width + x] >> 6) - 64) / 876.0;
					const size_t ci = (static_cast<size_t>(y) * (a.width / 2) + x / 2) * 2;
					const double cb = ((a.chroma[ci] >> 6) - 512) / 896.0, cr = ((a.chroma[ci + 1] >> 6) - 512) / 896.0;
					const double r = yv + 1.5748 * cr, g = yv - 0.1873 * cb - 0.4681 * cr, b = yv + 1.8556 * cb;
					outOfRange[static_cast<size_t>(y) * a.width + x] =
						(std::min)({ r, g, b }) < -0.002 || (std::max)({ r, g, b }) > 1.002;
				}
			auto nearOutOfRange = [&](int ox, int oy)
			{
				const int cx = ox * 2 + 1, cy = oy * 2 + 1;
				for (int y = (std::max)(0, cy - 12); y <= (std::min)(a.height - 1, cy + 12); ++y)
					for (int x = (std::max)(0, cx - 12); x <= (std::min)(a.width - 1, cx + 12); ++x)
						if (outOfRange[static_cast<size_t>(y) * a.width + x])
							return true;
				return false;
			};
			const std::string ssimText = LoadBundledShader("SSimDownscaler.glsl");
			const pl_hook* ssim = pl_mpv_user_shader_parse(fixture.Gpu(), ssimText.data(), ssimText.size());
			Assert::IsNotNull(ssim);
			std::string report;
			bool ok = true;
			for (enum pl_color_primaries primaries : { PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_PRIM_BT_709 })
				for (bool withSsim : { false, true })
				{
					Case on; on.scale = 0.5; on.mitchell = true; on.outputPrimaries = primaries;
					if (withSsim) on.extraHooks.push_back(ssim);
					Case off = on; off.on = false;
					const Result r = Render(fixture.Gpu(), fixture.Renderer(), a, on);
					const Result o = Render(fixture.Gpu(), fixture.Renderer(), a, off);
					Assert::IsTrue(r.ok && o.ok && r.restores > 0);
					int clearMax = 0, clear = 0, nearMax = 0, closeBy = 0;
					for (int y = 2; y < 30; ++y)   // the texture rows, clear of the ramp
						for (int x = 2; x < r.width - 2; ++x)
						{
							int d = 0;
							for (int ch = 0; ch < 3; ++ch)
								d = (std::max)(d, std::abs(r.Channel(x, y, ch) - o.Channel(x, y, ch)));
							if (nearOutOfRange(x, y)) { nearMax = (std::max)(nearMax, d); ++closeBy; }
							else { clearMax = (std::max)(clearMax, d); ++clear; }
						}
					// 10a: 46 codes here (P3, SSIM). The residual with SSIM is its
					// magnification of the two conversions' rounding.
					const int allowed = withSsim ? 3 : 1;
					ok = ok && clearMax <= allowed && clear > 1000;
					report += std::string(primaries == PL_COLOR_PRIM_BT_709 ? "BT.709 output, " : "P3 output, ") +
						(withSsim ? "SSIM" : "plain") + ": clear of out-of-range content " + std::to_string(clearMax) +
						" codes at most (" + std::to_string(clear) + " pixels, allowed " + std::to_string(allowed) + "); near it " +
						std::to_string(nearMax) + " (" + std::to_string(closeBy) + " pixels, the true values mixed in)\n";
				}
			pl_mpv_user_shader_destroy(&ssim);
			Logger::WriteMessage(report.c_str());
			Assert::IsTrue(ok, ToWide(report).c_str());
		}

		TEST_METHOD(ExtendedRangeAfterPlaneHooks)
		{
			using namespace ExtendedRangeGpuProbe;
			for (unsigned mode : { 1u, 2u, 3u, 7u })
			{
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create());
				pl_gpu gpu = fixture.Gpu();
				std::string error;
				auto src = [](const char* file) { return LoadBundledShader(file); };
				auto pre = ChainGpuProbe::Load(gpu, src, "FSRCNNX_x2_16-0-4-1.glsl", 1280, 192, {}, error);
				auto cfl = ChainGpuProbe::Load(gpu, src, "CfL_Prediction.glsl", 1280, 192, {}, error);
				auto sr = ChainGpuProbe::Load(gpu, src, "SSimSuperRes.glsl", 1280, 192, {}, error);
				Assert::IsTrue(pre && cfl && sr, ToWide(error).c_str());
				const std::vector<int> ramp = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
				Planes a = ChainGpuProbe::MovingFrame(0, ramp);
				ChainGpuProbe::Colourise(a, 0, 64);
				Case c;
				c.scale = 2.0;
				if (mode & 1u) c.extraHooks.push_back(pre->hook);
				if (mode & 2u) c.extraHooks.push_back(cfl->hook);
				if (mode & 4u) c.extraHooks.push_back(sr->hook);
				const Result r = Render(gpu, fixture.Renderer(), a, c);
				Assert::IsTrue(r.ok && r.restores > 0, (L"mode " + std::to_wstring(mode)).c_str());
				{ const pl_render_errors e = pl_renderer_get_errors(fixture.Renderer()); Assert::AreEqual(0, e.num_disabled_hooks, (L"mode " + std::to_wstring(mode) + L" disabled " + (e.num_disabled_hooks ? std::to_wstring(e.disabled_hooks[0]) : L"")).c_str()); }
			}
		}
	};
}
