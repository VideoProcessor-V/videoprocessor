// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Identity kit for the Config editor (doc 60 section 1.4; built in P7-0 of R1 W1, reused by P8, P9, P12).
//
//   VideoProcessorConfigTests.exe --identity <out.txt> [<extra.cfg> ...]
//
// is not a test: it opens a test-mode editor (off screen when QT_QPA_PLATFORM=offscreen, no contact
// with a running VP) on every file of a corpus, in the VP-V and in the Original pathway, and writes
// tab-separated lines that are compared by diff between a "before" build and an "after" build:
//   C  every control of every page (and, for a profile page, of every profile of each profile list):
//      page, ordinal, class, object name, ancestors, label/text, enabled, visible, tooltip hash,
//      choices, displayed value, vpvOwner
//   G  QWidget::grab() hash of every page and of every tab of every tab widget on it (1120x700)
//   D  document bytes (SHA-256) after load + load migrations + Apply, per corpus file
//   E  document bytes after one scripted edit per control of the Calibration and Dynamic Drive pages
// Timers are stopped right after construction so a status poll cannot change a label between runs.
// Controls are walked in tree order (creation order), which is deterministic.

#include "ConfigEditorWindow.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

#include <functional>

namespace
{
	QString Clean(QString text)
	{
		text.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '))
			.replace(QLatin1Char('\n'), QLatin1Char(' '));
		return text.trimmed();
	}

	QString Hash(const QByteArray& bytes)
	{
		return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
	}

	QString Short(const QString& text) { return Hash(text.toUtf8()).left(16); }

	QByteArray ReadAll(const QString& path)
	{
		QFile file(path);
		return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<unreadable>");
	}

	void StopTimers(ConfigEditorWindow& window)
	{
		for (QTimer* timer : window.findChildren<QTimer*>()) timer->stop();
	}

	// The corpus the "Document" layer needs besides the files named on the command line: the shipped
	// sample, then the doc 42 hazard shapes (BOM, equals separator, retired keys, legacy queue,
	// [vprenderer] root, a renderer-less file).
	std::vector<std::pair<QString, QByteArray>> BuiltInCorpus()
	{
		std::vector<std::pair<QString, QByteArray>> corpus;
		corpus.emplace_back(QStringLiteral("minimal"), QByteArray("[general]\nrenderer: VP Renderer\n"));
		corpus.emplace_back(QStringLiteral("bom-equals"), QByteArray("\xEF\xBB\xBF[general]\nrenderer = VP Renderer\nfullscreen = true\n"));
		corpus.emplace_back(QStringLiteral("retired-keys"), QByteArray(
			"[general]\nrenderer: VP Renderer\n[vprenderer]\nquality: high\nvideo_output_color_space: bt709\n"
			"[vprenderer.Default]\nquality: high\ndithering: auto\n"));
		corpus.emplace_back(QStringLiteral("legacy-queue"), QByteArray(
			"[general]\nrenderer: VP Renderer\n[queue]\ntarget_frames: 3\n[vprenderer]\nqueue_target_frames: 2\n"));
		corpus.emplace_back(QStringLiteral("calibration"), QByteArray(
			"[general]\nrenderer: VP Renderer\n[calibration]\nmeter_model: i1d3\nlut_size: 33\nverify: true\n"
			"[vprenderer.color.Default]\ncalibration_lut_enabled: true\ncalibration_lut_bt709: none\n"));
		corpus.emplace_back(QStringLiteral("led-drive"), QByteArray(
			"[general]\nrenderer: VP Renderer\n[led_drive.Default]\nenabled: true\ncommand_source: compensate\n"
			"[projector.Default]\nprotocol: barco_ascii\nport: COM1\n"));
		return corpus;
	}

	struct Scope
	{
		QTextStream& out;
		QString corpus;
		QString pathway;
	};

	QString Describe(QWidget* w, QWidget* pageRoot)
	{
		QString label, value, choices;
		if (auto* b = qobject_cast<QAbstractButton*>(w))
		{
			label = b->text();
			value = b->isCheckable() ? (b->isChecked() ? QStringLiteral("on") : QStringLiteral("off")) : QString();
		}
		else if (auto* l = qobject_cast<QLabel*>(w)) label = l->text();
		else if (auto* g = qobject_cast<QGroupBox*>(w)) label = g->title();
		else if (auto* c = qobject_cast<QComboBox*>(w))
		{
			value = c->currentText() + QLatin1Char('=') + c->currentData().toString();
			for (int i = 0; i < c->count(); ++i) choices += c->itemData(i).toString() + QLatin1Char('=') + c->itemText(i) + QLatin1Char('|');
		}
		else if (auto* e = qobject_cast<QLineEdit*>(w))
		{
			value = e->text();
			label = e->placeholderText();
		}
		else if (auto* s = qobject_cast<QAbstractSpinBox*>(w)) value = s->text();
		else if (auto* sl = qobject_cast<QSlider*>(w)) value = QString::number(sl->value());
		else if (auto* p = qobject_cast<QPlainTextEdit*>(w)) value = p->toPlainText();
		else if (auto* t = qobject_cast<QTextEdit*>(w)) value = t->toPlainText();
		else if (auto* pr = qobject_cast<QProgressBar*>(w)) value = QString::number(pr->value());
		else if (auto* list = qobject_cast<QListWidget*>(w))
		{
			for (int i = 0; i < list->count(); ++i) choices += list->item(i)->text() + QLatin1Char('|');
			value = QString::number(list->currentRow());
		}
		else if (auto* table = qobject_cast<QTableWidget*>(w)) value = QStringLiteral("%1x%2").arg(table->rowCount()).arg(table->columnCount());
		else if (auto* bar = qobject_cast<QTabBar*>(w))
		{
			for (int i = 0; i < bar->count(); ++i) choices += bar->tabText(i) + QLatin1Char('|');
			value = QString::number(bar->currentIndex());
		}
		QString ancestors;
		for (QWidget* p = w->parentWidget(); p && p != pageRoot; p = p->parentWidget())
			if (!p->objectName().isEmpty()) ancestors = p->objectName() + QLatin1Char('/') + ancestors;
		return QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10")
			.arg(QLatin1String(w->metaObject()->className()), Clean(w->objectName()), Clean(ancestors), Clean(label),
				w->isEnabled() ? QStringLiteral("1") : QStringLiteral("0"),
				w->isVisibleTo(pageRoot) ? QStringLiteral("1") : QStringLiteral("0"),
				Short(w->toolTip()), Clean(choices).left(600), Clean(value).left(300),
				Clean(w->property("vpvOwner").toString()));
	}

	bool IsInteresting(QWidget* w)
	{
		return !w->objectName().isEmpty() || qobject_cast<QAbstractButton*>(w) || qobject_cast<QLabel*>(w) ||
			qobject_cast<QComboBox*>(w) || qobject_cast<QLineEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w) ||
			qobject_cast<QSlider*>(w) || qobject_cast<QGroupBox*>(w) || qobject_cast<QListWidget*>(w) ||
			qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QTextEdit*>(w) || qobject_cast<QTableWidget*>(w) ||
			qobject_cast<QTabBar*>(w) || qobject_cast<QProgressBar*>(w);
	}

	void DumpControls(const Scope& scope, int pageIndex, const QString& tag, QWidget* pageRoot)
	{
		int ordinal = 0;
		for (QWidget* w : pageRoot->findChildren<QWidget*>())
		{
			if (!IsInteresting(w)) continue;
			scope.out << "C\t" << scope.corpus << '\t' << scope.pathway << '\t' << pageIndex << '\t' << tag << '\t'
				<< ordinal++ << '\t' << Describe(w, pageRoot) << '\n';
		}
	}

	void Grab(const Scope& scope, int pageIndex, const QString& tag, QWidget* page)
	{
		QCoreApplication::processEvents();
		const QImage image = page->grab().toImage().convertToFormat(QImage::Format_RGBA8888);
		scope.out << "G\t" << scope.corpus << '\t' << scope.pathway << '\t' << pageIndex << '\t' << tag << '\t'
			<< image.width() << 'x' << image.height() << '\t'
			<< Hash(QByteArray(reinterpret_cast<const char*>(image.constBits()), static_cast<qsizetype>(image.sizeInBytes())))
			<< '\n';
	}

	QByteArray WithPathway(QByteArray bytes, bool original)
	{
		if (!original) return bytes;
		const QByteArray key = "pipeline_original_pathway: on\n";
		const int header = bytes.indexOf("[vpv]");
		if (header < 0) return bytes + (bytes.endsWith('\n') ? "" : "\n") + "[vpv]\n" + key;
		const int eol = bytes.indexOf('\n', header);
		return eol < 0 ? bytes + "\n" + key : bytes.insert(eol + 1, key);
	}

	void Pages(const Scope& scope, ConfigEditorWindow& window)
	{
		auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
		if (!pages) { scope.out << "X\tno settingsPages\n"; return; }
		for (int index = 0; index < pages->count(); ++index)
		{
			window.selectPage(index);
			QCoreApplication::processEvents();
			StopTimers(window);
			QWidget* page = pages->widget(index);
			DumpControls(scope, index, QStringLiteral("-"), page);
			Grab(scope, index, QStringLiteral("page"), page);
			// Every tab of every tab widget on the page.
			int tabsOrdinal = 0;
			for (QTabWidget* tabs : page->findChildren<QTabWidget*>())
			{
				const int original = tabs->currentIndex();
				for (int t = 0; t < tabs->count(); ++t)
				{
					tabs->setCurrentIndex(t);
					Grab(scope, index, QStringLiteral("tabs%1.%2:%3").arg(tabsOrdinal).arg(t).arg(Clean(tabs->tabText(t))), page);
				}
				tabs->setCurrentIndex(original);
				++tabsOrdinal;
			}
			// Every profile of every profile list (the profile pages re-load their fields on a switch).
			for (QListWidget* list : page->findChildren<QListWidget*>())
			{
				if (!list->objectName().endsWith(QStringLiteral(".profiles"))) continue;
				const int original = list->currentRow();
				for (int row = 0; row < list->count(); ++row)
				{
					list->setCurrentRow(row);
					QCoreApplication::processEvents();
					const QString tag = QStringLiteral("%1#%2:%3").arg(list->objectName()).arg(row).arg(Clean(list->item(row)->text()));
					DumpControls(scope, index, tag, page);
					Grab(scope, index, tag, page);
				}
				if (original >= 0) list->setCurrentRow(original);
			}
		}
	}

	// A scripted edit of one control, by kind. False when the control has no edit.
	bool EditControl(QWidget* w)
	{
		if (auto* c = qobject_cast<QCheckBox*>(w)) { c->setChecked(!c->isChecked()); return true; }
		if (auto* c = qobject_cast<QComboBox*>(w))
		{
			if (c->count() < 2) return false;
			c->setCurrentIndex((c->currentIndex() + 1) % c->count());
			return true;
		}
		if (auto* s = qobject_cast<QSpinBox*>(w))
		{
			s->setValue(s->value() < s->maximum() ? s->value() + s->singleStep() : s->value() - s->singleStep());
			return true;
		}
		if (auto* s = qobject_cast<QDoubleSpinBox*>(w))
		{
			s->setValue(s->value() < s->maximum() ? s->value() + s->singleStep() : s->value() - s->singleStep());
			return true;
		}
		if (auto* e = qobject_cast<QLineEdit*>(w))
		{
			if (e->isReadOnly()) return false;
			e->setText(e->text() + QStringLiteral("x"));
			return true;
		}
		return false;
	}

	bool ApplyAndRead(ConfigEditorWindow& window, const QString& path, QByteArray& bytes)
	{
		auto* apply = window.findChild<QPushButton*>(QStringLiteral("applyConfiguration"));
		if (apply && apply->isEnabled())
		{
			apply->click();
			QCoreApplication::processEvents();
		}
		bytes = ReadAll(path);
		return apply != nullptr;
	}

	// The document layer: corpus file -> load + migrations (+ Apply when anything is pending) -> bytes.
	void Document(const Scope& scope, const QString& path, const QByteArray& original)
	{
		ConfigEditorWindow window(path, 0, true);
		StopTimers(window);
		QByteArray bytes;
		ApplyAndRead(window, path, bytes);
		scope.out << "D\t" << scope.corpus << '\t' << scope.pathway << '\t' << (bytes == original ? "unchanged" : "changed")
			<< '\t' << Hash(bytes) << '\n';
	}

	// One scripted edit per control of the pages named: each in a fresh editor on a fresh copy.
	void Edits(const Scope& scope, const QString& dirPath, const QByteArray& original, const QStringList& pageObjectNames)
	{
		struct Target { QString name; int ordinal; };
		std::vector<Target> targets;
		{
			const QString probePath = QDir(dirPath).filePath(QStringLiteral("probe.cfg"));
			QFile probe(probePath);
			if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
			probe.write(original);
			probe.close();
			ConfigEditorWindow window(probePath, 0, true);
			StopTimers(window);
			for (const QString& marker : pageObjectNames)
			{
				window.selectPage(window.pageIndex(marker));
				auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
				QWidget* page = pages ? pages->currentWidget() : nullptr;
				if (!page) continue;
				int ordinal = 0;
				for (QWidget* w : page->findChildren<QWidget*>())
				{
					if (w->objectName().isEmpty()) continue;
					if (qobject_cast<QCheckBox*>(w) || qobject_cast<QComboBox*>(w) || qobject_cast<QSpinBox*>(w) ||
						qobject_cast<QDoubleSpinBox*>(w) || qobject_cast<QLineEdit*>(w))
						targets.push_back({ marker + QLatin1Char('|') + w->objectName(), ordinal });
					++ordinal;
				}
			}
		}
		for (const Target& target : targets)
		{
			const QString path = QDir(dirPath).filePath(QStringLiteral("edit.cfg"));
			QFile file(path);
			if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
			file.write(original);
			file.close();
			ConfigEditorWindow window(path, 0, true);
			StopTimers(window);
			const QStringList parts = target.name.split(QLatin1Char('|'));
			window.selectPage(window.pageIndex(parts[0]));
			auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
			QWidget* page = pages ? pages->currentWidget() : nullptr;
			QWidget* control = nullptr;
			if (page)
			{
				int ordinal = 0;
				for (QWidget* w : page->findChildren<QWidget*>())
				{
					if (w->objectName().isEmpty()) continue;
					if (qobject_cast<QCheckBox*>(w) || qobject_cast<QComboBox*>(w) || qobject_cast<QSpinBox*>(w) ||
						qobject_cast<QDoubleSpinBox*>(w) || qobject_cast<QLineEdit*>(w))
					{
						if (w->objectName() == parts[1] && ordinal >= 0) { control = w; break; }
					}
					++ordinal;
				}
			}
			QString outcome = QStringLiteral("missing");
			QByteArray bytes;
			if (control && EditControl(control))
			{
				QCoreApplication::processEvents();
				ApplyAndRead(window, path, bytes);
				outcome = Hash(bytes);
			}
			else if (control) outcome = QStringLiteral("noedit");
			scope.out << "E\t" << scope.corpus << '\t' << scope.pathway << '\t' << target.name << '\t' << outcome << '\n';
		}
	}
}

int WriteConfigIdentity(const QString& outPath, const QStringList& extraFiles)
{
	QFile out(outPath);
	if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return 1;
	QTextStream stream(&out);
	stream.setEncoding(QStringConverter::Utf8);
	auto corpus = BuiltInCorpus();
	for (const QString& file : extraFiles)
		corpus.emplace_back(QFileInfo(file).fileName(), ReadAll(file));
	for (const auto& entry : corpus)
	{
		for (int original = 0; original < 2; ++original)
		{
			const Scope scope{ stream, entry.first, original ? QStringLiteral("original") : QStringLiteral("vpv") };
			const QByteArray bytes = WithPathway(entry.second, original != 0);
			QTemporaryDir dir;
			const QString path = dir.filePath(QStringLiteral("VideoProcessor.cfg"));
			QFile file(path);
			if (!file.open(QIODevice::WriteOnly)) return 1;
			file.write(bytes);
			file.close();
			Document(scope, path, bytes);
			// Controls and pictures on a fresh copy (the document run may have rewritten the file).
			{
				QFile again(path);
				if (!again.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
				again.write(bytes);
				again.close();
				ConfigEditorWindow window(path, 0, true);
				window.resize(1120, 700);
				window.show();
				QCoreApplication::processEvents();
				StopTimers(window);
				Pages(scope, window);
				window.hide();
			}
			// Scripted edits: only for the corpus members that carry the pages' sections.
			if (entry.first == QStringLiteral("calibration") || entry.first == QStringLiteral("led-drive"))
				Edits(scope, dir.path(), bytes, { QStringLiteral("Calibration"), QStringLiteral("Dynamic Drive") });
		}
	}
	stream.flush();
	return stream.status() == QTextStream::Ok ? 0 : 1;
}
