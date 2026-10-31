// Ambient adaptation (intent interpreter item 3, docs/VP-intent-interpreter-design.md section 4):
// the lux filter's step responses (asymmetric taus), median rejection, sensor loss hold and
// decay, the 1/6-stop quantisation with hysteresis, cut-only jumps and the slew limit, and the
// guards (strength 0 / off bit-identical, monotonic, cap, never below the display black).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/colour/appearance/AmbientAdaptation.h>
#include <IntentInterpreterSupport.h>
#include <vprenderer/LibplaceboRenderParameters.h>
#include "ExtendedRangeGpuProbe.h"
#include <algorithm>
#include <vector>
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

    // Hunt 3 F2: ambient adaptation for SDR input (LibplaceboRenderParameters::SdrAmbientLift).
    // Off / strength 0 / a dark room: nothing changes (bit-identical). Lit room at strength 1:
    // the BT.1886 black lift V' = s V + o reaches the output codes - black raised, white fixed,
    // code order (BTB < black < ... < WTW) kept, and on the owner's display the picture shows
    // as on a display whose black is the effective black.
    namespace AmbientSdr
    {
        // B_eff from the real stepper (a cut, the filter settled on `lux`).
        inline double EffectiveBlack(double strength, double lux, double owner)
        {
            AmbientAdapt::Settings s = On();
            s.strength = strength;
            IntentInterpreter::AmbientApplyState st;
            return IntentInterpreter::AmbientStep(st, s, true, lux, owner, true, 1.0);
        }

        inline double Bt1886(double v, double black, double white)
        {
            const double lb = std::pow(black, 1.0 / 2.4), lw = std::pow(white, 1.0 / 2.4);
            return std::pow((std::max)(0.0, (lw - lb) * v + lb), 2.4);
        }

        inline int Lifted(int code, float contrast, float brightness)
        {
            const double v = (code - 64) / 876.0;
            return static_cast<int>(std::lround(64.0 + 876.0 * (contrast * v + brightness)));
        }
    }

    TEST_CLASS(AmbientSdrLiftTests)
    {
    public:
        TEST_METHOD(AmbientSdrStrengthZeroChangesNothing)
        {
            for (double owner : { 0.0, 0.02, 0.05 })
            {
                const double eff0 = AmbientSdr::EffectiveBlack(0.0, 200.0, owner);
                Assert::IsTrue(SameBits(owner, eff0), L"strength 0: the owner's black exactly");
                float c = 7.0f, b = 7.0f;
                Assert::IsFalse(LibplaceboRenderParameters::SdrAmbientLift(owner, eff0, 100.0, PL_COLOR_TRC_BT_1886, c, b),
                    L"strength 0: no adjustment");
                Assert::IsFalse(LibplaceboRenderParameters::SdrAmbientLift(owner, AmbientSdr::EffectiveBlack(1.0, 0.0, owner), 100.0,
                    PL_COLOR_TRC_BT_1886, c, b), L"a dark room at strength 1: no adjustment");
            }
        }

        TEST_METHOD(AmbientSdrLiftReachesTheCodes)
        {
            // Output codes of a 10-bit limited SDR ramp (BTB 4 .. WTW 1019) at strength 0 and 1 in
            // a lit room (200 lux x 0.01 cd/m2 per lux: a 2-nit veil, capped at 1 nit) on a 100-nit
            // BT.1886 display with black 0.05 nit.
            const double owner = 0.05, white = 100.0;
            const double eff1 = AmbientSdr::EffectiveBlack(1.0, 200.0, owner);
            Assert::IsTrue(eff1 > owner, L"(lit room: the effective black rose)");
            float c = 1.0f, b = 0.0f;
            Assert::IsTrue(LibplaceboRenderParameters::SdrAmbientLift(owner, eff1, white, PL_COLOR_TRC_BT_1886, c, b));
            Assert::IsTrue(c > 0.0f && c < 1.0f && b > 0.0f, L"a lift: contrast below 1, brightness above 0");
            Assert::AreEqual(1.0, double(c) + double(b), 1e-6, L"white fixed (s + o = 1)");
            const int codes[] = { 4, 16, 40, 64, 70, 80, 100, 128, 200, 300, 512, 700, 900, 940, 980, 1019 };
            int last = -1;
            std::string row0, row1;
            for (int code : codes)
            {
                const int l1 = AmbientSdr::Lifted(code, c, b);
                row0 += std::to_string(code) + " ";
                row1 += std::to_string(l1) + " ";
                if (code <= 940)
                    Assert::IsTrue(l1 >= code, L"never lowered (BTB rises with black)");
                Assert::IsTrue(l1 > last, L"code order kept");
                last = l1;
            }
            Logger::WriteMessage(("strength 0: " + row0 + "\nstrength 1: " + row1).c_str());
            Assert::IsTrue(AmbientSdr::Lifted(64, c, b) > 64, L"black raised");
            Assert::AreEqual(940, AmbientSdr::Lifted(940, c, b), L"white unchanged");
            Assert::IsTrue(AmbientSdr::Lifted(4, c, b) < AmbientSdr::Lifted(64, c, b), L"BTB stays below black");
            Assert::IsTrue(AmbientSdr::Lifted(1019, c, b) > 940, L"WTW stays above white");
            // On the owner's display the lifted signal shows as on a display with black B_eff.
            for (int i = 0; i <= 100; ++i)
            {
                const double v = i / 100.0;
                const double shown = AmbientSdr::Bt1886(c * v + b, owner, white);
                Assert::AreEqual(AmbientSdr::Bt1886(v, eff1, white), shown, 1e-5 * white);
            }
            // Pure power transfers follow their own exponent; piecewise / unknown ones are left alone.
            float c2 = 1.0f, b2 = 0.0f;
            Assert::IsTrue(LibplaceboRenderParameters::SdrAmbientLift(owner, eff1, white, PL_COLOR_TRC_GAMMA22, c2, b2));
            Assert::IsFalse(LibplaceboRenderParameters::SdrAmbientLift(owner, eff1, white, PL_COLOR_TRC_SRGB, c2, b2));
            Assert::IsFalse(LibplaceboRenderParameters::SdrAmbientLift(owner, eff1, white, PL_COLOR_TRC_PQ, c2, b2));
            // On top of an existing adjustment: RGB' = s (c RGB + b) + o.
            pl_color_adjustment adj = pl_color_adjustment_neutral;
            adj.saturation = 1.2f;
            adj.contrast = 1.1f;
            adj.brightness = 0.01f;
            LibplaceboRenderParameters::ApplySdrAmbientLift(c, b, adj);
            Assert::AreEqual(1.1f * c, adj.contrast, 1e-6f);
            Assert::AreEqual(0.01f * c + b, adj.brightness, 1e-6f);
            Assert::AreEqual(1.2f, adj.saturation);
        }
    };

    // GPU (for Claude: WARP or VP_TEST_GPU=hardware): the real libplacebo render of a BT.1886 SDR
    // grey ramp with extended range on, strength 0 vs 1.
    TEST_CLASS(AmbientSdrGpuTests)
    {
    public:
        TEST_METHOD(AmbientSdrGpuLiftReachesTheOutputCodes)
        {
            pl_log_params logParams{};
            logParams.log_level = PL_LOG_NONE;
            pl_log log = pl_log_create(PL_API_VER, &logParams);
            std::string device;
            const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
            Logger::WriteMessage(("GPU tests on: " + device + "\n").c_str());
            pl_d3d11 d3d = pl_d3d11_create(log, &deviceParams);
            Assert::IsNotNull(d3d, L"Could not create the libplacebo test device");
            pl_renderer renderer = pl_renderer_create(log, d3d->gpu);
            const std::vector<int> codes = { 4, 40, 64, 80, 128, 300, 600, 900, 940, 1019 };
            const ExtendedRangeGpuProbe::Planes planes = ExtendedRangeGpuProbe::Greys(codes);
            ExtendedRangeGpuProbe::Case base;
            base.sourceTransfer = base.outputTransfer = PL_COLOR_TRC_BT_1886;
            base.sourcePrimaries = base.outputPrimaries = PL_COLOR_PRIM_BT_709;
            pl_render_params plain = pl_render_default_params;
            base.params = &plain;
            const ExtendedRangeGpuProbe::Result none = ExtendedRangeGpuProbe::Render(d3d->gpu, renderer, planes, base);
            Assert::IsTrue(none.ok, ExtendedRangeGpuProbe::ToWide(none.error).c_str());
            const double owner = 0.05, white = 100.0;
            std::vector<ExtendedRangeGpuProbe::Result> results;
            float c1 = 1.0f, b1 = 0.0f;
            for (double strength : { 0.0, 1.0 })
            {
                pl_render_params p = plain;
                pl_color_adjustment adj = pl_color_adjustment_neutral;
                float c = 1.0f, b = 0.0f;
                if (LibplaceboRenderParameters::SdrAmbientLift(owner, AmbientSdr::EffectiveBlack(strength, 200.0, owner), white,
                    PL_COLOR_TRC_BT_1886, c, b))
                {
                    LibplaceboRenderParameters::ApplySdrAmbientLift(c, b, adj);
                    p.color_adjustment = &adj;
                    c1 = c;
                    b1 = b;
                }
                ExtendedRangeGpuProbe::Case k = base;
                k.params = &p;
                results.push_back(ExtendedRangeGpuProbe::Render(d3d->gpu, renderer, planes, k));
                Assert::IsTrue(results.back().ok, ExtendedRangeGpuProbe::ToWide(results.back().error).c_str());
            }
            const std::vector<int> c0 = ExtendedRangeGpuProbe::Columns(results[0], int(codes.size()), 1.0);
            const std::vector<int> cl = ExtendedRangeGpuProbe::Columns(results[1], int(codes.size()), 1.0);
            const std::vector<int> cn = ExtendedRangeGpuProbe::Columns(none, int(codes.size()), 1.0);
            Logger::WriteMessage(("no adjustment " + ExtendedRangeGpuProbe::Join(cn) + "\nstrength 0 " + ExtendedRangeGpuProbe::Join(c0) +
                "\nstrength 1 " + ExtendedRangeGpuProbe::Join(cl)).c_str());
            Assert::IsTrue(results[0].pixels == none.pixels, L"strength 0: bit-identical output");
            for (size_t i = 0; i < codes.size(); ++i)
                Assert::IsTrue(std::abs(cl[i] - AmbientSdr::Lifted(codes[i], c1, b1)) <= 1, L"strength 1: the predicted lift (+-1 code)");
            Assert::IsTrue(cl[2] > c0[2] && cl[8] == c0[8], L"black raised, white unchanged");
            Assert::IsTrue(cl[0] < cl[2] && cl[9] > cl[8], L"BTB / WTW kept");
            pl_renderer_destroy(&renderer);
            pl_d3d11_destroy(&d3d);
            pl_log_destroy(&log);
        }
    };
}
