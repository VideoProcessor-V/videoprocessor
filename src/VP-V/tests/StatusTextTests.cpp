// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// WP-11: VP's status text builders (src/VP-V/core/StatusText.h) and VP's shortcut table
// (src/VP-V/gui/VpShortcuts.h), pure and CPU-only. The expected strings are copies of the text
// the old dialog/overlay/renderer code produced at chore/early-migrations ac49a1de. The
// MFC-level before/after comparison is VpStatusAdapterTests.cpp.

#include <VP-V/core/StatusText.h>
#include <VP-V/gui/VpShortcuts.h>

#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpStatusText;

namespace StatusTextTest
{
	namespace
	{
		// docs/design/net-runner-wiring.patch (feat/nets 328b6d29), the Ctrl+I descriptions hunk,
		// verbatim except that the renderer's members are parameters.
		std::string DraftNetsLine(bool repairLoaded, const std::string& repairName, bool fp16, double repairMs,
			bool dLoaded, const std::string& dName, double dMs)
		{
			std::string nets = "Nets:";
			if (repairLoaded) nets += " " + repairName + (fp16 ? " (fp16" : " (fp32") +
				", " + std::to_string(repairMs).substr(0, 4) + " ms)";
			if (dLoaded) nets += " " + dName + " (" + std::to_string(dMs).substr(0, 4) + " ms)";
			return nets == "Nets:" ? "Nets: none running" : nets;
		}

		std::string Narrow(const std::string& s) { return s; }
	}

	TEST_CLASS(StatusTextTests)
	{
	public:
		TEST_METHOD(RendererModuleRoundTripKeepsKnownAndEmptyLines)
		{
			RendererLines<char> lines;
			lines.dejudder.Set(true, "on (24p in 60 Hz), motion: NVOF");
			lines.fallbacks.Set(true, "Fallbacks engaged: dither (libplacebo dither; x=1)");
			lines.ledDrive.Set(true, "");                 // known but empty: kept as known
			lines.sourceResolution.Set(false, "ignored"); // unknown: absent
			const std::string text = ComposeRendererModule(lines);
			Assert::AreEqual(std::string("dejudder=on (24p in 60 Hz), motion: NVOF\n"
				"fallbacks=Fallbacks engaged: dither (libplacebo dither; x=1)\nled_drive="), text);
			const RendererLines<char> back = ReadRendererModule(text + "\nnewer_key=from a newer renderer");
			Assert::IsTrue(back.dejudder.known && back.dejudder.text == lines.dejudder.text);
			Assert::IsTrue(back.fallbacks.Shown());
			Assert::IsTrue(back.ledDrive.known && !back.ledDrive.Shown(), L"known but empty shows nothing, as before");
			Assert::IsFalse(back.sourceResolution.known);
		}

		TEST_METHOD(RendererModuleAnswersVpAndTheIndexOnly)
		{
			int fills = 0;
			const auto fill = [&fills](RendererLines<char>& l) { ++fills; l.fallbacks.Set(true, "Fallbacks engaged: none"); };
			std::string out;
			Assert::IsTrue(RendererModuleStatus<char>("vp", fill, out));
			Assert::AreEqual(std::string("fallbacks=Fallbacks engaged: none"), out);
			Assert::IsTrue(RendererModuleStatus<char>("", fill, out));
			Assert::AreEqual(std::string("module=vp"), out);
			Assert::IsTrue(RendererModuleStatus<char>(nullptr, fill, out));
			Assert::IsFalse(RendererModuleStatus<char>("vpx", fill, out));
			Assert::IsFalse(RendererModuleStatus<char>("v", fill, out));
			Assert::IsTrue(out.empty());
			Assert::AreEqual(1, fills, L"the getters are read only for module vp");
		}

		TEST_METHOD(RowsUseTheOldLabelsOrderAndColumn)
		{
			CtrlILines<char> lines;
			lines.keyOverrides = "fullscreen on";
			lines.room = "dark (key)";
			lines.content = "film (inferred)";
			lines.motionIntent = "m";
			lines.ambientIntent = "a";
			lines.blackIntent = "b";
			lines.displayIntent = "d";
			lines.dejudder = "on";
			lines.fallbacks = "Fallbacks engaged: none";
			const CtrlIRows<char> rows = BuildCtrlIRows(lines);
			// The old StatsOverlayWindow.cpp format strings, filled.
			const char* expectedA[] = { "Key override:     fullscreen on", "Room:             dark (key)",
				"Content:          film (inferred)", "Motion intent:    m", "Ambient:          a",
				"Black:            b", "Display:          d" };
			Assert::AreEqual(size_t(7), rows.afterShaderRule.size());
			for (size_t i = 0; i < 7; ++i)
				Assert::AreEqual(std::string(expectedA[i]), FormatRow(rows.afterShaderRule[i]));
			Assert::AreEqual(size_t(2), rows.afterShaders.size());
			Assert::AreEqual(std::string("De-judder:        on"), FormatRow(rows.afterShaders[0]));
			Assert::AreEqual(std::string("Fallbacks engaged: none"), FormatRow(rows.afterShaders[1]));
		}

		TEST_METHOD(ResyncKeysRoundTripAndAreOptional)
		{
			RendererLines<char> lines;
			lines.dejudder.Set(true, "on");
			lines.resync.Set(true, "shadow: no FULL yet; minor x0 / 10 min; timing x0");
			lines.resyncEvents.Set(true, "era=1 capacity=64 ancient_below=0; #1 seq=1 full start");
			const std::string text = ComposeRendererModule(lines);
			Assert::AreEqual(std::string("dejudder=on\n"
				"resync=shadow: no FULL yet; minor x0 / 10 min; timing x0\n"
				"resync_events=era=1 capacity=64 ancient_below=0; #1 seq=1 full start"), text);
			const RendererLines<char> back = ReadRendererModule(text);
			Assert::IsTrue(back.resync.known && back.resync.text == lines.resync.text);
			Assert::IsTrue(back.resyncEvents.known && back.resyncEvents.text == lines.resyncEvents.text);

			// Absent keys (an old renderer): unknown, and the old text is unchanged.
			RendererLines<char> old;
			old.dejudder.Set(true, "on");
			Assert::AreEqual(std::string("dejudder=on"), ComposeRendererModule(old));
			const RendererLines<char> oldBack = ReadRendererModule(std::string("dejudder=on"));
			Assert::IsFalse(oldBack.resync.known);
			Assert::IsFalse(oldBack.resyncEvents.known);
		}

		TEST_METHOD(ResyncRowFollowsDejudderAndPrecedesFallbacks)
		{
			CtrlILines<char> lines;
			lines.dejudder = "on";
			lines.resync = "shadow: no FULL yet; minor x0 / 10 min; timing x0";
			lines.fallbacks = "Fallbacks engaged: none";
			const CtrlIRows<char> rows = BuildCtrlIRows(lines);
			Assert::AreEqual(size_t(3), rows.afterShaders.size());
			Assert::AreEqual(std::string("De-judder:        on"), FormatRow(rows.afterShaders[0]));
			Assert::AreEqual(std::string("Resync:           shadow: no FULL yet; minor x0 / 10 min; timing x0"),
				FormatRow(rows.afterShaders[1]));
			Assert::AreEqual(std::string("Fallbacks engaged: none"), FormatRow(rows.afterShaders[2]));

			lines.resync.clear();   // empty = no row, as every other line
			Assert::AreEqual(size_t(2), BuildCtrlIRows(lines).afterShaders.size());
		}

		TEST_METHOD(BundleEmitsResyncKeysAfterOutputModeOnlyWhenKnown)
		{
			BundleInputs<char> in;
			in.outputMode.Set(true, "Target");
			Assert::AreEqual(size_t(1), BundleRendererSettings(in, Narrow).size());
			in.resync.Set(true, "shadow: no FULL yet; minor x0 / 10 min; timing x0");
			in.resyncEvents.Set(true, "era=2 capacity=64 ancient_below=0; ");
			const auto settings = BundleRendererSettings(in, Narrow);
			Assert::AreEqual(size_t(3), settings.size());
			Assert::AreEqual(std::string("output_mode"), settings[0].first);
			Assert::AreEqual(std::string("resync"), settings[1].first);
			Assert::AreEqual(std::string("resync_events"), settings[2].first);
		}

		TEST_METHOD(CrashStateKeysAreUnchangedByResync)
		{
			Assert::AreEqual(size_t(11), sizeof(kCrashStateRendererKeys) / sizeof(kCrashStateRendererKeys[0]));
		}

		TEST_METHOD(EmptyLinesGiveNoRows)
		{
			CtrlILines<wchar_t> lines;
			lines.blackIntent = L"report: no shadow statistics";
			const CtrlIRows<wchar_t> rows = BuildCtrlIRows(lines);
			Assert::AreEqual(size_t(1), rows.afterShaderRule.size());
			Assert::AreEqual(std::wstring(L"Black:            report: no shadow statistics"), FormatRow(rows.afterShaderRule[0]));
			Assert::IsTrue(rows.afterShaders.empty());
		}

		TEST_METHOD(ContentLineMatchesTheOldWording)
		{
			ContentFacts f;
			Assert::AreEqual(std::string(), ContentLine(f), L"intent_content off");
			f.enabled = true;
			f.reason = "short window";
			Assert::AreEqual(std::string("classifying (short window)"), ContentLine(f));
			f.classified = true;
			f.content = "sports";
			f.source = "inferred";
			Assert::AreEqual(std::string("sports (inferred)"), ContentLine(f));
			f.pendingCandidate = "film";
			Assert::AreEqual(std::string("sports (inferred), next film at a cut"), ContentLine(f));
		}

		TEST_METHOD(AmbientLineMatchesTheOldWording)
		{
			AmbientFacts f;
			Assert::AreEqual(std::string(), AmbientLine(f), L"[ambient] mode off");
			f.enabled = true;
			f.value = "lit";
			Assert::AreEqual(std::string("lit (sensor: no port set; saved room)"), AmbientLine(f));
			f.portSet = true;
			Assert::AreEqual(std::string("lit (sensor not reporting; saved room)"), AmbientLine(f));
			f.sensed = true;
			f.lux = 12.345;
			Assert::AreEqual(std::string("lit (sensor 12.3 lux)"), AmbientLine(f));
			f.manual = true;
			Assert::AreEqual(std::string("lit (manual)"), AmbientLine(f));
			f.keyOverride = true;
			Assert::AreEqual(std::string("lit (key)"), AmbientLine(f), L"the key wins over every mode");
		}

		TEST_METHOD(KeyOverrideSummaryJoinsWithCommas)
		{
			Assert::AreEqual(std::wstring(), KeyOverrideSummary(std::vector<std::wstring>{}));
			Assert::AreEqual(std::wstring(L"motion Natural, fullscreen on, room lights dark"),
				KeyOverrideSummary(std::vector<std::wstring>{ L"motion Natural", L"fullscreen on", L"room lights dark" }));
		}

		TEST_METHOD(NetsLineIsTheDraftsLine)
		{
			const double times[] = { 0.0, 0.5, 1.23456, 12.5, 123.456 };
			for (double a : times)
				for (double d : times)
					for (int mask = 0; mask < 8; ++mask)
					{
						NetsFacts f;
						f.repairLoaded = (mask & 1) != 0;
						f.repairName = "A_motion_small";
						f.repairFp16 = (mask & 2) != 0;
						f.repairMs = a;
						f.dLoaded = (mask & 4) != 0;
						f.dName = "D_hybrid_refine";
						f.dMs = d;
						Assert::AreEqual(DraftNetsLine(f.repairLoaded, f.repairName, f.repairFp16, a, f.dLoaded, f.dName, d),
							NetsLine(f));
					}
			Assert::AreEqual(std::string("Nets: none running"), NetsLine(NetsFacts()));
		}

		TEST_METHOD(CrashStateLinesWithoutARendererAreAllDashes)
		{
			const auto lines = CrashStateRendererLines(false, CrashStateInputs<char>(), Narrow);
			Assert::AreEqual(size_t(11), lines.size());
			for (const auto& line : lines)
				Assert::AreEqual(std::string("-"), line.second);
			Assert::AreEqual(std::string("display intent"), lines.back().first);
		}

		TEST_METHOD(CrashStateLinesDashUnknownAndEmpty)
		{
			CrashStateInputs<char> in;
			in.dejudder.Set(true, "on");
			in.fallbacks.Set(true, "");
			in.ledDrive.Set(false, "x");
			in.content = "film (inferred)";
			const auto lines = CrashStateRendererLines(true, in, Narrow);
			Assert::AreEqual(std::string("on"), lines[0].second);
			Assert::AreEqual(std::string("-"), lines[1].second, L"known but empty");
			Assert::AreEqual(std::string("-"), lines[4].second, L"unknown");
			Assert::AreEqual(std::string("film (inferred)"), lines[6].second);
			Assert::AreEqual(std::string("-"), lines[7].second, L"intent off");
		}

		TEST_METHOD(BundleKeepsKnownLinesEvenWhenEmpty)
		{
			BundleInputs<char> in;
			in.presentation.Set(true, "ready");
			in.fallbacks.Set(true, "");
			in.outputMode.Set(true, "Target");
			const auto settings = BundleRendererSettings(in, Narrow);
			Assert::AreEqual(size_t(3), settings.size());
			Assert::AreEqual(std::string("presentation"), settings[0].first);
			Assert::AreEqual(std::string("fallbacks"), settings[1].first);
			Assert::AreEqual(std::string(), settings[1].second);
			Assert::AreEqual(std::string("output_mode"), settings[2].first);
		}

		TEST_METHOD(VpShortcutsRebuildTheOldTableOrder)
		{
			struct Def { const char* configKey; unsigned short command; };
			// VideoProcessorDlg.cpp after the patch: the host's own table (fake ids for Bill's).
			const Def host[] = {
				{ "auto_set", 1 }, { "fullscreen_exit", 2 }, { "fullscreen_toggle", 3 }, { "toggle_stats_overlay", 4 },
				{ "capture_rendered_output", 5 }, { "reapply_rules", 6 }, { "show_profiles", 7 }, { "pq_set", 8 },
				{ "renderer_restart", 9 }, { "renderer_reset", 10 }, { "capture_1", 11 }, { "capture_2", 12 },
				{ "capture_3", 13 }, { "capture_4", 14 }, { "video_conversion_off", 15 }, { "video_conversion_p010", 16 },
				{ "config_editor", 17 }, { "toggle_noui", 18 }, { "display_rules_auto", 19 } };
			// SHORTCUT_DEFINITIONS at ac49a1de, in order.
			const char* old[] = { "auto_set", "fullscreen_exit", "fullscreen_toggle", "toggle_stats_overlay",
				"capture_rendered_output", "state_report", "raw_capture", "reapply_rules", "room_lights", "show_profiles",
				"pq_set", "renderer_restart", "renderer_reset", "capture_1", "capture_2", "capture_3", "capture_4",
				"video_conversion_off", "video_conversion_p010", "config_editor", "toggle_noui", "display_rules_auto" };
			const std::vector<Def> merged = VpvShortcuts::Merge(host);
			Assert::AreEqual(sizeof(old) / sizeof(old[0]), merged.size());
			for (size_t i = 0; i < merged.size(); ++i)
				Assert::AreEqual(std::string(old[i]), std::string(merged[i].configKey));
			Assert::AreEqual(33207, static_cast<int>(merged[5].command), L"state_report keeps 33207 (on-screen test posts it)");
			Assert::AreEqual(33208, static_cast<int>(merged[6].command));
			Assert::AreEqual(33206, static_cast<int>(merged[8].command));
		}

		TEST_METHOD(VpCommandBlockIsBelowTheUnifiedProfiles)
		{
			Assert::IsTrue(VpvShortcuts::IsVpCommand(ID_COMMAND_ROOM_LIGHTS));
			Assert::IsTrue(VpvShortcuts::IsVpCommand(ID_COMMAND_RAW_CAPTURE_TOGGLE));
			Assert::IsFalse(VpvShortcuts::IsVpCommand(33205), L"ID_COMMAND_SUBTITLE_TOGGLE (Bill's)");
			Assert::IsFalse(VpvShortcuts::IsVpCommand(33300), L"ID_COMMAND_UNIFIED_PROFILE_FIRST");
			for (const auto& def : VpvShortcuts::kDefinitions)
				Assert::IsTrue(VpvShortcuts::IsVpCommand(def.command));
		}
	};
}
