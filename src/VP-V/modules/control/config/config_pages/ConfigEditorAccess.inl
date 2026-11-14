// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S4A A4 (refs/112 pattern P2): the Config editor's VP-V seams as one friend struct. Bill's
// ConfigEditorWindow.h carries one `friend struct Vpv::ConfigEditorAccess;` line and
// ConfigEditorWindow.cpp includes this file once, at its end (the editor's anonymous-namespace helpers
// ResponsiveCardGrid, helpLabel, controlName and sandboxPreview are visible there). The three seams it replaces
// were 100 hook lines of lambdas in the editor; their bodies are unchanged and now live here:
//   BindFormApi            fills formApi_ and its EditorKit for the provider pages (constructor);
//   BuildVpvPages          appends the provider sub-pages and the master VP-V page (shell build);
//   ApplyMotionAheadText   the VP-V wording of Bill's "Motion vectors computed ahead" choice.
// Included only by ConfigEditorWindow.cpp; nothing here runs unless the editor calls it.

#include <VP-V/modules/control/config/config_pages/VpvSection.h>

namespace Vpv
{
	void ConfigEditorAccess::BindFormApi(ConfigEditorWindow& window)
	{
		ConfigEditorWindow* const self = &window;
		FunctionFormApi& formApi = self->formApi_;
		formApi.valueFn = [self](const QString& section, const QString& key) { return self->value(section, key); };
		formApi.setValueFn = [self](const QString& section, const QString& key, const QString& text)
		{
			if (!self->document_) return;
			if (text.isEmpty()) self->document_->RemoveKnown(section.toStdString(), key.toStdString().c_str());
			else self->document_->SetKnown(section.toStdString(), key.toStdString().c_str(), text.toLocal8Bit().constData());
			if (self->centralWidget()) self->markDirty();
		};
		formApi.markDirtyFn = [self] { self->markDirty(); };
		formApi.profileSectionsFn = [self](const QString& root) { return self->profileSections(root); };
		// The editor's own field helpers and services for provider pages (ConfigPageHost::EditorKit).
		FunctionEditorKit& kit = formApi.kit;
		kit.valueFn = [self](const QString& s, const QString& k, const QString& fallback) { return self->value(s, k, fallback); };
		kit.bindTextFn = [self](const QString& s, const QString& k, const QString& fallback) { return self->bindTextField(s, k, fallback); };
		kit.bindChoiceFn = [self](const QString& s, const QString& k, const QStringList& values, const QStringList& labels) { return self->bindChoiceField(s, k, values, labels); };
		kit.bindCheckFn = [self](const QString& label, const QString& s, const QString& k, bool on) { return self->bindCheckField(label, s, k, on); };
		kit.controlNameFn = [](const QString& s, const QString& k) { return controlName(s, k); };
		kit.helpLabelFn = [](const QString& text) { return helpLabel(text); };
		kit.createCardFn = [self](const QString& title, const QString& help, QWidget* content) { return self->createCard(title, help, content); };
		kit.createPageFn = [self](const QString& title, const QString& help, QWidget* body) { return self->createPage(title, help, body); };
		kit.createCardGridFn = [] { return static_cast<QWidget*>(new ResponsiveCardGrid); };
		kit.addCardFn = [](QWidget* grid, QWidget* card) { static_cast<ResponsiveCardGrid*>(grid)->addCard(card); };
		kit.setKnownFn = [self](const QString& s, const QString& k, const QString& text)
		{
			if (!self->document_) return;
			if (!self->document_->SetKnown(s.toStdString(), k.toStdString().c_str(), text.toLocal8Bit().constData()))
			{
				self->document_->AddSection(s.toStdString());
				self->document_->SetKnown(s.toStdString(), k.toStdString().c_str(), text.toLocal8Bit().constData());
			}
		};
		kit.removeKnownFn = [self](const QString& s, const QString& k) { if (self->document_) self->document_->RemoveKnown(s.toStdString(), k.toStdString().c_str()); };
		kit.configPathFn = [self] { return self->configPath_; };
		kit.markDirtyFn = [self](QObject* edited)
		{
			if (edited && edited->objectName().startsWith(QStringLiteral("config.")))
				self->editOrder_[edited->objectName()] = ++self->editSerial_;
			self->markDirty();
		};
		kit.windowFn = [self] { return static_cast<QWidget*>(self); };
		kit.currentPageFn = [self] { return self->pages_ ? self->pages_->currentWidget() : nullptr; };
		kit.settingsPreviewFn = [] { return sandboxPreview(); };
		kit.noTargetFn = [self] { return self->noTarget_; };
		kit.remoteTargetFn = [self] { return ConfigPageHost::EditorKit::RemoteTarget{ self->remoteClient_ != nullptr, self->remoteHost_, self->remotePort_ }; };
		kit.documentDirtyFn = [self] { return self->dirty_; };
		kit.discardEditsFn = [self] { self->discardEdits(); };
		kit.setStatusFn = [self](const QString& text) { self->setStatus(text); };
		kit.setWarningStatusFn = [self](const QString& text) { self->setWarningStatus(text); };
	}

	void ConfigEditorAccess::BuildVpvPages(ConfigEditorWindow& window)
	{
		ConfigEditorWindow* const self = &window;
		self->vpvPages_.reset();
		self->vpvPages_.appendProviderPages(self->pages_, self->formApi_);   // sub-pages first, so the VP-V page can link them
		// VP-V section (owner 2026-10-24; src/VP-V/config/README.md). The whole VP-V section lives in src/VP-V/config/;
		// this and the "VP-V" navigation leaf are its only footprint in the editor.
		Host vpvHost;
		vpvHost.value = [self](const QString& section, const QString& key) { return self->value(section, key); };
		vpvHost.setValue = [self](const QString& section, const QString& key, const QString& text)
		{
			if (!self->document_) return;
			if (text.isEmpty()) self->document_->RemoveKnown(section.toStdString(), key.toStdString().c_str());
			else self->document_->SetKnown(section.toStdString(), key.toStdString().c_str(), text.toLocal8Bit().constData());
			self->markDirty();
			// The pathway decides which Dithering choices the Scaling page offers.
			if (key == QStringLiteral("pipeline_original_pathway"))
				self->pathwayRefreshers_.refresh();
		};
		vpvHost.profileSections = [self](const QString& root) { return self->profileSections(root); };
		vpvHost.upstreamControlName = [](const QString& key) { return controlName(QStringLiteral("vprenderer"), key); };
		vpvHost.upstreamRoot = self->pages_;
		vpvHost.subPages = [self](const QString& unitId)
		{
			std::vector<SubPage> subs;
			for (const auto& sub : self->vpvPages_.subPagesOf(QStringLiteral("VP-V"), unitId))
				subs.push_back({ sub.name, sub.name, sub.description });
			return subs;
		};
		vpvHost.openPage = [self](const QString& pageName) { self->selectPage(pageName); };
		vpvHost.bothPathwayCards = [self] { return BothPathwayCards(self->formApi_.kit); };
		vpvHost.unitCards = [self](const QString& unitId) { return self->vpvPages_.takeUnitCards(unitId); };
		self->vpvPages_.append(self->pages_, QStringLiteral("VP-V"), self->createPage(QStringLiteral("VP-V"),
			QStringLiteral("Everything VP-V adds, by unit: one tier per unit plus a global tier, each unit's settings, and the fallbacks engaged."),
			CreateSection(vpvHost)));
	}

	void ApplyMotionAheadText(QComboBox* motionAhead)
	{
		// B1 (owner 2026-11-10, doc 91 C): the value is the minimum; the look-ahead runs every frame queued ahead.
		motionAhead->setItemText(motionAhead->findData(QStringLiteral("4")), QStringLiteral("On (looks ahead across the queued frames)"));
		motionAhead->setItemText(motionAhead->findData(QStringLiteral("0")), QStringLiteral("Off"));
		// Settings guide audit 2026-10-24: the forced 5-frame queue minimum is gone (r6 fix 2) and the
		// look-ahead is a time (r6 fix 3), so the old "holds at least 5" text was wrong.
		motionAhead->setToolTip(QStringLiteral(
			"NVIDIA GPUs: the optical-flow engine computes the motion ahead of the picture on its own hardware, off "
			"the render path. On: the look-ahead runs every frame queued ahead of render (up to 13 frames); the value "
			"is the minimum the queue keeps for it (about 67 ms: 2 frames at 24p, 4 at 50/60p). It never lengthens "
			"the queue beyond that minimum; on a short queue it looks less far ahead, and with no frames ahead the "
			"de-judder estimates at render time or holds. The low-latency queue switches it off. Off, or other GPUs: "
			"VP's matcher at render time."));
	}
}
