// Ambient adaptation (intent interpreter item 3, docs/VP-intent-interpreter-design.md section 4):
// the lux filter's step responses (asymmetric taus), median rejection, sensor loss hold and
// decay, the 1/6-stop quantisation with hysteresis, cut-only jumps and the slew limit, and the
// guards (strength 0 / off bit-identical, monotonic, cap, never below the display black).
#include "pch.h"
#include "CppUnitTest.h"
#include <AmbientAdaptation.h>
#include <cmath>
#include <cstring>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
    AmbientAdapt::Settings On()
    {
        AmbientAdapt::Settings s;
        s.enabled = true;
        s.strength = 1.0;
        s.screenReflectNitsPerLux = 0.01;
        s.maxBlackNits = 1.0;
        s.tauUpSeconds = 15.0;
        s.tauDownSeconds = 90.0;
        return s;
    }

    double LogState(const AmbientAdapt::Filter& f)
    {
        return std::log2(f.SmoothedLux() + AmbientAdapt::kLuxOffset);
    }

    bool SameBits(double a, double b)
    {
        return std::memcmp(&a, &b, sizeof(double)) == 0;
    }

    // The lux that gives B_eff = B x 2^(x/6) at strength 1.
    double LuxForStep(const AmbientAdapt::Settings& s, double black, double x)
    {
        return (black * std::exp2(x / 6.0) - black) / s.screenReflectNitsPerLux;
    }
}

namespace VideoProcessorTest
{
    TEST_CLASS(AmbientAdaptationTests)
    {
    public:
        TEST_METHOD(StepUpFollowsTauUpInLogLux)
        {
            AmbientAdapt::Filter f;
            f.Configure(On());
            double t = 0.0;
            for (; t < 10.0; t += 1.0) f.AddReading(0.0, t);
            Assert::AreEqual(0.0, f.SmoothedLux(), 0.0, L"dark room: exactly 0 lux");
            const double from = LogState(f);
            const double to = std::log2(100.0 + AmbientAdapt::kLuxOffset);
            // The median of 5 turns at the third 100-lux reading (t = 12).
            f.AddReading(100.0, 10.0);
            f.AddReading(100.0, 11.0);
            Assert::AreEqual(from, LogState(f), 1e-12, L"two readings do not move the median");
            for (t = 12.0; t < 12.0 + 15.0; t += 1.0) f.AddReading(100.0, t);
            const double fraction = (LogState(f) - from) / (to - from);
            Assert::AreEqual(1.0 - std::exp(-1.0), fraction, 1e-9, L"63 % after tau_up (15 s)");
        }

        TEST_METHOD(StepDownFollowsTauDown)
        {
            AmbientAdapt::Filter f;
            f.Configure(On());
            double t = 0.0;
            for (; t < 10.0; t += 1.0) f.AddReading(200.0, t);
            const double from = LogState(f);
            const double to = std::log2(5.0 + AmbientAdapt::kLuxOffset);
            f.AddReading(5.0, 10.0);
            f.AddReading(5.0, 11.0);
            for (t = 12.0; t < 12.0 + 90.0; t += 1.0) f.AddReading(5.0, t);
            const double fraction = (LogState(f) - from) / (to - from);
            Assert::AreEqual(1.0 - std::exp(-1.0), fraction, 1e-9, L"63 % after tau_down (90 s)");
            // Asymmetric: after 15 s down it is far less than 63 % of the way.
            AmbientAdapt::Filter g;
            g.Configure(On());
            for (t = 0.0; t < 10.0; t += 1.0) g.AddReading(200.0, t);
            for (; t < 12.0 + 15.0; t += 1.0) g.AddReading(5.0, t);
            Assert::IsTrue((LogState(g) - from) / (to - from) < 0.2);
        }

        TEST_METHOD(MedianRejectsFlickerAndAPersonWalkingPast)
        {
            AmbientAdapt::Filter f;
            f.Configure(On());
            double t = 0.0;
            for (; t < 10.0; t += 1.0) f.AddReading(50.0, t);
            const double steady = f.SmoothedLux();
            f.AddReading(5000.0, t); t += 1.0;   // a flash / lamp flicker
            Assert::IsTrue(SameBits(steady, f.SmoothedLux()), L"one outlier changes nothing");
            for (int i = 0; i < 3; ++i) { f.AddReading(50.0, t); t += 1.0; }
            f.AddReading(1.0, t); t += 1.0;       // a person shading the sensor for two readings
            f.AddReading(1.0, t); t += 1.0;
            Assert::IsTrue(SameBits(steady, f.SmoothedLux()), L"two outliers change nothing");
            f.AddReading(std::nan(""), t);
            Assert::IsTrue(SameBits(steady, f.SmoothedLux()), L"non-finite readings are ignored");
        }

        TEST_METHOD(SensorLossHoldsThenDecaysTowardOff)
        {
            AmbientAdapt::Filter f;
            f.Configure(On());
            for (double t = 0.0; t <= 10.0; t += 1.0) f.AddReading(100.0, t);
            const double held = f.SmoothedLux();
            f.Tick(14.0);
            Assert::IsFalse(f.Holding(), L"within the loss time nothing happens");
            f.Tick(20.0);
            Assert::IsTrue(f.Holding());
            Assert::IsTrue(SameBits(held, f.SmoothedLux()), L"held exactly");
            f.Tick(10.0 + AmbientAdapt::kSensorLossSeconds + AmbientAdapt::kHoldSeconds);
            Assert::IsTrue(f.Holding() && SameBits(held, f.SmoothedLux()), L"held for the whole hold time");
            f.Tick(10.0 + AmbientAdapt::kSensorLossSeconds + AmbientAdapt::kHoldSeconds + 30.0);
            Assert::IsTrue(f.Decaying());
            Assert::IsTrue(f.SmoothedLux() < held && f.SmoothedLux() > 0.0, L"decaying toward off");
            Assert::IsTrue(f.Valid());
            f.Tick(10000.0);
            Assert::IsTrue(f.Lost());
            Assert::IsFalse(f.Valid(), L"decayed to off: invalid");
            Assert::AreEqual(0.0, f.SmoothedLux(), 0.0);
            AmbientIntentStatus status;
            AmbientAdapt::FillStatus(On(), f, 0.05, 0.05, status);
            Assert::AreEqual(std::string("sensor lost: decayed to off"), std::string(status.reason));
            // A new reading starts afresh (no ramp from the old state).
            f.AddReading(30.0, 10001.0);
            Assert::AreEqual(30.0, f.SmoothedLux(), 1e-9);
        }

        TEST_METHOD(StrengthZeroAndOffAreBitIdentical)
        {
            AmbientAdapt::Settings zero = On();
            zero.strength = 0.0;
            AmbientAdapt::Settings off = On();
            off.enabled = false;
            for (const double black : { 0.0, 1e-6, 0.0137, 0.05, 0.31 })
                for (const double lux : { 0.0, 0.3, 12.0, 250.0, 1e5 })
                    for (const AmbientAdapt::Settings& s : { zero, off })
                    {
                        Assert::IsTrue(SameBits(black, AmbientAdapt::EffectiveBlackNits(s, black, lux)));
                        Assert::IsTrue(SameBits(black, AmbientAdapt::QuantisedBlackNits(s, black, lux)));
                        Assert::IsTrue(SameBits(black, AmbientAdapt::QuantisedBlackNits(s, black, lux, black * 3.0 + 0.1)));
                    }
            // No veil (0 lux) with the function on: the display black exactly too.
            Assert::IsTrue(SameBits(0.0137, AmbientAdapt::QuantisedBlackNits(On(), 0.0137, 0.0)));
            Assert::IsTrue(SameBits(0.0137, AmbientAdapt::QuantisedBlackNits(On(), 0.0137, 0.0, 0.0137)));
        }

        TEST_METHOD(MonotonicInLuxAndCapped)
        {
            const AmbientAdapt::Settings s = On();
            const double black = 0.05;
            double previousEff = 0.0, previousQ = 0.0;
            for (double lux = 0.0; lux < 20000.0; lux = lux * 1.07 + 0.1)
            {
                const double eff = AmbientAdapt::EffectiveBlackNits(s, black, lux);
                const double q = AmbientAdapt::QuantisedBlackNits(s, black, lux);
                Assert::IsTrue(eff >= previousEff && q >= previousQ, L"monotonic");
                Assert::IsTrue(eff >= black && q >= black, L"never below the display black");
                Assert::IsTrue(eff <= s.maxBlackNits && q <= eff, L"capped; quantisation rounds down");
                previousEff = eff;
                previousQ = q;
            }
            Assert::AreEqual(s.maxBlackNits, AmbientAdapt::EffectiveBlackNits(s, black, 1e6), 0.0);
            // A cap below the display black leaves the display black.
            AmbientAdapt::Settings low = s;
            low.maxBlackNits = 0.01;
            Assert::AreEqual(black, AmbientAdapt::EffectiveBlackNits(low, black, 1e6), 0.0);
            // Strength scales the veil.
            AmbientAdapt::Settings half = s;
            half.strength = 0.5;
            Assert::AreEqual(black + 0.5 * 10.0 * 0.01, AmbientAdapt::EffectiveBlackNits(half, black, 10.0), 1e-15);
        }

        TEST_METHOD(QuantisesToSixthStopStepsWithHysteresis)
        {
            const AmbientAdapt::Settings s = On();
            const double black = 0.05;
            for (int k = 0; k < 12; ++k)
            {
                const double q = AmbientAdapt::QuantisedBlackNits(s, black, LuxForStep(s, black, k + 0.5));
                const double expected = k == 0 ? black : black * std::exp2(k / 6.0);
                Assert::AreEqual(expected, q, 1e-12, (L"step " + std::to_wstring(k)).c_str());
            }
            const double step1 = black * std::exp2(1.0 / 6.0);
            const double step2 = black * std::exp2(2.0 / 6.0);
            auto q = [&](double x, double previous)
            {
                return AmbientAdapt::QuantisedBlackNits(s, black, LuxForStep(s, black, x), previous);
            };
            Assert::AreEqual(step1, q(0.9, step1), 1e-12, L"slightly below the boundary: holds");
            Assert::AreEqual(step1, q(0.76, step1), 1e-12);
            Assert::AreEqual(black, q(0.7, step1), 0.0, L"well below: one step down, to B exactly");
            Assert::AreEqual(step1, q(2.2, step1), 1e-12, L"slightly above the next boundary: holds");
            Assert::AreEqual(step2, q(2.3, step1), 1e-12);
            Assert::AreEqual(black, q(1.1, black), 0.0, L"rising from B: the first step needs 1.25");
            Assert::AreEqual(step1, q(1.3, black), 1e-12);
            // A value dithering across a boundary never flickers.
            double previous = step1;
            for (int i = 0; i < 100; ++i)
            {
                previous = q(i % 2 ? 0.95 : 1.05, previous);
                Assert::AreEqual(step1, previous, 1e-12);
            }
        }

        TEST_METHOD(JumpsOnlyAtCutsAndSlewsBelowOnePercentPerSecond)
        {
            Assert::AreEqual(0.08, AmbientAdapt::NextAppliedBlackNits(0.05, 0.08, true, 0.0), 0.0, L"a cut jumps");
            const double up = AmbientAdapt::NextAppliedBlackNits(0.05, 0.08, false, 1.0);
            Assert::IsTrue(up > 0.05 && (up - 0.05) / 0.05 < 0.01, L"slew up below 1 %/s");
            const double down = AmbientAdapt::NextAppliedBlackNits(0.08, 0.05, false, 1.0);
            Assert::IsTrue(down < 0.08 && (0.08 - down) / 0.08 < 0.01, L"slew down below 1 %/s");
            Assert::AreEqual(0.05, AmbientAdapt::NextAppliedBlackNits(0.05, 0.08, false, 0.0), 0.0, L"no time: no move");
            // A static picture reaches the target without overshoot.
            double applied = 0.05;
            for (int s = 0; s < 200; ++s) applied = AmbientAdapt::NextAppliedBlackNits(applied, 0.06, false, 1.0);
            Assert::AreEqual(0.06, applied, 0.0);
            const double same = 0.0137;
            Assert::IsTrue(SameBits(same, AmbientAdapt::NextAppliedBlackNits(same, same, true, 5.0)));
        }

        TEST_METHOD(FillStatusReportsValuesAndReasons)
        {
            AmbientAdapt::Filter f;
            AmbientAdapt::Settings s = On();
            f.Configure(s);
            AmbientIntentStatus status;
            AmbientAdapt::FillStatus(s, f, 0.05, 0.05, status);
            Assert::AreEqual(std::string("no lux source"), std::string(status.reason));
            for (double t = 0.0; t < 5.0; t += 1.0) f.AddReading(20.0, t);
            AmbientAdapt::FillStatus(s, f, 0.05, 0.05, status);
            Assert::IsTrue(status.enabled && status.sensorValid);
            Assert::AreEqual(20.0f, status.smoothedLux, 1e-4f);
            Assert::AreEqual(0.2f, status.veilNits, 1e-5f);
            Assert::IsTrue(status.targetBlackNits > 0.05f);
            Assert::AreEqual(std::string("waiting for a cut / slewing"), std::string(status.reason));
            s.strength = 0.0;
            AmbientAdapt::FillStatus(s, f, 0.05, 0.05, status);
            Assert::AreEqual(0.05f, status.targetBlackNits);
            Assert::AreEqual(std::string("strength 0: display black"), std::string(status.reason));
        }
    };
}
