// VPV-HOOK-FILE
/*
 * Copyright(C) 2021 Dennis Fleurbaaij <mail@dennisfleurbaaij.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QFormLayout>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

// Config editor extension point: an extension adds pages and profile rows, migrates settings at
// load, and is told before a save and after a successful save, without the editor knowing it.
// With nothing registered every call below does nothing.
//
// Registration contract: Register() is called only at start-up, before the editor is created,
// and the registry is read-only afterwards (UI thread only, no locks). Registration order is
// call order. Providers are not owned and must outlive the registry.
namespace ConfigPageHost
{
	// The narrow view of the editor a provider may use.
	class ConfigFormApi
	{
	public:
		virtual ~ConfigFormApi() = default;
		virtual QString value(const QString& section, const QString& key) const = 0;
		virtual void setValue(const QString& section, const QString& key, const QString& text) = 0;
		virtual void markDirty() = 0;
		virtual QStringList profileSections(const QString& root) const = 0;
	};

	struct ConfigPage
	{
		QString name;        // navigation title and page name
		QString leafAfter;   // title of the navigation leaf this one prefers to follow (advisory)
		QWidget* widget = nullptr;   // ownership passes to the editor
	};

	class IConfigPageProvider
	{
	public:
		virtual ~IConfigPageProvider() = default;
		virtual void AppendPages(ConfigFormApi& api, std::vector<ConfigPage>& pages) { (void)api; (void)pages; }
		virtual void AppendProfileRows(const QString& sectionPrefix, QFormLayout* form, ConfigFormApi& api)
		{ (void)sectionPrefix; (void)form; (void)api; }
		// True when it changed the document.
		virtual bool MigrateOnLoad(ConfigFormApi& api) { (void)api; return false; }
		virtual void BeforeSave(ConfigFormApi& api) { (void)api; }
		virtual void AfterApply(ConfigFormApi& api) { (void)api; }
	};

	namespace Detail
	{
		inline std::vector<IConfigPageProvider*>& Registry()
		{
			static std::vector<IConfigPageProvider*> registry;
			return registry;
		}
	}

	// Start-up only. A null pointer is ignored.
	inline void Register(IConfigPageProvider* provider) noexcept
	{
		try { if (provider) Detail::Registry().push_back(provider); }
		catch (...) {}
	}

	inline void AppendPages(ConfigFormApi& api, std::vector<ConfigPage>& pages) noexcept
	{
		try { for (IConfigPageProvider* p : Detail::Registry()) { try { p->AppendPages(api, pages); } catch (...) {} } }
		catch (...) {}
	}

	// `makeForm` returns the form to fill and is called once, only when a provider is registered,
	// so an editor with no providers gets no extra widget.
	template <class MakeForm>
	void AppendProfileRows(const QString& sectionPrefix, MakeForm&& makeForm, ConfigFormApi& api) noexcept
	{
		try
		{
			if (Detail::Registry().empty()) return;
			QFormLayout* form = makeForm();
			for (IConfigPageProvider* p : Detail::Registry())
			{
				try { p->AppendProfileRows(sectionPrefix, form, api); } catch (...) {}
			}
		}
		catch (...) {}
	}

	// All providers run; true when any changed the document.
	inline bool MigrateOnLoad(ConfigFormApi& api) noexcept
	{
		bool changed = false;
		try
		{
			for (IConfigPageProvider* p : Detail::Registry())
			{
				try { changed = p->MigrateOnLoad(api) || changed; } catch (...) {}
			}
		}
		catch (...) {}
		return changed;
	}

	inline void BeforeSave(ConfigFormApi& api) noexcept
	{
		try { for (IConfigPageProvider* p : Detail::Registry()) { try { p->BeforeSave(api); } catch (...) {} } }
		catch (...) {}
	}

	inline void AfterApply(ConfigFormApi& api) noexcept
	{
		try { for (IConfigPageProvider* p : Detail::Registry()) { try { p->AfterApply(api); } catch (...) {} } }
		catch (...) {}
	}

	// Tests only.
	inline void ResetForTesting() noexcept
	{
		try { Detail::Registry().clear(); } catch (...) {}
	}
}
