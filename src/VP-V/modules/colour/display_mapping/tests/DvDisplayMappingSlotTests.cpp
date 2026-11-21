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
