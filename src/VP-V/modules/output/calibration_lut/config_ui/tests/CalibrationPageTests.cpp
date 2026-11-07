// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// The Calibration settings page as a Config page provider (R1 W1 CAL-C): it is registered as a
// sub-page of the master VP-V page (grouped under Output, no navigation leaf of its own), it keeps
// the editor's object names and write path, it takes a status reply through the test seam found by
// ConfigPageHost::Find<CalibrationPage>(), and its per-editor state goes with the editor.
// Registered from ConfigEditorWindowTests.cpp's main with one line: RunCalibrationPageTests().
// Run with --match CalibrationPageTests.

#include "ConfigEditorWindow.h"
#include <VP-V/config/VpvConfigProviders.h>

#include <QAbstractButton>
#include <QCoreApplication>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>

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
		for (QPushButton* button : window.findChildren<QPushButton*>())
			require(button->property("pageName").toString() != QStringLiteral("Calibration"),
				"the Calibration sub-page has a navigation leaf of its own");
		auto* open = window.findChild<QPushButton*>(QStringLiteral("vpv.output.subpage.Calibration"));
		require(open != nullptr, "the Output unit of the VP-V page has no button for the Calibration page");
		window.selectPage(QStringLiteral("General"));
		open->click();
		QCoreApplication::processEvents();
		require(pages->currentIndex() == calibration, "the Output unit's Calibration button opened another page");
		bool vpvLeafLit = false;
		for (QPushButton* button : window.findChildren<QPushButton*>())
			if (button->property("pageName").toString() == QStringLiteral("VP-V") && button->isChecked()) vpvLeafLit = true;
		require(vpvLeafLit, "opening a sub-page did not light the VP-V navigation leaf");
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
	return failures;
}
