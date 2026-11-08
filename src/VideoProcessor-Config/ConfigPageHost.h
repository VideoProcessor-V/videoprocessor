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

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QObject;

// Config editor extension point: an extension adds pages and sees each bound field, migrates settings at
// load, and is told before a save and after a successful save, without the editor knowing it.
// With nothing registered every call below does nothing.
//
// Registration contract: Register() is called only at start-up, before the editor is created,
// and the registry is read-only afterwards (UI thread only, no locks). Registration order is
// call order. Providers are not owned and must outlive the registry.
namespace ConfigPageHost
{
	// What a provider may use to build its own page the way the editor builds its pages: the editor's
	// own field helpers (same object names, combo style, dirty path) and a few editor services.
	// Every member has a harmless default so a test can use a small fake. UI thread only.
	class EditorKit
	{
	public:
		virtual ~EditorKit() = default;

		struct RemoteTarget
		{
			bool remote = false;      // editing a remote VideoProcessor (not this computer)
			QString host;
			unsigned short port = 0;
		};

		// Field helpers.
		virtual QString value(const QString& section, const QString& key, const QString& fallback) const
		{ (void)section; (void)key; return fallback; }
		virtual QLineEdit* bindTextField(const QString& section, const QString& key, const QString& fallback) { (void)section; (void)key; (void)fallback; return nullptr; }
		virtual QComboBox* bindChoiceField(const QString& section, const QString& key, const QStringList& values, const QStringList& labels)
		{ (void)section; (void)key; (void)values; (void)labels; return nullptr; }
		virtual QCheckBox* bindCheckField(const QString& label, const QString& section, const QString& key, bool defaultValue)
		{ (void)label; (void)section; (void)key; (void)defaultValue; return nullptr; }
		virtual QString controlName(const QString& section, const QString& key) const { return QStringLiteral("config.%1.%2").arg(section, key); }
		virtual QLabel* helpLabel(const QString& text) { (void)text; return nullptr; }
		virtual QWidget* createCard(const QString& title, const QString& description, QWidget* content) { (void)title; (void)description; return content; }
		virtual QWidget* createPage(const QString& title, const QString& description, QWidget* body) { (void)title; (void)description; return body; }
		// The editor's responsive card grid (a widget that takes cards).
		virtual QWidget* createCardGrid() { return nullptr; }
		virtual void addCard(QWidget* grid, QWidget* card) { (void)grid; (void)card; }
		// Writes a known key of the document, adding the section when it is missing. Does not mark dirty.
		virtual void setKnown(const QString& section, const QString& key, const QString& text) { (void)section; (void)key; (void)text; }
		// Marks the document edited by `edited` (keeps the editor's edit order for validation messages).
		virtual void markDirty(QObject* edited) { (void)edited; }

		// Editor services.
		virtual QWidget* window() { return nullptr; }       // parent of dialogs and helper threads
		virtual QWidget* currentPage() const { return nullptr; }
		virtual bool settingsPreview() const { return false; }
		virtual bool noTarget() const { return false; }
		virtual RemoteTarget remoteTarget() const { return RemoteTarget(); }
		virtual bool documentDirty() const { return false; }
		virtual void discardEdits() {}
		virtual void setStatus(const QString& text) { (void)text; }
		virtual void setWarningStatus(const QString& text) { (void)text; }
	};

	// The narrow view of the editor a provider may use.
	class ConfigFormApi
	{
	public:
		virtual ~ConfigFormApi() = default;
		virtual QString value(const QString& section, const QString& key) const = 0;
		virtual void setValue(const QString& section, const QString& key, const QString& text) = 0;
		virtual void markDirty() = 0;
		virtual QStringList profileSections(const QString& root) const = 0;
		// The editor's field helpers and services; null when the editor offers none.
		virtual EditorKit* editorKit() { return nullptr; }
	};

	struct ConfigPage
	{
		QString name;        // navigation title and page name
		QString leafAfter;   // title of the navigation leaf this one prefers to follow (advisory)
		QWidget* widget = nullptr;   // ownership passes to the editor
		// A sub-page has no navigation leaf of its own: it is opened from the page named in parentPage
		// (grouped there under `group`) and keeps its page name and place in the stack.
		QString parentPage;
		QString group;
		QString description;   // one line shown by the parent page where it links the sub-page
	};

	class IConfigPageProvider
	{
	public:
		virtual ~IConfigPageProvider() = default;
		virtual void AppendPages(ConfigFormApi& api, std::vector<ConfigPage>& pages) { (void)api; (void)pages; }
		// An either/or hook for a field Bill's editor binds (e.g. a choice set that depends on a VP-V
		// switch): called for each bound field on every profile load/refresh, before the editor sets the widget's value, with its section prefix, key and widget (so use it to change the widget's choices, not its value).
		virtual void OnBillField(const QString& sectionPrefix, const QString& key, QWidget* widget, ConfigFormApi& api)
		{ (void)sectionPrefix; (void)key; (void)widget; (void)api; }
		// True when it changed the document.
		virtual bool MigrateOnLoad(ConfigFormApi& api) { (void)api; return false; }
		virtual void BeforeSave(ConfigFormApi& api) { (void)api; }
		virtual void AfterApply(ConfigFormApi& api) { (void)api; }
		// The editor is being destroyed: join helper threads and drop per-editor state (UI thread).
		virtual void OnEditorClosing(ConfigFormApi& api) { (void)api; }
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

	inline void OnBillField(const QString& sectionPrefix, const QString& key, QWidget* widget, ConfigFormApi& api) noexcept
	{
		try
		{
			for (IConfigPageProvider* p : Detail::Registry())
			{
				try { p->OnBillField(sectionPrefix, key, widget, api); } catch (...) {}
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

	inline void OnEditorClosing(ConfigFormApi& api) noexcept
	{
		try { for (IConfigPageProvider* p : Detail::Registry()) { try { p->OnEditorClosing(api); } catch (...) {} } }
		catch (...) {}
	}

	// The registered provider of type T, or null (test seam).
	template <class T>
	T* Find() noexcept
	{
		try
		{
			for (IConfigPageProvider* p : Detail::Registry())
				if (T* found = dynamic_cast<T*>(p)) return found;
		}
		catch (...) {}
		return nullptr;
	}

	// Tests only.
	inline void ResetForTesting() noexcept
	{
		try { Detail::Registry().clear(); } catch (...) {}
	}
}
