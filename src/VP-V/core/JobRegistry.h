// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// S-3a S1 (lead9/brief-S-3a-scheduler.md s4 S1; refs/122 S1, N3; IC-2 core/SchedulerContract.h R3, R4, R9): the ONE registration path of a
// scheduled job. Every job IS a skeleton product fill (owner correction 2026-11-12T20:01:26Z [cf1d27fa]: "Anything shared is shared and anything
// needing deriving that is of use should have a skeleton spot"), so a job is registered by its JobDecl against the run-time product graph:
//  - graphOrder = the product's index in Graph::nodes (topological over lag-0 edges, ties by product id); a hand-set order is REFUSED (R3);
//  - the work's Id / Class / Order must equal its declaration (Id = product, Class = SpareClassOf(cls), Order = graphOrder);
//  - a Derived job sits after every lag-0 input in the graph (it runs right after its inputs; else it is Derived-live through Acquire, R3);
//  - one job per product per host (compute once): a second registration of a product is refused;
//  - CoreFirst only here; SharedGap (R10, the hidden test key) is S5's and is refused until it lands;
//  - Original pathway: nothing is registered (R9, byte-identical), counted;
//  - P13 as an Unpack job is refused: P13 is render-device VIEWS valid only until AfterRender of the frame they were acquired in
//    (lead8/iface-L-CORE.md s6) and the slot runs after the present, so it has nothing it may hold. No production Unpack job until the
//    queued-ahead source ring lands (coordinator 2026-11-14 08:22 via lead 10, Q1; 102b S3A-UNPACK-SLOT); the Unpack CLASS path is wired and
//    tested with a fake product;
//  - cpu = true jobs are not render-device work: they go to the CPU lane (LaneRunner) under the same graph order through ICpuJobSink (needed
//    by DVDM's "dv trims LUT prebuild-ahead"); the sink refuses a product without a registered lane producer.
// Every refusal is counted with its reason and shown on the status line (rule 5). Render thread (registration at Configure). Pure C++14.

#include <VP-V/core/ProductGraph.h>
#include <VP-V/core/SchedulerContract.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace VpSkeleton {
namespace Jobs {

constexpr uint32_t kNoGraphOrder = 0xFFFFFFFFu;

// The product's index in Graph::nodes, or kNoGraphOrder when the graph does not hold it.
inline uint32_t GraphOrderOf(const Graph& g, ProductId id)
{
    const int i = g.IndexOf(id);
    return i < 0 ? kNoGraphOrder : uint32_t(i);
}

inline const char* JobClassName(JobClass c)
{
    switch (c) {
    case JobClass::Unpack: return "unpack";
    case JobClass::Core: return "core";
    case JobClass::Derived: return "derived";
    case JobClass::Slice: return "slice";
    case JobClass::Fill: return "fill";
    }
    return "?";
}

// The CPU lane's side of a cpu job (the production sink adapts LaneRunner at wiring time: the CPU worker already runs lane producers in the
// graph's topological order, so the sink only confirms the product has a laned producer and records the declaration).
class ICpuJobSink {
public:
    virtual ~ICpuJobSink() {}
    virtual bool Accept(const JobDecl& decl, std::string* why) = 0;
};

// The stateless checks of one registration. True = the declaration is valid against the graph (and the work, when given).
inline bool Validate(const Graph& g, const JobDecl& d, const ISpareWork* w, ScheduleMode mode, std::string* why)
{
    auto fail = [why](const std::string& s) { if (why) *why = s; return false; };
    char buf[160] = {};
    if (mode != ScheduleMode::CoreFirst) return fail("shared_gap mode is not built yet (S5)");
    if (!g.valid) return fail("product graph not valid");
    const uint32_t order = GraphOrderOf(g, d.product);
    if (order == kNoGraphOrder) {
        std::snprintf(buf, sizeof(buf), "product %u not in the graph", unsigned(d.product));
        return fail(buf);
    }
    if (d.graphOrder != order) {
        std::snprintf(buf, sizeof(buf), "product %u: hand-set order %u refused (graph order %u)", unsigned(d.product), unsigned(d.graphOrder), unsigned(order));
        return fail(buf);
    }
    if (d.cls == JobClass::Unpack && d.product == kP13SourceRing)
        return fail("P13 unpack refused: source ring views end at AfterRender (no queued-ahead source ring yet, S3A-UNPACK-SLOT)");
    if (d.cls == JobClass::Derived) {
        const Node& n = g.nodes[order];
        for (uint8_t i = 0; i < n.inputCount; ++i) {
            const InputRef& in = n.inputs[i];
            if (in.lag != 0 || in.product == kNoProduct) continue;
            const uint32_t io = GraphOrderOf(g, in.product);
            if (io != kNoGraphOrder && io >= order) {
                std::snprintf(buf, sizeof(buf), "derived product %u is not after its input %u", unsigned(d.product), unsigned(in.product));
                return fail(buf);
            }
        }
    }
    if (w != nullptr) {
        if (w->Id() != d.product) return fail("work id differs from the declared product");
        if (w->Class() != SpareClassOf(d.cls)) return fail("work class differs from the declaration");
        if (w->Order() != d.graphOrder) return fail("work order differs from the graph order");
    }
    return true;
}

// The brief's interface form (one registration, no duplicate tracking: JobRegistry below adds that).
inline bool Register(SpareHost& host, const Graph& g, const JobDecl& d, ISpareWork* w, std::string* why)
{
    if (d.cpu) { if (why) *why = "cpu job: register through JobRegistry with a CPU sink"; return false; }
    if (w == nullptr) { if (why) *why = "no work"; return false; }
    if (!Validate(g, d, w, ScheduleMode::CoreFirst, why)) return false;
    host.Slot().Register(w);
    return true;
}

// The per-renderer registry: one job per product, the refusals counted, the status line.
class JobRegistry {
public:
    struct Entry {
        JobDecl decl;
        ISpareWork* work;
    };

    void SetMode(ScheduleMode m) { m_mode = m; }
    ScheduleMode Mode() const { return m_mode; }
    void SetOriginal(bool original) { m_original = original; }
    void SetCpuSink(ICpuJobSink* sink) { m_cpuSink = sink; }

    bool Register(SpareHost& host, const Graph& g, const JobDecl& d, ISpareWork* w, std::string* why = nullptr)
    {
        std::string local;
        std::string& r = why ? *why : local;
        r.clear();
        if (m_original) { ++m_originalRefused; r = "Original pathway: nothing registered"; return Refuse(r); }
        for (const Entry& e : m_entries)
            if (e.decl.product == d.product) {
                char buf[96] = {};
                std::snprintf(buf, sizeof(buf), "product %u already has a job (compute once)", unsigned(d.product));
                r = buf;
                return Refuse(r);
            }
        if (d.cpu) {
            if (!Validate(g, d, nullptr, m_mode, &r)) return Refuse(r);
            if (m_cpuSink == nullptr) { r = "cpu job: no CPU lane"; return Refuse(r); }
            if (!m_cpuSink->Accept(d, &r)) return Refuse(r);
            m_entries.push_back(Entry{ d, nullptr });
            ++m_cpuJobs;
            return true;
        }
        if (w == nullptr) { r = "no work"; return Refuse(r); }
        if (!Validate(g, d, w, m_mode, &r)) return Refuse(r);
        host.Slot().Register(w);
        m_entries.push_back(Entry{ d, w });
        return true;
    }

    // A Configure / graph rebuild / mode change: every job leaves the host (they register again from the new graph).
    void Clear(SpareHost& host)
    {
        for (const Entry& e : m_entries)
            if (e.work != nullptr) host.Slot().Unregister(e.work);
        m_entries.clear();
        m_cpuJobs = 0;
    }

    const std::vector<Entry>& Entries() const { return m_entries; }
    size_t Count(JobClass c) const
    {
        size_t n = 0;
        for (const Entry& e : m_entries) if (e.decl.cls == c) ++n;
        return n;
    }
    size_t CpuJobs() const { return m_cpuJobs; }
    unsigned long long Refused() const { return m_refused; }
    unsigned long long OriginalRefused() const { return m_originalRefused; }
    const std::string& LastRefusal() const { return m_lastRefusal; }

    // Ctrl+I / Config status / Ctrl+Alt+D (rule 5). The absent Unpack job is named (Q1).
    std::string StatusLine() const
    {
        char buf[320] = {};
        if (m_original) {
            std::snprintf(buf, sizeof(buf), "slot jobs: off (Original pathway: nothing registered)");
            return buf;
        }
        std::snprintf(buf, sizeof(buf),
            "slot jobs (%s): core %u, derived %u, slice %u, fill %u, cpu %u; unpack: none (source planes unpacked in the render until the "
            "queued-ahead source ring)%s%s",
            m_mode == ScheduleMode::CoreFirst ? "core first" : "shared gap", unsigned(Count(JobClass::Core)), unsigned(Count(JobClass::Derived)),
            unsigned(Count(JobClass::Slice)), unsigned(Count(JobClass::Fill)), unsigned(m_cpuJobs),
            m_refused > 0 ? "; refused: " : "", m_refused > 0 ? m_lastRefusal.c_str() : "");
        return buf;
    }

private:
    bool Refuse(const std::string& why)
    {
        ++m_refused;
        m_lastRefusal = why;
        return false;
    }

    std::vector<Entry> m_entries;
    ScheduleMode m_mode = ScheduleMode::CoreFirst;
    bool m_original = false;
    ICpuJobSink* m_cpuSink = nullptr;
    size_t m_cpuJobs = 0;
    unsigned long long m_refused = 0;
    unsigned long long m_originalRefused = 0;
    std::string m_lastRefusal;
};

}   // namespace Jobs
}   // namespace VpSkeleton
