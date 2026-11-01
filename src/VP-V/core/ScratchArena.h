// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "GpuAllocator.h"

#include <VP-V/core/FallbackCatalog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Skeleton SA1: the shared scratch arena (doc 51 section 3). Reserve(plan) creates the pooled textures
// once, in the plan's fixed order (never inside Acquire, never per frame). A render stage opens a SCOPE
// (BeginScope / EndScope) and acquires its temporaries: the k-th Acquire of a shape inside a scope
// always gets pool slot k (deterministic), so two runs bind the same textures. Scopes run one after the
// other, so they share the pool (capacity = the busiest scope, GpuAllocator.h).
//
// Graceful fallback (owner rule): Acquire beyond the reserved capacity, for a request the plan did not
// admit (its tier was shed by the governor), outside a scope or from another thread returns a null
// handle and is counted. The caller then runs reduced or off for that frame; the arena records WHO
// went without, and FrameDecision() turns that into the FallbackCatalog entry "scratch_arena" (engaged
// while a frame was short, released by itself on the first frame that was not: recovery is automatic
// once a larger plan is reserved). Never a silent fallback.
//
// Threading: single owner thread (documented, checked race-free: the owner is a ThreadBinding, bound by the first
// Reserve or BeginScope, handed over only by Unbind(); another thread's Reserve / BeginFrame / BeginScope / Acquire is
// refused (false / no-op / false / null), is counted in CrossThreadAcquires
// and flagged in FrameDecision through atomics only, and never touches the pools or the owner's records).
//
// TexBackend: using Handle (null-testable, default constructed = null); Handle Create(const TexShape&)
// (null on failure); void Destroy(Handle&) (leaves it null).
namespace VpSkeleton {

// The owning thread of a single-thread object (the scratch arena, a grain-split SplitterFill), race-free: the id is
// atomic, and binding an unbound object is a compare-exchange, so when two threads race for the first use exactly one
// wins and the other sees "bound to another thread". Rebind() is an explicit transfer by the owner (e.g. a re-Reserve).
class ThreadBinding {
public:
    ThreadBinding() : id_(std::thread::id()) {}
    ThreadBinding(const ThreadBinding&) = delete;
    ThreadBinding& operator=(const ThreadBinding&) = delete;

    // Binds to the caller when unbound; true when (now) bound to the caller, false when another thread owns it.
    bool BindIfUnbound()
    {
        const std::thread::id me = std::this_thread::get_id();
        std::thread::id expected = std::thread::id();
        if (id_.compare_exchange_strong(expected, me))
            return true;
        return expected == me;
    }
    void Rebind() { id_.store(std::this_thread::get_id()); }
    void Reset() { id_.store(std::thread::id()); }
    bool Bound() const { return id_.load() != std::thread::id(); }
    bool BoundToCaller() const { return id_.load() == std::this_thread::get_id(); }
    bool BoundToOther() const
    {
        const std::thread::id v = id_.load();
        return v != std::thread::id() && v != std::this_thread::get_id();
    }

private:
    std::atomic<std::thread::id> id_;
};

template <class TexBackend>
class ScratchArena {
public:
    using Handle = typename TexBackend::Handle;

    explicit ScratchArena(TexBackend& backend) : backend_(&backend) {}
    ~ScratchArena() { Release(); }
    ScratchArena(const ScratchArena&) = delete;
    ScratchArena& operator=(const ScratchArena&) = delete;

    // Makes the pools match `plan`: textures of a shape already held are kept (up to the new capacity),
    // missing ones are created in pool order, surplus and unplanned ones are destroyed. False when a
    // texture could not be created (that pool stays short: its Acquires are exhausted, visibly).
    // Only the owner (or the first caller of an unbound arena) may reserve: another thread's Reserve is refused (false,
    // counted in CrossThreadAcquires, flagged in FrameDecision) and touches nothing. Unbind() hands the arena over.
    bool Reserve(const ArenaPlan& plan)
    {
        if (!owner_.BindIfUnbound()) {
            ++crossThread_;
            frameCross_.store(true);
            return false;
        }
        scopeOpen_ = false;
        std::vector<Pool> next;
        bool ok = true;
        std::vector<char> taken(pools_.size(), 0);
        for (const ArenaPool& pp : plan.pools) {
            Pool np;
            np.shape = pp.shape;
            for (size_t i = 0; i < pools_.size(); ++i)
                if (!taken[i] && pools_[i].shape == pp.shape) {
                    taken[i] = 1;
                    for (Handle& h : pools_[i].tex) {
                        if (np.tex.size() < pp.capacity)
                            np.tex.push_back(h);
                        else if (h)
                            backend_->Destroy(h);
                        h = Handle();
                    }
                    break;
                }
            while (np.tex.size() < pp.capacity) {
                Handle h = backend_->Create(pp.shape);
                if (!h) {
                    ++createFailures_;
                    ok = false;
                    break;
                }
                np.tex.push_back(h);
            }
            next.push_back(std::move(np));
        }
        for (size_t i = 0; i < pools_.size(); ++i)
            if (!taken[i])
                for (Handle& h : pools_[i].tex)
                    if (h)
                        backend_->Destroy(h);
        pools_ = std::move(next);
        plan_ = plan;
        ++reserves_;
        return ok;
    }

    // The owner hands the arena over (e.g. to a lane thread that will Reserve it next). Owner thread only.
    void Unbind()
    {
        if (owner_.BoundToCaller())
            owner_.Reset();
    }

    // Destroys every texture (device loss, shutdown). Owner thread, or after every user has stopped (destruction).
    void Release()
    {
        for (Pool& p : pools_)
            for (Handle& h : p.tex)
                if (h)
                    backend_->Destroy(h);
        pools_.clear();
        plan_ = ArenaPlan();
        scopeOpen_ = false;
    }

    // A new frame: forgets the previous frame's shortage record.
    void BeginFrame()
    {
        if (owner_.BoundToOther()) {
            ++crossThread_;
            frameCross_.store(true);
            return;   // another thread: the owner's records are untouched
        }
        frameShort_.clear();
        frameNested_ = false;
        frameCross_.store(false);
    }

    // False (and the OUTER scope untouched) when a scope is already open: nested scopes are refused, counted and
    // shown (FrameDecision). The caller must call EndScope only when this returned true.
    bool BeginScope(uint16_t scope)
    {
        if (!owner_.BindIfUnbound()) {
            ++crossThread_;   // another thread's scope: refused, the owner's state untouched
            frameCross_.store(true);
            return false;
        }
        if (scopeOpen_) {
            ++nestedScopes_;
            frameNested_ = true;
            return false;
        }
        scope_ = scope;
        scopeOpen_ = true;
        uses_.clear();
        return true;
    }

    void EndScope() { scopeOpen_ = false; }

    // True when the arena is bound to a thread other than the caller's (its Acquires would be refused): a caller
    // that runs on another thread (a lane producer) must use its own arena.
    bool BoundToOtherThread() const { return owner_.BoundToOther(); }

    // The texture for `owner`'s next temporary of `shape` in the open scope; null when there is none (the
    // caller reduces or switches off for this frame; the shortage is recorded for FrameDecision()).
    Handle Acquire(const char* owner, const TexShape& shape)
    {
        if (!owner_.Bound()) {
            ++outsideScope_;   // never reserved nor scoped: no scope is open
            return Handle();
        }
        if (!owner_.BoundToCaller()) {
            ++crossThread_;
            frameCross_.store(true);
            return Handle();   // another thread: atomics only (count + flag), never the owner's shortage record
        }
        if (!scopeOpen_) {
            ++outsideScope_;
            return Fail(owner);
        }
        const size_t pool = PoolIndex(shape);
        if (pool == pools_.size() || !plan_.Admits(scope_, owner, shape)) {
            ++shed_;   // not planned: its tier was shed (or it never asked)
            return Fail(owner);
        }
        // Counted per (owner, shape): up to this owner's admitted count, from this owner's base index.
        const uint32_t mine = plan_.OwnerCount(scope_, owner, shape);
        const uint32_t base = plan_.OwnerBase(scope_, owner, shape);
        const std::string name = owner ? owner : "";
        Use* use = nullptr;
        for (Use& u : uses_)
            if (u.pool == pool && u.owner == name)
                use = &u;
        if (!use) {
            uses_.push_back(Use{ name, pool, 0u });
            use = &uses_.back();
        }
        const size_t index = size_t(base) + use->count;
        if (use->count >= mine || index >= pools_[pool].tex.size() || !pools_[pool].tex[index]) {
            ++exhausted_;
            return Fail(owner);
        }
        ++use->count;
        ++acquired_;
        return pools_[pool].tex[index];
    }

    // The FallbackCatalog entry for the frame so far: engaged while any Acquire went without.
    FallbackCatalog::Decision FrameDecision() const
    {
        const bool cross = frameCross_.load();
        if (frameShort_.empty() && !frameNested_ && !cross)
            return FallbackCatalog::Released("scratch_arena");
        std::string reason;
        if (!frameShort_.empty()) {
            std::string list;
            for (size_t i = 0; i < frameShort_.size(); ++i)
                list += (i ? ", " : "") + frameShort_[i];
            reason = "scratch memory short: " + list + " reduced/off";
        }
        if (frameNested_)
            reason += std::string(reason.empty() ? "" : "; ") + "nested scratch scope refused (a stage did not close its scope)";
        if (cross)
            reason += std::string(reason.empty() ? "" : "; ") + "scratch arena used from another thread (refused; that caller ran reduced/off)";
        return FallbackCatalog::Engaged("scratch_arena", reason);
    }

    // The textures of the pool of `shape` (read-only, for tests and the ledger).
    size_t PoolSize(const TexShape& shape) const
    {
        const size_t i = PoolIndex(shape);
        return i == pools_.size() ? 0 : pools_[i].tex.size();
    }
    const ArenaPlan& Plan() const { return plan_; }
    uint64_t ReservedBytes() const
    {
        uint64_t b = 0;
        for (const Pool& p : pools_)
            b += ShapeBytes(p.shape) * p.tex.size();
        return b;
    }
    // Counters (never reset): acquired, exhausted (reserved capacity used up), shed (not admitted by the plan).
    uint64_t Acquired() const { return acquired_; }
    uint64_t Exhausted() const { return exhausted_; }
    uint64_t Shed() const { return shed_; }
    uint64_t CrossThreadAcquires() const { return crossThread_.load(); }
    uint64_t OutsideScopeAcquires() const { return outsideScope_; }
    uint64_t NestedScopes() const { return nestedScopes_; }
    uint32_t CreateFailures() const { return createFailures_; }
    uint32_t Reserves() const { return reserves_; }

private:
    struct Pool {
        TexShape shape;
        std::vector<Handle> tex;
    };
    struct Use {
        std::string owner;
        size_t pool;
        uint32_t count;
    };

    size_t PoolIndex(const TexShape& s) const
    {
        for (size_t i = 0; i < pools_.size(); ++i)
            if (pools_[i].shape == s)
                return i;
        return pools_.size();
    }

    Handle Fail(const char* owner)
    {
        const std::string name = owner ? owner : "?";
        if (std::find(frameShort_.begin(), frameShort_.end(), name) == frameShort_.end()) {
            frameShort_.push_back(name);
            std::sort(frameShort_.begin(), frameShort_.end());
        }
        return Handle();
    }

    TexBackend* backend_;
    std::vector<Pool> pools_;
    ArenaPlan plan_;
    std::vector<Use> uses_;
    bool frameNested_ = false;
    uint64_t nestedScopes_ = 0;
    std::vector<std::string> frameShort_;   // owners that went without this frame (sorted, unique)
    ThreadBinding owner_;   // atomic: BoundToOtherThread / Acquire read it from any thread
    bool scopeOpen_ = false;
    uint16_t scope_ = 0;
    uint64_t acquired_ = 0, exhausted_ = 0, shed_ = 0, outsideScope_ = 0;
    std::atomic<uint64_t> crossThread_{ 0 };
    std::atomic<bool> frameCross_{ false };
    uint32_t createFailures_ = 0, reserves_ = 0;
};

} // namespace VpSkeleton
