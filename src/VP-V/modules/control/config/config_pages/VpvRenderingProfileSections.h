// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// VP-V's Rendering profile sections, moved out of Bill's ConfigEditorWindow.cpp into compiled VP-V
// code so ConfigEditorWindow.cpp holds only short call sites and the shared header adds zero TU lines.

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QBoxLayout;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QObject;
class QRadioButton;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace ConfigPageHost { class EditorKit; }

namespace Vpv
{
    struct RenderingSectionKit
    {
        QObject* receiver = nullptr;
        QString sectionPrefix;
        QFormLayout** form = nullptr;
        QWidget* profileFields = nullptr;
        QVBoxLayout* profileFieldsLayout = nullptr;
        std::function<QFormLayout*(const QString& id, const QString& heading, const QString& description, bool expanded)> addCollapsibleSection;
        std::function<QLineEdit*(const QString& label, const QString& key, const QString& unit, double displayScale)> addText;
        std::function<QComboBox*(const QString& label, const QString& key, const QStringList& choices, bool allowInherited)> addChoice;
    };

    struct RenderingSectionHandles
    {
        QComboBox* capturePath = nullptr;
        QComboBox* sourceRange = nullptr;
        QWidget* pictureBox = nullptr;
        QComboBox* sourceResolution = nullptr;
        QLineEdit* autoConfidence = nullptr;
        QLineEdit* evidenceSeconds = nullptr;
        QLineEdit* evidenceShots = nullptr;
        QComboBox* textGuard = nullptr;
        QComboBox* mixedGuard = nullptr;
        QComboBox* kernelMatch = nullptr;
        QComboBox* debandVp = nullptr;
        QComboBox* lowBitrate = nullptr;
        QComboBox* ssimStabilise = nullptr;
        QComboBox* bitrateAware = nullptr;
        QLineEdit* bitrateFloor = nullptr;
        QComboBox* cnnStabilise = nullptr;
    };

    void MarkGpuCost(QFormLayout* form, QWidget* control, int cost);
    const char* GpuCostLegend();

    void SnapNumberFieldToStep(QLineEdit* edit, double lo, double hi, double step);
    void ClampNumberFieldOnLeave(QLineEdit* edit, double lo, double hi, int decimals);
    QString RetiredChoiceTarget(const QString& key, const QString& value);
    QToolButton* AddChoiceUnsetButton(QComboBox* combo, QBoxLayout* layout, QObject* context);
    void MarkChoiceButtons(const QComboBox* combo, const QList<QRadioButton*>& radios, QToolButton* unset);

    RenderingSectionHandles AddVpvRenderingSections(const RenderingSectionKit& kit);
    void AddVpvProcessingAndReorganise(const RenderingSectionKit& kit, const RenderingSectionHandles& handles);
    void RefreshVpvRenderingProfileState(QWidget* editorRoot, QWidget* profileFields,
        const QString& sectionPrefix, const QString& whenValue);
    void AddStartupHardwareRows(QFormLayout* hardwareForm, ConfigPageHost::EditorKit& kit);
}
