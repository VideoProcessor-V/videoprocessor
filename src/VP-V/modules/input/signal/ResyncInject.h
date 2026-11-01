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

// Test-only fault injection for the resync work (Q0-A). ARMED only when the environment variable
// VP_RESYNC_INJECT is exactly "1" (read ONCE in the constructor); unarmed it is inert: every command
// answers "not armed", Poll is false and every accessor returns a neutral value.
//
// Command "resync.inject", args "kind [n]":
//   seek | reset | inplace | rebuild | capture_restart   no number (a number is an error)
//   gap n      drop n capture frames (n = positive integer, <= kMaxGap = 1000)
//   rate ppm   scale capture timestamps by 1 + ppm * 1e-6 (integer ppm, -1,000,000..+1,000,000;
//              0 restores 1.0)
//   stall ms   stall the capture thread n ms (positive integer, <= kMaxStallMs = 60000)
// Every valid request is queued for the host (bounded, kQueueMax = 16; a full queue rejects with
// "error: queue full" and applies nothing). gap, rate and stall ALSO set the adapter-side accessors
// at Invoke time (TakeGapFramesToDrop, TimestampScale, TakeStallMs); the queue entry tells the host
// the request happened (log / Ctrl+I). Header-only, pure C++14. Invoke may run on any thread.
namespace VpResync {

class ResyncInject {
public:
    enum class Kind : uint8_t { Seek, Reset, InPlace, Rebuild, Gap, Rate, Stall, CaptureRestart };

    struct Request {
        Kind kind = Kind::Seek;
        int64_t value = 0;   // gap frames, rate ppm or stall ms; 0 for the others
    };

    static constexpr size_t kQueueMax = 16;
    static constexpr int64_t kMaxGap = 1000;
    static constexpr int64_t kMaxRatePpm = 1000000;
    static constexpr int64_t kMaxStallMs = 60000;

    using Getter = std::function<const char*(const char*)>;

    explicit ResyncInject(Getter getter = Getter())
    {
        const char* v = getter ? getter("VP_RESYNC_INJECT") : DefaultGetenv("VP_RESYNC_INJECT");
        m_armed = v != nullptr && std::strcmp(v, "1") == 0;
    }

    bool Armed() const { return m_armed; }

    // "resync injection armed" when armed, else "" (no status row).
    std::string StatusText() const { return m_armed ? "resync injection armed" : ""; }

    std::string Invoke(const std::string& command, const std::string& args)
    {
        if (!m_armed)
            return "not armed";
        if (command != "resync.inject")
            return "error: unknown command";
        std::string kindText, numText, extra;
        if (!Split(args, kindText, numText, extra))
            return "error: missing kind";
        if (!extra.empty())
            return "error: too many arguments";
        Request req;
        bool needsNumber = false;
        if (kindText == "seek") req.kind = Kind::Seek;
        else if (kindText == "reset") req.kind = Kind::Reset;
        else if (kindText == "inplace") req.kind = Kind::InPlace;
        else if (kindText == "rebuild") req.kind = Kind::Rebuild;
        else if (kindText == "capture_restart") req.kind = Kind::CaptureRestart;
        else if (kindText == "gap") { req.kind = Kind::Gap; needsNumber = true; }
        else if (kindText == "rate") { req.kind = Kind::Rate; needsNumber = true; }
        else if (kindText == "stall") { req.kind = Kind::Stall; needsNumber = true; }
        else
            return "error: unknown kind '" + kindText + "'";

        if (needsNumber) {
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
            } else if (req.kind == Kind::Gap && n > kMaxGap) {
                return "error: gap out of range (max 1000)";
            } else if (req.kind == Kind::Stall && n > kMaxStallMs) {
                return "error: stall out of range (max 60000 ms)";
            }
            req.value = n;
        } else if (!numText.empty()) {
            return "error: " + kindText + " takes no number";
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_count >= kQueueMax)
                return "error: queue full";
            m_queue[(m_head + m_count) % kQueueMax] = req;
            ++m_count;
        }
        switch (req.kind) {
        case Kind::Gap: {
            const uint32_t n = static_cast<uint32_t>(req.value);
            m_gap.fetch_add(n, std::memory_order_relaxed);
            break;
        }
        case Kind::Rate: ApplyRate(req.value); break;
        case Kind::Stall: m_stallMs.store(static_cast<uint32_t>(req.value), std::memory_order_relaxed); break;
        default: break;
        }
        return "ok: queued " + kindText;
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

    // Sets the timestamp scale to 1 + ppm * 1e-6 (done by Invoke for "rate"); ignored unarmed.
    void ApplyRate(int64_t ppm)
    {
        if (!m_armed)
            return;
        if (ppm < -kMaxRatePpm) ppm = -kMaxRatePpm;
        if (ppm > kMaxRatePpm) ppm = kMaxRatePpm;
        m_ratePpm.store(ppm, std::memory_order_relaxed);
    }

private:
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
};

} // namespace VpResync
