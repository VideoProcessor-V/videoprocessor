#pragma once

#include <DolbyVisionLuminanceStatus.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>
#include <libplacebo/colorspace.h>

namespace DolbyVisionLuminancePolicy
{
    constexpr bool ShouldClearStatusForFrame(
        bool tunnelActive,
        bool frameHasMetadata)
    {
        return !tunnelActive || !frameHasMetadata;
    }

    // The HDMI tunnel supplies DM source range and L1 scene data, not HDR10
    // MaxCLL/MaxFALL. Keep the supplied static range only as an explicit fallback.
    inline DolbyVisionLuminanceStatus Apply(
        const DolbyVisionTunnelFrameMetadata& dm, pl_hdr_metadata& hdr)
    {
        DolbyVisionLuminanceStatus status;
        status.tunnelActive = true;
        status.available = dm.valid;
        hdr.max_cll = hdr.max_fall = 0;
        hdr.max_pq_y = hdr.avg_pq_y = 0;
        if (!dm.valid)
            return status;
        status.sourceRangeFromMetadata = dm.sourceMaxPq > dm.sourceMinPq &&
            dm.sourceMaxPq <= 4095;
        if (status.sourceRangeFromMetadata)
        {
            hdr.min_luma = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS,
                static_cast<float>(dm.sourceMinPq) / 4095.0f);
            hdr.max_luma = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS,
                static_cast<float>(dm.sourceMaxPq) / 4095.0f);
        }
        status.sourceMin = hdr.min_luma;
        status.sourceMax = hdr.max_luma;
        status.sceneAvailable = dm.hasSceneMetadata &&
            dm.sceneMaxPq <= 4095 && dm.sceneAvgPq <= 4095;
        if (status.sceneAvailable)
        {
            hdr.max_pq_y = static_cast<float>(dm.sceneMaxPq) / 4095.0f;
            hdr.avg_pq_y = static_cast<float>(dm.sceneAvgPq) / 4095.0f;
            status.sceneMax = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, hdr.max_pq_y);
            status.sceneAvg = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, hdr.avg_pq_y);
        }
        return status;
    }

    // Forward declaration for ApplyWithSceneMin.
    inline bool SceneMinNits(const DolbyVisionTunnelFrameMetadata& dm, float& nits);

    // Apply plus the L1 scene minimum on the status (2026-10-17). hdr is
    // written exactly as by Apply; the minimum is status only.
    inline DolbyVisionLuminanceStatus ApplyWithSceneMin(
        const DolbyVisionTunnelFrameMetadata& dm, pl_hdr_metadata& hdr)
    {
        DolbyVisionLuminanceStatus status = Apply(dm, hdr);
        status.sceneMinAvailable = SceneMinNits(dm, status.sceneMin);
        return status;
    }

    // audit 2026-10-16: the L1 scene minimum. pl_hdr_metadata has no scene
    // minimum (only max_pq_y / avg_pq_y), and min_luma is the static
    // mastering/source black, so this is deliberately NOT written into hdr.
    // Returns false (nits = 0) when L1 is absent or out of range; a present
    // black scene minimum returns true with 0 nits.
    inline bool SceneMinNits(const DolbyVisionTunnelFrameMetadata& dm, float& nits)
    {
        nits = 0;
        if (!dm.valid || !dm.hasSceneMetadata || dm.sceneMinPq > 4095 ||
            dm.sceneMaxPq > 4095 || dm.sceneAvgPq > 4095)
            return false;
        nits = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS,
            static_cast<float>(dm.sceneMinPq) / 4095.0f);
        return true;
    }

    // audit 2026-10-16: PQ 0-1 scene minimum for a future VP-owned curve
    // (same 4095 normalisation as max_pq_y / avg_pq_y). -1 when absent.
    inline float SceneMinPq(const DolbyVisionTunnelFrameMetadata& dm)
    {
        float unused = 0;
        return SceneMinNits(dm, unused) ?
            static_cast<float>(dm.sceneMinPq) / 4095.0f : -1.0f;
    }
}
