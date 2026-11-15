// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S-3f b (lead 17 MUST-1; owner 2026-11-15: the default lives in VP-V code): the report key's default
// global action (src/VP-V/gui/ShortcutObserverStatus.h), driven by the existing one-call
// DialogExtension::OnTimer() tick, and its "global shortcuts" Fallbacks entry added to the poll's
// Fallbacks line by VpvGui::WithHostFallbacks (VpStatusAdapter.h).
//   wanted + failed -> one entry, one notice; Original pathway -> no entry; running -> no entry.
// The hook itself is a fake here (the real low-level keyboard hook is GUI-only).
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
		// The VP-V default in force: global_actions absent in a VP-V pathway, global_hotkeys and
		// foreground_only off, the report key on Ctrl+Alt+D.
		ShortcutObserverStatus::Facts DefaultWanted()
		{
			ShortcutObserverStatus::Facts facts;
			facts.defaultApplies = true;
			facts.keyBound = true;
			facts.modifiers = static_cast<unsigned char>(FCONTROL | FALT);
			facts.key = 'D';
			return facts;
		}

		// The hook as the extension sees it: Start succeeds when `startWorks`; Running follows it.
		class FakeObserver : public ShortcutObserverStatus::IObserver
		{
		public:
			bool startWorks = false;
			bool running = false;
			int starts = 0;
			ShortcutObserverStatus::Plan started;

			bool Start(const ShortcutObserverStatus::Plan& plan) override
			{
				++starts;
				started = plan;
				running = startWorks;
				return running;
			}
			void Stop() override { running = false; }
			bool Running() const override { return running; }
		};

		// One registered extension for the test; the registry is cleared before and after. `facts` is
		// what the configuration says at the next tick; `readable` false = it cannot be read then.
		struct Registered
		{
			std::string note;
			int reads = 0;
			bool readable = true;
			ShortcutObserverStatus::Facts facts;
			FakeObserver observer;
			ShortcutObserverStatus::Extension extension;

			explicit Registered(const ShortcutObserverStatus::Facts& initial)
				: facts(initial),
				  extension([this](ShortcutObserverStatus::Facts& out) { ++reads; out = facts; return readable; },
					  observer, note)
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
			Registered r(DefaultWanted());   // the fake hook does not start
			FallbackStatus::Notices notices;
			int noticesShown = 0;
			for (int tick = 0; tick < 3; ++tick)
			{
				DialogExtension::OnTimer();
				Assert::AreEqual(std::string(ShortcutObserverStatus::kNote), r.note);
				const std::string heading = Heading(VpvGui::WithHostFallbacks(VpvGui::Snapshot(), r.note));
				const std::vector<std::string> ids = FallbackStatus::Ids(heading);
				Assert::AreEqual(size_t(1), ids.size());
				Assert::AreEqual(std::string(ShortcutObserverStatus::kId), ids[0]);
				Assert::AreEqual(std::string("Fallbacks engaged: global shortcuts (") + ShortcutObserverStatus::kNote + ")", heading);
				noticesShown += static_cast<int>(notices.Take(heading).size());
			}
			Assert::AreEqual(1, noticesShown);
			Assert::AreEqual(3, r.reads);             // read every tick: the file is followed, a switch applies at once
			Assert::AreEqual(1, r.observer.starts);   // started once: the plan did not change
		}

		TEST_METHOD(EntryIsAddedAfterTheRendererFallbacks)
		{
			Registered r(DefaultWanted());
			DialogExtension::OnTimer();
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
			ShortcutObserverStatus::Facts original = DefaultWanted();
			original.defaultApplies = false;   // GlobalActionsForConfig: no default in the Original pathway
			Registered r(original);
			FallbackStatus::Notices notices;
			DialogExtension::OnTimer();
			Assert::IsTrue(r.note.empty());
			Assert::AreEqual(0, r.observer.starts);   // no default global hook in the Original pathway
			VpvGui::Snapshot snapshot;
			const VpvGui::Snapshot shown = VpvGui::WithHostFallbacks(snapshot, r.note);
			Assert::IsFalse(shown.vp.fallbacks.known);
			Assert::IsTrue(Heading(shown).empty());
			Assert::AreEqual(size_t(0), notices.Take(Heading(shown)).size());
		}

		TEST_METHOD(RunningObserverGivesNoEntry)
		{
			Registered r(DefaultWanted());
			r.observer.startWorks = true;
			DialogExtension::OnTimer();
			Assert::IsTrue(r.observer.running);
			Assert::IsTrue(r.note.empty());
			VpvGui::Snapshot snapshot;
			snapshot.rendererPresent = true;
			snapshot.vp.fallbacks.Set(true, L"Fallbacks engaged: none");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), Heading(VpvGui::WithHostFallbacks(snapshot, r.note)));
		}

		TEST_METHOD(EntryClearsWhenTheObserverRuns)
		{
			Registered r(DefaultWanted());
			DialogExtension::OnTimer();
			Assert::IsFalse(r.note.empty());
			r.observer.running = true;   // the hook runs after all
			DialogExtension::OnTimer();
			Assert::IsTrue(r.note.empty());
		}

		TEST_METHOD(BillsConfigOnlyObserverAndNoGlobalKeysGiveNoEntry)
		{
			// [shortcuts] foreground_only: keys act only while VP has focus (Bill's config-only observer).
			ShortcutObserverStatus::Facts configOnly = DefaultWanted();
			configOnly.foregroundOnly = true;
			Assert::IsFalse(ShortcutObserverStatus::Decide(configOnly).wanted);
			Assert::IsTrue(ShortcutObserverStatus::Note(ShortcutObserverStatus::Decide(configOnly).wanted, false).empty());
			// An explicit global_actions value (here none): the list is the user's and the dialog's.
			ShortcutObserverStatus::Facts none = DefaultWanted();
			none.defaultApplies = false;
			Assert::IsTrue(ShortcutObserverStatus::Note(ShortcutObserverStatus::Decide(none).wanted, false).empty());
			// global_hotkeys on: the dialog's own observer makes every binding global, so the VP-V hook stands
			// down (that observer's state needs a seam payload: log-only, as at integ ab32bc33b).
			ShortcutObserverStatus::Facts hotkeys = DefaultWanted();
			hotkeys.globalHotkeys = true;
			Assert::IsFalse(ShortcutObserverStatus::Decide(hotkeys).wanted);
			Assert::IsTrue(ShortcutObserverStatus::Note(ShortcutObserverStatus::Decide(hotkeys).wanted, false).empty());
		}

		TEST_METHOD(OnTimerWithoutFactsStillReachesOnTimer)
		{
			// The seam's one-call OnTimer() is the extension's only input: it reads its facts itself.
			Registered r((ShortcutObserverStatus::Facts()));   // nothing wanted
			DialogExtension::OnTimer();
			Assert::AreEqual(1, r.reads);   // OnTimer() reached the extension (lead 18 SHOULD-B)
			Assert::IsTrue(r.note.empty());
		}
	};
}
