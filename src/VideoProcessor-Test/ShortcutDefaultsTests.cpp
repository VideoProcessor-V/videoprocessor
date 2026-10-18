/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// VP's compiled [shortcuts] defaults (ShortcutDefaults.h, 2026-10-18): one
// table for the player and Config. No two defaults may share a chord, none
// may take a chord another branch already ships (kReservedShortcuts: the
// report-a-problem shortcut), and the shipped VideoProcessor.cfg's own
// shortcuts must not land on a default. The raw-capture recorder used to
// default to Ctrl+Alt+D, the state_report chord.
#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <ConfigurationLiveApply.h>
#include <RendererProfileConfig.h>
#include <ShortcutDefaults.h>

#include <algorithm>
#include <map>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace
	{
		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		std::string Canonical(const char* chord)
		{
			std::string canonical;
			Assert::IsTrue(RendererProfileConfig::CanonicalizeKeyChord(chord, canonical),
				Wide(std::string("not a valid chord: ") + chord).c_str());
			return canonical;
		}

		std::string RepositoryFile(const char* name)
		{
			std::string path = __FILE__;
			std::replace(path.begin(), path.end(), '/', '\\');
			const size_t at = path.rfind("\\src\\VideoProcessor-Test\\");
			return path.substr(0, at) + "\\" + name;
		}

		// The chord ConfigurationLiveApply's virtual-key defaults spell.
		std::string ChordOf(uint16_t key, uint8_t modifiers)
		{
			std::string text;
			if (modifiers & 0x08) text += "Ctrl+";   // FCONTROL
			if (modifiers & 0x10) text += "Alt+";    // FALT
			if (modifiers & 0x04) text += "Shift+";  // FSHIFT
			if (key == 0x0d) text += "Enter";
			else if (key == 0x1b) text += "Esc";
			else text += static_cast<char>(key);
			return text;
		}
	}

	TEST_CLASS(ShortcutDefaultsTests)
	{
	public:
		TEST_METHOD(NoTwoDefaultShortcutsCollide)
		{
			std::map<std::string, std::string> owner;
			for (const ShortcutDefault& entry : kShortcutDefaults)
			{
				if (entry.chord[0] == '\0')
					continue;
				const std::string chord = Canonical(entry.chord);
				const auto inserted = owner.emplace(chord, entry.key);
				Assert::IsTrue(inserted.second, Wide(std::string(entry.key) + " and " + inserted.first->second +
					" both default to " + chord).c_str());
			}
			for (const ReservedShortcut& reserved : kReservedShortcuts)
			{
				const std::string chord = Canonical(reserved.chord);
				const auto found = owner.find(chord);
				Assert::IsTrue(found == owner.end(), Wide(found == owner.end() ? std::string() :
					found->second + " takes " + chord + ", already " + reserved.owner).c_str());
			}
			// Every key appears once.
			std::map<std::string, int> keys;
			for (const ShortcutDefault& entry : kShortcutDefaults)
				Assert::AreEqual(1, ++keys[entry.key], Wide(entry.key).c_str());
		}

		TEST_METHOD(RawCaptureHasItsOwnConfigurableShortcut)
		{
			const ShortcutDefault* raw = FindShortcutDefault("raw_capture");
			Assert::IsNotNull(raw, L"[shortcuts] raw_capture");
			Assert::AreEqual(std::string("Ctrl+Alt+W"), Canonical(raw->chord));
			Assert::AreNotEqual(Canonical("Ctrl+Alt+D"), Canonical(raw->chord), L"Ctrl+Alt+D is the report-a-problem shortcut");
			Assert::IsNull(FindShortcutDefault("raw_capture_toggle"), L"the old key is gone");
			// Ctrl+Alt+R stays free for user bindings (reapply_rules in the Config tests).
			for (const ShortcutDefault& entry : kShortcutDefaults)
				if (entry.chord[0] != '\0')
					Assert::AreNotEqual(Canonical("Ctrl+Alt+R"), Canonical(entry.chord), Wide(entry.key).c_str());
		}

		// The two defaults ConfigurationLiveApply spells as virtual keys agree
		// with the table.
		TEST_METHOD(LiveApplyDefaultsMatchTheTable)
		{
			Assert::AreEqual(Canonical(ChordOf(ConfigurationLiveApply::ViewToggleDefaultKey,
				ConfigurationLiveApply::ViewToggleDefaultModifiers).c_str()), Canonical(ShortcutDefaultChord("fullscreen_toggle")));
			Assert::AreEqual(Canonical(ChordOf(ConfigurationLiveApply::VideoOnlyToggleDefaultKey,
				ConfigurationLiveApply::VideoOnlyToggleDefaultModifiers).c_str()), Canonical(ShortcutDefaultChord("toggle_noui")));
		}

		// The shipped VideoProcessor.cfg: its [shortcuts] values and every
		// profile's shortcut / cycle_shortcut stay off the compiled defaults
		// (Config reports such a pair as a clash).
		TEST_METHOD(ShippedConfigurationShortcutsAvoidTheDefaults)
		{
			ConfigFile config;
			Assert::IsTrue(config.Load(RepositoryFile("VideoProcessor.cfg"), ConfigFile::ReadPolicy::Fresh));
			std::map<std::string, std::string> defaults;
			for (const ShortcutDefault& entry : kShortcutDefaults)
				if (entry.chord[0] != '\0' && !entry.rendererSpecific)
					defaults.emplace(Canonical(entry.chord), entry.key);
			size_t checked = 0;
			for (const std::string& section : config.GetSectionNames())
			{
				const auto* values = config.GetSectionValues(section);
				if (!values) continue;
				for (const auto& kv : *values)
				{
					const std::string key = ConfigFile::NormalizeName(kv.first);
					const bool shortcutSection = ConfigFile::NormalizeName(section) == "shortcuts";
					if (!(key == "shortcut" || key == "cycle_shortcut" || (shortcutSection && key != "foreground_only")))
						continue;
					const std::string value = ConfigFile::Trim(kv.second);
					std::string canonical;
					if (value.empty() || !RendererProfileConfig::CanonicalizeKeyChord(value, canonical))
						continue;
					++checked;
					if (shortcutSection && FindShortcutDefault(key.c_str()))
						continue;   // an explicit value for a fixed command replaces its default
					const auto clash = defaults.find(canonical);
					Assert::IsTrue(clash == defaults.end(), Wide("[" + section + "] " + kv.first + " " + canonical +
						" is the default of " + (clash == defaults.end() ? std::string() : clash->second)).c_str());
				}
			}
			Assert::IsTrue(checked > 5, L"the shipped configuration's shortcuts were read");
		}
	};
}
