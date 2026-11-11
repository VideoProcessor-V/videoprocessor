// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// VP-V's "Profile use" controls for the Queue and Rendering profile pages (P8 step 2, D2): the frame-rate
// rule ("use this profile for sources above N fps") and, for queues, "Use this queue: only when chosen /
// by rules / 50 Hz and faster sources". They used to sit in Bill's profile details; they now sit in a
// VP-V page card (On both pathways) with a profile picker that follows the profile selected on Bill's
// page and, when changed, selects that profile there. They write Bill's own rule editor ("When should VP
// use this profile?", key `when`) and his "Use rule" check, so the stored rule is exactly what his editor
// writes; any other formula is left alone and the controls step aside. Bill's rule editor keeps his
// placeholder (RESTORED-CFG-02). Object names are unchanged (config.<page>.rate_rule, rate_rule_fps,
// rate_rule_note, use_mode.*). UI thread only; C++14.

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QObject>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>
#include <VP-V/modules/control/config/config_pages/VpvSection.h>
#include <functional>
#include <vector>

namespace Vpv
{
	namespace ProfileUseDetail
	{
		inline QString Name(const QString& sectionPrefix, const QString& key)
		{
			return QStringLiteral("config.%1.%2").arg(sectionPrefix, key);
		}

		// The picker lists Bill's profiles (his list's item text) and keeps his selection.
		inline void FollowList(QComboBox* picker, QListWidget* list)
		{
			const auto rebuild = [picker, list]()
			{
				const QSignalBlocker blocker(picker);
				picker->clear();
				for (int row = 0; row < list->count(); ++row)
					picker->addItem(list->item(row)->text());
				picker->setCurrentIndex(list->currentRow());
			};
			rebuild();
			QAbstractItemModel* model = list->model();
			QObject::connect(model, &QAbstractItemModel::rowsInserted, picker, rebuild);
			QObject::connect(model, &QAbstractItemModel::rowsRemoved, picker, rebuild);
			QObject::connect(model, &QAbstractItemModel::rowsMoved, picker, rebuild);
			QObject::connect(model, &QAbstractItemModel::dataChanged, picker, rebuild);
			QObject::connect(model, &QAbstractItemModel::modelReset, picker, rebuild);
			QObject::connect(list, &QListWidget::currentRowChanged, picker, [picker](int row)
			{
				const QSignalBlocker blocker(picker);
				picker->setCurrentIndex(row);
			});
			QObject::connect(picker, qOverload<int>(&QComboBox::activated), list, [list](int row)
			{
				if (row >= 0 && row < list->count()) list->setCurrentRow(row);
			});
		}
	}

	// Called by the profile page for the Queue ("queue") and Rendering ("vprenderer") pages after Bill's
	// rule editor is built: `list` is his profile list, `rule` his rule editor, `useRule` his "Use rule"
	// check. The card goes to `cards` (unit "both": the VP-V page's On both pathways block).
	inline void AddVpvProfileUseRows(const QString& sectionPrefix, const QString& pageTitle, QListWidget* list,
		QPlainTextEdit* rule, QCheckBox* useRule, const std::function<QLabel*(const QString&)>& helpLabel,
		std::vector<UnitCard>& cards)
	{
		using namespace ProfileUseDetail;
		if (sectionPrefix != QStringLiteral("queue") && sectionPrefix != QStringLiteral("vprenderer")) return;
		if (!list || !rule || !useRule) return;
		const auto help = [&helpLabel](const QString& text) -> QLabel*
		{
			QLabel* label = helpLabel ? helpLabel(text) : nullptr;
			if (!label) { label = new QLabel(text); label->setWordWrap(true); }
			return label;
		};
		auto* body = new QWidget;
		auto* layout = new QVBoxLayout(body);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(8);
		{
			auto* pickerRow = new QHBoxLayout;
			pickerRow->addWidget(new QLabel(QStringLiteral("Profile")));
			auto* picker = new QComboBox;
			picker->setObjectName(QStringLiteral("vpv.both.%1.profile").arg(sectionPrefix));
			picker->setAccessibleName(QStringLiteral("%1 profile").arg(pageTitle));
			picker->setToolTip(QStringLiteral("The %1 profile these controls edit; the same profile is selected on the %1 page.").arg(pageTitle));
			pickerRow->addWidget(picker, 1);
			layout->addLayout(pickerRow);
			FollowList(picker, list);
		}

		// The common queue rule as a plain control: "use this queue above N
		// fps"; also for rendering profiles, e.g. a low-latency profile
		// (audit 2026-10-14c: no hand-written rule). It writes the same rule as the formula field and follows it;
		// any other formula is left alone and the control steps aside.
		auto* rateRow = new QWidget;
		auto* rateLayout = new QHBoxLayout(rateRow);
		rateLayout->setContentsMargins(0, 0, 0, 0);
		auto* rateRule = new QCheckBox(sectionPrefix == QStringLiteral("queue") ?
			QStringLiteral("Use this queue for sources above") : QStringLiteral("Use this profile for sources above"));
		rateRule->setObjectName(Name(sectionPrefix, QStringLiteral("rate_rule")));
		auto* rateThreshold = new QDoubleSpinBox;
		rateThreshold->setObjectName(Name(sectionPrefix, QStringLiteral("rate_rule_fps")));
		rateThreshold->setRange(1.0, 1000.0);
		rateThreshold->setDecimals(2);
		rateThreshold->setValue(59.0);
		rateThreshold->setSuffix(QStringLiteral(" fps"));
		rateThreshold->setMaximumWidth(120);
		rateLayout->addWidget(rateRule);
		rateLayout->addWidget(rateThreshold);
		rateLayout->addStretch();
		auto* rateNote = help(QStringLiteral(
			"This profile has its own rule on the %1 page; edit it there, or clear it to use the frame-rate switch.").arg(pageTitle));
		rateNote->setObjectName(Name(sectionPrefix, QStringLiteral("rate_rule_note")));
		rateNote->setVisible(false);
		if (sectionPrefix == QStringLiteral("queue"))
		{
			// Owner 2026-10-16: when this queue is used - Off (chosen by hand or shortcut only), By rules
			// (the formula), or video-rate sources.
			// Fixed-constant batch 2026-10-18 (owner [[vp-general-not-fixed]]): the button writes the
			// class rule - every source at the video rate floor and above (DejudderPolicy::VideoRateFloorHz,
			// 49 Hz: 50, 59.94, 60, 100, 119.88, 120, 144, 240 ...) - instead of the fixed 49-61 band,
			// which missed 100/120p. A saved 49-61 rule still reads as this button (and is kept as
			// written until the button is clicked again).
			static const QString kRate5060 = QStringLiteral("${cadence} >= 49");
			static const QString kRate5060Legacy = QStringLiteral("${cadence} == 49-61");
			const auto isRateRule = [](const QString& text) { return text == kRate5060 || text == kRate5060Legacy; };
			rateRow->setVisible(false);
			auto* useRow = new QWidget;
			useRow->setObjectName(Name(sectionPrefix, QStringLiteral("use_mode")));
			auto* useLayout = new QHBoxLayout(useRow);
			useLayout->setContentsMargins(0, 0, 0, 0);
			useLayout->addWidget(new QLabel(QStringLiteral("Use this queue:")));
			// audit 2026-10-16: "Off" read as the queue being disabled.
			auto* offButton = new QRadioButton(QStringLiteral("Only when chosen (shortcut or by hand)"));
			offButton->setObjectName(Name(sectionPrefix, QStringLiteral("use_mode.off")));
			auto* rulesButton = new QRadioButton(QStringLiteral("By rules"));
			rulesButton->setObjectName(Name(sectionPrefix, QStringLiteral("use_mode.rules")));
			auto* rateButton = new QRadioButton(QStringLiteral("50 Hz and faster sources"));
			rateButton->setObjectName(Name(sectionPrefix, QStringLiteral("use_mode.rate5060")));
			rateButton->setToolTip(QStringLiteral(
				"Writes the rule ${cadence} >= 49: every video-rate signal (50, 59.94, 60, 100, 120 Hz and faster), the "
				"same 50 Hz floor the film / video classification uses. Film sent in a 60 Hz signal (3:2) matches too "
				"until the cadence detection keeps it on the regular path. A rule saved as ${cadence} == 49-61 (50/60 Hz "
				"only) still shows here and is kept until this button is clicked again."));
			auto* group = new QButtonGroup(useRow);
			group->setExclusive(true);
			for (QRadioButton* b : { offButton, rulesButton, rateButton })
			{
				group->addButton(b);
				useLayout->addWidget(b);
			}
			useLayout->addStretch();
			layout->addWidget(useRow);
			layout->addWidget(help(QStringLiteral(
				"An empty rule is not saved: the queue is then used only when chosen. A hand-written rule is kept aside "
				"while one of the other two choices is on, and By rules brings it back.")));
			auto syncUse = [rule, useRule, offButton, rulesButton, rateButton, isRateRule]
			{
				const QString text = useRule->isChecked() ? rule->toPlainText().trimmed() : QString();
				const QSignalBlocker a(offButton), b(rulesButton), c(rateButton);
				offButton->setChecked(text.isEmpty() && !useRule->isChecked());
				rateButton->setChecked(isRateRule(text));
				rulesButton->setChecked(useRule->isChecked() && !isRateRule(text));
			};
			QObject::connect(rule, &QPlainTextEdit::textChanged, useRow, syncUse);
			QObject::connect(useRule, &QCheckBox::toggled, useRow, syncUse);
			// Audit 2026-10-16: a hand-written rule is kept aside when Off or 50/60 Hz replaces it, and
			// By rules brings it back.
			auto keepCustom = [rule, useRow, isRateRule]() {
				const QString text = rule->toPlainText().trimmed();
				if (!text.isEmpty() && !isRateRule(text)) useRow->setProperty("customRule", text);
			};
			QObject::connect(offButton, &QRadioButton::toggled, useRow, [rule, useRule, keepCustom](bool on) {
				if (!on) return;
				keepCustom();
				rule->clear();
				useRule->setChecked(false);
			});
			QObject::connect(rulesButton, &QRadioButton::toggled, useRow, [rule, useRule, useRow, isRateRule](bool on) {
				if (!on) return;
				const QString text = rule->toPlainText().trimmed();
				if (text.isEmpty() || isRateRule(text))
					rule->setPlainText(useRow->property("customRule").toString());
				useRule->setChecked(true);
			});
			QObject::connect(rateButton, &QRadioButton::toggled, useRow, [rule, useRule, keepCustom](bool on) {
				if (!on) return;
				keepCustom();
				rule->setPlainText(kRate5060);
				useRule->setChecked(true);
			});
			syncUse();
		}
		layout->addWidget(rateRow);
		layout->addWidget(rateNote);
		auto syncFromRule = [rule, useRule, rateRule, rateThreshold, rateNote]
		{
			QSignalBlocker blockRule(rateRule);
			QSignalBlocker blockThreshold(rateThreshold);
			const QString text = useRule->isChecked() ? rule->toPlainText().trimmed() : QString();
			// ${cadence} is the exact rate (59.94); ${source_rate} is a whole
			// number (59.94 reads as 59), so the switch writes cadence. Rules
			// written with source_rate are still recognised.
			const QRegularExpressionMatch match = QRegularExpression(QStringLiteral(
				"^\\$\\{(?:cadence|source_rate)\\}\\s*>\\s*([0-9]+(?:\\.[0-9]+)?)$")).match(text);
			const bool custom = !text.isEmpty() && !match.hasMatch();
			rateRule->setChecked(match.hasMatch());
			if (match.hasMatch()) rateThreshold->setValue(match.captured(1).toDouble());
			rateRule->setEnabled(!custom && useRule->isEnabled());
			rateThreshold->setEnabled(!custom && useRule->isEnabled());
			rateNote->setVisible(custom);
		};
		auto applyRateRule = [rule, useRule, rateRule, rateThreshold]
		{
			if (rateRule->isChecked())
			{
				rule->setPlainText(QStringLiteral("${cadence} > %1")
					.arg(QString::number(rateThreshold->value(), 'g', 6)));
				useRule->setChecked(true);
			}
			else
			{
				rule->clear();
				useRule->setChecked(false);
			}
		};
		QObject::connect(rule, &QPlainTextEdit::textChanged, rateRow, syncFromRule);
		QObject::connect(useRule, &QCheckBox::toggled, rateRow, syncFromRule);
		QObject::connect(rateRule, &QCheckBox::toggled, rateRow, applyRateRule);
		QObject::connect(rateThreshold, qOverload<double>(&QDoubleSpinBox::valueChanged), rateRow,
			[rateRule, applyRateRule](double) { if (rateRule->isChecked()) applyRateRule(); });

		cards.push_back({ QStringLiteral("both"), QStringLiteral("profile_use_") + sectionPrefix,
			QStringLiteral("Profile use (%1)").arg(pageTitle),
			QStringLiteral("When a %1 profile is used: plain controls that write the profile's rule on the %1 page.").arg(pageTitle), body });
	}
}
