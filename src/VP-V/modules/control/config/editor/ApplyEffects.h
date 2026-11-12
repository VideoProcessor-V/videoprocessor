// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Config editor: apply/save effects that exist only in VP-V (no Bill counterpart in a591ad3b5), moved
// byte-for-byte out of ConfigEditorWindow.cpp (P9 row d). Declarations stay in the class.
// Included by ConfigEditorWindow.cpp ONLY (through VpvRenderingDefaults.h): the member definitions are not
// inline, so a second including translation unit would be a duplicate definition.

#include <QPushButton>
#include <QToolButton>
#include <QString>
#include "ConfigEditorWindow.h"

void ConfigEditorWindow::setRemoteApplyBusy(bool busy)
{
    if (configurationHost_) configurationHost_->setEnabled(!busy);
    if (navigation_) navigation_->setEnabled(!busy);
    if (auto* cancel = findChild<QPushButton*>(QStringLiteral("cancelConfiguration")))
        cancel->setEnabled(!busy);
    // Second pass (audit 2026-10-16): no target switch or refresh mid-Apply.
    if (targetChoice_) targetChoice_->setEnabled(!busy);
    if (auto* refresh = findChild<QToolButton*>(QStringLiteral("config.refreshTargets")))
        refresh->setEnabled(!busy);
    if (busy)
    {
        if (applyButton_) applyButton_->setEnabled(false);
        if (saveButton_) saveButton_->setEnabled(false);
        return;
    }
    updateValidationState();
}
