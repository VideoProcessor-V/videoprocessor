#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/drive/projector_control/LedController.h>
#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>
#include <vprenderer/AlphaQueuePolicy.h>
#include <VP-V/modules/input/queue_timing/AlphaQueuePolicyVpCompat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

// script_lookahead (owner 2026-10-17): the internal script copy decides from the
// queued frame on screen when its command lands, jumps to a brighter cut's targets
// landing on its first frame, pre-lifts out of black within black_prelift_max, uses
// led_floor only when the queue covers the projector's response, and falls back to
// the frame on screen exactly when nothing is queued. Queue = look-ahead.
namespace
{
	struct Scene
	{
		std::function<double(uint64_t)> apl;
		std::function<double(uint64_t)> maximum;
		std::function<bool(uint64_t)> cut;
		std::function<double(uint64_t)> highlight;   // empty: max(apl, 16)
	};

	FrameStats StatsOf(const Scene& scene, uint64_t sequence)
	{
		FrameStats stats;
		stats.apl = scene.apl(sequence);
		stats.maximum = scene.maximum(sequence);
		stats.highlight = scene.highlight ? scene.highlight(sequence) : std::min(stats.maximum, std::max(stats.apl, 16.0));
		stats.sustainedApl = stats.apl;
		stats.cut = scene.cut ? scene.cut(sequence) : false;
		stats.blocks = 100;
		return stats;
	}

	struct Landing
	{
		double ms;
		bool light;
		int value;
		bool operator==(const Landing& other) const
		{
			return std::abs(ms - other.ms) < 1e-6 && light == other.light && value == other.value;
		}
	};

	// The module's script tick on a virtual clock (script pacing): the same planner
	// and controller calls, the sends at their times, each landing its latency later.
	struct Simulation
	{
		LedDriveConfig config;
		double fps = 24.0;
		int ledLatencyMs = 80;
		int cntrLatencyMs = 80;
		int ledSettleMs = 300;
		size_t queueFrames = 0;
		bool lookahead = true;
		int startLight = 255;
		int startContrast = 50;
		double commandGapMs = 200.0;   // min_command_gap_ms
		std::vector<Landing> landings;
		double longestTickMs = 0.0;
		int cutJumps = 0;

		double Period() const { return 1000.0 / fps; }
		uint64_t OnScreen(double ms) const { return 1 + static_cast<uint64_t>(std::floor(ms / Period() + 1e-9)); }
		double Display(uint64_t sequence) const { return static_cast<double>(sequence - 1) * Period(); }

		void Run(const Scene& scene, double durationMs)
		{
			config.mode = Mode::Script;
			ScriptController controller(config);
			controller.Reset(startLight, startContrast);
			double t = 0.0;
			uint64_t lastOnScreen = 0;
			while (t < durationMs)
			{
				const double tickStart = t;
				const uint64_t onScreen = OnScreen(t);
				FrameStats stats = StatsOf(scene, onScreen);
				// As the module: a cut since the last tick is flagged on this one.
				for (uint64_t s = lastOnScreen + 1; s < onScreen && scene.cut; ++s)
					stats.cut = stats.cut || scene.cut(s);
				lastOnScreen = onScreen;
				double done = t;
				auto send = [&](double at, bool light, int value)
				{
					at = std::max(at, done);
					landings.push_back({ at + (light ? ledLatencyMs : cntrLatencyMs), light, value });
					done = at + std::max(commandGapMs, static_cast<double>((light ? 2 : 1) * config.scriptEchoMs));
				};
				if (lookahead)
				{
					ScriptLookaheadInput in;
					for (uint64_t s = onScreen + 1; s <= onScreen + queueFrames; ++s)
						in.ahead.push_back({ s, StatsOf(scene, s) });
					in.onScreenSequence = onScreen;
					in.onScreenMs = Display(onScreen);
					in.nowMs = t;
					in.sourcePeriodMs = Period();
					in.sendOffsetMs = config.scriptMeasureMs;
					in.nextTickMs = ScriptTickBoundMs(config);
					in.ledLatencyMs = ledLatencyMs;
					in.ledSettleMs = ledSettleMs;
					in.cntrLatencyMs = cntrLatencyMs;
					in.commandGapMs = commandGapMs;
					in.light = controller.Light();
					in.contrast = controller.Contrast();
					const ScriptLookaheadPlan plan = PlanScriptLookahead(config, controller, stats, in);
					controller.SetFloor(plan.floor);
					controller.SetLightHold(plan.kind == ScriptLookaheadPlan::Kind::PreLift ? plan.lightHold : -1);
					if (plan.kind == ScriptLookaheadPlan::Kind::CutJump)
					{
						++cutJumps;
						for (const ScriptTimedSend& s : plan.sends)
						{
							send(s.atMs, s.light, s.value);
							if (s.light) controller.Reset(s.value, controller.Contrast());
							else controller.Reset(controller.Light(), s.value);
						}
						t = std::max(done, tickStart + std::max(config.scriptMeasureMs, 10));
						longestTickMs = std::max(longestTickMs, t - tickStart);
						continue;
					}
					if (plan.kind == ScriptLookaheadPlan::Kind::PreLift && plan.preliftTarget >= 0)
					{
						send(t + in.sendOffsetMs, true, plan.preliftTarget);
						controller.Reset(plan.preliftTarget, controller.Contrast());
					}
					if (plan.kind == ScriptLookaheadPlan::Kind::Ahead)
						stats = plan.stats;
				}
				const ControllerOutput out = controller.Tick(stats);
				std::vector<ScriptSend> sends;
				const int lengthMs = PaceScriptTick(out, config.scriptMeasureMs, config.scriptEchoMs, sends);
				for (const ScriptSend& s : sends)
					send(tickStart + s.offsetMs, s.light, s.value);
				t = std::max(done, tickStart + std::max(lengthMs, 10));
				longestTickMs = std::max(longestTickMs, t - tickStart);
			}
		}

		// The light on screen at a time: the last LPW1 landed by then.
		int LightAt(double ms) const
		{
			int light = startLight;
			double best = -1e300;
			for (const Landing& landing : landings)
				if (landing.light && landing.ms <= ms && landing.ms >= best) { light = landing.value; best = landing.ms; }
			return light;
		}
	};

	Scene StepScene(uint64_t cutFrame, double before, double after, bool cutFlag, double maxBefore = 120.0,
		double maxAfter = 235.0)
	{
		Scene scene;
		scene.apl = [=](uint64_t s) { return s < cutFrame ? before : after; };
		scene.maximum = [=](uint64_t s) { return s < cutFrame ? maxBefore : maxAfter; };
		scene.cut = [=](uint64_t s) { return cutFlag && s == cutFrame; };
		return scene;
	}

	// A varied programme: slow APL drift, highlights coming and going, cuts.
	Scene Programme()
	{
		Scene scene;
		scene.apl = [](uint64_t s)
		{
			const double drift = 60.0 + 45.0 * std::sin(static_cast<double>(s) / 37.0);
			return (s / 90) % 3 == 1 ? 18.0 : drift;
		};
		scene.maximum = [](uint64_t s) { return (s / 23) % 2 ? 230.0 : 150.0; };
		scene.cut = [](uint64_t s) { return s % 90 == 0; };
		return scene;
	}

	size_t QueueFor(const Simulation& sim)
	{
		// Deep enough to cover measure + latency + settle and the next tick.
		return static_cast<size_t>(std::ceil((sim.config.scriptMeasureMs + sim.ledLatencyMs + sim.ledSettleMs +
			500.0) / sim.Period())) + 2;
	}

	struct QueueEntry
	{
		bool cadenceRepeat = false;
	};
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedScriptLookaheadTests)
	{
	public:
		// Off, or nothing queued (Low Latency, depth 0): the script's command stream
		// is exactly as before. A queue short of the landing frame still looks ahead
		// (its last frame, and a cut in it jumps at once).
		TEST_METHOD(NoQueueOrSwitchOffKeepsTheScriptStream)
		{
			for (double fps : { 24.0, 60.0 })
			{
				Simulation off;
				off.fps = fps;
				off.lookahead = false;
				off.Run(Programme(), 60000.0);
				for (size_t queue : { size_t{ 0 } })
				{
					Simulation none = off;
					none.landings.clear();
					none.lookahead = true;
					none.queueFrames = queue;
					none.Run(Programme(), 60000.0);
					Assert::IsTrue(!off.landings.empty(), L"the programme commands the projector");
					Assert::AreEqual(off.landings.size(), none.landings.size(), L"same number of commands");
					Assert::IsTrue(std::equal(off.landings.begin(), off.landings.end(), none.landings.begin()),
						(L"identical script stream with " + std::to_wstring(queue) + L" queued at " +
							std::to_wstring(static_cast<int>(fps)) + L" fps").c_str());
				}
				// A short queue (Normal's four frames ahead): a bright cut seen in it is
				// sent at once and lands earlier than the reactive script's.
				const uint64_t cut = static_cast<uint64_t>(std::lround(4000.0 * fps / 1000.0));
				const Scene scene = StepScene(cut, 30.0, 90.0, true, 150.0, 235.0);
				Simulation reactive;
				reactive.fps = fps;
				reactive.lookahead = false;
				reactive.startLight = ScriptController(reactive.config).LightTargetFor(30);
				reactive.startContrast = ScriptController::ContrastTargetFor(30, false);
				Simulation shortQueue = reactive;
				shortQueue.lookahead = true;
				shortQueue.queueFrames = 4;
				reactive.Run(scene, 8000.0);
				shortQueue.Run(scene, 8000.0);
				const int target = ScriptController(reactive.config).LightTargetFor(90);
				auto firstAt = [&](const Simulation& sim)
				{
					for (const Landing& l : sim.landings)
						if (l.light && l.value == target) return l.ms;
					return 1e300;
				};
				Assert::IsTrue(shortQueue.cutJumps >= 1, L"a short queue still jumps to a bright cut");
				Assert::IsTrue(firstAt(shortQueue) < firstAt(reactive), L"and its light lands earlier than reacting");
				Assert::IsTrue(firstAt(shortQueue) >= reactive.Display(cut) - reactive.Period() - 1e-6,
					L"but not before the cut's frame");
			}
		}

		// In-scene: the script's step goes out so it lands on the frame it was decided
		// for, within one tick, at 24/50/60 Hz and 50-150 ms latency - a fall on the
		// frame that darkens, a rise led_settle_ms ahead of the frame that brightens
		// (so the light has settled by then). A brighter cut's targets land on its
		// first frame.
		TEST_METHOD(CommandsLandOnTheTargetFrame)
		{
			for (double fps : { 24.0, 50.0, 60.0 })
				for (int latency : { 50, 80, 100, 125, 150 })
				{
					const std::wstring where = std::to_wstring(static_cast<int>(fps)) + L" fps, " +
						std::to_wstring(latency) + L" ms";
					// In-scene rise (no cut): the first LPW1 for the new level.
					{
						Simulation sim;
						sim.fps = fps;
						sim.ledLatencyMs = sim.cntrLatencyMs = latency;
						sim.startLight = ScriptController(sim.config).LightTargetFor(50);
						sim.queueFrames = QueueFor(sim);
						const uint64_t change = static_cast<uint64_t>(std::lround(4000.0 / sim.Period()));
						sim.Run(StepScene(change, 50.0, 65.0, false, 150.0, 150.0), 8000.0);
						const double changeMs = sim.Display(change);
						const auto first = std::find_if(sim.landings.begin(), sim.landings.end(),
							[&](const Landing& l) { return l.light && l.value > sim.startLight; });
						Assert::IsTrue(first != sim.landings.end(), (L"the rise is commanded, " + where).c_str());
						const double riseMs = changeMs - sim.ledSettleMs;
						Assert::IsTrue(first->ms >= riseMs - sim.Period() - 1e-6,
							(L"the rise lands no earlier than its settle time before the frame, " + where).c_str());
						Assert::IsTrue(first->ms <= riseMs + sim.longestTickMs + 1e-6,
							(L"the rise lands within one tick of its settle time before the frame, " + where).c_str());
						// Without look-ahead it lands at least the send time + latency late.
						Simulation late = sim;
						late.landings.clear();
						late.lookahead = false;
						late.Run(StepScene(change, 50.0, 65.0, false, 150.0, 150.0), 8000.0);
						const auto lateFirst = std::find_if(late.landings.begin(), late.landings.end(),
							[&](const Landing& l) { return l.light && l.value > sim.startLight; });
						Assert::IsTrue(lateFirst != late.landings.end() &&
							lateFirst->ms >= changeMs + sim.config.scriptMeasureMs + latency - 1e-6,
							(L"without look-ahead the step is late, " + where).c_str());
					}
					// In-scene fall: the first lower LPW1 lands on the darker frame, within a tick.
					{
						Simulation sim;
						sim.fps = fps;
						sim.ledLatencyMs = sim.cntrLatencyMs = latency;
						sim.startLight = ScriptController(sim.config).LightTargetFor(65);
						sim.queueFrames = QueueFor(sim);
						const uint64_t change = static_cast<uint64_t>(std::lround(4000.0 / sim.Period()));
						sim.Run(StepScene(change, 65.0, 50.0, false, 150.0, 150.0), 8000.0);
						const double changeMs = sim.Display(change);
						const auto first = std::find_if(sim.landings.begin(), sim.landings.end(),
							[&](const Landing& l) { return l.light && l.value < sim.startLight; });
						Assert::IsTrue(first != sim.landings.end(), (L"the fall is commanded, " + where).c_str());
						Assert::IsTrue(first->ms >= changeMs - sim.Period() - 1e-6,
							(L"the fall lands no earlier than its frame, " + where).c_str());
						Assert::IsTrue(first->ms <= changeMs + sim.longestTickMs + 1e-6,
							(L"the fall lands within one tick of its frame, " + where).c_str());
					}
					// A bright cut: the scene's light lands on its first frame.
					{
						Simulation sim;
						sim.fps = fps;
						sim.ledLatencyMs = sim.cntrLatencyMs = latency;
						sim.startLight = ScriptController(sim.config).LightTargetFor(30);
						sim.startContrast = ScriptController::ContrastTargetFor(30, false);
						sim.queueFrames = QueueFor(sim);
						const uint64_t cut = static_cast<uint64_t>(std::lround(4000.0 / sim.Period()));
						const Scene scene = StepScene(cut, 30.0, 90.0, true, 150.0, 235.0);
						sim.Run(scene, 8000.0);
						const double cutMs = sim.Display(cut);
						const int target = ScriptController(sim.config).LightTargetFor(90);
						Assert::AreEqual(target, sim.LightAt(cutMs + sim.Period() - 1e-3),
							(L"the cut's light is on by its first frame, " + where).c_str());
						Assert::IsTrue(sim.LightAt(cutMs - sim.Period()) < target,
							(L"and not before the frame ahead of it, " + where).c_str());
						Assert::IsTrue(sim.cutJumps >= 1, (L"the cut jump was used, " + where).c_str());
					}
				}
		}

		// Out of black: the light is at the bright scene's target by its first frame,
		// and while the picture is still black it rises no higher than the pre-lift
		// cap (black_prelift_max; 255 = no pre-positioning: nothing before the cut).
		TEST_METHOD(OutOfBlackOnTimeWithoutEarlyBrightening)
		{
			for (double fps : { 24.0, 50.0, 60.0 })
				for (int latency : { 50, 100, 150 })
					for (int cap : { 120, 255 })
					{
						const std::wstring where = std::to_wstring(static_cast<int>(fps)) + L" fps, " +
							std::to_wstring(latency) + L" ms, cap " + std::to_wstring(cap);
						Simulation sim;
						sim.fps = fps;
						sim.config.blackPreliftMax = cap;
						sim.ledLatencyMs = sim.cntrLatencyMs = latency;
						sim.startLight = ScriptController(sim.config).LightTargetFor(16);
						sim.startContrast = 74;
						// A deep queue (3 s more): the bright scene is seen well before its
						// commands fall due, so the pre-lift has time.
						sim.queueFrames = QueueFor(sim) + static_cast<size_t>(3000.0 / sim.Period());
						const uint64_t cut = static_cast<uint64_t>(std::lround(5000.0 / sim.Period()));
						// Truly black (APL 16, nothing brighter) until the cut; no cut flag
						// needed - black to picture is the entry.
						sim.Run(StepScene(cut, 16.0, 85.0, false, 16.0, 235.0), 9000.0);
						const double cutMs = sim.Display(cut);
						const int target = ScriptController(sim.config).LightTargetFor(85);
						Assert::AreEqual(target, sim.LightAt(cutMs + sim.Period() - 1e-3),
							(L"at the target by the first bright frame, " + where).c_str());
						const int allowed = cap >= 255 ? sim.startLight : std::max(sim.startLight, cap);
						for (const Landing& landing : sim.landings)
							if (landing.light && landing.ms < cutMs - 1e-6)
								Assert::IsTrue(landing.value <= allowed,
									(L"no brightening above the cap during black, " + where).c_str());
						if (cap < 255)
							Assert::AreEqual(cap, sim.LightAt(cutMs - sim.Period()),
								(L"the pre-lift is on before the cut, " + where).c_str());
						// CNTR: the bright scene's value lands with the cut, not later.
						int contrast = sim.startContrast;
						double best = -1e300;
						for (const Landing& landing : sim.landings)
							if (!landing.light && landing.ms <= cutMs + sim.Period() && landing.ms >= best)
							{
								contrast = landing.value;
								best = landing.ms;
							}
						Assert::AreEqual(ScriptController::ContrastTargetFor(85, true), contrast,
							(L"the scene's contrast lands with the cut, " + where).c_str());
					}
		}

		// led_floor needs the look-ahead to cover the projector's response: then the
		// script's bottom band continues its curve down to it; otherwise the higher
		// of led_floor and led_floor_reactive holds; switch off: led_floor as before.
		TEST_METHOD(DeepFloorOnlyWithCoverage)
		{
			LedDriveConfig config;
			config.mode = Mode::Script;
			config.ledFloor = 60;
			config.ledFloorReactive = 100;
			ScriptController controller(config);
			Assert::AreEqual(100, controller.LightTargetFor(5), L"legacy: the script's own bottom band");
			controller.SetFloor(ScriptFloor::Reactive);
			Assert::AreEqual(100, controller.LightTargetFor(5), L"reactive floor");
			controller.SetFloor(ScriptFloor::Covered);
			Assert::AreEqual(60, controller.LightTargetFor(5), L"covered: down to led_floor");
			Assert::AreEqual(80, controller.LightTargetFor(16), L"covered: the curve's slope below APL 20");
			Assert::AreEqual(125, controller.LightTargetFor(25), L"the script's curve above its bottom band");
			Assert::AreEqual(ScriptController(config).LightTargetFor(60), controller.LightTargetFor(60));

			for (double fps : { 24.0, 60.0 })
			{
				const uint64_t cut = static_cast<uint64_t>(std::lround(30000.0 / (1000.0 / fps)));
				// A dark scene with some picture in it (not black: no pre-lift).
				Scene scene = StepScene(cut, 5.0, 90.0, true, 100.0, 235.0);
				scene.highlight = [cut](uint64_t s) { return s < cut ? 60.0 : 235.0; };
				Simulation covered;
				covered.config = config;
				covered.fps = fps;
				covered.startLight = 100;
				covered.startContrast = 74;
				covered.queueFrames = QueueFor(covered);
				covered.Run(scene, 34000.0);
				const double cutMs = covered.Display(cut);
				Assert::AreEqual(60, covered.LightAt(cutMs - 2.0 * covered.Period()), L"the deeper floor is reached");
				for (const Landing& landing : covered.landings)
					if (landing.light && landing.ms < cutMs - 1e-6)
						Assert::IsTrue(landing.value <= 100, L"nothing brightens before the cut");
				Assert::AreEqual(ScriptController(config).LightTargetFor(90), covered.LightAt(cutMs + covered.Period() - 1e-3),
					L"a bright cut after the deep floor lands on target by the cut frame");

				for (size_t queue : { size_t{ 0 }, size_t{ 2 } })
				{
					Simulation reactive = covered;
					reactive.landings.clear();
					reactive.queueFrames = queue;
					reactive.Run(scene, 29000.0);
					Assert::AreEqual(100, reactive.LightAt(29000.0), L"without coverage the reactive floor holds");
				}
			}
		}

		// The planner itself: coverage and the fallback.
		TEST_METHOD(PlannerCoverageAndFallback)
		{
			LedDriveConfig config;
			config.mode = Mode::Script;
			ScriptController controller(config);
			FrameStats current;
			current.apl = 50.0;
			current.maximum = 150.0;
			ScriptLookaheadInput in;
			in.onScreenSequence = 100;
			in.onScreenMs = 0.0;
			in.nowMs = 0.0;
			in.sourcePeriodMs = 1000.0 / 24.0;
			in.sendOffsetMs = 150.0;
			in.ledLatencyMs = 80;
			in.ledSettleMs = 300;
			ScriptLookaheadPlan plan = PlanScriptLookahead(config, controller, current, in);
			Assert::IsTrue(plan.kind == ScriptLookaheadPlan::Kind::Current && !plan.covered, L"empty queue");
			Assert::AreEqual(current.apl, plan.stats.apl);
			// A darkening queue (APL falling a step a frame): the landing frame's.
			for (uint64_t s = 101; s <= 106; ++s)
			{
				FrameStats f = current;
				f.apl = static_cast<double>(150 - s);
				in.ahead.push_back({ s, f });
			}
			plan = PlanScriptLookahead(config, controller, current, in);
			// 150 + 80 ms = frame 105 (5.52 frames); settle not covered (530 ms = frame 112).
			Assert::IsTrue(plan.kind == ScriptLookaheadPlan::Kind::Ahead, L"the landing frame is queued");
			Assert::AreEqual<uint64_t>(105, plan.targetSequence);
			Assert::AreEqual(45.0, plan.stats.apl);
			Assert::IsTrue(!plan.covered && plan.floor == ScriptFloor::Reactive, L"settle not covered");
			for (uint64_t s = 107; s <= 113; ++s)
			{
				FrameStats f = current;
				f.apl = static_cast<double>(150 - s);
				in.ahead.push_back({ s, f });
			}
			plan = PlanScriptLookahead(config, controller, current, in);
			Assert::IsTrue(plan.covered && plan.floor == ScriptFloor::Covered, L"latency + settle covered");
			Assert::AreEqual<uint64_t>(105, plan.targetSequence, L"a fall: the landing frame");
			// A brightening queue: the frame where the rise will have settled (112).
			for (auto& frame : in.ahead) frame.second.apl = frame.first < 110 ? 30.0 : 90.0;
			plan = PlanScriptLookahead(config, controller, current, in);
			Assert::IsTrue(plan.kind == ScriptLookaheadPlan::Kind::Ahead, L"a rise is looked ahead");
			Assert::AreEqual<uint64_t>(112, plan.targetSequence, L"a rise: where it will have settled");
			for (auto& frame : in.ahead) frame.second.apl = static_cast<double>(150 - frame.first);
			in.ahead.resize(4);   // frames 101-104: short of the landing frame
			plan = PlanScriptLookahead(config, controller, current, in);
			Assert::IsTrue(plan.kind == ScriptLookaheadPlan::Kind::Ahead, L"a short queue looks ahead");
			Assert::AreEqual<uint64_t>(104, plan.targetSequence, L"a short queue: its last frame");
			// The landing time on the frame on screen (a slow source): that frame.
			in.sourcePeriodMs = 1000.0;
			in.ahead.resize(1);
			plan = PlanScriptLookahead(config, controller, current, in);
			Assert::IsTrue(plan.kind == ScriptLookaheadPlan::Kind::Current, L"landing on the frame on screen");
		}

		// Queue = look-ahead (owner 2026-10-17): the LED asks for the whole queue in
		// every mode with look-ahead; led_lookahead_ms is a cap; off asks for nothing.
		TEST_METHOD(WantedDepthIsTheWholeQueue)
		{
			LedDriveConfig script;
			script.mode = Mode::Script;
			{
				LedDriveModule module(ProjectorConfig(), script);
				Assert::AreEqual(LedDriveModule::WHOLE_QUEUE, module.WantedLookaheadFrames(), L"script: whole queue");
				Assert::IsTrue(module.WantsPredictions(), L"script with look-ahead wants the queued frames");
			}
			script.scriptLookahead = false;
			{
				LedDriveModule module(ProjectorConfig(), script);
				Assert::AreEqual<size_t>(0, module.WantedLookaheadFrames(), L"script_lookahead off");
				Assert::IsFalse(module.WantsPredictions());
			}
			script.scriptLookahead = true;
			script.ledLookaheadMs = 100;
			{
				LedDriveModule module(ProjectorConfig(), script);
				module.BeginFrameAt(1, 0.0, 24.0);
				Assert::AreEqual<size_t>(2, module.WantedLookaheadFrames(), L"a 100 ms cap at 24p");
			}
			LedDriveConfig compensate;
			compensate.mode = Mode::Compensate;
			{
				LedDriveModule module(ProjectorConfig(), compensate);
				Assert::AreEqual(LedDriveModule::WHOLE_QUEUE, module.WantedLookaheadFrames(), L"compensate: whole queue");
			}
			compensate.lookahead = false;
			{
				LedDriveModule module(ProjectorConfig(), compensate);
				Assert::AreEqual<size_t>(0, module.WantedLookaheadFrames(), L"lookahead off");
			}
		}

		// The renderer's walk with the LED asking for the whole queue: every queued
		// frame whatever active_picture_lookahead_frames says (Normal holds 5, a deeper
		// profile more), none ahead under Low Latency (one frame), a cap applied.
		TEST_METHOD(WalkDepthFollowsTheQueueProfile)
		{
			struct Profile { const wchar_t* name; size_t queued; };
			for (const Profile& profile : { Profile{ L"Normal", 5 }, Profile{ L"Reliable", 12 }, Profile{ L"Low_Latency", 1 } })
			{
				const std::vector<QueueEntry> queue(profile.queued);
				for (size_t activePicture : { size_t{ 0 }, size_t{ 2 } })
				{
					const auto walk = AlphaQueuePolicy::SelectLookaheadWalk(queue, activePicture,
						LedDriveModule::WHOLE_QUEUE, 8);
					Assert::AreEqual(profile.queued - 1, walk.window.indices.empty() ? 0 : walk.window.indices.size() - 1,
						profile.name);
					Assert::AreEqual(profile.queued - 1, walk.window.availableFutureFrames, profile.name);
					Assert::AreEqual(activePicture == 0 ? size_t{ 0 } : std::min(profile.queued, activePicture + 1),
						walk.activePictureFrames, profile.name);
				}
				// led_lookahead_ms's cap (here 3 frames) below the queue depth.
				const auto capped = AlphaQueuePolicy::SelectLookaheadWalk(queue, 0, 3, 8);
				Assert::AreEqual(std::min<size_t>(3, profile.queued - 1),
					capped.window.indices.empty() ? 0 : capped.window.indices.size() - 1, profile.name);
			}
			// Low Latency: no frames ahead, so the module has no look-ahead depth and
			// the script copy falls back to the frame on screen (see the stream test).
			LedDriveConfig script;
			script.mode = Mode::Script;
			LedDriveModule module(ProjectorConfig(), script);
			module.SetLookaheadDepth(0);
			Assert::AreEqual<size_t>(0, module.LookaheadDepth());
		}
	};
}
