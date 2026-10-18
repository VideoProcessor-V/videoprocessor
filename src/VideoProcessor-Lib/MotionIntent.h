#pragma once

// Motion intent (intent interpreter item 2, Authentic Motion analog).
// docs/VP-intent-interpreter-design.md section 1 and the owner decision
// 2026-10-17 "motion intent as selectable policies".
//
// Per shot: pan speed v in picture HEIGHTS per second (aspect-independent),
// steadiness (the spread of v within the shot), edge contrast and shot
// length. Five owner-selectable policies (key intent_motion_policy):
//   display    - analysis only (status + log); the scale stays 1.
//   ease_fast  - reduce strength as pan speed rises.
//   ease_slow  - full strength on fast pans; ease on slow, steady shots.
//   visibility - scale by the judder-visibility estimate J = band(v) x edge,
//                scale = min + (1 - min) x smoothstep(J_lo, J_hi, J).
//   metadata   - act only on a DV L11 frame-rate-conversion hint or real
//                DV2 when present; otherwise 1.
// Shared rules: the owner's dejudder_strength is the master; the scale only
// multiplies it, in [intent_motion_min, 1], never above 1. Decided once per
// shot and applied at the cut (never mid-shot; a missed cut waits for the
// next one); dead band 0.1; shots shorter than intent_motion_min_shot_s
// inherit the previous scale; fades keep the previous value; video-class
// sources (not film-like by source_class / InterpolationPolicy::IsFilmLike
// at the measured rates) get 1.
//
// Rule-based only: no fitted or neural values. Every constant below is a
// documented, owner-editable taste constant (not a key: the owner wants
// fewer keys). No frame rate, refresh rate or resolution is fixed: speeds
// come from the measured content rate, distances are shares of the picture
// height.
//
// Pure: no renderer, GPU or configuration state (CPU-tested).

#include <IntentDescriptor.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>

namespace MotionIntent
{
    // ---- judder visibility (documented, owner-editable, not fitted) ----
    // band(v): 0 for a nearly static shot (below kBandStaticHps), rising to 1
    // at kBandRiseHps, flat to kBandFallHps, falling to 0 at kBandBlurHps where
    // motion blur dominates (judder hides in the blur).
    constexpr double kBandStaticHps = 0.02;
    constexpr double kBandRiseHps = 0.08;
    constexpr double kBandFallHps = 0.40;
    constexpr double kBandBlurHps = 1.20;
    // Edge term: the mean luma gradient of the reduced grid (10-bit limited
    // codes per grid step) mapped 0..1 between these (soft, low-contrast
    // pictures show little judder).
    constexpr double kEdgeLoCodes = 1.5;
    constexpr double kEdgeHiCodes = 10.0;
    // Visibility thresholds J_lo / J_hi of the visibility policy.
    constexpr double kVisibilityLo = 0.10;
    constexpr double kVisibilityHi = 0.60;
    // ease_fast: full strength up to kEaseFastLoHps, the minimum from kEaseFastHiHps.
    constexpr double kEaseFastLoHps = 0.15;
    constexpr double kEaseFastHiHps = 0.60;
    // ease_slow: a shot at or below kEaseSlowLoHps is slow, at or above
    // kEaseSlowHiHps fast (full strength); steady when the relative spread
    // of v (standard deviation / max(mean, kSpreadFloorHps)) is at or below
    // kSteadyLo, unsteady from kSteadyHi.
    constexpr double kEaseSlowLoHps = 0.05;
    constexpr double kEaseSlowHiHps = 0.25;
    constexpr double kSpreadFloorHps = 0.02;
    constexpr double kSteadyLo = 0.25;
    constexpr double kSteadyHi = 0.75;

    // ---- shared hysteresis (owner rules) ----
    constexpr double kDeadBand = 0.1;
    // A shot needs at least this many measured frame pairs in the look-ahead
    // when the display reaches its cut; otherwise the cut was missed and the
    // value waits for the next cut.
    constexpr int kMinDecisionPairs = 2;
    // Fades: a frame whose mean luma moves at least FadeStepCodes (10-bit codes
    // per frame: kFadeStepCodesPerSecond at the content rate) in the same
    // direction as the frames before it for kFadeMinRunSeconds, without a cut,
    // is fading; a frame whose mean luma is at or below kNearBlackCodes
    // (limited 64..940) is near black. A cut with a fading or near-black frame
    // within kFadeWindowSeconds on either side is a fade boundary: the previous
    // value is kept. Bug hunt 3 F9: in time, not frames (identical at 24p: 1
    // code per frame, a run of 3 frames, a window of 6; at 48p HFR a 4 s fade
    // of 100 codes moves 0.52 codes per frame and was missed).
    constexpr double kFadeStepCodesPerSecond = 24.0;
    constexpr double kFadeMinRunSeconds = 3.0 / 24.0;
    constexpr double kNearBlackCodes = 80.0;
    constexpr double kFadeWindowSeconds = 6.0 / 24.0;
    // The rate the frame counts fall back to while the content rate is unknown
    // (the values before: 1 code, 3 frames, 6 frames).
    constexpr double kFadeUnknownRateHz = 24.0;

    inline double FadeRateHz(double contentHz)
    {
        return std::isfinite(contentHz) && contentHz > 0.0 ? contentHz : kFadeUnknownRateHz;
    }
    inline double FadeStepCodes(double contentHz) { return kFadeStepCodesPerSecond / FadeRateHz(contentHz); }
    inline int FadeMinRunFrames(double contentHz)
    {
        return (std::max)(1, static_cast<int>(std::lround(kFadeMinRunSeconds * FadeRateHz(contentHz))));
    }
    inline int FadeWindowFrames(double contentHz)
    {
        return (std::max)(1, static_cast<int>(std::lround(kFadeWindowSeconds * FadeRateHz(contentHz))));
    }

    // The fade run: one frame's mean-luma change (measurable: a valid pair without
    // a cut, else the run starts over); true while the frame is fading.
    struct FadeRun
    {
        int run = 0;
        int sign = 0;
        bool Step(bool measurable, double meanDelta, double contentHz)
        {
            if (!measurable)
            {
                run = 0;
                sign = 0;
                return false;
            }
            const double step = FadeStepCodes(contentHz);
            const int s = meanDelta >= step ? 1 : meanDelta <= -step ? -1 : 0;
            run = s != 0 && s == sign ? run + 1 : (s != 0 ? 1 : 0);
            sign = s;
            return run >= FadeMinRunFrames(contentHz);
        }
        void Reset() { run = 0; sign = 0; }
    };

    inline double Smoothstep(double lo, double hi, double x)
    {
        if (!(hi > lo)) return x >= hi ? 1.0 : 0.0;
        const double t = (std::min)(1.0, (std::max)(0.0, (x - lo) / (hi - lo)));
        return t * t * (3.0 - 2.0 * t);
    }

    inline double Band(double hps)
    {
        return Smoothstep(kBandStaticHps, kBandRiseHps, hps) * (1.0 - Smoothstep(kBandFallHps, kBandBlurHps, hps));
    }

    inline double EdgeTerm(double edgeCodes) { return Smoothstep(kEdgeLoCodes, kEdgeHiCodes, edgeCodes); }

    inline double Visibility(double hps, double edgeTerm) { return Band(hps) * (std::min)(1.0, (std::max)(0.0, edgeTerm)); }

    inline double ClampScale(double scale, double minScale)
    {
        const double lo = (std::min)(1.0, (std::max)(0.0, minScale));
        if (!std::isfinite(scale)) return 1.0;
        return (std::min)(1.0, (std::max)(lo, scale));
    }

    // ---- the shot statistics ----
    struct ShotStats
    {
        bool valid = false;            // at least kMinDecisionPairs measured pairs
        int measuredPairs = 0;
        int vectorPairs = 0;           // pairs whose speed came from the de-judder's valid vectors
        double seconds = 0.0;          // seconds seen so far (at the measured content rate)
        double meanHps = 0.0;          // mean pan speed over measured pairs (holds count as 0)
        double spreadHps = 0.0;        // standard deviation of v within the shot
        double relativeSpread = 0.0;   // spreadHps / max(meanHps, kSpreadFloorHps)
        double edgeTerm = 0.0;         // mean edge term 0..1
        double visibility = 0.0;       // J = band(meanHps) x edgeTerm
    };

    // Running sums for one shot; a sample's speed can be replaced later (the
    // de-judder's vectors for that pair arrive at render time).
    class ShotAccumulator
    {
    public:
        struct Sample
        {
            uint64_t sequence = 0;
            uint64_t previousSequence = 0;   // the frame the pair was measured against
            double contentHz = 0.0;
            double hps = 0.0;
            bool fromVectors = false;
        };

        // One frame: dt seconds, the edge term of its picture, and a measured
        // pair (hps valid) or not.
        void AddFrame(double dtSeconds, bool gridValid, double edgeTerm)
        {
            if (dtSeconds > 0.0 && std::isfinite(dtSeconds)) m_seconds += dtSeconds;
            if (gridValid && std::isfinite(edgeTerm)) { m_edgeSum += edgeTerm; ++m_edgeCount; }
        }

        void AddPair(uint64_t sequence, uint64_t previousSequence, double contentHz, double hps)
        {
            if (!std::isfinite(hps) || hps < 0.0) return;
            Sample s;
            s.sequence = sequence;
            s.previousSequence = previousSequence;
            s.contentHz = contentHz;
            s.hps = hps;
            m_samples.push_back(s);
            Add(hps, +1.0);
            while (m_samples.size() > kKeptSamples)
                m_samples.pop_front();   // its contribution stays in the sums
        }

        // The de-judder's valid vectors for pair (from, to): dHeights per frame.
        // Accepted only for a pair measured here between exactly these frames.
        bool ReplaceWithVectors(uint64_t from, uint64_t to, double heightsPerFrame)
        {
            if (!std::isfinite(heightsPerFrame) || heightsPerFrame < 0.0) return false;
            for (auto it = m_samples.rbegin(); it != m_samples.rend(); ++it)
            {
                if (it->sequence != to) continue;
                if (it->previousSequence != from || it->contentHz <= 0.0) return false;
                const double hps = heightsPerFrame * it->contentHz;
                Add(it->hps, -1.0);
                Add(hps, +1.0);
                if (!it->fromVectors) ++m_vectorPairs;
                it->hps = hps;
                it->fromVectors = true;
                return true;
            }
            return false;
        }

        ShotStats Stats() const
        {
            ShotStats s;
            s.measuredPairs = static_cast<int>(m_count + 0.5);
            s.vectorPairs = m_vectorPairs;
            s.seconds = m_seconds;
            s.valid = s.measuredPairs >= kMinDecisionPairs;
            if (m_count > 0.5)
            {
                s.meanHps = (std::max)(0.0, m_sum / m_count);
                const double variance = m_sumSquares / m_count - s.meanHps * s.meanHps;
                s.spreadHps = variance > 0.0 ? std::sqrt(variance) : 0.0;
            }
            s.relativeSpread = s.spreadHps / (std::max)(s.meanHps, kSpreadFloorHps);
            s.edgeTerm = m_edgeCount ? m_edgeSum / m_edgeCount : 0.0;
            s.visibility = Visibility(s.meanHps, s.edgeTerm);
            return s;
        }

        double Seconds() const { return m_seconds; }

    private:
        void Add(double hps, double sign)
        {
            m_count += sign;
            m_sum += sign * hps;
            m_sumSquares += sign * hps * hps;
        }

        static constexpr size_t kKeptSamples = 512;   // vectors arrive within a few frames
        std::deque<Sample> m_samples;
        double m_count = 0.0, m_sum = 0.0, m_sumSquares = 0.0;
        double m_edgeSum = 0.0;
        size_t m_edgeCount = 0;
        double m_seconds = 0.0;
        int m_vectorPairs = 0;
    };

    // ---- the policies ----
    inline double VisibilityScale(const ShotStats& s, double minScale)
    {
        const double m = ClampScale(minScale, 0.0);
        return ClampScale(m + (1.0 - m) * Smoothstep(kVisibilityLo, kVisibilityHi, s.visibility), m);
    }

    inline double EaseFastScale(const ShotStats& s, double minScale)
    {
        const double m = ClampScale(minScale, 0.0);
        return ClampScale(1.0 - (1.0 - m) * Smoothstep(kEaseFastLoHps, kEaseFastHiHps, s.meanHps), m);
    }

    inline double EaseSlowScale(const ShotStats& s, double minScale)
    {
        const double m = ClampScale(minScale, 0.0);
        const double slow = 1.0 - Smoothstep(kEaseSlowLoHps, kEaseSlowHiHps, s.meanHps);
        const double steady = 1.0 - Smoothstep(kSteadyLo, kSteadyHi, s.relativeSpread);
        return ClampScale(1.0 - (1.0 - m) * slow * steady, m);
    }

    // DV L11 frame-rate-conversion hint (2 bits; the decoding is an assumption
    // until a live packet confirms it, see CR210DolbyVisionTunnelFormatter):
    // 0 = no hint (unchanged), 1 = conversion off (the judder is intended: the
    // minimum), 2 = low (halfway), 3 = high (full strength).
    inline bool L11FrameRateConversionScale(uint8_t hint, double minScale, double& scale)
    {
        const double m = ClampScale(minScale, 0.0);
        switch (hint & 3U)
        {
        case 1: scale = m; return true;
        case 2: scale = m + 0.5 * (1.0 - m); return true;
        case 3: scale = 1.0; return true;
        default: return false;
        }
    }

    struct PolicyInput
    {
        MotionIntentPolicy policy = MotionIntentPolicy::Display;
        double minScale = 0.5;               // intent_motion_min
        bool rateKnown = false;              // the content (and, for measured, display) rate is known
        bool filmLike = false;               // the existing film / video classification at the measured rates
        bool rolloffOn = false;              // dejudder_motion_rolloff (R-A) is on
        bool l11Present = false;
        uint8_t l11FrameRateConversion = 0;
        bool dv2Present = false;             // reserved: real DV2 Authentic Motion
        double dv2Scale = 1.0;
    };

    struct Decision
    {
        double candidate = 1.0;              // what the policy wants for this shot
        IntentSource source = IntentSource::Off;
        double inferred = 1.0;               // the policy's measured value (display / metadata: visibility)
        bool metadataPresent = false;
        double metadata = 1.0;               // the metadata value when present
        double easeFast = 1.0, easeSlow = 1.0, visibility = 1.0;   // every measured policy, for comparison
        const char* reason = "";
        // False only when the policy needs shot statistics it does not have yet
        // (the fade model then has no target to ease toward).
        bool decided = true;
    };

    inline Decision Evaluate(const ShotStats& s, const PolicyInput& in)
    {
        Decision d;
        const double m = ClampScale(in.minScale, 0.0);
        if (s.valid)
        {
            d.easeFast = EaseFastScale(s, m);
            d.easeSlow = EaseSlowScale(s, m);
            d.visibility = VisibilityScale(s, m);
        }
        d.inferred = in.policy == MotionIntentPolicy::EaseFast ? d.easeFast :
            in.policy == MotionIntentPolicy::EaseSlow ? d.easeSlow : d.visibility;
        double meta = 1.0;
        if (in.dv2Present)
        {
            d.metadataPresent = true;
            d.metadata = ClampScale(in.dv2Scale, m);
        }
        else if (in.l11Present && L11FrameRateConversionScale(in.l11FrameRateConversion, m, meta))
        {
            d.metadataPresent = true;
            d.metadata = ClampScale(meta, m);
        }
        if (!in.rateKnown)
        {
            d.reason = "content rate not known";
            return d;
        }
        if (!in.filmLike)
        {
            d.reason = "video-class source";
            return d;
        }
        switch (in.policy)
        {
        case MotionIntentPolicy::Display:
            d.reason = "display only";
            return d;
        case MotionIntentPolicy::Metadata:
            if (!d.metadataPresent)
            {
                d.reason = "no metadata hint";
                return d;
            }
            d.candidate = d.metadata;
            d.source = in.dv2Present ? IntentSource::Dv2 : IntentSource::DvL11;
            d.reason = in.dv2Present ? "DV2 motion" : "L11 frame-rate-conversion hint";
            return d;
        case MotionIntentPolicy::EaseFast:
            if (in.rolloffOn)
            {
                // R-A (dejudder_motion_rolloff) already eases fast pans per pixel:
                // never reduce twice.
                d.reason = "deferred to dejudder_motion_rolloff";
                return d;
            }
            break;
        default:
            break;
        }
        if (!s.valid)
        {
            d.reason = "no motion statistics";
            d.decided = false;
            return d;
        }
        d.candidate = d.inferred;
        d.source = IntentSource::Inferred;
        d.reason = in.policy == MotionIntentPolicy::EaseFast ? "pan speed" :
            in.policy == MotionIntentPolicy::EaseSlow ? "slow steady shot eased" : "judder visibility";
        return d;
    }

    // ---- applying at cuts ----
    struct CutInput
    {
        bool decisionReady = false;          // the look-ahead measured enough of the new shot
        bool fadeBoundary = false;
        bool shotEndKnown = false;           // the next cut is already in the look-ahead
        double shotSeconds = 0.0;            // its length when the end is known
        double minShotSeconds = 2.0;         // intent_motion_min_shot_s
        double minScale = 0.5;
    };

    struct CutResult
    {
        bool changed = false;
        double applied = 1.0;
        IntentSource source = IntentSource::Off;
        const char* reason = "";
    };

    // ---- the fade model (owner decision 2026-10-17, key intent_ease_s) ----
    // At a cut the look-ahead holds only the first queued frames of the new
    // shot (a few tenths of a second). With intent_ease_s > 0 the cut applies a
    // PROVISIONAL value: the policy's value when the queued evidence covers at
    // least kEaseConfidentSeconds of the shot, otherwise the previous value
    // carried over. Over the first intent_ease_s of the shot the value then
    // eases toward the better-informed estimate as more frames arrive,
    // rate-limited to the full scale range (1 - intent_motion_min) per
    // intent_ease_s, never overshooting; at intent_ease_s the value settles and
    // holds for the rest of the shot. intent_ease_s 0 = the instant decision at
    // the cut alone (OnCut). Documented constant, not a key:
    constexpr double kEaseConfidentSeconds = 0.5;

    // Rate-limited follower toward a target; shared by motion intent and black
    // protection's LED share. Pure.
    class ShotEase
    {
    public:
        // A cut: start a new ease of `easeSeconds` (0 = no easing).
        void Start(double easeSeconds)
        {
            m_easeSeconds = easeSeconds > 0.0 && std::isfinite(easeSeconds) ? easeSeconds : 0.0;
            m_elapsed = 0.0;
            m_active = m_easeSeconds > 0.0;
        }
        void Stop() { m_active = false; }
        bool Active() const { return m_active; }
        double Elapsed() const { return m_elapsed; }

        // One displayed source frame of dtSeconds: `value` moves toward
        // `target` by at most range x dt / easeSeconds (never past it); at
        // easeSeconds the ease settles (inactive; the value then holds).
        double Step(double value, double target, double range, double dtSeconds, bool haveTarget)
        {
            if (!m_active)
                return value;
            const double dt = dtSeconds > 0.0 && std::isfinite(dtSeconds) ? dtSeconds : 0.0;
            m_elapsed += dt;
            if (haveTarget && std::isfinite(target))
            {
                const double limit = (std::max)(0.0, range) * dt / m_easeSeconds;
                const double delta = target - value;
                value += (std::max)(-limit, (std::min)(limit, delta));
            }
            if (m_elapsed >= m_easeSeconds - 1e-9)
                m_active = false;   // settled: hold
            return value;
        }

    private:
        double m_easeSeconds = 0.0;
        double m_elapsed = 0.0;
        bool m_active = false;
    };

    // The scale on screen; changes only through OnCut / OnCutEased and, while
    // a shot's ease runs, Progress (and Reset).
    class Applier
    {
    public:
        void Reset()
        {
            m_applied = 1.0;
            m_source = IntentSource::Off;
            m_ease.Stop();
        }

        double Applied() const { return m_applied; }
        IntentSource Source() const { return m_source; }
        bool Easing() const { return m_ease.Active(); }

        // The cut under the fade model; easeSeconds 0 = OnCut exactly.
        CutResult OnCutEased(const Decision& d, const CutInput& in, const ShotStats& s, double easeSeconds)
        {
            m_ease.Stop();
            if (!(easeSeconds > 0.0))
                return OnCut(d, in);
            if (in.fadeBoundary || (in.shotEndKnown && in.shotSeconds < in.minShotSeconds))
                return OnCut(d, in);   // fades keep the previous value; a short shot inherits it (no ease)
            CutResult r;
            if (in.decisionReady && s.seconds >= kEaseConfidentSeconds - 1e-9)
            {
                r = OnCut(d, in);   // confident: the provisional value is the decision (dead band applies)
                if (r.changed) r.reason = "provisional (confident) at the cut; easing";
            }
            else
            {
                m_applied = ClampScale(m_applied, in.minScale);
                r.applied = m_applied;
                r.source = m_source;
                r.reason = "provisional: previous value carried over (low confidence); easing";
            }
            m_ease.Start(easeSeconds);
            return r;
        }

        // A newly displayed frame of the shot being eased: follow the updated
        // estimate (rate-limited, no dead band); a short shot whose end became
        // known stops the ease and holds. Returns true when the value moved.
        bool Progress(const Decision& d, const ShotStats& s, double dtSeconds, double minScale, bool shortShotKnown)
        {
            if (!m_ease.Active())
                return false;
            if (shortShotKnown)
            {
                m_ease.Stop();
                return false;
            }
            const double lo = ClampScale(minScale, 0.0);
            const double before = m_applied;
            // A target exists once the shot has statistics, or when the policy
            // decides without them (metadata hint, display only, video class).
            (void)s;
            const bool haveTarget = d.decided;
            const double target = ClampScale(d.candidate, lo);
            m_applied = ClampScale(m_ease.Step(m_applied, target, 1.0 - lo, dtSeconds, haveTarget), lo);
            if (m_applied != before)
                m_source = m_applied < 1.0 ? (d.source != IntentSource::Off ? d.source : m_source) : IntentSource::Off;
            return m_applied != before;
        }

        CutResult OnCut(const Decision& d, const CutInput& in)
        {
            CutResult r;
            r.applied = m_applied;
            r.source = m_source;
            if (!in.decisionReady)
                r.reason = "missed cut: waits for the next cut";
            else if (in.fadeBoundary)
                r.reason = "fade: keeps the previous value";
            else if (in.shotEndKnown && in.shotSeconds < in.minShotSeconds)
                r.reason = "short shot inherits";
            else
            {
                const double wanted = ClampScale(d.candidate, in.minScale);
                if (std::fabs(wanted - m_applied) < kDeadBand - 1e-12)
                    r.reason = "within the dead band";
                else
                {
                    r.changed = true;
                    m_applied = wanted;
                    m_source = wanted < 1.0 ? d.source : IntentSource::Off;
                    r.applied = m_applied;
                    r.source = m_source;
                    r.reason = d.reason;
                }
            }
            // The minimum may have been raised since this value was applied.
            m_applied = ClampScale(m_applied, in.minScale);
            r.applied = m_applied;
            return r;
        }

    private:
        double m_applied = 1.0;
        IntentSource m_source = IntentSource::Off;
        ShotEase m_ease;
    };

    // The comparison log line ("motion scale <policy> metadata=... inferred=...").
    inline std::string LogLine(MotionIntentPolicy policy, const Decision& d, const ShotStats& s, const CutResult& r)
    {
        char metadata[16];
        if (d.metadataPresent) std::snprintf(metadata, sizeof(metadata), "%.2f", d.metadata);
        else std::snprintf(metadata, sizeof(metadata), "-");
        char text[384];
        std::snprintf(text, sizeof(text),
            "motion scale %s metadata=%s inferred=%.2f candidate=%.2f applied=%.2f (%s; %s) "
            "ease_fast=%.2f ease_slow=%.2f visibility=%.2f pan=%.3f picture heights/s spread=%.3f edge=%.2f J=%.2f "
            "pairs=%d vectors=%d seen=%.2f s",
            ToString(policy), metadata, d.inferred, d.candidate, r.applied, r.reason, d.reason,
            d.easeFast, d.easeSlow, d.visibility, s.meanHps, s.spreadHps, s.edgeTerm, s.visibility,
            s.measuredPairs, s.vectorPairs, s.seconds);
        return text;
    }
}
