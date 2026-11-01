// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// VP-V tests split out of Bill's DolbyVisionLuminancePolicyTests.cpp by tools/m2/move_tests.py.
#include "pch.h"
#include "CppUnitTest.h"
#include <vprenderer/DolbyVisionLuminancePolicy.h>
#include <cstring>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
    TEST_CLASS(DolbyVisionLuminancePolicyTestsVpv)
    {
    public:

        // audit 2026-10-16: L1 scene minimum is carried, not written into the
        // static source range (libplacebo has no scene-minimum field).
        TEST_METHOD(SceneMinimumIsCarriedSeparatelyFromSourceRange)
        {
            pl_hdr_metadata hdr{};
            DolbyVisionTunnelFrameMetadata dm;
            dm.valid = true;
            dm.sourceMinPq = 62;
            dm.sourceMaxPq = 3696;
            dm.hasSceneMetadata = true;
            dm.sceneMinPq = 1024;
            dm.sceneMaxPq = 3072;
            dm.sceneAvgPq = 2048;
            DolbyVisionLuminancePolicy::Apply(dm, hdr);
            const float sourceMin = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, 62.0f / 4095.0f);
            Assert::AreEqual(sourceMin, hdr.min_luma, 1e-6f);
            float nits = -1;
            Assert::IsTrue(DolbyVisionLuminancePolicy::SceneMinNits(dm, nits));
            Assert::AreEqual(pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, 1024.0f / 4095.0f), nits, 1e-4f);
            Assert::AreEqual(1024.0f / 4095.0f, DolbyVisionLuminancePolicy::SceneMinPq(dm));
            Assert::IsTrue(nits < pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, 2048.0f / 4095.0f));
        }

        TEST_METHOD(SceneMinimumAbsentOrBlackIsDistinguished)
        {
            DolbyVisionTunnelFrameMetadata dm;
            dm.valid = true;
            dm.sourceMaxPq = 4095;
            float nits = -1;
            Assert::IsFalse(DolbyVisionLuminancePolicy::SceneMinNits(dm, nits));
            Assert::AreEqual(0.0f, nits);
            Assert::AreEqual(-1.0f, DolbyVisionLuminancePolicy::SceneMinPq(dm));
            dm.hasSceneMetadata = true; // present black scene minimum
            Assert::IsTrue(DolbyVisionLuminancePolicy::SceneMinNits(dm, nits));
            Assert::AreEqual(0.0f, nits, 1e-6f);
            Assert::AreEqual(0.0f, DolbyVisionLuminancePolicy::SceneMinPq(dm));
            dm.sceneMinPq = 5000; // out of 12-bit range
            Assert::IsFalse(DolbyVisionLuminancePolicy::SceneMinNits(dm, nits));
            dm.sceneMinPq = 0;
            dm.valid = false; // lost metadata never publishes a minimum
            Assert::IsFalse(DolbyVisionLuminancePolicy::SceneMinNits(dm, nits));
        }
    
        TEST_METHOD(SceneMinimumReachesTheStatusButNotTheToneMapper)
        {
            // 2026-10-17: the L1 scene minimum is published with the status
            // (DV card, capture JSON); hdr is written exactly as Apply does.
            DolbyVisionTunnelFrameMetadata dm;
            dm.valid = true;
            dm.sourceMinPq = 62;
            dm.sourceMaxPq = 3696;
            dm.hasSceneMetadata = true;
            dm.sceneMinPq = 410;
            dm.sceneMaxPq = 3000;
            dm.sceneAvgPq = 1500;
            pl_hdr_metadata a{}, b{};
            const auto plain = DolbyVisionLuminancePolicy::Apply(dm, a);
            const auto withMin = DolbyVisionLuminancePolicy::ApplyWithSceneMin(dm, b);
            Assert::IsTrue(memcmp(&a, &b, sizeof(a)) == 0, L"hdr unchanged by the scene minimum");
            Assert::IsFalse(plain.sceneMinAvailable);
            Assert::IsTrue(withMin.sceneMinAvailable);
            float nits = 0;
            Assert::IsTrue(DolbyVisionLuminancePolicy::SceneMinNits(dm, nits));
            Assert::AreEqual(nits, withMin.sceneMin);
            Assert::IsTrue(withMin.sceneMin > 0.0f && withMin.sceneMin < withMin.sceneAvg);
            dm.hasSceneMetadata = false;
            pl_hdr_metadata c{};
            Assert::IsFalse(DolbyVisionLuminancePolicy::ApplyWithSceneMin(dm, c).sceneMinAvailable);
        }
    };
}
