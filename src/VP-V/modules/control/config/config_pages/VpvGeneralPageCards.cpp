// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// See VpvGeneralPageCards.h. The row bodies are the ones Bill's ConfigEditorWindow::createGeneralPage built
// (moved, CC-GEM P2): keys, defaults, ranges, labels and tooltips unchanged; the editor's members they used
// are the EditorKit's (bindCheckField / bindChoiceField / bindTextField / value / setKnown / markDirty).

#include "VpvGeneralPageCards.h"
#include "VpvBothPathwayCards.h"
#include "VpvConfigFormApi.h"
#include <VP-V/core/AheadCeiling.h>

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleValidator>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <MainConfigSchema.h>
#include <ShortcutDefaults.h>
#include <VideoProcessor-Config/ConfigPageHost.h>
#include <string>
#include <utility>

namespace Vpv
{
	namespace
	{
		using ConfigPageHost::EditorKit;

		QFormLayout* Form(QWidget* body)
		{
			auto* form = new QFormLayout(body);
			form->setContentsMargins(0, 0, 0, 0);
			form->setVerticalSpacing(8);
			return form;
		}

		QLabel* Note(EditorKit& kit, const QString& text)
		{
			QLabel* label = kit.helpLabel(text);
			if (!label) label = new QLabel(text);
			label->setWordWrap(true);
			return label;
		}

		// Developer (Retirement Manager): the capture / display fix under test (was in General > General behavior).
		// Hosted under Developer > Retirement Manager (vpvOnlyTabs), so Original pathway greys it; its runtime is off there too
		// (CaptureDisplayFixes::Enabled, GEM-221 / MUST-8). Keeps vpv.input.card.capture_fixes objectName.
		UnitCard CaptureFixesCard(EditorKit& kit)
		{
			auto* body = new QWidget;
			auto* layout = new QVBoxLayout(body);
			layout->setContentsMargins(0, 0, 0, 0);
			layout->setSpacing(8);
			// Restore switch (post-merge audit 2026-10-17): [general] capture_display_fixes.
			if (QCheckBox* captureDisplayFixes = kit.bindCheckField(QStringLiteral("Capture / display fixes in Bill's code (fix under test)"),
				QStringLiteral("general"), QStringLiteral("capture_display_fixes"), true))
			{
				captureDisplayFixes->setToolTip(QStringLiteral(
					"On (default): the target-only full-screen session picks the monitor's largest mode first (then the "
					"highest refresh) and never an interlaced one; a DeckLink Rec.601 source is recognised by its active "
					"lines (486 / 480 NTSC, 576 PAL). Off: Bill's earlier behaviour - highest refresh first, interlaced "
					"allowed, 486 lines left without a colour space and 576 on the analog PAL entry."));
				layout->addWidget(captureDisplayFixes);
			}
			auto* note = Note(kit, QStringLiteral(
				"On the Original pathway this is off: Bill's own capture and display behaviour, from the start-up configuration "
				"on (the target-only full-screen session included). Read when VideoProcessor starts."));
			note->setObjectName(QStringLiteral("vpv.input.capture_fixes.note"));
			layout->addWidget(note);
			return { QStringLiteral("input"), QStringLiteral("capture_fixes"), QStringLiteral("Capture fixes (under test)"),
				QStringLiteral("VP's fix in Bill's capture and display code (was on Bill's General page)."), body };
		}

		// Both pathways: room lights, the profile rules' `ambient` variable (Stage 9; was General > Room lights).
		UnitCard RoomLightsCard(EditorKit& kit)
		{
			auto* ambient = new QWidget;
			QFormLayout* ambientForm = Form(ambient);
			if (QComboBox* ambientMode = kit.bindChoiceField(QStringLiteral("ambient"), QStringLiteral("mode"),
				{ QStringLiteral("off"), QStringLiteral("manual"), QStringLiteral("sensor") },
				{ QStringLiteral("Off"), QStringLiteral("Manual"), QStringLiteral("Light sensor") }))
			{
				ambientMode->setToolTip(QStringLiteral(
					"Off: rules see no room state. Manual: the room is as set below. Light sensor: a light sensor "
					"decides (an ESP32 / Arduino with a VEML7700 or TSL2591 on its own USB port; the bundled sketch is "
					"tools/light-sensor/vp_light_sensor.ino). Without readings the room below is used."));
				ambientForm->addRow(QStringLiteral("Room lights"), ambientMode);
			}
			if (QComboBox* ambientRoom = kit.bindChoiceField(QStringLiteral("ambient"), QStringLiteral("room"),
				{ QStringLiteral("dark"), QStringLiteral("lit") }, { QStringLiteral("Dark"), QStringLiteral("Lit") }))
			{
				ambientRoom->setToolTip(QStringLiteral(
					"The room in Manual mode, and the fallback when the light sensor is not reporting. The Room "
					"lights key flips it until restart."));
				ambientForm->addRow(QStringLiteral("Room"), ambientRoom);
			}
			struct AmbientNumber { const char* label; const char* key; double hi; const char* placeholder; const char* tip; };
			const AmbientNumber ambientNumbers[] = {
				{ "Lit above (lux)", "lux_lit", 100000.0, "20", "Light sensor: above this the room is lit." },
				{ "Dark below (lux)", "lux_dark", 100000.0, "8", "Light sensor: below this the room is dark; in between it stays as it was." },
				{ "Hold (seconds)", "hold_s", 600.0, "5", "Light sensor: a change must last this long (a door, someone passing)." } };
			for (const AmbientNumber& number : ambientNumbers)
			{
				QLineEdit* edit = kit.bindTextField(QStringLiteral("ambient"), QString::fromLatin1(number.key), QString());
				if (!edit) continue;
				edit->setValidator(new QDoubleValidator(0.0, number.hi, 3, edit));
				edit->setPlaceholderText(QString::fromLatin1(number.placeholder));
				edit->setToolTip(QString::fromLatin1(number.tip));
				ambientForm->addRow(QString::fromLatin1(number.label), edit);
			}
			if (QLineEdit* ambientPort = kit.bindTextField(QStringLiteral("ambient"), QStringLiteral("port"), QString()))
			{
				ambientPort->setPlaceholderText(QStringLiteral("COM5"));
				ambientPort->setToolTip(QStringLiteral(
					"Light sensor: its own USB serial port (Device Manager, Ports). Never the projector's RS-232 port: "
					"a port opens for one user at a time, so Apply refuses the same port."));
				ambientForm->addRow(QStringLiteral("Sensor port"), ambientPort);
			}
			if (QLineEdit* ambientBaud = kit.bindTextField(QStringLiteral("ambient"), QStringLiteral("baud"), QString()))
			{
				ambientBaud->setValidator(new QIntValidator(1200, 921600, ambientBaud));
				ambientBaud->setPlaceholderText(QStringLiteral("115200"));
				ambientBaud->setToolTip(QStringLiteral("Light sensor: the serial speed its sketch uses (the bundled sketch: 115200)."));
				ambientForm->addRow(QStringLiteral("Sensor speed (baud)"), ambientBaud);
			}
			auto* ambientNote = Note(kit, QStringLiteral(
				"Profiles choose by it: a rendering profile with <b>when: $ambient==\"lit\"</b> and a raised SDR "
				"black level renders to the lit room's black."));
			ambientNote->setTextFormat(Qt::RichText);
			ambientForm->addRow(ambientNote);
			// GEM-060 cross-link (Colour > Main > Room-light adaptation names this card back) and GEM-222.
			auto* linkNote = Note(kit, QStringLiteral(
				"The room-light picture adaptation (experimental), which follows the light sensor set here, is on the "
				"Colour tab: Main > Room-light adaptation. On Original, room lights only steer profile rules; the "
				"room-light picture adaptation is off."));
			linkNote->setObjectName(QStringLiteral("vpv.both.room_lights.note"));
			ambientForm->addRow(linkNote);
			return { QStringLiteral("both"), QStringLiteral("room_lights"), QStringLiteral("Room Light Sensor"),
				QStringLiteral("Dark or lit room, for profiles that render to the room's black."), ambient };
		}

		// Both pathways: [general] keys read only by the VP Renderer DirectShow filter's FileHost when MPC-HC /
		// MPC-BE play a local file through VP (2026-10-17; was General > File playback).
		UnitCard FilePlaybackCard(EditorKit& kit)
		{
			ConfigPageHost::EditorKit* k = &kit;
			auto* filePlayback = new QWidget;
			QFormLayout* fileForm = Form(filePlayback);
			auto fileMillisecondsField = [k](const QString& key, const QString& accessible, int minimum,
				int maximum, int fallback, const QString& special, const QString& tip) -> QWidget*
			{
				const QString general = QStringLiteral("general");
				auto* row = new QWidget;
				auto* rowLayout = new QHBoxLayout(row);
				rowLayout->setContentsMargins(0, 0, 0, 0);
				auto* spin = new QSpinBox;
				spin->setObjectName(k->controlName(general, key));
				spin->setAccessibleName(accessible);
				spin->setRange(minimum, maximum);
				spin->setSingleStep(10);
				spin->setSuffix(QStringLiteral(" ms"));
				spin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
				spin->setAlignment(Qt::AlignRight);
				spin->setMaximumWidth(128);
				if (!special.isEmpty())
					spin->setSpecialValueText(special);
				spin->setToolTip(tip + QStringLiteral(" %1 to %2 ms; default %3 ms.").arg(minimum).arg(maximum).arg(fallback));
				// Unset or out of range shows the default without writing it.
				bool valid = false;
				const int configured = k->value(general, key, QString()).trimmed().toInt(&valid);
				spin->setValue(valid && configured >= minimum && configured <= maximum ? configured : fallback);
				QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), spin, [k, spin, general, key](int milliseconds)
				{
					// setKnown adds the section when it is missing (as the General page's AddSection fallback did).
					k->setKnown(general, key, QString::number(milliseconds));
					k->markDirty(spin);
				});
				auto* reset = new QToolButton;
				reset->setObjectName(spin->objectName() + QStringLiteral(".reset"));
				reset->setIcon(reset->style()->standardIcon(QStyle::SP_BrowserReload));
				reset->setFixedSize(26, 26);
				reset->setToolTip(QStringLiteral("Reset %1 to %2 ms (default)").arg(accessible).arg(fallback));
				reset->setAccessibleName(reset->toolTip());
				QObject::connect(reset, &QToolButton::clicked, spin, [spin, fallback] { spin->setValue(fallback); });
				rowLayout->addWidget(spin);
				rowLayout->addWidget(reset);
				rowLayout->addStretch(1);
				return row;
			};
			fileForm->addRow(QStringLiteral("Picture offset (A/V)"), fileMillisecondsField(
				QStringLiteral("file_av_offset_ms"), QStringLiteral("File playback picture offset"),
				MainConfigSchema::FileAvOffsetMinMs, MainConfigSchema::FileAvOffsetMaxMs,
				MainConfigSchema::FileAvOffsetDefaultMs, QString(),
				QStringLiteral("Moves the picture against the sound for files played through the VP Renderer filter: "
					"positive shows the picture later, negative earlier. Capture is not affected.")));
			fileForm->addRow(QStringLiteral("Dynamic Drive hold between files"), fileMillisecondsField(
				QStringLiteral("file_led_holdover_ms"), QStringLiteral("Dynamic Drive hold between files"),
				MainConfigSchema::FileLedHoldoverMinMs, MainConfigSchema::FileLedHoldoverMaxMs,
				MainConfigSchema::FileLedHoldoverDefaultMs, QStringLiteral("Off"),
				QStringLiteral("The player rebuilds its graph for every file. The filter keeps Dynamic Drive running this "
					"long so the next file carries on from the current LED level instead of flashing the projector to "
					"full light; with no next file it then restores full light as usual. Off: full light at once.")));
			auto* fileNote = Note(kit, QStringLiteral(
				"Profiles can tell the two apart: <b>when: $source==\"file\"</b> matches file playback, "
				"<b>$source==\"capture\"</b> VideoProcessor.exe."));
			fileNote->setTextFormat(Qt::RichText);
			fileForm->addRow(fileNote);
			return { QStringLiteral("both"), QStringLiteral("file_playback"), QStringLiteral("File Playback"),
				QStringLiteral("Local files played in MPC-HC / MPC-BE through VP's processing. Read by the filter when it starts and when this file changes."),
				filePlayback };
		}
	}

	std::vector<UnitCard> WithGeneralPageCards(EditorKit& kit, const QString& unitId, std::vector<UnitCard> taken)
	{
		if (unitId == QStringLiteral("developer"))
			taken.push_back(CaptureFixesCard(kit));
		else if (unitId == QStringLiteral("both"))
		{
			taken.push_back(RoomLightsCard(kit));
			taken.push_back(FilePlaybackCard(kit));
		}
		return taken;
	}

	QLineEdit* BindUtf8TextField(EditorKit& kit, const QString& section, const QString& key,
		const QString& fallback, const QString& accessibleName)
	{
		auto* edit = new QLineEdit(kit.value(section, key, fallback));
		edit->setObjectName(kit.controlName(section, key));
		edit->setAccessibleName(accessibleName);
		EditorKit* k = &kit;
		QObject::connect(edit, &QLineEdit::textChanged, edit, [k, edit, section, key](const QString& text)
		{
			if (text.trimmed().isEmpty())
				k->removeKnown(section, key);
			else
				k->setKnown(section, key, text);
			k->markDirty(edit);
		});
		return edit;
	}

	void RefreshGlobalActionsPlaceholder(QWidget* root, bool vpvOn)
	{
		if (!root) return;
		if (auto* edit = root->findChild<QLineEdit*>(QStringLiteral("config.shortcuts.global_actions")))
		{
			edit->setPlaceholderText(vpvOn ?
				QStringLiteral("Default: state_report (for example: state_report, config_editor, toggle_noui)") :
				QStringLiteral("Default: none (Original pathway; for example: config_editor, toggle_noui)"));
		}
	}

	std::vector<QWidget*> BothPathwayCards(EditorKit& kit)
	{
		auto makeColumn = [](QWidget* body) -> QVBoxLayout*
		{
			auto* layout = new QVBoxLayout(body);
			layout->setContentsMargins(0, 0, 0, 0);
			layout->setSpacing(8);
			return layout;
		};
		auto addCard = [](std::vector<QWidget*>& cards, EditorKit& kitRef, const QString& id,
			const QString& title, const QString& description, QWidget* body)
		{
			QWidget* card = kitRef.createCard(title, description, body);
			if (!card) card = body;
			card->setObjectName(QStringLiteral("vpv.system.card.") + id);
			card->setProperty("vpvCardTitle", title);
			cards.push_back(card);
		};

		std::vector<QWidget*> cards;
		EditorKit* k = &kit;

		// ---- Capture & Signal Bridges ----
		{
			auto* body = new QWidget;
			QVBoxLayout* column = makeColumn(body);
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
			column->addWidget(Note(kit, QStringLiteral(
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
			auto* magewellNote = Note(kit, QStringLiteral(
				"Original pathway: forced off (Original matches Bill's Magewell capture behaviour). Read when VideoProcessor starts."));
			magewellNote->setObjectName(QStringLiteral("vpv.both.magewell_recovery.note"));
			column->addWidget(magewellNote);
			addCard(cards, kit, QStringLiteral("capture_bridges"), QStringLiteral("Capture & Signal Bridges"),
				QStringLiteral("Capture format, Dolby Vision hand-off, and capture recovery switches that act on both pathways."), body);
		}

		// ---- Global Shortcuts & Problem Reports ----
		{
			auto* body = new QWidget;
			QVBoxLayout* layout = makeColumn(body);
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
			layout->addWidget(Note(kit, QStringLiteral(
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
			addCard(cards, kit, QStringLiteral("shortcuts_reports"), QStringLiteral("Global Shortcuts & Problem Reports"),
				QStringLiteral("Global shortcut behaviour and saved state reports (Bill's own shortcut and logging settings stay on his pages)."), body);
		}
		return cards;
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
