// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// The Calibration settings page as a Config page provider (R1 W1 CAL-C): it is registered as a
// sub-page of the master VP-V page (grouped under Output, with a child entry under the VP-V entry in the list), it keeps
// the editor's object names and write path, it takes a status reply through the test seam found by
// ConfigPageHost::Find<CalibrationPage>(), and its per-editor state goes with the editor.
// Registered from ConfigEditorWindowTests.cpp's main with one line: RunCalibrationPageTests().
// Run with --match CalibrationPageTests.

#include "ConfigEditorWindow.h"
#include <VP-V/modules/control/config/config_pages/VpvConfigProviders.h>

#include <QAbstractButton>
#include <QCoreApplication>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>

#include <functional>

namespace
{
	std::function<void(bool, const char*)> g_require;
	std::function<void(ConfigEditorWindow&)> g_save;
	void require(bool condition, const char* message) { g_require(condition, message); }

	QString writeFixture(QTemporaryDir& dir)
	{
		const QString path = dir.filePath(QStringLiteral("VideoProcessor.cfg"));
		QFile file(path);
		require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write fixture");
		const QByteArray text = "[general]\nrenderer: VP Renderer\n[calibration]\nsettle_ms: 700\n";
		require(file.write(text) == text.size(), "Cannot write fixture");
		return path;
	}

	void testCalibrationIsASubPageOfVpv()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		ConfigEditorWindow window(writeFixture(dir), 0, true);
		auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
		require(pages != nullptr, "no page stack");
		const int calibration = window.pageIndex(QStringLiteral("Calibration"));
		const int vpv = window.pageIndex(QStringLiteral("VP-V"));
		require(calibration >= 0 && vpv == calibration + 1,
			"the Calibration page does not keep its place before the VP-V page");
		// Owner 2026-11-07: a left-hand entry under the VP-V group as well as the Output unit's button.
		QPushButton* leaf = nullptr;
		for (QPushButton* button : window.findChildren<QPushButton*>())
			if (button->property("nav").toBool() && button->property("pageName").toString() == QStringLiteral("Calibration")) leaf = button;
		require(leaf != nullptr && leaf->property("vpvChild").toBool() && leaf->property("vpvParent").toString() == QStringLiteral("VP-V"),
			"Calibration has no navigation child under the VP-V group");
		auto* open = window.findChild<QPushButton*>(QStringLiteral("vpv.output.subpage.Calibration"));
		require(open != nullptr, "the Output unit of the VP-V page has no button for the Calibration page");
		window.selectPage(QStringLiteral("General"));
		open->click();
		QCoreApplication::processEvents();
		require(pages->currentIndex() == calibration, "the Output unit's Calibration button opened another page");
		require(leaf->isChecked(), "opening the page from the button did not light its navigation child");
		window.selectPage(QStringLiteral("General"));
		leaf->click();
		QCoreApplication::processEvents();
		require(pages->currentIndex() == calibration, "the navigation child opened another page than the button");
	}

	void testCalibrationControlsKeepNamesAndWritePath()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		const QString path = writeFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* settle = window.findChild<QSpinBox*>(QStringLiteral("config.calibration.settle_ms"));
			require(settle != nullptr && settle->value() == 700, "the settle spin box is missing or shows another value");
			for (const char* name : { "calibration.detect", "calibration.probe", "calibration.plan_estimate", "calibration.wizard.step",
					"calibration.wizard.start", "calibration.wizard.kind", "calibration.wizard.lux", "config.calibration.meter_model",
					"config.calibration.argyll_path", "config.calibration.lut_size" })
				require(window.findChild<QWidget*>(QString::fromLatin1(name)) != nullptr,
					(std::string("Calibration control missing: ") + name).c_str());
			settle->setValue(900);
			g_save(window);
		}
		QFile saved(path);
		require(saved.open(QIODevice::ReadOnly) && QString::fromUtf8(saved.readAll()).contains(QStringLiteral("settle_ms: 900")),
			"an edit of the Calibration page was not saved");
	}

	void testStatusReplyReachesThePageThroughTheSeam()
	{
		Vpv::RegisterConfigProviders();
		auto* provider = ConfigPageHost::Find<Vpv::CalibrationPage>();
		require(provider != nullptr, "ConfigPageHost::Find<CalibrationPage>() found no provider");
		QTemporaryDir dir;
		ConfigEditorWindow window(writeFixture(dir), 0, true);
		auto* step = window.findChild<QLabel*>(QStringLiteral("calibration.wizard.step"));
		require(step != nullptr && step->text() == QStringLiteral("Ready"), "the wizard does not start at Ready");
		provider->ApplyStatusForTesting(step, QStringLiteral("step: measuring\nmessage: patch 3\n"), QString(), QStringLiteral("status"));
		require(step->text() == QStringLiteral("4. Measuring"), "a status reply did not reach the wizard step");
		auto* message = window.findChild<QLabel*>(QStringLiteral("calibration.wizard.message"));
		require(message != nullptr && message->text().contains(QStringLiteral("patch 3")), "the status message was not shown");
		provider->ApplyStatusForTesting(step, QString(), QStringLiteral("refused"), QStringLiteral("status"));
		require(message->text().contains(QStringLiteral("VideoProcessor did not answer: refused")), "an RPC error was not shown");
	}

	void testStateGoesWithTheEditor()
	{
		Vpv::RegisterConfigProviders();
		auto* provider = ConfigPageHost::Find<Vpv::CalibrationPage>();
		require(provider != nullptr, "no Calibration provider");
		const int before = provider->StateCountForTesting();
		QTemporaryDir dir;
		const QString path = writeFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			require(provider->StateCountForTesting() == before + 1, "the provider holds no state for an open editor");
		}
		require(provider->StateCountForTesting() == before, "the provider kept state after its editor closed");
	}

	// Unit 3 (Phase 2) Red-Team adversarial test: Calibration splits into Setup & Wizard (calibration.setup) and
	// Advanced (calibration.advanced) tabs inside calibration.pages; everyday setup + wizard stay on Setup & Wizard
	// without advanced/transcript clutter (negative control); Advanced holds settle_ms, led_flux_step, and a collapsed
	// calibration.card.transcripts card; both tabs fit 1040x700 with zero horizontal scroll.
	void testCalibrationSetupAndAdvancedTabsAndCollapsedTranscripts()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		ConfigEditorWindow window(writeFixture(dir), 0, true);
		QCoreApplication::processEvents();

		auto* calTabs = window.findChild<QTabWidget*>(QStringLiteral("calibration.pages"));
		require(calTabs != nullptr && calTabs->count() == 2,
			"Unit 3: calibration.pages QTabWidget must exist with exactly 2 tabs");
		require(calTabs->tabText(0) == QStringLiteral("Setup & Wizard") &&
			calTabs->widget(0) != nullptr && calTabs->widget(0)->objectName() == QStringLiteral("calibration.setup"),
			"Unit 3: calibration.pages tab 0 must be 'Setup & Wizard' (calibration.setup)");
		require(calTabs->tabText(1) == QStringLiteral("Advanced") &&
			calTabs->widget(1) != nullptr && calTabs->widget(1)->objectName() == QStringLiteral("calibration.advanced"),
			"Unit 3: calibration.pages tab 1 must be 'Advanced' (calibration.advanced)");

		QWidget* setupTab = calTabs->widget(0);
		QWidget* advancedTab = calTabs->widget(1);

		// Everyday setup & wizard live on calibration.setup.
		for (const char* name : {
			"config.calibration.argyll_path",
			"config.calibration.meter_model",
			"config.calibration.plan",
			"config.calibration.target_transfer",
			"config.calibration.lut_size",
			"calibration.wizard.start",
			"calibration.original_note" })
		{
			auto* w = window.findChild<QWidget*>(QString::fromLatin1(name));
			require(w != nullptr && setupTab->isAncestorOf(w),
				(std::string("Unit 3: everyday calibration control must live on calibration.setup: ") + name).c_str());
		}

		// Negative control: advanced timing, LED sweep, and transcript controls must NOT live on calibration.setup.
		for (const char* name : {
			"config.calibration.record_transcripts",
			"config.calibration.transcript_folder",
			"config.calibration.settle_ms",
			"config.calibration.led_flux_step" })
		{
			auto* w = window.findChild<QWidget*>(QString::fromLatin1(name));
			require(w != nullptr && !setupTab->isAncestorOf(w),
				(std::string("Unit 3 negative control: advanced control leaked onto calibration.setup: ") + name).c_str());
		}

		// Advanced tab holds settle_ms, led_flux_step, and collapsed calibration.card.transcripts.
		for (const char* name : { "config.calibration.settle_ms", "config.calibration.led_flux_step" })
		{
			auto* w = window.findChild<QWidget*>(QString::fromLatin1(name));
			require(w != nullptr && advancedTab->isAncestorOf(w),
				(std::string("Unit 3: advanced calibration control must live on calibration.advanced: ") + name).c_str());
		}

		auto* transcriptsCard = window.findChild<QWidget*>(QStringLiteral("calibration.card.transcripts"));
		auto* transcriptsToggle = window.findChild<QToolButton*>(QStringLiteral("calibration.card.transcripts.toggle"));
		auto* recordTranscripts = window.findChild<QWidget*>(QStringLiteral("config.calibration.record_transcripts"));
		auto* transcriptFolder = window.findChild<QWidget*>(QStringLiteral("config.calibration.transcript_folder"));
		require(transcriptsCard != nullptr && advancedTab->isAncestorOf(transcriptsCard),
			"Unit 3: calibration.card.transcripts must live inside calibration.advanced");
		require(recordTranscripts != nullptr && transcriptsCard->isAncestorOf(recordTranscripts) &&
			transcriptFolder != nullptr && transcriptsCard->isAncestorOf(transcriptFolder),
			"Unit 3: calibration.card.transcripts must hold record_transcripts and transcript_folder");
		require(transcriptsToggle != nullptr && advancedTab->isAncestorOf(transcriptsToggle) &&
			transcriptsToggle->isCheckable() && !transcriptsToggle->isChecked(),
			"Unit 3: calibration.card.transcripts.toggle must exist and start collapsed (!isChecked())");
		require(!recordTranscripts->isVisibleTo(transcriptsCard) && !transcriptFolder->isVisibleTo(transcriptsCard),
			"Unit 3 negative control: transcript controls must be hidden while calibration.card.transcripts is collapsed");

		transcriptsToggle->click();
		QCoreApplication::processEvents();
		require(transcriptsToggle->isChecked() &&
			recordTranscripts->isVisibleTo(transcriptsCard) && transcriptFolder->isVisibleTo(transcriptsCard),
			"Unit 3: clicking calibration.card.transcripts.toggle must reveal record_transcripts and transcript_folder");
		transcriptsToggle->click();
		QCoreApplication::processEvents();
		require(!transcriptsToggle->isChecked() && !recordTranscripts->isVisibleTo(transcriptsCard),
			"Unit 3 negative control: clicking calibration.card.transcripts.toggle again must re-collapse it");

		// 1040x700 horizontal-scroll fit on both Calibration tabs.
		if (!QFontDatabase::families().isEmpty())
		{
			window.resize(1040, 700);
			window.show();
			window.selectPage(QStringLiteral("Calibration"));
			QCoreApplication::processEvents();
			QScrollArea* scroll = nullptr;
			for (QWidget* p = calTabs; p && !scroll; p = p->parentWidget())
				scroll = qobject_cast<QScrollArea*>(p);
			require(scroll != nullptr, "Unit 3: Calibration page QScrollArea not found");
			for (int t = 0; t < calTabs->count(); ++t)
			{
				calTabs->setCurrentIndex(t);
				QCoreApplication::processEvents();
				require(scroll->horizontalScrollBar()->maximum() == 0,
					(QStringLiteral("Unit 3 fit (1040x700): Calibration tab '%1' scrolls sideways (%2 px)")
						.arg(calTabs->tabText(t)).arg(scroll->horizontalScrollBar()->maximum())).toStdString().c_str());
			}
			// Also verify Advanced tab still fits when transcripts card is expanded.
			calTabs->setCurrentIndex(1);
			transcriptsToggle->setChecked(true);
			QCoreApplication::processEvents();
			require(scroll->horizontalScrollBar()->maximum() == 0,
				(QStringLiteral("Unit 3 fit (1040x700): Calibration Advanced tab scrolls sideways with transcripts expanded (%1 px)")
					.arg(scroll->horizontalScrollBar()->maximum())).toStdString().c_str());
			window.hide();
		}
	}
}

int RunCalibrationPageTests(const std::function<int(const char*, const std::function<void()>&)>& run,
	const std::function<void(bool, const char*)>& requireHelper, const std::function<void(ConfigEditorWindow&)>& saveHelper)
{
	g_require = requireHelper;
	g_save = saveHelper;
	int failures = 0;
	failures += run("CalibrationPageTests: Calibration is a sub-page of the VP-V page", testCalibrationIsASubPageOfVpv);
	failures += run("CalibrationPageTests: controls keep names and write path", testCalibrationControlsKeepNamesAndWritePath);
	failures += run("CalibrationPageTests: a status reply reaches the page through the seam", testStatusReplyReachesThePageThroughTheSeam);
	failures += run("CalibrationPageTests: state goes with the editor", testStateGoesWithTheEditor);
	failures += run("CalibrationPageTests: Unit 3 Setup & Wizard and Advanced tabs with collapsed transcripts", testCalibrationSetupAndAdvancedTabsAndCollapsedTranscripts);
	return failures;
}
