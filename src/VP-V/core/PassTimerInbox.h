// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "PassTimers.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// MV7: GPU jobs of the motion engine run on threads of their own (the optical-flow producer's worker, the long-pair service), where
// the render thread's PassTimerHook accumulator cannot be written from. They POST their completed measurements here (any thread,
// never waits on the GPU: the producer's own timed run already waited on ITS device); the render thread drains the queue into the
// hook's accumulator (DrainInto: one generic host call at the existing timer site) so the figures appear in the Ctrl+Alt+D timer
// readout beside the passes, and the skeleton shows the cumulative figures itself (Lines) without any host call. Samples are plain
// PassTimers::Accumulator samples (a GPU kind with a zero or negative time is not a sample). The queue is bounded: with nobody
// draining it the oldest sample is dropped and counted, never grown. Header-only, C++14.
namespace PassTimers {

// Row names the benchmark and the readout key on (PassTimers keys rows by name).
namespace Rows {
constexpr const char* kCameraFit = "camera.fit";              // CPU work: the camera fit on the camera worker
constexpr const char* kCameraReadback = "camera.readback";    // CPU wait: the field copy's request -> ready latency (never counted as work)
}


class Inbox {
public:
    static constexpr size_t kCapacity = 256;

    // The one inbox of the motion jobs (names start with "motion: ").
    static Inbox& Motion()
    {
        static Inbox inbox;
        return inbox;
    }

    // Any thread. `ms` is an already-completed measurement.
    void Post(Kind kind, const std::string& name, double ms)
    {
        if (ms <= 0.0 && (kind == Kind::GpuPass || kind == Kind::GpuGroup))
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        total_.Add(kind, name, ms);
        bool known = false;
        for (const std::string& n : names_)
            known = known || n == name;
        if (!known)
            names_.push_back(name);
        if (queue_.size() >= kCapacity) {
            queue_.pop_front();
            ++dropped_;
        }
        Sample s;
        s.kind = kind;
        s.name = name;
        s.ms = ms;
        queue_.push_back(s);
        ++posted_;
    }

    // The camera's two CPU rows (SK-W stage D): the fit on the camera worker (work) and the field readback latency (a wait).
    void PostCameraFit(double ms) { Post(Kind::CpuWork, Rows::kCameraFit, ms); }
    void PostCameraReadback(double ms) { Post(Kind::CpuWait, Rows::kCameraReadback, ms); }

    // Render thread: moves the queued samples into `acc` (the hook's accumulator); returns how many.
    size_t DrainInto(Accumulator& acc)
    {
        std::deque<Sample> taken;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            taken.swap(queue_);
        }
        for (const Sample& s : taken)
            acc.Add(s.kind, s.name, s.ms);
        return taken.size();
    }

    // The cumulative figures, plain words, one line per job ("motion: NVOF producer engine ...: 1.234 ms avg, 2.100 max (12 samples)").
    // Nothing (an empty vector) until a job has posted.
    std::vector<std::string> Lines() const
    {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& n : names_) {
            const Stat* s = total_.Find(n);
            if (s == nullptr || s->samples == 0)
                continue;
            char buf[160] = {};
            std::snprintf(buf, sizeof(buf), ": %.3f ms avg, %.3f max (%llu sample(s))", s->MeanMs(), s->maxMs,
                          static_cast<unsigned long long>(s->samples));
            out.push_back(n + buf);
        }
        return out;
    }

    uint64_t Posted() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return posted_;
    }
    uint64_t Dropped() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    // Tests only (the inbox is process-wide).
    void ResetForTesting()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        names_.clear();
        total_.Clear();
        posted_ = dropped_ = 0;
    }

private:
    struct Sample {
        Kind kind = Kind::GpuGroup;
        std::string name;
        double ms = 0.0;
    };
    mutable std::mutex mutex_;
    std::deque<Sample> queue_;
    std::vector<std::string> names_;
    Accumulator total_;
    uint64_t posted_ = 0;
    uint64_t dropped_ = 0;
};

} // namespace PassTimers
