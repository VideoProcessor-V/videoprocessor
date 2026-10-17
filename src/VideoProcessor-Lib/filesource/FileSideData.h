/*
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once

// VP Renderer DirectShow filter: per-sample side data from LAV Video
// (IMediaSideData, 3rdparty/lavfilters/IMediaSideData.h) -> VP HDR / Dolby
// Vision structures. Pure: every parser takes (GUID, bytes, size) and touches
// no COM object, so it is CPU-testable.
//
// Size rule: each LAV struct must be AT LEAST its declared size. LAV may append
// fields (its DOVI struct says so explicitly), so a larger buffer is accepted
// and only the known prefix is read; a null or short buffer is rejected.

#include <cstddef>
#include <cstdint>

#include <Windows.h>

#include <HDRData.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>

namespace FileSideData
{
	enum class ParseResult
	{
		Ok,
		WrongType,   // the GUID is not the one this parser handles
		NullData,    // null pointer or zero size
		TooShort,    // smaller than the minimum struct size
		Invalid,     // structurally present but unusable values
	};

	// GUIDs that the vendored header marks [[deprecated]] (using those names is
	// an error under /sdl), defined here with the same values:
	// IID_MediaSideDataHDR10PlusOld {183ED511-8910-4262-88F6-4946BC799C84}:
	// HDR10+ without the itu_t_t35_country_code / application_version prefix.
	const GUID& LegacyHdr10PlusGuid();
	// IID_MediaSideDataDOVIMetadata {277EE779-13F4-434E-BDEC-3D6F8C0E15D2}:
	// Dolby Vision metadata version 1 (no Extensions block).
	const GUID& DoviMetadataV1Guid();

	// ---------------------------------------------------------------
	// HDR10 static metadata
	// ---------------------------------------------------------------

	// IID_MediaSideDataHDR (MediaSideDataHDR: doubles, CIE 1931 xy, primaries
	// in G-B-R order, luminance in cd/m2) -> primaries, white point and
	// mastering luminance of `hdr`. maxCll / maxFall are left untouched.
	ParseResult ParseMasteringDisplay(const GUID& type, const BYTE* data, size_t size, HDRData& hdr);

	// IID_MediaSideDataHDRContentLightLevel -> hdr.maxCll / hdr.maxFall (cd/m2).
	// Only maxCll / maxFall are written.
	ParseResult ParseContentLightLevel(const GUID& type, const BYTE* data, size_t size, HDRData& hdr);

	// ---------------------------------------------------------------
	// HDR10+ (SMPTE ST 2094-40) dynamic metadata, kept for later use.
	// VP does not consume it yet; the fields chosen are what libplacebo's
	// pl_hdr_metadata takes (scene_max[3], scene_avg, ootf target / knee /
	// Bezier anchors) for PL_HDR_METADATA_HDR10PLUS tone mapping.
	// ---------------------------------------------------------------
	struct Hdr10PlusMetadata
	{
		bool valid = false;
		bool hasIdentification = false; // false for the legacy GUID
		uint32_t countryCode = 0;        // itu_t_t35_country_code (0xB5 = USA)
		uint32_t applicationVersion = 0;
		uint32_t numWindows = 0;         // 1..3; only window 0 (the full frame) is kept
		// Window 0. LAV normalises the rationals: maxscl / average_maxrgb are
		// linear light in [0, 1] of 10000 cd/m2; the *Nits copies are scaled.
		double maxScl[3] = {};
		double averageMaxRgb = 0.0;
		double sceneMaxNits[3] = {};
		double sceneAvgNits = 0.0;
		uint32_t numPercentiles = 0;     // <= 15
		uint32_t percentilePercentage[15] = {};
		double percentile[15] = {};      // linear [0, 1] of 10000 cd/m2
		double fractionBrightPixels = 0.0;
		bool toneMapping = false;        // tone_mapping_flag
		double kneePointX = 0.0;         // [0, 1]
		double kneePointY = 0.0;
		uint32_t numBezierAnchors = 0;   // <= 15
		double bezierAnchors[15] = {};
		bool colorSaturationMapping = false;
		double colorSaturationWeight = 0.0;
		// Display the grade targets (cd/m2).
		double targetedSystemDisplayMaximumLuminance = 0.0;
		bool targetedSystemDisplayActualPeakLuminanceFlag = false;
		bool masteringDisplayActualPeakLuminanceFlag = false;
	};

	// IID_MediaSideDataHDR10Plus (current) or LegacyHdr10PlusGuid().
	ParseResult ParseHdr10Plus(const GUID& type, const BYTE* data, size_t size, Hdr10PlusMetadata& out);

	// ---------------------------------------------------------------
	// Dolby Vision (LAV MediaSideDataDOVIMetadata, V2 = with Extensions)
	// ---------------------------------------------------------------

	// Reshaping (composer) curves, normalised exactly as libplacebo's
	// pl_map_dovi_metadata() does (utils/libav_internal.h): pivots scaled by
	// 1 / (2^bl_bit_depth - 1), coefficients by 1 / 2^coef_log2_denom. The
	// layout mirrors pl_reshape_data so a renderer can copy it into
	// pl_dovi_metadata::comp. Not part of any ABI.
	struct DolbyVisionReshaping
	{
		static constexpr int kMaxPieces = 8;
		struct Component
		{
			uint8_t numPivots = 0;                 // [2, 9]
			float pivots[kMaxPieces + 1] = {};
			uint8_t method[kMaxPieces] = {};       // 0 polynomial, 1 MMR
			float polyCoeffs[kMaxPieces][3] = {};
			uint8_t mmrOrder[kMaxPieces] = {};     // [1, 3]
			float mmrConstant[kMaxPieces] = {};
			float mmrCoeffs[kMaxPieces][3][7] = {};
		};
		Component comp[3];
	};

	struct DolbyVisionFileMetadata
	{
		bool valid = false;
		bool hasExtensions = false; // V2 GUID (V1 carries no extension blocks)

		// The existing per-picture DV structure (as filled by the HDMI tunnel
		// formatter): matrices, source range, L1, L2/L8 trims, L5, L6, L11.
		DolbyVisionTunnelFrameMetadata dm;

		// RPU header / base layer, which the tunnel struct has no place for.
		uint8_t rpuType = 0;
		uint16_t rpuFormat = 0;
		uint8_t vdrRpuProfile = 0;
		uint8_t vdrRpuLevel = 0;
		uint8_t blBitDepth = 0;
		uint8_t elBitDepth = 0;
		uint8_t vdrBitDepth = 0;
		bool blFullRange = false;
		bool disableResidual = false;
		bool elSpatialResampling = false;
		uint8_t coefLog2Denom = 0;
		uint8_t mappingColorSpace = 0;
		uint8_t mappingChromaFormat = 0;
		// Guessed Dolby Vision profile (libdovi's RpuDataHeader::guess_profile
		// heuristic): 5 (IPTPQc2 base layer, reshaping required), 7 (base +
		// enhancement layer) or 8 (HDR10/HLG-compatible base layer). The
		// DOVI decoder configuration record of the media type is authoritative
		// when the filter has it.
		uint8_t guessedProfile = 0;
		// NLQ (profile 7 enhancement layer). LAV delivers only the base layer
		// picture, so NLQ cannot be applied; carried for diagnostics.
		bool hasNlq = false;
		uint8_t nlqMethod = 0;

		bool hasReshaping = false;
		DolbyVisionReshaping reshaping;

		// Colour metadata extras.
		uint8_t dmMetadataId = 0;
		bool sceneRefresh = false;
		uint16_t signalEotf = 0;
		uint8_t signalBitDepth = 0;
		uint8_t signalColorSpace = 0;
		uint8_t signalChromaFormat = 0;
		uint8_t signalFullRangeRaw = 0; // [0, 3]; dm.fullRange = (raw != 0) as in the tunnel
		uint16_t sourceDiagonal = 0;

		// L9 source display primaries (R-G-B order in LAV), not representable
		// in DolbyVisionTunnelFrameMetadata.
		bool hasL9 = false;
		uint8_t l9SourcePrimaryIndex = 0;
		double l9PrimaryRedX = 0, l9PrimaryRedY = 0;
		double l9PrimaryGreenX = 0, l9PrimaryGreenY = 0;
		double l9PrimaryBlueX = 0, l9PrimaryBlueY = 0;
		double l9WhitePointX = 0, l9WhitePointY = 0;

		// L254 (DM v2 info block).
		bool hasL254 = false;
		uint8_t l254DmMode = 0;
		uint8_t l254DmVersionIndex = 0;

		// Bit n set: an extension block of level n (1..31) was present;
		// level 254 is reported by hasL254. Diagnostics.
		uint32_t extensionLevelMask = 0;
		uint8_t extensionCount = 0;
	};

	// IID_MediaSideDataDOVIMetadataV2 (with extensions) or DoviMetadataV1Guid()
	// (header / mapping / colour only).
	ParseResult ParseDolbyVision(const GUID& type, const BYTE* data, size_t size, DolbyVisionFileMetadata& out);

	// ---------------------------------------------------------------
	// One sample's side data, and the static-HDR change detector.
	// ---------------------------------------------------------------
	struct SideDataSnapshot
	{
		bool hasMasteringDisplay = false;
		bool hasContentLight = false;
		HDRData hdr; // fields valid per the two flags above

		bool hasHdr10Plus = false;
		Hdr10PlusMetadata hdr10Plus;

		bool hasDolbyVision = false;
		DolbyVisionFileMetadata dolbyVision;

		bool HasStaticHdr() const { return hasMasteringDisplay || hasContentLight; }
	};

	// Routes one (GUID, data) pair to its parser and records the result in
	// `snapshot`. Unknown GUIDs return WrongType and change nothing. A failed
	// parse leaves the snapshot's previous value for that type untouched.
	ParseResult AddToSnapshot(SideDataSnapshot& snapshot, const GUID& type, const BYTE* data, size_t size);

	// The GUIDs AddToSnapshot understands, in the order the filter should query
	// them (current before legacy, so a sample carrying both uses the current).
	size_t KnownSideDataTypeCount();
	const GUID& KnownSideDataType(size_t index);

	// Decides when the filter must emit a new VideoState: only when the static
	// HDR10 metadata (mastering display or content light level) changes.
	// Absence in a sample is NOT a change - LAV may attach static metadata to
	// key frames only - so the last values stay in force until Reset() (new
	// stream / media type / flush to a new segment). Dynamic data (HDR10+, DV)
	// is ignored here and goes with every frame.
	class StaticHdrChangeDetector
	{
	public:
		// Returns true when the effective static metadata differs from the
		// previous effective value (first appearance of either part counts).
		bool Update(const SideDataSnapshot& snapshot);
		void Reset();

		bool HasMasteringDisplay() const { return m_hasMasteringDisplay; }
		bool HasContentLight() const { return m_hasContentLight; }
		// The merged effective metadata (mastering + content light).
		const HDRData& Current() const { return m_current; }

	private:
		bool m_hasMasteringDisplay = false;
		bool m_hasContentLight = false;
		HDRData m_current;
	};
}

// TODO(VP Renderer, DV from files): the renderer has no entry point that takes
// Dolby Vision metadata with a file frame. Capture DV reaches it only through
// IVideoFrameFormatter::GetDolbyVisionTunnelMetadata() of the R210 tunnel
// formatter, and that path assumes already-composed (tunnelled) pixels.
// Minimal ABI addition (one API bump, 35 -> 36): a plain-data
// `FileFrameDolbyVision { uint32_t structSize; DolbyVisionTunnelFrameMetadata dm;
// uint8_t guessedProfile; uint8_t blBitDepth; bool blFullRange; bool hasReshaping;
// DolbyVisionReshaping reshaping; }` passed as an optional pointer next to
// FileFrameTiming (e.g. `OnTimedVideoFrame(VideoFrame&, const FileFrameTiming&,
// const FileFrameDolbyVision*)`, null for non-DV). The renderer would then fill
// pl_dovi_metadata from dm (matrices, as today) plus reshaping.comp, set
// repr.sys = PL_COLOR_SYSTEM_DOLBYVISION for profile 5 (and for 8 when the
// composer is wanted), and run the existing DolbyVisionLuminancePolicy / trims
// / SceneDescriptor code on dm unchanged.
