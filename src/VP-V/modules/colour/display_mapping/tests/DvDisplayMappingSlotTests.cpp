// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackPlainText.h>
#include <VP-V/core/OrderValidator.h>
#include <VP-V/core/OriginalPathway.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutHostPolicy.h>
#include "../../../../tests/SettingsTestSupport.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// CF-1 P3: the DV display-mapping slots (colour.dm_curve, colour.dm_trims) are real slots with their own
// switches, an either/or master (dv_display_mapping), a pin in the order, and the calibration LUT "auto" stage.
namespace Tests
{
	namespace
	{
		using PipelineSlots::Id;
		inline std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
		// The source tree from this file's own path (SettingsTestSupport::RepoRoot); an unreadable file FAILS the test.
		inline std::string ReadUp(const std::string& rel)
		{
			const std::string text = SettingsTestSupport::ReadFile(SettingsTestSupport::RepoRoot() + rel);
			Assert::IsFalse(text.empty(), Wide(rel + " is readable from the repository root").c_str());
			return text;
		}
		inline bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
		inline std::vector<Id> Moved(const std::vector<Id>& base, Id id, Id before)
		{
			std::vector<Id> order = base;
			order.erase(std::remove(order.begin(), order.end(), id), order.end());
			order.insert(std::find(order.begin(), order.end(), before), id);
			return order;
		}
	}

	TEST_CLASS(OrderValidatorDmPinTests)
	{
	public:
		TEST_METHOD(DesignedOrderIsValidWithTheDmSlotsBuilt)
		{
			std::string reason;
			Assert::IsTrue(PipelineSlots::Validate(PipelineSlots::DefaultOrder(), &reason), Wide(reason).c_str());
		}
		TEST_METHOD(TrimsMustPrecedeAppearance)
		{
			std::string reason;
			const auto order = Moved(PipelineSlots::DefaultOrder(), Id::ColourDmTrims, Id::DriveLedMeasure);
			Assert::IsFalse(PipelineSlots::Validate(order, &reason));
			Assert::IsTrue(reason.find("colour.dm_trims must run before colour.appearance") != std::string::npos, Wide(reason).c_str());
		}
		TEST_METHOD(AppearanceMustPrecedeTheLedMeasurement)
		{
			std::string reason;
			const auto order = Moved(PipelineSlots::DefaultOrder(), Id::ColourAppearance, Id::DriveLedCompensation);
			Assert::IsFalse(PipelineSlots::Validate(order, &reason));
			// CF1-FIN SHOULD D: rejected by THIS rule, not by another one.
			Assert::IsTrue(reason.find("colour.appearance must run before drive.led_measure") != std::string::npos, Wide(reason).c_str());
		}
		TEST_METHOD(DmSlotsAreBuiltAndModifyTheTargetLight)
		{
			for (Id id : { Id::ColourDmCurve, Id::ColourDmTrims })
			{
				Assert::IsTrue(PipelineSlots::Get(id).status == PipelineSlots::Status::Built);
				Assert::IsTrue(PipelineSlots::ContractOf(id).modifies);
			}
			Assert::IsTrue(PipelineSlots::Get(Id::ColourDmPre).status == PipelineSlots::Status::Placeholder);
		}
	};

	TEST_CLASS(DvDisplayMappingOriginalTests)
	{
	public:
		TEST_METHOD(OriginalRowsNameTheDmSlots)
		{
			bool master = false, trims = false, curve = false;
			for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
			{
				if (!std::strcmp(o.key, "dv_display_mapping")) { master = true; Assert::AreEqual("original", o.value); }
				if (!std::strcmp(o.key, "dv_trims")) { trims = true; Assert::AreEqual("colour.dm_trims", o.slot); }
				if (!std::strcmp(o.key, "dv_tone_mapping")) { curve = true; Assert::AreEqual("colour.dm_curve", o.slot); }
			}
			Assert::IsTrue(master && trims && curve);
		}
		TEST_METHOD(OriginalPathwayTurnsTheDmSlotsOff)
		{
			PipelineSlots::Registry registry;
			Assert::IsTrue(registry.Runs(Id::ColourDmTrims) && registry.Runs(Id::ColourDmCurve));
			registry.ApplyOriginalPathway();
			Assert::IsFalse(registry.Runs(Id::ColourDmTrims));
			Assert::IsFalse(registry.Runs(Id::ColourDmCurve));
		}
	};

	TEST_CLASS(DmSlotGateTests)
	{
	public:
		TEST_METHOD(SdrToneShapeOffNoLongerSwitchesDvOff)
		{
			PipelineSlots::Registry registry;
			registry.SetEnabled(Id::ColourSdrToneShape, false);
			Assert::IsTrue(registry.Runs(Id::ColourDmCurve));
			Assert::IsTrue(registry.Runs(Id::ColourDmTrims));
		}
		TEST_METHOD(EachDmSlotHasItsOwnSwitchHonoured)
		{
			Assert::IsTrue(PipelineSlots::SwitchHonoured(Id::ColourDmCurve));
			Assert::IsTrue(PipelineSlots::SwitchHonoured(Id::ColourDmTrims));
			PipelineSlots::Registry registry;
			registry.SetEnabled(Id::ColourDmTrims, false);
			Assert::IsFalse(registry.Runs(Id::ColourDmTrims));
			Assert::IsTrue(registry.Runs(Id::ColourDmCurve));
		}
	};

	TEST_CLASS(CalibrationLutAutoStageTests)
	{
	public:
		TEST_METHOD(AutoTakesTheVpStageWhileTheTrimsRun)
		{
			Assert::AreEqual(std::string("vp"), CalibrationLutHost::ResolveStage("auto", true));
			Assert::AreEqual(std::string("libplacebo"), CalibrationLutHost::ResolveStage("auto", false));
		}
		TEST_METHOD(ExplicitChoicesPassThrough)
		{
			Assert::AreEqual(std::string("vp"), CalibrationLutHost::ResolveStage("vp", false));
			Assert::AreEqual(std::string("libplacebo"), CalibrationLutHost::ResolveStage("libplacebo", true));
			Assert::AreEqual(std::string("libplacebo"), CalibrationLutHost::ResolveStage("", true));
		}
		// CF1-G7a: an explicit calibration_lut_stage = vp must take the VP stage on the FIRST frame. The Adapter's
		// autoConfigured started true (auto) and only the colour segment's per-frame SetAutoNeed, which runs AFTER TakeLut,
		// corrected it, so the first frame went to libplacebo's target LUT. The fix initialises it from the resolved setting at
		// the settings seam. The Adapter (CalibrationLutHost.cpp) is not linked into this project, so the decision is checked
		// as the real policy functions plus the wiring read as text (as CamFix1Tests does): reverting the seam call fails here.
		TEST_METHOD(ExplicitVpTakesTheVpStageOnTheFirstFrame)
		{
			using namespace CalibrationLutHost;
			// The policy on the first frame: nothing has wanted a step yet (stepWants false). Auto keeps libplacebo's LUT; an
			// explicit vp (configuredAuto false, from the seam) takes the VP stage.
			StageFacts facts;
			facts.vpStageWanted = true;   // calibration_lut_stage resolves to vp
			facts.slotRuns = true;
			Assert::AreEqual(std::string("vp"), ResolveStage("vp", false));
			Assert::IsTrue(AutoKeepsVpStage(false, false), L"explicit vp keeps the VP stage with no step wanting it");
			StageFacts effective = facts;
			effective.vpStageWanted = facts.vpStageWanted && AutoKeepsVpStage(false, false);
			Assert::IsTrue(UseVpStage(effective, false), L"first frame: the VP stage takes the LUT");
			Assert::IsFalse(AutoKeepsVpStage(true, false), L"the stale initial value (auto, no step) is the libplacebo frame the fix removes");

			// The wiring: the Adapter takes the configured stage from the settings, without touching the per-frame step need.
			const std::string host = ReadUp("src\VP-V\modules\output\calibration_lut\CalibrationLutHost.h");
			Assert::IsTrue(Has(host, "void NoteConfiguredStage(bool configuredAuto) { autoConfigured = configuredAuto; }"),
				L"NoteConfiguredStage stores the configured stage and nothing else");
			// The settings seam calls it from the settings' resolved-or-configured value, inside UpdatePipelineSlots.
			const std::string core = ReadUp("src\VP-V\segments\CoreSegments.inl");
			const size_t begin = core.find("LibplaceboVideoRenderer::Impl::UpdatePipelineSlots(const RendererSettings& settings)");
			Assert::IsTrue(begin != std::string::npos, L"UpdatePipelineSlots is the settings seam");
			const size_t end = core.find("\n}\n", begin);
			Assert::IsTrue(end != std::string::npos);
			const std::string seam = core.substr(begin, end - begin);
			Assert::IsTrue(Has(seam, "calibrationHost.NoteConfiguredStage(settings.calibrationLutStageConfigured == \"auto\");"),
				L"the seam initialises the stage from the user's configured value");
			// SetAutoNeed stays per frame (the step need), after the frame's decisions.
			Assert::IsTrue(Has(core, "calibrationHost.SetAutoNeed(activeSettings.calibrationLutStageConfigured == \"auto\","));
		}


	};

	TEST_CLASS(DmFallbackCatalogTests)
	{
	public:
		TEST_METHOD(CalibrationLutDisplayMappingIsVisibleWhileItLasts)
		{
			const auto on = FallbackCatalog::CalibrationLutDisplayMapping(true);
			Assert::IsTrue(on.engaged);
			Assert::AreEqual(std::string("calibration LUT: display mapping"), on.id);
			Assert::IsFalse(FallbackCatalog::CalibrationLutDisplayMapping(false).engaged);
		}
		TEST_METHOD(UnsupportedContainerIsVisible)
		{
			const auto on = FallbackCatalog::DvDisplayMappingContainer(true, "R12B");
			Assert::IsTrue(on.engaged);
			Assert::AreEqual(std::string("DV display mapping: container"), on.id);
			Assert::IsFalse(FallbackCatalog::DvDisplayMappingContainer(false, "").engaged);
		}
	};
}
