// Motion intent (intent interpreter item 2, docs/VP-intent-interpreter-design.md, owner
// decisions 2026-10-17): the five policies on synthetic shots, the shared rules (never
// above 1, the owner minimum, cut-only application, missed cut, dead band, short-shot
// inherit, fades, video-class excluded, metadata with and without the L11 hint, the
// dejudder_motion_rolloff deferral), the inferred value always computed, and the shot
// observer end to end (vectors when valid, the CPU measurement otherwise).
#include "pch.h"
#include "CppUnitTest.h"
#include <IntentDescriptor.h>
#include <IntentInterpreterObserver.h>
#include <MotionIntent.h>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
    namespace
    {
        using namespace MotionIntent;
        using SourceClass = InterpolationPolicy::SourceClass;

        ShotStats Shot(double meanHps, double spreadHps, double edgeTerm, int pairs = 24)
        {
            ShotStats s;
            s.valid = pairs >= kMinDecisionPairs;
            s.measuredPairs = pairs;
            s.seconds = pairs / 24.0;
            s.meanHps = meanHps;
            s.spreadHps = spreadHps;
            s.relativeSpread = spreadHps / (std::max)(meanHps, kSpreadFloorHps);
            s.edgeTerm = edgeTerm;
            s.visibility = Visibility(meanHps, edgeTerm);
            return s;
        }

        PolicyInput Film(MotionIntentPolicy policy, double minScale = 0.5)
        {
            PolicyInput in;
            in.policy = policy;
            in.minScale = minScale;
            in.rateKnown = true;
            in.filmLike = true;
            return in;
        }

        void Near(double expected, double actual, const wchar_t* what, double tolerance = 0.01)
        {
            if (std::fabs(expected - actual) > tolerance)
                Assert::Fail((std::wstring(what) + L": expected " + std::to_wstring(expected) + L", got " +
                    std::to_wstring(actual)).c_str());
        }

        // A textured picture over a wide random canvas (projections can track it).
        struct Canvas
        {
            std::vector<uint16_t> columns, rows;
            Canvas(int width, uint32_t seed, int low, int high, int rowAmplitude = 24)
            {
                uint32_t state = seed;
                auto next = [&state]() { state = state * 1664525u + 1013904223u; return state >> 8; };
                for (int x = 0; x < width; ++x)
                    columns.push_back(static_cast<uint16_t>(low + static_cast<int>(next() % static_cast<uint32_t>(high - low))));
                for (int y = 0; y < 108; ++y)
                    rows.push_back(static_cast<uint16_t>(rowAmplitude > 0 ? next() % static_cast<uint32_t>(rowAmplitude) : 0));
            }
            ContentIntent::LumaGrid Window(int offset, double gain = 1.0) const
            {
                ContentIntent::LumaGrid g;
                g.width = 192;
                g.height = 108;
                g.pictureAspect = 16.0 / 9.0;
                g.luma.resize(192u * 108u);
                for (int y = 0; y < 108; ++y)
                    for (int x = 0; x < 192; ++x)
                    {
                        const double v = columns[static_cast<size_t>(x + offset)] + rows[static_cast<size_t>(y)];
                        g.luma[static_cast<size_t>(y) * 192 + x] = static_cast<uint16_t>(64.0 + (v - 64.0) * gain);
                    }
                return g;
            }
        };

        IntentInterpreter::FrameContext FilmContext(double hz = 24.0)
        {
            IntentInterpreter::FrameContext c;
            c.contentHz = hz;
            c.displayHz = 0.0;
            c.sourceClass = SourceClass::Legacy;
            return c;
        }

        struct Rig
        {
            IntentInterpreter::Observer observer;
            uint64_t sequence = 0;
            Rig(MotionIntentPolicy policy, double minScale = 0.5, double minShot = 0.5, bool rolloff = false)
            {
                IntentInterpreter::Switches sw;
                sw.motion = true;
                IntentInterpreter::MotionSettings m;
                m.policy = policy;
                m.minScale = minScale;
                m.minShotSeconds = minShot;
                m.rolloffOn = rolloff;
                observer.Configure(sw, m);
            }
            // Returns the first sequence of the frames fed.
            uint64_t Feed(const Canvas& canvas, int frames, int cellsPerFrame,
                const IntentInterpreter::FrameContext& context = FilmContext(), int offset = 100)
            {
                const uint64_t first = sequence + 1;
                for (int i = 0; i < frames; ++i)
                    observer.ObserveGrid(++sequence, canvas.Window(offset + i * cellsPerFrame), context);
                return first;
            }
        };
    }

    TEST_CLASS(MotionIntentTests)
    {
    public:
        TEST_METHOD(BandEdgeAndVisibilityShape)
        {
            Assert::AreEqual(0.0, Band(0.0));
            Assert::AreEqual(1.0, Band(0.2));
            Assert::AreEqual(0.0, Band(1.5));
            Assert::IsTrue(Band(0.05) > 0.0 && Band(0.05) < 1.0, L"rising");
            Assert::IsTrue(Band(0.8) > 0.0 && Band(0.8) < 1.0, L"falling into blur");
            Assert::AreEqual(0.0, EdgeTerm(0.5));
            Assert::AreEqual(1.0, EdgeTerm(20.0));
            Assert::AreEqual(0.0, Visibility(0.2, 0.0));
            Assert::AreEqual(1.0, Visibility(0.2, 1.0));
        }

        TEST_METHOD(EveryPolicyOnSyntheticShots)
        {
            const ShotStats still = Shot(0.0, 0.0, 1.0);
            const ShotStats slowSteady = Shot(0.04, 0.005, 1.0);
            const ShotStats moderate = Shot(0.25, 0.03, 1.0);
            const ShotStats fastBlur = Shot(1.5, 0.1, 1.0);
            const ShotStats lowEdge = Shot(0.25, 0.03, 0.05);
            // display: analysis only.
            for (const ShotStats& s : { still, slowSteady, moderate, fastBlur, lowEdge })
            {
                const Decision d = Evaluate(s, Film(MotionIntentPolicy::Display));
                Assert::AreEqual(1.0, d.candidate, L"display keeps 1");
                Assert::IsTrue(d.source == IntentSource::Off);
                Near(VisibilityScale(s, 0.5), d.inferred, L"display shows the visibility estimate");
            }
            // visibility: eases static, blur-dominated and low-edge shots; full on a visible pan.
            Near(0.5, Evaluate(still, Film(MotionIntentPolicy::Visibility)).candidate, L"visibility still");
            Near(1.0, Evaluate(moderate, Film(MotionIntentPolicy::Visibility)).candidate, L"visibility moderate");
            Near(0.5, Evaluate(fastBlur, Film(MotionIntentPolicy::Visibility)).candidate, L"visibility blur");
            Near(0.5, Evaluate(lowEdge, Film(MotionIntentPolicy::Visibility)).candidate, L"visibility low edge");
            const double slowVisibility = Evaluate(slowSteady, Film(MotionIntentPolicy::Visibility)).candidate;
            Assert::IsTrue(slowVisibility > 0.5 && slowVisibility < 1.0, L"visibility slow is in between");
            Assert::IsTrue(Evaluate(moderate, Film(MotionIntentPolicy::Visibility)).source == IntentSource::Inferred);
            // ease_fast: full strength when slow, the minimum on a fast pan.
            Near(1.0, Evaluate(still, Film(MotionIntentPolicy::EaseFast)).candidate, L"ease_fast still");
            Near(1.0, Evaluate(slowSteady, Film(MotionIntentPolicy::EaseFast)).candidate, L"ease_fast slow");
            const double easeModerate = Evaluate(moderate, Film(MotionIntentPolicy::EaseFast)).candidate;
            Assert::IsTrue(easeModerate < 1.0 && easeModerate > 0.8, L"ease_fast moderate eases a little");
            Near(0.5, Evaluate(fastBlur, Film(MotionIntentPolicy::EaseFast)).candidate, L"ease_fast fast");
            // ease_slow: full strength on fast pans, eased on slow steady shots.
            Near(0.5, Evaluate(still, Film(MotionIntentPolicy::EaseSlow)).candidate, L"ease_slow still");
            Near(0.5, Evaluate(slowSteady, Film(MotionIntentPolicy::EaseSlow)).candidate, L"ease_slow slow steady");
            Near(1.0, Evaluate(moderate, Film(MotionIntentPolicy::EaseSlow)).candidate, L"ease_slow moderate");
            Near(1.0, Evaluate(fastBlur, Film(MotionIntentPolicy::EaseSlow)).candidate, L"ease_slow fast");
            // A slow but unsteady shot is not eased by ease_slow.
            Near(1.0, Evaluate(Shot(0.04, 0.05, 1.0), Film(MotionIntentPolicy::EaseSlow)).candidate, L"ease_slow unsteady");
        }

        TEST_METHOD(MetadataPolicyWithAndWithoutTheL11Hint)
        {
            const ShotStats s = Shot(0.25, 0.03, 1.0);
            PolicyInput in = Film(MotionIntentPolicy::Metadata);
            Decision d = Evaluate(s, in);
            Assert::AreEqual(1.0, d.candidate, L"no hint: unchanged");
            Assert::IsFalse(d.metadataPresent);
            Assert::AreEqual(std::string("no metadata hint"), std::string(d.reason));
            in.l11Present = true;
            in.l11FrameRateConversion = 0;
            Assert::AreEqual(1.0, Evaluate(s, in).candidate, L"L11 without a frame-rate-conversion hint");
            in.l11FrameRateConversion = 1;
            d = Evaluate(s, in);
            Near(0.5, d.candidate, L"conversion off: the minimum");
            Assert::IsTrue(d.source == IntentSource::DvL11);
            in.l11FrameRateConversion = 2;
            Near(0.75, Evaluate(s, in).candidate, L"low: halfway");
            in.l11FrameRateConversion = 3;
            Near(1.0, Evaluate(s, in).candidate, L"high: full");
            // Real DV2 wins over L11 and is clamped to the owner minimum.
            in.dv2Present = true;
            in.dv2Scale = 0.3;
            d = Evaluate(s, in);
            Near(0.5, d.candidate, L"dv2 clamped to the minimum");
            Assert::IsTrue(d.source == IntentSource::Dv2);
            Near(1.0, d.inferred, L"inferred (visibility) still computed", 0.02);
        }

        TEST_METHOD(NeverAboveOneAndTheOwnerMinimum)
        {
            const double speeds[] = { 0.0, 0.03, 0.1, 0.25, 0.5, 0.9, 2.0, 10.0 };
            const double spreads[] = { 0.0, 0.01, 0.2 };
            const double edges[] = { 0.0, 0.3, 1.0 };
            const double minimums[] = { 0.0, 0.3, 0.5, 1.0 };
            for (const MotionIntentPolicy policy : { MotionIntentPolicy::Display, MotionIntentPolicy::EaseFast,
                MotionIntentPolicy::EaseSlow, MotionIntentPolicy::Visibility, MotionIntentPolicy::Metadata })
                for (const double v : speeds)
                    for (const double spread : spreads)
                        for (const double edge : edges)
                            for (const double minimum : minimums)
                            {
                                PolicyInput in = Film(policy, minimum);
                                in.l11Present = true;
                                in.l11FrameRateConversion = 1;
                                const Decision d = Evaluate(Shot(v, spread, edge), in);
                                Assert::IsTrue(d.candidate <= 1.0 && d.candidate >= minimum - 1e-12, L"in [min, 1]");
                                Assert::IsTrue(d.inferred <= 1.0 && d.inferred >= minimum - 1e-12, L"inferred in [min, 1]");
                                if (minimum >= 1.0)
                                    Assert::AreEqual(1.0, d.candidate, L"min 1 = no change");
                            }
            Assert::AreEqual(1.0, ClampScale(1.7, 0.5));
            Assert::AreEqual(0.5, ClampScale(0.1, 0.5));
            Assert::AreEqual(1.0, ClampScale(std::nan(""), 0.5));
        }

        TEST_METHOD(VideoClassSourcesAndUnknownRatesGetOne)
        {
            for (const MotionIntentPolicy policy : { MotionIntentPolicy::EaseFast, MotionIntentPolicy::EaseSlow,
                MotionIntentPolicy::Visibility, MotionIntentPolicy::Metadata })
            {
                PolicyInput in = Film(policy);
                in.l11Present = true;
                in.l11FrameRateConversion = 1;
                in.filmLike = false;
                Decision d = Evaluate(Shot(1.5, 0.0, 1.0), in);
                Assert::AreEqual(1.0, d.candidate, L"video-class");
                Assert::AreEqual(std::string("video-class source"), std::string(d.reason));
                in.rateKnown = false;
                Assert::AreEqual(1.0, Evaluate(Shot(1.5, 0.0, 1.0), in).candidate, L"rate unknown");
            }
        }

        TEST_METHOD(EaseFastDefersToTheRolloff)
        {
            PolicyInput in = Film(MotionIntentPolicy::EaseFast);
            in.rolloffOn = true;
            const Decision d = Evaluate(Shot(1.5, 0.1, 1.0), in);
            Assert::AreEqual(1.0, d.candidate, L"never reduced twice");
            Assert::AreEqual(std::string("deferred to dejudder_motion_rolloff"), std::string(d.reason));
            Near(0.5, d.inferred, L"inferred still computed");
            in.policy = MotionIntentPolicy::EaseSlow;
            Near(0.5, Evaluate(Shot(0.0, 0.0, 1.0), in).candidate, L"other policies are not deferred");
        }

        TEST_METHOD(InferredValueAlwaysComputedAndLogged)
        {
            const ShotStats s = Shot(0.25, 0.03, 1.0);
            const Decision d = Evaluate(s, Film(MotionIntentPolicy::Display));
            Assert::IsTrue(d.easeFast < 1.0, L"ease_fast computed under display");
            Near(1.0, d.easeSlow, L"ease_slow computed");
            Near(1.0, d.visibility, L"visibility computed");
            Applier applier;
            CutInput cut;
            cut.decisionReady = true;
            const std::string line = LogLine(MotionIntentPolicy::Display, d, s, applier.OnCut(d, cut));
            Assert::IsTrue(line.find("motion scale display metadata=- inferred=1.00") == 0, std::wstring(line.begin(), line.end()).c_str());
            Assert::IsTrue(line.find("ease_fast=") != std::string::npos);
        }

        TEST_METHOD(ApplierRulesAtCuts)
        {
            Applier applier;
            Decision d;
            d.candidate = 0.95;
            d.source = IntentSource::Inferred;
            CutInput in;
            in.decisionReady = true;
            in.minScale = 0.5;
            CutResult r = applier.OnCut(d, in);
            Assert::IsFalse(r.changed, L"dead band");
            Assert::AreEqual(1.0, applier.Applied());
            d.candidate = 0.5;
            in.decisionReady = false;
            r = applier.OnCut(d, in);
            Assert::IsFalse(r.changed, L"missed cut waits");
            Assert::AreEqual(std::string("missed cut: waits for the next cut"), std::string(r.reason));
            in.decisionReady = true;
            in.fadeBoundary = true;
            Assert::IsFalse(applier.OnCut(d, in).changed, L"fade keeps the value");
            in.fadeBoundary = false;
            in.shotEndKnown = true;
            in.shotSeconds = 1.0;
            in.minShotSeconds = 2.0;
            Assert::IsFalse(applier.OnCut(d, in).changed, L"a short shot inherits");
            in.shotSeconds = 3.0;
            r = applier.OnCut(d, in);
            Assert::IsTrue(r.changed, L"a long shot is applied");
            Assert::AreEqual(0.5, applier.Applied());
            Assert::IsTrue(applier.Source() == IntentSource::Inferred);
            // The owner raises the minimum: clamped at the next cut.
            in.minScale = 0.8;
            d.candidate = 0.55;
            applier.OnCut(d, in);
            Assert::AreEqual(0.8, applier.Applied());
        }

        // ---- the observer end to end ----
        TEST_METHOD(ObserverAppliesOnlyAtCuts)
        {
            Rig rig(MotionIntentPolicy::Visibility);
            const Canvas a(1200, 7, 100, 400), b(1200, 99, 500, 900), c(1200, 3, 120, 420);
            const uint64_t shotA = rig.Feed(a, 24, 0);    // static
            const uint64_t shotB = rig.Feed(b, 24, 2);    // 2 cells / frame at 24 Hz: about 0.44 picture heights / s
            const uint64_t shotC = rig.Feed(c, 24, 0);    // static
            Assert::AreEqual(size_t(3), rig.observer.ShotsKnown(), L"three shots");
            Near(0.5, rig.observer.AppliedMotionScale(shotA), L"static shot eased at its cut");
            for (uint64_t s = shotA + 1; s < shotB; ++s)
                Near(0.5, rig.observer.AppliedMotionScale(s), L"no change mid-shot");
            Near(1.0, rig.observer.AppliedMotionScale(shotB), L"the visible pan at its cut");
            const MotionIntentStatus status = rig.observer.MotionStatus();
            Assert::IsTrue(status.enabled && status.shotSequence == shotB);
            for (uint64_t s = shotB + 1; s < shotC; ++s)
                Near(1.0, rig.observer.AppliedMotionScale(s), L"no change mid-shot");
            Near(0.5, rig.observer.AppliedMotionScale(shotC), L"static again at the next cut");
            Assert::AreEqual(uint64_t(3), rig.observer.DisplayedCuts());
            const IntentDescriptor descriptor = rig.observer.Descriptor();
            Assert::IsTrue(descriptor.shotSequence == shotC && descriptor.motionScale.valid &&
                descriptor.motionScale.source == IntentSource::Inferred, L"descriptor motion field");
        }

        TEST_METHOD(ObserverMissedCutWaitsForTheNextCut)
        {
            Rig rig(MotionIntentPolicy::Visibility);
            const Canvas a(1200, 7, 100, 400), b(1200, 99, 500, 900), c(1200, 3, 120, 420);
            const uint64_t shotA = rig.Feed(a, 24, 0);
            Near(0.5, rig.observer.AppliedMotionScale(shotA), L"A");
            const uint64_t shotB = rig.Feed(b, 1, 2);     // only the cut frame is in the look-ahead
            Near(0.5, rig.observer.AppliedMotionScale(shotB), L"missed: A's value stays");
            rig.Feed(b, 23, 2, FilmContext(), 102);
            for (uint64_t s = shotB + 1; s < shotB + 24; ++s)
                Near(0.5, rig.observer.AppliedMotionScale(s), L"never mid-shot");
            const uint64_t shotC = rig.Feed(c, 24, 3);
            Near(1.0, rig.observer.AppliedMotionScale(shotC), L"the next cut decides");
        }

        TEST_METHOD(ObserverDeadBandAndShortShotInherit)
        {
            Rig rig(MotionIntentPolicy::Visibility, 0.5, 2.0);
            const Canvas a(1200, 7, 100, 400), b(1200, 99, 500, 900), c(1200, 3, 120, 420);
            const uint64_t shotA = rig.Feed(a, 60, 0);    // 2.5 s static
            const uint64_t shotB = rig.Feed(b, 12, 2);    // 0.5 s pan: shorter than the minimum shot
            const uint64_t shotC = rig.Feed(c, 60, 0);    // static again
            Near(0.5, rig.observer.AppliedMotionScale(shotA), L"A");
            Near(0.5, rig.observer.AppliedMotionScale(shotB), L"the short shot inherits");
            Assert::AreEqual(std::string("short shot inherits"), std::string(rig.observer.MotionStatus().reason));
            Near(0.5, rig.observer.AppliedMotionScale(shotC), L"C");
            Assert::AreEqual(std::string("within the dead band"), std::string(rig.observer.MotionStatus().reason));
        }

        TEST_METHOD(ObserverFadeKeepsThePreviousValue)
        {
            Rig rig(MotionIntentPolicy::Visibility);
            const Canvas a(1200, 7, 100, 400), b(1200, 99, 500, 900);
            const uint64_t shotA = rig.Feed(a, 24, 0);
            Near(0.5, rig.observer.AppliedMotionScale(shotA), L"A");
            // Fade to black: the same picture darkening over 16 frames (no cut).
            for (int i = 1; i <= 16; ++i)
                rig.observer.ObserveGrid(++rig.sequence, a.Window(100, 1.0 - i / 17.0), FilmContext());
            const uint64_t shotB = rig.Feed(b, 24, 2);
            Assert::AreEqual(size_t(2), rig.observer.ShotsKnown(), L"the fade is not a cut");
            Near(0.5, rig.observer.AppliedMotionScale(shotB), L"after a fade: the previous value");
            Assert::AreEqual(std::string("fade: keeps the previous value"), std::string(rig.observer.MotionStatus().reason));
        }

        TEST_METHOD(ObserverVideoClassAndMetadata)
        {
            {
                Rig rig(MotionIntentPolicy::Visibility);
                const Canvas a(1200, 7, 100, 400);
                const uint64_t shot = rig.Feed(a, 24, 0, FilmContext(60.0));   // 60p legacy: video-class
                Near(1.0, rig.observer.AppliedMotionScale(shot), L"video-class excluded");
                Near(1.0, rig.observer.MotionStatus().candidateScale, L"video-class candidate");
            }
            {
                Rig rig(MotionIntentPolicy::Metadata);
                const Canvas a(1200, 7, 100, 400), b(1200, 99, 500, 900);
                const uint64_t plain = rig.Feed(a, 24, 0);
                Near(1.0, rig.observer.AppliedMotionScale(plain), L"no hint: 1");
                IntentInterpreter::FrameContext hinted = FilmContext();
                hinted.l11Present = true;
                hinted.l11FrameRateConversion = 1;
                const uint64_t shot = rig.Feed(b, 24, 2, hinted);
                Near(0.5, rig.observer.AppliedMotionScale(shot), L"L11 conversion off: the minimum");
                Assert::IsTrue(rig.observer.MotionStatus().source == IntentSource::DvL11);
            }
        }

        TEST_METHOD(ObserverRolloffDeferral)
        {
            Rig rig(MotionIntentPolicy::EaseFast, 0.5, 0.5, true);
            const Canvas a(1200, 7, 100, 400);
            const uint64_t shot = rig.Feed(a, 24, 6);   // fast pan
            Near(1.0, rig.observer.AppliedMotionScale(shot), L"deferred: 1");
            Assert::IsTrue(rig.observer.MotionStatus().inferredScale < 1.0f, L"inferred still computed");
        }

        TEST_METHOD(ObserverUsesValidVectorsElseTheCpuMeasurement)
        {
            Rig rig(MotionIntentPolicy::Visibility);
            const Canvas a(1200, 7, 100, 400), b(1200, 99, 500, 900);
            const uint64_t shotA = rig.Feed(a, 24, 0);   // static by the CPU
            Near(0.5, rig.observer.AppliedMotionScale(shotA), L"A static");
            const uint64_t shotB = rig.Feed(b, 24, 0);   // static by the CPU too ...
            // ... but the de-judder's valid vectors say 0.02 picture heights per frame (0.48 / s).
            int used = 0;
            for (uint64_t s = shotB + 1; s < shotB + 24; ++s)
            {
                IntentInterpreter::VectorSample v;
                v.from = s - 1;
                v.to = s;
                v.heightsPerFrame = 0.02;
                v.valid = true;
                used += rig.observer.NoteVectors(v) ? 1 : 0;
            }
            Assert::AreEqual(23, used, L"every valid pair taken");
            const MotionIntentStatus before = rig.observer.MotionStatus();
            Assert::IsTrue(before.vectorsUsed, L"vectors used");
            Near(0.48, before.panPictureHeightsPerSecond, L"the vector speed", 0.02);
            Near(1.0, rig.observer.AppliedMotionScale(shotB), L"decided from the vectors");
            // Invalid pairs fall back: a resync / unclear motion (valid false), a repeat
            // (from == to), a drop (not the measured pair), a pair across a cut.
            IntentInterpreter::VectorSample v;
            v.from = shotB + 3; v.to = shotB + 4; v.heightsPerFrame = 0.5; v.valid = false;
            Assert::IsFalse(rig.observer.NoteVectors(v), L"invalid (resync)");
            v.valid = true; v.from = shotB + 4;
            Assert::IsFalse(rig.observer.NoteVectors(v), L"repeat");
            v.from = shotB + 1; v.to = shotB + 5;
            Assert::IsFalse(rig.observer.NoteVectors(v), L"drop: not the measured pair");
            v.from = shotB - 1; v.to = shotB;
            Assert::IsFalse(rig.observer.NoteVectors(v), L"across the cut");
            // A discontinuity breaks the pair: its vectors are not taken.
            IntentInterpreter::FrameContext gap = FilmContext();
            gap.discontinuity = true;
            rig.observer.ObserveGrid(++rig.sequence, b.Window(100), gap);
            v.from = rig.sequence - 1; v.to = rig.sequence;
            Assert::IsFalse(rig.observer.NoteVectors(v), L"after a discontinuity: no pair");
        }
    };
}
