// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// The silent-fallbacks batch (module findings 2026-10-24; ADR-0001): every builder in
// src/VP-V/core/FallbackCatalog.h engages exactly when the downgrade is in force, releases
// otherwise, keeps its stable id, and Sync() changes the heading only on a real change.

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/ProducerRecovery.h>
#include <VP-V/modules/control/diagnostics/DiagnosticOverrides.h>

#include <cmath>
#include <string>
#include <vector>

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

		TEST_METHOD(ExtraRebaseAndDetailFailuresAreVisible)
		{
			const Decision extra = RebaseExtraNative("no format");
			Assert::IsTrue(extra.engaged);
			Assert::AreEqual(std::string("re-base"), extra.id, L"shares RebasePaused's id");
			Assert::AreEqual(std::string("extra picture: native frame (no format)"), extra.reason);
			Assert::AreEqual(std::string("extra picture: native frame"), RebaseExtraNative("").reason);
			FallbackStatus::Registry registry;
			Assert::IsTrue(Sync(registry, extra));
			Assert::IsTrue(Sync(registry, RebasePaused(true, 5.0, "no format")), L"the pause takes the entry over");
			Assert::IsTrue(Sync(registry, RebasePaused(false, 5.0, "")));
			Assert::IsFalse(registry.Engaged("re-base"));

			const Decision pulse = DetailPulseFailed(true, "gpu");
			Assert::IsTrue(pulse.engaged);
			Assert::AreEqual(std::string("detail pulse"), pulse.id);
			Assert::IsTrue(Has(pulse, "(gpu)"));
			Assert::IsFalse(DetailPulseFailed(false, "gpu").engaged);
			Assert::AreEqual(std::string("detail pulse"), DetailPulseFailed(false, "").id);
			const Decision restore = TextureRestoreFailed(true, "");
			Assert::IsTrue(restore.engaged);
			Assert::AreEqual(std::string("texture restore"), restore.id);
			Assert::IsFalse(TextureRestoreFailed(false, "x").engaged);
			Assert::AreEqual(std::string("texture restore"), TextureRestoreFailed(false, "x").id);
		}

		TEST_METHOD(CustomHooksNotLoadedListsTheFailedHooks)
		{
			const Decision none = CustomHooksNotLoaded({});
			Assert::IsFalse(none.engaged);
			Assert::AreEqual(std::string("custom hooks"), none.id);
			const Decision two = CustomHooksNotLoaded({ "a.glsl", "b.glsl" });
			Assert::IsTrue(two.engaged);
			Assert::AreEqual(std::string("custom hooks"), two.id);
			Assert::AreEqual(std::string("not loaded: a.glsl, b.glsl (the picture renders without them)"), two.reason);
			const Decision many = CustomHooksNotLoaded({ "a", "b", "c", "d", "e" });
			Assert::AreEqual(std::string("not loaded: a, b, c +2 more (the picture renders without them)"), many.reason);
			Assert::AreEqual(std::string("not loaded: a, b, c (the picture renders without them)"),
				CustomHooksNotLoaded({ "a", "b", "c" }).reason);
		}

		// Bug hunt 35 #1: the vector producer restarts with a back-off instead of stopping for the
		// session, and the stop is visible to every vector consumer while it waits.
		TEST_METHOD(VectorProducerRestartBackOffAndEntry)
		{
			using MotionAhead::ProducerRecovery;
			const unsigned long long first = ProducerRecovery::kFirstDelayMs, later = ProducerRecovery::kLaterDelayMs,
				healthy = ProducerRecovery::kHealthyMs;
			ProducerRecovery r;
			Assert::IsFalse(r.Recovering());
			Assert::AreEqual(first, static_cast<unsigned long long>(r.OnStopped(1000)));
			Assert::IsTrue(r.Recovering());
			Assert::AreEqual(first - 1000, static_cast<unsigned long long>(r.RemainingMs(2000)));
			r.OnRestarted(6000);
			Assert::IsFalse(r.Recovering());
			Assert::AreEqual(0ull, static_cast<unsigned long long>(r.RemainingMs(6000)));
			// Failing again soon after the restart: the long wait, and it stays long.
			Assert::AreEqual(later, static_cast<unsigned long long>(r.OnStopped(7000)));
			r.OnRestarted(67000);
			Assert::AreEqual(later, static_cast<unsigned long long>(r.OnStopped(67000 + healthy - 1)));
			// A restart that ran healthy for the healthy interval forgives: the short wait again.
			const uint64_t t = 200000;
			r.OnRestarted(t);
			Assert::AreEqual(first, static_cast<unsigned long long>(r.OnStopped(t + healthy)));
			Assert::AreEqual(4u, r.Stops());
			Assert::AreEqual(3u, r.Restarts());
			// A stop that never restarted (no device) keeps growing the wait.
			ProducerRecovery n;
			n.OnStopped(0);
			Assert::AreEqual(later, static_cast<unsigned long long>(n.OnStopped(first)));

			const Decision off = VectorProducer(false, true, 1, "x");
			Assert::IsFalse(off.engaged);
			Assert::AreEqual(std::string("vector producer"), off.id);
			Assert::IsFalse(VectorProducer(true, false, 3, "x").engaged);   // running again: released
			const Decision on = VectorProducer(true, true, 2, "cannot create its textures");
			Assert::IsTrue(on.engaged);
			Assert::AreEqual(std::string("vector producer"), on.id);
			Assert::IsTrue(Has(on, "cannot create its textures"));
			Assert::IsTrue(Has(on, "restarting with back-off (stop 2)"));
			FallbackStatus::Registry registry;
			Assert::IsTrue(Sync(registry, on));
			Assert::IsFalse(Sync(registry, on));   // the same stop: no heading churn
			Assert::IsTrue(Sync(registry, VectorProducer(true, false, 2)));
			Assert::IsFalse(registry.Engaged("vector producer"));
		}

		// Bug hunt 31 H3: a look-ahead worker exception is visible at once and released after a run of
		// good batches (each batch retries on its own).
		TEST_METHOD(LookAheadWorkerExceptionsAreVisibleThenRecover)
		{
			LookAheadFailures motion = MotionAheadWorker();
			Assert::IsFalse(motion.Note(false).engaged);
			const Decision failed = motion.Note(true, "bad allocation");
			Assert::IsTrue(failed.engaged);
			Assert::AreEqual(std::string("motion look-ahead"), failed.id);
			Assert::IsTrue(Has(failed, "worker exception: bad allocation"));
			for (int i = 0; i < 7; ++i)
				Assert::IsTrue(motion.Note(false).engaged, L"held through seven good batches");
			Assert::IsFalse(motion.Note(false).engaged, L"released after eight");
			Assert::IsTrue(motion.Note(true).engaged);
			Assert::IsTrue(motion.Note(false).engaged, L"a new failure restarts the count");
			LookAheadFailures source = SourceResolutionWorker();
			const Decision s = source.Note(true, "x");
			Assert::AreEqual(std::string("source resolution look-ahead"), s.id);
			Assert::IsTrue(Has(s, "resolution judged on arrival only"));
			Assert::IsFalse(source.Note(false, std::string(), 1).engaged);
		}

		// Bug hunt 39 #6/#7: test and debug switches are visible outside a test harness; numbers
		// from the environment are parsed strictly.
		TEST_METHOD(DiagnosticOverridesAreVisibleAndNumbersStrict)
		{
			using namespace VpDiagnostics;
			const wchar_t block[] = L"=C:=C:\\x\0PATH=C:\\Windows\0vp_test_adapter=Intel\0VP_DEBUG_PHASE_AB=1\0"
				L"VP_CROP_TRACE_FRAMES=10\0VP_RAW_CAPTURE_PATH=D:\\raw\0VP_TEST_EMPTY=\0VP_DISABLE_AVX2=1\0"
				L"VP_RENDERER_FILTER_LOG=x\0VP_TEST_ADAPTER=dup\0\0";
			const std::vector<std::string> names = ScanEnvironmentBlock(block);
			const std::vector<std::string> want = { "VP_DEBUG_PHASE_AB", "VP_DISABLE_AVX2", "VP_RAW_CAPTURE_PATH", "VP_TEST_ADAPTER" };
			Assert::IsTrue(names == want, L"overrides only: no trace/log switches, no empty values, unique, sorted");
			Assert::IsTrue(ScanEnvironmentBlock(nullptr).empty());
			Assert::IsTrue(IsTestHarnessImage("testhost.exe") && IsTestHarnessImage("vstest.console.exe") &&
				IsTestHarnessImage("TE.ProcessHost.exe"));
			Assert::IsFalse(IsTestHarnessImage("VideoProcessor.exe"));
			Assert::IsTrue(OverridesToShow(names, true).empty(), L"inside the harness: nothing shown");
			Assert::AreEqual(std::string("diagnostic override active: VP_DEBUG_PHASE_AB, VP_DISABLE_AVX2, VP_RAW_CAPTURE_PATH, VP_TEST_ADAPTER"),
				StatusLine(OverridesToShow(names, false)));
			Assert::AreEqual(std::string(), StatusLine({}));
			const Decision d = DiagnosticOverrides(names);
			Assert::IsTrue(d.engaged);
			Assert::AreEqual(std::string("diagnostic override"), d.id);
			Assert::IsTrue(Has(d, "active: VP_DEBUG_PHASE_AB, "));
			Assert::IsFalse(DiagnosticOverrides({}).engaged);
			// This process is a test host: nothing to show whatever is set.
			Assert::IsTrue(IsTestHarnessProcess());
			Assert::IsTrue(OverridesToShow().empty());

			double v = 0.0;
			Assert::IsTrue(ParseStrictNumber(" 12.5 ", v));
			Assert::AreEqual(12.5, v);
			Assert::IsTrue(ParseStrictNumber("-1", v) && v == -1.0);
			Assert::IsTrue(ParseStrictNumber("1e3", v) && v == 1000.0);
			for (const char* bad : { "", " ", "nan", "NaN", "inf", "-inf", "infinity", "12abc", "0x10", "1e999", "1.2.3x", "--" })
				Assert::IsFalse(ParseStrictNumber(bad, v), L"rejected");
			Assert::AreEqual(7.0, StrictNumber("nan", 7.0, 0.0, 10.0), L"the fallback");
			Assert::AreEqual(10.0, StrictNumber("1e30", 7.0, 0.0, 10.0), L"clamped, never an undefined cast");
			Assert::AreEqual(0.0, StrictNumber("-5", 7.0, 0.0, 10.0));
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

		TEST_METHOD(EarlyUploadRenderUploadsAreVisibleAboveTheShare)
		{
			FallbackCatalog::EarlyUploadRenderUploads f;
			// Window 1 (0..2000 ms): 100 payloads, 1 late: at or below 5 %, and below the 2-frame floor.
			for (int i = 0; i < 100; ++i)
				Assert::IsFalse(f.Note(static_cast<unsigned long long>(i) * 20 + (i == 99 ? 100 : 0), i == 10, false, false).engaged);
			// Window 2: 40 payloads, 3 late + 2 no slot + 1 waited: 5 of 40 > 5 %: engaged at the window's end.
			FallbackCatalog::Decision d;
			for (int i = 0; i < 40; ++i)
				d = f.Note(2100 + static_cast<unsigned long long>(i) * 50 + (i == 39 ? 100 : 0), i < 3, i == 5 || i == 6, i == 9);
			Assert::IsTrue(d.engaged);
			Assert::AreEqual(std::string("early upload frames"), d.id);
			Assert::IsTrue(d.reason.find("5 of 40 frames took the render upload (3 before the early upload started, 2 no slot), 1 waited") == 0,
				std::wstring(d.reason.begin(), d.reason.end()).c_str());
			// Still engaged between windows; released after a clean window.
			Assert::IsTrue(f.Note(4300, false, false, false).engaged);
			for (int i = 0; i < 50; ++i)
				d = f.Note(4400 + static_cast<unsigned long long>(i) * 50, false, false, false);
			Assert::IsFalse(d.engaged);
			// Engaged again, then no eligible payloads for two windows: released by Current.
			for (int i = 0; i < 10; ++i)
				d = f.Note(7000 + static_cast<unsigned long long>(i) * 250, true, false, false);
			Assert::IsTrue(d.engaged);
			Assert::IsTrue(f.Current(10000).engaged);
			Assert::IsFalse(f.Current(14000).engaged);
		}
	};
}
