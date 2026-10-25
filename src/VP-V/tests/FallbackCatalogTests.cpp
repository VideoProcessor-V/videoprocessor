// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// The silent-fallbacks batch (module findings 2026-10-24; ADR-0001): every builder in
// src/VP-V/core/FallbackCatalog.h engages exactly when the downgrade is in force, releases
// otherwise, keeps its stable id, and Sync() changes the heading only on a real change.

#include <VP-V/core/FallbackCatalog.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace FallbackCatalog;

namespace FallbackCatalogTest
{
	namespace
	{
		bool Has(const Decision& d, const char* text) { return d.reason.find(text) != std::string::npos; }
	}

	TEST_CLASS(FallbackCatalogTests)
	{
	public:
		TEST_METHOD(SyncEngagesReleasesAndReportsChangesOnce)
		{
			FallbackStatus::Registry registry;
			const Decision on = Dither(true, DitherDrop::ShaderNotLoaded);
			Assert::IsTrue(Sync(registry, on));
			Assert::IsFalse(Sync(registry, on), L"the same decision again changes nothing");
			Assert::IsTrue(registry.Engaged("dither"));
			Assert::IsTrue(Sync(registry, Dither(true, DitherDrop::None)));
			Assert::IsFalse(registry.Engaged("dither"));
			Assert::IsFalse(Sync(registry, Dither(true, DitherDrop::None)), L"releasing twice changes nothing");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), registry.Heading());
		}

		TEST_METHOD(DitherOnlyWhenTheSigmaDeltaWasAskedFor)
		{
			Assert::IsFalse(Dither(false, DitherDrop::UnsupportedDomain).engaged, L"not requested: libplacebo's dither is the setting");
			Assert::IsFalse(Dither(true, DitherDrop::None).engaged);
			const Decision domain = Dither(true, DitherDrop::UnsupportedDomain, "limited range, 16-bit");
			Assert::IsTrue(domain.engaged);
			Assert::AreEqual(std::string("dither"), domain.id);
			Assert::IsTrue(Has(domain, "8- or 10-bit") && Has(domain, "16-bit"));
			Assert::IsTrue(Has(Dither(true, DitherDrop::DisabledByLibplacebo), "disabled by libplacebo"));
		}

		TEST_METHOD(BtbWtwNamesWhatClips)
		{
			Assert::IsFalse(BtbWtw(false, BtbWtwClip::HookFailed).engaged, L"preserve off: nothing promised");
			Assert::IsFalse(BtbWtw(true, BtbWtwClip::None).engaged);
			Assert::IsTrue(Has(BtbWtw(true, BtbWtwClip::HookFailed), "hook"));
			Assert::IsTrue(Has(BtbWtw(true, BtbWtwClip::FrameMixing), "frame mix"));
			const Decision mix = BtbWtw(true, BtbWtwClip::MixClipsBtb);
			Assert::AreEqual(std::string("BTB/WTW"), mix.id);
			Assert::IsTrue(Has(mix, "BTB clipped") && Has(mix, "smooth motion"));
		}

		TEST_METHOD(RebaseFactorsAreOneWhenTheRebaseDoesNotRun)
		{
			const RebaseFactors off = EffectiveRebaseFactors(false, 2.0, 2.0);
			Assert::AreEqual(1.0, off.x);
			Assert::AreEqual(1.0, off.y);
			const RebaseFactors on = EffectiveRebaseFactors(true, 2.0, 1.5);
			Assert::AreEqual(2.0, on.x);
			Assert::AreEqual(1.5, on.y);
			const RebaseFactors bad = EffectiveRebaseFactors(true, 0.0, -1.0);
			Assert::AreEqual(1.0, bad.x);
			Assert::AreEqual(1.0, bad.y);
			Assert::IsTrue(RebasePaused(true, 20.0, "no format").engaged);
			Assert::IsTrue(Has(RebasePaused(true, 20.0, ""), "20 s"));
			Assert::IsFalse(RebasePaused(false, 20.0, "x").engaged);
		}

		TEST_METHOD(DebandSubstitutesOnlyForAFailedVpDebander)
		{
			// VP's debander runs: libplacebo yields as configured, nothing engaged.
			DebandPlan plan = PlanDeband(true, true, false, false);
			Assert::IsTrue(plan.libplaceboYields);
			Assert::IsFalse(plan.fallback.engaged);
			// VP's failed: libplacebo debands, visibly.
			plan = PlanDeband(true, true, true, false, "texture");
			Assert::IsFalse(plan.libplaceboYields);
			Assert::IsTrue(plan.fallback.engaged);
			Assert::AreEqual(std::string("deband"), plan.fallback.id);
			Assert::IsTrue(Has(plan.fallback, "failed") && Has(plan.fallback, "texture"));
			// The slot switched off is a choice: no substitute, no entry (even after a failure).
			plan = PlanDeband(true, false, false, false);
			Assert::IsTrue(plan.libplaceboYields);
			Assert::IsFalse(plan.fallback.engaged);
			plan = PlanDeband(true, false, true, false);
			Assert::IsTrue(plan.libplaceboYields);
			Assert::IsFalse(plan.fallback.engaged);
			// Dolby Vision tunnel pictures: never substituted.
			plan = PlanDeband(true, true, true, true);
			Assert::IsTrue(plan.libplaceboYields);
			Assert::IsFalse(plan.fallback.engaged);
			// No yield by the settings: libplacebo's deband is the user's own setting, nothing engaged.
			plan = PlanDeband(false, true, true, false);
			Assert::IsFalse(plan.libplaceboYields);
			Assert::IsFalse(plan.fallback.engaged);
		}

		TEST_METHOD(MotionPresentationAndRefresh)
		{
			Assert::IsFalse(Motion(true, false, false).engaged);
			Assert::IsFalse(Motion(false, true, true).engaged, L"optical flow not requested");
			Assert::IsTrue(Has(Motion(true, true, false, "NvOFExecute"), "matcher only"));
			Assert::IsTrue(Has(Motion(true, false, true), "look-ahead"));
			Assert::IsFalse(Presentation(false, false).engaged);
			Assert::IsTrue(Has(Presentation(true, false), "open loop"));
			Assert::IsTrue(Has(Presentation(false, true), "de-judder held"));
			const Decision kept = RefreshRate(true, 23.976, 60.0, "no matching mode");
			Assert::IsTrue(kept.engaged);
			Assert::IsTrue(Has(kept, "60.000") && Has(kept, "23.976") && Has(kept, "no matching mode"));
			Assert::IsFalse(RefreshRate(false, 23.976, 60.0, "x").engaged);
			Assert::IsFalse(RefreshRate(true, 0.0, 60.0, "x").engaged, L"unknown input rate: nothing to claim");
		}

		TEST_METHOD(HandedOffSitesHaveStableIds)
		{
			Assert::AreEqual(std::string("calibration LUT"), CalibrationLutDropped(true, "e").id);
			Assert::AreEqual(std::string("calibration patch"), CalibrationPatchFullField(true, "").id);
			Assert::AreEqual(std::string("output range"), OutputRangeComposed(true, "").id);
			Assert::AreEqual(std::string("output request"), OutputRequestRejected(true, "Limited/2.4", "").id);
			Assert::AreEqual(std::string("geometry"), GeometrySafeFit(true).id);
			Assert::AreEqual(std::string("subtitle detection"), SubtitleDetection(SubtitleLoss::CueDropped).id);
			Assert::AreEqual(std::string("SDR tone shape"), SdrToneShape(true, false, "shader did not load").id);
			Assert::AreEqual(std::string("output scaling"), OutputScaling(true, 1, false).id);
			Assert::AreEqual(std::string("early upload"), EarlyUpload(true, false, "device").id);
			Assert::IsFalse(CalibrationLutDropped(false, "e").engaged);
			Assert::IsFalse(SubtitleDetection(SubtitleLoss::None).engaged);
			Assert::IsFalse(SdrToneShape(false, false, "").engaged, L"not requested");
			Assert::IsFalse(SdrToneShape(true, true, "").engaged);
			Assert::IsFalse(OutputScaling(false, 3, true).engaged);
			Assert::IsTrue(Has(OutputScaling(true, 0, true), "SSimDownscaler"));
			Assert::IsFalse(EarlyUpload(true, true, "").engaged);
		}

		TEST_METHOD(MotionConsumerGpuFailuresAreVisibleWhileRecent)
		{
			// Group D audit D3: warp / interpolation failures are listed, timing reasons are not.
			MotionConsumerFailures f;
			Assert::IsFalse(f.Note(1000, nullptr).engaged);
			Assert::IsFalse(f.Note(1100, "too late for the refresh").engaged);
			Decision d = f.Note(1200, "interpolation failed");
			Assert::IsTrue(d.engaged);
			Assert::AreEqual(std::string("motion consumers"), d.id);
			Assert::IsTrue(Has(d, "interpolation failed"));
			Assert::IsTrue(Has(d, "source frame"));
			Assert::IsTrue(f.Note(3000, nullptr).engaged);           // still recent
			Assert::IsTrue(Has(f.Note(4000, "rendered warp failed"), "rendered warp failed"));
			Assert::IsFalse(f.Note(9001, nullptr).engaged);          // 5 s without a failure
			Assert::IsTrue(f.Note(9100, "warp failed").engaged);
			Assert::IsFalse(f.Current(14100).engaged);
		}
	};
}
