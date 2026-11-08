// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared helpers of the Dynamic Drive page builders; moved out of ConfigEditorWindow.cpp (ledDisplayDecimals,
// flagClampedLedValue, configuredCheck, accessibleSettingName, and the createLedDrivePage numberControl lambda)
// unchanged.

#include "DriveConfigBase.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QDoubleSpinBox>

#include <algorithm>

namespace Vpv
{
namespace DriveConfig
{
// Audit 2026-10-17 (LED number boxes): enough decimals to show a stored
// value as it is, so touching the box does not round it (at most 6).
int ledDisplayDecimals(const QString& raw, int decimals)
{
    const QString text = raw.trimmed();
    bool valid = false;
    text.toDouble(&valid);
    const int point = text.indexOf(u'.');
    if (!valid || point < 0) return decimals;
    int digits = 0;
    for (int i = point + 1; i < text.size() && text[i].isDigit(); ++i) ++digits;
    return std::max(decimals, std::min(digits, 6));
}

// A stored value outside the control's range is shown clamped: flag the box
// (border, tool tip) and remember it for the page's note.
void flagClampedLedValue(QAbstractSpinBox* spin, const QString& raw, double minimum, double maximum)
{
    bool valid = false;
    const double stored = raw.trimmed().toDouble(&valid);
    if (!valid || (stored >= minimum && stored <= maximum)) return;
    const QString note = QStringLiteral("The saved value %1 is outside %2 to %3; it is shown as %4 and kept in the "
        "file until this box is changed.").arg(raw.trimmed()).arg(minimum).arg(maximum)
        .arg(stored < minimum ? minimum : maximum);
    spin->setProperty("clampedFrom", raw.trimmed());
    spin->setProperty("clampedNote", note);
    spin->setToolTip(spin->toolTip().isEmpty() ? note : spin->toolTip() + QStringLiteral("\n") + note);
    spin->setStyleSheet(QStringLiteral("border: 1px solid #e0a040;"));
}

namespace
{
// The editor's configuredBooleanValue (same words, same default for anything else).
bool configuredBooleanValue(const QString& value, bool defaultValue)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized.isEmpty()) return defaultValue;
    if (normalized == QStringLiteral("true") || normalized == QStringLiteral("yes") ||
        normalized == QStringLiteral("1") || normalized == QStringLiteral("on"))
        return true;
    if (normalized == QStringLiteral("false") || normalized == QStringLiteral("no") ||
        normalized == QStringLiteral("0") || normalized == QStringLiteral("off"))
        return false;
    return defaultValue;
}
}

QCheckBox* configuredCheck(const QString& text, const QString& value, bool defaultValue)
{
    auto* check = new QCheckBox(text);
    check->setTristate(false);
    check->setChecked(configuredBooleanValue(value, defaultValue));
    check->setEnabled(false);
    return check;
}

QString accessibleSettingName(QString key)
{
    key.replace(u'_', u' ');
    key.replace(u'.', u' ');
    if (!key.isEmpty()) key[0] = key[0].toUpper();
    return key;
}

// Stage 7 step 7c (control policy; the owner's correction): compensate's
// contrast assist, cuts, speeds and the projector's response. Every
// setting has a control; a speed set to "preset" is removed from the
// file so rate_preset applies.
QDoubleSpinBox* DriveConfigBase::numberControl(const QString& section, const QString& key, double minimum, double maximum,
    int decimals, double step, double fallback, const QString& special)
{
    auto* spin = new QDoubleSpinBox;
    spin->setObjectName(controlName(section, key));
    spin->setAccessibleName(accessibleSettingName(key));
    spin->setDecimals(ledDisplayDecimals(value(section, key), decimals));
    spin->setSingleStep(step);
    spin->setRange(special.isEmpty() ? minimum : minimum - step, maximum);
    if (!special.isEmpty()) spin->setSpecialValueText(special);
    spin->setMaximumWidth(120);
    spin->setAlignment(Qt::AlignRight);
    bool valid = false;
    const double configured = value(section, key).toDouble(&valid);
    spin->setValue(valid ? std::max(minimum, std::min(configured, maximum)) :
        (special.isEmpty() ? fallback : spin->minimum()));
    // A value below a special value's minimum is that special value (-1).
    if (special.isEmpty() || !valid || configured >= minimum)
        flagClampedLedValue(spin, value(section, key), minimum, maximum);
    connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
        [this, section, key, spin](double number)
    {
        if (!document_) return;
        const std::string name = section.toStdString();
        if (!spin->specialValueText().isEmpty() && number <= spin->minimum())
            document_->RemoveKnown(name, key.toStdString().c_str());
        else
        {
            const std::string text = QString::number(number, 'f', spin->decimals()).toStdString();
            if (!document_->SetKnown(name, key.toStdString().c_str(), text))
            {
                document_->AddSection(name);
                document_->SetKnown(name, key.toStdString().c_str(), text);
            }
        }
        markDirty();
    });
    return spin;
}
}
}
