// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/RingContract.h>

#include <array>
#include <cstdint>

// P13-DECL (TD reserve items 2 and 5, refs/99c-td-method-search.md A3): the PIN interface of the keyed source ring
// (core/RingContract.h) and its S7 invalidation contract. DECLARATION + bookkeeping only: nothing in the renderer
// calls it, it holds no frame memory and allocates nothing (the pins are 3 sequence numbers in a fixed array).
// The TD round binds a real host that keeps the pinned frames alive. Header-only, pure C++14.
//
// Contract.
//   Key     a frame is named by its SOURCE SEQUENCE (the same key the SlotTable stamps).
//   Window  at render frame `current`, the frames addressable are [current - back, current + lead]. `back` and `lead` are
//           passed in by the caller and are DERIVED (back = the largest reader lag of P13 in the built graph, lead =
//           GraphSettings.laneLead); this header has no constant for either.
//   Pin     Pin(key) is accepted only for a frame that is addressable NOW (inside the window) or already pinned, at most
//           kMaxRingPins at once. A pinned frame stays pinned while the window moves past it. Every refusal says why
//           (PinResult) and is counted; nothing is silent.
//   Facts   the S7 queue facts that drop or re-label frames (Flush, Discontinuity, FormatChange, Reset; refs/107 s1 S7 row,
//           RendererExtension.h QueueEvent) invalidate EVERY pin: a pinned frame from before the fact is no longer the
//           frame its key names. The epoch advances, so a holder that still believes it holds a pin sees Pinned() false.
namespace VpSourceRing {

// The queue facts that invalidate pins (a subset of the S7 queue facts, named as the host seam names them).
enum class Invalidation : uint8_t { Flush, Discontinuity, FormatChange, Reset };

inline const char* Token(Invalidation f)
{
    switch (f) {
    case Invalidation::Flush: return "flush";
    case Invalidation::Discontinuity: return "discontinuity";
    case Invalidation::FormatChange: return "format change";
    case Invalidation::Reset: return "reset";
    }
    return "?";
}

enum class PinResult : uint8_t {
    Pinned,           // newly pinned
    AlreadyPinned,    // the key was already pinned (no second pin taken)
    NotWired,         // no ring is bound (the no-op host, or no window set yet): refused visibly
    OutsideWindow,    // the frame is not addressable now (older than the window, or beyond the lead)
    TooManyPins       // kMaxRingPins pins are held
};

inline const char* Token(PinResult r)
{
    switch (r) {
    case PinResult::Pinned: return "pinned";
    case PinResult::AlreadyPinned: return "already pinned";
    case PinResult::NotWired: return "refused: source ring not bound";
    case PinResult::OutsideWindow: return "refused: frame outside the ring window";
    case PinResult::TooManyPins: return "refused: pin limit reached";
    }
    return "?";
}

// What a consumer sees (the interface the TD round binds a real host to).
class IFramePin {
public:
    virtual ~IFramePin() {}
    virtual PinResult Pin(uint64_t key) = 0;
    virtual bool Release(uint64_t key) = 0;       // false: the key was not pinned
    virtual bool Pinned(uint64_t key) const = 0;
};

// The host until the TD round: every Pin is refused with NotWired (visible through the result), nothing is held.
class NoopFramePin : public IFramePin {
public:
    PinResult Pin(uint64_t) override { return PinResult::NotWired; }
    bool Release(uint64_t) override { return false; }
    bool Pinned(uint64_t) const override { return false; }
};

struct PinCounters {
    uint64_t pinned = 0;
    uint64_t refusedNotWired = 0;
    uint64_t refusedOutsideWindow = 0;
    uint64_t refusedTooMany = 0;
    uint64_t invalidations = 0;      // facts that arrived
    uint64_t pinsDropped = 0;        // pins removed by those facts
};

// Pure bookkeeping of the pins (sequence numbers only). One thread at a time (the render thread owns the queue facts and the
// window; a lane consumer that pins does so under the host's own lock - the TD round decides).
class FramePinBook : public IFramePin {
public:
    // The window for render frame `current`: [current - back, current + lead]. Call whenever the derived values change.
    void SetWindow(uint64_t current, uint32_t back, uint32_t lead)
    {
        current_ = current;
        back_ = back;
        lead_ = lead;
        bound_ = true;
    }

    // Unbinds the ring (pins stay recorded until a fact or Release removes them; new pins are refused NotWired).
    void Unbind() { bound_ = false; }

    bool InWindow(uint64_t key) const
    {
        if (!bound_)
            return false;
        const uint64_t lo = current_ > back_ ? current_ - back_ : 0u;
        return key >= lo && key <= current_ + lead_;
    }

    PinResult Pin(uint64_t key) override
    {
        if (Pinned(key))
            return PinResult::AlreadyPinned;
        if (!bound_) {
            ++c_.refusedNotWired;
            return PinResult::NotWired;
        }
        if (!InWindow(key)) {
            ++c_.refusedOutsideWindow;
            return PinResult::OutsideWindow;
        }
        for (size_t i = 0; i < pins_.size(); ++i) {
            if (!used_[i]) {
                pins_[i] = key;
                used_[i] = true;
                ++count_;
                ++c_.pinned;
                return PinResult::Pinned;
            }
        }
        ++c_.refusedTooMany;
        return PinResult::TooManyPins;
    }

    bool Release(uint64_t key) override
    {
        for (size_t i = 0; i < pins_.size(); ++i)
            if (used_[i] && pins_[i] == key) {
                used_[i] = false;
                --count_;
                return true;
            }
        return false;
    }

    bool Pinned(uint64_t key) const override
    {
        for (size_t i = 0; i < pins_.size(); ++i)
            if (used_[i] && pins_[i] == key)
                return true;
        return false;
    }

    // An S7 queue fact that drops or re-labels frames: every pin is invalid. The epoch advances.
    void OnQueueFact(Invalidation)
    {
        ++c_.invalidations;
        c_.pinsDropped += count_;
        for (size_t i = 0; i < used_.size(); ++i)
            used_[i] = false;
        count_ = 0;
        ++epoch_;
    }

    size_t PinCount() const { return count_; }
    uint32_t Epoch() const { return epoch_; }
    const PinCounters& Counters() const { return c_; }

private:
    std::array<uint64_t, VpSkeleton::kMaxRingPins> pins_{};
    std::array<bool, VpSkeleton::kMaxRingPins> used_{};
    size_t count_ = 0;
    uint32_t epoch_ = 0;
    uint64_t current_ = 0;
    uint32_t back_ = 0;
    uint32_t lead_ = 0;
    bool bound_ = false;
    PinCounters c_;
};

}   // namespace VpSourceRing
