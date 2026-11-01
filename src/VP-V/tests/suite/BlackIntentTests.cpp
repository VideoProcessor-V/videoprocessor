// Black protection (intent interpreter item 4, docs/VP-intent-interpreter-design.md section 2):
// shadow percentiles from codes (PQ and BT.1886 gamma, limited and full range, BTB / code 0
// counted as black and never modified), the black + one 8-bit step threshold, the step count
// between the L1 minimum (or p1) and p10, and the led_protect minimum LED share.
#include "pch.h"
#include "CppUnitTest.h"
#include <BlackIntent.h>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
    std::vector<uint16_t> Codes(std::initializer_list<std::pair<uint16_t, int>> runs)
    {
        std::vector<uint16_t> codes;
        for (const auto& run : runs)
            codes.insert(codes.end(), static_cast<size_t>(run.second), run.first);
        return codes;
    }

    BlackIntent::ShadowStats PqStats(double p1Nits, double p5Nits, double p10Nits)
    {
        BlackIntent::ShadowStats s;
        s.valid = true;
        s.transfer = BlackIntent::Transfer::Pq;
        s.limitedRange = true;
        s.p1Nits = static_cast<float>(p1Nits);
        s.p5Nits = static_cast<float>(p5Nits);
        s.p10Nits = static_cast<float>(p10Nits);
        s.samples = 1000;
        return s;
    }

    float PqStep(int k)
    {
        return SceneSt2084::PqToNits(static_cast<float>(k / 219.0));
    }
}

namespace VideoProcessorTest
{
    TEST_CLASS(BlackIntentTests)
    {
    public:
        TEST_METHOD(GammaPercentilesFromAnEightBitHistogram)
        {
            const std::vector<uint16_t> codes = Codes({ { 16, 1 }, { 20, 4 }, { 30, 5 }, { 100, 90 } });
            const auto s = BlackIntent::ShadowStatsFromCodes(codes.data(), codes.size(), 8, true,
                BlackIntent::Transfer::Gamma, 2.4, 100.0, 0.05);
            Assert::IsTrue(s.valid);
            Assert::AreEqual(100u, s.samples);
            Assert::AreEqual(0.05f, s.p1Nits, 1e-7f, L"p1 = black");
            Assert::AreEqual(static_cast<float>(BlackIntent::Bt1886Nits(4.0 / 219.0, 2.4, 100.0, 0.05)), s.p5Nits, 1e-7f);
            Assert::AreEqual(static_cast<float>(BlackIntent::Bt1886Nits(14.0 / 219.0, 2.4, 100.0, 0.05)), s.p10Nits, 1e-7f);
        }

        TEST_METHOD(PqPercentilesFromATenBitHistogram)
        {
            // 10-bit limited: black = 64, one 8-bit step = 4 codes.
            const std::vector<uint16_t> codes = Codes({ { 64, 2 }, { 64 + 4 * 5, 3 }, { 64 + 4 * 20, 10 }, { 600, 85 } });
            const auto s = BlackIntent::ShadowStatsFromCodes(codes.data(), codes.size(), 10, true,
                BlackIntent::Transfer::Pq, 2.4, 100.0, 0.05);
            Assert::IsTrue(s.valid);
            Assert::AreEqual(0.0f, s.p1Nits, 0.0f, L"PQ black is 0 nit");
            Assert::AreEqual(PqStep(5), s.p5Nits, 1e-7f);
            Assert::AreEqual(PqStep(20), s.p10Nits, 1e-7f);
        }

        TEST_METHOD(BelowBlackAndCodeZeroCountAsBlackAndAreNeverModified)
        {
            const std::vector<uint16_t> codes = Codes({ { 0, 20 }, { 4, 10 }, { 15, 10 }, { 120, 60 } });
            const std::vector<uint16_t> copy = codes;
            const auto gamma = BlackIntent::ShadowStatsFromCodes(codes.data(), codes.size(), 8, true,
                BlackIntent::Transfer::Gamma, 2.4, 100.0, 0.05);
            Assert::AreEqual(0.05f, gamma.p1Nits, 1e-7f);
            Assert::AreEqual(0.05f, gamma.p5Nits, 1e-7f);
            Assert::AreEqual(0.05f, gamma.p10Nits, 1e-7f, L"BTB is black, never below it");
            const auto pq = BlackIntent::ShadowStatsFromCodes(codes.data(), codes.size(), 8, true,
                BlackIntent::Transfer::Pq, 2.4, 100.0, 0.05);
            Assert::AreEqual(0.0f, pq.p10Nits, 0.0f);
            Assert::IsTrue(codes == copy, L"the codes (code 0 included) are only read");
            // Full range: code 0 is black, code 1 the first step.
            const std::vector<uint16_t> full = Codes({ { 0, 5 }, { 1, 5 }, { 200, 90 } });
            const auto f = BlackIntent::ShadowStatsFromCodes(full.data(), full.size(), 8, false,
                BlackIntent::Transfer::Gamma, 2.4, 100.0, 0.05);
            Assert::AreEqual(0.05f, f.p5Nits, 1e-7f);
            Assert::AreEqual(static_cast<float>(BlackIntent::Bt1886Nits(1.0 / 255.0, 2.4, 100.0, 0.05)), f.p10Nits, 1e-7f);
        }

        TEST_METHOD(InvalidInputsGiveNoStatistics)
        {
            const std::vector<uint16_t> codes = Codes({ { 30, 10 } });
            Assert::IsFalse(BlackIntent::ShadowStatsFromCodes(nullptr, 10, 8, true, BlackIntent::Transfer::Pq, 2.4, 100, 0).valid);
            Assert::IsFalse(BlackIntent::ShadowStatsFromCodes(codes.data(), 0, 8, true, BlackIntent::Transfer::Pq, 2.4, 100, 0).valid);
            Assert::IsFalse(BlackIntent::ShadowStatsFromCodes(codes.data(), codes.size(), 7, true, BlackIntent::Transfer::Pq, 2.4, 100, 0).valid);
            Assert::IsFalse(BlackIntent::ShadowStatsFromCodes(codes.data(), codes.size(), 8, true, BlackIntent::Transfer::Unknown, 2.4, 100, 0).valid);
        }

        TEST_METHOD(ThresholdIsBlackPlusOneEightBitStep)
        {
            const double t = BlackIntent::ThresholdNits(48.0, 0.05);
            Assert::AreEqual(BlackIntent::Bt1886Nits(1.0 / 219.0, 2.4, 48.0, 0.05), t, 1e-15);
            Assert::AreEqual(0.0595123, t, 1e-6);
            Assert::AreEqual(100.0 * std::pow(1.0 / 219.0, 2.4), BlackIntent::ThresholdNits(100.0, 0.0), 1e-12,
                L"zero black: pure power law");
            Assert::IsTrue(BlackIntent::ThresholdNits(48.0, 0.05, DisplayEotf::Gamma22) > t, L"2.2 lifts the first step");
            const double pq = BlackIntent::ThresholdNits(1000.0, 0.005, DisplayEotf::Pq);
            Assert::IsTrue(pq > 0.005 && pq < 0.1);
            Assert::AreEqual(0.05, BlackIntent::ThresholdNits(0.05, 0.05), 0.0, L"no peak: black");
        }

        TEST_METHOD(CountsShadowStepsUnderTheThresholdWithAndWithoutL1Min)
        {
            // PQ source: shadow steps k = 1..20 (8-bit limited codes 17..36); display 48 / 0.05 nit,
            // threshold 0.0595 nit: steps 1..10 (up to 0.0489 nit) are under it.
            const auto stats = PqStats(0.0, PqStep(5), PqStep(20));
            const auto noL1 = BlackIntent::Evaluate(BlackIntentMode::Report, stats, IntentField<float>(), 48.0, 0.05);
            Assert::IsTrue(noL1.valid);
            Assert::AreEqual(20, noL1.stepsTotal);
            Assert::AreEqual(10, noL1.stepsBelow);
            Assert::AreEqual(std::string("10 of 20 shadow steps under black+1 (p1 floor)"), std::string(noL1.reason));
            const auto l1 = IntentField<float>::Make(PqStep(10), IntentSource::DvL1);
            const auto withL1 = BlackIntent::Evaluate(BlackIntentMode::Report, stats, l1, 48.0, 0.05);
            Assert::AreEqual(11, withL1.stepsTotal, L"steps 10..20");
            Assert::AreEqual(1, withL1.stepsBelow, L"only step 10 is under black + 1");
            Assert::AreEqual(PqStep(10), withL1.l1MinNits);
            Assert::IsTrue(withL1.l1Source == IntentSource::DvL1);
            Assert::AreEqual(0.0f, withL1.ledMinimumShare, 0.0f, L"report mode computes no LED cap");
        }

        TEST_METHOD(GammaSourceOnAMatchedDisplayLosesNoSteps)
        {
            BlackIntent::ShadowStats s;
            s.valid = true;
            s.transfer = BlackIntent::Transfer::Gamma;
            s.gamma = 2.4;
            s.sdrWhiteNits = 48.0;
            s.blackNits = 0.05;
            s.p1Nits = 0.05f;
            s.p10Nits = static_cast<float>(BlackIntent::Bt1886Nits(10.0 / 219.0, 2.4, 48.0, 0.05));
            const auto r = BlackIntent::Evaluate(BlackIntentMode::Report, s, IntentField<float>(), 48.0, 0.05);
            Assert::AreEqual(10, r.stepsTotal);
            Assert::AreEqual(0, r.stepsBelow, L"the first code above black is exactly the threshold, visible");
        }

        TEST_METHOD(OffAndMissingStatisticsReportNothing)
        {
            const auto stats = PqStats(0.0, PqStep(5), PqStep(20));
            const auto off = BlackIntent::Evaluate(BlackIntentMode::Off, stats, IntentField<float>(), 48.0, 0.05);
            Assert::IsFalse(off.valid);
            Assert::AreEqual(std::string("off"), std::string(off.reason));
            const auto none = BlackIntent::Evaluate(BlackIntentMode::Report, BlackIntent::ShadowStats(), IntentField<float>(), 48.0, 0.05);
            Assert::IsFalse(none.valid);
            Assert::AreEqual(std::string("no shadow statistics"), std::string(none.reason));
        }

        TEST_METHOD(LedMinimumShareEdgeCases)
        {
            const double black = 0.05, peak = 48.0;
            const double t = BlackIntent::ThresholdNits(peak, black);
            Assert::AreEqual(0.0, BlackIntent::LedMinimumShare(BlackIntent::ShadowStats(), peak, black), 0.0, L"no statistics");
            Assert::AreEqual(0.0, BlackIntent::LedMinimumShare(PqStats(0, black, 1), peak, black), 0.0, L"p5 black");
            Assert::AreEqual(0.0, BlackIntent::LedMinimumShare(PqStats(0, t * 0.99, 1), peak, black), 0.0,
                L"p5 already invisible: nothing to keep");
            const double p5 = black + 4.0 * (t - black);
            Assert::AreEqual(0.25, BlackIntent::LedMinimumShare(PqStats(0, p5, 1), peak, black), 1e-6,
                L"s x (p5 - B) = one step");
            const double justAbove = BlackIntent::LedMinimumShare(PqStats(0, t * 1.001, 1), peak, black);
            Assert::IsTrue(justAbove > 0.9 && justAbove <= 1.0);
            Assert::AreEqual(0.0, BlackIntent::LedMinimumShare(PqStats(0, 1, 1), black, black), 0.0, L"no peak");
            // Brighter shadows allow deeper dimming (monotonic).
            Assert::IsTrue(BlackIntent::LedMinimumShare(PqStats(0, 2.0, 3), peak, black) <
                BlackIntent::LedMinimumShare(PqStats(0, 0.5, 3), peak, black));
            const auto r = BlackIntent::Evaluate(BlackIntentMode::LedProtect, PqStats(0, p5, PqStep(20)),
                IntentField<float>(), peak, black);
            Assert::AreEqual(0.25f, r.ledMinimumShare, 1e-5f, L"led_protect reports the share");
        }
    };
}
