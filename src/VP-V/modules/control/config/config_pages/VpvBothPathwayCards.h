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
#include <VP-V/modules/control/config/config_pages/VpvDitherChoiceSet.h>
#include <VideoProcessor-Config/ConfigPageHost.h>
#include <ShortcutDefaults.h>
#include <string>
#include <vector>

namespace Vpv
{
	// Updates the placeholder of [shortcuts] global_actions to reflect the active pathway's default
	// ("state_report" in VP-V, none in Original), keeping the text empty when the key is absent so
	// an absent key is never written (GEM-037).
	inline void RefreshGlobalActionsPlaceholder(QWidget* root, bool vpvOn)
	{
		if (!root) return;
		if (auto* edit = root->findChild<QLineEdit*>(QStringLiteral("config.shortcuts.global_actions")))
		{
			edit->setPlaceholderText(vpvOn ?
				QStringLiteral("Default: state_report (for example: state_report, config_editor, toggle_noui)") :
				QStringLiteral("Default: none (Original pathway; for example: config_editor, toggle_noui)"));
		}
	}
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
			card->setObjectName(QStringLiteral("vpv.system.card.") + id);
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

		// ---- Capture & Signal Bridges ----
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
			// Hunt 2 (2026-10-17): VP-0179 recovery policy, read once at startup (default on).
			if (QCheckBox* boundedRecovery = kit.bindCheckField(QStringLiteral("Bounded invalid-capture recovery"),
				QStringLiteral("general"), QStringLiteral("bounded_invalid_capture_recovery"), true))
			{
				boundedRecovery->setToolTip(QStringLiteral(
					"On (default): a bounded 1500 ms grace after an invalid capture state; only a valid capture state cancels "
					"recovery. Off: the earlier recovery, which keeps the frame counter. Takes effect when VP restarts."));
				column->addWidget(boundedRecovery);
			}
			// Restore switch 2026-10-17: [general] magewell_recover_faults (both pathways; MUST-C).
			if (QCheckBox* magewellRecover = kit.bindCheckField(QStringLiteral("Magewell: recover from capture faults (fix under test)"),
				QStringLiteral("general"), QStringLiteral("magewell_recover_faults"), true))
			{
				magewellRecover->setToolTip(QStringLiteral(
					"On (default): a Magewell fault the card recovers from (no frames for 2 s, a failed start, the capture "
					"memory budget, a slow frame copy) holds the video invalid and restarts the device by itself. Off: the "
					"earlier behaviour - capture stops and needs a manual Restart."));
				column->addWidget(magewellRecover);
			}
			auto* magewellNote = Help(kit, QStringLiteral(
				"Original pathway: switching this off is PENDING (it sits inside Bill's Magewell capture code; the change "
				"waits on the owner), so for now it still applies on Original too. Read when VideoProcessor starts."));
			magewellNote->setObjectName(QStringLiteral("vpv.both.magewell_recovery.note"));
			column->addWidget(magewellNote);
			AddCard(cards, kit, QStringLiteral("capture_bridges"), QStringLiteral("Capture & Signal Bridges"),
				QStringLiteral("Capture format, Dolby Vision hand-off, and capture recovery switches that act on both pathways."), body);
		}

		// ---- Global Shortcuts & Problem Reports ----
		{
			auto* body = new QWidget;
			QVBoxLayout* layout = Column(body);
			if (QCheckBox* globalHotkeys = kit.bindCheckField(
				QStringLiteral("Every shortcut works while another application has focus"),
				QStringLiteral("shortcuts"), QStringLiteral("global_hotkeys"), false))
			{
				globalHotkeys->setToolTip(QStringLiteral(
					"Off (recommended): shortcuts act only while VideoProcessor (its window or its fullscreen "
					"picture) has focus, except the actions listed below; keys pressed in another application "
					"(MPC-HC, a browser) are left to that application. On: every shortcut also acts, and the "
					"Config and video-only keys are taken, while another application has focus."));
				layout->addWidget(globalHotkeys);
			}
			// Bug hunt 3 (SH6): the actions that stay global with global_hotkeys off.
			{
				auto* globalActionsRow = new QWidget;
				auto* globalActionsLayout = new QHBoxLayout(globalActionsRow);
				globalActionsLayout->setContentsMargins(0, 0, 0, 0);
				auto* globalActionsLabel = new QLabel(QStringLiteral("Global actions"));
				// GEM-037 + GF-P5 S13: absent key stays empty so saving never writes it, while the placeholder
				// follows the file's pathway via ShortcutDefaults.h OriginalPathwayForConfig (same rule as the player).
				const QString absent = QStringLiteral("\x01");   // not a value a key can hold: "the key is absent"
				const bool originalOn = ::OriginalPathwayForConfig(
					[&kit, &absent](const char* section, const char* key, std::string& out)
					{
						const QString v = kit.value(QString::fromLatin1(section), QString::fromLatin1(key), absent);
						if (v == absent)
							return false;
						out = v.toStdString();
						return true;
					});
				const QString storedActions = kit.value(QStringLiteral("shortcuts"),
					QStringLiteral("global_actions"), QString());
				auto* globalActions = new QLineEdit(storedActions);
				globalActions->setObjectName(kit.controlName(QStringLiteral("shortcuts"), QStringLiteral("global_actions")));
				globalActions->setAccessibleName(QStringLiteral("Global actions"));
				globalActions->setPlaceholderText(!originalOn ?
					QStringLiteral("Default: state_report (for example: state_report, config_editor, toggle_noui)") :
					QStringLiteral("Default: none (Original pathway; for example: config_editor, toggle_noui)"));
				globalActions->setToolTip(QStringLiteral(
					"Comma-separated [shortcuts] actions that act while another application has focus, such as "
					"config_editor or toggle_noui. Default: state_report (Ctrl+Alt+D, report a problem) in the VP-V pathways, none in the "
					"Original pathway; empty: none. A switch of the Original pathway changes this default at the next start of VP, not "
					"at once. For MPC-HC file playback leave "
					"out actions whose keys MPC uses itself (Alt+Enter, Ctrl+1..4, Ctrl+I)."));
				QObject::connect(globalActions, &QLineEdit::textChanged, globalActions, [k, globalActions](const QString& text)
				{
					k->setKnown(QStringLiteral("shortcuts"), QStringLiteral("global_actions"), text.trimmed());
					k->markDirty(globalActions);
				});
				globalActionsLayout->addWidget(globalActionsLabel);
				globalActionsLayout->addWidget(globalActions, 1);
				layout->addWidget(globalActionsRow);
			}
			auto* abHelp = new QLabel(QStringLiteral(
				"Settings are the primary control; shortcuts are a backup for A/B comparison. A key's change "
				"lasts until restart, a profile change, or applying that setting here, and the statistics "
				"overlay shows it on its \"Key override\" line. Keys marked DirectShow only do nothing with "
				"VP Renderer."));
			abHelp->setWordWrap(true);
			abHelp->setObjectName(QStringLiteral("fieldHelp"));
			layout->addWidget(abHelp);

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
			AddCard(cards, kit, QStringLiteral("shortcuts_reports"), QStringLiteral("Global Shortcuts & Problem Reports"),
				QStringLiteral("Global shortcut behaviour and saved state reports (Bill's own shortcut and logging settings stay on his pages)."), body);
		}
		return cards;
	}
}
