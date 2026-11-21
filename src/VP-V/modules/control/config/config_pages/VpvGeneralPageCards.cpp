// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// See VpvGeneralPageCards.h. The row bodies are the ones Bill's ConfigEditorWindow::createGeneralPage built
// (moved, CC-GEM P2): keys, defaults, ranges, labels and tooltips unchanged; the editor's members they used
// are the EditorKit's (bindCheckField / bindChoiceField / bindTextField / value / setKnown / markDirty).

#include "VpvGeneralPageCards.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <MainConfigSchema.h>
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

		// Input: the two capture fixes under test (were in General > General behavior).
		UnitCard CaptureFixesCard(EditorKit& kit)
		{
			auto* body = new QWidget;
			auto* layout = new QVBoxLayout(body);
			layout->setContentsMargins(0, 0, 0, 0);
			layout->setSpacing(8);
			// Restore switch 2026-10-17: [general] magewell_recover_faults.
			if (QCheckBox* magewellRecover = kit.bindCheckField(QStringLiteral("Magewell: recover from capture faults (fix under test)"),
				QStringLiteral("general"), QStringLiteral("magewell_recover_faults"), true))
			{
				magewellRecover->setToolTip(QStringLiteral(
					"On (default): a Magewell fault the card recovers from (no frames for 2 s, a failed start, the capture "
					"memory budget, a slow frame copy) holds the video invalid and restarts the device by itself. Off: the "
					"earlier behaviour - capture stops and needs a manual Restart."));
				layout->addWidget(magewellRecover);
			}
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
			layout->addWidget(Note(kit, QStringLiteral(
				"On the Original pathway both are off: Bill's own capture and display behaviour. Read when VideoProcessor starts.")));
			return { QStringLiteral("input"), QStringLiteral("capture_fixes"), QStringLiteral("Capture fixes (under test)"),
				QStringLiteral("VP's fixes in Bill's capture and display code (were on Bill's General page)."), body };
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
			return { QStringLiteral("both"), QStringLiteral("room_lights"), QStringLiteral("Room lights"),
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
			return { QStringLiteral("both"), QStringLiteral("file_playback"), QStringLiteral("File playback (VP Renderer filter)"),
				QStringLiteral("Local files played in MPC-HC / MPC-BE through VP's processing. Read by the filter when it starts and when this file changes."),
				filePlayback };
		}
	}

	std::vector<UnitCard> WithGeneralPageCards(EditorKit& kit, const QString& unitId, std::vector<UnitCard> taken)
	{
		if (unitId == QStringLiteral("input"))
			taken.push_back(CaptureFixesCard(kit));
		else if (unitId == QStringLiteral("both"))
		{
			taken.push_back(RoomLightsCard(kit));
			taken.push_back(FilePlaybackCard(kit));
		}
		return taken;
	}
}
