// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ProductGraph.h"
#include "Products.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

// Skeleton S2: the card profile, its cache, the adaptive headroom and the system-RAM rules (design
// doc 25 B5, owner decisions doc 21 item 3). The tier grant and the one VRAM governor that admits
// everything are in VramGovernor.h. Pure C++14: the renderer measures (QueryVideoMemoryInfo, a copy
// and a compute probe, the NvOF caps query and its ms per pair) and passes the numbers in; nothing
// here touches a GPU. System RAM holds only items < 1 MB per frame, CPU-made items or rarely touched
// items, never a full-resolution per-frame plane; they are uploaded ahead (the upload cost is
// charged in the tier plan).
namespace VpSkeleton {

struct AdapterKey {
    uint64_t luid = 0;
    uint64_t driverVersion = 0;
};

inline bool operator==(const AdapterKey& a, const AdapterKey& b)
{
    return a.luid == b.luid && a.driverVersion == b.driverVersion;
}

struct CardProfile {
    AdapterKey key;
    // Live values (re-queried at each graph build): QueryVideoMemoryInfo, local segment.
    uint64_t vramBudgetBytes = 0;
    uint64_t vramUsageBytes = 0;
    uint64_t systemRamFreeBytes = 0;
    // Measured once per adapter + driver (cached).
    double copyGBps = 0.0;        // device-local copy bandwidth
    double uploadGBps = 0.0;      // system RAM -> VRAM
    double computeScale = 1.0;    // compute relative to the reference card (RTX 3050 Ti laptop = 1.0)
    bool ofaPresent = false;      // NvOF caps query
    double ofaMsPerPairRef = 0.0; // measured ms per pair at the reference size (1920x1080)
    bool measured = false;

    uint64_t FreeVram() const { return vramBudgetBytes > vramUsageBytes ? vramBudgetBytes - vramUsageBytes : 0; }
    // A stable identity for the graph's rebuild key.
    uint64_t Identity() const { return key.luid * 1099511628211ull ^ key.driverVersion; }
};

// Cached measurements per adapter LUID + driver version. A driver update is a new entry.
class CardProfileCache {
public:
    const CardProfile* Find(const AdapterKey& key) const
    {
        for (const CardProfile& p : entries_)
            if (p.key == key)
                return &p;
        return nullptr;
    }

    void Store(const CardProfile& p)
    {
        for (CardProfile& e : entries_)
            if (e.key == p.key) {
                e = p;
                return;
            }
        entries_.push_back(p);
    }

    // The cached measurements with fresh live values.
    bool WithLiveValues(const AdapterKey& key, uint64_t budget, uint64_t usage, uint64_t systemFree,
                        CardProfile* out) const
    {
        const CardProfile* p = Find(key);
        if (!p || !out)
            return false;
        *out = *p;
        out->vramBudgetBytes = budget;
        out->vramUsageBytes = usage;
        out->systemRamFreeBytes = systemFree;
        return true;
    }

    size_t Size() const { return entries_.size(); }

private:
    std::vector<CardProfile> entries_;
};

// Adaptive headroom on free VRAM: shrinks at once on memory pressure (budget exceeded, a paging
// sign), grows a small step only after a dwell of calm windows. Deterministic.
constexpr double kHeadroomMin = 0.50;
constexpr double kHeadroomMax = 0.90;
constexpr double kHeadroomStart = 0.80;
constexpr double kHeadroomShrink = 0.85;
constexpr double kHeadroomGrowStep = 0.02;
constexpr int kHeadroomDwellWindows = 4;

class Headroom {
public:
    double Value() const { return value_; }

    void OnPressure()
    {
        value_ = std::max(kHeadroomMin, value_ * kHeadroomShrink);
        calm_ = 0;
    }

    void OnCalmWindow()
    {
        if (++calm_ >= kHeadroomDwellWindows) {
            value_ = std::min(kHeadroomMax, value_ + kHeadroomGrowStep);
            calm_ = 0;
        }
    }

private:
    double value_ = kHeadroomStart;
    int calm_ = 0;
};

constexpr uint64_t kSystemRamItemLimit = 1024u * 1024u;   // 1 MB per frame (doc 21 item 3)

// System RAM is allowed for items < 1 MB per frame, CPU-made items or rarely touched items, and
// never for a full-resolution per-frame plane.
inline bool SystemRamAllowed(const ProductDecl& d, uint64_t bytesPerFrame)
{
    if (IsFullResPerFrame(d.size))
        return false;
    return bytesPerFrame < kSystemRamItemLimit || d.backend == Backend::Cpu || d.rarelyTouched;
}

inline Residency ResidencyFor(const ProductDecl& d, uint64_t bytesPerFrame)
{
    return d.residency == Residency::SystemRam && SystemRamAllowed(d, bytesPerFrame) ? Residency::SystemRam
                                                                                      : Residency::Vram;
}

} // namespace VpSkeleton
