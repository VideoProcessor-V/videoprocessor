// VP-V section tests (settings agent, 2026-10-24): the tier data table and model, the delay
// budget display, and the VP-V page inside the editor (off screen like the rest of the suite).
// Registered from ConfigEditorWindowTests.cpp's main with one line: RunVpvConfigTests().

#include "ConfigEditorWindow.h"
#include <VP-V/config/VpvSection.h>
#include <VP-V/config/VpvTierTable.generated.h>
#include <VP-V/config/VpvOrganisation.h>
#include <VP-V/config/VpvSetBy.h>

#include <ConfigFile.h>
#include <VP-V/core/DelayBudgetLadder.h>
#include <RendererProfileConfig.h>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QTabWidget>
#include <QStackedWidget>
#include <QScrollBar>
#include <QScrollArea>
#include <QRegularExpression>
#include <QFormLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QToolButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QWidget>

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

// The suite's helpers (ConfigEditorWindowTests.cpp), handed in by RunVpvConfigTests.
namespace
{
	std::function<void(bool, const char*)> g_require;
	std::function<void(ConfigEditorWindow&)> g_save;
	void require(bool condition, const char* message) { g_require(condition, message); }
	bool RowShown(QWidget* w);   // below (organisation tests)
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
		// Logic audit 2026-10-24 (HIGH 1): the shipped values are a tier of every unit.
		require(UnitTier(p, "scaling") == 1, "defaults are scaling Low (re-base auto, no SSIM blend)");
		require(UnitTier(p, "output") == 1, "defaults are output Low (temporal dither as shipped)");
		require(Vpv::MatchGlobal(t, p.Get()) < 0, "defaults are global Custom (Motion Mid, the others Low)");

		ApplyGlobal(p, 3);
		require(Vpv::MatchGlobal(t, p.Get()) == 3, "global Upper reads back as Upper");
		require(UnitTier(p, "scaling") == 2 && UnitTier(p, "output") == 2, "units without Upper use their nearest lower tier");
		require(p.values["texture_restore"] == "statistical" && p.values["dejudder_est_split"] == "on", "Upper presets written");

		ApplyGlobal(p, 0);
		require(Vpv::MatchGlobal(t, p.Get()) == 0, "global Classic reads back as Classic");
		require(p.values["optical_flow"] == "off" && p.values["dejudder_est_qpel"] == "off", "Classic is the basic matcher");
		require(p.values.find("dejudder_strength") == p.values.end(), "no tier writes the de-judder amount (audit HIGH 2)");

		// A chosen taste is never overwritten by a tier change.
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
			auto* upstream = control<QWidget>(window, QStringLiteral("config.vprenderer.adaptive_upscale"));
			require(!upstream->isEnabled() && upstream->toolTip().startsWith(QStringLiteral("Managed by VP-V")),
				"an upstream control VP-V manages is greyed with 'Managed by VP-V'");
			auto* taste = window.findChild<QWidget*>(QStringLiteral("config.vprenderer.dejudder_strength"));
			require(!taste || !taste->property("vpvManaged").toBool(), "the motion taste fields are never greyed");
			auto* pathway = control<QComboBox>(window, Vpv::ObjectName("pathway"));
			pathway->setCurrentIndex(1);
			QCoreApplication::processEvents();
			// Organisation pass 2: the Original pathway switches every VP addition off, so the whole VP-V
			// page (tiers, switches and the VP rows it hosts) is disabled; Bill's controls stay live.
			require(!control<QWidget>(window, Vpv::ObjectName("content"))->isEnabled() &&
				!control<QWidget>(window, QStringLiteral("config.vprenderer.texture_restore_cap"))->isEnabled(),
				"Original pathway disables the VP-V page and the VP rows it hosts");
			require(control<QWidget>(window, Vpv::ObjectName("original_note"))->isVisibleTo(control<QWidget>(window, Vpv::ObjectName("section"))),
				"Original pathway says why the page is disabled");
			require(control<QWidget>(window, QStringLiteral("config.vprenderer.deband_strength"))->isEnabled(),
				"Bill's own controls stay live in the Original pathway");
			require(!RowShown(upstream), "a VP-V switch's duplicate row stays hidden in the Original pathway");
			save(window);
		}
		std::string v;
		ConfigFile config;
		require(config.Load(path.toStdString()) && config.TryGetString("vpv", "pipeline_original_pathway", v) && v == "on",
			"the pathway is stored in the namespaced [vpv] section");
		requireVpReads(path);
		{
			ConfigEditorWindow window(path, 0, true);
			require(!control<QWidget>(window, Vpv::ObjectName("content"))->isEnabled() &&
				control<QWidget>(window, QStringLiteral("config.vprenderer.deband_strength"))->isEnabled(),
				"reopened in Original pathway: the VP-V page disabled, Bill's controls live");
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
		require(control<QLabel>(window, Vpv::ObjectName("scaling.tier.planned"))->text().contains(QStringLiteral("Upper is planned")),
			"a unit's missing tier is listed as planned");
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
			auto* merged = control<QCheckBox>(window, Vpv::ObjectName("expert.output_segments_merged"));
			require(!merged->isChecked(), "the merged output chain is off by default");
			require(merged->toolTip().contains(QStringLiteral("off until its GPU check passes")), "the merged output chain says why it is off");
			merged->setChecked(true);
			control<QComboBox>(window, Vpv::ObjectName("expert.entrance_gpu_drain"))->setCurrentIndex(2);
			auto* analysis = control<QCheckBox>(window, Vpv::ObjectName("expert.frame_analysis_store"));
			require(!analysis->isChecked(), "the frame analysis store is off by default");
			analysis->setChecked(true);
			auto* hdr = control<QComboBox>(window, Vpv::ObjectName("expert.hdr_inference"));
			require(hdr->currentIndex() == 0, "hdr_inference shows vp by default");
			hdr->setCurrentIndex(1);
			save(window);
		}
		ConfigFile config;
		std::string v;
		require(config.Load(path.toStdString()) && config.TryGetString("vpv", "pipeline_order", v) &&
			v == "repair.encoding, scaling.rebase", "the accepted order is stored in [vpv]");
		require(config.TryGetString("vpv", "pipeline_slots_off", v) && v == "repair.texture_restore, colour.appearance",
			"the stages switched off are stored in [vpv]");
		require(config.TryGetString("vpv", "calibration_lut_stage", v) && v == "vp", "the LUT stage is stored in [vpv]");
		require(config.TryGetString("vpv", "output_segments_merged", v) && v == "on", "the merged output chain is stored in [vpv]");
		require(config.TryGetString("vpv", "entrance_gpu_drain", v) && v == "finish", "the entrance GPU drain is stored in [vpv]");
		require(config.TryGetString("vpv", "frame_analysis_store", v) && v == "on", "the frame analysis store is stored in [vpv]");
		require(config.TryGetString("vpv", "hdr_inference", v) && v == "original", "the HDR inference choice is stored in [vpv]");
		requireVpReads(path);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* pathway = control<QComboBox>(window, Vpv::ObjectName("pathway"));
			pathway->setCurrentIndex(1);
			QCoreApplication::processEvents();
			require(!control<QCheckBox>(window, Vpv::ObjectName("expert.output_segments_merged"))->isEnabled(),
				"the merged output chain is disabled in the Original pathway");
		}
	}
}

namespace
{
	// Coverage audit 2026-10-24 (docs/settings/coverage-audit.md): VP's own per-profile keys that
	// had a control only on the Rendering page are now VP-V toggles (the upstream control greys as
	// managed), each unit names where the rest of its settings are, the guide's retirement
	// candidates are listed and marked on their controls (never removed), and the LUT stage is no
	// longer shown as "not yet implemented" (the expert tab sets it).
	void testCoverageAuditAdditions()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			for (const char* name : { "input.source_range", "colour.dv_trims", "input.intent_content", "motion.interpolation_fallback",
				"motion.intent_motion", "repair.low_bitrate_handling", "scaling.source_resolution_text_guard",
				"scaling.source_resolution_mixed_guard", "scaling.adaptive_grain_map", "scaling.ssim_bitrate_aware",
				"scaling.ssim_residual_stabilise", "scaling.cnn_residual_stabilise", "scaling.adaptive_upscale",
				"colour.fbo_float32", "colour.ambient_adapt", "output.occlusion_mask" })
			{
				auto* combo = control<QComboBox>(window, Vpv::ObjectName(QString::fromLatin1(name)));
				require(combo->currentIndex() == 0 && combo->currentText().startsWith(QStringLiteral("Default: ")),
					(std::string("a new toggle shows Not set with its default: ") + name).c_str());
			}
			auto* upstream = control<QWidget>(window, QStringLiteral("config.vprenderer.adaptive_upscale"));
			require(!upstream->isEnabled() && upstream->toolTip().startsWith(QStringLiteral("Managed by VP-V")),
				"the upstream control of a new toggle is managed by VP-V");
			for (const char* unit : { "input", "motion", "repair", "scaling", "colour", "drive", "output" })
				require(!control<QLabel>(window, Vpv::ObjectName(QString::fromLatin1(unit) + QStringLiteral(".more")))->text().isEmpty(),
					(std::string("each unit names where the rest of its settings are: ") + unit).c_str());
			require(!window.findChild<QCheckBox*>(Vpv::ObjectName("output.planned.calibration_lut_stage")),
				"the LUT stage is not listed as not yet implemented (the expert tab sets it)");
			// The candidates are listed per group in plain words (no key names, no docs paths), each with its page.
			QString list;
			for (const char* group : { "A", "B", "C", "D" })
				list += control<QLabel>(window, Vpv::ObjectName(QStringLiteral("retirement.group.") + QString::fromLatin1(group) + QStringLiteral(".list")))->text();
			require(list.contains(QStringLiteral("VP-V &gt; Proposed retirement")) && list.contains(QStringLiteral("General page")) &&
				list.contains(QStringLiteral("Queue page")),
				"the Proposed retirement tab lists the candidates with their pages");
			require(!list.contains(QStringLiteral("no control found")), "every retirement candidate's control is found");
			require(control<QWidget>(window, QStringLiteral("config.vprenderer.pulldown_fixes"))->toolTip().contains(QStringLiteral("Proposed retirement")) &&
				control<QWidget>(window, QStringLiteral("config.general.magewell_recover_faults"))->toolTip().contains(QStringLiteral("Proposed retirement")),
				"retirement candidates are marked on their upstream controls");
			require(control<QComboBox>(window, Vpv::ObjectName("repair.detail_pulse_fast"))->accessibleName().contains(QStringLiteral("(proposed retirement)")),
				"a VP-V toggle that is a retirement candidate says so");
			auto* upscale = control<QComboBox>(window, Vpv::ObjectName("scaling.adaptive_upscale"));
			upscale->setCurrentIndex(upscale->findData(QStringLiteral("on")));
			QCoreApplication::processEvents();
			save(window);
		}
		ConfigFile config;
		std::string v;
		require(config.Load(path.toStdString()) && config.TryGetString(DefaultProfile(path), "adaptive_upscale", v) && v == "on",
			"a new toggle writes its key");
		requireVpReads(path);
	}
}

namespace
{
	// Is the form row holding w shown (its row's field not hidden by setRowVisible)?
	bool RowShown(QWidget* w)
	{
		for (QWidget* f = w; f && f->parentWidget(); f = f->parentWidget())
			if (auto* form = qobject_cast<QFormLayout*>(f->parentWidget()->layout()))
			{
				int row = -1;
				QFormLayout::ItemRole role;
				form->getWidgetPosition(f, &row, &role);
				if (row >= 0) return form->isRowVisible(row);
			}
		return !w->isHidden();
	}

}

namespace
{
	// Organisation pass (2026-10-24, VpvOrganisation.h, docs/settings/organisation.md): VP's own
	// Rendering-page rows and sections live in labelled cards on the VP-V unit tabs (moved, never
	// copied), the renderer-profile retirement candidates on the Proposed retirement tab, the rest
	// of VP's in a collapsed "All raw settings (expert)" group under Bill's sections; the VP-V
	// profile picker follows the Rendering page's list; a VP-V write reloads the moved rows.
	void testOrganisation()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[vprenderer]\nquality: high\ntexture_restore_cap: 1.5\n"
			"[vprenderer.Cinema]\ntexture_restore_cap: 2\n"
			"[queue]\ntarget_latency_ms: 250\n");
		ConfigEditorWindow window(path, 0, true);
		auto* section = control<QWidget>(window, Vpv::ObjectName("section"));
		const QString missing = section->property("vpvMissing").toString();
		require(missing.isEmpty(), ("every organisation item was found: " + missing).toStdString().c_str());
		for (const Vpv::Card& card : Vpv::Organisation())
		{
			auto* body = control<QWidget>(window, Vpv::ObjectName(QString::fromLatin1(card.unit) + QStringLiteral(".card.") +
				QString::fromLatin1(card.id) + QStringLiteral(".body")));
			for (const Vpv::CardItem& item : card.items)
			{
				QWidget* w = nullptr;
				if (item.kind == Vpv::ItemKind::Row)
					w = window.findChild<QWidget*>(QStringLiteral("config.vprenderer.") + QString::fromLatin1(item.id));
				else if (item.kind == Vpv::ItemKind::Section)
					w = window.findChild<QWidget*>(QStringLiteral("rendererSection.") + QString::fromLatin1(item.id));
				else if (item.kind == Vpv::ItemKind::Toggle && !Vpv::TierTable().FindRetirement(item.id, "vprenderer"))
					w = window.findChild<QWidget*>(Vpv::ObjectName(QString::fromLatin1(card.unit) + QLatin1Char('.') + QString::fromLatin1(item.id)));
				else
					continue;
				require(w && body->isAncestorOf(w), (std::string("in its card: ") + card.unit + "/" + card.id + " " + item.id).c_str());
			}
		}
		// Bill's sections stay on the Rendering page; the rest of VP's is in the raw group there.
		auto* raw = control<QWidget>(window, QStringLiteral("rendererSection.allRaw.content"));
		auto* tone = control<QWidget>(window, QStringLiteral("rendererSection.toneMapping"));
		require(!section->isAncestorOf(tone) && !raw->isAncestorOf(tone), "Bill's tone mapping stays in place on the Rendering page");
		require(raw->isAncestorOf(control<QWidget>(window, QStringLiteral("config.vprenderer.capture_path"))),
			"the managed duplicates are in All raw settings");
		require(!control<QToolButton>(window, QStringLiteral("rendererSection.allRaw"))->isChecked(), "All raw settings starts collapsed");
		// Retirement candidates (renderer profile): only on the Proposed retirement tab, except the
		// ones a tier or toggle manages (hidden there) and the diagnostics pair kept with the estimator.
		auto* retirement = control<QWidget>(window, Vpv::ObjectName("retirement"));
		const std::set<std::string> exceptions = { "motion_ahead_frames", "dither_vectors", "detail_pulse_fast",
			"dejudder_verify", "dejudder_verify_share" };
		for (const Vpv::Retirement& r : Vpv::TierTable().retirements)
		{
			if (std::string(r.section) != "vprenderer" || exceptions.count(r.key)) continue;
			auto* w = window.findChild<QWidget*>(QStringLiteral("config.vprenderer.") + QString::fromLatin1(r.key));
			require(w && retirement->isAncestorOf(w), (std::string("a retirement candidate on the retirement tab: ") + r.key).c_str());
		}
		// A managed row on the VP-V page is hidden (the VP-V control above is live); Original shows it.
		// One live control per key: a VP-V toggle's upstream rows are hidden wherever they are.
		auto* debandRow = control<QWidget>(window, QStringLiteral("config.vprenderer.low_bitrate_handling"));
		require(!RowShown(debandRow), "a managed row is hidden while the VP-V pathway is on");
		// The picker follows the Rendering list, and the moved rows edit the picked profile.
		auto* list = control<QListWidget>(window, QStringLiteral("config.vprenderer.profiles"));
		auto* picker = control<QComboBox>(window, Vpv::ObjectName("profile"));
		require(picker->count() == list->count(), "the picker lists the Rendering profiles");
		picker->setCurrentIndex(1);
		QCoreApplication::processEvents();
		require(list->currentRow() == 1, "the Rendering list follows the VP-V picker");
		require(control<QLineEdit>(window, QStringLiteral("config.vprenderer.texture_restore_cap"))->text() == QStringLiteral("2"),
			"a moved row shows the picked profile's value");
		list->setCurrentRow(0);
		QCoreApplication::processEvents();
		require(picker->currentIndex() == 0, "the VP-V picker follows the Rendering list");
		// A tier write reloads the upstream rows (no stale values next to the tier).
		control<QSlider>(window, Vpv::ObjectName("repair.tier"))->setValue(3);
		QCoreApplication::processEvents();
		require(control<QComboBox>(window, QStringLiteral("config.vprenderer.deband_vp"))->currentData().toString() == QStringLiteral("on"),
			"the Rendering control shows the value the tier wrote");
		// The input-policy notes.
		for (const char* note : { "note.input_policy.general", "note.input_policy.vprenderer.input_processing", "note.input_policy.directshow" })
			control<QLabel>(window, Vpv::ObjectName(QString::fromLatin1(note)));
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
		QCoreApplication::processEvents();
		require(!RowShown(debandRow), "the duplicate row stays hidden in the Original pathway (one control per key)");
		// Bill's own Rendering keys are never on the VP-V page (checked against billslack2 v1.4.02-beta's
		// RendererProfileConfig.h, 2026-10-24: none of the keys VP-V hosts is Bill's).
		for (const char* key : { "quality", "deband_strength", "dithering", "display_bit_depth", "tone_mapping", "gamut_mapping",
			"peak_detection", "contrast_recovery", "saturation", "sdr_target_nits", "sdr_black_nits" })
		{
			auto* w = window.findChild<QWidget*>(QStringLiteral("config.vprenderer.") + QString::fromLatin1(key));
			require(w && !section->isAncestorOf(w), (std::string("Bill's key stays on the Rendering page: ") + key).c_str());
		}
		save(window);
		requireVpReads(path);
	}
}

namespace
{
	// Organisation pass 2 (2026-10-24): masters and the settings they set; one control per unit; the
	// VP-V page fits the default window; the Dynamic Drive Advanced LED card under its two masters.
	void testMastersAndFit()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[vprenderer]\nquality: high\n"
			"[vprenderer.Cinema]\nquality: high\n"
			"[queue]\ntarget_latency_ms: 250\n"
			"[led_drive.Default]\nenabled: true\ncommand_source: compensate\n"
			"[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
		ConfigEditorWindow window(path, 0, true);
		// Tier keys: in Advanced > Set by the tier, greyed until Custom; Custom unlocks; unticking puts the tier back.
		auto* debandVp = control<QComboBox>(window, QStringLiteral("config.vprenderer.deband_vp"));
		auto* custom = control<QCheckBox>(window, Vpv::ObjectName("repair.card.tier_keys.custom"));
		require(!custom->isChecked() && !debandVp->isEnabled(), "a tier key is greyed, set by the tier");
		require(control<QLabel>(window, Vpv::ObjectName("repair.card.tier_keys.lock"))->text().startsWith(QStringLiteral("Set by the Repair tier")),
			"the lock says which master sets it");
		custom->setChecked(true);
		QCoreApplication::processEvents();
		require(debandVp->isEnabled(), "Custom unlocks the tier's keys");
		debandVp->setCurrentIndex(debandVp->findData(QStringLiteral("on")));
		QCoreApplication::processEvents();
		auto* tierSlider = control<QSlider>(window, Vpv::ObjectName("repair.tier"));
		const QString before = control<QLabel>(window, Vpv::ObjectName("repair.tier.name"))->text();
		custom->setChecked(false);
		QCoreApplication::processEvents();
		require(!debandVp->isEnabled() && control<QLabel>(window, Vpv::ObjectName("repair.tier.name"))->text() != QStringLiteral("Custom"),
			("unticking Custom puts the tier back: " + before.toStdString()).c_str());
		(void)tierSlider;
		// A feature's tuning is greyed while the feature is off.
		auto* knee = control<QWidget>(window, QStringLiteral("config.vprenderer.sdr_knee_start"));
		require(!knee->isEnabled(), "the SDR knee values are greyed while SDR tone shape is off");
		auto* shape = control<QComboBox>(window, Vpv::ObjectName("colour.sdr_tone_shape"));
		shape->setCurrentIndex(shape->findData(QStringLiteral("knee")));
		QCoreApplication::processEvents();
		require(knee->isEnabled(), "turning SDR tone shape on frees its values");
		// The motion slider's fields are set by it (greyed) until Custom; at the Classic tier the amount is greyed.
		auto* strength = control<QWidget>(window, QStringLiteral("config.vprenderer.dejudder_strength"));
		require(!strength->isEnabled() || control<QCheckBox>(window, Vpv::ObjectName("motion.card.taste_keys.custom"))->isChecked(),
			"the de-judder fields are set by the de-judder amount");
		// The tier never sets the taste (audit HIGH 2): Classic and back leave the amount alone and enabled.
		control<QSlider>(window, Vpv::ObjectName("motion.tier"))->setValue(0);
		QCoreApplication::processEvents();
		require(control<QWidget>(window, QStringLiteral("config.vprenderer.motion_preset_box"))->isEnabled(),
			"the Classic Motion tier leaves the de-judder amount usable");
		control<QSlider>(window, Vpv::ObjectName("motion.tier"))->setValue(2);
		QCoreApplication::processEvents();
		require(control<QSlider>(window, QStringLiteral("config.vprenderer.picture_preset"))->minimum() == 1,
			"the picture slider never offers its own Off (the Scaling tier owns re-base on/off)");
		// The slider-set fields and the independent shaping are separate cards.
		require(control<QWidget>(window, Vpv::ObjectName("motion.card.taste_keys.body"))->isAncestorOf(strength) &&
			control<QWidget>(window, Vpv::ObjectName("motion.card.shaping.body"))->isAncestorOf(
				control<QWidget>(window, QStringLiteral("config.vprenderer.dejudder_fade_match"))),
			"the de-judder amount's fields and the shaping are separate cards");
		// One live control per key: no shown upstream row for a key a VP-V toggle owns.
		for (const std::string& key : Vpv::ManagedKeys(Vpv::TierTable()))
		{
			const QRegularExpression name(QStringLiteral("^config\\.vprenderer\\.%1(\\.[^.]+)?$").arg(QString::fromStdString(key)));
			auto* home = window.findChild<QWidget*>(Vpv::ObjectName("section"));
			for (QWidget* w : window.findChildren<QWidget*>(name))
			{
				bool keyHome = false;
				for (QWidget* p = w; p; p = p->parentWidget()) keyHome = keyHome || p->property("vpvKeyHome").toBool();
				if (keyHome) continue;
				require(!RowShown(w) || !w->isEnabled(),
					("a second live control: " + w->objectName().toStdString()).c_str());
				(void)home;
			}
		}
		// Dynamic Drive: the Advanced LED card's masters grey their groups.
		auto* values = control<QCheckBox>(window, Vpv::ObjectName("drive.advanced_values"));
		auto* highlights = control<QWidget>(window, QStringLiteral("config.led_drive.default.highlight_percentile"));
		require(!values->isChecked() && !highlights->isEnabled(), "the LED groups are set by the LED profile");
		values->setChecked(true);
		QCoreApplication::processEvents();
		require(highlights->isEnabled() && !control<QWidget>(window, QStringLiteral("config.led_drive.default.tick_ms"))->isEnabled(),
			"Custom frees the groups the chosen source uses (Script-only stays greyed in Compensate)");
		// The Dynamic Drive page: masters on Main, what they set on Advanced (the speeds set by their preset).
		auto* driveMain = control<QWidget>(window, Vpv::ObjectName("drivepage.main"));
		auto* driveAdvanced = control<QWidget>(window, Vpv::ObjectName("drivepage.advanced"));
		require(driveMain->isAncestorOf(control<QWidget>(window, QStringLiteral("config.led_drive.default.command_source"))) &&
			driveMain->isAncestorOf(control<QWidget>(window, QStringLiteral("config.led_drive.default.rate_preset"))),
			"the Dynamic Drive masters are on Main");
		auto* rateUp = control<QWidget>(window, QStringLiteral("config.led_drive.default.rate_gain_up"));
		require(driveAdvanced->isAncestorOf(rateUp) && !rateUp->isEnabled(), "the LED speeds values are set by their preset (Advanced)");
		control<QCheckBox>(window, Vpv::ObjectName("drive.card.speed_values.custom"))->setChecked(true);
		QCoreApplication::processEvents();
		require(rateUp->isEnabled(), "Custom frees the LED speeds values");
		require(driveAdvanced->isAncestorOf(control<QWidget>(window, Vpv::ObjectName("drive.group.highlights"))),
			"each advanced LED group is a card of its own on Advanced");
		auto* fixes = control<QCheckBox>(window, Vpv::ObjectName("drive.led_fixes"));
		require(!fixes->isChecked() && !control<QWidget>(window, QStringLiteral("config.led_drive.default.cntr_pair_predict"))->isEnabled(),
			"the LED fixes are set by their master");
		// The VP-V page fits the default window: no sideways scroll on any unit sub-page. Measured only with
		// real fonts (run_config_tests.ps1 reruns this test with QT_QPA_FONTDIR); off screen without
		// fonts the text measures as placeholder boxes.
		if (QFontDatabase::families().isEmpty())
		{
			save(window);
			requireVpReads(path);
			return;
		}
		// The VP-V page fits the default window: no sideways scroll on any unit sub-page.
		window.resize(1040, 700);
		window.show();
		auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
		auto* section = control<QWidget>(window, Vpv::ObjectName("section"));
		for (int i = 0; pages && i < pages->count(); ++i)
			if (pages->widget(i)->isAncestorOf(section)) window.selectPage(i);
		QScrollArea* scroll = nullptr;
		for (QWidget* p = section; p && !scroll; p = p->parentWidget()) scroll = qobject_cast<QScrollArea*>(p);
		auto* units = control<QTabWidget>(window, Vpv::ObjectName("units"));
		for (int u = 0; u < units->count(); ++u)
		{
			units->setCurrentIndex(u);
			QTabWidget* sub = units->widget(u)->findChild<QTabWidget*>();
			for (int s2 = 0; s2 < (sub ? sub->count() : 1); ++s2)
			{
				if (sub) sub->setCurrentIndex(s2);
				for (QToolButton* t : units->widget(u)->findChildren<QToolButton*>())
					if (t->isCheckable() && t->objectName().endsWith(QStringLiteral(".toggle"))) t->setChecked(true);
				QCoreApplication::processEvents();
				QString widest;
				if (scroll && scroll->horizontalScrollBar()->maximum() > 0)
				{
					const int limit = scroll->viewport()->width();
					for (QWidget* w : section->findChildren<QWidget*>())
					{
						if (w->minimumSizeHint().width() <= limit - 40) continue;
						bool childWide = false;
						for (QWidget* c : w->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
							childWide = childWide || c->minimumSizeHint().width() > limit - 40;
						if (!childWide) widest += QStringLiteral(" %1[%2]=%3 under %4").arg(w->objectName(), QString::fromLatin1(w->metaObject()->className())).arg(w->minimumSizeHint().width()).arg(w->parentWidget() ? w->parentWidget()->objectName() + QLatin1Char('/') + (w->parentWidget()->parentWidget() ? w->parentWidget()->parentWidget()->objectName() : QString()) : QString());
					}
				}
				require(scroll && scroll->horizontalScrollBar()->maximum() == 0,
					(QStringLiteral("the VP-V page scrolls sideways on %1 / %2 (%3 px)").arg(units->tabText(u),
						sub ? sub->tabText(s2) : QString()).arg(scroll ? scroll->horizontalScrollBar()->maximum() : -1) + widest).toStdString().c_str());
			}
		}
		// The Dynamic Drive page, in the same scheme (Main | Advanced), fits too.
		auto* drivePages = control<QTabWidget>(window, Vpv::ObjectName("drivepage.pages"));
		for (int i = 0; pages && i < pages->count(); ++i)
			if (pages->widget(i)->isAncestorOf(drivePages)) window.selectPage(i);
		QScrollArea* driveScroll = nullptr;
		for (QWidget* p = drivePages; p && !driveScroll; p = p->parentWidget()) driveScroll = qobject_cast<QScrollArea*>(p);
		for (int t = 0; t < drivePages->count(); ++t)
		{
			drivePages->setCurrentIndex(t);
			QCoreApplication::processEvents();
			require(driveScroll && driveScroll->horizontalScrollBar()->maximum() == 0,
				(QStringLiteral("the Dynamic Drive page scrolls sideways on %1 (%2 px)").arg(drivePages->tabText(t))
					.arg(driveScroll ? driveScroll->horizontalScrollBar()->maximum() : -1)).toStdString().c_str());
		}
		window.hide();
		save(window);
		requireVpReads(path);
	}
}

namespace
{
	// The shown control of a key in a row: the row's field, when it is shown and enabled.
	QSet<QWidget*> LiveRows(QWidget& window, const QString& key)
	{
		QSet<QWidget*> rows;
		const QRegularExpression name(QStringLiteral("^config\\.vprenderer\\.%1(\\.[^.]+)?$").arg(QRegularExpression::escape(key)));
		for (QWidget* w : window.findChildren<QWidget*>(name))
		{
			if (!w->isEnabled() || w->isHidden()) continue;
			for (QWidget* f = w; f && f->parentWidget(); f = f->parentWidget())
				if (auto* form = qobject_cast<QFormLayout*>(f->parentWidget()->layout()))
				{
					int row = -1;
					QFormLayout::ItemRole role;
					form->getWidgetPosition(f, &row, &role);
					if (row < 0) continue;
					if (form->isRowVisible(row)) rows.insert(f);
					break;
				}
		}
		return rows;
	}

	// Logic audit 2026-10-24 (HIGH 1-2, MEDIUM 3-6, LOW): tiers never written unasked, the taste kept,
	// Custom recomputed, one live control per key in both pathways, defaults equal the renderer's,
	// the LED profile named, Dynamic Drive off in the Original pathway, fixes restored explicitly.
	void testLogicAuditFixes()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[vprenderer]\nquality: high\n"
			"[vprenderer.Cinema]\nquality: high\n"
			"[queue]\ntarget_latency_ms: 250\n"
			"[led_drive.Default]\nenabled: true\ncommand_source: compensate\n"
			"[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
		ConfigEditorWindow window(path, 0, true);
		const Vpv::Table& table = Vpv::TierTable();
		// MEDIUM 5 (on the fresh config, before any edit): the layout's defaults equal the renderer's (Config's mirrored default table).
		for (const Vpv::KeyValue& kv : table.defaults)
			if (auto* w = window.findChild<QWidget*>(QStringLiteral("config.vprenderer.") + QString::fromLatin1(kv.key)))
			{
				const QString effective = w->property("effectiveValue").toString().trimmed();
				if (effective.isEmpty()) continue;
				require(Vpv::SameValue(effective.toStdString(), kv.value),
					(std::string("default of ") + kv.key + ": layout " + kv.value + ", renderer " + effective.toStdString()).c_str());
			}
		// HIGH 1: on a fresh config every tiered unit reads a tier (the shipped values), never Custom; Default works.
		for (const Vpv::Unit& u : table.units)
		{
			if (!u.Tiered()) continue;
			const QString id = QString::fromLatin1(u.id);
			require(control<QLabel>(window, Vpv::ObjectName(id + QStringLiteral(".tier.name")))->text() != QStringLiteral("Custom"),
				(std::string("the shipped values read as a tier: ") + u.id).c_str());
			require(control<QPushButton>(window, Vpv::ObjectName(id + QStringLiteral(".tier.default")))->isEnabled(),
				(std::string("Default is offered: ") + u.id).c_str());
		}
		auto* dither = control<QComboBox>(window, QStringLiteral("config.vprenderer.temporal_dither"));
		auto effective = [](QComboBox* combo) { const QString v = combo->currentData().toString(); return v.isEmpty() ? combo->property("effectiveValue").toString() : v; };
		const QString ditherBefore = effective(dither);
		control<QPushButton>(window, Vpv::ObjectName("global_tier.reset"))->click();
		auto* outputCustom = control<QCheckBox>(window, Vpv::ObjectName("output.card.tier_keys.custom"));
		outputCustom->setChecked(true);
		outputCustom->setChecked(false);
		QCoreApplication::processEvents();
		require(effective(dither) == ditherBefore,
			"Reset all units / unticking Custom on a fresh config writes no tier (the sigma-delta is kept)");
		// HIGH 2: the tier never writes the taste; leaving Classic keeps the amount.
		auto* even = control<QLineEdit>(window, QStringLiteral("config.vprenderer.dejudder_strength_even"));
		control<QCheckBox>(window, Vpv::ObjectName("motion.card.taste_keys.custom"))->setChecked(true);
		even->setText(QStringLiteral("0.5"));
		QMetaObject::invokeMethod(even, "editingFinished", Qt::DirectConnection);
		control<QSlider>(window, Vpv::ObjectName("motion.tier"))->setValue(0);
		control<QSlider>(window, Vpv::ObjectName("motion.tier"))->setValue(2);
		QCoreApplication::processEvents();
		require(even->text() == QStringLiteral("0.5"), "Classic and back keep the even-cadence strength (the taste)");
		// MEDIUM 3: Custom recomputes both ways on a profile switch.
		auto* list = control<QListWidget>(window, QStringLiteral("config.vprenderer.profiles"));
		auto* repairCustom = control<QCheckBox>(window, Vpv::ObjectName("repair.card.tier_keys.custom"));
		repairCustom->setChecked(true);
		list->setCurrentRow(1);
		QCoreApplication::processEvents();
		list->setCurrentRow(0);
		QCoreApplication::processEvents();
		require(!repairCustom->isChecked(), "a profile switch unticks Custom when the values sit on the tier");
		// One visible, enabled control per key, in both pathways.
		auto oneEach = [&](const char* pathway)
		{
			std::set<std::string> keys;
			for (QWidget* w : window.findChildren<QWidget*>(QRegularExpression(QStringLiteral("^config\\.vprenderer\\.[a-z0-9_]+$"))))
				keys.insert(w->objectName().mid(18).toStdString());
			for (const std::string& key : keys)
			{
				int live = LiveRows(window, QString::fromStdString(key)).size();
				for (QWidget* w : window.findChildren<QWidget*>(QRegularExpression(QStringLiteral("^vpv\\.[a-z]+\\.%1$").arg(QString::fromStdString(key)))))
					live += (w->isEnabled() && !w->isHidden()) ? 1 : 0;
				require(live <= 1, (std::string(pathway) + ": more than one live control for " + key).c_str());
			}
		};
		oneEach("VP-V pathway");
		// LOW: the proven fixes come back explicitly on.
		auto* fixesCustom = control<QCheckBox>(window, Vpv::ObjectName("retirement.card.fixes.custom"));
		fixesCustom->setChecked(true);
		{ auto* pulldown = control<QComboBox>(window, QStringLiteral("config.vprenderer.pulldown_fixes")); pulldown->setCurrentIndex(pulldown->findData(QStringLiteral("off"))); }
		fixesCustom->setChecked(false);
		QCoreApplication::processEvents();
		require(control<QComboBox>(window, QStringLiteral("config.vprenderer.pulldown_fixes"))->currentData().toString() == QStringLiteral("on"),
			"unticking Proven fixes writes every fix on again");
		// LED profile: the greyed groups say where their values come from.
		bool named = false;
		for (QLabel* label : control<QWidget>(window, Vpv::ObjectName("drive.group.highlights"))->findChildren<QLabel*>())
			named = named || label->text().contains(QStringLiteral("your script's built-in values"));
		require(named, "the greyed LED values say they are the script's built-in values (no profile file)");
		if (auto* file = window.findChild<QLineEdit*>(QStringLiteral("config.led_drive.default.profile_file")))
		{
			file->setText(QStringLiteral("C:/profiles/cinema.json"));
			QCoreApplication::processEvents();
			QCoreApplication::processEvents();
			bool fromFile = false;
			for (QLabel* label : control<QWidget>(window, Vpv::ObjectName("drive.group.highlights"))->findChildren<QLabel*>())
				fromFile = fromFile || label->text().contains(QStringLiteral("cinema.json"));
			require(fromFile, "the greyed LED values name the LED profile file");
		}
		// MEDIUM 6: the Original pathway turns every VP addition off, Dynamic Drive included.
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
		QCoreApplication::processEvents();
		require(!control<QWidget>(window, Vpv::ObjectName("drivepage.pages"))->isEnabled(), "the Original pathway disables Dynamic Drive");
		oneEach("Original pathway");
	}
}

namespace
{
	// ---- Config usability package (2026-10-25) ----

	// "Set by" wording and order (pure: VpvSetBy.h).
	void testSetByWording()
	{
		Vpv::SetByQuery q;
		q.key = "deband_vp";
		q.builtInDefault = "off";
		Vpv::SetByResult r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::BuiltInDefault && r.text == "Set by VP's built-in default: Off", ("built-in default: " + r.text).c_str());
		q.master = "the Repair tier (Low)";
		r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::Master && r.text == "Set by the Repair tier (Low): Off", ("master: " + r.text).c_str());
		q.master.clear();
		q.ownValue = "on";
		r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::ThisProfile && r.text == "Set by you: On", ("you: " + r.text).c_str());
		q.defaultProfileOnScreen = false;
		q.profileName = "Film24";
		r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::ThisProfile && r.text == "Set by you, in profile \"Film24\": On", ("you in a profile: " + r.text).c_str());
		q.profileRule = "$source_rate < 25";
		r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::RuleProfile && r.text.find("used when its rule matches") != std::string::npos, ("rule profile: " + r.text).c_str());
		q.ownValue.clear();
		q.inheritedValue = "on";
		r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::DefaultProfile && r.text == "Set by the default profile: On", ("default profile: " + r.text).c_str());
		q.notUsedBecause = "VP's debander is off";
		r = Vpv::SetBy(q);
		require(r.text == "Set by the default profile: On. Not used now: VP's debander is off", ("not used: " + r.text).c_str());
		q.originalPathway = true;
		r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::OriginalPathway && r.text.find("Original pathway") != std::string::npos &&
			r.text.find(": Off") != std::string::npos, ("Original pathway: " + r.text).c_str());
		q.key = "quality";   // Bill's key: the baseline does not override it
		r = Vpv::SetBy(q);
		require(r.source != Vpv::ValueSource::OriginalPathway, "a key the Original pathway does not set is unaffected by it");
		std::string value;
		for (char group : { 'A', 'B', 'C', 'D' })
			require(!Vpv::OriginalPathwayValue(Vpv::DecisionKey(group), value), "a retirement decision has no render effect, so no Original-pathway entry");
	}

	// Every setting on a VP-V unit tab says where its value comes from, and every setting a lock greys says why.
	void testSetByOnThePage()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[vprenderer]\nquality: high\ntexture_restore_cap: 1.5\nadaptive_upscale: on\n"
			"[vprenderer.Film24]\nwhen: $source_rate < 25\nquality: high\ntexture_restore_cap: 2\n"
			"[queue]\ntarget_latency_ms: 250\n");
		ConfigEditorWindow window(path, 0, true);
		auto note = [&](const char* key) { return control<QLabel>(window, Vpv::ObjectName(QStringLiteral("setby.line.") + QString::fromLatin1(key)))->text(); };
		// A tier-locked setting names its master and the value.
		require(note("deband_vp") == QStringLiteral("Set by the Repair tier (Low): Off"), ("tier-locked: " + S(note("deband_vp"))).c_str());
		// A setting nothing sets: the built-in default; one the profile writes: you.
		require(note("adaptive_upscale") == QStringLiteral("Set by you: On"), ("you (toggle): " + S(note("adaptive_upscale"))).c_str());
		require(note("rebase_kernel_match").startsWith(QStringLiteral("Set by VP's built-in default: ")), ("built-in: " + S(note("rebase_kernel_match"))).c_str());
		require(note("texture_restore_cap") == QStringLiteral("Set by you: 1.5"), ("you: " + S(note("texture_restore_cap"))).c_str());
		// A feature's tuning that its switch leaves unused says so.
		require(note("intent_content_hold_seconds").contains(QStringLiteral("Not used now: Content labels is off")),
			("feature off: " + S(note("intent_content_hold_seconds"))).c_str());
		// Every row a card places has its line, and no line shows a Qt placeholder or an empty string.
		int lines = 0;
		for (QLabel* label : window.findChildren<QLabel*>(QRegularExpression(QStringLiteral("^vpv\\.setby\\.line\\."))))
		{
			++lines;
			require(label->text().startsWith(QStringLiteral("Set by ")), ("a Set by line: " + S(label->objectName()) + " = " + S(label->text())).c_str());
		}
		require(lines > 40, "the unit tabs carry a Set by line for their settings");
		// A named profile: its own value under its rule, the default profile's value when it sets nothing.
		auto* picker = control<QComboBox>(window, Vpv::ObjectName("profile"));
		picker->setCurrentIndex(picker->findData(QStringLiteral("vprenderer.Film24")));
		QCoreApplication::processEvents();
		require(note("texture_restore_cap") == QStringLiteral("Set by you, in profile \"Film24\" (used when its rule matches): 2"),
			("rule profile: " + S(note("texture_restore_cap"))).c_str());
		require(note("adaptive_upscale") == QStringLiteral("Set by the default profile: On"), ("inherited: " + S(note("adaptive_upscale"))).c_str());
		picker->setCurrentIndex(0);
		QCoreApplication::processEvents();
		// The Original pathway: the baseline's value, whatever the profile says.
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
		QCoreApplication::processEvents();
		require(note("deband_vp") == QStringLiteral("Set by the Original pathway (VP's additions are off): Off"), ("Original pathway: " + S(note("deband_vp"))).c_str());
		require(note("texture_restore_cap").startsWith(QStringLiteral("Set by you")), "a key the Original pathway does not set keeps its own line");
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(0);
		QCoreApplication::processEvents();
		// Custom: the setting is yours, not the tier's.
		control<QCheckBox>(window, Vpv::ObjectName("repair.card.tier_keys.custom"))->setChecked(true);
		auto* deband = control<QComboBox>(window, QStringLiteral("config.vprenderer.deband_vp"));
		deband->setCurrentIndex(deband->findData(QStringLiteral("on")));
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(0);
		QCoreApplication::processEvents();
		require(note("deband_vp") == QStringLiteral("Set by you: On"), ("Custom: " + S(note("deband_vp"))).c_str());
	}

	// The Dynamic Drive page's Main holds only on / off, the command source, the LED profile, the speeds preset and
	// the projector link; every greyed control says why; the white-clips control is always there.
	void testDynamicDriveMainPage()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[vprenderer]\nquality: high\n[queue]\ntarget_latency_ms: 250\n"
			"[led_drive.Default]\nenabled: true\ncommand_source: compensate\n"
			"[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
		ConfigEditorWindow window(path, 0, true);
		auto* main = control<QWidget>(window, Vpv::ObjectName("drivepage.main"));
		auto* advanced = control<QWidget>(window, Vpv::ObjectName("drivepage.advanced"));
		std::set<std::string> allowed;
		for (const char* key : Vpv::DriveMainKeys()) allowed.insert(key);
		int onMain = 0;
		for (QWidget* w : main->findChildren<QWidget*>(QRegularExpression(QStringLiteral("^config\\.(led_drive|projector)\\.default\\."))))
		{
			++onMain;
			const std::string key = S(w->objectName().section(QLatin1Char('.'), 3, 3));
			require(allowed.count(key) != 0, ("not a Dynamic Drive Main control: " + S(w->objectName())).c_str());
		}
		require(onMain > 0, "the Main page holds the Dynamic Drive controls");
		for (const char* key : Vpv::DriveMainKeys())
		{
			const bool projector = std::string(key) == "protocol" || std::string(key) == "port" || std::string(key) == "connection_test";
			auto* w = control<QWidget>(window, QStringLiteral("config.%1.default.%2").arg(QLatin1String(projector ? "projector" : "led_drive"), QString::fromLatin1(key)));
			require(main->isAncestorOf(w), (std::string("on the Dynamic Drive Main page: ") + key).c_str());
		}
		for (const char* name : { "config.led_drive.default.adaptive_gamma", "config.led_drive.default.lookahead", "config.led_drive.default.gamma_boost",
			"config.led_drive.default.shadow_lift_ansi", "config.led_drive.default.led_floor", "config.led_drive.default.script_pacing",
			"config.projector.default.led_latency_ms", "config.projector.default.cntr_latency_ms", "config.projector.default.read_back",
			"config.projector.default.dmd_clip_cntr", "config.projector.default.cntr_max_usable", "config.led_drive.default.cntr_assist",
			"config.led_drive.default.ftb_experimental" })
			require(advanced->isAncestorOf(control<QWidget>(window, QString::fromLatin1(name))), (std::string("on Advanced, not Main: ") + name).c_str());
		// "Contrast where white clips" always has its control; with another source it is greyed and says why.
		auto* whiteClips = control<QWidget>(window, QStringLiteral("config.projector.default.dmd_clip_cntr"));
		require(whiteClips->isEnabled() && RowShown(whiteClips), "Compensate: the white-clips control is usable");
		control<QRadioButton>(window, QStringLiteral("config.led_drive.default.command_source.internal_script"))->click();
		QCoreApplication::processEvents();
		require(RowShown(whiteClips) && !whiteClips->isEnabled() && whiteClips->toolTip().contains(QStringLiteral("Compensate")),
			"another source: the white-clips control stays, greyed, saying it is for Compensate");
		// "No LED commands": the LED profile and the speeds preset stay on Main, greyed with the reason (not hidden).
		control<QRadioButton>(window, QStringLiteral("config.led_drive.default.command_source.none"))->click();
		QCoreApplication::processEvents();
		for (const char* key : { "profile_file", "rate_preset" })
		{
			auto* w = control<QWidget>(window, QStringLiteral("config.led_drive.default.") + QString::fromLatin1(key));
			require(w->isVisibleTo(main) && !w->isEnabled(), (std::string("greyed, not hidden, with another source: ") + key).c_str());
			bool reason = false;
			for (QWidget* p = w; p && p != main; p = p->parentWidget()) reason = reason || p->toolTip().contains(QStringLiteral("Greyed: used only with"));
			require(reason, (std::string("the greyed control says why: ") + key).c_str());
		}
		control<QRadioButton>(window, QStringLiteral("config.led_drive.default.command_source.compensate"))->click();
		QCoreApplication::processEvents();
		require(control<QWidget>(window, QStringLiteral("config.led_drive.default.rate_preset"))->isEnabled(), "Compensate: the speeds preset is usable");
		// Greyed controls say why: adaptive gamma off greys the shadow lift; Dynamic Drive off greys the rest.
		auto* adaptive = control<QCheckBox>(window, QStringLiteral("config.led_drive.default.adaptive_gamma"));
		adaptive->setChecked(false);
		QCoreApplication::processEvents();
		auto* lift = control<QWidget>(window, QStringLiteral("config.led_drive.default.shadow_lift_ansi"));
		require(lift->toolTip().contains(QStringLiteral("Adaptive gamma")), "the shadow lift says it needs Adaptive gamma");
		require(!control<QLabel>(window, Vpv::ObjectName("drive.reason.behaviour"))->isHidden() &&
			control<QLabel>(window, Vpv::ObjectName("drive.reason.behaviour"))->text().contains(QStringLiteral("Adaptive gamma")),
			"the behaviour card lists why the shadow lift is greyed");
		auto* drive = control<QCheckBox>(window, QStringLiteral("config.led_drive.default.enabled"));
		drive->setChecked(false);
		QCoreApplication::processEvents();
		require(!control<QLabel>(window, Vpv::ObjectName("drivepage.off_note"))->isHidden() &&
			control<QLabel>(window, Vpv::ObjectName("drivepage.off_note"))->text().contains(QStringLiteral("Dynamic Drive is off")),
			"Dynamic Drive off: the page says why the rest is greyed");
		require(adaptive->toolTip().contains(QStringLiteral("Greyed: Dynamic Drive is off")), "a greyed control names Dynamic Drive being off");
		drive->setChecked(true);
		QCoreApplication::processEvents();
		require(control<QLabel>(window, Vpv::ObjectName("drivepage.off_note"))->isHidden() &&
			!adaptive->toolTip().contains(QStringLiteral("Greyed: Dynamic Drive is off")), "switching it on removes the reason");
		save(window);
		requireVpReads(path);
	}

	// Four tier steps on every tiered unit; a planned step is shown, marked, and writes nothing.
	void testFourTierSteps()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		ConfigEditorWindow window(path, 0, true);
		int tiered = 0;
		for (const Vpv::Unit& u : Vpv::TierTable().units)
		{
			const QString id = QString::fromLatin1(u.id);
			if (!u.Tiered())
			{
				require(window.findChild<QLabel*>(Vpv::ObjectName(id + QStringLiteral(".untiered"))) != nullptr,
					(std::string("an untiered unit keeps its explanation: ") + u.id).c_str());
				require(!window.findChild<QSlider*>(Vpv::ObjectName(id + QStringLiteral(".tier"))), (std::string("no slider on an untiered unit: ") + u.id).c_str());
				continue;
			}
			++tiered;
			auto* slider = control<QSlider>(window, Vpv::ObjectName(id + QStringLiteral(".tier")));
			require(slider->minimum() == 0 && slider->maximum() == 3, (std::string("four tier steps: ") + u.id).c_str());
			const QString steps = control<QLabel>(window, Vpv::ObjectName(id + QStringLiteral(".tier.steps")))->text();
			for (const char* tier : { "Classic", "Low", "Mid", "Upper" })
				require(steps.contains(QString::fromLatin1(tier)), (std::string("the steps name ") + tier + " on " + u.id).c_str());
			if (u.plannedTiers.empty()) continue;
			// A planned Upper: marked, and the slider refuses to stay on it; nothing is written.
			require(steps.contains(QStringLiteral("Upper (planned - uses Mid for now)")), (std::string("Upper is marked planned: ") + u.id).c_str());
			const QString before = control<QLabel>(window, Vpv::ObjectName(id + QStringLiteral(".tier.name")))->text();
			const int position = slider->value();
			std::map<std::string, std::string> keys;
			for (const Vpv::Preset& p : u.presets)
				for (const Vpv::KeyValue& kv : p.set)
					if (auto* combo = window.findChild<QComboBox*>(QStringLiteral("config.vprenderer.") + QString::fromLatin1(kv.key)))
						keys[kv.key] = S(combo->currentData().toString());
			slider->setValue(3);
			QCoreApplication::processEvents();
			require(slider->value() == position, (std::string("the handle goes back from a planned step: ") + u.id).c_str());
			require(control<QLabel>(window, Vpv::ObjectName(id + QStringLiteral(".tier.name")))->text() == before,
				(std::string("a planned step changes no tier: ") + u.id).c_str());
			for (const auto& kv : keys)
				if (auto* combo = window.findChild<QComboBox*>(QStringLiteral("config.vprenderer.") + QString::fromStdString(kv.first)))
					require(S(combo->currentData().toString()) == kv.second, ("a planned step writes nothing: " + kv.first).c_str());
			const QString planned = control<QLabel>(window, Vpv::ObjectName(id + QStringLiteral(".tier.planned")))->text();
			require(planned.contains(QStringLiteral("Upper is planned")) && planned.contains(QStringLiteral("uses Mid for now")) &&
				planned.contains(QStringLiteral("Nothing was changed")), (std::string("the planned step explains itself: ") + u.id).c_str());
		}
		require(tiered == 4, "motion, repair, scaling and output are the tiered units");
		// The global tier has the same four steps, Upper included.
		require(control<QSlider>(window, Vpv::ObjectName("global_tier"))->maximum() == 3, "the global tier has four steps");
		save(window);
		requireVpReads(path);
	}

	// Dragging the handle from Custom onto the planned Upper and letting go changes nothing (audit 2026-10-25, MEDIUM).
	void testPlannedDragFromCustomWritesNothing()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		ConfigEditorWindow window(path, 0, true);
		auto set = [&](const char* key, const char* value)
		{
			auto* combo = control<QComboBox>(window, QStringLiteral("config.vprenderer.") + QString::fromLatin1(key));
			combo->setCurrentIndex(combo->findData(QString::fromLatin1(value)));
		};
		control<QCheckBox>(window, Vpv::ObjectName("scaling.card.tier_keys.custom"))->setChecked(true);
		set("source_resolution", "native");   // Classic's value ...
		set("adaptive_downscale", "on");      // ... with Mid's: no tier matches
		QCoreApplication::processEvents();
		require(control<QLabel>(window, Vpv::ObjectName("scaling.tier.name"))->text() == QStringLiteral("Custom"), "the unit reads Custom");
		auto snapshot = [&]()
		{
			std::map<std::string, std::string> keys;
			for (const char* key : { "source_resolution", "adaptive_downscale", "adaptive_w_base" })
				if (auto* w = window.findChild<QWidget*>(QStringLiteral("config.vprenderer.") + QString::fromLatin1(key)))
				{
					if (auto* combo = qobject_cast<QComboBox*>(w)) keys[key] = S(combo->currentData().toString());
					else if (auto* edit = qobject_cast<QLineEdit*>(w)) keys[key] = S(edit->text());
				}
			return keys;
		};
		const auto before = snapshot();
		auto* slider = control<QSlider>(window, Vpv::ObjectName("scaling.tier"));
		slider->setSliderDown(true);
		slider->setValue(3);
		slider->setSliderDown(false);   // releases: emits sliderReleased
		QCoreApplication::processEvents();
		require(snapshot() == before, "a drag from Custom onto the planned Upper and release writes no key");
		require(control<QLabel>(window, Vpv::ObjectName("scaling.tier.name"))->text() == QStringLiteral("Custom"), "the unit is still Custom");
	}

	// Approve / Keep per retirement group: only recorded; nothing is hidden; stored as [vpv] retirement_a..d.
	void testRetirementDecisions()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		// The validator: approve / keep (any case) only, and only groups A-D.
		{
			QTemporaryDir bad;
			const QString badPath = WriteFixture(bad, "[vprenderer]\nquality: high\n[vpv]\nretirement_a: maybe\n");
			ConfigFile config;
			require(config.Load(badPath.toStdString()), "load");
			RendererProfileConfig::Model model;
			std::string error;
			require(!RendererProfileConfig::Read(config, model, error) && error.find("retirement_a") != std::string::npos, "a decision other than approve or keep is refused");
			const QString unknownPath = WriteFixture(bad, "[vprenderer]\nquality: high\n[vpv]\nretirement_e: keep\n");
			ConfigFile unknown;
			require(unknown.Load(unknownPath.toStdString()), "load");
			require(!RendererProfileConfig::Read(unknown, model, error), "an unknown retirement group is refused");
			const QString goodPath = WriteFixture(bad, "[vprenderer]\nquality: high\n[vpv]\nretirement_a: Approve\nretirement_d: keep\n");
			requireVpReads(goodPath);
		}
		auto liveCandidates = [](ConfigEditorWindow& window)
		{
			std::set<std::string> live;
			for (const Vpv::Retirement& r : Vpv::TierTable().retirements)
				if (std::string(r.section) == "vprenderer")
					if (auto* w = window.findChild<QWidget*>(QStringLiteral("config.vprenderer.") + QString::fromLatin1(r.key)))
						if (RowShown(w)) live.insert(r.key);
			return live;
		};
		std::set<std::string> before;
		{
			ConfigEditorWindow window(path, 0, true);
			before = liveCandidates(window);
			require(before.size() > 10, "the retirement candidates have shown controls");
			for (char group : { 'A', 'B', 'C', 'D' })
			{
				const QString g = QString(QLatin1Char(group));
				require(control<QRadioButton>(window, Vpv::ObjectName(QStringLiteral("retirement.decision_") + g + QStringLiteral(".none")))->isChecked(),
					"no decision is recorded at first");
				require(control<QLabel>(window, Vpv::ObjectName(QStringLiteral("retirement.group.") + g + QStringLiteral(".status")))->text().contains(QStringLiteral("No decision")),
					"the group says nothing is decided");
			}
			control<QRadioButton>(window, Vpv::ObjectName("retirement.decision_A.approve"))->click();
			control<QRadioButton>(window, Vpv::ObjectName("retirement.decision_B.keep"))->click();
			QCoreApplication::processEvents();
			const QString status = control<QLabel>(window, Vpv::ObjectName("retirement.group.A.status"))->text();
			require(status.contains(QStringLiteral("Recorded: Approve")) && status.contains(QStringLiteral("Nothing is hidden")), "Approve says it only records");
			require(control<QLabel>(window, Vpv::ObjectName("retirement.group.B.status"))->text().contains(QStringLiteral("Recorded: Keep")), "Keep is recorded");
			// Approve hides nothing: every candidate's control and its listing are still there.
			require(liveCandidates(window) == before, "approving a group hides no control");
			for (char group : { 'A', 'B', 'C', 'D' })
			{
				const QString list = control<QLabel>(window, Vpv::ObjectName(QStringLiteral("retirement.group.") + QString(QLatin1Char(group)) + QStringLiteral(".list")))->text();
				require(!list.isEmpty() && !list.contains(QStringLiteral("no control found")), "each group still lists its settings");
				require(!list.contains(QStringLiteral("docs/")) && !list.contains(QStringLiteral(".md")) && !list.contains(QStringLiteral("_fixes")) &&
					!list.contains(QStringLiteral("dither_vectors")), "the listing uses plain names (no key names, no docs paths)");
			}
			// The control of dither_vectors is where the listing says it is (not on the Rendering page).
			const QString groupC = control<QLabel>(window, Vpv::ObjectName("retirement.group.C.list"))->text();
			require(groupC.contains(QStringLiteral("VP-V &gt; ")), ("group C names the VP-V tab: " + S(groupC)).c_str());
			// The Original pathway keeps the record and greys the controls with the rest of the page.
			control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
			QCoreApplication::processEvents();
			require(control<QRadioButton>(window, Vpv::ObjectName("retirement.decision_A.approve"))->isChecked() &&
				!control<QRadioButton>(window, Vpv::ObjectName("retirement.decision_A.approve"))->isEnabled(),
				"the Original pathway keeps the recorded decision and greys the choice with the page");
			control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(0);
			save(window);
		}
		ConfigFile config;
		std::string v;
		require(config.Load(path.toStdString()), "reload");
		require(config.TryGetString("vpv", "retirement_a", v) && v == "approve", "Approve is stored in [vpv] retirement_a");
		require(config.TryGetString("vpv", "retirement_b", v) && v == "keep", "Keep is stored in [vpv] retirement_b");
		require(!config.TryGetString("vpv", "retirement_c", v) && !config.TryGetString("vpv", "retirement_d", v), "an undecided group writes nothing");
		requireVpReads(path);
		{
			ConfigEditorWindow window(path, 0, true);
			require(control<QRadioButton>(window, Vpv::ObjectName("retirement.decision_A.approve"))->isChecked() &&
				control<QRadioButton>(window, Vpv::ObjectName("retirement.decision_B.keep"))->isChecked() &&
				control<QRadioButton>(window, Vpv::ObjectName("retirement.decision_C.none"))->isChecked(), "the decisions read back after reopening");
			require(liveCandidates(window) == before, "after reopening with an Approve recorded, nothing is hidden");
			control<QRadioButton>(window, Vpv::ObjectName("retirement.decision_A.none"))->click();
			save(window);
		}
		ConfigFile cleared;
		require(cleared.Load(path.toStdString()) && !cleared.TryGetString("vpv", "retirement_a", v), "Not decided yet removes the stored decision");
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
	failures += run("VP-V: coverage audit toggles, pointers and retirement marks", testCoverageAuditAdditions);
	failures += run("VP-V: organisation cards, raw group, picker and reload", testOrganisation);
	failures += run("VP-V: masters, derived settings, one control per key, fits the window", testMastersAndFit);
	failures += run("VP-V: logic audit fixes", testLogicAuditFixes);
	failures += run("VP-V: Set by wording and order", testSetByWording);
	failures += run("VP-V: every setting says where its value comes from (Set by)", testSetByOnThePage);
	failures += run("VP-V: Dynamic Drive main page contents and greyed reasons", testDynamicDriveMainPage);
	failures += run("VP-V: four tier steps, Upper shown as planned", testFourTierSteps);
	failures += run("VP-V: a drag from Custom onto the planned step writes nothing", testPlannedDragFromCustomWritesNothing);
	failures += run("VP-V: retirement Approve and Keep are recorded without hiding", testRetirementDecisions);
	return failures;
}
