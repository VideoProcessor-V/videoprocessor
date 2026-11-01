#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <MainConfigSchema.h>
#include <RendererProfileConfig.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>
#include <VP-V/modules/motion/vector_delivery/DejudderWarp.h>
#include <VP-V/modules/repair/deband/VpDebander.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Integration merge 2026-10-17 (v1.4.02): the interaction-audit fixes that are
// CPU-testable - the optical-flow producer's break rule, the faithful grain
// object defaults and the sample configuration through runtime validation.
namespace Integration20261017Test
{
	namespace
	{
		std::string RepositoryFile(const char* relative)
		{
			std::string path = __FILE__;
			std::replace(path.begin(), path.end(), '/', '\\');
			const std::string marker = "\\src\\VideoProcessor-Test\\";
			const size_t at = path.rfind(marker);
			Assert::IsTrue(at != std::string::npos);
			return path.substr(0, at) + "\\" + relative;
		}
	}

	TEST_CLASS(MotionAheadBreakTests)
	{
	public:
		// The producer gets a break before a frame (no pair with the previous
		// submission) on a gap or film update, when the pair's times are not
		// adjacent (timing_continuity_fixes) and on an untrusted DV pair.
		TEST_METHOD(ProducerBreaksOnANonAdjacentPairAndAnUntrustedDvPair)
		{
			using DejudderPolicy::DolbyVisionFrameKind;
			using DejudderPolicy::MotionAheadBreakBefore;
			using DejudderPolicy::MotionAheadFrame;
			for (double rate : { 23.976, 25.0, 29.97, 50.0, 59.94 })
			{
				const double period = 1.0 / rate;
				MotionAheadFrame a, b;
				a.time = 100.0;
				a.kind = b.kind = DolbyVisionFrameKind::Picture;
				b.time = a.time + period;
				Assert::IsFalse(MotionAheadBreakBefore(false, false, true, rate, false, true, a, b), L"adjacent: no break");
				Assert::IsTrue(MotionAheadBreakBefore(true, false, true, rate, false, true, a, b), L"a gap breaks");
				Assert::IsTrue(MotionAheadBreakBefore(false, true, true, rate, false, true, a, b), L"a film update breaks");
				b.time = a.time + 2.0 * period;
				Assert::IsTrue(MotionAheadBreakBefore(false, false, true, rate, false, true, a, b),
					L"two periods apart: not adjacent, a break");
				Assert::IsFalse(MotionAheadBreakBefore(false, false, false, rate, false, true, a, b),
					L"timing_continuity_fixes off: times not judged");
				b.time = a.time - period;
				Assert::IsTrue(MotionAheadBreakBefore(false, false, true, rate, false, true, a, b), L"backwards: a break");
				// Unknown times are not judged.
				MotionAheadFrame unknown;
				unknown.kind = DolbyVisionFrameKind::Picture;
				Assert::IsFalse(MotionAheadBreakBefore(false, false, true, rate, false, true, a, unknown));
			}
			// The DV tunnel: an untrusted B or a picture/SDR mix breaks; off the tunnel it does not.
			MotionAheadFrame dv, black, sdr;
			dv.time = 1.0;
			black.time = sdr.time = 1.0 + 1.0 / 24.0;
			dv.kind = DolbyVisionFrameKind::Picture;
			black.kind = DolbyVisionFrameKind::Untrusted;
			sdr.kind = DolbyVisionFrameKind::SdrFallback;
			MotionAheadFrame dv2 = dv;
			dv2.time = black.time;
			Assert::IsFalse(MotionAheadBreakBefore(false, false, true, 24.0, true, true, dv, dv2));
			Assert::IsTrue(MotionAheadBreakBefore(false, false, true, 24.0, true, true, dv, black), L"untrusted B breaks");
			Assert::IsTrue(MotionAheadBreakBefore(false, false, true, 24.0, true, true, dv, sdr), L"DV then SDR breaks");
			Assert::IsFalse(MotionAheadBreakBefore(false, false, true, 24.0, false, true, dv, black), L"not on the tunnel");
			// Nothing submitted before: no pair to break.
			Assert::IsFalse(MotionAheadBreakBefore(false, false, true, 24.0, true, false, dv, black));
		}
	};

	TEST_CLASS(GrainObjectDefaultTests)
	{
	public:
		// The faithful grain paths are the object defaults as well as the
		// profile defaults (the profile defaults: RendererCaptureHarnessTests).
		TEST_METHOD(FaithfulGrainPathsAreTheObjectDefaults)
		{
			Assert::IsTrue(Dejudder::Settings().sourceWarpCatmull, L"the source warp samples with Catmull-Rom");
			Assert::IsTrue(VpDebander::Params().grainGuard, L"VP's debander keeps grain");
		}
	};

	TEST_CLASS(SampleConfigurationTests)
	{
	public:
		// The shipped VideoProcessor.cfg through the runtime's validation: the
		// startup schema, the unified profile model, and the key bindings (F2/F3
		// are bound in both a Viewport and a Zoom profile: one accelerator each).
		TEST_METHOD(SampleConfigurationPassesRuntimeValidation)
		{
			const std::string path = RepositoryFile("VideoProcessor.cfg");
			ConfigFile config;
			Assert::IsTrue(config.Load(path), L"the sample loads");
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error), std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(RendererProfileConfig::ValidateOwnedSections(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			RendererProfileConfig::Model model;
			Assert::IsTrue(RendererProfileConfig::Read(config, model, error), std::wstring(error.begin(), error.end()).c_str());
			std::vector<std::string> chords;
			Assert::IsTrue(RendererProfileConfig::CollectKeyChords(model, chords, error),
				std::wstring(error.begin(), error.end()).c_str());
			// The GUI registers one accelerator per chord; each must be registrable
			// and listed once (a duplicate is what rejectInvalidBindings refuses).
			std::vector<std::string> sorted = chords;
			std::sort(sorted.begin(), sorted.end());
			Assert::IsTrue(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), L"each chord once");
			for (const std::string& chord : chords)
				Assert::IsTrue(RendererProfileConfig::IsRegistrableKeyChord(chord),
					std::wstring(chord.begin(), chord.end()).c_str());
			auto has = [&](const char* key)
			{
				std::string canonical;
				return RendererProfileConfig::CanonicalizeKeyChord(key, canonical) &&
					std::find(chords.begin(), chords.end(), canonical) != chords.end();
			};
			Assert::IsTrue(has("F2") && has("F3"), L"F2 / F3 bound");
			// Motion settings live in [vprenderer.motion.Default] (the config branch's rename).
			Assert::IsTrue(config.HasSection("vprenderer.motion.Default") || config.HasSection("vprenderer.motion.default"),
				L"the motion profile section");
		}
	};
}
