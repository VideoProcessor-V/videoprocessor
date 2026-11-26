// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "VpvRenderingProfileSections.h"
#include "VpvConfigFormApi.h"
#include <DisplayRuleExpression.h>

#include <QAction>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QObject>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>

namespace Vpv
{
    void MarkGpuCost(QFormLayout* form, QWidget* control, int cost)
    {
        if (!form || !control || cost < 1 || cost > 3)
            return;
        static const char* const marks[] = { "", "+", "++", "+++" };
        static const char* const words[] = { "", "small", "moderate", "large" };
        static const char* const colours[] = { "", "#2e7d32", "#b26a00", "#c62828" };
        QLabel* label = qobject_cast<QLabel*>(form->labelForField(control));
        if (!label && control->parentWidget())
            label = qobject_cast<QLabel*>(form->labelForField(control->parentWidget()));
        if (label && !label->property("gpuCost").isValid())
        {
            label->setProperty("gpuCost", cost);
            label->setTextFormat(Qt::RichText);
            label->setText(label->text().toHtmlEscaped() +
                QStringLiteral(" <span style=\"color:%1; font-weight:600\">%2</span>")
                    .arg(QString::fromLatin1(colours[cost]), QString::fromLatin1(marks[cost])));
        }
        const QString note = QStringLiteral("GPU cost: %1 (%2, estimated).")
            .arg(QString::fromLatin1(marks[cost]), QString::fromLatin1(words[cost]));
        control->setToolTip(control->toolTip().isEmpty() ? note : note + QStringLiteral("\n") + control->toolTip());
        control->setAccessibleDescription(control->accessibleDescription().isEmpty() ? note :
            note + QStringLiteral(" ") + control->accessibleDescription());
    }

    const char* GpuCostLegend()
    {
        return "GPU cost: + small, ++ moderate, +++ large (estimated per frame; the stats overlay, Ctrl+I, shows the real render time).";
    }

    void SnapNumberFieldToStep(QLineEdit* edit, double lo, double hi, double step)
    {
        QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, lo, hi, step]() {
            if (edit->property("inherited").toBool()) return;
            const QString text = edit->text().trimmed();
            if (text.isEmpty()) return;
            bool ok = false;
            double value = QLocale::c().toDouble(text, &ok);
            if (!ok) value = text.toDouble(&ok);
            if (!ok) return;
            value = (std::min)(hi, (std::max)(lo, lo + std::round((value - lo) / step) * step));
            const QString clean = QString::number(std::round(value * 100.0) / 100.0, 'g', 12);
            if (clean != edit->text()) edit->setText(clean);
        });
    }

    void ClampNumberFieldOnLeave(QLineEdit* edit, double lo, double hi, int decimals)
    {
        QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, lo, hi, decimals]() {
            if (edit->property("inherited").toBool()) return;
            const QString text = edit->text().trimmed();
            if (text.isEmpty() || text.compare(QStringLiteral("Auto"), Qt::CaseInsensitive) == 0) return;
            bool ok = false;
            double value = QLocale::c().toDouble(text, &ok);
            if (!ok) value = text.toDouble(&ok);
            if (!ok) { edit->clear(); return; }
            value = (std::min)(hi, (std::max)(lo, value));
            const double scale = std::pow(10.0, decimals);
            value = std::round(value * scale) / scale;
            const QString clean = QString::number(value, 'g', 12);
            if (clean != edit->text()) edit->setText(clean);
        });
    }

    QString RetiredChoiceTarget(const QString& key, const QString& value)
    {
        if (key == QStringLiteral("dejudder_even_rates"))
        {
            if (value.compare(QStringLiteral("30"), Qt::CaseInsensitive) == 0) return QStringLiteral("whole");
            if (value.compare(QStringLiteral("30_and_29.97"), Qt::CaseInsensitive) == 0) return QStringLiteral("whole_and_1001");
        }
        return {};
    }

    QToolButton* AddChoiceUnsetButton(QComboBox* combo, QBoxLayout* layout, QObject* context)
    {
        auto* unset = new QToolButton;
        unset->setObjectName(combo->objectName() + QStringLiteral(".unset"));
        unset->setText(QStringLiteral("Not set"));
        unset->setToolTip(QStringLiteral(
            "Removes this profile's choice: the default profile's choice (or the built-in default) applies."));
        layout->addWidget(unset);
        QObject::connect(unset, &QToolButton::clicked, context, [combo]() {
            if (combo->count() > 0 && combo->itemData(0).toString().isEmpty()) combo->setCurrentIndex(0);
        });
        return unset;
    }

    void MarkChoiceButtons(const QComboBox* combo, const QList<QRadioButton*>& radios, QToolButton* unset)
    {
        const bool unspecified = combo->currentData().toString().isEmpty();
        const bool fromDefaultProfile = combo->property("inheritedFromDefault").isValid() ?
            combo->property("inheritedFromDefault").toBool() : combo->property("inherited").toBool();
        const QString suffix = !unspecified ? QString() :
            fromDefaultProfile ? QStringLiteral(" (inherited)") : QStringLiteral(" (built-in default)");
        for (QRadioButton* radio : radios)
        {
            if (!radio->property("baseText").isValid()) radio->setProperty("baseText", radio->text());
            radio->setText(radio->property("baseText").toString() + (radio->isChecked() ? suffix : QString()));
        }
        if (unset)
            unset->setVisible(!unspecified && combo->count() > 0 && combo->itemData(0).toString().isEmpty());
    }

    namespace
    {
        QLabel* helpLabel(const QString& text)
        {
            auto* label = new QLabel(text);
            label->setProperty("help", true);
            label->setWordWrap(true);
            return label;
        }

        QString effectiveChoiceValue(const QComboBox* combo)
        {
            if (!combo) return {};
            const QString selected = combo->currentData().toString();
            if (selected.isEmpty() && combo->property("inheritedValue").isValid())
                return combo->property("inheritedValue").toString().trimmed().toLower();
            return (selected.isEmpty() ? combo->property("effectiveValue").toString() : selected).trimmed().toLower();
        }

        QString controlName(const QString& section, const QString& key)
        {
            return QStringLiteral("config.%1.%2").arg(section, key);
        }

        const char* const kGpuCostLegend =
            "GPU cost: + small, ++ moderate, +++ large (estimated per frame; the stats overlay, Ctrl+I, shows the real render time).";

        void snapNumberFieldToStep(QLineEdit* edit, double lo, double hi, double step)
        {
            SnapNumberFieldToStep(edit, lo, hi, step);
        }

        void clampNumberFieldOnLeave(QLineEdit* edit, double lo, double hi, int decimals)
        {
            ClampNumberFieldOnLeave(edit, lo, hi, decimals);
        }

        QString retiredChoiceTarget(const QString& key, const QString& value)
        {
            return RetiredChoiceTarget(key, value);
        }

        QToolButton* addChoiceUnsetButton(QComboBox* combo, QBoxLayout* layout, QObject* context)
        {
            return AddChoiceUnsetButton(combo, layout, context);
        }

        void markChoiceButtons(const QComboBox* combo, const QList<QRadioButton*>& radios, QToolButton* unset)
        {
            MarkChoiceButtons(combo, radios, unset);
        }
    }

    RenderingSectionHandles AddVpvRenderingSections(const RenderingSectionKit& kit)
    {
        QFormLayout*& form = *kit.form;
        QObject* const receiver = kit.receiver;
        const QString& sectionPrefix = kit.sectionPrefix;
        const auto& addCollapsibleSection = kit.addCollapsibleSection;
        auto addText = [&](const QString& label, const QString& key,
            const QString& unit = {}, double displayScale = 1.0) -> QLineEdit* {
            return kit.addText(label, key, unit, displayScale);
        };
        auto addChoice = [&](const QString& label, const QString& key,
            const QStringList& choices, bool allowInherited = true) -> QComboBox* {
            return kit.addChoice(label, key, choices, allowInherited);
        };
        auto* saturation = addText(QStringLiteral("Saturation (-100 to +100)"), QStringLiteral("saturation"), QStringLiteral("%"));
        saturation->setValidator(new QIntValidator(-100, 100, saturation));
        saturation->setToolTip(QStringLiteral(
            "Colour saturation offset in percent. 0 is neutral; +5 matches madVR's saturation +5."));
        // Owner 2026-10-13: the low-latency capture path (rule-selected, e.g.
        // for sources above 59 Hz). Experimental, off by default (2026-10-14);
        // its own section, not inside Tone Mapping (audit 2026-10-14c).

        // Stage 6b-2b: the de-judder (film motion). Meant for the cinema
        // profile; the renderer switches it off by itself in the low-latency
        // queue and for video cadences. Audit 2026-10-14c (owner: 46 settings
        // is far too many): the taste settings first, shown only as they
        // apply; the measured motion estimation and the experiments in their
        // own collapsed sections; the dither's rows in a card of their own.
        form = addCollapsibleSection(QStringLiteral("dejudder"), QStringLiteral("De-judder (film motion)"),
            QStringLiteral("Taste settings, set by eye: nothing here is measured or fitted. ") +
            QString::fromLatin1(kGpuCostLegend), false);
        QFormLayout* dejudderTasteForm = form;
        // Owner 2026-10-16 (docs/VP-motion-slider.md): one question - how much should motion be
        // changed from what was shot? Five positions moving only the keys that answer it (the
        // fields below, still editable: a changed value shows Custom; Reset to preset restores).
        // Everything else is quality (always on) or advanced. Each profile has its own position,
        // so rules can give each frame rate its own level.
        auto* motionBox = new QWidget;
        motionBox->setObjectName(controlName(sectionPrefix, QStringLiteral("motion_preset_box")));
        auto* motionBoxLayout = new QVBoxLayout(motionBox);
        motionBoxLayout->setContentsMargins(0, 0, 0, 0);
        auto* motionRow = new QHBoxLayout;
        auto* motionSlider = new QSlider(Qt::Horizontal);
        motionSlider->setObjectName(controlName(sectionPrefix, QStringLiteral("motion_preset")));
        motionSlider->setAccessibleName(QStringLiteral("Motion"));
        motionSlider->setRange(0, 5);
        motionSlider->setPageStep(1);
        motionSlider->setTickPosition(QSlider::TicksBelow);
        motionSlider->setTickInterval(1);
        motionSlider->setToolTip(QStringLiteral(
            "How much motion is changed from what was shot, graded to the maximum: As shot, Film, Cinema (default), Natural, "
            "Smooth, Max. "
            // audit 2026-10-16: the slider also moves the even-cadence (30p) strength and the 1000/1001 rates.
            "Each step moves only the de-judder strength, the strength on even cadences (30p doubled to 60p), whether "
            "1000/1001 rates (23.976, 29.97) are included, the smoothing of motion within the frame and how much " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "fast action keeps its strobing; the values appear in the fields below and can be edited (Custom). "
            "Each profile has its own position, so profile rules can give each frame rate its own level "
            "(e.g. a 24p profile at Cinema, a 30p profile at Natural). Film-like sources only; off for 50p/60p "
            "video, in the low-latency queue and for interlaced input. The Motion page's Motion handling chooses "
            "VP's de-judder or libplacebo's smooth motion - one or the other, never both."));
        auto* motionReset = new QPushButton(QStringLiteral("Reset"))   /* one word for every slider's reset (organisation audit 2026-10-24) */;
        motionReset->setObjectName(controlName(sectionPrefix, QStringLiteral("motion_preset_reset")));
        motionReset->setToolTip(QStringLiteral("Puts the fields below back to the slider position's values."));
        motionRow->addWidget(new QLabel(QStringLiteral("As shot")));
        motionRow->addWidget(motionSlider, 1);
        motionRow->addWidget(new QLabel(QStringLiteral("Max")));
        motionRow->addWidget(motionReset);
        auto* motionDefault = new QPushButton(QStringLiteral("Default"));
        motionDefault->setObjectName(controlName(sectionPrefix, QStringLiteral("motion_preset_default")));
        motionDefault->setToolTip(QStringLiteral("Back to the default level, Cinema, with its values in the fields below."));
        motionRow->addWidget(motionDefault);
        motionBoxLayout->addLayout(motionRow);
        auto* motionName = new QLabel;
        motionName->setObjectName(controlName(sectionPrefix, QStringLiteral("motion_preset_name")));
        QFont motionNameFont = motionName->font();
        motionNameFont.setBold(true);
        motionName->setFont(motionNameFont);
        auto* motionDescription = new QLabel;
        motionDescription->setObjectName(controlName(sectionPrefix, QStringLiteral("motion_preset_description")));
        motionDescription->setWordWrap(true);
        motionBoxLayout->addWidget(motionName);
        motionBoxLayout->addWidget(motionDescription);
        form->addRow(QStringLiteral("Motion"), motionBox);
        auto* dejudderStrength = addText(QStringLiteral("Strength (uneven cadences, e.g. 24p on 60 Hz)"), // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            QStringLiteral("dejudder_strength"));
        dejudderStrength->setValidator(new QDoubleValidator(0.0, 1.0, 3, dejudderStrength));
        dejudderStrength->setPlaceholderText(QStringLiteral("0 = off; the slider sets it"));
        dejudderStrength->setToolTip(QStringLiteral(   // settings guide audit 2026-10-24
            "How far the 3:2 hitch of film on a 60 Hz display is eased toward even motion: 0 = as shot, 1 = full " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "frame-rate conversion. Normally set by the Motion slider above (Cinema = 0.25)."));
        // Delta 10: even cadences (a whole number of refreshes per film frame).
        auto* strengthEven = addText(QStringLiteral("Strength (even cadences, e.g. 30p on 60 Hz)"), // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            QStringLiteral("dejudder_strength_even"));
        strengthEven->setValidator(new QDoubleValidator(0.0, 1.0, 3, strengthEven));
        strengthEven->setPlaceholderText(QStringLiteral("0 = off"));
        strengthEven->setToolTip(QStringLiteral(
            "When each film frame is shown for a whole number of refreshes (e.g. 30p on 60 Hz, 25p on 50 Hz), " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "the strobing on slow pans is what shows: the repeats become evenly spaced in-between pictures. "
            "1.0 is full frame-rate conversion. Held regions keep the source rate; landscapes do well, fast "
            "action holds more. 0 = off (uneven cadences use the strength above)."));
        auto* evenRates = addChoice(QStringLiteral("Even strength applies to"),
            QStringLiteral("dejudder_even_rates"), { QStringLiteral("whole"), QStringLiteral("whole_and_1001") });
        evenRates->setItemText(evenRates->findData(QStringLiteral("whole")),
            QStringLiteral("Whole-number rates (24, 25, 30)")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        evenRates->setItemText(evenRates->findData(QStringLiteral("whole_and_1001")),
            QStringLiteral("Also 1000/1001 rates (23.976, 29.97)")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        evenRates->setToolTip(QStringLiteral(
            "1000/1001 rates mostly come from broadcast television, often video-look already, where "
            "smoothing is purely taste."));
        MarkGpuCost(form, strengthEven, 2);
        // Pulldown detection (owner 2026-10-14).
        auto* pulldown = addChoice(QStringLiteral("Find film sent with repeated frames"),
            QStringLiteral("dejudder_pulldown"), { QStringLiteral("off"), QStringLiteral("on") });
        pulldown->setToolTip(QStringLiteral(
            "Some sources send film inside a faster signal by repeating frames (e.g. 24p as 2:3 in 60 Hz). " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "On: the repeats are recognised, skipped and the film is shown at its own rate, so the de-judder "
            "above applies. Experimental; default off."));
        // Delta 14: two rules against the soap-opera look.
        auto* rolloff = addChoice(QStringLiteral("Keep fast motion's strobing"),
            QStringLiteral("dejudder_motion_rolloff"), { QStringLiteral("on"), QStringLiteral("off") });
        rolloff->setToolTip(QStringLiteral(
            "Slow and medium pans get the full de-judder; above a fast pan the strength falls back (to the floor below), "
            "so fast action keeps the strobing film is expected to have. Off = the strength only rises with motion."));
        auto* rollLo = addText(QStringLiteral("Fall from (px per frame)"), QStringLiteral("dejudder_roll_lo"));
        rollLo->setValidator(new QDoubleValidator(0.0, 200.0, 1, rollLo));
        rollLo->setPlaceholderText(QStringLiteral("8"));
        rollLo->setToolTip(QStringLiteral(
            "The camera speed where the strength starts to fall (8 is about a brisk pan). In pixels of a "
            "2160-line picture; scaled for other sizes.")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        auto* rollHi = addText(QStringLiteral("Floor reached at (px per frame)"), QStringLiteral("dejudder_roll_hi"));
        rollHi->setValidator(new QDoubleValidator(0.0, 400.0, 1, rollHi));
        rollHi->setPlaceholderText(QStringLiteral("24"));
        rollHi->setToolTip(QStringLiteral(
            "The camera speed where the strength reaches the floor (24 is about a whip pan). In pixels of a "
            "2160-line picture; scaled for other sizes.")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        auto* rollFloor = addText(QStringLiteral("Floor (share of the strength)"), QStringLiteral("dejudder_roll_floor"));
        rollFloor->setValidator(new QDoubleValidator(0.0, 1.0, 2, rollFloor));
        rollFloor->setPlaceholderText(QStringLiteral("0.25"));
        rollFloor->setToolTip(QStringLiteral("What is left of the strength on the fastest motion (0.25 = a quarter)."));
        auto* objectStrength = addText(QStringLiteral("Smoothing of motion within the frame"), QStringLiteral("dejudder_object_strength"));
        objectStrength->setValidator(new QDoubleValidator(0.0, 1.0, 2, objectStrength));
        objectStrength->setPlaceholderText(QStringLiteral("1 (off)"));
        objectStrength->setToolTip(QStringLiteral(
            "The camera's motion (pans, tracking) is always smoothed fully; this scales the smoothing of things moving "
            "within the frame. 1 = everything smoothed alike (off); 0.5 = people and objects smoothed half as much, which "
            "reads less like video; 0 = the camera only."));
        {
            // The five positions (docs/VP-motion-slider.md §3), the keys they move, the defaults
            // an unset field means. Strings as the fields hold them.
            struct MotionPosition { const char* name; const char* cost; const char* description; std::array<const char*, 8> values; };
            static const std::array<const char*, 8> kKeys = { "dejudder_strength", "dejudder_strength_even", "dejudder_even_rates",
                "dejudder_object_strength", "dejudder_motion_rolloff", "dejudder_roll_floor", "dejudder_roll_lo", "dejudder_roll_hi" };
            static const std::array<const char*, 8> kDefaults = { "0", "0", "whole", "1", "off", "0.25", "8", "24" };
            static const std::array<MotionPosition, 6> kPositions = { {
                { "As shot", "", "No motion processing. Frames shown exactly as the source provides them, with the cadence the "
                    "display imposes (3:2 at 24p).", { "0", "0", "whole", "1", "off", "0.25", "8", "24" } },
                { "Film", " (GPU +)", "Eases the 3:2 hitch a little (the in-between pictures moved 15% of the way to even motion); "
                    "people and fast action keep most of their film motion.", { "0.15", "0", "whole", "0.4", "on", "0.15", "6", "18" } },
                { "Cinema (default)", " (GPU +)", "Eases the 3:2 hitch on pans (a quarter of the way to even motion; the earlier "
                    "Gentle). People keep about half their 24p character; fast action keeps its strobing.",
                    { "0.25", "0", "whole", "0.5", "on", "0.25", "8", "24" } },
                { "Natural", " (GPU ++)", "Smoother: film 40% of the way to even motion, 30p material (documentaries) halfway "
                    "to 60p, people smoothed most of the way. Fast action still partly strobes.",
                    { "0.4", "0.5", "whole_and_1001", "0.7", "on", "0.5", "12", "36" } },
                { "Smooth", " (GPU ++)", "Film two-thirds of the way to even motion, 30p 80% of the way to 60p, people nearly as "
                    "smooth as the camera; fast action strobes only a little. Clearly smoother than film.",
                    { "0.65", "0.8", "whole_and_1001", "0.85", "on", "0.75", "16", "48" } },
                { "Max", " (GPU ++)", "Full strength: the in-between pictures at their true times, 30p fully doubled to 60p, "
                    "everything smoothed alike, no strobing kept. The \"video\" look - for sport and live events, wrong for "
                    "films - and where any motion error shows most.", { "1", "1", "whole_and_1001", "1", "off", "1", "16", "48" } } } };
            const std::array<QLineEdit*, 8> texts = { dejudderStrength, strengthEven, nullptr, objectStrength, nullptr, rollFloor, rollLo, rollHi };
            const std::array<QComboBox*, 8> choices = { nullptr, nullptr, evenRates, nullptr, rolloff, nullptr, nullptr, nullptr };
            auto applying = std::make_shared<bool>(false);
            auto raw = [texts, choices](int i) {
                QString value = texts[i] ? texts[i]->text().trimmed() : choices[i]->currentData().toString().trimmed();
                if (value.compare(QStringLiteral("Auto"), Qt::CaseInsensitive) == 0) value.clear();   // the default profile's unset field
                // Hunt 3 (CFG3): a retired value counts as the value it maps to.
                const QString retired = retiredChoiceTarget(QString::fromLatin1(kKeys[i]), value);
                if (!retired.isEmpty()) value = retired;
                return value;
            };
            auto matches = [raw](int p) {
                for (int i = 0; i < 8; ++i)
                {
                    QString value = raw(i);
                    if (value.isEmpty()) value = QString::fromLatin1(kDefaults[i]);
                    const QString want = QString::fromLatin1(kPositions[p].values[i]);
                    bool okA = false, okB = false;
                    const double a = value.toDouble(&okA), b = want.toDouble(&okB);
                    if (okA && okB ? std::fabs(a - b) > 1e-9 : value.compare(want, Qt::CaseInsensitive) != 0)
                        return false;
                }
                return true;
            };
            auto refresh = [=]() {
                if (*applying) return;
                bool anySet = false;
                for (int i = 0; i < 8; ++i) anySet = anySet || !raw(i).isEmpty();
                if (!anySet)
                {
                    // Audit 2026-10-16: the handle moves to the built-in level too (it kept the previous
                    // profile's position); releasing it on that position writes the level.
                    for (int p = 0; p < 6; ++p)
                        if (matches(p))
                        {
                            const QSignalBlocker blocker(motionSlider);
                            motionSlider->setValue(p);
                            break;
                        }
                    motionName->setText(QStringLiteral("Not set (the default profile's, or As shot)"));
                    motionDescription->setText(QStringLiteral("Move the slider to choose this profile's level."));
                    return;
                }
                for (int p = 0; p < 6; ++p)
                    if (matches(p))
                    {
                        const QSignalBlocker blocker(motionSlider);
                        motionSlider->setValue(p);
                        motionName->setText(QString::fromLatin1(kPositions[p].name) + QString::fromLatin1(kPositions[p].cost));
                        motionDescription->setText(QString::fromLatin1(kPositions[p].description));
                        return;
                    }
                motionName->setText(QStringLiteral("Custom"));
                motionDescription->setText(QStringLiteral("The fields below differ from %1. Reset puts them back.")
                    .arg(QString::fromLatin1(kPositions[motionSlider->value()].name)));
            };
            auto apply = [=](int p) {
                *applying = true;
                for (int i = 0; i < 8; ++i)
                {
                    const QString value = QString::fromLatin1(kPositions[p].values[i]);
                    if (texts[i] && texts[i]->text() != value) texts[i]->setText(value);
                    if (choices[i])
                    {
                        const int index = choices[i]->findData(value);
                        if (index >= 0 && choices[i]->currentIndex() != index) choices[i]->setCurrentIndex(index);
                    }
                }
                *applying = false;
                refresh();
            };
            QObject::connect(motionSlider, &QSlider::valueChanged, receiver, [apply](int p) { apply(p); });
            // Not set: the handle already on the chosen position emits no valueChanged; the release writes it.
            QObject::connect(motionSlider, &QSlider::sliderReleased, receiver, [apply, raw, motionSlider]() {
                for (int i = 0; i < 8; ++i)
                    if (!raw(i).isEmpty()) return;
                apply(motionSlider->value());
            });
            QObject::connect(motionReset, &QPushButton::clicked, receiver, [apply, motionSlider]() { apply(motionSlider->value()); });
            // The default level (Cinema, position 2): one click back.
            QObject::connect(motionDefault, &QPushButton::clicked, receiver, [apply, motionSlider]() {
                const QSignalBlocker blocker(motionSlider);
                motionSlider->setValue(2);
                apply(2);
            });
            for (int i = 0; i < 8; ++i)
            {
                if (texts[i]) QObject::connect(texts[i], &QLineEdit::textChanged, receiver, [refresh](const QString&) { refresh(); });
                if (choices[i]) QObject::connect(choices[i], qOverload<int>(&QComboBox::currentIndexChanged), receiver, [refresh](int) { refresh(); });
            }
            // Owner 2026-10-16: the freehand boxes - the allowed range beside each, only valid numbers
            // kept (clamped to the range and rounded to the field's precision when the field is left;
            // whole numbers for integer fields), and a reset in each field back to the slider
            // position's value.
            static const std::array<double, 8> kMin = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
            static const std::array<double, 8> kMax = { 1.0, 1.0, 0.0, 1.0, 0.0, 1.0, 200.0, 400.0 };
            static const std::array<int, 8> kDecimals = { 3, 3, 0, 2, 0, 2, 1, 1 };
            static const std::array<const char*, 8> kUnits = { "", "", "", "", "", "", " px", " px" };
            for (int i = 0; i < 8; ++i)
            {
                QLineEdit* edit = texts[i];
                if (!edit) continue;
                const double lo = kMin[i], hi = kMax[i];
                const int decimals = kDecimals[i];
                auto* validator = new QDoubleValidator(lo, hi, decimals, edit);
                validator->setNotation(QDoubleValidator::StandardNotation);
                validator->setLocale(QLocale::c());
                edit->setValidator(validator);
                const QString range = QStringLiteral("%1-%2%3").arg(lo, 0, 'g', 6).arg(hi, 0, 'g', 6).arg(QString::fromLatin1(kUnits[i]));
                edit->setObjectName(edit->objectName());   // unchanged; the range is shown in the row's label
                if (auto* label = qobject_cast<QLabel*>(dejudderTasteForm->labelForField(edit)))
                    label->setText(label->text() + QStringLiteral(" (") + range + QStringLiteral(")"));
                edit->setAccessibleDescription(QStringLiteral("Allowed: %1.").arg(range));
                QObject::connect(edit, &QLineEdit::editingFinished, receiver, [edit, lo, hi, decimals]() {
                    // Leaving an inherited field untouched writes no override (hunt 2, L5).
                    if (edit->property("inherited").toBool()) return;
                    QString text = edit->text().trimmed();
                    if (text.isEmpty() || text.compare(QStringLiteral("Auto"), Qt::CaseInsensitive) == 0)
                        return;   // unset: the default
                    bool ok = false;
                    double value = QLocale::c().toDouble(text, &ok);
                    if (!ok) value = text.toDouble(&ok);
                    if (!ok) { edit->clear(); return; }
                    value = (std::min)(hi, (std::max)(lo, value));
                    const double scale = std::pow(10.0, decimals);
                    value = std::round(value * scale) / scale;
                    const QString clean = QString::number(value, 'g', 12);
                    if (clean != edit->text()) edit->setText(clean);
                });
                // The reset inside the field: this one value back to the slider position's.
                QAction* fieldReset = edit->addAction(edit->style()->standardIcon(QStyle::SP_BrowserReload), QLineEdit::TrailingPosition);
                fieldReset->setObjectName(edit->objectName() + QStringLiteral(".reset"));
                fieldReset->setToolTip(QStringLiteral("Back to the slider position's value"));
                QObject::connect(fieldReset, &QAction::triggered, receiver, [edit, i, motionSlider]() {
                    edit->setText(QString::fromLatin1(kPositions[motionSlider->value()].values[i]));
                });
            }
            // Profile loads block the fields' signals: the load triggers this to re-read them.
            auto* motionRefresh = new QAction(motionSlider);
            motionRefresh->setObjectName(controlName(sectionPrefix, QStringLiteral("motion_preset_refresh")));
            QObject::connect(motionRefresh, &QAction::triggered, receiver, [refresh]() { refresh(); });
            refresh();
        }
        auto* fadeMatch = addChoice(QStringLiteral("Follow motion through fades"),
            QStringLiteral("dejudder_fade_match"), { QStringLiteral("on"), QStringLiteral("off") });
        fadeMatch->setToolTip(QStringLiteral(
            "During a smooth fade (the whole picture brightening or darkening steadily) motion is matched despite the "
            "brightness change, and the de-judder runs at the fade floor below. A flash or a cut still holds."));
        auto* fadeFloor = addText(QStringLiteral("Fade floor (share of the strength)"), QStringLiteral("dejudder_fade_floor"));
        fadeFloor->setValidator(new QDoubleValidator(0.0, 1.0, 2, fadeFloor));
        fadeFloor->setPlaceholderText(QStringLiteral("0.25"));
        fadeFloor->setToolTip(QStringLiteral("The most de-judder a fading scene gets (0.25 = a quarter)."));
        MarkGpuCost(form, motionBox, 2);

        form = addCollapsibleSection(QStringLiteral("dejudderEstimation"),
            QStringLiteral("De-judder: motion estimation (measured - leave alone)"),
            QStringLiteral("How the de-judder finds and checks motion. The defaults come from the measurements on real "
                "footage; change them only to compare. ") + QString::fromLatin1(kGpuCostLegend), false);
        QFormLayout* dejudderEstimationForm = form;
        auto* dejudderTolerance = addText(QStringLiteral("Match tolerance (0.5 to 16)"),
            QStringLiteral("dejudder_match_tolerance"), QStringLiteral("codes"));
        dejudderTolerance->setValidator(new QDoubleValidator(0.5, 16.0, 2, dejudderTolerance));
        dejudderTolerance->setPlaceholderText(QStringLiteral("4.5"));
        dejudderTolerance->setToolTip(QStringLiteral(
            "Mean luma difference (8-bit codes) up to which a block's motion is trusted. "
            "Lower is stricter (more blocks held still)."));
        auto* dejudderFrameTrust = addText(QStringLiteral("Cut threshold (0 to 1)"),
            QStringLiteral("dejudder_frame_trust"));
        dejudderFrameTrust->setValidator(new QDoubleValidator(0.0, 1.0, 3, dejudderFrameTrust));
        dejudderFrameTrust->setPlaceholderText(QStringLiteral("0.25"));
        dejudderFrameTrust->setToolTip(QStringLiteral(
            "Below this share of trusted blocks the frame is treated as a cut and held still."));
        // Refinements (docs/VP-build-plan-refinements.md).
        auto* globalPrior = addChoice(QStringLiteral("Global-motion prior"),
            QStringLiteral("dejudder_global_prior"), { QStringLiteral("on"), QStringLiteral("off") });
        globalPrior->setToolTip(QStringLiteral(
            "Estimates the whole picture's movement first and starts every block's search there, so "
            "fast pans (up to about a quarter of the picture height per frame) are still followed. Default on."));
        auto* estGate = addChoice(QStringLiteral("Skip the search on still scenes"),
            QStringLiteral("dejudder_est_gate"), { QStringLiteral("on"), QStringLiteral("off") });
        estGate->setToolTip(QStringLiteral(
            "When nothing moves (needs the global-motion prior), last frame's motion is kept instead of searching again."));
        auto* vectorSmooth = addChoice(QStringLiteral("Vector smoothing"),
            QStringLiteral("dejudder_vector_smooth"), { QStringLiteral("on"), QStringLiteral("off") });
        vectorSmooth->setToolTip(QStringLiteral(
            "Replaces a block's motion that disagrees with all its neighbours by theirs, and lets a "
            "held block move with agreeing neighbours when its own picture matches there. Default on."));
        auto* motionLo = addText(QStringLiteral("Motion scale from (pixels/frame)"),
            QStringLiteral("dejudder_motion_scale_lo"));
        motionLo->setValidator(new QDoubleValidator(0.0, 16.0, 2, motionLo));
        motionLo->setPlaceholderText(QStringLiteral("0.25"));
        motionLo->setToolTip(QStringLiteral(
            "Below this average movement per film frame the de-judder does nothing: near-still scenes have no "
            "judder to fix. In pixels of a 2160-line picture; scaled for other sizes.")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        auto* motionHi = addText(QStringLiteral("Full strength at (pixels/frame)"),
            QStringLiteral("dejudder_motion_scale_hi"));
        motionHi->setValidator(new QDoubleValidator(0.0, 32.0, 2, motionHi));
        motionHi->setPlaceholderText(QStringLiteral("1.5"));
        motionHi->setToolTip(QStringLiteral(
            "The average movement from which the full strength applies; in between it ramps up. "
            "Lower it if slow pans still hitch. 0 turns the motion scaling off."));
        auto* trustScale = addText(QStringLiteral("Full strength at trusted share (0 to 1)"),
            QStringLiteral("dejudder_trust_scale"));
        trustScale->setValidator(new QDoubleValidator(0.0, 1.0, 3, trustScale));
        trustScale->setPlaceholderText(QStringLiteral("0.6"));
        trustScale->setToolTip(QStringLiteral(
            "When fewer blocks than this share are trusted, the strength eases down for that frame "
            "instead of holding more of it. 0 turns it off."));
        // Stage 11a, estimator-lite.
        auto* estCandidates = addChoice(QStringLiteral("Neighbour and temporal candidates"),
            QStringLiteral("dejudder_est_candidates"), { QStringLiteral("on"), QStringLiteral("off") });
        estCandidates->setToolTip(QStringLiteral(
            "A second search pass also tries the neighbours' motion and last frame's, and prefers motion that agrees "
            "with the neighbours unless another match is clearly better: fewer wrong vectors on repeating texture."));
        auto* estLambda = addText(QStringLiteral("Neighbour agreement weight (0 to 8)"), QStringLiteral("dejudder_est_lambda"));
        estLambda->setValidator(new QDoubleValidator(0.0, 8.0, 2, estLambda));
        estLambda->setPlaceholderText(QStringLiteral("0.5"));
        estLambda->setToolTip(QStringLiteral(
            "How much a match must beat the neighbours' motion to win (codes per source pixel of disagreement). 0 = best match only."));
        auto* estQpel = addChoice(QStringLiteral("Quarter-pixel refinement"),
            QStringLiteral("dejudder_est_qpel"), { QStringLiteral("on"), QStringLiteral("off") });
        estQpel->setToolTip(QStringLiteral(
            "Refines trusted motion to a quarter of a source pixel: sharper in-between frames on slow pans, "
            "no stepping."));
        auto* estStillZero = addChoice(QStringLiteral("Identical blocks are still"),
            QStringLiteral("dejudder_est_still_zero"), { QStringLiteral("on"), QStringLiteral("off") });
        estStillZero->setToolTip(QStringLiteral(
            "A block that is pixel-for-pixel the same in both frames gets exactly no motion, so a repeated or "
            "static picture is shown unchanged between frames (off: the search's own small vectors)."));
        auto* estSplit = addChoice(QStringLiteral("Split poorly matched blocks"),
            QStringLiteral("dejudder_est_split"), { QStringLiteral("on"), QStringLiteral("off") });
        estSplit->setToolTip(QStringLiteral(
            "A block that matches poorly (often a small object on a moving background) is searched as four "
            "quarter blocks, so the object and its surround each get their own motion."));
        auto* estSplitSad = addText(QStringLiteral("Split above (codes, 0 = match tolerance)"),
            QStringLiteral("dejudder_est_split_sad"));
        estSplitSad->setValidator(new QDoubleValidator(0.0, 16.0, 2, estSplitSad));
        estSplitSad->setPlaceholderText(QStringLiteral("0"));
        estSplitSad->setToolTip(QStringLiteral(
            "How poorly a block must match (mean luma difference, 8-bit codes) before it is split. Lower splits "
            "more blocks (more GPU time). 0 = the match tolerance above."));
        // Motion fixes (owner 2026-10-13 night): how hard the matcher looks
        // and what it accepts.
        auto* searchRange = addText(QStringLiteral("Motion search reach"), QStringLiteral("dejudder_search_range"));
        searchRange->setValidator(new QIntValidator(1, 16, searchRange));
        searchRange->setPlaceholderText(QStringLiteral("8"));
        searchRange->setToolTip(QStringLiteral(
            "How far each block's coarse search looks (8, the default, is about a quarter of the picture height "
            "per frame: fast pans; 3 was the earlier reach). Higher costs a little more time."));
        auto* globalTwo = addChoice(QStringLiteral("Two whole-picture motions"),
            QStringLiteral("dejudder_global_two"), { QStringLiteral("on"), QStringLiteral("off") });
        globalTwo->setToolTip(QStringLiteral(
            "Every block also tries the second strongest whole-picture motion (for example a tracked subject and a fast "
            "background), even when neither is certain; each block's own match decides."));
        auto* textureTol = addText(QStringLiteral("Texture allowance"), QStringLiteral("dejudder_texture_tolerance"));
        textureTol->setValidator(new QDoubleValidator(0.0, 4.0, 2, textureTol));
        textureTol->setPlaceholderText(QStringLiteral("0.25"));
        textureTol->setToolTip(QStringLiteral(
            "Fine detail, grain and water make even a correct match differ a little; this lets a block's allowed "
            "difference grow with its detail (0 = the fixed match tolerance only)."));
        auto* consistency = addText(QStringLiteral("Forward/backward agreement (pixels)"), QStringLiteral("dejudder_consistency_px"));
        consistency->setValidator(new QDoubleValidator(0.5, 8.0, 1, consistency));
        consistency->setPlaceholderText(QStringLiteral("2"));
        consistency->setToolTip(QStringLiteral(
            "A block is trusted only when its motion found forwards and backwards agree within this (half-size pixels)."));
        auto* regional = addChoice(QStringLiteral("Hold only the unmatched areas on hard motion"),
            QStringLiteral("dejudder_regional_hold"), { QStringLiteral("on"), QStringLiteral("off") });
        regional->setToolTip(QStringLiteral(
            "When few blocks match but the picture has not changed (hard motion, not a cut), the matched areas still move "
            "and the rest holds. A cut still holds the whole frame."));
        auto* cutHistogram = addText(QStringLiteral("Cut test (picture change)"), QStringLiteral("dejudder_cut_histogram"));
        cutHistogram->setValidator(new QDoubleValidator(0.0, 2.0, 2, cutHistogram));
        cutHistogram->setPlaceholderText(QStringLiteral("0.3"));
        cutHistogram->setToolTip(QStringLiteral(
            "With the regional hold: how much the picture's brightness distribution must change for a frame to count as a "
            "cut (0 to 2)."));
        auto* pixelTrust = addChoice(QStringLiteral("Decide pixel by pixel"),
            QStringLiteral("dejudder_pixel_trust"), { QStringLiteral("on"), QStringLiteral("off") });
        pixelTrust->setToolTip(QStringLiteral(
            "Instead of moving or holding whole 16-pixel blocks, each pixel moves with the motion of the matched blocks "
            "around it as far as it predicts the next frame; a block that did not match can follow its matched "
            "neighbours. Real footage: about 6 points more of the judder removed at the same damage level."));
        auto* pixelLo = addText(QStringLiteral("Pixel decision: full from (x tolerance)"), QStringLiteral("dejudder_pixel_trust_lo"));
        pixelLo->setValidator(new QDoubleValidator(0.25, 16.0, 2, pixelLo));
        pixelLo->setPlaceholderText(QStringLiteral("3"));
        pixelLo->setToolTip(QStringLiteral(
            "A pixel whose predicted next frame is within this many times the match tolerance moves fully."));
        auto* pixelHi = addText(QStringLiteral("Pixel decision: held from (x tolerance)"), QStringLiteral("dejudder_pixel_trust_hi"));
        pixelHi->setValidator(new QDoubleValidator(0.5, 32.0, 2, pixelHi));
        pixelHi->setPlaceholderText(QStringLiteral("8"));
        pixelHi->setToolTip(QStringLiteral(
            "From this many times the match tolerance a pixel holds still; in between it moves partly. Keep it above "
            "the value before."));
        auto* landing = addChoice(QStringLiteral("Check each moved pixel lands where it is drawn"),
            QStringLiteral("dejudder_landing_check"), { QStringLiteral("on"), QStringLiteral("off") });
        landing->setToolTip(QStringLiteral(
            "With pixel-by-pixel decisions: a pixel taken from the film frame is used only if its own motion really "
            "carries it to where it is being drawn. Where something moves in front of something else, the matched "
            "pixel can be a valid one that is not the one visible there; it then stays still. On real footage this "
            "cut the damaged patches to a third for about half a point less judder removed. Default on."));
        auto* verify = addChoice(QStringLiteral("Check matches pixel by pixel"),
            QStringLiteral("dejudder_verify"), { QStringLiteral("on"), QStringLiteral("off") });
        verify->setToolTip(QStringLiteral(
            "Each matched block predicts the next frame pixel by pixel with the motion the de-judder will use; a block "
            "whose pixels do not follow (an edge, something uncovered) holds instead, so looser matching stays safe."));
        auto* verifyShare = addText(QStringLiteral("Pixel check: hold above this share wrong"), QStringLiteral("dejudder_verify_share"));
        verifyShare->setValidator(new QDoubleValidator(0.02, 1.0, 2, verifyShare));
        verifyShare->setPlaceholderText(QStringLiteral("0.2"));
        verifyShare->setToolTip(QStringLiteral("A block holds when more than this share of its pixels mispredict (0.2 = a fifth)."));
        auto* lowRes = addChoice(QStringLiteral("Motion estimation size (tall sources)"),
            QStringLiteral("dejudder_low_res"), { QStringLiteral("all"), QStringLiteral("search"), QStringLiteral("off") });
        lowRes->setItemText(lowRes->findData(QStringLiteral("all")), QStringLiteral("Half size, every step (recommended)"));
        lowRes->setItemText(lowRes->findData(QStringLiteral("search")), QStringLiteral("Half size search, full-size refinement"));
        lowRes->setItemText(lowRes->findData(QStringLiteral("off")), QStringLiteral("Full size"));
        lowRes->setToolTip(QStringLiteral(
            "For sources about 1530 lines and taller, how finely motion is estimated (halved by height, again for "
            "the tallest). On real footage, half size for every step kept more de-judder benefit than full size "
            "(108%; averaging the grain helps the matching) at about half the GPU time. Half-size search with "
            "full-size refinement keeps quarter-pixel precision and 8-pixel splits (95% of the benefit). The "
            "occlusion-fill experiment is skipped at half size. Shorter sources always use full size."));
        auto* estimateAhead = addChoice(QStringLiteral("Estimate motion ahead"),
            QStringLiteral("dejudder_estimate_ahead"), { QStringLiteral("on"), QStringLiteral("off") });
        estimateAhead->setToolTip(QStringLiteral(
            "When the frame after next is already queued, the motion for the next frame is worked out in the "
            "spare time after a de-judder extra, so the next frame's own refresh only has its render to do. "
            "Without that frame (a short queue) it is worked out before the frame, as before. Needs a queue "
            "target of 3 frames or more to take effect; the stats line shows the share done ahead."));
        // Delta 9, synthesis-lite.
        auto* feather = addText(QStringLiteral("Soften held-block edges (px, 0 = off)"),
            QStringLiteral("dejudder_hold_feather_px"));
        feather->setValidator(new QDoubleValidator(0.0, 16.0, 1, feather));
        feather->setPlaceholderText(QStringLiteral("4"));
        feather->setToolTip(QStringLiteral(
            "Where a moved block meets a held one, the moved picture fades to the unmoved one over this many pixels, "
            "so the hold leaves no visible line. Not frame blending: both sides are the same frame."));
        auto* hysteresis = addText(QStringLiteral("Frames before a block changes (1 = off)"),
            QStringLiteral("dejudder_hold_hysteresis"));
        hysteresis->setValidator(new QIntValidator(1, 8, hysteresis));
        hysteresis->setPlaceholderText(QStringLiteral("2"));
        hysteresis->setToolTip(QStringLiteral(
            "A block switches between moved and held only after this many film frames in a row say so (counted at "
            "23.976p; scaled to the same time at other rates), so it does not flicker between the two. A cut holds "
            "everything at once, and the first film frame after a start or a cut stays held while the blocks earn "
            "their trust."));
        MarkGpuCost(form, globalPrior, 1);
        MarkGpuCost(form, vectorSmooth, 1);
        MarkGpuCost(form, estCandidates, 1);
        MarkGpuCost(form, estQpel, 1);
        MarkGpuCost(form, estStillZero, 1);
        MarkGpuCost(form, estSplit, 1);
        MarkGpuCost(form, searchRange, 1);
        MarkGpuCost(form, globalTwo, 1);
        MarkGpuCost(form, pixelTrust, 1);
        MarkGpuCost(form, verify, 1);

        // Owner 2026-10-14: fixes that change existing behaviour, each on by
        // default and switchable back while they are proven on the projector.
        form = addCollapsibleSection(QStringLiteral("fixesUnderTest"), QStringLiteral("Fixes under test"),
            QStringLiteral("Recent fixes, on by default. Switch one off to get the old behaviour back if something "
                "looks wrong."), false);
        // Pruning 2026-10-17: "Ignore black bars when matching" (dejudder_picture_area), "Scene-cut
        // cooldown 2 s" (fix_scene_cooldown) and "Dither reuses de-judder motion on new frames"
        // (dither_vectors_new_frame) were retired - always on; old files keep loading
        // (RetiredSettings.h).
        auto* capturePath = addChoice(QStringLiteral("Capture unpack on the GPU (v210 / UYVY)"), QStringLiteral("capture_path"),
            { QStringLiteral("gpu"), QStringLiteral("standard") });
        capturePath->setItemText(capturePath->findData(QStringLiteral("gpu")), QStringLiteral("GPU (default)"));
        capturePath->setItemText(capturePath->findData(QStringLiteral("standard")), QStringLiteral("CPU (the old path)"));
        capturePath->setToolTip(QStringLiteral(
            "DeckLink v210 / UYVY frames are uploaded as they arrive and unpacked on the GPU: the same picture byte for "
            "byte, about half the time at 4K and no CPU conversion. CPU = the old conversion. RGB formats follow the "
            "'Native 10/12-bit RGB capture' choice (in Fixes under test)."));   // audit 2026-10-16: was "below"
        // The default is Auto: early upload in every mode, guarded by free video
        // memory (efficiency fixes off: the low-latency path only), so the label
        // no longer says experimental.
        auto* captureUpload = addChoice(QStringLiteral("Upload frames as they arrive"), QStringLiteral("capture_upload"),
            { QStringLiteral("auto"), QStringLiteral("render"), QStringLiteral("early") });
        captureUpload->setItemText(captureUpload->findData(QStringLiteral("auto")), QStringLiteral("Auto (early in every mode)"));
        captureUpload->setItemText(captureUpload->findData(QStringLiteral("render")), QStringLiteral("Off (the render thread uploads)"));
        captureUpload->setItemText(captureUpload->findData(QStringLiteral("early")), QStringLiteral("On (early, second device)"));
        captureUpload->setToolTip(QStringLiteral(
            "Auto (default): early in every mode, guarded by free video memory (the render upload, shown under Fallbacks "
            "engaged, when memory is short); with efficiency fixes off, only in the low-latency path. On: each captured frame is copied to the "
            "GPU on its own thread as it arrives, instead of by the render thread when it is shown: about 5 ms less work "
            "for the render thread per 4K 12-bit frame, and the copy overlaps the previous frame's rendering. A frame "
            "with no free upload slot simply takes the normal path. Off: the render thread uploads every frame."));
        auto* sourceRange = addChoice(QStringLiteral("Source range"), QStringLiteral("source_range"),
            { QStringLiteral("auto"), QStringLiteral("full"), QStringLiteral("limited") });
        sourceRange->setItemText(sourceRange->findData(QStringLiteral("auto")), QStringLiteral("Auto (from the capture format)"));
        sourceRange->setItemText(sourceRange->findData(QStringLiteral("full")), QStringLiteral("Full (0-255)"));
        sourceRange->setItemText(sourceRange->findData(QStringLiteral("limited")), QStringLiteral("Limited (16-235)"));
        sourceRange->setToolTip(QStringLiteral(
            "The range the source sends. Auto: YCbCr and 10-bit RGB are limited, 8- and 12-bit RGB full - right for "
            "most sources. If blacks look grey and whites dull (or blacks crush), the source sends the other range: "
            "set it here. The picture and the LED dimming both follow it."));
        // Owner 2026-10-16 (docs/VP-settings-sliders): the Picture slider - how readily VP
        // undoes a player's upscale. Off, Cautious, Standard, Fast move only the detection's
        // keys (the fields below, still editable: a changed value shows Custom).
        QFormLayout* pictureForm = form;
        auto* pictureBox = new QWidget;
        pictureBox->setObjectName(controlName(sectionPrefix, QStringLiteral("picture_preset_box")));
        auto* pictureBoxLayout = new QVBoxLayout(pictureBox);
        pictureBoxLayout->setContentsMargins(0, 0, 0, 0);
        auto* pictureRow = new QHBoxLayout;
        auto* pictureSlider = new QSlider(Qt::Horizontal);
        pictureSlider->setObjectName(controlName(sectionPrefix, QStringLiteral("picture_preset")));
        pictureSlider->setAccessibleName(QStringLiteral("Picture: undo the player's upscale"));
        pictureSlider->setRange(0, 3);
        pictureSlider->setPageStep(1);
        pictureSlider->setTickPosition(QSlider::TicksBelow);
        pictureSlider->setTickInterval(1);
        pictureSlider->setToolTip(QStringLiteral(
            "How readily VP detects a stream upscaled before it reaches VP (many 4K streams are 1080p underneath) and "
            "undoes the upscale: Off, Cautious, Standard, Fast. Each step moves only the detection's evidence and "
            "upscale clarity; the values appear in the fields below and can be edited (Custom). Real detail above the "
            "detected size, a seek or a source change always returns to native at once. Subtitles the player draws "
            "itself are softened (to about the detected size) when a stream is re-based; they stay legible."));
        auto* pictureReset = new QPushButton(QStringLiteral("Reset"))   /* one word for every slider's reset (organisation audit 2026-10-24) */;
        pictureReset->setObjectName(controlName(sectionPrefix, QStringLiteral("picture_preset_reset")));
        pictureReset->setToolTip(QStringLiteral("Puts the fields below back to the slider position's values."));
        auto* pictureDefault = new QPushButton(QStringLiteral("Default"));
        pictureDefault->setObjectName(controlName(sectionPrefix, QStringLiteral("picture_preset_default")));
        pictureDefault->setToolTip(QStringLiteral("Back to the default, Standard (Auto: 3 shots and 10 s agreeing, upscale clarity 0.8)."));
        pictureRow->addWidget(new QLabel(QStringLiteral("Off")));
        pictureRow->addWidget(pictureSlider, 1);
        pictureRow->addWidget(new QLabel(QStringLiteral("Fast")));
        pictureRow->addWidget(pictureReset);
        pictureRow->addWidget(pictureDefault);
        pictureBoxLayout->addLayout(pictureRow);
        auto* pictureName = new QLabel;
        pictureName->setObjectName(controlName(sectionPrefix, QStringLiteral("picture_preset_name")));
        QFont pictureNameFont = pictureName->font();
        pictureNameFont.setBold(true);
        pictureName->setFont(pictureNameFont);
        auto* pictureDescription = new QLabel;
        pictureDescription->setObjectName(controlName(sectionPrefix, QStringLiteral("picture_preset_description")));
        pictureDescription->setWordWrap(true);
        pictureBoxLayout->addWidget(pictureName);
        pictureBoxLayout->addWidget(pictureDescription);
        form->addRow(QStringLiteral("Picture"), pictureBox);
        auto* sourceResolution = addChoice(QStringLiteral("Source resolution"), QStringLiteral("source_resolution"),
            { QStringLiteral("native"), QStringLiteral("1440"), QStringLiteral("1080"), QStringLiteral("720"),
              QStringLiteral("auto") });
        sourceResolution->setItemText(sourceResolution->findData(QStringLiteral("native")), QStringLiteral("Native (as captured)"));
        sourceResolution->setItemText(sourceResolution->findData(QStringLiteral("1440")), QStringLiteral("1440 lines")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        sourceResolution->setItemText(sourceResolution->findData(QStringLiteral("1080")), QStringLiteral("1080 lines")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        sourceResolution->setItemText(sourceResolution->findData(QStringLiteral("720")), QStringLiteral("720 lines")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        sourceResolution->setItemText(sourceResolution->findData(QStringLiteral("auto")), QStringLiteral("Auto (detected)"));
        sourceResolution->setToolTip(QStringLiteral(
            "For a stream upscaled before it reaches VP (many 4K streams are 1080p underneath): VP shrinks the picture "
            "back to that size, then upscales it with its own path, as for a real 1080p source - fewer halos, cleaner "
            "edges. Native = as captured. A size = always that size (a profile for such a source). "
            "Auto (the default) = VP detects the size the stream was upscaled from: it changes only after several shots agree "
            "(the evidence below; standard 3 shots and 10 s), and only at a cut; any real detail above that size, a "
            "seek or a source change returns it to native at once. The stats line shows what it reads. A film finished at "
            "1080p or 2K and encoded as a 4K stream reads native (the 4K encoding removes the upscale's trace)."));
        auto* autoConfidence = addText(QStringLiteral("Auto: upscale clarity needed"), QStringLiteral("source_resolution_auto_min_confidence"));
        autoConfidence->setValidator(new QDoubleValidator(0.0, 1.0, 2, autoConfidence));
        autoConfidence->setPlaceholderText(QStringLiteral("0.8"));
        autoConfidence->setToolTip(QStringLiteral(
            "Not a probability: a 0-1 score of how clearly the frames carry an upscale's fingerprint, averaged over "
            "the last two minutes (real 4K scores about 0, real upscales 1). It is checked only after every frame in "
            "that window has read lower and the shots and time below agree; any frame with real detail above the size "
            "blocks or cancels it at once. Higher = slower to act. Used only with Source resolution: Auto."));
        auto* evidenceSeconds = addText(QStringLiteral("Auto: evidence time"), QStringLiteral("source_resolution_evidence_seconds"));
        evidenceSeconds->setPlaceholderText(QStringLiteral("10"));
        evidenceSeconds->setToolTip(QStringLiteral(
            "Seconds of agreeing frames before Auto shrinks the picture (a source upscaled in one direction only "
            "needs twice this). Used only with Source resolution: Auto."));
        auto* evidenceShots = addText(QStringLiteral("Auto: shots that must agree"), QStringLiteral("source_resolution_evidence_shots"));
        evidenceShots->setPlaceholderText(QStringLiteral("3"));
        evidenceShots->setToolTip(QStringLiteral(
            "Separate shots (between cuts) that must agree: one soft shot of a real 4K film never decides alone "
            "(a one-direction upscale needs twice this). Used only with Source resolution: Auto."));
        // Latch review 2026-10-17 (owner): when a confirmed change is shown (with the latch fixes on).
        auto* engageWait = addText(QStringLiteral("Auto: longest wait for a cut"), QStringLiteral("source_resolution_engage_max_wait_s"));
        engageWait->setValidator(new QDoubleValidator(0.0, 600.0, 2, engageWait));
        engageWait->setPlaceholderText(QStringLiteral("20"));
        engageWait->setToolTip(QStringLiteral(
            "Seconds (0-600). Once Auto is sure, it switches at the next cut, real black, fast pan or very dark / "
            "detail-free moment, where the change cannot be seen. If none comes within this time (a long take), it "
            "crossfades instead. Used with Source resolution: Auto and the latch fixes on."));
        auto* crossfade = addText(QStringLiteral("Auto: crossfade time"), QStringLiteral("source_resolution_crossfade_s"));
        crossfade->setValidator(new QDoubleValidator(0.0, 10.0, 2, crossfade));
        crossfade->setPlaceholderText(QStringLiteral("1.5"));
        crossfade->setToolTip(QStringLiteral(
            "Seconds (0-10) of the crossfade a long take gets: both versions of the picture are drawn and mixed. "
            "0 = switch at once."));
        auto* releaseFade = addText(QStringLiteral("Auto: fade back to native"), QStringLiteral("source_resolution_release_fade_s"));
        releaseFade->setValidator(new QDoubleValidator(0.0, 0.25, 2, releaseFade));
        releaseFade->setPlaceholderText(QStringLiteral("0.25"));
        releaseFade->setToolTip(QStringLiteral(
            "Seconds (0-0.25). Returning to native is immediate when real detail appears; this short fade only "
            "avoids a visible snap. 0 = a hard switch."));
        {
            // The four positions, the keys they move, the defaults an unset field means.
            struct PicturePosition { const char* name; const char* description; std::array<const char*, 4> values; };
            static const std::array<const char*, 4> kDefaults = { "auto", "0.8", "10", "3" };
            static const std::array<PicturePosition, 4> kPositions = { {
                { "Off", "The picture as captured: no detection acts on it.", { "native", "0.8", "10", "3" } },
                { "Cautious", "Acts only on clear evidence: 4 shots and 20 s agreeing, upscale clarity 0.9. Slowest to act, "
                    "never on a doubtful source.", { "auto", "0.9", "20", "4" } },
                { "Standard (default)", "3 shots and 10 s agreeing, upscale clarity 0.8.", { "auto", "0.8", "10", "3" } },
                { "Fast", "2 shots and 5 s agreeing, upscale clarity 0.7: quickest to act, more likely to act on a soft "
                    "native film for a while.", { "auto", "0.7", "5", "2" } } } };
            const std::array<QLineEdit*, 4> texts = { nullptr, autoConfidence, evidenceSeconds, evidenceShots };
            const std::array<QComboBox*, 4> choices = { sourceResolution, nullptr, nullptr, nullptr };
            auto applying = std::make_shared<bool>(false);
            auto raw = [texts, choices](int i) {
                QString value = texts[i] ? texts[i]->text().trimmed() : choices[i]->currentData().toString().trimmed();
                if (value.compare(QStringLiteral("Auto"), Qt::CaseInsensitive) == 0 && texts[i]) value.clear();
                return value;
            };
            // This profile's own value: text inherited from the default profile is
            // not set here (hunt 2, L5). raw() still reads it for the handle's level.
            auto own = [texts, raw](int i) {
                return texts[i] && texts[i]->property("inherited").toBool() ? QString() : raw(i);
            };
            auto matches = [raw](int p) {
                // Off is the source size alone (the evidence does not apply).
                const int keys = p == 0 ? 1 : 4;
                for (int i = 0; i < keys; ++i)
                {
                    QString value = raw(i);
                    if (value.isEmpty()) value = QString::fromLatin1(kDefaults[i]);
                    const QString want = QString::fromLatin1(kPositions[p].values[i]);
                    bool okA = false, okB = false;
                    const double a = value.toDouble(&okA), b = want.toDouble(&okB);
                    if (okA && okB ? std::fabs(a - b) > 1e-9 : value.compare(want, Qt::CaseInsensitive) != 0)
                        return false;
                }
                return true;
            };
            auto refresh = [=]() {
                if (*applying) return;
                bool anySet = false;
                for (int i = 0; i < 4; ++i) anySet = anySet || !own(i).isEmpty();
                if (!anySet)
                {
                    // Audit 2026-10-16: the handle moves to the built-in level too (it kept the previous
                    // profile's position); releasing it on that position writes the level.
                    for (int p = 0; p < 4; ++p)
                        if (matches(p))
                        {
                            const QSignalBlocker blocker(pictureSlider);
                            pictureSlider->setValue(p);
                            break;
                        }
                    pictureName->setText(QStringLiteral("Not set (the default profile's, or Standard)"));
                    pictureDescription->setText(QStringLiteral("Move the slider to choose this profile's level."));
                    return;
                }
                for (int p = 0; p < 4; ++p)
                    if (matches(p))
                    {
                        const QSignalBlocker blocker(pictureSlider);
                        pictureSlider->setValue(p);
                        pictureName->setText(QString::fromLatin1(kPositions[p].name));
                        pictureDescription->setText(QString::fromLatin1(kPositions[p].description));
                        return;
                    }
                pictureName->setText(QStringLiteral("Custom"));
                pictureDescription->setText(QStringLiteral("The fields below differ from %1. Reset puts them back.")
                    .arg(QString::fromLatin1(kPositions[pictureSlider->value()].name)));
            };
            auto apply = [=](int p) {
                *applying = true;
                for (int i = 0; i < 4; ++i)
                {
                    const QString value = QString::fromLatin1(kPositions[p].values[i]);
                    if (texts[i] && texts[i]->text() != value) texts[i]->setText(value);
                    if (choices[i])
                    {
                        const int index = choices[i]->findData(value);
                        if (index >= 0 && choices[i]->currentIndex() != index) choices[i]->setCurrentIndex(index);
                    }
                }
                *applying = false;
                refresh();
            };
            QObject::connect(pictureSlider, &QSlider::valueChanged, receiver, [apply](int p) { apply(p); });
            // Not set: the handle already on the chosen position emits no valueChanged; the release writes it.
            QObject::connect(pictureSlider, &QSlider::sliderReleased, receiver, [apply, own, pictureSlider]() {
                for (int i = 0; i < 4; ++i)
                    if (!own(i).isEmpty()) return;
                apply(pictureSlider->value());
            });
            QObject::connect(pictureReset, &QPushButton::clicked, receiver, [apply, pictureSlider]() { apply(pictureSlider->value()); });
            // The default level (Standard, position 2): one click back.
            QObject::connect(pictureDefault, &QPushButton::clicked, receiver, [apply, pictureSlider]() {
                const QSignalBlocker blocker(pictureSlider);
                pictureSlider->setValue(2);
                apply(2);
            });
            for (int i = 0; i < 4; ++i)
            {
                if (texts[i]) QObject::connect(texts[i], &QLineEdit::textChanged, receiver, [refresh](const QString&) { refresh(); });
                if (choices[i]) QObject::connect(choices[i], qOverload<int>(&QComboBox::currentIndexChanged), receiver, [refresh](int) { refresh(); });
            }
            // The freehand boxes: the allowed range beside each, only valid numbers kept (clamped
            // and rounded when the field is left; whole shots), and a reset in each field back to
            // the slider position's value.
            static const std::array<double, 4> kMin = { 0.0, 0.0, 2.0, 1.0 };
            static const std::array<double, 4> kMax = { 0.0, 1.0, 120.0, 10.0 };
            static const std::array<int, 4> kDecimals = { 0, 2, 1, 0 };
            static const std::array<const char*, 4> kUnits = { "", "", " s", "" };
            for (int i = 0; i < 4; ++i)
            {
                QLineEdit* edit = texts[i];
                if (!edit) continue;
                const double lo = kMin[i], hi = kMax[i];
                const int decimals = kDecimals[i];
                auto* validator = new QDoubleValidator(lo, hi, decimals, edit);
                validator->setNotation(QDoubleValidator::StandardNotation);
                validator->setLocale(QLocale::c());
                edit->setValidator(validator);
                const QString range = QStringLiteral("%1-%2%3").arg(lo, 0, 'g', 6).arg(hi, 0, 'g', 6).arg(QString::fromLatin1(kUnits[i]));
                if (auto* label = qobject_cast<QLabel*>(pictureForm->labelForField(edit)))
                    label->setText(label->text() + QStringLiteral(" (") + range + QStringLiteral(")"));
                edit->setAccessibleDescription(QStringLiteral("Allowed: %1.").arg(range));
                QObject::connect(edit, &QLineEdit::editingFinished, receiver, [edit, lo, hi, decimals]() {
                    // Leaving an inherited field untouched writes no override (hunt 2, L5).
                    if (edit->property("inherited").toBool()) return;
                    QString text = edit->text().trimmed();
                    if (text.isEmpty() || text.compare(QStringLiteral("Auto"), Qt::CaseInsensitive) == 0)
                        return;   // unset: the default
                    bool ok = false;
                    double value = QLocale::c().toDouble(text, &ok);
                    if (!ok) value = text.toDouble(&ok);
                    if (!ok) { edit->clear(); return; }
                    value = (std::min)(hi, (std::max)(lo, value));
                    const double scale = std::pow(10.0, decimals);
                    value = std::round(value * scale) / scale;
                    const QString clean = QString::number(value, 'g', 12);
                    if (clean != edit->text()) edit->setText(clean);
                });
                QAction* fieldReset = edit->addAction(edit->style()->standardIcon(QStyle::SP_BrowserReload), QLineEdit::TrailingPosition);
                fieldReset->setObjectName(edit->objectName() + QStringLiteral(".reset"));
                fieldReset->setToolTip(QStringLiteral("Back to the slider position's value"));
                QObject::connect(fieldReset, &QAction::triggered, receiver, [edit, i, pictureSlider]() {
                    edit->setText(QString::fromLatin1(kPositions[pictureSlider->value()].values[i]));
                });
            }
            // Profile loads block the fields' signals: the load triggers this to re-read them.
            auto* pictureRefresh = new QAction(pictureSlider);
            pictureRefresh->setObjectName(controlName(sectionPrefix, QStringLiteral("picture_preset_refresh")));
            QObject::connect(pictureRefresh, &QAction::triggered, receiver, [refresh]() { refresh(); });
            refresh();
        }
        auto* debandVp = addChoice(QStringLiteral("VP deband"), QStringLiteral("deband_vp"),
            { QStringLiteral("off"), QStringLiteral("auto"), QStringLiteral("on") });
        debandVp->setItemText(debandVp->findData(QStringLiteral("off")), QStringLiteral("Off"));
        debandVp->setItemText(debandVp->findData(QStringLiteral("auto")), QStringLiteral("Auto (when the source needs it)"));
        debandVp->setItemText(debandVp->findData(QStringLiteral("on")), QStringLiteral("On (always)"));
        debandVp->setToolTip(QStringLiteral(
            "VP's own debander, safe on HDR (libplacebo's leaves black specks there). Smooths the steps in skies and "
            "shadows of 8-bit content and low-bitrate streams, native 4K included, before any scaling; texture and "
            "grain are left alone. Auto: only as strong as the source needs (from its measured bit depth and "
            "bitrate quality - the stats line shows both); a 10-bit clean source is untouched. With the de-judder's "
            "motion it keeps its decisions steady along the motion."));
        auto* lowBitrate = addChoice(QStringLiteral("Low-bitrate handling"), QStringLiteral("low_bitrate_handling"),
            { QStringLiteral("auto"), QStringLiteral("off"), QStringLiteral("on") });
        lowBitrate->setItemText(lowBitrate->findData(QStringLiteral("auto")), QStringLiteral("Auto (measured)"));
        lowBitrate->setItemText(lowBitrate->findData(QStringLiteral("off")), QStringLiteral("Off"));
        lowBitrate->setItemText(lowBitrate->findData(QStringLiteral("on")), QStringLiteral("On (treat as a low-bitrate stream)"));
        lowBitrate->setToolTip(QStringLiteral(
            "How VP deband reacts to compression (blocking, banding). Auto: by the measured bitrate quality. "
            "Off: by the content's bit depth only. On: always as for a poor stream."));
        auto* sharpStabilise = addChoice(QStringLiteral("Steady sharp detail (upscaling)"), QStringLiteral("sharp_residual_stabilise"),
            { QStringLiteral("on"), QStringLiteral("off") });
        sharpStabilise->setToolTip(QStringLiteral(
            "The detail the sharp upscaler branch adds can shimmer on compressed streams. "
            "On: where motion vectors are trusted, that added detail is held along the motion over about "
            "three frames; the alias-safe base is untouched. Without motion vectors it does nothing (same as off)."));
        auto* ssimStabilise = addChoice(QStringLiteral("Steady SSIM detail (downscaling)"), QStringLiteral("ssim_residual_stabilise"),
            { QStringLiteral("on"), QStringLiteral("off") });
        ssimStabilise->setToolTip(QStringLiteral(
            "The same for the SSIM downscaler's added contrast on grain and fine texture: averaged along the trusted "
            "motion so it does not shimmer. Without the motion it does nothing (same as off)."));
        auto* bitrateAware = addChoice(QStringLiteral("Gentler sharpening on poor streams"), QStringLiteral("ssim_bitrate_aware"),
            { QStringLiteral("on"), QStringLiteral("off") });
        bitrateAware->setToolTip(QStringLiteral(
            "On a low-bitrate stream (the measured bitrate quality on the stats line) SSIM and the CNN would sharpen "
            "blocking and mosquito noise as if it were texture: their strength is lowered towards the floor below, and "
            "the faithful scaler's anti-ringing raised. A clean source is untouched."));
        auto* bitrateFloor = addText(QStringLiteral("Poor-stream strength floor"), QStringLiteral("ssim_bitrate_floor"));
        bitrateFloor->setValidator(new QDoubleValidator(0.0, 1.0, 2, bitrateFloor));
        bitrateFloor->setPlaceholderText(QStringLiteral("0.4"));
        bitrateFloor->setToolTip(QStringLiteral("What is left of the SSIM / CNN strength on the poorest stream (0.4 = 40 %)."));
        // docs/VP-nvof-ahead.md (owner 2026-10-16): optical-flow frames ahead, whole frames 0..4.
        auto* kernelMatch = addChoice(QStringLiteral("Re-base: undo the player's upscaler"), QStringLiteral("rebase_kernel_match"),
            { QStringLiteral("on"), QStringLiteral("off") });
        kernelMatch->setToolTip(QStringLiteral(
            "A 1080p stream upscaled 2x by the player (an Apple TV, a set-top box): VP identifies the player's upscale "
            "filter from the picture and undoes it exactly, recovering the decoded 1080p before VP's own scaling "
            "(+8 to +15 dB closer than an ordinary downscale on real streams). Only when the filter is identified "
            "with confidence; otherwise the usual re-base filter. Off: always the usual filter."));
        auto* textGuard = addChoice(QStringLiteral("Source resolution: ignore on-screen text"), QStringLiteral("source_resolution_text_guard"),
            { QStringLiteral("on"), QStringLiteral("off") });
        textGuard->setToolTip(QStringLiteral(
            "A player that upscales 1080p video draws its own subtitles at 4K: sharp text over a soft picture. On "
            "(default): a detection patch whose sharp detail is a band of text is left out of that frame's reading, so "
            "subtitles neither block the detection nor switch an active re-base off. Off: every patch counts."));
        auto* mixedGuard = addChoice(QStringLiteral("Source resolution: never on mixed screens"), QStringLiteral("source_resolution_mixed_guard"),
            { QStringLiteral("on"), QStringLiteral("off") });
        mixedGuard->setToolTip(QStringLiteral(
            "A menu or app screen drawn at 4K with a smaller video playing inside it, a 4K info panel or controls over "
            "the film, or picture-in-picture: the re-base is one size for the whole picture, so it would soften the "
            "sharp parts. On (default): when sharp full-resolution detail shows anywhere beside the upscaled video "
            "(not a subtitle line), the picture counts as native - no re-base, and an active one switches off at "
            "once. Off: the screen is read as a whole, as before."));
        // Hunt 2 M2/M3: the coding readings behind auto deband and low-bitrate handling.
        auto* codingFixes = addChoice(QStringLiteral("Bit depth and coding detection fixes"), QStringLiteral("source_coding_fixes"),
            { QStringLiteral("on"), QStringLiteral("off") });
        codingFixes->setToolTip(QStringLiteral(
            "Fix under test (default on). How VP reads the source's real bit depth and its coding damage, which drive "
            "auto debanding and low-bitrate handling. On: the bit depth is read from the flat parts of the picture and "
            "codes used only by a few pixels count little, so 8-bit video upscaled or dithered into 10 bits reads 8-bit; "
            "block edges are found at any spacing (an upscaled stream's 16- or 24-pixel blocks, not only 8 and 16) and "
            "bands still count with noise or a soft upscaled edge. Off: the earlier readings."));
        // Latch review 2026-10-17 (fix under test, default on).
        auto* latchFixes = addChoice(QStringLiteral("Source resolution: auto latch fixes"), QStringLiteral("source_resolution_latch_fixes"),
            { QStringLiteral("on"), QStringLiteral("off") });
        latchFixes->setToolTip(QStringLiteral(
            "Under test (default on). Auto re-bases again within the evidence standard after a menu or a native shot "
            "(not two minutes later); dark or low-contrast frames, player-added bars and fades no longer block or "
            "fake a change; the switch waits for a cut, a real black, a fast pan or a low-detail moment, else crossfades "
            "(times below); a return to native stays immediate with a short fade. Off: the earlier behaviour."));
        // h3-latch 2026-10-18 (fix under test, default on).
        auto* cutFixes = addChoice(QStringLiteral("Source resolution: auto cut judge fixes"), QStringLiteral("source_resolution_cut_fixes"),
            { QStringLiteral("on"), QStringLiteral("off") });
        cutFixes->setToolTip(QStringLiteral(
            "Under test (default on, with the latch fixes). How auto tells a cut from movement: camera moves, credit "
            "scrolls and a camera passing through a dark object no longer count as cuts (the queued frames show no jump "
            "between two frames), lightning and other short flashes that return to the same picture are not cuts, and "
            "cuts between two similar shots at the same brightness are found. A lower source is also declared only "
            "after part of the evidence time, so a burst of fast cuts on native footage never declares one. Off: the "
            "latch fixes' cut judge."));
        // h3-latch 2026-10-18 (K3, fix under test, default on).
        auto* startupHold = addChoice(QStringLiteral("Black bars: hold the start-up picture until cropped"), QStringLiteral("active_picture_startup_hold"),
            { QStringLiteral("on"), QStringLiteral("off") });
        startupHold->setToolTip(QStringLiteral(
            "Under test (default on). At start-up and after a renderer or queue reset the first crop decision takes "
            "about a second; meanwhile the uncropped picture (bars, subtitles and edges placed for it) used to show. On: "
            "the picture stays black until the first crop decision (at most 2.5 s), or shows the remembered crop at once "
            "when the source is unchanged. Black-bar detection, subtitle placement, A-lens and edge blanking decide as "
            "before; only the output waits. Off: the uncropped picture shows until the crop lands."));
        // Owner 2026-10-16: the engine's options on the motion (de-judder) section, not here.
        QFormLayout* fixesForm = form;
        form = dejudderTasteForm;
        {
            auto* heading = new QLabel(QStringLiteral("<b>Motion source</b>"));
            heading->setObjectName(controlName(sectionPrefix, QStringLiteral("motion_source_heading")));
            form->addRow(heading);
        }
        auto* opticalFlowGlobal = addChoice(QStringLiteral("Camera motion from the engine"), QStringLiteral("optical_flow_global"),
            { QStringLiteral("on"), QStringLiteral("off") });
        opticalFlowGlobal->setToolTip(QStringLiteral(
            "On (default): with the NVIDIA optical-flow engine, the whole picture's motion (the camera's pan or tilt) is "
            "read from the engine's vectors, computed ahead off the render thread, instead of a separate search (about "
            "0.4 ms less). The motion vectors are unchanged, and 'Keep fast motion's strobing' and the smoothing of people "
            "read the motion field's own speed either way. Without the engine the search runs as before. Off: always "
            "the search."));
        // The engine's extras (2026-10-17, docs/VP-nvof-ahead.md).
        auto* opticalFlowFallback = addChoice(QStringLiteral("Camera motion: search when the engine is unclear"),
            QStringLiteral("optical_flow_global_fallback"), { QStringLiteral("on"), QStringLiteral("off") });
        opticalFlowFallback->setToolTip(QStringLiteral(
            "On (default): when the engine's whole-picture motion is not clear (no dominant motion), the separate "
            "search reads the camera motion instead. Off: the engine's reading is used as it is."));
        auto* opticalFlowCost = addChoice(QStringLiteral("Engine's match cost lowers trust"),
            QStringLiteral("optical_flow_cost_trust"), { QStringLiteral("on"), QStringLiteral("off") });
        opticalFlowCost->setToolTip(QStringLiteral(
            "On: the engine's own cost for each vector counts in the trust check - a less certain vector "
            "counts as a worse match (tapered or held). Off (default): only VP's match error decides "
            "(on real clips it made no measurable difference, 2026-10-17)."));
        auto* opticalFlowFb = addChoice(QStringLiteral("Engine's forward/backward check"),
            QStringLiteral("optical_flow_fb_check"), { QStringLiteral("on"), QStringLiteral("off") });
        opticalFlowFb->setToolTip(QStringLiteral(
            "On: the engine's forward and backward vectors must agree for a vector to be fully trusted "
            "(occlusions and failed matches show as disagreement): about 6% less damage for about 1-2% less "
            "judder removed on real clips. Off (default): not checked."));
        auto* opticalFlow = addChoice(QStringLiteral("Optical-flow engine (NVIDIA)"), QStringLiteral("optical_flow"),
            { QStringLiteral("on"), QStringLiteral("off") });
        opticalFlow->setToolTip(QStringLiteral(
            "On (default): on NVIDIA GPUs the optical-flow hardware computes the motion vectors, ahead of the picture. "
            "Off: VP's own matcher only (as on other GPUs). Either way the same trust checks apply, and the matcher "
            "takes over whenever the engine has no vectors."));
        auto* motionAhead = addChoice(QStringLiteral("Motion vectors computed ahead"), QStringLiteral("motion_ahead_frames"),
            { QStringLiteral("4"), QStringLiteral("0") });
        Vpv::ApplyMotionAheadText(motionAhead);
        {
            // The engine as mutually exclusive buttons (owner 2026-10-16); the drop-down holds the value.
            auto* sourceButtons = new QWidget;
            sourceButtons->setObjectName(controlName(sectionPrefix, QStringLiteral("optical_flow.buttons")));
            auto* sourceLayout = new QHBoxLayout(sourceButtons);
            sourceLayout->setContentsMargins(0, 0, 0, 0);
            auto* engineButton = new QRadioButton(QStringLiteral("NVIDIA optical-flow engine (falls back to VP's matcher)"));
            engineButton->setObjectName(controlName(sectionPrefix, QStringLiteral("optical_flow.engine")));
            engineButton->setToolTip(QStringLiteral(
                "The default. NVIDIA RTX 20 / GTX 16 or newer with driver 528+: the engine computes the motion vectors ahead "
                "of the picture on its own hardware; VP's matcher refines and checks them. Other GPUs or no engine: VP's "
                "matcher alone, automatically."));
            auto* matcherButton = new QRadioButton(QStringLiteral("VP's matcher only"));
            matcherButton->setObjectName(controlName(sectionPrefix, QStringLiteral("optical_flow.matcher")));
            matcherButton->setToolTip(QStringLiteral("VP's own block searches always (as on other GPUs)."));
            auto* group = new QButtonGroup(sourceButtons);
            group->setExclusive(true);
            group->addButton(engineButton);
            group->addButton(matcherButton);
            sourceLayout->addWidget(engineButton);
            sourceLayout->addWidget(matcherButton);
            QToolButton* flowUnset = addChoiceUnsetButton(opticalFlow, sourceLayout, receiver);
            sourceLayout->addStretch(1);
            form->addRow(QStringLiteral("Motion vectors from"), sourceButtons);
            form->setRowVisible(opticalFlow, false);
            auto sync = [opticalFlow, engineButton, matcherButton, opticalFlowGlobal, motionAhead, flowUnset]() {
                const QSignalBlocker a(engineButton), b(matcherButton);
                const bool off = effectiveChoiceValue(opticalFlow) == QStringLiteral("off");
                matcherButton->setChecked(off);
                engineButton->setChecked(!off);
                markChoiceButtons(opticalFlow, { engineButton, matcherButton }, flowUnset);
                // The engine's own options do nothing with the matcher only.
                for (QWidget* w : { static_cast<QWidget*>(opticalFlowGlobal), static_cast<QWidget*>(motionAhead) })
                {
                    w->setProperty("greyedOff", off);
                    w->setEnabled(!off);
                }
            };
            auto choose = [opticalFlow](const QString& value) {
                const int index = opticalFlow->findData(value);
                if (index >= 0 && opticalFlow->currentIndex() != index) opticalFlow->setCurrentIndex(index);
            };
            QObject::connect(opticalFlow, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [sync](int) { sync(); });
            QObject::connect(engineButton, &QRadioButton::clicked, receiver, [choose]() { choose(QStringLiteral("on")); });
            QObject::connect(matcherButton, &QRadioButton::clicked, receiver, [choose]() { choose(QStringLiteral("off")); });
            auto* refresh = new QAction(sourceButtons);
            refresh->setObjectName(controlName(sectionPrefix, QStringLiteral("optical_flow_refresh")));
            QObject::connect(refresh, &QAction::triggered, receiver, [sync]() { sync(); });
            sync();
        }
        // VP's 50/60p frame interpolation (docs/VP-interpolator-50-60-plan.md): beside the motion
        // source, as exclusive buttons (owner style); the drop-downs hold the values.
        {
            // Owner 2026-10-17: motion smoothing and hiccup repair are separate groups.
            auto* heading = new QLabel(QStringLiteral("<b>Motion smoothing (interpolation)</b>"));
            heading->setObjectName(controlName(sectionPrefix, QStringLiteral("interpolation_heading")));
            form->addRow(heading);
            auto* note = helpLabel(QStringLiteral("Off for film (film keeps the de-judder). Hiccup repair is separate, below."));
            note->setObjectName(controlName(sectionPrefix, QStringLiteral("interpolation_note")));
            form->addRow(note);
        }
        struct ChoiceButton { const char* value; QString text; QString tip; };
        // Exclusive buttons over a hidden drop-down: returns the refresh step (also run on profile load).
        auto exclusiveButtons = [receiver, &form, sectionPrefix](QComboBox* combo, const QString& key, const QString& label,
            const std::vector<ChoiceButton>& choices, const QString& fallback, QWidget** boxOut) -> std::function<void()> {
            auto* box = new QWidget;
            if (boxOut) *boxOut = box;
            box->setObjectName(controlName(sectionPrefix, key + QStringLiteral(".buttons")));
            auto* layout = new QHBoxLayout(box);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* group = new QButtonGroup(box);
            group->setExclusive(true);
            QList<QRadioButton*> radios;
            for (const ChoiceButton& c : choices)
            {
                auto* radio = new QRadioButton(c.text);
                radio->setObjectName(controlName(sectionPrefix, key + QStringLiteral(".") + QString::fromLatin1(c.value)));
                radio->setToolTip(c.tip);
                group->addButton(radio);
                layout->addWidget(radio);
                radios.push_back(radio);
                const QString value = QString::fromLatin1(c.value);
                QObject::connect(radio, &QRadioButton::clicked, receiver, [combo, value]() {
                    const int index = combo->findData(value);
                    if (index >= 0 && combo->currentIndex() != index) combo->setCurrentIndex(index);
                });
            }
            QToolButton* unset = addChoiceUnsetButton(combo, layout, receiver);
            layout->addStretch(1);
            form->addRow(label, box);
            form->setRowVisible(combo, false);
            auto sync = [combo, radios, unset, fallback]() {
                QString value = effectiveChoiceValue(combo);
                if (value.isEmpty() || (value == QStringLiteral("auto") && combo->findData(QStringLiteral("auto")) < 0))
                    value = fallback;
                for (QRadioButton* radio : radios)
                {
                    const QSignalBlocker block(radio);
                    radio->setChecked(radio->objectName().endsWith(QStringLiteral(".") + value));
                }
                markChoiceButtons(combo, radios, unset);
            };
            QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [sync](int) { sync(); });
            return sync;
        };
        auto* interpolation = addChoice(QStringLiteral("Frame interpolation"), QStringLiteral("interpolation"),
            { QStringLiteral("off"), QStringLiteral("auto"), QStringLiteral("always") });
        interpolation->setToolTip(QStringLiteral(
            "VP's own HDR-safe frame interpolation for video sources (50p / 60p and other video rates), from the "
            "motion vectors above. Off (default): today's picture. Auto: only where the source rate does not divide "
            "the display rate (50p on 60 Hz, 60p on 50 Hz): every display refresh shows the picture at its true " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "time, instead of repeated or dropped frames. Always: also at whole ratios (30p video on 60 Hz). Film stays with the " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "de-judder; 50/60p at the display's own rate is never touched; the low-latency queue switches it off. " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "Exclusive with libplacebo's smooth motion."));
        auto* interpolationFallback = addChoice(QStringLiteral("Where the motion is not trusted"),
            QStringLiteral("interpolation_fallback"), { QStringLiteral("hold"), QStringLiteral("blend") });
        interpolationFallback->setToolTip(QStringLiteral(
            "Where the motion vectors are not trusted (uncovered areas, failed matches): Hold (default) shows the "
            "nearest source frame's own pixels there, never a double image (the de-judder's rule). Blend mixes the "
            "two frames by their time, as libplacebo's frame mixer does. A scene cut always holds."));
        auto* sourceClass = addChoice(QStringLiteral("Film or video"), QStringLiteral("source_class"),
            { QStringLiteral("legacy"), QStringLiteral("measured") });
        sourceClass->setItemText(sourceClass->findData(QStringLiteral("legacy")), QStringLiteral("Fixed rates (up to 30 fps and 48p are film)")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        sourceClass->setItemText(sourceClass->findData(QStringLiteral("measured")), QStringLiteral("Measured (film detected in the signal, or 2+ refreshes per frame)"));
        sourceClass->setToolTip(QStringLiteral(
            "Which sources count as film (the de-judder's) and which as video (the interpolator's). Fixed rates "
            "(default): as before - measured rates up to about 30 fps and 48p are film. Measured: film found inside " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "the signal by pulldown detection, or a source that owns at least two display refreshes per frame; no "
            "fixed rates (48p on 60 Hz then counts as video).")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        {
            const auto syncMode = exclusiveButtons(interpolation, QStringLiteral("interpolation"), QStringLiteral("Interpolation"),
                { { "off", QStringLiteral("Off"), QStringLiteral("Today's picture (the default).") },
                  { "auto", QStringLiteral("Auto (rate mismatch only)"), QStringLiteral(
                      "Only where the source rate does not divide the display rate, e.g. 50p on 60 Hz or 60p on 50 Hz.") }, // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
                  { "always", QStringLiteral("Always (video sources)"), QStringLiteral(
                      "Also at whole ratios (30p video on 60 Hz): smooth-motion parity. Film keeps the de-judder.") } }, // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
                QStringLiteral("off"), nullptr);
            QWidget* fallbackButtons = nullptr;
            const auto syncFallback = exclusiveButtons(interpolationFallback, QStringLiteral("interpolation_fallback"),
                QStringLiteral("Untrusted motion"),
                { { "hold", QStringLiteral("Hold the nearest frame"), QStringLiteral("Never a double image (the default).") },
                  { "blend", QStringLiteral("Blend the two frames"), QStringLiteral("A time-weighted mix, as a frame mixer.") } },
                QStringLiteral("hold"), &fallbackButtons);
            auto sync = [syncMode, syncFallback, interpolation, interpolationFallback, sourceClass, fallbackButtons]() {
                syncMode();
                syncFallback();
                // The fallback and the classification do nothing with interpolation off. Hiccup
                // repair is its own group and is never greyed by motion smoothing (owner 2026-10-17).
                const bool off = effectiveChoiceValue(interpolation) != QStringLiteral("auto") &&
                    effectiveChoiceValue(interpolation) != QStringLiteral("always");
                for (QWidget* w : { static_cast<QWidget*>(interpolationFallback), static_cast<QWidget*>(sourceClass) })
                {
                    w->setProperty("greyedOff", off);
                    w->setEnabled(!off);
                }
                if (fallbackButtons)
                    fallbackButtons->setEnabled(!off);
            };
            QObject::connect(interpolation, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [sync](int) { sync(); });
            auto* refresh = new QAction(interpolation);
            refresh->setObjectName(controlName(sectionPrefix, QStringLiteral("interpolation_refresh")));
            QObject::connect(refresh, &QAction::triggered, receiver, [sync]() { sync(); });
            sync();
        }
        {
            auto* heading = new QLabel(QStringLiteral("<b>Repair dropped frames / hiccups</b>"));
            heading->setObjectName(controlName(sectionPrefix, QStringLiteral("hiccup_repair_heading")));
            form->addRow(heading);
            // Render-ahead 2026-10-17: the renderer decides the fill independently of
            // interpolation, so hiccup repair works with motion smoothing off.
            form->addRow(QString(), helpLabel(QStringLiteral(
                "Works with motion smoothing on or off (on by default).")));
        }
        // Owner 2026-10-18: hiccup repair is on by default; motion smoothing (interpolation) stays off.
        auto* fillGaps = addChoice(QStringLiteral("Fill dropped frames"), QStringLiteral("interpolation_fill_gaps"),
            { QStringLiteral("on"), QStringLiteral("off") });
        fillGaps->setItemText(fillGaps->findData(QStringLiteral("on")), QStringLiteral("On (default; pair across gaps up to about 1/6 s)"));
        fillGaps->setItemText(fillGaps->findData(QStringLiteral("off")), QStringLiteral("Off"));
        // Fixed-constant batch 2026-10-18: the span is a time at the measured rate (general_rate_fixes).
        fillGaps->setToolTip(QStringLiteral(
            "On (default); works with motion smoothing on or off, and motion smoothing stays off by default. "
            "When a stream loses frames for up to about 1/6 second (1-3 frames at 24p, up to 4 at 30p, up to 9 at 60p: "
            "the same time at every frame rate), On pairs the "
            "frames either side of the gap and fills the missing refreshes with pictures at their true time (the "
            "motion is checked as always; untrusted areas hold). Longer gaps are never paired. With General "
            "frame-rate rules off the gap is 1-3 frames at any rate. Off keeps the rule that no motion is "
            "paired across a gap: the last frame stays on screen until the next arrives. (Key: interpolation_fill_gaps.)"));
        form = fixesForm;
        auto* pulldownFixes = addChoice(QStringLiteral("Film-in-video detection: keep the lock"), QStringLiteral("pulldown_fixes"),
            { QStringLiteral("on"), QStringLiteral("off") });
        pulldownFixes->setToolTip(QStringLiteral(
            "For 'Find film sent with repeated frames' (when it is on). On (default): a missed capture frame or an "
            "edit in the film's repeat pattern no longer drops the lock for 2 s - the pattern is followed through "
            "it, and the film's even timing is kept. Off: the earlier behaviour (drop the lock, find it again)."));
        // Bug hunt 2026-10-17.
        auto* timingContinuity = addChoice(QStringLiteral("Timing continuity fixes"), QStringLiteral("timing_continuity_fixes"),
            { QStringLiteral("on"), QStringLiteral("off") });
        timingContinuity->setToolTip(QStringLiteral(
            "Under test (default on). A jump in the capture's timestamps is treated as a break: no motion is carried "
            "across it. The film clock stays continuous when the measured capture rate replaces or resets the nominal one, "
            "and the 50/60p timeline keeps the last measured display rate while it is re-measured. Off: the earlier behaviour.")); // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
        // Hunt 2 2026-10-17 (timing and motion).
        auto* cadenceClass = addChoice(QStringLiteral("Film / video classification fixes"), QStringLiteral("cadence_class_fixes"),
            { QStringLiteral("on"), QStringLiteral("off") });
        cadenceClass->setToolTip(QStringLiteral(
            "Under test (default on). Film-in-video detection never takes animation drawn on twos for 15 fps film, its film "
            "timing re-anchors after a phase shift and ignores a single late frame, and the queue holds the same time while "
            "locked. Film / video is judged from the source alone (never from the display rate), and content recognition "
            "no longer takes 3:2 film or dark film for animation or a still menu, and also runs without look-ahead frames. "
            "Off: the earlier behaviour."));
        // Hunt 2 fix group 5 (motion).
        auto* motionHistory = addChoice(QStringLiteral("Motion history fixes"), QStringLiteral("motion_history_fixes"),
            { QStringLiteral("on"), QStringLiteral("off") });
        motionHistory->setToolTip(QStringLiteral(
            "Under test (default on). The motion search starts its history over when a frame pair was skipped, and "
            "forgets its strength ramp at a break; a repeated frame never uses the next pair's motion; the frame "
            "estimated ahead follows the same timing and Dolby Vision rules as the next frame, and Dolby Vision frames "
            "pair only under the same colour matrices; changing a motion setting or the motion source restarts the "
            "motion; a frame dropped to keep a queued repeat counts as a gap. Off: the earlier behaviour."));
        auto* vectorDemand = addChoice(QStringLiteral("Vector engine on demand"), QStringLiteral("vector_demand"),
            { QStringLiteral("on"), QStringLiteral("off") });
        vectorDemand->setToolTip(QStringLiteral(
            "Runs the motion engine only when a feature that needs motion vectors is active, at any frame rate. "
            "Off: no motion work; de-judder and hiccup smoothing are off, other features use their non-motion path."));
        // Hunt 2 group 5 (13-timing).
        auto* queueTiming = addChoice(QStringLiteral("Queue timing fixes"), QStringLiteral("queue_timing_fixes"),
            { QStringLiteral("on"), QStringLiteral("off") });
        queueTiming->setToolTip(QStringLiteral(
            "Under test (default on). The queue drops frames to catch up only when it is really behind (time beyond "
            "its own target), so motion smoothing at 24-50 Hz no longer drops a frame every minute or two; the drift " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "correction's timings are in seconds at every frame rate and follow the real queue depth; a dropped frame's "
            "gap is kept; render ahead without frame statistics prefers the measured refresh over the whole-hertz mode. "
            "Off: the earlier behaviour."));
        // Fixed-constant batch 2026-10-18 (owner: no fixed rates, cadences or resolutions).
        auto* generalRates = addChoice(QStringLiteral("General frame-rate rules"), QStringLiteral("general_rate_fixes"),
            { QStringLiteral("on"), QStringLiteral("off") });
        generalRates->setToolTip(QStringLiteral(
            "Under test (default on). Film / video is judged from the measured frame rate with no fixed list of rates: "
            "everything slower than the 50 Hz television rate (below 49 fps) is film-like, 50 fps and faster is video. " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "24, 25, 30 and 48 fps are classed as before; odd rates between 31 and 47 fps are now film-like. " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "Repairing dropped frames pairs across a gap of up to about 1/6 second at any frame rate (1-3 missed frames "
            "at 24p, up to 9 at 60p) instead of a fixed 1-3 frames. Above 60 fps the frame counts tuned at 60 fps keep " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "their time: motion vectors computed ahead (4 frames at 60 fps, 8 at 120), the queue's time-target ceiling, " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "the debander's hold and the DirectShow priming evidence. Up to 2160 lines nothing changes with the picture "
            "height; taller pictures scale the matcher's tolerances. 24 and 60 fps at 1080 and 2160 lines are unchanged. " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "Off: the earlier fixed rate bands and frame counts."));
        // Efficiency round 2026-10-18 (docs/agent-reports/efficiency.md).
        auto* efficiency = addChoice(QStringLiteral("Efficiency fixes"), QStringLiteral("efficiency_fixes"),
            { QStringLiteral("on"), QStringLiteral("off") });
        efficiency->setToolTip(QStringLiteral(
            "Under test (default on). Work done twice is done once, with the same picture: the frame shown uses the "
            "planes already uploaded for the motion estimate when it was the next frame (one upload per frame saved "
            "while the de-judder runs), and the look-ahead measures a queued frame's picture area only while the LED "
            "or content recognition still needs that frame. Off: the earlier behaviour."));
        // Render-ahead (docs/VP-render-ahead-design.md, owner 2026-10-17): processing follows the
        // source frame rate, not the display refresh. Off by default until the on-screen runs pass.
        auto* renderAhead = addChoice(QStringLiteral("De-judder: render ahead (under test)"), QStringLiteral("render_ahead"),
            { QStringLiteral("off"), QStringLiteral("on") });
        renderAhead->setToolTip(QStringLiteral(
            "Under test (default off). On: a frame's de-judder in-between pictures are queued straight after the frame, "
            "without waiting for each refresh, so the next frame starts rendering at once. The GPU then has a whole "
            "source frame's time (41.7 ms at 24p) for a frame and its in-betweens, instead of one refresh (16.7 ms at "
            "60 Hz): processing follows the source frame rate, not the display's. Cost: about the in-betweens' span " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "of extra display latency, reported so audio stays in step. Off: each in-between waits for the screen, as "
            "before."));
        auto* dejudderGate = addChoice(QStringLiteral("De-judder: GPU time budget (under test)"), QStringLiteral("dejudder_gate"),
            { QStringLiteral("auto"), QStringLiteral("period"), QStringLiteral("refresh") });
        dejudderGate->setItemText(dejudderGate->findData(QStringLiteral("auto")), QStringLiteral("Auto (per frame with render ahead)"));
        dejudderGate->setItemText(dejudderGate->findData(QStringLiteral("period")), QStringLiteral("Per source frame"));
        dejudderGate->setItemText(dejudderGate->findData(QStringLiteral("refresh")), QStringLiteral("Per refresh (the earlier rule)"));
        dejudderGate->setToolTip(QStringLiteral(
            "How VP decides the de-judder fits the GPU. Per source frame: the GPU work of a frame and its in-betweens "
            "must fit the share of a source frame set below. When it does not, VP steps down one rung at a time and "
            "never lowers precision: first the dither reuses the de-judder's motion instead of its own search, then the "
            "least visible in-between is dropped, then the next, and last the picture holds (plain 3:2). It steps back "
            "up when there is room. Per refresh: the earlier rule (a new frame's whole render plus 4 ms must fit one "
            "refresh). Auto (default): per source frame with render ahead on, otherwise per refresh. Per source frame "
            "without render ahead acts as per refresh."));
        auto* renderAheadUtilisation = addText(QStringLiteral("De-judder: share of a source frame to use"),
            QStringLiteral("render_ahead_utilisation"));
        renderAheadUtilisation->setValidator(new QDoubleValidator(0.5, 1.0, 2, renderAheadUtilisation));
        clampNumberFieldOnLeave(renderAheadUtilisation, 0.5, 1.0, 2);
        renderAheadUtilisation->setPlaceholderText(QStringLiteral("0.85"));
        renderAheadUtilisation->setToolTip(QStringLiteral(
            "With the per-source-frame budget: the share of a source frame's time the GPU may be busy (0.5 to 1.0; "
            "default 0.85 = 85 %). The rest is margin for clock swings and the desktop. Higher keeps more in-betweens "
            "on a slow GPU but risks late frames."));
        auto* renderAheadCredit = addChoice(QStringLiteral("Render ahead: shorten the input queue (under test)"),
            QStringLiteral("render_ahead_queue_credit"), { QStringLiteral("off"), QStringLiteral("on") });
        renderAheadCredit->setToolTip(QStringLiteral(
            "With render ahead on. On: the input queue is shortened by the frames render ahead adds, so the total "
            "delay from capture to screen stays as it was. Off (default): the queue is unchanged and the added latency "
            "is reported for audio."));
        // Film grain (owner 2026-10-17: never averaged or interpolated away; docs/audit/grain-audit-2026-10-17.md).
        // Each default is the faithful path; the other value is the established output.
        auto* interpolationGrain = addChoice(QStringLiteral("50/60p interpolation keeps film grain"),
            QStringLiteral("interpolation_grain"), { QStringLiteral("preserve"), QStringLiteral("blend") });
        interpolationGrain->setItemText(interpolationGrain->findData(QStringLiteral("preserve")), QStringLiteral("Preserve (default)"));
        interpolationGrain->setItemText(interpolationGrain->findData(QStringLiteral("blend")), QStringLiteral("Blend (the earlier output)"));
        interpolationGrain->setToolTip(QStringLiteral(
            "Preserve (default): an interpolated picture keeps the grain of the nearer source frame instead of "
            "averaging two frames' grain away. Blend: the earlier output (grain softened between frames). "
            "Also used by Fill dropped frames (hiccup repair), with motion smoothing on or off."));
        auto* warpFilter = addChoice(QStringLiteral("De-judder source warp filter"),
            QStringLiteral("dejudder_source_warp_filter"), { QStringLiteral("catmull"), QStringLiteral("bilinear") });
        warpFilter->setItemText(warpFilter->findData(QStringLiteral("catmull")), QStringLiteral("Catmull-Rom (default, keeps grain)"));
        warpFilter->setItemText(warpFilter->findData(QStringLiteral("bilinear")), QStringLiteral("Bilinear (the earlier output)"));
        warpFilter->setToolTip(QStringLiteral(
            "How the de-judder samples the source when it warps it. Catmull-Rom (default) keeps fine grain; "
            "bilinear keeps only about half of it at half-pixel shifts. The rendered warp is Catmull-Rom either way."));
        // Hunt 2 M1: how the NLS / NLS+ warp samples the source.
        auto* nlsSampling = addChoice(QStringLiteral("NLS stretch sampling"),
            QStringLiteral("nls_sampling"), { QStringLiteral("catmull"), QStringLiteral("bilinear") });
        nlsSampling->setItemText(nlsSampling->findData(QStringLiteral("catmull")), QStringLiteral("Catmull-Rom (default, faithful)"));
        nlsSampling->setItemText(nlsSampling->findData(QStringLiteral("bilinear")), QStringLiteral("Bilinear (the earlier output)"));
        nlsSampling->setToolTip(QStringLiteral(
            "How the nonlinear stretch (NLS, NLS+, NLS-V) reads the picture it reshapes. Catmull-Rom (default): a sharp "
            "filter along the stretched axis that keeps grain and detail, widened where the stretch shrinks the "
            "picture (the centre) so fine detail does not alias. Bilinear: the earlier output (softer, the centre "
            "shrunk without a filter). Only matters while an NLS shader is active."));
        MarkGpuCost(form, nlsSampling, 1);
        auto* residualGrainGuard = addChoice(QStringLiteral("Residual stabilise leaves grain alone"),
            QStringLiteral("residual_stabilise_grain_guard"), { QStringLiteral("on"), QStringLiteral("off") });
        residualGrainGuard->setToolTip(QStringLiteral(
            "On (default): the CNN / SSIM residual held along the motion is not held where the block is grain, so "
            "grain is not frozen or smeared. Off: the earlier behaviour."));
        auto* debandGrainGuard = addChoice(QStringLiteral("VP deband leaves grain alone"),
            QStringLiteral("deband_vp_grain_guard"), { QStringLiteral("on"), QStringLiteral("veto"), QStringLiteral("off") });
        debandGrainGuard->setItemText(debandGrainGuard->findData(QStringLiteral("on")), QStringLiteral("On: grain split (default)"));
        debandGrainGuard->setItemText(debandGrainGuard->findData(QStringLiteral("veto")), QStringLiteral("Veto grainy blocks (the earlier guard)"));
        debandGrainGuard->setItemText(debandGrainGuard->findData(QStringLiteral("off")), QStringLiteral("Off (the earlier debander)"));
        debandGrainGuard->setToolTip(QStringLiteral(
            "On (default): VP's debander splits the picture into a smooth layer and the fine grain and noise on top; "
            "only the smooth layer is debanded and the grain is put back untouched, so banding is removed even where "
            "the stream kept some noise in the bands. Veto: the earlier guard, which skips blocks that look grainy "
            "(it also skips noisy bands). Off: the earlier debander, byte for byte (it smooths grain)."));
        auto* debandVpExclusive = addChoice(QStringLiteral("VP deband replaces libplacebo's"),
            QStringLiteral("deband_vp_exclusive"), { QStringLiteral("on"), QStringLiteral("off") });
        debandVpExclusive->setToolTip(QStringLiteral(
            "On (default): while VP's debander runs (VP deband Auto or On) with its grain guard, libplacebo's "
            "automatic deband (Debanding: Auto) is turned off, because it thins fine grain. A Debanding strength "
            "chosen explicitly (Light or Default) is kept. Off: both debanders run, as before."));
        {
            // Hunt 2 (L4): a setting that does nothing in the current state is greyed
            // (greyedOff, audit 2026-10-14c), read from the effective values so an
            // inherited or default choice counts. Profile loads trigger the refresh.
            const auto grey = [](QWidget* widget, bool off)
            {
                widget->setProperty("greyedOff", off);
                widget->setEnabled(!off);
            };
            auto syncGreyed = [grey, opticalFlow, opticalFlowGlobal, opticalFlowFallback, opticalFlowCost,
                opticalFlowFb, renderAhead, dejudderGate, renderAheadUtilisation, renderAheadCredit,
                sourceResolution, autoConfidence, evidenceSeconds, evidenceShots, interpolation,
                interpolationGrain, debandVp, debandGrainGuard, debandVpExclusive, latchFixes, engageWait,
                crossfade, releaseFade, fillGaps]()
            {
                // The engine's extras do nothing with VP's matcher only; the search
                // fallback also needs the engine's camera motion.
                const bool matcherOnly = effectiveChoiceValue(opticalFlow) == QStringLiteral("off");
                grey(opticalFlowCost, matcherOnly);
                grey(opticalFlowFb, matcherOnly);
                grey(opticalFlowFallback, matcherOnly ||
                    effectiveChoiceValue(opticalFlowGlobal) == QStringLiteral("off"));
                // Render ahead's budget, queue credit and share apply only with it on;
                // the share only with the per-source-frame budget.
                const bool renderAheadOff = effectiveChoiceValue(renderAhead) != QStringLiteral("on");
                grey(dejudderGate, renderAheadOff);
                grey(renderAheadCredit, renderAheadOff);
                grey(renderAheadUtilisation, renderAheadOff ||
                    effectiveChoiceValue(dejudderGate) == QStringLiteral("refresh"));
                // Auto's evidence is used only with Source resolution: Auto.
                const bool sourceAuto = effectiveChoiceValue(sourceResolution) == QStringLiteral("auto");
                for (QWidget* widget : { static_cast<QWidget*>(autoConfidence), static_cast<QWidget*>(evidenceSeconds),
                    static_cast<QWidget*>(evidenceShots) })
                    grey(widget, !sourceAuto);
                // Hunt 3 (CFG2): the latch timings need Auto and the latch fixes.
                const bool latchOff = !sourceAuto || effectiveChoiceValue(latchFixes) == QStringLiteral("off");
                for (QWidget* widget : { static_cast<QWidget*>(engageWait), static_cast<QWidget*>(crossfade),
                    static_cast<QWidget*>(releaseFade) })
                    grey(widget, latchOff);
                // Hunt 3 (CFG4): the grain choice also shapes hiccup repair's gap fill
                // (interpolation_fill_gaps), which presents interpolated pictures with
                // motion smoothing off.
                const QString interpolationMode = effectiveChoiceValue(interpolation);
                grey(interpolationGrain, interpolationMode != QStringLiteral("auto") &&
                    interpolationMode != QStringLiteral("always") &&
                    effectiveChoiceValue(fillGaps) != QStringLiteral("on"));
                grey(debandGrainGuard, effectiveChoiceValue(debandVp) == QStringLiteral("off"));
                grey(debandVpExclusive, effectiveChoiceValue(debandVp) == QStringLiteral("off"));
            };
            for (QComboBox* driver : { opticalFlow, opticalFlowGlobal, renderAhead, dejudderGate, sourceResolution,
                interpolation, debandVp, latchFixes, fillGaps })
                QObject::connect(driver, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [syncGreyed](int) { syncGreyed(); });
            auto* greyedRefresh = new QAction(debandGrainGuard);
            greyedRefresh->setObjectName(controlName(sectionPrefix, QStringLiteral("greyed_state_refresh")));
            QObject::connect(greyedRefresh, &QAction::triggered, receiver, [syncGreyed]() { syncGreyed(); });
            syncGreyed();
        }
        // Restore switch (DV tunnel bug hunt 2026-10-17).
        auto* dvTunnelFixes = addChoice(QStringLiteral("Dolby Vision tunnel fixes"),
            QStringLiteral("dv_tunnel_fixes"), { QStringLiteral("on"), QStringLiteral("off") });
        dvTunnelFixes->setToolTip(QStringLiteral(
            "Under test (default on). The SDR placement probe samples the whole picture (letterboxed or dark menus no "
            "longer stay black), a grey SDR menu ends the tunnel, the metadata-loss limit counts source pictures, and "
            "L2 trims are kept before L8. Off: the earlier behaviour."));
        auto* nativeRgb = addChoice(QStringLiteral("Native 10/12-bit RGB capture"),
            QStringLiteral("native_rgb_ingest"), { QStringLiteral("on"), QStringLiteral("off") });
        nativeRgb->setToolTip(QStringLiteral(
            "DeckLink 12-bit RGB (R12B/R12L) and 10-bit r210 reach the renderer as full-precision RGB 4:4:4, unpacked "
            "on the GPU. Off = the old CPU conversion to 10-bit 4:2:0 YCbCr."));
        auto* dvTrims = addChoice(QStringLiteral("VP's interpretation of DV metadata: L2 trims"),
            QStringLiteral("dv_trims"), { QStringLiteral("on"), QStringLiteral("off") });
        dvTrims->setToolTip(QStringLiteral(
            "Default on (DV synthetic validation, 2026-10-24). VP's interpretation of DV metadata: applies the Level 2 trim chosen for "
            "your display's peak (interpolated between the grader's trims) after VP's tone mapping: slope, offset, "
            "power, chroma weight and saturation. Not certified and not Dolby display management. "
            "Not used with a calibration 3D LUT."));
        auto* dvTone = addChoice(QStringLiteral("VP's interpretation of DV metadata: tone mapping"),
            QStringLiteral("dv_tone_mapping"), { QStringLiteral("libplacebo"), QStringLiteral("dolby_style") });
        dvTone->setItemText(dvTone->findData(QStringLiteral("dolby_style")), QStringLiteral("Dolby-style (approx.)"));
        dvTone->setToolTip(QStringLiteral(
            "Under test (default libplacebo). dolby_style: for Dolby Vision pictures, a Dolby-style curve driven by the "
            "scene metadata (L1) and your tone-mapping target, with the L2/L8 trims applied in ICtCp. An approximation of "
            "the published structure, not certified and not Dolby display management; to be judged against LLDV. "
            "When it cannot run (e.g. a calibration 3D LUT) VP uses bt2390 + trims and lists it under Fallbacks engaged."));
        auto* dvCompare = addText(QStringLiteral("DV comparison capture folder (diagnostic; empty = off)"),
            QStringLiteral("dv_compare_capture_folder"));
        dvCompare->setToolTip(QStringLiteral(
            "Diagnostic, off when empty. A folder on a large drive (never a source folder): the dv_compare_capture "
            "shortcut (default Ctrl+Alt+Shift+W) or a RECORD file there records a short labelled clip (raw frames plus the "
            "decoded Dolby Vision metadata and the trims VP chose) for the LLDV-vs-tunnel comparison "
            "(docs/dv/LLDV-vs-tunnel-check.md). Clips are never uploaded or committed."));
        auto* dvGpu = addChoice(QStringLiteral("Dolby Vision tunnel: GPU unpack"),
            QStringLiteral("dv_tunnel_gpu_unpack"), { QStringLiteral("on"), QStringLiteral("off") });
        dvGpu->setToolTip(QStringLiteral(
            "Default on (DV synthetic validation, 2026-10-24). The Dolby Vision RGB tunnel's 12-bit picture is unpacked on the GPU, bit for bit "
            "the same as the CPU; the CPU still reads and checks the Dolby Vision metadata packet, but no longer the whole "
            "frame. If the GPU unpack fails, VP goes back to the CPU unpack for the session (logged)."));

        form = addCollapsibleSection(QStringLiteral("dejudderExperiments"), QStringLiteral("De-judder: experiments"),
            QStringLiteral("Off by default; for comparisons only. ") + QString::fromLatin1(kGpuCostLegend), false);
        QFormLayout* dejudderExperimentsForm = form;
        auto* warpStage = addChoice(QStringLiteral("Extra frames from"),
            QStringLiteral("dejudder_warp_stage"), { QStringLiteral("rendered"), QStringLiteral("source") });
        warpStage->setItemText(warpStage->findData(QStringLiteral("rendered")), QStringLiteral("Rendered picture (fast)"));
        warpStage->setItemText(warpStage->findData(QStringLiteral("source")), QStringLiteral("Source (full re-render)"));
        warpStage->setToolTip(QStringLiteral(
            "Rendered picture: each in-between frame warps the finished picture and runs only the final "
            "dither stage (a fraction of the GPU time of a full render). Source: each in-between frame re-renders "
            "the whole chain from a warped source, the earlier path, kept for A/B comparison."));
        auto* occlusionFill = addChoice(QStringLiteral("Fill uncovered areas (experiment)"),
            QStringLiteral("dejudder_occlusion_fill"), { QStringLiteral("on"), QStringLiteral("off") });
        occlusionFill->setToolTip(QStringLiteral(
            "Experiment, default off: picture newly uncovered between two film frames is taken from the "
            "next frame instead of held. Occlusion fill: source warp path only (Extra frames from: "
            "Source); with Rendered picture it is ignored (logged once)."));
        auto* taper = addChoice(QStringLiteral("Taper marginal blocks (experiment)"),
            QStringLiteral("dejudder_block_taper"), { QStringLiteral("on"), QStringLiteral("off") });
        taper->setToolTip(QStringLiteral(
            "Experiment, default off: blocks that match only marginally move at a reduced strength instead of "
            "holding. Fewer judder patches, at the risk of a regional, floaty look."));
        auto* taperSad = addText(QStringLiteral("Taper ends at (codes, 0 = twice the tolerance)"),
            QStringLiteral("dejudder_taper_sad"));
        taperSad->setValidator(new QDoubleValidator(0.0, 32.0, 2, taperSad));
        taperSad->setPlaceholderText(QStringLiteral("0"));
        taperSad->setToolTip(QStringLiteral(
            "With the taper on: the match error (8-bit codes) at which a marginal block's strength reaches zero; "
            "between the tolerance and this it ramps down. 0 = twice the match tolerance."));
        MarkGpuCost(form, warpStage, 3);
        MarkGpuCost(form, occlusionFill, 1);

        // Intent interpreter (docs/VP-intent-interpreter-design.md; owner decision 2026-10-17): one
        // group with a checkbox per function, each off by default, its sub-settings beneath it. Item 1
        // (content recognition) and items 2-5 (motion, ambient, black, display) are all built.
        form = addCollapsibleSection(QStringLiteral("contentAdaptive"), QStringLiteral("Experimental content-adaptive settings"),
            QStringLiteral("Each function is off by default. Rule-based (no AI): VP measures the picture and reads the "
                "Dolby Vision metadata, and only steers settings you already set, within your limits."), false);
        {
            auto* contentMode = addChoice(QStringLiteral("Content recognition"), QStringLiteral("intent_content"),
                { QStringLiteral("off"), QStringLiteral("on") });
            form->setRowVisible(contentMode, false);
            auto* contentRow = new QWidget;
            contentRow->setObjectName(controlName(sectionPrefix, QStringLiteral("intent_content.row")));
            auto* contentLayout = new QHBoxLayout(contentRow);
            contentLayout->setContentsMargins(0, 0, 0, 0);
            auto* contentCheck = new QCheckBox(QStringLiteral("Content recognition"));
            contentCheck->setObjectName(controlName(sectionPrefix, QStringLiteral("intent_content.check")));
            contentCheck->setToolTip(QStringLiteral(
                "Off (default). On: VP labels what is playing - film, video, sports, animation or a still menu "
                "(static_ui) - from the pulldown lock and measured rates, the film/video setting, the camera "
                "motion of the queued frames and the Dolby Vision L11 content type (which wins when present). "
                "Your profile rules can then use it: when: $content==\"sports\". It only selects profiles you "
                "wrote; it never changes a setting itself. Sports means a video-class source with sustained "
                "large pans, never a fixed frame rate. Needs the active-picture look-ahead for the motion. "
                "(Key: intent_content.)"));
            contentLayout->addWidget(contentCheck);
            QToolButton* contentUnset = addChoiceUnsetButton(contentMode, contentLayout, receiver);
            contentLayout->addStretch(1);
            form->addRow(QString(), contentRow);
            auto* hold = addText(QStringLiteral("    Change class after"), QStringLiteral("intent_content_hold_seconds"),
                QStringLiteral("s"));
            hold->setValidator(new QDoubleValidator(1.0, 60.0, 1, hold));
            clampNumberFieldOnLeave(hold, 1.0, 60.0, 1);
            hold->setPlaceholderText(QStringLiteral("5"));
            hold->setAccessibleDescription(QStringLiteral("Value in s, 1-60."));
            hold->setToolTip(QStringLiteral(
                "A new class must hold this long (seconds of content, 1-60, default 5) and then waits for a cut, "
                "so a profile never switches in the middle of a shot. A still menu is entered without a cut and "
                "left at once. (Key: intent_content_hold_seconds.)"));
            QObject::connect(contentCheck, &QCheckBox::clicked, receiver, [contentMode](bool checked) {
                const int index = contentMode->findData(checked ? QStringLiteral("on") : QStringLiteral("off"));
                if (index >= 0 && contentMode->currentIndex() != index) contentMode->setCurrentIndex(index);
            });
            auto syncContent = [contentMode, contentCheck, contentUnset, hold]() {
                const bool on = effectiveChoiceValue(contentMode) == QStringLiteral("on");
                {
                    const QSignalBlocker block(contentCheck);
                    contentCheck->setChecked(on);
                }
                const bool inherited = contentMode->currentData().toString().isEmpty();
                contentCheck->setProperty("inherited", inherited);
                contentCheck->style()->unpolish(contentCheck);
                contentCheck->style()->polish(contentCheck);
                if (contentUnset)
                    contentUnset->setVisible(!inherited && contentMode->count() > 0 &&
                        contentMode->itemData(0).toString().isEmpty());
                hold->setProperty("greyedOff", !on);
                hold->setEnabled(on);
            };
            QObject::connect(contentMode, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [syncContent](int) { syncContent(); });
            auto* contentRefresh = new QAction(contentMode);
            contentRefresh->setObjectName(controlName(sectionPrefix, QStringLiteral("intent_content_refresh")));
            QObject::connect(contentRefresh, &QAction::triggered, receiver, [syncContent]() { syncContent(); });
            syncContent();

            // Items 2-5 (owner decision 2026-10-17): a checkbox per function over a hidden drop-down
            // holding the value, its sub-settings beneath, greyed while the function is off.
            struct IntentToggle { QComboBox* combo; QCheckBox* check; QToolButton* unset; };
            auto intentSyncs = std::make_shared<std::vector<std::function<void()>>>();
            // onValue: what ticking the box selects (on, or report for black protection).
            auto addIntentToggle = [&](const QString& label, const QString& key, const QString& tip,
                const QStringList& choices, const QString& onValue) -> IntentToggle {
                auto* combo = addChoice(label, key, choices);
                form->setRowVisible(combo, false);
                auto* row = new QWidget;
                row->setObjectName(controlName(sectionPrefix, key + QStringLiteral(".row")));
                auto* rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                auto* check = new QCheckBox(label);
                check->setObjectName(controlName(sectionPrefix, key + QStringLiteral(".check")));
                check->setToolTip(tip);
                rowLayout->addWidget(check);
                QToolButton* unset = addChoiceUnsetButton(combo, rowLayout, receiver);
                rowLayout->addStretch(1);
                form->addRow(QString(), row);
                QObject::connect(check, &QCheckBox::clicked, receiver, [combo, onValue](bool checked) {
                    const int index = combo->findData(checked ? onValue : QStringLiteral("off"));
                    if (index >= 0 && combo->currentIndex() != index) combo->setCurrentIndex(index);
                });
                return { combo, check, unset };
            };
            // Wires the checkbox state; greyTargets are greyed while the function is off.
            auto wireIntentToggle = [&](const IntentToggle& t, const QList<QWidget*>& greyTargets) {
                auto sync = [t, greyTargets]() {
                    const bool on = effectiveChoiceValue(t.combo) != QStringLiteral("off");
                    {
                        const QSignalBlocker block(t.check);
                        t.check->setChecked(on);
                    }
                    const bool inherited = t.combo->currentData().toString().isEmpty();
                    t.check->setProperty("inherited", inherited);
                    t.check->style()->unpolish(t.check);
                    t.check->style()->polish(t.check);
                    if (t.unset)
                        t.unset->setVisible(!inherited && t.combo->count() > 0 &&
                            t.combo->itemData(0).toString().isEmpty());
                    for (QWidget* w : greyTargets)
                    {
                        w->setProperty("greyedOff", !on);
                        w->setEnabled(on);
                    }
                };
                QObject::connect(t.combo, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [sync](int) { sync(); });
                intentSyncs->push_back(sync);
                sync();
            };
            auto addIntentNumber = [&](const QString& label, const QString& key, const QString& unit,
                double lo, double hi, int decimals, const QString& placeholder, const QString& tip) {
                auto* edit = addText(QStringLiteral("    ") + label, key, unit);
                edit->setValidator(new QDoubleValidator(lo, hi, decimals, edit));
                clampNumberFieldOnLeave(edit, lo, hi, decimals);
                edit->setPlaceholderText(placeholder);
                edit->setAccessibleDescription(QStringLiteral("%1 to %2%3.").arg(lo).arg(hi)
                    .arg(unit.isEmpty() ? QString() : QStringLiteral(" ") + unit));
                edit->setToolTip(tip);
                return edit;
            };

            // Motion intent.
            {
                const auto motion = addIntentToggle(QStringLiteral("Motion intent"), QStringLiteral("intent_motion"),
                    QStringLiteral(
                        "Experimental, default off. On: VP analyses each shot's motion and, by the policy below, may "
                        "scale your de-judder strength DOWN for that shot. It never raises dejudder_strength (yours "
                        "is the master), and the scale only changes at cuts (shots shorter than the time below keep "
                        "the previous scale; fades keep it; video-class sources are left alone). With the policy "
                        "\"Display only\" (the default) it only reports on the stats overlay and in the log. Off: "
                        "nothing is analysed and the strength is exactly yours. (Key: intent_motion.)"),
                    { QStringLiteral("off"), QStringLiteral("on") }, QStringLiteral("on"));
                auto* policy = addChoice(QStringLiteral("Motion intent policy"), QStringLiteral("intent_motion_policy"),
                    { QStringLiteral("display"), QStringLiteral("ease_fast"), QStringLiteral("ease_slow"),
                      QStringLiteral("visibility"), QStringLiteral("metadata") });
                policy->setItemText(policy->findData(QStringLiteral("display")), QStringLiteral("Display only"));
                policy->setItemText(policy->findData(QStringLiteral("ease_fast")), QStringLiteral("Ease on fast pans"));
                policy->setItemText(policy->findData(QStringLiteral("ease_slow")), QStringLiteral("Keep fast pans, ease on slow deliberate shots"));
                policy->setItemText(policy->findData(QStringLiteral("visibility")), QStringLiteral("Visibility-weighted"));
                policy->setItemText(policy->findData(QStringLiteral("metadata")), QStringLiteral("Metadata only"));
                policy->setToolTip(QStringLiteral(
                    "Experimental. What motion intent may do with the de-judder strength; it only ever scales yours "
                    "down, at cuts. Default Display only. (Key: intent_motion_policy.)"));
                QWidget* policyBox = nullptr;
                const auto syncPolicy = exclusiveButtons(policy, QStringLiteral("intent_motion_policy"),
                    QStringLiteral("    Policy"),
                    { { "display", QStringLiteral("Display only"), QStringLiteral(
                          "Per-shot analysis on the stats overlay and in the log; the strength is unchanged (the "
                          "default). Motion intent never raises dejudder_strength. (Key: intent_motion_policy.)") },
                      { "ease_fast", QStringLiteral("Ease on fast pans"), QStringLiteral(
                          "Scale the strength down as the pan speed rises (the de-judder's motion roll-off, as one "
                          "policy). Never above your strength; changes at cuts. (Key: intent_motion_policy.)") },
                      { "ease_slow", QStringLiteral("Keep fast pans, ease on slow deliberate shots"), QStringLiteral(
                          "Full strength where judder is most visible; scaled down where the shot is slow and steady "
                          "and the film look is likely intended. Never above your strength. (Key: intent_motion_policy.)") },
                      { "visibility", QStringLiteral("Visibility-weighted"), QStringLiteral(
                          "Scale by the estimated judder visibility (pan speed band and edge contrast): eased on "
                          "static and blur-dominated shots. Never above your strength. (Key: intent_motion_policy.)") },
                      { "metadata", QStringLiteral("Metadata only"), QStringLiteral(
                          "Acts only on Dolby Vision L11 frame-rate-conversion hints or Authentic Motion metadata "
                          "when present; otherwise unchanged. Never above your strength. (Key: intent_motion_policy.)") } },
                    QStringLiteral("display"), &policyBox);
                intentSyncs->push_back(syncPolicy);
                auto* motionMin = addIntentNumber(QStringLiteral("Minimum strength scale"), QStringLiteral("intent_motion_min"),
                    QString(), 0.0, 1.0, 2, QStringLiteral("0.5"), QStringLiteral(
                        "Experimental. The lowest factor motion intent may apply to your de-judder strength (0-1, "
                        "default 0.5): 1 = never reduce, 0 = may switch the de-judder off for a shot. It never "
                        "raises the strength. (Key: intent_motion_min.)"));
                auto* motionShot = addIntentNumber(QStringLiteral("Shortest shot that changes the scale"),
                    QStringLiteral("intent_motion_min_shot_s"), QStringLiteral("s"), 0.5, 30.0, 1, QStringLiteral("2"),
                    QStringLiteral(
                        "Experimental. A shot shorter than this (seconds, 0.5-30, default 2) keeps the previous scale, "
                        "so rapid cutting never makes the strength flutter. (Key: intent_motion_min_shot_s.)"));
                wireIntentToggle(motion, { policyBox, motionMin, motionShot });
            }

            // Ambient adaptation.
            {
                const auto ambient = addIntentToggle(QStringLiteral("Ambient adaptation"), QStringLiteral("ambient_adapt"),
                    QStringLiteral(
                        "Experimental, default off. For rooms that are not dark theatres: VP renders to the black "
                        "the room light leaves visible (screen reflection of the room light adds to the display's "
                        "black), using the [ambient] light sensor (or, in manual mode, the lit room's lux_lit), "
                        "applied at cuts with slow time constants. It never moves code 0 or BTB. "
                        "Off: no change. (Key: ambient_adapt.)"),
                    { QStringLiteral("off"), QStringLiteral("on") }, QStringLiteral("on"));
                auto* strength = addIntentNumber(QStringLiteral("Strength"), QStringLiteral("ambient_adapt_strength"),
                    QString(), 0.0, 1.0, 2, QStringLiteral("1"), QStringLiteral(
                        "Experimental. How much of the room light's veil is rendered into the black (0-1, default 1 = "
                        "all of it; 0 changes nothing). (Key: ambient_adapt_strength.)"));
                auto* reflect = addIntentNumber(QStringLiteral("Screen reflectance"), QStringLiteral("ambient_screen_reflect"),
                    QStringLiteral("cd/m2 per lux"), 0.0, 1.0, 4, QStringLiteral("0.01"), QStringLiteral(
                        "Experimental. How much light the screen reflects back per lux of room light (0-1 cd/m2 per "
                        "lux, default 0.01). Effective black = display black + strength x lux x this. "
                        "(Key: ambient_screen_reflect.)"));
                auto* maxBlack = addIntentNumber(QStringLiteral("Maximum effective black"), QStringLiteral("ambient_max_black_nits"),
                    QStringLiteral("nits"), 0.0, 10.0, 2, QStringLiteral("1"), QStringLiteral(
                        "Experimental. The black level the adaptation never exceeds (0-10 nits, default 1). "
                        "(Key: ambient_max_black_nits.)"));
                auto* tauUp = addIntentNumber(QStringLiteral("Brighten time constant"), QStringLiteral("ambient_tau_up_s"),
                    QStringLiteral("s"), 1.0, 600.0, 1, QStringLiteral("15"), QStringLiteral(
                        "Experimental. How slowly the target follows a rising room light (seconds, 1-600, default 15). "
                        "(Key: ambient_tau_up_s.)"));
                auto* tauDown = addIntentNumber(QStringLiteral("Darken time constant"), QStringLiteral("ambient_tau_down_s"),
                    QStringLiteral("s"), 1.0, 1800.0, 1, QStringLiteral("90"), QStringLiteral(
                        "Experimental. How slowly the target follows a falling room light (seconds, 1-1800, default 90). "
                        "(Key: ambient_tau_down_s.)"));
                wireIntentToggle(ambient, { strength, reflect, maxBlack, tauUp, tauDown });
            }

            // Black protection.
            {
                const auto black = addIntentToggle(QStringLiteral("Black protection"), QStringLiteral("intent_black"),
                    QStringLiteral(
                        "Experimental, default off. Ticking it selects Report only: shadow steps lost below the "
                        "display's black are measured and shown in the stats overlay and log. The second choice "
                        "also stops the LED dimming from crushing the shadows. Black protection never moves code 0 "
                        "or BTB. Off: nothing is measured. (Key: intent_black.)"),
                    { QStringLiteral("off"), QStringLiteral("report"), QStringLiteral("led_protect") }, QStringLiteral("report"));
                black.combo->setItemText(black.combo->findData(QStringLiteral("report")), QStringLiteral("Report only"));
                black.combo->setItemText(black.combo->findData(QStringLiteral("led_protect")), QStringLiteral("Protect shadows from LED dimming"));
                QWidget* blackBox = nullptr;
                const auto syncBlack = exclusiveButtons(black.combo, QStringLiteral("intent_black"),
                    QStringLiteral("    Mode"),
                    { { "report", QStringLiteral("Report only"), QStringLiteral(
                          "Measures and reports lost shadow steps; changes nothing. (Key: intent_black.)") },
                      { "led_protect", QStringLiteral("Protect shadows from LED dimming"), QStringLiteral(
                          "Also limits how far the LED dimming may go so the shadows stay; never moves code 0 or "
                          "BTB. (Key: intent_black.)") } },
                    QStringLiteral("report"), &blackBox);
                intentSyncs->push_back(syncBlack);
                wireIntentToggle(black, { blackBox });
            }

            // Display mapping readout.
            {
                const auto display = addIntentToggle(QStringLiteral("Display mapping readout"), QStringLiteral("intent_display"),
                    QStringLiteral(
                        "Experimental, default off. Shows the scene-to-display compression and the trim in use on "
                        "the stats overlay and in the log. Changes nothing in the picture. Off: no readout. "
                        "(Key: intent_display.)"),
                    { QStringLiteral("off"), QStringLiteral("report") }, QStringLiteral("report"));
                wireIntentToggle(display, {});
            }

            // The fade model (owner decision 2026-10-17): shared by motion intent and
            // black protection's LED share; always editable (it changes nothing while both are off).
            addIntentNumber(QStringLiteral("Ease intent changes over"), QStringLiteral("intent_ease_s"),
                QStringLiteral("s"), 0.0, 5.0, 1, QStringLiteral("1.5"), QStringLiteral(
                    "Experimental. At a cut VP has seen only the first queued frames of the new shot, so motion "
                    "intent and black protection's LED share apply a provisional value there (or keep the previous "
                    "one when the evidence is thin), then ease toward the better-informed value over this many "
                    "seconds as more of the shot arrives, and hold it for the rest of the shot (seconds, 0-5, "
                    "default 1.5). 0 = decide once at the cut and change instantly. Never raises dejudder_strength. "
                    "(Key: intent_ease_s.)"));

            auto* intentRefresh = new QAction(contentMode);
            intentRefresh->setObjectName(controlName(sectionPrefix, QStringLiteral("intent_adaptive_refresh")));
            QObject::connect(intentRefresh, &QAction::triggered, receiver, [intentSyncs]() {
                for (const auto& step : *intentSyncs) step();
            });
            // Detail pulse stabilisation and texture restore (docs/VP-detail-restore.md, 2026-10-17):
            // two picture stages that run before scaling, after the VP debander. Each has its own
            // default (named in its tip); the caps are greyed while their stage is off.
            auto* detailPulse = addChoice(QStringLiteral("Detail pulse stabilise"), QStringLiteral("detail_pulse_stabilise"),
                { QStringLiteral("on"), QStringLiteral("off") });
            detailPulse->setItemText(detailPulse->findData(QStringLiteral("on")), QStringLiteral("On"));
            detailPulse->setItemText(detailPulse->findData(QStringLiteral("off")), QStringLiteral("Off (default)"));
            detailPulse->setToolTip(QStringLiteral(
                "On (explicit choice; default is off): a starved stream codes some frames finely and others coarsely, so the fine detail and "
                "the grain \"breathe\" with the group of pictures. This scales each frame's own fine detail up "
                "towards the level of its best-coded recent neighbour (never beyond the cap below; a picture with "
                "steady detail is left exactly as it is). Nothing is copied or averaged between frames: every frame "
                "keeps its own grain. Cuts, fades and motion the player cannot follow restart it. Runs before "
                "scaling and never changes source-resolution detection. Off: the stage is not run at all. "
                "(Key: detail_pulse_stabilise.)"));
            MarkGpuCost(form, detailPulse, 1);
            auto* detailPulseCap = addText(QStringLiteral("    Detail pulse: strongest lift"), QStringLiteral("detail_pulse_cap"),
                QStringLiteral("x"));
            detailPulseCap->setValidator(new QDoubleValidator(1.0, 1.5, 2, detailPulseCap));
            clampNumberFieldOnLeave(detailPulseCap, 1.0, 1.5, 2);
            snapNumberFieldToStep(detailPulseCap, 1.0, 1.5, 0.05);
            detailPulseCap->setPlaceholderText(QStringLiteral("1.35"));
            detailPulseCap->setAccessibleDescription(QStringLiteral("Value in x, 1.00-1.50 in steps of 0.05."));
            detailPulseCap->setToolTip(QStringLiteral(
                "The most a frame's fine detail is raised towards its best neighbour, as a multiple (1.00-1.50 in "
                "steps of 0.05, default 1.35). 1.00 = no lift. It only ever raises detail the frame already has. "
                "(Key: detail_pulse_cap.)"));
            auto* detailPulseFast = addChoice(QStringLiteral("    Detail pulse: fast path"), QStringLiteral("detail_pulse_fast"),
                { QStringLiteral("on"), QStringLiteral("off") });
            detailPulseFast->setItemText(detailPulseFast->findData(QStringLiteral("on")), QStringLiteral("On (default)"));
            detailPulseFast->setItemText(detailPulseFast->findData(QStringLiteral("off")), QStringLiteral("Off (original passes)"));
            detailPulseFast->setToolTip(QStringLiteral(
                "On (default): the detail pulse runs in about half the GPU time (a separate low-band pass computed "
                "once per picture, fewer texture reads, and areas left exactly as they are only copied). The "
                "picture is the same: identical to the original passes except where a pixel can land one 16-bit "
                "storage step apart from another rounding of the same sums. Off: the original passes, for "
                "comparison. (Key: detail_pulse_fast.)"));
            auto* textureRestore = addChoice(QStringLiteral("Texture restore (experimental)"), QStringLiteral("texture_restore"),
                { QStringLiteral("off"), QStringLiteral("statistical") });
            textureRestore->setItemText(textureRestore->findData(QStringLiteral("off")), QStringLiteral("Off (default)"));
            textureRestore->setItemText(textureRestore->findData(QStringLiteral("statistical")), QStringLiteral("Statistical (experimental)"));
            textureRestore->setToolTip(QStringLiteral(
                "Off (default). Statistical: repairs the picture towards the original where the stream starved it. "
                "Per area VP separates the grain from the real scene texture, then scales this frame's own grain and "
                "texture up towards the best-coded recent frame (never beyond the cap below). Grain is kept per frame "
                "and never averaged or smoothed away; nothing is synthesised, so a frame with no grain left gets none. "
                "Texture is only restored where the picture is in focus: out-of-focus areas never gain detail. "
                "Runs before scaling and never changes source-resolution detection. (Key: texture_restore.)"));
            MarkGpuCost(form, textureRestore, 2);
            auto* textureRestoreCap = addText(QStringLiteral("    Texture restore: strongest lift"), QStringLiteral("texture_restore_cap"),
                QStringLiteral("x"));
            textureRestoreCap->setValidator(new QDoubleValidator(1.0, 2.0, 2, textureRestoreCap));
            clampNumberFieldOnLeave(textureRestoreCap, 1.0, 2.0, 2);
            snapNumberFieldToStep(textureRestoreCap, 1.0, 2.0, 0.05);
            textureRestoreCap->setPlaceholderText(QStringLiteral("1.50"));
            textureRestoreCap->setAccessibleDescription(QStringLiteral("Value in x, 1.00-2.00 in steps of 0.05."));
            textureRestoreCap->setToolTip(QStringLiteral(
                "The most grain or in-focus texture is raised towards the reference, as a multiple (1.00-2.00 in "
                "steps of 0.05, default 1.50). 1.00 = no lift. It only scales what the frame already has. "
                "(Key: texture_restore_cap.)"));
            {
                // The caps do nothing while their stage is off (read from the effective values).
                auto syncDetailCaps = [detailPulse, detailPulseCap, detailPulseFast, textureRestore, textureRestoreCap]() {
                    const bool pulseOn = effectiveChoiceValue(detailPulse) != QStringLiteral("off");
                    detailPulseCap->setProperty("greyedOff", !pulseOn);
                    detailPulseCap->setEnabled(pulseOn);
                    detailPulseFast->setProperty("greyedOff", !pulseOn);
                    detailPulseFast->setEnabled(pulseOn);
                    const bool restoreOn = effectiveChoiceValue(textureRestore) == QStringLiteral("statistical");
                    textureRestoreCap->setProperty("greyedOff", !restoreOn);
                    textureRestoreCap->setEnabled(restoreOn);
                };
                QObject::connect(detailPulse, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [syncDetailCaps](int) { syncDetailCaps(); });
                QObject::connect(textureRestore, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [syncDetailCaps](int) { syncDetailCaps(); });
                auto* detailRefresh = new QAction(detailPulse);
                detailRefresh->setObjectName(controlName(sectionPrefix, QStringLiteral("detail_restore_refresh")));
                QObject::connect(detailRefresh, &QAction::triggered, receiver, [syncDetailCaps]() { syncDetailCaps(); });
                syncDetailCaps();
            }
        }

        form = addCollapsibleSection(QStringLiteral("temporalDither"), QStringLiteral("Temporal dither"),
            // audit 2026-10-16: VP's temporal choice loads the sigma-delta itself.
            QStringLiteral("Which dither runs, and how VP's motion-aware dither (sigma-delta) uses the de-judder's motion. "
                "Choosing VP's dither loads it whatever the shader list says. ") +
            QString::fromLatin1(kGpuCostLegend), false);
        // Owner 2026-10-16: the dither as one choice - Off, libplacebo's, or VP's temporal sigma-delta
        // (mutually exclusive buttons; the drop-down holds the value). Not set: as before, the shader
        // list and the Dithering setting decide.
        auto* temporalDither = addChoice(QStringLiteral("Dither"), QStringLiteral("temporal_dither"),
            { QStringLiteral("auto"), QStringLiteral("off"), QStringLiteral("libplacebo"), QStringLiteral("sigma_delta") });
        // One "Not set" (the unspecified item); an explicit auto is Auto (audit 2026-10-17).
        temporalDither->setItemText(temporalDither->findData(QStringLiteral("auto")), QStringLiteral("Auto (the shader list decides)"));
        temporalDither->setItemText(temporalDither->findData(QStringLiteral("off")), QStringLiteral("Off"));
        temporalDither->setItemText(temporalDither->findData(QStringLiteral("libplacebo")), QStringLiteral("libplacebo"));
        temporalDither->setItemText(temporalDither->findData(QStringLiteral("sigma_delta")), QStringLiteral("VP's motion-aware dither (sigma-delta)"));   // audit 2026-10-16
        QWidget* ditherButtons = new QWidget;
        ditherButtons->setObjectName(controlName(sectionPrefix, QStringLiteral("temporal_dither.buttons")));
        {
            auto* layout = new QVBoxLayout(ditherButtons);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* row = new QHBoxLayout;
            auto* group = new QButtonGroup(ditherButtons);
            group->setExclusive(true);
            struct Choice { const char* value; const char* label; const char* tip; };
            static const Choice choices[] = {
                { "auto", "Auto", "As before: the shader list and the Dithering setting decide." },
                { "off", "Off", "No dither at all (libplacebo's off, the sigma-delta not loaded)." },
                { "libplacebo", "libplacebo", "libplacebo's own dither, with the Dithering method on the Processing section "
                    "(Auto = blue noise; Off there = none). The sigma-delta is left out even if the shader list selects it." },
                { "sigma_delta", "VP's motion-aware dither (sigma-delta)", "VP's temporal sigma-delta, loaded by VP whatever the shader "
                    "list says; libplacebo's dither is off while it runs. It uses the de-judder's motion when present and "
                    "searches itself otherwise." } };
            std::vector<QRadioButton*> radios;
            for (const Choice& c : choices)
            {
                auto* radio = new QRadioButton(QString::fromLatin1(c.label));
                radio->setObjectName(controlName(sectionPrefix, QStringLiteral("temporal_dither.") + QString::fromLatin1(c.value)));
                radio->setToolTip(QString::fromLatin1(c.tip));
                group->addButton(radio);
                row->addWidget(radio);
                radios.push_back(radio);
                const QString value = QString::fromLatin1(c.value);
                // clicked, not toggled: a click on the button shown for an inherited value writes it.
                QObject::connect(radio, &QRadioButton::clicked, receiver, [temporalDither, value]() {
                    const int index = temporalDither->findData(value);
                    if (index >= 0 && temporalDither->currentIndex() != index) temporalDither->setCurrentIndex(index);
                });
            }
            QToolButton* unset = addChoiceUnsetButton(temporalDither, row, receiver);
            row->addStretch(1);
            layout->addLayout(row);
            auto* note = new QLabel;
            note->setObjectName(controlName(sectionPrefix, QStringLiteral("temporal_dither.note")));
            note->setWordWrap(true);
            layout->addWidget(note);
            auto sync = [temporalDither, radios, group, note, unset]() {
                const QString v = effectiveChoiceValue(temporalDither);
                group->setExclusive(false);
                for (QRadioButton* r : radios)
                {
                    const QSignalBlocker blocker(r);
                    r->setChecked(false);
                }
                group->setExclusive(true);
                const char* values[] = { "auto", "off", "libplacebo", "sigma_delta" };
                bool set = false;
                for (int i = 0; i < 4; ++i)
                    if (v == QString::fromLatin1(values[i]))
                    {
                        const QSignalBlocker blocker(radios[static_cast<size_t>(i)]);
                        radios[static_cast<size_t>(i)]->setChecked(true);
                        set = true;
                    }
                const bool autoMode = !set || v == QStringLiteral("auto");
                note->setText(autoMode ? QStringLiteral("Auto: the shader list and the Dithering setting decide (as before).") : QString());
                note->setVisible(autoMode);
                markChoiceButtons(temporalDither, QList<QRadioButton*>(radios.begin(), radios.end()), unset);
            };
            QObject::connect(temporalDither, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [sync](int) { sync(); });
            auto* refresh = new QAction(ditherButtons);
            refresh->setObjectName(controlName(sectionPrefix, QStringLiteral("temporal_dither_refresh")));
            QObject::connect(refresh, &QAction::triggered, receiver, [sync]() { sync(); });
            sync();
        }
        form->addRow(QStringLiteral("Dither"), ditherButtons);
        form->setRowVisible(temporalDither, false);
        auto* occlusionMask = addChoice(QStringLiteral("Occlusion mask for the dither"),
            QStringLiteral("occlusion_mask"), { QStringLiteral("on"), QStringLiteral("off") });
        occlusionMask->setToolTip(QStringLiteral(
            "Tells the temporal sigma-delta where new or uncovered picture content appears, so it "
            "does not carry its sub-code residual into it. Works with the de-judder off too "
            "(the motion matcher then runs for the mask alone). Needs the temporal sigma-delta: Dither set to VP's "
            "motion-aware dither, or the sigma-delta shader selected in the shader list."));   // audit 2026-10-16
        // WP-24 (draft, default off until its GPU byte-identity check passes): captured repeats drawn from
        // the rendered-picture cache with the OUTPUT stage only. The renderer forces it off in the Original
        // pathway and with output.presentation switched off.
        auto* repeatReuse = addChoice(QStringLiteral("Reuse the picture on captured repeats (draft)"),
            QStringLiteral("repeat_render_reuse"), { QStringLiteral("on"), QStringLiteral("off") });
        repeatReuse->setToolTip(QStringLiteral(
            "Key repeat_render_reuse. When the capture repeats a frame (24p in 30, 30p in 60), draw the " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "repeat from the first present's cached picture and run only the output stage (dither keeps "
            "running), instead of rendering the whole chain again. Draft: off until its GPU check passes. "
            "Anything that would change the picture refuses the reuse (shown in Ctrl+I and Fallbacks "
            "engaged). Always off in the Original pathway and with output.presentation switched off."));
        auto* grainTolerance = addText(QStringLiteral("Dither carry on grain (1 to 8)"),
            QStringLiteral("dither_grain_tolerance_scale"));
        grainTolerance->setValidator(new QDoubleValidator(1.0, 8.0, 2, grainTolerance));
        grainTolerance->setPlaceholderText(QStringLiteral("3"));
        grainTolerance->setToolTip(QStringLiteral(
            "Where the adaptive downscale's grain map sees film grain, the temporal sigma-delta's "
            "motion match is this many times more tolerant, so its 10-bit averaging survives the grain. "
            "Needs the grain map and the sigma-delta. 1 turns it off."));
        auto* ditherVectors = addChoice(QStringLiteral("Dither reuses the de-judder's motion"), QStringLiteral("dither_vectors"),
            { QStringLiteral("auto"), QStringLiteral("own") });
        ditherVectors->setItemText(ditherVectors->findData(QStringLiteral("auto")), QStringLiteral("De-judder's (auto)"));
        ditherVectors->setItemText(ditherVectors->findData(QStringLiteral("own")), QStringLiteral("The dither's own search"));   // audit 2026-10-16
        ditherVectors->setToolTip(QStringLiteral(
            "On de-judder extras the temporal sigma-delta can take the motion from the de-judder (exact there, and "
            "it skips its own search, about 1 ms less per extra) instead of searching itself. Needs the VP Renderer "
            "Beta (libplacebo 377); otherwise, and on every other refresh, it searches itself."));
        MarkGpuCost(form, occlusionMask, 1);
        {
            // Show only what applies (audit 2026-10-14c): a setting's own
            // values only while it is on; the de-judder's shaping and its
            // sections only with a strength set. Unset = the renderer default.
            auto isOn = [](QComboBox* combo, bool byDefault)
            {
                const QString chosen = combo->currentData().toString().trimmed().toLower();
                return chosen == QStringLiteral("on") ? true : chosen == QStringLiteral("off") ? false : byDefault;
            };
            auto number = [](QLineEdit* edit)
            {
                bool ok = false;
                const double parsed = edit->text().trimmed().toDouble(&ok);
                return ok ? parsed : 0.0;
            };
            QWidget* estimationSection = dejudderEstimationForm->parentWidget();
            QWidget* experimentsSection = dejudderExperimentsForm->parentWidget();
            auto update = [=]()
            {
                const bool active = number(dejudderStrength) > 0.0 || number(strengthEven) > 0.0;
                // VP-V organisation pass 2 (2026-10-24, src/VP-V/config/VpvOrganisation.h): the VP-V page may
                // move a row to a form of its own; show or hide it in the form that holds it now. The row's
                // label, when VP-V stacked it above the field, goes with it (property vpvRowLabel).
                auto showIn = [](QFormLayout* layout, QWidget* row, bool visible) {
                    if (auto* current = row->parentWidget() ? qobject_cast<QFormLayout*>(row->parentWidget()->layout()) : nullptr)
                        layout = current;
                    layout->setRowVisible(row, visible);
                    if (auto* label = qobject_cast<QWidget*>(row->property("vpvRowLabel").value<QObject*>()))
                        layout->setRowVisible(label, visible);
                };
                showIn(dejudderTasteForm, evenRates, number(strengthEven) > 0.0);
                for (QWidget* row : { static_cast<QWidget*>(rolloff), static_cast<QWidget*>(objectStrength),
                        static_cast<QWidget*>(fadeMatch) })
                    showIn(dejudderTasteForm, row, active);
                const bool roll = active && isOn(rolloff, false);
                for (QWidget* row : { rollLo, rollHi, rollFloor })
                    showIn(dejudderTasteForm, row, roll);
                showIn(dejudderTasteForm, fadeFloor, active && isOn(fadeMatch, true));
                showIn(dejudderEstimationForm, estSplitSad, isOn(estSplit, false));
                showIn(dejudderEstimationForm, estLambda, isOn(estCandidates, true));
                showIn(dejudderEstimationForm, estGate, isOn(globalPrior, true));
                showIn(dejudderEstimationForm, cutHistogram, isOn(regional, true));
                showIn(dejudderEstimationForm, pixelLo, isOn(pixelTrust, true));
                showIn(dejudderEstimationForm, pixelHi, isOn(pixelTrust, true));
                showIn(dejudderEstimationForm, landing, isOn(pixelTrust, true));
                showIn(dejudderEstimationForm, verifyShare, isOn(verify, false));
                showIn(dejudderExperimentsForm, taperSad, isOn(taper, false));
                // Filling uncovered areas works on the source path only (W23-CFG-M1: honour inherited value).
                const bool sourcePath = effectiveChoiceValue(warpStage) == QStringLiteral("source");
                occlusionFill->setProperty("greyedOff", !sourcePath);
                occlusionFill->setEnabled(sourcePath);
                // Estimation and experiments matter only with a strength.
                if (estimationSection) estimationSection->setEnabled(active);
                if (experimentsSection) experimentsSection->setEnabled(active);
                // A "from" at or above its "to" is flagged as it is typed
                // (audit 2026-10-14c); empty fields use the defaults.
                auto pairCheck = [](QLineEdit* lo, QLineEdit* hi, double loDefault, double hiDefault)
                {
                    bool okLo = false, okHi = false;
                    const double a = lo->text().trimmed().toDouble(&okLo), b = hi->text().trimmed().toDouble(&okHi);
                    const bool wrong = (okLo || okHi) && (okLo ? a : loDefault) >= (okHi ? b : hiDefault) &&
                        (okHi ? b : hiDefault) > 0.0;
                    if (hi->property("vpvPairError").toBool() != wrong)
                    {
                        hi->setProperty("vpvPairError", wrong);
                        if (QStyle* s = hi->style())
                        {
                            s->unpolish(hi);
                            s->polish(hi);
                        }
                    }
                    hi->setProperty("pairWarning", wrong);
                    if (wrong)
                        hi->setToolTip(hi->property("settingHelp").toString() +
                            QStringLiteral("\nThis must be above the value before it."));
                };
                pairCheck(motionLo, motionHi, 0.25, 1.5);
                pairCheck(rollLo, rollHi, 8.0, 24.0);
                pairCheck(pixelLo, pixelHi, 3.0, 8.0);
            };
            for (QLineEdit* edit : { motionLo, motionHi, rollLo, rollHi, pixelLo, pixelHi })
                QObject::connect(edit, &QLineEdit::textChanged, receiver, [update](const QString&) { update(); });
            for (QLineEdit* edit : { dejudderStrength, strengthEven })
                QObject::connect(edit, &QLineEdit::textChanged, receiver, [update](const QString&) { update(); });
            for (QComboBox* combo : { rolloff, fadeMatch, estSplit, estCandidates, globalPrior, regional, pixelTrust,
                    verify, taper, warpStage })
                QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [update](int) { update(); });
            update();
            // Every switch says its default (audit 2026-10-14c).
            const std::pair<QComboBox*, const char*> switchDefaults[] = {
                { rolloff, "Default off." }, { fadeMatch, "Default on." }, { estCandidates, "Default on." },
                { estQpel, "Default on." }, { estStillZero, "Default on." }, { estGate, "Default on." },
                { estSplit, "Default off: with the 4.5 match tolerance and pixel-by-pixel decisions, splitting cost "
                    "de-judder benefit on real footage." },
                { globalTwo, "Default on." }, { regional, "Default on." }, { pixelTrust, "Default on." },
                { verify, "Default off." }, { estimateAhead, "Default on." }, { taper, "Default off." },
                { lowRes, "Default: half size, every step." }, { warpStage, "Default: rendered picture." } };
            for (const auto& entry : switchDefaults)
                if (!entry.first->toolTip().contains(QStringLiteral("Default")))
                    entry.first->setToolTip(entry.first->toolTip() + QStringLiteral("\n") + QString::fromLatin1(entry.second));
            occlusionFill->setToolTip(occlusionFill->toolTip() +
                QStringLiteral("\nAvailable only with Extra frames from: Source (in this section)."));
        }

        // Stage 8-2: the adaptive downscale blend (SSIM / faithful).
        form = addCollapsibleSection(QStringLiteral("adaptiveDownscale"), QStringLiteral("Adaptive downscale"),
            QString::fromLatin1(kGpuCostLegend), false);
        auto* adaptiveOn = addChoice(QStringLiteral("SSIM / faithful blend"),
            QStringLiteral("adaptive_downscale"), { QStringLiteral("on"), QStringLiteral("off") });
        adaptiveOn->setToolTip(QStringLiteral(
            "Blends the SSimDownscaler shader with a faithful ewa_lanczossharp downscale, luma only: hard "
            "edges and grain lean faithful, texture keeps SSIM. Needs the SSimDownscaler shader enabled; "
            "acts only when the picture is downscaled. Off keeps plain SSIM."));
        struct AdaptiveField { const char* label; const char* key; double lo, hi; const char* placeholder; const char* tip; };
        const AdaptiveField adaptiveFields[] = {
            { "SSIM weight (0 to 1)", "adaptive_w_base", 0.0, 1.0, "1", "How much SSIM where nothing argues against it. The hybrid scaler's design value is 0.2 (the VP-V Scaling tiers set it); 1 = plain SSIM." },
            { "Edge start (codes)", "adaptive_edge_lo", 0.0, 255.0, "8", "Gradient (8-bit codes) where hard edges start leaning faithful." },
            { "Edge full (codes)", "adaptive_edge_hi", 0.0, 255.0, "40", "Gradient where an edge is fully faithful (halo control)." },
            { "Grain start (codes)", "adaptive_grain_lo", 0.0, 64.0, "2", "Flat-area grain energy where the SSIM weight starts to drop." },
            { "Grain full (codes)", "adaptive_grain_hi", 0.0, 64.0, "8", "Grain energy at which the SSIM weight reaches the grain floor." },
            { "Grain floor (0 to 1)", "adaptive_grain_floor", 0.0, 1.0, "0.4", "The SSIM weight left on a heavily grained source." },
            { "Grain smoothing (ms)", "adaptive_grain_smooth_ms", 0.0, 10000.0, "1000", "Time constant of the per-frame grain measure; a cut snaps it." },
            { "Shadow bias (0 to 1)", "adaptive_grain_shadow_bias", 0.0, 1.0, "0.5", "Weights grain in the shadows more (the LED compensation lifts them)." } };
        std::vector<QWidget*> adaptiveRows;
        for (const AdaptiveField& field : adaptiveFields)
        {
            auto* edit = addText(QString::fromLatin1(field.label), QString::fromLatin1(field.key));
            adaptiveRows.push_back(edit);
            edit->setValidator(new QDoubleValidator(field.lo, field.hi, 3, edit));
            edit->setPlaceholderText(QString::fromLatin1(field.placeholder));
            edit->setToolTip(QString::fromLatin1(field.tip));
        }
        auto* adaptiveChroma = addChoice(QStringLiteral("Blend chroma too"),
            QStringLiteral("adaptive_chroma_ssim"), { QStringLiteral("off"), QStringLiteral("on") });
        adaptiveChroma->setToolTip(QStringLiteral(
            "Off (recommended): chroma stays faithful; SSIM on chroma can fringe colour at edges."));
        auto* adaptiveFaithful = addChoice(QStringLiteral("Grain-true downscale filter"),
            QStringLiteral("adaptive_faithful"), { QStringLiteral("polar"), QStringLiteral("ortho"), QStringLiteral("auto") });
        // "polar" is retired (runs as Auto, RetiredSettings.h); the hidden value holder keeps it
        // only so a file that still says polar round-trips unchanged.
        adaptiveFaithful->setItemText(adaptiveFaithful->findData(QStringLiteral("polar")),
            QStringLiteral("Polar (retired: runs as Auto)"));
        adaptiveFaithful->setItemText(adaptiveFaithful->findData(QStringLiteral("auto")),
            QStringLiteral("Auto (polar; separable when only one axis shrinks)"));
        adaptiveFaithful->setItemText(adaptiveFaithful->findData(QStringLiteral("ortho")),
            QStringLiteral("Separable Lanczos (faster)"));
        adaptiveFaithful->setToolTip(QStringLiteral(
            "The filter of the plain, grain-true downscale the blend leans to on grainy areas. Separable "
            "Lanczos keeps the grain's character at about a quarter of the polar filter's GPU time."));
        MarkGpuCost(form, adaptiveFaithful, 2);
        // Owner 2026-10-16: the filter as two mutually exclusive buttons - Auto (polar where both axes
        // shrink alike, the 1D separable filter when only one does) or 1D (separable always). The
        // drop-down stays as the value holder (hidden); a saved "polar" shows as Auto.
        auto* faithfulButtons = new QWidget;
        faithfulButtons->setObjectName(controlName(sectionPrefix, QStringLiteral("adaptive_faithful.buttons")));
        auto* faithfulButtonsLayout = new QHBoxLayout(faithfulButtons);
        faithfulButtonsLayout->setContentsMargins(0, 0, 0, 0);
        auto* faithfulAuto = new QRadioButton(QStringLiteral("Auto (polar where able)"));
        faithfulAuto->setObjectName(controlName(sectionPrefix, QStringLiteral("adaptive_faithful.auto")));
        faithfulAuto->setToolTip(QStringLiteral(
            "Polar EWA Lanczos sharp where both axes shrink alike (the most accurate; about 2.4 ms more than 1D on the "
            "laptop, the full blend effect); the 1D separable filter when only one axis shrinks, where a polar "
            "filter would soften the other."));
        auto* faithfulOneD = new QRadioButton(QStringLiteral("1D (separable, faster)"));
        faithfulOneD->setObjectName(controlName(sectionPrefix, QStringLiteral("adaptive_faithful.one_d")));
        faithfulOneD->setToolTip(QStringLiteral(
            "Two separable Lanczos passes always: about 2.4 ms cheaper than polar, keeps about 82% of the blend's effect."));
        auto* faithfulGroup = new QButtonGroup(faithfulButtons);
        faithfulGroup->setExclusive(true);
        faithfulGroup->addButton(faithfulAuto);
        faithfulGroup->addButton(faithfulOneD);
        faithfulButtonsLayout->addWidget(faithfulAuto);
        faithfulButtonsLayout->addWidget(faithfulOneD);
        QToolButton* faithfulUnset = addChoiceUnsetButton(adaptiveFaithful, faithfulButtonsLayout, receiver);
        faithfulButtonsLayout->addStretch(1);
        form->addRow(QStringLiteral("Grain-true downscale filter"), faithfulButtons);
        form->setRowVisible(adaptiveFaithful, false);
        {
            auto syncButtons = [adaptiveFaithful, faithfulAuto, faithfulOneD, faithfulUnset]() {
                const QSignalBlocker a(faithfulAuto), b(faithfulOneD);
                const bool oneD = effectiveChoiceValue(adaptiveFaithful) == QStringLiteral("ortho");
                faithfulOneD->setChecked(oneD);
                faithfulAuto->setChecked(!oneD);
                markChoiceButtons(adaptiveFaithful, { faithfulAuto, faithfulOneD }, faithfulUnset);
            };
            QObject::connect(adaptiveFaithful, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [syncButtons](int) { syncButtons(); });
            auto choose = [adaptiveFaithful](const QString& value) {
                const int index = adaptiveFaithful->findData(value);
                if (index >= 0 && adaptiveFaithful->currentIndex() != index) adaptiveFaithful->setCurrentIndex(index);
            };
            QObject::connect(faithfulAuto, &QRadioButton::clicked, receiver, [choose]() { choose(QStringLiteral("auto")); });
            QObject::connect(faithfulOneD, &QRadioButton::clicked, receiver, [choose]() { choose(QStringLiteral("ortho")); });
            // Profile loads block the drop-down's signals: the load triggers this.
            auto* faithfulRefresh = new QAction(faithfulButtons);
            faithfulRefresh->setObjectName(controlName(sectionPrefix, QStringLiteral("adaptive_faithful_refresh")));
            QObject::connect(faithfulRefresh, &QAction::triggered, receiver, [syncButtons]() { syncButtons(); });
            syncButtons();
        }
        // Stage 8-3: the per-block grain map from the de-judder's vectors.
        auto* adaptiveMap = addChoice(QStringLiteral("Grain map from motion"),
            QStringLiteral("adaptive_grain_map"), { QStringLiteral("on"), QStringLiteral("off") });
        adaptiveMap->setToolTip(QStringLiteral(
            "On: grain is measured per block from frame-to-frame noise along the motion vectors, so grainy "
            "areas lean faithful while clean texture keeps SSIM. Film rates (30 fps or less) with a next frame " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "queued, using the de-judder's motion vectors when they exist (it never starts the motion search on its "
            "own). Otherwise (de-judder off, the low-latency queue, 50/60p) the per-frame measure is used. Off: the " // GENERAL-REF: UI help/label string only (no hard-coded rate/size logic)
            "per-frame measure only."));
        auto* adaptiveMapSmooth = addText(QStringLiteral("Grain map smoothing (ms)"),
            QStringLiteral("adaptive_grain_map_smooth_ms"));
        adaptiveMapSmooth->setValidator(new QDoubleValidator(0.0, 10000.0, 3, adaptiveMapSmooth));
        adaptiveMapSmooth->setPlaceholderText(QStringLiteral("500"));
        adaptiveMapSmooth->setToolTip(QStringLiteral(
            "Time constant of the per-block grain map (it follows the content along the vectors); a cut resets it."));
        // VP-V scaling.output (owner 2026-10-31): the motion-adapted blend, a candidate (off by default).
        // Continuum 0.0..1.0 (off | on | [0.0, 1.0]): hidden text control holds the saved value while the
        // visible composite control offers Off / On (1.0) / Custom strength (0.0-1.0).
        auto* adaptiveMotion = addText(QStringLiteral("Motion-adapted blend (candidate)"),
            QStringLiteral("adaptive_motion"));
        const QString adaptiveMotionTip = QStringLiteral(
            "On (1.0) or custom strength (0.0 to 1.0): the sharper scaler (SSIM down, the CNN up) is kept on steady, "
            "confidently tracked areas and eased toward the faithful scaler on fast or uncertain motion, smoothed over "
            "time so motion edges do not flicker. Uses the de-judder's motion vectors (asks for them like the grain map); "
            "without vectors the plain blend runs and the status says so. Applies to the downscale and the upscale blend.");
        adaptiveMotion->setToolTip(adaptiveMotionTip);
        auto* adaptiveMotionControls = new QWidget;
        adaptiveMotionControls->setObjectName(controlName(sectionPrefix, QStringLiteral("adaptive_motion.controls")));
        adaptiveMotionControls->setToolTip(adaptiveMotionTip);
        auto* adaptiveMotionLayout = new QHBoxLayout(adaptiveMotionControls);
        adaptiveMotionLayout->setContentsMargins(0, 0, 0, 0);
        auto* adaptiveMotionMode = new QComboBox(adaptiveMotionControls);
        adaptiveMotionMode->setObjectName(controlName(sectionPrefix, QStringLiteral("adaptive_motion.mode")));
        adaptiveMotionMode->addItem(QStringLiteral("Default (off)"), QString());
        adaptiveMotionMode->addItem(QStringLiteral("Off"), QStringLiteral("off"));
        adaptiveMotionMode->addItem(QStringLiteral("On (1.0)"), QStringLiteral("on"));
        adaptiveMotionMode->addItem(QStringLiteral("Custom strength (0.0\u20131.0)"), QStringLiteral("custom"));
        adaptiveMotionMode->setToolTip(adaptiveMotionTip);
        auto* adaptiveMotionStrength = new QDoubleSpinBox(adaptiveMotionControls);
        adaptiveMotionStrength->setObjectName(controlName(sectionPrefix, QStringLiteral("adaptive_motion.strength")));
        adaptiveMotionStrength->setKeyboardTracking(false);
        adaptiveMotionStrength->setRange(0.0, 1.0);
        adaptiveMotionStrength->setDecimals(2);
        adaptiveMotionStrength->setSingleStep(0.05);
        adaptiveMotionStrength->setValue(0.50);
        adaptiveMotionStrength->setToolTip(QStringLiteral(
            "Continuous motion-adapted blend strength from 0.0 (off) to 1.0 (full)."));
        adaptiveMotionLayout->addWidget(adaptiveMotionMode);
        adaptiveMotionLayout->addWidget(adaptiveMotionStrength);
        adaptiveMotionLayout->addStretch(1);
        form->addRow(QStringLiteral("Motion-adapted blend (candidate)"), adaptiveMotionControls);
        form->setRowVisible(adaptiveMotion, false);
        MarkGpuCost(form, adaptiveMotionControls, 1);
        const AdaptiveField motionFields[] = {
            { "Motion: full sharp below (heights/s)", "adaptive_motion_speed_lo", 0.0, 10.0, "0.1", "Motion slower than this (picture heights per second) keeps the sharper scaler's full weight." },
            { "Motion: safe above (heights/s)", "adaptive_motion_speed_hi", 0.0, 10.0, "0.4", "Motion faster than this uses the floor below." },
            { "Motion: weight on fast motion (0 to 1)", "adaptive_motion_floor", 0.0, 1.0, "0", "The sharper scaler's share kept on fast or untracked motion (0 = the faithful scaler only)." },
            { "Motion: smoothing (ms)", "adaptive_motion_smooth_ms", 0.0, 10000.0, "250", "Time constant of the per-block weight (it follows the content along the vectors); a cut resets it." } };
        std::vector<QWidget*> motionRows;
        for (const AdaptiveField& field : motionFields)
        {
            auto* edit = addText(QString::fromLatin1(field.label), QString::fromLatin1(field.key));
            motionRows.push_back(edit);
            edit->setValidator(new QDoubleValidator(field.lo, field.hi, 3, edit));
            edit->setPlaceholderText(QString::fromLatin1(field.placeholder));
            edit->setToolTip(QString::fromLatin1(field.tip));
        }
        {
            auto parseStrength = [](const QString& raw, double& out) -> bool
            {
                bool ok = false;
                const double v = QLocale::c().toDouble(raw, &ok);
                if (!ok || !std::isfinite(v) || v < 0.0 || v > 1.0)
                    return false;
                out = v;
                return true;
            };
            auto syncAdaptiveMotion = [adaptiveMotion, adaptiveMotionMode, adaptiveMotionStrength, motionRows, parseStrength](bool forceSpinSync)
            {
                const QSignalBlocker blockMode(adaptiveMotionMode);
                const QSignalBlocker blockStrength(adaptiveMotionStrength);
                const QString raw = adaptiveMotion->text().trimmed().toLower();
                bool active = false;
                double strengthValue = 0.0;
                if (raw.isEmpty())
                {
                    adaptiveMotionMode->setCurrentIndex(adaptiveMotionMode->findData(QString()));
                    adaptiveMotionStrength->setVisible(false);
                    adaptiveMotionStrength->setEnabled(false);
                    active = false;
                }
                else if (raw == QStringLiteral("on"))
                {
                    adaptiveMotionMode->setCurrentIndex(adaptiveMotionMode->findData(QStringLiteral("on")));
                    adaptiveMotionStrength->setVisible(false);
                    adaptiveMotionStrength->setEnabled(false);
                    active = true;
                }
                else if (raw == QStringLiteral("off"))
                {
                    adaptiveMotionMode->setCurrentIndex(adaptiveMotionMode->findData(QStringLiteral("off")));
                    adaptiveMotionStrength->setVisible(false);
                    adaptiveMotionStrength->setEnabled(false);
                    active = false;
                }
                else if (parseStrength(raw, strengthValue))
                {
                    adaptiveMotionMode->setCurrentIndex(adaptiveMotionMode->findData(QStringLiteral("custom")));
                    adaptiveMotionStrength->setVisible(true);
                    adaptiveMotionStrength->setEnabled(true);
                    if (std::abs(adaptiveMotionStrength->value() - strengthValue) >= 1e-6 &&
                        (forceSpinSync || !adaptiveMotionStrength->hasFocus()))
                        adaptiveMotionStrength->setValue(strengthValue);
                    active = strengthValue > 0.0;
                }
                else
                {
                    adaptiveMotionMode->setCurrentIndex(adaptiveMotionMode->findData(QStringLiteral("off")));
                    adaptiveMotionStrength->setVisible(false);
                    adaptiveMotionStrength->setEnabled(false);
                    active = false;
                }
                for (QWidget* row : motionRows)
                {
                    row->setProperty("greyedOff", !active);
                    row->setEnabled(active);
                }
            };
            QObject::connect(adaptiveMotion, &QLineEdit::textChanged, receiver, [syncAdaptiveMotion](const QString&) {
                syncAdaptiveMotion(false);
            });
            QObject::connect(adaptiveMotionMode, qOverload<int>(&QComboBox::currentIndexChanged), receiver,
                [adaptiveMotion, adaptiveMotionMode, adaptiveMotionStrength, parseStrength, syncAdaptiveMotion](int)
            {
                const QString chosen = adaptiveMotionMode->currentData().toString();
                if (chosen.isEmpty() || chosen == QStringLiteral("off") || chosen == QStringLiteral("on"))
                {
                    if (adaptiveMotion->text() != chosen)
                        adaptiveMotion->setText(chosen);
                    else
                        syncAdaptiveMotion(false);
                }
                else if (chosen == QStringLiteral("custom"))
                {
                    const QString raw = adaptiveMotion->text().trimmed().toLower();
                    double existing = 0.0;
                    if (!parseStrength(raw, existing))
                    {
                        const double initial = (raw == QStringLiteral("on")) ? 1.0 : 0.5;
                        {
                            const QSignalBlocker blockStrength(adaptiveMotionStrength);
                            adaptiveMotionStrength->setValue(initial);
                        }
                        adaptiveMotion->setText(QString::number(adaptiveMotionStrength->value(), 'g', 4));
                    }
                    else
                        syncAdaptiveMotion(true);
                }
            });
            QObject::connect(adaptiveMotionStrength, qOverload<double>(&QDoubleSpinBox::valueChanged), receiver,
                [adaptiveMotion, adaptiveMotionMode](double val)
            {
                if (adaptiveMotionMode->currentData().toString() != QStringLiteral("custom"))
                    return;
                const QString formatted = QString::number(val, 'g', 4);
                if (adaptiveMotion->text() != formatted)
                    adaptiveMotion->setText(formatted);
            });
            auto* adaptiveMotionRefresh = new QAction(adaptiveMotionControls);
            adaptiveMotionRefresh->setObjectName(controlName(sectionPrefix, QStringLiteral("adaptive_motion_refresh")));
            QObject::connect(adaptiveMotionRefresh, &QAction::triggered, receiver, [syncAdaptiveMotion]() {
                syncAdaptiveMotion(true);
            });
            syncAdaptiveMotion(true);
        }
        MarkGpuCost(form, adaptiveOn, 2);
        MarkGpuCost(form, adaptiveMap, 1);
        {
            // Greyed while they do nothing (audit 2026-10-14c).
            adaptiveRows.push_back(adaptiveChroma);
            adaptiveRows.push_back(adaptiveFaithful);
            adaptiveRows.push_back(faithfulButtons);
            adaptiveRows.push_back(adaptiveMap);
            auto greyAdaptive = [adaptiveOn, adaptiveMap, adaptiveRows, adaptiveMapSmooth]()
            {
                const bool on = effectiveChoiceValue(adaptiveOn) == QStringLiteral("on");
                const QString map = effectiveChoiceValue(adaptiveMap);
                for (QWidget* row : adaptiveRows) { row->setProperty("greyedOff", !on); row->setEnabled(on); }
                const bool smooth = on && map != QStringLiteral("off");
                adaptiveMapSmooth->setProperty("greyedOff", !smooth);
                adaptiveMapSmooth->setEnabled(smooth);
            };
            for (QComboBox* combo : { adaptiveOn, adaptiveMap })
                QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [greyAdaptive](int) { greyAdaptive(); });
            auto* adaptiveRefresh = new QAction(adaptiveOn);
            adaptiveRefresh->setObjectName(controlName(sectionPrefix, QStringLiteral("adaptive_downscale_refresh")));
            QObject::connect(adaptiveRefresh, &QAction::triggered, receiver, [greyAdaptive]() { greyAdaptive(); });
            greyAdaptive();
        }

        // Stage 8b: the upscale blend (sources smaller than the screen).
        form = addCollapsibleSection(QStringLiteral("adaptiveUpscale"), QStringLiteral("Adaptive upscale"),
            QString::fromLatin1(kGpuCostLegend), false);
        auto* upscaleOn = addChoice(QStringLiteral("CNN / faithful blend"),
            QStringLiteral("adaptive_upscale"), { QStringLiteral("on"), QStringLiteral("off") });
        upscaleOn->setToolTip(QStringLiteral(
            "For sources shorter than the screen: where the grain measure sees film grain, the picture leans from "
            "the CNN upscaler (FSRCNNX / ArtCNN, which sharpens grain into crisper noise) toward a faithful "
            "ewa_lanczossharp upscale. Clean pictures and edges stay the CNN's. Uses the grain settings above."));
        auto* upscaleWBase = addText(QStringLiteral("CNN weight on clean pictures (0 to 1)"),
            QStringLiteral("adaptive_upscale_w_base"));
        upscaleWBase->setValidator(new QDoubleValidator(0.0, 1.0, 3, upscaleWBase));
        upscaleWBase->setPlaceholderText(QStringLiteral("1"));
        upscaleWBase->setToolTip(QStringLiteral(
            "How much of the CNN upscaler's picture is kept where there is no grain (1 = all of it; lower mixes in "
            "the plain upscale everywhere)."));
        auto* upscaleSuperres = addChoice(QStringLiteral("SuperRes strength follows grain"),
            QStringLiteral("adaptive_upscale_superres_scale"), { QStringLiteral("on"), QStringLiteral("off") });
        upscaleSuperres->setToolTip(QStringLiteral(
            "With SSimSuperRes enabled, its refinement is scaled down where there is grain, so it does not "
            "refine noise."));
        auto* upscaleLight = addChoice(QStringLiteral("Upscale light"),
            QStringLiteral("upscale_light"), { QStringLiteral("linear"), QStringLiteral("gamma") });
        upscaleLight->setItemText(upscaleLight->findData(QStringLiteral("linear")),
            QStringLiteral("linear (downscale after a doubler)"));
        upscaleLight->setItemText(upscaleLight->findData(QStringLiteral("gamma")), QStringLiteral("gamma (all)"));
        upscaleLight->setToolTip(QStringLiteral(
            "The light domain while the blend is on (both branches always share it). Linear: after a CNN doubler "
            "the picture is scaled down from 2x in linear light; without a CNN the plain upscale is in gamma light "
            "(libplacebo upscales in linear light only through sigmoid, which is kept off because it clips "
            "above-white). Gamma: gamma light everywhere, like madVR."));
        auto* upscaleRoute = addChoice(QStringLiteral("4K route pre-render upscale"),
            QStringLiteral("upscale_route"), { QStringLiteral("off"), QStringLiteral("rebased_only"), QStringLiteral("always_4k") });
        upscaleRoute->setToolTip(QStringLiteral(
            "Pre-render 4K route upscaling (scaling.route_4k): off = bypassed straight to output scaling, "
            "rebased_only = upscale only rebased frames, always_4k = upscale whenever target exceeds input."));
        auto* upscaleSharp = addChoice(QStringLiteral("Sharp upscaler branch"),
            QStringLiteral("upscale_sharp"), { QStringLiteral("lanczos3"), QStringLiteral("ewa_lanczossharp"),
            QStringLiteral("catmull_rom"), QStringLiteral("lanczos2"), QStringLiteral("lanczos4"), QStringLiteral("bicubic") });
        upscaleSharp->setToolTip(QStringLiteral(
            "Resampling kernel for the sharp branch of the hybrid upscaler (default: lanczos3)."));
        auto* upscaleSafe = addChoice(QStringLiteral("Alias-safe upscaler branch"),
            QStringLiteral("upscale_safe"), { QStringLiteral("hermite"), QStringLiteral("lanczos3"),
            QStringLiteral("catmull_rom"), QStringLiteral("ewa_lanczossharp"), QStringLiteral("bicubic"), QStringLiteral("bilinear") });
        upscaleSafe->setToolTip(QStringLiteral(
            "Resampling kernel for the alias-safe branch of the hybrid upscaler (default: hermite)."));
        auto* upscaleSharpAntiring = addText(QStringLiteral("Sharp branch anti-ringing (0 to 1)"),
            QStringLiteral("upscale_sharp_antiring"));
        upscaleSharpAntiring->setValidator(new QDoubleValidator(0.0, 1.0, 2, upscaleSharpAntiring));
        upscaleSharpAntiring->setPlaceholderText(QStringLiteral("0.5"));
        upscaleSharpAntiring->setToolTip(QStringLiteral(
            "Anti-ringing clamp strength for the sharp upscaler branch (default 0.5; 0 = off, 1 = full clamp)."));
        MarkGpuCost(form, upscaleOn, 2);
        {
            auto greyUpscale = [upscaleOn, upscaleWBase, upscaleSuperres, upscaleLight]()
            {
                const bool on = effectiveChoiceValue(upscaleOn) == QStringLiteral("on");
                for (QWidget* row : { static_cast<QWidget*>(upscaleWBase), static_cast<QWidget*>(upscaleSuperres),
                        static_cast<QWidget*>(upscaleLight) })
                {
                    row->setProperty("greyedOff", !on);
                    row->setEnabled(on);
                }
            };
            QObject::connect(upscaleOn, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [greyUpscale](int) { greyUpscale(); });
            auto* upscaleRefresh = new QAction(upscaleOn);
            upscaleRefresh->setObjectName(controlName(sectionPrefix, QStringLiteral("adaptive_upscale_refresh")));
            QObject::connect(upscaleRefresh, &QAction::triggered, receiver, [greyUpscale]() { greyUpscale(); });
            greyUpscale();
        }

        RenderingSectionHandles handles;
        handles.capturePath = capturePath;
        handles.sourceRange = sourceRange;
        handles.pictureBox = pictureBox;
        handles.sourceResolution = sourceResolution;
        handles.autoConfidence = autoConfidence;
        handles.evidenceSeconds = evidenceSeconds;
        handles.evidenceShots = evidenceShots;
        handles.textGuard = textGuard;
        handles.mixedGuard = mixedGuard;
        handles.kernelMatch = kernelMatch;
        handles.debandVp = debandVp;
        handles.lowBitrate = lowBitrate;
        handles.ssimStabilise = ssimStabilise;
        handles.bitrateAware = bitrateAware;
        handles.bitrateFloor = bitrateFloor;
        handles.cnnStabilise = sharpStabilise;
        return handles;
    }

    void AddVpvProcessingAndReorganise(const RenderingSectionKit& kit, const RenderingSectionHandles& h)
    {
        QFormLayout*& form = *kit.form;
        QObject* const receiver = kit.receiver;
        QWidget* const profileFields = kit.profileFields;
        QVBoxLayout* const profileFieldsLayout = kit.profileFieldsLayout;
        const auto& addCollapsibleSection = kit.addCollapsibleSection;
        auto addText = [&](const QString& label, const QString& key,
            const QString& unit = {}, double displayScale = 1.0) -> QLineEdit* {
            return kit.addText(label, key, unit, displayScale);
        };
        auto addChoice = [&](const QString& label, const QString& key,
            const QStringList& choices, bool allowInherited = true) -> QComboBox* {
            return kit.addChoice(label, key, choices, allowInherited);
        };
        QComboBox* const capturePath = h.capturePath;
        QComboBox* const sourceRange = h.sourceRange;
        QWidget* const pictureBox = h.pictureBox;
        QComboBox* const sourceResolution = h.sourceResolution;
        QLineEdit* const autoConfidence = h.autoConfidence;
        QLineEdit* const evidenceSeconds = h.evidenceSeconds;
        QLineEdit* const evidenceShots = h.evidenceShots;
        QComboBox* const textGuard = h.textGuard;
        QComboBox* const mixedGuard = h.mixedGuard;
        QComboBox* const kernelMatch = h.kernelMatch;
        QComboBox* const debandVp = h.debandVp;
        QComboBox* const lowBitrate = h.lowBitrate;
        QComboBox* const ssimStabilise = h.ssimStabilise;
        QComboBox* const bitrateAware = h.bitrateAware;
        QLineEdit* const bitrateFloor = h.bitrateFloor;
        QComboBox* const cnnStabilise = h.cnnStabilise;
        form = addCollapsibleSection(QStringLiteral("processing"), QStringLiteral("Processing"),
            QString(), false);
        QFormLayout* processingForm = form;
        // Debanding moved to Scaling / Quality (Bill, v1.4.02); its VP deband
        // note is applied there.
        // Film grain (owner 2026-10-17): libplacebo's deband adds no grain by default.
        auto* debandGrain = addChoice(QStringLiteral("Debanding adds grain (libplacebo)"),
            QStringLiteral("deband_grain"), { QStringLiteral("off"), QStringLiteral("on") });
        debandGrain->setToolTip(QStringLiteral(
            "Off (default): libplacebo's debanding adds no synthetic grain - VP's dither does that job and the "
            "film's own grain is left as it is. On: libplacebo's grain is added as before 2026-10-17."));
        auto* extendedRange = addChoice(QStringLiteral("Keep below-black and above-white (SDR)"),
            QStringLiteral("preserve_extended_range"), { QStringLiteral("on"), QStringLiteral("off") });
        extendedRange->setToolTip(QStringLiteral(
            "Keeps SDR picture detail below video black and above video white through scaling and "
            "the colour conversion, which otherwise clip it. Needs pure-power gammas (1.8 to 2.8) on "
            "the source and the output, a BT.709 source and no calibration LUT; it turns itself off "
            "otherwise and for HDR. On by default (owner: keep below-black and above-white). To reach the "
            "projector they also need limited output range (Color profile: output range limited, with output "
            "gamma 2.4). Off: libplacebo's standard path (clips them). Works with deinterlacing; libplacebo smooth motion "
            "clips below-black on blended refreshes (libplacebo limitation) - VP's own motion engine keeps both."));
        MarkGpuCost(form, extendedRange, 1);
        auto* extendedMore = addChoice(QStringLiteral("Below-black / above-white: also BT.1886, sRGB, wide-gamut SDR, LUT, HDR"),
            QStringLiteral("extended_range_more_cases"), { QStringLiteral("on"), QStringLiteral("off") });
        extendedMore->setToolTip(QStringLiteral(
            "Fix under test (audit 2026-10-16). On (default): the keeping of below-black and above-white also works "
            "with BT.1886 and sRGB curves, SDR in wider colour spaces (BT.2020 / P3), a calibration 3D LUT (extended "
            "past its edges by its own slope) and HDR shown without tone mapping. Off: only pure-power gammas with "
            "BT.709 SDR and no LUT, as before."));
        // Delta 13: the SDR tone shape and headroom, in their own section
        // (audit 2026-10-14c).
        form = addCollapsibleSection(QStringLiteral("sdrHighlights"), QStringLiteral("SDR highlights"),
            QStringLiteral("How SDR highlights are shaped on an SDR output. Off by default."), false);
        auto* sdrShape = addChoice(QStringLiteral("SDR tone shape"), QStringLiteral("sdr_tone_shape"),
            { QStringLiteral("off"), QStringLiteral("knee") });
        sdrShape->setItemText(sdrShape->findData(QStringLiteral("knee")), QStringLiteral("Knee (soft highlight roll-off)"));
        sdrShape->setToolTip(QStringLiteral(
            "SDR sources shown on an SDR output only. Knee: highlights roll off softly toward peak, the shape tone-mapped HDR has, "
            "with no inverse tone map. Nothing is created above the master's white. Off is the standard path."));
        MarkGpuCost(form, sdrShape, 1);
        auto* sdrHeadroom = addText(QStringLiteral("SDR white headroom (share of peak, 0 to 0.5)"), QStringLiteral("sdr_white_headroom"));
        sdrHeadroom->setValidator(new QDoubleValidator(0.0, 0.5, 3, sdrHeadroom));
        sdrHeadroom->setPlaceholderText(QStringLiteral("0"));
        sdrHeadroom->setToolTip(QStringLiteral(
            "Places reference white below the projector's peak: 0.15 puts it at 85% of peak (in light), leaving room "
            "above it for above-white detail and the knee. 0 = reference white at peak (off)."));
        auto* sdrKneeStart = addText(QStringLiteral("SDR knee start (0.5 to 0.99 of peak)"), QStringLiteral("sdr_knee_start"));
        sdrKneeStart->setValidator(new QDoubleValidator(0.5, 0.99, 3, sdrKneeStart));
        sdrKneeStart->setPlaceholderText(QStringLiteral("0.80"));
        sdrKneeStart->setToolTip(QStringLiteral(
            "With the knee: where the highlights start to roll off, as a share of the projector's peak light. "
            "Lower starts the roll-off earlier (softer highlights)."));
        auto* sdrKneeStrength = addText(QStringLiteral("SDR knee strength (0 to 1)"), QStringLiteral("sdr_knee_strength"));
        sdrKneeStrength->setValidator(new QDoubleValidator(0.0, 1.0, 3, sdrKneeStrength));
        sdrKneeStrength->setPlaceholderText(QStringLiteral("0.5"));
        sdrKneeStrength->setToolTip(QStringLiteral(
            "With the knee: 0 = a straight line (headroom only), 1 = the full roll-off. 0.5 keeps most of the "
            "highlight contrast and still avoids clipping with headroom 0.15."));
        auto* sdrRecovery = addText(QStringLiteral("SDR contrast recovery (auto, off, 0 to 2)"),
            QStringLiteral("sdr_contrast_recovery"));
        sdrRecovery->setPlaceholderText(QStringLiteral("auto"));
        sdrRecovery->setToolTip(QStringLiteral(
            "With the knee: returns the local detail the knee flattens in the highlights (libplacebo's own contrast "
            "recovery works only on HDR). Auto = follow Tone Mapping > Contrast recovery; off = none; or a strength "
            "from 0 to 2."));
        sdrRecovery->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("^(|[Aa][Uu][Tt][Oo]|[Oo][Ff][Ff]|[01](\\.\\d{0,3})?|2(\\.0{0,3})?)$")),
            sdrRecovery));
        MarkGpuCost(form, sdrRecovery, 1);
        {
            auto greySdr = [sdrShape, sdrKneeStart, sdrKneeStrength, sdrRecovery]()
            {
                const bool knee = effectiveChoiceValue(sdrShape) == QStringLiteral("knee");
                for (QWidget* row : { sdrKneeStart, sdrKneeStrength, sdrRecovery })
                {
                    row->setProperty("greyedOff", !knee);
                    row->setEnabled(knee);
                }
            };
            QObject::connect(sdrShape, qOverload<int>(&QComboBox::currentIndexChanged), receiver, [greySdr](int) { greySdr(); });
            auto* sdrRefresh = new QAction(sdrShape);
            sdrRefresh->setObjectName(controlName(kit.sectionPrefix, QStringLiteral("sdr_tone_refresh")));
            QObject::connect(sdrRefresh, &QAction::triggered, receiver, [greySdr]() { greySdr(); });
            greySdr();
        }
        form = processingForm;
        // Dithering and Dither target depth moved to Scaling / Quality (Bill, v1.4.02).
        auto* fboFloat32 = addChoice(QStringLiteral("32-bit float intermediates"),
            QStringLiteral("fbo_float32"), { QStringLiteral("auto"), QStringLiteral("on"), QStringLiteral("off") });
        fboFloat32->setToolTip(QStringLiteral(
            "Every intermediate picture in 32-bit float: exact highlights (half-float rounds values above white "
            "a little) at more GPU memory bandwidth (a few ms per frame at high resolutions; check the render time in the stats "
            "overlay). Auto (default): on everywhere, the low-latency path too (higher precision is the default). "
            "Off only by choice. The final output stage is 32-bit float always."));
        MarkGpuCost(form, fboFloat32, 2);

        // Owner 2026-10-16 (reorganise if necessary): VP's own settings in signal order - capture, source
        // resolution and re-base, scaling blends, motion, dither, then what is still under test. The rows
        // were built where their logic lives; here they move to their sections (the controls, names and
        // behaviour unchanged). Bill's sections keep their places.
        {
            auto sectionWidget = [profileFields](const QString& id) -> QWidget* {
                auto* toggle = profileFields->findChild<QToolButton*>(QStringLiteral("rendererSection.") + id);
                return toggle ? toggle->parentWidget() : nullptr;
            };
            auto sectionForm = [profileFields](const QString& id) -> QFormLayout* {
                auto* content = profileFields->findChild<QWidget*>(QStringLiteral("rendererSection.%1.content").arg(id));
                if (!content) return nullptr;
                for (QWidget* child : content->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
                    if (auto* layout = qobject_cast<QFormLayout*>(child->layout())) return layout;
                return nullptr;
            };
            auto moveRow = [](QWidget* field, QFormLayout* to) {
                // Bill's addChoice wraps its combo (with the Reset button) in a row widget, so the
                // combo is not itself a form field: walk up to the widget that is.
                for (QWidget* f = field; f && f->parentWidget(); f = f->parentWidget())
                    if (auto* owner = qobject_cast<QFormLayout*>(f->parentWidget()->layout()))
                    {
                        int at = -1;
                        QFormLayout::ItemRole itemRole;
                        owner->getWidgetPosition(f, &at, &itemRole);
                        if (at >= 0) { field = f; break; }
                    }
                if (!field || !to || !field->parentWidget()) return;
                auto* from = qobject_cast<QFormLayout*>(field->parentWidget()->layout());
                if (!from || from == to) return;
                int row = -1;
                QFormLayout::ItemRole role;
                from->getWidgetPosition(field, &row, &role);
                if (row < 0) return;
                const bool visible = from->isRowVisible(row);
                const QFormLayout::TakeRowResult taken = from->takeRow(row);
                QWidget* label = taken.labelItem ? taken.labelItem->widget() : nullptr;
                delete taken.labelItem;
                delete taken.fieldItem;
                if (label) to->addRow(label, field); else to->addRow(field);
                to->setRowVisible(field, visible);
            };
            QFormLayout* captureForm = addCollapsibleSection(QStringLiteral("captureSource"), QStringLiteral("Capture & source"),
                QStringLiteral("How the captured signal is read: unpacked on the GPU and its range (native RGB: Fixes under test). ") +
                QString::fromLatin1(kGpuCostLegend), false);
            // Native RGB stays in Fixes under test (owner: a Fixes switch, default native).
            for (QWidget* w : { static_cast<QWidget*>(capturePath), static_cast<QWidget*>(sourceRange) })
                moveRow(w, captureForm);
            QFormLayout* rebaseForm = addCollapsibleSection(QStringLiteral("sourceRebase"), QStringLiteral("Source resolution & re-base"),
                QStringLiteral("Finds a picture upscaled before VP (a 1080p film sent at 4K), re-bases it to its real resolution "
                    "so VP's scaler works from the source, and debands there. ") + QString::fromLatin1(kGpuCostLegend), false);
            for (QWidget* w : { static_cast<QWidget*>(pictureBox), static_cast<QWidget*>(sourceResolution),
                    static_cast<QWidget*>(autoConfidence), static_cast<QWidget*>(evidenceSeconds),
                    static_cast<QWidget*>(evidenceShots), static_cast<QWidget*>(textGuard), static_cast<QWidget*>(mixedGuard),
                    static_cast<QWidget*>(kernelMatch), static_cast<QWidget*>(debandVp), static_cast<QWidget*>(lowBitrate) })
                moveRow(w, rebaseForm);
            if (QFormLayout* downscale = sectionForm(QStringLiteral("adaptiveDownscale")))
                for (QWidget* w : { static_cast<QWidget*>(ssimStabilise), static_cast<QWidget*>(bitrateAware),
                        static_cast<QWidget*>(bitrateFloor) })
                    moveRow(w, downscale);
            if (QFormLayout* upscale = sectionForm(QStringLiteral("adaptiveUpscale")))
                moveRow(cnnStabilise, upscale);
            // The dither's motion-reuse fix stays in Fixes under test (a fix with its switch); the
            // extended-range widening (built beside Bill's Processing rows) joins it.
            if (QFormLayout* fixes = sectionForm(QStringLiteral("fixesUnderTest")))
                moveRow(extendedMore, fixes);
            // The section order: Bill's opening sections, then VP's in signal order, then Bill's others.
            const QStringList order = { QStringLiteral("captureSource"), QStringLiteral("sourceRebase"),
                QStringLiteral("adaptiveDownscale"), QStringLiteral("adaptiveUpscale"), QStringLiteral("dejudder"),
                QStringLiteral("dejudderEstimation"), QStringLiteral("temporalDither"), QStringLiteral("fixesUnderTest"),
                QStringLiteral("dejudderExperiments") };
            QWidget* anchor = sectionWidget(QStringLiteral("dejudder"));
            int at = anchor ? profileFieldsLayout->indexOf(anchor) : -1;
            for (const QString& id : order)
                if (QWidget* section = sectionWidget(id); section && at >= 0)
                {
                    const int index = profileFieldsLayout->indexOf(section);
                    if (index < 0) continue;
                    profileFieldsLayout->removeWidget(section);
                    if (index < at) --at;
                    profileFieldsLayout->insertWidget(at++, section);
                }
        }
    }

    void RefreshVpvRenderingProfileState(QWidget* editorRoot, QWidget* profileFields,
        const QString& sectionPrefix, const QString& whenValue)
    {
        const auto findProfileWidget = [editorRoot, profileFields](const QString& objectName) -> QWidget* {
            QWidget* found = profileFields ? profileFields->findChild<QWidget*>(objectName) : nullptr;
            return found ? found : (editorRoot ? editorRoot->findChild<QWidget*>(objectName) : nullptr);
        };
        const auto findProfileAction = [editorRoot, profileFields](const QString& objectName) -> QAction* {
            QAction* found = profileFields ? profileFields->findChild<QAction*>(objectName) : nullptr;
            return found ? found : (editorRoot ? editorRoot->findChild<QAction*>(objectName) : nullptr);
        };
        static const QStringList kContentGateKeys = { QStringLiteral("intent_content"),
            QStringLiteral("intent_content.check"), QStringLiteral("source_class"),
            QStringLiteral("dejudder_pulldown"), QStringLiteral("cadence_class_fixes") };
        if (sectionPrefix != QStringLiteral("vprenderer"))
            return;
        for (const QString& key : kContentGateKeys)
            if (QWidget* widget = findProfileWidget(controlName(sectionPrefix, key)))
                if (widget->property("contentGateIgnored").toBool())
                {
                    widget->setProperty("contentGateIgnored", false);
                    widget->setProperty("greyedOff", false);
                    widget->setEnabled(true);
                    widget->setToolTip(widget->property("contentGateTooltip").toString());
                }
        for (const char* name : { "motion_preset_refresh", "picture_preset_refresh",
                "adaptive_faithful_refresh", "adaptive_motion_refresh", "optical_flow_refresh", "interpolation_refresh",
                "temporal_dither_refresh", "intent_content_refresh", "detail_restore_refresh",
                "intent_adaptive_refresh", "greyed_state_refresh",
                "adaptive_downscale_refresh", "adaptive_upscale_refresh", "sdr_tone_refresh" })
            if (auto* refresh = findProfileAction(controlName(sectionPrefix, QString::fromLatin1(name))))
                refresh->trigger();
        {
            bool contentGated = false;
            const std::string when = whenValue.trimmed().toStdString();
            if (!when.empty())
            {
                DisplayRuleExpression::Expression expression;
                std::string error;
                contentGated = expression.Compile(when, error, true) && expression.Variables().count("content") != 0;
            }
            const QString cadenceFixes = effectiveChoiceValue(qobject_cast<QComboBox*>(findProfileWidget(
                controlName(sectionPrefix, QStringLiteral("cadence_class_fixes")))));
            const bool ignored = contentGated && cadenceFixes != QStringLiteral("off");
            for (const QString& key : kContentGateKeys)
            {
                QWidget* widget = findProfileWidget(controlName(sectionPrefix, key));
                if (!widget || !ignored) continue;
                widget->setProperty("contentGateTooltip", widget->toolTip());
                widget->setProperty("contentGateIgnored", true);
                widget->setProperty("greyedOff", true);
                widget->setEnabled(false);
                widget->setToolTip(widget->toolTip() + QStringLiteral(
                    "\n\nIgnored in this profile: its when: rule reads $content, which this setting changes, "
                    "so the renderer keeps the value from outside this profile (cadence_class_fixes)."));
            }
        }
    }

    void AddStartupHardwareRows(QFormLayout* hardwareForm, ConfigPageHost::EditorKit& kit)
    {
        QFormLayout* vpvForm = AddNestedVpvForm(hardwareForm);
        const auto deckLinkPacking = [&kit, vpvForm](const QString& key, const QString& label,
            const QStringList& packings, const QString& defaults)
        {
            QStringList values{ QString(), QStringLiteral("AUTO") };
            QStringList labels{ QStringLiteral("Not set (AUTO: %1)").arg(defaults), QStringLiteral("AUTO (%1)").arg(defaults) };
            for (const QString& packing : packings) { values << packing; labels << packing; }
            auto* combo = kit.bindChoiceField(QStringLiteral("decklink"), key, values, labels);
            combo->setToolTip(QStringLiteral(
                "DeckLink only: host-memory packing requested for detected RGB input at this depth. AUTO keeps the "
                "established ARGB / r210 / R12B. The device and mode are checked; an unsupported packing falls "
                "back safely and is logged."));
            vpvForm->addRow(label, combo);
        };
        deckLinkPacking(QStringLiteral("rgb_8bit_packing"), QStringLiteral("8-bit RGB packing (DeckLink)"),
            { QStringLiteral("ARGB"), QStringLiteral("BGRA") }, QStringLiteral("ARGB"));
        deckLinkPacking(QStringLiteral("rgb_10bit_packing"), QStringLiteral("10-bit RGB packing (DeckLink)"),
            { QStringLiteral("R210"), QStringLiteral("R10B"), QStringLiteral("R10L") }, QStringLiteral("r210"));
        deckLinkPacking(QStringLiteral("rgb_12bit_packing"), QStringLiteral("12-bit RGB packing (DeckLink)"),
            { QStringLiteral("R12B"), QStringLiteral("R12L") }, QStringLiteral("R12B"));
        auto* conversionMethod = kit.bindChoiceField(QStringLiteral("directshow.conversion"), QStringLiteral("conversion_method"),
            { QString(), QStringLiteral("auto"), QStringLiteral("simd"), QStringLiteral("optimized"), QStringLiteral("standard") },
            { QStringLiteral("Not set (Auto)"), QStringLiteral("Auto"), QStringLiteral("SIMD"), QStringLiteral("Optimized"),
              QStringLiteral("Standard") });
        conversionMethod->setToolTip(QStringLiteral(
            "DirectShow path: the CPU method that converts v210 capture to P010. Auto picks the fastest available."));
        vpvForm->addRow(QStringLiteral("P010 conversion method (DirectShow)"), conversionMethod);
        auto* chromaDownsampling = kit.bindChoiceField(QStringLiteral("directshow.conversion"), QStringLiteral("chroma_downsampling"),
            { QString(), QStringLiteral("AVERAGE"), QStringLiteral("LEGACY"), QStringLiteral("ADVANCED") },
            { QStringLiteral("Not set (Average)"), QStringLiteral("Average adjacent rows"),
              QStringLiteral("Legacy: even rows, no averaging"), QStringLiteral("Advanced: Lanczos-3 (more CPU)") });
        chromaDownsampling->setToolTip(QStringLiteral(
            "DirectShow path: 4:2:2 to 4:2:0 chroma for P010. Average (default) averages adjacent chroma rows; "
            "Legacy takes the even rows; Advanced uses 12-tap Lanczos-3 vertical filtering (more CPU, possible ringing)."));
        vpvForm->addRow(QStringLiteral("P010 chroma downsampling (DirectShow)"), chromaDownsampling);
        for (const auto& cores : { std::make_pair(QStringLiteral("min_core_count"), QStringLiteral("P010 conversion helpers, minimum")),
            std::make_pair(QStringLiteral("max_core_count"), QStringLiteral("P010 conversion helpers, maximum")) })
        {
            auto* count = kit.bindTextField(QStringLiteral("directshow.conversion"), cores.first, QString());
            count->setValidator(new QIntValidator(1, INT_MAX, count));
            count->setPlaceholderText(QStringLiteral("1 (default)"));
            count->setToolTip(QStringLiteral(
                "DirectShow path: helper threads for the P010 conversion, not counting the calling thread. Empty = 1."));
            vpvForm->addRow(cores.second, count);
        }
        auto* ppm = kit.bindTextField(QStringLiteral("directshow.ppm"), QStringLiteral("ppm"), QString());
        ppm->setValidator(new QRegularExpressionValidator(QRegularExpression(
            QStringLiteral("^(|[Aa][Uu][Tt][Oo]|-?(1000000|[0-9]{1,6}))$")), ppm));
        ppm->setPlaceholderText(QStringLiteral("Not set (no correction)"));
        ppm->setToolTip(QStringLiteral(
            "DirectShow path: clock correction in parts per million for every source rate. AUTO measures it; a number "
            "from -1000000 to 1000000 applies it; empty applies none."));
        vpvForm->addRow(QStringLiteral("Clock PPM correction (DirectShow)"), ppm);
    }
}
