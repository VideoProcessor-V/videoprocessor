// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ResyncCause.h>
#include <VP-V/modules/input/signal/DvFallbacks.h>
#include <VP-V/modules/input/signal/DvMetadataHold.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

// The Dolby Vision metadata family as a reader of the central Indicator (item 16). ONE reader,
// "dv-metadata", registered by a FrameSeal provider (RegisterOn) and called on the render thread at the
// seal of the frame that carries the change:
//
//   F  (FULL)            Relatch  drop the held RPU / L8 metadata and the tunnel detection; both are
//                                 re-learned from the next valid picture. The HDR inference epoch is KEPT:
//                                 a resync is not a signal epoch (HdrInference decides that itself).
//   M  (continuity)      Hold     MINOR: keep the last valid metadata; nothing is dropped
//   settings[output]     Relatch  an output-affecting settings change re-derives what the metadata feeds
//   timing / others                not read: ignored
//
// Like Drive's and the motion readers, it LATCHES on its first observe after a registration (a new
// Indicator era, or a re-registration): the Indicator reports every counter then, and the reader acts
// only on what changed AT that frame, never on the era's history, so a new era does not apply a spurious
// FULL. It acts exactly once per carrying frame (a repeated callback for the same or an older frame is
// counted in Duplicates() and ignored).
//
// The reader is generic: the host binds hooks (OnRelatch, OnHold) to its DV holders and/or takes the
// pending flags with Take(). MetadataHolder below is the concrete adapter around the real
// DvMetadataHold::Tracker, used by the tests and as the pattern for the formatter's call site.
//
// Header-only, C++14. Render thread only (callbacks, Take, counters). The Reader must outlive the
// Indicator's callbacks (they hold its address).
namespace VpDvResync {

using VpIndicator::Bit;
using VpIndicator::CounterMask;
using VpIndicator::Policy;

struct ReaderSpec {
    const char* name;
    CounterMask reads;
    std::array<Policy, VpIndicator::kCounterCount> policies;
};

inline const ReaderSpec& Spec()
{
    using namespace VpIndicator;
    static const ReaderSpec spec = {
        "dv-metadata",
        Bit(kFull) | Bit(kContinuity) | Bit(kSettingsOutput),
        [] {
            std::array<Policy, kCounterCount> p{};
            p[kFull] = Policy::Relatch;
            p[kContinuity] = Policy::Hold;
            p[kSettingsOutput] = Policy::Relatch;
            return p;
        }() };
    return spec;
}

class Reader {
public:
    static constexpr const char* kName = "dv-metadata";

    enum class Action : uint8_t { None, Relatch, Hold };

    struct Act {
        uint64_t seq = 0;            // the carrying frame
        CounterMask counters = 0;    // the relevant counters that changed AT that frame
        Action action = Action::None;
    };
    using Hook = std::function<void(const Act&)>;

    // What the host takes at its apply point (cleared by Take).
    struct Pending {
        bool relatch = false;
        bool hold = false;
        uint64_t seq = 0;            // the newest carrying frame
        CounterMask counters = 0;
        uint32_t relatches = 0;      // acts since the last Take
        uint32_t holds = 0;
    };

    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Host setup: called inside the callback, on the render thread, at the seal of the carrying frame.
    void SetHooks(Hook onRelatch, Hook onHold)
    {
        m_onRelatch = std::move(onRelatch);
        m_onHold = std::move(onHold);
    }
    // Told about every act (name, carrying seq): feeds ResyncEventMetrics::OnReaderAct.
    void SetActObserver(std::function<void(const char*, uint64_t)> observer) { m_observer = std::move(observer); }

    // FrameSeal provider entry (before the Indicator's first Observe). A no-op for the Indicator already
    // registered; a different Indicator is a NEW ERA: the latch re-arms. Pending flags the host has not
    // taken yet are KEPT (a relatch owed from the old era is still owed).
    // An Indicator that refused the registration (already observed) leaves ReaderId() == -1, never acts,
    // and says so in StatusText.
    void RegisterOn(const std::shared_ptr<VpIndicator::Indicator>& indicator)
    {
        if (!indicator)
            return;
        if (indicator == m_keepAlive)
            return;
        m_keepAlive = indicator;
        RegisterOn(*indicator);
    }

    void RegisterOn(VpIndicator::Indicator& indicator)
    {
        if (m_haveKey && m_keyAddress == &indicator && m_keyEra == indicator.Era())
            return;
        m_haveKey = true;
        m_keyAddress = &indicator;
        m_keyEra = indicator.Era();
        m_latched = false;
        m_haveAct = false;
        m_lastActSeq = 0;
        m_latchBaseline = indicator.LatchBaseline();
        ++m_eras;
        const ReaderSpec& spec = Spec();
        VpIndicator::Indicator* ind = &indicator;
        m_id = indicator.Register(spec.name, spec.reads, spec.policies,
            [this, ind](const VpIndicator::Indicator::Change& c) { OnChange(*ind, c); });
    }

    // The flags the reader set since the last Take (then cleared).
    Pending Take()
    {
        const Pending p = m_pending;
        m_pending = Pending();
        return p;
    }
    const Pending& Peek() const { return m_pending; }

    int ReaderId() const { return m_id; }
    uint64_t Calls() const { return m_calls; }            // all callbacks, including the latching one
    uint64_t Relatches() const { return m_relatches; }
    uint64_t Holds() const { return m_holds; }
    uint64_t Duplicates() const { return m_duplicates; }
    uint32_t Eras() const { return m_eras; }

    std::string StatusText() const
    {
        std::string s = "dv-metadata reader: ";
        if (m_id < 0 && m_haveKey)
            return s + "NOT registered (the Indicator refused it: already observed); metadata is never relatched on resync";
        if (!m_haveKey)
            return s + "not registered";
        return s + std::to_string(m_relatches) + " relatches, " + std::to_string(m_holds) + " holds, " +
            std::to_string(m_eras) + " era(s)";
    }

private:
    void OnChange(const VpIndicator::Indicator& ind, const VpIndicator::Indicator::Change& c)
    {
        ++m_calls;
        CounterMask rel = c.relevant;
        if (!m_latched) {
            m_latched = true;
            rel &= VpIndicator::ChangedSinceLatch(ind, m_latchBaseline, c.seq);
        }
        if (rel == 0)
            return;
        if (m_haveAct && c.seq <= m_lastActSeq) {
            ++m_duplicates;
            return;
        }
        m_haveAct = true;
        m_lastActSeq = c.seq;
        Act act;
        act.seq = c.seq;
        act.counters = rel;
        // A FULL (or an output settings change) relatches; a continuity-only change holds.
        if ((rel & (Bit(VpIndicator::kFull) | Bit(VpIndicator::kSettingsOutput))) != 0) {
            act.action = Action::Relatch;
            ++m_relatches;
            m_pending.relatch = true;
            ++m_pending.relatches;
        } else {
            act.action = Action::Hold;
            ++m_holds;
            m_pending.hold = true;
            ++m_pending.holds;
        }
        m_pending.seq = c.seq;
        m_pending.counters |= rel;
        const Hook& hook = act.action == Action::Relatch ? m_onRelatch : m_onHold;
        if (hook)
            hook(act);
        if (m_observer)
            m_observer(kName, c.seq);
    }

    Hook m_onRelatch;
    Hook m_onHold;
    std::function<void(const char*, uint64_t)> m_observer;
    std::shared_ptr<VpIndicator::Indicator> m_keepAlive;
    bool m_haveKey = false;
    const VpIndicator::Indicator* m_keyAddress = nullptr;
    uint32_t m_keyEra = 0;
    uint64_t m_latchBaseline = 0;
    int m_id = -1;
    bool m_latched = false;
    bool m_haveAct = false;
    uint64_t m_lastActSeq = 0;
    Pending m_pending;
    uint64_t m_calls = 0;
    uint64_t m_relatches = 0;
    uint64_t m_holds = 0;
    uint64_t m_duplicates = 0;
    uint32_t m_eras = 0;
};

// The signalling modes a dv_switch moves through (the host maps its own enum; Tunnel and Lldv carry Dolby
// Vision metadata, Hdr10 and Sdr do not, so a missing RPU there is not a loss).
enum class Mode : uint8_t { Tunnel, Lldv, Hdr10, Sdr };

inline bool ModeCarriesMetadata(Mode m) { return m == Mode::Tunnel || m == Mode::Lldv; }

inline const char* ModeName(Mode m)
{
    switch (m) {
    case Mode::Tunnel: return "tunnel";
    case Mode::Lldv: return "lldv";
    case Mode::Hdr10: return "hdr10";
    case Mode::Sdr: return "sdr";
    }
    return "tunnel";
}

// The concrete adapter around the real DvMetadataHold::Tracker: the held ("retained") metadata, the
// tunnel detection state and the HDR inference epoch the reader must leave alone. `Metadata` is the
// formatter's DolbyVisionTunnelFrameMetadata (valid, reusedPreviousDm, dmHoldPhase, dmRejectedPictures).
//
// Per picture the host calls OnPicture(fresh or nullptr, counter): a valid fresh metadata is latched; a
// picture without it holds the last valid metadata (Tracker phase Held), then, past the hold (a TIME at
// the measured rate, Tracker::HoldPictures), shows its static part (phase Static) and Lost() is true: the
// visible fallback (DvFallbacks.h). With nothing retained (after a Relatch) the picture is black
// (HaveMetadata false) and Lost() follows the same bound.
template <class Tracker = DvMetadataHold::Tracker, class Metadata = DolbyVisionTunnelFrameMetadata>
class MetadataHolder {
public:
    struct Shown {
        bool haveMetadata = false;   // false: nothing to tone map from (black)
        Metadata metadata;           // the picture's metadata (held or static copies are marked)
        DvMetadataHold::Phase phase = DvMetadataHold::Phase::Own;
        bool lost = false;           // the visible fallback is engaged
        uint32_t lostFrames = 0;     // consecutive pictures without metadata
    };

    // The measured source rate (caller-provided; no fixed rate) and the static-phase cap in seconds.
    void SetRate(double hz, double staticSeconds)
    {
        m_hold.SetRate(hz, staticSeconds);
        m_rateKnown = hz > 0.0 && hz == hz;
    }
    bool RateKnown() const { return m_rateKnown; }

    void SetMode(Mode mode)
    {
        m_mode = mode;
        if (!ModeCarriesMetadata(mode))
            m_lostFrames = 0;
    }
    Mode GetMode() const { return m_mode; }

    // Binds the reader's hooks to this holder (the holder must outlive the Reader's callbacks).
    void Bind(Reader& reader)
    {
        reader.SetHooks([this](const Reader::Act& a) { OnRelatch(a); }, [this](const Reader::Act& a) { OnHold(a); });
    }

    // FULL / output settings: drop the held metadata and the tunnel detection, restart the hold run.
    // KEPT: the HDR inference epoch, the signal mode, the lost-picture count (a loss in progress stays
    // visible) and the retained rate.
    void OnRelatch(const Reader::Act&)
    {
        m_sealThread = std::this_thread::get_id();
        m_have = false;
        m_dm = Metadata();
        m_tunnelDetected = false;
        m_awaitingRelatch = true;
        m_hold.Reset();
        ++m_relatchCount;
    }

    // MINOR: the last valid metadata stays; only counted.
    void OnHold(const Reader::Act&)
    {
        m_sealThread = std::this_thread::get_id();
        ++m_holdCount;
    }

    // One picture. fresh = the picture's own accepted metadata (valid), or nullptr when it carried none /
    // it was rejected. headerValid = a CRC-valid tunnel header was seen (tunnel detection).
    // Render thread, AFTER the picture's frame was sealed (the reader's relatch for that frame has run):
    // the same thread as the reader's acts (asserted in debug builds).
    Shown OnPicture(const Metadata* fresh, uint64_t counter, bool headerValid = false)
    {
        assert((m_sealThread == std::thread::id() || m_sealThread == std::this_thread::get_id()) &&
               "MetadataHolder::OnPicture must run on the seal (render) thread");
        Shown s;
        if (headerValid)
            m_tunnelDetected = true;
        if (fresh != nullptr && fresh->valid) {
            m_dm = *fresh;
            m_have = true;
            m_tunnelDetected = true;
            if (m_awaitingRelatch) {
                m_awaitingRelatch = false;
                ++m_relatched;
            }
            m_hold.OnPicture(true, true, counter);
            m_lostFrames = 0;
            s.haveMetadata = true;
            s.metadata = m_dm;
            return s;
        }
        if (!ModeCarriesMetadata(m_mode)) {
            m_hold.OnPicture(true, m_have, counter);   // nothing is expected: not a rejected picture
            return s;
        }
        if (m_lostFrames != UINT32_MAX)
            ++m_lostFrames;
        const DvMetadataHold::Phase phase = m_hold.OnPicture(false, m_have, counter);
        s.phase = phase;
        s.lostFrames = m_lostFrames;
        s.lost = m_lostFrames > m_hold.HoldPictures();
        if (m_have) {
            s.haveMetadata = true;
            s.metadata = m_dm;
            s.metadata.reusedPreviousDm = true;
            s.metadata.dmHoldPhase = static_cast<uint8_t>(phase);
            s.metadata.dmRejectedPictures = m_hold.RejectedPictures();
            if (phase == DvMetadataHold::Phase::Static)
                DvMetadataHold::StripToStatic(s.metadata);   // shown picture only; the retained DM stays whole
        }
        return s;
    }

    // The visible fallback for the last picture (hand to FallbackCatalog::Sync).
    FallbackCatalog::Decision Fallback() const
    {
        return DvMetadataLost(m_lostFrames > m_hold.HoldPictures() && ModeCarriesMetadata(m_mode),
                              static_cast<int>(m_lostFrames > 0x7FFFFFFFu ? 0x7FFFFFFFu : m_lostFrames));
    }

    bool HaveMetadata() const { return m_have; }
    const Metadata& Retained() const { return m_dm; }
    bool TunnelDetected() const { return m_tunnelDetected; }
    bool AwaitingRelatch() const { return m_awaitingRelatch; }
    uint32_t LostFrames() const { return m_lostFrames; }
    bool Lost() const { return ModeCarriesMetadata(m_mode) && m_lostFrames > m_hold.HoldPictures(); }
    uint64_t RelatchCount() const { return m_relatchCount; }    // reader acts: held metadata dropped
    uint64_t Relatched() const { return m_relatched; }          // re-latched from a valid picture afterwards
    uint64_t HoldCount() const { return m_holdCount; }
    const Tracker& Hold() const { return m_hold; }

    // The HDR inference epoch the reader must NOT touch: the host advances it when HdrInference starts a
    // new signal epoch (this holder only keeps the value for the status line and the tests).
    uint32_t HdrInferenceEpoch() const { return m_hdrEpoch; }
    void AdvanceHdrInferenceEpoch() { ++m_hdrEpoch; }

    std::string StatusText() const
    {
        std::string s = std::string("DV metadata: ") + ModeName(m_mode) + (m_have ? ", held" : ", none") +
            (m_tunnelDetected ? ", tunnel detected" : ", tunnel not detected");
        if (m_awaitingRelatch)
            s += ", relatching";
        if (Lost())
            s += "; LOST for " + std::to_string(m_lostFrames) + " pictures (static metadata only)";
        if (!m_rateKnown)
            s += "; frame rate not measured (hold bound is the shortest)";
        return s;
    }

private:
    Tracker m_hold;
    std::thread::id m_sealThread;   // the thread the reader's acts ran on (render thread)
    Metadata m_dm;
    bool m_have = false;
    bool m_tunnelDetected = false;
    bool m_awaitingRelatch = false;
    bool m_rateKnown = false;
    Mode m_mode = Mode::Tunnel;
    uint32_t m_lostFrames = 0;
    uint32_t m_hdrEpoch = 0;
    uint64_t m_relatchCount = 0;
    uint64_t m_relatched = 0;
    uint64_t m_holdCount = 0;
};

} // namespace VpDvResync
