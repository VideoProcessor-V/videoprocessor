// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// The Calibration settings page, moved out of ConfigEditorWindow (R1 W1 CAL-C, P7-4): the page body,
// the calibration RPC and the status poll are the editor's former createCalibrationPage /
// runCalibrationCommand / applyCalibrationStatus, line for line; only the way they reach the editor
// changed (the EditorKit of ConfigPageHost.h instead of `this`). Object names, tooltips, rows and the
// order of cards are unchanged.

#include "CalibrationPage.h"

#include <ConfigurationRpcClient.h>

#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <string>

namespace Vpv
{
using ConfigPageHost::EditorKit;

struct CalibrationPage::State
{
    EditorKit* kit = nullptr;
    QObject* context = nullptr;      // owned here; receivers of queued results, dropped with the state
    QPointer<QWidget> page;          // the newest Calibration page of this editor
    QThread* rpcThread = nullptr;    // a status request in flight (bounded by the RPC timeout)
    QTimer* timer = nullptr;         // child of the editor window
    QString lastStep;
    bool active = false;
    bool probeRunning = false;
    ~State() { delete context; }
};

namespace
{
template <class T>
T* findIn(QWidget* root, const QString& name)
{
    return root ? root->findChild<T*>(name) : nullptr;
}

// Automatic calibration (docs/calibration/AUTO-CALIBRATION.md): the [calibration]
// settings - every key has a control here - and the wizard. The wizard talks to
// VP over the configuration RPC (Operation::Calibration): VP owns the renderer,
// the meter (ArgyllCMS spotread) and the configuration edits; this page sends
// commands and shows the status. A run uses the saved settings.
namespace
{
    QMap<QString, QString> parseCalibrationStatus(const QString& text)
    {
        QMap<QString, QString> values;
        for (const QString& line : text.split(QLatin1Char('\n')))
        {
            const int colon = line.indexOf(QStringLiteral(": "));
            if (colon <= 0) continue;
            QString value = line.mid(colon + 2);
            QString unescaped;
            for (int i = 0; i < value.size(); ++i)
            {
                if (value[i] == QLatin1Char('\\') && i + 1 < value.size())
                {
                    ++i;
                    unescaped += value[i] == QLatin1Char('n') ? QChar(QLatin1Char('\n')) : value[i];
                }
                else
                    unescaped += value[i];
            }
            values.insert(line.left(colon), unescaped);
        }
        return values;
    }

    QString calibrationStepTitle(const QString& step)
    {
        static const QMap<QString, QString> titles = {
            { QStringLiteral("idle"), QStringLiteral("Ready") },
            { QStringLiteral("opening_meter"), QStringLiteral("1. Connecting to the meter") },
            { QStringLiteral("meter_calibration"), QStringLiteral("1. Meter calibration") },
            { QStringLiteral("placement"), QStringLiteral("2. Place the meter") },
            { QStringLiteral("warm_up"), QStringLiteral("3. Warm-up") },
            { QStringLiteral("measuring"), QStringLiteral("4. Measuring") },
            { QStringLiteral("fitting"), QStringLiteral("5. Building the LUTs") },
            { QStringLiteral("verifying"), QStringLiteral("6. Verifying") },
            { QStringLiteral("review"), QStringLiteral("7. Review") },
            { QStringLiteral("applied"), QStringLiteral("Applied") },
            { QStringLiteral("discarded"), QStringLiteral("Discarded") },
            { QStringLiteral("cancelled"), QStringLiteral("Cancelled") },
            { QStringLiteral("failed"), QStringLiteral("Stopped") },
            // Phase 2.
            { QStringLiteral("user_action"), QStringLiteral("Your turn") },
            { QStringLiteral("done"), QStringLiteral("Done") },
        };
        return titles.value(step, step);
    }

    bool calibrationStepFinal(const QString& step)
    {
        return step.isEmpty() || step == QStringLiteral("idle") || step == QStringLiteral("applied") ||
            step == QStringLiteral("discarded") || step == QStringLiteral("cancelled") || step == QStringLiteral("failed") ||
            step == QStringLiteral("done");
    }
}
}

QWidget* CalibrationPage::Build(State& st)
{
    EditorKit* const k = st.kit;
    State* const stp = &st;
    auto value = [k](const QString& s, const QString& key) { return k->value(s, key, QString()); };
    auto helpLabel = [k](const QString& text) { return k->helpLabel(text); };
    auto createCard = [k](const QString& title, const QString& description, QWidget* content) { return k->createCard(title, description, content); };
    auto controlName = [k](const QString& s, const QString& key) { return k->controlName(s, key); };
    auto bindTextField = [k](const QString& s, const QString& key, const QString& fallback = QString()) { return k->bindTextField(s, key, fallback); };
    auto bindChoiceField = [k](const QString& s, const QString& key, const QStringList& values, const QStringList& labels = QStringList())
    { return k->bindChoiceField(s, key, values, labels); };
    auto bindCheckField = [k](const QString& label, const QString& s, const QString& key, bool defaultValue = false)
    { return k->bindCheckField(label, s, key, defaultValue); };
    const QString section = QStringLiteral("calibration");
    QWidget* cards = k->createCardGrid();

    auto setValue = [k, section](const QString& key, const QString& text, QObject* edited)
    {
        k->setKnown(section, key, text);
        k->markDirty(edited);
    };
    auto integer = [&](const QString& key, int minimum, int maximum, int fallback,
        const QString& suffix)
    {
        auto* spin = new QSpinBox;
        spin->setObjectName(controlName(section, key));
        spin->setAccessibleName(key);
        spin->setRange(minimum, maximum);
        bool ok = false;
        const int configured = value(section, key).toInt(&ok);
        spin->setValue(ok ? std::min(std::max(configured, minimum), maximum) : fallback);
        if (!suffix.isEmpty()) spin->setSuffix(QStringLiteral(" ") + suffix);
        spin->setMaximumWidth(140);
        QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), spin,
            [setValue, key, spin](int v) { setValue(key, QString::number(v), spin); });
        return spin;
    };
    auto number = [&](const QString& key, double minimum, double maximum, double fallback,
        int decimals, const QString& suffix)
    {
        auto* spin = new QDoubleSpinBox;
        spin->setObjectName(controlName(section, key));
        spin->setAccessibleName(key);
        spin->setRange(minimum, maximum);
        spin->setDecimals(decimals);
        bool ok = false;
        const double configured = value(section, key).toDouble(&ok);
        spin->setValue(ok ? std::min(std::max(configured, minimum), maximum) : fallback);
        if (!suffix.isEmpty()) spin->setSuffix(QStringLiteral(" ") + suffix);
        spin->setMaximumWidth(140);
        QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), spin,
            [setValue, key, spin](double v) { setValue(key, QString::number(v, 'g', 6), spin); });
        return spin;
    };
    auto browseRow = [&](QLineEdit* edit, bool folder, const QString& filter)
    {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(edit, 1);
        auto* browse = new QPushButton(QStringLiteral("Browse…"));
        browse->setObjectName(edit->objectName() + QStringLiteral(".browse"));
        layout->addWidget(browse);
        QObject::connect(browse, &QPushButton::clicked, browse, [k, edit, folder, filter]()
        {
            const QString chosen = folder ?
                QFileDialog::getExistingDirectory(k->window(), QStringLiteral("ArgyllCMS folder"), edit->text()) :
                QFileDialog::getOpenFileName(k->window(), QStringLiteral("Spectral correction"), edit->text(), filter);
            if (!chosen.isEmpty()) edit->setText(QDir::toNativeSeparators(chosen));
        });
        return row;
    };

    // ------------------------------------------------------------- meter
    auto* meter = new QWidget;
    auto* meterForm = new QFormLayout(meter);
    meterForm->setContentsMargins(0, 0, 0, 0);
    meterForm->setVerticalSpacing(8);
    auto* argyll = bindTextField(section, QStringLiteral("argyll_path"));
    argyll->setPlaceholderText(QStringLiteral("Search PATH and the usual install folders"));
    argyll->setToolTip(QStringLiteral(
        "The ArgyllCMS folder (the one holding bin\\spotread.exe), bin itself, or spotread.exe. Empty: VP searches "
        "ARGYLL_PATH, PATH, Program Files, your profile and C:\\ for an Argyll* folder. VP never bundles or "
        "downloads ArgyllCMS."));
    meterForm->addRow(QStringLiteral("ArgyllCMS folder"), browseRow(argyll, true, QString()));
    auto* detect = new QPushButton(QStringLiteral("Detect ArgyllCMS and meters"));
    detect->setObjectName(QStringLiteral("calibration.detect"));
    detect->setMaximumWidth(260);
    auto* probe = helpLabel(QStringLiteral("Press Detect to find ArgyllCMS and the connected meters (uses the saved "
        "folder; Apply first after changing it)."));
    probe->setObjectName(QStringLiteral("calibration.probe"));
    probe->setTextInteractionFlags(Qt::TextSelectableByMouse);
    meterForm->addRow(detect, probe);
    QObject::connect(detect, &QPushButton::clicked, detect, [this, stp]() { Run(*stp, QStringLiteral("detect"), QString()); });

    const QStringList models = { QStringLiteral("auto"), QStringLiteral("i1d3"), QStringLiteral("colormunki_display"),
        QStringLiteral("i1d2"), QStringLiteral("spyderx2"), QStringLiteral("spyderx"), QStringLiteral("spyder5"),
        QStringLiteral("spyder4"), QStringLiteral("dtp94"), QStringLiteral("i1pro3"), QStringLiteral("i1pro2"),
        QStringLiteral("i1pro"), QStringLiteral("colormunki_photo"), QStringLiteral("jeti"), QStringLiteral("klein_k10"),
        QStringLiteral("other"), QStringLiteral("simulated") };
    const QStringList modelLabels = { QStringLiteral("Automatic (from the connected meter)"),
        QStringLiteral("Calibrite Display Pro / Plus (HL), i1Display Pro / Plus"),
        QStringLiteral("ColorMunki Display, i1Display Studio"), QStringLiteral("i1Display 2 / LT, Huey Pro"),
        QStringLiteral("Datacolor Spyder X2"), QStringLiteral("Datacolor Spyder X"), QStringLiteral("Datacolor Spyder 5"),
        QStringLiteral("Datacolor Spyder 4"), QStringLiteral("X-Rite DTP94"), QStringLiteral("X-Rite i1Pro 3"),
        QStringLiteral("X-Rite i1Pro 2"), QStringLiteral("X-Rite i1Pro"), QStringLiteral("X-Rite ColorMunki Photo / Design"),
        QStringLiteral("JETI specbos / spectraval"), QStringLiteral("Klein K10-A"),
        QStringLiteral("Another ArgyllCMS instrument"), QStringLiteral("Dry run (simulated meter, real patches)") };
    auto* model = bindChoiceField(section, QStringLiteral("meter_model"), models, modelLabels);
    model->setToolTip(QStringLiteral("Any meter ArgyllCMS supports works. The model tunes VP's handling: sensor "
        "calibration prompts (spectros), the low-light floor for near-black averaging, CCSS or CCMX corrections."));
    meterForm->addRow(QStringLiteral("Meter"), model);
    auto* port = integer(QStringLiteral("meter_port"), 0, 64, 0, QString());
    port->setSpecialValueText(QStringLiteral("First found"));
    port->setToolTip(QStringLiteral("spotread's -c number from Detect's list; First found uses the first meter."));
    meterForm->addRow(QStringLiteral("Meter number"), port);
    auto* displayType = bindTextField(section, QStringLiteral("display_type"));
    displayType->setPlaceholderText(QStringLiteral("The meter's default"));
    displayType->setMaximumWidth(140);
    displayType->setToolTip(QStringLiteral("spotread's -y display type selector (Detect lists the meter's choices). "
        "For a colorimeter pick the type closest to your display's light source."));
    meterForm->addRow(QStringLiteral("Display type"), displayType);
    auto* correction = bindTextField(section, QStringLiteral("spectral_correction"));
    correction->setPlaceholderText(QStringLiteral("None"));
    correction->setToolTip(QStringLiteral("A .ccss spectral sample set (i1Display / Spyder family) or a .ccmx matrix "
        "for your display's light source; strongly advised for laser and LED projectors and wide-gamut screens."));
    meterForm->addRow(QStringLiteral("Spectral correction"),
        browseRow(correction, false, QStringLiteral("Corrections (*.ccss *.ccmx);;All files (*)")));
    auto* refresh = bindChoiceField(section, QStringLiteral("refresh_mode"),
        { QStringLiteral("auto"), QStringLiteral("refresh"), QStringLiteral("non_refresh") },
        { QStringLiteral("Meter decides"), QStringLiteral("Refresh display"), QStringLiteral("Non-refresh display") });
    refresh->setToolTip(QStringLiteral("Whether the meter synchronises to the display's refresh (spotread -Y r / -Y n). "
        "Meter decides measures it."));
    meterForm->addRow(QStringLiteral("Refresh mode"), refresh);
    meterForm->addRow(QStringLiteral("Reading time limit"),
        integer(QStringLiteral("read_timeout_s"), 5, 600, 60, QStringLiteral("s")));
    k->addCard(cards, createCard(QStringLiteral("Meter"), QStringLiteral(
        "ArgyllCMS's spotread drives the meter; install ArgyllCMS yourself from argyllcms.com."), meter));

    // ----------------------------------------------------- display / patches
    auto* display = new QWidget;
    auto* displayForm = new QFormLayout(display);
    displayForm->setContentsMargins(0, 0, 0, 0);
    displayForm->setVerticalSpacing(8);
    auto* kind = bindChoiceField(section, QStringLiteral("display_kind"),
        { QStringLiteral("projector"), QStringLiteral("lcd"), QStringLiteral("oled"), QStringLiteral("other") },
        { QStringLiteral("Projector"), QStringLiteral("LCD TV / monitor"), QStringLiteral("OLED TV / monitor"),
            QStringLiteral("Other") });
    kind->setToolTip(QStringLiteral("Sets the patch window and settle time when those are left at automatic: "
        "projectors full field, LCD 18% window, OLED 10% window (keeps ABL out of the measurement)."));
    displayForm->addRow(QStringLiteral("Display"), kind);
    auto* name = bindTextField(section, QStringLiteral("display_name"));
    name->setPlaceholderText(QStringLiteral("The output monitor's name"));
    displayForm->addRow(QStringLiteral("Display name"), name);
    auto* window = number(QStringLiteral("patch_window_percent"), 0.0, 100.0, 0.0, 1, QStringLiteral("% of the screen"));
    window->setSpecialValueText(QStringLiteral("Automatic"));
    displayForm->addRow(QStringLiteral("Patch window"), window);
    displayForm->addRow(QStringLiteral("Window surround"),
        number(QStringLiteral("background_percent"), 0.0, 100.0, 0.0, 1, QStringLiteral("% grey")));
    auto* settle = integer(QStringLiteral("settle_ms"), 0, 30000, 0, QStringLiteral("ms"));
    settle->setSpecialValueText(QStringLiteral("Automatic"));
    settle->setToolTip(QStringLiteral("The wait after each patch change before reading. Automatic: 0.5 s for "
        "projectors, 0.8 s OLED, 1 s LCD, plus more after a large drop to dark."));
    displayForm->addRow(QStringLiteral("Settle time"), settle);
    auto* reads = integer(QStringLiteral("near_black_max_reads"), 1, 16, 4, QStringLiteral("readings"));
    reads->setToolTip(QStringLiteral("Near black a colorimeter's count is noisy: VP averages up to this many readings "
        "(stopping early when two agree) and marks patches below the meter's floor."));
    displayForm->addRow(QStringLiteral("Near-black readings"), reads);
    displayForm->addRow(QStringLiteral("Agreement"),
        number(QStringLiteral("repeat_tolerance_percent"), 0.1, 20.0, 1.0, 1, QStringLiteral("%")));
    auto* warm = integer(QStringLiteral("warmup_minutes"), 0, 180, 30, QStringLiteral("min"));
    warm->setToolTip(QStringLiteral("Displays drift as they warm up; the wizard can skip it when the display has "
        "been on long enough."));
    displayForm->addRow(QStringLiteral("Warm-up"), warm);
    auto* drift = integer(QStringLiteral("drift_interval"), 0, 1000, 50, QStringLiteral("patches"));
    drift->setToolTip(QStringLiteral("White is measured again every this many patches; VP corrects slow drift."));
    displayForm->addRow(QStringLiteral("Drift check every"), drift);
    k->addCard(cards, createCard(QStringLiteral("Display and patches"), QStringLiteral(
        "Patches are exact codes in VP's active output (BTB / WTW included); the sigma-delta and dither are "
        "bypassed for the patch only."), display));

    // ------------------------------------------------------------ target
    auto* target = new QWidget;
    auto* targetForm = new QFormLayout(target);
    targetForm->setContentsMargins(0, 0, 0, 0);
    targetForm->setVerticalSpacing(8);
    auto* plan = bindChoiceField(section, QStringLiteral("plan"),
        { QStringLiteral("quick"), QStringLiteral("cube5"), QStringLiteral("standard"), QStringLiteral("cube13"),
            QStringLiteral("thorough") },
        { QStringLiteral("Quick (greyscale + colours)"), QStringLiteral("5³ cube"), QStringLiteral("Standard (9³ cube)"),
            QStringLiteral("13³ cube"), QStringLiteral("Thorough (17³ cube)") });
    targetForm->addRow(QStringLiteral("Measurements"), plan);
    auto* estimate = helpLabel(QString());
    estimate->setObjectName(QStringLiteral("calibration.plan_estimate"));
    targetForm->addRow(QString(), estimate);
    auto showEstimate = [plan, estimate]()
    {
        static const QMap<QString, QString> times = {
            { QStringLiteral("quick"), QStringLiteral("About 10 minutes on a projector (longer on a dim display).") },
            { QStringLiteral("cube5"), QStringLiteral("About 15 minutes.") },
            { QStringLiteral("standard"), QStringLiteral("About 35-45 minutes.") },
            { QStringLiteral("cube13"), QStringLiteral("About 1.5 hours.") },
            { QStringLiteral("thorough"), QStringLiteral("About 2.5 hours.") } };
        estimate->setText(times.value(plan->currentData().toString()));
    };
    QObject::connect(plan, qOverload<int>(&QComboBox::currentIndexChanged), plan, [showEstimate](int) { showEstimate(); });
    showEstimate();
    targetForm->addRow(QStringLiteral("Target response"), bindChoiceField(section, QStringLiteral("target_transfer"),
        { QStringLiteral("bt1886"), QStringLiteral("gamma") },
        { QStringLiteral("BT.1886 (with the measured black)"), QStringLiteral("Pure gamma") }));
    targetForm->addRow(QStringLiteral("Gamma"), number(QStringLiteral("target_gamma"), 1.6, 3.0, 2.4, 2, QString()));
    targetForm->addRow(QStringLiteral("White level"),
        number(QStringLiteral("target_white_percent"), 25.0, 100.0, 100.0, 0, QStringLiteral("% of the brightest D65 white")));
    targetForm->addRow(QStringLiteral("LUT size"), bindChoiceField(section, QStringLiteral("lut_size"),
        { QStringLiteral("17"), QStringLiteral("33"), QStringLiteral("65") },
        { QStringLiteral("17³"), QStringLiteral("33³ (recommended)"), QStringLiteral("65³") }));
    auto* verify = bindCheckField(QStringLiteral("Verify through the new LUT (about 60 patches)"), section,
        QStringLiteral("verify_after"), true);
    targetForm->addRow(QString(), verify);
    k->addCard(cards, createCard(QStringLiteral("Target and LUT"), QStringLiteral(
        "One run writes the BT.709, P3-D65 and BT.2020 LUTs; Apply attaches them to the live Color / Output profile."),
        target));

    // ------------------------------------------- phase 2: the other runs
    auto* runs = new QWidget;
    auto* runsForm = new QFormLayout(runs);
    runsForm->setContentsMargins(0, 0, 0, 0);
    runsForm->setVerticalSpacing(8);
    auto* fluxStep = integer(QStringLiteral("led_flux_step"), 1, 50, 5, QStringLiteral("LPW1 steps"));
    fluxStep->setToolTip(QStringLiteral("Dynamic Drive run: the flux sweep's LPW1 step, from full light down to the "
        "lowest level VP drives and back up (both directions show the hysteresis)."));
    runsForm->addRow(QStringLiteral("Flux sweep step"), fluxStep);
    auto* dwell = integer(QStringLiteral("led_dwell_ms"), 500, 20000, 3000, QStringLiteral("ms"));
    dwell->setToolTip(QStringLiteral("Wait after each LPW1 change before the meter reads (the LED settling plus margin)."));
    runsForm->addRow(QStringLiteral("LED dwell"), dwell);
    runsForm->addRow(QStringLiteral("Readings per level"),
        integer(QStringLiteral("led_reads_per_level"), 1, 8, 2, QStringLiteral("readings")));
    auto* cntrMax = integer(QStringLiteral("led_cntr_sweep_max"), 50, 100, 74, QString());
    cntrMax->setToolTip(QStringLiteral("The highest CNTR the run sets (from 50 up). Find where code 235 stops gaining "
        "(dmd_clip_cntr) and where light stops rising (cntr_max_usable). CNTR goes back to 50 at the end or on Cancel."));
    runsForm->addRow(QStringLiteral("CNTR sweep up to"), cntrMax);
    runsForm->addRow(QStringLiteral("CNTR dwell"),
        integer(QStringLiteral("led_cntr_dwell_ms"), 200, 20000, 1500, QStringLiteral("ms")));
    auto* sensorPort = bindTextField(section, QStringLiteral("led_sensor_port"));
    sensorPort->setPlaceholderText(QStringLiteral("None (latency not measured)"));
    sensorPort->setMaximumWidth(160);
    sensorPort->setToolTip(QStringLiteral("A fast light sensor (photodiode or VEML7700 in a fast mode on a "
        "microcontroller, one reading per line: value, or device microseconds and value) on its own COM port. A "
        "colorimeter is too slow for the LED's response time; without this sensor latency and settle are not measured."));
    runsForm->addRow(QStringLiteral("Fast light sensor port"), sensorPort);
    runsForm->addRow(QStringLiteral("Fast sensor speed"),
        integer(QStringLiteral("led_sensor_baud"), 1200, 2000000, 115200, QStringLiteral("baud")));
    runsForm->addRow(QStringLiteral("Steps per direction"),
        integer(QStringLiteral("led_step_repeats"), 1, 10, 3, QString()));
    auto* hdrCurve = bindChoiceField(section, QStringLiteral("hdr_verify_tone_curve"),
        { QStringLiteral("vp"), QStringLiteral("bt2390"), QStringLiteral("none") },
        { QStringLiteral("VP's own tone mapping"), QStringLiteral("BT.2390 model"), QStringLiteral("None (PQ as sent)") });
    hdrCurve->setToolTip(QStringLiteral("HDR verify: the target for each PQ patch. VP's own = the tone curve VP uses for "
        "the playing HDR source (falls back to BT.2390 before the first HDR frame)."));
    runsForm->addRow(QStringLiteral("HDR verify target"), hdrCurve);
    runsForm->addRow(QStringLiteral("HDR source peak (model)"),
        number(QStringLiteral("hdr_verify_source_peak"), 100.0, 10000.0, 1000.0, 0, QStringLiteral("cd/m²")));
    auto* record = bindCheckField(QStringLiteral("Record real meter transcripts (test-data format)"), section,
        QStringLiteral("record_transcripts"), false);
    record->setToolTip(QStringLiteral("Saves every spotread session as a script for VP's tests "
        "(recorded_<meter>_<time>.txt): commit one to src\\VideoProcessor-Test\\data\\spotread."));
    runsForm->addRow(QString(), record);
    auto* transcripts = bindTextField(section, QStringLiteral("transcript_folder"));
    transcripts->setPlaceholderText(QStringLiteral("luts\\calibration\\transcripts"));
    runsForm->addRow(QStringLiteral("Transcript folder"), browseRow(transcripts, true, QString()));
    k->addCard(cards, createCard(QStringLiteral("Dynamic Drive, room light and HDR runs"), QStringLiteral(
        "Choose the run in Calibrate. The Dynamic Drive run steps the projector's LED and CNTR within its ranges and "
        "always restores full light and CNTR 50."), runs));

    // ------------------------------------------------------------ wizard
    auto* wizard = new QWidget;
    auto* wizardLayout = new QVBoxLayout(wizard);
    wizardLayout->setContentsMargins(0, 0, 0, 0);
    wizardLayout->setSpacing(8);
    auto* step = new QLabel(QStringLiteral("Ready"));
    step->setObjectName(QStringLiteral("calibration.wizard.step"));
    step->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600;"));
    wizardLayout->addWidget(step);
    auto* instruction = helpLabel(QStringLiteral(
        "Start runs the calibration with the saved settings: connect the meter, place it, measure, review, then "
        "Apply or Discard. VP shows the patches; keep a source playing so the VP Renderer is running."));
    instruction->setObjectName(QStringLiteral("calibration.wizard.instruction"));
    instruction->setTextInteractionFlags(Qt::TextSelectableByMouse);
    wizardLayout->addWidget(instruction);
    auto* progress = new QProgressBar;
    progress->setObjectName(QStringLiteral("calibration.wizard.progress"));
    progress->setRange(0, 1);
    progress->setValue(0);
    progress->setTextVisible(true);
    wizardLayout->addWidget(progress);
    auto* reading = new QLabel;
    reading->setObjectName(QStringLiteral("calibration.wizard.reading"));
    wizardLayout->addWidget(reading);
    auto* message = helpLabel(QString());
    message->setObjectName(QStringLiteral("calibration.wizard.message"));
    message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    wizardLayout->addWidget(message);
    auto* review = new QPlainTextEdit;
    review->setObjectName(QStringLiteral("calibration.wizard.review"));
    review->setReadOnly(true);
    review->setMinimumHeight(140);
    review->setVisible(false);
    wizardLayout->addWidget(review);
    // Phase 2: which run Start begins, and the room's lux for the ambient check.
    auto* runRow = new QWidget;
    auto* runLayout = new QHBoxLayout(runRow);
    runLayout->setContentsMargins(0, 0, 0, 0);
    runLayout->addWidget(new QLabel(QStringLiteral("Run")));
    auto* runKind = new QComboBox;
    runKind->setObjectName(QStringLiteral("calibration.wizard.kind"));
    runKind->addItem(QStringLiteral("Display calibration (3D LUTs)"), QStringLiteral("calibration"));
    runKind->addItem(QStringLiteral("Dynamic Drive characterisation (projector LED / CNTR)"), QStringLiteral("led"));
    runKind->addItem(QStringLiteral("Ambient-light check (screen reflectance)"), QStringLiteral("ambient"));
    runKind->addItem(QStringLiteral("HDR (PQ) verify"), QStringLiteral("hdr_verify"));
    runKind->setToolTip(QStringLiteral("Display calibration builds the LUTs. Dynamic Drive measures the flux table, the "
        "CNTR curve and (with a fast sensor) the LED timing. Ambient-light measures black and white with the room "
        "lights off and on (ambient_screen_reflect). HDR verify measures PQ tracking and dE ITP."));
    runLayout->addWidget(runKind, 1);
    runLayout->addWidget(new QLabel(QStringLiteral("Room light")));
    auto* lux = new QDoubleSpinBox;
    lux->setObjectName(QStringLiteral("calibration.wizard.lux"));
    lux->setRange(0.0, 200000.0);
    lux->setDecimals(1);
    lux->setSuffix(QStringLiteral(" lux"));
    lux->setSpecialValueText(QStringLiteral("Sensor"));
    lux->setMaximumWidth(130);
    lux->setToolTip(QStringLiteral("Ambient-light check without VP's light sensor: the room's lux, sent with Continue "
        "(a phone light-meter app is close enough). Sensor: VP's [ambient] sensor reading; a dark room counts as 0."));
    runLayout->addWidget(lux);
    wizardLayout->addWidget(runRow);
    auto* buttons = new QWidget;
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    const QList<QPair<QString, QString>> actions = {
        { QStringLiteral("start"), QStringLiteral("Start") }, { QStringLiteral("continue"), QStringLiteral("Continue") },
        { QStringLiteral("skip"), QStringLiteral("Skip warm-up") }, { QStringLiteral("apply"), QStringLiteral("Apply") },
        { QStringLiteral("discard"), QStringLiteral("Discard") }, { QStringLiteral("cancel"), QStringLiteral("Cancel") } };
    for (const auto& action : actions)
    {
        auto* button = new QPushButton(action.second);
        button->setObjectName(QStringLiteral("calibration.wizard.") + action.first);
        button->setEnabled(action.first == QStringLiteral("start"));
        buttonLayout->addWidget(button);
        const QString command = action.first;
        QObject::connect(button, &QPushButton::clicked, button, [this, stp, k, command, runKind, lux]()
        {
            if (command == QStringLiteral("start") && k->documentDirty())
                k->setWarningStatus(QStringLiteral("Calibration uses the saved settings: Apply your changes first."));
            QString arguments;
            if (command == QStringLiteral("start"))
                arguments = QStringLiteral("kind: ") + runKind->currentData().toString();
            else if (command == QStringLiteral("continue") && lux->value() > 0.0)
                arguments = QStringLiteral("lux: ") + QString::number(lux->value(), 'f', 1);
            Run(*stp, command, arguments);
        });
    }
    buttonLayout->addStretch(1);
    wizardLayout->addWidget(buttons);
    if (k->noTarget())
    {
        instruction->setText(QStringLiteral("Connect to a running VideoProcessor to calibrate."));
        for (QPushButton* button : buttons->findChildren<QPushButton*>()) button->setEnabled(false);
        detect->setEnabled(false);
    }
    k->addCard(cards, createCard(QStringLiteral("Calibrate"), QStringLiteral(
        "Cancel at any step returns the display to video; nothing changes until you Apply, and Apply backs up the "
        "configuration first."), wizard));

    if (!st.timer)
    {
        // One status poll per editor, as before: only while this page is showing or a run is active.
        st.timer = new QTimer(k->window());
        st.timer->setInterval(1000);
        QObject::connect(st.timer, &QTimer::timeout, st.context, [this, stp]()
        {
            if (stp->kit->noTarget()) return;
            QWidget* current = stp->kit->currentPage();
            const bool visible = current && stp->page && current == stp->page.data();
            if (visible || stp->active || stp->probeRunning)
                Run(*stp, QStringLiteral("status"), QString());
        });
        st.timer->start();
    }
    QWidget* page = k->createPage(QStringLiteral("Calibration"),
        QStringLiteral("Automatic display calibration with a meter: VP measures its own patches and writes 3D LUTs."),
        cards);
    st.page = page;
    return page;
}

void CalibrationPage::Run(State& st, const QString& command, const QString& arguments)
{
    if (st.kit->settingsPreview())
    {
        Apply(st, QString(), QStringLiteral("Settings preview: VP is not connected."), command);
        return;
    }
    if (st.kit->noTarget() || st.rpcThread) return;
    const EditorKit::RemoteTarget target = st.kit->remoteTarget();
    const bool remote = target.remote;
    const QString host = remote ? target.host : QStringLiteral("127.0.0.1");
    const quint16 rpcPort = remote ? target.port : 41686;
    struct Outcome { bool ok = false; QString text, error; };
    const auto outcome = std::make_shared<Outcome>();
    const std::string commandText = command.toStdString();
    const std::string argumentText = arguments.toStdString();
    QThread* worker = QThread::create([outcome, host, rpcPort, commandText, argumentText]()
    {
        ConfigurationRpcClient client(host.toStdString(), rpcPort);
        std::string status, error;
        outcome->ok = client.Calibration(commandText, argumentText, status, error);
        outcome->text = QString::fromStdString(status);
        outcome->error = QString::fromStdString(error);
    });
    worker->setParent(st.kit->window());
    st.rpcThread = worker;
    State* const stp = &st;
    QObject::connect(worker, &QThread::finished, st.context, [this, stp, worker, outcome, command]()
    {
        if (stp->rpcThread == worker) stp->rpcThread = nullptr;
        worker->deleteLater();
        Apply(*stp, outcome->ok ? outcome->text : QString(),
            outcome->ok ? QString() : outcome->error, command);
    });
    worker->start();
}

void CalibrationPage::Apply(State& st, const QString& text, const QString& error, const QString& command)
{
    QWidget* const root = st.page.data();
    auto* step = findIn<QLabel>(root, QStringLiteral("calibration.wizard.step"));
    auto* instruction = findIn<QLabel>(root, QStringLiteral("calibration.wizard.instruction"));
    auto* message = findIn<QLabel>(root, QStringLiteral("calibration.wizard.message"));
    auto* reading = findIn<QLabel>(root, QStringLiteral("calibration.wizard.reading"));
    auto* progress = findIn<QProgressBar>(root, QStringLiteral("calibration.wizard.progress"));
    auto* review = findIn<QPlainTextEdit>(root, QStringLiteral("calibration.wizard.review"));
    auto* probe = findIn<QLabel>(root, QStringLiteral("calibration.probe"));
    if (!step || !instruction || !message || !reading || !progress || !review) return;
    if (!error.isEmpty())
    {
        message->setText(QStringLiteral("VideoProcessor did not answer: ") + error +
            QStringLiteral(" (calibration runs inside VideoProcessor; start it first)."));
        return;
    }
    const QMap<QString, QString> s = parseCalibrationStatus(text);
    const QString state = s.value(QStringLiteral("step"));
    const QString previous = st.lastStep;
    st.lastStep = state;
    st.active = !calibrationStepFinal(state) || state == QStringLiteral("review");
    st.probeRunning = s.value(QStringLiteral("probe")) == QStringLiteral("running");
    step->setText(calibrationStepTitle(state));
    if (!s.value(QStringLiteral("instruction")).isEmpty())
        instruction->setText(s.value(QStringLiteral("instruction")));
    QString note = s.value(QStringLiteral("message"));
    if (text.startsWith(QStringLiteral("error: ")))
        note = text.section(QLatin1Char('\n'), 0, 0).mid(7).replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    if (!s.value(QStringLiteral("format")).isEmpty())
        note += (note.isEmpty() ? QString() : QStringLiteral("\n")) + QStringLiteral("Output: ") + s.value(QStringLiteral("format"));
    message->setText(note);
    const QStringList xyz = s.value(QStringLiteral("reading")).split(QLatin1Char(' '));
    if (xyz.size() == 3)
    {
        const double x = xyz[0].toDouble(), y = xyz[1].toDouble(), z = xyz[2].toDouble();
        const double sum = x + y + z;
        reading->setText(sum > 0.0 ? QStringLiteral("Last reading: Y %1 cd/m², x %2, y %3")
            .arg(y, 0, 'f', 3).arg(x / sum, 0, 'f', 4).arg(y / sum, 0, 'f', 4) : QString());
    }
    else
        reading->clear();
    auto fraction = [](const QString& value, int& done, int& total)
    {
        done = value.section(QLatin1Char('/'), 0, 0).toInt();
        total = value.section(QLatin1Char('/'), 1, 1).toInt();
    };
    int done = 0, total = 0;
    if (state == QStringLiteral("verifying")) fraction(s.value(QStringLiteral("verify")), done, total);
    else fraction(s.value(QStringLiteral("patch")), done, total);
    const int remaining = s.value(QStringLiteral("seconds_remaining")).toInt();
    if (state == QStringLiteral("warm_up"))
    {
        const int left = s.value(QStringLiteral("warmup_remaining")).toInt();
        progress->setRange(0, 1);
        progress->setValue(0);
        progress->setFormat(QStringLiteral("Warm-up: %1:%2 left").arg(left / 60).arg(left % 60, 2, 10, QLatin1Char('0')));
    }
    else if (total > 0 && (state == QStringLiteral("measuring") || state == QStringLiteral("verifying")))
    {
        progress->setRange(0, total);
        progress->setValue(done);
        progress->setFormat(QStringLiteral("%v / %m") + (remaining > 0 ?
            QStringLiteral(" - about %1 min left").arg((remaining + 59) / 60) : QString()));
    }
    else
    {
        progress->setRange(0, 1);
        progress->setValue(state == QStringLiteral("review") || state == QStringLiteral("applied") ||
            state == QStringLiteral("done") ? 1 : 0);
        progress->setFormat(QString());
    }
    const bool showReview = state == QStringLiteral("review") || state == QStringLiteral("applied") ||
        state == QStringLiteral("done");
    review->setVisible(showReview);
    if (showReview)
    {
        QString body = s.value(QStringLiteral("review"));
        if (!s.value(QStringLiteral("diff")).isEmpty())
            body += QStringLiteral("\n\nConfiguration changes:\n") + s.value(QStringLiteral("diff"));
        if (!s.value(QStringLiteral("backup")).isEmpty())
            body += QStringLiteral("\n") + s.value(QStringLiteral("backup"));
        if (!s.value(QStringLiteral("run")).isEmpty())
            body += QStringLiteral("\nRun folder: ") + s.value(QStringLiteral("run"));
        if (review->toPlainText() != body) review->setPlainText(body);
    }
    const bool canApply = state == QStringLiteral("review") &&
        !s.value(QStringLiteral("diff")).startsWith(QStringLiteral("Apply is not possible"));
    const QMap<QString, bool> enabled = {
        { QStringLiteral("start"), calibrationStepFinal(state) },
        { QStringLiteral("continue"), state == QStringLiteral("meter_calibration") || state == QStringLiteral("placement") ||
            state == QStringLiteral("user_action") },
        { QStringLiteral("skip"), state == QStringLiteral("warm_up") },
        { QStringLiteral("apply"), canApply },
        { QStringLiteral("discard"), state == QStringLiteral("review") },
        { QStringLiteral("cancel"), !calibrationStepFinal(state) && state != QStringLiteral("review") } };
    for (auto it = enabled.begin(); it != enabled.end(); ++it)
        if (auto* button = findIn<QPushButton>(root, QStringLiteral("calibration.wizard.") + it.key()))
            button->setEnabled(it.value() && !st.kit->noTarget());
    if (probe && s.value(QStringLiteral("probe")) == QStringLiteral("running"))
        probe->setText(QStringLiteral("Detecting… (running spotread -?)"));
    else if (probe && s.contains(QStringLiteral("probe_text")))
        probe->setText(s.value(QStringLiteral("probe_text")) +
            (s.value(QStringLiteral("display_types")).isEmpty() ? QString() :
                QStringLiteral("\nDisplay types (-y): ") +
                s.value(QStringLiteral("display_types")).replace(QLatin1Char('\n'), QStringLiteral("; "))
                    .replace(QLatin1Char('|'), QStringLiteral(" = "))));
    // The run changed the configuration VP uses: reload it here too.
    if (state == QStringLiteral("applied") && previous != QStringLiteral("applied") && command == QStringLiteral("status"))
    {
        st.kit->setStatus(s.value(QStringLiteral("kind")) == QStringLiteral("calibration") || s.value(QStringLiteral("kind")).isEmpty() ?
            QStringLiteral("Calibration applied: VP attached the new LUTs. Reloading the configuration.") :
            QStringLiteral("Calibration result applied. Reloading the configuration."));
        if (!st.kit->documentDirty()) st.kit->discardEdits();
    }
}

CalibrationPage& CalibrationPageInstance()
{
    static CalibrationPage instance;
    return instance;
}

CalibrationPage::CalibrationPage() = default;
CalibrationPage::~CalibrationPage() = default;

void CalibrationPage::AppendPages(ConfigPageHost::ConfigFormApi& api, std::vector<ConfigPageHost::ConfigPage>& pages)
{
    EditorKit* kit = api.editorKit();
    if (!kit) return;
    std::unique_ptr<State>& slot = states_[&api];
    if (!slot)
    {
        slot = std::make_unique<State>();
        slot->kit = kit;
        slot->context = new QObject;
    }
    ConfigPageHost::ConfigPage page;
    page.name = QStringLiteral("Calibration");
    page.widget = Build(*slot);
    page.parentPage = QStringLiteral("VP-V");
    page.group = QStringLiteral("Output");
    page.description = QStringLiteral("Automatic display calibration with a meter: VP measures its own patches and writes 3D LUTs.");
    pages.push_back(page);
}

void CalibrationPage::OnEditorClosing(ConfigPageHost::ConfigFormApi& api)
{
    const auto found = states_.find(&api);
    if (found == states_.end()) return;
    // A status request (bounded by the RPC timeout) is joined before the editor goes.
    if (found->second->rpcThread)
    {
        found->second->rpcThread->wait();
        found->second->rpcThread = nullptr;
    }
    states_.erase(found);
}

void CalibrationPage::ApplyStatusForTesting(QWidget* inPage, const QString& text, const QString& error, const QString& command)
{
    for (auto& entry : states_)
    {
        State& st = *entry.second;
        if (st.page && inPage && (st.page == inPage || st.page->isAncestorOf(inPage)))
        {
            Apply(st, text, error, command);
            return;
        }
    }
}

int CalibrationPage::StateCountForTesting() const
{
    return static_cast<int>(states_.size());
}
}
