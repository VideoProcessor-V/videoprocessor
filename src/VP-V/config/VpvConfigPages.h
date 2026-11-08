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
#include <VP-V/config/VpvBothPathwayCards.h>
#include <VP-V/config/VpvProfileUseRows.h>
#include <VP-V/config/VpvScalingRows.h>
#include <VP-V/config/VpvSection.h>
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

		// Forgets the VP-V pages (the shell is being rebuilt). The unit cards are kept: Bill's pages,
		// which build them, are created before this call; the VP-V page takes them.
		void reset() { vpvNames_.clear(); subPages_.clear(); }

		// Unit cards built by Bill's pages for the VP-V page (VpvScalingRows.h), taken once each.
		std::vector<UnitCard>& unitCardStash() { return unitCards_; }
		std::vector<UnitCard> takeUnitCards(const QString& unit)
		{
			std::vector<UnitCard> taken, kept;
			for (const UnitCard& card : unitCards_) (card.unit == unit ? taken : kept).push_back(card);
			unitCards_.swap(kept);
			return taken;
		}

		// A page that belongs to another page (ConfigPage::parentPage): no top-level navigation leaf, opened from
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

		// The page whose navigation leaf stands for page `index`: itself. A sub-page of the VP-V master
		// page has a child leaf under the VP-V leaf (addNavigationLeaves); a sub-page of any other page
		// has no leaf of its own and its parent's leaf stands for it.
		int navigationIndex(int index) const
		{
			const QString own = name(index);
			for (const SubPageInfo& sub : subPages_)
				if (sub.name == own && sub.parent != QStringLiteral("VP-V"))
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
		// VP-V page, so that page can link the sub-pages the providers registered. A sub-page (parentPage
		// set) is named and stacked like any page; a sub-page of VP-V gets a child leaf right after the VP-V
		// leaf, in registration order, and a sub-page of any other parent gets none. A page's leafAfter is
		// advisory and not honoured: a provider page without a parent keeps its stack order, so its leaf
		// comes before the VP-V leaf; child leaves never use it.
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

		// Adds the VP-V navigation leaves through the host's own leaf builder (title, page index). The
		// VP-V master page gets its leaf, and each sub-page registered under it (Dynamic Drive,
		// Calibration, ...) a child leaf right below it, in registration order, indented. Every VP-V
		// setting is thus reachable from the VP-V group of the list (owner 2026-11-07); the unit tabs'
		// "Open ..." buttons reach the same pages. A sub-page of another parent has no leaf.
		template <class AddLeaf>
		void addNavigationLeaves(AddLeaf&& addLeaf) const
		{
			for (const QString& title : vpvNames_)
			{
				bool sub = false;
				for (const SubPageInfo& info : subPages_) sub = sub || info.name == title;
				if (sub) continue;
				QPushButton* button = addLeaf(title, index(title));
				if (title != QStringLiteral("VP-V")) continue;
				button->setObjectName(QStringLiteral("config.openVpv"));
				for (const SubPageInfo& info : subPages_)
				{
					if (info.parent != title) continue;
					QPushButton* child = addLeaf(info.name, index(info.name));
					child->setProperty("vpvChild", true);
					child->setProperty("vpvParent", title);
					child->setStyleSheet(QStringLiteral("QPushButton { padding-left: 26px; }"));
				}
			}
		}

	private:
		QStringList vpvNames_;
		std::vector<SubPageInfo> subPages_;
		std::vector<UnitCard> unitCards_;
	};
}
