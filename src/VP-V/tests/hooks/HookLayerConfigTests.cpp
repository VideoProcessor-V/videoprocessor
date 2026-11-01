// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// The generic hook layer (M2 P0), Config editor side (H3): the page provider registry in
// VideoProcessor-Config/ConfigPageHost.h and its call sites in ConfigEditorWindow. Empty = the
// editor as before; registered = providers called in registration order; a throwing provider
// never reaches the editor. Registered from ConfigEditorWindowTests.cpp's main with one line:
// RunHookLayerConfigTests(). (The Config tests are the suite's own runner, not MSTest, so the
// class name is the test-name prefix: run with --match HookLayerConfigTests.)

#include "ConfigEditorWindow.h"
#include "ConfigPageHost.h"

#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	namespace CPH = ConfigPageHost;

	std::function<void(bool, const char*)> g_require;
	std::function<void(ConfigEditorWindow&)> g_save;
	void require(bool condition, const char* message) { g_require(condition, message); }

	// Empties the registry whatever a test does.
	struct Scope
	{
		Scope() { CPH::ResetForTesting(); }
		~Scope() { CPH::ResetForTesting(); }
	};

	class NullApi : public CPH::ConfigFormApi
	{
	public:
		QString value(const QString&, const QString&) const override { return QString(); }
		void setValue(const QString&, const QString&, const QString&) override {}
		void markDirty() override {}
		QStringList profileSections(const QString&) const override { return QStringList(); }
	};

	// Records every call, by name, in the shared log.
	class Recorder : public CPH::IConfigPageProvider
	{
	public:
		Recorder(std::vector<std::string>* log, std::string name, bool migrates = false)
			: log_(log), name_(std::move(name)), migrates_(migrates) {}
		bool addPage = false;
		bool setKeyOnMigrate = false;

		void AppendPages(CPH::ConfigFormApi&, std::vector<CPH::ConfigPage>& pages) override
		{
			log_->push_back(name_ + ":pages");
			if (!addPage) return;
			auto* page = new QLabel(QString::fromStdString(name_));
			page->setObjectName(QString::fromStdString("hooktest.page." + name_));
			CPH::ConfigPage entry;
			entry.name = QString::fromStdString("Hook " + name_);
			entry.leafAfter = QStringLiteral("Shortcuts");
			entry.widget = page;
			pages.push_back(entry);
		}
		void AppendProfileRows(const QString& prefix, QFormLayout*, CPH::ConfigFormApi&) override
		{ log_->push_back(name_ + ":rows:" + prefix.toStdString()); }
		bool MigrateOnLoad(CPH::ConfigFormApi& api) override
		{
			log_->push_back(name_ + ":migrate");
			if (!setKeyOnMigrate || !api.value(QStringLiteral("vprenderer.Cinema"), QStringLiteral("dejudder_strength")).isEmpty())
				return migrates_;
			api.setValue(QStringLiteral("vprenderer.Cinema"), QStringLiteral("dejudder_strength"), QStringLiteral("0.25"));
			return true;
		}
		void BeforeSave(CPH::ConfigFormApi&) override { log_->push_back(name_ + ":before"); }
		void AfterApply(CPH::ConfigFormApi&) override { log_->push_back(name_ + ":after"); }
	private:
		std::vector<std::string>* log_;
		std::string name_;
		bool migrates_;
	};

	class Thrower : public CPH::IConfigPageProvider
	{
	public:
		void AppendPages(CPH::ConfigFormApi&, std::vector<CPH::ConfigPage>&) override { throw std::runtime_error("x"); }
		void AppendProfileRows(const QString&, QFormLayout*, CPH::ConfigFormApi&) override { throw 1; }
		bool MigrateOnLoad(CPH::ConfigFormApi&) override { throw 1; }
		void BeforeSave(CPH::ConfigFormApi&) override { throw 1; }
		void AfterApply(CPH::ConfigFormApi&) override { throw 1; }
	};

	int Count(const std::vector<std::string>& log, const std::string& entry)
	{
		int n = 0;
		for (const std::string& line : log) if (line == entry) ++n;
		return n;
	}

	void testEmptyRegistryIsInert()
	{
		Scope scope;
		NullApi api;
		std::vector<CPH::ConfigPage> pages;
		CPH::AppendPages(api, pages);
		require(pages.empty(), "an empty registry added pages");
		require(!CPH::MigrateOnLoad(api), "an empty registry reported a migration");
		int factoryCalls = 0;
		CPH::AppendProfileRows(QStringLiteral("vprenderer"), [&]() -> QFormLayout* { ++factoryCalls; return nullptr; }, api);
		require(factoryCalls == 0, "an empty registry asked for a profile form (it would add an empty widget)");
		CPH::BeforeSave(api);
		CPH::AfterApply(api);
	}

	void testRegistrationOrderIsCallOrder()
	{
		Scope scope;
		NullApi api;
		std::vector<std::string> log;
		Recorder a(&log, "a"), b(&log, "b", true);
		CPH::Register(&a);
		CPH::Register(nullptr);   // ignored
		CPH::Register(&b);
		std::vector<CPH::ConfigPage> pages;
		CPH::AppendPages(api, pages);
		int factoryCalls = 0;
		CPH::AppendProfileRows(QStringLiteral("p"), [&]() -> QFormLayout* { ++factoryCalls; return nullptr; }, api);
		require(CPH::MigrateOnLoad(api), "a migrating provider was not reported");
		CPH::BeforeSave(api);
		CPH::AfterApply(api);
		const std::vector<std::string> want = { "a:pages", "b:pages", "a:rows:p", "b:rows:p", "a:migrate", "b:migrate",
			"a:before", "b:before", "a:after", "b:after" };
		require(log == want, "the providers were not called in registration order");
		require(factoryCalls == 1, "the profile form was not created exactly once");
		CPH::ResetForTesting();
		log.clear();
		CPH::BeforeSave(api);
		require(log.empty(), "ResetForTesting did not empty the registry");
	}

	void testThrowingProviderIsSkipped()
	{
		Scope scope;
		NullApi api;
		std::vector<std::string> log;
		Thrower thrower;
		Recorder after(&log, "after", true);
		CPH::Register(&thrower);
		CPH::Register(&after);
		std::vector<CPH::ConfigPage> pages;
		CPH::AppendPages(api, pages);
		CPH::AppendProfileRows(QStringLiteral("p"), []() -> QFormLayout* { return nullptr; }, api);
		require(CPH::MigrateOnLoad(api), "a provider after a throwing one was not reached");
		CPH::BeforeSave(api);
		CPH::AfterApply(api);
		require(Count(log, "after:pages") == 1 && Count(log, "after:rows:p") == 1 &&
			Count(log, "after:before") == 1 && Count(log, "after:after") == 1,
			"a provider after a throwing one missed a call");
	}

	void writeCinemaFixture(const QString& path)
	{
		QFile file(path);
		require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write fixture");
		const QByteArray text = "[vprenderer.Cinema]\nquality: high\n";
		require(file.write(text) == text.size(), "Cannot write fixture");
	}

	void testEditorWithoutProvidersHasNoExtraPages()
	{
		Scope scope;
		QTemporaryDir dir;
		const QString path = dir.filePath("VideoProcessor.cfg");
		writeCinemaFixture(path);
		ConfigEditorWindow window(path, 0, true);
		require(window.findChildren<QLabel*>(QRegularExpression(QStringLiteral("^hooktest\\.page\\."))).isEmpty(),
			"an editor with no providers has a provider page");
	}

	void testEditorCallsProviders()
	{
		Scope scope;
		std::vector<std::string> log;
		Recorder provider(&log, "p");
		provider.addPage = true;
		provider.setKeyOnMigrate = true;
		CPH::Register(&provider);
		QTemporaryDir dir;
		const QString path = dir.filePath("VideoProcessor.cfg");
		writeCinemaFixture(path);
		{
			ConfigEditorWindow window(path, 0, true);
			require(Count(log, "p:migrate") >= 1, "MigrateOnLoad was not called at load");
			require(Count(log, "p:pages") >= 1, "AppendPages was not called when the shell was built");
			require(window.findChild<QLabel*>(QStringLiteral("hooktest.page.p")) != nullptr, "the provider page is not in the editor");
			bool leaf = false;
			for (QPushButton* button : window.findChildren<QPushButton*>())
				if (button->text() == QStringLiteral("Hook p")) leaf = true;
			require(leaf, "the provider page has no navigation leaf");
			require(Count(log, "p:rows:vprenderer") >= 1, "AppendProfileRows was not called for a profile page");
			g_save(window);   // the migration left a pending change
			require(Count(log, "p:before") >= 1, "BeforeSave was not called by Apply");
			require(Count(log, "p:after") >= 1, "AfterApply was not called after a successful Apply");
		}
		QFile saved(path);
		require(saved.open(QIODevice::ReadOnly) && QString::fromUtf8(saved.readAll()).contains(QStringLiteral("dejudder_strength")),
			"the provider's migrated key was not saved");
	}
}

int RunHookLayerConfigTests(const std::function<int(const char*, const std::function<void()>&)>& run,
	const std::function<void(bool, const char*)>& requireHelper, const std::function<void(ConfigEditorWindow&)>& saveHelper)
{
	g_require = requireHelper;
	g_save = saveHelper;
	int failures = 0;
	failures += run("HookLayerConfigTests: empty registry is inert", testEmptyRegistryIsInert);
	failures += run("HookLayerConfigTests: registration order is call order", testRegistrationOrderIsCallOrder);
	failures += run("HookLayerConfigTests: a throwing provider is skipped", testThrowingProviderIsSkipped);
	failures += run("HookLayerConfigTests: editor without providers has no extra pages", testEditorWithoutProvidersHasNoExtraPages);
	failures += run("HookLayerConfigTests: editor calls providers", testEditorCallsProviders);
	return failures;
}
