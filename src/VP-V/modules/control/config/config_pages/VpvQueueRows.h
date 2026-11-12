// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// VP-V's rows of the Queue profile page (P8 step 2, row c, RESTORED-CFG-04; owner rule 2026-11-01: Bill's
// pages stay exactly his). They are built by Bill's own field helpers (addChoice / addInteger: same keys,
// object names, inheritance and save path, bound to the queue profile selected on his Queue page) into a
// form of their own, which the VP-V page shows as a card (On both pathways) with a picker that follows his
// profile list:
//   VP's low-latency path (low_latency_path), Target delay (target_latency_ms) with its note, the
//   "Queue target" By frames / By time choice (target_mode.*; it shows or hides the Target delay row of
//   this card only - Bill's Target frames stays in his Advanced section, always shown), the look-ahead
//   note, and VP-V's Active-picture lookahead with Auto (config.queue.active_picture_lookahead_frames.vpv,
//   -1..8; stored -1 / "auto" / absent = follows the queue) with the explicit-0 notice.
// Bill's own look-ahead control (0..8, his loader's display: Auto shows as 0) stays on his page; the two
// follow each other (either/or, DIV-D row in docs/design/upstream-divergence-register.md). Also the
// loader / preset hooks of these rows (one VPV-HOOK line each in ConfigEditorWindow.cpp). UI thread
// only; C++14.

#include <QAction>
#include <QButtonGroup>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QObject>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QString>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <ConfigFile.h>
#include <QueueLookahead.h>
#include <VP-V/modules/control/config/config_pages/VpvProfileUseRows.h>
#include <VP-V/modules/control/config/config_pages/VpvSection.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace Vpv
{
	namespace QueueRowsDetail
	{
		inline QString LookaheadKey() { return QStringLiteral("active_picture_lookahead_frames"); }
		inline QString VpvLookaheadName() { return QStringLiteral("config.queue.active_picture_lookahead_frames.vpv"); }
		inline QString NoticeName() { return QStringLiteral("config.queue.active_picture_lookahead_notice"); }
		inline QString LowLatencyPathName() { return QStringLiteral("config.queue.low_latency_path"); }
		// The Target delay spin box carries its card's refresh action (By frames / By time and the note).
		const char* const kRefreshProperty = "vpvQueueTargetRefresh";

		inline bool IsVpvLookahead(const QWidget* widget)
		{
			return widget && widget->objectName() == VpvLookaheadName();
		}

		// The explicit-0 notice sits on the same card as VP-V's look-ahead (found through the top-level
		// widget: the card body before the VP-V page takes it, the editor window after).
		inline QLabel* NoticeNear(QWidget* widget)
		{
			QWidget* top = widget ? widget->window() : nullptr;
			return top ? top->findChild<QLabel*>(NoticeName()) : nullptr;
		}

		// VP-V's look-ahead and Bill's 0..8 control of the same key show each other's value; the edited
		// control's own handler writes the document, the mirrored one is set with its signals blocked, so
		// nothing is written twice and Bill's displayed 0 for Auto is never written back. VP-V -> Bill only
		// when VP-V's control is enabled and the value Bill's control would show differs (Auto = -1 shows as
		// his minimum, 0, the way his loader shows an absent or "auto" key). Bill -> VP-V only when his value
		// is not already what VP-V's value shows on his control (so a load leaves VP-V's Auto alone).
		inline void FollowLookahead(QSpinBox* vpv, QSpinBox* bills)
		{
			if (!vpv || !bills) return;
			QObject::connect(vpv, qOverload<int>(&QSpinBox::valueChanged), vpv, [vpv, bills](int value)
			{
				if (!vpv->isEnabled()) return;
				const int shown = (std::max)(value, bills->minimum());
				if (bills->value() == shown) return;
				const QSignalBlocker blocker(bills);
				bills->setValue(shown);
			});
			QObject::connect(bills, qOverload<int>(&QSpinBox::valueChanged), vpv, [vpv, bills](int value)
			{
				if ((std::max)(vpv->value(), bills->minimum()) == value) return;
				const QSignalBlocker blocker(vpv);
				vpv->setValue(value);
			});
		}

		template <class Fields>
		QWidget* FieldWidget(const Fields& fields, const QString& objectName)
		{
			for (const auto& field : fields)
				if (field.widget && field.widget->objectName() == objectName) return field.widget;
			return nullptr;
		}
	}

	// Called once by the Queue profile page at the end of Bill's queue rows. `form` is his form variable
	// (restored before return); `fields` his field list (its last entry is the control addInteger made);
	// `list` his queue profile list. The card goes to `cards` (unit "both").
	template <class AddChoice, class AddInteger, class Fields>
	void AddVpvQueueRows(QFormLayout*& form, AddChoice& addChoice, AddInteger& addInteger, Fields& fields,
		QListWidget* list, const std::function<QString(int)>& latencyNoteText, int maxLatencyMs,
		const std::function<QLabel*(const QString&)>& helpLabel, std::vector<UnitCard>& cards)
	{
		using namespace QueueRowsDetail;
		QFormLayout* const bills = form;
		QWidget* billsRoot = bills ? bills->parentWidget() : nullptr;
		while (billsRoot && billsRoot->parentWidget()) billsRoot = billsRoot->parentWidget();
		const auto help = [&helpLabel](const QString& text) -> QLabel*
		{
			QLabel* label = helpLabel ? helpLabel(text) : nullptr;
			if (!label) { label = new QLabel(text); label->setWordWrap(true); }
			return label;
		};

		auto* body = new QWidget;
		auto* column = new QVBoxLayout(body);
		column->setContentsMargins(0, 0, 0, 0);
		column->setSpacing(8);
		QComboBox* picker = nullptr;
		if (list)
		{
			auto* pickerRow = new QHBoxLayout;
			pickerRow->addWidget(new QLabel(QStringLiteral("Profile")));
			picker = new QComboBox;
			picker->setObjectName(QStringLiteral("vpv.both.queue_target.profile"));
			picker->setAccessibleName(QStringLiteral("Queue profile"));
			picker->setToolTip(QStringLiteral("The queue profile these settings edit; the same profile is selected on the Queue page."));
			pickerRow->addWidget(picker, 1);
			column->addLayout(pickerRow);
			ProfileUseDetail::FollowList(picker, list);
		}
		auto* formHost = new QWidget;
		auto* queueForm = new QFormLayout(formHost);
		queueForm->setContentsMargins(0, 0, 0, 0);
		queueForm->setVerticalSpacing(8);
		column->addWidget(formHost);
		form = queueForm;

		// Owner 2026-10-16: VP's low-latency path as a property of the queue profile (unset: on for a
		// profile named Low_Latency, as before).
		auto* lowLatencyPath = addChoice(QStringLiteral("VP's low-latency path"), QStringLiteral("low_latency_path"),
			{ QStringLiteral("on"), QStringLiteral("off") });
		lowLatencyPath->setToolTip(QStringLiteral(
			"On: with this queue VP takes its low-latency path - the de-judder off, no motion computed ahead, "
			"cinema-only shaders skipped, and early upload with 'Upload frames as they arrive: Auto' (full 32-bit "
			"precision is kept). Off: the regular path. Not set: on only for a queue named Low_Latency."));
		// Stage 5: the queue target as time. VP turns it into Target frames
		// for each source rate, so the delay - and the audio offset - stays
		// about the same at 24p, 30p and 60p.
		addInteger(QStringLiteral("Target delay"), QStringLiteral("target_latency_ms"),
			0, maxLatencyMs, QStringLiteral("ms"));
		auto* latencySpin = qobject_cast<QSpinBox*>(fields->back().widget);
		latencySpin->setSpecialValueText(QStringLiteral("Off"));
		// Settings guide audit 2026-10-24 (ADR-0020): one total delay, 4 frames recommended.
		latencySpin->setToolTip(QStringLiteral(
			"The total delay from capture to screen: ONE value, the same at every frame rate, so the AVR's "
			"fixed audio delay always matches. Recommended: 267 ms (4 queued frames at 23.976p, the slowest "
			"rate). Shorter still works: the look-ahead features (motion ahead, Dynamic Drive, crop and "
			"subtitle look-ahead, hiccup repair) fall back to 3 or 2 frames. The VP-V page's Input tab shows "
			"the frames and padding at every rate. Off uses Target frames (Queue page, Advanced)."));
		auto* latencyNote = help(QString());
		latencyNote->setObjectName(QStringLiteral("config.queue.target_latency_note"));
		queueForm->addRow(QString(), latencyNote);
		const auto updateLatencyNote = [latencyNote, latencyNoteText](int milliseconds)
		{
			if (latencyNoteText) latencyNote->setText(latencyNoteText(milliseconds));
		};
		QObject::connect(latencySpin, qOverload<int>(&QSpinBox::valueChanged), latencyNote, updateLatencyNote);
		updateLatencyNote(0);
		// Owner 2026-10-16: the queue target as an either / or - by frames or by time. By frames writes the
		// time as off (also switching off a time target inherited from the default queue); by time keeps
		// target_frames (Bill's Target frames, Advanced section of the Queue page) as the fallback until the
		// source rate is known. Only this card's Target delay row is shown or hidden.
		{
			auto* modeButtons = new QWidget;
			modeButtons->setObjectName(QStringLiteral("config.queue.target_mode"));
			auto* modeLayout = new QHBoxLayout(modeButtons);
			modeLayout->setContentsMargins(0, 0, 0, 0);
			auto* byFrames = new QRadioButton(QStringLiteral("By frames"));
			byFrames->setObjectName(QStringLiteral("config.queue.target_mode.frames"));
			byFrames->setToolTip(QStringLiteral(
				"A fixed number of source frames (Target frames, Queue page > Advanced): the delay changes with the frame rate."));
			auto* byTime = new QRadioButton(QStringLiteral("By time"));
			byTime->setObjectName(QStringLiteral("config.queue.target_mode.time"));
			byTime->setToolTip(QStringLiteral(
				"A delay in ms, turned into whole source frames for each frame rate, so the delay (and the audio offset) "
				"stays about the same at 24p, 30p and 60p."));
			auto* group = new QButtonGroup(modeButtons);
			group->setExclusive(true);
			group->addButton(byFrames);
			group->addButton(byTime);
			modeLayout->addWidget(byFrames);
			modeLayout->addWidget(byTime);
			modeLayout->addStretch(1);
			// Rows so far: low-latency path, Target delay, its note; the choice goes above Target delay.
			queueForm->insertRow(queueForm->rowCount() - 2, QStringLiteral("Queue target"), modeButtons);
			auto* lookAheadNote = help(QStringLiteral(
				"With the optical-flow look-ahead on (de-judder profiles), the queue holds at least 5 frames either way "
				"(4 ahead); the low-latency queue is exempt."));
			lookAheadNote->setObjectName(QStringLiteral("config.queue.target_lookahead_note"));
			queueForm->addRow(QString(), lookAheadNote);
			// addInteger puts the spin box in a row widget with its unit: that row widget is the form's field.
			QWidget* latencyRow = latencySpin->parentWidget() && latencySpin->parentWidget() != formHost ?
				latencySpin->parentWidget() : latencySpin;
			auto show = [queueForm, latencySpin, latencyRow, latencyNote, byFrames, byTime]() {
				const bool time = latencySpin->value() > 0;
				{
					const QSignalBlocker a(byFrames), b(byTime);
					byTime->setChecked(time);
					byFrames->setChecked(!time);
				}
				queueForm->setRowVisible(latencyRow, time);
				queueForm->setRowVisible(latencyNote, time);
			};
			QObject::connect(byTime, &QRadioButton::toggled, modeButtons, [latencySpin, show](bool on) {
				if (on && latencySpin->value() == 0) latencySpin->setValue(200);
				show();
			});
			QObject::connect(byFrames, &QRadioButton::toggled, modeButtons, [latencySpin, show](bool on) {
				if (on && latencySpin->value() != 0) latencySpin->setValue(0);
				show();
			});
			QObject::connect(latencySpin, qOverload<int>(&QSpinBox::valueChanged), modeButtons, [show](int) { show(); });
			// Triggered after a profile load and a buffering preset (both set the spin box with its signals
			// blocked or unchanged): the buttons, the row and the note follow its value.
			auto* refresh = new QAction(modeButtons);
			refresh->setObjectName(QStringLiteral("config.queue.target_mode_refresh"));
			// The picker also shows the loaded profile (a list change made under blocked signals reaches it here).
			QObject::connect(refresh, &QAction::triggered, modeButtons, [show, latencySpin, updateLatencyNote, picker, list]() {
				updateLatencyNote(latencySpin->value());
				show();
				if (picker && list && picker->count() == list->count() && picker->currentIndex() != list->currentRow())
				{
					const QSignalBlocker blocker(picker);
					picker->setCurrentIndex(list->currentRow());
				}
			});
			latencySpin->setProperty(kRefreshProperty, QVariant::fromValue<QObject*>(refresh));
			show();
		}

		// VP-V's look-ahead with Auto (owner decision 2026-10-17: the look-ahead follows the queue when the
		// key is absent). Bill's own control of the key (0..8) stays in his Advanced section.
		addInteger(QStringLiteral("Active-picture lookahead"), LookaheadKey(), -1, 8, QStringLiteral("frames"));
		auto* lookahead = qobject_cast<QSpinBox*>(fields->back().widget);
		if (QWidget* row = lookahead->parentWidget())
			if (auto* unit = row->findChild<QLabel*>(lookahead->objectName() + QStringLiteral(".unit")))
				unit->setObjectName(VpvLookaheadName() + QStringLiteral(".unit"));
		lookahead->setObjectName(VpvLookaheadName());
		lookahead->setSpecialValueText(QStringLiteral("Auto"));
		lookahead->setToolTip(QStringLiteral(
			"Auto (default; the key is absent): the look-ahead follows the queue. A normal queue profile uses "
			"min(target frames - 1, queue size - 1, 8) queued future frames; a low-latency profile uses 0. "
			"A number from 0 to 8 overrides it, including 0 (no look-ahead). Raising it never enlarges the "
			"queue. (Key: active_picture_lookahead_frames.) Bill's Active-picture lookahead (Queue page > "
			"Advanced, 0..8) is the same key: it shows Auto as 0."));
		// Bug hunt 3 F8: an explicit 0 (older queue presets saved one) keeps the automatic
		// look-ahead off; shown, never migrated (the owner decides).
		auto* lookaheadNotice = help(QString::fromLatin1(QueueLookahead::kExplicitZeroLookaheadNotice));
		lookaheadNotice->setObjectName(NoticeName());
		lookaheadNotice->setVisible(false);
		queueForm->addRow(QString(), lookaheadNotice);
		if (billsRoot)
			FollowLookahead(lookahead, billsRoot->findChild<QSpinBox*>(QStringLiteral("config.queue.active_picture_lookahead_frames")));
		form = bills;
		cards.push_back({ QStringLiteral("both"), QStringLiteral("queue_target"), QStringLiteral("Queue target and low latency"),
			QStringLiteral("VP-V's queue settings: the low-latency path, the target as a delay, and the automatic look-ahead. "
				"Bill's queue values (depth, lead, pre-roll, Target frames, look-ahead, reset) are on the Queue page."), body });
	}

	// ---- Loader and preset hooks (createProfilePage) ----

	// VP-V's built-in values of its queue keys (before Bill's queue fallbacks): no delay, look-ahead Auto.
	// Empty = not VP-V's (Bill's fallback applies). Bill's own look-ahead control reads "auto" as 0.
	inline QString QueueFallback(bool originalPathway, const QString& key)   // VPV-GATED
	{
		if (originalPathway) return QString();   // Original: Bill's own fallbacks (his look-ahead shows 0)
		if (key == QStringLiteral("target_latency_ms")) return QStringLiteral("0");
		if (key == QueueRowsDetail::LookaheadKey()) return QStringLiteral("auto");
		return QString();
	}

	// VP never inherits low_latency_path from the default queue (RendererProfileConfig: unset = on only for
	// the name Low_Latency); second pass, audit 2026-10-16.
	inline bool QueueOwnOnlyField(bool originalPathway, const QString& sectionPrefix, const QString& key)   // VPV-GATED
	{
		if (originalPathway) return false;
		return sectionPrefix == QStringLiteral("queue") && key == QStringLiteral("low_latency_path");
	}

	// Stage 5, as VP reads it: a queue's own Target frames switches off a delay it would inherit, so
	// Target delay shows Off rather than the default's. `ownFrames`: the profile sets target_frames (or its
	// legacy alias steady_reserve_frames).
	inline bool QueueDelayShowsOff(bool originalPathway, const QString& sectionPrefix, bool defaultProfile, const QString& raw,
		const QString& key, const std::function<bool()>& ownFrames)   // VPV-GATED
	{
		if (originalPathway) return false;
		return sectionPrefix == QStringLiteral("queue") && !defaultProfile && raw.isEmpty() &&
			key == QStringLiteral("target_latency_ms") && ownFrames && ownFrames();
	}

	// The value an Integer field shows: Bill's toInt; VP-V's look-ahead shows absent / "auto" as Auto (-1)
	// and its explicit-0 notice (Bug hunt 3 F8: a saved explicit 0, own or inherited).
	inline int QueueLoadedInteger(bool originalPathway, QWidget* widget, const QString& configured)   // VPV-GATED
	{
		if (originalPathway) return configured.toInt();   // Original: Bill's toInt, no notice
		bool numeric = false;
		const int number = configured.toInt(&numeric);
		if (!QueueRowsDetail::IsVpvLookahead(widget)) return configured.toInt();
		if (QLabel* notice = QueueRowsDetail::NoticeNear(widget))
			notice->setVisible(QueueLookahead::ExplicitZeroLookahead(configured.toStdString()));
		return numeric ? number : -1;
	}

	// An Integer field was edited (state not loading): the explicit-0 notice follows an edit of either
	// look-ahead control; true = VP-V's Auto was chosen, the caller removes the key (an absent key, never a
	// written number).
	inline bool QueueIntegerEdited(bool originalPathway, QWidget* window, const QString& key, int selected)   // VPV-GATED
	{
		if (originalPathway) return false;   // Original: Bill's own save of the value
		if (key != QueueRowsDetail::LookaheadKey()) return false;
		if (QLabel* notice = window ? window->findChild<QLabel*>(QueueRowsDetail::NoticeName()) : nullptr)
			notice->setVisible(selected == 0);
		return selected < 0;
	}

	// The "Not set" text of an unset low-latency path (audit 2026-10-16), as VP decides it: on only when the
	// profile's name normalises to low_latency.
	inline QString QueueLowLatencyUnsetText(bool originalPathway, const QString& section)   // VPV-GATED
	{
		if (originalPathway) return QStringLiteral("Not set");
		const QString queueName = section.startsWith(QStringLiteral("queue."), Qt::CaseInsensitive) ? section.mid(6) : QString();
		const bool namedLowLatency = !queueName.isEmpty() && ConfigFile::NormalizeName(queueName.toStdString()) == "low_latency";
		return namedLowLatency ? QStringLiteral("Not set: on (the Low Latency queue)") :
			QStringLiteral("Not set: off (on only for the Low Latency queue)");
	}

	// After a profile load: the queue target's mode, row and note follow the loaded Target delay.
	template <class Fields>
	void RefreshQueueRows(bool originalPathway, const Fields& fields)   // VPV-GATED
	{
		if (originalPathway) return;
		for (const auto& field : fields)
			if (field.widget)
				if (auto* refresh = qobject_cast<QAction*>(field.widget->property(QueueRowsDetail::kRefreshProperty).value<QObject*>()))
					refresh->trigger();
	}

	// Bill's buffering preset, after his own writes (he writes active_picture_lookahead_frames 0): VP-V's
	// preset is a frame-count baseline, so it switches Target delay off (target_latency_ms 0) and keeps the
	// look-ahead automatic (follows the queue): the explicit value is removed (Bug hunt 3 F8 / owner
	// decision 2026-10-17). The document ends exactly as before row c.
	template <class Document>
	void QueuePresetWrites(bool originalPathway, Document& document, const std::string& section)   // VPV-GATED
	{
		if (originalPathway) return;   // Original: Bill's preset writes Bill's keys only (look-ahead 0)
		document.SetKnown(section, "target_latency_ms", "0");
		document.RemoveKnown(section, "active_picture_lookahead_frames");
	}

	// After Bill's buffering preset wrote its values (VP-V's look-ahead set to Auto by the preset loop):
	// Low latency also takes VP's low-latency path, whatever the profile is called (audit 2026-10-16: on
	// another name the look-ahead minimum raised a 1-frame queue's target to 5); the others return it to
	// Not set (the Low_Latency name rule), so a deep queue never stays on it - on a profile named
	// Low_Latency "Not set" means on, so the other presets write off there. Then the queue target follows.
	template <class Fields>
	void QueuePresetApplied(bool originalPathway, const Fields& fields, const QString& section, const QString& presetId)   // VPV-GATED
	{
		if (originalPathway) return;
		if (auto* path = qobject_cast<QComboBox*>(QueueRowsDetail::FieldWidget(fields, QueueRowsDetail::LowLatencyPathName())))
		{
			const bool namedLowLatency = section.compare(QStringLiteral("queue.low_latency"), Qt::CaseInsensitive) == 0;
			const int index = presetId == QStringLiteral("low_latency") ? path->findData(QStringLiteral("on")) :
				(namedLowLatency ? path->findData(QStringLiteral("off")) : 0);
			if (index >= 0 && path->currentIndex() != index) path->setCurrentIndex(index);
		}
		RefreshQueueRows(originalPathway, fields);
	}
}
