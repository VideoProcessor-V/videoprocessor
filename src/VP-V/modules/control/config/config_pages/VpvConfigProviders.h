// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The Config page providers VP-V registers with the generic Config page host
// (VideoProcessor-Config/ConfigPageHost.h). One call at process start, before the first editor is
// created, registers them in the order their pages take in the editor: a sub-page appears in the
// stack in registration order, ahead of the master VP-V page (VpvConfigPages.h).
// Idempotent. Nothing here runs unless a process calls it, so an editor without the call is Bill's.

#include <VP-V/modules/drive/led_compensation/config_ui/LedDrivePage.h>
#include <VP-V/modules/output/calibration_lut/config_ui/CalibrationPage.h>
#include <VP-V/modules/control/config/config_pages/VpvQueueBillGrey.h>
#include <VP-V/modules/control/config/config_pages/VpvTierTable.generated.h>

namespace Vpv
{
	// GEM-040: re-applies VP-V's managed-control greying after every Rendering profile load/reload
	// through OnBillField (without setting derivedSetting, so save-time freeze keeps working).
	class ManagedBillGrey final : public ConfigPageHost::IConfigPageProvider
	{
	public:
		void OnBillField(const QString& sectionPrefix, const QString& key, QWidget* widget,
			ConfigPageHost::ConfigFormApi& api) override
		{
			if (!widget || sectionPrefix != QStringLiteral("vprenderer")) return;
			if (widget->objectName().startsWith(QStringLiteral("vpv."))) return;
			for (QWidget* p = widget; p; p = p->parentWidget())
				if (p->property("vpvKeyHome").toBool()) return;
			// Tier keys move into vpv.<unit>.card.tier_keys (vpvKeyHome) where LockKind::Tier owns their
			// enabled state; never grey them here (initial load runs before VpvSection moves them).
			static const std::set<std::string> kManagedToggles = []()
			{
				const Table& table = TierTable();
				std::set<std::string> keys = ManagedKeys(table);
				for (const Unit& u : table.units)
					for (const Preset& p : u.presets)
						for (const KeyValue& kv : p.set)
							keys.erase(kv.key);
				return keys;
			}();
			if (!kManagedToggles.count(key.toStdString())) return;
			const bool vpvOn = !ConfigOriginalPathwayOn(
				[&api](const QString& s, const QString& k) { return api.value(s, k); });
			if (!vpvOn) return;
			const QString prefix = QStringLiteral("Managed by VP-V: change it on the VP-V page (or switch VP-V to the "
				"Original pathway).");
			if (!widget->property("vpvManaged").toBool())
			{
				widget->setProperty("vpvWasEnabled", widget->isEnabled());
				widget->setProperty("vpvToolTip", widget->toolTip());
				widget->setProperty("vpvManaged", true);
			}
			else
			{
				// Profile reload rebuilt settingHelp tooltip; keep the refreshed base tooltip for Original restore.
				const QString curTip = widget->toolTip();
				if (!curTip.startsWith(prefix))
					widget->setProperty("vpvToolTip", curTip);
			}
			widget->setEnabled(false);
			const QString baseTip = widget->property("vpvToolTip").toString();
			widget->setToolTip(prefix + (baseTip.isEmpty() ? QString() : QStringLiteral("\n\n") + baseTip));
		}
	};

	inline ManagedBillGrey& ManagedBillGreyInstance()
	{
		static ManagedBillGrey instance;
		return instance;
	}

	inline void RegisterConfigProviders() noexcept
	{
		// Dynamic Drive first, then Calibration: the stack order is Dynamic Drive, Calibration, VP-V.
		if (!ConfigPageHost::Find<LedDrivePage>())
			ConfigPageHost::Register(&LedDrivePageInstance());
		if (!ConfigPageHost::Find<CalibrationPage>())
			ConfigPageHost::Register(&CalibrationPageInstance());
		// Bill's Queue page: his controls that VP-V's queue settings override are greyed with a pointer.
		if (!ConfigPageHost::Find<QueueBillGrey>())
			ConfigPageHost::Register(&QueueBillGreyInstance());
		// Bill's Rendering page: re-apply managed greying on every profile load without touching derivedSetting.
		if (!ConfigPageHost::Find<ManagedBillGrey>())
			ConfigPageHost::Register(&ManagedBillGreyInstance());
	}
}
