// VP-V section tests (settings agent, 2026-10-24): the tier data table and model, the delay
// budget display, and the VP-V page inside the editor (off screen like the rest of the suite).
// Registered from ConfigEditorWindowTests.cpp's main with one line: RunVpvConfigTests().
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/control/config/VpvRendererFields.h>

#include "ConfigEditorWindow.h"
#include <VP-V/modules/control/config/config_pages/VpvConfigProviders.h>
#include <VP-V/modules/control/config/config_pages/VpvSection.h>
#include <VP-V/modules/control/config/config_pages/VpvTierTable.generated.h>
#include <VP-V/modules/control/config/config_pages/VpvOrganisation.h>
#define VPV_RENDERING_DEFAULTS_PURE
#include <VP-V/modules/control/config/config_pages/VpvRenderingDefaults.h>
#include <VP-V/modules/control/config/config_pages/VpvSetBy.h>

#include <ConfigFile.h>
#include <VP-V/core/DelayBudgetLadder.h>
#include <VP-V/modules/colour/appearance/AppearanceStatus.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutHostPolicy.h>
#include <RendererProfileConfig.h>
// CameraMotion.h has a member named `signals`, which Qt's keyword macro would rewrite: parse the header without it.
#pragma push_macro("signals")
#undef signals
#include <VP-V/modules/motion/estimation/global_model/link/CameraSettings.h>
#pragma pop_macro("signals")

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QTabWidget>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QScrollBar>
#include <QScrollArea>
#include <QRegularExpression>
#include <QFormLayout>

namespace
{
	// CS-S5: default QT_QPA_FONTDIR to %WINDIR%\Fonts before QApplication starts when unset so
	// font-sensitive fit tests run deterministically in offscreen/CI environments without touching Bill's main().
	struct DefaultQtFontDirInit
	{
		DefaultQtFontDirInit()
		{
			if (!qEnvironmentVariableIsSet("QT_QPA_FONTDIR"))
			{
				const QByteArray windir = qgetenv("WINDIR");
				const QByteArray fonts = (windir.isEmpty() ? QByteArray("C:\\Windows") : windir) + "\\Fonts";
				qputenv("QT_QPA_FONTDIR", fonts);
			}
		}
	} g_defaultQtFontDirInit;
}
#include <QAbstractItemView>
#include <QColor>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QLayout>
#include <QListView>
#include <QListWidget>
#include <QPointer>
#include <QToolButton>
#include <QSlider>
#include <QDir>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
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
		// WAIVER DPTR-W1 + Unit 2 (Phase 1): Repair keys {deband_vp, detail_pulse_stabilise, texture_restore}.
		// Defaults {off,off,off} match Classic (0). Repair Low now sets deband_vp=auto (distinct from Classic=off and Mid=on).
		require(UnitTier(p, "repair") == 0, "defaults are repair Classic (deband_vp=off: libplacebo deband only)");
		// Logic audit 2026-10-24 (HIGH 1): the shipped values are a tier of every unit.
		require(UnitTier(p, "scaling") == 1, "defaults are scaling Low (re-base auto, no SSIM blend)");
		require(UnitTier(p, "output") == 1, "defaults are output Low (temporal dither as shipped)");
		require(Vpv::MatchGlobal(t, p.Get()) < 0, "defaults are global Custom (Motion Mid, the others Low)");

		ApplyGlobal(p, 3);
		require(Vpv::MatchGlobal(t, p.Get()) == 3, "global Upper reads back as Upper");
		require(UnitTier(p, "scaling") == 2 && UnitTier(p, "output") == 2, "units without Upper use their nearest lower tier");
		// WAIVER DPTR-W1: Upper's texture_restore moved statistical -> off, the same as the default, so ApplyTier (which writes only
		// keys whose effective value changes) no longer writes it: Upper presets now show as deband_vp=on (the Repair change)
		// plus dejudder_est_split=on, and texture_restore is NOT statistical.
		require(p.values["deband_vp"] == "on" && p.values["texture_restore"] != "statistical" && p.values["dejudder_est_split"] == "on", "Upper presets written");
		// Repair Upper is planned (uses Mid), while Repair Low (deband_vp=auto) is distinct from Classic (off) and Mid (on).
		require(UnitTier(p, "repair") == 2, "saved Repair Upper keys read back as Mid (lowest identical tier)");
		{
			Profile low;
			low.Apply(Vpv::ApplyTier(t, *t.FindUnit("repair"), 1, low.Get()));
			require(low.values["deband_vp"] == "auto" && UnitTier(low, "repair") == 1,
				"saved Repair Low writes deband_vp=auto and reads back as Low (distinct from Classic)");
			Profile valuesOnly;   // only values, no stored tier: the lowest match
			valuesOnly.values["deband_vp"] = "on";
			require(UnitTier(valuesOnly, "repair") == 2, "a cfg with only values reads back the lowest match (Mid)");
		}

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
			control<QPushButton>(window, Vpv::ObjectName("global_tier.btn.upper"))->click();
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
		for (const auto& kv : std::map<std::string, std::string>{ { "optical_flow", "on" }, { "texture_restore", "off" /* DPTR-OFF: was statistical (Upper), doc 91 :106 */ },
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
			// WAIVER DPTR-W2 (rule 9, as DPTR-W1): Repair Upper and Mid now write identical keys, so the label is the lowest
			// match, "Mid" (was "Upper"); the global tier above still reads Upper.
			require(control<QLabel>(window, Vpv::ObjectName("repair.tier.name"))->text().startsWith(QStringLiteral("Mid")),
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
			require(control<QWidget>(window, QStringLiteral("config.vprenderer.scaling.deband_strength"))->isEnabled(),
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
				control<QWidget>(window, QStringLiteral("config.vprenderer.scaling.deband_strength"))->isEnabled(),
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
			auto* ditherNeed = control<QComboBox>(window, Vpv::ObjectName("expert.dither_need"));
			require(ditherNeed->currentIndex() == 0, "dither_need is off by default");
			ditherNeed->setCurrentIndex(1);
			auto* analysis = control<QCheckBox>(window, Vpv::ObjectName("expert.frame_analysis_store"));
			require(!analysis->isChecked(), "the frame analysis store is off by default");
			analysis->setChecked(true);
			auto* resync = control<QComboBox>(window, Vpv::ObjectName("expert.resync_event"));
			require(resync->currentIndex() == 0, "the resync event is off by default");
			require(resync->count() == 3 && resync->itemData(2).toString() == QStringLiteral("on"), "the resync event offers off, shadow and on");
			resync->setCurrentIndex(1);
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
		require(config.TryGetString("vpv", "dither_need", v) && v == "auto", "dither_need is stored in [vpv]");
		require(config.TryGetString("vpv", "frame_analysis_store", v) && v == "on", "the frame analysis store is stored in [vpv]");
		require(config.TryGetString("vpv", "resync_event", v) && v == "shadow", "the resync event shadow is stored in [vpv]");
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

	// Camera K3 (docs 61 section 3.1): the "Camera model" group of the Motion unit writes the [vpv.camera] keys VP reads (every key
	// of CameraSettings.h, same names and values), an off box removes the key (off is the default), the switches are greyed while
	// the model is off, and everything is greyed in the Original pathway.
	void testCameraModelControlsReachVp()
	{
		static const char* const kBoolKeys[] = { "camera_labels_gpu", "camera_meas_model_global", "camera_meas_guided_candidate",
			"camera_meas_narrow_window", "camera_meas_e1_skip", "camera_meas_static_gate", "camera_meas_cut_skip",
			"camera_meas_fade_normalised", "camera_meas_flash_bridge", "camera_meas_temporal_seed", "camera_meas_tiny_mover_roi",
			"camera_use_grain", "camera_use_detail", "camera_use_dither", "camera_use_deband", "camera_use_filler", "camera_use_dejudder" };
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* model = control<QCheckBox>(window, Vpv::ObjectName("camera.camera_model"));
			auto* overlay = control<QComboBox>(window, Vpv::ObjectName("camera.camera_model_overlay"));
			require(!model->isChecked() && overlay->currentIndex() == 0, "the camera model and its overlay are off by default");
			require(control<QWidget>(window, Vpv::ObjectName("motion.card.camera.body"))->isAncestorOf(model),
				"the controls sit in the Camera model group of the Motion unit");
			require(control<QLabel>(window, Vpv::ObjectName("camera.status")) != nullptr, "the group has a status row");
			for (const char* key : kBoolKeys)
			{
				auto* box = control<QCheckBox>(window, Vpv::ObjectName(QStringLiteral("camera.") + QString::fromLatin1(key)));
				require(!box->isChecked(), (std::string("off by default: ") + key).c_str());
				require(!box->isEnabled(), (std::string("greyed while the camera model is off: ") + key).c_str());
			}
			require(!overlay->isEnabled(), "the overlay is greyed while the camera model is off");
			model->setChecked(true);
			QCoreApplication::processEvents();
			for (const char* key : kBoolKeys)
				require(control<QCheckBox>(window, Vpv::ObjectName(QStringLiteral("camera.") + QString::fromLatin1(key)))->isEnabled(),
					(std::string("usable once the camera model is on: ") + key).c_str());
			require(overlay->isEnabled() && overlay->count() == 4 && overlay->itemData(3).toString() == QStringLiteral("vectors"),
				"the overlay offers off, labels, residual and vectors");
			for (const char* key : { "camera_labels_gpu", "camera_meas_guided_candidate", "camera_meas_static_gate", "camera_use_dejudder", "camera_use_grain" })
				control<QCheckBox>(window, Vpv::ObjectName(QStringLiteral("camera.") + QString::fromLatin1(key)))->setChecked(true);
			overlay->setCurrentIndex(2);
			save(window);
		}
		ConfigFile config;
		std::string v;
		require(config.Load(path.toStdString()), "reload");
		const char* const kSet[] = { "camera_model", "camera_labels_gpu", "camera_meas_guided_candidate", "camera_meas_static_gate",
			"camera_use_dejudder", "camera_use_grain" };
		for (const char* key : kSet)
			require(config.TryGetString("vpv.camera", key, v) && v == "on", (std::string("stored in [vpv.camera]: ") + key).c_str());
		require(config.TryGetString("vpv.camera", "camera_model_overlay", v) && v == "residual", "the overlay is stored as residual");
		for (const char* key : kBoolKeys)
		{
			bool set = false;
			for (const char* s : kSet) set = set || std::string(s) == key;
			require(set || !config.TryGetString("vpv.camera", key, v), (std::string("an off box writes nothing: ") + key).c_str());
		}
		requireVpReads(path);
		// The real parser reads what the page saved (same names, same values, nothing invalid or unknown), and the card offers
		// exactly the keys the parser knows: camera_model, the overlay and the boolean keys above.
		{
			const Vpv::Camera::CameraSettings read = Vpv::Camera::Parse(config);
			require(read.model && read.overlay == Vpv::Camera::OverlayMode::Residual && read.labelsGpu, "the parser reads the model, overlay and labels");
			require(read.meas.guidedCandidate && read.meas.staticGate && !read.meas.modelGlobal && !read.meas.narrowWindow &&
				!read.meas.e1Skip && !read.meas.cutSkip && !read.meas.fadeNormalised && !read.meas.flashBridge &&
				!read.meas.temporalSeed && !read.meas.tinyMoverRoi, "the parser reads exactly the two measurement switches the page set");
			require(read.use.dejudder && read.use.grain && !read.use.detail && !read.use.dither && !read.use.deband && !read.use.filler,
				"the parser reads exactly the two consumers the page set");
			require(read.problems.empty() && read.unknownKeys.empty(), "the parser finds nothing invalid or unknown in the saved section");
			std::set<std::string> cardKeys = { "camera_model", "camera_model_overlay" };
			for (const char* key : kBoolKeys) cardKeys.insert(key);
			std::set<std::string> parserKeys;
			for (size_t i = 0; i < Vpv::Camera::kCameraKeyCount; ++i) parserKeys.insert(Vpv::Camera::kCameraKeys[i]);
			require(cardKeys == parserKeys, "the card's keys equal CameraSettings kCameraKeys");
			require(Vpv::Camera::Validate("camera_model_overlay", "residual") == Vpv::Camera::Verdict::Valid, "the validator accepts the page's overlay value");
		}
		{
			ConfigEditorWindow window(path, 0, true);
			require(control<QCheckBox>(window, Vpv::ObjectName("camera.camera_model"))->isChecked() &&
				control<QCheckBox>(window, Vpv::ObjectName("camera.camera_use_dejudder"))->isChecked() &&
				control<QComboBox>(window, Vpv::ObjectName("camera.camera_model_overlay"))->currentIndex() == 2,
				"the camera settings read back after reopening");
			control<QCheckBox>(window, Vpv::ObjectName("camera.camera_model"))->setChecked(false);
			QCoreApplication::processEvents();
			require(!control<QCheckBox>(window, Vpv::ObjectName("camera.camera_use_dejudder"))->isEnabled() &&
				control<QCheckBox>(window, Vpv::ObjectName("camera.camera_use_dejudder"))->isChecked(),
				"model off greys the switch and keeps its stored value");
			control<QCheckBox>(window, Vpv::ObjectName("camera.camera_model"))->setChecked(true);
			control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
			QCoreApplication::processEvents();
			require(!control<QCheckBox>(window, Vpv::ObjectName("camera.camera_model"))->isEnabled() &&
				!control<QCheckBox>(window, Vpv::ObjectName("camera.camera_use_grain"))->isEnabled() &&
				!control<QComboBox>(window, Vpv::ObjectName("camera.camera_model_overlay"))->isEnabled(),
				"the Original pathway greys the whole group");
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
			for (const char* unit : { "input", "motion", "repair", "scaling", "colour", "output" })
				require(!control<QLabel>(window, Vpv::ObjectName(QString::fromLatin1(unit) + QStringLiteral(".more")))->text().isEmpty(),
					(std::string("each unit names where the rest of its settings are: ") + unit).c_str());
			require(!window.findChild<QCheckBox*>(Vpv::ObjectName("output.planned.calibration_lut_stage")),
				"the LUT stage is not listed as not yet implemented (the expert tab sets it)");
			// The candidates are listed per group in plain words (no key names, no docs paths), each with its page.
			QString list;
			for (const char* group : { "A", "B", "C", "D" })
				list += control<QLabel>(window, Vpv::ObjectName(QStringLiteral("retirement.group.") + QString::fromLatin1(group) + QStringLiteral(".list")))->text();
			// CC-GEM P2 + Unit 3 (Phase 2): capture_display_fixes sits on Capture fixes (under test) inside Developer,
			// and magewell_recover_faults sits on System > Capture bridges; the list names those tabs/cards for them.
			require(list.contains(QStringLiteral("VP-V &gt; Proposed retirement")) &&
				list.contains(QStringLiteral("VP-V &gt; Developer")) &&
				list.contains(QStringLiteral("Capture fixes (under test)")) &&
				list.contains(QStringLiteral("VP-V &gt; System")) &&
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
		Vpv::RegisterConfigProviders();
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
				else if (item.kind == Vpv::ItemKind::Text)
					w = window.findChild<QLineEdit*>(Vpv::ObjectName(QString::fromLatin1(card.unit) + QLatin1Char('.') + QString::fromLatin1(item.id)));
				else if (item.kind != Vpv::ItemKind::Hide && item.kind != Vpv::ItemKind::Note && item.kind != Vpv::ItemKind::Elsewhere)
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
		// Repair Upper is planned (owner Q6 / GEM-057); Mid (2) switches VP deband on.
		control<QSlider>(window, Vpv::ObjectName("repair.tier"))->setValue(2);
		QCoreApplication::processEvents();
		require(control<QComboBox>(window, QStringLiteral("config.vprenderer.deband_vp"))->currentData().toString() == QStringLiteral("on"),
			"the Rendering control shows the value the tier wrote");
		// The input-policy notes.
		for (const char* note : { "note.input_policy.general", "note.input_policy.vprenderer.input_processing", "note.input_policy.directshow" })
			control<QLabel>(window, Vpv::ObjectName(QString::fromLatin1(note)));
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
		QCoreApplication::processEvents();
		require(!RowShown(debandRow), "the duplicate row stays hidden in the Original pathway (one control per key)");
		// Bill's own controls of these keys are never on the VP-V page. v1.4.02: Bill retired `quality` (no
		// control) and moved debanding, dithering and dither depth to his Scaling page. Two of his keys also
		// have a VP-V control of their own on the VP-V page under another object name (P8 step 2: `.vpv`, e.g.
		// config.vprenderer.scaling.dithering.vpv, chroma_upscaler.vpv); this checks Bill's, by his name.
		for (const char* key : { "scaling.deband_strength", "scaling.dithering", "scaling.display_bit_depth", "tone_mapping", "gamut_mapping",
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
	// P8-3 (2026-11-11): the placement table is ONE table (Organisation()). A key sits on one card only and a
	// Section id on one card only; the Motion unit's de-judder slider rows, estimation rows and intent rows
	// are on Motion cards. tools/check_config_placement.py holds the module README tables to the same table.
	void testPlacementTable()
	{
		std::map<std::string, std::string> keyCard, sectionCard;
		for (const Vpv::Card& card : Vpv::Organisation())
		{
			const std::string where = std::string(card.unit) + "/" + card.id;
			for (const Vpv::CardItem& item : card.items)
			{
				if (item.kind == Vpv::ItemKind::Section)
				{
					const auto inserted = sectionCard.emplace(item.id, where);
					require(inserted.second, ("a section is on one card only: " + std::string(item.id)).c_str());
				}
				else if (item.kind != Vpv::ItemKind::Hide && item.kind != Vpv::ItemKind::Note && item.kind != Vpv::ItemKind::Elsewhere)
				{
					const auto inserted = keyCard.emplace(item.id, where);
					require(inserted.second, ("a key is on one card only: " + std::string(item.id) + " (" + inserted.first->second + " and " + where + ")").c_str());
				}
			}
		}
		const char* const kMotionKeys[] = { "intent_motion", "intent_motion_policy", "intent_motion_min", "intent_motion_min_shot_s",
			"intent_ease_s", "dejudder_strength", "dejudder_strength_even", "dejudder_even_rates", "dejudder_object_strength",
			"dejudder_motion_rolloff", "dejudder_roll_floor", "dejudder_roll_lo", "dejudder_roll_hi", "optical_flow",
			"motion_ahead_frames", "dejudder_est_split", "dejudder_est_qpel", "dejudder_estimate_ahead", "vector_demand" };
		for (const char* key : kMotionKeys)
		{
			const auto found = keyCard.find(key);
			require(found != keyCard.end() && found->second.compare(0, 7, "motion/") == 0,
				(std::string("a motion key is on a Motion card: ") + key).c_str());
		}
		{
			const auto found = sectionCard.find("dejudder");
			require(found != sectionCard.end() && found->second.compare(0, 7, "motion/") == 0,
				"a motion section is on a Motion card: dejudder");
		}
		{
			// Unit 3 (Phase 2): measured estimator internals live under Developer.
			const auto found = sectionCard.find("dejudderEstimation");
			require(found != sectionCard.end() && found->second.compare(0, 10, "developer/") == 0,
				"the dejudderEstimation internals section is on a Developer card");
		}
		// P8-4 (2026-11-11): the source-resolution picture-slider keys sit on Scaling cards; the Fixes under test
		// keys that have no card yet (tools/check_config_placement.py FIXES_CONTAINER_STAYS) are on none.
		const char* const kSourceResolutionKeys[] = { "source_resolution", "source_resolution_auto_min_confidence",
			"source_resolution_evidence_seconds", "source_resolution_evidence_shots", "source_resolution_engage_max_wait_s",
			"source_resolution_crossfade_s", "source_resolution_release_fade_s", "source_resolution_text_guard",
			"source_resolution_mixed_guard" };
		for (const char* key : kSourceResolutionKeys)
		{
			const auto found = keyCard.find(key);
			require(found != keyCard.end() && found->second.compare(0, 8, "scaling/") == 0,
				(std::string("a source-resolution key is on a Scaling card: ") + key).c_str());
		}
		// Unit 2 (Phase 1): all 5 previously stranded Fixes-under-test keys are now placed on their proper cards
		// (FIXES_CONTAINER_STAYS is empty).
		for (const char* key : { "optical_flow_fb_check", "optical_flow_global", "optical_flow_global_fallback", "source_class" })
		{
			const auto found = keyCard.find(key);
			require(found != keyCard.end() && found->second.compare(0, 7, "motion/") == 0,
				(std::string("previously stranded motion key is now on a Motion card: ") + key).c_str());
		}
		{
			const auto found = keyCard.find("ssim_bitrate_floor");
			require(found != keyCard.end() && found->second.compare(0, 8, "scaling/") == 0,
				"previously stranded ssim_bitrate_floor is now on a Scaling card");
		}
		// P8-5 + Unit 3 (Phase 2): the adaptive scaler measured-threshold sections moved from Scaling > Advanced
		// to Developer (developer/scaling_internals).
		const char* const kAdaptiveSections[] = { "adaptiveDownscale", "adaptiveUpscale" };
		for (const char* section : kAdaptiveSections)
		{
			const auto found = sectionCard.find(section);
			require(found != sectionCard.end() && found->second.compare(0, 10, "developer/") == 0,
				(std::string("an adaptive scaler internals section is on a Developer card: ") + section).c_str());
		}
		const char* const kOutputKeys[] = { "temporal_dither", "dither_grain_tolerance_scale" };
		for (const char* key : kOutputKeys)
		{
			const auto found = keyCard.find(key);
			require(found != keyCard.end() && found->second.compare(0, 7, "output/") == 0,
				(std::string("a dither key is on an Output card: ") + key).c_str());
		}
		const auto grainGuard = keyCard.find("deband_vp_grain_guard");
		require(grainGuard != keyCard.end() && grainGuard->second.compare(0, 7, "repair/") == 0, "the deband grain guard is on a Repair card");
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

	// GEM-173: source_geometry_detect has a row in the Original pathway table, so the "Set by" line names it whatever
	// the profile holds; with the pathway off the same inputs fall through to the profile / default.
	void testSourceGeometryDetectRowShowsSetByOriginal()
	{
		Vpv::SetByQuery q;
		q.key = "source_geometry_detect";
		q.builtInDefault = "all";
		q.ownValue = "hdv_1440x1080";
		q.originalPathway = true;
		std::string original;
		require(Vpv::OriginalPathwayValue("source_geometry_detect", original) && original == "all", "the table carries the key with the value all");
		Vpv::SetByResult r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::OriginalPathway, ("Original pathway source: " + r.text).c_str());
		require(r.text == "Set by the Original pathway (VP's additions are off): All", ("Original pathway text: " + r.text).c_str());
		q.originalPathway = false;
		r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::ThisProfile && r.text == "Set by you: Hdv_1440x1080", ("pathway off, own value: " + r.text).c_str());
		q.ownValue.clear();
		r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::BuiltInDefault && r.text == "Set by VP's built-in default: All", ("pathway off, nothing set: " + r.text).c_str());
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
		// DPTR-W1/W2 (coordinator 2026-11-12 09:58; owner-decisions): the default Repair tier collapses to Classic while the tiers are identical, so the master reads (Classic).
		require(note("deband_vp") == QStringLiteral("Set by the Repair tier (Classic): Off"), ("tier-locked: " + S(note("deband_vp"))).c_str());
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
				if (id != QStringLiteral("drive"))   // Dynamic Drive has its own page; redundant stub tab removed (Unit 2)
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
		// Unit 3 (Phase 2): the global tier offers the same four clickable preset buttons (Classic / Low / Mid / Upper).
		for (const char* step : { "classic", "low", "mid", "upper" })
		{
			auto* btn = control<QPushButton>(window, Vpv::ObjectName(QStringLiteral("global_tier.btn.") + QString::fromLatin1(step)));
			require(btn->isCheckable(), (std::string("global tier preset button is not checkable: ") + step).c_str());
		}
		// A control the Original pathway greys says why (the frame analysis store).
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
		QCoreApplication::processEvents();
		auto* analysis = control<QCheckBox>(window, Vpv::ObjectName("expert.frame_analysis_store"));
		require(!analysis->isEnabled() && analysis->toolTip().contains(QStringLiteral("Greyed: the Original pathway is on")),
			"the frame analysis store is greyed in the Original pathway and says why");
		auto* resyncShadow = control<QComboBox>(window, Vpv::ObjectName("expert.resync_event"));
		require(!resyncShadow->isEnabled() && resyncShadow->toolTip().contains(QStringLiteral("Greyed: the Original pathway is on")),
			"the resync event shadow is greyed in the Original pathway and says why");
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(0);
		QCoreApplication::processEvents();
		require(analysis->isEnabled() && !analysis->toolTip().contains(QStringLiteral("Greyed")), "back on the VP-V pathway the reason is gone");
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
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);   // a round trip re-reads the tiers
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(0);
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
	// P8 step 2: the VP-V rows that left Bill's pages. The both-pathway cards exist once and stay usable on the
	// Original pathway; the reports help names the folder with a backslash; Bill's newlldv label is his again;
	// global_actions marks the document edited and is saved.
	void testBothPathwayCardsLeaveBillsPages()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir, "[general]\nrenderer: VP Renderer\n[vpv]\npipeline_original_pathway: on\n");
		ConfigEditorWindow window(path, 0, true);
		for (const char* id : { "capture_bridges", "shortcuts_reports" })
			require(window.findChildren<QWidget*>(QStringLiteral("vpv.system.card.") + QString::fromLatin1(id)).size() == 1,
				(std::string("System card not there exactly once: ") + id).c_str());
		for (const char* name : { "config.decklink.ycbcr_12bit_capture", "config.general.dv_tunnel_handoff",
				"config.general.newlldv_sdr_veto", "config.logging.auto_report", "config.logging.report_thumbnail",
				"config.logging.open_reports", "config.shortcuts.global_hotkeys", "config.shortcuts.global_actions",
				"config.general.bounded_invalid_capture_recovery" })
		{
			const QList<QWidget*> found = window.findChildren<QWidget*>(QString::fromLatin1(name));
			require(found.size() == 1, (std::string("moved control not there exactly once: ") + name).c_str());
			require(found.front()->isEnabled(), (std::string("moved control greyed on the Original pathway: ") + name).c_str());
		}
		bool folder = false;
		for (QLabel* label : window.findChildren<QLabel*>())
		{
			require(!label->text().contains(QLatin1Char('\r')), "a label holds a carriage return");
			folder = folder || label->text().contains(QStringLiteral("to logs\\reports (the newest 20"));
		}
		require(folder, "the problem-report help does not name logs\\reports");
		require(control<QCheckBox>(window, QStringLiteral("config.general.newlldv"))->text() == QStringLiteral("Alternate LLDV detection"),
			"Bill's newlldv label is not his");
		auto* apply = control<QPushButton>(window, QStringLiteral("applyConfiguration"));
		control<QLineEdit>(window, QStringLiteral("config.shortcuts.global_actions"))->setText(QStringLiteral("config_editor"));
		QCoreApplication::processEvents();
		require(apply->isEnabled(), "editing global_actions did not mark the document edited");
		save(window);
		QFile file(path);
		require(file.open(QIODevice::ReadOnly) && file.readAll().contains("global_actions: config_editor"), "global_actions not saved");
	}

	// GF-P5 S13 + GEM-037: for an ABSENT key, global_actions stays empty (so untouched saves never write it)
	// and its placeholder follows the file's pathway as the player reads it: none in the Original pathway,
	// state_report otherwise; an explicit key is always shown as written; the pathway key is found in [vpv]
	// and in [vprenderer], its value matched case-insensitively (NormalizeName).
	void testGlobalActionsDefaultFollowsThePathway()
	{
		auto globalActionsState = [](const char* fixture) {
			QTemporaryDir dir;
			const QString path = WriteFixture(dir, fixture);
			ConfigEditorWindow window(path, 0, true);
			auto* edit = control<QLineEdit>(window, QStringLiteral("config.shortcuts.global_actions"));
			return std::pair<QString, QString>{ edit->text(), edit->placeholderText() };
		};
		// A: the Original pathway, no [shortcuts]: empty text, placeholder "none".
		{
			const auto [text, placeholder] = globalActionsState("[general]\nrenderer: VP Renderer\n[vpv]\npipeline_original_pathway: on\n");
			require(text.isEmpty(), "Original pathway, key absent: global_actions text is not empty");
			require(placeholder.contains(QStringLiteral("Default: none")), "Original pathway, key absent: placeholder does not show none");
		}
		// B: no pathway key: empty text, placeholder "state_report".
		{
			const auto [text, placeholder] = globalActionsState("[general]\nrenderer: VP Renderer\n");
			require(text.isEmpty(), "no pathway key, key absent: global_actions text is not empty");
			require(placeholder.contains(QStringLiteral("Default: state_report")), "no pathway key, key absent: placeholder does not show state_report");
		}
		// C: the pathway key in [vprenderer], value spelt On.
		{
			const auto [text, placeholder] = globalActionsState("[general]\nrenderer: VP Renderer\n[vprenderer]\npipeline_original_pathway: On\n");
			require(text.isEmpty(), "Original pathway in [vprenderer] (On), key absent: global_actions text is not empty");
			require(placeholder.contains(QStringLiteral("Default: none")), "Original pathway in [vprenderer] (On): placeholder does not show none");
		}
		// D: an explicit key wins in the Original pathway.
		{
			const auto [text, placeholder] = globalActionsState("[general]\nrenderer: VP Renderer\n[vpv]\npipeline_original_pathway: on\n"
				"[shortcuts]\nglobal_actions: config_editor\n");
			(void)placeholder;
			require(text == QStringLiteral("config_editor"),
				"Original pathway, key present: global_actions is not shown as written");
		}
	}

	// CC-GEM P2 (GEM-060 / GEM-222, owner Q13): the VP rows left on Bill's General page (the two capture fixes,
	// Room lights, File playback) are on the VP-V page only; saved values load into them and edits save under the same
	// keys; the Original pathway greys the capture / display fix (Input tab) but not Magewell recovery, Room lights or
	// File playback (both pathways; MUST-C); the Room lights card carries the GEM-222 note and Colour's Room-light adaptation card names it back.
	void testGeneralPageRowsOnTheVpvPage()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir, "[general]\nrenderer: VP Renderer\nmagewell_recover_faults: false\n"
			"capture_display_fixes: false\nfile_av_offset_ms: -120\n[ambient]\nmode: manual\nroom: lit\nport: COM7\n");
		ConfigEditorWindow window(path, 0, true);
		QCoreApplication::processEvents();
		const auto ancestorNamed = [](QWidget* w, const QString& name) -> QWidget*
		{
			for (QWidget* p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget())
				if (p->objectName() == name) return p;
			return nullptr;
		};
		// Bill's General page: the scroll area around one of his own General behavior rows (SHOULD-E: "Start as Video
		// Only", [general] noui, is Bill's at a591ad3b5; renderer_failure_keep_fullscreen is a VP row and could move).
		QWidget* general = ancestorNamed(control<QWidget>(window, QStringLiteral("config.general.noui")),
			QStringLiteral("pageViewport"));
		require(general != nullptr, "Bill's General page not found");
		const char* const inputCard = "vpv.input.card.capture_fixes";
		const char* const magewellCard = "vpv.system.card.capture_bridges";
		const char* const roomCard = "vpv.system.card.room_lights";
		const char* const fileCard = "vpv.system.card.file_playback";
		const std::pair<const char*, const char*> moved[] = {
			{ "config.general.magewell_recover_faults", magewellCard }, { "config.general.capture_display_fixes", inputCard },
			{ "config.ambient.mode", roomCard }, { "config.ambient.room", roomCard }, { "config.ambient.lux_lit", roomCard },
			{ "config.ambient.lux_dark", roomCard }, { "config.ambient.hold_s", roomCard }, { "config.ambient.port", roomCard },
			{ "config.ambient.baud", roomCard }, { "config.general.file_av_offset_ms", fileCard },
			{ "config.general.file_led_holdover_ms", fileCard }, { "config.general.file_av_offset_ms.reset", fileCard } };
		for (const auto& row : moved)
		{
			const QList<QWidget*> found = window.findChildren<QWidget*>(QString::fromLatin1(row.first));
			require(found.size() == 1, (std::string("moved General row not there exactly once: ") + row.first).c_str());
			require(!general->isAncestorOf(found.front()), (std::string("row still on Bill's General page: ") + row.first).c_str());
			require(ancestorNamed(found.front(), QString::fromLatin1(row.second)) != nullptr,
				(std::string("row not on its VP-V card: ") + row.first + " -> " + row.second).c_str());
			require(ancestorNamed(found.front(), Vpv::ObjectName(QStringLiteral("section"))) != nullptr,
				(std::string("row not on the VP-V page: ") + row.first).c_str());
		}
		// Saved values load into the moved controls.
		auto* magewell = control<QCheckBox>(window, QStringLiteral("config.general.magewell_recover_faults"));
		auto* displayFixes = control<QCheckBox>(window, QStringLiteral("config.general.capture_display_fixes"));
		require(!magewell->isChecked() && !displayFixes->isChecked(), "a saved capture fix off is not shown");
		require(control<QComboBox>(window, QStringLiteral("config.ambient.mode"))->currentData().toString() == QStringLiteral("manual") &&
			control<QComboBox>(window, QStringLiteral("config.ambient.room"))->currentData().toString() == QStringLiteral("lit"),
			"the saved room-light mode / room is not shown");
		require(control<QLineEdit>(window, QStringLiteral("config.ambient.port"))->text() == QStringLiteral("COM7"), "the saved sensor port is not shown");
		auto* offset = control<QSpinBox>(window, QStringLiteral("config.general.file_av_offset_ms"));
		require(offset->value() == -120, "the saved picture offset is not shown");
		// GEM-222 note and the GEM-060 cross-link (both directions).
		auto* note = control<QLabel>(window, QStringLiteral("vpv.both.room_lights.note"));
		require(note->text().contains(QStringLiteral("On Original, room lights only steer profile rules; the room-light picture adaptation is off")),
			"the Room lights card lacks the Original-pathway note");
		require(note->text().contains(QStringLiteral("Main > Room-light adaptation")), "the Room lights card does not name Colour's Room-light adaptation");
		bool backLink = false;
		for (QLabel* label : window.findChildren<QLabel*>())
			backLink = backLink || label->text().contains(QStringLiteral("set on this page's Room lights card"));
		require(backLink, "Colour's Room-light adaptation card does not name the Room lights card");
		// Edits save under the same keys.
		magewell->setChecked(true);
		control<QLineEdit>(window, QStringLiteral("config.ambient.port"))->setText(QStringLiteral("COM9"));
		offset->setValue(40);
		QCoreApplication::processEvents();
		require(control<QPushButton>(window, QStringLiteral("applyConfiguration"))->isEnabled(), "editing a moved row did not mark the document edited");
		// MUST-C (G-CC4 P2 review): each card says only what the runtime does. capture_display_fixes is off on Original
		// (CaptureDisplayFixes::Enabled); magewell_recover_faults is not gated (Bill's Magewell code, owner item).
		auto* captureNote = control<QLabel>(window, QStringLiteral("vpv.input.capture_fixes.note"));
		require(captureNote->text().contains(QStringLiteral("On the Original pathway this is off")) &&
			captureNote->text().contains(QStringLiteral("the target-only full-screen session included")),
			"the capture fixes note does not state the Original off from the start-up configuration on");
		require(!captureNote->text().contains(QStringLiteral("both are off")), "the capture fixes note still says both are off");
		auto* magewellNote = control<QLabel>(window, QStringLiteral("vpv.both.magewell_recovery.note"));
		require(magewellNote->text().contains(QStringLiteral("switching this off is PENDING")) &&
			magewellNote->text().contains(QStringLiteral("still applies on Original")),
			"the Magewell card does not say its Original switch-off is pending");
		// Original pathway: the capture / display fix is greyed; Magewell recovery, Room lights and File playback stay live.
		require(magewell->isEnabled() && displayFixes->isEnabled(), "the capture fixes are greyed on the VP-V pathway");
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
		QCoreApplication::processEvents();
		require(!displayFixes->isEnabled(), "capture_display_fixes stays live on the Original pathway");
		require(magewell->isEnabled(), "Magewell recovery is greyed on the Original pathway, but its runtime still runs there");
		for (const char* name : { "config.ambient.mode", "config.ambient.room", "config.ambient.port", "config.general.file_av_offset_ms",
				"config.general.file_led_holdover_ms" })
			require(control<QWidget>(window, QString::fromLatin1(name))->isEnabled(),
				(std::string("a both-pathway row is greyed on the Original pathway: ") + name).c_str());
		save(window);
		QFile file(path);
		require(file.open(QIODevice::ReadOnly), "the saved file cannot be read");
		const QByteArray saved = file.readAll();
		require(saved.contains("magewell_recover_faults: true") && saved.contains("capture_display_fixes: false") &&
			saved.contains("port: COM9") && saved.contains("room: lit") && saved.contains("file_av_offset_ms: 40"),
			(QByteArray("the moved General rows did not save under their keys: ") + saved).constData());
	}

	// D3: Bill's tests find Bill's own Chroma upscaler (his five filters); VP-V's list is its own control of the
	// same key, and a VP-V-only choice is written and shown on Bill's control as his loader shows it.
	void testChromaBillsControlAndVpvList()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir, "[general]\nrenderer: VP Renderer\n[vprenderer.scaling.Default]\nchroma_upscaler: lanczos\n");
		ConfigEditorWindow window(path, 0, true);
		window.selectPage(QStringLiteral("Scaling"));
		QCoreApplication::processEvents();
		require(window.findChildren<QComboBox*>(QStringLiteral("config.vprenderer.scaling.chroma_upscaler")).size() == 1,
			"more than one control is named as Bill's Chroma upscaler");
		auto* bills = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.chroma_upscaler"));
		QStringList offered;
		for (int i = 0; i < bills->count(); ++i)
			if (!bills->itemData(i).toString().isEmpty()) offered << bills->itemData(i).toString();
		require(offered == QStringList({ "ewa_lanczossharp", "ewa_lanczos", "lanczos", "bicubic", "bilinear" }),
			("Bill's Chroma upscaler does not offer his five filters: " + offered.join(QStringLiteral(","))).toStdString().c_str());
		require(bills->currentData().toString() == QStringLiteral("lanczos"), "Bill's control does not show the stored filter");
		auto* vpv = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.chroma_upscaler.vpv"));
		require(vpv->currentData().toString() == QStringLiteral("lanczos"), "VP-V's list does not show the stored filter");
		const int catmull = vpv->findData(QStringLiteral("catmull_rom"));
		require(catmull >= 0, "VP-V's list lacks Catmull-Rom");
		vpv->setCurrentIndex(catmull);
		emit vpv->activated(catmull);
		QCoreApplication::processEvents();
		require(bills->currentData().toString() == QStringLiteral("catmull_rom"), "Bill's control does not follow VP-V's choice");
		save(window);
		QFile file(path);
		require(file.open(QIODevice::ReadOnly) && file.readAll().contains("chroma_upscaler: catmull_rom"), "VP-V's chroma choice not saved");
	}
	// D4 (WAIVER-P8-05): Bill's Dithering control is his again in VP-V mode too - his four choices; a VP-V-only
	// value shows as his "Saved legacy method" and nothing is written - while VP-V's own control shows the value.
	void testBillsDitheringStaysHis()
	{
		for (const char* mode : { "ordered_lut", "error_diffusion_stucki", "blue_noise" })
		{
			QTemporaryDir dir;
			const std::string text = std::string("[general]\nrenderer: VP Renderer\n[vprenderer.scaling.Default]\ndithering: ") + mode + "\n";
			const QString path = WriteFixture(dir, text.c_str());
			ConfigEditorWindow window(path, 0, true);
			window.selectPage(QStringLiteral("Scaling"));
			QCoreApplication::processEvents();
			auto* bills = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.dithering"));
			QStringList offered;
			// Offered = enabled items: his loader keeps a stored retired value as a disabled, hidden item.
			auto* model = qobject_cast<QStandardItemModel*>(bills->model());
			for (int i = 0; i < bills->count(); ++i)
				if (!bills->itemData(i).toString().isEmpty() && (!model || model->item(i)->isEnabled())) offered << bills->itemData(i).toString();
			require(offered == QStringList({ "blue_noise", "ordered_fixed", "white_noise", "off" }),
				(std::string("Bill's Dithering does not keep his four choices: ") + S(offered.join(QStringLiteral(",")))).c_str());
			auto* vpv = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.dithering.vpv"));
			require(vpv->currentData().toString() == QString::fromLatin1(mode), (std::string("VP-V's Dithering does not show the stored ") + mode).c_str());
			if (QString::fromLatin1(mode) == QStringLiteral("blue_noise"))
				require(bills->currentData().toString() == QStringLiteral("blue_noise"), "Bill's control does not show his own value");
			else
				require(bills->currentIndex() < 0 && bills->placeholderText().startsWith(QStringLiteral("Saved legacy method")),
					(std::string("Bill's control does not show a VP-V value as his legacy method: ") + mode).c_str());
			require(control<QLabel>(window, QStringLiteral("vpv.output.dither.note"))->text().contains(QStringLiteral("legacy method")),
				"the VP-V dither card does not explain Bill's legacy-method display");
			QFile file(path);
			require(file.open(QIODevice::ReadOnly) && file.readAll() == QByteArray(text.c_str()), "opening Config wrote the file");
		}
	}
	// Audit P8-2: the Profile use card is on the VP-V page (On both pathways) and its picker follows Bill's
	// Queue list both ways; Bill -> VP-V chroma sync, Reset on both controls, and VP-V's dither Auto shown on
	// Bill's control with his own mapping (Blue noise).
	void testProfileUseAndSyncDirections()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir, "[general]\nrenderer: VP Renderer\n[queue.Normal]\nqueue_size: 32\ntarget_frames: 3\n"
			"[queue.Game]\nqueue_size: 2\ntarget_frames: 1\n[vprenderer.scaling.Default]\nchroma_upscaler: lanczos\ndithering: off\n"
			"[vprenderer.scaling.Named]\nchroma_upscaler: bicubic\n");
		ConfigEditorWindow window(path, 0, true);
		auto* section = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section")));
		auto* card = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("system.card.profile_rules")));
		require(section->isAncestorOf(card), "the queue Profile use card is not on the VP-V page");
		require(section->isAncestorOf(control<QWidget>(window, QStringLiteral("config.queue.rate_rule"))), "the queue rate rule is not on the VP-V page");
		auto* picker = control<QComboBox>(window, Vpv::ObjectName(QStringLiteral("both.queue.profile")));
		auto* queues = control<QListWidget>(window, QStringLiteral("config.queue.profiles"));
		require(picker->count() == queues->count() && picker->count() == 2, "the picker does not list Bill's queue profiles");
		queues->setCurrentRow(1);
		QCoreApplication::processEvents();
		require(picker->currentIndex() == 1, "the picker does not follow Bill's queue list");
		emit picker->activated(0);
		QCoreApplication::processEvents();
		require(queues->currentRow() == 0, "the picker does not select the profile on Bill's list");

		window.selectPage(QStringLiteral("Scaling"));
		QCoreApplication::processEvents();
		auto* bills = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.chroma_upscaler"));
		auto* vpv = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.chroma_upscaler.vpv"));
		const int bicubic = bills->findData(QStringLiteral("bicubic"));
		bills->setCurrentIndex(bicubic);
		emit bills->activated(bicubic);
		QCoreApplication::processEvents();
		require(vpv->currentData().toString() == QStringLiteral("bicubic"), "VP-V's chroma list does not follow Bill's choice");
		vpv->setCurrentIndex(vpv->findData(QStringLiteral("catmull_rom")));
		QCoreApplication::processEvents();
		require(bills->currentData().toString() == QStringLiteral("catmull_rom"), "Bill's control does not follow a set on VP-V's list");
		auto* vpvReset = control<QPushButton>(window, QStringLiteral("config.vprenderer.scaling.chroma_upscaler.vpv.reset_override"));
		require(vpvReset->isEnabled(), "VP-V's chroma Reset is not enabled after an edit");
		vpvReset->click();
		QCoreApplication::processEvents();
		require(!vpv->currentData().toString().isEmpty(), "VP-V's chroma Reset left no value");
		require(bills->currentData().toString().compare(vpv->currentData().toString(), Qt::CaseInsensitive) == 0,
			"Bill's control does not follow VP-V's Reset");
		int added = 0;
		for (int i = 0; i < bills->count(); ++i)
			if (bills->itemData(i, Qt::UserRole + 41).toBool()) ++added;
		require(added == 0 || bills->itemData(bills->currentIndex(), Qt::UserRole + 41).toBool(), "a stale VP-V item stays in Bill's list");

		auto* billsDither = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.dithering"));
		auto* vpvDither = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.dithering.vpv"));
		const int automatic = vpvDither->findData(QStringLiteral("AUTO"));
		require(automatic >= 0, "VP-V's dither list lacks Auto");
		vpvDither->setCurrentIndex(automatic);
		emit vpvDither->activated(automatic);
		QCoreApplication::processEvents();
		require(billsDither->currentData().toString() == QStringLiteral("blue_noise") && billsDither->placeholderText().isEmpty(),
			"Bill's control does not show VP-V's Auto with his mapping (Blue noise)");
		auto* billsReset = control<QPushButton>(window, QStringLiteral("config.vprenderer.scaling.dithering.reset_override"));
		require(billsReset->isEnabled(), "Bill's Dithering Reset is not enabled for a set value");
		billsReset->click();
		QCoreApplication::processEvents();
		require(billsDither->currentIndex() >= 0, "Bill's Dithering Reset left no value");
		require(vpvDither->findData(billsDither->currentData().toString(), Qt::UserRole, Qt::MatchFixedString) == vpvDither->currentIndex(),
			"VP-V's dither control does not follow Bill's Reset");
	}
	// Row c (RESTORED-CFG-04, WAIVER-C-1): VP-V's queue rows sit on the VP-V card "Queue target and low latency",
	// bound to the queue profile selected on Bill's list; Bill's Queue form is his a591ad3b form again. VP-V's
	// look-ahead (.vpv) shows an absent key, a stored -1 or "auto" as Auto while Bill's 0..8 control shows 0 the
	// way his loader does, and opening writes nothing. (A stored -1 / "auto" is outside Bill's queue schema, 0..8,
	// so Apply refuses such a file before and after row c; Auto is saved as an absent key.) Opening and saving
	// untouched keeps every stored value (Bill's displayed 0 is never written back); the two controls follow each
	// other both ways; an explicit 0 shows the notice.
	// SID r2 + Unit 2 (Phase 1): source_geometry_detect lives on Scaling > Advanced > Re-base timing as a mode
	// dropdown (vpv.scaling.source_geometry_detect.mode) plus a conditional custom IDs QLineEdit
	// (vpv.scaling.source_geometry_detect, rule 32: no row on Bill's pages).
	void testSourceGeometryDetectRowOnRebaseCard()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir, "[vprenderer]\nsdr_target_nits: 100\n"
			"[vprenderer.Main]\nsource_geometry_detect: conventional\n"
			"[vprenderer.Cinema]\nquality: high\n");
		const QString name = Vpv::ObjectName(QStringLiteral("scaling.source_geometry_detect"));
		const QString modeName = Vpv::ObjectName(QStringLiteral("scaling.source_geometry_detect.mode"));
		{
			ConfigEditorWindow window(path, 0, true);
			auto* card = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("scaling.card.rebase_timing.body")));
			auto* scalingMain = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("scaling.main")));
			auto* scalingAdv = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("scaling.advanced")));
			auto* mode = control<QComboBox>(window, modeName);
			auto* edit = control<QLineEdit>(window, name);
			require(window.findChildren<QLineEdit*>(name).size() == 1, "the source_geometry_detect custom edit is not there exactly once");
			require(scalingAdv->isAncestorOf(card) && card->isAncestorOf(mode) && card->isAncestorOf(edit),
				"source_geometry_detect mode + custom edit must sit on Scaling > Advanced > Re-base timing");
			require(!scalingMain->isAncestorOf(mode) && !scalingMain->isAncestorOf(edit),
				"source_geometry_detect must not sit on Scaling > Main");
			require(window.findChild<QWidget*>(QStringLiteral("config.vprenderer.source_geometry_detect")) == nullptr,
				"a source_geometry_detect row is on Bill's Rendering page");
			const int conventionalIdx = mode->findData(QStringLiteral("conventional"));
			const int customIdx = mode->findData(QStringLiteral("custom"));
			const int unsetIdx = mode->findData(QString());
			const int allIdx = mode->findData(QStringLiteral("all"));
			require(conventionalIdx >= 0 && customIdx >= 0 && (unsetIdx >= 0 || allIdx >= 0),
				"source_geometry_detect mode combo must offer default/all, conventional, and custom");
			auto* profile = control<QComboBox>(window, Vpv::ObjectName("profile"));
			profile->setCurrentIndex(profile->findData(QStringLiteral("vprenderer.Main")));
			QCoreApplication::processEvents();
			require(mode->currentData().toString() == QStringLiteral("conventional") && edit->isHidden(),
				"conventional profile selects conventional mode and hides custom edit");
			profile->setCurrentIndex(profile->findData(QStringLiteral("vprenderer.Cinema")));
			QCoreApplication::processEvents();
			require((mode->currentData().toString().isEmpty() || mode->currentData().toString() == QStringLiteral("all")) && edit->isHidden(),
				"unset profile selects default/all mode and hides custom edit");
			mode->setCurrentIndex(customIdx);
			QCoreApplication::processEvents();
			require(!edit->isHidden() && edit->isEnabled(), "custom mode shows and enables the custom IDs edit");
			edit->setText(QStringLiteral("hdv_1440x1080, 1920x1080"));
			emit edit->editingFinished();
			QCoreApplication::processEvents();
			profile->setCurrentIndex(profile->findData(QStringLiteral("vprenderer.Main")));
			QCoreApplication::processEvents();
			mode->setCurrentIndex(unsetIdx >= 0 ? unsetIdx : allIdx);
			QCoreApplication::processEvents();
			require(edit->isHidden(), "switching back to default/all hides the custom IDs edit");
			save(window);
		}
		ConfigFile config;
		require(config.Load(path.toStdString()), "reload");
		std::string v;
		require(config.TryGetString("vprenderer.cinema", "source_geometry_detect", v) && v == "hdv_1440x1080, 1920x1080",
			("the row does not save the picked profile's key\n" + FileText(path)).c_str());
		require(!config.TryGetString("vprenderer.main", "source_geometry_detect", v), "resetting mode to default/all does not remove the key");
		require(!config.TryGetString("vprenderer", "source_geometry_detect", v), "the row wrote the base section");
		requireVpReads(path);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* profile = control<QComboBox>(window, Vpv::ObjectName("profile"));
			profile->setCurrentIndex(profile->findData(QStringLiteral("vprenderer.Cinema")));
			QCoreApplication::processEvents();
			auto* mode = control<QComboBox>(window, modeName);
			auto* edit = control<QLineEdit>(window, name);
			require(mode->currentData().toString() == QStringLiteral("custom") && !edit->isHidden() &&
				edit->text() == QStringLiteral("hdv_1440x1080, 1920x1080"),
				"saved custom source_geometry_detect value does not reopen in custom mode");
		}
	}

	// GEM-032 / GEM-049 (Gemini's tests): on the Original pathway the By frames / By time choice is off with Target
	// delay, and a click on it writes nothing to target_latency_ms; on the VP-V pathway it stays live.
	void testQueueTargetModeOffInOriginal()
	{
		const char* text = "[general]\nrenderer: VP Renderer\n[queue.Normal]\nqueue_size: 32\ntarget_frames: 4\n";
		const auto modeButtons = [](ConfigEditorWindow& window) {
			return std::make_pair(control<QRadioButton>(window, QStringLiteral("config.queue.target_mode.frames")),
				control<QRadioButton>(window, QStringLiteral("config.queue.target_mode.time")));
		};
		{
			// Opened on the Original pathway.
			QTemporaryDir dir;
			const QString path = WriteFixture(dir, (std::string(text) + "[vpv]\npipeline_original_pathway: on\n").c_str());
			ConfigEditorWindow window(path, 0, true);
			QCoreApplication::processEvents();
			auto* latency = control<QSpinBox>(window, QStringLiteral("config.queue.target_latency_ms"));
			const auto buttons = modeButtons(window);
			require(!control<QWidget>(window, QStringLiteral("config.queue.target_mode"))->isEnabled() &&
				!buttons.first->isEnabled() && !buttons.second->isEnabled(), "By frames / By time is live on the Original pathway");
			require(!latency->isEnabled(), "Target delay is live on the Original pathway");
			const int before = latency->value();
			buttons.second->click();
			QCoreApplication::processEvents();
			require(latency->value() == before, "a By time click on the Original pathway changed target_latency_ms");
		}
		{
			// Opened on the VP-V pathway, then switched to the Original pathway on the page.
			QTemporaryDir dir;
			const QString path = WriteFixture(dir, text);
			ConfigEditorWindow window(path, 0, true);
			QCoreApplication::processEvents();
			const auto buttons = modeButtons(window);
			require(buttons.first->isEnabled() && buttons.second->isEnabled(), "By frames / By time is not live on the VP-V pathway");
			control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
			QCoreApplication::processEvents();
			require(!buttons.first->isEnabled() && !buttons.second->isEnabled(), "switching to the Original pathway leaves By frames / By time live");
			auto* latency = control<QSpinBox>(window, QStringLiteral("config.queue.target_latency_ms"));
			const int before = latency->value();
			buttons.second->click();
			QCoreApplication::processEvents();
			require(latency->value() == before, "a By time click after switching to the Original pathway changed target_latency_ms");
		}
	}

	// GEM-031 (VpvSetBy.h): on the Original pathway chroma_upscaler reads "Set by the Original pathway" only when the
	// override applies (unset or a VP-only value); a saved one of Bill's five filters is the profile's own value.
	void testSetByChromaOnOriginal()
	{
		Vpv::SetByQuery q;
		q.originalPathway = true;
		q.key = "chroma_upscaler";
		q.builtInDefault = "auto";
		Vpv::SetByResult r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::OriginalPathway && r.text.find(": Bicubic") != std::string::npos, ("unset chroma: " + r.text).c_str());
		for (const char* vp : { "catmull_rom", "upscaler", "auto" })
		{
			q.ownValue = vp;
			r = Vpv::SetBy(q);
			require(r.source == Vpv::ValueSource::OriginalPathway, (std::string("VP-only chroma ") + vp + ": " + r.text).c_str());
		}
		for (const char* bill : { "ewa_lanczossharp", "ewa_lanczos", "lanczos", "bicubic", "bilinear" })
		{
			q.ownValue = bill;
			r = Vpv::SetBy(q);
			require(r.source == Vpv::ValueSource::ThisProfile, (std::string("Bill's chroma ") + bill + ": " + r.text).c_str());
		}
		q.ownValue.clear();
		q.defaultProfileOnScreen = false;
		q.inheritedValue = "lanczos";
		r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::DefaultProfile, ("inherited Bill chroma: " + r.text).c_str());
		q.originalPathway = false;
		q.inheritedValue.clear();
		q.ownValue = "catmull_rom";
		q.defaultProfileOnScreen = true;
		r = Vpv::SetBy(q);
		require(r.source == Vpv::ValueSource::ThisProfile, ("VP-V pathway chroma: " + r.text).c_str());
	}

	void testQueueRowsOnTheVpvCard()
	{
		const auto readFile = [](const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll().replace("\r\n", "\n") : QByteArray(); };
		const auto selectQueue = [](QListWidget* queues, const QString& name)
		{
			for (int row = 0; row < queues->count(); ++row)
				if (queues->item(row)->data(Qt::UserRole).toString().compare(name, Qt::CaseInsensitive) == 0)
				{
					queues->setCurrentRow(row);
					QCoreApplication::processEvents();
					return true;
				}
			return false;
		};
		{
			QTemporaryDir dir;
			const char* text = "[general]\nrenderer: VP Renderer\n[queue.Normal]\nqueue_size: 32\ntarget_frames: 4\n"
				"[queue.Minus]\nqueue_size: 16\nactive_picture_lookahead_frames: -1\n"
				"[queue.Word]\nqueue_size: 16\nactive_picture_lookahead_frames: auto\n"
				"[queue.Three]\nqueue_size: 16\nactive_picture_lookahead_frames: 3\n";
			const QString path = WriteFixture(dir, text);
			ConfigEditorWindow window(path, 0, true);
			auto* section = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section")));
			// GEM-049: the card is on the Input unit (off on the Original pathway), not "On both pathways".
			auto* card = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("input.card.queue_target")));
			require(section->isAncestorOf(card), "the queue target card is not on the VP-V page");
			require(window.findChildren<QWidget*>(Vpv::ObjectName(QStringLiteral("both.card.queue_target"))).isEmpty(),
				"the queue target card is still under On both pathways");
			for (const char* name : { "config.queue.low_latency_path", "config.queue.target_latency_ms", "config.queue.target_latency_note",
					"config.queue.target_mode", "config.queue.target_lookahead_note", "config.queue.active_picture_lookahead_frames.vpv",
					"config.queue.active_picture_lookahead_notice" })
			{
				require(window.findChildren<QWidget*>(QString::fromLatin1(name)).size() == 1, (std::string("not there exactly once: ") + name).c_str());
				require(card->isAncestorOf(control<QWidget>(window, QString::fromLatin1(name))), (std::string("not on the VP-V queue card: ") + name).c_str());
			}
			auto* bills = control<QSpinBox>(window, QStringLiteral("config.queue.active_picture_lookahead_frames"));
			auto* vpv = control<QSpinBox>(window, QStringLiteral("config.queue.active_picture_lookahead_frames.vpv"));
			require(!card->isAncestorOf(bills) && !card->isAncestorOf(control<QSpinBox>(window, QStringLiteral("config.queue.target_frames"))),
				"Bill's look-ahead or Target frames left his Queue page");
			require(bills->minimum() == 0 && bills->maximum() == 8 && bills->specialValueText().isEmpty(),
				"Bill's look-ahead is not his plain 0..8 control");
			require(vpv->minimum() == -1 && vpv->maximum() == 8 && vpv->specialValueText() == QStringLiteral("Auto"), "VP-V's look-ahead has no Auto");
			auto* queues = control<QListWidget>(window, QStringLiteral("config.queue.profiles"));
			auto* picker = control<QComboBox>(window, QStringLiteral("vpv.both.queue_target.profile"));
			auto* notice = control<QLabel>(window, QStringLiteral("config.queue.active_picture_lookahead_notice"));
			for (const char* name : { "queue.normal", "queue.minus", "queue.word" })
			{
				require(selectQueue(queues, QString::fromLatin1(name)), (std::string("profile missing: ") + name).c_str());
				require(picker->currentIndex() == queues->currentRow(), (std::string("the card's picker does not follow Bill's list: ") + name).c_str());
				require(vpv->value() == -1 && vpv->text() == QStringLiteral("Auto"), (std::string("VP-V's look-ahead does not show Auto: ") + name).c_str());
				require(bills->value() == 0, (std::string("Bill's control does not show Auto as his 0: ") + name).c_str());
				require(!notice->isVisibleTo(card), "the explicit-0 notice shows for Auto");
			}
			require(selectQueue(queues, QStringLiteral("queue.three")) && vpv->value() == 3 && bills->value() == 3,
				"an explicit value is not shown on both controls");
			require(readFile(path) == QByteArray(text), "opening Config wrote the file");
		}
		QTemporaryDir dir;
		const char* text = "[general]\nrenderer: VP Renderer\n[queue.Normal]\nqueue_size: 32\ntarget_frames: 4\n"
			"[queue.Zero]\nqueue_size: 16\nactive_picture_lookahead_frames: 0\n"
			"[queue.Three]\nqueue_size: 16\nactive_picture_lookahead_frames: 3\n";
		const QString path = WriteFixture(dir, text);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* card = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("input.card.queue_target")));
			auto* queues = control<QListWidget>(window, QStringLiteral("config.queue.profiles"));
			auto* vpv = control<QSpinBox>(window, QStringLiteral("config.queue.active_picture_lookahead_frames.vpv"));
			auto* notice = control<QLabel>(window, QStringLiteral("config.queue.active_picture_lookahead_notice"));
			require(selectQueue(queues, QStringLiteral("queue.zero")) && vpv->value() == 0 && notice->isVisibleTo(card),
				"an explicit 0 does not show 0 and the notice on VP-V's control");
			require(selectQueue(queues, QStringLiteral("queue.normal")) && vpv->value() == -1 && !notice->isVisibleTo(card),
				"Auto after an explicit 0 keeps the notice");
			save(window);
			const QByteArray saved = readFile(path);
			for (const char* part : { "[queue.Normal]\nqueue_size: 32\ntarget_frames: 4\n", "[queue.Zero]\nqueue_size: 16\nactive_picture_lookahead_frames: 0\n",
					"[queue.Three]\nqueue_size: 16\nactive_picture_lookahead_frames: 3\n" })
				require(saved.contains(part), (std::string("saving untouched changed a queue profile: ") + part + " in " + saved.toStdString()).c_str());
			require(saved.count("active_picture_lookahead_frames") == 2, "saving untouched wrote Bill's displayed 0 for the Auto profile");
		}
		{
			ConfigEditorWindow window(path, 0, true);
			auto* bills = control<QSpinBox>(window, QStringLiteral("config.queue.active_picture_lookahead_frames"));
			auto* vpv = control<QSpinBox>(window, QStringLiteral("config.queue.active_picture_lookahead_frames.vpv"));
			require(selectQueue(control<QListWidget>(window, QStringLiteral("config.queue.profiles")), QStringLiteral("queue.three")), "Three missing");
			bills->setValue(5);
			QCoreApplication::processEvents();
			require(vpv->value() == 5, "VP-V's look-ahead does not follow Bill's control");
			vpv->setValue(2);
			QCoreApplication::processEvents();
			require(bills->value() == 2, "Bill's control does not follow VP-V's look-ahead");
			vpv->setValue(-1);
			QCoreApplication::processEvents();
			require(bills->value() == 0 && vpv->value() == -1, "VP-V's Auto does not show as 0 on Bill's control");
			// A buffering preset keeps VP-V's look-ahead Auto (the key removed) and switches the delay off.
			vpv->setValue(4);
			QCoreApplication::processEvents();
			auto* policy = control<QComboBox>(window, QStringLiteral("config.queue.policy_preset"));
			policy->setCurrentIndex(policy->findData(QStringLiteral("reliable")));
			QCoreApplication::processEvents();
			require(vpv->value() == -1 && bills->value() == 0 && control<QSpinBox>(window, QStringLiteral("config.queue.target_latency_ms"))->value() == 0,
				"a preset left an explicit look-ahead or a delay on the VP-V card");
			save(window);
			const QByteArray saved = readFile(path);
			require(saved.contains("[queue.Three]\n") && saved.contains("target_latency_ms: 0") && saved.count("active_picture_lookahead_frames") == 1 &&
				saved.contains("[queue.Zero]\nqueue_size: 16\nactive_picture_lookahead_frames: 0\n"),
				(std::string("VP-V's Auto did not remove the key, or another profile's value changed: ") + saved.toStdString()).c_str());
		}
	}

	// P9 row a: the VP-only load-time migrations (editor/Migrations.h) moved out of ConfigEditorWindow.cpp byte for
	// byte; runLoadMigrations (Bill's calls, the VPV-HOOK line) keeps their order. This opens one old-style file that
	// needs four of them and checks the saved result: the old renderer name, queue ownership folded into the first
	// queue profile, the calibration LUT path separators, and the input policy keys moved to their new section.
	void testLoadMigrationsMoved()
	{
		QTemporaryDir dir;
		const char* text = "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n[queue_recovery]\nreset_after_render_restart_seconds: 7\n"
			"[queue.Normal]\nqueue_size: 32\n[vprenderer]\nvideo_conversion: auto\n"
			"[vprenderer.color.Default]\ncalibration_lut_bt709: C:\\luts\\a.cube\n";
		const QString path = WriteFixture(dir, text);
		{
			ConfigEditorWindow window(path, 0, true);
			save(window);
		}
		QFile file(path);
		require(file.open(QIODevice::ReadOnly), "cannot read the migrated file");
		const QByteArray saved = file.readAll().replace("\r\n", "\n");
		require(!saved.contains("VideoProcessor Renderer (Alpha)") && saved.contains("renderer: VP Renderer\n"),
			(std::string("the old renderer name was not migrated: ") + saved.toStdString()).c_str());
		const int queueAt = saved.indexOf("[queue.Normal]");
		const int movedAt = saved.indexOf("reset_after_render_restart_seconds: 7");
		require(queueAt >= 0 && movedAt > queueAt && saved.count("reset_after_render_restart_seconds") == 1,
			(std::string("the legacy queue key did not move into the first queue profile: ") + saved.toStdString()).c_str());
		require(saved.contains("calibration_lut_bt709: C:/luts/a.cube"),
			(std::string("the calibration LUT path kept backslashes: ") + saved.toStdString()).c_str());
		const int inputAt = saved.indexOf("[vprenderer.input_processing]");
		require(inputAt >= 0 && saved.indexOf("video_conversion: auto") > inputAt,
			(std::string("the input policy key did not move to [vprenderer.input_processing]: ") + saved.toStdString()).c_str());
	}

	// migrateRefreshKeys (moved to editor/Migrations.h) rewrites a saved integer refresh key that stands for a 1000/1001
	// rate (59 -> 59.94, 23 -> 23.976) in [ppm_correction] and [display_refresh_rate_override], leaves a whole rate
	// (60) alone, and drops the old key when its migrated spelling is already there (the explicit entry wins).
	void testRefreshKeyMigrationMoved()
	{
		QTemporaryDir dir;
		const char* text = "[general]\nrenderer: VP Renderer\n[ppm_correction]\n59: 12\n60: 3\n"
			"[display_refresh_rate_override]\n23: 24\n29: 30\n29.97: 31\n";
		const QString path = WriteFixture(dir, text);
		{
			ConfigEditorWindow window(path, 0, true);
			save(window);
		}
		QFile file(path);
		require(file.open(QIODevice::ReadOnly), "cannot read the migrated file");
		const QByteArray saved = file.readAll().replace("\r\n", "\n");
		const std::string shown = std::string(": ") + saved.toStdString();
		require(saved.contains("59.94: 12") && !saved.contains("\n59: "),
			("the ppm_correction key 59 was not rewritten as 59.94" + shown).c_str());
		require(saved.contains("\n60: 3"), ("the whole-rate key 60 was changed" + shown).c_str());
		require(saved.contains("23.976: 24") && !saved.contains("\n23: "),
			("the override key 23 was not rewritten as 23.976" + shown).c_str());
		require(saved.contains("29.97: 31") && !saved.contains("\n29: "),
			("the old key 29 was not dropped in favour of the explicit 29.97" + shown).c_str());
	}

	// Batch B (lead18 plan-motion-engine-switch.md (c) + Q3; WAIVER-B1-ENGINE-03, an addition): the motion engine is one switch
	// (vector_demand). Off greys the de-judder amount and the hiccup repair switches in place with "Used only when the motion engine
	// is on."; on frees them. The defaults (engine on) leave them usable (testMastersAndFit's motion_preset_box check).
	void testMotionEngineOffGreysDejudderAndHiccup()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		ConfigEditorWindow window(path, 0, true);
		auto* engine = control<QComboBox>(window, Vpv::ObjectName("motion.vector_demand"));
		auto* amount = control<QWidget>(window, QStringLiteral("config.vprenderer.motion_preset_box"));
		auto* hiccup = control<QComboBox>(window, Vpv::ObjectName("motion.interpolation_fill_gaps"));
		require(amount->isEnabled(), "engine on (default): the de-judder amount is usable");
		require(hiccup->isEnabled(), "engine on (default): hiccup repair is usable");
		const int off = engine->findData(QStringLiteral("off"));
		require(off >= 0, "the Motion engine switch offers off");
		engine->setCurrentIndex(off);
		QCoreApplication::processEvents();
		const QString why = QStringLiteral("Used only when the motion engine is on");
		require(!amount->isEnabled() && amount->toolTip().startsWith(why),
			("engine off greys the de-judder amount and says why: " + amount->toolTip().toStdString()).c_str());
		require(!hiccup->isEnabled() && hiccup->toolTip().startsWith(why),
			("engine off greys hiccup repair and says why: " + hiccup->toolTip().toStdString()).c_str());
		const int on = engine->findData(QStringLiteral("on"));
		require(on >= 0, "the Motion engine switch offers on");
		engine->setCurrentIndex(on);
		QCoreApplication::processEvents();
		require(amount->isEnabled() && hiccup->isEnabled(), "engine on again frees both");
	}

	// W4 profile-defaults-parity (GEM-033, 042, 043): automated parity across VpvRenderer::Fields{},
	// VpvSchemaKeys.h, VpvRenderingDefaults.h, and VpvTierTable.generated.h, plus intent_black unset button.
	void testProfileDefaultsParity()
	{
		// 1. Key membership in IsVpvOnlyKey / IsVpvChoiceKey (sorted binary search).
		require(Vpv::IsVpvOnlyKey(QStringLiteral("chroma_upscale_vp")), "chroma_upscale_vp in IsVpvOnlyKey");
		for (const char* k : { "ambient_adapt", "chroma_upscale_vp", "intent_black", "intent_display",
			"intent_motion", "sdr_contrast_recovery", "source_resolution_frame_bound" })
			require(Vpv::IsVpvChoiceKey(QString::fromLatin1(k)),
				(std::string("missing from IsVpvChoiceKey: ") + k).c_str());

		// 2. Choice defaults match VpvRenderer::Fields{} and pass VpvSchemaKeys validation.
		const VpvRenderer::Fields fields{};
		const std::vector<std::pair<const char*, std::string>> expected = {
			{ "adaptive_motion", fields.adaptive.motionStrength > 0.0f ? "on" : "off" },
			{ "chroma_upscale_vp", "off" },
			{ "fbo_float32", fields.fboFloat32 < 0 ? "auto" : (fields.fboFloat32 ? "on" : "off") },
			{ "repeat_render_reuse", fields.repeatRenderReuse ? "on" : "off" },
			{ "source_resolution_frame_bound", fields.sourceResolutionFrameBound ? "on" : "off" },
			{ "temporal_dither", fields.temporalDither },
			{ "optical_flow", fields.opticalFlow ? "on" : "off" },
			{ "optical_flow_on_demand", fields.opticalFlowOnDemand },
			{ "deband_vp", fields.debandVp },
			{ "source_resolution", fields.sourceResolution },
			{ "preserve_extended_range", fields.preserveExtendedRange ? "on" : "off" },
			{ "downscale_light", fields.downscaleLight },
			{ "upscale_light", fields.upscaleLight },
		};
		for (const auto& pair : expected)
		{
			QString def;
			require(Vpv::VpvRenderingDefault(QString::fromLatin1(pair.first), def),
				(std::string("VpvRenderingDefault missing key: ") + pair.first).c_str());
			require(def.toStdString() == pair.second,
				(std::string("VpvRenderingDefault mismatch for ") + pair.first + ": got " + def.toStdString() + ", expected " + pair.second).c_str());
			require(RendererProfileConfig::ValidateBaseSetting(pair.first, pair.second),
				(std::string("RendererProfileConfig rejected default for ") + pair.first + "=" + pair.second).c_str());
		}

		// 3. Tier table defaults agree with VpvRenderingDefault where both define a default.
		const Vpv::Table& table = Vpv::TierTable();
		for (const auto& unit : table.units)
			for (const auto& toggle : unit.toggles)
			{
				const std::string tdef = table.Default(toggle.key);
				if (tdef.empty())
					continue;
				QString rd;
				if (Vpv::VpvRenderingDefault(QString::fromLatin1(toggle.key), rd) && !rd.isEmpty())
				{
					require(rd.toStdString() == tdef,
						(std::string("Tier table default disagrees with VpvRenderingDefault for ") + toggle.key +
							": tier=" + tdef + " rendering=" + rd.toStdString()).c_str());
				}
			}

		// 4. intent_black keeps its "Not set" button (was stripped when absent from IsVpvChoiceKey) and clears overrides.
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[general]\nfullscreen: false\n[vprenderer]\nquality: high\n"
			"[vprenderer.Movie]\nwhen: source_fps <= 30\nintent_black: report\n");
		ConfigEditorWindow window(path, 0, true);
		auto* unsetBtn = window.findChild<QToolButton*>(QStringLiteral("config.vprenderer.intent_black.unset"));
		require(unsetBtn != nullptr, "config.vprenderer.intent_black.unset exists (IsVpvChoiceKey keeps item 0)");
		auto* combo = window.findChild<QComboBox*>(QStringLiteral("config.vprenderer.intent_black"));
		require(combo != nullptr && combo->findData(QString()) == 0, "intent_black keeps empty inherited item at index 0");
	}

	// CB W4-2 (config-placement-fixes: GEM-038, 039, 047, 048, 051, 052, 053, 055, 056, 057, 061, 223).
	void testConfigPlacementFixes()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[general]\nfullscreen: false\n"
			"[vprenderer]\nsdr_target_nits: 100\n"
			"[vprenderer.Cinema]\nquality: high\n"
			"[queue.Cinema]\ntarget_latency_ms: 150\n"
			"[queue]\ntarget_latency_ms: 250\n");
		ConfigEditorWindow window(path, 0, true);
		window.show();
		QCoreApplication::processEvents();

		// GEM-047 & GEM-051: toneCurves is NOT in allRaw; allRawGroup auto-hides once repeat_render_reuse is placed.
		auto* allRawContent = window.findChild<QWidget*>(QStringLiteral("rendererSection.allRaw.content"));
		auto* toneCurves = window.findChild<QWidget*>(QStringLiteral("rendererSection.toneCurves"));
		require(toneCurves != nullptr, "Bill's toneCurves section exists");
		require(!allRawContent || !allRawContent->isAncestorOf(toneCurves),
			"toneCurves is NOT swept into All raw settings (expert)");
		auto* allRawGroup = window.findChild<QWidget*>(QStringLiteral("rendererSection.allRawGroup"));
		require(allRawGroup && allRawGroup->isHidden(),
			"allRawGroup auto-hides once repeat_render_reuse is placed");
		auto* draftBody = control<QWidget>(window, Vpv::ObjectName("developer.card.output_draft.body"));
		auto* reuse = control<QComboBox>(window, QStringLiteral("config.vprenderer.repeat_render_reuse"));
		require(draftBody->isAncestorOf(reuse) && !draftBody->isEnabled() &&
			draftBody->toolTip().contains(QStringLiteral("Draft, no effect yet")),
			"repeat_render_reuse is placed under Developer > Draft (disabled, not a live control)");

		// GEM-048 + 223: click every <a href> link in vpv.<unit>.more and verify target page/tab.
		const QRegularExpression hrefRe(QStringLiteral("href=\"([^\"]+)\""));
		int linkClicks = 0;
		for (const char* unit : { "input", "motion", "repair", "scaling", "colour", "output" })
		{
			auto* more = window.findChild<QLabel*>(Vpv::ObjectName(QString::fromLatin1(unit) + QStringLiteral(".more")));
			if (!more) continue;
			auto it = hrefRe.globalMatch(more->text());
			while (it.hasNext())
			{
				const QString target = it.next().captured(1);
				const QString wantNav = target.section(QLatin1Char('|'), 0, 0);
				const QString wantTab = target.section(QLatin1Char('|'), 1, 1);
				QMetaObject::invokeMethod(more, "linkActivated", Qt::DirectConnection, Q_ARG(QString, target));
				QCoreApplication::processEvents();
				if (!wantTab.isEmpty())
				{
					auto* tabs = window.findChild<QTabBar*>(QStringLiteral("settingsSectionTabs"));
					require(tabs != nullptr && tabs->currentIndex() >= 0 &&
						tabs->tabText(tabs->currentIndex()) == wantTab,
						("more link did not open tab " + wantTab.toStdString() + " for unit " + unit).c_str());
				}
				else
				{
					auto* sidebar = window.findChild<QWidget*>(QStringLiteral("sidebar"));
					bool checked = false;
					if (sidebar)
						for (QAbstractButton* b : sidebar->findChildren<QAbstractButton*>())
							if (b->text() == wantNav && b->isChecked()) { checked = true; break; }
					require(checked, ("more link did not select sidebar page " + wantNav.toStdString()).c_str());
				}
				++linkClicks;
			}
		}
		require(linkClicks >= 9, "all cross-page more links exercised");

		// GEM-052: optical_flow_on_demand is a live QComboBox under Motion > Advanced > De-judder shaping.
		auto* shapingBody = control<QWidget>(window, Vpv::ObjectName("motion.card.shaping.body"));
		auto* ofod = control<QComboBox>(window, Vpv::ObjectName("motion.optical_flow_on_demand"));
		require(shapingBody->isAncestorOf(ofod) && ofod->isEnabled(),
			"optical_flow_on_demand is a live QComboBox under Motion > Advanced > De-judder shaping");
		require(ofod->findData(QStringLiteral("auto")) >= 0 &&
			ofod->findData(QStringLiteral("on")) >= 0 &&
			ofod->findData(QStringLiteral("off")) >= 0,
			"optical_flow_on_demand exposes auto/on/off");

		// GEM-053: source_resolution_frame_bound is a QComboBox under Scaling > Advanced > Re-base timing.
		auto* rebaseTimingBody = control<QWidget>(window, Vpv::ObjectName("scaling.card.rebase_timing.body"));
		auto* frameBound = control<QComboBox>(window, Vpv::ObjectName("scaling.source_resolution_frame_bound"));
		require(rebaseTimingBody->isAncestorOf(frameBound) &&
			frameBound->findData(QStringLiteral("on")) >= 0 &&
			frameBound->findData(QStringLiteral("off")) >= 0,
			"source_resolution_frame_bound is a QComboBox (on/off) under Scaling > Advanced > Re-base timing");
		require(!window.findChild<QLineEdit*>(Vpv::ObjectName("input.source_resolution_frame_bound")),
			"source_resolution_frame_bound is no longer a QLineEdit under Input");

		// GEM-039 + 055: chroma_upscale_vp is disabled under Scaling > Advanced (planned).
		auto* scalingAdv = control<QWidget>(window, Vpv::ObjectName("scaling.advanced"));
		auto* chromaPlanned = control<QWidget>(window, Vpv::ObjectName("scaling.card.chroma_planned.body"));
		auto* chromaVp = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.chroma_upscale_vp"));
		require(scalingAdv->isAncestorOf(chromaPlanned) && chromaPlanned->isAncestorOf(chromaVp) && !chromaVp->isEnabled(),
			"chroma_upscale_vp is disabled under Scaling > Advanced (planned)");

		// GEM-056: intent_black lives under Dynamic Drive, not Input > Readouts.
		auto* driveBlack = control<QWidget>(window, Vpv::ObjectName("drive.card.black_protection.body"));
		auto* intentBlack = control<QComboBox>(window, QStringLiteral("config.vprenderer.intent_black"));
		require(driveBlack->isAncestorOf(intentBlack),
			"intent_black moved to Dynamic Drive > Advanced > Black protection");

		// GEM-038: stale wording removed and DelaySummary prefers exact [queue] (250 ms) over [queue.Cinema] (150 ms).
		auto* lookaheadNote = control<QLabel>(window, QStringLiteral("config.queue.target_lookahead_note"));
		require(!lookaheadNote->text().contains(QStringLiteral("at least 5 frames")),
			"stale 'at least 5 frames' wording removed");
		auto* delayLabel = control<QLabel>(window, Vpv::ObjectName("input.delay"));
		require(!delayLabel->text().contains(QStringLiteral("Until the total-delay budget lands")),
			"stale 'Until the total-delay budget lands' wording removed");
		require(delayLabel->text().contains(QStringLiteral("250 ms")),
			"DelaySummary prefers exact [queue] (250 ms) over [queue.Cinema] (150 ms)");

		// GEM-061: upscale_light reunited with downscale_light on Scaling > Chroma, duplicate hidden,
		// downscale_light tooltip fixed, and cut_soft_reset locked on detail_pulse_stabilise|texture_restore.
		auto* chromaCard = control<QWidget>(window, Vpv::ObjectName("scaling.card.chroma.body"));
		auto* downLight = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.downscale_light"));
		auto* upLight = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.upscale_light"));
		require(chromaCard->isAncestorOf(downLight) && chromaCard->isAncestorOf(upLight),
			"upscale_light sits beside downscale_light in Scaling > Chroma and scaling light");
		require(!downLight->toolTip().contains(QStringLiteral("Upscaled frames always use linear light")),
			"downscale_light tooltip no longer falsely claims upscaled frames always use linear light");
		auto* dupUpLight = window.findChild<QWidget*>(QStringLiteral("config.vprenderer.upscale_light"));
		require(!dupUpLight || !RowShown(dupUpLight),
			"Bill-section duplicate upscale_light row is hidden in VP-V pathway");

		auto* softReset = control<QComboBox>(window, Vpv::ObjectName("repair.cut_soft_reset"));
		auto* detailPulse = control<QComboBox>(window, QStringLiteral("config.vprenderer.detail_pulse_stabilise"));
		auto* textureRestore = control<QComboBox>(window, QStringLiteral("config.vprenderer.texture_restore"));
		require(!softReset->isEnabled(),
			"cut_soft_reset is disabled while both detail_pulse_stabilise and texture_restore are off");
		control<QCheckBox>(window, Vpv::ObjectName("repair.card.tier_keys.custom"))->setChecked(true);
		QCoreApplication::processEvents();
		detailPulse->setCurrentIndex(detailPulse->findData(QStringLiteral("on")));
		QCoreApplication::processEvents();
		require(softReset->isEnabled(),
			"cut_soft_reset unlocks when detail_pulse_stabilise is turned on");
		detailPulse->setCurrentIndex(detailPulse->findData(QStringLiteral("off")));
		QCoreApplication::processEvents();
		require(!softReset->isEnabled(),
			"cut_soft_reset greys again when both are off");
		textureRestore->setCurrentIndex(textureRestore->findData(QStringLiteral("statistical")));
		QCoreApplication::processEvents();
		require(softReset->isEnabled(),
			"cut_soft_reset unlocks when texture_restore is turned on");
		textureRestore->setCurrentIndex(textureRestore->findData(QStringLiteral("off")));
		QCoreApplication::processEvents();

		// GEM-057: non-planned distinct tier steps stick without bouncing (and Repair Upper is planned).
		for (const Vpv::Unit& u : Vpv::TierTable().units)
		{
			if (!u.Tiered()) continue;
			auto* slider = control<QSlider>(window, Vpv::ObjectName(QString::fromLatin1(u.id) + QStringLiteral(".tier")));
			for (int step = 0; step < static_cast<int>(u.presets.size()); ++step)
			{
				// Skip steps intentionally identical to an earlier step (Repair Low == Classic until Low differs, owner Q6).
				bool dupOfEarlier = false;
				for (int prev = 0; prev < step; ++prev)
					if (Vpv::SamePresetValues(u.presets[static_cast<size_t>(prev)], u.presets[static_cast<size_t>(step)]))
						dupOfEarlier = true;
				if (dupOfEarlier) continue;
				slider->setValue(step);
				QCoreApplication::processEvents();
				require(slider->value() == step,
					(std::string("non-planned tier step did not stick: ") + u.id + " step " + std::to_string(step)).c_str());
			}
		}
	}

	// CB W4-3 (editor-state-fixes: GEM-036, 037, 040, 050 Calibration).
	void testEditorStateFixes()
	{
		Vpv::RegisterConfigProviders();

		// GEM-036: named default queue [queue.Main] with target_latency_ms greys Target frames on first open,
		// and a child queue setting its own target_frames ungreys it (QueueDelayShowsOff).
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[general]\nfullscreen: false\n"
				"[vprenderer]\nsdr_target_nits: 100\n"
				"[queue.Main]\ntarget_latency_ms: 250\n"
				"[queue.Override]\ntarget_frames: 6\n");
			ConfigEditorWindow window(path, 0, true);
			auto* targetFrames = control<QSpinBox>(window, QStringLiteral("config.queue.target_frames"));
			require(!targetFrames->isEnabled(),
				"GEM-036: named default queue with target_latency_ms greys Target frames on first open");
			auto* qList = control<QListWidget>(window, QStringLiteral("config.queue.profiles"));
			require(qList->count() >= 2, "two queue profiles loaded");
			qList->setCurrentRow(1);
			QCoreApplication::processEvents();
			require(targetFrames->isEnabled(),
				"GEM-036: child queue with own target_frames honors QueueDelayShowsOff and keeps Target frames enabled");
		}

		// GEM-037: absent [shortcuts] global_actions shows empty text + pathway placeholder and is never written on save.
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[general]\nfullscreen: false\n"
				"[vprenderer]\nsdr_target_nits: 100\n"
				"[vprenderer.Cinema]\nquality: high\n");
			{
				ConfigEditorWindow window(path, 0, true);
				auto* globalActions = control<QLineEdit>(window, QStringLiteral("config.shortcuts.global_actions"));
				require(globalActions->text().isEmpty(),
					"GEM-037: absent global_actions stays empty instead of pre-filling 'state_report'");
				require(globalActions->placeholderText().contains(QStringLiteral("state_report")),
					"GEM-037: VP-V pathway placeholder shows state_report");
				auto* pathway = control<QComboBox>(window, Vpv::ObjectName("pathway"));
				pathway->setCurrentIndex(1);   // Original
				QCoreApplication::processEvents();
				require(globalActions->text().isEmpty() &&
					globalActions->placeholderText().contains(QStringLiteral("none")),
					"GEM-037: Original pathway refreshes placeholder to none without setting text");
				// GEM-050 (Calibration): Calibration inline note shows on Original and Open Calibration stays reachable.
				auto* calNote = control<QLabel>(window, QStringLiteral("calibration.original_note"));
				require(!calNote->isHidden(),
					"GEM-050: Calibration shows inline Original-pathway note");
				auto* openCal = control<QPushButton>(window, Vpv::ObjectName("original.open_calibration"));
				require(!openCal->isHidden() && openCal->isEnabled(),
					"GEM-050: Open Calibration stays enabled when Original pathway disables content");
				pathway->setCurrentIndex(0);   // back to VP-V
				QCoreApplication::processEvents();
				require(calNote->isHidden(),
					"GEM-050: Calibration Original-pathway note hides back on VP-V");

				// GEM-040: switching Rendering profiles keeps managed controls greyed via OnBillField without derivedSetting.
				auto* rList = control<QListWidget>(window, QStringLiteral("config.vprenderer.profiles"));
				require(rList->count() >= 2, "two rendering profiles loaded");
				rList->setCurrentRow(1);
				QCoreApplication::processEvents();
				auto* upstream = control<QWidget>(window, QStringLiteral("config.vprenderer.adaptive_upscale"));
				require(!upstream->isEnabled() &&
					upstream->toolTip().startsWith(QStringLiteral("Managed by VP-V")),
					"GEM-040: Rendering profile switch preserves Managed by VP-V greying");
				require(!upstream->property("derivedSetting").toBool(),
					"GEM-040: managed greying does not set derivedSetting (preserves save-time freeze)");

				save(window);
			}
			ConfigFile saved;
			require(saved.Load(path.toStdString()), "saved config reloads");
			std::string ga;
			require(!saved.TryGetString("shortcuts", "global_actions", ga),
				"GEM-037: saving without editing never writes absent global_actions");
		}
	}

	// CB W4-5 (OPT-BOTH-PATHWAYS-TAB: GEM-058 owner Q7 + GEM-049 queue_target move to Input).
	void testBothPathwaysTabAndQueueTargetInInput()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[general]\nrenderer: VP Renderer\n"
			"[vprenderer]\nsdr_target_nits: 100\n"
			"[queue]\ntarget_latency_ms: 250\n");
		ConfigEditorWindow window(path, 0, true);
		auto* units = control<QTabWidget>(window, Vpv::ObjectName("units"));
		auto* bothTab = control<QWidget>(window, Vpv::ObjectName("both"));
		int bothIdx = -1;
		for (int i = 0; i < units->count(); ++i)
			if (units->widget(i) == bothTab) { bothIdx = i; break; }
		require(bothIdx >= 0 && units->tabText(bothIdx) == QStringLiteral("System"),
			"GEM-058 + Unit 3: 'On both pathways' cards live on the System tab of vpv.units");

		// Consolidated System cards appear once inside System (vpv.both) and never under any unit tab.
		for (const char* id : { "room_lights", "capture_bridges", "file_playback", "shortcuts_reports", "profile_rules" })
		{
			const QString name = QStringLiteral("vpv.system.card.") + QString::fromLatin1(id);
			const QList<QWidget*> found = window.findChildren<QWidget*>(name);
			require(found.size() == 1 && bothTab->isAncestorOf(found.front()),
				(std::string("GEM-058 + Unit 3: System card not solely on System tab: ") + id).c_str());
			for (const char* u : { "input", "motion", "repair", "scaling", "colour", "output" })
			{
				auto* unitPage = control<QWidget>(window, Vpv::ObjectName(QString::fromLatin1(u)));
				require(!unitPage->isAncestorOf(found.front()),
					(std::string("GEM-058 + Unit 3: System card leaked under unit tab ") + u).c_str());
			}
		}

		// GEM-049: queue_target moved from Both pathways to Input > Main.
		auto* inputTab = control<QWidget>(window, Vpv::ObjectName("input"));
		auto* queueCard = control<QWidget>(window, Vpv::ObjectName("input.card.queue_target"));
		require(inputTab->isAncestorOf(queueCard) && !bothTab->isAncestorOf(queueCard),
			"GEM-049: queue_target lives under Input, not System");

		// Proposed retirement and Pipeline (expert) moved from Overview to Developer sub-tabs (Unit 3).
		auto* devPages = control<QTabWidget>(window, Vpv::ObjectName("developer.pages"));
		auto* retirementPage = control<QWidget>(window, Vpv::ObjectName("retirement"));
		auto* expertPage = control<QWidget>(window, Vpv::ObjectName("expert"));
		bool hasRetirementTab = false;
		for (int i = 0; i < devPages->count(); ++i)
			if (devPages->tabText(i) == QStringLiteral("Proposed retirement")) hasRetirementTab = true;
		require(devPages->count() >= 2 && hasRetirementTab &&
			devPages->isAncestorOf(retirementPage) && devPages->isAncestorOf(expertPage),
			"GEM-058 + Unit 3: Developer sub-tabs (vpv.developer.pages) hold Proposed retirement and Expert");

		// Original pathway disables Input + queue_target radios while keeping Both pathways tab unlocked.
		control<QComboBox>(window, Vpv::ObjectName("pathway"))->setCurrentIndex(1);
		QCoreApplication::processEvents();
		auto* byFrames = control<QRadioButton>(window, QStringLiteral("config.queue.target_mode.frames"));
		auto* byTime = control<QRadioButton>(window, QStringLiteral("config.queue.target_mode.time"));
		auto* delaySpin = control<QSpinBox>(window, QStringLiteral("config.queue.target_latency_ms"));
		require(!inputTab->isEnabled() && !byFrames->isEnabled() && !byTime->isEnabled(),
			"GEM-049: Original pathway disables Input tab and queue target_mode radios");
		const int beforeMs = delaySpin->value();
		byFrames->click();
		QCoreApplication::processEvents();
		require(delaySpin->value() == beforeMs,
			"GEM-049: clicking disabled By frames radio in Original leaves target_latency_ms unchanged");
		require(bothTab->isEnabled() &&
			control<QWidget>(window, QStringLiteral("config.shortcuts.global_actions"))->isEnabled(),
			"GEM-058: Both pathways tab stays unlocked and editable in Original pathway");
	}

	// Rule 8 Red Team (Failure 1): cut_soft_reset and source_resolution_frame_bound are real, live, interactive
	// rule-32 controls on the VP-V page (vpv.repair.cut_soft_reset, vpv.scaling.source_resolution_frame_bound per GEM-053)
	// that round-trip edits and clears cleanly to [vprenderer.Default] without leaking duplicate QWidgets onto Bill's
	// pages or detached dummy QObject coverage aliases.
	void testRule32ProfileControlsRoundTripWithoutBillDuplicates()
	{
		auto checkNoBillDuplicatesAndAliasOwnership = [](ConfigEditorWindow& window, QWidget* cutWidget, QWidget* frameBoundWidget)
		{
			require(window.findChild<QWidget*>(QStringLiteral("config.vprenderer.cut_soft_reset")) == nullptr &&
				window.findChildren<QWidget*>(QStringLiteral("config.vprenderer.cut_soft_reset")).isEmpty(),
				"duplicate QWidget leaked onto Bill's pages for cut_soft_reset");
			require(window.findChild<QWidget*>(QStringLiteral("config.vprenderer.source_resolution_frame_bound")) == nullptr &&
				window.findChildren<QWidget*>(QStringLiteral("config.vprenderer.source_resolution_frame_bound")).isEmpty(),
				"duplicate QWidget leaked onto Bill's pages for source_resolution_frame_bound");
			const std::pair<QString, QWidget*> pairs[] = {
				{ QStringLiteral("config.vprenderer.cut_soft_reset"), cutWidget },
				{ QStringLiteral("config.vprenderer.source_resolution_frame_bound"), frameBoundWidget },
			};
			for (const auto& pair : pairs)
			{
				require(window.findChildren<QObject*>(pair.first).size() == 1,
					("expected exactly one coverage alias QObject for: " + S(pair.first)).c_str());
				for (QObject* alias : window.findChildren<QObject*>(pair.first))
				{
					require(qobject_cast<QWidget*>(alias) == nullptr,
						("coverage alias must not be a QWidget: " + S(pair.first)).c_str());
					require(alias->parent() == pair.second,
						("coverage alias must be a direct child of the live VP-V control: " + S(pair.first)).c_str());
				}
			}
		};

		QTemporaryDir dir;
		// W23 feature lock: cut_soft_reset is active while detail_pulse_stabilise or texture_restore is on.
		const QString path = WriteFixture(dir, "[general]\nrenderer: VP Renderer\n[vprenderer.Default]\ndetail_pulse_stabilise: on\n");
		const QString cutName = Vpv::ObjectName(QStringLiteral("repair.cut_soft_reset"));
		const QString frameBoundName = Vpv::ObjectName(QStringLiteral("scaling.source_resolution_frame_bound"));
		{
			ConfigEditorWindow window(path, 0, true);
			auto* section = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section")));
			require(window.findChildren<QComboBox*>(cutName).size() == 1,
				"vpv.repair.cut_soft_reset is not present exactly once");
			require(window.findChildren<QComboBox*>(frameBoundName).size() == 1,
				"vpv.scaling.source_resolution_frame_bound is not present exactly once");
			auto* cut = control<QComboBox>(window, cutName);
			auto* frameBound = control<QComboBox>(window, frameBoundName);
			require(section->isAncestorOf(cut) && cut->isEnabled(),
				"vpv.repair.cut_soft_reset is not a live control on the VP-V page");
			require(section->isAncestorOf(frameBound) && frameBound->isEnabled(),
				"vpv.scaling.source_resolution_frame_bound is not a live control on the VP-V page");
			// Baseline defaults before editing (guards against pre-baked "off" values).
			require(cut->currentData().toString().isEmpty(),
				"vpv.repair.cut_soft_reset did not start at its unset default");
			require(frameBound->currentData().toString().isEmpty(),
				"vpv.scaling.source_resolution_frame_bound did not start at its unset default");
			checkNoBillDuplicatesAndAliasOwnership(window, cut, frameBound);

			const int offIdx = cut->findData(QStringLiteral("off"));
			require(offIdx >= 0, "vpv.repair.cut_soft_reset does not offer 'off'");
			const int fbOffIdx = frameBound->findData(QStringLiteral("off"));
			require(fbOffIdx >= 0, "vpv.scaling.source_resolution_frame_bound does not offer 'off'");
			cut->setCurrentIndex(offIdx);
			frameBound->setCurrentIndex(fbOffIdx);
			QCoreApplication::processEvents();
			require(control<QPushButton>(window, QStringLiteral("applyConfiguration"))->isEnabled(),
				"editing rule-32 VP-V controls did not mark the document edited");
			save(window);
		}
		{
			ConfigFile config;
			require(config.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh), "cannot reload saved config");
			std::string v;
			require(config.TryGetString("vprenderer.default", "cut_soft_reset", v) && Vpv::SameValue(v, "off"),
				("cut_soft_reset=off was not written to [vprenderer.Default]\n" + FileText(path)).c_str());
			require(config.TryGetString("vprenderer.default", "source_resolution_frame_bound", v) && Vpv::SameValue(v, "off"),
				("source_resolution_frame_bound=off was not written to [vprenderer.Default]\n" + FileText(path)).c_str());
			require(!config.TryGetString("vprenderer", "cut_soft_reset", v),
				"cut_soft_reset leaked into the base [vprenderer] section");
			require(!config.TryGetString("vprenderer", "source_resolution_frame_bound", v),
				"source_resolution_frame_bound leaked into the base [vprenderer] section");
			requireVpReads(path);
		}
		{
			ConfigEditorWindow reopened(path, 0, true);
			auto* cut = control<QComboBox>(reopened, cutName);
			auto* frameBound = control<QComboBox>(reopened, frameBoundName);
			require(cut->currentData().toString().compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0,
				"cut_soft_reset=off did not reload into the VP-V UI");
			require(frameBound->currentData().toString().compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0,
				"source_resolution_frame_bound=off did not reload into the VP-V UI");
			checkNoBillDuplicatesAndAliasOwnership(reopened, cut, frameBound);

			// Bidirectional round-trip: reset both back to unset defaults and verify removal from disk.
			cut->setCurrentIndex(0);
			frameBound->setCurrentIndex(0);
			QCoreApplication::processEvents();
			save(reopened);
		}
		{
			ConfigFile cleared;
			require(cleared.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh), "cannot reload cleared config");
			std::string v;
			require(!cleared.TryGetString("vprenderer.default", "cut_soft_reset", v),
				("resetting cut_soft_reset to default did not remove the key\n" + FileText(path)).c_str());
			require(!cleared.TryGetString("vprenderer.default", "source_resolution_frame_bound", v),
				("resetting source_resolution_frame_bound to default did not remove the key\n" + FileText(path)).c_str());
			requireVpReads(path);
		}
	}

	// Rule 8 Red Team (Failure 3): legacy [vprenderer] pipeline_original_pathway migrates cleanly to [vpv],
	// never strands in [vprenderer.profile_1] or any other section, obeys [vpv] precedence, preserves co-located
	// renderer profile settings, and never treats a named [vprenderer.*] profile section as global [vpv].
	void testLegacyVprendererPathwayMigratesToVpvAndSurvivesSave()
	{
		auto hasNonVpvPathway = [](const ConfigFile& cfg)
		{
			for (const std::string& section : cfg.GetSectionNames())
			{
				if (ConfigFile::NormalizeName(section) == "vpv") continue;
				std::string v;
				if (cfg.TryGetString(section, "pipeline_original_pathway", v)) return true;
			}
			return false;
		};
		auto pathwayValue = [](ConfigEditorWindow& window)
		{
			return control<QComboBox>(window, Vpv::ObjectName(QStringLiteral("pathway")))->currentData().toString();
		};
		auto requireDefaultGlobalActions = [](ConfigEditorWindow& window, bool expectStateReport, const char* context)
		{
			auto* edit = control<QLineEdit>(window, QStringLiteral("config.shortcuts.global_actions"));
			require(edit->text().isEmpty(), context);
			const QString expected = expectStateReport ? QStringLiteral("Default: state_report") : QStringLiteral("Default: none");
			require(edit->placeholderText().contains(expected), context);
		};

		// Case 1: Save round-trip & zero stranding in [vprenderer] or [vprenderer.profile_1].
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[general]\nrenderer: VP Renderer\n[vprenderer]\npipeline_original_pathway: On\n");
			{
				ConfigEditorWindow window(path, 0, true);
				requireDefaultGlobalActions(window, false,
					"Case 1: legacy [vprenderer] pathway=On did not show Original global_actions default (none)");
				require(pathwayValue(window).compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0,
					"Case 1: vpv.pathway did not show Original (on)");
				require(!control<QWidget>(window, Vpv::ObjectName(QStringLiteral("content")))->isEnabled(),
					"Case 1: VP-V content was not disabled on Original pathway");
				save(window);
			}
			ConfigFile config;
			require(config.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh), "Case 1: cannot reload saved config");
			std::string v;
			require(config.TryGetString("vpv", "pipeline_original_pathway", v) &&
				QString::fromStdString(v).trimmed().compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0,
				("Case 1: [vpv] pipeline_original_pathway was not migrated to 'on'\n" + FileText(path)).c_str());
			require(!config.TryGetString("vprenderer", "pipeline_original_pathway", v),
				("Case 1: [vprenderer] retained pipeline_original_pathway\n" + FileText(path)).c_str());
			require(!config.TryGetString("vprenderer.profile_1", "pipeline_original_pathway", v),
				("Case 1: pipeline_original_pathway stranded in [vprenderer.profile_1]\n" + FileText(path)).c_str());
			require(!hasNonVpvPathway(config),
				("Case 1: pipeline_original_pathway stranded in a non-[vpv] section\n" + FileText(path)).c_str());
			requireVpReads(path);
			{
				ConfigEditorWindow reopened(path, 0, true);
				requireDefaultGlobalActions(reopened, false,
					"Case 1: reopened config lost Original global_actions default after save");
				require(pathwayValue(reopened).compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0,
					"Case 1: reopened config did not stay on Original pathway");
				require(!control<QWidget>(reopened, Vpv::ObjectName(QStringLiteral("content")))->isEnabled(),
					"Case 1: reopened VP-V content was not disabled");
			}
		}

		// Case 2: [vpv] precedence over legacy [vprenderer].
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[general]\nrenderer: VP Renderer\n[vpv]\npipeline_original_pathway: off\n[vprenderer]\npipeline_original_pathway: on\n");
			{
				ConfigEditorWindow window(path, 0, true);
				requireDefaultGlobalActions(window, true,
					"Case 2: [vpv] pipeline_original_pathway=off did not take precedence over [vprenderer]=on");
				require(pathwayValue(window).compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0,
					"Case 2: vpv.pathway did not honour [vpv] off over [vprenderer] on");
				require(control<QWidget>(window, Vpv::ObjectName(QStringLiteral("content")))->isEnabled(),
					"Case 2: VP-V content was disabled despite [vpv] off");
				save(window);
			}
			ConfigFile config;
			require(config.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh), "Case 2: cannot reload saved config");
			std::string v;
			require(config.TryGetString("vpv", "pipeline_original_pathway", v) &&
				QString::fromStdString(v).trimmed().compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0,
				("Case 2: [vpv] off was overwritten on save\n" + FileText(path)).c_str());
			require(!config.TryGetString("vprenderer", "pipeline_original_pathway", v) &&
				!config.TryGetString("vprenderer.profile_1", "pipeline_original_pathway", v) &&
				!hasNonVpvPathway(config),
				("Case 2: legacy [vprenderer] pathway leaked or stranded on save\n" + FileText(path)).c_str());
			requireVpReads(path);
			{
				ConfigEditorWindow reopened(path, 0, true);
				requireDefaultGlobalActions(reopened, true,
					"Case 2: reopened config did not preserve [vpv] precedence");
			}
		}

		// Case 3a: Mixed [vprenderer] with upscaler: lanczos + pipeline_original_pathway: On.
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[general]\nrenderer: VP Renderer\n[vprenderer]\nupscaler: lanczos\npipeline_original_pathway: On\n");
			{
				ConfigEditorWindow window(path, 0, true);
				requireDefaultGlobalActions(window, false,
					"Case 3a: mixed [vprenderer] pathway=On did not show Original global_actions default");
				require(pathwayValue(window).compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0,
					"Case 3a: mixed [vprenderer] did not select Original pathway");
				save(window);
			}
			ConfigFile config;
			require(config.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh), "Case 3a: cannot reload saved config");
			std::string v;
			require(config.TryGetString("vpv", "pipeline_original_pathway", v) &&
				QString::fromStdString(v).trimmed().compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0,
				("Case 3a: [vpv] pipeline_original_pathway was not migrated\n" + FileText(path)).c_str());
			require(!hasNonVpvPathway(config),
				("Case 3a: pipeline_original_pathway stranded outside [vpv]\n" + FileText(path)).c_str());
			bool preservedUpscaler = false;
			for (const std::string& section : config.GetSectionNames())
			{
				if (ConfigFile::NormalizeName(section).rfind("vprenderer.", 0) != 0) continue;
				std::string u;
				if (config.TryGetString(section, "upscaler", u) && Vpv::SameValue(u, "lanczos"))
					preservedUpscaler = true;
			}
			require(preservedUpscaler,
				("Case 3a: upscaler: lanczos was lost during legacy [vprenderer] migration\n" + FileText(path)).c_str());
			requireVpReads(path);
			{
				ConfigEditorWindow reopened(path, 0, true);
				requireDefaultGlobalActions(reopened, false,
					"Case 3a: reopened config lost Original global_actions default");
				reopened.selectPage(QStringLiteral("Scaling"));
				QCoreApplication::processEvents();
				require(control<QComboBox>(reopened, QStringLiteral("config.vprenderer.scaling.upscaler"))->currentData().toString()
					== QStringLiteral("lanczos"),
					"Case 3a: upscaler: lanczos did not survive into reopened Scaling UI");
			}
		}

		// Case 3b: Mixed [vprenderer] with rendering-owned sdr_target_nits: 120 + pipeline_original_pathway: On
		// (guards against a shortcut migration that drops the whole [vprenderer] section unconditionally).
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[general]\nrenderer: VP Renderer\n[vprenderer]\nsdr_target_nits: 120\npipeline_original_pathway: On\n");
			{
				ConfigEditorWindow window(path, 0, true);
				requireDefaultGlobalActions(window, false,
					"Case 3b: mixed [vprenderer] pathway=On did not show Original global_actions default");
				save(window);
			}
			ConfigFile config;
			require(config.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh), "Case 3b: cannot reload saved config");
			std::string v;
			require(config.TryGetString("vpv", "pipeline_original_pathway", v) &&
				QString::fromStdString(v).trimmed().compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0,
				("Case 3b: [vpv] pipeline_original_pathway was not migrated\n" + FileText(path)).c_str());
			require(!hasNonVpvPathway(config),
				("Case 3b: pipeline_original_pathway stranded outside [vpv]\n" + FileText(path)).c_str());
			bool preservedNits = false;
			for (const std::string& section : config.GetSectionNames())
			{
				if (ConfigFile::NormalizeName(section).rfind("vprenderer.", 0) != 0) continue;
				std::string nits;
				if (config.TryGetString(section, "sdr_target_nits", nits) && Vpv::SameValue(nits, "120"))
					preservedNits = true;
			}
			require(preservedNits,
				("Case 3b: sdr_target_nits: 120 was dropped when migrating [vprenderer] pathway\n" + FileText(path)).c_str());
			requireVpReads(path);
		}

		// Case 4 (Negative control): named profile [vprenderer.Default] must NOT be treated as global [vpv].
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[general]\nrenderer: VP Renderer\n[vprenderer.Default]\npipeline_original_pathway: on\n");
			{
				ConfigEditorWindow window(path, 0, true);
				requireDefaultGlobalActions(window, true,
					"Case 4: named [vprenderer.Default] was wrongly treated as global Original pathway");
				require(pathwayValue(window).compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0,
					"Case 4: vpv.pathway wrongly read pipeline_original_pathway from [vprenderer.Default]");
				require(control<QWidget>(window, Vpv::ObjectName(QStringLiteral("content")))->isEnabled(),
					"Case 4: VP-V content was wrongly disabled by [vprenderer.Default]");
				save(window);
			}
			ConfigFile config;
			require(config.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh), "Case 4: cannot reload saved config");
			std::string v;
			require(!config.TryGetString("vpv", "pipeline_original_pathway", v),
				("Case 4: named [vprenderer.Default] key was wrongly promoted to [vpv]\n" + FileText(path)).c_str());
			{
				ConfigEditorWindow reopened(path, 0, true);
				requireDefaultGlobalActions(reopened, true,
					"Case 4: reopened config wrongly switched away from state_report");
			}
		}
	}

	// Rule 8 Red Team (W1R-FIX5-2): VP-V styles are hoisted ONCE onto qApp with strictly VP-V-scoped property
	// selectors; per-widget stylesheets on vpv.section, vpv.units, cards, and setby labels stay empty; a second
	// window does not grow qApp->styleSheet(); and Bill's General and Rendering pages carry zero VP-V style tags.
	void testHoistedStylesStaySingleSheetAndIsolatedFromBillPages()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir, "[general]\nrenderer: VP Renderer\n[vprenderer.Default]\n");
		require(qApp != nullptr, "QApplication instance is missing");

		qsizetype sheetSizeAfterFirst = 0;
		QString sheetAfterFirst;
		{
			ConfigEditorWindow window(path, 0, true);
			const QString appSheet = qApp->styleSheet();
			for (const char* selector : {
				"QFrame[vpvCard=\"true\"]",
				"QLabel[vpvSetBy=\"true\"]",
				"QWidget[vpvSection=\"true\"]",
				"QTabWidget[vpvUnitsTabs=\"true\"]" })
			{
				require(appSheet.contains(QString::fromLatin1(selector)),
					(std::string("qApp->styleSheet() is missing hoisted VP-V selector: ") + selector).c_str());
			}
			sheetSizeAfterFirst = appSheet.size();
			sheetAfterFirst = appSheet;

			auto* section = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section")));
			auto* units = control<QTabWidget>(window, Vpv::ObjectName(QStringLiteral("units")));
			require(section->property("vpvSection").toBool(), "vpv.section lacks vpvSection=true property");
			require(section->styleSheet().isEmpty(),
				("vpv.section retains a per-widget stylesheet: " + S(section->styleSheet())).c_str());
			require(units->property("vpvUnitsTabs").toBool(), "vpv.units lacks vpvUnitsTabs=true property");
			require(units->styleSheet().isEmpty(),
				("vpv.units retains a per-widget stylesheet: " + S(units->styleSheet())).c_str());

			int cardCount = 0;
			for (QFrame* frame : section->findChildren<QFrame*>())
			{
				if (!frame->property("vpvCard").toBool()) continue;
				++cardCount;
				require(frame->styleSheet().isEmpty(),
					("a vpvCard frame retains a per-widget stylesheet: " + S(frame->objectName())).c_str());
			}
			require(cardCount > 0, "no vpvCard=true card frames were found inside vpv.section");

			int setByCount = 0;
			for (QLabel* label : section->findChildren<QLabel*>())
			{
				if (!label->objectName().startsWith(QStringLiteral("vpv.setby.line."))) continue;
				++setByCount;
				require(label->property("vpvSetBy").toBool(),
					("setby label lacks vpvSetBy=true property: " + S(label->objectName())).c_str());
				require(label->styleSheet().isEmpty(),
					("setby label retains a per-widget stylesheet: " + S(label->objectName())).c_str());
			}
			require(setByCount > 0, "no vpv.setby.line.* labels were found inside vpv.section");

			// Negative control (Rule 3 — Bill-page isolation): Bill's General (index 0) and Rendering (index 2)
			// pages must carry zero VP-V style-scoping properties so qApp's hoisted rules cannot bleed onto Bill's UI.
			auto* pages = control<QStackedWidget>(window, QStringLiteral("settingsPages"));
			require(pages->count() > 2, "settingsPages does not contain Bill's General and Rendering pages");
			for (const int billIndex : { 0, 2 })
			{
				QWidget* billPage = pages->widget(billIndex);
				require(billPage != nullptr && billPage != section && !billPage->isAncestorOf(section),
					"expected Bill page index unexpectedly points to vpv.section");
				const QList<QWidget*> widgets = billPage->findChildren<QWidget*>();
				require(!widgets.isEmpty(), "Bill page has no descendant widgets (vacuous isolation check)");
				auto checkNoVpvTags = [&](QWidget* w)
				{
					require(!w->property("vpvSection").toBool(),
						("Bill page widget leaked vpvSection=true: " + S(w->objectName())).c_str());
					require(!w->property("vpvCard").toBool(),
						("Bill page widget leaked vpvCard=true: " + S(w->objectName())).c_str());
					require(!w->property("vpvDrivePage").toBool(),
						("Bill page widget leaked vpvDrivePage=true: " + S(w->objectName())).c_str());
				};
				checkNoVpvTags(billPage);
				for (QWidget* child : widgets)
					checkNoVpvTags(child);
			}
		}

		// Idempotency: constructing a second ConfigEditorWindow must NOT append duplicate stylesheet text to qApp.
		{
			ConfigEditorWindow window2(path, 0, true);
			require(qApp->styleSheet().size() == sheetSizeAfterFirst && qApp->styleSheet() == sheetAfterFirst,
				"constructing a second ConfigEditorWindow grew or duplicated qApp->styleSheet()");
		}
	}

	// Rule 8 Red Team (W1R-FIX5-5): Vpv::State has no reference cycle and frees cleanly on normal close, on
	// rebuildConfigurationShell() (previous->deleteLater()), and when ConfigEditorWindow closes while an orphaned
	// previous shell (and its timers) is still pending DeferredDelete.
	void testSectionStateFreesOnCloseAndShellRebuildWithoutDanglingTimers()
	{
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		const int baseline = Vpv::LiveSectionStateCount();

		QTemporaryDir dir;
		const QString path = WriteFixture(dir, "[general]\nrenderer: VP Renderer\n[vprenderer.Default]\n");

		// 1. Normal open/close lifecycle + negative control proving LiveSectionStateCount tracks live states.
		{
			ConfigEditorWindow window(path, 0, true);
			require(Vpv::LiveSectionStateCount() == baseline + 1,
				"LiveSectionStateCount did not increment to baseline + 1 when ConfigEditorWindow opened");
		}
		QCoreApplication::processEvents();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		require(Vpv::LiveSectionStateCount() == baseline,
			"normal ConfigEditorWindow close leaked Vpv::State");

		// 2. Shell rebuild leak test + orphaned-shell dangling-timer hazard on close.
		{
			ConfigEditorWindow window(path, 0, true);
			require(Vpv::LiveSectionStateCount() == baseline + 1,
				"LiveSectionStateCount did not increment to baseline + 1 on second window open");

			// Rebuild #1: previous shell is queued for deleteLater(); draining events must free the old Vpv::State.
			const QPointer<QWidget> secBeforeRebuild1(control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section"))));
			window.setRendererDiscoveryForTesting(
				{ QStringLiteral("VP Renderer"), QStringLiteral("DirectShow - madVR") },
				{ QStringLiteral("VP Renderer") });
			require(Vpv::LiveSectionStateCount() == baseline + 2,
				"shell rebuild #1 did not allocate a second Vpv::State while previous shell is pending DeferredDelete");
			QCoreApplication::processEvents();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			QCoreApplication::processEvents();
			QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
			QCoreApplication::processEvents();
			require(control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section"))) != nullptr,
				"shell rebuild did not recreate vpv.section");
			require(secBeforeRebuild1.isNull() &&
				control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section"))) != secBeforeRebuild1.data(),
				"shell rebuild #1 did not destroy and replace the previous vpv.section");
			require(Vpv::LiveSectionStateCount() == baseline + 1,
				"shell rebuild leaked previous Vpv::State (cycle or retained strong reference)");

			// Rebuild #2 WITHOUT draining processEvents(): leave an orphaned previous shell + its timers pending
			// DeferredDelete when ~ConfigEditorWindow() executes on scope exit.
			const QPointer<QWidget> secBeforeRebuild2(control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section"))));
			window.setRendererDiscoveryForTesting(
				{ QStringLiteral("VP Renderer"), QStringLiteral("DirectShow - madVR") },
				{ QStringLiteral("VP Renderer") });
			require(control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section"))) != nullptr,
				"second shell rebuild did not recreate vpv.section");
			require(Vpv::LiveSectionStateCount() == baseline + 2 &&
				control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section"))) != secBeforeRebuild2.data(),
				"second shell rebuild did not leave previous Vpv::State pending DeferredDelete on a distinct vpv.section");
		}
		// Drain pending 0-ms timers and deferred deletes after ~ConfigEditorWindow(): must neither crash nor leak.
		QCoreApplication::processEvents();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		require(Vpv::LiveSectionStateCount() == baseline,
			"closing ConfigEditorWindow with a pending orphaned shell leaked Vpv::State");
	}

	// Rule 8 Red Team (D2): an explicit empty key in [vpv] (pipeline_original_pathway:) MUST NOT be
	// overwritten by a legacy [vprenderer] value; same-input negative control verifies an absent [vpv]
	// key still migrates legacy [vprenderer] = on.
	void testD2EmptyVpvKeyNotOverwrittenByLegacyVprenderer()
	{
		auto hasNonVpvPathway = [](const ConfigFile& cfg)
		{
			for (const std::string& section : cfg.GetSectionNames())
			{
				if (ConfigFile::NormalizeName(section) == "vpv") continue;
				std::string v;
				if (cfg.TryGetString(section, "pipeline_original_pathway", v)) return true;
			}
			return false;
		};
		auto pathwayValue = [](ConfigEditorWindow& window)
		{
			return control<QComboBox>(window, Vpv::ObjectName(QStringLiteral("pathway")))->currentData().toString();
		};
		auto requireDefaultGlobalActions = [](ConfigEditorWindow& window, bool expectStateReport, const char* context)
		{
			auto* edit = control<QLineEdit>(window, QStringLiteral("config.shortcuts.global_actions"));
			require(edit->text().isEmpty(), context);
			const QString expected = expectStateReport ? QStringLiteral("Default: state_report") : QStringLiteral("Default: none");
			require(edit->placeholderText().contains(expected), context);
		};

		// Case A (bug): explicit empty [vpv] key must win over legacy [vprenderer] = on.
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[general]\nrenderer: VP Renderer\n[vpv]\npipeline_original_pathway:\n[vprenderer]\npipeline_original_pathway: on\n");
			{
				ConfigEditorWindow window(path, 0, true);
				require(pathwayValue(window).compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0,
					"D2 Case A: empty [vpv] pipeline_original_pathway was overwritten by legacy [vprenderer]=on");
				require(control<QWidget>(window, Vpv::ObjectName(QStringLiteral("content")))->isEnabled(),
					"D2 Case A: vpv.content was disabled despite explicit empty [vpv] key");
				requireDefaultGlobalActions(window, true,
					"D2 Case A: config.shortcuts.global_actions did not retain VP-V default (state_report)");
				save(window);
			}
			ConfigFile config;
			require(config.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh),
				"D2 Case A: cannot reload saved config");
			std::string v;
			const bool hasVpvKey = config.TryGetString("vpv", "pipeline_original_pathway", v);
			require(!hasVpvKey || QString::fromStdString(v).trimmed().isEmpty(),
				("D2 Case A: saved [vpv] pipeline_original_pathway was overwritten with '" + v + "'\n" + FileText(path)).c_str());
			require(!config.TryGetString("vprenderer", "pipeline_original_pathway", v),
				("D2 Case A: legacy [vprenderer] pipeline_original_pathway was not removed\n" + FileText(path)).c_str());
			require(!config.TryGetString("vprenderer.profile_1", "pipeline_original_pathway", v),
				("D2 Case A: pipeline_original_pathway stranded in [vprenderer.profile_1]\n" + FileText(path)).c_str());
			require(!hasNonVpvPathway(config),
				("D2 Case A: pipeline_original_pathway stranded outside [vpv]\n" + FileText(path)).c_str());
			requireVpReads(path);
			{
				ConfigEditorWindow reopened(path, 0, true);
				require(pathwayValue(reopened).compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0,
					"D2 Case A: reopened config did not stay on VP-V pathway");
				require(control<QWidget>(reopened, Vpv::ObjectName(QStringLiteral("content")))->isEnabled(),
					"D2 Case A: reopened vpv.content was disabled");
				requireDefaultGlobalActions(reopened, true,
					"D2 Case A: reopened config.shortcuts.global_actions lost VP-V default (state_report)");
			}
		}

		// Case B (same-input negative control): key ABSENT from [vpv] must still migrate legacy [vprenderer] = on.
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[general]\nrenderer: VP Renderer\n[vpv]\n[vprenderer]\npipeline_original_pathway: on\n");
			{
				ConfigEditorWindow window(path, 0, true);
				require(pathwayValue(window).compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0,
					"D2 Case B (negative control): absent [vpv] key did not migrate legacy [vprenderer]=on");
				require(!control<QWidget>(window, Vpv::ObjectName(QStringLiteral("content")))->isEnabled(),
					"D2 Case B (negative control): vpv.content was not disabled on Original pathway");
				requireDefaultGlobalActions(window, false,
					"D2 Case B (negative control): config.shortcuts.global_actions did not switch to Original default (none)");
				save(window);
			}
			ConfigFile config;
			require(config.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh),
				"D2 Case B (negative control): cannot reload saved config");
			std::string v;
			require(config.TryGetString("vpv", "pipeline_original_pathway", v) &&
				QString::fromStdString(v).trimmed().compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0,
				("D2 Case B (negative control): [vpv] pipeline_original_pathway did not migrate to 'on'\n" + FileText(path)).c_str());
			require(!config.TryGetString("vprenderer", "pipeline_original_pathway", v) &&
				!config.TryGetString("vprenderer.profile_1", "pipeline_original_pathway", v) &&
				!hasNonVpvPathway(config),
				("D2 Case B (negative control): legacy [vprenderer] key was not removed\n" + FileText(path)).c_str());
			requireVpReads(path);
		}

		// Case C (edge case): duplicate [vprenderer] pathway lines + whitespace-only explicit [vpv] key
		// + co-located rendering key (sdr_target_nits: 120) must preserve [vpv], strip every legacy duplicate,
		// and preserve co-located rendering settings.
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[general]\nrenderer: VP Renderer\n"
				"[vpv]\npipeline_original_pathway:   \n"
				"[vprenderer]\nsdr_target_nits: 120\n"
				"pipeline_original_pathway: on\n"
				"pipeline_original_pathway: on\n");
			{
				ConfigEditorWindow window(path, 0, true);
				require(pathwayValue(window).compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0,
					"D2 Case C: whitespace-only [vpv] key was overwritten by duplicate legacy [vprenderer]=on");
				require(control<QWidget>(window, Vpv::ObjectName(QStringLiteral("content")))->isEnabled(),
					"D2 Case C: vpv.content was disabled despite explicit whitespace-only [vpv] key");
				requireDefaultGlobalActions(window, true,
					"D2 Case C: config.shortcuts.global_actions did not retain VP-V default (state_report)");
				save(window);
			}
			ConfigFile config;
			require(config.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh),
				"D2 Case C: cannot reload saved config");
			std::string v;
			const bool hasVpvKey = config.TryGetString("vpv", "pipeline_original_pathway", v);
			require(!hasVpvKey || QString::fromStdString(v).trimmed().isEmpty(),
				("D2 Case C: saved [vpv] pipeline_original_pathway was overwritten with '" + v + "'\n" + FileText(path)).c_str());
			require(!config.TryGetString("vprenderer", "pipeline_original_pathway", v) &&
				!config.TryGetString("vprenderer.profile_1", "pipeline_original_pathway", v) &&
				!hasNonVpvPathway(config),
				("D2 Case C: duplicate legacy pipeline_original_pathway survived outside [vpv]\n" + FileText(path)).c_str());
			bool preservedNits = false;
			for (const std::string& section : config.GetSectionNames())
			{
				if (ConfigFile::NormalizeName(section).rfind("vprenderer.", 0) != 0) continue;
				std::string nits;
				if (config.TryGetString(section, "sdr_target_nits", nits) && Vpv::SameValue(nits, "120"))
					preservedNits = true;
			}
			require(preservedNits,
				("D2 Case C: co-located sdr_target_nits: 120 was dropped during duplicate legacy pathway cleanup\n" + FileText(path)).c_str());
			requireVpReads(path);
			{
				ConfigEditorWindow reopened(path, 0, true);
				require(pathwayValue(reopened).compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0,
					"D2 Case C: reopened config did not stay on VP-V pathway");
				require(control<QWidget>(reopened, Vpv::ObjectName(QStringLiteral("content")))->isEnabled(),
					"D2 Case C: reopened vpv.content was disabled");
				requireDefaultGlobalActions(reopened, true,
					"D2 Case C: reopened config.shortcuts.global_actions lost VP-V default (state_report)");
			}
		}
	}

	// Rule 8 Red Team (D3): VP-V and Dynamic Drive 1000 ms refresh timers must not tick while hidden,
	// must stop when an ACTIVE page is detached or hidden, must resume and run their live callbacks
	// after transient detach + timeout + re-attach, and must free cleanly with zero Vpv::State leaks.
	void testD3RefreshTimersSurviveTransientDetachWithoutFiringWhileHidden()
	{
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		const int baseline = Vpv::LiveSectionStateCount();

		QTemporaryDir dir;
		const QString path = WriteFixture(dir, "[general]\nrenderer: VP Renderer\n[vprenderer.Default]\n");
		{
			ConfigEditorWindow window(path, 0, true);
			auto* root = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section")));
			auto* fallbacks = control<QLabel>(window, Vpv::ObjectName(QStringLiteral("fallbacks")));
			auto* drivePages = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("drivepage.pages")));
			auto* driveOffNote = control<QLabel>(window, Vpv::ObjectName(QStringLiteral("drivepage.off_note")));

			auto find1000msTimer = [](QObject* parent) -> QTimer*
			{
				for (QTimer* t : parent->findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly))
					if (t->interval() == 1000) return t;
				return nullptr;
			};
			QTimer* rootTimer = find1000msTimer(fallbacks);
			QTimer* driveTimer = find1000msTimer(drivePages);
			require(rootTimer != nullptr, "D3: missing 1000 ms refresh QTimer on vpv.fallbacks");
			require(driveTimer != nullptr, "D3: missing 1000 ms refresh QTimer on vpv.drivepage.pages");

			// Hidden safety before show: neither timer may tick while the window is hidden.
			require(!root->isVisible() && !drivePages->isVisible(),
				"D3: expected vpv.section and vpv.drivepage.pages to start hidden before window.show()");
			require(!rootTimer->isActive(),
				"D3: vpv.fallbacks refresh timer is active while window is still hidden");
			require(!driveTimer->isActive(),
				"D3: vpv.drivepage.pages refresh timer is active while window is still hidden");

			// VP-V page: show FIRST -> verify ACTIVE -> detach ACTIVE root -> timeout -> re-attach -> verify callback -> hide ACTIVE root.
			{
				window.selectPage(QStringLiteral("VP-V"));
				window.show();
				QCoreApplication::processEvents();
				require(root->isVisible(), "D3: vpv.section did not become visible on initial show");
				require(rootTimer->isActive(), "D3: vpv.fallbacks timer did not activate when VP-V page was shown");
				require(!driveTimer->isActive(),
					"D3: vpv.drivepage.pages timer should remain stopped while VP-V page is active");

				QWidget* origParent = root->parentWidget();
				QLayout* origLayout = origParent ? origParent->layout() : nullptr;
				require(origParent != nullptr, "D3: vpv.section has no parent widget before detach");
				root->setParent(nullptr);
				QCoreApplication::processEvents();
				require(!rootTimer->isActive(),
					"D3: detaching an ACTIVE vpv.section did not stop vpv.fallbacks timer");
				QMetaObject::invokeMethod(rootTimer, "timeout", Qt::DirectConnection);
				require(!rootTimer->isActive(),
					"D3: vpv.fallbacks timer restarted unexpectedly while vpv.section was detached");

				if (origLayout) origLayout->addWidget(root);
				else root->setParent(origParent);
				root->setVisible(true);
				window.selectPage(QStringLiteral("VP-V"));
				window.show();
				QCoreApplication::processEvents();
				require(root->isVisible(), "D3: vpv.section did not become visible after re-attach and show");
				require(rootTimer->isActive(),
					"D3: vpv.fallbacks refresh timer did not restart after transient detach and re-attach");
				require(!driveTimer->isActive(),
					"D3: vpv.drivepage.pages timer should remain stopped while VP-V page is shown");

				// Unconditional callback proof (never silently skipped): mirror() must overwrite sentinel text.
				fallbacks->setText(QStringLiteral("D3-sentinel-before-tick"));
				QMetaObject::invokeMethod(rootTimer, "timeout", Qt::DirectConnection);
				require(fallbacks->text() != QStringLiteral("D3-sentinel-before-tick"),
					"D3: rootTimer timeout did not execute mirror() after re-attach");
				if (auto* liveFallbacks = window.findChild<QLabel*>(QStringLiteral("config.live.fallbacks")))
				{
					liveFallbacks->setText(QStringLiteral("Fallbacks engaged: D3 live probe"));
					QMetaObject::invokeMethod(rootTimer, "timeout", Qt::DirectConnection);
					require(fallbacks->text() == QStringLiteral("Fallbacks engaged: D3 live probe"),
						"D3: vpv.fallbacks timer callback did not mirror config.live.fallbacks after re-attach");
				}
				require(rootTimer->isActive(),
					"D3: vpv.fallbacks timer stopped on timeout while visible and attached");

				// Negative control: hiding an ACTIVE VP-V page must stop rootTimer.
				window.hide();
				QCoreApplication::processEvents();
				require(!rootTimer->isActive(), "D3: ACTIVE vpv.fallbacks timer stayed active after window.hide()");
				require(!driveTimer->isActive(), "D3: vpv.drivepage.pages timer stayed active after window.hide()");
			}

			// Dynamic Drive page: show FIRST -> verify ACTIVE -> detach ACTIVE drivePages -> timeout -> re-attach -> verify callback -> hide ACTIVE drivePages.
			{
				window.selectPage(QStringLiteral("Dynamic Drive"));
				window.show();
				QCoreApplication::processEvents();
				require(drivePages->isVisible(), "D3: vpv.drivepage.pages did not become visible on initial show");
				require(driveTimer->isActive(), "D3: vpv.drivepage.pages timer did not activate when Dynamic Drive page was shown");
				require(!rootTimer->isActive(),
					"D3: vpv.fallbacks timer should remain stopped while Dynamic Drive page is active");

				QWidget* origParent = drivePages->parentWidget();
				QLayout* origLayout = origParent ? origParent->layout() : nullptr;
				require(origParent != nullptr, "D3: vpv.drivepage.pages has no parent widget before detach");
				drivePages->setParent(nullptr);
				QCoreApplication::processEvents();
				require(!driveTimer->isActive(),
					"D3: detaching an ACTIVE vpv.drivepage.pages did not stop driveTimer");
				QMetaObject::invokeMethod(driveTimer, "timeout", Qt::DirectConnection);
				require(!driveTimer->isActive(),
					"D3: vpv.drivepage.pages timer restarted unexpectedly while detached");

				if (origLayout) origLayout->addWidget(drivePages);
				else drivePages->setParent(origParent);
				drivePages->setVisible(true);
				window.selectPage(QStringLiteral("Dynamic Drive"));
				window.show();
				QCoreApplication::processEvents();
				require(drivePages->isVisible(), "D3: vpv.drivepage.pages did not become visible after re-attach and show");
				require(driveTimer->isActive(),
					"D3: vpv.drivepage.pages refresh timer did not restart after transient detach and re-attach");
				require(!rootTimer->isActive(),
					"D3: vpv.fallbacks timer did not stop when navigating away to Dynamic Drive");

				// Unconditional driveTimer callback proof: refreshReasons() must overwrite drivepage.off_note sentinel.
				driveOffNote->setText(QStringLiteral("D3-drive-sentinel-before-tick"));
				QMetaObject::invokeMethod(driveTimer, "timeout", Qt::DirectConnection);
				require(driveOffNote->text() != QStringLiteral("D3-drive-sentinel-before-tick"),
					"D3: driveTimer timeout did not execute refreshReasons() after re-attach");
				require(driveTimer->isActive(),
					"D3: vpv.drivepage.pages timer stopped on timeout while visible and attached");

				// Negative control: hiding an ACTIVE Dynamic Drive page must stop driveTimer.
				window.hide();
				QCoreApplication::processEvents();
				require(!driveTimer->isActive(), "D3: ACTIVE vpv.drivepage.pages timer stayed active after window.hide()");
				require(!rootTimer->isActive(), "D3: vpv.fallbacks timer stayed active after window.hide()");
			}

			// Live visible page-switch check (without hiding window): switching VP-V -> Dynamic Drive
			// must stop rootTimer and start driveTimer cleanly.
			{
				window.selectPage(QStringLiteral("VP-V"));
				window.show();
				QCoreApplication::processEvents();
				require(root->isVisible() && rootTimer->isActive() && !driveTimer->isActive(),
					"D3: live page switch baseline expected VP-V timer active and Dynamic Drive timer stopped");
				window.selectPage(QStringLiteral("Dynamic Drive"));
				QCoreApplication::processEvents();
				require(drivePages->isVisible() && driveTimer->isActive() && !rootTimer->isActive(),
					"D3: live page switch VP-V -> Dynamic Drive did not stop rootTimer and start driveTimer");
				window.hide();
				QCoreApplication::processEvents();
				require(!rootTimer->isActive() && !driveTimer->isActive(),
					"D3: timers stayed active after window.hide() following live page switch");
			}
		}
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		require(Vpv::LiveSectionStateCount() == baseline,
			"D3: Vpv::LiveSectionStateCount() did not return to baseline after window destroy");
	}

	// Rule 8 Red Team (D4-D5): zero per-widget VP-V styles (including vpvChild sidebar leaves and
	// all vpv.section descendants), hoisted qApp stylesheet slices cleanly at the begin marker and
	// matches SectionStyleSheetForMode(), preserves focus/tab-bar/disabled-heading/note-specificity
	// cascade priority, keeps high-contrast free of color overrides, and keeps every VP-V selector scoped.
	void testD4D5StylesheetCascadePriorityAndZeroPerWidgetVpvStyles()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir, "[general]\nrenderer: VP Renderer\n[vprenderer.Default]\n");
		require(qApp != nullptr, "QApplication instance is missing");
		ConfigEditorWindow window(path, 0, true);

		// 1. Zero per-widget stylesheets on vpvChild sidebar leaves (Dynamic Drive, Calibration)
		//    and on all VP-V descendants inside vpv.section (excluding Bill's bindChoiceField
		//    QAbstractItemView popups and visible Bill .reset_override buttons).
		int vpvChildCount = 0;
		for (QPushButton* btn : window.findChildren<QPushButton*>())
		{
			if (!btn->property("vpvChild").toBool()) continue;
			++vpvChildCount;
			require(btn->styleSheet().isEmpty(),
				("D4-D5: vpvChild sidebar button retains a per-widget stylesheet (" + S(btn->text()) + "): " + S(btn->styleSheet())).c_str());
		}
		require(vpvChildCount >= 2, "D4-D5: expected at least 2 vpvChild sidebar buttons (Dynamic Drive, Calibration)");

		auto* section = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("section")));
		require(section->styleSheet().isEmpty(),
			("D4-D5: vpv.section itself retains a per-widget stylesheet: " + S(section->styleSheet())).c_str());
		int scannedVpvWidgets = 0;
		for (QWidget* w : section->findChildren<QWidget*>())
		{
			if (qobject_cast<QAbstractItemView*>(w) != nullptr) continue;
			const bool isVisibleBillReset = qobject_cast<QPushButton*>(w) != nullptr &&
				w->objectName().endsWith(QStringLiteral(".reset_override")) &&
				w->parentWidget() && !w->parentWidget()->isHidden();
			if (isVisibleBillReset) continue;
			++scannedVpvWidgets;
			require(w->styleSheet().isEmpty(),
				("D4-D5: widget inside vpv.section retains a per-widget stylesheet (" +
					std::string(w->metaObject()->className()) + " '" + S(w->objectName()) + "'): " +
					S(w->styleSheet())).c_str());
		}
		require(scannedVpvWidgets > 50,
			"D4-D5: unexpectedly few descendant widgets scanned inside vpv.section");
		for (const char* driveNote : { "drivepage.original_note", "drivepage.off_note" })
		{
			auto* note = control<QLabel>(window, Vpv::ObjectName(QString::fromLatin1(driveNote)));
			require(note->styleSheet().isEmpty(),
				(std::string("D4-D5: VpvSection Dynamic Drive note retains a per-widget stylesheet (") +
					driveNote + "): " + S(note->styleSheet())).c_str());
		}

		// 2. Slice qApp->styleSheet() strictly at /* vpv-section-style-begin */ (non-tautological boundary)
		//    and verify live wiring to Vpv::SectionStyleSheetForMode().
		const QString appSheet = qApp->styleSheet();
		const QString beginMarker = QStringLiteral("/* vpv-section-style-begin */");
		const int markerPos = appSheet.indexOf(beginMarker);
		require(markerPos >= 0,
			"D4-D5: qApp->styleSheet() is missing /* vpv-section-style-begin */ marker");
		const QString billSheet = appSheet.left(markerPos);
		const QString vpvSheet = appSheet.mid(markerPos);
		require(billSheet.contains(QStringLiteral("QLabel[help=\"true\"]")) && !billSheet.contains(QStringLiteral("[vpv")),
			"D4-D5 negative control: expected Bill's VpTheme rules (without [vpv) strictly before /* vpv-section-style-begin */");

		const QString dark = Vpv::SectionStyleSheetForMode(false);
		const QString hc = Vpv::SectionStyleSheetForMode(true);
		const bool expectHighContrast = !billSheet.contains(QLatin1Char('#'));
		require(vpvSheet.trimmed() == (expectHighContrast ? hc : dark).trimmed(),
			"D4-D5: installed qApp VP-V stylesheet does not match the active high-contrast/dark mode from SectionStyleSheetForMode()");

		// Structural CSS rule/declaration parser (eliminates comment/comma/pseudo-state regex loopholes).
		struct CssRule
		{
			QString selector;
			QString body;
		};
		auto parseCssRules = [](const QString& css, const char* label) -> std::vector<CssRule>
		{
			QString cleaned = css;
			cleaned.remove(QRegularExpression(QStringLiteral(R"(/\*[\s\S]*?\*/)")));
			std::vector<CssRule> rules;
			for (const QString& block : cleaned.split(QLatin1Char('}')))
			{
				const QString trimmed = block.trimmed();
				if (trimmed.isEmpty()) continue;
				const int brace = trimmed.indexOf(QLatin1Char('{'));
				require(brace > 0, (std::string("D4-D5 (") + label + "): malformed CSS rule: " + S(trimmed)).c_str());
				const QString selectorList = trimmed.left(brace);
				const QString body = trimmed.mid(brace + 1).trimmed();
				for (const QString& sel : selectorList.split(QLatin1Char(',')))
				{
					const QString s = sel.trimmed();
					require(!s.isEmpty(), (std::string("D4-D5 (") + label + "): empty selector in rule list").c_str());
					rules.push_back(CssRule{ s, body });
				}
			}
			return rules;
		};
		auto parseDecls = [](const QString& body) -> std::vector<std::pair<QString, QString>>
		{
			std::vector<std::pair<QString, QString>> decls;
			for (const QString& part : body.split(QLatin1Char(';')))
			{
				const QString t = part.trimmed();
				if (t.isEmpty()) continue;
				const int colon = t.indexOf(QLatin1Char(':'));
				if (colon <= 0) continue;
				decls.emplace_back(t.left(colon).trimmed(), t.mid(colon + 1).trimmed());
			}
			return decls;
		};
		auto findDeclValue = [&](const QString& body, const QString& propName) -> QString
		{
			QString result;
			for (const auto& kv : parseDecls(body))
				if (kv.first.compare(propName, Qt::CaseInsensitive) == 0) result = kv.second;
			return result;
		};
		auto hasPseudoState = [](const QString& sel) -> bool
		{
			QString s = sel;
			s.replace(QStringLiteral("::"), QString());
			return s.contains(QLatin1Char(':'));
		};
		auto requireAllSelectorsVpvScoped = [&](const std::vector<CssRule>& rules, int minRules, const char* label)
		{
			require(static_cast<int>(rules.size()) >= minRules,
				(std::string("D4-D5 (") + label + "): unexpectedly few CSS rules parsed: " + std::to_string(rules.size())).c_str());
			for (const CssRule& r : rules)
			{
				require(r.selector.contains(QStringLiteral("[vpv")),
					(std::string("D4-D5 (") + label + "): unscoped selector risks bleeding onto Bill's pages: " + S(r.selector)).c_str());
			}
		};

		const std::vector<CssRule> vpvRules = parseCssRules(vpvSheet, "qApp vpvSheet");
		const std::vector<CssRule> darkRules = parseCssRules(dark, "darkMode");
		const std::vector<CssRule> hcRules = parseCssRules(hc, "highContrast");
		requireAllSelectorsVpvScoped(vpvRules, 4, "qApp vpvSheet");
		requireAllSelectorsVpvScoped(darkRules, 10, "darkMode");
		requireAllSelectorsVpvScoped(hcRules, 4, "highContrast");

		// 3. Dark-mode cascade priority checks on parsed SectionStyleSheetForMode(false) rules.
		require(dark.contains(QLatin1Char('#')),
			"D4-D5 negative control: dark-mode VP-V stylesheet must carry dark theme hex colors that high-contrast omits");

		auto hasVpvChildPaddingRule = [&](const std::vector<CssRule>& rules) -> bool
		{
			for (const CssRule& r : rules)
			{
				if (r.selector.startsWith(QStringLiteral("QPushButton")) &&
					r.selector.contains(QStringLiteral("[vpvChild=\"true\"]")) &&
					!hasPseudoState(r.selector) &&
					!findDeclValue(r.body, QStringLiteral("padding-left")).isEmpty())
					return true;
			}
			return false;
		};
		require(hasVpvChildPaddingRule(darkRules),
			"D4-D5: dark-mode VP-V stylesheet is missing a hoisted QPushButton[vpvChild=\"true\"] padding-left rule");

		bool hasPairErrorColor = false;
		for (const CssRule& r : darkRules)
		{
			if (r.selector.contains(QStringLiteral("QLineEdit[vpvPairError=\"true\"]")) &&
				!hasPseudoState(r.selector))
			{
				const QColor c(findDeclValue(r.body, QStringLiteral("color")));
				if (c.isValid()) { hasPairErrorColor = true; break; }
			}
		}
		require(hasPairErrorColor,
			"D4-D5: dark-mode VP-V stylesheet is missing a valid color on QLineEdit[vpvPairError=\"true\"]");

		// Preserve visible :focus border on QToolButton[profileSection="true"]:focus (not clobbering VpTheme.cpp:244).
		require(appSheet.contains(QStringLiteral("QToolButton[profileSection=\"true\"]:focus")),
			"D4-D5: QToolButton[profileSection=\"true\"]:focus is missing from qApp->styleSheet()");
		bool overridesProfileSection = false;
		bool preservesProfileSectionFocus = false;
		for (const CssRule& r : darkRules)
		{
			if (r.selector.contains(QStringLiteral("QToolButton[profileSection=\"true\"]")) &&
				!hasPseudoState(r.selector))
			{
				overridesProfileSection = true;
			}
			if (r.selector.contains(QStringLiteral("QWidget[vpvSection=\"true\"]")) &&
				r.selector.contains(QStringLiteral("QToolButton[profileSection=\"true\"]:focus")) &&
				!r.selector.contains(QStringLiteral(":hover")) &&
				!r.selector.contains(QStringLiteral(":disabled")))
			{
				const QString border = findDeclValue(r.body, QStringLiteral("border"));
				const QStringList parts = border.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
				if (parts.size() >= 3 &&
					parts[0].compare(QStringLiteral("1px"), Qt::CaseInsensitive) == 0 &&
					parts[1].compare(QStringLiteral("solid"), Qt::CaseInsensitive) == 0)
				{
					const QColor bc(parts[2]);
					if (bc.isValid() && bc.alpha() > 0) preservesProfileSectionFocus = true;
				}
			}
		}
		require(!overridesProfileSection || preservesProfileSectionFocus,
			"D4-D5: dark-mode VP-V stylesheet overrides QToolButton[profileSection=\"true\"] without preserving visible 1px solid :focus border");

		// Explicit QTabBar QToolButton rule under vpvSection with a valid foreground color.
		bool hasTabBarToolButtonColor = false;
		for (const CssRule& r : darkRules)
		{
			if (r.selector.contains(QStringLiteral("QWidget[vpvSection=\"true\"]")) &&
				(r.selector.contains(QStringLiteral("QTabBar QToolButton")) ||
					r.selector.contains(QStringLiteral("QTabBar > QToolButton"))) &&
				!hasPseudoState(r.selector))
			{
				if (QColor(findDeclValue(r.body, QStringLiteral("color"))).isValid())
				{
					hasTabBarToolButtonColor = true;
					break;
				}
			}
		}
		require(hasTabBarToolButtonColor,
			"D4-D5: dark-mode VP-V stylesheet is missing a QWidget[vpvSection=\"true\"] QTabBar QToolButton rule with a valid color");

		// Enabled vs :disabled dimming verification for vpvCardHeading and vpvSetBy (extracted directly
		// from winning enabled and matching :disabled rules by lightness, respecting vpvSection specificity).
		auto requireDimmedDisabledLabel = [&](const char* prop, bool requireScopedDisabled)
		{
			const QString token = QStringLiteral("QLabel[%1=\"true\"]").arg(QString::fromLatin1(prop));
			const QString disabledToken = token + QStringLiteral(":disabled");
			QColor enabledColor;
			int enabledSpec = -1;
			for (const CssRule& r : darkRules)
			{
				if (!r.selector.contains(token) || hasPseudoState(r.selector)) continue;
				const QColor c(findDeclValue(r.body, QStringLiteral("color")));
				if (!c.isValid()) continue;
				const int spec = r.selector.contains(QStringLiteral("QWidget[vpvSection=\"true\"]")) ? 2 : 1;
				if (spec >= enabledSpec) { enabledSpec = spec; enabledColor = c; }
			}
			require(enabledColor.isValid(),
				(std::string("D4-D5: dark-mode VP-V stylesheet is missing an enabled color rule for ") + prop).c_str());

			const bool needScopedDisabled = requireScopedDisabled || (enabledSpec >= 2);
			QColor disabledColor;
			int disabledSpec = -1;
			for (const CssRule& r : darkRules)
			{
				if (!r.selector.contains(disabledToken) ||
					r.selector.contains(QStringLiteral(":hover")) ||
					r.selector.contains(QStringLiteral(":focus")))
					continue;
				const bool scoped = r.selector.contains(QStringLiteral("QWidget[vpvSection=\"true\"]"));
				if (needScopedDisabled && !scoped) continue;
				const QColor c(findDeclValue(r.body, QStringLiteral("color")));
				if (!c.isValid()) continue;
				const int spec = scoped ? 2 : 1;
				if (spec >= disabledSpec) { disabledSpec = spec; disabledColor = c; }
			}
			require(disabledColor.isValid(),
				(std::string("D4-D5: dark-mode VP-V stylesheet is missing a matching :disabled color rule for ") + prop).c_str());
			require(disabledColor.lightness() < enabledColor.lightness(),
				(std::string("D4-D5: dark-mode :disabled color for ") + prop + " does not dim below its enabled color").c_str());
		};
		requireDimmedDisabledLabel("vpvCardHeading", true);
		requireDimmedDisabledLabel("vpvSetBy", false);

		// vpvPrecisionNote, vpvOriginalNote, vpvOffNote scoped under QWidget[vpvSection="true"] with a
		// valid foreground color (strictly higher specificity than VpTheme's QLabel[help="true"]),
		// AND verified against the live widgets and their vpvSection ancestor (zero dead CSS).
		for (const char* prop : { "vpvPrecisionNote", "vpvOriginalNote", "vpvOffNote" })
		{
			const QString token = QStringLiteral("QLabel[%1=\"true\"]").arg(QString::fromLatin1(prop));
			bool foundValidNoteColor = false;
			for (const CssRule& r : darkRules)
			{
				if (r.selector.contains(QStringLiteral("QWidget[vpvSection=\"true\"]")) &&
					r.selector.contains(token) &&
					!hasPseudoState(r.selector) &&
					QColor(findDeclValue(r.body, QStringLiteral("color"))).isValid())
				{
					foundValidNoteColor = true;
					break;
				}
			}
			require(foundValidNoteColor,
				(std::string("D4-D5: ") + prop + " is missing a vpvSection-scoped QLabel valid color rule").c_str());
		}
		auto hasVpvSectionAncestor = [](const QWidget* w) -> bool
		{
			for (const QWidget* anc = w ? w->parentWidget() : nullptr; anc; anc = anc->parentWidget())
				if (anc->property("vpvSection").toBool()) return true;
			return false;
		};
		const std::pair<const char*, const char*> liveNoteChecks[] = {
			{ "full_precision.note", "vpvPrecisionNote" },
			{ "original_note", "vpvOriginalNote" },
			{ "drivepage.original_note", "vpvOriginalNote" },
			{ "drivepage.off_note", "vpvOffNote" },
		};
		for (const auto& nc : liveNoteChecks)
		{
			auto* label = control<QLabel>(window, Vpv::ObjectName(QString::fromLatin1(nc.first)));
			require(label->property(nc.second).toBool(),
				(std::string("D4-D5: live note widget vpv.") + nc.first + " lacks property " + nc.second + "=true").c_str());
			require(hasVpvSectionAncestor(label),
				(std::string("D4-D5: live note widget vpv.") + nc.first + " has no QWidget[vpvSection=\"true\"] ancestor").c_str());
		}

		// 4. D4 high-contrast mode verification via parsed SectionStyleSheetForMode(true) declarations:
		//    zero color/gradient declarations, background strictly transparent, border/outline strictly 0/0px/none.
		require(!hc.isEmpty(), "D4-D5: high-contrast VP-V stylesheet must not be empty");
		require(hc.contains(beginMarker) && hc.contains(QStringLiteral("/* vpv-section-style */")),
			"D4-D5: high-contrast VP-V stylesheet is missing begin/end style markers");
		for (const CssRule& r : hcRules)
		{
			for (const auto& kv : parseDecls(r.body))
			{
				const QString& key = kv.first;
				const QString& val = kv.second;
				require(!key.contains(QStringLiteral("color"), Qt::CaseInsensitive),
					("D4-D5: high-contrast rule sets forbidden color property (" + S(key) + ": " + S(val) + ")").c_str());
				require(!val.contains(QLatin1Char('#')) &&
					!val.contains(QStringLiteral("rgb("), Qt::CaseInsensitive) &&
					!val.contains(QStringLiteral("rgba("), Qt::CaseInsensitive) &&
					!val.contains(QStringLiteral("hsl("), Qt::CaseInsensitive) &&
					!val.contains(QStringLiteral("hsla("), Qt::CaseInsensitive) &&
					!val.contains(QStringLiteral("hsv("), Qt::CaseInsensitive) &&
					!val.contains(QStringLiteral("hsva("), Qt::CaseInsensitive) &&
					!val.contains(QStringLiteral("gradient("), Qt::CaseInsensitive),
					("D4-D5: high-contrast rule contains forbidden color/gradient literal (" + S(key) + ": " + S(val) + ")").c_str());
				if (key.compare(QStringLiteral("background"), Qt::CaseInsensitive) == 0)
				{
					require(val.compare(QStringLiteral("transparent"), Qt::CaseInsensitive) == 0,
						("D4-D5: high-contrast background must be strictly transparent, got: " + S(val)).c_str());
				}
				const QString lowerKey = key.toLower();
				if (lowerKey == QStringLiteral("border") ||
					lowerKey == QStringLiteral("border-top") ||
					lowerKey == QStringLiteral("border-right") ||
					lowerKey == QStringLiteral("border-bottom") ||
					lowerKey == QStringLiteral("border-left") ||
					lowerKey == QStringLiteral("outline"))
				{
					const QString lowerVal = val.toLower();
					require(lowerVal == QStringLiteral("0") ||
						lowerVal == QStringLiteral("0px") ||
						lowerVal == QStringLiteral("none"),
						("D4-D5: high-contrast " + S(key) + " must be 0/0px/none, got: " + S(val)).c_str());
				}
			}
		}
		require(hasVpvChildPaddingRule(hcRules),
			"D4-D5: high-contrast VP-V stylesheet is missing structural QPushButton[vpvChild=\"true\"] padding-left rule");
	}

	// Rule 8 Red Team (Item 5): hidden VP-V combo popups intercept queued QComboBoxListView MetaCalls
	// during shell rebuilds (while unfiltered Bill combos prove the Qt hazard is real), model mutations
	// on closed VP-V combos never busy-spin delayedLayout timers, hidden vpvRow .reset_override buttons
	// carry zero per-widget stylesheets, and coalesced VP-V combos lay out items, scrollbars, and open
	// interactive selection cleanly on showPopup().
	void testItem5HiddenVpvComboPopupsCoalesceEventsAndSyncOnShow()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[general]\nrenderer: VP Renderer\n[vprenderer.Default]\ndetail_pulse_stabilise: on\n");
		require(qApp != nullptr, "QApplication instance is missing");

		ConfigEditorWindow window(path, 0, true);
		QCoreApplication::processEvents();
		window.setRendererDiscoveryForTesting({
			QStringLiteral("VP Renderer"),
			QStringLiteral("DirectShow - madVR"),
			QStringLiteral("DirectShow - Video Renderer"),
			QStringLiteral("DirectShow - Video Mixing Renderer 9") }, {
			QStringLiteral("VP Renderer"),
			QStringLiteral("DirectShow - madVR") });
		auto* pages = control<QStackedWidget>(window, QStringLiteral("settingsPages"));
		require(pages->count() > 7, "Item 5: settingsPages has fewer than 8 pages");
		pages->setCurrentIndex(7);
		auto* hideLegacyRenderers = control<QCheckBox>(window, QStringLiteral("config.general.hide_legacy_renderers"));

		auto isVpvCombo = [](QComboBox* combo) -> bool
		{
			if (!combo) return false;
			const QString name = combo->objectName();
			if (name.startsWith(QStringLiteral("vpv.")) ||
				name.startsWith(QStringLiteral("calibration.")) ||
				name.startsWith(QStringLiteral("config.calibration.")))
				return true;
			const QString prefix = QStringLiteral("config.vprenderer.");
			if (name.startsWith(prefix))
			{
				const QString tail = name.mid(prefix.size());
				if (Vpv::IsVpvOnlyKey(tail) || Vpv::IsVpvChoiceKey(tail))
					return true;
				const int lastDot = tail.lastIndexOf(QLatin1Char('.'));
				if (lastDot >= 0 && lastDot + 1 < tail.size())
				{
					const QString key = tail.mid(lastDot + 1);
					if (Vpv::IsVpvOnlyKey(key) || Vpv::IsVpvChoiceKey(key))
						return true;
				}
			}
			for (QWidget* anc = combo; anc; anc = anc->parentWidget())
			{
				const QString ancName = anc->objectName();
				if (ancName == QStringLiteral("vpv.section") ||
					ancName == QStringLiteral("vpv.drivepage.pages"))
					return true;
			}
			return false;
		};

		int billComboMetaCalls = 0;
		int vpvComboMetaCalls = 0;
		struct ComboMetaCallSpy : QObject
		{
			std::function<bool(QComboBox*)> classifyVpv;
			int* billCount = nullptr;
			int* vpvCount = nullptr;
			bool eventFilter(QObject* receiver, QEvent* event) override
			{
				if (event && event->type() == QEvent::MetaCall && receiver && receiver->metaObject() &&
					std::strcmp(receiver->metaObject()->className(), "QComboBoxListView") == 0)
				{
					QComboBox* owner = nullptr;
					for (QObject* p = receiver->parent(); p; p = p->parent())
					{
						if (auto* cb = qobject_cast<QComboBox*>(p))
						{
							owner = cb;
							break;
						}
					}
					if (owner)
					{
						if (classifyVpv(owner)) ++(*vpvCount);
						else ++(*billCount);
					}
				}
				return QObject::eventFilter(receiver, event);
			}
		};
		struct ObjectEventFilterGuard
		{
			QObject* target = nullptr;
			QObject* filter = nullptr;
			ObjectEventFilterGuard(QObject* t, QObject* f) : target(t), filter(f)
			{
				if (target && filter) target->installEventFilter(filter);
			}
			~ObjectEventFilterGuard()
			{
				if (target && filter) target->removeEventFilter(filter);
			}
		};

		auto* rendererCombo = control<QComboBox>(window, QStringLiteral("config.general.renderer"));
		auto* rendererListView = qobject_cast<QListView*>(rendererCombo->view());
		require(rendererListView != nullptr, "Item 5: config.general.renderer does not use a QListView");
		auto countVisibleRows = [](QComboBox* cb, QListView* lv)
		{
			int n = 0;
			for (int i = 0; i < cb->count(); ++i)
				if (!lv->isRowHidden(i)) ++n;
			return n;
		};
		require(hideLegacyRenderers->isChecked(),
			"Item 5: expected hide_legacy_renderers to start checked before toggling false");
		const int visibleRenderersBefore = countVisibleRows(rendererCombo, rendererListView);

		qint64 toggleElapsedMs = 0;
		std::uint64_t deferredBefore = 0;
		std::uint64_t deferredAfter = 0;
		{
			ComboMetaCallSpy spy;
			spy.classifyVpv = isVpvCombo;
			spy.billCount = &billComboMetaCalls;
			spy.vpvCount = &vpvComboMetaCalls;
			ObjectEventFilterGuard guard(qApp, &spy);
			deferredBefore = Vpv::DeferredHiddenPopupEventCount();
			QElapsedTimer toggleTimer;
			toggleTimer.start();
			hideLegacyRenderers->setChecked(false);
			QCoreApplication::processEvents();
			toggleElapsedMs = toggleTimer.elapsed();
			deferredAfter = Vpv::DeferredHiddenPopupEventCount();
		}
		require(!hideLegacyRenderers->isChecked(),
			"Item 5: hide_legacy_renderers did not uncheck after setChecked(false)");
		const int visibleRenderersAfter = countVisibleRows(rendererCombo, rendererListView);
		require(visibleRenderersAfter > visibleRenderersBefore,
			("Item 5: unchecking hide_legacy_renderers did not reveal legacy renderers (before=" +
				std::to_string(visibleRenderersBefore) + ", after=" + std::to_string(visibleRenderersAfter) + ")").c_str());
		const std::uint64_t deferredDelta = deferredAfter - deferredBefore;

		// 1. In-test negative control: unfiltered Bill combos genuinely incur hundreds of QComboBoxListView MetaCalls.
		require(billComboMetaCalls > 500,
			("Item 5 negative control failed: expected > 500 Bill QComboBoxListView MetaCalls during shell rebuild, got " +
				std::to_string(billComboMetaCalls) + " (vpv=" + std::to_string(vpvComboMetaCalls) + ")").c_str());

		// 2. Aligned MetaCall interception + Rule 4 observability + latency bound:
		//    qApp's spy and deferredBefore/deferredAfter bracket the exact same event-drain window;
		//    both vpvComboMetaCalls and deferredDelta must be > 0, and every VP-V MetaCall reaching
		//    the loop must be intercepted by HiddenComboPopupCoalescer.
		require(vpvComboMetaCalls > 0,
			("Item 5 non-vacuous guard: expected > 0 VP-V QComboBoxListView MetaCalls reaching event loop during drain, got " +
				std::to_string(vpvComboMetaCalls) + " (bill=" + std::to_string(billComboMetaCalls) + ")").c_str());
		require(deferredDelta > 0,
			("Item 5 Rule 4 observability: expected > 0 deferred hidden popup events during drain, got " +
				std::to_string(deferredDelta) + " (before=" + std::to_string(deferredBefore) +
				", after=" + std::to_string(deferredAfter) + ")").c_str());
		require(deferredDelta == static_cast<std::uint64_t>(vpvComboMetaCalls),
			("Item 5 interception mismatch: deferredDelta=" + std::to_string(deferredDelta) +
				" != vpvComboMetaCalls=" + std::to_string(vpvComboMetaCalls) +
				" (expected exact 1-to-1 interception of closed VP-V QComboBoxListView MetaCalls)").c_str());
		require(toggleElapsedMs < 250,
			("Item 5 latency bound: hideLegacyRenderers toggle + processEvents took " +
				std::to_string(toggleElapsedMs) + " ms (limit < 250 ms)").c_str());

		// 2b. Independent closed-popup MetaCall suppression proof across EVERY closed combo in window
		//     + symmetric Bill negative control (ZERO reliance on production's own counter):
		//     queue a real MetaCall to every closed combo's existing QAbstractItemView, drain once,
		//     and verify 100% of VP-V combos suppressed it while 100% of Bill combos delivered it.
		{
			struct ClosedMetaCallProbe
			{
				QString name;
				bool delivered = false;
			};
			std::deque<ClosedMetaCallProbe> vpvProbes;
			std::deque<ClosedMetaCallProbe> billProbes;
			for (QComboBox* cb : window.findChildren<QComboBox*>())
			{
				QAbstractItemView* v = cb->findChild<QAbstractItemView*>();
				if (!v || v->isVisible()) continue;
				const QString label = cb->objectName().isEmpty() ? QStringLiteral("<unnamed>") : cb->objectName();
				auto& bucket = isVpvCombo(cb) ? vpvProbes : billProbes;
				bucket.push_back(ClosedMetaCallProbe{ label, false });
				bool* deliveredPtr = &bucket.back().delivered;
				QMetaObject::invokeMethod(v, [deliveredPtr]() { *deliveredPtr = true; }, Qt::QueuedConnection);
			}
			QCoreApplication::processEvents();
			require(vpvProbes.size() >= 100,
				("Item 5 closed-suppression probe: expected >= 100 closed VP-V combos, found " +
					std::to_string(vpvProbes.size())).c_str());
			require(billProbes.size() >= 20,
				("Item 5 closed-suppression negative control: expected >= 20 closed Bill combos, found " +
					std::to_string(billProbes.size())).c_str());
			for (const ClosedMetaCallProbe& p : vpvProbes)
			{
				require(!p.delivered,
					("Item 5: closed VP-V popup view failed to suppress queued MetaCall on " + S(p.name)).c_str());
			}
			for (const ClosedMetaCallProbe& p : billProbes)
			{
				require(p.delivered,
					("Item 5 negative control: closed Bill popup view did not deliver queued MetaCall on " + S(p.name)).c_str());
			}
		}

		// 3. BLOCKER #1 busy-spin regression check + in-test negative control:
		//    Phase 1 simulates swallowing QEvent::Timer on profileView and proves Qt's delayedLayout
		//    0-ms timer genuinely busy-spins across 3 consecutive passes; Phase 2 removes the swallow
		//    filter and proves production's coalescer lets delayedLayout fire once and settle to 0.
		{
			auto* profileCombo = control<QComboBox>(window, Vpv::ObjectName(QStringLiteral("profile")));
			QAbstractItemView* profileView = profileCombo->view();
			require(profileView != nullptr && !profileView->isVisible(),
				"Item 5 busy-spin check: expected vpv.profile popup view to be hidden");

			std::vector<std::pair<QString, QVariant>> savedItems;
			savedItems.reserve(static_cast<size_t>(profileCombo->count()));
			for (int i = 0; i < profileCombo->count(); ++i)
				savedItems.emplace_back(profileCombo->itemText(i), profileCombo->itemData(i));
			require(!savedItems.empty(), "Item 5 busy-spin check: vpv.profile has no items to mutate");
			const int savedIdx = profileCombo->currentIndex();

			struct ViewTimerSpy : QObject
			{
				int timerEvents = 0;
				bool eventFilter(QObject* receiver, QEvent* event) override
				{
					if (event && event->type() == QEvent::Timer) ++timerEvents;
					return QObject::eventFilter(receiver, event);
				}
			};
			struct SwallowTimerFilter : QObject
			{
				bool eventFilter(QObject* receiver, QEvent* event) override
				{
					if (event && event->type() == QEvent::Timer) return true;
					return QObject::eventFilter(receiver, event);
				}
			};
			ViewTimerSpy timerSpy;

			// Phase 1 (negative control): install SwallowTimerFilter first, then timerSpy second so
			// timerSpy runs first and observes every spin tick before SwallowTimerFilter consumes it.
			{
				SwallowTimerFilter swallow;
				ObjectEventFilterGuard swallowGuard(profileView, &swallow);
				ObjectEventFilterGuard spyGuard(profileView, &timerSpy);
				{
					const QSignalBlocker blocker(profileCombo);
					profileCombo->addItem(QStringLiteral("busy_spin_probe"), QStringLiteral("probe"));
					profileCombo->clear();
					for (const auto& item : savedItems)
						profileCombo->addItem(item.first, item.second);
					profileCombo->setCurrentIndex(savedIdx);
				}
				int badPassTimers[3] = {};
				for (int pass = 0; pass < 3; ++pass)
				{
					timerSpy.timerEvents = 0;
					QCoreApplication::processEvents();
					badPassTimers[pass] = timerSpy.timerEvents;
				}
				require(badPassTimers[0] > 0 && badPassTimers[1] > 0 && badPassTimers[2] > 0,
					("Item 5 busy-spin negative control: swallowing QEvent::Timer did not trigger delayedLayout spin "
						"(badPasses=[" + std::to_string(badPassTimers[0]) + "," +
						std::to_string(badPassTimers[1]) + "," + std::to_string(badPassTimers[2]) + "])").c_str());
			}

			// Phase 2 (production behavior): without SwallowTimerFilter, delayedLayout fires and stops cleanly.
			{
				ObjectEventFilterGuard spyGuard(profileView, &timerSpy);
				int passTimers[5] = {};
				for (int pass = 0; pass < 5; ++pass)
				{
					timerSpy.timerEvents = 0;
					QCoreApplication::processEvents();
					passTimers[pass] = timerSpy.timerEvents;
				}
				require(passTimers[0] + passTimers[1] > 0,
					("Item 5 busy-spin check: delayedLayout timer did not fire and clear after removing swallow filter "
						"(passes=[" + std::to_string(passTimers[0]) + "," + std::to_string(passTimers[1]) + "])").c_str());
				require(passTimers[2] == 0 && passTimers[3] == 0 && passTimers[4] == 0,
					("Item 5 busy-spin regression: hidden VP-V QAbstractItemView kept receiving QEvent::Timer "
						"(passes=[" + std::to_string(passTimers[0]) + "," + std::to_string(passTimers[1]) + "," +
						std::to_string(passTimers[2]) + "," + std::to_string(passTimers[3]) + "," +
						std::to_string(passTimers[4]) + "])").c_str());
			}
		}

		// 4. Hidden vpvRow .reset_override positive-count guard + zero per-widget stylesheets + Bill negative control.
		int hiddenVpvResetTotal = 0;
		int hiddenVpvResetStyled = 0;
		int visibleBillResetStyled = 0;
		for (QPushButton* btn : window.findChildren<QPushButton*>())
		{
			if (!btn->objectName().endsWith(QStringLiteral(".reset_override"))) continue;
			const QWidget* holder = btn->parentWidget();
			const bool isHiddenVpvReset = holder && holder->isHidden() &&
				(qobject_cast<const QComboBox*>(holder->parentWidget()) != nullptr);
			if (isHiddenVpvReset)
			{
				++hiddenVpvResetTotal;
				if (!btn->styleSheet().isEmpty()) ++hiddenVpvResetStyled;
			}
			else if (holder && !holder->isHidden() && !btn->styleSheet().isEmpty())
			{
				++visibleBillResetStyled;
			}
		}
		require(hiddenVpvResetTotal > 0,
			"Item 5 non-vacuous guard failed: expected > 0 hidden vpvRow .reset_override buttons");
		require(visibleBillResetStyled > 0,
			"Item 5 negative control failed: expected at least one visible Bill .reset_override button to retain its stylesheet");
		require(hiddenVpvResetStyled == 0,
			("Item 5: found " + std::to_string(hiddenVpvResetStyled) + " of " + std::to_string(hiddenVpvResetTotal) +
				" hidden vpvRow .reset_override buttons retaining non-empty stylesheets").c_str());

		// 5. Real show-sync proof on 3 combos (header, VP-V toggle with forced overflow scrollbar,
		//    and moved config.vprenderer.* VP-V combo with >= 4 items): verify layout rects, vertical
		//    scrollbar sync on overflow, and non-suppression of events while popup is open.
		window.selectPage(QStringLiteral("VP-V"));
		window.show();
		QCoreApplication::processEvents();
		auto* pathway = control<QComboBox>(window, Vpv::ObjectName(QStringLiteral("pathway")));
		auto* units = control<QTabWidget>(window, Vpv::ObjectName(QStringLiteral("units")));
		auto* repairTab = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("repair")));
		auto* scalingTab = control<QWidget>(window, Vpv::ObjectName(QStringLiteral("scaling")));
		auto* cutSoftReset = control<QComboBox>(window, Vpv::ObjectName(QStringLiteral("repair.cut_soft_reset")));
		auto* chromaVpv = control<QComboBox>(window, QStringLiteral("config.vprenderer.scaling.chroma_upscaler.vpv"));

		auto verifyPopupSync = [](QComboBox* combo, const char* label, int minItems)
		{
			require(combo->count() >= minItems,
				(std::string("Item 5 show-sync: combo has too few items (") + std::to_string(combo->count()) +
					" < " + std::to_string(minItems) + "): " + label).c_str());

			combo->showPopup();
			QCoreApplication::processEvents();
			QAbstractItemView* view = combo->view();
			require(view && view->isVisible(),
				(std::string("Item 5 show-sync: popup view did not become visible on showPopup(): ") + label).c_str());
			require(view->width() > 0 && view->height() > 0,
				(std::string("Item 5 show-sync: popup view has degenerate geometry: ") + label).c_str());
			const auto* listView = qobject_cast<QListView*>(view);
			int rowA = -1;
			int rowB = -1;
			for (int r = 0; r < combo->count(); ++r)
			{
				if (listView && listView->isRowHidden(r)) continue;
				if (rowA < 0) rowA = r;
				else { rowB = r; break; }
			}
			require(rowA >= 0 && rowB > rowA,
				(std::string("Item 5 show-sync: fewer than 2 non-hidden popup rows on ") + label).c_str());
			const QRect r0 = view->visualRect(combo->model()->index(rowA, 0));
			const QRect r1 = view->visualRect(combo->model()->index(rowB, 0));
			require(!r0.isEmpty() && !r1.isEmpty() && r0.bottom() < r1.top(),
				(std::string("Item 5 show-sync: unlaid-out or overlapping item visualRects on ") + label).c_str());

			// Direct MetaCall delivery check while open: coalescer must NOT suppress QEvent::MetaCall on
			// an actively open popup view.
			bool openMetaCallDelivered = false;
			QMetaObject::invokeMethod(view, [&openMetaCallDelivered]() { openMetaCallDelivered = true; }, Qt::QueuedConnection);
			QCoreApplication::processEvents();
			require(openMetaCallDelivered,
				(std::string("Item 5 show-sync: coalescer suppressed MetaCall on open popup view: ") + label).c_str());

			// Interactive selection while open: open popup view must genuinely track the selection.
			const int origIdx = combo->currentIndex();
			const int altIdx = (origIdx == rowA) ? rowB : rowA;
			combo->setCurrentIndex(altIdx);
			QCoreApplication::processEvents();
			require(view->isVisible() && combo->currentIndex() == altIdx && view->currentIndex().row() == altIdx,
				(std::string("Item 5 show-sync: open popup view did not track interactive selection on ") + label).c_str());
			combo->setCurrentIndex(origIdx);
			QCoreApplication::processEvents();

			combo->hidePopup();
			QCoreApplication::processEvents();
			bool postHideDelivered = false;
			QMetaObject::invokeMethod(view, [&postHideDelivered]() { postHideDelivered = true; }, Qt::QueuedConnection);
			QCoreApplication::processEvents();
			require(!postHideDelivered,
				(std::string("Item 5 show-sync: coalescer did not re-engage after hidePopup() on ") + label).c_str());
		};

		verifyPopupSync(pathway, "vpv.pathway", 2);
		units->setCurrentWidget(repairTab);
		QCoreApplication::processEvents();
		verifyPopupSync(cutSoftReset, "vpv.repair.cut_soft_reset", 3);
		units->setCurrentWidget(scalingTab);
		QCoreApplication::processEvents();
		verifyPopupSync(chromaVpv, "config.vprenderer.scaling.chroma_upscaler.vpv", 4);

		// Genuine Fusion-overflowing scrollbar sync check (matching ConfigEditorWindowTests.cpp:1291):
		// in Qt 6 Fusion (SH_ComboBox_Popup == true), a popup overflows only when its item height
		// exceeds screen height; add 60 items while closed, then open and assert verticalScrollBar syncs.
		{
			auto* profileCombo = control<QComboBox>(window, Vpv::ObjectName(QStringLiteral("profile")));
			std::vector<std::pair<QString, QVariant>> savedItems;
			savedItems.reserve(static_cast<size_t>(profileCombo->count()));
			for (int i = 0; i < profileCombo->count(); ++i)
				savedItems.emplace_back(profileCombo->itemText(i), profileCombo->itemData(i));
			const int savedIdx = profileCombo->currentIndex();
			struct RestoreProfileCombo
			{
				QComboBox* cb;
				const std::vector<std::pair<QString, QVariant>>* items;
				int idx;
				~RestoreProfileCombo()
				{
					if (!cb) return;
					cb->hidePopup();
					const QSignalBlocker blocker(cb);
					cb->clear();
					for (const auto& it : *items) cb->addItem(it.first, it.second);
					cb->setCurrentIndex(idx);
				}
			} restoreGuard{ profileCombo, &savedItems, savedIdx };

			{
				const QSignalBlocker blocker(profileCombo);
				for (int i = 0; i < 60; ++i)
					profileCombo->addItem(QStringLiteral("Overflow probe %1").arg(i), QStringLiteral("probe_%1").arg(i));
			}
			QCoreApplication::processEvents();

			profileCombo->showPopup();
			QCoreApplication::processEvents();
			QAbstractItemView* view = profileCombo->view();
			require(view && view->isVisible(),
				"Item 5 overflow show-sync: vpv.profile popup view did not become visible");
			require(view->width() > 0 && view->height() > 0,
				"Item 5 overflow show-sync: vpv.profile popup view has degenerate geometry");
			QScrollBar* bar = view->verticalScrollBar();
			require(bar != nullptr && bar->maximum() > 0,
				"Item 5 overflow show-sync: verticalScrollBar did not sync maximum > 0 on 60-item Fusion popup");
			if (view->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff)
			{
				require(bar->isVisible(),
					"Item 5 overflow show-sync: verticalScrollBar is not visible when policy allows scrollbars");
			}
			else
			{
				QWidget* container = view->parentWidget();
				require(container != nullptr,
					"Item 5 overflow show-sync: popup view has no container parent under ScrollBarAlwaysOff");
				bool hasVisibleScroller = false;
				for (QWidget* child : container->findChildren<QWidget*>())
				{
					if (child->metaObject() &&
						std::strcmp(child->metaObject()->className(), "QComboBoxPrivateScroller") == 0 &&
						child->isVisible())
					{
						hasVisibleScroller = true;
						break;
					}
				}
				require(hasVisibleScroller,
					"Item 5 overflow show-sync: expected a visible QComboBoxPrivateScroller under Fusion ScrollBarAlwaysOff");
			}
			const int beforeScroll = bar->value();
			bar->setValue(bar->maximum());
			QCoreApplication::processEvents();
			require(bar->value() > beforeScroll,
				"Item 5 overflow show-sync: open 60-item popup did not scroll");
			profileCombo->hidePopup();
			QCoreApplication::processEvents();
		}

		// CT-M9 hardened show-sync proof + in-test negative control:
		// Prime a 2-item popup under Qt::ScrollBarAsNeeded (using combobox-popup: 0 so VpStyle/Fusion
		// honours ScrollBarAsNeeded instead of forcing ScrollBarAlwaysOff), add 60 items while closed,
		// open WITHOUT any intermediate processEvents(), and assert bar->isVisible() && bar->maximum() > 0.
		// Then run an identical QComboBox with a naive SwallowHiddenMetaCallOnlyFilter (swallows hidden
		// MetaCalls, does nothing on QEvent::Show) at the cached 2-item popup geometry (no resize) to
		// prove SyncPopupForShow is genuinely exercised and load-bearing.
		{
			auto* profileCombo = control<QComboBox>(window, Vpv::ObjectName(QStringLiteral("profile")));
			QAbstractItemView* prodView = profileCombo->view();
			require(prodView != nullptr, "CT-M9: vpv.profile has no popup view");

			std::vector<std::pair<QString, QVariant>> savedItems;
			savedItems.reserve(static_cast<size_t>(profileCombo->count()));
			for (int i = 0; i < profileCombo->count(); ++i)
				savedItems.emplace_back(profileCombo->itemText(i), profileCombo->itemData(i));
			const int savedIdx = profileCombo->currentIndex();
			const int savedMaxVisible = profileCombo->maxVisibleItems();
			const QString savedStyle = profileCombo->styleSheet();
			const Qt::ScrollBarPolicy savedPolicy = prodView->verticalScrollBarPolicy();
			struct RestoreCtM9Combo
			{
				QComboBox* cb;
				QAbstractItemView* view;
				const std::vector<std::pair<QString, QVariant>>* items;
				int idx;
				int maxVisible;
				QString style;
				Qt::ScrollBarPolicy policy;
				~RestoreCtM9Combo()
				{
					if (!cb) return;
					if (view && view->parentWidget()) view->parentWidget()->hide();
					cb->hidePopup();
					cb->setStyleSheet(style);
					cb->setMaxVisibleItems(maxVisible);
					if (view) view->setVerticalScrollBarPolicy(policy);
					const QSignalBlocker blocker(cb);
					cb->clear();
					for (const auto& it : *items) cb->addItem(it.first, it.second);
					cb->setCurrentIndex(idx);
				}
			} restoreCtM9{ profileCombo, prodView, &savedItems, savedIdx, savedMaxVisible, savedStyle, savedPolicy };

			auto primeTwoItems = [](QComboBox* cb, const char* label)
			{
				cb->setStyleSheet(QStringLiteral("QComboBox { combobox-popup: 0; }"));
				cb->setMaxVisibleItems(8);
				QAbstractItemView* v = cb->view();
				require(v != nullptr, (std::string("CT-M9: missing view on ") + label).c_str());
				v->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
				{
					const QSignalBlocker blocker(cb);
					cb->clear();
					cb->addItem(QStringLiteral("Prime A"), QStringLiteral("prime_a"));
					cb->addItem(QStringLiteral("Prime B"), QStringLiteral("prime_b"));
				}
				cb->showPopup();
				QCoreApplication::processEvents();
				QScrollBar* bar = v->verticalScrollBar();
				require(v->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
					(std::string("CT-M9: expected ScrollBarAsNeeded on ") + label).c_str());
				require(bar != nullptr && !bar->isVisible() && bar->maximum() == 0,
					(std::string("CT-M9: primed 2-item popup must cache hidden zero-range scrollbar on ") + label).c_str());
				cb->hidePopup();
				QCoreApplication::processEvents();
			};

			// Part A: real showPopup() WITHOUT any intermediate processEvents() after adding 60 items.
			primeTwoItems(profileCombo, "vpv.profile (Part A)");
			{
				const std::uint64_t deferredBefore = Vpv::DeferredHiddenPopupEventCount();
				bool armedDelivered = false;
				QMetaObject::invokeMethod(prodView, [&armedDelivered]() { armedDelivered = true; }, Qt::QueuedConnection);
				QCoreApplication::processEvents();
				require(!armedDelivered && Vpv::DeferredHiddenPopupEventCount() > deferredBefore,
					"CT-M9 Part A: hidden coalescer did not suppress closed MetaCall to arm show-sync");

				{
					const QSignalBlocker blocker(profileCombo);
					for (int i = 0; i < 60; ++i)
						profileCombo->addItem(QStringLiteral("CT-M9 item %1").arg(i), QStringLiteral("ctm9_%1").arg(i));
				}
				// Deliberately NO QCoreApplication::processEvents() before or after showPopup().
				profileCombo->showPopup();
				QScrollBar* bar = prodView->verticalScrollBar();
				require(prodView->isVisible(),
					"CT-M9 Part A: popup view did not become visible on showPopup() without processEvents()");
				require(bar != nullptr && bar->isVisible() && bar->maximum() > 0,
					"CT-M9 Part A: verticalScrollBar must be visible with maximum > 0 immediately on showPopup() without processEvents()");
				profileCombo->hidePopup();
				QCoreApplication::processEvents();
			}

			// Part B: same-input in-test negative control at cached 2-item geometry (no resize runs).
			struct SwallowHiddenMetaCallOnlyFilter : QObject
			{
				int swallowedCount = 0;
				bool eventFilter(QObject* obj, QEvent* ev) override
				{
					if (ev && ev->type() == QEvent::MetaCall)
					{
						if (auto* v = qobject_cast<QAbstractItemView*>(obj))
						{
							if (!v->isVisible())
							{
								++swallowedCount;
								return true;
							}
						}
					}
					// Deliberately does NOTHING on QEvent::Show (naive unprotected baseline).
					return false;
				}
			};

			primeTwoItems(profileCombo, "vpv.profile (Part B)");

			QComboBox naiveCombo(&window);
			naiveCombo.setGeometry(profileCombo->geometry());
			naiveCombo.show();
			QCoreApplication::processEvents();
			QAbstractItemView* naiveView = naiveCombo.view();
			require(naiveView != nullptr, "CT-M9 negative control: naive combo has no view");
			SwallowHiddenMetaCallOnlyFilter naiveFilter;
			ObjectEventFilterGuard naiveFilterGuard(naiveView, &naiveFilter);
			primeTwoItems(&naiveCombo, "naiveCombo (Part B)");

			bool prodDelivered = false;
			bool naiveDelivered = false;
			QMetaObject::invokeMethod(prodView, [&prodDelivered]() { prodDelivered = true; }, Qt::QueuedConnection);
			QMetaObject::invokeMethod(naiveView, [&naiveDelivered]() { naiveDelivered = true; }, Qt::QueuedConnection);
			QCoreApplication::processEvents();
			require(!prodDelivered && !naiveDelivered && naiveFilter.swallowedCount > 0,
				"CT-M9 Part B: both production and naive filters must swallow hidden MetaCall");

			// Mutate both combos with the exact same 60 items while closed, with NO intermediate processEvents().
			{
				const QSignalBlocker blockerA(profileCombo);
				const QSignalBlocker blockerB(&naiveCombo);
				for (int i = 0; i < 60; ++i)
				{
					const QString text = QStringLiteral("CT-M9 neg %1").arg(i);
					const QString data = QStringLiteral("ctm9_neg_%1").arg(i);
					profileCombo->addItem(text, data);
					naiveCombo.addItem(text, data);
				}
			}

			// Show both popup containers directly at their cached 2-item geometry (no resize, no processEvents()).
			QWidget* prodContainer = prodView->parentWidget();
			QWidget* naiveContainer = naiveView->parentWidget();
			require(prodContainer != nullptr && naiveContainer != nullptr,
				"CT-M9 Part B: primed popups have no container widget");
			prodContainer->show();
			naiveContainer->show();

			QScrollBar* prodBar = prodView->verticalScrollBar();
			QScrollBar* naiveBar = naiveView->verticalScrollBar();
			require(prodBar != nullptr && prodBar->isVisible() && prodBar->maximum() > 0,
				"CT-M9 Part B: production HiddenComboPopupCoalescer failed to sync scrollbar on cached-geometry show");
			require(naiveBar != nullptr && !(naiveBar->isVisible() && naiveBar->maximum() > 0),
				"CT-M9 negative control failed: naive SwallowHiddenMetaCallOnlyFilter unexpectedly synced scrollbar without SyncPopupForShow");

			prodContainer->hide();
			naiveContainer->hide();
			QCoreApplication::processEvents();
		}
		window.hide();
		QCoreApplication::processEvents();
	}

	// S4-M1: adaptive_motion continuum control (off / on / 0..1 strength) and greying of the 4 motion tuning rows.
	void testAdaptiveMotionContinuumAndGreying()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[vprenderer]\nsdr_target_nits: 100\nadaptive_motion: 0.5\n"
			"[vprenderer.Cinema]\nquality: high\nadaptive_motion: off\n"
			"[queue]\ntarget_latency_ms: 250\n");

		static const char* kMotionRows[] = {
			"config.vprenderer.adaptive_motion_speed_lo",
			"config.vprenderer.adaptive_motion_speed_hi",
			"config.vprenderer.adaptive_motion_floor",
			"config.vprenderer.adaptive_motion_smooth_ms",
		};
		auto requireRowsEnabled = [](ConfigEditorWindow& win, bool expected, const char* context)
		{
			for (const char* name : kMotionRows)
			{
				auto* w = control<QWidget>(win, QString::fromLatin1(name));
				require(w->isEnabled() == expected,
					(std::string("S4-M1 (") + context + "): " + name +
						(expected ? " must be enabled" : " must be disabled")).c_str());
			}
		};
		auto selectMode = [](QComboBox* mode, const QString& token)
		{
			int idx = mode->findData(token);
			if (idx < 0)
			{
				for (int i = 0; i < mode->count(); ++i)
				{
					if (mode->itemData(i).toString().compare(token, Qt::CaseInsensitive) == 0 ||
						mode->itemText(i).compare(token, Qt::CaseInsensitive) == 0)
					{
						idx = i;
						break;
					}
				}
			}
			require(idx >= 0, ("S4-M1: adaptive_motion.mode missing option '" + token.toStdString() + "'").c_str());
			mode->setCurrentIndex(idx);
			QCoreApplication::processEvents();
		};
		auto readBacking = [](QWidget* backing) -> QString
		{
			if (auto* le = qobject_cast<QLineEdit*>(backing)) return le->text().trimmed();
			if (auto* cb = qobject_cast<QComboBox*>(backing))
			{
				const QString data = cb->currentData().toString().trimmed();
				return data.isEmpty() ? cb->currentText().trimmed() : data;
			}
			return backing->property("effectiveValue").toString().trimmed();
		};

		ConfigEditorWindow window(path, 0, true);
		QCoreApplication::processEvents();

		// Initial load with adaptive_motion: 0.5 -> all 4 motion tuning rows enabled.
		requireRowsEnabled(window, true, "loaded adaptive_motion: 0.5");

		// Continuum controls exist and display 0.5.
		auto* mode = window.findChild<QComboBox*>(QStringLiteral("config.vprenderer.adaptive_motion.mode"));
		auto* strength = window.findChild<QDoubleSpinBox*>(QStringLiteral("config.vprenderer.adaptive_motion.strength"));
		require(mode != nullptr, "S4-M1: missing config.vprenderer.adaptive_motion.mode QComboBox");
		require(strength != nullptr, "S4-M1: missing config.vprenderer.adaptive_motion.strength QDoubleSpinBox");
		require(!strength->isHidden() && strength->isEnabled(),
			"S4-M1: adaptive_motion.strength must be visible and enabled for adaptive_motion: 0.5");
		require(std::abs(strength->value() - 0.5) < 1e-4,
			("S4-M1: expected adaptive_motion.strength == 0.5, got " + std::to_string(strength->value())).c_str());

		// Change strength to 0.25 -> backing control updates, save & reload persists 0.25.
		strength->setValue(0.25);
		QCoreApplication::processEvents();
		auto* backing = control<QWidget>(window, QStringLiteral("config.vprenderer.adaptive_motion"));
		const QString backingStr = readBacking(backing);
		bool okBacking = false;
		const double backingVal = backingStr.toDouble(&okBacking);
		require(okBacking && std::abs(backingVal - 0.25) < 1e-4,
			("S4-M1: backing config.vprenderer.adaptive_motion did not reflect 0.25, got '" + backingStr.toStdString() + "'").c_str());
		requireRowsEnabled(window, true, "strength 0.25");

		save(window);
		requireVpReads(path);
		{
			ConfigFile reloaded;
			std::string stored;
			require(reloaded.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh) &&
				reloaded.TryGetString(DefaultProfile(path), "adaptive_motion", stored),
				"S4-M1: adaptive_motion missing after save");
			bool okStored = false;
			const double storedVal = QString::fromStdString(stored).trimmed().toDouble(&okStored);
			require(okStored && std::abs(storedVal - 0.25) < 1e-4,
				("S4-M1: saved adaptive_motion expected 0.25, got '" + stored + "'").c_str());
			const std::string fileText = FileText(path);
			require(fileText.find("adaptive_motion: 0.25") != std::string::npos,
				("S4-M1: saved file does not contain 'adaptive_motion: 0.25':\n" + fileText).c_str());
		}

		// Negative control: strength 0.0 (and mode "off") disables all 4 motion tuning rows.
		if (strength->minimum() <= 0.0)
		{
			strength->setValue(0.0);
			QCoreApplication::processEvents();
			requireRowsEnabled(window, false, "negative control strength 0.0");
		}
		selectMode(mode, QStringLiteral("off"));
		requireRowsEnabled(window, false, "negative control mode off");

		// Re-enabling via mode "on" restores all 4 motion tuning rows.
		selectMode(mode, QStringLiteral("on"));
		requireRowsEnabled(window, true, "re-enabled mode on");

		// Switching profiles updates enabled state cleanly: Cinema (off) -> disabled; Default -> enabled.
		auto* profiles = control<QListWidget>(window, QStringLiteral("config.vprenderer.profiles"));
		require(profiles->count() >= 2, "S4-M1: expected at least 2 rendering profiles");
		profiles->setCurrentRow(1);
		QCoreApplication::processEvents();
		requireRowsEnabled(window, false, "profile switched to Cinema (adaptive_motion: off)");

		profiles->setCurrentRow(0);
		QCoreApplication::processEvents();
		requireRowsEnabled(window, true, "profile switched back to Default");
	}

	// S4-L1: pipeline_slots_off / pipeline_order case-insensitive parsing, lowercase canonical storage,
	// and exact-token matching (no substring false positives).
	void testPipelineSlotsOffCaseInsensitiveAndExactTokens()
	{
		// 1. CalibrationLutHost::DisplayMappingStepsRun: mixed-case disable & substring negative controls.
		require(!CalibrationLutHost::DisplayMappingStepsRun(false, "vp", false, true, "Colour.Appearance"),
			"S4-L1: mixed-case 'Colour.Appearance' must disable appearance step");
		require(CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, true, "Colour.Appearance"),
			"S4-L1: disabling 'Colour.Appearance' must keep dv_trims step active");

		require(!CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, false, "Colour.DM_Trims"),
			"S4-L1: mixed-case 'Colour.DM_Trims' must disable dm_trims step");
		require(CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, true, "Colour.DM_Trims"),
			"S4-L1: disabling 'Colour.DM_Trims' must keep appearance step active");

		require(!CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, true, "Colour.Appearance, Colour.DM_Trims"),
			"S4-L1: mixed-case disabling both colour steps must return false");

		// Negative control (substring trap): prefix/suffix lookalikes must NOT disable real slots.
		require(CalibrationLutHost::DisplayMappingStepsRun(false, "vp", false, true, "colour.appearance_v2, my_colour.dm_trims_extra"),
			"S4-L1 negative control: 'colour.appearance_v2' must NOT match 'colour.appearance'");
		require(CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, false, "colour.appearance_v2, my_colour.dm_trims_extra"),
			"S4-L1 negative control: 'my_colour.dm_trims_extra' must NOT match 'colour.dm_trims'");
		require(CalibrationLutHost::DisplayMappingStepsRun(false, "vp", true, true, "colour.appearance_v2, my_colour.dm_trims_extra"),
			"S4-L1 negative control: substring lookalikes must leave both colour steps active");

		// 2. VpvSection expert pipeline_slots_off & pipeline_order inputs accept mixed case and store lowercase.
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* slotsOff = control<QLineEdit>(window, Vpv::ObjectName("expert.pipeline_slots_off"));
			auto* slotsStatus = control<QLabel>(window, Vpv::ObjectName("expert.pipeline_slots_off.status"));
			auto* order = control<QLineEdit>(window, Vpv::ObjectName("expert.pipeline_order"));
			auto* orderStatus = control<QLabel>(window, Vpv::ObjectName("expert.pipeline_order.status"));

			// Negative control: unknown lookalike must still be rejected.
			slotsOff->setText(QStringLiteral("Colour.Appearance_v2"));
			QMetaObject::invokeMethod(slotsOff, "editingFinished", Qt::DirectConnection);
			require(slotsStatus->text().startsWith(QStringLiteral("Not saved:")),
				("S4-L1 negative control: unknown 'Colour.Appearance_v2' must be rejected, got: " +
					slotsStatus->text().toStdString()).c_str());

			// Valid mixed-case entries must be accepted without error/unknown.
			slotsOff->setText(QStringLiteral("Colour.Appearance"));
			QMetaObject::invokeMethod(slotsOff, "editingFinished", Qt::DirectConnection);
			require(!slotsStatus->text().contains(QStringLiteral("unknown"), Qt::CaseInsensitive) &&
				!slotsStatus->text().startsWith(QStringLiteral("Not saved:")),
				("S4-L1: mixed-case 'Colour.Appearance' rejected: " + slotsStatus->text().toStdString()).c_str());

			order->setText(QStringLiteral("Repair.Encoding"));
			QMetaObject::invokeMethod(order, "editingFinished", Qt::DirectConnection);
			require(!orderStatus->text().contains(QStringLiteral("unknown"), Qt::CaseInsensitive) &&
				!orderStatus->text().startsWith(QStringLiteral("Not saved:")),
				("S4-L1: mixed-case 'Repair.Encoding' rejected: " + orderStatus->text().toStdString()).c_str());

			save(window);
		}
		ConfigFile saved;
		std::string v;
		require(saved.Load(path.toStdString(), ConfigFile::ReadPolicy::Fresh), "S4-L1: reload saved config");
		require(saved.TryGetString("vpv", "pipeline_slots_off", v) && v == "colour.appearance",
			("S4-L1: expected saved pipeline_slots_off == 'colour.appearance', got '" + v + "'").c_str());
		require(saved.TryGetString("vpv", "pipeline_order", v) && v == "repair.encoding",
			("S4-L1: expected saved pipeline_order == 'repair.encoding', got '" + v + "'").c_str());
		requireVpReads(path);
	}

	// S4-L2: appearance status label evaluates appearance / appearance_screen_white_measured / ambient_adapt
	// case-insensitively (e.g. ON / On).
	void testAppearanceStatusCaseInsensitive()
	{
		// Mixed-case ON / On must report the active appearance status line (NOT the off line).
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[vprenderer]\nappearance: ON\nappearance_screen_white_measured: On\nambient_adapt: ON\n"
				"[queue]\ntarget_latency_ms: 250\n");
			ConfigEditorWindow window(path, 0, true);
			auto* body = control<QWidget>(window, Vpv::ObjectName("colour.card.appearance.body"));
			auto* status = body->findChild<QLabel*>(Vpv::ObjectName("colour.appearance.status"));
			require(status != nullptr, "S4-L2: missing appearance status QLabel inside vpv.colour.card.appearance.body");
			const QString expected = QString::fromStdString(AppearanceStatus::ConfigLine(true, true, true));
			const QString offLine = QString::fromStdString(AppearanceStatus::ConfigLine(false, false, false));
			require(status->text() != offLine && status->text() == expected,
				("S4-L2: expected '" + expected.toStdString() + "', got '" + status->text().toStdString() + "'").c_str());
		}

		// Negative control: appearance: off must report ConfigLine(false, ...).
		{
			QTemporaryDir dir;
			const QString path = WriteFixture(dir,
				"[vprenderer]\nappearance: off\nappearance_screen_white_measured: On\nambient_adapt: ON\n"
				"[queue]\ntarget_latency_ms: 250\n");
			ConfigEditorWindow window(path, 0, true);
			auto* body = control<QWidget>(window, Vpv::ObjectName("colour.card.appearance.body"));
			auto* status = body->findChild<QLabel*>(Vpv::ObjectName("colour.appearance.status"));
			require(status != nullptr, "S4-L2 negative control: missing appearance status QLabel");
			const QString expectedOff = QString::fromStdString(AppearanceStatus::ConfigLine(false, true, true));
			require(status->text() == expectedOff,
				("S4-L2 negative control: expected '" + expectedOff.toStdString() + "', got '" +
					status->text().toStdString() + "'").c_str());
		}
	}

	// Migrations & UTF-8 hygiene: migrateLegacyQueueOwnership preserves surrounding UTF-8 bytes and folds
	// legacy keys cleanly, and migrateRefreshKeys triggers a migration backup (.bak) of the original file.
	void testLegacyQueueMigrationPreservesUtf8()
	{
		// 1. Legacy queue ownership migration alongside non-ASCII UTF-8 config content.
		{
			QTemporaryDir dir;
			const QByteArray utf8Note = QByteArray("Caf\xC3\xA9_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
			const QByteArray fixture =
				QByteArray("# UTF-8 note: ") + utf8Note + QByteArray("\n"
				"[general]\nrenderer: VP Renderer\n"
				"[queue_recovery]\nreset_after_render_restart_seconds: 9\nreset_queue_too_large_percent: 120\n"
				"[queue.Normal]\nqueue_size: 32\nsteady_reserve_frames: 4\n"
				"[vprenderer]\ndv_compare_capture_folder: C:/captures/") + utf8Note + QByteArray("\n");
			const QString path = dir.filePath(QStringLiteral("VideoProcessor.cfg"));
			{
				QFile f(path);
				require(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "UTF-8 queue migration: cannot write fixture");
				require(f.write(fixture) == fixture.size(), "UTF-8 queue migration: short fixture write");
			}
			{
				ConfigEditorWindow window(path, 0, true);
				save(window);
			}
			QFile savedFile(path);
			require(savedFile.open(QIODevice::ReadOnly), "UTF-8 queue migration: cannot read saved file");
			const QByteArray savedBytes = savedFile.readAll().replace("\r\n", "\n");
			require(savedBytes.contains(QByteArray("# UTF-8 note: ") + utf8Note),
				"UTF-8 queue migration: UTF-8 comment bytes were corrupted");
			require(savedBytes.contains(QByteArray("dv_compare_capture_folder: C:/captures/") + utf8Note),
				"UTF-8 queue migration: UTF-8 path value bytes were corrupted");
			require(savedBytes.contains("reset_after_render_restart_seconds: 9") &&
				savedBytes.contains("reset_queue_too_large_percent: 120") &&
				savedBytes.contains("target_frames: 4") &&
				!savedBytes.contains("steady_reserve_frames"),
				"UTF-8 queue migration: legacy queue keys did not migrate into [queue.Normal]");
		}

		// 2. migrateRefreshKeys must produce a migration backup (.before-config-migration.*.bak)
		//    holding the original un-migrated bytes when the config existed at load.
		{
			QTemporaryDir dir;
			const char* original = "[general]\nrenderer: VP Renderer\n[ppm_correction]\n59: 12\n60: 3\n";
			const QString path = WriteFixture(dir, original);
			{
				ConfigEditorWindow window(path, 0, true);
				save(window);
			}
			const QStringList backups = QDir(dir.path()).entryList(
				{ QStringLiteral("VideoProcessor.cfg.before-config-migration.*.bak") }, QDir::Files);
			require(!backups.isEmpty(),
				"migrateRefreshKeys: saving a refresh-key migration did not create a .before-config-migration.*.bak backup");
			QFile backupFile(dir.filePath(backups.front()));
			require(backupFile.open(QIODevice::ReadOnly), "migrateRefreshKeys: cannot read migration backup");
			const QByteArray backupBytes = backupFile.readAll().replace("\r\n", "\n");
			require(backupBytes.contains("59: 12") && !backupBytes.contains("59.94: 12"),
				"migrateRefreshKeys: migration backup does not contain the original pre-migration bytes");
		}
	}

	// GEM-040 Option A regression: managed upstream controls on Bill's Rendering page stay disabled
	// across Rendering profile switches (via ManagedBillGrey::OnBillField without setting derivedSetting,
	// preserving save-time freeze) and restore cleanly when switching to the Original pathway.
	void testManagedWidgetsStayDisabledAcrossProfileSwitch()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[vprenderer]\nsdr_target_nits: 100\n"
			"[vprenderer.Cinema]\nquality: high\n"
			"[queue]\ntarget_latency_ms: 250\n");

		ConfigEditorWindow window(path, 0, true);
		QCoreApplication::processEvents();

		static const char* kManagedNames[] = {
			"config.vprenderer.adaptive_upscale",
			"config.vprenderer.low_bitrate_handling",
			"config.vprenderer.capture_path",
		};
		auto checkManagedState = [&](bool expectManaged, const char* phase)
		{
			for (const char* name : kManagedNames)
			{
				auto* w = control<QWidget>(window, QString::fromLatin1(name));
				require(!w->property("derivedSetting").toBool(),
					(std::string("GEM-040 (") + phase + "): " + name +
						" must keep derivedSetting == false (preserves save-time freeze)").c_str());
				if (expectManaged)
				{
					require(!w->isEnabled() && w->property("vpvManaged").toBool() &&
						w->toolTip().startsWith(QStringLiteral("Managed by VP-V")),
						(std::string("GEM-040 (") + phase + "): " + name +
							" must remain disabled and marked Managed by VP-V").c_str());
				}
				else
				{
					require(w->isEnabled() && !w->property("vpvManaged").toBool(),
						(std::string("GEM-040 (") + phase + "): " + name +
							" must restore enabled with vpvManaged == false on Original pathway").c_str());
				}
			}
		};

		// Initial VP-V pathway load (Default profile).
		checkManagedState(true, "initial VP-V Default");

		// Switch Bill's Rendering profile list to row 1 (Cinema): managed widgets must stay disabled.
		auto* profiles = control<QListWidget>(window, QStringLiteral("config.vprenderer.profiles"));
		require(profiles->count() >= 2, "GEM-040: expected at least 2 rendering profiles");
		profiles->setCurrentRow(1);
		QCoreApplication::processEvents();
		checkManagedState(true, "VP-V Cinema profile");

		// Switch pathway to Original (index 1): managed widgets must restore enabled without derivedSetting.
		auto* pathway = control<QComboBox>(window, Vpv::ObjectName("pathway"));
		pathway->setCurrentIndex(1);
		QCoreApplication::processEvents();
		checkManagedState(false, "Original pathway on Cinema");

		// Round-trip back to VP-V (index 0) and Default profile (row 0): managed greying re-engages without derivedSetting.
		pathway->setCurrentIndex(0);
		QCoreApplication::processEvents();
		profiles->setCurrentRow(0);
		QCoreApplication::processEvents();
		checkManagedState(true, "back to VP-V Default");
	}

	// Unit 2 (Phase 1) Red-Team adversarial test 1: CAM16 appearance validated widgets (combo + spinboxes,
	// never free-text QLineEdits) and mutual exclusion against legacy room-light adaptation (ambient_adapt).
	void testCam16AppearanceWidgetsAndMutualExclusion()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[vprenderer]\nappearance: off\nambient_adapt: on\n"
			"[queue]\ntarget_latency_ms: 250\n");
		{
			ConfigEditorWindow window(path, 0, true);
			QCoreApplication::processEvents();

			// Validated widgets only: appearance_room is a QComboBox, strength & screen_white_nits are QDoubleSpinBoxes.
			require(window.findChild<QLineEdit*>(Vpv::ObjectName("colour.appearance_room")) == nullptr,
				"Unit 2: vpv.colour.appearance_room must be a QComboBox, not a QLineEdit");
			require(window.findChild<QLineEdit*>(Vpv::ObjectName("colour.appearance_strength")) == nullptr,
				"Unit 2: vpv.colour.appearance_strength must be a QDoubleSpinBox, not a QLineEdit");
			require(window.findChild<QLineEdit*>(Vpv::ObjectName("colour.appearance_screen_white_nits")) == nullptr,
				"Unit 2: vpv.colour.appearance_screen_white_nits must be a QDoubleSpinBox, not a QLineEdit");

			auto* appearance = control<QComboBox>(window, Vpv::ObjectName("colour.appearance"));
			auto* room = control<QComboBox>(window, Vpv::ObjectName("colour.appearance_room"));
			for (const char* val : { "cinema", "living_low", "living_lit", "auto" })
				require(room->findData(QString::fromLatin1(val)) >= 0,
					(std::string("Unit 2: appearance_room QComboBox missing option: ") + val).c_str());

			auto* strength = control<QDoubleSpinBox>(window, Vpv::ObjectName("colour.appearance_strength"));
			require(std::abs(strength->minimum() - 0.0) < 1e-6 && std::abs(strength->maximum() - 1.0) < 1e-6,
				"Unit 2: appearance_strength QDoubleSpinBox must have range 0.0..1.0");
			auto* whiteNits = control<QDoubleSpinBox>(window, Vpv::ObjectName("colour.appearance_screen_white_nits"));
			require(std::abs(whiteNits->minimum() - 1.0) < 1e-6 && std::abs(whiteNits->maximum() - 1000.0) < 1e-6,
				"Unit 2: appearance_screen_white_nits QDoubleSpinBox must have range 1.0..1000.0");

			auto* appearanceTuningBody = control<QWidget>(window, Vpv::ObjectName("colour.card.appearance_tuning.body"));
			auto* ambientAdapt = control<QComboBox>(window, Vpv::ObjectName("colour.ambient_adapt"));
			auto* roomTuningBody = control<QWidget>(window, Vpv::ObjectName("colour.card.room_tuning.body"));
			auto* replacedNote = control<QLabel>(window, Vpv::ObjectName("colour.room.replaced_note"));

			// Negative control (appearance=off, ambient_adapt=on): CAM16 controls disabled, legacy room-light enabled, replaced note hidden.
			require(!room->isEnabled() && !strength->isEnabled() && !whiteNits->isEnabled() && !appearanceTuningBody->isEnabled(),
				"Unit 2 negative control (appearance=off): CAM16 room/strength/white_nits/tuning must be disabled");
			require(appearance->isEnabled() && ambientAdapt->isEnabled() && roomTuningBody->isEnabled(),
				"Unit 2 negative control (appearance=off): appearance switch, ambient_adapt, and room_tuning must be enabled");
			require(replacedNote->isHidden(),
				"Unit 2 negative control (appearance=off): vpv.colour.room.replaced_note must be hidden");

			// Switch CAM16 appearance ON: CAM16 controls enable, legacy room-light adaptation disables & shows replaced note.
			const int onIdx = appearance->findData(QStringLiteral("on"));
			const int offIdx = appearance->findData(QStringLiteral("off"));
			require(onIdx >= 0 && offIdx >= 0, "Unit 2: appearance combo must offer on and off");
			appearance->setCurrentIndex(onIdx);
			QCoreApplication::processEvents();
			require(room->isEnabled() && strength->isEnabled() && whiteNits->isEnabled() && appearanceTuningBody->isEnabled(),
				"Unit 2 (appearance=on): CAM16 room/strength/white_nits/tuning must become enabled");
			require(!ambientAdapt->isEnabled() && !roomTuningBody->isEnabled(),
				"Unit 2 (appearance=on): legacy ambient_adapt and room_tuning must become disabled");
			require(!replacedNote->isHidden() && replacedNote->text().contains(QStringLiteral("Replaced by CAM16 Appearance")),
				("Unit 2 (appearance=on): vpv.colour.room.replaced_note must be visible with 'Replaced by CAM16 Appearance', got: " +
					S(replacedNote->text())).c_str());

			// Toggle back OFF to prove mutual exclusion reverses cleanly (not sticky).
			appearance->setCurrentIndex(offIdx);
			QCoreApplication::processEvents();
			require(!room->isEnabled() && !strength->isEnabled() && !whiteNits->isEnabled() && !appearanceTuningBody->isEnabled(),
				"Unit 2 (back to off): CAM16 controls must disable again");
			require(ambientAdapt->isEnabled() && roomTuningBody->isEnabled() && replacedNote->isHidden(),
				"Unit 2 (back to off): legacy ambient_adapt/room_tuning must re-enable and replaced_note must hide again");

			// Switch back ON, set validated CAM16 values, and save.
			appearance->setCurrentIndex(onIdx);
			QCoreApplication::processEvents();
			room->setCurrentIndex(room->findData(QStringLiteral("living_lit")));
			strength->setValue(0.75);
			QMetaObject::invokeMethod(strength, "editingFinished", Qt::DirectConnection);
			whiteNits->setValue(100.0);
			QMetaObject::invokeMethod(whiteNits, "editingFinished", Qt::DirectConnection);
			QCoreApplication::processEvents();
			save(window);
		}

		ConfigFile config;
		require(config.Load(path.toStdString()), "Unit 2: saved CAM16 config reloads");
		const std::string prof = DefaultProfile(path);
		std::string v;
		require(config.TryGetString(prof, "appearance", v) && Vpv::SameValue(v, "on"),
			"Unit 2: appearance=on saved");
		require(config.TryGetString(prof, "appearance_room", v) && Vpv::SameValue(v, "living_lit"),
			("Unit 2: appearance_room=living_lit saved, got: " + v).c_str());
		require(config.TryGetString(prof, "appearance_strength", v) && Vpv::SameValue(v, "0.75"),
			("Unit 2: appearance_strength=0.75 saved, got: " + v).c_str());
		require(config.TryGetString(prof, "appearance_screen_white_nits", v) && Vpv::SameValue(v, "100"),
			("Unit 2: appearance_screen_white_nits=100 saved, got: " + v).c_str());
		requireVpReads(path);

		// Reopen: exact round-trip AND mutual exclusion active on initial load.
		{
			ConfigEditorWindow reopened(path, 0, true);
			QCoreApplication::processEvents();
			auto* room = control<QComboBox>(reopened, Vpv::ObjectName("colour.appearance_room"));
			auto* strength = control<QDoubleSpinBox>(reopened, Vpv::ObjectName("colour.appearance_strength"));
			auto* whiteNits = control<QDoubleSpinBox>(reopened, Vpv::ObjectName("colour.appearance_screen_white_nits"));
			require(room->currentData().toString() == QStringLiteral("living_lit"),
				"Unit 2 reload: appearance_room round-tripped");
			require(std::abs(strength->value() - 0.75) < 1e-4,
				"Unit 2 reload: appearance_strength round-tripped");
			require(std::abs(whiteNits->value() - 100.0) < 1e-4,
				"Unit 2 reload: appearance_screen_white_nits round-tripped");
			require(!control<QComboBox>(reopened, Vpv::ObjectName("colour.ambient_adapt"))->isEnabled() &&
				!control<QWidget>(reopened, Vpv::ObjectName("colour.card.room_tuning.body"))->isEnabled(),
				"Unit 2 reload: mutual exclusion disables ambient_adapt and room_tuning on initial open");
			auto* replacedNote = control<QLabel>(reopened, Vpv::ObjectName("colour.room.replaced_note"));
			require(!replacedNote->isHidden() && replacedNote->text().contains(QStringLiteral("Replaced by CAM16 Appearance")),
				"Unit 2 reload: replaced_note visible on initial open when appearance=on");
		}
	}

	// Unit 2 (Phase 1) Red-Team adversarial test 2: Repair Low tier sticks with zero bounce (deband_vp=auto,
	// distinct from Classic=off and Mid=on) and Owner #3 clarity summaries exist & update meaningfully on all tiered units + Overview.
	void testRepairTierNoBounceAndAllTiersClaritySummary()
	{
		QTemporaryDir dir;
		const QString path = WriteFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			QCoreApplication::processEvents();

			auto* repairSlider = control<QSlider>(window, Vpv::ObjectName("repair.tier"));
			auto* repairName = control<QLabel>(window, Vpv::ObjectName("repair.tier.name"));
			auto* debandVp = control<QComboBox>(window, QStringLiteral("config.vprenderer.deband_vp"));
			auto effectiveDeband = [debandVp]()
			{
				const QString data = debandVp->currentData().toString();
				return data.isEmpty() ? debandVp->property("effectiveValue").toString() : data;
			};

			// Drag Repair slider to Low (1): must stick at 1 (no bounce to 0) and write deband_vp=auto.
			repairSlider->setValue(1);
			QCoreApplication::processEvents();
			require(repairSlider->value() == 1,
				"Unit 2: Repair Low (1) bounced back instead of sticking");
			require(repairName->text().contains(QStringLiteral("Low")),
				("Unit 2: Repair Low name label should contain 'Low', got: " + S(repairName->text())).c_str());
			const QString lowDeband = effectiveDeband();
			require(lowDeband.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0,
				("Unit 2: Repair Low must set deband_vp=auto, got: " + S(lowDeband)).c_str());

			// Compare against Classic (0 -> off) and Mid (2 -> on): all 3 distinct.
			repairSlider->setValue(0);
			QCoreApplication::processEvents();
			require(repairSlider->value() == 0 && repairName->text().contains(QStringLiteral("Classic")),
				"Unit 2: Repair Classic (0) did not stick");
			const QString classicDeband = effectiveDeband();
			require(classicDeband.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0,
				("Unit 2: Repair Classic must set deband_vp=off, got: " + S(classicDeband)).c_str());

			repairSlider->setValue(2);
			QCoreApplication::processEvents();
			require(repairSlider->value() == 2 && repairName->text().contains(QStringLiteral("Mid")),
				"Unit 2: Repair Mid (2) did not stick");
			const QString midDeband = effectiveDeband();
			require(midDeband.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0,
				("Unit 2: Repair Mid must set deband_vp=on, got: " + S(midDeband)).c_str());
			require(classicDeband != lowDeband && lowDeband != midDeband && classicDeband != midDeband,
				"Unit 2: Repair Classic / Low / Mid deband_vp values must all be distinct");

			// Owner #3 Clarity Mandate across ALL tiered units + Overview.
			auto* motionSummary = control<QLabel>(window, Vpv::ObjectName("motion.tier.summary"));
			auto* repairSummary = control<QLabel>(window, Vpv::ObjectName("repair.tier.summary"));
			auto* scalingSummary = control<QLabel>(window, Vpv::ObjectName("scaling.tier.summary"));
			auto* outputSummary = control<QLabel>(window, Vpv::ObjectName("output.tier.summary"));
			auto* globalSummary = control<QLabel>(window, Vpv::ObjectName("global_tier.summary"));

			auto checkDistinctNonEmpty = [](const char* unitName, const QStringList& texts)
			{
				for (int i = 0; i < texts.size(); ++i)
				{
					require(!texts[i].trimmed().isEmpty(),
						(std::string("Unit 2 clarity summary empty for ") + unitName + " step " + std::to_string(i)).c_str());
					for (int j = i + 1; j < texts.size(); ++j)
						require(texts[i] != texts[j],
							(std::string("Unit 2 clarity summary did not change between steps ") +
								std::to_string(i) + " and " + std::to_string(j) + " on " + unitName +
								": " + S(texts[i])).c_str());
				}
			};
			auto hasCI = [](const QString& text, const char* needle)
			{
				return text.contains(QString::fromLatin1(needle), Qt::CaseInsensitive);
			};

			// Repair clarity summaries (Classic 0, Low 1, Mid 2).
			QStringList repairTexts;
			for (int step = 0; step <= 2; ++step)
			{
				repairSlider->setValue(step);
				QCoreApplication::processEvents();
				repairTexts << repairSummary->text();
			}
			checkDistinctNonEmpty("repair", repairTexts);
			require(hasCI(repairTexts[0], "deband") && hasCI(repairTexts[0], "off"),
				("Unit 2: Repair Classic summary must mention deband + off, got: " + S(repairTexts[0])).c_str());
			require(hasCI(repairTexts[1], "deband") && hasCI(repairTexts[1], "auto"),
				("Unit 2: Repair Low summary must mention deband + auto/automatic, got: " + S(repairTexts[1])).c_str());
			require(hasCI(repairTexts[2], "deband") && hasCI(repairTexts[2], "on"),
				("Unit 2: Repair Mid summary must mention deband + on, got: " + S(repairTexts[2])).c_str());

			// Motion clarity summaries (Classic 0, Low 1, Mid 2, Upper 3 — all 4 built).
			auto* motionSlider = control<QSlider>(window, Vpv::ObjectName("motion.tier"));
			QStringList motionTexts;
			for (int step = 0; step <= 3; ++step)
			{
				motionSlider->setValue(step);
				QCoreApplication::processEvents();
				motionTexts << motionSummary->text();
			}
			checkDistinctNonEmpty("motion", motionTexts);
			require((hasCI(motionTexts[0], "optical flow") || hasCI(motionTexts[0], "motion") || hasCI(motionTexts[0], "matcher")) &&
				hasCI(motionTexts[0], "off"),
				("Unit 2: Motion Classic summary must explicitly state what is off, got: " + S(motionTexts[0])).c_str());
			require((hasCI(motionTexts[2], "optical flow") || hasCI(motionTexts[2], "motion")) && hasCI(motionTexts[2], "on"),
				("Unit 2: Motion Mid summary must explicitly state what is on, got: " + S(motionTexts[2])).c_str());
			require(hasCI(motionTexts[3], "on"),
				("Unit 2: Motion Upper summary must explicitly state what is on, got: " + S(motionTexts[3])).c_str());

			// Scaling clarity summaries (Classic 0, Low 1, Mid 2).
			auto* scalingSlider = control<QSlider>(window, Vpv::ObjectName("scaling.tier"));
			QStringList scalingTexts;
			for (int step = 0; step <= 2; ++step)
			{
				scalingSlider->setValue(step);
				QCoreApplication::processEvents();
				scalingTexts << scalingSummary->text();
			}
			checkDistinctNonEmpty("scaling", scalingTexts);
			require((hasCI(scalingTexts[0], "re-base") || hasCI(scalingTexts[0], "rebase") || hasCI(scalingTexts[0], "downscale")) &&
				hasCI(scalingTexts[0], "off"),
				("Unit 2: Scaling Classic summary must explicitly state what is off, got: " + S(scalingTexts[0])).c_str());
			require((hasCI(scalingTexts[2], "re-base") || hasCI(scalingTexts[2], "rebase") || hasCI(scalingTexts[2], "downscale")) &&
				hasCI(scalingTexts[2], "on"),
				("Unit 2: Scaling Mid summary must explicitly state what is on, got: " + S(scalingTexts[2])).c_str());

			// Output clarity summaries (Classic 0, Low 1, Mid 2).
			auto* outputSlider = control<QSlider>(window, Vpv::ObjectName("output.tier"));
			QStringList outputTexts;
			for (int step = 0; step <= 2; ++step)
			{
				outputSlider->setValue(step);
				QCoreApplication::processEvents();
				outputTexts << outputSummary->text();
			}
			checkDistinctNonEmpty("output", outputTexts);
			for (int step = 0; step <= 2; ++step)
				require(hasCI(outputTexts[step], "dither"),
					("Unit 2: Output tier summary must name dither, got: " + S(outputTexts[step])).c_str());
			require(hasCI(outputTexts[0], "libplacebo") || hasCI(outputTexts[0], "off"),
				("Unit 2: Output Classic summary must state libplacebo/off, got: " + S(outputTexts[0])).c_str());
			require(hasCI(outputTexts[2], "vp") || hasCI(outputTexts[2], "sigma") || hasCI(outputTexts[2], "on"),
				("Unit 2: Output Mid summary must state VP/sigma-delta/on, got: " + S(outputTexts[2])).c_str());

			// Overview global tier summary (Classic -> Low -> Mid -> Upper via Unit 3 preset buttons).
			QStringList globalTexts;
			for (const char* step : { "classic", "low", "mid", "upper" })
			{
				control<QPushButton>(window, Vpv::ObjectName(QStringLiteral("global_tier.btn.") + QString::fromLatin1(step)))->click();
				QCoreApplication::processEvents();
				globalTexts << globalSummary->text();
			}
			checkDistinctNonEmpty("global_tier", globalTexts);

			// Save at Repair Low (1) and prove round-trip reload sticks at 1 without bounce.
			repairSlider->setValue(1);
			QCoreApplication::processEvents();
			save(window);
		}
		requireVpReads(path);
		{
			ConfigEditorWindow reopened(path, 0, true);
			QCoreApplication::processEvents();
			require(control<QSlider>(reopened, Vpv::ObjectName("repair.tier"))->value() == 1 &&
				control<QLabel>(reopened, Vpv::ObjectName("repair.tier.name"))->text().contains(QStringLiteral("Low")),
				"Unit 2 reload: saved Repair Low (deband_vp=auto) reopens cleanly at step 1 without bouncing");
		}
	}

	// Unit 2 (Phase 1) Red-Team adversarial test 3: all 5 stranded Fixes-under-test keys placed (allRaw hidden),
	// Scaling/Motion Advanced demotions, Motion engine lock on render_ahead + intent_motion, Output moves for
	// dither_need + calibration_lut_stage, Dynamic Drive stub tab removal + black_protection on LedDrivePage > Advanced,
	// and input.delay wording pointing to Input > Main instead of Queue page.
	void testPhase1PlacementsAndAllRawHidden()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[general]\nrenderer: VP Renderer\n"
			"[vprenderer]\nsdr_target_nits: 100\n"
			"[queue]\ntarget_latency_ms: 250\n"
			"[led_drive.Default]\nenabled: true\ncommand_source: compensate\n"
			"[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
		{
			ConfigEditorWindow window(path, 0, true);
			QCoreApplication::processEvents();

			// 1. rendererSection.allRawGroup is hidden and ZERO of the 5 stranded keys remain in allRawGroup.
			auto* allRawGroup = control<QWidget>(window, QStringLiteral("rendererSection.allRawGroup"));
			require(allRawGroup->isHidden(),
				"Unit 2: rendererSection.allRawGroup must be hidden once all stranded keys are placed");
			auto* motionAdv = control<QWidget>(window, Vpv::ObjectName("motion.advanced"));
			auto* motionMain = control<QWidget>(window, Vpv::ObjectName("motion.main"));
			auto* motionShapingBody = control<QWidget>(window, Vpv::ObjectName("motion.card.shaping.body"));
			for (const char* key : { "optical_flow_global", "optical_flow_global_fallback", "optical_flow_fb_check", "source_class" })
			{
				auto* w = control<QWidget>(window, QStringLiteral("config.vprenderer.") + QString::fromLatin1(key));
				require(!allRawGroup->isAncestorOf(w),
					(std::string("Unit 2: stranded key still in allRawGroup: ") + key).c_str());
				require(motionAdv->isAncestorOf(w) && motionShapingBody->isAncestorOf(w),
					(std::string("Unit 2: key must live inside motion.advanced / motion.card.shaping.body: ") + key).c_str());
			}
			auto* scalingAdv = control<QWidget>(window, Vpv::ObjectName("scaling.advanced"));
			auto* scalingMain = control<QWidget>(window, Vpv::ObjectName("scaling.main"));
			auto* scalingSteadinessBody = control<QWidget>(window, Vpv::ObjectName("scaling.card.steadiness.body"));
			auto* ssimFloor = control<QWidget>(window, QStringLiteral("config.vprenderer.ssim_bitrate_floor"));
			require(!allRawGroup->isAncestorOf(ssimFloor) && scalingAdv->isAncestorOf(ssimFloor) && scalingSteadinessBody->isAncestorOf(ssimFloor),
				"Unit 2: ssim_bitrate_floor must live inside scaling.advanced / scaling.card.steadiness.body, not allRawGroup");

			// 2. source_geometry_detect (mode combo + conditional custom QLineEdit) and rebase_kernel_match live on Scaling > Advanced.
			auto* sgdMode = control<QComboBox>(window, Vpv::ObjectName("scaling.source_geometry_detect.mode"));
			auto* sgdEdit = control<QLineEdit>(window, Vpv::ObjectName("scaling.source_geometry_detect"));
			auto* kernelMatch = control<QWidget>(window, Vpv::ObjectName("scaling.rebase_kernel_match"));
			require(scalingAdv->isAncestorOf(sgdMode) && scalingAdv->isAncestorOf(sgdEdit) && scalingAdv->isAncestorOf(kernelMatch),
				"Unit 2: source_geometry_detect and rebase_kernel_match must live inside vpv.scaling.advanced");
			require(!scalingMain->isAncestorOf(sgdMode) && !scalingMain->isAncestorOf(sgdEdit) && !scalingMain->isAncestorOf(kernelMatch),
				"Unit 2: source_geometry_detect and rebase_kernel_match must NOT live inside vpv.scaling.main");

			const int allIdx = sgdMode->findData(QStringLiteral("all"));
			const int unsetIdx = sgdMode->findData(QString());
			const int convIdx = sgdMode->findData(QStringLiteral("conventional"));
			const int customIdx = sgdMode->findData(QStringLiteral("custom"));
			require((allIdx >= 0 || unsetIdx >= 0) && convIdx >= 0 && customIdx >= 0,
				"Unit 2: source_geometry_detect.mode must offer all/default, conventional, and custom");
			if (allIdx >= 0)
			{
				sgdMode->setCurrentIndex(allIdx);
				QCoreApplication::processEvents();
				require(sgdEdit->isHidden(), "Unit 2: source_geometry_detect mode 'all' must hide custom QLineEdit");
			}
			if (unsetIdx >= 0)
			{
				sgdMode->setCurrentIndex(unsetIdx);
				QCoreApplication::processEvents();
				require(sgdEdit->isHidden(), "Unit 2: source_geometry_detect default mode must hide custom QLineEdit");
			}
			sgdMode->setCurrentIndex(convIdx);
			QCoreApplication::processEvents();
			require(sgdEdit->isHidden(), "Unit 2: source_geometry_detect mode 'conventional' must hide custom QLineEdit");
			sgdMode->setCurrentIndex(customIdx);
			QCoreApplication::processEvents();
			require(!sgdEdit->isHidden() && sgdEdit->isEnabled(),
				"Unit 2: source_geometry_detect mode 'custom' must show and enable custom QLineEdit");
			sgdEdit->setText(QStringLiteral("1920x1080, 1280x720"));
			emit sgdEdit->editingFinished();
			QCoreApplication::processEvents();

			// 3. dejudder_estimate_ahead lives on Motion > Advanced (NOT Motion > Main).
			auto* estAhead = control<QWidget>(window, Vpv::ObjectName("motion.dejudder_estimate_ahead"));
			require(motionAdv->isAncestorOf(estAhead) && !motionMain->isAncestorOf(estAhead),
				"Unit 2: dejudder_estimate_ahead must live inside vpv.motion.advanced, not vpv.motion.main");

			// 4. render_ahead and intent_motion lock to vector_demand (negative control: enabled when on, disabled when off, re-enabled when on).
			auto* vectorDemand = control<QComboBox>(window, Vpv::ObjectName("motion.vector_demand"));
			auto* renderAhead = control<QWidget>(window, Vpv::ObjectName("motion.render_ahead"));
			auto* intentMotion = control<QWidget>(window, Vpv::ObjectName("motion.intent_motion"));
			require(renderAhead->isEnabled() && intentMotion->isEnabled(),
				"Unit 2 negative control (vector_demand=on): render_ahead and intent_motion must start enabled");
			vectorDemand->setCurrentIndex(vectorDemand->findData(QStringLiteral("off")));
			QCoreApplication::processEvents();
			require(!renderAhead->isEnabled() && !intentMotion->isEnabled(),
				"Unit 2 (vector_demand=off): render_ahead and intent_motion must disable when motion engine is off");
			vectorDemand->setCurrentIndex(vectorDemand->findData(QStringLiteral("on")));
			QCoreApplication::processEvents();
			require(renderAhead->isEnabled() && intentMotion->isEnabled(),
				"Unit 2 (vector_demand=on again): render_ahead and intent_motion must re-enable");

			// 5. dither_need lives inside Output > Main (output.card.dither_need) and calibration_lut_stage inside Output > Advanced (output.card.calibration_lut_stage), NOT vpv.expert.
			auto* outputMain = control<QWidget>(window, Vpv::ObjectName("output.main"));
			auto* outputAdv = control<QWidget>(window, Vpv::ObjectName("output.advanced"));
			auto* expertTab = control<QWidget>(window, Vpv::ObjectName("expert"));
			auto* ditherNeedCard = control<QWidget>(window, Vpv::ObjectName("output.card.dither_need"));
			auto* ditherNeed = control<QComboBox>(window, Vpv::ObjectName("expert.dither_need"));
			auto* lutStageCard = control<QWidget>(window, Vpv::ObjectName("output.card.calibration_lut_stage"));
			auto* lutStage = control<QComboBox>(window, Vpv::ObjectName("expert.calibration_lut_stage"));
			require(outputMain->isAncestorOf(ditherNeedCard) && ditherNeedCard->isAncestorOf(ditherNeed) && !expertTab->isAncestorOf(ditherNeed),
				"Unit 2: vpv.expert.dither_need must live inside vpv.output.main (vpv.output.card.dither_need), not vpv.expert");
			require(outputAdv->isAncestorOf(lutStageCard) && lutStageCard->isAncestorOf(lutStage) && !expertTab->isAncestorOf(lutStage),
				"Unit 2: vpv.expert.calibration_lut_stage must live inside vpv.output.advanced (vpv.output.card.calibration_lut_stage), not vpv.expert");

			// 6. black_protection (holding intent_black) lives inside vpv.drivepage.advanced, and redundant Dynamic Drive stub tab is removed from vpv.units.
			auto* drivePageAdv = control<QWidget>(window, Vpv::ObjectName("drivepage.advanced"));
			auto* blackProtBody = control<QWidget>(window, Vpv::ObjectName("drive.card.black_protection.body"));
			auto* intentBlack = control<QWidget>(window, QStringLiteral("config.vprenderer.intent_black"));
			require(drivePageAdv->isAncestorOf(blackProtBody) && blackProtBody->isAncestorOf(intentBlack),
				"Unit 2: vpv.drive.card.black_protection.body (with intent_black) must live inside vpv.drivepage.advanced");
			require(window.findChild<QWidget*>(Vpv::ObjectName("drive")) == nullptr,
				"Unit 2: redundant vpv.drive stub tab must not exist");
			auto* units = control<QTabWidget>(window, Vpv::ObjectName("units"));
			for (int i = 0; i < units->count(); ++i)
				require(units->tabText(i) != QStringLiteral("Dynamic Drive"),
					"Unit 2: vpv.units must not contain a redundant 'Dynamic Drive' stub tab");

			// 7. vpv.input.delay wording points to Input > Main (not stale Queue page wording).
			const QString delayWithTarget = control<QLabel>(window, Vpv::ObjectName("input.delay"))->text();
			require(!delayWithTarget.contains(QStringLiteral("Not set on the Queue page")) &&
				!delayWithTarget.contains(QStringLiteral("Queue page Target delay")),
				("Unit 2: vpv.input.delay retains stale Queue page wording: " + S(delayWithTarget)).c_str());
			require(delayWithTarget.contains(QStringLiteral("Input")) && delayWithTarget.contains(QStringLiteral("250 ms")),
				("Unit 2: vpv.input.delay must point to Input > Main and show 250 ms: " + S(delayWithTarget)).c_str());

			save(window);
		}

		// Verify custom source_geometry_detect saved cleanly and reloads.
		ConfigFile config;
		std::string v;
		require(config.Load(path.toStdString()) && config.TryGetString(DefaultProfile(path), "source_geometry_detect", v) &&
			v == "1920x1080, 1280x720",
			("Unit 2: custom source_geometry_detect IDs did not save cleanly, got: " + v).c_str());
		requireVpReads(path);

		// Secondary fixture (no target_latency_ms): verify unset delay wording also avoids "Not set on the Queue page".
		{
			QTemporaryDir dir2;
			const QString noTargetPath = WriteFixture(dir2,
				"[general]\nrenderer: VP Renderer\n"
				"[vprenderer]\nsdr_target_nits: 100\n"
				"[queue]\ntarget_frames: 4\n");
			ConfigEditorWindow window(noTargetPath, 0, true);
			QCoreApplication::processEvents();
			const QString delayUnset = control<QLabel>(window, Vpv::ObjectName("input.delay"))->text();
			require(!delayUnset.contains(QStringLiteral("Not set on the Queue page")) &&
				!delayUnset.contains(QStringLiteral("Queue page Target delay")),
				("Unit 2: unset vpv.input.delay retains stale Queue page wording: " + S(delayUnset)).c_str());
			require(delayUnset.contains(QStringLiteral("Input")),
				("Unit 2: unset vpv.input.delay must point to Input > Main: " + S(delayUnset)).c_str());
		}
	}

	// Unit 3 (Phase 2) Red-Team adversarial test 1: Developer tab is hidden by default behind vpv.developer.toggle,
	// holds expert/retirement/camera_debug(18)/internals/diagnostics/draft/capture_fixes/planned/stages, keeps everyday
	// tabs clean (negative controls), falls back to Overview when hidden while active, and camera_model still enables/disables
	// all 18 camera debug controls across the Motion -> Developer boundary.
	void testDeveloperTabHiddenByDefaultAndToggleable()
	{
		Vpv::RegisterConfigProviders();
		static const char* const kCameraBoolDebugKeys[] = {
			"camera_labels_gpu", "camera_meas_model_global", "camera_meas_guided_candidate",
			"camera_meas_narrow_window", "camera_meas_e1_skip", "camera_meas_static_gate", "camera_meas_cut_skip",
			"camera_meas_fade_normalised", "camera_meas_flash_bridge", "camera_meas_temporal_seed", "camera_meas_tiny_mover_roi",
			"camera_use_grain", "camera_use_detail", "camera_use_dither", "camera_use_deband", "camera_use_filler", "camera_use_dejudder"
		};
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[general]\nrenderer: VP Renderer\n"
			"[vprenderer]\nsdr_target_nits: 100\n"
			"[queue]\ntarget_latency_ms: 250\n");
		ConfigEditorWindow window(path, 0, true);
		QCoreApplication::processEvents();

		auto* units = control<QTabWidget>(window, Vpv::ObjectName("units"));
		auto* devToggle = control<QCheckBox>(window, Vpv::ObjectName("developer.toggle"));
		require(!devToggle->isChecked(), "Unit 3: vpv.developer.toggle must start unchecked by default");

		auto* devTab = control<QWidget>(window, Vpv::ObjectName("developer"));
		int devIdx = -1;
		for (int i = 0; i < units->count(); ++i)
			if (units->widget(i) == devTab) { devIdx = i; break; }
		require(devIdx >= 0, "Unit 3: vpv.developer must exist as a tab on vpv.units");
		require(!units->isTabVisible(devIdx), "Unit 3: vpv.developer tab must be hidden by default");

		// Positive placement assertions: all developer/diagnostic/internal/planned/stages cards live under vpv.developer.
		auto* expert = control<QWidget>(window, Vpv::ObjectName("expert"));
		auto* retirement = control<QWidget>(window, Vpv::ObjectName("retirement"));
		auto* cameraDebugCard = control<QWidget>(window, Vpv::ObjectName("developer.card.camera_debug"));
		auto* motionInternalsCard = control<QWidget>(window, Vpv::ObjectName("developer.card.motion_internals"));
		auto* scalingInternalsCard = control<QWidget>(window, Vpv::ObjectName("developer.card.scaling_internals"));
		auto* inputDiagCard = control<QWidget>(window, Vpv::ObjectName("developer.card.input_diagnostics"));
		auto* outputDraftCard = control<QWidget>(window, Vpv::ObjectName("developer.card.output_draft"));
		auto* outputDraftBody = control<QWidget>(window, Vpv::ObjectName("developer.card.output_draft.body"));
		auto* captureFixesCard = control<QWidget>(window, Vpv::ObjectName("input.card.capture_fixes"));
		auto* repairPlannedCard = control<QWidget>(window, Vpv::ObjectName("repair.card.planned"));
		auto* repairStagesCard = control<QWidget>(window, Vpv::ObjectName("repair.card.stages"));

		require(devTab->isAncestorOf(expert) && devTab->isAncestorOf(retirement),
			"Unit 3: vpv.developer must be an ancestor of vpv.expert and vpv.retirement");
		require(devTab->isAncestorOf(cameraDebugCard),
			"Unit 3: vpv.developer must be an ancestor of vpv.developer.card.camera_debug");
		require(devTab->isAncestorOf(motionInternalsCard) &&
			motionInternalsCard->isAncestorOf(control<QWidget>(window, QStringLiteral("rendererSection.dejudderEstimation"))),
			"Unit 3: vpv.developer.card.motion_internals must hold rendererSection.dejudderEstimation");
		require(devTab->isAncestorOf(scalingInternalsCard) &&
			scalingInternalsCard->isAncestorOf(control<QWidget>(window, QStringLiteral("rendererSection.adaptiveDownscale"))) &&
			scalingInternalsCard->isAncestorOf(control<QWidget>(window, QStringLiteral("rendererSection.adaptiveUpscale"))),
			"Unit 3: vpv.developer.card.scaling_internals must hold adaptiveDownscale and adaptiveUpscale");
		require(devTab->isAncestorOf(inputDiagCard) &&
			inputDiagCard->isAncestorOf(control<QWidget>(window, QStringLiteral("config.vprenderer.intent_display"))),
			"Unit 3: vpv.developer.card.input_diagnostics must hold config.vprenderer.intent_display");
		auto* repeatReuse = control<QComboBox>(window, QStringLiteral("config.vprenderer.repeat_render_reuse"));
		require(devTab->isAncestorOf(outputDraftCard) && outputDraftBody->isAncestorOf(repeatReuse) &&
			!outputDraftBody->isEnabled() && !repeatReuse->isEnabled() &&
			outputDraftBody->toolTip().contains(QStringLiteral("Draft, no effect yet")),
			"Unit 3: vpv.developer.card.output_draft must hold repeat_render_reuse disabled with 'Draft, no effect yet'");
		require(devTab->isAncestorOf(captureFixesCard) &&
			captureFixesCard->isAncestorOf(control<QWidget>(window, QStringLiteral("config.general.capture_display_fixes"))),
			"Unit 3: vpv.input.card.capture_fixes (capture_display_fixes) must live inside vpv.developer");
		require(devTab->isAncestorOf(repairPlannedCard) && devTab->isAncestorOf(repairStagesCard),
			"Unit 3: per-unit planned and stages cards (vpv.repair.card.planned/stages) must live inside vpv.developer");

		// All 18 [vpv.camera] debug controls live inside vpv.developer.card.camera_debug (and NOT under vpv.motion).
		auto* motionTab = control<QWidget>(window, Vpv::ObjectName("motion"));
		auto* motionMain = control<QWidget>(window, Vpv::ObjectName("motion.main"));
		auto* motionCameraCard = control<QWidget>(window, Vpv::ObjectName("motion.card.camera"));
		auto* cameraModel = control<QCheckBox>(window, Vpv::ObjectName("camera.camera_model"));
		auto* cameraOverlay = control<QComboBox>(window, Vpv::ObjectName("camera.camera_model_overlay"));
		require(cameraDebugCard->isAncestorOf(cameraOverlay) && !motionTab->isAncestorOf(cameraOverlay),
			"Unit 3: vpv.camera.camera_model_overlay must live inside vpv.developer.card.camera_debug, not vpv.motion");
		for (const char* key : kCameraBoolDebugKeys)
		{
			auto* box = control<QCheckBox>(window, Vpv::ObjectName(QStringLiteral("camera.") + QString::fromLatin1(key)));
			require(cameraDebugCard->isAncestorOf(box),
				(std::string("Unit 3: camera debug switch must live inside vpv.developer.card.camera_debug: ") + key).c_str());
			require(!motionTab->isAncestorOf(box),
				(std::string("Unit 3 negative control: camera debug switch must NOT live inside vpv.motion: ") + key).c_str());
		}

		// Negative controls (everyday cleanliness): everyday tabs stay uncluttered.
		auto* overview = control<QWidget>(window, Vpv::ObjectName("overview"));
		auto* inputTab = control<QWidget>(window, Vpv::ObjectName("input"));
		auto* inputMain = control<QWidget>(window, Vpv::ObjectName("input.main"));
		auto* repairTab = control<QWidget>(window, Vpv::ObjectName("repair"));
		require(!overview->isAncestorOf(retirement) && !overview->isAncestorOf(expert),
			"Unit 3 negative control: vpv.overview must NOT be an ancestor of vpv.retirement or vpv.expert");
		require(!inputMain->isAncestorOf(captureFixesCard) && !inputTab->isAncestorOf(captureFixesCard),
			"Unit 3 negative control: vpv.input.main / vpv.input must NOT be an ancestor of vpv.input.card.capture_fixes");
		require(motionMain->isAncestorOf(motionCameraCard) && motionCameraCard->isAncestorOf(cameraModel),
			"Unit 3: vpv.motion.main must be an ancestor of vpv.motion.card.camera holding vpv.camera.camera_model");
		require(!repairTab->isAncestorOf(repairPlannedCard) && !repairTab->isAncestorOf(repairStagesCard),
			"Unit 3 negative control: vpv.repair must NOT be an ancestor of vpv.repair.card.planned or vpv.repair.card.stages");

		// Toggling vpv.developer.toggle shows/hides Developer tab and falls back cleanly to Overview (index 0).
		devToggle->setChecked(true);
		QCoreApplication::processEvents();
		require(units->isTabVisible(devIdx), "Unit 3: checking vpv.developer.toggle must show the Developer tab");
		units->setCurrentIndex(devIdx);
		QCoreApplication::processEvents();
		require(units->currentIndex() == devIdx, "Unit 3: Developer tab must be selectable when visible");
		devToggle->setChecked(false);
		QCoreApplication::processEvents();
		require(!units->isTabVisible(devIdx), "Unit 3: unchecking vpv.developer.toggle must hide the Developer tab");
		require(units->currentIndex() == 0, "Unit 3: hiding active Developer tab must fall back cleanly to Overview (index 0)");

		// Cross-tab master/derived lock: vpv.camera.camera_model on Motion > Main enables/disables all 18 debug controls in Developer.
		require(!cameraModel->isChecked() && !cameraOverlay->isEnabled(),
			"Unit 3 negative control: camera_model starts off and camera_model_overlay starts disabled");
		for (const char* key : kCameraBoolDebugKeys)
			require(!control<QCheckBox>(window, Vpv::ObjectName(QStringLiteral("camera.") + QString::fromLatin1(key)))->isEnabled(),
				(std::string("Unit 3: camera debug switch must start disabled when camera_model is off: ") + key).c_str());

		cameraModel->setChecked(true);
		QCoreApplication::processEvents();
		require(cameraOverlay->isEnabled(), "Unit 3: turning camera_model ON must enable camera_model_overlay in Developer");
		for (const char* key : kCameraBoolDebugKeys)
			require(control<QCheckBox>(window, Vpv::ObjectName(QStringLiteral("camera.") + QString::fromLatin1(key)))->isEnabled(),
				(std::string("Unit 3: turning camera_model ON must enable camera debug switch in Developer: ") + key).c_str());

		cameraModel->setChecked(false);
		QCoreApplication::processEvents();
		require(!cameraOverlay->isEnabled(), "Unit 3: turning camera_model OFF must disable camera_model_overlay in Developer");
		for (const char* key : kCameraBoolDebugKeys)
			require(!control<QCheckBox>(window, Vpv::ObjectName(QStringLiteral("camera.") + QString::fromLatin1(key)))->isEnabled(),
				(std::string("Unit 3: turning camera_model OFF must disable camera debug switch in Developer: ") + key).c_str());
	}

	// Unit 3 (Phase 2) Red-Team adversarial test 2: Clean Overview dashboard — vpv.overview.more removed, status card
	// holds pathway/chips/fallbacks (with 1000 ms QTimer direct child), 4 clickable global tier preset buttons + reset
	// work end-to-end with clarity summary updates, precision card holds fbo_float32 + note, launch card holds both
	// Dynamic Drive and Calibration buttons.
	void testCleanOverviewDashboardAndPresetButtons()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[general]\nrenderer: VP Renderer\n"
			"[vprenderer]\nsdr_target_nits: 100\n"
			"[queue]\ntarget_latency_ms: 250\n"
			"[led_drive.Default]\nenabled: true\ncommand_source: compensate\n"
			"[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
		ConfigEditorWindow window(path, 0, true);
		QCoreApplication::processEvents();

		auto* overview = control<QWidget>(window, Vpv::ObjectName("overview"));
		// 1. Nested vpv.overview.more sub-tabs are completely gone.
		require(window.findChild<QWidget*>(Vpv::ObjectName("overview.more")) == nullptr,
			"Unit 3: nested vpv.overview.more sub-tabs must no longer exist");

		// 2. Status card holds pathway status, global tier chips, and fallbacks (with its 1000 ms QTimer direct child).
		auto* statusCard = control<QWidget>(window, Vpv::ObjectName("overview.card.status"));
		auto* pathwayStatus = control<QLabel>(window, Vpv::ObjectName("overview.status.pathway"));
		auto* chips = control<QLabel>(window, Vpv::ObjectName("global_tier.chips"));
		auto* fallbacks = control<QLabel>(window, Vpv::ObjectName("fallbacks"));
		require(overview->isAncestorOf(statusCard) &&
			statusCard->isAncestorOf(pathwayStatus) && !pathwayStatus->text().trimmed().isEmpty() &&
			statusCard->isAncestorOf(chips) && !chips->text().trimmed().isEmpty() &&
			statusCard->isAncestorOf(fallbacks),
			"Unit 3: vpv.overview.card.status must live in Overview and hold pathway, chips, and fallbacks");
		bool has1000MsTimer = false;
		for (QTimer* t : fallbacks->findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly))
			if (t->interval() == 1000) has1000MsTimer = true;
		require(has1000MsTimer, "Unit 3: vpv.fallbacks must keep its 1000 ms QTimer as a direct child");

		// 3. 4 clickable preset buttons + reset button + visible non-empty summary.
		auto* btnClassic = control<QPushButton>(window, Vpv::ObjectName("global_tier.btn.classic"));
		auto* btnLow = control<QPushButton>(window, Vpv::ObjectName("global_tier.btn.low"));
		auto* btnMid = control<QPushButton>(window, Vpv::ObjectName("global_tier.btn.mid"));
		auto* btnUpper = control<QPushButton>(window, Vpv::ObjectName("global_tier.btn.upper"));
		auto* btnReset = control<QPushButton>(window, Vpv::ObjectName("global_tier.reset"));
		auto* globalName = control<QLabel>(window, Vpv::ObjectName("global_tier.name"));
		auto* globalSummary = control<QLabel>(window, Vpv::ObjectName("global_tier.summary"));
		for (QPushButton* b : { btnClassic, btnLow, btnMid, btnUpper })
			require(overview->isAncestorOf(b) && b->isCheckable(),
				("Unit 3: global tier preset button must be checkable and on Overview: " + S(b->objectName())).c_str());
		require(overview->isAncestorOf(btnReset), "Unit 3: vpv.global_tier.reset must live on Overview");
		require(!globalSummary->isHidden() && !globalSummary->text().trimmed().isEmpty(),
			"Unit 3: vpv.global_tier.summary must be visible and non-empty");

		// Negative control: shipped default is Mixed, so none of the 4 uniform preset buttons start checked.
		const QString initialSummary = globalSummary->text();
		require(!btnClassic->isChecked() && !btnLow->isChecked() && !btnMid->isChecked() && !btnUpper->isChecked(),
			"Unit 3 negative control: shipped Mixed default must leave all 4 uniform preset buttons unchecked");

		auto* motionTier = control<QSlider>(window, Vpv::ObjectName("motion.tier"));
		auto* repairTier = control<QSlider>(window, Vpv::ObjectName("repair.tier"));
		auto* scalingTier = control<QSlider>(window, Vpv::ObjectName("scaling.tier"));
		auto* outputTier = control<QSlider>(window, Vpv::ObjectName("output.tier"));
		auto* debandVp = control<QComboBox>(window, QStringLiteral("config.vprenderer.deband_vp"));
		auto effectiveValue = [](QComboBox* combo) -> QString
		{
			const QString d = combo->currentData().toString().trimmed();
			return d.isEmpty() ? combo->property("effectiveValue").toString().trimmed() : d;
		};

		// Click Upper preset button.
		btnUpper->click();
		QCoreApplication::processEvents();
		require(globalName->text() == QStringLiteral("Upper"),
			("Unit 3: clicking Upper must set vpv.global_tier.name to Upper, got: " + S(globalName->text())).c_str());
		require(motionTier->value() == 3, "Unit 3: Upper preset must set Motion tier to 3");
		require(repairTier->value() == 2 || repairTier->value() == 3,
			"Unit 3: Upper preset must set Repair tier to Mid (2) or Upper (3)");
		require(scalingTier->value() == 2 && outputTier->value() == 2,
			"Unit 3: Upper preset must set Scaling and Output to their nearest built tier (Mid = 2)");
		const QString upperSummary = globalSummary->text();
		require(!upperSummary.trimmed().isEmpty() && upperSummary != initialSummary,
			"Unit 3: clicking Upper must update vpv.global_tier.summary");
		require(btnUpper->isChecked() && !btnClassic->isChecked() && !btnLow->isChecked() && !btnMid->isChecked(),
			"Unit 3: clicking Upper must check btn.upper exclusively");

		// Click Low preset button.
		btnLow->click();
		QCoreApplication::processEvents();
		require(motionTier->value() == 1 && repairTier->value() == 1 && scalingTier->value() == 1 && outputTier->value() == 1,
			"Unit 3: clicking Low must move all 4 tiered units to Low (1)");
		require(effectiveValue(debandVp).compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0,
			("Unit 3: Low preset must set Repair deband_vp=auto, got: " + S(effectiveValue(debandVp))).c_str());
		require(btnLow->isChecked() && !btnUpper->isChecked() && !btnClassic->isChecked() && !btnMid->isChecked(),
			"Unit 3: clicking Low must check btn.low exclusively");
		require(!globalSummary->text().trimmed().isEmpty() && globalSummary->text() != upperSummary,
			"Unit 3: clicking Low must update vpv.global_tier.summary");

		// Click Reset button -> shipped default mix returns (Motion: Mid, Repair: Classic, Scaling: Low, Output: Low).
		btnReset->click();
		QCoreApplication::processEvents();
		require(motionTier->value() == 2 && repairTier->value() == 0 && scalingTier->value() == 1 && outputTier->value() == 1,
			"Unit 3: clicking vpv.global_tier.reset must restore shipped default mix (Motion: Mid, Repair: Classic, Scaling: Low, Output: Low)");
		require(!btnClassic->isChecked() && !btnLow->isChecked() && !btnMid->isChecked() && !btnUpper->isChecked(),
			"Unit 3: after reset to shipped Mixed default, none of the 4 uniform preset buttons stay checked");

		// 4. Precision card and Launch card.
		auto* precisionCard = control<QWidget>(window, Vpv::ObjectName("overview.card.precision"));
		require(overview->isAncestorOf(precisionCard) &&
			precisionCard->isAncestorOf(control<QWidget>(window, Vpv::ObjectName("colour.fbo_float32"))) &&
			precisionCard->isAncestorOf(control<QLabel>(window, Vpv::ObjectName("full_precision.note"))),
			"Unit 3: vpv.overview.card.precision must hold vpv.colour.fbo_float32 and vpv.full_precision.note");

		auto* launchCard = control<QWidget>(window, Vpv::ObjectName("overview.card.launch"));
		auto* openDrive = control<QPushButton>(window, Vpv::ObjectName("drive.subpage.Dynamic Drive"));
		auto* openCal = control<QPushButton>(window, Vpv::ObjectName("output.subpage.Calibration"));
		require(overview->isAncestorOf(launchCard) && launchCard->isAncestorOf(openDrive) && launchCard->isAncestorOf(openCal),
			"Unit 3: vpv.overview.card.launch must hold BOTH vpv.drive.subpage.Dynamic Drive and vpv.output.subpage.Calibration");
	}

	// Unit 3 (Phase 2) Red-Team adversarial test 3: System tab (vpv.both) renamed "System", holds the exact 5
	// consolidated cards, stays unlocked & editable on Original pathway while VP-V unit tabs lock, and every page
	// (Overview, unit tabs, System, Developer when shown, Dynamic Drive, Calibration) fits 1040x700 with 0 px horizontal scroll.
	void testSystemTabFiveCardsAndOriginalUnlocked()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		const QString path = WriteFixture(dir,
			"[general]\nrenderer: VP Renderer\n"
			"[vprenderer]\nsdr_target_nits: 100\n"
			"[vprenderer.Cinema]\nquality: high\n"
			"[queue]\ntarget_latency_ms: 250\n"
			"[led_drive.Default]\nenabled: true\ncommand_source: compensate\n"
			"[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
		ConfigEditorWindow window(path, 0, true);
		QCoreApplication::processEvents();

		auto* units = control<QTabWidget>(window, Vpv::ObjectName("units"));
		auto* systemTab = control<QWidget>(window, Vpv::ObjectName("both"));
		int bothIdx = -1;
		for (int i = 0; i < units->count(); ++i)
			if (units->widget(i) == systemTab) { bothIdx = i; break; }
		require(bothIdx >= 0 && units->tabText(bothIdx) == QStringLiteral("System"),
			("Unit 3: vpv.both tab must be titled 'System', got: " + (bothIdx >= 0 ? S(units->tabText(bothIdx)) : std::string("<missing>"))).c_str());

		struct SystemCardSpec { const char* id; std::vector<const char*> controls; std::vector<const char*> notes; };
		const std::vector<SystemCardSpec> expectedCards = {
			{ "room_lights",
				{ "config.ambient.mode", "config.ambient.room", "config.ambient.lux_lit", "config.ambient.lux_dark",
				  "config.ambient.hold_s", "config.ambient.port", "config.ambient.baud" },
				{ "vpv.both.room_lights.note" } },
			{ "capture_bridges",
				{ "config.decklink.ycbcr_12bit_capture", "config.general.dv_tunnel_handoff",
				  "config.general.newlldv_sdr_veto", "config.general.bounded_invalid_capture_recovery",
				  "config.general.magewell_recover_faults" },
				{ "vpv.both.magewell_recovery.note" } },
			{ "file_playback",
				{ "config.general.file_av_offset_ms", "config.general.file_led_holdover_ms" },
				{} },
			{ "shortcuts_reports",
				{ "config.shortcuts.global_hotkeys", "config.shortcuts.global_actions",
				  "config.logging.auto_report", "config.logging.report_thumbnail", "config.logging.open_reports" },
				{} },
			{ "profile_rules",
				{ "vpv.both.queue.profile", "vpv.both.vprenderer.profile" },
				{} },
		};

		for (const SystemCardSpec& spec : expectedCards)
		{
			const QString cardName = QStringLiteral("vpv.system.card.") + QString::fromLatin1(spec.id);
			const QList<QWidget*> found = window.findChildren<QWidget*>(cardName);
			require(found.size() == 1 && systemTab->isAncestorOf(found.front()),
				(std::string("Unit 3: System card missing or outside vpv.both: ") + spec.id).c_str());
			QWidget* card = found.front();
			for (const char* u : { "overview", "input", "motion", "repair", "scaling", "colour", "output" })
				require(!control<QWidget>(window, Vpv::ObjectName(QString::fromLatin1(u)))->isAncestorOf(card),
					(std::string("Unit 3 negative control: System card leaked under ") + u + ": " + spec.id).c_str());
			for (const char* ctrl : spec.controls)
				require(card->isAncestorOf(control<QWidget>(window, QString::fromLatin1(ctrl))),
					(std::string("Unit 3: control not inside vpv.system.card.") + spec.id + ": " + ctrl).c_str());
			for (const char* note : spec.notes)
				require(card->isAncestorOf(control<QLabel>(window, QString::fromLatin1(note))),
					(std::string("Unit 3: note not inside vpv.system.card.") + spec.id + ": " + note).c_str());
		}

		// Negative control: old fragmented vpv.both.card.* wrappers must be consolidated away.
		for (const char* oldId : { "input", "reports", "keyboard", "bill", "room_lights", "file_playback",
			"magewell_recovery", "profile_use_queue", "profile_use_vprenderer" })
		{
			require(window.findChild<QWidget*>(QStringLiteral("vpv.both.card.") + QString::fromLatin1(oldId)) == nullptr,
				(std::string("Unit 3 negative control: stale fragmented card still exists: vpv.both.card.") + oldId).c_str());
		}

		// Switch to Original pathway (index 1): VP-V unit tabs disable, System stays enabled & clickable, and all controls in all 5 cards stay enabled.
		auto* pathway = control<QComboBox>(window, Vpv::ObjectName("pathway"));
		pathway->setCurrentIndex(1);
		QCoreApplication::processEvents();
		require(!control<QWidget>(window, Vpv::ObjectName("input"))->isEnabled() &&
			!control<QWidget>(window, Vpv::ObjectName("motion"))->isEnabled(),
			"Unit 3 negative control: Original pathway must disable VP-V unit tabs");
		require(systemTab->isEnabled() && units->isTabEnabled(bothIdx),
			"Unit 3: System tab must stay enabled on the Original pathway");
		units->setCurrentIndex(bothIdx);
		QCoreApplication::processEvents();
		require(units->currentIndex() == bothIdx, "Unit 3: System tab must stay clickable on the Original pathway");
		for (const SystemCardSpec& spec : expectedCards)
			for (const char* ctrl : spec.controls)
				require(control<QWidget>(window, QString::fromLatin1(ctrl))->isEnabled(),
					(std::string("Unit 3: System card control disabled on Original pathway: ") + ctrl).c_str());

		pathway->setCurrentIndex(0);
		QCoreApplication::processEvents();

		// Width/scroll fit at 1040x700 across Overview, every unit tab, System, Developer (when shown), Dynamic Drive, and Calibration.
		if (QFontDatabase::families().isEmpty())
			return;

		control<QCheckBox>(window, Vpv::ObjectName("developer.toggle"))->setChecked(true);
		QCoreApplication::processEvents();

		window.resize(1040, 700);
		window.show();
		QCoreApplication::processEvents();

		auto* pages = control<QStackedWidget>(window, QStringLiteral("settingsPages"));
		auto findScrollFor = [](QWidget* w) -> QScrollArea*
		{
			for (QWidget* p = w; p; p = p->parentWidget())
				if (auto* sa = qobject_cast<QScrollArea*>(p)) return sa;
			return nullptr;
		};

		// 1. All tabs on vpv.units (Overview, Input..Output, System, Developer).
		auto* section = control<QWidget>(window, Vpv::ObjectName("section"));
		for (int i = 0; i < pages->count(); ++i)
			if (pages->widget(i)->isAncestorOf(section)) window.selectPage(i);
		QScrollArea* vpvScroll = findScrollFor(section);
		for (int u = 0; u < units->count(); ++u)
		{
			units->setCurrentIndex(u);
			QTabWidget* sub = units->widget(u)->findChild<QTabWidget*>();
			for (int s2 = 0; s2 < (sub ? sub->count() : 1); ++s2)
			{
				if (sub) sub->setCurrentIndex(s2);
				QCoreApplication::processEvents();
				require(vpvScroll && vpvScroll->horizontalScrollBar()->maximum() == 0,
					(QStringLiteral("Unit 3 fit (1040x700): VP-V scrolls sideways on %1 / %2 (%3 px)")
						.arg(units->tabText(u), sub ? sub->tabText(s2) : QString())
						.arg(vpvScroll ? vpvScroll->horizontalScrollBar()->maximum() : -1)).toStdString().c_str());
			}
		}

		// 2. Dynamic Drive (Main & Advanced).
		auto* drivePages = control<QTabWidget>(window, Vpv::ObjectName("drivepage.pages"));
		for (int i = 0; i < pages->count(); ++i)
			if (pages->widget(i)->isAncestorOf(drivePages)) window.selectPage(i);
		QScrollArea* driveScroll = findScrollFor(drivePages);
		for (int t = 0; t < drivePages->count(); ++t)
		{
			drivePages->setCurrentIndex(t);
			QCoreApplication::processEvents();
			require(driveScroll && driveScroll->horizontalScrollBar()->maximum() == 0,
				(QStringLiteral("Unit 3 fit (1040x700): Dynamic Drive scrolls sideways on %1 (%2 px)")
					.arg(drivePages->tabText(t)).arg(driveScroll ? driveScroll->horizontalScrollBar()->maximum() : -1)).toStdString().c_str());
		}

		// 3. Calibration (Setup & Wizard, Advanced).
		auto* calPages = control<QTabWidget>(window, QStringLiteral("calibration.pages"));
		for (int i = 0; i < pages->count(); ++i)
			if (pages->widget(i)->isAncestorOf(calPages)) window.selectPage(i);
		QScrollArea* calScroll = findScrollFor(calPages);
		for (int t = 0; t < calPages->count(); ++t)
		{
			calPages->setCurrentIndex(t);
			QCoreApplication::processEvents();
			require(calScroll && calScroll->horizontalScrollBar()->maximum() == 0,
				(QStringLiteral("Unit 3 fit (1040x700): Calibration scrolls sideways on %1 (%2 px)")
					.arg(calPages->tabText(t)).arg(calScroll ? calScroll->horizontalScrollBar()->maximum() : -1)).toStdString().c_str());
		}

		window.hide();
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
	failures += run("VP-V: placement table is one table (Motion keys on Motion cards)", testPlacementTable);
	failures += run("VP-V: masters, derived settings, one control per key, fits the window", testMastersAndFit);
	failures += run("VP-V: logic audit fixes", testLogicAuditFixes);
	failures += run("VP-V: Set by wording and order", testSetByWording);
	failures += run("VP-V: every setting says where its value comes from (Set by)", testSetByOnThePage);
	failures += run("VP-V: source_geometry_detect shows Set by the Original pathway", testSourceGeometryDetectRowShowsSetByOriginal);
	failures += run("VP-V: Dynamic Drive main page contents and greyed reasons", testDynamicDriveMainPage);
	failures += run("VP-V: four tier steps, Upper shown as planned", testFourTierSteps);
	failures += run("VP-V: a drag from Custom onto the planned step writes nothing", testPlannedDragFromCustomWritesNothing);
	failures += run("VP-V: retirement Approve and Keep are recorded without hiding", testRetirementDecisions);
	failures += run("VP-V: Camera model group writes the keys VP reads", testCameraModelControlsReachVp);
	failures += run("VP-V: both-pathway cards leave Bill's pages", testBothPathwayCardsLeaveBillsPages);
	failures += run("VP-V: global_actions default follows the pathway", testGlobalActionsDefaultFollowsThePathway);
	failures += run("VP-V: General page VP rows on the VP-V page (GEM-060/222)", testGeneralPageRowsOnTheVpvPage);
	failures += run("VP-V: Bill's Chroma upscaler and VP-V's list", testChromaBillsControlAndVpvList);
	failures += run("VP-V: Bill's Dithering stays his, VP-V's list shows the value", testBillsDitheringStaysHis);
	failures += run("VP-V: Profile use card and the sync directions", testProfileUseAndSyncDirections);
	failures += run("VP-V: source_geometry_detect row on the Re-base card", testSourceGeometryDetectRowOnRebaseCard);
	failures += run("VP-V: queue rows on the VP-V card, look-ahead Auto kept", testQueueRowsOnTheVpvCard);
	failures += run("VP-V: By frames / By time is off on the Original pathway (GEM-032/049)", testQueueTargetModeOffInOriginal);
	failures += run("VP-V: Set by for chroma_upscaler on the Original pathway (GEM-031)", testSetByChromaOnOriginal);
	failures += run("VP-V: the moved load-time migrations still apply", testLoadMigrationsMoved);
	failures += run("VP-V: the moved refresh-key migration rewrites the NTSC band keys", testRefreshKeyMigrationMoved);
	failures += run("VP-V: motion engine off greys the de-judder amount and hiccup repair (batch B)", testMotionEngineOffGreysDejudderAndHiccup);
	failures += run("VP-V: profile defaults parity across Fields, SchemaKeys, RenderingDefaults and TierTable", testProfileDefaultsParity);
	failures += run("VP-V: config placement, links, planned tiers and feature locks (CB W4-2)", testConfigPlacementFixes);
	failures += run("VP-V: editor state fixes (queue grey, global_actions placeholder, managed grey, calibration note)", testEditorStateFixes);
	failures += run("VP-V: Both pathways tab unlocked and queue_target under Input (CB W4-5)", testBothPathwaysTabAndQueueTargetInInput);
	failures += run("VP-V: rule-32 profile controls round trip without Bill duplicates", testRule32ProfileControlsRoundTripWithoutBillDuplicates);
	failures += run("VP-V: legacy vprenderer pathway migrates to vpv and survives save", testLegacyVprendererPathwayMigratesToVpvAndSurvivesSave);
	failures += run("VP-V: hoisted styles stay single-sheet and isolated from Bill pages", testHoistedStylesStaySingleSheetAndIsolatedFromBillPages);
	failures += run("VP-V: section state frees on close and shell rebuild without dangling timers", testSectionStateFreesOnCloseAndShellRebuildWithoutDanglingTimers);
	failures += run("VP-V: D2 empty [vpv] key is not overwritten by legacy [vprenderer] value", testD2EmptyVpvKeyNotOverwrittenByLegacyVprenderer);
	failures += run("VP-V: D3 refresh timers survive transient detach and re-attach without firing while hidden", testD3RefreshTimersSurviveTransientDetachWithoutFiringWhileHidden);
	failures += run("VP-V: D4-D5 stylesheet cascade priority and zero per-widget VP-V styles", testD4D5StylesheetCascadePriorityAndZeroPerWidgetVpvStyles);
	failures += run("VP-V: Item 5 hidden VP-V combo popups coalesce events and sync on show", testItem5HiddenVpvComboPopupsCoalesceEventsAndSyncOnShow);
	failures += run("VP-V: S4-M1 adaptive_motion continuum control and motion row greying", testAdaptiveMotionContinuumAndGreying);
	failures += run("VP-V: S4-L1 pipeline_slots_off and pipeline_order case-insensitive exact tokens", testPipelineSlotsOffCaseInsensitiveAndExactTokens);
	failures += run("VP-V: S4-L2 appearance status case-insensitive evaluation", testAppearanceStatusCaseInsensitive);
	failures += run("VP-V: legacy queue and refresh-key migrations preserve UTF-8 and backup", testLegacyQueueMigrationPreservesUtf8);
	failures += run("VP-V: managed upstream controls stay disabled across profile switch without derivedSetting (GEM-040)", testManagedWidgetsStayDisabledAcrossProfileSwitch);
	failures += run("VP-V: Unit 2 CAM16 appearance validated widgets and ambient_adapt mutual exclusion", testCam16AppearanceWidgetsAndMutualExclusion);
	failures += run("VP-V: Unit 2 Repair Low no-bounce and all tiers clarity summaries", testRepairTierNoBounceAndAllTiersClaritySummary);
	failures += run("VP-V: Unit 2 Phase 1 placements, allRaw hidden, and Dynamic Drive stub removal", testPhase1PlacementsAndAllRawHidden);
	failures += run("VP-V: Unit 3 Developer tab hidden by default, toggleable, and camera debug separation", testDeveloperTabHiddenByDefaultAndToggleable);
	failures += run("VP-V: Unit 3 clean Overview dashboard, preset buttons, precision and launch cards", testCleanOverviewDashboardAndPresetButtons);
	failures += run("VP-V: Unit 3 System tab 5 consolidated cards, Original unlocked, and 1040x700 fit", testSystemTabFiveCardsAndOriginalUnlocked);
	return failures;
}
