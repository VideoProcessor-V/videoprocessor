// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// VP-V's rows of the Scaling profile page, built by Bill's own field helper (addChoice: same keys,
// inheritance, Reset and save path, bound to the Scaling profile selected on VP Renderer > Scaling)
// but into forms of their own, which the VP-V page shows as unit cards (P8 step 2, owner rule
// 2026-11-01: Bill's pages stay exactly his; either/or with Original = Bill's control on his page).
//
//   Scaling unit, card "chroma":  VP-V's chroma upscaler list (key chroma_upscaler, object name
//                                 config.vprenderer.scaling.chroma_upscaler.vpv; Bill's own five-value
//                                 control stays on his page), chroma_upscale_vp, downscale_light.
//   Output unit, card "dither":   VP-V's Dithering list (key dithering, object name
//                                 config.vprenderer.scaling.dithering.vpv, its Auto status); Bill's
//                                 four-value Dithering control stays on his page, exactly his.
//
// The VP-V control and Bill's control of the same key follow each other when either is changed by
// the user (both are also loaded from the document on every profile change). A VP-V value that is
// not in Bill's list shows on his control the way his loader shows it. UI thread only; C++14.

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QObject>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QString>
#include <QVariant>
#include <QWidget>
#include <VP-V/config/VpvDitherChoiceSet.h>
#include <VP-V/config/VpvSection.h>
#include <functional>
#include <vector>

namespace Vpv
{
	namespace ScalingRowsDetail
	{
		// Renames a VP-V control of one of Bill's keys (and its Reset button) so his own control keeps
		// the key's plain object name.
		inline void RenameVpv(QComboBox* combo)
		{
			const QString billName = combo->objectName();
			if (QWidget* row = combo->parentWidget())
				if (auto* reset = row->findChild<QWidget*>(billName + QStringLiteral(".reset_override")))
					reset->setObjectName(billName + QStringLiteral(".vpv.reset_override"));
			if (auto* reset = combo->findChild<QWidget*>(billName + QStringLiteral(".reset_override")))
				reset->setObjectName(billName + QStringLiteral(".vpv.reset_override"));
			combo->setObjectName(billName + QStringLiteral(".vpv"));
		}

		inline bool ItemEnabled(QComboBox* combo, int index)
		{
			auto* model = qobject_cast<QStandardItemModel*>(combo->model());
			return !model || !model->item(index) || model->item(index)->isEnabled();
		}

		// Shows `value` on Bill's control as his loader shows a stored value: his item when he offers
		// it, otherwise (retiredLabel set) his "Saved legacy method" placeholder, else an added item.
		inline void ShowOnBills(QComboBox* bills, const QString& value, const QString& label, bool retiredStyle)
		{
			const QSignalBlocker blocker(bills);
			const int index = bills->findData(value, Qt::UserRole, Qt::MatchFixedString);
			if (index >= 0 && ItemEnabled(bills, index))
			{
				bills->setPlaceholderText(QString());
				bills->setCurrentIndex(index);
				return;
			}
			if (retiredStyle)
			{
				bills->setPlaceholderText(QStringLiteral("Saved legacy method: %1").arg(label));
				bills->setCurrentIndex(-1);
				return;
			}
			bills->addItem(label, value);
			bills->setCurrentIndex(bills->count() - 1);
		}

		// The user's choice on one control is shown on the other (the document is already written by the
		// edited control's own handler).
		inline void Follow(QComboBox* vpv, QComboBox* bills, bool retiredStyle)
		{
			if (!vpv || !bills) return;
			QObject::connect(vpv, qOverload<int>(&QComboBox::activated), vpv, [vpv, bills, retiredStyle](int index)
			{
				if (index < 0) return;
				ShowOnBills(bills, vpv->itemData(index).toString(), vpv->itemText(index), retiredStyle);
			});
			QObject::connect(bills, qOverload<int>(&QComboBox::activated), vpv, [vpv, bills](int index)
			{
				if (index < 0) return;
				const QSignalBlocker blocker(vpv);
				const int mine = vpv->findData(bills->itemData(index).toString(), Qt::UserRole, Qt::MatchFixedString);
				if (mine >= 0) vpv->setCurrentIndex(mine);
			});
		}

		inline QFormLayout* NewForm(QWidget* body)
		{
			auto* form = new QFormLayout(body);
			form->setContentsMargins(0, 0, 0, 0);
			form->setVerticalSpacing(8);
			return form;
		}
	}

	// Called once by the Scaling profile page, after Bill's Dithering rows (both of his controls exist), with his
	// `form` variable (restored before return), his addChoice and addRendererAutoStatus helpers, a
	// GPU-cost marker and the friendly-label function. The cards go to `cards` for the VP-V page.
	template <class AddChoice, class AddAutoStatus, class MarkCost>
	void AddVpvScalingRows(QFormLayout*& form, AddChoice& addChoice, AddAutoStatus& addRendererAutoStatus,
		MarkCost&& markGpuCost, const std::function<QString(const QString&)>& friendlyLabel, std::vector<UnitCard>& cards)
	{
		using namespace ScalingRowsDetail;
		QFormLayout* const bills = form;
		// The top of the page being built: Bill's Scaling and Debanding / Dithering sections are under it.
		QWidget* billsRoot = bills ? bills->parentWidget() : nullptr;
		while (billsRoot && billsRoot->parentWidget()) billsRoot = billsRoot->parentWidget();
		const QString scalingNote = QStringLiteral(
			"These rows edit the Scaling profile selected on VP Renderer > Scaling.");

		// ---- Scaling unit: chroma ----
		{
			auto* body = new QWidget;
			form = NewForm(body);
			auto* chromaUpscaler = addChoice(QStringLiteral("Chroma upscaler (VP-V list)"),
				QStringLiteral("chroma_upscaler"), { QStringLiteral("AUTO"),
				QStringLiteral("catmull_rom"), QStringLiteral("lanczos"), QStringLiteral("ewa_lanczos"),
				QStringLiteral("ewa_lanczossharp"), QStringLiteral("upscaler"), QStringLiteral("bicubic"),
				QStringLiteral("bilinear") });
			chromaUpscaler->setItemText(chromaUpscaler->findData(QStringLiteral("upscaler")),
				QStringLiteral("Same as the upscaler (the earlier behaviour)"));
			chromaUpscaler->setToolTip(QStringLiteral(
				"How the colour planes of 4:2:0 / 4:2:2 video are brought up to the picture's size before the "
				"main scaling. Auto = Catmull-Rom: on real footage it looked identical to the polar EWA Lanczos "
				"filter at about a seventh of its GPU time. 'Same as the "
				"upscaler' is the previous behaviour (EWA Lanczos sharp with High quality, the costliest). "
				"The same key as Bill's Chroma upscaler (VP Renderer > Scaling), which offers his five filters."));
			markGpuCost(form, chromaUpscaler, 2);
			RenameVpv(chromaUpscaler);
			auto* chromaUpscaleVp = addChoice(QStringLiteral("VP chroma upscale (candidate)"),
				QStringLiteral("chroma_upscale_vp"), { QStringLiteral("off"), QStringLiteral("cfl") });
			chromaUpscaleVp->setToolTip(QStringLiteral(
				"CfL predicts colour detail from the grain-free luma inside the active picture; chroma grain is "
				"kept exactly as the normal upscaler passes it. Off = libplacebo's chroma upscaler (above). "
				"Candidate: not yet active in the renderer."));
			markGpuCost(form, chromaUpscaleVp, 1);
			auto* downscaleLight = addChoice(QStringLiteral("Downscale light"),
				QStringLiteral("downscale_light"), { QStringLiteral("linear"), QStringLiteral("gamma") });
			downscaleLight->setToolTip(QStringLiteral(
				"Light domain for frames that are downscaled. Linear is libplacebo's "
				"default; gamma matches madVR's SSIM look. Upscaled frames always use linear light."));
			auto* note = new QLabel(scalingNote);
			note->setWordWrap(true);
			form->addRow(note);
			// VP-V's text about Bill's Debanding (his control on his page keeps his own text).
			auto* deband = new QLabel(QStringLiteral(
				"Bill's Debanding (VP Renderer > Scaling) is libplacebo's debander (smooths steps in skies and shadows); Auto "
				"lets libplacebo decide. While VP's own grain-safe debander runs ('VP deband' Auto or On, with 'VP deband "
				"replaces libplacebo's'), libplacebo's automatic deband is switched off because it thins fine grain; an "
				"explicit Standard or Light is kept."));
			deband->setObjectName(QStringLiteral("vpv.scaling.deband.note"));
			deband->setWordWrap(true);
			form->addRow(deband);
			if (billsRoot)
				Follow(chromaUpscaler, billsRoot->findChild<QComboBox*>(QStringLiteral("config.vprenderer.scaling.chroma_upscaler")), false);
			cards.push_back({ QStringLiteral("scaling"), QStringLiteral("chroma"), QStringLiteral("Chroma and downscale light"),
				QStringLiteral("VP-V's chroma choices. Original pathway: Bill's Chroma upscaler on VP Renderer > Scaling."), body });
		}

		// ---- Output unit: dither ----
		{
			auto* body = new QWidget;
			form = NewForm(body);
			auto* dithering = addChoice(QStringLiteral("Dithering (VP-V list)"), QStringLiteral("dithering"),
				{ "blue_noise", "ordered_fixed", "white_noise", "off" });
			// VP-V's full list (Auto, ordered_lut, the error_diffusion_* modes) on its own control; Bill's
			// loader shows its stored value as stored (no retired mapping).
			Vpv::ApplyDitherChoiceSet(dithering, false, friendlyLabel);
			dithering->setProperty("vpvOwnChoices", true);
			RenameVpv(dithering);
			addRendererAutoStatus(QStringLiteral("dithering"), dithering);
			auto* note = new QLabel(QStringLiteral(
				"Bill's Dithering control shows 'legacy method' while a VP method is selected; changing it there "
				"replaces the VP choice. ") + scalingNote);
			note->setObjectName(QStringLiteral("vpv.output.dither.note"));
			note->setWordWrap(true);
			form->addRow(note);
			if (billsRoot)
				Follow(dithering, billsRoot->findChild<QComboBox*>(QStringLiteral("config.vprenderer.scaling.dithering")), true);
			cards.push_back({ QStringLiteral("output"), QStringLiteral("dither_choice"), QStringLiteral("Dithering (VP-V list)"),
				QStringLiteral("VP-V's dithering modes. Original pathway: Bill's Dithering on VP Renderer > Scaling."), body });
		}
		form = bills;
	}
}
