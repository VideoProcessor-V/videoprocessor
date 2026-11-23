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
#include <VP-V/core/AheadCeiling.h>

#include <QPointer>

namespace Vpv
{
	void ConfigEditorAccess::BindFormApi(ConfigEditorWindow& window)
	{
		EnsureSectionStyleSheet();   // install VP-V's property-scoped rules on qApp once before any widget exists
		QPointer<ConfigEditorWindow> self(&window);
		FunctionFormApi& formApi = self->formApi_;
		formApi.valueFn = [self](const QString& section, const QString& key) { return self ? self->value(section, key) : QString(); };
		formApi.setValueFn = [self](const QString& section, const QString& key, const QString& text)
		{
			if (!self || !self->document_) return;
			if (text.isEmpty()) self->document_->RemoveKnown(section.toStdString(), key.toStdString().c_str());
			else self->document_->SetKnown(section.toStdString(), key.toStdString().c_str(), text.toLocal8Bit().constData());
			if (self->centralWidget()) self->markDirty();
		};
		formApi.markDirtyFn = [self] { if (self) self->markDirty(); };
		formApi.profileSectionsFn = [self](const QString& root) { return self ? self->profileSections(root) : QStringList(); };
		// The editor's own field helpers and services for provider pages (ConfigPageHost::EditorKit).
		FunctionEditorKit& kit = formApi.kit;
		kit.valueFn = [self](const QString& s, const QString& k, const QString& fallback) { return self ? self->value(s, k, fallback) : fallback; };
		kit.bindTextFn = [self](const QString& s, const QString& k, const QString& fallback) { return self ? self->bindTextField(s, k, fallback) : nullptr; };
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
			if (!self->document_->SetKnown(s.toStdString(), k.toStdString().c_str(), text.toLocal8Bit().constData()))
			{
				self->document_->AddSection(s.toStdString());
				self->document_->SetKnown(s.toStdString(), k.toStdString().c_str(), text.toLocal8Bit().constData());
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
		vpvHost.value = [self](const QString& section, const QString& key) { return self ? self->value(section, key) : QString(); };
		vpvHost.setValue = [self](const QString& section, const QString& key, const QString& text)
		{
			if (!self || !self->document_) return;
			if (text.isEmpty()) self->document_->RemoveKnown(section.toStdString(), key.toStdString().c_str());
			else self->document_->SetKnown(section.toStdString(), key.toStdString().c_str(), text.toLocal8Bit().constData());
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
		vpvHost.unitCards = [self](const QString& unitId) { return self ? self->vpvPages_.takeUnitCards(unitId) : std::vector<UnitCard>{}; };
		auto* vpvRoot = new QWidget;
		self->vpvPages_.append(self->pages_, QStringLiteral("VP-V"), self->createPage(QStringLiteral("VP-V"),
			QStringLiteral("Everything VP-V adds, by unit: one tier per unit plus a global tier, each unit's settings, and the fallbacks engaged."),
			vpvRoot));
		PopulateSection(vpvRoot, vpvHost);
	}

	void ApplyMotionAheadText(QComboBox* motionAhead)
	{
		// B1 (owner 2026-11-10, doc 91 C): the value is the minimum; the look-ahead runs every frame queued ahead.
		motionAhead->setItemText(motionAhead->findData(QStringLiteral("4")), QStringLiteral("On (looks ahead across the queued frames)"));
		motionAhead->setItemText(motionAhead->findData(QStringLiteral("0")), QStringLiteral("Off"));
		// Settings guide audit 2026-10-24: the forced 5-frame queue minimum is gone (r6 fix 2) and the
		// look-ahead is a time (r6 fix 3), so the old "holds at least 5" text was wrong.
		// C2 gate SHOULD 3 (rule 6): the ceiling in the text is VpSkeleton::kAheadCeiling, never a literal.
		motionAhead->setToolTip(QStringLiteral(
			"NVIDIA GPUs: the optical-flow engine computes the motion ahead of the picture on its own hardware, off "
			"the render path. On: the look-ahead runs every frame queued ahead of render (up to the ahead-compute ceiling, %1 frames); the value "
			"is the minimum the queue keeps for it (about 67 ms: 2 frames at 24p, 4 at 50/60p). It never lengthens "   // GENERAL-REF: worked example in user-facing text; the depth is derived from the measured content rate
			"the queue beyond that minimum; on a short queue it looks less far ahead, and with no frames ahead the "
			"de-judder estimates at render time or holds. The low-latency queue switches it off. Off, or other GPUs: "
			"VP's matcher at render time.").arg(VpSkeleton::kAheadCeiling));
	}
}
