// First, before any Qt header: the core headers use "slots" as a name, which Qt defines as a macro.
#include <VP-V/core/SlotRegistry.h>

#include "VpvSection.h"

#include "VpvOrganisation.h"
#include "VpvTierTable.generated.h"

#include <VP-V/core/DelayBudgetLadder.h>

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <set>
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

		// The Config page a widget is on (createPage's viewport is named "<title> settings"); a
		// control moved onto the VP-V page (organisation pass) says which unit tab and card.
		QString PageOf(const QWidget* w)
		{
			QString card, tab;
			for (const QWidget* p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget())
			{
				if (card.isEmpty() && !p->property("vpvCardTitle").toString().isEmpty())
					card = p->property("vpvCardTitle").toString();
				if (tab.isEmpty() && !p->property("vpvTabTitle").toString().isEmpty())
					tab = p->property("vpvTabTitle").toString();
				if (p->objectName() == ObjectName(QStringLiteral("section")))
					return QStringLiteral("VP-V > ") + tab + (card.isEmpty() ? QString() : QStringLiteral(" > ") + card);
				if (p->objectName() == QStringLiteral("pageViewport") && qobject_cast<const QScrollArea*>(p))
				{
					QString page = p->accessibleName();
					if (page.endsWith(QStringLiteral(" settings"))) page.chop(9);
					return page;
				}
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
		QWidget* NewTabPage(const QString& tabTitle)
		{
			auto* frame = new QFrame;
			frame->setProperty("card", true);
			frame->setProperty("vpvTabTitle", tabTitle);
			return frame;
		}

		// ---- Organisation pass (VpvOrganisation.h): cards, and moving upstream rows/sections ----

		// One labelled card inside a unit tab. A collapsible card (the "Advanced" ones) opens from its
		// header, styled like the Rendering page's collapsible sections.
		struct CardBox
		{
			QFrame* frame = nullptr;
			QWidget* body = nullptr;
			QVBoxLayout* layout = nullptr;
			QFormLayout* form = nullptr;   // the current form for toggles and rows (a section or note ends it)
			QToolButton* toggle = nullptr;

			QFormLayout* Form()
			{
				if (!form)
				{
					auto* host = new QWidget;
					form = new QFormLayout(host);
					form->setContentsMargins(0, 0, 0, 0);
					form->setHorizontalSpacing(18);
					form->setVerticalSpacing(8);
					layout->addWidget(host);
				}
				return form;
			}
			void Add(QWidget* w) { form = nullptr; layout->addWidget(w); }
			bool hasHelp = false;
			bool Empty() const { return layout->count() <= (hasHelp ? 1 : 0); }   // only the help line (or nothing)
		};

		CardBox MakeCard(const QString& objectName, const QString& title, const QString& help, bool collapsible)
		{
			CardBox c;
			c.frame = new QFrame;
			c.frame->setObjectName(objectName);
			c.frame->setProperty("vpvCard", true);
			c.frame->setStyleSheet(QStringLiteral(
				"QFrame[vpvCard=\"true\"] { background: #0c1620; border: 1px solid #22384c; border-radius: 6px; }"));
			auto* outer = new QVBoxLayout(c.frame);
			outer->setContentsMargins(10, 8, 10, 10);
			outer->setSpacing(6);
			if (collapsible)
			{
				c.toggle = new QToolButton;
				c.toggle->setObjectName(objectName + QStringLiteral(".toggle"));
				c.toggle->setText(title);
				c.toggle->setProperty("profileSection", true);
				c.toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
				c.toggle->setArrowType(Qt::RightArrow);
				c.toggle->setCheckable(true);
				c.toggle->setChecked(false);
				c.toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
				c.toggle->setAccessibleDescription(QStringLiteral("Expand or collapse %1.").arg(title));
				c.toggle->setToolTip(help);
				outer->addWidget(c.toggle);
			}
			else
			{
				auto* heading = new QLabel(title);
				heading->setObjectName(objectName + QStringLiteral(".title"));
				heading->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 650; color: #f0f6fb;"));
				heading->setToolTip(help);
				outer->addWidget(heading);
			}
			c.body = new QWidget(c.frame);
			c.body->setObjectName(objectName + QStringLiteral(".body"));
			c.body->setProperty("vpvCardTitle", title);
			c.layout = new QVBoxLayout(c.body);
			c.layout->setContentsMargins(collapsible ? 8 : 0, 0, 0, 0);
			c.layout->setSpacing(6);
			if (!help.isEmpty())
			{
				auto* line = Help(help);
				line->setProperty("help", true);
				c.layout->addWidget(line);
				c.hasHelp = true;
			}
			outer->addWidget(c.body);
			if (c.toggle)
			{
				c.body->setVisible(false);
				QToolButton* toggle = c.toggle;
				QWidget* body = c.body;
				QObject::connect(toggle, &QToolButton::toggled, toggle, [toggle, body](bool open)
				{
					body->setVisible(open);
					toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
				});
			}
			return c;
		}

		// A Rendering-page section ("rendererSection.<id>": its toggle; the section is its parent).
		QWidget* UpstreamSection(QWidget* root, const QString& id)
		{
			if (!root) return nullptr;
			auto* toggle = root->findChild<QToolButton*>(QStringLiteral("rendererSection.") + id);
			return toggle ? toggle->parentWidget() : nullptr;
		}

		// The form rows of a renderer key: every control named config.vprenderer.<key>[.<part>] is
		// walked up to the widget that is a field of a QFormLayout. Sorted in form order.
		QList<QWidget*> RowFields(QWidget* root, const QString& key)
		{
			QList<QWidget*> fields;
			if (!root) return fields;
			const QRegularExpression name(QStringLiteral("^config\\.vprenderer\\.%1(\\.[^.]+)?$").arg(QRegularExpression::escape(key)));
			for (QWidget* w : root->findChildren<QWidget*>(name))
				for (QWidget* f = w; f && f->parentWidget() && f != root; f = f->parentWidget())
					if (auto* form = qobject_cast<QFormLayout*>(f->parentWidget()->layout()))
					{
						int row = -1;
						QFormLayout::ItemRole role = QFormLayout::LabelRole;
						form->getWidgetPosition(f, &row, &role);
						if (row < 0) continue;
						if (role != QFormLayout::LabelRole && !fields.contains(f)) fields << f;
						break;
					}
			std::stable_sort(fields.begin(), fields.end(), [](QWidget* a, QWidget* b)
			{
				auto* fa = qobject_cast<QFormLayout*>(a->parentWidget()->layout());
				auto* fb = qobject_cast<QFormLayout*>(b->parentWidget()->layout());
				if (fa != fb) return false;
				int ra = -1, rb = -1;
				QFormLayout::ItemRole role;
				fa->getWidgetPosition(a, &ra, &role);
				fb->getWidgetPosition(b, &rb, &role);
				return ra < rb;
			});
			return fields;
		}

		// Moves one form row (label and field) to another form, keeping its visibility; the controls,
		// names and behaviour are unchanged (as Config's own moveRow, ConfigEditorWindow.cpp).
		// Stacked (VP-V cards): the label gets its own row above the field, so a long label never sits
		// beside a wide field (the page must not scroll sideways); the label is remembered on the field.
		bool MoveRow(QWidget* field, QFormLayout* to, bool stacked = true)
		{
			if (!field || !to || !field->parentWidget()) return false;
			auto* from = qobject_cast<QFormLayout*>(field->parentWidget()->layout());
			if (!from || from == to) return false;
			int row = -1;
			QFormLayout::ItemRole role;
			from->getWidgetPosition(field, &row, &role);
			if (row < 0) return false;
			const bool visible = from->isRowVisible(row);
			const QFormLayout::TakeRowResult taken = from->takeRow(row);
			QWidget* label = taken.labelItem ? taken.labelItem->widget() : nullptr;
			delete taken.labelItem;
			delete taken.fieldItem;
			if (label && stacked)
			{
				to->addRow(label);
				to->setRowVisible(label, visible);
				field->setProperty("vpvRowLabel", QVariant::fromValue(static_cast<QObject*>(label)));
				to->addRow(field);
			}
			else if (label) to->addRow(label, field);
			else to->addRow(field);
			to->setRowVisible(field, visible);
			return true;
		}

		// Moves a whole upstream section into a card. A card that is the section's only home (not an
		// Advanced card) shows it open without its own collapse button.
		bool MoveSection(QWidget* root, const QString& id, CardBox& card, bool flatten)
		{
			QWidget* section = UpstreamSection(root, id);
			if (!section) return false;
			if (QLayout* old = section->parentWidget() ? section->parentWidget()->layout() : nullptr) old->removeWidget(section);
			card.Add(section);
			if (flatten)
				if (auto* toggle = section->findChild<QToolButton*>(QStringLiteral("rendererSection.") + id, Qt::FindDirectChildrenOnly))
				{
					toggle->setChecked(true);
					toggle->setVisible(false);
				}
			return true;
		}

		// Shared state of one VP-V page.
		struct State
		{
			Host host;
			QString defaultProfile = kDefaultProfile;   // the first renderer profile (file order picks the default)
			QString profile = kDefaultProfile;
			std::vector<std::function<void()>> refreshers;
			bool refreshing = false;
			QWidget* root = nullptr;
			QPointer<QListWidget> upstreamList;          // the Rendering page's profile list (the picker follows it)
			std::vector<QPointer<QWidget>> own;          // VP-V's own controls: disabled in the Original pathway
			QStringList missing;                         // organisation items the editor does not have (tests)
			QStringList hidden;                          // upstream rows hidden while the VP-V pathway is on (Hide items)
			bool vpvOn = true;                           // the pathway (locks to a tier apply only on VP-V)

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
			// The upstream Rendering rows (some now on this page) show the document's values again after
			// VP-V wrote keys: Config reloads a profile's fields when its list's current item changes.
			void ReloadUpstream()
			{
				if (!upstreamList) return;
				if (QListWidgetItem* item = upstreamList->currentItem())
					emit upstreamList->currentItemChanged(item, item);
			}
			void Write(const std::vector<std::pair<std::string, std::string>>& writes)
			{
				if (!host.setValue) return;
				for (const auto& w : writes)
					host.setValue(profile, QString::fromStdString(w.first), QString::fromStdString(w.second));
				if (!writes.empty()) ReloadUpstream();
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
				"<b>Recommended: %1 ms</b> (4 queued frames at 23.976p, the slowest rate, on a %2 Hz display; VP plans with "
				"the display refresh it measures, so a faster display needs less). Shorter settings still work: the "
				"look-ahead features fall back to 3 or 2 frames.").arg(recommended, 0, 'f', 0).arg(DelayBudget::kReferenceRefreshHz, 0, 'f', 0);
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

		// A VP-V toggle: a choice with "Not set (default: ...)" first; it writes the key in the profile.
		void AddToggleRow(const std::shared_ptr<State>& state, const QString& unitId, const Toggle& toggle, QFormLayout* form)
		{
			const Table& table = TierTable();
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
			state->own.push_back(combo);
			QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), combo, [state, combo, key](int index)
			{
				if (state->refreshing || index < 0 || !state->host.setValue) return;
				state->host.setValue(state->profile, key, combo->itemData(index).toString());
				state->ReloadUpstream();
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

		const Toggle* FindToggle(const Unit& unit, const char* key)
		{
			for (const Toggle& t : unit.toggles)
				if (std::string(t.key) == key) return &t;
			return nullptr;
		}

		// ---- Masters and the settings they set (organisation pass 2) ----

		// A card's lock: the derived controls are greyed with "Set by <master>" (or "Used only when
		// <master> is on") until the card's Custom box is ticked. The lock disables a host widget (the
		// card body, or each listed key's controls), so upstream's own greying underneath is kept.
		struct LockState
		{
			Lock spec;
			QString unitId;
			QPointer<QWidget> body;
			QPointer<QCheckBox> custom;      // Tier / Slider / Defaults
			QPointer<QLabel> note;
			QStringList keys;                // the card's row keys (Defaults: what Custom clears)
		};

		bool EffectiveOff(const std::shared_ptr<State>& state, const Lock& lock)
		{
			std::string v = state->Get(lock.masterKey ? lock.masterKey : "");
			if (v.empty()) v = lock.fallback ? lock.fallback : "";
			if (v.empty()) v = TierTable().Default(lock.masterKey ? lock.masterKey : "");
			for (const QString& off : Q(lock.offValues).split(QLatin1Char(','), Qt::SkipEmptyParts))
				if (SameValue(v, off.trimmed().toStdString())) return true;
			return false;
		}

		QList<QWidget*> KeyControls(QWidget* root, const QString& key)
		{
			QList<QWidget*> found;
			if (!root) return found;
			const QRegularExpression name(QStringLiteral("^config\\.vprenderer\\.%1(\\.[^.]+)?$").arg(QRegularExpression::escape(key)));
			for (QWidget* w : root->findChildren<QWidget*>(name)) found << w;
			// The row's label goes with it.
			for (QWidget* f : RowFields(root, key))
				if (auto* form = qobject_cast<QFormLayout*>(f->parentWidget()->layout()))
					if (QWidget* label = form->labelForField(f)) found << label;
					else if (auto* stacked = qobject_cast<QWidget*>(f->property("vpvRowLabel").value<QObject*>())) found << stacked;
			return found;
		}

		void ApplyLock(const std::shared_ptr<State>& state, const std::shared_ptr<LockState>& lock)
		{
			const Lock& spec = lock->spec;
			bool locked = false;
			QString why;
			switch (spec.kind)
			{
			case LockKind::None: return;
			case LockKind::Tier:
			case LockKind::Slider:
			case LockKind::Defaults:
				// A tier exists only on the VP-V pathway; in the Original pathway its keys are plain controls.
				locked = lock->custom && !lock->custom->isChecked() && (spec.kind != LockKind::Tier || state->vpvOn);
				why = QStringLiteral("Set by %1. Tick Custom to change these by hand.").arg(Q(spec.master));
				break;
			case LockKind::Feature:
				locked = EffectiveOff(state, spec);
				why = QStringLiteral("Used only when %1 is on (Main).").arg(Q(spec.master));
				break;
			}
			if (lock->note) lock->note->setText(locked ? why : (spec.kind == LockKind::Feature ? QString() :
				QStringLiteral("Custom: these are yours; unticking Custom puts back the values %1 sets.").arg(Q(spec.master))));
			if (lock->note) lock->note->setVisible(!lock->note->text().isEmpty());
			if (spec.lockKeys.empty())
			{
				// The body's content (everything after the help, the note and the Custom box).
				if (lock->body)
					for (QWidget* child : lock->body->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
						if (!child->property("vpvLockControl").toBool())
						{
							child->setEnabled(!locked);
							child->setToolTip(locked ? why : QString());
						}
			}
			else
				for (const char* key : spec.lockKeys)
					for (QWidget* w : KeyControls(lock->body, Q(key)))
					{
						if (locked && !w->property("vpvLocked").toBool())
						{
							w->setProperty("vpvLocked", true);
							w->setProperty("vpvLockTip", w->toolTip());
							w->setProperty("derivedSetting", true);   // Config's profile reload keeps it greyed
							w->setEnabled(false);
							w->setToolTip(why);
						}
						else if (!locked && w->property("vpvLocked").toBool())
						{
							w->setProperty("vpvLocked", false);
							w->setProperty("derivedSetting", false);
							w->setEnabled(true);
							w->setToolTip(w->property("vpvLockTip").toString());
						}
					}
		}

		// Is the master reading Custom (its values differ from what it sets)?
		bool MasterIsCustom(const std::shared_ptr<State>& state, const std::shared_ptr<LockState>& lock, const Unit* unit)
		{
			switch (lock->spec.kind)
			{
			case LockKind::Tier:
				return unit && unit->Tiered() && MatchTier(TierTable(), *unit, state->MakeGetter()) < 0;
			case LockKind::Slider:
			{
				auto* label = state->root ? state->root->findChild<QLabel*>(QStringLiteral("config.vprenderer.") + Q(lock->spec.masterKey)) : nullptr;
				return label && label->text().startsWith(QStringLiteral("Custom"));
			}
			case LockKind::Defaults:
				for (const QString& key : lock->keys)
					if (!state->Own(key.toStdString()).empty()) return true;
				return false;
			default:
				return false;
			}
		}

		// Unticking Custom puts back the master's values.
		void BackToMaster(const std::shared_ptr<State>& state, const std::shared_ptr<LockState>& lock, const Unit* unit)
		{
			switch (lock->spec.kind)
			{
			case LockKind::Tier:
				if (unit && unit->Tiered())
				{
					int tier = 0;
					if (auto* slider = state->root->findChild<QSlider*>(ObjectName(lock->unitId + QStringLiteral(".tier")))) tier = slider->value();
					state->Write(ApplyTier(TierTable(), *unit, tier, state->MakeGetter()));
				}
				break;
			case LockKind::Slider:
				if (auto* reset = state->root->findChild<QPushButton*>(QStringLiteral("config.vprenderer.") + Q(lock->spec.resetKey)))
					reset->click();
				break;
			case LockKind::Defaults:
			{
				std::vector<std::pair<std::string, std::string>> writes;
				for (const QString& key : lock->keys) writes.emplace_back(key.toStdString(), std::string());
				state->Write(writes);
				break;
			}
			default: break;
			}
		}

		// Fills one card from the organisation table: toggles of `unit` (null on the retirement tab),
		// upstream rows and sections moved here, rows hidden, notes; then its lock.
		void FillCard(const std::shared_ptr<State>& state, const Unit* unit, const QString& unitId, const Card& spec,
			CardBox& card, std::set<std::string>& placedToggles)
		{
			const Table& table = TierTable();
			auto lock = std::make_shared<LockState>();
			lock->spec = spec.lock;
			lock->unitId = unitId;
			lock->body = card.body;
			if (spec.lock.kind != LockKind::None)
			{
				lock->note = Help(QString());
				lock->note->setProperty("help", true);
				lock->note->setProperty("vpvLockControl", true);
				lock->note->setObjectName(card.frame->objectName() + QStringLiteral(".lock"));
				card.Add(lock->note);
				if (spec.lock.kind != LockKind::Feature)
				{
					lock->custom = new QCheckBox(QStringLiteral("Custom (change these by hand)"));
					lock->custom->setObjectName(card.frame->objectName() + QStringLiteral(".custom"));
					lock->custom->setProperty("vpvLockControl", true);
					lock->custom->setToolTip(QStringLiteral("Off: the values %1 sets (greyed). On: edit them; the master then "
						"reads Custom. Unticking puts the master's values back.").arg(Q(spec.lock.master)));
					card.Add(lock->custom);
				}
			}
			int sections = 0;
			for (const CardItem& item : spec.items) sections += item.kind == ItemKind::Section ? 1 : 0;
			for (const CardItem& item : spec.items)
			{
				const QString id = Q(item.id);
				switch (item.kind)
				{
				case ItemKind::Toggle:
				{
					const Toggle* t = unit ? FindToggle(*unit, item.id) : nullptr;
					if (!t) { state->missing << QStringLiteral("toggle %1 (%2)").arg(id, unitId); break; }
					placedToggles.insert(t->key);
					// Owner: retirement candidates only on the Proposed retirement tab.
					if (table.FindRetirement(t->key, "vprenderer")) break;
					AddToggleRow(state, unitId, *t, card.Form());
					break;
				}
				case ItemKind::Row:
				{
					// On the Rendering page, or in a section an earlier card already moved here.
					QList<QWidget*> fields = RowFields(state->host.upstreamRoot, id);
					if (fields.isEmpty()) fields = RowFields(state->root, id);
					bool moved = false;
					for (QWidget* f : fields) moved = MoveRow(f, card.Form()) || moved;
					if (!moved) state->missing << QStringLiteral("row %1").arg(id);
					lock->keys << id;
					break;
				}
				case ItemKind::Section:
					if (!MoveSection(state->host.upstreamRoot, id, card, !spec.collapsed && sections == 1))
						state->missing << QStringLiteral("section %1").arg(id);
					break;
				case ItemKind::Hide:
					state->hidden << id;
					break;
				case ItemKind::Note:
				{
					auto* note = Help(id);
					note->setProperty("help", true);
					card.Add(note);
					break;
				}
				}
			}
			// Tier keys: their rows here are the one control of the key (no managed greying or hiding).
			if (spec.lock.kind == LockKind::Tier) card.body->setProperty("vpvKeyHome", true);
			if (spec.lock.kind == LockKind::None) return;
			if (lock->custom)
				QObject::connect(lock->custom, &QCheckBox::toggled, lock->custom, [state, lock, unit](bool on)
				{
					if (state->refreshing) return;
					if (!on) BackToMaster(state, lock, unit);
					ApplyLock(state, lock);
					state->RefreshAll();
				});
			// A feature's tuning follows its switch as soon as it changes (also through its upstream control).
			if (spec.lock.kind == LockKind::Feature)
				for (QWidget* root : { state->host.upstreamRoot, state->root, static_cast<QWidget*>(card.frame) })   // the card is not placed yet
					if (auto* master = root ? root->findChild<QComboBox*>(QStringLiteral("config.vprenderer.") + Q(spec.lock.masterKey)) : nullptr)
						QObject::connect(master, qOverload<int>(&QComboBox::currentIndexChanged), card.body, [state, lock](int)
						{
							ApplyLock(state, lock);
						});
			state->refreshers.push_back([state, lock, unit]()
			{
				// Custom follows the values: ticked by itself when they differ from the master's.
				if (lock->custom && MasterIsCustom(state, lock, unit) && !lock->custom->isChecked())
				{
					const QSignalBlocker blocker(lock->custom);
					lock->custom->setChecked(true);
				}
				ApplyLock(state, lock);
			});
		}

		QWidget* NewSubPage(const QString& objectName, const QString& title, QVBoxLayout** layout)
		{
			auto* page = new QWidget;
			page->setObjectName(objectName);
			page->setProperty("vpvTabTitle", title);
			*layout = new QVBoxLayout(page);
			(*layout)->setContentsMargins(4, 8, 4, 4);
			(*layout)->setSpacing(10);
			return page;
		}

		QWidget* MakeUnitPage(const std::shared_ptr<State>& state, const Unit& unit)
		{
			const Table& table = TierTable();
			const QString unitId = Q(unit.id);
			QWidget* page = NewTabPage(Q(unit.name));
			page->setObjectName(ObjectName(unitId));
			auto* outer = new QVBoxLayout(page);
			outer->setSpacing(8);
			auto* summary = Help(Q(unit.summary));
			outer->addWidget(summary);
			auto* sub = new QTabWidget;
			sub->setObjectName(ObjectName(unitId + QStringLiteral(".pages")));
			sub->setStyleSheet(QStringLiteral("QTabWidget::pane { border: 0; background: transparent; }"));
			sub->setDocumentMode(true);
			outer->addWidget(sub);
			QVBoxLayout* layout = nullptr;
			QVBoxLayout* advanced = nullptr;
			QWidget* mainPage = NewSubPage(ObjectName(unitId + QStringLiteral(".main")), Q(unit.name), &layout);
			QWidget* advancedPage = NewSubPage(ObjectName(unitId + QStringLiteral(".advanced")), Q(unit.name), &advanced);
			sub->addTab(mainPage, QStringLiteral("Main"));
			sub->addTab(advancedPage, QStringLiteral("Advanced"));
			sub->setTabToolTip(0, QStringLiteral("The %1 tier and the switches you might use; start here.").arg(Q(unit.name)));
			sub->setTabToolTip(1, QStringLiteral("What the controls on Main set (greyed until you tick Custom), tuning used only "
				"while its feature is on, and measured internals. Normal viewing never needs it."));
			auto* advancedIntro = Help(QStringLiteral("Settings set by the controls on Main (greyed, \"Set by ...\", until you tick "
				"Custom), tuning used only while its feature is on, and the measured internals."));
			advancedIntro->setProperty("help", true);
			advanced->addWidget(advancedIntro);

			// 1. The unit's tier slider.
			if (unit.Tiered())
			{
				QStringList positions;
				for (const Preset& p : unit.presets) positions << Q(p.tier);
				TierBox t = MakeTierBox(unitId + QStringLiteral(".tier"), Q(unit.name) + QStringLiteral(" tier"), positions,
					QStringLiteral("The %1 tier: each step sets the settings listed in its description (Advanced > Set by the "
						"tier). A changed setting shows Custom; Reset to tier puts it back.").arg(Q(unit.name)));
				layout->addWidget(t.box);
				state->own.push_back(t.box);
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
						t.description->setText(QStringLiteral("The settings differ from %1 (Advanced > Set by the tier). Reset to "
							"tier puts them back.").arg(Q(u->presets[static_cast<size_t>(t.slider->value())].tier)));
					}
				});
			}
			else
			{
				auto* reason = Help(QStringLiteral("Not tiered. ") + Q(unit.untieredReason));
				reason->setObjectName(ObjectName(unitId + QStringLiteral(".untiered")));
				layout->addWidget(reason);
			}

			// 2. The unit's cards (VpvOrganisation.h): Main, then Advanced, in table order.
			std::set<std::string> placed;
			for (const Card& spec : Organisation())
			{
				if (unitId != Q(spec.unit)) continue;
				CardBox card = MakeCard(ObjectName(unitId + QStringLiteral(".card.") + Q(spec.id)), Q(spec.title), Q(spec.help), spec.collapsed);
				FillCard(state, &unit, unitId, spec, card, placed);
				(spec.advanced ? advanced : layout)->addWidget(card.frame);
			}
			// A toggle the table does not place (nothing is lost) - unless it is a retirement candidate.
			{
				CardBox other = MakeCard(ObjectName(unitId + QStringLiteral(".card.other")), QStringLiteral("Other settings"), QString(), false);
				for (const Toggle& t : unit.toggles)
					if (!placed.count(t.key) && !table.FindRetirement(t.key, "vprenderer"))
						AddToggleRow(state, unitId, t, other.Form());
				if (other.Empty()) delete other.frame; else layout->addWidget(other.frame);
			}
			if (unit.id == std::string("input"))
			{
				CardBox delayCard = MakeCard(ObjectName(QStringLiteral("input.card.delay")), QStringLiteral("Total delay"), QString(), false);
				auto* delay = Help(QString());
				delay->setObjectName(ObjectName(QStringLiteral("input.delay")));
				delay->setTextFormat(Qt::RichText);
				delayCard.Add(delay);
				layout->addWidget(delayCard.frame);
				state->refreshers.push_back([state, delay]() { delay->setText(DelaySummary(state->host, TierTable())); });
			}
			if (unit.more && *unit.more)
			{
				auto* more = Help(QStringLiteral("<b>More settings of this unit</b> (on the other pages): ") + Q(unit.more).toHtmlEscaped());
				more->setTextFormat(Qt::RichText);
				more->setObjectName(ObjectName(unitId + QStringLiteral(".more")));
				layout->addWidget(more);
			}
			layout->addStretch();

			// 3. Advanced, last: the planned settings and the unit's stages.
			if (!unit.planned.empty())
			{
				CardBox planned = MakeCard(ObjectName(unitId + QStringLiteral(".card.planned")), QStringLiteral("Planned (not yet implemented)"),
					QStringLiteral("Reserved in the pipeline; their components are not built yet."), true);
				for (const Planned& p : unit.planned)
				{
					auto* box = new QCheckBox(QStringLiteral("(not yet implemented)"));
					box->setObjectName(ObjectName(unitId + QStringLiteral(".planned.") + Q(p.key)));
					box->setEnabled(false);
					box->setToolTip(QStringLiteral("%1. Planned key %2 (slot %3): reserved in the pipeline; its component is not built yet.")
						.arg(Q(p.label), Q(p.key), Q(p.slot)));
					auto* name = new QLabel(Q(p.label));
					name->setWordWrap(true);
					planned.Form()->addRow(name, box);
				}
				advanced->addWidget(planned.frame);
			}
			CardBox stages = MakeCard(ObjectName(unitId + QStringLiteral(".card.stages")), QStringLiteral("Stages in this unit (pipeline order)"),
				QString(), true);
			auto* slotsHeading = Bold(ObjectName(unitId + QStringLiteral(".slots_heading")));
			slotsHeading->setText(QStringLiteral("Each stage and what it falls back to"));
			stages.Add(slotsHeading);
			QStringList lines;
			for (const char* id : unit.slotIds) lines << SlotLine(table, id);
			auto* slotLabel = Help(lines.join(QStringLiteral("<br>")));
			slotLabel->setTextFormat(Qt::RichText);
			slotLabel->setObjectName(ObjectName(unitId + QStringLiteral(".slots")));
			stages.Add(slotLabel);
			advanced->addWidget(stages.frame);
			advanced->addStretch();
			return page;
		}

		// The expert page: the skeleton's global pipeline keys (ADR-0019/0021, R1), stored in [vpv]
		// so Config's renderer-profile migrations never move them. The order is checked with the
		// renderer's own validator (SlotRegistry / OrderValidator) and its plain-language reason is
		// shown; an invalid order is never written, so the renderer never has to reject one.
		QWidget* MakeExpertPage(const std::shared_ptr<State>& state)
		{
			const QString section = Q(TierTable().section);
			QWidget* page = NewTabPage(QStringLiteral("Pipeline (expert)"));
			page->setObjectName(ObjectName(QStringLiteral("expert")));
			state->own.push_back(page);
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

			// B3 (ADR-0024): the motion pictures from the processed A and B (the input side once per
			// source frame). A diagnostic switch until it passes its acceptance criteria.
			auto* entranceOnce = new QComboBox;
			entranceOnce->setObjectName(ObjectName(QStringLiteral("expert.pipeline_entrance_once")));
			entranceOnce->addItem(QStringLiteral("Off (default; the stages run on every picture)"), QString());
			entranceOnce->addItem(QStringLiteral("On (stages once per source frame; under test)"), QStringLiteral("on"));
			entranceOnce->setToolTip(QStringLiteral("Key pipeline_entrance_once. On: interpolated pictures, hiccup repairs and "
				"warped extras are made from each frame's processed picture, so the input stages run once per source frame "
				"(design B3). The Original pathway never uses it."));
			form->addRow(QStringLiteral("Input stages once per frame"), entranceOnce);
			QObject::connect(entranceOnce, qOverload<int>(&QComboBox::currentIndexChanged), entranceOnce, [state, entranceOnce, section](int i)
			{
				if (state->refreshing || i < 0 || !state->host.setValue) return;
				state->host.setValue(section, QStringLiteral("pipeline_entrance_once"), entranceOnce->itemData(i).toString());
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

			state->refreshers.push_back([state, lutStage, entranceOnce, order, slotsOff, section]()
			{
				const auto get = [&](const char* key) { return state->host.value ? state->host.value(section, QString::fromLatin1(key)).trimmed() : QString(); };
				{
					const QSignalBlocker blocker(lutStage);
					lutStage->setCurrentIndex(get("calibration_lut_stage").compare(QStringLiteral("vp"), Qt::CaseInsensitive) == 0 ? 1 : 0);
				}
				{
					const QSignalBlocker blocker(entranceOnce);
					entranceOnce->setCurrentIndex(get("pipeline_entrance_once").compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0 ? 1 : 0);
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
		// in use; the same text is appended to the control's own tooltip (MarkRetirements). The
		// renderer-profile candidates' controls themselves live here (organisation pass: candidates
		// only on this tab); [general], [led_drive] and [queue] ones stay on their own pages.
		QWidget* MakeRetirementPage(const std::shared_ptr<State>& state)
		{
			const Table& table = TierTable();
			QWidget* page = NewTabPage(QStringLiteral("Proposed retirement"));
			page->setObjectName(ObjectName(QStringLiteral("retirement")));
			auto* layout = new QVBoxLayout(page);
			auto* intro = Help(Q(table.retirementNote));
			intro->setObjectName(ObjectName(QStringLiteral("retirement.note")));
			layout->addWidget(intro);

			// VP-V toggles that are candidates (their unit tabs leave them out).
			CardBox toggles = MakeCard(ObjectName(QStringLiteral("retirement.card.toggles")),
				QStringLiteral("VP-V switches proposed for retirement"), QString(), false);
			for (const Unit& u : table.units)
				for (const Toggle& t : u.toggles)
					if (table.FindRetirement(t.key, "vprenderer")) AddToggleRow(state, Q(u.id), t, toggles.Form());
			if (toggles.Empty()) delete toggles.frame; else layout->addWidget(toggles.frame);
			std::set<std::string> unused;
			for (const Card& spec : Organisation())
			{
				if (std::string(spec.unit) != "retirement") continue;
				CardBox card = MakeCard(ObjectName(QStringLiteral("retirement.card.") + Q(spec.id)), Q(spec.title), Q(spec.help), spec.collapsed);
				FillCard(state, nullptr, QStringLiteral("retirement"), spec, card, unused);
				layout->addWidget(card.frame);
			}

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
						QList<QWidget*> controls = UpstreamControls(state->host.upstreamRoot, Q(r.section), Q(r.key));
						if (controls.isEmpty() && state->root)
							controls = UpstreamControls(state->root, Q(r.section), Q(r.key));
						QString where = controls.isEmpty() ? QStringLiteral("no control found") : PageOf(controls.front());
						if (!controls.isEmpty() && !where.startsWith(QStringLiteral("VP-V"))) where += QStringLiteral(" page");
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
						rows += QStringLiteral("<p><code>%1</code> &mdash; %2 &mdash; this profile: %3<br><i>%4</i></p>")
							.arg(Q(r.key).toHtmlEscaped(), where.toHtmlEscaped(),
								value.isEmpty() ? QStringLiteral("-") : value.toHtmlEscaped(), Q(r.reason).toHtmlEscaped());
					}
					if (rows.isEmpty()) continue;
					html += QStringLiteral("<p><b>%1. %2</b> (%3)</p>%4")
						.arg(Q(g.key), Q(g.value).toHtmlEscaped(),
							std::string(g.key) == "D" ? QStringLiteral("proposed: hide as diagnostics") : QStringLiteral("proposed retirement"),
							rows);
				}
				list->setText(html);
			});
			return page;
		}

		// Rows of VP-V-managed keys that sit on the VP-V page (inside moved sections) are hidden
		// while the VP-V pathway is on: the VP-V tier or toggle above them is the live control. The
		// Original pathway shows them again (only the rows hidden here).
		// The home of a tier key's row (Advanced > Set by the tier): there the row is the key's one control.
		bool InKeyHome(const QWidget* w)
		{
			for (const QWidget* p = w; p; p = p->parentWidget())
				if (p->property("vpvKeyHome").toBool()) return true;
			return false;
		}

		void SetRowShown(QWidget* field, bool vpvOn)
		{
			auto* form = qobject_cast<QFormLayout*>(field->parentWidget()->layout());
			int row = -1;
			QFormLayout::ItemRole role;
			if (form) form->getWidgetPosition(field, &row, &role);
			if (row < 0) return;
			auto* label = qobject_cast<QWidget*>(field->property("vpvRowLabel").value<QObject*>());
			if (vpvOn && form->isRowVisible(row) && !field->property("vpvHidRow").toBool())
			{
				field->setProperty("vpvHidRow", true);
				form->setRowVisible(row, false);
				if (label) form->setRowVisible(label, false);
			}
			else if (!vpvOn && field->property("vpvHidRow").toBool())
			{
				field->setProperty("vpvHidRow", false);
				form->setRowVisible(row, true);
				if (label) form->setRowVisible(label, true);
			}
		}

		// Exactly one live control per key: while the VP-V pathway is on, every upstream row of a key a
		// VP-V toggle owns is hidden, wherever it is (the Rendering page's raw group or a VP-V card); so
		// are the headings of hidden rows. The Original pathway shows them again (only the rows hidden
		// here). A tier key's row in its "Set by the tier" card is its one control and stays.
		// Organisation pass 2 (coordinator 2026-10-24): in BOTH pathways - VP-V's own controls are the
		// one place for VP's keys (disabled in the Original pathway, where every VP addition is off), so
		// the duplicate rows never come back. The parameter stays for the pathway switch's symmetry.
		void ApplyManagedRows(const std::shared_ptr<State>& state, bool /*vpvOn*/)
		{
			QStringList keys = state->hidden;
			for (const std::string& key : ManagedKeys(TierTable())) keys << QString::fromStdString(key);
			for (QWidget* root : { state->host.upstreamRoot, state->root })
				for (const QString& key : keys)
					for (QWidget* field : RowFields(root, key))
						if (!InKeyHome(field)) SetRowShown(field, true);
			// The Rendering page's raw group shows only while it holds a shown control.
			if (QWidget* group = state->host.upstreamRoot ? state->host.upstreamRoot->findChild<QWidget*>(
					QStringLiteral("rendererSection.allRawGroup")) : nullptr)
			{
				bool any = false;
				for (QFormLayout* form : group->findChildren<QFormLayout*>())
					for (int r = 0; r < form->rowCount() && !any; ++r)
					{
						QLayoutItem* item = form->itemAt(r, QFormLayout::FieldRole);
						if (!item) item = form->itemAt(r, QFormLayout::SpanningRole);
						QWidget* field = item ? item->widget() : nullptr;
						if (!field || !form->isRowVisible(r)) continue;
						const bool named = field->objectName().startsWith(QStringLiteral("config.")) ||
							!field->findChildren<QWidget*>(QRegularExpression(QStringLiteral("^config\."))).isEmpty();
						any = named;
					}
				group->setVisible(any);
			}
		}

		// The Rendering page after the moves: Bill's sections stay as his themed cards; every other
		// section left there goes into one collapsed "All raw settings (expert)" group at the bottom
		// (the safety net), after a line saying where VP's settings went.
		void OrganiseRendering(const Host& host)
		{
			QWidget* tone = UpstreamSection(host.upstreamRoot, QStringLiteral("toneMapping"));
			QWidget* fields = tone ? tone->parentWidget() : nullptr;
			auto* fieldsLayout = fields ? qobject_cast<QBoxLayout*>(fields->layout()) : nullptr;
			if (!fieldsLayout || fields->findChild<QToolButton*>(QStringLiteral("rendererSection.allRaw"))) return;
			std::set<QString> bill;
			for (const char* id : UpstreamRenderingSections()) bill.insert(Q(id));
			QList<QWidget*> rest;
			for (int i = 0; i < fieldsLayout->count(); ++i)
			{
				QWidget* w = fieldsLayout->itemAt(i)->widget();
				if (!w) continue;
				for (QToolButton* t : w->findChildren<QToolButton*>(QRegularExpression(QStringLiteral("^rendererSection\\.[A-Za-z0-9]+$")),
					Qt::FindDirectChildrenOnly))
					if (!bill.count(t->objectName().mid(16))) rest << w;
			}
			auto* pointer = Help(QStringLiteral("<b>VP's own settings</b> (motion and de-judder, deband and texture, re-base and "
				"grain-aware scaling, SDR tone shape, room light, temporal dither) are on the <b>VP-V</b> page, one tab per "
				"unit. Bill's settings stay here."));
			pointer->setTextFormat(Qt::RichText);
			pointer->setObjectName(ObjectName(QStringLiteral("rendering.pointer")));
			pointer->setProperty("help", true);
			fieldsLayout->addWidget(pointer);

			auto* group = new QWidget(fields);
			group->setObjectName(QStringLiteral("rendererSection.allRawGroup"));
			auto* groupLayout = new QVBoxLayout(group);
			groupLayout->setContentsMargins(0, 0, 0, 0);
			groupLayout->setSpacing(5);
			auto* toggle = new QToolButton;
			toggle->setObjectName(QStringLiteral("rendererSection.allRaw"));
			toggle->setText(QStringLiteral("All raw settings (expert)"));
			toggle->setProperty("profileSection", true);
			toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
			toggle->setArrowType(Qt::RightArrow);
			toggle->setCheckable(true);
			toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			groupLayout->addWidget(toggle);
			auto* content = new QWidget(group);
			content->setObjectName(QStringLiteral("rendererSection.allRaw.content"));
			auto* contentLayout = new QVBoxLayout(content);
			contentLayout->setContentsMargins(12, 4, 4, 8);
			contentLayout->setSpacing(7);
			auto* note = Help(QStringLiteral("The rest of this profile's settings, as they were: controls VP-V manages (greyed "
				"\"Managed by VP-V\"; change them on the VP-V page), the switches it does not show elsewhere, and "
				"diagnostics. Nothing here needs changing for normal viewing."));
			note->setProperty("help", true);
			contentLayout->addWidget(note);
			for (QWidget* w : rest)
			{
				fieldsLayout->removeWidget(w);
				contentLayout->addWidget(w);
			}
			groupLayout->addWidget(content);
			fieldsLayout->addWidget(group);
			content->setVisible(false);
			QObject::connect(toggle, &QToolButton::toggled, toggle, [toggle, content](bool open)
			{
				content->setVisible(open);
				toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
			});
		}

		// Video conversion / container colour space / HDR colour space / HDR luminance have a
		// control on General and an override on each renderer's Input processing page (Bill's
		// layered design, kept). A note names General as the one place to set them.
		void NoteAbove(QWidget* root, const QString& controlName, const QString& objectName, const QString& text)
		{
			auto* control = root ? root->findChild<QWidget*>(controlName) : nullptr;
			if (!control || root->findChild<QLabel*>(objectName)) return;
			for (QWidget* f = control; f && f->parentWidget(); f = f->parentWidget())
				if (auto* form = qobject_cast<QFormLayout*>(f->parentWidget()->layout()))
				{
					int row = -1;
					QFormLayout::ItemRole role;
					form->getWidgetPosition(f, &row, &role);
					if (row < 0) continue;
					auto* note = Help(text);
					note->setObjectName(objectName);
					note->setProperty("help", true);
					form->insertRow(row, note);
					return;
				}
		}

		void NoteInputPolicy(const Host& host)
		{
			NoteAbove(host.upstreamRoot, QStringLiteral("config.general.video_conversion"), ObjectName(QStringLiteral("note.input_policy.general")),
				QStringLiteral("Set these four once, here. The VP Renderer and DirectShow pages each have an override for them "
					"(Input processing); they show \"Inherited\" while they follow this page, which is normal."));
			for (const char* section : { "vprenderer.input_processing", "directshow" })
				NoteAbove(host.upstreamRoot, QStringLiteral("config.%1.video_conversion").arg(Q(section)),
					ObjectName(QStringLiteral("note.input_policy.") + Q(section)),
					QStringLiteral("Overrides of General > Input processing, for this renderer only. Leave them on Inherited "
						"unless this renderer must differ from General."));
		}
	}

	namespace
	{
		// ---- Dynamic Drive: the Advanced LED card regrouped under two masters (organisation pass 2) ----

		// The form rows of a control named <prefix><key>[.<part>] (prefix e.g. "config.led_drive.default.").
		QList<QWidget*> PrefixedRowFields(QWidget* root, const QString& prefix, const QString& key)
		{
			QList<QWidget*> fields;
			if (!root) return fields;
			const QRegularExpression name(QStringLiteral("^%1%2(\\.[^.]+)?$").arg(QRegularExpression::escape(prefix), QRegularExpression::escape(key)));
			for (QWidget* w : root->findChildren<QWidget*>(name))
				for (QWidget* f = w; f && f->parentWidget() && f != root; f = f->parentWidget())
					if (auto* form = qobject_cast<QFormLayout*>(f->parentWidget()->layout()))
					{
						int row = -1;
						QFormLayout::ItemRole role = QFormLayout::LabelRole;
						form->getWidgetPosition(f, &row, &role);
						if (row < 0) continue;
						if (role != QFormLayout::LabelRole && !fields.contains(f)) fields << f;
						break;
					}
			return fields;
		}

		// The Config card frame titled `title` (createCard: a "card" frame with a "cardTitle" label).
		QWidget* CardTitled(const QList<QWidget*>& cards, const QString& title)
		{
			for (QWidget* card : cards)
				for (QLabel* label : card->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly))
					if (label->property("cardTitle").toBool() && label->text() == title) return card;
			return nullptr;
		}

		// The Dynamic Drive page in the same scheme as the VP-V units (organisation pass 2): Main holds the
		// masters (the drive and its command source, the LED profile, the LED speeds preset, the projector
		// connection); Advanced holds what they set or tune, greyed "Set by <master>" until Custom, and the
		// projector's measured limits. The page's own show/hide by command source keeps working (it shows
		// and hides the same card frames). Returns the new page body (for the width fit), or null.
		QWidget* OrganiseDynamicDrive(const Host& host)
		{
			QWidget* root = host.upstreamRoot;
			const QList<QToolButton*> toggles = root ? root->findChildren<QToolButton*>(
				QRegularExpression(QStringLiteral("^config\\.led_drive\\.[^.]+\\.advanced_toggle$"))) : QList<QToolButton*>();
			QToolButton* toggle = toggles.isEmpty() ? nullptr : toggles.front();
			if (!toggle || root->findChild<QComboBox*>(ObjectName(QStringLiteral("drive.advanced_values")))) return nullptr;
			const QString prefix = toggle->objectName().chopped(QStringLiteral("advanced_toggle").size());
			QWidget* holder = toggle->parentWidget();
			QWidget* advanced = holder && holder->layout() && holder->layout()->count() > 1 ? holder->layout()->itemAt(1)->widget() : nullptr;
			auto* advancedForm = advanced ? qobject_cast<QFormLayout*>(advanced->layout()) : nullptr;
			auto* source = root->findChild<QComboBox*>(prefix + QStringLiteral("command_source"));
			auto* enabled = root->findChild<QCheckBox*>(prefix + QStringLiteral("enabled"));
			// The page's card grid and the page column it sits in.
			QWidget* grid = holder;
			while (grid && !qobject_cast<QGridLayout*>(grid->layout())) grid = grid->parentWidget();
			auto* column = grid && grid->parentWidget() ? qobject_cast<QBoxLayout*>(grid->parentWidget()->layout()) : nullptr;
			if (!advancedForm || !grid || !column) return nullptr;
			QList<QWidget*> cards;
			for (int i = 0; i < grid->layout()->count(); ++i)
				if (QWidget* w = grid->layout()->itemAt(i)->widget()) cards << w;

			// Main | Advanced, replacing the two-column grid (which is deleted once its cards are taken out,
			// so it can never take them back on a resize).
			auto* pages = new QTabWidget;
			pages->setObjectName(ObjectName(QStringLiteral("drive.pages")));
			pages->setDocumentMode(true);
			auto* mainPage = new QWidget;
			mainPage->setObjectName(ObjectName(QStringLiteral("drive.main")));
			auto* mainLayout = new QVBoxLayout(mainPage);
			mainLayout->setContentsMargins(0, 8, 0, 0);
			mainLayout->setSpacing(12);
			auto* advancedPage = new QWidget;
			advancedPage->setObjectName(ObjectName(QStringLiteral("drive.advanced")));
			auto* advancedLayout = new QVBoxLayout(advancedPage);
			advancedLayout->setContentsMargins(0, 8, 0, 0);
			advancedLayout->setSpacing(12);
			auto* intro = Help(QStringLiteral("Settings set by the controls on Main (greyed, \"Set by ...\", until you choose "
				"Custom), the tuning of Compensate, and the projector's measured limits. Cards a command source does not "
				"use are hidden."));
			intro->setProperty("help", true);
			advancedLayout->addWidget(intro);
			pages->addTab(mainPage, QStringLiteral("Main"));
			pages->addTab(advancedPage, QStringLiteral("Advanced"));
			pages->setTabToolTip(0, QStringLiteral("Whether Dynamic Drive runs, what drives it, its LED profile and speed preset, and the projector link."));
			pages->setTabToolTip(1, QStringLiteral("What those choices set (greyed until Custom), Compensate's tuning and the projector's measured limits."));
			column->insertWidget(column->indexOf(grid), pages);
			auto place = [&](const QString& title, QVBoxLayout* to)
			{
				if (QWidget* card = CardTitled(cards, title)) { to->addWidget(card); cards.removeAll(card); }
			};
			for (const char* title : { "Dynamic Drive", "LED profile", "LED speeds", "Projector connection" })
				place(Q(title), mainLayout);

			// LED speeds: the preset stays on Main; its nine values go to Advanced, set by it until Custom.
			CardBox speedValues = MakeCard(ObjectName(QStringLiteral("drive.card.speed_values")), QStringLiteral("LED speeds: values"),
				QStringLiteral("Levels per second (gain x distance, kept between min and max). \"preset\" = the LED speeds "
					"preset on Main."), false);
			auto* speedNote = Help(QString());
			speedNote->setProperty("help", true);
			speedValues.Add(speedNote);
			auto* speedCustom = new QCheckBox(QStringLiteral("Custom (change these by hand)"));
			speedCustom->setObjectName(ObjectName(QStringLiteral("drive.card.speed_values.custom")));
			speedCustom->setToolTip(QStringLiteral("Off: every speed follows the LED speeds preset on Main (greyed). On: set them "
				"by hand. Unticking puts every field back to \"preset\"."));
			speedValues.Add(speedCustom);
			auto* speedRows = new QWidget;
			auto* speedForm = new QFormLayout(speedRows);
			speedForm->setContentsMargins(0, 0, 0, 0);
			speedValues.Add(speedRows);
			static const char* kSpeedKeys[] = { "rate_gain_up", "rate_gain_down", "recovery_rate_gain" };
			QList<QDoubleSpinBox*> speedSpins;
			for (const char* key : kSpeedKeys)
				for (QWidget* f : PrefixedRowFields(root, prefix, Q(key))) MoveRow(f, speedForm, false);
			for (const char* key : { "rate_gain_up", "rate_min_up", "rate_max_up", "rate_gain_down", "rate_min_down", "rate_max_down",
				"recovery_rate_gain", "recovery_rate_min", "recovery_rate_max" })
				if (auto* spin = root->findChild<QDoubleSpinBox*>(prefix + Q(key))) speedSpins << spin;
			advancedLayout->addWidget(speedValues.frame);

			place(QStringLiteral("Contrast and cuts"), advancedLayout);

			// Advanced LED: its card keeps the two masters; each group becomes a card of its own.
			auto* values = new QComboBox;
			values->setObjectName(ObjectName(QStringLiteral("drive.advanced_values")));
			values->addItem(QStringLiteral("Your script's values (default)"), QStringLiteral("default"));
			values->addItem(QStringLiteral("Custom (change them below)"), QStringLiteral("custom"));
			values->setToolTip(QStringLiteral("The master of the advanced LED groups below. Your script's values: each field shows "
				"the built-in value (your dimming script's) and is greyed. Custom: edit them; choosing your script's values "
				"again removes every change (each field's Reset)."));
			auto* fixes = new QComboBox;
			fixes->setObjectName(ObjectName(QStringLiteral("drive.led_fixes")));
			fixes->addItem(QStringLiteral("All on (default)"), QStringLiteral("default"));
			fixes->addItem(QStringLiteral("Custom (switch them one by one)"), QStringLiteral("custom"));
			fixes->setToolTip(QStringLiteral("The master of the nine LED fixes under test (Dynamic Drive, Contrast and cuts, "
				"Experimental cards; proposed for retirement, which keeps them on). All on: greyed on. Custom: switch them one by one."));
			advancedForm->insertRow(1, QStringLiteral("Advanced LED values"), values);
			advancedForm->insertRow(2, QStringLiteral("LED fixes under test"), fixes);
			toggle->setChecked(true);    // the card is on Advanced already: no second "show advanced" step
			toggle->setVisible(false);
			QWidget* advancedLedCard = CardTitled(cards, QStringLiteral("Advanced LED"));
			place(QStringLiteral("Advanced LED"), advancedLayout);

			struct Group { QPointer<QWidget> card; QPointer<QWidget> rows; QPointer<QLabel> note; const char* mode; QStringList keys; };
			auto groups = std::make_shared<std::vector<Group>>();
			for (const LedGroup& g : LedAdvancedGroups())
			{
				CardBox card = MakeCard(ObjectName(QStringLiteral("drive.group.") + Q(g.id)), Q(g.title), Q(g.help), false);
				auto* note = Help(QString());
				note->setProperty("help", true);
				card.Add(note);
				auto* rows = new QWidget;
				auto* rowsForm = new QFormLayout(rows);
				rowsForm->setContentsMargins(0, 0, 0, 0);
				card.Add(rows);
				Group group{ card.frame, rows, note, g.mode, {} };
				for (const char* key : g.keys)
				{
					group.keys << Q(key);
					for (QWidget* f : PrefixedRowFields(advanced, prefix, Q(key))) MoveRow(f, rowsForm, false);
				}
				advancedLayout->addWidget(card.frame);
				groups->push_back(group);
			}
			for (const char* title : { "Projector response", "Advanced projector", "Experimental" })
				place(Q(title), advancedLayout);
			for (QWidget* card : cards) advancedLayout->addWidget(card);   // anything else, last (nothing is lost)
			// Every card is out of the old grid now (a layout reparents its widgets and keeps a card the
			// page hid hidden); delete the grid so it can never take them back on a resize.
			delete grid;
			mainLayout->addStretch();
			advancedLayout->addStretch();

			QWidget* speedsCard = nullptr;
			if (QWidget* any = mainPage->findChild<QComboBox*>(prefix + QStringLiteral("rate_preset")))
				for (QWidget* p = any; p && !speedsCard; p = p->parentWidget())
					if (p->property("card").toBool()) speedsCard = p;
			auto apply = [groups, values, fixes, source, enabled, root, prefix, advancedLedCard, speedsCard, speedValues, speedRows,
				speedNote, speedCustom, speedSpins]()
			{
				const bool driveOn = !enabled || enabled->isChecked();
				const bool custom = values->currentData().toString() == QStringLiteral("custom");
				const QString chosen = source ? source->currentData().toString() : QString();
				// The group cards follow the Advanced LED card (shown for VP's own sources, greyed with the drive off).
				const bool groupsShown = !advancedLedCard || !advancedLedCard->isHidden();
				for (Group& g : *groups)
				{
					if (!g.card) continue;
					g.card->setVisible(groupsShown);
					g.card->setEnabled(driveOn);
					const bool modeOk = !g.mode || chosen == Q(g.mode);
					g.rows->setEnabled(custom && modeOk);
					g.note->setText(!modeOk ? QStringLiteral("Used only in %1 mode.").arg(std::string(g.mode) == "compensate" ?
						QStringLiteral("Compensate") : QStringLiteral("Script")) :
						!custom ? QStringLiteral("Set by Advanced LED values: your script's values. Choose Custom to change.") : QString());
					g.note->setVisible(!g.note->text().isEmpty());
				}
				const bool fixesCustom = fixes->currentData().toString() == QStringLiteral("custom");
				for (const char* key : LedFixKeys())
					if (auto* box = root->findChild<QCheckBox*>(prefix + Q(key)))
					{
						box->setEnabled(fixesCustom);
						if (!box->property("vpvFixTip").isValid()) box->setProperty("vpvFixTip", box->toolTip());
						box->setToolTip(fixesCustom ? box->property("vpvFixTip").toString() :
							QStringLiteral("Set by LED fixes under test: all on (Advanced LED card). Choose Custom there to switch "
								"it.\n\n") + box->property("vpvFixTip").toString());
					}
				// LED speeds values follow their card on Main (Compensate only) and the preset until Custom.
				speedValues.frame->setVisible(!speedsCard || !speedsCard->isHidden());
				speedValues.frame->setEnabled(driveOn);
				speedRows->setEnabled(speedCustom->isChecked());
				speedNote->setText(speedCustom->isChecked() ? QString() :
					QStringLiteral("Set by the LED speeds preset (Main). Tick Custom to change these by hand."));
				speedNote->setVisible(!speedNote->text().isEmpty());
			};
			// Custom follows the saved values: any field set (its Reset is enabled) / any fix off / any speed not "preset".
			bool anySet = false;
			for (const Group& g : *groups)
				for (const QString& key : g.keys)
					if (auto* reset = root->findChild<QToolButton*>(prefix + key + QStringLiteral(".reset")))
						anySet = anySet || reset->isEnabled();
			bool anyFixOff = false;
			for (const char* key : LedFixKeys())
				if (auto* box = root->findChild<QCheckBox*>(prefix + Q(key))) anyFixOff = anyFixOff || !box->isChecked();
			bool anySpeed = false;
			for (QDoubleSpinBox* spin : speedSpins) anySpeed = anySpeed || spin->value() > spin->minimum();
			values->setCurrentIndex(anySet ? 1 : 0);
			fixes->setCurrentIndex(anyFixOff ? 1 : 0);
			speedCustom->setChecked(anySpeed);
			apply();
			QObject::connect(values, qOverload<int>(&QComboBox::currentIndexChanged), values, [groups, values, root, prefix, apply](int)
			{
				if (values->currentData().toString() == QStringLiteral("default"))
					for (const Group& g : *groups)
						for (const QString& key : g.keys)
							if (auto* reset = root->findChild<QToolButton*>(prefix + key + QStringLiteral(".reset")))
								if (reset->isEnabled()) reset->click();
				apply();
			});
			QObject::connect(fixes, qOverload<int>(&QComboBox::currentIndexChanged), fixes, [fixes, root, prefix, apply](int)
			{
				if (fixes->currentData().toString() == QStringLiteral("default"))
					for (const char* key : LedFixKeys())
						if (auto* box = root->findChild<QCheckBox*>(prefix + Q(key)))
							if (!box->isChecked())
							{
								box->setEnabled(true);
								box->click();   // the page's own handler writes the key
							}
				apply();
			});
			QObject::connect(speedCustom, &QCheckBox::toggled, speedCustom, [speedSpins, apply](bool on)
			{
				if (!on)
					for (QDoubleSpinBox* spin : speedSpins) spin->setValue(spin->minimum());   // "preset": the page removes the key
				apply();
			});
			if (source)
				QObject::connect(source, qOverload<int>(&QComboBox::currentIndexChanged), values, [apply](int) { apply(); });
			if (enabled)
				QObject::connect(enabled, &QCheckBox::toggled, values, [apply](bool) { apply(); });
			return pages;
		}

		// The VP-V page fits the default window (no sideways scroll): drop-downs size to a short text
		// (the popup still shows the whole entry), long form rows wrap their field under the label,
		// and the tab rows scroll instead of widening the page.
		void FitWidth(QWidget* root)
		{
			for (QComboBox* combo : root->findChildren<QComboBox*>())
			{
				combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
				combo->setMinimumContentsLength(14);
			}
			for (QTabWidget* tabs : root->findChildren<QTabWidget*>())
			{
				tabs->setUsesScrollButtons(true);
				tabs->tabBar()->setExpanding(false);
			}
			for (QLabel* label : root->findChildren<QLabel*>())
				if (label->text().size() > 60 && !label->wordWrap()) label->setWordWrap(true);
			// A row of choice buttons wider than half the page stacks them (the same buttons, top to bottom).
			QList<QHBoxLayout*> wide;
			for (QHBoxLayout* row : root->findChildren<QHBoxLayout*>())
			{
				int radios = 0;
				for (int i = 0; i < row->count(); ++i)
					radios += qobject_cast<QRadioButton*>(row->itemAt(i)->widget()) ? 1 : 0;
				if (radios >= 2 && row->minimumSize().width() >= 420) wide << row;
			}
			for (QHBoxLayout* row : wide)
			{
				QList<QWidget*> widgets;
				for (int i = 0; i < row->count(); ++i)
					if (QWidget* item = row->itemAt(i)->widget()) widgets << item;
				auto* column = new QVBoxLayout;
				column->setContentsMargins(0, 0, 0, 0);
				column->setSpacing(4);
				if (auto* parent = qobject_cast<QBoxLayout*>(row->parent()))
				{
					const int index = parent->indexOf(row);
					delete parent->takeAt(index);   // the row layout itself (its widgets stay)
					parent->insertLayout(index, column);
				}
				else if (QWidget* owner = row->parentWidget(); owner && owner->layout() == row)
				{
					delete row;
					owner->setLayout(column);
				}
				else
				{
					delete column;
					continue;
				}
				for (QWidget* item : widgets) column->addWidget(item, 0, Qt::AlignLeft);
			}
			// Forms: each field under its label (the cards are narrow; a label beside a wide field would
			// widen the page), the same on every card.
			for (QFormLayout* form : root->findChildren<QFormLayout*>())
				form->setRowWrapPolicy(QFormLayout::WrapAllRows);
		}

	}

	namespace
	{
		void MarkRetirementsIn(QWidget* root)
		{
			const Table& table = TierTable();
			for (const Retirement& r : table.retirements)
				for (QWidget* w : UpstreamControls(root, Q(r.section), Q(r.key)))
				{
					if (w->property("vpvRetirementMarked").toBool()) continue;
					w->setProperty("vpvRetirementMarked", true);
					w->setToolTip((w->toolTip().isEmpty() ? QString() : w->toolTip() + QStringLiteral("\n\n")) + RetirementTip(table, r));
				}
		}
	}

	void MarkRetirements(const Host& host)
	{
		MarkRetirementsIn(host.upstreamRoot);
	}

	bool PathwayIsVpv(const Host& host)
	{
		const Table& table = TierTable();
		// The renderer's precedence (skeleton audit item 6): [vpv] first, then [vprenderer].
		QString v = host.value ? host.value(Q(table.section), Q(table.pathwayKey)).trimmed() : QString();
		if (v.isEmpty() && host.value) v = host.value(QStringLiteral("vprenderer"), Q(table.pathwayKey)).trimmed();
		if (v.isEmpty()) v = Q(table.pathwayDefault);
		// pipeline_original_pathway on = the Original pathway (ADR-0019); off/absent = VP-V.
		return v.compare(QStringLiteral("on"), Qt::CaseInsensitive) != 0;
	}

	void ApplyManagedState(const Host& host, bool vpvOn)
	{
		ApplyManagedIn(host, host.upstreamRoot, vpvOn);
	}

	void ApplyManagedIn(const Host& host, QWidget* root, bool vpvOn)
	{
		if (!root || !host.upstreamControlName) return;
		for (const std::string& key : ManagedKeys(TierTable()))
		{
			const QString name = host.upstreamControlName(QString::fromStdString(key));
			if (name.isEmpty()) continue;
			// The key's control itself (exact name). Its parts (".engine" buttons, ".check" boxes beside a
			// hidden combo) stay as upstream built them: inside VP-V cards their rows are hidden instead.
			for (QWidget* w : root->findChildren<QWidget*>(name))
			{
				if (w->objectName().startsWith(QStringLiteral("vpv.")) || InKeyHome(w)) continue;
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
		state->root = root;
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
			"(\"Managed by VP-V\"). Original: the tiers and VP-V switches are disabled and the other pages behave exactly "
			"as upstream. In the renderer, Original switches every VP addition off and runs the mixed stages in their "
			"upstream form (ADR-0019). Stored as [vpv] pipeline_original_pathway."));
		pathwayRow->addRow(QStringLiteral("Pathway"), pathway);
		layout->addLayout(pathwayRow);

		auto* content = new QWidget;
		content->setObjectName(ObjectName(QStringLiteral("content")));
		auto* contentLayout = new QVBoxLayout(content);
		contentLayout->setContentsMargins(0, 0, 0, 0);
		auto* originalNote = Help(QStringLiteral("<b>Original pathway:</b> every VP-V addition is off, so this page is "
			"disabled. Bill's own settings are on his pages (VP Renderer, DirectShow, General); switch back to the VP-V "
			"pathway to change VP's."));
		originalNote->setTextFormat(Qt::RichText);
		originalNote->setObjectName(ObjectName(QStringLiteral("original_note")));
		originalNote->setVisible(false);
		layout->addWidget(originalNote);
		layout->addWidget(content);

		// The renderer profile. It follows the Rendering page's profile list, whose selected profile
		// the moved upstream rows edit (one current renderer profile on both pages).
		auto* profileRow = new QFormLayout;
		auto* profile = new QComboBox;
		profile->setObjectName(ObjectName(QStringLiteral("profile")));
		profile->setAccessibleName(QStringLiteral("Renderer profile"));
		profile->setToolTip(QStringLiteral("The renderer profile this page edits (the same one the Rendering page shows). "
			"A named profile inherits every setting it does not set from the default profile; profile rules choose the "
			"profile (e.g. per frame rate)."));
		if (host.upstreamRoot && host.upstreamControlName)
			state->upstreamList = host.upstreamRoot->findChild<QListWidget*>(host.upstreamControlName(QStringLiteral("profiles")));
		auto fillPicker = [state, profile, host]()
		{
			QStringList sections, names;
			if (state->upstreamList)
				for (int i = 0; i < state->upstreamList->count(); ++i)
				{
					sections << state->upstreamList->item(i)->data(Qt::UserRole).toString();
					names << state->upstreamList->item(i)->data(Qt::UserRole + 1).toString();
				}
			if (sections.isEmpty())
			{
				sections = host.profileSections ? host.profileSections(kDefaultProfile) : QStringList();
				if (sections.isEmpty()) sections << kDefaultProfile;
				for (const QString& section : sections)
					names << (section.compare(kDefaultProfile, Qt::CaseInsensitive) == 0 ? QStringLiteral("(unnamed)") :
						section.mid(kDefaultProfile.size() + 1));
			}
			const QSignalBlocker blocker(profile);
			profile->clear();
			for (int i = 0; i < sections.size(); ++i)
				profile->addItem(i == 0 ? QStringLiteral("%1 (default profile)").arg(names[i]) : names[i], sections[i]);
			state->defaultProfile = sections.front();
			if (state->upstreamList && state->upstreamList->currentItem())
				state->profile = state->upstreamList->currentItem()->data(Qt::UserRole).toString();
			int index = profile->findData(state->profile, Qt::UserRole, Qt::MatchFixedString);
			if (index < 0) { index = 0; state->profile = sections.front(); }
			profile->setCurrentIndex(index);
		};
		fillPicker();
		profileRow->addRow(QStringLiteral("Profile"), profile);
		contentLayout->addLayout(profileRow);

		// The global tier.
		QStringList globalTiers;
		for (const char* g : table.tiers) globalTiers << Q(g);
		TierBox global = MakeTierBox(QStringLiteral("global_tier"), QStringLiteral("Global tier"), globalTiers,
			QStringLiteral("Sets every unit's tier at once: Classic = no advanced features (straight to the projector), "
				"Low / Mid / Upper add measured features. A unit without that step uses its nearest lower one. Each unit "
				"can then be changed on its own tab (the global tier then shows Custom)."));
		auto* globalHeading = Bold(ObjectName(QStringLiteral("global_tier.heading")));
		globalHeading->setText(QStringLiteral("Global tier (every unit at once)"));
		contentLayout->addWidget(globalHeading);
		global.reset->setText(QStringLiteral("Reset all units"));
		global.defaults->setVisible(false);
		contentLayout->addWidget(global.box);
		state->own.push_back(global.box);
		auto applyGlobal = [state](int g)
		{
			std::vector<std::pair<std::string, std::string>> writes;
			for (const Unit& u : TierTable().units)
				if (u.Tiered())
				{
					const auto unitWrites = ApplyTier(TierTable(), u, GlobalToUnit(TierTable(), u, g), state->MakeGetter());
					writes.insert(writes.end(), unitWrites.begin(), unitWrites.end());
				}
			state->Write(writes);
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
		// Also re-reads the tiers while the page is shown: a moved upstream row edits the document
		// directly, so a tier name may need to change to Custom (or back).
		QObject::connect(timer, &QTimer::timeout, fallbacks, [mirror, state, root]()
		{
			mirror();
			if (root->isVisible()) state->RefreshAll();
		});
		timer->start();

		auto* tabs = new QTabWidget;
		tabs->setObjectName(ObjectName(QStringLiteral("units")));
		tabs->setStyleSheet(QStringLiteral("QTabWidget::pane { border: 0; background: transparent; }"));
		contentLayout->addWidget(tabs);   // first, so a card can find what an earlier card moved under root
		for (const Unit& u : table.units)
		{
			tabs->addTab(MakeUnitPage(state, u), Q(u.name));
			tabs->setTabToolTip(tabs->count() - 1, Q(u.summary));
		}
		tabs->addTab(MakeExpertPage(state), QStringLiteral("Pipeline (expert)"));
		tabs->addTab(MakeRetirementPage(state), QStringLiteral("Proposed retirement"));
		tabs->setTabToolTip(tabs->count() - 2, QStringLiteral("For testing and measurement: where the calibration LUT runs, the stage order, stages switched off."));
		tabs->setTabToolTip(tabs->count() - 1, QStringLiteral("Switches proposed for retirement (still working until you decide), with why and where each is."));
		OrganiseRendering(host);
		QWidget* drivePage = OrganiseDynamicDrive(host);
		NoteInputPolicy(host);
		// One control per unit (organisation pass 2): the tier decides whether the unit's taste
		// slider applies. At the Classic Motion tier there is no de-judder, so the amount is greyed;
		// below the Mid Scaling tier re-base is off, so the picture slider is greyed (and never offers
		// its own Off, which would fight the tier).
		state->refreshers.push_back([state]()
		{
			const Table& t = TierTable();
			auto lockBox = [](QWidget* box, bool locked, const QString& why)
			{
				if (!box) return;
				if (!box->property("vpvTasteTip").isValid()) box->setProperty("vpvTasteTip", box->toolTip());
				box->setEnabled(!locked);
				box->setToolTip(locked ? why : box->property("vpvTasteTip").toString());
			};
			for (const Unit& u : t.units)
				if (u.id == std::string("motion"))
				{
					const bool classic = state->vpvOn && MatchTier(t, u, state->MakeGetter()) == 0;
					lockBox(state->root->findChild<QWidget*>(QStringLiteral("config.vprenderer.motion_preset_box")), classic,
						QStringLiteral("Set by the Motion tier: Classic has no de-judder. Choose Low or above to set the amount."));
				}
			std::string source = state->Get("source_resolution");
			if (source.empty()) source = t.Default("source_resolution");
			const bool rebaseOff = state->vpvOn && !SameValue(source, "auto");
			lockBox(state->root->findChild<QWidget*>(QStringLiteral("config.vprenderer.picture_preset_box")), rebaseOff,
				QStringLiteral("Set by the Scaling tier: re-base is off below Mid. Choose Mid to set how much evidence it needs."));
			if (auto* slider = state->root->findChild<QSlider*>(QStringLiteral("config.vprenderer.picture_preset")))
			{
				const QSignalBlocker blocker(slider);
				slider->setMinimum(state->vpvOn ? 1 : 0);
			}
		});
		FitWidth(root);
		if (drivePage) FitWidth(drivePage);   // the Dynamic Drive page fits the default window too
		root->setProperty("vpvMissing", state->missing.join(QStringLiteral("; ")));

		QObject::connect(profile, qOverload<int>(&QComboBox::currentIndexChanged), profile, [state, profile](int index)
		{
			if (index < 0) return;
			if (state->upstreamList && index < state->upstreamList->count())
			{
				state->upstreamList->setCurrentRow(index);   // loads the moved rows; the list's signal refreshes below
				return;
			}
			state->profile = profile->itemData(index).toString();
			state->RefreshAll();
		});
		if (QListWidget* list = state->upstreamList)
		{
			QObject::connect(list, &QListWidget::currentRowChanged, profile, [state, profile](int row)
			{
				if (row < 0 || !state->upstreamList) return;
				state->profile = state->upstreamList->item(row)->data(Qt::UserRole).toString();
				const QSignalBlocker blocker(profile);
				profile->setCurrentIndex((std::min)(row, profile->count() - 1));
				state->RefreshAll();
			});
			// Profiles added, removed, renamed or moved on the Rendering page: rebuild the picker
			// once the list has settled.
			auto rebuild = [profile, fillPicker, state]()
			{
				QTimer::singleShot(0, profile, [fillPicker, state]() { fillPicker(); state->RefreshAll(); });
			};
			QAbstractItemModel* model = list->model();
			QObject::connect(model, &QAbstractItemModel::rowsInserted, profile, rebuild);
			QObject::connect(model, &QAbstractItemModel::rowsRemoved, profile, rebuild);
			QObject::connect(model, &QAbstractItemModel::rowsMoved, profile, rebuild);
			QObject::connect(model, &QAbstractItemModel::dataChanged, profile, rebuild);
			QObject::connect(model, &QAbstractItemModel::modelReset, profile, rebuild);
		}

		auto applyPathway = [state](bool vpvOn)
		{
			state->vpvOn = vpvOn;
			// The Original pathway switches every VP addition off in the renderer, so the whole VP-V page
			// (its tiers, switches and the VP rows it hosts) is disabled; Bill's own settings are on his
			// pages and none of them is on this page (organisation pass 2).
			for (const QPointer<QWidget>& w : state->own)
				if (w) w->setEnabled(vpvOn);
			if (QWidget* content = state->root->findChild<QWidget*>(ObjectName(QStringLiteral("content")))) content->setEnabled(vpvOn);
			if (QWidget* note = state->root->findChild<QWidget*>(ObjectName(QStringLiteral("original_note")))) note->setVisible(!vpvOn);
			ApplyManagedState(state->host, vpvOn);
			ApplyManagedIn(state->host, state->root, vpvOn);   // the managed rows moved onto this page
			ApplyManagedRows(state, vpvOn);
			state->RefreshAll();
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
			// Written explicitly, 'off' included: an absent [vpv] key would let a [vprenderer] value win
			// (the renderer reads [vpv] first, then [vprenderer]; skeleton audit item 6).
			state->host.setValue(Q(t.section), Q(t.pathwayKey), value);
			applyPathway(value != QStringLiteral("on"));
		});
		// Before the managed greying, so the Original pathway restores tooltips that keep the mark.
		MarkRetirements(host);
		MarkRetirementsIn(root);   // the candidates' controls moved onto this page
		state->RefreshAll();
		applyPathway(PathwayIsVpv(host));
		return root;
	}
}
