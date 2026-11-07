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
#include <VideoProcessor-Config/ConfigPageHost.h>
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
		void reset() { vpvNames_.clear(); subPages_.clear(); }

		// A page that belongs to another page (ConfigPage::parentPage): no navigation leaf, opened from
		// its parent, grouped there under `group`.
		struct SubPageInfo
		{
			QString name, parent, group, description;
		};

		// The sub-pages of `parent` grouped under `group` (case-insensitive; an empty group matches all).
		std::vector<SubPageInfo> subPagesOf(const QString& parent, const QString& group = QString()) const
		{
			std::vector<SubPageInfo> found;
			for (const SubPageInfo& sub : subPages_)
				if (sub.parent == parent && (group.isEmpty() || sub.group.compare(group, Qt::CaseInsensitive) == 0))
					found.push_back(sub);
			return found;
		}

		// The page whose navigation leaf stands for page `index`: the parent of a sub-page, else itself.
		int navigationIndex(int index) const
		{
			const QString own = name(index);
			for (const SubPageInfo& sub : subPages_)
				if (sub.name == own)
				{
					const int parent = this->index(sub.parent);
					return parent >= 0 ? parent : index;
				}
			return index;
		}

		// Adds a VP-V page to the stack (which must already hold Bill's pages) and names it.
		int append(QStackedWidget* pages, const QString& name, QWidget* page)
		{
			const int index = pages->addWidget(page);
			vpvNames_.push_back(name);
			return index;
		}

		// Adds the pages of registered config page providers (ConfigPageHost.h); nothing registered adds
		// nothing. The editor calls this after its own VP-V pages (Dynamic Drive) and before the master
		// VP-V page, so that page can link the sub-pages the providers registered. A page's leafAfter is
		// advisory and not yet honoured: provider leaves follow Dynamic Drive, before the VP-V leaf. A sub-page (parentPage set) is
		// named and stacked like any page but gets no navigation leaf.
		int appendProviderPages(QStackedWidget* pages, ConfigPageHost::ConfigFormApi& api)
		{
			std::vector<ConfigPageHost::ConfigPage> extra;
			ConfigPageHost::AppendPages(api, extra);
			int added = 0;
			for (const ConfigPageHost::ConfigPage& page : extra)
			{
				if (!page.widget || page.name.isEmpty()) continue;
				append(pages, page.name, page.widget);
				if (!page.parentPage.isEmpty())
					subPages_.push_back({ page.name, page.parentPage, page.group, page.description });
				++added;
			}
			return added;
		}

		// Adds the VP-V navigation leaves through the host's own leaf builder (title, page index).
		template <class AddLeaf>
		void addNavigationLeaves(AddLeaf&& addLeaf) const
		{
			for (const QString& title : vpvNames_)
			{
				bool sub = false;
				for (const SubPageInfo& info : subPages_) sub = sub || info.name == title;
				if (sub) continue;
				QPushButton* button = addLeaf(title, index(title));
				if (title == QStringLiteral("VP-V")) button->setObjectName(QStringLiteral("config.openVpv"));
			}
		}

	private:
		QStringList vpvNames_;
		std::vector<SubPageInfo> subPages_;
	};
}
