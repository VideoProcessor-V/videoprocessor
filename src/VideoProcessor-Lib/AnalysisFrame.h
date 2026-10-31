#pragma once
#include <VideoFrameEncoding.h>

#include <AnalysisLumaSource.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// Identifies the consumer that requested the frame. Source selection is
// canonical across live and queued analysis and does not depend on renderer
// upload format.
enum class AnalysisSourceProfile : uint8_t
{
    LiveRenderer,
    QueuedCapture,
};

enum class AnalysisFrameDomain : uint8_t
{
    Unavailable,
    NativeRgb,
    NativeYuv422,
    ConvertedP010,
    ConvertedP210,
    DecodedDolbyVision422,
    SdrTunnelRgb,
    NativeYuv420,
};

struct AnalysisFrameInput
{
    const uint8_t* nativeData = nullptr;
    size_t nativeBytes = 0;
    size_t nativeRowBytes = 0;
    std::shared_ptr<const void> nativeOwner;
    const uint8_t* convertedData = nullptr;
    size_t convertedBytes = 0;
    size_t convertedRowBytes = 0; // zero uses the beta width*2 layout
    size_t convertedChromaRowBytes = 0;
    std::shared_ptr<const void> convertedOwner;
    VideoFrameFormatterOutputContract convertedContract;
    DolbyVisionTunnelFrameMetadata dolby;
    int width = 0;
    int height = 0;
    VideoFrameEncoding encoding = VideoFrameEncoding::UNKNOWN;
    ColorSpace colorspace = ColorSpace::UNKNOWN;
    uint32_t transferKey = 0; // caller-provided EOTF; affects code interpretation
    uint64_t sourceGeneration = 0;
    AnalysisSourceProfile profile = AnalysisSourceProfile::QueuedCapture;
    // Renderer-output metadata is retained for diagnostics/call-site clarity;
    // neither field selects the analysis source.
    bool rendererUsesNative = false;
    bool converted422 = false;
    bool dolbyVisionTunnel = false;
    // dv_tunnel_gpu_unpack: the decoded planes are on the GPU only; sample the
    // raw r210 words (native) with this RGB8 placement (1 high, 2 low eight
    // bits). Zero: the CPU formatter's planes (converted).
    uint8_t dolbyTunnelPackedAlignment = 0;
    TunnelSdrRgbRange tunnelSdrRgbRange = TunnelSdrRgbRange::Full;
    // VP-V DIV-AN-01 (either/or): the prepared source samples in the limited
    // analysis code domain (AnalysisLumaSource::limitedDomain, fullRangeYuv for
    // full-range DV planes). The renderer sets it from source_coding_fixes
    // (Original pathway: off); false = Bill's range-tagged delivered codes.
    bool vpLimitedDomain = false;
};

// The prepared source retains the selected frame pixels while sampling them
// at original resolution with the established code and chroma footprint.
// Its domain identifies interpretation, not transport identity.
struct PreparedAnalysisFrame
{
    AnalysisLumaSource source;
    std::shared_ptr<const void> sourceOwner;
    AnalysisFrameDomain domain = AnalysisFrameDomain::Unavailable;
    AnalysisSourceProfile profile = AnalysisSourceProfile::QueuedCapture;
    uint32_t transferKey = 0;
    // Describe the encoded storage contract selected for the frame. They do
    // not rescale AnalysisLumaSample: Sample emits the established 10-bit
    // evidence codes using format-specific unpacking and rounding (Bill), or,
    // with vpLimitedDomain, codes in the limited domain (VP-V DIV-AN-01).
    VideoFrameSampleRange sampleRange = VideoFrameSampleRange::UNKNOWN;
    uint8_t colorDepth = 0;
    uint8_t bitShift = 0;
    uint8_t rgbBitAlignment = 0;
    bool dolbyFullRange = false;
    bool usesConverted = false;
    uint64_t domainKey = 0;

    bool IsValid() const
    {
        return domain != AnalysisFrameDomain::Unavailable && sourceOwner &&
            source.IsValid() && domainKey != 0;
    }
};

bool AnalysisNativeRgbSupported(VideoFrameEncoding encoding);
PreparedAnalysisFrame PrepareAnalysisFrame(const AnalysisFrameInput& input);
