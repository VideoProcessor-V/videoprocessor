// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// VP-V rows that used to sit inside Bill's pages (P8 step 2, owner rule 2026-11-01: Bill's pages stay
// exactly his). They are global keys that act on BOTH pathways (the Original pathway does not switch
// them off), so they live in cards below the VP-V page's unit tabs, outside the part the Original
// pathway greys. Built through the editor's own field helpers (EditorKit): same object names, same
// keys, defaults and save/load round trip as before the move. Labels and tooltips are unchanged.
//
//   Input                             [decklink] ycbcr_12bit_capture (was Bill's General > Hardware card);
//                                     [general] dv_tunnel_handoff, newlldv_sdr_veto (were on Bill's DV page).
//   Problem reports                   [logging] auto_report, report_thumbnail, the reports folder button
//                                     (were on Bill's Logs page).
//   Keyboard                          [shortcuts] global_hotkeys, global_actions (were on Bill's Shortcuts
//                                     page; either/or DIV-D-06: Original = Bill's foreground_only row).
//   Bill settings without a Bill control
//                                     [general] bounded_invalid_capture_recovery: Bill's key, VP's control
//                                     (was on Bill's General > General behavior card).
// UI thread only.

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <VideoProcessor-Config/ConfigPageHost.h>
#include <vector>

namespace Vpv
{
	namespace BothPathwayDetail
	{
		inline QVBoxLayout* Column(QWidget* body)
		{
			auto* layout = new QVBoxLayout(body);
			layout->setContentsMargins(0, 0, 0, 0);
			layout->setSpacing(8);
			return layout;
		}
		inline QLabel* Help(ConfigPageHost::EditorKit& kit, const QString& text)
		{
			QLabel* label = kit.helpLabel(text);
			if (!label)
			{
				label = new QLabel(text);
				label->setWordWrap(true);
			}
			return label;
		}
		inline void AddCard(std::vector<QWidget*>& cards, ConfigPageHost::EditorKit& kit, const QString& id,
			const QString& title, const QString& description, QWidget* body)
		{
			QWidget* card = kit.createCard(title, description, body);
			if (!card) card = body;
			card->setObjectName(QStringLiteral("vpv.both.card.") + id);
			card->setProperty("vpvCardTitle", title);
			cards.push_back(card);
		}
	}

	// The cards, in page order. Each control is created once per editor shell.
	inline std::vector<QWidget*> BothPathwayCards(ConfigPageHost::EditorKit& kit)
	{
		using namespace BothPathwayDetail;
		std::vector<QWidget*> cards;
		ConfigPageHost::EditorKit* k = &kit;

		// ---- Input ----
		{
			auto* body = new QWidget;
			QVBoxLayout* column = Column(body);
			auto* form = new QFormLayout;
			form->setContentsMargins(0, 0, 0, 0);
			form->setVerticalSpacing(8);
			column->addLayout(form);
			if (QComboBox* ycbcr12 = kit.bindChoiceField(QStringLiteral("decklink"), QStringLiteral("ycbcr_12bit_capture"),
				{ QStringLiteral("v210"), QStringLiteral("rgb12"), QStringLiteral("off") },
				{ QStringLiteral("10-bit 4:2:2, keeps BTB/WTW (default)"),
				  QStringLiteral("12-bit RGB from the card, clips BTB/WTW (earlier default)"),
				  QStringLiteral("Refuse the signal (older behaviour)") }))
			{
				ycbcr12->setToolTip(QStringLiteral(
					"DeckLink only: a source sending 12-bit YCbCr 4:2:2 (some players and consoles); the SDK has no 12-bit "
					"YCbCr capture format. 10-bit 4:2:2 (default): the signal as sent minus the lowest two bits; "
					"below-black/above-white kept and VP owns the colour matrix. 12-bit RGB: the card converts to full-range "
					"12-bit RGB when it offers that for the mode (all 12 bits, but the card's conversion clips "
					"below-black/above-white and out-of-gamut values), otherwise 10-bit 4:2:2. Refuse: no picture."));
				form->addRow(QStringLiteral("12-bit YCbCr signals (DeckLink)"), ycbcr12);
			}
			if (QCheckBox* handoff = kit.bindCheckField(
				QStringLiteral("Hand LLDV / HDR10 sent as RGB to the normal path"),
				QStringLiteral("general"), QStringLiteral("dv_tunnel_handoff"), true))
			{
				handoff->setToolTip(QStringLiteral(
					"With DV Tunneling, a signal that carries no tunnel metadata for about a second and is either 10-bit RGB "
					"or BT.2020 RGB without an HDR flag (LLDV or HDR10 converted to RGB, for example by an HDFury) leaves the "
					"tunnel path: it is shown normally and treated as PQ HDR (8-bit LLDV included) instead of staying black or "
					"being shown as SDR. A tunnel arriving later takes the signal back. LLDV and HDR10 sent as YCbCr are also "
					"declined by the tunnel (it is only ever 8-bit RGB), even under a profile set to DV Tunneling, and get the "
					"normal LLDV / newlldv handling. Default on. Restart VideoProcessor after changing it. Key: dv_tunnel_handoff."));
				column->addWidget(handoff);
			}
			if (QCheckBox* sdrVeto = kit.bindCheckField(
				QStringLiteral("Return real SDR to SDR from the picture (SDR veto)"),
				QStringLiteral("general"), QStringLiteral("newlldv_sdr_veto"), true))
			{
				sdrVeto->setToolTip(QStringLiteral(
					"With Bill's \"Alternate LLDV detection\" (DV page), SDR sent as BT.2020 returns to SDR when the picture "
					"shows SDR highlights (at a cut, after evidence in two shots; once per signal). Default on. Key: newlldv_sdr_veto."));
				column->addWidget(sdrVeto);
			}
			// The VP explanation of Bill's newlldv switch (his label and help are back on his DV page).
			column->addWidget(Help(kit, QStringLiteral(
				"Bill's \"Alternate LLDV detection\" (DV page, key newlldv): a BT.2020, 10/12-bit signal without an HDR "
				"InfoFrame is shown as PQ HDR after 1.5 s: LLDV, and HDR10 whose HDR flag was dropped (for example an HDFury "
				"with its HDR toggle off). Metadata comes from the LLDV profile; peak detection works as usual. Never for the "
				"DV tunnel, BT.709 or a source that sends an HDR InfoFrame. These switches apply to all LLDV profiles and to "
				"HDR10 without an HDR flag. Restart VideoProcessor after changing them.")));
			AddCard(cards, kit, QStringLiteral("input"), QStringLiteral("Input (both pathways)"),
				QStringLiteral("Capture and signal switches VP-V adds; they act on both pathways."), body);
		}

		// ---- Problem reports ----
		{
			auto* body = new QWidget;
			QVBoxLayout* layout = Column(body);
			// Report-a-problem (StateReportSettings.h): the switches beside the
			// Shortcuts page's "Save problem report" (default Ctrl+Alt+D).
			auto* reportsTitle = new QLabel(QStringLiteral("Problem reports"));
			reportsTitle->setObjectName(QStringLiteral("config.logging.reports_title"));
			QFont reportsFont = reportsTitle->font();
			reportsFont.setBold(true);
			reportsTitle->setFont(reportsFont);
			layout->addWidget(reportsTitle);
			if (QCheckBox* autoReport = kit.bindCheckField(QStringLiteral("Save a problem report automatically"),
				QStringLiteral("logging"), QStringLiteral("auto_report"), false))
			{
				autoReport->setAccessibleDescription(QStringLiteral(
					"When VP recovers from a stall or a source gap, or a renderer reset fails, it saves a problem report "
					"by itself: at most one every 10 minutes and five per run. Default: off."));
				layout->addWidget(autoReport);
			}
			if (QCheckBox* reportThumbnail = kit.bindCheckField(QStringLiteral("Include a small picture in problem reports"),
				QStringLiteral("logging"), QStringLiteral("report_thumbnail"), false))
			{
				reportThumbnail->setAccessibleDescription(QStringLiteral(
					"Adds a 480-pixel-wide copy of the next rendered frame to each report (VP Renderer only). "
					"Default: off - reports then hold no pictures."));
				layout->addWidget(reportThumbnail);
			}
			layout->addWidget(Help(kit, QStringLiteral(
				"Press the problem-report shortcut (Shortcuts page, default Ctrl+Alt+D) or choose \"Save problem report\" "
				"in VP's window menu right after you see a problem. VP saves the last few seconds of its state - timings, "
				"decisions, Dynamic Drive commands, settings, the log and system details, without your name or private "
				"folder paths - to logs\\reports (the newest 20 are kept). Send the .zip file with a short description.")));
			const bool remote = kit.remoteTarget().remote;
			auto* openReports = new QPushButton(QStringLiteral("Open reports folder"));
			openReports->setObjectName(QStringLiteral("config.logging.open_reports"));
			openReports->setEnabled(!remote);
			if (remote)
				openReports->setToolTip(QStringLiteral("Reports are on the VP computer."));
			openReports->setAccessibleDescription(QStringLiteral("Open the folder containing VideoProcessor problem reports."));
			auto* actions = new QHBoxLayout;
			actions->addWidget(openReports);
			actions->addStretch();
			layout->addLayout(actions);
			const QString logDirectory = QFileInfo(kit.configPath()).absoluteDir().filePath(QStringLiteral("logs"));
			const QString reportsDirectory = QDir(logDirectory).filePath(QStringLiteral("reports"));
			QObject::connect(openReports, &QPushButton::clicked, openReports, [k, reportsDirectory]
			{
				if (!QDir(reportsDirectory).exists())
				{
					QMessageBox::information(k->window(), QStringLiteral("Problem reports"),
						QStringLiteral("No problem report has been saved yet. Press the problem-report shortcut in VP first."));
					return;
				}
				if (!QDesktopServices::openUrl(QUrl::fromLocalFile(reportsDirectory)))
					QMessageBox::warning(k->window(), QStringLiteral("Problem reports"),
						QStringLiteral("Windows could not open the reports folder."));
			});
			AddCard(cards, kit, QStringLiteral("reports"), QStringLiteral("Problem reports"),
				QStringLiteral("Saved state for reporting a problem (Bill's logging settings are on his Logs page)."), body);
		}

		// ---- Keyboard ----
		{
			auto* body = new QWidget;
			QVBoxLayout* handlingLayout = Column(body);
			if (QCheckBox* globalHotkeys = kit.bindCheckField(
				QStringLiteral("Every shortcut works while another application has focus"),
				QStringLiteral("shortcuts"), QStringLiteral("global_hotkeys"), false))
			{
				globalHotkeys->setToolTip(QStringLiteral(
					"Off (recommended): shortcuts act only while VideoProcessor (its window or its fullscreen "
					"picture) has focus, except the actions listed below; keys pressed in another application "
					"(MPC-HC, a browser) are left to that application. On: every shortcut also acts, and the "
					"Config and video-only keys are taken, while another application has focus."));
				handlingLayout->addWidget(globalHotkeys);
			}
			// Bug hunt 3 (SH6): the actions that stay global with global_hotkeys off.
			{
				auto* globalActionsRow = new QWidget;
				auto* globalActionsLayout = new QHBoxLayout(globalActionsRow);
				globalActionsLayout->setContentsMargins(0, 0, 0, 0);
				auto* globalActionsLabel = new QLabel(QStringLiteral("Global actions"));
				auto* globalActions = new QLineEdit(kit.value(QStringLiteral("shortcuts"),
					QStringLiteral("global_actions"), QString()));
				globalActions->setObjectName(kit.controlName(QStringLiteral("shortcuts"), QStringLiteral("global_actions")));
				globalActions->setAccessibleName(QStringLiteral("Global actions"));
				globalActions->setPlaceholderText(QStringLiteral("none (for example: config_editor, toggle_noui)"));
				globalActions->setToolTip(QStringLiteral(
					"Comma-separated [shortcuts] actions that act while another application has focus, such as "
					"config_editor or toggle_noui. Empty (the default): none. For MPC-HC file playback leave "
					"out actions whose keys MPC uses itself (Alt+Enter, Ctrl+1..4, Ctrl+I)."));
				QObject::connect(globalActions, &QLineEdit::textChanged, globalActions, [k, globalActions](const QString& text)
				{
					k->setKnown(QStringLiteral("shortcuts"), QStringLiteral("global_actions"), text.trimmed());
					k->markDirty(globalActions);
				});
				globalActionsLayout->addWidget(globalActionsLabel);
				globalActionsLayout->addWidget(globalActions, 1);
				handlingLayout->addWidget(globalActionsRow);
			}
			auto* abHelp = new QLabel(QStringLiteral(
				"Settings are the primary control; shortcuts are a backup for A/B comparison. A key's change "
				"lasts until restart, a profile change, or applying that setting here, and the statistics "
				"overlay shows it on its \"Key override\" line. Keys marked DirectShow only do nothing with "
				"VP Renderer."));
			abHelp->setWordWrap(true);
			abHelp->setObjectName(QStringLiteral("fieldHelp"));
			handlingLayout->addWidget(abHelp);
			AddCard(cards, kit, QStringLiteral("keyboard"), QStringLiteral("Keyboard"),
				QStringLiteral("VP-V's global-shortcut switch. Original = Bill's \"Only process shortcuts while VideoProcessor is in "
					"the foreground\" (Shortcuts page)."), body);
		}

		// ---- Bill settings without a Bill control ----
		{
			auto* body = new QWidget;
			QVBoxLayout* behaviorLayout = Column(body);
			// Hunt 2 (2026-10-17): VP-0179 recovery policy, read once at startup (default on).
			if (QCheckBox* boundedRecovery = kit.bindCheckField(QStringLiteral("Bounded invalid-capture recovery"),
				QStringLiteral("general"), QStringLiteral("bounded_invalid_capture_recovery"), true))
			{
				boundedRecovery->setToolTip(QStringLiteral(
					"On (default): a bounded 1500 ms grace after an invalid capture state; only a valid capture state cancels "
					"recovery. Off: the earlier recovery, which keeps the frame counter. Takes effect when VP restarts."));
				behaviorLayout->addWidget(boundedRecovery);
			}
			AddCard(cards, kit, QStringLiteral("bill"), QStringLiteral("Bill settings without a Bill control"),
				QStringLiteral("Bill's own keys that his editor has no control for; VP-V shows them here. They act on both pathways."), body);
		}
		return cards;
	}
}
