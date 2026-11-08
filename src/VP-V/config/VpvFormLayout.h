// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Config editor layout helpers for VP-V rows and combos inside Bill's pages (FX-CFG14; moved out of
// ConfigEditorWindow.cpp so his file keeps call sites only). Layout only: no value, key or signal changes.

#include <QComboBox>
#include <QFormLayout>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace Vpv
{
	// VP-V's DeckLink / DirectShow rows have long labels. They sit in a form of their own (never wrapping),
	// appended as one row of the parent form, so they do not widen Bill's label column and wrap his Capture
	// device row (the card then grew taller than the grid's size hint and the two cards of a row did not align).
	inline QFormLayout* AddNestedVpvForm(QFormLayout* parent)
	{
		auto* form = new QFormLayout;
		form->setContentsMargins(0, 0, 0, 0);
		form->setHorizontalSpacing(18);
		form->setVerticalSpacing(8);
		form->setRowWrapPolicy(QFormLayout::DontWrapRows);
		parent->addRow(form);
		return form;
	}

	// A combo with long item names must not set its card's minimum width, or the two-column grid (which
	// switches at 720 px) overflows sideways on a narrow window. It shrinks to the width it is given instead.
	inline void MakeComboShrinkable(QComboBox* combo)
	{
		combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo->setMinimumContentsLength(0);
		combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

	// Every combo under `root` whose object name starts with one of `prefixes` becomes shrinkable
	// (Bill's own combos, e.g. Capture device, are left as he built them by not matching a prefix).
	inline void MakeCombosShrinkable(QWidget* root, const QStringList& prefixes)
	{
		for (QComboBox* combo : root->findChildren<QComboBox*>())
			for (const QString& prefix : prefixes)
				if (combo->objectName().startsWith(prefix))
				{
					MakeComboShrinkable(combo);
					break;
				}
	}
}
