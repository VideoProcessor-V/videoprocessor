// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split GrainGuardGlsl: the GLSL form of the shared grain guard unit (GrainGuard.h, kUnitVersion 1).
//
// Defines() emits the unit's constants (the GrainGuard.h values, one formatting: VpvGpu::Define); Functions() is the
// law as GLSL, written against those defines:
//
//     float gs_guard_violation(float o[7])          GrainSplit::Guard::Violation (pooled sums s[0..6])
//     float gs_guard_addback(float o0, float o2)    GrainSplit::Guard::AddBack (only s[0] and s[2] are read)
//     bool  gs_guard_step(float v, float fall, float rise, inout float on, inout float weight)
//                                                   GrainSplit::Guard::Step (on is 0 / 1; returns true on a trip)
//
// A reader prepends Defines() + Functions() to the passes that judge the guard. temporal_detail is the first reader
// (15b A4, coreplan-C10-15b-PC-grain.md). Header-only, C++14, no Windows headers.
#pragma once

#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/GrainGuard.h>

#include <string>

namespace GrainSplit
{
	namespace Guard
	{
		inline std::string Defines()
		{
			return VpvGpu::Define("GS_GUARD_MAX_SHARE", kMaxShare) + VpvGpu::Define("GS_GUARD_ADDBACK_MIN", kAddBackMin) +
				VpvGpu::Define("GS_GUARD_DROP_TRIP", kDropTrip) + VpvGpu::Define("GS_GUARD_RISE_TRIP", kRiseTrip) +
				VpvGpu::Define("GS_GUARD_FREEZE_TRIP", kFreezeTrip) + VpvGpu::Define("GS_GUARD_RELEASE", kRelease);
		}

		inline const char* Functions()
		{
			return R"GLSL(
float gs_guard_violation(float o[7])
{
    float v = 0.0;
    if (o[0] > 1e-9) {
        float L = sqrt(o[1] / o[0]);
        float drop = max(1.0 - L, -o[2] / o[0]);
        v = max(drop / GS_GUARD_DROP_TRIP, (L - 1.0) / GS_GUARD_RISE_TRIP);
        if (o[5] > 1e-9 && o[6] > 1e-9 && o[1] > 1e-9) {
            float freeze = o[4] / sqrt(o[1] * o[6]) - o[3] / sqrt(o[0] * o[5]);
            v = max(v, freeze / GS_GUARD_FREEZE_TRIP);
        }
    }
    return v;
}
float gs_guard_addback(float o0, float o2)
{
    float kk = o0 > 1e-9 ? -o2 / o0 : 0.0;
    return kk > GS_GUARD_ADDBACK_MIN ? min(1.0, kk) : 0.0;
}
bool gs_guard_step(float v, float fall, float rise, inout float on, inout float weight)
{
    bool trip = false;
    if (on < 0.5 && v > 1.0) { on = 1.0; trip = true; }
    else if (on > 0.5 && v < GS_GUARD_RELEASE) on = 0.0;
    float target = on > 0.5 ? (v > 1.0 ? weight / v : weight) : 1.0;
    weight += clamp(target - weight, -fall, rise);
    return trip;
}
)GLSL";
		}
	}
}
