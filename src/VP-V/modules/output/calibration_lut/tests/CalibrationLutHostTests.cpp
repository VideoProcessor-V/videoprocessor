#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutHostPolicy.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// WP-12: the decisions of the calibration host adapter (CalibrationLutHostPolicy.h). The adapter
// itself (libplacebo, D3D11) is exercised by the renderer; these pin what it decides.
namespace CalibrationLutHostTests
{
	using namespace CalibrationLutHost;

	TEST_CLASS(CalibrationLutHostPolicyTests)
	{
	public:
		TEST_METHOD(VpStageNeedsTheSettingTheSlotAndNoOriginalPathway)
		{
			StageFacts f;
			f.vpStageWanted = true;
			f.slotRuns = true;
			Assert::IsTrue(UseVpStage(f, false));
			Assert::IsFalse(UseVpStage(f, true), L"a failed stage stays on libplacebo's target LUT");
			StageFacts original = f;
			original.originalPathway = true;
			Assert::IsFalse(UseVpStage(original, false), L"the Original pathway keeps libplacebo's target LUT");
			StageFacts slotOff = f;
			slotOff.slotRuns = false;
			Assert::IsFalse(UseVpStage(slotOff, false));
			StageFacts libplacebo = f;
			libplacebo.vpStageWanted = false;
			Assert::IsFalse(UseVpStage(libplacebo, false));
		}

		TEST_METHOD(VpStageStatusIsWrittenOnlyWhenItChanges)
		{
			std::string status = "Loaded";
			Assert::IsTrue(VpStageStatus(status, "cal_bt709.cube"));
			Assert::AreEqual(std::string("Active (VP stage): cal_bt709.cube"), status);
			Assert::IsFalse(VpStageStatus(status, "cal_bt709.cube"), L"unchanged: no rewrite every frame");
			Assert::IsTrue(VpStageStatus(status, "cal_p3.cube"));
			Assert::AreEqual(std::string("Active (VP stage): cal_p3.cube"), status);
		}

		// Module findings 2026-10-24 part 3: the VP stage overwrote "Active (reload rejected: ...)"
		// every frame, so a rejected reload was never shown in the VP stage.
		TEST_METHOD(VpStageKeepsAReloadRejection)
		{
			std::string status = "Active (reload rejected: invalid cube)";
			Assert::IsTrue(VpStageStatus(status, "a.cube"));
			Assert::AreEqual(std::string("Active (VP stage; reload rejected: invalid cube)"), status);
			Assert::IsFalse(VpStageStatus(status, "a.cube"), L"kept on the next frames");
			Assert::AreEqual(std::string("Active (VP stage; reload rejected: invalid cube)"), status);
			// Back on libplacebo's target LUT: Bill's text again (his branch keeps it).
			Assert::IsTrue(LibplaceboStageStatus(status));
			Assert::AreEqual(std::string("Active (reload rejected: invalid cube)"), status);
			Assert::IsFalse(LibplaceboStageStatus(status));
			// A successful (re)load writes "Loaded": the VP stage text returns.
			status = "Loaded";
			Assert::IsTrue(VpStageStatus(status, "a.cube"));
			Assert::AreEqual(std::string("Active (VP stage): a.cube"), status);
			std::string plain = "Active: a.cube (33^3)";
			Assert::IsFalse(LibplaceboStageStatus(plain));
		}

		// Module findings 2026-10-24 part 3: "attached" read only libplacebo's target.lut, which the
		// VP stage leaves empty on purpose.
		TEST_METHOD(AttachedCountsTheVpStage)
		{
			Assert::IsTrue(LutAttached(true, true, false), L"libplacebo's target LUT");
			Assert::IsTrue(LutAttached(true, false, true), L"the VP stage");
			Assert::IsFalse(LutAttached(true, false, false));
			Assert::IsFalse(LutAttached(false, true, true), L"nothing rendered");
		}

		// Module README "Cost" item 1: while patterns show, a filled cache answers the output query
		// (the format cannot change with video paused); otherwise the renderer is asked.
		TEST_METHOD(OutputQueryUsesTheCacheWhilePatternsShow)
		{
			Assert::IsTrue(WantFreshOutputFacts(false, 10));
			Assert::IsTrue(WantFreshOutputFacts(true, 0), L"nothing cached yet");
			Assert::IsFalse(WantFreshOutputFacts(true, 10));
		}

		// ADR-0001: the two calibration downgrades the adapter reports (the renderer engages them).
		TEST_METHOD(CalibrationFallbackEntriesNameWhatRunsInstead)
		{
			const FallbackCatalog::Decision dropped = FallbackCatalog::CalibrationLutDropped(true, FileName("C:/vp/a.cube"));
			Assert::IsTrue(dropped.engaged);
			Assert::AreEqual(std::string("calibration LUT"), dropped.id);
			Assert::AreEqual(std::string("dropped after a render error: a.cube; no projector LUT for this renderer"), dropped.reason);
			const FallbackCatalog::Decision patch = FallbackCatalog::CalibrationPatchFullField(true, "the surface takes no blit");
			Assert::IsTrue(patch.engaged);
			Assert::AreEqual(std::string("calibration patch"), patch.id);
			Assert::IsTrue(patch.reason.find("full field") == 0);
		}

		TEST_METHOD(FileNameTakesTheLastComponent)
		{
			Assert::AreEqual(std::string("a.cube"), FileName("C:\\vp\\luts\\a.cube"));
			Assert::AreEqual(std::string("a.cube"), FileName("luts/a.cube"));
			Assert::AreEqual(std::string("a.cube"), FileName("a.cube"));
		}

		// VP's automatic reload (today's default) is the probe once a second of the LUT file the
		// renderer loaded; byte-for-byte the renderer's old MaybeReloadDisplayLut schedule.
		TEST_METHOD(AutoReloadProbesOnceASecondAndReloadsOnAChangedFile)
		{
			ReloadPolicy policy;
			Assert::IsTrue(policy.Mode() == ReloadMode::Auto);
			policy.NoteLoaded(true, 100, 5, 10000);
			Assert::IsFalse(policy.ProbeDue(true, true, 10500), L"not before a second");
			Assert::IsTrue(policy.ProbeDue(true, true, 11000));
			Assert::IsFalse(policy.ProbeDue(true, true, 11500));
			Assert::IsTrue(policy.ProbeDue(true, true, 12000));
			Assert::IsFalse(policy.ProbeDue(false, true, 99999), L"no file selected");
			Assert::IsFalse(policy.ProbeDue(true, false, 99999), L"the LUT is disabled");
			Assert::IsFalse(policy.Changed(true, 100, 5));
			Assert::IsTrue(policy.Changed(true, 101, 5));
			Assert::IsTrue(policy.Changed(true, 100, 6));
			Assert::IsTrue(policy.Changed(false, 0, 0), L"the file disappeared");
		}

		// Bill's explicit Apply (the other choice, either/or): the file is never probed; only VP's own
		// calibration may load its finished LUT without a Config Apply, once per request, with a notice.
		TEST_METHOD(ExplicitApplyNeverProbesButTakesACalibrationReloadOnce)
		{
			ReloadPolicy policy;
			policy.SetMode(ReloadMode::ExplicitApply);
			policy.NoteLoaded(true, 100, 5, 0);
			Assert::IsFalse(policy.ProbeDue(true, true, 100000));
			Assert::IsFalse(policy.TakeCalibrationReload());
			policy.RequestCalibrationReload();
			Assert::IsTrue(policy.TakeCalibrationReload());
			Assert::IsFalse(policy.TakeCalibrationReload(), L"once per request");
			Assert::IsTrue(std::string(ReloadPolicy::CalibrationReloadNotice()).find("no Config Apply") != std::string::npos);
		}

		TEST_METHOD(OutputFactsMatchTheSwapchainFormat)
		{
			Assert::AreEqual(10u, ContainerBits(DXGI_FORMAT_R10G10B10A2_UNORM));
			Assert::AreEqual(16u, ContainerBits(DXGI_FORMAT_R16G16B16A16_FLOAT));
			Assert::AreEqual(8u, ContainerBits(DXGI_FORMAT_B8G8R8A8_UNORM));
			Assert::AreEqual(8u, ContainerBits(DXGI_FORMAT_R8G8B8A8_UNORM));
			Assert::AreEqual(0u, ContainerBits(DXGI_FORMAT_UNKNOWN));
			Assert::AreEqual(8u, CodeBits(10, "8"), L"8-in-10");
			Assert::AreEqual(10u, CodeBits(10, "10"));
			Assert::AreEqual(8u, CodeBits(8, "10"));
			Assert::AreEqual(10u, CodeBits(10, "auto"));
			Assert::AreEqual(16u, CodeBits(16, "8"), L"float containers are not limited (patterns refuse them)");
			Assert::AreEqual(1023.0, PatchMaxCode(DXGI_FORMAT_R10G10B10A2_UNORM));
			Assert::AreEqual(255.0, PatchMaxCode(DXGI_FORMAT_B8G8R8A8_UNORM));
			Assert::AreEqual(0.0, PatchMaxCode(DXGI_FORMAT_R16G16B16A16_FLOAT));
		}
	};
}
