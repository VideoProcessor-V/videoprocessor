#pragma once

// Content recognition producer (intent interpreter item 1; design "Shared
// core": a QueuedFrameStatistics::IObserver in the LedLookahead pattern). It
// sees each queued frame once, before display, in source order: a reduced
// luma grid (about 192 x 108) over the active picture, the global motion from
// projection correlation, holds, flat areas and cuts (ContentClassifier.h),
// and the classifier and its hysteresis on top. CPU only, cheap; nothing runs
// while intent_content is off (the status then says off and the rules'
// `content` variable is empty).
//
// Threads: ObserveFrame / SetFrameContext / Configure on the render loop
// thread; Status() from any thread.

#include <AnalysisLumaSource.h>
#include <ContentClassifier.h>
#include <DebugLog.h>
#include <IntentDescriptor.h>
#include <QueuedFrameStatistics.h>
#include <vprenderer/InterpolationPolicy.h>

#include <algorithm>
#include <cstdint>
#include <mutex>

namespace ContentIntent
{
    // What the renderer knows about the frame the walk is about to show the
    // observer (all measured; nothing fixed).
    struct FrameContext
    {
        double contentHz = 0.0;        // pulldown film rate when locked, else the capture rate
        double displayHz = 0.0;        // the measured display rate (0 = not known yet)
        bool pulldownLocked = false;
        InterpolationPolicy::SourceClass sourceClass = InterpolationPolicy::SourceClass::Legacy;
        bool l11Present = false;
        uint8_t l11ContentType = 0;
        // Frames were lost before this one, or the source marked a discontinuity:
        // no motion pair with the previous frame (a cut for the hysteresis).
        bool discontinuity = false;
        // Hunt 2 (cadence_class_fixes, default on): the source-only film / video
        // class (InterpolationPolicy::IsFilmLike) and the classifier's fixes.
        bool classFixes = true;
    };

    // The existing film / video classification (source_class) at the
    // measured rates. Legacy needs only the content rate; measured needs the
    // display rate too (2+ refreshes per frame), and a pulldown lock is film.
    inline bool FilmLike(const FrameContext& c, bool& rateKnown)
    {
        rateKnown = c.contentHz > 0.0 &&
            (c.sourceClass == InterpolationPolicy::SourceClass::Legacy || c.classFixes || c.pulldownLocked || c.displayHz > 0.0);
        return rateKnown && InterpolationPolicy::IsFilmLike(c.sourceClass, c.contentHz, c.displayHz, c.pulldownLocked,
            c.classFixes);
    }

    // The reduced grid from any sampler (sample(x, y, luma) -> bool), cell
    // centres over the active rectangle [left, right) x [top, bottom).
    template <typename Sampler>
    inline bool BuildGrid(Sampler sample, int left, int top, int right, int bottom, LumaGrid& grid)
    {
        const int pw = right - left, ph = bottom - top;
        if (pw < 8 || ph < 8)
            return false;
        grid.width = (std::min)(kGridMaxWidth, pw);
        grid.height = (std::min)(kGridMaxHeight, ph);
        grid.pictureAspect = static_cast<double>(pw) / ph;
        grid.luma.assign(static_cast<size_t>(grid.width) * grid.height, 0);
        for (int gy = 0; gy < grid.height; ++gy)
        {
            const int y = top + static_cast<int>((2LL * gy + 1) * ph / (2LL * grid.height));
            for (int gx = 0; gx < grid.width; ++gx)
            {
                const int x = left + static_cast<int>((2LL * gx + 1) * pw / (2LL * grid.width));
                uint16_t luma = 0;
                if (!sample(x, y, luma))
                    return false;
                grid.luma[static_cast<size_t>(gy) * grid.width + gx] = luma;
            }
        }
        return true;
    }

    class Observer : public QueuedFrameStatistics::IObserver
    {
    public:
        // intent_content and intent_content_hold_seconds. Switching off
        // forgets everything (the class re-forms when it is switched on).
        void Configure(bool enabled, double holdSeconds)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_hysteresis.SetHoldSeconds(holdSeconds);
            if (enabled == m_enabled)
                return;
            m_enabled = enabled;
            ResetLocked();
            DebugLog::Log("Content intent: content recognition %s (hold %.1f s)",
                enabled ? "on" : "off", holdSeconds);
        }

        bool Enabled() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_enabled;
        }

        void SetFrameContext(const FrameContext& context) { m_context = context; }

        void BeginWalk(size_t) override {}

        // Efficiency round 2026-10-18: ObserveFrame's own early return (off, or seen in an
        // earlier walk), checked without building the grid.
        bool WantsFrame(uint64_t sourceSequence) const override
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_enabled && !(m_haveLast && sourceSequence <= m_lastSequence);
        }

        void ObserveFrame(uint64_t sourceSequence, size_t, const AnalysisLumaSource& luma,
            const QueuedFrameStatistics::PictureRect& picture) override
        {
            {
                std::lock_guard<std::mutex> guard(m_mutex);
                if (!m_enabled || (m_haveLast && sourceSequence <= m_lastSequence))
                    return;   // off, or seen in an earlier walk
            }
            LumaGrid grid;
            const bool built = luma.IsValid() && BuildGrid(
                [&luma](int x, int y, uint16_t& value)
                {
                    AnalysisLumaSample s;
                    if (!luma.Sample(x, y, s)) return false;
                    value = s.luma;
                    return true;
                }, picture.left, picture.top, picture.right, picture.bottom, grid);
            FrameContext context = m_context;
            if (luma.generation != m_generation)
            {
                // A format change: a discontinuity (no motion pair; counts as a cut).
                m_generation = luma.generation;
                context.discontinuity = true;
            }
            ObserveGrid(sourceSequence, built ? grid : LumaGrid(), context);
        }

        // The step itself (tests drive it directly). An invalid grid still
        // advances time and the rate / metadata classification.
        void ObserveGrid(uint64_t sourceSequence, const LumaGrid& grid, const FrameContext& context)
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            if (!m_enabled || (m_haveLast && sourceSequence <= m_lastSequence))
                return;
            // Sequence gaps are normal (pulldown repeats are not queued); only a
            // marked discontinuity or a format change breaks the motion pair.
            const bool contiguous = m_haveLast && !context.discontinuity;
            m_lastSequence = sourceSequence;
            m_haveLast = true;
            FrameMeasure m = Measure(contiguous ? m_previous : LumaGrid(), grid);
            // A discontinuity (a format change, lost frames) is a cut for the hysteresis.
            const bool cut = m.cut || (!contiguous && m_previousValid);
            m_previous = grid;
            m_previousValid = grid.Valid();
            m_statistics.Add(m, context.contentHz);
            ClassifierInput in;
            in.l11Present = context.l11Present;
            in.l11ContentType = context.l11ContentType;
            in.filmLike = FilmLike(context, in.rateKnown);
            in.pulldownLocked = context.pulldownLocked;
            in.stats = m_statistics.Current();
            in.classFixes = context.classFixes;
            in.contentHz = context.contentHz;
            if (CarrierHolds(in))
            {
                // Hunt 2: the pictures' own rate inside the carrier.
                FrameContext pictures = context;
                pictures.contentHz = context.contentHz * in.stats.pictureShare;
                bool known = false;
                in.carrierFilmLike = FilmLike(pictures, known) && known;
            }
            const Classification candidate = Classify(in);
            const ContentClass before = m_hysteresis.Current().content;
            if (m_hysteresis.Update(candidate, m_statistics.Time(), cut))
            {
                const Classification& now = m_hysteresis.Current();
                DebugLog::Log("Content intent: %s -> %s (%s: %s) pan=%.3f picture heights/s wide_pan=%.2f "
                    "hold_share=%.2f flat=%.2f window=%.1f s content=%.3f Hz display=%.3f Hz pulldown=%d L11=%s",
                    before == ContentClass::Unknown ? "none" : ToString(before), ToString(now.content),
                    DisplayName(now.source), now.reason, in.stats.meanPanHps, in.stats.widePanShare,
                    in.stats.holdShare, in.stats.flatShare, in.stats.seconds, context.contentHz, context.displayHz,
                    context.pulldownLocked ? 1 : 0, context.l11Present ? std::to_string(context.l11ContentType).c_str() : "absent");
            }
            m_status.enabled = true;
            m_status.content = m_hysteresis.Current().content;
            m_status.source = m_hysteresis.Current().source;
            m_status.candidate = candidate.content;
            m_status.candidateSource = candidate.source;
            m_status.motionValid = in.stats.motionValid;
            m_status.panPictureHeightsPerSecond = static_cast<float>(in.stats.meanPanHps);
            m_status.widePanShare = static_cast<float>(in.stats.widePanShare);
            m_status.windowSeconds = static_cast<float>(in.stats.seconds);
            SetReason(m_status, candidate.reason);
            m_descriptor.content = IntentField<ContentClass>::Make(m_status.content,
                m_status.content == ContentClass::Unknown ? IntentSource::Off : m_status.source);
            if (cut || m_descriptor.shotSequence == 0)
                m_descriptor.shotSequence = sourceSequence;
        }

        ContentIntentStatus Status() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            ContentIntentStatus status = m_status;
            status.enabled = m_enabled;
            if (m_enabled && !m_haveLast)
                SetReason(status, "waiting for look-ahead frames");
            return status;
        }

        IntentDescriptor Descriptor() const
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            return m_descriptor;
        }

    private:
        void ResetLocked()
        {
            m_statistics.Reset();
            m_hysteresis.Reset();
            m_previous = LumaGrid();
            m_previousValid = false;
            m_haveLast = false;
            m_lastSequence = 0;
            m_status = ContentIntentStatus();
            m_descriptor = IntentDescriptor();
        }

        mutable std::mutex m_mutex;
        bool m_enabled = false;
        FrameContext m_context;              // render thread only
        uint64_t m_generation = 0;           // render thread only
        Statistics m_statistics;
        Hysteresis m_hysteresis;
        LumaGrid m_previous;
        bool m_previousValid = false;
        bool m_haveLast = false;
        uint64_t m_lastSequence = 0;
        ContentIntentStatus m_status;
        IntentDescriptor m_descriptor;
    };
}
