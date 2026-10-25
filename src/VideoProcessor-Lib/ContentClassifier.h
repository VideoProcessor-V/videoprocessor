#pragma once

// Content recognition (intent interpreter item 1, docs/VP-intent-interpreter-
// design.md section 5; owner 2026-10-17). Rule-based only: no AI or neural
// methods, nothing fitted. Every threshold below is a fixed, documented,
// owner-editable constant; no frame rate, refresh rate or resolution is fixed
// (owner [[vp-general-not-fixed]]): speeds are picture heights per second at
// the measured content rate, times are seconds at that rate, film / video
// comes from the existing classifier (source_class, InterpolationPolicy).
//
// The class only SELECTS owner-written profiles through the `content`
// variable of when: rules; it never sets a value.
//
// Pure: no renderer, GPU or capture types (the observer feeds it).

#include <IntentDescriptor.h>
#include <VP-V/modules/input/queue_timing/PulldownDetector.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <vector>

namespace ContentIntent
{
    // ---- measurement constants (documented, owner-editable, not fitted) ----
    // The reduced luma grid (design: about 192 x 108), sampled over the
    // active picture.
    constexpr int kGridMaxWidth = 192;
    constexpr int kGridMaxHeight = 108;
    // Projection search: at most this share of the grid per frame each way.
    constexpr double kSearchShare = 0.125;
    // Mean absolute difference (10-bit limited codes) below which a frame is
    // a hold of the previous picture (a repeat, a twos-hold, a still).
    constexpr double kHeldCodes = 1.5;
    // A cut: the 16-bin luma histogram moves by at least this L1 distance
    // (0..2) AND the mean difference is at least kCutDiffCodes. A pan keeps
    // the histogram; a flash keeps the structure for one frame only.
    constexpr double kCutHistogramDistance = 0.5;
    constexpr double kCutDiffCodes = 40.0;
    // Gradient (codes per grid step) below which a grid cell is flat.
    constexpr double kFlatGradientCodes = 2.0;
    // Mean gradient above which a still picture has visible structure (a
    // menu, a paused picture) rather than black.
    constexpr double kStaticEdgeCodes = 6.0;
    // Motion below this (picture heights per second) counts as zero motion.
    constexpr double kZeroMotionHps = 0.01;

    // ---- class rules (documented, owner-editable, not fitted) ----
    // Seconds of statistics the classifier looks back over.
    constexpr double kWindowSeconds = 10.0;
    // Sports: a measured video-class source with sustained large pans - a
    // mean pan speed of at least kSportsMeanPanHps over at least
    // kSportsMinSeconds, and at least kSportsWidePanShare of its moving frames
    // panning at kWidePanHps or more.
    constexpr double kWidePanHps = 0.25;
    constexpr double kSportsMeanPanHps = 0.12;
    constexpr double kSportsWidePanShare = 0.30;
    constexpr double kSportsMinSeconds = 6.0;
    // Animation: at least this share of new pictures held for one or more
    // frames (twos / threes), with at least kAnimationFlatShare flat area,
    // and the holds not a perfectly even cadence (a carrier's repeats with
    // pulldown detection off are even; drawn animation mixes ones, twos and
    // threes).
    constexpr double kAnimationHoldShare = 0.5;
    constexpr double kAnimationFlatShare = 0.35;
    constexpr double kAnimationMinSeconds = 4.0;
    // Static UI: zero motion with static edges for at least this long.
    constexpr double kStaticUiSeconds = 8.0;

    // Hunt 2 (cadence_class_fixes, 02-dv M2/M3), on by default:
    // - Carrier repeats are judged by the rates, not by run evenness (3:2 runs
    //   2,1 are not even; native animation on twos at 24p is): holds are a
    //   carrier's when the capture rate is video-class (a faster carrier), no
    //   pulldown lock gives the film rate, and every new picture is held for
    //   the same number of frames or one more (pulldown's own spread, as
    //   Pulldown::IsEvenPattern) - then the pictures' own rate (capture rate x
    //   new pictures per frame) decides film / video, and it is never animation.
    // - At a film-like rate (or under a lock) animation needs positive evidence:
    //   the new pictures change by at least kAnimationChangeCodes and by at least
    //   kAnimationChangeContrast times the held frames' difference (drawn holds
    //   are near-exact; a dark or locked-off film shot sits near kHeldCodes in
    //   both, its "holds" are noise).
    // - At a film-like rate static UI needs kStaticUiFilmFactor times the still
    //   (a long locked-off shot is not a menu).
    constexpr double kAnimationChangeCodes = 4.0 * kHeldCodes;
    constexpr double kAnimationChangeContrast = 4.0;
    constexpr double kStaticUiFilmFactor = 3.0;

    // The reduced luma grid of one frame (10-bit limited codes).
    struct LumaGrid
    {
        int width = 0;
        int height = 0;
        // Active picture width / height (pixel aspect 1): converts a
        // horizontal shift in grid cells to picture heights.
        double pictureAspect = 16.0 / 9.0;
        std::vector<uint16_t> luma;
        bool Valid() const
        {
            return width >= 8 && height >= 8 &&
                luma.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
        }
    };

    // What one frame shows against the previous one.
    struct FrameMeasure
    {
        bool valid = false;        // a previous grid of the same shape existed
        bool cut = false;
        bool held = false;         // a hold of the previous picture
        double dxHeights = 0.0;    // global shift, picture heights per frame (+ = right)
        double dyHeights = 0.0;    // (+ = down)
        double meanDiffCodes = 0.0;
        double histogramDistance = 0.0;
        double flatShare = 0.0;    // share of flat cells in the current grid
        double edgeCodes = 0.0;    // mean gradient of the current grid
    };

    // Best 1D shift s (cur[i] ~ prev[i - s]) by mean absolute difference over
    // the overlap, refined to a sub-cell value by a parabola through the
    // neighbouring costs.
    inline double BestShift(const std::vector<double>& prev, const std::vector<double>& cur, int range)
    {
        const int n = static_cast<int>(cur.size());
        if (n < 4 || static_cast<int>(prev.size()) != n)
            return 0.0;
        range = (std::max)(0, (std::min)(range, n / 2));
        std::vector<double> cost(static_cast<size_t>(2 * range + 1), 0.0);
        int best = 0;
        double bestCost = 1e300;
        for (int s = -range; s <= range; ++s)
        {
            double sum = 0.0;
            int count = 0;
            for (int i = (std::max)(0, s); i < (std::min)(n, n + s); ++i)
            {
                sum += std::abs(cur[static_cast<size_t>(i)] - prev[static_cast<size_t>(i - s)]);
                ++count;
            }
            const double c = count > 0 ? sum / count : 1e300;
            cost[static_cast<size_t>(s + range)] = c;
            // Ties prefer the smaller shift (a flat picture has no motion).
            if (c < bestCost - 1e-9 || (std::abs(c - bestCost) <= 1e-9 && std::abs(s) < std::abs(best)))
            {
                bestCost = c;
                best = s;
            }
        }
        double shift = best;
        if (best > -range && best < range)
        {
            const double l = cost[static_cast<size_t>(best - 1 + range)];
            const double c = cost[static_cast<size_t>(best + range)];
            const double r = cost[static_cast<size_t>(best + 1 + range)];
            const double denominator = l - 2.0 * c + r;
            if (denominator > 1e-12)
                shift += (std::max)(-0.5, (std::min)(0.5, 0.5 * (l - r) / denominator));
        }
        return shift;
    }

    inline void Histogram16(const LumaGrid& grid, double out[16])
    {
        for (int i = 0; i < 16; ++i) out[i] = 0.0;
        if (grid.luma.empty()) return;
        for (const uint16_t v : grid.luma)
        {
            // 10-bit limited codes 64..940 over 16 bins (BTB / WTW in the end bins).
            const int bin = (std::max)(0, (std::min)(15, (static_cast<int>(v) - 64) * 16 / 877));
            out[bin] += 1.0;
        }
        for (int i = 0; i < 16; ++i) out[i] /= static_cast<double>(grid.luma.size());
    }

    // Spatial statistics of one grid (no previous frame needed).
    inline void Spatial(const LumaGrid& grid, double& flatShare, double& edgeCodes)
    {
        flatShare = 0.0;
        edgeCodes = 0.0;
        if (!grid.Valid()) return;
        size_t flat = 0, cells = 0;
        double sum = 0.0;
        for (int y = 0; y + 1 < grid.height; ++y)
            for (int x = 0; x + 1 < grid.width; ++x)
            {
                const int v = grid.luma[static_cast<size_t>(y) * grid.width + x];
                const int gx = std::abs(grid.luma[static_cast<size_t>(y) * grid.width + x + 1] - v);
                const int gy = std::abs(grid.luma[static_cast<size_t>(y + 1) * grid.width + x] - v);
                const double g = 0.5 * (gx + gy);
                sum += g;
                flat += g < kFlatGradientCodes ? 1 : 0;
                ++cells;
            }
        if (cells)
        {
            flatShare = static_cast<double>(flat) / cells;
            edgeCodes = sum / cells;
        }
    }

    inline FrameMeasure Measure(const LumaGrid& previous, const LumaGrid& current)
    {
        FrameMeasure m;
        Spatial(current, m.flatShare, m.edgeCodes);
        if (!previous.Valid() || !current.Valid() ||
            previous.width != current.width || previous.height != current.height)
            return m;
        m.valid = true;
        const int w = current.width, h = current.height;
        std::vector<double> colPrev(static_cast<size_t>(w), 0.0), colCur(static_cast<size_t>(w), 0.0);
        std::vector<double> rowPrev(static_cast<size_t>(h), 0.0), rowCur(static_cast<size_t>(h), 0.0);
        double diff = 0.0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t i = static_cast<size_t>(y) * w + x;
                const double a = previous.luma[i], b = current.luma[i];
                colPrev[static_cast<size_t>(x)] += a; colCur[static_cast<size_t>(x)] += b;
                rowPrev[static_cast<size_t>(y)] += a; rowCur[static_cast<size_t>(y)] += b;
                diff += std::abs(b - a);
            }
        for (auto* v : { &colPrev, &colCur })
            for (double& d : *v) d /= h;
        for (auto* v : { &rowPrev, &rowCur })
            for (double& d : *v) d /= w;
        m.meanDiffCodes = diff / (static_cast<double>(w) * h);
        m.held = m.meanDiffCodes < kHeldCodes;
        double hp[16], hc[16];
        Histogram16(previous, hp);
        Histogram16(current, hc);
        for (int i = 0; i < 16; ++i) m.histogramDistance += std::abs(hp[i] - hc[i]);
        m.cut = m.histogramDistance >= kCutHistogramDistance && m.meanDiffCodes >= kCutDiffCodes;
        if (!m.held && !m.cut)
        {
            const double sx = BestShift(colPrev, colCur, static_cast<int>(std::ceil(w * kSearchShare)));
            const double sy = BestShift(rowPrev, rowCur, static_cast<int>(std::ceil(h * kSearchShare)));
            // Grid cells -> picture widths (sx / w) -> heights (x aspect).
            m.dxHeights = sx / w * current.pictureAspect;
            m.dyHeights = sy / h;
        }
        return m;
    }

    // ---- the rolling statistics over the last kWindowSeconds ----
    struct WindowStats
    {
        bool motionValid = false;    // enough measured frames behind the numbers
        double seconds = 0.0;        // seconds the window covers
        double meanPanHps = 0.0;     // mean pan speed over moving (non-held) frames
        double widePanShare = 0.0;   // share of moving frames at kWidePanHps or more
        double holdShare = 0.0;      // share of new pictures held for one frame or more
        bool evenHolds = false;      // every new picture held the same number of frames
        // Hunt 2 (cadence_class_fixes): every new picture held one or more frames,
        // the runs differing by at most one (a carrier's pulldown spread).
        bool uniformHolds = false;
        double pictureShare = 1.0;   // new pictures per frame over the counted runs
        double newDiffCodes = 0.0;   // mean difference of the new pictures (valid, non-held)
        double heldDiffCodes = 0.0;  // mean difference of the held frames
        double flatShare = 0.0;      // mean flat share
        double stillSeconds = 0.0;   // current run of zero motion with static edges
    };

    class Statistics
    {
    public:
        void Reset()
        {
            m_frames.clear();
            m_stillSeconds = 0.0;
            m_time = 0.0;
        }

        // One new frame at the measured content rate (Hz > 0). The window
        // rolls across cuts (a broadcast stays sports from shot to shot); a
        // cut frame carries no motion, and it ends a still run.
        void Add(const FrameMeasure& m, double contentHz)
        {
            if (!(contentHz > 0.0))
                return;
            const double dt = 1.0 / contentHz;
            m_time += dt;
            Frame f;
            f.time = m_time;
            f.valid = m.valid && !m.cut;
            f.held = m.held;
            f.speedHps = std::hypot(m.dxHeights, m.dyHeights) * contentHz;
            f.flatShare = m.flatShare;
            f.diffCodes = m.meanDiffCodes;
            m_frames.push_back(f);
            while (!m_frames.empty() && m_frames.front().time <= m_time - kWindowSeconds)
                m_frames.pop_front();
            const bool still = m.valid && !m.cut && (m.held || f.speedHps < kZeroMotionHps) &&
                m.meanDiffCodes < 2.0 * kHeldCodes && m.edgeCodes >= kStaticEdgeCodes;
            m_stillSeconds = still ? m_stillSeconds + dt : 0.0;
        }

        double Time() const { return m_time; }

        WindowStats Current() const
        {
            WindowStats s;
            s.stillSeconds = m_stillSeconds;
            if (m_frames.empty())
                return s;
            s.seconds = m_frames.back().time - m_frames.front().time + (m_frames.size() > 1 ?
                (m_frames.back().time - m_frames.front().time) / (m_frames.size() - 1) : 0.0);
            size_t moving = 0, wide = 0, measured = 0, held = 0;
            double speed = 0.0, flat = 0.0, newDiff = 0.0, heldDiff = 0.0;
            for (const Frame& f : m_frames)
            {
                flat += f.flatShare;
                if (!f.valid) continue;
                ++measured;
                if (f.held)
                {
                    ++held;
                    heldDiff += f.diffCodes;
                    continue;
                }
                ++moving;
                newDiff += f.diffCodes;
                speed += f.speedHps;
                wide += f.speedHps >= kWidePanHps ? 1 : 0;
            }
            s.newDiffCodes = moving ? newDiff / moving : 0.0;
            s.heldDiffCodes = held ? heldDiff / held : 0.0;
            s.flatShare = flat / m_frames.size();
            s.motionValid = measured >= 8;
            if (moving)
            {
                s.meanPanHps = speed / moving;
                s.widePanShare = static_cast<double>(wide) / moving;
            }
            // Hold runs after each new picture (a new picture = a valid non-held frame).
            size_t pictures = 0, heldPictures = 0, runFrames = 0;
            int firstRun = -1, minRun = 0, maxRun = 0;
            bool even = true;
            for (size_t i = 0; i < m_frames.size(); ++i)
            {
                const Frame& f = m_frames[i];
                if (!f.valid || f.held) continue;
                size_t j = i + 1;
                while (j < m_frames.size() && m_frames[j].valid && m_frames[j].held) ++j;
                if (j >= m_frames.size()) break;   // the run is not finished yet
                const int run = static_cast<int>(j - i - 1);
                ++pictures;
                heldPictures += run > 0 ? 1 : 0;
                runFrames += static_cast<size_t>(run) + 1;
                if (firstRun < 0) { firstRun = minRun = maxRun = run; }
                else
                {
                    if (run != firstRun) even = false;
                    minRun = (std::min)(minRun, run);
                    maxRun = (std::max)(maxRun, run);
                }
            }
            if (pictures)
            {
                s.holdShare = static_cast<double>(heldPictures) / pictures;
                s.pictureShare = static_cast<double>(pictures) / static_cast<double>(runFrames);
            }
            s.evenHolds = pictures >= 4 && even;
            s.uniformHolds = pictures >= 4 && minRun >= 1 && maxRun - minRun <= 1;
            return s;
        }

    private:
        struct Frame
        {
            double time = 0.0;
            bool valid = false;
            bool held = false;
            double speedHps = 0.0;
            double flatShare = 0.0;
            double diffCodes = 0.0;
        };
        std::deque<Frame> m_frames;
        double m_stillSeconds = 0.0;
        double m_time = 0.0;
    };

    // ---- the class rules ----
    struct ClassifierInput
    {
        // DV L11 content type (wins when it names a class).
        bool l11Present = false;
        uint8_t l11ContentType = 0;
        // The existing film / video classification at the measured rates
        // (InterpolationPolicy::IsFilmLike with source_class), and whether the
        // content rate came from a pulldown lock.
        bool rateKnown = false;
        bool filmLike = false;
        bool pulldownLocked = false;
        WindowStats stats;
        // Hunt 2 (cadence_class_fixes, default on): the rate-based carrier test,
        // animation evidence at film rates and the film-rate static UI still;
        // carrierFilmLike = the classification at the pictures' own rate
        // (contentHz x stats.pictureShare), used only when the holds are a
        // carrier's. A lock alone is not film with the fixes (the film rate is).
        bool classFixes = true;
        bool carrierFilmLike = false;
        double contentHz = 0.0;   // the measured content rate (the carrier's, without a lock)
    };

    // Hunt 2: the holds are a faster carrier's repeats (see kAnimationChangeCodes).
    inline bool CarrierHolds(const ClassifierInput& in)
    {
        // Pictures at or under the pulldown film floor (30p drawn on twos in 60 Hz: 15 per
        // second) are drawn holds, not a carrier's: the same floor as Pulldown's lock.
        return in.classFixes && in.rateKnown && !in.pulldownLocked && !in.filmLike && in.stats.uniformHolds &&
            Pulldown::AboveFilmFloor(in.contentHz * in.stats.pictureShare, true);
    }

    struct Classification
    {
        ContentClass content = ContentClass::Unknown;
        IntentSource source = IntentSource::Off;
        const char* reason = "no information";
    };

    // Order: L11 > static UI > animation > film > sports > video.
    inline Classification Classify(const ClassifierInput& in)
    {
        Classification c;
        ContentClass fromMetadata = ContentClass::Unknown;
        if (in.l11Present && ContentClassFromL11(in.l11ContentType, fromMetadata))
        {
            c.content = fromMetadata;
            c.source = IntentSource::DvL11;
            c.reason = "L11 content type";
            return c;
        }
        const WindowStats& s = in.stats;
        const bool filmRate = in.rateKnown && (in.filmLike || in.pulldownLocked);
        const bool carrier = CarrierHolds(in);
        const double staticSeconds = in.classFixes && filmRate ? kStaticUiSeconds * kStaticUiFilmFactor : kStaticUiSeconds;
        if (s.stillSeconds >= staticSeconds)
        {
            c.content = ContentClass::StaticUi;
            c.source = IntentSource::Inferred;
            c.reason = "long zero motion with static edges";
            return c;
        }
        const bool carrierVeto = in.classFixes ? carrier : (s.evenHolds && !in.pulldownLocked);
        const bool animationEvidence = !in.classFixes || !filmRate ||
            (s.newDiffCodes >= kAnimationChangeCodes && s.newDiffCodes >= kAnimationChangeContrast * s.heldDiffCodes);
        if (s.motionValid && s.seconds >= kAnimationMinSeconds && s.holdShare >= kAnimationHoldShare &&
            s.flatShare >= kAnimationFlatShare && !carrierVeto && animationEvidence)
        {
            c.content = ContentClass::Animation;
            c.source = IntentSource::Inferred;
            c.reason = "twos-holds and flat areas";
            return c;
        }
        if (!in.rateKnown)
            return c;
        if ((in.pulldownLocked && !in.classFixes) || in.filmLike || (carrier && in.carrierFilmLike))
        {
            c.content = ContentClass::Film;
            c.source = IntentSource::Inferred;
            c.reason = in.filmLike ? (in.pulldownLocked ? "pulldown lock at a film rate" : "film-like measured rate") :
                in.pulldownLocked ? "pulldown lock" : "carrier repeats at a film rate";
            return c;
        }
        c.source = IntentSource::Inferred;
        if (s.motionValid && s.seconds >= kSportsMinSeconds && s.meanPanHps >= kSportsMeanPanHps &&
            s.widePanShare >= kSportsWidePanShare)
        {
            c.content = ContentClass::Sports;
            c.reason = "video-class source with sustained large pans";
            return c;
        }
        c.content = ContentClass::Video;
        c.reason = "video-class source";
        return c;
    }

    // ---- hysteresis: a class changes only after holdSeconds and at a cut ----
    class Hysteresis
    {
    public:
        void SetHoldSeconds(double seconds) { m_hold = seconds > 0.0 ? seconds : 0.0; }
        double HoldSeconds() const { return m_hold; }

        void Reset()
        {
            m_current = Classification();
            m_pending = ContentClass::Unknown;
            m_pendingSince = 0.0;
        }

        // now: seconds at the content rate (Statistics::Time()); cut: this
        // frame is a cut. Returns true when the class (or its source) changed.
        // The first class is taken once it has held for holdSeconds (nothing
        // was selected before, so there is no cut to wait for); later changes
        // need holdSeconds of the new class and then a cut. Static UI is
        // entered after holdSeconds without a cut (the picture is still) and
        // left as soon as the still ends. An Unknown
        // candidate never replaces a class. A change of source alone (the
        // same class now from metadata) is taken at once: nothing visible moves.
        bool Update(const Classification& candidate, double now, bool cut)
        {
            if (candidate.content == ContentClass::Unknown)
            {
                m_pending = ContentClass::Unknown;
                return false;
            }
            if (candidate.content == m_current.content)
            {
                m_pending = ContentClass::Unknown;
                if (candidate.source != m_current.source)
                {
                    m_current = candidate;
                    return true;
                }
                m_current.reason = candidate.reason;
                return false;
            }
            if (candidate.content != m_pending)
            {
                m_pending = candidate.content;
                m_pendingSince = now;
            }
            const bool held = now - m_pendingSince >= m_hold - 1e-9;
            // A still picture hides the switch into static UI (no cut needed);
            // the end of a still (motion or a cut) leaves it at once.
            const bool enterStatic = candidate.content == ContentClass::StaticUi;
            const bool leaveStatic = m_current.content == ContentClass::StaticUi;
            if ((held && (m_current.content == ContentClass::Unknown || cut || enterStatic)) || leaveStatic)
            {
                m_current = candidate;
                m_pending = ContentClass::Unknown;
                return true;
            }
            return false;
        }

        const Classification& Current() const { return m_current; }

    private:
        Classification m_current;
        ContentClass m_pending = ContentClass::Unknown;
        double m_pendingSince = 0.0;
        double m_hold = 5.0;
    };
}
