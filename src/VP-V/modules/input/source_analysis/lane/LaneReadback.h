// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/LaneInterfaces.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Skeleton S6 (design B4, "non-blocking P5 readback"): a fixed ring of staging buffers. The render
// thread copies a finished GPU product into a Free staging buffer, polls the device's non-blocking
// QueryDone (a D3D11 host may answer it with a DO_NOT_WAIT Map of the staging copy, as
// drive/led_measure/NativeReadback.h; not an event query), and maps without waiting once it is done. Nothing here blocks: a busy map is retried later and counted.
// The staging count is fixed at build (the host derives it from the measured readback latency in
// frames + 1). Pure C++14; the host owns the device.
namespace VpLaneProducts {

using VpSkeleton::IReadbackDevice;
using VpSkeleton::ProductId;

struct ReadbackCounters {
    uint64_t submitted = 0;
    uint64_t skippedNoStaging = 0;   // no Free staging buffer: the frame is not read back (visible)
    uint64_t copyFailed = 0;
    uint64_t pollFailed = 0;         // device reported copy/map failure: slot reclaimed (visible)
    uint64_t taken = 0;
    uint64_t mapBusy = 0;            // MapNoWait returned null: stays Ready, retried later
    uint64_t staleDropped = 0;       // an older Ready result freed when superseded or evicted
};

class ReadbackRing {
public:
    enum class State : uint8_t { Free, Copying, Ready };

    // Derived from VpSkeleton::kMaxSealLag (Products.h, 8 frames): bounded recent-failure memory so
    // per-sequence failure checks never poison subsequent healthy frames and never grow unbounded.
    static constexpr size_t kRecentFailedCapacity = VpSkeleton::kMaxSealLag;

    ReadbackRing(IReadbackDevice* device, uint8_t stagingCount) : device_(device), slots_(stagingCount) {}

    size_t Count() const { return slots_.size(); }
    State StateOf(size_t i) const { return slots_[i].state; }
    const ReadbackCounters& Counters() const { return counters_; }

    // Inspection & lifecycle helpers for non-blocking async readback owners (S#2 / R2).
    bool HasInFlight(uint64_t seq) const
    {
        for (size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].state == State::Copying && slots_[i].seq == seq)
                return true;
        return false;
    }

    bool HasReady(uint64_t seq) const
    {
        for (size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].state == State::Ready && slots_[i].seq == seq)
                return true;
        return false;
    }

    bool HasPending(uint64_t seq) const { return HasInFlight(seq) || HasReady(seq); }

    bool HasFreeSlot() const
    {
        for (size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].state == State::Free)
                return true;
        return false;
    }

    // Per-sequence device failure tracking (reclaimed in Poll/Take; never poisons unrelated sequences).
    bool WasFailed(uint64_t seq) const
    {
        for (size_t i = 0; i < recentFailed_.size(); ++i)
            if (recentFailed_[i].valid && recentFailed_[i].seq == seq)
                return true;
        return false;
    }

    bool ConsumeFailed(uint64_t seq)
    {
        for (size_t i = 0; i < recentFailed_.size(); ++i)
            if (recentFailed_[i].valid && recentFailed_[i].seq == seq) {
                recentFailed_[i].valid = false;
                return true;
            }
        return false;
    }

    // Resets all slot states to Free (discarding any Ready mappings first, for device teardown / resize).
    void Reset()
    {
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (device_ != nullptr && slots_[i].state == State::Ready)
                device_->Discard(int(i));
            slots_[i].state = State::Free;
            slots_[i].seq = 0;
        }
        for (size_t i = 0; i < recentFailed_.size(); ++i) {
            recentFailed_[i].valid = false;
            recentFailed_[i].seq = 0;
        }
        recentFailedNext_ = 0;
    }

    // Reclaims Ready slots strictly older than `minKeepSeq` (counted as staleDropped).
    size_t EvictReadyOlderThan(uint64_t minKeepSeq)
    {
        size_t evicted = 0;
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].state == State::Ready && slots_[i].seq < minKeepSeq) {
                if (device_ != nullptr)
                    device_->Discard(int(i));
                slots_[i].state = State::Free;
                slots_[i].seq = 0;
                ++counters_.staleDropped;
                ++evicted;
            }
        }
        return evicted;
    }

    // Reclaims the single oldest Ready slot (lowest seq) to prevent unconsumed-frame ring livelock.
    bool EvictOldestReady()
    {
        size_t oldest = slots_.size();
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].state != State::Ready)
                continue;
            if (oldest == slots_.size() || slots_[i].seq < slots_[oldest].seq)
                oldest = i;
        }
        if (oldest == slots_.size())
            return false;
        if (device_ != nullptr)
            device_->Discard(int(oldest));
        slots_[oldest].state = State::Free;
        slots_[oldest].seq = 0;
        ++counters_.staleDropped;
        return true;
    }

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
            ClearFailedSeq(seq);
            ++counters_.submitted;
            return true;
        }
        ++counters_.skippedNoStaging;
        return false;
    }

    // Copying buffers whose QueryDone reports done become Ready; failed copies are reclaimed to Free (non-blocking).
    void Poll()
    {
        if (device_ == nullptr)
            return;
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].state != State::Copying)
                continue;
            if (device_->QueryDone(int(i))) {
                slots_[i].state = State::Ready;
            } else if (device_->QueryFailed(int(i))) {
                const uint64_t failedSeq = slots_[i].seq;
                device_->Discard(int(i));
                slots_[i].state = State::Free;
                slots_[i].seq = 0;
                ++counters_.pollFailed;
                RecordFailedSeq(failedSeq);
            }
        }
    }

    // Ready(seq): map without waiting; null keeps it Ready (counted) unless the slot failed (reclaimed).
    // On success consume, unmap, free, and free Ready results older than `seq - keepOlder` (stale, counted).
    bool Take(uint64_t seq, const std::function<void(const void*, size_t)>& consume, uint64_t keepOlder = 0)
    {
        if (device_ == nullptr)
            return false;
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].state != State::Ready || slots_[i].seq != seq)
                continue;
            size_t bytes = 0;
            const void* p = device_->MapNoWait(int(i), &bytes);
            if (p == nullptr) {
                if (device_->QueryFailed(int(i))) {
                    const uint64_t failedSeq = slots_[i].seq;
                    device_->Discard(int(i));
                    slots_[i].state = State::Free;
                    slots_[i].seq = 0;
                    ++counters_.pollFailed;
                    RecordFailedSeq(failedSeq);
                    return false;
                }
                ++counters_.mapBusy;
                return false;
            }
            if (consume)
                consume(p, bytes);
            device_->Unmap(int(i));
            slots_[i].state = State::Free;
            slots_[i].seq = 0;
            ++counters_.taken;
            for (size_t j = 0; j < slots_.size(); ++j)
                if (slots_[j].state == State::Ready && slots_[j].seq < seq && (seq - slots_[j].seq) > keepOlder) {
                    device_->Discard(int(j));
                    slots_[j].state = State::Free;
                    slots_[j].seq = 0;
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
    struct FailedEntry {
        uint64_t seq = 0;
        bool valid = false;
    };

    void RecordFailedSeq(uint64_t seq)
    {
        for (size_t i = 0; i < recentFailed_.size(); ++i)
            if (recentFailed_[i].valid && recentFailed_[i].seq == seq)
                return;
        recentFailed_[recentFailedNext_].seq = seq;
        recentFailed_[recentFailedNext_].valid = true;
        recentFailedNext_ = (recentFailedNext_ + 1) % recentFailed_.size();
    }

    void ClearFailedSeq(uint64_t seq)
    {
        for (size_t i = 0; i < recentFailed_.size(); ++i)
            if (recentFailed_[i].valid && recentFailed_[i].seq == seq)
                recentFailed_[i].valid = false;
    }

    IReadbackDevice* device_;
    std::vector<Slot> slots_;
    ReadbackCounters counters_;
    std::array<FailedEntry, kRecentFailedCapacity> recentFailed_{};
    size_t recentFailedNext_ = 0;
};

} // namespace VpLaneProducts
