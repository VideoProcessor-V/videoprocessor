// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "FallbackStatus.h"
#include "Products.h"

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// B1 (refs/126 s1.2; owner rule 2026-11-12): GET-OR-COMPUTE-AND-PUBLISH, the one access path to a shared skeleton
// product. Owner: control/skeleton. Header-only, pure C++14, no Windows, no GPU API (a GPU form is an opaque handle the
// owning unit's fill creates through the skeleton's GPU dispatcher).
//
//   G1 every consumer ASKS: Acquire(id, seq, form, variant)
//   G2 the product exists for that frame (the lane's sealed slot, or a memo an earlier reader caused) -> reused
//   G3 absent -> the ONE fill registered by the OWNING unit (RegisterOwnerFill) computes it once and PUBLISHES it into the
//      per-frame memo spot under (id, seq, form, variant); every later reader of that frame reuses it. Concurrent readers of
//      the same key wait for the one fill (never a second computation).
//   G4 a variant ("my own split when the shared one does not fit") is never computed privately: the owner declares its
//      variant axes and publishes the variant under its key; an undeclared variant is refused, visibly.
//   G5 a fill that cannot run (no owner fill, no GPU dispatcher, owner declined, regulator dropped, frame released, over
//      budget, owner conflict) is AcquireStatus::Unavailable with a cause: counted per product and cause, on the
//      status lines and engaged in the Fallbacks heading (never log-only).
//
// Lifetime: memo spots live for their frame. OnSeal(seq) releases every memo of frames older than seq - keepFrames
// (keepFrames 0 = discarded when the next frame seals: the owner's per-frame memo, owner-decisions 2026-11-12 17:31).
// A request for a released frame is refused (FrameReleased), never silently recomputed. Bytes are budgeted
// (SetBudgetBytes: the VRAM ledger's grant to the memo arena user).
//
// The lane's existing seal-time live fill is the same path: SkeletonAcquireLane.h binds the lane's slots as step G2's
// first source, registers the lane's CPU live computes as the owner fills of their products (one law,
// AnalysisLane::RunCpuLive) and lets the seal reuse a frame an Acquire reader published before it.
//
// Threads: the memo itself is thread-safe; a lane-bound memo (SkeletonAcquireLane.h) reads the lane's slots, which is a
// render-thread operation, so Acquire of a LANE product is render thread. Pointers returned stay valid until the frame's
// memo is released (OnSeal / Clear), which the render thread does.
namespace VpSkeleton {

enum class AcquireForm : uint8_t { Cpu, Gpu };

// The variant axes (refs/126 s1.2). A zero field is "the product's default"; VariantKey{} is the default variant (the
// lane's form). An owner declares which axes it serves (DeclareVariantAxes); a key using another axis is refused.
struct VariantKey {
    uint8_t plane = 0;         // 0 = default plane (luma); 1.. = plane index + 1
    uint32_t width = 0;        // 0 = the product's own size
    uint32_t height = 0;
    uint8_t scale = 0;         // GrainScale / pyramid level, 0 = default
    uint8_t codeUnit = 0;      // code-unit form (bits), 0 = default
    uint32_t sigmaMilli = 0;   // band sigma x 1000, 0 = default
    uint8_t precision = 0;     // storage precision (e.g. 16 = fp16, 32 = fp32), 0 = default

    bool IsDefault() const { return Axes() == 0u; }
    uint32_t Axes() const
    {
        return (plane ? uint32_t(kAxisPlane) : 0u) | (width || height ? uint32_t(kAxisSize) : 0u) | (scale ? uint32_t(kAxisScale) : 0u)
               | (codeUnit ? uint32_t(kAxisCodeUnit) : 0u) | (sigmaMilli ? uint32_t(kAxisSigma) : 0u)
               | (precision ? uint32_t(kAxisPrecision) : 0u);
    }
    std::string Text() const
    {
        if (IsDefault())
            return "default";
        std::string t;
        auto add = [&t](const char* k, uint64_t v) { t += (t.empty() ? "" : ",") + std::string(k) + "=" + std::to_string(v); };
        if (plane) add("plane", plane);
        if (width || height) add("w", width), add("h", height);
        if (scale) add("scale", scale);
        if (codeUnit) add("code", codeUnit);
        if (sigmaMilli) add("sigma_milli", sigmaMilli);
        if (precision) add("precision", precision);
        return t;
    }

    enum : uint32_t {
        kAxisPlane = 1u << 0,
        kAxisSize = 1u << 1,
        kAxisScale = 1u << 2,
        kAxisCodeUnit = 1u << 3,
        kAxisSigma = 1u << 4,
        kAxisPrecision = 1u << 5
    };
};

inline bool operator==(const VariantKey& a, const VariantKey& b)
{
    return a.plane == b.plane && a.width == b.width && a.height == b.height && a.scale == b.scale && a.codeUnit == b.codeUnit
           && a.sigmaMilli == b.sigmaMilli && a.precision == b.precision;
}
inline bool operator!=(const VariantKey& a, const VariantKey& b) { return !(a == b); }
inline bool operator<(const VariantKey& a, const VariantKey& b)
{
    return std::make_tuple(a.plane, a.width, a.height, a.scale, a.codeUnit, a.sigmaMilli, a.precision)
           < std::make_tuple(b.plane, b.width, b.height, b.scale, b.codeUnit, b.sigmaMilli, b.precision);
}

// A GPU form: an opaque resource of the skeleton's GPU dispatcher (texture / buffer / view), the fence value after which
// it is readable, and a keep-alive the memo holds until the frame is released (the resource goes back to the arena then).
struct GpuMemoHandle {
    void* resource = nullptr;
    void* view = nullptr;
    uint64_t fence = 0;
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t bytes = 0;                  // budgeted against the memo grant
    std::shared_ptr<void> keepAlive;
};

enum class AcquireStatus : uint8_t {
    Reused,         // G2: the lane's sealed / live-filled slot, or a memo published earlier for this frame
    FilledAtRead,   // G3: absent; the owner's fill ran now (once) and published it
    Unavailable     // G5: see the cause
};

enum class AcquireCause : uint8_t {
    None,
    NoOwnerFill,         // nobody owns a fill for (product, form)
    NoGpuDispatcher,     // a GPU form with no skeleton GPU dispatcher (the SkeletonModule no-dispatcher wiring)
    OwnerDeclined,       // the owner's fill returned false (input missing, VRAM refused ...)
    RegulatorDropped,    // the regulator shed the product (optional fill)
    FrameReleased,       // the frame's memo was already released (lifetime = frame): never recomputed
    VariantUndeclared,   // the owner does not serve that variant axis (G4)
    OverBudget,          // the memo grant cannot hold the result
    LanePending,         // the lane's GPU live fill is recorded but not run yet (deferred to Entrance)
    LaneFailed,          // the lane's deferred GPU live fill failed
    CauseCount
};

inline const char* Token(AcquireStatus s)
{
    switch (s) {
    case AcquireStatus::Reused: return "reused";
    case AcquireStatus::FilledAtRead: return "filled at read";
    case AcquireStatus::Unavailable: return "unavailable";
    }
    return "?";
}

inline const char* Token(AcquireCause c)
{
    switch (c) {
    case AcquireCause::None: return "none";
    case AcquireCause::NoOwnerFill: return "no owner fill";
    case AcquireCause::NoGpuDispatcher: return "no GPU dispatcher";
    case AcquireCause::OwnerDeclined: return "owner fill declined";
    case AcquireCause::RegulatorDropped: return "dropped by the regulator";
    case AcquireCause::FrameReleased: return "frame released";
    case AcquireCause::VariantUndeclared: return "variant not served by the owner";
    case AcquireCause::OverBudget: return "memo budget";
    case AcquireCause::LanePending: return "lane live fill pending";
    case AcquireCause::LaneFailed: return "lane live fill failed";
    case AcquireCause::CauseCount: break;
    }
    return "?";
}

enum class AcquireSource : uint8_t { None, LaneSlot, Memo };

struct AcquireResult {
    AcquireStatus status = AcquireStatus::Unavailable;
    AcquireCause cause = AcquireCause::None;
    AcquireSource source = AcquireSource::None;
    const uint8_t* cpu = nullptr;           // CPU form (valid until the frame's memo is released / the lane slot reused)
    size_t cpuBytes = 0;
    const GpuMemoHandle* gpu = nullptr;     // GPU form
    bool Ok() const { return status != AcquireStatus::Unavailable; }
};

// What the owner's fill is asked for.
struct OwnerFillRequest {
    ProductId id = kNoProduct;
    uint64_t seq = 0;
    AcquireForm form = AcquireForm::Cpu;
    VariantKey variant;
};

// The owning unit's fill. `cpu` writes the CPU form into `out` (sized by the fill); `gpu` creates the GPU form through
// the skeleton's dispatcher. `acquireInput` lets a fill get-or-compute its own inputs through the same path. A fill
// returns false to decline (visible as OwnerDeclined).
class SkeletonMemo;
struct OwnerFill {
    std::string owner;   // the owning unit (folder token, e.g. "control/skeleton", "input/source_analysis"); N1 checks the call site
    std::function<bool(const OwnerFillRequest&, SkeletonMemo&, std::vector<uint8_t>& out)> cpu;
    std::function<bool(const OwnerFillRequest&, SkeletonMemo&, GpuMemoHandle& out)> gpu;
    uint32_t variantAxes = 0;   // the VariantKey axes this owner serves (0 = the default variant only)
};

// The lane's slots as the first G2 source (SkeletonAcquireLane.h binds it). Default variant only.
struct LaneSlotSource {
    enum class State : uint8_t { NotLaneProduct, Absent, Present, Pending, Failed };
    // (id, seq, form) -> state; Present fills cpu/cpuBytes for a CPU product. A GPU lane product that is Present is Reused
    // with no handle (gpu null): its texture is the lane's slot, read through the dispatcher's existing view of it.
    std::function<State(ProductId, uint64_t, AcquireForm, const uint8_t**, size_t*)> read;
    std::function<bool(ProductId)> dropped;   // regulator-dropped products
};

struct AcquireCounters {
    uint64_t reusedLane = 0;
    uint64_t reusedMemo = 0;
    uint64_t filledAtRead = 0;
    uint64_t ownerFillCalls = 0;   // == filledAtRead + declined fills: proves "computed once"
    uint64_t released = 0;
    uint64_t unavailable[size_t(AcquireCause::CauseCount)] = {};
    uint64_t Unavailable() const
    {
        uint64_t t = 0;
        for (uint64_t u : unavailable)
            t += u;
        return t;
    }
};

class SkeletonMemo {
public:
    static constexpr const char* kFallbackPrefix = "skeleton acquire ";

    // ---- setup ----

    // G3: the ONE owner fill of product `id`. A second owner for the same product is refused (false) and counted
    // (OwnerConflicts, on the status lines); the same owner re-registering replaces its fill (graph rebuild).
    bool RegisterOwnerFill(ProductId id, OwnerFill fill)
    {
        std::lock_guard<std::mutex> g(mutex_);
        const auto it = owners_.find(id);
        if (it != owners_.end() && it->second.owner != fill.owner) {
            ++ownerConflicts_;
            conflictText_ = NameOfLocked(id) + ": owned by " + it->second.owner + ", refused " + fill.owner;
            return false;
        }
        owners_[id] = std::move(fill);
        return true;
    }
    bool HasOwnerFill(ProductId id) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return owners_.count(id) != 0;
    }
    std::string OwnerOf(ProductId id) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        const auto it = owners_.find(id);
        return it == owners_.end() ? std::string() : it->second.owner;
    }
    void SetLaneSource(LaneSlotSource src)
    {
        std::lock_guard<std::mutex> g(mutex_);
        lane_ = std::move(src);
    }
    // The skeleton GPU dispatcher exists (SkeletonModule wiring). Without it every GPU form declines: NoGpuDispatcher.
    void SetGpuDispatcherPresent(bool present)
    {
        std::lock_guard<std::mutex> g(mutex_);
        gpuPresent_ = present;
    }
    void SetKeepFrames(uint32_t frames)
    {
        std::lock_guard<std::mutex> g(mutex_);
        keepFrames_ = frames;
    }
    // 0 = unbounded (tests); the host passes the VRAM ledger's memo grant.
    void SetBudgetBytes(uint64_t bytes)
    {
        std::lock_guard<std::mutex> g(mutex_);
        budgetBytes_ = bytes;
    }
    // Product names for the status / Fallbacks text (the registry's tokens).
    void SetNames(const Registry* reg)
    {
        std::lock_guard<std::mutex> g(mutex_);
        names_.clear();
        if (reg != nullptr)
            for (const ProductDecl& d : reg->All())
                if (d.token != nullptr && d.token[0] != 0)
                    names_[d.id] = d.token;
    }
    // Unavailable results also engage `<kFallbackPrefix><product>` in this registry (the component's Fallbacks heading);
    // a later success of the same product releases it.
    void SetFallbackSink(FallbackStatus::Registry* sink)
    {
        std::lock_guard<std::mutex> g(mutex_);
        sink_ = sink;
    }

    // ---- G1-G5 ----

    AcquireResult Acquire(ProductId id, uint64_t seq, AcquireForm form, const VariantKey& v = VariantKey())
    {
        const Key key{ id, seq, uint8_t(form), v };
        std::unique_lock<std::mutex> lk(mutex_);
        AcquireCounters& c = counters_[id];

        // G2 (a): the lane's slot (default variant).
        if (v.IsDefault() && lane_.read) {
            const uint8_t* p = nullptr;
            size_t n = 0;
            const LaneSlotSource::State s = lane_.read(id, seq, form, &p, &n);
            if (s == LaneSlotSource::State::Present) {
                ++c.reusedLane;
                AcquireResult r;
                r.status = AcquireStatus::Reused;
                r.source = AcquireSource::LaneSlot;
                r.cpu = p;
                r.cpuBytes = n;
                ReleaseFallbackLocked(lk, id);
                return r;
            }
            if (s == LaneSlotSource::State::Pending)
                return Fail(lk, c, id, seq, AcquireCause::LanePending);
            if (s == LaneSlotSource::State::Failed)
                return Fail(lk, c, id, seq, AcquireCause::LaneFailed);
        }

        // A frame whose memo was released is never recomputed (its lane slots, above, stay readable while the lane holds them).
        if (haveReleased_ && seq <= releasedThrough_)
            return Fail(lk, c, id, seq, AcquireCause::FrameReleased);

        // G2 (b): the memo spot (wait for a fill in flight: computed once).
        for (;;) {
            const auto it = memo_.find(key);
            if (it == memo_.end())
                break;
            if (it->second->state == Entry::Filling) {
                cv_.wait(lk);
                continue;
            }
            if (it->second->state == Entry::Ready) {
                ++c.reusedMemo;
                return ResultOf(*it->second, AcquireStatus::Reused);
            }
            // Declined for this frame: the same cause again (the owner is not asked twice for one frame).
            return Fail(lk, c, id, seq, it->second->cause);
        }

        if (lane_.dropped && lane_.dropped(id))
            return Fail(lk, c, id, seq, AcquireCause::RegulatorDropped);
        const auto ow = owners_.find(id);
        if (ow == owners_.end() || (form == AcquireForm::Cpu ? !ow->second.cpu : !ow->second.gpu))
            return Fail(lk, c, id, seq, AcquireCause::NoOwnerFill);
        if ((v.Axes() & ~ow->second.variantAxes) != 0u)
            return Fail(lk, c, id, seq, AcquireCause::VariantUndeclared);
        if (form == AcquireForm::Gpu && !gpuPresent_)
            return Fail(lk, c, id, seq, AcquireCause::NoGpuDispatcher);

        // G3: the owner fills it once, outside the lock (other keys proceed; readers of this key wait).
        std::shared_ptr<Entry> e = std::make_shared<Entry>();
        e->state = Entry::Filling;
        e->seq = seq;
        memo_[key] = e;
        ++c.ownerFillCalls;
        const OwnerFill fill = ow->second;   // a copy: a re-registration during the fill does not invalidate it
        lk.unlock();
        OwnerFillRequest req;
        req.id = id;
        req.seq = seq;
        req.form = form;
        req.variant = v;
        bool ok = false;
        std::vector<uint8_t> bytes;
        GpuMemoHandle gpu;
        if (form == AcquireForm::Cpu)
            ok = fill.cpu(req, *this, bytes);
        else
            ok = fill.gpu(req, *this, gpu);
        lk.lock();
        AcquireCounters& c2 = counters_[id];
        const uint64_t need = form == AcquireForm::Cpu ? uint64_t(bytes.size()) : gpu.bytes;
        AcquireCause cause = AcquireCause::None;
        if (!ok)
            cause = AcquireCause::OwnerDeclined;
        else if (budgetBytes_ != 0 && liveBytes_ + need > budgetBytes_)
            cause = AcquireCause::OverBudget;
        if (cause != AcquireCause::None) {
            e->state = Entry::Declined;
            e->cause = cause;
            cv_.notify_all();
            return Fail(lk, c2, id, seq, cause);
        }
        e->bytes = std::move(bytes);
        e->gpu = std::move(gpu);
        e->budgeted = need;
        liveBytes_ += need;
        e->state = Entry::Ready;
        ++c2.filledAtRead;
        cv_.notify_all();
        ReleaseFallbackLocked(lk, id);
        return ResultOf(*e, AcquireStatus::FilledAtRead);
    }

    // The seal's question (SkeletonAcquireLane.h): did a reader publish the default CPU form of (id, seq)? Copies it.
    bool CopyPublished(ProductId id, uint64_t seq, uint8_t* dst, size_t n) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        const auto it = memo_.find(Key{ id, seq, uint8_t(AcquireForm::Cpu), VariantKey() });
        if (it == memo_.end() || it->second->state != Entry::Ready || it->second->bytes.size() != n || dst == nullptr)
            return false;
        std::memcpy(dst, it->second->bytes.data(), n);
        return true;
    }

    // Lifetime: frame `seq` is sealed; memos of frames <= seq - 1 - keepFrames are released (their bytes / GPU keep-alives
    // go back to the arena). Render thread.
    void OnSeal(uint64_t seq)
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (haveSealed_ && seq < lastSeal_)
            ClearLocked();   // the frame numbers went back (a new epoch): nothing of the old numbering is kept or refused
        haveSealed_ = true;
        lastSeal_ = seq;
        if (seq < 1u + keepFrames_)
            return;
        const uint64_t through = seq - 1u - keepFrames_;
        for (auto it = memo_.begin(); it != memo_.end();) {
            if (it->second->state != Entry::Filling && it->second->seq <= through) {
                liveBytes_ -= it->second->budgeted;
                if (it->second->state == Entry::Ready)
                    ++counters_[it->first.id].released;
                it = memo_.erase(it);
            } else {
                ++it;
            }
        }
        if (!haveReleased_ || through > releasedThrough_)
            releasedThrough_ = through;
        haveReleased_ = true;
    }
    // A discontinuity (graph rebuild, resync): every memo released; frames are accepted again from any seq.
    void Clear()
    {
        std::lock_guard<std::mutex> g(mutex_);
        ClearLocked();
        haveSealed_ = false;
    }

    // ---- diagnostics ----

    AcquireCounters CountersOf(ProductId id) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        const auto it = counters_.find(id);
        return it == counters_.end() ? AcquireCounters() : it->second;
    }
    size_t MemoCount() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return memo_.size();
    }
    uint64_t MemoBytes() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return liveBytes_;
    }
    uint64_t OwnerConflicts() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return ownerConflicts_;
    }
    // One line per product asked: "skeleton acquire: p1_half_luma 12 reused (lane 10, memo 2), 1 filled at read, 0 unavailable".
    std::vector<std::string> StatusLines() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        std::vector<std::string> lines;
        for (const auto& kv : counters_) {
            const AcquireCounters& c = kv.second;
            std::string l = "skeleton acquire: " + NameOfLocked(kv.first) + " " + std::to_string(c.reusedLane + c.reusedMemo)
                            + " reused (lane " + std::to_string(c.reusedLane) + ", memo " + std::to_string(c.reusedMemo) + "), "
                            + std::to_string(c.filledAtRead) + " filled at read, " + std::to_string(c.Unavailable())
                            + " unavailable";
            for (size_t k = 1; k < size_t(AcquireCause::CauseCount); ++k)
                if (c.unavailable[k] != 0)
                    l += std::string("; ") + Token(AcquireCause(k)) + " " + std::to_string(c.unavailable[k]);
            lines.push_back(l);
        }
        if (ownerConflicts_ != 0)
            lines.push_back("skeleton acquire: owner conflicts " + std::to_string(ownerConflicts_) + " (" + conflictText_ + ")");
        return lines;
    }
    // The last Unavailable cause text per product (what the Fallbacks heading shows).
    std::string LastCause(ProductId id) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        const auto it = lastCause_.find(id);
        return it == lastCause_.end() ? std::string() : it->second;
    }

private:
    struct Key {
        ProductId id;
        uint64_t seq;
        uint8_t form;
        VariantKey variant;
        bool operator<(const Key& o) const
        {
            if (id != o.id) return id < o.id;
            if (seq != o.seq) return seq < o.seq;
            if (form != o.form) return form < o.form;
            return variant < o.variant;
        }
    };
    struct Entry {
        enum State : uint8_t { Filling, Ready, Declined } state = Filling;
        AcquireCause cause = AcquireCause::None;
        uint64_t seq = 0;
        std::vector<uint8_t> bytes;
        GpuMemoHandle gpu;
        uint64_t budgeted = 0;
    };

    void ClearLocked()
    {
        for (auto it = memo_.begin(); it != memo_.end();) {
            if (it->second->state == Entry::Filling) {
                ++it;
                continue;
            }
            liveBytes_ -= it->second->budgeted;
            it = memo_.erase(it);
        }
        haveReleased_ = false;
        releasedThrough_ = 0;
    }

    static AcquireResult ResultOf(const Entry& e, AcquireStatus st)
    {
        AcquireResult r;
        r.status = st;
        r.source = AcquireSource::Memo;
        if (!e.bytes.empty()) {
            r.cpu = e.bytes.data();
            r.cpuBytes = e.bytes.size();
        }
        if (e.gpu.resource != nullptr || e.gpu.view != nullptr)
            r.gpu = &e.gpu;
        return r;
    }

    std::string NameOfLocked(ProductId id) const
    {
        const auto it = names_.find(id);
        return it != names_.end() ? it->second : "product " + std::to_string(id);
    }

    AcquireResult Fail(std::unique_lock<std::mutex>& lk, AcquireCounters& c, ProductId id, uint64_t seq, AcquireCause cause)
    {
        ++c.unavailable[size_t(cause)];
        const std::string reason = std::string(Token(cause)) + ", frame " + std::to_string(seq);
        lastCause_[id] = reason;
        FallbackStatus::Registry* sink = sink_;
        const std::string fid = kFallbackPrefix + NameOfLocked(id);
        lk.unlock();
        if (sink != nullptr)
            sink->Engage(fid, Token(cause));   // the cause (not the frame) so the heading does not churn per frame
        lk.lock();
        if (sink != nullptr)
            engaged_[id] = true;
        AcquireResult r;
        r.status = AcquireStatus::Unavailable;
        r.cause = cause;
        return r;
    }

    void ReleaseFallbackLocked(std::unique_lock<std::mutex>& lk, ProductId id)
    {
        const auto it = engaged_.find(id);
        if (sink_ == nullptr || it == engaged_.end())
            return;
        engaged_.erase(it);
        FallbackStatus::Registry* sink = sink_;
        const std::string fid = kFallbackPrefix + NameOfLocked(id);
        lk.unlock();
        sink->Release(fid);
        lk.lock();
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<ProductId, OwnerFill> owners_;
    std::map<Key, std::shared_ptr<Entry>> memo_;
    std::map<ProductId, AcquireCounters> counters_;
    std::map<ProductId, std::string> names_;
    std::map<ProductId, std::string> lastCause_;
    std::map<ProductId, bool> engaged_;   // products whose Fallbacks entry is engaged in sink_
    LaneSlotSource lane_;
    FallbackStatus::Registry* sink_ = nullptr;
    bool gpuPresent_ = false;
    uint32_t keepFrames_ = 0;
    uint64_t budgetBytes_ = 0;
    uint64_t liveBytes_ = 0;
    uint64_t ownerConflicts_ = 0;
    std::string conflictText_;
    bool haveReleased_ = false;
    uint64_t releasedThrough_ = 0;
    bool haveSealed_ = false;
    uint64_t lastSeal_ = 0;
};

}  // namespace VpSkeleton
