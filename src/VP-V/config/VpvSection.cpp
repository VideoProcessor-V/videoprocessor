// First, before any Qt header: the core headers use "slots" as a name, which Qt defines as a macro.
#include <VP-V/core/SlotRegistry.h>

#include "VpvSection.h"

#include "VpvTierTable.generated.h"

#include <VP-V/core/DelayBudgetLadder.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <vector>

namespace Vpv
{
	namespace
	{
		const QString kDefaultProfile = QStringLiteral("vprenderer");

		QString Q(const char* text) { return text ? QString::fromUtf8(text) : QString(); }

		QLabel* Help(const QString& text)
		{
			auto* label = new QLabel(text);
			label->setWordWrap(true);
			label->setTextInteractionFlags(Qt::TextSelectableByMouse);
			return label;
		}

		QLabel* Bold(const QString& objectName)
		{
			auto* label = new QLabel;
			label->setObjectName(objectName);
			QFont font = label->font();
			font.setBold(true);
			label->setFont(font);
			return label;
		}

		// The upstream controls of a key: "config.<section>[.<group>...].<key>" (the Config naming;
		// a control's helpers such as ".reset" end differently and are not matched).
		QList<QWidget*> UpstreamControls(QWidget* root, const QString& section, const QString& key)
		{
			if (!root) return {};
			const QRegularExpression name(QStringLiteral("^config\\.%1(\\.[^.]+)*\\.%2$")
				.arg(QRegularExpression::escape(section), QRegularExpression::escape(key)));
			return root->findChildren<QWidget*>(name);
		}

		// The Config page a widget is on (createPage's viewport is named "<title> settings").
		QString PageOf(const QWidget* w)
		{
			for (const QWidget* p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget())
				if (p->objectName() == QStringLiteral("pageViewport") && qobject_cast<const QScrollArea*>(p))
				{
					QString page = p->accessibleName();
					if (page.endsWith(QStringLiteral(" settings"))) page.chop(9);
					return page;
				}
			return QString();
		}

		QString RetirementCaption(const Retirement& r)
		{
			return std::string(r.group) == "D" ? QStringLiteral("proposed: hide as diagnostic") : QStringLiteral("proposed retirement");
		}

		QString RetirementTip(const Table& table, const Retirement& r)
		{
			const QString caption = RetirementCaption(r);
			return QStringLiteral("%1 (group %2, %3): %4 Nothing changes until the owner approves; see the VP-V page's "
				"Proposed retirement tab and docs/settings/VP-settings-reference.md section 12.")
				.arg(caption.left(1).toUpper() + caption.mid(1), Q(r.group), Q(table.RetirementGroupTitle(r.group)), Q(r.reason));
		}

		// A unit tab's page: a themed card (VpTheme styles QFrame[card="true"]), so the tab's text is
		// on Config's dark surface instead of the platform style's white tab pane.
		QWidget* NewTabPage()
		{
			auto* frame = new QFrame;
			frame->setProperty("card", true);
			return frame;
		}

		// Shared state of one VP-V page.
		struct State
		{
			Host host;
			QString defaultProfile = kDefaultProfile;   // the first renderer profile (file order picks the default)
			QString profile = kDefaultProfile;
			std::vector<std::function<void()>> refreshers;
			bool refreshing = false;

			// The profile's value, else the default profile's (renderer profiles inherit).
			std::string Get(const std::string& key) const
			{
				const QString k = QString::fromStdString(key);
				QString v = host.value ? host.value(profile, k) : QString();
				if (v.trimmed().isEmpty() && profile.compare(defaultProfile, Qt::CaseInsensitive) != 0 && host.value)
					v = host.value(defaultProfile, k);
				return v.trimmed().toStdString();
			}
			std::string Own(const std::string& key) const
			{
				return host.value ? host.value(profile, QString::fromStdString(key)).trimmed().toStdString() : std::string();
			}
			Vpv::Getter MakeGetter() const { return [this](const std::string& key) { return Get(key); }; }
			void Write(const std::vector<std::pair<std::string, std::string>>& writes)
			{
				if (!host.setValue) return;
				for (const auto& w : writes)
					host.setValue(profile, QString::fromStdString(w.first), QString::fromStdString(w.second));
			}
			void RefreshAll()
			{
				if (refreshing) return;
				refreshing = true;
				for (const auto& r : refreshers) r();
				refreshing = false;
			}
		};

		QString TierCaption(const Preset& p)
		{
			QString text = Q(p.tier);
			if (p.cost && *p.cost) text += QStringLiteral(" (cost: %1)").arg(Q(p.cost));
			return text;
		}

		// One graded tier slider: positions = the unit's tiers (or the global tiers).
		struct TierBox
		{
			QWidget* box = nullptr;
			QSlider* slider = nullptr;
			QLabel* name = nullptr;
			QLabel* description = nullptr;
			QPushButton* reset = nullptr;
			QPushButton* defaults = nullptr;
		};

		TierBox MakeTierBox(const QString& prefix, const QString& accessible, const QStringList& positions,
			const QString& sliderTip)
		{
			TierBox t;
			t.box = new QWidget;
			t.box->setObjectName(ObjectName(prefix + QStringLiteral(".box")));
			auto* layout = new QVBoxLayout(t.box);
			layout->setContentsMargins(0, 0, 0, 0);
			auto* row = new QHBoxLayout;
			t.slider = new QSlider(Qt::Horizontal);
			t.slider->setObjectName(ObjectName(prefix));
			t.slider->setAccessibleName(accessible);
			t.slider->setRange(0, (std::max)(0, static_cast<int>(positions.size()) - 1));
			t.slider->setPageStep(1);
			t.slider->setTickPosition(QSlider::TicksBelow);
			t.slider->setTickInterval(1);
			t.slider->setToolTip(sliderTip);
			row->addWidget(new QLabel(positions.value(0)));
			row->addWidget(t.slider, 1);
			row->addWidget(new QLabel(positions.value(positions.size() - 1)));
			t.reset = new QPushButton(QStringLiteral("Reset to tier"));
			t.reset->setObjectName(ObjectName(prefix + QStringLiteral(".reset")));
			t.reset->setToolTip(QStringLiteral("Puts the tier's settings back to the slider position's values."));
			row->addWidget(t.reset);
			t.defaults = new QPushButton(QStringLiteral("Default"));
			t.defaults->setObjectName(ObjectName(prefix + QStringLiteral(".default")));
			row->addWidget(t.defaults);
			layout->addLayout(row);
			auto* steps = new QLabel(QStringLiteral("Steps: ") + positions.join(QStringLiteral(" / ")));
			steps->setObjectName(ObjectName(prefix + QStringLiteral(".steps")));
			layout->addWidget(steps);
			t.name = Bold(ObjectName(prefix + QStringLiteral(".name")));
			t.description = Help(QString());
			t.description->setObjectName(ObjectName(prefix + QStringLiteral(".description")));
			layout->addWidget(t.name);
			layout->addWidget(t.description);
			return t;
		}

		QString SlotLine(const Table& table, const char* id)
		{
			const SlotInfo* s = table.Slot(id);
			if (!s) return Q(id);
			const QString status = Q(s->status);
			QString line = QStringLiteral("<b>%1</b> (%2)").arg(Q(s->name).toHtmlEscaped(),
				status == QStringLiteral("placeholder") ? QStringLiteral("not yet implemented") : status);
			if (s->fallback)
				line += QStringLiteral(" &mdash; if it falls back: <i>%1</i>").arg(Q(s->fallback).toHtmlEscaped());
			return line;
		}

		QString DelaySummary(const Host& host, const Table& table)
		{
			const std::vector<double> rates = DelayBudget::StandardRates();
			const double recommended = DelayBudget::BudgetForFrames(table.recommendedFrames, rates);
			QString text = QStringLiteral(
				"<b>Total delay.</b> One value, the same at every frame rate, so your AVR's fixed audio delay always matches. "
				"<b>Recommended: %1 ms</b> (4 queued frames at 23.976p, the slowest rate). Shorter settings still work: the "
				"look-ahead features fall back to 3 or 2 frames.").arg(recommended, 0, 'f', 0);
			// The default queue profile's delay (the first [queue.*] profile; a literal [queue] in old files).
			QStringList queues = host.profileSections ? host.profileSections(Q(table.delaySection)) : QStringList();
			if (queues.isEmpty()) queues << Q(table.delaySection);
			const QString stored = host.value ? host.value(queues.front(), Q(table.delayKey)).trimmed() : QString();
			bool ok = false;
			const double budget = stored.toDouble(&ok);
			if (!ok || budget <= 0.0)
				return text + QStringLiteral("<br>Not set on the Queue page (it uses Target frames).");
			text += QStringLiteral("<br>Queue page Target delay: <b>%1 ms</b>:").arg(budget, 0, 'f', 0);
			text += QStringLiteral("<table cellspacing=6><tr><th align=left>Rate</th><th>Frames</th><th>Padding</th>"
				"<th align=left>Look-ahead</th></tr>");
			for (double fps : rates)
			{
				const DelayBudget::Plan p = DelayBudget::Compute(DelayBudget::InputsFor(budget, fps));
				text += QStringLiteral("<tr><td>%1p</td><td align=center>%2</td><td align=center>%3 ms</td><td>%4</td></tr>")
					.arg(fps, 0, 'f', 3).arg(p.queueFrames).arg(p.paddingMs, 0, 'f', 0)
					.arg(QString::fromLatin1(DelayBudget::LevelName(p)));
			}
			text += QStringLiteral("</table>Assumes a 60 Hz display, 2 swapchain frames, one frame of capture transfer and "
				"8 ms processing. Until the total-delay budget lands in the renderer, VP still treats this value as "
				"queue time.");
			return text;
		}

		QWidget* MakeUnitPage(const std::shared_ptr<State>& state, const Unit& unit)
		{
			const Table& table = TierTable();
			const QString unitId = Q(unit.id);
			QWidget* page = NewTabPage();
			page->setObjectName(ObjectName(unitId));
			auto* layout = new QVBoxLayout(page);
			layout->addWidget(Help(Q(unit.summary)));

			if (unit.Tiered())
			{
				QStringList positions;
				for (const Preset& p : unit.presets) positions << Q(p.tier);
				TierBox t = MakeTierBox(unitId + QStringLiteral(".tier"), Q(unit.name) + QStringLiteral(" tier"), positions,
					QStringLiteral("The %1 tier: each step sets the settings listed in its description. A changed setting "
						"shows Custom; Reset to tier puts it back.").arg(Q(unit.name)));
				layout->addWidget(t.box);
				QStringList notes;
				for (const PlannedTier& pt : unit.plannedTiers)
					notes << QStringLiteral("%1: not yet implemented (%2).").arg(Q(pt.tier), Q(pt.note));
				if (!notes.isEmpty())
				{
					auto* planned = Help(notes.join(QLatin1Char(' ')));
					planned->setObjectName(ObjectName(unitId + QStringLiteral(".tier.planned")));
					layout->addWidget(planned);
				}
				const Unit* u = &unit;
				// The tier the built-in defaults give (none when they match no tier).
				const int defaultTier = MatchTier(table, unit, [](const std::string&) { return std::string(); });
				t.defaults->setEnabled(defaultTier >= 0);
				t.defaults->setToolTip(defaultTier >= 0 ?
					QStringLiteral("Back to the built-in default tier, %1.").arg(Q(unit.presets[static_cast<size_t>(defaultTier)].tier)) :
					QStringLiteral("VP's built-in defaults match no tier of this unit."));
				auto apply = [state, u](int p)
				{
					state->Write(ApplyTier(TierTable(), *u, p, state->MakeGetter()));
					state->RefreshAll();
				};
				QObject::connect(t.slider, &QSlider::valueChanged, t.slider, [state, apply](int p)
				{
					if (!state->refreshing) apply(p);
				});
				QObject::connect(t.reset, &QPushButton::clicked, t.reset, [apply, t]() { apply(t.slider->value()); });
				QObject::connect(t.defaults, &QPushButton::clicked, t.defaults, [apply, defaultTier]() { apply(defaultTier); });
				state->refreshers.push_back([state, u, t]()
				{
					const int match = MatchTier(TierTable(), *u, state->MakeGetter());
					if (match >= 0)
					{
						const QSignalBlocker blocker(t.slider);
						t.slider->setValue(match);
						const Preset& p = u->presets[static_cast<size_t>(match)];
						t.name->setText(TierCaption(p));
						t.description->setText(Q(p.description));
					}
					else
					{
						t.name->setText(QStringLiteral("Custom"));
						t.description->setText(QStringLiteral("The settings differ from %1. Reset to tier puts them back.")
							.arg(Q(u->presets[static_cast<size_t>(t.slider->value())].tier)));
					}
				});
			}
			else
			{
				auto* reason = Help(QStringLiteral("Not tiered. ") + Q(unit.untieredReason));
				reason->setObjectName(ObjectName(unitId + QStringLiteral(".untiered")));
				layout->addWidget(reason);
			}

			if (unit.id == std::string("input"))
			{
				auto* delay = Help(QString());
				delay->setObjectName(ObjectName(QStringLiteral("input.delay")));
				delay->setTextFormat(Qt::RichText);
				layout->addWidget(delay);
				state->refreshers.push_back([state, delay]() { delay->setText(DelaySummary(state->host, TierTable())); });
			}

			if (!unit.toggles.empty() || !unit.planned.empty())
			{
				auto* heading = Bold(ObjectName(unitId + QStringLiteral(".settings")));
				heading->setText(QStringLiteral("Settings in this unit"));
				layout->addWidget(heading);
				auto* formHost = new QWidget;
				auto* form = new QFormLayout(formHost);
				form->setContentsMargins(0, 0, 0, 0);
				for (const Toggle& toggle : unit.toggles)
				{
					auto* combo = new QComboBox;
					const QString key = Q(toggle.key);
					combo->setObjectName(ObjectName(unitId + QLatin1Char('.') + key));
					const Retirement* retiring = table.FindRetirement(toggle.key, "vprenderer");
					const QString label = retiring ? QStringLiteral("%1 (%2)").arg(Q(toggle.label), RetirementCaption(*retiring)) : Q(toggle.label);
					combo->setAccessibleName(label);
					combo->addItem(QStringLiteral("Not set (default: %1)").arg(Q(table.Default(toggle.key))), QString());
					for (const char* v : toggle.values) combo->addItem(Q(v), Q(v));
					const SlotInfo* slot = table.Slot(toggle.slot);
					combo->setToolTip(QStringLiteral("Key %1 (renderer profile). Slot: %2. See docs/settings/VP-settings-reference.md.")
						.arg(key, slot ? Q(slot->name) : Q(toggle.slot)) +
						(retiring ? QStringLiteral("\n\n") + RetirementTip(table, *retiring) : QString()));
					form->addRow(label, combo);
					QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), combo, [state, combo, key](int index)
					{
						if (state->refreshing || index < 0 || !state->host.setValue) return;
						state->host.setValue(state->profile, key, combo->itemData(index).toString());
						state->RefreshAll();
					});
					const std::string k = toggle.key;
					state->refreshers.push_back([state, combo, k]()
					{
						const QString own = QString::fromStdString(state->Own(k));
						int index = 0;
						for (int i = 1; i < combo->count(); ++i)
							if (SameValue(combo->itemData(i).toString().toStdString(), own.toStdString())) index = i;
						const QSignalBlocker blocker(combo);
						combo->setCurrentIndex(own.isEmpty() ? 0 : index);
					});
				}
				for (const Planned& planned : unit.planned)
				{
					auto* box = new QCheckBox(Q(planned.label) + QStringLiteral(" - not yet implemented"));
					box->setObjectName(ObjectName(unitId + QStringLiteral(".planned.") + Q(planned.key)));
					box->setEnabled(false);
					box->setToolTip(QStringLiteral("Planned key %1 (slot %2): reserved in the pipeline; its component is not built yet.")
						.arg(Q(planned.key), Q(planned.slot)));
					form->addRow(QString(), box);
				}
				layout->addWidget(formHost);
			}

			if (unit.more && *unit.more)
			{
				auto* more = Help(QStringLiteral("<b>More settings of this unit</b> (on the other pages): ") + Q(unit.more).toHtmlEscaped());
				more->setTextFormat(Qt::RichText);
				more->setObjectName(ObjectName(unitId + QStringLiteral(".more")));
				layout->addWidget(more);
			}

			auto* slotsHeading = Bold(ObjectName(unitId + QStringLiteral(".slots_heading")));
			slotsHeading->setText(QStringLiteral("Stages in this unit (pipeline order)"));
			layout->addWidget(slotsHeading);
			QStringList lines;
			for (const char* id : unit.slotIds) lines << SlotLine(table, id);
			auto* slotLabel = Help(lines.join(QStringLiteral("<br>")));
			slotLabel->setTextFormat(Qt::RichText);
			slotLabel->setObjectName(ObjectName(unitId + QStringLiteral(".slots")));
			layout->addWidget(slotLabel);
			layout->addStretch();
			return page;
		}

		// The expert page: the skeleton's global pipeline keys (ADR-0019/0021, R1), stored in [vpv]
		// so Config's renderer-profile migrations never move them. The order is checked with the
		// renderer's own validator (SlotRegistry / OrderValidator) and its plain-language reason is
		// shown; an invalid order is never written, so the renderer never has to reject one.
		QWidget* MakeExpertPage(const std::shared_ptr<State>& state)
		{
			const QString section = Q(TierTable().section);
			QWidget* page = NewTabPage();
			page->setObjectName(ObjectName(QStringLiteral("expert")));
			auto* layout = new QVBoxLayout(page);
			layout->addWidget(Help(QStringLiteral("For testing and measurement. Normal viewing never needs these: the tiers "
				"and the Original pathway switch cover it.")));
			auto* form = new QFormLayout;

			// Where the projector calibration LUT runs (R1): libplacebo today, VP's last output hook later.
			auto* lutStage = new QComboBox;
			lutStage->setObjectName(ObjectName(QStringLiteral("expert.calibration_lut_stage")));
			lutStage->addItem(QStringLiteral("libplacebo (default; before VP's output steps)"), QString());
			lutStage->addItem(QStringLiteral("VP (the last colour step, after LED compensation)"), QStringLiteral("vp"));
			lutStage->setToolTip(QStringLiteral("Key calibration_lut_stage. VP applies the LUT after Dynamic Drive, so the LED "
				"measurement sees target light instead of projector codes (design R1)."));
			form->addRow(QStringLiteral("Calibration LUT runs in"), lutStage);
			QObject::connect(lutStage, qOverload<int>(&QComboBox::currentIndexChanged), lutStage, [state, lutStage, section](int i)
			{
				if (state->refreshing || i < 0 || !state->host.setValue) return;
				state->host.setValue(section, QStringLiteral("calibration_lut_stage"), lutStage->itemData(i).toString());
			});

			// The module order: slot names moved within their zone.
			auto* order = new QLineEdit;
			order->setObjectName(ObjectName(QStringLiteral("expert.pipeline_order")));
			order->setPlaceholderText(QStringLiteral("the designed order, e.g. repair.encoding, scaling.rebase"));
			order->setToolTip(QStringLiteral("Key pipeline_order: the slots to move, in their new order (comma separated). "
				"Each slot stays in its zone; the check below uses the renderer's own rules."));
			auto* orderStatus = Help(QString());
			orderStatus->setObjectName(ObjectName(QStringLiteral("expert.pipeline_order.status")));
			form->addRow(QStringLiteral("Module order"), order);
			form->addRow(QString(), orderStatus);
			QObject::connect(order, &QLineEdit::editingFinished, order, [state, order, orderStatus, section]()
			{
				if (!state->host.setValue) return;
				const std::string text = order->text().trimmed().toStdString();
				PipelineSlots::Registry check;
				std::string reason;
				if (!check.SetOrderByNames(text, &reason))
				{
					orderStatus->setText(QStringLiteral("Not saved: ") + QString::fromStdString(reason) + QLatin1Char('.'));
					return;
				}
				orderStatus->setText(text.empty() ? QStringLiteral("The designed order.") : QStringLiteral("Accepted."));
				state->host.setValue(section, QStringLiteral("pipeline_order"), QString::fromStdString(text));
			});

			// Slots switched off at slot level (zero cost: no pass, no hook).
			auto* slotsOff = new QLineEdit;
			slotsOff->setObjectName(ObjectName(QStringLiteral("expert.pipeline_slots_off")));
			slotsOff->setPlaceholderText(QStringLiteral("none, e.g. repair.texture_restore, colour.appearance"));
			slotsOff->setToolTip(QStringLiteral("Key pipeline_slots_off: slots switched off entirely (no pass, no hook), for "
				"A/B tests and fault isolation. Unknown names are not saved."));
			auto* slotsStatus = Help(QString());
			slotsStatus->setObjectName(ObjectName(QStringLiteral("expert.pipeline_slots_off.status")));
			form->addRow(QStringLiteral("Stages switched off"), slotsOff);
			form->addRow(QString(), slotsStatus);
			QObject::connect(slotsOff, &QLineEdit::editingFinished, slotsOff, [state, slotsOff, slotsStatus, section]()
			{
				if (!state->host.setValue) return;
				QStringList names;
				for (const QString& part : slotsOff->text().split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts))
				{
					if (PipelineSlots::FromName(part.toStdString().c_str()) == PipelineSlots::Id::Count)
					{
						slotsStatus->setText(QStringLiteral("Not saved: unknown stage '%1'.").arg(part));
						return;
					}
					names << part;
				}
				slotsStatus->setText(names.isEmpty() ? QStringLiteral("Every stage on.") : QStringLiteral("Accepted."));
				state->host.setValue(section, QStringLiteral("pipeline_slots_off"), names.join(QStringLiteral(", ")));
			});
			layout->addLayout(form);
			layout->addStretch();

			state->refreshers.push_back([state, lutStage, order, slotsOff, section]()
			{
				const auto get = [&](const char* key) { return state->host.value ? state->host.value(section, QString::fromLatin1(key)).trimmed() : QString(); };
				{
					const QSignalBlocker blocker(lutStage);
					lutStage->setCurrentIndex(get("calibration_lut_stage").compare(QStringLiteral("vp"), Qt::CaseInsensitive) == 0 ? 1 : 0);
				}
				if (!order->hasFocus()) order->setText(get("pipeline_order"));
				if (!slotsOff->hasFocus()) slotsOff->setText(get("pipeline_slots_off"));
			});
			return page;
		}
	}

	namespace
	{
		// The guide's section-12 candidates (coverage audit 2026-10-24): visible, never removed.
		// Each line says where its control is and, for renderer-profile and [general] keys, the value
		// in use; the same text is appended to the control's own tooltip (MarkRetirements).
		QWidget* MakeRetirementPage(const std::shared_ptr<State>& state)
		{
			const Table& table = TierTable();
			QWidget* page = NewTabPage();
			page->setObjectName(ObjectName(QStringLiteral("retirement")));
			auto* layout = new QVBoxLayout(page);
			auto* intro = Help(Q(table.retirementNote));
			intro->setObjectName(ObjectName(QStringLiteral("retirement.note")));
			layout->addWidget(intro);
			auto* list = Help(QString());
			list->setObjectName(ObjectName(QStringLiteral("retirement.list")));
			list->setTextFormat(Qt::RichText);
			layout->addWidget(list);
			layout->addStretch();
			state->refreshers.push_back([state, list]()
			{
				const Table& t = TierTable();
				QString html;
				for (const KeyValue& g : t.retirementGroups)
				{
					QString rows;
					for (const Retirement& r : t.retirements)
					{
						if (std::string(r.group) != g.key) continue;
						const QList<QWidget*> controls = UpstreamControls(state->host.upstreamRoot, Q(r.section), Q(r.key));
						const QString where = controls.isEmpty() ? QStringLiteral("no control found") :
							QStringLiteral("%1 page").arg(PageOf(controls.front()));
						QString value;
						if (std::string(r.section) == "vprenderer")
						{
							const std::string v = state->Get(r.key);
							value = v.empty() ? QStringLiteral("not set (default)") : QString::fromStdString(v);
						}
						else if (std::string(r.section) == "general" && state->host.value)
						{
							const QString v = state->host.value(QStringLiteral("general"), Q(r.key)).trimmed();
							value = v.isEmpty() ? QStringLiteral("not set (default)") : v;
						}
						rows += QStringLiteral("<tr><td><code>%1</code></td><td>%2</td><td>%3</td><td>%4</td></tr>")
							.arg(Q(r.key).toHtmlEscaped(), where.toHtmlEscaped(),
								value.isEmpty() ? QStringLiteral("-") : value.toHtmlEscaped(), Q(r.reason).toHtmlEscaped());
					}
					if (rows.isEmpty()) continue;
					html += QStringLiteral("<p><b>%1. %2</b> (%3)</p><table cellspacing=4><tr><th align=left>Key</th>"
						"<th align=left>Control</th><th align=left>This profile</th><th align=left>Why</th></tr>%4</table>")
						.arg(Q(g.key), Q(g.value).toHtmlEscaped(),
							std::string(g.key) == "D" ? QStringLiteral("proposed: hide as diagnostics") : QStringLiteral("proposed retirement"),
							rows);
				}
				list->setText(html);
			});
			return page;
		}
	}

	void MarkRetirements(const Host& host)
	{
		const Table& table = TierTable();
		for (const Retirement& r : table.retirements)
			for (QWidget* w : UpstreamControls(host.upstreamRoot, Q(r.section), Q(r.key)))
			{
				if (w->property("vpvRetirementMarked").toBool()) continue;
				w->setProperty("vpvRetirementMarked", true);
				w->setToolTip((w->toolTip().isEmpty() ? QString() : w->toolTip() + QStringLiteral("\n\n")) + RetirementTip(table, r));
			}
	}

	bool PathwayIsVpv(const Host& host)
	{
		const Table& table = TierTable();
		QString v = host.value ? host.value(Q(table.section), Q(table.pathwayKey)).trimmed() : QString();
		if (v.isEmpty()) v = Q(table.pathwayDefault);
		// pipeline_original_pathway on = the Original pathway (ADR-0019); off/absent = VP-V.
		return v.compare(QStringLiteral("on"), Qt::CaseInsensitive) != 0;
	}

	void ApplyManagedState(const Host& host, bool vpvOn)
	{
		if (!host.upstreamRoot || !host.upstreamControlName) return;
		for (const std::string& key : ManagedKeys(TierTable()))
		{
			const QString name = host.upstreamControlName(QString::fromStdString(key));
			if (name.isEmpty()) continue;
			for (QWidget* w : host.upstreamRoot->findChildren<QWidget*>(name))
			{
				if (w->objectName().startsWith(QStringLiteral("vpv."))) continue;
				const bool managed = w->property("vpvManaged").toBool();
				if (vpvOn && !managed)
				{
					w->setProperty("vpvWasEnabled", w->isEnabled());
					w->setProperty("vpvToolTip", w->toolTip());
					w->setProperty("vpvManaged", true);
					w->setEnabled(false);
					w->setToolTip(QStringLiteral("Managed by VP-V: change it on the VP-V page (or switch VP-V to the "
						"Original pathway).") + (w->toolTip().isEmpty() ? QString() : QStringLiteral("\n\n") + w->toolTip()));
				}
				else if (!vpvOn && managed)
				{
					w->setEnabled(w->property("vpvWasEnabled").toBool());
					w->setToolTip(w->property("vpvToolTip").toString());
					w->setProperty("vpvManaged", false);
				}
			}
		}
	}

	QWidget* CreateSection(const Host& host)
	{
		const Table& table = TierTable();
		auto state = std::make_shared<State>();
		state->host = host;

		auto* root = new QWidget;
		root->setObjectName(ObjectName(QStringLiteral("section")));
		auto* layout = new QVBoxLayout(root);
		layout->setContentsMargins(0, 0, 0, 0);

		// Pathway: VP-V (our units) or Original (upstream exactly as upstream).
		auto* pathwayRow = new QFormLayout;
		auto* pathway = new QComboBox;
		pathway->setObjectName(ObjectName(QStringLiteral("pathway")));
		pathway->setAccessibleName(QStringLiteral("Pathway"));
		pathway->addItem(QStringLiteral("VP-V pathway"), QStringLiteral("off"));
		pathway->addItem(QStringLiteral("Original pathway"), QStringLiteral("on"));
		pathway->setToolTip(QStringLiteral(
			"VP-V: the settings below own the units they list, and the matching controls on the other pages are greyed "
			"(\"Managed by VP-V\"). Original: this page is disabled and the other pages behave exactly as upstream. "
			"In the renderer, Original switches every VP addition off and runs the mixed stages in their upstream form "
			"(ADR-0019). Stored as [vpv] pipeline_original_pathway."));
		pathwayRow->addRow(QStringLiteral("Pathway"), pathway);
		layout->addLayout(pathwayRow);

		auto* content = new QWidget;
		content->setObjectName(ObjectName(QStringLiteral("content")));
		auto* contentLayout = new QVBoxLayout(content);
		contentLayout->setContentsMargins(0, 0, 0, 0);
		layout->addWidget(content);

		auto* profileRow = new QFormLayout;
		auto* profile = new QComboBox;
		profile->setObjectName(ObjectName(QStringLiteral("profile")));
		profile->setAccessibleName(QStringLiteral("Renderer profile"));
		profile->setToolTip(QStringLiteral("The renderer profile the tiers below edit. A named profile inherits every "
			"setting it does not set from the default profile; profile rules choose the profile (e.g. per frame rate)."));
		QStringList profiles = host.profileSections ? host.profileSections(kDefaultProfile) : QStringList();
		if (profiles.isEmpty()) profiles << kDefaultProfile;
		state->defaultProfile = state->profile = profiles.front();
		for (const QString& section : profiles)
		{
			const QString name = section.compare(kDefaultProfile, Qt::CaseInsensitive) == 0 ? QStringLiteral("(unnamed)") :
				section.mid(kDefaultProfile.size() + 1);
			profile->addItem(section == profiles.front() ? QStringLiteral("%1 (default profile)").arg(name) : name, section);
		}
		profileRow->addRow(QStringLiteral("Profile"), profile);
		contentLayout->addLayout(profileRow);

		// The global tier.
		QStringList globalTiers;
		for (const char* g : table.tiers) globalTiers << Q(g);
		TierBox global = MakeTierBox(QStringLiteral("global_tier"), QStringLiteral("Global tier"), globalTiers,
			QStringLiteral("Sets every unit's tier at once: Classic = no advanced features (straight to the projector), "
				"Low / Mid / Upper add measured features. A unit without that step uses its nearest lower one. Each unit "
				"can then be changed on its own tab (the global tier then shows Custom)."));
		global.reset->setText(QStringLiteral("Reset all units"));
		global.defaults->setVisible(false);
		contentLayout->addWidget(global.box);
		auto applyGlobal = [state](int g)
		{
			for (const Unit& u : TierTable().units)
				if (u.Tiered())
					state->Write(ApplyTier(TierTable(), u, GlobalToUnit(TierTable(), u, g), state->MakeGetter()));
			state->RefreshAll();
		};
		QObject::connect(global.slider, &QSlider::valueChanged, global.slider, [state, applyGlobal](int g)
		{
			if (!state->refreshing) applyGlobal(g);
		});
		QObject::connect(global.reset, &QPushButton::clicked, global.reset, [applyGlobal, global]() { applyGlobal(global.slider->value()); });
		state->refreshers.push_back([state, global]()
		{
			const int g = MatchGlobal(TierTable(), state->MakeGetter());
			if (g >= 0)
			{
				const QSignalBlocker blocker(global.slider);
				global.slider->setValue(g);
				global.name->setText(Q(TierTable().tiers[static_cast<size_t>(g)]));
				global.description->setText(QStringLiteral("Every unit is on its %1 step.").arg(Q(TierTable().tiers[static_cast<size_t>(g)])));
			}
			else
			{
				global.name->setText(QStringLiteral("Custom (per unit)"));
				global.description->setText(QStringLiteral("The units are on different tiers; each tab shows its own."));
			}
		});

		// "Fallbacks engaged": mirrors the running renderer's heading (the r6 registry's footer
		// label, config.live.fallbacks); no second registry here.
		auto* fallbacks = Help(QString());
		fallbacks->setObjectName(ObjectName(QStringLiteral("fallbacks")));
		fallbacks->setAccessibleName(QStringLiteral("Fallbacks engaged"));
		contentLayout->addWidget(fallbacks);
		auto mirror = [fallbacks]()
		{
			QWidget* top = fallbacks->window();
			const QLabel* live = top ? top->findChild<QLabel*>(QStringLiteral("config.live.fallbacks")) : nullptr;
			const QString text = live ? live->text().trimmed() : QString();
			fallbacks->setText(text.isEmpty() ? QStringLiteral("Fallbacks engaged: (no status from a running VP)") : text);
		};
		mirror();
		auto* timer = new QTimer(fallbacks);
		timer->setInterval(1000);
		QObject::connect(timer, &QTimer::timeout, fallbacks, mirror);
		timer->start();

		auto* tabs = new QTabWidget;
		tabs->setObjectName(ObjectName(QStringLiteral("units")));
		tabs->setStyleSheet(QStringLiteral("QTabWidget::pane { border: 0; background: transparent; }"));
		for (const Unit& u : table.units)
			tabs->addTab(MakeUnitPage(state, u), Q(u.name));
		tabs->addTab(MakeExpertPage(state), QStringLiteral("Pipeline (expert)"));
		tabs->addTab(MakeRetirementPage(state), QStringLiteral("Proposed retirement"));
		contentLayout->addWidget(tabs);

		QObject::connect(profile, qOverload<int>(&QComboBox::currentIndexChanged), profile, [state, profile](int index)
		{
			if (index < 0) return;
			state->profile = profile->itemData(index).toString();
			state->RefreshAll();
		});

		auto applyPathway = [state, content](bool vpvOn)
		{
			content->setEnabled(vpvOn);
			ApplyManagedState(state->host, vpvOn);
		};
		{
			const QSignalBlocker blocker(pathway);
			pathway->setCurrentIndex(PathwayIsVpv(host) ? 0 : 1);
		}
		QObject::connect(pathway, qOverload<int>(&QComboBox::currentIndexChanged), pathway, [state, pathway, applyPathway](int index)
		{
			if (index < 0 || !state->host.setValue) return;
			const Table& t = TierTable();
			const QString value = pathway->itemData(index).toString();
			// The default (VP-V) is written as an absent key, so untouched configs stay unchanged.
			state->host.setValue(Q(t.section), Q(t.pathwayKey),
				value == Q(t.pathwayDefault) ? QString() : value);
			applyPathway(value != QStringLiteral("on"));
		});
		// Before the managed greying, so the Original pathway restores tooltips that keep the mark.
		MarkRetirements(host);
		state->RefreshAll();
		applyPathway(PathwayIsVpv(host));
		return root;
	}
}
