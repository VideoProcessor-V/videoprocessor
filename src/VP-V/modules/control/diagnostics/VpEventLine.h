// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

// "vp-event/1": one structured, machine-parseable log line per event.
//
//   VPEV1 t=<steady-clock ms, 3 decimals> kind=<kind> key=value key=value ...
//
// - t is std::chrono::steady_clock in milliseconds (QPC on MSVC, the clock StreamRecorder uses).
// - Keys match [a-z0-9_]+. Fields keep the emitter's order.
// - A value is bare when it matches [A-Za-z0-9_.:+/-]+, else it is double-quoted with backslash-escaped quote
//   and backslash characters. Control characters (CR, LF, TAB, ...) become one space each BEFORE the quoting
//   decision. An empty value is "".
// - A line is at most 1000 characters: the LAST value is truncated and trunc=1 is appended; the
//   line stays valid.
//
// Kinds (closed list) and their fields ([] = optional):
//   session        build_id [commit]
//   resync         level cause seq [gap] [detail] mode
//   fallback       state=engaged|released id [reason]
//   gate           from to reason
//   gpu_error      api code [what]
//   source_change  w h rate_num rate_den transfer [dv]
//   cadence        state pattern [phase]
//   repeat         cause count
//   frame_stats    window_s n p50_ms p99_ms max_ms
//   flag           source tag
//   stamp          dir index
//
// Header-only, pure C++14, no Windows headers, locale-independent and deterministic. The process-
// wide sink is null by default: Emit() then costs one atomic load and changes no behaviour.
namespace VpEvent {

enum class Kind : uint8_t {
    session, resync, fallback, gate, gpu_error, source_change, cadence, repeat, frame_stats, stamp, flag
};

constexpr size_t kKindCount = 11;
constexpr size_t kMaxLineChars = 1000;

inline const char* KindToken(Kind k)
{
    switch (k) {
    case Kind::session: return "session";
    case Kind::resync: return "resync";
    case Kind::fallback: return "fallback";
    case Kind::gate: return "gate";
    case Kind::gpu_error: return "gpu_error";
    case Kind::source_change: return "source_change";
    case Kind::cadence: return "cadence";
    case Kind::repeat: return "repeat";
    case Kind::frame_stats: return "frame_stats";
    case Kind::stamp: return "stamp";
    case Kind::flag: return "flag";
    }
    return "flag";
}

inline bool KindFromToken(const char* token, Kind& out)
{
    if (!token)
        return false;
    for (size_t i = 0; i < kKindCount; ++i) {
        const Kind k = static_cast<Kind>(i);
        const char* t = KindToken(k);
        size_t j = 0;
        while (t[j] && token[j] == t[j])
            ++j;
        if (t[j] == '\0' && token[j] == '\0') {
            out = k;
            return true;
        }
    }
    return false;
}

namespace detail {

inline std::string UnsignedText(uint64_t v)
{
    char buf[24];
    size_t n = 0;
    do {
        buf[n++] = static_cast<char>('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    std::string s;
    while (n)
        s += buf[--n];
    return s;
}

inline std::string SignedText(int64_t v)
{
    if (v >= 0)
        return UnsignedText(static_cast<uint64_t>(v));
    return "-" + UnsignedText(~static_cast<uint64_t>(v) + 1u);
}

// Fixed 3 decimals, round half away from zero, integer arithmetic only (locale independent).
inline std::string DoubleText(double v)
{
    if (v != v)
        return "nan";
    if (v > 9.0e15)
        return "inf";
    if (v < -9.0e15)
        return "-inf";
    const bool negative = v < 0.0;
    const double a = negative ? -v : v;
    const uint64_t milli = static_cast<uint64_t>(a * 1000.0 + 0.5);
    std::string frac = UnsignedText(milli % 1000u);
    while (frac.size() < 3)
        frac.insert(frac.begin(), '0');
    std::string s = UnsignedText(milli / 1000u) + "." + frac;
    // "-0.000" is not a meaningful value.
    if (negative && milli != 0)
        s.insert(s.begin(), '-');
    return s;
}

inline bool IsBareChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '_' || c == '.' || c == ':' || c == '+' || c == '/' || c == '-';
}

inline std::string Sanitize(const std::string& raw)
{
    std::string s = raw;
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            c = ' ';
    return s;
}

// `clean` is already sanitized.
inline std::string Encode(const std::string& clean)
{
    bool bare = !clean.empty();
    for (char c : clean)
        if (!IsBareChar(c)) {
            bare = false;
            break;
        }
    if (bare)
        return clean;
    std::string q = "\"";
    for (char c : clean) {
        if (c == '"' || c == '\\')
            q += '\\';
        q += c;
    }
    q += '"';
    return q;
}

inline std::string CleanKey(const std::string& key)
{
    std::string k;
    for (char c : key) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        k += ok ? c : '_';
    }
    return k.empty() ? std::string("_") : k;
}

} // namespace detail

struct Field {
    std::string key;
    std::string value;

    Field(const char* k, const char* v) : key(k ? k : ""), value(v ? v : "") {}
    Field(const char* k, const std::string& v) : key(k ? k : ""), value(v) {}
    Field(const char* k, int64_t v) : key(k ? k : ""), value(detail::SignedText(v)) {}
    Field(const char* k, uint64_t v) : key(k ? k : ""), value(detail::UnsignedText(v)) {}
    Field(const char* k, int v) : key(k ? k : ""), value(detail::SignedText(v)) {}
    Field(const char* k, unsigned v) : key(k ? k : ""), value(detail::UnsignedText(v)) {}
    Field(const char* k, double v) : key(k ? k : ""), value(detail::DoubleText(v)) {}
};

inline double NowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// truncIndex: which field's value is truncated when the line is too long (out of range = the last).
inline std::string FormatRange(Kind kind, double tMs, const Field* fields, size_t count,
                               size_t truncIndex = static_cast<size_t>(-1))
{
    const std::string head = "VPEV1 t=" + detail::DoubleText(tMs) + " kind=" + KindToken(kind);
    std::vector<std::string> keys;
    std::vector<std::string> clean;
    std::vector<std::string> parts;   // encoded values
    keys.reserve(count);
    clean.reserve(count);
    parts.reserve(count);
    size_t total = head.size();
    for (size_t i = 0; i < count; ++i) {
        keys.push_back(" " + detail::CleanKey(fields[i].key) + "=");
        clean.push_back(detail::Sanitize(fields[i].value));
        parts.push_back(detail::Encode(clean.back()));
        total += keys.back().size() + parts.back().size();
    }
    bool truncated = false;
    if (total > kMaxLineChars && count > 0) {
        const size_t t = (truncIndex < count) ? truncIndex : count - 1;
        const size_t suffix = 8;   // " trunc=1"
        const size_t others = total - parts[t].size();
        const size_t budget = kMaxLineChars > others + suffix ? kMaxLineChars - others - suffix : 0;
        std::string cut = clean[t];
        if (cut.size() > budget)
            cut.resize(budget);
        std::string enc = detail::Encode(cut);
        while (enc.size() > budget && !cut.empty()) {
            cut.resize(cut.size() - 1);
            enc = detail::Encode(cut);
        }
        parts[t] = enc;   // at worst `""`
        truncated = true;
    }
    std::string line = head;
    for (size_t i = 0; i < count; ++i) {
        line += keys[i];
        line += parts[i];
    }
    if (truncated)
        line += " trunc=1";
    return line;
}

inline std::string Format(Kind kind, double tMs, std::initializer_list<Field> fields)
{
    return FormatRange(kind, tMs, fields.begin(), fields.size());
}

// Rate limiter: per kind, at most N allowed lines in any rolling second. The next allowed line
// of a kind reports how many were suppressed since the previous allowed one. Deterministic given
// the time values passed (no clock inside). Thread-safe.
class Limiter {
public:
    static constexpr uint32_t kDefaultPerSecond = 20;
    static constexpr uint32_t kMaxPerSecond = 64;

    explicit Limiter(uint32_t perSecond = kDefaultPerSecond)
        : m_n(perSecond == 0 ? 1u : (perSecond > uint32_t(kMaxPerSecond) ? uint32_t(kMaxPerSecond) : perSecond))
    {
    }

    uint32_t PerSecond() const { return m_n; }

    // true: emit now; suppressedOut = lines of this kind dropped since the last allowed one.
    bool Allow(Kind kind, double tMs, uint32_t& suppressedOut)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Slot& s = m_slots[static_cast<size_t>(kind) % kKindCount];
        if (s.filled >= m_n) {
            const double oldest = s.times[s.next];   // the N-th most recent allowed line
            if (!(tMs - oldest >= 1000.0)) {
                ++s.suppressed;
                suppressedOut = 0;
                return false;
            }
        }
        s.times[s.next] = tMs;
        s.next = (s.next + 1u) % m_n;
        if (s.filled < m_n)
            ++s.filled;
        suppressedOut = s.suppressed;
        s.suppressed = 0;
        return true;
    }

    // The formatted line (with suppressed=<n> appended when n > 0), or an empty string when the
    // line is rate-limited.
    std::string FormatLimited(Kind kind, double tMs, std::initializer_list<Field> fields)
    {
        uint32_t suppressed = 0;
        if (!Allow(kind, tMs, suppressed))
            return std::string();
        if (suppressed == 0)
            return Format(kind, tMs, fields);
        std::vector<Field> all(fields.begin(), fields.end());
        const size_t original = all.size();
        all.emplace_back("suppressed", suppressed);
        return FormatRange(kind, tMs, all.data(), all.size(), original ? original - 1 : static_cast<size_t>(-1));
    }

private:
    struct Slot {
        double times[kMaxPerSecond] = {};
        uint32_t next = 0;
        uint32_t filled = 0;
        uint32_t suppressed = 0;
    };

    uint32_t m_n;
    std::mutex m_mutex;
    Slot m_slots[kKindCount];
};

// Process-wide sink. It must not throw. Null (the default) = no-op.
using SinkFn = void (*)(const char*);

inline std::atomic<SinkFn>& SinkSlot()
{
    static std::atomic<SinkFn> slot{nullptr};
    return slot;
}

inline void SetSink(SinkFn sink) { SinkSlot().store(sink, std::memory_order_release); }

// Formats only when a sink is set; never throws.
inline void Emit(Kind kind, std::initializer_list<Field> fields)
{
    const SinkFn sink = SinkSlot().load(std::memory_order_acquire);
    if (!sink)
        return;
    try {
        const std::string line = Format(kind, NowMs(), fields);
        sink(line.c_str());
    } catch (...) {
    }
}

} // namespace VpEvent
