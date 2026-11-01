// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Indicator.h"

#include <cstdint>

// Per-record temporal validity from the central Indicator, for the Frame Analysis Store
// (FrameAnalysisStore.h). A record is keyed by its source sequence and keeps NO stamp of its own:
// the Indicator's StampOf(recordSeq) is the only source, compared with the stamp of the frame that
// wants to use it. FrameAnalysisStore itself is unchanged; its epoch form keeps working, and a
// caller that runs resync-driven asks this helper instead.
namespace VpFrameAnalysis {

// True when the record made for recordSeq may be used together with the frame useSeq (a temporal
// window, a look-ahead statistic) under `mask`: same Indicator era, neither stamp ancient, every
// masked counter equal. A FULL between the two (mask containing Bit(kFull)) makes it false.
// Single-frame use of a record does not need this (it is valid for its own frame).
inline bool RecordValid(const VpIndicator::Indicator& ind, uint64_t recordSeq, uint64_t useSeq,
                        VpIndicator::CounterMask mask)
{
    return VpIndicator::TemporalValid(ind, recordSeq, useSeq, mask);
}

} // namespace VpFrameAnalysis
