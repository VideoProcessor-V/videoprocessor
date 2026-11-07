// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// The Dynamic Drive settings page, moved out of ConfigEditorWindow (M2 lane C, P7-3): the page body is the
// editor's former createLedDrivePage line for line (ledDisplayDecimals and flagClampedLedValue in
// projector_control/config_ui/DriveConfigBase, the projector's cards in projector_control/config_ui/
// ProjectorCards); only the way it reaches the editor changed (the EditorKit of ConfigPageHost.h instead of
// `this`). Object names, tooltips, rows, widget creation order and the order of cards are unchanged.

#include "LedDrivePage.h"

#include <VP-V/modules/drive/led_compensation/LedDriveConfig.h>
#include <VP-V/modules/drive/projector_control/config_ui/DriveConfigBase.h>
#include <VP-V/modules/drive/projector_control/config_ui/ProjectorCards.h>

#include <QAbstractSpinBox>
#include <QButtonGroup>
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
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Vpv
{
using ConfigPageHost::EditorKit;
using DriveConfig::configuredCheck;
using DriveConfig::flagClampedLedValue;
using DriveConfig::ledDisplayDecimals;

namespace
{
class LedDriveBuilder final : public DriveConfig::DriveConfigBase
{
public:
    explicit LedDriveBuilder(EditorKit& kit) : DriveConfig::DriveConfigBase(kit), projectorCards_(kit) {}
    QWidget* createLedDrivePage();
    ProjectorCards& projectorCards() { return projectorCards_; }

private:
    ProjectorCards projectorCards_;
};

QWidget* LedDriveBuilder::createLedDrivePage()
{
    // Plain [led_drive.Default] / [projector.Default] settings (Stage 7). Only
    // the core choices are here; everything else comes from the built-in
    // profile (the beta_22 script's curve and tables) or an LED profile file.
    // VP applies changes on Apply without restarting the LED module unless it
    // starts or stops driving the projector.
    const QString led = QStringLiteral("led_drive.default");
    QWidget* cards = kit_.createCardGrid();

    // The projector's LED cannot go below 60 with LPW1, so the controls start
    // there; a configured value below it (the configuration allows 0-255) is
    // shown as it is, not as 60 (audit 2026-10-16).
    auto levelControl = [this, led](const QString& key, int fallback)
    {
        auto* spin = new QSpinBox;
        spin->setObjectName(controlName(led, key));
        bool valid = false;
        const int configured = value(led, key).toInt(&valid);
        const bool inRange = valid && configured >= 0 && configured <= 255;
        spin->setRange(inRange ? std::min(60, configured) : 60, 255);
        spin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
        spin->setMaximumWidth(108);
        spin->setAlignment(Qt::AlignRight);
        spin->setValue(inRange ? configured : fallback);
        flagClampedLedValue(spin, value(led, key), 0, 255);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, led, key](int level)
        {
            if (!document_) return;
            const std::string section = led.toStdString();
            if (!document_->SetKnown(section, key.toStdString().c_str(), std::to_string(level)))
            {
                document_->AddSection(section);
                document_->SetKnown(section, key.toStdString().c_str(), std::to_string(level));
            }
            markDirty();
        });
        return spin;
    };

    auto* drive = new QWidget;
    auto* driveForm = new QFormLayout(drive);
    driveForm->setContentsMargins(0, 0, 0, 0);
    driveForm->setVerticalSpacing(8);
    auto* enabled = bindCheckField(QStringLiteral("Dynamic Drive on"), led, QStringLiteral("enabled"), false);
    enabled->setAccessibleDescription(QStringLiteral(
        "Lets VideoProcessor drive the projector's LED. Off: nothing is sent to the projector."));
    driveForm->addRow(QString(), enabled);
    // The LED core (owner 2026-10-14): one place that holds the port, sends
    // and logs, fed by a chosen command source, with two switches. Older
    // configurations name only `mode`; the source shown is read from it, and
    // choosing a source writes command_source (and the matching mode).
    QComboBox* ledSource = nullptr;
    QCheckBox* ledAdaptive = nullptr;
    QLabel* compensateWarning = nullptr;
    QWidget* aheadRow = nullptr;
    QWidget* aheadHelp = nullptr;
    std::vector<QWidget*> pacingRows;
    {
        // An older file's retired source (command_source: python, mode:
        // listen) shows, and runs, VP's copy of the script (owner 2026-10-15).
        const QStringList sources = { QStringLiteral("internal_script"), QStringLiteral("compensate"),
            QStringLiteral("none") };
        auto* source = new QComboBox;
        ledSource = source;
        source->setObjectName(controlName(led, QStringLiteral("command_source")));
        source->setAccessibleName(QStringLiteral("Command source"));
        source->addItem(QStringLiteral("VP's copy of your script"), sources[0]);
        source->addItem(QStringLiteral("Compensate (VP's own controller)"), sources[1]);
        source->addItem(QStringLiteral("No LED commands"), sources[2]);
        const QString legacyMode = value(led, QStringLiteral("mode")).trimmed().toLower();
        QString configuredSource = value(led, QStringLiteral("command_source")).trimmed().toLower();
        // An older file names `mode` and no command_source; a new file names
        // neither (audit 2026-10-14c: only the former keeps the old switches).
        const bool olderFile = configuredSource.isEmpty() && !legacyMode.isEmpty();
        if (configuredSource.isEmpty())
            configuredSource = legacyMode == QStringLiteral("listen") || legacyMode == QStringLiteral("script") ? sources[0] :
                legacyMode == QStringLiteral("compensate") ? sources[1] : sources[2];
        if (configuredSource == QStringLiteral("python"))
            configuredSource = sources[0];
        source->setCurrentIndex(std::max(0, static_cast<int>(sources.indexOf(configuredSource))));
        driveForm->addRow(QStringLiteral("Command source"), source);
        {
            // Owner 2026-10-16: the source as mutually exclusive buttons; the drop-down stays as the
            // value holder (hidden), so writing and older files behave as before.
            auto* sourceButtons = new QWidget;
            sourceButtons->setObjectName(controlName(led, QStringLiteral("command_source.buttons")));
            auto* sourceLayout = new QHBoxLayout(sourceButtons);
            sourceLayout->setContentsMargins(0, 0, 0, 0);
            auto* sourceGroup = new QButtonGroup(sourceButtons);
            sourceGroup->setExclusive(true);
            const QStringList labels = { QStringLiteral("VP's copy of your script"), QStringLiteral("Compensate"),
                QStringLiteral("No LED commands") };
            QList<QRadioButton*> radios;
            for (int i = 0; i < sources.size(); ++i)
            {
                auto* radio = new QRadioButton(labels[i]);
                radio->setObjectName(controlName(led, QStringLiteral("command_source.") + sources[i]));
                sourceGroup->addButton(radio, i);
                sourceLayout->addWidget(radio);
                radios.push_back(radio);
                connect(radio, &QRadioButton::clicked, this, [source, i]() {
                    if (source->currentIndex() != i) source->setCurrentIndex(i);
                    else emit source->activated(i);   // converts an older file; writes "No LED commands" on a new one
                });
            }
            sourceLayout->addStretch(1);
            auto syncRadios = [source, radios]() {
                for (int i = 0; i < radios.size(); ++i)
                {
                    const QSignalBlocker blocker(radios[i]);
                    radios[i]->setChecked(source->currentIndex() == i);
                }
            };
            connect(source, qOverload<int>(&QComboBox::currentIndexChanged), this, [syncRadios](int) { syncRadios(); });
            syncRadios();
            driveForm->addRow(QStringLiteral("Command source"), sourceButtons);
            driveForm->setRowVisible(source, false);
        }
        driveForm->addRow(QString(), helpLabel(QStringLiteral(
            "Where the LED and contrast commands come from. VP holds the RS-232 port, sends them and logs them. "
            "VP's copy of your dimming script makes the script's decisions, in its rhythm. No LED "
            "commands with adaptive gamma on: only VP's picture adjustments, the port left free.")));
        // Adaptive gamma, shown as the file behaves: named; off for a new
        // file; for an older mode-only file, on where the picture adapted
        // there (compensate, picture only, script smoothing or a shadow lift).
        const QString adaptiveText = value(led, QStringLiteral("adaptive_gamma")).trimmed().toLower();
        const bool shadowLiftOn = value(led, QStringLiteral("shadow_lift_ansi")).toDouble() > 0.0;
        const bool smoothingOn = value(led, QStringLiteral("script_smoothing")).trimmed().toLower() == QStringLiteral("true");
        const bool adaptiveNow = !adaptiveText.isEmpty() ?
            (adaptiveText == QStringLiteral("true") || adaptiveText == QStringLiteral("on") || adaptiveText == QStringLiteral("1")) :
            olderFile && (legacyMode == QStringLiteral("compensate") || legacyMode == QStringLiteral("picture_only") ||
                ((legacyMode == QStringLiteral("script") || legacyMode == QStringLiteral("listen")) && smoothingOn) || shadowLiftOn);
        auto* adaptive = configuredCheck(QStringLiteral("Adaptive gamma"), QString(), adaptiveNow);
        ledAdaptive = adaptive;
        adaptive->setObjectName(controlName(led, QStringLiteral("adaptive_gamma")));
        adaptive->setEnabled(true);
        driveForm->addRow(QString(), adaptive);
        driveForm->addRow(QString(), helpLabel(QStringLiteral(
            "Off (the default): VP leaves the picture alone - exactly the projector's own steps. On: VP eases the "
            "picture across each light and contrast step, and lifts the darks next to large highlights (the shadow "
            "lift's strength, below).")));
        compensateWarning = helpLabel(QStringLiteral(
            "Compensate with adaptive gamma off: the LED still dims in dark scenes, but VP does not brighten the "
            "picture to make up for it, so dark scenes look darker. Tick adaptive gamma for Compensate's normal "
            "behaviour."));
        compensateWarning->setObjectName(controlName(led, QStringLiteral("compensate_warning")));
        driveForm->addRow(QString(), compensateWarning);
        auto* ahead = bindCheckField(QStringLiteral("Look-ahead"), led, QStringLiteral("lookahead"), true);
        aheadRow = ahead;
        driveForm->addRow(QString(), ahead);
        aheadHelp = helpLabel(QStringLiteral(
            "VP sees the coming frames (every frame the queue profile holds; Low Latency holds none) and times each "
            "change for when the LED gets there. Off: it reacts to the frame on screen."));
        driveForm->addRow(QString(), aheadHelp);
        // Restore switches (bug hunt 2 2026-10-17, owner decision): led_peak_full_coverage,
        // led_picture_only.
        auto* fullPeak = bindCheckField(QStringLiteral("Peak from every pixel of the picture (fix under test)"), led,
            QStringLiteral("led_peak_full_coverage"), true);
        fullPeak->setToolTip(QStringLiteral(
            "On (default): Dynamic Drive's peak (the script's highlight test) is the brightest area of the whole "
            "picture, averaged over areas the same share of the screen as your script's 854 x 480 screenshot "
            "downscale - every pixel counts, so a small lamp or specular is never missed. Off: the brightest of "
            "the sparse sample blocks, as before."));
        driveForm->addRow(QString(), fullPeak);
        auto* pictureOnly = bindCheckField(QStringLiteral("Measure the picture only, not subtitles (fix under test)"), led,
            QStringLiteral("led_picture_only"), true);
        pictureOnly->setToolTip(QStringLiteral(
            "On (default): a subtitle VP has detected in the picture (and where VP shows a moved subtitle) is left "
            "out of everything Dynamic Drive measures - peak, average level and highlights. Subtitles are "
            "detected only with a subtitle style that moves them (Black or Generated gray); otherwise nothing is "
            "left out. VP's own on-screen panels are never measured. Off: subtitles inside the picture count, "
            "as before."));
        driveForm->addRow(QString(), pictureOnly);
        // Owner 2026-10-15: the script copy in the original script's own
        // rhythm (the default) or a fixed loop.
        auto* pacing = bindChoiceField(led, QStringLiteral("script_pacing"),
            { QStringLiteral("script"), QStringLiteral("tick") },
            { QStringLiteral("The script's own rhythm"), QStringLiteral("Fixed loop") });
        pacing->setToolTip(QStringLiteral(
            "How often VP's copy of the script decides. The script's own rhythm (the default): its measuring "
            "time, then its commands with their pauses, then the next loop at once - the same commands at the "
            "same moments as your original script. Fixed loop: every 400 ms."));
        driveForm->addRow(QStringLiteral("Script pacing"), pacing);
        pacingRows.push_back(pacing);
        // Owner 2026-10-17: script_lookahead - the script copy looks ahead too.
        auto* scriptAhead = bindCheckField(QStringLiteral("Look-ahead for the script"), led,
            QStringLiteral("script_lookahead"), true);
        scriptAhead->setToolTip(QStringLiteral(
            "On (default): VP's copy of the script decides from the frame that will be on screen when its command "
            "reaches the projector, using the frames already queued (the queue profile sets how many; Low Latency "
            "queues none; a shorter queue uses its last frame). The script's own bands, steps, contrast logic and "
            "rhythm are kept. A rise is aimed so the light has settled by its frame, a fall lands on its frame. A cut "
            "to a brighter scene gets that scene's light and contrast landing on its first frame; out of black the light may come "
            "up early, no higher than the pre-lift cap, while the picture is still black. When the queue covers the "
            "projector's response (LED latency plus settle) the lowest LED level is 'Lowest LED level', otherwise "
            "'Lowest LED level without look-ahead'. Off: decisions on the frame on screen, as your original script."));
        driveForm->addRow(QString(), scriptAhead);
        pacingRows.push_back(scriptAhead);
        // Restore switch (bug hunt 2026-10-17): script_resend_failed_writes.
        auto* resend = bindCheckField(QStringLiteral("Re-send a failed LED / contrast write (fix under test)"), led,
            QStringLiteral("script_resend_failed_writes"), true);
        resend->setToolTip(QStringLiteral(
            "Script mode. On (default): an LPW1 or CNTR whose write failed (a port or write error; a missing "
            "reply is not a failure) is sent again at the next decision with the script's current value (only "
            "when that decision sends nothing itself, so the script's own commands are unchanged). Off: the pure "
            "script stream - a failed write is not repeated."));
        driveForm->addRow(QString(), resend);
        auto pacingSpin = [this, led, driveForm](const char* key, int maximum, int fallback, const QString& label,
            const QString& tip)
        {
            auto* spin = new QSpinBox;
            spin->setObjectName(controlName(led, QString::fromLatin1(key)));
            spin->setRange(0, maximum);
            spin->setSingleStep(10);
            spin->setSuffix(QStringLiteral(" ms"));
            spin->setMaximumWidth(120);
            spin->setAlignment(Qt::AlignRight);
            spin->setToolTip(tip);
            bool valid = false;
            const int configured = value(led, QString::fromLatin1(key)).toInt(&valid);
            spin->setValue(valid ? std::max(0, std::min(configured, maximum)) : fallback);
            flagClampedLedValue(spin, value(led, QString::fromLatin1(key)), 0, maximum);
            const std::string name = key;
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, led, name](int ms)
            {
                if (!document_) return;
                const std::string section = led.toStdString();
                if (!document_->SetKnown(section, name.c_str(), std::to_string(ms)))
                {
                    document_->AddSection(section);
                    document_->SetKnown(section, name.c_str(), std::to_string(ms));
                }
                markDirty();
            });
            driveForm->addRow(label, spin);
            return spin;
        };
        pacingRows.push_back(pacingSpin("script_measure_ms", 2000, 150, QStringLiteral("Script's measuring time"),
            QStringLiteral("How long your original script took to measure the picture (its screenshot): part of its rhythm. "
                "Default 150.")));
        pacingRows.push_back(pacingSpin("script_echo_ms", 1000, 30, QStringLiteral("Script's command time"),
            QStringLiteral("How long each of the script's commands took to send (its echo to the port). Default 30.")));
        // What every source change writes: the source, the matching mode for
        // older builds (no LED commands: picture only when adaptive gamma is
        // on), and adaptive gamma as shown, so nothing changes behind it.
        auto writeSource = [this, led, source, adaptive]()
        {
            if (!document_) return;
            const QString chosen = source->currentData().toString();
            const std::string section = led.toStdString();
            if (!document_->SetKnown(section, "command_source", chosen.toStdString()))
            {
                document_->AddSection(section);
                document_->SetKnown(section, "command_source", chosen.toStdString());
            }
            const char* mode = chosen == QStringLiteral("internal_script") ? "script" :
                chosen == QStringLiteral("compensate") ? "compensate" :
                adaptive->isChecked() ? "picture_only" : "log_only";
            document_->SetKnown(section, "mode", mode);
            document_->SetKnown(section, "adaptive_gamma", adaptive->isChecked() ? "true" : "false");
            markDirty();
        };
        // A pick writes even when it is the source already shown (an older
        // file is converted; "No LED commands" can be chosen on a new file).
        connect(source, qOverload<int>(&QComboBox::currentIndexChanged), this, [writeSource](int) { writeSource(); });
        connect(source, qOverload<int>(&QComboBox::activated), this, [writeSource](int) { writeSource(); });
        connect(adaptive, &QCheckBox::toggled, this, [this, led, source, writeSource](bool checked)
        {
            if (!document_) return;
            if (value(led, QStringLiteral("command_source")).trimmed().isEmpty())
            {
                // An older file without a source: the box alone.
                document_->SetKnown(led.toStdString(), "adaptive_gamma", checked ? "true" : "false");
                markDirty();
            }
            else
                writeSource();   // keeps the matching mode (picture only / log only)
            (void)source;
        });
    }
    // Compensate's own settings: shown only when it is the command source
    // (audit 2026-10-14b: show only what the chosen source uses).
    std::vector<QWidget*> compensateRows;
    // The floors: compensate's and, with script_lookahead (owner 2026-10-17), the script copy's.
    std::vector<QWidget*> floorRows;
    auto* floor = levelControl(QStringLiteral("led_floor"), 100);
    driveForm->addRow(QStringLiteral("Lowest LED level"), floor);
    floorRows.push_back(floor);
    floorRows.push_back(helpLabel(QStringLiteral(
        "How far the LED may dim in dark scenes, 60 (darkest) to 255 (never dims), when VP can see the "
        "coming frames far enough for the projector's response (Normal queue). The script copy's darkest band "
        "continues its curve down to it.")));
    driveForm->addRow(QString(), floorRows.back());
    auto* floorReactive = levelControl(QStringLiteral("led_floor_reactive"), 100);
    driveForm->addRow(QStringLiteral("Lowest LED level without look-ahead"), floorReactive);
    floorRows.push_back(floorReactive);
    floorRows.push_back(helpLabel(QStringLiteral(
        "Used when VP cannot see ahead (Low Latency queue): it cannot prepare for a bright scene, so it "
        "should not sit as low. Your script used 100.")));
    driveForm->addRow(QString(), floorRows.back());
    auto* floorBlack = levelControl(QStringLiteral("led_floor_black"), 60);
    driveForm->addRow(QStringLiteral("Lowest level in fades to black"), floorBlack);
    compensateRows.push_back(floorBlack);
    // A fade can dim further than a scene, never less far.
    floorBlack->setMaximum(floor->value());
    connect(floor, qOverload<int>(&QSpinBox::valueChanged), floorBlack,
        [floorBlack](int level) { floorBlack->setMaximum(level); });
    auto* highlightProtection = bindChoiceField(led,
        QStringLiteral("highlight_tolerance_stops"),
        { QStringLiteral("0.33"), QStringLiteral("0.15"), QStringLiteral("0.6") },
        { QStringLiteral("Medium"), QStringLiteral("Strong (keep highlights; the LED dims less)"),
          QStringLiteral("Light (the LED dims more; highlights compress more)") });
    driveForm->addRow(QStringLiteral("Highlight protection"), highlightProtection);
    compensateRows.push_back(highlightProtection);
    driveForm->addRow(QStringLiteral("Gamma boost"), bindChoiceField(led, QStringLiteral("gamma_boost"),
        { QStringLiteral("off"), QStringLiteral("2.4") },
        { QStringLiteral("Off (plain output gamma)"),
          QStringLiteral("2.4 (the look of the old output gamma 2.4)") }));
    // The projector's light exponent on the final code, which Compensate
    // linearises with (compensation_eotf). A number, because it is whatever a
    // light meter says (design review 3, SF-1); 2.41 is the FL35's light-meter
    // fit (delta 4). The gamma boost no longer depends on it.
    auto* projectorGamma = new QDoubleSpinBox;
    projectorGamma->setObjectName(controlName(led, QStringLiteral("compensation_eotf")));
    projectorGamma->setAccessibleName(QStringLiteral("Projector gamma"));
    projectorGamma->setRange(1.0, 3.0);
    projectorGamma->setDecimals(ledDisplayDecimals(value(led, QStringLiteral("compensation_eotf")), 2));
    projectorGamma->setSingleStep(0.01);
    projectorGamma->setMaximumWidth(120);
    projectorGamma->setAlignment(Qt::AlignRight);
    {
        bool valid = false;
        const double configured = value(led, QStringLiteral("compensation_eotf")).toDouble(&valid);
        // The spin box's own range (1.0-3.0, as the LED validation); was clamped to 1.8 (audit 2026-10-16).
        projectorGamma->setValue(valid ? std::max(1.0, std::min(configured, 3.0)) : 2.41);
        flagClampedLedValue(projectorGamma, value(led, QStringLiteral("compensation_eotf")), 1.0, 3.0);
    }
    connect(projectorGamma, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
        [this, led, projectorGamma](double exponent)
    {
        if (!document_) return;
        const std::string section = led.toStdString();
        const std::string text = QString::number(exponent, 'f', projectorGamma->decimals()).toStdString();
        if (!document_->SetKnown(section, "compensation_eotf", text))
        {
            document_->AddSection(section);
            document_->SetKnown(section, "compensation_eotf", text);
        }
        markDirty();
    });
    driveForm->addRow(QStringLiteral("Projector gamma"), projectorGamma);
    driveForm->addRow(QString(), helpLabel(QStringLiteral(
        "Projector gamma is how the projector turns the final signal into light; Compensate needs it to keep the "
        "picture's brightness exact while the LED dims. 2.41 is the FL35's light-meter measurement; change it "
        "only if a new measurement says otherwise. It does not change the gamma boost.\n"
        "The gamma boost applies even with Dynamic Drive off. Switching Dynamic Drive on or off returns the "
        "projector to full light; other changes carry on from the current level.")));

    // Experimental: deeper fade to black with the projector's custom LED dim.
    // Off by default; its options stay greyed out until it is switched on.
    auto* experiment = new QWidget;
    auto* experimentForm = new QFormLayout(experiment);
    experimentForm->setContentsMargins(0, 0, 0, 0);
    experimentForm->setVerticalSpacing(8);
    auto saveLed = [this, led](const QString& key, const QString& text)
    {
        if (!document_) return;
        const std::string section = led.toStdString();
        if (!document_->SetKnown(section, key.toStdString().c_str(), text.toLocal8Bit().constData()))
        {
            document_->AddSection(section);
            document_->SetKnown(section, key.toStdString().c_str(), text.toLocal8Bit().constData());
        }
        markDirty();
    };
    const QString configuredMethod = value(led, QStringLiteral("ftb_custom_dim")).trimmed().toLower();
    const bool experimentOn = configuredMethod == QStringLiteral("together") ||
        configuredMethod == QStringLiteral("turns");
    auto* experimentEnabled = new QCheckBox(QStringLiteral("Experimental: deeper fade to black (custom LED dim)"));
    experimentEnabled->setObjectName(QStringLiteral("config.led_drive.default.ftb_experimental"));
    experimentEnabled->setChecked(experimentOn);
    experimentForm->addRow(QString(), experimentEnabled);
    auto* method = new QComboBox;
    method->setObjectName(QStringLiteral("config.led_drive.default.ftb_custom_dim"));
    method->addItem(QStringLiteral("All three colours together"), QStringLiteral("together"));
    method->addItem(QStringLiteral("Colours taking turns in small steps"), QStringLiteral("turns"));
    method->setCurrentIndex(configuredMethod == QStringLiteral("turns") ? 1 : 0);
    experimentForm->addRow(QStringLiteral("Method"), method);
    auto numberSetting = [this, led, saveLed](const QString& key, int minimum, int maximum, int fallback,
        const QString& suffix)
    {
        auto* spin = new QSpinBox;
        spin->setObjectName(controlName(led, key));
        spin->setRange(minimum, maximum);
        spin->setMaximumWidth(120);
        spin->setAlignment(Qt::AlignRight);
        if (!suffix.isEmpty()) spin->setSuffix(suffix);
        bool valid = false;
        const int configured = value(led, key).toInt(&valid);
        spin->setValue(valid ? std::max(minimum, std::min(configured, maximum)) : fallback);
        flagClampedLedValue(spin, value(led, key), minimum, maximum);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
            [saveLed, key](int number) { saveLed(key, QString::number(number)); });
        return spin;
    };
    auto* stepMs = numberSetting(QStringLiteral("ftb_step_ms"), 50, 1000, 100, QStringLiteral(" ms"));
    experimentForm->addRow(QStringLiteral("Step interval"), stepMs);
    // How far each colour moves per command when taking turns: smaller keeps
    // the three colours closer (less colour shift) but takes longer.
    auto* turnStep = numberSetting(QStringLiteral("ftb_turn_step"), 1, 50, 2, QStringLiteral(" levels"));
    experimentForm->addRow(QStringLiteral("Step size per colour"), turnStep);
    // The FL35's custom LED dim runs 0-40 per colour (40 is about 40/255, below
    // LPW1's 60; 0 is off).
    auto* customStart = numberSetting(QStringLiteral("ftb_custom_start"), 1, 40, 40, QString());
    experimentForm->addRow(QStringLiteral("Starting custom level (1-40)"), customStart);
    auto* customFloor = numberSetting(QStringLiteral("ftb_custom_floor"), 0, 39, 0, QString());
    experimentForm->addRow(QStringLiteral("Deepest custom level (0 = off)"), customFloor);
    customFloor->setMaximum(customStart->value() - 1);
    connect(customStart, qOverload<int>(&QSpinBox::valueChanged), customFloor,
        [customFloor](int start) { customFloor->setMaximum(start - 1); });
    experimentForm->addRow(QString(), helpLabel(QStringLiteral(
        "When the picture has been black for half a second and the LED is at its "
        "fade-to-black level, VP switches the projector to its custom LED dim and steps the red, green and "
        "blue LEDs down, one command per step interval; at the first light it switches back. \"Taking turns\" "
        "keeps the three colours within one small step of each other, so the black does not change colour. "
        "The custom dim runs 0-40 (40 is about 40/255, a small step down from LPW1's 60; 0 turns the LEDs "
        "off, true black).")));
    // Fix under test (audit 2026-10-16), default on.
    auto* correctionByTime = bindCheckField(QStringLiteral("Fix under test: look-ahead correction smoothed by real time"),
        led, QStringLiteral("correction_weight_by_time"), true);
    correctionByTime->setToolTip(QStringLiteral(
        "VP compares each measured frame with what the look-ahead predicted for it and smooths the "
        "difference over about three seconds. On: each update counts for the real time since the last one "
        "(24p film on a 60 Hz output updates once per film frame, so it settles in about three seconds). "
        "Off: each update counts for one output frame, as before (about 2.5 times slower with 24p on 60 Hz)."));
    experimentForm->addRow(QString(), correctionByTime);
    // Audit 2026-10-16: full light after no video (every source), on the main LED card.
    auto* idleRestore = numberSetting(QStringLiteral("led_idle_restore_s"), 0, 600, 10, QStringLiteral(" s"));
    idleRestore->setToolTip(QStringLiteral(
        "After this many seconds without a video frame (pause, stop), VP puts the LED back to the LED ceiling "
        "and CNTR to 50 (and leaves the custom dim). VP's copy of your script stops acting on the last frame "
        "until video returns. 0 keeps the projector where it was, as before."));
    driveForm->addRow(QStringLiteral("Full light after no video (0 = off)"), idleRestore);
    auto syncExperiment = [experimentEnabled, method, stepMs, turnStep, customStart, customFloor]
    {
        const bool on = experimentEnabled->isChecked();
        for (QWidget* control : std::vector<QWidget*>{ method, stepMs, customStart, customFloor })
            control->setEnabled(on);
        // The step size only matters when the colours take turns.
        turnStep->setEnabled(on && method->currentData().toString() == QStringLiteral("turns"));
    };
    syncExperiment();
    connect(experimentEnabled, &QCheckBox::toggled, this, [syncExperiment, saveLed, method](bool on)
    {
        syncExperiment();
        saveLed(QStringLiteral("ftb_custom_dim"), on ? method->currentData().toString() : QStringLiteral("off"));
    });
    connect(method, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [experimentEnabled, saveLed, method, syncExperiment](int)
    {
        syncExperiment();
        if (experimentEnabled->isChecked())
            saveLed(QStringLiteral("ftb_custom_dim"), method->currentData().toString());
    });

    auto* profile = new QWidget;
    auto* profileLayout = new QVBoxLayout(profile);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    profileLayout->setSpacing(8);
    auto* profileRow = new QHBoxLayout;
    auto* profilePath = bindTextField(led, QStringLiteral("profile_file"));
    profilePath->setPlaceholderText(QStringLiteral("Built-in (your dimming script's values)"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"));
    browse->setObjectName(QStringLiteral("config.led_drive.default.profile_browse"));
    auto* builtIn = new QPushButton(QStringLiteral("Use built-in"));
    builtIn->setObjectName(QStringLiteral("config.led_drive.default.profile_builtin"));
    profileRow->addWidget(profilePath, 1);
    profileRow->addWidget(browse);
    profileRow->addWidget(builtIn);
    profileLayout->addLayout(profileRow);
    profileLayout->addWidget(helpLabel(QStringLiteral(
        "Optional. A profile file can change the light curve, step tables, ramp speeds "
        "and the other LED details; anything it leaves out uses the built-in value, and the settings on this page "
        "always win. Projector settings (connection, LED response time, flux table) are not read from a profile. "
        "See docs\\cinema\\LED-PROFILE-GUIDE.md.")));
    connect(browse, &QPushButton::clicked, this, [this, profilePath]
    {
        const QString start = QFileInfo(kit_.configPath()).absolutePath();
        const QString chosen = QFileDialog::getOpenFileName(kit_.window(), QStringLiteral("LED profile"),
            start, QStringLiteral("LED profiles (*.cfg *.txt);;All files (*)"));
        if (chosen.isEmpty()) return;
        // Next to the configuration: store just the name, so the folder can move.
        const QString relative = QDir(start).relativeFilePath(chosen);
        profilePath->setText(relative.startsWith(QStringLiteral("..")) ? chosen : relative);
    });
    connect(builtIn, &QPushButton::clicked, profilePath, [profilePath] { profilePath->clear(); });
    // Audit round 2: a broken profile no longer stops VP (it is left out),
    // so say here when VP would leave it out.
    auto* profileStatus = helpLabel(QString());
    profileStatus->setObjectName(QStringLiteral("config.led_drive.default.profile_status"));
    profileLayout->addWidget(profileStatus);
    const auto checkProfile = [this, profileStatus](const QString& text)
    {
        const QString name = text.trimmed();
        if (name.isEmpty() || kit_.remoteTarget().remote)
        {
            profileStatus->setVisible(false);
            return;
        }
        const QString full = QFileInfo(name).isAbsolute() ? name :
            QDir(QFileInfo(kit_.configPath()).absolutePath()).filePath(name);
        std::string problem;
        const bool ok = LedDrive::ValidateLedProfileFile(
            QDir::toNativeSeparators(full).toLocal8Bit().toStdString(), problem);
        profileStatus->setText(ok ? QStringLiteral("Profile file OK.") :
            QStringLiteral("VP will ignore this profile and use the built-in values: %1")
                .arg(QString::fromLocal8Bit(problem.c_str())));
        profileStatus->setVisible(true);
    };
    connect(profilePath, &QLineEdit::textChanged, this, checkProfile);
    checkProfile(profilePath->text());
    browse->setEnabled(!kit_.remoteTarget().remote);
    if (kit_.remoteTarget().remote)
        browse->setToolTip(QStringLiteral("The profile file must be on the VP computer; type its path."));


    // The projector's connection card (drive.projector_control).
    const ProjectorCards::Connection projectorConnection = projectorCards_.BuildConnection();
    QWidget* connection = projectorConnection.content;
    QComboBox* protocolChoice = projectorConnection.protocolChoice;


    // Stage 7 step 7c (control policy; the owner's correction): compensate's
    // contrast assist, cuts, speeds and the projector's response. Every
    // setting has a control; a speed set to "preset" is removed from the
    // file so rate_preset applies.
    // Delta 11: the boost's toe and the scene-adaptive shadow lift.
    auto* boostKnee = numberControl(led, QStringLiteral("gamma_boost_knee"), 0.0, 0.5, 2, 0.01, 0.0, QString());
    boostKnee->setToolTip(QStringLiteral(
        "Keep the darkest tones out of the gamma boost: below this part of the signal (0.10 = the bottom tenth) "
        "the picture keeps its native contrast, so dark hair and shadows are not lifted flat. 0 = the boost "
        "everywhere (as before)."));
    driveForm->addRow(QStringLiteral("Boost spares the darks below"), boostKnee);
    auto* boostKneeWidth = numberControl(led, QStringLiteral("gamma_boost_knee_width"), 0.0, 0.5, 2, 0.01, 0.05, QString());
    boostKneeWidth->setToolTip(QStringLiteral(
        "How gradually the boost comes in above that point (part of the signal; 0 = a hard switch). "
        "Only matters when the setting above is not 0."));
    driveForm->addRow(QStringLiteral("Boost blend width"), boostKneeWidth);
    if (auto* boostChoice = drive->findChild<QComboBox*>(controlName(led, QStringLiteral("gamma_boost"))))
    {
        // The toe rows do nothing without the boost (audit 2026-10-14c).
        auto greyBoost = [boostChoice, boostKnee, boostKneeWidth]()
        {
            const QString chosen = boostChoice->currentData().toString();
            const bool on = !chosen.isEmpty() && chosen != QStringLiteral("off");
            boostKnee->setEnabled(on);
            boostKneeWidth->setEnabled(on);
        };
        connect(boostChoice, qOverload<int>(&QComboBox::currentIndexChanged), this, [greyBoost](int) { greyBoost(); });
        greyBoost();
    }
    auto* shadowLift = numberControl(led, QStringLiteral("shadow_lift_ansi"), 0.0, 1.0, 2, 0.05, 0.0, QString());
    shadowLift->setToolTip(QStringLiteral(
        "When something bright shares a dark frame, the lens scatters a veil of light over the darks. This lifts "
        "the darks in proportion to that veil so their texture stays visible; a dark frame with nothing bright "
        "is left alone. 0 = off; 0.3 is a moderate lift. Needs adaptive gamma."));
    driveForm->addRow(QStringLiteral("Shadow lift next to highlights"), shadowLift);
    auto* liftRampLo = numberControl(led, QStringLiteral("shadow_lift_ansi_ramp_lo"), 0.0, 1.0, 3, 0.005, 0.01, QString());
    liftRampLo->setToolTip(QStringLiteral(
        "How much highlight it takes before the lift starts: brightness x share of the picture, so 0.01 is a "
        "full-white area covering 1% of the frame. The lift is full at the next value."));
    driveForm->addRow(QStringLiteral("Lift starts at highlight amount"), liftRampLo);
    auto* liftRampHi = numberControl(led, QStringLiteral("shadow_lift_ansi_ramp_hi"), 0.0, 1.0, 3, 0.005, 0.10, QString());
    liftRampHi->setToolTip(QStringLiteral(
        "The highlight amount at which the lift reaches its full strength. Keep it above the start value."));
    driveForm->addRow(QStringLiteral("Lift full at highlight amount"), liftRampHi);
    auto* liftRate = numberControl(led, QStringLiteral("shadow_lift_ansi_rate"), 0.1, 10.0, 2, 0.1, 1.0, QString());
    liftRate->setToolTip(QStringLiteral(
        "How fast the lift follows a change in the highlights (full range per second; 1 = about a second). "
        "A cut always takes the new value at once. Lower if a flash makes the darks pump."));
    driveForm->addRow(QStringLiteral("Lift follows within (per second)"), liftRate);
    const std::vector<QWidget*> shadowLiftRows = { shadowLift, liftRampLo, liftRampHi, liftRate };
    auto* assist = new QWidget;
    auto* assistForm = new QFormLayout(assist);
    assistForm->setContentsMargins(0, 0, 0, 0);
    assistForm->setVerticalSpacing(8);
    assistForm->addRow(QString(), bindCheckField(QStringLiteral("Use the projector's contrast to help"), led,
        QStringLiteral("cntr_assist"), true));
    assistForm->addRow(QString(), bindCheckField(QStringLiteral("Software first"), led,
        QStringLiteral("software_first"), true));
    assistForm->addRow(QString(), bindCheckField(QStringLiteral("Ignore film grain in the highlight measure"), led,
        QStringLiteral("led_highlight_grain_suppress"), true));
    assistForm->addRow(QStringLiteral("Highest contrast for the assist"),
        numberControl(led, QStringLiteral("cntr_assist_max"), 50, 100, 0, 1, 100, QString()));
    assistForm->addRow(QString(), helpLabel(QStringLiteral(
        "Compensate only. The contrast adds the light VP's gain cannot add without crushing highlights, so the "
        "LED can dim further in scenes with highlights. Software first: VP's gain does what it can before the "
        "contrast moves. 100 = no limit beyond the projector's usable contrast.")));
    assistForm->addRow(QString(), bindCheckField(QStringLiteral("On a cut to a brighter scene, set the contrast to the cut level"),
        led, QStringLiteral("cut_cntr_dump"), true));
    assistForm->addRow(QStringLiteral("Contrast on a bright cut"),
        numberControl(led, QStringLiteral("cut_cntr_level"), 50, 100, 0, 1, 59, QStringLiteral("auto (white clip)")));
    assistForm->addRow(QString(), bindCheckField(QStringLiteral("Smooth fade-up after a bright cut"), led,
        QStringLiteral("shaped_recovery"), true));
    assistForm->addRow(QStringLiteral("Darkest start of a fade-up"),
        numberControl(led, QStringLiteral("max_cut_dip"), 0.0, 1.0, 2, 0.05, 0.5, QString()));
    assistForm->addRow(QStringLiteral("Contrast kept bright-side (%)"),
        numberControl(led, QStringLiteral("recovery_bias_pct"), 0.0, 20.0, 1, 0.5, 3.0, QString()));
    auto* prelift = levelControl(QStringLiteral("black_prelift_max"), 120);
    assistForm->addRow(QStringLiteral("LED lift allowed while still black"), prelift);
    // Review 2026-10-17 (docs/audit/VP-led-compensate-review-2026-10-17.md).
    auto* blackOnly = bindCheckField(QStringLiteral("Pre-position only on true black (fix under test)"), led,
        QStringLiteral("preposition_black_only"), true);
    blackOnly->setToolTip(QStringLiteral(
        "On (default): the contrast hold and the LED pre-lift when coming out of black apply only while the frame "
        "the LED lands on is truly black, so a visible fade-in never carries the cut contrast. Off: also on very "
        "dark frames, as before."));
    assistForm->addRow(QString(), blackOnly);
    auto* landingMs = bindCheckField(QStringLiteral("Time each command from when it was written (fix under test)"), led,
        QStringLiteral("led_landing_ms"), true);
    landingMs->setToolTip(QStringLiteral(
        "On (default): each LED / contrast command lands its response time after the moment the port wrote it, on "
        "the nearest frame. Off: it is credited to the frame during which it was written and lands whole frames "
        "later, as before."));
    assistForm->addRow(QString(), landingMs);
    auto* cutLead = bindChoiceField(led, QStringLiteral("cut_lead"),
        { QStringLiteral("off"), QStringLiteral("window") },
        { QStringLiteral("Off (default)"), QStringLiteral("As soon as the cut is seen (experimental)") });
    cutLead->setToolTip(QStringLiteral(
        "Experimental. As soon as the cut is seen: a bright cut found in the look-ahead gets its LED jump when it "
        "enters the look-ahead window; until the cut the LED holds and the contrast does not rise. Off: the jump "
        "goes out at the command latency ahead of the cut, as before."));
    assistForm->addRow(QStringLiteral("Bright cut lead (experimental)"), cutLead);
    auto* lookaheadMs = numberControl(led, QStringLiteral("led_lookahead_ms"), 0, 2000, 0, 10, 0,
        QStringLiteral("queue depth"));
    lookaheadMs->setToolTip(QStringLiteral(
        "An optional cap on how far ahead the LED looks, in milliseconds (converted with the measured source rate). "
        "0 (default): every frame the queue profile has queued (Normal, Reliable: their queue; Low Latency: none, "
        "so the LED reacts to the frame on screen). Above 0: at most that far, never past what is queued. "
        "Independent of the active-picture look-ahead."));
    assistForm->addRow(QStringLiteral("LED look-ahead (ms)"), lookaheadMs);
    // Restore switch (bug hunt 2026-10-17): led_wtw_passthrough.
    auto* wtw = bindCheckField(QStringLiteral("Above-white keeps its value under a boost (fix under test)"), led,
        QStringLiteral("led_wtw_passthrough"), true);
    wtw->setToolTip(QStringLiteral(
        "On (default): when VP's gain brightens the picture, whiter-than-white detail keeps its own value instead "
        "of being rolled under white with the boosted picture (owner rule: keep BTB and WTW). Off: as before."));
    assistForm->addRow(QString(), wtw);
    // Restore switch (bug hunt 2 2026-10-17): led_btb_passthrough.
    auto* btb = bindCheckField(QStringLiteral("Below-black passes through unchanged (fix under test)"), led,
        QStringLiteral("led_btb_passthrough"), true);
    btb->setToolTip(QStringLiteral(
        "On (default): blacker-than-black detail leaves VP's compensation exactly as captured - the gain and the "
        "gamma boost act only on the picture above black, so below-black codes are never pushed to the floor and "
        "clipped when the LED dims (owner rule: keep BTB and WTW). Off: below black is boosted and gained with the "
        "picture, as before."));
    assistForm->addRow(QString(), btb);
    // Post-merge audit 2026-10-17 (owner rule: CNTR above need is a defect): cntr_pair_predict.
    auto* pairPredict = bindCheckField(QStringLiteral("Contrast paired with the LED where it lands (fix under test)"), led,
        QStringLiteral("cntr_pair_predict"), true);
    pairPredict->setToolTip(QStringLiteral(
        "On (default): each contrast step is chosen for the LED as it will be when the step lands (contrast "
        "response time ahead: the LED commands in flight and its settling), so contrast is never left above "
        "what is needed when it arrives - for example while the LED rises out of a dark scene. During the "
        "fade-up after a bright cut contrast only goes down, counting only the picture gain the fade-up allows. "
        "Off: paired with the LED as it is now (the earlier behaviour)."));
    assistForm->addRow(QString(), pairPredict);
    assistForm->addRow(QString(), helpLabel(QStringLiteral(
        "On a cut to a brighter scene the contrast goes to the cut level (auto = where white starts to clip, 59 "
        "until measured; never 50), VP's gain to its maximum and the knee rolls off highlights; then the picture "
        "fades up with the LED. The darkest start (0.5 = half brightness) also sets the LED floor in every scene. "
        "Coming out of black with the look-ahead, the contrast is set and the LED may lift to the level above "
        "while the screen is still black (60 = keep the black absolutely; 255 = the old behaviour).")));

    auto* speeds = new QWidget;
    auto* speedsForm = new QFormLayout(speeds);
    speedsForm->setContentsMargins(0, 0, 0, 0);
    speedsForm->setVerticalSpacing(8);
    speedsForm->addRow(QStringLiteral("LED speeds"), bindChoiceField(led, QStringLiteral("rate_preset"),
        { QStringLiteral("script"), QStringLiteral("compensated") },
        { QStringLiteral("Script (your beta 22 speeds)"), QStringLiteral("Compensated (faster)") }));
    const QString preset = QStringLiteral("preset");
    speedsForm->addRow(QStringLiteral("Brightening: gain / min / max"), [&]
    {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(numberControl(led, QStringLiteral("rate_gain_up"), 0.0, 100.0, 2, 0.05, 0.25, preset));
        layout->addWidget(numberControl(led, QStringLiteral("rate_min_up"), 0.0, 1000.0, 1, 0.5, 2.5, preset));
        layout->addWidget(numberControl(led, QStringLiteral("rate_max_up"), 1.0, 1000.0, 1, 0.5, 17.5, preset));
        return row;
    }());
    speedsForm->addRow(QStringLiteral("Dimming: gain / min / max"), [&]
    {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(numberControl(led, QStringLiteral("rate_gain_down"), 0.0, 100.0, 2, 0.05, 0.15, preset));
        layout->addWidget(numberControl(led, QStringLiteral("rate_min_down"), 0.0, 1000.0, 1, 0.5, 2.5, preset));
        layout->addWidget(numberControl(led, QStringLiteral("rate_max_down"), 1.0, 1000.0, 1, 0.5, 12.5, preset));
        return row;
    }());
    speedsForm->addRow(QStringLiteral("Fade-up after a cut: gain / min / max"), [&]
    {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(numberControl(led, QStringLiteral("recovery_rate_gain"), 0.0, 100.0, 2, 0.05, 4.0, preset));
        layout->addWidget(numberControl(led, QStringLiteral("recovery_rate_min"), 0.0, 1000.0, 1, 0.5, 20.0, preset));
        layout->addWidget(numberControl(led, QStringLiteral("recovery_rate_max"), 1.0, 1000.0, 1, 0.5, 400.0, preset));
        return row;
    }());
    speedsForm->addRow(QString(), helpLabel(QStringLiteral(
        "Levels per second: gain x distance, kept between min and max, so the LED is fast while far from its "
        "target and eases in. The LED within a scene, and the LED and contrast moving together after a cut, "
        "use the brightening / dimming speeds; only the fade-up for what they cannot yet supply uses the "
        "fade-up speeds. \"preset\" = the LED speeds choice above.")));

    // The projector's response card (drive.projector_control).
    const ProjectorCards::Response projectorResponse = projectorCards_.BuildResponse();
    QWidget* response = projectorResponse.content;
    QFormLayout* responseForm = projectorResponse.form;
    QDoubleSpinBox* maxUsable = projectorResponse.maxUsable;
    QDoubleSpinBox* whiteClips = projectorResponse.whiteClips;

    // The projector's advanced serial settings (drive.projector_control).
    QWidget* advancedProjector = projectorCards_.BuildAdvanced();

    // Audit 2026-10-17: every LED setting VP reads has a control. The rest of
    // [led_drive] (LedDriveConfig.h ApplyLedDriveSection), with the parser's
    // ranges and defaults (LedDriveRules, LedDriveConfig); collapsed, expert.
    auto* advancedLed = new QWidget;
    auto* advancedLedForm = new QFormLayout(advancedLed);
    advancedLedForm->setContentsMargins(0, 0, 0, 0);
    advancedLedForm->setVerticalSpacing(8);
    advancedLedForm->addRow(QString(), helpLabel(QStringLiteral(
        "The curve and the step tables are your dimming script's tables: your script is the reference, and the "
        "built-in values are its values. Leave a field at its default unless the script changed. Each Reset "
        "removes the setting, so the built-in value (or an LED profile file's) applies.")));
    auto ledResetButton = [](const QString& name)
    {
        auto* reset = new QToolButton;
        reset->setObjectName(name + QStringLiteral(".reset"));
        reset->setText(QStringLiteral("Reset"));
        reset->setToolTip(QStringLiteral("Remove this setting: the built-in default applies."));
        return reset;
    };
    auto advancedNumber = [this, led, advancedLedForm, ledResetButton](const char* keyText, double minimum,
        double maximum, int decimals, double step, double fallback, const QString& suffix, const QString& label,
        const QString& tip)
    {
        const QString key = QString::fromLatin1(keyText);
        auto* spin = new QDoubleSpinBox;
        spin->setObjectName(controlName(led, key));
        spin->setAccessibleName(label);
        const QString raw = value(led, key).trimmed();
        spin->setDecimals(ledDisplayDecimals(raw, decimals));
        spin->setSingleStep(step);
        spin->setRange(minimum, maximum);
        if (!suffix.isEmpty()) spin->setSuffix(suffix);
        spin->setMaximumWidth(130);
        spin->setAlignment(Qt::AlignRight);
        bool valid = false;
        const double configured = raw.toDouble(&valid);
        spin->setValue(valid ? std::max(minimum, std::min(configured, maximum)) : fallback);
        spin->setToolTip(tip + QStringLiteral("\nRange %1 to %2; default %3.")
            .arg(minimum).arg(maximum).arg(fallback));
        flagClampedLedValue(spin, raw, minimum, maximum);
        auto* reset = ledResetButton(spin->objectName());
        reset->setEnabled(!raw.isEmpty());
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, led, key, spin, reset](double number)
        {
            if (!document_) return;
            const std::string section = led.toStdString();
            const std::string text = QString::number(number, 'f', spin->decimals()).toStdString();
            if (!document_->SetKnown(section, key.toStdString().c_str(), text))
            {
                document_->AddSection(section);
                document_->SetKnown(section, key.toStdString().c_str(), text);
            }
            reset->setEnabled(true);
            markDirty();
        });
        connect(reset, &QToolButton::clicked, this, [this, led, key, spin, reset, fallback]()
        {
            if (!document_) return;
            document_->RemoveKnown(led.toStdString(), key.toStdString().c_str());
            {
                const QSignalBlocker blocker(spin);
                spin->setValue(fallback);
            }
            reset->setEnabled(false);
            markDirty();
        });
        auto* row = new QWidget;
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(spin);
        rowLayout->addWidget(reset);
        rowLayout->addStretch(1);
        advancedLedForm->addRow(label, row);
        return spin;
    };
    auto advancedSwitch = [this, led, advancedLedForm, ledResetButton](const char* keyText, bool fallback,
        const QString& label, const QString& tip)
    {
        const QString key = QString::fromLatin1(keyText);
        auto* check = configuredCheck(label, value(led, key), fallback);
        check->setObjectName(controlName(led, key));
        check->setEnabled(true);
        check->setToolTip(tip + (fallback ? QStringLiteral("\nDefault on.") : QStringLiteral("\nDefault off.")));
        auto* reset = ledResetButton(check->objectName());
        reset->setEnabled(!value(led, key).trimmed().isEmpty());
        connect(check, &QCheckBox::toggled, this, [this, led, key, reset](bool checked)
        {
            if (!document_) return;
            const std::string section = led.toStdString();
            if (!document_->SetKnown(section, key.toStdString().c_str(), checked ? "true" : "false"))
            {
                document_->AddSection(section);
                document_->SetKnown(section, key.toStdString().c_str(), checked ? "true" : "false");
            }
            reset->setEnabled(true);
            markDirty();
        });
        connect(reset, &QToolButton::clicked, this, [this, led, key, check, reset, fallback]()
        {
            if (!document_) return;
            document_->RemoveKnown(led.toStdString(), key.toStdString().c_str());
            {
                const QSignalBlocker blocker(check);
                check->setChecked(fallback);
            }
            reset->setEnabled(false);
            markDirty();
        });
        auto* row = new QWidget;
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(check);
        rowLayout->addWidget(reset);
        rowLayout->addStretch(1);
        advancedLedForm->addRow(QString(), row);
        return check;
    };
    // Lists and tables in the parser's own syntax, checked with its parser.
    auto advancedTable = [this, led, advancedLedForm, ledResetButton](const char* keyText,
        const QString& fallback, std::function<bool(const std::string&)> parse, const QString& label,
        const QString& syntax)
    {
        const QString key = QString::fromLatin1(keyText);
        auto* edit = new QLineEdit;
        edit->setObjectName(controlName(led, key));
        edit->setAccessibleName(label);
        edit->setPlaceholderText(fallback);
        edit->setToolTip(syntax + QStringLiteral(".\nEmpty = the built-in value: ") + fallback);
        const QString raw = value(led, key).trimmed();
        edit->setText(raw);
        auto* problem = new QLabel;
        problem->setObjectName(controlName(led, key) + QStringLiteral(".problem"));
        problem->setWordWrap(true);
        problem->setStyleSheet(QStringLiteral("color: #e57373;"));
        auto check = [edit, problem, parse, syntax]()
        {
            const std::string text = edit->text().trimmed().toStdString();
            const bool ok = text.empty() || parse(text);
            problem->setText(ok ? QString() :
                QStringLiteral("Not valid (VP refuses the configuration until it is fixed): ") + syntax + QStringLiteral("."));
            problem->setVisible(!ok);
            edit->setProperty("invalidValue", !ok);
        };
        check();
        auto* reset = ledResetButton(edit->objectName());
        reset->setEnabled(!raw.isEmpty());
        connect(edit, &QLineEdit::textEdited, this, [this, led, key, reset, check](const QString& text)
        {
            if (!document_) return;
            const std::string section = led.toStdString();
            const QString trimmed = text.trimmed();
            if (trimmed.isEmpty())
                document_->RemoveKnown(section, key.toStdString().c_str());
            else if (!document_->SetKnown(section, key.toStdString().c_str(), trimmed.toLocal8Bit().constData()))
            {
                document_->AddSection(section);
                document_->SetKnown(section, key.toStdString().c_str(), trimmed.toLocal8Bit().constData());
            }
            reset->setEnabled(!trimmed.isEmpty());
            check();
            markDirty();
        });
        connect(reset, &QToolButton::clicked, this, [this, led, key, edit, reset, check]()
        {
            if (!document_) return;
            document_->RemoveKnown(led.toStdString(), key.toStdString().c_str());
            {
                const QSignalBlocker blocker(edit);
                edit->clear();
            }
            reset->setEnabled(false);
            check();
            markDirty();
        });
        auto* row = new QWidget;
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(edit, 1);
        rowLayout->addWidget(reset);
        advancedLedForm->addRow(label, row);
        advancedLedForm->addRow(QString(), problem);
        return edit;
    };
    const auto stepTable = [](const std::string& text) { LedDrive::StepTable table; return LedDrive::ParseStepTable(text, table); };
    const QString stepSyntax = QStringLiteral(
        "Descending distance:step pairs ending at 0, each step 1 or more (the script's table)");
    advancedNumber("tick_ms", 50, 5000, 0, 10, 400, QStringLiteral(" ms"), QStringLiteral("Fixed loop interval"),
        QStringLiteral("How often VP's copy of the script decides when Script pacing is Fixed loop."));
    advancedNumber("deadband", 0, 50, 0, 1, 3, QStringLiteral(" levels"), QStringLiteral("Dead band"),
        QStringLiteral("Compensate: no LED movement while the target is within this many levels."));
    advancedNumber("led_ceiling", 1, 255, 0, 1, 255, QString(), QStringLiteral("Highest LED level"),
        QStringLiteral("The brightest LED level VP sends."));
    advancedTable("curve", QStringLiteral("0:100, 20:100, 40:200, 70:230, 100:255, 255:255"),
        [](const std::string& text) { std::vector<LedDrive::CurvePoint> curve; return LedDrive::ParseCurve(text, curve); },
        QStringLiteral("Light curve (APL:level)"),
        QStringLiteral("Ascending APL:level pairs (0-255), at least two, e.g. 0:100, 40:200, 100:255 (the script's curve)"));
    advancedNumber("knee", 0.0, 0.5, 2, 0.01, 0.08, QString(), QStringLiteral("Highlight knee"),
        QStringLiteral("Compensate: where VP's gain starts rolling off highlights (part of the signal)."));
    advancedNumber("anti_pump_band", 0, 50, 0, 1, 3, QStringLiteral(" levels"), QStringLiteral("Anti-pump band"),
        QStringLiteral("Small target changes inside this band do not reverse a ramp."));
    advancedTable("steps_up_dark", QStringLiteral("100:7, 50:5, 25:4, 10:1, 0:1"), stepTable,
        QStringLiteral("Brightening steps (dark scenes)"), stepSyntax);
    advancedTable("steps_up_bright", QStringLiteral("100:7, 50:4, 25:2, 10:1, 0:1"), stepTable,
        QStringLiteral("Brightening steps (bright scenes)"), stepSyntax);
    advancedNumber("steps_up_split_apl", 0, 255, 0, 1, 40, QString(), QStringLiteral("Dark / bright split (APL)"),
        QStringLiteral("The APL that chooses between the dark and bright brightening tables."));
    advancedTable("steps_down", QStringLiteral("100:5, 50:3, 25:2, 10:1, 0:1"), stepTable,
        QStringLiteral("Dimming steps"), stepSyntax);
    advancedTable("apl_weights", QStringLiteral("0.114, 0.587, 0.299"),
        [](const std::string& text) { double weights[3]; return LedDrive::ParseWeights(text, weights); },
        QStringLiteral("APL weights (R, G, B)"),
        QStringLiteral("Three non-negative R, G, B weights summing to 1 (the script's effective weights)"));
    advancedNumber("highlight_percentile", 50.0, 100.0, 1, 0.1, 99.5, QStringLiteral(" %"),
        QStringLiteral("Highlight percentile"), QStringLiteral("The brightness percentile measured as the frame's highlight."));
    advancedNumber("highlight_persist_ms", 0, 10000, 0, 10, 250, QStringLiteral(" ms"), QStringLiteral("Highlight hold"),
        QStringLiteral("How long a highlight must last before it counts (flash rejection)."));
    advancedNumber("apl_persist_ms", 0, 10000, 0, 10, 100, QStringLiteral(" ms"), QStringLiteral("APL hold"),
        QStringLiteral("Compensate: the window for the sustained APL the LED aims at; a cut uses the new scene's APL at once."));
    advancedNumber("highlight_area_small", 0.0, 1.0, 3, 0.001, 0.005, QString(), QStringLiteral("Small highlight area"),
        QStringLiteral("Highlights covering less of the frame than this are small (keep it below the large area)."));
    advancedNumber("highlight_area_large", 0.0, 1.0, 3, 0.001, 0.05, QString(), QStringLiteral("Large highlight area"),
        QStringLiteral("Highlights covering more of the frame than this are large."));
    advancedNumber("highlight_release_ms", 0, 60000, 0, 100, 2000, QStringLiteral(" ms"), QStringLiteral("Highlight release"),
        QStringLiteral("How long the highlight protection stays after a highlight leaves."));
    advancedSwitch("early_ramp", true, QStringLiteral("Start a ramp ahead of a cut the look-ahead has seen"),
        QStringLiteral("Compensate with look-ahead."));
    advancedSwitch("cut_jump", true, QStringLiteral("Jump the LED on a cut"),
        QStringLiteral("On a scene cut the LED goes straight to the new scene's level instead of ramping."));
    advancedSwitch("bright_cut_dump", true, QStringLiteral("Full light at once on a cut to a bright scene"),
        QStringLiteral("Compensate: a cut to a bright scene takes the LED up at once."));
    advancedNumber("cut_threshold", 1.0, 255.0, 1, 1.0, 24.0, QString(), QStringLiteral("Cut threshold (APL change)"),
        QStringLiteral("The APL change between frames that counts as a scene cut."));
    advancedNumber("fade_ms", 20, 5000, 0, 10, 250, QStringLiteral(" ms"), QStringLiteral("Fade detection window"),
        QStringLiteral("How long a steady change must last to count as a fade."));
    advancedNumber("fade_min_slope", 0.0, 1200.0, 1, 0.5, 12.0, QStringLiteral(" /s"), QStringLiteral("Fade minimum slope"),
        QStringLiteral("The slowest APL change per second that counts as a fade."));
    advancedNumber("ftb_black_ms", 100, 10000, 0, 50, 500, QStringLiteral(" ms"), QStringLiteral("Black before the custom dim"),
        QStringLiteral("Experimental fade to black: how long the picture is black before the custom LED dim starts."));
    advancedNumber("script_highlight_code", 16, 255, 0, 1, 215, QString(), QStringLiteral("Script highlight code"),
        QStringLiteral("VP's copy of the script: a frame whose maximum reaches this code has a highlight (the script used 215)."));
    auto* advancedLedHolder = new QWidget;
    {
        auto* holderLayout = new QVBoxLayout(advancedLedHolder);
        holderLayout->setContentsMargins(0, 0, 0, 0);
        auto* toggle = new QToolButton;
        toggle->setObjectName(controlName(led, QStringLiteral("advanced_toggle")));
        toggle->setText(QStringLiteral("Show the advanced LED settings"));
        toggle->setCheckable(true);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setArrowType(Qt::RightArrow);
        holderLayout->addWidget(toggle);
        holderLayout->addWidget(advancedLed);
        advancedLed->setVisible(false);
        connect(toggle, &QToolButton::toggled, this, [toggle, advancedLed](bool open)
        {
            advancedLed->setVisible(open);
            toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            toggle->setText(open ? QStringLiteral("Hide the advanced LED settings") :
                QStringLiteral("Show the advanced LED settings"));
        });
    }

    // In setup order (audit 2026-10-14c): what to choose, then the source's
    // own card, the connection, and the rest; Experimental last. Each card
    // shows only when the chosen source uses it.
    kit_.addCard(cards, createCard(QStringLiteral("Dynamic Drive"),
        QStringLiteral("Dim the projector's LED with the picture, as your dimming script does."), drive));
    QWidget* connectionBox = createCard(QStringLiteral("Projector connection"),
        QStringLiteral("The serial link to the projector."), connection);
    kit_.addCard(cards, connectionBox);
    QWidget* responseBox = createCard(QStringLiteral("Projector response"),
        QStringLiteral("Measured timings and contrast limits."), response);
    kit_.addCard(cards, responseBox);
    QWidget* profileBox = createCard(QStringLiteral("LED profile"),
        QStringLiteral("The detailed behaviour of VP's own sources: built-in, or your own file."), profile);
    kit_.addCard(cards, profileBox);
    QWidget* assistBox = createCard(QStringLiteral("Contrast and cuts"),
        QStringLiteral("Compensate: the projector's contrast, bright cuts and coming out of black."), assist);
    kit_.addCard(cards, assistBox);
    QWidget* speedsBox = createCard(QStringLiteral("LED speeds"),
        QStringLiteral("Compensate: how fast the LED and the fade-up move."), speeds);
    kit_.addCard(cards, speedsBox);
    QWidget* advancedHolder = projectorCards_.WrapAdvanced(advancedProjector);
    QWidget* advancedBox = createCard(QStringLiteral("Advanced projector"),
        QStringLiteral("Serial details for other projectors; the defaults match the FL35."), advancedHolder);
    kit_.addCard(cards, advancedBox);
    QWidget* advancedLedBox = createCard(QStringLiteral("Advanced LED"),
        QStringLiteral("The script's tables and the remaining LED details; the defaults are your script's."), advancedLedHolder);
    kit_.addCard(cards, advancedLedBox);
    QWidget* experimentBox = createCard(QStringLiteral("Experimental"),
        QStringLiteral("Compensate: deeper fade to black - test on the projector before relying on it."), experiment);
    kit_.addCard(cards, experimentBox);
    {
        // Show only what the chosen source uses; grey the shadow lift while
        // adaptive gamma is off and everything while the LED drive is off
        // (audits 2026-10-14b/c).
        QComboBox* source = ledSource;
        QCheckBox* adaptive = ledAdaptive;
        auto show = [driveForm, responseForm, compensateRows, floorRows, experimentBox, assistBox, speedsBox,
            profileBox, source, adaptive, compensateWarning, aheadRow, aheadHelp, maxUsable, whiteClips, pacingRows]()
        {
            const QString chosen = source ? source->currentData().toString() : QString();
            const bool compensate = chosen == QStringLiteral("compensate");
            const bool own = compensate || chosen == QStringLiteral("internal_script");
            for (QWidget* row : compensateRows)
                driveForm->setRowVisible(row, compensate);
            for (QWidget* row : floorRows)
                driveForm->setRowVisible(row, own);
            for (QWidget* box : { experimentBox, assistBox, speedsBox })
                box->setVisible(compensate);
            for (QWidget* row : { maxUsable, whiteClips })
                responseForm->setRowVisible(row, compensate);
            profileBox->setVisible(own);
            // Compensate's look-ahead switch; the script copy's is script_lookahead
            // (with the script pacing rows).
            driveForm->setRowVisible(aheadRow, compensate);
            driveForm->setRowVisible(aheadHelp, compensate);
            for (QWidget* row : pacingRows)
                driveForm->setRowVisible(row, chosen == QStringLiteral("internal_script"));
            driveForm->setRowVisible(compensateWarning, compensate && adaptive && !adaptive->isChecked());
        };
        auto grey = [shadowLiftRows, adaptive]()
        {
            for (QWidget* row : shadowLiftRows)
                row->setEnabled(!adaptive || adaptive->isChecked());
        };
        // A source that drives the projector needs the projector protocol.
        auto needPort = [source, protocolChoice]()
        {
            const QString chosen = source ? source->currentData().toString() : QString();
            if (chosen != QStringLiteral("none") && protocolChoice->currentData().toString() == QStringLiteral("none"))
                protocolChoice->setCurrentIndex(protocolChoice->findData(QStringLiteral("barco_ascii")));
        };
        auto onOff = [enabled, source, adaptive, connectionBox, responseBox, profileBox, assistBox,
            speedsBox, advancedBox, experimentBox, aheadRow, pacingRows]()
        {
            const bool on = enabled->isChecked();
            for (QWidget* row : pacingRows) row->setEnabled(on);
            for (QWidget* w : { static_cast<QWidget*>(source), static_cast<QWidget*>(adaptive), aheadRow,
                connectionBox, responseBox, profileBox, assistBox, speedsBox, advancedBox, experimentBox })
                if (w) w->setEnabled(on);
        };
        show();
        grey();
        onOff();
        connect(source, qOverload<int>(&QComboBox::currentIndexChanged), this, [show](int) { show(); });
        connect(source, qOverload<int>(&QComboBox::activated), this, [needPort](int) { needPort(); });
        connect(source, qOverload<int>(&QComboBox::currentIndexChanged), this, [needPort](int) { needPort(); });
        connect(adaptive, &QCheckBox::toggled, this, [grey, show](bool) { grey(); show(); });
        connect(enabled, &QCheckBox::toggled, this, [onOff](bool) { onOff(); });
        // The advanced LED settings belong to VP's own sources (audit 2026-10-17).
        auto advancedLedSync = [advancedLedBox, source, enabled]()
        {
            const QString chosen = source ? source->currentData().toString() : QString();
            advancedLedBox->setVisible(chosen == QStringLiteral("compensate") ||
                chosen == QStringLiteral("internal_script"));
            advancedLedBox->setEnabled(enabled->isChecked());
        };
        advancedLedSync();
        connect(source, qOverload<int>(&QComboBox::currentIndexChanged), this, [advancedLedSync](int) { advancedLedSync(); });
        connect(enabled, &QCheckBox::toggled, this, [advancedLedSync](bool) { advancedLedSync(); });
    }
    {
        // Saved values outside a control's range, said once at the top of the
        // page (audit 2026-10-17); each box is flagged too.
        QStringList clamped;
        for (QAbstractSpinBox* spin : cards->findChildren<QAbstractSpinBox*>())
            if (spin->property("clampedFrom").isValid())
                clamped.push_back(QStringLiteral("%1 = %2").arg(
                    spin->objectName().section(u'.', -1), spin->property("clampedFrom").toString()));
        if (!clamped.isEmpty())
        {
            auto* note = helpLabel(QStringLiteral("Saved values outside their control's range (shown clamped, "
                "kept until changed): %1.").arg(clamped.join(QStringLiteral(", "))));
            note->setObjectName(QStringLiteral("config.led_drive.default.clamped_note"));
            note->setStyleSheet(QStringLiteral("color: #e0a040;"));
            driveForm->insertRow(0, QString(), note);
        }
    }
    return kit_.createPage(QStringLiteral("Dynamic Drive"),
        QStringLiteral("Projector LED dimming and the gamma boost."), cards);
}

}
}

struct LedDrivePage::State
{
    std::unique_ptr<LedDriveBuilder> builder;
};

LedDrivePage::LedDrivePage() = default;
LedDrivePage::~LedDrivePage() = default;

void LedDrivePage::AppendPages(ConfigPageHost::ConfigFormApi& api, std::vector<ConfigPageHost::ConfigPage>& pages)
{
    EditorKit* kit = api.editorKit();
    if (!kit) return;
    std::unique_ptr<State>& slot = states_[&api];
    if (!slot)
    {
        slot = std::make_unique<State>();
        slot->builder = std::make_unique<LedDriveBuilder>(*kit);
    }
    ConfigPageHost::ConfigPage page;
    page.name = QStringLiteral("Dynamic Drive");
    try
    {
        page.widget = slot->builder->createLedDrivePage();
    }
    catch (const std::exception& error)
    {
        // Visible, not silent: the editor shows no Dynamic Drive page, says so, and the log has the reason.
        qWarning("Dynamic Drive page could not be built: %s", error.what());
        kit->setWarningStatus(QStringLiteral("The Dynamic Drive page could not be built: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    page.parentPage = QStringLiteral("VP-V");
    page.group = QStringLiteral("Drive");
    page.description = QStringLiteral("Projector LED dimming from the picture, the projector link and the gamma boost.");
    pages.push_back(page);
}

void LedDrivePage::OnEditorClosing(ConfigPageHost::ConfigFormApi& api)
{
    const auto found = states_.find(&api);
    if (found == states_.end()) return;
    // The RS-232 connection test (about 8 s) is joined before the editor goes.
    found->second->builder->projectorCards().JoinTest();
    states_.erase(found);
}

int LedDrivePage::StateCountForTesting() const
{
    return static_cast<int>(states_.size());
}

bool LedDrivePage::ProjectorTestRunningForTesting(ConfigPageHost::ConfigFormApi& api) const
{
    const auto found = states_.find(&api);
    return found != states_.end() && found->second->builder->projectorCards().TestRunning();
}

LedDrivePage& LedDrivePageInstance()
{
    static LedDrivePage instance;
    return instance;
}
}
