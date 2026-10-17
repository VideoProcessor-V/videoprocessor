#include "pch.h"
#include "CppUnitTest.h"
#include <SceneDescriptor.h>
#include <DisplayProfile.h>
#include <MappingPolicy.h>
#include <RendererProfileConfig.h>
#include <vprenderer/DolbyVisionLuminancePolicy.h>
#include <cmath>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// DV next step (f), 2026-10-17: the format-neutral scene descriptor, display
// profile and mapping policy. The mapping must reproduce, bit for bit, the HDR
// metadata the renderer gives libplacebo today (TranslateColorSpace's static
// values, then DolbyVisionLuminancePolicy::Apply on the DV tunnel).
namespace Tests
{
    namespace
    {
        // Mirror of the renderer's TranslateColorSpace HDR block
        // (LibplaceboVideoRenderer.cpp, anonymous namespace): the reference.
        pl_hdr_metadata ReferenceStaticHdr(const HDRData* hdrData)
        {
            pl_color_space result{};
            if (hdrData && hdrData->IsValid())
            {
                const HDRData& hdr = *hdrData;
                result.hdr.prim.red.x = static_cast<float>(hdr.displayPrimaryRedX);
                result.hdr.prim.red.y = static_cast<float>(hdr.displayPrimaryRedY);
                result.hdr.prim.green.x = static_cast<float>(hdr.displayPrimaryGreenX);
                result.hdr.prim.green.y = static_cast<float>(hdr.displayPrimaryGreenY);
                result.hdr.prim.blue.x = static_cast<float>(hdr.displayPrimaryBlueX);
                result.hdr.prim.blue.y = static_cast<float>(hdr.displayPrimaryBlueY);
                result.hdr.prim.white.x = static_cast<float>(hdr.whitePointX);
                result.hdr.prim.white.y = static_cast<float>(hdr.whitePointY);
                result.hdr.min_luma = static_cast<float>(hdr.masteringDisplayMinLuminance);
                result.hdr.max_luma = static_cast<float>(hdr.masteringDisplayMaxLuminance);
                result.hdr.max_cll = static_cast<float>(hdr.maxCll);
                result.hdr.max_fall = static_cast<float>(hdr.maxFall);
            }
            else if (hdrData)
            {
                const HDRData& hdr = *hdrData;
                if (hdr.maxCll > 0) result.hdr.max_cll = static_cast<float>(hdr.maxCll);
                if (hdr.maxFall > 0) result.hdr.max_fall = static_cast<float>(hdr.maxFall);
                if (hdr.masteringDisplayMaxLuminance > 0)
                {
                    result.hdr.max_luma = static_cast<float>(hdr.masteringDisplayMaxLuminance);
                    if (hdr.masteringDisplayMinLuminance > 0)
                        result.hdr.min_luma = static_cast<float>(hdr.masteringDisplayMinLuminance);
                }
            }
            return result.hdr;
        }

        HDRData Bt2020Master(double maxNits, double minNits, double cll, double fall)
        {
            HDRData hdr;
            hdr.displayPrimaryRedX = 0.708; hdr.displayPrimaryRedY = 0.292;
            hdr.displayPrimaryGreenX = 0.170; hdr.displayPrimaryGreenY = 0.797;
            hdr.displayPrimaryBlueX = 0.131; hdr.displayPrimaryBlueY = 0.046;
            hdr.whitePointX = 0.3127; hdr.whitePointY = 0.3290;
            hdr.masteringDisplayMaxLuminance = maxNits;
            hdr.masteringDisplayMinLuminance = minNits;
            hdr.maxCll = cll;
            hdr.maxFall = fall;
            return hdr;
        }

        std::vector<HDRData> StaticCases()
        {
            std::vector<HDRData> cases;
            cases.push_back(Bt2020Master(1000.0, 0.0001, 1000.0, 400.0));
            cases.push_back(Bt2020Master(4000.0, 0.005, 0.0, 0.0));
            cases.push_back(Bt2020Master(1000.0, 0.0, 0.0, 0.0));
            HDRData zeroPrimaries;  // the audit 2026-10-16 partial InfoFrame
            zeroPrimaries.masteringDisplayMaxLuminance = 1000.0;
            zeroPrimaries.masteringDisplayMinLuminance = 0.05;
            zeroPrimaries.maxCll = 1200.0;
            cases.push_back(zeroPrimaries);
            HDRData minWithoutMax;
            minWithoutMax.masteringDisplayMinLuminance = 0.05;
            minWithoutMax.maxFall = 300.0;
            cases.push_back(minWithoutMax);
            cases.push_back(HDRData{});  // the mandatory empty object
            return cases;
        }

        DolbyVisionTunnelFrameMetadata L1(uint16_t srcMin, uint16_t srcMax,
            bool scene, uint16_t sMin, uint16_t sMax, uint16_t sAvg)
        {
            DolbyVisionTunnelFrameMetadata dm;
            dm.valid = true;
            dm.sourceMinPq = srcMin;
            dm.sourceMaxPq = srcMax;
            dm.hasSceneMetadata = scene;
            dm.sceneMinPq = sMin;
            dm.sceneMaxPq = sMax;
            dm.sceneAvgPq = sAvg;
            return dm;
        }

        std::vector<DolbyVisionTunnelFrameMetadata> DvCases()
        {
            std::vector<DolbyVisionTunnelFrameMetadata> cases;
            cases.push_back(L1(62, 3696, true, 0, 2771, 1229));     // typical 1000-nit master
            cases.push_back(L1(7, 3079, true, 12, 3079, 2048));
            cases.push_back(L1(0, 4095, true, 4095, 4095, 4095));   // edges
            cases.push_back(L1(0, 0, true, 0, 0, 0));               // all black: range unusable
            cases.push_back(L1(3000, 2000, true, 10, 2000, 900));   // max <= min: fallback range
            cases.push_back(L1(0, 4096, true, 10, 2000, 900));      // max out of range
            cases.push_back(L1(62, 3696, false, 10, 2000, 900));    // no L1
            cases.push_back(L1(62, 3696, true, 10, 4096, 900));     // L1 max out of range
            cases.push_back(L1(62, 3696, true, 10, 2000, 4096));    // L1 avg out of range
            cases.push_back(L1(62, 3696, true, 4096, 2000, 900));   // L1 min out of range only
            DolbyVisionTunnelFrameMetadata invalid = L1(62, 3696, true, 10, 2000, 900);
            invalid.valid = false;
            cases.push_back(invalid);
            return cases;
        }

        DisplayProfile Fl35Profile()
        {
            DisplayProfileRendererSettings settings;
            settings.targetNits = 120.0;
            settings.resolvedBlackNits = 0.005;
            settings.outputEotf = DisplayEotf::Bt1886;
            settings.targetGamut = DisplayGamut::Bt709;
            settings.displayBitDepth = "8";
            return DisplayProfiles::FromRendererSettings(settings);
        }

        std::wstring Describe(const pl_hdr_metadata& a, const pl_hdr_metadata& b)
        {
            return L"mapped min/max/cll/fall/maxpq/avgpq " + std::to_wstring(a.min_luma) + L"/" +
                std::to_wstring(a.max_luma) + L"/" + std::to_wstring(a.max_cll) + L"/" +
                std::to_wstring(a.max_fall) + L"/" + std::to_wstring(a.max_pq_y) + L"/" +
                std::to_wstring(a.avg_pq_y) + L" vs reference " + std::to_wstring(b.min_luma) + L"/" +
                std::to_wstring(b.max_luma) + L"/" + std::to_wstring(b.max_cll) + L"/" +
                std::to_wstring(b.max_fall) + L"/" + std::to_wstring(b.max_pq_y) + L"/" +
                std::to_wstring(b.avg_pq_y);
        }
    }

    TEST_CLASS(SceneDescriptorTests)
    {
    public:
        TEST_METHOD(StaticParserMappingReproducesTodaysSourceMetadata)
        {
            const DisplayProfile display = Fl35Profile();
            for (const HDRData& hdr : StaticCases())
            {
                const SceneDescriptor d = SceneDescriptorParsers::FromStaticHdr(&hdr);
                Assert::IsTrue(d.source == SceneMetadataSource::Hdr10Static);
                const pl_hdr_metadata mapped = MappingPolicies::ToLibplaceboHdrMetadata(
                    d, display, MappingPolicies::Default());
                const pl_hdr_metadata reference = ReferenceStaticHdr(&hdr);
                Assert::IsTrue(MappingPolicies::SameHdrMetadataBits(mapped, reference),
                    Describe(mapped, reference).c_str());
            }
            const SceneDescriptor none = SceneDescriptorParsers::FromStaticHdr(nullptr);
            Assert::IsTrue(none.source == SceneMetadataSource::None);
            Assert::IsFalse(none.valid);
            const pl_hdr_metadata zero{};
            Assert::IsTrue(MappingPolicies::SameHdrMetadataBits(zero,
                MappingPolicies::ToLibplaceboHdrMetadata(none, display, MappingPolicies::Default())));
        }

        TEST_METHOD(StaticParserFields)
        {
            const HDRData hdr = Bt2020Master(1000.0, 0.0001, 1100.0, 400.0);
            const SceneDescriptor d = SceneDescriptorParsers::FromStaticHdr(&hdr);
            Assert::IsTrue(d.valid && d.mastering.hasPrimaries);
            Assert::IsFalse(d.mastering.rangeFromDynamicMetadata);
            Assert::AreEqual(1000.0f, d.mastering.peak.nits);
            Assert::AreEqual(0.0001f, d.mastering.min.nits);
            Assert::AreEqual(1100.0f, d.maxCll.nits);
            Assert::AreEqual(400.0f, d.maxFall.nits);
            Assert::IsFalse(d.mastering.peak.fromPq);
            Assert::AreEqual(0.7518f, d.mastering.peak.pq, 0.0005f);  // ST 2084 of 1000 nits
            Assert::IsFalse(d.HasSceneStats());
            Assert::AreEqual(0.708f, d.mastering.redX);
        }

        TEST_METHOD(DvTunnelMappingReproducesApplyBitForBit)
        {
            const DisplayProfile display = Fl35Profile();
            std::vector<const HDRData*> fallbacks;
            const std::vector<HDRData> statics = StaticCases();
            fallbacks.push_back(nullptr);
            for (const HDRData& hdr : statics)
                fallbacks.push_back(&hdr);
            int checked = 0;
            for (const HDRData* fallback : fallbacks)
                for (const DolbyVisionTunnelFrameMetadata& dm : DvCases())
                {
                    pl_hdr_metadata reference = ReferenceStaticHdr(fallback);
                    const DolbyVisionLuminanceStatus status =
                        DolbyVisionLuminancePolicy::ApplyWithSceneMin(dm, reference);
                    const SceneDescriptor d = SceneDescriptorParsers::FromDolbyVisionTunnel(
                        dm, SceneDescriptorParsers::FromStaticHdr(fallback));
                    const pl_hdr_metadata mapped = MappingPolicies::ToLibplaceboHdrMetadata(
                        d, display, MappingPolicies::Default());
                    Assert::IsTrue(MappingPolicies::SameHdrMetadataBits(mapped, reference),
                        Describe(mapped, reference).c_str());
                    Assert::IsTrue(d.source == SceneMetadataSource::DvTunnelL1);
                    Assert::AreEqual(status.available, d.valid);
                    Assert::AreEqual(status.sourceRangeFromMetadata, d.mastering.rangeFromDynamicMetadata);
                    Assert::AreEqual(status.sceneAvailable, d.HasSceneStats());
                    Assert::AreEqual(status.sceneMinAvailable, d.sceneMin.present);
                    Assert::IsFalse(d.maxCll.present || d.maxFall.present);
                    if (d.HasSceneStats())
                    {
                        Assert::AreEqual(status.sceneMax, MappingPolicies::LibplaceboNits(d.sceneMax));
                        Assert::AreEqual(status.sceneAvg, MappingPolicies::LibplaceboNits(d.sceneAvg));
                    }
                    if (d.sceneMin.present)
                        Assert::AreEqual(status.sceneMin, MappingPolicies::LibplaceboNits(d.sceneMin));
                    ++checked;
                }
            Logger::WriteMessage(("DV mapping cases bit-identical to Apply: " +
                std::to_string(checked) + "\n").c_str());
        }

        TEST_METHOD(DvParserL1CodesAndNitsAcrossTheRange)
        {
            // Every 12-bit code: PQ exactly code/4095 (as Apply), and VP's own
            // ST 2084 nits close to libplacebo's conversion.
            for (int code = 0; code <= 4095; ++code)
            {
                const auto dm = L1(0, 4095, true, static_cast<uint16_t>(code),
                    static_cast<uint16_t>(code), static_cast<uint16_t>(code));
                const SceneDescriptor d = SceneDescriptorParsers::FromDolbyVisionTunnel(dm, SceneDescriptor{});
                Assert::IsTrue(d.sceneMax.fromPq && d.sceneMax.pqCode12 == code);
                Assert::AreEqual(static_cast<float>(code) / 4095.0f, d.sceneMax.pq);
                const float pl = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, d.sceneMax.pq);
                const float tolerance = 1e-4f * pl + 1e-6f;
                Assert::AreEqual(pl, d.sceneMax.nits, tolerance,
                    (L"code " + std::to_wstring(code)).c_str());
            }
        }

        TEST_METHOD(DvParserCarriesTrims)
        {
            DolbyVisionTunnelFrameMetadata dm = L1(62, 3696, true, 0, 2771, 1229);
            dm.trimCount = 2;
            dm.trims[0].level = 2;
            dm.trims[0].targetMaxPq = 2081;  // ~100 nits
            dm.trims[0].slope = 2100; dm.trims[0].offset = 2000; dm.trims[0].power = 2048;
            dm.trims[0].chromaWeight = 2048; dm.trims[0].saturationGain = 2100; dm.trims[0].msWeight = -1;
            dm.trims[1].level = 8;
            dm.trims[1].targetDisplayIndex = 3;
            dm.trims[1].slope = 1900;
            const SceneDescriptor d = SceneDescriptorParsers::FromDolbyVisionTunnel(dm, SceneDescriptor{});
            Assert::AreEqual(2, static_cast<int>(d.trimCount));
            Assert::IsFalse(d.trimOverflow);
            Assert::AreEqual(2, static_cast<int>(d.trims[0].level));
            Assert::IsTrue(d.trims[0].target.present);
            Assert::AreEqual(100.0f, d.trims[0].target.nits, 2.0f);
            Assert::AreEqual(2100, static_cast<int>(d.trims[0].slope));
            Assert::AreEqual(2100, static_cast<int>(d.trims[0].saturationGain));
            Assert::AreEqual(-1, static_cast<int>(d.trims[0].msWeight));
            Assert::AreEqual(8, static_cast<int>(d.trims[1].level));
            Assert::AreEqual(3, static_cast<int>(d.trims[1].targetDisplayIndex));
            Assert::IsFalse(d.trims[1].target.present);
            // Trims never enter libplacebo's source metadata (the dv_trims stage owns them).
            DolbyVisionTunnelFrameMetadata noTrims = dm;
            noTrims.trimCount = 0;
            MappingPolicy withTrims = MappingPolicies::Default();
            withTrims.useTrims = true;
            Assert::IsTrue(MappingPolicies::SameHdrMetadataBits(
                MappingPolicies::ToLibplaceboHdrMetadata(d, Fl35Profile(), withTrims),
                MappingPolicies::ToLibplaceboHdrMetadata(
                    SceneDescriptorParsers::FromDolbyVisionTunnel(noTrims, SceneDescriptor{}),
                    Fl35Profile(), MappingPolicies::Default())));
        }

        TEST_METHOD(LldvParserReproducesTheSubstitutedStaticMetadata)
        {
            const DisplayProfile display = Fl35Profile();
            const RendererProfileConfig::LldvMetadata variants[] = {
                RendererProfileConfig::DefaultLldvMetadata(false),
                RendererProfileConfig::DefaultLldvMetadata(true),
                { 600.0, 200.0, 0.0, 1500.0 },
            };
            const HDRData primaries = Bt2020Master(0.0, 0.0, 0.0, 0.0);
            const HDRData* existings[] = { nullptr, &primaries };
            for (const auto& lldv : variants)
                for (const HDRData* existing : existings)
                {
                    LldvStaticValues values;
                    values.maxCll = lldv.maxCll;
                    values.maxFall = lldv.maxFall;
                    values.masteringMinLuminance = lldv.masteringMinLuminance;
                    values.masteringMaxLuminance = lldv.masteringMaxLuminance;
                    const SceneDescriptor d = SceneDescriptorParsers::FromLldvStatic(values, existing);
                    Assert::IsTrue(d.source == SceneMetadataSource::LldvStatic);
                    // What the GUI writes over the capture HDRData today.
                    HDRData substituted;
                    if (existing) substituted = *existing;
                    substituted.maxCll = lldv.maxCll;
                    substituted.maxFall = lldv.maxFall;
                    substituted.masteringDisplayMinLuminance = lldv.masteringMinLuminance;
                    substituted.masteringDisplayMaxLuminance = lldv.masteringMaxLuminance;
                    const pl_hdr_metadata reference = ReferenceStaticHdr(&substituted);
                    const pl_hdr_metadata mapped = MappingPolicies::ToLibplaceboHdrMetadata(
                        d, display, MappingPolicies::Default());
                    Assert::IsTrue(MappingPolicies::SameHdrMetadataBits(mapped, reference),
                        Describe(mapped, reference).c_str());
                    Assert::AreEqual(static_cast<float>(lldv.masteringMaxLuminance), d.mastering.peak.nits);
                }
        }

        TEST_METHOD(MeasuredPlaceholderDoesNotEnterLibplacebo)
        {
            const HDRData hdr = Bt2020Master(1000.0, 0.0001, 1000.0, 400.0);
            const SceneDescriptor fallback = SceneDescriptorParsers::FromStaticHdr(&hdr);
            MeasuredSceneStats stats;
            stats.valid = true;
            stats.maxNits = 650.0f;
            stats.avgNits = 80.0f;
            stats.hasMin = true;
            stats.minNits = 0.01f;
            stats.frameCount = 12;
            const SceneDescriptor d = SceneDescriptorParsers::FromMeasured(stats, fallback);
            Assert::IsTrue(d.source == SceneMetadataSource::Measured);
            Assert::IsTrue(d.HasSceneStats() && d.sceneMin.present);
            Assert::AreEqual(650.0f, d.sceneMax.nits);
            Assert::IsFalse(d.sceneMax.fromPq);
            MappingPolicy measured = MappingPolicies::Default();
            measured.useMeasuredStats = true;
            const pl_hdr_metadata mapped = MappingPolicies::ToLibplaceboHdrMetadata(d, Fl35Profile(), measured);
            Assert::IsTrue(MappingPolicies::SameHdrMetadataBits(mapped, ReferenceStaticHdr(&hdr)));
            Assert::AreEqual(0.0f, mapped.max_pq_y);
        }

        TEST_METHOD(ReservedPolicyOptionsKeepTodaysParameters)
        {
            const HDRData hdr = Bt2020Master(4000.0, 0.005, 0.0, 0.0);
            const SceneDescriptor d = SceneDescriptorParsers::FromDolbyVisionTunnel(
                L1(62, 3696, true, 40, 2771, 1229), SceneDescriptorParsers::FromStaticHdr(&hdr));
            const pl_hdr_metadata base = MappingPolicies::ToLibplaceboHdrMetadata(
                d, Fl35Profile(), MappingPolicies::Default());
            MappingPolicy reserved = MappingPolicies::Default();
            reserved.curve = CurveImplementation::VpOwned;
            reserved.useSceneMin = true;
            Assert::IsTrue(MappingPolicies::SameHdrMetadataBits(base,
                MappingPolicies::ToLibplaceboHdrMetadata(d, Fl35Profile(), reserved)));
            Assert::AreEqual(2771.0f / 4095.0f, base.max_pq_y);
            Assert::AreEqual(1229.0f / 4095.0f, base.avg_pq_y);
        }

        TEST_METHOD(BitComparisonSeesEveryField)
        {
            pl_hdr_metadata a{};
            pl_hdr_metadata b{};
            Assert::IsTrue(MappingPolicies::SameHdrMetadataBits(a, b));
            b.avg_pq_y = 1e-30f;
            Assert::IsFalse(MappingPolicies::SameHdrMetadataBits(a, b));
            b = a; b.prim.white.y = 0.329f;
            Assert::IsFalse(MappingPolicies::SameHdrMetadataBits(a, b));
            b = a; b.ootf.anchors[14] = 0.5f;
            Assert::IsFalse(MappingPolicies::SameHdrMetadataBits(a, b));
            b = a; b.min_luma = -0.0f;  // bit-for-bit, not ==
            Assert::IsFalse(MappingPolicies::SameHdrMetadataBits(a, b));
        }

        TEST_METHOD(DisplayProfileFromFl35LikeSettings)
        {
            const DisplayProfile fl35 = Fl35Profile();
            Assert::IsTrue(fl35.fromRendererSettings);
            Assert::AreEqual(120.0f, fl35.peakNits);
            Assert::AreEqual(0.005f, fl35.blackNits);
            Assert::IsTrue(fl35.eotf == DisplayEotf::Bt1886);
            Assert::IsTrue(fl35.gamut == DisplayGamut::Bt709);
            Assert::AreEqual(8, fl35.linkBitDepth);
            Assert::IsFalse(fl35.doesOwnDynamicMapping);
            Assert::AreEqual(10, DisplayProfiles::ParseLinkBitDepth("10"));
            Assert::AreEqual(0, DisplayProfiles::ParseLinkBitDepth("auto"));
            Assert::AreEqual(0, DisplayProfiles::ParseLinkBitDepth(""));
            Assert::AreEqual(std::string("bt1886"), std::string(ToString(fl35.eotf)));
            Assert::AreEqual(std::string("p3"), std::string(ToString(DisplayGamut::DisplayP3)));
        }

        TEST_METHOD(SourceTagNames)
        {
            Assert::AreEqual(std::string("dv_tunnel_l1"), std::string(ToString(SceneMetadataSource::DvTunnelL1)));
            Assert::AreEqual(std::string("hdr10_static"), std::string(ToString(SceneMetadataSource::Hdr10Static)));
            Assert::AreEqual(std::string("lldv_static"), std::string(ToString(SceneMetadataSource::LldvStatic)));
            Assert::AreEqual(std::string("measured"), std::string(ToString(SceneMetadataSource::Measured)));
            Assert::AreEqual(std::string("none"), std::string(ToString(SceneMetadataSource::None)));
        }
    };
}
