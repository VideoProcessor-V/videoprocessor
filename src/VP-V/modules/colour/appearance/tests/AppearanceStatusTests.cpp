// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackPlainText.h>
#include <VP-V/modules/colour/appearance/AppearanceStage.h>
#include <VP-V/modules/colour/appearance/AppearanceStatus.h>
#include "../../../../tests/SettingsTestSupport.h"

#include <cstring>
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
			// Line-ending-proof: the working copy may be CRLF (autocrlf) and the read is binary.
			std::string normal;
			normal.reserve(text.size());
			for (const char c : text)
				if (c != '\r')
					normal.push_back(c);
			return normal;
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

		// CF1-G7b: ambient_adapt is replaced only while appearance really ACTS (the rule the saturation step uses per frame).
		// PostLoad forces the stored ambient_adapt off for a configured appearance; a frame where the pass is not active (strength
		// 0, a calibration LUT before it, a lost shader) gives it back and does not list it as replaced. Before the fix the
		// replacement was decided at settings time, so such a frame lost ambient_adapt AND listed it as replaced.
		TEST_METHOD(AmbientAdaptIsReplacedOnlyWhileAppearanceActs)
		{
			// Configured appearance (replaced: the stored ambient_adapt was forced off), the pass acts: replaced and listed.
			Assert::IsFalse(AppearanceStage::AmbientAdaptGivenBack(true, true));
			Assert::IsFalse(AppearanceStage::AmbientAdaptOn(false, true, true), L"appearance acts: ambient_adapt stays off");
			Assert::IsTrue(FallbackCatalog::AmbientAdaptReplaced(true && !AppearanceStage::AmbientAdaptGivenBack(true, true)).engaged,
				L"and the replacement is listed");
			// Configured, but the pass does not act this frame: given back, not listed.
			Assert::IsTrue(AppearanceStage::AmbientAdaptGivenBack(true, false));
			Assert::IsTrue(AppearanceStage::AmbientAdaptOn(false, true, false), L"appearance idle: ambient_adapt runs again");
			Assert::IsFalse(FallbackCatalog::AmbientAdaptReplaced(true && !AppearanceStage::AmbientAdaptGivenBack(true, false)).engaged,
				L"and is not listed as replaced");
			// Nothing was replaced (appearance off, strength 0, Original pathway): the user's own ambient_adapt decides, and an
			// idle appearance never turns it on.
			for (const bool active : { false, true })
			{
				Assert::IsFalse(AppearanceStage::AmbientAdaptGivenBack(false, active));
				Assert::IsFalse(AppearanceStage::AmbientAdaptOn(false, false, active), L"not replaced and not asked for: off");
				Assert::IsTrue(AppearanceStage::AmbientAdaptOn(true, false, active), L"the user's ambient_adapt is untouched");
			}

			// The wiring: both per-render-cycle consumers and the fallback row use the per-frame appearance decision.
			const std::string intent = ReadUp("src/VP-V/modules/drive/led_compensation/LedIntentMethods.inl");
			Assert::IsTrue(Has(intent, "ambient.enabled = AppearanceStage::AmbientAdaptOn(s.ambientAdapt, s.ambientAdaptReplaced, dvStage.AppearanceDecision().active);"));
			const std::string motion = ReadUp("src/VP-V/modules/motion/consumers/IntentMotionMethods.inl");
			Assert::IsTrue(Has(motion, "switches.ambient = AppearanceStage::AmbientAdaptOn(s.ambientAdapt, s.ambientAdaptReplaced, dvStage.AppearanceDecision().active);"));
			const std::string core = ReadUp("src/VP-V/segments/CoreSegments.inl");
			Assert::IsTrue(Has(core, "AppearanceStage::AmbientAdaptGivenBack(activeSettings.ambientAdaptReplaced, dvStage.AppearanceDecision().active)"),
				L"the AmbientAdaptReplaced fallback is synced per frame");
			Assert::IsTrue(Has(core, "SyncFallback(FallbackCatalog::AmbientAdaptReplaced(activeSettings.ambientAdaptReplaced &&"));
		}

		// CF1-G7b: the per-frame decision sits where it runs every frame. The AmbientAdaptReplaced fallback row is synced INSIDE the
		// per-frame CollectFrameHooks body (not only at the settings seam, which runs when settings change), and the two intent
		// consumers take ambient_adapt back inside their own per-render-cycle methods. Each needle is searched only within its
		// function body (a stray copy elsewhere does not satisfy it), and the sources are read line-ending-proof. A missing file FAILS.
		TEST_METHOD(AmbientAdaptReturnsWhenAppearanceStopsActing)
		{
			auto body = [](const std::string& text, const char* signature)
			{
				const size_t begin = text.find(signature);
				Assert::IsTrue(begin != std::string::npos, (std::wstring(L"function found: ") + std::wstring(signature, signature + std::strlen(signature))).c_str());
				const size_t end = text.find("\n}\n", begin);
				Assert::IsTrue(end != std::string::npos, L"function end found");
				return text.substr(begin, end - begin);
			};
			const std::string core = ReadUp("src/VP-V/segments/CoreSegments.inl");
			const std::string frame = body(core, "LibplaceboVideoRenderer::Impl::CollectFrameHooks(");
			Assert::IsTrue(Has(frame, "SyncFallback(FallbackCatalog::AmbientAdaptReplaced(activeSettings.ambientAdaptReplaced &&"),
				L"the AmbientAdaptReplaced row is synced per frame");
			Assert::IsTrue(Has(frame, "AppearanceStage::AmbientAdaptGivenBack(activeSettings.ambientAdaptReplaced, dvStage.AppearanceDecision().active)"),
				L"from this frame's appearance decision");
			const std::string seam = body(core, "LibplaceboVideoRenderer::Impl::UpdatePipelineSlots(");
			Assert::IsFalse(Has(seam, "AmbientAdaptGivenBack"), L"the settings seam only states the configured replacement");
			const std::string intent = ReadUp("src/VP-V/modules/drive/led_compensation/LedIntentMethods.inl");
			Assert::IsTrue(Has(body(intent, "LibplaceboVideoRenderer::Impl::ConfigureIntentAmbient()"),
				"ambient.enabled = AppearanceStage::AmbientAdaptOn(s.ambientAdapt, s.ambientAdaptReplaced, dvStage.AppearanceDecision().active);"));
			const std::string motion = ReadUp("src/VP-V/modules/motion/consumers/IntentMotionMethods.inl");
			Assert::IsTrue(Has(body(motion, "LibplaceboVideoRenderer::Impl::ConfigureIntentMotion()"),
				"switches.ambient = AppearanceStage::AmbientAdaptOn(s.ambientAdapt, s.ambientAdaptReplaced, dvStage.AppearanceDecision().active);"));
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
