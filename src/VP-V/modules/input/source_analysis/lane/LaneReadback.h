// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/LaneInterfaces.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Skeleton S6 (design B4, "non-blocking P5 readback"): a fixed ring of staging buffers. The render
// thread copies a finished GPU product into a Free staging buffer, polls an event query, and maps
// without waiting once it is done. Nothing here blocks: a busy map is retried later and counted.
// The staging count is fixed at build (the host derives it from the measured readback latency in
// frames + 1). Pure C++14; the host owns the device.
namespace VpLaneProducts {

using VpSkeleton::IReadbackDevice;
using VpSkeleton::ProductId;

struct ReadbackCounters {
    uint64_t submitted = 0;
    uint64_t skippedNoStaging = 0;   // no Free staging buffer: the frame is not read back (visible)
    uint64_t copyFailed = 0;
    uint64_t taken = 0;
    uint64_t mapBusy = 0;            // MapNoWait returned null: stays Ready, retried later
    uint64_t staleDropped = 0;       // an older Ready result freed when a newer one was taken
};

class ReadbackRing {
public:
    enum class State : uint8_t { Free, Copying, Ready };

    ReadbackRing(IReadbackDevice* device, uint8_t stagingCount) : device_(device), slots_(stagingCount) {}

    size_t Count() const { return slots_.size(); }
    State StateOf(size_t i) const { return slots_[i].state; }
    const ReadbackCounters& Counters() const { return counters_; }

    // Needs a Free buffer (lowest index first). False: none free (counted) or the copy failed.
    bool Submit(ProductId product, uint64_t seq)
    {
        if (device_ == nullptr)
            return false;
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].state != State::Free)
                continue;
            if (!device_->CopyToStaging(product, seq, int(i))) {
                ++counters_.copyFailed;
                return false;
            }
            slots_[i].state = State::Copying;
            slots_[i].seq = seq;
            ++counters_.submitted;
            return true;
        }
        ++counters_.skippedNoStaging;
        return false;
    }

    // Copying buffers whose event query is done become Ready (non-blocking).
    void Poll()
    {
        if (device_ == nullptr)
            return;
        for (size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].state == State::Copying && device_->QueryDone(int(i)))
                slots_[i].state = State::Ready;
    }

    // Ready(seq): map without waiting; null keeps it Ready (counted). On success consume, unmap,
    // free, and free every older Ready result (stale, counted). False when nothing was consumed.
    bool Take(uint64_t seq, const std::function<void(const void*, size_t)>& consume)
    {
        if (device_ == nullptr)
            return false;
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].state != State::Ready || slots_[i].seq != seq)
                continue;
            size_t bytes = 0;
            const void* p = device_->MapNoWait(int(i), &bytes);
            if (p == nullptr) {
                ++counters_.mapBusy;
                return false;
            }
            if (consume)
                consume(p, bytes);
            device_->Unmap(int(i));
            slots_[i].state = State::Free;
            ++counters_.taken;
            for (size_t j = 0; j < slots_.size(); ++j)
                if (slots_[j].state == State::Ready && slots_[j].seq < seq) {
                    slots_[j].state = State::Free;
                    ++counters_.staleDropped;
                }
            return true;
        }
        return false;
    }

private:
    struct Slot {
        State state = State::Free;
        uint64_t seq = 0;
    };
    IReadbackDevice* device_;
    std::vector<Slot> slots_;
    ReadbackCounters counters_;
};

} // namespace VpLaneProducts
