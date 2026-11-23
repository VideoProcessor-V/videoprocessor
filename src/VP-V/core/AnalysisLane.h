// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "GapScheduler.h"
#include "Indicator.h"
#include "LaneGapChunk.h"
#include "LaneInterfaces.h"
#include "LaneLease.h"
#include "LaneRunner.h"
#include "ProductGraph.h"
#include "ProductSlots.h"
#include "Products.h"
#include "Regulator.h"
#include "SealPoint.h"
#include "VramGovernor.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Skeleton S3-S7: the analysis lane facade. THE one object the renderer will hold (null when
// analysis_lane = off: nothing is constructed, no hook does anything, no thread starts). It owns the
// per-graph state (graph, SlotTable, CpuBuffers per CPU node, LaneRunner, source ring, gap works) as a
// std::shared_ptr<LaneState> swapped with std::atomic_store at a rebuild: a lane worker takes the
// shared_ptr at the start of each step, so the old table dies with its last holder and is never freed
// under a worker. Header-only, pure C++14, no Windows, no GPU, no libplacebo; clocks are injected.
//
// HOST CALL SITES (generic, inert until the M2 H1 layer wires them; the renderer gets one line each):
//   capture/conversion thread  OnConvertedFrame          -> OnSourceFrame(view)
//   render thread, render start BeforeRender             -> while ((w = DeterministicWaitUs(seq, slack)) > 0)
//                                                             wait w us;   then OnRenderStart(seq, slack)
//   render thread, post-submit  AfterRender              -> OnPostSubmitGap(gapEndUs)
//   render thread, present done retired                  -> OnFrameRetired(seq, releaseFenceValue)
//   render thread, queue drop   frame never rendered     -> OnFrameDropped(seq)
//   render thread, per frame    PassTimers + slack       -> OnFrameTiming(seq, slackMs, renderOnlyMs, missed)
//   render thread, source cut   source stats cut event   -> OnCut()
//   render thread, settings     OnSettings               -> Configure(settings, registry, consumers, ...)
//   render thread, status       GetModuleStatus          -> StatusLines()
//   render thread, fallbacks    FallbackStatus sync      -> Fallbacks()   (FallbackCatalog::Sync each)
//   render thread, diagnostics  DiagnosticHistory        -> RegulatorTraces(), the regulator decisions
//   walk call site              active-picture walk      -> EffectiveWalk() (Original = Bill's walk)
//   OFA / CPU worker threads    LaneWorkerThreads        -> StepWorker(Ofa | Cpu, budgetUs) in a loop
//   keys (M2 H2 settings hook)  analysis_lane, analysis_lane_chunk_us, analysis_lane_deterministic,
//                               analysis_lane_test_wait_ms, source_stats_walk -> LaneSettings::Parse
//
// PRODUCER / SOURCE CONTRACT. (1) A SourceFrameView handed to OnSourceFrame owns its planes through
// keepAlive: the lane copies the view for the whole of a job, so the host may recycle the planes
// the moment every copy is released (ring overwrite, OnFrameDropped, job end), never earlier.
// (2) An ILaneProducer's Begin may follow an unfinished job of a previous graph's runner (a graph
// swap re-registers the same producer with the new runner): Begin resets ALL job state. Device work of
// that abandoned job lands in the OLD graph's buffers, which the host keeps until the old LaneState
// (and so its last holder) is released.
//
// Timing decides WHEN lane work runs, never WHAT a product contains: every product is the pure
// function of the frame and its inputs; the live (in-render) form is bit-identical for CPU products.
namespace VpSkeleton {

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

struct LaneSettings {
    enum class Mode : uint8_t { Off, Shadow, On };
    enum class Det : uint8_t { Auto, On, Off };

    Mode mode = Mode::Off;
    uint32_t chunkUs = 0;                    // 0 = derived from the measured frame period
    Det deterministic = Det::Auto;           // Auto follows the host's deterministic run
    uint32_t testWaitUs = 0;                 // explicit bounded test wait for N1 (0 = none)
    VpLaneProducts::WalkSource walk = VpLaneProducts::WalkSource::Original;

    // One config key. False for an unknown key or an invalid value (the setting is left unchanged).
    bool Parse(const char* key, const char* value)
    {
        if (key == nullptr)
            return false;
        const char* v = value != nullptr ? value : "";
        if (EqI(key, "analysis_lane")) {
            if (EqI(v, "off")) mode = Mode::Off;
            else if (EqI(v, "shadow")) mode = Mode::Shadow;
            else if (EqI(v, "on")) mode = Mode::On;
            else return false;
            return true;
        }
        if (EqI(key, "analysis_lane_chunk_us")) {
            uint32_t n = 0;
            if (!ParseUInt(v, 4000000u, &n))
                return false;
            chunkUs = n;
            return true;
        }
        if (EqI(key, "analysis_lane_deterministic")) {
            if (EqI(v, "auto")) deterministic = Det::Auto;
            else if (EqI(v, "on")) deterministic = Det::On;
            else if (EqI(v, "off")) deterministic = Det::Off;
            else return false;
            return true;
        }
        if (EqI(key, "analysis_lane_test_wait_ms")) {
            uint32_t n = 0;
            if (!ParseUInt(v, 4000000u, &n))
                return false;
            testWaitUs = n * 1000u;
            return true;
        }
        if (EqI(key, "source_stats_walk")) {
            if (EqI(v, "original")) walk = VpLaneProducts::WalkSource::Original;
            else if (EqI(v, "lane")) walk = VpLaneProducts::WalkSource::Lane;
            else return false;
            return true;
        }
        return false;
    }

private:
    static bool EqI(const char* a, const char* b)
    {
        for (;; ++a, ++b) {
            char x = *a;
            char y = *b;
            if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
            if (x != y)
                return false;
            if (x == '\0')
                return true;
        }
    }
    static bool ParseUInt(const char* s, uint32_t maxValue, uint32_t* out)
    {
        if (*s == '\0')
            return false;
        uint64_t n = 0;
        for (; *s != '\0'; ++s) {
            if (*s < '0' || *s > '9')
                return false;
            n = n * 10u + uint64_t(*s - '0');
            if (n > maxValue)
                return false;
        }
        *out = uint32_t(n);
        return true;
    }
};

// ---------------------------------------------------------------------------------------------
// The source ring: the views the lane reads. Capacity = the graph's framesInFlight. The host keeps
// the planes valid until OnFrameRetired / OnFrameDropped. Workers read through a mutex; the render
// thread reads lock-free through a seqlock mirror (it never takes the mutex).
//
// LOCK ORDER: a view's keepAlive deleter is host code (typically a pool release that takes the pool's
// own lock). It can run on ANY thread that drops the last copy: the capture thread (Store overwrite),
// the render thread (Drop, OnFrameRetired, an old LaneState's destruction), or a lane worker
// (LaneRunner::ReleaseSource). It must be thread-safe, and the ring never runs it under its mutex: a
// displaced view is moved into a local and destroyed after the lock is released, so a deleter may even
// call back into the ring.
// ---------------------------------------------------------------------------------------------

class LaneSourceRing {
public:
    explicit LaneSourceRing(size_t capacity)
        : cap_(capacity < 2 ? size_t(2) : capacity)
        , views_(cap_)
        , has_(cap_, 0)
        , mirror_(cap_)
    {
        for (Mirror& m : mirror_)
            m.seq.store(kInvalid, std::memory_order_relaxed);
    }

    size_t Capacity() const { return cap_; }

    void Store(const SourceFrameView& v)
    {
        const size_t slot = size_t(v.seq % cap_);
        SourceFrameView displaced;   // declared before the lock: destroyed after it is released
        std::lock_guard<std::mutex> lock(mutex_);   // the mirror writes are under it too: one writer at a time
        displaced = std::move(views_[slot]);
        views_[slot] = v;
        has_[slot] = 1;
        if (!any_ || v.seq > newest_) {
            newest_ = v.seq;
            any_ = true;
        }
        Mirror& m = mirror_[slot];
        m.seq.store(kInvalid, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        m.luma.store(reinterpret_cast<uintptr_t>(v.luma), std::memory_order_relaxed);
        m.width.store(v.width, std::memory_order_relaxed);
        m.height.store(v.height, std::memory_order_relaxed);
        m.stride.store(v.strideSamples, std::memory_order_relaxed);
        m.shift.store(v.msbShift, std::memory_order_relaxed);
        m.seq.store(v.seq, std::memory_order_release);
    }

    // The frame will never be rendered: the ring lets go of its copy (the planes live on in any lane
    // job's own copy until the job ends).
    void Drop(uint64_t seq)
    {
        SourceFrameView displaced;   // destroyed after the lock is released (see LOCK ORDER)
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t slot = size_t(seq % cap_);
        if (has_[slot] == 0 || views_[slot].seq != seq)
            return;
        mirror_[slot].seq.store(kInvalid, std::memory_order_release);
        displaced = std::move(views_[slot]);
        views_[slot] = SourceFrameView();
        has_[slot] = 0;
    }

    // Let go of every held frame (the ring of a graph that was swapped out: the new ring carries the
    // frames on, so the host pool is not held twice). The copies die outside the lock.
    void Clear()
    {
        std::vector<SourceFrameView> displaced;
        std::lock_guard<std::mutex> lock(mutex_);
        displaced.reserve(cap_);
        for (size_t i = 0; i < cap_; ++i) {
            if (has_[i] == 0)
                continue;
            mirror_[i].seq.store(kInvalid, std::memory_order_release);
            displaced.push_back(std::move(views_[i]));
            views_[i] = SourceFrameView();
            has_[i] = 0;
        }
        any_ = false;
    }

    // Worker side (mutex): COPIES the held view of seq into *out (the copy owns the planes through
    // its keepAlive, so a later overwrite or drop cannot free them under the caller). False: not held.
    bool Find(uint64_t seq, SourceFrameView* out) const
    {
        SourceFrameView copy;   // the copy is taken under the lock; *out (and its old keepAlive) is touched after it
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t slot = size_t(seq % cap_);
            if (has_[slot] == 0 || views_[slot].seq != seq)
                return false;
            if (out)
                copy = views_[slot];
        }
        if (out)
            *out = std::move(copy);
        return true;
    }

    // Render side (lock-free copy; carries NO keepAlive: used at render start of a frame the render
    // itself holds). False when the frame is not (or no longer) held.
    bool TryGet(uint64_t seq, SourceFrameView* out) const
    {
        const Mirror& m = mirror_[size_t(seq % cap_)];
        const uint64_t s1 = m.seq.load(std::memory_order_acquire);
        if (s1 != seq)
            return false;
        SourceFrameView v;
        v.seq = seq;
        v.luma = reinterpret_cast<const uint16_t*>(m.luma.load(std::memory_order_relaxed));
        v.width = m.width.load(std::memory_order_relaxed);
        v.height = m.height.load(std::memory_order_relaxed);
        v.strideSamples = m.stride.load(std::memory_order_relaxed);
        v.msbShift = uint8_t(m.shift.load(std::memory_order_relaxed));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m.seq.load(std::memory_order_relaxed) != s1)
            return false;
        if (out)
            *out = v;
        return true;
    }

    bool Newest(uint64_t* out) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!any_)
            return false;
        if (out)
            *out = newest_;
        return true;
    }

    // Carry the held frames over from the ring of the previous graph (ascending seq).
    void CopyFrom(const LaneSourceRing& other)
    {
        std::vector<SourceFrameView> snap;
        {
            std::lock_guard<std::mutex> lock(other.mutex_);
            for (size_t i = 0; i < other.cap_; ++i)
                if (other.has_[i] != 0)
                    snap.push_back(other.views_[i]);
        }
        std::sort(snap.begin(), snap.end(),
                  [](const SourceFrameView& a, const SourceFrameView& b) { return a.seq < b.seq; });
        for (const SourceFrameView& v : snap)
            Store(v);
    }

private:
    static constexpr uint64_t kInvalid = ~uint64_t(0);
    struct Mirror {
        std::atomic<uint64_t> seq;
        std::atomic<uintptr_t> luma{ 0 };
        std::atomic<uint32_t> width{ 0 };
        std::atomic<uint32_t> height{ 0 };
        std::atomic<uint32_t> stride{ 0 };
        std::atomic<uint32_t> shift{ 0 };
    };

    const size_t cap_;
    mutable std::mutex mutex_;
    std::vector<SourceFrameView> views_;
    std::vector<char> has_;
    std::vector<Mirror> mirror_;
    bool any_ = false;
    uint64_t newest_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Lane GPU-compute products share the render thread's idle gap: an IGapWork adapter per product that
// calls runner.Step(id, budget) on the render thread.
// ---------------------------------------------------------------------------------------------

// LaneStepProgressed and the gap-chunk law: core/LaneGapChunk.h (one copy; S-3a's LaneSpareWork gap face calls the same law).
class LaneComputeGapWork : public IGapWork {
public:
    LaneComputeGapWork(LaneRunner* runner, ProductId id, Tier tier, double heavyMs, std::function<uint64_t()> nowUs)
        : runner_(runner), id_(id), tier_(tier), heavyMs_(heavyMs), nowUs_(std::move(nowUs))
    {
    }

    GapClass Class() const override { return GapClass::LaneCompute; }
    uint16_t Id() const override { return id_; }
    Tier WorkTier() const override { return tier_; }
    double HeavyMs() const override { return heavyMs_; }
    bool HasWork() const override { return !gap_.idle && runner_ != nullptr && runner_->Laned(id_); }
    // The gap-chunk law (core/LaneGapChunk.h): predict = max(measured, device prediction), halving on cap-no-fit, one Step per chunk.
    uint32_t PredictUs() const override { return gap_.PredictUs(runner_, id_); }
    void OnCapNoFit() override { gap_.OnCapNoFit(); }
    void RunChunk(uint32_t budgetUs) override { gap_.RunChunk(runner_, id_, budgetUs, nowUs_); }

    void Wake() { gap_.Wake(); }
    uint64_t Chunks() const { return gap_.chunks; }

private:
    LaneRunner* runner_;
    ProductId id_;
    Tier tier_;
    double heavyMs_;
    std::function<uint64_t()> nowUs_;
    LaneGapChunk gap_;
};

// ---------------------------------------------------------------------------------------------
// The per-graph state.
// ---------------------------------------------------------------------------------------------

struct LaneNodeInfo {
    bool sourceFrame = false;
    bool identical = true;
    bool reducedMode = false;
    std::string name;
};

struct LaneState {
    // Keeps the Indicator this state's runner reads alive (declared first: destroyed last). Null when the
    // lane was built on a raw pointer (tests).
    std::shared_ptr<VpIndicator::Indicator> indHold;
    Graph graph;
    std::unique_ptr<LaneSourceRing> ring;
    std::unique_ptr<SlotTable> table;                 // (declared before the runner: destroyed after it)
    std::vector<std::unique_ptr<CpuBuffers>> bufs;    // per node index; null unless the node is CPU
    std::vector<LaneNodeInfo> info;                   // per node index
    std::vector<int> indexOfId;                       // ProductId -> node index, -1 none
    TierPlan plan;
    std::unique_ptr<LaneRunner> runner;
    std::array<std::vector<ProductId>, 4> workerOrder;   // per Backend: tier, then topological
    std::vector<std::unique_ptr<LaneComputeGapWork>> gapWorks;
    GapScheduler gap;
    std::vector<uint64_t> lateSeen;                   // per node index (render thread)
    std::vector<uint64_t> missSeen;                   // per node index (render thread)
    uint64_t firstSeq = 0;                            // the first frame this graph serves
    uint64_t nextExpectedRetire = 0;                  // next frame seq expected to retire or drop in order (render thread)
    uint64_t generation = 0;                          // 1,2,3...: one per built state (LaneLease staleness check)
    std::string capsMismatch;                         // non-empty: the real plan's history caps differ from the trial's

    int IndexOf(ProductId id) const { return id < indexOfId.size() ? indexOfId[id] : -1; }
};

// ---------------------------------------------------------------------------------------------
// C9 commit A: the published read of a CPU product by an OUTSIDE worker (doc 93 C9P s2b).
// ---------------------------------------------------------------------------------------------

// A seqlock on the slot word, with the public SlotSet calls only (no pin, nothing written): Read(seq, allowPublished)
// names the ring slot of a Published / Sealed result; `pass(bytes, n)` reads it; then, after an acquire fence, the slot
// must still name the same ring slot for the same frame (Published / Sealed) under the same lane-header claim generation.
// Otherwise the lane (or the render's release) reused the slot during the pass: Recycled, and whatever `pass` produced is
// discarded by the caller. A result is a pure function of the frame, so an unchanged word means unchanged bytes. Any thread.
// Memory order: the read side relies on the writer's claim CAS (an acq_rel / seq_cst RMW on the slot word) being ordered before its
// plain ring writes - true on x86 (TSO: stores are not reordered with an earlier locked RMW). On a weakly ordered CPU (ARM)
// that is not enough: the writer needs a release fence after the claim (and the reader's acquire fence stays), else a pass
// could see new bytes under the old word. VP builds x64 only today; revisit before any ARM build.
template <class Pass>
inline PublishedRead ReadPublishedBytes(const SlotSet& set, const CpuBuffers& bufs, uint64_t seq, Pass&& pass)
{
    const ReadInfo before = set.Read(seq, true);
    if (before.kind != ReadInfo::Ring || before.index < 0)
        return PublishedRead::Absent;
    const VpAncestry::SlotView v0 = set.ViewAt(uint16_t(before.index));
    if (!v0.present || v0.seq != seq || !(v0.valid || v0.lanePublished))
        return PublishedRead::Absent;
    pass(bufs.Ring(size_t(before.index)), bufs.Bytes());
    std::atomic_thread_fence(std::memory_order_acquire);
    const ReadInfo after = set.Read(seq, true);
    const VpAncestry::SlotView v1 = set.ViewAt(uint16_t(before.index));
    const bool same = after.kind == ReadInfo::Ring && after.index == before.index && v1.present && v1.seq == seq
                      && v1.cgen == v0.cgen;
    return same ? PublishedRead::Copied : PublishedRead::Recycled;
}

// The same through a lease (AnalysisLane::Lease / IExternalDemand::LeaseFor of product `id`: the lease holds the state, so
// the table and the bytes ring live for the call). `needBytes` must equal the product's buffer size (a different source
// size is Absent, never a partial copy). Any thread.
template <class Pass>
inline PublishedRead ReadPublishedBytes(const LaneLease& lease, ProductId id, uint64_t seq, size_t needBytes, Pass&& pass)
{
    if (!lease.Valid() || lease.bytes == nullptr || lease.bytes->Bytes() != needBytes)
        return PublishedRead::Absent;
    const SlotSet* set = lease.table->Find(id);
    if (set == nullptr)
        return PublishedRead::Absent;
    return ReadPublishedBytes(*set, *lease.bytes, seq, std::forward<Pass>(pass));
}

// P1's engine half of frame `seq` (source `src`) from a lease of kP1HalfLuma: Engine8Half of the published half plane into
// `out` (HalfDim(w) x HalfDim(h) bytes). Exact for the lane's 10-bit tap views (msbShift 6). On Absent / Recycled `out`
// holds nothing usable. Any thread.
inline PublishedRead ReadPublishedEngine8(const LaneLease& lease, uint64_t seq, Dims src, uint8_t* out)
{
    const uint32_t hw = VpLaneProducts::HalfDim(src.width);
    const uint32_t hh = VpLaneProducts::HalfDim(src.height);
    if (out == nullptr || hw == 0 || hh == 0)
        return PublishedRead::Absent;
    return ReadPublishedBytes(lease, kP1HalfLuma, seq, VpLaneProducts::HalfLumaBytes(src),
                              [hw, hh, out](const uint8_t* bytes, size_t) {
                                  VpLaneProducts::Engine8Half(reinterpret_cast<const uint16_t*>(bytes), hw, hh, out);
                              });
}

// The published-engine-view hook (LaneInterfaces.h PublishedEngine8Fn) over a lease source: each call takes the CURRENT lease
// of P1 from `leases` (IExternalDemand::LeaseFor(host, kP1HalfLuma) at the hand-off), so a rebuilt lane is followed.
inline PublishedEngine8Fn PublishedEngine8Of(LaneLeaseSource leases)
{
    return [leases](uint64_t seq, uint32_t width, uint32_t height, uint8_t* out) {
        if (!leases)
            return PublishedRead::Absent;
        const LaneLease lease = leases();
        return ReadPublishedEngine8(lease, seq, Dims{ width, height }, out);
    };
}

// ---------------------------------------------------------------------------------------------
// The facade.
// ---------------------------------------------------------------------------------------------

class AnalysisLane {
public:
    using CpuLiveCompute = std::function<bool(const Node&, const SourceFrameView*,
                                              const std::array<const uint8_t*, kMaxInputs>&, uint8_t*, size_t)>;
    using LiveFill = std::function<bool(const Node&, uint64_t, int, Header&, uint32_t)>;

    // `indicator` is the host's central Indicator (not owned; may be null: the lane is then unavailable).
    // Resync Q0 (plan 50 gap G3): the lane holds the Indicator by SHARED HANDLE (BindIndicator, rebound on an era
    // change by FrameSeal) and, with SetExternalSeal(true), never seals it itself: FrameSeal owns the one seal per
    // frame and calls OnSealed(seq, changed). The raw-pointer constructor is the standalone path (the lane seals
    // itself in OnRenderStart); it is kept for the lane's existing tests and must not be used next to a FrameSeal.
    AnalysisLane(VpIndicator::Indicator* indicator, std::function<uint64_t()> nowUs)
        : ind_(indicator), indEra_(indicator != nullptr ? indicator->Era() : 0u), nowUs_(std::move(nowUs))
    {
    }
    AnalysisLane(std::shared_ptr<VpIndicator::Indicator> indicator, std::function<uint64_t()> nowUs)
        : ind_(indicator.get()), indEra_(indicator ? indicator->Era() : 0u), indHandle_(std::move(indicator)),
          nowUs_(std::move(nowUs))
    {
    }
    AnalysisLane(const AnalysisLane&) = delete;
    AnalysisLane& operator=(const AnalysisLane&) = delete;

    // The frame-bound source-resolution monitor's readiness (Monitor::ReadyFor), consulted by
    // DeterministicWaitUs after the lane's own products. `onMissed` runs once per frame whose bound ran
    // out (Monitor::NoteWaitMissed). Setup / render thread. Null = none (today's behaviour).
    void SetReadyProbe(std::function<bool(uint64_t)> ready, std::function<void()> onMissed)
    {
        readyProbe_ = std::move(ready);
        readyMissed_ = std::move(onMissed);
    }
    uint64_t ReadyProbeMisses() const { return readyProbeMisses_; }

    // ---- FrameSeal integration (render thread) ----

    // Rebind the lane to a (new-era) Indicator. A state built on the previous Indicator keeps it alive through
    // LaneState::indHold, so a worker never reads a destroyed Indicator; the next Configure rebuilds the graph
    // (the era is part of the graph key) on the new one. The first observation after a rebind is not a change.
    void BindIndicator(const std::shared_ptr<VpIndicator::Indicator>& indicator)
    {
        // The same Indicator is the same ERA (eras are process-unique), never the same address: a freed
        // Indicator's address may be reused by the next era's.
        const uint32_t era = indicator ? indicator->Era() : 0u;
        if (era == indEra_ && (era != 0 || ind_ == nullptr)) {
            indHandle_ = indicator;   // the same Indicator: only adopt the shared handle, nothing to rebuild
            ind_ = indicator.get();
            return;
        }
        indHandle_ = indicator;
        ind_ = indicator.get();
        indEra_ = era;
        observedOnce_ = false;
        waitValid_ = false;
        ++indicatorRebinds_;
        // The built graph, ring slots and runner belong to the previous Indicator's era: their stamps
        // can no longer match. The next OnRenderStart rebuilds them from the last Configure inputs
        // (RebuildOnRebind); until then the status says so (never a silent in-render fallback).
        if (std::atomic_load(&state_))
            rebuildPending_ = true;
    }
    // True between BindIndicator and the rebuild at the next OnRenderStart.
    bool RebuildPending() const { return rebuildPending_; }
    uint64_t RebindRebuilds() const { return rebindRebuilds_; }
    // FrameSeal owns the seal: OnRenderStart no longer seals; the lane learns the frame's changed mask
    // through OnSealed (called by FrameSeal before OnRenderStart of the same frame).
    void SetExternalSeal(bool on) { externalSeal_ = on; }
    bool ExternalSeal() const { return externalSeal_; }
    // Render thread. `changed` is FrameSeal's SealAndObserve result for `seq`.
    void OnSealed(uint64_t seq, VpIndicator::CounterMask changed)
    {
        if (observedOnce_ && changed != 0)
            regulator_.OnIndicatorChange(changed);   // the first observation is not a change
        observedOnce_ = true;
        sealedSeq_ = seq;
        haveSealedSeq_ = true;
        ApplyStorm();
    }
    // The resync storm hold (Shadow::StormSafeSettings().laneT0): while held the regulator is kept at the
    // conservative floor (T0). Released when the storm ends; the regulator then recovers as usual.
    void SetStormHold(bool held)
    {
        if (held && !stormHold_) {
            ++stormHoldEngagements_;
            stormEdgePending_ = true;   // the engagement itself is traced once, even when already at T0
        }
        stormHold_ = held;
    }
    bool StormHold() const { return stormHold_; }
    uint64_t StormHoldEngagements() const { return stormHoldEngagements_; }
    uint64_t IndicatorRebinds() const { return indicatorRebinds_; }
    // OnRenderStart calls in external-seal mode whose frame was not sealed by FrameSeal first (a wiring bug:
    // counted, never silently sealed twice).
    uint64_t ExternalSealMisses() const { return externalSealMisses_; }

    // ---- setup (before the lane threads start) ----

    // Setup only: before the lane worker threads start (LaneWorkerThreads::Start marks them). A product
    // WITHOUT a producer gets no lane (it is in-render-only: the seal fills it live, nothing waits for
    // it, nothing counts it missing); register producers before Configure. A producer registered later
    // is wired into the live graph at once (not regulated until the next rebuild).
    void RegisterProducer(ILaneProducer* p)
    {
        if (p == nullptr)
            return;
        assert(workersRunning_.load(std::memory_order_acquire) == 0 && "RegisterProducer after the lane threads started");
        producers_.push_back(p);
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (st) {
            st->runner->Register(p);
            ApplyRegulator(*st);
        }
    }
    // Registered producers (tests / diagnostics): how many, and whether this exact object is one of them. Read-only; no behaviour reads them.
    size_t ProducerCount() const { return producers_.size(); }
    bool HasProducer(const ILaneProducer* p) const { return std::find(producers_.begin(), producers_.end(), p) != producers_.end(); }
    // LaneWorkerThreads tells the lane whether its threads run (setup-order assertion and async CPU worker lead).
    void SetWorkersRunning(bool running)
    {
        workersRunning_.store(running ? 1 : 0, std::memory_order_release);
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (st && st->runner)
            st->runner->SetAsyncCpuWorkers(running);
    }
    void SetGpuDispatcher(IGpuDispatcher* d) { dispatcher_ = d; }
    void SetLiveFill(LiveFill fill) { liveFill_ = std::move(fill); }
    void RegisterCpuLiveCompute(ProductId id, CpuLiveCompute fn) { cpuLive_[id] = std::move(fn); }

    // ---- B1 GET-OR-COMPUTE-AND-PUBLISH (refs/126 s1.2; SkeletonAcquire.h / SkeletonAcquireLane.h) ----
    // The registered CPU live computes ARE the owning units' fills of their lane products: Acquire runs them through
    // ComputeCpuOwnerForm (the same RunCpuLive law as the seal's live fill, never a second law). A product a reader
    // already published for frame N before its seal (an Acquire memo of the default variant) is COPIED into the slot's
    // live buffer by the seal instead of being computed twice (`PublishedLookup`, counted SealReusedPublished()). A frame an
    // Acquire had the lane make AHEAD (RunAheadNow) is accepted by its seal and counted there too (design s9.4 b; AheadSealReused()).
    // Render thread.
    using PublishedLookup = std::function<bool(ProductId, uint64_t, uint8_t*, size_t)>;
    using InputResolver = std::function<const uint8_t*(ProductId, uint64_t, size_t)>;
    void SetPublishedLookup(PublishedLookup f) { publishedLookup_ = std::move(f); }
    uint64_t SealReusedPublished() const { return sealReusedPublished_; }
    std::vector<ProductId> CpuLiveProducts() const
    {
        std::vector<ProductId> ids;
        for (const auto& kv : cpuLive_)
            ids.push_back(kv.first);
        return ids;
    }
    bool HasGpuDispatcher() const { return dispatcher_ != nullptr; }

    // ---- QKP slice C (design s2.4 / s9.4): the lane CLAIM for a frame ahead of the render frame ----
    // The render thread asks the lane to make product `id` of frame `seq` NOW, into the ring slot of that frame, instead of the memo
    // making a second copy: the same Claim / Publish protocol the worker runs (a compare-exchange on the slot word, so the worker and
    // this call can never both own a frame), the same RunCpuLive law with the inputs pinned. The frame is marked made-by-render
    // (SlotSet::MarkRenderMade): the worker that finds the slot taken counts WorkerReused (LaneRunner), the seal counts it reused.
    //   Ran      the frame is Published in its slot (or already was: a reuse): the caller reads it through ReadOf
    //   Claimed  a worker is writing it right now: nothing is computed twice, the caller reports LanePending (visible, no wait)
    //   NotNode  no graph / not a node of this graph: the memo's own owner fill path
    //   Declined why names the reason (visible): beyond the lane's slots, an input not published, dropped, a GPU form outside a
    //            GPU-legal phase, no compute registered, an ordered chain (worker only), the fill refused
    // A GPU lane product is never dispatched here: its ahead form is recorded by the seal / RunDeferredLive at Entrance, and a call
    // outside that phase is Declined "lane GPU ahead: not at a GPU-legal phase" (design s9.4 d). Render thread.
    // C5-SKREDS (get-or-compute of the inputs, OD 11-12 17:26 / 11-13 13:34): an input of the frame that is itself a CPU lane product
    // AHEAD of the render frame and not made yet is made FIRST by its own claim (the same law, into its own slot: one copy), then the
    // pins are tried once more. Without it an ahead ask of P4 declined whenever no lane worker had made P1 of that frame (at lane
    // lead 1 the workers publish nothing ahead by themselves), so a reader's ask could never make the frame it is meant to make.
    // `ownerMayFill` (Declined only): the frame cannot be made in the lane's slots NOW (beyond its slots, or an input still not made),
    // but the product's owner form may make it in the memo's own spot (SkeletonMemo::Acquire then runs the owner fill, whose inputs
    // are got-or-computed through the same Acquire chain). False for every other decline (a GPU node, dropped, an ordered chain,
    // published outside the lane, no compute, the fill refused, ...): those stay declined.
    enum class AheadRun : uint8_t { Ran, Claimed, NotNode, Declined };
    struct AheadRunResult {
        AheadRun kind = AheadRun::NotNode;
        const char* why = "";
        bool ownerMayFill = false;
    };
    AheadRunResult RunAheadNow(ProductId id, uint64_t seq) { return RunAheadNowAt(id, seq, 0u); }
    // The input recursion is bounded by the graph (lag-0 edges are acyclic; a lead input is bounded by the slots); this is the guard.
    static constexpr unsigned kAheadInputDepth = 8u;
    AheadRunResult RunAheadNowAt(ProductId id, uint64_t seq, unsigned depth)
    {
        AheadRunResult out;
        LaneState* st = renderState_.get();
        if (st == nullptr || ind_ == nullptr) {
            out.why = "no lane graph";
            return out;
        }
        // "Ahead" is relative to the render frame (the last OnRenderStart). Configure already holds the graph, but before the first
        // seal there is no render frame: ReadOf could not read a Published slot made now (its ahead edge is unset), so a Ran here
        // would break "the caller reads it through ReadOf". NotNode: the memo's own owner fill path, as with no graph.
        if (!haveRenderSeq_) {
            out.why = "no render frame sealed yet";
            return out;
        }
        const int idx = st->IndexOf(id);
        if (idx < 0) {
            out.why = "not a lane product";
            return out;
        }
        const Node& n = st->graph.nodes[size_t(idx)];
        out.kind = AheadRun::Declined;
        if (n.backend != Backend::Cpu) {
            out.why = "lane GPU ahead: not at a GPU-legal phase";
            ++aheadGpuNotLegal_;
            return out;
        }
        for (ProductId e : externalIds_)
            if (e == id) {
                out.why = "published outside the lane";
                return out;
            }
        if (regulator_.StateOf(id) == ProductState::Dropped) {
            out.why = "dropped by the regulator";
            return out;
        }
        if (n.orderedChain) {
            out.why = "ordered chain: the lane worker makes it";
            return out;
        }
        if (cpuLive_.find(id) == cpuLive_.end() || !st->bufs[size_t(idx)]) {
            out.why = "no lane compute registered";
            return out;
        }
        if (seq < st->firstSeq) {
            out.why = "before the lane's first frame";
            return out;
        }
        SlotSet& set = st->table->At(size_t(idx));
        // The inputs are pinned BEFORE the claim (a claim abandoned for a missing input would leave the frame Stale for the worker).
        Header scratch;
        LaneInputs pins;
        // countWaits false: this is the render thread; the wait dedupe entries belong to the lane worker, which counts the wait once.
        if (!st->table->LaneAncestors(id, seq, scratch, pins, /*countWaits*/ false)) {
            // C5-SKREDS: the missing ahead CPU lane inputs are made first (their own claims), then the pins are tried once more.
            bool madeInput = false;
            if (depth < kAheadInputDepth) {
                for (size_t k = 0; k < n.inputCount; ++k) {
                    const InputRef& in = n.inputs[k];
                    uint64_t aseq = 0;
                    if (in.product == id || !InputSeq(in, seq, &aseq) || aseq <= renderSeq_)
                        continue;   // the product itself, or a render / sealed frame: never made here
                    Backend ib = Backend::Cpu;
                    if (!IsNode(in.product, &ib) || ib != Backend::Cpu)
                        continue;
                    const AheadRunResult ir = RunAheadNowAt(in.product, aseq, depth + 1u);
                    if (ir.kind == AheadRun::Claimed) {
                        out.kind = AheadRun::Claimed;   // a worker is writing the input right now: LanePending (visible, no wait)
                        out.why = "a lane worker is making an input";
                        return out;
                    }
                    madeInput = madeInput || ir.kind == AheadRun::Ran;
                }
            }
            if (!madeInput || !st->table->LaneAncestors(id, seq, scratch, pins, /*countWaits*/ false)) {
                out.why = "an input is not published yet";
                out.ownerMayFill = true;
                return out;
            }
        }
        LaneTicket t;
        const ClaimResult cr = set.Claim(seq, Provenance::AheadLane, ~uint64_t(0), &t);
        if (cr == ClaimResult::TooEarly || cr == ClaimResult::ReleasePending || cr == ClaimResult::OutOfRange) {
            pins.Unpin();
            out.why = "beyond the lane's slots";   // design s9.4 a: not LanePending (no worker will ever have it)
            out.ownerMayFill = cr != ClaimResult::OutOfRange && (n.inputCount != 0 || st->ring->Find(seq, nullptr));   // inside the window the memo's own spot may hold it once its source is held
            ++aheadBeyondSlots_;
            return out;
        }
        if (cr == ClaimResult::Stale) {
            pins.Unpin();
            const SlotState ss = set.StateOf(seq);
            if (ss == SlotState::Writing) {
                out.kind = AheadRun::Claimed;   // a worker owns it right now
                out.why = "a lane worker is making it";
                ++aheadClaimedByWorker_;
                return out;
            }
            if (HasContent(ss)) {
                out.kind = AheadRun::Ran;   // already made (worker or an earlier call): reused
                out.why = "already in its slot";
                ++aheadAlreadyMade_;
                return out;
            }
            out.why = "the frame left the lane's slots";
            return out;
        }
        Header& hdr = set.LaneHeader(t);
        if (!set.Writable(t)) {
            pins.Unpin();
            LaneTicket a = t;
            set.Abandon(a);
            out.why = "slot still pinned by a reader of the previous frame";
            return out;
        }
        hdr.ancestors = scratch.ancestors;
        hdr.ancestorCount = scratch.ancestorCount;
        hdr.chainReduced = scratch.chainReduced;
        hdr.reads = scratch.reads;
        hdr.honoured = ind_->StampOf(seq);   // read AFTER the pins: a later event is seen by the seal (mismatch -> live)
        hdr.costUs = 0;
        uint8_t* dst = st->bufs[size_t(idx)]->Ring(size_t(t.slot));
        const size_t bytes = st->bufs[size_t(idx)]->Bytes();
        const uint64_t t0 = nowUs_ ? nowUs_() : 0u;
        const ::ComputeOnce::FrameScope frameScope(static_cast<int64_t>(seq), "lane", static_cast<int64_t>(id));   // compute-once: the ahead job's frame
        const bool ok = RunCpuLive(*st, n, seq, 0u, dst, bytes, InputResolver(), true);
        const uint64_t t1 = nowUs_ ? nowUs_() : t0;
        if (!ok) {
            const bool held = st->ring->Find(seq, nullptr);
            pins.Unpin();
            LaneTicket a = t;
            set.Abandon(a);
            out.why = held ? "the fill refused" : "the frame's source is not held";
            return out;
        }
        VPV_COMPUTE_ONCE_AT(("lane:" + std::to_string(int(id))).c_str(), seq, ::ComputeOnce::Variant(0));   // compute-once: the ahead live compute (recorded only when computed)
        if (set.LaneContinue(t))
            set.LaneHeader(t).costUs = t1 > t0 ? uint32_t(t1 - t0) : 0u;
        set.MarkRenderMade(seq);   // before the Publish: a worker that sees the slot taken finds the mark
        const PublishResult pr = set.Publish(t);
        pins.Unpin();
        if (pr != PublishResult::Published) {
            set.ClearRenderMade(seq);   // not made by the render after all: no reuse may be counted for it (audit S3)
            out.why = "the seal took the frame first";
            return out;
        }
        ++aheadMadeByRender_;
        out.kind = AheadRun::Ran;
        out.why = "made now";
        return out;
    }
    // The ledger of slice C (render thread; the worker's skips are LaneRunner counters workerReused).
    uint64_t AheadMadeByRender() const { return aheadMadeByRender_; }
    uint64_t AheadClaimedByWorker() const { return aheadClaimedByWorker_; }
    uint64_t AheadAlreadyMade() const { return aheadAlreadyMade_; }
    uint64_t AheadBeyondSlots() const { return aheadBeyondSlots_; }
    uint64_t AheadGpuNotLegal() const { return aheadGpuNotLegal_; }
    uint64_t AheadSealReused() const { return aheadSealReused_; }
    // GF-P2b MUST-4 (2): Configures that kept the graph but rebuilt the state because its lane decisions moved, and Configures that kept
    // the state and re-planned only the other VRAM users (both shown on the status lines).
    uint64_t PlanMovedRebuilds() const { return planMovedRebuilds_.load(std::memory_order_relaxed); }
    uint64_t UsersReplans() const { return usersReplans_.load(std::memory_order_relaxed); }
    // The CURRENT governor plan of every VRAM user: a kept state's users re-plan when one was made for it, else the state's own plan
    // (null without a state). The skeleton reads the memo's grant (kUserSkeletonMemo) and the users' shortfall here. Any thread.
    std::shared_ptr<const GovernorPlan> CurrentGovernorPlan() const
    {
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st)
            return std::shared_ptr<const GovernorPlan>();
        const std::shared_ptr<const UsersPlan> up = std::atomic_load(&usersPlan_);
        if (up && up->generation == st->generation)
            return std::shared_ptr<const GovernorPlan>(up, &up->plan);
        return std::shared_ptr<const GovernorPlan>(st, &st->plan.governor);
    }
    // The seal compute in a DECLARED reduced form (an input missing at the seal): counted apart from sealComputed, which is the
    // compute-once ledger of the full form (L-CORE s1 SHOULD): the two are different results of one frame.
    uint64_t SealComputedReduced() const { return sealComputedReduced_; }
    // The product is a node of the current graph (render thread); `backend` receives its backend.
    bool IsNode(ProductId id, Backend* backend = nullptr) const
    {
        const LaneState* st = renderState_.get();
        if (st == nullptr)
            return false;
        const int idx = st->IndexOf(id);
        if (idx < 0)
            return false;
        if (backend != nullptr)
            *backend = st->graph.nodes[size_t(idx)].backend;
        return true;
    }
    bool RegulatorDropped(ProductId id) const { return regulator_.StateOf(id) == ProductState::Dropped; }
    // Bytes of a CPU product's frame buffer in the current graph (0 = not a CPU node / no graph).
    size_t CpuBytesOf(ProductId id) const
    {
        const LaneState* st = renderState_.get();
        if (st == nullptr)
            return 0;
        const int idx = st->IndexOf(id);
        return idx < 0 || !st->bufs[size_t(idx)] ? size_t(0) : st->bufs[size_t(idx)]->Bytes();
    }
    // The owning unit's CPU form of product `id` for frame `seq` into `dst` (n bytes): the registered live compute with
    // the inputs read from the slots exactly like the seal's live fill; an input the slots do not hold is asked of
    // `inputOf` (the Acquire chain: get-or-compute of the input). False = no node / no registered compute / declined.
    bool ComputeCpuOwnerForm(ProductId id, uint64_t seq, uint8_t* dst, size_t n, const InputResolver& inputOf = InputResolver())
    {
        const LaneState* st = renderState_.get();
        if (st == nullptr || dst == nullptr)
            return false;
        const int idx = st->IndexOf(id);
        if (idx < 0 || !st->bufs[size_t(idx)] || n != st->bufs[size_t(idx)]->Bytes())
            return false;
        const ::ComputeOnce::FrameScope frameScope(static_cast<int64_t>(seq), "lane", static_cast<int64_t>(id));   // compute-once: the owner form's frame (req.seq)
        const bool ok = RunCpuLive(*st, st->graph.nodes[size_t(idx)], seq, 0u, dst, n, inputOf);
        if (ok)
            VPV_COMPUTE_ONCE_AT(("lane:" + std::to_string(int(id))).c_str(), seq, ::ComputeOnce::Variant(0));   // compute-once: the Acquire CPU owner form (recorded only when computed)
        return ok;
    }
    // SK-W stage D: products whose frames are PUBLISHED by a producer outside the lane (the vector engine feed for P2m / P3, the camera
    // fit worker for P7 ...): the lane never makes them, so the seal's live fill declines them (counted, shown) instead of recording a GPU
    // fill nothing can run. A frame the outside producer published in time is sealed like any lane result. Render thread.
    void SetExternalProducts(const std::vector<ProductId>& ids) { externalIds_ = ids; }
    const std::vector<ProductId>& ExternalProducts() const { return externalIds_; }
    uint64_t ExternalDeclined() const { return externalDeclined_; }
    // The render-device release fence's completed value for a product (lane polls it before reuse).
    void SetReleaseCompleted(std::function<uint64_t(ProductId)> f) { releaseCompleted_ = std::move(f); }
    // QKP slice C (R14-73): the memo's ledgers. Set before the lane starts (SkeletonAcquireLane.h BindLaneToMemo). `reduced` is told on
    // the render thread when a seal computed a DECLARED reduced form; `reused` on the worker when it skipped a frame the render thread
    // made first (the sink must be thread-safe: the memo's Note* take its mutex).
    void SetAheadSinks(std::function<void(ProductId)> reduced, std::function<void(ProductId)> reused)
    {
        sealReducedSink_ = std::move(reduced);
        workerReusedSink_ = std::move(reused);
    }
    // C5-SKREDS MUST-1: the lane WORKER's twin of SetPublishedLookup (LaneRunner::SetAdopt): a frame the product's owner fill already
    // published for an Acquire reader is copied into the worker's claimed slot, not computed again. Set like the sinks above (the
    // binder runs with the worker threads joined); called on the lane threads (the callee must be thread-safe: the memo's mutex).
    using WorkerAdopt = std::function<bool(ProductId, uint64_t, uint8_t*, size_t)>;
    void SetWorkerAdopt(WorkerAdopt f) { workerAdopt_ = std::move(f); }
    // 70b G0: GPU live deferral. On, a GPU-compute product's live fill at the seal (OnRenderStart, BEFORE the
    // render lock: no GPU recording there) only reserves the live buffer and records a LivePending entry; the
    // render thread later calls RunDeferredLive(seq) at a GPU-legal phase (Entrance), which dispatches the
    // entries in topological order through IGpuDispatcher::DispatchLive. Off (default): today's behaviour (the
    // dispatcher records at the seal). Needs a dispatcher; liveFill_ still wins when set (unchanged precedence).
    void SetGpuLiveDeferred(bool on) { gpuLiveDeferred_ = on; }
    bool GpuLiveDeferred() const { return gpuLiveDeferred_; }

    // ---- render thread ----

    // OnSettings: advance the graph (GraphOwner::Update), on a rebuild build the new LaneState (tier plan
    // through the VRAM governor, lane grants, runner, regulator, gap chunk cap) and swap it in. Returns
    // the rebuild reasons (0 = graph kept). Mode Off clears everything and returns 0.
    uint32_t Configure(const LaneSettings& s, const Registry& reg, const std::vector<ConsumerDecl>& consumers,
                       const GraphSettings& gsIn, const CardProfile& card, const TierInput& tier, uint64_t fromSeq,
                       bool hostDeterministic, uint64_t workerSkipBefore = 0)
    {
        // Budget-governed history: the depths the governor granted (a previous plan) ride with the graph settings so a
        // repeated Configure by a host that knows nothing of them keeps the same graph (no rebuild flapping).
        GraphSettings gs = gsIn;
        if (gs.historyCaps.empty())
            gs.historyCaps = historyCaps_;
        settings_ = s;
        mode_.store(uint8_t(s.mode), std::memory_order_release);
        if (s.mode != LaneSettings::Mode::Off) {
            // The inputs of the last Configure, kept so an Indicator rebind can rebuild (RebuildOnRebind).
            // `cfg` may alias the previously saved copy (the rebuild passes a local copy, so it does not).
            cfg_.reset(new SavedConfig{ reg, consumers, gs, card, tier, hostDeterministic });
        }
        rebuildPending_ = false;
        if (s.mode == LaneSettings::Mode::Off || ind_ == nullptr) {
            Clear();
            error_ = s.mode == LaneSettings::Mode::Off ? "" : "no indicator";
            return 0;
        }
        const bool det = s.deterministic == LaneSettings::Det::Auto ? hostDeterministic
                                                                     : s.deterministic == LaneSettings::Det::On;
        const bool detChanged = det != det_;
        det_ = det;

        // Budget-governed history: plan the grant BEFORE the graph is (re)built, on a trial graph and a copy of the governor
        // (the headroom state is stepped only by the real Update in BuildState), so one Configure builds one graph and
        // advances the graph counter once. Re-planned on every Configure: the grant recovers when memory frees.
        {
            bool governed = false;
            for (const ProductDecl& d : reg.All())
                governed = governed || d.historyFloor != kHistoryFixed;
            if (governed) {
                const Graph trial = BuildGraph(reg, consumers, gs);
                if (trial.valid) {
                    VramGovernor probe = governor_;
                    gs.historyCaps = probe.Update(reg, trial, card, tier).historyCaps;
                }
            }
            historyCaps_ = gs.historyCaps;
        }
        const VpIndicator::Stamp stamp = ind_->StampOf(fromSeq);
        const uint32_t reasons = owner_.Update(reg, consumers, gs, stamp, ind_, fromSeq);
        const std::shared_ptr<LaneState> cur = std::atomic_load(&state_);
        const Graph& g = owner_.Current();
        if (!g.valid) {
            error_ = g.ErrorText();
            stateGeneration_.fetch_add(1, std::memory_order_acq_rel);
            std::atomic_store(&state_, std::shared_ptr<LaneState>());
            renderState_.reset();
            return reasons;
        }
        // A look-ahead input (lead > 0) is sealed by the K1 rule (SlotTable::Check, stamp source set in BuildState).
        error_.clear();
        periodMs_ = tier.framePeriodMs;
        chunkCapUs_.store(GapScheduler::DerivedChunkCapUs(tier.framePeriodMs, s.chunkUs), std::memory_order_relaxed);

        // GF-P2b MUST-4 (2) (lead ruling, replaces GV's shared-LaneState publish: a LaneState owns its ring / table / bufs / runner and
        // the runner's callbacks hold the raw state pointer, so a second state sharing them is unsafe): the graph is kept, but the VRAM
        // plan of that graph may have moved (a new card reading, a new other-user such as the memo's raised GPU need). A PROBE on a copy
        // of the governor plans the kept graph with this card / tier.
        // - The lane's OWN decisions equal (every product's grant / tier / history depth, the product users' grants, the history caps,
        //   the over-budget flags): the state is kept as before (never mutated). If only the OTHER users' grants moved (the memo,
        //   the optical-flow engines, ...: ids below kUserProductBase), the probe's governor plan is published as the current users
        //   plan (CurrentGovernorPlan; the lane's products are unaffected), counted UsersReplans and shown.
        // - The lane's own decisions moved: fall through to BuildState on the SAME graph (the capsMismatch check stays). An ordinary
        //   reconfigure, not a resync: a new state generation (leases re-lease), failed_ re-keyed by the state pointer at the next
        //   OnRenderStart, ahead claims and render-made marks of the old tables gone (the next ask makes the frame once again), the
        //   ring's frames carried over, nothing outside the lane told (no vector-validity or resync event). Counted (PlanMovedRebuilds)
        //   and shown on the lane status lines.
        bool planMoved = false;
        if (reasons == 0 && cur) {
            VramGovernor probe = governor_;
            const TierPlan& probed = probe.Update(reg, cur->graph, card, tier);
            planMoved = !SameLaneDecisions(probed, cur->plan);
            if (planMoved) {
                planMovedLastSeq_.store(fromSeq, std::memory_order_relaxed);
                planMovedRebuilds_.fetch_add(1, std::memory_order_relaxed);
            } else {
                const std::shared_ptr<const GovernorPlan> users = CurrentGovernorPlan();
                if (!users || !SameUserGrants(probed.governor, *users)) {
                    std::shared_ptr<UsersPlan> up = std::make_shared<UsersPlan>();
                    up->generation = cur->generation;
                    up->plan = probed.governor;
                    std::atomic_store(&usersPlan_, std::shared_ptr<const UsersPlan>(up));
                    usersReplans_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (reasons == 0 && cur && !planMoved) {
            cur->gap.SetChunkCapUs(ChunkCapUs());
            if (detChanged) {
                ConfigureRegulator();
                ApplyRegulator(*cur);   // the regulator's states changed under the kept graph
            }
            renderState_ = cur;
            return 0;
        }

        std::shared_ptr<LaneState> st = BuildState(reg, consumers, gs, card, tier, fromSeq, workerSkipBefore);
        if (!st->capsMismatch.empty()) {
            // The graph was sized for the trial's history grant but the real plan grants otherwise: the memory plan
            // does not describe the rings. Fail closed and visibly (Analysis lane unavailable + the Fallbacks entry).
            error_ = st->capsMismatch;
            stateGeneration_.fetch_add(1, std::memory_order_acq_rel);
            std::atomic_store(&state_, std::shared_ptr<LaneState>());
            renderState_.reset();
            return reasons;
        }
        // Regulator products from the new graph.
        ConfigureRegulatorFor(*st, reg, consumers, gs, card, tier);
        lateRingSize_ = periodMs_ > 0.0 ? size_t(std::max(1.0, 10000.0 / periodMs_ + 0.5)) : size_t(0);
        lateRing_.assign(lateRingSize_, 0u);
        lateRingPos_ = 0;
        lateRingSum_ = 0;

        if (cur)
            st->ring->CopyFrom(*cur->ring);
        const uint32_t kCarryReasons = kRebuildQueue | kRebuildCard | kRebuildConsumers | kRebuildHistory;
        const bool mayCarryCpu = cur && (reasons & ~kCarryReasons) == 0u;
        if (mayCarryCpu)
            CarryCpuFrom(*cur, *st, fromSeq);
        NotifyNewest(*st);
        std::atomic_store(&state_, st);
        st->runner->SetAsyncCpuWorkers(workersRunning_.load(std::memory_order_acquire) != 0);
        if (cur) {   // frames stored in the old ring while the swap happened
            if (mayCarryCpu)
                CarryCpuFrom(*cur, *st, fromSeq);
            st->ring->CopyFrom(*cur->ring);
            NotifyNewest(*st);
            cur->ring->Clear();   // the new ring carries the frames: the host pool is not held twice
        }
        renderState_ = st;
        ApplyRegulator(*st);
        waitValid_ = false;
        NotifyWake();
        return reasons;
    }

    // Render start of frame `seq`: seal it in the Indicator (reporting what changed to the regulator),
    // then seal every product in topological order. A regulator-dropped product declines (FillDeclined,
    // visible); CPU products fill live through their registered one-shot compute (bit-identical to the
    // lane); others through SetLiveFill, else the GPU dispatcher. The returned reference stays valid
    // until the next OnRenderStart / Configure. Shadow mode computes the same results (ShadowOnly()).
    const std::vector<ProductFrameResult>& OnRenderStart(uint64_t seq, uint32_t presentSlackUs)
    {
        if (settings_.mode == LaneSettings::Mode::Off || ind_ == nullptr)
            return empty_;
        if (rebuildPending_)
            RebuildOnRebind(seq);
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st)
            return empty_;
        // 70b G0: entries the previous frame never ran (no Entrance reached) are stale: failed, counted.
        for (const LivePending& e : livePending_)
            MarkFailed(e.id, e.seq);
        deferredTotals_.stale += livePending_.size();
        livePending_.clear();
        if (pendingState_ != st.get())
            failed_.clear();   // another graph: its failures named the old graph's slots
        pendingState_ = st.get();
        renderState_ = st;
        renderSeq_ = seq;   // ReadOf: frames after this one are ahead (their Published slot is readable)
        haveRenderSeq_ = true;
        lastSlackUs_ = presentSlackUs;

        if (externalSeal_) {
            // One seal owner: FrameSeal sealed this frame and reported it through OnSealed.
            if (!haveSealedSeq_ || sealedSeq_ != seq)
                ++externalSealMisses_;
        } else {
            const VpIndicator::CounterMask changed = ind_->SealAndObserve(seq);
            if (observedOnce_ && changed != 0)
                regulator_.OnIndicatorChange(changed);   // the first observation is not a change
            observedOnce_ = true;
            ApplyStorm();
        }
        regulator_.OnUnfedFrame(stormHold_);   // B4 S3: an unfed step-down recovers (no-op while fed)
        ApplyRegulator(*st);
        if (seq < st->firstSeq)
            return empty_;

        LeadInput li;
        li.presentSeq = seq >= st->graph.settings.renderAhead ? seq - st->graph.settings.renderAhead : 0u;
        uint64_t newest = 0;
        if (!st->ring->Newest(&newest))
            newest = seq;
        li.newestQueuedSeq = newest;
        li.renderAhead = st->graph.settings.renderAhead;
        li.laneLead = st->graph.settings.laneLead;
        li.mode = st->graph.settings.queueMode;
        lead_ = CheckLead(li);

        const VpIndicator::Stamp stamp = ind_->StampOf(seq);
        LaneState* sp = st.get();
        auto fill = [this, sp](const Node& n, uint64_t s, int live, Header& hdr, uint32_t missing) {
            if (regulator_.StateOf(n.id) == ProductState::Dropped)
                return false;   // dropped by the regulator: declined, visible
            return LiveFillOf(*sp, n, s, live, hdr, missing);
        };
        st->table->SetDeterministicSeal(det_);   // B3 X1: a deterministic run seals lagged-seal products at the final seal only
        const std::vector<ProductFrameResult>& res = st->table->Resolve(seq, stamp, fill);
        lastAhead_ = lastInRender_ = lastDeclined_ = 0;
        for (const ProductFrameResult& r : res) {
            if (r.seal.outcome == SealOutcome::Sealed && r.seal.reason == SealReason::Accepted) {
                ++lastAhead_;
                const int ri = st->IndexOf(r.id);
                if (ri >= 0 && st->table->At(size_t(ri)).RenderMade(seq)) {
                    ++aheadSealReused_;   // design s9.4 b: the seal of a frame RunAheadNow made accepts it, computes nothing
                    ++sealReusedPublished_;   // s9.4 b "counted sealReused": the Acquire's (ahead) publication reused by the seal; AheadSealReused is its share
                }
            }
            if (r.liveFilled)
                ++lastInRender_;
            if (r.final == SealOutcome::Unavailable && r.finalReason == SealReason::FillDeclined)
                ++lastDeclined_;
        }
        if (st->graph.maxSealLag > 0) {
            // B3 X1: the final seal of lagged-seal frames N = seq - sealLag (after this frame's seal, so a lead ancestor is
            // sealed). Their classes are counted on the product's set (ahead / live-filled / late / absent, StatusLines).
            const std::vector<ProductFrameResult>& lag = st->table->ResolveLagged(seq, stamp, fill);
            for (const ProductFrameResult& r : lag) {
                if (r.liveFilled)
                    ++lastInRender_;
                if (r.final == SealOutcome::Unavailable && r.finalReason == SealReason::FillDeclined)
                    ++lastDeclined_;
            }
        }
        totalAhead_ += lastAhead_;
        totalInRender_ += lastInRender_;
        totalDeclined_ += lastDeclined_;
        PruneCarriedCpu(seq);
        return res;
    }

    // N1: before OnRenderStart a deterministic run (or an explicit test wait) may hold the seal a
    // bounded time for a lane still working on a product whose in-render form would differ. Returns the
    // microseconds to wait now (0 = seal now). Call it in a loop; each call assumes the previous
    // returned time was waited:  while ((w = DeterministicWaitUs(seq, slack)) > 0) Wait(w);
    // A bound that ran out is counted once per frame (determinism fallback).
    uint32_t DeterministicWaitUs(uint64_t seq, uint32_t presentSlackUs)
    {
        if (settings_.mode == LaneSettings::Mode::Off)
            return 0;
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st || (!det_ && settings_.testWaitUs == 0))
            return 0;
        if (!waitValid_ || seq != waitSeq_) {
            waitValid_ = true;
            waitSeq_ = seq;
            waitedUs_ = 0;
            lastAskUs_ = 0;
            missCounted_ = false;
        } else {
            waitedUs_ += lastAskUs_;
        }
        WaitPolicy p;
        p.deterministicRun = true;
        p.explicitTestWait = settings_.testWaitUs > 0;
        p.presentSlackUs = presentSlackUs;
        const double guard = regulator_.MarginMs() * 1000.0;
        p.guardUs = guard > 0.0 ? uint32_t(guard + 0.5) : 0u;
        p.testWaitUs = settings_.testWaitUs;
        uint32_t more = 0;
        bool missed = false;
        bool probeMissed = false;
        for (size_t i = 0; i < st->table->Count(); ++i) {
            const Node& n = st->table->NodeAt(i);
            if (n.backend == Backend::RenderOnly || !st->runner->Laned(n.id) || !st->runner->HasProducer(n.id))
                continue;   // no producer: nothing to wait for (the seal fills it live), no bound burnt
            // B3 X1: a lagged-seal product is sealed at its final seal only in a deterministic run: no wait at N; the wait (same
            // bound, same missed-bound count) is for its frame N = seq - sealLag, finalised at this render start (ResolveLagged).
            uint64_t waitSeq = seq;
            if (n.sealLag > 0) {
                if (seq < n.sealLag || seq - n.sealLag < st->firstSeq)
                    continue;   // no frame of this graph to finalise yet
                waitSeq = seq - n.sealLag;
            }
            uint32_t m = 0;
            const Variants v = st->info[i].identical ? Variants::Identical : Variants::NonIdentical;
            const SlotSet& set = st->table->At(i);
            const WaitDecision d = DecideWait(v, set.LaneGranted(), set.StateOf(waitSeq), p, waitedUs_, &m);
            if (d == WaitDecision::Wait)
                more = std::max(more, m);
            else if (d == WaitDecision::BoundMissed)
                missed = true;
        }
        // Q3 (N1): the source-resolution monitor's frame-bound decision needs the analysis through
        // s - D. Same bounded wait as a laned product (deterministic runs only, inside the present
        // slack, never a late present); a bound that runs out is counted here and reported to the
        // probe's owner (SRM NoteWaitMissed), so the run is visibly not reproducible.
        if (readyProbe_ && !readyProbe_(seq)) {
            uint32_t m = 0;
            const WaitDecision d = DecideWait(Variants::NonIdentical, true, SlotState::Writing, p, waitedUs_, &m);
            if (d == WaitDecision::Wait)
                more = std::max(more, m);
            else if (d == WaitDecision::BoundMissed) {
                missed = true;
                probeMissed = true;
            }
        }
        if (more > 0) {
            lastAskUs_ = more;
            return more;
        }
        lastAskUs_ = 0;
        if (missed && !missCounted_) {
            ++missedBounds_;
            missCounted_ = true;
            if (probeMissed) {
                ++readyProbeMisses_;
                if (readyMissed_)
                    readyMissed_();
            }
        }
        return 0;
    }

    // Post-submit idle gap: run the lane GPU-compute products (render thread) until gapEndUs.
    GapReport OnPostSubmitGap(uint64_t gapEndUs)
    {
        if (settings_.mode == LaneSettings::Mode::Off)
            return GapReport();
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st)
            return GapReport();
        for (const std::unique_ptr<LaneComputeGapWork>& w : st->gapWorks)
            w->Wake();
        return st->gap.RunGap(gapEndUs, nowUs_);
    }

    // ---- 70b G0: deferred GPU live fills, product reads (render thread) ----

    struct LivePending {
        ProductId id = kNoProduct;
        uint64_t seq = 0;
        int live = -1;
        Header hdr;
        uint32_t missing = 0;
    };
    struct DeferredReport {
        uint64_t run = 0;       // dispatched and succeeded
        uint64_t failed = 0;    // DispatchLive returned false, or an input failed and no missing-input form
        uint64_t stale = 0;     // entries of an earlier frame or graph (never dispatched; their reads say Failed)
    };

    // The GPU live fills still waiting for RunDeferredLive (render thread; topological order).
    const std::vector<LivePending>& LivePendingList() const { return livePending_; }

    // Entrance of frame `seq`: dispatch its pending GPU live fills in topological order. An entry whose input
    // failed this frame runs in the product's missing-input form when declared, else fails too (cascade).
    // Entries of another frame or graph are stale: never dispatched, counted, their reads say Failed.
    DeferredReport RunDeferredLive(uint64_t seq)
    {
        DeferredReport r;
        if (livePending_.empty())
            return r;
        std::vector<LivePending> todo;
        todo.swap(livePending_);
        LaneState* st = renderState_.get();
        for (const LivePending& e : todo) {
            if (st == nullptr || e.seq != seq || pendingState_ != st) {
                MarkFailed(e.id, e.seq);
                ++r.stale;
                continue;
            }
            const int idx = st->IndexOf(e.id);
            if (idx < 0 || dispatcher_ == nullptr) {
                MarkFailed(e.id, e.seq);
                ++r.failed;
                continue;
            }
            const Node& n = st->table->NodeAt(size_t(idx));
            uint32_t missing = e.missing;
            for (size_t k = 0; k < n.inputCount; ++k)
                if (seq >= n.inputs[k].lag && IsFailed(n.inputs[k].product, seq - n.inputs[k].lag))
                    missing |= 1u << k;
            const bool ok = (missing == e.missing || n.missingInputForm)
                            && dispatcher_->DispatchLive(n, seq, e.live, e.hdr, missing);
            if (ok) {
                ++r.run;
            } else {
                MarkFailed(e.id, e.seq);
                ++r.failed;
            }
        }
        deferredTotals_.run += r.run;
        deferredTotals_.failed += r.failed;
        deferredTotals_.stale += r.stale;
        return r;
    }
    const DeferredReport& DeferredTotals() const { return deferredTotals_; }

    // Where the render reads product `id` of frame `seq` (render thread). A GPU live fill still pending is
    // Pending, one whose deferred dispatch failed is Failed: both carry no ReadInfo (kind None), so a reader
    // falls back exactly as for a missing product. CPU products also give their bytes.
    struct ProductRead {
        enum State : uint8_t { None, Ring, Live, Pending, Failed } state = None;
        ReadInfo info;
        const uint8_t* cpu = nullptr;
        size_t cpuBytes = 0;
    };
    ProductRead ReadOf(ProductId id, uint64_t seq) const
    {
        ProductRead out;
        const LaneState* st = renderState_.get();
        if (st == nullptr)
            return out;
        const int idx = st->IndexOf(id);
        if (idx < 0)
            return out;
        if (IsFailed(id, seq)) {
            out.state = ProductRead::Failed;
            return out;
        }
        for (const LivePending& e : livePending_)
            if (e.id == id && e.seq == seq) {
                out.state = ProductRead::Pending;
                return out;
            }
        // QKP slice C (RunAheadNow contract: "the caller reads it through ReadOf"): a CPU product's frame AHEAD of the render frame
        // (after the last OnRenderStart) is read from its Published slot: its seal has not run yet, and RunAheadNow / the worker made
        // it with the one law the seal accepts. The render frame and older frames stay sealed-only (a lagged seal is not bypassed).
        const bool ahead = haveRenderSeq_ && seq > renderSeq_ && st->graph.nodes[size_t(idx)].backend == Backend::Cpu;
        out.info = st->table->At(size_t(idx)).Read(seq, ahead);
        if (out.info.kind == ReadInfo::None)
            return out;
        out.state = out.info.kind == ReadInfo::Ring ? ProductRead::Ring : ProductRead::Live;
        const CpuBuffers* b = st->bufs[size_t(idx)].get();
        if (b != nullptr) {
            out.cpu = out.info.kind == ReadInfo::Ring ? b->Ring(size_t(out.info.index)) : b->Live(size_t(out.info.index));
            out.cpuBytes = b->Bytes();
        }
        return out;
    }

    // The source view of frame `seq` (render thread, lock-free mirror; no keepAlive: the render holds the frame).
    bool SourceOf(uint64_t seq, SourceFrameView* out) const
    {
        const LaneState* st = renderState_.get();
        return st != nullptr && st->ring->TryGet(seq, out);
    }

    // The frame's last use is over (presented / render-ahead slot retired).
    void OnFrameRetired(uint64_t seq, uint64_t releaseFenceValue)
    {
        if (settings_.mode == LaneSettings::Mode::Off)
            return;
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st)
            return;
        st->nextExpectedRetire = (std::max)(st->nextExpectedRetire, seq + 1u);
        // B3 X1: with a lagged-seal product (graph K = maxSealLag > 0) every release and the source drop run K frames later: a
        // frame and its ancestors stay until the final seal at N + K (the rings were sized + K). K = 0: today.
        const uint64_t K = st->graph.maxSealLag;
        for (size_t i = 0; i < st->table->Count(); ++i) {
            const uint64_t h = uint64_t(st->table->NodeAt(i).history) + K;
            // A frame that left its history before this graph's first frame was never in this table.
            if (seq >= h && seq - h >= st->firstSeq)
                st->table->At(i).ReleaseThrough(seq - h, releaseFenceValue);
        }
        if (seq >= K)
            st->ring->Drop(seq - K);   // the ring's copy (and its host plane hold) ends with the frame; a running job keeps its own
        NotifyWake();
    }

    // The frame will never be rendered: free its slot on every set.
    void OnFrameDropped(uint64_t seq)
    {
        if (settings_.mode == LaneSettings::Mode::Off)
            return;
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st || seq < st->firstSeq)
            return;
        if (seq == st->nextExpectedRetire) {
            st->nextExpectedRetire = seq + 1u;
            const uint64_t K = st->graph.maxSealLag;
            for (size_t i = 0; i < st->table->Count(); ++i) {
                const uint64_t h = uint64_t(st->table->NodeAt(i).history) + K;
                if (seq >= h && seq - h >= st->firstSeq)
                    st->table->At(i).ReleaseThrough(seq - h, 0);
            }
        }
        for (size_t i = 0; i < st->table->Count(); ++i)
            st->table->At(i).Discard(seq);
        st->ring->Drop(seq);
        NotifyWake();
    }

    // Per presented frame: the regulator's input (slack, render-only time, deadline missed) plus the
    // lanes' lateness since the last frame; the regulator's states are applied to the runner.
    void OnFrameTiming(uint64_t seq, double slackMs, double renderOnlyMs, bool missed)
    {
        if (settings_.mode == LaneSettings::Mode::Off)
            return;
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st)
            return;
        uint32_t lateNow = 0;
        for (size_t i = 0; i < st->table->Count(); ++i) {
            // A lane too far behind to claim shows as missing (counted only while granted), not late.
            const SlotCounters& c = st->table->At(i).Counters();
            const uint64_t cur = c.late.load(std::memory_order_relaxed);
            const uint64_t curMiss = c.missing.load(std::memory_order_relaxed);
            const uint64_t delta = (cur > st->lateSeen[i] ? cur - st->lateSeen[i] : 0u)
                                   + (curMiss > st->missSeen[i] ? curMiss - st->missSeen[i] : 0u);
            st->lateSeen[i] = cur;
            st->missSeen[i] = curMiss;
            if (delta > 0) {
                regulator_.OnLaneLate(st->table->NodeAt(i).id, uint32_t(delta));
                lateNow += uint32_t(delta);
            }
        }
        if (lateRingSize_ > 0) {
            lateRingSum_ -= lateRing_[lateRingPos_];
            lateRing_[lateRingPos_] = lateNow;
            lateRingSum_ += lateNow;
            lateRingPos_ = (lateRingPos_ + 1u) % lateRingSize_;
        }
        regulator_.OnFrame(seq, slackMs, renderOnlyMs, missed);
        if (lateNow > 0)
            st->runner->SetLateSkipBefore(seq + 1u);   // a late lane skips to the next frame now (lowered by sealLag, B3 X1)
        ApplyRegulator(*st);
    }

    // A source cut: the regulator steps down one level.
    void OnCut()
    {
        if (settings_.mode == LaneSettings::Mode::Off)
            return;
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st)
            return;
        regulator_.OnCut();
        ApplyRegulator(*st);
    }

    // ---- capture / conversion thread ----

    void OnSourceFrame(const SourceFrameView& v)
    {
        if (IsOff())
            return;
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st)
            return;
        st->ring->Store(v);
        st->runner->OnSourceFrame(v.seq);
        NotifyWake();
    }

    // ---- lane worker threads (OFA worker for Ofa, CPU worker for Cpu) ----

    // Step every laned product of backend `b` once, in tier then topological order. True when any made
    // progress. budgetUs 0 = the chunk cap.
    bool StepWorker(Backend b, uint32_t budgetUs)
    {
        if (IsOff())
            return false;
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st)
            return false;
        const uint32_t budget = budgetUs != 0 ? budgetUs : chunkCapUs_.load(std::memory_order_relaxed);
        bool progress = false;
        for (ProductId id : st->workerOrder[size_t(b)])
            progress = LaneStepProgressed(st->runner->Step(id, budget, true)) || progress;
        return progress;
    }

    uint64_t WakeCount() const { return wake_.load(std::memory_order_acquire); }
    // Wait until the wake count differs from `seen` or the timeout passes (a lost notify is covered by it).
    bool WaitWake(uint64_t seen, uint32_t timeoutUs)
    {
        std::unique_lock<std::mutex> lock(wakeMutex_);
        return wakeCv_.wait_for(lock, std::chrono::microseconds(timeoutUs),
                                [&]() { return wake_.load(std::memory_order_acquire) != seen; });
    }
    void NotifyWake()
    {
        wake_.fetch_add(1, std::memory_order_release);
        wakeCv_.notify_all();
    }

    // ---- queries ----

    bool Active() const { return bool(std::atomic_load(&state_)); }
    bool ShadowOnly() const { return settings_.mode == LaneSettings::Mode::Shadow; }
    const LaneSettings& Settings() const { return settings_; }
    std::shared_ptr<LaneState> State() const { return std::atomic_load(&state_); }
    // The generation of the CURRENT state (0 = no state). Any thread.
    uint64_t Generation() const { const std::shared_ptr<LaneState> st = std::atomic_load(&state_); return st ? st->generation : 0; }
    // A lease on the CURRENT state for an outside worker (the camera fit thread): the shared hold keeps the table, the buffers
    // and the Indicator alive across any number of rebuilds; the holder compares `generation` with Generation() and rebinds.
    // `bytesProduct` names the CPU product whose bytes ring the holder writes (kInvalidProduct-style: pass a product not in the
    // graph, or leave default, for none). Empty lease (Valid() false) when the lane is inactive. Any thread.
    LaneLease Lease(ProductId bytesProduct = ProductId(0xFFFFu)) const
    {
        LaneLease l;
        // The counter is read BEFORE the state: an empty lease carries it, so a holder orders it against the states (LaneLeaseNewer).
        l.generation = stateGeneration_.load(std::memory_order_acquire);
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st || !st->table)
            return l;
        l.hold = st;
        l.table = st->table.get();
        l.indicator = st->indHold.get();
        l.generation = st->generation;
        const int i = st->IndexOf(bytesProduct);
        if (i >= 0 && size_t(i) < st->bufs.size() && st->bufs[size_t(i)])
            l.bytes = st->bufs[size_t(i)].get();
        return l;
    }
    const Regulator& GetRegulator() const { return regulator_; }
    uint32_t ChunkCapUs() const { return chunkCapUs_.load(std::memory_order_relaxed); }
    bool Deterministic() const { return det_; }
    uint64_t MissedBounds() const { return missedBounds_; }
    uint32_t Rebuilds() const { return owner_.Rebuilds(); }
    LeadState Lead() const { return lead_; }
    uint64_t TotalAhead() const { return totalAhead_; }
    uint64_t TotalInRender() const { return totalInRender_; }
    uint64_t TotalDeclined() const { return totalDeclined_; }
    size_t RegulatorTraces(RegTrace* out, size_t max) const { return regulator_.Traces(out, max); }

    // The active-picture walk to use: Bill's original unless the lane was requested AND P4 is available.
    VpLaneProducts::WalkSource EffectiveWalk() const
    {
        return VpLaneProducts::EffectiveWalk(settings_.walk, P4Available());
    }

    // Plain language, one line per fact.
    std::vector<std::string> StatusLines() const
    {
        std::vector<std::string> out;
        if (settings_.mode == LaneSettings::Mode::Off) {
            out.push_back("Analysis lane: off");
            return out;
        }
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st) {
            out.push_back(std::string("Analysis lane: unavailable") + (error_.empty() ? "" : " (" + error_ + ")"));
            return out;
        }
        out.push_back(settings_.mode == LaneSettings::Mode::Shadow ? "Analysis lane: shadow (results not used)"
                                                                    : "Analysis lane: on");
        out.push_back("Analysis products: " + std::to_string(lastAhead_) + " ahead, " + std::to_string(lastInRender_)
                      + " in render, " + std::to_string(regulator_.Dropped().size()) + " dropped");
        if (totalDeclined_ > 0)
            out.push_back("Analysis: " + std::to_string(totalDeclined_) +
                          " product frames unavailable (no producer result and no live fill; the consumer uses its own fallback)");
        out.push_back(std::string("Analysis lead: ") + LeadText(lead_));
        if (st->graph.maxSealLag > 0) {
            // B3 X1 (CHAR GAP-2): per lagged-seal product, how each frame ended; never log-only.
            for (size_t i = 0; i < st->table->Count(); ++i) {
                const Node& n = st->table->NodeAt(i);
                if (n.sealLag == 0)
                    continue;
                const SlotCounters& c = st->table->At(i).Counters();
                std::string line = "Analysis seal " + NameOf(*st, n.id) + " (seal lag " + std::to_string(unsigned(n.sealLag)) + "): " +
                                   std::to_string(c.sealAhead.load(std::memory_order_relaxed)) + " ahead, " +
                                   std::to_string(c.sealLiveFilled.load(std::memory_order_relaxed)) + " live-filled, " +
                                   std::to_string(c.sealLate.load(std::memory_order_relaxed)) + " late, " +
                                   std::to_string(c.sealAbsent.load(std::memory_order_relaxed)) + " absent";
                const uint64_t rej = c.lateResyncRejected.load(std::memory_order_relaxed);
                const uint64_t ota = c.onTimeAbsent.load(std::memory_order_relaxed);
                const uint64_t unf = c.sealUnfinalised.load(std::memory_order_relaxed);
                if (rej > 0)
                    line += " (" + std::to_string(rej) + " rejected by a resync)";
                if (unf > 0)
                    line += "; " + std::to_string(unf) + " never finalised (a render frame was skipped)";
                bool sealedLead = false;
                for (size_t k = 0; k < n.inputCount; ++k)
                    sealedLead = sealedLead || (n.inputs[k].lead > 0 && n.sealLag >= n.inputs[k].lead);
                if (sealedLead)
                    line += "; never ahead (it reads a lead frame sealed at its final seal: late = accepted at the final seal)";
                if (ota > 0)
                    line += "; readers found it absent on time x" + std::to_string(ota);
                if (det_)
                    line += "; deterministic: final seal only";
                out.push_back(line);
            }
        }
        if (st->graph.sealLagForcedZero)
            out.push_back("Analysis seal: lagged-seal products run at seal lag 0 in this mode (competitive low latency or no lane lead)");
        if (!externalIds_.empty()) {
            std::string names;
            for (ProductId id : externalIds_)
                if (st->IndexOf(id) >= 0)
                    names += (names.empty() ? "" : ", ") + NameOf(*st, id);
            if (!names.empty())
                out.push_back("Analysis: products published outside the lane: " + names + "; " + std::to_string(externalDeclined_) +
                              " frame(s) were not published in time (the readers use their own fallback)");
        }
        out.push_back("Analysis level: " + std::to_string(unsigned(regulator_.Level())) + "/"
                      + std::to_string(unsigned(regulator_.MaxLevel())));
        out.push_back("Analysis late: " + std::to_string(lateRingSum_) + " per 10 s");
        if (det_)
            out.push_back("Analysis deterministic: " + std::to_string(missedBounds_) + " frames missed the wait bound");
        if (rebuildPending_)
            out.push_back("Analysis: the Indicator changed (new source era); the graph is rebuilt at the next frame");
        if (indicatorRebinds_ > 0)
            out.push_back("Analysis: Indicator rebound x" + std::to_string(indicatorRebinds_) + ", graph rebuilt x" +
                          std::to_string(rebindRebuilds_));
        if (externalSealMisses_ > 0)
            out.push_back("Analysis: " + std::to_string(externalSealMisses_) +
                          " frames reached the lane without the shared seal (not sealed twice)");
        if (stormHold_)
            out.push_back("Analysis: held at T0 by the resync storm (" + std::to_string(stormHoldEngagements_) + " engagements)");
        uint64_t skipped = 0;
        for (size_t i = 0; i < st->table->Count(); ++i)
            skipped += st->runner->Counters(st->table->NodeAt(i).id).skippedDropped.load(std::memory_order_relaxed);
        if (skipped > 0)
            out.push_back("Analysis: " + std::to_string(skipped) + " frames skipped (source gone)");
        uint64_t carried = 0, refused = 0, chainWait = 0, chainWaitSeal = 0, pinWait = 0, storeWait = 0;
        for (size_t i = 0; i < st->table->Count(); ++i) {
            const LaneProductCounters& pc = st->runner->Counters(st->table->NodeAt(i).id);
            carried += pc.chainCarryForward.load(std::memory_order_relaxed);
            refused += pc.chainRefused.load(std::memory_order_relaxed);
            chainWait += st->table->At(i).Counters().chainWaitRejected.load(std::memory_order_relaxed);
            chainWaitSeal += st->table->At(i).Counters().chainWaitUnsealed.load(std::memory_order_relaxed);
            pinWait += st->table->At(i).Counters().writeWaitPinned.load(std::memory_order_relaxed);
            storeWait += st->table->At(i).Counters().storeWaitRejected.load(std::memory_order_relaxed);
        }
        uint64_t laterRejected = 0;
        for (size_t i = 0; i < st->table->Count(); ++i)
            laterRejected += st->table->At(i).Counters().leadLaterRejected.load(std::memory_order_relaxed);
        if (laterRejected > 0)
            out.push_back("Analysis: " + std::to_string(laterRejected)
                          + " look-ahead frames were rejected at their own seal after a reader used them");
        if (carried > 0)
            out.push_back("Analysis: " + std::to_string(carried)
                          + " ordered-chain frames ran in the reduced form (no predecessor result)");
        if (refused > 0)
            out.push_back("Analysis: " + std::to_string(refused) + " out-of-order chain frames refused");
        if (chainWait > 0)
            out.push_back("Analysis: " + std::to_string(chainWait)
                          + " ordered-chain frames waited on a rejected predecessor (its live fill pending, declined or without a buffer)");
        if (chainWaitSeal > 0)
            out.push_back("Analysis: " + std::to_string(chainWaitSeal)
                          + " ordered-chain frames waited for the predecessor's seal (a chain job pins only a sealed or live predecessor)");
        if (storeWait > 0)
            out.push_back("Analysis: " + std::to_string(storeWait)
                          + " single-store / held-state reader frames waited on a rejected store frame (a store has no live fill: the reader's lane job does not run for that frame and the render makes it in its declared reduced form, or it is unavailable)");
        if (!st->graph.leadChainWarning.empty())
            out.push_back("Analysis warning: " + st->graph.leadChainWarning);
        if (pinWait > 0)
            out.push_back("Analysis: " + std::to_string(pinWait)
                          + " lane jobs waited for a late reader to release their slot (single-store products wait on a lag-1 reader)");
        const GapReport& gt = st->gap.Totals();
        if (gt.capNoFit > 0)
            out.push_back("Analysis: " + std::to_string(gt.capNoFit) + " lane chunks deferred (too big for the chunk cap)");
        if (gt.laneStarvedGaps > 0)
            out.push_back("Analysis: lane compute got no time in " + std::to_string(gt.laneStarvedGaps) + " idle gaps");
        if (regulator_.SuppressedStepDowns() > 0)
            out.push_back("Analysis: " + std::to_string(regulator_.SuppressedStepDowns())
                          + " source changes ignored (already stepped down in this window)");
        const std::string notice = regulator_.Notice();
        if (!notice.empty())
            out.push_back("Analysis: " + notice);
        if (!st->plan.notice.empty())
            out.push_back("Analysis: " + st->plan.notice);
        const uint64_t planMovedN = planMovedRebuilds_.load(std::memory_order_relaxed);
        if (planMovedN > 0)
            out.push_back("Analysis: lane state rebuilt " + std::to_string(planMovedN)
                          + " time(s) on a kept graph because its VRAM plan moved (last at frame "
                          + std::to_string(planMovedLastSeq_.load(std::memory_order_relaxed))
                          + "; a reconfigure, not a resync)");
        const uint64_t usersN = usersReplans_.load(std::memory_order_relaxed);
        if (usersN > 0)
            out.push_back("Analysis: other VRAM users re-planned " + std::to_string(usersN)
                          + " time(s) on a kept lane state (the lane's own products unchanged)");
        return out;
    }

    // The automatic downgrades (no silent fallbacks): analysis_lane, analysis_lane_determinism,
    // source_stats_walk.
    std::vector<FallbackCatalog::Decision> Fallbacks() const
    {
        std::vector<FallbackCatalog::Decision> out;
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (settings_.mode != LaneSettings::Mode::Off && !st && !error_.empty()) {
            out.push_back(FallbackCatalog::Engaged("analysis_lane", "unavailable: " + error_));
        } else if (!st) {
            out.push_back(FallbackCatalog::Released("analysis_lane"));
        } else {
            std::vector<std::string> dropped;
            std::vector<std::string> forced;
            for (ProductId id : regulator_.Dropped())
                dropped.push_back(NameOf(*st, id));
            for (ProductId id : regulator_.ForcedLiveIds())
                forced.push_back(NameOf(*st, id));
            out.push_back(FallbackCatalog::AnalysisLane(dropped, forced, regulator_.PinnedAtMinimum(),
                                                         regulator_.T0InRender() || st->plan.t0OverBudget));
            if (!st->graph.leadChainWarning.empty())   // appended only when engaged (a WARNING, not a reject): the list keeps its shape otherwise
                out.push_back(FallbackCatalog::SkeletonLeadChain(st->graph.leadChainWarning));
            if (st->graph.sealLagForcedZero) {   // B3 X1: appended only when engaged (no product of the catalog declares a lag today)
                std::vector<std::string> zeroed;
                for (size_t i = 0; i < st->table->Count(); ++i) {
                    const Node& n = st->table->NodeAt(i);
                    if (n.declaredSealLag > 0 && n.sealLag == 0)
                        zeroed.push_back(NameOf(*st, n.id));
                }
                out.push_back(FallbackCatalog::AnalysisSealLagZero(zeroed));
            }
        }
        out.push_back(FallbackCatalog::AnalysisLaneDeterminism(st && det_ ? missedBounds_ : 0u));
        out.push_back(FallbackCatalog::SourceStatsWalk(settings_.walk == VpLaneProducts::WalkSource::Lane,
                                                        !P4Available()));
        if (externalSealMisses_ != 0)   // appended only when engaged: the list keeps its shape otherwise
            out.push_back(FallbackCatalog::ResyncSeal(externalSealMisses_, 0u));
        return out;
    }

private:
    bool IsOff() const { return mode_.load(std::memory_order_acquire) == uint8_t(LaneSettings::Mode::Off); }

    bool P4Available() const
    {
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        return settings_.mode != LaneSettings::Mode::Off && st && st->graph.Has(kP4SourceStats)
               && regulator_.StateOf(kP4SourceStats) != ProductState::Dropped;
    }

    static std::string NameOf(const LaneState& st, ProductId id)
    {
        const int i = st.IndexOf(id);
        if (i >= 0 && !st.info[size_t(i)].name.empty())
            return st.info[size_t(i)].name;
        return "product " + std::to_string(unsigned(id));
    }

    void Clear()
    {
        stateGeneration_.fetch_add(1, std::memory_order_acq_rel);
            std::atomic_store(&state_, std::shared_ptr<LaneState>());
        renderState_.reset();
        haveRenderSeq_ = false;   // the next render frame sets the ahead edge again (ReadOf, RunAheadNow)
        renderSeq_ = 0;
        owner_ = GraphOwner();
        regProducts_.clear();
        regulator_.Configure(regProducts_, 1.0, false);
        governor_ = VramGovernor();
        historyCaps_.clear();
        det_ = false;
        observedOnce_ = false;
        waitValid_ = false;
        missedBounds_ = 0;
        lastAhead_ = lastInRender_ = lastDeclined_ = 0;
        totalAhead_ = totalInRender_ = totalDeclined_ = 0;
        lateRing_.clear();
        lateRingSize_ = 0;
        lateRingPos_ = 0;
        lateRingSum_ = 0;
        lead_ = LeadState::Ok;
        {
            std::lock_guard<std::mutex> lock(carriedMutex_);
            carriedCpu_.clear();
        }
    }

    // Pure causal lane-owned CPU products whose law inputs are invariant across non-discontinuity reconfigures
    // (kRebuildQueue | kRebuildCard | kRebuildConsumers | kRebuildHistory). Admits strictly P1 and P4 today;
    // never ordered chains, held state, singleton stores, look-ahead readers, or non-cpuLive products.
    bool CanCarryCpuNode(const Graph& oldG, const Graph& newG, ProductId id, unsigned depth = 0) const
    {
        if (depth > kMaxInputs)
            return false;
        const Node* cn = oldG.Find(id);
        const Node* nn = newG.Find(id);
        if (cn == nullptr || nn == nullptr)
            return false;
        if (cn->backend != Backend::Cpu || nn->backend != Backend::Cpu)
            return false;
        if (cn->bufferBytes != nn->bufferBytes)
            return false;
        if (cn->orderedChain || nn->orderedChain || cn->heldState || nn->heldState
            || cn->singletonStore || nn->singletonStore || cn->lead != 0 || nn->lead != 0)
            return false;
        if (cpuLive_.find(id) == cpuLive_.end())
            return false;
        if (cn->reads != nn->reads || cn->inputCount != nn->inputCount)
            return false;
        for (size_t i = 0; i < cn->inputCount; ++i) {
            if (cn->inputs[i].product != nn->inputs[i].product
                || cn->inputs[i].lag != nn->inputs[i].lag
                || cn->inputs[i].lead != 0 || nn->inputs[i].lead != 0)
                return false;
            if (!CanCarryCpuNode(oldG, newG, cn->inputs[i].product, depth + 1u))
                return false;
        }
        return true;
    }

    // Wait-free carry of already-published pure causal CPU products across a non-discontinuity reconfigure
    // (queue / card / consumer / history / VRAM plan) so the replacement lane state reuses them instead of
    // recomputing the same frame a second time.
    void CarryCpuFrom(const LaneState& oldSt, const LaneState& newSt, uint64_t fromSeq)
    {
        if (!oldSt.table || !newSt.table)
            return;
        std::vector<uint8_t> tmp;
        for (size_t ci = 0; ci < oldSt.table->Count(); ++ci) {
            const Node& cn = oldSt.table->NodeAt(ci);
            if (!CanCarryCpuNode(oldSt.graph, newSt.graph, cn.id) || !oldSt.bufs[ci])
                continue;
            const int ni = newSt.IndexOf(cn.id);
            if (ni < 0 || !newSt.bufs[size_t(ni)] || newSt.bufs[size_t(ni)]->Bytes() != oldSt.bufs[ci]->Bytes())
                continue;
            // kGraph was just advanced by GraphOwner::Update for this non-discontinuity rebuild; check every other
            // honoured counter (kFull, kContinuity, kSettings*, era, ancient) so a discontinuity never leaks stale bytes.
            const VpIndicator::CounterMask carryMask = cn.reads & ~VpIndicator::Bit(VpIndicator::kGraph);
            const SlotSet& cset = oldSt.table->At(ci);
            for (uint16_t k = 0; k < cset.Ring(); ++k) {
                const VpAncestry::SlotView v = cset.ViewAt(k);
                if (!v.present || !(v.lanePublished || v.valid) || v.seq < fromSeq)
                    continue;
                const ReadInfo ri = cset.Read(v.seq, true);
                if (ri.header == nullptr
                    || (ind_ != nullptr && !VpIndicator::SameFor(ri.header->honoured, ind_->StampOf(v.seq), carryMask)))
                    continue;
                tmp.clear();
                const PublishedRead pr = ReadPublishedBytes(cset, *oldSt.bufs[ci], v.seq, [&tmp](const uint8_t* p, size_t n) {
                    tmp.assign(p, p + n);
                });
                if (pr == PublishedRead::Copied && tmp.size() == oldSt.bufs[ci]->Bytes()) {
                    std::lock_guard<std::mutex> lock(carriedMutex_);
                    carriedCpu_[std::make_pair(cn.id, v.seq)] = tmp;
                }
            }
        }
    }

    bool CopyCarriedCpu(ProductId id, uint64_t seq, uint8_t* dst, size_t n)
    {
        if (dst == nullptr || n == 0)
            return false;
        std::lock_guard<std::mutex> lock(carriedMutex_);
        const auto it = carriedCpu_.find(std::make_pair(id, seq));
        if (it == carriedCpu_.end() || it->second.size() != n)
            return false;
        std::copy_n(it->second.data(), n, dst);
        return true;
    }

    void PruneCarriedCpu(uint64_t sealedSeq)
    {
        std::lock_guard<std::mutex> lock(carriedMutex_);
        for (auto it = carriedCpu_.begin(); it != carriedCpu_.end();) {
            if (it->first.second <= sealedSeq)
                it = carriedCpu_.erase(it);
            else
                ++it;
        }
    }

    void NotifyNewest(LaneState& st)
    {
        uint64_t n = 0;
        if (st.ring->Newest(&n))
            st.runner->OnSourceFrame(n);
    }

    std::shared_ptr<LaneState> BuildState(const Registry& reg, const std::vector<ConsumerDecl>& consumers,
                                          const GraphSettings& gs, const CardProfile& card, const TierInput& tier,
                                          uint64_t fromSeq, uint64_t workerSkipBefore)
    {
        (void)consumers;
        std::shared_ptr<LaneState> st = std::make_shared<LaneState>();
        st->graph = owner_.Current();
        st->firstSeq = fromSeq;
        st->nextExpectedRetire = fromSeq;
        st->generation = stateGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1u;
        st->ring.reset(new LaneSourceRing(std::max<size_t>(st->graph.framesInFlight, 4u)));
        st->table.reset(new SlotTable(st->graph));
        assert(st->table->Valid());
        {
            const VpIndicator::Indicator* ind = ind_;   // the K1 seal of lead ancestors needs the stamp of frame N+k
            st->table->SetStampSource([ind](uint64_t q) { return ind->StampOf(q); });
        }
        ProductId maxId = 0;
        for (const Node& n : st->graph.nodes)
            maxId = std::max(maxId, n.id);
        st->indexOfId.assign(st->graph.nodes.empty() ? size_t(0) : size_t(maxId) + 1u, -1);
        for (size_t i = 0; i < st->graph.nodes.size(); ++i) {
            const Node& n = st->graph.nodes[i];
            st->indexOfId[n.id] = int(i);
            st->bufs.emplace_back(n.backend == Backend::Cpu ? new CpuBuffers(n) : nullptr);
            LaneNodeInfo info;
            const ProductDecl& d = reg.Get(n.id);
            info.sourceFrame = d.sourceFrame;
            info.identical = d.variants == Variants::Identical;
            info.name = d.name != nullptr ? d.name : "";
            st->info.push_back(info);
        }
        st->lateSeen.assign(st->graph.nodes.size(), 0u);
        st->missSeen.assign(st->graph.nodes.size(), 0u);

        st->plan = governor_.Update(reg, st->graph, card, tier);
        // The graph was built with the trial probe's history caps (Configure); the real plan must grant the same: a cheap
        // equality check (the caps are short, sorted by id) so the rings and the memory plan cannot silently disagree.
        if (st->plan.historyCaps != gs.historyCaps)
            st->capsMismatch = "the history grant changed between the trial plan and the real plan (graph sized for another grant)";
        ApplyLaneGrants(st->plan, *st->table);

        st->indHold = indHandle_;
        st->runner.reset(new LaneRunner(*st->table, ind_, nowUs_));
        LaneState* sp = st.get();
        st->runner->SetBuffers(
            [sp](ProductId p, int slot) -> uint8_t* {
                const int i = sp->IndexOf(p);
                return i >= 0 && sp->bufs[size_t(i)] ? sp->bufs[size_t(i)]->Ring(size_t(slot)) : nullptr;
            },
            [sp](ProductId p, uint64_t seq, int live) -> const uint8_t* {
                const int i = sp->IndexOf(p);
                if (i < 0 || !sp->bufs[size_t(i)])
                    return nullptr;
                return live >= 0 ? sp->bufs[size_t(i)]->Live(size_t(live))
                                 : sp->bufs[size_t(i)]->Ring(size_t(sp->table->At(size_t(i)).SlotOf(seq)));
            },
            [sp](ProductId p) -> size_t {
                const int i = sp->IndexOf(p);
                return i >= 0 ? size_t(sp->graph.nodes[size_t(i)].bufferBytes) : size_t(0);
            });
        st->runner->SetSources([sp](uint64_t seq, SourceFrameView* out) { return sp->ring->Find(seq, out); });
        st->runner->SetReleaseCompleted([this](ProductId id) -> uint64_t {
            return releaseCompleted_ ? releaseCompleted_(id) : ~uint64_t(0);
        });
        st->runner->SetWorkerReusedSink([this](ProductId id) {
            if (workerReusedSink_)
                workerReusedSink_(id);
        });
        st->runner->SetAdopt([this](ProductId id, uint64_t seq, uint8_t* dst, size_t n) {
            if (CopyCarriedCpu(id, seq, dst, n))
                return true;
            return workerAdopt_ ? workerAdopt_(id, seq, dst, n) : false;
        });
        for (ILaneProducer* p : producers_)
            st->runner->Register(p);
        for (size_t i = 0; i < st->graph.nodes.size(); ++i) {
            const Node& n = st->graph.nodes[i];
            st->runner->SetReadsSource(n.id, st->info[i].sourceFrame);
            // A product without a producer has no lane: in-render-only, never counted missing.
            const bool laned = st->plan.Laned(n.id) && (n.backend == Backend::RenderOnly || st->runner->HasProducer(n.id));
            st->runner->SetLaned(n.id, laned);
            st->table->At(i).SetLaneGranted(laned);
        }
        st->runner->SetAsyncCpuWorkers(workersRunning_.load(std::memory_order_acquire) != 0);
        st->runner->SetSkipBefore(workerSkipBefore != 0 ? workerSkipBefore : fromSeq);

        // Worker order: tier, then topological, per backend.
        const Tier tiers[3] = { Tier::T0, Tier::T1, Tier::T2 };
        for (Tier t : tiers)
            for (const Node& n : st->graph.nodes)
                if (n.tier == t && n.backend != Backend::RenderOnly)
                    st->workerOrder[size_t(n.backend)].push_back(n.id);

        // The render-thread gap works: lane GPU-compute products.
        st->gap.SetChunkCapUs(ChunkCapUs());
        for (size_t i = 0; i < st->graph.nodes.size(); ++i) {
            const Node& n = st->graph.nodes[i];
            if (n.backend != Backend::GpuCompute)
                continue;
            const double heavy = RenderMsOf(reg.Get(n.id), st->graph, card, tier.measured);
            st->gapWorks.emplace_back(new LaneComputeGapWork(st->runner.get(), n.id, n.tier, heavy, nowUs_));
            st->gap.Add(st->gapWorks.back().get());
        }
        return st;
    }

    // Regulator products of a graph: lane products with their identical / reduced-mode / measured cost.
    void ConfigureRegulatorFor(const LaneState& st, const Registry& reg, const std::vector<ConsumerDecl>& consumers,
                               const GraphSettings& gs, const CardProfile& card, const TierInput& tier)
    {
        const size_t n = st.graph.nodes.size();
        std::vector<std::vector<bool>> directReduced(n);   // per node: each direct consumer's hasReducedMode
        std::vector<std::vector<size_t>> dependents(n);
        for (const ConsumerDecl& c : consumers)
            for (size_t k = 0; k < c.count && k < kMaxConsumes; ++k) {
                const int i = st.IndexOf(Resolve(reg, c.consumes[k].product, gs.ofaPresent));
                if (i >= 0)
                    directReduced[size_t(i)].push_back(c.hasReducedMode);
            }
        for (size_t m = 0; m < n; ++m)
            for (size_t k = 0; k < st.graph.nodes[m].inputCount; ++k) {
                const int i = st.IndexOf(st.graph.nodes[m].inputs[k].product);
                if (i >= 0)
                    dependents[size_t(i)].push_back(m);
            }
        // reducedMode: every direct consumer and every dependent product may run reduced (greatest fixpoint).
        std::vector<char> reduced(n, 1);
        for (size_t pass = 0; pass <= n; ++pass) {
            bool changed = false;
            for (size_t i = 0; i < n; ++i) {
                bool r = !directReduced[i].empty() || !dependents[i].empty();
                for (bool b : directReduced[i])
                    r = r && b;
                for (size_t d : dependents[i])
                    r = r && reduced[d] != 0;
                if ((r ? 1 : 0) != reduced[i]) {
                    reduced[i] = r ? 1 : 0;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
        regProducts_.clear();
        for (size_t i = 0; i < n; ++i) {
            const Node& nd = st.graph.nodes[i];
            if (nd.backend == Backend::RenderOnly || !st.runner->HasProducer(nd.id))
                continue;   // no lane to shed: an in-render-only product is never the regulator's to drop
            RegProduct p;
            p.id = nd.id;
            p.tier = nd.tier;
            p.identical = st.info[i].identical;
            p.reducedMode = reduced[i] != 0;
            p.inRenderMs = RenderMsOf(reg.Get(nd.id), st.graph, card, tier.measured);
            regProducts_.push_back(p);
        }
        ConfigureRegulator();
    }

    void ConfigureRegulator() { regulator_.Configure(regProducts_, periodMs_, det_); }
    // GF-P2b MUST-4 (2): do two plans of the SAME graph make the same decisions for the LANE's own products? Every product's grant /
    // tier / history depth, the product users' grants (ids from kUserProductBase), the history caps and the over-budget flags.
    // Plain-language text (notice, lines) and the measured figures (budget, totals, ms) are not decisions: a new card reading alone
    // does not rebuild the lane. The other VRAM users' grants are compared apart (SameUserGrants).
    static bool SameGrant(const UserGrant& x, const UserGrant& y)
    {
        return x.id == y.id && x.level == y.level && x.bytes == y.bytes && x.shed == y.shed;
    }
    static bool SameLaneDecisions(const TierPlan& a, const TierPlan& b)
    {
        if (a.entries.size() != b.entries.size() || a.historyCaps != b.historyCaps || a.t0OverBudget != b.t0OverBudget
            || a.liveOverBudget != b.liveOverBudget)
            return false;
        for (size_t i = 0; i < a.entries.size(); ++i) {
            const TierEntry& x = a.entries[i];
            const TierEntry& y = b.entries[i];
            if (x.id != y.id || x.grant != y.grant || x.tier != y.tier || x.historyGranted != y.historyGranted)
                return false;
        }
        std::vector<const UserGrant*> pa, pb;
        for (const UserGrant& g : a.governor.grants)
            if (g.id >= kUserProductBase)
                pa.push_back(&g);
        for (const UserGrant& g : b.governor.grants)
            if (g.id >= kUserProductBase)
                pb.push_back(&g);
        if (pa.size() != pb.size())
            return false;
        for (size_t i = 0; i < pa.size(); ++i)
            if (!SameGrant(*pa[i], *pb[i]))
                return false;
        return true;
    }
    // Every user's grant, in order (the users plan the skeleton reads: the memo's kUserSkeletonMemo grant among them).
    static bool SameUserGrants(const GovernorPlan& a, const GovernorPlan& b)
    {
        if (a.grants.size() != b.grants.size())
            return false;
        for (size_t i = 0; i < a.grants.size(); ++i)
            if (!SameGrant(a.grants[i], b.grants[i]))
                return false;
        return true;
    }

    // The regulator's states onto the runner and the slots: a product moved into the render or dropped
    // has no lane (its misses are expected, not counted); forced live stays laned.
    void ApplyRegulator(LaneState& st)
    {
        for (size_t i = 0; i < st.graph.nodes.size(); ++i) {
            const Node& n = st.graph.nodes[i];
            if (n.backend == Backend::RenderOnly)
                continue;
            const ProductState ps = regulator_.StateOf(n.id);
            const bool laned = st.plan.Laned(n.id) && st.runner->HasProducer(n.id)
                               && (ps == ProductState::Laned || ps == ProductState::ForcedLive);
            st.runner->SetLaned(n.id, laned);
            st.table->At(i).SetLaneGranted(laned);
        }
    }

    // The ONE CPU live law (seal live fill and Acquire's owner fill): the registered compute of node `n` for frame `seq` with its
    // inputs read from the slots (a lead reads the published result); an input bit set in `missing` stays null, an input the
    // slots do not hold is asked of `inputOf` when given (Acquire chain). Render thread.
    bool RunCpuLive(const LaneState& st, const Node& n, uint64_t seq, uint32_t missing, uint8_t* dst, size_t bytes,
                    const InputResolver& inputOf, bool holdSource = false) const
    {
        const auto it = cpuLive_.find(n.id);
        if (it == cpuLive_.end())
            return false;
        std::array<const uint8_t*, kMaxInputs> in{};
        for (size_t k = 0; k < n.inputCount && k < in.size(); ++k) {
            if (((missing >> k) & 1u) != 0)
                continue;
            const int j = st.IndexOf(n.inputs[k].product);
            uint64_t aseq = 0;
            if (j < 0 || !st.bufs[size_t(j)] || !InputSeq(n.inputs[k], seq, &aseq))
                continue;
            const ReadInfo ri = st.table->At(size_t(j)).Read(aseq, n.inputs[k].lead > 0 || holdSource);   // a lead reads the published result; so does an AHEAD run (holdSource): its lag-0 input is Published, not yet Sealed (GEM SK-LANE-AHEAD)
            if (ri.kind == ReadInfo::Ring)
                in[k] = st.bufs[size_t(j)]->Ring(size_t(ri.index));
            else if (ri.kind == ReadInfo::Live)
                in[k] = st.bufs[size_t(j)]->Live(size_t(ri.index));
            else if (inputOf)
                in[k] = inputOf(n.inputs[k].product, aseq, st.bufs[size_t(j)]->Bytes());
        }
        SourceFrameView view;
        // holdSource (RunAheadNow, a frame AHEAD of the render frame the render does not hold): the copy under the ring's lock carries the
        // keepAlive, so a drop of the frame mid-compute cannot free the planes (QKP audit MUST-1). Else the lock-free copy of the frame
        // the render holds (no keepAlive).
        const bool haveView = holdSource ? st.ring->Find(seq, &view) : st.ring->TryGet(seq, &view);
        return it->second(n, haveView ? &view : nullptr, in, dst, bytes);
    }

    // The live (in-render) form of a product.
    bool LiveFillOf(LaneState& st, const Node& n, uint64_t seq, int live, Header& hdr, uint32_t missing)
    {
        const int idx = st.IndexOf(n.id);
        if (idx < 0)
            return false;
        for (ProductId e : externalIds_)
            if (e == n.id) {
                ++externalDeclined_;   // published outside the lane or not at all: never a GPU / CPU fill of the lane's
                return false;
            }
        const uint64_t t0 = nowUs_ ? nowUs_() : 0u;
        bool ok = false;
        if (n.backend == Backend::Cpu) {
            if (cpuLive_.find(n.id) == cpuLive_.end() || !st.bufs[size_t(idx)])
                return false;
            uint8_t* dst = st.bufs[size_t(idx)]->Live(size_t(live));
            const size_t bytes = st.bufs[size_t(idx)]->Bytes();
            // B1: an Acquire reader published it already (the same law, all inputs): reused, not recomputed. Only when no input is
            // missing at the seal: a missing-input form is a different result and is computed by the law itself.
            if (missing == 0 && (CopyCarriedCpu(n.id, seq, dst, bytes) || (publishedLookup_ && publishedLookup_(n.id, seq, dst, bytes)))) {
                ++sealReusedPublished_;
                ok = true;
            } else {
                ok = RunCpuLive(st, n, seq, missing, dst, bytes, InputResolver());
                if (ok)
                    VPV_COMPUTE_ONCE_AT(("lane:" + std::to_string(int(n.id))).c_str(), seq, ::ComputeOnce::Variant(0));   // compute-once: the seal live compute (recorded only when computed)
                if (ok && missing != 0) {
                    ++sealComputedReduced_;
                    if (sealReducedSink_)
                        sealReducedSink_(n.id);
                }
            }
        } else if (liveFill_) {
            ok = liveFill_(n, seq, live, hdr, missing);
        } else if (dispatcher_ != nullptr && n.backend == Backend::GpuCompute) {
            if (gpuLiveDeferred_) {
                // 70b G0: reserve only; RunDeferredLive records it at Entrance (no GPU work before the render lock).
                LivePending e;
                e.id = n.id;
                e.seq = seq;
                e.live = live;
                e.hdr = hdr;
                e.missing = missing;
                livePending_.push_back(e);
                ok = true;
            } else {
                ok = dispatcher_->DispatchLive(n, seq, live, hdr, missing);
            }
        }
        if (ok && nowUs_) {
            const uint64_t t1 = nowUs_();
            const uint64_t dt = t1 > t0 ? t1 - t0 : 0u;
            if (hdr.costUs == 0)
                hdr.costUs = dt > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(dt);
        }
        return ok;
    }

    // 70b G0: failed deferred live fills (product, frame), kept for the frames still readable (bounded).
    void MarkFailed(ProductId id, uint64_t seq)
    {
        if (IsFailed(id, seq))
            return;
        if (failed_.size() >= kFailedKept)
            failed_.erase(failed_.begin());
        failed_.push_back(std::make_pair(id, seq));
    }
    bool IsFailed(ProductId id, uint64_t seq) const
    {
        for (const std::pair<ProductId, uint64_t>& f : failed_)
            if (f.first == id && f.second == seq)
                return true;
        return false;
    }

    // While the storm hold is in force keep the regulator at the conservative floor (T0), once per frame.
    // The Indicator was replaced (new era) since the graph was built: rebuild it on the new one from
    // the inputs of the last Configure (the state is swapped like any Configure rebuild; the ring's
    // frames are carried over; the previous state keeps the previous Indicator alive until its last
    // worker lets go).
    void RebuildOnRebind(uint64_t seq)
    {
        rebuildPending_ = false;
        if (!cfg_)
            return;
        const SavedConfig c = *cfg_;
        const uint64_t skipBefore = workersRunning_.load(std::memory_order_acquire) != 0 ? (seq + 1u) : seq;
        Configure(settings_, c.reg, c.consumers, c.gs, c.card, c.tier, seq, c.hostDet, skipBefore);
        ++rebindRebuilds_;
        // The rebuild re-derived the regulator for the new graph: a storm hold in force must floor it at
        // T0 again in THIS frame (OnSealed applied it to the previous graph's regulator before the rebuild).
        if (stormHold_) {
            stormEdgePending_ = true;
            ApplyStorm();
        }
    }

    struct SavedConfig {
        Registry reg;
        std::vector<ConsumerDecl> consumers;
        GraphSettings gs;
        CardProfile card;
        TierInput tier;
        bool hostDet = false;
    };

    void ApplyStorm()
    {
        if (!stormHold_)
            return;
        if (stormEdgePending_ || regulator_.LaneLevel() > regulator_.T0StepCount()) {
            stormEdgePending_ = false;
            regulator_.OnIndicatorChange(VpIndicator::Bit(VpIndicator::kFull));
        }
    }

    VpIndicator::Indicator* ind_;
    uint32_t indEra_ = 0;                                 // render thread only; ind_'s era (0 = none)
    std::shared_ptr<VpIndicator::Indicator> indHandle_;   // render thread only
    bool externalSeal_ = false;
    bool haveSealedSeq_ = false;
    uint64_t sealedSeq_ = 0;
    bool stormHold_ = false;
    bool stormEdgePending_ = false;
    std::function<bool(uint64_t)> readyProbe_;
    std::function<void()> readyMissed_;
    uint64_t readyProbeMisses_ = 0;
    std::unique_ptr<SavedConfig> cfg_;
    std::vector<HistoryCap> historyCaps_;   // the governor's last grant below full history (see Configure)
    bool rebuildPending_ = false;
    uint64_t rebindRebuilds_ = 0;
    uint64_t stormHoldEngagements_ = 0;
    uint64_t indicatorRebinds_ = 0;
    uint64_t externalSealMisses_ = 0;
    std::function<uint64_t()> nowUs_;
    LaneSettings settings_;
    std::shared_ptr<LaneState> state_;         // swapped with std::atomic_store / atomic_load
    std::atomic<uint64_t> planMovedRebuilds_{ 0 };   // GF-P2b MUST-4 (2): kept-graph Configures whose VRAM plan moved (written on the render thread)
    std::atomic<uint64_t> planMovedLastSeq_{ 0 };    // the frame of the last one
    // GF-P2b MUST-4 (2): the other VRAM users' re-plan of a KEPT state (its generation: a newer state's own plan wins).
    struct UsersPlan {
        uint64_t generation = 0;
        GovernorPlan plan;
    };
    std::shared_ptr<const UsersPlan> usersPlan_;     // std::atomic_load / atomic_store (status lines read it from any thread)
    std::atomic<uint64_t> usersReplans_{ 0 };
    std::atomic<uint64_t> stateGeneration_{ 0 };   // last LaneState::generation handed out; also bumped when the state is dropped (Lease() reads it)
    std::shared_ptr<LaneState> renderState_;   // the render thread's hold (keeps returned references valid)
    uint64_t renderSeq_ = 0;                    // the frame of the last OnRenderStart (render thread; ReadOf's ahead edge)
    bool haveRenderSeq_ = false;
    GraphOwner owner_;
    VramGovernor governor_;
    Regulator regulator_;
    std::vector<RegProduct> regProducts_;
    std::vector<ILaneProducer*> producers_;
    IGpuDispatcher* dispatcher_ = nullptr;
    LiveFill liveFill_;
    // 70b G0 (render thread)
    static constexpr size_t kFailedKept = 64;
    bool gpuLiveDeferred_ = false;
    std::vector<LivePending> livePending_;
    const LaneState* pendingState_ = nullptr;   // the graph the pending entries and failures belong to
    std::vector<std::pair<ProductId, uint64_t>> failed_;
    DeferredReport deferredTotals_;
    std::map<ProductId, CpuLiveCompute> cpuLive_;
    PublishedLookup publishedLookup_;      // B1: the seal copies an Acquire-published default-variant CPU form (render thread)
    uint64_t aheadMadeByRender_ = 0;
    uint64_t aheadClaimedByWorker_ = 0;
    uint64_t aheadAlreadyMade_ = 0;
    uint64_t aheadBeyondSlots_ = 0;
    uint64_t aheadGpuNotLegal_ = 0;
    uint64_t aheadSealReused_ = 0;
    uint64_t sealComputedReduced_ = 0;
    uint64_t sealReusedPublished_ = 0;     // seal live fills served from that publication, plus seals accepting a RunAheadNow frame (never computed twice)
    std::vector<ProductId> externalIds_;   // products published outside the lane (SetExternalProducts)
    uint64_t externalDeclined_ = 0;        // seal-time live fills of those products that were declined (nothing published in time)
    std::function<uint64_t(ProductId)> releaseCompleted_;
    std::function<void(ProductId)> sealReducedSink_;
    std::function<void(ProductId)> workerReusedSink_;
    WorkerAdopt workerAdopt_;   // C5-SKREDS MUST-1 (SetWorkerAdopt)
    std::vector<ProductFrameResult> empty_;
    std::string error_;
    double periodMs_ = 0.0;
    std::atomic<uint32_t> chunkCapUs_{ 1000 };
    std::atomic<uint8_t> mode_{ 0 };   // LaneSettings::Mode, readable from the lane threads
    std::atomic<int> workersRunning_{ 0 };   // LaneWorkerThreads running (setup-order assertion)
    bool det_ = false;
    bool observedOnce_ = false;
    uint32_t lastSlackUs_ = 0;
    LeadState lead_ = LeadState::Ok;

    // N1 wait bookkeeping (render thread)
    bool waitValid_ = false;
    uint64_t waitSeq_ = 0;
    uint32_t waitedUs_ = 0;
    uint32_t lastAskUs_ = 0;
    bool missCounted_ = false;
    uint64_t missedBounds_ = 0;

    // statistics (render thread)
    uint32_t lastAhead_ = 0, lastInRender_ = 0, lastDeclined_ = 0;
    uint64_t totalAhead_ = 0, totalInRender_ = 0, totalDeclined_ = 0;
    std::vector<uint32_t> lateRing_;
    size_t lateRingSize_ = 0;
    size_t lateRingPos_ = 0;
    uint32_t lateRingSum_ = 0;

    std::atomic<uint64_t> wake_{ 0 };
    std::mutex wakeMutex_;
    std::condition_variable wakeCv_;
    std::mutex carriedMutex_;
    std::map<std::pair<ProductId, uint64_t>, std::vector<uint8_t>> carriedCpu_;
};

// ---------------------------------------------------------------------------------------------
// Optional real worker threads: one per lane backend (OFA, CPU), woken by OnSourceFrame / retire,
// never started in Off mode. Stop() joins.
// ---------------------------------------------------------------------------------------------

class LaneWorkerThreads {
public:
    explicit LaneWorkerThreads(AnalysisLane& lane) : lane_(lane) {}
    ~LaneWorkerThreads() { Stop(); }
    LaneWorkerThreads(const LaneWorkerThreads&) = delete;
    LaneWorkerThreads& operator=(const LaneWorkerThreads&) = delete;

    // False (no thread started) when the lane is off / unavailable or the threads already run.
    bool Start()
    {
        if (running_ || !lane_.Active())
            return false;
        stop_.store(false, std::memory_order_release);
        lane_.SetWorkersRunning(true);
        running_ = true;
        threads_.emplace_back([this]() { Loop(Backend::Ofa); });
        threads_.emplace_back([this]() { Loop(Backend::Cpu); });
        return true;
    }

    void Stop()
    {
        if (!running_)
            return;
        stop_.store(true, std::memory_order_release);
        lane_.NotifyWake();
        for (std::thread& t : threads_)
            if (t.joinable())
                t.join();
        threads_.clear();
        running_ = false;
        lane_.SetWorkersRunning(false);
    }

    bool Running() const { return running_; }

private:
    void Loop(Backend b)
    {
        while (!stop_.load(std::memory_order_acquire)) {
            const uint64_t seen = lane_.WakeCount();
            if (!lane_.StepWorker(b, 0))
                lane_.WaitWake(seen, 500u);
        }
    }

    AnalysisLane& lane_;
    std::atomic<bool> stop_{ false };
    bool running_ = false;
    std::vector<std::thread> threads_;
};

} // namespace VpSkeleton
