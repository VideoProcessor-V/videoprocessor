// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Windowed measured frame rate (bug hunt 35 #3, audit 25a R5 MUST, 2026-10-31).
//
// Why: the renderer's measured source rate is a cumulative average since the last timestamp break
// ((frames - 1) / (now - first)), published every 5 s. After minutes it moves by under 1 ppm per
// second, so the resync detector's RateStep (a 500 ppm step between successive measurements) never
// fires for a genuine unannounced rate move (59.94 <-> 60, 24p-in-60 -> 60p inside one HDMI mode)
// once the session is a few minutes old.
//
// This estimator keeps only the frames of the last `windowSeconds` (a time constant, not a rate:
// the frame count in the window follows the measured rate) and fits the frame period by least
// squares over them (frame index against timestamp), so arrival jitter averages out and a step is
// seen within about half a window whatever the session length. Breaks (a gap, a FULL resync, a
// timestamp step) restart it. Bounded storage: a fixed ring (capacity frames; at very high rates the
// window is the last `capacity` frames instead). Pure, deterministic, header-only C++14; fixed
// summation order (oldest to newest).
#include <cmath>
#include <cstddef>
#include <vector>

namespace VpTiming {

class WindowedRate {
public:
    static constexpr double kDefaultWindowSeconds = 10.0;
    static constexpr size_t kDefaultCapacity = 4096;

    explicit WindowedRate(double windowSeconds = kDefaultWindowSeconds, size_t capacity = kDefaultCapacity)
        : m_window(windowSeconds > 0.0 && std::isfinite(windowSeconds) ? windowSeconds : kDefaultWindowSeconds),
          m_times(capacity < 3 ? 3 : capacity, 0.0)
    {
    }

    void Reset()
    {
        m_head = 0;
        m_count = 0;
    }

    // One frame's timestamp in seconds (any origin; monotonic within a run). A timestamp that is not
    // finite or not after the previous one restarts the window from it (a break the caller missed).
    void Observe(double seconds)
    {
        if (!std::isfinite(seconds))
        {
            Reset();
            return;
        }
        if (m_count > 0 && !(seconds > Newest()))
            Reset();
        // A frame missing without a flagged gap (the fit assumes consecutive frames): an interval
        // beyond 1.5 periods of the window's own rate restarts it (audit 2026-10-31).
        else if (m_count >= 3 && seconds - Newest() > 1.5 * SpanSeconds() / static_cast<double>(m_count - 1))
            Reset();
        const size_t cap = m_times.size();
        m_times[(m_head + m_count) % cap] = seconds;
        if (m_count < cap)
            ++m_count;
        else
            m_head = (m_head + 1) % cap;
        // Older than the window: dropped (the newest always stays).
        while (m_count > 2 && seconds - m_times[m_head] > m_window)
        {
            m_head = (m_head + 1) % cap;
            --m_count;
        }
    }

    size_t Samples() const { return m_count; }
    double SpanSeconds() const { return m_count >= 2 ? Newest() - m_times[m_head] : 0.0; }
    double WindowSeconds() const { return m_window; }

    // The window is full (it spans the window, or holds `capacity` frames) and the fit is usable.
    bool Valid() const
    {
        if (m_count < 3)
            return false;
        // The window is full when the frame just past it was dropped: its span plus one frame
        // period reaches the window (or the ring is full at a very high rate).
        const double span = SpanSeconds();
        const double period = span / static_cast<double>(m_count - 1);
        return (m_count == m_times.size() || span + period >= m_window) && RateHz() > 0.0;
    }

    // Least-squares frame rate over the window (frames per second); 0 when fewer than 3 frames.
    double RateHz() const
    {
        if (m_count < 3)
            return 0.0;
        const size_t cap = m_times.size();
        const double n = static_cast<double>(m_count);
        const double meanX = (n - 1.0) * 0.5;
        const double origin = m_times[m_head];
        double meanY = 0.0;
        for (size_t i = 0; i < m_count; ++i)
            meanY += m_times[(m_head + i) % cap] - origin;
        meanY /= n;
        double sxy = 0.0, sxx = 0.0;
        for (size_t i = 0; i < m_count; ++i)
        {
            const double dx = static_cast<double>(i) - meanX;
            sxy += dx * (m_times[(m_head + i) % cap] - origin - meanY);
            sxx += dx * dx;
        }
        if (!(sxx > 0.0) || !(sxy > 0.0))
            return 0.0;
        const double period = sxy / sxx;
        return std::isfinite(period) && period > 0.0 ? 1.0 / period : 0.0;
    }

private:
    double Newest() const { return m_times[(m_head + m_count - 1) % m_times.size()]; }

    double m_window;
    std::vector<double> m_times;
    size_t m_head = 0;
    size_t m_count = 0;
};

} // namespace VpTiming
