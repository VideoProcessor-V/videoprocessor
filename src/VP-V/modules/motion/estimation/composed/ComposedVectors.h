// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ENGINE-COMPOSED-VECTORS: the shared per-pixel COMPOSED vectors of the motion engine (OD 11-14, lead 13 via the coordinator; owner
// 2026-11-14 "weak texture": option B = 6 neighbours along the vectors, current frame excluded; "ONE shared skeleton neighbour-warp product
// read by TD and P10g v2"; lead12\proof-weaktex-BC.md s11 row "pairwise / composed vectors t <-> t+-k and trust": shared P2/P7 + CellTrust
// memo, epochs from MotionValidity, consumer only, no private matcher or trust map).
//
// WHAT IT IS. For a current frame t and an offset k it answers, for every output cell of a per-pixel grid, where that pixel is in frame
// t - k (back) or t + k (ahead): the displacement (source px), a trust in [0, 1] and an uncertainty in px. The displacement is the CHAIN of
// the engine's ADJACENT pairs, each sampled bilinearly (so the field is per pixel, not per 16 px block) at the position the chain has
// reached:
//     d_k(p) = d_{k-1}(p) + V_{(t-k+1) -> (t-k)}( p + d_{k-1}(p) )            (ahead: V_{(t+k-1) -> (t+k)})
//     trust_k = trust_{k-1} x cellTrust x FallOff(consistency)               consistency = |V_ab(q) + V_ba(q + V_ab(q))| at the landing point
//     unc_k   = unc_{k-1} + (1 - cellTrust) x cellPx + consistency           (the TemporalDetail composed-uncertainty rule, TemporalDetailModel.h)
// A pixel whose chain leaves the frame, or whose pair is absent, has trust 0 from that step on (never a guessed vector).
//
// WHERE THE PAIRS COME FROM. Only through IPairSource: the engine's P2 pairs and the ONE CellTrust (OpticalFlowTrust::CellTrust) are served
// by the adapter (the same function TemporalDetail::SharedMotionSource uses); the product never starts the engine, never searches for motion
// and keeps no trust definition of its own (rule 21, N1). The adapter applies MotionValidity (PairServes / PairGenerationCurrent); the
// product additionally drops every plane on Invalidate() (an epoch cause, rule 25) so a stale chain is never served.
//
// DEPTH. The reader-declared depth law is ComposedVectorPlan.h (8 back + 4 ahead; low latency 10 + 0; ahead = max active request capped by
// VpSkeleton::kAheadCeiling and the queue). Acquire() refuses an offset beyond the plan (OutOfPlan, counted): a reader cannot widen the
// ahead compute by asking.
//
// ONE COMPUTATION. A plane (t, direction, k) is computed at most once per epoch and step (counter `planesComputed`; `planeHits` counts the
// shares). Plane k builds on plane k - 1 (get-or-compute-and-publish, recursive, rule 2). The grid step (source px per output cell) is a
// variant axis of the ONE product: the FINEST step any active reader declares is computed, coarser readers sample it bilinearly (Sample).
//
// COST / FORM (owner R14-16). The product form is a BLOCK field (kBlockStep = 8 source px per cell: 4K = 480 x 270 cells, 14 planes x 0.13 M cells per
// frame) read through the shared plane sampler: Plane::At (bilinear) and its landing form Plane::Warp (what the readers call). Product::Value now goes through Plane::Warp. ComposeStep (the PRODUCER, not a consumer path) samples the adjacent PAIR fields (SampleField: PairField is the engine's input, not a plane) and reads the previous plane cell by cell on the same grid; no reader ever calls it. The CPU per-pixel form (step 1) is a REFERENCE only
// (4K: 14 planes x 8.3 M px per frame is not a render-time budget); a reader may ask for a finer step (SetReaderStep), the finest active step wins. Form / step contract: a plane is
// ALWAYS a raster of `step` source px per cell with trust and uncertainty; step 1 = per pixel; a GPU form must publish the same fields at the same step.
// CALLERS NOT BUILT HERE: the IPairSource adapter over the P2 slot / CellTrust memo / MotionValidity (the grain-level and TD lanes) and the Invalidate()
// call at every epoch cause (MotionValidity epoch change) are wired by those lanes; this product exposes the seams.
//
// GPU. This is the CPU form and the product contract; a GPU form of the block field (one dispatch per offset, same fields, same step) is a later
// step (102b ECV-GPU-FORM closed by decision R14-16: block field + shared plane sampler). Header-only, C++14, no Windows headers. Render / lane thread, one caller at a time
// (the lane serialises; the class holds no lock).

#include "ComposedVectorPlan.h"

#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace VpComposed {

// One adjacent pair from -> to on from's grid: vx / vy = (position in `to`) - (position in `from`), source px; trust 0..1 per cell.
struct PairField
{
    bool valid = false;
    int gridW = 0, gridH = 0;
    double cellPx = 16.0;                       // source px per cell
    std::vector<float> vx, vy, trust;           // gridW x gridH, raster

    static PairField Uniform(int imageW, int imageH, double cellPx, double dx, double dy, double trust = 1.0)
    {
        PairField f;
        f.valid = true;
        f.cellPx = cellPx;
        f.gridW = static_cast<int>(std::ceil(imageW / cellPx));
        f.gridH = static_cast<int>(std::ceil(imageH / cellPx));
        const size_t n = static_cast<size_t>(f.gridW) * f.gridH;
        f.vx.assign(n, static_cast<float>(dx));
        f.vy.assign(n, static_cast<float>(dy));
        f.trust.assign(n, static_cast<float>(trust));
        return f;
    }
    bool Usable() const
    {
        const size_t n = static_cast<size_t>(gridW > 0 ? gridW : 0) * static_cast<size_t>(gridH > 0 ? gridH : 0);
        return valid && n > 0 && cellPx > 0.0 && vx.size() >= n && vy.size() >= n && trust.size() >= n;
    }
};

class IPairSource
{
public:
    virtual ~IPairSource() {}
    // The vectors of the ADJACENT pair (from -> to), |from - to| == 1, on from's grid. False = absent or stale (the implementation applies the
    // engine's vectors-valid state). `why` (optional) gets the plain reason for the status line.
    virtual bool Adjacent(uint64_t from, uint64_t to, PairField& out, std::string* why) const = 0;
};

enum class Dir : uint8_t { Back = 0, Ahead = 1 };

// The product form (owner R14-16): a BLOCK field of kBlockStep source px per cell, read through the shared plane sampler (Plane::At / Plane::Warp). Per-pixel
// (step 1) stays selectable per reader (SetReaderStep) as the reference form only.
constexpr int kBlockStep = 8;

struct Sampled
{
    float dx = 0.0f, dy = 0.0f, trust = 0.0f, unc = 0.0f;
};

// The composed field of one (frame, direction, offset): ow x oh output cells, cell (i, j) centred on source position ((i + 0.5) step, (j + 0.5) step).
struct Plane
{
    uint64_t seq = 0;
    Dir dir = Dir::Back;
    uint16_t k = 0;
    int step = 1;
    int srcW = 0, srcH = 0;
    int w = 0, h = 0;
    std::vector<float> dx, dy, trust, unc;
    // True when a step of the chain had no REVERSE pair (the newest queued frame has none yet): the trust is the pair trust x kNoReverseTrust and the
    // uncertainty carries 2 x consistencyPx instead of the measured landing error. Such a plane is returned but never cached (recomputed once the pair exists).
    bool degraded = false;

    size_t Bytes() const { return (dx.size() + dy.size() + trust.size() + unc.size()) * sizeof(float); }
    // The reference frame of this plane (the frame the displacements point into).
    uint64_t RefSeq() const { return dir == Dir::Back ? seq - k : seq + k; }

    // Bilinear value at source position (x, y) (pixel-centre coordinates; clamped to the grid). A position outside the frame has trust 0.
    Sampled At(double x, double y) const
    {
        Sampled s;
        if (w <= 0 || h <= 0 || dx.empty())
            return s;
        if (!(x >= 0.0 && y >= 0.0 && x <= srcW && y <= srcH))
            return s;
        const double gx = (std::min)((std::max)(x / step - 0.5, 0.0), double(w - 1));
        const double gy = (std::min)((std::max)(y / step - 0.5, 0.0), double(h - 1));
        const int x0 = static_cast<int>(gx), y0 = static_cast<int>(gy);
        const int x1 = (std::min)(x0 + 1, w - 1), y1 = (std::min)(y0 + 1, h - 1);
        const float fx = static_cast<float>(gx - x0), fy = static_cast<float>(gy - y0);
        const size_t i00 = static_cast<size_t>(y0) * w + x0, i10 = static_cast<size_t>(y0) * w + x1;
        const size_t i01 = static_cast<size_t>(y1) * w + x0, i11 = static_cast<size_t>(y1) * w + x1;
        const auto mix = [&](const std::vector<float>& v) {
            const float a = v[i00] + (v[i10] - v[i00]) * fx;
            const float b = v[i01] + (v[i11] - v[i01]) * fx;
            return a + (b - a) * fy;
        };
        s.dx = mix(dx);
        s.dy = mix(dy);
        s.trust = mix(trust);
        s.unc = mix(unc);
        return s;
    }

    // The shared landing sampler of a plane (owner R14-16; the ONE reader-facing entry, built on At): where the chain of source position (x, y) lands in the reference frame, with its trust and uncertainty.
    // Every reader (P10g, TemporalDetail, the chain itself) reads the field only through this; `valid` is false outside the frame, for a dead chain
    // (trust 0) and when the landing leaves the frame (never a guessed vector).
    struct Warped
    {
        double x = 0.0, y = 0.0;
        float trust = 0.0f, unc = 0.0f;
        bool valid = false;
    };
    Warped Warp(double x, double y) const
    {
        Warped r;
        const Sampled s = At(x, y);
        r.trust = s.trust;
        r.unc = s.unc;
        r.x = x + s.dx;
        r.y = y + s.dy;
        r.valid = s.trust > 0.0f && r.x >= 0.0 && r.y >= 0.0 && r.x <= srcW && r.y <= srcH;
        return r;
    }
};

using PlanePtr = std::shared_ptr<const Plane>;

enum class Got : uint8_t
{
    Ok = 0,
    OutOfPlan,     // the offset is beyond the plan (no active reader, the ceiling, the queue, low latency): never computed
    PairAbsent,    // a pair of the chain is absent or stale (the engine's vectors-valid state, no producer, a cut)
    BadGeometry,   // no source size / a pair that does not fit
    Budget,        // the byte budget of the kept planes would be exceeded
};

inline const char* GotName(Got g)
{
    switch (g) {
    case Got::Ok: return "ok";
    case Got::OutOfPlan: return "beyond the plan";
    case Got::PairAbsent: return "pair absent";
    case Got::BadGeometry: return "geometry unknown";
    case Got::Budget: return "plane budget reached";
    }
    return "?";
}

struct Reached
{
    uint16_t back = 0, ahead = 0;   // contiguous valid offsets 1..n computed for the frame
    uint16_t degradedBack = 0, degradedAhead = 0;   // of those, planes built without a reverse pair (pair trust only; visible in Status)
    Got backWhy = Got::Ok, aheadWhy = Got::Ok;
    std::string lastWhy;            // the adapter's plain reason for the last absent pair
};

struct Stats
{
    uint64_t planesComputed = 0, planeHits = 0;
    uint64_t outOfPlan = 0, chainBreaks = 0, badGeometry = 0, budgetRefused = 0, reverseAbsent = 0;
    uint64_t invalidations = 0, stepRebuilds = 0, evictions = 0;
    uint64_t bytes = 0;             // the kept planes
};

class Product
{
public:
    struct Config
    {
        // Landing consistency |V_ab(q) + V_ba(r)| in SOURCE px: weight 1 up to this, 0 from twice it (OpticalFlowTrust::FallOff, the engine's shape).
        double consistencyPx = 1.0;
        // Weight of a step whose reverse pair is absent (no landing check possible): the pair trust is multiplied by this and the uncertainty grows as if the
        // landing error were at its zero-trust edge (2 x consistencyPx). Never a silent fallback: Reached::degraded*, Status and the Ctrl+I decision say so.
        double noReverseTrust = 0.5;
        // Upper bound of the kept planes (bytes). 0 = unbounded. A refused plane is visible (Stats.budgetRefused, Status).
        uint64_t budgetBytes = 0;
    };

    explicit Product(const IPairSource* source) : m_source(source) {}
    Product(const IPairSource* source, const Config& cfg) : m_source(source), m_cfg(cfg) {}

    void SetSourceSize(int width, int height)
    {
        if (width == m_w && height == m_h)
            return;
        m_w = width;
        m_h = height;
        DropAll();
    }

    // ---- readers (get-or-compute: a reader that is not active costs nothing) ----
    void Activate(Reader r, bool on)
    {
        if (m_active[Idx(r)] == on)
            return;
        m_active[Idx(r)] = on;
        if (on && m_need[Idx(r)][0].back == 0 && m_need[Idx(r)][0].ahead == 0 && m_need[Idx(r)][1].back == 0 && m_need[Idx(r)][1].ahead == 0)
            SetNeed(r, DefaultNeed(r, false), DefaultNeed(r, true));
        RecomputeStep();
    }
    void SetNeed(Reader r, const Need& standard, const Need& lowLatency)
    {
        m_need[Idx(r)][0] = standard;
        m_need[Idx(r)][1] = lowLatency;
    }
    // The finest grid step (source px per output cell) the reader wants; the product runs the FINEST of the active readers.
    void SetReaderStep(Reader r, int step)
    {
        m_readerStep[Idx(r)] = step < 1 ? 1 : step;
        RecomputeStep();
    }
    void SetMode(bool lowLatency) { m_lowLatency = lowLatency; }
    // The frames the queue holds after / the keep window holds behind the current frame (QueueFrames.h, the keep window = the full queue back).
    void SetWindow(unsigned queuedAhead, unsigned keptBehind)
    {
        m_queuedAhead = queuedAhead;
        m_keptBehind = keptBehind;
    }

    Plan CurrentPlan() const
    {
        PlanInput in;
        for (size_t i = 0; i < static_cast<size_t>(Reader::kCount); ++i) {
            in.active[i] = m_active[i];
            in.need[i] = m_need[i][m_lowLatency ? 1 : 0];
        }
        in.lowLatency = m_lowLatency;
        in.queuedAhead = m_queuedAhead;
        in.keptBehind = m_keptBehind;
        return PlanDepth(in);
    }
    int Step() const { return m_step; }

    // ---- compute ----
    // Compute (or share) every plane of the plan for frame t. Contiguous: the first missing pair ends that direction (offsets beyond it are
    // not reachable through this frame's chain, whatever later pairs hold).
    Reached Prepare(uint64_t t)
    {
        Reached r;
        const Plan plan = CurrentPlan();
        const Dir dirs[2] = { Dir::Back, Dir::Ahead };
        for (const Dir d : dirs) {
            const uint16_t want = d == Dir::Back ? plan.back : plan.ahead;
            uint16_t got = 0, degraded = 0;
            Got why = Got::Ok;
            for (uint16_t k = 1; k <= want; ++k) {
                PlanePtr p;
                why = Acquire(t, d, k, p, &r.lastWhy);
                if (why != Got::Ok)
                    break;
                got = k;
                if (p && p->degraded)
                    ++degraded;
            }
            (d == Dir::Back ? r.back : r.ahead) = got;
            (d == Dir::Back ? r.degradedBack : r.degradedAhead) = degraded;
            (d == Dir::Back ? r.backWhy : r.aheadWhy) = why;
        }
        m_last = r;
        m_haveLast = true;
        return r;
    }

    // One plane. k == 0 is not a plane (identity). Beyond the plan: OutOfPlan.
    Got Acquire(uint64_t t, Dir dir, uint16_t k, PlanePtr& out, std::string* why = nullptr)
    {
        out.reset();
        if (k == 0) {
            ++m_stats.outOfPlan;
            return Got::OutOfPlan;
        }
        const Plan plan = CurrentPlan();
        if (k > (dir == Dir::Back ? plan.back : plan.ahead)) {
            ++m_stats.outOfPlan;
            return Got::OutOfPlan;
        }
        return Get(t, dir, k, out, why);
    }

    // ---- lifetime ----
    // An epoch cause (resync, cut, gap, seek, settings, source format ...): every plane is dropped, none is ever served across it.
    void Invalidate()
    {
        if (!m_planes.empty())
            DropAll();
        ++m_stats.invalidations;
        m_haveLast = false;
    }
    // Planes whose frame lies outside [keepFrom, keepTo] are released.
    void Prune(uint64_t keepFrom, uint64_t keepTo)
    {
        for (auto it = m_planes.begin(); it != m_planes.end();) {
            if (it->first.seq < keepFrom || it->first.seq > keepTo) {
                m_stats.bytes -= it->second->Bytes();
                it = m_planes.erase(it);
                ++m_stats.evictions;
            } else {
                ++it;
            }
        }
    }

    const Stats& GetStats() const { return m_stats; }
    size_t PlaneCount() const { return m_planes.size(); }

    // The reader-facing status: engaged while the last Prepare could not reach what the plan asked for BECAUSE a pair was absent (the queue /
    // ceiling / low-latency shapes are the plan's own and are in Plan::note). The text is plain words for the Ctrl+I line (rule 5).
    struct Status
    {
        bool engaged = false;
        std::string text;
    };
    Status CurrentStatus() const
    {
        Status s;
        if (!m_haveLast)
            return s;
        const Plan plan = CurrentPlan();
        const bool backShort = m_last.back < plan.back && m_last.backWhy != Got::Ok;
        const bool aheadShort = m_last.ahead < plan.ahead && m_last.aheadWhy != Got::Ok;
        const unsigned noRev = static_cast<unsigned>(m_last.degradedBack) + m_last.degradedAhead;
        if (!backShort && !aheadShort && noRev == 0)
            return s;
        s.engaged = true;
        if (!backShort && !aheadShort) {
            s.text = "composed vectors: " + std::to_string(noRev) + " planes without a reverse pair (pair trust only)";
            return s;
        }
        s.text = "composed vectors reach " + std::to_string(m_last.back) + " of " + std::to_string(plan.back) + " frames back and " +
                 std::to_string(m_last.ahead) + " of " + std::to_string(plan.ahead) + " ahead (" +
                 (m_last.lastWhy.empty() ? std::string(GotName(backShort ? m_last.backWhy : m_last.aheadWhy)) : m_last.lastWhy) + ")";
        return s;
    }

    // A pure helper for tests and for readers that need a single point: the composed value of pixel (x, y) of frame t at offset k, or trust 0.
    // It goes through the shared sampler Plane::Warp (landing form), so a dead chain or a landing outside the frame reads as trust 0 here too.
    static Sampled Value(const PlanePtr& p, double x, double y)
    {
        if (!p)
            return Sampled();
        const Sampled raw = p->At(x, y);
        const Plane::Warped w = p->Warp(x, y);
        Sampled s = raw;
        if (w.valid) {
            s.dx = static_cast<float>(w.x - x);
            s.dy = static_cast<float>(w.y - y);
        } else {
            s.trust = 0.0f;
        }
        s.unc = w.unc;
        return s;
    }

private:
    struct Key
    {
        uint64_t seq;
        uint8_t dir;
        uint16_t k;
        bool operator<(const Key& o) const
        {
            if (seq != o.seq) return seq < o.seq;
            if (dir != o.dir) return dir < o.dir;
            return k < o.k;
        }
    };

    static size_t Idx(Reader r) { return static_cast<size_t>(r); }

    void RecomputeStep()
    {
        int step = 0;
        for (size_t i = 0; i < static_cast<size_t>(Reader::kCount); ++i)
            if (m_active[i])
                step = step == 0 ? m_readerStep[i] : (std::min)(step, m_readerStep[i]);
        if (step == 0)
            step = m_step;   // nobody active: keep the last (nothing is computed anyway)
        if (step != m_step) {
            m_step = step;
            DropAll();
            ++m_stats.stepRebuilds;
        }
    }

    void DropAll()
    {
        m_planes.clear();
        m_stats.bytes = 0;
    }

    Got Get(uint64_t t, Dir dir, uint16_t k, PlanePtr& out, std::string* why)
    {
        if (m_w <= 0 || m_h <= 0 || !m_source) {
            ++m_stats.badGeometry;
            return Got::BadGeometry;
        }
        const Key key{ t, static_cast<uint8_t>(dir), k };
        const auto hit = m_planes.find(key);
        if (hit != m_planes.end()) {
            ++m_stats.planeHits;
            out = hit->second;
            return Got::Ok;
        }
        // Prerequisite: plane k - 1 (recursive publish).
        PlanePtr prev;
        if (k > 1) {
            const Got g = Get(t, dir, static_cast<uint16_t>(k - 1), prev, why);
            if (g != Got::Ok)
                return g;
        }
        // The pair of this step: back = (t-k+1) -> (t-k), ahead = (t+k-1) -> (t+k); the reverse is the landing consistency check.
        if (dir == Dir::Back && t < k) {   // before the first frame
            ++m_stats.chainBreaks;
            if (why) *why = "no frame that far back";
            return Got::PairAbsent;
        }
        const uint64_t a = dir == Dir::Back ? t - (k - 1u) : t + (k - 1u);
        const uint64_t b = dir == Dir::Back ? t - k : t + k;
        // The forward step pair is required; the REVERSE pair (the landing check) may be absent for the newest queued frame: then the step runs on the pair
        // trust alone, flagged degraded (visible), not cached. An adapter that serves both (the engine serves a pair in both directions) never degrades.
        PairField ab, ba;
        std::string reason;
        if (!m_source->Adjacent(a, b, ab, &reason) || !ab.Usable()) {
            ++m_stats.chainBreaks;
            if (why) *why = reason.empty() ? std::string("a pair of the chain is absent") : reason;
            return Got::PairAbsent;
        }
        const uint64_t bytes = static_cast<uint64_t>((m_w + m_step - 1) / m_step) * static_cast<uint64_t>((m_h + m_step - 1) / m_step) * 4u * sizeof(float);
        if (m_cfg.budgetBytes && m_stats.bytes + bytes > m_cfg.budgetBytes) {
            ++m_stats.budgetRefused;
            if (why) *why = "the plane budget is reached";
            return Got::Budget;
        }
        std::shared_ptr<Plane> p = std::make_shared<Plane>();
        p->seq = t;
        p->dir = dir;
        p->k = k;
        p->step = m_step;
        p->srcW = m_w;
        p->srcH = m_h;
        p->w = (m_w + m_step - 1) / m_step;
        p->h = (m_h + m_step - 1) / m_step;
        const bool haveBa = m_source->Adjacent(b, a, ba, nullptr) && ba.Usable();
        ComposeStep(prev.get(), ab, haveBa ? &ba : nullptr, *p);
        p->degraded = !haveBa || (prev && prev->degraded);
        ++m_stats.planesComputed;
        out = p;
        if (p->degraded) {
            ++m_stats.reverseAbsent;
            return Got::Ok;   // served, not kept: recomputed when the reverse pair exists
        }
        m_stats.bytes += p->Bytes();
        m_planes[key] = out;
        return Got::Ok;
    }

    static Sampled SampleField(const PairField& f, double x, double y)
    {
        Sampled s;
        const double gx = (std::min)((std::max)(x / f.cellPx - 0.5, 0.0), double(f.gridW - 1));
        const double gy = (std::min)((std::max)(y / f.cellPx - 0.5, 0.0), double(f.gridH - 1));
        const int x0 = static_cast<int>(gx), y0 = static_cast<int>(gy);
        const int x1 = (std::min)(x0 + 1, f.gridW - 1), y1 = (std::min)(y0 + 1, f.gridH - 1);
        const float fx = static_cast<float>(gx - x0), fy = static_cast<float>(gy - y0);
        const size_t i00 = static_cast<size_t>(y0) * f.gridW + x0, i10 = static_cast<size_t>(y0) * f.gridW + x1;
        const size_t i01 = static_cast<size_t>(y1) * f.gridW + x0, i11 = static_cast<size_t>(y1) * f.gridW + x1;
        const auto mix = [&](const std::vector<float>& v) {
            const float a = v[i00] + (v[i10] - v[i00]) * fx;
            const float b = v[i01] + (v[i11] - v[i01]) * fx;
            return a + (b - a) * fy;
        };
        s.dx = mix(f.vx);
        s.dy = mix(f.vy);
        s.trust = mix(f.trust);
        return s;
    }

    // u_n = u_{n-1} + (1 - trust) x cellPx + e: carried PER PIXEL OF THE CURRENT FRAME along its own chain (prev->unc[idx]); TemporalDetailModel samples
    // u_{n-1} at p + Vb(p) in the neighbour frame's grid. Both accumulate the same terms; the per-pixel chain is the same quantity without a resample.
    void ComposeStep(const Plane* prev, const PairField& ab, const PairField* ba, Plane& out) const
    {
        const size_t n = static_cast<size_t>(out.w) * out.h;
        out.dx.assign(n, 0.0f);
        out.dy.assign(n, 0.0f);
        out.trust.assign(n, 0.0f);
        out.unc.assign(n, 0.0f);
        const float tol = static_cast<float>(m_cfg.consistencyPx > 0.0 ? m_cfg.consistencyPx : 1.0);
        for (int j = 0; j < out.h; ++j) {
            for (int i = 0; i < out.w; ++i) {
                const size_t idx = static_cast<size_t>(j) * out.w + i;
                const double px = (i + 0.5) * out.step, py = (j + 0.5) * out.step;
                const float pdx = prev ? prev->dx[idx] : 0.0f, pdy = prev ? prev->dy[idx] : 0.0f;
                const float ptr = prev ? prev->trust[idx] : 1.0f, pun = prev ? prev->unc[idx] : 0.0f;
                out.dx[idx] = pdx;
                out.dy[idx] = pdy;
                out.unc[idx] = pun;
                if (ptr <= 0.0f)
                    continue;   // a dead chain stays dead (trust 0), the displacement is carried unchanged
                const double qx = px + pdx, qy = py + pdy;   // where the chain stands in frame a
                if (!(qx >= 0.0 && qy >= 0.0 && qx <= out.srcW && qy <= out.srcH))
                    continue;
                const Sampled v = SampleField(ab, qx, qy);
                const double rx = qx + v.dx, ry = qy + v.dy;
                if (!(rx >= 0.0 && ry >= 0.0 && rx <= out.srcW && ry <= out.srcH))
                    continue;
                const float cellTrust = (std::min)((std::max)(v.trust, 0.0f), 1.0f);
                float e, consistency;
                if (ba) {
                    const Sampled w = SampleField(*ba, rx, ry);
                    e = std::sqrt((v.dx + w.dx) * (v.dx + w.dx) + (v.dy + w.dy) * (v.dy + w.dy));
                    consistency = OpticalFlowTrust::FallOff(tol, 2.0f * tol, e);
                } else {
                    e = 2.0f * tol;
                    consistency = static_cast<float>(m_cfg.noReverseTrust);
                }
                out.dx[idx] = pdx + v.dx;
                out.dy[idx] = pdy + v.dy;
                out.trust[idx] = ptr * cellTrust * consistency;
                out.unc[idx] = pun + (1.0f - cellTrust) * static_cast<float>(ab.cellPx) + e;
            }
        }
    }

    const IPairSource* m_source = nullptr;
    Config m_cfg;
    int m_w = 0, m_h = 0;
    int m_step = kBlockStep;
    bool m_active[static_cast<size_t>(Reader::kCount)] = { false, false };
    Need m_need[static_cast<size_t>(Reader::kCount)][2] = {};
    int m_readerStep[static_cast<size_t>(Reader::kCount)] = { kBlockStep, kBlockStep };
    bool m_lowLatency = false;
    unsigned m_queuedAhead = 0, m_keptBehind = 0;
    std::map<Key, PlanePtr> m_planes;
    Stats m_stats;
    Reached m_last;
    bool m_haveLast = false;
};

}   // namespace VpComposed
