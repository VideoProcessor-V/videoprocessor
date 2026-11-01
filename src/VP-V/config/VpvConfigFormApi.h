// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Adapter that lets the generic Config page host (VideoProcessor-Config/ConfigPageHost.h) talk to
// the editor: the editor fills in four callables, nothing else of the editor is visible.

#include <VideoProcessor-Config/ConfigPageHost.h>

#include <functional>

namespace Vpv
{
	class FunctionFormApi : public ConfigPageHost::ConfigFormApi
	{
	public:
		std::function<QString(const QString&, const QString&)> valueFn;
		std::function<void(const QString&, const QString&, const QString&)> setValueFn;
		std::function<void()> markDirtyFn;
		std::function<QStringList(const QString&)> profileSectionsFn;

		QString value(const QString& section, const QString& key) const override
		{ return valueFn ? valueFn(section, key) : QString(); }
		void setValue(const QString& section, const QString& key, const QString& text) override
		{ if (setValueFn) setValueFn(section, key, text); }
		void markDirty() override { if (markDirtyFn) markDirtyFn(); }
		QStringList profileSections(const QString& root) const override
		{ return profileSectionsFn ? profileSectionsFn(root) : QStringList(); }
	};
}
