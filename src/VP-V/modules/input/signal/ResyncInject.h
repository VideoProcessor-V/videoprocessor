// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Test-only fault injection for the resync work (Q0-A). ARMED only when the environment variable
// VP_RESYNC_INJECT is exactly "1" (read ONCE in the constructor); unarmed it is inert: every command
// answers "not armed", Poll is false, LoadSchedule/Release do nothing and every accessor returns a
// neutral value.
//
// Command "resync.inject", args "kind [arg]":
//   seek | reset | inplace | rebuild | capture_restart   no argument (an argument is an error)
//   gap n      drop n capture frames (n = positive integer, <= kMaxGap = 1000)
//   rate ppm   scale capture timestamps by 1 + ppm * 1e-6 (integer ppm, -1,000,000..+1,000,000;
//              0 restores 1.0)
//   stall ms   stall the capture thread n ms (positive integer, <= kMaxStallMs = 60000)
//   dv_switch tunnel|lldv|hdr10|sdr   the Dolby Vision signalling mode changes to the target
//   rpu_loss n     the RPU disappears for n frames (1..kMaxRpuLossFrames = 100000)
//   rpu_return     the RPU is back (also cancels the rest of an rpu_loss)
//   tunnel_handoff  a DV tunnel handoff happens
//   signal_loss ms  the capture signal is lost for ms (1..kMaxSignalLossMs = 600000)
//   signal_return   the signal is back (also cancels the rest of a signal_loss)
//   minor [n]       a late MINOR slip, optionally with a gap of n frames (1..kMaxGap)
//   full            a plain FULL resync
//   overflow n      a queue overflow burst of n frames (1..kMaxOverflow = 1000)
// Every valid request is queued for the host (bounded, kQueueMax = 16; a full queue rejects with
// "error: queue full" and applies nothing). gap, rate, stall, dv_switch, rpu_loss, rpu_return,
// tunnel_handoff, signal_loss, signal_return and overflow ALSO set the adapter-side accessors at
// Invoke/Release time (TakeGapFramesToDrop, TimestampScale, TakeStallMs, TakeDvSwitch,
// RpuLossFramesToDrop, RpuReturnPending, TakeTunnelHandoff, TakeSignalLossMs, SignalReturnPending,
// TakeOverflowFrames); the queue entry tells the host the request happened (log / Ctrl+I). minor and
// full only reach the host through Poll.
//
// Schedule (LoadSchedule / Release), a text, one entry per line:
//   # comment                        blank lines and '#' comments are ignored
//   seed 12345                       optional, before the first entry (u64; informational)
//   at 12.5s seek                    <number>s = source seconds (decimal, <= 9 integer digits)
//   at #4000 gap 3                   #<n> or "seq <n>" = source sequence
//   at seq 4100 dv_switch lldv
// Entries use the same parser and caps as Invoke; the time must not decrease within each clock kind
// (seconds, sequence); at most kMaxScheduleEntries = 100000 entries. Release(sourceSeconds, seq)
// moves every due entry into the pending queue in file order (a full queue leaves the rest scheduled
// and counts them in ScheduleBlocked). Released requests carry injected = true and their line number.
// Header-only, pure C++14. Invoke may run on any thread; the schedule is guarded by its own mutex.
namespace VpResync {

class ResyncInject {
public:
    enum class Kind : uint8_t {
        Seek, Reset, InPlace, Rebuild, Gap, Rate, Stall, CaptureRestart,
        DvSwitch, RpuLoss, RpuReturn, TunnelHandoff, SignalLoss, SignalReturn, Minor, Full, Overflow
    };

    // Target of dv_switch (Request.value).
    enum class DvTarget : int8_t { None = -1, Tunnel = 0, Lldv = 1, Hdr10 = 2, Sdr = 3 };

    struct Request {
        Kind kind = Kind::Seek;
        int64_t value = 0;      // gap frames, rate ppm, stall/signal-loss ms, rpu-loss frames, overflow frames,
                                // minor gap, DvTarget for dv_switch; 0 for the others
        bool injected = false;  // true for a request released from a schedule
        uint32_t line = 0;      // 1-based schedule line number (0 = not from a schedule)
    };

    static constexpr size_t kQueueMax = 16;
    static constexpr int64_t kMaxGap = 1000;
    static constexpr int64_t kMaxRatePpm = 1000000;
    static constexpr int64_t kMaxStallMs = 60000;
    static constexpr int64_t kMaxRpuLossFrames = 100000;
    static constexpr int64_t kMaxSignalLossMs = 600000;
    static constexpr int64_t kMaxOverflow = 1000;
    static constexpr size_t kMaxScheduleEntries = 100000;

    using Getter = std::function<const char*(const char*)>;

    explicit ResyncInject(Getter getter = Getter())
    {
        const char* v = getter ? getter("VP_RESYNC_INJECT") : DefaultGetenv("VP_RESYNC_INJECT");
        m_armed = v != nullptr && std::strcmp(v, "1") == 0;
    }

    bool Armed() const { return m_armed; }

    // "resync injection armed" when armed (plus the schedule state once one is loaded), else "".
    std::string StatusText() const
    {
        if (!m_armed)
            return "";
        std::lock_guard<std::mutex> lock(m_schedMutex);
        if (!m_scheduleLoaded)
            return "resync injection armed";
        return "resync injection armed; schedule " + std::to_string(RemainingLocked()) + " of " +
            std::to_string(m_secEntries.size() + m_seqEntries.size()) + " pending, " +
            std::to_string(m_blocked) + " blocked by a full queue";
    }

    std::string Invoke(const std::string& command, const std::string& args)
    {
        if (!m_armed)
            return "not armed";
        if (command != "resync.inject")
            return "error: unknown command";
        Request req;
        const std::string err = ParseArgs(args, req);
        if (!err.empty())
            return err;
        if (!Enqueue(req))
            return "error: queue full";
        return "ok: queued " + std::string(KindName(req.kind));
    }

    // Host / render thread: the oldest queued request (false when none, and always when unarmed).
    bool Poll(Request& out)
    {
        if (!m_armed)
            return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_count == 0)
            return false;
        out = m_queue[m_head];
        m_head = (m_head + 1) % kQueueMax;
        --m_count;
        return true;
    }

    // Adapter side (any thread). Neutral when unarmed or nothing requested.
    uint32_t TakeGapFramesToDrop() { return m_gap.exchange(0, std::memory_order_relaxed); }
    uint32_t TakeStallMs() { return m_stallMs.exchange(0, std::memory_order_relaxed); }
    double TimestampScale() const
    {
        return 1.0 + static_cast<double>(m_ratePpm.load(std::memory_order_relaxed)) * 1.0e-6;
    }
    // The pending dv_switch target (DvTarget::None when nothing is pending); taking clears it.
    DvTarget TakeDvSwitch() { return static_cast<DvTarget>(m_dvSwitch.exchange(-1, std::memory_order_relaxed)); }
    // Frames of RPU to suppress (atomic exchange: the caller keeps its own countdown); 0 = none.
    uint32_t RpuLossFramesToDrop() { return m_rpuLoss.exchange(0, std::memory_order_relaxed); }
    bool RpuReturnPending() { return m_rpuReturn.exchange(false, std::memory_order_relaxed); }
    bool TakeTunnelHandoff() { return m_handoff.exchange(false, std::memory_order_relaxed); }
    uint32_t TakeSignalLossMs() { return m_signalLossMs.exchange(0, std::memory_order_relaxed); }
    bool SignalReturnPending() { return m_signalReturn.exchange(false, std::memory_order_relaxed); }
    uint32_t TakeOverflowFrames() { return m_overflow.exchange(0, std::memory_order_relaxed); }

    // Sets the timestamp scale to 1 + ppm * 1e-6 (done by Invoke for "rate"); ignored unarmed.
    void ApplyRate(int64_t ppm)
    {
        if (!m_armed)
            return;
        if (ppm < -kMaxRatePpm) ppm = -kMaxRatePpm;
        if (ppm > kMaxRatePpm) ppm = kMaxRatePpm;
        m_ratePpm.store(ppm, std::memory_order_relaxed);
    }

    static const char* KindName(Kind k)
    {
        switch (k) {
        case Kind::Seek: return "seek";
        case Kind::Reset: return "reset";
        case Kind::InPlace: return "inplace";
        case Kind::Rebuild: return "rebuild";
        case Kind::Gap: return "gap";
        case Kind::Rate: return "rate";
        case Kind::Stall: return "stall";
        case Kind::CaptureRestart: return "capture_restart";
        case Kind::DvSwitch: return "dv_switch";
        case Kind::RpuLoss: return "rpu_loss";
        case Kind::RpuReturn: return "rpu_return";
        case Kind::TunnelHandoff: return "tunnel_handoff";
        case Kind::SignalLoss: return "signal_loss";
        case Kind::SignalReturn: return "signal_return";
        case Kind::Minor: return "minor";
        case Kind::Full: return "full";
        case Kind::Overflow: return "overflow";
        }
        return "seek";
    }

    // ---- schedule ----

    // Loads a schedule (replaces any previous one). false + error ("line N: ...") on the first bad
    // line, with nothing loaded. Unarmed: false, "not armed", nothing allocated.
    bool LoadSchedule(const std::string& text, std::string& error)
    {
        if (!m_armed) {
            error = "not armed";
            return false;
        }
        std::vector<Entry> sec, seq;
        uint64_t seed = 0;
        bool haveSeed = false;
        double lastSec = 0.0;
        uint64_t lastSeq = 0;
        size_t order = 0;
        uint32_t lineNo = 0;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos)
                eol = text.size();
            std::string line = text.substr(pos, eol - pos);
            pos = eol + 1;
            ++lineNo;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::vector<std::string> tok = Tokenize(line);
            if (tok.empty())
                continue;
            if (tok[0] == "seed") {
                if (haveSeed || order != 0 || tok.size() != 2 || !ParseU64(tok[1], seed)) {
                    error = LineError(lineNo, haveSeed ? "duplicate seed" :
                        (order != 0 ? "seed must come before the first entry" : "bad seed"));
                    return false;
                }
                haveSeed = true;
                continue;
            }
            if (tok[0] != "at" || tok.size() < 3) {
                error = LineError(lineNo, "expected 'at <time> <kind> [args]'");
                return false;
            }
            size_t k = 2;
            bool isSeq = false;
            double tSec = 0.0;
            uint64_t tSeq = 0;
            if (tok[1] == "seq") {
                isSeq = true;
                if (tok.size() < 4 || !ParseU64(tok[2], tSeq) || tSeq > kMaxSeq) {
                    error = LineError(lineNo, "bad sequence number");
                    return false;
                }
                k = 3;
            } else if (tok[1][0] == '#') {
                isSeq = true;
                if (!ParseU64(tok[1].substr(1), tSeq) || tSeq > kMaxSeq) {
                    error = LineError(lineNo, "bad sequence number");
                    return false;
                }
            } else if (!ParseSeconds(tok[1], tSec)) {
                error = LineError(lineNo, "bad time '" + tok[1] + "' (use <number>s, #<n> or seq <n>)");
                return false;
            }
            if (isSeq ? (!seq.empty() && tSeq < lastSeq) : (!sec.empty() && tSec < lastSec)) {
                error = LineError(lineNo, "time goes backwards within its clock");
                return false;
            }
            if (sec.size() + seq.size() >= kMaxScheduleEntries) {
                error = LineError(lineNo, "too many entries (max 100000)");
                return false;
            }
            std::string args;
            for (size_t i = k; i < tok.size(); ++i) {
                if (i > k)
                    args += ' ';
                args += tok[i];
            }
            Entry e;
            e.order = order++;
            e.sec = tSec;
            e.seq = tSeq;
            const std::string err = ParseArgs(args, e.req);
            if (!err.empty()) {
                error = LineError(lineNo, err.compare(0, 7, "error: ") == 0 ? err.substr(7) : err);
                return false;
            }
            e.req.injected = true;
            e.req.line = lineNo;
            if (isSeq) {
                lastSeq = tSeq;
                seq.push_back(e);
            } else {
                lastSec = tSec;
                sec.push_back(e);
            }
        }
        std::lock_guard<std::mutex> lock(m_schedMutex);
        m_secEntries.swap(sec);
        m_seqEntries.swap(seq);
        m_secCursor = m_seqCursor = 0;
        m_seed = seed;
        m_blocked = 0;
        m_scheduleLoaded = true;
        error.clear();
        return true;
    }

    // Moves every due entry (time <= sourceSeconds, or seq <= seq) into the pending queue, in file
    // order; returns how many moved. A full queue stops the release (no overtaking): the due entries
    // left behind stay scheduled and are added to ScheduleBlocked.
    size_t Release(double sourceSeconds, uint64_t seq)
    {
        if (!m_armed)
            return 0;
        std::lock_guard<std::mutex> lock(m_schedMutex);
        size_t released = 0;
        for (;;) {
            const bool aDue = m_secCursor < m_secEntries.size() && m_secEntries[m_secCursor].sec <= sourceSeconds;
            const bool bDue = m_seqCursor < m_seqEntries.size() && m_seqEntries[m_seqCursor].seq <= seq;
            if (!aDue && !bDue)
                break;
            const bool useA = aDue && (!bDue || m_secEntries[m_secCursor].order < m_seqEntries[m_seqCursor].order);
            const Entry& e = useA ? m_secEntries[m_secCursor] : m_seqEntries[m_seqCursor];
            if (!Enqueue(e.req)) {
                size_t blocked = 0;
                for (size_t i = m_secCursor; i < m_secEntries.size() && m_secEntries[i].sec <= sourceSeconds; ++i)
                    ++blocked;
                for (size_t i = m_seqCursor; i < m_seqEntries.size() && m_seqEntries[i].seq <= seq; ++i)
                    ++blocked;
                m_blocked += blocked;
                break;
            }
            if (useA) ++m_secCursor; else ++m_seqCursor;
            ++released;
        }
        return released;
    }

    size_t ScheduleRemaining() const
    {
        std::lock_guard<std::mutex> lock(m_schedMutex);
        return RemainingLocked();
    }
    uint64_t ScheduleBlocked() const
    {
        std::lock_guard<std::mutex> lock(m_schedMutex);
        return m_blocked;
    }
    uint64_t ScheduleSeed() const
    {
        std::lock_guard<std::mutex> lock(m_schedMutex);
        return m_seed;
    }

private:
    static constexpr uint64_t kMaxSeq = 999999999999999999ULL;

    struct Entry {
        size_t order = 0;      // file order across both clocks
        double sec = 0.0;
        uint64_t seq = 0;
        Request req;
    };

    size_t RemainingLocked() const
    {
        return (m_secEntries.size() - m_secCursor) + (m_seqEntries.size() - m_seqCursor);
    }

    static std::string LineError(uint32_t line, const std::string& what)
    {
        return "line " + std::to_string(line) + ": " + what;
    }

    // Whitespace tokens; a '#' starts a comment, except the sequence time token "#<digits>" directly
    // after the leading "at".
    static std::vector<std::string> Tokenize(const std::string& s)
    {
        std::vector<std::string> out;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == ' ' || s[i] == '\t') {
                ++i;
                continue;
            }
            if (s[i] == '#') {
                const bool seqTime = out.size() == 1 && out[0] == "at" && i + 1 < s.size() &&
                    s[i + 1] >= '0' && s[i + 1] <= '9';
                if (!seqTime)
                    break;
            }
            const size_t start = i;
            while (i < s.size() && s[i] != ' ' && s[i] != '\t')
                ++i;
            out.push_back(s.substr(start, i - start));
        }
        return out;
    }

    static bool ParseU64(const std::string& s, uint64_t& out)
    {
        if (s.empty() || s.size() > 20)
            return false;
        uint64_t v = 0;
        for (char c : s) {
            if (c < '0' || c > '9')
                return false;
            const uint64_t d = static_cast<uint64_t>(c - '0');
            if (v > (UINT64_MAX - d) / 10u)
                return false;
            v = v * 10u + d;
        }
        out = v;
        return true;
    }

    // "<digits>[.<digits>]s": at most 9 integer and 6 fractional digits. Integer arithmetic, so the
    // value is deterministic.
    static bool ParseSeconds(const std::string& s, double& out)
    {
        if (s.size() < 2 || s.back() != 's')
            return false;
        const std::string n = s.substr(0, s.size() - 1);
        const size_t dot = n.find('.');
        const std::string ip = dot == std::string::npos ? n : n.substr(0, dot);
        const std::string fp = dot == std::string::npos ? std::string() : n.substr(dot + 1);
        if (ip.empty() || ip.size() > 9 || (dot != std::string::npos && (fp.empty() || fp.size() > 6)))
            return false;
        uint64_t whole = 0, frac = 0, scale = 1;
        if (!ParseU64(ip, whole))
            return false;
        if (!fp.empty()) {
            if (!ParseU64(fp, frac))
                return false;
            for (size_t i = 0; i < fp.size(); ++i)
                scale *= 10u;
        }
        out = static_cast<double>(whole) + static_cast<double>(frac) / static_cast<double>(scale);
        return true;
    }

    // Parses "kind [arg]" into req. "" on success, else the "error: ..." text Invoke returns.
    static std::string ParseArgs(const std::string& args, Request& req)
    {
        std::string kindText, numText, extra;
        if (!Split(args, kindText, numText, extra))
            return "error: missing kind";
        if (!extra.empty())
            return "error: too many arguments";
        enum class Arg { None, Number, OptNumber, Word } arg = Arg::None;
        if (kindText == "seek") req.kind = Kind::Seek;
        else if (kindText == "reset") req.kind = Kind::Reset;
        else if (kindText == "inplace") req.kind = Kind::InPlace;
        else if (kindText == "rebuild") req.kind = Kind::Rebuild;
        else if (kindText == "capture_restart") req.kind = Kind::CaptureRestart;
        else if (kindText == "rpu_return") req.kind = Kind::RpuReturn;
        else if (kindText == "tunnel_handoff") req.kind = Kind::TunnelHandoff;
        else if (kindText == "signal_return") req.kind = Kind::SignalReturn;
        else if (kindText == "full") req.kind = Kind::Full;
        else if (kindText == "gap") { req.kind = Kind::Gap; arg = Arg::Number; }
        else if (kindText == "rate") { req.kind = Kind::Rate; arg = Arg::Number; }
        else if (kindText == "stall") { req.kind = Kind::Stall; arg = Arg::Number; }
        else if (kindText == "rpu_loss") { req.kind = Kind::RpuLoss; arg = Arg::Number; }
        else if (kindText == "signal_loss") { req.kind = Kind::SignalLoss; arg = Arg::Number; }
        else if (kindText == "overflow") { req.kind = Kind::Overflow; arg = Arg::Number; }
        else if (kindText == "minor") { req.kind = Kind::Minor; arg = Arg::OptNumber; }
        else if (kindText == "dv_switch") { req.kind = Kind::DvSwitch; arg = Arg::Word; }
        else
            return "error: unknown kind '" + kindText + "'";
        req.value = 0;

        if (arg == Arg::Word) {
            if (numText.empty())
                return "error: dv_switch needs a target (tunnel|lldv|hdr10|sdr)";
            if (numText == "tunnel") req.value = static_cast<int64_t>(DvTarget::Tunnel);
            else if (numText == "lldv") req.value = static_cast<int64_t>(DvTarget::Lldv);
            else if (numText == "hdr10") req.value = static_cast<int64_t>(DvTarget::Hdr10);
            else if (numText == "sdr") req.value = static_cast<int64_t>(DvTarget::Sdr);
            else
                return "error: bad dv target '" + numText + "' (tunnel|lldv|hdr10|sdr)";
        } else if (arg != Arg::None && !(arg == Arg::OptNumber && numText.empty())) {
            if (numText.empty())
                return "error: " + kindText + " needs a number";
            int64_t n = 0;
            if (!ParseInt(numText, n))
                return "error: bad number '" + numText + "'";
            if (req.kind == Kind::Rate) {
                if (n < -kMaxRatePpm || n > kMaxRatePpm)
                    return "error: rate ppm out of range (+-1000000)";
            } else if (n <= 0) {
                return "error: " + kindText + " must be a positive integer";
            } else if ((req.kind == Kind::Gap || req.kind == Kind::Minor) && n > kMaxGap) {
                return "error: " + kindText + " out of range (max 1000)";
            } else if (req.kind == Kind::Stall && n > kMaxStallMs) {
                return "error: stall out of range (max 60000 ms)";
            } else if (req.kind == Kind::RpuLoss && n > kMaxRpuLossFrames) {
                return "error: rpu_loss out of range (max 100000 frames)";
            } else if (req.kind == Kind::SignalLoss && n > kMaxSignalLossMs) {
                return "error: signal_loss out of range (max 600000 ms)";
            } else if (req.kind == Kind::Overflow && n > kMaxOverflow) {
                return "error: overflow out of range (max 1000)";
            }
            req.value = n;
        } else if (!numText.empty()) {
            return "error: " + kindText + " takes no number";
        }
        return std::string();
    }

    static void AddSaturating(std::atomic<uint32_t>& a, uint32_t n)
    {
        uint32_t cur = a.load(std::memory_order_relaxed);
        for (;;) {
            const uint32_t next = (UINT32_MAX - cur < n) ? UINT32_MAX : cur + n;
            if (a.compare_exchange_weak(cur, next, std::memory_order_relaxed))
                return;
        }
    }

    // Queues a validated request and sets the adapter-side accessors; false (nothing applied) when full.
    bool Enqueue(const Request& req)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_count >= kQueueMax)
                return false;
            m_queue[(m_head + m_count) % kQueueMax] = req;
            ++m_count;
        }
        switch (req.kind) {
        case Kind::Gap: AddSaturating(m_gap, static_cast<uint32_t>(req.value)); break;
        case Kind::Rate: ApplyRate(req.value); break;
        case Kind::Stall: m_stallMs.store(static_cast<uint32_t>(req.value), std::memory_order_relaxed); break;
        case Kind::DvSwitch: m_dvSwitch.store(static_cast<int32_t>(req.value), std::memory_order_relaxed); break;
        case Kind::RpuLoss: AddSaturating(m_rpuLoss, static_cast<uint32_t>(req.value)); break;
        case Kind::RpuReturn:
            m_rpuLoss.store(0, std::memory_order_relaxed);
            m_rpuReturn.store(true, std::memory_order_relaxed);
            break;
        case Kind::TunnelHandoff: m_handoff.store(true, std::memory_order_relaxed); break;
        case Kind::SignalLoss: m_signalLossMs.store(static_cast<uint32_t>(req.value), std::memory_order_relaxed); break;
        case Kind::SignalReturn:
            m_signalLossMs.store(0, std::memory_order_relaxed);
            m_signalReturn.store(true, std::memory_order_relaxed);
            break;
        case Kind::Overflow: AddSaturating(m_overflow, static_cast<uint32_t>(req.value)); break;
        default: break;
        }
        return true;
    }

    static const char* DefaultGetenv(const char* name)
    {
        static thread_local std::string buffer;
        buffer.clear();
#ifdef _MSC_VER
        char* v = nullptr;
        size_t len = 0;
        if (_dupenv_s(&v, &len, name) != 0 || v == nullptr)
            return nullptr;
        buffer = v;
        std::free(v);
        return buffer.c_str();
#else
        const char* v = std::getenv(name);
        if (v == nullptr)
            return nullptr;
        buffer = v;
        return buffer.c_str();
#endif
    }

    // Up to three whitespace-separated tokens: kind, number, anything further (error).
    static bool Split(const std::string& s, std::string& a, std::string& b, std::string& rest)
    {
        size_t i = 0;
        auto token = [&](std::string& out) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
                ++i;
            const size_t start = i;
            while (i < s.size() && s[i] != ' ' && s[i] != '\t')
                ++i;
            out = s.substr(start, i - start);
        };
        token(a);
        token(b);
        token(rest);
        return !a.empty();
    }

    // Optional leading '-', then 1..9 digits (no overflow, no sign-only, no trailing junk).
    static bool ParseInt(const std::string& s, int64_t& out)
    {
        size_t i = 0;
        bool negative = false;
        if (i < s.size() && s[i] == '-') {
            negative = true;
            ++i;
        }
        if (i >= s.size() || s.size() - i > 9)
            return false;
        int64_t v = 0;
        for (; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9')
                return false;
            v = v * 10 + (s[i] - '0');
        }
        out = negative ? -v : v;
        return true;
    }

    bool m_armed = false;
    std::mutex m_mutex;
    Request m_queue[kQueueMax];
    size_t m_head = 0;
    size_t m_count = 0;
    std::atomic<uint32_t> m_gap{ 0 };
    std::atomic<uint32_t> m_stallMs{ 0 };
    std::atomic<int64_t> m_ratePpm{ 0 };
    std::atomic<int32_t> m_dvSwitch{ -1 };
    std::atomic<uint32_t> m_rpuLoss{ 0 };
    std::atomic<bool> m_rpuReturn{ false };
    std::atomic<bool> m_handoff{ false };
    std::atomic<uint32_t> m_signalLossMs{ 0 };
    std::atomic<bool> m_signalReturn{ false };
    std::atomic<uint32_t> m_overflow{ 0 };

    mutable std::mutex m_schedMutex;
    std::vector<Entry> m_secEntries;   // m_schedMutex; ascending seconds
    std::vector<Entry> m_seqEntries;   // m_schedMutex; ascending sequence
    size_t m_secCursor = 0;            // m_schedMutex
    size_t m_seqCursor = 0;            // m_schedMutex
    uint64_t m_seed = 0;               // m_schedMutex
    uint64_t m_blocked = 0;            // m_schedMutex
    bool m_scheduleLoaded = false;     // m_schedMutex
};

} // namespace VpResync
