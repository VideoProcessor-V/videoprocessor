#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <objbase.h>

#include "ConfigEditorWindow.h"
#include <led/ConnectionTest.h>
#include <led/LedDriveConfig.h>
#include <led/SerialPort.h>
#include "UpdatePanel.h"
#include "ColorOutputProfileMigration.h"
#include "ProfileListController.h"
#include "InlineColorPicker.h"
#include "ToneMappingControl.h"
#include <ConfigurationApplyPolicy.h>
#include <ConfigurationIdentity.h>
#include <ConfigurationLiveApply.h>
#include <ActiveProfileStatus.h>
#include <UpdateLauncher.h>
#include <QMenuBar>

#include <ConfigEditorCore.h>
#include <ConfigurationRpcClient.h>
#include <RendererProfileConfig.h>
#include <QueueLatency.h>
#include <led/LedDriveConfig.h>

#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QApplication>
#include <QDoubleValidator>
#include <QRegularExpressionValidator>
#include <QStandardItemModel>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHash>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QIcon>
#include <QInputDialog>
#include <QIntValidator>
#include <QLabel>
#include <QLayout>
#include <QLibrary>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QRadioButton>
#include <QRect>
#include <QRegion>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSettings>
#include <QStyle>
#include <QShowEvent>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTabBar>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <QWindowStateChangeEvent>
#include <QWinEventNotifier>

#include <algorithm>
#include <functional>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace
{
// RS-232 connection test: whether VideoProcessor.exe runs on this computer
// (then the test goes through it, never around it).
bool LocalVideoProcessorRunning()
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return true;   // unknown: do not open the port behind VP's back
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	bool found = false;
	for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
		found = _wcsicmp(entry.szExeFile, L"VideoProcessor.exe") == 0;
	CloseHandle(snapshot);
	return found;
}

class TargetComboBox final : public QComboBox
{
protected:
	void resizeEvent(QResizeEvent* event) override
	{
		QComboBox::resizeEvent(event);
		QPainterPath shape;
		shape.addRoundedRect(QRectF(rect()), 4, 4);
		setMask(QRegion(shape.toFillPolygon().toPolygon()));
	}
};

// OUTOFCONTEXT events arrive on the Qt thread that installed the hook.
// No window ownership changes, cross-process SendMessage or recurring polling.
std::map<HWINEVENTHOOK, std::function<void()>> configForegroundHandlers;

QIcon targetRefreshIcon()
{
	QPixmap image(36, 36);
	image.setDevicePixelRatio(2.0);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.scale(0.75, 0.75);
	painter.setPen(QPen(QColor(QStringLiteral("#91c7e9")), 2.0,
		Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	QPainterPath curves;
	curves.moveTo(3, 11);
	curves.cubicTo(3.5, 6.4, 7.3, 3, 12, 3);
	curves.cubicTo(15.5, 3, 18.4, 4.6, 20, 7);
	curves.moveTo(21, 13);
	curves.cubicTo(20.5, 17.6, 16.7, 21, 12, 21);
	curves.cubicTo(8.5, 21, 5.6, 19.4, 4, 17);
	painter.drawPath(curves);
	painter.drawPolyline(QPolygonF{ QPointF(16, 7), QPointF(20, 7),
		QPointF(20, 3) });
	painter.drawPolyline(QPolygonF{ QPointF(8, 17), QPointF(4, 17),
		QPointF(4, 21) });
	return QIcon(image);
}

void CALLBACK configWindowOrderChanged(HWINEVENTHOOK hook, DWORD, HWND,
    LONG, LONG, DWORD, DWORD)
{
    const auto found = configForegroundHandlers.find(hook);
    if (found != configForegroundHandlers.end()) found->second();
}
constexpr int kPageMargin = 16;
constexpr int kCardPadding = 12;
constexpr int kResponsiveContentWidth = 720;
constexpr int kRendererNameRole = Qt::UserRole + 1;

using DocumentSection = std::map<std::string, std::string>;
using DocumentSnapshot = std::map<std::string, DocumentSection>;
constexpr const char* kSnapshotSectionOrderKey = "\x1eprofile_section_order";

DocumentSnapshot captureDocumentSnapshot(
    const ConfigEditorCore::ConfigDocument& document)
{
    DocumentSnapshot snapshot;
    const std::vector<std::string> orderedSections = document.SectionNames();
    std::map<std::string, size_t> profileOrder;
    for (const std::string& section : orderedSections)
    {
        snapshot[section];
        const std::string profileGroup =
            ConfigurationApplyPolicy::OrderedProfileGroup(section);
        if (!profileGroup.empty())
        {
            // Section order is executable profile precedence. Keep it in the
            // in-memory comparison only so an order-only Apply still tells VP
            // to rebuild its ordered profile model.
            snapshot[section][kSnapshotSectionOrderKey] =
                std::to_string(profileOrder[profileGroup]++);
        }
        for (const auto& setting : document.SectionSettings(section))
            snapshot[section][setting.first] = setting.second;
    }
    return snapshot;
}

std::vector<ConfigurationApplyPolicy::Change> changedDocumentValues(
    const DocumentSnapshot& previous, const DocumentSnapshot& current)
{
    std::set<std::string> names;
    for (const auto& entry : previous) names.insert(entry.first);
    for (const auto& entry : current) names.insert(entry.first);
    std::vector<ConfigurationApplyPolicy::Change> changed;
    for (const std::string& name : names)
    {
        const auto before = previous.find(name);
        const auto after = current.find(name);
        if (before == previous.end())
        {
            if (after->second.empty()) changed.push_back({ name, {} });
            for (const auto& setting : after->second)
                changed.push_back({ name, setting.first });
            continue;
        }
        if (after == current.end())
        {
            if (before->second.empty()) changed.push_back({ name, {} });
            for (const auto& setting : before->second)
                changed.push_back({ name, setting.first });
            continue;
        }
        for (const auto& setting : before->second)
        {
            const auto replacement = after->second.find(setting.first);
            if (replacement == after->second.end() ||
                replacement->second != setting.second)
                changed.push_back({ name, setting.first });
        }
        for (const auto& setting : after->second)
            if (before->second.find(setting.first) == before->second.end())
                changed.push_back({ name, setting.first });
    }
    return changed;
}

bool snapshotUsesDirectShowRenderer(const DocumentSnapshot& snapshot)
{
    for (const auto& section : snapshot)
    {
        if (ConfigurationApplyPolicy::NormalizeSection(section.first) != "general")
            continue;
        for (const auto& setting : section.second)
        {
            if (ConfigFile::NormalizeName(setting.first) != "renderer") continue;
            std::string normalized = setting.second;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return normalized.find("vp renderer") == std::string::npos &&
                normalized.find("videoprocessor renderer") == std::string::npos;
        }
    }
    return true;
}

QString effectCategory(const std::string& section)
{
    const std::string normalized = ConfigurationApplyPolicy::NormalizeSection(section);
    if (ConfigurationApplyPolicy::HasPrefix(normalized, "queue") ||
        normalized == "queue_recovery") return QStringLiteral("Queue");
    if (ConfigurationApplyPolicy::HasPrefix(normalized, "directshow"))
        return QStringLiteral("DirectShow");
    if (ConfigurationApplyPolicy::HasPrefix(normalized, "vprenderer"))
        return QStringLiteral("VP Renderer");
    if (ConfigurationApplyPolicy::HasPrefix(normalized, "viewport") ||
        ConfigurationApplyPolicy::HasPrefix(normalized, "profiles.viewport"))
        return QStringLiteral("Viewport");
    if (ConfigurationApplyPolicy::HasPrefix(normalized, "profiles"))
        return QStringLiteral("Profiles");
    if (ConfigurationApplyPolicy::HasPrefix(normalized, "lldv"))
        return QStringLiteral("LLDV");
    if (ConfigurationApplyPolicy::HasPrefix(normalized, "shader") ||
        ConfigurationApplyPolicy::HasPrefix(normalized, "shaders"))
        return QStringLiteral("Shaders");
    if (ConfigurationApplyPolicy::HasPrefix(normalized, "actions") ||
        ConfigurationApplyPolicy::HasPrefix(normalized, "event_actions"))
        return QStringLiteral("Actions");
    if (normalized == "shortcuts") return QStringLiteral("Shortcuts");
    if (normalized == "logging") return QStringLiteral("Logging");
    if (normalized == "general" || normalized == "command_line" ||
        normalized == "renderer_alias" || normalized == "decklink" ||
        normalized == "p010_conversion" || normalized == "ppm_correction" ||
        normalized == "display_refresh_rate_override")
        return QStringLiteral("Startup / input");
    return QStringLiteral("Preserved content");
}

constexpr int ActiveProfileRole = Qt::UserRole + 12;

class ProfileStateItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        QStyleOptionViewItem base(option);
        initStyleOption(&base, index);
        const bool active = index.data(ActiveProfileRole).toBool();
        // Keep the standard selection treatment, including its full-width
        // background. Drawing the control ourselves is necessary because the
        // base delegate re-initializes its option and would discard this text
        // offset before it paints.
        // Reserve the marker column for every profile row so names stay
        // aligned as the active selection changes.
        base.text.prepend(QStringLiteral("   "));
        const QWidget* widget = base.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &base, painter, widget);
        if (active)
        {
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#26b46b")));
            constexpr int diameter = 8;
            painter->drawEllipse(option.rect.left() + 4,
                option.rect.center().y() - diameter / 2, diameter, diameter);
            painter->restore();
        }
    }
};

// The platform delegate centers its editor using the editor's size hint. Our
// themed QLineEdit has a larger minimum height than the parameter row, which
// made the editor appear to jump downward when a value was clicked. Parameter
// values are all plain text, so give every value cell the same compact editor
// geometry instead of allowing that platform-dependent repositioning.
class ShaderParameterItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
        const QModelIndex& index) const override
    {
        if (index.column() != 1) return nullptr;
        auto* editor = new QLineEdit(parent);
        editor->setObjectName(QStringLiteral("config.shader.nls.parameter_value_editor"));
        // The application theme gives ordinary form fields a comfortable
        // minimum height. Table editors must instead fit the existing row.
        editor->setStyleSheet(QStringLiteral(
            "QLineEdit { min-height: 0; padding: 0 7px; }"));
        editor->setMinimumSize(0, 0);
        editor->setMaximumHeight(QWIDGETSIZE_MAX);
        return editor;
    }

    void updateEditorGeometry(QWidget* editor,
        const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        // Keep a one-pixel inset for the table gridline while occupying the
        // full row height. This is deterministic for every shader parameter.
        editor->setGeometry(option.rect.adjusted(1, 1, -1, -1));
    }
};

class ResponsiveCardGrid final : public QWidget
{
public:
    ResponsiveCardGrid()
    {
        layout_ = new QGridLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setHorizontalSpacing(14);
        layout_->setVerticalSpacing(14);
        layout_->setAlignment(Qt::AlignTop);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    }

    void addCard(QWidget* card)
    {
        // Cards share their row height on wide, two-column pages while their
        // contents stay top-justified. This keeps the visual grid aligned
        // without allowing controls to drift apart as the window grows.
        card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        if (card->layout()) card->layout()->setAlignment(Qt::AlignTop);
        cards_.push_back(card);
        reflow(width());
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        reflow(event->size().width());
    }

private:
    void reflow(int availableWidth)
    {
        const int columns = availableWidth >= kResponsiveContentWidth ? 2 : 1;
        if (columns != columns_ || layout_->count() != cards_.size())
        {
            columns_ = columns;
            for (QWidget* card : cards_) layout_->removeWidget(card);
            for (int index = 0; index < cards_.size(); ++index)
                layout_->addWidget(cards_[index], index / columns_, index % columns_, Qt::AlignTop);
            for (int column = 0; column < 2; ++column)
                layout_->setColumnStretch(column, column < columns_ ? 1 : 0);
        }

        for (QWidget* card : cards_) card->setMinimumHeight(0);
        if (columns_ != 2) return;

        layout_->activate();
        const int rowCount = (cards_.size() + columns_ - 1) / columns_;
        for (int row = 0; row < rowCount; ++row) layout_->setRowMinimumHeight(row, 0);
        for (int first = 0; first < cards_.size(); first += columns_)
        {
            const int second = first + 1;
            const int rowHeight = second < cards_.size() ?
                qMax(cards_[first]->sizeHint().height(), cards_[second]->sizeHint().height()) :
                cards_[first]->sizeHint().height();
            layout_->setRowMinimumHeight(first / columns_, rowHeight);
            cards_[first]->setMinimumHeight(rowHeight);
            if (second < cards_.size()) cards_[second]->setMinimumHeight(rowHeight);
        }
    }

    QGridLayout* layout_ = nullptr;
    QList<QWidget*> cards_;
    int columns_ = 0;
};

class ResponsiveSplitter final : public QSplitter
{
public:
    ResponsiveSplitter() : QSplitter(Qt::Horizontal)
    {
        setChildrenCollapsible(false);
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        const Qt::Orientation wanted =
            event->size().width() >= kResponsiveContentWidth ? Qt::Horizontal : Qt::Vertical;
        if (orientation() != wanted) setOrientation(wanted);
        QSplitter::resizeEvent(event);
    }
};

// Stage 5: what a Target delay really gives at the common frame rates.
QString queueLatencyNoteText(int milliseconds)
{
    if (milliseconds <= 0)
        return QStringLiteral(
            "Off: the queue holds Target frames (Advanced), so the delay changes with the frame rate.");
    QStringList parts;
    const struct { const char* name; double rate; } rates[] = {
        { "24p", 24000.0 / 1001.0 }, { "25p", 25.0 }, { "30p", 30000.0 / 1001.0 },
        { "50p", 50.0 }, { "60p", 60000.0 / 1001.0 } };
    for (const auto& rate : rates)
    {
        const size_t frames = QueueLatency::FramesForLatency(milliseconds, rate.rate);
        parts << QStringLiteral("%1 %2 frames (%3 ms)").arg(QString::fromLatin1(rate.name))
            .arg(frames).arg(qRound(QueueLatency::EffectiveLatencyMs(frames, rate.rate)));
    }
    return QStringLiteral(
        "Real delay, in whole frames: %1. Set the audio delay once to match. "
        "A profile's own Target frames (like Low latency's 1) still wins over a delay it would inherit.")
        .arg(parts.join(QStringLiteral(", ")));
}

QLabel* helpLabel(const QString& text)
{
    auto* label = new QLabel(text);
    label->setProperty("help", true);
    label->setWordWrap(true);
    return label;
}

QLineEdit* readOnlyValue(const QString& text)
{
    auto* edit = new QLineEdit(text);
    edit->setReadOnly(true);
    return edit;
}

QWidget* fieldWithHelp(const QString& labelText, QWidget* field, const QString& help)
{
    auto* widget = new QWidget;
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    auto* label = new QLabel(labelText);
    label->setBuddy(field);
    if (field->accessibleName().isEmpty()) field->setAccessibleName(labelText);
    if (!help.isEmpty()) field->setAccessibleDescription(help);
    layout->addWidget(label);
    layout->addWidget(field);
    if (!help.isEmpty()) layout->addWidget(helpLabel(help));
    return widget;
}

QString displayName(const QString& section, const QString& root)
{
    if (section.compare(root, Qt::CaseInsensitive) == 0) return QStringLiteral("Profile 1");
    QString name = section.mid(root.size() + 1);
    if (root.compare(QStringLiteral("vprenderer.viewport"), Qt::CaseInsensitive) == 0 &&
        name.startsWith(QStringLiteral("viewport_"), Qt::CaseInsensitive))
        name.remove(0, 9);
    name.replace(QStringLiteral("__"), QStringLiteral("\x1"));
    name.replace(u'_', u' ');
    name.replace(QStringLiteral("\x1"), QStringLiteral("_"));
    if (QRegularExpression(QStringLiteral("^profile \\d+$"), QRegularExpression::CaseInsensitiveOption).match(name).hasMatch())
        name[0] = name[0].toUpper();
    return name.isEmpty() ? QStringLiteral("Profile") : name;
}

QString profileIdentifier(const QString& name)
{
    QString result;
    for (const QChar character : name.trimmed())
    {
        if (character.unicode() < 128 && character.isLetterOrNumber()) result += character;
        else if (character == u'-') result += character;
        else if (character == u'_') result += QStringLiteral("__");
        else if (character.isSpace()) result += u'_';
        else result += u'_';
    }
    while (result.contains(QStringLiteral("___"))) result.replace(QStringLiteral("___"), QStringLiteral("__"));
    if (result.isEmpty() || !result.front().isLetter()) result.prepend(QStringLiteral("profile_"));
    return result;
}

QString canonicalShortcutText(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return trimmed;
    std::string canonical;
    return RendererProfileConfig::CanonicalizeKeyChord(
        trimmed.toStdString(), canonical) ?
        QString::fromStdString(canonical) : trimmed;
}

QString effectiveChoiceValue(const QComboBox* combo)
{
    if (!combo) return {};
    const QString selected = combo->currentData().toString();
    return (selected.isEmpty() ? combo->property("effectiveValue").toString() : selected).trimmed().toLower();
}

QString diagnosticPresetForControls(const QObject* parent)
{
    for (const char* key : { "diagnostic_disable_compute", "diagnostic_force_8bit_sdr_swapchain",
        "diagnostic_vp_owned_dxgi_presenter", "diagnostic_disable_shader_cache" })
        if (const auto* flag = parent->findChild<QCheckBox*>("config.vprenderer.color." + QString::fromLatin1(key)))
            if (flag->isChecked()) return QStringLiteral("custom");
    const auto* diagnostics = parent->findChild<QCheckBox*>("config.vprenderer.color.output_diagnostics");
    return diagnostics && diagnostics->isChecked() ? QStringLiteral("proposed") : QStringLiteral("legacy");
}

bool configuredBooleanValue(const QString& value, bool defaultValue)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized.isEmpty()) return defaultValue;
    if (normalized == QStringLiteral("true") || normalized == QStringLiteral("yes") ||
        normalized == QStringLiteral("1") || normalized == QStringLiteral("on"))
        return true;
    if (normalized == QStringLiteral("false") || normalized == QStringLiteral("no") ||
        normalized == QStringLiteral("0") || normalized == QStringLiteral("off"))
        return false;
    return defaultValue;
}

QCheckBox* configuredCheck(const QString& text, const QString& value, bool defaultValue = false)
{
    auto* check = new QCheckBox(text);
    check->setTristate(false);
    check->setChecked(configuredBooleanValue(value, defaultValue));
    check->setEnabled(false);
    return check;
}

QStringList discoverValues(const char* exportName, bool hideLegacyRenderers = true)
{
    static QLibrary library(QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("VideoProcessorConfigDiscovery.dll")));
    if (!library.load()) return {};
    using DiscoverNoArgument = wchar_t* (__stdcall*)();
    using DiscoverRenderers = wchar_t* (__stdcall*)(BOOL);
    wchar_t* values = nullptr;
    if (qstrcmp(exportName, "VPDiscoverRenderers") == 0)
    {
        auto discover = reinterpret_cast<DiscoverRenderers>(library.resolve(exportName));
        if (discover) values = discover(hideLegacyRenderers ? TRUE : FALSE);
    }
	else
	{
        auto discover = reinterpret_cast<DiscoverNoArgument>(library.resolve(exportName));
        if (discover) values = discover();
    }
    QStringList result;
    for (const wchar_t* current = values; current && *current; current += wcslen(current) + 1)
        result.push_back(QString::fromWCharArray(current));
    if (values) CoTaskMemFree(values);
    return result;
}

QStringList discoverCaptureConnections(const QString& captureDeviceName)
{
    static QLibrary library(QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("VideoProcessorConfigDiscovery.dll")));
    if (!library.load()) return {};
    using DiscoverConnections = wchar_t* (__stdcall*)(const wchar_t*);
    auto discover = reinterpret_cast<DiscoverConnections>(
        library.resolve("VPDiscoverCaptureConnections"));
    if (!discover) return {};
    wchar_t* values = discover(reinterpret_cast<const wchar_t*>(
        captureDeviceName.utf16()));
    QStringList result;
    for (const wchar_t* current = values; current && *current;
        current += wcslen(current) + 1)
        result.push_back(QString::fromWCharArray(current));
    if (values) CoTaskMemFree(values);
    return result;
}

QStringList withDefaultChoice(QStringList values)
{
    values.prepend(QString());
    return values;
}

void openValidationError(QWidget* parent, const QString& message)
{
    auto* dialog = new QDialog(parent, Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
    dialog->setWindowTitle(QStringLiteral("Unable to save configuration"));
    dialog->setObjectName(QStringLiteral("noticeDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(true);
    dialog->setMinimumWidth(440);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(16);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    auto* headingRow = new QHBoxLayout;
    headingRow->setSpacing(12);
    auto* badge = new QLabel(QStringLiteral("!"));
    badge->setObjectName(QStringLiteral("errorBadge"));
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedSize(34, 34);
    auto* heading = new QLabel(QStringLiteral("Check this setting"));
    heading->setObjectName(QStringLiteral("noticeTitle"));
    headingRow->addWidget(badge);
    headingRow->addWidget(heading, 1);
    layout->addLayout(headingRow);

    auto* detail = new QLabel(message);
    detail->setObjectName(QStringLiteral("noticeMessage"));
    detail->setWordWrap(true);
    detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detail);

    auto* actions = new QHBoxLayout;
    actions->addStretch();
    auto* ok = new QPushButton(QStringLiteral("OK"));
    ok->setProperty("primary", true);
    ok->setDefault(true);
    actions->addWidget(ok);
    layout->addLayout(actions);
    QObject::connect(ok, &QPushButton::clicked, dialog, &QDialog::accept);
    dialog->open();
}

bool isSharedInputSetting(const QString& key)
{
    return key == QStringLiteral("video_conversion") ||
        key == QStringLiteral("container_colorspace") ||
        key == QStringLiteral("hdr_colorspace") ||
        key == QStringLiteral("hdr_luminance");
}

bool isShaderStructuralKey(const QString& key)
{
    static const QStringList keys = {
        QStringLiteral("label"), QStringLiteral("shortcut"), QStringLiteral("cycle_shortcut"), QStringLiteral("when"),
        QStringLiteral("type"),
        QStringLiteral("shader_type"), QStringLiteral("hlsl_file"),
        QStringLiteral("glsl_file"), QStringLiteral("stage"), QStringLiteral("order")
    };
    return keys.contains(key, Qt::CaseInsensitive);
}

struct ShaderEditorState
{
    QString section;
    bool loading = false;
};

// One //!PARAM block of an mpv-style GLSL shader.
struct GlslShaderParameter
{
    QString name;
    QString description;
    bool integer = false;
    bool hasMinimum = false;
    bool hasMaximum = false;
    double minimum = 0.0;
    double maximum = 0.0;
    double defaultValue = 0.0;
};

// The numeric //!PARAM blocks of a shader file; found is false when the file
// cannot be read. ENUM parameters (non-numeric defaults) are skipped.
QList<GlslShaderParameter> readGlslShaderParameters(const QString& path, bool& found)
{
    QList<GlslShaderParameter> parameters;
    QFile file(path);
    found = file.open(QIODevice::ReadOnly | QIODevice::Text);
    if (!found) return parameters;
    GlslShaderParameter current;
    bool inParameter = false;
    for (QString line : QString::fromUtf8(file.readAll()).split(u'\n'))
    {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("//!")))
        {
            const QString header = line.mid(3);
            const int space = header.indexOf(u' ');
            const QString key = (space < 0 ? header : header.left(space)).toUpper();
            const QString argument = space < 0 ? QString() : header.mid(space + 1).trimmed();
            if (key == QStringLiteral("PARAM"))
            {
                current = GlslShaderParameter();
                current.name = argument;
                inParameter = !argument.isEmpty();
            }
            else if (!inParameter) continue;
            else if (key == QStringLiteral("DESC")) current.description = argument;
            else if (key == QStringLiteral("TYPE"))
                current.integer = !argument.contains(QStringLiteral("float"), Qt::CaseInsensitive);
            else if (key == QStringLiteral("MINIMUM"))
                current.minimum = argument.toDouble(&current.hasMinimum);
            else if (key == QStringLiteral("MAXIMUM"))
                current.maximum = argument.toDouble(&current.hasMaximum);
            else inParameter = false;
            continue;
        }
        if (!inParameter || line.isEmpty()) continue;
        bool numeric = false;
        current.defaultValue = line.toDouble(&numeric);
        if (numeric) parameters.push_back(current);
        inParameter = false;
    }
    return parameters;
}

struct ShaderParameterControls
{
    QToolButton* toggle = nullptr;
    QWidget* fields = nullptr;
    QTableWidget* table = nullptr;
    std::function<void()> reload;
};

ShaderParameterControls addShaderParameterControls(QWidget* dialogParent,
    QObject* signalContext, QVBoxLayout* detailsLayout,
    const std::shared_ptr<ShaderEditorState>& state,
    std::function<ConfigEditorCore::ConfigDocument*()> document,
    std::function<void()> markDirty, const QString& objectPrefix,
    const QString& accessibleName, const QString& removeDescription)
{
    ShaderParameterControls result;
    result.toggle = new QToolButton;
    result.toggle->setObjectName(objectPrefix + QStringLiteral(".parameters_toggle"));
    result.toggle->setText(QStringLiteral("Custom parameters"));
    result.toggle->setProperty("profileSection", true);
    result.toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    result.toggle->setArrowType(Qt::RightArrow);
    result.toggle->setCheckable(true);
    result.toggle->setChecked(false);
    result.toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    result.toggle->setAccessibleDescription(
        QStringLiteral("Expand or collapse custom parameters."));
    detailsLayout->addWidget(result.toggle);
    result.fields = new QWidget;
    auto* parameterLayout = new QVBoxLayout(result.fields);
    parameterLayout->setContentsMargins(0, 0, 0, 0);
    parameterLayout->setSpacing(10);
    result.table = new QTableWidget(0, 2);
    result.table->setAccessibleName(accessibleName);
    result.table->setAccessibleDescription(
        QStringLiteral("Shader-specific parameter names and values."));
    result.table->setObjectName(objectPrefix + QStringLiteral(".parameters"));
    result.table->setHorizontalHeaderLabels({ QStringLiteral("Parameter"), QStringLiteral("Value") });
    result.table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    result.table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    result.table->verticalHeader()->setVisible(false);
    result.table->setSelectionBehavior(QAbstractItemView::SelectRows);
    result.table->setSelectionMode(QAbstractItemView::SingleSelection);
    result.table->setItemDelegateForColumn(1,
        new ShaderParameterItemDelegate(result.table));
    auto updateHeight = [table = result.table]
    {
        const int frame = table->frameWidth() * 2;
        const int header = table->horizontalHeader()->height();
        int rows = 0;
        for (int row = 0; row < table->rowCount(); ++row)
            rows += table->rowHeight(row);
        table->setFixedHeight(frame + header + rows);
    };
    result.table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(QStringLiteral("+ Add parameter"));
    auto* remove = new QPushButton(QStringLiteral("Remove selected"));
    remove->setProperty("danger", true);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    parameterLayout->addWidget(new QLabel(QStringLiteral("Custom parameters")));
    parameterLayout->addWidget(helpLabel(QStringLiteral(
        "Optional implementation-specific key/value pairs. Untouched keys and comments are preserved.")));
    parameterLayout->addWidget(result.table);
    parameterLayout->addLayout(buttons);
    result.fields->setVisible(false);
    detailsLayout->addWidget(result.fields);
    QObject::connect(result.table->model(), &QAbstractItemModel::rowsInserted, signalContext,
        [updateHeight](const QModelIndex&, int, int) { updateHeight(); });
    QObject::connect(result.table->model(), &QAbstractItemModel::rowsRemoved, signalContext,
        [updateHeight](const QModelIndex&, int, int) { updateHeight(); });
    QObject::connect(result.toggle, &QToolButton::toggled, signalContext,
        [toggle = result.toggle, fields = result.fields, state](bool open)
    {
        fields->setVisible(open && !state->section.isEmpty());
        toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    QObject::connect(result.table, &QTableWidget::cellChanged, signalContext,
        [state, table = result.table, document, markDirty](int row, int column)
    {
        ConfigEditorCore::ConfigDocument* current = document();
        if (state->loading || state->section.isEmpty() || !current || column != 1) return;
        const auto* keyItem = table->item(row, 0);
        const auto* valueItem = table->item(row, 1);
        if (!keyItem || !valueItem) return;
        current->SetKnown(state->section.toStdString(),
            keyItem->text().toLocal8Bit().constData(),
            valueItem->text().toLocal8Bit().constData());
        markDirty();
    });
    QObject::connect(add, &QPushButton::clicked, signalContext,
        [dialogParent, state, table = result.table, document, markDirty]
    {
        ConfigEditorCore::ConfigDocument* current = document();
        if (state->section.isEmpty() || !current) return;
        bool accepted = false;
        const QString key = QInputDialog::getText(dialogParent,
            QStringLiteral("Add shader parameter"), QStringLiteral("Parameter name"),
            QLineEdit::Normal, QString(), &accepted).trimmed();
        if (!accepted || key.isEmpty()) return;
        if (!QRegularExpression(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]*$")).match(key).hasMatch() ||
            isShaderStructuralKey(key))
        {
            QMessageBox::warning(dialogParent, QStringLiteral("Shader parameter"),
                QStringLiteral("Use a non-reserved key beginning with a letter or underscore."));
            return;
        }
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0)->text().compare(key, Qt::CaseInsensitive) == 0)
            {
                QMessageBox::warning(dialogParent, QStringLiteral("Shader parameter"),
                    QStringLiteral("That parameter already exists."));
                return;
            }
        const QString value = QInputDialog::getText(dialogParent,
            QStringLiteral("Add shader parameter"), QStringLiteral("Value"),
            QLineEdit::Normal, QString(), &accepted);
        if (!accepted) return;
        current->SetKnown(state->section.toStdString(), key.toLocal8Bit().constData(),
            value.toLocal8Bit().constData());
        state->loading = true;
        const int row = table->rowCount();
        table->insertRow(row);
        auto* keyItem = new QTableWidgetItem(key);
        keyItem->setFlags(keyItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 0, keyItem);
        table->setItem(row, 1, new QTableWidgetItem(value));
        state->loading = false;
        markDirty();
    });
    QObject::connect(remove, &QPushButton::clicked, signalContext,
        [dialogParent, state, table = result.table, document, markDirty, removeDescription]
    {
        ConfigEditorCore::ConfigDocument* current = document();
        const int row = table->currentRow();
        if (state->section.isEmpty() || !current || row < 0) return;
        const QString key = table->item(row, 0)->text();
        if (QMessageBox::question(dialogParent, QStringLiteral("Remove shader parameter"),
            QStringLiteral("Remove '%1' from this %2?").arg(key, removeDescription),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
        current->RemoveKnown(state->section.toStdString(), key.toLocal8Bit().constData());
        state->loading = true;
        table->removeRow(row);
        state->loading = false;
        markDirty();
    });
    result.reload = [state, table = result.table, toggle = result.toggle,
        fields = result.fields, document, updateHeight]
    {
        ConfigEditorCore::ConfigDocument* current = document();
        const bool available = !state->section.isEmpty() && current;
        toggle->setVisible(available);
        fields->setVisible(available && toggle->isChecked());
        table->setRowCount(0);
        if (!available) return;
        for (const auto& setting : current->SectionSettings(state->section.toStdString()))
        {
            const QString key = QString::fromLocal8Bit(setting.first.c_str());
            if (isShaderStructuralKey(key)) continue;
            const int row = table->rowCount();
            table->insertRow(row);
            auto* keyItem = new QTableWidgetItem(key);
            keyItem->setFlags(keyItem->flags() & ~Qt::ItemIsEditable);
            table->setItem(row, 0, keyItem);
            table->setItem(row, 1,
                new QTableWidgetItem(QString::fromLocal8Bit(setting.second.c_str())));
        }
        updateHeight();
    };
    return result;
}

QString controlName(const QString& section, const QString& key)
{
    return QStringLiteral("config.%1.%2").arg(section, key);
}

QString accessibleSettingName(QString key)
{
    key.replace(u'_', u' ');
    key.replace(u'.', u' ');
    if (!key.isEmpty()) key[0] = key[0].toUpper();
    return key;
}

bool openPathExternally(const QString& path)
{
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", native.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
}

QString friendlyChoiceLabel(const QString& raw)
{
    static const QHash<QString, QString> labels = {
        { QStringLiteral("CLASSIC"), QStringLiteral("Classic") },
        { QStringLiteral("GENERATED_GRAY"), QStringLiteral("Move / Reconstruction (Beta)") },
        { QStringLiteral("BLACK"), QStringLiteral("Move / Solid Background (Beta)") },
        { QStringLiteral("AUTO"), QStringLiteral("Auto") },
        { QStringLiteral("NONE"), QStringLiteral("Disabled") },
        { QStringLiteral("V210_TO_P010"), QStringLiteral("V210 to P010") },
        { QStringLiteral("FOLLOW_INPUT"), QStringLiteral("Follow input") },
        { QStringLiteral("FOLLOW_INPUT_LLDV"), QStringLiteral("Follow input (LLDV)") },
        { QStringLiteral("FOLLOW_CONTAINER"), QStringLiteral("Follow container") },
        { QStringLiteral("HDR_LUMINANCE_USER"), QStringLiteral("User values") },
        { QStringLiteral("FAST"), QStringLiteral("Fast") },
        { QStringLiteral("BALANCED"), QStringLiteral("Balanced") },
        { QStringLiteral("HIGH"), QStringLiteral("High") },
        { QStringLiteral("DIRECT"), QStringLiteral("Direct") },
        { QStringLiteral("COMPOSED"), QStringLiteral("Composed") },
        { QStringLiteral("FULL"), QStringLiteral("Full") },
        { QStringLiteral("LIMITED"), QStringLiteral("Limited") },
        { QStringLiteral("SMALL"), QStringLiteral("Small") },
        { QStringLiteral("ON"), QStringLiteral("On") },
        { QStringLiteral("OFF"), QStringLiteral("Off") },
        { QStringLiteral("LIGHT"), QStringLiteral("Light") },
        { QStringLiteral("DEFAULT"), QStringLiteral("Standard") },
        { QStringLiteral("BT1886"), QStringLiteral("BT.1886") },
        { QStringLiteral("SRGB"), QStringLiteral("sRGB") },
        { QStringLiteral("REC709"), QStringLiteral("Rec. 709") },
        { QStringLiteral("REC601_525"), QStringLiteral("Rec. 601 (525-line)") },
        { QStringLiteral("REC601_625"), QStringLiteral("Rec. 601 (625-line)") },
        { QStringLiteral("BT709"), QStringLiteral("BT.709") },
        { QStringLiteral("BT601"), QStringLiteral("BT.601") },
        { QStringLiteral("BT2020"), QStringLiteral("BT.2020") },
        { QStringLiteral("BT2020_CONST"), QStringLiteral("BT.2020 constant luminance") },
        { QStringLiteral("BT2020_10"), QStringLiteral("BT.2020 (10-bit)") },
        { QStringLiteral("BT2020_12"), QStringLiteral("BT.2020 (12-bit)") },
        { QStringLiteral("P3_D65"), QStringLiteral("P3-D65") },
        { QStringLiteral("P3_DCI"), QStringLiteral("DCI-P3") },
        { QStringLiteral("P3_D60"), QStringLiteral("P3-D60") },
        { QStringLiteral("HIGH_QUALITY"), QStringLiteral("High quality") },
		{ QStringLiteral("PIXEL_SHADERS"), QStringLiteral("Tone map HDR using pixel shaders") },
		{ QStringLiteral("EXTERNAL_3DLUT"), QStringLiteral("Tone map HDR using external 3D LUT") },
		{ QStringLiteral("PASSTHROUGH"), QStringLiteral("Pass HDR through to display") },
        { QStringLiteral("EWA_LANCZOSSHARP"), QStringLiteral("EWA Lanczos sharp") },
        { QStringLiteral("EWA_LANCZOS"), QStringLiteral("EWA Lanczos") },
        { QStringLiteral("EWA_LANCZOS4SHARPEST"), QStringLiteral("EWA Lanczos 4 sharpest") },
        { QStringLiteral("CATMULL_ROM"), QStringLiteral("Catmull-Rom") },
        { QStringLiteral("NEAREST"), QStringLiteral("Nearest") },
        { QStringLiteral("OVERSAMPLE"), QStringLiteral("Oversample") },
        { QStringLiteral("GAUSSIAN"), QStringLiteral("Gaussian") },
        { QStringLiteral("LANCZOS"), QStringLiteral("Lanczos") },
        { QStringLiteral("BOX"), QStringLiteral("Box") },
        { QStringLiteral("HERMITE"), QStringLiteral("Hermite") },
        { QStringLiteral("MITCHELL"), QStringLiteral("Mitchell") },
        { QStringLiteral("BICUBIC"), QStringLiteral("Bicubic") },
        { QStringLiteral("BILINEAR"), QStringLiteral("Bilinear") },
        { QStringLiteral("SPLINE"), QStringLiteral("Spline") },
        { QStringLiteral("BT2390"), QStringLiteral("BT.2390") },
        { QStringLiteral("ST2094-40"), QStringLiteral("ST 2094-40") },
        { QStringLiteral("REINHARD"), QStringLiteral("Reinhard") },
        { QStringLiteral("PERCEPTUAL"), QStringLiteral("Perceptual") },
        { QStringLiteral("SOFTCLIP"), QStringLiteral("Soft clip") },
        { QStringLiteral("RELATIVE"), QStringLiteral("Relative") },
        { QStringLiteral("DESATURATE"), QStringLiteral("Desaturate") },
        { QStringLiteral("GAMMA_1.8"), QStringLiteral("Gamma 1.8") },
        { QStringLiteral("GAMMA_2.0"), QStringLiteral("Gamma 2.0") },
        { QStringLiteral("GAMMA_2.2"), QStringLiteral("Gamma 2.2") },
        { QStringLiteral("GAMMA_2.6"), QStringLiteral("Gamma 2.6") },
        { QStringLiteral("GAMMA_2.8"), QStringLiteral("Gamma 2.8") },
        { QStringLiteral("LINEAR_RGB"), QStringLiteral("Linear RGB") },
        { QStringLiteral("8BIT_GAMMA_2.2"), QStringLiteral("8-bit gamma 2.2") },
        { QStringLiteral("LOG_100_1"), QStringLiteral("Log 100:1") },
        { QStringLiteral("LOG_316_1"), QStringLiteral("Log 316:1") },
        { QStringLiteral("HYBRID_LOG_GAMMA"), QStringLiteral("HLG") },
        { QStringLiteral("240M"), QStringLiteral("SMPTE 240M") },
        { QStringLiteral("YCGCO"), QStringLiteral("YCgCo") },
        { QStringLiteral("NTSC_SYSM"), QStringLiteral("NTSC System M") },
        { QStringLiteral("NTSC_SYSBG"), QStringLiteral("NTSC System B/G") },
        { QStringLiteral("CIE1931_ZYX"), QStringLiteral("CIE 1931 ZYX") },
        { QStringLiteral("ACES"), QStringLiteral("ACES") },
        { QStringLiteral("DCI-P3"), QStringLiteral("DCI-P3") },
        { QStringLiteral("PQ"), QStringLiteral("PQ") },
        { QStringLiteral("FCC"), QStringLiteral("FCC") }
    };
    return labels.value(raw.trimmed().toUpper(), raw);
}
}

QRect ConfigEditorPlacement::ClampFrameToWorkArea(const QRect& frame,
    const QRect& workArea)
{
    if (!frame.isValid() || !workArea.isValid()) return frame;
    const int maximumX = workArea.right() - frame.width() + 1;
    const int maximumY = workArea.bottom() - frame.height() + 1;
    const int x = frame.width() <= workArea.width() ?
        std::max(workArea.left(), std::min(frame.left(), maximumX)) :
        workArea.left();
    const int y = frame.height() <= workArea.height() ?
        std::max(workArea.top(), std::min(frame.top(), maximumY)) :
        workArea.top();
    return QRect(x, y, frame.width(), frame.height());
}

namespace
{
    // GPU cost marker for a Config row (owner 2026-10-13): "+", "++" or "+++"
    // in green, amber or red after the row's label, and the same words at
    // the front of the control's tooltip and accessible description (the
    // label text itself stays plain: it is the control's accessible name).
    // The ratings are estimates; the stats overlay shows the real render time.
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

    const char* const kGpuCostLegend =
        "GPU cost: + small, ++ moderate, +++ large (estimated per frame; the stats overlay, Ctrl+I, shows the real render time).";
}

ConfigEditorWindow::ConfigEditorWindow(QString configPath, quintptr ownerHandle,
    bool testMode, const QStringList& testFilteredRenderers,
    const QStringList& testAllRenderers, const QString& remoteHost,
    quint16 remotePort, const QString& remoteName, bool noTarget,
    bool localAvailable, const QString& rememberedTargetId,
    const QString& rememberedTargetLabel)
    : configPath_(std::move(configPath)), localConfigPath_(configPath_),
      ownerHandle_(ownerHandle),
      noTarget_(noTarget), localAvailable_(localAvailable),
      rememberedTargetId_(rememberedTargetId),
      rememberedTargetLabel_(rememberedTargetLabel),
      initialSearchPending_(noTarget), testMode_(testMode),
      document_(std::make_unique<ConfigEditorCore::ConfigDocument>())
{
	remoteHost_ = remoteHost;
	remoteName_ = remoteName.isEmpty() ? remoteHost : remoteName;
	remotePort_ = remotePort;
	if (!remoteHost_.isEmpty())
		remoteClient_ = std::make_unique<ConfigurationRpcClient>(
			remoteHost_.toStdString(), remotePort);
    // A VP-launched editor can acquire a native HWND while it is still being
    // constructed. Keep that frame off-screen until reveal() has finished
    // layout/polish work and the event loop is ready to paint it.
    if (!testMode_)
        setAttribute(Qt::WA_DontShowOnScreen, true);
    // Windows advertises a roughly 150 ms combo-box animation through Qt.
    // Config is a dense control surface; dropdowns should open synchronously.
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    if (ownerHandle_)
    {
        DWORD processId = 0;
        if (IsWindow(reinterpret_cast<HWND>(ownerHandle_)))
            GetWindowThreadProcessId(reinterpret_cast<HWND>(ownerHandle_), &processId);
        ownerProcessId_ = processId;
    }
    setWindowTitle(remoteClient_ ?
		QStringLiteral("VideoProcessor Configuration · %1").arg(remoteName_) :
		QStringLiteral("VideoProcessor Configuration"));
    setAccessibleName(QStringLiteral("VideoProcessor Configuration"));
    setObjectName(QStringLiteral("root"));
    // This must be a Qt-owned flag, not only a later SetWindowPos call: the
    // QWindows backend can otherwise recreate or restyle the native HWND and
    // silently remove WS_EX_TOPMOST after Config has been revealed.
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    // This is the smallest dashboard layout that keeps both card columns,
    // navigation, and the fixed action footer usable without clipping.
    resize(1040, 700);
    setMinimumSize(1040, 700);
    loadConfiguration();
    migrateLldvInputPolicy();
    migrateSharedRefreshRate();
	migrateRefreshRateSwitchMode();
    migrateSeparatedRendererProfiles();
    migrateUnifiedColorOutputProfiles();
    migrateCalibrationProfiles();
    migrateViewportZoomProfiles();
    if (!testMode_ && !noTarget_) loadDiscoveryCache();
    else if (testMode_)
    {
        filteredRenderers_ = testFilteredRenderers;
        allRenderers_ = testAllRenderers;
    }
    setCentralWidget(createShell());
	if (noTarget_)
		setStatus(QStringLiteral("No VideoProcessor target is available. Searching this LAN."));
	else if (!remoteLoadError_.isEmpty())
		setStatus(remoteLoadError_, true);
    profileSelectionReady_ = true;
    refreshLimitedTransportControls();
    refreshCalibrationControls();
    if (!testMode_)
    {
        activeProfileTimer_ = new QTimer(this);
        connect(activeProfileTimer_, &QTimer::timeout, this,
            &ConfigEditorWindow::refreshActiveProfileIndicators);
        if (!remoteClient_ && !noTarget_) refreshActiveProfileIndicators();
        shaderCacheStatusTimer_ = new QTimer(this);
        shaderCacheStatusTimer_->setInterval(2000);
        connect(shaderCacheStatusTimer_, &QTimer::timeout, this,
            &ConfigEditorWindow::refreshShaderCacheStatus);
        shaderCacheStatusTimer_->start();
    }
    if (!testMode_)
    {
        setupTray();
        auto* updateTimer = new QTimer(this);
        updateTimer->setInterval(6 * 60 * 60 * 1000);
        connect(updateTimer, &QTimer::timeout, this, [] { UpdateLauncher::Launch(true); });
        updateTimer->start();
        QTimer::singleShot(30000, this, [] { UpdateLauncher::Launch(true); });
    }
	const std::wstring revealEventName =
		ConfigurationLiveApply::ConfigurationEditorRevealEventName(
			GetCurrentProcessId());
	revealEvent_ = CreateEventW(nullptr, FALSE, FALSE,
		revealEventName.c_str());
	if (revealEvent_)
	{
		revealEventNotifier_ = new QWinEventNotifier(revealEvent_, this);
		connect(revealEventNotifier_, &QWinEventNotifier::activated, this,
			[this]
		{
			// Consume the auto-reset event through the notifier and route the
			// command through the same current-window path as a tray click.
			reveal();
		});
	}
    const QKeySequence configToggle(value(QStringLiteral("shortcuts"),
        QStringLiteral("config_editor"), QStringLiteral("Ctrl+Shift+S")));
    if (!configToggle.isEmpty())
    {
        auto* toggleShortcut = new QShortcut(configToggle, this);
        toggleShortcut->setContext(Qt::ApplicationShortcut);
        connect(toggleShortcut, &QShortcut::activated, this, [this]
        {
            // The Config shortcut is a reveal command everywhere, including
            // when Qt itself owns the keyboard focus. It must not toggle the
            // editor back into the tray.
            reveal();
        });
    }

    const HWINEVENTHOOK foregroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        configWindowOrderChanged, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    foregroundEventHook_ = foregroundHook;
    if (foregroundHook)
        configForegroundHandlers.emplace(foregroundHook,
            [this] { repairOrderAboveVideoProcessor(true); });
    if (ownerProcessId_)
    {
        // VP can raise a topmost fullscreen window without acquiring the
        // foreground, so a foreground-only hook misses that z-order change.
        const HWINEVENTHOOK orderHook = SetWinEventHook(
            EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr,
            configWindowOrderChanged, ownerProcessId_, 0,
            WINEVENT_OUTOFCONTEXT);
        ownerOrderEventHook_ = orderHook;
        if (orderHook)
            configForegroundHandlers.emplace(orderHook,
                [this] { repairOrderAboveVideoProcessor(false); });
    }

    if (ownerHandle_)
    {
        // A background/warm editor still needs a usable native receiver.  Do
        // not wait for the first visible show cycle: VP may already have a
        // pending reveal by the time it acknowledges this association.
        (void)winId();
        applyNativeOwner();
        publishNativeAssociation();
    }
}

ConfigEditorWindow::~ConfigEditorWindow()
{
    if (foregroundEventHook_)
    {
        const auto hook = static_cast<HWINEVENTHOOK>(foregroundEventHook_);
        configForegroundHandlers.erase(hook);
        UnhookWinEvent(hook);
    }
    if (ownerOrderEventHook_)
    {
        const auto hook = static_cast<HWINEVENTHOOK>(ownerOrderEventHook_);
        configForegroundHandlers.erase(hook);
        UnhookWinEvent(hook);
    }
	if (monitorDiscoveryThread_)
	{
		monitorDiscoveryThread_->requestInterruption();
		monitorDiscoveryThread_->wait();
		monitorDiscoveryThread_ = nullptr;
	}
	if (targetSwitchThread_)
	{
		targetSwitchThread_->wait();
		targetSwitchThread_ = nullptr;
	}
	if (activeProfileThread_)
	{
		activeProfileThread_->wait();
		activeProfileThread_ = nullptr;
	}
	if (manualCommandThread_)
	{
		manualCommandThread_->wait();
		manualCommandThread_ = nullptr;
	}
	if (revealEventNotifier_)
		revealEventNotifier_->setEnabled(false);
	if (revealEvent_)
	{
		CloseHandle(revealEvent_);
		revealEvent_ = nullptr;
	}
    removeScopedTopmost();
    clearNativeOwner();
}

void ConfigEditorWindow::setTargetRefresh(std::function<void()> refresh)
{
	targetRefresh_ = std::move(refresh);
}

QString ConfigEditorWindow::rememberedTargetName() const
{
	return rememberedTargetLabel_.trimmed().isEmpty() ?
		QStringLiteral("Last used VideoProcessor") :
		rememberedTargetLabel_.trimmed();
}

QString ConfigEditorWindow::targetSearchTitleText(bool searching) const
{
	if (rememberedTargetId_.isEmpty())
		return searching ? QStringLiteral("Looking for VideoProcessor…") :
			!discoveredTargets_.isEmpty() ?
				QStringLiteral("Choose a VideoProcessor") :
				QStringLiteral("No VideoProcessor found");
	return searching ?
		QStringLiteral("Looking for %1…").arg(rememberedTargetName()) :
		QStringLiteral("%1 is unavailable").arg(rememberedTargetName());
}

void ConfigEditorWindow::finishInitialTargetSearch()
{
	initialSearchPending_ = false;
	if (!noTarget_) return;
	if (targetSearchTitle_)
		targetSearchTitle_->setText(targetSearchTitleText(false));
	if (targetSearchHelp_)
		targetSearchHelp_->setText(rememberedTargetId_.isEmpty() ?
			(discoveredTargets_.isEmpty() ?
				QStringLiteral("Available VideoProcessors on this LAN appear in the "
					"target menu above. Select one to edit its settings.") :
				QStringLiteral("Select an available VideoProcessor from the "
					"target menu above to edit its settings.")) :
			QStringLiteral("Still looking for your last target. Select another "
				"target above if you want to edit its settings."));
	if (targetSearchProgress_) targetSearchProgress_->hide();
	populateTargetChoices();
	if (!remoteLoadError_.isEmpty())
		setStatus(remoteLoadError_, true);
	else if (rememberedTargetId_.isEmpty())
		setStatus(discoveredTargets_.isEmpty() ?
			QStringLiteral("No target is available. Checking this LAN regularly.") :
			QStringLiteral("Choose a VideoProcessor from the target menu."));
	else
		setStatus(QStringLiteral("%1 is unavailable. Still checking for it.")
			.arg(rememberedTargetName()));
}

void ConfigEditorWindow::refreshTargetChoices()
{
	if (noTarget_)
	{
		remoteLoadError_.clear();
		initialSearchPending_ = true;
		if (targetSearchTitle_)
			targetSearchTitle_->setText(targetSearchTitleText(true));
		if (targetSearchHelp_)
			targetSearchHelp_->setText(rememberedTargetId_.isEmpty() ?
				QStringLiteral("Checking for VideoProcessors on this LAN. "
					"Select a target above to edit its settings.") :
				QStringLiteral("Checking your LAN for the last selected VideoProcessor."));
		if (targetSearchProgress_) targetSearchProgress_->show();
		populateTargetChoices();
		QTimer::singleShot(5000, this,
			[this] { finishInitialTargetSearch(); });
	}
	if (targetRefresh_) targetRefresh_();
	if (noTarget_)
		setStatus(QStringLiteral("Searching for VideoProcessor targets on this LAN."));
}

void ConfigEditorWindow::setDiscoveredTargets(const QList<Target>& targets)
{
	discoveredTargets_ = targets;
	if (targetChoice_ && targetChoice_->view()->isVisible()) return;
	populateTargetChoices();
	if (!noTarget_) return;
	if (!initialSearchPending_) finishInitialTargetSearch();
	const Target* preferred = nullptr;
	Target manual;
	if (rememberedTargetId_.startsWith(QStringLiteral("manual:")))
	{
		QSettings preferences;
		manual.host = preferences.value(
			QStringLiteral("configRpc/manualHost")).toString();
		manual.port = static_cast<quint16>(preferences.value(
			QStringLiteral("configRpc/manualPort"), 41686).toUInt());
		manual.label = manual.host;
		if (!manual.host.isEmpty() && manual.port != 0)
			preferred = &manual;
	}
	else if (!rememberedTargetId_.isEmpty() &&
		rememberedTargetId_ != QStringLiteral("local"))
	{
		for (const auto& target : discoveredTargets_)
			if (target.instanceId == rememberedTargetId_)
			{
				preferred = &target;
				break;
			}
	}
	else if (rememberedTargetId_.isEmpty() && !localAvailable_ &&
		discoveredTargets_.size() == 1)
		preferred = &discoveredTargets_.front();
	if (!preferred) return;
	const QString key = preferred->instanceId + u'|' + preferred->vpVersion +
		u'|' + preferred->host + u'|' + QString::number(preferred->port);
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (key == lastAutoAttemptKey_ && now - lastAutoAttemptMs_ < 3000) return;
	lastAutoAttemptKey_ = key;
	lastAutoAttemptMs_ = now;
	selectAnotherTarget(*preferred);
}

void ConfigEditorWindow::populateTargetChoices()
{
	if (!targetChoice_) return;
	const QSignalBlocker blocked(targetChoice_);
	targetChoice_->clear();
	targetChoices_.clear();
	targetChoices_.push_back({ currentInstanceId_,
		noTarget_ ? (rememberedTargetId_.isEmpty() ?
			(initialSearchPending_ ? QStringLiteral("Searching…") :
				(discoveredTargets_.isEmpty() ? QStringLiteral("No target") :
					QStringLiteral("Choose target"))) :
			QStringLiteral("%1 (%2)").arg(rememberedTargetName(),
				initialSearchPending_ ? QStringLiteral("searching…") :
					QStringLiteral("unavailable"))) : remoteClient_ ?
		remoteName_ : QStringLiteral("This computer"),
		remoteHost_, remotePort_, {} });
	if (localAvailable_ && (remoteClient_ || noTarget_))
		targetChoices_.push_back({ {}, QStringLiteral("This computer"), {}, 41686, {} });
	for (const auto& target : discoveredTargets_)
	{
		if (localAvailable_ && (target.local ||
			target.host == QStringLiteral("127.0.0.1")))
			continue;
		if (!noTarget_ &&
			((!currentInstanceId_.isEmpty() &&
				target.instanceId == currentInstanceId_) ||
			(target.host.compare(remoteHost_, Qt::CaseInsensitive) == 0 &&
				target.port == remotePort_)))
		{
			targetChoices_.front().instanceId = target.instanceId;
			targetChoices_.front().host = target.host;
			targetChoices_.front().port = target.port;
			targetChoices_.front().vpVersion = target.vpVersion;
			currentInstanceId_ = target.instanceId;
			continue;
		}
		targetChoices_.push_back(target);
	}
	for (int index = 0; index < targetChoices_.size(); ++index)
	{
		const auto& target = targetChoices_[index];
		targetChoice_->addItem(target.label);
		targetChoice_->setItemData(targetChoice_->count() - 1,
			noTarget_ && index == 0 ?
				(rememberedTargetId_.isEmpty() ?
					QStringLiteral("Searching for VideoProcessors") :
					QStringLiteral("%1 is not connected").arg(rememberedTargetName())) :
			target.host.isEmpty() ?
				QStringLiteral("Configuration file on this computer") :
				QStringLiteral("%1:%2 · %3").arg(target.host)
					.arg(target.port).arg(target.vpVersion),
			Qt::ToolTipRole);
	}
	targetChoice_->addItem(QStringLiteral("Enter address…"));
	// Fusion's styled popup needs an explicit viewport height to expose its
	// final row instead of adding an overflow control over that choice.
	auto* list = targetChoice_->view();
	const int visibleRows = std::min(targetChoice_->count(), 12);
	const int rowHeight = std::max(1, list->sizeHintForRow(0));
	list->setMinimumHeight(visibleRows * rowHeight +
		2 * list->frameWidth() + 2);
	targetChoice_->setCurrentIndex(0);
}

void ConfigEditorWindow::enterTargetAddress()
{
	bool entered = false;
	const QString address = QInputDialog::getText(this,
		QStringLiteral("Connect to VideoProcessor"),
		QStringLiteral("Computer name or IPv4 address:"),
		QLineEdit::Normal, {}, &entered).trimmed();
	if (!entered) return;
	QString host = address;
	quint16 port = 41686;
	const int colon = address.lastIndexOf(u':');
	if (colon > 0)
	{
		bool valid = false;
		const uint parsed = address.mid(colon + 1).toUInt(&valid);
		if (!valid || parsed == 0 || parsed > 65535)
		{
			QMessageBox::warning(this, QStringLiteral("Invalid address"),
				QStringLiteral("Enter a computer name or IPv4 address, optionally followed by :port."));
			return;
		}
		host = address.left(colon);
		port = static_cast<quint16>(parsed);
	}
	if (host.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("Invalid address"),
			QStringLiteral("Enter a computer name or IPv4 address."));
		return;
	}
	selectAnotherTarget({ {}, host, host, port });
}

void ConfigEditorWindow::showTargetSwitchOverlay(const QString& label)
{
	if (!centralWidget()) return;
	auto* overlay = new QWidget(centralWidget());
	overlay->setObjectName(QStringLiteral("config.targetSwitchOverlay"));
	overlay->setAttribute(Qt::WA_StyledBackground);
	overlay->setStyleSheet(QStringLiteral("background: #0b121b;"));
	overlay->setGeometry(centralWidget()->rect());
	overlay->setFocusPolicy(Qt::StrongFocus);
	auto* layout = new QVBoxLayout(overlay);
	layout->setContentsMargins(24, 24, 24, 24);
	layout->addStretch();
	auto* title = new QLabel(label == QStringLiteral("This computer") ?
		QStringLiteral("Loading settings on this computer…") :
		QStringLiteral("Connecting to %1…").arg(label), overlay);
	title->setObjectName(QStringLiteral("config.targetSwitchTitle"));
	title->setAlignment(Qt::AlignCenter);
	title->setStyleSheet(QStringLiteral(
		"font-size: 22px; font-weight: 600; color: #f5f8ff;"));
	layout->addWidget(title);
	auto* help = new QLabel(QStringLiteral("Loading VideoProcessor settings and available devices."), overlay);
	help->setAlignment(Qt::AlignCenter);
	help->setStyleSheet(QStringLiteral("font-size: 14px; color: #a8bad0;"));
	layout->addWidget(help);
	auto* progress = new QProgressBar(overlay);
	progress->setObjectName(QStringLiteral("configTargetSearchProgress"));
	progress->setRange(0, 0);
	progress->setTextVisible(false);
	progress->setFixedSize(220, 5);
	layout->addWidget(progress, 0, Qt::AlignHCenter);
	layout->addStretch();
	targetSwitchOverlay_ = overlay;
	centralWidget()->installEventFilter(this);
	overlay->show();
	overlay->raise();
	overlay->setFocus();
}

void ConfigEditorWindow::hideTargetSwitchOverlay()
{
	if (!targetSwitchOverlay_) return;
	if (QWidget* parent = targetSwitchOverlay_->parentWidget())
		parent->removeEventFilter(this);
	delete targetSwitchOverlay_;
	targetSwitchOverlay_ = nullptr;
}

bool ConfigEditorWindow::eventFilter(QObject* watched, QEvent* event)
{
	if (targetSwitchOverlay_ && watched == targetSwitchOverlay_->parent() &&
		event->type() == QEvent::Resize)
		targetSwitchOverlay_->setGeometry(
			static_cast<QWidget*>(watched)->rect());
	return QMainWindow::eventFilter(watched, event);
}

void ConfigEditorWindow::beginRemoteTargetSwitch(const Target& target)
{
	targetSwitchPending_ = true;
	remoteLoadError_.clear();
	showTargetSwitchOverlay(target.label.section(QStringLiteral(" ("), 0, 0));
	struct Result
	{
		std::string path;
		std::string bytes;
		std::string error;
		ConfigurationRpcClient::Capabilities capabilities;
	};
	auto result = std::make_shared<Result>();
	targetSwitchThread_ = QThread::create([result, target]
	{
		ConfigurationRpcClient client(target.host.toStdString(), target.port);
		if (client.GetConfig(result->path, result->bytes, result->error))
			client.GetCapabilities(result->capabilities, result->error);
	});
	targetSwitchThread_->setParent(this);
	QThread* worker = targetSwitchThread_;
	connect(worker, &QThread::finished, this, [this, worker, target, result]
	{
		if (targetSwitchThread_ == worker) targetSwitchThread_ = nullptr;
		worker->deleteLater();
		finishRemoteTargetSwitch(target, std::move(result->path),
			std::move(result->bytes), std::move(result->capabilities),
			QString::fromUtf8(result->error));
	});
	worker->start();
}

void ConfigEditorWindow::finishRemoteTargetSwitch(const Target& target,
	std::string path, std::string bytes,
	ConfigurationRpcClient::Capabilities capabilities, const QString& error)
{
	targetSwitchPending_ = false;
	hideTargetSwitchOverlay();
	if (!error.isEmpty())
	{
		remoteLoadError_ = error;
		setStatus(error, true);
		return;
	}
	preparedRemoteSwitch_ = std::make_unique<PreparedRemoteSwitch>(
		PreparedRemoteSwitch{ std::move(path), std::move(bytes),
			std::move(capabilities) });
	selectAnotherTarget(target);
	preparedRemoteSwitch_.reset();
}

void ConfigEditorWindow::beginLocalTargetSwitch(const Target& target)
{
	targetSwitchPending_ = true;
	showTargetSwitchOverlay(QStringLiteral("This computer"));
	auto result = std::make_shared<PreparedLocalSwitch>();
	const QString path = QFileInfo(localConfigPath_).absoluteFilePath();
	targetSwitchThread_ = QThread::create([result, path]
	{
		result->document = std::make_unique<ConfigEditorCore::ConfigDocument>();
		std::wstring error;
		result->loaded = result->document->Load(path.toStdWString(), error);
		result->error = QString::fromStdWString(error);
		if (!result->loaded) return;
		const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		result->devices = discoverValues("VPDiscoverCaptureDevices");
		for (const QString& device : result->devices)
			result->connections.insert(device, discoverCaptureConnections(device));
		result->monitors = discoverValues("VPDiscoverMonitors");
		result->filteredRenderers = discoverValues("VPDiscoverRenderers", true);
		result->allRenderers = discoverValues("VPDiscoverRenderers", false);
		if (SUCCEEDED(initialized)) CoUninitialize();
	});
	targetSwitchThread_->setParent(this);
	QThread* worker = targetSwitchThread_;
	connect(worker, &QThread::finished, this, [this, worker, target, result]
	{
		if (targetSwitchThread_ == worker) targetSwitchThread_ = nullptr;
		worker->deleteLater();
		finishLocalTargetSwitch(target, std::move(result->document),
			result->loaded, result->error, result->devices,
			result->connections, result->monitors,
			result->filteredRenderers, result->allRenderers);
	});
	worker->start();
}

void ConfigEditorWindow::finishLocalTargetSwitch(const Target& target,
	std::unique_ptr<ConfigEditorCore::ConfigDocument> document,
	bool loaded, const QString& error, const QStringList& devices,
	const QMap<QString, QStringList>& connections,
	const QStringList& monitors, const QStringList& filteredRenderers,
	const QStringList& allRenderers)
{
	targetSwitchPending_ = false;
	hideTargetSwitchOverlay();
	if (!loaded)
	{
		remoteLoadError_ = error;
		setStatus(error, true);
		return;
	}
	preparedLocalSwitch_ = std::make_unique<PreparedLocalSwitch>();
	preparedLocalSwitch_->document = std::move(document);
	preparedLocalSwitch_->loaded = true;
	preparedLocalSwitch_->devices = devices;
	preparedLocalSwitch_->connections = connections;
	preparedLocalSwitch_->monitors = monitors;
	preparedLocalSwitch_->filteredRenderers = filteredRenderers;
	preparedLocalSwitch_->allRenderers = allRenderers;
	selectAnotherTarget(target);
	preparedLocalSwitch_.reset();
}

bool ConfigEditorWindow::selectAnotherTarget(const Target& target)
{
	if (targetSwitchPending_) return false;
	if (target.host.isEmpty() && !localAvailable_) return false;
	if (!noTarget_ && target.host.compare(remoteHost_, Qt::CaseInsensitive) == 0 &&
		target.port == remotePort_) return true;
	if (dirty_ && !preparedRemoteSwitch_ && !preparedLocalSwitch_)
	{
		QMessageBox prompt(this);
		prompt.setWindowTitle(QStringLiteral("Change VideoProcessor target"));
		prompt.setText(QStringLiteral("Resolve edits to the current target before switching."));
		auto* apply = prompt.addButton(QStringLiteral("Apply to current target"),
			QMessageBox::AcceptRole);
		auto* discard = prompt.addButton(QStringLiteral("Discard edits"),
			QMessageBox::DestructiveRole);
		prompt.addButton(QMessageBox::Cancel);
		prompt.exec();
		if (prompt.clickedButton() == apply)
		{
			if (!saveChanges()) return false;
		}
		else if (prompt.clickedButton() != discard) return false;
	}
	if (!preparedRemoteSwitch_ && !preparedLocalSwitch_)
	{
		if (target.host.isEmpty()) beginLocalTargetSwitch(target);
		else beginRemoteTargetSwitch(target);
		return true;
	}
	std::unique_ptr<ConfigurationRpcClient> replacement;
	if (!target.host.isEmpty())
		replacement = std::make_unique<ConfigurationRpcClient>(
			target.host.toStdString(), target.port);
	const QString previousHost = remoteHost_;
	const QString previousName = remoteName_;
	const QString previousInstanceId = currentInstanceId_;
	const quint16 previousPort = remotePort_;
	const bool previousNoTarget = noTarget_;
	const QString previousPath = configPath_;
	const auto previousDocument = *document_;
	const auto previousSnapshot = savedSnapshot_;
	const bool previousDirty = dirty_;
	const bool previousLoaded = configurationLoaded_;
	const bool previousMigrations = hasPendingMigrations_;
	const auto previousDevices = captureDevices_;
	const auto previousConnections = captureConnections_;
	const auto previousMonitors = monitors_;
	const auto previousFiltered = filteredRenderers_;
	const auto previousAll = allRenderers_;
	const auto previousLuts = remoteLuts_;
	auto previousClient = std::move(remoteClient_);
	remoteClient_ = std::move(replacement);
	remoteHost_ = target.host;
	remotePort_ = target.port;
	remoteName_ = target.host.isEmpty() ? QStringLiteral("This computer") :
		target.label.section(QStringLiteral(" ("), 0, 0);
	currentInstanceId_ = target.instanceId;
	noTarget_ = false;
	if (!remoteClient_) configPath_ = localConfigPath_;
	loadConfiguration();
	if (!remoteLoadError_.isEmpty() || !configurationLoaded_)
	{
		remoteClient_ = std::move(previousClient);
		remoteHost_ = previousHost;
		remoteName_ = previousName;
		currentInstanceId_ = previousInstanceId;
		remotePort_ = previousPort;
		noTarget_ = previousNoTarget;
		configPath_ = previousPath;
		*document_ = previousDocument;
		savedSnapshot_ = previousSnapshot;
		dirty_ = previousDirty;
		configurationLoaded_ = previousLoaded;
		hasPendingMigrations_ = previousMigrations;
		captureDevices_ = previousDevices;
		captureConnections_ = previousConnections;
		monitors_ = previousMonitors;
		filteredRenderers_ = previousFiltered;
		allRenderers_ = previousAll;
		remoteLuts_ = previousLuts;
		setStatus(remoteLoadError_, true);
		return false;
	}
	if (!testMode_)
	{
		QSettings preferences;
		preferences.setValue(QStringLiteral("configRpc/selectedLabel"),
			remoteName_);
		if (target.host.isEmpty())
			preferences.setValue(QStringLiteral("configRpc/selectedInstanceId"),
				QStringLiteral("local"));
		else if (!target.instanceId.isEmpty())
			preferences.setValue(QStringLiteral("configRpc/selectedInstanceId"),
				target.instanceId);
		else
		{
			preferences.setValue(QStringLiteral("configRpc/selectedInstanceId"),
				QStringLiteral("manual:%1:%2").arg(target.host).arg(target.port));
			preferences.setValue(QStringLiteral("configRpc/manualHost"), target.host);
			preferences.setValue(QStringLiteral("configRpc/manualPort"), target.port);
		}
	}
	dirty_ = false;
	migrateLldvInputPolicy();
	migrateSharedRefreshRate();
	migrateRefreshRateSwitchMode();
	migrateSeparatedRendererProfiles();
	migrateUnifiedColorOutputProfiles();
	migrateCalibrationProfiles();
	migrateViewportZoomProfiles();
	loadDiscoveryCache();
	preparedRemoteSwitch_.reset();
	preparedLocalSwitch_.reset();
	rebuildConfigurationShell();
	if (activeProfileTimer_)
	{
		activeProfileTimer_->setInterval(remoteClient_ ? 3000 : 500);
		if (isVisible()) refreshActiveProfileIndicators();
	}
	setWindowTitle(QStringLiteral("VideoProcessor Configuration · %1")
		.arg(remoteName_));
	if (trayOpenAction_)
		trayOpenAction_->setText(QStringLiteral("Open Configuration (%1)")
			.arg(remoteName_));
	setStatus(remoteLoadError_.isEmpty() ?
		QStringLiteral("Connected to %1.").arg(remoteName_) :
		remoteLoadError_, !remoteLoadError_.isEmpty());
	return true;
}

void ConfigEditorWindow::selectPage(int index)
{
    if (!pages_ || index < 0 || index >= pages_->count()) return;
    if (noTarget_ && index != 20) index = 21;
    if (index == 13) index = 16; // Preserve old Output page links.
    pages_->setCurrentIndex(index);
    if (!navigation_) return;
    const int navigationIndex = index == 4 || index == 11 || index == 13 ? 2 :
        index == 12 ? 3 :
        index == 8 || index == 9 ? 8 : index;
    for (QAbstractButton* button : navigation_->findChildren<QAbstractButton*>())
    {
        const bool selected = button->property("pageIndex").isValid() &&
            button->property("pageIndex").toInt() == navigationIndex;
        if (button->isCheckable()) button->setChecked(selected);
        if (selected)
        {
            QWidget* container = button->parentWidget();
            if (container && container->property("navChildren").toBool())
            {
                container->setVisible(true);
                if (auto* header = qobject_cast<QToolButton*>(container->property("navHeader").value<QObject*>()))
                    header->setArrowType(Qt::DownArrow);
            }
        }
    }
}

void ConfigEditorWindow::loadConfiguration()
{
    std::wstring error;
	if (noTarget_)
	{
		document_ = std::make_unique<ConfigEditorCore::ConfigDocument>();
		configurationLoaded_ = false;
		dirty_ = false;
		hasPendingMigrations_ = false;
		remoteLoadError_.clear();
		return;
	}
	if (remoteClient_)
	{
		std::string path;
		std::string bytes;
		std::string rpcError;
		if (preparedRemoteSwitch_)
		{
			path = preparedRemoteSwitch_->path;
			bytes = preparedRemoteSwitch_->bytes;
		}
		else if (!remoteClient_->GetConfig(path, bytes, rpcError))
		{
			remoteLoadError_ = QString::fromUtf8(rpcError);
			return;
		}
		auto loaded = std::make_unique<ConfigEditorCore::ConfigDocument>();
		loaded->LoadBytes(QString::fromUtf8(path).toStdWString(), bytes);
		configPath_ = QString::fromUtf8(path);
		document_ = std::move(loaded);
		configurationLoaded_ = true;
		remoteLoadError_.clear();
	}
	else
	{
		if (preparedLocalSwitch_)
		{
			document_ = std::move(preparedLocalSwitch_->document);
			configurationLoaded_ = preparedLocalSwitch_->loaded;
			remoteLoadError_ = preparedLocalSwitch_->error;
		}
		else
		{
			document_ = std::make_unique<ConfigEditorCore::ConfigDocument>();
			configurationLoaded_ = document_->Load(
				QFileInfo(configPath_).absoluteFilePath().toStdWString(), error);
			remoteLoadError_ = configurationLoaded_ ? QString() :
				QString::fromStdWString(error);
		}
	}
	hasPendingMigrations_ = false;
	savedSnapshot_ = configurationLoaded_ ? captureDocumentSnapshot(*document_) :
		DocumentSnapshot{};
	bool migratedLegacyInputPolicy = false;
	if (configurationLoaded_)
	{
		for (const char* inputKey : { "video_conversion", "container_colorspace",
			"hdr_colorspace", "hdr_luminance" })
		{
			const std::string vpRendererInput =
				document_->Get("vprenderer.input_processing", inputKey);
			const std::string interimVpRendererInput =
				document_->Get("vprenderer.input", inputKey);
			const std::string legacyVpRenderer =
				document_->Get("vprenderer", inputKey);
			if (!interimVpRendererInput.empty())
			{
				if (vpRendererInput.empty())
				{
					document_->AddSection("vprenderer.input_processing");
					document_->SetKnown("vprenderer.input_processing", inputKey,
						interimVpRendererInput);
				}
				document_->RemoveKnown("vprenderer.input", inputKey);
				migratedLegacyInputPolicy = true;
			}
			if (!legacyVpRenderer.empty())
			{
				if (vpRendererInput.empty() && interimVpRendererInput.empty())
				{
					document_->AddSection("vprenderer.input_processing");
					document_->SetKnown("vprenderer.input_processing", inputKey,
						legacyVpRenderer);
				}
				document_->RemoveKnown("vprenderer", inputKey);
				migratedLegacyInputPolicy = true;
			}
		}
		if (document_->SectionSettings("vprenderer.input").empty())
			document_->RemoveSection("vprenderer.input");
		if (document_->SectionSettings("vprenderer").empty())
			document_->RemoveSection("vprenderer");
	}
	dirty_ = configurationLoaded_ && (!document_->existedAtLoad ||
		migratedLegacyInputPolicy);
	hasPendingMigrations_ = migratedLegacyInputPolicy;
    if (configurationLoaded_ && !document_->existedAtLoad)
    {
        seedCalibratedProfile(QStringLiteral("vprenderer"), QStringLiteral("vprenderer.Default"));
        seedCalibratedProfile(QStringLiteral("vprenderer.color"), QStringLiteral("vprenderer.color.Default"));
    }
}

void ConfigEditorWindow::migrateUnifiedColorOutputProfiles()
{
    if (!configurationLoaded_ || !document_) return;
    ColorOutputProfileMigration::Sections sections;
    std::vector<std::string> order;
    std::map<std::string, std::string> originalNames;
    for (const auto& section : document_->SectionNames())
    {
        const auto normalized = ConfigFile::NormalizeName(section);
        order.push_back(normalized);
        originalNames[normalized] = section;
        auto& values = sections[normalized];
        for (const auto& setting : document_->SectionSettings(section))
            values[ConfigFile::NormalizeName(setting.first)] = setting.second;
    }
    const auto plan = ColorOutputProfileMigration::Build(sections, order);
    if (plan.Empty()) return;
    for (const auto& item : plan.additions)
    {
        const auto existing = originalNames.find(item.first);
        const auto destination = existing == originalNames.end() ? item.first : existing->second;
        document_->AddSection(destination);
        for (const auto& setting : item.second)
            document_->SetKnown(destination, setting.first.c_str(), setting.second);
    }
    for (const auto& item : plan.archives)
        document_->RenameSection(originalNames.at(item.first), item.second);
    document_->requiresMigrationBackup = document_->existedAtLoad;
    dirty_ = true;
    hasPendingMigrations_ = true;
}

void ConfigEditorWindow::migrateCalibrationProfiles()
{
    if (!configurationLoaded_ || !document_) return;
    const bool profilesChanged = document_->MigrateCalibrationProfiles();
    const bool transfersChanged = document_->MigrateTransferChoices();
    if (profilesChanged || transfersChanged)
    {
        dirty_ = true;
        hasPendingMigrations_ = true;
    }
}

void ConfigEditorWindow::seedCalibratedProfile(const QString& root, const QString& section)
{
    if (!document_) return;
    document_->AddSection(section.toStdString());
    const auto set = [&](const char* key, const char* value)
    { document_->SetKnown(section.toStdString(), key, value); };
    if (root == QStringLiteral("vprenderer"))
    {
        set("quality", "high"); set("sdr_target_nits", "100");
        set("sdr_black_nits", "0"); set("display_bit_depth", "10");
        set("dithering", "auto");
    }
    else if (root == QStringLiteral("vprenderer.color"))
    {
        set("target_primaries", "rec709"); set("output_gamma", "2.2");
        set("calibration_lut_enabled", "false"); set("hdr_tone_map_target_gamma", "2.2");
        set("calibration_lut_bt709", "none"); set("calibration_lut_p3_d65", "none");
        set("calibration_lut_bt2020", "none");
        set("sdr_input_transfer", "2.4"); set("sdr_adjust_gamma", "passthrough");
        set("sdr_lut_input_gamma", "passthrough");
        set("report_bt2020_to_display", "false");
        set("output_range", "full"); set("output_transport_gamma", "2.2");
        set("output_presentation", "auto"); set("diagnostic_allow_limited_g22", "false");
    }
}

QMap<QString, QString> ConfigEditorWindow::outputTransportSelections() const
{
    QMap<QString, QString> result;
    const QStringList profiles = profileSections(QStringLiteral("vprenderer.color"));
    if (profiles.isEmpty()) return result;
    for (const QString& section : profiles)
    {
        const auto effective = [&](const char* key)
        {
            QString setting = value(section, QString::fromLatin1(key));
            if (setting.isEmpty()) setting = value(profiles.front(), QString::fromLatin1(key));
            return setting.trimmed().toLower();
        };
        result.insert(section, effective("output_range") + u'/' + effective("output_transport_gamma"));
    }
    return result;
}

void ConfigEditorWindow::synchronizeLimitedTransportFlags(
    const QMap<QString, QString>& before, const QString& editedSection)
{
    if (!document_) return;
    const auto after = outputTransportSelections();
    for (auto it = after.cbegin(); it != after.cend(); ++it)
    {
        // Repair only the explicitly edited profile or profiles whose effective
        // inherited transport actually changed. Unrelated legacy gates survive.
        if (it.key() != editedSection && before.contains(it.key()) && before.value(it.key()) == it.value()) continue;
        document_->SetKnown(it.key().toStdString(), "diagnostic_allow_limited_g22",
            it.value() == QStringLiteral("limited/2.2") ? "true" : "false");
    }
}

void ConfigEditorWindow::refreshLimitedTransportControls()
{
    auto* range = findChild<QComboBox*>(QStringLiteral("config.vprenderer.color.output_range"));
    auto* gamma = findChild<QComboBox*>(QStringLiteral("config.vprenderer.color.output_transport_gamma"));
    auto* flag = findChild<QCheckBox*>(QStringLiteral("config.vprenderer.color.diagnostic_allow_limited_g22"));
    if (!range || !gamma || !flag || !document_) return;
    const QString section = range->property("profileSection").toString();
    const QStringList profiles = profileSections(QStringLiteral("vprenderer.color"));
    const auto effective = [&](const char* key)
    {
        QString result = value(section, QString::fromLatin1(key));
        if (result.isEmpty() && !profiles.isEmpty()) result = value(profiles.front(), QString::fromLatin1(key));
        return result.trimmed().toLower();
    };
    const bool limited = effective("output_range") == QStringLiteral("limited");
    auto* notice = findChild<QLabel*>(QStringLiteral("config.vprenderer.color.transport_gamma_notice"));
    auto* match = findChild<QPushButton*>(QStringLiteral("config.vprenderer.color.match_transport_gamma"));
    const QString display = effective("output_gamma");
    const QString savedTransport = effective("output_transport_gamma");
    const QString transport = savedTransport.isEmpty() ? QStringLiteral("2.2") :
        savedTransport == QStringLiteral("auto") ? QStringLiteral("2.4") : savedTransport;
    const bool lut = effective("calibration_lut_enabled") == QStringLiteral("true");
    const bool canMatch = limited && !lut &&
        (display == QStringLiteral("2.2") || display == QStringLiteral("2.4")) && display != transport;
    if (match)
    {
        match->setVisible(canMatch);
        match->setEnabled(canMatch);
        match->setProperty("matchingGamma", canMatch ? display : QString());
        match->setText(QStringLiteral("Set Limited transfer to %1").arg(display));
    }
    if (notice)
    {
        QString text;
        if (limited)
        {
            if (lut)
                text = QStringLiteral("Selected profile: the LUT determines the final output response. Its input gamma does not establish its output gamma; transport agreement cannot be inferred. Check the live status for LUT fallback.");
            else if (display.isEmpty() || display == QStringLiteral("auto"))
                text = QStringLiteral("Selected profile: display gamma follows the accepted output. Check live status for the rendered encoding and DXGI declaration.");
            else if (display != transport)
                text = QStringLiteral("Selected profile: gamma mismatch - display gamma %1, Limited transfer %2. Changing the transport declaration does not change calibrated pixels.").arg(display, transport);
            else
                text = QStringLiteral("Selected profile: display gamma and Limited transfer are both %1. This does not verify the display signal.").arg(display);
            if (transport == QStringLiteral("2.2"))
                text += QStringLiteral(" Windows G22 is not an exact pure-2.2 declaration; this remains the beta transport path.");
            if (!lut && !canMatch && !display.isEmpty() && display != transport && display != QStringLiteral("auto"))
                text += QStringLiteral(" Limited transport has no matching declaration for this display gamma.");
        }
        notice->setText(text);
        notice->setVisible(!text.isEmpty());
    }
    gamma->setEnabled(!section.isEmpty() && limited);
    const QSignalBlocker blocker(flag);
    flag->setChecked(effective("diagnostic_allow_limited_g22") == QStringLiteral("true"));
    flag->setEnabled(false);
    const bool desired = limited && effective("output_transport_gamma") == QStringLiteral("2.2");
    flag->setToolTip(desired != flag->isChecked() ?
        QStringLiteral("Saved flag differs from the selected transport. Reselect the range or transfer to synchronize it; opening this profile preserves the saved behavior.") :
        QStringLiteral("Saved automatically: enabled only for Limited + 2.2. This is not an independent setting."));
}

void ConfigEditorWindow::migrateViewportZoomProfiles()
{
    if (!configurationLoaded_ || !document_) return;

    const QString legacyRoot = QStringLiteral("vprenderer.viewport");
    const QStringList legacySections = profileSections(legacyRoot);
    if (legacySections.isEmpty()) return;

    const QStringList zoomKeys = {
        QStringLiteral("automatic_crop"),
        QStringLiteral("crop_narrower_content_to_fill_screen"),
        QStringLiteral("crop_narrower_content_aspect_limit"),
        QStringLiteral("crop_wider_content_to_fill_screen"),
        QStringLiteral("crop_wider_content_aspect_limit"),
		QStringLiteral("fixed_crop_aspect"),
        QStringLiteral("hdr_peak_analysis_picture_only"),
		QStringLiteral("hdr_peak_analysis_motion_compensation"),
		QStringLiteral("hdr_peak_analysis_height_percent"),
		QStringLiteral("hdr_peak_analysis_position") };
    const QStringList screenKeys = {
        QStringLiteral("screen_aspect"),
        QStringLiteral("vertical_alignment"),
        QStringLiteral("anamorphic_enabled"),
        QStringLiteral("anamorphic_scale") };

    bool migrated = false;
    for (const QString& legacySection : legacySections)
    {
        QStringList configuredZoomKeys;
        for (const QString& key : zoomKeys)
            if (!value(legacySection, key).isEmpty())
                configuredZoomKeys.push_back(key);
        if (configuredZoomKeys.isEmpty()) continue;

        const QString suffix = legacySection.compare(legacyRoot,
            Qt::CaseInsensitive) == 0 ? QStringLiteral("profile_1") :
            legacySection.mid(legacyRoot.size() + 1);
        const QString zoomSection = QStringLiteral("vprenderer.zoom.%1")
            .arg(suffix);
        document_->AddSection(zoomSection.toStdString());
        const QString label = value(legacySection, QStringLiteral("label"));
        if (!label.isEmpty() && value(zoomSection,
            QStringLiteral("label")).isEmpty())
            document_->SetKnown(zoomSection.toStdString(), "label",
                label.toLocal8Bit().constData());
        for (const QString& key : configuredZoomKeys)
        {
            const QString configured = value(legacySection, key);
            if (value(zoomSection, key).isEmpty())
                document_->SetKnown(zoomSection.toStdString(),
                    key.toLocal8Bit().constData(),
                    configured.toLocal8Bit().constData());
            document_->RemoveKnown(legacySection.toStdString(),
                key.toLocal8Bit().constData());
        }
        // A legacy viewport shortcut selected all viewport settings. Copy it
        // only to the new Zoom profile when that legacy profile actually
        // owned Zoom fields. This preserves F2's matching Scope zoom profile
        // while letting Shift+2 affect Zoom alone.
        for (const QString& selector : { QStringLiteral("shortcut"),
            QStringLiteral("cycle_shortcut"), QStringLiteral("when") })
        {
            const QString configured = value(legacySection, selector);
            if (!configured.isEmpty() && value(zoomSection, selector).isEmpty())
                document_->SetKnown(zoomSection.toStdString(),
                    selector.toLocal8Bit().constData(),
                    configured.toLocal8Bit().constData());
        }

        bool hasScreenSetting = false;
        for (const QString& key : screenKeys)
            if (!value(legacySection, key).isEmpty())
            {
                hasScreenSetting = true;
                break;
            }
        // A crop-only legacy profile must not remain a selectable Screen
        // profile. If it did, Shift+2 would still replace F2's Scope screen
        // with the inherited 16:9 baseline.
        if (!hasScreenSetting)
            document_->RemoveSection(legacySection.toStdString());
        migrated = true;
    }
    if (migrated)
    {
        dirty_ = true;
        hasPendingMigrations_ = true;
    }
}

void ConfigEditorWindow::migrateSeparatedRendererProfiles()
{
    if (!configurationLoaded_ || !document_) return;

    // Scaling, Color, and Output used to live together in Rendering.  They
    // are now independent profile families, so moving settings must also
    // remove a source profile which contains nothing except its old selector.
    // Leaving that shell behind is not benign: its shortcut would still select
    // Rendering and replace an unrelated active Rendering baseline.
    struct Transfer
    {
        const char* targetRoot;
        QStringList keys;
    };
    const std::vector<Transfer> transfers = {
        { "vprenderer.output", { QStringLiteral("output_path_profile"),
            QStringLiteral("output_presentation"),
            QStringLiteral("output_range"),
            QStringLiteral("output_transport_gamma"),
            QStringLiteral("output_diagnostics"),
            QStringLiteral("diagnostic_allow_limited_g22"),
            QStringLiteral("diagnostic_allow_full_g22"),
            QStringLiteral("diagnostic_disable_compute"),
            QStringLiteral("diagnostic_force_8bit_sdr_swapchain"),
            QStringLiteral("diagnostic_vp_owned_dxgi_presenter"),
            QStringLiteral("diagnostic_disable_shader_cache") } },
        { "vprenderer.color", { QStringLiteral("target_primaries"),
            QStringLiteral("output_gamma"),
            QStringLiteral("report_bt2020_to_display"),
            QStringLiteral("sdr_adjust_gamma"),
            QStringLiteral("sdr_input_transfer") } },
        { "vprenderer.scaling", { QStringLiteral("upscaler"),
            QStringLiteral("downscaler"), QStringLiteral("sigmoid") } }
    };

    bool migrated = false;
    const QString renderingRoot = QStringLiteral("vprenderer");
    const QStringList legacySections = profileSections(renderingRoot);
    for (const QString& renderingSection : legacySections)
    {
        const QString suffix = renderingSection.compare(renderingRoot,
            Qt::CaseInsensitive) == 0 ? QStringLiteral("Default") :
            renderingSection.mid(renderingRoot.size() + 1);
        for (const Transfer& transfer : transfers)
        {
            QStringList configuredKeys;
            for (const QString& key : transfer.keys)
                if (!value(renderingSection, key).isEmpty())
                    configuredKeys.push_back(key);
            if (configuredKeys.isEmpty()) continue;

            const QString targetSection = QString::fromLatin1(transfer.targetRoot) +
                QStringLiteral(".%1").arg(suffix);
            document_->AddSection(targetSection.toStdString());
            for (const QString& key : configuredKeys)
            {
                const QString configured = value(renderingSection, key);
                if (value(targetSection, key).isEmpty())
                    document_->SetKnown(targetSection.toStdString(),
                        key.toStdString().c_str(),
                        configured.toLocal8Bit().constData());
                document_->RemoveKnown(renderingSection.toStdString(),
                    key.toStdString().c_str());
            }
            // A legacy selector selected all of its settings.  Copy it only to
            // a target family that actually received settings from this
            // profile; the old family is pruned below when it owns nothing.
            for (const QString& selector : { QStringLiteral("shortcut"),
                QStringLiteral("cycle_shortcut"), QStringLiteral("when") })
            {
                const QString configured = value(renderingSection, selector);
                if (!configured.isEmpty() && value(targetSection, selector).isEmpty())
                    document_->SetKnown(targetSection.toStdString(),
                        selector.toStdString().c_str(),
                        configured.toLocal8Bit().constData());
            }
            // Before Output had its own family, output_gamma doubled as the
            // transport gamma for a limited RGB experiment. Preserve that
            // legacy meaning before Color takes ownership of output_gamma.
            if (QString::fromLatin1(transfer.targetRoot) ==
                QStringLiteral("vprenderer.output") &&
                value(targetSection, QStringLiteral("output_range")).compare(
                    QStringLiteral("limited"), Qt::CaseInsensitive) == 0 &&
                value(targetSection, QStringLiteral("output_transport_gamma")).isEmpty())
            {
                const QString legacyGamma = value(renderingSection,
                    QStringLiteral("output_gamma")).toLower();
                if (legacyGamma == QStringLiteral("2.2") ||
                    legacyGamma == QStringLiteral("2.4"))
                    document_->SetKnown(targetSection.toStdString(),
                        "output_transport_gamma",
                        legacyGamma.toLocal8Bit().constData());
            }
            migrated = true;
        }

        bool hasRenderingSetting = false;
        for (const auto& setting : document_->SectionSettings(
            renderingSection.toStdString()))
        {
            const QString key = QString::fromLocal8Bit(setting.first.c_str());
            if (key.compare(QStringLiteral("shortcut"), Qt::CaseInsensitive) != 0 &&
                key.compare(QStringLiteral("when"), Qt::CaseInsensitive) != 0)
            {
                hasRenderingSetting = true;
                break;
            }
        }
        if (!hasRenderingSetting &&
            document_->RemoveSection(renderingSection.toStdString()))
            migrated = true;
    }
    if (migrated)
    {
        dirty_ = true;
        hasPendingMigrations_ = true;
    }
}

void ConfigEditorWindow::loadDiscoveryCache()
{
	if (remoteClient_)
	{
		captureDevices_.clear();
		captureConnections_.clear();
		monitors_.clear();
		filteredRenderers_.clear();
		allRenderers_.clear();
		remoteLuts_.clear();
		ConfigurationRpcClient::Capabilities offered;
		std::string error;
		if (preparedRemoteSwitch_)
			offered = preparedRemoteSwitch_->capabilities;
		else if (!remoteClient_->GetCapabilities(offered, error))
		{
			remoteLoadError_ = QString::fromUtf8(error);
			return;
		}
		const auto strings = [](const std::vector<std::string>& source)
		{
			QStringList result;
			for (const auto& item : source) result.push_back(QString::fromUtf8(item));
			return result;
		};
		captureDevices_ = strings(offered.captureDevices);
		for (const auto& entry : offered.captureConnections)
			captureConnections_.insert(QString::fromUtf8(entry.first),
				strings(entry.second));
		monitors_ = strings(offered.monitors);
		filteredRenderers_ = strings(offered.filteredRenderers);
		allRenderers_ = strings(offered.allRenderers);
		remoteLuts_ = strings(offered.luts);
		return;
	}
	if (preparedLocalSwitch_)
	{
		captureDevices_ = preparedLocalSwitch_->devices;
		captureConnections_ = preparedLocalSwitch_->connections;
		monitors_ = preparedLocalSwitch_->monitors;
		filteredRenderers_ = preparedLocalSwitch_->filteredRenderers;
		allRenderers_ = preparedLocalSwitch_->allRenderers;
		remoteLuts_.clear();
		return;
	}
    // Hardware discovery can enumerate COM registrations and capture/display
    // devices. Do it once before the pages are created; General, Actions, and
    // Shortcuts then share an immutable startup snapshot.
    captureDevices_ = discoverValues("VPDiscoverCaptureDevices");
    for (const QString& captureDevice : captureDevices_)
        captureConnections_.insert(captureDevice,
            discoverCaptureConnections(captureDevice));
    monitors_ = discoverValues("VPDiscoverMonitors");
    filteredRenderers_ = discoverValues("VPDiscoverRenderers", true);
    allRenderers_ = discoverValues("VPDiscoverRenderers", false);
}

void ConfigEditorWindow::migrateLldvInputPolicy()
{
    if (!configurationLoaded_ || !document_) return;

    QStringList sections = profileSections(QStringLiteral("lldv"));
    if (sections.isEmpty()) return;

    const QString section = sections.front();

    bool hasMetadata = false;
    for (const char* key : { "max_cll", "max_fall",
        "mastering_min_luminance", "mastering_max_luminance" })
    {
        if (!document_->Get(section.toStdString().c_str(), key).empty())
        {
            hasMetadata = true;
            break;
        }
    }

    // Explicit LLDV metadata is an unambiguous request to use VP's established
    // LLDV-aware follow modes. Previously Config displayed and saved these
    // values while ordinary FOLLOW_INPUT silently bypassed all four of them.
    const auto normalizedGeneral = [this](const char* key)
    {
        return QString::fromLocal8Bit(document_->Get("general", key).c_str())
            .trimmed().toLower();
    };
    if (hasMetadata && normalizedGeneral("hdr_colorspace") == QStringLiteral("follow_input") &&
        normalizedGeneral("hdr_luminance") == QStringLiteral("follow_input"))
    {
        document_->SetKnown("general", "hdr_colorspace", "FOLLOW_INPUT_LLDV");
        document_->SetKnown("general", "hdr_luminance", "FOLLOW_INPUT_LLDV");
        dirty_ = true;
        hasPendingMigrations_ = true;
    }
}

void ConfigEditorWindow::migrateSharedRefreshRate()
{
    if (!configurationLoaded_ || !document_) return;

    const QString root = QStringLiteral("vprenderer");
    const QString key = QStringLiteral("switch_refresh_rate");
    QString shared = value(QStringLiteral("general"), key);
    bool migrated = false;
    for (const QString& section : profileSections(root))
    {
        const QString configured = value(section, key);
        if (configured.isEmpty()) continue;
        if (shared.isEmpty()) shared = configured;
        document_->RemoveKnown(section.toStdString(), key.toStdString().c_str());
        migrated = true;
    }
    if (migrated && !shared.isEmpty())
    {
        document_->AddSection("general");
        document_->SetKnown("general", key.toStdString().c_str(),
            shared.toLocal8Bit().constData());
        dirty_ = true;
        hasPendingMigrations_ = true;
    }
    if (document_->SectionSettings(root.toStdString()).empty())
        document_->RemoveSection(root.toStdString());
}

void ConfigEditorWindow::migrateRefreshRateSwitchMode()
{
	if (!configurationLoaded_ || !document_) return;
	const QString raw = value(QStringLiteral("general"),
		QStringLiteral("switch_refresh_rate")).trimmed().toLower();
	QString migrated = raw;
	if (raw.isEmpty() || raw == QStringLiteral("true") || raw == QStringLiteral("yes") ||
		raw == QStringLiteral("on") || raw == QStringLiteral("1"))
		migrated = QStringLiteral("fullscreen_only");
	else if (raw == QStringLiteral("false") || raw == QStringLiteral("no") ||
		raw == QStringLiteral("off") || raw == QStringLiteral("0"))
		migrated = QStringLiteral("never");
	if (migrated != raw)
	{
		document_->AddSection("general");
		document_->SetKnown("general", "switch_refresh_rate",
			migrated.toLocal8Bit().constData());
		dirty_ = true;
		hasPendingMigrations_ = true;
	}
}

void ConfigEditorWindow::installProfileContextMenu(QListWidget* list,
	const QString& sectionPrefix)
{
	list->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(list, &QListWidget::customContextMenuRequested, this,
		[this, list, sectionPrefix](const QPoint& point)
		{
			QListWidgetItem* item = list->itemAt(point);
			if (!item) return;
			QString group, name, label;
			bool enabled = true;
			if (!liveProfileCommand(sectionPrefix, item, group, name, enabled, label)) return;
			QMenu menu(list);
			QAction* command = menu.addAction(label);
			command->setEnabled(!noTarget_ && !manualCommandThread_);
			command->setToolTip(QStringLiteral(
				"Uses the configuration currently saved in VideoProcessor."));
			if (menu.exec(list->viewport()->mapToGlobal(point)) == command)
				runTargetCommand(group, name, enabled, false);
		});
}

bool ConfigEditorWindow::liveProfileCommand(const QString& sectionPrefix,
	const QListWidgetItem* item, QString& group, QString& name, bool& enabled,
	QString& label) const
{
			if (!item) return false;
			const QString section = item->data(Qt::UserRole).toString();
			if (section.isEmpty()) return false;
			const QMap<QString, QString> groups{
				{ QStringLiteral("queue"), QStringLiteral("queue") },
				{ QStringLiteral("vprenderer"), QStringLiteral("display") },
				{ QStringLiteral("vprenderer.color"), QStringLiteral("color") },
				{ QStringLiteral("vprenderer.scaling"), QStringLiteral("scaling") },
				{ QStringLiteral("vprenderer.output"), QStringLiteral("output") },
				{ QStringLiteral("vprenderer.viewport"), QStringLiteral("viewport") },
				{ QStringLiteral("vprenderer.zoom"), QStringLiteral("zoom") },
                { QStringLiteral("vprenderer.subtitles"), QStringLiteral("subtitles") },
				{ QStringLiteral("lldv"), QStringLiteral("lldv") },
				{ QStringLiteral("shader.nls"), QStringLiteral("nls") },
				{ QStringLiteral("shader.standard"), QStringLiteral("standard_shaders") }
			};
			group = groups.value(sectionPrefix);
			if (group.isEmpty() ||
				(section.compare(sectionPrefix, Qt::CaseInsensitive) != 0 &&
				 !section.startsWith(sectionPrefix + u'.', Qt::CaseInsensitive)))
				return false;
			name = section.compare(sectionPrefix,
				Qt::CaseInsensitive) == 0 ? QStringLiteral("base") :
				section.mid(sectionPrefix.size() + 1);
			const bool standard = group == QStringLiteral("standard_shaders");
			const bool defaultShader = standard && name == QStringLiteral("base");
			enabled = !standard || defaultShader ||
				!item->data(ActiveProfileRole).toBool();
			label = standard && !defaultShader ?
				(enabled ? QStringLiteral("Enable in VP") :
					QStringLiteral("Disable in VP")) :
				QStringLiteral("Activate in VP");
			return true;
}

QPushButton* ConfigEditorWindow::addLiveToggle(QListWidget* list, const QString& sectionPrefix)
{
	auto* toggle = new QPushButton(QStringLiteral("Switch on in VP"));
	toggle->setObjectName(controlName(sectionPrefix, QStringLiteral("live_toggle")));
	toggle->setAccessibleDescription(QStringLiteral(
		"Switches the selected profile in the running VideoProcessor, using its saved configuration."));
	toggle->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
	for (ProfileListBinding& binding : activeProfileLists_)
		if (binding.list == list) binding.liveToggle = toggle;
	connect(toggle, &QPushButton::clicked, this, [this, list, sectionPrefix]
	{
		QString group, name, label;
		bool enabled = true;
		if (liveProfileCommand(sectionPrefix, list->currentItem(), group, name, enabled, label))
			runTargetCommand(group, name, enabled, false);
	});
	connect(list, &QListWidget::currentItemChanged, this, [this] { refreshLiveToggles(); });
	return toggle;
}

void ConfigEditorWindow::refreshLiveToggles()
{
	for (const ProfileListBinding& binding : activeProfileLists_)
	{
		if (!binding.liveToggle || !binding.list) continue;
		const QListWidgetItem* item = binding.list->currentItem();
		QString group, name, label;
		bool enabled = true;
		const bool command = liveProfileCommand(binding.sectionPrefix, item, group, name, enabled, label);
		const bool standard = group == QStringLiteral("standard_shaders") && name != QStringLiteral("base");
		const bool active = item && item->data(ActiveProfileRole).toBool();
		QString text = standard ? (active ? QStringLiteral("Switch off in VP") :
			QStringLiteral("Switch on in VP")) :
			(active ? QStringLiteral("In use in VP") : QStringLiteral("Use in VP"));
		const bool usable = command && !noTarget_ && !manualCommandThread_ && (standard || !active);
		if (binding.liveToggle->text() != text) binding.liveToggle->setText(text);
		binding.liveToggle->setEnabled(usable);
		binding.liveToggle->setToolTip(noTarget_ ?
			QStringLiteral("Start VideoProcessor (or choose it as the target) to switch this live.") :
			QStringLiteral("Takes effect in VideoProcessor at once and is remembered when VP restarts."));
	}
}

void ConfigEditorWindow::installActionContextMenu(QListWidget* list)
{
	list->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(list, &QListWidget::customContextMenuRequested, this,
		[this, list](const QPoint& point)
		{
			QListWidgetItem* item = list->itemAt(point);
			if (!item) return;
			const QString section = item->data(Qt::UserRole).toString();
			if (!section.startsWith(QStringLiteral("actions."),
				Qt::CaseInsensitive)) return;
			QMenu menu(list);
			QAction* command = menu.addAction(QStringLiteral("Run now in VP"));
			command->setToolTip(QStringLiteral(
				"Runs the saved enabled action on the selected VP without its event rule or delay."));
			const QString enabledText = value(section,
				QStringLiteral("enabled")).trimmed().toLower();
			const bool enabled = enabledText != QStringLiteral("false") &&
				enabledText != QStringLiteral("off") &&
				enabledText != QStringLiteral("no") &&
				enabledText != QStringLiteral("0");
			command->setEnabled(!noTarget_ && !manualCommandThread_ &&
				enabled);
			if (menu.exec(list->viewport()->mapToGlobal(point)) == command)
				runTargetCommand({}, section.mid(8), true, true);
		});
}

void ConfigEditorWindow::runTargetCommand(const QString& group,
	const QString& name, bool enabled, bool action)
{
	if (noTarget_ || manualCommandThread_) return;
	const QString host = remoteClient_ ? remoteHost_ : QStringLiteral("127.0.0.1");
	const quint16 port = remoteClient_ ? remotePort_ : 41686;
	const QString instanceId = currentInstanceId_;
	struct Result { bool success = false; std::string error; };
	const auto result = std::make_shared<Result>();
	setStatus(action ? QStringLiteral("Running action in VP…") :
		QStringLiteral("Selecting profile in VP…"));
	manualCommandThread_ = QThread::create([result, host, port,
		group, name, enabled, action]
	{
		ConfigurationRpcClient client(host.toStdString(), port);
		result->success = action ?
			client.RunAction(name.toLocal8Bit().toStdString(), result->error) :
			client.SelectProfile(group.toStdString(),
				name.toLocal8Bit().toStdString(), enabled, result->error);
	});
	manualCommandThread_->setParent(this);
	QThread* worker = manualCommandThread_;
	connect(worker, &QThread::finished, this,
		[this, worker, result, host, port, instanceId, name, action]
		{
			if (manualCommandThread_ == worker) manualCommandThread_ = nullptr;
			worker->deleteLater();
			refreshLiveToggles();
			if ((remoteClient_ ? remoteHost_ : QStringLiteral("127.0.0.1")) != host ||
				(remoteClient_ ? remotePort_ : 41686) != port ||
				currentInstanceId_ != instanceId) return;
			setStatus(result->success ?
				(action ? QStringLiteral("Action %1 queued in VP.") :
					QStringLiteral("Profile %1 selected in VP.")).arg(name) :
				QString::fromStdString(result->error), !result->success);
			if (result->success) refreshActiveProfileIndicators();
		});
	manualCommandThread_->start();
	refreshLiveToggles();
}

void ConfigEditorWindow::refreshActiveProfileIndicators()
{
    // A combo popup has its own transient native window. Avoid unrelated model
    // notifications while the operator is opening or selecting from it.
    if (hasActiveOwnedPopup()) return;
	if (remoteClient_)
	{
		if (activeProfileThread_ || !isVisible() || noTarget_) return;
		const QString host = remoteHost_;
		const quint16 port = remotePort_;
		const QString instanceId = currentInstanceId_;
		struct Result
		{
			ConfigurationRpcProtocol::LiveProfileStatus status;
			bool success = false;
		};
		const auto result = std::make_shared<Result>();
		activeProfileThread_ = QThread::create([result, host, port]
		{
			ConfigurationRpcClient client(host.toStdString(), port);
			std::string error;
			result->success = client.GetActiveProfileStatus(result->status, error);
		});
		activeProfileThread_->setParent(this);
		QThread* worker = activeProfileThread_;
		connect(worker, &QThread::finished, this,
			[this, worker, result, host, port, instanceId]
			{
				if (activeProfileThread_ == worker) activeProfileThread_ = nullptr;
				worker->deleteLater();
				if (!remoteClient_ || noTarget_ || !isVisible()) return;
				if (remoteHost_ != host || remotePort_ != port ||
					currentInstanceId_ != instanceId)
				{
					refreshActiveProfileIndicators();
					return;
				}
				if (hasActiveOwnedPopup()) return;
				const auto& status = result->status;
				const bool available = result->success && status.available;
				QStringList shaders;
				if (available && status.shaderAvailable)
					for (const auto& shader : status.shaders)
						shaders.push_back(QString::fromLocal8Bit(shader.c_str()));
				applyActiveProfileIndicators(available,
					QString::fromLocal8Bit(status.queue.c_str()),
					QString::fromLocal8Bit(status.renderer.c_str()),
					QString::fromLocal8Bit(status.color.c_str()),
					QString::fromLocal8Bit(status.viewport.c_str()),
					shaders, available && status.shaderAvailable,
					QString::fromLocal8Bit(status.zoom.c_str()),
					QString::fromLocal8Bit(status.scaling.c_str()),
					QString::fromLocal8Bit(status.output.c_str()),
                    QString::fromLocal8Bit(status.subtitles.c_str()));
			});
		activeProfileThread_->start();
		return;
	}

    // Config can remain in the tray across a VP restart. Its old owner HWND
    // then disappears; read the sole live publisher as a standalone editor
    // until a new VP activation associates this window with its fresh PID.
    const uint32_t expectedProcessId = nativeOwnerIsValid() ?
        ownerProcessId_ : 0;
    ActiveProfileStatus::Snapshot active;
    const bool available = ActiveProfileStatus::Read(expectedProcessId, active);
    const QString renderer = available ? QString::fromLocal8Bit(active.renderer) : QString();
    const QString color = available ? QString::fromLocal8Bit(active.color) : QString();
    const QString scaling = available ? QString::fromLocal8Bit(active.scaling) : QString();
    const QString output = available ? QString::fromLocal8Bit(active.output) : QString();
    const QString viewport = available ? QString::fromLocal8Bit(active.viewport) : QString();
    const QString subtitles = available ? QString::fromLocal8Bit(active.subtitles) : QString();
    const QString zoom = available ? QString::fromLocal8Bit(active.zoom) : QString();
    const QString queue = available ? QString::fromLocal8Bit(active.queue) : QString();
    const QString sourceEotf = available ?
        QString::fromLocal8Bit(active.sourceEotf).trimmed() : QString();
    // The renderer assumes pure gamma 2.4 for captured SDR before applying the
    // configured SDR-transfer policy. For HDR inputs, expose the actual EOTF
    // so an SDR-only setting does not imply it is currently active.
    liveSourceTransfer_ = sourceEotf.compare(QStringLiteral("SDR"),
        Qt::CaseInsensitive) == 0 ? QStringLiteral("2.4 (assumed)") : sourceEotf;
    liveCalibrationAvailable_ = available && active.calibrationStatusAvailable != 0;
    liveCalibrationLutAttached_ = liveCalibrationAvailable_ && active.calibrationLutAttached != 0;
    liveCalibrationHdrSource_ = sourceEotf.startsWith(QStringLiteral("PQ"), Qt::CaseInsensitive) ||
        sourceEotf.compare(QStringLiteral("HLG"), Qt::CaseInsensitive) == 0 ||
        sourceEotf.compare(QStringLiteral("HDR"), Qt::CaseInsensitive) == 0;
    liveCalibrationConfigIdentity_ = available ? active.calibrationConfigIdentity : 0;
    liveCalibrationConfigPath_ = available ? QString::fromLocal8Bit(active.calibrationConfigPath) : QString();
    liveCalibrationRenderer_ = renderer;
    liveCalibrationColor_ = color;
    const bool shaderAvailable = available &&
        ActiveProfileStatus::ShaderSetIsCurrent(active);
    QStringList shaders;
    if (shaderAvailable)
        for (uint32_t index = 0; index < active.shaderCount; ++index)
            shaders.push_back(QString::fromLocal8Bit(active.shaders[index]));
    applyActiveProfileIndicators(available, queue, renderer, color, viewport,
        shaders, shaderAvailable, zoom, scaling, output, subtitles);
    refreshRendererAutoStatus();
    refreshCalibrationControls();
    if (auto* label = findChild<QLabel*>(QStringLiteral("config.color_output.live_status")))
        label->setText(available && active.outputSummary[0] ?
            QStringLiteral("Live output — Color / Output: %1\n%2\nApplies to the running renderer, not unsaved edits. Windows/HDMI wire state is not measured here.")
                .arg(color, QString::fromLocal8Bit(active.outputSummary)) :
            QStringLiteral("Live output unavailable. Saved preferences are not proof of the active output contract."));
}

void ConfigEditorWindow::setActiveProfileStatusForTesting(const QString& queue,
    const QString& renderer, const QString& color, const QString& viewport,
    const QStringList& shaders, bool shaderAvailable, const QString& zoom,
    const QString& scaling, const QString& output, const QString& subtitles)
{
    applyActiveProfileIndicators(true, queue, renderer, color, viewport, shaders,
        shaderAvailable, zoom, scaling, output, subtitles);
}

void ConfigEditorWindow::applyActiveProfileIndicators(bool available,
    const QString& queue, const QString& renderer, const QString& color,
    const QString& viewport,
    const QStringList& shaders, bool shaderAvailable, const QString& zoom,
    const QString& scaling, const QString& output, const QString& subtitles)
{
    activeProfileSnapshotAvailable_ = available;
    for (ProfileListBinding& binding : activeProfileLists_)
    {
        const bool shaderList = binding.sectionPrefix.startsWith(
            QStringLiteral("shader."));
        const QString activeSection = binding.sectionPrefix == QStringLiteral("vprenderer") ? renderer :
            (binding.sectionPrefix == QStringLiteral("vprenderer.color") ? color :
                (binding.sectionPrefix == QStringLiteral("vprenderer.scaling") ? scaling :
                    (binding.sectionPrefix == QStringLiteral("vprenderer.output") ? output :
                        (binding.sectionPrefix == QStringLiteral("vprenderer.viewport") ? viewport :
                            (binding.sectionPrefix == QStringLiteral("vprenderer.zoom") ? zoom :
                                (binding.sectionPrefix == QStringLiteral("subtitles") || binding.sectionPrefix == QStringLiteral("vprenderer.subtitles") ? subtitles :
                                (binding.sectionPrefix == QStringLiteral("queue") ? queue : QString())))))));
        int activeRow = -1;
        for (int index = 0; binding.list && index < binding.list->count(); ++index)
        {
            QListWidgetItem* item = binding.list->item(index);
            const QString section = item->data(Qt::UserRole).toString();
            const bool isActive = shaderList ?
                (shaderAvailable && std::any_of(shaders.cbegin(), shaders.cend(),
                    [&section](const QString& shader)
                    {
                        return shader.compare(section, Qt::CaseInsensitive) == 0;
                    })) :
                (available && !activeSection.isEmpty() &&
                    section.compare(activeSection, Qt::CaseInsensitive) == 0);
            // QListWidget emits dataChanged even when the assigned value is
            // identical. Do not continually invalidate every profile list on
            // the UI thread for an unchanged shared-memory snapshot.
            if (item->data(ActiveProfileRole).toBool() != isActive)
                item->setData(ActiveProfileRole, isActive);
            if (isActive) activeRow = index;
        }
        // A profile list initially edits its first (default) row. Once the
        // live snapshot arrives, open the active row instead. A later snapshot
        // must never move an operator away from a row they chose to edit.
        if (!shaderList && profileSelectionReady_ &&
            binding.pendingActiveSelection)
        {
            if (binding.editSelectionTouched)
                binding.pendingActiveSelection = false;
            else if (activeRow >= 0)
            {
                selectingActiveProfile_ = true;
                binding.list->setCurrentRow(activeRow);
                selectingActiveProfile_ = false;
                binding.pendingActiveSelection = false;
            }
        }
        if (binding.selectedTitle && binding.list && binding.list->currentItem())
        {
            const QListWidgetItem* current = binding.list->currentItem();
            const QString display = current->data(Qt::UserRole + 1).toString();
            const QString title = !available ?
                QStringLiteral("%1 (Live status unavailable)").arg(display) :
                current->data(ActiveProfileRole).toBool() ?
                    QStringLiteral("%1 (Active)").arg(display) :
                    QStringLiteral("%1 (Not active)").arg(display);
            if (binding.selectedTitle->text() != title)
                binding.selectedTitle->setText(title);
        }
    }
    refreshLiveToggles();
    const QString hdrSelection=available ? viewport+QStringLiteral("\n")+zoom : QString();
    if (hdrSelection!=subtitleHdrFallbackSelection_) {
        subtitleHdrFallbackSelection_=hdrSelection;
        if (refreshSubtitleHdrFallback_) refreshSubtitleHdrFallback_();
    }

    refreshRendererAutoStatus();
}

void ConfigEditorWindow::refreshMonitorDiscovery()
{
    if (testMode_ || remoteClient_ || monitorDiscoveryThread_) return;

    // Configuration activation can coincide with a Windows display-topology
    // rebuild. Keep hardware discovery away from the UI thread; every selector
    // continues serving the cache while this refresh is in flight.
    QPointer<ConfigEditorWindow> guarded(this);
    monitorDiscoveryThread_ = QThread::create([guarded]
    {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const QStringList discovered = discoverValues("VPDiscoverMonitors");
        if (SUCCEEDED(initialized)) CoUninitialize();
        if (!guarded) return;
        QMetaObject::invokeMethod(guarded, [guarded, discovered]
        {
            if (guarded) guarded->applyMonitorDiscovery(discovered);
        }, Qt::QueuedConnection);
    });
    monitorDiscoveryThread_->setParent(this);
    QThread* worker = monitorDiscoveryThread_;
    connect(worker, &QThread::finished, this, [this, worker]
    {
        if (monitorDiscoveryThread_ == worker)
            monitorDiscoveryThread_ = nullptr;
        worker->deleteLater();
    });
    worker->start();
}

void ConfigEditorWindow::applyMonitorDiscovery(const QStringList& discovered)
{
    monitors_ = discovered;
    if (!monitorChoice_) return;

    QString selected = monitorChoice_->currentData().toString();
    if (monitorChoice_->isEditable())
    {
        const int current = monitorChoice_->currentIndex();
        if (current < 0 || monitorChoice_->currentText() !=
            monitorChoice_->itemText(current))
            selected = monitorChoice_->currentText().trimmed();
    }
    if (selected.isEmpty())
        selected = value(QStringLiteral("general"),
            QStringLiteral("fullscreen_monitor_name"));

    const QSignalBlocker blocker(monitorChoice_);
    monitorChoice_->clear();
    monitorChoice_->addItem(QStringLiteral("Default monitor"), QString());
    for (const QString& monitor : monitors_)
        monitorChoice_->addItem(monitor, monitor);

    int index = monitorChoice_->findData(selected, Qt::UserRole,
        Qt::MatchFixedString);
    if (!selected.isEmpty() && index < 0)
    {
        // A projector or secondary display can be offline while Config is
        // open. Keep the configured/typed target intact and editable; active
        // discovery is advisory, not validation.
        monitorChoice_->addItem(selected, selected);
        index = monitorChoice_->count() - 1;
    }
    monitorChoice_->setCurrentIndex(index >= 0 ? index : 0);
    if (monitorChoice_->isEditable())
        monitorChoice_->setEditText(monitorChoice_->itemText(
            monitorChoice_->currentIndex()));
}

QString ConfigEditorWindow::value(const QString& section, const QString& key, const QString& fallback) const
{
    if (!configurationLoaded_ || !document_) return fallback;
    const std::string result = document_->Get(section.toStdString().c_str(), key.toStdString().c_str());
    if (result.empty()) return fallback;
    return QString::fromLocal8Bit(result.c_str());
}

QStringList ConfigEditorWindow::profileSections(const QString& root) const
{
    QStringList result;
    if (!configurationLoaded_ || !document_) return result;
    const QString prefix = root + u'.';
    for (const std::string& raw : document_->SectionNamesWithPrefix(root.toStdString()))
    {
        const QString section = QString::fromLocal8Bit(raw.c_str());
        if (section.compare(root, Qt::CaseInsensitive) == 0 ||
            (section.startsWith(prefix, Qt::CaseInsensitive) && !section.mid(prefix.size()).contains(u'.')))
        {
            const QString suffix = section.mid(prefix.size()).toLower();
            if (root.compare(QStringLiteral("vprenderer"), Qt::CaseInsensitive) == 0 &&
                RendererProfileConfig::IsRendererChildRoot(suffix.toStdString()))
                continue;
            result.push_back(section);
        }
    }
    return result;
}

QString ConfigEditorWindow::inheritedSharedInputLabel(const QString& key) const
{
    // The General control is authoritative for the effective display value.
    // This also covers choices whose stored value is empty, such as Container
    // color space's "Follow input" policy.
    if (QComboBox* general = findChild<QComboBox*>(
        controlName(QStringLiteral("general"), key)))
    {
        const QString selected = general->currentData().toString();
        return selected.isEmpty() ? general->currentText() :
            friendlyChoiceLabel(selected);
    }

    const QString configured = value(QStringLiteral("general"), key);
    if (!configured.isEmpty()) return friendlyChoiceLabel(configured);
    if (key == QStringLiteral("container_colorspace"))
        return QStringLiteral("Follow input");
    if (key == QStringLiteral("video_conversion"))
        return QStringLiteral("Disabled");
    if (key == QStringLiteral("hdr_colorspace") ||
        key == QStringLiteral("hdr_luminance"))
        return QStringLiteral("Follow input");
    return {};
}

void ConfigEditorWindow::refreshInheritedSharedInputChoices(const QString& key)
{
    if (!isSharedInputSetting(key)) return;

    const QString inheritedLabel = inheritedSharedInputLabel(key);
    const QString text = inheritedLabel.isEmpty() ?
        QStringLiteral("Inherited / not set") :
        QStringLiteral("Inherited: %1").arg(inheritedLabel);
    const QString tooltip = inheritedLabel.isEmpty() ?
        QStringLiteral("No General value is currently set. Selecting a value creates a renderer override.") :
        QStringLiteral("Inherited from General: %1. Selecting a value creates a renderer override.")
            .arg(inheritedLabel);
    for (const QString& section : { QStringLiteral("directshow"),
        QStringLiteral("vprenderer.input_processing") })
    {
        QComboBox* combo = findChild<QComboBox*>(controlName(section, key));
        if (!combo) continue;
        const QSignalBlocker blocker(combo);
        combo->setItemText(0, text);
        if (combo->currentData().toString().trimmed().isEmpty())
        {
            combo->setToolTip(tooltip);
            combo->setProperty("inherited", true);
        }
    }
    refreshRendererAutoStatus();
}

void ConfigEditorWindow::setCalibrationStatusForTesting(bool available, bool attached,
    quint64 configIdentity, const QString& configPath,
    const QString& renderer, const QString& color, bool hdrSource)
{
    liveCalibrationAvailable_ = available;
    liveCalibrationLutAttached_ = attached;
    liveCalibrationConfigIdentity_ = configIdentity;
    liveCalibrationConfigPath_ = configPath;
    liveCalibrationRenderer_ = renderer;
    liveCalibrationColor_ = color;
    liveCalibrationHdrSource_ = hdrSource;
    refreshCalibrationControls();
}

void ConfigEditorWindow::refreshCalibrationControls()
{
    refreshLimitedTransportControls();
    auto* displayGamma = findChild<QComboBox*>(QStringLiteral("config.vprenderer.color.output_gamma"));
    auto* sourceGamma = findChild<QComboBox*>(QStringLiteral("config.vprenderer.color.sdr_input_transfer"));
    auto* conversion = findChild<QCheckBox*>(QStringLiteral("config.vprenderer.color.sdr_adjust_gamma"));
    auto* inherit = findChild<QPushButton*>(QStringLiteral("config.vprenderer.color.sdr_adjust_gamma.inherit"));
    auto* hdrGamma = findChild<QComboBox*>(QStringLiteral("config.vprenderer.color.hdr_tone_map_target_gamma"));
    const auto* enabled = findChild<QCheckBox*>(QStringLiteral("config.vprenderer.color.calibration_lut_enabled"));
    const bool lutConfigured = enabled && enabled->isChecked();
    if (auto* method = findChild<QRadioButton*>(QStringLiteral("config.vprenderer.color.calibration_method.vp")))
        method->setChecked(!lutConfigured);
    if (auto* method = findChild<QRadioButton*>(QStringLiteral("config.vprenderer.color.calibration_method.lut")))
        method->setChecked(lutConfigured);
    auto* fallback = findChild<QPushButton*>(QStringLiteral("config.vprenderer.color.edit_fallback"));
    const QString editedProfile = displayGamma ? displayGamma->property("profileSection").toString() : QString();
    if (fallback && (fallback->property("editedProfile").toString() != editedProfile || !lutConfigured))
    {
        const QSignalBlocker blocker(fallback);
        fallback->setChecked(false);
        fallback->setProperty("editedProfile", editedProfile);
    }
    const bool showFallback = lutConfigured && fallback && fallback->isChecked();
    if (fallback) fallback->setText(showFallback ? QStringLiteral("Hide fallback settings") : QStringLiteral("Edit fallback settings"));
    const auto* gamut = findChild<QComboBox*>(QStringLiteral("config.vprenderer.color.target_primaries"));
    QString selectedGamut = gamut ? gamut->currentData().toString() : QString();
    if (gamut && selectedGamut.isEmpty()) selectedGamut = gamut->property("effectiveValue").toString();
    const QString slot = selectedGamut.compare(QStringLiteral("BT2020"), Qt::CaseInsensitive) == 0 ?
        QStringLiteral("calibration_lut_bt2020") :
        selectedGamut.compare(QStringLiteral("P3_D65"), Qt::CaseInsensitive) == 0 ?
        QStringLiteral("calibration_lut_p3_d65") : QStringLiteral("calibration_lut_bt709");
    for (QWidget* control : findChildren<QWidget*>())
    {
        const bool displayRow = control->property("displayOnlyRow").toBool();
        if (!control->property("lutOnlyRow").toBool() && !displayRow) continue;
        auto* layout = qobject_cast<QFormLayout*>(control->property("calibrationForm").value<QObject*>());
        const QString key = control->property("lutConfigKey").toString();
        if (layout) layout->setRowVisible(control, displayRow ? (!lutConfigured || showFallback) :
            lutConfigured && (key.isEmpty() || key == slot));
    }

    if (enabled)
        for (const QString& key : { QStringLiteral("calibration_lut_bt709"), QStringLiteral("calibration_lut_p3_d65"),
            QStringLiteral("calibration_lut_bt2020"), QStringLiteral("calibration_lut.open_folder") })
            if (auto* control = findChild<QWidget*>(controlName(QStringLiteral("vprenderer.color"), key)))
                control->setEnabled(lutConfigured);
    if (!displayGamma || !conversion || !hdrGamma || !document_) return;
    const auto normalizedPath = [](const QString& path)
    {
        const QFileInfo info(path);
        const QString canonical = info.canonicalFilePath();
        return QDir::cleanPath(QDir::fromNativeSeparators(
            canonical.isEmpty() ? info.absoluteFilePath() : canonical));
    };
    const QString colorSection = displayGamma->property("profileSection").toString();
    // The editor may be standalone, on another file, on another calibration profile,
    // or editing values the renderer has not applied yet. Only confirmed
    // runtime evidence may disable the physical-display fallback controls.
    // LUT preparation controls follow the edited profile's enablement instead.
    const bool matchesLive = liveCalibrationAvailable_ &&
        !liveCalibrationConfigPath_.isEmpty() && !colorSection.isEmpty() &&
        normalizedPath(configPath_).compare(normalizedPath(liveCalibrationConfigPath_), Qt::CaseInsensitive) == 0 &&
        colorSection.compare(liveCalibrationColor_, Qt::CaseInsensitive) == 0 &&
        liveCalibrationConfigIdentity_ != 0 &&
        ConfigurationIdentity::FromText(document_->Serialize()) == liveCalibrationConfigIdentity_;
    const bool usableLut = matchesLive && liveCalibrationLutAttached_;
    // These are editable profile settings, including fallback settings, not live-state switches.
    displayGamma->setEnabled(true);
    conversion->setEnabled(true);
    if (sourceGamma) sourceGamma->setEnabled(true);
    if (inherit)
        inherit->setEnabled(!value(colorSection, QStringLiteral("sdr_adjust_gamma")).isEmpty());
    // Allow preparing HDR settings while viewing SDR or editing offline.
    hdrGamma->setEnabled(lutConfigured);
    const auto effectiveChoice = [](const QComboBox* combo)
    {
        if (!combo) return QString();
        const QString selected = combo->currentData().toString();
        return (selected.isEmpty() ? combo->property("effectiveValue").toString() : selected).toLower();
    };
    if (auto* label = findChild<QLabel*>(QStringLiteral("config.vprenderer.color.target_primaries.label")))
    {
        const QString text = lutConfigured ? QStringLiteral("LUT input gamut") : QStringLiteral("Display calibrated gamut");
        label->setText(text);
        if (auto* control = findChild<QComboBox*>(QStringLiteral("config.vprenderer.color.target_primaries")))
            control->setAccessibleName(text);
    }
    if (auto* label = findChild<QLabel*>(QStringLiteral("config.vprenderer.color.output_gamma.label")))
        label->setText(lutConfigured ? QStringLiteral("Fallback display gamma") : QStringLiteral("Display calibrated gamma"));
    if (auto* response = findChild<QComboBox*>(QStringLiteral("config.vprenderer.color.sdr_response")))
    {
        const QSignalBlocker blocker(response);
        response->clear();
        response->addItem(QStringLiteral("No gamma adjustment"), QStringLiteral("passthrough"));
        for (const QString& gamma : { QStringLiteral("srgb"),
            QStringLiteral("1.8"), QStringLiteral("2.0"), QStringLiteral("2.2"),
            QStringLiteral("2.4"), QStringLiteral("2.6"), QStringLiteral("2.8") })
            response->addItem(friendlyChoiceLabel(gamma), gamma);
        const QString mode = conversion->property("effectiveValue").toString().toLower();
        const QString gamma = effectiveChoice(sourceGamma);
        QString selected = mode == QStringLiteral("passthrough") ? mode : gamma;
        if (response->findData(selected) < 0) selected = QStringLiteral("2.4");
        const QStringList profiles = profileSections(QStringLiteral("vprenderer.color"));
        if (!profiles.isEmpty() && colorSection != profiles.front())
            response->addItem(QStringLiteral("Use profile default"), QStringLiteral("__inherit"));
        response->setCurrentIndex(response->findData(selected));
        const QString label = lutConfigured ? QStringLiteral("Fallback desired SDR gamma") : QStringLiteral("Desired SDR gamma");
        response->setAccessibleName(label);
        if (auto* fieldLabel = findChild<QLabel*>(QStringLiteral("config.vprenderer.color.sdr_response.label")))
            fieldLabel->setText(label);
        response->setToolTip(QStringLiteral("SDR only. Select the viewing response VP should produce on your calibrated display. No gamma adjustment preserves the transfer without disabling gamut mapping, scaling or range conversion."));

    }
    if (sourceGamma)
    {
        const QString label = lutConfigured ? QStringLiteral("SDR reference gamma") :
            QStringLiteral("Desired SDR gamma");
        sourceGamma->setAccessibleName(label);
        if (auto* fieldLabel = findChild<QLabel*>(QStringLiteral("config.vprenderer.color.sdr_input_transfer.label")))
            fieldLabel->setText(label);
        const QString help = QStringLiteral("Assumed SDR reference response for decoding before linear-light processing. "
            "Default: pure gamma 2.4; this is not detected mastering metadata. "
            "SDR LUT input gamma separately chooses the encoding delivered to the LUT. HDR settings do not change this reference.");
        sourceGamma->setToolTip(help);
        if (auto* hint = findChild<QLabel*>(QStringLiteral("config.vprenderer.color.sdr_input_transfer.help")))
            hint->setText(help);
    }
    if (auto* status = findChild<QLabel*>(QStringLiteral("config.vprenderer.color.output_gamma.status")))
    {
        QString text = lutConfigured ? QStringLiteral("Used if no usable LUT is attached. Editing these settings does not disable the LUT.") :
            QStringLiteral("Enter the response your display is calibrated to.");
        status->setText(text);
    }
    if (auto* status = findChild<QLabel*>(QStringLiteral("config.vprenderer.color.lut_attachment_status")))
    {
        const QString fallbackGamma = effectiveChoice(displayGamma);
        QString text = !matchesLive ? QStringLiteral("Live LUT status unavailable for these settings.") :
            usableLut ? QStringLiteral("Attached - calibration LUT active.") :
            QStringLiteral("Not attached - using display gamma %1.").arg(fallbackGamma);
        if (!matchesLive) text += QStringLiteral(" Fallback display gamma: %1.").arg(fallbackGamma);
        status->setText(text);
        status->setToolTip(QStringLiteral("Attachment is confirmed only for the matching applied configuration. A last-known-good LUT can remain attached when a replacement is rejected; attachment does not prove that an edited file is active."));
    }
    if (auto* status = findChild<QLabel*>(QStringLiteral("config.vprenderer.color.hdr_tone_map_target_gamma.status")))
    {
        QString text = QStringLiteral("HDR only: match the encoding expected by the LUT. SDR uses its separate LUT input gamma.");
        status->setText(text);
        hdrGamma->setToolTip(text + QStringLiteral(" Without a usable LUT, the saved display gamma is used instead."));
        if (auto* layout = qobject_cast<QFormLayout*>(status->property("calibrationForm").value<QObject*>()))
            layout->setRowVisible(status, false);
    }

}

void ConfigEditorWindow::refreshRendererAutoStatus()
{
    const auto qualityControl = findChild<QComboBox*>(
        QStringLiteral("config.vprenderer.quality"));
    const QString quality = effectiveChoiceValue(qualityControl);
    const auto qualityValue = [&quality](const QString& high,
        const QString& balanced, const QString& fast)
    {
        if (quality == QStringLiteral("fast")) return fast;
        if (quality == QStringLiteral("balanced")) return balanced;
        return high;
    };
    const auto isAuto = [](QWidget* control)
    {
        if (const auto* combo = qobject_cast<QComboBox*>(control))
        {
            const QString selected = combo->currentData().toString();
            return (selected.isEmpty() ? combo->property("effectiveValue").toString() : selected).trimmed().compare(
                QStringLiteral("auto"), Qt::CaseInsensitive) == 0;
        }
        if (const auto* edit = qobject_cast<QLineEdit*>(control))
            return edit->text().trimmed().compare(QStringLiteral("auto"),
                Qt::CaseInsensitive) == 0;
        return false;
    };
    for (const RendererAutoStatusBinding& binding : rendererAutoStatusBindings_)
    {
        if (!binding.label || !binding.control) continue;
        if (!isAuto(binding.control))
        {
            binding.label->clear();
            binding.label->setVisible(false);
            continue;
        }

        QString text;
        if (binding.key == QStringLiteral("sdr_black_nits"))
        {
            text = QStringLiteral("0 nits (0.000001 nit internally)");
        }
		else if (binding.key == QStringLiteral("output_gamma"))
		{
			text = QStringLiteral("Follows accepted transport; see live output");
		}
        else if (binding.key == QStringLiteral("sdr_adjust_gamma"))
            // This selector controls the conversion policy, rather than
            // declaring the input transfer. Do not echo the live input EOTF
            // here: that belongs only to sdr_input_transfer below.
            text = QStringLiteral("Conditional SDR-to-sRGB policy");
        else if (binding.key == QStringLiteral("sdr_input_transfer"))
            text = liveSourceTransfer_.isEmpty() ?
                QStringLiteral("Source unavailable") : liveSourceTransfer_;
        else if (binding.key == QStringLiteral("tone_mapping"))
            text = QStringLiteral("Spline");
        else if (binding.key == QStringLiteral("gamut_mapping"))
            text = QStringLiteral("Perceptual");
        else if (binding.key == QStringLiteral("peak_detection"))
        {
            text = qualityValue(QStringLiteral("High quality"),
                QStringLiteral("Standard"), QStringLiteral("Off"));
        }
        else if (binding.key == QStringLiteral("contrast_recovery"))
        {
            text = qualityValue(QStringLiteral("0.30"), QStringLiteral("0.00"),
                QStringLiteral("0.00"));
        }
        else if (binding.key == QStringLiteral("upscaler"))
        {
            text = qualityValue(QStringLiteral("EWA Lanczos sharp"),
                QStringLiteral("Lanczos"), QStringLiteral("Built-in sampling"));
        }
        else if (binding.key == QStringLiteral("downscaler"))
        {
            text = qualityValue(QStringLiteral("Hermite"),
                QStringLiteral("Hermite"), QStringLiteral("Built-in sampling"));
        }
        else if (binding.key == QStringLiteral("deband_strength"))
        {
            text = qualityValue(QStringLiteral("Standard"), QStringLiteral("Off"),
                QStringLiteral("Off"));
        }
        else if (binding.key == QStringLiteral("sigmoid"))
        {
            text = qualityValue(QStringLiteral("On"), QStringLiteral("On"),
                QStringLiteral("Off"));
        }
        else if (binding.key == QStringLiteral("dithering"))
        {
            text = qualityValue(QStringLiteral("Blue noise"),
                QStringLiteral("Blue noise"), QStringLiteral("Off"));
        }
        else if (binding.key == QStringLiteral("display_bit_depth"))
            text = QStringLiteral("Output format");
		else if (binding.sectionPrefix == QStringLiteral("vprenderer.color") &&
            binding.key == QStringLiteral("output_presentation"))
        {
            text = QStringLiteral("Flip");
        }
        else if (binding.sectionPrefix == QStringLiteral("vprenderer.color") &&
            binding.key == QStringLiteral("output_range"))
        {
            text = QStringLiteral("Full RGB");
        }
        else if (binding.sectionPrefix == QStringLiteral("vprenderer.color") &&
            binding.key == QStringLiteral("output_transport_gamma"))
        {
            const auto* range = findChild<QComboBox*>(
                QStringLiteral("config.vprenderer.color.output_range"));
            text = effectiveChoiceValue(range) == QStringLiteral("limited") ?
                QStringLiteral("2.4") : QStringLiteral("Not used");
        }
        if (text.isEmpty())
        {
            binding.label->clear();
            binding.label->setVisible(false);
            continue;
        }
        binding.label->setText(QStringLiteral("Auto: %1").arg(text));
        binding.label->setVisible(true);
    }
}

QLineEdit* ConfigEditorWindow::bindTextField(const QString& section, const QString& key, const QString& fallback)
{
    auto* edit = new QLineEdit(value(section, key, fallback));
    edit->setObjectName(controlName(section, key));
    edit->setAccessibleName(accessibleSettingName(key));
    connect(edit, &QLineEdit::textChanged, this, [this, section, key](const QString& text)
    {
        if (!document_) return;
        const std::string sectionText = section.toStdString();
        const std::string keyText = key.toStdString();
        if (text.trimmed().isEmpty()) document_->RemoveKnown(sectionText, keyText.c_str());
        else document_->SetKnown(sectionText, keyText.c_str(), text.toLocal8Bit().constData());
        markDirty();
    });
    return edit;
}

QComboBox* ConfigEditorWindow::bindChoiceField(const QString& section, const QString& key,
    const QStringList& values, const QStringList& labels, bool editable)
{
    auto* combo = new QComboBox;
    combo->setObjectName(controlName(section, key));
    combo->setAccessibleName(accessibleSettingName(key));
    combo->setEditable(editable);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->view()->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    combo->view()->setStyleSheet(QStringLiteral(R"(
        QAbstractItemView {
            color: #e7f0f8;
            background: #111e2b;
            border: 1px solid #3a5871;
            outline: 0;
            padding: 6px;
            selection-background-color: #173c59;
            selection-color: #f4fbff;
        }
        QAbstractItemView::item {
            min-height: 28px;
            padding: 5px 10px;
            border-radius: 4px;
        }
        QAbstractItemView::item:hover { background: #173047; }
        QAbstractItemView::item:selected { background: #173c59; color: #f4fbff; }
    )"));
    for (int index = 0; index < values.size(); ++index)
    {
        QString display = index < labels.size() ? labels[index] : friendlyChoiceLabel(values[index]);
        combo->addItem(display, values[index]);
    }

    QString fallback;
    const bool backendInputSetting =
        (section.compare(QStringLiteral("directshow"), Qt::CaseInsensitive) == 0 ||
         section.compare(QStringLiteral("vprenderer.input_processing"), Qt::CaseInsensitive) == 0) &&
        isSharedInputSetting(key);
    if (backendInputSetting ||
        section.compare(QStringLiteral("directshow"), Qt::CaseInsensitive) == 0)
        fallback = value(QStringLiteral("general"), key);
    const QString raw = value(section, key);
    const bool inherited = backendInputSetting && raw.isEmpty();
    const QString configuredValue = inherited ? fallback : value(section, key, fallback);
    const QString subtitleSectionPrefix = QStringLiteral("vprenderer.subtitles");
    const bool subtitleScriptField = key.compare(QStringLiteral("subtitle_script_hint"), Qt::CaseInsensitive) == 0 &&
        (section.compare(subtitleSectionPrefix, Qt::CaseInsensitive) == 0 ||
            section.startsWith(subtitleSectionPrefix + QStringLiteral("."), Qt::CaseInsensitive));
    const QString configured = subtitleScriptField &&
        configuredValue.compare(QStringLiteral("general"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("latin") : configuredValue;
    if (inherited)
    {
        const QString inheritedLabel = inheritedSharedInputLabel(key);
        combo->setItemText(0, inheritedLabel.isEmpty() ?
            QStringLiteral("Inherited / not set") :
            QStringLiteral("Inherited: %1").arg(inheritedLabel));
        combo->setProperty("inherited", true);
        combo->setToolTip(inheritedLabel.isEmpty() ?
            QStringLiteral("No General value is currently set. Selecting a value creates a renderer override.") :
            QStringLiteral("Inherited from General: %1. Selecting a value creates a renderer override.")
                .arg(inheritedLabel));
        combo->setCurrentIndex(0);
    }
    else if (!configured.isEmpty())
    {
        int index = combo->findData(configured, Qt::UserRole, Qt::MatchFixedString);
        if (index < 0)
            for (int candidate = 0; candidate < combo->count(); ++candidate)
                if (combo->itemData(candidate).toString().compare(
                    configured, Qt::CaseInsensitive) == 0)
                {
                    index = candidate;
                    break;
                }
        bool customValue = false;
        if (index < 0)
        {
            const QString display = friendlyChoiceLabel(configured);
            combo->addItem(display, configured);
            index = combo->count() - 1;
            customValue = true;
        }
        combo->setCurrentIndex(index);
        if (editable && customValue) combo->setEditText(friendlyChoiceLabel(configured));
    }
    else if (combo->count() > 0)
        combo->setCurrentIndex(0);

    // Keep the inherit command meaningful even while a renderer override is
    // selected: users can see the value they will get before choosing it.
    if (backendInputSetting)
    {
        const QString inheritedLabel = inheritedSharedInputLabel(key);
        combo->setItemText(0, inheritedLabel.isEmpty() ?
            QStringLiteral("Inherited / not set") :
            QStringLiteral("Inherited: %1").arg(inheritedLabel));
    }

    auto save = [this, section, key](const QString& selected)
    {
        if (!document_) return;
        QString normalized = selected;
        if (section.compare(QStringLiteral("general"), Qt::CaseInsensitive) == 0 &&
            key.compare(QStringLiteral("renderer"), Qt::CaseInsensitive) == 0 &&
            normalized.compare(QStringLiteral("VideoProcessor Renderer (Alpha)"), Qt::CaseInsensitive) == 0)
            normalized = QStringLiteral("VP Renderer");
        const std::string sectionText = section.toStdString();
        const std::string keyText = key.toStdString();
        if (section.compare(QStringLiteral("directshow"), Qt::CaseInsensitive) == 0 &&
            !isSharedInputSetting(key))
            document_->RemoveKnown("general", keyText.c_str());
        if (normalized.trimmed().isEmpty()) document_->RemoveKnown(sectionText, keyText.c_str());
        else if (!document_->SetKnown(sectionText, keyText.c_str(),
            normalized.toLocal8Bit().constData()))
        {
            document_->AddSection(sectionText);
            document_->SetKnown(sectionText, keyText.c_str(),
                normalized.toLocal8Bit().constData());
        }
        markDirty();
        if (section.compare(QStringLiteral("general"), Qt::CaseInsensitive) == 0)
            refreshInheritedSharedInputChoices(key);
    };
    if (editable)
    {
        connect(combo, qOverload<int>(&QComboBox::activated), this, [combo, save](int index)
        {
            if (index >= 0) save(combo->itemData(index).toString());
        });
        connect(combo->lineEdit(), &QLineEdit::textChanged, this,
            [combo, save](const QString& text)
        {
            const int index = combo->findText(text, Qt::MatchFixedString);
            save(index >= 0 ? combo->itemData(index).toString() : text);
        });
    }
    else
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [combo, save, backendInputSetting](int index)
        {
            if (index < 0) return;
            const bool inherited = backendInputSetting &&
                combo->itemData(index).toString().trimmed().isEmpty();
            combo->setProperty("inherited", inherited);
            combo->style()->unpolish(combo);
            combo->style()->polish(combo);
            save(combo->itemData(index).toString());
        });
    return combo;
}

QCheckBox* ConfigEditorWindow::bindCheckField(const QString& label, const QString& section,
    const QString& key, bool defaultValue)
{
    auto* check = configuredCheck(label, value(section, key), defaultValue);
    check->setObjectName(controlName(section, key));
    check->setEnabled(true);
    connect(check, &QCheckBox::toggled, this, [this, section, key](bool checked)
    {
        if (!document_) return;
        document_->SetKnown(section.toStdString(), key.toStdString().c_str(), checked ? "true" : "false");
        markDirty();
    });
    return check;
}

void ConfigEditorWindow::markDirty()
{
    if (QObject* edited = sender(); edited &&
        edited->objectName().startsWith(QStringLiteral("config.")))
        editOrder_[edited->objectName()] = ++editSerial_;
    dirty_ = document_ &&
        captureDocumentSnapshot(*document_) != savedSnapshot_;
    updateEffectSummary();
    updateValidationState();
    refreshCalibrationControls();
}

void ConfigEditorWindow::prepareRendererPopup()
{
    if (!rendererChoice_) return;
    auto* list = qobject_cast<QListView*>(rendererChoice_->view());
    if (!list) return;

    // QComboBox otherwise defers polishing and item geometry until the first
    // popup open. Renderer discovery is already cached; prepare the cached
    // view now so the user's first click has no one-time layout penalty.
    list->setUniformItemSizes(true);
    rendererChoice_->ensurePolished();
    list->ensurePolished();
    list->viewport()->ensurePolished();
    list->doItemsLayout();
    if (rendererChoice_->count() > 0)
    {
        list->sizeHintForRow(0);
        list->sizeHintForColumn(rendererChoice_->modelColumn());
        list->visualRect(rendererChoice_->model()->index(
            qMax(0, rendererChoice_->currentIndex()),
            rendererChoice_->modelColumn(), rendererChoice_->rootModelIndex()));
    }
    list->window()->ensurePolished();
}

void ConfigEditorWindow::updateEffectSummary()
{
    if (!effectSummary_) return;
    if (!configurationLoaded_ || !document_)
    {
        effectSummary_->setText(QStringLiteral("Configuration unavailable"));
        return;
    }
    if (!dirty_)
    {
        effectSummary_->setText(QStringLiteral("No pending changes"));
        return;
    }
    const DocumentSnapshot currentSnapshot = captureDocumentSnapshot(*document_);
    const std::vector<ConfigurationApplyPolicy::Change> changed = changedDocumentValues(
        savedSnapshot_, currentSnapshot);
    const auto action = ConfigurationApplyPolicy::ClassifyChanges(changed,
        snapshotUsesDirectShowRenderer(savedSnapshot_));
    QStringList sections;
    for (const ConfigurationApplyPolicy::Change& change : changed)
    {
        const QString category = effectCategory(change.section);
        if (!sections.contains(category)) sections.push_back(category);
    }
    effectSummary_->setText(QString::fromLatin1(
        ConfigurationApplyPolicy::ActionLabel(action)) +
        QStringLiteral(": ") + sections.join(QStringLiteral(", ")));
}

bool ConfigEditorWindow::validateCandidate(std::wstring& error,
    bool allowActionDrafts) const
{
    QStringList fields;
    const QStringList errors = validationErrors(fields, allowActionDrafts);
    if (errors.isEmpty()) return true;
    error = errors.front().toStdWString();
    return false;
}

QStringList ConfigEditorWindow::validationErrors(QStringList& fields,
    bool allowActionDrafts, bool includeCoreValidation) const
{
    QStringList errors;
    fields.clear();
    if (!configurationLoaded_ || !document_)
    {
        errors.push_back(QStringLiteral("The configuration was not loaded."));
        fields.push_back(QString());
        return errors;
    }
    ConfigEditorCore::ConfigDocument validationDocument = *document_;
    std::wstring coreError;
    bool coreValid = !includeCoreValidation;
    if (includeCoreValidation && allowActionDrafts)
    {
        const int maximumDrafts = static_cast<int>(
            validationDocument.SectionNamesWithPrefix("actions").size());
        for (int attempt = 0; attempt <= maximumDrafts; ++attempt)
        {
            if (ConfigEditorCore::ValidateCandidate(validationDocument, coreError))
            {
                coreValid = true;
                break;
            }
            const QString message = QString::fromStdWString(coreError);
            const QRegularExpression actionError(
                QStringLiteral("^\\[(actions\\.[^\\]]+)\\] (?!enabled(?: |$))"));
            const QRegularExpressionMatch match = actionError.match(message);
            if (!match.hasMatch()) break;
            const std::string section = match.captured(1).toStdString();
            if (!configuredBooleanValue(QString::fromLocal8Bit(
                validationDocument.Get(section.c_str(), "enabled").c_str()), true))
                break;
            validationDocument.SetKnown(section, "enabled", "false");
        }
        if (!coreValid)
            coreValid = ConfigEditorCore::ValidateCandidate(
                validationDocument, coreError);
    }
    else if (includeCoreValidation)
        coreValid = ConfigEditorCore::ValidateCandidate(
            validationDocument, coreError);

    if (includeCoreValidation && !coreValid)
    {
        const QString message = QString::fromStdWString(coreError);
        errors.push_back(message);
        const QRegularExpression sectionError(
            QStringLiteral("^\\[([^\\]]+)\\]"));
        const QRegularExpressionMatch sectionMatch = sectionError.match(message);
        const QRegularExpression quotedKey(
            QStringLiteral("\\bkey ['\"]([A-Za-z0-9_.-]+)['\"]"));
        const QRegularExpressionMatch quotedMatch = quotedKey.match(message);
        const QRegularExpression plainKey(
            QStringLiteral("^\\[[^\\]]+\\]\\s+([A-Za-z0-9_.-]+)"));
        const QRegularExpressionMatch plainMatch = plainKey.match(message);
        const QString key = quotedMatch.hasMatch() ? quotedMatch.captured(1) :
            (plainMatch.hasMatch() ? plainMatch.captured(1) : QString());
        fields.push_back(sectionMatch.hasMatch() && !key.isEmpty() ?
            controlName(sectionMatch.captured(1), key) : QString());
    }

    auto discoveredContains = [](const QStringList& discovered,
        const QString& candidate)
    {
        for (const QString& value : discovered)
            if (value.trimmed().compare(candidate.trimmed(),
                Qt::CaseInsensitive) == 0)
                return true;
        return false;
    };

    QString renderer = value(QStringLiteral("general"),
        QStringLiteral("renderer")).trimmed();
    if (renderer.compare(QStringLiteral("VideoProcessor Renderer (Alpha)"),
        Qt::CaseInsensitive) == 0)
        renderer = QStringLiteral("VP Renderer");
    // Discovery is advisory for validation. A transient or unavailable helper
    // must not make every unrelated profile edit unsavable. When it returns a
    // real list we remain strict; otherwise accept VP's built-in choices.
    const QStringList acceptedRenderers = allRenderers_.isEmpty() ?
        QStringList{ QStringLiteral("VP Renderer"), QStringLiteral("VP Renderer (Beta)"),
            QStringLiteral("DirectShow - madVR") } : allRenderers_;
	if (!renderer.isEmpty() &&
		!discoveredContains(acceptedRenderers, renderer))
    {
        errors.push_back(QStringLiteral("[general] renderer '%1' was not discovered. Choose an available renderer.")
            .arg(renderer.isEmpty() ? QStringLiteral("(empty)") : renderer));
        fields.push_back(controlName(QStringLiteral("general"),
            QStringLiteral("renderer")));
    }

    const QString captureDevice = value(QStringLiteral("general"),
        QStringLiteral("capture_device")).trimmed();
	if (!captureDevice.isEmpty() && !captureDevices_.isEmpty() &&
		!discoveredContains(captureDevices_, captureDevice))
    {
        errors.push_back(QStringLiteral("[general] capture_device '%1' was not discovered. Choose an available capture device.")
            .arg(captureDevice.isEmpty() ? QStringLiteral("(empty)") : captureDevice));
        fields.push_back(controlName(QStringLiteral("general"),
            QStringLiteral("capture_device")));
    }
    // Validate luminance in memory as well as on Apply. Keep invalid text
    // visible and the saved document intact instead of substituting a default.
    const auto renderingProfiles = profileSections(QStringLiteral("vprenderer"));
    for (const QString& section : renderingProfiles)
    {
        const auto inheritedLuminance = [&](const QString& key)
        {
            QString result = value(section, key).trimmed();
            if (result.isEmpty() && !renderingProfiles.isEmpty())
                result = value(renderingProfiles.front(), key).trimmed();
            if (result.isEmpty()) result = value(QStringLiteral("vprenderer"), key).trimmed();
            return result;
        };
        std::string toneError, toneKey;
        if (!ToneMappingTuning::Validate([&](const char* key) {
            return inheritedLuminance(QString::fromUtf8(key)).toStdString();
        }, toneError, toneKey)) {
            errors.push_back(QStringLiteral("[%1] %2").arg(section, QString::fromStdString(toneError)));
            fields.push_back(controlName(section, QString::fromStdString(toneKey)));
        }
        const QString white = value(section, QStringLiteral("sdr_target_nits")).trimmed();
        std::string expected;
        if (!white.isEmpty() && !RendererProfileConfig::ValidateProfileSetting(
            "display", "sdr_target_nits", white.toStdString(), expected))
        {
            errors.push_back(QStringLiteral("[%1] sdr_target_nits must be %2. The saved value is retained.")
                .arg(section, QString::fromStdString(expected)));
            fields.push_back(controlName(section, QStringLiteral("sdr_target_nits")));
        }
        const QString black = inheritedLuminance(QStringLiteral("sdr_black_nits"));
        if (black.isEmpty() || black.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0)
            continue;
        QString effectiveWhite = inheritedLuminance(QStringLiteral("sdr_target_nits"));
        if (effectiveWhite.isEmpty()) effectiveWhite = QStringLiteral("203");
        bool whiteOk = false, blackOk = false;
        const double whiteNits = effectiveWhite.toDouble(&whiteOk);
        const double blackNits = black.toDouble(&blackOk);
        if (!blackOk || !whiteOk || !HdrTargetLuminance::ValidBlack(blackNits, whiteNits))
        {
            errors.push_back(QStringLiteral("[%1] sdr_black_nits must be Auto or non-negative and below the HDR target white. The saved value is retained.").arg(section));
            fields.push_back(controlName(section, QStringLiteral("sdr_black_nits")));
        }
    }
    return errors;
}

QString ConfigEditorWindow::displayWarning() const
{
    const QString display = value(QStringLiteral("general"),
        QStringLiteral("fullscreen_monitor_name")).trimmed();
    if (display.isEmpty()) return {};
    for (const QString& discovered : monitors_)
        if (discovered.trimmed().compare(display, Qt::CaseInsensitive) == 0)
            return {};
    return QStringLiteral("Warning: Display '%1' is not currently discoverable.")
        .arg(display);
}

bool ConfigEditorWindow::updateValidationState()
{
    QStringList fields;
    // Keep ordinary editing entirely in memory. Full schema/shader validation
    // serializes a candidate beside the configuration and is intentionally
    // reserved for Apply/OK, where safe persistence validates again.
    const QStringList errors = validationErrors(fields, true, false);
    const bool valid = errors.isEmpty();
    validationValid_ = valid;
    if (saveButton_)
        saveButton_->setEnabled(configurationLoaded_ && valid);
    if (applyButton_)
        applyButton_->setEnabled(configurationLoaded_ && dirty_ && valid);
    if (!valid)
    {
        int latest = 0;
        quint64 latestOrder = 0;
        for (int index = 0; index < fields.size(); ++index)
        {
            quint64 order = editOrder_.value(fields[index], 0);
            if (order == 0 && !fields[index].isEmpty())
            {
                const QString keySuffix = fields[index].mid(
                    fields[index].lastIndexOf(u'.'));
                for (auto edited = editOrder_.cbegin(); edited != editOrder_.cend(); ++edited)
                    if (edited.key().endsWith(keySuffix, Qt::CaseInsensitive))
                        order = std::max(order, edited.value());
            }
            if (order >= latestOrder)
            {
                latest = index;
                latestOrder = order;
            }
        }
        QString message = errors[latest];
        const int additional = errors.size() - 1;
        if (additional > 0)
            message += QStringLiteral(" And %1 additional error%2.")
                .arg(additional).arg(additional == 1 ? QString() : QStringLiteral("s"));
        setStatus(message, true);
    }
    else if (const QString warning = displayWarning(); !warning.isEmpty())
        setWarningStatus(warning);
    else if (dirty_)
        setStatus(QStringLiteral("Unsaved changes. OK or Apply will validate again before saving."));
    else
        setStatus(QStringLiteral("Configuration loaded."));
    return valid;
}

void ConfigEditorWindow::applyNativeOwner()
{
	ownerApplied_ = false;
}

void ConfigEditorWindow::clearNativeOwner()
{
    ownerApplied_ = false;
}

bool ConfigEditorWindow::nativeOwnerIsValid() const
{
    const HWND owner = reinterpret_cast<HWND>(ownerHandle_);
    if (!owner || !IsWindow(owner) || ownerProcessId_ == 0) return false;
    DWORD actualProcessId = 0;
    GetWindowThreadProcessId(owner, &actualProcessId);
    return actualProcessId == ownerProcessId_;
}

void ConfigEditorWindow::publishNativeAssociation()
{
    const HWND owner = reinterpret_cast<HWND>(ownerHandle_);
    const HWND editor = reinterpret_cast<HWND>(effectiveWinId());
    if (!nativeOwnerIsValid() || !editor || !IsWindow(editor)) return;
    const quintptr handle = reinterpret_cast<quintptr>(editor);
    if (publishedWindowHandle_ == handle) return;

    static const UINT associationMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigEditor.Association.v1");
    // Association is an advertisement, not a request. Posting prevents a
    // reverse SendMessage cycle when VP is itself synchronously activating us.
    if (associationMessage && PostMessageW(owner, associationMessage,
        GetCurrentProcessId(), reinterpret_cast<LPARAM>(editor)))
        publishedWindowHandle_ = handle;
}

void ConfigEditorWindow::positionForReveal()
{
    const HWND target = reinterpret_cast<HWND>(presentationTargetHandle_);
    if (!target || !IsWindow(target) ||
        presentationTargetProcessId_ == 0 ||
        presentationTargetProcessId_ != ownerProcessId_)
        return;
    DWORD actualProcessId = 0;
    GetWindowThreadProcessId(target, &actualProcessId);
    if (actualProcessId != presentationTargetProcessId_) return;
    const HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return;
    const HWND editor = reinterpret_cast<HWND>(effectiveWinId());
    RECT current{};
    if (!editor || !IsWindow(editor) || !GetWindowRect(editor, &current)) return;
    const QRect frame(current.left, current.top,
        current.right - current.left, current.bottom - current.top);
    const RECT& work = monitorInfo.rcWork;
    const QRect workArea(work.left, work.top,
        work.right - work.left, work.bottom - work.top);
    const QRect placed = ConfigEditorPlacement::ClampFrameToWorkArea(
        frame, workArea);
    SetWindowPos(editor, nullptr, placed.left(), placed.top(), 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void ConfigEditorWindow::repairOrderAboveVideoProcessor(
    bool requireOwnerForeground)
{
    if (foregroundRepairQueued_ || !isVisible() || isMinimized() || !nativeOwnerIsValid())
        return;
    DWORD foregroundProcess = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
    if (requireOwnerForeground && foregroundProcess != ownerProcessId_) return;
    foregroundRepairQueued_ = true;
    QTimer::singleShot(0, this, [this, requireOwnerForeground]
    {
        foregroundRepairQueued_ = false;
        if (!isVisible() || isMinimized() || !nativeOwnerIsValid()) return;
        const HWND foreground = GetForegroundWindow();
        DWORD process = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &process);
        if (requireOwnerForeground && process != ownerProcessId_) return;
        const HWND editor = reinterpret_cast<HWND>(effectiveWinId());
        // Keep the relative order above VP, rather than merely checking the
        // WS_EX_TOPMOST bit shared by both Config and fullscreen presentation.
        for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT))
        {
            if (window == editor) return;
            DWORD windowProcess = 0;
            GetWindowThreadProcessId(window, &windowProcess);
            if (windowProcess == ownerProcessId_)
            {
                pendingTopmostReassert_ = true;
                applyScopedTopmost();
                return;
            }
        }
    });
}

void ConfigEditorWindow::applyScopedTopmost()
{
    if (!pendingTopmostReassert_ || !scopedTopmostEligible_ || !isVisible())
        return;
	if (hasActiveOwnedPopup())
	{
		if (!topmostReassertDeferredForPopup_)
		{
			topmostReassertDeferredForPopup_ = true;
			qInfo("Configuration editor z-order repair deferred: active Qt popup");
		}
		// A native topmost request can reorder the popup out from under its menu
		// or combo list. Keep the repair pending and retry from the Qt event loop
		// after the popup has had a chance to close.
		QTimer::singleShot(50, this, [this] { applyScopedTopmost(); });
		return;
	}
    // Config is an operator modal surface.  While it is visible, retain its
    // topmost placement even after another application receives foreground.
    // This deliberately matches madVR's configuration behavior and keeps the
    // editor above VP's exclusive fullscreen host.
    const HWND editor = reinterpret_cast<HWND>(effectiveWinId());
    if (!editor || !IsWindow(editor)) return;
    scopedTopmost_ = SetWindowPos(editor, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
    pendingTopmostReassert_ = false;
	if (topmostReassertDeferredForPopup_)
	{
		topmostReassertDeferredForPopup_ = false;
		qInfo("Configuration editor z-order repair applied after Qt popup closed");
	}
}

void ConfigEditorWindow::removeScopedTopmost()
{
    QWindow* editorWindow = windowHandle();
    const HWND editor = editorWindow ?
        reinterpret_cast<HWND>(editorWindow->winId()) : nullptr;
    if (editor && IsWindow(editor) &&
        (scopedTopmost_ ||
            (GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0))
    {
        SetWindowPos(editor, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    scopedTopmost_ = false;
}

bool ConfigEditorWindow::hasActiveOwnedPopup() const
{
    QWidget* popup = QApplication::activePopupWidget();
    if (!popup || !popup->isVisible()) return false;
    for (QWidget* parent = popup->parentWidget(); parent;
        parent = parent->parentWidget())
        if (parent == this) return true;
    QWindow* transient = popup->windowHandle() ?
        popup->windowHandle()->transientParent() : nullptr;
    return transient && transient == windowHandle();
}

bool ConfigEditorWindow::notifyVideoProcessor()
{
	if (remoteClient_) return false;
    // Test-mode editors represent the normal VP-absent standalone case. This
    // also prevents a UI test from disturbing a developer's running VP.
    if (testMode_) return false;
    SetLastError(ERROR_SUCCESS);
    HANDLE changedEvent = CreateEventW(nullptr, FALSE, FALSE,
        ConfigurationLiveApply::ChangedEventName);
    if (!changedEvent) return false;
    const bool videoProcessorWasListening = GetLastError() == ERROR_ALREADY_EXISTS;
    const bool signaled = videoProcessorWasListening && SetEvent(changedEvent) != FALSE;
    CloseHandle(changedEvent);
    return signaled;
}

void ConfigEditorWindow::applyChanges()
{
    if (!dirty_) return;
    saveChanges();
}

void ConfigEditorWindow::setRendererDiscoveryForTesting(
    const QStringList& allRenderers, const QStringList& filteredRenderers)
{
    allRenderers_ = allRenderers;
    filteredRenderers_ = filteredRenderers;
    rebuildConfigurationShell();
}

void ConfigEditorWindow::rebuildConfigurationShell()
{
    // Recreate the editor after a full document reload (for example Cancel).
    // Ordinary renderer-visibility changes are handled in place and never
    // enter this comparatively expensive path.
    migrateCalibrationProfiles();
    profileSelectionReady_ = false;
    const int currentPage = pages_ ? pages_->currentIndex() : 0;
	activeProfileLists_.clear();
	rendererAutoStatusBindings_.clear();
	monitorChoice_ = nullptr;
	rendererChoice_ = nullptr;
	actionRendererTarget_ = nullptr;
	rendererShortcutForm_ = nullptr;
    QWidget* replacement = createShell();
    QWidget* previous = takeCentralWidget();
    setCentralWidget(replacement);
    profileSelectionReady_ = true;
    selectPage(currentPage);
	if (!testMode_ && !remoteClient_) refreshActiveProfileIndicators();
    if (previous) previous->deleteLater();
}

void ConfigEditorWindow::applyRendererVisibilityFilter(bool hideLegacyRenderers)
{
    const auto containsRenderer = [](const QStringList& renderers,
        const QString& candidate)
    {
        return std::any_of(renderers.cbegin(), renderers.cend(),
            [&candidate](const QString& renderer)
            {
                return renderer.compare(candidate, Qt::CaseInsensitive) == 0;
            });
    };
    const auto isLegacy = [this, &containsRenderer](const QString& renderer)
    {
        return containsRenderer(allRenderers_, renderer) &&
            !containsRenderer(filteredRenderers_, renderer);
    };
    const auto filterCombo = [hideLegacyRenderers, &isLegacy](QComboBox* combo)
    {
        auto* view = combo ? qobject_cast<QListView*>(combo->view()) : nullptr;
        if (!view) return;
        for (int index = 0; index < combo->count(); ++index)
        {
            const QString renderer = combo->itemData(index,
                kRendererNameRole).toString();
            view->setRowHidden(index,
                hideLegacyRenderers && !renderer.isEmpty() &&
                isLegacy(renderer));
        }
    };

    filterCombo(rendererChoice_);
    filterCombo(actionRendererTarget_);
    if (!rendererShortcutForm_) return;
    for (int row = 0; row < rendererShortcutForm_->rowCount(); ++row)
    {
        QLayoutItem* fieldItem = rendererShortcutForm_->itemAt(row,
            QFormLayout::FieldRole);
        QWidget* field = fieldItem ? fieldItem->widget() : nullptr;
        const QString renderer = field ?
            field->property("rendererName").toString() : QString();
        rendererShortcutForm_->setRowVisible(row,
            !hideLegacyRenderers || renderer.isEmpty() ||
            !isLegacy(renderer));
    }
}

bool ConfigEditorWindow::saveChanges()
{
    if (!configurationLoaded_ || !document_) return false;

    const DocumentSnapshot currentSnapshot = captureDocumentSnapshot(*document_);
    const std::vector<ConfigurationApplyPolicy::Change> changed = changedDocumentValues(
        savedSnapshot_, currentSnapshot);
    const auto action = ConfigurationApplyPolicy::ClassifyChanges(changed,
        snapshotUsesDirectShowRenderer(savedSnapshot_));
    // Persist shortcut spelling in the same canonical form used by the
    // accelerator parser. Case is not a modifier: L and l are both L, while
    // Shift+L is the distinct shifted chord.
    const QStringList shortcutRoots = {
        QStringLiteral("shortcuts"), QStringLiteral("queue"),
        QStringLiteral("lldv"), QStringLiteral("vprenderer"),
        QStringLiteral("shader"), QStringLiteral("shaders") };
    for (const QString& root : shortcutRoots)
    {
        for (const std::string& section : document_->SectionNamesWithPrefix(
            root.toStdString()))
        {
            for (const auto& setting : document_->SectionSettings(section))
            {
                const bool isShortcut =
                    (root == QStringLiteral("shortcuts") &&
                        ConfigFile::NormalizeName(setting.first) !=
                            "foreground_only" &&
                        ConfigFile::NormalizeName(setting.first) !=
                            "global_hotkeys") ||
                    ConfigFile::NormalizeName(setting.first) == "shortcut";
                if (!isShortcut || ConfigFile::Trim(setting.second).empty()) continue;
                std::string canonical;
                if (RendererProfileConfig::CanonicalizeKeyChord(setting.second, canonical))
                    document_->SetKnown(section, setting.first.c_str(), canonical);
            }
        }
    }

    // Saving is also the boundary at which an unfinished enabled action becomes
    // a draft.  Do not make users discover every required action field merely
    // to persist work in progress; disable only the action rejected by the
    // runtime validator, then validate the remaining configuration normally.
    QStringList draftedActions;
    const int maximumDrafts = static_cast<int>(document_->SectionNamesWithPrefix("actions").size());
    for (int attempt = 0; attempt <= maximumDrafts; ++attempt)
    {
        std::wstring validationError;
        if (ConfigEditorCore::ValidateCandidate(*document_, validationError)) break;

        const QString message = QString::fromStdWString(validationError);
        const QRegularExpression actionError(
            QStringLiteral("^\\[(actions\\.[^\\]]+)\\] (?!enabled(?: |$))"));
        const QRegularExpressionMatch match = actionError.match(message);
        if (!match.hasMatch()) break;

        const QString section = match.captured(1);
        if (!configuredBooleanValue(value(section, QStringLiteral("enabled")), true)) break;
        document_->SetKnown(section.toStdString(), "enabled", "false");
        draftedActions.push_back(displayName(section, QStringLiteral("actions")));
    }

	ConfigEditorCore::SaveResult result;
	std::wstring error;
	const bool creatingConfiguration = !document_->existedAtLoad;
    if (!validateCandidate(error))
    {
        setStatus(QString::fromStdWString(error), true);
        if (saveButton_) saveButton_->setEnabled(false);
        if (applyButton_) applyButton_->setEnabled(false);
        return false;
    }
	if (remoteClient_)
	{
		ConfigurationRpcClient::ApplyResult applied;
		std::string rpcError;
		const std::string candidate = document_->Serialize();
		if (!remoteClient_->ApplyConfig(document_->loadedBytes,
			candidate, applied, rpcError))
		{
			setStatus(QString::fromUtf8(rpcError), true);
			return false;
		}
		document_->loadedBytes = candidate;
		document_->requiresMigrationBackup = false;
		dirty_ = false;
		if (saveButton_) saveButton_->setEnabled(true);
		if (applyButton_) applyButton_->setEnabled(false);
		savedSnapshot_ = captureDocumentSnapshot(*document_);
		updateEffectSummary();
		const QString outcome = applied.status == 0 ?
			QStringLiteral("Saved on target for next start") :
			applied.status == 1 ? QStringLiteral("Applied on target") :
			QStringLiteral("Saved on target; reset or restart requested");
		setStatus(QStringLiteral("%1: %2.").arg(outcome,
			QString::fromUtf8(applied.action)));
		return true;
	}
    // Config is the interactive authority for this file. Save the user's
    // validated editor state instead of refusing a stale-file conflict.
    if (!ConfigEditorCore::SaveSafely(*document_, result, error, true))
    {
        setStatus(QString::fromStdWString(error), true);
        if (!testMode_) openValidationError(this, QString::fromStdWString(error));
        return false;
    }
    dirty_ = false;
    saveButton_->setEnabled(true);
    if (applyButton_) applyButton_->setEnabled(false);
    savedSnapshot_ = captureDocumentSnapshot(*document_);
    updateEffectSummary();

    // The editor is a separate process. Signal the running VP instance only
    // after the atomic safe-save has committed the complete validated file.
    // An auto-reset event also makes saves harmless when VP is not running.
    const bool notified = notifyVideoProcessor();
    if (!draftedActions.isEmpty())
    {
        if (auto* actions = findChild<QListWidget*>(QStringLiteral("config.actions.items"));
            actions && actions->currentItem() &&
            draftedActions.contains(actions->currentItem()->text(), Qt::CaseInsensitive))
        {
            if (auto* enabled = findChild<QCheckBox*>(QStringLiteral("config.actions.enabled")))
            {
                const QSignalBlocker blocker(enabled);
                enabled->setChecked(false);
            }
        }
        setStatus(QStringLiteral("Changes saved%1. Incomplete action%2 %3 saved as disabled draft%2.")
            .arg(notified ? QStringLiteral(" and sent to VideoProcessor") : QStringLiteral("; VideoProcessor could not be notified"))
            .arg(draftedActions.size() == 1 ? QString() : QStringLiteral("s"),
                draftedActions.join(QStringLiteral(", "))));
    }
	else if (creatingConfiguration)
    {
        const QString effect = QString::fromLatin1(
            ConfigurationApplyPolicy::ActionLabel(action));
        setStatus(notified ?
            QStringLiteral("Configuration created safely at %1. %2 was requested.")
                .arg(configPath_, effect) :
            QStringLiteral("Configuration created safely at %1. It will take effect when VideoProcessor next starts.")
                .arg(configPath_), false);
    }
    else
    {
        const QString effect = QString::fromLatin1(
            ConfigurationApplyPolicy::ActionLabel(action));
        setStatus(notified ?
            QStringLiteral("Changes saved safely. %1 was requested.").arg(effect) :
            QStringLiteral("Changes saved safely. Takes effect when VideoProcessor next starts."), false);
    }
    return true;
}

QWidget* ConfigEditorWindow::createShell()
{
    pages_ = nullptr;
    updatePanel_ = nullptr;
    navigation_ = nullptr;
    status_ = nullptr;
    effectSummary_ = nullptr;
    monitorChoice_ = nullptr;
    rendererChoice_ = nullptr;
    rendererShortcutForm_ = nullptr;
    actionRendererTarget_ = nullptr;
    targetChoice_ = nullptr;
	targetSearchTitle_ = nullptr;
	targetSearchHelp_ = nullptr;
	targetSearchProgress_ = nullptr;
    applyButton_ = nullptr;
    saveButton_ = nullptr;
    configurationHost_ = nullptr;
    shaderCacheStatus_ = nullptr;
    activeProfileLists_.clear();
    rendererAutoStatusBindings_.clear();
    auto* root = new QWidget;
    root->setObjectName(QStringLiteral("root"));
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* header = new QFrame;
    header->setObjectName(QStringLiteral("brandHeader"));
    header->setMinimumHeight(56);
    header->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 6, 16, 6);
    auto* icon = new QLabel;
    icon->setPixmap(windowIcon().pixmap(32, 32));
    icon->setAccessibleName(QStringLiteral("VideoProcessor"));
    headerLayout->addWidget(icon);
    auto* brand = new QWidget;
    auto* brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(5, 0, 0, 0);
    brandLayout->setSpacing(6);
    brand->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    auto* title = new QLabel(QStringLiteral("VideoProcessor Configuration"));
    title->setObjectName(QStringLiteral("brandTitle"));
    // Profile detail pages can legitimately request more horizontal space than
    // the initial window. Keep the product identity intact rather than
    // allowing the header labels to be the widgets that get compressed away.
    title->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    brandLayout->addWidget(title);
    headerLayout->addWidget(brand);
    headerLayout->addStretch();
	// Every launch mode can move from its current source to a discovered VP.
	{
		auto* caption = new QLabel(QStringLiteral("Configure VP:"));
		caption->setObjectName(QStringLiteral("configTargetCaption"));
		caption->setToolTip(QStringLiteral(
			"Choose which VideoProcessor this window configures."));
		headerLayout->addWidget(caption);
		targetChoice_ = new TargetComboBox;
		targetChoice_->setObjectName(QStringLiteral("config.targetChoice"));
		targetChoice_->setProperty("targetSelector", true);
		targetChoice_->setAccessibleName(QStringLiteral("VideoProcessor target"));
		targetChoice_->setToolTip(QStringLiteral(
			"Select the VideoProcessor whose settings you want to edit."));
		targetChoice_->setFixedSize(184, 24);
		targetChoice_->setMaxVisibleItems(12);
		targetChoice_->view()->setStyleSheet(QStringLiteral("padding: 0px;"));
		caption->setBuddy(targetChoice_);
		populateTargetChoices();
		connect(targetChoice_, qOverload<int>(&QComboBox::activated), this,
			[this](int index)
			{
				if (index == targetChoices_.size())
					enterTargetAddress();
				else if (index >= 0 && index < targetChoices_.size() &&
					(noTarget_ || index > 0 ||
						targetChoices_[index].host != remoteHost_ ||
						targetChoices_[index].port != remotePort_))
					selectAnotherTarget(targetChoices_[index]);
				if (targetChoice_) targetChoice_->setCurrentIndex(0);
			});
		headerLayout->addWidget(targetChoice_);
		auto* refresh = new QToolButton;
		refresh->setObjectName(QStringLiteral("config.refreshTargets"));
		refresh->setProperty("targetRefresh", true);
		refresh->setIcon(targetRefreshIcon());
		refresh->setIconSize(QSize(14, 14));
		refresh->setAccessibleName(QStringLiteral("Refresh targets"));
		refresh->setToolTip(QStringLiteral("Refresh targets"));
		refresh->setFixedSize(24, 24);
		connect(refresh, &QToolButton::clicked, this,
			[this] { refreshTargetChoices(); });
		headerLayout->addWidget(refresh);
	}
    rootLayout->addWidget(header);

    auto* center = new QWidget;
    auto* centerLayout = new QHBoxLayout(center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(0);

    navigation_ = new QWidget;
    navigation_->setObjectName(QStringLiteral("sidebar"));
    navigation_->setMinimumWidth(172);
    navigation_->setMaximumWidth(200);
    navigation_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    navigation_->setAccessibleName(QStringLiteral("Settings navigation"));
    auto* navLayout = new QVBoxLayout(navigation_);
    navLayout->setContentsMargins(12, 14, 0, 12);
    navLayout->setSpacing(2);
    auto* caption = new QLabel(QStringLiteral("Settings"));
    caption->setObjectName(QStringLiteral("sidebarCaption"));
    navLayout->addWidget(caption);

    pages_ = new QStackedWidget;
    pages_->setObjectName(QStringLiteral("settingsPages"));
    pages_->addWidget(createStartupPage());
    pages_->addWidget(createQueuePage());
    pages_->addWidget(createRendererPage());
    pages_->addWidget(createDirectShowPage());
    pages_->addWidget(createViewportPage());
    pages_->addWidget(createLldvPage());
    pages_->addWidget(createShortcutsPage());
    pages_->addWidget(createActionsPage());
    pages_->addWidget(createStandardShadersPage());
    pages_->addWidget(createNlsShadersPage());
    pages_->addWidget(createLogsPage());
    pages_->addWidget(createInputProcessingPage(
        QStringLiteral("Input processing"),
        QStringLiteral("Override the General input policy for VP Renderer, or inherit it."),
        QStringLiteral("vprenderer.input_processing")));
    pages_->addWidget(createInputProcessingPage(
        QStringLiteral("Input processing"),
        QStringLiteral("Override the General input policy for DirectShow, or inherit it."),
        QStringLiteral("directshow")));
    pages_->addWidget(new QWidget); // Former Output page; selectPage redirects to Color / Output.
    pages_->addWidget(createShadersSetupPage());
    pages_->addWidget(createShortcutsSetupPage());
    pages_->addWidget(createColorConfigPage());
	pages_->addWidget(createScalingPage());
	pages_->addWidget(createZoomPage());
    pages_->addWidget(createProfilePage(QStringLiteral("Subtitles"),
        QStringLiteral("Choose subtitle processing and inward placement. The first profile is the default."),
        QStringLiteral("vprenderer.subtitles")));
	pages_->addWidget(createLedDrivePage()); // 20

    // 21 (RC3 merge: Bill's Updates page follows our LED drive page, 20).
    updatePanel_ = new UpdatePanel(QString::fromStdWString(UpdateLauncher::InstallationRoot()), nullptr, !testMode_);
    pages_->addWidget(createPage(QStringLiteral("Updates"),
        QStringLiteral("Manage updates on this computer. Remote VideoProcessors update separately."), updatePanel_));

    auto* navGroup = new QButtonGroup(root);
    navGroup->setExclusive(true);
    const auto addLeaf = [this, navLayout, navGroup](const QString& title, int page)
    {
        QPushButton* button = addNavigationButton(title, page);
        navGroup->addButton(button, page);
        navLayout->addWidget(button);
        if (page == 0) button->setChecked(true);
        return button;
    };
    addLeaf(QStringLiteral("General"), 0);
    addLeaf(QStringLiteral("Queue"), 1);
    addLeaf(QStringLiteral("LLDV"), 5);
    QPushButton* shadersNavigation = addLeaf(QStringLiteral("Shaders"), 14);
    addLeaf(QStringLiteral("Actions"), 7);
    QPushButton* shortcutsNavigation = addLeaf(QStringLiteral("Shortcuts"), 15);
    addLeaf(QStringLiteral("LED drive"), 20);
    addLeaf(QStringLiteral("Logs"), 10);
    navLayout->addSpacing(8);
    QPushButton* vpNavigation = addLeaf(QStringLiteral("VP Renderer"), 2);
    QPushButton* directShowNavigation = addLeaf(QStringLiteral("DirectShow"), 3);
    addLeaf(QStringLiteral("Updates"), 21)->setObjectName(QStringLiteral("config.openUpdates"));
    navLayout->addStretch();

    auto* pageHost = new QWidget;
    pageHost->setObjectName(QStringLiteral("sectionPageHost"));
    auto* pageHostLayout = new QVBoxLayout(pageHost);
    pageHostLayout->setContentsMargins(0, 0, 0, 0);
    pageHostLayout->setSpacing(0);
    auto* sectionTabs = new QTabBar;
    sectionTabs->setObjectName(QStringLiteral("settingsSectionTabs"));
    sectionTabs->setAccessibleName(QStringLiteral("Settings section pages"));
    sectionTabs->setProperty("sectionTabs", true);
    sectionTabs->setExpanding(false);
    sectionTabs->setDrawBase(false);
    sectionTabs->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    pageHostLayout->addWidget(sectionTabs, 0, Qt::AlignLeft);
    pageHostLayout->addWidget(pages_, 1);

    const auto showSectionTabs = [sectionTabs](
        const std::vector<std::pair<QString, int>>& entries, int currentPage)
    {
        QSignalBlocker blocker(sectionTabs);
        bool sameTabs = sectionTabs->count() == static_cast<int>(entries.size());
        for (int index = 0; sameTabs && index < sectionTabs->count(); ++index)
            sameTabs = sectionTabs->tabText(index) == entries[index].first &&
                sectionTabs->tabData(index).toInt() == entries[index].second;
        if (!sameTabs)
        {
            while (sectionTabs->count() > 0) sectionTabs->removeTab(0);
            for (int index = 0; index < static_cast<int>(entries.size()); ++index)
            {
                sectionTabs->addTab(entries[index].first);
                sectionTabs->setTabData(index, entries[index].second);
            }
        }
        int selected = 0;
        for (int index = 0; index < static_cast<int>(entries.size()); ++index)
            if (entries[index].second == currentPage) selected = index;
        sectionTabs->setCurrentIndex(selected);
        sectionTabs->setVisible(!entries.empty());
    };
    const auto updateSectionTabs = [showSectionTabs, shadersNavigation,
        shortcutsNavigation, vpNavigation, directShowNavigation](int page)
    {
        if (page == 8 || page == 9 || page == 14)
        {
            shadersNavigation->setChecked(true);
            showSectionTabs({ { QStringLiteral("Setup"), 14 },
                { QStringLiteral("Standard"), 8 },
                { QStringLiteral("NLS"), 9 } }, page);
        }
        else if (page == 2 || page == 4 || page == 11 || page == 13 ||
			page == 16 || page == 17 || page == 18 || page == 19)
        {
            vpNavigation->setChecked(true);
            showSectionTabs({ { QStringLiteral("Rendering"), 2 },
				{ QStringLiteral("Color / Output"), 16 },
				{ QStringLiteral("Scaling"), 17 },
				{ QStringLiteral("Screen"), 4 },
				{ QStringLiteral("Zoom"), 18 },
                { QStringLiteral("Subtitles"), 19 },
				{ QStringLiteral("Processing"), 11 } }, page);
        }
        else if (page == 3 || page == 12)
        {
            directShowNavigation->setChecked(true);
            showSectionTabs({ { QStringLiteral("General"), 3 },
                { QStringLiteral("Input Processing"), 12 } }, page);
        }
        else if (page == 6 || page == 15)
        {
            shortcutsNavigation->setChecked(true);
            showSectionTabs({ { QStringLiteral("Setup"), 15 },
                { QStringLiteral("Shortcuts"), 6 } }, page);
        }
        else
            showSectionTabs({}, page);
    };
    connect(sectionTabs, &QTabBar::currentChanged, this,
        [this, sectionTabs](int tab)
        {
            if (tab < 0) return;
            const int page = sectionTabs->tabData(tab).toInt();
            if (page >= 0 && page < pages_->count() && pages_->currentIndex() != page)
                selectPage(page);
        });
    connect(pages_, &QStackedWidget::currentChanged, this, updateSectionTabs);
    updateSectionTabs(pages_->currentIndex());
    centerLayout->addWidget(navigation_);
    centerLayout->addWidget(pageHost, 1);
    configurationHost_ = center;
	if (noTarget_)
	{

		auto* emptyState = new QWidget;
		auto* emptyLayout = new QVBoxLayout(emptyState);
		emptyLayout->setContentsMargins(24, 24, 24, 24);
		emptyLayout->addStretch();
		auto* emptyTitle = new QLabel(
			targetSearchTitleText(initialSearchPending_));
		emptyTitle->setObjectName(QStringLiteral("config.targetSearchTitle"));
		targetSearchTitle_ = emptyTitle;
		emptyTitle->setWordWrap(true);
		emptyTitle->setMaximumWidth(700);
		emptyTitle->setStyleSheet(QStringLiteral(
			"font-size: 22px; font-weight: 600; color: #f5f8ff;"));
		emptyTitle->setAlignment(Qt::AlignCenter);
		emptyLayout->addWidget(emptyTitle);
		auto* help = new QLabel(initialSearchPending_ ?
			(rememberedTargetId_.isEmpty() ? QStringLiteral(
				"Checking for VideoProcessors on this LAN. Select a target "
				"above to edit its settings.") :
				QStringLiteral("Checking your LAN for the last selected VideoProcessor.")) :
			QStringLiteral("Available targets appear automatically. Select another "
				"target above if you want to edit its settings."));
		help->setObjectName(QStringLiteral("config.targetSearchHelp"));
		targetSearchHelp_ = help;
		help->setWordWrap(true);
		help->setAlignment(Qt::AlignCenter);
		help->setFixedWidth(560);
		help->setMinimumHeight(64);
		help->setStyleSheet(QStringLiteral(
			"font-size: 14px; color: #a8bad0;"));
		emptyLayout->addWidget(help, 0, Qt::AlignHCenter);
		auto* progress = new QProgressBar(emptyState);
		progress->setObjectName(QStringLiteral("configTargetSearchProgress"));
		progress->setRange(0, 0);
		progress->setTextVisible(false);
		progress->setFixedSize(220, 5);
		progress->setVisible(initialSearchPending_);
		targetSearchProgress_ = progress;
		emptyLayout->addWidget(progress, 0, Qt::AlignHCenter);
		emptyLayout->addStretch();
        pages_->addWidget(emptyState);
        pages_->setCurrentWidget(emptyState);
        for (auto* button : navigation_->findChildren<QPushButton*>())
            button->setEnabled(button->property("pageIndex").toInt() == 21);   // Updates (our LED drive is 20)
	}
    rootLayout->addWidget(center, 1);

    auto* footer = new QWidget;
    footer->setObjectName(QStringLiteral("footer"));
    footer->setMinimumHeight(54);
    footer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(16, 8, 16, 8);
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("configurationStatus"));
    status_->setAccessibleName(QStringLiteral("Configuration status"));
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    status_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    status_->setWordWrap(true);
    footerLayout->addWidget(status_, 1);
    effectSummary_ = new QLabel;
    effectSummary_->setObjectName(QStringLiteral("configurationEffectSummary"));
    effectSummary_->setAccessibleName(QStringLiteral("Pending configuration effect"));
    effectSummary_->setWordWrap(true);
    effectSummary_->setMinimumWidth(180);
    footerLayout->addWidget(effectSummary_);
    applyButton_ = new QPushButton(QStringLiteral("&Apply"));
    saveButton_ = new QPushButton(QStringLiteral("&OK"));
    auto* cancel = new QPushButton(QStringLiteral("&Cancel"));
    applyButton_->setObjectName(QStringLiteral("applyConfiguration"));
    saveButton_->setObjectName(QStringLiteral("okConfiguration"));
    cancel->setObjectName(QStringLiteral("cancelConfiguration"));
    saveButton_->setProperty("primary", true);
    saveButton_->setDefault(true);
    applyButton_->setEnabled(configurationLoaded_ && dirty_);
    saveButton_->setEnabled(configurationLoaded_);
    footerLayout->addWidget(saveButton_);
    footerLayout->addWidget(cancel);
    footerLayout->addWidget(applyButton_);
    rootLayout->addWidget(footer);
    updateEffectSummary();

    connect(saveButton_, &QPushButton::clicked, this, [this]
    {
        // A clean OK is just a close. A dirty OK must remain open if validation
        // or safe persistence fails, so the user can correct the candidate.
        if (!dirty_) hide();
        else if (saveChanges()) hide();
    });
    connect(cancel, &QPushButton::clicked, this, [this]
    {
        // Cancel is intentionally an editor-only operation: the working copy
        // is discarded without touching the file or signaling VideoProcessor.
        dirty_ = false;
        hide();
		if (remoteClient_ && document_)
		{
			const std::string baseline = document_->loadedBytes;
			document_->LoadBytes(configPath_.toStdWString(), baseline);
			savedSnapshot_ = captureDocumentSnapshot(*document_);
			hasPendingMigrations_ = false;
		}
		else loadConfiguration();
		rebuildConfigurationShell();
    });
    connect(applyButton_, &QPushButton::clicked, this, [this] { applyChanges(); });
    auto* saveShortcut = new QShortcut(QKeySequence::Save, root);
    saveShortcut->setContext(Qt::ApplicationShortcut);
    connect(saveShortcut, &QShortcut::activated, applyButton_, [this]
    {
        if (applyButton_ && applyButton_->isEnabled()) applyButton_->click();
    });
    auto selectAdjacentPage = [this](int direction)
    {
        if (!pages_ || pages_->count() == 0) return;
        selectPage((pages_->currentIndex() + direction + pages_->count()) % pages_->count());
    };
    auto* nextPage = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Tab")), root);
    nextPage->setContext(Qt::ApplicationShortcut);
    connect(nextPage, &QShortcut::activated, this,
        [selectAdjacentPage] { selectAdjacentPage(1); });
    auto* previousPage = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Tab")), root);
    previousPage->setContext(Qt::ApplicationShortcut);
    connect(previousPage, &QShortcut::activated, this,
        [selectAdjacentPage] { selectAdjacentPage(-1); });

    applyRendererVisibilityFilter(configuredBooleanValue(value(
        QStringLiteral("general"), QStringLiteral("hide_legacy_renderers")), true));
    updateValidationState();
    return root;
}

QPushButton* ConfigEditorWindow::addNavigationButton(const QString& text, int pageIndex)
{
    auto* button = new QPushButton;
    button->setText(text);
    button->setAccessibleName(text);
    button->setAccessibleDescription(QStringLiteral("Open the %1 settings page.").arg(text));
    button->setProperty("nav", true);
    button->setProperty("pageIndex", pageIndex);
    button->setCheckable(true);
    button->setFlat(true);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(button, &QToolButton::clicked, this, [this, pageIndex] { selectPage(pageIndex); });
    return button;
}

QWidget* ConfigEditorWindow::createCard(const QString& title, const QString& description, QWidget* content)
{
    auto* card = new QFrame;
    card->setProperty("card", true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(kCardPadding, kCardPadding, kCardPadding, kCardPadding);
    layout->setSpacing(6);
    auto* heading = new QLabel(title);
    heading->setProperty("cardTitle", true);
    layout->addWidget(heading);
    if (!description.isEmpty()) layout->addWidget(helpLabel(description));
    layout->addSpacing(4);
    layout->addWidget(content);
    return card;
}

QWidget* ConfigEditorWindow::createPage(const QString& title, const QString& description, QWidget* body)
{
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setAccessibleName(QStringLiteral("%1 settings").arg(title));
    scroll->setObjectName(QStringLiteral("pageViewport"));
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("pageViewport"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(kPageMargin, 10, kPageMargin, 8);
    layout->setSpacing(4);
    auto* heading = new QLabel(title);
    heading->setObjectName(QStringLiteral("pageTitle"));
    heading->setAccessibleName(QStringLiteral("%1 settings").arg(title));
    layout->addWidget(heading);
    auto* subtitle = new QLabel(description);
    subtitle->setObjectName(QStringLiteral("pageDescription"));
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);
    layout->addSpacing(10);
    layout->addWidget(body);
    layout->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget* ConfigEditorWindow::createStartupPage()
{
    // Keep old configurations loadable, but migrate the former public display
    // name in the pending document so the next editor save uses the final name.
    if (configurationLoaded_ && document_ &&
        value(QStringLiteral("general"), QStringLiteral("renderer")).compare(
            QStringLiteral("VideoProcessor Renderer (Alpha)"), Qt::CaseInsensitive) == 0)
    {
        document_->SetKnown("general", "renderer", "VP Renderer");
        dirty_ = true;
        hasPendingMigrations_ = true;
    }

	auto* cards = new ResponsiveCardGrid;
	auto persistDiscoveredDefault = [this](const QString& key,
		const QStringList& choices)
	{
		if (!configurationLoaded_ || !document_ ||
			!value(QStringLiteral("general"), key).trimmed().isEmpty())
			return;
		QString selected;
		for (const QString& choice : choices)
			if (!choice.trimmed().isEmpty())
			{
				selected = choice.trimmed();
				break;
			}
		if (selected.isEmpty()) return;
		// A missing or zero-byte file has no legacy renderer-profile layout to
		// preserve. Mark the document as the unified format before adding startup
		// hardware keys so [general] is validated with the application schema.
		if (document_->loadedBytes.empty())
			document_->AddSection("vprenderer");
		document_->AddSection("general");
		if (document_->SetKnown("general", key.toLocal8Bit().constData(),
			selected.toLocal8Bit().constData()))
		{
			dirty_ = true;
			hasPendingMigrations_ = true;
		}
	};

    auto* hardware = new QWidget;
    auto* hardwareForm = new QFormLayout(hardware);
    hardwareForm->setContentsMargins(0, 0, 0, 0);
    hardwareForm->setHorizontalSpacing(18);
    hardwareForm->setVerticalSpacing(8);
	persistDiscoveredDefault(QStringLiteral("capture_device"), captureDevices_);
	auto* captureDevice = bindChoiceField(QStringLiteral("general"),
        QStringLiteral("capture_device"), captureDevices_.isEmpty() ?
            QStringList{ QString() } : captureDevices_,
        captureDevices_.isEmpty() ? QStringList{ QStringLiteral("No capture devices discovered") } : QStringList{});
    hardwareForm->addRow(QStringLiteral("Capture device"), captureDevice);

    auto* captureConnection = new QComboBox;
    captureConnection->setObjectName(QStringLiteral("fieldControl"));
    captureConnection->setObjectName(controlName(QStringLiteral("general"),
        QStringLiteral("capture_input")));
    auto updateCaptureConnections = [this, captureConnection](const QString& deviceName)
    {
        const QSignalBlocker blocker(captureConnection);
        captureConnection->clear();
        QStringList connections;
        for (auto found = captureConnections_.cbegin();
            found != captureConnections_.cend(); ++found)
            if (found.key().compare(deviceName, Qt::CaseInsensitive) == 0)
            {
                connections = found.value();
                break;
            }
        QStringList uniqueConnections;
        for (const QString& connection : connections)
        {
            const QString normalized = connection.trimmed();
            if (normalized.isEmpty() ||
                uniqueConnections.contains(normalized, Qt::CaseInsensitive))
                continue;
            uniqueConnections.push_back(normalized);
        }
        const QString configured = value(QStringLiteral("general"),
            QStringLiteral("capture_input"));
        if (uniqueConnections.size() == 1)
        {
            captureConnection->addItem(uniqueConnections.front(),
                uniqueConnections.front());
            captureConnection->setCurrentIndex(0);
            captureConnection->setEnabled(false);
            captureConnection->setToolTip(
                QStringLiteral("This capture device exposes only one input connection."));
            return;
        }

        captureConnection->setEnabled(uniqueConnections.size() > 1);
        captureConnection->setToolTip(uniqueConnections.isEmpty() ?
            QStringLiteral("No input connections were discovered for this capture device.") :
            QStringLiteral("Choose the capture device input connection."));
        captureConnection->addItem(uniqueConnections.isEmpty() ?
            QStringLiteral("No inputs discovered") : QStringLiteral("Device default"),
            QString());
        for (const QString& connection : uniqueConnections)
            captureConnection->addItem(connection, connection);
        if (!configured.isEmpty())
        {
            int index = captureConnection->findData(configured, Qt::UserRole,
                Qt::MatchFixedString);
            if (index < 0)
            {
                captureConnection->addItem(
                    QStringLiteral("%1 (unavailable)").arg(configured), configured);
                index = captureConnection->count() - 1;
            }
            captureConnection->setCurrentIndex(index);
        }
    };
    updateCaptureConnections(captureDevice->currentText());
    connect(captureDevice, &QComboBox::currentTextChanged, this,
        updateCaptureConnections);
    connect(captureConnection, qOverload<int>(&QComboBox::activated), this,
        [this, captureConnection](int index)
    {
        if (!document_ || index < 0) return;
        const QString selected = captureConnection->itemData(index).toString();
        if (selected.isEmpty()) document_->RemoveKnown("general", "capture_input");
        else document_->SetKnown("general", "capture_input",
            selected.toLocal8Bit().constData());
        markDirty();
    });
    hardwareForm->addRow(QStringLiteral("Input connection"), captureConnection);
    auto* behavior = new QWidget;
    auto* behaviorLayout = new QVBoxLayout(behavior);
    behaviorLayout->setContentsMargins(0, 0, 0, 0);
    behaviorLayout->setSpacing(8);
    behaviorLayout->addWidget(bindCheckField(QStringLiteral("Start fullscreen"), QStringLiteral("general"), QStringLiteral("fullscreen")));
    behaviorLayout->addWidget(bindCheckField(QStringLiteral("Windowed fullscreen"), QStringLiteral("general"), QStringLiteral("windowed_fullscreen_mode")));
    behaviorLayout->addWidget(bindCheckField(QStringLiteral("Start as Video Only"),
        QStringLiteral("general"), QStringLiteral("noui"),
        configuredBooleanValue(value(QStringLiteral("general"),
            QStringLiteral("no_ui")), false)));
    behaviorLayout->addWidget(bindCheckField(QStringLiteral("Start minimized"), QStringLiteral("general"), QStringLiteral("startminimized")));
    // Delta 6 (settings first): the saved state of what a key or a main-window
    // control can change.
    auto* statsOverlay = bindCheckField(QStringLiteral("Show statistics overlay"),
        QStringLiteral("general"), QStringLiteral("stats_overlay"), false);
    statsOverlay->setToolTip(QStringLiteral(
        "Shows the statistics overlay at start. The statistics key toggles it until restart."));
    behaviorLayout->addWidget(statsOverlay);
    auto* useQueue = bindCheckField(QStringLiteral("Use the frame queue"),
        QStringLiteral("general"), QStringLiteral("use_frame_queue"), true);
    useQueue->setToolTip(QStringLiteral(
        "The renderer's frame queue (the main window's \"use queue\" check). On is the normal setting."));
    behaviorLayout->addWidget(useQueue);
    auto* autoReset = bindCheckField(QStringLiteral("Reset the queue automatically"),
        QStringLiteral("general"), QStringLiteral("queue_auto_reset"), true);
    autoReset->setToolTip(QStringLiteral(
        "Resets an over-full queue by itself (the main window's \"auto reset\" check), at the "
        "queue recovery threshold."));
    behaviorLayout->addWidget(autoReset);
    auto* persistSelection = bindCheckField(QStringLiteral("Keep key-selected profiles after restart"),
        QStringLiteral("general"), QStringLiteral("persist_profile_selection"), false);
    persistSelection->setToolTip(QStringLiteral(
        "Off (default): a profile chosen with a key lasts until restart or until the rules select a "
        "different profile. On: the earlier behaviour, choices are saved to VideoProcessor.state and "
        "kept across restarts as fallbacks."));
    behaviorLayout->addWidget(persistSelection);
    // Scene detection is the sole supported choice here. It is deliberately
    // opt-in: an absent value means off, and clearing the checkbox writes the
    // explicit false value. The basic correction and feature-disable switches
    // are retained for manual compatibility editing only.
    behaviorLayout->addWidget(bindCheckField(QStringLiteral("Scene detection"),
        QStringLiteral("general"), QStringLiteral("scene_detect"), false));

    auto* source = new QWidget;
    auto* sourceForm = new QFormLayout(source);
    sourceForm->setContentsMargins(0, 0, 0, 0);
    sourceForm->setVerticalSpacing(8);
    monitorChoice_ = bindChoiceField(QStringLiteral("general"),
        QStringLiteral("fullscreen_monitor_name"), withDefaultChoice(monitors_),
        { QStringLiteral("Default monitor") });
    sourceForm->addRow(QStringLiteral("Monitor"), monitorChoice_);
	const bool hideLegacy = configuredBooleanValue(value(QStringLiteral("general"),
		QStringLiteral("hide_legacy_renderers")), true);
	const QStringList visibleRenderers = hideLegacy ? filteredRenderers_ : allRenderers_;
	persistDiscoveredDefault(QStringLiteral("renderer"), visibleRenderers);
    rendererChoice_ = bindChoiceField(QStringLiteral("general"),
        QStringLiteral("renderer"), allRenderers_.isEmpty() ?
            QStringList{ QString() } : allRenderers_,
        allRenderers_.isEmpty() ? QStringList{ QStringLiteral("No renderers discovered") } : QStringList{});
    for (int index = 0; index < rendererChoice_->count(); ++index)
        rendererChoice_->setItemData(index,
            rendererChoice_->itemData(index).toString(), kRendererNameRole);
    sourceForm->addRow(QStringLiteral("Renderer"), rendererChoice_);
    connect(rendererChoice_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this] { refreshShaderCacheStatus(); });
    auto* hideLegacyRenderers = configuredCheck(
        QStringLiteral("Hide legacy renderers"), value(QStringLiteral("general"),
            QStringLiteral("hide_legacy_renderers")), true);
    hideLegacyRenderers->setObjectName(controlName(QStringLiteral("general"),
        QStringLiteral("hide_legacy_renderers")));
    hideLegacyRenderers->setEnabled(true);
    sourceForm->addRow(QString(), hideLegacyRenderers);
    connect(hideLegacyRenderers, &QCheckBox::toggled, this,
        [this](bool hide) { applyRendererVisibilityFilter(hide); });
    connect(hideLegacyRenderers, &QCheckBox::toggled, this,
        [this, hideLegacyRenderers](bool checked)
    {
        if (!document_) return;
        document_->SetKnown("general", "hide_legacy_renderers",
            checked ? "true" : "false");
        editOrder_[hideLegacyRenderers->objectName()] = ++editSerial_;
        dirty_ = true;
        if (effectSummary_)
            effectSummary_->setText(QStringLiteral(
                "Save required: Config renderer visibility"));
        if (saveButton_)
            saveButton_->setEnabled(configurationLoaded_ && validationValid_);
        if (applyButton_)
            applyButton_->setEnabled(configurationLoaded_ && validationValid_);
    });
    applyRendererVisibilityFilter(hideLegacy);
    prepareRendererPopup();
	sourceForm->addRow(QStringLiteral("Switch refresh rate"), bindChoiceField(
		QStringLiteral("general"), QStringLiteral("switch_refresh_rate"),
		{ QStringLiteral("never"), QStringLiteral("fullscreen_only"),
		  QStringLiteral("always") },
		{ QStringLiteral("Never"), QStringLiteral("Full Screen Only"),
		  QStringLiteral("Always") }));
	auto* profileChangeDisplay = new QSpinBox;
	profileChangeDisplay->setObjectName(controlName(QStringLiteral("general"),
		QStringLiteral("profile_change_display_seconds")));
	profileChangeDisplay->setAccessibleName(
		QStringLiteral("Profile change display duration"));
	profileChangeDisplay->setRange(0, 60);
	profileChangeDisplay->setSuffix(QStringLiteral(" seconds"));
	profileChangeDisplay->setSpecialValueText(QStringLiteral("Off"));
	profileChangeDisplay->setToolTip(QStringLiteral(
		"How long profile selections remain visible, including the fade. Set 0 to never display them."));
	bool profileDisplayOk = false;
	const int configuredProfileDisplay = value(QStringLiteral("general"),
		QStringLiteral("profile_change_display_seconds")).toInt(&profileDisplayOk);
	profileChangeDisplay->setValue(profileDisplayOk ? configuredProfileDisplay : 5);
	connect(profileChangeDisplay, qOverload<int>(&QSpinBox::valueChanged), this,
		[this, profileChangeDisplay](int seconds)
	{
		if (!document_) return;
		document_->SetKnown("general", "profile_change_display_seconds",
			std::to_string(seconds));
		editOrder_[profileChangeDisplay->objectName()] = ++editSerial_;
		markDirty();
	});
	sourceForm->addRow(QStringLiteral("Profile display"),
		profileChangeDisplay);

    auto* input = new QWidget;
    auto* inputForm = new QFormLayout(input);
    inputForm->setContentsMargins(0, 0, 0, 0);
    inputForm->setVerticalSpacing(8);
    inputForm->addRow(QStringLiteral("Video conversion"), bindChoiceField(QStringLiteral("general"),
        QStringLiteral("video_conversion"), { QString(), QStringLiteral("NONE"), QStringLiteral("V210_TO_P010") },
        { QStringLiteral("Not set (default: Disabled)"), QStringLiteral("Disabled"), QStringLiteral("V210 to P010") }));
    inputForm->addRow(QStringLiteral("Container color space"), bindChoiceField(QStringLiteral("general"),
        QStringLiteral("container_colorspace"),
        { QString(), QStringLiteral("BT2020"), QStringLiteral("P3_D65"), QStringLiteral("P3_DCI"),
          QStringLiteral("P3_D60"), QStringLiteral("REC709"), QStringLiteral("REC601_525"), QStringLiteral("REC601_625") },
        { QStringLiteral("Follow input") }));
    inputForm->addRow(QStringLiteral("HDR color space"), bindChoiceField(QStringLiteral("general"),
        QStringLiteral("hdr_colorspace"),
        { QStringLiteral("FOLLOW_INPUT"), QStringLiteral("FOLLOW_INPUT_LLDV"),
          QStringLiteral("FOLLOW_CONTAINER"), QStringLiteral("BT2020"), QStringLiteral("P3"), QStringLiteral("REC709") },
        { QStringLiteral("Follow input"), QStringLiteral("Follow input (LLDV)"),
          QStringLiteral("Follow container"), QStringLiteral("BT.2020"), QStringLiteral("P3"), QStringLiteral("Rec. 709") }));
    inputForm->addRow(QStringLiteral("HDR luminance"), bindChoiceField(QStringLiteral("general"),
        QStringLiteral("hdr_luminance"),
        { QStringLiteral("FOLLOW_INPUT"), QStringLiteral("FOLLOW_INPUT_LLDV"), QStringLiteral("HDR_LUMINANCE_USER") },
        { QStringLiteral("Follow input"), QStringLiteral("Follow input (LLDV)"), QStringLiteral("User values") }));

    // Stage 9: room lights (the profile rules' `ambient` variable).
    auto* ambient = new QWidget;
    auto* ambientForm = new QFormLayout(ambient);
    ambientForm->setContentsMargins(0, 0, 0, 0);
    ambientForm->setVerticalSpacing(8);
    auto* ambientMode = bindChoiceField(QStringLiteral("ambient"), QStringLiteral("mode"),
        { QStringLiteral("off"), QStringLiteral("manual"), QStringLiteral("sensor") },
        { QStringLiteral("Off"), QStringLiteral("Manual"), QStringLiteral("Light sensor") });
    ambientMode->setToolTip(QStringLiteral(
        "Off: rules see no room state. Manual: the room is as set below. Light sensor: a light sensor "
        "decides (an ESP32 / Arduino with a VEML7700 or TSL2591 on its own USB port; the bundled sketch is "
        "tools/light-sensor/vp_light_sensor.ino). Without readings the room below is used."));
    ambientForm->addRow(QStringLiteral("Room lights"), ambientMode);
    auto* ambientRoom = bindChoiceField(QStringLiteral("ambient"), QStringLiteral("room"),
        { QStringLiteral("dark"), QStringLiteral("lit") }, { QStringLiteral("Dark"), QStringLiteral("Lit") });
    ambientRoom->setToolTip(QStringLiteral(
        "The room in Manual mode, and the fallback when the light sensor is not reporting. The Room "
        "lights key flips it until restart."));
    ambientForm->addRow(QStringLiteral("Room"), ambientRoom);
    struct AmbientNumber { const char* label; const char* key; double hi; const char* placeholder; const char* tip; };
    const AmbientNumber ambientNumbers[] = {
        { "Lit above (lux)", "lux_lit", 100000.0, "20", "Light sensor: above this the room is lit." },
        { "Dark below (lux)", "lux_dark", 100000.0, "8", "Light sensor: below this the room is dark; in between it stays as it was." },
        { "Hold (seconds)", "hold_s", 600.0, "5", "Light sensor: a change must last this long (a door, someone passing)." } };
    for (const AmbientNumber& number : ambientNumbers)
    {
        auto* edit = bindTextField(QStringLiteral("ambient"), QString::fromLatin1(number.key));
        edit->setValidator(new QDoubleValidator(0.0, number.hi, 3, edit));
        edit->setPlaceholderText(QString::fromLatin1(number.placeholder));
        edit->setToolTip(QString::fromLatin1(number.tip));
        ambientForm->addRow(QString::fromLatin1(number.label), edit);
    }
    auto* ambientPort = bindTextField(QStringLiteral("ambient"), QStringLiteral("port"));
    ambientPort->setPlaceholderText(QStringLiteral("COM5"));
    ambientPort->setToolTip(QStringLiteral(
        "Light sensor: its own USB serial port (Device Manager, Ports). Never the projector's RS-232 port: "
        "a port opens for one user at a time, so Apply refuses the same port."));
    ambientForm->addRow(QStringLiteral("Sensor port"), ambientPort);
    auto* ambientBaud = bindTextField(QStringLiteral("ambient"), QStringLiteral("baud"));
    ambientBaud->setValidator(new QIntValidator(1200, 921600, ambientBaud));
    ambientBaud->setPlaceholderText(QStringLiteral("115200"));
    ambientBaud->setToolTip(QStringLiteral("Light sensor: the serial speed its sketch uses (the bundled sketch: 115200)."));
    ambientForm->addRow(QStringLiteral("Sensor speed (baud)"), ambientBaud);
    auto* ambientNote = new QLabel(QStringLiteral(
        "Profiles choose by it: a rendering profile with <b>when: $ambient==\"lit\"</b> and a raised SDR "
        "black level renders to the lit room's black."));
    ambientNote->setWordWrap(true);
    ambientForm->addRow(ambientNote);

    cards->addCard(createCard(QStringLiteral("Hardware"),
        QStringLiteral("Capture selection used when VP starts."), hardware));
    cards->addCard(createCard(QStringLiteral("General behavior"),
        QStringLiteral("How VideoProcessor opens and prepares a source."), behavior));
    cards->addCard(createCard(QStringLiteral("Display"),
        QStringLiteral("Monitor targeting and refresh-rate behavior. Refresh switching currently applies to VP Renderer."), source));
    cards->addCard(createCard(QStringLiteral("Input processing"),
        QStringLiteral("Shared source conversion and metadata defaults. Apply restarts the renderer; renderer pages may override each value."), input));
    cards->addCard(createCard(QStringLiteral("Room lights"),
        QStringLiteral("Dark or lit room, for profiles that render to the room's black."), ambient));
    return createPage(QStringLiteral("General"), QStringLiteral("Choose how VideoProcessor starts and which hardware it uses."), cards);
}

QString ConfigEditorWindow::legacySubtitleHdrValue(const QString& key) const
{
                    QString legacy;
                    for (const QString& root : {QStringLiteral("vprenderer.viewport"),QStringLiteral("vprenderer.zoom")}) {
                        const auto sections=profileSections(root);
                        if (sections.isEmpty()) continue;
                        QString selected=sections.front();
                        for (const auto& binding : activeProfileLists_)
                            if (binding.sectionPrefix==root)
                                for (int i=0;i<binding.list->count();++i)
                                    if (binding.list->item(i)->data(ActiveProfileRole).toBool())
                                        selected=binding.list->item(i)->data(Qt::UserRole).toString();
                        QString configured=value(selected,key);
                        if (configured.isEmpty()) configured=value(sections.front(),key);
                        if (!configured.isEmpty()) legacy=configured;
                    }
                    if (!legacy.isEmpty()) return legacy;
                    if (key==QStringLiteral("hdr_peak_analysis_height_percent")) return QStringLiteral("75");
                    if (key==QStringLiteral("hdr_peak_analysis_position")) return QStringLiteral("top");
                    return QStringLiteral("false");
}

QWidget* ConfigEditorWindow::createProfilePage(const QString& title, const QString& description,
    const QString& sectionPrefix)
{
    // A short-lived Color Config split placed the SDR white/black targets in
    // the wrong group. Restore them to the matching Rendering profile in the
    // pending document. The disk file changes only after the user explicitly
    // applies the editor changes.
    if (configurationLoaded_ && document_ &&
        sectionPrefix == QStringLiteral("vprenderer"))
    {
        const QStringList luminanceKeys = {
            QStringLiteral("sdr_target_nits"),
            QStringLiteral("sdr_black_nits")
        };
        bool migrated = false;
        for (const QString& renderingSection : profileSections(
            QStringLiteral("vprenderer")))
        {
            const QString suffix = renderingSection == QStringLiteral("vprenderer") ?
                QStringLiteral("Default") :
                renderingSection.mid(QStringLiteral("vprenderer.").size());
            const QString colorSection =
                QStringLiteral("vprenderer.color.%1").arg(suffix);
            for (const QString& key : luminanceKeys)
            {
                const QString configured = value(colorSection, key);
                if (configured.isEmpty()) continue;
                if (value(renderingSection, key).isEmpty())
                    document_->SetKnown(renderingSection.toStdString(),
                        key.toStdString().c_str(),
                        configured.toLocal8Bit().constData());
                document_->RemoveKnown(colorSection.toStdString(),
                    key.toStdString().c_str());
                migrated = true;
            }
        }
        if (migrated)
        {
            dirty_ = true;
            hasPendingMigrations_ = true;
        }
    }

    // Literal roots are the legacy unnamed form. Profiles in the editor are
    // named and their file order alone selects the default, so migrate a root
    // to a unique generated name in the pending document. Disk is unchanged
    // until the user explicitly saves.
    const bool rendererOwnsInputPolicy = sectionPrefix == QStringLiteral("vprenderer") &&
        ( !value(sectionPrefix, QStringLiteral("video_conversion")).isEmpty() ||
          !value(sectionPrefix, QStringLiteral("container_colorspace")).isEmpty() ||
          !value(sectionPrefix, QStringLiteral("hdr_colorspace")).isEmpty() ||
          !value(sectionPrefix, QStringLiteral("hdr_luminance")).isEmpty() );
    // Do not hide an ambiguous Base identity by silently renaming its root.
    // Retain both sections so shared validation can request an explicit rename.
    if (configurationLoaded_ && document_ && !rendererOwnsInputPolicy &&
        !profileSections(sectionPrefix).contains(sectionPrefix + QStringLiteral(".base"), Qt::CaseInsensitive) &&
        profileSections(sectionPrefix).contains(sectionPrefix, Qt::CaseInsensitive))
    {
        const QStringList before = profileSections(sectionPrefix);
        QString renamed = sectionPrefix + QStringLiteral(".profile_1");
        int suffix = 2;
        while (before.contains(renamed, Qt::CaseInsensitive))
            renamed = sectionPrefix + QStringLiteral(".profile_%1").arg(suffix++);
        if (document_->RenameSection(sectionPrefix.toStdString(), renamed.toStdString()))
        {
            QStringList ordered = before;
            ordered[ordered.indexOf(sectionPrefix, 0, Qt::CaseInsensitive)] = renamed;
            const QString newDefault = ordered.front();
            if (newDefault != renamed)
            {
                QStringList defaultOnlyKeys;
                for (const QString& key : defaultOnlyKeys)
                {
                    const QString configured = value(renamed, key);
                    if (configured.isEmpty()) continue;
                    const std::string keyText = key.toStdString();
                    document_->RemoveKnown(renamed.toStdString(), keyText.c_str());
                    document_->SetKnown(newDefault.toStdString(), keyText.c_str(), configured.toLocal8Bit().constData());
                }
            }
            dirty_ = true;
            hasPendingMigrations_ = true;
        }
    }

    // Fold legacy queue ownership into the first ordered queue profile in the
    // pending document. Compatibility readers remain in place for old files,
    // but every save from this editor uses [queue.*] exclusively.
    if (configurationLoaded_ && document_ && sectionPrefix == QStringLiteral("queue"))
    {
        const QStringList queues = profileSections(sectionPrefix);
        if (!queues.isEmpty())
        {
            const QString baseline = queues.front();
            bool migrated = false;
            auto migrate = [this, &baseline, &migrated](const QString& oldSection,
                const QString& oldKey, const QString& newKey)
            {
                const QString legacy = value(oldSection, oldKey);
                if (legacy.isEmpty()) return;
                if (value(baseline, newKey).isEmpty())
                    document_->SetKnown(baseline.toStdString(),
                        newKey.toStdString().c_str(), legacy.toLocal8Bit().constData());
                document_->RemoveKnown(oldSection.toStdString(),
                    oldKey.toStdString().c_str());
                migrated = true;
            };
            migrate(QStringLiteral("queue_recovery"),
                QStringLiteral("reset_after_render_restart_seconds"),
                QStringLiteral("reset_after_render_restart_seconds"));
            migrate(QStringLiteral("queue_recovery"),
                QStringLiteral("reset_queue_too_large_percent"),
                QStringLiteral("reset_queue_too_large_percent"));
            migrate(QStringLiteral("directshow"),
                QStringLiteral("presentation_lead_frames"),
                QStringLiteral("lead_frames"));
            migrate(QStringLiteral("command_line"),
                QStringLiteral("queue_size"), QStringLiteral("queue_size"));
            for (const QString& queueSection : queues)
            {
                const QString legacy = value(queueSection,
                    QStringLiteral("steady_reserve_frames"));
                if (legacy.isEmpty()) continue;
                if (value(queueSection, QStringLiteral("target_frames")).isEmpty())
                    document_->SetKnown(queueSection.toStdString(), "target_frames",
                        legacy.toLocal8Bit().constData());
                document_->RemoveKnown(queueSection.toStdString(),
                    "steady_reserve_frames");
                migrated = true;
            }
            if (migrated) markDirty();
        }
    }
    struct Field
    {
        QString key;
        QWidget* widget = nullptr;
        enum Kind { Text, Boolean, SdrGamma, Choice, Integer } kind = Text;
        double displayScale = 1.0;
    };
    struct State { QString section; bool loading = false; };
    struct QueuePolicy
    {
        const char* id;
        const char* label;
        const char* summary;
        int capacity;
        int lead;
        int target;
    };
    static constexpr QueuePolicy queuePolicies[] = {
        { "low_latency", "Low latency", "Keeps at most two VP queue frames; targets one frame.", 2, 0, 1 },
        { "balanced", "Balanced", "Uses a moderate queue for ordinary playback.", 16, 3, 2 },
        { "reliable", "Reliable", "Uses a larger queue for the most resilient playback.", 32, 4, 3 },
    };
    auto state = std::make_shared<State>();
    auto fields = std::make_shared<std::vector<Field>>();
    std::function<void()> updateOutputCompatibility = [] {};
    std::function<void()> updateToneControls = [] {};

    auto* splitter = new ResponsiveSplitter;

    auto* listContent = new QWidget;
    auto* listLayout = new QVBoxLayout(listContent);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(8);
    auto* add = new QPushButton(QStringLiteral("+ Add profile"));
    add->setObjectName(controlName(sectionPrefix, QStringLiteral("add_profile")));
    listLayout->addWidget(add);
    auto* actions = new QHBoxLayout;
    auto* remove = new QPushButton(QStringLiteral("Remove"));
    auto* up = new QPushButton(QStringLiteral("Move up"));
    auto* down = new QPushButton(QStringLiteral("Move down"));
    remove->setObjectName(controlName(sectionPrefix, QStringLiteral("remove_profile")));
    up->setObjectName(controlName(sectionPrefix, QStringLiteral("move_up")));
    down->setObjectName(controlName(sectionPrefix, QStringLiteral("move_down")));
    remove->setProperty("danger", true);
    actions->addWidget(remove);
    actions->addWidget(up);
    actions->addWidget(down);
    listLayout->addLayout(actions);
    auto* list = new QListWidget;
    list->setObjectName(controlName(sectionPrefix, QStringLiteral("profiles")));
    list->setAccessibleName(QStringLiteral("%1 profiles").arg(title));
    list->setAccessibleDescription(
        QStringLiteral("Ordered profiles. The first profile is the default. Use the buttons or drag to reorder."));
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setDefaultDropAction(Qt::MoveAction);
    list->setDragDropOverwriteMode(false);
    const bool showsActiveProfile = sectionPrefix == QStringLiteral("queue") ||
        sectionPrefix == QStringLiteral("vprenderer") ||
        sectionPrefix == QStringLiteral("vprenderer.scaling") ||
        sectionPrefix == QStringLiteral("vprenderer.color") ||
        sectionPrefix == QStringLiteral("vprenderer.viewport") ||
        sectionPrefix == QStringLiteral("vprenderer.zoom") ||
        sectionPrefix == QStringLiteral("vprenderer.subtitles");
    if (showsActiveProfile)
    {
        list->setItemDelegate(new ProfileStateItemDelegate(list));
        activeProfileLists_.push_back({ list, sectionPrefix });
    }
    installProfileContextMenu(list, sectionPrefix);
    listLayout->addWidget(list, 1);

    auto* detail = new QWidget;
    auto* detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(12);
    auto* selectedTitle = new QLabel(QStringLiteral("Profile"));
    selectedTitle->setObjectName(controlName(sectionPrefix,
        QStringLiteral("selected_profile_status")));
    selectedTitle->setProperty("cardTitle", true);
    detailLayout->addWidget(selectedTitle);
    if (showsActiveProfile)
    {
        activeProfileLists_.back().selectedTitle = selectedTitle;
        detailLayout->addWidget(addLiveToggle(list, sectionPrefix), 0, Qt::AlignLeft);
    }
    auto* name = new QLineEdit;
    name->setObjectName(controlName(sectionPrefix, QStringLiteral("name")));
    detailLayout->addWidget(fieldWithHelp(QStringLiteral("Name"), name,
        QStringLiteral("The first profile in the list is the default; list order determines precedence.")));
    auto* shortcut = new QLineEdit;
    shortcut->setObjectName(controlName(sectionPrefix, QStringLiteral("shortcut")));
    detailLayout->addWidget(fieldWithHelp(QStringLiteral("Shortcut key"), shortcut,
        QStringLiteral("Optional. Activates this profile in addition to its rule.")));
    auto* cycleShortcut = new QLineEdit;
    cycleShortcut->setObjectName(controlName(sectionPrefix, QStringLiteral("cycle_shortcut")));
    detailLayout->addWidget(fieldWithHelp(QStringLiteral("Cycle shortcut key"), cycleShortcut,
        QStringLiteral("Optional. Cycles through profiles that use the same key, in list order.")));
    auto* useRule = new QCheckBox(QStringLiteral("Use rule"));
    useRule->setObjectName(controlName(sectionPrefix, QStringLiteral("use_rule")));
    detailLayout->addWidget(useRule);
    auto* rule = new QPlainTextEdit;
    rule->setObjectName(controlName(sectionPrefix, QStringLiteral("when")));
    rule->setPlaceholderText(QStringLiteral("${key} == \"F2\""));
    rule->setMaximumBlockCount(3);
    rule->setMinimumHeight(72);
    rule->setMaximumHeight(96);
    auto* ruleField = fieldWithHelp(QStringLiteral("When should VP use this profile?"), rule,
        QStringLiteral("Optional source-condition rule. Example: ${renderer} ~= \"VP Renderer\" && ${source_rate} <= 30. ~= matches a literal substring (case-insensitive for renderer names). Use one non-empty quoted string with a text variable, not a number, Boolean, or ${key}. Parentheses group conditions; ! negates, then && binds before ||. Active profiles can still be selected manually when their rule is false."));
    detailLayout->addWidget(ruleField);
    if (sectionPrefix == QStringLiteral("queue") || sectionPrefix == QStringLiteral("vprenderer"))
    {
        // The common queue rule as a plain control: "use this queue above N
        // fps"; also for rendering profiles, e.g. a low-latency profile
        // (audit 2026-10-14c: no hand-written rule). It writes the same rule as the formula field and follows it;
        // any other formula is left alone and the control steps aside.
        auto* rateRow = new QWidget;
        auto* rateLayout = new QHBoxLayout(rateRow);
        rateLayout->setContentsMargins(0, 0, 0, 0);
        auto* rateRule = new QCheckBox(sectionPrefix == QStringLiteral("queue") ?
            QStringLiteral("Use this queue for sources above") : QStringLiteral("Use this profile for sources above"));
        rateRule->setObjectName(controlName(sectionPrefix, QStringLiteral("rate_rule")));
        auto* rateThreshold = new QDoubleSpinBox;
        rateThreshold->setObjectName(controlName(sectionPrefix, QStringLiteral("rate_rule_fps")));
        rateThreshold->setRange(1.0, 1000.0);
        rateThreshold->setDecimals(2);
        rateThreshold->setValue(59.0);
        rateThreshold->setSuffix(QStringLiteral(" fps"));
        rateThreshold->setMaximumWidth(120);
        rateLayout->addWidget(rateRule);
        rateLayout->addWidget(rateThreshold);
        rateLayout->addStretch();
        auto* rateNote = helpLabel(QStringLiteral(
            "This profile has its own rule below; edit it there, or clear it to use the frame-rate switch."));
        rateNote->setObjectName(controlName(sectionPrefix, QStringLiteral("rate_rule_note")));
        rateNote->setVisible(false);
        const int ruleIndex = detailLayout->indexOf(useRule);
        detailLayout->insertWidget(ruleIndex, rateNote);
        detailLayout->insertWidget(ruleIndex, rateRow);
        auto syncFromRule = [rule, useRule, rateRule, rateThreshold, rateNote]
        {
            QSignalBlocker blockRule(rateRule);
            QSignalBlocker blockThreshold(rateThreshold);
            const QString text = useRule->isChecked() ? rule->toPlainText().trimmed() : QString();
            // ${cadence} is the exact rate (59.94); ${source_rate} is a whole
            // number (59.94 reads as 59), so the switch writes cadence. Rules
            // written with source_rate are still recognised.
            const QRegularExpressionMatch match = QRegularExpression(QStringLiteral(
                "^\\$\\{(?:cadence|source_rate)\\}\\s*>\\s*([0-9]+(?:\\.[0-9]+)?)$")).match(text);
            const bool custom = !text.isEmpty() && !match.hasMatch();
            rateRule->setChecked(match.hasMatch());
            if (match.hasMatch()) rateThreshold->setValue(match.captured(1).toDouble());
            rateRule->setEnabled(!custom && useRule->isEnabled());
            rateThreshold->setEnabled(!custom && useRule->isEnabled());
            rateNote->setVisible(custom);
        };
        auto applyRateRule = [rule, useRule, rateRule, rateThreshold]
        {
            if (rateRule->isChecked())
            {
                rule->setPlainText(QStringLiteral("${cadence} > %1")
                    .arg(QString::number(rateThreshold->value(), 'g', 6)));
                useRule->setChecked(true);
            }
            else
            {
                rule->clear();
                useRule->setChecked(false);
            }
        };
        connect(rule, &QPlainTextEdit::textChanged, this, syncFromRule);
        connect(useRule, &QCheckBox::toggled, this, syncFromRule);
        connect(rateRule, &QCheckBox::toggled, this, applyRateRule);
        connect(rateThreshold, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [rateRule, applyRateRule](double) { if (rateRule->isChecked()) applyRateRule(); });
    }

    auto* profileFields = new QWidget;
    auto* profileFieldsLayout = new QVBoxLayout(profileFields);
    profileFieldsLayout->setContentsMargins(0, 8, 0, 0);
    profileFieldsLayout->setSpacing(8);
    QFormLayout* form = nullptr;
    auto createForm = [] (QWidget* parent)
    {
        auto* created = new QFormLayout(parent);
        created->setContentsMargins(0, 0, 0, 0);
        created->setHorizontalSpacing(18);
        created->setVerticalSpacing(8);
        return created;
    };
    auto addPlainForm = [&]
    {
        auto* content = new QWidget;
        profileFieldsLayout->addWidget(content);
        return createForm(content);
    };
    auto addCollapsibleSection = [&](const QString& id, const QString& heading,
        const QString& description, bool expanded)
    {
        const QString sectionObjectPrefix = sectionPrefix == QStringLiteral("vprenderer.viewport") ?
            QStringLiteral("screenSection") :
            (sectionPrefix == QStringLiteral("vprenderer.zoom") ?
                QStringLiteral("zoomSection") : (sectionPrefix == QStringLiteral("vprenderer.subtitles") ?
                QStringLiteral("subtitleSection") : QStringLiteral("rendererSection")));
        auto* section = new QWidget(profileFields);
        auto* sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(5);
        auto* toggle = new QToolButton;
        toggle->setObjectName(QStringLiteral("%1.%2").arg(sectionObjectPrefix, id));
        toggle->setText(heading);
        toggle->setProperty("profileSection", true);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        toggle->setCheckable(true);
        toggle->setChecked(expanded);
        toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        toggle->setAccessibleDescription(
            QStringLiteral("Expand or collapse the %1 settings.").arg(heading));
        sectionLayout->addWidget(toggle);
        auto* content = new QWidget(section);
        content->setObjectName(QStringLiteral("%1.%2.content").arg(sectionObjectPrefix, id));
        auto* contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(12, 4, 4, 8);
        contentLayout->setSpacing(7);
        if (!description.isEmpty()) contentLayout->addWidget(helpLabel(description));
        auto* formContent = new QWidget;
        QFormLayout* sectionForm = createForm(formContent);
        contentLayout->addWidget(formContent);
        sectionLayout->addWidget(content);
        // The content must be parented and attached before it can be made
        // visible. Calling setVisible(true) while it is parentless promotes it
        // to a temporary native top-level window on Windows.
        content->setVisible(expanded);
        connect(toggle, &QToolButton::toggled, this, [toggle, content](bool open)
        {
            content->setVisible(open);
            toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        });
        profileFieldsLayout->addWidget(section);
        return sectionForm;
    };
    if (sectionPrefix != QStringLiteral("vprenderer") &&
        sectionPrefix != QStringLiteral("vprenderer.color") &&
        sectionPrefix != QStringLiteral("vprenderer.viewport") &&
        sectionPrefix != QStringLiteral("vprenderer.zoom"))
        form = addPlainForm();
    QCheckBox* sdrGammaEnabled = nullptr;
    QLabel* sdrGammaStatus = nullptr;
    QPushButton* sdrGammaInherit = nullptr;
    QComboBox* desiredSdrGamma = nullptr;
    QComboBox* combinedSdrResponse = nullptr;
    QCheckBox* anamorphicEnabled = nullptr;
    QLineEdit* anamorphicValue = nullptr;
	QComboBox* hdrAnalysisMode = nullptr;
	QCheckBox* pictureOnlyHdrAnalysis = nullptr;
	QCheckBox* motionCompensatedHdrAnalysis = nullptr;
	QLineEdit* hdrAnalysisHeight = nullptr;
	QComboBox* hdrAnalysisPosition = nullptr;






    const int fixedUnitFieldWidth = QLineEdit().sizeHint().width();
    const auto deprecatedViewportAlias = [sectionPrefix](const QString& key) -> QString
    {
        if (sectionPrefix != QStringLiteral("vprenderer.viewport")) return {};
        if (key == QStringLiteral("screen_aspect")) return QStringLiteral("scope_screen_aspect");
        if (key == QStringLiteral("automatic_crop")) return QStringLiteral("scope_automatic_crop");
        if (key == QStringLiteral("subtitle_fit")) return QStringLiteral("scope_subtitle_fit");
        if (key == QStringLiteral("subtitle_hold_seconds")) return QStringLiteral("scope_subtitle_hold_seconds");
        if (key == QStringLiteral("subtitle_engage_drift_ms")) return QStringLiteral("scope_subtitle_engage_drift_ms");
        if (key == QStringLiteral("subtitle_release_drift_ms")) return QStringLiteral("scope_subtitle_release_drift_ms");
        if (key == QStringLiteral("subtitle_padding_pixels")) return QStringLiteral("scope_subtitle_padding_pixels");
        return {};
    };
    auto addText = [&](const QString& label, const QString& key,
        const QString& unit = {}, double displayScale = 1.0)
    {
        auto* edit = new QLineEdit;
        edit->setObjectName(controlName(sectionPrefix, key));
        edit->setAccessibleName(label);
        if (!unit.isEmpty()) edit->setAccessibleDescription(
            QStringLiteral("Value in %1.").arg(unit));
        QWidget* control = edit;
        if (!unit.isEmpty())
        {
            auto* row = new QWidget;
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);
            edit->setFixedWidth(fixedUnitFieldWidth);
            edit->setAlignment(Qt::AlignRight);
            rowLayout->addWidget(edit);
            auto* unitLabel = new QLabel(unit);
            unitLabel->setObjectName(controlName(sectionPrefix, key) + QStringLiteral(".unit"));
            rowLayout->addWidget(unitLabel);
            rowLayout->addStretch();
            control = row;
        }
        form->addRow(label, control);
        fields->push_back({ key, edit, Field::Text, displayScale });
        connect(edit, &QLineEdit::textChanged, this,
            [this, state, key, sectionPrefix, deprecatedViewportAlias,
                displayScale, edit](const QString& text)
        {
            if (state->loading || state->section.isEmpty() || !document_) return;
            // Text inherited from the default profile is deliberately muted.
            // As soon as the operator types an override, update that state
            // immediately instead of leaving the new value looking inherited
            // until they reselect the profile.
            const bool inherited = text.trimmed().isEmpty() &&
                state->section != sectionPrefix;
            if (edit->property("inherited").toBool() != inherited)
            {
                edit->setProperty("inherited", inherited);
                edit->style()->unpolish(edit);
                edit->style()->polish(edit);
            }
            const QString alias = deprecatedViewportAlias(key);
            if (!alias.isEmpty())
                document_->RemoveKnown(state->section.toStdString(), alias.toStdString().c_str());
			if (text.trimmed().isEmpty())
				document_->RemoveKnown(state->section.toStdString(),
					key.toStdString().c_str());
			else
            {
                QString stored = text.trimmed().compare(
                    QStringLiteral("Auto"), Qt::CaseInsensitive) == 0 ?
                    QStringLiteral("AUTO") : text;
                if (displayScale != 1.0)
                {
                    bool ok = false;
                    const double displayed = text.trimmed().toDouble(&ok);
                    if (ok)
                        stored = QString::number(displayed / displayScale,
                            'g', 12);
                }
                document_->SetKnown(state->section.toStdString(), key.toStdString().c_str(), stored.toLocal8Bit().constData());
            }
            markDirty();
        });
        return edit;
    };
    auto addBoolean = [&](const QString& label, const QString& key)
    {
        auto* check = new QCheckBox;
        check->setObjectName(controlName(sectionPrefix, key));
        check->setAccessibleName(label);
        check->setTristate(false);
        form->addRow(label, check);
        fields->push_back({ key, check, Field::Boolean });
        connect(check, &QCheckBox::toggled, this,
            [this, state, key, deprecatedViewportAlias, check](bool checked)
        {
            if (state->loading || state->section.isEmpty() || !document_) return;
            if (check->property("inherited").toBool())
            {
                check->setProperty("inherited", false);
                check->style()->unpolish(check);
                check->style()->polish(check);
            }
            const QString alias = deprecatedViewportAlias(key);
            if (!alias.isEmpty())
                document_->RemoveKnown(state->section.toStdString(), alias.toStdString().c_str());
            document_->SetKnown(state->section.toStdString(), key.toStdString().c_str(), checked ? "true" : "false");
            markDirty();
        });
		return check;
    };
    auto addChoice = [&](const QString& label, const QString& key,
        const QStringList& choices, bool allowInherited = true)
    {
        auto* combo = new QComboBox;
        combo->setObjectName(controlName(sectionPrefix, key));
        combo->setAccessibleName(label);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (allowInherited)
            combo->addItem(QStringLiteral("Inherited / not set"), QString());
        for (const QString& choice : choices)
            combo->addItem(friendlyChoiceLabel(choice), choice);
        form->addRow(label, combo);
        fields->push_back({ key, combo, Field::Choice });
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, state, key, combo, sectionPrefix](int index)
        {
            if (state->loading || state->section.isEmpty() || !document_ || index < 0) return;
            const auto previousTransport = outputTransportSelections();
            const QString selected = combo->itemData(index).toString();
            const bool inherited = selected.isEmpty() &&
                state->section != sectionPrefix;
            if (combo->property("inherited").toBool() != inherited)
            {
                combo->setProperty("inherited", inherited);
                combo->style()->unpolish(combo);
                combo->style()->polish(combo);
            }
            const bool canonicalDebanding =
                sectionPrefix == QStringLiteral("vprenderer") &&
                key == QStringLiteral("deband_strength");
            // deband_strength is the single canonical control. Preserve
            // reading the legacy deband toggle, but never write both keys
            // from the editor because they have overlapping semantics.
            if (canonicalDebanding)
                document_->RemoveKnown(state->section.toStdString(), "deband");
            if (selected.isEmpty()) document_->RemoveKnown(state->section.toStdString(), key.toStdString().c_str());
            else
            {
                document_->SetKnown(state->section.toStdString(), key.toStdString().c_str(), selected.toLocal8Bit().constData());
            }
            if (sectionPrefix == QStringLiteral("vprenderer.color") &&
                (key == QStringLiteral("output_range") || key == QStringLiteral("output_transport_gamma")))
                synchronizeLimitedTransportFlags(previousTransport, state->section);
            markDirty();
            refreshLimitedTransportControls();
        });
        if (sectionPrefix == QStringLiteral("vprenderer.color") &&
            (key == QStringLiteral("output_range") || key == QStringLiteral("output_transport_gamma")))
            connect(combo, qOverload<int>(&QComboBox::activated), this, [this, state](int)
            {
                if (state->loading || state->section.isEmpty()) return;
                synchronizeLimitedTransportFlags(outputTransportSelections(), state->section);
                refreshLimitedTransportControls();
                markDirty();
            });
        return combo;
    };
    const auto addRendererAutoStatus = [this, &form, sectionPrefix](
        const QString& key, QWidget* control)
    {
        if ((sectionPrefix != QStringLiteral("vprenderer") &&
             sectionPrefix != QStringLiteral("vprenderer.scaling") &&
             sectionPrefix != QStringLiteral("vprenderer.color")) ||
            !form || !control)
            return;
        auto* status = helpLabel(QString());
        status->setObjectName(controlName(sectionPrefix, key) +
            QStringLiteral(".auto_status"));
        QFont statusFont = status->font();
        statusFont.setItalic(true);
        status->setFont(statusFont);
        status->setVisible(false);
        int row = -1;
        QFormLayout::ItemRole role = QFormLayout::SpanningRole;
        QWidget* fieldControl = control;
        while (fieldControl && row < 0)
        {
            form->getWidgetPosition(fieldControl, &row, &role);
            if (row < 0)
                fieldControl = fieldControl->parentWidget();
        }
        if (row < 0 || role != QFormLayout::FieldRole)
        {
            delete status;
            return;
        }
        auto* field = new QWidget;
        auto* fieldLayout = new QVBoxLayout(field);
        fieldLayout->setContentsMargins(0, 0, 0, 0);
        fieldLayout->setSpacing(2);
        form->removeWidget(fieldControl);
        fieldLayout->addWidget(fieldControl);
        fieldLayout->addWidget(status);
        form->setWidget(row, QFormLayout::FieldRole, field);
        rendererAutoStatusBindings_.push_back(
            { sectionPrefix, key, control, status });
        if (auto* combo = qobject_cast<QComboBox*>(control))
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) { refreshRendererAutoStatus(); });
        else if (auto* edit = qobject_cast<QLineEdit*>(control))
            connect(edit, &QLineEdit::textChanged, this,
                [this](const QString&) { refreshRendererAutoStatus(); });
    };
    auto addInteger = [&](const QString& label, const QString& key,
        int minimum, int maximum, const QString& unit)
    {
        auto* spin = new QSpinBox;
        spin->setObjectName(controlName(sectionPrefix, key));
        spin->setAccessibleName(label);
        if (!unit.isEmpty()) spin->setAccessibleDescription(
            QStringLiteral("Value in %1.").arg(unit));
        spin->setRange(minimum, maximum);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setAlignment(Qt::AlignRight);
        spin->setFixedWidth(fixedUnitFieldWidth);
        if (key == QStringLiteral("reset_queue_too_large_percent"))
            spin->setToolTip(QStringLiteral(
                "Allowed range: 1-200%. Observed depth can exceed 100% because "
                "the complete queue path includes work outside nominal capacity."));
        QWidget* control = spin;
        if (!unit.isEmpty())
        {
            auto* row = new QWidget;
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);
            rowLayout->addWidget(spin);
            auto* unitLabel = new QLabel(unit);
            unitLabel->setObjectName(controlName(sectionPrefix, key) +
                QStringLiteral(".unit"));
            rowLayout->addWidget(unitLabel);
            rowLayout->addStretch();
            control = row;
        }
        form->addRow(label, control);
        fields->push_back({ key, spin, Field::Integer });
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, state, key](int selected)
        {
            if (state->loading || state->section.isEmpty() || !document_) return;
            document_->SetKnown(state->section.toStdString(),
                key.toStdString().c_str(), std::to_string(selected));
            markDirty();
        });
    };

    QComboBox* queuePolicy = nullptr;
    QLabel* queuePolicySummary = nullptr;
    if (sectionPrefix == QStringLiteral("queue"))
    {
        queuePolicy = new QComboBox;
        queuePolicy->setObjectName(QStringLiteral("config.queue.policy_preset"));
        queuePolicy->setAccessibleName(QStringLiteral("Queue buffering policy"));
        queuePolicy->addItem(QStringLiteral("Custom"), QString());
        for (const QueuePolicy& policy : queuePolicies)
            queuePolicy->addItem(QString::fromLatin1(policy.label),
                QString::fromLatin1(policy.id));
        form->addRow(QStringLiteral("Buffering policy"), queuePolicy);
        queuePolicySummary = helpLabel(QStringLiteral(
            "Choose a policy for a complete queue baseline."));
        queuePolicySummary->setObjectName(QStringLiteral("config.queue.policy_summary"));
        form->addRow(QString(), queuePolicySummary);
        // Stage 5: the queue target as time. VP turns it into Target frames
        // for each source rate, so the delay - and the audio offset - stays
        // about the same at 24p, 30p and 60p.
        addInteger(QStringLiteral("Target delay"), QStringLiteral("target_latency_ms"),
            0, QueueLatency::MaxTargetLatencyMs, QStringLiteral("ms"));
        auto* latencySpin = qobject_cast<QSpinBox*>(fields->back().widget);
        latencySpin->setSpecialValueText(QStringLiteral("Off"));
        latencySpin->setToolTip(QStringLiteral(
            "Off uses Target frames. A delay in ms becomes whole source frames "
            "for each frame rate; a longer delay gives the LED drive and the crop "
            "detector more frames to look ahead."));
        auto* latencyNote = helpLabel(QString());
        latencyNote->setObjectName(QStringLiteral("config.queue.target_latency_note"));
        form->addRow(QString(), latencyNote);
        const auto updateLatencyNote = [latencyNote](int milliseconds)
        {
            latencyNote->setText(queueLatencyNoteText(milliseconds));
        };
        connect(latencySpin, qOverload<int>(&QSpinBox::valueChanged), this, updateLatencyNote);
        updateLatencyNote(0);
        form = addCollapsibleSection(QStringLiteral("queueAdvanced"),
            QStringLiteral("Advanced"), QStringLiteral(
                "Fine-tune individual queue values. Editing one switches this profile to Custom."), false);
        addInteger(QStringLiteral("Queue depth"), QStringLiteral("queue_size"),
            1, INT_MAX, QStringLiteral("frames"));
        addInteger(QStringLiteral("Lead frames"), QStringLiteral("lead_frames"),
            0, 16, QStringLiteral("frames"));
        addInteger(QStringLiteral("Startup pre-roll"), QStringLiteral("startup_preroll_frames"),
            0, 16, QStringLiteral("frames"));
        addInteger(QStringLiteral("Target frames"), QStringLiteral("target_frames"),
            0, 16, QStringLiteral("frames"));
        addInteger(QStringLiteral("Active-picture lookahead"), QStringLiteral("active_picture_lookahead_frames"),
            0, 8, QStringLiteral("frames"));
        addInteger(QStringLiteral("Queue reset delay"), QStringLiteral("reset_after_render_restart_seconds"),
            1, INT_MAX, QStringLiteral("seconds"));
        addInteger(QStringLiteral("Queue recovery threshold"), QStringLiteral("reset_queue_too_large_percent"),
            1, 200, QStringLiteral("%"));
    }
    else if (sectionPrefix == QStringLiteral("vprenderer.color"))
    {
        form = addCollapsibleSection(QStringLiteral("calibration"),
            QStringLiteral("Display calibration"), QStringLiteral(
                "Configure the display response or a calibration LUT. Target white and black remain in Rendering."), true);
        auto* calibrationForm = form;
        auto* calibrationLutEnabled = addBoolean(
            QStringLiteral("Enable display calibration 3D LUT"),
            QStringLiteral("calibration_lut_enabled"));
        form->setRowVisible(calibrationLutEnabled, false);
        auto* methodRow = new QWidget;
        auto* methodLayout = new QHBoxLayout(methodRow);
        methodLayout->setContentsMargins(0, 0, 0, 0);
        auto* vpMethod = new QRadioButton(QStringLiteral("VP"));
        auto* lutMethod = new QRadioButton(QStringLiteral("3D LUT"));
        vpMethod->setObjectName(QStringLiteral("config.vprenderer.color.calibration_method.vp"));
        lutMethod->setObjectName(QStringLiteral("config.vprenderer.color.calibration_method.lut"));
        vpMethod->setToolTip(QStringLiteral("VP uses the display gamut and gamma you specify."));
        lutMethod->setToolTip(QStringLiteral("The selected 3D LUT handles display calibration."));
        auto* methodGroup = new QButtonGroup(methodRow);
        methodGroup->addButton(vpMethod, 0);
        methodGroup->addButton(lutMethod, 1);
        methodLayout->addWidget(vpMethod);
        methodLayout->addWidget(lutMethod);
        methodLayout->addStretch();
        form->addRow(QStringLiteral("Calibration method"), methodRow);
        connect(methodGroup, &QButtonGroup::idClicked, this,
            [calibrationLutEnabled](int id) { calibrationLutEnabled->setChecked(id == 1); });
        auto* luminanceLink = new QPushButton(QStringLiteral("Target white / black - Rendering"));
        luminanceLink->setObjectName(QStringLiteral("config.vprenderer.color.luminance_link"));
        luminanceLink->setMaximumWidth(280);
        luminanceLink->setToolTip(QStringLiteral("Edit the current Rendering profile's HDR target luminance. These values are not yet owned by this Color / Output profile."));
        form->addRow(QStringLiteral("Display luminance"), luminanceLink);
        connect(luminanceLink, &QPushButton::clicked, this, [this]
        {
            selectPage(2);
            if (auto* section = findChild<QToolButton*>(QStringLiteral("rendererSection.toneMapping")))
                if (!section->isChecked()) section->click();
            if (auto* white = findChild<QWidget*>(QStringLiteral("config.vprenderer.sdr_target_nits")))
            {
                white->setFocus();
                for (QWidget* parent = white->parentWidget(); parent; parent = parent->parentWidget())
                    if (auto* scroll = qobject_cast<QScrollArea*>(parent)) { scroll->ensureWidgetVisible(white); break; }
            }
        });
		auto* targetGamut = addChoice(QStringLiteral("Target gamut"),
			QStringLiteral("target_primaries"),
			{ QStringLiteral("REC709"), QStringLiteral("P3_D65"),
				QStringLiteral("BT2020") }, false);
        form->labelForField(targetGamut)->setObjectName(QStringLiteral("config.vprenderer.color.target_primaries.label"));
        connect(targetGamut, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { refreshCalibrationControls(); });
        auto* outputGamma = addChoice(QStringLiteral("Calibrated display gamma"),
            QStringLiteral("output_gamma"),
            { QStringLiteral("srgb"), QStringLiteral("1.8"),
                QStringLiteral("2.0"), QStringLiteral("2.2"),
                QStringLiteral("2.4"), QStringLiteral("2.6"),
                QStringLiteral("2.8") });
        outputGamma->setToolTip(QStringLiteral(
            "The calibrated physical display response used without a usable calibration LUT. "
            "A usable LUT owns display calibration, so this setting has no effect while it is attached. "
			"This setting does not change Windows' normal Full RGB / sRGB presentation declaration."));
        form->labelForField(outputGamma)->setObjectName(QStringLiteral("config.vprenderer.color.output_gamma.label"));
        addRendererAutoStatus(QStringLiteral("output_gamma"), outputGamma);
        auto* displayGammaRow = outputGamma->parentWidget();
        auto* calibrationStatus = helpLabel(QString());
        calibrationStatus->setObjectName(QStringLiteral("config.vprenderer.color.output_gamma.status"));
        form->addRow(QString(), calibrationStatus);
        sdrGammaEnabled = new QCheckBox;
        sdrGammaEnabled->setObjectName(controlName(sectionPrefix, QStringLiteral("sdr_adjust_gamma")));
        sdrGammaEnabled->setAccessibleName(QStringLiteral("Enable SDR gamma processing"));
        sdrGammaEnabled->setToolTip(QStringLiteral("Without a usable LUT, compensate for calibrated display gamma to produce the desired SDR gamma. With a usable LUT, VP automatically preserves the source encoding through the pre-LUT transfer stage. Range, gamut, scaling and the LUT still apply."));
        auto* gammaRow = new QWidget;
        auto* gammaRowLayout = new QHBoxLayout(gammaRow);
        gammaRowLayout->setContentsMargins(0, 0, 0, 0);
        gammaRowLayout->addWidget(sdrGammaEnabled);
        sdrGammaInherit = new QPushButton(QStringLiteral("Use profile default"));
        sdrGammaInherit->setObjectName(controlName(sectionPrefix, QStringLiteral("sdr_adjust_gamma.inherit")));
        gammaRowLayout->addWidget(sdrGammaInherit);
        gammaRowLayout->addStretch();
        form->addRow(QStringLiteral("Enable SDR gamma processing"), gammaRow);
        form->setRowVisible(gammaRow, false);
        fields->push_back({ QStringLiteral("sdr_adjust_gamma"), sdrGammaEnabled, Field::SdrGamma });
        sdrGammaStatus = helpLabel(QString());
        sdrGammaStatus->setObjectName(controlName(sectionPrefix, QStringLiteral("sdr_adjust_gamma.status")));
        form->addRow(QString(), sdrGammaStatus);
        form->setRowVisible(sdrGammaStatus, false);
        auto* sdrResponse = new QComboBox;
        combinedSdrResponse = sdrResponse;
        sdrResponse->setObjectName(QStringLiteral("config.vprenderer.color.sdr_response"));
        form->addRow(QStringLiteral("Desired SDR gamma"), sdrResponse);
        form->labelForField(sdrResponse)->setObjectName(QStringLiteral("config.vprenderer.color.sdr_response.label"));
        desiredSdrGamma = addChoice(QStringLiteral("Desired SDR gamma"),
            QStringLiteral("sdr_input_transfer"),
            { QStringLiteral("srgb"),
                QStringLiteral("1.8"), QStringLiteral("2.0"), QStringLiteral("2.2"),
                QStringLiteral("2.4"), QStringLiteral("2.6"), QStringLiteral("2.8") });
        if (auto* label = qobject_cast<QLabel*>(form->labelForField(desiredSdrGamma)))
            label->setObjectName(controlName(sectionPrefix, QStringLiteral("sdr_input_transfer.label")));
        auto* sdrReferenceHelp = helpLabel(QString());
        sdrReferenceHelp->setObjectName(controlName(sectionPrefix, QStringLiteral("sdr_input_transfer.help")));
        form->addRow(QString(), sdrReferenceHelp);
        connect(sdrResponse, qOverload<int>(&QComboBox::activated), this,
            [this, state, sdrResponse, sdrGammaEnabled, desiredSdrGamma](int index)
        {
            if (state->loading || state->section.isEmpty() || !document_) return;
            const QString choice = sdrResponse->itemData(index).toString();
            if (choice.isEmpty() || choice == QStringLiteral("__inherit")) return; // Preserve conditional settings; inheritance is handled after profile binding.
            const bool passthrough = choice == QStringLiteral("passthrough");
            const QSignalBlocker modeBlock(sdrGammaEnabled);
            const QSignalBlocker referenceBlock(desiredSdrGamma);
            document_->SetKnown(state->section.toStdString(), "sdr_adjust_gamma", passthrough ? "passthrough" : "on");
            sdrGammaEnabled->setTristate(false);
            sdrGammaEnabled->setChecked(!passthrough);
            sdrGammaEnabled->setProperty("effectiveValue", passthrough ? "passthrough" : "on");
            sdrGammaEnabled->setProperty("inherited", false);
            if (!passthrough)
            {
                document_->SetKnown(state->section.toStdString(), "sdr_input_transfer", choice.toStdString());
                desiredSdrGamma->setCurrentIndex(desiredSdrGamma->findData(choice));
                desiredSdrGamma->setProperty("effectiveValue", choice);
                desiredSdrGamma->setProperty("inherited", false);
            }
            markDirty();
            refreshCalibrationControls();
        });
        connect(sdrGammaEnabled, &QCheckBox::checkStateChanged, this,
            [this, state, sdrGammaEnabled, sdrGammaStatus, desiredSdrGamma, sdrGammaInherit](Qt::CheckState checked)
        {
            if (state->loading || state->section.isEmpty() || !document_ || checked == Qt::PartiallyChecked) return;
            const bool enabled = checked == Qt::Checked;
            // Existing 'off' is transport-based conversion, not an unchecked
            // checkbox. Only an explicit user edit replaces its behavior.
            const QSignalBlocker blocker(sdrGammaEnabled);
            sdrGammaEnabled->setTristate(false);
            sdrGammaEnabled->setProperty("effectiveValue", enabled ? "on" : "passthrough");
            sdrGammaEnabled->setProperty("inherited", false);
            sdrGammaStatus->setText(enabled ? QStringLiteral("Compensate for calibrated display gamma to produce the desired SDR gamma when no usable LUT is attached.") :
                QStringLiteral("No SDR gamma adjustment without a LUT. A usable LUT handles calibration; other processing still applies."));
            desiredSdrGamma->setEnabled(true);
            sdrGammaInherit->setEnabled(true);
            document_->SetKnown(state->section.toStdString(), "sdr_adjust_gamma", enabled ? "on" : "passthrough");
            markDirty();
        });

		calibrationLutEnabled->setToolTip(QStringLiteral(
			"A usable LUT handles display calibration. SDR can retain its reference encoding or convert to the declared SDR LUT input gamma; HDR uses its separate LUT input gamma. "
            "HDR peak analysis and pixel-shader tone mapping remain active."));
        auto* sdrLutGamma = addChoice(QStringLiteral("SDR LUT input gamma"),
            QStringLiteral("sdr_lut_input_gamma"),
            { QStringLiteral("passthrough"), QStringLiteral("srgb"),
              QStringLiteral("1.8"), QStringLiteral("2.0"), QStringLiteral("2.2"),
              QStringLiteral("2.4"), QStringLiteral("2.6"), QStringLiteral("2.8") });
        sdrLutGamma->setItemText(sdrLutGamma->findData(QStringLiteral("passthrough")), QStringLiteral("No gamma conversion"));
        sdrLutGamma->setToolTip(QStringLiteral("SDR only, with a usable LUT. Select the input gamma the LUT was built for. "
            "VP decodes using SDR reference gamma, then encodes into this gamma before the LUT. "
            "No gamma conversion uses the same reference on both sides. This does not change HDR LUT input gamma. "
            "A .cube file does not reliably identify its intended input gamma."));
        auto* hdrTargetGamma = addChoice(QStringLiteral("HDR LUT input gamma"),
            QStringLiteral("hdr_tone_map_target_gamma"),
            { QStringLiteral("srgb"),
              QStringLiteral("1.8"), QStringLiteral("2.0"), QStringLiteral("2.2"),
              QStringLiteral("2.4"), QStringLiteral("2.6"), QStringLiteral("2.8") });
        hdrTargetGamma->setToolTip(QStringLiteral(
            "The SDR encoding produced by HDR tone mapping before a usable calibration LUT. "
            "Prepare the LUT for this input gamma. Default: 2.2. SDR has its own LUT input gamma setting. "
            "Without a usable LUT, calibrated display gamma is used instead."));
        auto* hdrGammaStatus = helpLabel(QString());
        hdrGammaStatus->setObjectName(QStringLiteral("config.vprenderer.color.hdr_tone_map_target_gamma.status"));

        const QString lutDirectoryPath = QFileInfo(configPath_).absoluteDir()
            .filePath(QStringLiteral("luts"));
        const auto discoveredLuts = [this, lutDirectoryPath]()
        {
			if (remoteClient_) return remoteLuts_;
            QStringList result;
            const QDir lutDirectory(lutDirectoryPath);
            const QFileInfoList lutFiles = lutDirectory.entryInfoList(
                { QStringLiteral("*.cube"), QStringLiteral("*.CUBE") },
                QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
            for (const QFileInfo& lutFile : lutFiles)
                result << QStringLiteral("luts/%1").arg(lutFile.fileName());
            return result;
        };
		auto* lutBt709 = addChoice(QStringLiteral("LUT file"),
			QStringLiteral("calibration_lut_bt709"), QStringList{ QStringLiteral("none") } + discoveredLuts());
		auto* lutP3 = addChoice(QStringLiteral("LUT file"),
			QStringLiteral("calibration_lut_p3_d65"), QStringList{ QStringLiteral("none") } + discoveredLuts());
		auto* lutBt2020 = addChoice(QStringLiteral("LUT file"),
			QStringLiteral("calibration_lut_bt2020"), QStringList{ QStringLiteral("none") } + discoveredLuts());
        auto* lutAttachmentStatus = helpLabel(QString());
        lutAttachmentStatus->setObjectName(QStringLiteral("config.vprenderer.color.lut_attachment_status"));
        form->addRow(QString(), lutAttachmentStatus);
        form->addRow(QString(), hdrGammaStatus);
        auto* fallbackButton = new QPushButton(QStringLiteral("Edit fallback settings"));
        fallbackButton->setObjectName(QStringLiteral("config.vprenderer.color.edit_fallback"));
        fallbackButton->setCheckable(true);
        fallbackButton->setMaximumWidth(190);
        form->addRow(QString(), fallbackButton);
        connect(fallbackButton, &QPushButton::toggled, this, [this](bool) { refreshCalibrationControls(); });
		const QList<QComboBox*> externalLutSelectors = {
			lutBt709, lutP3, lutBt2020
		};
		lutBt709->setProperty("lutConfigKey", QStringLiteral("calibration_lut_bt709"));
		lutP3->setProperty("lutConfigKey", QStringLiteral("calibration_lut_p3_d65"));
		lutBt2020->setProperty("lutConfigKey", QStringLiteral("calibration_lut_bt2020"));
		for (QComboBox* selector : externalLutSelectors)
			selector->setToolTip(QStringLiteral(
				"The slot follows Target gamut for both SDR and HDR, never source gamut. "
                "The Cube receives full-domain RGB in the selected SDR or HDR LUT input gamma. "
                "Prepare calibration LUTs for that input contract; gamut mapping has already run. "
                "None disables this slot; inheritance uses the default Color / Output profile's slot."));
		const auto refreshLutSelectors = [this, state, externalLutSelectors, discoveredLuts]
        {
            const QStringList available = discoveredLuts();
			const auto availablePath = [&available](const QString& configured)
			{
				const QString normalized = QDir::fromNativeSeparators(configured.trimmed());
                if (normalized.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
                    return QStringLiteral("none");
				for (const QString& candidate : available)
					if (candidate.compare(normalized, Qt::CaseInsensitive) == 0)
						return candidate;
				return QString();
			};
			const auto displayName = [](const QString& path)
			{
                return path.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0 ?
                    QStringLiteral("None") : QFileInfo(path).completeBaseName();
			};
			for (QComboBox* selector : externalLutSelectors)
			{
				const QSignalBlocker blocker(selector);
				const QString selected = selector->currentData().toString();
				const QString effective = selected.isEmpty() ?
					selector->property("effectiveValue").toString() : selected;
				const QString resolvedSelected = availablePath(selected);
				const QString resolvedEffective = availablePath(effective);
				selector->clear();
				QString emptyLabel = QStringLiteral("None");
				if (selected.isEmpty() && !resolvedEffective.isEmpty())
					emptyLabel = QStringLiteral("Inherited: %1").arg(displayName(resolvedEffective));
                const QStringList profiles = profileSections(QStringLiteral("vprenderer.color"));
                const bool defaultProfile = !profiles.isEmpty() && selector->property("profileSection").toString() == profiles.front();
				selector->addItem(emptyLabel, QString());
                selector->addItem(QStringLiteral("None"), QStringLiteral("none"));
                if (auto* view = qobject_cast<QListView*>(selector->view())) view->setRowHidden(0, defaultProfile);
				for (const QString& lut : available)
					selector->addItem(displayName(lut), lut);
                if (selected.isEmpty() && !effective.isEmpty() && resolvedEffective.isEmpty())
                    selector->setItemText(0, QStringLiteral("Inherited - Missing: %1").arg(effective));
                if (!selected.isEmpty() && resolvedSelected.isEmpty())
                    selector->addItem(QStringLiteral("Missing: %1").arg(selected), selected);
                const QString selection = resolvedSelected.isEmpty() ? selected : resolvedSelected;
                selector->setCurrentIndex(std::max(0, selector->findData(selection, Qt::UserRole, Qt::MatchFixedString)));
			}
        };
		refreshLutSelectors();
		auto* lutWatcher = new QFileSystemWatcher(lutBt709);
        const auto watchLutDirectory = [lutWatcher, lutDirectoryPath]
        {
            if (QDir(lutDirectoryPath).exists() &&
                !lutWatcher->directories().contains(lutDirectoryPath))
                lutWatcher->addPath(lutDirectoryPath);
        };
        if (!remoteClient_) watchLutDirectory();
        connect(lutWatcher, &QFileSystemWatcher::directoryChanged, this,
			[refreshLutSelectors](const QString&) { refreshLutSelectors(); });
        auto* openLutFolder = new QPushButton;
		openLutFolder->setObjectName(QStringLiteral("config.vprenderer.color.calibration_lut.open_folder"));
        openLutFolder->setText(QStringLiteral("Open LUT folder"));
        openLutFolder->setToolTip(QStringLiteral("Open the folder where VideoProcessor discovers 3D LUT files."));
        openLutFolder->setAccessibleName(QStringLiteral("Open LUT folder"));
        openLutFolder->setMaximumWidth(170);
		openLutFolder->setEnabled(!remoteClient_);
		if (remoteClient_) openLutFolder->setToolTip(
			QStringLiteral("Manage LUT files on the VP computer."));
        connect(openLutFolder, &QPushButton::clicked, this,
			[this, lutDirectoryPath, watchLutDirectory, refreshLutSelectors]
        {
            if (!QDir().mkpath(lutDirectoryPath))
            {
                QMessageBox::warning(this, QStringLiteral("LUT folder"),
                    QStringLiteral("VideoProcessor could not create the LUT folder."));
                return;
            }
            watchLutDirectory();
			refreshLutSelectors();
            if (!openPathExternally(lutDirectoryPath))
                QMessageBox::warning(this, QStringLiteral("LUT folder"),
                    QStringLiteral("Windows could not open the LUT folder."));
        });
        form->addRow(QString(), openLutFolder);
        auto* lutFolderHelp = helpLabel(QStringLiteral(
            "Put .cube files in the luts folder next to VideoProcessor.cfg (normally the VP installation). "
			"A missing or invalid selection falls back to the normal tone-mapped image. "
			"Target gamut above selects the LUT slot. HDR tone-map target gamma belongs to this calibration profile; Target nits and target black remain in Rendering. "
            "A retained last-known-good LUT remains active if a replacement for the same contract is rejected."));
        form->addRow(QString(), lutFolderHelp);

        for (QWidget* control : QList<QWidget*>{ sdrLutGamma, hdrTargetGamma, hdrGammaStatus,
            lutBt709, lutP3, lutBt2020, openLutFolder, lutFolderHelp, lutAttachmentStatus, fallbackButton, desiredSdrGamma })
        {
            control->setProperty("calibrationForm", QVariant::fromValue(static_cast<QObject*>(calibrationForm)));
            control->setProperty("lutOnlyRow", true);
        }

        for (QWidget* control : QList<QWidget*>{ displayGammaRow, calibrationStatus, sdrResponse })
        {
            control->setProperty("calibrationForm", QVariant::fromValue(static_cast<QObject*>(calibrationForm)));
            control->setProperty("displayOnlyRow", true);
        }
        const int calibrationLabelWidth = outputGamma->fontMetrics().horizontalAdvance(
            QStringLiteral("Fallback desired SDR gamma"));
        for (int row = 0; row < calibrationForm->rowCount(); ++row)
            if (auto* item = calibrationForm->itemAt(row, QFormLayout::LabelRole))
                if (auto* label = item->widget()) label->setFixedWidth(calibrationLabelWidth + 24);
        // Detailed explanations belong in tooltips; keep the working form compact.
        form->setRowVisible(sdrReferenceHelp, false);
        form->setRowVisible(lutFolderHelp, false);
        lutFolderHelp->setProperty("lutOnlyRow", false);
        openLutFolder->setToolTip(lutFolderHelp->text());

		const QList<QWidget*> calibrationLutControls = {
			lutBt709, lutP3, lutBt2020, openLutFolder
		};
		const auto updateCalibrationLutControls = [calibrationLutEnabled,
			calibrationLutControls]
		{
			for (QWidget* control : calibrationLutControls)
				control->setEnabled(calibrationLutEnabled->isChecked());
		};
		connect(calibrationLutEnabled, &QCheckBox::toggled, this,
			[this, updateCalibrationLutControls](bool)
			{
				updateCalibrationLutControls();
				refreshRendererAutoStatus();
			});
		updateCalibrationLutControls();
		// Generic field loading occurs after page construction; reapply gating on
		// the first event turn so disabled profiles are correct on first open.
		QTimer::singleShot(0, calibrationLutEnabled,
			updateCalibrationLutControls);

        form = addCollapsibleSection(QStringLiteral("advancedOutput"),
            QStringLiteral("Output"), QStringLiteral(
                "Presentation preference and RGB transport for this output path. "
                "Windows determines the final presentation path."), true);
        form->addRow(QString(), helpLabel(QStringLiteral(
            "Full RGB is the default. Limited transport uses 2.2 for the beta; "
            "the live status reports whether the requested contract is active.")));
        auto* outputPresentation = addChoice(QStringLiteral("Presentation preference"),
            QStringLiteral("output_presentation"),
            { QStringLiteral("AUTO"), QStringLiteral("direct"),
                QStringLiteral("composed") });
        outputPresentation->setItemText(outputPresentation->findData(QStringLiteral("AUTO")), QStringLiteral("Prefer flip (allow fallback)"));
        outputPresentation->setItemText(outputPresentation->findData(QStringLiteral("direct")), QStringLiteral("Flip model"));
        outputPresentation->setItemText(outputPresentation->findData(QStringLiteral("composed")), QStringLiteral("BitBlt model"));
        outputPresentation->setToolTip(QStringLiteral(
            "Flip model does not guarantee DirectFlip: Windows may compose it. "
            "BitBlt uses the copy-based presentation path. Fallback may change RGB range; check live status."));
		addRendererAutoStatus(QStringLiteral("output_presentation"), outputPresentation);
        auto* outputRange = addChoice(QStringLiteral("RGB output range"),
            QStringLiteral("output_range"),
            {  QStringLiteral("full"),
                QStringLiteral("limited") });
        outputRange->setToolTip(QStringLiteral(
            "Full RGB is the fresh-profile default. Select Limited only for a known "
            "limited-range display chain or a transport diagnostic."));
		addRendererAutoStatus(QStringLiteral("output_range"), outputRange);
        auto* outputTransportGamma = addChoice(
            QStringLiteral("Limited transport transfer"),
            QStringLiteral("output_transport_gamma"),
            {  QStringLiteral("2.2"),
                QStringLiteral("2.4") });
        outputTransportGamma->setToolTip(QStringLiteral(
            "Only active for Limited RGB. Selecting 2.2 enables its beta transport flag; "
            "2.4 or Full disables it. Display gamma remains independent."));
		addRendererAutoStatus(QStringLiteral("output_transport_gamma"), outputTransportGamma);
        if (auto* label = qobject_cast<QLabel*>(form->labelForField(outputTransportGamma->parentWidget())))
            label->setContentsMargins(16, 0, 0, 0);

        auto* transportGammaNotice = helpLabel(QString());
        transportGammaNotice->setObjectName(QStringLiteral("config.vprenderer.color.transport_gamma_notice"));
        form->addRow(QString(), transportGammaNotice);
        auto* matchTransportGamma = new QPushButton;
        matchTransportGamma->setObjectName(QStringLiteral("config.vprenderer.color.match_transport_gamma"));
        matchTransportGamma->setToolTip(QStringLiteral("Update this profile's Limited transport declaration and derived beta flag. Display calibration and pixel processing stay unchanged. Save with Apply or OK."));
        form->addRow(QString(), matchTransportGamma);
        connect(matchTransportGamma, &QPushButton::clicked, this,
            [outputTransportGamma, matchTransportGamma]()
            {
                const QString gamma = matchTransportGamma->property("matchingGamma").toString();
                const int index = outputTransportGamma->findData(gamma);
                if (!gamma.isEmpty() && index >= 0) outputTransportGamma->setCurrentIndex(index);
            });

        auto* outputCompatibility = helpLabel(QString());
        outputCompatibility->setObjectName(
            QStringLiteral("config.vprenderer.color.advanced_output.compatibility"));
        form->addRow(QString(), outputCompatibility);

        addBoolean(QStringLiteral("Report BT.2020 to display"),
            QStringLiteral("report_bt2020_to_display"))->setToolTip(QStringLiteral(
                "Request BT.2020 signalling to the display. This does not change the rendering gamut."));

        form = addCollapsibleSection(QStringLiteral("outputExperiments"),
            QStringLiteral("Output Experiments (beta)"), QStringLiteral(
                "Implementation diagnostics for repeatable renderer testing. "
                "They do not change display calibration, presentation preference, or RGB range. "
                "Changes are saved with this output profile; Apply performs "
                "a hard capture-and-renderer reinitialization before they take effect."), false);
        form->addRow(QString(), helpLabel(QStringLiteral(
            "Diagnostic presets set only the beta controls below. Apply always "
            "hard-reinitializes capture and renderer state.")));
        auto* outputPathProfile = addChoice(QStringLiteral("Diagnostic preset"),
            QStringLiteral("output_path_profile"),
            { QStringLiteral("legacy"), QStringLiteral("proposed"),
                QStringLiteral("custom") });

        outputPathProfile->setItemText(outputPathProfile->findData(QStringLiteral("legacy")),
            QStringLiteral("Normal diagnostics"));
        outputPathProfile->setItemText(outputPathProfile->findData(QStringLiteral("proposed")),
            QStringLiteral("Output investigation"));
        outputPathProfile->setItemText(outputPathProfile->findData(QStringLiteral("custom")),
            QStringLiteral("Custom diagnostics"));
        auto* limitedFlag = addBoolean(QStringLiteral("Limited 2.2 beta transport (derived)"),
            QStringLiteral("diagnostic_allow_limited_g22"));
        limitedFlag->setProperty("derivedSetting", true);
        limitedFlag->setEnabled(false);
        form->setRowVisible(limitedFlag, false);
        addBoolean(QStringLiteral("Disable D3D11 compute shaders"),
            QStringLiteral("diagnostic_disable_compute"));
        addBoolean(QStringLiteral("Force 8-bit SDR swapchain"),
            QStringLiteral("diagnostic_force_8bit_sdr_swapchain"));
        addBoolean(QStringLiteral("Force VP-owned DXGI presenter (flip only, beta)"),
            QStringLiteral("diagnostic_vp_owned_dxgi_presenter"));
        form->addRow(QString(), helpLabel(QStringLiteral(
            "Experimental only: VP owns the DXGI swapchain and Present call. "
            "It supports flip/direct only; Composed always uses libplacebo's "
            "proven presenter.")));
        addBoolean(QStringLiteral("Capture detailed output diagnostics"),
            QStringLiteral("output_diagnostics"));
        addBoolean(QStringLiteral("Disable shader cache"),
            QStringLiteral("diagnostic_disable_shader_cache"));
        updateOutputCompatibility = [profileFields, outputPresentation,
            outputRange, outputTransportGamma, outputCompatibility]()
        {
            const auto* vpOwned = profileFields->findChild<QCheckBox*>(
                QStringLiteral("config.vprenderer.color.diagnostic_vp_owned_dxgi_presenter"));
            QStringList notices;
            if (effectiveChoiceValue(outputPresentation) == QStringLiteral("composed") &&
                vpOwned && vpOwned->isChecked())
            {
                notices << QStringLiteral(
				"Notice: VP-owned DXGI is Direct-only; Composed uses libplacebo's presenter.");
			}
			const QString gamma = effectiveChoiceValue(outputTransportGamma);
			const bool limited = effectiveChoiceValue(outputRange) == QStringLiteral("limited");
			if (limited)
			{
				const auto* limitedG22 = profileFields->findChild<QCheckBox*>(
					QStringLiteral("config.vprenderer.color.diagnostic_allow_limited_g22"));
				if (gamma == QStringLiteral("2.2") &&
					(!limitedG22 || !limitedG22->isChecked()))
				{
					notices << QStringLiteral(
						"Limited 2.2 requires the beta transport flag. Selecting Limited + 2.2 enables it automatically.");
				}
				else if (gamma != QStringLiteral("auto") &&
					gamma != QStringLiteral("2.4") && gamma != QStringLiteral("2.2"))
				{
					notices << QStringLiteral(
						"Blocked: Limited RGB supports Auto/2.4, plus the 2.2 diagnostic experiment.");
				}
				else
				{
					notices << QStringLiteral(
						"Limited RGB changes output transport only; display calibration remains independent.");
				}
			}
            outputCompatibility->setText(notices.join(QStringLiteral("\n")));
            outputCompatibility->setVisible(!notices.isEmpty());
        };
        connect(outputPresentation, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [updateOutputCompatibility](int) { updateOutputCompatibility(); });
        connect(outputTransportGamma, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [updateOutputCompatibility](int) { updateOutputCompatibility(); });
        connect(outputRange, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [updateOutputCompatibility](int) { updateOutputCompatibility(); });
        if (auto* vpOwned = findChild<QCheckBox*>(
            QStringLiteral("config.vprenderer.color.diagnostic_vp_owned_dxgi_presenter")))
        {
            connect(vpOwned, &QCheckBox::toggled, this,
                [updateOutputCompatibility](bool) { updateOutputCompatibility(); });
        }
		if (auto* limitedG22 = findChild<QCheckBox*>(
			QStringLiteral("config.vprenderer.color.diagnostic_allow_limited_g22")))
		{
			connect(limitedG22, &QCheckBox::toggled, this,
				[updateOutputCompatibility](bool) { updateOutputCompatibility(); });
        }
        updateOutputCompatibility();
        const auto applyOutputPathProfile = [this, state, fields,
            outputPathProfile, updateOutputCompatibility](const QString& profile)
        {
            if (state->loading || state->section.isEmpty() || !document_ ||
                profile.isEmpty() || profile == QStringLiteral("custom")) return;
            struct Value { const char* key; const char* value; };
		static constexpr Value legacy[] = {
				{ "diagnostic_disable_compute", "false" },
                { "diagnostic_force_8bit_sdr_swapchain", "false" },
                { "diagnostic_vp_owned_dxgi_presenter", "false" },
                { "output_diagnostics", "false" },
                { "diagnostic_disable_shader_cache", "false" }
            };
		static constexpr Value proposed[] = {
                { "diagnostic_disable_compute", "false" },
                { "diagnostic_force_8bit_sdr_swapchain", "false" },
                { "diagnostic_vp_owned_dxgi_presenter", "false" },
                { "output_diagnostics", "true" },
                { "diagnostic_disable_shader_cache", "false" }
            };
            const Value* values = profile == QStringLiteral("proposed") ?
                proposed : legacy;
            const size_t count = profile == QStringLiteral("proposed") ?
                std::size(proposed) : std::size(legacy);
            const std::string section = state->section.toStdString();
            document_->SetKnown(section, "output_path_profile",
                profile.toLocal8Bit().constData());
            for (size_t index = 0; index < count; ++index)
            {
                document_->SetKnown(section, values[index].key, values[index].value);
                for (const Field& field : *fields)
                    if (field.key == QString::fromLatin1(values[index].key))
                    {
                        const QSignalBlocker blocker(field.widget);
                        if (field.kind == Field::Boolean)
                            qobject_cast<QCheckBox*>(field.widget)->setChecked(
                                QString::fromLatin1(values[index].value) == QStringLiteral("true"));
                        else if (field.kind == Field::Choice)
                            qobject_cast<QComboBox*>(field.widget)->setCurrentIndex(
                                qobject_cast<QComboBox*>(field.widget)->findData(
                                    QString::fromLatin1(values[index].value)));
                        break;
                    }
            }
            const QSignalBlocker profileBlocker(outputPathProfile);
            outputPathProfile->setCurrentIndex(outputPathProfile->findData(profile));
            updateOutputCompatibility();
            refreshLimitedTransportControls();
            markDirty();
        };
        connect(outputPathProfile, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [outputPathProfile, applyOutputPathProfile](int)
        { applyOutputPathProfile(outputPathProfile->currentData().toString()); });
        const auto markOutputPathCustom = [this, state, outputPathProfile]
        {
            if (state->loading || state->section.isEmpty() || !document_) return;
            const QString preset = diagnosticPresetForControls(this);
            const QSignalBlocker blocker(outputPathProfile);
            outputPathProfile->setCurrentIndex(outputPathProfile->findData(preset));
            document_->SetKnown(state->section.toStdString(), "output_path_profile", preset.toStdString());
            markDirty();
        };
        for (const Field& field : *fields)
			if (field.key.startsWith(QStringLiteral("diagnostic_")) ||
                field.key == QStringLiteral("output_diagnostics"))
            {
                if (field.kind == Field::Boolean)
                    connect(qobject_cast<QCheckBox*>(field.widget), &QCheckBox::toggled,
                        this, [markOutputPathCustom](bool) { markOutputPathCustom(); });
                else if (field.kind == Field::Choice && field.widget != outputPathProfile)
                    connect(qobject_cast<QComboBox*>(field.widget),
                        qOverload<int>(&QComboBox::currentIndexChanged), this,
                        [markOutputPathCustom](int) { markOutputPathCustom(); });
            }
        auto* resetOutputExperiments = new QPushButton(
            QStringLiteral("Restore Normal Diagnostics"));
        resetOutputExperiments->setObjectName(
            QStringLiteral("config.vprenderer.color.output_experiments.reset_defaults"));
        resetOutputExperiments->setToolTip(QStringLiteral(
            "Restore this profile's normal diagnostic settings."));
        resetOutputExperiments->setAccessibleName(
            QStringLiteral("Restore normal diagnostic settings"));
        connect(resetOutputExperiments, &QPushButton::clicked, this,
            [this, state, applyOutputPathProfile]
        {
            if (state->section.isEmpty() || !document_) return;
            if (QMessageBox::question(this,
                QStringLiteral("Restore normal diagnostics"),
				QStringLiteral("Restore normal diagnostic settings for this output "
				"profile? This leaves display calibration, presentation preference, "
                    "and RGB range unchanged."),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes) return;
            applyOutputPathProfile(QStringLiteral("legacy"));
        });
        form->addRow(QString(), resetOutputExperiments);
    }
    else if (sectionPrefix == QStringLiteral("vprenderer"))
    {
		form = addPlainForm();
		auto* renderingQuality = addChoice(QStringLiteral("Rendering quality"),
			QStringLiteral("quality"), { QStringLiteral("high"),
			QStringLiteral("balanced"), QStringLiteral("fast") });
		connect(renderingQuality, qOverload<int>(&QComboBox::currentIndexChanged), this,
			[this](int) { refreshRendererAutoStatus(); });

        form = addCollapsibleSection(QStringLiteral("toneMapping"),
            QStringLiteral("Tone Mapping: General"), QStringLiteral(
                "HDR tone mapping, gamut compression, dynamic peak handling, and contrast recovery."), false);
        auto* sdrTargetWhiteLevel = addText(QStringLiteral("Target nits"),
            QStringLiteral("sdr_target_nits"), QStringLiteral("nits"));
        sdrTargetWhiteLevel->setToolTip(QStringLiteral(
            "HDR-to-SDR tone-mapping destination: above 0.000001 through 10000 nits, and above target black. "
            "Does not change SDR input brightness or processing. "
            "Invalid values are rejected when saving; the saved value is retained."));
        sdrTargetWhiteLevel->setValidator(new QDoubleValidator(HdrTargetLuminance::BlackFloor, HdrTargetLuminance::Maximum, -1, sdrTargetWhiteLevel));
        connect(sdrTargetWhiteLevel, &QLineEdit::textChanged, this,
            [this](const QString&) { refreshRendererAutoStatus(); });
        auto* sdrBlackLevel = addText(QStringLiteral("HDR tone-map target black"),
            QStringLiteral("sdr_black_nits"), QStringLiteral("nits"));
        sdrBlackLevel->setToolTip(QStringLiteral(
            "Black level for HDR-to-SDR tone mapping only. Omitted values and legacy Auto default to 0 (0.000001 nit internally); positive values are used as entered. SDR input is unaffected."));
        sdrBlackLevel->setPlaceholderText(QStringLiteral("0 or a measured black level"));
        sdrBlackLevel->setValidator(new QDoubleValidator(0.0, HdrTargetLuminance::Maximum, -1, sdrBlackLevel));
        addRendererAutoStatus(QStringLiteral("sdr_black_nits"), sdrBlackLevel);
        auto* toneMapping = addChoice(QStringLiteral("Tone mapping"), QStringLiteral("tone_mapping"), { QStringLiteral("AUTO"), QStringLiteral("spline"), QStringLiteral("bt2390"), QStringLiteral("st2094-40"), QStringLiteral("reinhard") });
        addRendererAutoStatus(QStringLiteral("tone_mapping"), toneMapping);
        auto* gamutMapping = addChoice(QStringLiteral("Gamut mapping"), QStringLiteral("gamut_mapping"), { QStringLiteral("AUTO"), QStringLiteral("perceptual"), QStringLiteral("softclip"), QStringLiteral("relative"), QStringLiteral("desaturate") });
        addRendererAutoStatus(QStringLiteral("gamut_mapping"), gamutMapping);
        auto* peakDetection = addChoice(QStringLiteral("Peak detection"), QStringLiteral("peak_detection"), { QStringLiteral("AUTO"), QStringLiteral("high_quality"), QStringLiteral("on"), QStringLiteral("off") });
        peakDetection->setItemText(peakDetection->findData(QStringLiteral("on")),
            QStringLiteral("Standard"));
        addRendererAutoStatus(QStringLiteral("peak_detection"), peakDetection);
        auto* contrastRecovery = addText(QStringLiteral("Contrast recovery (0 to 2)"), QStringLiteral("contrast_recovery"));
        contrastRecovery->setPlaceholderText(QStringLiteral("Auto or a value from 0 to 2"));
        addRendererAutoStatus(QStringLiteral("contrast_recovery"), contrastRecovery);
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
        auto* dejudderPreset = new QComboBox;
        dejudderPreset->setObjectName(controlName(sectionPrefix, QStringLiteral("dejudder_preset")));
        dejudderPreset->setAccessibleName(QStringLiteral("De-judder preset"));
        dejudderPreset->addItem(QStringLiteral("Not set (the default profile's, or off)"), QString());
        dejudderPreset->addItem(QStringLiteral("Off"), QStringLiteral("0"));
        dejudderPreset->addItem(QStringLiteral("Gentle"), QStringLiteral("0.25"));
        dejudderPreset->addItem(QStringLiteral("Medium"), QStringLiteral("0.35"));
        dejudderPreset->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
        dejudderPreset->setToolTip(QStringLiteral(
            "Smooths film motion when the display's refreshes do not divide evenly into film frames "
            "(e.g. 24p on 60 Hz), by filling the repeated refreshes with slightly moved pictures. "
            "Gentle (0.25) is the recommended start; Medium is 0.35; Custom takes the strength typed below. "
            "Film-like sources only (up to about 30 fps, and 48p); off for 50p/60p video, in the low-latency "
            "queue and for RGB or interlaced input."));
        form->addRow(QStringLiteral("De-judder"), dejudderPreset);
        auto* dejudderStrength = addText(QStringLiteral("Strength (uneven cadences, e.g. 24p on 60 Hz)"),
            QStringLiteral("dejudder_strength"));
        dejudderStrength->setValidator(new QDoubleValidator(0.0, 1.0, 3, dejudderStrength));
        dejudderStrength->setPlaceholderText(QStringLiteral("0 = off; Gentle 0.25, Medium 0.35"));
        // The preset writes the strength; the strength shows which preset it is.
        connect(dejudderPreset, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [dejudderPreset, dejudderStrength](int index)
        {
            const QString value = dejudderPreset->itemData(index).toString();
            if (value == QStringLiteral("custom"))
            {
                dejudderStrength->setFocus();
                return;
            }
            if (dejudderStrength->text() != value)
                dejudderStrength->setText(value);
        });
        connect(dejudderStrength, &QLineEdit::textChanged, this,
            [dejudderPreset](const QString& text)
        {
            QString trimmed = text.trimmed();
            if (trimmed.compare(QStringLiteral("Auto"), Qt::CaseInsensitive) == 0)
                trimmed.clear();   // the default profile shows an unset field as Auto
            int index = trimmed.isEmpty() ? 0 : dejudderPreset->findData(trimmed);
            if (index < 0)
            {
                bool ok = false;
                const double value = trimmed.toDouble(&ok);
                index = trimmed.isEmpty() ? 0 :
                    ok && value == 0.0 ? 1 : ok && value == 0.25 ? 2 : ok && value == 0.35 ? 3 : 4;
            }
            const QSignalBlocker blocker(dejudderPreset);
            dejudderPreset->setCurrentIndex(index);
        });
        // Delta 10: even cadences (a whole number of refreshes per film frame).
        auto* strengthEven = addText(QStringLiteral("Strength (even cadences, e.g. 30p on 60 Hz)"),
            QStringLiteral("dejudder_strength_even"));
        strengthEven->setValidator(new QDoubleValidator(0.0, 1.0, 3, strengthEven));
        strengthEven->setPlaceholderText(QStringLiteral("0 = off"));
        strengthEven->setToolTip(QStringLiteral(
            "When each film frame is shown for a whole number of refreshes (e.g. 30p on 60 Hz, 25p on 50 Hz), "
            "the strobing on slow pans is what shows: the repeats become evenly spaced in-between pictures. "
            "1.0 is full frame-rate conversion. Held regions keep the source rate; landscapes do well, fast "
            "action holds more. 0 = off (uneven cadences use the strength above)."));
        auto* evenRates = addChoice(QStringLiteral("Even strength applies to"),
            QStringLiteral("dejudder_even_rates"), { QStringLiteral("whole"), QStringLiteral("whole_and_1001") });
        evenRates->setItemText(evenRates->findData(QStringLiteral("whole")),
            QStringLiteral("Whole-number rates (24, 25, 30)"));
        evenRates->setItemText(evenRates->findData(QStringLiteral("whole_and_1001")),
            QStringLiteral("Also 1000/1001 rates (23.976, 29.97)"));
        evenRates->setToolTip(QStringLiteral(
            "1000/1001 rates mostly come from broadcast television, often video-look already, where "
            "smoothing is purely taste."));
        MarkGpuCost(form, strengthEven, 2);
        // Pulldown detection (owner 2026-10-14).
        auto* pulldown = addChoice(QStringLiteral("Find film sent with repeated frames"),
            QStringLiteral("dejudder_pulldown"), { QStringLiteral("off"), QStringLiteral("on") });
        pulldown->setToolTip(QStringLiteral(
            "Some sources send film inside a faster signal by repeating frames (e.g. 24p as 2:3 in 60 Hz). "
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
            "2160-line picture; scaled for other sizes."));
        auto* rollHi = addText(QStringLiteral("Floor reached at (px per frame)"), QStringLiteral("dejudder_roll_hi"));
        rollHi->setValidator(new QDoubleValidator(0.0, 400.0, 1, rollHi));
        rollHi->setPlaceholderText(QStringLiteral("24"));
        rollHi->setToolTip(QStringLiteral(
            "The camera speed where the strength reaches the floor (24 is about a whip pan). In pixels of a "
            "2160-line picture; scaled for other sizes."));
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
        auto* fadeMatch = addChoice(QStringLiteral("Follow motion through fades"),
            QStringLiteral("dejudder_fade_match"), { QStringLiteral("on"), QStringLiteral("off") });
        fadeMatch->setToolTip(QStringLiteral(
            "During a smooth fade (the whole picture brightening or darkening steadily) motion is matched despite the "
            "brightness change, and the de-judder runs at the fade floor below. A flash or a cut still holds."));
        auto* fadeFloor = addText(QStringLiteral("Fade floor (share of the strength)"), QStringLiteral("dejudder_fade_floor"));
        fadeFloor->setValidator(new QDoubleValidator(0.0, 1.0, 2, fadeFloor));
        fadeFloor->setPlaceholderText(QStringLiteral("0.25"));
        fadeFloor->setToolTip(QStringLiteral("The most de-judder a fading scene gets (0.25 = a quarter)."));
        MarkGpuCost(form, dejudderPreset, 2);

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
            "judder to fix. In pixels of a 2160-line picture; scaled for other sizes."));
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
        auto* pictureArea = addChoice(QStringLiteral("Ignore black bars when matching"),
            QStringLiteral("dejudder_picture_area"), { QStringLiteral("on"), QStringLiteral("off") });
        pictureArea->setToolTip(QStringLiteral(
            "De-judder: letterbox and pillarbox bars are left out of the motion statistics (cut detection, fades) "
            "and stay still. Off = the bars count as picture (cuts in letterboxed film can be missed)."));
        auto* sceneCooldown = addChoice(QStringLiteral("Scene-cut cooldown 2 s"),
            QStringLiteral("fix_scene_cooldown"), { QStringLiteral("on"), QStringLiteral("off") });
        sceneCooldown->setToolTip(QStringLiteral(
            "After a cut, the next cut that may change the crop or NLS counts again after 2 seconds, as intended. "
            "Off = the old behaviour: 12.5 s on 24p film, 5 s at 60p."));
        auto* ditherNext = addChoice(QStringLiteral("Dither reuses de-judder motion on new frames"),
            QStringLiteral("dither_vectors_new_frame"), { QStringLiteral("on"), QStringLiteral("off") });
        ditherNext->setToolTip(QStringLiteral(
            "With the de-judder running, each new film frame's temporal dither takes the motion the de-judder already "
            "measured instead of searching again (about 5 ms less per new frame on the laptop). Off = its own search."));
        auto* capturePath = addChoice(QStringLiteral("Capture unpack on the GPU (v210 / UYVY)"), QStringLiteral("capture_path"),
            { QStringLiteral("gpu"), QStringLiteral("standard") });
        capturePath->setItemText(capturePath->findData(QStringLiteral("gpu")), QStringLiteral("GPU (default)"));
        capturePath->setItemText(capturePath->findData(QStringLiteral("standard")), QStringLiteral("CPU (the old path)"));
        capturePath->setToolTip(QStringLiteral(
            "DeckLink v210 / UYVY frames are uploaded as they arrive and unpacked on the GPU: the same picture byte for "
            "byte, about half the time at 4K and no CPU conversion. CPU = the old conversion. RGB formats follow the "
            "native RGB capture choice below."));
        auto* captureUpload = addChoice(QStringLiteral("Upload frames as they arrive (experimental)"), QStringLiteral("capture_upload"),
            { QStringLiteral("render"), QStringLiteral("early") });
        captureUpload->setItemText(captureUpload->findData(QStringLiteral("render")), QStringLiteral("Off (the render thread uploads)"));
        captureUpload->setItemText(captureUpload->findData(QStringLiteral("early")), QStringLiteral("On (early, second device)"));
        captureUpload->setToolTip(QStringLiteral(
            "Experimental, off by default. Each captured frame is copied to the GPU on its own thread as it arrives, "
            "instead of by the render thread when it is shown: about 5 ms less work for the render thread per 4K 12-bit "
            "frame, and the copy overlaps the previous frame's rendering. A frame with no free upload slot simply takes "
            "the normal path."));
        auto* nativeRgb = addChoice(QStringLiteral("Native 10/12-bit RGB capture"),
            QStringLiteral("native_rgb_ingest"), { QStringLiteral("on"), QStringLiteral("off") });
        nativeRgb->setToolTip(QStringLiteral(
            "DeckLink 12-bit RGB (R12B/R12L) and 10-bit r210 reach the renderer as full-precision RGB 4:4:4, unpacked "
            "on the GPU. Off = the old CPU conversion to 10-bit 4:2:0 YCbCr."));

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

        form = addCollapsibleSection(QStringLiteral("temporalDither"), QStringLiteral("Temporal dither"),
            QStringLiteral("The temporal sigma-delta shader's use of the de-judder's motion (needs that shader). ") +
            QString::fromLatin1(kGpuCostLegend), false);
        auto* occlusionMask = addChoice(QStringLiteral("Occlusion mask for the dither"),
            QStringLiteral("occlusion_mask"), { QStringLiteral("on"), QStringLiteral("off") });
        occlusionMask->setToolTip(QStringLiteral(
            "Tells the temporal sigma-delta where new or uncovered picture content appears, so it "
            "does not carry its sub-code residual into it. Works with the de-judder off too "
            "(the motion matcher then runs for the mask alone). Needs the sigma-delta shader."));
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
        ditherVectors->setItemText(ditherVectors->findData(QStringLiteral("own")), QStringLiteral("Sigma-delta's own search"));
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
                auto showIn = [](QFormLayout* layout, QWidget* row, bool visible) { layout->setRowVisible(row, visible); };
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
                // Filling uncovered areas works on the source path only.
                const bool sourcePath = warpStage->currentData().toString() == QStringLiteral("source");
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
                    hi->setStyleSheet(wrong ? QStringLiteral("color: #ff8a8a;") : QString());
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
                connect(edit, &QLineEdit::textChanged, this, [update](const QString&) { update(); });
            for (QLineEdit* edit : { dejudderStrength, strengthEven })
                connect(edit, &QLineEdit::textChanged, this, [update](const QString&) { update(); });
            for (QComboBox* combo : { rolloff, fadeMatch, estSplit, estCandidates, globalPrior, regional, pixelTrust,
                    verify, taper, warpStage })
                connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [update](int) { update(); });
            update();
            // Every switch says its default (audit 2026-10-14c).
            const std::pair<QComboBox*, const char*> switchDefaults[] = {
                { rolloff, "Default off." }, { fadeMatch, "Default on." }, { estCandidates, "Default on." },
                { estQpel, "Default on." }, { estGate, "Default on." },
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
            { "SSIM weight (0 to 1)", "adaptive_w_base", 0.0, 1.0, "1", "How much SSIM where nothing argues against it." },
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
            QStringLiteral("adaptive_faithful"), { QStringLiteral("polar"), QStringLiteral("ortho") });
        adaptiveFaithful->setItemText(adaptiveFaithful->findData(QStringLiteral("polar")),
            QStringLiteral("Polar EWA Lanczos sharp (most accurate)"));
        adaptiveFaithful->setItemText(adaptiveFaithful->findData(QStringLiteral("ortho")),
            QStringLiteral("Separable Lanczos (faster)"));
        adaptiveFaithful->setToolTip(QStringLiteral(
            "The filter of the plain, grain-true downscale the blend leans to on grainy areas. Separable "
            "Lanczos keeps the grain's character at about a quarter of the polar filter's GPU time."));
        MarkGpuCost(form, adaptiveFaithful, 2);
        // Stage 8-3: the per-block grain map from the de-judder's vectors.
        auto* adaptiveMap = addChoice(QStringLiteral("Grain map from motion"),
            QStringLiteral("adaptive_grain_map"), { QStringLiteral("on"), QStringLiteral("off") });
        adaptiveMap->setToolTip(QStringLiteral(
            "On: grain is measured per block from frame-to-frame noise along the motion vectors, so grainy "
            "areas lean faithful while clean texture keeps SSIM. Film rates (30 fps or less) with a next frame "
            "queued; runs the motion matcher even with de-judder off. Elsewhere (the low-latency queue, 50/60p) the "
            "per-frame measure is used. Off: the per-frame measure only."));
        auto* adaptiveMapSmooth = addText(QStringLiteral("Grain map smoothing (ms)"),
            QStringLiteral("adaptive_grain_map_smooth_ms"));
        adaptiveMapSmooth->setValidator(new QDoubleValidator(0.0, 10000.0, 3, adaptiveMapSmooth));
        adaptiveMapSmooth->setPlaceholderText(QStringLiteral("500"));
        adaptiveMapSmooth->setToolTip(QStringLiteral(
            "Time constant of the per-block grain map (it follows the content along the vectors); a cut resets it."));
        MarkGpuCost(form, adaptiveOn, 2);
        MarkGpuCost(form, adaptiveMap, 1);
        {
            // Greyed while they do nothing (audit 2026-10-14c).
            adaptiveRows.push_back(adaptiveChroma);
            adaptiveRows.push_back(adaptiveFaithful);
            adaptiveRows.push_back(adaptiveMap);
            auto greyAdaptive = [adaptiveOn, adaptiveMap, adaptiveRows, adaptiveMapSmooth]()
            {
                const bool on = adaptiveOn->currentData().toString() == QStringLiteral("on");
                const QString map = adaptiveMap->currentData().toString();
                for (QWidget* row : adaptiveRows) { row->setProperty("greyedOff", !on); row->setEnabled(on); }
                const bool smooth = on && map != QStringLiteral("off");
                adaptiveMapSmooth->setProperty("greyedOff", !smooth);
                adaptiveMapSmooth->setEnabled(smooth);
            };
            for (QComboBox* combo : { adaptiveOn, adaptiveMap })
                connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [greyAdaptive](int) { greyAdaptive(); });
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
        MarkGpuCost(form, upscaleOn, 2);
        {
            auto greyUpscale = [upscaleOn, upscaleWBase, upscaleSuperres, upscaleLight]()
            {
                const bool on = upscaleOn->currentData().toString() == QStringLiteral("on");
                for (QWidget* row : { static_cast<QWidget*>(upscaleWBase), static_cast<QWidget*>(upscaleSuperres),
                        static_cast<QWidget*>(upscaleLight) })
                {
                    row->setProperty("greyedOff", !on);
                    row->setEnabled(on);
                }
            };
            connect(upscaleOn, qOverload<int>(&QComboBox::currentIndexChanged), this, [greyUpscale](int) { greyUpscale(); });
            greyUpscale();
        }

        auto wrapToneControl = [&](const ToneMappingTuning::Spec& spec, QLineEdit* edit) {
            auto row = form->takeRow(edit);
            if (row.labelItem) { delete row.labelItem->widget(); delete row.labelItem; }
            delete row.fieldItem;
            auto* control = new ToneMappingControl(spec, edit);
            form->addRow(control);
            return control;
        };
        // General retains its existing Auto status; the new controls show their
        // resolved defaults numerically without creating configuration entries.
        std::array<ToneMappingControl*, ToneMappingTuning::Count> toneControls{};
        form = addCollapsibleSection(QStringLiteral("toneHighlightBeta"),
            QStringLiteral("Tone Mapping: Highlight Response (Beta)"),
            QStringLiteral("Available with VP Renderer (Beta). Saved values are retained with other renderers."), false);
        QWidget* betaFields = form->parentWidget();
        auto* customToneMapping = addBoolean(QStringLiteral("Gamut overflow correction (Experimental)"), QStringLiteral("custom_tone_mapping"));
        customToneMapping->setToolTip(QStringLiteral("Off: original tone/gamut mapping. On: smoothly compresses colors near or beyond the target gamut or brightness limits after the selected gamut mapping during HDR tone mapping. May reduce brightness or saturation. Default: Off. Apply to compare."));
        auto* dynamicTarget = addBoolean(QStringLiteral("Dynamic target nits"), QStringLiteral("dynamic_target"));
        dynamicTarget->setToolTip(QStringLiteral("Automatically trades some scene brightness for highlight headroom. Target nits remains the physical display peak. Default: On. Requires scene-average metadata; peak detection supplies it when available."));
        for (int i = ToneMappingTuning::FilmStrength; i < ToneMappingTuning::Count; ++i) {
            const auto& spec = ToneMappingTuning::Specs()[i];
            toneControls[i] = wrapToneControl(spec, addText(QString::fromUtf8(spec.label), QString::fromUtf8(spec.key)));
        }
        auto* restoreBeta = new QPushButton(QStringLiteral("Restore defaults"));
        restoreBeta->setObjectName(QStringLiteral("config.vprenderer.toneHighlightBeta.restore_defaults"));
        form->addRow(restoreBeta);
        connect(restoreBeta, &QPushButton::clicked, betaFields, [toneControls, dynamicTarget, customToneMapping] {
            customToneMapping->setChecked(false);
            dynamicTarget->setChecked(true);
            toneControls[ToneMappingTuning::FilmStrength]->restoreDefault();
            toneControls[ToneMappingTuning::MasteringClip]->restoreDefault();
        });
        const char* ids[] = { "toneSpline", "toneOther", "tonePeak" };
        const char* headings[] = { "Tone Mapping: Spline Curve", "Tone Mapping: Other Curves", "Tone Mapping: Peak Detection" };
        const char* descriptions[] = {
            "Knee controls also apply to ST2094-40. Inactive settings are retained.",
            "Controls apply only to their named tone-mapping curve.",
            "Controls apply while peak detection is enabled. Defaults follow the selected peak-detection mode." };
        const int starts[] = { 0, ToneMappingTuning::KneeOffset, ToneMappingTuning::Percentile };
        const int ends[] = { ToneMappingTuning::KneeOffset, ToneMappingTuning::Percentile, ToneMappingTuning::FilmStrength };
        for (int section = 0; section < 3; ++section) {
            form = addCollapsibleSection(QString::fromUtf8(ids[section]), QString::fromUtf8(headings[section]),
                QString::fromUtf8(descriptions[section]), false);
            for (int i = starts[section]; i < ends[section]; ++i) {
                const auto& spec = ToneMappingTuning::Specs()[i];
                auto* edit = addText(QString::fromUtf8(spec.label), QString::fromUtf8(spec.key));
                toneControls[i] = wrapToneControl(spec, edit);
            }
            auto* restore = new QPushButton(QStringLiteral("Restore defaults"));
            restore->setObjectName(QStringLiteral("config.vprenderer.%1.restore_defaults").arg(QString::fromUtf8(ids[section])));
            restore->setToolTip(QStringLiteral("Restore this section's built-in defaults for the selected profile."));
            auto* restoreRow = new QHBoxLayout;
            restoreRow->addStretch();
            restoreRow->addWidget(restore);
            form->addRow(restoreRow);
            connect(restore, &QPushButton::clicked, this, [toneControls, start=starts[section], end=ends[section]] {
                for (int i = start; i < end; ++i) toneControls[i]->restoreDefault();
            });
        }
        updateToneControls = [this, toneControls, toneMapping, peakDetection, renderingQuality, state, betaFields] {
            if (state->loading) return;
            const bool beta = rendererChoice_ && effectiveChoiceValue(rendererChoice_).compare(
                QStringLiteral("VP Renderer (Beta)"), Qt::CaseInsensitive) == 0;
            betaFields->setEnabled(beta);
            QString curve = effectiveChoiceValue(toneMapping);
            if (curve.isEmpty() || curve == "auto") curve = "spline";
            QString peak = effectiveChoiceValue(peakDetection);
            const QString quality = effectiveChoiceValue(renderingQuality);
            if (peak.isEmpty() || peak == "auto") peak = quality == "fast" ? "off" : quality == "balanced" ? "on" : "high_quality";
            for (int i = 0; i < ToneMappingTuning::Count; ++i) {
                const bool enabled = i < ToneMappingTuning::SlopeTuning ? (curve == "spline" || curve == "st2094-40") :
                    i < ToneMappingTuning::KneeOffset ? curve == "spline" :
                    i == ToneMappingTuning::KneeOffset ? curve == "bt2390" :
                    i == ToneMappingTuning::ReinhardContrast ? curve == "reinhard" :
                    i < ToneMappingTuning::FilmStrength ? peak != "off" : beta;
                toneControls[i]->setEnabled(enabled);
                toneControls[i]->refresh();
            }
            toneControls[ToneMappingTuning::Percentile]->setDefault(peak == "high_quality" ? 99.995 : 100.);
            const double low = toneControls[ToneMappingTuning::KneeMinimum]->value();
            const double high = toneControls[ToneMappingTuning::KneeMaximum]->value();
            if (low > 0 && low < .5 && high > .5 && high < 1) {
                toneControls[ToneMappingTuning::KneeDefault]->setBounds(low, high);
                toneControls[ToneMappingTuning::KneeDefault]->setDefault(std::clamp(.4, low, high));
            }
            const double sceneLow = toneControls[ToneMappingTuning::SceneThresholdLow]->value();
            const double sceneHigh = toneControls[ToneMappingTuning::SceneThresholdHigh]->value();
            // Keep drags within the valid window. Zero remains an explicit Off
            // detent; typed values retain the full API range and validation.
            toneControls[ToneMappingTuning::SceneThresholdLow]->setSliderBounds(0.,
                sceneHigh > 0 && sceneHigh <= 100 ? sceneHigh - std::min(.01, sceneHigh / 2.) : 100.);
            toneControls[ToneMappingTuning::SceneThresholdHigh]->setSliderBounds(
                sceneLow > 0 && sceneLow < 100 ? sceneLow + std::min(.01, (100. - sceneLow) / 2.) : 0.,
                sceneLow == 100 ? 0. : 100., sceneLow > 0);
            std::string error, key;
            if (!ToneMappingTuning::Validate([&](const char* name) {
                return toneControls[ToneMappingTuning::Find(name)]->configuredValue().toStdString();
            }, error, key)) toneControls[ToneMappingTuning::Find(key)]->setError(QString::fromStdString(error));
        };
        if (rendererChoice_)
            connect(rendererChoice_, qOverload<int>(&QComboBox::currentIndexChanged), betaFields,
                [updateToneControls] { updateToneControls(); });
        connect(customToneMapping, &QCheckBox::toggled, this, [updateToneControls] { updateToneControls(); });
        for (auto* control : toneControls)
            connect(control->editor(), &QLineEdit::textChanged, this, [updateToneControls] { updateToneControls(); });
        for (auto* combo : { toneMapping, peakDetection, renderingQuality })
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [updateToneControls] { updateToneControls(); });

        form = addCollapsibleSection(QStringLiteral("processing"), QStringLiteral("Processing"),
            QString(), false);
        QFormLayout* processingForm = form;
        auto* debanding = addChoice(QStringLiteral("Debanding"), QStringLiteral("deband_strength"), { QStringLiteral("AUTO"), QStringLiteral("default"), QStringLiteral("light"), QStringLiteral("off") });
        debanding->setItemText(debanding->findData(QStringLiteral("default")), QStringLiteral("Standard"));
        addRendererAutoStatus(QStringLiteral("deband_strength"), debanding);
        auto* extendedRange = addChoice(QStringLiteral("Keep below-black and above-white (SDR)"),
            QStringLiteral("preserve_extended_range"), { QStringLiteral("on"), QStringLiteral("off") });
        extendedRange->setToolTip(QStringLiteral(
            "Keeps SDR picture detail below video black and above video white through scaling and "
            "the colour conversion, which otherwise clip it. Needs pure-power gammas (1.8 to 2.8) on "
            "the source and the output, a BT.709 source and no calibration LUT; it turns itself off "
            "otherwise and for HDR. Off is the standard path."));
        MarkGpuCost(form, extendedRange, 1);
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
                const bool knee = sdrShape->currentData().toString() == QStringLiteral("knee");
                for (QWidget* row : { sdrKneeStart, sdrKneeStrength, sdrRecovery })
                {
                    row->setProperty("greyedOff", !knee);
                    row->setEnabled(knee);
                }
            };
            connect(sdrShape, qOverload<int>(&QComboBox::currentIndexChanged), this, [greySdr](int) { greySdr(); });
            greySdr();
        }
        form = processingForm;
        auto* dithering = addChoice(QStringLiteral("Dithering"), QStringLiteral("dithering"),
            { QStringLiteral("AUTO"), QStringLiteral("blue_noise"),
                QStringLiteral("ordered_lut"), QStringLiteral("ordered_fixed"),
                QStringLiteral("white_noise"), QStringLiteral("error_diffusion_simple"),
                QStringLiteral("error_diffusion_false_fs"),
                QStringLiteral("error_diffusion_sierra_lite"),
                QStringLiteral("error_diffusion_floyd_steinberg"),
                QStringLiteral("error_diffusion_atkinson"),
                QStringLiteral("error_diffusion_jarvis_judice_ninke"),
                QStringLiteral("error_diffusion_stucki"),
                QStringLiteral("error_diffusion_burkes"),
                QStringLiteral("error_diffusion_sierra2"),
                QStringLiteral("error_diffusion_sierra3"), QStringLiteral("off") });
        dithering->setItemText(1, QStringLiteral("Auto"));
        dithering->setItemText(2, QStringLiteral("Blue noise"));
        dithering->setItemText(3, QStringLiteral("Ordered (LUT)"));
        dithering->setItemText(4, QStringLiteral("Ordered (fixed)"));
        dithering->setItemText(5, QStringLiteral("White noise"));
        dithering->setItemText(6, QStringLiteral("Error diffusion: Simple"));
        dithering->setItemText(7, QStringLiteral("Error diffusion: False Floyd-Steinberg"));
        dithering->setItemText(8, QStringLiteral("Error diffusion: Sierra Lite"));
        dithering->setItemText(9, QStringLiteral("Error diffusion: Floyd-Steinberg"));
        dithering->setItemText(10, QStringLiteral("Error diffusion: Atkinson"));
        dithering->setItemText(11, QStringLiteral("Error diffusion: Jarvis-Judice-Ninke"));
        dithering->setItemText(12, QStringLiteral("Error diffusion: Stucki"));
        dithering->setItemText(13, QStringLiteral("Error diffusion: Burkes"));
        dithering->setItemText(14, QStringLiteral("Error diffusion: Sierra 2"));
        dithering->setItemText(15, QStringLiteral("Error diffusion: Sierra 3"));
        dithering->setItemText(16, QStringLiteral("Off"));
        dithering->setToolTip(QStringLiteral(
            "Auto follows the rendering-quality preset: Blue noise for Balanced "
            "and High, or Off for Fast. Blue noise is libplacebo's default and "
            "recommended mode. Ordered modes and White noise are alternate "
            "libplacebo methods. Error-diffusion modes require compute shaders "
            "and are substantially more expensive. The display-calibration LUT "
            "runs before this final dithering stage. Off disables dithering."));
        addRendererAutoStatus(QStringLiteral("dithering"), dithering);
        auto* displayBitDepth = addChoice(QStringLiteral("Dither target depth"),
            QStringLiteral("display_bit_depth"),
            {  QStringLiteral("10"), QStringLiteral("8") });
        displayBitDepth->setItemText(displayBitDepth->findData(QStringLiteral("10")), QStringLiteral("10-bit"));
        displayBitDepth->setItemText(displayBitDepth->findData(QStringLiteral("8")), QStringLiteral("8-bit"));
        displayBitDepth->setToolTip(QStringLiteral(
            "Dithering precision, clamped to the active surface depth. This does not configure GPU/HDMI bit depth or detect panel precision."));
        addRendererAutoStatus(QStringLiteral("display_bit_depth"), displayBitDepth);
        auto* fboFloat32 = addChoice(QStringLiteral("32-bit float intermediates"),
            QStringLiteral("fbo_float32"), { QStringLiteral("auto"), QStringLiteral("on"), QStringLiteral("off") });
        fboFloat32->setToolTip(QStringLiteral(
            "Every intermediate picture in 32-bit float: exact highlights (half-float rounds values above white "
            "a little) at more GPU memory bandwidth (a few ms per frame at high resolutions; check the render time in the stats "
            "overlay). Auto: on, except in the queue named Low_Latency (renaming that queue changes Auto). The final "
            "output stage is 32-bit float always."));
        MarkGpuCost(form, fboFloat32, 2);

    }

	else if (sectionPrefix == QStringLiteral("vprenderer.scaling"))
	{
		form = addPlainForm();
		auto* upscaler = addChoice(QStringLiteral("Upscaler"),
			QStringLiteral("upscaler"), { QStringLiteral("AUTO"),
			QStringLiteral("ewa_lanczos4sharpest"),
			QStringLiteral("ewa_lanczossharp"), QStringLiteral("ewa_lanczos"),
			QStringLiteral("lanczos"), QStringLiteral("catmull_rom"),
			QStringLiteral("bicubic"), QStringLiteral("gaussian"),
			QStringLiteral("oversample"), QStringLiteral("bilinear"),
			QStringLiteral("nearest"), QStringLiteral("none") });
		upscaler->setItemText(upscaler->findData(QStringLiteral("none")),
			QStringLiteral("Built-in GPU sampling"));
		addRendererAutoStatus(QStringLiteral("upscaler"), upscaler);
		auto* chromaUpscaler = addChoice(QStringLiteral("Chroma upscaler"),
			QStringLiteral("chroma_upscaler"), { QStringLiteral("AUTO"),
			QStringLiteral("catmull_rom"), QStringLiteral("lanczos"), QStringLiteral("ewa_lanczossharp"),
			QStringLiteral("upscaler"), QStringLiteral("bicubic"), QStringLiteral("bilinear") });
		chromaUpscaler->setItemText(chromaUpscaler->findData(QStringLiteral("upscaler")),
			QStringLiteral("Same as the upscaler (the earlier behaviour)"));
		chromaUpscaler->setToolTip(QStringLiteral(
			"How the colour planes of 4:2:0 / 4:2:2 video are brought up to the picture's size before the "
			"main scaling. Auto = Catmull-Rom: on real footage it looked identical to the polar EWA Lanczos "
			"filter at about a seventh of its GPU time. 'Same as the "
			"upscaler' is the previous behaviour (EWA Lanczos sharp with High quality, the costliest)."));
		MarkGpuCost(form, chromaUpscaler, 2);
		auto* downscaler = addChoice(QStringLiteral("Downscaler"),
			QStringLiteral("downscaler"), { QStringLiteral("AUTO"),
			QStringLiteral("lanczos"), QStringLiteral("mitchell"),
			QStringLiteral("catmull_rom"),
			QStringLiteral("bicubic"), QStringLiteral("gaussian"),
			QStringLiteral("hermite"), QStringLiteral("bilinear"),
			QStringLiteral("box"), QStringLiteral("gpu") });
		downscaler->setItemText(downscaler->findData(QStringLiteral("gpu")),
			QStringLiteral("Built-in GPU sampling"));
		addRendererAutoStatus(QStringLiteral("downscaler"), downscaler);
		auto* antiRinging = addChoice(QStringLiteral("Anti-ringing"),
			QStringLiteral("sigmoid"), { QStringLiteral("AUTO"),
			QStringLiteral("on"), QStringLiteral("off") });
		antiRinging->setToolTip(QStringLiteral(
			"Optional sigmoidization before upscaling. It reduces ringing artifacts; Auto uses the scaling-quality preset."));
		addRendererAutoStatus(QStringLiteral("sigmoid"), antiRinging);
		auto* downscaleLight = addChoice(QStringLiteral("Downscale light"),
			QStringLiteral("downscale_light"), { QStringLiteral("linear"), QStringLiteral("gamma") });
		downscaleLight->setToolTip(QStringLiteral(
			"Light domain for frames that are downscaled. Linear is libplacebo's "
			"default; gamma matches madVR's SSIM look. Upscaled frames always use linear light."));
	}
    else if (sectionPrefix == QStringLiteral("vprenderer.subtitles"))
    {
        auto* type = addChoice(QStringLiteral("Processing style"), QStringLiteral("type"),
            {QStringLiteral("off"), QStringLiteral("classic"), QStringLiteral("generated_gray"), QStringLiteral("black")}, false);
        type->setItemText(0, QStringLiteral("Off"));
        type->setItemText(1, QStringLiteral("Classic"));
        type->setItemText(2, QStringLiteral("Move / Reconstruction (Beta)"));
        type->setItemText(3, QStringLiteral("Move / Solid Background (Beta)"));
        auto* scriptHint = addChoice(QStringLiteral("Subtitle script hint"),
            QStringLiteral("subtitle_script_hint"),
            { QStringLiteral("latin"), QStringLiteral("cjk"),
              QStringLiteral("arabic"), QStringLiteral("thai") });
        scriptHint->setItemText(scriptHint->findData(QStringLiteral("latin")),
            QStringLiteral("Latin"));
        scriptHint->setItemText(scriptHint->findData(QStringLiteral("cjk")),
            QStringLiteral("CJK (Chinese / Japanese / Korean)"));
        scriptHint->setItemText(scriptHint->findData(QStringLiteral("arabic")),
            QStringLiteral("Arabic"));
        scriptHint->setItemText(scriptHint->findData(QStringLiteral("thai")),
            QStringLiteral("Thai"));
        scriptHint->setToolTip(QStringLiteral(
            "Guides Alpha's pixel-based grouping of subtitle rows, marks, and spacing. "
            "This does not recognize text or select Windows OCR languages."));
        auto* offset = addText(QStringLiteral("Inward offset / Classic padding"), QStringLiteral("offset_pixels"), QStringLiteral("source pixels"));
        offset->setValidator(new QIntValidator(0,500,offset));
        offset->setToolTip(QStringLiteral("Offsets the whole moved subtitle box: bottom subtitles move higher and top subtitles move lower. Text stays centered within the box; extension to the black bar does not count toward centering. In Classic, this is the original subtitle padding."));
        auto* commonForm=form;
        form = addCollapsibleSection(QStringLiteral("classic"), QStringLiteral("Classic picture movement"), QString(), true);
        auto* classicSection=profileFieldsLayout->itemAt(profileFieldsLayout->count()-1)->widget();
        auto number=[&](const QString& label,const QString& key,double low,double high,const QString& unit) {
            auto* field=addText(label,key,unit);
            if(key==QStringLiteral("subtitle_engage_drift_ms") || key==QStringLiteral("subtitle_release_drift_ms") || key==QStringLiteral("subtitle_target_buffer_pixels"))
                field->setValidator(new QIntValidator(static_cast<int>(low),static_cast<int>(high),field));
            else {
                auto* validator=new QDoubleValidator(low,high,3,field);
                validator->setNotation(QDoubleValidator::StandardNotation);
                field->setValidator(validator);
            }
            return field;
        };
        number(QStringLiteral("Hold after subtitles disappear"),QStringLiteral("subtitle_hold_seconds"),0.25,30,QStringLiteral("seconds"));
        number(QStringLiteral("Engage drift duration"),QStringLiteral("subtitle_engage_drift_ms"),0,30000,QStringLiteral("ms"));
        number(QStringLiteral("Release drift duration"),QStringLiteral("subtitle_release_drift_ms"),0,30000,QStringLiteral("ms"));
        number(QStringLiteral("Target buffer"),QStringLiteral("subtitle_target_buffer_pixels"),0,50,QStringLiteral("source pixels"));
        form = addCollapsibleSection(QStringLiteral("appearance"), QStringLiteral("Subtitle appearance"), QString(), true);
        auto* appearanceSection=profileFieldsLayout->itemAt(profileFieldsLayout->count()-1)->widget();
        auto color=[&](const QString& label,const QString& key) {
            auto* field=addText(label,key,QStringLiteral("RRGGBB"));
            field->setMaxLength(6);
            field->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9A-Fa-f]{6}")),field));
            form->addRow(new InlineColorPicker(field,label));
            return field;
        };
        auto* rounded = addBoolean(QStringLiteral("Rounded corners"), QStringLiteral("rounded_corners"));
        rounded->setToolTip(QStringLiteral("Round the subtitle box corners. Floating boxes round all four corners."));
        auto* edge = addChoice(QStringLiteral("Background edge"), QStringLiteral("background_edge"),
            {QStringLiteral("extend"), QStringLiteral("float")}, false);
        edge->setItemText(0, QStringLiteral("Extend to black bar"));
        edge->setItemText(1, QStringLiteral("Float"));
        edge->setToolTip(QStringLiteral("Float offsets the whole box from the black bar. Original subtitle backing that overlaps the picture always forces extension to the bar."));
        auto* textReduction = addText(QStringLiteral("Text size reduction"),
            QStringLiteral("text_reduction_percent"), QStringLiteral("% (0 = unchanged)"));
        textReduction->setValidator(new QIntValidator(0,75,textReduction));
        textReduction->setToolTip(QStringLiteral(
            "Reduces moved subtitle text size. 0% keeps the current size; "
            "75% reduces it to 25% of its original size. Applies to both moved styles."));
        number(QStringLiteral("Minimum headroom above text"),QStringLiteral("minimum_headroom_pixels"),0,200,QStringLiteral("source pixels"));
        color(QStringLiteral("Text color"),QStringLiteral("subtitle_text_color"))->setToolTip(
            QStringLiteral("White (FFFFFF) preserves the original text. Choose a darker gray to dim it, or a color to tint it. Applies to both Move styles; detection and background are unchanged."));
        color(QStringLiteral("Background color"),QStringLiteral("subtitle_generated_gray_color"));
        color(QStringLiteral("Border color"),QStringLiteral("subtitle_generated_gray_border_color"));
        number(QStringLiteral("Border opacity"),QStringLiteral("subtitle_generated_gray_border_opacity"),0,1,QStringLiteral("0–1"));
        number(QStringLiteral("Border width"),QStringLiteral("subtitle_generated_gray_border_width"),0,8,QStringLiteral("source pixels"));
        form = addCollapsibleSection(QStringLiteral("reconstruction"), QStringLiteral("Reconstruction"), QString(), true);
        auto* reconstructionSection=profileFieldsLayout->itemAt(profileFieldsLayout->count()-1)->widget();
        number(QStringLiteral("Background opacity"),QStringLiteral("subtitle_generated_gray_opacity"),0,1,QStringLiteral("0–1"));
        number(QStringLiteral("Background blur"),QStringLiteral("subtitle_generated_gray_blur_px"),0,30,QStringLiteral("source pixels"));
        number(QStringLiteral("Brightness limit"),QStringLiteral("subtitle_generated_gray_max_luminance"),0.01,1,QStringLiteral("linear 0.01–1"));
        form = addCollapsibleSection(QStringLiteral("hdr_analysis"),
            QStringLiteral("HDR analysis"), QStringLiteral("Protect HDR analysis from subtitles in every processing style."), false);
		pictureOnlyHdrAnalysis = addBoolean(
            QStringLiteral("Limit HDR analysis to picture center"),
            QStringLiteral("hdr_peak_analysis_picture_only"));
        pictureOnlyHdrAnalysis->setToolTip(QStringLiteral(
            "When a current trusted active-picture rectangle is available, "
			"use its configured central height and full available width for "
			"libplacebo's HDR peak and average-luminance analysis. This inset "
			"rejects subtitles and OSDs that cross from a black bar into the "
			"picture. Full-raster, invalid, or stale geometry falls back to "
			"full-frame analysis."));
		motionCompensatedHdrAnalysis = addBoolean(
			QStringLiteral("Protect HDR analysis during subtitle movement"),
			QStringLiteral("hdr_peak_analysis_motion_compensation"));
		motionCompensatedHdrAnalysis->setToolTip(QStringLiteral(
			"Experimental. When fixed center analysis is off, use VP's pending or "
			"active subtitle handling to exclude the affected active-picture "
			"edge from HDR analysis. This uses existing geometry and does not use OCR."));
		// Keep the established Boolean keys as the persistence contract, but
		// present their mutually exclusive semantics as one mode. This also
		// preserves configurations written by earlier VP-0147 test builds.
		pictureOnlyHdrAnalysis->hide();
		motionCompensatedHdrAnalysis->hide();
		if (QWidget* label = form->labelForField(pictureOnlyHdrAnalysis))
			label->hide();
		if (QWidget* label = form->labelForField(motionCompensatedHdrAnalysis))
			label->hide();
		hdrAnalysisMode = new QComboBox;
		hdrAnalysisMode->setObjectName(controlName(sectionPrefix,
			QStringLiteral("hdr_peak_analysis_mode")));
		hdrAnalysisMode->setAccessibleName(
			QStringLiteral("HDR analysis protection"));
		hdrAnalysisMode->setSizePolicy(
			QSizePolicy::Expanding, QSizePolicy::Fixed);
		hdrAnalysisMode->addItem(QStringLiteral("Off"), QStringLiteral("off"));
		hdrAnalysisMode->addItem(QStringLiteral("Smart (Experimental)"),
			QStringLiteral("automatic"));
		hdrAnalysisMode->addItem(QStringLiteral("Percentage (Beta)"),
			QStringLiteral("fixed"));
		hdrAnalysisMode->setToolTip(QStringLiteral(
			"Off uses normal full-presentation HDR analysis. Smart protects only the "
			"active-picture edge affected by VP's subtitle handling. Percentage "
			"uses the configured top, center, or bottom band. On bar-cropped content it "
			"uses the visible active picture; on full-raster content it uses the final "
			"presentation crop."));
		form->addRow(QStringLiteral("HDR analysis protection"), hdrAnalysisMode);
		hdrAnalysisHeight = addText(
			QStringLiteral("HDR analysis height"),
			QStringLiteral("hdr_peak_analysis_height_percent"),
			QStringLiteral("%"));
		hdrAnalysisHeight->setValidator(new QIntValidator(10, 100, hdrAnalysisHeight));
		hdrAnalysisHeight->setToolTip(QStringLiteral(
			"Percentage of visible picture height analyzed by libplacebo. Smaller values "
			"exclude more subtitle and OSD area."));
		hdrAnalysisHeight->setEnabled(false);
		hdrAnalysisPosition = addChoice(QStringLiteral("HDR analysis position"),
			QStringLiteral("hdr_peak_analysis_position"),
			{ QStringLiteral("top"), QStringLiteral("center"),
				QStringLiteral("bottom") }, false);
		hdrAnalysisPosition->setToolTip(QStringLiteral(
			"Anchors the percentage band within the visible picture. Top is the default "
			"because subtitles are usually at the bottom."));
		hdrAnalysisPosition->setProperty("requiresHdrFixedMode", true);
		hdrAnalysisPosition->setEnabled(false);
		connect(hdrAnalysisMode,
			qOverload<int>(&QComboBox::currentIndexChanged), this,
			[this, state, hdrAnalysisMode, pictureOnlyHdrAnalysis,
			 motionCompensatedHdrAnalysis, hdrAnalysisHeight, hdrAnalysisPosition](int index)
			{
				if (index < 0) return;
				const QString mode = hdrAnalysisMode->itemData(index).toString();
				const bool fixed = mode == QStringLiteral("fixed");
				const bool automatic = mode == QStringLiteral("automatic");
				hdrAnalysisHeight->setEnabled(fixed);
				hdrAnalysisPosition->setEnabled(fixed);
				if (state->loading) return;
				pictureOnlyHdrAnalysis->setChecked(fixed);
				motionCompensatedHdrAnalysis->setChecked(automatic);
                // Persist both halves even if one checkbox did not change:
                // an omitted half must not revive a conflicting legacy mode.
                if (!state->section.isEmpty() && document_) {
                    document_->SetKnown(state->section.toStdString(), "hdr_peak_analysis_picture_only", fixed ? "true" : "false");
                    document_->SetKnown(state->section.toStdString(), "hdr_peak_analysis_motion_compensation", automatic ? "true" : "false");
                    markDirty();
                }
			});
        // Let the scroll area grow to the styled controls' minimum sizes rather
        // than squeezing newly visible sections into the previous style's height.
        for(auto* layout:profileFields->findChildren<QLayout*>())
            layout->setSizeConstraint(QLayout::SetMinimumSize);
        profileFieldsLayout->setSizeConstraint(QLayout::SetMinimumSize);
        detailLayout->setSizeConstraint(QLayout::SetMinimumSize);
        auto update=[type,classicSection,appearanceSection,reconstructionSection,commonForm,offset] {
            const auto mode=type->currentData().toString();
            classicSection->setVisible(mode==QStringLiteral("classic"));
            appearanceSection->setVisible(mode==QStringLiteral("generated_gray") || mode==QStringLiteral("black"));
            reconstructionSection->setVisible(mode==QStringLiteral("generated_gray"));
            commonForm->setRowVisible(offset->parentWidget(),mode!=QStringLiteral("off"));
        };
        connect(type,qOverload<int>(&QComboBox::currentIndexChanged),this,[update](int){update();});
        update();
    }
	else if (sectionPrefix == QStringLiteral("vprenderer.viewport") ||
		sectionPrefix == QStringLiteral("vprenderer.zoom"))
    {
		const bool showGeometry = sectionPrefix == QStringLiteral("vprenderer.viewport");
		const bool showZoom = sectionPrefix == QStringLiteral("vprenderer.zoom");
		if (showGeometry)
		{
        form = addCollapsibleSection(QStringLiteral("geometry"),
            QStringLiteral("Screen geometry"), QStringLiteral(
                "Physical screen shape, picture placement, and fitting controls."), true);
        auto* screenAspect = addText(
            QStringLiteral("Screen aspect ratio"),
            QStringLiteral("screen_aspect"));
        screenAspect->setPlaceholderText(QStringLiteral("Use output display aspect"));
        screenAspect->setToolTip(QStringLiteral(
            "The physical screen shape. Enter a ratio, decimal aspect, or "
            "screen dimensions. If omitted and not inherited, use the output display aspect. This same value controls destination layout, "
            "black-bar cropping, subtitle fitting, and NLS's target aspect."));
        form->addRow(QString(), helpLabel(QStringLiteral(
            "Enter the physical screen shape as a ratio (for example 2.1:1), "
            "a decimal (2.1), or dimensions (2100x1000). VP uses this value "
            "consistently for black bars, cropping, subtitles, and NLS.")));
        auto* verticalAlignment = addChoice(
            QStringLiteral("Vertical picture alignment"),
            QStringLiteral("vertical_alignment"),
            { QStringLiteral("top"), QStringLiteral("center"),
                QStringLiteral("bottom") });
        verticalAlignment->setToolTip(QStringLiteral(
            "Sets the picture's resting position when unused vertical screen "
            "space remains. Subtitle fitting may temporarily move it away "
            "from this edge to keep HDMI subtitle pixels visible."));
        form->addRow(QString(), helpLabel(QStringLiteral(
            "Top and Bottom support one-sided masking. Center preserves the "
            "current presentation. Enabled subtitle fitting can still move "
            "the picture upward or downward as required.")));
		auto* screenEdgePadding = addText(QStringLiteral("Screen edge padding"),
			QStringLiteral("screen_edge_padding"), QStringLiteral("px"));
		screenEdgePadding->setValidator(new QIntValidator(0, 100000,
			screenEdgePadding));
		screenEdgePadding->setToolTip(QStringLiteral(
			"Reserves empty output space at the selected top or bottom screen edge. "
			"It does not add content padding, crop the picture, or change its scale."));
		form->addRow(QString(), helpLabel(QStringLiteral(
			"Uses existing vertical slack only; Center disables this setting.")));
		auto updateScreenEdgePadding = [verticalAlignment, screenEdgePadding]()
		{
			screenEdgePadding->setEnabled(
				verticalAlignment->currentData().toString() != QStringLiteral("center"));
		};
		connect(verticalAlignment, qOverload<int>(&QComboBox::currentIndexChanged),
			this, [updateScreenEdgePadding](int) { updateScreenEdgePadding(); });
		updateScreenEdgePadding();
		for (const auto& blank : {
			std::make_pair(QStringLiteral("Blank left edge"), QStringLiteral("blank_left")),
			std::make_pair(QStringLiteral("Blank right edge"), QStringLiteral("blank_right")),
			std::make_pair(QStringLiteral("Blank top edge"), QStringLiteral("blank_top")),
			std::make_pair(QStringLiteral("Blank bottom edge"), QStringLiteral("blank_bottom")) })
		{
			auto* blankEdge = addText(blank.first, blank.second, QStringLiteral("px"));
			blankEdge->setValidator(new QIntValidator(0, 4000, blankEdge));
			blankEdge->setToolTip(QStringLiteral(
				"Output pixels at this panel edge that are always black. The picture "
				"is scaled into the remaining area, so nothing is cut off."));
		}
		form->addRow(QString(), helpLabel(QStringLiteral(
			"Use for panel edges the display cannot show cleanly. Ignored if the "
			"blanked area would exceed half the output width or height.")));
        anamorphicEnabled = new QCheckBox(QStringLiteral("Enable anamorphic lens compensation"));
        anamorphicEnabled->setObjectName(controlName(sectionPrefix,
            QStringLiteral("anamorphic_enabled")));
        form->addRow(QString(), anamorphicEnabled);
        anamorphicValue = addText(QStringLiteral("Lens expansion ratio"), QStringLiteral("anamorphic_scale"));
        connect(anamorphicEnabled, &QCheckBox::toggled, this, [this, state, anamorphicValue](bool enabled)
        {
            anamorphicValue->setEnabled(enabled);
            if (state->loading || state->section.isEmpty()) return;
            if (!enabled) document_->RemoveKnown(state->section.toStdString(), "anamorphic_scale");
            else if (anamorphicValue->text().trimmed().isEmpty())
            {
                anamorphicValue->setText(QStringLiteral("1:1"));
                document_->SetKnown(state->section.toStdString(), "anamorphic_scale", "1:1");
            }
            markDirty();
        });
		}
		if (showZoom && !showGeometry)
			form = addPlainForm();
		if (showZoom)
		{
		addBoolean(QStringLiteral("Automatically crop black bars"),
			QStringLiteral("automatic_crop"));
		auto* cropNarrower = addBoolean(
			QStringLiteral("Crop narrower content to fill screen"),
			QStringLiteral("crop_narrower_content_to_fill_screen"));
		auto* cropNarrowerLimit = addText(QStringLiteral("Minimum eligible source aspect"),
			QStringLiteral("crop_narrower_content_aspect_limit"));
		cropNarrowerLimit->setToolTip(QStringLiteral(
            "Applies to content narrower than the screen, down to this source aspect. "
            "Fills the screen by cropping top and bottom. This is an eligibility limit, not the output aspect."));
        cropNarrowerLimit->setPlaceholderText(QStringLiteral("Optional, e.g. 2.20:1"));
		cropNarrowerLimit->setEnabled(cropNarrower->isChecked());
		connect(cropNarrower, &QCheckBox::toggled, this,
			[cropNarrowerLimit](bool enabled)
			{ cropNarrowerLimit->setEnabled(enabled); });

		auto* cropWider = addBoolean(
			QStringLiteral("Crop wider content to fill screen"),
			QStringLiteral("crop_wider_content_to_fill_screen"));
		auto* cropWiderLimit = addText(QStringLiteral("Maximum eligible source aspect"),
			QStringLiteral("crop_wider_content_aspect_limit"));
		cropWiderLimit->setToolTip(QStringLiteral(
            "Applies to content wider than the screen, up to this source aspect. "
            "For a 16:9 screen, 1.90 allows 1.85:1 content to fill by cropping left and right. "
            "This is an eligibility limit, not the output aspect."));
        cropWiderLimit->setPlaceholderText(QStringLiteral("Optional, e.g. 2.76:1"));
		cropWiderLimit->setEnabled(cropWider->isChecked());
		connect(cropWider, &QCheckBox::toggled, this,
			[cropWiderLimit](bool enabled)
			{ cropWiderLimit->setEnabled(enabled); });
		auto* fixedCropAspect = addText(QStringLiteral("Fixed crop aspect"),
			QStringLiteral("fixed_crop_aspect"));
		fixedCropAspect->setPlaceholderText(QStringLiteral("Off; e.g. 2.00:1"));
		fixedCropAspect->setToolTip(QStringLiteral(
			"Center-crop trusted content to this exact aspect before fitting it to the physical screen. "
			"This overrides the fill-screen crop choices for this Zoom profile."));



		}
    }
    else
    {
        addText(QStringLiteral("MaxCLL"), QStringLiteral("max_cll"), QStringLiteral("nits"));
        addText(QStringLiteral("MaxFALL"), QStringLiteral("max_fall"), QStringLiteral("nits"));
        addText(QStringLiteral("Mastering minimum"), QStringLiteral("mastering_min_luminance"), QStringLiteral("nits"));
        addText(QStringLiteral("Mastering maximum"), QStringLiteral("mastering_max_luminance"), QStringLiteral("nits"));
    }
    if (sectionPrefix == QStringLiteral("lldv"))
    {
        detailLayout->addWidget(helpLabel(QStringLiteral(
            "Uses the selected profile for recognized HDFury LLDV input and sends the same effective metadata to DirectShow and VP Renderer.")));
        detailLayout->addWidget(bindCheckField(QStringLiteral("Alternate LLDV detection"),
            QStringLiteral("general"), QStringLiteral("newlldv")));
        detailLayout->addWidget(helpLabel(QStringLiteral(
            "Applies to all LLDV profiles. Uses VP's newer LLDV detection heuristic. Restart VideoProcessor after changing this setting.")));
    }
    detailLayout->addWidget(profileFields);
    detailLayout->addStretch();

    const auto updateQueuePolicyPresentation = [queuePolicy, queuePolicySummary,
        profileFields]()
    {
        if (!queuePolicy) return;
        const QString selectedId = queuePolicy->currentData().toString();
        if (selectedId.isEmpty())
        {
            queuePolicySummary->setText(QStringLiteral(
                "Custom queue values are shown in Advanced."));
            if (auto* advanced = profileFields->findChild<QToolButton*>(
                QStringLiteral("rendererSection.queueAdvanced")); advanced &&
                !advanced->isChecked())
                advanced->setChecked(true);
        }
        else
        {
            for (const QueuePolicy& policy : queuePolicies)
                if (selectedId == QString::fromLatin1(policy.id))
                {
                    queuePolicySummary->setText(QString::fromLatin1(policy.summary));
                    break;
                }
        }
    };
    const auto updateQueuePolicyFromValues = [fields, queuePolicy,
        updateQueuePolicyPresentation]()
    {
        if (!queuePolicy) return;
        const auto queueValue = [fields](const QString& key) -> int
        {
            for (const Field& field : *fields)
                if (field.key == key && field.kind == Field::Integer)
                    return qobject_cast<QSpinBox*>(field.widget)->value();
            return -1;
        };
        QString policyId;
        for (const QueuePolicy& policy : queuePolicies)
            if (queueValue(QStringLiteral("queue_size")) == policy.capacity &&
                queueValue(QStringLiteral("lead_frames")) == policy.lead &&
                queueValue(QStringLiteral("target_frames")) == policy.target &&
                queueValue(QStringLiteral("target_latency_ms")) <= 0 &&
                queueValue(QStringLiteral("startup_preroll_frames")) == 0 &&
                queueValue(QStringLiteral("active_picture_lookahead_frames")) == 0 &&
                queueValue(QStringLiteral("reset_after_render_restart_seconds")) == 5 &&
                queueValue(QStringLiteral("reset_queue_too_large_percent")) == 75)
            {
                policyId = QString::fromLatin1(policy.id);
                break;
            }
        const QSignalBlocker blocker(queuePolicy);
        queuePolicy->setCurrentIndex(std::max(0, queuePolicy->findData(policyId)));
        updateQueuePolicyPresentation();
    };

    auto loadDetails = [this, state, fields, selectedTitle, name, shortcut, cycleShortcut, rule, ruleField, useRule, remove, up, down, list, sdrGammaEnabled, sdrGammaStatus, desiredSdrGamma, sdrGammaInherit,
		profileFields, sectionPrefix, anamorphicEnabled, anamorphicValue,
		hdrAnalysisMode, pictureOnlyHdrAnalysis,
		motionCompensatedHdrAnalysis, hdrAnalysisHeight, hdrAnalysisPosition,
        deprecatedViewportAlias, queuePolicy, updateQueuePolicyFromValues, updateOutputCompatibility, updateToneControls](QListWidgetItem* current)
    {
        state->loading = true;
        state->section = current ? current->data(Qt::UserRole).toString() : QString();
        const bool available = !state->section.isEmpty();
        name->setEnabled(available);
        shortcut->setEnabled(available);
        cycleShortcut->setEnabled(available);
        useRule->setEnabled(available);
        rule->setEnabled(available && useRule->isChecked());
        profileFields->setEnabled(available);
        remove->setEnabled(available && list->count() > 1);
        up->setEnabled(available && list->currentRow() > 0);
        down->setEnabled(available && list->currentRow() + 1 < list->count());
        if (!available)
        {
            selectedTitle->setText(QStringLiteral("Add a profile to configure it"));
            name->clear();
            shortcut->clear();
            cycleShortcut->clear();
            useRule->setChecked(false);
            ruleField->setVisible(false);
            state->loading = false;
            return;
        }
        const QString section = state->section;
        const QString display = current->data(Qt::UserRole + 1).toString();
        selectedTitle->setText(!activeProfileSnapshotAvailable_ ?
            QStringLiteral("%1 (Live status unavailable)").arg(display) :
            current->data(ActiveProfileRole).toBool() ?
                QStringLiteral("%1 (Active)").arg(display) :
                QStringLiteral("%1 (Not active)").arg(display));
        name->setText(display);
        shortcut->setText(canonicalShortcutText(
            value(section, QStringLiteral("shortcut"))));
        cycleShortcut->setText(canonicalShortcutText(
            value(section, QStringLiteral("cycle_shortcut"))));
        const QString expression = value(section, QStringLiteral("when"));
        rule->setPlainText(expression);
        useRule->setChecked(!expression.isEmpty());
        ruleField->setVisible(!expression.isEmpty());
        auto fallback = [this, sectionPrefix, section](const QString& key) -> QString
        {
            if (sectionPrefix == QStringLiteral("vprenderer.color"))
            {
                if (key == QStringLiteral("target_primaries")) return QStringLiteral("rec709");
                if (key == QStringLiteral("sdr_adjust_gamma")) return QStringLiteral("passthrough");
                if (key == QStringLiteral("sdr_input_transfer")) return QStringLiteral("2.4");
                if (key == QStringLiteral("sdr_lut_input_gamma")) return QStringLiteral("passthrough");
                if (key == QStringLiteral("report_bt2020_to_display")) return QStringLiteral("false");
                if (key == QStringLiteral("hdr_tone_map_target_gamma")) return QStringLiteral("2.2");
                if (key == QStringLiteral("calibration_lut_enabled")) return QStringLiteral("false");
                if (key.startsWith(QStringLiteral("calibration_lut_"))) return QStringLiteral("none");
                if (key.startsWith(QStringLiteral("diagnostic_")) || key == QStringLiteral("output_diagnostics")) return QStringLiteral("false");
                if (key == QStringLiteral("output_path_profile")) return QStringLiteral("legacy");
                return QStringLiteral("auto");
            }
            if (sectionPrefix == QStringLiteral("queue"))
            {
                if (key == QStringLiteral("queue_size")) return QStringLiteral("32");
                if (key == QStringLiteral("lead_frames")) return QStringLiteral("1");
                if (key == QStringLiteral("target_frames")) return QStringLiteral("4");
                if (key == QStringLiteral("target_latency_ms")) return QStringLiteral("0");
                if (key == QStringLiteral("active_picture_lookahead_frames")) return QStringLiteral("0");
                if (key == QStringLiteral("startup_preroll_frames")) return QStringLiteral("0");
                if (key == QStringLiteral("steady_reserve_frames")) return QStringLiteral("4");
                if (key == QStringLiteral("reset_after_render_restart_seconds")) return QStringLiteral("5");
                if (key == QStringLiteral("reset_queue_too_large_percent")) return QStringLiteral("75");
            }
            if (sectionPrefix == QStringLiteral("vprenderer.viewport"))
            {
                if (key == QStringLiteral("screen_aspect")) return QString();
                if (key == QStringLiteral("vertical_alignment")) return QStringLiteral("center");
				if (key == QStringLiteral("screen_edge_padding")) return QStringLiteral("0");
				if (key == QStringLiteral("blank_left") || key == QStringLiteral("blank_right") ||
					key == QStringLiteral("blank_top") || key == QStringLiteral("blank_bottom"))
					return QStringLiteral("0");
            }
            if (sectionPrefix == QStringLiteral("vprenderer.subtitles")) {
                if (key.startsWith(QStringLiteral("hdr_peak_analysis_"))) {
                    return legacySubtitleHdrValue(key);
                }
                if(key == QStringLiteral("type")) return QStringLiteral("classic");
                if(key == QStringLiteral("subtitle_script_hint")) return QStringLiteral("latin");
                if(key == QStringLiteral("offset_pixels")) return value(section,QStringLiteral("type"))==QStringLiteral("classic") || value(section,QStringLiteral("type")).isEmpty() ? QString::number(RendererProfileConfig::DEFAULT_CLASSIC_SUBTITLE_PADDING_PIXELS) : QStringLiteral("0");
                if(key == QStringLiteral("subtitle_hold_seconds")) return QStringLiteral("2");
                if(key == QStringLiteral("subtitle_engage_drift_ms")) return QString::number(RendererProfileConfig::DEFAULT_CLASSIC_SUBTITLE_ENGAGE_DRIFT_MS);
                if(key == QStringLiteral("subtitle_release_drift_ms")) return QString::number(RendererProfileConfig::DEFAULT_CLASSIC_SUBTITLE_RELEASE_DRIFT_MS);
                if(key == QStringLiteral("subtitle_target_buffer_pixels")) return QStringLiteral("10");
                if(key == QStringLiteral("subtitle_text_color")) return QStringLiteral("FFFFFF");
                if(key == QStringLiteral("subtitle_generated_gray_color")) return value(section,QStringLiteral("type"))==QStringLiteral("black") ? QStringLiteral("000000") : QStringLiteral("040404");
                if(key == QStringLiteral("minimum_headroom_pixels")) return QStringLiteral("12");
                if(key == QStringLiteral("text_reduction_percent")) return QStringLiteral("0");
                if(key == QStringLiteral("rounded_corners")) return QStringLiteral("true");
                if(key == QStringLiteral("background_edge")) return QStringLiteral("extend");
                if(key == QStringLiteral("subtitle_generated_gray_border_color")) return QStringLiteral("000000");
                if(key == QStringLiteral("subtitle_generated_gray_border_opacity")) return QStringLiteral("0.65");
                if(key == QStringLiteral("subtitle_generated_gray_border_width")) return QStringLiteral("0");
                if(key == QStringLiteral("subtitle_generated_gray_opacity")) return QStringLiteral("0.85");
                if(key == QStringLiteral("subtitle_generated_gray_blur_px")) return QStringLiteral("3");
                if(key == QStringLiteral("subtitle_generated_gray_max_luminance")) return QStringLiteral("0.16");
            }
            if (sectionPrefix == QStringLiteral("vprenderer.zoom"))
            {
                if (key == QStringLiteral("subtitle_hold_seconds")) return QStringLiteral("2");
				if (key == QStringLiteral("hdr_peak_analysis_height_percent")) return QStringLiteral("75");
				if (key == QStringLiteral("hdr_peak_analysis_position")) return QStringLiteral("top");
                if (key == QStringLiteral("subtitle_engage_drift_ms")) return QStringLiteral("0");
                if (key == QStringLiteral("subtitle_release_drift_ms")) return QStringLiteral("0");
                if (key == QStringLiteral("subtitle_padding_pixels")) return QStringLiteral("20");
                if (key == QStringLiteral("subtitle_target_buffer_pixels")) return QStringLiteral("10");
            }
            if (sectionPrefix == QStringLiteral("lldv"))
            {
                QString enabled = value(QStringLiteral("general"), QStringLiteral("newlldv"));
                if (enabled.isEmpty()) enabled = value(QStringLiteral("general"), QStringLiteral("new_lldv"));
                if (enabled.isEmpty()) enabled = value(QStringLiteral("command_line"), QStringLiteral("newlldv"));
                if (enabled.isEmpty()) enabled = value(QStringLiteral("command_line"), QStringLiteral("new_lldv"));
                const QString normalized = enabled.trimmed().toLower();
                const bool modern = normalized == QStringLiteral("true") || normalized == QStringLiteral("yes") ||
                    normalized == QStringLiteral("on") || normalized == QStringLiteral("1");
                if (key == QStringLiteral("max_cll")) return QStringLiteral("1000");
                if (key == QStringLiteral("max_fall")) return modern ? QStringLiteral("401") : QStringLiteral("1000");
                if (key == QStringLiteral("mastering_min_luminance")) return modern ? QStringLiteral("0.001") : QStringLiteral("0.0001");
                if (key == QStringLiteral("mastering_max_luminance")) return modern ? QStringLiteral("4000") : QStringLiteral("1000");
            }
            if (sectionPrefix == QStringLiteral("vprenderer"))
            {
                if (key == QStringLiteral("dynamic_target")) return QStringLiteral("true");
                if (key == QStringLiteral("custom_tone_mapping")) return QStringLiteral("false");
                if (key == QStringLiteral("quality")) return QStringLiteral("high");
                if (key == QStringLiteral("output_path_profile")) return QStringLiteral("legacy");
                if (key == QStringLiteral("target_primaries")) return QStringLiteral("REC709");
                if (key == QStringLiteral("sdr_target_nits")) return QStringLiteral("203");
                if (key == QStringLiteral("sdr_black_nits")) return QString::number(HdrTargetLuminance::DefaultBlack);
                if (key == QStringLiteral("contrast_recovery")) return QStringLiteral("Auto");
                if (key == QStringLiteral("saturation")) return QStringLiteral("0");
				if (key == QStringLiteral("deband_strength")) return QStringLiteral("AUTO");
				if (key == QStringLiteral("report_bt2020_to_display") ||
                    key == QStringLiteral("output_diagnostics") ||
                    key == QStringLiteral("diagnostic_disable_shader_cache") ||
                    key == QStringLiteral("diagnostic_disable_compute") ||
                    key == QStringLiteral("diagnostic_force_8bit_sdr_swapchain") ||
                    key == QStringLiteral("diagnostic_allow_limited_g22") ||
                    key == QStringLiteral("diagnostic_allow_full_g22") ||
                    key == QStringLiteral("diagnostic_vp_owned_dxgi_presenter")) return QStringLiteral("false");
                // The renderer's own defaults (LibplaceboVideoRenderer.cpp's
                // settings, AdaptiveDownscale.h), so an unset choice says what
                // it really is (audit 2026-10-14c: these all showed "Auto").
                static const std::pair<const char*, const char*> choiceDefaults[] = {
                    { "capture_path", "gpu" }, { "capture_upload", "render" }, { "dejudder_even_rates", "whole" }, { "occlusion_mask", "off" },
                    { "dejudder_warp_stage", "rendered" }, { "dejudder_global_prior", "on" },
                    { "dejudder_vector_smooth", "on" }, { "dejudder_occlusion_fill", "off" },
                    { "dejudder_est_candidates", "on" }, { "dejudder_est_qpel", "on" }, { "dejudder_est_gate", "on" },
                    { "dejudder_est_split", "off" }, { "dejudder_motion_rolloff", "off" }, { "dejudder_global_two", "on" },
                    { "dejudder_regional_hold", "on" }, { "dejudder_pixel_trust", "on" }, { "dejudder_verify", "off" },
                    { "dejudder_landing_check", "on" }, { "dejudder_picture_area", "on" }, { "fix_scene_cooldown", "on" }, { "dither_vectors_new_frame", "on" }, { "native_rgb_ingest", "on" }, { "dejudder_pulldown", "off" },
                    { "dejudder_fade_match", "on" }, { "dejudder_low_res", "all" }, { "dejudder_estimate_ahead", "on" },
                    { "dejudder_block_taper", "off" }, { "dither_vectors", "auto" }, { "adaptive_downscale", "off" },
                    { "adaptive_chroma_ssim", "off" }, { "adaptive_faithful", "polar" }, { "adaptive_grain_map", "on" },
                    { "adaptive_upscale", "off" }, { "adaptive_upscale_superres_scale", "on" }, { "upscale_light", "linear" },
                    { "downscale_light", "linear" }, { "preserve_extended_range", "off" }, { "sdr_tone_shape", "off" } };
                for (const auto& entry : choiceDefaults)
                    if (key == QLatin1String(entry.first)) return QString::fromLatin1(entry.second);
                // Numbers with a placeholder showing their default: empty, so
                // the placeholder (not "Auto") is what an unset field shows.
                if ((key.startsWith(QStringLiteral("dejudder_")) || key.startsWith(QStringLiteral("dither_")) ||
                        key.startsWith(QStringLiteral("adaptive_")) || key == QStringLiteral("sdr_white_headroom") ||
                        key == QStringLiteral("sdr_knee_start") || key == QStringLiteral("sdr_knee_strength")) &&
                    key != QStringLiteral("dejudder_preset"))
                    return QString();
                return QStringLiteral("AUTO");
            }
            return {};
        };
        const bool defaultProfile = list->currentRow() == 0;
        const auto profileValue = [this, deprecatedViewportAlias, sectionPrefix](
            const QString& profileSection, const QString& key) -> QString
        {
            QString configured = value(profileSection, key);
            if (configured.isEmpty() && sectionPrefix == QStringLiteral("vprenderer") &&
                key == QStringLiteral("deband_strength"))
            {
                const QString legacy = value(profileSection, QStringLiteral("deband")).trimmed();
                if (legacy.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0 ||
                    legacy.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
                    configured = QStringLiteral("default");
                else if (legacy.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0 ||
                    legacy.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
                    configured = QStringLiteral("off");
                else if (legacy.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0)
                    configured = QStringLiteral("AUTO");
            }
            const QString alias = deprecatedViewportAlias(key);
            if (configured.isEmpty() && !alias.isEmpty())
                configured = value(profileSection, alias);
            return configured;
        };
		for (const Field& field : *fields)
		{
            QString raw = profileValue(section, field.key);
			const bool calibrationLutSlot =
				sectionPrefix == QStringLiteral("vprenderer.color") &&
				(field.key == QStringLiteral("calibration_lut_bt709") ||
                 field.key == QStringLiteral("calibration_lut_p3_d65") ||
                 field.key == QStringLiteral("calibration_lut_bt2020"));
            if (calibrationLutSlot && !raw.isEmpty())
            {
                const QString normalized = QDir::fromNativeSeparators(raw);
                if (normalized != raw)
                {
                    document_->SetKnown(section.toStdString(), field.key.toStdString().c_str(), normalized.toStdString());
                    raw = normalized;
                    markDirty();
                }
            }
			const bool requiresExplicitValue =
				field.widget->property("requiresExplicitValue").toBool();
			const bool scalingDownscaler =
				sectionPrefix == QStringLiteral("vprenderer.scaling") &&
				field.key == QStringLiteral("downscaler");
			if (scalingDownscaler && !raw.isEmpty())
			{
				const QString normalized = raw.trimmed().toLower();
                if (normalized == QStringLiteral("none") || normalized == QStringLiteral("ewa_lanczos"))
				{
                    // These retired spellings resolve to the quality preset.
                    // Keep Auto explicit instead of inheriting a parent's named filter.
                    document_->SetKnown(section.toStdString(), field.key.toStdString().c_str(), "AUTO");
                    raw = QStringLiteral("AUTO");
					markDirty();
				}
			}
			const bool viewportAspectLimit =
				sectionPrefix == QStringLiteral("vprenderer.zoom") &&
				(field.key == QStringLiteral("crop_narrower_content_aspect_limit") ||
				 field.key == QStringLiteral("crop_wider_content_aspect_limit"));
			if (viewportAspectLimit && !raw.isEmpty())
			{
				AspectRatio ignored;
				std::string parseError;
				if (!AspectRatioParser::Parse(raw.toStdString(), 1.0, 4.0,
					ignored, parseError))
				{
					// Optional limits are best-effort. Clean up any legacy or
					// malformed value on the next save rather than carrying it on.
					document_->RemoveKnown(section.toStdString(),
						field.key.toStdString().c_str());
					raw.clear();
				}
			}
            const bool defaultOnlyRendererField = sectionPrefix == QStringLiteral("vprenderer") &&
                (field.key == QStringLiteral("output_diagnostics") ||
                 field.key == QStringLiteral("diagnostic_disable_shader_cache"));
            const bool defaultOnlyField = defaultOnlyRendererField;
            QString configured = raw;
			if (!requiresExplicitValue && configured.isEmpty() && list->count() > 0)
                configured = defaultProfile ? fallback(field.key) :
                    profileValue(list->item(0)->data(Qt::UserRole).toString(), field.key);
			// Stage 5, as VP reads it: a queue's own Target frames switches off a
			// delay it would inherit, so it shows Off rather than the default's.
			if (sectionPrefix == QStringLiteral("queue") && !defaultProfile && raw.isEmpty() &&
				field.key == QStringLiteral("target_latency_ms") &&
				(!value(section, QStringLiteral("target_frames")).isEmpty() ||
				 !value(section, QStringLiteral("steady_reserve_frames")).isEmpty()))
				configured = QStringLiteral("0");
			if (!requiresExplicitValue && configured.isEmpty())
				configured = fallback(field.key);
            // Display the accepted legacy alias as Latin without rewriting the
            // stored profile value; edits persist only after the user changes it.
            if (sectionPrefix == QStringLiteral("vprenderer.subtitles") &&
                field.key == QStringLiteral("subtitle_script_hint") &&
                configured.compare(QStringLiteral("general"), Qt::CaseInsensitive) == 0)
                configured = QStringLiteral("latin");
            if (defaultOnlyField && !defaultProfile && list->count() > 0)
                configured = value(list->item(0)->data(Qt::UserRole).toString(), field.key, fallback(field.key));
            // greyedOff: a setting that does nothing in the current state
            // (its switch is off) stays greyed (audit 2026-10-14c).
            field.widget->setEnabled((!defaultOnlyField || defaultProfile) &&
                !field.widget->property("derivedSetting").toBool() && !field.widget->property("greyedOff").toBool());
            field.widget->setProperty("profileSection", section);
			if (field.widget->property("requiresHdrFixedMode").toBool())
			{
				auto* mode = findChild<QComboBox*>(controlName(sectionPrefix,
					QStringLiteral("hdr_peak_analysis_mode")));
				field.widget->setEnabled(mode &&
					mode->currentData().toString() == QStringLiteral("fixed"));
			}
			field.widget->setProperty("effectiveValue", configured);
			field.widget->setProperty("inherited",
				!requiresExplicitValue && raw.isEmpty() && !defaultProfile);
			if (requiresExplicitValue && raw.isEmpty())
				field.widget->setToolTip(QStringLiteral(
					"An explicit value is required for this setting."));
			            else
            {
                if (!field.widget->property("settingHelp").isValid())
                    field.widget->setProperty("settingHelp", field.widget->toolTip());
                field.widget->setToolTip(field.widget->property("settingHelp").toString() + QStringLiteral("\n") + (raw.isEmpty() ?
                (defaultProfile ?
                    QStringLiteral("Using the built-in default. Editing creates an explicit setting.") :
                    QStringLiteral("Inherited from the default profile. Editing creates an override.")) :
                    QString()));
            }
            field.widget->style()->unpolish(field.widget);
            field.widget->style()->polish(field.widget);
            if (field.kind == Field::Text)
            {
                QString displayed = configured.compare(
                    QStringLiteral("AUTO"), Qt::CaseInsensitive) == 0 ?
                    QStringLiteral("Auto") : configured;
                if (field.displayScale != 1.0)
                {
                    bool ok = false;
                    const double stored = configured.toDouble(&ok);
                    if (ok)
                        displayed = QString::number(stored * field.displayScale,
                            'g', 12);
                }
                if (auto* tone = ToneMappingControl::For(field.widget)) tone->loadValue(displayed);
                else qobject_cast<QLineEdit*>(field.widget)->setText(displayed);
            }
            else if (field.kind == Field::Integer)
                qobject_cast<QSpinBox*>(field.widget)->setValue(configured.toInt());
            else if (field.kind == Field::SdrGamma)
            {
                const QString mode = configured.toLower();
                const bool retained = mode != QStringLiteral("on") && mode != QStringLiteral("passthrough");
                sdrGammaInherit->setVisible(!defaultProfile);
                sdrGammaInherit->setEnabled(!raw.isEmpty());
                sdrGammaEnabled->setTristate(retained);
                sdrGammaEnabled->setCheckState(retained ? Qt::PartiallyChecked :
                    mode == QStringLiteral("on") ? Qt::Checked : Qt::Unchecked);
                sdrGammaStatus->setText(mode == QStringLiteral("off") ?
                    QStringLiteral("Saved behavior retained: use output transport transfer as SDR input. Click the checkbox to select explicit gamma processing.") :
                    retained ? QStringLiteral("Saved behavior retained: conditional SDR gamma processing. Click the checkbox to select explicit gamma processing.") :
                    mode == QStringLiteral("on") ? QStringLiteral("Compensate for calibrated display gamma to produce the desired SDR gamma when no usable LUT is attached.") :
                    QStringLiteral("No SDR gamma adjustment without a LUT. A usable LUT handles calibration; other processing still applies."));
            }
            else if (field.kind == Field::Boolean)
            {
                auto* check = qobject_cast<QCheckBox*>(field.widget);
                check->setChecked(configuredBooleanValue(configured, false));
            }
            else
            {
                auto* combo = qobject_cast<QComboBox*>(field.widget);
                const int autoIndex = combo->findData(QStringLiteral("AUTO"),
                    Qt::UserRole, Qt::MatchFixedString);
                // A VP Renderer root profile already has an explicit quality
                // policy. Where libplacebo offers AUTO, it is the meaningful
                // default: an additional empty "Use default" choice is both
                // redundant and ambiguous. Keep that omission available only
                // to named profiles, where it means inherit this root value.
                const bool rootRendererExplicitDefault = defaultProfile &&
                    sectionPrefix == QStringLiteral("vprenderer") &&
                    (field.key == QStringLiteral("quality") ||
                     field.key == QStringLiteral("target_primaries"));
                const bool scalingAutoChoice = sectionPrefix == QStringLiteral("vprenderer.scaling") && autoIndex >= 0;
                const bool hasUnspecifiedChoice = combo->count() > 0 &&
                    combo->itemData(0).toString().isEmpty();
                static const QStringList calibratedKeys = { "output_gamma", "output_range", "output_transport_gamma", "sdr_adjust_gamma", "sdr_input_transfer", "display_bit_depth", "hdr_tone_map_target_gamma", "sdr_lut_input_gamma" };
                const bool retiredRootDefault = defaultProfile && (calibratedKeys.contains(field.key) || calibrationLutSlot);
                if (hasUnspecifiedChoice)
                    if (auto* model = qobject_cast<QStandardItemModel*>(combo->model()))
                        model->item(0)->setEnabled(!retiredRootDefault && !(defaultProfile && scalingAutoChoice));
                const bool hideUnspecifiedChoice = (defaultProfile && calibrationLutSlot) || (retiredRootDefault && (!raw.isEmpty() || field.key == QStringLiteral("sdr_lut_input_gamma"))) || rootRendererExplicitDefault ||
                    (defaultProfile && (sectionPrefix == QStringLiteral("vprenderer") || scalingAutoChoice) &&
                        autoIndex >= 0);
                if (auto* view = qobject_cast<QListView*>(combo->view()))
                    view->setRowHidden(0, hasUnspecifiedChoice &&
                        hideUnspecifiedChoice);
                if (calibrationLutSlot)
                    combo->setItemText(combo->findData(QStringLiteral("none")), QStringLiteral("None"));
				const QString inheritedDisplay = calibrationLutSlot
                    ? (configured.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0 ? QStringLiteral("None") : QFileInfo(configured).completeBaseName())
					: (field.key == QStringLiteral("sdr_lut_input_gamma") && configured == QStringLiteral("passthrough") ?
                        QStringLiteral("No gamma conversion") : friendlyChoiceLabel(configured));
                if (hasUnspecifiedChoice)
                {
					if (requiresExplicitValue && raw.isEmpty())
						combo->setItemText(0, QStringLiteral("Required - select gamut"));
					const bool emptyExternalLutSlot =
						calibrationLutSlot &&
						raw.isEmpty() && configured.isEmpty();
					if (emptyExternalLutSlot)
						combo->setItemText(0, QStringLiteral("None"));
					const int externalLutEffectiveIndex = combo->findData(
						configured, Qt::UserRole, Qt::MatchFixedString);
					const bool inheritedMissingExternalLut =
						calibrationLutSlot &&
						raw.isEmpty() && !configured.isEmpty() &&
						(externalLutEffectiveIndex < 0 || combo->itemData(
							externalLutEffectiveIndex, Qt::UserRole + 1).toBool());
					if ((requiresExplicitValue && raw.isEmpty()) || emptyExternalLutSlot)
					{
						// Keep the explicit required placeholder established above.
					}
					else if (inheritedMissingExternalLut)
						combo->setItemText(0, defaultProfile ?
							QStringLiteral("Default - Missing: %1").arg(inheritedDisplay) :
							QStringLiteral("Inherited - Missing: %1").arg(inheritedDisplay));
					else if (raw.isEmpty() && !configured.isEmpty())
                        combo->setItemText(0, defaultProfile ?
                            (retiredRootDefault ? QStringLiteral("When unset: %1") : QStringLiteral("Default: %1")).arg(inheritedDisplay) :
                            QStringLiteral("Inherited: %1").arg(inheritedDisplay));
                    else
                        combo->setItemText(0, defaultProfile ?
                            QStringLiteral("Use default") : QStringLiteral("Use inherited value"));
                }
                if (hasUnspecifiedChoice && scalingAutoChoice && !defaultProfile)
                    combo->setItemText(0, QStringLiteral("Inherit from default profile"));
                const int effectiveIndex = autoIndex >= 0 ? autoIndex :
                    combo->findData(configured, Qt::UserRole, Qt::MatchFixedString);
                int index = raw.isEmpty() ?
                    (hideUnspecifiedChoice || !hasUnspecifiedChoice ? effectiveIndex : 0) :
                    (configured.isEmpty() && hasUnspecifiedChoice ? 0 :
                        combo->findData(configured, Qt::UserRole, Qt::MatchFixedString));
                if (index < 0)
                    for (int candidate = 0; candidate < combo->count(); ++candidate)
                        if (combo->itemData(candidate).toString().compare(
                            configured, Qt::CaseInsensitive) == 0)
                        { index = candidate; break; }
				if (index < 0)
                {
                    const bool legacyAuto = configured.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0;
                    combo->addItem(legacyAuto ? (field.key == QStringLiteral("sdr_input_transfer") ? QStringLiteral("SDR reference: 2.4") : QStringLiteral("Saved automatic policy")) :
                        calibrationLutSlot ? QStringLiteral("Missing: %1").arg(configured) : friendlyChoiceLabel(configured), configured);
                    if (calibrationLutSlot)
                        combo->setItemData(combo->count() - 1, true, Qt::UserRole + 1);
                    if (legacyAuto)
                        if (auto* model = qobject_cast<QStandardItemModel*>(combo->model()))
                            model->item(combo->count() - 1)->setEnabled(false);
                    index = combo->count() - 1;
                }
                combo->setCurrentIndex(index);
            }
        }
        if (desiredSdrGamma) desiredSdrGamma->setEnabled(true);
        if (anamorphicEnabled && anamorphicValue)
        {
            const QString configured = value(section, QStringLiteral("anamorphic_scale"));
            anamorphicEnabled->setChecked(!configured.isEmpty());
            anamorphicValue->setEnabled(!configured.isEmpty());
            if (configured.isEmpty()) anamorphicValue->setText(QStringLiteral("1:1"));
        }
		if (hdrAnalysisMode && pictureOnlyHdrAnalysis &&
			motionCompensatedHdrAnalysis && hdrAnalysisHeight &&
			hdrAnalysisPosition)
		{
			const QString mode = pictureOnlyHdrAnalysis->isChecked()
				? QStringLiteral("fixed")
				: (motionCompensatedHdrAnalysis->isChecked()
					? QStringLiteral("automatic") : QStringLiteral("off"));
			const QSignalBlocker blocker(hdrAnalysisMode);
			hdrAnalysisMode->setCurrentIndex(hdrAnalysisMode->findData(mode));
			hdrAnalysisHeight->setEnabled(mode == QStringLiteral("fixed"));
			hdrAnalysisPosition->setEnabled(mode == QStringLiteral("fixed"));
		}
        if (queuePolicy)
            updateQueuePolicyFromValues();
        updateOutputCompatibility();
        if (sectionPrefix == QStringLiteral("vprenderer.color"))
            if (auto* preset = profileFields->findChild<QComboBox*>("config.vprenderer.color.output_path_profile"))
            {
                const QString rawPreset = preset->currentData().toString();
                if (rawPreset.isEmpty() || rawPreset == "legacy" || rawPreset == "proposed" || rawPreset == "custom")
                {
                    const QSignalBlocker blocker(preset);
                    preset->setCurrentIndex(preset->findData(diagnosticPresetForControls(profileFields)));
                    preset->setToolTip("Derived from the effective diagnostic flags. Selecting a preset writes those flags; a saved preset name alone does not enable them.");
                }
            }
        state->loading = false;
		// The de-judder preset follows the loaded strength (loading blocks the
		// strength field's own change signal).
		if (sectionPrefix == QStringLiteral("vprenderer"))
		{
			auto* preset = profileFields->findChild<QComboBox*>(
				controlName(sectionPrefix, QStringLiteral("dejudder_preset")));
			auto* strength = profileFields->findChild<QLineEdit*>(
				controlName(sectionPrefix, QStringLiteral("dejudder_strength")));
			if (preset && strength)
			{
				QString text = strength->text().trimmed();
				if (text.compare(QStringLiteral("Auto"), Qt::CaseInsensitive) == 0)
					text.clear();   // the default profile shows an unset field as Auto
				bool ok = false;
				const double value = text.toDouble(&ok);
				const int index = text.isEmpty() ? 0 : !ok ? 4 :
					value == 0.0 ? 1 : value == 0.25 ? 2 : value == 0.35 ? 3 : 4;
				const QSignalBlocker blocker(preset);
				preset->setCurrentIndex(index);
			}
		}
        updateToneControls();
		if (sectionPrefix == QStringLiteral("vprenderer.viewport"))
		{
			auto* alignment = profileFields->findChild<QComboBox*>(
				controlName(sectionPrefix, QStringLiteral("vertical_alignment")));
			auto* padding = profileFields->findChild<QLineEdit*>(
				controlName(sectionPrefix, QStringLiteral("screen_edge_padding")));
			if (alignment && padding)
				padding->setEnabled(alignment->currentData().toString() !=
					QStringLiteral("center"));
		}
        refreshLimitedTransportControls();
        refreshCalibrationControls();
        if (sectionPrefix == QStringLiteral("vprenderer") ||
			sectionPrefix == QStringLiteral("vprenderer.scaling") ||
            sectionPrefix == QStringLiteral("vprenderer.color"))
            refreshRendererAutoStatus();
    };

    if (sectionPrefix == QStringLiteral("vprenderer.subtitles")) {
        const QPointer<QListWidget> guardedList=list;
        refreshSubtitleHdrFallback_=[this,state,fields,guardedList,hdrAnalysisMode,
            pictureOnlyHdrAnalysis,motionCompensatedHdrAnalysis,hdrAnalysisHeight,hdrAnalysisPosition] {
            if (!guardedList || state->loading || state->section.isEmpty()) return;
            for (const Field& field : *fields) {
                if (!field.key.startsWith(QStringLiteral("hdr_peak_analysis_"))) continue;
                // Preserve both the current profile's edits and explicit values
                // inherited from the default Subtitle profile.
                if (!value(state->section,field.key).isEmpty() ||
                    (guardedList->count()>0 && !value(guardedList->item(0)->data(Qt::UserRole).toString(),field.key).isEmpty())) continue;
                const QString inherited=legacySubtitleHdrValue(field.key);
                const QSignalBlocker blocker(field.widget);
                if (field.kind==Field::Boolean)
                    qobject_cast<QCheckBox*>(field.widget)->setChecked(configuredBooleanValue(inherited,false));
                else if (field.kind==Field::Text)
                    qobject_cast<QLineEdit*>(field.widget)->setText(inherited);
                else if (field.kind==Field::Choice) {
                    auto* combo=qobject_cast<QComboBox*>(field.widget);
                    combo->setCurrentIndex(combo->findData(inherited));
                }
                field.widget->setProperty("effectiveValue",inherited);
            }
            const QString mode=pictureOnlyHdrAnalysis->isChecked() ? QStringLiteral("fixed") :
                (motionCompensatedHdrAnalysis->isChecked()?QStringLiteral("automatic"):QStringLiteral("off"));
            const QSignalBlocker blocker(hdrAnalysisMode);
            hdrAnalysisMode->setCurrentIndex(hdrAnalysisMode->findData(mode));
            hdrAnalysisHeight->setEnabled(mode==QStringLiteral("fixed"));
            hdrAnalysisPosition->setEnabled(mode==QStringLiteral("fixed"));
        };
        auto* style=profileFields->findChild<QComboBox*>(controlName(sectionPrefix,QStringLiteral("type")));
        connect(style,qOverload<int>(&QComboBox::currentIndexChanged),this,[state,list,loadDetails](int) {
            if(!state->loading) loadDetails(list->currentItem());
        });
    }

    if (sectionPrefix == QStringLiteral("vprenderer.color"))
        if (auto* response = combinedSdrResponse)
            connect(response, qOverload<int>(&QComboBox::activated), this,
                [this, state, list, loadDetails, response](int index)
            {
                if (state->loading || list->currentRow() <= 0 || !document_ ||
                    response->itemData(index).toString() != QStringLiteral("__inherit")) return;
                document_->RemoveKnown(state->section.toStdString(), "sdr_adjust_gamma");
                document_->RemoveKnown(state->section.toStdString(), "sdr_input_transfer");
                markDirty();
                loadDetails(list->currentItem());
            });

    if (sdrGammaInherit)
        connect(sdrGammaInherit, &QPushButton::clicked, this, [this, state, list, loadDetails]()
        {
            if (state->loading || list->currentRow() <= 0 || !document_) return;
            document_->RemoveKnown(state->section.toStdString(), "sdr_adjust_gamma");
            markDirty();
            loadDetails(list->currentItem());
        });

    if (queuePolicy)
    {
        auto* advanced = profileFields->findChild<QToolButton*>(
            QStringLiteral("rendererSection.queueAdvanced"));
        connect(advanced, &QToolButton::toggled, this, [queuePolicy, advanced](bool open)
        {
            if (!open && queuePolicy->currentData().toString().isEmpty())
                advanced->setChecked(true);
        });
        connect(queuePolicy, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, queuePolicy, state, fields, updateQueuePolicyPresentation](int index)
        {
            if (state->loading || state->section.isEmpty() || !document_ || index < 0) return;
            const QString selectedId = queuePolicy->itemData(index).toString();
            if (selectedId.isEmpty())
            {
                updateQueuePolicyPresentation();
                return;
            }
            const QueuePolicy* selected = nullptr;
            for (const QueuePolicy& policy : queuePolicies)
                if (selectedId == QString::fromLatin1(policy.id)) { selected = &policy; break; }
            if (!selected) return;
            const std::string section = state->section.toStdString();
            const auto setValue = [this, &section](const char* key, int value)
            {
                document_->SetKnown(section, key, std::to_string(value));
            };
            setValue("queue_size", selected->capacity);
            setValue("lead_frames", selected->lead);
            setValue("startup_preroll_frames", 0);
            setValue("target_frames", selected->target);
            setValue("target_latency_ms", 0);
            setValue("active_picture_lookahead_frames", 0);
            setValue("reset_after_render_restart_seconds", 5);
            setValue("reset_queue_too_large_percent", 75);
            for (const Field& field : *fields)
                if (field.kind == Field::Integer)
                {
                    int value = 0;
                    if (field.key == QStringLiteral("queue_size")) value = selected->capacity;
                    else if (field.key == QStringLiteral("lead_frames")) value = selected->lead;
                    else if (field.key == QStringLiteral("target_frames")) value = selected->target;
                    else if (field.key == QStringLiteral("reset_after_render_restart_seconds")) value = 5;
                    else if (field.key == QStringLiteral("reset_queue_too_large_percent")) value = 75;
                    QSignalBlocker blocker(field.widget);
                    qobject_cast<QSpinBox*>(field.widget)->setValue(value);
                }
            // A preset switches Target delay off; its note follows.
            if (auto* note = findChild<QLabel*>(QStringLiteral("config.queue.target_latency_note")))
                note->setText(queueLatencyNoteText(0));
            markDirty();
            updateQueuePolicyPresentation();
        });
        for (const Field& field : *fields)
            if (field.kind == Field::Integer)
                connect(qobject_cast<QSpinBox*>(field.widget),
                    qOverload<int>(&QSpinBox::valueChanged), this,
                    [state, updateQueuePolicyFromValues](int)
                {
                    if (state->loading) return;
                    updateQueuePolicyFromValues();
                });
    }

    ProfileListPolicy listPolicy;
    listPolicy.sections = [this, sectionPrefix]
        { return profileSections(sectionPrefix); };
    listPolicy.displayName = [this, sectionPrefix](const QString& section)
    {
        const QString configuredLabel = value(section, QStringLiteral("label"));
        return configuredLabel.isEmpty() ?
            ::displayName(section, sectionPrefix) : configuredLabel;
    };
    listPolicy.addProfile = [this, sectionPrefix]() -> QString
    {
        if (!document_) return {};
        const auto previousTransport = outputTransportSelections();
        const QStringList existing = profileSections(sectionPrefix);
        for (int number = 1; number <= 1000; ++number)
        {
            const QString requested = QStringLiteral("New %1").arg(number);
            const QString candidate = sectionPrefix + u'.' +
                profileIdentifier(requested);
            bool nameInUse = false;
            for (const QString& existingSection : existing)
            {
                const QString configuredLabel = value(existingSection,
                    QStringLiteral("label"));
                const QString existingName = configuredLabel.isEmpty() ?
                    ::displayName(existingSection, sectionPrefix) : configuredLabel;
                nameInUse = nameInUse ||
                    existingName.compare(requested, Qt::CaseInsensitive) == 0;
            }
            if (!nameInUse && !existing.contains(candidate, Qt::CaseInsensitive) &&
                document_->AddSection(candidate.toStdString()))
            {
                if (existing.isEmpty()) seedCalibratedProfile(sectionPrefix, candidate);
                if (sectionPrefix == QStringLiteral("vprenderer.color")) synchronizeLimitedTransportFlags(previousTransport);
                return candidate;
            }
        }
        setStatus(QStringLiteral(
            "Cannot add profile: New 1 through New 1000 are already in use."), true);
        return {};
    };
    listPolicy.removeProfile = [this, sectionPrefix](const QString& section)
    {
        const auto before = outputTransportSelections();
        const bool changed = document_ && document_->RemoveSection(section.toStdString());
        if (changed && sectionPrefix == QStringLiteral("vprenderer.color")) synchronizeLimitedTransportFlags(before);
        return changed;
    };
    listPolicy.normalizeForOrdering = [this, sectionPrefix](const QString& section)
    {
        if (!document_ || section.compare(sectionPrefix,
            Qt::CaseInsensitive) != 0)
            return section;
        const QStringList sections = profileSections(sectionPrefix);
        QString renamed = sectionPrefix + QStringLiteral(".profile_1");
        int suffix = 2;
        while (sections.contains(renamed, Qt::CaseInsensitive))
            renamed = sectionPrefix + QStringLiteral(".profile_%1").arg(suffix++);
        if (!document_->RenameSection(sectionPrefix.toStdString(),
            renamed.toStdString()))
            return section;
        markDirty();
        return renamed;
    };
    listPolicy.moveAfter = [this, sectionPrefix](const QString& section, const QString& previous)
    {
        const auto before = outputTransportSelections();
        const bool changed = document_ && document_->MoveSectionAfter(section.toStdString(), previous.toStdString());
        if (changed && sectionPrefix == QStringLiteral("vprenderer.color")) synchronizeLimitedTransportFlags(before);
        return changed;
    };
    listPolicy.markDirty = [this] { markDirty(); };
    listPolicy.refreshIndicators = [this] { refreshActiveProfileIndicators(); };
    auto* profileList = new ProfileListController(this, this, list, add,
        remove, up, down, std::move(listPolicy));
    profileList->setCurrentChanged(loadDetails);
    std::function<void(const QString&)> refresh = [profileList](const QString& wanted)
        { profileList->refresh(wanted); };
    connect(shortcut, &QLineEdit::textChanged, this, [this, state](const QString& text)
    {
        if (state->loading || state->section.isEmpty()) return;
        if (text.trimmed().isEmpty()) document_->RemoveKnown(state->section.toStdString(), "shortcut");
        else document_->SetKnown(state->section.toStdString(), "shortcut", text.toLocal8Bit().constData());
        markDirty();
    });
    connect(shortcut, &QLineEdit::editingFinished, this, [this, state, shortcut]
    {
        if (state->loading || state->section.isEmpty() || shortcut->text().trimmed().isEmpty()) return;
        std::string canonical;
        if (!RendererProfileConfig::CanonicalizeKeyChord(shortcut->text().toStdString(), canonical))
        {
            QMessageBox::warning(this, QStringLiteral("Shortcut key"),
                QStringLiteral("Use one key with optional Ctrl, Alt, or Shift modifiers (for example Ctrl+F2)."));
            return;
        }
        const QString normalized = QString::fromStdString(canonical);
        if (normalized == shortcut->text()) return;
        shortcut->setText(normalized);
        document_->SetKnown(state->section.toStdString(), "shortcut", canonical);
        markDirty();
    });
    connect(cycleShortcut, &QLineEdit::textChanged, this, [this, state](const QString& text)
    {
        if (state->loading || state->section.isEmpty()) return;
        if (text.trimmed().isEmpty()) document_->RemoveKnown(state->section.toStdString(), "cycle_shortcut");
        else document_->SetKnown(state->section.toStdString(), "cycle_shortcut", text.toLocal8Bit().constData());
        markDirty();
    });
    connect(cycleShortcut, &QLineEdit::editingFinished, this, [this, state, cycleShortcut]
    {
        if (state->loading || state->section.isEmpty() || cycleShortcut->text().trimmed().isEmpty()) return;
        std::string canonical;
        if (!RendererProfileConfig::CanonicalizeKeyChord(cycleShortcut->text().toStdString(), canonical))
        {
            QMessageBox::warning(this, QStringLiteral("Cycle shortcut key"),
                QStringLiteral("Use one key with optional Ctrl, Alt, or Shift modifiers (for example Ctrl+F2)."));
            return;
        }
        const QString normalized = QString::fromStdString(canonical);
        if (normalized == cycleShortcut->text()) return;
        cycleShortcut->setText(normalized);
        document_->SetKnown(state->section.toStdString(), "cycle_shortcut", canonical);
        markDirty();
    });
    connect(useRule, &QCheckBox::toggled, this, [this, state, rule, ruleField](bool enabled)
    {
        ruleField->setVisible(enabled);
        rule->setEnabled(enabled);
        if (state->loading || state->section.isEmpty()) return;
        if (!enabled) document_->RemoveKnown(state->section.toStdString(), "when");
        markDirty();
    });
    connect(rule, &QPlainTextEdit::textChanged, this, [this, state, rule]
    {
        if (state->loading || state->section.isEmpty()) return;
        QString text = rule->toPlainText().trimmed();
        text.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral(" "));
        if (text.isEmpty()) document_->RemoveKnown(state->section.toStdString(), "when");
        else document_->SetKnown(state->section.toStdString(), "when", text.toLocal8Bit().constData());
        markDirty();
    });
    connect(name, &QLineEdit::textChanged, this,
        [state, profileList](const QString& text)
    {
        if (!state->loading) profileList->updateCurrentLabel(text);
    });
    connect(name, &QLineEdit::editingFinished, this, [this, state, name, sectionPrefix, refresh]
    {
        if (state->loading || state->section.isEmpty() || !document_) return;
        const QString requested = name->text().trimmed();
        const QString configuredLabel = value(state->section, QStringLiteral("label"));
        const QString currentName = configuredLabel.isEmpty() ?
            displayName(state->section, sectionPrefix) : configuredLabel;
        if (requested.isEmpty())
        {
            name->setText(currentName);
            QMessageBox::warning(this, QStringLiteral("Profile name"),
                QStringLiteral("Profile names cannot be empty."));
            return;
        }
        for (const QString& existingSection : profileSections(sectionPrefix))
        {
            if (existingSection.compare(state->section, Qt::CaseInsensitive) == 0) continue;
            const QString existingLabel = value(existingSection, QStringLiteral("label"));
            const QString existingName = existingLabel.isEmpty() ? displayName(existingSection, sectionPrefix) : existingLabel;
            if (existingName.compare(requested, Qt::CaseInsensitive) == 0)
            {
                name->setText(currentName);
                QMessageBox::warning(this, QStringLiteral("Profile name"), QStringLiteral("Profile names must be unique."));
                return;
            }
        }
        QString identifier = profileIdentifier(requested);
        if (identifier.size() > 64)
        {
            name->setText(currentName);
            QMessageBox::warning(this, QStringLiteral("Profile name"), QStringLiteral("Profile names must encode to 64 characters or fewer."));
            return;
        }
        QString renamed = sectionPrefix + u'.' + identifier;
        if (renamed == state->section) return;
        const QStringList existing = profileSections(sectionPrefix);
        if (existing.contains(renamed, Qt::CaseInsensitive) &&
            renamed.compare(state->section, Qt::CaseInsensitive) != 0)
        {
            QMessageBox::warning(this, QStringLiteral("Profile name"), QStringLiteral("Another profile already uses that name."));
            name->setText(currentName);
            return;
        }
        const QString old = state->section;
        if (!document_->RenameSection(old.toStdString(), renamed.toStdString())) return;
        if (sectionPrefix == QStringLiteral("vprenderer.viewport") ||
            sectionPrefix == QStringLiteral("vprenderer.zoom") || sectionPrefix == QStringLiteral("vprenderer.subtitles"))
            document_->SetKnown(renamed.toStdString(), "label", requested.toLocal8Bit().constData());
        state->section = renamed;
        markDirty();
        refresh(renamed);
    });
    refresh({});
    if (showsActiveProfile)
    {
        const auto markEditSelection = [this, list]
        {
            if (!profileSelectionReady_ || selectingActiveProfile_) return;
            for (ProfileListBinding& binding : activeProfileLists_)
                if (binding.list == list)
                {
                    binding.editSelectionTouched = true;
                    break;
                }
        };
        connect(list, &QListWidget::currentRowChanged, this,
            [markEditSelection](int) { markEditSelection(); });
        connect(list, &QListWidget::itemClicked, this,
            [markEditSelection](QListWidgetItem*) { markEditSelection(); });
        for (QPushButton* button : { add, remove, up, down })
            connect(button, &QPushButton::pressed, this,
                markEditSelection);
    }

    splitter->addWidget(createCard(QStringLiteral("Profiles"),
        QStringLiteral("The first profile in the list is the default. Add, remove, or reorder profiles here."), listContent));
    auto* detailsCard=createCard(QStringLiteral("Profile details"),
        QStringLiteral("Shortcut and rule come first, followed by profile-specific settings."), detail);
    if(sectionPrefix==QStringLiteral("vprenderer.subtitles"))
        detailsCard->layout()->setSizeConstraint(QLayout::SetMinimumSize);
    splitter->addWidget(detailsCard);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 310, 620 });
    auto* page=createPage(title, description, splitter);
    if(sectionPrefix==QStringLiteral("vprenderer.subtitles")) {
        splitter->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);
        qobject_cast<QScrollArea*>(page)->widget()->layout()->setSizeConstraint(QLayout::SetMinimumSize);
    }
    return page;
}

QWidget* ConfigEditorWindow::createQueuePage()
{
    return createProfilePage(QStringLiteral("Queue"),
        QStringLiteral("Configure ordered queue profiles. Choose a buffering policy for a complete baseline, or tune its advanced values."),
        QStringLiteral("queue"));
}

QWidget* ConfigEditorWindow::createRendererPage()
{
    return createProfilePage(QStringLiteral("Rendering"),
        QStringLiteral("Configure ordered rendering profiles. The first profile in the list is the default."), QStringLiteral("vprenderer"));
}

QWidget* ConfigEditorWindow::createScalingPage()
{
	return createProfilePage(QStringLiteral("Scaling"),
		QStringLiteral("Configure ordered scaling profiles. The first profile in the list is the default."),
		QStringLiteral("vprenderer.scaling"));
}

QWidget* ConfigEditorWindow::createColorConfigPage()
{
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* live = helpLabel(QStringLiteral("Live output unavailable. Saved preferences are not proof of the active output contract."));
    live->setObjectName(QStringLiteral("config.color_output.live_status"));
    layout->addWidget(live);
    auto* family = qobject_cast<QScrollArea*>(createProfilePage(
        QStringLiteral("Profiles"),
        QStringLiteral("One profile selects display color, SDR interpretation, calibration LUTs and output transport. HDR target nits and target black remain in Rendering."),
        QStringLiteral("vprenderer.color")));
    layout->addWidget(family->takeWidget());
    delete family;
    return createPage(QStringLiteral("Color / Output"),
        QStringLiteral("Configure display color and output together in one profile."), body);
}

void ConfigEditorWindow::refreshShaderCacheStatus()
{
	if (remoteClient_)
	{
		if (shaderCacheStatus_) shaderCacheStatus_->setText(
			QStringLiteral("Remote cache status is unavailable."));
		return;
	}
    const bool beta = value(QStringLiteral("general"), QStringLiteral("renderer")).compare(
        QStringLiteral("VP Renderer (Beta)"), Qt::CaseInsensitive) == 0;
    const QDir rendererDirectory(QFileInfo(configPath_).absoluteDir().filePath(
        beta ? QStringLiteral("vprenderer-beta") : QStringLiteral("vprenderer")));
    const QFileInfo cache(rendererDirectory.filePath(
        QStringLiteral("VideoProcessorShaderCache.bin")));
    if (shaderCacheStatus_)
    {
        shaderCacheStatus_->setText(cache.exists() ?
            QStringLiteral("Persistent cache: %1 MB · Updated %2")
                .arg(cache.size() / (1024.0 * 1024.0), 0, 'f', 1)
                .arg(cache.lastModified().toString(QStringLiteral("MMM d, h:mm AP"))) :
            QStringLiteral("Persistent cache: Not created yet"));
    }
}

QWidget* ConfigEditorWindow::createInputProcessingPage(const QString& title,
    const QString& description, const QString& section)
{
    auto* input = new QWidget;
    auto* inputForm = new QFormLayout(input);
    inputForm->setContentsMargins(0, 0, 0, 0);
    inputForm->setVerticalSpacing(8);
    inputForm->addRow(QStringLiteral("Video conversion"), bindChoiceField(section,
        QStringLiteral("video_conversion"),
        { QString(), QStringLiteral("NONE"), QStringLiteral("V210_TO_P010") },
        { QStringLiteral("Inherited / not set"), QStringLiteral("Disabled"),
          QStringLiteral("V210 to P010") }));
    inputForm->addRow(QStringLiteral("Container color space"), bindChoiceField(section,
        QStringLiteral("container_colorspace"),
        { QString(), QStringLiteral("BT2020"), QStringLiteral("P3_D65"), QStringLiteral("P3_DCI"),
          QStringLiteral("P3_D60"), QStringLiteral("REC709"), QStringLiteral("REC601_525"),
          QStringLiteral("REC601_625") },
        { QStringLiteral("Inherited / not set") }));
    inputForm->addRow(QStringLiteral("HDR color space"), bindChoiceField(section,
        QStringLiteral("hdr_colorspace"),
        { QString(), QStringLiteral("FOLLOW_INPUT"), QStringLiteral("FOLLOW_INPUT_LLDV"),
          QStringLiteral("FOLLOW_CONTAINER"), QStringLiteral("BT2020"), QStringLiteral("P3"),
          QStringLiteral("REC709") },
        { QStringLiteral("Inherited / not set"), QStringLiteral("Follow input"),
          QStringLiteral("Follow input (LLDV)"), QStringLiteral("Follow container"),
          QStringLiteral("BT.2020"), QStringLiteral("P3"), QStringLiteral("Rec. 709") }));
    inputForm->addRow(QStringLiteral("HDR luminance"), bindChoiceField(section,
        QStringLiteral("hdr_luminance"),
        { QString(), QStringLiteral("FOLLOW_INPUT"), QStringLiteral("FOLLOW_INPUT_LLDV"),
          QStringLiteral("HDR_LUMINANCE_USER") },
        { QStringLiteral("Inherited / not set"), QStringLiteral("Follow input"),
          QStringLiteral("Follow input (LLDV)"), QStringLiteral("User values") }));

    auto* cards = new ResponsiveCardGrid;
    cards->addCard(createCard(QStringLiteral("Input processing"),
        QStringLiteral("Choose per-renderer overrides, or inherit the General defaults. Changes apply after restart."),
        input));
    return createPage(title, description, cards);
}

QWidget* ConfigEditorWindow::createDirectShowPage()
{
    const QString section = QStringLiteral("directshow");
    auto* cards = new ResponsiveCardGrid;

    auto* timing = new QWidget;
    auto* timingForm = new QFormLayout(timing);
    timingForm->setContentsMargins(0, 0, 0, 0);
    timingForm->setVerticalSpacing(8);
    timingForm->addRow(QStringLiteral("Start/stop method"), bindChoiceField(section,
        QStringLiteral("renderer_start_stop_time_method"),
        { QStringLiteral("CLOCK_SMART"), QStringLiteral("CLOCK_SMART2"),
          QStringLiteral("RATIONAL_RATIONAL"), QStringLiteral("CLOCK_RATIONAL"),
          QStringLiteral("CLOCK_THEO"), QStringLiteral("CLOCK_CLOCK"),
          QStringLiteral("THEO_THEO"), QStringLiteral("CLOCK_NONE"),
          QStringLiteral("THEO_NONE"), QStringLiteral("NONE") },
        { QStringLiteral("Clock Smart"), QStringLiteral("Clock Smart 2"),
          QStringLiteral("Rational / Rational"), QStringLiteral("Clock / Rational"),
          QStringLiteral("Clock / Theoretical"), QStringLiteral("Clock / Clock"),
          QStringLiteral("Theoretical / Theoretical"), QStringLiteral("Clock / None"),
          QStringLiteral("Theoretical / None"), QStringLiteral("None") }));
    QString configuredFrameOffset = value(section, QStringLiteral("frame_offset"));
    if (configuredFrameOffset.isEmpty())
        configuredFrameOffset = value(QStringLiteral("general"), QStringLiteral("frame_offset"), QStringLiteral("AUTO"));
    const bool automaticFrameOffset = configuredFrameOffset.isEmpty() ||
        configuredFrameOffset.compare(QStringLiteral("AUTO"), Qt::CaseInsensitive) == 0;
    auto* frameOffsetRow = new QWidget;
    auto* frameOffsetLayout = new QHBoxLayout(frameOffsetRow);
    frameOffsetLayout->setContentsMargins(0, 0, 0, 0);
    frameOffsetLayout->setSpacing(12);
    auto* frameOffsetAuto = new QCheckBox(QStringLiteral("Auto (90 ms)"));
    frameOffsetAuto->setObjectName(QStringLiteral("config.directshow.frame_offset.auto"));
    frameOffsetAuto->setChecked(automaticFrameOffset);
    auto* frameOffsetValue = new QSpinBox;
    frameOffsetValue->setObjectName(QStringLiteral("config.directshow.frame_offset.value"));
    frameOffsetValue->setAccessibleName(QStringLiteral("Frame offset in milliseconds"));
    frameOffsetValue->setRange(0, INT_MAX);
    frameOffsetValue->setButtonSymbols(QAbstractSpinBox::NoButtons);
    frameOffsetValue->setMaximumWidth(180);
    frameOffsetValue->setValue(automaticFrameOffset ? ConfigurationLiveApply::DefaultDirectShowFrameOffsetMs : configuredFrameOffset.toInt());
    frameOffsetValue->setEnabled(!automaticFrameOffset);
    frameOffsetValue->setFixedWidth(180);
    frameOffsetLayout->addWidget(frameOffsetValue);
    frameOffsetLayout->addWidget(frameOffsetAuto);
    frameOffsetLayout->addStretch();
    connect(frameOffsetAuto, &QCheckBox::toggled, this,
        [this, frameOffsetValue](bool automatic)
    {
        if (!document_) return;
        frameOffsetValue->setEnabled(!automatic);
        if (automatic)
        {
            const QSignalBlocker blocker(frameOffsetValue);
            frameOffsetValue->setValue(ConfigurationLiveApply::DefaultDirectShowFrameOffsetMs);
        }
        document_->RemoveKnown("general", "frame_offset");
        document_->SetKnown("directshow", "frame_offset",
            automatic ? "AUTO" : std::to_string(frameOffsetValue->value()));
        markDirty();
    });
    connect(frameOffsetValue, qOverload<int>(&QSpinBox::valueChanged), this,
        [this, frameOffsetAuto](int offset)
    {
        if (!document_ || frameOffsetAuto->isChecked()) return;
        document_->RemoveKnown("general", "frame_offset");
        document_->SetKnown("directshow", "frame_offset", std::to_string(offset));
        markDirty();
    });
    timingForm->addRow(QStringLiteral("Frame offset (ms)"), frameOffsetRow);

    auto* overrides = new QWidget;
    auto* overrideForm = new QFormLayout(overrides);
    overrideForm->setContentsMargins(0, 0, 0, 0);
    overrideForm->setVerticalSpacing(8);
    overrideForm->addRow(QStringLiteral("Nominal range"), bindChoiceField(section, QStringLiteral("renderer_nominal_range"),
        { QStringLiteral("AUTO"), QStringLiteral("FULL"), QStringLiteral("LIMITED"), QStringLiteral("SMALL") }));
    overrideForm->addRow(QStringLiteral("Transfer function"), bindChoiceField(section, QStringLiteral("renderer_transfer_function"),
        { QStringLiteral("AUTO"), QStringLiteral("PQ"), QStringLiteral("REC709"), QStringLiteral("BT2020_CONST"),
          QStringLiteral("GAMMA_1.8"), QStringLiteral("GAMMA_2.0"), QStringLiteral("GAMMA_2.2"), QStringLiteral("GAMMA_2.6"),
          QStringLiteral("GAMMA_2.8"), QStringLiteral("LINEAR_RGB"), QStringLiteral("204M"), QStringLiteral("8BIT_GAMMA_2.2"),
          QStringLiteral("LOG_100_1"), QStringLiteral("LOG_316_1"), QStringLiteral("BT2020"), QStringLiteral("HYBRID_LOG_GAMMA") }));
    overrideForm->addRow(QStringLiteral("Transfer matrix"), bindChoiceField(section, QStringLiteral("renderer_transfer_matrix"),
        { QStringLiteral("AUTO"), QStringLiteral("BT2020_10"), QStringLiteral("BT2020_12"), QStringLiteral("BT709"),
          QStringLiteral("BT601"), QStringLiteral("240M"), QStringLiteral("FCC"), QStringLiteral("YCGCO") }));
    overrideForm->addRow(QStringLiteral("Primaries"), bindChoiceField(section, QStringLiteral("renderer_primaries"),
        { QStringLiteral("AUTO"), QStringLiteral("BT2020"), QStringLiteral("DCI-P3"), QStringLiteral("BT709"),
          QStringLiteral("NTSC_SYSM"), QStringLiteral("NTSC_SYSBG"), QStringLiteral("CIE1931_ZYX"), QStringLiteral("ACES") }));

    cards->addCard(createCard(QStringLiteral("Timing"),
        QStringLiteral("Timing controls used only by DirectShow renderers."), timing));
    cards->addCard(createCard(QStringLiteral("Renderer overrides"),
        QStringLiteral("DirectShow color overrides. Auto lets the renderer decide."), overrides));
    return createPage(QStringLiteral("General"),
        QStringLiteral("Configure timing and color overrides used only by DirectShow renderers."), cards);
}

QWidget* ConfigEditorWindow::createViewportPage()
{
	return createProfilePage(QStringLiteral("Screen"),
		QStringLiteral("Configure VP Renderer screen geometry and selection. The first profile in the list is the default."),
		QStringLiteral("vprenderer.viewport"));
}

QWidget* ConfigEditorWindow::createZoomPage()
{
	return createProfilePage(QStringLiteral("Zoom"),
		QStringLiteral("Configure independent crop/fill profiles. Zoom shortcuts never change the selected Screen geometry."),
		QStringLiteral("vprenderer.zoom"));
}

QWidget* ConfigEditorWindow::createLldvPage()
{
    return createProfilePage(QStringLiteral("LLDV"),
        QStringLiteral("Configure ordered LLDV metadata profiles shared by every renderer. The first profile is the default."),
        QStringLiteral("lldv"));
}

QWidget* ConfigEditorWindow::createNlsShadersPage()
{
    auto state = std::make_shared<ShaderEditorState>();
    const QString root = QStringLiteral("shader.nls");

    auto* splitter = new ResponsiveSplitter;

    auto* selection = new QWidget;
    auto* selectionLayout = new QVBoxLayout(selection);
    selectionLayout->setContentsMargins(0, 0, 0, 0);
    selectionLayout->setSpacing(10);
    auto* list = new QListWidget;
    list->setAccessibleName(QStringLiteral("NLS profiles"));
    list->setAccessibleDescription(
        QStringLiteral("Configured nonlinear-stretch profiles."));
    list->setObjectName(QStringLiteral("config.shader.nls.modes"));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setDefaultDropAction(Qt::MoveAction);
    list->setDragDropOverwriteMode(false);
    list->setItemDelegate(new ProfileStateItemDelegate(list));
    activeProfileLists_.push_back({ list, QStringLiteral("shader.nls") });
    installProfileContextMenu(list, QStringLiteral("shader.nls"));
    QPushButton* nlsLiveToggle = addLiveToggle(list, QStringLiteral("shader.nls"));
    auto* profileActions = new QHBoxLayout;
    auto* addProfile = new QPushButton(QStringLiteral("+ Add profile"));
    addProfile->setObjectName(QStringLiteral("config.shader.nls.add"));
    addProfile->setAccessibleDescription(
        QStringLiteral("Add a named NLS shader profile."));
    auto* removeProfile = new QPushButton(QStringLiteral("Remove selected"));
    removeProfile->setObjectName(QStringLiteral("config.shader.nls.remove"));
    removeProfile->setProperty("danger", true);
    removeProfile->setAccessibleDescription(
        QStringLiteral("Remove the selected named NLS shader profile."));
    profileActions->addWidget(addProfile);
    profileActions->addWidget(removeProfile);
    profileActions->addStretch();
    selectionLayout->addLayout(profileActions);
    auto* orderActions = new QHBoxLayout;
    auto* moveUp = new QPushButton(QStringLiteral("Move up"));
    auto* moveDown = new QPushButton(QStringLiteral("Move down"));
    moveUp->setObjectName(QStringLiteral("config.shader.nls.move_up"));
    moveDown->setObjectName(QStringLiteral("config.shader.nls.move_down"));
    moveUp->setAccessibleDescription(
        QStringLiteral("Move the selected named NLS profile earlier in the selection order."));
    moveDown->setAccessibleDescription(
        QStringLiteral("Move the selected named NLS profile later in the selection order."));
    orderActions->addWidget(moveUp);
    orderActions->addWidget(moveDown);
    orderActions->addStretch();
    selectionLayout->addLayout(orderActions);
    selectionLayout->addWidget(helpLabel(
        QStringLiteral("A single NLS profile can be active.")));
    selectionLayout->addWidget(list, 1);
    selectionLayout->addWidget(nlsLiveToggle, 0, Qt::AlignLeft);

    QStringList manualSections;
    bool editableSingleGroup = true;
    if (document_)
    {
        const QString groupType = value(root, QStringLiteral("type")).trimmed();
        editableSingleGroup = groupType.isEmpty() ||
            groupType.compare(QStringLiteral("single"), Qt::CaseInsensitive) == 0;
        for (const std::string& section : document_->SectionNamesWithPrefix("shader"))
        {
            const QString name = QString::fromStdString(section);
            const bool directNlsChild = name.startsWith(root + u'.', Qt::CaseInsensitive) &&
                !name.mid(root.size() + 1).contains(u'.');
            const bool standardSection = name.compare(
                QStringLiteral("shader.standard"), Qt::CaseInsensitive) == 0 ||
                name.startsWith(QStringLiteral("shader.standard."), Qt::CaseInsensitive);
            const bool managedNlsProfile = editableSingleGroup && directNlsChild &&
                value(name, QStringLiteral("shader_type")).compare(
                    QStringLiteral("nls"), Qt::CaseInsensitive) == 0;
            if (!managedNlsProfile &&
                name.compare(root, Qt::CaseInsensitive) != 0 && !standardSection)
                manualSections.push_back(name);
        }
    }
    if (!editableSingleGroup)
    {
        auto* warning = helpLabel(QStringLiteral(
            "This NLS group uses advanced multi/effect semantics and is preserved for manual editing."));
        warning->setProperty("warning", true);
        selectionLayout->addWidget(warning);
    }
    if (!manualSections.isEmpty())
    {
        auto* custom = helpLabel(QStringLiteral(
            "%1 custom/manual shader section(s) are preserved unchanged. "
            "Use manual configuration for their program-specific parameters.")
            .arg(manualSections.size()));
        custom->setProperty("warning", true);
        custom->setToolTip(manualSections.join(u'\n'));
        selectionLayout->addWidget(custom);
    }

    auto* details = new QWidget;
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("NLS selection"));
    title->setProperty("cardTitle", true);
    detailsLayout->addWidget(title);

    auto* shortcut = new QLineEdit;
    shortcut->setObjectName(QStringLiteral("config.shader.nls.shortcut"));
    shortcut->setMaximumWidth(280);
    detailsLayout->addWidget(fieldWithHelp(QStringLiteral("Shortcut key"), shortcut,
        QStringLiteral("Optional. Selects this NLS profile.")));
    auto* cycleShortcut = new QLineEdit;
    cycleShortcut->setObjectName(QStringLiteral("config.shader.nls.cycle_shortcut"));
    cycleShortcut->setMaximumWidth(280);
    detailsLayout->addWidget(fieldWithHelp(QStringLiteral("Cycle shortcut key"), cycleShortcut,
        QStringLiteral("Optional. Cycles through NLS modes that use the same key, in list order.")));
    auto* useRule = new QCheckBox(QStringLiteral("Select automatically with a rule"));
    useRule->setObjectName(QStringLiteral("config.shader.nls.use_rule"));
    detailsLayout->addWidget(useRule);
    auto* rule = new QPlainTextEdit;
    rule->setObjectName(QStringLiteral("config.shader.nls.when"));
    rule->setMinimumHeight(72);
    rule->setMaximumHeight(96);
    rule->setMaximumBlockCount(3);
    rule->setPlaceholderText(QStringLiteral("${eotf} == \"HDR\""));
    auto* ruleField = fieldWithHelp(QStringLiteral("Rule"), rule,
        QStringLiteral("Optional source-condition rule. Shortcut and rule are alternatives; either may select this profile."));
    detailsLayout->addWidget(ruleField);

    auto* normalFields = new QWidget;
    auto* normalLayout = new QVBoxLayout(normalFields);
    normalLayout->setContentsMargins(0, 4, 0, 0);
    normalLayout->setSpacing(10);
    auto* label = new QLineEdit;
    label->setObjectName(QStringLiteral("config.shader.nls.label"));
    normalLayout->addWidget(fieldWithHelp(QStringLiteral("Display name"), label,
        QStringLiteral("The friendly name shown for this NLS profile.")));
    detailsLayout->addWidget(normalFields);

    auto* advanced = new QWidget;
    auto* advancedForm = new QFormLayout(advanced);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    advancedForm->setHorizontalSpacing(18);
    advancedForm->setVerticalSpacing(10);
    auto* stage = new QComboBox;
    stage->setObjectName(QStringLiteral("config.shader.nls.stage"));
    stage->setAccessibleName(QStringLiteral("Shader stage"));
    stage->addItem(QStringLiteral("Before resize"), QStringLiteral("pre_resize"));
    stage->addItem(QStringLiteral("After resize"), QStringLiteral("post_resize"));
    auto* hlsl = new QLineEdit;
    auto* glsl = new QLineEdit;
    hlsl->setObjectName(QStringLiteral("config.shader.nls.hlsl_file"));
    glsl->setObjectName(QStringLiteral("config.shader.nls.glsl_file"));
    hlsl->setAccessibleName(QStringLiteral("DirectShow shader file"));
    glsl->setAccessibleName(QStringLiteral("VP Renderer shader file"));
    hlsl->setPlaceholderText(QStringLiteral("Leave blank when unsupported"));
    glsl->setPlaceholderText(QStringLiteral("Leave blank when unsupported"));
    advancedForm->addRow(QStringLiteral("Stage"), stage);
    advancedForm->addRow(QStringLiteral("DirectShow shader file"), hlsl);
    advancedForm->addRow(QStringLiteral("VP Renderer shader file"), glsl);
    detailsLayout->addWidget(advanced);

    const ShaderParameterControls parameterEditor = addShaderParameterControls(
        this, this, detailsLayout, state, [this] { return document_.get(); },
        [this] { markDirty(); }, QStringLiteral("config.shader.nls"),
        QStringLiteral("Shader parameters"), QStringLiteral("shader mode"));
    detailsLayout->addStretch();

    auto setText = [this, state](const char* key, const QString& text)
    {
        if (state->loading || state->section.isEmpty() || !document_) return;
        const QByteArray keyBytes(key);
        if (text.trimmed().isEmpty())
            document_->RemoveKnown(state->section.toStdString(), keyBytes.constData());
        else
        {
            document_->AddSection(state->section.toStdString());
			if ((std::strcmp(key, "hlsl_file") == 0 ||
				std::strcmp(key, "glsl_file") == 0) &&
				value(state->section, QStringLiteral("shader_type")).trimmed().isEmpty())
				document_->SetKnown(state->section.toStdString(), "shader_type", "nls");
            document_->SetKnown(state->section.toStdString(), keyBytes.constData(),
                text.trimmed().toLocal8Bit().constData());
        }
        markDirty();
    };
    for (const auto& field : std::vector<std::pair<QLineEdit*, const char*>>{
        { hlsl, "hlsl_file" }, { glsl, "glsl_file" } })
        connect(field.first, &QLineEdit::textChanged, this,
            [setText, field](const QString& text) { setText(field.second, text); });
    connect(shortcut, &QLineEdit::textChanged, this,
        [setText](const QString& text) { setText("shortcut", text); });
    connect(shortcut, &QLineEdit::editingFinished, this, [this, state, shortcut]
    {
        if (state->loading || state->section.isEmpty() || shortcut->text().trimmed().isEmpty()) return;
        std::string canonical;
        if (!RendererProfileConfig::CanonicalizeKeyChord(shortcut->text().toStdString(), canonical))
        {
            QMessageBox::warning(this, QStringLiteral("Shortcut key"),
                QStringLiteral("Use one key with optional Ctrl, Alt, or Shift modifiers."));
            return;
        }
        const QString normalized = QString::fromStdString(canonical);
        if (normalized != shortcut->text()) shortcut->setText(normalized);
    });
    connect(cycleShortcut, &QLineEdit::textChanged, this,
        [setText](const QString& text) { setText("cycle_shortcut", text); });
    connect(cycleShortcut, &QLineEdit::editingFinished, this, [this, state, cycleShortcut]
    {
        if (state->loading || state->section.isEmpty() || cycleShortcut->text().trimmed().isEmpty()) return;
        std::string canonical;
        if (!RendererProfileConfig::CanonicalizeKeyChord(cycleShortcut->text().toStdString(), canonical))
        {
            QMessageBox::warning(this, QStringLiteral("Cycle shortcut key"),
                QStringLiteral("Use one key with optional Ctrl, Alt, or Shift modifiers."));
            return;
        }
        const QString normalized = QString::fromStdString(canonical);
        if (normalized != cycleShortcut->text()) cycleShortcut->setText(normalized);
    });
    auto setChoice = [this, state](const char* key, QComboBox* combo)
    {
        if (state->loading || state->section.isEmpty() || !document_) return;
        const QString stored = combo->currentData().toString();
        document_->SetKnown(state->section.toStdString(), key,
            (stored.isEmpty() ? combo->currentText().trimmed() : stored)
                .toLocal8Bit().constData());
        markDirty();
    };
    connect(stage, &QComboBox::currentTextChanged, this,
        [setChoice, stage](const QString&) { setChoice("stage", stage); });
    connect(useRule, &QCheckBox::toggled, this, [this, state, rule, ruleField](bool enabled)
    {
        ruleField->setVisible(enabled);
        rule->setEnabled(enabled);
        if (state->loading || state->section.isEmpty() || !document_) return;
        if (!enabled) document_->RemoveKnown(state->section.toStdString(), "when");
        markDirty();
    });
    connect(rule, &QPlainTextEdit::textChanged, this, [this, state, rule]
    {
        if (state->loading || state->section.isEmpty() || !document_) return;
        QString expression = rule->toPlainText().trimmed();
        expression.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral(" "));
        if (expression.isEmpty()) document_->RemoveKnown(state->section.toStdString(), "when");
        else
        {
            document_->AddSection(state->section.toStdString());
            document_->SetKnown(state->section.toStdString(), "when", expression.toLocal8Bit().constData());
        }
        markDirty();
    });

    auto load = [this, state, root, title, shortcut, cycleShortcut, useRule, rule, ruleField,
        normalFields, parameterEditor, advanced, label,
        stage, hlsl, glsl]
        (QListWidgetItem* item)
    {
        state->loading = true;
        state->section = item ? item->data(Qt::UserRole).toString() : QString();
        const bool available = !state->section.isEmpty();
        title->setText(item ? item->text() : QStringLiteral("No NLS profiles are configured"));
        shortcut->setEnabled(available);
        cycleShortcut->setEnabled(available);
        useRule->setEnabled(available);
        normalFields->setVisible(available);
        advanced->setVisible(available);
        if (!available)
        {
            shortcut->clear();
            cycleShortcut->clear();
            useRule->setChecked(false);
            ruleField->setVisible(false);
            parameterEditor.reload();
            parameterEditor.toggle->setVisible(false);
            state->loading = false;
            return;
        }
        shortcut->setText(canonicalShortcutText(
            value(state->section, QStringLiteral("shortcut"))));
        cycleShortcut->setText(canonicalShortcutText(
            value(state->section, QStringLiteral("cycle_shortcut"))));
        const QString expression = value(state->section, QStringLiteral("when"));
        useRule->setChecked(!expression.isEmpty());
        rule->setPlainText(expression);
        ruleField->setVisible(!expression.isEmpty());
        rule->setEnabled(!expression.isEmpty());
        label->setText(value(state->section, QStringLiteral("label")));
        auto loadCombo = [this, state](QComboBox* combo, const QString& key, const QString& fallback)
        {
            const QString configured = value(state->section, key, fallback);
            int index = combo->findData(configured, Qt::UserRole, Qt::MatchFixedString);
            if (index < 0)
                for (int candidate = 0; candidate < combo->count(); ++candidate)
                    if (combo->itemData(candidate).toString().compare(
                        configured, Qt::CaseInsensitive) == 0)
                    { index = candidate; break; }
            if (index < 0) { combo->addItem(friendlyChoiceLabel(configured), configured); index = combo->count() - 1; }
            combo->setCurrentIndex(index);
        };
        loadCombo(stage, QStringLiteral("stage"), QStringLiteral("pre_resize"));
        parameterEditor.reload();
        parameterEditor.toggle->setVisible(available);
        parameterEditor.fields->setVisible(available && parameterEditor.toggle->isChecked());
        // A logical shader member may implement only one renderer. Preserve an
        // omitted backend file as blank rather than silently substituting NLS.
        hlsl->setText(value(state->section, QStringLiteral("hlsl_file")));
        glsl->setText(value(state->section, QStringLiteral("glsl_file")));
        state->loading = false;
    };
    ProfileListPolicy listPolicy;
    listPolicy.sections = [this, root]
    {
        QStringList sections;
        if (!document_) return sections;
        for (const std::string& raw : document_->SectionNamesWithPrefix("shader"))
        {
            const QString section = QString::fromStdString(raw);
            if (section.compare(root, Qt::CaseInsensitive) == 0)
            {
                sections.push_back(section);
                continue;
            }
            if (!section.startsWith(root + u'.', Qt::CaseInsensitive) ||
                section.mid(root.size() + 1).contains(u'.'))
                continue;
            if (value(section, QStringLiteral("shader_type")).compare(
                QStringLiteral("nls"), Qt::CaseInsensitive) == 0)
                sections.push_back(section);
        }
        return sections;
    };
    listPolicy.displayName = [this, root](const QString& section)
    {
        const QString configured = value(section, QStringLiteral("label"));
        return configured.isEmpty() ?
            (section.compare(root, Qt::CaseInsensitive) == 0 ?
                QStringLiteral("Unnamed profile") : ::displayName(section, root)) :
            configured;
    };
    listPolicy.addProfile = [this, root]() -> QString
    {
        if (!document_) return {};
        const QStringList existing = profileSections(root);
        for (int number = 1; number <= 1000; ++number)
        {
            const QString shown = QStringLiteral("New %1").arg(number);
            const QString section = root + u'.' + profileIdentifier(shown);
            if (existing.contains(section, Qt::CaseInsensitive)) continue;
            if (!document_->AddSection(section.toStdString())) return {};
            document_->SetKnown(section.toStdString(), "shader_type", "nls");
            document_->SetKnown(section.toStdString(), "label",
                shown.toLocal8Bit().constData());
            document_->SetKnown(section.toStdString(), "stage", "pre_resize");
            return section;
        }
        return {};
    };
    listPolicy.removeProfile = [this](const QString& section)
    {
        return document_ && document_->RemoveSection(section.toStdString());
    };
    listPolicy.normalizeForOrdering = [this, root](const QString& section)
    {
        if (!document_ || section.compare(root, Qt::CaseInsensitive) != 0)
            return section;
        const QStringList existing = profileSections(root);
        QString renamed = root + QStringLiteral(".profile_1");
        int suffix = 2;
        while (existing.contains(renamed, Qt::CaseInsensitive))
            renamed = root + QStringLiteral(".profile_%1").arg(suffix++);
        if (!document_->RenameSection(root.toStdString(), renamed.toStdString()))
            return section;
        document_->SetKnown(renamed.toStdString(), "shader_type", "nls");
        markDirty();
        return renamed;
    };
    listPolicy.moveAfter = [this](const QString& section, const QString& previous)
    {
        return document_ && document_->MoveSectionAfter(
            section.toStdString(), previous.toStdString());
    };
    listPolicy.markDirty = [this] { markDirty(); };
    listPolicy.refreshIndicators = [this] { refreshActiveProfileIndicators(); };
    listPolicy.removeTitle = QStringLiteral("Remove NLS profile");
    listPolicy.removeQuestion = QStringLiteral(
        "Remove '%1'? This does not delete either shader file.");
    listPolicy.decorateFirstAsDefault = false;
    listPolicy.enabled = editableSingleGroup;
    auto* profileList = new ProfileListController(this, this, list, addProfile,
        removeProfile, moveUp, moveDown, std::move(listPolicy));
    profileList->setCurrentChanged(load);
    connect(label, &QLineEdit::textChanged, this,
        [state, setText, profileList](const QString& text)
    {
        setText("label", text);
        if (!state->loading) profileList->updateCurrentLabel(text);
    });
    profileList->refresh();

    splitter->addWidget(createCard(QStringLiteral("NLS profiles"),
        QStringLiteral("A profile with no shader files is unconfigured (Off). Reorder profiles by dragging or using the buttons; when no shortcut or rule matches, the first profile is selected."), selection));
    splitter->addWidget(createCard(QStringLiteral("Profile details"),
        QStringLiteral("NLS profiles do not inherit shader settings. Clear both shader files to leave the selected profile unconfigured."), details));
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 310, 620 });
    return createPage(QStringLiteral("NLS"),
        QStringLiteral("Configure nonlinear stretch (NLS) profiles. A single NLS profile can be active."), splitter);
}

QWidget* ConfigEditorWindow::createShadersSetupPage()
{
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto* statusContent = new QWidget;
    auto* statusLayout = new QVBoxLayout(statusContent);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(8);
    shaderCacheStatus_ = helpLabel(QStringLiteral("Checking shader cache..."));
    shaderCacheStatus_->setObjectName(QStringLiteral("config.shader.cache.status"));
    statusLayout->addWidget(shaderCacheStatus_);
    layout->addWidget(createCard(QStringLiteral("Shader cache"),
        QStringLiteral("VP Renderer compiles shaders when they are required and stores them here."),
        statusContent));

    auto* controls = new QWidget;
    auto* controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(8);
    auto* buttons = new QHBoxLayout;
    auto* clearCache = new QPushButton(QStringLiteral("Clear shader cache"));
    clearCache->setObjectName(QStringLiteral("config.shader.cache.clear"));
    clearCache->setProperty("danger", true);
    clearCache->setAccessibleDescription(QStringLiteral(
        "Clear the persistent VP Renderer shader cache at the next renderer start."));
	clearCache->setEnabled(!remoteClient_);
	if (remoteClient_) clearCache->setToolTip(
		QStringLiteral("Clear the cache on the VP computer."));
    buttons->addWidget(clearCache);
    buttons->addStretch();
    controlsLayout->addLayout(buttons);
    controlsLayout->addWidget(helpLabel(QStringLiteral(
        "The cache is rebuilt automatically as VP Renderer uses shaders.")));
    layout->addWidget(createCard(QStringLiteral("Cache maintenance"),
        QStringLiteral("Remove the persistent VP Renderer shader cache."),
        controls));
    layout->addStretch();

    connect(clearCache, &QPushButton::clicked, this, [this]
    {
        const QDir configurationDirectory = QFileInfo(configPath_).absoluteDir();
        const bool beta = value(QStringLiteral("general"), QStringLiteral("renderer")).compare(
            QStringLiteral("VP Renderer (Beta)"), Qt::CaseInsensitive) == 0;
        const QString rendererDirectory = configurationDirectory.filePath(
            beta ? QStringLiteral("vprenderer-beta") : QStringLiteral("vprenderer"));
        if (!QDir().mkpath(rendererDirectory))
        {
            setStatus(QStringLiteral("Could not access the VP Renderer cache folder."), true);
            return;
        }
        const QDir cacheDirectory(rendererDirectory);
        const QString cachePath = cacheDirectory.filePath(
            QStringLiteral("VideoProcessorShaderCache.bin"));
        const QString temporaryPath = cachePath + QStringLiteral(".tmp");
        const QString requestPath = cacheDirectory.filePath(
            QStringLiteral("VideoProcessorShaderCache.clear"));
        QFile request(requestPath);
        if (!request.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            request.write("clear\n") < 0)
        {
            setStatus(QStringLiteral("Could not request a shader cache clear."), true);
            return;
        }
        request.close();
        QFile::remove(cachePath);
        QFile::remove(temporaryPath);
        setStatus(QStringLiteral(
            "Shader cache will be cleared at the next VP Renderer start."));
        refreshShaderCacheStatus();
    });
    refreshShaderCacheStatus();
    return createPage(QStringLiteral("Shaders"),
        QStringLiteral("Inspect or clear VP Renderer shader storage."), content);
}

QWidget* ConfigEditorWindow::createStandardShadersPage()
{
    auto state = std::make_shared<ShaderEditorState>();
    const QString root = QStringLiteral("shader.standard");

    auto* splitter = new ResponsiveSplitter;
    auto* selection = new QWidget;
    auto* selectionLayout = new QVBoxLayout(selection);
    selectionLayout->setContentsMargins(0, 0, 0, 0);
    selectionLayout->setSpacing(10);
    auto* list = new QListWidget;
    list->setObjectName(QStringLiteral("config.shader.standard.items"));
    list->setAccessibleName(QStringLiteral("Standard shaders"));
    list->setAccessibleDescription(QStringLiteral(
        "Configured standard shader profiles. Multiple matching profiles compose in list order."));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setDefaultDropAction(Qt::MoveAction);
    list->setDragDropOverwriteMode(false);
    list->setItemDelegate(new ProfileStateItemDelegate(list));
    activeProfileLists_.push_back({ list, root });
    installProfileContextMenu(list, root);
    auto* addShader = new QPushButton(QStringLiteral("+ Add profile"));
    addShader->setObjectName(QStringLiteral("config.shader.standard.add"));
    addShader->setAccessibleDescription(
        QStringLiteral("Add a standard shader profile."));
    auto* removeShader = new QPushButton(QStringLiteral("Remove selected"));
    removeShader->setObjectName(QStringLiteral("config.shader.standard.remove"));
    removeShader->setProperty("danger", true);
    removeShader->setAccessibleDescription(
        QStringLiteral("Remove the selected standard shader profile."));
    auto* shaderButtons = new QHBoxLayout;
    shaderButtons->setContentsMargins(0, 0, 0, 0);
    shaderButtons->addWidget(addShader);
    shaderButtons->addWidget(removeShader);
    shaderButtons->addStretch();
    selectionLayout->addLayout(shaderButtons);
    auto* orderActions = new QHBoxLayout;
    auto* moveUp = new QPushButton(QStringLiteral("Move up"));
    auto* moveDown = new QPushButton(QStringLiteral("Move down"));
    moveUp->setObjectName(QStringLiteral("config.shader.standard.move_up"));
    moveDown->setObjectName(QStringLiteral("config.shader.standard.move_down"));
    moveUp->setAccessibleDescription(
        QStringLiteral("Move the selected named standard shader profile earlier in execution order."));
    moveDown->setAccessibleDescription(
        QStringLiteral("Move the selected named standard shader profile later in execution order."));
    orderActions->addWidget(moveUp);
    orderActions->addWidget(moveDown);
    orderActions->addStretch();
    selectionLayout->addLayout(orderActions);
    selectionLayout->addWidget(helpLabel(
        QStringLiteral("Multiple standard shaders profile can be active.")));
    selectionLayout->addWidget(list, 1);
    selectionLayout->addWidget(addLiveToggle(list, root), 0, Qt::AlignLeft);
    selectionLayout->addWidget(helpLabel(QStringLiteral(
        "Select a shader, then switch it on or off in VP with the button above (a green dot marks shaders on in VP). "
        "Standard profiles do not inherit shader settings. A profile with no shader files is unconfigured (Off). Matching profiles compose in this order.")));
    auto* details = new QWidget;
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("Standard shader details"));
    title->setProperty("cardTitle", true);
    detailsLayout->addWidget(title);
    auto* shortcut = new QLineEdit;
    shortcut->setObjectName(QStringLiteral("config.shader.standard.shortcut"));
    shortcut->setMaximumWidth(280);
    detailsLayout->addWidget(fieldWithHelp(QStringLiteral("Shortcut key"), shortcut,
        QStringLiteral("Optional. Selects this standard shader profile.")));
    auto* useRule = new QCheckBox(QStringLiteral("Select automatically with a rule"));
    useRule->setObjectName(QStringLiteral("config.shader.standard.use_rule"));
    detailsLayout->addWidget(useRule);
    auto* rule = new QPlainTextEdit;
    rule->setObjectName(QStringLiteral("config.shader.standard.when"));
    rule->setMinimumHeight(72);
    rule->setMaximumHeight(96);
    rule->setMaximumBlockCount(3);
    rule->setPlaceholderText(QStringLiteral("${eotf} == \"HDR\""));
    auto* ruleField = fieldWithHelp(QStringLiteral("Rule"), rule,
        QStringLiteral("Optional source-condition rule. Shortcut and rule are alternatives; either may select this profile."));
    detailsLayout->addWidget(ruleField);

    auto* label = new QLineEdit;
    label->setObjectName(QStringLiteral("config.shader.standard.label"));
    detailsLayout->addWidget(fieldWithHelp(QStringLiteral("Display name"), label,
        QStringLiteral("The friendly name shown for this standard shader profile.")));
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    auto* stage = new QComboBox;
    stage->setObjectName(QStringLiteral("config.shader.standard.stage"));
    stage->setAccessibleName(QStringLiteral("Shader stage"));
    stage->addItem(QStringLiteral("Before resize"), QStringLiteral("pre_resize"));
    stage->addItem(QStringLiteral("After resize"), QStringLiteral("post_resize"));
    auto* hlsl = new QLineEdit;
    auto* glsl = new QLineEdit;
    hlsl->setObjectName(QStringLiteral("config.shader.standard.hlsl_file"));
    glsl->setObjectName(QStringLiteral("config.shader.standard.glsl_file"));
    hlsl->setAccessibleName(QStringLiteral("DirectShow shader file"));
    glsl->setAccessibleName(QStringLiteral("VP Renderer shader file"));
    hlsl->setPlaceholderText(QStringLiteral("Leave blank when unsupported"));
    glsl->setPlaceholderText(QStringLiteral("Leave blank when unsupported"));
    form->addRow(QStringLiteral("Stage"), stage);
    form->addRow(QStringLiteral("DirectShow shader file"), hlsl);
    form->addRow(QStringLiteral("VP Renderer shader file"), glsl);
    detailsLayout->addLayout(form);
    auto* unsupported = helpLabel(QStringLiteral(
        "A blank backend file is intentional: that renderer does not support this shader implementation. "
        "Other existing shader sections are preserved unchanged."));
    detailsLayout->addWidget(unsupported);

    // The selected shader's own settings (its //!PARAM blocks): description,
    // range and default from the shader file, saved as param_<name> - the
    // same keys the custom parameter table below shows.
    auto* settingsTitle = new QLabel(QStringLiteral("Shader settings"));
    settingsTitle->setObjectName(QStringLiteral("config.shader.standard.settings_title"));
    auto* settingsBox = new QWidget;
    settingsBox->setObjectName(QStringLiteral("config.shader.standard.settings"));
    auto* settingsForm = new QFormLayout(settingsBox);
    settingsForm->setContentsMargins(0, 0, 0, 0);
    settingsForm->setHorizontalSpacing(18);
    settingsForm->setVerticalSpacing(8);
    auto* settingsNote = helpLabel(QString());
    settingsNote->setObjectName(QStringLiteral("config.shader.standard.settings_note"));
    auto* settingsReset = new QPushButton(QStringLiteral("Reset to shader defaults"));
    settingsReset->setObjectName(QStringLiteral("config.shader.standard.settings_reset"));
    settingsReset->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    detailsLayout->addWidget(settingsTitle);
    detailsLayout->addWidget(settingsBox);
    detailsLayout->addWidget(settingsNote);
    detailsLayout->addWidget(settingsReset, 0, Qt::AlignLeft);
    auto refreshRawParameters = std::make_shared<std::function<void()>>([] {});
    auto reloadSettings = std::make_shared<std::function<void()>>();
    auto shaderFile = [this](const QString& name)
    {
        // VP loads shaders from the shaders folder next to VideoProcessor.exe;
        // Config lives in that folder's config subfolder or beside it.
        const QStringList candidates = {
            QDir(QFileInfo(configPath_).absolutePath()).filePath(QStringLiteral("shaders/") + name),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("shaders/") + name),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../shaders/") + name) };
        for (const QString& candidate : candidates)
            if (QFileInfo::exists(candidate)) return candidate;
        return QString();
    };
    *reloadSettings = [this, state, glsl, settingsTitle, settingsBox, settingsForm, settingsNote,
        settingsReset, refreshRawParameters, shaderFile]
    {
        while (settingsForm->rowCount() > 0) settingsForm->removeRow(0);
        const QString name = glsl->text().trimmed();
        const bool available = !state->section.isEmpty();
        settingsTitle->setVisible(available);
        settingsBox->setVisible(false);
        settingsReset->setVisible(false);
        settingsNote->setVisible(available);
        if (!available) return;
        if (name.isEmpty())
        {
            settingsNote->setText(QStringLiteral("Set the VP Renderer shader file to see its settings."));
            return;
        }
        const QString path = remoteClient_ ? QString() : shaderFile(name);
        bool found = false;
        const QList<GlslShaderParameter> parameters = path.isEmpty() ?
            QList<GlslShaderParameter>() : readGlslShaderParameters(path, found);
        if (!found)
        {
            settingsNote->setText(remoteClient_ ?
                QStringLiteral("The shader files are on the VP computer; use Custom parameters below.") :
                QStringLiteral("%1 was not found in VP's shaders folder.").arg(name));
            return;
        }
        if (parameters.isEmpty())
        {
            settingsNote->setText(QStringLiteral("This shader has no adjustable settings."));
            return;
        }
        settingsNote->setVisible(false);
        settingsBox->setVisible(true);
        settingsReset->setVisible(true);
        for (const GlslShaderParameter& parameter : parameters)
        {
            const QString key = QStringLiteral("param_") + parameter.name;
            bool configured = false;
            const double stored = value(state->section, key).toDouble(&configured);
            const double minimum = parameter.hasMinimum ? parameter.minimum : -1.0e6;
            const double maximum = parameter.hasMaximum ? parameter.maximum : 1.0e6;
            const double current = std::max(minimum, std::min(configured ? stored : parameter.defaultValue, maximum));
            QWidget* control = nullptr;
            const QString objectName = QStringLiteral("config.shader.standard.setting.") + parameter.name;
            const auto save = [this, state, key, refreshRawParameters](const QString& text)
            {
                if (state->loading || state->section.isEmpty() || !document_) return;
                document_->SetKnown(state->section.toStdString(), key.toLocal8Bit().constData(),
                    text.toLocal8Bit().constData());
                markDirty();
                (*refreshRawParameters)();
            };
            // An on/off setting: range 0-1 and a description that says what 0
            // or 1 means ("1 = ...", "0 for ..."). A tick box, not 0.000-1.000.
            const bool toggle = parameter.hasMinimum && parameter.hasMaximum &&
                parameter.minimum == 0.0 && parameter.maximum == 1.0 &&
                QRegularExpression(QStringLiteral("^\\s*[01]\\s*(=|for\\b|when\\b)"))
                    .match(parameter.description).hasMatch();
            if (toggle)
            {
                auto* check = new QCheckBox;
                check->setChecked(current >= 0.5);
                check->setAccessibleName(parameter.description);
                connect(check, &QCheckBox::toggled, this,
                    [save](bool on) { save(on ? QStringLiteral("1") : QStringLiteral("0")); });
                control = check;
            }
            else if (parameter.integer)
            {
                auto* spin = new QSpinBox;
                spin->setRange(static_cast<int>(minimum), static_cast<int>(maximum));
                spin->setValue(static_cast<int>(std::lround(current)));
                connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
                    [save](int number) { save(QString::number(number)); });
                control = spin;
            }
            else
            {
                auto* spin = new QDoubleSpinBox;
                spin->setDecimals(3);
                spin->setRange(minimum, maximum);
                spin->setSingleStep(parameter.hasMinimum && parameter.hasMaximum ?
                    std::max(0.001, (maximum - minimum) / 100.0) : 0.1);
                spin->setValue(current);
                connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [save](double number) { save(QString::number(number, 'g', 6)); });
                control = spin;
            }
            control->setObjectName(objectName);
            control->setMaximumWidth(120);
            control->setToolTip(QStringLiteral("%1 (default %2)").arg(key)
                .arg(parameter.defaultValue, 0, 'g', 6));
            settingsForm->addRow(parameter.description.isEmpty() ? parameter.name : parameter.description, control);
        }
    };
    connect(glsl, &QLineEdit::textChanged, this, [reloadSettings] { (*reloadSettings)(); });
    connect(settingsReset, &QPushButton::clicked, this, [this, state, glsl, reloadSettings,
        refreshRawParameters, shaderFile]
    {
        if (state->section.isEmpty() || !document_) return;
        bool found = false;
        for (const GlslShaderParameter& parameter :
            readGlslShaderParameters(shaderFile(glsl->text().trimmed()), found))
            document_->RemoveKnown(state->section.toStdString(),
                (QStringLiteral("param_") + parameter.name).toLocal8Bit().constData());
        markDirty();
        (*reloadSettings)();
        (*refreshRawParameters)();
    });

    const ShaderParameterControls parameterEditor = addShaderParameterControls(
        this, this, detailsLayout, state, [this] { return document_.get(); },
        [this] { markDirty(); }, QStringLiteral("config.shader.standard"),
        QStringLiteral("Standard shader parameters"), QStringLiteral("standard shader"));
    *refreshRawParameters = parameterEditor.reload;
    auto* empty = helpLabel(QStringLiteral(
        "No standard shader configurations are present. The shipped example configuration includes optional defaults."));
    empty->setProperty("emptyState", true);
    detailsLayout->addWidget(empty);
    detailsLayout->addStretch();

    auto setText = [this, state, root](const char* key, const QString& text)
    {
        if (state->loading || state->section.isEmpty() || !document_) return;
        const QByteArray keyBytes(key);
        if (text.trimmed().isEmpty())
            document_->RemoveKnown(state->section.toStdString(), keyBytes.constData());
        else
        {
            document_->AddSection(state->section.toStdString());
			if (state->section.compare(root, Qt::CaseInsensitive) == 0)
				document_->SetKnown(root.toStdString(), "type", "multi");
			if ((std::strcmp(key, "hlsl_file") == 0 ||
				std::strcmp(key, "glsl_file") == 0) &&
				value(state->section, QStringLiteral("shader_type")).trimmed().isEmpty())
				document_->SetKnown(state->section.toStdString(), "shader_type", "custom");
            document_->SetKnown(state->section.toStdString(), keyBytes.constData(),
                text.trimmed().toLocal8Bit().constData());
        }
        markDirty();
    };
    for (const auto& field : std::vector<std::pair<QLineEdit*, const char*>>{
        { hlsl, "hlsl_file" }, { glsl, "glsl_file" } })
        connect(field.first, &QLineEdit::textChanged, this,
            [setText, field](const QString& text) { setText(field.second, text); });
    connect(shortcut, &QLineEdit::textChanged, this,
        [setText](const QString& text) { setText("shortcut", text); });
    connect(shortcut, &QLineEdit::editingFinished, this, [this, state, shortcut]
    {
        if (state->loading || state->section.isEmpty() || shortcut->text().trimmed().isEmpty()) return;
        std::string canonical;
        if (!RendererProfileConfig::CanonicalizeKeyChord(shortcut->text().toStdString(), canonical))
        {
            QMessageBox::warning(this, QStringLiteral("Shortcut key"),
                QStringLiteral("Use one key with optional Ctrl, Alt, or Shift modifiers."));
            return;
        }
        const QString normalized = QString::fromStdString(canonical);
        if (normalized != shortcut->text()) shortcut->setText(normalized);
    });
    connect(stage, &QComboBox::currentTextChanged, this, [this, state, stage](const QString&)
    {
        if (state->loading || state->section.isEmpty() || !document_) return;
        document_->SetKnown(state->section.toStdString(), "stage",
            stage->currentData().toString().toLocal8Bit().constData());
        markDirty();
    });
    connect(useRule, &QCheckBox::toggled, this, [this, state, rule, ruleField](bool enabled)
    {
        ruleField->setVisible(enabled);
        rule->setEnabled(enabled);
        if (state->loading || state->section.isEmpty() || !document_) return;
        if (!enabled) document_->RemoveKnown(state->section.toStdString(), "when");
        markDirty();
    });
    connect(rule, &QPlainTextEdit::textChanged, this, [this, state, rule]
    {
        if (state->loading || state->section.isEmpty() || !document_) return;
        QString expression = rule->toPlainText().trimmed();
        expression.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral(" "));
        if (expression.isEmpty()) document_->RemoveKnown(state->section.toStdString(), "when");
        else
        {
            document_->AddSection(state->section.toStdString());
            document_->SetKnown(state->section.toStdString(), "when", expression.toLocal8Bit().constData());
        }
        markDirty();
    });

    auto load = [this, state, title, shortcut, useRule, rule, ruleField, label,
        stage, hlsl, glsl, parameterEditor, empty, unsupported, reloadSettings](QListWidgetItem* item)
    {
        state->loading = true;
        state->section = item ? item->data(Qt::UserRole).toString() : QString();
        const bool available = !state->section.isEmpty();
        title->setText(item ? item->text() : QStringLiteral("No standard shaders are configured"));
        const std::vector<QWidget*> controls = {
            shortcut, useRule, rule, label, stage, hlsl, glsl
        };
        for (QWidget* control : controls) control->setEnabled(available);
        unsupported->setVisible(available);
        empty->setVisible(!available);
        if (!available)
        {
            shortcut->clear();
            useRule->setChecked(false);
            rule->clear();
            ruleField->setVisible(false);
            label->clear();
            hlsl->clear();
            glsl->clear();
            parameterEditor.reload();
            (*reloadSettings)();
            state->loading = false;
            return;
        }
        shortcut->setText(canonicalShortcutText(value(state->section, QStringLiteral("shortcut"))));
        const QString expression = value(state->section, QStringLiteral("when"));
        useRule->setChecked(!expression.isEmpty());
        rule->setPlainText(expression);
        ruleField->setVisible(!expression.isEmpty());
        rule->setEnabled(!expression.isEmpty());
        label->setText(value(state->section, QStringLiteral("label")));
        const QString configuredStage = value(state->section, QStringLiteral("stage"), QStringLiteral("pre_resize"));
        int index = stage->findData(configuredStage, Qt::UserRole, Qt::MatchFixedString);
        if (index < 0) { stage->addItem(friendlyChoiceLabel(configuredStage), configuredStage); index = stage->count() - 1; }
        stage->setCurrentIndex(index);
        hlsl->setText(value(state->section, QStringLiteral("hlsl_file")));
        glsl->setText(value(state->section, QStringLiteral("glsl_file")));
        parameterEditor.reload();
        state->loading = false;
        (*reloadSettings)();
    };
    ProfileListPolicy listPolicy;
    listPolicy.sections = [this, root]
    {
        QStringList sections;
        if (!document_) return sections;
        for (const std::string& raw : document_->SectionNamesWithPrefix("shader"))
        {
            const QString section = QString::fromStdString(raw);
            if (section.compare(root, Qt::CaseInsensitive) == 0)
            {
                sections.push_back(section);
                continue;
            }
            if (!section.startsWith(root + u'.', Qt::CaseInsensitive) ||
                section.mid(root.size() + 1).contains(u'.'))
                continue;
            if (value(section, QStringLiteral("shader_type")).compare(
                QStringLiteral("custom"), Qt::CaseInsensitive) == 0)
                sections.push_back(section);
        }
        return sections;
    };
    listPolicy.displayName = [this, root](const QString& section)
    {
        const QString configured = value(section, QStringLiteral("label"));
        return configured.isEmpty() ?
            (section.compare(root, Qt::CaseInsensitive) == 0 ?
                QStringLiteral("Unnamed profile") : ::displayName(section, root)) :
            configured;
    };
    listPolicy.addProfile = [this, root]() -> QString
    {
        if (!document_) return {};
        const QStringList existing = profileSections(root);
        for (int number = 1; number <= 1000; ++number)
        {
            const QString shown = QStringLiteral("New %1").arg(number);
            const QString section = root + u'.' + profileIdentifier(shown);
            if (existing.contains(section, Qt::CaseInsensitive)) continue;
            if (!document_->AddSection(section.toStdString())) return {};
            document_->SetKnown(section.toStdString(), "shader_type", "custom");
            document_->SetKnown(section.toStdString(), "label",
                shown.toLocal8Bit().constData());
            document_->SetKnown(section.toStdString(), "stage", "pre_resize");
            return section;
        }
        return {};
    };
    listPolicy.removeProfile = [this](const QString& section)
    {
        return document_ && document_->RemoveSection(section.toStdString());
    };
    listPolicy.normalizeForOrdering = [this, root](const QString& section)
    {
        if (!document_ || section.compare(root, Qt::CaseInsensitive) != 0)
            return section;
        const QStringList existing = profileSections(root);
        QString renamed = root + QStringLiteral(".profile_1");
        int suffix = 2;
        while (existing.contains(renamed, Qt::CaseInsensitive))
            renamed = root + QStringLiteral(".profile_%1").arg(suffix++);
        if (!document_->RenameSection(root.toStdString(), renamed.toStdString()))
            return section;
        document_->RemoveKnown(renamed.toStdString(), "type");
        document_->SetKnown(renamed.toStdString(), "shader_type", "custom");
        markDirty();
        return renamed;
    };
    listPolicy.moveAfter = [this](const QString& section, const QString& previous)
    {
        return document_ && document_->MoveSectionAfter(
            section.toStdString(), previous.toStdString());
    };
    listPolicy.markDirty = [this] { markDirty(); };
    listPolicy.refreshIndicators = [this] { refreshActiveProfileIndicators(); };
    listPolicy.removeTitle = QStringLiteral("Remove standard shader profile");
    listPolicy.removeQuestion = QStringLiteral(
        "Remove '%1'? This does not delete either shader file.");
    listPolicy.decorateFirstAsDefault = false;
    auto* profileList = new ProfileListController(this, this, list, addShader,
        removeShader, moveUp, moveDown, std::move(listPolicy));
    profileList->setCurrentChanged(load);
    connect(label, &QLineEdit::textChanged, this,
        [state, setText, profileList](const QString& text)
    {
        setText("label", text);
        if (!state->loading) profileList->updateCurrentLabel(text);
    });
    profileList->refresh();

    splitter->addWidget(createCard(QStringLiteral("Standard shaders"),
        QStringLiteral("A profile with no shader files is unconfigured (Off). Reorder profiles by dragging or using the buttons; matching profiles compose in this order, and the first profile is selected when no shortcut or rule matches."), selection));
    splitter->addWidget(createCard(QStringLiteral("Profile details"),
        QStringLiteral("Standard shader profiles do not inherit shader settings. Clear both shader files to leave the selected profile unconfigured."), details));
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 310, 620 });
    return createPage(QStringLiteral("Standard"),
        QStringLiteral("Configure standard shader profiles separately from NLS. Multiple standard shaders profile can be active."), splitter);
}

QWidget* ConfigEditorWindow::createActionsPage()
{
    struct State { QString section; bool loading = false; };
    auto state = std::make_shared<State>();
    auto* splitter = new ResponsiveSplitter;

    auto* listContent = new QWidget;
    auto* listLayout = new QVBoxLayout(listContent);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(8);
    auto* add = new QPushButton(QStringLiteral("+ Add action"));
    add->setObjectName(QStringLiteral("config.actions.add"));
    auto* remove = new QPushButton(QStringLiteral("Remove"));
    remove->setProperty("danger", true);
    auto* list = new QListWidget;
    list->setObjectName(QStringLiteral("config.actions.items"));
    list->setAccessibleName(QStringLiteral("Configured actions"));
    list->setAccessibleDescription(QStringLiteral("Actions that VideoProcessor can run after selected events."));
    installActionContextMenu(list);
    listLayout->addWidget(add);
    listLayout->addWidget(remove);
    listLayout->addWidget(list, 1);

    auto* detailStack = new QStackedWidget;
    detailStack->setObjectName(QStringLiteral("config.actions.details"));
    auto* emptyDetail = new QWidget;
    auto* emptyLayout = new QVBoxLayout(emptyDetail);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    emptyLayout->setSpacing(6);
    auto* emptyTitle = new QLabel(QStringLiteral("No actions configured"));
    emptyTitle->setProperty("cardTitle", true);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(helpLabel(QStringLiteral(
        "Actions are optional. Add an action when you want VideoProcessor to run a command after an event.")));
    emptyLayout->addStretch();
    detailStack->addWidget(emptyDetail);

    auto* detail = new QWidget;
    auto* detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(12);
    auto* selectedTitle = new QLabel(QStringLiteral("Add an action to configure it"));
    selectedTitle->setProperty("cardTitle", true);
    detailLayout->addWidget(selectedTitle);

    auto* name = new QLineEdit;
    name->setObjectName(QStringLiteral("config.actions.name"));
    detailLayout->addWidget(fieldWithHelp(QStringLiteral("Name"), name,
        QStringLiteral("Names must be unique. Spaces are supported.")));

    auto* enabled = new QCheckBox(QStringLiteral("Enable this action"));
    enabled->setObjectName(QStringLiteral("config.actions.enabled"));
    enabled->setAccessibleDescription(QStringLiteral(
        "Disabled actions are saved as drafts and ignored by VideoProcessor."));
    detailLayout->addWidget(enabled);
    detailLayout->addWidget(helpLabel(QStringLiteral(
        "Leave disabled while setting up an incomplete action. VP ignores disabled drafts.")));

    auto* rendererTarget = new QComboBox;
    actionRendererTarget_ = rendererTarget;
    rendererTarget->setObjectName(QStringLiteral("config.actions.renderer"));
    rendererTarget->setEditable(true);
    rendererTarget->setInsertPolicy(QComboBox::NoInsert);
    rendererTarget->addItem(QStringLiteral("VP Renderer (default)"), QStringLiteral("vprenderer"));
    rendererTarget->addItem(QStringLiteral("All renderers"), QStringLiteral("*"));
    const QStringList& renderers = allRenderers_;
    for (int index = 0; index < renderers.size(); ++index)
    {
        if (renderers[index].compare(QStringLiteral("VP Renderer"), Qt::CaseInsensitive) == 0 ||
            renderers[index].compare(QStringLiteral("VideoProcessor Renderer (Alpha)"), Qt::CaseInsensitive) == 0)
            continue;
        rendererTarget->addItem(QStringLiteral("%1 - %2").arg(index + 1).arg(renderers[index]), QString::number(index + 1));
        rendererTarget->setItemData(rendererTarget->count() - 1,
            renderers[index], kRendererNameRole);
    }
    rendererTarget->setCurrentIndex(0);
    rendererTarget->setEditText(rendererTarget->itemText(0));
    detailLayout->addWidget(fieldWithHelp(QStringLiteral("Renderer target"), rendererTarget,
        QStringLiteral("Choose VP Renderer, a discovered renderer, or All renderers.")));

    auto* coalesceRole = new QLineEdit;
    coalesceRole->setObjectName(QStringLiteral("config.actions.coalesce_role"));
    coalesceRole->setPlaceholderText(QStringLiteral("Optional, e.g. Color"));
    detailLayout->addWidget(fieldWithHelp(QStringLiteral("Group"), coalesceRole,
        QStringLiteral("Actions in the same group replace one another while waiting, so only the newest matching action runs. Leave blank to group this action by itself.")));

    auto* delaySeconds = new QSpinBox;
    delaySeconds->setObjectName(QStringLiteral("config.actions.delay_seconds"));
    delaySeconds->setRange(0, 30);
    delaySeconds->setSuffix(QStringLiteral(" seconds"));
    detailLayout->addWidget(fieldWithHelp(QStringLiteral("Delay"), delaySeconds,
        QStringLiteral("Wait before launching this action. Use 0 for immediate actions; a later trigger in the same coalescing role replaces it while waiting.")));

    auto* events = new QListWidget;
    events->setObjectName(QStringLiteral("config.actions.on"));
    events->setAccessibleName(QStringLiteral("Action events"));
    events->setAccessibleDescription(QStringLiteral("Check one or more events that can run this action."));
    events->setMinimumHeight(170);
    struct ActionEventChoice
    {
        const char* token;
        const char* label;
        const char* description;
    };
    const ActionEventChoice supportedEvents[] = {
        { "state.committed", "Any VP state update", "Runs once after VP accepts a complete source and profile snapshot." },
        { "renderer.ready", "Renderer is ready", "Runs when the selected renderer reaches Ready, including after a rebuild." },
        { "profile.changed", "Any active profile changed", "Runs once when one or more effective profile selections change." },
        { "profile.display.changed", "VP Renderer profile changed", "Runs when the selected VP Renderer rendering profile changes." },
        { "profile.color.changed", "Color Config changed", "Runs when the selected Rec.709 or BT.2020 Color Config changes." },
        { "profile.viewport.changed", "Screen Config profile changed", "Runs when the selected VP Renderer Screen Config profile changes." },
        { "profile.zoom.changed", "Zoom profile changed", "Runs when the selected VP Renderer Zoom profile changes without changing Screen geometry." },
        { "profile.queue.changed", "Queue profile changed", "Runs when the selected Queue profile changes." },
        { "profile.lldv.changed", "LLDV configuration changed", "Runs when the effective LLDV configuration changes." },
        { "profile.input.changed", "VP Renderer input settings changed", "Runs when advanced VP Renderer input settings change." },
        { "profile.scaling.changed", "VP Renderer scaling settings changed", "Runs when advanced VP Renderer scaling settings change." },
        { "source.eotf.changed", "Source EOTF changed", "Runs when the stable source EOTF changes." },
        { "source.transfer.changed", "Source transfer changed", "Runs when the stable source transfer characteristic changes." },
        { "source.colorspace.changed", "Source color space changed", "Runs when the reported source color space changes." },
        { "source.primaries.changed", "Source primaries changed", "Runs when the reported source color primaries change." },
        { "source.format.changed", "Source pixel format changed", "Runs when the stable source pixel format changes." },
        { "source.resolution.changed", "Source resolution changed", "Runs when the stable source resolution changes." },
        { "source.width.changed", "Source width changed", "Runs when the stable source width changes." },
        { "source.height.changed", "Source height changed", "Runs when the stable source height changes." },
        { "source.scan.changed", "Source scan mode changed", "Runs when progressive/interlaced scan reporting changes." },
        { "source.interlaced.changed", "Source interlacing changed", "Runs when the stable interlaced state changes." },
        { "source.hdr_metadata.changed", "HDR metadata availability changed", "Runs when source HDR metadata becomes available or unavailable." },
        { "source.source_rate.changed", "Source frame rate changed", "Runs when the stable nominal source frame rate changes." },
        { "source.cadence.changed", "Source cadence changed", "Runs when VP's stable source cadence changes." },
        { "refresh.applied", "Refresh rate switch applied", "VP Renderer changed the Windows display refresh rate." },
        { "refresh.confirmed", "Refresh rate already matched", "VP Renderer confirmed that the display was already at the requested refresh rate." },
        { "refresh.restored", "Previous refresh rate restored", "VP Renderer restored the display refresh rate it found before playback." }
    };
    for (const ActionEventChoice& event : supportedEvents)
    {
        const QString token = QString::fromLatin1(event.token);
        auto* item = new QListWidgetItem(QString::fromLatin1(event.label), events);
        item->setData(Qt::UserRole, token);
        item->setToolTip(QString::fromLatin1(event.description));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    detailLayout->addWidget(fieldWithHelp(QStringLiteral("Run on these events"), events,
        QStringLiteral("Choose one or more moments that may trigger this action. Nothing is selected for a new action; hover an entry for details.")));

    auto* rule = new QPlainTextEdit;
    rule->setObjectName(QStringLiteral("config.actions.when"));
    rule->setMinimumHeight(72);
    rule->setMaximumHeight(96);
    rule->setMaximumBlockCount(3);
    rule->setPlaceholderText(QStringLiteral("${eotf} == \"PQ\""));
    detailLayout->addWidget(fieldWithHelp(QStringLiteral("Only run when (optional)"), rule,
        QStringLiteral("Leave blank to run for every selected event, or add a condition such as ${eotf} == \"PQ\". For Screen Config actions, use its visible name: ${screen_config} == \"Scope\".")));

    auto* command = new QLineEdit;
    command->setObjectName(QStringLiteral("config.actions.run"));
    command->setPlaceholderText(QStringLiteral("C:\\Tools\\action.cmd argument"));
    detailLayout->addWidget(fieldWithHelp(QStringLiteral("Command line"), command,
        QStringLiteral("Required only when enabled. Begin with an .exe, .bat, or .cmd path; arguments may use supported ${variable} placeholders.")));
    detailLayout->addStretch();
    detailStack->addWidget(detail);

    auto actionSections = [this]
    {
        QStringList result;
        if (!document_) return result;
        for (const std::string& raw : document_->SectionNamesWithPrefix("actions"))
        {
            const QString section = QString::fromLocal8Bit(raw.c_str());
            if (section.startsWith(QStringLiteral("actions."), Qt::CaseInsensitive) &&
                !section.mid(8).contains(u'.'))
                result.push_back(section);
        }
        return result;
    };

    auto loadDetails = [this, state, list, remove, detailStack, selectedTitle, name, enabled,
        rendererTarget, coalesceRole, delaySeconds, events, rule, command](QListWidgetItem* current)
    {
        state->loading = true;
        state->section = current ? current->data(Qt::UserRole).toString() : QString();
        const bool available = !state->section.isEmpty();
        remove->setEnabled(available);
        if (!available)
        {
            detailStack->setCurrentIndex(0);
            state->loading = false;
            return;
        }
        detailStack->setCurrentIndex(1);
        const QString shown = displayName(state->section, QStringLiteral("actions"));
        selectedTitle->setText(shown);
        name->setText(shown);
        enabled->setChecked(configuredBooleanValue(
            value(state->section, QStringLiteral("enabled")), true));
        const QString configuredRenderer = value(state->section, QStringLiteral("renderer"), QStringLiteral("vprenderer"));
        int rendererIndex = rendererTarget->findData(configuredRenderer, Qt::UserRole, Qt::MatchFixedString);
        if (rendererIndex < 0)
        {
            rendererTarget->addItem(configuredRenderer, configuredRenderer);
            rendererIndex = rendererTarget->count() - 1;
        }
        rendererTarget->setCurrentIndex(rendererIndex);
        rendererTarget->setEditText(rendererTarget->itemText(rendererIndex));
        coalesceRole->setText(value(state->section, QStringLiteral("coalesce_role")));
        bool validDelay = false;
        int configuredDelay = value(state->section,
            QStringLiteral("delay_seconds")).toInt(&validDelay);
        if (!validDelay)
        {
            configuredDelay = value(QStringLiteral("general"),
                QStringLiteral("event_action_delay_seconds"),
                QStringLiteral("5")).toInt(&validDelay);
            if (!validDelay) configuredDelay = 5;
        }
        delaySeconds->setValue(qBound(0, configuredDelay, 30));
        const QStringList configuredEvents = value(state->section, QStringLiteral("on")).split(u',', Qt::SkipEmptyParts);
        QListWidgetItem* firstChecked = nullptr;
        for (int index = 0; index < events->count(); ++index)
        {
            bool checked = false;
            for (const QString& configured : configuredEvents)
                if (configured.trimmed().compare(
                    events->item(index)->data(Qt::UserRole).toString(), Qt::CaseInsensitive) == 0)
                { checked = true; break; }
            events->item(index)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            if (checked && !firstChecked) firstChecked = events->item(index);
        }
        if (firstChecked)
        {
            events->setCurrentItem(firstChecked);
            events->scrollToItem(firstChecked, QAbstractItemView::PositionAtCenter);
        }
        else
        {
            events->setCurrentItem(nullptr);
            events->scrollToTop();
        }
        rule->setPlainText(value(state->section, QStringLiteral("when")));
        command->setText(value(state->section, QStringLiteral("run")));
        state->loading = false;
    };

    std::function<void(const QString&)> refresh = [list, actionSections, loadDetails](const QString& wanted)
    {
        list->clear();
        int selected = 0;
        const QStringList sections = actionSections();
        for (int index = 0; index < sections.size(); ++index)
        {
            auto* item = new QListWidgetItem(displayName(sections[index], QStringLiteral("actions")));
            item->setData(Qt::UserRole, sections[index]);
            item->setSizeHint(QSize(0, 36));
            list->addItem(item);
            if (sections[index] == wanted) selected = index;
        }
        if (list->count() == 0) loadDetails(nullptr);
        else list->setCurrentRow(selected);
    };

    connect(list, &QListWidget::currentItemChanged, this,
        [loadDetails](QListWidgetItem* current) { loadDetails(current); });
    connect(enabled, &QCheckBox::toggled, this, [this, state](bool checked)
    {
        if (state->loading || state->section.isEmpty()) return;
        if (checked) document_->RemoveKnown(state->section.toStdString(), "enabled");
        else document_->SetKnown(state->section.toStdString(), "enabled", "false");
        markDirty();
    });
    connect(rendererTarget, qOverload<int>(&QComboBox::activated), this,
        [this, state, rendererTarget](int index)
    {
        if (state->loading || state->section.isEmpty() || index < 0) return;
        const QString selected = rendererTarget->itemData(index).toString();
        if (selected == QStringLiteral("vprenderer")) document_->RemoveKnown(state->section.toStdString(), "renderer");
        else document_->SetKnown(state->section.toStdString(), "renderer", selected.toStdString());
        markDirty();
    });
    connect(rendererTarget->lineEdit(), &QLineEdit::editingFinished, this,
        [this, state, rendererTarget]
    {
        if (state->loading || state->section.isEmpty()) return;
        QString selected = rendererTarget->currentText().trimmed();
        const int current = rendererTarget->currentIndex();
        if (current >= 0 && selected == rendererTarget->itemText(current))
            selected = rendererTarget->itemData(current).toString();
        if (selected.isEmpty() || selected.compare(QStringLiteral("vprenderer"), Qt::CaseInsensitive) == 0)
            document_->RemoveKnown(state->section.toStdString(), "renderer");
        else document_->SetKnown(state->section.toStdString(), "renderer", selected.toStdString());
        markDirty();
    });
    connect(coalesceRole, &QLineEdit::textChanged, this,
        [this, state](const QString& text)
    {
        if (state->loading || state->section.isEmpty()) return;
        const QString role = text.trimmed();
        if (role.isEmpty())
            document_->RemoveKnown(state->section.toStdString(), "coalesce_role");
        else
            document_->SetKnown(state->section.toStdString(), "coalesce_role",
                role.toStdString());
        markDirty();
    });
    connect(delaySeconds, qOverload<int>(&QSpinBox::valueChanged), this,
        [this, state](int seconds)
    {
        if (state->loading || state->section.isEmpty()) return;
        document_->SetKnown(state->section.toStdString(), "delay_seconds",
            std::to_string(seconds));
        markDirty();
    });
    connect(events, &QListWidget::itemChanged, this, [this, state, events](QListWidgetItem*)
    {
        if (state->loading || state->section.isEmpty()) return;
        QStringList selected;
        for (int index = 0; index < events->count(); ++index)
            if (events->item(index)->checkState() == Qt::Checked)
                selected.push_back(events->item(index)->data(Qt::UserRole).toString());
        if (selected.isEmpty()) document_->RemoveKnown(state->section.toStdString(), "on");
        else document_->SetKnown(state->section.toStdString(), "on", selected.join(u',').toStdString());
        markDirty();
    });
    connect(rule, &QPlainTextEdit::textChanged, this, [this, state, rule]
    {
        if (state->loading || state->section.isEmpty()) return;
        QString text = rule->toPlainText().trimmed();
        text.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral(" "));
        if (text.isEmpty()) document_->RemoveKnown(state->section.toStdString(), "when");
        else document_->SetKnown(state->section.toStdString(), "when", text.toStdString());
        markDirty();
    });
    connect(command, &QLineEdit::textChanged, this, [this, state](const QString& text)
    {
        if (state->loading || state->section.isEmpty()) return;
        if (text.trimmed().isEmpty()) document_->RemoveKnown(state->section.toStdString(), "run");
        else document_->SetKnown(state->section.toStdString(), "run", text.toStdString());
        markDirty();
    });
    connect(name, &QLineEdit::editingFinished, this, [this, state, name, actionSections, refresh]
    {
        if (state->loading || state->section.isEmpty()) return;
        const QString requested = name->text().trimmed();
        if (requested.isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("Action name"), QStringLiteral("Action names cannot be empty."));
            return;
        }
        const QString identifier = profileIdentifier(requested);
        if (identifier.size() > 64)
        {
            QMessageBox::warning(this, QStringLiteral("Action name"), QStringLiteral("Action names must encode to 64 characters or fewer."));
            return;
        }
        const QString renamed = QStringLiteral("actions.") + identifier;
        if (actionSections().contains(renamed, Qt::CaseInsensitive) &&
            renamed.compare(state->section, Qt::CaseInsensitive) != 0)
        {
            QMessageBox::warning(this, QStringLiteral("Action name"), QStringLiteral("Another action already uses that name."));
            return;
        }
        if (renamed == state->section || !document_->RenameSection(state->section.toStdString(), renamed.toStdString())) return;
        state->section = renamed;
        markDirty();
        refresh(renamed);
    });
    connect(add, &QPushButton::clicked, this, [this, actionSections, refresh, command]
    {
        bool ok = false;
        const QString requested = QInputDialog::getText(this, QStringLiteral("Add action"),
            QStringLiteral("Action name"), QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || requested.isEmpty()) return;
        const QString identifier = profileIdentifier(requested);
        if (identifier.size() > 64) return;
        QString section = QStringLiteral("actions.") + identifier;
        const QStringList existing = actionSections();
        int suffix = 2;
        while (existing.contains(section, Qt::CaseInsensitive))
            section = QStringLiteral("actions.") + identifier + QString::number(suffix++);
        if (!document_->AddSection(section.toStdString())) return;
        document_->SetKnown(section.toStdString(), "enabled", "false");
        markDirty();
        refresh(section);
        command->setFocus();
    });
    connect(remove, &QPushButton::clicked, this, [this, state, refresh]
    {
        if (state->section.isEmpty() || QMessageBox::question(this, QStringLiteral("Remove action"),
            QStringLiteral("Remove this action?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        document_->RemoveSection(state->section.toStdString());
        markDirty();
        refresh({});
    });
    refresh({});

    splitter->addWidget(createCard(QStringLiteral("Actions"),
        QStringLiteral("Actions have no default or priority. Disabled actions are retained as drafts."), listContent));
    splitter->addWidget(createCard(QStringLiteral("Action details"),
        QStringLiteral("Choose where and when an action runs, plus the command to start."), detailStack));
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 310, 620 });
    return createPage(QStringLiteral("Actions"),
        QStringLiteral("Run external commands when selected VideoProcessor events occur."), splitter);
}

QWidget* ConfigEditorWindow::createLedDrivePage()
{
    // Plain [led_drive.Default] / [projector.Default] settings (Stage 7). Only
    // the core choices are here; everything else comes from the built-in
    // profile (the beta_22 script's curve and tables) or an LED profile file.
    // VP applies changes on Apply without restarting the LED module unless it
    // starts or stops driving the projector.
    const QString led = QStringLiteral("led_drive.default");
    const QString projector = QStringLiteral("projector.default");
    auto* cards = new ResponsiveCardGrid;

    // The projector's LED cannot go below 60 with LPW1, so neither can these.
    auto levelControl = [this, led](const QString& key, int fallback)
    {
        auto* spin = new QSpinBox;
        spin->setObjectName(controlName(led, key));
        spin->setRange(60, 255);
        spin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
        spin->setMaximumWidth(108);
        spin->setAlignment(Qt::AlignRight);
        bool valid = false;
        const int configured = value(led, key).toInt(&valid);
        spin->setValue(valid ? std::max(60, std::min(configured, 255)) : fallback);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, led, key](int level)
        {
            if (!document_) return;
            const std::string section = led.toStdString();
            if (!document_->SetKnown(section, key.toStdString().c_str(), std::to_string(level)))
            {
                document_->AddSection(section);
                document_->SetKnown(section, key.toStdString().c_str(), std::to_string(level));
            }
            markDirty();
        });
        return spin;
    };

    auto* drive = new QWidget;
    auto* driveForm = new QFormLayout(drive);
    driveForm->setContentsMargins(0, 0, 0, 0);
    driveForm->setVerticalSpacing(8);
    auto* enabled = bindCheckField(QStringLiteral("LED drive on"), led, QStringLiteral("enabled"), false);
    enabled->setAccessibleDescription(QStringLiteral(
        "Lets VideoProcessor drive the projector's LED. Off: nothing is sent to the projector."));
    driveForm->addRow(QString(), enabled);
    // The LED core (owner 2026-10-14): one place that holds the port, sends
    // and logs, fed by a chosen command source, with two switches. Older
    // configurations name only `mode`; the source shown is read from it, and
    // choosing a source writes command_source (and the matching mode).
    QComboBox* ledSource = nullptr;
    QCheckBox* ledAdaptive = nullptr;
    QLabel* compensateWarning = nullptr;
    QWidget* aheadRow = nullptr;
    QWidget* aheadHelp = nullptr;
    std::vector<QWidget*> pacingRows;
    {
        // An older file's retired source (command_source: python, mode:
        // listen) shows, and runs, VP's copy of the script (owner 2026-10-15).
        const QStringList sources = { QStringLiteral("internal_script"), QStringLiteral("compensate"),
            QStringLiteral("none") };
        auto* source = new QComboBox;
        ledSource = source;
        source->setObjectName(controlName(led, QStringLiteral("command_source")));
        source->setAccessibleName(QStringLiteral("Command source"));
        source->addItem(QStringLiteral("VP's copy of your script"), sources[0]);
        source->addItem(QStringLiteral("Compensate (VP's own controller)"), sources[1]);
        source->addItem(QStringLiteral("No LED commands"), sources[2]);
        const QString legacyMode = value(led, QStringLiteral("mode")).trimmed().toLower();
        QString configuredSource = value(led, QStringLiteral("command_source")).trimmed().toLower();
        // An older file names `mode` and no command_source; a new file names
        // neither (audit 2026-10-14c: only the former keeps the old switches).
        const bool olderFile = configuredSource.isEmpty() && !legacyMode.isEmpty();
        if (configuredSource.isEmpty())
            configuredSource = legacyMode == QStringLiteral("listen") || legacyMode == QStringLiteral("script") ? sources[0] :
                legacyMode == QStringLiteral("compensate") ? sources[1] : sources[2];
        if (configuredSource == QStringLiteral("python"))
            configuredSource = sources[0];
        source->setCurrentIndex(std::max(0, static_cast<int>(sources.indexOf(configuredSource))));
        driveForm->addRow(QStringLiteral("Command source"), source);
        driveForm->addRow(QString(), helpLabel(QStringLiteral(
            "Where the LED and contrast commands come from. VP holds the RS-232 port, sends them and logs them. "
            "VP's copy of your dimming script makes the script's decisions, in its rhythm. No LED "
            "commands with adaptive gamma on: only VP's picture adjustments, the port left free.")));
        // Adaptive gamma, shown as the file behaves: named; off for a new
        // file; for an older mode-only file, on where the picture adapted
        // there (compensate, picture only, script smoothing or a shadow lift).
        const QString adaptiveText = value(led, QStringLiteral("adaptive_gamma")).trimmed().toLower();
        const bool shadowLiftOn = value(led, QStringLiteral("shadow_lift_ansi")).toDouble() > 0.0;
        const bool smoothingOn = value(led, QStringLiteral("script_smoothing")).trimmed().toLower() == QStringLiteral("true");
        const bool adaptiveNow = !adaptiveText.isEmpty() ?
            (adaptiveText == QStringLiteral("true") || adaptiveText == QStringLiteral("on") || adaptiveText == QStringLiteral("1")) :
            olderFile && (legacyMode == QStringLiteral("compensate") || legacyMode == QStringLiteral("picture_only") ||
                ((legacyMode == QStringLiteral("script") || legacyMode == QStringLiteral("listen")) && smoothingOn) || shadowLiftOn);
        auto* adaptive = configuredCheck(QStringLiteral("Adaptive gamma"), QString(), adaptiveNow);
        ledAdaptive = adaptive;
        adaptive->setObjectName(controlName(led, QStringLiteral("adaptive_gamma")));
        adaptive->setEnabled(true);
        driveForm->addRow(QString(), adaptive);
        driveForm->addRow(QString(), helpLabel(QStringLiteral(
            "Off (the default): VP leaves the picture alone - exactly the projector's own steps. On: VP eases the "
            "picture across each light and contrast step, and lifts the darks next to large highlights (the shadow "
            "lift's strength, below).")));
        compensateWarning = helpLabel(QStringLiteral(
            "Compensate with adaptive gamma off: the LED still dims in dark scenes, but VP does not brighten the "
            "picture to make up for it, so dark scenes look darker. Tick adaptive gamma for Compensate's normal "
            "behaviour."));
        compensateWarning->setObjectName(controlName(led, QStringLiteral("compensate_warning")));
        driveForm->addRow(QString(), compensateWarning);
        auto* ahead = bindCheckField(QStringLiteral("Look-ahead"), led, QStringLiteral("lookahead"), true);
        aheadRow = ahead;
        driveForm->addRow(QString(), ahead);
        aheadHelp = helpLabel(QStringLiteral(
            "VP sees the coming frames and times each change for when the LED gets there. Off: it reacts to the "
            "frame on screen."));
        driveForm->addRow(QString(), aheadHelp);
        // Owner 2026-10-15: the script copy in the original script's own
        // rhythm (the default) or a fixed loop.
        auto* pacing = bindChoiceField(led, QStringLiteral("script_pacing"),
            { QStringLiteral("script"), QStringLiteral("tick") },
            { QStringLiteral("The script's own rhythm"), QStringLiteral("Fixed loop") });
        pacing->setToolTip(QStringLiteral(
            "How often VP's copy of the script decides. The script's own rhythm (the default): its measuring "
            "time, then its commands with their pauses, then the next loop at once - the same commands at the "
            "same moments as your original script. Fixed loop: every 400 ms."));
        driveForm->addRow(QStringLiteral("Script pacing"), pacing);
        pacingRows.push_back(pacing);
        auto pacingSpin = [this, led, driveForm](const char* key, int maximum, int fallback, const QString& label,
            const QString& tip)
        {
            auto* spin = new QSpinBox;
            spin->setObjectName(controlName(led, QString::fromLatin1(key)));
            spin->setRange(0, maximum);
            spin->setSingleStep(10);
            spin->setSuffix(QStringLiteral(" ms"));
            spin->setMaximumWidth(120);
            spin->setAlignment(Qt::AlignRight);
            spin->setToolTip(tip);
            bool valid = false;
            const int configured = value(led, QString::fromLatin1(key)).toInt(&valid);
            spin->setValue(valid ? std::max(0, std::min(configured, maximum)) : fallback);
            const std::string name = key;
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, led, name](int ms)
            {
                if (!document_) return;
                const std::string section = led.toStdString();
                if (!document_->SetKnown(section, name.c_str(), std::to_string(ms)))
                {
                    document_->AddSection(section);
                    document_->SetKnown(section, name.c_str(), std::to_string(ms));
                }
                markDirty();
            });
            driveForm->addRow(label, spin);
            return spin;
        };
        pacingRows.push_back(pacingSpin("script_measure_ms", 2000, 150, QStringLiteral("Script's measuring time"),
            QStringLiteral("How long your original script took to measure the picture (its screenshot): part of its rhythm. "
                "Default 150.")));
        pacingRows.push_back(pacingSpin("script_echo_ms", 1000, 30, QStringLiteral("Script's command time"),
            QStringLiteral("How long each of the script's commands took to send (its echo to the port). Default 30.")));
        // What every source change writes: the source, the matching mode for
        // older builds (no LED commands: picture only when adaptive gamma is
        // on), and adaptive gamma as shown, so nothing changes behind it.
        auto writeSource = [this, led, source, adaptive]()
        {
            if (!document_) return;
            const QString chosen = source->currentData().toString();
            const std::string section = led.toStdString();
            if (!document_->SetKnown(section, "command_source", chosen.toStdString()))
            {
                document_->AddSection(section);
                document_->SetKnown(section, "command_source", chosen.toStdString());
            }
            const char* mode = chosen == QStringLiteral("internal_script") ? "script" :
                chosen == QStringLiteral("compensate") ? "compensate" :
                adaptive->isChecked() ? "picture_only" : "log_only";
            document_->SetKnown(section, "mode", mode);
            document_->SetKnown(section, "adaptive_gamma", adaptive->isChecked() ? "true" : "false");
            markDirty();
        };
        // A pick writes even when it is the source already shown (an older
        // file is converted; "No LED commands" can be chosen on a new file).
        connect(source, qOverload<int>(&QComboBox::currentIndexChanged), this, [writeSource](int) { writeSource(); });
        connect(source, qOverload<int>(&QComboBox::activated), this, [writeSource](int) { writeSource(); });
        connect(adaptive, &QCheckBox::toggled, this, [this, led, source, writeSource](bool checked)
        {
            if (!document_) return;
            if (value(led, QStringLiteral("command_source")).trimmed().isEmpty())
            {
                // An older file without a source: the box alone.
                document_->SetKnown(led.toStdString(), "adaptive_gamma", checked ? "true" : "false");
                markDirty();
            }
            else
                writeSource();   // keeps the matching mode (picture only / log only)
            (void)source;
        });
    }
    // Compensate's own settings: shown only when it is the command source
    // (audit 2026-10-14b: show only what the chosen source uses).
    std::vector<QWidget*> compensateRows;
    auto* floor = levelControl(QStringLiteral("led_floor"), 100);
    driveForm->addRow(QStringLiteral("Lowest LED level"), floor);
    compensateRows.push_back(floor);
    compensateRows.push_back(helpLabel(QStringLiteral(
        "How far the LED may dim in dark scenes, 60 (darkest) to 255 (never dims), when VP can see the "
        "coming frames (Normal queue).")));
    driveForm->addRow(QString(), compensateRows.back());
    auto* floorReactive = levelControl(QStringLiteral("led_floor_reactive"), 100);
    driveForm->addRow(QStringLiteral("Lowest LED level without look-ahead"), floorReactive);
    compensateRows.push_back(floorReactive);
    compensateRows.push_back(helpLabel(QStringLiteral(
        "Used when VP cannot see ahead (Low Latency queue): it cannot prepare for a bright scene, so it "
        "should not sit as low. Your script used 100.")));
    driveForm->addRow(QString(), compensateRows.back());
    auto* floorBlack = levelControl(QStringLiteral("led_floor_black"), 60);
    driveForm->addRow(QStringLiteral("Lowest level in fades to black"), floorBlack);
    compensateRows.push_back(floorBlack);
    // A fade can dim further than a scene, never less far.
    floorBlack->setMaximum(floor->value());
    connect(floor, qOverload<int>(&QSpinBox::valueChanged), floorBlack,
        [floorBlack](int level) { floorBlack->setMaximum(level); });
    auto* highlightProtection = bindChoiceField(led,
        QStringLiteral("highlight_tolerance_stops"),
        { QStringLiteral("0.33"), QStringLiteral("0.15"), QStringLiteral("0.6") },
        { QStringLiteral("Medium"), QStringLiteral("Strong (keep highlights; the LED dims less)"),
          QStringLiteral("Light (the LED dims more; highlights compress more)") });
    driveForm->addRow(QStringLiteral("Highlight protection"), highlightProtection);
    compensateRows.push_back(highlightProtection);
    driveForm->addRow(QStringLiteral("Gamma boost"), bindChoiceField(led, QStringLiteral("gamma_boost"),
        { QStringLiteral("off"), QStringLiteral("2.4") },
        { QStringLiteral("Off (plain output gamma)"),
          QStringLiteral("2.4 (the look of the old output gamma 2.4)") }));
    // The projector's light exponent on the final code, which Compensate
    // linearises with (compensation_eotf). A number, because it is whatever a
    // light meter says (design review 3, SF-1); 2.41 is the FL35's light-meter
    // fit (delta 4). The gamma boost no longer depends on it.
    auto* projectorGamma = new QDoubleSpinBox;
    projectorGamma->setObjectName(controlName(led, QStringLiteral("compensation_eotf")));
    projectorGamma->setAccessibleName(QStringLiteral("Projector gamma"));
    projectorGamma->setRange(1.0, 3.0);
    projectorGamma->setDecimals(2);
    projectorGamma->setSingleStep(0.01);
    projectorGamma->setMaximumWidth(120);
    projectorGamma->setAlignment(Qt::AlignRight);
    {
        bool valid = false;
        const double configured = value(led, QStringLiteral("compensation_eotf")).toDouble(&valid);
        projectorGamma->setValue(valid ? std::max(1.8, std::min(configured, 3.0)) : 2.41);
    }
    connect(projectorGamma, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
        [this, led](double exponent)
    {
        if (!document_) return;
        const std::string section = led.toStdString();
        const std::string text = QString::number(exponent, 'f', 2).toStdString();
        if (!document_->SetKnown(section, "compensation_eotf", text))
        {
            document_->AddSection(section);
            document_->SetKnown(section, "compensation_eotf", text);
        }
        markDirty();
    });
    driveForm->addRow(QStringLiteral("Projector gamma"), projectorGamma);
    driveForm->addRow(QString(), helpLabel(QStringLiteral(
        "Projector gamma is how the projector turns the final signal into light; Compensate needs it to keep the "
        "picture's brightness exact while the LED dims. 2.41 is the FL35's light-meter measurement; change it "
        "only if a new measurement says otherwise. It does not change the gamma boost.\n"
        "The gamma boost applies even with the LED drive off. Switching the LED drive on or off returns the "
        "projector to full light; other changes carry on from the current level.")));

    // Experimental: deeper fade to black with the projector's custom LED dim.
    // Off by default; its options stay greyed out until it is switched on.
    auto* experiment = new QWidget;
    auto* experimentForm = new QFormLayout(experiment);
    experimentForm->setContentsMargins(0, 0, 0, 0);
    experimentForm->setVerticalSpacing(8);
    auto saveLed = [this, led](const QString& key, const QString& text)
    {
        if (!document_) return;
        const std::string section = led.toStdString();
        if (!document_->SetKnown(section, key.toStdString().c_str(), text.toLocal8Bit().constData()))
        {
            document_->AddSection(section);
            document_->SetKnown(section, key.toStdString().c_str(), text.toLocal8Bit().constData());
        }
        markDirty();
    };
    const QString configuredMethod = value(led, QStringLiteral("ftb_custom_dim")).trimmed().toLower();
    const bool experimentOn = configuredMethod == QStringLiteral("together") ||
        configuredMethod == QStringLiteral("turns");
    auto* experimentEnabled = new QCheckBox(QStringLiteral("Experimental: deeper fade to black (custom LED dim)"));
    experimentEnabled->setObjectName(QStringLiteral("config.led_drive.default.ftb_experimental"));
    experimentEnabled->setChecked(experimentOn);
    experimentForm->addRow(QString(), experimentEnabled);
    auto* method = new QComboBox;
    method->setObjectName(QStringLiteral("config.led_drive.default.ftb_custom_dim"));
    method->addItem(QStringLiteral("All three colours together"), QStringLiteral("together"));
    method->addItem(QStringLiteral("Colours taking turns in small steps"), QStringLiteral("turns"));
    method->setCurrentIndex(configuredMethod == QStringLiteral("turns") ? 1 : 0);
    experimentForm->addRow(QStringLiteral("Method"), method);
    auto numberSetting = [this, led, saveLed](const QString& key, int minimum, int maximum, int fallback,
        const QString& suffix)
    {
        auto* spin = new QSpinBox;
        spin->setObjectName(controlName(led, key));
        spin->setRange(minimum, maximum);
        spin->setMaximumWidth(120);
        spin->setAlignment(Qt::AlignRight);
        if (!suffix.isEmpty()) spin->setSuffix(suffix);
        bool valid = false;
        const int configured = value(led, key).toInt(&valid);
        spin->setValue(valid ? std::max(minimum, std::min(configured, maximum)) : fallback);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
            [saveLed, key](int number) { saveLed(key, QString::number(number)); });
        return spin;
    };
    auto* stepMs = numberSetting(QStringLiteral("ftb_step_ms"), 50, 1000, 100, QStringLiteral(" ms"));
    experimentForm->addRow(QStringLiteral("Step interval"), stepMs);
    // How far each colour moves per command when taking turns: smaller keeps
    // the three colours closer (less colour shift) but takes longer.
    auto* turnStep = numberSetting(QStringLiteral("ftb_turn_step"), 1, 50, 2, QStringLiteral(" levels"));
    experimentForm->addRow(QStringLiteral("Step size per colour"), turnStep);
    // The FL35's custom LED dim runs 0-40 per colour (40 is about 40/255, below
    // LPW1's 60; 0 is off).
    auto* customStart = numberSetting(QStringLiteral("ftb_custom_start"), 1, 40, 40, QString());
    experimentForm->addRow(QStringLiteral("Starting custom level (1-40)"), customStart);
    auto* customFloor = numberSetting(QStringLiteral("ftb_custom_floor"), 0, 39, 0, QString());
    experimentForm->addRow(QStringLiteral("Deepest custom level (0 = off)"), customFloor);
    customFloor->setMaximum(customStart->value() - 1);
    connect(customStart, qOverload<int>(&QSpinBox::valueChanged), customFloor,
        [customFloor](int start) { customFloor->setMaximum(start - 1); });
    experimentForm->addRow(QString(), helpLabel(QStringLiteral(
        "When the picture has been black for half a second and the LED is at its "
        "fade-to-black level, VP switches the projector to its custom LED dim and steps the red, green and "
        "blue LEDs down, one command per step interval; at the first light it switches back. \"Taking turns\" "
        "keeps the three colours within one small step of each other, so the black does not change colour. "
        "The custom dim runs 0-40 (40 is about 40/255, a small step down from LPW1's 60; 0 turns the LEDs "
        "off, true black).")));
    auto syncExperiment = [experimentEnabled, method, stepMs, turnStep, customStart, customFloor]
    {
        const bool on = experimentEnabled->isChecked();
        for (QWidget* control : std::vector<QWidget*>{ method, stepMs, customStart, customFloor })
            control->setEnabled(on);
        // The step size only matters when the colours take turns.
        turnStep->setEnabled(on && method->currentData().toString() == QStringLiteral("turns"));
    };
    syncExperiment();
    connect(experimentEnabled, &QCheckBox::toggled, this, [syncExperiment, saveLed, method](bool on)
    {
        syncExperiment();
        saveLed(QStringLiteral("ftb_custom_dim"), on ? method->currentData().toString() : QStringLiteral("off"));
    });
    connect(method, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [experimentEnabled, saveLed, method, syncExperiment](int)
    {
        syncExperiment();
        if (experimentEnabled->isChecked())
            saveLed(QStringLiteral("ftb_custom_dim"), method->currentData().toString());
    });

    auto* profile = new QWidget;
    auto* profileLayout = new QVBoxLayout(profile);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    profileLayout->setSpacing(8);
    auto* profileRow = new QHBoxLayout;
    auto* profilePath = bindTextField(led, QStringLiteral("profile_file"));
    profilePath->setPlaceholderText(QStringLiteral("Built-in (your dimming script's values)"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"));
    browse->setObjectName(QStringLiteral("config.led_drive.default.profile_browse"));
    auto* builtIn = new QPushButton(QStringLiteral("Use built-in"));
    builtIn->setObjectName(QStringLiteral("config.led_drive.default.profile_builtin"));
    profileRow->addWidget(profilePath, 1);
    profileRow->addWidget(browse);
    profileRow->addWidget(builtIn);
    profileLayout->addLayout(profileRow);
    profileLayout->addWidget(helpLabel(QStringLiteral(
        "Optional. A profile file can change the light curve, step tables, ramp speeds "
        "and the other LED details; anything it leaves out uses the built-in value, and the settings on this page "
        "always win. Projector settings (connection, LED response time, flux table) are not read from a profile. "
        "See docs\\cinema\\LED-PROFILE-GUIDE.md.")));
    connect(browse, &QPushButton::clicked, this, [this, profilePath]
    {
        const QString start = QFileInfo(configPath_).absolutePath();
        const QString chosen = QFileDialog::getOpenFileName(this, QStringLiteral("LED profile"),
            start, QStringLiteral("LED profiles (*.cfg *.txt);;All files (*)"));
        if (chosen.isEmpty()) return;
        // Next to the configuration: store just the name, so the folder can move.
        const QString relative = QDir(start).relativeFilePath(chosen);
        profilePath->setText(relative.startsWith(QStringLiteral("..")) ? chosen : relative);
    });
    connect(builtIn, &QPushButton::clicked, profilePath, [profilePath] { profilePath->clear(); });
    // Audit round 2: a broken profile no longer stops VP (it is left out),
    // so say here when VP would leave it out.
    auto* profileStatus = helpLabel(QString());
    profileStatus->setObjectName(QStringLiteral("config.led_drive.default.profile_status"));
    profileLayout->addWidget(profileStatus);
    const auto checkProfile = [this, profileStatus](const QString& text)
    {
        const QString name = text.trimmed();
        if (name.isEmpty() || remoteClient_)
        {
            profileStatus->setVisible(false);
            return;
        }
        const QString full = QFileInfo(name).isAbsolute() ? name :
            QDir(QFileInfo(configPath_).absolutePath()).filePath(name);
        std::string problem;
        const bool ok = LedDrive::ValidateLedProfileFile(
            QDir::toNativeSeparators(full).toLocal8Bit().toStdString(), problem);
        profileStatus->setText(ok ? QStringLiteral("Profile file OK.") :
            QStringLiteral("VP will ignore this profile and use the built-in values: %1")
                .arg(QString::fromLocal8Bit(problem.c_str())));
        profileStatus->setVisible(true);
    };
    connect(profilePath, &QLineEdit::textChanged, this, checkProfile);
    checkProfile(profilePath->text());
    browse->setEnabled(!remoteClient_);
    if (remoteClient_)
        browse->setToolTip(QStringLiteral("The profile file must be on the VP computer; type its path."));

    auto* connection = new QWidget;
    auto* connectionForm = new QFormLayout(connection);
    connectionForm->setContentsMargins(0, 0, 0, 0);
    connectionForm->setVerticalSpacing(8);
    // VP's default is no projector (audit 2026-10-14c: an unset protocol
    // showed Barco while VP opened nothing); picking a source that drives
    // the projector switches it to Barco (below).
    auto* protocolChoice = bindChoiceField(projector, QStringLiteral("protocol"),
        { QStringLiteral("none"), QStringLiteral("barco_ascii") },
        { QStringLiteral("No projector (never open a port)"), QStringLiteral("Barco serial (RS232)") });
    connectionForm->addRow(QStringLiteral("Protocol"), protocolChoice);
    auto* port = bindTextField(projector, QStringLiteral("port"), QStringLiteral("COM1"));
    port->setMaximumWidth(140);
    connectionForm->addRow(QStringLiteral("COM port"), port);
    // How long the projector takes to change the light after a command; the
    // picture gain is timed to land with it. Tuned by eye on scene changes.
    auto* latency = new QSpinBox;
    latency->setObjectName(controlName(projector, QStringLiteral("led_latency_ms")));
    latency->setRange(0, 2000);
    latency->setSingleStep(10);
    latency->setSuffix(QStringLiteral(" ms"));
    latency->setMaximumWidth(120);
    latency->setAlignment(Qt::AlignRight);
    {
        bool valid = false;
        const int configured = value(projector, QStringLiteral("led_latency_ms")).toInt(&valid);
        latency->setValue(valid ? std::max(0, std::min(configured, 2000)) : 80);
    }
    connect(latency, qOverload<int>(&QSpinBox::valueChanged), this, [this, projector](int ms)
    {
        if (!document_) return;
        const std::string section = projector.toStdString();
        if (!document_->SetKnown(section, "led_latency_ms", std::to_string(ms)))
        {
            document_->AddSection(section);
            document_->SetKnown(section, "led_latency_ms", std::to_string(ms));
        }
        markDirty();
    });
    connectionForm->addRow(QStringLiteral("LED response time"), latency);
    connectionForm->addRow(QString(), helpLabel(QStringLiteral(
        "How long the light takes to change after VP sends a command; the picture gain is timed to land with "
        "it. On a dark-to-bright cut: a glow or lifted blacks just before the cut - lower it; the bright scene "
        "starts too dark or dips - raise it. Change in steps of 20 ms. Default 80.")));
    // The same for the projector's contrast (CNTR): script mode's smoothing
    // times VP's gain with it.
    auto* contrastLatency = new QSpinBox;
    contrastLatency->setObjectName(controlName(projector, QStringLiteral("cntr_latency_ms")));
    contrastLatency->setRange(0, 2000);
    contrastLatency->setSingleStep(10);
    contrastLatency->setSuffix(QStringLiteral(" ms"));
    contrastLatency->setMaximumWidth(120);
    contrastLatency->setAlignment(Qt::AlignRight);
    {
        bool valid = false;
        const int configured = value(projector, QStringLiteral("cntr_latency_ms")).toInt(&valid);
        contrastLatency->setValue(valid ? std::max(0, std::min(configured, 500)) : 80);
    }
    connect(contrastLatency, qOverload<int>(&QSpinBox::valueChanged), this, [this, projector](int ms)
    {
        if (!document_) return;
        const std::string section = projector.toStdString();
        if (!document_->SetKnown(section, "cntr_latency_ms", std::to_string(ms)))
        {
            document_->AddSection(section);
            document_->SetKnown(section, "cntr_latency_ms", std::to_string(ms));
        }
        markDirty();
    });
    connectionForm->addRow(QStringLiteral("Contrast response time"), contrastLatency);
    connectionForm->addRow(QString(), helpLabel(QStringLiteral(
        "How long the projector's contrast takes to change after a command. Used to time VP's picture "
        "adjustment with each contrast step (adaptive gamma, and Compensate): if the picture briefly dips or "
        "brightens at a contrast step, adjust it in steps of 20 ms. Default 80.")));
    auto* readBack = bindCheckField(QStringLiteral("Read the projector's replies"), projector,
        QStringLiteral("read_back"), false);
    connectionForm->addRow(QString(), readBack);
    connectionForm->addRow(QString(), helpLabel(QStringLiteral(
        "Off: send only, exactly like the old script. On: wait for each reply, ask the projector its range and "
        "log lost commands. 19200 baud, 8 data bits, no parity, 1 stop bit, no flow control.")));
    // RS-232 connection test: LED 60, read back, 5 s, LED 255, read back.
    auto* testButton = new QPushButton(QStringLiteral("Test RS-232 connection"));
    testButton->setObjectName(controlName(projector, QStringLiteral("connection_test")));
    testButton->setToolTip(QStringLiteral(
        "Sets the LED to 60, reads it back, waits 5 seconds, sets 255 and reads it back (about 8 s). Uses the "
        "saved settings (Apply first). With VideoProcessor running it pauses the LED drive for the test; it is "
        "refused while video is playing. Without VideoProcessor, Config opens the port itself."));
    auto* testResult = new QLabel;
    testResult->setObjectName(controlName(projector, QStringLiteral("connection_test.result")));
    testResult->setWordWrap(true);
    testResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    connectionForm->addRow(testButton, testResult);
    connect(testButton, &QPushButton::clicked, this, [this, testButton, testResult]
    {
        if (noTarget_) return;
        const bool remote = remoteClient_ != nullptr;
        const QString host = remote ? remoteHost_ : QStringLiteral("127.0.0.1");
        const quint16 rpcPort = remote ? remotePort_ : 41686;
        const std::string configPath = configPath_.toStdString();
        testButton->setEnabled(false);
        testResult->setStyleSheet(QString());
        testResult->setText(dirty_ ?
            QStringLiteral("Testing with the saved settings (unapplied changes are not used)… about 8 s") :
            QStringLiteral("Testing… about 8 s"));
        struct Outcome { bool pass = false; bool refused = false; QString summary, details; };
        const auto outcome = std::make_shared<Outcome>();
        QThread* worker = QThread::create([outcome, host, rpcPort, remote, configPath]
        {
            ConfigurationRpcClient client(host.toStdString(), rpcPort);
            ConfigurationRpcClient::ProjectorTestResult rpc;
            std::string error;
            if (client.RunProjectorTest(rpc, error))
            {
                outcome->pass = rpc.pass;
                outcome->refused = rpc.refused;
                outcome->summary = QString::fromStdString(rpc.summary);
                outcome->details = QString::fromStdString(rpc.details);
                return;
            }
            // Config opens the port itself only when no VP runs on this
            // computer. A VP that runs but does not answer (busy, a test
            // already running, its RPC off) keeps the port and its playback
            // refusal: show its error (audit stage8-rs232 item 2).
            if (remote || LocalVideoProcessorRunning())
            {
                outcome->summary = QStringLiteral("FAIL: ") + QString::fromStdString(error);
                return;
            }
            ConfigFile config;
            if (configPath.empty() || !config.Load(configPath))
            {
                outcome->summary = QStringLiteral("FAIL: could not read the configuration");
                return;
            }
            const LedDrive::ConnectionTestResult result = LedDrive::RunConnectionTestOnNewConnection(
                LedDrive::ReadProjectorConfig(config), [] { return LedDrive::CreateWin32SerialPort(); });
            outcome->pass = result.pass;
            outcome->summary = QString::fromStdString(result.summary) +
                QStringLiteral(" (tested by Config; VideoProcessor is not running)");
            for (const std::string& line : result.lines)
                outcome->details += QString::fromStdString(line) + QLatin1Char('\n');
        });
        worker->setParent(this);
        connect(worker, &QThread::finished, this, [worker, outcome, testButton, testResult]
        {
            worker->deleteLater();
            testButton->setEnabled(true);
            testResult->setText(outcome->summary);
            testResult->setToolTip(outcome->details.trimmed());
            testResult->setStyleSheet(outcome->pass ? QStringLiteral("color: #2e7d32;") :
                outcome->refused ? QString() : QStringLiteral("color: #c62828;"));
        });
        worker->start();
    });

    // Stage 7 step 7c (control policy; the owner's correction): compensate's
    // contrast assist, cuts, speeds and the projector's response. Every
    // setting has a control; a speed set to "preset" is removed from the
    // file so rate_preset applies.
    auto numberControl = [this](const QString& section, const QString& key, double minimum, double maximum,
        int decimals, double step, double fallback, const QString& special)
    {
        auto* spin = new QDoubleSpinBox;
        spin->setObjectName(controlName(section, key));
        spin->setAccessibleName(accessibleSettingName(key));
        spin->setDecimals(decimals);
        spin->setSingleStep(step);
        spin->setRange(special.isEmpty() ? minimum : minimum - step, maximum);
        if (!special.isEmpty()) spin->setSpecialValueText(special);
        spin->setMaximumWidth(120);
        spin->setAlignment(Qt::AlignRight);
        bool valid = false;
        const double configured = value(section, key).toDouble(&valid);
        spin->setValue(valid ? std::max(minimum, std::min(configured, maximum)) :
            (special.isEmpty() ? fallback : spin->minimum()));
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, section, key, spin, decimals](double number)
        {
            if (!document_) return;
            const std::string name = section.toStdString();
            if (!spin->specialValueText().isEmpty() && number <= spin->minimum())
                document_->RemoveKnown(name, key.toStdString().c_str());
            else
            {
                const std::string text = QString::number(number, 'f', decimals).toStdString();
                if (!document_->SetKnown(name, key.toStdString().c_str(), text))
                {
                    document_->AddSection(name);
                    document_->SetKnown(name, key.toStdString().c_str(), text);
                }
            }
            markDirty();
        });
        return spin;
    };

    // Delta 11: the boost's toe and the scene-adaptive shadow lift.
    auto* boostKnee = numberControl(led, QStringLiteral("gamma_boost_knee"), 0.0, 0.5, 2, 0.01, 0.0, QString());
    boostKnee->setToolTip(QStringLiteral(
        "Keep the darkest tones out of the gamma boost: below this part of the signal (0.10 = the bottom tenth) "
        "the picture keeps its native contrast, so dark hair and shadows are not lifted flat. 0 = the boost "
        "everywhere (as before)."));
    driveForm->addRow(QStringLiteral("Boost spares the darks below"), boostKnee);
    auto* boostKneeWidth = numberControl(led, QStringLiteral("gamma_boost_knee_width"), 0.0, 0.5, 2, 0.01, 0.05, QString());
    boostKneeWidth->setToolTip(QStringLiteral(
        "How gradually the boost comes in above that point (part of the signal; 0 = a hard switch). "
        "Only matters when the setting above is not 0."));
    driveForm->addRow(QStringLiteral("Boost blend width"), boostKneeWidth);
    if (auto* boostChoice = drive->findChild<QComboBox*>(controlName(led, QStringLiteral("gamma_boost"))))
    {
        // The toe rows do nothing without the boost (audit 2026-10-14c).
        auto greyBoost = [boostChoice, boostKnee, boostKneeWidth]()
        {
            const QString chosen = boostChoice->currentData().toString();
            const bool on = !chosen.isEmpty() && chosen != QStringLiteral("off");
            boostKnee->setEnabled(on);
            boostKneeWidth->setEnabled(on);
        };
        connect(boostChoice, qOverload<int>(&QComboBox::currentIndexChanged), this, [greyBoost](int) { greyBoost(); });
        greyBoost();
    }
    auto* shadowLift = numberControl(led, QStringLiteral("shadow_lift_ansi"), 0.0, 1.0, 2, 0.05, 0.0, QString());
    shadowLift->setToolTip(QStringLiteral(
        "When something bright shares a dark frame, the lens scatters a veil of light over the darks. This lifts "
        "the darks in proportion to that veil so their texture stays visible; a dark frame with nothing bright "
        "is left alone. 0 = off; 0.3 is a moderate lift. Needs adaptive gamma."));
    driveForm->addRow(QStringLiteral("Shadow lift next to highlights"), shadowLift);
    auto* liftRampLo = numberControl(led, QStringLiteral("shadow_lift_ansi_ramp_lo"), 0.0, 1.0, 3, 0.005, 0.01, QString());
    liftRampLo->setToolTip(QStringLiteral(
        "How much highlight it takes before the lift starts: brightness x share of the picture, so 0.01 is a "
        "full-white area covering 1% of the frame. The lift is full at the next value."));
    driveForm->addRow(QStringLiteral("Lift starts at highlight amount"), liftRampLo);
    auto* liftRampHi = numberControl(led, QStringLiteral("shadow_lift_ansi_ramp_hi"), 0.0, 1.0, 3, 0.005, 0.10, QString());
    liftRampHi->setToolTip(QStringLiteral(
        "The highlight amount at which the lift reaches its full strength. Keep it above the start value."));
    driveForm->addRow(QStringLiteral("Lift full at highlight amount"), liftRampHi);
    auto* liftRate = numberControl(led, QStringLiteral("shadow_lift_ansi_rate"), 0.1, 10.0, 2, 0.1, 1.0, QString());
    liftRate->setToolTip(QStringLiteral(
        "How fast the lift follows a change in the highlights (full range per second; 1 = about a second). "
        "A cut always takes the new value at once. Lower if a flash makes the darks pump."));
    driveForm->addRow(QStringLiteral("Lift follows within (per second)"), liftRate);
    const std::vector<QWidget*> shadowLiftRows = { shadowLift, liftRampLo, liftRampHi, liftRate };
    auto* assist = new QWidget;
    auto* assistForm = new QFormLayout(assist);
    assistForm->setContentsMargins(0, 0, 0, 0);
    assistForm->setVerticalSpacing(8);
    assistForm->addRow(QString(), bindCheckField(QStringLiteral("Use the projector's contrast to help"), led,
        QStringLiteral("cntr_assist"), true));
    assistForm->addRow(QString(), bindCheckField(QStringLiteral("Software first"), led,
        QStringLiteral("software_first"), true));
    assistForm->addRow(QString(), bindCheckField(QStringLiteral("Ignore film grain in the highlight measure"), led,
        QStringLiteral("led_highlight_grain_suppress"), true));
    assistForm->addRow(QStringLiteral("Highest contrast for the assist"),
        numberControl(led, QStringLiteral("cntr_assist_max"), 50, 100, 0, 1, 100, QString()));
    assistForm->addRow(QString(), helpLabel(QStringLiteral(
        "Compensate only. The contrast adds the light VP's gain cannot add without crushing highlights, so the "
        "LED can dim further in scenes with highlights. Software first: VP's gain does what it can before the "
        "contrast moves. 100 = no limit beyond the projector's usable contrast.")));
    assistForm->addRow(QString(), bindCheckField(QStringLiteral("On a cut to a brighter scene, set the contrast to the cut level"),
        led, QStringLiteral("cut_cntr_dump"), true));
    assistForm->addRow(QStringLiteral("Contrast on a bright cut"),
        numberControl(led, QStringLiteral("cut_cntr_level"), 50, 100, 0, 1, 59, QStringLiteral("auto (white clip)")));
    assistForm->addRow(QString(), bindCheckField(QStringLiteral("Smooth fade-up after a bright cut"), led,
        QStringLiteral("shaped_recovery"), true));
    assistForm->addRow(QStringLiteral("Darkest start of a fade-up"),
        numberControl(led, QStringLiteral("max_cut_dip"), 0.0, 1.0, 2, 0.05, 0.5, QString()));
    assistForm->addRow(QStringLiteral("Contrast kept bright-side (%)"),
        numberControl(led, QStringLiteral("recovery_bias_pct"), 0.0, 20.0, 1, 0.5, 3.0, QString()));
    auto* prelift = levelControl(QStringLiteral("black_prelift_max"), 120);
    assistForm->addRow(QStringLiteral("LED lift allowed while still black"), prelift);
    assistForm->addRow(QString(), helpLabel(QStringLiteral(
        "On a cut to a brighter scene the contrast goes to the cut level (auto = where white starts to clip, 59 "
        "until measured; never 50), VP's gain to its maximum and the knee rolls off highlights; then the picture "
        "fades up with the LED. The darkest start (0.5 = half brightness) also sets the LED floor in every scene. "
        "Coming out of black with the look-ahead, the contrast is set and the LED may lift to the level above "
        "while the screen is still black (60 = keep the black absolutely; 255 = the old behaviour).")));

    auto* speeds = new QWidget;
    auto* speedsForm = new QFormLayout(speeds);
    speedsForm->setContentsMargins(0, 0, 0, 0);
    speedsForm->setVerticalSpacing(8);
    speedsForm->addRow(QStringLiteral("LED speeds"), bindChoiceField(led, QStringLiteral("rate_preset"),
        { QStringLiteral("script"), QStringLiteral("compensated") },
        { QStringLiteral("Script (your beta 22 speeds)"), QStringLiteral("Compensated (faster)") }));
    const QString preset = QStringLiteral("preset");
    speedsForm->addRow(QStringLiteral("Brightening: gain / min / max"), [&]
    {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(numberControl(led, QStringLiteral("rate_gain_up"), 0.0, 100.0, 2, 0.05, 0.25, preset));
        layout->addWidget(numberControl(led, QStringLiteral("rate_min_up"), 0.0, 1000.0, 1, 0.5, 2.5, preset));
        layout->addWidget(numberControl(led, QStringLiteral("rate_max_up"), 1.0, 1000.0, 1, 0.5, 17.5, preset));
        return row;
    }());
    speedsForm->addRow(QStringLiteral("Dimming: gain / min / max"), [&]
    {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(numberControl(led, QStringLiteral("rate_gain_down"), 0.0, 100.0, 2, 0.05, 0.15, preset));
        layout->addWidget(numberControl(led, QStringLiteral("rate_min_down"), 0.0, 1000.0, 1, 0.5, 2.5, preset));
        layout->addWidget(numberControl(led, QStringLiteral("rate_max_down"), 1.0, 1000.0, 1, 0.5, 12.5, preset));
        return row;
    }());
    speedsForm->addRow(QStringLiteral("Fade-up after a cut: gain / min / max"), [&]
    {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(numberControl(led, QStringLiteral("recovery_rate_gain"), 0.0, 100.0, 2, 0.05, 4.0, preset));
        layout->addWidget(numberControl(led, QStringLiteral("recovery_rate_min"), 0.0, 1000.0, 1, 0.5, 20.0, preset));
        layout->addWidget(numberControl(led, QStringLiteral("recovery_rate_max"), 1.0, 1000.0, 1, 0.5, 400.0, preset));
        return row;
    }());
    speedsForm->addRow(QString(), helpLabel(QStringLiteral(
        "Levels per second: gain x distance, kept between min and max, so the LED is fast while far from its "
        "target and eases in. The LED within a scene, and the LED and contrast moving together after a cut, "
        "use the brightening / dimming speeds; only the fade-up for what they cannot yet supply uses the "
        "fade-up speeds. \"preset\" = the LED speeds choice above.")));

    auto* response = new QWidget;
    auto* responseForm = new QFormLayout(response);
    responseForm->setContentsMargins(0, 0, 0, 0);
    responseForm->setVerticalSpacing(8);
    responseForm->addRow(QStringLiteral("LED settle time (ms)"),
        numberControl(projector, QStringLiteral("led_settle_ms"), 0, 5000, 0, 10, 300, QString()));
    responseForm->addRow(QStringLiteral("Contrast settle time (ms)"),
        numberControl(projector, QStringLiteral("cntr_settle_ms"), 0, 5000, 0, 10, 0, QString()));
    auto* maxUsable = numberControl(projector, QStringLiteral("cntr_max_usable"), 50, 100, 0, 1, 74, QString());
    responseForm->addRow(QStringLiteral("Highest useful contrast"), maxUsable);
    auto* whiteClips = numberControl(projector, QStringLiteral("dmd_clip_cntr"), 50, 100, 0, 1, 59, QString());
    responseForm->addRow(QStringLiteral("Contrast where white clips"), whiteClips);
    responseForm->addRow(QString(), helpLabel(QStringLiteral(
        "From the camera and light-meter measurements; leave the defaults until then. Settle time: how long "
        "the light takes to arrive after the response time (300 ms is deliberately fast). White clips: the "
        "contrast at which a white field stops getting brighter at full LED (59 = where your script's dump "
        "ended).")));

    // Settings VP reads that only a hand edit could set before (audit
    // 2026-10-14c: every setting has a Config control): collapsed, expert.
    auto* advancedProjector = new QWidget;
    auto* advancedForm = new QFormLayout(advancedProjector);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    advancedForm->setVerticalSpacing(8);
    advancedForm->addRow(QString(), helpLabel(QStringLiteral(
        "Only for a projector other than the FL35, or when the projector's replies say something is wrong. "
        "The defaults match the FL35.")));
    advancedForm->addRow(QStringLiteral("Baud rate"),
        numberControl(projector, QStringLiteral("baud"), 1200, 921600, 0, 1, 19200, QString()));
    advancedForm->addRow(QStringLiteral("Lowest LED level the projector accepts"),
        numberControl(projector, QStringLiteral("light_min"), 0, 255, 0, 1, 0, QStringLiteral("Ask the projector")));
    advancedForm->addRow(QStringLiteral("Highest LED level the projector accepts"),
        numberControl(projector, QStringLiteral("light_max"), 0, 255, 0, 1, 0, QStringLiteral("Ask the projector")));
    advancedForm->addRow(QStringLiteral("Wait for a reply (ms)"),
        numberControl(projector, QStringLiteral("ack_timeout_ms"), 20, 5000, 0, 10, 300, QString()));
    advancedForm->addRow(QStringLiteral("Gap between commands (ms)"),
        numberControl(projector, QStringLiteral("min_command_gap_ms"), 0, 5000, 0, 10, 200, QString()));
    advancedForm->addRow(QStringLiteral("Largest LED step per command"),
        numberControl(projector, QStringLiteral("max_levels_per_command"), 0, 255, 0, 1, 12, QString()));
    advancedForm->addRow(QStringLiteral("Commands before a pause (0 = no pause)"),
        numberControl(projector, QStringLiteral("burst_commands"), 0, 1000, 0, 1, 0, QString()));
    advancedForm->addRow(QStringLiteral("Pause length (ms)"),
        numberControl(projector, QStringLiteral("burst_pause_ms"), 0, 60000, 0, 100, 5000, QString()));
    advancedForm->addRow(QString(), bindCheckField(QStringLiteral("Ask the projector to echo commands"), projector,
        QStringLiteral("echo"), true));
    advancedForm->addRow(QStringLiteral("Line ending"), bindChoiceField(projector, QStringLiteral("terminator"),
        { QStringLiteral("auto"), QStringLiteral("cr"), QStringLiteral("literal_cr"), QStringLiteral("script") },
        { QStringLiteral("Automatic (try the projector)"), QStringLiteral("Carriage return"),
          QStringLiteral("The letters \"cr\" then a carriage return"), QStringLiteral("Exactly as your script sent it") }));
    auto* fluxTable = bindTextField(projector, QStringLiteral("flux_table"));
    fluxTable->setPlaceholderText(QStringLiteral("empty = light proportional to the LED level"));
    fluxTable->setToolTip(QStringLiteral(
        "Light-meter readings as LED level:relative light pairs, e.g. 60:0.21,128:0.49,255:1. Compensate uses "
        "them to know how much light each LED level really gives."));
    advancedForm->addRow(QStringLiteral("Measured light per LED level"), fluxTable);

    // In setup order (audit 2026-10-14c): what to choose, then the source's
    // own card, the connection, and the rest; Experimental last. Each card
    // shows only when the chosen source uses it.
    cards->addCard(createCard(QStringLiteral("LED drive"),
        QStringLiteral("Dim the projector's LED with the picture, as your dimming script does."), drive));
    QWidget* connectionBox = createCard(QStringLiteral("Projector connection"),
        QStringLiteral("The serial link to the projector."), connection);
    cards->addCard(connectionBox);
    QWidget* responseBox = createCard(QStringLiteral("Projector response"),
        QStringLiteral("Measured timings and contrast limits."), response);
    cards->addCard(responseBox);
    QWidget* profileBox = createCard(QStringLiteral("LED profile"),
        QStringLiteral("The detailed behaviour of VP's own sources: built-in, or your own file."), profile);
    cards->addCard(profileBox);
    QWidget* assistBox = createCard(QStringLiteral("Contrast and cuts"),
        QStringLiteral("Compensate: the projector's contrast, bright cuts and coming out of black."), assist);
    cards->addCard(assistBox);
    QWidget* speedsBox = createCard(QStringLiteral("LED speeds"),
        QStringLiteral("Compensate: how fast the LED and the fade-up move."), speeds);
    cards->addCard(speedsBox);
    // Collapsed until asked for.
    auto* advancedHolder = new QWidget;
    auto* advancedLayout = new QVBoxLayout(advancedHolder);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    auto* advancedToggle = new QToolButton;
    advancedToggle->setObjectName(controlName(projector, QStringLiteral("advanced_toggle")));
    advancedToggle->setText(QStringLiteral("Show the advanced settings"));
    advancedToggle->setCheckable(true);
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle->setArrowType(Qt::RightArrow);
    advancedLayout->addWidget(advancedToggle);
    advancedLayout->addWidget(advancedProjector);
    advancedProjector->setVisible(false);
    connect(advancedToggle, &QToolButton::toggled, this, [advancedToggle, advancedProjector](bool open)
    {
        advancedProjector->setVisible(open);
        advancedToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        advancedToggle->setText(open ? QStringLiteral("Hide the advanced settings") : QStringLiteral("Show the advanced settings"));
    });
    QWidget* advancedBox = createCard(QStringLiteral("Advanced projector"),
        QStringLiteral("Serial details for other projectors; the defaults match the FL35."), advancedHolder);
    cards->addCard(advancedBox);
    QWidget* experimentBox = createCard(QStringLiteral("Experimental"),
        QStringLiteral("Compensate: deeper fade to black - test on the projector before relying on it."), experiment);
    cards->addCard(experimentBox);
    {
        // Show only what the chosen source uses; grey the shadow lift while
        // adaptive gamma is off and everything while the LED drive is off
        // (audits 2026-10-14b/c).
        QComboBox* source = ledSource;
        QCheckBox* adaptive = ledAdaptive;
        auto show = [driveForm, responseForm, compensateRows, experimentBox, assistBox, speedsBox,
            profileBox, source, adaptive, compensateWarning, aheadRow, aheadHelp, maxUsable, whiteClips, pacingRows]()
        {
            const QString chosen = source ? source->currentData().toString() : QString();
            const bool compensate = chosen == QStringLiteral("compensate");
            const bool own = compensate || chosen == QStringLiteral("internal_script");
            for (QWidget* row : compensateRows)
                driveForm->setRowVisible(row, compensate);
            for (QWidget* box : { experimentBox, assistBox, speedsBox })
                box->setVisible(compensate);
            for (QWidget* row : { maxUsable, whiteClips })
                responseForm->setRowVisible(row, compensate);
            profileBox->setVisible(own);
            // The script copy decides on the frame on screen, as the script did
            // (look-ahead is compensate's).
            driveForm->setRowVisible(aheadRow, compensate);
            driveForm->setRowVisible(aheadHelp, compensate);
            for (QWidget* row : pacingRows)
                driveForm->setRowVisible(row, chosen == QStringLiteral("internal_script"));
            driveForm->setRowVisible(compensateWarning, compensate && adaptive && !adaptive->isChecked());
        };
        auto grey = [shadowLiftRows, adaptive]()
        {
            for (QWidget* row : shadowLiftRows)
                row->setEnabled(!adaptive || adaptive->isChecked());
        };
        // A source that drives the projector needs the projector protocol.
        auto needPort = [source, protocolChoice]()
        {
            const QString chosen = source ? source->currentData().toString() : QString();
            if (chosen != QStringLiteral("none") && protocolChoice->currentData().toString() == QStringLiteral("none"))
                protocolChoice->setCurrentIndex(protocolChoice->findData(QStringLiteral("barco_ascii")));
        };
        auto onOff = [enabled, source, adaptive, connectionBox, responseBox, profileBox, assistBox,
            speedsBox, advancedBox, experimentBox, aheadRow, pacingRows]()
        {
            const bool on = enabled->isChecked();
            for (QWidget* row : pacingRows) row->setEnabled(on);
            for (QWidget* w : { static_cast<QWidget*>(source), static_cast<QWidget*>(adaptive), aheadRow,
                connectionBox, responseBox, profileBox, assistBox, speedsBox, advancedBox, experimentBox })
                if (w) w->setEnabled(on);
        };
        show();
        grey();
        onOff();
        connect(source, qOverload<int>(&QComboBox::currentIndexChanged), this, [show](int) { show(); });
        connect(source, qOverload<int>(&QComboBox::activated), this, [needPort](int) { needPort(); });
        connect(source, qOverload<int>(&QComboBox::currentIndexChanged), this, [needPort](int) { needPort(); });
        connect(adaptive, &QCheckBox::toggled, this, [grey, show](bool) { grey(); show(); });
        connect(enabled, &QCheckBox::toggled, this, [onOff](bool) { onOff(); });
    }
    return createPage(QStringLiteral("LED drive"),
        QStringLiteral("Projector LED dimming and the gamma boost."), cards);
}

QWidget* ConfigEditorWindow::createLogsPage()
{
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto* enabled = bindCheckField(QStringLiteral("Enable logging"),
        QStringLiteral("logging"), QStringLiteral("enabled"), true);
    enabled->setObjectName(QStringLiteral("config.logging.enabled"));
    enabled->setAccessibleDescription(QStringLiteral(
        "Enable VP logging. Logging is enabled by default."));
    layout->addWidget(enabled);

    auto* enhanced = bindCheckField(QStringLiteral("Enable enhanced logging"),
        QStringLiteral("logging"), QStringLiteral("debug"), false);
    enhanced->setObjectName(QStringLiteral("config.logging.debug"));
    enhanced->setAccessibleDescription(QStringLiteral(
        "Keep all VP logs and write additional live telemetry files for diagnosis."));
    layout->addWidget(enhanced);

    auto* retention = new QSpinBox;
    retention->setObjectName(QStringLiteral("config.logging.debug_log_retention"));
    retention->setAccessibleName(QStringLiteral("Log retention"));
    retention->setRange(1, 100);
    retention->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    retention->setMaximumWidth(108);
    retention->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    retention->setAlignment(Qt::AlignRight);
    bool validRetention = false;
    const int configuredRetention = value(QStringLiteral("logging"),
        QStringLiteral("debug_log_retention"), QStringLiteral("10")).toInt(&validRetention);
    retention->setValue(validRetention && configuredRetention >= 1 && configuredRetention <= 100 ? configuredRetention : 10);
    retention->setToolTip(QStringLiteral(
        "Keep 1 to 100 total files, including the active VP log. Default: 10."));
    connect(retention, qOverload<int>(&QSpinBox::valueChanged), this,
        [this](int count)
    {
        if (!document_) return;
        document_->SetKnown("logging", "debug_log_retention",
            std::to_string(count));
        markDirty();
    });
    auto syncLoggingControls = [enabled, enhanced, retention]
    {
        const bool loggingEnabled = enabled->isChecked();
        enhanced->setEnabled(loggingEnabled);
        retention->setEnabled(loggingEnabled && !enhanced->isChecked());
    };
    connect(enabled, &QCheckBox::toggled, this, syncLoggingControls);
    connect(enhanced, &QCheckBox::toggled, this, syncLoggingControls);
    syncLoggingControls();
    layout->addWidget(fieldWithHelp(QStringLiteral("Log files to keep"), retention,
        QStringLiteral("1–100 total files, including the active log. Default: 10. Changes apply when VP next starts.")));

    layout->addWidget(helpLabel(QStringLiteral(
        "Enhanced logging keeps all logs and writes additional live telemetry files. Use it only while diagnosing an issue.")));

    QString logDirectory = QFileInfo(configPath_).absoluteDir().filePath(QStringLiteral("logs"));
    const QString logPath = QDir(logDirectory).filePath(QStringLiteral("vp.log"));
    auto* actions = new QHBoxLayout;
    auto* openFolder = new QPushButton(QStringLiteral("Open log folder"));
    auto* openLog = new QPushButton(QStringLiteral("Open current log"));
	openFolder->setEnabled(!remoteClient_);
	openLog->setEnabled(!remoteClient_);
	if (remoteClient_)
	{
		openFolder->setToolTip(QStringLiteral("Logs are on the VP computer."));
		openLog->setToolTip(QStringLiteral("Logs are on the VP computer."));
	}
    openFolder->setAccessibleDescription(QStringLiteral("Open the folder containing VideoProcessor logs."));
    openLog->setAccessibleDescription(QStringLiteral("Open the current VideoProcessor log."));
    actions->addWidget(openFolder);
    actions->addWidget(openLog);
    actions->addStretch();
    layout->addLayout(actions);
    connect(openFolder, &QPushButton::clicked, this, [this, logDirectory]
    {
        if (!QDir(logDirectory).exists())
        {
            QMessageBox::information(this, QStringLiteral("Log folder"),
                QStringLiteral("VP has not created its logs folder yet. Start VideoProcessor once, then try again."));
            return;
        }
        if (!openPathExternally(logDirectory))
            QMessageBox::warning(this, QStringLiteral("Log folder"),
                QStringLiteral("Windows could not open the log folder."));
    });
    connect(openLog, &QPushButton::clicked, this, [this, logPath]
    {
        if (!QFileInfo::exists(logPath))
        {
            QMessageBox::information(this, QStringLiteral("Current log"),
                QStringLiteral("No current VP log exists yet. Start VideoProcessor once, then try again."));
            return;
        }
        if (!openPathExternally(logPath))
            QMessageBox::warning(this, QStringLiteral("Current log"),
                QStringLiteral("Windows could not open the current log."));
    });
    layout->addStretch();

    return createPage(QStringLiteral("Logs"),
        QStringLiteral("Control VideoProcessor log files and diagnostic detail."), content);
}

QWidget* ConfigEditorWindow::createShortcutsPage()
{
    struct ShortcutField
    {
        const char* label;
        const char* key;
        const char* defaultValue;
    };
    const ShortcutField applicationFields[] = {
        { "Open configuration", "config_editor", "Ctrl+Shift+S" },
        { "Toggle video-only UI", "toggle_noui", "Ctrl+Shift+U" },
        { "Toggle fullscreen", "fullscreen_toggle", "Alt+Enter" },
        { "Exit fullscreen", "fullscreen_exit", "Esc" },
        { "Toggle statistics", "toggle_stats_overlay", "Ctrl+I" },
        { "Screenshot", "capture_rendered_output", "Ctrl+Alt+S" },
        { "Re-apply rules", "reapply_rules", "" },
        { "Room lights (dark / lit)", "room_lights", "" },
        { "Show profiles", "show_profiles", "Ctrl+Alt+I" },
        { "Automatic transfer (DirectShow only)", "auto_set", "Ctrl+Shift+A" },
        { "PQ transfer (DirectShow only)", "pq_set", "Ctrl+Shift+P" }
    };
    const ShortcutField captureFields[] = {
        { "Restart renderer", "renderer_restart", "Shift+R" },
        { "Reset renderer", "renderer_reset", "R" },
        { "DeckLink input 1", "capture_1", "Ctrl+1" },
        { "DeckLink input 2", "capture_2", "Ctrl+2" },
        { "DeckLink input 3", "capture_3", "Ctrl+3" },
        { "DeckLink input 4", "capture_4", "Ctrl+4" },
        { "Video conversion off", "video_conversion_off", "V" },
        { "V210 to P010 conversion", "video_conversion_p010", "Shift+V" }
    };

    auto makeEditor = [this](const QString& key, const QString& defaultValue)
    {
        const std::string section = "shortcuts";
        const std::string keyText = key.toStdString();
        size_t line = 0, start = 0, end = 0;
        const bool explicitlyConfigured = document_ &&
            document_->Find(section, keyText.c_str(), line, start, end);
        const QString initial = canonicalShortcutText(explicitlyConfigured ?
            QString::fromLocal8Bit(document_->Get(section.c_str(), keyText.c_str()).c_str()) :
            defaultValue);
        auto* edit = new QLineEdit(initial);
        edit->setObjectName(controlName(QStringLiteral("shortcuts"), key));
        edit->setAccessibleName(accessibleSettingName(key));
        // QLineEdit's size hint shrinks when the clear button is absent. Keep
        // every shortcut column aligned whether the value is populated,
        // explicitly empty, or currently has no visible clear affordance.
        edit->setFixedWidth(220);
        edit->setClearButtonEnabled(true);
        edit->setToolTip(defaultValue.isEmpty() ?
            QStringLiteral("No built-in shortcut. Clear and save to disable.") :
            QStringLiteral("VP default: %1. Clear and save to disable.").arg(defaultValue));
        connect(edit, &QLineEdit::textChanged, this, [this, key = keyText](const QString& text)
        {
            if (!document_) return;
            if (!document_->SetKnown("shortcuts", key.c_str(), text.toLocal8Bit().constData()))
            {
                document_->AddSection("shortcuts");
                document_->SetKnown("shortcuts", key.c_str(), text.toLocal8Bit().constData());
            }
            markDirty();
        });
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, key = keyText]
        {
            const QString entered = edit->text().trimmed();
            if (entered.isEmpty()) return;
            std::string canonical;
            if (!RendererProfileConfig::CanonicalizeKeyChord(entered.toStdString(), canonical))
            {
                edit->setProperty("invalid", true);
                edit->style()->unpolish(edit);
                edit->style()->polish(edit);
                setStatus(QStringLiteral("%1 is not a valid shortcut. Use a chord such as Ctrl+Shift+A.").arg(entered), true);
                return;
            }
            edit->setProperty("invalid", false);
            edit->style()->unpolish(edit);
            edit->style()->polish(edit);
            const QString normalized = QString::fromStdString(canonical);
            if (edit->text() != normalized) edit->setText(normalized);
            else if (document_) document_->SetKnown("shortcuts", key.c_str(), canonical);
        });
        return edit;
    };

    auto makeForm = [&makeEditor](const ShortcutField* fields, size_t count)
    {
        auto* content = new QWidget;
        auto* form = new QFormLayout(content);
        form->setContentsMargins(0, 0, 0, 0);
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(8);
        form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        for (size_t index = 0; index < count; ++index)
            form->addRow(QString::fromLatin1(fields[index].label), makeEditor(
                QString::fromLatin1(fields[index].key), QString::fromLatin1(fields[index].defaultValue)));
        return content;
    };

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    auto* fixedShortcuts = new ResponsiveCardGrid;
    auto* applicationCard = createCard(QStringLiteral("Application"),
        QStringLiteral("Built-in application and display controls."),
        makeForm(applicationFields, std::size(applicationFields)));
    auto* captureCard = createCard(QStringLiteral("Capture & renderer"),
        QStringLiteral("Capture selection, renderer control, and active-renderer conversion."),
        makeForm(captureFields, std::size(captureFields)));
    fixedShortcuts->addCard(applicationCard);
    fixedShortcuts->addCard(captureCard);
    layout->addWidget(fixedShortcuts);

    const QStringList& renderers = allRenderers_;
    if (!renderers.isEmpty())
    {
        auto* rendererContent = new QWidget;
        auto* rendererForm = new QFormLayout(rendererContent);
        rendererShortcutForm_ = rendererForm;
        rendererForm->setContentsMargins(0, 0, 0, 0);
        rendererForm->setHorizontalSpacing(18);
        rendererForm->setVerticalSpacing(8);
        rendererForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        rendererForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
        for (int index = 0; index < renderers.size(); ++index)
        {
            QLineEdit* edit = makeEditor(QStringLiteral("render.%1").arg(index + 1), QString());
            edit->setProperty("rendererName", renderers[index]);
            rendererForm->addRow(QStringLiteral("%1 - %2").arg(index + 1).arg(renderers[index]), edit);
        }
        layout->addWidget(createCard(QStringLiteral("Renderer selection"),
            QStringLiteral("Optional shortcuts select a renderer by its one-based position in VP's renderer list on this PC."),
            rendererContent));
        applyRendererVisibilityFilter(configuredBooleanValue(value(
            QStringLiteral("general"), QStringLiteral("hide_legacy_renderers")),
            true));
    }
    return createPage(QStringLiteral("Shortcuts"),
        QStringLiteral("Configure keyboard shortcuts. Defaults are shown; clear a field and save to disable it. A running VP applies saved shortcuts immediately."),
        content);
}

bool ConfigEditorWindow::savedForegroundOnlyEnabled() const
{
    for (const auto& section : savedSnapshot_)
    {
        if (ConfigurationApplyPolicy::NormalizeSection(section.first) !=
            "shortcuts")
            continue;
        for (const auto& setting : section.second)
            if (ConfigurationApplyPolicy::NormalizeSection(setting.first) ==
                "foreground_only")
                return configuredBooleanValue(
                    QString::fromLocal8Bit(setting.second.c_str()), false);
    }
    return false;
}

void ConfigEditorWindow::returnFocusToPresentationTarget(const char* reason)
{
    const HWND target = reinterpret_cast<HWND>(presentationTargetHandle_);
    DWORD actualProcessId = 0;
    const bool validTarget = target && IsWindow(target) &&
        presentationTargetProcessId_ != 0 &&
        presentationTargetProcessId_ == ownerProcessId_ &&
        GetWindowThreadProcessId(target, &actualProcessId) != 0 &&
        actualProcessId == presentationTargetProcessId_;
    if (!ConfigurationLiveApply::ShouldReturnPresentationFocus(
        savedForegroundOnlyEnabled(), true, validTarget))
        return;

    if (IsIconic(target)) ShowWindowAsync(target, SW_RESTORE);
    AllowSetForegroundWindow(ownerProcessId_);
    BringWindowToTop(target);
    const BOOL requested = SetForegroundWindow(target);
    const HWND foreground = GetForegroundWindow();
    const QString diagnostic = QStringLiteral(
        "VideoProcessor Config focus handoff: reason=%1 target=0x%2 requested=%3 foreground=0x%4\n")
        .arg(QString::fromLatin1(reason))
        .arg(reinterpret_cast<quintptr>(target), 0, 16)
        .arg(requested ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(reinterpret_cast<quintptr>(foreground), 0, 16);
    OutputDebugStringW(reinterpret_cast<LPCWSTR>(diagnostic.utf16()));
}

QWidget* ConfigEditorWindow::createShortcutsSetupPage()
{
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    auto* handling = new QWidget;
    auto* handlingLayout = new QVBoxLayout(handling);
    handlingLayout->setContentsMargins(0, 0, 0, 0);
    handlingLayout->setSpacing(8);
    handlingLayout->addWidget(bindCheckField(
        QStringLiteral("Only process shortcuts while VideoProcessor is in the foreground"),
        QStringLiteral("shortcuts"), QStringLiteral("foreground_only"), false));
    auto* help = new QLabel(QStringLiteral(
        "When enabled, background applications keep their keystrokes. VideoProcessor asks Windows to return focus after startup, a renderer change, or closing or minimizing Config."));
    help->setWordWrap(true);
    help->setObjectName(QStringLiteral("fieldHelp"));
    handlingLayout->addWidget(help);
    auto* globalHotkeys = bindCheckField(
        QStringLiteral("Single keys work while another application has focus"),
        QStringLiteral("shortcuts"), QStringLiteral("global_hotkeys"), false);
    globalHotkeys->setToolTip(QStringLiteral(
        "Off (recommended): keys without Ctrl or Alt, and Shift+letter, act only while VideoProcessor "
        "has focus, so typing in another application never changes VideoProcessor. Ctrl, Alt and "
        "function-key shortcuts stay global for a remote keyboard. On: every shortcut is global."));
    handlingLayout->addWidget(globalHotkeys);
    auto* abHelp = new QLabel(QStringLiteral(
        "Settings are the primary control; shortcuts are a backup for A/B comparison. A key's change "
        "lasts until restart, a profile change, or applying that setting here, and the statistics "
        "overlay shows it on its \"Key override\" line. Keys marked DirectShow only do nothing with "
        "VP Renderer."));
    abHelp->setWordWrap(true);
    abHelp->setObjectName(QStringLiteral("fieldHelp"));
    handlingLayout->addWidget(abHelp);
    layout->addWidget(createCard(QStringLiteral("Keyboard handling"),
        QStringLiteral("Choose whether shortcuts remain global or require VideoProcessor focus."),
        handling));
    layout->addStretch();

    return createPage(QStringLiteral("Shortcuts"),
        QStringLiteral("Control when VideoProcessor processes configured keyboard shortcuts."),
        content);
}

void ConfigEditorWindow::showUpdates()
{
    reveal();
    selectPage(21);   // Updates (RC3 merge: after our LED drive page, 20)
}

void ConfigEditorWindow::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    tray_ = new QSystemTrayIcon(windowIcon(), this);
    tray_->setToolTip(QStringLiteral("VideoProcessor Configuration"));
    auto* menu = new QMenu(this);
	QAction* open = menu->addAction(noTarget_ ?
		QStringLiteral("Open Configuration (%1)").arg(
			rememberedTargetId_.isEmpty() ? QStringLiteral("no target") :
				rememberedTargetName()) : remoteClient_ ?
		QStringLiteral("Open Configuration (%1)").arg(remoteName_) :
		QStringLiteral("Open Configuration (This computer)"));
	trayOpenAction_ = open;
	// Keep target selection available even when this instance opened a file.
	{
		targetsMenu_ = menu->addMenu(QStringLiteral("Targets"));
		connect(targetsMenu_, &QMenu::aboutToShow, this,
			[this] { populateTrayTargets(); });
	}
    menu->addSeparator();
    connect(menu->addAction(QStringLiteral("Check for updates on this computer…")),
        &QAction::triggered, this, [this] { showUpdates(); });
    QAction* exit = menu->addAction(QStringLiteral("Exit"));
    tray_->setContextMenu(menu);
    connect(open, &QAction::triggered, this, [this] { reveal(); });
    connect(exit, &QAction::triggered, this, [this] { exitApplication(); });
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason)
    {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) reveal();
    });
    tray_->show();
}

void ConfigEditorWindow::populateTrayTargets()
{
	if (!targetsMenu_) return;
	targetsMenu_->clear();
	if (targetChoices_.isEmpty() ||
		(noTarget_ && targetChoices_.size() == 1))
	{
		auto* unavailable = targetsMenu_->addAction(
			QStringLiteral("No VideoProcessor found"));
		unavailable->setEnabled(false);
	}
	else
	{
		if (noTarget_)
		{
			auto* waiting = targetsMenu_->addAction(targetChoices_.front().label);
			waiting->setEnabled(false);
		}
		for (int index = noTarget_ ? 1 : 0;
			index < targetChoices_.size(); ++index)
		{
			const Target target = targetChoices_[index];
			auto* action = targetsMenu_->addAction(target.label);
			action->setToolTip(target.host.isEmpty() ?
				QStringLiteral("Configuration file on this computer") :
				QStringLiteral("%1:%2 · %3")
					.arg(target.host).arg(target.port).arg(target.vpVersion));
			if (!noTarget_ && index == 0)
			{
				action->setCheckable(true);
				action->setChecked(true);
			}
			connect(action, &QAction::triggered, this, [this, target]
			{
				reveal();
				selectAnotherTarget(target);
			});
		}
	}
	targetsMenu_->addSeparator();
	connect(targetsMenu_->addAction(QStringLiteral("Enter address…")),
		&QAction::triggered, this, [this]
		{
			reveal();
			enterTargetAddress();
		});
	connect(targetsMenu_->addAction(QStringLiteral("Refresh")),
		&QAction::triggered, this,
		[this] { refreshTargetChoices(); });
}

void ConfigEditorWindow::reveal()
{
    // AppMain deliberately defers the first reveal until the event loop has
    // started. Clearing this only here prevents a transient blank native frame
    // from appearing during construction or owner association.
    setAttribute(Qt::WA_DontShowOnScreen, false);
    // A reveal that arrived after an older queued close is the newer command.
    // Remove only already-posted close requests; a subsequent user close is
    // still delivered normally and returns the editor to the tray.
    QCoreApplication::removePostedEvents(this, QEvent::Close);
    positionForReveal();
    showNormal();
    scopedTopmostEligible_ = true;
    explicitRevealIntent_ = true;
    pendingTopmostReassert_ = true;
    // Let Qt issue its activation/z-order requests first. The native placement
    // below is authoritative and must run last; otherwise Qt can finalize a
    // queued raise afterward and remove WS_EX_TOPMOST a few seconds later.
    raise();
    activateWindow();
    applyScopedTopmost();
	// Execute the native z-order/foreground request for every explicit reveal,
	// even when our cached state already says the window is topmost. Renderer
	// handoffs and exclusive fullscreen can reorder or cover the native window
	// without changing Qt's visibility/topmost bookkeeping.
	const HWND editor = reinterpret_cast<HWND>(effectiveWinId());
	if (editor && IsWindow(editor))
	{
		ShowWindow(editor, SW_RESTORE);
		SetWindowPos(editor, HWND_TOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		BringWindowToTop(editor);
		SetForegroundWindow(editor);
	}
    // Ignore only activation churn caused by this reveal itself. A later,
    // settled user transition away from Config disables lifecycle reasserts.
    QTimer::singleShot(100, this, [this] { explicitRevealIntent_ = false; });
}

void ConfigEditorWindow::exitApplication()
{
    exitRequested_ = true;
    if (tray_) tray_->hide();
    QCoreApplication::quit();
}

void ConfigEditorWindow::closeEvent(QCloseEvent* event)
{
    if (!exitRequested_ && tray_ && tray_->isVisible())
    {
        const HWND editor = reinterpret_cast<HWND>(effectiveWinId());
        returnFocusAfterHide_ = savedForegroundOnlyEnabled() && editor &&
            GetForegroundWindow() == editor;
        if (returnFocusAfterHide_ && ownerProcessId_ != 0)
            AllowSetForegroundWindow(ownerProcessId_);
        hide();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void ConfigEditorWindow::hideEvent(QHideEvent* event)
{
    for (ProfileListBinding& binding : activeProfileLists_)
    {
        binding.pendingActiveSelection = true;
        binding.editSelectionTouched = false;
    }
    if (activeProfileTimer_) activeProfileTimer_->stop();
    pendingTopmostReassert_ = false;
    explicitRevealIntent_ = false;
    scopedTopmostEligible_ = false;
    removeScopedTopmost();
    clearNativeOwner();
    QMainWindow::hideEvent(event);
    if (returnFocusAfterHide_)
    {
        returnFocusAfterHide_ = false;
        returnFocusToPresentationTarget("config-close");
    }
}

void ConfigEditorWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (activeProfileTimer_ && !activeProfileTimer_->isActive())
    {
        refreshActiveProfileIndicators();
        activeProfileTimer_->start(remoteClient_ ? 3000 : 500);
    }
    scopedTopmostEligible_ = true;
    pendingTopmostReassert_ = true;
    applyNativeOwner();
    publishNativeAssociation();
    // QWindows can finalize its native top-level style/owner after showEvent.
    // Reapply once at the next event-loop turn so that finalization cannot
    // clear the validated cross-process owner.
    QTimer::singleShot(0, this, [this]
    {
        applyNativeOwner();
        publishNativeAssociation();
        applyScopedTopmost();
    });
}

bool ConfigEditorWindow::nativeEvent(const QByteArray& eventType,
    void* nativeMessage, qintptr* result)
{
    Q_UNUSED(eventType);
    MSG* message = static_cast<MSG*>(nativeMessage);
    static const UINT updateExitMessage = RegisterWindowMessageW(L"VideoProcessor.UpdateExit.v1");
    if (message && updateExitMessage && message->message == updateExitMessage)
    {
        // New helpers request visibility in the acknowledgment; old helpers still receive 1.
        // Capture the actual window state immediately before shutdown.
        *result = message->wParam == 1 && !isVisible() ? 3 : 1;
        if (!updateExitPending_)
        {
            updateExitPending_ = true;
            QTimer::singleShot(0, this, [this] { exitApplication(); });
        }
        return true;
    }

    static const UINT activationMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigEditor.Activate.v1");
    static const UINT presentationTargetMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigEditor.PresentationTarget.v1");
	static const UINT presentationTargetMessageV2 = RegisterWindowMessageW(
		L"VideoProcessor.ConfigEditor.PresentationTarget.v2");
	static const UINT presentationTargetAcknowledgementEndpointMessage =
		RegisterWindowMessageW(
			L"VideoProcessor.ConfigEditor.PresentationTargetAckEndpoint.v1");
	static const UINT presentationTargetAcknowledgementMessageV2 =
		RegisterWindowMessageW(
			L"VideoProcessor.ConfigEditor.PresentationTargetAck.v2");
    static const UINT reassertMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigEditor.Reassert.v1");
    if (!message)
        return QMainWindow::nativeEvent(eventType, nativeMessage, result);
	if (presentationTargetAcknowledgementEndpointMessage &&
		message->message == presentationTargetAcknowledgementEndpointMessage)
	{
		const DWORD requestedProcessId = static_cast<DWORD>(message->wParam);
		const HWND requestedEndpoint = reinterpret_cast<HWND>(message->lParam);
		DWORD actualProcessId = 0;
		if (requestedEndpoint && IsWindow(requestedEndpoint))
			GetWindowThreadProcessId(requestedEndpoint, &actualProcessId);
		const bool accepted = requestedProcessId != 0 &&
			requestedProcessId == ownerProcessId_ &&
			actualProcessId == requestedProcessId;
		if (accepted)
		{
			presentationTargetAcknowledgementHandle_ =
				reinterpret_cast<quintptr>(requestedEndpoint);
			presentationTargetAcknowledgementProcessId_ = requestedProcessId;
		}
		if (result) *result = accepted ? 1 : 0;
		return true;
	}

    if (message->message == presentationTargetMessage ||
		message->message == presentationTargetMessageV2)
    {
        const DWORD requestedProcessId = static_cast<DWORD>(message->wParam);
		const quint32 requestedSequence =
			message->message == presentationTargetMessageV2 ?
				static_cast<quint32>(static_cast<quint64>(message->wParam) >> 32) :
				0;
        const HWND requestedTarget = reinterpret_cast<HWND>(message->lParam);
        DWORD actualProcessId = 0;
        if (requestedTarget && IsWindow(requestedTarget))
            GetWindowThreadProcessId(requestedTarget, &actualProcessId);
        const bool accepted = requestedProcessId != 0 &&
            requestedProcessId == ownerProcessId_ &&
            actualProcessId == requestedProcessId;
        if (accepted)
        {
            presentationTargetHandle_ = reinterpret_cast<quintptr>(requestedTarget);
            presentationTargetProcessId_ = requestedProcessId;
			// The warm editor may be revealed through its process event before
			// this asynchronous target update is dispatched.  Place it immediately
			// as well as during reveal so every open follows VP's current display.
			positionForReveal();
			if (isVisible() && scopedTopmostEligible_)
			{
				pendingTopmostReassert_ = true;
				QTimer::singleShot(0, this, [this] { applyScopedTopmost(); });
			}
			// The target is placement metadata only: Config remains an independent
			// native top-level window and never acquires the fullscreen HWND as an
			// owner. v2 acknowledges receiver acceptance asynchronously; VP treats
			// a queued PostMessage as pending, never as accepted.
			const HWND editor = reinterpret_cast<HWND>(effectiveWinId());
			if (requestedSequence != 0 && presentationTargetAcknowledgementMessageV2 &&
				presentationTargetAcknowledgementHandle_ && editor &&
				IsWindow(editor) &&
				presentationTargetAcknowledgementProcessId_ == requestedProcessId &&
				IsWindow(reinterpret_cast<HWND>(
					presentationTargetAcknowledgementHandle_)))
			{
				PostMessageW(reinterpret_cast<HWND>(
					presentationTargetAcknowledgementHandle_),
					presentationTargetAcknowledgementMessageV2,
					static_cast<WPARAM>(requestedSequence),
					reinterpret_cast<LPARAM>(editor));
			}
        }
        if (result) *result = accepted ? 1 : 0;
        return true;
    }
    if (message->message == reassertMessage)
    {
        const DWORD requestedProcessId = static_cast<DWORD>(message->wParam);
        const HWND requestedTarget = reinterpret_cast<HWND>(message->lParam);
        DWORD actualProcessId = 0;
        if (requestedTarget && IsWindow(requestedTarget))
            GetWindowThreadProcessId(requestedTarget, &actualProcessId);
        const bool accepted = requestedProcessId != 0 &&
            requestedProcessId == ownerProcessId_ &&
            actualProcessId == requestedProcessId &&
            reinterpret_cast<quintptr>(requestedTarget) ==
                presentationTargetHandle_;
        if (accepted && scopedTopmostEligible_)
        {
            pendingTopmostReassert_ = true;
            QTimer::singleShot(0, this, [this] { applyScopedTopmost(); });
        }
        if (result) *result = accepted ? 1 : 0;
        return true;
    }
    if (message->message != activationMessage)
        return QMainWindow::nativeEvent(eventType, nativeMessage, result);

    const DWORD requestedProcessId = static_cast<DWORD>(message->wParam);
    const HWND requestedOwner = reinterpret_cast<HWND>(message->lParam);
    if (requestedOwner && IsWindow(requestedOwner) && requestedProcessId != 0)
    {
        DWORD actualProcessId = 0;
        GetWindowThreadProcessId(requestedOwner, &actualProcessId);
        if (actualProcessId == requestedProcessId)
        {
            clearNativeOwner();
            ownerHandle_ = reinterpret_cast<quintptr>(requestedOwner);
            ownerProcessId_ = requestedProcessId;
            if (presentationTargetProcessId_ != requestedProcessId)
            {
                presentationTargetHandle_ = 0;
                presentationTargetProcessId_ = 0;
            }
            ownerApplied_ = false;
            publishedWindowHandle_ = 0;
            if (isVisible()) applyNativeOwner();
        }
    }

    // Acknowledge the cross-process SendMessage immediately. Revealing can
    // synchronously enter showEvent; doing it inside this handler used to send
    // Association back to the VP UI thread while that thread was blocked here.
    // Queue all UI work until this native call stack has unwound.
    if (result) *result = 1;
    QTimer::singleShot(0, this, [this]
    {
        // A popup from the hidden/previous editor state must not survive as an
        // independently floating native window when VP reactivates Config.
        if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        reveal();
    });
    return true;
}

bool ConfigEditorWindow::event(QEvent* event)
{
    bool returnAfterMinimize = false;
    if (event->type() == QEvent::WindowStateChange)
    {
        const auto* stateChange = static_cast<QWindowStateChangeEvent*>(event);
        const HWND editor = reinterpret_cast<HWND>(effectiveWinId());
        returnAfterMinimize = !(stateChange->oldState() & Qt::WindowMinimized) &&
            editor && GetForegroundWindow() == editor &&
            savedForegroundOnlyEnabled();
        if (returnAfterMinimize && ownerProcessId_ != 0)
            AllowSetForegroundWindow(ownerProcessId_);
    }
    const bool handled = QMainWindow::event(event);
    if (returnAfterMinimize && isMinimized())
        returnFocusToPresentationTarget("config-minimize");
    if (event->type() == QEvent::WinIdChange)
    {
        if (scopedTopmost_)
        {
            scopedTopmost_ = false;
            pendingTopmostReassert_ = true;
        }
        publishedWindowHandle_ = 0;
        QTimer::singleShot(0, this, [this]
        {
            if (isVisible()) applyNativeOwner();
            publishNativeAssociation();
            applyScopedTopmost();
        });
    }
    return handled;
}

void ConfigEditorWindow::setStatus(const QString& message, bool error)
{
    if (!status_) return;
    status_->setText(message);
    status_->setStyleSheet(error ? QStringLiteral("color: #ff8d86;") : QStringLiteral("color: #9fb2c4;"));
    QAccessibleEvent changed(status_, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&changed);
}

void ConfigEditorWindow::setWarningStatus(const QString& message)
{
    if (!status_) return;
    status_->setText(message);
    status_->setStyleSheet(QStringLiteral("color: #e0b45c;"));
    QAccessibleEvent changed(status_, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&changed);
}
