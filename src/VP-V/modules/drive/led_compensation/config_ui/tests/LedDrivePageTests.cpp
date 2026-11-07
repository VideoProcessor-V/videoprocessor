// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// The Dynamic Drive page as a Config page provider (M2 lane C, P7-3): it is registered as a sub-page of
// the master VP-V page (grouped under Drive, no navigation leaf of its own) ahead of Calibration, it keeps
// the editor's object names, write path (including Reset removing a key), card order and the projector
// cards, and its per-editor state goes with the editor.
// Registered from ConfigEditorWindowTests.cpp's main with one line: RunLedDrivePageTests().
// Run with --match LedDrivePageTests.

#include "ConfigEditorWindow.h"
#include <VP-V/config/VpvConfigProviders.h>

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>

#include <atomic>
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
		const QByteArray text =
			"[general]\nrenderer: VP Renderer\n"
			"[led_drive.default]\nenabled: true\ncommand_source: compensate\nled_floor: 90\nshadow_lift_ansi: 0.25\ntick_ms: 500\n"
			"[projector.default]\nprotocol: barco_ascii\nport: COM7\nled_latency_ms: 120\nbaud: 38400\n";
		require(file.write(text) == text.size(), "Cannot write fixture");
		return path;
	}

	QString readAll(const QString& path)
	{
		QFile file(path);
		require(file.open(QIODevice::ReadOnly), "Cannot read the saved file");
		return QString::fromUtf8(file.readAll());
	}

	void testDynamicDriveIsASubPageBeforeCalibration()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		ConfigEditorWindow window(writeFixture(dir), 0, true);
		auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
		require(pages != nullptr, "no page stack");
		const int drive = window.pageIndex(QStringLiteral("Dynamic Drive"));
		const int calibration = window.pageIndex(QStringLiteral("Calibration"));
		const int vpv = window.pageIndex(QStringLiteral("VP-V"));
		require(drive >= 0 && calibration == drive + 1 && vpv == calibration + 1,
			"the stack order is not Dynamic Drive, Calibration, VP-V");
		for (QPushButton* button : window.findChildren<QPushButton*>())
			require(button->property("pageName").toString() != QStringLiteral("Dynamic Drive"),
				"the Dynamic Drive sub-page has a navigation leaf of its own");
		auto* open = window.findChild<QPushButton*>(QStringLiteral("vpv.drive.subpage.Dynamic Drive"));
		require(open != nullptr, "the Drive unit of the VP-V page has no button for the Dynamic Drive page");
		window.selectPage(QStringLiteral("General"));
		open->click();
		QCoreApplication::processEvents();
		require(pages->currentIndex() == drive, "the Drive unit's button opened another page");
	}

	void testControlsKeepNamesAndShowStoredValues()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		ConfigEditorWindow window(writeFixture(dir), 0, true);
		for (const char* name : { "config.led_drive.default.command_source", "config.led_drive.default.command_source.buttons",
				"config.led_drive.default.adaptive_gamma", "config.led_drive.default.enabled", "config.led_drive.default.led_floor",
				"config.led_drive.default.compensation_eotf", "config.led_drive.default.shadow_lift_ansi", "config.led_drive.default.profile_file",
				"config.led_drive.default.profile_browse", "config.led_drive.default.profile_status", "config.led_drive.default.ftb_experimental",
				"config.led_drive.default.advanced_toggle", "config.led_drive.default.tick_ms", "config.led_drive.default.curve",
				"config.projector.default.protocol", "config.projector.default.port", "config.projector.default.led_latency_ms",
				"config.projector.default.cntr_latency_ms", "config.projector.default.connection_test",
				"config.projector.default.connection_test.result", "config.projector.default.led_settle_ms",
				"config.projector.default.dmd_clip_cntr", "config.projector.default.advanced_toggle", "config.projector.default.baud" })
			require(window.findChild<QWidget*>(QString::fromLatin1(name)) != nullptr,
				(std::string("Dynamic Drive control missing: ") + name).c_str());
		auto* floor = window.findChild<QSpinBox*>(QStringLiteral("config.led_drive.default.led_floor"));
		require(floor && floor->value() == 90, "the LED floor does not show the stored value");
		auto* lift = window.findChild<QDoubleSpinBox*>(QStringLiteral("config.led_drive.default.shadow_lift_ansi"));
		require(lift && lift->value() > 0.249 && lift->value() < 0.251, "the shadow lift does not show the stored value");
		auto* latency = window.findChild<QSpinBox*>(QStringLiteral("config.projector.default.led_latency_ms"));
		require(latency && latency->value() == 120, "the LED response time does not show the stored value");
		auto* baud = window.findChild<QDoubleSpinBox*>(QStringLiteral("config.projector.default.baud"));
		require(baud && baud->value() == 38400.0, "the baud rate does not show the stored value");
		auto* source = window.findChild<QComboBox*>(QStringLiteral("config.led_drive.default.command_source"));
		require(source && source->currentData().toString() == QStringLiteral("compensate"), "the command source does not show compensate");
	}

	void testEditsAndResetsReachTheFile()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		const QString path = writeFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* floor = window.findChild<QSpinBox*>(QStringLiteral("config.led_drive.default.led_floor"));
			require(floor != nullptr, "no LED floor box");
			floor->setValue(120);
			auto* latency = window.findChild<QSpinBox*>(QStringLiteral("config.projector.default.led_latency_ms"));
			require(latency != nullptr, "no LED response time box");
			latency->setValue(140);
			auto* tick = window.findChild<QDoubleSpinBox*>(QStringLiteral("config.led_drive.default.tick_ms"));
			require(tick != nullptr, "no fixed loop interval box");
			auto* reset = window.findChild<QToolButton*>(QStringLiteral("config.led_drive.default.tick_ms.reset"));
			require(reset != nullptr && reset->isEnabled(), "the stored fixed loop interval has no enabled Reset");
			reset->click();
			auto* lift = window.findChild<QDoubleSpinBox*>(QStringLiteral("config.led_drive.default.shadow_lift_ansi"));
			require(lift != nullptr, "no shadow lift box");
			lift->setValue(0.4);
			g_save(window);
		}
		const QString saved = readAll(path);
		require(saved.contains(QStringLiteral("led_floor: 120")), "an LED floor edit was not saved");
		require(saved.contains(QStringLiteral("led_latency_ms: 140")), "a projector response time edit was not saved");
		require(saved.contains(QStringLiteral("shadow_lift_ansi: 0.40")), "a shadow lift edit was not saved");
		require(!saved.contains(QStringLiteral("tick_ms")), "Reset did not remove the setting from the file");
	}

	void testSourcePickWritesTheModeToo()
	{
		Vpv::RegisterConfigProviders();
		QTemporaryDir dir;
		const QString path = writeFixture(dir);
		{
			ConfigEditorWindow window(path, 0, true);
			auto* none = window.findChild<QAbstractButton*>(QStringLiteral("config.led_drive.default.command_source.none"));
			require(none != nullptr, "no 'No LED commands' button");
			none->click();
			g_save(window);
		}
		const QString saved = readAll(path);
		require(saved.contains(QStringLiteral("command_source: none")), "the command source pick was not saved");
		require(saved.contains(QStringLiteral("mode: log_only")) || saved.contains(QStringLiteral("mode: picture_only")),
			"the command source pick did not write the matching mode");
	}

	void testRs232TestSurvivesARebuildAndIsJoinedOnClose()
	{
		Vpv::RegisterConfigProviders();
		auto* provider = ConfigPageHost::Find<Vpv::LedDrivePage>();
		require(provider != nullptr, "no Dynamic Drive provider");
		const int before = provider->StateCountForTesting();
		QTemporaryDir dir;
		std::atomic<bool> release(false);
		{
			ConfigEditorWindow window(writeFixture(dir), 0, true);
			QThread* fake = QThread::create([&release] { while (!release.load()) QThread::msleep(5); });
			fake->start();
			require(provider->AdoptProjectorTestThreadForTesting(&window, fake), "the provider does not serve this editor");
			require(provider->ProjectorTestRunningForTesting(&window), "the test thread is not recorded as running");
			// A rebuild (Cancel) deletes the page; the new one shows the test as still running.
			window.findChild<QPushButton*>(QStringLiteral("cancelConfiguration"))->click();
			QCoreApplication::processEvents();
			auto* button = window.findChild<QPushButton*>(QStringLiteral("config.projector.default.connection_test"));
			require(button != nullptr && !button->isEnabled(), "the rebuilt page enables the test button while a test runs");
			auto* result = window.findChild<QLabel*>(QStringLiteral("config.projector.default.connection_test.result"));
			require(result != nullptr && result->text().startsWith(QStringLiteral("Testing")), "the rebuilt page does not say the test runs");
			require(provider->ProjectorTestRunningForTesting(&window), "a rebuild lost the running test");
			// The editor closing joins the thread: release it first so the join returns.
			release = true;
			fake->setParent(&window);
		}
		require(provider->StateCountForTesting() == before, "the provider kept state after its editor closed");
	}

	void testStateGoesWithTheEditor()
	{
		Vpv::RegisterConfigProviders();
		auto* provider = ConfigPageHost::Find<Vpv::LedDrivePage>();
		require(provider != nullptr, "ConfigPageHost::Find<LedDrivePage>() found no provider");
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

int RunLedDrivePageTests(const std::function<int(const char*, const std::function<void()>&)>& run,
	const std::function<void(bool, const char*)>& requireHelper, const std::function<void(ConfigEditorWindow&)>& saveHelper)
{
	g_require = requireHelper;
	g_save = saveHelper;
	int failures = 0;
	failures += run("LedDrivePageTests: Dynamic Drive is a sub-page before Calibration", testDynamicDriveIsASubPageBeforeCalibration);
	failures += run("LedDrivePageTests: controls keep names and show stored values", testControlsKeepNamesAndShowStoredValues);
	failures += run("LedDrivePageTests: edits and resets reach the file", testEditsAndResetsReachTheFile);
	failures += run("LedDrivePageTests: a source pick writes the mode too", testSourcePickWritesTheModeToo);
	failures += run("LedDrivePageTests: an RS-232 test survives a rebuild and is joined on close", testRs232TestSurvivesARebuildAndIsJoinedOnClose);
	failures += run("LedDrivePageTests: state goes with the editor", testStateGoesWithTheEditor);
	return failures;
}
