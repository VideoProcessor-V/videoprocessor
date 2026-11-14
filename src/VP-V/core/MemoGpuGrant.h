// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// L-CORE (lead 8 ruling 2026-11-14 ~05:45 on 102b P13-NO-LANE-GPU-ALLOC; D9 "memo without analysis_lane"): the VRAM ledger
// grant of a renderer's memo when NO lane plans it. The same ledger rule as with the lane (the memo's GPU spots are budgeted
// as the scratch-arena user, kUserScratchArena, and reserved against the grant BEFORE the fill; released at the seal /
// eviction): the governor (PlanVram) grants the memo's declared need out of what the card has left after every other VP-V
// VRAM user, whose CURRENT holdings are taken as fixed (they do not shed for the memo). No grant = GPU fills that allocate stay
// Unavailable(no VRAM ledger grant), visibly. Views (OwnerFill::view) need no grant. Header-only, C++14, no GPU API.

#include "CardProfile.h"
#include "SkeletonAcquire.h"
#include "VramGovernor.h"

#include <cstdint>
#include <vector>

namespace VpSkeleton {

struct MemoGpuGrant {
    bool granted = false;
    uint64_t bytes = 0;      // the grant (== need when it fits; 0 = refused)
    uint64_t need = 0;       // the memo's declared GPU working set
    uint64_t budget = 0;     // the governor budget it was planned against
    bool cardKnown = false;
};

// `need`: SkeletonMemo::GpuNeedBytes(); `othersHeld`: the bytes every declared VP-V VRAM user holds now (VramUsers::Take
// heldBytes, inside the card's current usage). A need of 0 is "nothing to grant" (not granted, no refusal).
inline MemoGpuGrant PlanMemoGpuGrant(const CardProfile& card, bool cardKnown, uint64_t othersHeld, uint64_t need)
{
    MemoGpuGrant g;
    g.need = need;
    g.cardKnown = cardKnown;
    if (!cardKnown || need == 0)
        return g;
    VramUser memo;
    memo.id = kUserScratchArena;
    memo.name = "skeleton memo (no lane)";
    memo.tier = Tier::T0;
    memo.shedOrder = 200;
    memo.lossless = false;
    memo.levels.push_back(need);
    memo.levelText.push_back(nullptr);
    memo.levels.push_back(0);
    memo.levelText.push_back("memo GPU spots off");
    GovernorInput in;
    in.card = card;
    in.ownUsageBytes = othersHeld;     // VP's own usage inside the card's current usage ...
    in.transientBytes = othersHeld;    // ... held as it is: the other users do not shed for the memo
    in.users.push_back(memo);
    const GovernorPlan plan = PlanVram(in);
    g.budget = plan.budgetBytes;
    if (const UserGrant* u = plan.Find(kUserScratchArena)) {
        g.bytes = u->bytes;
        g.granted = u->bytes > 0;
    }
    return g;
}

}  // namespace VpSkeleton
