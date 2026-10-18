// LED drive intent limits (intent interpreter items 3 and 4, LedDriveModule::SetIntentLimits):
// 0 / 0 leaves both controllers' command streams identical, the led_protect shadow cap and the
// ambient veil floor hold in compensate and internal_script, the peak drive is never raised, and
// the share -> level / ambient share derivations.
#include "pch.h"
#include "CppUnitTest.h"

#include <led/LedController.h>
#include <led/LedDriveModule.h>
#include <led/LedMeasurement.h>

#include "LedDriveTestSupport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

namespace
{
    RawMeasurement IntentFrame(double code, uint32_t bright = 0, int highlight = 235, uint32_t blocks = 10000)
    {
        RawMeasurement raw{};
        raw.histogram[static_cast<int>(code)] += blocks - bright;
        raw.histogram[highlight] += bright;
        raw.blocks = blocks;
        raw.sum = static_cast<uint32_t>((code * (blocks - bright) + highlight * bright) * SUM_SCALE);
        for (int i = 0; i < GRID_W * GRID_H; ++i)
        {
            raw.cellCount[i] = 4;
            raw.cellSum[i] = static_cast<uint32_t>(4 * code * SUM_SCALE);
        }
        return raw;
    }

    LedDriveConfig CompensateDefaults()
    {
        LedDriveConfig config;
        config.enabled = true;
        config.mode = Mode::Compensate;
        config.ledFloor = 60;
        config.ledFloorReactive = 60;
        return config;
    }

    // A varied scene: dark, highlights, a fade, a cut to black, a bright cut, mid scenes.
    std::vector<RawMeasurement> Scenes()
    {
        std::vector<RawMeasurement> frames;
        auto add = [&](const RawMeasurement& raw, int count) { for (int i = 0; i < count; ++i) frames.push_back(raw); };
        add(IntentFrame(18.0), 240);
        add(IntentFrame(18.0, 60, 230), 120);
        for (int i = 0; i < 60; ++i) frames.push_back(IntentFrame(120.0 - i * 1.6));
        add(IntentFrame(16.0), 90);
        add(IntentFrame(180.0, 2000, 240), 120);
        add(IntentFrame(40.0), 120);
        add(IntentFrame(24.0, 600, 235), 120);
        add(IntentFrame(16.0), 60);
        add(IntentFrame(90.0), 120);
        return frames;
    }

    bool SameOutput(const ControllerOutput& a, const ControllerOutput& b)
    {
        return a.lightCommand == b.lightCommand && a.lightTarget == b.lightTarget && a.light == b.light &&
            a.contrastTarget == b.contrastTarget && a.contrast == b.contrast &&
            a.contrastSchedule.size() == b.contrastSchedule.size() && a.gain == b.gain &&
            a.durationMs == b.durationMs && a.highlightCap == b.highlightCap && a.capWeight == b.capWeight &&
            a.vpHeadroom == b.vpHeadroom && a.holdGain == b.holdGain && a.jump == b.jump &&
            a.brightCut == b.brightCut && a.darkCut == b.darkCut && a.preposition == b.preposition &&
            a.cutLeadHold == b.cutLeadHold && a.cutHighlight == b.cutHighlight && a.cutCeiling == b.cutCeiling &&
            a.log == b.log;
    }

    // Runs a compensate controller over the scenes, with a look-ahead of
    // `aheadFrames` (predicted / covered), and calls `check` per output.
    void RunCompensate(CompensateController& controller, const LedDriveConfig& config, int aheadFrames,
        const std::function<void(const ControllerOutput&)>& check)
    {
        const std::vector<RawMeasurement> frames = Scenes();
        StatsBuilder builder(config), aheadBuilder(config);
        std::vector<FrameStats> stats;
        double t = 0.0;
        for (const RawMeasurement& raw : frames)
        {
            stats.push_back(builder.Build(raw, t));
            t += 1000.0 / 60.0;
        }
        for (size_t i = 0; i < stats.size(); ++i)
        {
            if (aheadFrames > 0 && i + aheadFrames < stats.size())
            {
                Lookahead ahead;
                ahead.stats = stats[i + aheadFrames];
                ahead.framesAhead = aheadFrames;
                for (size_t j = i + 1; j <= i + aheadFrames; ++j) ahead.cutAhead = ahead.cutAhead || stats[j].cut;
                check(controller.Tick(stats[i + 2 < stats.size() ? i + 2 : i], 1000.0 / 60.0, &ahead, true, true));
            }
            else
                check(controller.Tick(stats[i], 1000.0 / 60.0));
        }
    }

    bool WaitUntilTrue(const std::function<bool()>& condition, int timeoutMs = 4000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return condition();
    }

    std::vector<int> LightWrites(FakeProjector& projector)
    {
        std::vector<int> levels;
        for (const std::string& write : projector.Writes())
            if (write.rfind(":LPW1", 0) == 0 && write.size() > 5 && std::isdigit(static_cast<unsigned char>(write[5])))
                levels.push_back(std::stoi(write.substr(5)));
        return levels;
    }
}

namespace VideoProcessorTest
{
    TEST_CLASS(LedIntentLimitsTests)
    {
    public:
        TEST_METHOD(ZeroLimitsLeaveTheCompensateStreamIdentical)
        {
            for (const int ahead : { 0, 12 })
            {
                const LedDriveConfig config = CompensateDefaults();
                CompensateController plain(config), limited(config);
                limited.SetIntentFloor(0);
                std::vector<ControllerOutput> a, b;
                RunCompensate(plain, config, ahead, [&](const ControllerOutput& out) { a.push_back(out); });
                RunCompensate(limited, config, ahead, [&](const ControllerOutput& out) { b.push_back(out); });
                Assert::AreEqual(a.size(), b.size());
                int commands = 0;
                for (size_t i = 0; i < a.size(); ++i)
                {
                    Assert::IsTrue(SameOutput(a[i], b[i]), (L"frame " + std::to_wstring(i)).c_str());
                    commands += a[i].lightCommand >= 0;
                }
                Assert::IsTrue(commands > 10, L"the scenes do move the LED");
            }
        }

        TEST_METHOD(ZeroLimitsLeaveTheScriptStreamIdentical)
        {
            LedDriveConfig config = CompensateDefaults();
            config.mode = Mode::Script;
            ScriptController plain(config), limited(config);
            plain.Reset(255, 50);
            limited.Reset(255, 50);
            limited.SetIntentFloor(0);
            StatsBuilder builder(config);
            double t = 0.0;
            int commands = 0;
            for (const RawMeasurement& raw : Scenes())
            {
                const FrameStats stats = builder.Build(raw, t);
                t += 400.0;
                const ControllerOutput a = plain.Tick(stats);
                const ControllerOutput b = limited.Tick(stats);
                Assert::IsTrue(SameOutput(a, b));
                for (size_t i = 0; i < a.contrastSchedule.size(); ++i)
                    Assert::IsTrue(a.contrastSchedule[i].contrast == b.contrastSchedule[i].contrast &&
                        a.contrastSchedule[i].offsetMs == b.contrastSchedule[i].offsetMs);
                commands += a.lightCommand >= 0;
            }
            Assert::IsTrue(commands > 5);
        }

        TEST_METHOD(ShadowCapHoldsInCompensate)
        {
            for (const int ahead : { 0, 12 })
            {
                const LedDriveConfig config = CompensateDefaults();
                CompensateController controller(config);
                controller.Reset(150);   // as if dimmed to the floor already (the ramp is slow)
                controller.SetIntentFloor(150);
                int lowest = 255;
                RunCompensate(controller, config, ahead, [&](const ControllerOutput& out)
                {
                    Assert::IsTrue(out.lightTarget >= 150, L"every target respects the floor");
                    lowest = std::min(lowest, out.light);
                });
                Assert::AreEqual(150, lowest, L"dark scenes, black and fades never go below the floor");
            }
        }

        TEST_METHOD(PeakDriveIsNeverRaised)
        {
            LedDriveConfig config = CompensateDefaults();
            config.ledCeiling = 200;
            config.curve = { { 0, 60 }, { 20, 60 }, { 40, 150 }, { 70, 180 }, { 100, 200 }, { 255, 200 } };
            CompensateController controller(config);
            controller.Reset(200);
            controller.SetIntentFloor(250);   // asks for more than the ceiling
            RunCompensate(controller, config, 12, [&](const ControllerOutput& out)
            {
                Assert::IsTrue(out.light <= 200 && out.lightTarget <= 200, L"never above led_ceiling");
            });
            // Through the module: a share of 1 is the ceiling, never more.
            LedDriveModule module(ProjectorConfig(), config);
            module.SetIntentLimits(1.0, 0.0);
            Assert::AreEqual(200, module.IntentFloorLevel());
            module.SetIntentLimits(5.0, 0.0);
            Assert::AreEqual(200, module.IntentFloorLevel(), L"clamped share");
        }

        TEST_METHOD(ShadowCapAndAmbientFloorHoldInTheScript)
        {
            LedDriveConfig config = CompensateDefaults();
            config.mode = Mode::Script;
            ScriptController controller(config);
            controller.Reset(170, 50);
            controller.SetIntentFloor(170);
            Assert::AreEqual(170, controller.LightTargetFor(10), L"dark: the floor, not the script's 60/100");
            Assert::AreEqual(255, controller.LightTargetFor(120), L"bright: the script's own level");
            controller.SetFloor(ScriptFloor::Covered);
            Assert::AreEqual(170, controller.LightTargetFor(5));
            StatsBuilder builder(config);
            double t = 0.0;
            int lowest = 255;
            for (const RawMeasurement& raw : Scenes())
            {
                const ControllerOutput out = controller.Tick(builder.Build(raw, t));
                t += 400.0;
                Assert::IsTrue(out.lightTarget >= 170);
                lowest = std::min(lowest, out.light);
            }
            Assert::AreEqual(170, lowest);
            // The floor is carried into the look-ahead plan (its controller copy).
            ScriptController copy = controller;
            Assert::AreEqual(170, copy.LightTargetFor(0));
        }

        TEST_METHOD(FloorShareDerivations)
        {
            Assert::AreEqual(0.0, LedDriveModule::IntentFloorShare(0.0, 0.0, 0.0), 0.0, L"0 / 0: none");
            Assert::AreEqual(0.3, LedDriveModule::IntentFloorShare(0.3, 0.0, 0.0), 0.0);
            const double step = std::pow(2.0, 1.0 / 6.0) - 1.0;
            Assert::AreEqual(step * 0.10 / 0.05, LedDriveModule::IntentFloorShare(0.0, 0.15, 0.05), 1e-12,
                L"V = 0.1 nit over B = 0.05 nit");
            Assert::AreEqual(0.3, LedDriveModule::IntentFloorShare(0.3, 0.06, 0.05), 1e-12, L"the larger floor wins");
            Assert::AreEqual(1.0, LedDriveModule::IntentFloorShare(0.0, 50.0, 0.05), 0.0, L"bright room: no dimming");
            Assert::AreEqual(0.0, LedDriveModule::IntentFloorShare(0.0, 0.05, 0.05), 0.0, L"no veil");
            Assert::AreEqual(0.0, LedDriveModule::IntentFloorShare(0.0, 0.2, 0.0), 0.0, L"unknown display black");
            Assert::IsTrue(LedDriveModule::IntentFloorShare(0.0, 0.3, 0.05) > LedDriveModule::IntentFloorShare(0.0, 0.1, 0.05),
                L"more veil, higher floor");

            LedDriveModule module(ProjectorConfig(), CompensateDefaults());
            Assert::AreEqual(0, module.IntentFloorLevel());
            module.SetIntentLimits(0.5, 0.0);
            Assert::AreEqual(128, module.IntentFloorLevel(), L"linear flux: lowest level >= half of 255");
            module.SetIntentLimits(0.0, 0.15, 0.05);
            Assert::AreEqual(static_cast<int>(std::ceil(step * 2.0 * 255.0)), module.IntentFloorLevel());
            module.SetIntentLimits(0.0, 0.15);   // the display black given earlier is kept
            Assert::AreEqual(static_cast<int>(std::ceil(step * 2.0 * 255.0)), module.IntentFloorLevel());
            module.SetIntentLimits(0.0, 0.0);
            Assert::AreEqual(0, module.IntentFloorLevel(), L"0 / 0 clears the floor");
            Assert::AreEqual(0.0, module.IntentFloorShareNow(), 0.0);
            // A flux table changes the level for the same share.
            ProjectorConfig table;
            table.fluxTable = { { 0, 0.0 }, { 128, 0.25 }, { 255, 1.0 } };
            LedDriveModule fluxModule(table, CompensateDefaults());
            fluxModule.SetIntentLimits(0.25, 0.0);
            Assert::AreEqual(128, fluxModule.IntentFloorLevel());
        }

        TEST_METHOD(ModuleCompensateHoldsTheFloorOnThePort)
        {
            auto projector = std::make_shared<FakeProjector>();
            projector->contrast = 50;
            // The regression recipe's fixed ramp (rate_down 120 levels/s), so the
            // LED reaches the floor within the frames fed.
            LedDriveConfig config = CompensateDefaults();
            config.cntrAssist = false;
            config.rateGainUp = config.rateGainDown = 0.0;
            config.maxCutDip = 0.0;
            config.shapedRecovery = false;
            config.blackPreliftMax = 255;
            ProjectorConfig port;
            port.protocol = "barco_ascii";
            port.ackTimeoutMs = 60;
            port.minCommandGapMs = 0;
            port.readBack = true;
            port.ledLatencyMs = 80;
            port.ledSettleMs = 0;
            LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); });
            module.SetIntentLimits(0.6, 0.0);
            Assert::AreEqual(153, module.IntentFloorLevel());
            module.Start();
            Assert::IsTrue(WaitUntilTrue([&] { return module.DrivingProjector() && projector->light == 255; }),
                L"compensate started");
            double time = 0.0;
            for (int frame = 0; frame < 240; ++frame)
            {
                module.SubmitMeasurement(IntentFrame(frame < 120 ? 18.0 : 16.0));
                time += 1000.0 / 60.0;
                module.BeginFrameAt(0, time);
                WaitUntilTrue([&] { return !module.ProjectorStatus().busy; }, 500);
            }
            Assert::IsTrue(WaitUntilTrue([&] { return projector->light == 153; }), L"dimmed to the floor");
            module.Stop();
            const std::vector<int> levels = LightWrites(*projector);
            Assert::IsFalse(levels.empty());
            for (size_t i = 0; i + 1 < levels.size(); ++i)   // the last write is the exit restore (255)
                Assert::IsTrue(levels[i] >= 153 && levels[i] <= 255,
                    (L"LPW1 " + std::to_wstring(levels[i])).c_str());
            Assert::AreEqual(255, levels.back(), L"exit restores full light");
        }
    };
}
