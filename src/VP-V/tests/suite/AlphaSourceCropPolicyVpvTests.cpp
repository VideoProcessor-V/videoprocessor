// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// VP-V tests split out of Bill's AlphaSourceCropPolicyTests.cpp by tools/m2/move_tests.py.
#include "pch.h"
#include "CppUnitTest.h"

#include <microsoft_directshow/MadVRShaderRuntimeState.h>
#include <ActivePictureDecisionTimeline.h>
#include <SceneDetector.h>
#include <CropDiagnosticThrottle.h>
#include <vector>
#include <limits>
#include <vprenderer/AlphaSourceCropPolicy.h>
#include <vprenderer/AnamorphicPresentation.h>
#include <vprenderer/EdgeBlanking.h>
#include <NlsGeometryPolicy.h>
#include <vprenderer/BufferedPictureExpansion.h>
#include <vprenderer/SubtitleAspectRetention.h>
#include <SubtitlePreviewMode.h>


using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace AlphaSourceCrop;

namespace Tests
{

	TEST_CLASS(AlphaSourceCropPolicyTestsVpv)
	{
	public:




















        TEST_METHOD(EdgeBlankingFitsPictureInsideLitArea)
        {
            // FL35 with ISCO III: 2560x1600 panel, left 10 and right 15 unusable.
            const PresentationRect panel = {0,0,2560,1600};
            const auto lit = EdgeBlanking::Apply(panel, {10, 0, 15, 0});
            Assert::IsTrue(lit.applied && !lit.rejected);
            Assert::AreEqual(10.0, lit.rect.left, 1e-8);
            Assert::AreEqual(2545.0, lit.rect.right, 1e-8);
            Assert::AreEqual(0.0, lit.rect.top, 1e-8);
            Assert::AreEqual(1600.0, lit.rect.bottom, 1e-8);
            for (double content : {16.0/9.0, 1.85, 2.39})
            {
                const auto screen = AnamorphicPresentation::FitScreen(false, 0.0, 4.0/3.0,
                    lit.rect, VerticalPictureAlignment::CENTER, 0);
                const auto fit = AnamorphicPresentation::FitPicture(content, 4.0/3.0, screen,
                    VerticalPictureAlignment::CENTER);
                Assert::IsTrue(fit.valid);
                Assert::IsTrue(fit.picture.left >= 10.0 - 1e-6 &&
                    fit.picture.right <= 2545.0 + 1e-6);
                Assert::AreEqual(content, 4.0/3.0*(fit.picture.right-fit.picture.left)/
                    (fit.picture.bottom-fit.picture.top), 1e-8);
            }
            Assert::AreEqual(2535.0/1600.0,
                EdgeBlanking::BlankedAspect(2560, 1600, {10, 0, 15, 0}), 1e-12);
        }

        TEST_METHOD(EdgeBlankingZeroAndExcessiveInsetsLeaveOutputUnchanged)
        {
            const PresentationRect panel = {0,0,2560,1600};
            for (const EdgeBlanking::Insets insets : {
                EdgeBlanking::Insets{}, EdgeBlanking::Insets{700, 0, 700, 0},
                EdgeBlanking::Insets{0, 801, 0, 0}, EdgeBlanking::Insets{-5, 0, 0, 0} })
            {
                const auto result = EdgeBlanking::Apply(panel, insets);
                Assert::IsFalse(result.applied);
                Assert::AreEqual(0.0, result.rect.left, 1e-8);
                Assert::AreEqual(2560.0, result.rect.right, 1e-8);
                Assert::AreEqual(0.0, result.rect.top, 1e-8);
                Assert::AreEqual(1600.0, result.rect.bottom, 1e-8);
            }
            Assert::IsTrue(EdgeBlanking::Apply(panel, {700, 0, 700, 0}).rejected);
            Assert::IsFalse(EdgeBlanking::Apply(panel, {}).rejected);
            Assert::AreEqual(1.6, EdgeBlanking::BlankedAspect(2560, 1600, {}), 1e-12);
            Assert::IsTrue(EdgeBlanking::Apply(panel, {640, 400, 640, 400}).applied);
        }
	};


}
