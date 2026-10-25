#include "pch.h"
#include "CppUnitTest.h"

#include <RendererProfileConfig.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>
#include <VideoFrameEncoding.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Hunt 2 fix group 5 (motion; motion_history_fixes): the CPU-testable rules of the
// matcher's history chain (04 M1), the optical-flow reach (04 M3), the camera window
// (04 L5) and the DV pairing (02-dv L1 / L2).
namespace Hunt2MotionTest
{
	namespace
	{
		struct Payload
		{
			DolbyVisionTunnelFrameMetadata dolby;
			VideoFrameEncoding captureEncoding = VideoFrameEncoding::R10b;
		};
		Payload DvPicture(float matrixValue)
		{
			Payload p;
			p.dolby.valid = true;
			p.dolby.rgbBitAlignment = 1;
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
				{
					p.dolby.nonlinear[r][c] = r == c ? matrixValue : 0.0f;
					p.dolby.linear[r][c] = r == c ? 1.0f : 0.0f;
				}
				p.dolby.nonlinearOffset[r] = 0.5f;
			}
			return p;
		}
	}

	TEST_CLASS(Hunt2MotionHistoryTests)
	{
	public:
		// 04 M1: the history is the last estimated pair's (ending at keptSequence); a pair
		// whose A is another frame starts it over. Unknown sequences behave as before.
		TEST_METHOD(MatcherHistoryChainsOnlyFromTheLastEstimatedPair)
		{
			using DejudderPolicy::MatcherHistoryChained;
			Assert::IsTrue(MatcherHistoryChained(11, 11), L"(10, 11) then (11, 12): chained");
			Assert::IsFalse(MatcherHistoryChained(12, 11), L"(10, 11) then (12, 13): (11, 12) never estimated");
			Assert::IsFalse(MatcherHistoryChained(12, 0), L"after ForgetHistory: no history to chain");
			Assert::IsFalse(MatcherHistoryChained(10, 11), L"a step back is not chained");
			Assert::IsTrue(MatcherHistoryChained(0, 11), L"sequence unknown (direct use): as before");
			Assert::IsTrue(MatcherHistoryChained(0, 0));
		}

		// 04 M3: the engine's reach follows the range the search really uses (scaled to the
		// working height, clamped to 16), not the raw 1080-line setting.
		TEST_METHOD(OpticalFlowReachFollowsTheScaledSearchRange)
		{
			using DejudderPolicy::OpticalFlowReach;
			using DejudderPolicy::ScaledSearchRange;
			// The renderer's matcher (referenceHeight > 0) at its working heights.
			Assert::AreEqual(3, ScaledSearchRange(3, 2160.0f, 1080));
			Assert::AreEqual(56.0f, OpticalFlowReach(ScaledSearchRange(3, 2160.0f, 1080)), L"1080 lines: as before");
			Assert::AreEqual(2, ScaledSearchRange(3, 2160.0f, 720));
			Assert::AreEqual(40.0f, OpticalFlowReach(ScaledSearchRange(3, 2160.0f, 720)), L"720p: the search's 2, not 3");
			Assert::AreEqual(2, ScaledSearchRange(3, 2160.0f, 576), L"PAL: lround(1.6)");
			Assert::AreEqual(4, ScaledSearchRange(3, 2160.0f, 1440));
			Assert::AreEqual(72.0f, OpticalFlowReach(ScaledSearchRange(3, 2160.0f, 1440)), L"1440p: longer than the raw 56");
			// The lowRes inner (referenceHeight < 0): its own working height.
			Assert::AreEqual(3, ScaledSearchRange(3, -1.0f, 1080));
			Assert::AreEqual(2, ScaledSearchRange(3, -1.0f, 800));
			// Direct use (referenceHeight 0): as given; the clamp.
			Assert::AreEqual(3, ScaledSearchRange(3, 0.0f, 720));
			Assert::AreEqual(16, ScaledSearchRange(40, 2160.0f, 1080));
			Assert::AreEqual(0, ScaledSearchRange(-2, 0.0f, 1080));
			Assert::AreEqual(24.0f, OpticalFlowReach(0), L"at least one coarse step");
		}

		// 04 L5: one global step per halving, at least 6 px; 1080p and 4K as before.
		TEST_METHOD(CameraWindowCoversOneStepForEveryHalving)
		{
			using DejudderPolicy::CameraWindowPixels;
			Assert::AreEqual(6.0f, CameraWindowPixels(0), L"full size: 6 px (as before)");
			Assert::AreEqual(8.0f, CameraWindowPixels(1), L"halved once (4K): 8 px (as before)");
			Assert::AreEqual(16.0f, CameraWindowPixels(2), L"halved twice (8K): its 16 px step (was 8)");
			Assert::AreEqual(6.0f, CameraWindowPixels(-1));
		}

		// 04 L3: the causes the renderer now raises exist and name themselves.
		TEST_METHOD(SettingsAndSourceChangesInvalidateTheVectors)
		{
			MotionValidity::State state;
			const uint64_t before = state.Epoch();
			state.Invalidate(MotionValidity::Cause::SettingsChange);
			state.Invalidate(MotionValidity::Cause::MotionSourceChange);
			Assert::AreEqual(before + 2, state.Epoch());
			Assert::AreEqual(1ull, static_cast<unsigned long long>(state.Count(MotionValidity::Cause::SettingsChange)));
			Assert::AreEqual(1ull, static_cast<unsigned long long>(state.Count(MotionValidity::Cause::MotionSourceChange)));
			Assert::IsFalse(MotionValidity::ReleasesResources(MotionValidity::Cause::SettingsChange),
				L"a settings change forgets history, it does not release the matcher");
			Assert::AreEqual(before, state.StreamGeneration(), L"the producer's stored pairs are kept");
		}

		TEST_METHOD(MotionHistoryFixesIsAValidatedSwitch)
		{
			std::string expected;
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "motion_history_fixes", "on", expected));
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "motion_history_fixes", "off", expected));
			Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "motion_history_fixes", "auto", expected));
		}
	};

	TEST_CLASS(Hunt2DolbyVisionPairTests)
	{
	public:
		// 02-dv L2: two DV pictures pair only under byte-equal DM matrices (with the switch).
		TEST_METHOD(DvPicturesPairOnlyUnderTheSameMatrices)
		{
			const Payload a = DvPicture(1.0f), same = DvPicture(1.0f), other = DvPicture(1.25f);
			Assert::IsTrue(DejudderPolicy::DolbyVisionNextPairs(true, a, same, true));
			Assert::IsFalse(DejudderPolicy::DolbyVisionNextPairs(true, a, other, true), L"a new matrix: no pair");
			Assert::IsTrue(DejudderPolicy::DolbyVisionNextPairs(true, a, other, false), L"switch off: as before");
			Assert::IsTrue(DejudderPolicy::DolbyVisionNextPairs(false, a, other, true), L"off the tunnel: not judged");
			// SDR fallbacks carry no matrices: decided by kind alone.
			Payload s1, s2;
			s1.captureEncoding = s2.captureEncoding = VideoFrameEncoding::R210;
			s1.dolby.allowSdrFallback = s2.dolby.allowSdrFallback = true;
			s1.dolby.rgbBitAlignment = s2.dolby.rgbBitAlignment = 2;
			s2.dolby.nonlinear[0][0] = 7.0f;
			Assert::IsTrue(DejudderPolicy::DolbyVisionNextPairs(true, s1, s2, true), L"SDR fallback pair: matrices not judged");
		}

		TEST_METHOD(ProducerBreaksOnADvMatrixChange)
		{
			using DejudderPolicy::MotionAheadBreakBefore;
			using DejudderPolicy::MotionAheadFrame;
			const Payload pa = DvPicture(1.0f), pb = DvPicture(1.25f);
			MotionAheadFrame a, b;
			a.time = 10.0;
			b.time = 10.0 + 1.0 / 24.0;
			a.kind = DejudderPolicy::ClassifyDolbyVisionPayload(pa);
			b.kind = DejudderPolicy::ClassifyDolbyVisionPayload(pb);
			a.matrices = DejudderPolicy::DolbyVisionMatricesOf(pa.dolby);
			b.matrices = DejudderPolicy::DolbyVisionMatricesOf(pb.dolby);
			Assert::IsTrue(MotionAheadBreakBefore(false, false, true, 24.0, true, true, a, b, true), L"new matrix: break");
			Assert::IsFalse(MotionAheadBreakBefore(false, false, true, 24.0, true, true, a, b, false), L"switch off: no break");
			b.matrices = a.matrices;
			Assert::IsFalse(MotionAheadBreakBefore(false, false, true, 24.0, true, true, a, b, true), L"same matrix: pair");
		}

		// 02-dv L1: the producer's kind is the payload's (capture encoding), so an r210 SDR
		// fallback is a picture there as at the dequeue.
		TEST_METHOD(AheadKindFollowsThePayloadsCaptureEncoding)
		{
			Payload sdr;
			sdr.dolby.allowSdrFallback = true;
			sdr.dolby.rgbBitAlignment = 1;
			sdr.captureEncoding = VideoFrameEncoding::R210;
			Assert::IsTrue(DejudderPolicy::ClassifyDolbyVisionPayload(sdr) == DejudderPolicy::DolbyVisionFrameKind::SdrFallback);
			sdr.captureEncoding = VideoFrameEncoding::R10b;
			Assert::IsTrue(DejudderPolicy::ClassifyDolbyVisionPayload(sdr) == DejudderPolicy::DolbyVisionFrameKind::Untrusted);
		}
	};
}
