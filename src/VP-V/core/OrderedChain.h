// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

// Skeleton W1-SKG: the frame-order rule of an ordered-chain lane product (ProductDecl::orderedChain; doc 72
// section 5.3: td.blocks reads itself at lag 1, so its jobs run in frame order). Pure state, no threads: the lane
// runner of the product owns one (its lane thread is the only caller).
//
//   Admit(seq)     InOrder        the predecessor (seq - 1) was the last frame this chain published
//                  CarryForward   the predecessor is not the last published (chain start, a late or abandoned job,
//                                 a skipped range): the job may run, in its declared reduced form. INFORMATIONAL:
//                                 LaneRunner acts only on Refused; whether a job runs reduced is decided by the
//                                 slot table (SlotTable::LaneAncestors: the self input is truly Absent), which is
//                                 the authority, so a verdict here never gates or counts anything.
//                  Refused        seq is not after the last published frame (a late N+0 after N+1, a repeat):
//                                 out-of-order work is refused, never run and never published
//   Published(seq) records the publish (the new tail of the chain)
//
// The refusal is the visible half of "a late N either completes before N+1 starts or is declared reduced, never
// skipped silently": nothing moves the chain backwards, and every gap is a counted carry-forward.
namespace VpSkeleton {

class ChainOrder {
public:
    enum class Verdict : uint8_t { InOrder, CarryForward, Refused };

    Verdict Admit(uint64_t seq) const
    {
        if (have_ && seq <= last_)
            return Verdict::Refused;
        if (!have_ || seq != last_ + 1u)
            return Verdict::CarryForward;
        return Verdict::InOrder;
    }
    void Published(uint64_t seq)
    {
        if (!have_ || seq > last_) {
            last_ = seq;
            have_ = true;
        }
    }
    bool Have() const { return have_; }
    uint64_t Last() const { return last_; }
    void Reset() { have_ = false; last_ = 0; }

private:
    bool have_ = false;
    uint64_t last_ = 0;
};

} // namespace VpSkeleton
