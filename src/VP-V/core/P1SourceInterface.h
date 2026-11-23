// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// The host's analysis source (VideoProcessor-Lib AnalysisLumaSource.h), named by reference only in IP1Source below:
// core never includes its header and never reads it (the implementation lives in modules/input/source_analysis).
struct AnalysisLumaSource;

namespace VpSkeleton {

// C9 commit A (doc 93 C9P s2b; owner O3: one decode, one upload, one recipe). How a P1 reader got a frame's engine view.
enum class P1ReadKind : uint8_t {
    None,            // neither: the caller keeps its own decode (and shows it)
    LanePublished,   // the lane's published (or sealed) P1, copied under the slot seqlock
    LiveFill         // the lane had not made it: the same law through the frame's shared luma decode
};

// The outcome of one seqlocked copy of a lane-published product (AnalysisLane.h ReadPublishedBytes).
enum class PublishedRead : uint8_t {
    Absent,     // no published / sealed result of the frame in its ring slot (or no lane, wrong size)
    Copied,     // copied; the slot held the same frame and result before and after the copy
    Recycled    // the lane reused the slot during the copy: the copy is discarded (the caller live-fills)
};

// The lane side of a P1 reader: frame `seq`'s engine half (width x height source) copied from the lane's published P1 into
// `out` (HalfDim(width) x HalfDim(height) bytes). AnalysisLane.h PublishedEngine8Of builds it over a lease source.
using PublishedEngine8Fn = std::function<PublishedRead(uint64_t seq, uint32_t width, uint32_t height, uint8_t* out)>;

// The motion engine's one P1 reader: frame `seq`'s 8-bit engine half (hw x hh = ceil(w/2) x ceil(h/2) of `source`,
// Engine8FromP1 of the P1 half plane), from the lane's published P1, else through the frame's shared decode with the
// same law. False (how = None) when neither can be read. Any thread; never waits for the lane and never writes its ring.
class IP1Source {
public:
    virtual ~IP1Source() {}
    virtual bool ReadEngine8(uint64_t seq, const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& hw, int& hh,
                             P1ReadKind& how) = 0;
};

} // namespace VpSkeleton
