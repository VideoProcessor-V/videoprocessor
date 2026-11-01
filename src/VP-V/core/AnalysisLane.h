// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "GapScheduler.h"
#include "Indicator.h"
#include "LaneInterfaces.h"
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

inline bool LaneStepProgressed(StepOutcome o)
{
    return o == StepOutcome::Ran || o == StepOutcome::Published || o == StepOutcome::Late
           || o == StepOutcome::Abandoned;
}

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
    bool HasWork() const override { return !idle_ && runner_ != nullptr && runner_->Laned(id_); }
    // The larger of the last measured CPU time of a Step and the producer's own device-cost prediction
    // (a GPU-compute Step only records: its CPU time says nothing about the chunk's GPU time). The
    // measured part decays through OnCapNoFit, so one jittery over-cap Step cannot starve the product.
    uint32_t PredictUs() const override
    {
        const uint32_t p = runner_ != nullptr ? runner_->PredictChunkUs(id_) : 0u;
        return std::max(lastUs_, p);
    }
    void OnCapNoFit() override { lastUs_ /= 2u; }   // halves per gap: an over-cap outlier is forgotten in a few gaps
    void RunChunk(uint32_t budgetUs) override
    {
        const uint64_t t0 = nowUs_ ? nowUs_() : 0u;
        const StepOutcome o = runner_->Step(id_, budgetUs);
        const uint64_t t1 = nowUs_ ? nowUs_() : 0u;
        ++chunks_;
        if (LaneStepProgressed(o)) {
            const uint64_t dt = t1 > t0 ? t1 - t0 : 0u;
            lastUs_ = dt > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(dt);
        } else {
            idle_ = true;   // nothing to do until the next gap (Wake)
        }
    }

    void Wake() { idle_ = false; }
    uint64_t Chunks() const { return chunks_; }

private:
    LaneRunner* runner_;
    ProductId id_;
    Tier tier_;
    double heavyMs_;
    std::function<uint64_t()> nowUs_;
    bool idle_ = false;
    uint32_t lastUs_ = 0;
    uint64_t chunks_ = 0;
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

    int IndexOf(ProductId id) const { return id < indexOfId.size() ? indexOfId[id] : -1; }
};

// ---------------------------------------------------------------------------------------------
// The facade.
// ---------------------------------------------------------------------------------------------

class AnalysisLane {
public:
    using CpuLiveCompute = std::function<bool(const Node&, const SourceFrameView*,
                                              const std::array<const uint8_t*, kMaxInputs>&, uint8_t*, size_t)>;
    using LiveFill = std::function<bool(const Node&, uint64_t, int, Header&, uint32_t)>;

    // `indicator` is the host's central Indicator (not owned; may be null: the lane is then unavailable).
    AnalysisLane(VpIndicator::Indicator* indicator, std::function<uint64_t()> nowUs)
        : ind_(indicator), nowUs_(std::move(nowUs))
    {
    }
    AnalysisLane(const AnalysisLane&) = delete;
    AnalysisLane& operator=(const AnalysisLane&) = delete;

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
    // LaneWorkerThreads tells the lane whether its threads run (setup-order assertion only).
    void SetWorkersRunning(bool running) { workersRunning_.store(running ? 1 : 0, std::memory_order_release); }
    void SetGpuDispatcher(IGpuDispatcher* d) { dispatcher_ = d; }
    void SetLiveFill(LiveFill fill) { liveFill_ = std::move(fill); }
    void RegisterCpuLiveCompute(ProductId id, CpuLiveCompute fn) { cpuLive_[id] = std::move(fn); }
    // The render-device release fence's completed value for a product (lane polls it before reuse).
    void SetReleaseCompleted(std::function<uint64_t(ProductId)> f) { releaseCompleted_ = std::move(f); }

    // ---- render thread ----

    // OnSettings: advance the graph (GraphOwner::Update), on a rebuild build the new LaneState (tier plan
    // through the VRAM governor, lane grants, runner, regulator, gap chunk cap) and swap it in. Returns
    // the rebuild reasons (0 = graph kept). Mode Off clears everything and returns 0.
    uint32_t Configure(const LaneSettings& s, const Registry& reg, const std::vector<ConsumerDecl>& consumers,
                       const GraphSettings& gs, const CardProfile& card, const TierInput& tier, uint64_t fromSeq,
                       bool hostDeterministic)
    {
        settings_ = s;
        mode_.store(uint8_t(s.mode), std::memory_order_release);
        if (s.mode == LaneSettings::Mode::Off || ind_ == nullptr) {
            Clear();
            error_ = s.mode == LaneSettings::Mode::Off ? "" : "no indicator";
            return 0;
        }
        const bool det = s.deterministic == LaneSettings::Det::Auto ? hostDeterministic
                                                                     : s.deterministic == LaneSettings::Det::On;
        const bool detChanged = det != det_;
        det_ = det;

        const VpIndicator::Stamp stamp = ind_->StampOf(fromSeq);
        const uint32_t reasons = owner_.Update(reg, consumers, gs, stamp, ind_, fromSeq);
        const std::shared_ptr<LaneState> cur = std::atomic_load(&state_);
        const Graph& g = owner_.Current();
        if (!g.valid) {
            error_ = g.error;
            std::atomic_store(&state_, std::shared_ptr<LaneState>());
            renderState_.reset();
            return reasons;
        }
        error_.clear();
        periodMs_ = tier.framePeriodMs;
        chunkCapUs_.store(GapScheduler::DerivedChunkCapUs(tier.framePeriodMs, s.chunkUs), std::memory_order_relaxed);

        if (reasons == 0 && cur) {
            cur->gap.SetChunkCapUs(ChunkCapUs());
            if (detChanged) {
                ConfigureRegulator();
                ApplyRegulator(*cur);   // the regulator's states changed under the kept graph
            }
            renderState_ = cur;
            return 0;
        }

        std::shared_ptr<LaneState> st = BuildState(reg, consumers, gs, card, tier, fromSeq);
        // Regulator products from the new graph.
        ConfigureRegulatorFor(*st, reg, consumers, gs, card, tier);
        lateRingSize_ = periodMs_ > 0.0 ? size_t(std::max(1.0, 10000.0 / periodMs_ + 0.5)) : size_t(0);
        lateRing_.assign(lateRingSize_, 0u);
        lateRingPos_ = 0;
        lateRingSum_ = 0;

        if (cur)
            st->ring->CopyFrom(*cur->ring);
        NotifyNewest(*st);
        std::atomic_store(&state_, st);
        if (cur) {   // frames stored in the old ring while the swap happened
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
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st)
            return empty_;
        renderState_ = st;
        lastSlackUs_ = presentSlackUs;

        const VpIndicator::CounterMask changed = ind_->SealAndObserve(seq);
        if (observedOnce_ && changed != 0)
            regulator_.OnIndicatorChange(changed);   // the first observation is not a change
        observedOnce_ = true;
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
        const std::vector<ProductFrameResult>& res = st->table->Resolve(
            seq, stamp, [this, sp](const Node& n, uint64_t s, int live, Header& hdr, uint32_t missing) {
                if (regulator_.StateOf(n.id) == ProductState::Dropped)
                    return false;   // dropped by the regulator: declined, visible
                return LiveFillOf(*sp, n, s, live, hdr, missing);
            });
        lastAhead_ = lastInRender_ = lastDeclined_ = 0;
        for (const ProductFrameResult& r : res) {
            if (r.seal.outcome == SealOutcome::Sealed && r.seal.reason == SealReason::Accepted)
                ++lastAhead_;
            if (r.liveFilled)
                ++lastInRender_;
            if (r.final == SealOutcome::Unavailable && r.finalReason == SealReason::FillDeclined)
                ++lastDeclined_;
        }
        totalAhead_ += lastAhead_;
        totalInRender_ += lastInRender_;
        totalDeclined_ += lastDeclined_;
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
        for (size_t i = 0; i < st->table->Count(); ++i) {
            const Node& n = st->table->NodeAt(i);
            if (n.backend == Backend::RenderOnly || !st->runner->Laned(n.id) || !st->runner->HasProducer(n.id))
                continue;   // no producer: nothing to wait for (the seal fills it live), no bound burnt
            uint32_t m = 0;
            const Variants v = st->info[i].identical ? Variants::Identical : Variants::NonIdentical;
            const SlotSet& set = st->table->At(i);
            const WaitDecision d = DecideWait(v, set.LaneGranted(), set.StateOf(seq), p, waitedUs_, &m);
            if (d == WaitDecision::Wait)
                more = std::max(more, m);
            else if (d == WaitDecision::BoundMissed)
                missed = true;
        }
        if (more > 0) {
            lastAskUs_ = more;
            return more;
        }
        lastAskUs_ = 0;
        if (missed && !missCounted_) {
            ++missedBounds_;
            missCounted_ = true;
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

    // The frame's last use is over (presented / render-ahead slot retired).
    void OnFrameRetired(uint64_t seq, uint64_t releaseFenceValue)
    {
        if (settings_.mode == LaneSettings::Mode::Off)
            return;
        const std::shared_ptr<LaneState> st = std::atomic_load(&state_);
        if (!st)
            return;
        for (size_t i = 0; i < st->table->Count(); ++i) {
            const uint64_t h = st->table->NodeAt(i).history;
            // A frame that left its history before this graph's first frame was never in this table.
            if (seq >= h && seq - h >= st->firstSeq)
                st->table->At(i).Release(seq - h, releaseFenceValue);
        }
        st->ring->Drop(seq);   // the ring's copy (and its host plane hold) ends with the frame; a running job keeps its own
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
            st->runner->SetSkipBefore(seq + 1u);   // a late lane skips to the next frame now
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
            progress = LaneStepProgressed(st->runner->Step(id, budget)) || progress;
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
        out.push_back(std::string("Analysis lead: ") + LeadText(lead_));
        out.push_back("Analysis level: " + std::to_string(unsigned(regulator_.Level())) + "/"
                      + std::to_string(unsigned(regulator_.MaxLevel())));
        out.push_back("Analysis late: " + std::to_string(lateRingSum_) + " per 10 s");
        if (det_)
            out.push_back("Analysis deterministic: " + std::to_string(missedBounds_) + " frames missed the wait bound");
        uint64_t skipped = 0;
        for (size_t i = 0; i < st->table->Count(); ++i)
            skipped += st->runner->Counters(st->table->NodeAt(i).id).skippedDropped.load(std::memory_order_relaxed);
        if (skipped > 0)
            out.push_back("Analysis: " + std::to_string(skipped) + " frames skipped (source gone)");
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
                                                         regulator_.T0InRender() || st->plan.t0OverBudget,
                                                         unsigned(regulator_.OfaStep())));
        }
        out.push_back(FallbackCatalog::AnalysisLaneDeterminism(st && det_ ? missedBounds_ : 0u));
        out.push_back(FallbackCatalog::SourceStatsWalk(settings_.walk == VpLaneProducts::WalkSource::Lane,
                                                        !P4Available()));
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
        std::atomic_store(&state_, std::shared_ptr<LaneState>());
        renderState_.reset();
        owner_ = GraphOwner();
        regProducts_.clear();
        regulator_.Configure(regProducts_, 1.0, false);
        governor_ = VramGovernor();
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
    }

    void NotifyNewest(LaneState& st)
    {
        uint64_t n = 0;
        if (st.ring->Newest(&n))
            st.runner->OnSourceFrame(n);
    }

    std::shared_ptr<LaneState> BuildState(const Registry& reg, const std::vector<ConsumerDecl>& consumers,
                                          const GraphSettings& gs, const CardProfile& card, const TierInput& tier,
                                          uint64_t fromSeq)
    {
        (void)consumers;
        (void)gs;
        std::shared_ptr<LaneState> st = std::make_shared<LaneState>();
        st->graph = owner_.Current();
        st->firstSeq = fromSeq;
        st->ring.reset(new LaneSourceRing(std::max<size_t>(st->graph.framesInFlight, 4u)));
        st->table.reset(new SlotTable(st->graph));
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
        ApplyLaneGrants(st->plan, *st->table);

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
        st->runner->SetSkipBefore(fromSeq);

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
            p.ofa = nd.backend == Backend::Ofa;
            p.inRenderMs = RenderMsOf(reg.Get(nd.id), st.graph, card, tier.measured);
            regProducts_.push_back(p);
        }
        ConfigureRegulator();
    }

    void ConfigureRegulator() { regulator_.Configure(regProducts_, periodMs_, det_); }

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

    // The live (in-render) form of a product.
    bool LiveFillOf(LaneState& st, const Node& n, uint64_t seq, int live, Header& hdr, uint32_t missing)
    {
        const int idx = st.IndexOf(n.id);
        if (idx < 0)
            return false;
        const uint64_t t0 = nowUs_ ? nowUs_() : 0u;
        bool ok = false;
        if (n.backend == Backend::Cpu) {
            const auto it = cpuLive_.find(n.id);
            if (it == cpuLive_.end() || !st.bufs[size_t(idx)])
                return false;
            std::array<const uint8_t*, kMaxInputs> in{};
            for (size_t k = 0; k < n.inputCount && k < in.size(); ++k) {
                if (((missing >> k) & 1u) != 0)
                    continue;
                const int j = st.IndexOf(n.inputs[k].product);
                if (j < 0 || !st.bufs[size_t(j)] || seq < n.inputs[k].lag)
                    continue;
                const ReadInfo ri = st.table->At(size_t(j)).Read(seq - n.inputs[k].lag);
                if (ri.kind == ReadInfo::Ring)
                    in[k] = st.bufs[size_t(j)]->Ring(size_t(ri.index));
                else if (ri.kind == ReadInfo::Live)
                    in[k] = st.bufs[size_t(j)]->Live(size_t(ri.index));
            }
            SourceFrameView view;
            const bool haveView = st.ring->TryGet(seq, &view);   // lock-free, no keepAlive copy: the render holds the frame
            ok = it->second(n, haveView ? &view : nullptr, in, st.bufs[size_t(idx)]->Live(size_t(live)),
                            st.bufs[size_t(idx)]->Bytes());
        } else if (liveFill_) {
            ok = liveFill_(n, seq, live, hdr, missing);
        } else if (dispatcher_ != nullptr && n.backend == Backend::GpuCompute) {
            ok = dispatcher_->DispatchLive(n, seq, live, hdr, missing);
        }
        if (ok && nowUs_) {
            const uint64_t t1 = nowUs_();
            const uint64_t dt = t1 > t0 ? t1 - t0 : 0u;
            if (hdr.costUs == 0)
                hdr.costUs = dt > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(dt);
        }
        return ok;
    }

    VpIndicator::Indicator* ind_;
    std::function<uint64_t()> nowUs_;
    LaneSettings settings_;
    std::shared_ptr<LaneState> state_;         // swapped with std::atomic_store / atomic_load
    std::shared_ptr<LaneState> renderState_;   // the render thread's hold (keeps returned references valid)
    GraphOwner owner_;
    VramGovernor governor_;
    Regulator regulator_;
    std::vector<RegProduct> regProducts_;
    std::vector<ILaneProducer*> producers_;
    IGpuDispatcher* dispatcher_ = nullptr;
    LiveFill liveFill_;
    std::map<ProductId, CpuLiveCompute> cpuLive_;
    std::function<uint64_t(ProductId)> releaseCompleted_;
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
