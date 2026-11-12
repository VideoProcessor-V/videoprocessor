// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Bill's Queue page versus VP-V's queue settings (owner 2026-11-11: "our settings in our own sub menus so we're
// not overlapping them, greying out overlapping settings"; owner H 2026-11-10: Bill's pages stay his, his
// conflicting controls are GREYED with a pointer to the VP-V control, never moved or hidden).
//
// A Config page provider that uses only the generic OnBillField seam (the one existing hook call in
// ConfigEditorWindow.cpp; no Bill line is added). For each bound field of the Queue page, on every
// profile load / refresh:
//   VP-V pathway:
//     - Bill's "Active-picture lookahead" (0..8) is greyed with a pointer: VP-V's Auto look-ahead on the
//       VP-V page owns the same key (DIV-D-10);
//     - Bill's "Target frames" is greyed while the Target delay (target_latency_ms) is above 0 (the delay
//       is turned into frames per source rate; the frames value is only the fallback).
//   Original pathway: nothing of Bill's is greyed or changed (his page is his); VP-V's own queue controls
//     (low-latency path, Target delay, look-ahead Auto) are greyed because the Original pathway ignores them.
// The grey uses the "derivedSetting" property so the editor's profile reload keeps it. UI thread only; C++17.

#include <VP-V/modules/control/config/config_pages/VpvDitherChoiceSet.h>
#include <VideoProcessor-Config/ConfigPageHost.h>

#include <QSpinBox>
#include <QString>
#include <QWidget>

namespace Vpv
{
	namespace QueueGreyDetail
	{
		inline QString PointerText() { return QStringLiteral("Set on the VP-V page > Queue target and low latency"); }
		inline QString OriginalOffText() { return QStringLiteral("Not used in the Original pathway (Bill's queue is in use)."); }
		inline QString BillLookaheadName() { return QStringLiteral("config.queue.active_picture_lookahead_frames"); }
		inline QString VpvLookaheadName() { return QStringLiteral("config.queue.active_picture_lookahead_frames.vpv"); }
		inline QString TargetFramesName() { return QStringLiteral("config.queue.target_frames"); }
		inline QString TargetDelayName() { return QStringLiteral("config.queue.target_latency_ms"); }
		inline QString LowLatencyPathName() { return QStringLiteral("config.queue.low_latency_path"); }

		inline void SetNote(QWidget* widget, const QString& text, bool on)
		{
			QString tip = widget->toolTip();
			tip.remove(QLatin1Char('\n') + text);
			tip.remove(text);
			if (on) tip = tip.isEmpty() ? text : tip + QLatin1Char('\n') + text;
			widget->setToolTip(tip);
		}

		// Greys a control for one reason (property `flag`), with a note; clears only its own grey.
		inline void Grey(QWidget* widget, const char* flag, const QString& note, bool grey)
		{
			if (!widget) return;
			if (grey)
			{
				widget->setProperty(flag, true);
				widget->setProperty("derivedSetting", true);
				widget->setEnabled(false);
				SetNote(widget, note, true);
			}
			else if (widget->property(flag).toBool())
			{
				widget->setProperty(flag, false);
				widget->setProperty("derivedSetting", false);
				widget->setEnabled(true);
				SetNote(widget, note, false);
			}
		}
		inline void GreyBills(QWidget* widget, bool grey) { Grey(widget, "vpvQueueGrey", PointerText(), grey); }
		inline void OffInOriginal(QWidget* widget, bool original) { Grey(widget, "vpvQueueOriginalOff", OriginalOffText(), original); }

		inline QWidget* FindQueueWidget(QWidget* from, const QString& name)
		{
			QWidget* top = from ? from->window() : nullptr;
			return top ? top->findChild<QWidget*>(name) : nullptr;
		}
	}

	class QueueBillGrey final : public ConfigPageHost::IConfigPageProvider
	{
	public:
		void OnBillField(const QString& sectionPrefix, const QString& key, QWidget* widget,
			ConfigPageHost::ConfigFormApi& api) override
		{
			using namespace QueueGreyDetail;
			(void)key;
			if (!widget || sectionPrefix != QStringLiteral("queue")) return;
			const bool original = ConfigOriginalPathwayOn(
				[&api](const QString& s, const QString& k) { return api.value(s, k); });
			const QString name = widget->objectName();

			if (name == VpvLookaheadName() || name == LowLatencyPathName() || name == TargetDelayName())
				OffInOriginal(widget, original);
			if (name == BillLookaheadName())
				GreyBills(widget, !original);

			if (name == TargetDelayName())
			{
				auto* spin = qobject_cast<QSpinBox*>(widget);
				if (!spin) return;
				spin->setProperty("vpvQueueOriginal", original);
				if (!spin->property("vpvQueueGreyWired").toBool())
				{
					spin->setProperty("vpvQueueGreyWired", true);
					QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), spin, [spin](int value)
					{
						GreyBills(FindQueueWidget(spin, TargetFramesName()),
							!spin->property("vpvQueueOriginal").toBool() && value > 0);
					});
				}
			}
			else if (name == TargetFramesName())
			{
				auto* delay = qobject_cast<QSpinBox*>(FindQueueWidget(widget, TargetDelayName()));
				const QString section = widget->property("profileSection").toString();
				QString raw = section.isEmpty() ? QString()
					: api.value(section, QStringLiteral("target_latency_ms")).trimmed();
				if (raw.isEmpty()) raw = api.value(QStringLiteral("queue"), QStringLiteral("target_latency_ms")).trimmed();
				GreyBills(widget, !original && (delay ? delay->value() : raw.toInt()) > 0);
			}
		}
	};

	inline QueueBillGrey& QueueBillGreyInstance()
	{
		static QueueBillGrey instance;
		return instance;
	}
}
