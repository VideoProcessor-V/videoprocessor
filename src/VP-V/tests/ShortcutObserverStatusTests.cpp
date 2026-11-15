// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S-3f b (lead 17 MUST-1): the "global shortcuts" Fallbacks entry reached through the existing
// DialogExtension::OnTimer seam (src/VP-V/gui/ShortcutObserverStatus.h) and added to the poll's
// Fallbacks line by VpvGui::WithHostFallbacks (VpStatusAdapter.h).
//   wanted + failed -> one entry, one notice; Original pathway -> no entry; running -> no entry.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FallbackStatus.h>
#include <VP-V/gui/ShortcutObserverStatus.h>
#include <VP-V/gui/VpStatusAdapter.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ShortcutObserverStatusTest
{
	namespace
	{
		// VP's global keys wanted: modern interface, UI shown, foreground_only off, one global action.
		DialogExtension::DialogFacts GlobalKeysWanted(bool observerRunning)
		{
			DialogExtension::DialogFacts facts;
			facts.modernInterface = true;
			facts.noUi = false;
			facts.shortcutsForegroundOnly = false;
			facts.shortcutsGlobalHotkeys = false;
			facts.shortcutsGlobalActions = 1;   // state_report, the VP-V default
			facts.shortcutObserverRunning = observerRunning;
			return facts;
		}

		// One registered extension for the test; the registry is cleared before and after.
		struct Registered
		{
			std::string note;
			int pathwayReads = 0;
			ShortcutObserverStatus::Extension extension;

			explicit Registered(bool originalPathway)
				: extension([this, originalPathway] { ++pathwayReads; return originalPathway; }, note)
			{
				DialogExtension::ResetForTesting();
				DialogExtension::Register(&extension);
			}
			~Registered() { DialogExtension::ResetForTesting(); }
		};

		std::string Heading(const VpvGui::Snapshot& snapshot)
		{
			return VpvGui::FallbacksHeading(snapshot);
		}
	}

	TEST_CLASS(ShortcutObserverStatusTests)
	{
	public:
		TEST_METHOD(WantedAndFailedGivesOneEntryAndOneNotice)
		{
			Registered r(false);
			FallbackStatus::Notices notices;
			int noticesShown = 0;
			for (int tick = 0; tick < 3; ++tick)
			{
				DialogExtension::OnTimer(GlobalKeysWanted(false));
				Assert::AreEqual(std::string(ShortcutObserverStatus::kNote), r.note);
				const std::string heading = Heading(VpvGui::WithHostFallbacks(VpvGui::Snapshot(), r.note));
				const std::vector<std::string> ids = FallbackStatus::Ids(heading);
				Assert::AreEqual(size_t(1), ids.size());
				Assert::AreEqual(std::string(ShortcutObserverStatus::kId), ids[0]);
				Assert::AreEqual(std::string("Fallbacks engaged: global shortcuts (") + ShortcutObserverStatus::kNote + ")", heading);
				noticesShown += static_cast<int>(notices.Take(heading).size());
			}
			Assert::AreEqual(1, noticesShown);
			Assert::AreEqual(1, r.pathwayReads);   // read once: a pathway switch applies at the next start
		}

		TEST_METHOD(EntryIsAddedAfterTheRendererFallbacks)
		{
			Registered r(false);
			DialogExtension::OnTimer(GlobalKeysWanted(false));
			VpvGui::Snapshot snapshot;
			snapshot.rendererPresent = true;
			snapshot.vp.fallbacks.Set(true, L"Fallbacks engaged: render-ahead (off: no statistics)");
			const std::string heading = Heading(VpvGui::WithHostFallbacks(snapshot, r.note));
			const std::vector<std::string> ids = FallbackStatus::Ids(heading);
			Assert::AreEqual(size_t(2), ids.size());
			Assert::AreEqual(std::string("render-ahead"), ids[0]);
			Assert::AreEqual(std::string(ShortcutObserverStatus::kId), ids[1]);
		}

		TEST_METHOD(OriginalPathwayGivesNoEntry)
		{
			Registered r(true);
			FallbackStatus::Notices notices;
			DialogExtension::OnTimer(GlobalKeysWanted(false));
			Assert::IsTrue(r.note.empty());
			VpvGui::Snapshot snapshot;
			const VpvGui::Snapshot shown = VpvGui::WithHostFallbacks(snapshot, r.note);
			Assert::IsFalse(shown.vp.fallbacks.known);
			Assert::IsTrue(Heading(shown).empty());
			Assert::AreEqual(size_t(0), notices.Take(Heading(shown)).size());
		}

		TEST_METHOD(RunningObserverGivesNoEntry)
		{
			Registered r(false);
			DialogExtension::OnTimer(GlobalKeysWanted(true));
			Assert::IsTrue(r.note.empty());
			VpvGui::Snapshot snapshot;
			snapshot.rendererPresent = true;
			snapshot.vp.fallbacks.Set(true, L"Fallbacks engaged: none");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), Heading(VpvGui::WithHostFallbacks(snapshot, r.note)));
		}

		TEST_METHOD(EntryClearsWhenTheObserverRuns)
		{
			Registered r(false);
			DialogExtension::OnTimer(GlobalKeysWanted(false));
			Assert::IsFalse(r.note.empty());
			DialogExtension::OnTimer(GlobalKeysWanted(true));
			Assert::IsTrue(r.note.empty());
		}

		TEST_METHOD(BillsConfigOnlyObserverAndNoGlobalKeysGiveNoEntry)
		{
			// [shortcuts] foreground_only: Bill's config-only observer, log-only as Bill.
			DialogExtension::DialogFacts configOnly = GlobalKeysWanted(false);
			configOnly.shortcutsForegroundOnly = true;
			Assert::IsTrue(ShortcutObserverStatus::Note(configOnly, false).empty());
			// No global action and global_hotkeys off: nothing is wanted.
			DialogExtension::DialogFacts none = GlobalKeysWanted(false);
			none.shortcutsGlobalActions = 0;
			Assert::IsTrue(ShortcutObserverStatus::Note(none, false).empty());
			// global_hotkeys on alone wants the hook.
			none.shortcutsGlobalHotkeys = true;
			Assert::AreEqual(std::string(ShortcutObserverStatus::kNote), ShortcutObserverStatus::Note(none, false));
		}

		TEST_METHOD(OnTimerWithoutFactsStillReachesOnTimer)
		{
			// The facts-less OnTimer() keeps working for extensions that need no facts.
			Registered r(false);
			DialogExtension::OnTimer();
			Assert::IsTrue(r.note.empty());   // default facts: nothing wanted
		}
	};
}
