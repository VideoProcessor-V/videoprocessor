// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ResyncCause.h>
#include <VP-V/modules/control/diagnostics/VpEventLine.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Per-event resync metrics (item 15). Tracks every FULL resync from the moment it is recorded until
// it SETTLES, then emits ONE vp-event/1 `resync_settle` line:
//
//   settle_ms      source milliseconds (caller-provided sourceMs, never a wall clock) from the carrying
//                  frame's seal to the settling frame's seal
//   settle_frames  frames sealed after the carrying frame, up to and including the settling frame
//   drops/repeats  attributed to the NEWEST pending event whose fromSeq <= the dropped/repeated seq
//   relock         yes | no | na (na: the event never touched the pulldown)
//   relock_frames  frames from the carrying frame to the relock completing (0 unless relock=yes)
//   vectors        valid | invalid
//
// SETTLED = every reader that was DUE (AddReader) has acted (OnReaderAct) AND the motion vectors are valid
// again (OnVectorsValid) AND, if the event touched the pulldown (OnEvent flag, or OnRelock(false) after
// it), the relock finished (OnRelock(true)).
// ABANDONED = not settled after MaxSettleFrames() sealed frames (default: abandon seconds, 10, times the
// measured frame rate the caller gives with SetFrameRate; no rate means no abandon bound and that is
// said in StatusText). An abandoned event emits its line with vectors=invalid (the "did not settle"
// marker; relock=no when a relock was needed and not done) and counts in Unsettled().
//
// MINOR and TIMING events are only counted. At most kPendingMax events are tracked at once; a FULL
// that finds the ring full is NOT tracked and counts in Overflowed() (shown in StatusText).
//
// Threading: every method takes one mutex (OnEvent may come from the enqueue thread, the rest from the
// render thread); the sink is called AFTER the mutex is released. Header-only, pure C++14, deterministic
// (the only time is the caller's sourceMs; the line's t= comes from SetClock, default: the last sourceMs).
namespace VpResync {

class EventMetrics {
public:
    static constexpr size_t kPendingMax = 64;
    static constexpr size_t kReadersMax = 16;
    static constexpr double kDefaultAbandonSeconds = 10.0;

    using Sink = std::function<void(const std::string&)>;

    struct Settle {
        uint64_t seq = 0;
        double settleMs = 0.0;
        uint32_t settleFrames = 0;
        uint32_t drops = 0;
        uint32_t repeats = 0;
        const char* relock = "na";
        uint32_t relockFrames = 0;
        bool vectorsValid = true;
        bool abandoned = false;
        Cause cause = Cause::None;
    };

    void SetSink(Sink sink)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sink = std::move(sink);
    }
    // Source of the t= field of the emitted lines (host: VpEvent::NowMs). Default: the last sourceMs.
    void SetClock(std::function<double()> clock)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_clock = std::move(clock);
    }
    // Measured source frames per second (0 or negative = not measured: no abandon bound).
    void SetFrameRate(double framesPerSecond)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fps = framesPerSecond > 0.0 ? framesPerSecond : 0.0;
    }
    void SetAbandonSeconds(double seconds)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_abandonSeconds = seconds > 0.0 ? seconds : kDefaultAbandonSeconds;
    }
    // Explicit abandon bound in frames; 0 = derive it (abandon seconds * frame rate).
    void SetMaxSettleFrames(uint32_t frames)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_maxFramesOverride = frames;
    }
    // false: no vector consumer reports validity, so events do not wait for it (line says vectors=valid).
    void SetVectorsRequired(bool required)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_vectorsRequired = required;
    }

    // A reader that is due for every tracked FULL (setup). Returns its index, or -1 when the name is
    // empty/duplicate or kReadersMax is reached.
    int AddReader(const char* name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (name == nullptr || *name == '\0' || m_readerCount >= kReadersMax)
            return -1;
        for (size_t i = 0; i < m_readerCount; ++i)
            if (m_readers[i] == name)
                return -1;
        m_readers[m_readerCount] = name;
        return static_cast<int>(m_readerCount++);
    }

    // The due readers, comma-joined: the `readers` field of the resync line.
    std::string DueReadersText() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string s;
        for (size_t i = 0; i < m_readerCount; ++i) {
            if (i)
                s += ',';
            s += m_readers[i];
        }
        return s;
    }

    // The additive fields of the `resync` line: injected=0|1 and readers (omitted when empty).
    // readers contains ',' so the encoder quotes it: readers="render-ahead,timeline".
    static void AppendResyncExtras(std::vector<VpEvent::Field>& fields, bool injected, const std::string& readers)
    {
        fields.emplace_back("injected", injected ? 1 : 0);
        if (!readers.empty())
            fields.emplace_back("readers", readers);
    }

    // The recorded event (Shadow's record path; any thread).
    void OnEvent(uint64_t seq, Level level, Cause cause, uint32_t gapFrames, bool injected, bool touchesPulldown = false)
    {
        (void)gapFrames;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (level == Level::Minor) {
            ++m_minor;
            return;
        }
        if (level == Level::Timing) {
            ++m_timing;
            return;
        }
        if (level != Level::Full)
            return;
        ++m_fullSeen;
        if (m_pendingCount >= kPendingMax) {
            ++m_overflowed;
            return;
        }
        Pending& p = m_pending[m_pendingCount++];
        p = Pending();
        p.seq = seq;
        p.cause = cause;
        p.injected = injected;
        p.due = DueMaskLocked();
        p.vectorsOk = !m_vectorsRequired || m_vectorsValid;
        p.relockNeeded = touchesPulldown || m_relockPending;
        p.relockDone = !p.relockNeeded;
        ++m_tracked;
    }

    // Once per sealed frame, in sequence order (FrameSeal subscriber, render thread). Settles and
    // abandons are emitted after this returns from the critical section.
    void OnSealed(const std::shared_ptr<VpIndicator::Indicator>& ind, uint64_t seq, VpIndicator::CounterMask changed,
                  double sourceMs)
    {
        (void)changed;
        std::vector<std::string> lines;
        Sink sink;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!ind) {
                ++m_noIndicator;
                return;
            }
            m_lastSourceMs = sourceMs;
            ++m_sealedFrames;
            const uint32_t maxFrames = MaxSettleFramesLocked();
            size_t out = 0;
            for (size_t i = 0; i < m_pendingCount; ++i) {
                Pending& p = m_pending[i];
                bool done = false;
                if (seq >= p.seq) {
                    if (!p.started) {
                        p.started = true;
                        p.startMs = sourceMs;
                        p.frames = 0;
                    } else {
                        ++p.frames;
                    }
                    if (IsSettled(p)) {
                        done = true;
                        Finish(p, sourceMs, false, lines);
                    } else if (maxFrames != 0 && p.frames >= maxFrames) {
                        done = true;
                        Finish(p, sourceMs, true, lines);
                    }
                }
                if (!done)
                    m_pending[out++] = p;
            }
            m_pendingCount = out;
            sink = m_sink;
        }
        if (sink)
            for (const std::string& l : lines)
                sink(l);
    }

    // A reader acted at the seal of `seq`: it handled every pending event with fromSeq <= seq.
    void OnReaderAct(const char* name, uint64_t seq)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const int idx = ReaderIndexLocked(name);
        if (idx < 0) {
            ++m_unknownReaderActs;
            return;
        }
        for (size_t i = 0; i < m_pendingCount; ++i)
            if (m_pending[i].seq <= seq)
                m_pending[i].acted |= 1u << static_cast<unsigned>(idx);
    }

    void OnDrop(uint64_t seq) { Attribute(seq, true); }
    void OnRepeat(uint64_t seq) { Attribute(seq, false); }

    // The motion vectors became valid (true) / were invalidated (false).
    void OnVectorsValid(bool valid)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_vectorsValid = valid;
        for (size_t i = 0; i < m_pendingCount; ++i)
            m_pending[i].vectorsOk = valid;
    }

    // Pulldown relock: false = a relock started and is pending (every pending event now waits for it),
    // true = it finished.
    void OnRelock(bool done)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_relockPending = !done;
        for (size_t i = 0; i < m_pendingCount; ++i) {
            Pending& p = m_pending[i];
            if (!done) {
                p.relockNeeded = true;
                p.relockDone = false;
            } else if (p.relockNeeded && !p.relockDone) {
                p.relockDone = true;
                p.relockFrames = p.started ? p.frames : 0;
            }
        }
    }

    // Counters (any thread).
    uint64_t Tracked() const { return Read(m_tracked); }
    uint64_t Settled() const { return Read(m_settled); }
    uint64_t Unsettled() const { return Read(m_unsettled); }
    uint64_t Overflowed() const { return Read(m_overflowed); }
    uint64_t MinorCount() const { return Read(m_minor); }
    uint64_t TimingCount() const { return Read(m_timing); }
    uint64_t UnattributedDrops() const { return Read(m_unattributedDrops); }
    uint64_t UnattributedRepeats() const { return Read(m_unattributedRepeats); }
    uint64_t UnknownReaderActs() const { return Read(m_unknownReaderActs); }
    size_t PendingCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pendingCount;
    }
    // The abandon bound in frames in force now (0 = none: no frame rate measured and no override).
    uint32_t MaxSettleFrames() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return MaxSettleFramesLocked();
    }
    Settle LastSettle() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last;
    }

    // Ctrl+Alt+D / Config status.
    std::string StatusText() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string s = "resync settle: " + std::to_string(m_tracked) + " FULL tracked, " + std::to_string(m_settled) +
            " settled, " + std::to_string(m_unsettled) + " unsettled, " + std::to_string(m_pendingCount) + " pending";
        s += "; " + std::to_string(m_minor) + " minor and " + std::to_string(m_timing) + " timing counted";
        if (m_overflowed != 0)
            s += "; " + std::to_string(m_overflowed) + " FULL not tracked (pending ring of " +
                std::to_string(kPendingMax) + " full)";
        if (m_unattributedDrops != 0 || m_unattributedRepeats != 0)
            s += "; " + std::to_string(m_unattributedDrops) + " drops and " + std::to_string(m_unattributedRepeats) +
                " repeats outside any tracked event";
        if (m_unknownReaderActs != 0)
            s += "; " + std::to_string(m_unknownReaderActs) + " acts from unregistered readers";
        const uint32_t maxFrames = MaxSettleFramesLocked();
        if (maxFrames == 0)
            s += "; no abandon bound (source frame rate not measured)";
        else
            s += "; abandon after " + std::to_string(maxFrames) + " frames";
        if (m_haveLast)
            s += "; last seq " + std::to_string(m_last.seq) + (m_last.abandoned ? " UNSETTLED after " : " settled in ") +
                std::to_string(m_last.settleFrames) + " frames";
        return s;
    }

private:
    struct Pending {
        uint64_t seq = 0;
        Cause cause = Cause::None;
        bool injected = false;
        uint32_t due = 0;
        uint32_t acted = 0;
        bool vectorsOk = true;
        bool relockNeeded = false;
        bool relockDone = true;
        uint32_t relockFrames = 0;
        bool started = false;
        double startMs = 0.0;
        uint32_t frames = 0;
        uint32_t drops = 0;
        uint32_t repeats = 0;
    };

    uint64_t Read(const uint64_t& v) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return v;
    }

    uint32_t DueMaskLocked() const
    {
        return m_readerCount >= 32 ? 0xFFFFFFFFu : ((1u << static_cast<unsigned>(m_readerCount)) - 1u);
    }

    int ReaderIndexLocked(const char* name) const
    {
        if (name == nullptr)
            return -1;
        for (size_t i = 0; i < m_readerCount; ++i)
            if (m_readers[i] == name)
                return static_cast<int>(i);
        return -1;
    }

    uint32_t MaxSettleFramesLocked() const
    {
        if (m_maxFramesOverride != 0)
            return m_maxFramesOverride;
        if (m_fps <= 0.0)
            return 0;
        const double f = m_abandonSeconds * m_fps;
        if (f >= 4294967295.0)
            return 4294967295u;
        uint32_t whole = static_cast<uint32_t>(f);
        if (static_cast<double>(whole) < f)
            ++whole;   // round up
        return whole == 0 ? 1u : whole;
    }

    static bool IsSettled(const Pending& p)
    {
        return (p.acted & p.due) == p.due && p.vectorsOk && p.relockDone;
    }

    void Attribute(uint64_t seq, bool drop)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = m_pendingCount; i-- > 0;) {
            if (m_pending[i].seq <= seq) {
                if (drop) ++m_pending[i].drops; else ++m_pending[i].repeats;
                return;
            }
        }
        if (drop) ++m_unattributedDrops; else ++m_unattributedRepeats;
    }

    double NowLocked() const { return m_clock ? m_clock() : m_lastSourceMs; }

    void Finish(const Pending& p, double sourceMs, bool abandoned, std::vector<std::string>& lines)
    {
        Settle s;
        s.seq = p.seq;
        s.settleMs = sourceMs > p.startMs ? sourceMs - p.startMs : 0.0;
        s.settleFrames = p.frames;
        s.drops = p.drops;
        s.repeats = p.repeats;
        s.cause = p.cause;
        s.abandoned = abandoned;
        if (!p.relockNeeded)
            s.relock = "na";
        else
            s.relock = p.relockDone ? "yes" : "no";
        s.relockFrames = (p.relockNeeded && p.relockDone) ? p.relockFrames : 0;
        s.vectorsValid = !abandoned && (p.vectorsOk || !m_vectorsRequired);
        if (abandoned)
            ++m_unsettled;
        else
            ++m_settled;
        m_last = s;
        m_haveLast = true;
        const VpEvent::Field fields[] = {
            { "seq", s.seq },
            { "settle_ms", s.settleMs },
            { "settle_frames", static_cast<uint64_t>(s.settleFrames) },
            { "drops", static_cast<uint64_t>(s.drops) },
            { "repeats", static_cast<uint64_t>(s.repeats) },
            { "relock", s.relock },
            { "relock_frames", static_cast<uint64_t>(s.relockFrames) },
            { "vectors", s.vectorsValid ? "valid" : "invalid" },
            { "cause", Token(s.cause) },
        };
        lines.push_back(VpEvent::FormatRange(VpEvent::Kind::resync_settle, NowLocked(), fields,
                                             sizeof(fields) / sizeof(fields[0]), sizeof(fields) / sizeof(fields[0]) - 1));
    }

    mutable std::mutex m_mutex;
    Sink m_sink;                                    // m_mutex
    std::function<double()> m_clock;                // m_mutex
    double m_fps = 0.0;                             // m_mutex
    double m_abandonSeconds = kDefaultAbandonSeconds;   // m_mutex
    uint32_t m_maxFramesOverride = 0;               // m_mutex
    bool m_vectorsRequired = true;                  // m_mutex
    bool m_vectorsValid = true;                     // m_mutex
    bool m_relockPending = false;                   // m_mutex
    std::array<std::string, kReadersMax> m_readers; // m_mutex
    size_t m_readerCount = 0;                       // m_mutex
    std::array<Pending, kPendingMax> m_pending;     // m_mutex; oldest first
    size_t m_pendingCount = 0;                      // m_mutex
    double m_lastSourceMs = 0.0;                    // m_mutex
    Settle m_last;                                  // m_mutex
    bool m_haveLast = false;                        // m_mutex
    uint64_t m_fullSeen = 0;                        // m_mutex
    uint64_t m_tracked = 0;                         // m_mutex
    uint64_t m_settled = 0;                         // m_mutex
    uint64_t m_unsettled = 0;                       // m_mutex
    uint64_t m_overflowed = 0;                      // m_mutex
    uint64_t m_minor = 0;                           // m_mutex
    uint64_t m_timing = 0;                          // m_mutex
    uint64_t m_unattributedDrops = 0;               // m_mutex
    uint64_t m_unattributedRepeats = 0;             // m_mutex
    uint64_t m_unknownReaderActs = 0;               // m_mutex
    uint64_t m_noIndicator = 0;                     // m_mutex
    uint64_t m_sealedFrames = 0;                    // m_mutex
};

} // namespace VpResync
