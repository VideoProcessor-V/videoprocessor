// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S4A A4 (refs/112 pattern P2): the Config editor's VP-V seams as one friend struct. Bill's
// ConfigEditorWindow.h carries one `friend struct Vpv::ConfigEditorAccess;` line and
// ConfigEditorWindow.cpp includes this file once, at its end (the editor's anonymous-namespace helpers
// ResponsiveCardGrid, helpLabel, controlName and sandboxPreview are visible there). The three seams it replaces
// were 100 hook lines of lambdas in the editor; their bodies are unchanged and now live here:
//   BindFormApi            fills formApi_ and its EditorKit for the provider pages (constructor);
//   BuildVpvPages          appends the provider sub-pages and the master VP-V page (shell build).
// Included only by ConfigEditorWindow.cpp; nothing here runs unless the editor calls it.

#include <VP-V/modules/control/config/config_pages/VpvGeneralPageCards.h>   // and VpvSection.h, which it includes

#include <QPointer>

namespace Vpv
{
	void ConfigEditorAccess::BindFormApi(ConfigEditorWindow& window)
	{
		EnsureSectionStyleSheet();   // install VP-V's property-scoped rules on qApp once before any widget exists
		QPointer<ConfigEditorWindow> self(&window);
		auto readValue = [self](const QString& section, const QString& key, const QString& fallback) -> QString
		{
			if (!self || !self->configurationLoaded_ || !self->document_)
				return fallback;
			const std::string result = self->document_->Get(section.toStdString().c_str(), key.toStdString().c_str());
			return result.empty() ? fallback : DecodeConfigValue(result);
		};
		FunctionFormApi& formApi = self->formApi_;
		formApi.valueFn = [readValue](const QString& section, const QString& key) { return readValue(section, key, QString()); };
		formApi.setValueFn = [self](const QString& section, const QString& key, const QString& text)
		{
			if (!self || !self->document_) return;
			if (text.isEmpty()) self->document_->RemoveKnown(section.toStdString(), key.toStdString().c_str());
			else self->document_->SetKnown(section.toStdString(), key.toStdString().c_str(), text.toUtf8().constData());
			if (self->centralWidget()) self->markDirty();
		};
		formApi.markDirtyFn = [self] { if (self) self->markDirty(); };
		formApi.profileSectionsFn = [self](const QString& root) { return self ? self->profileSections(root) : QStringList(); };
		// The editor's own field helpers and services for provider pages (ConfigPageHost::EditorKit).
		FunctionEditorKit& kit = formApi.kit;
		kit.valueFn = readValue;
		kit.bindTextFn = [self](const QString& s, const QString& k, const QString& fallback) { return self ? BindUtf8TextField(self->formApi_.kit, s, k, fallback, accessibleSettingName(k)) : nullptr; };
		kit.bindChoiceFn = [self](const QString& s, const QString& k, const QStringList& values, const QStringList& labels) { return self ? self->bindChoiceField(s, k, values, labels) : nullptr; };
		kit.bindCheckFn = [self](const QString& label, const QString& s, const QString& k, bool on) { return self ? self->bindCheckField(label, s, k, on) : nullptr; };
		kit.controlNameFn = [](const QString& s, const QString& k) { return controlName(s, k); };
		kit.helpLabelFn = [](const QString& text) { return helpLabel(text); };
		kit.createCardFn = [self](const QString& title, const QString& help, QWidget* content) { return self ? self->createCard(title, help, content) : nullptr; };
		kit.createPageFn = [self](const QString& title, const QString& help, QWidget* body) { return self ? self->createPage(title, help, body) : nullptr; };
		kit.createCardGridFn = [] { return static_cast<QWidget*>(new ResponsiveCardGrid); };
		kit.addCardFn = [](QWidget* grid, QWidget* card) { static_cast<ResponsiveCardGrid*>(grid)->addCard(card); };
		kit.setKnownFn = [self](const QString& s, const QString& k, const QString& text)
		{
			if (!self || !self->document_) return;
			const std::string bytes = text.toUtf8().toStdString();
			if (!self->document_->SetKnown(s.toStdString(), k.toStdString().c_str(), bytes))
			{
				self->document_->AddSection(s.toStdString());
				self->document_->SetKnown(s.toStdString(), k.toStdString().c_str(), bytes);
			}
		};
		kit.removeKnownFn = [self](const QString& s, const QString& k) { if (self && self->document_) self->document_->RemoveKnown(s.toStdString(), k.toStdString().c_str()); };
		kit.configPathFn = [self] { return self ? self->configPath_ : QString(); };
		kit.markDirtyFn = [self](QObject* edited)
		{
			if (!self) return;
			if (edited && edited->objectName().startsWith(QStringLiteral("config.")))
				self->editOrder_[edited->objectName()] = ++self->editSerial_;
			self->markDirty();
		};
		kit.windowFn = [self] { return static_cast<QWidget*>(self.data()); };
		kit.currentPageFn = [self] { return (self && self->pages_) ? self->pages_->currentWidget() : nullptr; };
		kit.settingsPreviewFn = [] { return sandboxPreview(); };
		kit.noTargetFn = [self] { return self ? self->noTarget_ : false; };
		kit.remoteTargetFn = [self] { return self ? ConfigPageHost::EditorKit::RemoteTarget{ self->remoteClient_ != nullptr, self->remoteHost_, self->remotePort_ } : ConfigPageHost::EditorKit::RemoteTarget{}; };
		kit.documentDirtyFn = [self] { return self ? self->dirty_ : false; };
		kit.discardEditsFn = [self] { if (self) self->discardEdits(); };
		kit.setStatusFn = [self](const QString& text) { if (self) self->setStatus(text); };
		kit.setWarningStatusFn = [self](const QString& text) { if (self) self->setWarningStatus(text); };
	}

	void ConfigEditorAccess::BuildVpvPages(ConfigEditorWindow& window)
	{
		QPointer<ConfigEditorWindow> self(&window);
		self->vpvPages_.reset();
		self->vpvPages_.appendProviderPages(self->pages_, self->formApi_);   // sub-pages first, so the VP-V page can link them
		// VP-V section (owner 2026-10-24; src/VP-V/config/README.md). The whole VP-V section lives in src/VP-V/config/;
		// this and the "VP-V" navigation leaf are its only footprint in the editor.
		Host vpvHost;
		vpvHost.value = self->formApi_.valueFn;
		vpvHost.setValue = [self](const QString& section, const QString& key, const QString& text)
		{
			if (!self || !self->document_) return;
			if (text.isEmpty()) self->document_->RemoveKnown(section.toStdString(), key.toStdString().c_str());
			else self->document_->SetKnown(section.toStdString(), key.toStdString().c_str(), text.toUtf8().constData());
			self->markDirty();
			// The pathway decides which Dithering choices the Scaling page offers.
			if (key == QStringLiteral("pipeline_original_pathway"))
				self->pathwayRefreshers_.refresh();
		};
		vpvHost.profileSections = [self](const QString& root) { return self ? self->profileSections(root) : QStringList(); };
		vpvHost.upstreamControlName = [](const QString& key) { return controlName(QStringLiteral("vprenderer"), key); };
		vpvHost.upstreamRoot = self->pages_;
		vpvHost.subPages = [self](const QString& unitId)
		{
			std::vector<SubPage> subs;
			if (!self) return subs;
			for (const auto& sub : self->vpvPages_.subPagesOf(QStringLiteral("VP-V"), unitId))
				subs.push_back({ sub.name, sub.name, sub.description });
			return subs;
		};
		vpvHost.openPage = [self](const QString& pageName) { if (self) self->selectPage(pageName); };
		vpvHost.bothPathwayCards = [self] { return self ? BothPathwayCards(self->formApi_.kit) : std::vector<QWidget*>{}; };
		vpvHost.unitCards = [self](const QString& unitId) { return self ? WithGeneralPageCards(self->formApi_.kit, unitId, self->vpvPages_.takeUnitCards(unitId)) : std::vector<UnitCard>{}; };
		auto* vpvRoot = new QWidget;
		self->vpvPages_.append(self->pages_, QStringLiteral("VP-V"), self->createPage(QStringLiteral("VP-V"),
			QStringLiteral("Everything VP-V adds, by unit: one tier per unit plus a global tier, each unit's settings, and the fallbacks engaged."),
			vpvRoot));
		PopulateSection(vpvRoot, vpvHost);
	}
}
