// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "FallbackCatalog.h"
#include "ProductCatalog.h"
#include "Products.h"

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
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
// A request for a released frame is refused (FrameReleased), never silently recomputed. A result carries a PIN: the bytes /
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

// The PRECISION axis values (L-CORE, advisor A6 + coordinator 2026-11-14 05:11; 00-UNIVERSAL rule 7 "12-BIT END TO END"):
//   kPrecisionDefault  the owner's own form;
//   kPrecisionUnorm16  R16 unorm at FULL 16-bit scale (msb-aligned code: 10-bit << 6, 12-bit << 4); exact for sources <= 14
//                      bits including a 2x2 mean (2 extra fraction bits);
//   kPrecisionFloat32  R32F in the plane's normalised units (0..1 as sampled);
//   kPrecisionFloat16  R16F: an 11-bit mantissa, NOT 12-bit safe; never for pixel values (OwnerFill::pixelData refuses it).
enum : uint8_t { kPrecisionDefault = 0, kPrecisionUnorm16 = 1, kPrecisionFloat32 = 2, kPrecisionFloat16 = 3 };

inline const char* PrecisionName(uint8_t p)
{
    switch (p) {
    case kPrecisionDefault: return "default";
    case kPrecisionUnorm16: return "unorm16";
    case kPrecisionFloat32: return "float32";
    case kPrecisionFloat16: return "float16";
    }
    return "?";
}
// True when a pixel value stored at precision `p` keeps every code of a 12-bit pipeline (a 2x2 mean included).
inline bool PrecisionTwelveBitSafe(uint8_t p) { return p == kPrecisionUnorm16 || p == kPrecisionFloat32; }

// The variant axes (refs/126 s1.2; L-CORE adds STAGE, CROP, PARAM0/1 and the precision values). A zero field is "the
// product's default"; VariantKey{} is the default variant (the lane's form). Keys are CANONICAL before use (Canonical(): a
// field equal to the owner's declared default is 0, a size with one zero side is no size, a crop with a zero side or equal to
// the owner's full frame is no crop), so two spellings of one variant share one spot. Contract: lead8/iface-L-CORE.md s4.
struct VariantKey {
    uint8_t plane = 0;         // 0 = default plane (luma); 1.. = plane index + 1
    uint32_t width = 0;        // 0 = the product's own size
    uint32_t height = 0;
    uint8_t scale = 0;         // GrainScale / pyramid level, 0 = default
    uint8_t codeUnit = 0;      // code-unit form (bits), 0 = default
    uint32_t sigmaMilli = 0;   // band sigma x 1000, 0 = default
    uint8_t precision = 0;     // storage precision: kPrecision* above, 0 = default
    // STAGE (coordinator ruling iface-L-B3 5b, OD 11-14 04:44): the pipeline position of the planes the variant is computed on.
    // 0 = the SOURCE planes (as uploaded, before any slot) and every owner's default; StageAfter(slot) = the planes as they
    // LEAVE pipeline slot `slot` (core/PipelineSlots.h Id: a stable identity, so a reorder of the slots keeps the key).
    uint8_t stage = 0;
    // CROP (OD 11-14 04:30, 04:44): a rectangle of the stage's planes in source pixels; cropW == 0 || cropH == 0 = none.
    uint32_t cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    // PARAM0 / PARAM1 (lead 8 2026-11-14 05:2x for L-B5): generic owner-defined thresholds (e.g. P11 busy share lap_min / ext_min
    // in 1/16 tap code). The owner documents their meaning in its catalog row / interface.
    uint16_t param0 = 0, param1 = 0;

    enum : uint32_t {
        kAxisPlane = 1u << 0,
        kAxisSize = 1u << 1,
        kAxisScale = 1u << 2,
        kAxisCodeUnit = 1u << 3,
        kAxisSigma = 1u << 4,
        kAxisPrecision = 1u << 5,
        kAxisStage = 1u << 6,
        kAxisCrop = 1u << 7,
        kAxisParam0 = 1u << 8,
        kAxisParam1 = 1u << 9
    };

    static uint8_t StageAfter(uint8_t pipelineSlotId) { return uint8_t(pipelineSlotId + 1u); }
    // The pipeline slot whose output `stage` names (false = the source planes).
    static bool StageSlot(uint8_t stage, uint8_t* slotId)
    {
        if (stage == 0)
            return false;
        if (slotId != nullptr)
            *slotId = uint8_t(stage - 1u);
        return true;
    }
    bool HasCrop() const { return cropW != 0 && cropH != 0; }

    bool IsDefault() const { return Axes() == 0u; }
    uint32_t Axes() const
    {
        return (plane ? uint32_t(kAxisPlane) : 0u) | (width && height ? uint32_t(kAxisSize) : 0u) | (scale ? uint32_t(kAxisScale) : 0u)
               | (codeUnit ? uint32_t(kAxisCodeUnit) : 0u) | (sigmaMilli ? uint32_t(kAxisSigma) : 0u)
               | (precision ? uint32_t(kAxisPrecision) : 0u) | (stage ? uint32_t(kAxisStage) : 0u)
               | (HasCrop() ? uint32_t(kAxisCrop) : 0u) | (param0 ? uint32_t(kAxisParam0) : 0u) | (param1 ? uint32_t(kAxisParam1) : 0u);
    }
    // `defaults`: the owner's own form (its plane / size / scale ...); a field equal to it means "the default". The owner's
    // full frame for the crop axis is defaults.width x defaults.height when given (a crop covering it is no crop).
    VariantKey Canonical(const VariantKey& defaults = VariantKey()) const
    {
        VariantKey k = *this;
        if (k.width == 0 || k.height == 0 || (k.width == defaults.width && k.height == defaults.height))
            k.width = k.height = 0;
        if (k.plane == defaults.plane) k.plane = 0;
        if (k.scale == defaults.scale) k.scale = 0;
        if (k.codeUnit == defaults.codeUnit) k.codeUnit = 0;
        if (k.sigmaMilli == defaults.sigmaMilli) k.sigmaMilli = 0;
        if (k.precision == defaults.precision) k.precision = 0;
        if (k.stage == defaults.stage) k.stage = 0;
        if (k.param0 == defaults.param0) k.param0 = 0;
        if (k.param1 == defaults.param1) k.param1 = 0;
        const bool fullFrame = defaults.width != 0 && defaults.height != 0 && k.cropX == 0 && k.cropY == 0
                               && k.cropW == defaults.width && k.cropH == defaults.height;
        const bool sameAsDefault = k.cropX == defaults.cropX && k.cropY == defaults.cropY && k.cropW == defaults.cropW
                                   && k.cropH == defaults.cropH;
        if (!k.HasCrop() || fullFrame || sameAsDefault)
            k.cropX = k.cropY = k.cropW = k.cropH = 0;
        return k;
    }
    // FNV-1a 64 over every field (call it on a canonical key): the stable id of a variant for counters, logs and the
    // compute-once detector (iface-L-DET VPV_COMPUTE_ONCE variant). 0 for the default variant.
    uint64_t Hash() const
    {
        if (IsDefault())
            return 0;
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v, int bytes) {
            for (int i = 0; i < bytes; ++i) {
                h ^= (v >> (8 * i)) & 0xFFu;
                h *= 1099511628211ull;
            }
        };
        mix(plane, 1); mix(width, 4); mix(height, 4); mix(scale, 1); mix(codeUnit, 1); mix(sigmaMilli, 4); mix(precision, 1);
        mix(stage, 1); mix(cropX, 4); mix(cropY, 4); mix(cropW, 4); mix(cropH, 4); mix(param0, 2); mix(param1, 2);
        return h == 0 ? 1u : h;
    }
    std::string Text() const
    {
        if (IsDefault())
            return "default";
        std::string t;
        auto add = [&t](const char* k, uint64_t v) { t += (t.empty() ? "" : ",") + std::string(k) + "=" + std::to_string(v); };
        if (plane) add("plane", plane);
        if (width && height) { add("w", width); add("h", height); }
        if (scale) add("scale", scale);
        if (codeUnit) add("code", codeUnit);
        if (sigmaMilli) add("sigma_milli", sigmaMilli);
        if (precision) t += (t.empty() ? "" : ",") + std::string("precision=") + PrecisionName(precision);
        if (stage) add("stage", stage);
        if (HasCrop()) { add("crop_x", cropX); add("crop_y", cropY); add("crop_w", cropW); add("crop_h", cropH); }
        if (param0) add("p0", param0);
        if (param1) add("p1", param1);
        return t;
    }
};

inline bool operator==(const VariantKey& a, const VariantKey& b)
{
    return a.plane == b.plane && a.width == b.width && a.height == b.height && a.scale == b.scale && a.codeUnit == b.codeUnit
           && a.sigmaMilli == b.sigmaMilli && a.precision == b.precision && a.stage == b.stage && a.cropX == b.cropX
           && a.cropY == b.cropY && a.cropW == b.cropW && a.cropH == b.cropH && a.param0 == b.param0 && a.param1 == b.param1;
}
inline bool operator!=(const VariantKey& a, const VariantKey& b) { return !(a == b); }
inline bool operator<(const VariantKey& a, const VariantKey& b)
{
    return std::make_tuple(a.plane, a.width, a.height, a.scale, a.codeUnit, a.sigmaMilli, a.precision, a.stage, a.cropX, a.cropY,
                           a.cropW, a.cropH, a.param0, a.param1)
           < std::make_tuple(b.plane, b.width, b.height, b.scale, b.codeUnit, b.sigmaMilli, b.precision, b.stage, b.cropX, b.cropY,
                             b.cropW, b.cropH, b.param0, b.param1);
}

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
};
// What it produces: the CPU form (`bytes`) or the GPU form (`gpu`).
struct OwnerFillOutput {
    std::vector<uint8_t> bytes;
    GpuMemoHandle gpu;
    std::string declineReason;   // a declining fill's reason: shown with the cause (LastCause, Fallbacks entry)
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
    const void* token = nullptr;   // the bound lane (BindLane / UnbindLane by token)
};

struct AcquireCounters {
    uint64_t reusedLane = 0;
    uint64_t reusedMemo = 0;
    uint64_t filledAtRead = 0;
    uint64_t ownerFillCalls = 0;   // == filledAtRead + declined / over-budget fills: proves "computed once"
    uint64_t reusedLaneCopy = 0;   // off the render thread: the lane's published bytes copied out (no recompute)
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
    // first: nothing happens). Module registrations stay. True = unbound.
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
    // The memo's declared GPU working set: every GPU owner fill that ALLOCATES (not a view) x (1 + its kept frames). The
    // no-lane ledger grant is planned for it (MemoGpuGrant.h).
    uint64_t GpuNeedBytes() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        uint64_t need = 0;
        auto add = [&need](const std::map<uint32_t, OwnerFill>& m, const std::map<uint32_t, OwnerFill>* skip) {
            for (const auto& kv : m) {
                if ((kv.first & 1u) == 0u || kv.second.view)
                    continue;   // CPU form, or a view (0 bytes)
                if (skip != nullptr && skip->count(kv.first) != 0)
                    continue;   // shadowed by the bound lane's fill
                need += kv.second.reserveBytes * (1u + uint64_t(kv.second.keepFrames));
            }
        };
        add(laneOwners_, nullptr);
        add(owners_, &laneOwners_);
        return need;
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

    AcquireResult Acquire(ProductId id, uint64_t seq, AcquireForm form, const VariantKey& vIn = VariantKey())
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
        const auto it = memo_.find(key);
        if (it == memo_.end() && haveReleased_ && seq <= releasedThrough_)
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
                return ResultOf(e, AcquireStatus::Reused);
            }
            // Declined for this frame: the same cause, the owner is not asked twice for one frame.
            return Fail(c, id, seq, e->cause == AcquireCause::None ? AcquireCause::OwnerDeclined : e->cause);
        }

        if (renderThread && lane_.dropped && lane_.dropped(id))
            return Fail(c, id, seq, AcquireCause::RegulatorDropped);
        if (ow == nullptr || !ow->fill)
            return Fail(c, id, seq, AcquireCause::NoOwnerFill);
        if ((v.Axes() & ~ow->variantAxes) != 0u || (ow->pixelData && v.precision == kPrecisionFloat16))
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
        e->keep = ow->keepFrames;
        e->reserved = reserve;
        gpuBytes_ += reserve;
        memo_[key] = e;
        ++c.ownerFillCalls;
        const OwnerFill fill = *ow;   // a copy: a replacement / an unbind during the fill does not invalidate it
        lk.unlock();
        OwnerFillRequest req;
        req.host = host_;
        req.id = id;
        req.seq = seq;
        req.form = form;
        req.variant = v;
        OwnerFillOutput out;
        bool ok = false;
        FillingOnThisThread().push_back(FillingKey{ this, key });
        try {
            ok = fill.fill(req, *this, out);
        } catch (...) {
            ok = false;   // a throwing fill is a decline (counted, visible), never a stuck Filling entry
        }
        FillingOnThisThread().pop_back();
        if (ok && form == AcquireForm::Cpu && out.bytes.empty())
            ok = false;
        if (ok && form == AcquireForm::Gpu && !out.gpu.Valid())
            ok = false;
        if (ok && fill.view && out.gpu.bytes != 0)
            ok = false;   // a view allocates nothing: bytes would be outside the ledger
        lk.lock();
        AcquireCounters& c2 = counters_[id];
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
            const std::string reason = out.declineReason;
            lk.unlock();
            out = OwnerFillOutput();   // released outside the lock
            lk.lock();
            AcquireResult failed = Fail(counters_[id], id, seq, cause);
            if (!reason.empty())
                lastCause_[id] += " (" + reason + ")";
            return failed;
        }
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
            if (haveSealed_ && seq < lastSeal_) {
                ReleaseIfLocked([](const Entry&) { return true; }, drop);   // the old numbering: nothing kept, nothing refused
                haveReleased_ = false;
                releasedThrough_ = 0;
            }
            haveSealed_ = true;
            lastSeal_ = seq;
            ++seals_;
            ReleaseIfLocked([](const Entry& e) { return e.frameScoped; }, drop);   // the previous render frame's views
            if (seq >= 1u + keepFrames_) {
                const uint64_t through = seq - 1u - keepFrames_;
                const uint32_t memoKeep = keepFrames_;
                ReleaseIfLocked([seq, memoKeep](const Entry& e) {
                    const uint64_t keep = e.keep > memoKeep ? e.keep : memoKeep;   // OwnerFill::keepFrames
                    return seq >= 1u + keep && e.seq <= seq - 1u - keep;
                }, drop);
                if (!haveReleased_ || through > releasedThrough_)
                    releasedThrough_ = through;
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
        if (!retired_.empty())
            lines.push_back("skeleton acquire: " + std::to_string(retired_.size()) + " released GPU spot(s) held for a pending read fence"
                            + (fenceCompleted_ ? "" : " (no fence completion source: held until teardown)"));
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
        uint8_t keep = 0;          // OwnerFill::keepFrames
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
    const OwnerFill* OwnerLocked(uint32_t k) const
    {
        const auto l = laneOwners_.find(k);
        if (l != laneOwners_.end())
            return &l->second;
        const auto m = owners_.find(k);
        return m != owners_.end() ? &m->second : nullptr;
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
};

}  // namespace VpSkeleton
