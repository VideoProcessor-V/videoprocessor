// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// The projector cards of the Dynamic Drive page, moved out of ConfigEditorWindow::createLedDrivePage line for
// line: only the way the code reaches the editor changed (the EditorKit of ConfigPageHost.h through
// DriveConfigBase instead of `this`). Object names, tooltips, rows and widget creation order are unchanged.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "ProjectorCards.h"

#include <ConfigurationRpcClient.h>

#include <VP-V/modules/drive/led_compensation/LedDriveConfig.h>
#include <VP-V/modules/drive/projector_control/ConnectionTest.h>
#include <VP-V/modules/drive/projector_control/SerialPort.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <string>

namespace Vpv
{
using DriveConfig::flagClampedLedValue;

namespace
{
// RS-232 connection test: whether VideoProcessor.exe runs on this computer
// (then the test goes through it, never around it).
bool LocalVideoProcessorRunning()
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return true;   // unknown: do not open the port behind VP's back
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	bool found = false;
	for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
		found = _wcsicmp(entry.szExeFile, L"VideoProcessor.exe") == 0;
	CloseHandle(snapshot);
	return found;
}
}

ProjectorCards::Connection ProjectorCards::BuildConnection()
{
    const QString projector = QStringLiteral("projector.default");
    auto* connection = new QWidget;
    auto* connectionForm = new QFormLayout(connection);
    connectionForm->setContentsMargins(0, 0, 0, 0);
    connectionForm->setVerticalSpacing(8);
    // VP's default is no projector (audit 2026-10-14c: an unset protocol
    // showed Barco while VP opened nothing); picking a source that drives
    // the projector switches it to Barco (below).
    auto* protocolChoice = bindChoiceField(projector, QStringLiteral("protocol"),
        { QStringLiteral("none"), QStringLiteral("barco_ascii") },
        { QStringLiteral("No projector (never open a port)"), QStringLiteral("Barco serial (RS232)") });
    connectionForm->addRow(QStringLiteral("Protocol"), protocolChoice);
    auto* port = bindTextField(projector, QStringLiteral("port"), QStringLiteral("COM1"));
    port->setMaximumWidth(140);
    connectionForm->addRow(QStringLiteral("COM port"), port);
    // How long the projector takes to change the light after a command; the
    // picture gain is timed to land with it. Tuned by eye on scene changes.
    auto* latency = new QSpinBox;
    latency->setObjectName(controlName(projector, QStringLiteral("led_latency_ms")));
    latency->setRange(0, 2000);
    latency->setSingleStep(10);
    latency->setSuffix(QStringLiteral(" ms"));
    latency->setMaximumWidth(120);
    latency->setAlignment(Qt::AlignRight);
    {
        bool valid = false;
        const int configured = value(projector, QStringLiteral("led_latency_ms")).toInt(&valid);
        latency->setValue(valid ? std::max(0, std::min(configured, 2000)) : 80);
        flagClampedLedValue(latency, value(projector, QStringLiteral("led_latency_ms")), 0, 2000);
    }
    connect(latency, qOverload<int>(&QSpinBox::valueChanged), this, [this, projector](int ms)
    {
        if (!document_) return;
        const std::string section = projector.toStdString();
        if (!document_->SetKnown(section, "led_latency_ms", std::to_string(ms)))
        {
            document_->AddSection(section);
            document_->SetKnown(section, "led_latency_ms", std::to_string(ms));
        }
        markDirty();
    });
    connectionForm->addRow(QStringLiteral("LED response time"), latency);
    connectionForm->addRow(QString(), helpLabel(QStringLiteral(
        "How long the light takes to change after VP sends a command; the picture gain is timed to land with "
        "it. On a dark-to-bright cut: a glow or lifted blacks just before the cut - lower it; the bright scene "
        "starts too dark or dips - raise it. Change in steps of 20 ms. Default 80.")));
    // The same for the projector's contrast (CNTR): script mode's smoothing
    // times VP's gain with it.
    auto* contrastLatency = new QSpinBox;
    contrastLatency->setObjectName(controlName(projector, QStringLiteral("cntr_latency_ms")));
    contrastLatency->setRange(0, 2000);
    contrastLatency->setSingleStep(10);
    contrastLatency->setSuffix(QStringLiteral(" ms"));
    contrastLatency->setMaximumWidth(120);
    contrastLatency->setAlignment(Qt::AlignRight);
    {
        bool valid = false;
        const int configured = value(projector, QStringLiteral("cntr_latency_ms")).toInt(&valid);
        // audit 2026-10-16: clamp to the control's range (0-2000), not 500.
        contrastLatency->setValue(valid ? std::max(contrastLatency->minimum(),
            std::min(configured, contrastLatency->maximum())) : 80);
        flagClampedLedValue(contrastLatency, value(projector, QStringLiteral("cntr_latency_ms")), 0, 2000);
    }
    connect(contrastLatency, qOverload<int>(&QSpinBox::valueChanged), this, [this, projector](int ms)
    {
        if (!document_) return;
        const std::string section = projector.toStdString();
        if (!document_->SetKnown(section, "cntr_latency_ms", std::to_string(ms)))
        {
            document_->AddSection(section);
            document_->SetKnown(section, "cntr_latency_ms", std::to_string(ms));
        }
        markDirty();
    });
    connectionForm->addRow(QStringLiteral("Contrast response time"), contrastLatency);
    connectionForm->addRow(QString(), helpLabel(QStringLiteral(
        "How long the projector's contrast takes to change after a command. Used to time VP's picture "
        "adjustment with each contrast step (adaptive gamma, and Compensate): if the picture briefly dips or "
        "brightens at a contrast step, adjust it in steps of 20 ms. Default 80.")));
    auto* readBack = bindCheckField(QStringLiteral("Read the projector's replies"), projector,
        QStringLiteral("read_back"), false);
    connectionForm->addRow(QString(), readBack);
    connectionForm->addRow(QString(), helpLabel(QStringLiteral(
        "Default off: send only, exactly like the original script (which never read the projector back). "
        "Start-up still reaches a known state without replies: light to the ceiling and CNTR 50 are sent "
        "unconditionally, and the light range comes from LED floor / ceiling. A send is retried only when the "
        "write itself fails. On (diagnostics, the old behaviour): wait for each reply, ask the projector its "
        "range and state at start-up and log lost commands. The RS-232 connection test always reads back. "
        "19200 baud, 8 data bits, no parity, 1 stop bit, no flow control.")));
    // RS-232 connection test: LED 60, read back, 5 s, LED 255, read back.
    auto* testButton = new QPushButton(QStringLiteral("Test RS-232 connection"));
    testButton->setObjectName(controlName(projector, QStringLiteral("connection_test")));
    testButton->setToolTip(QStringLiteral(
        "Sets the LED to 60, reads it back, waits 5 seconds, sets 255 and reads it back (about 8 s). Uses the "
        "saved settings (Apply first). With VideoProcessor running it pauses Dynamic Drive for the test; it is "
        "refused while video is playing. Without VideoProcessor, Config opens the port itself."));
    auto* testResult = new QLabel;
    testResult->setObjectName(controlName(projector, QStringLiteral("connection_test.result")));
    testResult->setWordWrap(true);
    testResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    connectionForm->addRow(testButton, testResult);
    if (projectorTestThread_)
    {
        // audit 2026-10-16: a test started before this page was rebuilt.
        testButton->setEnabled(false);
        testResult->setText(QStringLiteral("Testing… about 8 s"));
    }
    // audit 2026-10-16: a rebuild (Cancel, reload, target switch) deletes
    // this button and label while the ~8 s test runs; guard them with
    // QPointer and keep the worker in a member that the destructor waits for.
    connect(testButton, &QPushButton::clicked, this, [this, projector, button = QPointer<QPushButton>(testButton),
        label = QPointer<QLabel>(testResult)]
    {
        if (kit_.noTarget() || !button || !label) return;
        if (kit_.settingsPreview())
        {
            label->setText(QStringLiteral("Settings preview: VP is not connected, so the projector cannot be tested."));
            return;
        }
        if (projectorTestThread_)
        {
            label->setStyleSheet(QString());
            label->setText(QStringLiteral("A connection test is already running; wait for it to finish."));
            return;
        }
        QPushButton* testButton = button;
        QLabel* testResult = label;
        const bool remote = kit_.remoteTarget().remote;
        const QString host = remote ? kit_.remoteTarget().host : QStringLiteral("127.0.0.1");
        const quint16 rpcPort = remote ? kit_.remoteTarget().port : 41686;
        const std::string configPath = kit_.configPath().toStdString();
        testButton->setEnabled(false);
        testResult->setStyleSheet(QString());
        testResult->setText(kit_.documentDirty() ?
            QStringLiteral("Testing with the saved settings (unapplied changes are not used)… about 8 s") :
            QStringLiteral("Testing… about 8 s"));
        struct Outcome { bool pass = false; bool refused = false; QString summary, details; };
        const auto outcome = std::make_shared<Outcome>();
        QThread* worker = QThread::create([outcome, host, rpcPort, remote, configPath]
        {
            ConfigurationRpcClient client(host.toStdString(), rpcPort);
            ConfigurationRpcClient::ProjectorTestResult rpc;
            std::string error;
            if (client.RunProjectorTest(rpc, error))
            {
                outcome->pass = rpc.pass;
                outcome->refused = rpc.refused;
                outcome->summary = QString::fromStdString(rpc.summary);
                outcome->details = QString::fromStdString(rpc.details);
                return;
            }
            // Config opens the port itself only when no VP runs on this
            // computer. A VP that runs but does not answer (busy, a test
            // already running, its RPC off) keeps the port and its playback
            // refusal: show its error (audit stage8-rs232 item 2).
            if (remote || LocalVideoProcessorRunning())
            {
                outcome->summary = QStringLiteral("FAIL: ") + QString::fromStdString(error);
                return;
            }
            ConfigFile config;
            if (configPath.empty() || !config.Load(configPath))
            {
                outcome->summary = QStringLiteral("FAIL: could not read the configuration");
                return;
            }
            const LedDrive::ConnectionTestResult result = LedDrive::RunConnectionTestOnNewConnection(
                LedDrive::ReadProjectorConfig(config), [] { return LedDrive::CreateWin32SerialPort(); });
            outcome->pass = result.pass;
            outcome->summary = QString::fromStdString(result.summary) +
                QStringLiteral(" (tested by Config; VideoProcessor is not running)");
            for (const std::string& line : result.lines)
                outcome->details += QString::fromStdString(line) + QLatin1Char('\n');
        });
        worker->setParent(kit_.window());
        projectorTestThread_ = worker;
        connect(worker, &QThread::finished, this, [this, projector, worker, outcome, button, label]
        {
            if (projectorTestThread_ == worker) projectorTestThread_.clear();
            worker->deleteLater();
            // audit 2026-10-16: find the current page's controls by name when
            // the originals were rebuilt meanwhile.
            QPointer<QPushButton> shownButton = button;
            QPointer<QLabel> shownResult = label;
            if (!shownButton)
                shownButton = editorChild<QPushButton*>(controlName(projector,
                    QStringLiteral("connection_test")));
            if (!shownResult)
                shownResult = editorChild<QLabel*>(controlName(projector,
                    QStringLiteral("connection_test.result")));
            if (shownButton) shownButton->setEnabled(true);
            if (!shownResult) return;
            shownResult->setText(outcome->summary);
            shownResult->setToolTip(outcome->details.trimmed());
            shownResult->setStyleSheet(outcome->pass ? QStringLiteral("color: #2e7d32;") :
                outcome->refused ? QString() : QStringLiteral("color: #c62828;"));
        });
        worker->start();
    });
    return { connection, protocolChoice };
}

ProjectorCards::Response ProjectorCards::BuildResponse()
{
    const QString projector = QStringLiteral("projector.default");
    auto* response = new QWidget;
    auto* responseForm = new QFormLayout(response);
    responseForm->setContentsMargins(0, 0, 0, 0);
    responseForm->setVerticalSpacing(8);
    responseForm->addRow(QStringLiteral("LED settle time (ms)"),
        numberControl(projector, QStringLiteral("led_settle_ms"), 0, 5000, 0, 10, 300, QString()));
    responseForm->addRow(QStringLiteral("Contrast settle time (ms)"),
        numberControl(projector, QStringLiteral("cntr_settle_ms"), 0, 5000, 0, 10, 0, QString()));
    auto* maxUsable = numberControl(projector, QStringLiteral("cntr_max_usable"), 50, 100, 0, 1, 74, QString());
    responseForm->addRow(QStringLiteral("Highest useful contrast"), maxUsable);
    // audit 2026-10-16: dmd_clip_cntr -1 = cntr_max_usable, shown as the
    // special value below 50 and written as -1 (not removed: unset is 59).
    auto* whiteClips = numberControl(projector, QStringLiteral("dmd_clip_cntr"), 50, 100, 0, 1, 59,
        QStringLiteral("Usable max (-1)"));
    {
        const QSignalBlocker blocked(whiteClips);
        bool valid = false;
        const double configured = value(projector, QStringLiteral("dmd_clip_cntr")).toDouble(&valid);
        whiteClips->setValue(!valid ? 59.0 : configured < 50.0 ? whiteClips->minimum() : std::min(configured, 100.0));
    }
    disconnect(whiteClips, qOverload<double>(&QDoubleSpinBox::valueChanged), this, nullptr);
    connect(whiteClips, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
        [this, projector, whiteClips](double number)
    {
        if (!document_) return;
        const std::string name = projector.toStdString();
        const std::string text = number <= whiteClips->minimum() ? std::string("-1") :
            QString::number(number, 'f', 0).toStdString();
        if (!document_->SetKnown(name, "dmd_clip_cntr", text))
        {
            document_->AddSection(name);
            document_->SetKnown(name, "dmd_clip_cntr", text);
        }
        markDirty();
    });
    responseForm->addRow(QStringLiteral("Contrast where white clips"), whiteClips);
    responseForm->addRow(QString(), helpLabel(QStringLiteral(
        "From the camera and light-meter measurements; leave the defaults until then. Settle time: how long "
        "the light takes to arrive after the response time (300 ms is deliberately fast). White clips: the "
        "contrast at which a white field stops getting brighter at full LED (59 = where your script's dump "
        "ended).")));
    return { response, responseForm, maxUsable, whiteClips };
}

QWidget* ProjectorCards::BuildAdvanced()
{
    const QString projector = QStringLiteral("projector.default");
    // Settings VP reads that only a hand edit could set before (audit
    // 2026-10-14c: every setting has a Config control): collapsed, expert.
    auto* advancedProjector = new QWidget;
    auto* advancedForm = new QFormLayout(advancedProjector);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    advancedForm->setVerticalSpacing(8);
    advancedForm->addRow(QString(), helpLabel(QStringLiteral(
        "Only for a projector that needs different serial settings, or when the projector's replies say something is wrong. "
        "The defaults suit the projector.")));
    advancedForm->addRow(QStringLiteral("Baud rate"),
        numberControl(projector, QStringLiteral("baud"), 1200, 921600, 0, 1, 19200, QString()));
    advancedForm->addRow(QStringLiteral("Lowest LED level the projector accepts"),
        numberControl(projector, QStringLiteral("light_min"), 0, 255, 0, 1, 0, QStringLiteral("Ask the projector")));
    advancedForm->addRow(QStringLiteral("Highest LED level the projector accepts"),
        numberControl(projector, QStringLiteral("light_max"), 0, 255, 0, 1, 0, QStringLiteral("Ask the projector")));
    advancedForm->addRow(QStringLiteral("Wait for a reply (ms)"),
        numberControl(projector, QStringLiteral("ack_timeout_ms"), 20, 5000, 0, 10, 300, QString()));
    advancedForm->addRow(QStringLiteral("Gap between commands (ms)"),
        numberControl(projector, QStringLiteral("min_command_gap_ms"), 0, 5000, 0, 10, 200, QString()));
    advancedForm->addRow(QStringLiteral("Largest LED step per command"),
        numberControl(projector, QStringLiteral("max_levels_per_command"), 0, 255, 0, 1, 12, QString()));
    advancedForm->addRow(QStringLiteral("Commands before a pause (0 = no pause)"),
        numberControl(projector, QStringLiteral("burst_commands"), 0, 1000, 0, 1, 0, QString()));
    advancedForm->addRow(QStringLiteral("Pause length (ms)"),
        numberControl(projector, QStringLiteral("burst_pause_ms"), 0, 60000, 0, 100, 5000, QString()));
    advancedForm->addRow(QString(), bindCheckField(QStringLiteral("Ask the projector to echo commands"), projector,
        QStringLiteral("echo"), true));
    advancedForm->addRow(QStringLiteral("Line ending"), bindChoiceField(projector, QStringLiteral("terminator"),
        { QStringLiteral("auto"), QStringLiteral("cr"), QStringLiteral("literal_cr"), QStringLiteral("script") },
        { QStringLiteral("Automatic (try the projector)"), QStringLiteral("Carriage return"),
          QStringLiteral("The letters \"cr\" then a carriage return"), QStringLiteral("Exactly as your script sent it") }));
    auto* fluxTable = bindTextField(projector, QStringLiteral("flux_table"));
    fluxTable->setPlaceholderText(QStringLiteral("empty = light proportional to the LED level"));
    fluxTable->setToolTip(QStringLiteral(
        "Light-meter readings as LED level:relative light pairs, e.g. 60:0.21,128:0.49,255:1. Compensate uses "
        "them to know how much light each LED level really gives."));
    advancedForm->addRow(QStringLiteral("Measured light per LED level"), fluxTable);
    return advancedProjector;
}

QWidget* ProjectorCards::WrapAdvanced(QWidget* advancedProjector)
{
    const QString projector = QStringLiteral("projector.default");
    // Collapsed until asked for.
    auto* advancedHolder = new QWidget;
    auto* advancedLayout = new QVBoxLayout(advancedHolder);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    auto* advancedToggle = new QToolButton;
    advancedToggle->setObjectName(controlName(projector, QStringLiteral("advanced_toggle")));
    advancedToggle->setText(QStringLiteral("Show the advanced settings"));
    advancedToggle->setCheckable(true);
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle->setArrowType(Qt::RightArrow);
    advancedLayout->addWidget(advancedToggle);
    advancedLayout->addWidget(advancedProjector);
    advancedProjector->setVisible(false);
    connect(advancedToggle, &QToolButton::toggled, this, [advancedToggle, advancedProjector](bool open)
    {
        advancedProjector->setVisible(open);
        advancedToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        advancedToggle->setText(open ? QStringLiteral("Hide the advanced settings") : QStringLiteral("Show the advanced settings"));
    });
    return advancedHolder;
}

void ProjectorCards::JoinTest()
{
    // audit 2026-10-16: the RS-232 test may still run.
    if (projectorTestThread_)
    {
        projectorTestThread_->wait();
        projectorTestThread_.clear();
    }
}
}
