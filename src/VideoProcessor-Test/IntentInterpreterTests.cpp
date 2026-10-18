// Intent interpreter items 2-5 (2026-10-17): the shot observer (runs when any one
// function is on, idle when all are off, shot boundaries, shadow statistics handed
// through, the descriptor), the ambient target-black identity at strength 0 / off,
// the GUI's lux feed and status text, and the renderer's parsing of every key
// (through the renderer DLL's settings dump, no GPU).
#include "pch.h"
#include "CppUnitTest.h"
#include <AmbientState.h>
#include <BlackIntent.h>
#include <IntentDescriptor.h>
#include <IntentInterpreterObserver.h>
#include <IntentInterpreterSupport.h>
#include <ActivePictureEvidence.h>
#include <AnalysisFrame.h>
#include <QueueLookahead.h>
#include <vprenderer/AlphaQueuePolicy.h>
#include <chrono>
#include <cstdlib>
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <sstream>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
    namespace
    {
        ContentIntent::LumaGrid TexturedGrid(uint32_t seed, int low, int high)
        {
            ContentIntent::LumaGrid g;
            g.width = 192;
            g.height = 108;
            g.luma.resize(192u * 108u);
            uint32_t state = seed;
            for (uint16_t& v : g.luma)
            {
                state = state * 1664525u + 1013904223u;
                v = static_cast<uint16_t>(low + static_cast<int>((state >> 8) % static_cast<uint32_t>(high - low)));
            }
            return g;
        }

        IntentInterpreter::FrameContext Film()
        {
            IntentInterpreter::FrameContext c;
            c.contentHz = 24.0;
            c.transfer = BlackIntent::Transfer::Gamma;
            c.sdrWhiteNits = 100.0;
            c.displayBlackNits = 0.05;
            return c;
        }

        std::string Here()
        {
            char here[MAX_PATH] = {};
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&Here), &self);
            GetModuleFileNameA(self, here, MAX_PATH);
            const std::string binary(here);
            return binary.substr(0, binary.find_last_of("\\/") + 1);
        }

        bool SameBits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
    }

    TEST_CLASS(IntentInterpreterTests)
    {
    public:
        TEST_METHOD(ObserverIdleWhenEveryFunctionIsOff)
        {
            IntentInterpreter::Observer observer;
            observer.Configure(IntentInterpreter::Switches(), IntentInterpreter::MotionSettings());
            Assert::IsFalse(observer.Active());
            for (uint64_t s = 1; s <= 10; ++s)
                observer.ObserveGrid(s, TexturedGrid(1, 100, 400), Film());
            Assert::AreEqual(uint64_t(0), observer.FramesObserved(), L"nothing computed");
            Assert::AreEqual(1.0, observer.AppliedMotionScale(5));
            Assert::IsFalse(observer.MotionStatus().enabled);
            Assert::AreEqual(uint64_t(0), observer.DisplayedCuts());
        }

        TEST_METHOD(ObserverRunsWhenOnlyOneFunctionIsOn)
        {
            for (int which = 0; which < 5; ++which)
            {
                IntentInterpreter::Switches sw;
                sw.content = which == 0;
                sw.motion = which == 1;
                sw.ambient = which == 2;
                sw.black = which == 3;
                sw.display = which == 4;
                IntentInterpreter::Observer observer;
                observer.Configure(sw, IntentInterpreter::MotionSettings());
                Assert::IsTrue(observer.Active());
                for (uint64_t s = 1; s <= 4; ++s)
                    observer.ObserveGrid(s, TexturedGrid(1, 100, 400), Film());
                for (uint64_t s = 5; s <= 8; ++s)
                    observer.ObserveGrid(s, TexturedGrid(2, 500, 900), Film());
                Assert::AreEqual(uint64_t(8), observer.FramesObserved(), L"observed");
                Assert::AreEqual(size_t(2), observer.ShotsKnown(), L"the cut found");
                observer.AdvanceDisplay(1);
                observer.AdvanceDisplay(5);
                Assert::AreEqual(uint64_t(2), observer.DisplayedCuts(), L"both boundaries crossed");
                Assert::AreEqual(uint64_t(5), observer.DisplayedShotStart());
                // Motion intent off: the de-judder strength is never touched.
                if (!sw.motion)
                    Assert::AreEqual(1.0, observer.AppliedMotionScale(8));
            }
        }

        TEST_METHOD(ObserverShotBoundariesFormatChangeAndDiscontinuity)
        {
            IntentInterpreter::Switches sw;
            sw.ambient = true;
            IntentInterpreter::Observer observer;
            observer.Configure(sw, IntentInterpreter::MotionSettings());
            uint64_t s = 0;
            for (int i = 0; i < 4; ++i) observer.ObserveGrid(++s, TexturedGrid(1, 100, 400), Film());
            // A discontinuity alone (lost frames) is no cut.
            IntentInterpreter::FrameContext gap = Film();
            gap.discontinuity = true;
            observer.ObserveGrid(++s, TexturedGrid(1, 100, 400), gap);
            Assert::AreEqual(size_t(1), observer.ShotsKnown(), L"no cut at a discontinuity");
            // A format change is a boundary.
            observer.ObserveGrid(++s, TexturedGrid(1, 100, 400), Film(), true);
            Assert::AreEqual(size_t(2), observer.ShotsKnown(), L"format change");
            // A hard cut.
            observer.ObserveGrid(++s, TexturedGrid(9, 520, 900), Film());
            Assert::AreEqual(size_t(3), observer.ShotsKnown(), L"cut");
            // Already-seen frames (a later walk) are ignored.
            observer.ObserveGrid(s, TexturedGrid(3, 100, 300), Film());
            Assert::AreEqual(uint64_t(7), observer.FramesObserved());
            // Display moves forward only.
            observer.AdvanceDisplay(7);
            observer.AdvanceDisplay(2);
            Assert::AreEqual(uint64_t(7), observer.DisplayedShotStart());
        }

        TEST_METHOD(ObserverHandsShadowStatisticsThrough)
        {
            IntentInterpreter::Switches sw;
            sw.black = true;
            IntentInterpreter::Observer observer;
            observer.Configure(sw, IntentInterpreter::MotionSettings());
            const ContentIntent::LumaGrid grid = TexturedGrid(5, 64, 300);
            const IntentInterpreter::FrameContext context = Film();
            observer.ObserveGrid(1, grid, context);
            observer.AdvanceDisplay(1);
            const BlackIntent::ShadowStats direct = BlackIntent::ShadowStatsFromCodes(grid.luma.data(), grid.luma.size(),
                10, true, context.transfer, context.gamma, context.sdrWhiteNits, context.displayBlackNits);
            const BlackIntent::ShadowStats handed = observer.DisplayedShadows();
            Assert::AreEqual(direct.valid, handed.valid);
            Assert::AreEqual(direct.p1Nits, handed.p1Nits);
            Assert::AreEqual(direct.p5Nits, handed.p5Nits);
            Assert::AreEqual(direct.p10Nits, handed.p10Nits);
            Assert::AreEqual(direct.samples, handed.samples);
            // Black protection off: no shadow measurement at all.
            IntentInterpreter::Switches ambientOnly;
            ambientOnly.ambient = true;
            IntentInterpreter::Observer other;
            other.Configure(ambientOnly, IntentInterpreter::MotionSettings());
            other.ObserveGrid(1, grid, context);
            other.AdvanceDisplay(1);
            Assert::IsFalse(other.DisplayedShadows().valid);
            Assert::AreEqual(0u, other.DisplayedShadows().samples);
        }

        TEST_METHOD(DescriptorFieldsAndPrecedence)
        {
            IntentInterpreter::Switches sw;
            sw.display = true;
            IntentInterpreter::Observer observer;
            observer.Configure(sw, IntentInterpreter::MotionSettings());
            observer.SetContentField(IntentField<ContentClass>::Make(ContentClass::Film, IntentSource::DvL11));
            observer.SetDisplayField(IntentField<float>::Make(4.4f, IntentSource::DvL1));
            observer.SetAmbientField(IntentField<float>::Make(0.2f, IntentSource::Inferred));
            const IntentDescriptor d = observer.Descriptor();
            Assert::IsTrue(d.content.valid && d.content.value == ContentClass::Film && d.content.source == IntentSource::DvL11);
            Assert::AreEqual(4.4f, d.displayCompressionStops.value);
            Assert::IsFalse(d.motionScale.valid, L"motion off: no field");
            const IntentField<float> inferred = IntentField<float>::Make(0.55f, IntentSource::Inferred);
            const IntentField<float> dv2 = IntentField<float>::Make(0.40f, IntentSource::Dv2);
            Assert::AreEqual(0.40f, Prefer(inferred, dv2).value, L"Dv2 > Inferred");
            Assert::AreEqual(0.55f, Prefer(inferred, IntentField<float>()).value, L"Off never wins");
        }

        TEST_METHOD(AmbientTargetBlackIsBitIdenticalWhenOffOrAtStrengthZero)
        {
            const double owner = 0.0123456789;
            AmbientAdapt::Settings s;
            s.screenReflectNitsPerLux = 0.01;
            s.maxBlackNits = 1.0;
            AmbientAdapt::Filter filter;
            filter.Configure(s);
            for (double lux = 0.0; lux < 500.0; lux += 37.0)
            {
                filter.AddReading(lux, lux);
                IntentInterpreter::AmbientApplyState st;
                s.enabled = false;
                s.strength = 1.0;
                Assert::IsTrue(SameBits(owner, IntentInterpreter::AmbientStep(st, s, filter.Valid(), filter.SmoothedLux(),
                    owner, true, lux)), L"off: the owner's black, bit for bit");
                s.enabled = true;
                s.strength = 0.0;
                Assert::IsTrue(SameBits(owner, IntentInterpreter::AmbientStep(st, s, filter.Valid(), filter.SmoothedLux(),
                    owner, true, lux)), L"strength 0: the owner's black, bit for bit");
                Assert::IsFalse(st.valid);
            }
            // On: unchanged until a cut, then never below the owner's black.
            s.enabled = true;
            s.strength = 1.0;
            IntentInterpreter::AmbientApplyState st;
            filter.AddReading(100.0, 1000.0);
            const double before = IntentInterpreter::AmbientStep(st, s, true, 100.0, owner, false, 1000.0);
            Assert::IsTrue(SameBits(owner, before), L"no cut yet: no step");
            const double atCut = IntentInterpreter::AmbientStep(st, s, true, 100.0, owner, true, 1001.0);
            Assert::IsTrue(atCut >= owner, L"never below the owner's black");
            Assert::AreEqual(AmbientAdapt::QuantisedBlackNits(s, owner, 100.0), atCut, L"the quantised B_eff at the cut");
            // Sensor lost (filter invalid): back to the owner's black at a cut.
            const double lost = IntentInterpreter::AmbientStep(st, s, false, 0.0, owner, true, 1002.0);
            Assert::AreEqual(owner, lost);
        }

        TEST_METHOD(AmbientLuxFromTheDetector)
        {
            Ambient::Settings settings;
            settings.luxLit = 25.0;
            Ambient::State state;
            double lux = -1.0;
            state.Configure(settings);
            Assert::IsFalse(IntentInterpreter::AmbientLuxFromDetector(state, 10.0, lux), L"off: invalid");
            settings.mode = Ambient::Mode::Manual;
            settings.room = Ambient::Room::Dark;
            state.Configure(settings);
            Assert::IsTrue(IntentInterpreter::AmbientLuxFromDetector(state, 10.0, lux));
            Assert::AreEqual(0.0, lux, L"manual dark = 0 lux");
            const Ambient::Room lit = Ambient::Room::Lit;
            Assert::IsTrue(IntentInterpreter::AmbientLuxFromDetector(state, 10.0, lux, &lit));
            Assert::AreEqual(25.0, lux, L"key override lit = lux_lit");
            settings.room = Ambient::Room::Lit;
            state.Configure(settings);
            Assert::IsTrue(IntentInterpreter::AmbientLuxFromDetector(state, 10.0, lux));
            Assert::AreEqual(25.0, lux, L"manual lit = lux_lit");
            settings.mode = Ambient::Mode::Sensor;
            settings.lostSeconds = 10.0;
            state.Configure(settings);
            Assert::IsFalse(IntentInterpreter::AmbientLuxFromDetector(state, 10.0, lux), L"no reading yet");
            state.AddReading(42.5, 11.0);
            Assert::IsTrue(IntentInterpreter::AmbientLuxFromDetector(state, 12.0, lux));
            Assert::AreEqual(42.5, lux, L"the newest reading");
            Assert::IsFalse(IntentInterpreter::AmbientLuxFromDetector(state, 30.0, lux), L"lost: invalid");
        }

        TEST_METHOD(StatusTextEmptyWhenOffAndLedLimitChanges)
        {
            IntentStatus off;
            Assert::IsTrue(IntentInterpreter::MotionIntentLine(off.motion).empty());
            Assert::IsTrue(IntentInterpreter::AmbientIntentLine(off.ambient).empty());
            Assert::IsTrue(IntentInterpreter::BlackIntentLine(off.black).empty());
            Assert::IsTrue(IntentInterpreter::DisplayIntentLine(off.display).empty());
            IntentStatus on;
            on.motion.enabled = true;
            on.motion.policy = MotionIntentPolicy::Visibility;
            on.motion.appliedScale = 0.62f;
            CopyIntentText(on.motion.reason, "judder visibility");
            on.ambient.enabled = true;
            on.black.mode = BlackIntentMode::Report;
            on.display.enabled = true;
            Assert::IsTrue(IntentInterpreter::MotionIntentLine(on.motion).find("visibility x0.62") == 0);
            Assert::IsFalse(IntentInterpreter::AmbientIntentLine(on.ambient).empty());
            Assert::IsTrue(IntentInterpreter::BlackIntentLine(on.black).find("report: no shadow statistics") == 0);
            Assert::AreEqual(std::string("no scene maximum"), IntentInterpreter::DisplayIntentLine(on.display));
            IntentInterpreter::LedIntentLimits a, b;
            Assert::IsFalse(IntentInterpreter::LedLimitsDiffer(a, b), L"0 / 0 = nothing to send");
            b.shadowMinimumShare = 0.002;
            Assert::IsTrue(IntentInterpreter::LedLimitsDiffer(a, b), L"from no cap to a cap");
            a.shadowMinimumShare = 0.3;
            b.shadowMinimumShare = 0.302;
            Assert::IsFalse(IntentInterpreter::LedLimitsDiffer(a, b), L"small changes are not resent");
            b.ambientBlackNits = 0.1;
            Assert::IsTrue(IntentInterpreter::LedLimitsDiffer(a, b));
        }

        // The renderer's parsing (LoadRendererSettings) through its settings dump: every
        // key's default, valid values, invalid values (kept) and the live-apply fingerprint.
        TEST_METHOD(RendererParsesEveryIntentKey)
        {
            const std::string folder = Here();
            HMODULE dll = LoadLibraryExA((folder + "vprenderer\\VideoProcessorVPRenderer.dll").c_str(), nullptr,
                LOAD_WITH_ALTERED_SEARCH_PATH);
            Assert::IsNotNull(dll, L"the renderer DLL next to the tests");
            using Export = int(__cdecl*)(const char*, char*, int);
            const auto describe = reinterpret_cast<Export>(GetProcAddress(dll, "VideoProcessorLibplaceboDescribeSettingsForTest"));
            const auto fingerprintOf = reinterpret_cast<Export>(GetProcAddress(dll, "VideoProcessorLibplaceboSettingsFingerprintForTest"));
            Assert::IsNotNull(reinterpret_cast<void*>(describe));
            Assert::IsNotNull(reinterpret_cast<void*>(fingerprintOf));
            const std::string path = folder + "vp-intent-keys-test.cfg";
            auto call = [&](Export function, const std::string& keys) {
                {
                    std::ofstream config(path, std::ios::trunc | std::ios::binary);
                    config << "[general]\npersist_profile_selection: false\n[vprenderer.Default]\nquality: high\n" << keys;
                }
                std::vector<char> buffer(1 << 20);
                const int length = function(path.c_str(), buffer.data(), static_cast<int>(buffer.size()));
                Assert::IsTrue(length > 0, L"the export ran");
                return std::string(buffer.data());
            };
            auto line = [](const std::string& dump, const std::string& key) {
                const size_t at = dump.find("\n" + key + "=");
                if (at == std::string::npos) return std::string("(missing)");
                const size_t start = at + key.size() + 2;
                return dump.substr(start, dump.find('\n', start) - start);
            };
            auto expect = [&](const std::string& dump, const char* key, const char* value) {
                const std::string got = line(dump, key);
                Assert::AreEqual(std::string(value), got, (std::wstring(key, key + std::strlen(key)) + L"=" +
                    std::wstring(got.begin(), got.end())).c_str());
            };
            const std::string base = call(describe, "");
            const std::pair<const char*, const char*> defaults[] = {
                { "intent_motion", "off" }, { "intent_motion_policy", "display" }, { "intent_motion_min", "0.5" },
                { "intent_motion_min_shot_s", "2" }, { "ambient_adapt", "off" }, { "ambient_adapt_strength", "1" },
                { "ambient_screen_reflect", "0.01" }, { "ambient_max_black_nits", "1" }, { "ambient_tau_up_s", "15" },
                { "ambient_tau_down_s", "90" }, { "intent_black", "off" }, { "intent_display", "off" } };
            for (const auto& d : defaults)
                expect(base, d.first, d.second);
            const std::string set = call(describe,
                "intent_motion: on\nintent_motion_policy: ease_slow\nintent_motion_min: 0.25\nintent_motion_min_shot_s: 4\n"
                "ambient_adapt: on\nambient_adapt_strength: 0.5\nambient_screen_reflect: 0.02\nambient_max_black_nits: 2.5\n"
                "ambient_tau_up_s: 30\nambient_tau_down_s: 120\nintent_black: led_protect\nintent_display: report\n");
            const std::pair<const char*, const char*> values[] = {
                { "intent_motion", "on" }, { "intent_motion_policy", "ease_slow" }, { "intent_motion_min", "0.25" },
                { "intent_motion_min_shot_s", "4" }, { "ambient_adapt", "on" }, { "ambient_adapt_strength", "0.5" },
                { "ambient_screen_reflect", "0.02" }, { "ambient_max_black_nits", "2.5" }, { "ambient_tau_up_s", "30" },
                { "ambient_tau_down_s", "120" }, { "intent_black", "led_protect" }, { "intent_display", "report" } };
            for (const auto& v : values)
                expect(set, v.first, v.second);
            for (const char* policy : { "display", "ease_fast", "ease_slow", "visibility", "metadata" })
                expect(call(describe, std::string("intent_motion_policy: ") + policy + "\n"), "intent_motion_policy", policy);
            expect(call(describe, "intent_black: report\n"), "intent_black", "report");
            // Invalid values: the default is kept (out of range, wrong words).
            const std::string invalid = call(describe,
                "intent_motion: auto\nintent_motion_policy: fast\nintent_motion_min: 1.5\nintent_motion_min_shot_s: 0.2\n"
                "ambient_adapt: yes\nambient_adapt_strength: -1\nambient_screen_reflect: 2\nambient_max_black_nits: 11\n"
                "ambient_tau_up_s: 0.5\nambient_tau_down_s: 2000\nintent_black: on\nintent_display: on\n");
            for (const auto& d : defaults)
                expect(invalid, d.first, d.second);
            // Every key changes the fingerprint (live-apply change detection).
            const std::string baseFingerprint = call(fingerprintOf, "");
            for (const char* key : { "intent_motion: on\n", "intent_motion_policy: visibility\n", "intent_motion_min: 0.7\n",
                "intent_motion_min_shot_s: 3\n", "ambient_adapt: on\n", "ambient_adapt_strength: 0.4\n",
                "ambient_screen_reflect: 0.03\n", "ambient_max_black_nits: 3\n", "ambient_tau_up_s: 20\n",
                "ambient_tau_down_s: 100\n", "intent_black: report\n", "intent_display: report\n" })
                Assert::AreNotEqual(baseFingerprint, call(fingerprintOf, key), std::wstring(key, key + std::strlen(key)).c_str());
            DeleteFileA(path.c_str());
        }
    };
    // Owner 2026-10-17: active_picture_lookahead_frames follows the queue unless set.
    TEST_CLASS(QueueLookaheadTests)
    {
    public:
        TEST_METHOD(AbsentFollowsTheQueue)
        {
            using QueueLookahead::ResolveActivePictureLookahead;
            Assert::AreEqual(size_t(4), ResolveActivePictureLookahead(false, 0, false, 5, 32), L"Normal: target 5 -> 4");
            Assert::AreEqual(size_t(0), ResolveActivePictureLookahead(false, 0, true, 5, 32), L"Low latency: 0");
            Assert::AreEqual(size_t(2), ResolveActivePictureLookahead(false, 0, false, 5, 3), L"short queue: queue_size - 1");
            Assert::AreEqual(size_t(8), ResolveActivePictureLookahead(false, 0, false, 20, 32), L"cap 8");
            Assert::AreEqual(size_t(0), ResolveActivePictureLookahead(false, 0, false, 1, 1), L"one frame: nothing ahead");
            Assert::AreEqual(size_t(0), ResolveActivePictureLookahead(false, 0, false, 0, 0), L"nothing known");
        }

        TEST_METHOD(AnExplicitValueAlwaysWins)
        {
            using QueueLookahead::ResolveActivePictureLookahead;
            Assert::AreEqual(size_t(0), ResolveActivePictureLookahead(true, 0, false, 5, 32), L"explicit 0");
            Assert::AreEqual(size_t(6), ResolveActivePictureLookahead(true, 6, false, 5, 3), L"explicit above the queue");
            Assert::AreEqual(size_t(3), ResolveActivePictureLookahead(true, 3, true, 1, 1), L"explicit on low latency");
            Assert::AreEqual(size_t(8), ResolveActivePictureLookahead(true, 12, false, 5, 32), L"explicit still capped at 8");
        }
    };

    // The queue-fed intent observer (owner 2026-10-17): the walk covers the queue ahead
    // while an intent function is on, with active_picture_lookahead_frames 0 and no LED.
    TEST_CLASS(QueueFedIntentWalkTests)
    {
        struct Entry { bool cadenceRepeat = false; uint64_t sequence = 0; };

    public:
        TEST_METHOD(WantedDepthIsTheQueueAheadAndAtLeastTheCurrentFrame)
        {
            Assert::AreEqual(size_t(1), IntentInterpreter::WantedWalkFrames(0));
            Assert::AreEqual(size_t(1), IntentInterpreter::WantedWalkFrames(1), L"low latency: current frame");
            Assert::AreEqual(size_t(4), IntentInterpreter::WantedWalkFrames(5));
            Assert::AreEqual(IntentInterpreter::kMaxWalkFrames, IntentInterpreter::WantedWalkFrames(200), L"bounded");
        }

        TEST_METHOD(ObserverSeesTheQueueWithActivePictureLookaheadZero)
        {
            IntentInterpreter::Switches sw;
            sw.motion = true;
            IntentInterpreter::Observer observer;
            observer.Configure(sw, IntentInterpreter::MotionSettings());
            std::vector<Entry> queue;
            for (uint64_t s = 1; s <= 6; ++s)
                queue.push_back(Entry{ s == 4, s });   // one cadence repeat
            // active_picture_lookahead_frames 0, no LED: only the intent depth.
            const auto walk = AlphaQueuePolicy::SelectLookaheadWalk(queue, 0, (std::max)(size_t{0},
                IntentInterpreter::WantedWalkFrames(queue.size())), 8);
            Assert::AreEqual(size_t(0), walk.activePictureFrames, L"no active-picture work");
            Assert::AreEqual(size_t(5), walk.window.indices.size(), L"the current frame and the queue ahead (repeat skipped)");
            // The walk (observeStatisticsOnly) skips frames already observed before decoding.
            uint64_t decoded = 0;
            for (int round = 0; round < 3; ++round)
                for (const size_t index : walk.window.indices)
                {
                    if (queue[index].sequence <= observer.LastObservedSequence())
                        continue;
                    ++decoded;
                    observer.ObserveGrid(queue[index].sequence, TexturedGrid(1, 100, 400), Film());
                }
            Assert::AreEqual(uint64_t(5), decoded, L"one analysis per source frame");
            Assert::AreEqual(uint64_t(5), observer.FramesObserved());
            Assert::AreEqual(uint64_t(6), observer.LastObservedSequence());
        }

        TEST_METHOD(LowLatencyWalksTheCurrentFrameOnly)
        {
            IntentInterpreter::Switches sw;
            sw.motion = true;
            IntentInterpreter::MotionSettings ms;
            ms.policy = MotionIntentPolicy::Visibility;
            IntentInterpreter::Observer observer;
            observer.Configure(sw, ms);
            std::vector<Entry> queue = { Entry{ false, 1 } };
            const auto walk = AlphaQueuePolicy::SelectLookaheadWalk(queue, 0, IntentInterpreter::WantedWalkFrames(queue.size()), 8);
            Assert::AreEqual(size_t(1), walk.window.indices.size(), L"current frame only");
            // Frame by frame as they arrive: a cut has no measured pair yet when shown, so
            // it waits (the missed-cut rule); nothing changes mid-shot.
            uint64_t s = 0;
            for (int i = 0; i < 24; ++i)
            {
                observer.ObserveGrid(++s, TexturedGrid(1, 100, 400), Film());
                Assert::AreEqual(1.0, observer.AppliedMotionScale(s), L"first shot: missed at its cut");
            }
            const uint64_t cut = s + 1;
            observer.ObserveGrid(++s, TexturedGrid(2, 500, 900), Film());
            Assert::AreEqual(1.0, observer.AppliedMotionScale(cut), L"the cut arrives alone: missed");
            Assert::AreEqual(std::string("missed cut: waits for the next cut"), std::string(observer.MotionStatus().reason));
        }
    };

    // CPU cost of the walk per frame (owner 2026-10-17). Off by default: set
    // VP_TEST_BENCH=1 (and optionally VP_TEST_BENCH_OUT=<file>). Times the CPU work the
    // walk does per source frame: PrepareAnalysisFrame (native v210), the active-picture
    // evidence (ExtractActivePictureEvidence) and the intent observer's step (grid +
    // measure), on synthetic 3840x2160 and 1920x1080 frames. No GPU.
    TEST_CLASS(IntentWalkBenchmark)
    {
        static double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
        {
            return std::chrono::duration<double, std::milli>(b - a).count();
        }

    public:
        TEST_METHOD(WalkCpuCostPerFrame)
        {
            char flag[8] = {};
            if (GetEnvironmentVariableA("VP_TEST_BENCH", flag, sizeof(flag)) == 0 || flag[0] != '1')
            {
                Logger::WriteMessage("IntentWalkBenchmark skipped (set VP_TEST_BENCH=1)");
                return;
            }
            std::ostringstream report;
            const int sizes[2][2] = { { 3840, 2160 }, { 1920, 1080 } };
            constexpr int kIterations = 12;
            for (const auto& size : sizes)
            {
                const int w = size[0], h = size[1];
                const int bars = h / 8;   // letterbox bars top and bottom
                // ---- P010 (Y plane, then interleaved UV), textured with bars ----
                auto p010 = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 3);
                uint32_t state = 12345u;
                uint16_t* y = reinterpret_cast<uint16_t*>(p010->data());
                for (int row = 0; row < h; ++row)
                    for (int x = 0; x < w; ++x)
                    {
                        state = state * 1664525u + 1013904223u;
                        const int code = row < bars || row >= h - bars ? 0 : 100 + static_cast<int>((state >> 8) % 800u);
                        y[static_cast<size_t>(row) * w + x] = static_cast<uint16_t>(code << 6);
                    }
                uint16_t* uv = y + static_cast<size_t>(w) * h;
                for (size_t i = 0; i < static_cast<size_t>(w) * h / 2; ++i) uv[i] = static_cast<uint16_t>(512 << 6);
                AnalysisLumaSource p010Source;
                p010Source.data = p010->data();
                p010Source.dataBytes = p010->size();
                p010Source.width = w;
                p010Source.height = h;
                p010Source.rowBytes = static_cast<size_t>(w) * 2;
                p010Source.chromaRowBytes = static_cast<size_t>(w) * 2;
                p010Source.format = AnalysisLumaFormat::P010;
                p010Source.colorspace = ColorSpace::REC_709;
                p010Source.fullRangeYuv = false;
                p010Source.backingOwner = p010;
                // ---- native v210 ----
                const size_t v210Row = static_cast<size_t>((w + 47) / 48) * 128;
                auto v210 = std::make_shared<std::vector<uint8_t>>(v210Row * h);
                uint32_t* words = reinterpret_cast<uint32_t*>(v210->data());
                for (size_t i = 0; i < v210->size() / 4; ++i)
                {
                    state = state * 1664525u + 1013904223u;
                    const uint32_t a = 64 + (state >> 8) % 876u, b = 64 + (state >> 12) % 876u, c = 512;
                    words[i] = (i % 2 == 0) ? (c | (a << 10) | (c << 20)) : (b | (c << 10) | (a << 20));
                }
                AnalysisFrameInput input;
                input.nativeData = v210->data();
                input.nativeBytes = v210->size();
                input.nativeRowBytes = v210Row;
                input.nativeOwner = v210;
                input.width = w;
                input.height = h;
                input.encoding = VideoFrameEncoding::V210;
                input.colorspace = ColorSpace::REC_709;
                input.sourceGeneration = 1;
                input.profile = AnalysisSourceProfile::QueuedCapture;
                double prepareMs = 0.0, v210EvidenceMs = 0.0, p010EvidenceMs = 0.0, observeP010Ms = 0.0, observeV210Ms = 0.0;
                bool v210Valid = false;
                IntentInterpreter::Switches sw;
                sw.motion = true;
                sw.black = true;
                IntentInterpreter::Observer observer;
                observer.Configure(sw, IntentInterpreter::MotionSettings());
                observer.SetFrameContext(Film());
                QueuedFrameStatistics::PictureRect rect;
                rect.top = bars;
                rect.right = w;
                rect.bottom = h - bars;
                uint64_t seq = 0;
                for (int i = 0; i < kIterations; ++i)
                {
                    auto t0 = std::chrono::steady_clock::now();
                    const PreparedAnalysisFrame prepared = PrepareAnalysisFrame(input);
                    auto t1 = std::chrono::steady_clock::now();
                    v210Valid = prepared.IsValid();
                    if (v210Valid)
                    {
                        const ActivePictureEvidence e = ExtractActivePictureEvidence(prepared.source);
                        (void)e;
                    }
                    auto t2 = std::chrono::steady_clock::now();
                    const ActivePictureEvidence e2 = ExtractActivePictureEvidence(p010Source);
                    (void)e2;
                    auto t3 = std::chrono::steady_clock::now();
                    observer.ObserveFrame(++seq, 0, p010Source, rect);
                    auto t4 = std::chrono::steady_clock::now();
                    if (v210Valid)
                        observer.ObserveFrame(++seq, 0, prepared.source, rect);
                    auto t5 = std::chrono::steady_clock::now();
                    if (i == 0) continue;   // warm-up
                    prepareMs += Ms(t0, t1);
                    v210EvidenceMs += Ms(t1, t2);
                    p010EvidenceMs += Ms(t2, t3);
                    observeP010Ms += Ms(t3, t4);
                    observeV210Ms += Ms(t4, t5);
                }
                const double n = kIterations - 1;
                const double v210Frame = (prepareMs + v210EvidenceMs + observeV210Ms) / n;
                const double p010Frame = (p010EvidenceMs + observeP010Ms) / n;
                report << w << "x" << h << ": v210 prepare " << prepareMs / n << " ms, v210 evidence " << v210EvidenceMs / n
                    << " ms, v210 intent step " << observeV210Ms / n << " ms (valid " << v210Valid << "); P010 evidence "
                    << p010EvidenceMs / n << " ms, P010 intent step " << observeP010Ms / n << " ms. Per source frame: v210 "
                    << v210Frame << " ms, P010 " << p010Frame << " ms. At 24 fps: intent-only walk (each frame once) v210 "
                    << 24.0 * v210Frame << " ms/s, P010 " << 24.0 * p010Frame << " ms/s; uncached walk of current + 4 ahead "
                    << "(5 analyses per render) v210 " << 120.0 * v210Frame << " ms/s, P010 " << 120.0 * p010Frame << " ms/s\n";
                Assert::IsTrue(observer.FramesObserved() > 0);
            }
            Logger::WriteMessage(report.str().c_str());
            char out[MAX_PATH] = {};
            if (GetEnvironmentVariableA("VP_TEST_BENCH_OUT", out, sizeof(out)) > 0)
            {
                std::ofstream file(out, std::ios::trunc);
                file << report.str();
            }
        }
    };
}
