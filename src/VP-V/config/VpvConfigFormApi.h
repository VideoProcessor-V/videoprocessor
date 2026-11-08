// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Adapter that lets the generic Config page host (VideoProcessor-Config/ConfigPageHost.h) talk to
// the editor: the editor fills in callables, nothing else of the editor is visible.
//   FunctionFormApi    the four document callables (ConfigFormApi);
//   FunctionEditorKit  the editor's own field helpers and services (EditorKit, R23 and R25), reached
//                      through FunctionFormApi::editorKit(). A callable the editor leaves empty gives
//                      the EditorKit default (null / empty / false).

#include <VideoProcessor-Config/ConfigPageHost.h>

#include <functional>

namespace Vpv
{
	class FunctionEditorKit : public ConfigPageHost::EditorKit
	{
	public:
		std::function<QString(const QString&, const QString&, const QString&)> valueFn;
		std::function<QLineEdit*(const QString&, const QString&, const QString&)> bindTextFn;
		std::function<QComboBox*(const QString&, const QString&, const QStringList&, const QStringList&)> bindChoiceFn;
		std::function<QCheckBox*(const QString&, const QString&, const QString&, bool)> bindCheckFn;
		std::function<QString(const QString&, const QString&)> controlNameFn;
		std::function<QLabel*(const QString&)> helpLabelFn;
		std::function<QWidget*(const QString&, const QString&, QWidget*)> createCardFn;
		std::function<QWidget*(const QString&, const QString&, QWidget*)> createPageFn;
		std::function<QWidget*()> createCardGridFn;
		std::function<void(QWidget*, QWidget*)> addCardFn;
		std::function<void(const QString&, const QString&, const QString&)> setKnownFn;
		std::function<void(QObject*)> markDirtyFn;
		std::function<QWidget*()> windowFn;
		std::function<QWidget*()> currentPageFn;
		std::function<bool()> settingsPreviewFn;
		std::function<bool()> noTargetFn;
		std::function<RemoteTarget()> remoteTargetFn;
		std::function<bool()> documentDirtyFn;
		std::function<void()> discardEditsFn;
		std::function<void(const QString&)> setStatusFn;
		std::function<void(const QString&)> setWarningStatusFn;

		QString value(const QString& section, const QString& key, const QString& fallback) const override
		{ return valueFn ? valueFn(section, key, fallback) : fallback; }
		QLineEdit* bindTextField(const QString& section, const QString& key, const QString& fallback) override
		{ return bindTextFn ? bindTextFn(section, key, fallback) : nullptr; }
		QComboBox* bindChoiceField(const QString& section, const QString& key, const QStringList& values, const QStringList& labels) override
		{ return bindChoiceFn ? bindChoiceFn(section, key, values, labels) : nullptr; }
		QCheckBox* bindCheckField(const QString& label, const QString& section, const QString& key, bool defaultValue) override
		{ return bindCheckFn ? bindCheckFn(label, section, key, defaultValue) : nullptr; }
		QString controlName(const QString& section, const QString& key) const override
		{ return controlNameFn ? controlNameFn(section, key) : EditorKit::controlName(section, key); }
		QLabel* helpLabel(const QString& text) override { return helpLabelFn ? helpLabelFn(text) : nullptr; }
		QWidget* createCard(const QString& title, const QString& description, QWidget* content) override
		{ return createCardFn ? createCardFn(title, description, content) : content; }
		QWidget* createPage(const QString& title, const QString& description, QWidget* body) override
		{ return createPageFn ? createPageFn(title, description, body) : body; }
		QWidget* createCardGrid() override { return createCardGridFn ? createCardGridFn() : nullptr; }
		void addCard(QWidget* grid, QWidget* card) override { if (addCardFn) addCardFn(grid, card); }
		void setKnown(const QString& section, const QString& key, const QString& text) override
		{ if (setKnownFn) setKnownFn(section, key, text); }
		void markDirty(QObject* edited) override { if (markDirtyFn) markDirtyFn(edited); }
		QWidget* window() override { return windowFn ? windowFn() : nullptr; }
		QWidget* currentPage() const override { return currentPageFn ? currentPageFn() : nullptr; }
		bool settingsPreview() const override { return settingsPreviewFn && settingsPreviewFn(); }
		bool noTarget() const override { return noTargetFn && noTargetFn(); }
		RemoteTarget remoteTarget() const override { return remoteTargetFn ? remoteTargetFn() : RemoteTarget(); }
		bool documentDirty() const override { return documentDirtyFn && documentDirtyFn(); }
		void discardEdits() override { if (discardEditsFn) discardEditsFn(); }
		void setStatus(const QString& text) override { if (setStatusFn) setStatusFn(text); }
		void setWarningStatus(const QString& text) override { if (setWarningStatusFn) setWarningStatusFn(text); }
	};

	class FunctionFormApi : public ConfigPageHost::ConfigFormApi
	{
	public:
		std::function<QString(const QString&, const QString&)> valueFn;
		std::function<void(const QString&, const QString&, const QString&)> setValueFn;
		std::function<void()> markDirtyFn;
		std::function<QStringList(const QString&)> profileSectionsFn;
		FunctionEditorKit kit;

		QString value(const QString& section, const QString& key) const override
		{ return valueFn ? valueFn(section, key) : QString(); }
		void setValue(const QString& section, const QString& key, const QString& text) override
		{ if (setValueFn) setValueFn(section, key, text); }
		void markDirty() override { if (markDirtyFn) markDirtyFn(); }
		QStringList profileSections(const QString& root) const override
		{ return profileSectionsFn ? profileSectionsFn(root) : QStringList(); }
		ConfigPageHost::EditorKit* editorKit() override { return &kit; }
	};
}
