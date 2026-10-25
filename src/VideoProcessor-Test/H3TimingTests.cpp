// Bug hunt round 3, hunter A (timing): F1 engine global vector scale, F2 locked-film
// queue floor with the automatic look-ahead, F3 arrival-rate estimator across a
// pulldown lock change, F4 the idle motion engine kept for hiccup repair (and its
// low-memory fallback), F5 a standalone gap pair keeps the de-judder strength, F6 a
// skipped frame is a discontinuity for the intent observer, F8 the explicit-0
// look-ahead notice, F9 the fade detector in time, F10 texture_restore's one-period
// field gate. CPU only.
#include "pch.h"
#include "CppUnitTest.h"
#include <IntentInterpreterObserver.h>
#include <MotionIntent.h>
#include <QueueLookahead.h>
#include <vprenderer/AlphaCadenceCorrectionPolicy.h>
#include <vprenderer/DejudderPolicy.h>
#include <vprenderer/InterpolationTimeline.h>
#include <vprenderer/MotionEngineIdle.h>
#include <vprenderer/PulldownDetector.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
    namespace
    {
        std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

        ContentIntent::LumaGrid StillGrid(uint32_t seed)
        {
            ContentIntent::LumaGrid g;
            g.width = 192;
            g.height = 108;
            g.luma.resize(192u * 108u);
            uint32_t state = seed;
            for (uint16_t& v : g.luma)
            {
                state = state * 1664525u + 1013904223u;
                v = static_cast<uint16_t>(100 + static_cast<int>((state >> 8) % 300u));
            }
            return g;
        }

        // A textured canvas panning right (ContentIntentTests' pattern).
        struct PanCanvas
        {
            std::vector<uint16_t> columns, rows;
            PanCanvas(int width, int height, uint32_t seed)
            {
                uint32_t state = seed;
                auto next = [&state]() { state = state * 1664525u + 1013904223u; return state >> 8; };
                for (int x = 0; x < width; ++x) columns.push_back(static_cast<uint16_t>(100 + next() % 700u));
                for (int y = 0; y < height; ++y) rows.push_back(static_cast<uint16_t>(next() % 24u));
            }
            ContentIntent::LumaGrid Window(int offset) const
            {
                ContentIntent::LumaGrid g;
                g.width = 192;
                g.height = 108;
                g.pictureAspect = 16.0 / 9.0;
                g.luma.resize(192u * 108u);
                for (int y = 0; y < 108; ++y)
                    for (int x = 0; x < 192; ++x)
                        g.luma[static_cast<size_t>(y) * 192 + x] =
                            static_cast<uint16_t>(columns[static_cast<size_t>(x + offset)] + rows[static_cast<size_t>(y)]);
                return g;
            }
        };

        IntentInterpreter::FrameContext Context(double hz)
        {
            IntentInterpreter::FrameContext c;
            c.contentHz = hz;
            c.transfer = BlackIntent::Transfer::Gamma;
            return c;
        }

        void ConfigureMotion(IntentInterpreter::Observer& o)
        {
            IntentInterpreter::Switches sw;
            sw.motion = true;
            o.Configure(sw, IntentInterpreter::MotionSettings());
        }
    }

    TEST_CLASS(H3TimingTests)
    {
    public:
        // ---- F1: the engine global vector at 1080, 2160 and 4320 ----
        TEST_METHOD(F1EngineGlobalVectorScalesWithTheSearchHalvings)
        {
            const double panHeightsPerSecond = 0.6, fps = 24.0;
            const int heights[3] = { 1080, 2160, 4320 };
            const int halvingsExpected[3] = { 0, 1, 2 };
            for (int i = 0; i < 3; ++i)
            {
                const int h = heights[i];
                const int halvings = DejudderPolicy::SearchHalvingsFor(true, h);
                Assert::AreEqual(halvingsExpected[i], halvings, L"MotionWarp halves at 2160 once and at 4320 twice");
                Assert::AreEqual(8.0 * (1 << halvings), DejudderPolicy::EngineCellSourcePixels(halvings));
                // The engine reports the pan in its own cells (8 px of its full-size equivalent per halving).
                const double sourcePxPerFrame = panHeightsPerSecond * h / fps;
                const double engineCells = sourcePxPerFrame / (8.0 * (1 << halvings));
                const double gx = engineCells * 0.8, gy = engineCells * 0.6;   // |g| = engineCells
                // The latch's fast-pan switch: native px per frame, heights per second.
                const double latchPx = DejudderPolicy::EngineGlobalSourcePixels(gx, gy, halvings);
                const double latchHps = latchPx * fps / h;
                Assert::AreEqual(panHeightsPerSecond, latchHps, 1e-9, W("latch heights/s at " + std::to_string(h)).c_str());
                Assert::IsTrue(latchHps >= SourceResolutionMonitor::Monitor::kPanHeightsPerSecond,
                    W("a 0.6 h/s pan is a fast pan at " + std::to_string(h)).c_str());
                // Before the fix (8 px at every size): 0.6 / 2^halvings, under the 0.5 switch at 4K.
                const double oldHps = 8.0 * std::hypot(gx, gy) * fps / h;
                Assert::AreEqual(panHeightsPerSecond / (1 << halvings), oldHps, 1e-9);
                // Motion intent: the same vectors, picture heights per frame, through the observer.
                IntentInterpreter::Observer o;
                ConfigureMotion(o);
                const ContentIntent::LumaGrid still = StillGrid(7);
                for (uint64_t s = 1; s <= 6; ++s)
                    o.ObserveGrid(s, still, Context(fps));
                for (uint64_t s = 2; s <= 6; ++s)
                {
                    IntentInterpreter::VectorSample v;
                    v.from = s - 1;
                    v.to = s;
                    v.valid = true;
                    v.heightsPerFrame = latchPx / h;
                    Assert::IsTrue(o.NoteVectors(v), L"the pair's vectors are taken");
                }
                const MotionIntentStatus status = o.MotionStatus();
                Assert::IsTrue(status.vectorsUsed);
                Assert::AreEqual(panHeightsPerSecond, static_cast<double>(status.panPictureHeightsPerSecond), 1e-4,
                    W("intent pan heights/s at " + std::to_string(h)).c_str());
            }
        }

        // ---- F2: the locked-film queue target with the automatic look-ahead ----
        TEST_METHOD(F2AutomaticLookaheadKeepsTheLockedFilmScaling)
        {
            const double threeTwo = Pulldown::FilmToSignalRatio(5, 2);
            auto locked = [threeTwo](size_t target, size_t lookahead, bool automatic)
            {
                size_t minimum = Pulldown::kLockedQueuePairFrames;   // gate r3 (round 5): A, a converted B and the arrival
                minimum = (std::max)(minimum, QueueLookahead::LockedFilmLookaheadFloor(lookahead, automatic));
                return Pulldown::LockedQueueTarget(target, threeTwo, minimum);
            };
            const size_t auto5 = QueueLookahead::ResolveActivePictureLookahead(false, 0, false, 5, 32);
            const size_t auto8 = QueueLookahead::ResolveActivePictureLookahead(false, 0, false, 8, 32);
            Assert::AreEqual(size_t(4), auto5);
            Assert::AreEqual(size_t(7), auto8);
            // Automatic: the film scaling applies (round(target x 2/5)), floored at the
            // locked pair's 3 frames (gate r3, merged in integration round 5).
            Assert::AreEqual(size_t(3), locked(5, auto5, true), L"target 5, 3:2, automatic look-ahead");
            Assert::AreEqual(size_t(3), locked(8, auto8, true), L"target 8, 3:2, automatic look-ahead");
            // The same values treated as explicit (the behaviour before): no scaling at all.
            Assert::AreEqual(size_t(5), locked(5, auto5, false), L"explicit 4 holds target 5");
            Assert::AreEqual(size_t(8), locked(8, auto8, false), L"explicit 7 holds target 8");
            // An explicit look-ahead still needs its frames; an explicit 0 needs none.
            Assert::AreEqual(size_t(4), locked(8, 3, false), L"explicit 3: floor 4");
            Assert::AreEqual(size_t(3), locked(8, 0, false), L"explicit 0: scaled");
            Assert::AreEqual(size_t(0), QueueLookahead::LockedFilmLookaheadFloor(4, true));
            Assert::AreEqual(size_t(5), QueueLookahead::LockedFilmLookaheadFloor(4, false));
        }

        // ---- F3: the arrival-rate estimator across a pulldown lock change ----
        TEST_METHOD(F3ArrivalRateRecoversWithinSecondsOfALockChange)
        {
            // The replica (D:/codex/hunt3-scratch/a/arrival_estimator_lockchange.py): 60 s unlocked
            // at 59.94 (hint 59.94), 60 s locked (film-clock arrivals, hint 23.976), unlock.
            const double signal = 60000.0 / 1001.0, film = 24000.0 / 1001.0;
            AlphaArrivalRateEstimator e;
            double t = 1000.0;
            for (int n = 0; n < static_cast<int>(60 * signal); ++n) { t += 1.0 / signal; e.Add(t, signal); }
            Assert::AreEqual(0.0, (e.RateHz() / signal - 1.0) * 1e6, 50.0, L"unlocked");
            double lockedAt12 = 0.0;
            for (int n = 0; n < static_cast<int>(60 * film); ++n)
            {
                t += 1.0 / film;
                e.Add(t, film);
                if (n == static_cast<int>(12 * film)) lockedAt12 = e.RateHz();
            }
            Assert::AreEqual(0.0, (lockedAt12 / film - 1.0) * 1e6, 200.0, L"locked: the film rate within 12 s");
            double at12 = 0.0, at30 = 0.0;
            for (int sec = 1; sec <= 30; ++sec)
            {
                for (int n = 0; n < static_cast<int>(signal); ++n) { t += 1.0 / signal; e.Add(t, signal); }
                if (sec == 12) at12 = e.RateHz();
                if (sec == 30) at30 = e.RateHz();
            }
            const double ppm12 = (at12 / signal - 1.0) * 1e6, ppm30 = (at30 / signal - 1.0) * 1e6;
            Assert::IsTrue(at12 > 0.0 && std::fabs(ppm12) < 200.0, W("unlock +12 s: " + std::to_string(ppm12) + " ppm").c_str());
            Assert::IsTrue(at30 > 0.0 && std::fabs(ppm30) < 200.0, W("unlock +30 s: " + std::to_string(ppm30) + " ppm").c_str());
            // A 1000/1001 step of the hint (0.1%) keeps the fit: no restart, the rate stays known.
            AlphaArrivalRateEstimator k;
            t = 50.0;
            for (int n = 0; n < static_cast<int>(20 * signal); ++n) { t += 1.0 / signal; k.Add(t, signal); }
            t += 1.0 / signal;
            k.Add(t, 60.0);
            Assert::IsTrue(k.RateHz() > 0.0, L"a 0.1% hint change is not a restart");
        }

        // ---- F4: the idle motion engine kept for hiccup repair ----
        TEST_METHOD(F4SecondGapDoesNotRecreateTheEngine)
        {
            // The renderer's rule per frame: a gap pair creates the engine when absent; an idle
            // frame releases it unless Keep.
            auto run = [](bool repairPossible, MotionEngineIdle::Keeper& keeper, int& creates, int& releases)
            {
                bool held = false;
                creates = releases = 0;
                for (int frame = 0; frame < 400; ++frame)
                {
                    const bool gap = frame == 100 || frame == 250;
                    if (gap)
                    {
                        if (!held) { held = true; ++creates; }
                        continue;
                    }
                    if (held && !keeper.Keep(true, repairPossible)) { held = false; ++releases; }
                }
            };
            MotionEngineIdle::Keeper keeper;
            int creates = 0, releases = 0;
            run(true, keeper, creates, releases);   // off, a video-rate cadence, mask only on video
            Assert::AreEqual(1, creates, L"created at the first gap only");
            Assert::AreEqual(0, releases, L"kept idle between the gaps");
            run(false, keeper, creates, releases);   // low latency, interlaced, smooth motion, native RGB
            Assert::AreEqual(2, creates, L"no repair there: released as before");
            Assert::IsFalse(keeper.Keep(false, true), L"hiccup repair off: released");
        }

        TEST_METHOD(F4LowVideoMemoryReleasesAndRecreatesLater)
        {
            const uint64_t mb = 1024ull * 1024ull, budget = 4096 * mb;
            const uint64_t engine = MotionEngineIdle::IdleBytes(3840, 2160);
            MotionEngineIdle::Keeper k;
            k.NoteBudgetReported(true);
            k.NoteBudget(2000 * mb, budget, 0, 0.0);
            Assert::IsTrue(k.Keep(true, true), L"room: kept");
            k.NoteBudget(3950 * mb, budget, 0, 1.0);   // 96%: pressure
            Assert::IsFalse(k.Keep(true, true), L"budget pressure: released");
            Assert::AreEqual(uint64_t(1), k.Pressures());
            k.NoteBudget(1000 * mb, budget, engine, 10.0);
            Assert::IsFalse(k.Keep(true, true), L"held off for the hold time");
            k.NoteBudget(3400 * mb, budget, engine, 40.0);   // 83% + the engine > 85%
            Assert::IsFalse(k.Keep(true, true), L"no room for the engine yet");
            k.NoteBudget(2000 * mb, budget, engine, 41.0);
            Assert::IsTrue(k.Keep(true, true), L"recreated at the next gap once the pressure passed");
            // An allocation failure without budget readings: released, kept again after the hold.
            MotionEngineIdle::Keeper f;
            f.NoteAllocationFailure(100.0);
            Assert::IsFalse(f.Keep(true, true));
            f.NoteTime(110.0);
            Assert::IsFalse(f.Keep(true, true));
            f.NoteTime(131.0);
            Assert::IsTrue(f.Keep(true, true));
            Assert::IsTrue(f.BudgetDue(0.0));
            // The estimate behind the report: 1080p about 35 MB, 4K about 141 MB.
            Assert::AreEqual(35.3, MotionEngineIdle::IdleBytes(1920, 1080) / 1e6, 0.1);
            Assert::AreEqual(141.0, MotionEngineIdle::IdleBytes(3840, 2160) / 1e6, 0.1);
        }

        // ---- F5: a standalone gap pair keeps the owner's de-judder strength ----
        TEST_METHOD(F5StandaloneGapKeepsTheActiveDecision)
        {
            const float owner = 0.3f, intent = 0.8f;
            const auto active = InterpolationTimeline::StandaloneGap(true, owner * intent);
            Assert::IsTrue(active.keepActive);
            Assert::AreEqual(owner * intent, active.strength, 1e-6f, L"strength x intent scale kept, not 1.0");
            Assert::AreEqual(owner * intent, active.phaseScale, 1e-6f, L"the gap pair drawn at phase x strength");
            const auto off = InterpolationTimeline::StandaloneGap(false, owner);
            Assert::IsFalse(off.keepActive);
            Assert::AreEqual(1.0f, off.strength, L"no de-judder: the true phase");
            Assert::AreEqual(1.0f, off.phaseScale);
            Assert::AreEqual(1.0f, InterpolationTimeline::StandaloneGap(true, 1.0f).phaseScale);
        }

        // ---- F6: a skipped frame is a discontinuity for the intent observer ----
        TEST_METHOD(F6SkippedFrameIsADiscontinuity)
        {
            const PanCanvas canvas(800, 108, 11);
            const int cellsPerFrame = 2;
            auto meanPan = [&](const std::vector<uint64_t>& sequences)
            {
                IntentInterpreter::Observer o;
                ConfigureMotion(o);
                for (const uint64_t s : sequences)
                    o.ObserveGrid(s, canvas.Window(static_cast<int>(s) * cellsPerFrame), Context(24.0));
                return static_cast<double>(o.MotionStatus().panPictureHeightsPerSecond);
            };
            const double contiguous = meanPan({ 1, 2, 3, 4, 5 });
            const double holed = meanPan({ 1, 2, 4, 5 });
            Assert::IsTrue(contiguous > 0.0, L"the pan is measured");
            Assert::AreEqual(contiguous, holed, contiguous * 0.02,
                W("the pair across the hole is not measured as one frame: " + std::to_string(contiguous) + " vs " +
                    std::to_string(holed)).c_str());
            // Sequences 1, 2, 4: one measured pair (1-2) only.
            IntentInterpreter::Observer o;
                ConfigureMotion(o);
            for (const uint64_t s : { 1ull, 2ull, 4ull })
                o.ObserveGrid(s, canvas.Window(static_cast<int>(s) * cellsPerFrame), Context(24.0));
            Assert::AreEqual(size_t(1), o.ShotsKnown(), L"no cut judged across the hole");
        }

        // ---- F9: the fade detector in time ----
        TEST_METHOD(F9FadeThresholdsInTime)
        {
            Assert::AreEqual(1.0, MotionIntent::FadeStepCodes(24.0), 1e-12, L"24p: 1 code per frame, as before");
            Assert::AreEqual(3, MotionIntent::FadeMinRunFrames(24.0));
            Assert::AreEqual(6, MotionIntent::FadeWindowFrames(24.0));
            Assert::AreEqual(0.5, MotionIntent::FadeStepCodes(48.0), 1e-12);
            Assert::AreEqual(6, MotionIntent::FadeMinRunFrames(48.0));
            Assert::AreEqual(12, MotionIntent::FadeWindowFrames(48.0));
            Assert::AreEqual(1.0, MotionIntent::FadeStepCodes(0.0), 1e-12, L"rate unknown: the frame values before");
            // A 4 s fade of 100 codes: detected at 24p and at 48p HFR, after the same time.
            for (const double hz : { 24.0, 48.0 })
            {
                MotionIntent::FadeRun run;
                const double perFrame = 100.0 / (4.0 * hz);
                int firstFading = -1;
                for (int i = 0; i < static_cast<int>(4.0 * hz); ++i)
                    if (run.Step(true, perFrame, hz) && firstFading < 0) firstFading = i;
                Assert::IsTrue(firstFading >= 0, W("fade found at " + std::to_string(hz)).c_str());
                Assert::AreEqual(MotionIntent::kFadeMinRunSeconds, (firstFading + 1) / hz, 1e-9, L"after the same time");
                // A cut (not measurable) starts the run over.
                Assert::IsFalse(run.Step(false, perFrame, hz));
            }
            // Below the step in time: no fade (0.9 codes per frame at 24p, 0.45 at 48p).
            for (const double hz : { 24.0, 48.0 })
            {
                MotionIntent::FadeRun run;
                bool any = false;
                for (int i = 0; i < 20; ++i) any = run.Step(true, 21.6 / hz, hz) || any;
                Assert::IsFalse(any, W("slow drift is no fade at " + std::to_string(hz)).c_str());
            }
        }

        // ---- F10: texture_restore takes a one-period field only ----
        TEST_METHOD(F10GapPairFieldIsNotOnePeriod)
        {
            using InterpolationTimeline::FieldPeriods;
            using InterpolationTimeline::OnePeriodField;
            Assert::AreEqual(1, FieldPeriods(false, 0), L"adjacent pair");
            Assert::AreEqual(1, FieldPeriods(false, 3), L"a skipped pulldown update is one film period");
            Assert::AreEqual(2, FieldPeriods(true, 1), L"one missed frame: two periods");
            Assert::AreEqual(3, FieldPeriods(true, 2));
            Assert::IsTrue(OnePeriodField(FieldPeriods(false, 0)));
            Assert::IsFalse(OnePeriodField(FieldPeriods(true, 1)), L"a gap-fill field is not current one-period motion");
        }

        // ---- F8: the explicit-0 look-ahead notice ----
        TEST_METHOD(F8ExplicitZeroLookaheadNotice)
        {
            using QueueLookahead::ExplicitZeroLookahead;
            Assert::IsTrue(ExplicitZeroLookahead("0"));
            Assert::IsTrue(ExplicitZeroLookahead(" 0 "));
            Assert::IsTrue(ExplicitZeroLookahead("00"));
            Assert::IsFalse(ExplicitZeroLookahead(""), L"absent: automatic, no notice");
            Assert::IsFalse(ExplicitZeroLookahead("4"));
            Assert::IsFalse(ExplicitZeroLookahead("auto"));
            Assert::IsTrue(std::string(QueueLookahead::kExplicitZeroLookaheadNotice).find("Auto") != std::string::npos);
        }
    };
}
