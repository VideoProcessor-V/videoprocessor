// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// What the Dynamic Drive page builders (led_compensation/config_ui) and the projector cards
// (projector_control/config_ui) share: the editor's own field helpers reached through the EditorKit
// (ConfigPageHost.h), the document write path, and the number boxes of the drive settings.
//
// The page code was the Config editor's createLedDrivePage; it moved here line for line, so these
// members keep the names and meaning the editor's members had (value, controlName, helpLabel,
// createCard, bind*Field, markDirty with the sender(), numberControl, document_). A builder is a
// QObject that serves as the receiver of its widgets' connections, exactly as the editor was.
// UI thread only.

#include <VideoProcessor-Config/ConfigPageHost.h>

#include <QObject>
#include <QString>
#include <QStringList>

#include <string>

class QAbstractSpinBox;
class QCheckBox;
class QDoubleSpinBox;

namespace Vpv
{
namespace DriveConfig
{
	// Audit 2026-10-17 (LED number boxes): enough decimals to show a stored value as it is, so touching
	// the box does not round it (at most 6).
	int ledDisplayDecimals(const QString& raw, int decimals);

	// A stored value outside the control's range is shown clamped: flag the box (border, tool tip) and
	// remember it for the page's note.
	void flagClampedLedValue(QAbstractSpinBox* spin, const QString& raw, double minimum, double maximum);

	// The editor's unbound check box (set from the stored text, disabled until the caller enables it).
	QCheckBox* configuredCheck(const QString& text, const QString& value, bool defaultValue = false);

	// "led_floor" -> "Led floor".
	QString accessibleSettingName(QString key);

	// The write path of the editor's document, as the page code calls it (document_->SetKnown / AddSection /
	// RemoveKnown): the EditorKit adds a missing section itself, so SetKnown always succeeds.
	class DocumentWriter
	{
	public:
		explicit DocumentWriter(ConfigPageHost::EditorKit& kit) : kit_(kit) {}
		bool SetKnown(const std::string& section, const char* key, const std::string& text)
		{ kit_.setKnown(QString::fromStdString(section), QString::fromLatin1(key), QString::fromLocal8Bit(text.c_str())); return true; }
		void AddSection(const std::string&) {}
		void RemoveKnown(const std::string& section, const char* key)
		{ kit_.removeKnown(QString::fromStdString(section), QString::fromLatin1(key)); }
	private:
		ConfigPageHost::EditorKit& kit_;
	};

	class DriveConfigBase : public QObject
	{
	public:
		explicit DriveConfigBase(ConfigPageHost::EditorKit& kit) : kit_(kit), writer_(kit), document_(&writer_) {}

	protected:
		ConfigPageHost::EditorKit& kit_;
		DocumentWriter writer_;
		DocumentWriter* document_;

		QString value(const QString& section, const QString& key) const { return kit_.value(section, key, QString()); }
		QString controlName(const QString& section, const QString& key) const { return kit_.controlName(section, key); }
		QLabel* helpLabel(const QString& text) { return kit_.helpLabel(text); }
		QWidget* createCard(const QString& title, const QString& description, QWidget* content)
		{ return kit_.createCard(title, description, content); }
		QLineEdit* bindTextField(const QString& section, const QString& key, const QString& fallback = QString())
		{ return kit_.bindTextField(section, key, fallback); }
		QComboBox* bindChoiceField(const QString& section, const QString& key, const QStringList& values,
			const QStringList& labels = QStringList())
		{ return kit_.bindChoiceField(section, key, values, labels); }
		QCheckBox* bindCheckField(const QString& label, const QString& section, const QString& key, bool defaultValue = false)
		{ return kit_.bindCheckField(label, section, key, defaultValue); }

		// The editor's markDirty() takes the editing control from sender(); the receiver of the page's
		// connections is this object, so sender() is the control that fired, as it was for the editor.
		void markDirty() { kit_.markDirty(sender()); }

		// A child of the editor window (null when the kit has no window).
		template <class T>
		T editorChild(const QString& name) const
		{ QWidget* window = kit_.window(); return window ? window->findChild<T>(name) : nullptr; }

		// A number box for [section] key: range, decimals shown as stored, a special value that removes
		// the key, the clamp flag, and the write path.
		QDoubleSpinBox* numberControl(const QString& section, const QString& key, double minimum, double maximum,
			int decimals, double step, double fallback, const QString& special);
	};
}
}
