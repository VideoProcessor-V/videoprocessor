// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "FallbackCatalog.h"
#include "GpuAllocator.h"
#include "ProductCatalog.h"
#include "Products.h"
#include "QueueWindow.h"
#include "VariantKey.h"

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// B1 (refs/126 s1.2; owner rule 2026-11-12): GET-OR-COMPUTE-AND-PUBLISH, the one access path to a shared skeleton
// product. Owner: control/skeleton. Header-only, pure C++14, no Windows, no GPU API (a GPU form is an opaque handle the
// owning unit's fill creates through the skeleton's GPU dispatcher). Contract: modules/control/skeleton/README.md "Acquire".
//
//   G1 every consumer ASKS: Acquire(id, seq, form, variant)
//   G2 the product exists for that frame (the lane's sealed / live-filled slot, or a memo an earlier reader caused) -> reused
//   G3 absent -> the ONE fill registered for (product, form) by the OWNING unit (the catalog's unit, ProductRows) computes it
//      once and PUBLISHES it into the per-frame memo spot under (id, seq, form, canonical variant); every later reader of
//      that frame reuses it. Concurrent readers of one key wait for the one fill (never a second computation).
//   G4 a variant ("my own split when the shared one does not fit") is never computed privately: the owner declares its
//      variant axes and publishes the variant under its key; an undeclared axis is refused, visibly.
//   G5 a fill that cannot run is AcquireStatus::Unavailable with a cause: counted per product and cause, on the status lines
//      and engaged as the Fallbacks entry "skeleton acquire <product token>" (Decisions(): the Ctrl+I / Config status /
//      Ctrl+Alt+D path), released when the product is served again. Never log-only.
//
// Lifetime: memo spots live for their frame. OnSeal(seq) releases every memo of frames older than seq - keepFrames
// (keepFrames 0 = discarded when the next frame seals: the owner's per-frame memo, owner-decisions 2026-11-12 17:31).
// QUEUE-KEYED (lead10/design-queue-keyed-products.md, owner 2026-11-14 08:33 EST): with a QueueWindow (SetWindow) a spot stays
// attached to ITS frame until the frame has left the queue (f < min(seal - back, presented - history)); a fill beyond
// current + ahead is NotQueued, beyond the planned ahead depth NotPlannedAhead (SetAheadPlan); a dropped frame is Discard()ed, a
// discontinuity Invalidate()s everything (owner Q1 a). Nothing attached to a queued frame is ever evicted to make room (owner Q2).
// A request for a released frame is refused (FrameReleased), never silently recomputed; "released" is per owner (a frame inside
// an owner's DeclaredHistory() window that nobody filled yet fills once at read). A result carries a PIN: the bytes /
// handle stay valid while the reader holds it, even past OnSeal. A GPU spot whose recorded read fence (NoteGpuRead) is not
// complete yet (SetFenceCompleted) is retired only when it completes: its keep-alive is not dropped under a GPU read.
// GPU bytes are budgeted against the VRAM ledger grant (SetGpuBudget; reserved BEFORE the fill); CPU bytes are counted.
//
// Threads: the memo is thread-safe and fills run outside its lock (NO lock is held across an owner fill, so a fill may
// Acquire its prerequisites: nested get-or-compute-and-publish, owner rule 2026-11-13). The re-entrancy guard is PER KEY
// and cycles are detected BY KEY: a fill asking (directly or through other fills, on this thread or across threads that
// wait for each other's fills) for a key whose fill is waiting on it is refused as Reentrant, never a deadlock. The per-key
// guard is per memo instance (a fill of memo A asking memo B for the same key is not a cycle). A fill must NOT hand a nested
// Acquire to another thread and wait for it (a pool worker is not in the wait graph: that deadlock would not be detected).
// L-CORE thread contract (lead8/iface-L-CORE.md s5, OD 11-14 04:26 (a)): the render thread is declared by the skeleton
// (SetRenderThread, every Begin; none declared = every thread counts as the render thread, the B1 behaviour). CPU forms are
// Acquire-able from ANY thread and pinned; a lane slot read is render-thread state, so off the render thread a lane CPU
// product is served from its PUBLISHED bytes (copied out through the lane lease, counted reusedLaneCopy, never recomputed);
// a GPU form and a render-thread-bound fill (OwnerFill::renderThreadOnly: the lane's CPU live computes) asked off the render
// thread are Unavailable(WrongThread), visible. The memo exists per renderer with or without the analysis lane (the skeleton
// module holds it); a lane BINDS it (BindLane) and unbinds by token (UnbindLane): the module registrations stay.
namespace VpSkeleton {

enum class AcquireForm : uint8_t { Cpu, Gpu };

inline const char* Token(AcquireForm f) { return f == AcquireForm::Cpu ? "cpu" : "gpu"; }

// VariantKey and the precision axis values: core/VariantKey.h (split out so core/QueueWindow.h can name a variant).

// A GPU form: an opaque resource of the skeleton's GPU dispatcher (texture / buffer / view), the fence value after which it
// is readable, and a keep-alive the memo holds until the frame is released AND its last recorded read fence completed.
// A lane GPU product's handle names the lane slot (textureSet + ring/live + index) of the dispatcher's product textures.
struct GpuMemoHandle {
    void* resource = nullptr;
    void* view = nullptr;
    const void* textureSet = nullptr;   // lane products: the dispatcher's texture set of the product
    int32_t index = -1;                 // lane products: ring slot or live buffer
    bool ringSlot = false;
    uint64_t fence = 0;                 // readable after this producer fence value
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t bytes = 0;                 // budgeted against the ledger grant
    std::shared_ptr<void> keepAlive;
    bool Valid() const { return resource != nullptr || view != nullptr; }
};

// R2 (lead 12): THE sanctioned GPU context of a non-view GPU owner fill. A fill that makes something on the GPU (not a VIEW of another
// unit's texture) gets the render device, the dispatch and texture allocation ONLY through OwnerFillRequest::gpu, never by capturing
// a pl_gpu of its own. The device side (IOwnerGpuDevice) is the skeleton GPU dispatcher (SkeletonGpuDispatcher; libplacebo stays
// out of core: the handles are opaque here). Every texture the fill allocates is accounted against the fill's reserve
// (OwnerFill::reserveBytes, itself reserved against the VRAM ledger grant BEFORE the fill): an allocation beyond it is refused,
// counted (AcquireCounters::gpuAllocRefused) and worded (the fill's decline reason). The context is valid for the fill call only:
// it is closed when the fill returns (Gpu() / Dispatch() null, Allocate refused), so a kept pointer gets nothing.
class IOwnerGpuDevice {
public:
    virtual ~IOwnerGpuDevice() {}
    virtual void* OwnerGpu() const = 0;        // the render pl_gpu (opaque in core)
    virtual void* OwnerDispatch() const = 0;   // the dispatcher's pl_dispatch (opaque in core)
    // One sampleable + storable texture of `shape`: out.resource = the pl_tex, out.width / height / format / bytes (ShapeBytes) set,
    // out.keepAlive destroys it at the device's next GPU-legal phase (or forgets it when the device died). False: `why`.
    virtual bool CreateOwnerTexture(const TexShape& shape, GpuMemoHandle& out, std::string& why) = 0;
};

class OwnerGpuContext {
public:
    OwnerGpuContext(IOwnerGpuDevice* device, uint64_t grantBytes) : device_(device), grant_(grantBytes) {}
    OwnerGpuContext(const OwnerGpuContext&) = delete;
    OwnerGpuContext& operator=(const OwnerGpuContext&) = delete;

    void* Gpu() const { return open_ && device_ != nullptr ? device_->OwnerGpu() : nullptr; }
    void* Dispatch() const { return open_ && device_ != nullptr ? device_->OwnerDispatch() : nullptr; }
    // A texture of `shape`, accounted against the grant. False (nothing allocated): closed, no device, over the grant or refused by
    // the device; `why` says which.
    bool Allocate(const TexShape& shape, GpuMemoHandle& out, std::string& why)
    {
        out = GpuMemoHandle();
        const uint64_t bytes = ShapeBytes(shape);
        if (!open_) {
            why = "the GPU context is used after its fill returned";
        } else if (device_ == nullptr) {
            why = "no skeleton GPU device";
        } else if (bytes == 0) {
            why = "an empty texture shape";
        } else if (used_ + bytes > grant_) {
            why = "a texture of " + std::to_string(bytes) + " bytes would pass the fill's reserve (" + std::to_string(used_) + " of "
                  + std::to_string(grant_) + " used; OwnerFill::reserveBytes)";
        } else if (device_->CreateOwnerTexture(shape, out, why)) {
            out.bytes = bytes;
            used_ += bytes;
            ++made_;
            return true;
        }
        out = GpuMemoHandle();
        ++refused_;
        lastRefusal_ = why;
        return false;
    }
    uint64_t Granted() const { return grant_; }
    uint64_t Used() const { return used_; }
    uint64_t Made() const { return made_; }
    uint64_t Refused() const { return refused_; }
    const std::string& LastRefusal() const { return lastRefusal_; }
    void Close() { open_ = false; }

private:
    IOwnerGpuDevice* device_ = nullptr;
    uint64_t grant_ = 0;
    uint64_t used_ = 0;
    uint64_t made_ = 0;
    uint64_t refused_ = 0;
    std::string lastRefusal_;
    bool open_ = true;
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
    OwnerDeclined,       // the owner's fill returned false or threw (input missing, VRAM refused ...)
    RegulatorDropped,    // the regulator shed the product (optional fill)
    FrameReleased,       // the frame's memo was already released (lifetime = frame): never recomputed
    VariantUndeclared,   // the owner does not serve that variant axis (G4)
    OverBudget,          // the ledger grant cannot hold the result
    NoLedgerGrant,       // a GPU spot with no VRAM ledger grant for the memo (nothing may be allocated outside the ledger)
    LanePending,         // the lane's GPU live fill is recorded but not run yet (deferred to Entrance)
    LaneFailed,          // the lane's deferred GPU live fill failed
    NoGpuHandle,         // the lane holds the GPU product but its texture is not reachable (dispatcher not configured)
    Reentrant,           // an owner fill asked for its own key, directly or through a cycle of fills (would wait for itself)
    WrongThread,         // a GPU form, a render-thread-bound fill or a lane slot read asked off the render thread (L-CORE s5)
    NotQueued,           // the frame is beyond the queue window (seq > current + ahead): not a fill (design s2.3)
    NotPlannedAhead,     // the frame is beyond the product's planned ahead depth (SetAheadPlan; design s2.7): made at its render
    PresentsStalled,     // presents stopped advancing past the window (design s9.0 A3 / audit S3): ahead demand stops, nothing evicted
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
    case AcquireCause::OverBudget: return "VRAM ledger grant exceeded";
    case AcquireCause::NoLedgerGrant: return "no VRAM ledger grant";
    case AcquireCause::LanePending: return "lane live fill pending";
    case AcquireCause::LaneFailed: return "lane live fill failed";
    case AcquireCause::NoGpuHandle: return "lane GPU texture not reachable";
    case AcquireCause::Reentrant: return "owner fill asked for its own product";
    case AcquireCause::WrongThread: return "asked off the render thread";
    case AcquireCause::NotQueued: return "frame not in the queue yet";
    case AcquireCause::NotPlannedAhead: return "not planned this far ahead (made at its render)";
    case AcquireCause::PresentsStalled: return "presents stalled: no work ahead until they move";
    case AcquireCause::CauseCount: break;
    }
    return "?";
}

enum class AcquireSource : uint8_t { None, LaneSlot, Memo };

struct AcquireResult {
    AcquireStatus status = AcquireStatus::Unavailable;
    AcquireCause cause = AcquireCause::None;
    AcquireSource source = AcquireSource::None;
    const uint8_t* cpu = nullptr;           // CPU form: valid while `pin` is held
    size_t cpuBytes = 0;
    GpuMemoHandle gpu;                      // GPU form (gpu.Valid())
    std::shared_ptr<const void> pin;        // the memo spot or the lane state: what keeps `cpu` / `gpu` alive
    bool Ok() const { return status != AcquireStatus::Unavailable; }
};

// What the owner's fill is asked for.
struct OwnerFillRequest {
    const void* host = nullptr;   // the renderer whose memo asks (SkeletonMemo::SetHost; null in a standalone memo)
    ProductId id = kNoProduct;
    uint64_t seq = 0;
    AcquireForm form = AcquireForm::Cpu;
    VariantKey variant;   // canonical
    // R2: the sanctioned GPU context, set ONLY for a non-view GPU fill (null for CPU and view fills). Valid during the call only.
    OwnerGpuContext* gpu = nullptr;
};
// What it produces: the CPU form (`bytes`) or the GPU form (`gpu`).
struct OwnerFillOutput {
    std::vector<uint8_t> bytes;
    GpuMemoHandle gpu;
    std::string declineReason;   // a declining fill's reason: shown with the cause (LastCause, Fallbacks entry)
    // PER-RESULT lifetime (design s2.3): Frame = a one-render-frame VIEW (released at the next seal, e.g. P13 of the render
    // frame); Window = kept with its frame in the queue (an owned ahead texture); FillDefault = OwnerFill::frameScoped.
    enum class Scope : uint8_t { FillDefault, Frame, Window } scope = Scope::FillDefault;
};

// The owning unit's fill of ONE (product, form). `memo` lets a fill get-or-compute its inputs through the same path.
// False (or an exception) = declined (visible as OwnerDeclined).
class SkeletonMemo;
struct OwnerFill {
    std::function<bool(const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput&)> fill;
    uint32_t variantAxes = 0;    // the VariantKey axes this owner serves (0 = the default variant only)
    VariantKey defaults;         // the owner's own form (canonicalisation); defaults.stage must be 0 (the source planes)
    uint64_t reserveBytes = 0;   // GPU: bytes reserved against the ledger grant BEFORE the fill (a larger result re-checks)
    // L-CORE (iface-L-CORE s5): the thread contract. A CPU fill runs on the asking thread (any thread) unless it touches render
    // state (renderThreadOnly: the lane's CPU live computes); a GPU fill is ALWAYS render thread.
    bool renderThreadOnly = false;
    // Frame-scoped: the spot is a VIEW valid for one render frame (P13 plane views): released at the NEXT OnSeal whatever
    // keepFrames is.
    bool frameScoped = false;
    // A GPU VIEW of something another unit owns (0 bytes, no texture made): needs neither the skeleton GPU dispatcher nor a
    // ledger grant (nothing is allocated). Its result must carry gpu.bytes == 0.
    bool view = false;
    // The fill publishes PIXEL values: a Float16 precision request is refused (VariantUndeclared; 12-bit rule).
    bool pixelData = false;
    // Frames its spots are KEPT after their own frame (max with SetKeepFrames): 1 = frame n-1 stays readable while n renders
    // (P1 GPU: the matcher's A is the previous frame's B, iface-L-B4). 0 = the owner's per-frame memo (OD 11-12 17:31).
    uint8_t keepFrames = 0;
    // Queue-keyed products (design s2.3): the DECLARED reader lag of this product's consumers (P1 GPU: 1 = frame n - 1 is read
    // while n renders). An input of the window's history term H (the skeleton folds it in); the memo keeps a spot at least this
    // many frames behind the release edge. keepFrames above is the same declaration under its B1 name (the larger one counts).
    uint8_t historyFrames = 0;
    uint8_t DeclaredHistory() const { return historyFrames > keepFrames ? historyFrames : keepFrames; }
    // The S3 admission link (design s2.7): the class of this product's ahead work (shed order) and the ahead depth its consumers
    // declared (a Core product keeps at least this much when memory is short).
    JobClass jobClass = JobClass::Core;
    uint16_t aheadDepth = 0;
};

// The lane's slots as the first G2 source (SkeletonAcquireLane.h binds it). Default variant only.
struct LaneSlotSource {
    enum class State : uint8_t { NotLaneProduct, Absent, Present, Pending, Failed, NoHandle };
    struct Read {
        const uint8_t* cpu = nullptr;
        size_t cpuBytes = 0;
        GpuMemoHandle gpu;
        std::shared_ptr<const void> pin;
    };
    std::function<State(ProductId, uint64_t, AcquireForm, Read*)> read;   // render thread
    std::function<bool(ProductId)> dropped;   // regulator-dropped products (render thread)
    // Off the render thread (L-CORE s5): the CPU form's PUBLISHED bytes copied out through the lane's lease (never a slot
    // pointer: a ring slot is recycled by the render thread). Present = `copy` holds them; NotLaneProduct / Absent otherwise.
    std::function<State(ProductId, uint64_t, std::vector<uint8_t>* copy)> copyPublished;
    // QKP slice C (R14-73): the render thread asks the lane to make (product, frame) NOW, into the lane's slot for that frame,
    // for a frame AHEAD of the sealed render frame (the memo then reuses the lane's slot instead of making a second copy).
    // Ran = the frame is Published in the lane slot (made now, or already made); Claimed = a worker is writing it right now
    // (LanePending, visible, no wait); NotNode = not a lane product: the memo's own owner fill path; Declined = visible, `why`
    // names it. Mirrors AnalysisLane::AheadRun (the binder maps it); the memo never includes the lane header.
    enum class AheadNow : uint8_t { Ran, Claimed, NotNode, Declined };
    struct AheadNowResult {
        AheadNow kind = AheadNow::NotNode;
        const char* why = "";
    };
    std::function<AheadNowResult(ProductId, uint64_t)> aheadNow;   // render thread; null = the lane has no ahead claim
    const void* token = nullptr;   // the bound lane (BindLane / UnbindLane by token)
};

struct AcquireCounters {
    uint64_t reusedLane = 0;
    uint64_t reusedMemo = 0;
    uint64_t filledAtRead = 0;
    uint64_t ownerFillCalls = 0;   // == filledAtRead + declined / over-budget fills: proves "computed once"
    uint64_t reusedLaneCopy = 0;   // off the render thread: the lane's published bytes copied out (no recompute)
    // L-CORE M5 (coordinator ruling on B1-SEAL-VIA-ACQUIRE): the seal's own CPU live fill, counted in the same ledger.
    uint64_t sealComputed = 0;     // the seal computed the frame (no Acquire publication): published as the lane slot
    uint64_t sealReused = 0;       // the seal copied an Acquire publication (computed once, by the owner fill)
    // QKP slice C (R14-73). COUNTING BASIS of these three: LANE FRAMES of ONE product (a count of (product, frame) events). They are
    // not the declared history H (DeclaredFillHistory: frames of reader lag) and not the VRAM bytes basis of AheadNeed (gpuOnly).
    // aheadNow = Ran and the slot readable: made on this request. (A worker publishing between the slot read and the ask also
    // answers Ran: that frame is then counted here though it was made by the worker; rare, and it is never counted twice.)
    uint64_t laneAheadMade = 0;
    uint64_t sealComputedReduced = 0;  // seal live fills that computed nothing because the frame was already made (ahead or published)
    uint64_t workerReused = 0;         // the lane's worker skipped a frame the render thread had made first (nothing recomputed)
    // Frames COMPUTED for this product (any variant): owner fills that published + seal computes. The compute-once ledger.
    // Queue-keyed products (design s2.4/2.5): fills of a frame AHEAD of the render frame, render-time reuses of such a fill, and
    // the recompute of a frame after an invalidation (counted apart, so the compute-once ledger stays readable).
    uint64_t aheadFilled = 0;
    uint64_t aheadReused = 0;
    uint64_t invalidatedRecompute = 0;
    uint64_t discarded = 0;          // spots of dropped frames released (Discard)
    uint64_t planMiss = 0;           // an AHEAD fill measured above its reserve did not fit: refused, made at its render (s2.7 safety net)
    // R2: textures non-view GPU fills made through their OwnerGpuContext, the bytes, and the allocations it refused (over the reserve,
    // no device, device refusal).
    uint64_t gpuAllocMade = 0;
    uint64_t gpuAllocBytes = 0;
    uint64_t gpuAllocRefused = 0;
    uint64_t Computed() const { return filledAtRead + sealComputed + laneAheadMade - invalidatedRecompute; }
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

    // The owning unit of a product: the catalog's unit folder (ProductRows). "" = not in the catalog (no owner can register).
    static std::string OwnerUnitOf(ProductId id)
    {
        const Catalog::ProductRow* r = Catalog::RowOf(id);
        return r != nullptr && r->unit != nullptr ? std::string(r->unit) : std::string();
    }

    SkeletonMemo() = default;
    SkeletonMemo(const SkeletonMemo&) = delete;
    SkeletonMemo& operator=(const SkeletonMemo&) = delete;
    ~SkeletonMemo()
    {
        std::vector<std::shared_ptr<Entry>> drop;
        {
            std::lock_guard<std::mutex> g(mutex_);
            for (auto& kv : memo_)
                drop.push_back(kv.second);
            memo_.clear();
            for (auto& e : retired_)
                drop.push_back(e);
            retired_.clear();
        }
    }

    // ---- setup ----

    enum class Registration : uint8_t { Registered, Replaced, NotOwner, NotInCatalog, AlreadyRegistered, BadDefaults };
    // G3: the ONE fill of (product, form). `unit` must be the catalog's owning unit (OwnerUnitOf); anything else is refused
    // and counted (Refusals, on the status lines). A second registration of a registered (product, form) is refused too:
    // a replacement (graph rebuild) is explicit, ReplaceOwnerFill.
    Registration RegisterOwnerFill(ProductId id, AcquireForm form, const std::string& unit, OwnerFill fill)
    {
        return Register(id, form, unit, std::move(fill), false);
    }
    Registration ReplaceOwnerFill(ProductId id, AcquireForm form, const std::string& unit, OwnerFill fill)
    {
        return Register(id, form, unit, std::move(fill), true);
    }
    // Removes a module registration (the unit must be the owner). False = none was registered.
    bool UnregisterOwnerFill(ProductId id, AcquireForm form, const std::string& unit)
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (unit != OwnerUnitOf(id))
            return false;
        return owners_.erase(OwnerKey(id, form)) != 0;
    }
    bool HasOwnerFill(ProductId id, AcquireForm form) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return OwnerLocked(OwnerKey(id, form)) != nullptr;
    }
    // L-CORE: the lane BINDS the memo: its slots become the first G2 source and its fills (the lane's CPU live computes) take
    // precedence over module registrations of the same (product, form) while bound. A second bind replaces the first (graph
    // rebuild: the new bundle binds before the old one ends). A fill for a product outside the catalog is ignored.
    void BindLane(LaneSlotSource src, std::map<std::pair<ProductId, AcquireForm>, OwnerFill> fills)
    {
        std::lock_guard<std::mutex> g(mutex_);
        lane_ = std::move(src);
        laneOwners_.clear();
        for (auto& kv : fills)
            if (!OwnerUnitOf(kv.first.first).empty())
                laneOwners_[OwnerKey(kv.first.first, kv.first.second)] = std::move(kv.second);
        ++laneBinds_;
    }
    // The lane `token` ends: its source and fills are dropped, ONLY if it is still the bound one (a rebuild bound a newer lane
    // first: nothing happens). Module registrations stay. True = unbound. RENDER THREAD ONLY (like BindLane): the Acquire ahead
    // branch asks the lane with the memo lock released and relies on this never running concurrently with it.
    bool UnbindLane(const void* token)
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (token == nullptr || lane_.token != token)
            return false;
        lane_ = LaneSlotSource();
        laneOwners_.clear();
        return true;
    }
    bool LaneBound() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return bool(lane_.read) || bool(lane_.copyPublished);
    }
    uint64_t LaneBinds() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return laneBinds_;
    }
    // The render thread (the skeleton, every Begin). Not declared = every thread counts as the render thread.
    void SetRenderThread(std::thread::id id)
    {
        std::lock_guard<std::mutex> g(mutex_);
        renderThread_ = id;
    }
    bool OnRenderThread() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return OnRenderThreadLocked();
    }
    // A frame whose memo spots were released (a request for it is FrameReleased).
    bool Released(uint64_t seq) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return haveReleased_ && seq <= releasedThrough_;
    }
    // The memo's GPU working set as VRAM ledger LEVELS (amendment A5 / audit M6): one level per step of THE ahead ladder
    // (QueueWindow.h AheadLadderStates over the registered allocating GPU fills and the current window), non-increasing, so a short
    // grant gives the largest plan that fits, never 0. `mode` / `budget` = the S-C inputs (QueueWindow.h; null budget = not fed, no
    // S-C level). The top level is the full window (GpuNeedBytes).
    std::vector<uint64_t> GpuNeedLevels(QueueMode mode = QueueMode::Normal, const DelayBudget::Inputs* budget = nullptr) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        // The memo's OWN window: before the first SetWindow there is none, so the render frame only (nothing ahead).
        QueueWindow own = window_;
        if (!windowSet_) {
            own.ahead = ahead_;
            own.aheadCompute = 0;
        }
        AheadNeed n = AheadNeedLocked(own, mode, budget, true);
        return AheadLevels(n);
    }
    uint64_t GpuNeedBytes() const
    {
        const std::vector<uint64_t> levels = GpuNeedLevels();
        return levels.empty() ? 0u : levels.front();
    }
    // OnSeal calls so far (the skeleton seals once per Begin: with the lane after its seal, else directly).
    uint64_t Seals() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return seals_;
    }
    uint32_t KeepFrames() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return keepFrames_;
    }
    uint64_t Refusals() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return refusals_;
    }
    // The renderer this memo belongs to (handed to every fill as OwnerFillRequest::host).
    void SetHost(const void* host)
    {
        std::lock_guard<std::mutex> g(mutex_);
        host_ = host;
    }
    void SetLaneSource(LaneSlotSource src)
    {
        std::lock_guard<std::mutex> g(mutex_);
        lane_ = std::move(src);
    }
    // The skeleton GPU dispatcher exists (SkeletonModule wiring). Without it every GPU fill declines: NoGpuDispatcher.
    void SetGpuDispatcherPresent(bool present)
    {
        std::lock_guard<std::mutex> g(mutex_);
        gpuPresent_ = present;
    }
    // R2: the device side of the sanctioned GPU context (the skeleton GPU dispatcher; null when it goes). Render thread.
    void SetGpuDevice(IOwnerGpuDevice* device)
    {
        std::lock_guard<std::mutex> g(mutex_);
        gpuDevice_ = device;
    }
    // R2 (audit S1): textures a reader still pinned when the device went: forgotten by the device (never destroyed through a device
    // that may be gone; the driver frees them with the device). Counted here and on the status lines (not silent).
    void NoteGpuForgotten(uint64_t textures, uint64_t bytes)
    {
        std::lock_guard<std::mutex> g(mutex_);
        gpuForgotten_ += textures;
        gpuForgottenBytes_ += bytes;
    }
    // R2: the GPU device goes (dispatcher release / device lost): every GPU spot is released now, retired ones too (their textures
    // are the device's), so no spot names a destroyed texture. Render thread, between fills.
    void ReleaseGpuSpots()
    {
        std::vector<std::shared_ptr<Entry>> drop;
        {
            std::lock_guard<std::mutex> g(mutex_);
            ReleaseIfLocked([](const Entry& e) { return e.gpuForm && e.state != Entry::Filling; }, drop);
            for (auto it = retired_.begin(); it != retired_.end();) {
                if ((*it)->gpuForm) {
                    drop.push_back(*it);
                    it = retired_.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = stale_.begin(); it != stale_.end();) {
                if ((*it)->gpuForm) {
                    drop.push_back(*it);
                    it = stale_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        drop.clear();   // outside the lock: the texture keep-alives run here
    }
    // THE QUEUE WINDOW (core/QueueWindow.h; design s2.3): the memo keeps a frame's spots while the frame is in the queue and
    // admits fills up to current + ahead. Spots are released at the seal when the frame has LEFT the queue:
    //   f < min(seal - back, presented - history)   (the presented edge only when present facts were delivered)
    // never to make room. A window set once turns the admission on: a fill beyond the window is NotQueued (visible, counted).
    // The skeleton sets it at every Begin (slice B); SetKeepFrames below stays the B1 / test form (back only, no admission).
    void SetWindow(const QueueWindow& w)
    {
        std::lock_guard<std::mutex> g(mutex_);
        keepFrames_ = w.back;
        ahead_ = w.ahead;
        history_ = w.history;
        // presented_ only moves forward (NotePresented feeds the exact value, the window an older peak-based one: audit SHOULD 8),
        // except when the window's numbering went back (a restart after a discontinuity). A window without present facts leaves
        // what NotePresented delivered; when only earlier WINDOWS delivered them, the facts lapse with the window (design s4
        // MEMO-T4: present facts not fed -> the render-seq edge, and the status line says so).
        if (w.presentKnown) {
            const bool restarted = haveSealed_ && w.current < lastSeal_;
            if (!presentKnown_ || restarted || w.presented > presented_)
                presented_ = w.presented;
            presentKnown_ = true;
        } else if (!presentNoted_) {
            presentKnown_ = false;
        }
        windowSet_ = true;
        window_ = w;
    }
    QueueWindow Window() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return window_;
    }
    // The last presented source frame (the AfterPresent feed); a frame is kept until it was presented (minus history).
    void NotePresented(uint64_t seq)
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (!presentKnown_ || seq > presented_)
            presented_ = seq;
        presentKnown_ = true;
        presentNoted_ = true;
    }
    // A DROPPED frame (never rendered): its spots are released, every other frame is untouched (design s2.5).
    void Discard(uint64_t seq)
    {
        std::vector<std::shared_ptr<Entry>> drop;
        {
            std::lock_guard<std::mutex> g(mutex_);
            for (auto& kv : memo_)
                if (kv.second->seq == seq && kv.second->state == Entry::Ready)
                    ++counters_[kv.second->id].discarded;
            ReleaseIfLocked([seq](const Entry& e) { return e.seq == seq; }, drop);
            RetireCompletedLocked(drop);
        }
        drop.clear();
    }
    // A DISCONTINUITY (flush, reset, live-queue reset, discontinuity, gap, format change): every spot of every frame is released
    // (ahead frames, the presenter queue, the history), the epoch moves, and the next fill of a released key is counted as
    // invalidatedRecompute. The vector-validity rule: one valid state, invalidated at discontinuities (design s2.5, owner Q1 a).
    enum class InvalidateCause : uint8_t { Flush, Reset, LiveQueueReset, Discontinuity, Gap, FormatChange, Count };
    static const char* InvalidateToken(InvalidateCause c)
    {
        switch (c) {
        case InvalidateCause::Flush: return "flush";
        case InvalidateCause::Reset: return "reset";
        case InvalidateCause::LiveQueueReset: return "live queue reset";
        case InvalidateCause::Discontinuity: return "discontinuity";
        case InvalidateCause::Gap: return "gap";
        case InvalidateCause::FormatChange: return "format change";
        case InvalidateCause::Count: break;
        }
        return "?";
    }
    // Any thread (the queue thread's OnQueue, amendment A4 / audit M5): the epoch moves AT ONCE, so an Acquire that follows (on any
    // thread, before the next Begin) never sees a pre-flush spot: an entry of an older epoch is absent. Its bytes are released at the
    // next seal (OnSeal), never under the caller's lock. Every bump moves the epoch (a spot filled between two bumps is invalidated
    // by the second); the COUNT is coalesced per Begin interval: Reset then Flush is ONE discontinuity (audit SHOULD 11).
    // The admission stays ON across the bump (audit MUST 4): NotQueued, the ahead-compute / plan NotPlannedAhead and PresentsStalled
    // keep using the last seal's window until the next Begin re-derives it, so a fill between the flush and the next Begin is never
    // beyond the ceiling or the queue. Only the RELEASED numbering is reset (the numbering may restart after a discontinuity).
    void BumpEpoch(InvalidateCause cause)
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (cause < InvalidateCause::Count)
            ++invalidations_[size_t(cause)];
        if (!bumpedThisInterval_) {
            bumpedThisInterval_ = true;
            ++discontinuities_;
            invalidatedKeys_.clear();   // only this interval's discontinuity keys are a recompute "after invalidation"
        }
        for (const auto& kv : memo_)
            if (kv.second->state == Entry::Ready && kv.second->epoch == epoch_)
                invalidatedKeys_.insert(kv.first);
        ++epoch_;
        fillsPerFrame_.clear();         // the new epoch measures its own occupancy (r2 MUST-1)
        haveReleased_ = false;          // the numbering may restart after a discontinuity: nothing older is refused
        releasedThrough_ = 0;
        numberingMayRestart_ = true;    // the next seal may go back without releasing this epoch's fills
    }
    void Invalidate(InvalidateCause cause) { BumpEpoch(cause); }
    uint64_t Discontinuities() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return discontinuities_;
    }
    uint64_t Epoch() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return epoch_;
    }
    // The S3 admission link (design s2.7). What the memo's ahead work costs for `w`: one AheadCost per registered GPU fill that
    // allocates (its declared reserve, or the measured bytes of its largest result when larger, times the measured fills one render
    // frame holds: a per-picture keyed product holds several at once) and per CPU fill that has run (its
    // measured bytes; CPU bytes are not in the VRAM grant, listed for the cost ledger with bytesPerFrame 0 for the VRAM plan).
    // `budget`: the delay budget the scheduler plans the queue with (null = not fed: the ladder offers no S-C step and says so).
    AheadNeed MakeAheadNeed(const QueueWindow& w, QueueMode mode = QueueMode::Normal, const DelayBudget::Inputs* budget = nullptr) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return AheadNeedLocked(w, mode, budget, false);
    }
    // `gpuOnly`: the VRAM view (GPU allocating fills, back = max(window back, the fills' declared history)).
    // `w` is described as given: the Configure-time admission (S-3a S3) plans a window BEFORE the memo has one set, so the memo's
    // no-window state never overrides the caller's window (GpuNeedLevels applies it to the memo's own window).
    AheadNeed AheadNeedLocked(const QueueWindow& w, QueueMode mode, const DelayBudget::Inputs* budget, bool gpuOnly) const
    {
        AheadNeed n;
        n.window = w;
        n.grantBytes = gpuGranted_ ? gpuBudget_ : 0u;
        n.heldBytes = gpuBytes_;
        n.mode = mode;
        if (budget != nullptr) {
            n.budget = *budget;
            n.budgetFed = true;
        }
        uint16_t back = n.window.back > keepFrames_ ? n.window.back : uint16_t(keepFrames_);
        const std::map<uint32_t, const OwnerFill*> all = MergedOwnersLocked();
        for (const auto& kv : all) {
            const OwnerFill& f = *kv.second;
            const bool gpu = (kv.first & 1u) != 0u;
            if (gpu && f.view)
                continue;   // 0 bytes
            AheadCost c;
            c.id = uint16_t(kv.first >> 1);
            c.form = gpu ? uint8_t(1) : uint8_t(0);
            const auto m = measuredBytes_.find(kv.first);
            const uint64_t measured = m != measuredBytes_.end() ? m->second : 0u;
            if (gpu) {
                // One frame's spot holds EVERY GPU result of this product held at once: a product keyed per picture (the edge
                // map's stage ordinals, CB-18 SHOULD-2) holds several in one render frame, each against its own reserve. The count
                // is MEASURED (the most fills one render frame has held at once in this epoch, 1 before any), never a fixed
                // constant; a cadence repeat re-renders after a seal released the earlier results, so it does not add (r2 MUST-1).
                const uint64_t perFill = measured > f.reserveBytes ? measured : f.reserveBytes;
                const auto fp = fillsPerFrame_.find(kv.first);
                const uint64_t fills = fp != fillsPerFrame_.end() && fp->second > 1u ? uint64_t(fp->second) : 1u;
                c.bytesPerFrame = perFill * fills;
                c.measured = measured != 0;
                if (f.DeclaredHistory() > back)
                    back = f.DeclaredHistory();
            } else {
                if (gpuOnly || measured == 0)
                    continue;   // a CPU fill costs no VRAM; one that never ran costs nothing known yet
                c.measured = true;
            }
            c.cls = f.jobClass;
            c.declaredDepth = f.aheadDepth;
            n.costs.push_back(c);
        }
        if (gpuOnly)
            n.window.back = back;
        return n;
    }
    // QKP-BCE read-only accessors (R14-61). Both read the registered owner fills (the bound lane's fill shadows a module's) under
    // the memo mutex and change nothing.
    // The window history term H's owner input: the largest DECLARED reader lag (OwnerFill::DeclaredHistory) of any registered
    // fill, CPU or GPU, view or not; 0 when no owner declares one.
    uint16_t DeclaredFillHistory() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        uint16_t h = 0;
        for (const auto& kv : MergedOwnersLocked())
            if (kv.second->DeclaredHistory() > h)
                h = kv.second->DeclaredHistory();
        return h;
    }
    // One declared ahead demand: a registered fill whose consumers declared an ahead depth (OwnerFill::aheadDepth > 0).
    struct AheadDemand {
        uint16_t id = 0;     // ProductId
        uint8_t form = 0;    // 0 = CPU, 1 = GPU
        uint16_t depth = 0;  // the declared ahead depth in frames
        JobClass cls = JobClass::Core;
    };
    // The kAheadNeed term's input: every registered fill (lane fill shadowing a module's) declaring an ahead depth > 0, ordered
    // by owner key. Empty = no consumer (aheadCompute stays 0 / NoConsumer upstream).
    std::vector<AheadDemand> ActiveAheadDemands() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        std::vector<AheadDemand> out;
        for (const auto& kv : MergedOwnersLocked()) {
            if (kv.second->aheadDepth == 0)
                continue;
            AheadDemand d;
            d.id = uint16_t(kv.first >> 1);
            d.form = (kv.first & 1u) != 0u ? uint8_t(1) : uint8_t(0);
            d.depth = kv.second->aheadDepth;
            d.cls = kv.second->jobClass;
            out.push_back(d);
        }
        return out;
    }
    // The plan the admission made (S3's PlanAhead / the interim PlanAheadLadder): an Acquire that would FILL a frame further ahead
    // of the render frame than its product's planned depth is NotPlannedAhead (visible, counted; the frame is made at its render).
    // A product the plan does not name keeps the window's ahead. Spots already attached are never released by a plan.
    void SetAheadPlan(const AheadPlan& plan)
    {
        std::lock_guard<std::mutex> g(mutex_);
        plan_ = plan;
        planSet_ = true;
    }
    AheadPlan Plan() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return plan_;
    }
    uint64_t Invalidations(InvalidateCause c) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return c < InvalidateCause::Count ? invalidations_[size_t(c)] : 0u;
    }
    void SetKeepFrames(uint32_t frames)
    {
        std::lock_guard<std::mutex> g(mutex_);
        keepFrames_ = frames;
    }
    // The VRAM ledger's grant for the memo's GPU spots (SkeletonModule: the scratch-arena grant of the lane plan). Not granted
    // = every GPU fill declines (NoLedgerGrant); granted = GPU bytes are reserved against `bytes`.
    void SetGpuBudget(bool granted, uint64_t bytes)
    {
        std::lock_guard<std::mutex> g(mutex_);
        gpuGranted_ = granted;
        gpuBudget_ = bytes;
    }
    bool GpuGranted() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return gpuGranted_;
    }
    uint64_t GpuBudget() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return gpuBudget_;
    }
    // The producer fence completion (GPU retire): a released GPU spot whose recorded read fence is beyond it is kept. Without a
    // source, a spot with a noted read is held until teardown (shown on the status lines), never dropped under a GPU read.
    void SetFenceCompleted(std::function<uint64_t()> f)
    {
        std::lock_guard<std::mutex> g(mutex_);
        fenceCompleted_ = std::move(f);
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

    // ---- G1-G5 ----

    // `laneAheadTried` (internal): set by the memo's own re-entry after the lane said NotNode, so the lane is asked once.
    AcquireResult Acquire(ProductId id, uint64_t seq, AcquireForm form, const VariantKey& vIn = VariantKey(), bool laneAheadTried = false)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        const OwnerFill* ow = OwnerLocked(OwnerKey(id, form));
        const VariantKey v = vIn.Canonical(ow != nullptr ? ow->defaults : VariantKey());
        const Key key{ id, seq, uint8_t(form), v };
        const bool renderThread = OnRenderThreadLocked();

        // Re-entrancy: an owner fill on this thread asking THIS memo for the key it is filling would wait for itself.
        for (const FillingKey& k : FillingOnThisThread())
            if (k.memo == this && !(k.key < key) && !(key < k.key))
                return Fail(counters_[id], id, seq, AcquireCause::Reentrant);

        // GPU forms are render thread (their textures and fences are the render pl_gpu's).
        if (form == AcquireForm::Gpu && !renderThread)
            return Fail(counters_[id], id, seq, AcquireCause::WrongThread);

        // G2 (a'): off the render thread, a lane CPU product's PUBLISHED bytes (copied out through its lease).
        if (v.IsDefault() && !renderThread && form == AcquireForm::Cpu && lane_.copyPublished) {
            std::shared_ptr<std::vector<uint8_t>> copy = std::make_shared<std::vector<uint8_t>>();
            const auto fn = lane_.copyPublished;
            lk.unlock();   // the copy runs outside the lock (a lease read: any thread)
            const LaneSlotSource::State s = fn(id, seq, copy.get());
            lk.lock();
            if (s == LaneSlotSource::State::Present && !copy->empty()) {
                ++counters_[id].reusedLaneCopy;
                AcquireResult r;
                r.status = AcquireStatus::Reused;
                r.source = AcquireSource::LaneSlot;
                r.cpu = copy->data();
                r.cpuBytes = copy->size();
                r.pin = copy;
                Served(id);
                return r;
            }
            ow = OwnerLocked(OwnerKey(id, form));   // re-read after the unlock (a bind may have changed it)
        }
        AcquireCounters& c = counters_[id];

        // G2 (a): the lane's slot (default variant), render thread.
        if (v.IsDefault() && renderThread && lane_.read) {
            LaneSlotSource::Read rd;
            const LaneSlotSource::State s = lane_.read(id, seq, form, &rd);
            if (s == LaneSlotSource::State::Present) {
                ++c.reusedLane;
                AcquireResult r;
                r.status = AcquireStatus::Reused;
                r.source = AcquireSource::LaneSlot;
                r.cpu = rd.cpu;
                r.cpuBytes = rd.cpuBytes;
                r.gpu = rd.gpu;
                r.pin = rd.pin;
                Served(id);
                return r;
            }
            if (s == LaneSlotSource::State::Pending)
                return Fail(c, id, seq, AcquireCause::LanePending);
            if (s == LaneSlotSource::State::Failed)
                return Fail(c, id, seq, AcquireCause::LaneFailed);
            if (s == LaneSlotSource::State::NoHandle)
                return Fail(c, id, seq, AcquireCause::NoGpuHandle);
        }

        // G2 (b): the memo spot (a kept spot of a released frame included); a fill in flight is waited for (computed once).
        auto it = memo_.find(key);
        if (it != memo_.end() && it->second->epoch != epoch_ && it->second->state != Entry::Filling) {
            // A spot of an older epoch (a discontinuity since): absent for every reader. Its bytes go at the next seal.
            Entry& old = *it->second;
            if (old.gpuForm)
                gpuBytes_ -= old.reserved;
            else
                cpuBytes_ -= old.bytes.size();
            old.reserved = 0;
            old.orphaned = true;
            stale_.push_back(it->second);
            memo_.erase(it);
            it = memo_.end();
        }
        if (it == memo_.end() && ReleasedForOwnerLocked(seq, ow))
            return Fail(c, id, seq, AcquireCause::FrameReleased);   // a released frame is never recomputed (lane slots above stay readable)
        if (it != memo_.end()) {
            const std::shared_ptr<Entry> e = it->second;
            if (e->state == Entry::Filling) {
                // Cycle by key across threads: the fill we would wait for is (transitively) waiting for a fill of ours.
                if (WaitWouldCycleLocked(e))
                    return Fail(c, id, seq, AcquireCause::Reentrant);
                const std::thread::id self = std::this_thread::get_id();
                waitingOn_[self] = e;
                cv_.wait(lk, [&e]() { return e->state != Entry::Filling; });
                waitingOn_.erase(self);
            }
            if (e->state == Entry::Ready) {
                ++c.reusedMemo;
                if (e->ahead && !e->aheadReuseCounted && haveSealed_ && seq <= lastSeal_) {
                    ++c.aheadReused;   // an ahead fill reused once its frame renders (or later): the same spot
                    e->aheadReuseCounted = true;
                }
                return ResultOf(e, AcquireStatus::Reused);
            }
            // Declined for this frame: the same cause, the owner is not asked twice for one frame.
            return Fail(c, id, seq, e->cause == AcquireCause::None ? AcquireCause::OwnerDeclined : e->cause);
        }

        // Admission (design s2.3): a fill for a frame beyond the queue window is refused, never run.
        if (windowSet_ && haveSealed_ && seq > lastSeal_ + uint64_t(ahead_))
            return Fail(c, id, seq, AcquireCause::NotQueued);
        // amendment 3: inside the queue but beyond the computed depth: not yet; computed once when it comes within aheadCompute.
        if (windowSet_ && haveSealed_ && seq > lastSeal_ + uint64_t(window_.aheadCompute))
            return Fail(c, id, seq, AcquireCause::NotPlannedAhead);
        if (planSet_ && haveSealed_ && seq > lastSeal_) {
            uint16_t d = 0;
            if (plan_.Has(uint16_t(id), v.Hash(), &d) && seq - lastSeal_ > uint64_t(d))
                return Fail(c, id, seq, AcquireCause::NotPlannedAhead);
        }
        // Stalled presents (audit S3): the presented frame lags the render by more than the queue holds by structure (H + R + 1 + ahead;
        // P itself grows in a stall, so it is not the yardstick): ahead demand stops (visible), nothing is evicted.
        if (windowSet_ && presentKnown_ && haveSealed_ && seq > lastSeal_ && lastSeal_ > presented_
            && lastSeal_ - presented_ > uint64_t(window_.history) + uint64_t(window_.renderAhead) + 1u + uint64_t(ahead_)) {
            return Fail(c, id, seq, AcquireCause::PresentsStalled);
        }
        if (renderThread && lane_.dropped && lane_.dropped(id))
            return Fail(c, id, seq, AcquireCause::RegulatorDropped);
        // QKP slice C: a lane product AHEAD of the render frame (admitted above) is made by the lane's own claim, not twice.
        if (renderThread && v.IsDefault() && form == AcquireForm::Cpu && !laneAheadTried && lane_.aheadNow && lane_.read && haveSealed_ && seq > lastSeal_) {
            const auto ahead = lane_.aheadNow;
            lk.unlock();   // the lane runs its compute outside the memo lock (the lane's inputs may Acquire)
            const LaneSlotSource::AheadNowResult ar = ahead(id, seq);
            lk.lock();
            if (ar.kind == LaneSlotSource::AheadNow::Claimed)
                return Fail(c, id, seq, AcquireCause::LanePending);
            if (ar.kind == LaneSlotSource::AheadNow::Declined) {
                AcquireResult r = Fail(c, id, seq, AcquireCause::OwnerDeclined);
                if (ar.why != nullptr && ar.why[0] != '\0')
                    lastCause_[id] += std::string(": ") + ar.why;
                return r;
            }
            if (ar.kind == LaneSlotSource::AheadNow::Ran) {
                LaneSlotSource::Read rd;
                // The lock was released: a lane unbind in that window leaves no read (the render thread owns UnbindLane / BindLane).
                if (lane_.read && lane_.read(id, seq, form, &rd) == LaneSlotSource::State::Present) {
                    ++c.laneAheadMade;
                    ++c.reusedLane;
                    AcquireResult r;
                    r.status = AcquireStatus::Reused;
                    r.source = AcquireSource::LaneSlot;
                    r.cpu = rd.cpu;
                    r.cpuBytes = rd.cpuBytes;
                    r.pin = rd.pin;
                    Served(id);
                    return r;
                }
                return Fail(c, id, seq, AcquireCause::LanePending);   // told Ran but the slot is not readable yet: visible, retried
            }
            // NotNode: not a lane product. The lock was released, so the owner and the memo may have changed (an unbind erased the
            // owner map node; another thread inserted this key): start over through the normal path with the lane not asked again.
            lk.unlock();
            return Acquire(id, seq, form, vIn, true);
        }
        if (ow == nullptr || !ow->fill) {
            AcquireResult r = Fail(c, id, seq, AcquireCause::NoOwnerFill);
            if (IsReservedProduct(id))   // a fixed-table id whose product has not landed: say so, not just "no owner fill"
                lastCause_[id] += std::string(": reserved product id ") + ReservedProductName(id) + " (not landed, no owner)";
            return r;
        }
        if ((v.Axes() & ~ow->variantAxes) != 0u || v.precision == kPrecisionFloat16)  // float16 is not 12-bit safe: every owner declines it, visibly (N2)
            return Fail(c, id, seq, AcquireCause::VariantUndeclared);
        if (ow->renderThreadOnly && !renderThread)
            return Fail(c, id, seq, AcquireCause::WrongThread);
        uint64_t reserve = 0;
        if (form == AcquireForm::Gpu && !ow->view) {
            if (!gpuPresent_)
                return Fail(c, id, seq, AcquireCause::NoGpuDispatcher);
            if (!gpuGranted_)
                return Fail(c, id, seq, AcquireCause::NoLedgerGrant);
            reserve = ow->reserveBytes;
            if (gpuBytes_ + reserve > gpuBudget_)
                return Fail(c, id, seq, AcquireCause::OverBudget);
        }

        // G3: the owner fills it once, outside the lock (other keys proceed; readers of this key wait on the entry).
        const std::shared_ptr<Entry> e = std::make_shared<Entry>();
        e->state = Entry::Filling;
        e->seq = seq;
        e->id = id;
        e->gpuForm = form == AcquireForm::Gpu;
        e->filler = std::this_thread::get_id();
        e->frameScoped = ow->frameScoped;
        e->keep = ow->DeclaredHistory();
        e->ahead = haveSealed_ && seq > lastSeal_;
        e->epoch = epoch_;
        e->reserved = reserve;
        gpuBytes_ += reserve;
        memo_[key] = e;
        ++c.ownerFillCalls;
        const OwnerFill fill = *ow;   // a copy: a replacement / an unbind during the fill does not invalidate it
        IOwnerGpuDevice* const gpuDevice = gpuDevice_;
        lk.unlock();
        OwnerFillRequest req;
        req.host = host_;
        req.id = id;
        req.seq = seq;
        req.form = form;
        req.variant = v;
        // R2: the one sanctioned GPU context, for a non-view GPU fill only, granted the fill's reserve.
        std::unique_ptr<OwnerGpuContext> gpuContext;
        if (form == AcquireForm::Gpu && !fill.view) {
            gpuContext.reset(new OwnerGpuContext(gpuDevice, reserve));
            req.gpu = gpuContext.get();
        }
        OwnerFillOutput out;
        bool ok = false;
        FillingOnThisThread().push_back(FillingKey{ this, key });
        try {
            ok = fill.fill(req, *this, out);
        } catch (...) {
            ok = false;   // a throwing fill is a decline (counted, visible), never a stuck Filling entry
        }
        FillingOnThisThread().pop_back();
        if (gpuContext)
            gpuContext->Close();   // a kept pointer gets nothing after the fill
        if (!ok && gpuContext && gpuContext->Refused() != 0 && out.declineReason.empty())
            out.declineReason = gpuContext->LastRefusal();   // a refused allocation is worded with the decline
        if (ok && form == AcquireForm::Cpu && out.bytes.empty())
            ok = false;
        if (ok && form == AcquireForm::Gpu && !out.gpu.Valid())
            ok = false;
        if (ok && fill.view && out.gpu.bytes != 0)
            ok = false;   // a view allocates nothing: bytes would be outside the ledger
        lk.lock();
        AcquireCounters& c2 = counters_[id];
        if (gpuContext) {
            c2.gpuAllocMade += gpuContext->Made();
            c2.gpuAllocBytes += gpuContext->Used();
            c2.gpuAllocRefused += gpuContext->Refused();
        }
        AcquireCause cause = ok ? AcquireCause::None : AcquireCause::OwnerDeclined;
        if (ok && form == AcquireForm::Gpu && !e->orphaned && out.gpu.bytes > e->reserved
            && gpuBytes_ - e->reserved + out.gpu.bytes > gpuBudget_)
            cause = AcquireCause::OverBudget;
        if (!e->orphaned) {
            gpuBytes_ -= e->reserved;
            e->reserved = 0;
        }
        if (cause != AcquireCause::None) {
            e->state = Entry::Declined;
            e->cause = cause;
            cv_.notify_all();
            if (cause == AcquireCause::OverBudget && e->ahead && !e->orphaned) {
                // The s2.7 safety net: an AHEAD fill measured above its reserve is a plan miss. Its key is not kept declined, so the
                // frame is made at its own render; nothing already attached to a queued frame is released.
                ++c2.planMiss;
                const auto self = memo_.find(key);
                if (self != memo_.end() && self->second == e)
                    memo_.erase(self);
                measuredBytes_[OwnerKey(id, form)] = out.gpu.bytes > measuredBytes_[OwnerKey(id, form)] ? out.gpu.bytes
                                                                                                        : measuredBytes_[OwnerKey(id, form)];
            }
            const std::string reason = out.declineReason;
            lk.unlock();
            out = OwnerFillOutput();   // released outside the lock
            lk.lock();
            AcquireResult failed = Fail(counters_[id], id, seq, cause);
            if (!reason.empty())
                lastCause_[id] += " (" + reason + ")";
            return failed;
        }
        {
            uint64_t& mb = measuredBytes_[OwnerKey(id, form)];   // the measured size, for AheadNeed (the cost ledger's input)
            const uint64_t got = form == AcquireForm::Gpu ? out.gpu.bytes : uint64_t(out.bytes.size());
            if (got > mb)
                mb = got;
            if (form == AcquireForm::Gpu && !e->orphaned) {
                // The measured fills one render frame HOLDS (AheadNeed's multiplier): the live results of this (product, form)
                // for `seq` in this epoch at this moment, this fill included (still Filling here). Results a seal released
                // (frame-scoped spots: a cadence repeat's earlier render of the seq) are no longer in the memo, so a repeat
                // render never adds to the count (r2 MUST-1).
                uint32_t held = 0;
                for (const auto& kv : memo_) {
                    const Entry& x = *kv.second;
                    if (x.id == id && x.seq == seq && x.gpuForm && !x.orphaned && x.epoch == epoch_ && x.state != Entry::Declined)
                        ++held;
                }
                uint32_t& most = fillsPerFrame_[OwnerKey(id, form)];
                if (held > most)
                    most = held;
            }
        }
        if (out.scope != OwnerFillOutput::Scope::FillDefault)
            e->frameScoped = out.scope == OwnerFillOutput::Scope::Frame;   // per-result lifetime (design s2.3)
        e->bytes = std::move(out.bytes);
        e->gpu = std::move(out.gpu);
        if (!e->orphaned) {
            if (e->gpuForm) {
                e->reserved = e->gpu.bytes;
                gpuBytes_ += e->gpu.bytes;
            }
            cpuBytes_ += e->bytes.size();
        }
        e->state = Entry::Ready;
        ++c2.filledAtRead;
        if (e->ahead)
            ++c2.aheadFilled;
        if (!invalidatedKeys_.empty() && invalidatedKeys_.erase(key) != 0)
            ++c2.invalidatedRecompute;   // the frame was computed before the invalidation: counted apart
        cv_.notify_all();
        Served(id);
        return ResultOf(e, AcquireStatus::FilledAtRead);
    }

    // A GPU reader recorded work that reads `r`'s texture up to `fence`: the spot is not retired before it completes.
    void NoteGpuRead(const AcquireResult& r, uint64_t fence)
    {
        if (r.source != AcquireSource::Memo || !r.pin)
            return;
        std::lock_guard<std::mutex> g(mutex_);
        Entry* e = const_cast<Entry*>(static_cast<const Entry*>(r.pin.get()));
        if (fence > e->readFence)
            e->readFence = fence;
    }

    // L-CORE M5: the bound lane's seal reports what it did with a CPU live fill (SkeletonAcquireLane.h's lookup).
    void NoteSealComputed(ProductId id)
    {
        std::lock_guard<std::mutex> g(mutex_);
        ++counters_[id].sealComputed;
    }
    void NoteSealReused(ProductId id)
    {
        std::lock_guard<std::mutex> g(mutex_);
        ++counters_[id].sealReused;
    }
    // QKP slice C (R14-73): the lane's seal computed nothing for (id) because the frame was already made (ahead claim / published).
    void NoteSealComputedReduced(ProductId id)
    {
        std::lock_guard<std::mutex> g(mutex_);
        ++counters_[id].sealComputedReduced;
    }
    // QKP slice C (R14-73): the lane's worker skipped a frame of (id) the render thread had made first.
    void NoteWorkerReused(ProductId id)
    {
        std::lock_guard<std::mutex> g(mutex_);
        ++counters_[id].workerReused;
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

    // Lifetime: frame `seq` is sealed; memos of frames <= seq - 1 - keepFrames are released (pinned results stay valid; GPU spots
    // under a read fence are retired when it completes). Frame numbers going back (a new epoch) release everything first,
    // Filling entries included (orphaned: their filler finishes, nothing is counted). Entries are destroyed after the lock.
    void OnSeal(uint64_t seq)
    {
        std::vector<std::shared_ptr<Entry>> drop;
        {
            std::lock_guard<std::mutex> g(mutex_);
            if (haveSealed_ && seq < lastSeal_ && !numberingMayRestart_) {
                ReleaseIfLocked([](const Entry&) { return true; }, drop);   // the old numbering: nothing kept, nothing refused
                haveReleased_ = false;
                releasedThrough_ = 0;
            }
            numberingMayRestart_ = false;   // after a BumpEpoch the older epoch's spots go below; this epoch's fills stay
            haveSealed_ = true;
            lastSeal_ = seq;
            ++seals_;
            ReleaseIfLocked([](const Entry& e) { return e.frameScoped; }, drop);   // the previous render frame's views
            {
                const uint64_t ep = epoch_;
                ReleaseIfLocked([ep](const Entry& e) { return e.epoch != ep; }, drop);   // spots of an older epoch (BumpEpoch)
                bumpedThisInterval_ = false;
                for (auto& s : stale_)
                    drop.push_back(s);
                stale_.clear();
            }
            // A frame has left the queue below the edge (design s2.3): f < min(seal - back, presented - history).
            const uint64_t edge = EdgeLocked(seq, 0);
            if (edge > 0) {
                ReleaseIfLocked([this, seq](const Entry& e) { return e.seq < EdgeLocked(seq, e.keep); }, drop);
                if (!haveReleased_ || edge - 1u > releasedThrough_)
                    releasedThrough_ = edge - 1u;
                haveReleased_ = true;
            }
            RetireCompletedLocked(drop);
        }
        drop.clear();
    }
    // A discontinuity (graph rebuild, resync): every memo released; frames are accepted again from any seq.
    void Clear()
    {
        std::vector<std::shared_ptr<Entry>> drop;
        {
            std::lock_guard<std::mutex> g(mutex_);
            ReleaseIfLocked([](const Entry&) { return true; }, drop);
            RetireCompletedLocked(drop);
            fillsPerFrame_.clear();   // a new graph measures its own occupancy (r2 MUST-1)
            haveReleased_ = false;
            releasedThrough_ = 0;
            haveSealed_ = false;
        }
        drop.clear();
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
    size_t RetiredPending() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return retired_.size();
    }
    uint64_t GpuBytes() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return gpuBytes_;
    }
    uint64_t CpuBytes() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return cpuBytes_;
    }
    // One line per product asked: "skeleton acquire: p1_half_luma 12 reused (lane 10, memo 2), 1 filled at read, 0 unavailable".
    std::vector<std::string> StatusLines() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        std::vector<std::string> lines;
        if (gpuForgotten_ != 0)
            lines.push_back("skeleton acquire: " + std::to_string(gpuForgotten_) + " owner GPU texture(s) (" + std::to_string(gpuForgottenBytes_)
                            + " bytes) were still pinned when the GPU device went: left to the device's teardown");
        for (const auto& kv : counters_) {
            const AcquireCounters& c = kv.second;
            std::string l = "skeleton acquire: " + NameOfLocked(kv.first) + " " + std::to_string(c.reusedLane + c.reusedMemo)
                            + " reused (lane " + std::to_string(c.reusedLane) + ", memo " + std::to_string(c.reusedMemo) + "), "
                            + std::to_string(c.filledAtRead) + " filled at read, " + std::to_string(c.Unavailable())
                            + " unavailable";
            if (c.planMiss != 0)
                l += "; plan miss " + std::to_string(c.planMiss);
            if (c.gpuAllocMade != 0 || c.gpuAllocRefused != 0)
                l += "; gpu textures made " + std::to_string(c.gpuAllocMade) + " (" + std::to_string(c.gpuAllocBytes)
                     + " bytes), refused " + std::to_string(c.gpuAllocRefused);
            if (c.aheadFilled != 0 || c.aheadReused != 0 || c.invalidatedRecompute != 0 || c.discarded != 0)
                l += "; ahead fills " + std::to_string(c.aheadFilled) + ", ahead reused " + std::to_string(c.aheadReused)
                     + ", recomputed after invalidation " + std::to_string(c.invalidatedRecompute) + ", discarded "
                     + std::to_string(c.discarded);
            if (c.sealComputed != 0 || c.sealReused != 0 || c.reusedLaneCopy != 0)
                l += "; seal computed " + std::to_string(c.sealComputed) + ", seal reused " + std::to_string(c.sealReused)
                     + ", copied off thread " + std::to_string(c.reusedLaneCopy);
            if (c.laneAheadMade != 0 || c.sealComputedReduced != 0 || c.workerReused != 0)
                l += "; lane frames: made ahead " + std::to_string(c.laneAheadMade) + ", seal computed reduced "
                     + std::to_string(c.sealComputedReduced) + ", worker reused " + std::to_string(c.workerReused);
            for (size_t k = 1; k < size_t(AcquireCause::CauseCount); ++k)
                if (c.unavailable[k] != 0)
                    l += std::string("; ") + Token(AcquireCause(k)) + " " + std::to_string(c.unavailable[k]);
            lines.push_back(l);
        }
        if (planSet_ && !plan_.line.empty())
            lines.push_back("skeleton acquire: " + plan_.line);
        if (windowSet_)
            lines.push_back("skeleton acquire: window back " + std::to_string(keepFrames_) + ", ahead " + std::to_string(ahead_)
                            + ", history " + std::to_string(history_) + ", frames " + std::to_string(window_.Frames()) + ", epoch "
                            + std::to_string(epoch_) + (presentKnown_ ? ", presented " + std::to_string(presented_) : ", present facts not fed")
                            + (window_.wired != QueueWindowTerms::kAll ? "; not fed: " + NotFed(window_.wired) : std::string())
                            + "; " + AheadComputeText(window_));
        {
            std::string inv;
            for (size_t k = 0; k < size_t(InvalidateCause::Count); ++k)
                if (invalidations_[k] != 0)
                    inv += std::string(inv.empty() ? "" : ", ") + InvalidateToken(InvalidateCause(k)) + " " + std::to_string(invalidations_[k]);
            if (!inv.empty())
                lines.push_back("skeleton acquire: invalidated (" + inv + ")");
        }
        if (!retired_.empty())
            lines.push_back("skeleton acquire: " + std::to_string(retired_.size()) + " released GPU spot(s) held for a pending read fence"
                            + (fenceCompleted_ ? "" : " (no fence completion source: held until teardown)"));
        {
            std::string reserved;
            for (ProductId r : kReservedProductIds)
                reserved += (reserved.empty() ? "" : ", ") + std::to_string(unsigned(r)) + " " + ReservedProductName(r);
            lines.push_back("skeleton acquire: reserved product ids (no owner yet): " + reserved);
        }
        if (refusals_ != 0)
            lines.push_back("skeleton acquire: owner registrations refused " + std::to_string(refusals_) + " (" + refusalText_ + ")");
        return lines;
    }
    // G5: one engaged Fallbacks entry per product whose last Acquire was Unavailable ("skeleton acquire <token>": cause, frame);
    // released when it is served again. The skeleton adds them to its Fallbacks list (Ctrl+I, Config status, Ctrl+Alt+D).
    std::vector<FallbackCatalog::Decision> Decisions() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        std::vector<FallbackCatalog::Decision> out;
        for (const auto& kv : lastCause_) {
            FallbackCatalog::Decision d;
            d.id = kFallbackPrefix + NameOfLocked(kv.first);
            d.engaged = true;
            d.reason = kv.second;
            out.push_back(d);
        }
        return out;
    }
    // The current Unavailable cause text of a product ("" = served / never asked).
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
        ProductId id = kNoProduct;
        uint64_t seq = 0;
        bool gpuForm = false;
        bool orphaned = false;     // released from the memo (while filling or after): its bytes are no longer counted
        bool frameScoped = false;  // OwnerFill::frameScoped: released at the next OnSeal
        uint64_t epoch = 0;        // the memo epoch it was filled in (an older one is absent: BumpEpoch)
        uint8_t keep = 0;          // OwnerFill::DeclaredHistory()
        bool ahead = false;        // filled while its frame was ahead of the render frame
        bool aheadReuseCounted = false;
        uint64_t reserved = 0;     // GPU bytes counted in gpuBytes_
        uint64_t readFence = 0;    // NoteGpuRead
        std::thread::id filler;    // the thread running the fill (cycle detection while Filling)
        std::vector<uint8_t> bytes;
        GpuMemoHandle gpu;
    };

    static uint32_t OwnerKey(ProductId id, AcquireForm f) { return (uint32_t(id) << 1) | (f == AcquireForm::Gpu ? 1u : 0u); }

    // Waiting for `e` (Filling) would close a cycle: following "the filler of e waits for the fill of ..." reaches this thread.
    bool WaitWouldCycleLocked(const std::shared_ptr<Entry>& e) const
    {
        const std::thread::id self = std::this_thread::get_id();
        const Entry* cur = e.get();
        for (size_t hops = 0; cur != nullptr && cur->state == Entry::Filling && hops <= waitingOn_.size(); ++hops) {
            if (cur->filler == self)
                return true;
            const auto w = waitingOn_.find(cur->filler);
            if (w == waitingOn_.end())
                return false;
            cur = w->second.get();
        }
        return false;
    }

    struct FillingKey {
        const SkeletonMemo* memo;
        Key key;
    };
    // The fills this thread runs, by memo instance (G0 audit S2: one memo's fill asking another memo is not re-entrant).
    static std::vector<FillingKey>& FillingOnThisThread()
    {
        static thread_local std::vector<FillingKey> keys;
        return keys;
    }
    // The release edge at seal `seq` for a spot declaring `entryKeep` history frames: frames below it have left the queue.
    uint64_t EdgeLocked(uint64_t seq, uint8_t entryKeep) const
    {
        const uint64_t back = keepFrames_ > entryKeep ? keepFrames_ : entryKeep;
        uint64_t edge = seq >= back ? seq - back : 0u;
        if (presentKnown_) {
            const uint64_t hist = history_ > entryKeep ? history_ : entryKeep;
            const uint64_t pe = presented_ >= hist ? presented_ - hist : 0u;
            if (pe < edge)
                edge = pe;   // a frame not yet presented (minus its history readers) is still in the queue
        }
        return edge;
    }
    // P16-LATCH-EVIDENCE: is frame `seq` released FOR THIS OWNER? OnSeal keeps an owner's spots down to EdgeLocked(seal, its
    // DeclaredHistory()), so a frame inside that window that no reader has filled yet is not released for the owner either: it
    // fills once at read (a lagging reader inside the declared window, e.g. the source-resolution monitor's worker), and a frame
    // below the owner's edge is refused (FrameReleased), never recomputed. An owner declaring no history uses the memo's line
    // unchanged; the owner's edge is never above it, so this only ever serves frames the owner's own spots would still hold.
    bool ReleasedForOwnerLocked(uint64_t seq, const OwnerFill* ow) const
    {
        if (!haveReleased_ || seq > releasedThrough_)
            return false;
        const uint8_t keep = ow != nullptr ? ow->DeclaredHistory() : uint8_t(0);
        if (keep == 0u || !haveSealed_)
            return true;
        return seq < EdgeLocked(lastSeal_, keep);
    }
    const OwnerFill* OwnerLocked(uint32_t k) const
    {
        const auto l = laneOwners_.find(k);
        if (l != laneOwners_.end())
            return &l->second;
        const auto m = owners_.find(k);
        return m != owners_.end() ? &m->second : nullptr;
    }
    // Every registered owner fill, ordered by owner key; the bound lane's fill shadows a module registration with the same key
    // (the same precedence as OwnerLocked). The one merge used by AheadNeedLocked, DeclaredFillHistory and ActiveAheadDemands.
    // Caller holds mutex_.
    std::map<uint32_t, const OwnerFill*> MergedOwnersLocked() const
    {
        std::map<uint32_t, const OwnerFill*> all;
        for (const auto& kv : owners_)
            all[kv.first] = &kv.second;
        for (const auto& kv : laneOwners_)
            all[kv.first] = &kv.second;   // the bound lane's fill shadows a module registration
        return all;
    }
    bool OnRenderThreadLocked() const
    {
        return renderThread_ == std::thread::id() || renderThread_ == std::this_thread::get_id();
    }

    Registration Register(ProductId id, AcquireForm form, const std::string& unit, OwnerFill fill, bool replace)
    {
        std::lock_guard<std::mutex> g(mutex_);
        const std::string owner = OwnerUnitOf(id);
        const std::string what = NameOfLocked(id) + " (" + Token(form) + ") by " + unit;
        if (owner.empty()) {
            ++refusals_;
            refusalText_ = what + ": not in the product catalog";
            return Registration::NotInCatalog;
        }
        if (unit != owner) {
            ++refusals_;
            refusalText_ = what + ": owned by " + owner;
            return Registration::NotOwner;
        }
        if (fill.defaults.stage != 0) {
            ++refusals_;
            refusalText_ = what + ": an owner's own form is the source planes (stage 0)";
            return Registration::BadDefaults;
        }
        const uint32_t k = OwnerKey(id, form);
        const bool had = owners_.count(k) != 0;
        if (had && !replace) {
            ++refusals_;
            refusalText_ = what + ": already registered (replace explicitly)";
            return Registration::AlreadyRegistered;
        }
        owners_[k] = std::move(fill);
        return had ? Registration::Replaced : Registration::Registered;
    }

    static AcquireResult ResultOf(const std::shared_ptr<Entry>& e, AcquireStatus st)
    {
        AcquireResult r;
        r.status = st;
        r.source = AcquireSource::Memo;
        if (!e->bytes.empty()) {
            r.cpu = e->bytes.data();
            r.cpuBytes = e->bytes.size();
        }
        r.gpu = e->gpu;
        r.pin = std::shared_ptr<const void>(e, e.get());
        return r;
    }

    std::string NameOfLocked(ProductId id) const
    {
        const auto it = names_.find(id);
        if (it == names_.end() && IsReservedProduct(id))
            return std::string(ReservedProductName(id));   // the plain token: also the stable Fallbacks id suffix
        return it != names_.end() ? it->second : "product " + std::to_string(id);
    }

    AcquireResult Fail(AcquireCounters& c, ProductId id, uint64_t seq, AcquireCause cause)
    {
        ++c.unavailable[size_t(cause)];
        lastCause_[id] = std::string(Token(cause)) + ", frame " + std::to_string(seq);
        AcquireResult r;
        r.status = AcquireStatus::Unavailable;
        r.cause = cause;
        return r;
    }
    void Served(ProductId id) { lastCause_.erase(id); }

    // Release the entries `pred` names. GPU spots under an incomplete read fence go to retired_; the rest to `drop`.
    template <class Pred>
    void ReleaseIfLocked(Pred pred, std::vector<std::shared_ptr<Entry>>& drop)
    {
        for (auto it = memo_.begin(); it != memo_.end();) {
            Entry& e = *it->second;
            if (!pred(e)) {
                ++it;
                continue;
            }
            if (e.state == Entry::Ready)
                ++counters_[e.id].released;
            if (e.gpuForm)
                gpuBytes_ -= e.reserved;
            else
                cpuBytes_ -= e.bytes.size();
            e.reserved = 0;
            e.orphaned = true;
            if (e.gpuForm && e.state == Entry::Ready && e.readFence > (fenceCompleted_ ? fenceCompleted_() : 0u))
                retired_.push_back(it->second);
            else
                drop.push_back(it->second);
            it = memo_.erase(it);
        }
    }
    void RetireCompletedLocked(std::vector<std::shared_ptr<Entry>>& drop)
    {
        const uint64_t done = fenceCompleted_ ? fenceCompleted_() : 0u;   // no completion source: a noted read is never assumed done
        for (auto it = retired_.begin(); it != retired_.end();) {
            if ((*it)->readFence <= done) {
                drop.push_back(*it);
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<uint32_t, OwnerFill> owners_;        // module registrations
    std::map<uint32_t, OwnerFill> laneOwners_;    // the bound lane's fills (precedence while bound)
    std::thread::id renderThread_;                // none = every thread counts as the render thread
    uint64_t laneBinds_ = 0;
    uint64_t seals_ = 0;
    // The queue window (SetWindow; design s2.3).
    bool windowSet_ = false;
    uint16_t ahead_ = 0;
    uint16_t history_ = 0;
    bool presentKnown_ = false;
    bool presentNoted_ = false;   // NotePresented (the AfterPresent feed) delivered present facts: they persist across windows
    uint64_t presented_ = 0;
    QueueWindow window_;
    uint64_t epoch_ = 0;
    uint64_t discontinuities_ = 0;
    bool bumpedThisInterval_ = false;
    std::vector<std::shared_ptr<Entry>> stale_;   // old-epoch spots taken out by an Acquire: destroyed at the next seal
    uint64_t invalidations_[size_t(InvalidateCause::Count)] = {};
    std::set<Key> invalidatedKeys_;
    std::map<uint32_t, uint64_t> measuredBytes_;   // the largest result size per (product, form)
    // Per (product, form): the most GPU results of one seq held in the memo at once (counted at each fill from the live entries),
    // i.e. the fills one render frame holds. Reset at Clear() and BumpEpoch: a new graph / epoch measures its own occupancy.
    std::map<uint32_t, uint32_t> fillsPerFrame_;
    AheadPlan plan_;
    bool planSet_ = false;
    const void* host_ = nullptr;
    std::map<Key, std::shared_ptr<Entry>> memo_;
    std::vector<std::shared_ptr<Entry>> retired_;
    std::map<std::thread::id, std::shared_ptr<Entry>> waitingOn_;   // a thread -> the Filling entry it waits for (cycle detection)
    std::map<ProductId, AcquireCounters> counters_;
    std::map<ProductId, std::string> names_;
    std::map<ProductId, std::string> lastCause_;
    LaneSlotSource lane_;
    std::function<uint64_t()> fenceCompleted_;
    bool gpuPresent_ = false;
    IOwnerGpuDevice* gpuDevice_ = nullptr;   // R2: the sanctioned GPU context's device (SetGpuDevice)
    uint64_t gpuForgotten_ = 0, gpuForgottenBytes_ = 0;   // R2 audit S1: pinned owner textures left to the device's teardown
    bool gpuGranted_ = false;
    uint64_t gpuBudget_ = 0;
    uint64_t gpuBytes_ = 0;
    uint64_t cpuBytes_ = 0;
    uint32_t keepFrames_ = 0;
    uint64_t refusals_ = 0;
    std::string refusalText_;
    bool haveReleased_ = false;
    uint64_t releasedThrough_ = 0;
    bool haveSealed_ = false;
    uint64_t lastSeal_ = 0;
    bool numberingMayRestart_ = false;   // a BumpEpoch since the last seal (the released numbering was reset)
};

}  // namespace VpSkeleton
