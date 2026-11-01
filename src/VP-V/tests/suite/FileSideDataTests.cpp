#include "pch.h"
#include "CppUnitTest.h"

#include <IMediaSideData.h>
#include <filesource/FileSideData.h>

#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// VP Renderer DirectShow filter: LAV Video side data (IMediaSideData) -> VP
// HDRData / DolbyVisionTunnelFrameMetadata (filesource/FileSideData.h).
namespace Tests
{
	namespace
	{
		using namespace FileSideData;

		template<typename T>
		const BYTE* Bytes(const T& value)
		{
			return reinterpret_cast<const BYTE*>(&value);
		}

		// The struct followed by `extra` bytes of a newer, larger LAV struct.
		template<typename T>
		std::vector<BYTE> Padded(const T& value, size_t extra)
		{
			std::vector<BYTE> buffer(sizeof(T) + extra, 0xA5);
			memcpy(buffer.data(), &value, sizeof(T));
			return buffer;
		}

		MediaSideDataHDR Bt2020Mastering()
		{
			MediaSideDataHDR hdr;
			memset(&hdr, 0, sizeof(hdr));
			// LAV order: G, B, R.
			hdr.display_primaries_x[0] = 0.170;
			hdr.display_primaries_y[0] = 0.797;
			hdr.display_primaries_x[1] = 0.131;
			hdr.display_primaries_y[1] = 0.046;
			hdr.display_primaries_x[2] = 0.708;
			hdr.display_primaries_y[2] = 0.292;
			hdr.white_point_x = 0.3127;
			hdr.white_point_y = 0.3290;
			hdr.max_display_mastering_luminance = 1000.0;
			hdr.min_display_mastering_luminance = 0.0001;
			return hdr;
		}

		MediaSideDataHDRContentLightLevel Cll(unsigned int maxCll, unsigned int maxFall)
		{
			MediaSideDataHDRContentLightLevel cll;
			cll.MaxCLL = maxCll;
			cll.MaxFALL = maxFall;
			return cll;
		}

		std::unique_ptr<MediaSideDataDOVIMetadata> DoviSample()
		{
			std::unique_ptr<MediaSideDataDOVIMetadata> md(new MediaSideDataDOVIMetadata);
			memset(md.get(), 0, sizeof(MediaSideDataDOVIMetadata));
			// Profile 8.1-like header: RPU profile 1, 10-bit limited-range BL.
			md->Header.vdr_rpu_profile = 1;
			md->Header.bl_bit_depth = 10;
			md->Header.el_bit_depth = 10;
			md->Header.vdr_bit_depth = 12;
			md->Header.coef_log2_denom = 23;
			md->Header.disable_residual_flag = 1;
			md->Mapping.nlq_method_idc = 0xFF;
			for (int c = 0; c < 3; ++c)
			{
				auto& curve = md->Mapping.curves[c];
				curve.num_pivots = 2;
				curve.pivots[0] = 0;
				curve.pivots[1] = 1023;
				curve.mapping_idc[0] = 0;
				curve.poly_order[0] = 1;
				curve.poly_coef[0][0] = 0;
				curve.poly_coef[0][1] = 1 << 23; // identity
				curve.poly_coef[0][2] = 12345;   // beyond order 1: ignored
			}
			auto& color = md->ColorMetadata;
			const double ycc[9] = { 1.0, 0.0, 1.5, 1.0, -0.25, -0.5, 1.0, 1.875, 0.0 };
			const double lms[9] = { 0.4, 0.6, 0.0, 0.2, 0.7, 0.1, 0.0, 0.05, 0.95 };
			for (int i = 0; i < 9; ++i)
			{
				color.ycc_to_rgb_matrix[i] = ycc[i];
				color.rgb_to_lms_matrix[i] = lms[i];
			}
			color.ycc_to_rgb_offset[0] = 0.0;
			color.ycc_to_rgb_offset[1] = 0.5;
			color.ycc_to_rgb_offset[2] = 0.5;
			color.signal_bit_depth = 12;
			color.signal_full_range_flag = 1;
			color.source_min_pq = 62;
			color.source_max_pq = 3079;
			color.scene_refresh_flag = 1;
			return md;
		}

		void AddDoviExtensions(MediaSideDataDOVIMetadata& md)
		{
			auto* ext = md.Extensions;
			ext[0].level = 1;
			ext[0].Level1.min_pq = 7;
			ext[0].Level1.max_pq = 2828;
			ext[0].Level1.avg_pq = 1240;
			// L8 before L2: L2 must still take the first trim slot.
			ext[1].level = 8;
			ext[1].Level8.target_display_index = 1;
			ext[1].Level8.trim_slope = 2100;
			ext[1].Level8.trim_offset = 2000;
			ext[1].Level8.trim_power = 2048;
			ext[1].Level8.trim_chroma_weight = 2048;
			ext[1].Level8.trim_saturation_gain = 2048;
			ext[1].Level8.ms_weight = 2048;
			ext[1].Level8.target_mid_contrast = 2048;
			ext[1].Level8.clip_trim = 2048;
			ext[2].level = 2;
			ext[2].Level2.target_max_pq = 2081;
			ext[2].Level2.trim_slope = 2148;
			ext[2].Level2.trim_offset = 1948;
			ext[2].Level2.trim_power = 2000;
			ext[2].Level2.trim_chroma_weight = 2048;
			ext[2].Level2.trim_saturation_gain = 2100;
			ext[2].Level2.ms_weight = -1;
			ext[3].level = 5;
			ext[3].Level5.left_offset = 0;
			ext[3].Level5.right_offset = 0;
			ext[3].Level5.top_offset = 276;
			ext[3].Level5.bottom_offset = 276;
			ext[4].level = 6;
			ext[4].Level6.max_luminance = 1000;
			ext[4].Level6.min_luminance = 50;
			ext[4].Level6.max_cll = 900;
			ext[4].Level6.max_fall = 300;
			ext[5].level = 9;
			ext[5].Level9.source_primary_index = 2;
			ext[5].Level9.display_primaries_x[0] = 0.680; // R-G-B for L9
			ext[5].Level9.display_primaries_y[0] = 0.320;
			ext[5].Level9.display_primaries_x[1] = 0.265;
			ext[5].Level9.display_primaries_y[1] = 0.690;
			ext[5].Level9.display_primaries_x[2] = 0.150;
			ext[5].Level9.display_primaries_y[2] = 0.060;
			ext[5].Level9.white_point_x = 0.3127;
			ext[5].Level9.white_point_y = 0.3290;
			ext[6].level = 11;
			ext[6].Level11.content_type = 1;
			ext[6].Level11.whitepoint = 0;
			ext[6].Level11.reference_mode_flag = 1;
			ext[7].level = 254;
			ext[7].Level254.dm_mode = 0;
			ext[7].Level254.dm_version_index = 2;
			// A second L1 block: the first one wins.
			ext[8].level = 1;
			ext[8].Level1.max_pq = 100;
		}
	}

	TEST_CLASS(FileSideDataTests)
	{
	public:

		TEST_METHOD(MasteringDisplayMapsLavGbrOrderToVpRgb)
		{
			const MediaSideDataHDR in = Bt2020Mastering();
			HDRData hdr;
			hdr.maxCll = 123; // untouched by the mastering parser
			Assert::IsTrue(ParseMasteringDisplay(IID_MediaSideDataHDR, Bytes(in), sizeof(in), hdr) == ParseResult::Ok);
			Assert::AreEqual(0.708, hdr.displayPrimaryRedX);
			Assert::AreEqual(0.292, hdr.displayPrimaryRedY);
			Assert::AreEqual(0.170, hdr.displayPrimaryGreenX);
			Assert::AreEqual(0.797, hdr.displayPrimaryGreenY);
			Assert::AreEqual(0.131, hdr.displayPrimaryBlueX);
			Assert::AreEqual(0.046, hdr.displayPrimaryBlueY);
			Assert::AreEqual(0.3127, hdr.whitePointX);
			Assert::AreEqual(0.3290, hdr.whitePointY);
			Assert::AreEqual(1000.0, hdr.masteringDisplayMaxLuminance);
			Assert::AreEqual(0.0001, hdr.masteringDisplayMinLuminance);
			Assert::AreEqual(123.0, hdr.maxCll);
		}

		TEST_METHOD(MasteringDisplayRejectsNonFiniteValues)
		{
			MediaSideDataHDR in = Bt2020Mastering();
			in.white_point_x = std::numeric_limits<double>::quiet_NaN();
			HDRData hdr;
			Assert::IsTrue(ParseMasteringDisplay(IID_MediaSideDataHDR, Bytes(in), sizeof(in), hdr) == ParseResult::Invalid);
			Assert::AreEqual(0.0, hdr.displayPrimaryRedX);
		}

		TEST_METHOD(ContentLightLevelMapsMaxCllAndMaxFall)
		{
			const MediaSideDataHDRContentLightLevel in = Cll(1000, 400);
			HDRData hdr;
			hdr.masteringDisplayMaxLuminance = 4000;
			Assert::IsTrue(ParseContentLightLevel(IID_MediaSideDataHDRContentLightLevel, Bytes(in), sizeof(in), hdr) == ParseResult::Ok);
			Assert::AreEqual(1000.0, hdr.maxCll);
			Assert::AreEqual(400.0, hdr.maxFall);
			Assert::AreEqual(4000.0, hdr.masteringDisplayMaxLuminance);
		}

		TEST_METHOD(LargerNewerStructsAreAccepted)
		{
			const std::vector<BYTE> hdrBuffer = Padded(Bt2020Mastering(), 32);
			HDRData hdr;
			Assert::IsTrue(ParseMasteringDisplay(IID_MediaSideDataHDR, hdrBuffer.data(), hdrBuffer.size(), hdr) == ParseResult::Ok);
			Assert::AreEqual(0.708, hdr.displayPrimaryRedX);

			const std::vector<BYTE> cllBuffer = Padded(Cll(800, 200), 8);
			Assert::IsTrue(ParseContentLightLevel(IID_MediaSideDataHDRContentLightLevel, cllBuffer.data(), cllBuffer.size(), hdr) == ParseResult::Ok);
			Assert::AreEqual(800.0, hdr.maxCll);

			auto md = DoviSample();
			AddDoviExtensions(*md);
			const std::vector<BYTE> doviBuffer = Padded(*md, 256);
			DolbyVisionFileMetadata dv;
			Assert::IsTrue(ParseDolbyVision(IID_MediaSideDataDOVIMetadataV2, doviBuffer.data(), doviBuffer.size(), dv) == ParseResult::Ok);
			Assert::IsTrue(dv.dm.hasSceneMetadata);
			Assert::AreEqual<int>(2828, dv.dm.sceneMaxPq);
		}

		TEST_METHOD(ShortNullAndForeignBuffersAreRejected)
		{
			const MediaSideDataHDR in = Bt2020Mastering();
			HDRData hdr;
			Assert::IsTrue(ParseMasteringDisplay(IID_MediaSideDataHDR, Bytes(in), sizeof(in) - 1, hdr) == ParseResult::TooShort);
			Assert::IsTrue(ParseMasteringDisplay(IID_MediaSideDataHDR, nullptr, sizeof(in), hdr) == ParseResult::NullData);
			Assert::IsTrue(ParseMasteringDisplay(IID_MediaSideDataHDR, Bytes(in), 0, hdr) == ParseResult::NullData);
			Assert::IsTrue(ParseMasteringDisplay(IID_MediaSideDataHDRContentLightLevel, Bytes(in), sizeof(in), hdr) == ParseResult::WrongType);
			Assert::AreEqual(0.0, hdr.displayPrimaryRedX);

			const MediaSideDataHDRContentLightLevel cll = Cll(1000, 400);
			Assert::IsTrue(ParseContentLightLevel(IID_MediaSideDataHDRContentLightLevel, Bytes(cll), sizeof(cll) - 1, hdr) == ParseResult::TooShort);
			Assert::AreEqual(0.0, hdr.maxCll);

			auto md = DoviSample();
			DolbyVisionFileMetadata dv;
			// A V1-sized buffer is too short for the V2 GUID.
			Assert::IsTrue(ParseDolbyVision(IID_MediaSideDataDOVIMetadataV2, Bytes(*md), offsetof(MediaSideDataDOVIMetadata, Extensions), dv) == ParseResult::TooShort);
			Assert::IsFalse(dv.valid);

			std::unique_ptr<MediaSideDataHDR10Plus> plus(new MediaSideDataHDR10Plus);
			memset(plus.get(), 0, sizeof(MediaSideDataHDR10Plus));
			plus->num_windows = 1;
			Hdr10PlusMetadata meta;
			Assert::IsTrue(ParseHdr10Plus(IID_MediaSideDataHDR10Plus, Bytes(*plus), sizeof(MediaSideDataHDR10Plus) - 1, meta) == ParseResult::TooShort);
			Assert::IsFalse(meta.valid);
		}

		TEST_METHOD(DolbyVisionV2MapsToTunnelMetadata)
		{
			auto md = DoviSample();
			AddDoviExtensions(*md);
			DolbyVisionFileMetadata dv;
			Assert::IsTrue(ParseDolbyVision(IID_MediaSideDataDOVIMetadataV2, Bytes(*md), sizeof(*md), dv) == ParseResult::Ok);
			Assert::IsTrue(dv.valid);
			Assert::IsTrue(dv.hasExtensions);

			const DolbyVisionTunnelFrameMetadata& dm = dv.dm;
			Assert::IsTrue(dm.valid);
			Assert::IsTrue(dm.fullRange);
			Assert::AreEqual(1.5f, dm.nonlinear[0][2]);
			Assert::AreEqual(-0.25f, dm.nonlinear[1][1]);
			Assert::AreEqual(1.875f, dm.nonlinear[2][1]);
			Assert::AreEqual(0.5f, dm.nonlinearOffset[1]);
			Assert::AreEqual(0.7f, dm.linear[1][1]);
			Assert::AreEqual(0.95f, dm.linear[2][2]);
			Assert::AreEqual<int>(62, dm.sourceMinPq);
			Assert::AreEqual<int>(3079, dm.sourceMaxPq);

			// L1 (first block wins).
			Assert::IsTrue(dm.hasSceneMetadata);
			Assert::AreEqual<int>(7, dm.sceneMinPq);
			Assert::AreEqual<int>(2828, dm.sceneMaxPq);
			Assert::AreEqual<int>(1240, dm.sceneAvgPq);

			// L2 first, then L8.
			Assert::AreEqual<int>(2, dm.trimCount);
			Assert::IsFalse(dm.trimOverflow);
			Assert::AreEqual<int>(2, dm.trims[0].level);
			Assert::AreEqual<int>(2081, dm.trims[0].targetMaxPq);
			Assert::AreEqual<int>(2148, dm.trims[0].slope);
			Assert::AreEqual<int>(1948, dm.trims[0].offset);
			Assert::AreEqual<int>(2000, dm.trims[0].power);
			Assert::AreEqual<int>(2100, dm.trims[0].saturationGain);
			Assert::AreEqual<int>(-1, dm.trims[0].msWeight);
			Assert::AreEqual<int>(8, dm.trims[1].level);
			Assert::AreEqual<int>(1, dm.trims[1].targetDisplayIndex);
			Assert::AreEqual<int>(2100, dm.trims[1].slope);
			Assert::IsTrue(dm.trims[1].hasMidContrastAndClip);
			Assert::IsFalse(dm.trims[1].hasSaturationVector);
			Assert::IsFalse(dm.trims[1].hasHueVector);

			// L5.
			Assert::IsTrue(dm.hasActiveArea);
			Assert::AreEqual<int>(0, dm.activeAreaLeft);
			Assert::AreEqual<int>(276, dm.activeAreaTop);
			Assert::AreEqual<int>(276, dm.activeAreaBottom);

			// L6.
			Assert::IsTrue(dm.hasL6);
			Assert::AreEqual<int>(1000, dm.l6MaxDisplayMasteringLuminance);
			Assert::AreEqual<int>(50, dm.l6MinDisplayMasteringLuminance);
			Assert::IsTrue(dm.hasContentLight);
			Assert::AreEqual<int>(900, dm.maxCll);
			Assert::AreEqual<int>(300, dm.maxFall);

			// L11.
			Assert::IsTrue(dm.hasL11);
			Assert::AreEqual<int>(1, dm.l11ContentType);
			Assert::IsTrue(dm.l11ReferenceMode);
			Assert::AreEqual<int>(0x10, dm.l11Raw[1]);

			// L9 / L254 / header extras outside the tunnel struct.
			Assert::IsTrue(dv.hasL9);
			Assert::AreEqual<int>(2, dv.l9SourcePrimaryIndex);
			Assert::AreEqual(0.680, dv.l9PrimaryRedX);
			Assert::AreEqual(0.690, dv.l9PrimaryGreenY);
			Assert::AreEqual(0.150, dv.l9PrimaryBlueX);
			Assert::IsTrue(dv.hasL254);
			Assert::AreEqual<int>(2, dv.l254DmVersionIndex);
			Assert::AreEqual<int>(9, dv.extensionCount);
			Assert::IsTrue(((1u << 1) | (1u << 2) | (1u << 5) | (1u << 6) | (1u << 8) | (1u << 9) | (1u << 11)) ==
				dv.extensionLevelMask);
			Assert::AreEqual<int>(8, dv.guessedProfile);
			Assert::AreEqual<int>(10, dv.blBitDepth);
			Assert::IsFalse(dv.blFullRange);
			Assert::IsFalse(dv.hasNlq);
			Assert::IsTrue(dv.sceneRefresh);
		}

		TEST_METHOD(DolbyVisionReshapingNormalisedLikeLibplacebo)
		{
			auto md = DoviSample();
			md->Header.vdr_rpu_profile = 0;
			md->Header.bl_video_full_range_flag = 1; // profile 5
			auto& curve = md->Mapping.curves[1];
			curve.num_pivots = 3;
			curve.pivots[1] = 512;
			curve.pivots[2] = 1023;
			curve.mapping_idc[1] = 1; // MMR
			curve.mmr_order[1] = 2;
			curve.mmr_constant[1] = 1 << 22;      // 0.5
			curve.mmr_coef[1][0][0] = 1 << 21;    // 0.25
			curve.mmr_coef[1][1][6] = -(1 << 23); // -1
			DolbyVisionFileMetadata dv;
			Assert::IsTrue(ParseDolbyVision(IID_MediaSideDataDOVIMetadataV2, Bytes(*md), sizeof(*md), dv) == ParseResult::Ok);
			Assert::AreEqual<int>(5, dv.guessedProfile);
			Assert::IsTrue(dv.hasReshaping);
			const auto& luma = dv.reshaping.comp[0];
			Assert::AreEqual<int>(2, luma.numPivots);
			Assert::AreEqual(0.0f, luma.pivots[0]);
			Assert::AreEqual(1.0f, luma.pivots[1]);
			Assert::AreEqual(0.0f, luma.polyCoeffs[0][0]);
			Assert::AreEqual(1.0f, luma.polyCoeffs[0][1]);
			Assert::AreEqual(0.0f, luma.polyCoeffs[0][2]); // above poly_order
			const auto& cb = dv.reshaping.comp[1];
			Assert::AreEqual<int>(3, cb.numPivots);
			Assert::AreEqual(512.0f / 1023.0f, cb.pivots[1], 1e-6f);
			Assert::AreEqual<int>(1, cb.method[1]);
			Assert::AreEqual<int>(2, cb.mmrOrder[1]);
			Assert::AreEqual(0.5f, cb.mmrConstant[1]);
			Assert::AreEqual(0.25f, cb.mmrCoeffs[1][0][0]);
			Assert::AreEqual(-1.0f, cb.mmrCoeffs[1][1][6]);

			// Descending pivots: no reshaping, the DM is still delivered.
			curve.pivots[2] = 100;
			DolbyVisionFileMetadata bad;
			Assert::IsTrue(ParseDolbyVision(IID_MediaSideDataDOVIMetadataV2, Bytes(*md), sizeof(*md), bad) == ParseResult::Ok);
			Assert::IsFalse(bad.hasReshaping);
			Assert::IsTrue(bad.dm.valid);
		}

		TEST_METHOD(DolbyVisionV1HasNoExtensions)
		{
			auto md = DoviSample();
			AddDoviExtensions(*md); // ignored: V1 ends before Extensions
			DolbyVisionFileMetadata dv;
			Assert::IsTrue(ParseDolbyVision(DoviMetadataV1Guid(), Bytes(*md), offsetof(MediaSideDataDOVIMetadata, Extensions), dv) == ParseResult::Ok);
			Assert::IsTrue(dv.valid);
			Assert::IsFalse(dv.hasExtensions);
			Assert::IsFalse(dv.dm.hasSceneMetadata);
			Assert::AreEqual<int>(0, dv.dm.trimCount);
			Assert::AreEqual<int>(3079, dv.dm.sourceMaxPq);
		}

		TEST_METHOD(DolbyVisionInvalidExtensionBlocksAreSkipped)
		{
			auto md = DoviSample();
			md->Extensions[0].level = 1;
			md->Extensions[0].Level1.max_pq = 5000; // > 12 bits
			md->Extensions[1].level = 6;
			md->Extensions[1].Level6.max_luminance = 20000; // > ST 2084 range
			md->Extensions[2].level = 6;
			md->Extensions[2].Level6.max_luminance = 4000;
			md->Extensions[2].Level6.max_cll = 100;
			md->Extensions[2].Level6.max_fall = 200; // MaxFALL > MaxCLL: unknown
			for (int i = 0; i < 10; ++i)
			{
				md->Extensions[3 + i].level = 2;
				md->Extensions[3 + i].Level2.target_max_pq = static_cast<uint16_t>(2000 + i);
			}
			DolbyVisionFileMetadata dv;
			Assert::IsTrue(ParseDolbyVision(IID_MediaSideDataDOVIMetadataV2, Bytes(*md), sizeof(*md), dv) == ParseResult::Ok);
			Assert::IsFalse(dv.dm.hasSceneMetadata);
			Assert::IsTrue(dv.dm.hasL6);
			Assert::AreEqual<int>(4000, dv.dm.l6MaxDisplayMasteringLuminance);
			Assert::IsFalse(dv.dm.hasContentLight);
			Assert::AreEqual<int>(static_cast<int>(DolbyVisionTunnelFrameMetadata::kMaxTrims), dv.dm.trimCount);
			Assert::IsTrue(dv.dm.trimOverflow);
		}

		TEST_METHOD(Hdr10PlusCurrentAndLegacyLayouts)
		{
			std::unique_ptr<MediaSideDataHDR10Plus> plus(new MediaSideDataHDR10Plus);
			memset(plus.get(), 0, sizeof(MediaSideDataHDR10Plus));
			plus->itu_t_t35_country_code = 0xB5;
			plus->application_version = 1;
			plus->num_windows = 1;
			plus->windows[0].maxscl[0] = 0.1;
			plus->windows[0].maxscl[1] = 0.05;
			plus->windows[0].maxscl[2] = 0.02;
			plus->windows[0].average_maxrgb = 0.01;
			plus->windows[0].num_distribution_maxrgb_percentiles = 2;
			plus->windows[0].distribution_maxrgb_percentiles[1].percentage = 99;
			plus->windows[0].distribution_maxrgb_percentiles[1].percentile = 0.08;
			plus->windows[0].tone_mapping_flag = 1;
			plus->windows[0].knee_point_x = 0.25;
			plus->windows[0].knee_point_y = 0.5;
			plus->windows[0].num_bezier_curve_anchors = 3;
			plus->windows[0].bezier_curve_anchors[2] = 0.75;
			plus->targeted_system_display_maximum_luminance = 400.0;

			Hdr10PlusMetadata meta;
			Assert::IsTrue(ParseHdr10Plus(IID_MediaSideDataHDR10Plus, Bytes(*plus), sizeof(*plus), meta) == ParseResult::Ok);
			Assert::IsTrue(meta.valid);
			Assert::IsTrue(meta.hasIdentification);
			Assert::IsTrue(meta.countryCode == 0xB5u);
			Assert::AreEqual(1000.0, meta.sceneMaxNits[0], 1e-9);
			Assert::AreEqual(100.0, meta.sceneAvgNits, 1e-9);
			Assert::IsTrue(meta.percentilePercentage[1] == 99u);
			Assert::IsTrue(meta.toneMapping);
			Assert::AreEqual(0.5, meta.kneePointY);
			Assert::IsTrue(meta.numBezierAnchors == 3u);
			Assert::AreEqual(0.75, meta.bezierAnchors[2]);
			Assert::AreEqual(400.0, meta.targetedSystemDisplayMaximumLuminance);

			// Legacy GUID: the same struct without the 8 identification bytes.
			const BYTE* legacy = Bytes(*plus) + 2 * sizeof(unsigned int);
			const size_t legacySize = sizeof(*plus) - 2 * sizeof(unsigned int);
			Hdr10PlusMetadata old;
			Assert::IsTrue(ParseHdr10Plus(LegacyHdr10PlusGuid(), legacy, legacySize, old) == ParseResult::Ok);
			Assert::IsFalse(old.hasIdentification);
			Assert::AreEqual(1000.0, old.sceneMaxNits[0], 1e-9);
			Assert::AreEqual(400.0, old.targetedSystemDisplayMaximumLuminance);

			plus->num_windows = 0;
			Hdr10PlusMetadata invalid;
			Assert::IsTrue(ParseHdr10Plus(IID_MediaSideDataHDR10Plus, Bytes(*plus), sizeof(*plus), invalid) == ParseResult::Invalid);
			Assert::IsFalse(invalid.valid);
		}

		TEST_METHOD(SnapshotRoutesKnownTypes)
		{
			SideDataSnapshot snapshot;
			const MediaSideDataHDR hdr = Bt2020Mastering();
			const MediaSideDataHDRContentLightLevel cll = Cll(1000, 400);
			Assert::IsTrue(AddToSnapshot(snapshot, IID_MediaSideDataHDR, Bytes(hdr), sizeof(hdr)) == ParseResult::Ok);
			Assert::IsTrue(AddToSnapshot(snapshot, IID_MediaSideDataHDRContentLightLevel, Bytes(cll), sizeof(cll)) == ParseResult::Ok);
			Assert::IsTrue(snapshot.HasStaticHdr());
			Assert::IsTrue(snapshot.hdr.IsValid());
			Assert::AreEqual(0.708, snapshot.hdr.displayPrimaryRedX);
			Assert::AreEqual(1000.0, snapshot.hdr.maxCll);
			Assert::IsTrue(AddToSnapshot(snapshot, IID_MediaSideDataEIA608CC, Bytes(cll), sizeof(cll)) == ParseResult::WrongType);
			Assert::IsTrue(AddToSnapshot(snapshot, IID_MediaSideDataHDR, Bytes(hdr), 3) == ParseResult::TooShort);
			Assert::IsTrue(snapshot.hasMasteringDisplay); // previous value kept

			Assert::AreEqual<size_t>(6, KnownSideDataTypeCount());
			Assert::IsTrue(IsEqualGUID(KnownSideDataType(2), IID_MediaSideDataHDR10Plus) != 0);
			Assert::IsTrue(IsEqualGUID(KnownSideDataType(3), LegacyHdr10PlusGuid()) != 0);
		}

		TEST_METHOD(StaticHdrChangeDetectorEmitsOnlyOnChange)
		{
			StaticHdrChangeDetector detector;
			SideDataSnapshot none;
			Assert::IsFalse(detector.Update(none));

			SideDataSnapshot full;
			const MediaSideDataHDR hdr = Bt2020Mastering();
			const MediaSideDataHDRContentLightLevel cll = Cll(1000, 400);
			AddToSnapshot(full, IID_MediaSideDataHDR, Bytes(hdr), sizeof(hdr));
			AddToSnapshot(full, IID_MediaSideDataHDRContentLightLevel, Bytes(cll), sizeof(cll));
			Assert::IsTrue(detector.Update(full));
			Assert::IsFalse(detector.Update(full));
			// A frame without static metadata keeps the last values.
			Assert::IsFalse(detector.Update(none));
			Assert::AreEqual(1000.0, detector.Current().maxCll);

			// Dynamic data alone never triggers a new VideoState.
			auto md = DoviSample();
			SideDataSnapshot dynamicOnly;
			AddToSnapshot(dynamicOnly, IID_MediaSideDataDOVIMetadataV2, Bytes(*md), sizeof(*md));
			Assert::IsTrue(dynamicOnly.hasDolbyVision);
			Assert::IsFalse(detector.Update(dynamicOnly));

			// Content light only, changed: a change; mastering stays merged.
			SideDataSnapshot cllOnly;
			const MediaSideDataHDRContentLightLevel brighter = Cll(4000, 400);
			AddToSnapshot(cllOnly, IID_MediaSideDataHDRContentLightLevel, Bytes(brighter), sizeof(brighter));
			Assert::IsTrue(detector.Update(cllOnly));
			Assert::AreEqual(4000.0, detector.Current().maxCll);
			Assert::AreEqual(0.708, detector.Current().displayPrimaryRedX);
			Assert::IsTrue(detector.HasMasteringDisplay());

			detector.Reset();
			Assert::IsFalse(detector.HasMasteringDisplay());
			Assert::IsTrue(detector.Update(full));
		}
	};
}
