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
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>

#include <functional>
#include <memory>
#include <string>

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

	// CUI viewport fit: CalibrationPage fits cleanly at the supported minimum window size (760x540) across
	// Setup & Wizard, Advanced, and Advanced with transcripts expanded, verified with an in-test oversized-widget
	// negative control.
	void testCalibrationMinimumViewport760x540Fit()
	{
		Vpv::RegisterConfigProviders();
		if (QFontDatabase::families().isEmpty())
			return;

		QTemporaryDir dir;
		ConfigEditorWindow window(writeFixture(dir), 0, true);
		struct WindowHideGuard { QWidget& w; ~WindowHideGuard() { w.hide(); } } hideGuard{ window };

		window.resize(760, 540);
		window.show();
		window.selectPage(QStringLiteral("Calibration"));
		for (int n = 0; n < 4; ++n) QCoreApplication::processEvents();
		require(window.width() == 760 && window.height() == 540,
			("CUI fit (760x540): Calibration window did not resize to 760x540, got " +
				std::to_string(window.width()) + "x" + std::to_string(window.height())).c_str());

		auto* calTabs = window.findChild<QTabWidget*>(QStringLiteral("calibration.pages"));
		require(calTabs != nullptr && calTabs->count() == 2,
			"CUI fit (760x540): calibration.pages QTabWidget missing");
		QScrollArea* scroll = nullptr;
		for (QWidget* p = calTabs; p && !scroll; p = p->parentWidget())
			scroll = qobject_cast<QScrollArea*>(p);
		require(scroll != nullptr && scroll->widget() != nullptr && scroll->viewport() != nullptr,
			"CUI fit (760x540): Calibration page QScrollArea/viewport missing");

		auto isInsideTabBar = [](const QWidget* w) -> bool
		{
			for (const QWidget* p = w; p; p = p->parentWidget())
				if (qobject_cast<const QTabBar*>(p)) return true;
			return false;
		};

		auto verifyTabFits = [&](const QString& label, QWidget* scope)
		{
			for (int n = 0; n < 3; ++n) QCoreApplication::processEvents();
			const int hMax = scroll->horizontalScrollBar()->maximum();
			const int contentW = scroll->widget()->width();
			const int viewW = scroll->viewport()->width();
			require(hMax == 0 && contentW <= viewW,
				(QStringLiteral("CUI fit (760x540): Calibration overflows horizontally on %1 (hMax=%2, content=%3, viewport=%4)")
					.arg(label).arg(hMax).arg(contentW).arg(viewW)).toStdString().c_str());
			for (QWidget* child : scope->findChildren<QWidget*>())
			{
				if (!child->isVisible() || isInsideTabBar(child)) continue;
				const bool isControl =
					qobject_cast<QAbstractButton*>(child) != nullptr ||
					qobject_cast<QComboBox*>(child) != nullptr ||
					qobject_cast<QLineEdit*>(child) != nullptr ||
					qobject_cast<QAbstractSpinBox*>(child) != nullptr ||
					qobject_cast<QSlider*>(child) != nullptr;
				if (!isControl) continue;
				const int rightEdge = child->mapTo(scroll->viewport(), QPoint(child->width(), 0)).x();
				require(child->width() > 0 && rightEdge <= viewW + 2,
					(QStringLiteral("CUI fit (760x540): Calibration control '%1' [%2] clipped on %3 (right=%4 > viewport=%5)")
						.arg(child->objectName(), QString::fromLatin1(child->metaObject()->className()), label)
						.arg(rightEdge).arg(viewW)).toStdString().c_str());
			}
		};

		for (int t = 0; t < calTabs->count(); ++t)
		{
			if (!calTabs->isTabVisible(t)) continue;
			calTabs->setCurrentIndex(t);
			verifyTabFits(calTabs->tabText(t), calTabs->widget(t));
		}

		// Also verify Advanced fits at 760x540 with the transcripts card expanded.
		calTabs->setCurrentIndex(1);
		auto* transcriptsToggle = window.findChild<QToolButton*>(QStringLiteral("calibration.card.transcripts.toggle"));
		require(transcriptsToggle != nullptr, "CUI fit (760x540): transcripts toggle missing");
		transcriptsToggle->setChecked(true);
		verifyTabFits(QStringLiteral("Advanced (transcripts expanded)"), calTabs->widget(1));

		// In-test negative control: injecting an oversized child widget MUST trigger horizontal overflow on 760x540.
		calTabs->setCurrentIndex(0);
		for (int n = 0; n < 3; ++n) QCoreApplication::processEvents();
		QLayout* setupLayout = calTabs->widget(0)->layout();
		require(setupLayout != nullptr, "CUI fit negative control: Setup & Wizard tab has no layout");
		{
			std::unique_ptr<QWidget> oversized(new QWidget);
			oversized->setMinimumWidth(scroll->viewport()->width() + 200);
			setupLayout->addWidget(oversized.get());
			for (int n = 0; n < 3; ++n) QCoreApplication::processEvents();
			require(scroll->horizontalScrollBar()->maximum() > 0 &&
				scroll->widget()->width() > scroll->viewport()->width(),
				"CUI fit negative control failed: oversized child did not trigger horizontal overflow at 760x540");
			setupLayout->removeWidget(oversized.get());
		}
		for (int n = 0; n < 3; ++n) QCoreApplication::processEvents();
		require(scroll->horizontalScrollBar()->maximum() == 0,
			"CUI fit negative control cleanup: horizontal overflow remained after removing oversized probe");
	}

	// CUI-M1: kit.bindTextFn must persist and reload exact UTF-8 bytes (not lossy toLocal8Bit ACP encoding)
	// for CalibrationPage text fields such as argyll_path and spectral_correction.
	void testCalibrationBindTextUtf8RoundTrip()
	{
		Vpv::RegisterConfigProviders();
		const QString utf8Argyll = QString::fromUtf8("C:\\cal\\caf\xC3\xA9_\xE6\x97\xA5\xE6\x9C\xAC\\spotread.exe");
		const QString utf8Correction = QString::fromUtf8("C:\\cal\\caf\xC3\xA9_\xE6\x97\xA5\xE6\x9C\xAC\\wled.ccss");
		const QByteArray expectedArgyllBytes = utf8Argyll.toUtf8();
		const QByteArray expectedCorrectionBytes = utf8Correction.toUtf8();

		// In-test negative control: prove the test string genuinely contains multi-byte non-Latin-1/non-ACP
		// characters that legacy single-byte encoding corrupts.
		require(utf8Argyll.toLatin1() != expectedArgyllBytes &&
			QString::fromUtf8(utf8Argyll.toLatin1()) != utf8Argyll,
			"CUI-M1 negative control precondition: test path must contain non-ASCII/CJK characters lost by single-byte encoding");
		const QByteArray localArgyllBytes = utf8Argyll.toLocal8Bit();
		const bool nonUtf8Acp = (localArgyllBytes != expectedArgyllBytes);
		if (nonUtf8Acp)
		{
			require(QString::fromUtf8(localArgyllBytes) != utf8Argyll &&
				QString::fromLocal8Bit(localArgyllBytes) != utf8Argyll,
				"CUI-M1 negative control: toLocal8Bit() on non-UTF-8 ACP must visibly corrupt Japanese/accented characters");
		}

		QTemporaryDir dir;
		const QString path = writeFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* argyll = window.findChild<QLineEdit*>(QStringLiteral("config.calibration.argyll_path"));
			auto* correction = window.findChild<QLineEdit*>(QStringLiteral("config.calibration.spectral_correction"));
			require(argyll != nullptr && correction != nullptr,
				"CUI-M1: CalibrationPage bindTextFn QLineEdits (argyll_path, spectral_correction) missing");
			argyll->setText(utf8Argyll);
			correction->setText(utf8Correction);
			QCoreApplication::processEvents();
			g_save(window);
		}

		// Verify raw saved file bytes contain exact UTF-8 sequences (and NOT corrupted local8Bit bytes).
		QByteArray savedBytes;
		{
			QFile saved(path);
			require(saved.open(QIODevice::ReadOnly), "CUI-M1: cannot open saved calibration config");
			savedBytes = saved.readAll();
		}
		require(savedBytes.contains(QByteArray("argyll_path: ") + expectedArgyllBytes),
			"CUI-M1: saved calibration config does not contain exact UTF-8 bytes for argyll_path");
		require(savedBytes.contains(QByteArray("spectral_correction: ") + expectedCorrectionBytes),
			"CUI-M1: saved calibration config does not contain exact UTF-8 bytes for spectral_correction");
		if (nonUtf8Acp)
		{
			require(!savedBytes.contains(QByteArray("argyll_path: ") + localArgyllBytes),
				"CUI-M1 negative control: saved calibration config stored lossy toLocal8Bit() bytes instead of UTF-8");
		}

		// Reload in a fresh ConfigEditorWindow and verify exact QString round-trip, then verify empty-clear removes the key.
		{
			ConfigEditorWindow reopened(path, 0, true);
			auto* argyll = reopened.findChild<QLineEdit*>(QStringLiteral("config.calibration.argyll_path"));
			auto* correction = reopened.findChild<QLineEdit*>(QStringLiteral("config.calibration.spectral_correction"));
			require(argyll != nullptr && argyll->text() == utf8Argyll,
				"CUI-M1: reopened argyll_path did not round-trip exact UTF-8 characters");
			require(correction != nullptr && correction->text() == utf8Correction,
				"CUI-M1: reopened spectral_correction did not round-trip exact UTF-8 characters");
			argyll->setText(QStringLiteral("   "));
			QCoreApplication::processEvents();
			g_save(reopened);
		}
		{
			QFile cleared(path);
			require(cleared.open(QIODevice::ReadOnly), "CUI-M1: cannot reopen cleared calibration config");
			const QByteArray afterClear = cleared.readAll();
			require(!afterClear.contains("argyll_path:"),
				"CUI-M1: clearing a kit.bindTextFn field to whitespace must remove argyll_path on save");
			require(afterClear.contains(QByteArray("spectral_correction: ") + expectedCorrectionBytes),
				"CUI-M1: clearing argyll_path must leave spectral_correction UTF-8 bytes intact");
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
	failures += run("CalibrationPageTests: CUI minimum viewport 760x540 fit across tabs", testCalibrationMinimumViewport760x540Fit);
	failures += run("CalibrationPageTests: CUI-M1 bindTextFn UTF-8 round-trip without local8Bit corruption", testCalibrationBindTextUtf8RoundTrip);
	return failures;
}
