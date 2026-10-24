// VP-V section tests (settings agent, 2026-10-24): the tier data table and model, the delay
// budget display, and the VP-V page inside the editor (off screen like the rest of the suite).
// Registered from ConfigEditorWindowTests.cpp's main with one line: RunVpvConfigTests().

#include "ConfigEditorWindow.h"
#include <VP-V/config/VpvSection.h>
#include <VP-V/config/VpvTierTable.generated.h>

#include <ConfigFile.h>
#include <VP-V/core/DelayBudgetLadder.h>
#include <RendererProfileConfig.h>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QTemporaryDir>
#include <QWidget>

#include <cmath>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

// The suite's helpers (ConfigEditorWindowTests.cpp), handed in by RunVpvConfigTests.
namespace
{
	std::function<void(bool, const char*)> g_require;
	std::function<void(ConfigEditorWindow&)> g_save;
	void require(bool condition, const char* message) { g_require(condition, message); }
	void save(ConfigEditorWindow& window) { g_save(window); }
}

namespace
{
	template <typename T>
	T* control(QObject& root, const QString& name)
	{
		T* found = root.findChild<T*>(name);
		if (!found) throw std::runtime_error(("Missing control: " + name).toStdString());
		return found;
	}

	std::string S(const QString& text) { return text.toStdString(); }

	// A map-backed profile for the pure model tests.
	struct Profile
	{
		std::map<std::string, std::string> values;
		Vpv::Getter Get() const
		{
			return [this](const std::string& key)
			{
				const auto it = values.find(key);
				return it == values.end() ? std::string() : it->second;
			};
		}
		void Apply(const std::vector<std::pair<std::string, std::string>>& writes)
		{
			for (const auto& w : writes) values[w.first] = w.second;
		}
	};

	void ApplyGlobal(Profile& p, int g)
	{
		const Vpv::Table& t = Vpv::TierTable();
		for (const Vpv::Unit& u : t.units)
			if (u.Tiered()) p.Apply(Vpv::ApplyTier(t, u, Vpv::GlobalToUnit(t, u, g), p.Get()));
	}

	int UnitTier(const Profile& p, const char* unit)
	{
		const Vpv::Table& t = Vpv::TierTable();
		return Vpv::MatchTier(t, *t.FindUnit(unit), p.Get());
	}

	void testTableCoversEverySlot()
	{
		const Vpv::Table& t = Vpv::TierTable();
		require(t.tiers.size() == 4 && std::string(t.tiers[0]) == "Classic" && std::string(t.tiers[3]) == "Upper",
			"global tiers are Classic / Low / Mid / Upper");
		require(t.slotList.size() >= 30, "every slots.json slot is in the table");
		std::map<std::string, int> owners;
		for (const Vpv::Unit& u : t.units)
			for (const char* s : u.slotIds) ++owners[s];
		for (const Vpv::SlotInfo& s : t.slotList)
		{
			const bool control = s.order >= 900 && std::string(s.unit) != "drive";
			require(control || owners[s.id] == 1, (std::string("slot in exactly one VP-V unit: ") + s.id).c_str());
			require(control || s.fallback != nullptr, (std::string("slot has a fallback entry: ") + s.id).c_str());
		}
		for (const Vpv::Unit& u : t.units)
		{
			for (const Vpv::Preset& p : u.presets)
			{
				require(t.GlobalIndex(p.tier) >= 0, "unit tier is a global tier");
				for (const Vpv::KeyValue& kv : p.set)
					require(*t.Default(kv.key) != 0, (std::string("tier key has a default: ") + kv.key).c_str());
			}
			for (const Vpv::Toggle& tg : u.toggles)
				for (const Vpv::Preset& p : u.presets)
					for (const Vpv::KeyValue& kv : p.set)
						require(std::string(kv.key) != tg.key, "a toggle is never also a tier key");
		}
	}

	void testTierMatchingAndGlobal()
	{
		const Vpv::Table& t = Vpv::TierTable();
		Profile p;   // nothing set: VP's built-in defaults
		require(UnitTier(p, "motion") == 2, "defaults are motion Mid (NVOF + matcher, 4 ahead)");
		require(UnitTier(p, "repair") == 1, "defaults are repair Low (libplacebo deband + breathing reduction)");
		require(UnitTier(p, "scaling") < 0, "defaults are scaling Custom (re-base auto, SSIM blend off)");
		require(Vpv::MatchGlobal(t, p.Get()) < 0, "defaults are global Custom");

		ApplyGlobal(p, 3);
		require(Vpv::MatchGlobal(t, p.Get()) == 3, "global Upper reads back as Upper");
		require(UnitTier(p, "scaling") == 2 && UnitTier(p, "output") == 1, "units without Upper use their nearest lower tier");
		require(p.values["texture_restore"] == "statistical" && p.values["dejudder_est_split"] == "on", "Upper presets written");

		ApplyGlobal(p, 0);
		require(Vpv::MatchGlobal(t, p.Get()) == 0, "global Classic reads back as Classic");
		require(Vpv::Effective(t, p.Get(), "dejudder_strength") == "0" && p.values["optical_flow"] == "off", "Classic switches motion off");

		// Leaving Classic restores the default motion taste; a chosen taste is never overwritten.
		p.Apply(Vpv::ApplyTier(t, *t.FindUnit("motion"), 2, p.Get()));
		require(p.values["dejudder_strength"] == "0.25", "leaving Classic restores the default de-judder strength");
		p.values["dejudder_strength"] = "0.4";
		p.Apply(Vpv::ApplyTier(t, *t.FindUnit("motion"), 1, p.Get()));
		require(p.values["dejudder_strength"] == "0.4", "a chosen motion taste survives a tier change");

		// A changed tier key reads as Custom; values are compared as the parsers read them.
		p.values["optical_flow"] = "true";
		require(UnitTier(p, "motion") < 0, "a changed tier key reads as Custom");
		require(Vpv::SameValue("true", "on") && Vpv::SameValue("0.20", "0.2") && !Vpv::SameValue("0.2", "0.25"),
			"value comparison");
		require(Vpv::ManagedKeys(t).count("deband_vp") == 1 && Vpv::ManagedKeys(t).count("dejudder_strength") == 0,
			"managed keys include tier keys, never the taste keys");
	}

	void testDelayBudget()
	{
		const auto rates = DelayBudget::StandardRates();
		const double recommended = DelayBudget::BudgetForFrames(4, rates);
		require(std::abs(recommended - 267.0) < 1e-9, "4 frames at 23.976 on a 60 Hz display recommend 267 ms (display phase included)");
		for (int frames : { 4, 3, 2 })
		{
			const double budget = DelayBudget::BudgetForFrames(frames, rates);
			for (double fps : rates)
			{
				const DelayBudget::Plan plan = DelayBudget::Compute(DelayBudget::InputsFor(budget, fps));
				require(plan.queueFrames >= frames, "the budget gives at least its frames at every rate");
				require(std::abs(DelayBudget::TotalMs(plan) - budget) < 1e-6, "the total delay is the same at every rate");
				require(plan.paddingMs >= 0.0, "padding is never negative");
			}
		}
		const DelayBudget::Plan tight = DelayBudget::Compute(DelayBudget::InputsFor(100.0, rates.front()));
		require(!tight.feasible && std::string(DelayBudget::LevelName(tight)) == "too short",
			"a delay too short for 2 frames says so instead of pretending");
	}

	const char* kFixture =
		"[vprenderer]\nsdr_target_nits: 100\n"
		"[vprenderer.Cinema]\nquality: high\n"
		"[queue]\ntarget_latency_ms: 250\n";

	QString WriteFixture(QTemporaryDir& dir, const char* text = kFixture)
	{
		const QString path = dir.filePath("VideoProcessor.cfg");
		QFile file(path);
		require(file.open(QIODevice::WriteOnly), "Cannot create the VP-V fixture");
		file.write(text);
		return path;
	}

	// Config names a literal or first profile itself (e.g. profile_1): the first renderer profile.
	std::string DefaultProfile(const QString& path)
	{
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly)) return {};
		for (const QByteArray& line : file.readAll().split('\n'))
		{
			const QString text = QString::fromUtf8(line).trimmed();
			if (text.startsWith(QStringLiteral("[vprenderer.")) && text.endsWith(QLatin1Char(']')))
				return text.mid(1, text.size() - 2).toLower().toStdString();
		}
		return "vprenderer";
	}

	std::string FileText(const QString& path)
	{
		QFile file(path);
		return file.open(QIODevice::ReadOnly) ? file.readAll().toStdString() : std::string();
	}

	void requireVpReads(const QString& path)
	{
		ConfigFile config;
		require(config.Load(path.toStdString()), "saved config reloads");
		RendererProfileConfig::Model model;
		std::string error;
		require(RendererProfileConfig::Read(config, model, error), ("VP rejects the VP-V config: " + error).c_str());
	}

	void testPageGlobalTierSavesKeys()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			control<QSlider>(window, Vpv::ObjectName("global_tier"))->setValue(3);
			QCoreApplication::processEvents();
			require(control<QLabel>(window, Vpv::ObjectName("global_tier.name"))->text() == QStringLiteral("Upper"),
				"global tier shows Upper");
			require(control<QSlider>(window, Vpv::ObjectName("scaling.tier"))->value() == 2, "scaling follows to Mid");
			save(window);
		}
		ConfigFile config;
		require(config.Load(path.toStdString()), "reload");
		const std::string main = DefaultProfile(path);
		// Only keys that differ from VP's built-in value are written (optical_flow is on by default).
		for (const auto& kv : std::map<std::string, std::string>{ { "optical_flow", "on" }, { "texture_restore", "statistical" },
			{ "deband_vp", "on" }, { "source_resolution", "auto" }, { "adaptive_downscale", "on" }, { "temporal_dither", "sigma_delta" },
			{ "dejudder_est_split", "on" }, { "adaptive_w_base", "0.2" } })
		{
			std::string v;
			if (!config.TryGetString(main, kv.first, v)) v = Vpv::TierTable().Default(kv.first);
			require(Vpv::SameValue(v, kv.second), ("tier key effective after save: " + kv.first + " = " + v + "\n" + FileText(path)).c_str());
		}
		std::string written;
		require(!config.TryGetString(main, "optical_flow", written), "a key already at VP's default is not written");
		requireVpReads(path);
		{
			ConfigEditorWindow window(path, 0, true);
			require(control<QLabel>(window, Vpv::ObjectName("global_tier.name"))->text() == QStringLiteral("Upper"),
				"the tier reads back from the keys after reopening");
			// A named profile edits its own section and inherits the rest.
			auto* profile = control<QComboBox>(window, Vpv::ObjectName("profile"));
			profile->setCurrentIndex(profile->findData(QStringLiteral("vprenderer.Cinema")));
			QCoreApplication::processEvents();
			require(control<QLabel>(window, Vpv::ObjectName("repair.tier.name"))->text().startsWith(QStringLiteral("Upper")),
				"a named profile inherits the default profile's tier");
			control<QSlider>(window, Vpv::ObjectName("repair.tier"))->setValue(0);
			save(window);
		}
		std::string v;
		require(config.Load(path.toStdString()) && config.TryGetString("vprenderer.cinema", "deband_vp", v) && v == "off",
			"a named profile's tier is written to that profile");
		require(config.TryGetString(main, "deband_vp", v) && v == "on", "the default profile is unchanged");
	}

	void testManagedControlsAndOriginalPathway()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* upstream = control<QWidget>(window, QStringLiteral("config.vprenderer.deband_vp"));
			require(!upstream->isEnabled() && upstream->toolTip().startsWith(QStringLiteral("Managed by VP-V")),
				"an upstream control VP-V manages is greyed with 'Managed by VP-V'");
			auto* taste = window.findChild<QWidget*>(QStringLiteral("config.vprenderer.dejudder_strength"));
			require(!taste || !taste->property("vpvManaged").toBool(), "the motion taste fields are never greyed");
			auto* pathway = control<QComboBox>(window, Vpv::ObjectName("pathway"));
			pathway->setCurrentIndex(1);
			QCoreApplication::processEvents();
			require(upstream->isEnabled(), "Original pathway gives the upstream control back");
			require(!control<QWidget>(window, Vpv::ObjectName("content"))->isEnabled(), "Original pathway disables VP-V");
			save(window);
		}
		std::string v;
		ConfigFile config;
		require(config.Load(path.toStdString()) && config.TryGetString("vpv", "pipeline_original_pathway", v) && v == "on",
			"the pathway is stored in the namespaced [vpv] section");
		requireVpReads(path);
		{
			ConfigEditorWindow window(path, 0, true);
			require(control<QWidget>(window, QStringLiteral("config.vprenderer.deband_vp"))->isEnabled(),
				"reopened in Original pathway: upstream controls behave as upstream");
			control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(0);
			save(window);
		}
		ConfigFile reloaded;
		require(reloaded.Load(path.toStdString()) && reloaded.TryGetString("vpv", "pipeline_original_pathway", v) && v == "off",
			"back to VP-V writes 'off' explicitly, so a [vprenderer] value cannot win (skeleton audit item 6)");
	}

	void testUnbuiltSlotsAndToggles()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		ConfigEditorWindow window(path, 0, true);
		auto* planned = control<QCheckBox>(window, Vpv::ObjectName("repair.planned.encoding_repair"));
		require(!planned->isEnabled() && planned->text().contains(QStringLiteral("not yet implemented")),
			"an unbuilt slot's setting is shown disabled as not yet implemented");
		require(control<QLabel>(window, Vpv::ObjectName("scaling.tier.planned"))->text().contains(QStringLiteral("Upper: not yet implemented")),
			"a unit's missing tier is listed as not yet implemented");
		require(control<QLabel>(window, Vpv::ObjectName("repair.slots"))->text().contains(QStringLiteral("not yet implemented")),
			"placeholder slots are listed as not yet implemented");
		require(control<QLabel>(window, Vpv::ObjectName("fallbacks"))->text().startsWith(QStringLiteral("Fallbacks engaged:")),
			"the Fallbacks engaged heading is shown");
		const QString delay = control<QLabel>(window, Vpv::ObjectName("input.delay"))->text();
		require(delay.contains(QStringLiteral("Recommended: 267 ms")) && delay.contains(QStringLiteral("Target delay: <b>250 ms</b>")),
			"the delay card states the recommended value and the chosen one");
		auto* guard = control<QComboBox>(window, Vpv::ObjectName("repair.deband_vp_grain_guard"));
		const int before = control<QSlider>(window, Vpv::ObjectName("repair.tier"))->value();
		guard->setCurrentIndex(guard->findData(QStringLiteral("veto")));
		QCoreApplication::processEvents();
		require(control<QSlider>(window, Vpv::ObjectName("repair.tier"))->value() == before, "a toggle never moves the tier");
		save(window);
		ConfigFile config;
		std::string v;
		require(config.Load(path.toStdString()) && config.TryGetString(DefaultProfile(path), "deband_vp_grain_guard", v) && v == "veto",
			"a toggle writes its key");
	}
}

namespace
{
	void testExpertPipelineKeys()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* order = control<QLineEdit>(window, Vpv::ObjectName("expert.pipeline_order"));
			auto* status = control<QLabel>(window, Vpv::ObjectName("expert.pipeline_order.status"));
			order->setText(QStringLiteral("output.dither, input.capture"));
			QMetaObject::invokeMethod(order, "editingFinished", Qt::DirectConnection);
			require(status->text().startsWith(QStringLiteral("Not saved:")),
				("an order the renderer would reject shows its reason: " + status->text()).toStdString().c_str());
			order->setText(QStringLiteral("repair.encoding, scaling.rebase"));
			QMetaObject::invokeMethod(order, "editingFinished", Qt::DirectConnection);
			require(status->text() == QStringLiteral("Accepted."), ("the repair-first order is accepted: " + status->text()).toStdString().c_str());
			auto* off = control<QLineEdit>(window, Vpv::ObjectName("expert.pipeline_slots_off"));
			off->setText(QStringLiteral("repair.texture_restore, colour.appearance"));
			QMetaObject::invokeMethod(off, "editingFinished", Qt::DirectConnection);
			auto* lut = control<QComboBox>(window, Vpv::ObjectName("expert.calibration_lut_stage"));
			lut->setCurrentIndex(1);
			save(window);
		}
		ConfigFile config;
		std::string v;
		require(config.Load(path.toStdString()) && config.TryGetString("vpv", "pipeline_order", v) &&
			v == "repair.encoding, scaling.rebase", "the accepted order is stored in [vpv]");
		require(config.TryGetString("vpv", "pipeline_slots_off", v) && v == "repair.texture_restore, colour.appearance",
			"the stages switched off are stored in [vpv]");
		require(config.TryGetString("vpv", "calibration_lut_stage", v) && v == "vp", "the LUT stage is stored in [vpv]");
		requireVpReads(path);
	}
}

int RunVpvConfigTests(const std::function<int(const char*, const std::function<void()>&)>& run,
	const std::function<void(bool, const char*)>& requireHelper, const std::function<void(ConfigEditorWindow&)>& saveHelper)
{
	g_require = requireHelper;
	g_save = saveHelper;
	int failures = 0;
	failures += run("VP-V: tier table covers every slot", testTableCoversEverySlot);
	failures += run("VP-V: tier matching and global tier", testTierMatchingAndGlobal);
	failures += run("VP-V: delay budget recommended and constant", testDelayBudget);
	failures += run("VP-V: global tier saves keys and reads back", testPageGlobalTierSavesKeys);
	failures += run("VP-V: managed controls and Original pathway", testManagedControlsAndOriginalPathway);
	failures += run("VP-V: unbuilt slots and toggles", testUnbuiltSlotsAndToggles);
	failures += run("VP-V: expert pipeline keys validated and stored", testExpertPipelineKeys);
	return failures;
}
