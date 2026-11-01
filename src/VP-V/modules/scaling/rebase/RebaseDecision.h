// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

// The re-base decision product P0 (R4a, design 25 v2 §A4): per source frame, the width (H) and
// height (V) factors, the kernel, and each axis's crossfade, timed in SOURCE time (the frame's
// sequence x the source period), not on the wall clock, so the same frames give the same weights.
//
// Per axis: only the axis whose factor changes re-latches and fades; the other axis keeps its value
// and its fade. A FULL resync goes native with no fade (Reset). The renderer blends two pictures
// (the path being left and the new one, one weight): Blend() folds the two axes into that form.
//
// Header-only, pure C++14, deterministic, render thread (one owner).
namespace VpRebase {

constexpr uint32_t kUnitVersion = 1;   // P0 re-base decision and P6 entrance output (core/ProductCatalog.h)

enum Axis : int { kH = 0, kV = 1, kAxes = 2 };

struct AxisDecision {
    double factor = 1.0;     // applied (the `to` of a fade)
    double from = 1.0;       // the factor being left while fading (== factor otherwise)
    double weight = 1.0;     // of `factor`: 1 = no fade in progress
    bool changed = false;    // the factor changed at this frame
    bool Fading() const { return weight < 1.0; }
};

struct Decision {
    uint64_t seq = 0;
    AxisDecision axis[kAxes];
    int kernel = -1;
    bool kernelChanged = false;

    // The two-picture blend the renderer draws: `from` = each fading axis's from, else its factor;
    // weight = the smallest weight of the fading axes (the later fade governs), 1 when none fades.
    struct Blend {
        double fromX = 1.0, fromY = 1.0;
        double weight = 1.0;
        bool Active() const { return weight < 1.0; }
    };
    Blend TwoPicture() const
    {
        Blend b;
        b.fromX = axis[kH].Fading() ? axis[kH].from : axis[kH].factor;
        b.fromY = axis[kV].Fading() ? axis[kV].from : axis[kV].factor;
        double w = 1.0;
        for (int a = 0; a < kAxes; ++a)
            if (axis[a].Fading() && axis[a].weight < w)
                w = axis[a].weight;
        b.weight = w;
        return b;
    }
    // The axes whose re-base stamp moves (factor change, or the kernel: it shapes both axes).
    bool StampH() const { return axis[kH].changed || kernelChanged; }
    bool StampV() const { return axis[kV].changed || kernelChanged; }
};

class DecisionTracker {
public:
    // A FULL resync: native, no fade, nothing carried.
    void Reset()
    {
        for (int a = 0; a < kAxes; ++a)
            m_axis[a] = AxisState();
        m_kernel = -1;
        m_haveKernel = false;
        m_lastSeq = 0;
    }

    // One call per rendered source frame (a cadence repeat of the same seq returns the same weights).
    // periodUs: the source frame period in microseconds (0 = unknown: a change is a hard switch).
    // fadeUsH / fadeUsV: the crossfade the decider asks for on that axis when it changes (0 = a hard
    // switch); ignored for an axis that does not change at this frame.
    Decision Update(uint64_t seq, double periodUs, double factorX, double factorY, int kernel,
                    double fadeUsH, double fadeUsV)
    {
        Decision d;
        d.seq = seq;
        const double wanted[kAxes] = { factorX, factorY };
        const double fadeUs[kAxes] = { fadeUsH, fadeUsV };
        const bool repeat = seq != 0 && seq == m_lastSeq;
        for (int a = 0; a < kAxes; ++a) {
            AxisState& s = m_axis[a];
            AxisDecision& out = d.axis[a];
            if (!repeat && !Same(wanted[a], s.factor)) {
                // Re-latch this axis. From what is on screen now: mid-fade the path being left stays
                // the `from` (as the monitor's own transit); a reversal to it leaves nothing to fade.
                const bool midFade = WeightOf(s, seq) < 1.0;
                const double from = midFade ? s.from : s.factor;
                s.factor = wanted[a];
                out.changed = true;
                if (fadeUs[a] > 0.0 && periodUs > 0.0 && seq != 0 && !Same(from, s.factor)) {
                    s.from = from;
                    s.fadeStartSeq = seq;
                    s.fadeUs = fadeUs[a];
                    s.periodUs = periodUs;
                } else {
                    s.from = s.factor;
                    s.fadeUs = 0.0;
                }
            }
            out.factor = s.factor;
            out.weight = WeightOf(s, seq);
            if (!(out.weight < 1.0)) {
                s.from = s.factor;   // the fade is over (or none)
                s.fadeUs = 0.0;
            }
            out.from = out.weight < 1.0 ? s.from : s.factor;
        }
        d.kernel = kernel;
        d.kernelChanged = !repeat && m_haveKernel && kernel != m_kernel;
        if (!repeat) {
            m_kernel = kernel;
            m_haveKernel = true;
        }
        if (seq != 0)
            m_lastSeq = seq;
        return d;
    }

private:
    struct AxisState {
        double factor = 1.0;
        double from = 1.0;
        uint64_t fadeStartSeq = 0;
        double fadeUs = 0.0;
        double periodUs = 0.0;
    };

    static bool Same(double a, double b) { return std::fabs(a - b) < 1e-9; }

    // Source time since the fade started: whole source frames x the period latched at its start.
    static double WeightOf(const AxisState& s, uint64_t seq)
    {
        if (!(s.fadeUs > 0.0) || seq < s.fadeStartSeq)
            return 1.0;
        const double elapsedUs = static_cast<double>(seq - s.fadeStartSeq) * s.periodUs;
        if (elapsedUs >= s.fadeUs)
            return 1.0;
        return elapsedUs / s.fadeUs;
    }

    AxisState m_axis[kAxes];
    int m_kernel = -1;
    bool m_haveKernel = false;
    uint64_t m_lastSeq = 0;
};

// The last P0 for the status line: written by the render thread, read by any thread (relaxed
// atomics; a line may mix two frames' values, display only).
class StatusCell {
public:
    void Publish(const Decision& d)
    {
        m_factor[kH].store(d.axis[kH].factor, std::memory_order_relaxed);
        m_factor[kV].store(d.axis[kV].factor, std::memory_order_relaxed);
        m_weight[kH].store(d.axis[kH].weight, std::memory_order_relaxed);
        m_weight[kV].store(d.axis[kV].weight, std::memory_order_relaxed);
        m_valid.store(true, std::memory_order_release);
    }
    void Clear() { m_valid.store(false, std::memory_order_release); }

    // "re-base H x0.667 (fading 40%), V native (source-time fade)"; false before any P0.
    bool Text(std::string& out) const
    {
        if (!m_valid.load(std::memory_order_acquire))
            return false;
        out = "re-base " + Axis("H", kH) + ", " + Axis("V", kV) + " (source-time fade)";
        return true;
    }

private:
    std::string Axis(const char* name, int a) const
    {
        const double f = m_factor[a].load(std::memory_order_relaxed);
        const double w = m_weight[a].load(std::memory_order_relaxed);
        char text[64];
        if (std::fabs(f - 1.0) < 1e-9)
            std::snprintf(text, sizeof(text), "%s native", name);
        else
            std::snprintf(text, sizeof(text), "%s x%.3f", name, f);
        std::string s = text;
        if (w < 1.0) {
            std::snprintf(text, sizeof(text), " (fading %d%%)", static_cast<int>(w * 100.0));
            s += text;
        }
        return s;
    }

    std::atomic<double> m_factor[kAxes] = { {1.0}, {1.0} };
    std::atomic<double> m_weight[kAxes] = { {1.0}, {1.0} };
    std::atomic<bool> m_valid{false};
};

} // namespace VpRebase
