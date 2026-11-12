// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

// P13-DECL (TD reserve item 2, refs/99c-td-method-search.md A3): the KEYED RING contract of the source ring (P13).
// DECLARATION ONLY: no allocation, no behaviour, nothing consumes it yet (the TD round builds on it). Header-only,
// pure C++14.
//
//   key     a frame in the ring is named by its SOURCE SEQUENCE, never by a slot index.
//   window  the frames the ring keeps addressable at render frame N: the age window
//               [N - BACK, N + LEAD]  =  BACK + 1 + LEAD  frames.
//           BACK and LEAD are DERIVED, never constants:
//             LEAD = GraphSettings.laneLead (core/ProductGraph.h, the sealed queue's lane lead, measured to render start);
//             BACK = the largest lag any active reader reads P13 at = Node::historyFull of the P13 node in the built
//                    graph (the graph already derives that depth from the declared reads).
//           Neither number appears in this file. RingWindowFrames() only adds them.
//   pins    a consumer may PIN up to maxPins frames by key; a pin keeps a frame that was addressable when it was taken
//           after the window has moved past it. maxPins is a PIN COUNT (A3 item 2: "up to 3 frames"), not a depth.
//   facts   every S7 queue fact that drops or re-labels frames (Flush, Discontinuity, FormatChange, Reset) invalidates
//           every pin (A3 item 5): see modules/input/source_analysis/SourceRingPin.h.
//
// How a seq-keyed window meets the SlotTable's `slot = seq mod ring` (core/ProductSlots.h, comment there): the SlotTable
// is a FIXED ring of ring = history + 1 + R + L + q slots addressed by seq mod ring; a derived window is just a
// different `history` and `L` fed to BuildGraph (history from the readers' lag, L from the sealed laneLead), so the
// ring is already sized for the window and a key maps to its slot by the same modulus. A key outside the window shares
// its slot with a newer frame (seq + ring), which is why the slot's own 40-bit seq stamp, not the modulus, decides
// whether the frame is still there. A PIN beyond the window needs a slot the modulus cannot give: the TD round must
// reserve extra ring slots (history + pins) at BuildGraph; this header only fixes the contract and the figure.
namespace VpSkeleton {

constexpr uint8_t kMaxRingPins = 3;   // A3 item 2: "a consumer may pin up to 3 frames by key beyond the window"

struct KeyedRingContract {
    bool keyed = false;         // frames are addressed by source sequence (key), window = BACK + 1 + LEAD
    bool backDerived = false;   // BACK comes from the largest reader lag in the built graph (Node::historyFull), not a constant
    bool leadDerived = false;   // LEAD comes from GraphSettings.laneLead (the sealed queue), not a constant
    uint8_t maxPins = 0;        // frames a consumer may pin beyond the window (0: no pin interface)

    bool operator==(const KeyedRingContract& o) const
    {
        return keyed == o.keyed && backDerived == o.backDerived && leadDerived == o.leadDerived && maxPins == o.maxPins;
    }
    bool operator!=(const KeyedRingContract& o) const { return !(*this == o); }
};

// The P13 contract (the declaration in core/Products.h AppendSourceRingProduct copies this).
constexpr KeyedRingContract SourceRingContract()
{
    return KeyedRingContract{ true, true, true, kMaxRingPins };
}

// The age window in frames for a derived BACK and LEAD. The caller passes the derived values (see the header comment).
constexpr uint32_t RingWindowFrames(uint32_t back, uint32_t lead)
{
    return back + 1u + lead;
}

}   // namespace VpSkeleton
