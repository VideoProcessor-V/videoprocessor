// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// THE ceiling on ahead COMPUTE (OD 11-14, owner question window 2026-11-14 ~09:10 / ~09:15 via the coordinator;
// lead10/design-queue-keyed-products.md s10, amendment 3). It supersedes the 11-08 rule "the lane runs the full queue depth" and
// the look-ahead lead's old cap MotionAhead::kMaxLead 13 (the 16-pair P2 ring minus the 3 kept behind the render).
//   aheadCompute = min(max over ACTIVE consumers of their declared ahead need, kAheadCeiling, queue frames ahead); 0 with no active
//   consumer. An upper bound, never a target: the scheduler (S-3a S3 PlanAhead, the interim QueueWindow ladder S-B) may plan lower.
// The only named ahead depth (rule 6): QueueWindow.h, S-3a's AdmissionTable.h, MotionAhead.h (kMaxLead) and LaneAheadPolicy.h
// (DepthMax) all read this one value. Its own tiny header so the motion headers (included by the renderer host through the
// forwarding header) need not include the queue window. What does NOT change: the queue length (QueueFrames.h, DelayBudget), the
// keep window (the full queue back) and the P2 ring size. Header-only, C++14.

#include <cstdint>

namespace VpSkeleton {

constexpr uint16_t kAheadCeiling = 6;

}   // namespace VpSkeleton
