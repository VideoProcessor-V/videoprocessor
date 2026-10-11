#include "pch.h"
#include "CppUnitTest.h"

#include <led/LedController.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

namespace
{
	// docs/reference/beta_22.py transcribed statement for statement (the
	// screenshot, prints and serial calls replaced by the values they would
	// send). Kept deliberately literal: it is the oracle for ScriptController.
	struct ScriptReference
	{
		int current_contrast = 255; // the script's name for the LED power
		int current_con = 50;
		int set_contrast = 50;

		struct Iteration
		{
			int powerSent = -1;
			std::vector<int> contrastSends;
		};

		Iteration Step(double mean, int max_val)
		{
			Iteration sent;
			int highlightdetect = 0;
			if (max_val >= 215) highlightdetect = 1;
			if (max_val < 215) highlightdetect = 0;
			const int mean2 = static_cast<int>(mean);
			int target_power = 0, target_contrast = 0;

			if (mean2 >= 125) { target_power = 255; target_contrast = 50; }
			if (100 <= mean2 && mean2 < 125) { target_power = 255; target_contrast = 50; }
			if (70 <= mean2 && mean2 < 100) target_power = static_cast<int>(((mean2 - 70) * 0.8333) + 230);
			if (40 <= mean2 && mean2 < 70) target_power = static_cast<int>(((mean2 - 40) * 1) + 200);
			if (20 <= mean2 && mean2 < 40) target_power = static_cast<int>(((mean2 - 20) * 5) + 100);
			if (0 <= mean2 && mean2 < 20) target_power = 100;

			if (60 <= mean2 && mean2 < 100) target_contrast = 50;
			if (45 <= mean2 && mean2 < 60)
			{
				if (highlightdetect == 1) target_contrast = 50;
				if (highlightdetect == 0) target_contrast = 51;
			}
			if (30 <= mean2 && mean2 < 45) target_contrast = static_cast<int>((std::abs(mean2 - 45) * 0.7333) + 51);
			if (20 <= mean2 && mean2 < 30) target_contrast = static_cast<int>((std::abs(mean2 - 30) * 1.2) + 62);
			if (0 <= mean2 && mean2 < 20) target_contrast = 74;

			if (current_contrast != target_power)
			{
				const int difference = std::abs(target_power - current_contrast);
				int set_power = current_contrast;
				if (difference >= 100)
				{
					if (target_power > current_contrast) { set_power = current_contrast + 7; current_contrast = set_power; }
					if (target_power < current_contrast) { set_power = current_contrast - 5; current_contrast = set_power; }
				}
				if (50 <= difference && difference < 100)
				{
					if (target_power > current_contrast)
					{
						if (mean2 < 40) { set_power = current_contrast + 5; current_contrast = set_power; }
						if (mean2 >= 40) { set_power = current_contrast + 4; current_contrast = set_power; }
					}
					if (target_power < current_contrast) { set_power = current_contrast - 3; current_contrast = set_power; }
				}
				if (25 <= difference && difference < 50)
				{
					if (target_power > current_contrast)
					{
						if (mean2 < 40) { set_power = current_contrast + 4; current_contrast = set_power; }
						if (mean2 >= 40) { set_power = current_contrast + 2; current_contrast = set_power; }
					}
					if (target_power < current_contrast) { set_power = current_contrast - 2; current_contrast = set_power; }
				}
				if (10 <= difference && difference < 25)
				{
					if (target_power > current_contrast) { set_power = current_contrast + 1; current_contrast = set_power; }
					if (target_power < current_contrast) { set_power = current_contrast - 1; current_contrast = set_power; }
				}
				if (difference < 10)
				{
					if (target_power > current_contrast) { set_power = current_contrast + 1; current_contrast = set_power; }
					if (target_power < current_contrast) { set_power = current_contrast - 1; current_contrast = set_power; }
				}
				sent.powerSent = set_power;
			}

			if (current_con != target_contrast)
			{
				int difference2 = std::abs(target_contrast - current_con);
				if (10 <= difference2)
				{
					if (target_contrast > current_con) { set_contrast = current_con + 1; current_con = set_contrast; }
					if (target_contrast < current_con)
					{
						if (highlightdetect == 1)
						{
							while (10 < difference2)
							{
								set_contrast = current_con - 1;
								current_con = set_contrast;
								sent.contrastSends.push_back(set_contrast);
								difference2 = std::abs(target_contrast - current_con);
							}
							if (difference2 == 10) { set_contrast = current_con - 1; current_con = set_contrast; }
						}
						if (highlightdetect == 0) set_contrast = current_con - 1;
						current_con = set_contrast;
					}
				}
				if (7 <= difference2 && difference2 < 10)
				{
					if (target_contrast > current_con) { set_contrast = current_con + 1; current_con = set_contrast; }
					if (target_contrast < current_con)
						while (7 <= difference2 && difference2 < 10)
						{
							set_contrast = current_con - 1;
							current_con = set_contrast;
							sent.contrastSends.push_back(set_contrast);
							difference2 = std::abs(target_contrast - current_con);
						}
				}
				if (1 <= difference2 && difference2 < 7)
				{
					if (6 <= difference2 && difference2 < 7)
					{
						if (target_contrast > current_con) { set_contrast = current_con + 1; current_con = set_contrast; }
						if (target_contrast < current_con) { set_contrast = current_con - 1; current_con = set_contrast; }
					}
					if (4 <= difference2 && difference2 < 6)
					{
						if (target_contrast > current_con) { set_contrast = current_con + 1; current_con = set_contrast; }
						if (target_contrast < current_con) { set_contrast = current_con - 1; current_con = set_contrast; }
					}
					if (1 <= difference2 && difference2 < 4)
					{
						if (target_contrast > current_con)
						{
							if (highlightdetect == 1) set_contrast = current_con + 0;
							if (highlightdetect == 0) set_contrast = current_con + 1;
							current_con = set_contrast;
						}
						if (target_contrast < current_con)
						{
							if (highlightdetect == 1) set_contrast = current_con - 1;
							if (highlightdetect == 0) set_contrast = current_con - 0;
							current_con = set_contrast;
						}
					}
				}
				sent.contrastSends.push_back(set_contrast);
			}
			return sent;
		}
	};

	// The script's 0.8333 constant truncates one level below the exact
	// 25/30 slope at these APLs (see the brief's open questions).
	bool RoundedSlopeApl(int apl)
	{
		return apl == 76 || apl == 82 || apl == 88 || apl == 94;
	}

	// Deterministic synthetic APL / highlight sequence: slow drift, scene cuts,
	// fades to black and highlights switching on and off.
	struct SyntheticScene
	{
		uint32_t state;
		double apl = 60.0;
		int highlight = 180;

		explicit SyntheticScene(uint32_t seed) : state(seed) {}

		uint32_t Next()
		{
			state = state * 1664525u + 1013904223u;
			return state >> 8;
		}

		void Advance()
		{
			const uint32_t r = Next() % 1000;
			if (r < 30) apl = static_cast<double>(Next() % 140);           // cut
			else if (r < 60) apl = apl * 0.8;                                 // fade step
			else apl += (static_cast<double>(Next() % 2001) - 1000.0) / 250.0; // drift
			apl = std::max(0.0, std::min(apl, 160.0));
			if (Next() % 100 < 8) highlight = Next() % 2 ? 230 : 190;
			// Step 7b: script mode uses the script's literal 0.8333 formula, so
			// the oracle covers every APL (76, 82, 88 and 94 included).
		}
	};
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedDriveControllerTests)
	{
	public:
		TEST_METHOD(LightTargetsFollowTheScriptCurve)
		{
			const ScriptController controller{ LedDriveConfig() };
			for (int apl = 0; apl <= 255; ++apl)
			{
				int expected = 0;
				if (apl >= 100) expected = 255;
				else if (apl >= 70) expected = static_cast<int>(((apl - 70) * 0.8333) + 230);
				else if (apl >= 40) expected = apl - 40 + 200;
				else if (apl >= 20) expected = (apl - 20) * 5 + 100;
				else expected = 100;
				Assert::AreEqual(expected, controller.LightTargetFor(apl),
					(L"APL " + std::to_wstring(apl)).c_str());
				Assert::AreEqual(expected, ScriptLiteralLevel(apl));
			}
			// Step 7b: the script's rounded 0.8333 slope, as the script has it;
			// the interpolated curve (compensate, custom curves) keeps the exact
			// 25/30 slope, one level higher at these APLs.
			const LedDriveConfig defaults;
			Assert::IsTrue(IsBuiltInCurve(defaults.curve));
			for (int apl = 0; apl <= 255; ++apl)
				Assert::AreEqual(ScriptLiteralLevel(apl) + (RoundedSlopeApl(apl) ? 1 : 0), CurveLevel(defaults, apl),
					(L"APL " + std::to_wstring(apl)).c_str());
			Assert::AreEqual(234, controller.LightTargetFor(76));
			Assert::AreEqual(235, CurveLevel(defaults, 76));
			// A custom curve is interpolated as before, even with the same shape
			// shifted by one point.
			LedDriveConfig custom;
			custom.curve.back().level = 254;
			Assert::IsFalse(IsBuiltInCurve(custom.curve));
			Assert::AreEqual(235, ScriptController(custom).LightTargetFor(76));
		}

		TEST_METHOD(ContrastTargetsFollowTheScriptBands)
		{
			for (int apl = 0; apl <= 255; ++apl)
				for (bool highlight : { false, true })
				{
					int expected = 74;
					if (apl >= 60) expected = 50;
					else if (apl >= 45) expected = highlight ? 50 : 51;
					else if (apl >= 30) expected = static_cast<int>(std::abs(apl - 45) * 0.7333 + 51);
					else if (apl >= 20) expected = static_cast<int>(std::abs(apl - 30) * 1.2 + 62);
					Assert::AreEqual(expected, ScriptController::ContrastTargetFor(apl, highlight));
				}
			// The guide says "62 at 30"; the script's 0.7333 truncates to 61 there
			// and jumps to 63 at APL 29.
			Assert::AreEqual(61, ScriptController::ContrastTargetFor(30, false));
			Assert::AreEqual(63, ScriptController::ContrastTargetFor(29, false));
			Assert::AreEqual(74, ScriptController::ContrastTargetFor(20, false));
		}

		TEST_METHOD(ControllerReproducesBeta22TickForTickOnSyntheticSequences)
		{
			for (uint32_t seed : { 1u, 22u, 333u, 4444u, 55555u })
			{
				ScriptController controller{ LedDriveConfig() };
				ScriptReference reference;
				SyntheticScene scene(seed);
				for (int tick = 0; tick < 3000; ++tick)
				{
					scene.Advance();
					FrameStats stats;
					stats.apl = scene.apl;
					stats.maximum = scene.highlight;
					const ControllerOutput out = controller.Tick(stats);
					const auto expected = reference.Step(scene.apl, scene.highlight);
					const std::wstring where = L"seed " + std::to_wstring(seed) +
						L" tick " + std::to_wstring(tick);
					Assert::AreEqual(expected.powerSent, out.lightCommand, where.c_str());
					Assert::AreEqual(expected.contrastSends.size(), out.contrastSchedule.size(), where.c_str());
					for (size_t i = 0; i < expected.contrastSends.size(); ++i)
						Assert::AreEqual(expected.contrastSends[i], out.contrastSchedule[i].contrast, where.c_str());
					Assert::AreEqual(reference.current_contrast, out.light, where.c_str());
					Assert::AreEqual(reference.current_con, out.contrast, where.c_str());
				}
			}
		}

		TEST_METHOD(BrighteningIsFasterInDarkScenesAndDimmingUsesOneTable)
		{
			const LedDriveConfig config;
			ScriptController controller(config);
			FrameStats stats;
			controller.Reset(150);
			stats.apl = 39; // target 195, distance 45 -> dark table 4
			Assert::AreEqual(154, controller.Tick(stats).light);
			controller.Reset(150);
			stats.apl = 41; // target 201, distance 51 -> bright table 4
			Assert::AreEqual(154, controller.Tick(stats).light);
			controller.Reset(100);
			stats.apl = 39; // distance 95 -> dark 5
			Assert::AreEqual(105, controller.Tick(stats).light);
			controller.Reset(150);
			stats.apl = 45; // target 205, distance 55 -> bright 4
			Assert::AreEqual(154, controller.Tick(stats).light);
			controller.Reset(200);
			stats.apl = 70; // target 230, distance 30 -> bright 2
			Assert::AreEqual(202, controller.Tick(stats).light);
			controller.Reset(210);
			stats.apl = 70; // distance 20 -> 1 (not 2: the brief's table was wrong)
			Assert::AreEqual(211, controller.Tick(stats).light);
			controller.Reset(255);
			stats.apl = 10; // target 100, distance 155 -> down 5
			Assert::AreEqual(250, controller.Tick(stats).light);
			controller.Reset(255);
			stats.apl = 70; // distance 25 -> down 2
			Assert::AreEqual(253, controller.Tick(stats).light);
		}

		TEST_METHOD(HighlightFastDumpStepsEvery100msThenOneMore)
		{
			ScriptController controller{ LedDriveConfig() };
			controller.Reset(230, 74);
			FrameStats stats;
			stats.apl = 70;          // light target 230 (no light change), contrast target 50
			stats.maximum = 230;   // highlight on screen
			const auto out = controller.Tick(stats);
			Assert::AreEqual(-1, out.lightCommand);
			// 74 -> 60 in fast steps (distance 24 -> 10), then 59 after 200 ms.
			Assert::AreEqual(size_t{ 15 }, out.contrastSchedule.size());
			Assert::AreEqual(73, out.contrastSchedule.front().contrast);
			Assert::AreEqual(100, out.contrastSchedule.front().offsetMs);
			Assert::AreEqual(60, out.contrastSchedule[13].contrast);
			Assert::AreEqual(1400, out.contrastSchedule[13].offsetMs);
			Assert::AreEqual(59, out.contrastSchedule.back().contrast);
			Assert::AreEqual(1600, out.contrastSchedule.back().offsetMs);
			Assert::AreEqual(1600, out.durationMs);

			// Without a highlight the same drop is one step per tick.
			controller.Reset(230, 74);
			stats.maximum = 180;
			const auto slow = controller.Tick(stats);
			Assert::AreEqual(size_t{ 1 }, slow.contrastSchedule.size());
			Assert::AreEqual(73, slow.contrast);
		}

		TEST_METHOD(SecondFastDumpFallsThroughIntoTheAntiPumpBand)
		{
			ScriptController controller{ LedDriveConfig() };
			controller.Reset(230, 59);
			FrameStats stats;
			stats.apl = 70;          // contrast target 50, distance 9
			stats.maximum = 180;   // no highlight: this dump runs regardless
			const auto out = controller.Tick(stats);
			// 58, 57, 56 at 100 ms, then the fall-through step to 55.
			Assert::AreEqual(size_t{ 4 }, out.contrastSchedule.size());
			Assert::AreEqual(56, out.contrastSchedule[2].contrast);
			Assert::AreEqual(55, out.contrast);
			Assert::AreEqual(500, out.durationMs);
		}

		TEST_METHOD(AntiPumpIgnoresSmallChangesInTheScriptsDirections)
		{
			ScriptController controller{ LedDriveConfig() };
			FrameStats stats;
			stats.apl = 70; // contrast target 50
			controller.Reset(230, 52);
			stats.maximum = 180;
			Assert::AreEqual(52, controller.Tick(stats).contrast, L"No highlight: small decreases ignored");
			controller.Reset(230, 52);
			stats.maximum = 230;
			Assert::AreEqual(51, controller.Tick(stats).contrast, L"Highlight: small decreases allowed");
			controller.Reset(230, 48);
			stats.maximum = 230;
			Assert::AreEqual(48, controller.Tick(stats).contrast, L"Highlight: small increases ignored");
			controller.Reset(230, 48);
			stats.maximum = 180;
			Assert::AreEqual(49, controller.Tick(stats).contrast, L"No highlight: small increases allowed");
			controller.Reset(230, 55);
			stats.maximum = 180;
			Assert::AreEqual(54, controller.Tick(stats).contrast, L"Outside the band both directions move");
		}

		// Step 7b, script_smoothing: on. The projector's CNTR moves in whole
		// steps; VP's gain bridges them: the picture (gain x landed CNTR)
		// moves continuously, is never brighter than the projector's own value
		// and equals it once each step has been bridged.
		TEST_METHOD(ScriptSmootherBridgesIncreasesAfterTheyLand)
		{
			ScriptSmoother smoother;
			smoother.Reset(50);
			// LPW1 at 0, CNTR 51 at 200 ms, landing 80 ms later; the next tick's
			// CNTR 52 at 600 ms, landing at 680.
			smoother.Plan({ { 280.0, 51 } });
			double previous = 50.0;
			for (double t = 0.0; t <= 1200.0; t += 4.0)
			{
				if (t == 400.0) smoother.Plan({ { 680.0, 52 } });
				const double gain = smoother.Gain(t);
				const double picture = gain * smoother.Landed();
				Assert::IsTrue(gain <= 1.0 + 1e-12, (L"Brighter than the projector at " + std::to_wstring(t)).c_str());
				Assert::IsTrue(std::abs(picture - previous) <= 0.025,
					(L"A visible step at " + std::to_wstring(t)).c_str());
				if (t < 280.0) Assert::AreEqual(50.0, picture, 1e-12, L"Nothing moves before the step lands");
				if (t >= 480.0 && t < 680.0) Assert::AreEqual(51.0, picture, 1e-9, L"51 bridged within 200 ms");
				previous = picture;
			}
			Assert::AreEqual(52, smoother.Landed());
			Assert::AreEqual(1.0, smoother.Gain(1300.0), 1e-12, L"Gain 1 once the steps are bridged");
		}

		TEST_METHOD(ScriptSmootherRampsFastDumpsDownBeforeTheyLand)
		{
			ScriptSmoother smoother;
			smoother.Reset(74);
			// The highlight fast dump: 73..60 every 100 ms, then 59 200 ms later.
			std::vector<std::pair<double, int>> plan;
			for (int i = 1; i <= 14; ++i) plan.push_back({ 80.0 + 100.0 * i, 74 - i });
			plan.push_back({ 80.0 + 1600.0, 59 });
			smoother.Plan(plan);
			double previous = 74.0;
			for (double t = 0.0; t <= 2000.0; t += 2.0)
			{
				const double gain = smoother.Gain(t);
				const double picture = gain * smoother.Landed();
				Assert::IsTrue(gain <= 1.0 + 1e-12, (L"Brighter than the projector at " + std::to_wstring(t)).c_str());
				Assert::IsTrue(std::abs(picture - previous) <= 0.025,
					(L"A visible step at " + std::to_wstring(t)).c_str());
				for (const auto& landing : plan)
					if (t == landing.first)
						Assert::AreEqual(static_cast<double>(landing.second), picture, 1e-9,
							(L"Not at the projector's value as it lands, " + std::to_wstring(t)).c_str());
				previous = picture;
			}
			Assert::AreEqual(59, smoother.Landed());
			Assert::AreEqual(1.0, smoother.Gain(2100.0), 1e-12);
			Assert::AreEqual(size_t{ 0 }, smoother.Pending());
		}

		TEST_METHOD(ScriptSmootherFollowsTheRealSendTimes)
		{
			ScriptSmoother smoother;
			smoother.Reset(60);
			smoother.Plan({ { 100.0, 59 }, { 200.0, 58 } });
			smoother.Sent(150.0); // a slow port: the first lands 50 ms late
			Assert::AreEqual(1.0, smoother.Gain(0.0), 1e-12);
			smoother.Gain(120.0);
			Assert::AreEqual(60, smoother.Landed(), L"Not landed at its planned time");
			Assert::IsTrue(smoother.Smooth() < 60.0, L"Already ramping down to it");
			smoother.Gain(150.0);
			Assert::AreEqual(59, smoother.Landed());
			Assert::AreEqual(59.0, smoother.Smooth(), 1e-9);
			smoother.Sent(260.0); // and the second later than planned too
			smoother.Gain(220.0);
			Assert::AreEqual(59, smoother.Landed());
			smoother.Gain(260.0);
			Assert::AreEqual(58, smoother.Landed());
			Assert::AreEqual(58.0, smoother.Smooth(), 1e-9);
			// A send can never land before the previous one.
			smoother.Plan({ { 300.0, 57 }, { 310.0, 56 } });
			smoother.Sent(400.0);
			smoother.Gain(350.0);
			Assert::AreEqual(58, smoother.Landed(), L"56 waits for 57");
		}

		TEST_METHOD(ContrastIsEmulatedAsAnEncodedGain)
		{
			ScriptController controller{ LedDriveConfig() };
			controller.Reset(100, 50);
			FrameStats stats;
			stats.apl = 5; // contrast target 74
			ControllerOutput out;
			for (int tick = 0; tick < 40; ++tick) out = controller.Tick(stats);
			Assert::AreEqual(74, out.contrast);
			Assert::AreEqual(74.0 / 50.0, out.gain, 1e-12);
			Assert::IsTrue(out.gainDomain == GainDomain::Encoded);
			Assert::AreEqual(0.08, out.knee, 1e-12);
			Assert::AreEqual(100, out.light);
			stats.apl = 120;
			out = controller.Tick(stats);
			Assert::IsTrue(out.gain < 74.0 / 50.0);
			Assert::IsTrue(out.log.find("LED script: apl=120") == 0);
		}

		TEST_METHOD(ConfiguredTablesCurveAndHighlightLevelAreUsed)
		{
			LedDriveConfig config;
			ParseCurve("0:60, 100:255", config.curve);
			config.ledFloor = 60;
			ParseStepTable("100:9, 0:3", config.stepsDown);
			ParseStepTable("0:2", config.stepsUpDark);
			config.scriptHighlightCode = 200;
			ScriptController controller(config);
			Assert::AreEqual(60, controller.LightTargetFor(0));
			Assert::AreEqual(157, controller.LightTargetFor(50));
			controller.Reset(255);
			FrameStats stats;
			stats.apl = 0;
			Assert::AreEqual(246, controller.Tick(stats).light);
			controller.Reset(100);
			stats.apl = 30;
			Assert::AreEqual(102, controller.Tick(stats).light);
			controller.Reset(230, 52);
			stats.apl = 70;
			stats.maximum = 205; // counts as a highlight at 200
			Assert::AreEqual(51, controller.Tick(stats).contrast);
		}
	};
}
