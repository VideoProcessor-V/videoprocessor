// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Config editor helpers that were VP-only lines inside Bill's ConfigEditorWindow.cpp (P9 rows i and h, doc 60 s4.1):
// the sandbox-preview switch, the Target-delay note, canonical shortcut spelling in the fields, the number-field
// clamp/snap, retired choice values, the exclusive "Not set" buttons and the GLSL //!PARAM reader. Moved byte for
// byte; ConfigEditorWindow.cpp reaches them through VpvRenderingDefaults.h, its one VP-V helper include, so its
// call sites are unchanged. Included by ConfigEditorWindow.cpp ONLY (unnamed namespace: one copy per translation
// unit, never include this from another file). Qt, QueueLatency.h and the standard library only.

#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QWidget>

#include <QueueLatency.h>

#include <algorithm>
#include <cmath>

// Settings preview (queue item 23): VideoProcessorConfig.exe --sandbox edits only its --config
// file. It never signals, queries or commands a running VP (no live apply, no live status, no
// profile/action/calibration/projector commands) and never searches the LAN for targets.
static bool sandboxPreview()
{
    return qApp && qApp->property("vpSandboxPreview").toBool();
}

namespace
{
// Bill's canonicalShortcutText (ConfigEditorWindow.cpp, same unnamed namespace): declared here for
// showCanonicalShortcutSpelling below.
QString canonicalShortcutText(const QString& text);

// Stage 5: what a Target delay really gives at the common frame rates.
QString queueLatencyNoteText(int milliseconds)
{
    if (milliseconds <= 0)
        return QStringLiteral(
            // audit 2026-10-16: Target frames moved out of Advanced (Queue target: By frames).
            "Off: the queue holds Target frames (Queue target: By frames), so the delay changes with the frame rate.");
    QStringList parts;
    const struct { const char* name; double rate; } rates[] = {
        { "24p", 24000.0 / 1001.0 }, { "25p", 25.0 }, { "30p", 30000.0 / 1001.0 },
        { "50p", 50.0 }, { "60p", 60000.0 / 1001.0 } };
    for (const auto& rate : rates)
    {
        const size_t frames = QueueLatency::FramesForLatency(milliseconds, rate.rate);
        parts << QStringLiteral("%1 %2 frames (%3 ms)").arg(QString::fromLatin1(rate.name))
            .arg(frames).arg(qRound(QueueLatency::EffectiveLatencyMs(frames, rate.rate)));
    }
    return QStringLiteral(
        "Real delay, in whole frames: %1. Set the audio delay once to match. "
        "A profile's own Target frames (like Low latency's 1) still wins over a delay it would inherit.")
        .arg(parts.join(QStringLiteral(", ")));
}

// After a save the document holds every shortcut in canonical spelling; show
// the same in the shortcut fields without marking anything edited (second
// pass, audit 2026-10-16).
void showCanonicalShortcutSpelling(QWidget* root)
{
    if (!root) return;
    for (QLineEdit* edit : root->findChildren<QLineEdit*>())
    {
        const QString name = edit->objectName();
        if (!name.startsWith(QStringLiteral("config.shortcuts.")) &&
            !name.endsWith(QStringLiteral(".shortcut")) &&
            !name.endsWith(QStringLiteral(".cycle_shortcut")))
            continue;
        const QString canonical = canonicalShortcutText(edit->text());
        if (canonical == edit->text()) continue;
        const QSignalBlocker blocker(edit);
        edit->setText(canonical);
    }
}

// Hunt 2 (L6): a number field whose validator lets Intermediate input through
// (0.3 under a 0.5 minimum) is clamped and rounded when it is left, as the
// Picture fields are, so an out-of-range value is never stored. An inherited
// or empty field is left alone.
// A number field that moves in fixed steps (detail_pulse_cap, texture_restore_cap): on leaving,
// the clamped value snaps to the nearest step from the minimum. Connected after clampNumberFieldOnLeave.
void snapNumberFieldToStep(QLineEdit* edit, double lo, double hi, double step)
{
    QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, lo, hi, step]() {
        if (edit->property("inherited").toBool()) return;
        const QString text = edit->text().trimmed();
        if (text.isEmpty()) return;
        bool ok = false;
        double value = QLocale::c().toDouble(text, &ok);
        if (!ok) value = text.toDouble(&ok);
        if (!ok) return;
        value = (std::min)(hi, (std::max)(lo, lo + std::round((value - lo) / step) * step));
        const QString clean = QString::number(std::round(value * 100.0) / 100.0, 'g', 12);
        if (clean != edit->text()) edit->setText(clean);
    });
}

void clampNumberFieldOnLeave(QLineEdit* edit, double lo, double hi, int decimals)
{
    QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, lo, hi, decimals]() {
        if (edit->property("inherited").toBool()) return;
        const QString text = edit->text().trimmed();
        if (text.isEmpty() || text.compare(QStringLiteral("Auto"), Qt::CaseInsensitive) == 0) return;
        bool ok = false;
        double value = QLocale::c().toDouble(text, &ok);
        if (!ok) value = text.toDouble(&ok);
        if (!ok) { edit->clear(); return; }
        value = (std::min)(hi, (std::max)(lo, value));
        const double scale = std::pow(10.0, decimals);
        value = std::round(value * scale) / scale;
        const QString clean = QString::number(value, 'g', 12);
        if (clean != edit->text()) edit->setText(clean);
    });
}

// Hunt 3 (CFG3): a retired choice value the renderer still reads, and the
// current value it maps to (empty when `value` is not a retired one). The
// renderer's own mapping: LibplaceboVideoRenderer's dejudder_even_rates read.
QString retiredChoiceTarget(const QString& key, const QString& value)
{
    if (key == QStringLiteral("dejudder_even_rates"))
    {
        if (value.compare(QStringLiteral("30"), Qt::CaseInsensitive) == 0) return QStringLiteral("whole");
        if (value.compare(QStringLiteral("30_and_29.97"), Qt::CaseInsensitive) == 0) return QStringLiteral("whole_and_1001");
    }
    return {};
}

// Audit 2026-10-16: exclusive buttons over a hidden drop-down. The buttons show the effective
// value (inherited from the default profile, or the built-in default), marked as such; a
// "Not set" button returns the profile to the drop-down's unspecified item (the key removed).
QToolButton* addChoiceUnsetButton(QComboBox* combo, QBoxLayout* layout, QObject* context)
{
    auto* unset = new QToolButton;
    unset->setObjectName(combo->objectName() + QStringLiteral(".unset"));
    unset->setText(QStringLiteral("Not set"));
    unset->setToolTip(QStringLiteral(
        "Removes this profile's choice: the default profile's choice (or the built-in default) applies."));
    layout->addWidget(unset);
    QObject::connect(unset, &QToolButton::clicked, context, [combo]() {
        if (combo->count() > 0 && combo->itemData(0).toString().isEmpty()) combo->setCurrentIndex(0);
    });
    return unset;
}

void markChoiceButtons(const QComboBox* combo, const QList<QRadioButton*>& radios, QToolButton* unset)
{
    const bool unspecified = combo->currentData().toString().isEmpty();
    // "(inherited)" only when the default profile sets the value; otherwise it
    // is VP's own default (audit 2026-10-17).
    const bool fromDefaultProfile = combo->property("inheritedFromDefault").isValid() ?
        combo->property("inheritedFromDefault").toBool() : combo->property("inherited").toBool();
    const QString suffix = !unspecified ? QString() :
        fromDefaultProfile ? QStringLiteral(" (inherited)") : QStringLiteral(" (built-in default)");
    for (QRadioButton* radio : radios)
    {
        if (!radio->property("baseText").isValid()) radio->setProperty("baseText", radio->text());
        radio->setText(radio->property("baseText").toString() + (radio->isChecked() ? suffix : QString()));
    }
    if (unset)
        unset->setVisible(!unspecified && combo->count() > 0 && combo->itemData(0).toString().isEmpty());
}

// One //!PARAM block of an mpv-style GLSL shader.
struct GlslShaderParameter
{
    QString name;
    QString description;
    bool integer = false;
    bool hasMinimum = false;
    bool hasMaximum = false;
    double minimum = 0.0;
    double maximum = 0.0;
    double defaultValue = 0.0;
};

// The numeric //!PARAM blocks of a shader file; found is false when the file
// cannot be read. ENUM parameters (non-numeric defaults) are skipped.
QList<GlslShaderParameter> readGlslShaderParameters(const QString& path, bool& found)
{
    QList<GlslShaderParameter> parameters;
    QFile file(path);
    found = file.open(QIODevice::ReadOnly | QIODevice::Text);
    if (!found) return parameters;
    GlslShaderParameter current;
    bool inParameter = false;
    for (QString line : QString::fromUtf8(file.readAll()).split(u'\n'))
    {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("//!")))
        {
            const QString header = line.mid(3);
            const int space = header.indexOf(u' ');
            const QString key = (space < 0 ? header : header.left(space)).toUpper();
            const QString argument = space < 0 ? QString() : header.mid(space + 1).trimmed();
            if (key == QStringLiteral("PARAM"))
            {
                current = GlslShaderParameter();
                current.name = argument;
                inParameter = !argument.isEmpty();
            }
            else if (!inParameter) continue;
            else if (key == QStringLiteral("DESC")) current.description = argument;
            else if (key == QStringLiteral("TYPE"))
                current.integer = !argument.contains(QStringLiteral("float"), Qt::CaseInsensitive);
            else if (key == QStringLiteral("MINIMUM"))
                current.minimum = argument.toDouble(&current.hasMinimum);
            else if (key == QStringLiteral("MAXIMUM"))
                current.maximum = argument.toDouble(&current.hasMaximum);
            else inParameter = false;
            continue;
        }
        if (!inParameter || line.isEmpty()) continue;
        bool numeric = false;
        current.defaultValue = line.toDouble(&numeric);
        if (numeric) parameters.push_back(current);
        inParameter = false;
    }
    return parameters;
}
} // namespace
