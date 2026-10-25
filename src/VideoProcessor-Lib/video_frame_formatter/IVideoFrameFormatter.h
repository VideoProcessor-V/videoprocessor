/*
 * Copyright(C) 2021 Dennis Fleurbaaij <mail@dennisfleurbaaij.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once


#include <cstdint>

#include <Windows.h>

// Keep conversion kernel declarations independent of ATL startup initializers.
class VideoFrame;
class VideoState;
namespace ATL { template<class T> class CComPtr; }
typedef ATL::CComPtr<VideoState> VideoStateComPtr;


enum class VideoFrameSampleRange
{
	UNKNOWN,
	FULL,
	LIMITED,
};


// Describes the code values written by a formatter. Consumers must use this
// instead of inferring range or precision from the captured input format.
struct VideoFrameFormatterOutputContract
{
	VideoFrameSampleRange sampleRange = VideoFrameSampleRange::UNKNOWN;
	uint8_t colorDepth = 0;
	uint8_t bitShift = 0;

	bool IsValid() const
	{
		return sampleRange != VideoFrameSampleRange::UNKNOWN &&
			colorDepth != 0;
	}
};


// Parsed per-picture HDMI Dolby Vision display-management metadata. This is
// kept independent of libplacebo so capture formatters can expose it directly.
struct DolbyVisionTunnelFrameMetadata
{
    bool valid = false;
    bool fullRange = true;
    // CRC loss after DV lock is not evidence that the source became SDR.
    bool allowSdrFallback = false;
    // Cached DM can survive a brief loss, but an untrusted current picture
    // must be presented as black and excluded from image analysis.
    bool requiresBlackFrame = false;
    uint8_t rgbBitAlignment = 0; // 1: high 8 bits of r210, 2: low 8 bits
    float nonlinear[3][3] = {};
    float nonlinearOffset[3] = {};
    float linear[3][3] = {};
    uint16_t sourceMinPq = 0;
    uint16_t sourceMaxPq = 0;
    bool hasSceneMetadata = false;
    uint16_t sceneMaxPq = 0;
    uint16_t sceneAvgPq = 0;
    // audit 2026-10-16: L1 scene minimum (12-bit PQ code), valid when
    // hasSceneMetadata. libplacebo has no scene-minimum input; carried for
    // VP-owned curves and status only.
    uint16_t sceneMinPq = 0;

    // audit 2026-10-16: L2 / L8 display trims, parsed generically but NOT
    // applied (no rendering change). Fields use the same big-endian 16-bit
    // containers as the existing L1 parser. The L8 container layout is an
    // assumption until a live packet is checked (raw bytes are logged once
    // per title). 12-bit trim codes are kept as transmitted (unscaled).
    struct Trim
    {
        uint8_t level = 0;              // 2 or 8
        uint8_t targetDisplayIndex = 0; // L8 only (refers to L10 / predefined target)
        uint16_t targetMaxPq = 0;       // L2 only (12-bit PQ code)
        uint16_t slope = 0;
        uint16_t offset = 0;
        uint16_t power = 0;
        uint16_t chromaWeight = 0;
        uint16_t saturationGain = 0;
        int16_t msWeight = 0;
        bool hasMidContrastAndClip = false; // L8 optional
        uint16_t midContrast = 0;
        uint16_t clipTrim = 0;
        bool hasSaturationVector = false;   // L8 optional
        uint8_t saturationVector[6] = {};
        bool hasHueVector = false;          // L8 optional
        uint8_t hueVector[6] = {};
    };
    static constexpr uint8_t kMaxTrims = 8;
    uint8_t trimCount = 0;
    bool trimOverflow = false; // more L2/L8 blocks than kMaxTrims arrived
    Trim trims[kMaxTrims] = {};

    // Content intent (intent interpreter item 1, 2026-10-17): L5 / L6 / L11,
    // parsed for status, the scene descriptor and the content classifier only
    // (no rendering change). Field layouts follow libdovi's public parser
    // (quietvoid/dovi_tool, dolby_vision/src/rpu/extension/level5.rs,
    // level6.rs, level11.rs); see CR210DolbyVisionTunnelFormatter.cpp.
    // L5 active area: offsets in source pixels from each edge (13-bit fields).
    bool hasActiveArea = false;
    uint16_t activeAreaLeft = 0;
    uint16_t activeAreaRight = 0;
    uint16_t activeAreaTop = 0;
    uint16_t activeAreaBottom = 0;
    // L6 static ST 2086 / CTA-861.3 values carried in the DM: mastering
    // display max (nits) and min (0.0001 nits), MaxCLL and MaxFALL (nits).
    // MaxCLL 0 = not given (hasContentLight stays false then).
    bool hasL6 = false;
    uint16_t l6MaxDisplayMasteringLuminance = 0;
    uint16_t l6MinDisplayMasteringLuminance = 0;
    bool hasContentLight = false;
    uint16_t maxCll = 0;
    uint16_t maxFall = 0;
    // L11 content type (0 default/unknown, 1 movies, 2 game, 3 sport,
    // 4 user-generated, others reserved), white point, reference mode, and
    // the intended picture-mode hints (2 bits each, 0 = no preference; the
    // byte 2-3 layout is to be verified against the live "title L11" log line,
    // which carries the raw bytes).
    bool hasL11 = false;
    uint8_t l11ContentType = 0;
    uint8_t l11Whitepoint = 0;
    bool l11ReferenceMode = false;
    uint8_t l11Sharpness = 0;
    uint8_t l11NoiseReduction = 0;
    uint8_t l11MpegNoiseReduction = 0;
    uint8_t l11FrameRateConversion = 0;
    uint8_t l11Brightness = 0;
    uint8_t l11Color = 0;
    uint8_t l11Raw[4] = {};
    // DV open item (2026-10-24, no silent fallbacks): this picture's own DM was rejected
    // (missing, CRC-invalid or malformed) and the previous DM is being reused within the loss
    // limit. The renderer lists it under "Fallbacks engaged" while it lasts.
    bool reusedPreviousDm = false;

    // feat/dv-l8 (2026-10-24): CM v4.0 L9 (source primaries) and L10 (custom target
    // displays), needed to interpret an L8 trim's target_display_index. Parsed by
    // VP-V/modules/input/signal/DvSignalLevels.h (16-bit containers like L1/L2, or the
    // RPU's packed form). The first block per target index wins.
    struct TargetDisplay
    {
        uint8_t index = 0;          // target_display_index (what L8 refers to)
        uint16_t maxPq = 0;         // 12-bit PQ code
        uint16_t minPq = 0;         // 12-bit PQ code
        uint8_t primaryIndex = 0;
    };
    static constexpr uint8_t kMaxTargetDisplays = 8;
    uint8_t targetDisplayCount = 0;
    TargetDisplay targetDisplays[kMaxTargetDisplays] = {};
    bool hasL9 = false;
    uint8_t l9SourcePrimaryIndex = 0;
    // feat/dv-l8 (no silent fallbacks): the bounded hold of a rejected DM
    // (VP-V/modules/input/signal/DvMetadataHold.h). 0 = this picture's own DM; 1 = the
    // previous DM held (within the hold); 2 = beyond the hold: static/base metadata only
    // (matrices, source range, L5/L6/L9/L10/L11 kept; L1 scene values and trims dropped).
    uint8_t dmHoldPhase = 0;
    uint32_t dmRejectedPictures = 0;   // pictures whose DM was rejected since the signal started
};

/**
 * Interface for a video frame formatter, this can render from one
 * format to another.
 */
class IVideoFrameFormatter
{
public:

	virtual ~IVideoFrameFormatter() {}

	// New video state, must be called before FormatVideoFrame()
	virtual void OnVideoState(VideoStateComPtr& videoState) = 0;

	// Handle video frame
	// Can only be called after OnVideoState()
	// Returns true if something was converted, false if not
	virtual bool FormatVideoFrame(const VideoFrame& inFrame, BYTE* outBuffer) = 0;

	// Get size of frame that will be put in FormatVideoFrame()'s outBuffer, in bytes
	// Can only be called after OnVideoState()
	virtual LONG GetOutFrameSize() const = 0;

	// Get the sample-range and precision contract for FormatVideoFrame output.
	virtual VideoFrameFormatterOutputContract GetOutputContract() const
	{
		return {};
	}

    virtual DolbyVisionTunnelFrameMetadata GetDolbyVisionTunnelMetadata() const
    {
        return {};
    }
	
	// Get conversion performance metrics (optional, default implementation returns no data)
	// currentUs: Latest frame conversion time in microseconds
	// avg10s: Average conversion time over last 10 seconds (?s)
	// max10s: Maximum conversion time over last 10 seconds (?s)
	virtual void GetConversionPerformance(double& currentUs, double& avg10s, double& max10s) const
	{
		currentUs = 0.0;
		avg10s = 0.0;
		max10s = 0.0;
	}
};
