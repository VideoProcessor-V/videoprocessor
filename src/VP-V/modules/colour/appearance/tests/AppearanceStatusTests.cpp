// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackPlainText.h>
#include <VP-V/modules/colour/appearance/AppearanceStage.h>
#include <VP-V/modules/colour/appearance/AppearanceStatus.h>
#include "../../../../tests/SettingsTestSupport.h"

#include <fstream>
#include <sstream>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// CF-1 CAM-4: the appearance keys, the OFF state shown, every degraded state listed, ambient_adapt's exclusion. Pure CPU;
// the settings code is read as text (it is not linked into this project), as the other config tests do.
namespace Tests
{
	namespace
	{
		// CF1-FIN MUST 3 / 4: read from the repository root (this file's own path), never the working directory; an
		// unreadable file FAILS the test.
		std::string ReadUp(const std::string& rel)
		{
			const std::string text = SettingsTestSupport::ReadFile(SettingsTestSupport::RepoRoot() + rel);
			Assert::IsFalse(text.empty(), std::wstring(rel.begin(), rel.end()).append(L" is readable from the repository root").c_str());
			return text;
		}
		bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
	}

	TEST_CLASS(AppearanceStatusTests)
	{
	public:
		TEST_METHOD(OffIsShownAndOnSaysWhatItReplaces)
		{
			Assert::AreEqual(std::string("appearance: off"), AppearanceStatus::ConfigLine(false, false, false));
			Assert::AreEqual(std::string("appearance: off"), AppearanceStatus::ConfigLine(false, true, true));
			const std::string on = AppearanceStatus::ConfigLine(true, true, false);
			Assert::IsTrue(Has(on, "replaces the gamma boost and saturation"));
			Assert::IsFalse(Has(on, "not measured"));
			Assert::IsTrue(Has(AppearanceStatus::ConfigLine(true, false, false), "screen white not measured"));
			Assert::IsTrue(Has(AppearanceStatus::ConfigLine(true, true, true), "ambient_adapt"));
		}

		TEST_METHOD(EveryDegradedStateIsACatalogRowWithAPlainSentence)
		{
			const FallbackCatalog::Decision rows[] = {
				FallbackCatalog::AppearanceShader(true),
				FallbackCatalog::RoomLightSensor(true, false, false),
				FallbackCatalog::RoomLightSensor(false, true, false),
				FallbackCatalog::RoomLightSensor(false, false, true),
				FallbackCatalog::AppearanceL11Reference(true),
				FallbackCatalog::AmbientAdaptReplaced(true) };
			for (const FallbackCatalog::Decision& d : rows)
			{
				Assert::IsTrue(d.engaged);
				Assert::IsFalse(d.reason.empty());
				Assert::IsTrue(std::string(FallbackPlainText::Sentence(d.id)).size() > 0, L"a plain sentence for the id");
			}
			// The checker (check_fallback_coverage) wants every catalog id named as a quoted id in a test.
			Assert::AreEqual(std::string("Colour appearance"), rows[0].id);
			Assert::AreEqual(std::string("room light sensor"), rows[1].id);
			Assert::AreEqual(std::string("appearance: L11 reference mode"), rows[4].id);
			Assert::AreEqual(std::string("ambient_adapt: replaced by appearance"), rows[5].id);
			Assert::IsFalse(FallbackCatalog::AppearanceShader(false).engaged);
			Assert::IsFalse(FallbackCatalog::RoomLightSensor(false, false, false).engaged);
			Assert::IsFalse(FallbackCatalog::AppearanceL11Reference(false).engaged);
			Assert::IsFalse(FallbackCatalog::AmbientAdaptReplaced(false).engaged);
		}

		TEST_METHOD(SensorStatesAreSetOnTheAutoRoomOnly)
		{
			RoomLight::Params p;
			RoomLight::Record r;
			RoomLight::Smoother s;
			r.holding = true;
			p.room = RoomLight::Room::Auto;
			RoomLight::Resolved held = RoomLight::Resolve(p, r, s, false, 0.0);
			Assert::IsTrue(held.sensorHeld);
			Assert::IsFalse(held.sensorMissing);
			r.holding = false;
			r.decaying = true;
			Assert::IsTrue(RoomLight::Resolve(p, r, s, false, 1.0).sensorDecaying);
			r = RoomLight::Record();
			Assert::IsTrue(RoomLight::Resolve(p, r, s, false, 2.0).sensorMissing);
			p.room = RoomLight::Room::Cinema;   // a preset room has no sensor to lose
			const RoomLight::Resolved preset = RoomLight::Resolve(p, r, s, false, 3.0);
			Assert::IsFalse(preset.sensorHeld || preset.sensorDecaying || preset.sensorMissing);
		}

		TEST_METHOD(L11ReferenceModeIsFlagged)
		{
			RoomLight::Smoother s;
			AppearanceStage::Inputs in;
			in.on = true;
			in.l11ReferenceMode = true;
			Assert::IsTrue(AppearanceStage::Decide(in, s).l11Ignored);
			in.honourL11 = false;
			Assert::IsFalse(AppearanceStage::Decide(in, s).l11Ignored);
		}

		TEST_METHOD(SettingsPlumbingIsInPlace)
		{
			// CF1-FIN MUST 3: every separator escaped (the single-backslash literals never named a file).
			const std::string cfg = "src\\VP-V\\modules\\control\\config\\";
			const std::string settings = ReadUp(cfg + "VpvRendererSettings.cpp");
			// Off by default; the slot gate, the Original pathway and the one-owner exclusion live in PostLoad.
			Assert::IsTrue(Has(settings, "settings.ambientAdaptReplaced = appearanceRuns && settings.ambientAdapt;"));
			Assert::IsTrue(Has(settings, "settings.ambientAdapt = false;"));
			Assert::IsTrue(Has(settings, "// key=appearance"));
			Assert::IsTrue(Has(settings, "// key=appearance_room"));
			const std::string fields = ReadUp(cfg + "VpvRendererFields.h");
			Assert::IsTrue(Has(fields, "bool appearance = false;"));
			Assert::IsTrue(Has(fields, "bool appearanceHonourL11 = true;"));
			Assert::IsTrue(Has(fields, "std::string appearanceRoom = \"cinema\";"));
			const std::string org = ReadUp(cfg + "config_pages\\VpvOrganisation.h");
			Assert::IsTrue(Has(org, "{ K::Toggle, \"appearance\" }"));
			Assert::IsTrue(Has(org, "{ K::Text, \"appearance_room\" }"));
			Assert::IsTrue(Has(org, "@appearance_status"));
			const std::string intent = ReadUp("src\\VP-V\\modules\\drive\\led_compensation\\LedIntentMethods.inl");
			Assert::IsTrue(Has(intent, "roomLightFeed = s.appearance && !s.pipelineOriginalPathway"));
			const std::string core = ReadUp("src\\VP-V\\segments\\CoreSegments.inl");
			Assert::IsTrue(Has(core, "FallbackCatalog::AmbientAdaptReplaced(settings.ambientAdaptReplaced)"));
		}
	};
}
