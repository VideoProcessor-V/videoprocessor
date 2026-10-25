#pragma once

// The shot-level intent observer (intent interpreter items 2-5, 2026-10-17;
// design "Shared core": a QueuedFrameStatistics::IObserver in the LedLookahead
// pattern). It sees each queued frame once, before display, in source order:
// the reduced luma grid over the active picture (ContentIntent::BuildGrid /
// Measure, bars excluded by the caller's rectangle), and from it
// - shot boundaries: cuts, format changes (a discontinuity alone breaks the
//   motion pair but is not a cut), fades and near-black frames;
// - per-shot motion statistics for motion intent (MotionIntent.h), with the
//   de-judder's valid vectors replacing the CPU projection measurement for a
//   pair when they arrive (NoteVectors);
// - the shadow luma percentiles BlackIntent needs (ShadowStatsFromCodes on
//   the active picture's grid codes).
// The display side (AdvanceDisplay, called with the source frame about to be
// rendered) crosses the shot boundaries when the picture reaches them: motion
// intent decides there, once per shot, and ambient adaptation may step there.
//
// Runs when ANY of intent_content / intent_motion / ambient_adapt /
// intent_black / intent_display is on; idle (nothing computed) when all are
// off. CPU only, about the content observer's cost.
//
// Threads: ObserveFrame / NoteVectors / AdvanceDisplay on the render loop
// thread; Configure / status from any thread (one mutex).

#include <AnalysisLumaSource.h>
#include <BlackIntent.h>
#include <ContentClassifier.h>
#include <ContentIntentObserver.h>
#include <DebugLog.h>
#include <IntentDescriptor.h>
#include <MotionIntent.h>
#include <QueuedFrameStatistics.h>
#include <VP-V/modules/motion/consumers/InterpolationPolicy.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>

namespace IntentInterpreter
{
    // Seconds between shadow measurements within a shot (the first frame of
    // every shot is always measured). Documented constant, not a key.
    constexpr double kShadowIntervalSeconds = 0.5;
    // Shot records kept ahead of the display (look-ahead depth is small).
    constexpr size_t kMaxShots = 32;
    // Queue-fed observer (owner 2026-10-17): the most frames ahead the render loop
    // walks for the intent observer (the queue ahead, bounded; documented, not a key).
    constexpr size_t kMaxWalkFrames = 32;

    // The walk depth the intent observer wants with `queued` frames in the queue
    // (current included): the whole queue ahead, bounded by kMaxWalkFrames, and at
    // least 1 so the walk runs with the current frame alone (low latency: a cut is
    // then decided as it arrives, under the missed-cut rule).
    inline size_t WantedWalkFrames(size_t queued)
    {
        const size_t ahead = queued > 1 ? queued - 1 : 0;
        return (std::max)(size_t{1}, (std::min)(ahead, kMaxWalkFrames));
    }

    struct Switches
    {
        bool content = false;   // intent_content
        bool motion = false;    // intent_motion
        bool ambient = false;   // ambient_adapt
        bool black = false;     // intent_black != off
        bool display = false;   // intent_display != off
        bool Any() const { return content || motion || ambient || black || display; }
    };

    struct MotionSettings
    {
        MotionIntentPolicy policy = MotionIntentPolicy::Display;
        double minScale = 0.5;           // intent_motion_min
        double minShotSeconds = 2.0;     // intent_motion_min_shot_s
        bool rolloffOn = false;          // dejudder_motion_rolloff
        double easeSeconds = 1.5;        // intent_ease_s (the fade model; 0 = instant at the cut)
        bool operator==(const MotionSettings& o) const
        {
            return policy == o.policy && minScale == o.minScale && minShotSeconds == o.minShotSeconds &&
                rolloffOn == o.rolloffOn && easeSeconds == o.easeSeconds;
        }
    };

    // What the renderer knows about the frame it hands the observer (measured).
    struct FrameContext
    {
        double contentHz = 0.0;          // pulldown film rate when locked, else the capture rate
        double displayHz = 0.0;          // measured display rate (0 = not known yet)
        bool pulldownLocked = false;
        InterpolationPolicy::SourceClass sourceClass = InterpolationPolicy::SourceClass::Legacy;
        bool l11Present = false;
        uint8_t l11FrameRateConversion = 0;
        // Frames were lost before this one or the source marked a discontinuity:
        // no motion pair with the previous frame (not a cut by itself).
        bool discontinuity = false;
        // general_rate_fixes (integration round 4): the film / video split the
        // motion intent's shots use follows the renderer's switch.
        bool generalRates = true;
        // For the shadow percentiles (BlackIntent::ShadowStatsFromCodes).
        BlackIntent::Transfer transfer = BlackIntent::Transfer::Unknown;
        double gamma = 2.4;              // BT.1886 for gamma sources
        double sdrWhiteNits = 100.0;
        double displayBlackNits = 0.0;
    };

    // The de-judder's motion for pair (from, to), read where its estimate takes
    // the pair: valid only when the renderer's vectors-valid state
    // (MotionValidity::PairValid, the current epoch) accepted the pair and the
    // whole-frame motion was clear. heightsPerFrame: |global vector| in
    // picture heights per source frame.
    struct VectorSample
    {
        uint64_t from = 0;
        uint64_t to = 0;
        double heightsPerFrame = 0.0;
        bool valid = false;
    };

    class Observer : public QueuedFrameStatistics::IObserver
    {
    public:
        void Configure(const Switches& switches, const MotionSettings& motion)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            const bool wasActive = m_switches.Any();
            const bool motionChanged = switches.motion != m_switches.motion || !(motion == m_motion);
            const bool shadowsTurnedOn = switches.black && !m_switches.black;
            m_switches = switches;
            m_motion = motion;
            if (wasActive != switches.Any())
            {
                ResetLocked();
                DebugLog::Log("Intent interpreter: shot observer %s (content=%d motion=%d ambient=%d black=%d display=%d)",
                    switches.Any() ? "on" : "off", switches.content ? 1 : 0, switches.motion ? 1 : 0,
                    switches.ambient ? 1 : 0, switches.black ? 1 : 0, switches.display ? 1 : 0);
                return;
            }
            if (motionChanged)
            {
                // An owner change: back to the owner's strength at once; the next
                // cut decides under the new settings.
                m_applier.Reset();
                m_lastCut = MotionIntent::CutResult();
                DebugLog::Log("Motion intent: %s, policy %s, min %.2f, min shot %.1f s, rolloff %s; scale 1 until the next cut",
                    switches.motion ? "on" : "off", ToString(motion.policy), motion.minScale, motion.minShotSeconds,
                    motion.rolloffOn ? "on" : "off");
            }
            if (shadowsTurnedOn)
                for (Shot& shot : m_shots) shot.shadowSeconds = -1.0;
        }

        // The newest source frame observed (0 = none yet): the walk skips frames at or
        // below it before decoding them.
        uint64_t LastObservedSequence() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_haveLast ? m_lastSequence : 0;
        }

        bool Active() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_switches.Any();
        }

        void SetFrameContext(const FrameContext& context)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_context = context;
        }

        void BeginWalk(size_t) override {}

        void ObserveFrame(uint64_t sourceSequence, size_t, const AnalysisLumaSource& luma,
            const QueuedFrameStatistics::PictureRect& picture) override
        {
            FrameContext context;
            {
                std::lock_guard<std::mutex> guard(m_mutex);
                if (!m_switches.Any() || (m_haveLast && sourceSequence <= m_lastSequence))
                    return;   // idle, or seen in an earlier walk
                context = m_context;
            }
            ContentIntent::LumaGrid grid;
            const bool built = luma.IsValid() && ContentIntent::BuildGrid(
                [&luma](int x, int y, uint16_t& value)
                {
                    return luma.SampleLuma(x, y, value);   // WP-23: the shared decode when ready
                }, picture.left, picture.top, picture.right, picture.bottom, grid);
            bool formatChange = false;
            if (luma.generation != m_generation)
            {
                formatChange = m_generationSeen;
                m_generation = luma.generation;
                m_generationSeen = true;
            }
            ObserveGrid(sourceSequence, built ? grid : ContentIntent::LumaGrid(), context, formatChange);
        }

        // The step itself (tests drive it directly). formatChange: a new source
        // format (always a shot boundary).
        void ObserveGrid(uint64_t sourceSequence, const ContentIntent::LumaGrid& grid, const FrameContext& context,
            bool formatChange = false)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            if (!m_switches.Any() || (m_haveLast && sourceSequence <= m_lastSequence))
                return;
            // Bug hunt 3 F6: a frame the walk skipped (an analysis that failed, a conversion
            // failure, a DV gap) leaves a hole in the sequence: the next frame is not measured
            // against the one before the hole as if one frame apart (twice the pan speed, a
            // cut judged across two frames). Repeats carry their frame's sequence (never here).
            const bool contiguous = m_haveLast && sourceSequence == m_lastSequence + 1 &&
                !context.discontinuity && !formatChange;
            const ContentIntent::FrameMeasure m = ContentIntent::Measure(
                contiguous ? m_previous : ContentIntent::LumaGrid(), grid);
            const bool cut = m.cut || formatChange || (m_shots.empty());
            // Fades and near black (MotionIntent constants).
            double mean = 0.0;
            if (grid.Valid())
            {
                double sum = 0.0;
                for (const uint16_t v : grid.luma) sum += v;
                mean = sum / grid.luma.size();
            }
            // Bug hunt 3 F9: the fade thresholds in time (codes per second, run and window in
            // seconds) at this frame's content rate.
            const int fadeWindowFrames = MotionIntent::FadeWindowFrames(context.contentHz);
            const bool fading = m_fade.Step(m.valid && !m.cut && m_previousValid, mean - m_previousMean,
                context.contentHz);
            const bool nearBlack = grid.Valid() && mean <= MotionIntent::kNearBlackCodes;
            const bool fadeFlag = fading || nearBlack;
            if (cut)
            {
                if (!m_shots.empty())
                {
                    Shot& last = m_shots.back();
                    last.ended = true;
                    last.fadeAtEnd = m_framesSinceFade <= fadeWindowFrames;
                }
                Shot shot;
                shot.start = sourceSequence;
                m_shots.push_back(shot);
                while (m_shots.size() > kMaxShots)
                    m_shots.pop_front();
            }
            m_framesSinceFade = fadeFlag ? 0 : (m_framesSinceFade < 1000000 ? m_framesSinceFade + 1 : m_framesSinceFade);
            Shot& shot = m_shots.back();
            ++shot.frames;
            if (fadeFlag && shot.frames <= fadeWindowFrames)
                shot.fadeAtStart = true;
            const double hz = context.contentHz;
            const double dt = hz > 0.0 ? 1.0 / hz : 0.0;
            shot.acc.AddFrame(dt, grid.Valid(), MotionIntent::EdgeTerm(m.edgeCodes));
            if (m.valid && !m.cut && hz > 0.0)
            {
                const double hps = m.held ? 0.0 : std::hypot(m.dxHeights, m.dyHeights) * hz;
                shot.acc.AddPair(sourceSequence, m_lastSequence, hz, hps);
            }
            bool rateKnown = false;
            ContentIntent::FrameContext classify;
            classify.contentHz = context.contentHz;
            classify.displayHz = context.displayHz;
            classify.pulldownLocked = context.pulldownLocked;
            classify.sourceClass = context.sourceClass;
            classify.generalRates = context.generalRates;
            shot.filmLike = ContentIntent::FilmLike(classify, rateKnown);
            shot.rateKnown = rateKnown;
            shot.l11Present = context.l11Present;
            shot.l11FrameRateConversion = context.l11FrameRateConversion;
            if (hz > 0.0) shot.contentHz = hz;
            // Shadows (only while black protection is on).
            if (m_switches.black && grid.Valid() &&
                (shot.shadowSeconds < 0.0 || shot.acc.Seconds() - shot.shadowSeconds >= kShadowIntervalSeconds - 1e-9))
            {
                shot.shadows = BlackIntent::ShadowStatsFromCodes(grid.luma.data(), grid.luma.size(), 10, true,
                    context.transfer, context.gamma, context.sdrWhiteNits, context.displayBlackNits);
                shot.shadowSeconds = shot.acc.Seconds();
                shot.shadowsMeasured = true;
            }
            m_previous = grid;
            m_previousValid = grid.Valid();
            m_previousMean = mean;
            m_lastSequence = sourceSequence;
            m_haveLast = true;
            ++m_framesObserved;
            // The candidate for the shot being measured (status only).
            m_candidateStats = shot.acc.Stats();
            m_candidate = MotionIntent::Evaluate(m_candidateStats, PolicyInputLocked(shot));
        }

        // The de-judder's motion for one pair (see VectorSample). Invalid pairs
        // (a resync, a drop, a repeat, an unclear whole-frame motion) are
        // ignored: the CPU measurement stays.
        bool NoteVectors(const VectorSample& v)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            if (!m_switches.Any() || !v.valid || v.to <= v.from)
                return false;
            for (auto it = m_shots.rbegin(); it != m_shots.rend(); ++it)
                if (v.to >= it->start)
                {
                    const bool used = v.from >= it->start && it->acc.ReplaceWithVectors(v.from, v.to, v.heightsPerFrame);
                    if (used && &*it == &m_shots.back())
                    {
                        m_candidateStats = it->acc.Stats();
                        m_candidate = MotionIntent::Evaluate(m_candidateStats, PolicyInputLocked(*it));
                    }
                    return used;
                }
            return false;
        }

        // The source frame about to be rendered. Crossing into a new shot is the
        // cut: motion intent decides there (once per shot). Idempotent per frame.
        void AdvanceDisplay(uint64_t sourceSequence)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            AdvanceLocked(sourceSequence);
        }

        // The multiplier on the de-judder strength for this frame (1 while
        // intent_motion is off or nothing was decided).
        double AppliedMotionScale(uint64_t sourceSequence)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            AdvanceLocked(sourceSequence);
            return m_switches.motion ? m_applier.Applied() : 1.0;
        }

        // Cuts the display has crossed so far (consumers compare counts).
        uint64_t DisplayedCuts() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_displayCuts;
        }

        uint64_t DisplayedShotStart() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_displayShotStart;
        }

        // The shadow percentiles of the shot on screen (invalid when none).
        BlackIntent::ShadowStats DisplayedShadows() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            for (const Shot& shot : m_shots)
                if (shot.start == m_displayShotStart && shot.shadowsMeasured)
                    return shot.shadows;
            return BlackIntent::ShadowStats();
        }

        MotionIntentStatus MotionStatus() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            MotionIntentStatus s;
            s.enabled = m_switches.motion;
            s.policy = m_motion.policy;
            if (!m_switches.motion)
                return s;
            s.shotValid = m_lastCutStats.valid;
            s.shotSequence = m_displayShotStart;
            s.appliedScale = static_cast<float>(m_applier.Applied());
            s.source = m_applier.Source();
            s.candidateScale = static_cast<float>(m_candidate.candidate);
            s.inferredScale = static_cast<float>(m_candidate.inferred);
            s.panPictureHeightsPerSecond = static_cast<float>(m_candidateStats.meanHps);
            s.panSpread = static_cast<float>(m_candidateStats.spreadHps);
            s.edgeContrast = static_cast<float>(m_candidateStats.edgeTerm);
            s.visibility = static_cast<float>(m_candidateStats.visibility);
            s.shotSeconds = static_cast<float>(m_candidateStats.seconds);
            s.vectorsUsed = m_candidateStats.vectorPairs > 0;
            if (!m_haveLast)
                CopyIntentText(s.reason, "waiting for look-ahead frames");
            else if (m_lastCut.reason && m_lastCut.reason[0])
                CopyIntentText(s.reason, m_lastCut.reason);
            else
                CopyIntentText(s.reason, m_candidate.reason);
            return s;
        }

        // ---- the per-shot descriptor (precedence Dv2 > DvL11 / DvL1 > Inferred > Off) ----
        IntentDescriptor Descriptor() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_descriptor;
        }

        void SetContentField(const IntentField<ContentClass>& f) { Set(&IntentDescriptor::content, f); }
        void SetAmbientField(const IntentField<float>& f) { Set(&IntentDescriptor::ambientBlackNits, f); }
        void SetBlackField(const IntentField<float>& f) { Set(&IntentDescriptor::shadowStepsBelowBlack, f); }
        void SetDisplayField(const IntentField<float>& f) { Set(&IntentDescriptor::displayCompressionStops, f); }

        uint64_t FramesObserved() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_framesObserved;
        }

        size_t ShotsKnown() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_shots.size();
        }

    private:
        struct Shot
        {
            uint64_t start = 0;
            MotionIntent::ShotAccumulator acc;
            int frames = 0;
            bool ended = false;
            bool fadeAtStart = false;
            bool fadeAtEnd = false;
            bool rateKnown = false;
            bool filmLike = false;
            bool l11Present = false;
            uint8_t l11FrameRateConversion = 0;
            double contentHz = 0.0;          // the newest measured content rate (the ease's frame period)
            BlackIntent::ShadowStats shadows;
            bool shadowsMeasured = false;
            double shadowSeconds = -1.0;
        };

        template <typename T>
        void Set(IntentField<T> IntentDescriptor::* member, const IntentField<T>& f)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_descriptor.*member = f;
        }

        MotionIntent::PolicyInput PolicyInputLocked(const Shot& shot) const
        {
            MotionIntent::PolicyInput in;
            in.policy = m_motion.policy;
            in.minScale = m_motion.minScale;
            in.rolloffOn = m_motion.rolloffOn;
            in.rateKnown = shot.rateKnown;
            in.filmLike = shot.filmLike;
            in.l11Present = shot.l11Present;
            in.l11FrameRateConversion = shot.l11FrameRateConversion;
            return in;
        }

        void AdvanceLocked(uint64_t sequence)
        {
            if (!m_switches.Any() || m_shots.empty())
                return;
            if (m_displayValid && sequence < m_displaySequence)
                return;
            const bool newFrame = !m_displayValid || sequence > m_displaySequence;
            m_displaySequence = sequence;
            m_displayValid = true;
            size_t index = m_shots.size();
            for (size_t i = m_shots.size(); i-- > 0;)
                if (m_shots[i].start <= sequence) { index = i; break; }
            if (index == m_shots.size())
                return;   // the walk has not seen this frame's shot
            if (!m_displayCrossed || m_shots[index].start != m_displayShotStart)
            {
                m_displayCrossed = true;
                m_displayShotStart = m_shots[index].start;
                ++m_displayCuts;
                if (m_switches.motion)
                    DecideLocked(m_shots[index], index > 0 ? &m_shots[index - 1] : nullptr);
                m_descriptor.shotSequence = m_displayShotStart;
            }
            else if (newFrame && m_switches.motion && m_applier.Easing())
                EaseLocked(m_shots[index]);
            if (index > 0)
                m_shots.erase(m_shots.begin(), m_shots.begin() + static_cast<std::ptrdiff_t>(index));
        }

        void DecideLocked(const Shot& shot, const Shot* previous)
        {
            const MotionIntent::ShotStats stats = shot.acc.Stats();
            const MotionIntent::Decision d = MotionIntent::Evaluate(stats, PolicyInputLocked(shot));
            MotionIntent::CutInput in;
            in.decisionReady = stats.valid;
            in.fadeBoundary = shot.fadeAtStart || (previous && previous->fadeAtEnd);
            in.shotEndKnown = shot.ended;
            in.shotSeconds = shot.acc.Seconds();
            in.minShotSeconds = m_motion.minShotSeconds;
            in.minScale = m_motion.minScale;
            m_lastCut = m_applier.OnCutEased(d, in, stats, m_motion.easeSeconds);
            m_lastCutStats = stats;
            m_descriptor.motionScale = IntentField<float>::Make(static_cast<float>(m_applier.Applied()),
                m_applier.Source());
            DebugLog::Log("Motion intent: shot %llu: %s", static_cast<unsigned long long>(shot.start),
                MotionIntent::LogLine(m_motion.policy, d, stats, m_lastCut).c_str());
        }

        // The fade model: one newly displayed frame of the shot being eased; the
        // estimate from every frame of the shot seen so far, the value
        // rate-limited toward it (MotionIntent::Applier::Progress).
        void EaseLocked(const Shot& shot)
        {
            const MotionIntent::ShotStats stats = shot.acc.Stats();
            const MotionIntent::Decision d = MotionIntent::Evaluate(stats, PolicyInputLocked(shot));
            const double dt = shot.contentHz > 0.0 ? 1.0 / shot.contentHz : 0.0;
            const bool shortShot = shot.ended && shot.acc.Seconds() < m_motion.minShotSeconds;
            const bool moved = m_applier.Progress(d, stats, dt, m_motion.minScale, shortShot);
            m_lastCutStats = stats;
            if (moved)
                m_descriptor.motionScale = IntentField<float>::Make(static_cast<float>(m_applier.Applied()),
                    m_applier.Source());
            if (!m_applier.Easing())
            {
                m_lastCut.applied = m_applier.Applied();
                m_lastCut.source = m_applier.Source();
                m_lastCut.reason = shortShot ? "short shot: ease stopped, holds" : "settled: holds for the shot";
                DebugLog::Log("Motion intent: shot %llu: settled at %.2f (estimate %.2f from %.2f s, %s)",
                    static_cast<unsigned long long>(shot.start), m_applier.Applied(), d.candidate, stats.seconds,
                    m_lastCut.reason);
            }
        }

        void ResetLocked()
        {
            m_shots.clear();
            m_previous = ContentIntent::LumaGrid();
            m_previousValid = false;
            m_previousMean = 0.0;
            m_fade.Reset();
            m_framesSinceFade = 1000000;
            m_haveLast = false;
            m_lastSequence = 0;
            m_displayValid = false;
            m_displayCrossed = false;
            m_displaySequence = 0;
            m_displayShotStart = 0;
            m_applier.Reset();
            m_lastCut = MotionIntent::CutResult();
            m_lastCutStats = MotionIntent::ShotStats();
            m_candidate = MotionIntent::Decision();
            m_candidateStats = MotionIntent::ShotStats();
            m_descriptor = IntentDescriptor();
        }

        mutable std::mutex m_mutex;
        Switches m_switches;
        MotionSettings m_motion;
        FrameContext m_context;
        uint64_t m_generation = 0;            // render thread only
        bool m_generationSeen = false;        // render thread only
        std::deque<Shot> m_shots;
        ContentIntent::LumaGrid m_previous;
        bool m_previousValid = false;
        double m_previousMean = 0.0;
        MotionIntent::FadeRun m_fade;
        int m_framesSinceFade = 1000000;
        bool m_haveLast = false;
        uint64_t m_lastSequence = 0;
        uint64_t m_framesObserved = 0;
        bool m_displayValid = false;
        bool m_displayCrossed = false;
        uint64_t m_displaySequence = 0;
        uint64_t m_displayShotStart = 0;
        uint64_t m_displayCuts = 0;
        MotionIntent::Applier m_applier;
        MotionIntent::CutResult m_lastCut;
        MotionIntent::ShotStats m_lastCutStats;
        MotionIntent::Decision m_candidate;
        MotionIntent::ShotStats m_candidateStats;
        IntentDescriptor m_descriptor;
    };
}
