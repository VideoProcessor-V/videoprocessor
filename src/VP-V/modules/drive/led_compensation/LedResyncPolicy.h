// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/Indicator.h>

#include <cstdint>

// Dynamic Drive reader policy for resync_event = on (Q0-A). INERT until the renderer's LED call site
// uses it. A FULL resync restarts the module through LedDriveModule::OnSourceRestart (read, not
// changed): the look-ahead starts over, the GAIN IS HELD at its last value until the new source's
// first measurement, CNTR goes to its safe baseline at once and the CNTR assist's ceiling, bias,
// hold and cut recovery are dropped, so the old source's CNTR is never held above the new source's
// need. Compensate only (OnSourceRestart returns early otherwise). MINOR (continuity) is Hold and
// timing is the separate ResetPeriodSmoothing; neither is handled here.
//
// A template over the module type so the header needs no LedDriveModule include.
namespace VpResync {

// True when the restart was applied (FULL is in `changed`). measuredFps 0 = unknown.
template <class LedModule>
inline bool ApplyFullToLed(LedModule& led, VpIndicator::CounterMask changed, uint64_t seq, double measuredFps)
{
    if ((changed & VpIndicator::Bit(VpIndicator::kFull)) == 0)
        return false;
    led.OnSourceRestart(seq, measuredFps);
    return true;
}

} // namespace VpResync
