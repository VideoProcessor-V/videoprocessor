/*
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#include <pch.h>

// Define (DECLSPEC_SELECTANY) the IMediaSideData GUIDs in this object so the
// parsers and their tests link without LAVFilters.lib; duplicates with
// lavfilters.cpp fold at link time.
#include <initguid.h>
#include <IMediaSideData.h>

#include "FileSideData.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace FileSideData
{
	namespace
	{
		// {183ED511-8910-4262-88F6-4946BC799C84}
		const GUID kLegacyHdr10Plus =
			{ 0x183ed511, 0x8910, 0x4262, { 0x88, 0xf6, 0x49, 0x46, 0xbc, 0x79, 0x9c, 0x84 } };
		// {277EE779-13F4-434E-BDEC-3D6F8C0E15D2}
		const GUID kDoviMetadataV1 =
			{ 0x277ee779, 0x13f4, 0x434e, { 0xbd, 0xec, 0x3d, 0x6f, 0x8c, 0x0e, 0x15, 0xd2 } };

		// Size of the legacy HDR10+ struct: the current one without the two
		// leading identification fields.
		const size_t kHdr10PlusIdentificationSize = 2 * sizeof(unsigned int);
		const size_t kLegacyHdr10PlusSize = sizeof(MediaSideDataHDR10Plus) - kHdr10PlusIdentificationSize;
		// V1 DOVI ends where the V2 Extensions block starts.
		const size_t kDoviV1Size = offsetof(MediaSideDataDOVIMetadata, Extensions);

		bool Finite(double value)
		{
			return std::isfinite(value) != 0;
		}

		ParseResult CheckBuffer(const BYTE* data, size_t size, size_t minimum)
		{
			if (!data || size == 0)
				return ParseResult::NullData;
			if (size < minimum)
				return ParseResult::TooShort;
			return ParseResult::Ok;
		}

		// Copies the known prefix (buffers may be unaligned and larger).
		template<typename T>
		void CopyPrefix(T& out, const BYTE* data, size_t bytes)
		{
			memset(&out, 0, sizeof(T));
			memcpy(&out, data, bytes < sizeof(T) ? bytes : sizeof(T));
		}

		uint8_t GuessProfile(const MediaSideDataDOVIMetadata& md)
		{
			// libdovi RpuDataHeader::guess_profile.
			if (md.Header.vdr_rpu_profile == 0)
				return md.Header.bl_video_full_range_flag ? 5 : 0;
			if (md.Header.vdr_rpu_profile == 1)
			{
				if (md.Header.el_spatial_resampling_filter_flag && !md.Header.disable_residual_flag)
					return md.Header.vdr_bit_depth == 12 ? 7 : 4;
				return 8;
			}
			return 0;
		}

		bool MapReshaping(const MediaSideDataDOVIMetadata& md, DolbyVisionReshaping& out)
		{
			out = DolbyVisionReshaping();
			const unsigned blBitDepth = md.Header.bl_bit_depth;
			const unsigned log2Denom = md.Header.coef_log2_denom;
			if (blBitDepth < 8 || blBitDepth > 16 || log2Denom > 32)
				return false;
			const float pivotScale = 1.0f / static_cast<float>((1u << blBitDepth) - 1u);
			const float coefScale = static_cast<float>(1.0 / std::ldexp(1.0, static_cast<int>(log2Denom)));
			for (int c = 0; c < 3; ++c)
			{
				const auto& src = md.Mapping.curves[c];
				DolbyVisionReshaping::Component& dst = out.comp[c];
				if (src.num_pivots < 2 || src.num_pivots > LAV_DOVI_MAX_PIECES + 1)
					return false;
				dst.numPivots = src.num_pivots;
				for (int i = 0; i < src.num_pivots; ++i)
				{
					if (i > 0 && src.pivots[i] < src.pivots[i - 1])
						return false; // LAV promises ascending pivots
					dst.pivots[i] = pivotScale * static_cast<float>(src.pivots[i]);
				}
				for (int i = 0; i < src.num_pivots - 1; ++i)
				{
					dst.method[i] = src.mapping_idc[i];
					if (src.mapping_idc[i] == 0)
					{
						if (src.poly_order[i] < 1 || src.poly_order[i] > 2)
							return false;
						for (int k = 0; k < 3; ++k)
							dst.polyCoeffs[i][k] = k <= src.poly_order[i] ?
								coefScale * static_cast<float>(src.poly_coef[i][k]) : 0.0f;
					}
					else if (src.mapping_idc[i] == 1)
					{
						if (src.mmr_order[i] < 1 || src.mmr_order[i] > 3)
							return false;
						dst.mmrOrder[i] = src.mmr_order[i];
						dst.mmrConstant[i] = coefScale * static_cast<float>(src.mmr_constant[i]);
						for (int j = 0; j < src.mmr_order[i]; ++j)
							for (int k = 0; k < 7; ++k)
								dst.mmrCoeffs[i][j][k] = coefScale * static_cast<float>(src.mmr_coef[i][j][k]);
					}
					else
						return false;
				}
			}
			return true;
		}

		// Extension is LAV's unnamed per-block struct type.
		template<typename Extension>
		void MapTrimL2(const Extension& ext, DolbyVisionTunnelFrameMetadata::Trim& trim)
		{
			trim = DolbyVisionTunnelFrameMetadata::Trim();
			trim.level = 2;
			trim.targetMaxPq = ext.Level2.target_max_pq;
			trim.slope = ext.Level2.trim_slope;
			trim.offset = ext.Level2.trim_offset;
			trim.power = ext.Level2.trim_power;
			trim.chromaWeight = ext.Level2.trim_chroma_weight;
			trim.saturationGain = ext.Level2.trim_saturation_gain;
			trim.msWeight = ext.Level2.ms_weight;
		}

		bool AnyNonZero(const uint8_t* bytes, size_t count)
		{
			for (size_t i = 0; i < count; ++i)
				if (bytes[i] != 0)
					return true;
			return false;
		}

		template<typename Extension>
		void MapTrimL8(const Extension& ext, DolbyVisionTunnelFrameMetadata::Trim& trim)
		{
			trim = DolbyVisionTunnelFrameMetadata::Trim();
			trim.level = 8;
			trim.targetDisplayIndex = ext.Level8.target_display_index;
			trim.slope = ext.Level8.trim_slope;
			trim.offset = ext.Level8.trim_offset;
			trim.power = ext.Level8.trim_power;
			trim.chromaWeight = ext.Level8.trim_chroma_weight;
			trim.saturationGain = ext.Level8.trim_saturation_gain;
			trim.msWeight = static_cast<int16_t>(ext.Level8.ms_weight);
			// libavcodec zero-fills the optional L8 fields a short block omits,
			// so zero is read as "absent" (neutral values are non-zero).
			trim.midContrast = ext.Level8.target_mid_contrast;
			trim.clipTrim = ext.Level8.clip_trim;
			trim.hasMidContrastAndClip = trim.midContrast != 0 || trim.clipTrim != 0;
			trim.hasSaturationVector = AnyNonZero(ext.Level8.saturation_vector_field, 6);
			if (trim.hasSaturationVector)
				memcpy(trim.saturationVector, ext.Level8.saturation_vector_field, 6);
			trim.hasHueVector = AnyNonZero(ext.Level8.hue_vector_field, 6);
			if (trim.hasHueVector)
				memcpy(trim.hueVector, ext.Level8.hue_vector_field, 6);
		}

		void MapExtensions(const MediaSideDataDOVIMetadata& md, DolbyVisionFileMetadata& out)
		{
			DolbyVisionTunnelFrameMetadata& dm = out.dm;
			// The tunnel convention (dv_tunnel_fixes): L2 trims take the slots
			// first, L8 fill what is left. First block of L1/L5/L6/L11 wins.
			DolbyVisionTunnelFrameMetadata::Trim l8[DolbyVisionTunnelFrameMetadata::kMaxTrims];
			uint8_t l8Count = 0;
			for (int i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i)
			{
				const auto& ext = md.Extensions[i];
				const uint8_t level = ext.level;
				if (level == 0)
					continue; // unused slot
				++out.extensionCount;
				if (level < 32)
					out.extensionLevelMask |= 1u << level;
				switch (level)
				{
				case 1:
					if (!dm.hasSceneMetadata && ext.Level1.min_pq <= 4095 &&
						ext.Level1.max_pq <= 4095 && ext.Level1.avg_pq <= 4095)
					{
						dm.hasSceneMetadata = true;
						dm.sceneMinPq = ext.Level1.min_pq;
						dm.sceneMaxPq = ext.Level1.max_pq;
						dm.sceneAvgPq = ext.Level1.avg_pq;
					}
					break;
				case 2:
					if (dm.trimCount >= DolbyVisionTunnelFrameMetadata::kMaxTrims)
						dm.trimOverflow = true;
					else
						MapTrimL2(ext, dm.trims[dm.trimCount++]);
					break;
				case 5:
					if (!dm.hasActiveArea && ext.Level5.left_offset <= 0x1FFF &&
						ext.Level5.right_offset <= 0x1FFF && ext.Level5.top_offset <= 0x1FFF &&
						ext.Level5.bottom_offset <= 0x1FFF)
					{
						dm.hasActiveArea = true;
						dm.activeAreaLeft = ext.Level5.left_offset;
						dm.activeAreaRight = ext.Level5.right_offset;
						dm.activeAreaTop = ext.Level5.top_offset;
						dm.activeAreaBottom = ext.Level5.bottom_offset;
					}
					break;
				case 6:
					// Same rules as the tunnel's ParseL6: ST 2084 tops out at
					// 10000 cd/m2; MaxCLL 0 or MaxFALL > MaxCLL = unknown.
					if (!dm.hasL6 && ext.Level6.max_luminance <= 10000 &&
						ext.Level6.max_cll <= 10000 && ext.Level6.max_fall <= 10000)
					{
						dm.hasL6 = true;
						dm.l6MaxDisplayMasteringLuminance = ext.Level6.max_luminance;
						dm.l6MinDisplayMasteringLuminance = ext.Level6.min_luminance;
						dm.hasContentLight = ext.Level6.max_cll > 0 && ext.Level6.max_fall <= ext.Level6.max_cll;
						dm.maxCll = dm.hasContentLight ? ext.Level6.max_cll : 0;
						dm.maxFall = dm.hasContentLight ? ext.Level6.max_fall : 0;
					}
					break;
				case 8:
					if (l8Count >= DolbyVisionTunnelFrameMetadata::kMaxTrims)
						dm.trimOverflow = true;
					else
						MapTrimL8(ext, l8[l8Count++]);
					break;
				case 9:
					if (!out.hasL9)
					{
						const auto& l9 = ext.Level9;
						bool finite = Finite(l9.white_point_x) && Finite(l9.white_point_y);
						for (int p = 0; p < 3; ++p)
							finite = finite && Finite(l9.display_primaries_x[p]) && Finite(l9.display_primaries_y[p]);
						if (!finite)
							break;
						out.hasL9 = true;
						out.l9SourcePrimaryIndex = l9.source_primary_index;
						// LAV: R-G-B order for L9 (unlike MediaSideDataHDR).
						out.l9PrimaryRedX = l9.display_primaries_x[0];
						out.l9PrimaryRedY = l9.display_primaries_y[0];
						out.l9PrimaryGreenX = l9.display_primaries_x[1];
						out.l9PrimaryGreenY = l9.display_primaries_y[1];
						out.l9PrimaryBlueX = l9.display_primaries_x[2];
						out.l9PrimaryBlueY = l9.display_primaries_y[2];
						out.l9WhitePointX = l9.white_point_x;
						out.l9WhitePointY = l9.white_point_y;
					}
					break;
				case 11:
					if (!dm.hasL11)
					{
						dm.hasL11 = true;
						dm.l11ContentType = ext.Level11.content_type & 0x0FU;
						dm.l11Whitepoint = ext.Level11.whitepoint & 0x0FU;
						dm.l11ReferenceMode = ext.Level11.reference_mode_flag != 0;
						// Reconstructed RPU bytes 0-1; libavcodec does not keep
						// the reserved bytes 2-3, so the picture-mode hints
						// (sharpness ... colour) stay 0 = no preference.
						dm.l11Raw[0] = dm.l11ContentType;
						dm.l11Raw[1] = static_cast<uint8_t>(dm.l11Whitepoint | (dm.l11ReferenceMode ? 0x10U : 0U));
					}
					break;
				case 254:
					if (!out.hasL254)
					{
						out.hasL254 = true;
						out.l254DmMode = ext.Level254.dm_mode;
						out.l254DmVersionIndex = ext.Level254.dm_version_index;
					}
					break;
				default:
					break; // L3 / L4 / L10 and unknown: counted only
				}
			}
			for (uint8_t i = 0; i < l8Count; ++i)
			{
				if (dm.trimCount >= DolbyVisionTunnelFrameMetadata::kMaxTrims)
				{
					dm.trimOverflow = true;
					break;
				}
				dm.trims[dm.trimCount++] = l8[i];
			}
		}

		const GUID* const kKnownTypes[] = {
			&IID_MediaSideDataHDR,
			&IID_MediaSideDataHDRContentLightLevel,
			&IID_MediaSideDataHDR10Plus,
			&kLegacyHdr10Plus,
			&IID_MediaSideDataDOVIMetadataV2,
			&kDoviMetadataV1,
		};
	}

	const GUID& LegacyHdr10PlusGuid()
	{
		return kLegacyHdr10Plus;
	}

	const GUID& DoviMetadataV1Guid()
	{
		return kDoviMetadataV1;
	}

	ParseResult ParseMasteringDisplay(const GUID& type, const BYTE* data, size_t size, HDRData& hdr)
	{
		if (!IsEqualGUID(type, IID_MediaSideDataHDR))
			return ParseResult::WrongType;
		const ParseResult check = CheckBuffer(data, size, sizeof(MediaSideDataHDR));
		if (check != ParseResult::Ok)
			return check;
		MediaSideDataHDR in;
		CopyPrefix(in, data, size);
		const double values[] = {
			in.display_primaries_x[0], in.display_primaries_x[1], in.display_primaries_x[2],
			in.display_primaries_y[0], in.display_primaries_y[1], in.display_primaries_y[2],
			in.white_point_x, in.white_point_y,
			in.max_display_mastering_luminance, in.min_display_mastering_luminance };
		for (const double value : values)
			if (!Finite(value) || value < 0.0)
				return ParseResult::Invalid;
		// LAV order is G-B-R (index 0 green, 1 blue, 2 red).
		hdr.displayPrimaryGreenX = in.display_primaries_x[0];
		hdr.displayPrimaryGreenY = in.display_primaries_y[0];
		hdr.displayPrimaryBlueX = in.display_primaries_x[1];
		hdr.displayPrimaryBlueY = in.display_primaries_y[1];
		hdr.displayPrimaryRedX = in.display_primaries_x[2];
		hdr.displayPrimaryRedY = in.display_primaries_y[2];
		hdr.whitePointX = in.white_point_x;
		hdr.whitePointY = in.white_point_y;
		hdr.masteringDisplayMaxLuminance = in.max_display_mastering_luminance;
		hdr.masteringDisplayMinLuminance = in.min_display_mastering_luminance;
		return ParseResult::Ok;
	}

	ParseResult ParseContentLightLevel(const GUID& type, const BYTE* data, size_t size, HDRData& hdr)
	{
		if (!IsEqualGUID(type, IID_MediaSideDataHDRContentLightLevel))
			return ParseResult::WrongType;
		const ParseResult check = CheckBuffer(data, size, sizeof(MediaSideDataHDRContentLightLevel));
		if (check != ParseResult::Ok)
			return check;
		MediaSideDataHDRContentLightLevel in;
		CopyPrefix(in, data, size);
		hdr.maxCll = static_cast<double>(in.MaxCLL);
		hdr.maxFall = static_cast<double>(in.MaxFALL);
		return ParseResult::Ok;
	}

	ParseResult ParseHdr10Plus(const GUID& type, const BYTE* data, size_t size, Hdr10PlusMetadata& out)
	{
		const bool current = IsEqualGUID(type, IID_MediaSideDataHDR10Plus) != 0;
		const bool legacy = !current && IsEqualGUID(type, kLegacyHdr10Plus) != 0;
		if (!current && !legacy)
			return ParseResult::WrongType;
		const ParseResult check = CheckBuffer(data, size, current ? sizeof(MediaSideDataHDR10Plus) : kLegacyHdr10PlusSize);
		if (check != ParseResult::Ok)
			return check;

		// ~11 KB: on the heap, the parser may run on a streaming thread.
		std::unique_ptr<MediaSideDataHDR10Plus> in(new MediaSideDataHDR10Plus);
		memset(in.get(), 0, sizeof(MediaSideDataHDR10Plus));
		if (current)
			memcpy(in.get(), data, sizeof(MediaSideDataHDR10Plus));
		else
			memcpy(reinterpret_cast<BYTE*>(in.get()) + kHdr10PlusIdentificationSize, data, kLegacyHdr10PlusSize);

		if (in->num_windows < 1 || in->num_windows > 3)
			return ParseResult::Invalid;
		const auto& w = in->windows[0];
		if (w.num_distribution_maxrgb_percentiles > 15 || w.num_bezier_curve_anchors > 15)
			return ParseResult::Invalid;

		Hdr10PlusMetadata result;
		result.valid = true;
		result.hasIdentification = current;
		result.countryCode = in->itu_t_t35_country_code;
		result.applicationVersion = in->application_version;
		result.numWindows = in->num_windows;
		for (int i = 0; i < 3; ++i)
		{
			if (!Finite(w.maxscl[i]))
				return ParseResult::Invalid;
			result.maxScl[i] = w.maxscl[i];
			result.sceneMaxNits[i] = 10000.0 * w.maxscl[i];
		}
		if (!Finite(w.average_maxrgb) || !Finite(w.knee_point_x) || !Finite(w.knee_point_y) ||
			!Finite(in->targeted_system_display_maximum_luminance))
			return ParseResult::Invalid;
		result.averageMaxRgb = w.average_maxrgb;
		result.sceneAvgNits = 10000.0 * w.average_maxrgb;
		result.numPercentiles = w.num_distribution_maxrgb_percentiles;
		for (uint32_t i = 0; i < result.numPercentiles; ++i)
		{
			result.percentilePercentage[i] = w.distribution_maxrgb_percentiles[i].percentage;
			result.percentile[i] = w.distribution_maxrgb_percentiles[i].percentile;
		}
		result.fractionBrightPixels = w.fraction_bright_pixels;
		result.toneMapping = w.tone_mapping_flag != 0;
		result.kneePointX = w.knee_point_x;
		result.kneePointY = w.knee_point_y;
		result.numBezierAnchors = w.num_bezier_curve_anchors;
		for (uint32_t i = 0; i < result.numBezierAnchors; ++i)
			result.bezierAnchors[i] = w.bezier_curve_anchors[i];
		result.colorSaturationMapping = w.color_saturation_mapping_flag != 0;
		result.colorSaturationWeight = w.color_saturation_weight;
		result.targetedSystemDisplayMaximumLuminance = in->targeted_system_display_maximum_luminance;
		result.targetedSystemDisplayActualPeakLuminanceFlag = in->targeted_system_display_actual_peak_luminance_flag != 0;
		result.masteringDisplayActualPeakLuminanceFlag = in->mastering_display_actual_peak_luminance_flag != 0;
		out = result;
		return ParseResult::Ok;
	}

	ParseResult ParseDolbyVision(const GUID& type, const BYTE* data, size_t size, DolbyVisionFileMetadata& out)
	{
		const bool v2 = IsEqualGUID(type, IID_MediaSideDataDOVIMetadataV2) != 0;
		const bool v1 = !v2 && IsEqualGUID(type, kDoviMetadataV1) != 0;
		if (!v2 && !v1)
			return ParseResult::WrongType;
		const ParseResult check = CheckBuffer(data, size, v2 ? sizeof(MediaSideDataDOVIMetadata) : kDoviV1Size);
		if (check != ParseResult::Ok)
			return check;

		// ~9 KB: on the heap. V1 copies only the prefix; Extensions stay zero.
		std::unique_ptr<MediaSideDataDOVIMetadata> holder(new MediaSideDataDOVIMetadata);
		MediaSideDataDOVIMetadata& md = *holder;
		CopyPrefix(md, data, v2 ? size : kDoviV1Size);

		const auto& color = md.ColorMetadata;
		for (int i = 0; i < 9; ++i)
			if (!Finite(color.ycc_to_rgb_matrix[i]) || !Finite(color.rgb_to_lms_matrix[i]))
				return ParseResult::Invalid;
		for (int i = 0; i < 3; ++i)
			if (!Finite(color.ycc_to_rgb_offset[i]))
				return ParseResult::Invalid;

		std::unique_ptr<DolbyVisionFileMetadata> resultHolder(new DolbyVisionFileMetadata);
		DolbyVisionFileMetadata& result = *resultHolder;
		result.valid = true;
		result.hasExtensions = v2;

		// Header / base layer.
		result.rpuType = md.Header.rpu_type;
		result.rpuFormat = md.Header.rpu_format;
		result.vdrRpuProfile = md.Header.vdr_rpu_profile;
		result.vdrRpuLevel = md.Header.vdr_rpu_level;
		result.blBitDepth = md.Header.bl_bit_depth;
		result.elBitDepth = md.Header.el_bit_depth;
		result.vdrBitDepth = md.Header.vdr_bit_depth;
		result.blFullRange = md.Header.bl_video_full_range_flag != 0;
		result.disableResidual = md.Header.disable_residual_flag != 0;
		result.elSpatialResampling = md.Header.el_spatial_resampling_filter_flag != 0;
		result.coefLog2Denom = md.Header.coef_log2_denom;
		result.mappingColorSpace = md.Mapping.mapping_color_space;
		result.mappingChromaFormat = md.Mapping.mapping_chroma_format_idc;
		result.guessedProfile = GuessProfile(md);
		// nlq_method_idc is uint8 here; LAV documents -1 (0xFF) as "none".
		result.nlqMethod = md.Mapping.nlq_method_idc;
		result.hasNlq = md.Mapping.nlq_method_idc != 0xFF && !result.disableResidual &&
			md.Mapping.num_x_partitions > 0;
		result.hasReshaping = MapReshaping(md, result.reshaping);

		// Colour metadata -> the tunnel struct (same domains as the tunnel's
		// DM: ycc_to_rgb = coef / 8192, offset = coef / 2^28, rgb_to_lms =
		// coef / 16384, already normalised by libavcodec).
		DolbyVisionTunnelFrameMetadata& dm = result.dm;
		dm.valid = true;
		dm.fullRange = color.signal_full_range_flag != 0; // tunnel convention
		for (int row = 0; row < 3; ++row)
		{
			for (int col = 0; col < 3; ++col)
			{
				dm.nonlinear[row][col] = static_cast<float>(color.ycc_to_rgb_matrix[row * 3 + col]);
				dm.linear[row][col] = static_cast<float>(color.rgb_to_lms_matrix[row * 3 + col]);
			}
			dm.nonlinearOffset[row] = static_cast<float>(color.ycc_to_rgb_offset[row]);
		}
		dm.sourceMinPq = color.source_min_pq;
		dm.sourceMaxPq = color.source_max_pq;
		result.dmMetadataId = color.dm_metadata_id;
		result.sceneRefresh = color.scene_refresh_flag != 0;
		result.signalEotf = color.signal_eotf;
		result.signalBitDepth = color.signal_bit_depth;
		result.signalColorSpace = color.signal_color_space;
		result.signalChromaFormat = color.signal_chroma_format;
		result.signalFullRangeRaw = color.signal_full_range_flag;
		result.sourceDiagonal = color.source_diagonal;

		if (v2)
			MapExtensions(md, result);

		out = result;
		return ParseResult::Ok;
	}

	ParseResult AddToSnapshot(SideDataSnapshot& snapshot, const GUID& type, const BYTE* data, size_t size)
	{
		if (IsEqualGUID(type, IID_MediaSideDataHDR))
		{
			HDRData hdr(snapshot.hdr);
			const ParseResult r = ParseMasteringDisplay(type, data, size, hdr);
			if (r == ParseResult::Ok)
			{
				snapshot.hdr = hdr;
				snapshot.hasMasteringDisplay = true;
			}
			return r;
		}
		if (IsEqualGUID(type, IID_MediaSideDataHDRContentLightLevel))
		{
			HDRData hdr(snapshot.hdr);
			const ParseResult r = ParseContentLightLevel(type, data, size, hdr);
			if (r == ParseResult::Ok)
			{
				snapshot.hdr = hdr;
				snapshot.hasContentLight = true;
			}
			return r;
		}
		if (IsEqualGUID(type, IID_MediaSideDataHDR10Plus) || IsEqualGUID(type, kLegacyHdr10Plus))
		{
			// The current GUID wins over the legacy copy LAV also sends.
			if (snapshot.hasHdr10Plus && snapshot.hdr10Plus.hasIdentification &&
				IsEqualGUID(type, kLegacyHdr10Plus))
				return ParseResult::Ok;
			const ParseResult r = ParseHdr10Plus(type, data, size, snapshot.hdr10Plus);
			if (r == ParseResult::Ok)
				snapshot.hasHdr10Plus = true;
			return r;
		}
		if (IsEqualGUID(type, IID_MediaSideDataDOVIMetadataV2) || IsEqualGUID(type, kDoviMetadataV1))
		{
			if (snapshot.hasDolbyVision && snapshot.dolbyVision.hasExtensions &&
				IsEqualGUID(type, kDoviMetadataV1))
				return ParseResult::Ok;
			const ParseResult r = ParseDolbyVision(type, data, size, snapshot.dolbyVision);
			if (r == ParseResult::Ok)
				snapshot.hasDolbyVision = true;
			return r;
		}
		return ParseResult::WrongType;
	}

	size_t KnownSideDataTypeCount()
	{
		return sizeof(kKnownTypes) / sizeof(kKnownTypes[0]);
	}

	const GUID& KnownSideDataType(size_t index)
	{
		static const GUID kNone = {};
		return index < KnownSideDataTypeCount() ? *kKnownTypes[index] : kNone;
	}

	bool StaticHdrChangeDetector::Update(const SideDataSnapshot& snapshot)
	{
		if (!snapshot.HasStaticHdr())
			return false; // absence keeps the last values in force
		HDRData next(m_current);
		bool hasMastering = m_hasMasteringDisplay;
		bool hasContentLight = m_hasContentLight;
		if (snapshot.hasMasteringDisplay)
		{
			next.displayPrimaryRedX = snapshot.hdr.displayPrimaryRedX;
			next.displayPrimaryRedY = snapshot.hdr.displayPrimaryRedY;
			next.displayPrimaryGreenX = snapshot.hdr.displayPrimaryGreenX;
			next.displayPrimaryGreenY = snapshot.hdr.displayPrimaryGreenY;
			next.displayPrimaryBlueX = snapshot.hdr.displayPrimaryBlueX;
			next.displayPrimaryBlueY = snapshot.hdr.displayPrimaryBlueY;
			next.whitePointX = snapshot.hdr.whitePointX;
			next.whitePointY = snapshot.hdr.whitePointY;
			next.masteringDisplayMaxLuminance = snapshot.hdr.masteringDisplayMaxLuminance;
			next.masteringDisplayMinLuminance = snapshot.hdr.masteringDisplayMinLuminance;
			hasMastering = true;
		}
		if (snapshot.hasContentLight)
		{
			next.maxCll = snapshot.hdr.maxCll;
			next.maxFall = snapshot.hdr.maxFall;
			hasContentLight = true;
		}
		const bool changed = hasMastering != m_hasMasteringDisplay ||
			hasContentLight != m_hasContentLight || next != m_current;
		m_current = next;
		m_hasMasteringDisplay = hasMastering;
		m_hasContentLight = hasContentLight;
		return changed;
	}

	void StaticHdrChangeDetector::Reset()
	{
		m_hasMasteringDisplay = false;
		m_hasContentLight = false;
		m_current = HDRData();
	}
}
