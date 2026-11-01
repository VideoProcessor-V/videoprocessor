// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Indicator.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// FrameSeal (resync Q0-B, plan 50 gap G3 / G5): the ONE owner of the central Indicator's seal.
//
// Before: the resync Drive and the analysis lane each sealed (SealAndObserve) the same Indicator, and
// Indicator::Register refuses readers after the first Observe, so a second sealer or a late provider
// broke the first. The lane also held a raw pointer that dangled across an era swap.
//
// Now one render-thread object holds the shared_ptr<Indicator> the host (Shadow::GetIndicator) hands it
// each frame. On a NEW era (a different Indicator: Shadow::Configure built one) it, before the first
// Observe of that Indicator:
//   1. calls every PROVIDER's registerOn (the resync Drive's readers, the R2 motion readers, the lane's
//      handle rebind, the early-upload placeholder, ...), in registration order;
// and then, for every frame, seals and observes ONCE and fans (seq, changed) out to the SUBSCRIBERS in
// subscription order (Drive latch, lane regulator, ...). Nothing else may seal this Indicator.
//
// Render thread only (Seal, AddProvider, Subscribe): setup before frames flow. Header-only, C++14.
// Threads outside the render thread may read the counters.
namespace VpSeal {

class FrameSeal {
public:
    using Changed = VpIndicator::CounterMask;
    // Registers readers on a new-era Indicator. Called before that Indicator's first Observe.
    using RegisterFn = std::function<void(const std::shared_ptr<VpIndicator::Indicator>&)>;
    // Told about each sealed frame. `changed` is the Indicator's SealAndObserve result.
    using SubscriberFn = std::function<void(const std::shared_ptr<VpIndicator::Indicator>&, uint64_t seq, Changed changed)>;

    // Setup. A provider added after the first Seal is called at the next new era (never retroactively on
    // an Indicator that has observed): it is counted in LateProviders() and shown in Describe().
    void AddProvider(const char* name, RegisterFn registerOn)
    {
        Provider p;
        p.name = name != nullptr ? name : "";
        p.registerOn = std::move(registerOn);
        if (m_sealed)
            ++m_lateProviders;
        m_providers.push_back(std::move(p));
    }
    void Subscribe(const char* name, SubscriberFn fn)
    {
        Subscriber s;
        s.name = name != nullptr ? name : "";
        s.fn = std::move(fn);
        m_subscribers.push_back(std::move(s));
    }

    // Render thread, once per frame, in sequence order. Returns the changed mask (0 without an
    // Indicator or for seq 0, which is not a valid sequence: nothing is sealed or fanned out).
    Changed Seal(const std::shared_ptr<VpIndicator::Indicator>& indicator, uint64_t seq)
    {
        if (!indicator || seq == 0)
            return 0;
        if (indicator != m_current) {
            m_current = indicator;   // also keeps the Indicator alive for the subscribers' frames
            ++m_eras;
            for (const Provider& p : m_providers)
                if (p.registerOn)
                    p.registerOn(indicator);
        }
        m_sealed = true;
        const Changed changed = indicator->SealAndObserve(seq);
        ++m_seals;
        m_lastSeq = seq;
        for (const Subscriber& s : m_subscribers)
            if (s.fn)
                s.fn(indicator, seq, changed);
        return changed;
    }

    // The Indicator readers were last registered on (null before the first frame).
    const std::shared_ptr<VpIndicator::Indicator>& Current() const { return m_current; }

    uint64_t Seals() const { return m_seals; }
    uint32_t Eras() const { return m_eras; }
    uint32_t LateProviders() const { return m_lateProviders; }
    uint64_t LastSeq() const { return m_lastSeq; }
    size_t ProviderCount() const { return m_providers.size(); }
    size_t SubscriberCount() const { return m_subscribers.size(); }

    // Status text for Ctrl+Alt+D / Config status ("one seal owner" evidence).
    std::string Describe() const
    {
        std::string text = "seal: " + std::to_string(m_seals) + " frames, " + std::to_string(m_eras) + " era(s), " +
                           std::to_string(m_providers.size()) + " providers, " + std::to_string(m_subscribers.size()) +
                           " subscribers";
        if (m_lateProviders != 0)
            text += "; " + std::to_string(m_lateProviders) + " provider(s) added after the first frame (not registered on this era)";
        return text;
    }

private:
    struct Provider {
        std::string name;
        RegisterFn registerOn;
    };
    struct Subscriber {
        std::string name;
        SubscriberFn fn;
    };

    std::vector<Provider> m_providers;
    std::vector<Subscriber> m_subscribers;
    std::shared_ptr<VpIndicator::Indicator> m_current;
    bool m_sealed = false;
    uint64_t m_seals = 0;
    uint64_t m_lastSeq = 0;
    uint32_t m_eras = 0;
    uint32_t m_lateProviders = 0;
};

} // namespace VpSeal
