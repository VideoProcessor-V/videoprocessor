// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// S-3a S1 (lead9/brief-S-3a-scheduler.md s4 S1; refs/122 S1; 93-prep-B3Z s2, s3): P3 for each queued source frame is made ONCE in the
// post-present slot as a Core job, not in the render; render-time matching stays only as the visible last fallback (doc 91 C :77, owner
// 2026-11-11 07:10: "P3 ... CORE GUARANTEED PRECOMPUTE"; OD:612 (1), 2026-11-12 15:13).
//
// The ISpareWork adapter of the matcher top-up: Id = kP3AnalysisField, Class = SpareClassOf(Core), Order = the graph order (core/JobRegistry.h),
// RowsLeft = pairs still wanted this slot (one pair = one unsplittable matcher call; the slot row-splits by whole pairs). Run estimates pairs in
// strict chain order through the ENGINE (ILaneAheadEngine: the vector engine's one matcher with P2 / P2m and the P7 guidance it Acquires itself;
// never a second ahead path - the engine's AheadSchedule is the one arbiter, the chain bookkeeping is LaneAhead::AheadRing) and holds each
// result in the ring (Z1). The render takes a held entry through ConsumeAtRender; a miss estimates at render as today, counted by cause.
// Source planes: each pair's two frames are ACQUIRED from the P13 source ring (kP13SourceRing, AcquireForm::Gpu) - never a lane-held copy
// (rule 1; coordinator ruling via lead 10, 2026-11-14: "S1 must Acquire the P13 SourceRing product"). Today P13's views end at AfterRender of
// their frame (lead8/iface-L-CORE.md s6), so in the post-present slot the Acquire is Unavailable and the pair is not estimated (reason
// SourceNotServed, counted with the Acquire cause, on the status line). The needed change is P13's OWNER's: serve views of the queued frames
// through the slot (named P13-AHEAD-VIEWS, routed by lead 10; the same trigger as 102b S3A-UNPACK-SLOT).
//
// Rules: a pair is estimated at most once (the ring refuses a held pair; the slot runs once per SOURCE frame); low latency (games, 1 queued)
// = no field ahead, competitive = no slot work (R9; the reason is on the status line); without the optical-flow producer the matcher runs on
// P2m (doc 91 C :78: Degraded still works, the policy never waits for P2 then); a Core job is never cost-decayed (R4: the cost is the measured
// per-pair EWMA, only a measurement changes it). The adapter is pure (no host types): the engine side is wired by the matcher lane's owner.

#include <VP-V/core/Products.h>
#include <VP-V/core/SchedulerContract.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/motion/estimation/LaneAheadPolicy.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace VpMotion {
namespace LaneAhead {

// The engine's side (render thread, render device). Implemented by the vector engine at wiring time; tests use a fake.
class ILaneAheadEngine {
public:
    virtual ~ILaneAheadEngine() {}
    // The policy facts of this slot (key, latency mode, matcher state, queue, lane lead, P2 producer state).
    virtual Input Facts() const = 0;
    // The queued pair that starts at frame `from` (0 = the pair after the render frame's next pair), checked against the dequeue's rules
    // (PairValidAt, adjacency, same layout) and stamped with the current epoch. False = no such pair queued.
    virtual bool PairFrom(uint64_t from, EntryKey* out) const = 0;
    // The matcher on that pair (through the engine's own inputs: P2 or P2m, P7 guidance), reading the pair's source planes from the P13
    // source-ring views the adapter Acquired (`fromPlanes`, `toPlanes`: pinned for the call; the engine records its GPU read on them), its
    // outputs written into ring slot `slot`. Never a private plane copy (rule 1). Returns the measured ms (>= 0) or < 0 on failure.
    virtual double Estimate(const EntryKey& pair, int slot, const VpSkeleton::AcquireResult& fromPlanes, const VpSkeleton::AcquireResult& toPlanes) = 0;
    // The source frame the slot serves (the slot's frame key; 0 = unknown).
    virtual uint64_t SourceFrame() const = 0;
};

// The skeleton's one access path for a shared product (core/SkeletonAcquire.h SkeletonMemo::Acquire): the production binding is
// [&memo](id, seq, form) { return memo.Acquire(id, seq, form); } with the module's MemoOf(host).
using AcquireFn = std::function<VpSkeleton::AcquireResult(VpSkeleton::ProductId, uint64_t, VpSkeleton::AcquireForm)>;

inline AcquireFn AcquireThrough(VpSkeleton::SkeletonMemo* memo)
{
    return [memo](VpSkeleton::ProductId id, uint64_t seq, VpSkeleton::AcquireForm form) {
        return memo != nullptr ? memo->Acquire(id, seq, form) : VpSkeleton::AcquireResult();
    };
}

class LaneAheadSpareWork : public VpSkeleton::ISpareWork {
public:
    // `ring` is the ONE chain ring of ahead pairs, owned by the engine side (Dejudder::AheadSchedule holds it at wiring: 93-prep-B3Z s3 "ONE
    // arbiter"; estimation README: Z1 maps onto AheadSchedule, not a second ring). The adapter only fills and consumes it.
    LaneAheadSpareWork(ILaneAheadEngine* engine, AcquireFn acquire, uint32_t graphOrder, AheadRing& ring)
        : m_engine(engine), m_acquire(std::move(acquire)), m_order(graphOrder), m_ring(ring)
    {
    }

    // ISpareWork
    uint16_t Id() const override { return VpSkeleton::kP3AnalysisField; }
    VpSkeleton::SpareClass Class() const override { return VpSkeleton::SpareClassOf(VpSkeleton::JobClass::Core); }
    uint32_t Order() const override { return m_order; }
    bool HasWork() const override
    {
        Refresh();
        return m_pairsWanted > 0;
    }
    double PredictMs() const override
    {
        const double perPair = m_cost.PredictMs();
        return perPair > 0.0 ? perPair * double(m_pairsWanted > 0 ? m_pairsWanted : 1) : 0.0;
    }
    // Splittable by whole pairs only when more than one pair is wanted: one pair is one matcher call, so a single pair that does not fit is
    // DEFERRED (R4), never "split" into an overrun (PostPresentSpare's split always runs at least one row).
    uint32_t RowsLeft() const override { return m_pairsWanted > 1 ? uint32_t(m_pairsWanted) : 0u; }
    double Run(uint32_t rows) override
    {
        if (m_engine == nullptr) return -1.0;
        const uint64_t frame = m_engine->SourceFrame();
        m_ranFrame = frame;
        int want = m_pairsWanted;
        if (rows > 0 && int(rows) < want) want = int(rows);
        double used = 0.0;
        int done = 0;
        for (int i = 0; i < want; ++i) {
            EntryKey k;
            if (!m_engine->PairFrom(m_ring.BackTo(), &k)) { m_reasons.Note(Reason::ShortQueue); break; }
            if (m_ring.Holds(k.from, k.to)) { ++m_alreadyHeld; break; }   // compute once: never a second estimate of a held pair
            // The pair's source planes: Acquire the P13 source ring (one copy, rule 1; coordinator ruling via lead 10 2026-11-14). Unavailable
            // = its owner serves no view of that frame in the slot (today P13 views end at AfterRender): not estimated, counted, visible; the
            // render estimates the pair as today.
            const VpSkeleton::AcquireResult fromPlanes = m_acquire ? m_acquire(VpSkeleton::kP13SourceRing, k.from, VpSkeleton::AcquireForm::Gpu)
                                                                   : VpSkeleton::AcquireResult();
            const VpSkeleton::AcquireResult toPlanes = fromPlanes.Ok() && m_acquire
                ? m_acquire(VpSkeleton::kP13SourceRing, k.to, VpSkeleton::AcquireForm::Gpu) : VpSkeleton::AcquireResult();
            if (!fromPlanes.Ok() || !toPlanes.Ok()) {
                m_lastSourceCause = !fromPlanes.Ok() ? fromPlanes.cause : toPlanes.cause;
                ++m_sourceNotServed;
                m_reasons.Note(Reason::SourceNotServed);
                break;
            }
            const int slot = m_ring.Held();
            const double ms = m_engine->Estimate(k, slot, fromPlanes, toPlanes);
            if (!(ms >= 0.0)) { ++m_failed; if (done == 0) return -1.0; break; }
            if (m_ring.Push(k) != PushResult::Held) { ++m_pushRefused; break; }
            m_cost.Note(ms);
            used += ms;
            ++done;
            ++m_estimated;
            if (k.to > m_lastTo) { m_lastTo = k.to; ++m_framesMade; }
        }
        if (done > 0) m_reasons.Note(Reason::Ahead);
        m_pairsWanted = m_pairsWanted > done ? m_pairsWanted - done : 0;
        return used;
    }

    // The render frame's estimate of (from, to): true = taken from the ring (no render-time P3); false = estimate now (counted with the
    // reason the slot last gave: the visible fallback).
    bool ConsumeAtRender(uint64_t from, uint64_t to, uint64_t epoch)
    {
        if (m_ring.Consume(from, to, epoch)) { ++m_renderSaved; return true; }
        ++m_renderFallback;
        ++m_fallbackBy[static_cast<size_t>(m_reasons.Last())];
        return false;
    }

    // A resync / seek / queue reset (host): the ring is discarded (counted), the decision starts over.
    void Reset() { m_ring.Reset(); m_pairsWanted = 0; m_offeredFrame = 0; m_ranFrame = 0; }

    AheadRing& Ring() { return m_ring; }
    const AheadRing& Ring() const { return m_ring; }
    const ReasonCounts& Reasons() const { return m_reasons; }
    const VpSkeleton::SpareMeasuredCost& PairCost() const { return m_cost; }
    unsigned long long Estimated() const { return m_estimated; }
    unsigned long long FramesMade() const { return m_framesMade; }          // distinct `to` frames made (<= 1 estimate per frame)
    unsigned long long RenderSaved() const { return m_renderSaved; }        // render estimates replaced by a ring entry
    unsigned long long RenderFallback() const { return m_renderFallback; }  // render-time P3 (the visible fallback)
    unsigned long long RenderFallbackBy(Reason r) const { return m_fallbackBy[static_cast<size_t>(r)]; }
    unsigned long long Failed() const { return m_failed; }
    unsigned long long AlreadyHeld() const { return m_alreadyHeld; }
    unsigned long long SourceNotServed() const { return m_sourceNotServed; }
    VpSkeleton::AcquireCause LastSourceCause() const { return m_lastSourceCause; }

    // Ctrl+I / Config status / Ctrl+Alt+D (rule 5; 93-prep-B3Z s4 line form).
    std::string StatusLine() const
    {
        const Input in = m_engine ? m_engine->Facts() : Input();
        char buf[360] = {};
        std::snprintf(buf, sizeof(buf),
            "motion field ahead (%s): depth %d/%d; slot %llu, render %llu; discarded: drop %llu, resync %llu, next differs %llu, epoch %llu, "
            "picture %llu; last: %s",
            KeyStateText(in.key, in.autoAccepted), m_ring.Held(), m_ring.Capacity(), m_renderSaved, m_renderFallback,
            m_ring.Discarded(DiscardCause::Drop), m_ring.Discarded(DiscardCause::Resync), m_ring.Discarded(DiscardCause::NextDiffers),
            m_ring.Discarded(DiscardCause::EpochMoved), m_ring.Discarded(DiscardCause::PictureArea), ReasonName(m_reasons.Last()));
        std::string s(buf);
        if (m_sourceNotServed > 0) s += std::string("; source ring not served ") + std::to_string(m_sourceNotServed) + " (" + VpSkeleton::Token(m_lastSourceCause) + ")";
        return s;
    }

private:
    // One decision per source frame (the slot asks HasWork once per frame it runs). A frame whose wanted work the slot did not run is the
    // slot's NoSpare verdict (core deferred), noted when the next frame is decided.
    void Refresh() const
    {
        if (m_engine == nullptr) { m_pairsWanted = 0; return; }
        const uint64_t frame = m_engine->SourceFrame();
        if (frame != 0 && frame == m_offeredFrame) return;   // already decided for this frame
        if (m_offeredFrame != 0 && m_pairsWanted > 0 && m_ranFrame != m_offeredFrame) m_reasons.Note(Reason::NoSpare);
        m_offeredFrame = frame;
        Input in = m_engine->Facts();
        in.held = m_ring.Held();
        const Decision d = Decide(in);
        if (d.reason != Reason::Ahead) {
            m_reasons.Note(d.reason);
            m_pairsWanted = 0;
            return;
        }
        const int room = m_ring.Capacity() - m_ring.Held();   // the ring's own depth (its VRAM tier) bounds the policy depth
        m_pairsWanted = d.pairsWanted < room ? d.pairsWanted : (room > 0 ? room : 0);
        if (m_pairsWanted == 0) m_reasons.Note(Reason::RingFull);
    }

    ILaneAheadEngine* m_engine;
    AcquireFn m_acquire;
    uint32_t m_order;
    AheadRing& m_ring;
    VpSkeleton::SpareMeasuredCost m_cost;   // per pair; never decayed (R4)
    mutable ReasonCounts m_reasons;
    mutable int m_pairsWanted = 0;
    mutable uint64_t m_offeredFrame = 0;
    uint64_t m_ranFrame = 0;
    uint64_t m_lastTo = 0;
    unsigned long long m_estimated = 0, m_framesMade = 0, m_renderSaved = 0, m_renderFallback = 0;
    unsigned long long m_failed = 0, m_alreadyHeld = 0, m_pushRefused = 0, m_sourceNotServed = 0;
    VpSkeleton::AcquireCause m_lastSourceCause = VpSkeleton::AcquireCause::None;
    unsigned long long m_fallbackBy[static_cast<size_t>(Reason::Count)] = {};
};

}   // namespace LaneAhead
}   // namespace VpMotion
