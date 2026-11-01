// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Config editor glue for the Original / VP-V pathway (moved out of ConfigEditorWindow):
// the Dithering choice set that follows the pathway, the pathway-refresh list, the pathway
// lookup and the Dithering Auto marker used while migrating profiles.

#include "VpvDitherChoices.h"

#include <QComboBox>
#include <QSignalBlocker>
#include <QString>
#include <functional>
#include <string>
#include <vector>

namespace Vpv
{
	// Dithering on the Scaling page (owner decision: keep both). Original pathway = Bill's four
	// choices exactly; otherwise VP-V's full list. Only the choice items and the tooltip change;
	// the stored value is never touched.
	inline void ApplyDitherChoiceSet(QComboBox* combo, bool original,
		const std::function<QString(const QString&)>& friendlyLabel)
	{
		if (!combo) return;
		const QString wanted = original ? QStringLiteral("original") : QStringLiteral("vpv");
		if (combo->property("ditherSet").toString() == wanted) return;
		const QSignalBlocker blocker(combo);
		for (int i = combo->count() - 1; i >= 0; --i)
			if (!combo->itemData(i).toString().isEmpty()) combo->removeItem(i);
		if (original)
		{
			for (const char* choice : OriginalDitherChoices())
				combo->addItem(friendlyLabel(QString::fromLatin1(choice)), QString::fromLatin1(choice));
			combo->setToolTip(combo->property("originalToolTip").toString());
		}
		else
		{
			for (const DitherChoice& choice : VpvDitherChoices())
				combo->addItem(QString::fromLatin1(choice.label), QString::fromLatin1(choice.data));
			combo->setToolTip(QStringLiteral(
				"Auto follows the rendering-quality preset: Blue noise for Balanced "
				"and High, or Off for Fast. Blue noise is libplacebo's default and "
				"recommended mode. Ordered modes and White noise are alternate "
				"libplacebo methods. Error-diffusion modes require compute shaders "
				"and are substantially more expensive. The display-calibration LUT "
				"runs before this final dithering stage. Off disables dithering."));
		}
		combo->setProperty("ditherSet", wanted);
	}

	// The control as built carries Bill's four choices: remember that set and its tooltip.
	inline void MarkOriginalDitherSet(QComboBox* dithering)
	{
		dithering->setProperty("ditherSet", QStringLiteral("original"));
		dithering->setProperty("originalToolTip", dithering->toolTip());
	}

	// Pages that re-read their rows when the pathway changes.
	class PathwayRefreshers
	{
	public:
		void clear() { refreshers_.clear(); }
		void add(std::function<void()> refresh) { refreshers_.push_back(std::move(refresh)); }
		void refresh() const { for (const auto& refresh : refreshers_) refresh(); }
	private:
		std::vector<std::function<void()>> refreshers_;
	};

	// The renderer's precedence for the pathway key: [vpv] first, then [vprenderer]. on = the
	// Original pathway. `lookup(section, key)` reads a configured value.
	template <class Lookup>
	bool ConfigOriginalPathwayOn(Lookup&& lookup)
	{
		QString v = lookup(QStringLiteral("vpv"), QStringLiteral("pipeline_original_pathway")).trimmed();
		if (v.isEmpty()) v = lookup(QStringLiteral("vprenderer"), QStringLiteral("pipeline_original_pathway")).trimmed();
		return v.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0;
	}

	// Dithering Auto is a VP-V value (VP-V mode): Bill's plan would rewrite it to the preset's
	// blue_noise. Hide it from the plan behind a marker and put it back unchanged, so the
	// migration moves it but never rewrites it. The Original pathway keeps Bill's behaviour.
	inline const std::string& DitherAutoMarker()
	{
		static const std::string marker = "\x01vpv-auto";
		return marker;
	}

	template <class Sections>
	void HideDitherAutoFromPlan(Sections& sections, bool original)
	{
		if (original) return;
		for (auto& entry : sections)
		{
			auto found = entry.second.find("dithering");
			if (found != entry.second.end() &&
				QString::fromStdString(found->second).trimmed().compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0)
				found->second = DitherAutoMarker();
		}
	}

	inline std::string RestoreDitherAuto(const std::string& value)
	{
		return value == DitherAutoMarker() ? std::string("AUTO") : value;
	}
}
