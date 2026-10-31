// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Indicator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Ancestry of a published product: which ancestor slots it was computed from, and a pure validator
// the seal runs. The slots themselves are S1; this header holds only the types and the check.
//
// N2 rule for S1: an ancestor's cgen must stay readable until every descendant that recorded it
// has been sealed or released. Validate reports AncestorGone when the slot was released or reused
// first; S1's release rule must make that impossible, and S1 counts and asserts zero.
namespace VpAncestry {

enum class Provenance : uint8_t { None, AheadLane, AheadOfa, LiveRender, LiveFallback };

struct AncestorRef {
    uint16_t productId = 0;
    uint16_t slotIndex = 0;
    uint64_t seq = 0;
    uint32_t cgen = 0;
    Provenance prov = Provenance::None;
};

constexpr size_t kMaxAncestors = 8;   // k is fixed at graph build, <= this

struct Header {
    uint32_t cgen = 0;                       // 0 = never published
    uint64_t seq = 0;
    Provenance prov = Provenance::None;
    VpIndicator::CounterMask reads = 0;      // counters this product depends on
    VpIndicator::Stamp honoured;             // the stamp it was computed under
    uint8_t ancestorCount = 0;
    std::array<AncestorRef, kMaxAncestors> ancestors{};
    uint32_t costUs = 0;
};

// Increments, wrapping past 0 (0 = never published).
inline uint32_t NextCgen(uint32_t g)
{
    ++g;
    return g == 0 ? 1u : g;
}

// What the seal sees of an ancestor slot.
struct SlotView {
    bool present = false;
    uint64_t seq = 0;
    uint32_t cgen = 0;
    bool valid = false;
};

enum class Verdict : uint8_t { Valid, NeverPublished, StampMismatch, AncestorReplaced, AncestorInvalid, AncestorGone };

inline const char* Token(Verdict v)
{
    switch (v) {
    case Verdict::Valid: return "valid";
    case Verdict::NeverPublished: return "never_published";
    case Verdict::StampMismatch: return "stamp_mismatch";
    case Verdict::AncestorReplaced: return "ancestor_replaced";
    case Verdict::AncestorInvalid: return "ancestor_invalid";
    case Verdict::AncestorGone: return "ancestor_gone";
    default: return "unknown";
    }
}

// lookup(productId, slotIndex) -> SlotView.
template <class Lookup>
Verdict Validate(const Header& h, const VpIndicator::Stamp& stampN, Lookup lookup)
{
    if (h.cgen == 0)
        return Verdict::NeverPublished;
    if (!VpIndicator::SameFor(h.honoured, stampN, h.reads))
        return Verdict::StampMismatch;
    const size_t n = h.ancestorCount < kMaxAncestors ? size_t(h.ancestorCount) : kMaxAncestors;
    for (size_t i = 0; i < n; ++i) {
        const AncestorRef& ref = h.ancestors[i];
        const SlotView view = lookup(ref.productId, ref.slotIndex);
        if (!view.present || view.seq != ref.seq)
            return Verdict::AncestorGone;
        if (view.cgen != ref.cgen)
            return Verdict::AncestorReplaced;
        if (!view.valid)
            return Verdict::AncestorInvalid;
    }
    return Verdict::Valid;
}

struct SlotEntry {
    uint16_t productId = 0;
    uint16_t slotIndex = 0;
    Header header;
};

struct TableEntry {
    uint16_t productId = 0;
    uint16_t slotIndex = 0;
    SlotView view;
};

// Validates slots given in topological order (ancestors first) against a mutable view table,
// writing each verdict back as that slot's `valid` before the next is checked, so a live refill
// (cgen++) of an ancestor cascades to every descendant in one pass. verdicts gets one per slot.
inline void ValidateInOrder(const std::vector<SlotEntry>& slots, const VpIndicator::Stamp& stampN,
                            std::vector<TableEntry>& table, std::vector<Verdict>& verdicts)
{
    verdicts.clear();
    verdicts.reserve(slots.size());
    auto find = [&table](uint16_t product, uint16_t slot) -> TableEntry* {
        for (TableEntry& t : table)
            if (t.productId == product && t.slotIndex == slot)
                return &t;
        return nullptr;
    };
    for (const SlotEntry& s : slots) {
        const Verdict v = Validate(s.header, stampN, [&find](uint16_t product, uint16_t slot) {
            const TableEntry* t = find(product, slot);
            return t ? t->view : SlotView();
        });
        verdicts.push_back(v);
        if (TableEntry* t = find(s.productId, s.slotIndex))
            t->view.valid = (v == Verdict::Valid);
    }
}

} // namespace VpAncestry
