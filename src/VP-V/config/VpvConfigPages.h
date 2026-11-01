// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Page directory of the Config editor: Bill's pages keep his order and numbers (0-22, a fixed
// table here), VP-V's pages (Dynamic Drive, Calibration, VP-V) are appended after them at
// runtime. Every VP-V lookup of a page goes through its name, so neither side's order can shift
// the other's. Moved out of ConfigEditorWindow so Bill's createShell keeps his literal order.

#include <QPushButton>
#include <QString>
#include <QStackedWidget>
#include <QStringList>
#include <QWidget>
#include <vector>

namespace Vpv
{
	class ConfigPageDirectory
	{
	public:
		// Bill's pages, by his page index (the names this editor has always given them).
		static const QStringList& BillPageNames()
		{
			static const QStringList names = {
				QStringLiteral("General"), QStringLiteral("Queue"), QStringLiteral("VP Renderer"),
				QStringLiteral("DirectShow"), QStringLiteral("Screen"), QStringLiteral("DV"),
				QStringLiteral("Shortcuts list"), QStringLiteral("Actions"),
				QStringLiteral("Standard shaders"), QStringLiteral("NLS shaders"), QStringLiteral("Logs"),
				QStringLiteral("Input processing (VP Renderer)"),
				QStringLiteral("Input processing (DirectShow)"), QStringLiteral("Output (former)"),
				QStringLiteral("Shaders"), QStringLiteral("Shortcuts"), QStringLiteral("Color"),
				QStringLiteral("Scaling"), QStringLiteral("Zoom"), QStringLiteral("Subtitles"),
				QStringLiteral("Updates"), QStringLiteral("Motion"), QStringLiteral("Display") };
			return names;
		}

		// -1 for an unknown name.
		int index(const QString& name) const
		{
			const int bill = BillPageNames().indexOf(name);
			if (bill >= 0) return bill;
			const int vpv = vpvNames_.indexOf(name);
			return vpv >= 0 ? BillPageNames().size() + vpv : -1;
		}

		// Empty for an index that is not a named page.
		QString name(int index) const
		{
			const int bill = static_cast<int>(BillPageNames().size());
			if (index < 0) return QString();
			if (index < bill) return BillPageNames().at(index);
			return index - bill < vpvNames_.size() ? vpvNames_.at(index - bill) : QString();
		}

		// Forgets the VP-V pages (the shell is being rebuilt).
		void reset() { vpvNames_.clear(); }

		// Adds a VP-V page to the stack (which must already hold Bill's pages) and names it.
		int append(QStackedWidget* pages, const QString& name, QWidget* page)
		{
			const int index = pages->addWidget(page);
			vpvNames_.push_back(name);
			return index;
		}

		// Adds the VP-V navigation leaves through the host's own leaf builder (title, page index).
		template <class AddLeaf>
		void addNavigationLeaves(AddLeaf&& addLeaf) const
		{
			for (const QString& title : vpvNames_)
			{
				QPushButton* button = addLeaf(title, index(title));
				if (title == QStringLiteral("VP-V")) button->setObjectName(QStringLiteral("config.openVpv"));
			}
		}

	private:
		QStringList vpvNames_;
	};
}
