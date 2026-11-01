// Display mapping readout (intent interpreter item 5, docs/VP-intent-interpreter-design.md
// section 3): compression need in PQ and stops for several display peaks, never an expansion,
// the L2 trim closest to the display peak, the measured (inferred) fallback, and the DV-card line.
#include "pch.h"
#include "CppUnitTest.h"
#include <DisplayIntent.h>
#include <cmath>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
    SceneDescriptor DvScene(float sceneMaxNits)
    {
        SceneDescriptor d;
        d.source = SceneMetadataSource::DvTunnelL1;
        d.valid = true;
        d.sceneMax = SceneLuminance::FromNits(sceneMaxNits);
        d.sceneAvg = SceneLuminance::FromNits(sceneMaxNits / 10.0f);
        return d;
    }

    void AddL2(SceneDescriptor& d, float targetNits)
    {
        SceneTrim& t = d.trims[d.trimCount++];
        t.level = 2;
        t.target = SceneLuminance::FromPqCode12(static_cast<uint16_t>(
            std::lround(SceneSt2084::NitsToPq(targetNits) * 4095.0f)));
    }

    void AddL8(SceneDescriptor& d)
    {
        SceneTrim& t = d.trims[d.trimCount++];
        t.level = 8;
        t.targetDisplayIndex = 1;
    }

    DisplayProfile Display(float peak)
    {
        DisplayProfile p;
        p.peakNits = peak;
        p.blackNits = 0.05f;
        p.eotf = DisplayEotf::Bt1886;
        return p;
    }
}

namespace VideoProcessorTest
{
    TEST_CLASS(DisplayIntentTests)
    {
    public:
        TEST_METHOD(CompressionForSeveralPeaks)
        {
            const SceneDescriptor scene = DvScene(1000.0f);
            for (const float peak : { 48.0f, 100.0f, 300.0f })
            {
                const auto s = DisplayIntent::Evaluate(true, scene, Display(peak), 0.0);
                Assert::IsTrue(s.valid);
                Assert::IsTrue(s.sceneMaxSource == IntentSource::DvL1);
                Assert::AreEqual(static_cast<float>(std::log2(1000.0 / peak)), s.compressionStops, 1e-4f);
                Assert::AreEqual(SceneSt2084::NitsToPq(1000.0f) - SceneSt2084::NitsToPq(peak), s.compressionPq, 1e-6f);
                Assert::IsTrue(s.compressionPq > 0.0f);
            }
            Assert::AreEqual(4.3808f, DisplayIntent::Evaluate(true, scene, Display(48.0f), 0.0).compressionStops, 1e-3f);
        }

        TEST_METHOD(NeverExpandsAboveTheMaster)
        {
            const SceneDescriptor scene = DvScene(1000.0f);
            for (const float peak : { 1000.0f, 2000.0f, 4000.0f })
            {
                const auto s = DisplayIntent::Evaluate(true, scene, Display(peak), 0.0);
                Assert::IsTrue(s.valid);
                Assert::AreEqual(0.0f, s.compressionStops, 0.0f);
                Assert::AreEqual(0.0f, s.compressionPq, 0.0f);
            }
            Assert::AreEqual(std::string("scene 1000 nit -> 2000 nit (0.0 stops), no trim"),
                std::string(DisplayIntent::Evaluate(true, scene, Display(2000.0f), 0.0).line));
        }

        TEST_METHOD(ChoosesTheL2TrimClosestToTheDisplayPeak)
        {
            SceneDescriptor scene = DvScene(1000.0f);
            AddL8(scene);
            AddL2(scene, 100.0f);
            AddL2(scene, 600.0f);
            AddL2(scene, 1000.0f);
            Assert::AreEqual(100.0f, DisplayIntent::Evaluate(true, scene, Display(48.0f), 0.0).trimNits, 0.5f);
            Assert::AreEqual(600.0f, DisplayIntent::Evaluate(true, scene, Display(500.0f), 0.0).trimNits, 1.0f);
            Assert::AreEqual(1000.0f, DisplayIntent::Evaluate(true, scene, Display(900.0f), 0.0).trimNits, 1.5f);
            // L8 only (no parsed target): no trim.
            SceneDescriptor l8 = DvScene(1000.0f);
            AddL8(l8);
            Assert::AreEqual(0.0f, DisplayIntent::Evaluate(true, l8, Display(48.0f), 0.0).trimNits, 0.0f);
        }

        TEST_METHOD(LineFormat)
        {
            SceneDescriptor scene = DvScene(1000.0f);
            AddL2(scene, 100.0f);
            Assert::AreEqual(std::string("scene 1000 nit -> 48 nit (-4.4 stops), trim L2 100 nit"),
                std::string(DisplayIntent::Evaluate(true, scene, Display(48.0f), 0.0).line));
            Assert::AreEqual(std::string("scene 1000 nit -> 48 nit (-4.4 stops), no trim"),
                std::string(DisplayIntent::Evaluate(true, DvScene(1000.0f), Display(48.0f), 0.0).line));
            Assert::AreEqual(std::string("scene 4 nit -> 48 nit (0.0 stops), no trim"),
                std::string(DisplayIntent::Evaluate(true, DvScene(4.0f), Display(48.0f), 0.0).line));
        }

        TEST_METHOD(InferredFromTheMeasurementWithoutL1)
        {
            SceneDescriptor none;   // no metadata at all
            const auto s = DisplayIntent::Evaluate(true, none, Display(48.0f), 800.0);
            Assert::IsTrue(s.valid);
            Assert::IsTrue(s.sceneMaxSource == IntentSource::Inferred);
            Assert::AreEqual(800.0f, s.sceneMaxNits);
            Assert::AreEqual(std::string("scene 800 nit -> 48 nit (-4.1 stops), no trim [inferred]"), std::string(s.line));
            // L1 wins over the measurement.
            const auto l1 = DisplayIntent::Evaluate(true, DvScene(1000.0f), Display(48.0f), 800.0);
            Assert::IsTrue(l1.sceneMaxSource == IntentSource::DvL1);
            Assert::AreEqual(1000.0f, l1.sceneMaxNits, 0.01f);
            // Neither: not valid.
            const auto nothing = DisplayIntent::Evaluate(true, none, Display(48.0f), 0.0);
            Assert::IsFalse(nothing.valid);
            Assert::IsTrue(nothing.sceneMaxSource == IntentSource::Off);
        }

        TEST_METHOD(OffReportsNothing)
        {
            const auto s = DisplayIntent::Evaluate(false, DvScene(1000.0f), Display(48.0f), 800.0);
            Assert::IsFalse(s.enabled);
            Assert::IsFalse(s.valid);
            Assert::AreEqual(std::string(), std::string(s.line));
            const auto noPeak = DisplayIntent::Evaluate(true, DvScene(1000.0f), Display(0.0f), 0.0);
            Assert::IsFalse(noPeak.valid);
        }
    };
}
