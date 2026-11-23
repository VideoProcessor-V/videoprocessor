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

#include <ConfigFile.h>
#include <VP-V/core/FallbackStatus.h>
#include <VP-V/gui/ShortcutObserverStatus.h>
#include <VP-V/gui/VpStatusAdapter.h>

#include <fstream>
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

		// A configuration file with `text`, read fresh (not from the cache) the way the dialog reads one.
		ConfigFile LoadText(const std::string& text)
		{
			char base[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, base);
			const std::string folder = std::string(base) + "vp-shortcut-observer-status";
			CreateDirectoryA(folder.c_str(), nullptr);
			const std::string path = folder + "\\settings.cfg";
			{
				std::ofstream file(path, std::ios::binary | std::ios::trunc);
				file << text;
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path, ConfigFile::ReadPolicy::Fresh));
			return config;
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
			Assert::AreEqual(3, r.reads);             // read every tick: the [shortcuts] keys are followed (the pathway: FileFactsReader, once)
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

		// The facts from a real configuration file, through the shared readers.
		TEST_METHOD(FactsFromAConfigurationFile)
		{
			using ShortcutObserverStatus::FromConfig;
			// global_actions absent in a VP-V pathway: the default applies, on Ctrl+Alt+D.
			ShortcutObserverStatus::Facts facts = FromConfig(LoadText("[shortcuts]\nshow_profiles: Ctrl+P\n"));
			Assert::IsTrue(facts.defaultApplies);
			Assert::IsFalse(facts.globalHotkeys);
			Assert::IsFalse(facts.foregroundOnly);
			Assert::IsTrue(facts.keyBound);
			Assert::AreEqual(static_cast<int>(FCONTROL | FALT), static_cast<int>(facts.modifiers));
			Assert::AreEqual(static_cast<int>('D'), static_cast<int>(facts.key));
			Assert::IsTrue(ShortcutObserverStatus::Decide(facts).wanted);
			// An explicit value, even empty, is the user's (the dialog's own observer serves it).
			Assert::IsFalse(FromConfig(LoadText("[shortcuts]\nglobal_actions:\n")).defaultApplies);
			Assert::IsFalse(FromConfig(LoadText("[shortcuts]\nglobal_actions: config_editor\n")).defaultApplies);
			Assert::IsFalse(FromConfig(LoadText("[shortcuts]\nglobal_actions: state_report\n")).defaultApplies,
				L"listed explicitly: the dialog's observer serves it");
			// The Original pathway: no default, the value read as the renderer reads it (NormalizeName).
			Assert::IsFalse(FromConfig(LoadText("[vpv]\npipeline_original_pathway: on\n")).defaultApplies);
			Assert::IsFalse(FromConfig(LoadText("[vpv]\npipeline_original_pathway: -on\n")).defaultApplies,
				L"SHOULD-C: NormalizeName reads -on as on");
			Assert::IsTrue(FromConfig(LoadText("[vpv]\npipeline_original_pathway: off\n")).defaultApplies);
			// The switches the default depends on; an unreadable value is off, as the dialog reads it.
			Assert::IsTrue(FromConfig(LoadText("[shortcuts]\nglobal_hotkeys: true\n")).globalHotkeys);
			Assert::IsFalse(FromConfig(LoadText("[shortcuts]\nglobal_hotkeys: maybe\n")).globalHotkeys);
			Assert::IsTrue(FromConfig(LoadText("[shortcuts]\nforeground_only: yes\n")).foregroundOnly);
			// The report key's chord.
			Assert::IsFalse(FromConfig(LoadText("[shortcuts]\nstate_report:\n")).keyBound, L"empty: no report key");
			facts = FromConfig(LoadText("[shortcuts]\nstate_report: Ctrl+Shift+F9\n"));
			Assert::IsTrue(facts.keyBound);
			Assert::AreEqual(static_cast<int>(FCONTROL | FSHIFT), static_cast<int>(facts.modifiers));
			Assert::AreEqual(static_cast<int>(VK_F9), static_cast<int>(facts.key));
			facts = FromConfig(LoadText("[shortcuts]\nstate_report: Hyper+Q\n"));
			Assert::IsTrue(facts.keyBound, L"does not parse: the dialog binds the compiled default");
			Assert::AreEqual(static_cast<int>(FCONTROL | FALT), static_cast<int>(facts.modifiers));
			Assert::AreEqual(static_cast<int>('D'), static_cast<int>(facts.key));
		}

		TEST_METHOD(PlanFollowsTheFacts)
		{
			const ShortcutObserverStatus::Plan plan = ShortcutObserverStatus::Decide(DefaultWanted());
			Assert::IsTrue(plan.wanted);
			Assert::AreEqual(static_cast<int>(FCONTROL | FALT), static_cast<int>(plan.modifiers));
			Assert::AreEqual(static_cast<int>('D'), static_cast<int>(plan.key));
			ShortcutObserverStatus::Facts unbound = DefaultWanted();
			unbound.keyBound = false;   // [shortcuts] state_report empty: no report key at all
			Assert::IsFalse(ShortcutObserverStatus::Decide(unbound).wanted);
			Assert::IsTrue(ShortcutObserverStatus::Decide(unbound) == ShortcutObserverStatus::Plan());
			ShortcutObserverStatus::Facts otherChord = DefaultWanted();
			otherChord.key = VK_F9;
			Assert::IsTrue(ShortcutObserverStatus::Decide(otherChord) != plan);
		}

		// The hook's key rules are ConfigurationLiveApply's: AltGr is not Ctrl+Alt, VP or Config in front
		// keeps the key, and the key is never taken (dispatch and pass).
		TEST_METHOD(TheHookKeyRulesAreTheSharedOnes)
		{
			const ShortcutObserverStatus::Plan plan = ShortcutObserverStatus::Decide(DefaultWanted());   // Ctrl+Alt+D
			// virtual key, control, leftAlt, rightAlt, shift
			Assert::IsTrue(ShortcutObserverStatus::Matches(plan, 'D', true, true, false, false));
			Assert::IsTrue(ShortcutObserverStatus::Matches(plan, 'D', true, true, true, false), L"both Alt keys held");
			Assert::IsFalse(ShortcutObserverStatus::Matches(plan, 'D', true, false, true, false), L"AltGr+D types a character");
			Assert::IsFalse(ShortcutObserverStatus::Matches(plan, 'D', true, true, false, true), L"Ctrl+Alt+Shift+D is another chord");
			Assert::IsFalse(ShortcutObserverStatus::Matches(plan, 'E', true, true, false, false));
			Assert::IsFalse(ShortcutObserverStatus::Matches(ShortcutObserverStatus::Plan(), 'D', true, true, false, false),
				L"nothing wanted: nothing matches");
			// vpForeground, configurationForeground
			Assert::IsTrue(ShortcutObserverStatus::Dispatches(false, false), L"another application in front: report");
			Assert::IsFalse(ShortcutObserverStatus::Dispatches(true, false), L"VP in front: its own keys handle it");
			Assert::IsFalse(ShortcutObserverStatus::Dispatches(false, true), L"Config in front: the key is Config's");
		}

		TEST_METHOD(APlanChangeRestartsTheHook)
		{
			Registered r(DefaultWanted());
			r.observer.startWorks = true;
			DialogExtension::OnTimer();
			Assert::AreEqual(1, r.observer.starts);
			DialogExtension::OnTimer();
			Assert::AreEqual(1, r.observer.starts, L"same plan: the hook keeps running");
			// An explicit global_actions value arrives (Config Apply): the list is the dialog's now.
			r.facts.defaultApplies = false;
			DialogExtension::OnTimer();
			Assert::IsFalse(r.observer.running, L"the explicit value wins within a tick");
			Assert::AreEqual(1, r.observer.starts);
			Assert::IsTrue(r.note.empty());
			// The key is removed again: the default is back.
			r.facts.defaultApplies = true;
			DialogExtension::OnTimer();
			Assert::IsTrue(r.observer.running);
			Assert::AreEqual(2, r.observer.starts);
			// A new chord for the report key: the hook restarts on it.
			r.facts.modifiers = static_cast<unsigned char>(FCONTROL | FSHIFT);
			r.facts.key = VK_F9;
			DialogExtension::OnTimer();
			Assert::AreEqual(3, r.observer.starts);
			Assert::AreEqual(static_cast<int>(FCONTROL | FSHIFT), static_cast<int>(r.observer.started.modifiers));
			Assert::AreEqual(static_cast<int>(VK_F9), static_cast<int>(r.observer.started.key));
			Assert::IsTrue(r.extension.PlanInForce() == r.observer.started);
			// An explicit empty value (none): stopped at once (a pathway switch waits for the next start: FileFactsReader).
			r.facts.defaultApplies = false;
			DialogExtension::OnTimer();
			Assert::IsFalse(r.observer.running);
			Assert::IsFalse(r.extension.PlanInForce().wanted);
			Assert::IsTrue(r.note.empty());
		}

		TEST_METHOD(AnUnreadableConfigurationKeepsThePlanInForce)
		{
			Registered r(DefaultWanted());
			r.readable = false;   // not readable yet: nothing is wanted, nothing starts
			DialogExtension::OnTimer();
			Assert::AreEqual(0, r.observer.starts);
			Assert::IsTrue(r.note.empty());
			r.readable = true;
			r.observer.startWorks = true;
			DialogExtension::OnTimer();
			Assert::AreEqual(1, r.observer.starts);
			// The file cannot be read for a moment (being replaced): the hook keeps the plan in force.
			r.readable = false;
			r.facts.defaultApplies = false;
			DialogExtension::OnTimer();
			Assert::IsTrue(r.observer.running);
			Assert::AreEqual(1, r.observer.starts);
			Assert::IsTrue(r.extension.PlanInForce().wanted);
		}

		// The executable's reader (FileFactsReader): the pathway in force is read once, from the first readable
		// file (a [vpv] change takes effect at the next start); the [shortcuts] keys follow the file.
		TEST_METHOD(ThePathwayInForceIsReadOnce)
		{
			std::string text = "[shortcuts]\nshow_profiles: Ctrl+P\n";   // a VP-V pathway, global_actions absent
			bool readable = false;
			ShortcutObserverStatus::FileFactsReader read([&](ConfigFile& config)
				{
					if (!readable)
						return false;
					config = LoadText(text);
					return true;
				});
			ShortcutObserverStatus::Facts facts;
			Assert::IsFalse(read(facts), L"not readable: no facts, and no pathway read yet");
			readable = true;
			Assert::IsTrue(read(facts));
			Assert::IsTrue(facts.defaultApplies);
			// A switch to the Original pathway is saved: it takes effect at the next start.
			text = "[vpv]\npipeline_original_pathway: on\n";
			Assert::IsTrue(read(facts));
			Assert::IsTrue(facts.defaultApplies, L"the pathway in force is still a VP-V pathway");
			// An explicit value applies at once.
			text = "[vpv]\npipeline_original_pathway: on\n[shortcuts]\nglobal_actions:\n";
			Assert::IsTrue(read(facts));
			Assert::IsFalse(facts.defaultApplies);
			// A process that started in the Original pathway gets no default hook before its next start.
			text = "[vpv]\npipeline_original_pathway: on\n";
			ShortcutObserverStatus::FileFactsReader original([&](ConfigFile& config)
				{
					config = LoadText(text);
					return true;
				});
			Assert::IsTrue(original(facts));
			Assert::IsFalse(facts.defaultApplies);
			text = "[vpv]\npipeline_original_pathway: off\n";
			Assert::IsTrue(original(facts));
			Assert::IsFalse(facts.defaultApplies, L"Original == Bill until the next start");
			// FromConfig: the two-argument form takes the pathway in force, the one-argument form the file's own.
			Assert::IsTrue(ShortcutObserverStatus::FromConfig(LoadText("[vpv]\npipeline_original_pathway: on\n"), false).defaultApplies);
			Assert::IsFalse(ShortcutObserverStatus::FromConfig(LoadText("[shortcuts]\nshow_profiles: Ctrl+P\n"), true).defaultApplies);
			Assert::IsFalse(ShortcutObserverStatus::FromConfig(LoadText("[vpv]\npipeline_original_pathway: on\n")).defaultApplies);
		}

		// GEM-041: a bare press of the plan's key (e.g. 'D' without Ctrl+Alt) must never latch keyDown,
		// while a matched chord dispatches once and ignores auto-repeat until key-up.
		TEST_METHOD(BareKeyDoesNotLatchAndMatchedChordAutoRepeatsOnce)
		{
			const ShortcutObserverStatus::Plan plan = ShortcutObserverStatus::Decide(DefaultWanted());   // Ctrl+Alt+D
			bool keyDown = false;
			// Bare 'D' key-down: must not dispatch and must not latch keyDown.
			Assert::IsFalse(ShortcutObserverStatus::HandleKeyEvent(plan, keyDown, 'D', true, false, false, false, false, false));
			Assert::IsFalse(keyDown, L"bare 'D' must not latch keyDown");
			// Ctrl+Alt+D while 'D' was never released (or right after): dispatches and latches keyDown.
			Assert::IsTrue(ShortcutObserverStatus::HandleKeyEvent(plan, keyDown, 'D', true, false, true, true, false, false));
			Assert::IsTrue(keyDown, L"matched chord latches keyDown");
			// Auto-repeat while held: suppressed.
			Assert::IsFalse(ShortcutObserverStatus::HandleKeyEvent(plan, keyDown, 'D', true, false, true, true, false, false));
			// Key-up clears the latch; next matched press dispatches again.
			Assert::IsFalse(ShortcutObserverStatus::HandleKeyEvent(plan, keyDown, 'D', false, true, true, true, false, false));
			Assert::IsFalse(keyDown);
			Assert::IsTrue(ShortcutObserverStatus::HandleKeyEvent(plan, keyDown, 'D', true, false, true, true, false, false));
		}

		// GEM-041: when the hook is wanted but failed to start, OnTimer retries with a back-off
		// every kRetryIntervalTicks ticks and clears the fallback note once Start succeeds.
		TEST_METHOD(FailedStartRetriesWithBackOffAndClearsNote)
		{
			Registered r(DefaultWanted());   // startWorks = false initially
			DialogExtension::OnTimer();      // tick 0: initial Start attempt fails
			Assert::AreEqual(1, r.observer.starts);
			Assert::AreEqual(std::string(ShortcutObserverStatus::kNote), r.note);
			for (int i = 1; i < ShortcutObserverStatus::Extension::kRetryIntervalTicks; ++i)
			{
				DialogExtension::OnTimer();
				Assert::AreEqual(1, r.observer.starts, L"no retry before back-off interval elapses");
			}
			r.observer.startWorks = true;
			DialogExtension::OnTimer();      // back-off tick: retries and succeeds
			Assert::AreEqual(2, r.observer.starts);
			Assert::IsTrue(r.observer.running);
			Assert::IsTrue(r.note.empty(), L"fallback note clears once retry succeeds");
		}
	};
}
