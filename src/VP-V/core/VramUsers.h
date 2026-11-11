// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "VramGovernor.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// MV7: the VRAM users outside the product catalog, declared to the ONE governor.
//
// VramGovernor.h declares the ids kUserNvofProducer / kUserTemporalDetail / kUserCameraModel / kUserInterpolator /
// kUserEnhancers (and, here, kUserBasicChroma), but until now nothing populated them (TierInput::otherUsers stayed empty), so the
// optical-flow engines and their rings, the camera model's textures, the interpolator and BasicChroma's output texture were
// INVISIBLE to the budget: the lane products were granted memory those users already held. A module that holds video memory
// declares it here when it allocates and withdraws it when it releases (a call site in the module's own file, no host change);
// SkeletonModule reads the snapshot at every graph configure (TierInput::otherUsers + ownUsageBytes: the memory these users
// hold is already inside the adapter's CurrentUsage, so it is VP's own and is re-planned, never counted twice).
//
// A declaration is a VramUser: one level = a FIXED user (counts, never shed: the module has no way to give memory back yet, so the
// governor must not pretend to shed it); several levels = a user that can step down (the module's call site reads the grant and
// applies it; BasicChroma's output is the first, with its call site still to come). `name` and every `levelText` entry MUST have
// static storage (they are kept as pointers).
//
// Guard (MV7 must-land check 3): a module that is ACTIVE (MarkActive) but has no declaration is listed in Snapshot::unpopulated,
// and the skeleton shows it (skeleton_vram_users) instead of letting its memory stay invisible. Header-only, C++14, thread-safe
// (the producers declare from their own threads).
namespace VpSkeleton {

// The plain name of a declarable user ("" for any other id): the governor lines and the ledger name it the same way everywhere.
inline const char* VramUserName(uint16_t id)
{
    switch (id) {
    case kUserNvofProducer: return "optical-flow engines";
    case kUserTemporalDetail: return "temporal detail";
    case kUserCameraModel: return "camera model";
    case kUserInterpolator: return "interpolator";
    case kUserEnhancers: return "texture restore / detail pulse / debander";
    case kUserBasicChroma: return "basic chroma upscale output";
    case kUserLongPair: return "long-pair optical flow";
    default: break;
    }
    return "";
}

// The ids a module must declare while it is active (the guard's table; a test pins it against VramGovernor.h).
inline const uint16_t* DeclarableVramUsers(size_t* count)
{
    static const uint16_t kIds[] = { kUserNvofProducer, kUserTemporalDetail, kUserCameraModel, kUserInterpolator, kUserEnhancers,
                                     kUserBasicChroma, kUserLongPair };
    if (count)
        *count = sizeof(kIds) / sizeof(kIds[0]);
    return kIds;
}

// Bytes of one texture-like object (a libplacebo pl_tex: params.w / h / d and params.format->texel_size). A template so this core
// header needs no libplacebo include; null or format-less = 0.
template <class Tex>
inline uint64_t TexBytesOf(const Tex* tex)
{
    if (tex == nullptr || tex->params.format == nullptr)
        return 0;
    const uint64_t d = tex->params.d > 0 ? uint64_t(tex->params.d) : uint64_t(1);
    return uint64_t(tex->params.w) * uint64_t(tex->params.h) * d * uint64_t(tex->params.format->texel_size);
}

// The users that hold video memory today and do NOT declare it (MV7 open items). Shown as one standing line while the lane runs (a
// visible gap, not a guess of their size); a module leaves this list when it calls Declare. Static text.
inline const char* KnownUndeclaredVramUsers()
{
    return "temporal detail, texture restore / detail pulse / debander, the de-judder's pair flow textures (a few hundred KB), camera guidance textures";
}

class VramUsers {
public:
    static VramUsers& Instance()
    {
        static VramUsers instance;
        return instance;
    }

    // `owner` identifies the declaring object (its address); one declaration per (owner, id), replaced by the next Declare.
    // `heldBytes` = what the module holds now (inside the adapter's CurrentUsage; the governor treats it as VP's own).
    void Declare(const void* owner, const VramUser& user, uint64_t heldBytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Entry& e : entries_) {
            if (e.owner == owner && e.user.id == user.id) {
                const bool same = e.held == heldBytes && e.user.levels == user.levels && e.user.tier == user.tier;
                e.user = user;
                e.held = heldBytes;
                if (!same)
                    version_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        Entry e;
        e.owner = owner;
        e.user = user;
        e.held = heldBytes;
        entries_.push_back(e);
        version_.fetch_add(1, std::memory_order_relaxed);
    }

    // A fixed user (one level: counted, never shed) named by VramUserName(id).
    void DeclareFixed(const void* owner, uint16_t id, uint64_t bytes)
    {
        VramUser u;
        u.id = id;
        u.name = VramUserName(id);
        u.tier = Tier::T0;
        u.lossless = false;
        u.levels.push_back(bytes);
        u.levelText.push_back(nullptr);
        Declare(owner, u, bytes);
    }

    // A user that can step down to nothing (two levels: `bytes`, then 0): the module's call site reads the grant and frees its memory.
    void DeclareSheddable(const void* owner, uint16_t id, uint64_t bytes, Tier tier)
    {
        VramUser u;
        u.id = id;
        u.name = VramUserName(id);
        u.tier = tier;
        u.lossless = false;
        u.levels.push_back(bytes);
        u.levels.push_back(0);
        u.levelText.push_back(nullptr);
        u.levelText.push_back("not available (the seeded search runs)");
        Declare(owner, u, bytes);
    }

    void Withdraw(const void* owner, uint16_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].owner == owner && entries_[i].user.id == id) {
                entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
                version_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
    }

    // The owner is gone (or its device): every declaration and active mark of it.
    void WithdrawAll(const void* owner)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool changed = false;
        for (size_t i = entries_.size(); i-- > 0;)
            if (entries_[i].owner == owner) {
                entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
            }
        for (size_t i = active_.size(); i-- > 0;)
            if (active_[i].owner == owner) {
                active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
            }
        if (changed)
            version_.fetch_add(1, std::memory_order_relaxed);
    }

    // The module is active (it will hold video memory) / not. An active module without a declaration is `unpopulated`.
    void MarkActive(const void* owner, uint16_t id, bool active)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < active_.size(); ++i)
            if (active_[i].owner == owner && active_[i].id == id) {
                if (!active) {
                    active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));
                    version_.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
        if (active) {
            Mark m;
            m.owner = owner;
            m.id = id;
            active_.push_back(m);
            version_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Changes whenever the set or the size of a declaration changes (the skeleton re-plans when it moved).
    uint64_t Version() const { return version_.load(std::memory_order_relaxed); }

    // W2-C1 (B9): the last DXGI budget reading of an adapter (by LUID), so the motion engine's once-a-second reading also serves
    // the skeleton's tier plan. nowSec: GetTickCount64 seconds. Not a declaration: Version() does not move.
    void NoteBudget(uint64_t luid, uint64_t usage, uint64_t budget, double nowSec)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        budgetLuid_ = luid;
        budgetUsage_ = usage;
        budgetBytes_ = budget;
        budgetAtSec_ = nowSec;
        budgetValid_ = budget != 0;
    }

    // The reading of that adapter when it is at most maxAge seconds old (false: query DXGI).
    bool LastBudget(uint64_t luid, uint64_t& usage, uint64_t& budget, double nowSec, double maxAge = 1.0) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!budgetValid_ || budgetLuid_ != luid || nowSec < budgetAtSec_ || nowSec - budgetAtSec_ > maxAge)
            return false;
        usage = budgetUsage_;
        budget = budgetBytes_;
        return true;
    }

    struct Snapshot {
        std::vector<VramUser> users;        // one per id (declarations of one id are summed level by level)
        uint64_t heldBytes = 0;             // what they hold now: VP's own usage inside CurrentUsage
        std::vector<std::pair<uint16_t, uint64_t>> heldById;   // the same, per id (a shed user that still holds its texture is a shortfall)
        std::vector<uint16_t> unpopulated;  // ids of active modules that declared nothing (unique, first-seen order)
    };

    Snapshot Take() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot s;
        std::vector<uint16_t> ids;
        for (const Entry& e : entries_) {
            s.heldBytes += e.held;
            bool counted = false;
            for (std::pair<uint16_t, uint64_t>& h : s.heldById)
                if (h.first == e.user.id) {
                    h.second += e.held;
                    counted = true;
                }
            if (!counted)
                s.heldById.push_back(std::make_pair(e.user.id, e.held));
            bool seen = false;
            for (uint16_t id : ids)
                seen = seen || id == e.user.id;
            if (!seen)
                ids.push_back(e.user.id);
        }
        for (uint16_t id : ids) {
            VramUser merged;
            size_t most = 0;
            size_t levels = 0;
            bool first = true;
            for (const Entry& e : entries_) {
                if (e.user.id != id)
                    continue;
                if (first) {
                    merged = e.user;
                    merged.levels.clear();
                    merged.levelText.clear();
                    first = false;
                }
                levels = levels > e.user.levels.size() ? levels : e.user.levels.size();
                if (e.user.levels.size() > most) {
                    most = e.user.levels.size();
                    merged.levelText = e.user.levelText;
                }
            }
            const char* plain = VramUserName(id);
            if (plain[0] != '\0')
                merged.name = plain;
            for (size_t k = 0; k < levels; ++k) {
                uint64_t sum = 0;
                for (const Entry& e : entries_)
                    if (e.user.id == id)
                        sum += LevelBytes(e.user, k);
                merged.levels.push_back(sum);
            }
            s.users.push_back(merged);
        }
        for (const Mark& m : active_) {
            bool declared = false;
            for (const Entry& e : entries_)
                declared = declared || (e.owner == m.owner && e.user.id == m.id);
            bool listed = false;
            for (uint16_t id : s.unpopulated)
                listed = listed || id == m.id;
            if (!declared && !listed)
                s.unpopulated.push_back(m.id);
        }
        return s;
    }

    // Tests only: forget everything (the registry is process-wide).
    void ResetForTesting()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        active_.clear();
        version_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    struct Entry {
        const void* owner = nullptr;
        VramUser user;
        uint64_t held = 0;
    };
    struct Mark {
        const void* owner = nullptr;
        uint16_t id = 0;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::vector<Mark> active_;
    std::atomic<uint64_t> version_{ 0 };
    uint64_t budgetLuid_ = 0;      // W2-C1: the shared budget reading (NoteBudget / LastBudget)
    uint64_t budgetUsage_ = 0;
    uint64_t budgetBytes_ = 0;
    double budgetAtSec_ = 0.0;
    bool budgetValid_ = false;
};

// "optical-flow engines, camera model" for the Fallbacks line; empty when nothing is unpopulated.
inline std::string UnpopulatedUserNames(const std::vector<uint16_t>& ids)
{
    std::string out;
    for (uint16_t id : ids) {
        const char* name = VramUserName(id);
        out += std::string(out.empty() ? "" : ", ") + (name[0] != '\0' ? std::string(name) : "user " + std::to_string(unsigned(id)));
    }
    return out;
}

} // namespace VpSkeleton
