// Content recognition (intent interpreter item 1, docs/VP-intent-interpreter-design.md,
// owner 2026-10-17): the rule-based classifier across rates and content, L11 precedence,
// the hysteresis, the projection motion measure, the observer end to end, the rules'
// `content` variable (empty while off) and when:-rule selection.
#include "pch.h"
#include "CppUnitTest.h"
#include <ConfigFile.h>
#include <ContentClassifier.h>
#include <ContentIntentObserver.h>
#include <IntentDescriptor.h>
#include <MainConfigSchema.h>
#include <RendererProfileConfig.h>
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
    namespace
    {
        using namespace ContentIntent;
        using SourceClass = InterpolationPolicy::SourceClass;

        // classFixes false: the tables below are the behaviour before cadence_class_fixes
        // (Hunt2CadenceClass* tests cover the fixes, the default).
        FrameContext Context(double contentHz, double displayHz, SourceClass sourceClass = SourceClass::Legacy,
            bool pulldownLocked = false, bool classFixes = false)
        {
            FrameContext c;
            c.classFixes = classFixes;
            c.contentHz = contentHz;
            c.displayHz = displayHz;
            c.sourceClass = sourceClass;
            c.pulldownLocked = pulldownLocked;
            return c;
        }

        WindowStats PanStats(double meanPan, double wideShare, double seconds = 10.0)
        {
            WindowStats s;
            s.motionValid = true;
            s.seconds = seconds;
            s.meanPanHps = meanPan;
            s.widePanShare = wideShare;
            return s;
        }

        ContentClass ClassOf(const FrameContext& c, const WindowStats& stats)
        {
            ClassifierInput in;
            in.l11Present = c.l11Present;
            in.l11ContentType = c.l11ContentType;
            in.filmLike = FilmLike(c, in.rateKnown);
            in.pulldownLocked = c.pulldownLocked;
            in.stats = stats;
            in.classFixes = c.classFixes;
            return Classify(in).content;
        }

        const wchar_t* Name(ContentClass c)
        {
            switch (c)
            {
            case ContentClass::Film: return L"film";
            case ContentClass::Video: return L"video";
            case ContentClass::Sports: return L"sports";
            case ContentClass::Animation: return L"animation";
            case ContentClass::StaticUi: return L"static_ui";
            default: return L"unknown";
            }
        }

        void Expect(ContentClass expected, ContentClass actual, const std::wstring& what)
        {
            Assert::IsTrue(expected == actual, (what + L": expected " + Name(expected) + L", got " + Name(actual)).c_str());
        }

        // A textured picture panning over a wide random canvas: column values vary
        // (projections can track them), rows add a fixed profile. low..high = the
        // canvas's luma range (10-bit limited codes).
        struct Canvas
        {
            std::vector<uint16_t> columns, rows;
            Canvas(int width, int height, uint32_t seed, int low, int high)
            {
                uint32_t state = seed;
                auto next = [&state]() { state = state * 1664525u + 1013904223u; return state >> 8; };
                for (int x = 0; x < width; ++x)
                    columns.push_back(static_cast<uint16_t>(low + static_cast<int>(next() % static_cast<uint32_t>(high - low))));
                for (int y = 0; y < height; ++y)
                    rows.push_back(static_cast<uint16_t>(next() % 24));
            }
            LumaGrid Window(int offset, int width = 192, int height = 108) const
            {
                LumaGrid g;
                g.width = width;
                g.height = height;
                g.pictureAspect = 16.0 / 9.0;
                g.luma.resize(static_cast<size_t>(width) * height);
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x)
                        g.luma[static_cast<size_t>(y) * width + x] =
                            static_cast<uint16_t>(columns[static_cast<size_t>(x + offset)] + rows[static_cast<size_t>(y)]);
                return g;
            }
        };

        // Feeds `frames` frames panning `cellsPerFrame` grid cells right per frame
        // (0 = static), continuing from offset; returns the next offset.
        int Feed(Observer& observer, const Canvas& canvas, uint64_t& sequence, int offset, int frames,
            int cellsPerFrame, const FrameContext& context)
        {
            for (int i = 0; i < frames; ++i)
            {
                observer.ObserveGrid(++sequence, canvas.Window(offset), context);
                offset += cellsPerFrame;
            }
            return offset;
        }

        struct TempConfig
        {
            std::string path;
            TempConfig()
            {
                char directory[MAX_PATH]{}, file[MAX_PATH]{};
                Assert::IsTrue(GetTempPathA(MAX_PATH, directory) != 0);
                Assert::IsTrue(GetTempFileNameA(directory, "vpi", 0, file) != 0);
                path = file;
            }
            ~TempConfig() { DeleteFileA(path.c_str()); }
        };
    }

    TEST_CLASS(ContentIntentTests)
    {
    public:
        TEST_METHOD(ClassifierTableAcrossRatesAndContent)
        {
            const WindowStats none;
            const WindowStats pans = PanStats(0.40, 0.60);
            const WindowStats slowPans = PanStats(0.05, 0.05);
            // source_class legacy: the measured content rate alone decides film / video.
            struct Row { double hz; bool film; };
            const Row legacy[] = { { 24000.0 / 1001.0, true }, { 24.0, true }, { 25.0, true },
                { 30000.0 / 1001.0, true }, { 30.0, true }, { 48.0, true }, { 50.0, false },
                { 60000.0 / 1001.0, false }, { 60.0, false }, { 100.0, false }, { 120.0, false } };
            for (const Row& r : legacy)
            {
                const std::wstring at = L"legacy " + std::to_wstring(r.hz) + L" Hz";
                const FrameContext c = Context(r.hz, 60.0);
                Expect(r.film ? ContentClass::Film : ContentClass::Video, ClassOf(c, none), at + L" no motion");
                Expect(r.film ? ContentClass::Film : ContentClass::Sports, ClassOf(c, pans), at + L" sustained pans");
                Expect(r.film ? ContentClass::Film : ContentClass::Video, ClassOf(c, slowPans), at + L" slow pans");
            }
            // source_class measured: film = 2+ display refreshes per frame or a pulldown lock;
            // the display rate enters (nothing fixed: 30p on a 30 Hz display is video,
            // on a 60 Hz display film-like; 60p on a 120 Hz display is film-like by that rule).
            struct MeasuredRow { double hz, display; bool film; };
            const MeasuredRow measured[] = { { 24.0, 60.0, true }, { 30.0, 60.0, true }, { 30.0, 30.0, false },
                { 48.0, 60.0, false }, { 50.0, 50.0, false }, { 50.0, 60.0, false }, { 60.0, 60.0, false },
                { 60.0, 120.0, true }, { 100.0, 120.0, false } };
            for (const MeasuredRow& r : measured)
            {
                const std::wstring at = L"measured " + std::to_wstring(r.hz) + L" on " + std::to_wstring(r.display);
                const FrameContext c = Context(r.hz, r.display, SourceClass::Measured);
                Expect(r.film ? ContentClass::Film : ContentClass::Video, ClassOf(c, none), at);
                Expect(r.film ? ContentClass::Film : ContentClass::Sports, ClassOf(c, pans), at + L" pans");
            }
            // Measured without a display rate yet: unknown (no guess).
            Expect(ContentClass::Unknown, ClassOf(Context(50.0, 0.0, SourceClass::Measured), pans), L"no display rate");
            Expect(ContentClass::Unknown, ClassOf(Context(0.0, 60.0), pans), L"no content rate");
            // A pulldown lock is film at any carrier (25 in 50, 24 in 60, 24 in 120).
            for (const double carrier : { 50.0, 60.0, 120.0 })
                Expect(ContentClass::Film, ClassOf(Context(carrier == 50.0 ? 25.0 : 24.0, carrier,
                    SourceClass::Measured, true), pans), L"pulldown lock");
            // Sports needs sustained pans: too short a window, too few wide pans, too slow.
            const FrameContext video = Context(50.0, 50.0);
            Expect(ContentClass::Video, ClassOf(video, PanStats(0.40, 0.60, 3.0)), L"short window");
            Expect(ContentClass::Video, ClassOf(video, PanStats(0.40, 0.10)), L"few wide pans");
            Expect(ContentClass::Video, ClassOf(video, PanStats(0.08, 0.60)), L"slow mean pan");
            WindowStats invalid = pans;
            invalid.motionValid = false;
            Expect(ContentClass::Video, ClassOf(video, invalid), L"no motion statistics");
            // Animation: twos-holds and flat areas, not a perfectly even (carrier-like) hold.
            WindowStats drawn;
            drawn.motionValid = true;
            drawn.seconds = 10.0;
            drawn.holdShare = 0.7;
            drawn.flatShare = 0.5;
            Expect(ContentClass::Animation, ClassOf(Context(24.0, 60.0), drawn), L"animation");
            Expect(ContentClass::Animation, ClassOf(video, drawn), L"animation at a video rate");
            WindowStats even = drawn;
            even.evenHolds = true;
            Expect(ContentClass::Film, ClassOf(Context(24.0, 60.0), even), L"even holds without a lock: not animation");
            Expect(ContentClass::Animation, ClassOf(Context(24.0, 60.0, SourceClass::Legacy, true), even),
                L"even holds of the film frames under a lock");
            WindowStats unflat = drawn;
            unflat.flatShare = 0.1;
            Expect(ContentClass::Film, ClassOf(Context(24.0, 60.0), unflat), L"holds without flat areas");
            // Static UI: zero motion with static edges for long enough, at any rate.
            WindowStats still;
            still.stillSeconds = kStaticUiSeconds + 1.0;
            Expect(ContentClass::StaticUi, ClassOf(Context(24.0, 60.0), still), L"still menu over film");
            Expect(ContentClass::StaticUi, ClassOf(video, still), L"still menu over video");
            still.stillSeconds = kStaticUiSeconds - 1.0;
            Expect(ContentClass::Film, ClassOf(Context(24.0, 60.0), still), L"a short still");
        }

        TEST_METHOD(L11ContentTypeTakesPrecedence)
        {
            const WindowStats pans = PanStats(0.40, 0.60);
            auto withL11 = [](FrameContext c, uint8_t type) { c.l11Present = true; c.l11ContentType = type; return c; };
            ClassifierInput in;
            in.l11Present = true;
            in.l11ContentType = 3;
            in.filmLike = true;
            in.rateKnown = true;
            const Classification sport = Classify(in);
            Assert::IsTrue(sport.content == ContentClass::Sports && sport.source == IntentSource::DvL11);
            Assert::AreEqual("from metadata", DisplayName(sport.source));
            // Metadata wins over the measurement in both directions.
            Expect(ContentClass::Film, ClassOf(withL11(Context(60.0, 60.0), 1), pans), L"L11 movies on 60p pans");
            Expect(ContentClass::Sports, ClassOf(withL11(Context(24.0, 60.0), 3), PanStats(0, 0)), L"L11 sport at 24p");
            Expect(ContentClass::Video, ClassOf(withL11(Context(24.0, 60.0), 2), PanStats(0, 0)), L"L11 game");
            Expect(ContentClass::Video, ClassOf(withL11(Context(24.0, 60.0), 4), PanStats(0, 0)), L"L11 user-generated");
            // Default and reserved types say nothing: the measurement decides (inferred).
            for (const uint8_t type : { uint8_t(0), uint8_t(5), uint8_t(15) })
            {
                in.l11ContentType = type;
                in.filmLike = false;
                in.stats = pans;
                const Classification c = Classify(in);
                Assert::IsTrue(c.content == ContentClass::Sports && c.source == IntentSource::Inferred);
                Assert::AreEqual("inferred", DisplayName(c.source));
            }
            // Precedence of the shared field type: Dv2 > DvL11 > Inferred > Off.
            const auto inferred = IntentField<ContentClass>::Make(ContentClass::Video, IntentSource::Inferred);
            const auto l11 = IntentField<ContentClass>::Make(ContentClass::Sports, IntentSource::DvL11);
            const auto dv2 = IntentField<ContentClass>::Make(ContentClass::Film, IntentSource::Dv2);
            Assert::IsTrue(Prefer(inferred, l11).value == ContentClass::Sports);
            Assert::IsTrue(Prefer(l11, inferred).value == ContentClass::Sports);
            Assert::IsTrue(Prefer(l11, dv2).value == ContentClass::Film);
            Assert::IsTrue(Prefer(inferred, IntentField<ContentClass>()).value == ContentClass::Video);
            Assert::IsFalse(IntentField<ContentClass>::Make(ContentClass::Film, IntentSource::Off).valid);
        }

        TEST_METHOD(HysteresisChangesOnlyAfterHoldAndAtACut)
        {
            Hysteresis h;
            h.SetHoldSeconds(5.0);
            Classification video{ ContentClass::Video, IntentSource::Inferred, "v" };
            Classification sports{ ContentClass::Sports, IntentSource::Inferred, "s" };
            Classification film{ ContentClass::Film, IntentSource::Inferred, "f" };
            // The first class: after the hold, no cut needed.
            Assert::IsFalse(h.Update(video, 0.0, false));
            Assert::IsFalse(h.Update(video, 4.9, false));
            Assert::IsTrue(h.Current().content == ContentClass::Unknown);
            Assert::IsTrue(h.Update(video, 5.0, false));
            Assert::IsTrue(h.Current().content == ContentClass::Video);
            // A new class: a cut before the hold does not switch...
            Assert::IsFalse(h.Update(sports, 10.0, false));
            Assert::IsFalse(h.Update(sports, 12.0, true));
            // ...the hold without a cut does not switch...
            Assert::IsFalse(h.Update(sports, 15.5, false));
            Assert::IsFalse(h.Update(sports, 30.0, false));
            Assert::IsTrue(h.Current().content == ContentClass::Video);
            // ...the first cut after the hold does.
            Assert::IsTrue(h.Update(sports, 30.1, true));
            Assert::IsTrue(h.Current().content == ContentClass::Sports);
            // A flapping candidate restarts the hold each time it changes.
            Assert::IsFalse(h.Update(film, 31.0, false));
            Assert::IsFalse(h.Update(video, 34.0, false));
            Assert::IsFalse(h.Update(film, 37.0, true));
            Assert::IsFalse(h.Update(film, 41.0, true));
            Assert::IsTrue(h.Update(film, 42.0, true));
            Assert::IsTrue(h.Current().content == ContentClass::Film);
            // A candidate that returns to the current class cancels the pending change.
            Assert::IsFalse(h.Update(video, 43.0, false));
            Assert::IsFalse(h.Update(film, 47.0, false));
            Assert::IsFalse(h.Update(video, 49.0, true), L"the pending video was cancelled at 47 s");
            // Unknown never replaces a class.
            Assert::IsFalse(h.Update(Classification(), 100.0, true));
            Assert::IsTrue(h.Current().content == ContentClass::Film);
            // The same class now from metadata: the source changes at once.
            Classification filmL11{ ContentClass::Film, IntentSource::DvL11, "L11" };
            Assert::IsTrue(h.Update(filmL11, 100.5, false));
            Assert::IsTrue(h.Current().source == IntentSource::DvL11);
            // Static UI: entered after the hold without a cut, left at once.
            Classification menu{ ContentClass::StaticUi, IntentSource::Inferred, "still" };
            Assert::IsFalse(h.Update(menu, 101.0, false));
            Assert::IsTrue(h.Update(menu, 106.0, false));
            Assert::IsTrue(h.Current().content == ContentClass::StaticUi);
            Assert::IsTrue(h.Update(film, 106.1, false));
            Assert::IsTrue(h.Current().content == ContentClass::Film);
            // The hold is the key's value.
            Hysteresis quick;
            quick.SetHoldSeconds(1.0);
            quick.Update(video, 0.0, false);
            Assert::IsTrue(quick.Update(video, 1.0, false));
        }

        TEST_METHOD(ProjectionMotionHoldsAndCuts)
        {
            const Canvas canvas(600, 108, 7, 100, 500);
            const LumaGrid a = canvas.Window(100);
            for (const int shift : { 1, 3, -2, 8 })
            {
                const FrameMeasure m = Measure(a, canvas.Window(100 - shift));
                // The picture content moves right by `shift` cells when the window moves left.
                Assert::IsTrue(m.valid && !m.cut && !m.held);
                Assert::AreEqual(shift / 192.0 * (16.0 / 9.0), m.dxHeights, 0.002, L"horizontal pan");
                Assert::AreEqual(0.0, m.dyHeights, 0.002);
            }
            const FrameMeasure same = Measure(a, a);
            Assert::IsTrue(same.valid && same.held && !same.cut);
            Assert::AreEqual(0.0, same.dxHeights);
            // A cut: a different picture with a different luma distribution.
            const Canvas other(600, 108, 99, 600, 900);
            const FrameMeasure cut = Measure(a, other.Window(100));
            Assert::IsTrue(cut.cut);
            // Another part of the same picture is not a cut (same histogram).
            Assert::IsFalse(Measure(a, canvas.Window(400)).cut);
            // A different shape or a missing previous frame: no pair.
            Assert::IsFalse(Measure(LumaGrid(), a).valid);
            Assert::IsFalse(Measure(a, canvas.Window(0, 96, 54)).valid);
            // Flat areas: a uniform picture is all flat, the texture is not.
            LumaGrid flat = a;
            std::fill(flat.luma.begin(), flat.luma.end(), uint16_t(400));
            Assert::AreEqual(1.0, Measure(LumaGrid(), flat).flatShare);
            Assert::IsTrue(Measure(LumaGrid(), a).flatShare < 0.2);
            // The grid builder samples cell centres of the active rectangle.
            LumaGrid built;
            Assert::IsTrue(BuildGrid([](int x, int y, uint16_t& v) { v = static_cast<uint16_t>(x + y); return true; },
                0, 140, 3840, 2020, built));
            Assert::AreEqual(192, built.width);
            Assert::AreEqual(108, built.height);
            Assert::AreEqual(3840.0 / 1880.0, built.pictureAspect, 1e-9);
            Assert::IsFalse(BuildGrid([](int, int, uint16_t&) { return true; }, 0, 0, 4, 4, built));
        }

        TEST_METHOD(ObserverClassifiesPansByTheMeasuredClassNotTheRate)
        {
            // Sustained pans of one grid cell per frame (0.46 picture heights per second at
            // 50 Hz) on a video-class source become sports at the first cut after the hold; the
            // same pictures at a film rate stay film. 100p video pans are sports too, and 30p
            // on a 30 Hz display under source_class measured: the class decides, not a rate.
            struct Case { double hz, display; SourceClass sourceClass; ContentClass expected; };
            const Case cases[] = {
                { 50.0, 50.0, SourceClass::Legacy, ContentClass::Sports },
                { 100.0, 100.0, SourceClass::Legacy, ContentClass::Sports },
                { 30.0, 30.0, SourceClass::Measured, ContentClass::Sports },
                { 24.0, 60.0, SourceClass::Legacy, ContentClass::Film },
                { 30.0, 60.0, SourceClass::Measured, ContentClass::Film } };
            for (const Case& c : cases)
            {
                const int perSecond = static_cast<int>(c.hz);
                // Pan 1 cell per frame at 50 Hz and 100 Hz alike in picture heights per second:
                // cells per frame = 50 / rate rounded up (100 Hz pans one cell every frame too,
                // which is faster - still sustained large pans).
                const int cells = 1;
                const Canvas first(192 + 40 * perSecond + 10, 108, 3, 100, 400);
                const Canvas second(192 + 40 * perSecond + 10, 108, 4, 550, 900);
                Observer observer;
                observer.Configure(true, 5.0);
                const FrameContext context = Context(c.hz, c.display, c.sourceClass);
                uint64_t sequence = 0;
                int offset = Feed(observer, first, sequence, 0, 15 * perSecond, cells, context);
                ContentIntentStatus status = observer.Status();
                Assert::IsTrue(status.enabled);
                const bool film = c.expected == ContentClass::Film;
                Assert::IsTrue(status.content == (film ? ContentClass::Film : ContentClass::Video),
                    L"the first class (after the hold, before any cut)");
                if (!film)
                {
                    Assert::IsTrue(status.candidate == ContentClass::Sports, L"sports is pending");
                    Assert::IsTrue(status.motionValid && status.panPictureHeightsPerSecond > 0.25f);
                }
                // A cut (another picture), then the pan continues.
                offset = Feed(observer, second, sequence, offset, 2 * perSecond, cells, context);
                status = observer.Status();
                Assert::IsTrue(status.content == c.expected, (std::wstring(L"after the cut at ") +
                    std::to_wstring(c.hz) + L" Hz: " + Name(status.content)).c_str());
                Assert::IsTrue(status.source == IntentSource::Inferred);
                Assert::AreEqual(film ? "film" : "sports", ContentRuleValue(status).c_str());
                Assert::AreEqual(sequence, observer.Descriptor().shotSequence + 2 * perSecond - 1,
                    L"the descriptor's shot starts at the cut");
            }
            // The same 50 Hz video without motion stays video across cuts.
            Observer still;
            still.Configure(true, 5.0);
            const Canvas a(400, 108, 5, 100, 400), b(400, 108, 6, 550, 900);
            uint64_t sequence = 0;
            Feed(still, a, sequence, 0, 400, 0, Context(50.0, 50.0));
            Feed(still, b, sequence, 0, 800, 0, Context(50.0, 50.0));
            // A long still with structure is a menu (static UI), entered without a cut.
            Assert::IsTrue(still.Status().content == ContentClass::StaticUi, Name(still.Status().content));
            Feed(still, a, sequence, 0, 100, 1, Context(50.0, 50.0));
            Assert::IsTrue(still.Status().content == ContentClass::Video, L"motion leaves the menu at once");
        }

        TEST_METHOD(ObserverL11WinsAndSwitchingOffEmptiesTheVariable)
        {
            Observer observer;
            // Off (the default): nothing is measured and the rules see nothing.
            const Canvas canvas(2000, 108, 11, 100, 400);
            uint64_t sequence = 0;
            Feed(observer, canvas, sequence, 0, 400, 1, Context(50.0, 50.0));
            ContentIntentStatus status = observer.Status();
            Assert::IsFalse(status.enabled);
            Assert::AreEqual("", ContentRuleValue(status).c_str());
            Assert::IsFalse(observer.Enabled());
            // On, with L11 "movies" on a panning 50p source: film from metadata.
            observer.Configure(true, 2.0);
            FrameContext context = Context(50.0, 50.0);
            context.l11Present = true;
            context.l11ContentType = 1;
            Feed(observer, canvas, sequence, 0, 150, 1, context);
            status = observer.Status();
            Assert::IsTrue(status.content == ContentClass::Film && status.source == IntentSource::DvL11);
            Assert::AreEqual("film", ContentRuleValue(status).c_str());
            Assert::IsTrue(observer.Descriptor().content.valid &&
                observer.Descriptor().content.source == IntentSource::DvL11);
            // Not classified yet: still empty.
            Observer fresh;
            fresh.Configure(true, 5.0);
            Assert::AreEqual("", ContentRuleValue(fresh.Status()).c_str());
            Assert::AreEqual("waiting for look-ahead frames", fresh.Status().reason);
            // Off again: empty at once, and the class re-forms when switched on.
            observer.Configure(false, 2.0);
            status = observer.Status();
            Assert::IsFalse(status.enabled);
            Assert::AreEqual("", ContentRuleValue(status).c_str());
            ContentIntentStatus enabledButUnknown;
            enabledButUnknown.enabled = true;
            Assert::AreEqual("", ContentRuleValue(enabledButUnknown).c_str());
            ContentIntentStatus offButSet;
            offButSet.content = ContentClass::Sports;
            Assert::AreEqual("", ContentRuleValue(offButSet).c_str(), L"off: empty even with a stale class");
        }

        TEST_METHOD(ContentVariableSelectsOwnerProfiles)
        {
            TempConfig file;
            {
                std::ofstream out(file.path, std::ios::trunc);
                out << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
                    "[vprenderer.everyday]\nquality: high\n"
                    "[vprenderer.match_day]\nwhen: $content==\"sports\"\nsdr_black_nits: 0.05\n"
                    "[vprenderer.cartoons]\nwhen: $content==\"animation\"\nsdr_black_nits: 0.1\n";
            }
            ConfigFile rules;
            Assert::IsTrue(rules.Load(file.path, ConfigFile::ReadPolicy::Fresh), L"load");
            std::string error;
            Assert::IsTrue(MainConfigSchema::Validate(rules, error), (L"schema: " + std::wstring(error.begin(), error.end())).c_str());
            RendererProfileConfig::Model model;
            Assert::IsTrue(RendererProfileConfig::Read(rules, model, error), (L"read: " + std::wstring(error.begin(), error.end())).c_str());
            auto pick = [&](const char* content)
            {
                std::vector<RendererProfileConfig::AutomaticSelection> automatic;
                std::string selectError;
                Assert::IsTrue(RendererProfileConfig::SelectAutomatic(model,
                    [content](const std::string& name, std::string& value)
                    {
                        if (name != "content" || !content) return false;
                        value = content;
                        return true;
                    }, automatic, selectError), std::wstring(selectError.begin(), selectError.end()).c_str());
                Assert::AreEqual(static_cast<size_t>(1), automatic.size());
                return automatic.front().profiles.front();
            };
            Assert::AreEqual("match_day", pick("sports").c_str());
            Assert::AreEqual("cartoons", pick("animation").c_str());
            Assert::AreEqual("everyday", pick("film").c_str());
            Assert::AreEqual("everyday", pick(nullptr).c_str(), L"content off (empty): no content rule matches");
            {
                std::ofstream out(file.path, std::ios::trunc);
                out << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
                    "[vprenderer.everyday]\nquality: high\nintent_content: on\nintent_content_hold_seconds: 4\n";
            }
            ConfigFile keys;
            Assert::IsTrue(keys.Load(file.path, ConfigFile::ReadPolicy::Fresh), L"load keys");
            RendererProfileConfig::Model keyModel;
            error.clear();
            Assert::IsTrue(RendererProfileConfig::Read(keys, keyModel, error), (L"read keys: " + std::wstring(error.begin(), error.end())).c_str());
            // The variable is text, and the keys validate (default off; hold 1-60 s).
            DisplayRuleExpression::ValueType type;
            Assert::IsTrue(DisplayRuleExpression::GetVariableType("content", type) &&
                type == DisplayRuleExpression::ValueType::Text, L"content is a text variable");
            std::string expected;
            Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "intent_content", "on", expected), L"on");
            Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "intent_content", "off", expected), L"off");
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "intent_content", "auto", expected), L"auto");
            Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "intent_content_hold_seconds", "5", expected), L"5");
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "intent_content_hold_seconds", "0.5", expected), L"0.5");
            Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "intent_content_hold_seconds", "61", expected), L"61");
            // The class names are the rules' spellings.
            ContentClass parsed = ContentClass::Unknown;
            Assert::IsTrue(ParseContentClass("static_ui", parsed) && parsed == ContentClass::StaticUi, L"static_ui");
            Assert::IsFalse(ParseContentClass("news", parsed), L"news");
        }
    };
}
